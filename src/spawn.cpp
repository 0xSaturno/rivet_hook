// SPDX-FileCopyrightText: 2025-2026 Neptuwunium
//
// SPDX-License-Identifier: EUPL-1.2

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cmath>
#include <cstring>

#include "spawn.hpp"

#include "ddl_visit.hpp"
#include "game/scene_manager.hpp"
#include "game_thread.hpp"
#include "runtime.hpp"
#include "scene_query.hpp"
#include "signature.hpp"
#include "travel.hpp"

using namespace rivet_hook::game;

namespace rivet_hook {
	// overlay_core owns this, it is resolved once during Overlay::Init
	extern SceneManager *g_SceneManager;
} // namespace rivet_hook

namespace rivet_hook::spawn {
	using actor_call_t = void (*)(Actor *actor);
	using actor_destroy_t = bool (*)(Actor *actor);

	static actor_call_t g_activate = nullptr;
	static actor_call_t g_deactivate = nullptr;
	static actor_destroy_t g_destroy = nullptr;

	// uids in the form placed actors have, top bit set, the only form the scene's
	// uid map is asked for. next to the rift portals' and clear of them, counted
	// up from here and never handed out twice in a session
	constexpr uint64_t SPAWN_UID = 0xEE5F7EED00010000ull;
	static uint64_t g_next_uid = 0;

	constexpr float DEGREES = 3.14159265358979f / 180.0f;
	// the engine reads the scale back out of the matrix rows and builds the
	// actor's physics from it, none at all from a scale that is not positive
	constexpr float MIN_SCALE = 0.001f;
	constexpr float MAX_SCALE = 1000.0f;
	// a level and its physics world keep within this of the origin on every
	// axis. a mistyped coordinate is turned down here, before it is a matrix
	constexpr float MAX_COORDINATE = 4096.0f;
	constexpr const char *ACTOR_EXTENSION = ".actor";
	constexpr uint64_t ASSET_TIMEOUT_MS = 30000;
	// actor updates stop for a load, and the game thread's pump with them. it
	// going quiet for longer than this is taken for one
	constexpr uint64_t PUMP_GAP_MS = 1000;

	struct Entry {
		bool used = false;
		Info info {};
		// while loading: the asset waited for, and since when
		Asset *asset = nullptr;
		uint64_t since = 0;
		// the asset's id, to tell should the pointer come to mean another asset
		uint64_t asset_id = 0;
		// the hero there was when it was asked for. another one is another level
		uint32_t hero = 0;
	};

	static Entry g_entries[MAX_SPAWNED];
	static int32_t g_loading = 0;
	static char g_last_error[0x180] {};
	// when the pump last ran on the game thread
	static uint64_t g_last_game_pump = 0;

	// every load of an actor asset takes a reference and nothing here gives one
	// back, so a path is asked for once and its asset kept, as the rift keeps
	// its portals', a failed one too. past this many paths the oldest is
	// forgotten, not let go
	struct HeldAsset {
		char path[0x100] {};
		Asset *asset = nullptr;
		uint64_t id = 0;
	};

	constexpr int32_t MAX_HELD_ASSETS = 32;
	static HeldAsset g_assets[MAX_HELD_ASSETS];
	static int32_t g_next_asset = 0;

	auto
	init() -> void {
		if (g_activate != nullptr) {
			return;
		}

		const auto unload = find_address(LEVEL_UNLOAD_ACTOR_DESTROY_SIGNATURE);
		g_activate = reinterpret_cast<actor_call_t>(find_address(ACTOR_ACTIVATE_SIGNATURE));
		g_deactivate = reinterpret_cast<actor_call_t>(load_rel_var(unload, LEVEL_UNLOAD_ACTOR_DEACTIVATE_ADDRESS));
		g_destroy = reinterpret_cast<actor_destroy_t>(load_rel_var(unload, LEVEL_UNLOAD_ACTOR_DESTROY_ADDRESS));

		// Destroy is sub rsp, 28; mov rdx, rcx; lea rcx, [scene]; call. anything
		// else means the signature landed somewhere it should not have
		if (g_destroy != nullptr) {
			constexpr uint8_t DESTROY_START[] = { 0x48, 0x83, 0xEC, 0x28, 0x48, 0x8B, 0xD1, 0x48, 0x8D, 0x0D };
			const auto *code = reinterpret_cast<const uint8_t *>(g_destroy);
			const auto *scene = load_rel_var(find_address(SCENE_MANAGER_SIGNATURE), SCENE_MANAGER_ADDRESS);
			if (scene == nullptr || !ddl::is_readable_uncached(code, ACTOR_DESTROY_SCENE_ADDRESS + REL_ADDRESS_SIZE) || memcmp(code, DESTROY_START, sizeof(DESTROY_START)) != 0
				|| load_rel_var(reinterpret_cast<intptr_t>(code), ACTOR_DESTROY_SCENE_ADDRESS) != scene) {
				g_destroy = nullptr;
			}
		}

		if (g_activate == nullptr || g_deactivate == nullptr || g_destroy == nullptr) {
			g_activate = nullptr;
			g_output << "[spawn] the actor activate and destroy calls were not found, rivet.spawn is unavailable\n";
		} else if (!travel::actor_spawn_ready()) {
			g_output << "[spawn] the actor spawn was not found, rivet.spawn is unavailable\n";
		} else {
			g_output << "[spawn] Actor::Activate at " << reinterpret_cast<void *>(g_activate) << ", Actor::Destroy at " << reinterpret_cast<void *>(g_destroy) << "\n";
		}

		g_output.flush();
	}

	auto
	unavailable_reason() -> const char * {
		if (!travel::actor_spawn_ready()) {
			return "the actor spawn was not found";
		}

		return g_activate != nullptr ? "" : "the actor activate and destroy calls were not found";
	}

	static auto
	fail(const char **reason, const char *why) -> bool {
		if (reason != nullptr) {
			*reason = why;
		}

		return false;
	}

	// the engine calls on their own, so a fault is caught with nothing to unwind
	static auto
	call_activate(Actor *actor) -> bool {
#ifdef _MSC_VER
		__try {
#endif
			g_activate(actor);
			return true;
#ifdef _MSC_VER
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			return false;
		}
#endif
	}

	// deactivated first, the order the scene takes a level's actors down in: the
	// actor is out of the world now, and the scene frees it at the start of the
	// next actor update
	static auto
	call_destroy(Actor *actor) -> bool {
#ifdef _MSC_VER
		__try {
#endif
			if ((actor->flags & ActorFlag::Activated) != 0) {
				g_deactivate(actor);
			}

			g_destroy(actor);
			return true;
#ifdef _MSC_VER
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			return false;
		}
#endif
	}

	// the actor an entry spawned, or null once it is gone. a slot's generation
	// goes up when it is freed and wraps from 0x7FF to 1, so after 2047 frees of
	// one slot a handle comes round again. the handle resolving is not enough:
	// the slot has to be in use and still carry the uid the actor was given
	static auto
	actor_of(const Info &info) -> Actor * {
		if (info.handle == 0 || g_SceneManager == nullptr || g_SceneManager->actors == nullptr) {
			return nullptr;
		}

		auto *actor = g_SceneManager->ResolveActor(EngineHandle { .value = info.handle });
		if (actor == nullptr || !ddl::is_readable(actor, sizeof(Actor)) || (actor->flags & ActorFlag::Allocated) == 0 || scene_query::uid_of(actor) != info.uid) {
			return nullptr;
		}

		return actor;
	}

	// brings an entry up to what its actor is now. false when the actor is gone
	static auto
	refresh(Entry &entry) -> bool {
		auto &info = entry.info;
		const auto *actor = actor_of(info);
		if (actor == nullptr) {
			return false;
		}

		info.flags = actor->flags;
		// a destroy that was asked for stays asked for
		if (info.state != State::Destroying) {
			info.state = (info.flags & ActorFlag::Destroying) != 0 ? State::Destroying : (info.flags & ActorFlag::Activated) != 0 ? State::Active : State::Inactive;
		}

		if (actor->object != nullptr && ddl::is_readable(actor->object, sizeof(SceneObject))) {
			for (auto axis = 0; axis < 3; ++axis) {
				info.position[axis] = actor->object->transform_matrix[3][axis];
			}

			info.scale[0] = actor->object->scale.x;
			info.scale[1] = actor->object->scale.y;
			info.scale[2] = actor->object->scale.z;
		}

		return true;
	}

	// refreshes every spawned actor and lets go of the ones the engine has
	// destroyed, asked to here or along with the level. returns how many are left
	static auto
	prune() -> int32_t {
		int32_t left = 0;
		for (auto &entry : g_entries) {
			if (!entry.used) {
				continue;
			}

			if (entry.info.state != State::Loading && !refresh(entry)) {
				entry = {};
				continue;
			}

			++left;
		}

		return left;
	}

	// the actor asset at path, asked for unless it is held already. null, with
	// the reason, when there is none to be had
	static auto
	asset_for(const char *path, const char **reason) -> Asset * {
		HeldAsset *slot = nullptr;
		for (auto &held : g_assets) {
			if (held.asset != nullptr && _stricmp(held.path, path) == 0) {
				slot = &held;
				break;
			}
		}

		// a held pointer is a slot of the manager's pool. it is good for as long
		// as the slot holds an asset, and the asset the path was asked for
		auto *asset = slot != nullptr ? slot->asset : nullptr;
		if (asset == nullptr || !ddl::is_readable(asset, sizeof(Asset)) || asset->assetId != slot->id || asset->status < AssetStatus::InQueue || asset->status > AssetStatus::Aborted) {
			asset = nullptr;
			if (!travel::load_actor_asset(path, &asset) || asset == nullptr || !ddl::is_readable(asset, sizeof(Asset))) {
				fail(reason, "the actor asset could not be requested");
				return nullptr;
			}

			// the manager's own empty asset, handed out when it has no slot left
			// for one more or turns the load down. never queued, and not kept
			if (asset->status < AssetStatus::InQueue || asset->status > AssetStatus::Aborted) {
				fail(reason, "the actor asset manager did not take the load");
				return nullptr;
			}

			if (slot == nullptr) {
				slot = &g_assets[g_next_asset];
				g_next_asset = (g_next_asset + 1) % MAX_HELD_ASSETS;
			}

			strncpy_s(slot->path, path, _TRUNCATE);
			slot->asset = asset;
			slot->id = asset->assetId;
		}

		// a path with nothing behind it is marked failed before the load call is
		// back. asking again would only add a reference: the manager queues
		// nothing for an asset somebody holds
		if (asset->status == AssetStatus::Error || asset->status == AssetStatus::Aborted) {
			fail(reason, "there is no actor asset at that path, or it failed to load");
			return nullptr;
		}

		return asset;
	}

	// the next uid no loaded actor has. setting a uid another actor holds would
	// take that actor out of the scene's uid map
	static auto
	next_uid() -> uint64_t {
		auto uid = SPAWN_UID + g_next_uid++;
		for (auto tries = 0; tries < MAX_SPAWNED && scene_query::actor_by_uid(uid) != 0; ++tries) {
			uid = SPAWN_UID + g_next_uid++;
		}

		return uid;
	}

	// spawns an entry's actor from its loaded asset and activates it, the two
	// steps the game's own spawns take back to back
	static auto
	finish(Entry &entry, const char **reason) -> bool {
		auto &info = entry.info;
		const auto yaw = info.yaw * DEGREES;
		const auto sine = std::sin(yaw);
		const auto cosine = std::cos(yaw);
		// rows right, up, forward and position, each axis row as long as its
		// scale. yaw as the camera has it: 0 faces +z and turns toward +x
		const float matrix[4][4] = {
			{ cosine * info.scale[0], 0.0f, -sine * info.scale[0], 0.0f },
			{ 0.0f, info.scale[1], 0.0f, 0.0f },
			{ sine * info.scale[2], 0.0f, cosine * info.scale[2], 0.0f },
			{ info.position[0], info.position[1], info.position[2], 1.0f },
		};

		// the engine hatches the actor from whatever the asset holds and never
		// looks at its status. both callers have seen it Loaded
		Actor *actor = nullptr;
		const auto spawned = travel::spawn_actor(entry.asset_id, matrix, info.uid, &actor);
		const auto handle = actor != nullptr ? scene_query::handle_of(actor) : 0;
		if (!spawned) {
			// with an actor to show for it the fault came after the engine made
			// one, in the uid call, and the actor goes again. without, there is
			// no telling what the spawn left behind
			if (handle != 0 && call_destroy(actor)) {
				return fail(reason, "the actor spawn faulted, the actor was destroyed again");
			}

			return fail(reason, "the actor spawn faulted, and may have left an actor in the scene");
		}

		if (actor == nullptr) {
			return fail(reason, "the actor could not be spawned: the actor asset manager does not know its asset, or the scene has no actor slot left");
		}

		// Destroy goes by the slot index the actor holds and checks nothing, so
		// a pointer that is not one of the scene's actors is not handed to it
		if (handle == 0) {
			return fail(reason, "the spawned actor is not in the scene, it was left alone");
		}

		info.handle = handle;

		// without its uid it could not be told from whatever takes its slot
		// later, so it goes again now, while the pointer is known good
		if (scene_query::uid_of(actor) != info.uid) {
			call_destroy(actor);
			info.handle = 0;
			return fail(reason, "the spawned actor did not take its uid");
		}

		if (!call_activate(actor)) {
			// a fault part way leaves the actor marked as in the middle of it,
			// and neither Deactivate nor the scene's teardown deactivates an
			// actor with that mark. it comes off, so the components that did
			// activate are deactivated on the way out
			actor->flags &= ~ActorFlag::IsActivatingOrDeactivating;
			const auto destroyed = call_destroy(actor);
			info.handle = 0;
			return fail(reason, destroyed ? "the actor's activate faulted, the actor was destroyed again" : "the actor's activate faulted and so did its destroy, it is left in the scene");
		}

		// a spawned actor belongs to no zone and nothing unloads it. with this
		// flag the scene takes it down with the level; the engine sets it the
		// same way, after the activate, on actors it creates itself
		actor->flags |= ActorFlag::DestroyOnUnloadLevel;

		entry.asset = nullptr;
		entry.since = 0;
		info.state = State::Inactive;
		refresh(entry);
		g_output << "[spawn] " << info.path << " at " << info.position[0] << " " << info.position[1] << " " << info.position[2] << ", actor " << info.handle << ", " << state_name(info.state) << "\n";
		g_output.flush();
		return true;
	}

	auto
	request(const char *path, const float *position, const float yaw, const float *scale, Info &out, const char **reason) -> Result {
		out = {};
		if (const auto *why = unavailable_reason(); why[0] != '\0') {
			fail(reason, why);
			return Result::Failed;
		}

		// a spawn creates the actor, its scene object and its components
		if (!game_thread::on_game_thread()) {
			fail(reason, "actors can only spawn on the game thread, and it is not pumping (loading?)");
			return Result::Failed;
		}

		if (g_SceneManager == nullptr) {
			fail(reason, "the scene manager is not available");
			return Result::Failed;
		}

		const auto hero = scene_query::hero();
		if (hero == 0) {
			fail(reason, "there is no hero right now, actors only spawn while a level is in gameplay");
			return Result::Failed;
		}

		const auto length = path != nullptr ? strlen(path) : 0;
		const auto extension = strlen(ACTOR_EXTENSION);
		if (length <= extension || _stricmp(path + length - extension, ACTOR_EXTENSION) != 0) {
			fail(reason, "that is not an .actor path");
			return Result::Failed;
		}

		if (length >= sizeof(out.path)) {
			fail(reason, "the asset path is too long");
			return Result::Failed;
		}

		for (auto axis = 0; axis < 3; ++axis) {
			if (!std::isfinite(position[axis])) {
				fail(reason, "coordinates must be finite");
				return Result::Failed;
			}

			if (std::fabs(position[axis]) > MAX_COORDINATE) {
				fail(reason, "coordinates have to be within 4096 of the origin on every axis, a level reaches no further");
				return Result::Failed;
			}

			// written so that a nan fails it too
			if (!(scale[axis] >= MIN_SCALE && scale[axis] <= MAX_SCALE)) {
				fail(reason, "the scale has to be between 0.001 and 1000 on every axis");
				return Result::Failed;
			}
		}

		if (!std::isfinite(yaw)) {
			fail(reason, "the yaw must be finite");
			return Result::Failed;
		}

		Entry *entry = nullptr;
		if (prune() < MAX_SPAWNED) {
			for (auto &candidate : g_entries) {
				if (!candidate.used) {
					entry = &candidate;
					break;
				}
			}
		}

		if (entry == nullptr) {
			fail(reason, "too many actors are spawned already, destroy some first");
			return Result::Failed;
		}

		auto *asset = asset_for(path, reason);
		if (asset == nullptr) {
			return Result::Failed;
		}

		*entry = {};
		entry->used = true;
		entry->asset = asset;
		entry->asset_id = asset->assetId;
		entry->hero = hero;
		entry->since = GetTickCount64();
		auto &info = entry->info;
		info.uid = next_uid();
		info.state = State::Loading;
		info.yaw = yaw;
		memcpy(info.position, position, sizeof(info.position));
		memcpy(info.scale, scale, sizeof(info.scale));
		strncpy_s(info.path, path, _TRUNCATE);

		// the pump spawns it once the asset is in. the uid is settled already, so
		// the caller can look the actor up by it later
		if (asset->status != AssetStatus::Loaded) {
			++g_loading;
			out = info;
			return Result::Loading;
		}

		// a refusal here is a null spawn or a fault in the engine, and worth a
		// line in the log besides the answer
		const char *why = "refused";
		if (!finish(*entry, &why)) {
			g_output << "[spawn] " << path << ": " << why << "\n";
			g_output.flush();
			*entry = {};
			fail(reason, why);
			return Result::Failed;
		}

		out = info;
		return Result::Spawned;
	}

	// Destroy takes whatever actor it is handed and looks at nothing but its slot,
	// so the entry's actor is resolved again right before, and left alone after.
	// asked is whether this call was the one to ask: an actor that is gone or
	// going already is no failure, and no request either
	static auto
	destroy_entry(Entry &entry, bool &asked, const char **reason) -> bool {
		asked = false;
		auto &info = entry.info;
		auto *actor = actor_of(info);
		if (actor == nullptr) {
			// the engine destroyed it already
			entry = {};
			return true;
		}

		if (info.state == State::Destroying || (actor->flags & ActorFlag::Destroying) != 0) {
			info.state = State::Destroying;
			return true;
		}

		if (!call_destroy(actor)) {
			return fail(reason, "the destroy request faulted");
		}

		asked = true;
		info.state = State::Destroying;
		g_output << "[spawn] destroying " << info.path << ", actor " << info.handle << "\n";
		g_output.flush();
		return true;
	}

	auto
	destroy(const uint32_t handle, const char **reason) -> bool {
		if (const auto *why = unavailable_reason(); why[0] != '\0') {
			return fail(reason, why);
		}

		// only a handle from the list goes any further: the hero, a level actor
		// or anything else the engine spawned never reaches Destroy
		Entry *entry = nullptr;
		for (auto &candidate : g_entries) {
			if (candidate.used && handle != 0 && candidate.info.handle == handle) {
				entry = &candidate;
				break;
			}
		}

		if (entry == nullptr) {
			return fail(reason, "that handle is not an actor this hook spawned");
		}

		if (!game_thread::on_game_thread()) {
			return fail(reason, "actors can only be destroyed on the game thread, and it is not pumping (loading?)");
		}

		auto asked = false;
		return destroy_entry(*entry, asked, reason);
	}

	auto
	destroy_all(int32_t &destroyed, const char **reason) -> bool {
		destroyed = 0;
		if (const auto *why = unavailable_reason(); why[0] != '\0') {
			return fail(reason, why);
		}

		if (!game_thread::on_game_thread()) {
			return fail(reason, "actors can only be destroyed on the game thread, and it is not pumping (loading?)");
		}

		for (auto &entry : g_entries) {
			if (!entry.used) {
				continue;
			}

			auto asked = false;
			if (entry.info.state == State::Loading) {
				entry = {};
				--g_loading;
				++destroyed;
			} else if (destroy_entry(entry, asked, nullptr) && asked) {
				++destroyed;
			}
		}

		return true;
	}

	auto
	list(Info *out, const int32_t max) -> int32_t {
		prune();

		int32_t count = 0;
		for (const auto &entry : g_entries) {
			if (entry.used && count < max) {
				out[count++] = entry.info;
			}
		}

		return count;
	}

	auto
	state_name(const State state) -> const char * {
		switch (state) {
		case State::Loading:
			return "loading";
		case State::Active:
			return "active";
		case State::Inactive:
			return "inactive";
		case State::Destroying:
			return "destroying";
		}

		return "?";
	}

	auto
	last_error() -> const char * {
		return g_last_error;
	}

	// gives up on a spawn that was waiting for its asset
	static auto
	drop(Entry &entry, const char *why) -> void {
		_snprintf_s(g_last_error, sizeof(g_last_error), _TRUNCATE, "%s: %s", entry.info.path, why);
		g_output << "[spawn] " << g_last_error << "\n";
		g_output.flush();
		entry = {};
		--g_loading;
	}

	auto
	pump() -> void {
		const auto now = GetTickCount64();
		const auto on_game_thread = game_thread::on_game_thread();
		// a spawn that was waiting when actor updates stopped was meant for the
		// level that went with them, not for the one that came after
		const auto before = g_last_game_pump;
		const auto resumed = on_game_thread && before != 0 && now - before > PUMP_GAP_MS;
		if (on_game_thread) {
			g_last_game_pump = now;
		}

		if (g_loading == 0) {
			return;
		}

		if (on_game_thread) {
			// a cached readable region must never outlive the frame it was taken in
			ddl::reset_readable_cache();
		}

		for (auto &entry : g_entries) {
			if (!entry.used || entry.info.state != State::Loading) {
				continue;
			}

			// first, and on the render thread as well, where nothing of the
			// engine's is touched: however its asset is doing by now, a spawn
			// this old is not spawned any more
			if (now - entry.since > ASSET_TIMEOUT_MS) {
				drop(entry, "30 seconds went by without its asset loaded and the level in gameplay");
				continue;
			}

			if (!on_game_thread) {
				continue;
			}

			// asked for in this very pump it is from after the gap, and stays
			if (resumed && entry.since <= before) {
				drop(entry, "actor updates stopped, as they do for a load, before its asset was in");
				continue;
			}

			if (!ddl::is_readable(entry.asset, sizeof(Asset)) || entry.asset->assetId != entry.asset_id) {
				drop(entry, "the actor asset went away");
				continue;
			}

			const auto status = entry.asset->status;
			if (status < AssetStatus::InQueue || status > AssetStatus::Loaded) {
				drop(entry, "the actor asset failed to load");
				continue;
			}

			const auto hero = g_SceneManager != nullptr ? scene_query::hero() : 0;
			if (status != AssetStatus::Loaded || hero == 0) {
				continue;
			}

			if (hero != entry.hero) {
				drop(entry, "the hero is another actor than when this was asked for (a level change?)");
				continue;
			}

			// there is no caller left to hand a refusal to
			const char *reason = "refused";
			if (finish(entry, &reason)) {
				--g_loading;
				g_last_error[0] = '\0';
			} else {
				drop(entry, reason);
			}
		}
	}
} // namespace rivet_hook::spawn

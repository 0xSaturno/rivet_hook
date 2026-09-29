// SPDX-FileCopyrightText: 2025-2026 Neptuwunium
//
// SPDX-License-Identifier: EUPL-1.2

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

#include "travel.hpp"

#include "ddl_visit.hpp"
#include "game/load_system.hpp"
#include "game/scene_manager.hpp"
#include "game_thread.hpp"
#include "runtime.hpp"
#include "scene_query.hpp"
#include "script_signal.hpp"
#include "signature.hpp"

using namespace rivet_hook::game;

namespace rivet_hook {
	// overlay_core owns this, it is resolved once during Overlay::Init
	extern SceneManager *g_SceneManager;
} // namespace rivet_hook

namespace rivet_hook::travel {
	// "leave it as it is" for the hero type and the lighting mode. the hero
	// field idles at 4 (none) between warps, and passing that asks for a swap
	constexpr int32_t KEEP_HERO = -1;
	constexpr int32_t KEEP_LIGHTING = -1;
	constexpr uint32_t NO_DIMENSION = 0;

	using request_warp_t = void (*)(void *manager, uint32_t checkpoint, uint32_t dimension, int32_t hero, int32_t lighting);
	using overlay_t = bool (*)(void *load_system, const uint64_t *region);

	// where the load system instance pointer lives. it is set once the game has
	// a load system, so it is read at use
	static void *const *g_instance = nullptr;
	static uint32_t g_checkpoints_offset = 0;
	static request_warp_t g_request_warp = nullptr;
	static overlay_t g_overlay_load = nullptr;
	static overlay_t g_overlay_unload = nullptr;
	// optional, for telling loaded and story driven overlays apart
	static uint32_t g_overlay_manager_offset = 0;
	static const CustomOverlaySystem *g_custom_overlays = nullptr;
	// where the planet menu leaves the tunnel and destination checkpoint names
	static char *g_tunnel_name = nullptr;
	static char *g_destination_name = nullptr;
	constexpr const char *TUNNEL_PREFIX = "CHK_TRANSITION_TO_";

	// ScriptPlugs::SetVarString / SendSignal, as the planet menu listener calls them
	using set_var_string_t = void (*)(void *plugs, const char *value, uint32_t var);
	using send_signal_t = bool (*)(void *plugs, uint32_t output);
	static set_var_string_t g_set_var_string = nullptr;
	static send_signal_t g_send_signal = nullptr;

	// the ship's listener: its vars for the two names, its accept output, and the
	// var holding the ship's menu screen actor, for picking the nearest ship
	constexpr const char *SHIP_LISTENER = "OnPlanetMenuRTTEventAction";
	constexpr uint32_t SHIP_VAR_TUNNEL = 0x136b3eab;	 // INTERPLANETARY_SHIP_SELECTED
	constexpr uint32_t SHIP_VAR_DESTINATION = 0x41f2d10d; // DESTINATION_SHIP_SELECTED
	constexpr uint32_t SHIP_VAR_SCREEN = 0x7245fd51;
	constexpr uint32_t SHIP_OUT_ACCEPT = 0x482fae17;

	// the passive shift controller's parameters: four Portal component handles,
	// the checkpoint the airlock loads, and whether to restore the old one after.
	// the controller copies the struct whole, so it is padded with zeros past
	// what is known
	struct ShiftParams {
		uint32_t startSource = 0;
		uint32_t startDest = 0;
		uint32_t endSource = 0;
		uint32_t endDest = 0;
		uint32_t checkpoint = 0;
		uint8_t restoreCheckpoint = 0;
		uint8_t padding[0x2b] {};
	};

	// SimpleArray<ActorHandle>
	struct ActorArray {
		const uint32_t *data;
		int32_t count;
		int32_t capacity;
	};

	using shift_set_params_t = bool (*)(void *controller, const ShiftParams *params, const ActorArray *actors);
	using shift_start_t = bool (*)(void *controller, const uint32_t *triggering_actor);
	using shift_stop_t = void (*)(void *controller, const uint32_t *triggering_actor);
	static const uint32_t *g_shift_controller = nullptr; // a component handle
	static shift_set_params_t g_shift_set_params = nullptr;
	static shift_start_t g_shift_start = nullptr;
	static shift_stop_t g_shift_stop = nullptr;

	// the rift's own portals: spawned from the game's passive shift portal actor,
	// loaded on demand, so a rift needs no portals of the level's
	using spawn_actor_t = Actor *(*)(uint64_t asset, void *owner, const float (*matrix)[4]);
	using load_actor_asset_t = Asset *(*)(void *manager, const char *path, Asset *loaded_from, const char *load_info);
	static spawn_actor_t g_spawn_actor = nullptr;
	// Scene::SetActorUid (actor, uid). the hero's rift state names its portal by
	// uid, which only placed actors have, so spawned portals are given one. the
	// spawn calls it at +0xC1 when it is handed an owner
	using set_actor_uid_t = void (*)(Actor *actor, uint64_t uid);
	static set_actor_uid_t g_set_actor_uid = nullptr;
	constexpr uint32_t SPAWN_SET_UID_CALL = 0xC1;
	// a uid in the form placed actors have, top bit set, which is looked up in the
	// scene's uid map. the spawned form (bit 48) is looked up as a network id
	// instead and would find nothing
	constexpr uint64_t RIFT_PORTAL_UID = 0xEE5F7EED00007F00ull;
	static load_actor_asset_t g_load_actor_asset = nullptr;
	static void *g_actor_assets = nullptr;
	// two assets: the actors of one asset share their prius, and the two portals
	// that follow the hero need other switches than the two that stay put
	constexpr const char *RIFT_PORTAL_ASSETS[2] = {
		"environment/global/test/test_gbl_portal/test_gbl_portal_passive_shift.actor", // A and C
		"environment/global/test/test_gbl_portal/test_gbl_portal.actor",				 // B and D
	};
	static Asset *g_rift_assets[2] {};
	static uint32_t g_rift_portals[4] {};
	// how long the game's own rift portals take to open
	constexpr float RIFT_PORTAL_OPEN_TIME = 0.5f;

	// a rift waiting for its portal asset to load
	struct PendingRift {
		bool waiting = false;
		char checkpoint[0x80] {};
		bool at_position = false;
		float position[3] {};
		uint64_t since = 0;
	};

	static PendingRift g_pending;
	static bool g_rift_asset_failed = false; // gave up on it for this session
	constexpr uint64_t RIFT_ASSET_TIMEOUT_MS = 30000;

	// a portal lent to a rift: where it was, and the PortalPassiveShift switches
	// changed on it (-1 when untouched)
	struct BorrowedPortal {
		uint32_t actor = 0;
		float matrix[4][4] {};
		int8_t followPlayer = -1;
		int8_t gravityWell = -1;
	};

	static BorrowedPortal g_borrowed[4];
	static int32_t g_borrowed_count = 0;
	static bool g_rift_busy_seen = false;
	static uint64_t g_rift_started = 0;
	static uint8_t g_rift_state = 0;
	// where the game's own passive shifts put the hero while the far end loads,
	// gliding along +z: far off in the sky, so nothing is in the way
	constexpr float AIRLOCK_POSITION[3] = { -2908.0f, 3052.0f, 2060.0f };
	// how high over the target the far end opens
	constexpr float RIFT_EXIT_HEIGHT = 1.5f;
	constexpr uint64_t RIFT_TIMEOUT_MS = 60000;
	constexpr const char *SHIFT_STATE_NAMES[] = { "idle", "start transition", "loading", "end transition" };
	static const char *g_unavailable = "travel was not initialized";

	auto
	init() -> void {
		if (g_request_warp != nullptr) {
			return;
		}

		const auto site = find_address(LOAD_SYSTEM_CHECKPOINTS_SIGNATURE);
		const auto get_instance = reinterpret_cast<const uint8_t *>(load_rel_var(site, LOAD_SYSTEM_GET_INSTANCE_ADDRESS));

		// the getter is mov rax, [instance]; ret. anything else means the
		// signature landed somewhere it should not have
		if (get_instance != nullptr && get_instance[0] == 0x48 && get_instance[1] == 0x8B && get_instance[2] == 0x05 && get_instance[7] == 0xC3) {
			g_instance = static_cast<void *const *>(load_rel_var(reinterpret_cast<intptr_t>(get_instance), LOAD_SYSTEM_INSTANCE_ADDRESS));
			g_checkpoints_offset = *reinterpret_cast<const uint32_t *>(site + LOAD_SYSTEM_CHECKPOINTS_OFFSET);
		}

		g_request_warp = reinterpret_cast<request_warp_t>(find_address(HERO_REQUEST_CHECKPOINT_WARP_SIGNATURE));

		// overlays are optional: without them travel still warps
		g_overlay_load = reinterpret_cast<overlay_t>(load_rel_var(find_address(LOAD_SYSTEM_OVERLAY_LOAD_SIGNATURE), LOAD_SYSTEM_OVERLAY_LOAD_ADDRESS));
		g_overlay_unload = reinterpret_cast<overlay_t>(load_rel_var(find_address(LOAD_SYSTEM_OVERLAY_UNLOAD_SIGNATURE), LOAD_SYSTEM_OVERLAY_UNLOAD_ADDRESS));
		if (g_overlay_load == nullptr || g_overlay_unload == nullptr) {
			g_overlay_load = nullptr;
			g_overlay_unload = nullptr;
			g_output << "[travel] the overlay requests were not found, rivet.overlay is unavailable\n";
		} else {
			// RequestOverlayLoad is a guard then add rcx, <manager offset>
			const auto *code = reinterpret_cast<const uint8_t *>(g_overlay_load);
			if (code[OVERLAY_LOAD_MANAGER_ADD] == 0x48 && code[OVERLAY_LOAD_MANAGER_ADD + 1] == 0x81 && code[OVERLAY_LOAD_MANAGER_ADD + 2] == 0xC1) {
				g_overlay_manager_offset = *reinterpret_cast<const uint32_t *>(code + OVERLAY_LOAD_MANAGER_OFFSET);
			}

			g_custom_overlays = static_cast<const CustomOverlaySystem *>(load_rel_var(find_address(CUSTOM_OVERLAY_SYSTEM_SIGNATURE), CUSTOM_OVERLAY_SYSTEM_ADDRESS));
		}

		// the two readers load buffers 0x100 apart, the tunnel's first
		const auto readers = find_addresses(PLANET_MENU_CHECKPOINT_READ_SIGNATURE);
		if (readers.size() == 2) {
			auto *first = static_cast<char *>(load_rel_var(readers[0], PLANET_MENU_CHECKPOINT_ADDRESS));
			auto *second = static_cast<char *>(load_rel_var(readers[1], PLANET_MENU_CHECKPOINT_ADDRESS));
			if (first != nullptr && second != nullptr && first > second) {
				std::swap(first, second);
			}

			if (first != nullptr && second == first + PLANET_MENU_CHECKPOINT_SIZE) {
				g_tunnel_name = first;
				g_destination_name = second;
			}
		}

		if (const auto handler = find_address(PLANET_MENU_ACCEPT_HANDLER_SIGNATURE, 2, 0); handler != 0) {
			g_set_var_string = reinterpret_cast<set_var_string_t>(load_rel_var(handler, PLANET_MENU_SET_VAR_STRING_ADDRESS));
			g_send_signal = reinterpret_cast<send_signal_t>(load_rel_var(handler, PLANET_MENU_SEND_SIGNAL_ADDRESS));
		}

		if (g_destination_name == nullptr || g_set_var_string == nullptr || g_send_signal == nullptr) {
			g_destination_name = nullptr;
			g_output << "[travel] the planet menu pieces were not found, rivet.fly is unavailable\n";
		}

		const auto set_site = find_address(PASSIVE_SHIFT_SET_PARAMS_SIGNATURE);
		const auto start_site = find_address(PASSIVE_SHIFT_START_SIGNATURE);
		g_shift_controller = static_cast<const uint32_t *>(load_rel_var(set_site, PASSIVE_SHIFT_CONTROLLER_ADDRESS));
		g_shift_set_params = reinterpret_cast<shift_set_params_t>(load_rel_var(set_site, PASSIVE_SHIFT_SET_PARAMS_ADDRESS));
		g_shift_start = reinterpret_cast<shift_start_t>(load_rel_var(start_site, PASSIVE_SHIFT_START_ADDRESS));
		g_shift_stop = reinterpret_cast<shift_stop_t>(load_rel_var(find_address(PASSIVE_SHIFT_STOP_SIGNATURE), PASSIVE_SHIFT_STOP_ADDRESS));
		g_spawn_actor = reinterpret_cast<spawn_actor_t>(find_address(SPAWN_ACTOR_FROM_ASSET_SIGNATURE));
		g_load_actor_asset = reinterpret_cast<load_actor_asset_t>(find_address(LOAD_ACTOR_ASSET_SIGNATURE));
		g_actor_assets = load_rel_var(find_address(ACTOR_ASSET_MANAGER_SIGNATURE), ACTOR_ASSET_MANAGER_ADDRESS);
		if (g_spawn_actor != nullptr && reinterpret_cast<const uint8_t *>(g_spawn_actor)[SPAWN_SET_UID_CALL] == 0xE8) {
			g_set_actor_uid = reinterpret_cast<set_actor_uid_t>(load_rel_var(reinterpret_cast<uintptr_t>(g_spawn_actor), SPAWN_SET_UID_CALL + 1));
		}

		if (g_spawn_actor == nullptr || g_load_actor_asset == nullptr || g_actor_assets == nullptr || g_set_actor_uid == nullptr) {
			g_spawn_actor = nullptr;
			g_output << "[travel] the actor spawn was not found, rifts borrow the level's portals\n";
		}

		if (g_shift_controller == nullptr || g_shift_set_params == nullptr || g_shift_start == nullptr || g_shift_stop == nullptr) {
			g_shift_controller = nullptr;
			g_output << "[travel] the passive shift controller was not found, rivet.rift is unavailable\n";
		}

		if (g_instance == nullptr || g_request_warp == nullptr) {
			g_request_warp = nullptr;
			g_unavailable = "the load system or the warp request was not found";
			g_output << "[travel] the load system or the warp request was not found, rivet.warp is unavailable\n";
		} else {
			g_unavailable = "";
			g_output << "[travel] checkpoints at load system +0x" << std::hex << g_checkpoints_offset << std::dec << ", warp request at " << reinterpret_cast<void *>(g_request_warp) << "\n";
		}

		g_output.flush();
	}

	auto
	unavailable_reason() -> const char * {
		return g_unavailable;
	}

	static auto
	fail(const char **reason, const char *why) -> bool {
		if (reason != nullptr) {
			*reason = why;
		}

		return false;
	}

	// the level's checkpoint table, or null while no level is loaded
	static auto
	manager(const char **reason) -> const CheckpointManager * {
		if (g_request_warp == nullptr) {
			fail(reason, g_unavailable);
			return nullptr;
		}

		auto *load_system = *g_instance;
		if (load_system == nullptr) {
			fail(reason, "the game has no load system yet");
			return nullptr;
		}

		const auto *table = reinterpret_cast<const CheckpointManager *>(static_cast<const uint8_t *>(load_system) + g_checkpoints_offset);
		if (!ddl::is_readable(table, sizeof(CheckpointManager)) || table->checkpoints == nullptr || table->count <= 0) {
			fail(reason, "no level is loaded");
			return nullptr;
		}

		if (!ddl::is_readable(table->checkpoints, sizeof(CheckpointData) * static_cast<size_t>(table->count))) {
			fail(reason, "the checkpoint table is not readable");
			return nullptr;
		}

		return table;
	}

	// the loaded level, with every table the lookups read checked readable, or
	// null while there is none
	static auto
	level(const char **reason) -> const LevelAsset * {
		if (g_request_warp == nullptr) {
			fail(reason, g_unavailable);
			return nullptr;
		}

		const auto *load_system = static_cast<const LoadSystem *>(*g_instance);
		if (load_system == nullptr || !ddl::is_readable(load_system, sizeof(LoadSystem))) {
			fail(reason, "the game has no load system yet");
			return nullptr;
		}

		const auto *asset = load_system->level;
		if (asset == nullptr || !ddl::is_readable(asset, sizeof(LevelAsset)) || asset->regionCount <= 0 || asset->zoneCount <= 0) {
			fail(reason, "no level is loaded");
			return nullptr;
		}

		const auto regions = static_cast<size_t>(asset->regionCount);
		const auto zones = static_cast<size_t>(asset->zoneCount);
		if (!ddl::is_readable(asset->regions, sizeof(LevelRegion) * regions) || !ddl::is_readable(asset->regionNames, sizeof(uint32_t) * regions)
			|| !ddl::is_readable(asset->zones, sizeof(LevelZone) * zones) || !ddl::is_readable(asset->zoneNames, sizeof(uint32_t) * zones)
			|| !ddl::is_readable(asset->zoneRefs, sizeof(int16_t) * static_cast<size_t>(asset->zoneRefCount))
			|| (asset->lodCount > 0 && !ddl::is_readable(asset->lods, sizeof(LevelRegionLod) * static_cast<size_t>(asset->lodCount)))) {
			fail(reason, "the level's tables are not readable");
			return nullptr;
		}

		return asset;
	}

	static auto
	string_of(const LevelAsset *asset, const uint32_t *offsets, const int32_t index, const int32_t count, char *out, const size_t size) -> bool {
		out[0] = '\0';
		const auto *base = reinterpret_cast<const char *>(asset->dataFile >> 16);
		return index >= 0 && index < count && base != nullptr && ddl::read_string(base + offsets[index], out, size);
	}

	static auto
	region_path(const LevelAsset *asset, const int32_t region, char *out, const size_t size) -> bool {
		return string_of(asset, asset->regionNames, asset->regions[region].nameIndex, asset->regionCount, out, size);
	}

	static auto
	zone_path(const LevelAsset *asset, const int32_t zone, char *out, const size_t size) -> bool {
		return string_of(asset, asset->zoneNames, asset->zones[zone].nameIndex, asset->zoneCount, out, size);
	}

	static auto
	type_name(const int16_t type) -> const char * {
		switch (type) {
			case RegionType::Global: return "global";
			case RegionType::Container: return "container";
			case RegionType::Unit: return "unit";
			case RegionType::OpenWorld: return "open_world";
			case RegionType::Tile: return "tile";
			case RegionType::Overlay: return "overlay";
			default: return "unknown";
		}
	}

	// the region at the top of a region's tree
	static auto
	top_of(const LevelAsset *asset, int32_t region) -> int32_t {
		for (auto depth = 0; depth < 32 && region >= 0 && region < asset->regionCount; ++depth) {
			const auto parent = asset->regions[region].parent;
			if (parent < 0 || parent >= asset->regionCount) {
				return region;
			}

			region = parent;
		}

		return region;
	}

	// what a player would call where a region is: its top region's file name,
	// "Savali" or "Savali (open world)"
	static auto
	area_of(const LevelAsset *asset, const int32_t region, char *out, const size_t size) -> void {
		out[0] = '\0';
		const auto top = top_of(asset, region);
		if (top < 0 || top >= asset->regionCount) {
			return;
		}

		char path[0x100];
		if (!region_path(asset, top, path, sizeof(path))) {
			return;
		}

		const auto *leaf = strrchr(path, '/');
		leaf = leaf != nullptr ? leaf + 1 : path;
		auto length = strlen(leaf);
		if (length > 7 && _stricmp(leaf + length - 7, ".region") == 0) {
			length -= 7;
		}

		_snprintf_s(out, size, _TRUNCATE, "%.*s%s", static_cast<int>(length), leaf, asset->regions[top].type == RegionType::OpenWorld ? " (open world)" : "");
	}

	static auto
	find_data(const uint32_t hash) -> const CheckpointData * {
		const auto *table = manager(nullptr);
		if (table == nullptr || hash == 0) {
			return nullptr;
		}

		for (int32_t i = 0; i < table->count; ++i) {
			if (table->checkpoints[i].nameHash == hash) {
				return &table->checkpoints[i];
			}
		}

		return nullptr;
	}

	auto
	checkpoints(const char *filter, const size_t limit) -> nlohmann::json {
		nlohmann::json out;
		nlohmann::json::array_t found;
		auto truncated = false;

		const char *reason = nullptr;
		const auto *table = manager(&reason);
		const auto *asset = table != nullptr ? level(&reason) : nullptr;
		out["reason"] = reason != nullptr ? reason : "";
		if (table != nullptr && asset != nullptr) {
			for (int32_t i = 0; i < table->count; ++i) {
				const auto &checkpoint = table->checkpoints[i];
				char name[0x80];
				if (!ddl::read_string(checkpoint.name, name, sizeof(name))) {
					continue;
				}

				if (filter != nullptr && filter[0] != '\0' && strstr(name, filter) == nullptr) {
					continue;
				}

				if (found.size() >= limit) {
					truncated = true;
					break;
				}

				char hash[16];
				_snprintf_s(hash, sizeof(hash), _TRUNCATE, "0x%08x", checkpoint.nameHash);

				char area[0x80];
				area_of(asset, checkpoint.region, area, sizeof(area));

				nlohmann::json entry;
				entry["name"] = name;
				entry["hash"] = hash;
				entry["region"] = checkpoint.region;
				entry["area"] = area;
				entry["type"] = checkpoint.type;
				entry["position"] = nlohmann::json::array({ checkpoint.position[0], checkpoint.position[1], checkpoint.position[2] });
				found.emplace_back(std::move(entry));
			}
		}

		out["checkpoints"] = found;
		out["truncated"] = truncated;
		return out;
	}

	auto
	find(const char *name) -> uint32_t {
		if (name == nullptr || name[0] == '\0') {
			return 0;
		}

		if (name[0] == '0' && (name[1] == 'x' || name[1] == 'X')) {
			char *end = nullptr;
			const auto hash = static_cast<uint32_t>(strtoul(name, &end, 16));
			return end != name + 2 && *end == '\0' && find_data(hash) != nullptr ? hash : 0;
		}

		const auto *table = manager(nullptr);
		if (table == nullptr) {
			return 0;
		}

		// by name rather than by hash, so the case does not have to match
		for (int32_t i = 0; i < table->count; ++i) {
			char text[0x80];
			if (ddl::read_string(table->checkpoints[i].name, text, sizeof(text)) && _stricmp(text, name) == 0) {
				return table->checkpoints[i].nameHash;
			}
		}

		return 0;
	}

	// the hero's HeroTransitionManager, or null
	static auto
	transition_manager(const char **reason) -> uint8_t * {
		if (g_SceneManager == nullptr) {
			fail(reason, "the scene manager is not available");
			return nullptr;
		}

		const auto handle = scene_query::hero();
		if (handle == 0) {
			fail(reason, "there is no hero right now");
			return nullptr;
		}

		const auto *actor = g_SceneManager->ResolveActor(EngineHandle { .value = handle });
		if (actor == nullptr || actor->components == nullptr || actor->componentCount <= 0 || !ddl::is_readable(actor->components, sizeof(ComponentPointer) * actor->componentCount)) {
			fail(reason, "the hero has no readable component list");
			return nullptr;
		}

		for (auto index = 0; index < actor->componentCount; ++index) {
			const auto [type, instance] = actor->components[index];
			if (type == nullptr || instance == nullptr || !ddl::is_readable(type, sizeof(ComponentInfo))) {
				continue;
			}

			char name[0x40];
			if (!ddl::read_string(type->name, name, sizeof(name)) || strcmp(name, "HeroTransitionManager") != 0) {
				continue;
			}

			if (!ddl::is_writable(instance, HERO_CHECKPOINT_WARP_PENDING + 1) || instance->IsDestroyed()) {
				continue;
			}

			return reinterpret_cast<uint8_t *>(instance);
		}

		fail(reason, "the hero has no HeroTransitionManager");
		return nullptr;
	}

	// the engine call on its own, so a fault is caught with nothing to unwind
	static auto
	call_request(void *manager, const uint32_t checkpoint) -> bool {
#ifdef _MSC_VER
		__try {
#endif
			g_request_warp(manager, checkpoint, NO_DIMENSION, KEEP_HERO, KEEP_LIGHTING);
			return true;
#ifdef _MSC_VER
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			return false;
		}
#endif
	}

	auto
	warp(const uint32_t checkpoint, const char **reason) -> bool {
		if (g_request_warp == nullptr) {
			return fail(reason, g_unavailable);
		}

		// the warp state loads regions and moves the hero
		if (!game_thread::on_game_thread()) {
			return fail(reason, "warps can only start on the game thread, and it is not pumping (loading?)");
		}

		// the warp state reads the checkpoint's data without checking it exists
		const auto *data = find_data(checkpoint);
		if (data == nullptr) {
			return fail(reason, "the level has no checkpoint with that name or hash");
		}

		auto *manager = transition_manager(reason);
		if (manager == nullptr) {
			return false;
		}

		if (manager[HERO_CHECKPOINT_WARP_PENDING] != 0) {
			return fail(reason, "a warp is already on its way");
		}

		if (!call_request(manager, checkpoint)) {
			return fail(reason, "RequestCheckpointWarp faulted");
		}

		char name[0x80];
		g_output << "[travel] warp to " << (ddl::read_string(data->name, name, sizeof(name)) ? name : "?") << " (region " << std::dec << data->region << ")\n";
		g_output.flush();
		return true;
	}

	// ----------------------------------------------------------------- zones --

	// level paths mix I29 and i29, so every match ignores case
	static auto
	contains(const char *text, const char *fragment) -> bool {
		if (fragment == nullptr || fragment[0] == '\0') {
			return true;
		}

		for (; *text != '\0'; ++text) {
			auto i = 0;
			while (fragment[i] != '\0' && text[i] != '\0' && tolower(static_cast<unsigned char>(text[i])) == tolower(static_cast<unsigned char>(fragment[i]))) {
				++i;
			}

			if (fragment[i] == '\0') {
				return true;
			}
		}

		return false;
	}

	// whether the load system has an overlay loaded: 1, 0, or -1 when that is
	// not known
	static auto
	overlay_loaded(const uint64_t asset) -> int32_t {
		if (g_overlay_manager_offset == 0) {
			return -1;
		}

		const auto *manager = reinterpret_cast<const OverlayManager *>(static_cast<const uint8_t *>(*g_instance) + g_overlay_manager_offset);
		if (!ddl::is_readable(manager, sizeof(OverlayManager)) || manager->entries == nullptr || manager->count <= 0 || !ddl::is_readable(manager->entries, sizeof(OverlayEntry) * static_cast<size_t>(manager->count))) {
			return -1;
		}

		for (int32_t i = 0; i < manager->count; ++i) {
			if (manager->entries[i].asset == asset) {
				return (manager->entries[i].flags & OverlayFlag::Loaded) != 0 ? 1 : 0;
			}
		}

		return -1;
	}

	// whether the story drives an overlay: the game then loads and unloads it
	// with its mission state, and undoes any request of ours
	static auto
	story_overlay(const uint64_t asset) -> bool {
		const auto *system = g_custom_overlays;
		if (system == nullptr || !ddl::is_readable(system, sizeof(CustomOverlaySystem)) || system->overlays == nullptr || system->count <= 0) {
			return false;
		}

		if (!ddl::is_readable(system->overlays, sizeof(CustomOverlay) * static_cast<size_t>(system->count))) {
			return false;
		}

		for (int32_t i = 0; i < system->count; ++i) {
			if (system->overlays[i].asset == asset) {
				return true;
			}
		}

		return false;
	}

	// zone -> the regions listing it, built once per level
	static const LevelAsset *g_indexed = nullptr;
	static std::vector<std::vector<int16_t>> g_zone_regions;

	static auto
	regions_of(const LevelAsset *asset, const int32_t zone) -> const std::vector<int16_t> & {
		if (g_indexed != asset || g_zone_regions.size() != static_cast<size_t>(asset->zoneCount)) {
			g_zone_regions.assign(static_cast<size_t>(asset->zoneCount), {});
			for (int32_t r = 0; r < asset->regionCount; ++r) {
				const auto &region = asset->regions[r];
				if (region.zoneRefStart < 0 || region.zoneRefCount <= 0 || region.zoneRefStart + region.zoneRefCount > asset->zoneRefCount) {
					continue;
				}

				for (auto k = 0; k < region.zoneRefCount; ++k) {
					const auto listed = asset->zoneRefs[region.zoneRefStart + k];
					if (listed >= 0 && listed < asset->zoneCount) {
						g_zone_regions[listed].push_back(static_cast<int16_t>(r));
					}
				}
			}

			g_indexed = asset;
		}

		return g_zone_regions[zone];
	}

	enum class RouteKind : uint8_t {
		None,
		Loaded,
		Checkpoint,
		Overlay,
		Story, // an overlay the story drives, out of reach
	};

	static auto
	kind_name(const RouteKind kind) -> const char * {
		switch (kind) {
			case RouteKind::Loaded: return "loaded";
			case RouteKind::Checkpoint: return "checkpoint";
			case RouteKind::Overlay: return "overlay";
			case RouteKind::Story: return "story";
			default: return "none";
		}
	}

	struct Route {
		RouteKind kind = RouteKind::None;
		int32_t region = -1;
		const CheckpointData *checkpoint = nullptr;
	};

	// a checkpoint that loads a region: one in the region itself, for a tile the
	// nearest one in the same open world (tiles stream by distance), for a
	// container or an open world any one under it
	static auto
	checkpoint_for(const LevelAsset *asset, const CheckpointManager *table, const int32_t region) -> const CheckpointData * {
		const auto &target = asset->regions[region];
		const auto top = top_of(asset, region);
		const auto *lod = target.type == RegionType::Tile && target.lodIndex >= 0 && target.lodIndex < asset->lodCount ? &asset->lods[target.lodIndex] : nullptr;

		const CheckpointData *best = nullptr;
		auto best_distance = 0.0f;
		for (int32_t i = 0; i < table->count; ++i) {
			const auto &checkpoint = table->checkpoints[i];
			if (checkpoint.region == region) {
				return &checkpoint;
			}

			if (lod != nullptr && top_of(asset, checkpoint.region) == top) {
				const auto dx = checkpoint.position[0] - lod->position[0];
				const auto dz = checkpoint.position[2] - lod->position[2];
				if (best == nullptr || dx * dx + dz * dz < best_distance) {
					best = &checkpoint;
					best_distance = dx * dx + dz * dz;
				}
			} else if (best == nullptr && (target.type == RegionType::Container || target.type == RegionType::OpenWorld) && top_of(asset, checkpoint.region) == region) {
				best = &checkpoint;
			}
		}

		return best;
	}

	// how to get a zone loaded, preferring always loaded, then a warp, then an
	// overlay on top of wherever the hero is
	static auto
	route_for(const LevelAsset *asset, const CheckpointManager *table, const int32_t zone) -> Route {
		Route best;
		for (const auto region : regions_of(asset, zone)) {
			const auto type = asset->regions[region].type;
			if (type == RegionType::Global) {
				return { RouteKind::Loaded, region, nullptr };
			}

			if (best.kind == RouteKind::Checkpoint) {
				continue;
			}

			if (type == RegionType::Overlay) {
				const auto kind = story_overlay(asset->regions[region].asset) ? RouteKind::Story : RouteKind::Overlay;
				if (best.kind == RouteKind::None || (best.kind == RouteKind::Story && kind == RouteKind::Overlay)) {
					best = { kind, region, nullptr };
				}

				continue;
			}

			if (const auto *checkpoint = checkpoint_for(asset, table, region); checkpoint != nullptr) {
				best = { RouteKind::Checkpoint, region, checkpoint };
			} else if (best.kind == RouteKind::None) {
				best.region = region;
			}
		}

		return best;
	}

	static auto
	region_json(const LevelAsset *asset, const int32_t region) -> nlohmann::json {
		char path[0x100];
		region_path(asset, region, path, sizeof(path));

		nlohmann::json out;
		out["index"] = region;
		out["path"] = path;
		out["type"] = type_name(asset->regions[region].type);
		return out;
	}

	auto
	regions(const char *filter, const size_t limit) -> nlohmann::json {
		nlohmann::json out;
		nlohmann::json::array_t found;
		auto truncated = false;

		const char *reason = nullptr;
		const auto *asset = level(&reason);
		out["reason"] = reason != nullptr ? reason : "";
		for (int32_t r = 0; asset != nullptr && r < asset->regionCount; ++r) {
			char path[0x100];
			if (!region_path(asset, r, path, sizeof(path)) || !contains(path, filter)) {
				continue;
			}

			if (found.size() >= limit) {
				truncated = true;
				break;
			}

			char area[0x80];
			area_of(asset, r, area, sizeof(area));

			auto entry = region_json(asset, r);
			entry["area"] = area;
			entry["zones"] = asset->regions[r].zoneRefCount;
			if (asset->regions[r].type == RegionType::Overlay) {
				const auto loaded = overlay_loaded(asset->regions[r].asset);
				entry["loaded"] = loaded < 0 ? nlohmann::json(nullptr) : nlohmann::json(loaded == 1);
				entry["story"] = story_overlay(asset->regions[r].asset);
			}

			found.emplace_back(std::move(entry));
		}

		out["regions"] = found;
		out["truncated"] = truncated;
		return out;
	}

	auto
	zones(const char *filter, const size_t limit) -> nlohmann::json {
		nlohmann::json out;
		nlohmann::json::array_t found;
		auto truncated = false;

		const char *reason = nullptr;
		const auto *table = manager(&reason);
		const auto *asset = table != nullptr ? level(&reason) : nullptr;
		out["reason"] = reason != nullptr ? reason : "";
		for (int32_t z = 0; asset != nullptr && z < asset->zoneCount; ++z) {
			char path[0x100];
			if (!zone_path(asset, z, path, sizeof(path)) || !contains(path, filter)) {
				continue;
			}

			if (found.size() >= limit) {
				truncated = true;
				break;
			}

			char id[20];
			_snprintf_s(id, sizeof(id), _TRUNCATE, "%016llx", asset->zones[z].asset);

			nlohmann::json::array_t listed;
			for (const auto region : regions_of(asset, z)) {
				listed.emplace_back(region_json(asset, region));
			}

			const auto route = route_for(asset, table, z);
			nlohmann::json way;
			way["kind"] = kind_name(route.kind);
			way["region"] = route.region;
			char name[0x80] = {};
			if (route.checkpoint != nullptr) {
				ddl::read_string(route.checkpoint->name, name, sizeof(name));
			}

			way["checkpoint"] = name[0] != '\0' ? nlohmann::json(name) : nlohmann::json(nullptr);

			nlohmann::json entry;
			entry["path"] = path;
			entry["asset"] = id;
			entry["regions"] = std::move(listed);
			entry["route"] = std::move(way);
			found.emplace_back(std::move(entry));
		}

		out["zones"] = found;
		out["truncated"] = truncated;
		return out;
	}

	static auto
	is_hex_id(const char *text) -> bool {
		if (text == nullptr || strlen(text) != 16) {
			return false;
		}

		for (const auto *c = text; *c != '\0'; ++c) {
			if (isxdigit(static_cast<unsigned char>(*c)) == 0) {
				return false;
			}
		}

		return true;
	}

	// a zone by asset id, exact path, or a fragment only one path contains
	static auto
	resolve_zone(const LevelAsset *asset, const char *text, const char **reason) -> int32_t {
		if (is_hex_id(text)) {
			const auto id = _strtoui64(text, nullptr, 16);
			for (int32_t z = 0; z < asset->zoneCount; ++z) {
				if (asset->zones[z].asset == id) {
					return z;
				}
			}

			fail(reason, "the level has no zone with that asset id");
			return -1;
		}

		auto match = -1;
		auto matches = 0;
		for (int32_t z = 0; z < asset->zoneCount; ++z) {
			char path[0x100];
			if (!zone_path(asset, z, path, sizeof(path))) {
				continue;
			}

			if (_stricmp(path, text) == 0) {
				return z;
			}

			if (contains(path, text)) {
				match = z;
				++matches;
			}
		}

		if (matches == 1) {
			return match;
		}

		fail(reason, matches == 0 ? "the level has no zone whose path contains that" : "several zones contain that, give more of the path");
		return -1;
	}

	// an overlay region by index, exact path, or a fragment only one overlay
	// path contains
	static auto
	resolve_overlay(const LevelAsset *asset, const char *text, const char **reason) -> int32_t {
		if (text[0] != '\0' && strspn(text, "0123456789") == strlen(text)) {
			const auto index = atoi(text);
			if (index >= 0 && index < asset->regionCount) {
				return index;
			}

			fail(reason, "there is no region with that index");
			return -1;
		}

		auto match = -1;
		auto matches = 0;
		for (int32_t r = 0; r < asset->regionCount; ++r) {
			if (asset->regions[r].type != RegionType::Overlay) {
				continue;
			}

			char path[0x100];
			if (!region_path(asset, r, path, sizeof(path))) {
				continue;
			}

			if (_stricmp(path, text) == 0) {
				return r;
			}

			if (contains(path, text)) {
				match = r;
				++matches;
			}
		}

		if (matches == 1) {
			return match;
		}

		fail(reason, matches == 0 ? "the level has no overlay whose path contains that" : "several overlays contain that, give more of the path");
		return -1;
	}

	// the engine call on its own, so a fault is caught with nothing to unwind
	static auto
	call_overlay(const bool load, const uint64_t region, bool *accepted) -> bool {
#ifdef _MSC_VER
		__try {
#endif
			*accepted = (load ? g_overlay_load : g_overlay_unload)(*g_instance, &region);
			return true;
#ifdef _MSC_VER
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			return false;
		}
#endif
	}

	static auto
	overlay_region(const LevelAsset *asset, const int32_t region, const bool load, char *message, const size_t message_size, const char **reason) -> bool {
		if (asset->regions[region].type != RegionType::Overlay) {
			return fail(reason, "that region is not an overlay");
		}

		// the story's overlay system undoes the request within a frame, and
		// forcing it would fight the mission state
		if (story_overlay(asset->regions[region].asset)) {
			return fail(reason, "the story drives that overlay: the game loads and unloads it with its mission state");
		}

		bool accepted = false;
		if (!call_overlay(load, asset->regions[region].asset, &accepted)) {
			return fail(reason, load ? "RequestOverlayLoad faulted" : "RequestOverlayUnload faulted");
		}

		if (!accepted) {
			return fail(reason, "the load system refused, it is loading or unloading something else");
		}

		char path[0x100];
		region_path(asset, region, path, sizeof(path));
		_snprintf_s(message, message_size, _TRUNCATE, "%s overlay %s", load ? "loading" : "unloading", path);
		g_output << "[travel] " << message << "\n";
		g_output.flush();
		return true;
	}

	auto
	go(const char *zone, char *message, const size_t message_size, const char **reason) -> bool {
		message[0] = '\0';
		if (!game_thread::on_game_thread()) {
			return fail(reason, "travel can only start on the game thread, and it is not pumping (loading?)");
		}

		const auto *table = manager(reason);
		const auto *asset = table != nullptr ? level(reason) : nullptr;
		if (asset == nullptr) {
			return false;
		}

		const auto index = resolve_zone(asset, zone, reason);
		if (index < 0) {
			return false;
		}

		const auto route = route_for(asset, table, index);
		switch (route.kind) {
			case RouteKind::Loaded:
				_snprintf_s(message, message_size, _TRUNCATE, "that zone is always loaded");
				return true;
			case RouteKind::Checkpoint: {
				if (!warp(route.checkpoint->nameHash, reason)) {
					return false;
				}

				char name[0x80];
				ddl::read_string(route.checkpoint->name, name, sizeof(name));
				_snprintf_s(message, message_size, _TRUNCATE, "warping to %s", name);
				return true;
			}
			case RouteKind::Overlay:
				if (g_overlay_load == nullptr) {
					return fail(reason, "the zone is in an overlay, and the overlay requests were not found");
				}

				return overlay_region(asset, route.region, true, message, message_size, reason);
			case RouteKind::Story:
				return fail(reason, "that zone is in a story overlay: the game loads it with the mission state it belongs to, and unloads it again if anything else does");
			default:
				return fail(reason, "no checkpoint loads that zone's region, and it is not in an overlay");
		}
	}

	// ---------------------------------------------------------------- tunnels --

	// letters and digits only, lower case: "Nefarious_City (open world)" and
	// "NEFARIOUS_CITY" both become nefariouscity...
	static auto
	squash(const char *text, char *out, const size_t size) -> void {
		size_t n = 0;
		for (; *text != '\0' && n + 1 < size; ++text) {
			if (isalnum(static_cast<unsigned char>(*text)) != 0) {
				out[n++] = static_cast<char>(tolower(static_cast<unsigned char>(*text)));
			}
		}

		out[n] = '\0';
	}

	// the planet tunnel for a destination: the one whose planet the destination's
	// area names ("savali" in "Savali (open world)", "zurk" for Zurkons and
	// ZURKIES), else the first one
	static auto
	tunnel_for(const LevelAsset *asset, const CheckpointManager *table, const CheckpointData *destination) -> const CheckpointData * {
		char area[0x80];
		char area_key[0x80];
		area_of(asset, destination->region, area, sizeof(area));
		squash(area, area_key, sizeof(area_key));

		const CheckpointData *first = nullptr;
		const auto prefix = strlen(TUNNEL_PREFIX);
		for (int32_t i = 0; i < table->count; ++i) {
			char name[0x80];
			if (!ddl::read_string(table->checkpoints[i].name, name, sizeof(name)) || strncmp(name, TUNNEL_PREFIX, prefix) != 0) {
				continue;
			}

			if (first == nullptr) {
				first = &table->checkpoints[i];
			}

			char planet[0x80];
			squash(name + prefix, planet, sizeof(planet));
			if (planet[0] != '\0' && (strstr(area_key, planet) != nullptr || (strlen(planet) >= 4 && strncmp(area_key, planet, 4) == 0))) {
				return &table->checkpoints[i];
			}
		}

		return first;
	}

	auto
	tunnels() -> nlohmann::json {
		nlohmann::json::array_t found;
		const auto *table = manager(nullptr);
		const auto prefix = strlen(TUNNEL_PREFIX);
		for (int32_t i = 0; table != nullptr && i < table->count; ++i) {
			char name[0x80];
			if (ddl::read_string(table->checkpoints[i].name, name, sizeof(name)) && strncmp(name, TUNNEL_PREFIX, prefix) == 0) {
				found.emplace_back(name);
			}
		}

		return found;
	}

	// the world position of an actor's scene object, false when it has none
	static auto
	position_of(const uint32_t handle, float out[3]) -> bool {
		const auto *actor = g_SceneManager != nullptr ? g_SceneManager->ResolveActor(EngineHandle { .value = handle }) : nullptr;
		if (actor == nullptr || actor->object == nullptr || !ddl::is_readable(actor->object, sizeof(SceneObject))) {
			return false;
		}

		for (auto i = 0; i < 3; ++i) {
			out[i] = actor->object->transform_matrix[3][i];
		}

		return true;
	}

	// the actor a node's single actor var holds, 0 when unset or a group
	static auto
	var_actor(const ScriptPlugs *plugs, const uint32_t var) -> uint32_t {
		if (plugs->varCount == 0 || plugs->varPlugs == nullptr || !ddl::is_readable(plugs->varPlugs, sizeof(ScriptVarPlug) * plugs->varCount)) {
			return 0;
		}

		for (auto i = 0; i < plugs->varCount; ++i) {
			const auto &plug = plugs->varPlugs[i];
			if (plug.plug != var || plug.varGeneration == 0 || g_SceneManager->scriptVars == nullptr || plug.varIndex >= g_SceneManager->scriptVarMax) {
				continue;
			}

			const auto *value = &g_SceneManager->scriptVars[plug.varIndex];
			if (ddl::is_readable(value, sizeof(ScriptVar)) && value->generation == plug.varGeneration && value->type == ScriptVarType::Actors && (value->actors & SCRIPT_VAR_GROUP_BIT) == 0) {
				return value->actors;
			}
		}

		return 0;
	}

	// the ship's planet menu listener nearest the hero, by the ship screen actor
	// it holds; the first one when none can be placed
	static auto
	ship_listener(const char **reason) -> const ScriptAction * {
		float hero[3] {};
		const auto have_hero = position_of(scene_query::hero(), hero);

		const ScriptAction *best = nullptr;
		auto best_distance = 0.0f;
		const auto listing = script_signal::nodes(SHIP_LISTENER, 16, nullptr);
		for (const auto &entry : listing["nodes"]) {
			if (entry["class"] != SHIP_LISTENER) {
				continue;
			}

			const auto handle = EngineHandle { .value = entry["component"].get<uint32_t>() };
			const auto *component = g_SceneManager->ResolveComponent(handle);
			if (component == nullptr || !ddl::is_readable(component, sizeof(ScriptAction)) || component->IsDestroyed()) {
				continue;
			}

			const auto *node = reinterpret_cast<const ScriptAction *>(component);
			if (node->plugs == nullptr || !ddl::is_readable(node->plugs, sizeof(ScriptPlugs))) {
				continue;
			}

			float screen[3] {};
			auto distance = 1e30f;
			if (have_hero && position_of(var_actor(node->plugs, SHIP_VAR_SCREEN), screen)) {
				distance = 0.0f;
				for (auto i = 0; i < 3; ++i) {
					distance += (screen[i] - hero[i]) * (screen[i] - hero[i]);
				}
			}

			if (best == nullptr || distance < best_distance) {
				best = node;
				best_distance = distance;
			}
		}

		if (best == nullptr) {
			fail(reason, "no ship is loaded here: travel starts from a ship, as the planet menu does");
		}

		return best;
	}

	// the listener's own accept, on its own, so a fault is caught with nothing to
	// unwind: the two vars the script reads, then the accept output
	static auto
	call_accept(void *plugs, const char *through, const char *target, bool *sent) -> bool {
#ifdef _MSC_VER
		__try {
#endif
			g_set_var_string(plugs, through, SHIP_VAR_TUNNEL);
			g_set_var_string(plugs, target, SHIP_VAR_DESTINATION);
			*sent = g_send_signal(plugs, SHIP_OUT_ACCEPT);
			return true;
#ifdef _MSC_VER
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			return false;
		}
#endif
	}

	auto
	fly(const char *destination, const char *via, char *message, const size_t message_size, const char **reason) -> bool {
		message[0] = '\0';
		if (g_destination_name == nullptr) {
			return fail(reason, "the planet menu pieces were not found");
		}

		if (!game_thread::on_game_thread()) {
			return fail(reason, "travel can only start on the game thread, and it is not pumping (loading?)");
		}

		const auto *table = manager(reason);
		const auto *asset = table != nullptr ? level(reason) : nullptr;
		if (asset == nullptr) {
			return false;
		}

		const auto *target = find_data(find(destination));
		if (target == nullptr) {
			return fail(reason, "the level has no destination checkpoint with that name or hash");
		}

		const CheckpointData *through = nullptr;
		if (via != nullptr && via[0] != '\0') {
			// "SAVALI" is short for CHK_TRANSITION_TO_SAVALI
			char full[0x100];
			_snprintf_s(full, sizeof(full), _TRUNCATE, "%s%s", _strnicmp(via, TUNNEL_PREFIX, strlen(TUNNEL_PREFIX)) == 0 ? "" : TUNNEL_PREFIX, via);
			through = find_data(find(full));
			if (through == nullptr) {
				return fail(reason, "the level has no planet tunnel by that name");
			}
		} else {
			through = tunnel_for(asset, table, target);
			if (through == nullptr) {
				return fail(reason, "the level has no planet tunnels");
			}
		}

		char target_name[0x80];
		char through_name[0x80];
		if (!ddl::read_string(target->name, target_name, sizeof(target_name)) || !ddl::read_string(through->name, through_name, sizeof(through_name))) {
			return fail(reason, "the checkpoint names are not readable");
		}

		if (!ddl::is_writable(g_tunnel_name, PLANET_MENU_CHECKPOINT_SIZE * 2)) {
			return fail(reason, "the planet menu checkpoint buffers are not writable");
		}

		const auto *listener = ship_listener(reason);
		if (listener == nullptr) {
			return false;
		}

		// the same two names the planet menu's listener stores on accept: the
		// tunnel script reads the destination from here once its cinematic ends
		strncpy_s(g_tunnel_name, PLANET_MENU_CHECKPOINT_SIZE, through_name, _TRUNCATE);
		strncpy_s(g_destination_name, PLANET_MENU_CHECKPOINT_SIZE, target_name, _TRUNCATE);

		bool sent = false;
		if (!call_accept(listener->plugs, through_name, target_name, &sent)) {
			return fail(reason, "the ship listener faulted");
		}

		if (!sent) {
			return fail(reason, "the ship's listener is not active, so nothing took the accept");
		}

		_snprintf_s(message, message_size, _TRUNCATE, "taking off for %s through %s", target_name, through_name);
		g_output << "[travel] " << message << "\n";
		g_output.flush();
		return true;
	}

	// ------------------------------------------------------------------- rift --

	// the passive shift controller, or null while none is loaded
	static auto
	shift_controller() -> uint8_t * {
		if (g_shift_controller == nullptr || g_SceneManager == nullptr || g_SceneManager->components == nullptr) {
			return nullptr;
		}

		const auto handle = EngineHandle { .value = *g_shift_controller };
		if (handle.value == 0 || handle.value == UINT32_MAX || static_cast<int32_t>(handle.id) >= g_SceneManager->componentMax) {
			return nullptr;
		}

		auto *component = g_SceneManager->ResolveComponent(handle);
		if (component == nullptr || !ddl::is_writable(component, PASSIVE_SHIFT_STATE + 1) || component->IsDestroyed()) {
			return nullptr;
		}

		return reinterpret_cast<uint8_t *>(component);
	}

	struct PortalRef {
		uint32_t actor = 0;
		Component *component = nullptr;
		const ComponentInfo *type = nullptr;
		bool passive = false; // a PortalPassiveShift: can follow the player and pull
		bool active = false;  // the actor is activated: the level may be using it
		float position[3] {};
		char name[0x40] {};
	};

	// an actor's Portal component, or false when it has none
	static auto
	portal_ref(const uint32_t handle, PortalRef &entry) -> bool {
		const auto *portal = scene_query::find_class("Portal");
		const auto *passive = scene_query::find_class("PortalPassiveShift");
		const auto *actor = g_SceneManager != nullptr ? g_SceneManager->ResolveActor(EngineHandle { .value = handle }) : nullptr;
		if (portal == nullptr || actor == nullptr || actor->components == nullptr || actor->componentCount <= 0 || !ddl::is_readable(actor->components, sizeof(ComponentPointer) * actor->componentCount)) {
			return false;
		}

		for (auto c = 0; c < actor->componentCount; ++c) {
			const auto [type, instance] = actor->components[c];
			if (type == nullptr || instance == nullptr || !ddl::is_readable(type, sizeof(ComponentInfo)) || !ddl::is_readable(instance, sizeof(Component)) || instance->IsDestroyed()) {
				continue;
			}

			if (type != portal && !type->DerivesFrom(portal)) {
				continue;
			}

			entry = {};
			entry.actor = handle;
			entry.component = instance;
			entry.type = type;
			entry.passive = passive != nullptr && (type == passive || type->DerivesFrom(passive));
			entry.active = (actor->flags & ActorFlag::Activated) != 0;
			position_of(handle, entry.position);
			if (const auto *name = actor->GetName(); name != nullptr) {
				ddl::read_string(name, entry.name, sizeof(entry.name));
			}

			return true;
		}

		return false;
	}

	// the actors with a Portal component loaded now
	static auto
	portals(PortalRef *out, const int32_t max) -> int32_t {
		const auto *portal = scene_query::find_class("Portal");
		if (portal == nullptr || g_SceneManager == nullptr) {
			return 0;
		}

		uint32_t handles[256];
		const auto found = scene_query::actors_with(portal, true, handles, 256, nullptr);
		int32_t count = 0;
		for (int32_t i = 0; i < found && count < max; ++i) {
			if (portal_ref(handles[i], out[count])) {
				++count;
			}
		}

		return count;
	}

	// the engine calls on their own, so a fault is caught with nothing to unwind
	static auto
	call_load_asset(const char *path, Asset **out) -> bool {
#ifdef _MSC_VER
		__try {
#endif
			*out = g_load_actor_asset(g_actor_assets, path, nullptr, nullptr);
			return true;
#ifdef _MSC_VER
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			return false;
		}
#endif
	}

	static auto
	call_spawn(const uint64_t asset, const float (*matrix)[4], const uint64_t uid, Actor **out) -> bool {
#ifdef _MSC_VER
		__try {
#endif
			*out = g_spawn_actor(asset, nullptr, matrix);
			if (*out != nullptr) {
				g_set_actor_uid(*out, uid);
			}

			return true;
#ifdef _MSC_VER
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			return false;
		}
#endif
	}

	// sets a float of one of an actor's components' prius, the whole asset's
	// when the prius is shared
	static auto
	prius_float(const uint32_t handle, const char *class_name, const char *field, const float value) -> bool {
		const auto *wanted = scene_query::find_class(class_name);
		const auto *actor = g_SceneManager != nullptr ? g_SceneManager->ResolveActor(EngineHandle { .value = handle }) : nullptr;
		if (wanted == nullptr || actor == nullptr || actor->components == nullptr) {
			return false;
		}

		for (auto c = 0; c < actor->componentCount; ++c) {
			const auto [type, instance] = actor->components[c];
			if (type != wanted || instance == nullptr || type->prius == nullptr || instance->ddlPriusData == nullptr) {
				continue;
			}

			auto *data = ddl::prius_data(instance->ddlPriusData, type->prius->allocation_size);
			const auto index = ddl::find_field(type->prius, field);
			if (data == nullptr || index < 0) {
				return false;
			}

			ddl::Value next {};
			next.kind = ddl::ValueKind::Real;
			next.as_real = value;
			const char *why = nullptr;
			return ddl::write_field(type->prius, data, index, 0, next, &why);
		}

		return false;
	}

	// one rift portal asset's state, asking for it when it is neither loading
	// nor loaded. Invalid when it cannot be had
	static auto
	rift_asset(const int32_t which) -> AssetStatus {
		auto *&asset = g_rift_assets[which];
		if (asset != nullptr && ddl::is_readable(asset, sizeof(Asset)) && asset->status >= AssetStatus::InQueue && asset->status <= AssetStatus::Loaded) {
			return asset->status;
		}

		Asset *loaded = nullptr;
		if (!call_load_asset(RIFT_PORTAL_ASSETS[which], &loaded) || loaded == nullptr || !ddl::is_readable(loaded, sizeof(Asset))) {
			asset = nullptr;
			return AssetStatus::Invalid;
		}

		asset = loaded;
		return loaded->status == AssetStatus::Error || loaded->status == AssetStatus::Aborted ? AssetStatus::Invalid : loaded->status;
	}

	// both rift portal assets: Loaded when both are, Invalid when either cannot
	// be had, else still loading
	static auto
	rift_assets() -> AssetStatus {
		if (g_spawn_actor == nullptr || g_rift_asset_failed) {
			return AssetStatus::Invalid;
		}

		const auto first = rift_asset(0);
		const auto second = rift_asset(1);
		if (first == AssetStatus::Invalid || second == AssetStatus::Invalid) {
			return AssetStatus::Invalid;
		}

		return first == AssetStatus::Loaded && second == AssetStatus::Loaded ? AssetStatus::Loaded : AssetStatus::Loading;
	}

	// the rift's four portals, spawned in the sky past the airlock where
	// missing. A and C (0 and 2) are rift portals, B and D any portal. they stay
	// between rifts, idle
	static auto
	own_portals(PortalRef *out) -> bool {
		for (auto i = 0; i < 4; ++i) {
			const auto follows = i == 0 || i == 2;
			if (g_rift_portals[i] != 0 && portal_ref(g_rift_portals[i], out[i]) && (out[i].passive || !follows)) {
				continue;
			}

			const float matrix[4][4] = {
				{ 1.0f, 0.0f, 0.0f, 0.0f },
				{ 0.0f, 1.0f, 0.0f, 0.0f },
				{ 0.0f, 0.0f, 1.0f, 0.0f },
				{ AIRLOCK_POSITION[0] + 50.0f * static_cast<float>(i + 1), AIRLOCK_POSITION[1], AIRLOCK_POSITION[2], 1.0f },
			};

			Actor *actor = nullptr;
			if (!call_spawn(g_rift_assets[follows ? 0 : 1]->assetId, matrix, RIFT_PORTAL_UID + static_cast<uint64_t>(i), &actor) || actor == nullptr) {
				g_rift_portals[i] = 0;
				return false;
			}

			g_rift_portals[i] = scene_query::handle_of(actor);
			if (g_rift_portals[i] == 0 || !portal_ref(g_rift_portals[i], out[i]) || (follows && !out[i].passive)) {
				g_output << "[travel] spawned rift portal " << (i + 1) << " is not the kind of portal it has to be\n";
				g_output.flush();
				g_rift_portals[i] = 0;
				return false;
			}

			// the test portals open in no time, and a rift portal that has not
			// taken time to open never pulls
			prius_float(g_rift_portals[i], "PortalRender", "TimeToOpen", RIFT_PORTAL_OPEN_TIME);
			g_output << "[travel] spawned rift portal " << (i + 1) << ", actor " << g_rift_portals[i] << "\n";
			g_output.flush();
		}

		for (auto i = 0; i < 4; ++i) {
			_snprintf_s(out[i].name, sizeof(out[i].name), _TRUNCATE, "rift portal %d", i + 1);
		}

		return true;
	}

	// sets one bool of a portal's prius, handing back what it was
	static auto
	prius_bool(const PortalRef &portal, const char *field, const bool value, int8_t *previous) -> bool {
		const auto *prius = portal.type->prius;
		if (prius == nullptr || !ddl::is_readable(prius, sizeof(DDLTypeInfo)) || portal.component->ddlPriusData == nullptr) {
			return false;
		}

		auto *data = ddl::prius_data(portal.component->ddlPriusData, prius->allocation_size);
		const auto index = ddl::find_field(prius, field);
		if (data == nullptr || index < 0) {
			return false;
		}

		if (previous != nullptr) {
			const auto old = ddl::read_field(prius, data, index);
			*previous = old.kind == ddl::ValueKind::Bool ? (old.as_bool ? 1 : 0) : (old.as_unsigned != 0 ? 1 : 0);
		}

		ddl::Value next {};
		next.kind = ddl::ValueKind::Bool;
		next.as_bool = value;
		const char *why = nullptr;
		return ddl::write_field(prius, data, index, 0, next, &why);
	}

	// an actor's world matrix, writable, or null
	static auto
	matrix_of(const uint32_t handle) -> float (*)[4] {
		auto *actor = g_SceneManager != nullptr ? g_SceneManager->ResolveActor(EngineHandle { .value = handle }) : nullptr;
		if (actor == nullptr || actor->object == nullptr || !ddl::is_writable(actor->object, sizeof(SceneObject))) {
			return nullptr;
		}

		return actor->object->transform_matrix;
	}

	static auto
	restore_borrowed() -> void {
		PortalRef loaded[64];
		const auto count = portals(loaded, 64);
		for (int32_t i = 0; i < g_borrowed_count; ++i) {
			const auto &borrowed = g_borrowed[i];
			if (auto *matrix = matrix_of(borrowed.actor); matrix != nullptr) {
				memcpy(matrix, borrowed.matrix, sizeof(borrowed.matrix));
			}

			for (int32_t p = 0; p < count; ++p) {
				if (loaded[p].actor != borrowed.actor) {
					continue;
				}

				if (borrowed.followPlayer >= 0) {
					prius_bool(loaded[p], "FollowPlayer", borrowed.followPlayer != 0, nullptr);
				}

				if (borrowed.gravityWell >= 0) {
					prius_bool(loaded[p], "GravityWell", borrowed.gravityWell != 0, nullptr);
				}
			}
		}

		g_borrowed_count = 0;
		g_rift_busy_seen = false;
		g_rift_started = 0;
		g_rift_state = 0;
		g_output << "[travel] the rift's portals are put back\n";
		g_output.flush();
	}

	// the engine call on its own, so a fault is caught with nothing to unwind
	static auto
	call_stop(void *controller) -> bool {
		const uint32_t trigger = 0;
#ifdef _MSC_VER
		__try {
#endif
			g_shift_stop(controller, &trigger);
			return true;
#ifdef _MSC_VER
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			return false;
		}
#endif
	}

	auto
	pump() -> void {
		if (g_pending.waiting) {
			const auto status = rift_assets();
			if (status == AssetStatus::Loaded || status == AssetStatus::Invalid || GetTickCount64() - g_pending.since > RIFT_ASSET_TIMEOUT_MS) {
				const auto pending = g_pending;
				g_pending = {};
				char message[0x180];
				const char *reason = "refused";
				if (status != AssetStatus::Loaded) {
					g_rift_asset_failed = true;
					g_output << "[travel] rift: the rift portal asset " << (status == AssetStatus::Invalid ? "failed to load" : "took too long to load") << "; trying the level's portals\n";
				}

				if (rift(pending.at_position ? "" : pending.checkpoint, pending.at_position ? pending.position : nullptr, message, sizeof(message), &reason)) {
					g_output << "[travel] " << message << "\n";
				} else {
					g_output << "[travel] rift: " << reason << "\n";
				}

				g_output.flush();
			}
		}

		if (g_borrowed_count == 0) {
			return;
		}

		auto *controller = shift_controller();
		const auto state = controller != nullptr ? controller[PASSIVE_SHIFT_STATE] : static_cast<uint8_t>(0);
		if (state != g_rift_state) {
			g_output << "[travel] rift: " << (state < std::size(SHIFT_STATE_NAMES) ? SHIFT_STATE_NAMES[state] : "?") << " after " << std::dec << (GetTickCount64() - g_rift_started) << " ms\n";
			g_output.flush();
			g_rift_state = state;
		}

		if (state != 0) {
			g_rift_busy_seen = true;
		}

		if (g_rift_busy_seen && state == 0) {
			restore_borrowed();
			return;
		}

		// stuck: end the shift the way its script node's Stop input does, which
		// closes the portals; the hero may need a warp out of the glide
		if (GetTickCount64() - g_rift_started > RIFT_TIMEOUT_MS) {
			if (controller != nullptr && state != 0) {
				call_stop(controller);
				g_output << "[travel] rift: timed out and stopped; level.warp a checkpoint if the hero is still gliding\n";
			}

			restore_borrowed();
		}
	}

	// the engine calls on their own, so a fault is caught with nothing to unwind
	static auto
	call_shift(void *controller, const ShiftParams *params, const ActorArray *actors, const uint32_t *trigger, bool *started) -> bool {
#ifdef _MSC_VER
		__try {
#endif
			*started = g_shift_set_params(controller, params, actors) && g_shift_start(controller, trigger);
			return true;
#ifdef _MSC_VER
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			return false;
		}
#endif
	}

	static auto
	distance(const float *a, const float *b) -> float {
		return sqrtf((a[0] - b[0]) * (a[0] - b[0]) + (a[1] - b[1]) * (a[1] - b[1]) + (a[2] - b[2]) * (a[2] - b[2]));
	}

	// the planet the hero is on: the area of the checkpoint nearest them, or -1
	static auto
	hero_area(const LevelAsset *asset, const CheckpointManager *table, const float *hero) -> int32_t {
		const CheckpointData *home = nullptr;
		for (int32_t i = 0; i < table->count; ++i) {
			if (home == nullptr || distance(table->checkpoints[i].position, hero) < distance(home->position, hero)) {
				home = &table->checkpoints[i];
			}
		}

		return home != nullptr ? top_of(asset, home->region) : -1;
	}

	// the checkpoint an airlock at target loads: the nearest one to target on the
	// hero's planet
	static auto
	checkpoint_near(const LevelAsset *asset, const CheckpointManager *table, const float *hero, const float *target) -> const CheckpointData * {
		const auto area = hero_area(asset, table, hero);
		if (area < 0) {
			return nullptr;
		}

		const CheckpointData *best = nullptr;
		for (int32_t i = 0; i < table->count; ++i) {
			const auto &checkpoint = table->checkpoints[i];
			if (top_of(asset, checkpoint.region) == area && (best == nullptr || distance(checkpoint.position, target) < distance(best->position, target))) {
				best = &checkpoint;
			}
		}

		return best;
	}

	auto
	rift(const char *checkpoint, const float *position, char *message, const size_t message_size, const char **reason) -> bool {
		message[0] = '\0';
		if (g_shift_controller == nullptr) {
			return fail(reason, "the passive shift controller was not found");
		}

		if (!game_thread::on_game_thread()) {
			return fail(reason, "rifts can only open on the game thread, and it is not pumping (loading?)");
		}

		if (g_borrowed_count > 0) {
			return fail(reason, "a rift is still open");
		}

		if (g_pending.waiting) {
			return fail(reason, "a rift is waiting for its portals to load");
		}

		auto *controller = shift_controller();
		if (controller == nullptr) {
			return fail(reason, "the passive shift controller is not loaded");
		}

		if (controller[PASSIVE_SHIFT_STATE] != 0) {
			return fail(reason, "a passive shift is already under way");
		}

		const auto *table = manager(reason);
		const auto *asset = table != nullptr ? level(reason) : nullptr;
		if (asset == nullptr) {
			return false;
		}

		const auto hero = scene_query::hero();
		float hero_at[3] {};
		if (hero == 0 || !position_of(hero, hero_at)) {
			return fail(reason, "there is no hero right now");
		}

		// where the rift leads, and the checkpoint its airlock loads
		float target[3] {};
		const CheckpointData *through = nullptr;
		if (checkpoint != nullptr && checkpoint[0] != '\0') {
			through = find_data(find(checkpoint));
			if (through == nullptr) {
				return fail(reason, "the level has no checkpoint with that name or hash");
			}

			memcpy(target, through->position, sizeof(target));
		} else if (position != nullptr) {
			memcpy(target, position, sizeof(target));
			through = checkpoint_near(asset, table, hero_at, target);
			if (through == nullptr) {
				return fail(reason, "no checkpoint near the target for the airlock to load");
			}
		} else {
			return fail(reason, "the rift needs a checkpoint or a position");
		}

		// another planet unloads this one while the hero is in the airlock, like the
		// game's own shifts. the rift's spawned portals belong to no zone and
		// outlive that; borrowed ones are this planet's actors and would not
		const auto other_planet = top_of(asset, through->region) != hero_area(asset, table, hero_at);

		// the game's shift: A opens next to the hero and pulls them in, B is where
		// they come out gliding in the airlock, C opens in front of them once the
		// far end is loaded, D lets them out at the target. A and C have to be
		// rift portals, the kind that can follow the hero. the rift's own spawned
		// portals are used when their asset is in; it loads on the first rift
		PortalRef own[4];
		switch (rift_assets()) {
		case AssetStatus::Loaded:
			if (own_portals(own)) {
				break;
			}

			return fail(reason, "the rift portals could not be spawned");

		case AssetStatus::InQueue:
		case AssetStatus::Staging:
		case AssetStatus::Loading:
			g_pending = {};
			g_pending.waiting = true;
			g_pending.since = GetTickCount64();
			if (checkpoint != nullptr && checkpoint[0] != '\0') {
				strncpy_s(g_pending.checkpoint, checkpoint, _TRUNCATE);
			} else {
				g_pending.at_position = true;
				memcpy(g_pending.position, position, sizeof(g_pending.position));
			}

			_snprintf_s(message, message_size, _TRUNCATE, "loading the rift portals; the rift opens as soon as they are in");
			return true;

		default:
			break;
		}

		const auto spawned = own[0].component != nullptr;
		if (other_planet && !spawned) {
			return fail(reason, "that checkpoint is on another planet, which only the rift's own portals reach, and they could not be loaded");
		}

		// without them, the level's own portals are borrowed, idle ones first
		PortalRef loaded[64];
		const auto count = spawned ? 0 : portals(loaded, 64);
		const PortalRef *taken[4] {};
		auto taken_count = 0;
		const auto pick = [&](const bool passive) -> const PortalRef * {
			const PortalRef *best = nullptr;
			for (int32_t i = 0; i < count; ++i) {
				const auto *candidate = &loaded[i];
				if (passive && !candidate->passive) {
					continue;
				}

				if (std::find(taken, taken + taken_count, candidate) != taken + taken_count) {
					continue;
				}

				// idle first, then the farthest from the hero
				if (best == nullptr || (best->active && !candidate->active) || (best->active == candidate->active && distance(candidate->position, hero_at) > distance(best->position, hero_at))) {
					best = candidate;
				}
			}

			if (best != nullptr) {
				taken[taken_count++] = best;
			}

			return best;
		};

		const auto *a = spawned ? &own[0] : pick(true);
		const auto *c = spawned ? &own[2] : pick(true);
		const auto *b = spawned ? &own[1] : pick(false);
		const auto *d = spawned ? &own[3] : pick(false);
		if (a == nullptr || c == nullptr || b == nullptr || d == nullptr) {
			static char why[0x80];
			_snprintf_s(why, sizeof(why), _TRUNCATE, "the rift portal asset did not load, and a rift borrows two rift portals and two more loaded here; there are %d portals", count);
			return fail(reason, why);
		}

		float(*matrices[4])[4] = { matrix_of(a->actor), matrix_of(b->actor), matrix_of(c->actor), matrix_of(d->actor) };
		for (const auto *matrix : matrices) {
			if (matrix == nullptr) {
				return fail(reason, "a borrowed portal has no writable transform");
			}
		}

		// remember everything to put back
		const PortalRef *lent[4] = { a, b, c, d };
		for (auto i = 0; i < 4; ++i) {
			g_borrowed[i] = {};
			g_borrowed[i].actor = lent[i]->actor;
			memcpy(g_borrowed[i].matrix, matrices[i], sizeof(g_borrowed[i].matrix));
		}

		g_borrowed_count = 4;
		g_rift_started = GetTickCount64();
		g_rift_busy_seen = false;
		g_rift_state = 0;

		// A and C follow the hero, only A pulls, like the game's own. the spawned
		// A and C share one prius, so both pull; C only opens once the hero is
		// gliding, when a pull no longer starts a shift. theirs stays set
		if (spawned) {
			prius_bool(*a, "FollowPlayer", true, nullptr);
			prius_bool(*a, "GravityWell", true, nullptr);
			if (b->passive) {
				prius_bool(*b, "FollowPlayer", false, nullptr);
				prius_bool(*b, "GravityWell", false, nullptr);
			}
		} else {
			prius_bool(*a, "FollowPlayer", true, &g_borrowed[0].followPlayer);
			prius_bool(*a, "GravityWell", true, &g_borrowed[0].gravityWell);
			prius_bool(*c, "FollowPlayer", true, &g_borrowed[2].followPlayer);
			prius_bool(*c, "GravityWell", false, &g_borrowed[2].gravityWell);
		}

		// B at the game's airlock facing +z, D upright over the target facing the
		// way the hero was headed. the matrices keep their rows' scale
		const auto place = [](float (*matrix)[4], const float *at, const float *forward) {
			const float up[3] = { 0.0f, 1.0f, 0.0f };
			const float right[3] = { forward[2], 0.0f, -forward[0] }; // up x forward
			const float *axes[3] = { right, up, forward };
			for (auto row = 0; row < 3; ++row) {
				const auto scale = sqrtf(matrix[row][0] * matrix[row][0] + matrix[row][1] * matrix[row][1] + matrix[row][2] * matrix[row][2]);
				for (auto axis = 0; axis < 3; ++axis) {
					matrix[row][axis] = axes[row][axis] * (scale > 0.0f ? scale : 1.0f);
				}
			}

			for (auto axis = 0; axis < 3; ++axis) {
				matrix[3][axis] = at[axis];
			}
		};

		const float airlock_forward[3] = { 0.0f, 0.0f, 1.0f };
		place(matrices[1], AIRLOCK_POSITION, airlock_forward);

		float heading[3] = { target[0] - hero_at[0], 0.0f, target[2] - hero_at[2] };
		const auto length = sqrtf(heading[0] * heading[0] + heading[2] * heading[2]);
		if (length > 0.01f) {
			heading[0] /= length;
			heading[2] /= length;
		} else {
			heading[0] = 0.0f;
			heading[2] = 1.0f;
		}

		float exit[3] = { target[0], target[1] + RIFT_EXIT_HEIGHT, target[2] };
		place(matrices[3], exit, heading);

		ShiftParams params {};
		params.startSource = a->component->handle.value;
		params.startDest = b->component->handle.value;
		params.endSource = c->component->handle.value;
		params.endDest = d->component->handle.value;
		params.checkpoint = through->nameHash;

		const ActorArray actors { &hero, 1, 1 };
		const uint32_t trigger = 0;
		bool started = false;
		if (!call_shift(controller, &params, &actors, &trigger, &started) || !started) {
			restore_borrowed();
			return fail(reason, started ? "the passive shift controller faulted" : "the passive shift controller refused the rift");
		}

		char checkpoint_name[0x80];
		ddl::read_string(through->name, checkpoint_name, sizeof(checkpoint_name));
		_snprintf_s(message, message_size, _TRUNCATE, "rift open: in by %s, airlock %s -> %s, out by %s at %.0f %.0f %.0f (airlock loads %s)", a->name, b->name, c->name, d->name, exit[0], exit[1], exit[2], checkpoint_name);
		g_output << "[travel] " << message << "\n";
		g_output.flush();
		return true;
	}

	auto
	overlay(const char *region, const bool load, char *message, const size_t message_size, const char **reason) -> bool {
		message[0] = '\0';
		if (g_overlay_load == nullptr) {
			return fail(reason, "the overlay requests were not found");
		}

		if (!game_thread::on_game_thread()) {
			return fail(reason, "overlays can only load on the game thread, and it is not pumping (loading?)");
		}

		const auto *asset = level(reason);
		if (asset == nullptr) {
			return false;
		}

		const auto index = resolve_overlay(asset, region, reason);
		return index >= 0 && overlay_region(asset, index, load, message, message_size, reason);
	}
} // namespace rivet_hook::travel

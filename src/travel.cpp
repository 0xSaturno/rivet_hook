// SPDX-FileCopyrightText: 2025-2026 Neptuwunium
//
// SPDX-License-Identifier: EUPL-1.2

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdlib>
#include <cstring>

#include "travel.hpp"

#include "ddl_visit.hpp"
#include "game/load_system.hpp"
#include "game/scene_manager.hpp"
#include "game_thread.hpp"
#include "runtime.hpp"
#include "scene_query.hpp"
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

	// where the load system instance pointer lives. it is set once the game has
	// a load system, so it is read at use
	static void *const *g_instance = nullptr;
	static uint32_t g_checkpoints_offset = 0;
	static request_warp_t g_request_warp = nullptr;
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
		out["reason"] = reason != nullptr ? reason : "";
		if (table != nullptr) {
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

				nlohmann::json entry;
				entry["name"] = name;
				entry["hash"] = hash;
				entry["region"] = checkpoint.region;
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
		g_output << "[travel] warp to " << (ddl::read_string(data->name, name, sizeof(name)) ? name : "?") << " (region " << data->region << ")\n";
		g_output.flush();
		return true;
	}
} // namespace rivet_hook::travel

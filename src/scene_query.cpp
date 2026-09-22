// SPDX-FileCopyrightText: 2025-2026 Neptuwunium
//
// SPDX-License-Identifier: EUPL-1.2

#include <cstring>
#include <string>
#include <unordered_map>

#include "scene_query.hpp"

#include "ddl_visit.hpp"
#include "game/hero_manager.hpp"
#include "game/scene_manager.hpp"
#include "runtime.hpp"
#include "signature.hpp"
#include "signature_engine.hpp"

using namespace rivet_hook::game;

namespace rivet_hook {
	// overlay_core owns these, they are resolved once during Overlay::Init
	extern SceneManager *g_SceneManager;
	extern HeroSystem *g_HeroManager;
} // namespace rivet_hook

namespace rivet_hook::scene_query {
	constexpr int32_t EXPIRY_CHECK_MASK = 0x3FF;

	auto
	handle_of(const Actor *actor) -> uint32_t {
		if (g_SceneManager == nullptr || g_SceneManager->actors == nullptr || actor < g_SceneManager->actors) {
			return 0;
		}

		const auto index = actor - g_SceneManager->actors;
		if (index >= g_SceneManager->actorMax) {
			return 0;
		}

		return EngineHandle { .id = static_cast<uint32_t>(index), .generation = actor->generation }.value;
	}

	auto
	hero() -> uint32_t {
		// a global in the game image, so no readability check: that is a
		// VirtualQuery, and it would cost more than everything else here
		if (g_HeroManager == nullptr || g_SceneManager == nullptr) {
			return 0;
		}

		const auto handle = g_HeroManager->currentActorHandle;
		const auto *actor = g_SceneManager->ResolveActor(handle);
		if (actor == nullptr || !actor->IsValid()) {
			return 0;
		}

		return handle.value;
	}

	auto
	uid_of(const Actor *actor) -> uint64_t {
		const auto handle = handle_of(actor);
		if (handle == 0 || g_SceneManager->actorUids == nullptr) {
			return 0;
		}

		return g_SceneManager->actorUids[EngineHandle { .value = handle }.id];
	}

	auto
	actor_by_uid(const uint64_t uid) -> uint32_t {
		if (g_SceneManager == nullptr || uid == 0) {
			return 0;
		}

		using by_uid_t = uint32_t *(*)(SceneManager *scene, uint32_t *out, uint64_t uid);
		static const auto engine_by_uid = reinterpret_cast<by_uid_t>(find_address(ACTOR_HANDLE_BY_UID_SIGNATURE));

		EngineHandle handle {};
		if (engine_by_uid != nullptr) {
			engine_by_uid(g_SceneManager, &handle.value, uid);
		} else {
			// the probe could not be found, so walk the table: ~95k keys, well
			// under a millisecond
			const auto *keys = g_SceneManager->uidKeys;
			const auto capacity = g_SceneManager->uidCapacity;
			for (int32_t i = 0; keys != nullptr && i < capacity; ++i) {
				if (keys[i] == uid) {
					handle.value = handle_of(g_SceneManager->uidActors[i]);
					break;
				}
			}
		}

		const auto *actor = handle.value != 0 ? g_SceneManager->ResolveActor(handle) : nullptr;
		return actor != nullptr && actor->IsValid() ? handle.value : 0;
	}

	// classes register once at startup, so the name index is built on first use
	// and only rebuilt if the registered count ever changes
	auto
	find_class(const char *name) -> const ComponentInfo * {
		static const ComponentInfo *const *const *registry = nullptr;
		static const int32_t *count = nullptr;
		static int32_t indexed = -1;
		static std::unordered_map<std::string, const ComponentInfo *> by_name;

		if (registry == nullptr || count == nullptr) {
			const auto site = find_address(COMPONENT_REGISTER_SIGNATURE);
			registry = static_cast<const ComponentInfo *const *const *>(load_rel_var(site, COMPONENT_REGISTRY_ADDRESS));
			count = static_cast<const int32_t *>(load_rel_var(site, COMPONENT_COUNT_ADDRESS));
			if (registry == nullptr || count == nullptr) {
				registry = nullptr;
				return nullptr;
			}
		}

		const auto *infos = *registry;
		if (infos == nullptr) {
			return nullptr;
		}

		if (indexed != *count) {
			by_name.clear();
			for (int32_t i = 0; i < *count; ++i) {
				const auto *info = infos[i];
				if (info == nullptr || !ddl::is_readable(info, sizeof(ComponentInfo))) {
					continue;
				}

				char text[0x100];
				if (ddl::read_string(info->name, text, sizeof(text))) {
					by_name.emplace(text, info);
				}
			}

			indexed = *count;
		}

		const auto found = by_name.find(name);
		return found != by_name.end() ? found->second : nullptr;
	}

	auto
	actors_with(const ComponentInfo *type, const bool derived, uint32_t *out, const int32_t max, bool (*expired)()) -> int32_t {
		if (g_SceneManager == nullptr || type == nullptr) {
			return 0;
		}

		const auto *lookups = g_SceneManager->componentLookups;
		const auto count = g_SceneManager->componentLookupCount;
		if (lookups == nullptr || count <= 0 || !ddl::is_readable(lookups, sizeof(ComponentLookup) * static_cast<size_t>(count))) {
			return 0;
		}

		int32_t found = 0;
		for (int32_t i = 0; i < count && found < max; ++i) {
			if ((i & EXPIRY_CHECK_MASK) == 0 && expired != nullptr && expired()) {
				return -1;
			}

			const auto [component, component_type] = lookups[i];
			if (component == nullptr || component_type == nullptr) {
				continue;
			}

			if (component_type != type && (!derived || !component_type->DerivesFrom(type))) {
				continue;
			}

			if (component->IsDestroyed() || component->actor == nullptr || !component->actor->IsValid()) {
				continue;
			}

			const auto handle = handle_of(component->actor);
			if (handle == 0) {
				continue;
			}

			// an actor can hold several components of a class, list it once
			auto seen = false;
			for (int32_t j = 0; j < found && !seen; ++j) {
				seen = out[j] == handle;
			}

			if (!seen) {
				out[found++] = handle;
			}
		}

		return found;
	}
} // namespace rivet_hook::scene_query

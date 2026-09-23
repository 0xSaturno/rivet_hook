// SPDX-FileCopyrightText: 2025-2026 Neptuwunium
//
// SPDX-License-Identifier: EUPL-1.2

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cctype>
#include <cstdlib>
#include <cstring>

#include "vanity.hpp"

#include "ddl_visit.hpp"
#include "game/scene_manager.hpp"
#include "game_thread.hpp"
#include "runtime.hpp"
#include "runtime_loader.hpp"
#include "signature.hpp"

using namespace rivet_hook::game;

namespace rivet_hook {
	// overlay_core owns this, it is resolved once during Overlay::Init
	extern SceneManager *g_SceneManager;
} // namespace rivet_hook

namespace rivet_hook::vanity {
	// the hero type argument that means "whichever hero the manager belongs to"
	constexpr int32_t HERO_TYPE_OWN = 4;

	using equip_bundle_t = bool (*)(void *manager, uint64_t bundle, int32_t hero_type);
	using has_bundle_t = bool (*)(void *manager, uint64_t bundle, int32_t hero_type);
	using lookup_config_t = const void *(*)(void *configs, uint64_t id);

	static equip_bundle_t g_equip_bundle = nullptr;
	static has_bundle_t g_has_bundle = nullptr;
	static lookup_config_t g_lookup_config = nullptr;
	static void *g_configs = nullptr;

	auto
	init() -> void {
		if (g_equip_bundle != nullptr) {
			return;
		}

		g_equip_bundle = reinterpret_cast<equip_bundle_t>(find_address(VANITY_EQUIP_BUNDLE_SIGNATURE));
		const auto has = find_address(VANITY_HAS_BUNDLE_SIGNATURE);
		g_has_bundle = reinterpret_cast<has_bundle_t>(has);
		g_configs = load_rel_var(has, VANITY_HAS_BUNDLE_CONFIGS_ADDRESS);
		g_lookup_config = reinterpret_cast<lookup_config_t>(load_rel_var(has, VANITY_HAS_BUNDLE_LOOKUP_ADDRESS));
		if (g_equip_bundle == nullptr || g_has_bundle == nullptr || g_configs == nullptr || g_lookup_config == nullptr) {
			g_equip_bundle = nullptr;
			g_output << "[vanity] the vanity calls were not found, rivet.vanity_equip is unavailable\n";
		} else {
			g_output << "[vanity] EquipBundle at " << reinterpret_cast<void *>(g_equip_bundle) << "\n";
		}

		g_output.flush();
	}

	auto
	bundle_id(const char *text, uint64_t &out) -> bool {
		if (text == nullptr || text[0] == '\0') {
			return false;
		}

		// 16 hex digits is an id already
		const auto *digits = (text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) ? text + 2 : text;
		if (strlen(digits) == 16) {
			auto hex = true;
			for (const auto *c = digits; *c != '\0'; ++c) {
				hex = hex && std::isxdigit(static_cast<unsigned char>(*c)) != 0;
			}

			if (hex) {
				out = _strtoui64(digits, nullptr, 16);
				return true;
			}
		}

		return AssetLoader::asset_id(text, out);
	}

	// the live VanityInventoryManager on an actor, or null
	static auto
	manager_of(const uint32_t handle, const char **reason) -> void * {
		const auto fail = [reason](const char *why) -> void * {
			if (reason != nullptr) {
				*reason = why;
			}

			return nullptr;
		};

		if (g_equip_bundle == nullptr) {
			return fail("the vanity calls were not found");
		}

		// equipping loads and attaches model parts through the actor's components
		if (!game_thread::on_game_thread()) {
			return fail("outfits can only change on the game thread, and it is not pumping (loading?)");
		}

		if (g_SceneManager == nullptr) {
			return fail("the scene manager is not available");
		}

		EngineHandle engine_handle {};
		engine_handle.value = handle;
		const auto *actor = g_SceneManager->ResolveActor(engine_handle);
		if (actor == nullptr) {
			return fail("no actor for that handle");
		}

		if (actor->components == nullptr || actor->componentCount <= 0 || !ddl::is_readable(actor->components, sizeof(ComponentPointer) * actor->componentCount)) {
			return fail("the actor has no readable component list");
		}

		for (auto index = 0; index < actor->componentCount; ++index) {
			const auto [type, instance] = actor->components[index];
			if (type == nullptr || instance == nullptr || !ddl::is_readable(type, sizeof(ComponentInfo))) {
				continue;
			}

			char name[0x100];
			if (!ddl::read_string(type->name, name, sizeof(name)) || strcmp(name, "VanityInventoryManager") != 0) {
				continue;
			}

			if (!ddl::is_readable(instance, sizeof(Component)) || instance->IsDestroyed()) {
				continue;
			}

			return instance;
		}

		return fail("the actor has no VanityInventoryManager, which only the hero carries");
	}

	// the engine calls on their own, so a fault is caught with nothing to unwind
	static auto
	call(const bool equip, void *manager, const uint64_t bundle, bool *result) -> bool {
#ifdef _MSC_VER
		__try {
#endif
			*result = equip ? g_equip_bundle(manager, bundle, HERO_TYPE_OWN) : g_has_bundle(manager, bundle, HERO_TYPE_OWN);
			return true;
#ifdef _MSC_VER
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			return false;
		}
#endif
	}

	static auto
	call_lookup(const uint64_t id, const void **out) -> bool {
#ifdef _MSC_VER
		__try {
#endif
			*out = g_lookup_config(g_configs, id);
			return true;
#ifdef _MSC_VER
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			return false;
		}
#endif
	}

	auto
	has_bundle(const uint32_t actor, const uint64_t bundle, bool &owned, const char **reason) -> bool {
		auto *manager = manager_of(actor, reason);
		if (manager == nullptr) {
			return false;
		}

		// both HasBundle and EquipBundle dereference the config without a check,
		// so an id with no loaded config never reaches them
		const void *config = nullptr;
		if (!call_lookup(bundle, &config) || config == nullptr) {
			if (reason != nullptr) {
				*reason = "there is no loaded bundle config with that id or path";
			}

			return false;
		}

		if (!call(false, manager, bundle, &owned)) {
			if (reason != nullptr) {
				*reason = "HasBundle faulted";
			}

			return false;
		}

		return true;
	}

	auto
	equip_bundle(const uint32_t actor, const uint64_t bundle, bool &equipped, const char **reason) -> bool {
		bool owned = false;
		if (!has_bundle(actor, bundle, owned, reason)) {
			return false;
		}

		// a bundle the hero has not unlocked is the game's own refusal to make
		if (!owned) {
			if (reason != nullptr) {
				*reason = "the hero does not own that bundle";
			}

			return false;
		}

		auto *manager = manager_of(actor, reason);
		if (manager == nullptr) {
			return false;
		}

		if (!call(true, manager, bundle, &equipped)) {
			if (reason != nullptr) {
				*reason = "EquipBundle faulted";
			}

			g_output << "[vanity] EquipBundle faulted\n";
			g_output.flush();
			return false;
		}

		return true;
	}
} // namespace rivet_hook::vanity

// SPDX-FileCopyrightText: 2025-2026 Neptuwunium
//
// SPDX-License-Identifier: EUPL-1.2

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstring>
#include <string>

#include "hero_look.hpp"

#include "ddl_visit.hpp"
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

namespace rivet_hook::hero_look {
	// HeroTypes::kKit. any type but Ratchet and Rivet leaves the base model
	// rendering, the way Clank and Kit are drawn, instead of an invisible deformer
	// under vanity parts
	constexpr int32_t SKIN_HERO_TYPE = 3;

	// ActorAsset: the object asset name and scene object HandleTransformationEvent
	// creates the new model from
	constexpr size_t ACTOR_ASSET_OBJECT_NAME = 0x38;
	constexpr size_t ACTOR_ASSET_SCENE_OBJECT = 0x40;
	// ModelInst: its Model
	constexpr size_t MODEL_INST_MODEL = 0x88;

	using load_actor_asset_t = Asset *(*)(void *manager, const char *path, Asset *loaded_from, const char *load_info);
	using lookup_actor_asset_t = const void *(*)(void *manager, uint64_t id);
	using create_scene_object_t = void *(*)(void *scene, uint32_t *out, const char *name, const void *scene_object, void *unused);
	using resolve_model_inst_t = void *(*)(const uint32_t *handle);
	using switch_model_t = void (*)(void *model_inst, void *model, bool deferred);
	using destroy_model_inst_t = void (*)(void *model_inst);
	using remove_all_skin_items_t = void (*)(void *skin_manager);
	using reinit_from_prius_t = bool (*)(void *component, const void *class_info, void *prius);
	using post_activate_t = void (*)(void *component, const void *asset);

	static load_actor_asset_t g_load_actor_asset = nullptr;
	static lookup_actor_asset_t g_lookup_actor_asset = nullptr;
	static create_scene_object_t g_create_scene_object = nullptr;
	static resolve_model_inst_t g_resolve_model_inst = nullptr;
	static switch_model_t g_switch_model = nullptr;
	static destroy_model_inst_t g_destroy_model_inst = nullptr;
	static remove_all_skin_items_t g_remove_all_skin_items = nullptr;
	static reinit_from_prius_t g_reinit_from_prius = nullptr;
	static post_activate_t g_post_activate = nullptr;
	static void *g_actor_assets = nullptr;
	static const void *g_skin_prius_vtable = nullptr;
	static const void *g_skin_class = nullptr;
	static bool g_ready = false;

	// HeroSkinManagerPrius as OnTransformationPostActivate builds it on its stack
	struct SkinPrius {
		const void *vtable;
		int32_t hero_type;
		int32_t unused;
	};

	// what the hero wears now, and what waits for its asset to load
	static std::string g_worn_path;
	static uint32_t g_worn_actor = 0;
	static std::string g_pending_path;
	static Asset *g_pending_asset = nullptr;
	static std::string g_last_error;

	// a restore's second half: the skin manager is rebuilt only once the deferred
	// switch has put the hero's own model back, or its parts bind to the old rig
	static uint32_t g_restore_actor = 0;
	static void *g_restore_model = nullptr;
	static int32_t g_restore_pumps = 0;
	// pumps to wait for the switch before rebuilding anyway
	constexpr int32_t RESTORE_MAX_PUMPS = 30;

	auto
	init() -> void {
		if (g_ready) {
			return;
		}

		const auto handle_event = find_address(TRANSFORMATION_HANDLE_EVENT_SIGNATURE);
		const auto post_activate = find_address(HERO_TRANSFORMATION_POST_ACTIVATE_SIGNATURE);
		g_load_actor_asset = reinterpret_cast<load_actor_asset_t>(find_address(LOAD_ACTOR_ASSET_SIGNATURE));
		g_remove_all_skin_items = reinterpret_cast<remove_all_skin_items_t>(find_address(SKIN_REMOVE_ALL_ITEMS_SIGNATURE));
		if (handle_event != 0) {
			g_actor_assets = load_rel_var(handle_event, TRANSFORMATION_ACTOR_ASSETS_ADDRESS);
			g_lookup_actor_asset = reinterpret_cast<lookup_actor_asset_t>(load_rel_var(handle_event, TRANSFORMATION_LOOKUP_ACTOR_ASSET_ADDRESS));
			g_resolve_model_inst = reinterpret_cast<resolve_model_inst_t>(load_rel_var(handle_event, TRANSFORMATION_RESOLVE_MODEL_INST_ADDRESS));
			g_create_scene_object = reinterpret_cast<create_scene_object_t>(load_rel_var(handle_event, TRANSFORMATION_CREATE_SCENE_OBJECT_ADDRESS));
			g_switch_model = reinterpret_cast<switch_model_t>(load_rel_var(handle_event, TRANSFORMATION_SWITCH_MODEL_ADDRESS));
			g_destroy_model_inst = reinterpret_cast<destroy_model_inst_t>(load_rel_var(handle_event, TRANSFORMATION_DESTROY_MODEL_INST_ADDRESS));
		}

		if (post_activate != 0) {
			g_post_activate = reinterpret_cast<post_activate_t>(post_activate);
			g_skin_prius_vtable = load_rel_var(post_activate, HERO_POST_ACTIVATE_PRIUS_VTABLE_ADDRESS);
			g_skin_class = load_rel_var(post_activate, HERO_POST_ACTIVATE_SKIN_CLASS_ADDRESS);
			g_reinit_from_prius = reinterpret_cast<reinit_from_prius_t>(load_rel_var(post_activate, HERO_POST_ACTIVATE_REINIT_ADDRESS));
		}

		g_ready = g_load_actor_asset != nullptr && g_remove_all_skin_items != nullptr && g_actor_assets != nullptr && g_lookup_actor_asset != nullptr
			&& g_resolve_model_inst != nullptr && g_create_scene_object != nullptr && g_switch_model != nullptr && g_destroy_model_inst != nullptr
			&& g_post_activate != nullptr && g_skin_prius_vtable != nullptr && g_skin_class != nullptr && g_reinit_from_prius != nullptr;
		if (g_ready) {
			g_output << "[hero_look] HandleTransformationEvent at " << reinterpret_cast<void *>(handle_event) << ", OnTransformationPostActivate at " << reinterpret_cast<void *>(post_activate) << "\n";
		} else {
			g_output << "[hero_look] the transformation calls were not found, rivet.hero_look is unavailable\n";
		}

		g_output.flush();
	}

	// ------------------------------------------------------------ engine calls --
	// each on its own, so a fault is caught with nothing to unwind

	static auto
	call_lookup(const uint64_t id, const void **out) -> bool {
#ifdef _MSC_VER
		__try {
#endif
			*out = g_lookup_actor_asset(g_actor_assets, id);
			return true;
#ifdef _MSC_VER
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			return false;
		}
#endif
	}

	static auto
	call_load(const char *path, Asset **out) -> bool {
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
	call_resolve(const uint32_t handle, void **out) -> bool {
#ifdef _MSC_VER
		__try {
#endif
			*out = g_resolve_model_inst(&handle);
			return true;
#ifdef _MSC_VER
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			return false;
		}
#endif
	}

	static auto
	call_remove_parts(void *skin_manager) -> bool {
#ifdef _MSC_VER
		__try {
#endif
			g_remove_all_skin_items(skin_manager);
			return true;
#ifdef _MSC_VER
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			return false;
		}
#endif
	}

	// HandleTransformationEvent's model swap: a scene object from the asset, its
	// model onto the hero's ModelInst (applied at the end of the frame), and the
	// scene object destroyed again. the model belongs to the loaded asset.
	static auto
	call_switch(void *model_inst, const void *actor_asset, void **switched) -> bool {
#ifdef _MSC_VER
		__try {
#endif
			const auto *bytes = static_cast<const uint8_t *>(actor_asset);
			const auto *name = *reinterpret_cast<const char *const *>(bytes + ACTOR_ASSET_OBJECT_NAME);
			const auto *scene_object = *reinterpret_cast<const void *const *>(bytes + ACTOR_ASSET_SCENE_OBJECT);

			uint32_t created = 0;
			g_create_scene_object(g_SceneManager, &created, name, scene_object, nullptr);
			auto *created_inst = g_resolve_model_inst(&created);
			*switched = nullptr;
			if (created_inst != nullptr) {
				auto *model = *reinterpret_cast<void **>(static_cast<uint8_t *>(created_inst) + MODEL_INST_MODEL);
				*switched = model;
				g_switch_model(model_inst, model, true);
				g_destroy_model_inst(created_inst);
			}

			return true;
#ifdef _MSC_VER
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			return false;
		}
#endif
	}

	static auto
	call_reinit_skin(void *skin_manager, const int32_t hero_type) -> bool {
#ifdef _MSC_VER
		__try {
#endif
			SkinPrius prius { g_skin_prius_vtable, hero_type, 0 };
			g_reinit_from_prius(skin_manager, g_skin_class, &prius);
			return true;
#ifdef _MSC_VER
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			return false;
		}
#endif
	}

	static auto
	call_post_activate(void *component, const void *actor_asset) -> bool {
#ifdef _MSC_VER
		__try {
#endif
			g_post_activate(component, actor_asset);
			return true;
#ifdef _MSC_VER
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			return false;
		}
#endif
	}

	// ------------------------------------------------------------------ hero --

	struct Hero {
		uint32_t handle;
		const Actor *actor;
		void *skin_manager;
		void *model_inst;
	};

	static auto
	fail(const char **reason, const char *why) -> bool {
		if (reason != nullptr) {
			*reason = why;
		}

		return false;
	}

	// the hero, its HeroSkinManager and the ModelInst it renders through
	static auto
	find_hero(Hero &out, const char **reason) -> bool {
		if (!g_ready) {
			return fail(reason, "the transformation calls were not found");
		}

		// the switch creates and destroys a scene object and the skin manager
		// loads and drops model parts
		if (!game_thread::on_game_thread()) {
			return fail(reason, "the hero's model can only change on the game thread, and it is not pumping (loading?)");
		}

		if (g_SceneManager == nullptr) {
			return fail(reason, "the scene manager is not available");
		}

		out.handle = scene_query::hero();
		if (out.handle == 0) {
			return fail(reason, "there is no hero right now");
		}

		EngineHandle engine_handle {};
		engine_handle.value = out.handle;
		out.actor = g_SceneManager->ResolveActor(engine_handle);
		if (out.actor == nullptr || out.actor->object == nullptr || !ddl::is_readable(out.actor->object, sizeof(SceneObject))) {
			return fail(reason, "the hero has no scene object");
		}

		if (!call_resolve(out.actor->object->sceneHandle.value, &out.model_inst) || out.model_inst == nullptr) {
			return fail(reason, "the hero's scene object is not a ModelInst");
		}

		out.skin_manager = nullptr;
		if (out.actor->components == nullptr || out.actor->componentCount <= 0 || !ddl::is_readable(out.actor->components, sizeof(ComponentPointer) * out.actor->componentCount)) {
			return fail(reason, "the hero has no readable component list");
		}

		for (auto index = 0; index < out.actor->componentCount && out.skin_manager == nullptr; ++index) {
			const auto [type, instance] = out.actor->components[index];
			if (type == nullptr || instance == nullptr || !ddl::is_readable(type, sizeof(ComponentInfo))) {
				continue;
			}

			char name[0x100];
			if (!ddl::read_string(type->name, name, sizeof(name)) || strcmp(name, "HeroSkinManager") != 0) {
				continue;
			}

			if (ddl::is_readable(instance, sizeof(Component)) && !instance->IsDestroyed()) {
				out.skin_manager = instance;
			}
		}

		if (out.skin_manager == nullptr) {
			return fail(reason, "the hero has no HeroSkinManager");
		}

		return true;
	}

	static auto
	loaded_actor_asset(const uint64_t id, const char **reason) -> const void * {
		const void *asset = nullptr;
		if (!call_lookup(id, &asset) || asset == nullptr) {
			fail(reason, "the actor asset is not loaded");
			return nullptr;
		}

		return asset;
	}

	// the engine's order: parts off, model switched, skin manager rebuilt
	static auto
	apply(const uint64_t id, const char **reason) -> bool {
		Hero hero {};
		if (!find_hero(hero, reason)) {
			return false;
		}

		const auto *target = loaded_actor_asset(id, reason);
		if (target == nullptr) {
			return false;
		}

		if (!call_remove_parts(hero.skin_manager)) {
			return fail(reason, "RemoveAllSkinItemsByPart faulted");
		}

		void *switched = nullptr;
		if (!call_switch(hero.model_inst, target, &switched)) {
			return fail(reason, "the model switch faulted");
		}

		if (switched == nullptr) {
			return fail(reason, "the actor asset's scene object is not a model");
		}

		// a skin manager of a type that draws its base model. its Init hands it the
		// hero's equipped vanity again, so the parts come off a second time
		if (!call_reinit_skin(hero.skin_manager, SKIN_HERO_TYPE) || !call_remove_parts(hero.skin_manager)) {
			return fail(reason, "rebuilding the skin manager faulted");
		}

		g_worn_actor = hero.handle;
		return true;
	}

	auto
	request(const char *path, const char **reason) -> Result {
		if (!g_ready) {
			fail(reason, "the transformation calls were not found");
			return Result::Failed;
		}

		if (path == nullptr || path[0] == '\0') {
			fail(reason, "no actor asset path");
			return Result::Failed;
		}

		Asset *asset = nullptr;
		if (!call_load(path, &asset) || asset == nullptr || !ddl::is_readable(asset, sizeof(Asset))) {
			fail(reason, "the actor asset could not be requested");
			return Result::Failed;
		}

		if (asset->status == AssetStatus::Error || asset->status == AssetStatus::Aborted) {
			fail(reason, "the actor asset failed to load");
			return Result::Failed;
		}

		if (asset->status != AssetStatus::Loaded) {
			g_pending_path = path;
			g_pending_asset = asset;
			return Result::Loading;
		}

		g_pending_path.clear();
		g_pending_asset = nullptr;
		if (!apply(asset->assetId, reason)) {
			return Result::Failed;
		}

		g_worn_path = path;
		return Result::Applied;
	}

	auto
	restore(const char **reason) -> bool {
		g_pending_path.clear();
		g_pending_asset = nullptr;

		Hero hero {};
		if (!find_hero(hero, reason)) {
			return false;
		}

		if (hero.actor->actorAsset == nullptr || !ddl::is_readable(hero.actor->actorAsset, sizeof(Asset))) {
			return fail(reason, "the hero has no actor asset");
		}

		const auto *own = loaded_actor_asset(hero.actor->actorAsset->assetId, reason);
		if (own == nullptr) {
			return false;
		}

		void *switched = nullptr;
		if (!call_remove_parts(hero.skin_manager) || !call_switch(hero.model_inst, own, &switched)) {
			return fail(reason, "the model switch faulted");
		}

		if (switched == nullptr) {
			return fail(reason, "the hero's own actor asset has no model");
		}

		// the switch lands at the end of the frame, the rebuild waits for it
		g_restore_actor = hero.handle;
		g_restore_model = switched;
		g_restore_pumps = 0;
		g_worn_path.clear();
		g_worn_actor = 0;
		return true;
	}

	// the skin manager for the type the hero really is, vanity parts and all,
	// once the hero's ModelInst holds its own model again
	static auto
	finish_restore() -> void {
		if (g_restore_model == nullptr || !game_thread::on_game_thread()) {
			return;
		}

		Hero hero {};
		const char *reason = nullptr;
		if (!find_hero(hero, &reason) || hero.handle != g_restore_actor) {
			// respawned or gone: a new hero is built with its own look
			g_restore_model = nullptr;
			return;
		}

		const auto *current = static_cast<uint8_t *>(hero.model_inst) + MODEL_INST_MODEL;
		const auto switched = ddl::is_readable(current, sizeof(void *)) && *reinterpret_cast<void *const *>(current) == g_restore_model;
		if (!switched && ++g_restore_pumps < RESTORE_MAX_PUMPS) {
			return;
		}

		g_restore_model = nullptr;
		if (!switched) {
			g_output << "[hero_look] the hero's own model did not come back in time, rebuilding the skin manager anyway\n";
		}

		const auto *own = loaded_actor_asset(hero.actor->actorAsset->assetId, &reason);
		if (own == nullptr || !call_post_activate(hero.skin_manager, own)) {
			g_last_error = own == nullptr ? reason : "OnTransformationPostActivate faulted";
			g_output << "[hero_look] restore: " << g_last_error << "\n";
		}

		g_output.flush();
	}

	auto
	pump() -> void {
		finish_restore();

		if (g_pending_asset == nullptr || !game_thread::on_game_thread()) {
			return;
		}

		if (!ddl::is_readable(g_pending_asset, sizeof(Asset))) {
			g_last_error = "the pending actor asset went away";
			g_pending_path.clear();
			g_pending_asset = nullptr;
			return;
		}

		const auto status = g_pending_asset->status;
		if (status < AssetStatus::Loaded) {
			return;
		}

		const auto path = g_pending_path;
		const auto id = g_pending_asset->assetId;
		g_pending_path.clear();
		g_pending_asset = nullptr;
		if (status != AssetStatus::Loaded) {
			g_last_error = "the actor asset failed to load";
			return;
		}

		const char *reason = nullptr;
		if (!apply(id, &reason)) {
			g_last_error = reason != nullptr ? reason : "refused";
			g_output << "[hero_look] " << path << ": " << g_last_error << "\n";
			g_output.flush();
			return;
		}

		g_worn_path = path;
		g_last_error.clear();
	}

	auto
	status() -> nlohmann::json {
		nlohmann::json result;
		result["available"] = g_ready;
		result["worn"] = g_worn_path.empty() ? nlohmann::json() : nlohmann::json(g_worn_path);
		result["worn_on"] = g_worn_actor;
		result["pending"] = g_pending_path.empty() ? nlohmann::json() : nlohmann::json(g_pending_path);
		result["restoring"] = g_restore_model != nullptr;
		result["last_error"] = g_last_error.empty() ? nlohmann::json() : nlohmann::json(g_last_error);
		return result;
	}
} // namespace rivet_hook::hero_look

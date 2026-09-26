// SPDX-FileCopyrightText: 2025-2026 Neptuwunium
//
// SPDX-License-Identifier: EUPL-1.2

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <string>

#include "hero_look.hpp"

#include "ddl_visit.hpp"
#include "events.hpp"
#include "game/scene_manager.hpp"
#include "game_thread.hpp"
#include "runtime.hpp"
#include "runtime_loader.hpp"
#include "settings.hpp"
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
	using scratch_t = void (*)(void *scratch);
	using create_prius_t = bool (*)(void *manager, void **prius, void **type_info, const void *class_info, const void *asset);
	using anim_set_at_t = const uint64_t *(*)(const void *prius, void *scratch, uint32_t index);
	using remove_anim_set_t = void (*)(void *controller, uint64_t id, bool flag, uint32_t unique_id);
	using push_anim_set_t = void (*)(void *controller, void *out, uint64_t id, uint32_t flags, uint32_t unused);
	using destroy_prius_t = void (*)(void *prius);

	// AnimControllerComponent: its anim controller, and the dirty flags the engine
	// sets before every change to it
	constexpr size_t ANIM_COMPONENT_CONTROLLER = 0xF8;
	constexpr size_t ANIM_COMPONENT_FLAGS = 0x74;
	constexpr uint32_t ANIM_COMPONENT_DIRTY = 0x4;
	// AnimControllerComponentPrius: the AnimSets count
	constexpr size_t ANIM_PRIUS_SET_COUNT = 0x10;
	// DDLStructTypeInfo: its Destroy
	constexpr size_t TYPE_INFO_DESTROY = 0xE0;
	constexpr int32_t MAX_ANIM_SETS = 64;

	using load_model_t = Asset *(*)(void *manager, const uint64_t *id, const void *loaded_from, const char *load_info);
	using release_asset_t = bool (*)(void *manager, Asset *asset);

	// AssetManagerBase: the asset handed out when a load cannot be made, for the
	// model manager the default cube
	constexpr size_t ASSET_MANAGER_DEFAULT_ASSET = 0x88;
	// models switched away from whose switch has not landed yet
	constexpr int32_t MAX_RETIRED_MODELS = 8;

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

	static const void *g_anim_class = nullptr;
	static scratch_t g_scratch_save = nullptr;
	static scratch_t g_scratch_restore = nullptr;
	static create_prius_t g_create_prius = nullptr;
	static anim_set_at_t g_anim_set_at = nullptr;
	static remove_anim_set_t g_remove_anim_set = nullptr;
	static push_anim_set_t g_push_anim_set = nullptr;
	static bool g_anims_ready = false;

	static void *g_model_manager = nullptr;
	static load_model_t g_load_model = nullptr;
	static release_asset_t g_release_asset = nullptr;
	static bool g_models_ready = false;

	// a .model look holds one reference to its model while it is worn, and lets it
	// go once the switch away from it has landed
	static Asset *g_held_model = nullptr;
	static Asset *g_retired_models[MAX_RETIRED_MODELS];
	static int32_t g_retired_count = 0;
	static bool g_pending_is_model = false;

	// HeroSkinManagerPrius as OnTransformationPostActivate builds it on its stack
	struct SkinPrius {
		const void *vtable;
		int32_t hero_type;
		int32_t unused;
	};

	// what the hero wears now, and what waits for its asset to load
	static std::string g_worn_path;
	static uint32_t g_worn_actor = 0;
	static bool g_worn_anims = false;

	// a respawn builds a new hero actor with its own look. the worn look goes back
	// on once the new hero has been there this many pumps
	static uint32_t g_respawned_actor = 0;
	static int32_t g_respawned_pumps = 0;
	constexpr int32_t RESPAWN_SETTLE_PUMPS = 60;
	static std::string g_pending_path;
	static Asset *g_pending_asset = nullptr;
	static std::string g_last_error;

	static bool g_pending_anims = false;

	// SwitchModel lands at the end of the frame. whatever binds to the new rig
	// waits for it: a restore's skin manager rebuild, whose parts would otherwise
	// bind to the old rig, and the target's anim sets
	enum class AfterSwitch {
		None,
		RebuildSkin,
		PushAnimSets,
	};

	static AfterSwitch g_after = AfterSwitch::None;
	static uint32_t g_after_actor = 0;
	static void *g_after_model = nullptr;
	static uint64_t g_after_asset = 0;
	static int32_t g_after_pumps = 0;
	// pumps to wait for the switch before going ahead anyway
	constexpr int32_t AFTER_SWITCH_MAX_PUMPS = 30;

	// the target's anim sets pushed onto the hero, removed again on restore
	static uint32_t g_pushed_actor = 0;
	static uint64_t g_pushed_sets[MAX_ANIM_SETS];
	static int32_t g_pushed_count = 0;

	// the hero actors HeroCharacterConfig lists, by HeroTypes
	constexpr const char *HERO_ACTORS[] = {
		"characters/hero/hero_ratchet_ps4/hero_ratchet_ps4.actor",
		"characters/hero/hero_clank_ps4/hero_clank_ps4.actor",
		"characters/hero/hero_ratchette_ps4/hero_ratchette_ps4.actor",
		"characters/hero/hero_Kit/hero_kit.actor",
	};
	constexpr const char *HERO_NAMES[] = { "ratchet", "clank", "rivet", "kit" };

	// TransformationManager virtuals: GetTransformationState, -1 before the first
	// transformation, and GetTransformationAssetId (manager, out, state) -> id *
	constexpr int32_t TRANSFORM_VTABLE_GET_STATE = 10;
	constexpr int32_t TRANSFORM_VTABLE_GET_ASSET_ID = 11;

	// a play as whose hero actor asset is still loading
	static int32_t g_pending_play_as = -1;
	static Asset *g_pending_play_as_asset = nullptr;

	// the remembered look goes on with the first hero, once, after a launch
	static bool g_launch_checked = false;

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
		if (const auto finalize = find_address(TRANSFORMATION_FINALIZE_SIGNATURE); finalize != 0) {
			g_anim_class = load_rel_var(finalize, FINALIZE_ANIM_CLASS_ADDRESS);
			g_scratch_save = reinterpret_cast<scratch_t>(load_rel_var(finalize, FINALIZE_SCRATCH_SAVE_ADDRESS));
			g_scratch_restore = reinterpret_cast<scratch_t>(load_rel_var(finalize, FINALIZE_SCRATCH_RESTORE_ADDRESS));
			g_create_prius = reinterpret_cast<create_prius_t>(load_rel_var(finalize, FINALIZE_CREATE_PRIUS_ADDRESS));
			g_anim_set_at = reinterpret_cast<anim_set_at_t>(load_rel_var(finalize, FINALIZE_ANIM_SET_AT_ADDRESS));
			g_remove_anim_set = reinterpret_cast<remove_anim_set_t>(load_rel_var(finalize, FINALIZE_REMOVE_ANIM_SET_ADDRESS));
			g_push_anim_set = reinterpret_cast<push_anim_set_t>(load_rel_var(finalize, FINALIZE_PUSH_ANIM_SET_ADDRESS));
			g_anims_ready = g_anim_class != nullptr && g_scratch_save != nullptr && g_scratch_restore != nullptr && g_create_prius != nullptr
				&& g_anim_set_at != nullptr && g_remove_anim_set != nullptr && g_push_anim_set != nullptr;
		}

		if (const auto load_site = find_address(MODEL_MANAGER_LOAD_SIGNATURE); load_site != 0) {
			g_model_manager = load_rel_var(load_site, MODEL_MANAGER_ADDRESS);
			g_load_model = reinterpret_cast<load_model_t>(load_rel_var(load_site, MODEL_MANAGER_LOAD_ADDRESS));
		}

		g_release_asset = reinterpret_cast<release_asset_t>(find_address(ASSET_MANAGER_RELEASE_SIGNATURE));
		g_models_ready = g_model_manager != nullptr && g_load_model != nullptr && g_release_asset != nullptr;
		if (g_ready && !g_models_ready) {
			g_output << "[hero_look] the model manager calls were not found, only .actor looks can be worn\n";
		}

		if (g_ready && !g_anims_ready) {
			g_output << "[hero_look] the anim set calls were not found, looks are put on without their anim sets\n";
		}

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

	// ModelManager::LoadModel. adds a reference the caller releases
	static auto
	call_load_model(const uint64_t id, Asset **out) -> bool {
#ifdef _MSC_VER
		__try {
#endif
			*out = g_load_model(g_model_manager, &id, nullptr, nullptr);
			return true;
#ifdef _MSC_VER
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			return false;
		}
#endif
	}

	static auto
	call_release(Asset *asset) -> bool {
#ifdef _MSC_VER
		__try {
#endif
			g_release_asset(g_model_manager, asset);
			return true;
#ifdef _MSC_VER
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			return false;
		}
#endif
	}

	// a loaded model straight onto the hero's ModelInst, as
	// Cinematic2Component::ChangeHeroInEditor does
	static auto
	call_switch_to_model(void *model_inst, Asset *model) -> bool {
#ifdef _MSC_VER
		__try {
#endif
			g_switch_model(model_inst, model, true);
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

	// the AnimSets an actor asset's AnimControllerComponentPrius lists, built in
	// scratch memory the way FinalizeTransformation builds it. false when the
	// asset has no such prius or the read faulted.
	static auto
	call_read_anim_sets(void *manager, const void *actor_asset, uint64_t *out, const int32_t max, int32_t *count) -> bool {
		*count = 0;
		uint8_t scratch[0x10];
		g_scratch_save(scratch);
		bool found = false;
#ifdef _MSC_VER
		__try {
#endif
			void *prius = nullptr;
			void *type_info = nullptr;
			if (g_create_prius(manager, &prius, &type_info, g_anim_class, actor_asset) && prius != nullptr) {
				found = true;
				const auto sets = *reinterpret_cast<const uint32_t *>(static_cast<uint8_t *>(prius) + ANIM_PRIUS_SET_COUNT);
				for (uint32_t index = 0; index < sets && *count < max; ++index) {
					uint8_t id_scratch[0x10];
					if (const auto *id = g_anim_set_at(prius, id_scratch, index); id != nullptr && *id != 0) {
						out[(*count)++] = *id;
					}
				}

				if (type_info != nullptr) {
					(*reinterpret_cast<destroy_prius_t *>(static_cast<uint8_t *>(type_info) + TYPE_INFO_DESTROY))(prius);
				}
			}
#ifdef _MSC_VER
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			found = false;
		}
#endif
		g_scratch_restore(scratch);
		return found;
	}

	// pushes in reverse, as FinalizeTransformation does, so the first listed set
	// ends up on top
	static auto
	call_push_anim_sets(void *anim, const uint64_t *ids, const int32_t count) -> bool {
#ifdef _MSC_VER
		__try {
#endif
			auto *bytes = static_cast<uint8_t *>(anim);
			for (auto index = count - 1; index >= 0; --index) {
				uint8_t out[0x40] {};
				*reinterpret_cast<uint32_t *>(bytes + ANIM_COMPONENT_FLAGS) |= ANIM_COMPONENT_DIRTY;
				g_push_anim_set(bytes + ANIM_COMPONENT_CONTROLLER, out, ids[index], 0, 0);
			}

			return true;
#ifdef _MSC_VER
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			return false;
		}
#endif
	}

	static auto
	call_remove_anim_sets(void *anim, const uint64_t *ids, const int32_t count) -> bool {
#ifdef _MSC_VER
		__try {
#endif
			auto *bytes = static_cast<uint8_t *>(anim);
			for (auto index = 0; index < count; ++index) {
				*reinterpret_cast<uint32_t *>(bytes + ANIM_COMPONENT_FLAGS) |= ANIM_COMPONENT_DIRTY;
				g_remove_anim_set(bytes + ANIM_COMPONENT_CONTROLLER, ids[index], true, 0);
			}

			return true;
#ifdef _MSC_VER
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			return false;
		}
#endif
	}

	// the hero's transformation state and the actor asset that state stands for,
	// asked of the manager itself. state -1 and asset 0 before the first
	// transformation
	static auto
	call_transform_state(void *transform, int32_t *state, uint64_t *asset) -> bool {
#ifdef _MSC_VER
		__try {
#endif
			using get_state_t = int32_t (*)(void *manager);
			using get_asset_t = const uint64_t *(*)(void *manager, uint64_t *out, int32_t state);
			auto *const *vtable = *static_cast<void *const *const *>(transform);
			*state = reinterpret_cast<get_state_t>(vtable[TRANSFORM_VTABLE_GET_STATE])(transform);
			*asset = 0;
			if (*state >= 0) {
				uint64_t out = 0;
				const auto *id = reinterpret_cast<get_asset_t>(vtable[TRANSFORM_VTABLE_GET_ASSET_ID])(transform, &out, *state);
				*asset = id != nullptr ? *id : 0;
			}

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
		// the AnimControllerComponent, null when the hero has none
		void *anim;
		// the HeroTransformationManager, null when the hero has none
		void *transform;
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
		out.anim = nullptr;
		out.transform = nullptr;
		if (out.actor->components == nullptr || out.actor->componentCount <= 0 || !ddl::is_readable(out.actor->components, sizeof(ComponentPointer) * out.actor->componentCount)) {
			return fail(reason, "the hero has no readable component list");
		}

		for (auto index = 0; index < out.actor->componentCount; ++index) {
			const auto [type, instance] = out.actor->components[index];
			if (type == nullptr || instance == nullptr || !ddl::is_readable(type, sizeof(ComponentInfo))) {
				continue;
			}

			char name[0x100];
			if (!ddl::read_string(type->name, name, sizeof(name)) || !ddl::is_readable(instance, sizeof(Component)) || instance->IsDestroyed()) {
				continue;
			}

			if (strcmp(name, "HeroSkinManager") == 0) {
				out.skin_manager = instance;
			} else if (strcmp(name, "AnimControllerComponent") == 0) {
				out.anim = instance;
			} else if (strcmp(name, "HeroTransformationManager") == 0) {
				out.transform = instance;
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

	// the hero's transformation state, -1 when it never transformed or has no
	// transformation manager
	static auto
	transform_state(const Hero &hero, uint64_t *asset = nullptr) -> int32_t {
		int32_t state = -1;
		uint64_t id = 0;
		if (hero.transform == nullptr || !call_transform_state(hero.transform, &state, &id)) {
			state = -1;
			id = 0;
		}

		if (asset != nullptr) {
			*asset = id;
		}

		return state;
	}

	// the actor asset of the hero it is playing as now: the one its last
	// transformation put on, else the one it spawned from. 0 when there is none
	static auto
	own_asset_id(const Hero &hero) -> uint64_t {
		uint64_t transformed = 0;
		if (transform_state(hero, &transformed) >= 0 && transformed != 0) {
			return transformed;
		}

		if (hero.actor->actorAsset == nullptr || !ddl::is_readable(hero.actor->actorAsset, sizeof(Asset))) {
			return 0;
		}

		return hero.actor->actorAsset->assetId;
	}

	// what to do once the switch to model has landed on the hero. every switch
	// waits, so the models switched away from are only let go once the hero no
	// longer draws them
	static auto
	after_switch(const AfterSwitch action, const uint32_t actor, void *model, const uint64_t asset) -> void {
		g_after = action;
		g_after_actor = actor;
		g_after_model = model;
		g_after_asset = asset;
		g_after_pumps = 0;
	}

	// the worn .model, let go once the switch away from it has landed
	static auto
	retire_held_model() -> void {
		if (g_held_model == nullptr) {
			return;
		}

		if (g_retired_count < MAX_RETIRED_MODELS) {
			g_retired_models[g_retired_count++] = g_held_model;
		} else {
			// never released rather than released while it may still be drawn
			g_output << "[hero_look] too many model switches in flight, one model stays loaded\n";
			g_output.flush();
		}

		g_held_model = nullptr;
	}

	static auto
	release_retired_models() -> void {
		for (auto index = 0; index < g_retired_count; ++index) {
			call_release(g_retired_models[index]);
		}

		g_retired_count = 0;
	}

	// forgets the pending look, letting go of the reference a pending .model holds
	static auto
	drop_pending() -> void {
		if (g_pending_is_model && g_pending_asset != nullptr) {
			call_release(g_pending_asset);
		}

		g_pending_path.clear();
		g_pending_asset = nullptr;
		g_pending_is_model = false;
		g_pending_anims = false;
	}

	// the last look put on, kept in rivet.toml for the next launch. only written
	// when it changes, since a respawn puts the same look on again
	static auto
	remember(const std::string &path, const bool anims) -> void {
		auto &saved = g_settings.hero_look;
		if (saved.path == path && saved.anims == anims) {
			return;
		}

		saved.path = path;
		saved.anims = anims;
		g_settings.save();
	}

	static auto
	is_model_path(const char *path) -> bool {
		constexpr char EXTENSION[] = ".model";
		const auto length = strlen(path);
		return length > sizeof(EXTENSION) - 1 && _stricmp(path + length - (sizeof(EXTENSION) - 1), EXTENSION) == 0;
	}

	// takes the anim sets a look pushed back off, while the hero is the same actor
	static auto
	drop_pushed_anim_sets(const Hero &hero) -> void {
		if (g_pushed_count > 0 && g_pushed_actor == hero.handle && hero.anim != nullptr) {
			if (!call_remove_anim_sets(hero.anim, g_pushed_sets, g_pushed_count)) {
				g_output << "[hero_look] removing the pushed anim sets faulted\n";
				g_output.flush();
			}
		}

		g_pushed_count = 0;
		g_pushed_actor = 0;
	}

	// the target's anim sets on top of the hero's, leaving out the ones the hero's
	// own asset lists, so taking them off again never strips the hero's own
	static auto
	push_anim_sets(const Hero &hero, const uint64_t asset_id) -> void {
		const char *reason = nullptr;
		const auto *target = loaded_actor_asset(asset_id, &reason);
		if (target == nullptr || hero.anim == nullptr) {
			g_last_error = target == nullptr ? reason : "the hero has no AnimControllerComponent";
			return;
		}

		uint64_t sets[MAX_ANIM_SETS];
		int32_t count = 0;
		if (!call_read_anim_sets(hero.skin_manager, target, sets, MAX_ANIM_SETS, &count) || count == 0) {
			g_last_error = "the actor asset has no anim sets";
			return;
		}

		uint64_t own[MAX_ANIM_SETS];
		int32_t own_count = 0;
		if (const auto *own_asset = loaded_actor_asset(own_asset_id(hero), &reason); own_asset != nullptr) {
			call_read_anim_sets(hero.skin_manager, own_asset, own, MAX_ANIM_SETS, &own_count);
		}

		int32_t kept = 0;
		for (auto index = 0; index < count; ++index) {
			auto shared = false;
			for (auto other = 0; other < own_count && !shared; ++other) {
				shared = sets[index] == own[other];
			}

			if (!shared) {
				sets[kept++] = sets[index];
			}
		}

		if (!call_push_anim_sets(hero.anim, sets, kept)) {
			g_last_error = "pushing the anim sets faulted";
			return;
		}

		memcpy(g_pushed_sets, sets, sizeof(uint64_t) * kept);
		g_pushed_count = kept;
		g_pushed_actor = hero.handle;
		g_output << "[hero_look] pushed " << kept << " of " << count << " anim sets\n";
		g_output.flush();
	}

	// the engine's order: parts off, model switched, skin manager rebuilt. the
	// look is the actor asset id, or a loaded model whose reference this takes
	// over when it succeeds
	static auto
	apply(const uint64_t id, Asset *model, const bool anims, const char **reason) -> bool {
		Hero hero {};
		if (!find_hero(hero, reason)) {
			return false;
		}

		const void *target = nullptr;
		if (model == nullptr) {
			target = loaded_actor_asset(id, reason);
			if (target == nullptr) {
				return false;
			}
		}

		if (anims && (!g_anims_ready || hero.anim == nullptr)) {
			return fail(reason, g_anims_ready ? "the hero has no AnimControllerComponent" : "the anim set calls were not found");
		}

		// a previous look's sets come off before another goes on
		drop_pushed_anim_sets(hero);
		if (!call_remove_parts(hero.skin_manager)) {
			return fail(reason, "RemoveAllSkinItemsByPart faulted");
		}

		void *switched = nullptr;
		if (model != nullptr) {
			if (!call_switch_to_model(hero.model_inst, model)) {
				return fail(reason, "the model switch faulted");
			}

			switched = model;
		} else {
			if (!call_switch(hero.model_inst, target, &switched)) {
				return fail(reason, "the model switch faulted");
			}

			if (switched == nullptr) {
				return fail(reason, "the actor asset's scene object is not a model");
			}
		}

		// a skin manager of a type that draws its base model. its Init hands it the
		// hero's equipped vanity again, so the parts come off a second time
		if (!call_reinit_skin(hero.skin_manager, SKIN_HERO_TYPE) || !call_remove_parts(hero.skin_manager)) {
			return fail(reason, "rebuilding the skin manager faulted");
		}

		retire_held_model();
		g_held_model = model;
		after_switch(anims ? AfterSwitch::PushAnimSets : AfterSwitch::None, hero.handle, switched, id);
		g_worn_actor = hero.handle;
		return true;
	}

	// a .model look: loaded through the model manager, one reference held
	static auto
	request_model(const char *path, const char **reason) -> Result {
		if (!g_models_ready) {
			fail(reason, "the model manager calls were not found, only .actor looks can be worn");
			return Result::Failed;
		}

		uint64_t id = 0;
		if (!AssetLoader::asset_id(path, id)) {
			fail(reason, "that is not an asset path");
			return Result::Failed;
		}

		Asset *model = nullptr;
		if (!call_load_model(id, &model) || model == nullptr || !ddl::is_readable(model, sizeof(Asset))) {
			fail(reason, "the model could not be requested");
			return Result::Failed;
		}

		// the default cube comes back when there is nothing to load. it is not
		// reference counted, so there is nothing to release
		if (model == *reinterpret_cast<Asset *const *>(static_cast<uint8_t *>(g_model_manager) + ASSET_MANAGER_DEFAULT_ASSET)) {
			fail(reason, "there is no model at that path, in the game or in a mod folder");
			return Result::Failed;
		}

		if (model->status == AssetStatus::Error || model->status == AssetStatus::Aborted) {
			call_release(model);
			fail(reason, "the model failed to load");
			return Result::Failed;
		}

		drop_pending();
		if (model->status != AssetStatus::Loaded) {
			g_pending_path = path;
			g_pending_asset = model;
			g_pending_is_model = true;
			return Result::Loading;
		}

		if (!apply(id, model, false, reason)) {
			call_release(model);
			return Result::Failed;
		}

		g_worn_path = path;
		g_worn_anims = false;
		remember(path, false);
		return Result::Applied;
	}

	auto
	request(const char *path, const bool anims, const char **reason) -> Result {
		if (!g_ready) {
			fail(reason, "the transformation calls were not found");
			return Result::Failed;
		}

		if (path == nullptr || path[0] == '\0') {
			fail(reason, "no asset path");
			return Result::Failed;
		}

		if (is_model_path(path)) {
			if (anims) {
				fail(reason, "a .model has no anim sets of its own, its .actor has them");
				return Result::Failed;
			}

			return request_model(path, reason);
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

		drop_pending();
		if (asset->status != AssetStatus::Loaded) {
			g_pending_path = path;
			g_pending_asset = asset;
			g_pending_anims = anims;
			return Result::Loading;
		}

		if (!apply(asset->assetId, nullptr, anims, reason)) {
			return Result::Failed;
		}

		g_worn_path = path;
		g_worn_anims = anims;
		remember(path, anims);
		return Result::Applied;
	}

	auto
	restore(const char **reason) -> bool {
		drop_pending();

		Hero hero {};
		if (!find_hero(hero, reason)) {
			return false;
		}

		const auto own_id = own_asset_id(hero);
		if (own_id == 0) {
			return fail(reason, "the hero has no actor asset");
		}

		const auto *own = loaded_actor_asset(own_id, reason);
		if (own == nullptr) {
			return false;
		}

		drop_pushed_anim_sets(hero);

		void *switched = nullptr;
		if (!call_remove_parts(hero.skin_manager) || !call_switch(hero.model_inst, own, &switched)) {
			return fail(reason, "the model switch faulted");
		}

		if (switched == nullptr) {
			return fail(reason, "the hero's own actor asset has no model");
		}

		// the switch lands at the end of the frame, the rebuild waits for it
		retire_held_model();
		after_switch(AfterSwitch::RebuildSkin, hero.handle, switched, own_id);
		g_worn_path.clear();
		g_worn_actor = 0;
		g_respawned_actor = 0;
		remember("", false);
		return true;
	}

	auto
	hero_type(const char *name) -> int32_t {
		if (name == nullptr) {
			return -1;
		}

		for (auto type = 0; type < static_cast<int32_t>(std::size(HERO_NAMES)); ++type) {
			if (_stricmp(name, HERO_NAMES[type]) == 0) {
				return type;
			}
		}

		// the HeroTypes spellings too
		constexpr struct {
			const char *name;
			int32_t type;
		} ALIASES[] = { { "kratchet", 0 }, { "kclank", 1 }, { "kratchette", 2 }, { "ratchette", 2 }, { "krivet", 2 }, { "kkit", 3 } };
		for (const auto &[alias, type] : ALIASES) {
			if (_stricmp(name, alias) == 0) {
				return type;
			}
		}

		return -1;
	}

	// a look the transformation is about to replace: its anim sets off, its model
	// let go once the hero no longer draws it, and nothing left to put back on a
	// respawn. the remembered look stays for the next launch
	static auto
	forget_look(const Hero &hero) -> void {
		drop_pushed_anim_sets(hero);
		drop_pending();
		retire_held_model();
		g_after = AfterSwitch::None;
		g_after_model = nullptr;
		g_worn_path.clear();
		g_worn_actor = 0;
		g_respawned_actor = 0;
	}

	auto
	play_as(const int32_t type, const char **reason) -> Result {
		if (type < 0 || type >= static_cast<int32_t>(std::size(HERO_ACTORS))) {
			fail(reason, "the hero is ratchet, clank, rivet or kit");
			return Result::Failed;
		}

		if (!g_ready) {
			fail(reason, "the transformation calls were not found");
			return Result::Failed;
		}

		Hero hero {};
		if (!find_hero(hero, reason)) {
			return Result::Failed;
		}

		if (hero.transform == nullptr) {
			fail(reason, "the hero has no HeroTransformationManager");
			return Result::Failed;
		}

		uint64_t target_id = 0;
		AssetLoader::asset_id(HERO_ACTORS[type], target_id);
		const auto state = transform_state(hero);
		if (state == type || (state < 0 && own_asset_id(hero) == target_id)) {
			fail(reason, "already playing as that hero");
			return Result::Failed;
		}

		// the transformation only finds a loaded actor asset, and skips silently
		// when it is not
		Asset *asset = nullptr;
		if (!call_load(HERO_ACTORS[type], &asset) || asset == nullptr || !ddl::is_readable(asset, sizeof(Asset))) {
			fail(reason, "the hero's actor asset could not be requested");
			return Result::Failed;
		}

		if (asset->status == AssetStatus::Error || asset->status == AssetStatus::Aborted) {
			fail(reason, "the hero's actor asset failed to load");
			return Result::Failed;
		}

		if (asset->status != AssetStatus::Loaded) {
			g_pending_play_as = type;
			g_pending_play_as_asset = asset;
			return Result::Loading;
		}

		g_pending_play_as = -1;
		g_pending_play_as_asset = nullptr;

		const auto *info = events::find_class("TransformationEvent");
		if (info == nullptr) {
			fail(reason, events::ready() ? "TransformationEvent is not registered" : "the event system is not ready");
			return Result::Failed;
		}

		// the handler listens for its own actor as the sender, the way
		// TriggerHeroSwap sends it, not as a target
		events::Request request;
		request.sender = hero.handle;
		request.broadcast = true;

		forget_look(hero);
		auto *event = events::queue(info, request, reason);
		if (event == nullptr) {
			return Result::Failed;
		}

		ddl::Value value {};
		value.kind = ddl::ValueKind::Signed;
		value.as_signed = type;
		if (!events::set_field(info, event, "TransformationState", value, reason)) {
			return Result::Failed;
		}

		return Result::Applied;
	}

	auto
	set_apply_on_launch(const bool on) -> void {
		if (g_settings.hero_look.apply_on_launch != on) {
			g_settings.hero_look.apply_on_launch = on;
			g_settings.save();
		}
	}

	// the remembered look, once, the first time the pump runs after a launch:
	// handed to the respawn re-apply, which puts it on when the first hero has
	// settled
	static auto
	check_launch() -> void {
		if (g_launch_checked) {
			return;
		}

		g_launch_checked = true;
		const auto &saved = g_settings.hero_look;
		if (!saved.apply_on_launch || saved.path.empty() || !g_worn_path.empty()) {
			return;
		}

		g_worn_path = saved.path;
		g_worn_anims = saved.anims;
		g_worn_actor = 0;
		g_output << "[hero_look] putting " << saved.path << " back on once the hero is here\n";
		g_output.flush();
	}

	// runs once the hero's ModelInst holds the model it was switched to: lets go
	// of the models switched away from, then the pending step, the skin manager
	// for the type the hero really is (vanity parts and all) or the target's anim
	// sets
	static auto
	run_after_switch() -> void {
		if (!game_thread::on_game_thread()) {
			return;
		}

		// models retired by a switch the hook did not make (a play as) go once the
		// hero no longer draws them
		if (g_after_model == nullptr && g_retired_count > 0) {
			Hero hero {};
			const char *reason = nullptr;
			if (!find_hero(hero, &reason)) {
				release_retired_models();
				return;
			}

			const auto *current = static_cast<uint8_t *>(hero.model_inst) + MODEL_INST_MODEL;
			const auto *model = ddl::is_readable(current, sizeof(void *)) ? *reinterpret_cast<void *const *>(current) : nullptr;
			for (auto index = 0; index < g_retired_count; ++index) {
				if (g_retired_models[index] == model) {
					return;
				}
			}

			release_retired_models();
			return;
		}

		if (g_after_model == nullptr) {
			return;
		}

		Hero hero {};
		const char *reason = nullptr;
		if (!find_hero(hero, &reason) || hero.handle != g_after_actor) {
			// respawned or gone: a new hero is built with its own look, and the old
			// one no longer draws anything
			release_retired_models();
			g_after = AfterSwitch::None;
			g_after_model = nullptr;
			return;
		}

		const auto *current = static_cast<uint8_t *>(hero.model_inst) + MODEL_INST_MODEL;
		const auto switched = ddl::is_readable(current, sizeof(void *)) && *reinterpret_cast<void *const *>(current) == g_after_model;
		if (!switched && ++g_after_pumps < AFTER_SWITCH_MAX_PUMPS) {
			return;
		}

		const auto action = g_after;
		g_after = AfterSwitch::None;
		g_after_model = nullptr;
		if (!switched) {
			g_output << "[hero_look] the switched model did not land in time, going ahead anyway\n";
		}

		release_retired_models();
		if (action == AfterSwitch::PushAnimSets) {
			push_anim_sets(hero, g_after_asset);
		} else if (action == AfterSwitch::RebuildSkin) {
			const auto *own = loaded_actor_asset(g_after_asset, &reason);
			if (own == nullptr || !call_post_activate(hero.skin_manager, own)) {
				g_last_error = own == nullptr ? reason : "OnTransformationPostActivate faulted";
				g_output << "[hero_look] restore: " << g_last_error << "\n";
			}
		}

		g_output.flush();
	}

	// puts the worn look back on a hero that respawned, once it has settled
	static auto
	reapply_after_respawn() -> void {
		if (g_worn_path.empty() || g_pending_asset != nullptr || g_after_model != nullptr || !game_thread::on_game_thread()) {
			return;
		}

		const auto hero = scene_query::hero();
		if (hero == 0 || hero == g_worn_actor) {
			g_respawned_actor = 0;
			return;
		}

		if (hero != g_respawned_actor) {
			g_respawned_actor = hero;
			g_respawned_pumps = 0;
			return;
		}

		if (++g_respawned_pumps < RESPAWN_SETTLE_PUMPS) {
			return;
		}

		g_respawned_actor = 0;
		const auto path = g_worn_path;
		const char *reason = nullptr;
		const auto result = request(path.c_str(), g_worn_anims, &reason);
		if (result == Result::Failed) {
			// no retry loop on a hero it cannot be put on
			g_last_error = reason != nullptr ? reason : "refused";
			g_worn_path.clear();
			g_worn_actor = 0;
		}

		g_output << "[hero_look] the hero respawned, " << path << ": " << (result == Result::Failed ? g_last_error.c_str() : result == Result::Applied ? "put back on" : "loading") << "\n";
		g_output.flush();
	}

	auto
	pump() -> void {
		if (!game_thread::on_game_thread()) {
			return;
		}

		check_launch();
		run_after_switch();
		reapply_after_respawn();

		if (g_pending_play_as >= 0) {
			if (!ddl::is_readable(g_pending_play_as_asset, sizeof(Asset))) {
				g_pending_play_as = -1;
				g_pending_play_as_asset = nullptr;
				g_last_error = "the hero's actor asset went away";
			} else if (g_pending_play_as_asset->status >= AssetStatus::Loaded) {
				const auto type = g_pending_play_as;
				g_pending_play_as = -1;
				g_pending_play_as_asset = nullptr;
				const char *reason = nullptr;
				if (play_as(type, &reason) == Result::Failed) {
					g_last_error = reason != nullptr ? reason : "refused";
					g_output << "[hero_look] play as " << HERO_NAMES[type] << ": " << g_last_error << "\n";
					g_output.flush();
				}
			}
		}

		if (g_pending_asset == nullptr || !game_thread::on_game_thread()) {
			return;
		}

		if (!ddl::is_readable(g_pending_asset, sizeof(Asset))) {
			g_last_error = "the pending asset went away";
			g_pending_path.clear();
			g_pending_asset = nullptr;
			g_pending_is_model = false;
			return;
		}

		const auto status = g_pending_asset->status;
		if (status < AssetStatus::Loaded) {
			return;
		}

		const auto path = g_pending_path;
		auto *asset = g_pending_asset;
		const auto id = asset->assetId;
		const auto anims = g_pending_anims;
		const auto is_model = g_pending_is_model;
		g_pending_path.clear();
		g_pending_asset = nullptr;
		g_pending_is_model = false;
		if (status != AssetStatus::Loaded) {
			if (is_model) {
				call_release(asset);
			}

			g_last_error = is_model ? "the model failed to load" : "the actor asset failed to load";
			return;
		}

		const char *reason = nullptr;
		if (!apply(id, is_model ? asset : nullptr, anims, &reason)) {
			if (is_model) {
				call_release(asset);
			}

			g_last_error = reason != nullptr ? reason : "refused";
			g_output << "[hero_look] " << path << ": " << g_last_error << "\n";
			g_output.flush();
			return;
		}

		g_worn_path = path;
		g_worn_anims = anims;
		remember(path, anims);
		g_last_error.clear();
	}

	auto
	status() -> nlohmann::json {
		nlohmann::json result;
		result["available"] = g_ready;
		result["models_available"] = g_models_ready;
		result["worn"] = g_worn_path.empty() ? nlohmann::json() : nlohmann::json(g_worn_path);
		result["worn_on"] = g_worn_actor;
		result["pending"] = g_pending_path.empty() ? nlohmann::json() : nlohmann::json(g_pending_path);
		result["restoring"] = g_after == AfterSwitch::RebuildSkin;
		result["anim_sets_pushed"] = g_pushed_count;
		result["models_held"] = (g_held_model != nullptr ? 1 : 0) + g_retired_count;
		result["remembered"] = g_settings.hero_look.path.empty() ? nlohmann::json() : nlohmann::json(g_settings.hero_look.path);
		result["remembered_anims"] = g_settings.hero_look.anims;
		result["apply_on_launch"] = g_settings.hero_look.apply_on_launch;
		result["play_as_pending"] = g_pending_play_as >= 0 ? nlohmann::json(HERO_NAMES[g_pending_play_as]) : nlohmann::json();

		// which hero it plays as now, when the hero is here to ask
		Hero hero {};
		const char *reason = nullptr;
		if (find_hero(hero, &reason)) {
			const auto state = transform_state(hero);
			result["transformation_state"] = state;
			result["playing_as"] = state >= 0 && state < static_cast<int32_t>(std::size(HERO_NAMES)) ? nlohmann::json(HERO_NAMES[state]) : nlohmann::json("spawned");
		}
		result["last_error"] = g_last_error.empty() ? nlohmann::json() : nlohmann::json(g_last_error);
		return result;
	}
	// whole bodies of the game's own on the gameplay skeleton
	struct GameModel {
		const char *path;
		const char *name;
	};

	constexpr GameModel GAME_MODELS[] = {
		{ "characters/hero/hero_rivet/hero_rivet.model", "Rivet" },
		{ "characters/hero/hero_rivet/hero_rivet_beginning.model", "Rivet (beginning)" },
		{ "characters/hero/hero_rivet/hero_rivet_flashback.model", "Rivet (flashback)" },
		{ "characters/hero/hero_ratchet/hero_ratchet.model", "Ratchet" },
	};

	static auto
	contains(const std::string_view text, const std::string_view part) -> bool {
		if (part.empty()) {
			return true;
		}

		const auto found = std::ranges::search(text, part, [](const char a, const char b) {
			return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
		});
		return !found.empty();
	}

	auto
	models(const char *filter) -> nlohmann::json {
		const std::string_view part = filter != nullptr ? filter : "";
		auto mods = nlohmann::json::array();
		for (const auto &[path, mod] : AssetLoader::mod_models()) {
			if (contains(path, part) || contains(mod, part)) {
				mods.push_back({ { "path", path }, { "mod", mod } });
			}
		}

		auto game = nlohmann::json::array();
		for (const auto &[path, name] : GAME_MODELS) {
			if (contains(path, part) || contains(name, part)) {
				game.push_back({ { "path", path }, { "name", name } });
			}
		}

		nlohmann::json result;
		result["mods"] = std::move(mods);
		result["game"] = std::move(game);
		return result;
	}
} // namespace rivet_hook::hero_look

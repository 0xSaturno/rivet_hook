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

	// ------------------------------------------------------------------ hero --

	struct Hero {
		uint32_t handle;
		const Actor *actor;
		void *skin_manager;
		// the AnimControllerComponent, null when the hero has none
		void *anim;
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

	// what to do once the switch to model has landed on the hero
	static auto
	after_switch(const AfterSwitch action, const uint32_t actor, void *model, const uint64_t asset) -> void {
		g_after = action;
		g_after_actor = actor;
		g_after_model = model;
		g_after_asset = asset;
		g_after_pumps = 0;
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
		if (const auto *own_asset = loaded_actor_asset(hero.actor->actorAsset->assetId, &reason); own_asset != nullptr) {
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

	// the engine's order: parts off, model switched, skin manager rebuilt
	static auto
	apply(const uint64_t id, const bool anims, const char **reason) -> bool {
		Hero hero {};
		if (!find_hero(hero, reason)) {
			return false;
		}

		const auto *target = loaded_actor_asset(id, reason);
		if (target == nullptr) {
			return false;
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

		after_switch(anims ? AfterSwitch::PushAnimSets : AfterSwitch::None, hero.handle, switched, id);
		g_worn_actor = hero.handle;
		return true;
	}

	auto
	request(const char *path, const bool anims, const char **reason) -> Result {
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
			g_pending_anims = anims;
			return Result::Loading;
		}

		g_pending_path.clear();
		g_pending_asset = nullptr;
		if (!apply(asset->assetId, anims, reason)) {
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

		drop_pushed_anim_sets(hero);

		void *switched = nullptr;
		if (!call_remove_parts(hero.skin_manager) || !call_switch(hero.model_inst, own, &switched)) {
			return fail(reason, "the model switch faulted");
		}

		if (switched == nullptr) {
			return fail(reason, "the hero's own actor asset has no model");
		}

		// the switch lands at the end of the frame, the rebuild waits for it
		after_switch(AfterSwitch::RebuildSkin, hero.handle, switched, hero.actor->actorAsset->assetId);
		g_worn_path.clear();
		g_worn_actor = 0;
		return true;
	}

	// runs the pending step once the hero's ModelInst holds the model it was
	// switched to: the skin manager for the type the hero really is, vanity parts
	// and all, or the target's anim sets
	static auto
	run_after_switch() -> void {
		if (g_after == AfterSwitch::None || !game_thread::on_game_thread()) {
			return;
		}

		Hero hero {};
		const char *reason = nullptr;
		if (!find_hero(hero, &reason) || hero.handle != g_after_actor) {
			// respawned or gone: a new hero is built with its own look
			g_after = AfterSwitch::None;
			return;
		}

		const auto *current = static_cast<uint8_t *>(hero.model_inst) + MODEL_INST_MODEL;
		const auto switched = ddl::is_readable(current, sizeof(void *)) && *reinterpret_cast<void *const *>(current) == g_after_model;
		if (!switched && ++g_after_pumps < AFTER_SWITCH_MAX_PUMPS) {
			return;
		}

		const auto action = g_after;
		g_after = AfterSwitch::None;
		if (!switched) {
			g_output << "[hero_look] the switched model did not land in time, going ahead anyway\n";
		}

		if (action == AfterSwitch::PushAnimSets) {
			push_anim_sets(hero, g_after_asset);
		} else {
			const auto *own = loaded_actor_asset(g_after_asset, &reason);
			if (own == nullptr || !call_post_activate(hero.skin_manager, own)) {
				g_last_error = own == nullptr ? reason : "OnTransformationPostActivate faulted";
				g_output << "[hero_look] restore: " << g_last_error << "\n";
			}
		}

		g_output.flush();
	}

	auto
	pump() -> void {
		run_after_switch();

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
		const auto anims = g_pending_anims;
		g_pending_path.clear();
		g_pending_asset = nullptr;
		if (status != AssetStatus::Loaded) {
			g_last_error = "the actor asset failed to load";
			return;
		}

		const char *reason = nullptr;
		if (!apply(id, anims, &reason)) {
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
		result["restoring"] = g_after == AfterSwitch::RebuildSkin;
		result["anim_sets_pushed"] = g_pushed_count;
		result["last_error"] = g_last_error.empty() ? nlohmann::json() : nlohmann::json(g_last_error);
		return result;
	}
} // namespace rivet_hook::hero_look

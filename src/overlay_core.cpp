// SPDX-FileCopyrightText: 2025-2026 Neptuwunium
//
// SPDX-License-Identifier: EUPL-1.2

#include "imgui_internal.h"

#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <format>
#include <thread>

#include <imgui.h>

#include "ddl_inspector.hpp"
#include "events.hpp"
#include "game_thread.hpp"
#include "game/hero_manager.hpp"
#include "game/scene_manager.hpp"
#include "overlay.hpp"
#include "overlay_panel.hpp"
#include "overlay_tabs.hpp"
#include "runtime.hpp"
#include "scene_query.hpp"
#include "scripting.hpp"
#include "signature.hpp"
#include "signature_engine.hpp"

namespace rivet_hook {
	using namespace game;

	HeroSystem *g_HeroManager = nullptr;
	SceneManager *g_SceneManager = nullptr;
	AssetManager *g_ActorAssetManager = nullptr;

	using SpawnBot_t = void (*)(AssetManager *self, Asset *actorAsset);
	using LoadActorAsset_t = Asset * (*)(AssetManager *self, const char* name, Asset* loadedFrom, char const* loadInfo);

	SpawnBot_t game_SpawnBot = nullptr;
	LoadActorAsset_t game_LoadActorAsset = nullptr;

	char debugSpawnActorPath[0x200];
	// written by the load on the game thread, polled by the overlay
	std::atomic<Asset *> debugSpawnActor = nullptr;

	static overlay::Panel g_spawn;

	// spawning adds components, which only the game thread may do
	static auto
	QueueSpawn() -> void {
		overlay::act(g_spawn, [](std::string &message) {
			if (!game_thread::on_game_thread()) {
				message = "actors can only spawn on the game thread, and it is not pumping (loading?)";
				return false;
			}

			auto *actor = debugSpawnActor.load();
			if (actor == nullptr || actor->status != AssetStatus::Loaded) {
				message = "the actor asset is not loaded";
				return false;
			}

			game_SpawnBot(nullptr, actor);
			message = "spawned";
			return true;
		});
	}

	auto
	Overlay::HandleKeyPress(const int vk) -> void {
		if (vk == g_settings.overlay.spawn_debug_actor_key && game_SpawnBot != nullptr) {
			QueueSpawn();
		}

		if (g_settings.scripts.enabled && vk == g_settings.scripts.reload_key) {
			scripting::request_reload();
		}

		// this is the input thread, not the render thread, so the key is only
		// queued here and dispatched by the next pump
		scripting::on_key_event(vk);
	}

	static auto
	DrawDebugSpawn() -> void {
		const auto *actor = debugSpawnActor.load();
		const auto isDebugActorLoading = actor != nullptr && actor->status < AssetStatus::Loaded;
		const auto isDebugActorInvalid = actor == nullptr || actor->status != AssetStatus::Loaded;

		ImGui::LabelText("Actor Load Status", "%d", static_cast<int32_t>(actor ? actor->status : AssetStatus::Invalid));
		ImGui::InputText("Actor Path", debugSpawnActorPath, sizeof(debugSpawnActorPath), isDebugActorLoading ? ImGuiInputTextFlags_ReadOnly : ImGuiInputTextFlags_None);

		ImGui::BeginDisabled(isDebugActorLoading);
		if (ImGui::Button("Load")) {
			overlay::act(g_spawn, [path = std::string(debugSpawnActorPath)](std::string &message) {
				auto *loaded = game_LoadActorAsset(g_ActorAssetManager, path.c_str(), debugSpawnActor.load(), nullptr);
				debugSpawnActor = loaded;
				message = loaded != nullptr ? "loading " + path : "no actor asset at " + path;
				return loaded != nullptr;
			});
		}
		ImGui::EndDisabled();

		ImGui::SameLine();

		ImGui::BeginDisabled(isDebugActorInvalid);
		if (ImGui::Button("Spawn")) {
			QueueSpawn();
		}
		ImGui::EndDisabled();

		overlay::draw_message(g_spawn);
	}

	static overlay::Panel g_actor;

	// warps the hero onto an actor, read where it is when the job runs
	static auto
	TeleportHeroTo(const uint32_t target) -> void {
		overlay::act(g_actor, [target](std::string &message) {
			const auto hero = scene_query::hero();
			if (hero == 0) {
				message = "there is no hero right now";
				return false;
			}

			if (hero == target) {
				message = "that is the hero";
				return false;
			}

			const auto *actor = g_SceneManager->ResolveActor(EngineHandle { .value = target });
			if (actor == nullptr || !actor->IsValid() || actor->object == nullptr) {
				message = "the actor is gone";
				return false;
			}

			float position[3];
			memcpy(position, &actor->object->transform_matrix[3], sizeof(position));

			const char *reason = nullptr;
			if (!events::warp(hero, position, &reason)) {
				message = "could not warp the hero: " + overlay::why(reason);
				return false;
			}

			const char *name = actor->GetName();
			message = std::format("warped the hero to {} at {:.1f} {:.1f} {:.1f}", name != nullptr && *name ? name : "the actor", position[0], position[1], position[2]);
			return true;
		});
	}

	static auto
	DrawActorInfo(EngineHandle &handle) -> void {
		char labelSwap[0x100];
		static auto selectedChildIndex = -1;
		if (!handle.IsValid()) {
			return;
		}

		const auto actor = g_SceneManager->ResolveActor(handle);
		if (!actor) {
			return;
		}

		ImGui::LabelText("Object", "0x%016llx", reinterpret_cast<intptr_t>(&actor->object));
		if (actor->object) {
			static float savedPosition[3];
			static float savedScale[3];
			static auto positionHandle = INVALID_ENGINE_HANDLE;
			static auto scaleHandle = INVALID_ENGINE_HANDLE;

			if (positionHandle != handle) {
				memcpy(savedPosition, &actor->object->transform_matrix[3], sizeof(float) * 3);
			}

			if (ImGui::InputFloat3("Position", savedPosition) || ImGui::IsItemActive() || ImGui::IsItemActivated()) {
				positionHandle = handle;
			}

			if (scaleHandle != handle) {
				memcpy(savedScale, &actor->object->scale, sizeof(float) * 3);
			}

			if (ImGui::InputFloat3("Scale", savedScale) || ImGui::IsItemActive() || ImGui::IsItemActivated()) {
				scaleHandle = handle;
			}

			if (ImGui::Button("Update")) {
				const auto target = handle.value;
				const auto move = positionHandle == handle;
				const auto resize = scaleHandle == handle;
				std::array<float, 3> position;
				std::array<float, 3> scale;
				memcpy(position.data(), savedPosition, sizeof(savedPosition));
				memcpy(scale.data(), savedScale, sizeof(savedScale));

				// the hero is warped the way the game warps it, which sticks. anything
				// else gets its transform written, which the engine may stamp over
				overlay::act(g_actor, [=](std::string &message) {
					auto *live = g_SceneManager->ResolveActor(EngineHandle { .value = target });
					if (live == nullptr || live->object == nullptr) {
						message = "the actor is gone";
						return false;
					}

					if (resize) {
						memcpy(&live->object->scale, scale.data(), sizeof(float) * 3);
						message = "scale written";
					}

					if (!move) {
						return true;
					}

					if (target == scene_query::hero() && events::ready()) {
						const char *reason = nullptr;
						if (!events::warp(target, position.data(), &reason)) {
							message = "could not warp the hero: " + overlay::why(reason);
							return false;
						}

						message = "warped the hero";
						return true;
					}

					memcpy(&live->object->transform_matrix[3], position.data(), sizeof(float) * 3);
					message = "position written, the engine may stamp over it";
					return true;
				});

				positionHandle = INVALID_ENGINE_HANDLE;
				scaleHandle = INVALID_ENGINE_HANDLE;
			}

			overlay::draw_message(g_actor);
		}

		const auto hero = scene_query::hero();
		ImGui::BeginDisabled(hero == 0 || hero == handle.value || actor->object == nullptr);
		if (ImGui::Button("Teleport hero here")) {
			TeleportHeroTo(handle.value);
		}
		ImGui::EndDisabled();

		// the button sits outside the scene object block, so its outcome shows here too
		if (actor->object == nullptr) {
			overlay::draw_message(g_actor);
		}

		ImGui::SameLine();

		static std::string lastDumpPath;
		if (ImGui::Button("Dump JSON")) {
			lastDumpPath = DumpActor(actor);
		}

		ImGui::SetItemTooltip("writes this actor, every component and their live prius data to the game directory");

		if (!lastDumpPath.empty()) {
			ImGui::SameLine();
			ImGui::TextDisabled("%s", lastDumpPath.c_str());
		}

		ImGui::LabelText("Handle", "0x%08x", handle.value);
		ImGui::LabelText("UID", "0x%016llx", scene_query::uid_of(actor));
		ImGui::LabelText("Generation", "0x%04x", actor->generation);
		ImGui::LabelText("Scene Index", "0x%08x", actor->sceneIndex);
		ImGui::LabelText("Flags", "0x%08x", actor->flags);
		ImGui::TextWrapped("%s", DescribeActorFlags(actor->flags).c_str());
		// update order, not the transform hierarchy
		ImGui::LabelText("Update Parent", "0x%08x", actor->updateParent.value);
		ImGui::BeginDisabled(!actor->updateParent.IsValid());
		if (ImGui::Button("Show Update Parent")) {
			handle = actor->updateParent;
		}
		ImGui::EndDisabled();
		ImGui::LabelText("Index In Parent", "%d", actor->parentChildrenIndex);

		ImGui::Text("%d Update Children", actor->updateChildrenCount);
		const auto width = ImGui::GetContentRegionAvail().x;
		if (ImGui::BeginChild("actor_children", ImVec2(width, 0), ImGuiChildFlags_Borders | ImGuiChildFlags_ResizeY | ImGuiChildFlags_AutoResizeY)) {
			for (auto index = 0; index < actor->updateChildrenCount && actor->updateChildren != nullptr; index++) {
				const auto childHandle = actor->updateChildren[index];
				if (!childHandle.IsValid()) {
					continue;
				}

				const auto child = g_SceneManager->ResolveActor(childHandle);
				if (!child || !child->IsValid()) {
					continue;
				}

				const char *name = child->GetName();
				if (!name || !*name) {
					sprintf_s(labelSwap, "Actor %08x##DrawActorInfo", handle.value);
					name = labelSwap;
				}

				ImGui::PushID(index);
				if (ImGui::Selectable(name, selectedChildIndex == index)) {
					handle = childHandle;
				}
				ImGui::PopID();
			}
		}
		ImGui::EndChild();

		ImGui::Text("%d Components", actor->componentCount);
		if (ImGui::BeginChild("actor_components", ImVec2(width, 0))) {
			const auto inner_width = ImGui::GetContentRegionAvail().x;
			for (auto index = 0; index < actor->componentCount; index++) {
				const auto [componentType, instance] = actor->components[index];
				if (componentType == nullptr || instance == nullptr || instance->IsDestroyed()) {
					continue;
				}

				const char *name = componentType->name;
				ImGui::PushID(index);
				if (ImGui::BeginChild("actor_components", ImVec2(inner_width, 0), ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY)) {
					ImGui::LabelText("Name", "%s", name);
					ImGui::LabelText("Handle", "0x%08x", instance->handle.value);
					ImGui::LabelText("Parent Handle", "0x%08x", instance->parentComponent.value);
					ImGui::LabelText("Child Count", "0x%02x", instance->childCount);
					ImGui::LabelText("Prius", "%s", PriusBehaviorName(componentType->prius_behavior));
					ImGui::TextWrapped("%s", DescribeFlags(componentType->class_flags, COMPONENT_CLASS_FLAG_NAMES, std::size(COMPONENT_CLASS_FLAG_NAMES)).c_str());

					DrawComponentPrius(componentType, instance);
				}
				ImGui::EndChild();
				ImGui::PopID();
			}
		}
		ImGui::EndChild();
	}

	// the actor a uid lookup found, for the next frame to select
	static std::atomic_uint32_t g_foundActor = 0;
	static overlay::Panel g_lookup;

	static auto
	DrawActorLookup(EngineHandle &actorHandle) -> void {
		if (const auto found = g_foundActor.exchange(0); found != 0) {
			actorHandle = EngineHandle { .value = found };
		}

		const auto hero = scene_query::hero();
		ImGui::BeginDisabled(hero == 0);
		if (ImGui::Button("Hero")) {
			actorHandle = EngineHandle { .value = hero };
		}
		ImGui::EndDisabled();
		ImGui::SetItemTooltip("select the actor the game treats as the player");

		ImGui::SameLine();

		static char uidText[0x20];
		ImGui::SetNextItemWidth(200);
		const auto entered = ImGui::InputTextWithHint("##uid", "uid", uidText, sizeof(uidText), ImGuiInputTextFlags_EnterReturnsTrue);
		ImGui::SameLine();
		if (ImGui::Button("Find UID") || entered) {
			const auto uid = strtoull(uidText, nullptr, 0);
			overlay::act(g_lookup, [uid](std::string &message) {
				const auto found = scene_query::actor_by_uid(uid);
				if (found == 0) {
					message = std::format("no live actor has uid 0x{:016x}", uid);
					return false;
				}

				g_foundActor = found;
				message = std::format("uid 0x{:016x} is actor 0x{:08x}", uid, found);
				return true;
			});
		}

		ImGui::SetItemTooltip("hex with 0x, or decimal");

		ImGui::SameLine();
		overlay::draw_message(g_lookup);
	}

	static auto
	DrawActorGroups() -> void {
		static auto selectedIndex = -1;
		static auto actorHandle = INVALID_ENGINE_HANDLE;
		char labelSwap[0x100];

		DrawActorLookup(actorHandle);

		if (ImGui::BeginChild("actor_groups_left", ImVec2(150, 0), ImGuiChildFlags_Borders | ImGuiChildFlags_ResizeX)) {
			for (auto index = 0; index < g_SceneManager->actorGroupMax; index++) {
				if (const auto *actorGroup = &g_SceneManager->actorGroups[index]; actorGroup->handles != nullptr && actorGroup->count > 0) {
					const char *name = actorGroup->name;
					if (!name || !*name) {
						sprintf_s(labelSwap, "ActorGroup %08x##DrawActorGroups", index);
						name = labelSwap;
					}

					ImGui::PushID(index);
					if (ImGui::Selectable(name, selectedIndex == index)) {
						selectedIndex = index;
					}
					ImGui::PopID();
				}
			}
		}
		ImGui::EndChild();

		ImGui::SameLine();

		if (ImGui::BeginChild("actor_groups_middle", ImVec2(300, 0), ImGuiChildFlags_Borders | ImGuiChildFlags_ResizeX) && selectedIndex > -1 && selectedIndex < g_SceneManager->actorGroupMax) {
			if (const auto *actorGroup = &g_SceneManager->actorGroups[selectedIndex]; actorGroup->handles != nullptr && actorGroup->count > 0) {
				for (auto index = 0; index < actorGroup->count; index++) {
					const auto handle = actorGroup->handles[index];

					if (const auto actor = g_SceneManager->ResolveActor(handle); actor != nullptr) {
						const char *name = actor->GetName();
						if (!name || !*name) {
							sprintf_s(labelSwap, "Actor %08x##DrawActorGroups", handle.value);
							name = labelSwap;
						}

						ImGui::PushID(index);
						if (ImGui::Selectable(name, actorHandle == handle)) {
							actorHandle = handle;
						}

						if (ImGui::BeginPopupContextItem("actor_menu")) {
							if (ImGui::MenuItem("Teleport hero here", nullptr, false, handle.value != scene_query::hero())) {
								TeleportHeroTo(handle.value);
								actorHandle = handle;
							}

							ImGui::EndPopup();
						}
						ImGui::PopID();
					}
				}
			}
		}
		ImGui::EndChild();

		ImGui::SameLine();

		if (ImGui::BeginChild("actor_groups_right", ImVec2(0, 0)) && actorHandle.IsValid()) {
			DrawActorInfo(actorHandle);
		}

		ImGui::EndChild();
	}

	static auto
	CheckSpawnBot() -> bool {
		return g_ActorAssetManager != nullptr && game_SpawnBot != nullptr && game_LoadActorAsset != nullptr;
	}

	static auto
	CheckActorGroups() -> bool {
		return g_SceneManager != nullptr && g_SceneManager->actorGroups != nullptr && g_SceneManager->actorGroupMax > 0;
	}

	using RivetImGuiCallback = void(*)();
	using RivetImGuiCheckCallback = bool(*)();
	static std::array<std::tuple<RivetImGuiCallback, RivetImGuiCheckCallback, const char*>, 9> tabs {{
		{ DrawActorGroups, CheckActorGroups, "World" },
		{ DrawDebugSpawn, CheckSpawnBot, "Spawn Actor" },
		{ overlay::DrawHero, nullptr, "Hero" },
		{ overlay::DrawCameraTime, nullptr, "Camera & Time" },
		{ overlay::DrawHud, nullptr, "HUD" },
		{ overlay::DrawEvents, nullptr, "Events" },
		{ overlay::DrawConfigs, nullptr, "Configs" },
		{ overlay::DrawScripts, nullptr, "Scripts" },
		{ overlay::DrawStatus, nullptr, "Status" },
	}};

	auto
	Overlay::DrawImGUI() -> void {
		ImGui::Begin("Rivet");

		ImGui::BeginTabBar("RivetTabs");

		for (const auto &[callback, check, name] : tabs) {
			ImGui::BeginDisabled(check ? !check() : false);

			if (ImGui::BeginTabItem(name)) {
				callback();
				ImGui::EndTabItem();
			}

			ImGui::EndDisabled();
		}

		ImGui::EndTabBar();

		ImGui::End();
	}

	auto
	Overlay::Init() -> void {
		if (!g_settings.overlay.enabled) {
			return;
		}

		memset(debugSpawnActorPath, 0, sizeof(debugSpawnActorPath));

		g_HeroManager = static_cast<HeroSystem *>(load_rel_var(find_address(HERO_SYSTEM_SIGNATURE), HERO_SYSTEM_ADDRESS));
		g_SceneManager = static_cast<SceneManager *>(load_rel_var(find_address(SCENE_MANAGER_SIGNATURE), SCENE_MANAGER_ADDRESS));
		g_ActorAssetManager = static_cast<AssetManager *>(load_rel_var(find_address(ACTOR_ASSET_MANAGER_SIGNATURE), ACTOR_ASSET_MANAGER_ADDRESS));

		game_SpawnBot = reinterpret_cast<SpawnBot_t>(find_address(SPAWN_BOT_SIGNATURE));
		game_LoadActorAsset = reinterpret_cast<LoadActorAsset_t>(find_address(LOAD_ACTOR_ASSET_SIGNATURE));

		std::thread(D3D12Init).detach();
	}

	auto
	Overlay::Fini() -> void {
	}
} // namespace rivet_hook

// SPDX-FileCopyrightText: 2025-2026 Neptuwunium
//
// SPDX-License-Identifier: EUPL-1.2

#include "imgui_internal.h"

#include <cstdio>
#include <thread>
#include <format>
#include <mutex>

#include <imgui.h>

#include "ddl_inspector.hpp"
#include "game/hero_manager.hpp"
#include "game/scene_manager.hpp"
#include "overlay.hpp"
#include "runtime.hpp"
#include "scripting.hpp"
#include "signature.hpp"
#include "signature_engine.hpp"

namespace rivet_hook {
	using namespace game;

	std::thread g_SpawnThread;
	HANDLE g_SpawnSignal;

	HeroSystem *g_HeroManager = nullptr;
	SceneManager *g_SceneManager = nullptr;
	AssetManager *g_ActorAssetManager = nullptr;

	using SpawnBot_t = void (*)(AssetManager *self, Asset *actorAsset);
	using LoadActorAsset_t = Asset * (*)(AssetManager *self, const char* name, Asset* loadedFrom, char const* loadInfo);

	SpawnBot_t game_SpawnBot = nullptr;
	LoadActorAsset_t game_LoadActorAsset = nullptr;

	char debugSpawnActorPath[0x200];
	Asset *debugSpawnActor = nullptr;
	bool isSpawningDebugActor = false;

	auto
	SpawnDebugActor() -> void {
		while (true) {
			if (FAILED(WaitForSingleObject(g_SpawnSignal, INFINITE))) {
				break;
			}

			{
				if (debugSpawnActor == nullptr || debugSpawnActor->status != AssetStatus::Loaded || isSpawningDebugActor) {
					continue;
				}

				isSpawningDebugActor = true;
				game_SpawnBot(nullptr, debugSpawnActor);
				isSpawningDebugActor = false;
			}
		}
	}

	auto
	Overlay::HandleKeyPress(const int vk) -> void {
		if (vk == g_settings.overlay.spawn_debug_actor_key) {
			SetEvent(g_SpawnSignal);
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
		const auto isDebugActorLoading = debugSpawnActor != nullptr && debugSpawnActor->status < AssetStatus::Loaded;
		const auto isDebugActorInvalid = debugSpawnActor == nullptr || debugSpawnActor->status != AssetStatus::Loaded || isSpawningDebugActor;
		const auto shouldHideInput = isDebugActorLoading || isSpawningDebugActor;

		ImGui::LabelText("Actor Load Status", "%d", static_cast<int32_t>(debugSpawnActor ? debugSpawnActor->status : AssetStatus::Invalid));
		ImGui::InputText("Actor Path", debugSpawnActorPath, sizeof(debugSpawnActorPath), shouldHideInput ? ImGuiInputTextFlags_ReadOnly : ImGuiInputTextFlags_None);

		ImGui::BeginDisabled(isDebugActorLoading);
		if (ImGui::Button("Load")) {
			debugSpawnActor = game_LoadActorAsset(g_ActorAssetManager, debugSpawnActorPath, debugSpawnActor, nullptr);
		}
		ImGui::EndDisabled();

		ImGui::SameLine();

		ImGui::BeginDisabled(isDebugActorInvalid);
		if (ImGui::Button("Spawn")) {
			SetEvent(g_SpawnSignal);
		}
		ImGui::EndDisabled();
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
				if (positionHandle == handle) {
					memcpy(&actor->object->transform_matrix[3], savedPosition, sizeof(float) * 3);
					positionHandle = INVALID_ENGINE_HANDLE;
				}

				if (scaleHandle == handle) {
					memcpy(&actor->object->scale, savedScale, sizeof(float) * 3);
					scaleHandle = INVALID_ENGINE_HANDLE;
				}
			}
		}

		static std::string lastDumpPath;
		if (ImGui::Button("Dump JSON")) {
			lastDumpPath = DumpActor(actor);
		}

		ImGui::SetItemTooltip("writes this actor, every component and their live prius data to the game directory");

		if (!lastDumpPath.empty()) {
			ImGui::SameLine();
			ImGui::TextDisabled("%s", lastDumpPath.c_str());
		}

		ImGui::LabelText("Type", "0x%04x", actor->type);
		ImGui::LabelText("Scene Index", "0x%08x", actor->sceneIndex);
		ImGui::LabelText("Flags", "0x%08x", actor->flags);
		ImGui::LabelText("Parent", "0x%08x", actor->parentHandle.value);
		ImGui::BeginDisabled(!actor->parentHandle.IsValid());
		if (ImGui::Button("Show Parent")) {
			handle = actor->parentHandle;
		}
		ImGui::EndDisabled();
		ImGui::LabelText("Index", "0x%04x", actor->parentIndex);

		ImGui::Text("%d Children", actor->childCount);
		const auto width = ImGui::GetContentRegionAvail().x;
		if (ImGui::BeginChild("actor_children", ImVec2(width, 0), ImGuiChildFlags_Borders | ImGuiChildFlags_ResizeY | ImGuiChildFlags_AutoResizeY)) {
			for (auto index = 0; index < actor->childCount; index++) {
				const auto childHandle = actor->children[index];
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
				if (componentType == nullptr || instance == nullptr) {
					continue;
				}

				const char *name = componentType->name;
				ImGui::PushID(index);
				if (ImGui::BeginChild("actor_components", ImVec2(inner_width, 0), ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY)) {
					ImGui::LabelText("Name", "%s", name);
					ImGui::LabelText("Handle", "0x%08x", instance->handle.value);
					ImGui::LabelText("Parent Handle", "0x%08x", instance->parentComponent.value);
					ImGui::LabelText("Child Count", "0x%02x", instance->childCount);

					DrawComponentPrius(componentType, instance);
				}
				ImGui::EndChild();
				ImGui::PopID();
			}
		}
		ImGui::EndChild();
	}

	static auto
	DrawActorGroups() -> void {
		static auto selectedIndex = -1;
		static auto actorHandle = INVALID_ENGINE_HANDLE;
		char labelSwap[0x100];

		if (ImGui::BeginChild("actor_groups_left", ImVec2(150, 0), ImGuiChildFlags_Borders | ImGuiChildFlags_ResizeX)) {
			for (auto index = 0; index < g_SceneManager->actorGroupCount; index++) {
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

		if (ImGui::BeginChild("actor_groups_middle", ImVec2(300, 0), ImGuiChildFlags_Borders | ImGuiChildFlags_ResizeX) && selectedIndex > -1 && selectedIndex < g_SceneManager->actorGroupCount) {
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
	DrawHeroSystem() -> void {

	}

	static auto
	CheckSpawnBot() -> bool {
		return g_ActorAssetManager != nullptr && game_SpawnBot != nullptr && game_LoadActorAsset != nullptr;
	}

	static auto
	CheckActorGroups() -> bool {
		return g_SceneManager != nullptr && g_SceneManager->actorGroups != nullptr && g_SceneManager->actorGroupCount > 0;
	}

	static auto
	CheckHeroSystem() -> bool {
		return g_SceneManager != nullptr && g_SceneManager->actors != nullptr && g_HeroManager != nullptr && false;
	}

	using RivetImGuiCallback = void(*)();
	using RivetImGuiCheckCallback = bool(*)();
	static std::array<std::tuple<RivetImGuiCallback, RivetImGuiCheckCallback, const char*>, 3> tabs {{
		{ DrawDebugSpawn, CheckSpawnBot, "Spawn Actor" },
		{ DrawActorGroups, CheckActorGroups, "Actor Groups" },
		{ DrawHeroSystem, CheckHeroSystem, "Hero System" },
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
		g_SpawnSignal = CreateEvent(nullptr, false, false, "Rivet Debug Spawn Signal");
		g_SpawnThread = std::thread(SpawnDebugActor);

		g_HeroManager = static_cast<HeroSystem *>(load_rel_var(find_address(HERO_SYSTEM_SIGNATURE), HERO_SYSTEM_ADDRESS));
		g_SceneManager = static_cast<SceneManager *>(load_rel_var(find_address(SCENE_MANAGER_SIGNATURE), SCENE_MANAGER_ADDRESS));
		g_ActorAssetManager = static_cast<AssetManager *>(load_rel_var(find_address(ACTOR_ASSET_MANAGER_SIGNATURE), ACTOR_ASSET_MANAGER_ADDRESS));

		game_SpawnBot = reinterpret_cast<SpawnBot_t>(find_address(SPAWN_BOT_SIGNATURE));
		game_LoadActorAsset = reinterpret_cast<LoadActorAsset_t>(find_address(LOAD_ACTOR_ASSET_SIGNATURE));

		std::thread(D3D12Init).detach();
	}

	auto
	Overlay::Fini() -> void {
		if (!g_settings.overlay.enabled) {
			return;
		}

		CloseHandle(g_SpawnSignal);
	}
} // namespace rivet_hook

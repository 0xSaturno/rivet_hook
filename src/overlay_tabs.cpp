// SPDX-FileCopyrightText: 2025-2026 Neptuwunium
//
// SPDX-License-Identifier: EUPL-1.2

#include <algorithm>
#include <format>
#include <string>

#include <imgui.h>
#include <imgui_stdlib.h>
#include <nlohmann/json.hpp>

#include "overlay_tabs.hpp"

#include "camera.hpp"
#include "configs.hpp"
#include "ddl_visit.hpp"
#include "events.hpp"
#include "game_thread.hpp"
#include "hero_look.hpp"
#include "hud.hpp"
#include "overlay_panel.hpp"
#include "runtime.hpp"
#include "scene_query.hpp"
#include "scripting.hpp"
#include "time_scale.hpp"
#include "vanity.hpp"

namespace rivet_hook::overlay {
	// a key of a state object, fallback when the state has not been read yet or
	// the key is missing, null or of another type
	template <typename T>
	static auto
	get(const nlohmann::json &object, const char *key, T fallback) -> T {
		if (!object.is_object()) {
			return fallback;
		}

		const auto it = object.find(key);
		if (it == object.end() || it->is_null()) {
			return fallback;
		}

		try {
			return it->get<T>();
		} catch (const std::exception &) {
			return fallback;
		}
	}

	// a key of a state object by reference, an empty array when it is missing.
	// the lists are drawn every frame, so they are not copied out
	static auto
	member(const nlohmann::json &object, const char *key) -> const nlohmann::json & {
		static const nlohmann::json EMPTY = nlohmann::json::array();
		if (!object.is_object()) {
			return EMPTY;
		}

		const auto it = object.find(key);
		return it != object.end() ? *it : EMPTY;
	}

	static auto
	snapshot(Panel &panel) -> nlohmann::json {
		std::lock_guard guard { panel.lock };
		return panel.state;
	}

	static auto
	draw_reason(const std::string &reason) -> void {
		ImGui::TextDisabled("%s", reason.empty() ? "reading..." : reason.c_str());
	}

	// ----------------------------------------------------------------- hero --

	static Panel g_hero;
	static Panel g_vanity;

	auto
	DrawHero() -> void {
		refresh(g_hero, [] {
			nlohmann::json state;
			state["hero"] = scene_query::hero();
			state["look"] = hero_look::status();
			return state;
		}, 250);

		const auto state = snapshot(g_hero);
		const auto hero = get<uint32_t>(state, "hero", 0);
		if (hero == 0) {
			ImGui::TextDisabled("there is no hero right now");
		} else {
			ImGui::Text("Hero 0x%08x", hero);
		}

		ImGui::SeparatorText("Model");

		static std::string lookPath;
		static auto anims = false;
		ImGui::InputTextWithHint("Asset", ".actor or .model path", &lookPath);

		// a .model has no anim sets of its own
		const auto is_model = lookPath.size() > 6 && _stricmp(lookPath.c_str() + lookPath.size() - 6, ".model") == 0;
		ImGui::BeginDisabled(is_model);
		ImGui::Checkbox("Anim sets", &anims);
		ImGui::EndDisabled();
		ImGui::SetItemTooltip(is_model ? "a .model has no anim sets of its own, its .actor has them" : "also puts the actor asset's anim sets on top of the hero's");

		const auto wear = [](const std::string &path, const bool with_anims) {
			act(g_hero, [path, with_anims](std::string &message) {
				const char *reason = nullptr;
				switch (hero_look::request(path.c_str(), with_anims, &reason)) {
					case hero_look::Result::Applied:
						message = "wearing " + path;
						return true;
					case hero_look::Result::Loading:
						message = "loading " + path + ", it goes on once loaded";
						return true;
					default:
						message = why(reason);
						return false;
				}
			});
		};

		ImGui::BeginDisabled(lookPath.empty());
		if (ImGui::Button("Apply")) {
			wear(lookPath, anims && !is_model);
		}
		ImGui::EndDisabled();

		ImGui::SameLine();
		if (ImGui::Button("Restore")) {
			act(g_hero, [](std::string &message) {
				const char *reason = nullptr;
				if (!hero_look::restore(&reason)) {
					message = why(reason);
					return false;
				}

				message = "restoring the hero's own model";
				return true;
			});
		}

		const auto look = get<nlohmann::json>(state, "look", {});
		if (look.is_object()) {
			if (!get<bool>(look, "available", false)) {
				ImGui::TextDisabled("the engine calls did not resolve");
			}

			ImGui::LabelText("Worn", "%s", get<std::string>(look, "worn", "the hero's own").c_str());
			if (const auto pending = get<std::string>(look, "pending", ""); !pending.empty()) {
				ImGui::LabelText("Pending", "%s", pending.c_str());
			}

			if (get<bool>(look, "restoring", false)) {
				ImGui::TextDisabled("restoring...");
			}

			ImGui::LabelText("Anim sets pushed", "%d", get<int>(look, "anim_sets_pushed", 0));
			ImGui::LabelText("Models held", "%d", get<int>(look, "models_held", 0));

			// the last look put on, kept in rivet.toml; restore forgets it
			const auto remembered = get<std::string>(look, "remembered", "");
			ImGui::LabelText("Remembered", "%s", remembered.empty() ? "nothing" : remembered.c_str());
			auto on_launch = get<bool>(look, "apply_on_launch", false);
			if (ImGui::Checkbox("Apply on launch", &on_launch)) {
				act(g_hero, [on_launch](std::string &message) {
					hero_look::set_apply_on_launch(on_launch);
					message = on_launch ? "the remembered look goes back on after a launch" : "the hero starts with its own look";
					return true;
				});
			}

			ImGui::SetItemTooltip("puts the remembered look back on once the hero first appears after the game starts");
			if (const auto error = get<std::string>(look, "last_error", ""); !error.empty()) {
				ImGui::LabelText("Last error", "%s", error.c_str());
			}
		}

		draw_message(g_hero);

		// the list never changes after startup, so it is read here on the render
		// thread and only rebuilt when the filter does
		if (ImGui::CollapsingHeader("Browse models")) {
			static std::string filter;
			static std::string listedFilter = "\x01";
			static nlohmann::json listing;
			ImGui::InputTextWithHint("Filter", "path, mod or name", &filter);
			if (filter != listedFilter) {
				listing = hero_look::models(filter.c_str());
				listedFilter = filter;
			}

			const auto height = ImGui::GetTextLineHeightWithSpacing() * 12.0f;
			if (ImGui::BeginChild("hero_models", ImVec2(0, height), ImGuiChildFlags_Borders | ImGuiChildFlags_ResizeY)) {
				const auto entry = [&](const std::string &path, const std::string &label, const int id) {
					ImGui::PushID(id);
					if (ImGui::Selectable(label.c_str(), lookPath == path)) {
						lookPath = path;
						wear(path, false);
					}

					ImGui::SetItemTooltip("%s", path.c_str());
					ImGui::PopID();
				};

				auto id = 0;
				ImGui::SeparatorText("Game");
				for (const auto &model : listing["game"]) {
					entry(model["path"].get<std::string>(), model["name"].get<std::string>(), id++);
				}

				ImGui::SeparatorText("Mods");
				if (listing["mods"].empty()) {
					ImGui::TextDisabled("no .model in any mod path");
				}

				// grouped by the mod path each came from, in load order
				std::string mod;
				auto open = false;
				auto first = true;
				for (const auto &model : listing["mods"]) {
					if (const auto &from = model["mod"].get_ref<const std::string &>(); first || from != mod) {
						first = false;
						if (open) {
							ImGui::TreePop();
						}

						mod = from;
						open = ImGui::TreeNodeEx(mod.c_str(), ImGuiTreeNodeFlags_DefaultOpen);
					}

					if (open) {
						const auto &path = model["path"].get_ref<const std::string &>();
						const auto slash = path.find_last_of('/');
						entry(path, slash == std::string::npos ? path : path.substr(slash + 1), id);
					}

					++id;
				}

				if (open) {
					ImGui::TreePop();
				}
			}

			ImGui::EndChild();
			ImGui::TextDisabled("click to wear; hover for the full path");
		}

		ImGui::SeparatorText("Play as");

		// the game's own hero swap: hero type, abilities, voice and all
		if (look.is_object()) {
			const auto playing = get<std::string>(look, "playing_as", "");
			ImGui::LabelText("Playing as", "%s", playing.empty() ? "-" : playing == "spawned" ? "the hero it spawned as" : playing.c_str());
			if (const auto pending = get<std::string>(look, "play_as_pending", ""); !pending.empty()) {
				ImGui::TextDisabled("loading %s...", pending.c_str());
			}
		}

		constexpr const char *HEROES[][2] = { { "Ratchet", "ratchet" }, { "Rivet", "rivet" }, { "Clank", "clank" }, { "Kit", "kit" } };
		for (auto index = 0; index < 4; ++index) {
			if (index > 0) {
				ImGui::SameLine();
			}

			if (ImGui::Button(HEROES[index][0])) {
				act(g_hero, [label = std::string(HEROES[index][0]), name = std::string(HEROES[index][1])](std::string &message) {
					const char *reason = nullptr;
					switch (hero_look::play_as(hero_look::hero_type(name.c_str()), &reason)) {
						case hero_look::Result::Applied:
							message = "playing as " + label;
							return true;
						case hero_look::Result::Loading:
							message = "loading " + label + ", the swap happens once loaded";
							return true;
						default:
							message = why(reason);
							return false;
					}
				});
			}
		}

		ImGui::TextDisabled("a full hero swap: moves, abilities and voice change too");

		ImGui::SeparatorText("Outfit");

		static std::string bundle;
		ImGui::InputTextWithHint("Bundle", "bundle config path or 16 hex digits", &bundle);

		const auto vanity_act = [](const std::string &text, const bool equip) {
			act(g_vanity, [text, equip](std::string &message) {
				uint64_t id = 0;
				if (!vanity::bundle_id(text.c_str(), id)) {
					message = text + " is neither a bundle path nor a 16 digit hex id";
					return false;
				}

				const auto hero = scene_query::hero();
				if (hero == 0) {
					message = "there is no hero right now";
					return false;
				}

				const char *reason = nullptr;
				auto answer = false;
				const auto worked = equip ? vanity::equip_bundle(hero, id, answer, &reason) : vanity::has_bundle(hero, id, answer, &reason);
				if (!worked) {
					message = why(reason);
					return false;
				}

				if (equip) {
					message = answer ? "equipped" : "already worn";
				} else {
					message = answer ? "owned" : "not owned";
				}

				return true;
			});
		};

		ImGui::BeginDisabled(bundle.empty());
		if (ImGui::Button("Owns?")) {
			vanity_act(bundle, false);
		}

		ImGui::SameLine();
		if (ImGui::Button("Equip")) {
			vanity_act(bundle, true);
		}
		ImGui::EndDisabled();

		draw_message(g_vanity);
	}

	// -------------------------------------------------------- camera & time --

	static Panel g_camera;
	static Panel g_time;
	static Panel g_fov;
	static Panel g_free;
	static Panel g_shake;

	static auto
	DrawTimeScale(const nlohmann::json &state) -> void {
		ImGui::SeparatorText("Time scale");

		const auto time = get<nlohmann::json>(state, "time", {});
		if (!get<bool>(time, "ready", false)) {
			draw_reason(get<std::string>(time, "reason", ""));
			return;
		}

		ImGui::Text("Running at %.3fx", get<double>(time, "applied", 1.0));

		const auto names = get<std::vector<std::string>>(state, "channels", {});
		static auto channel = -1;
		if (channel < 0 || channel >= static_cast<int>(names.size())) {
			channel = 0;
			for (size_t index = 0; index < names.size(); ++index) {
				if (names[index] == "kGame") {
					channel = static_cast<int>(index);
				}
			}
		}

		const auto label = [&names](const int index) {
			return index < static_cast<int>(names.size()) && !names[index].empty() ? names[index] : std::to_string(index);
		};

		if (ImGui::BeginCombo("Channel", label(channel).c_str())) {
			for (auto index = 0; index < static_cast<int>(names.size()); ++index) {
				if (ImGui::Selectable(label(index).c_str(), index == channel)) {
					channel = index;
				}
			}

			ImGui::EndCombo();
		}

		static auto scale = 1.0f;
		static auto ramp = -1.0f;
		ImGui::SliderFloat("Scale", &scale, 0.05f, 4.0f, "%.2fx", ImGuiSliderFlags_Logarithmic);
		const auto released = ImGui::IsItemDeactivatedAfterEdit();
		ImGui::InputFloat("Ramp", &ramp, 0.0f, 0.0f, "%.2f");
		ImGui::SetItemTooltip("how fast the game eases toward it, in scale per second. negative takes the engine's default");

		if (ImGui::Button("Apply") || released) {
			act(g_time, [channel = channel, scale = scale, ramp = ramp](std::string &message) {
				const char *reason = nullptr;
				if (!time_scale::set(channel, scale, ramp, &reason)) {
					message = why(reason);
					return false;
				}

				message = std::format("{} asks for {:.2f}x", time_scale::channel_name(channel), scale);
				return true;
			});
		}

		ImGui::SameLine();
		if (ImGui::Button("Clear")) {
			act(g_time, [channel = channel](std::string &message) {
				const char *reason = nullptr;
				if (!time_scale::clear(channel, &reason)) {
					message = why(reason);
					return false;
				}

				message = std::format("{} is back to normal speed", time_scale::channel_name(channel));
				return true;
			});
		}

		if (const auto channels = get<nlohmann::json>(time, "channels", {}); channels.is_object() && !channels.empty()) {
			ImGui::TextDisabled("channels asking for something else:");
			for (const auto &[name, value] : channels.items()) {
				ImGui::BulletText("%s  %.3fx", name.c_str(), value.is_number() ? value.get<double>() : 1.0);
			}
		}

		draw_message(g_time);
	}

	static auto
	DrawFov(const nlohmann::json &state) -> void {
		ImGui::SeparatorText("Field of view");

		if (const auto reason = get<std::string>(state, "fov_reason", "reading..."); !reason.empty()) {
			draw_reason(reason);
			return;
		}

		static auto fov = 1.0f;
		static auto editing = false;
		if (!editing) {
			fov = get<float>(state, "fov", fov);
		}

		ImGui::SliderFloat("FOV scale", &fov, 0.5f, 2.0f, "%.3f");
		editing = ImGui::IsItemActive();
		if (ImGui::IsItemDeactivatedAfterEdit()) {
			act(g_fov, [scale = fov](std::string &message) {
				const char *reason = nullptr;
				if (!camera::set_fov_scale(scale, &reason)) {
					message = why(reason);
					return false;
				}

				message = std::format("fov scale {:.3f}", scale);
				return true;
			});
		}

		ImGui::SetItemTooltip("the game writes this again whenever the graphics settings are applied");
		draw_message(g_fov);
	}

	static auto
	DrawFreeCamera(const nlohmann::json &state) -> void {
		ImGui::SeparatorText("Free camera");

		if (const auto reason = get<std::string>(state, "free_reason", "reading..."); !reason.empty()) {
			draw_reason(reason);
			return;
		}

		const auto detached = get<bool>(state, "detached", false);
		if (!detached) {
			if (ImGui::Button("Detach")) {
				act(g_free, [](std::string &message) {
					const char *reason = nullptr;
					if (!camera::detach(&reason)) {
						message = why(reason);
						return false;
					}

					message = "detached";
					return true;
				});
			}
		} else if (ImGui::Button("Attach")) {
			act(g_free, [](std::string &message) {
				camera::attach();
				message = "attached";
				return true;
			});
		}

		ImGui::SameLine();
		ImGui::BeginDisabled(get<uint32_t>(state, "hero", 0) == 0);
		if (ImGui::Button("Warp hero to camera")) {
			act(g_free, [](std::string &message) {
				camera::View view;
				if (!camera::view(view)) {
					message = "the camera is not readable";
					return false;
				}

				const auto hero = scene_query::hero();
				if (hero == 0) {
					message = "there is no hero right now";
					return false;
				}

				const char *reason = nullptr;
				if (!events::warp(hero, view.position, &reason)) {
					message = "could not warp the hero: " + why(reason);
					return false;
				}

				message = std::format("warped the hero to {:.1f} {:.1f} {:.1f}", view.position[0], view.position[1], view.position[2]);
				return true;
			});
		}
		ImGui::EndDisabled();

		// live values until one is edited, then the edit holds until moved or reverted
		static camera::View edit;
		static auto dirty = false;
		const auto live = get<nlohmann::json>(state, "view", {});
		if (!dirty && live.is_object()) {
			const auto position = get<std::vector<float>>(live, "position", {});
			for (size_t axis = 0; axis < 3 && axis < position.size(); ++axis) {
				edit.position[axis] = position[axis];
			}

			edit.yaw = get<float>(live, "yaw", 0.0f);
			edit.pitch = get<float>(live, "pitch", 0.0f);
			edit.fov = get<float>(live, "fov", 0.0f);
		}

		ImGui::BeginDisabled(!detached);
		dirty |= ImGui::InputFloat3("Position", edit.position);
		dirty |= ImGui::DragFloat("Yaw", &edit.yaw, 0.5f, -180.0f, 180.0f, "%.1f");
		dirty |= ImGui::DragFloat("Pitch", &edit.pitch, 0.5f, -89.0f, 89.0f, "%.1f");
		dirty |= ImGui::DragFloat("FOV", &edit.fov, 0.1f, 1.0f, 170.0f, "%.1f");

		ImGui::BeginDisabled(!dirty);
		if (ImGui::Button("Move")) {
			act(g_free, [view = edit](std::string &message) {
				const char *reason = nullptr;
				if (!camera::set_view(view, &reason)) {
					message = why(reason);
					return false;
				}

				message = "moved";
				return true;
			});

			dirty = false;
		}

		ImGui::SameLine();
		if (ImGui::Button("Revert")) {
			dirty = false;
		}
		ImGui::EndDisabled();
		ImGui::EndDisabled();

		draw_message(g_free);
	}

	static auto
	DrawShake(const nlohmann::json &state) -> void {
		ImGui::SeparatorText("Camera shake");

		const auto blocked = get<bool>(state, "shake_blocked", false);
		const auto overridden = get<bool>(state, "shake_override", false);
		const auto mode = overridden ? (blocked ? 1 : 0) : 2;

		const auto set = [](const int to) {
			act(g_shake, [to](std::string &message) {
				if (to == 2) {
					camera::release_shake();
					message = "shake follows the game's option";
					return true;
				}

				const char *reason = nullptr;
				if (!camera::block_shake(to == 1, &reason)) {
					message = why(reason);
					return false;
				}

				message = to == 1 ? "shake blocked" : "shake let through";
				return true;
			});
		};

		if (ImGui::RadioButton("On", mode == 0) && mode != 0) {
			set(0);
		}

		ImGui::SameLine();
		if (ImGui::RadioButton("Off", mode == 1) && mode != 1) {
			set(1);
		}

		ImGui::SameLine();
		if (ImGui::RadioButton("Game option", mode == 2) && mode != 2) {
			set(2);
		}

		ImGui::SameLine();
		ImGui::TextDisabled("(%s now)", blocked ? "blocked" : "allowed");
		draw_message(g_shake);
	}

	auto
	DrawCameraTime() -> void {
		refresh(g_camera, [] {
			nlohmann::json state;
			state["time"] = time_scale::status();

			nlohmann::json::array_t channels;
			if (time_scale::ready()) {
				for (int32_t index = 0; index < time_scale::CHANNEL_COUNT; ++index) {
					channels.emplace_back(time_scale::channel_name(index));
				}
			}

			state["channels"] = channels;
			state["fov_reason"] = camera::fov_unavailable_reason();
			state["fov"] = camera::fov_scale();
			state["free_reason"] = camera::free_unavailable_reason();
			state["detached"] = camera::detached();
			state["shake_blocked"] = camera::shake_blocked();
			state["shake_override"] = camera::shake_overridden();
			state["hero"] = scene_query::hero();

			camera::View view;
			if (camera::view(view)) {
				state["view"] = {
					{ "position", { view.position[0], view.position[1], view.position[2] } },
					{ "yaw", view.yaw },
					{ "pitch", view.pitch },
					{ "fov", view.fov },
				};
			}

			return state;
		}, 100);

		const auto state = snapshot(g_camera);
		DrawTimeScale(state);
		DrawFov(state);
		DrawFreeCamera(state);
		DrawShake(state);
	}

	// ------------------------------------------------------------------ hud --

	static Panel g_hud;

	auto
	DrawHud() -> void {
		static const char *const TYPES[] = { "generic", "center", "pickup", "location", "planet", "corner", "tutorial", "arena_wave", "arena_reward" };
		static auto type = 0;
		static std::string text;
		static std::string sub;
		static auto duration = 3.0f;

		ImGui::Combo("Slot", &type, TYPES, IM_ARRAYSIZE(TYPES));
		ImGui::InputText("Text", &text);
		ImGui::InputTextWithHint("Sub", "optional second line", &sub);
		ImGui::SliderFloat("Seconds", &duration, 0.5f, 30.0f, "%.1f");

		ImGui::BeginDisabled(text.empty());
		if (ImGui::Button("Show")) {
			act(g_hud, [name = TYPES[type], text = text, sub = sub, duration = duration](std::string &message) {
				auto slot = hud::MessageType::Generic;
				hud::message_type(name, slot);

				const char *reason = nullptr;
				if (!hud::notify(slot, text.c_str(), duration, sub.empty() ? nullptr : sub.c_str(), &reason)) {
					message = why(reason);
					return false;
				}

				message = std::format("shown in the {} slot", name);
				return true;
			});
		}
		ImGui::EndDisabled();

		ImGui::SetItemTooltip("a new message replaces whatever that slot was showing");
		draw_message(g_hud);
	}

	// --------------------------------------------------------------- events --

	static Panel g_event_status;
	static Panel g_event_classes;
	static Panel g_event_info;
	static Panel g_event_tail;
	static Panel g_event_captures;

	static auto
	watch(Panel &panel, const std::string &name, const bool on) -> void {
		act(panel, [name, on](std::string &message) {
			const auto *info = events::find_class(name.c_str());
			if (info == nullptr) {
				message = "no event class called " + name;
				return false;
			}

			events::set_capture(info, on);
			message = (on ? "watching " : "stopped watching ") + name;
			invalidate(g_event_classes);
			invalidate(g_event_captures);
			return true;
		});
	}

	static auto
	DrawEventClasses() -> void {
		static std::string filter;
		static std::string selected;

		if (ImGui::InputTextWithHint("##filter", "filter by name", &filter)) {
			invalidate(g_event_classes);
		}

		refresh(g_event_classes, [filter = filter] {
			nlohmann::json state;
			state["classes"] = events::classes(filter.c_str(), 4000);
			state["watching"] = events::captures(nullptr, 0)["watching"];
			return state;
		}, ON_DEMAND);

		const auto height = ImGui::GetContentRegionAvail().y * 0.55f;
		constexpr auto flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_Resizable;
		if (ImGui::BeginTable("event_classes", 4, flags, ImVec2(0, height))) {
			ImGui::TableSetupScrollFreeze(0, 1);
			ImGui::TableSetupColumn("Watch", ImGuiTableColumnFlags_WidthFixed);
			ImGui::TableSetupColumn("Name");
			ImGui::TableSetupColumn("Parent");
			ImGui::TableSetupColumn("Size", ImGuiTableColumnFlags_WidthFixed);
			ImGui::TableHeadersRow();

			std::lock_guard guard { g_event_classes.lock };
			const auto &state = g_event_classes.state;
			const auto &classes = member(state, "classes");
			const auto &watching = member(state, "watching");

			ImGuiListClipper clipper;
			clipper.Begin(static_cast<int>(classes.size()));
			while (clipper.Step()) {
				for (auto row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
					const auto &entry = classes[row];
					const auto name = get<std::string>(entry, "name", "");

					ImGui::PushID(row);
					ImGui::TableNextRow();
					ImGui::TableNextColumn();

					auto on = std::find(watching.begin(), watching.end(), name) != watching.end();
					if (ImGui::Checkbox("##watch", &on)) {
						watch(g_event_info, name, on);
					}

					ImGui::TableNextColumn();
					if (ImGui::Selectable(name.c_str(), selected == name, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap)) {
						selected = name;
						invalidate(g_event_info);
					}

					ImGui::TableNextColumn();
					ImGui::TextDisabled("%s", get<std::string>(entry, "parent", "").c_str());
					ImGui::TableNextColumn();
					ImGui::Text("%u", get<uint32_t>(entry, "size", 0));
					ImGui::PopID();
				}
			}

			ImGui::EndTable();
		}

		draw_message(g_event_info);

		if (selected.empty()) {
			ImGui::TextDisabled("select a class for its fields");
			return;
		}

		refresh(g_event_info, [selected = selected] {
			return events::describe(events::find_class(selected.c_str()));
		}, ON_DEMAND);

		std::lock_guard guard { g_event_info.lock };
		const auto &info = g_event_info.state;
		if (get<std::string>(info, "name", "") != selected) {
			ImGui::TextDisabled("reading...");
			return;
		}

		std::string parents;
		for (const auto &parent : get<std::vector<std::string>>(info, "parents", {})) {
			parents += " < " + parent;
		}

		ImGui::Text("%s %s%s, %u bytes", selected.c_str(), get<std::string>(info, "hash", "").c_str(), parents.c_str(), get<uint32_t>(info, "size", 0));
		if (ImGui::BeginTable("event_fields", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_BordersInnerV)) {
			ImGui::TableSetupScrollFreeze(0, 1);
			ImGui::TableSetupColumn("Field");
			ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed);
			ImGui::TableSetupColumn("Offset", ImGuiTableColumnFlags_WidthFixed);
			ImGui::TableSetupColumn("Struct");
			ImGui::TableHeadersRow();

			for (const auto &field : get<nlohmann::json>(info, "fields", nlohmann::json::array())) {
				ImGui::TableNextRow();
				ImGui::TableNextColumn();
				ImGui::TextUnformatted(get<std::string>(field, "name", "").c_str());
				ImGui::TableNextColumn();
				ImGui::Text("%d/%d", get<int>(field, "type", 0), get<int>(field, "array_type", 0));
				ImGui::TableNextColumn();
				ImGui::Text("0x%x", get<uint32_t>(field, "offset", 0));
				ImGui::TableNextColumn();
				ImGui::TextDisabled("%s", get<std::string>(field, "struct", "").c_str());
			}

			ImGui::EndTable();
		}
	}

	static auto
	DrawEventTail() -> void {
		static std::string filter;
		static auto paused = false;

		ImGui::InputTextWithHint("##filter", "filter by class", &filter);
		ImGui::SameLine();
		ImGui::Checkbox("Pause", &paused);

		if (!paused) {
			refresh(g_event_tail, [filter = filter] {
				return events::tail(filter.c_str(), 300);
			}, 250);
		}

		constexpr auto flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_Resizable;
		if (!ImGui::BeginTable("event_tail", 6, flags)) {
			return;
		}

		ImGui::TableSetupScrollFreeze(0, 1);
		ImGui::TableSetupColumn("Seq", ImGuiTableColumnFlags_WidthFixed);
		ImGui::TableSetupColumn("Frame", ImGuiTableColumnFlags_WidthFixed);
		ImGui::TableSetupColumn("Class");
		ImGui::TableSetupColumn("Sender", ImGuiTableColumnFlags_WidthFixed);
		ImGui::TableSetupColumn("Target", ImGuiTableColumnFlags_WidthFixed);
		ImGui::TableSetupColumn("Broadcast", ImGuiTableColumnFlags_WidthFixed);
		ImGui::TableHeadersRow();

		std::lock_guard guard { g_event_tail.lock };
		const auto &tail = g_event_tail.state;
		const auto count = tail.is_array() ? static_cast<int>(tail.size()) : 0;

		// newest first
		ImGuiListClipper clipper;
		clipper.Begin(count);
		while (clipper.Step()) {
			for (auto row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
				const auto &entry = tail[count - 1 - row];
				ImGui::TableNextRow();
				ImGui::TableNextColumn();
				ImGui::Text("%llu", get<uint64_t>(entry, "sequence", 0));
				ImGui::TableNextColumn();
				ImGui::Text("%llu", get<uint64_t>(entry, "frame", 0));
				ImGui::TableNextColumn();
				ImGui::TextUnformatted(get<std::string>(entry, "class", "").c_str());
				ImGui::TableNextColumn();
				ImGui::Text("0x%08x", get<uint32_t>(entry, "sender", 0));
				ImGui::TableNextColumn();
				if (entry.contains("target")) {
					ImGui::Text("0x%08x", get<uint32_t>(entry, "target", 0));
				} else {
					ImGui::TextDisabled("%d", get<int>(entry, "targets", 0));
				}

				ImGui::TableNextColumn();
				ImGui::TextUnformatted(get<bool>(entry, "broadcast", false) ? "yes" : "");
			}
		}

		ImGui::EndTable();
	}

	static auto
	DrawEventCaptures() -> void {
		refresh(g_event_captures, [] {
			return events::captures(nullptr, 50);
		}, 250);

		static std::string name;
		ImGui::InputTextWithHint("##watch", "class name or 0xhash", &name);
		ImGui::SameLine();
		ImGui::BeginDisabled(name.empty());
		if (ImGui::Button("Watch")) {
			watch(g_event_captures, name, true);
		}
		ImGui::EndDisabled();

		draw_message(g_event_captures);

		std::lock_guard guard { g_event_captures.lock };
		const auto &state = g_event_captures.state;

		for (const auto &watched : get<std::vector<std::string>>(state, "watching", {})) {
			ImGui::PushID(watched.c_str());
			if (ImGui::SmallButton("x")) {
				watch(g_event_captures, watched, false);
			}
			ImGui::PopID();

			ImGui::SameLine();
			ImGui::TextUnformatted(watched.c_str());
		}

		const auto &captured = member(state, "events");
		if (captured.empty()) {
			ImGui::TextDisabled("nothing captured yet. watch a class, here or with the checkboxes under Classes");
			return;
		}

		if (!ImGui::BeginChild("captures", ImVec2(0, 0), ImGuiChildFlags_Borders)) {
			ImGui::EndChild();
			return;
		}

		for (auto it = captured.rbegin(); it != captured.rend(); ++it) {
			const auto sequence = get<uint64_t>(*it, "sequence", 0);
			const auto label = std::format("{}  #{}  frame {}", get<std::string>(*it, "class", ""), sequence, get<uint64_t>(*it, "frame", 0));

			ImGui::PushID(static_cast<int>(sequence));
			if (ImGui::TreeNode(label.c_str())) {
				ImGui::Text("sender 0x%08x, targets %s%s", get<uint32_t>(*it, "sender", 0), get<nlohmann::json>(*it, "targets", {}).dump().c_str(), get<bool>(*it, "broadcast", false) ? ", broadcast" : "");
				const auto fields = get<nlohmann::json>(*it, "fields", {}).dump(2, ' ', false, nlohmann::json::error_handler_t::replace);
				ImGui::TextUnformatted(fields.c_str());
				ImGui::TreePop();
			}
			ImGui::PopID();
		}

		ImGui::EndChild();
	}

	auto
	DrawEvents() -> void {
		refresh(g_event_status, [] {
			return events::status();
		}, 1000);

		const auto status = snapshot(g_event_status);
		if (!get<bool>(status, "ready", false)) {
			draw_reason(get<std::string>(status, "reason", ""));
			return;
		}

		ImGui::TextDisabled("%d classes, %llu seen, %llu queued, %llu failed", get<int>(status, "classes", 0), get<uint64_t>(status, "seen", 0), get<uint64_t>(status, "queued", 0), get<uint64_t>(status, "queue_failures", 0));

		if (!ImGui::BeginTabBar("event_tabs")) {
			return;
		}

		if (ImGui::BeginTabItem("Classes")) {
			DrawEventClasses();
			ImGui::EndTabItem();
		}

		if (ImGui::BeginTabItem("Tail")) {
			DrawEventTail();
			ImGui::EndTabItem();
		}

		if (ImGui::BeginTabItem("Captures")) {
			DrawEventCaptures();
			ImGui::EndTabItem();
		}

		ImGui::EndTabBar();
	}

	// -------------------------------------------------------------- configs --

	static Panel g_config_list;
	static Panel g_config;

	static auto
	set_config(const std::string &id, const std::string &path, const ddl::Value &value) -> void {
		act(g_config, [id, path, value](std::string &message) {
			uint64_t parsed = 0;
			configs::Config config;
			if (!configs::parse_id(id.c_str(), parsed) || !configs::find(parsed, config)) {
				message = "config " + id + " is no longer loaded";
				return false;
			}

			ddl::Value previous {};
			const char *reason = nullptr;
			if (!configs::set(config, path.c_str(), value, previous, &reason)) {
				message = path + ": " + why(reason);
				return false;
			}

			message = path + " was " + ddl::to_json(previous).dump();
			return true;
		});
	}

	// the number field being typed in. its value is held here rather than re-read
	// from the config every frame, so the frame it loses focus still has the edit
	static std::string g_edit_path;
	static double g_edit_value = 0.0;

	static auto
	DrawConfigFields(const nlohmann::json &fields, const std::string &prefix, const std::string &id) -> void {
		for (const auto &[key, value] : fields.items()) {
			const auto path = prefix.empty() ? key : prefix + "." + key;

			if (value.is_object()) {
				if (ImGui::TreeNode(key.c_str())) {
					DrawConfigFields(value, path, id);
					ImGui::TreePop();
				}

				continue;
			}

			if (value.is_boolean()) {
				auto on = value.get<bool>();
				if (ImGui::Checkbox(key.c_str(), &on)) {
					ddl::Value next {};
					next.kind = ddl::ValueKind::Bool;
					next.as_bool = on;
					set_config(id, path, next);
				}

				continue;
			}

			if (value.is_number()) {
				auto number = g_edit_path == path ? g_edit_value : value.get<double>();
				ImGui::SetNextItemWidth(200);
				if (ImGui::InputDouble(key.c_str(), &number, 0.0, 0.0, value.is_number_float() ? "%.6g" : "%.0f")) {
					g_edit_path = path;
					g_edit_value = number;
				}

				// applied on enter or when focus leaves, not on every keystroke
				if (ImGui::IsItemDeactivatedAfterEdit()) {
					ddl::Value next {};
					next.kind = ddl::ValueKind::Real;
					next.as_real = number;
					set_config(id, path, next);
					g_edit_path.clear();
				}

				continue;
			}

			// strings, arrays and whatever did not decode are read only
			auto text = value.is_string() ? value.get<std::string>() : value.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
			if (text.size() > 160) {
				text = text.substr(0, 160) + "...";
			}

			ImGui::LabelText(key.c_str(), "%s", text.c_str());
		}
	}

	auto
	DrawConfigs() -> void {
		static std::string type;
		static std::string selected;

		const auto entered = ImGui::InputTextWithHint("##type", "config class, empty for all", &type, ImGuiInputTextFlags_EnterReturnsTrue);
		ImGui::SameLine();
		if (ImGui::Button("List") || entered) {
			invalidate(g_config_list);
		}

		refresh(g_config_list, [type = type] {
			nlohmann::json state;
			state["reason"] = configs::unavailable_reason();
			state["configs"] = configs::list(type.c_str(), 10000);
			return state;
		}, ON_DEMAND);

		{
			std::lock_guard guard { g_config_list.lock };
			const auto &state = g_config_list.state;
			if (const auto reason = get<std::string>(state, "reason", "reading..."); !reason.empty()) {
				draw_reason(reason);
				return;
			}

			const auto &list = member(state, "configs");
			ImGui::SameLine();
			ImGui::TextDisabled("%zu loaded", list.size());

			if (ImGui::BeginChild("config_list", ImVec2(360, 0), ImGuiChildFlags_Borders | ImGuiChildFlags_ResizeX)) {
				ImGuiListClipper clipper;
				clipper.Begin(static_cast<int>(list.size()));
				while (clipper.Step()) {
					for (auto row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
						const auto id = get<std::string>(list[row], "id", "");
						const auto label = std::format("{}  {}##{}", get<std::string>(list[row], "type", ""), id, row);
						if (ImGui::Selectable(label.c_str(), selected == id)) {
							selected = id;
							invalidate(g_config);
						}
					}
				}
			}

			ImGui::EndChild();
		}

		ImGui::SameLine();

		if (!ImGui::BeginChild("config_fields", ImVec2(0, 0))) {
			ImGui::EndChild();
			return;
		}

		if (selected.empty()) {
			ImGui::TextDisabled("select a config for its fields");
			ImGui::EndChild();
			return;
		}

		refresh(g_config, [selected = selected] {
			nlohmann::json state;
			state["id"] = selected;

			uint64_t id = 0;
			configs::Config config;
			if (!configs::parse_id(selected.c_str(), id) || !configs::find(id, config)) {
				state["error"] = "no longer loaded";
				return state;
			}

			char name[0x100];
			state["type"] = ddl::read_string(config.type->name, name, sizeof(name)) ? name : "";
			state["fields"] = configs::values(config);
			return state;
		}, 1000);

		draw_message(g_config);
		ImGui::TextDisabled("enter applies a number. edits are in place: a component that copied a value when it started keeps its copy");

		{
			std::lock_guard guard { g_config.lock };
			const auto &state = g_config.state;
			if (get<std::string>(state, "id", "") != selected) {
				ImGui::TextDisabled("reading...");
			} else if (const auto error = get<std::string>(state, "error", ""); !error.empty()) {
				ImGui::TextDisabled("%s", error.c_str());
			} else {
				ImGui::Text("%s  %s", get<std::string>(state, "type", "").c_str(), selected.c_str());
				ImGui::Separator();
				DrawConfigFields(member(state, "fields"), "", selected);
			}
		}

		ImGui::EndChild();
	}

	// -------------------------------------------------------------- scripts --

	static Panel g_scripts;
	static Panel g_exec;

	auto
	DrawScripts() -> void {
		refresh(g_scripts, [] {
			return scripting::status();
		}, 500);

		const auto state = snapshot(g_scripts);
		if (!get<bool>(state, "enabled", g_settings.scripts.enabled)) {
			ImGui::TextDisabled("scripts are off, set [scripts] enabled in the settings");
			return;
		}

		ImGui::Text("%s from %s", get<bool>(state, "running", false) ? "running" : "not running", get<std::string>(state, "path", "").c_str());
		ImGui::SameLine();
		if (ImGui::Button("Reload")) {
			act(g_scripts, [](std::string &message) {
				scripting::request_reload();
				message = "reloading on the next pump";
				return true;
			});
		}

		ImGui::TextDisabled("last %.2fms, peak %.2fms, budget %dms, %d errors", get<double>(state, "last_ms", 0.0), get<double>(state, "peak_ms", 0.0), get<int>(state, "budget_ms", 0), get<int>(state, "errors", 0));
		ImGui::TextDisabled("%zu on_frame, %zu on_key, %zu on_event", get<nlohmann::json>(state, "on_frame", {}).size(), get<nlohmann::json>(state, "on_key", {}).size(), get<nlohmann::json>(state, "on_event", {}).size());

		if (const auto error = get<std::string>(state, "last_error", ""); !error.empty()) {
			ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.45f, 0.4f, 1.0f));
			ImGui::TextWrapped("%s", error.c_str());
			ImGui::PopStyleColor();
		}

		draw_message(g_scripts);

		ImGui::SeparatorText("Loaded");
		for (const auto &script : get<nlohmann::json>(state, "scripts", nlohmann::json::array())) {
			const auto error = get<std::string>(script, "error", "");
			ImGui::BulletText("%s  %s", get<std::string>(script, "name", "").c_str(), get<bool>(script, "ok", false) ? "ok" : error.c_str());
		}

		ImGui::SeparatorText("Exec");
		static std::string chunk;
		ImGui::SetNextItemWidth(-60);
		const auto entered = ImGui::InputTextWithHint("##exec", "lua chunk, e.g. return rivet.hero()", &chunk, ImGuiInputTextFlags_EnterReturnsTrue);
		ImGui::SameLine();
		if ((ImGui::Button("Run") || entered) && !chunk.empty()) {
			act(g_exec, [chunk = chunk](std::string &message) {
				char out[0x1000];
				const auto worked = scripting::exec(chunk.c_str(), out, sizeof(out));
				message = worked ? std::string("= ") + out : std::string(out);
				return worked;
			});
		}

		draw_message(g_exec);
	}

	// --------------------------------------------------------------- status --

	static Panel g_status;

	static auto
	feature_row(const char *name, const std::string &reason) -> void {
		ImGui::TableNextRow();
		ImGui::TableNextColumn();
		ImGui::TextUnformatted(name);
		ImGui::TableNextColumn();
		if (reason.empty()) {
			ImGui::TextUnformatted("ok");
		} else {
			ImGui::TextDisabled("%s", reason.c_str());
		}
	}

	auto
	DrawStatus() -> void {
		// atomics only, so read straight from here: this is what shows a stalled pump
		const auto pump = game_thread::status();
		ImGui::SeparatorText("Pump");
		ImGui::LabelText("Game thread hook", "%s", get<bool>(pump, "game_thread_hook", false) ? "installed" : "missing, present pumps");
		ImGui::LabelText("Game pumps", "%llu", get<uint64_t>(pump, "game_pumps", 0));
		ImGui::LabelText("Render pumps", "%llu", get<uint64_t>(pump, "render_pumps", 0));
		if (pump.contains("ms_since_game_pump") && !pump.at("ms_since_game_pump").is_null()) {
			ImGui::LabelText("Since game pump", "%llums", get<uint64_t>(pump, "ms_since_game_pump", 0));
		} else {
			ImGui::LabelText("Since game pump", "never");
		}

		ImGui::LabelText("Bridge", "%s", g_settings.bridge.enabled ? g_settings.bridge.pipe_name.c_str() : "off");
		ImGui::LabelText("Scripts", "%s", g_settings.scripts.enabled ? "on" : "off");

		refresh(g_status, [] {
			nlohmann::json state;
			state["events"] = events::ready() ? "" : events::unavailable_reason();
			state["time"] = time_scale::ready() ? "" : time_scale::unavailable_reason();
			state["fov"] = camera::fov_unavailable_reason();
			state["free"] = camera::free_unavailable_reason();
			state["configs"] = configs::unavailable_reason();
			state["hero_look"] = get<bool>(hero_look::status(), "available", false) ? "" : "the engine calls did not resolve";
			state["hero"] = scene_query::hero();
			return state;
		}, 1000);

		const auto state = snapshot(g_status);
		ImGui::SeparatorText("Features");
		if (!state.is_object()) {
			ImGui::TextDisabled("reading...");
			return;
		}

		if (ImGui::BeginTable("features", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV)) {
			ImGui::TableSetupColumn("Feature", ImGuiTableColumnFlags_WidthFixed);
			ImGui::TableSetupColumn("State");
			feature_row("Events", get<std::string>(state, "events", ""));
			feature_row("Time scale", get<std::string>(state, "time", ""));
			feature_row("FOV scale", get<std::string>(state, "fov", ""));
			feature_row("Free camera", get<std::string>(state, "free", ""));
			feature_row("Configs", get<std::string>(state, "configs", ""));
			feature_row("Hero look", get<std::string>(state, "hero_look", ""));
			feature_row("Hero", get<uint32_t>(state, "hero", 0) != 0 ? "" : "there is no hero right now");
			ImGui::EndTable();
		}
	}
} // namespace rivet_hook::overlay

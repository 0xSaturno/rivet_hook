// SPDX-FileCopyrightText: 2025-2026 Neptuwunium
//
// SPDX-License-Identifier: EUPL-1.2

#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <limits>
#include <mutex>
#include <string>

#include <imgui.h>
#include <nlohmann/json.hpp>

#include "game_thread.hpp"

// the overlay draws on the render thread, and nearly everything it drives is game
// thread only, so reads and actions go out through game_thread::post and their
// answers land in a panel for the following frames to draw
namespace rivet_hook::overlay {
	// refresh() with this interval only reads again after invalidate()
	constexpr uint64_t ON_DEMAND = (std::numeric_limits<uint64_t>::max)();

	struct Panel {
		Panel() = default;
		Panel(const Panel &) = delete;
		Panel(Panel &&) = delete;
		auto operator=(const Panel &) -> Panel & = delete;
		auto operator=(Panel &&) -> Panel & = delete;

		// guards state and message. the render thread holds it while drawing them
		std::mutex lock;
		nlohmann::json state;
		std::string message;
		bool failed = false;

		std::atomic_bool reading = false;
		std::atomic_uint64_t read_ms = 0;
	};

	// the next refresh() reads again whatever its interval
	inline auto
	invalidate(Panel &panel) -> void {
		panel.read_ms = 0;
	}

	// replaces state with what read answers on the game thread, at most once every
	// interval_ms and never with a read already in flight
	inline auto
	refresh(Panel &panel, std::function<nlohmann::json()> read, const uint64_t interval_ms) -> void {
		const auto last = panel.read_ms.load();
		if (last != 0 && (interval_ms == ON_DEMAND || GetTickCount64() - last < interval_ms)) {
			return;
		}

		if (panel.reading.exchange(true)) {
			return;
		}

		const auto posted = game_thread::post([&panel, read = std::move(read)] {
			// a throwing read must still clear reading, or the panel never reads again
			nlohmann::json state;
			try {
				state = read();
			} catch (const std::exception &) {
			}

			{
				std::lock_guard guard { panel.lock };
				panel.state = std::move(state);
			}

			panel.read_ms = GetTickCount64();
			panel.reading = false;
		});

		if (!posted) {
			panel.reading = false;
		}
	}

	// runs act on the game thread. act answers whether it worked and fills message
	// either way, which draw_message shows. the panel is read again afterwards.
	inline auto
	act(Panel &panel, std::function<bool(std::string &message)> act) -> void {
		game_thread::post([&panel, act = std::move(act)] {
			std::string message;
			auto worked = false;
			try {
				worked = act(message);
			} catch (const std::exception &failure) {
				message = std::string("exception: ") + failure.what();
			}

			{
				std::lock_guard guard { panel.lock };
				panel.message = std::move(message);
				panel.failed = !worked;
			}

			invalidate(panel);
		});
	}

	// "refused" when an engine call gave no reason
	inline auto
	why(const char *reason) -> std::string {
		return reason != nullptr ? reason : "refused";
	}

	inline auto
	draw_message(Panel &panel) -> void {
		std::lock_guard guard { panel.lock };
		if (panel.message.empty()) {
			return;
		}

		if (panel.failed) {
			ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.45f, 0.4f, 1.0f));
		} else {
			ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
		}

		ImGui::TextWrapped("%s", panel.message.c_str());
		ImGui::PopStyleColor();
	}
} // namespace rivet_hook::overlay

// SPDX-FileCopyrightText: 2025-2026 Neptuwunium
//
// SPDX-License-Identifier: EUPL-1.2

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <atomic>
#include <mutex>

#include "game_thread.hpp"

#include "bridge.hpp"
#include "ddl_visit.hpp"
#include "camera.hpp"
#include "events.hpp"
#include "runtime.hpp"
#include "scripting.hpp"
#include "signature.hpp"

namespace rivet_hook::game_thread {
	// the pass that carries the actors, and the phase after its first component
	// stages have run. the engine runs its own mid frame gui and actor code here.
	constexpr int MAIN_PASS = 1;
	constexpr int MID_FRAME_PHASE = 1;

	// how long the game thread may go without pumping before present takes over.
	// actor updates stop during loads, and the bridge should not go deaf with them.
	constexpr uint64_t QUIET_MS = 250;

	using pass_running_t = void (*)(int pass, int phase);
	static pass_running_t game_pass_running = nullptr;

	static std::once_flag g_installed;
	static std::mutex g_pump_lock;

	static std::atomic_uint64_t g_last_game_pump_ms = 0;
	static std::atomic_uint64_t g_game_pumps = 0;
	static std::atomic_uint64_t g_render_pumps = 0;
	static std::atomic_uint32_t g_game_thread_id = 0;
	static std::atomic_uint32_t g_render_thread_id = 0;

	static auto
	pump_all() -> void {
		// events first, so the scripts see what was queued since the last pump
		events::poll();
		bridge::pump();
		scripting::pump();
		camera::pump();
	}

	static auto
	pass_running(const int pass, const int phase) -> void {
		game_pass_running(pass, phase);

		if (pass != MAIN_PASS || phase != MID_FRAME_PHASE) {
			return;
		}

		g_game_thread_id = GetCurrentThreadId();
		{
			std::lock_guard guard { g_pump_lock };
			pump_all();
		}

		++g_game_pumps;
		g_last_game_pump_ms = GetTickCount64();
	}

	auto
	install() -> void {
		std::call_once(g_installed, [] {
			create_hook(ACTOR_UPDATE_PASS_RUNNING_SIGNATURE, reinterpret_cast<LPVOID>(&pass_running), reinterpret_cast<LPVOID *>(&game_pass_running));
			if (game_pass_running == nullptr) {
				g_output << "[rivet] game thread pump unavailable, scripts and the bridge stay on present\n";
			} else {
				g_output << "[rivet] scripts and the bridge pump on the game thread\n";
			}
			g_output.flush();
		});
	}

	auto
	present_tick() -> void {
		g_render_thread_id = GetCurrentThreadId();

		// the overlay walks engine state from here too, and each thread keeps its
		// own readable cache now, so this thread clears its own every frame
		ddl::reset_readable_cache();

		if (game_pass_running != nullptr && GetTickCount64() - g_last_game_pump_ms < QUIET_MS) {
			return;
		}

		// never stall the render thread waiting on a game thread pump
		std::unique_lock guard { g_pump_lock, std::try_to_lock };
		if (!guard.owns_lock()) {
			return;
		}

		pump_all();
		++g_render_pumps;
	}

	auto
	on_game_thread() -> bool {
		const auto id = g_game_thread_id.load();
		return id != 0 && id == GetCurrentThreadId();
	}

	auto
	status() -> nlohmann::json {
		const auto last = g_last_game_pump_ms.load();

		nlohmann::json result;
		result["game_thread_hook"] = game_pass_running != nullptr;
		result["game_thread_id"] = g_game_thread_id.load();
		result["render_thread_id"] = g_render_thread_id.load();
		result["game_pumps"] = g_game_pumps.load();
		result["render_pumps"] = g_render_pumps.load();
		result["ms_since_game_pump"] = last == 0 ? nlohmann::json(nullptr) : nlohmann::json(GetTickCount64() - last);
		return result;
	}
} // namespace rivet_hook::game_thread

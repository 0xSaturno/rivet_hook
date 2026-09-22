// SPDX-FileCopyrightText: 2025-2026 Neptuwunium
//
// SPDX-License-Identifier: EUPL-1.2

#pragma once

#include <cstdint>

#include <nlohmann/json.hpp>

namespace rivet_hook::game_thread {
	// hooks the engine's mid frame actor update callback so the bridge and scripts
	// pump on the game thread, between component update passes, instead of racing
	// them from present. safe to call more than once.
	auto
	install() -> void;

	// called from present every frame. pumps from the render thread only while the
	// game thread pump is missing or has gone quiet (loading screens stop actor
	// updates), so the bridge keeps answering either way.
	auto
	present_tick() -> void;

	// which thread each pump ran on and how recently, for the bridge's ping
	auto
	status() -> nlohmann::json;
} // namespace rivet_hook::game_thread

// SPDX-FileCopyrightText: 2025-2026 Neptuwunium
//
// SPDX-License-Identifier: EUPL-1.2

#pragma once

#include <cstdint>

// the game's own hud messages: the toasts and banners pickups, weapons and level
// scripts show
namespace rivet_hook::hud {
	// the game's message slots. each is one element on the hud, and a new message
	// replaces whatever that slot was showing.
	enum class MessageType : int32_t {
		Generic = 0,
		Center = 1,
		Pickup = 2,
		Location = 4,
		Planet = 5,
		Corner = 6,
		Tutorial = 7,
		ArenaWave = 8,
		ArenaReward = 9,
	};

	// resolves the hud and its message call. scans only.
	auto
	init() -> void;

	// the type for a name ("generic", "center", "pickup", "location", "planet",
	// "corner", "tutorial", "arena_wave", "arena_reward"), false when there is none
	auto
	message_type(const char *name, MessageType &out) -> bool;

	// shows a message in one of the game's hud slots for duration seconds. sub is
	// the smaller second line, null for none. game thread only. false with the
	// reason in reason when it could not be shown.
	auto
	notify(MessageType type, const char *text, float duration, const char *sub, const char **reason) -> bool;
} // namespace rivet_hook::hud

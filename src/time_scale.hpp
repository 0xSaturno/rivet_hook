// SPDX-FileCopyrightText: 2025-2026 Neptuwunium
//
// SPDX-License-Identifier: EUPL-1.2

#pragma once

#include <cstdint>

#include <nlohmann/json.hpp>

// the engine's time scale system. game speed is the lowest of 25 channels (the
// cinematic one outranks the rest), ramped toward at a per channel rate, and it
// drives the physics step too. scripts get a channel of their own to hold.
namespace rivet_hook::time_scale {
	constexpr int32_t CHANNEL_COUNT = 25;

	// resolves the system and its two setters. scans only.
	auto
	init() -> void;

	auto
	ready() -> bool;

	// why ready() answers false, as static text
	auto
	unavailable_reason() -> const char *;

	// the channel index for a name as the game spells it ("kGame") or without the
	// k ("Game", "game"), or a number as text. -1 when there is none.
	auto
	channel_index(const char *name) -> int32_t;

	// the channel's name, "" when out of range or not resolved yet
	auto
	channel_name(int32_t channel) -> const char *;

	// the scale the game is running at right now, after ramping
	auto
	applied() -> float;

	// the scale a channel is asking for
	auto
	channel_scale(int32_t channel) -> float;

	// asks for scale on a channel. ramp is how fast the game eases toward it, in
	// scale per second; negative takes the engine's default. game thread only.
	auto
	set(int32_t channel, float scale, float ramp, const char **reason) -> bool;

	// puts a channel back to normal speed, and drops whatever component was
	// driving it. game thread only.
	auto
	clear(int32_t channel, const char **reason) -> bool;

	auto
	status() -> nlohmann::json;
} // namespace rivet_hook::time_scale

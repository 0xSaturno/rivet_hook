// SPDX-FileCopyrightText: 2025-2026 Neptuwunium
//
// SPDX-License-Identifier: EUPL-1.2

#pragma once

#include <cstddef>
#include <cstdint>

#include <nlohmann/json.hpp>

// travel between planets and around them. every planet is a region of one
// level, and the level names its spawn points: checkpoints. warping to one is
// the engine's own checkpoint warp, the one level scripts use: the screen
// fades, the checkpoint's region loads if it is not the current one, and the
// hero is placed on the checkpoint's spawn point.
namespace rivet_hook::travel {
	// resolves the load system and the warp request. scans only.
	auto
	init() -> void;

	// why travel is unavailable, or empty when it is available
	auto
	unavailable_reason() -> const char *;

	// the level's checkpoints whose name contains filter (every one when it is
	// empty), as { name, hash, region, type, position }, at most limit of them.
	// game thread only.
	auto
	checkpoints(const char *filter, size_t limit) -> nlohmann::json;

	// the hash of a checkpoint the level has, by exact name or as 0x hex text,
	// or 0. game thread only.
	auto
	find(const char *name) -> uint32_t;

	// warps the hero to a checkpoint. the hero keeps who they are. moving to a
	// checkpoint also moves the save's current checkpoint, like the game's own
	// warps do. game thread only.
	auto
	warp(uint32_t checkpoint, const char **reason) -> bool;
} // namespace rivet_hook::travel

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
	// empty), as { name, hash, region, area, type, position }, at most limit of
	// them. area is the top level region the checkpoint is in, a planet or an
	// open world. game thread only.
	auto
	checkpoints(const char *filter, size_t limit) -> nlohmann::json;

	// the level's regions whose path contains filter, as { index, path, type,
	// area, zones }. type is global, container, unit, open_world, tile or overlay.
	// game thread only.
	auto
	regions(const char *filter, size_t limit) -> nlohmann::json;

	// the level's zones whose path contains filter, each with the regions that
	// load it and the route go would take: { kind, checkpoint, region }, kind
	// being loaded (always there), checkpoint (a warp), overlay (loaded on top of
	// where the hero is) or none. game thread only.
	auto
	zones(const char *filter, size_t limit) -> nlohmann::json;

	// goes where a zone is loaded: warps to the checkpoint its route names, or
	// loads its overlay region. zone is its path, its asset id as 16 hex digits,
	// or a fragment only one zone path contains. describes what it did in
	// message. game thread only.
	auto
	go(const char *zone, char *message, size_t message_size, const char **reason) -> bool;

	// the ship's travel, to any checkpoint: hands the ship's planet menu listener
	// the same accept the menu would, with a planet tunnel (a CHK_TRANSITION_TO_
	// checkpoint) and our destination. the ship's own script then takes off, flies
	// the tunnel and lands on the destination. needs a ship nearby; the listener
	// whose screen is nearest the hero is used. via names the tunnel; empty picks
	// the one of the destination's planet. game thread only.
	auto
	fly(const char *destination, const char *via, char *message, size_t message_size, const char **reason) -> bool;

	// a rift: the game's passive shift, the one that pulls the hero through a rift,
	// through an airlock and out at the far end, landing them. borrows four of
	// the rift portals loaded here: one opens next to the hero and pulls them in,
	// one near another rift stands in for the airlock, and the last is moved to
	// the target. target is a checkpoint name, or x y z with checkpoint empty. the
	// borrowed portals are put back once the shift is over, deactivated.
	// game thread only.
	auto
	rift(const char *checkpoint, const float *position, char *message, size_t message_size, const char **reason) -> bool;

	// puts borrowed rift portals back once the shift that used them is over
	auto
	pump() -> void;

	// the planet tunnels the level has, the CHK_TRANSITION_TO_ checkpoints
	auto
	tunnels() -> nlohmann::json;

	// loads an overlay region on top of whatever is loaded, or unloads one. region
	// is its index, its path, or a fragment only one overlay path contains. game
	// thread only.
	auto
	overlay(const char *region, bool load, char *message, size_t message_size, const char **reason) -> bool;

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

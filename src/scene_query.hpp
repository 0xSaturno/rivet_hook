// SPDX-FileCopyrightText: 2025-2026 Neptuwunium
//
// SPDX-License-Identifier: EUPL-1.2

#pragma once

#include <cstdint>

#include "game/actor.hpp"

// lookups that answer from the engine's own indexes instead of walking every
// actor slot. game thread only, like everything else that reads the scene.
namespace rivet_hook::scene_query {
	// the handle of an actor in the scene array, 0 if it is not in it
	auto
	handle_of(const game::Actor *actor) -> uint32_t;

	// the actor the game treats as the player, 0 while there is none
	auto
	hero() -> uint32_t;

	// the actor's uid, 0 when it has none. zone placed actors have bit 63 set and
	// keep their uid across loads and launches, which handles do not; spawned
	// actors, the hero among them, get a runtime uid without it.
	auto
	uid_of(const game::Actor *actor) -> uint64_t;

	// the live actor with this uid, 0 when none is loaded
	auto
	actor_by_uid(uint64_t uid) -> uint32_t;

	// a registered component class by exact name, null if there is none
	auto
	find_class(const char *name) -> const game::ComponentInfo *;

	// handles of the actors holding a live component of this class, or of a class
	// derived from it when derived is set. each actor is listed once. stops early
	// and returns -1 once expired() answers true, which is checked every so often.
	auto
	actors_with(const game::ComponentInfo *type, bool derived, uint32_t *out, int32_t max, bool (*expired)()) -> int32_t;
} // namespace rivet_hook::scene_query

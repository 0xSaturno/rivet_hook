// SPDX-FileCopyrightText: 2025-2026 Neptuwunium
//
// SPDX-License-Identifier: EUPL-1.2

#pragma once

#include <string>

#include "game/actor.hpp"
#include "game/ddl.hpp"

namespace rivet_hook {
	// draws the live prius data behind a component as a table of ddl fields.
	// draws nothing when the component has no prius, and says so when the
	// instance is not readable.
	auto
	DrawComponentPrius(const game::ComponentInfo *type, const game::Component *instance) -> void;

	// writes an actor, its components and their live prius data to
	// ./rivet_actor_<name>.json in the game directory. returns the path, or an
	// empty string when the file could not be written.
	auto
	DumpActor(const game::Actor *actor) -> std::string;
} // namespace rivet_hook

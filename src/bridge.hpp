// SPDX-FileCopyrightText: 2025-2026 Neptuwunium
//
// SPDX-License-Identifier: EUPL-1.2

#pragma once

#include <cstddef>

namespace rivet_hook::bridge {
	// starts the named pipe server when [bridge] enabled is set. the server only
	// parses requests, it never touches engine state itself.
	auto
	init() -> void;

	auto
	fini() -> void;

	// runs queued requests that need engine state. called once per presented frame,
	// which is the only place it is safe to walk the scene.
	auto
	pump() -> void;

	// installs, or retargets, one of the generic component update detours, and sets
	// whether it skips the body. shared with the lua binding so there is one pool
	// of these and one set of call counters, not two. on failure error receives the
	// reason. engine thread only.
	auto
	set_detour(const char *component, const char *slot, bool skip, char *error, size_t error_size) -> bool;
} // namespace rivet_hook::bridge


// SPDX-FileCopyrightText: 2025-2026 Neptuwunium
//
// SPDX-License-Identifier: EUPL-1.2

#pragma once

// the engine's camera system
namespace rivet_hook::camera {
	// resolves what the camera bindings touch. scans only.
	auto
	init() -> void;

	// why the fov scale is unavailable, "" when it is not
	auto
	fov_unavailable_reason() -> const char *;

	// the multiplier on every camera's field of view. the game's own fov slider
	// writes it (1.25 at the slider's default is common), and writes it again
	// whenever the graphics settings are applied, which undoes a change made here.
	auto
	fov_scale() -> float;

	// false with the reason in reason when the value is out of range or the field
	// was not found
	auto
	set_fov_scale(float scale, const char **reason) -> bool;
} // namespace rivet_hook::camera

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

	// where a camera is and where it looks. yaw and pitch are degrees: yaw 0 looks
	// down +Z and turns toward +X, pitch is positive looking up. fov is the field
	// of view in degrees as the camera node stores it, before the fov scale.
	struct View {
		float position[3] {};
		float yaw = 0.0f;
		float pitch = 0.0f;
		float fov = 0.0f;
	};

	// why the free camera is unavailable, "" when it is not
	auto
	free_unavailable_reason() -> const char *;

	// the camera the player sees: the free camera while detached, the game's
	// otherwise. false when the camera system is not readable.
	auto
	view(View &out) -> bool;

	// detaches the view from the game. the renderer takes its view from a camera
	// node the hook owns, which starts where the game's camera is; gameplay keeps
	// running on the game's own camera underneath, so aiming and camera relative
	// controls do not follow the free camera. sound does.
	auto
	detach(const char **reason) -> bool;

	// hands the view back to the game
	auto
	attach() -> void;

	auto
	detached() -> bool;

	// moves the free camera. only while detached. fov <= 0 keeps the current one.
	auto
	set_view(const View &view, const char **reason) -> bool;

	// overrides whether camera shake reaches the player's view. the game copies
	// its camera shake option into the same bit every frame, so an override is
	// held and put back on every pump until it is released. shakes the game
	// forces through ignore the bit either way.
	auto
	block_shake(bool blocked, const char **reason) -> bool;

	// hands the shake bit back to the game's option
	auto
	release_shake() -> void;

	// whether shake is blocked right now, and whether that is our override
	auto
	shake_blocked() -> bool;

	auto
	shake_overridden() -> bool;

	// re-applies a held override. called once a pump.
	auto
	pump() -> void;
} // namespace rivet_hook::camera

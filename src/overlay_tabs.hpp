// SPDX-FileCopyrightText: 2025-2026 Neptuwunium
//
// SPDX-License-Identifier: EUPL-1.2

#pragma once

// the overlay tabs over what the lua bindings and the pipe already drive. each
// reads and acts through game_thread::post, see overlay_panel.hpp
namespace rivet_hook::overlay {
	// hero_look and vanity
	auto
	DrawHero() -> void;

	// time scale, fov scale, free camera, camera shake
	auto
	DrawCameraTime() -> void;

	// the game's own hud messages
	auto
	DrawHud() -> void;

	// event classes, the live tail, and watched captures
	auto
	DrawEvents() -> void;

	// loaded config assets, their fields editable in place
	auto
	DrawConfigs() -> void;

	// lua status, reload, and a one line exec
	auto
	DrawScripts() -> void;

	// which thread pumps and how recently
	auto
	DrawStatus() -> void;
} // namespace rivet_hook::overlay

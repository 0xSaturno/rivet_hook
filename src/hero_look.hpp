// SPDX-FileCopyrightText: 2025-2026 Neptuwunium
//
// SPDX-License-Identifier: EUPL-1.2

#pragma once

#include <cstdint>

#include <nlohmann/json.hpp>

// the hero wearing another actor asset's model, with its own gameplay untouched.
// the same steps the engine's transformation takes for the model (a scene object
// from the target asset, then SwitchModel on the hero's ModelInst), without the
// hero type, abilities, voice or component re-init that come with it.
namespace rivet_hook::hero_look {
	// resolves the engine calls. scans only.
	auto
	init() -> void;

	enum class Result {
		Failed,
		Applied,
		// the actor asset is still loading, the next pump that sees it loaded
		// applies it
		Loading,
	};

	// puts the model of the .actor at path, or the .model at path, on the hero,
	// loading it first when it is not loaded. with anims (actor assets only), the
	// asset's anim sets go on top of the hero's once the model has switched. game
	// thread only.
	auto
	request(const char *path, bool anims, const char **reason) -> Result;

	// the hero's own model and vanity parts back, as the engine rebuilds them after
	// a transformation. game thread only.
	auto
	restore(const char **reason) -> bool;

	// applies a request whose asset finished loading. called once a pump.
	auto
	pump() -> void;

	// what is worn, what is pending, and whether the calls resolved
	auto
	status() -> nlohmann::json;

	// HeroTypes by name: ratchet 0, clank 1, rivet 2, kit 3 (the enum spellings,
	// kRatchette and so on, too). -1 when it is none of them
	auto
	hero_type(const char *name) -> int32_t;

	// the game's own hero swap, as TriggerHeroSwap makes it: the hero becomes
	// that hero type, gameplay and all, after its actor asset is loaded. a worn
	// look is dropped first. game thread only.
	auto
	play_as(int32_t type, const char **reason) -> Result;

	// whether the remembered look goes back on once the hero first appears after
	// a launch. saved to rivet.toml. game thread only.
	auto
	set_apply_on_launch(bool on) -> void;

	// the looks there are to wear: { mods: [{ path, mod }], game: [{ path, name }] },
	// each list narrowed to paths containing filter (any case)
	auto
	models(const char *filter) -> nlohmann::json;
} // namespace rivet_hook::hero_look

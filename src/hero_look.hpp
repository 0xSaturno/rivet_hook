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

	// puts the model of the actor asset at path on the hero, loading the asset
	// first when it is not loaded. game thread only.
	auto
	request(const char *path, const char **reason) -> Result;

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
} // namespace rivet_hook::hero_look

// SPDX-FileCopyrightText: 2025-2026 Neptuwunium
//
// SPDX-License-Identifier: EUPL-1.2

#pragma once

#include <cstdint>

// actors spawned on request from any actor asset, at a position, a yaw and a
// scale, activated, listed and destroyed again. the engine calls are the ones
// the game's own spawns make: the actor spawn the rift portals come from, then
// Actor::Activate, and Actor::Destroy to take one away. only actors spawned
// here are ever destroyed here.
namespace rivet_hook::spawn {
	// how many spawned actors, loading ones included, there can be at once
	constexpr int32_t MAX_SPAWNED = 256;

	enum class State {
		// the actor asset is still loading, the pump spawns the actor once it is in
		Loading,
		Active,
		// spawned, but the engine did not activate it
		Inactive,
		// asked to go; the scene destroys it at the start of an actor update
		Destroying,
	};

	// plain data, so a lua_CFunction can hold one across a call that may raise
	struct Info {
		uint32_t handle; // 0 while loading
		uint64_t uid;
		State state;
		uint32_t flags; // ActorFlag, 0 while loading
		// where the actor is and the scale its scene object has; while loading,
		// what was asked for
		float position[3];
		float scale[3];
		float yaw; // degrees, as asked for
		char path[0x100];
	};

	enum class Result {
		Failed,
		Spawned,
		// out holds the uid the actor will have, and no handle yet
		Loading,
	};

	// resolves the engine calls. scans only.
	auto
	init() -> void;

	// why spawning is unavailable, or empty when it is available
	auto
	unavailable_reason() -> const char *;

	// spawns the .actor at path and activates it, loading the asset first when it
	// is not loaded. yaw is in degrees about the up axis, 0 facing +z and turning
	// toward +x like the camera's; scale is per axis. the position has to be
	// within 4096 of the origin on every axis. game thread only.
	auto
	request(const char *path, const float *position, float yaw, const float *scale, Info &out, const char **reason) -> Result;

	// asks the engine to destroy an actor request spawned, deactivating it first
	// so it is gone from the world at once. any other handle is refused. game
	// thread only.
	auto
	destroy(uint32_t handle, const char **reason) -> bool;

	// the same for every spawned actor, and drops the ones still loading, which
	// is the only way to call one of those off. destroyed is how many this call
	// asked to go or dropped, the ones going or gone already not counted. game
	// thread only.
	auto
	destroy_all(int32_t &destroyed, const char **reason) -> bool;

	// the spawned actors, those the engine has destroyed since left out. at most
	// max of them; returns how many
	auto
	list(Info *out, int32_t max) -> int32_t;

	// "loading", "active", "inactive" or "destroying"
	auto
	state_name(State state) -> const char *;

	// why the last spawn that was waiting for its asset was dropped, or empty
	auto
	last_error() -> const char *;

	// spawns the actors whose asset finished loading, and drops the ones that
	// waited 30 seconds or through a level change. called once a pump, on
	// whichever thread it runs.
	auto
	pump() -> void;
} // namespace rivet_hook::spawn

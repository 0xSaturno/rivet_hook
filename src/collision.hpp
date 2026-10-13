// SPDX-FileCopyrightText: 2025-2026 Neptuwunium
//
// SPDX-License-Identifier: EUPL-1.2

#pragma once

#include <cstdint>

#include <nlohmann/json.hpp>

// mesh collision made while the game runs. experimental: none of it has run
// in the shipped game before the hook did it.
//
// the game ships its collision cooked, and nothing here writes that format.
// the hook hands triangles to the Havok mesh shape builder that is linked into
// the exe, and gets a static body for the result the way a zone gets its own:
// it describes the body and puts the description in the queue zones load their
// physics through. the engine then makes the body, adds it to the world and
// destroys it again, at the points in the frame where it does that for a zone.
// the hook never touches the physics world itself.
//
// every step is behind a stage, [collision] stage in rivet.toml or
// collision.stage for the session, and each stage adds one kind of risk:
//   0  off, the default. nothing is scanned for and nothing is called
//   1  reads only: status and inspect
//   2  the first engine calls: constructors on hook buffers, a heap round trip
//   3  a second body out of a loaded zone's own shape, through the queue
//   4  the mesh cook, with no body
//   5  a cooked shape as a body
// a command above the stage is refused. a fault in an engine call is caught
// and switches the whole feature off until the game restarts, since what the
// engine was left with is not known. catching one takes msvc's structured
// exceptions: any other build of the hook stops at stage 1.
//
// every answer is json, the same for the bridge and for lua. game thread only
// except where it says otherwise.
namespace rivet_hook::collision {
	constexpr int32_t MAX_STAGE = 5;
	// how many bodies, and how many cooked shapes, one session can make. what
	// the engine read of a body is kept until the game exits, see collision.cpp
	constexpr int32_t MAX_BODIES = 256;
	constexpr int32_t MAX_SHAPES = 256;
	// collision.cook's default: the gameplay flags most shipped ground has
	constexpr uint32_t DEFAULT_FLAGS = 0x43F;

	// reads the stage out of the settings, and resolves the engine calls when it
	// is not 0. scans only. a stage the exe or this build does not have is
	// taken as 1.
	auto
	init() -> void;

	// { stage, max_stage, available, reason, measured_build, faults_caught,
	// latched, last_error }. measured_build is whether the exe is the one the
	// layouts were measured on, null until the engine calls were looked for;
	// faults_caught whether this build of the hook catches a fault in an engine
	// call. any thread.
	auto
	stage_info() -> nlohmann::json;

	// sets the stage for this session, rivet.toml keeps what it has. the first
	// time it leaves 0 the engine calls are resolved, which scans the exe once
	// and can be felt as a hitch. it does not go below 3 while a body is still
	// followed: nothing would finish its removal. inside the pump, like every
	// command.
	auto
	set_stage(int32_t stage, const char **reason) -> bool;

	// stage 1. what was resolved and what the engine has right now: signature
	// matches, cross checks, the class names behind the vtables, the Havok
	// memory router of the calling thread, the world, the enable and pause
	// state, the queues, the stage and the last error. answers on any thread and
	// with the feature unavailable or switched off, that being what it is for.
	auto
	status(nlohmann::json &out, const char **reason) -> bool;

	// stage 1. walks the physics of a loaded zone and reads it out: the zone
	// slot, the system the engine made, its system data, materials and body
	// cinfos, the bodies in the world, and the shape tree with types, user data,
	// boxes and triangle counts. zone as travel::go takes it. reads only.
	auto
	inspect(const char *zone, nlohmann::json &out, const char **reason) -> bool;

	// stage 2. what is "ctor" (the three constructors the later stages rely on,
	// each on a hook buffer and compared byte for byte with what it is known to
	// write), "heap" (one block off the calling thread's Havok heap, written,
	// read and given back), or empty for both, the heap only when the
	// constructors passed.
	auto
	selftest(const char *what, nlohmann::json &out, const char **reason) -> bool;

	// stage 3. a second static body out of the root shape of a loaded zone,
	// offset from the zone's own by x y z, queued for the engine to create. the
	// shape stays the zone's: the body has to go before the zone does. a zone
	// placed by a matrix is refused, its own body is not where its description
	// was written.
	auto
	borrow(const char *zone, const float *offset, nlohmann::json &out, const char **reason) -> bool;

	// stage 3. the bodies made here that are not gone yet, each with its state:
	// queued until the engine has made it, created until a physics step has it
	// in the world, live, disabling and disabled, removing, and orphaned, broken
	// or lost for one that is left alone, with the reason as its error. a body
	// is lost when the physics world is another than the one it was made in, or
	// its system another than the engine made for it: no call is made on it.
	auto
	bodies(nlohmann::json &out, const char **reason) -> bool;

	// stage 3. queues taking a body out of the world or putting it back, the two
	// queues a zone's physics is switched off and on through.
	auto
	enable(int32_t body, bool on, nlohmann::json &out, const char **reason) -> bool;

	// stage 3. removes a body for good: out of the world first, as enable does
	// it, and once the engine has done that, the queue that lets go of its
	// system, which destroys the body. the pump takes the second step.
	auto
	remove(int32_t body, nlohmann::json &out, const char **reason) -> bool;

	// the same for every body that can be removed right now
	auto
	remove_all(nlohmann::json &out, const char **reason) -> bool;

	// stage 4. cooks a mesh into a Havok mesh shape, with no body. mesh is
	// "quad:<size>", a flat square of that many metres around the origin, or the
	// path of a Wavefront .obj, relative to the game's folder unless absolute:
	// its v and f lines are read, triangles only. flags are the gameplay flags
	// every triangle gets. the answer has what was measured on the new shape as
	// the builder left it, what the hook wrote into it after, and whether it
	// passed the checks a shape has to pass to be used.
	auto
	cook(const char *mesh, uint32_t flags, nlohmann::json &out, const char **reason) -> bool;

	// stage 4. the cooked shapes with their reference counts, as read right now
	auto
	shapes(nlohmann::json &out, const char **reason) -> bool;

	// stage 4. gives up the hook's reference on a cooked shape no body uses,
	// which frees it
	auto
	release(int32_t shape, nlohmann::json &out, const char **reason) -> bool;

	// the same for every shape no body uses
	auto
	release_all(nlohmann::json &out, const char **reason) -> bool;

	// stage 5. a static body out of a cooked shape at a position, turned yaw
	// degrees about the up axis, queued like a borrowed one and listed, switched
	// and removed the same way.
	auto
	add(int32_t shape, const float *position, float yaw, nlohmann::json &out, const char **reason) -> bool;

	// why the hook should not unload a zone right now, or with travel start a
	// warp, a flight or a rift, and null when nothing made here is in the way.
	// a body made of a loaded zone's shape reads that zone's physics file for as
	// long as it exists, so the zone must not be unloaded under it. travel is
	// refused for any body: none has been through a planet change yet. the text
	// says what to do about it, which is a restart once a body cannot be removed
	// any more. inside the pump.
	auto
	in_the_way(bool travel) -> const char *;

	// watches the physics world, on whichever thread the pump runs, and on the
	// game thread follows the bodies through the engine's queues and takes the
	// second step of a removal. called once a pump.
	auto
	pump() -> void;
} // namespace rivet_hook::collision

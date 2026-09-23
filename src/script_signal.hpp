// SPDX-FileCopyrightText: 2025-2026 Neptuwunium
//
// SPDX-License-Identifier: EUPL-1.2

#pragma once

#include <cstddef>
#include <cstdint>

#include <nlohmann/json.hpp>

// level scripting. a zone's scripts are ScriptAction components wired together by
// plugs, and firing an input plug is one entry in the engine's signal queue,
// processed later in the frame like any signal the zone sends itself.
namespace rivet_hook::script_signal {
	// resolves the signal queue. scans only.
	auto
	init() -> void;

	// the engine's string hash, the one plug names, event and class names are
	// hashed with: a reflected crc32 seeded with 0xedb88320 and no final xor
	auto
	hash(const char *text) -> uint32_t;

	// a plug hash from a name, or from hex text starting 0x
	auto
	plug_hash(const char *text) -> uint32_t;

	// the handle of the nth (0 based) live component of that exact class on an
	// actor, or 0. game thread only.
	auto
	find_node(uint32_t actor, const char *component_class, int32_t nth, const char **reason) -> uint32_t;

	// the level script nodes loaded right now, as { actor, uid, class }, for
	// classes whose name contains filter (every node when it is empty). each node
	// is the only component of an actor that has no scene object, so the scans
	// that want a placed actor never see one. stops after limit nodes or once
	// expired() answers true, and says so in truncated.
	auto
	nodes(const char *filter, size_t limit, bool (*expired)()) -> nlohmann::json;

	// fires an input plug on a script node component. game thread only.
	auto
	send(uint32_t component, uint32_t input_plug, const char **reason) -> bool;
} // namespace rivet_hook::script_signal

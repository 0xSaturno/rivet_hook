// SPDX-FileCopyrightText: 2025-2026 Neptuwunium
//
// SPDX-License-Identifier: EUPL-1.2

#pragma once

#include <cstddef>
#include <cstdint>

#include <nlohmann/json.hpp>

#include "game/script.hpp"

// level scripting. a zone's scripts are ScriptAction components wired together by
// plugs, and firing an input plug is one entry in the engine's signal queue,
// processed later in the frame like any signal the zone sends itself.
namespace rivet_hook::script_signal {
	// resolves the signal queue and loads the plug names. scans only.
	auto
	init() -> void;

	// the name of a plug hash, or null. the engine keeps only hashes, so names
	// come from a short built-in list of generic ones plus plug_names.txt in the
	// game folder, one name per line (the last word of a line is taken, so a
	// "hash<tab>name" table works as is, and # starts a comment)
	auto
	plug_name(uint32_t hash) -> const char *;

	// how many plug names are known
	auto
	plug_name_count() -> size_t;

	constexpr int32_t MAX_NODE_PLUGS = 255; // the plug counts are 8 bit
	constexpr int32_t MAX_NODE_INPUTS = 128;
	constexpr int32_t MAX_VAR_ACTORS = 8;

	// another node a plug connects to
	struct NodeRef {
		uint32_t component = 0;
		uint32_t actor = 0;
		char name[64] {}; // its class, empty when it is gone
	};

	struct NodeOutput {
		uint32_t plug = 0;
		uint32_t target_plug = 0;
		NodeRef target;
	};

	// a connection arriving at the node, found by scanning every node's outputs
	struct NodeInput {
		uint32_t plug = 0;
		uint32_t source_plug = 0;
		NodeRef source;
	};

	struct NodeVar {
		uint32_t plug = 0;
		uint32_t handle = 0; // { u16 index, u16 generation }
		bool live = false;	 // the handle still resolves
		game::ScriptVarType type {};
		// engine driven: the engine refreshes the value before a node reads it,
		// so what is stored can be stale
		bool dynamic = false;
		bool as_bool = false;
		float numbers[3] {};
		char text[128] {};
		char name[64] {}; // only globals are named
		// actors: a group, a single actor, or a uid not resolved yet
		bool group = false;
		uint64_t actor_uid = 0;
		int32_t actor_count = 0;
		uint32_t actors[MAX_VAR_ACTORS] {};
	};

	// one node and its wiring, copied out of the scene: plain data, so lua can
	// build its table from it after nothing is left to unwind
	struct Node {
		NodeRef self;
		uint64_t uid = 0;
		uint64_t graph_uid = 0;
		uint64_t zone = 0;
		int32_t in_count = 0; // connections arriving, as the node counts them
		int32_t output_count = 0;
		NodeOutput outputs[MAX_NODE_PLUGS];
		int32_t var_count = 0;
		NodeVar vars[MAX_NODE_PLUGS];
		int32_t input_count = 0;
		NodeInput inputs[MAX_NODE_INPUTS];
		// the scan for arriving connections ran out of time or room
		bool inputs_truncated = false;
	};

	// reads a node component: its outputs and where they go, its vars and their
	// values, and with inputs set the connections arriving at it, which takes a
	// walk over every loaded node (~16ms with 10k nodes loaded). game thread
	// only. false with reason when it is not a live node.
	auto
	inspect(uint32_t component, Node &out, bool inputs, bool (*expired)(), const char **reason) -> bool;

	// the node as json, plus its prius fields under properties (null when the
	// node keeps none)
	auto
	to_json(const Node &node) -> nlohmann::json;

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

	// the level script nodes loaded right now, as { component, actor, uid, class },
	// for classes whose name contains filter (every node when it is empty). nodes
	// live on actors without a scene object, most alone and some types packed
	// onto one shared actor, so the scans that want a placed actor never see one.
	// stops after limit nodes or once expired() answers true, and says so in
	// truncated.
	auto
	nodes(const char *filter, size_t limit, bool (*expired)()) -> nlohmann::json;

	// fires an input plug on a script node component. game thread only.
	auto
	send(uint32_t component, uint32_t input_plug, const char **reason) -> bool;
} // namespace rivet_hook::script_signal

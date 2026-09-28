// SPDX-FileCopyrightText: 2025-2026 Neptuwunium
//
// SPDX-License-Identifier: EUPL-1.2

#pragma once

#include <cstddef>
#include <cstdint>

#include "actor.hpp"

// level script nodes, as the zone loader builds them. offsets measured on the
// shipping exe: the plug block is filled right after the node component is
// created, and every plug send and var read goes through it.
namespace rivet_hook::game {
#pragma pack(push, 1)

	// one output connection. a node can wire one output plug to several targets,
	// each is its own entry with the same plug hash.
	struct ScriptOutPlug {
		uint32_t plug;		  // this node's output plug name hash
		uint32_t targetPlug;  // the input plug it fires on the target
		EngineHandle target;  // the target node component
	};

	static_assert(sizeof(ScriptOutPlug) == 0xc, "ScriptOutPlug size is not 0xc");

	// one variable connection. a var handle is { u16 index, u16 generation } into
	// the scene's script var array; generation 0 is never live
	struct ScriptVarPlug {
		uint32_t plug;
		uint16_t varIndex;
		uint16_t varGeneration;
	};

	static_assert(sizeof(ScriptVarPlug) == 0x8, "ScriptVarPlug size is not 0x8");

	// one allocation: this header, then outCount output plugs, then varCount var
	// plugs. input plugs are not stored, only how many connections arrive; which
	// plugs they are is only known from the senders' output plugs.
	struct ScriptPlugs {
		ScriptOutPlug *outPlugs;
		ScriptVarPlug *varPlugs;
		void *varGroups; // allocated on first use, one per var plug
		uint8_t inCount;
		uint8_t outCount;
		uint8_t varCount;
		uint8_t varGroupCount;
		uint64_t zone; // asset id of the zone that loaded the node, packed
		EngineHandle owner;
	};

	static_assert(sizeof(ScriptPlugs) == 0x28, "ScriptPlugs size is not 0x28");
	static_assert(offsetof(ScriptPlugs, inCount) == 0x18, "ScriptPlugs inCount offset is not 0x18");
	static_assert(offsetof(ScriptPlugs, owner) == 0x24, "ScriptPlugs owner offset is not 0x24");

	// every node class derives from ScriptAction, which adds these to Component
	struct ScriptAction {
		Component base;
		uint64_t instanceUid; // the node actor's uid
		uint64_t graphUid;	  // the graph node it was made from, shared by every instance of a subgraph
		ScriptPlugs *plugs;
	};

	static_assert(offsetof(ScriptAction, instanceUid) == 0x48, "ScriptAction instanceUid offset is not 0x48");
	static_assert(offsetof(ScriptAction, plugs) == 0x58, "ScriptAction plugs offset is not 0x58");

	enum class ScriptVarType : uint16_t {
		None = 0,
		Bool = 1,
		Float = 2,
		Vector = 3,
		String = 4,
		Actors = 5,
	};

	constexpr auto
	ScriptVarTypeName(const ScriptVarType type) -> const char * {
		switch (type) {
			case ScriptVarType::None: return "none";
			case ScriptVarType::Bool: return "bool";
			case ScriptVarType::Float: return "float";
			case ScriptVarType::Vector: return "vector";
			case ScriptVarType::String: return "string";
			case ScriptVarType::Actors: return "actors";
			default: return "unknown";
		}
	}

	// ScriptVar::actors with this bit set is an actor group handle, id in the low
	// 20 bits and generation in the next 11. without it, a plain actor handle.
	constexpr uint32_t SCRIPT_VAR_GROUP_BIT = 1u << 31;

	struct ScriptVar {
		union {
			bool asBool;
			float asFloat;
			float asVector[3];
			const char *asString;
			uint64_t actorUid; // actors: resolved into actors on first read
			uint8_t raw[0x10];
		};

		uint32_t actors;
		uint32_t unknown1;
		// set for engine driven globals. the engine calls it before every read,
		// so the stored value can lag behind what a node would see
		void *dynamicFunc;
		uint64_t unknown2;
		const char *name; // only globals are named
		uint16_t unknown3;
		uint16_t flags;
		ScriptVarType type;
		uint16_t generation;
		uint64_t unknown4;
	};

	static_assert(sizeof(ScriptVar) == 0x40, "ScriptVar size is not 0x40");
	static_assert(offsetof(ScriptVar, actors) == 0x10, "ScriptVar actors offset is not 0x10");
	static_assert(offsetof(ScriptVar, dynamicFunc) == 0x18, "ScriptVar dynamicFunc offset is not 0x18");
	static_assert(offsetof(ScriptVar, name) == 0x28, "ScriptVar name offset is not 0x28");
	static_assert(offsetof(ScriptVar, type) == 0x34, "ScriptVar type offset is not 0x34");

#pragma pack(pop)
} // namespace rivet_hook::game

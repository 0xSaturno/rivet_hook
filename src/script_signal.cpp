// SPDX-FileCopyrightText: 2025-2026 Neptuwunium
//
// SPDX-License-Identifier: EUPL-1.2

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <unordered_map>

#include "script_signal.hpp"

#include "ddl_visit.hpp"
#include "game/scene_manager.hpp"
#include "game_thread.hpp"
#include "runtime.hpp"
#include "scene_query.hpp"
#include "signature.hpp"

using namespace rivet_hook::game;

namespace rivet_hook {
	// overlay_core owns this, it is resolved once during Overlay::Init
	extern SceneManager *g_SceneManager;
} // namespace rivet_hook

namespace rivet_hook::script_signal {
	// the engine's seed. its table is the standard reflected one
	constexpr uint32_t HASH_SEED = 0xedb88320;
	constexpr uint32_t CRC_POLY = 0xedb88320;

	// source handle for a signal nothing in the zone sent
	constexpr uint32_t NO_SOURCE = 0;
	// output plug hash for the same
	constexpr uint32_t NO_OUTPUT = 0;

	using add_entry_t = bool (*)(void *queue, const uint32_t *component, uint32_t input_plug, uint32_t output_plug, uint32_t source);

	static void *g_queue = nullptr;
	static add_entry_t g_add_entry = nullptr;

	static auto
	load_plug_names() -> void;

	auto
	init() -> void {
		static auto names_loaded = false;
		if (!names_loaded) {
			names_loaded = true;
			load_plug_names();
		}

		if (g_queue != nullptr) {
			return;
		}

		const auto site = find_address(SCRIPT_SIGNAL_SEND_SIGNATURE);
		g_queue = load_rel_var(site, SCRIPT_SIGNAL_QUEUE_ADDRESS);
		g_add_entry = reinterpret_cast<add_entry_t>(load_rel_var(site, SCRIPT_SIGNAL_ADD_ENTRY_ADDRESS));
		if (g_queue == nullptr || g_add_entry == nullptr) {
			g_queue = nullptr;
			g_output << "[script] the signal queue was not found, rivet.signal is unavailable\n";
		} else {
			g_output << "[script] signal queue at " << g_queue << "\n";
		}

		g_output.flush();
	}

	static auto
	table() -> const std::array<uint32_t, 256> & {
		static const auto built = [] {
			std::array<uint32_t, 256> out {};
			for (uint32_t i = 0; i < 256; ++i) {
				auto c = i;
				for (auto bit = 0; bit < 8; ++bit) {
					c = (c & 1) != 0 ? (c >> 1) ^ CRC_POLY : c >> 1;
				}

				out[i] = c;
			}

			return out;
		}();

		return built;
	}

	auto
	hash(const char *text) -> uint32_t {
		if (text == nullptr || text[0] == '\0') {
			return 0;
		}

		const auto &lookup = table();
		auto crc = HASH_SEED;
		for (const auto *c = reinterpret_cast<const uint8_t *>(text); *c != 0; ++c) {
			crc = (crc >> 8) ^ lookup[*c ^ (crc & 0xff)];
		}

		return crc;
	}

	auto
	plug_hash(const char *text) -> uint32_t {
		if (text != nullptr && text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
			char *end = nullptr;
			const auto value = strtoul(text, &end, 16);
			if (end != text + 2 && *end == '\0') {
				return static_cast<uint32_t>(value);
			}
		}

		return hash(text);
	}

	// generic words, enough to label common wiring without a table
	constexpr const char *BUILTIN_PLUG_NAMES[] = {
		"In", "Out", "Start", "Stop", "Reset", "Enable", "Disable", "Done", "Finished", "Complete",
		"True", "False", "On", "Off", "Activate", "Deactivate", "Show", "Hide", "Open", "Close",
		"Trigger", "Pause", "Resume", "Cancel", "Next", "Actor", "Actors", "Value", "Target", "Count",
	};

	// written once by init, read only afterwards
	static std::unordered_map<uint32_t, std::string> g_plug_names;

	static auto
	add_plug_name(const std::string &name) -> void {
		if (!name.empty()) {
			g_plug_names.try_emplace(hash(name.c_str()), name);
		}
	}

	static auto
	load_plug_names() -> void {
		for (const auto *name : BUILTIN_PLUG_NAMES) {
			add_plug_name(name);
		}

		std::ifstream file { "./plug_names.txt" };
		if (!file.is_open()) {
			return;
		}

		const auto before = g_plug_names.size();
		std::string line;
		while (std::getline(file, line)) {
			if (const auto comment = line.find('#'); comment != std::string::npos) {
				line.resize(comment);
			}

			// the last word, so a "hash<tab>name" table needs no conversion. the
			// hash is always computed here, whatever the file says it is
			const auto end = line.find_last_not_of(" \t\r\n");
			if (end == std::string::npos) {
				continue;
			}

			const auto space = line.find_last_of(" \t", end);
			const auto start = space == std::string::npos ? 0 : space + 1;
			add_plug_name(line.substr(start, end - start + 1));
		}

		g_output << "[script] " << g_plug_names.size() - before << " plug names from plug_names.txt\n";
		g_output.flush();
	}

	auto
	plug_name(const uint32_t plug) -> const char * {
		const auto found = g_plug_names.find(plug);
		return found != g_plug_names.end() ? found->second.c_str() : nullptr;
	}

	auto
	plug_name_count() -> size_t {
		return g_plug_names.size();
	}

	static auto
	engine_thread(const char **reason) -> bool {
		if (g_queue == nullptr) {
			if (reason != nullptr) {
				*reason = "the signal queue was not found";
			}

			return false;
		}

		// the queue has no lock: the game thread is the only one that writes it
		if (!game_thread::on_game_thread()) {
			if (reason != nullptr) {
				*reason = "signals can only be sent on the game thread, and it is not pumping (loading?)";
			}

			return false;
		}

		return true;
	}

	auto
	find_node(const uint32_t actor_handle, const char *component_class, const int32_t nth, const char **reason) -> uint32_t {
		const auto fail = [reason](const char *why) -> uint32_t {
			if (reason != nullptr) {
				*reason = why;
			}

			return 0;
		};

		if (!engine_thread(reason)) {
			return 0;
		}

		if (g_SceneManager == nullptr) {
			return fail("the scene manager is not available");
		}

		EngineHandle handle {};
		handle.value = actor_handle;
		const auto *actor = g_SceneManager->ResolveActor(handle);
		if (actor == nullptr) {
			return fail("no actor for that handle");
		}

		if (actor->components == nullptr || actor->componentCount <= 0 || !ddl::is_readable(actor->components, sizeof(ComponentPointer) * actor->componentCount)) {
			return fail("the actor has no readable component list");
		}

		auto seen = 0;
		for (auto index = 0; index < actor->componentCount; ++index) {
			const auto [type, instance] = actor->components[index];
			if (type == nullptr || instance == nullptr || !ddl::is_readable(type, sizeof(ComponentInfo))) {
				continue;
			}

			char name[0x100];
			if (!ddl::read_string(type->name, name, sizeof(name)) || strcmp(name, component_class) != 0) {
				continue;
			}

			if (!ddl::is_readable(instance, sizeof(Component)) || instance->IsDestroyed()) {
				continue;
			}

			if (seen++ == nth) {
				return instance->handle.value;
			}
		}

		return fail(seen == 0 ? "the actor has no live component of that class" : "the actor has fewer components of that class");
	}

	static auto
	script_action_class() -> const ComponentInfo * {
		static const ComponentInfo *found = nullptr;
		if (found == nullptr) {
			found = scene_query::find_class("ScriptAction");
		}

		return found;
	}

	// the class is ScriptAction or derives from it. type is already checked readable
	static auto
	is_script_node(const ComponentInfo *type) -> bool {
		const auto *base = script_action_class();
		return base != nullptr && (type == base || type->DerivesFrom(base));
	}

	// visit(handle, actor, class, node) for every live node, until it answers
	// false. false when expired() cut the walk short.
	template <typename Visit>
	static auto
	for_each_node(Visit &&visit, bool (*expired)()) -> bool {
		if (g_SceneManager == nullptr || g_SceneManager->actors == nullptr || script_action_class() == nullptr) {
			return true;
		}

		const auto count = g_SceneManager->actorMax;
		if (!ddl::is_readable(g_SceneManager->actors, sizeof(Actor))) {
			return true;
		}

		for (int32_t index = 0; index < count; ++index) {
			if ((index & 0x3ff) == 0 && expired != nullptr && expired()) {
				return false;
			}

			const auto *actor = &g_SceneManager->actors[index];
			// not Actor::IsValid: that wants a scene object, which script actors lack
			if (actor->generation == 0 || (actor->flags & ActorFlag::Allocated) == 0) {
				continue;
			}

			// most nodes have an actor to themselves, but some node types share
			// one per zone. neither kind has a scene object, and skipping placed
			// actors with a full component list keeps the walk cheap
			if (actor->object != nullptr && actor->componentCount > 4) {
				continue;
			}

			if (actor->components == nullptr || actor->componentCount <= 0 || !ddl::is_readable(actor->components, sizeof(ComponentPointer) * actor->componentCount)) {
				continue;
			}

			const auto handle = EngineHandle { .id = static_cast<uint32_t>(index), .generation = actor->generation }.value;
			for (auto c = 0; c < actor->componentCount; ++c) {
				const auto [type, instance] = actor->components[c];
				if (type == nullptr || instance == nullptr || !ddl::is_readable(type, sizeof(ComponentInfo)) || !is_script_node(type)) {
					continue;
				}

				if (!ddl::is_readable(instance, sizeof(ScriptAction)) || instance->IsDestroyed()) {
					continue;
				}

				if (!visit(handle, actor, type, reinterpret_cast<const ScriptAction *>(instance))) {
					return true;
				}
			}
		}

		return true;
	}

	auto
	nodes(const char *filter, const size_t limit, bool (*expired)()) -> nlohmann::json {
		nlohmann::json::array_t found;
		auto truncated = false;

		const auto finished = for_each_node([&](const uint32_t handle, const Actor *, const ComponentInfo *type, const ScriptAction *node) {
			char name[0x100];
			if (!ddl::read_string(type->name, name, sizeof(name))) {
				return true;
			}

			if (filter != nullptr && filter[0] != '\0' && strstr(name, filter) == nullptr) {
				return true;
			}

			if (found.size() >= limit) {
				truncated = true;
				return false;
			}

			// the node's own: a shared actor carries the uid of whichever of its
			// nodes loaded last
			char uid[24];
			_snprintf_s(uid, sizeof(uid), _TRUNCATE, "%016llx", node->instanceUid);

			nlohmann::json entry;
			entry["component"] = node->base.handle.value;
			entry["actor"] = handle;
			entry["uid"] = uid;
			entry["class"] = name;
			found.emplace_back(std::move(entry));
			return true;
		}, expired);

		nlohmann::json out;
		out["nodes"] = found;
		out["truncated"] = truncated || !finished;
		return out;
	}

	// ------------------------------------------------------------- inspect --

	// the class of a component, from its actor's list, or null
	static auto
	class_of(const Component *instance) -> const ComponentInfo * {
		const auto *actor = instance->actor;
		if (actor == nullptr || !ddl::is_readable(actor, sizeof(Actor))) {
			return nullptr;
		}

		if (actor->components == nullptr || actor->componentCount <= 0 || !ddl::is_readable(actor->components, sizeof(ComponentPointer) * actor->componentCount)) {
			return nullptr;
		}

		for (auto c = 0; c < actor->componentCount; ++c) {
			if (actor->components[c].instance == instance) {
				const auto *type = actor->components[c].componentType;
				return type != nullptr && ddl::is_readable(type, sizeof(ComponentInfo)) ? type : nullptr;
			}
		}

		return nullptr;
	}

	// the live component behind a handle, with its actor and class filled into
	// out, or null
	static auto
	describe(const uint32_t component, NodeRef &out) -> const Component * {
		out = {};
		out.component = component;

		const auto handle = EngineHandle { .value = component };
		if (component == 0 || g_SceneManager == nullptr || g_SceneManager->components == nullptr || static_cast<int32_t>(handle.id) >= g_SceneManager->componentMax) {
			return nullptr;
		}

		if (!ddl::is_readable(&g_SceneManager->components[handle.id], sizeof(SceneComponent))) {
			return nullptr;
		}

		const auto *instance = g_SceneManager->ResolveComponent(handle);
		if (instance == nullptr || !ddl::is_readable(instance, sizeof(Component)) || instance->IsDestroyed()) {
			return nullptr;
		}

		out.actor = scene_query::handle_of(instance->actor);
		if (const auto *type = class_of(instance); type != nullptr) {
			ddl::read_string(type->name, out.name, sizeof(out.name));
		}

		return instance;
	}

	static auto
	read_actors(const ScriptVar *var, NodeVar &out) -> void {
		out.actor_uid = var->actorUid;

		const auto value = var->actors;
		if (value == 0) {
			// not resolved from the uid yet
			return;
		}

		if ((value & SCRIPT_VAR_GROUP_BIT) == 0) {
			out.actor_count = 1;
			out.actors[0] = value;
			return;
		}

		out.group = true;
		const auto id = value & 0xfffff;
		const auto generation = (value >> 20) & 0x7ff;
		if (g_SceneManager->actorGroups == nullptr || static_cast<int32_t>(id) >= g_SceneManager->actorGroupMax) {
			return;
		}

		const auto *group = &g_SceneManager->actorGroups[id];
		if (!ddl::is_readable(group, sizeof(ActorGroup)) || group->type != generation) {
			return;
		}

		out.actor_count = group->count;
		const auto shown = (std::min)(static_cast<int32_t>(group->count), MAX_VAR_ACTORS);
		if (shown > 0 && group->handles != nullptr && ddl::is_readable(group->handles, sizeof(EngineHandle) * shown)) {
			for (auto i = 0; i < shown; ++i) {
				out.actors[i] = group->handles[i].value;
			}
		}
	}

	static auto
	read_var(const ScriptVarPlug &plug, NodeVar &out) -> void {
		out = {};
		out.plug = plug.plug;
		out.handle = plug.varIndex | (static_cast<uint32_t>(plug.varGeneration) << 16);

		if (plug.varGeneration == 0 || g_SceneManager->scriptVars == nullptr || plug.varIndex >= g_SceneManager->scriptVarMax) {
			return;
		}

		const auto *var = &g_SceneManager->scriptVars[plug.varIndex];
		if (!ddl::is_readable(var, sizeof(ScriptVar)) || var->generation != plug.varGeneration) {
			return;
		}

		out.live = true;
		out.type = var->type;
		out.dynamic = var->dynamicFunc != nullptr;
		if (var->name != nullptr) {
			ddl::read_string(var->name, out.name, sizeof(out.name));
		}

		switch (var->type) {
			case ScriptVarType::Bool:
				out.as_bool = var->asBool;
				break;
			case ScriptVarType::Float:
				out.numbers[0] = var->asFloat;
				break;
			case ScriptVarType::Vector:
				memcpy(out.numbers, var->asVector, sizeof(out.numbers));
				break;
			case ScriptVarType::String:
				if (var->asString != nullptr) {
					ddl::read_string(var->asString, out.text, sizeof(out.text));
				}
				break;
			case ScriptVarType::Actors:
				read_actors(var, out);
				break;
			default:
				break;
		}
	}

	// the node's plug block, or null when it is not readable in full
	static auto
	plugs_of(const ScriptAction *node) -> const ScriptPlugs * {
		const auto *plugs = node->plugs;
		if (plugs == nullptr || !ddl::is_readable(plugs, sizeof(ScriptPlugs))) {
			return nullptr;
		}

		if (plugs->outCount > 0 && (plugs->outPlugs == nullptr || !ddl::is_readable(plugs->outPlugs, sizeof(ScriptOutPlug) * plugs->outCount))) {
			return nullptr;
		}

		if (plugs->varCount > 0 && (plugs->varPlugs == nullptr || !ddl::is_readable(plugs->varPlugs, sizeof(ScriptVarPlug) * plugs->varCount))) {
			return nullptr;
		}

		return plugs;
	}

	auto
	inspect(const uint32_t component, Node &out, const bool inputs, bool (*expired)(), const char **reason) -> bool {
		const auto fail = [reason](const char *why) {
			if (reason != nullptr) {
				*reason = why;
			}

			return false;
		};

		out.output_count = 0;
		out.var_count = 0;
		out.input_count = 0;
		out.inputs_truncated = false;

		if (!game_thread::on_game_thread()) {
			return fail("nodes can only be read on the game thread, and it is not pumping (loading?)");
		}

		if (g_SceneManager == nullptr) {
			return fail("the scene manager is not available");
		}

		const auto *instance = describe(component, out.self);
		if (instance == nullptr) {
			return fail("no live component for that handle");
		}

		const auto *type = class_of(instance);
		if (type == nullptr || !is_script_node(type) || !ddl::is_readable(instance, sizeof(ScriptAction))) {
			return fail("that component is not a script node");
		}

		const auto *node = reinterpret_cast<const ScriptAction *>(instance);
		out.uid = node->instanceUid;
		out.graph_uid = node->graphUid;

		const auto *plugs = plugs_of(node);
		if (plugs == nullptr) {
			return fail("the node's plugs are not readable");
		}

		out.zone = plugs->zone;
		out.in_count = plugs->inCount;

		for (auto i = 0; i < plugs->outCount; ++i) {
			const auto &plug = plugs->outPlugs[i];
			auto &entry = out.outputs[out.output_count++];
			entry.plug = plug.plug;
			entry.target_plug = plug.targetPlug;
			describe(plug.target.value, entry.target);
		}

		for (auto i = 0; i < plugs->varCount; ++i) {
			read_var(plugs->varPlugs[i], out.vars[out.var_count++]);
		}

		if (!inputs) {
			return true;
		}

		// which input plugs a node has is only written in its senders
		const auto finished = for_each_node([&](uint32_t, const Actor *, const ComponentInfo *, const ScriptAction *source) {
			const auto *sent = plugs_of(source);
			if (sent == nullptr) {
				return true;
			}

			for (auto i = 0; i < sent->outCount; ++i) {
				const auto &plug = sent->outPlugs[i];
				if (plug.target.value != component) {
					continue;
				}

				if (out.input_count >= MAX_NODE_INPUTS) {
					out.inputs_truncated = true;
					return false;
				}

				auto &entry = out.inputs[out.input_count++];
				entry.plug = plug.targetPlug;
				entry.source_plug = plug.plug;
				describe(source->base.handle.value, entry.source);
			}

			return true;
		}, expired);

		if (!finished) {
			out.inputs_truncated = true;
		}

		return true;
	}

	static auto
	hex(const uint64_t value, const int digits) -> std::string {
		char text[24];
		_snprintf_s(text, sizeof(text), _TRUNCATE, "0x%0*llx", digits, value);
		return text;
	}

	static auto
	plug_json(nlohmann::json &entry, const char *key, const uint32_t plug) -> void {
		entry[key] = hex(plug, 8);

		const auto *name = plug_name(plug);
		entry[std::string(key) + "_name"] = name != nullptr ? nlohmann::json(name) : nlohmann::json(nullptr);
	}

	static auto
	ref_json(const NodeRef &ref) -> nlohmann::json {
		nlohmann::json out;
		out["component"] = ref.component;
		out["actor"] = ref.actor;
		out["class"] = ref.name[0] != '\0' ? nlohmann::json(ref.name) : nlohmann::json(nullptr);
		return out;
	}

	static auto
	var_value(const NodeVar &var) -> nlohmann::json {
		switch (var.type) {
			case ScriptVarType::Bool: return var.as_bool;
			case ScriptVarType::Float: return var.numbers[0];
			case ScriptVarType::Vector: return nlohmann::json::array({ var.numbers[0], var.numbers[1], var.numbers[2] });
			case ScriptVarType::String: return var.text;
			case ScriptVarType::Actors: {
				nlohmann::json actors;
				actors["group"] = var.group;
				actors["uid"] = hex(var.actor_uid, 16);
				actors["count"] = var.actor_count;
				nlohmann::json::array_t handles;
				for (auto i = 0; i < var.actor_count && i < MAX_VAR_ACTORS; ++i) {
					handles.emplace_back(var.actors[i]);
				}

				actors["actors"] = handles;
				return actors;
			}
			default: return nullptr;
		}
	}

	// the prius fields the node was set up with, if it keeps them
	static auto
	properties(const uint32_t component) -> nlohmann::json {
		NodeRef ref;
		const auto *instance = describe(component, ref);
		if (instance == nullptr || instance->ddlPriusData == nullptr) {
			return nullptr;
		}

		const auto *type = class_of(instance);
		const auto *prius = type != nullptr ? type->prius : nullptr;
		if (prius == nullptr || !ddl::is_readable(prius, sizeof(DDLTypeInfo)) || prius->field_count <= 0) {
			return nullptr;
		}

		const auto *data = ddl::prius_data(instance->ddlPriusData, prius->allocation_size);
		return data != nullptr ? ddl::values_of(prius, data) : nlohmann::json(nullptr);
	}

	auto
	to_json(const Node &node) -> nlohmann::json {
		nlohmann::json out;
		out["component"] = node.self.component;
		out["actor"] = node.self.actor;
		out["class"] = node.self.name;
		out["uid"] = hex(node.uid, 16);
		out["graph_uid"] = hex(node.graph_uid, 16);
		out["zone"] = hex(node.zone, 16);
		out["connections_in"] = node.in_count;

		nlohmann::json::array_t inputs;
		for (auto i = 0; i < node.input_count; ++i) {
			const auto &input = node.inputs[i];
			nlohmann::json entry;
			plug_json(entry, "plug", input.plug);
			entry["from"] = ref_json(input.source);
			plug_json(entry, "from_plug", input.source_plug);
			inputs.emplace_back(std::move(entry));
		}

		out["inputs"] = inputs;
		out["inputs_truncated"] = node.inputs_truncated;

		nlohmann::json::array_t outputs;
		for (auto i = 0; i < node.output_count; ++i) {
			const auto &output = node.outputs[i];
			nlohmann::json entry;
			plug_json(entry, "plug", output.plug);
			entry["to"] = ref_json(output.target);
			plug_json(entry, "to_plug", output.target_plug);
			outputs.emplace_back(std::move(entry));
		}

		out["outputs"] = outputs;

		nlohmann::json::array_t vars;
		for (auto i = 0; i < node.var_count; ++i) {
			const auto &var = node.vars[i];
			nlohmann::json entry;
			plug_json(entry, "plug", var.plug);
			entry["var"] = hex(var.handle, 8);
			entry["live"] = var.live;
			if (var.live) {
				entry["type"] = ScriptVarTypeName(var.type);
				entry["value"] = var_value(var);
				entry["dynamic"] = var.dynamic;
				entry["name"] = var.name[0] != '\0' ? nlohmann::json(var.name) : nlohmann::json(nullptr);
			}

			vars.emplace_back(std::move(entry));
		}

		out["vars"] = vars;
		out["properties"] = properties(node.self.component);
		return out;
	}

	// the engine call on its own, so a fault is caught with nothing to unwind
	static auto
	call_add_entry(const uint32_t component, const uint32_t input_plug, bool *queued) -> bool {
#ifdef _MSC_VER
		__try {
#endif
			*queued = g_add_entry(g_queue, &component, input_plug, NO_OUTPUT, NO_SOURCE);
			return true;
#ifdef _MSC_VER
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			return false;
		}
#endif
	}

	auto
	send(const uint32_t component, const uint32_t input_plug, const char **reason) -> bool {
		if (!engine_thread(reason)) {
			return false;
		}

		if (component == 0 || input_plug == 0) {
			if (reason != nullptr) {
				*reason = "there is no component or no plug to signal";
			}

			return false;
		}

		// the queue hands the signal to the node's HandleSignal, a virtual only
		// script nodes have
		NodeRef ref;
		const auto *instance = describe(component, ref);
		const auto *type = instance != nullptr ? class_of(instance) : nullptr;
		if (type == nullptr || !is_script_node(type)) {
			if (reason != nullptr) {
				*reason = "that component is not a live script node";
			}

			return false;
		}

		bool queued = false;
		if (!call_add_entry(component, input_plug, &queued)) {
			if (reason != nullptr) {
				*reason = "AddEntry faulted";
			}

			return false;
		}

		if (!queued) {
			if (reason != nullptr) {
				*reason = "the signal queue is full this frame";
			}

			return false;
		}

		return true;
	}
} // namespace rivet_hook::script_signal

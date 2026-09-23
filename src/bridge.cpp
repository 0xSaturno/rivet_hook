// SPDX-FileCopyrightText: 2025-2026 Neptuwunium
//
// SPDX-License-Identifier: EUPL-1.2

#include <intrin.h>

#include <atomic>
#include <cmath>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "bridge.hpp"

#include "ddl_inspector.hpp"
#include "ddl_visit.hpp"
#include "events.hpp"
#include "time_scale.hpp"
#include "camera.hpp"
#include "hud.hpp"
#include "vanity.hpp"
#include "configs.hpp"
#include "script_signal.hpp"
#include "game/scene_manager.hpp"
#include "game_thread.hpp"
#include "scene_query.hpp"
#include "watch.hpp"
#include "scripting.hpp"
#include "signature.hpp"
#include "runtime.hpp"

using namespace rivet_hook::game;

namespace rivet_hook {
	// overlay_core owns these, they are resolved once during Overlay::Init
	extern SceneManager *g_SceneManager;
} // namespace rivet_hook

namespace rivet_hook::bridge {
	// requests are framed as a little endian uint32 length followed by that many
	// utf8 bytes, in both directions, so a multi megabyte actor dump needs no
	// special casing
	constexpr uint32_t MAX_REQUEST = 0x10000;
	constexpr DWORD RESPONSE_TIMEOUT_MS = 10000;

	static std::thread g_thread;
	static std::atomic_bool g_running = false;
	static HANDLE g_pipe = INVALID_HANDLE_VALUE;

	// one request in flight: the pipe thread fills request and waits, pump runs it
	static std::mutex g_lock;
	static std::string g_request;
	static std::string g_response;
	static bool g_pending = false;
	static HANDLE g_done = nullptr;

	static auto
	error(const std::string_view message) -> std::string {
		nlohmann::json out;
		out["ok"] = false;
		out["error"] = message;
		return out.dump();
	}

	static auto
	ok(nlohmann::json result) -> std::string {
		nlohmann::json out;
		out["ok"] = true;
		out["result"] = std::move(result);
		return out.dump(2, ' ', false, nlohmann::json::error_handler_t::replace);
	}

	static auto
	split(const std::string &line) -> std::vector<std::string> {
		std::vector<std::string> parts;
		size_t start = 0;
		while (start < line.size()) {
			const auto end = line.find(' ', start);
			if (end == std::string::npos) {
				parts.emplace_back(line.substr(start));
				break;
			}

			if (end > start) {
				parts.emplace_back(line.substr(start, end - start));
			}

			start = end + 1;
		}

		return parts;
	}

	static auto
	parse_handle(const std::string &text, EngineHandle &handle) -> bool {
		try {
			handle.value = static_cast<uint32_t>(std::stoul(text, nullptr, 0));
		} catch (const std::exception &) {
			return false;
		}

		return true;
	}

	// ------------------------------------------------------- engine thread --

	// uids as hex text: they run past what a json number survives in most readers
	static auto
	uid_text(const uint64_t uid) -> nlohmann::json {
		if (uid == 0) {
			return nullptr;
		}

		char text[24];
		sprintf_s(text, "%016llx", uid);
		return text;
	}

	static auto
	cmd_scene_actors(const std::vector<std::string> &args) -> std::string {
		if (g_SceneManager == nullptr || g_SceneManager->actors == nullptr) {
			return error("scene manager is not available");
		}

		// an optional substring filter, since a loaded level has thousands of actors
		const auto *filter = args.size() > 1 ? args[1].c_str() : nullptr;
		const auto limit = args.size() > 2 ? std::stoi(args[2]) : 200;

		const auto count = g_SceneManager->actorMax;
		if (count <= 0) {
			return error("scene has no actors");
		}

		// the actors array is one engine allocation: validate it once rather than
		// paying a VirtualQuery per actor, which is thousands of syscalls a frame
		if (!ddl::is_readable(g_SceneManager->actors, sizeof(Actor))) {
			return error("actor array is not readable");
		}

		// this runs on the render thread. during heavy streaming a full walk of
		// 70k actors measured 8.3 seconds, so the scan gets a hard time budget.
		constexpr uint64_t SCAN_BUDGET_MS = 250;
		const auto started = GetTickCount64();
		auto truncated = false;

		int32_t matched = 0;
		int32_t scanned = 0;
		nlohmann::json::array_t actors;
		for (int32_t index = 0; index < count; ++index) {
			if ((index & 0x3FF) == 0 && GetTickCount64() - started > SCAN_BUDGET_MS) {
				truncated = true;
				break;
			}

			++scanned;

			const auto *actor = &g_SceneManager->actors[index];
			if (!actor->IsValid()) {
				continue;
			}

			char name[0x100];
			if (!ddl::read_string(actor->GetName(), name, sizeof(name))) {
				name[0] = '\0';
			}

			if (filter != nullptr && strstr(name, filter) == nullptr) {
				continue;
			}

			++matched;
			if (static_cast<int>(actors.size()) >= limit) {
				continue;
			}

			nlohmann::json entry;
			entry["handle"] = EngineHandle { .id = static_cast<uint32_t>(index), .generation = actor->generation }.value;
			entry["index"] = index;
			entry["generation"] = actor->generation;
			entry["name"] = name;
			entry["uid"] = uid_text(scene_query::uid_of(actor));
			entry["flags"] = DescribeActorFlags(actor->flags);
			entry["components"] = actor->componentCount;
			entry["update_children"] = actor->updateChildrenCount;
			actors.emplace_back(entry);
		}

		nlohmann::json result;
		result["returned"] = actors.size();
		result["matched"] = matched;
		result["total"] = count;
		result["live"] = g_SceneManager->actorCount;
		result["scanned"] = scanned;
		result["truncated"] = truncated;
		result["limit"] = limit;
		result["actors"] = actors;
		return ok(result);
	}

	static auto
	cmd_actor_dump(const std::vector<std::string> &args) -> std::string {
		if (g_SceneManager == nullptr) {
			return error("scene manager is not available");
		}

		if (args.size() < 2) {
			return error("usage: actor.dump <handle>");
		}

		EngineHandle handle {};
		if (!parse_handle(args[1], handle)) {
			return error("could not parse handle");
		}

		const auto *actor = g_SceneManager->ResolveActor(handle);
		if (actor == nullptr) {
			return error("no actor for that handle");
		}

		const auto path = DumpActor(actor);
		if (path.empty()) {
			return error("dump failed, see rivet.log");
		}

		nlohmann::json result;
		result["path"] = path;
		return ok(result);
	}

	// cheap read of one actor, without the cost of a full component dump
	static auto
	cmd_actor_get(const std::vector<std::string> &args) -> std::string {
		if (g_SceneManager == nullptr) {
			return error("scene manager is not available");
		}

		if (args.size() < 2) {
			return error("usage: actor.get <handle>");
		}

		EngineHandle handle {};
		if (!parse_handle(args[1], handle)) {
			return error("could not parse handle");
		}

		const auto *actor = g_SceneManager->ResolveActor(handle);
		if (actor == nullptr) {
			return error("no actor for that handle");
		}

		char name[0x100];
		if (!ddl::read_string(actor->GetName(), name, sizeof(name))) {
			name[0] = '\0';
		}

		nlohmann::json result;
		result["handle"] = handle.value;
		result["name"] = name;
		result["uid"] = uid_text(scene_query::uid_of(actor));
		result["generation"] = actor->generation;
		result["flags"] = DescribeActorFlags(actor->flags);
		result["components"] = actor->componentCount;

		if (actor->object != nullptr && ddl::is_readable(actor->object, sizeof(SceneObject))) {
			result["position"] = { actor->object->transform_matrix[3][0], actor->object->transform_matrix[3][1], actor->object->transform_matrix[3][2] };
			result["scale"] = { actor->object->scale.x, actor->object->scale.y, actor->object->scale.z };
			result["last_modified_frame"] = actor->object->lastModifiedOnFrame;
		}

		return ok(result);
	}

	// the first write this bridge does. the overlay already pokes this exact field
	// from its Update button, so the target is known good.
	static auto
	cmd_actor_set_position(const std::vector<std::string> &args) -> std::string {
		if (g_SceneManager == nullptr) {
			return error("scene manager is not available");
		}

		if (args.size() < 5) {
			return error("usage: actor.set_position <handle> <x> <y> <z>");
		}

		EngineHandle handle {};
		if (!parse_handle(args[1], handle)) {
			return error("could not parse handle");
		}

		float position[3];
		for (auto i = 0; i < 3; ++i) {
			try {
				position[i] = std::stof(args[2 + i]);
			} catch (const std::exception &) {
				return error("could not parse a coordinate");
			}

			if (!std::isfinite(position[i])) {
				return error("coordinates must be finite");
			}
		}

		auto *actor = g_SceneManager->ResolveActor(handle);
		if (actor == nullptr) {
			return error("no actor for that handle");
		}

		if (actor->object == nullptr || !ddl::is_readable(actor->object, sizeof(SceneObject))) {
			return error("actor has no readable scene object");
		}

		nlohmann::json result;
		result["was"] = { actor->object->transform_matrix[3][0], actor->object->transform_matrix[3][1], actor->object->transform_matrix[3][2] };

		// the hero is warped the way the game warps it, which sticks. the event is
		// delivered later in the frame, so "now" still shows where it was
		if (handle.value == scene_query::hero() && events::ready()) {
			const char *reason = nullptr;
			if (!events::warp(handle.value, position, &reason)) {
				return error(std::string("could not warp the hero: ") + (reason != nullptr ? reason : "refused"));
			}

			result["method"] = "warp";
			result["to"] = { position[0], position[1], position[2] };
			return ok(result);
		}

		result["method"] = "write";
		memcpy(&actor->object->transform_matrix[3], position, sizeof(position));

		result["now"] = { actor->object->transform_matrix[3][0], actor->object->transform_matrix[3][1], actor->object->transform_matrix[3][2] };
		result["frame"] = actor->object->lastModifiedOnFrame;
		return ok(result);
	}

	static auto
	cmd_actor_uid(const std::vector<std::string> &args) -> std::string {
		if (g_SceneManager == nullptr) {
			return error("scene manager is not available");
		}

		if (args.size() < 2) {
			return error("usage: actor.uid <uid hex>");
		}

		uint64_t uid = 0;
		try {
			uid = std::stoull(args[1], nullptr, 16);
		} catch (const std::exception &) {
			return error("could not parse the uid, it has to be hex");
		}

		const auto handle = scene_query::actor_by_uid(uid);
		if (handle == 0) {
			return error("no loaded actor has that uid");
		}

		return cmd_actor_get({ "actor.get", std::to_string(handle) });
	}

	static auto
	cmd_actor_hero() -> std::string {
		if (g_SceneManager == nullptr) {
			return error("scene manager is not available");
		}

		const auto handle = scene_query::hero();
		if (handle == 0) {
			return error("there is no hero right now");
		}

		return cmd_actor_get({ "actor.get", std::to_string(handle) });
	}

	// the same hard budget scene.actors answers to
	static uint64_t g_scan_deadline = 0;

	static auto
	scan_expired() -> bool {
		return GetTickCount64() > g_scan_deadline;
	}

	// actors holding a component of a class, from the engine's component index
	static auto
	cmd_find_component(const std::vector<std::string> &args) -> std::string {
		constexpr int32_t MAX_FOUND = 1024;

		if (g_SceneManager == nullptr) {
			return error("scene manager is not available");
		}

		if (args.size() < 2) {
			return error("usage: scene.find_component <exact class name> [limit] [exact]");
		}

		auto limit = 200;
		if (args.size() > 2) {
			try {
				limit = std::stoi(args[2]);
			} catch (const std::exception &) {
				return error("could not parse limit");
			}
		}

		if (limit < 1 || limit > MAX_FOUND) {
			return error("limit must be between 1 and 1024");
		}

		const auto exact = args.size() > 3 && args[3] == "exact";

		const auto *type = scene_query::find_class(args[1].c_str());
		if (type == nullptr) {
			return error("no component class by that name");
		}

		const auto started = GetTickCount64();
		g_scan_deadline = started + 250;
		uint32_t found[MAX_FOUND];
		const auto count = scene_query::actors_with(type, !exact, found, limit, scan_expired);

		nlohmann::json::array_t actors;
		for (int32_t i = 0; i < count; ++i) {
			const auto *actor = g_SceneManager->ResolveActor(EngineHandle { .value = found[i] });
			char name[0x100];
			if (actor == nullptr || !ddl::read_string(actor->GetName(), name, sizeof(name))) {
				name[0] = '\0';
			}

			nlohmann::json entry;
			entry["handle"] = found[i];
			entry["name"] = name;
			actors.emplace_back(entry);
		}

		nlohmann::json result;
		result["class"] = args[1];
		result["derived"] = !exact;
		result["truncated"] = count < 0;
		result["returned"] = actors.size();
		result["ms"] = GetTickCount64() - started;
		result["actors"] = actors;
		return ok(result);
	}

	// hardware write watchpoint. a leading + makes the address relative to the
	// game module, so an rva from a disassembler can be used as is.
	static auto
	cmd_mem_watch(const std::vector<std::string> &args) -> std::string {
		if (args.size() < 2) {
			return error("usage: mem.watch <address|+rva|off> [length|exec]");
		}

		if (args[1] == "off") {
			watch::disarm();
			return ok(watch::results());
		}

		uintptr_t address = 0;
		uint32_t length = 4;
		const auto execute = args.size() > 2 && args[2] == "exec";
		try {
			const auto relative = args[1][0] == '+';
			address = static_cast<uintptr_t>(std::stoull(relative ? args[1].substr(1) : args[1], nullptr, 0));
			if (relative) {
				address += reinterpret_cast<uintptr_t>(g_game_module);
			}

			if (args.size() > 2 && !execute) {
				length = static_cast<uint32_t>(std::stoul(args[2], nullptr, 0));
			}
		} catch (const std::exception &) {
			return error("could not parse address or length");
		}

		std::string reason;
		if (!watch::arm(address, length, execute, reason)) {
			return error(reason);
		}

		return ok(watch::results());
	}

	// the per component type update callbacks live at the head of ComponentInfo.
	// read only: this tells us whether neutralising an update is even viable
	// before anything tries it.
	static auto
	cmd_component_info(const std::vector<std::string> &args) -> std::string {
		if (args.size() < 2) {
			return error("usage: component.info <name substring>");
		}

		const auto *registry_var = load_rel_var(find_address(COMPONENT_REGISTER_SIGNATURE), COMPONENT_REGISTRY_ADDRESS);
		const auto *count_var = load_rel_var(find_address(COMPONENT_REGISTER_SIGNATURE), COMPONENT_COUNT_ADDRESS);
		if (registry_var == nullptr || count_var == nullptr) {
			return error("could not resolve the component registry");
		}

		const auto *const *infos = *static_cast<const ComponentInfo *const *const *>(registry_var);
		const auto count = *static_cast<const int32_t *>(count_var);
		if (infos == nullptr || count <= 0) {
			return error("component registry is empty");
		}

		const auto base = reinterpret_cast<uintptr_t>(g_game_module);

		const auto describe = [base](const void *pointer) -> nlohmann::json {
			if (pointer == nullptr) {
				return nullptr;
			}

			char text[64];
			const auto address = reinterpret_cast<uintptr_t>(pointer);
			sprintf_s(text, "0x%016llx (+0x%llx)", address, address - base);
			return text;
		};

		nlohmann::json::array_t matches;
		for (int32_t i = 0; i < count; ++i) {
			const auto *info = infos[i];
			if (info == nullptr || !ddl::is_readable(info, sizeof(ComponentInfo))) {
				continue;
			}

			char name[0x100];
			if (!ddl::read_string(info->name, name, sizeof(name)) || strstr(name, args[1].c_str()) == nullptr) {
				continue;
			}

			nlohmann::json entry;
			entry["name"] = name;
			entry["id"] = info->id;
			entry["size"] = info->size;
			entry["info_address"] = describe(info);
			entry["update_first"] = describe(info->update_first);
			entry["update_first_results"] = describe(info->update_first_results);
			entry["update_middle"] = describe(info->update_middle);
			entry["update_last"] = describe(info->update_last);
			entry["update_async"] = describe(info->update_async);
			entry["update_async_results"] = describe(info->update_async_results);
			entry["create"] = describe(info->create);
			entry["prius_size"] = info->prius_info.size;
			entry["prius_behavior"] = PriusBehaviorName(info->prius_behavior);
			entry["class_flags"] = DescribeFlags(info->class_flags, COMPONENT_CLASS_FLAG_NAMES, std::size(COMPONENT_CLASS_FLAG_NAMES));
			entry["update_order"] = info->update_order;

			nlohmann::json::array_t parents;
			for (uint8_t p = 0; p < info->parent_count && p < std::size(info->parent_classes); ++p) {
				char parent[0x100];
				if (const auto *parent_info = info->parent_classes[p]; parent_info != nullptr && ddl::is_readable(parent_info, sizeof(ComponentInfo)) && ddl::read_string(parent_info->name, parent, sizeof(parent))) {
					parents.emplace_back(parent);
				}
			}

			entry["parents"] = parents;
			matches.emplace_back(entry);
		}

		nlohmann::json result;
		result["module_base"] = describe(reinterpret_cast<const void *>(base));
		result["registered"] = count;
		result["matches"] = matches;
		return ok(result);
	}

	static auto
	find_component(const std::string &name) -> ComponentInfo * {
		const auto *registry_var = load_rel_var(find_address(COMPONENT_REGISTER_SIGNATURE), COMPONENT_REGISTRY_ADDRESS);
		const auto *count_var = load_rel_var(find_address(COMPONENT_REGISTER_SIGNATURE), COMPONENT_COUNT_ADDRESS);
		if (registry_var == nullptr || count_var == nullptr) {
			return nullptr;
		}

		auto *const *infos = *static_cast<ComponentInfo *const *const *>(registry_var);
		const auto count = *static_cast<const int32_t *>(count_var);
		if (infos == nullptr) {
			return nullptr;
		}

		for (int32_t i = 0; i < count; ++i) {
			auto *info = infos[i];
			if (info == nullptr || !ddl::is_readable(info, sizeof(ComponentInfo))) {
				continue;
			}

			char text[0x100];
			if (ddl::read_string(info->name, text, sizeof(text)) && name == text) {
				return info;
			}
		}

		return nullptr;
	}

	// Nulling a registry update slot crashed the game: a component that was
	// registered with a live update is already in the dispatch list, and the
	// dispatcher calls through the pointer without checking it. Components that
	// ship with null slots are simply never added, which is a different thing.
	// So detour the function instead - the pointer stays valid, and the body is
	// skipped only while freecam is on.
	static auto
	find_component(const std::string &name) -> ComponentInfo *;

	// Detouring one guessed component per rebuild is a bad loop. This is a small
	// pool of generic detours that can be pointed at any component update from the
	// bridge, and - critically - it counts calls, so "the detour did nothing" can
	// be told apart from "the detour never ran".
	// The dispatcher calls updates with three arguments, read straight off the
	// middle phase call site at +0xf23248:
	//
	//   movaps xmm2, xmm6   ; arg3, a float
	//   mov    edx, ebx     ; arg2, the group count
	//   mov    rcx, rdi     ; arg1, the component array
	//   call   r8
	//
	// The thunk must therefore take and forward all three. Declaring it as
	// void *(void *) meant the compiler clobbered rdx and xmm2 setting up its own
	// call, so every detoured update ran with a garbage count and delta time.
	using update_fn_t = void *(*) (void *components, uint32_t count, float delta);

	constexpr int MAX_DETOURS = 8;

	// how many calls are recorded once capture is armed, and how much of the
	// component array is copied out of each. eight is enough to see whether the
	// arguments vary between calls without turning a hot update into a memcpy loop.
	constexpr int MAX_CAPTURES = 8;
	constexpr uint32_t CAPTURE_BYTES = 64;

	// one recorded call. the bytes matter more than the pointer: by the time this
	// is read back over the bridge the component array may have been recycled, so
	// reading through the pointer later would be a stale read.
	struct Capture {
		Capture() = default;
		Capture(Capture &&) = delete; // atomics are not movable

		void *components = nullptr;
		uint32_t count = 0;
		float delta = 0.0f;
		void *caller = nullptr;
		std::atomic_uint32_t bytes_read = 0;
		uint8_t bytes[CAPTURE_BYTES] {};
	};

	struct Detour {
		Detour() = default;
		Detour(Detour &&) = delete; // atomics are not movable

		std::string component;
		std::string slot;
		update_fn_t original = nullptr;
		std::atomic_uint64_t calls = 0;
		std::atomic_uint64_t skipped = 0;
		std::atomic_bool skip = false;
		std::atomic_bool capturing = false;
		std::atomic_int captured = 0;
		Capture captures[MAX_CAPTURES];
		void *caller = nullptr;
		bool installed = false;
	};

	static Detour g_detours[MAX_DETOURS];

	// runs on whatever thread dispatches component updates, which is not the render
	// thread. that rules out ddl::is_readable, whose region cache is only safe on
	// the thread that resets it - hence the uncached probe.
	static auto
	capture_arguments(Detour &detour, void *components, const uint32_t count, const float delta, void *caller) -> void {
		const auto slot = detour.captured.fetch_add(1);
		if (slot >= MAX_CAPTURES) {
			detour.capturing = false;
			return;
		}

		auto &capture = detour.captures[slot];
		capture.components = components;
		capture.count = count;
		capture.delta = delta;
		capture.caller = caller;

		if (ddl::is_readable_uncached(components, CAPTURE_BYTES)) {
			memcpy(capture.bytes, components, CAPTURE_BYTES);
			// published last: a reader only trusts the bytes once this is non zero
			capture.bytes_read.store(CAPTURE_BYTES, std::memory_order_release);
		}

		if (slot + 1 >= MAX_CAPTURES) {
			detour.capturing = false;
		}
	}

	static auto
	dispatch_detour(const int index, void *components, const uint32_t count, const float delta, void *caller) -> void * {
		auto &detour = g_detours[index];
		++detour.calls;

		// the caller is the engine's component update dispatcher. capturing it
		// locates that loop, which is where photo mode's gate has to live: in
		// photo mode every gameplay component stops being dispatched at once.
		if (detour.caller == nullptr) {
			detour.caller = caller;
		}

		if (detour.capturing.load(std::memory_order_relaxed)) {
			capture_arguments(detour, components, count, delta, caller);
		}

		if (detour.skip) {
			++detour.skipped;
			return nullptr;
		}

		return detour.original(components, count, delta);
	}

	template<int Index>
	static auto
	detour_thunk(void *components, uint32_t count, float delta) -> void * {
		return dispatch_detour(Index, components, count, delta, _ReturnAddress());
	}

	static update_fn_t g_thunks[MAX_DETOURS] = {
		&detour_thunk<0>, &detour_thunk<1>, &detour_thunk<2>, &detour_thunk<3>,
		&detour_thunk<4>, &detour_thunk<5>, &detour_thunk<6>, &detour_thunk<7>,
	};

	static auto
	slot_of(const ComponentInfo *info, const std::string &name) -> ddl_call_t * {
		if (name == "first") {
			return info->update_first;
		}

		if (name == "first_results") {
			return info->update_first_results;
		}

		if (name == "middle") {
			return info->update_middle;
		}

		if (name == "last") {
			return info->update_last;
		}

		if (name == "async") {
			return info->update_async;
		}

		if (name == "async_results") {
			return info->update_async_results;
		}

		return nullptr;
	}

	static auto
	describe_detour(const Detour &detour, const int index) -> nlohmann::json {
		nlohmann::json entry;
		entry["index"] = index;
		entry["component"] = detour.component;
		entry["slot"] = detour.slot;
		entry["skipping"] = detour.skip.load();
		entry["calls"] = detour.calls.load();
		entry["skipped"] = detour.skipped.load();
		entry["capturing"] = detour.capturing.load();
		entry["captured"] = detour.captured.load() < MAX_CAPTURES ? detour.captured.load() : MAX_CAPTURES;
		if (detour.caller != nullptr) {
			const auto base = reinterpret_cast<uintptr_t>(g_game_module);
			const auto address = reinterpret_cast<uintptr_t>(detour.caller);
			char text[64];
			sprintf_s(text, "0x%016llx (+0x%llx)", address, address - base);
			entry["dispatcher"] = text;
		}
		return entry;
	}

	// the slot this component and update pair already occupies, or -1
	static auto
	installed_detour(const char *component, const char *slot) -> int {
		for (auto i = 0; i < MAX_DETOURS; ++i) {
			if (g_detours[i].installed && g_detours[i].component == component && g_detours[i].slot == slot) {
				return i;
			}
		}

		return -1;
	}

	auto
	set_detour(const char *component, const char *slot, const bool skip, char *error_out, const size_t error_size) -> bool {
		const auto fail = [error_out, error_size](const char *text) {
			_snprintf_s(error_out, error_size, _TRUNCATE, "%s", text);
			return false;
		};

		// already installed? just flip whether it skips
		if (const auto existing = installed_detour(component, slot); existing >= 0) {
			g_detours[existing].skip = skip;
			return true;
		}

		auto free_slot = -1;
		for (auto i = 0; i < MAX_DETOURS; ++i) {
			if (!g_detours[i].installed) {
				free_slot = i;
				break;
			}
		}

		if (free_slot < 0) {
			return fail("no free detour slots");
		}

		const auto *info = find_component(component);
		if (info == nullptr) {
			return fail("no component registered with that exact name");
		}

		auto *target = slot_of(info, slot);
		if (target == nullptr) {
			return fail("that component has no function in that update slot");
		}

		auto &detour = g_detours[free_slot];
		detour.component = component;
		detour.slot = slot;
		create_hook(detour.component + "::" + detour.slot, reinterpret_cast<LPVOID>(target), reinterpret_cast<LPVOID>(g_thunks[free_slot]), reinterpret_cast<LPVOID *>(&detour.original));
		if (detour.original == nullptr) {
			detour.component.clear();
			detour.slot.clear();
			return fail("could not install the hook, see rivet.log");
		}

		detour.installed = true;
		detour.skip = skip;
		return true;
	}

	static auto
	cmd_component_detour(const std::vector<std::string> &args) -> std::string {
		if (args.size() < 4) {
			return error("usage: component.detour <exact name> <first|first_results|middle|last|async|async_results> <on|off>");
		}

		const auto skip = args[3] == "on" || args[3] == "1";

		char failure[0x200];
		if (!set_detour(args[1].c_str(), args[2].c_str(), skip, failure, sizeof(failure))) {
			return error(failure);
		}

		const auto index = installed_detour(args[1].c_str(), args[2].c_str());
		return ok(describe_detour(g_detours[index], index));
	}

	// the arguments the dispatcher passed, as json. read on the engine thread, so
	// the cached is_readable behind hex_dump is fine here.
	static auto
	describe_capture(const Capture &capture) -> nlohmann::json {
		const auto base = reinterpret_cast<uintptr_t>(g_game_module);

		char text[64];
		nlohmann::json entry;

		_snprintf_s(text, sizeof(text), _TRUNCATE, "0x%016llx", reinterpret_cast<uintptr_t>(capture.components));
		entry["components"] = text;
		entry["count"] = capture.count;
		entry["delta"] = capture.delta;

		if (capture.caller != nullptr) {
			const auto address = reinterpret_cast<uintptr_t>(capture.caller);
			_snprintf_s(text, sizeof(text), _TRUNCATE, "0x%016llx (+0x%llx)", address, address - base);
			entry["caller"] = text;
		}

		if (const auto read = capture.bytes_read.load(std::memory_order_acquire); read > 0) {
			entry["bytes"] = ddl::hex_dump(capture.bytes, read);
		} else {
			entry["bytes"] = nullptr;
			entry["unreadable"] = true;
		}

		return entry;
	}

	// arms argument capture on a detour, installing one if it is not there yet.
	// the counters alone cannot answer what an update is being handed.
	static auto
	cmd_component_capture(const std::vector<std::string> &args) -> std::string {
		if (args.size() < 4) {
			return error("usage: component.capture <exact name> <first|first_results|middle|last|async|async_results> <on|off>");
		}

		const auto arm = args[3] == "on" || args[3] == "1";

		auto index = installed_detour(args[1].c_str(), args[2].c_str());
		if (index < 0) {
			// installed without skipping: observing an update must not change it
			char failure[0x200];
			if (!set_detour(args[1].c_str(), args[2].c_str(), false, failure, sizeof(failure))) {
				return error(failure);
			}

			index = installed_detour(args[1].c_str(), args[2].c_str());
			if (index < 0) {
				return error("the detour was installed but could not be found again");
			}
		}

		auto &detour = g_detours[index];
		if (arm) {
			// reset before arming, so a second run does not read the first one back
			for (auto &capture : detour.captures) {
				capture.bytes_read.store(0, std::memory_order_relaxed);
			}

			detour.captured = 0;
		}

		detour.capturing = arm;
		return ok(describe_detour(detour, index));
	}

	static auto
	cmd_component_captures(const std::vector<std::string> &args) -> std::string {
		nlohmann::json::array_t entries;
		for (auto i = 0; i < MAX_DETOURS; ++i) {
			auto &detour = g_detours[i];
			if (!detour.installed) {
				continue;
			}

			if (args.size() > 1 && detour.component != args[1]) {
				continue;
			}

			if (args.size() > 2 && detour.slot != args[2]) {
				continue;
			}

			auto entry = describe_detour(detour, i);

			nlohmann::json::array_t captures;
			const auto taken = detour.captured.load();
			for (auto slot = 0; slot < (taken < MAX_CAPTURES ? taken : MAX_CAPTURES); ++slot) {
				captures.emplace_back(describe_capture(detour.captures[slot]));
			}

			entry["captures"] = captures;
			entries.emplace_back(entry);
		}

		nlohmann::json result;
		result["detours"] = entries;
		return ok(result);
	}

	static auto
	cmd_component_detours() -> std::string {
		nlohmann::json::array_t entries;
		for (auto i = 0; i < MAX_DETOURS; ++i) {
			if (g_detours[i].installed) {
				entries.emplace_back(describe_detour(g_detours[i], i));
			}
		}

		nlohmann::json result;
		result["detours"] = entries;
		return ok(result);
	}

	static auto
	cmd_actor_groups() -> std::string {
		if (g_SceneManager == nullptr || g_SceneManager->actorGroups == nullptr) {
			return error("actor groups are not available");
		}

		constexpr uint64_t GROUP_BUDGET_MS = 250;
		const auto started = GetTickCount64();
		auto truncated = false;

		nlohmann::json::array_t groups;
		for (int32_t index = 0; index < g_SceneManager->actorGroupMax; ++index) {
			if ((index & 0x3F) == 0 && GetTickCount64() - started > GROUP_BUDGET_MS) {
				truncated = true;
				break;
			}

			const auto *group = &g_SceneManager->actorGroups[index];
			if (!ddl::is_readable(group, sizeof(ActorGroup)) || group->handles == nullptr || group->count == 0) {
				continue;
			}

			char name[0x100];
			if (!ddl::read_string(group->name, name, sizeof(name))) {
				name[0] = '\0';
			}

			nlohmann::json entry;
			entry["index"] = index;
			entry["name"] = name;
			entry["count"] = group->count;

			nlohmann::json::array_t handles;
			for (int32_t i = 0; i < group->count; ++i) {
				handles.emplace_back(group->handles[i].value);
			}

			entry["handles"] = handles;
			groups.emplace_back(entry);
		}

		nlohmann::json result;
		result["truncated"] = truncated;
		result["groups"] = groups;
		return ok(result);
	}

	static auto
	cmd_mem_read(const std::vector<std::string> &args) -> std::string {
		if (args.size() < 3) {
			return error("usage: mem.read <address> <length>");
		}

		uintptr_t address = 0;
		uint32_t length = 0;
		try {
			address = static_cast<uintptr_t>(std::stoull(args[1], nullptr, 0));
			length = static_cast<uint32_t>(std::stoul(args[2], nullptr, 0));
		} catch (const std::exception &) {
			return error("could not parse address or length");
		}

		if (length == 0 || length > 0x1000) {
			return error("length must be between 1 and 0x1000");
		}

		const auto *at = reinterpret_cast<const uint8_t *>(address);
		const auto hex = ddl::hex_dump(at, length);
		if (hex.empty()) {
			return error("address is not readable");
		}

		nlohmann::json result;
		result["address"] = args[1];
		result["length"] = length;
		result["bytes"] = hex;
		return ok(result);
	}

	static auto
	cmd_script_exec(const std::string &source) -> std::string {
		if (source.empty()) {
			return error("usage: script.exec <lua chunk>");
		}

		char out[0x1000];
		if (!scripting::exec(source.c_str(), out, sizeof(out))) {
			return error(out);
		}

		nlohmann::json result;
		result["value"] = out;
		return ok(result);
	}

	// --------------------------------------------------------------- events --

	static auto
	parse_limit(const std::vector<std::string> &args, const size_t at, const size_t fallback) -> size_t {
		if (args.size() <= at) {
			return fallback;
		}

		try {
			return std::stoul(args[at]);
		} catch (const std::exception &) {
			return fallback;
		}
	}

	static auto
	events_unavailable() -> std::string {
		return error(events::unavailable_reason());
	}

	static auto
	cmd_event_classes(const std::vector<std::string> &args) -> std::string {
		if (!events::ready()) {
			return events_unavailable();
		}

		const auto *filter = args.size() > 1 ? args[1].c_str() : nullptr;
		return ok(events::classes(filter, parse_limit(args, 2, 200)));
	}

	static auto
	cmd_event_info(const std::vector<std::string> &args) -> std::string {
		if (args.size() < 2) {
			return error("usage: event.info <name|0xhash>");
		}

		if (!events::ready()) {
			return events_unavailable();
		}

		const auto *info = events::find_class(args[1].c_str());
		if (info == nullptr) {
			return error("no event class called " + args[1]);
		}

		return ok(events::describe(info));
	}

	static auto
	cmd_event_tail(const std::vector<std::string> &args) -> std::string {
		if (!events::ready()) {
			return events_unavailable();
		}

		// a lone number is the limit, not a filter
		if (args.size() == 2 && !args[1].empty() && std::isdigit(static_cast<unsigned char>(args[1][0]))) {
			return ok(events::tail(nullptr, parse_limit(args, 1, 50)));
		}

		const auto *filter = args.size() > 1 ? args[1].c_str() : nullptr;
		return ok(events::tail(filter, parse_limit(args, 2, 50)));
	}

	static auto
	cmd_event_watch(const std::vector<std::string> &args) -> std::string {
		if (args.size() < 3 || (args[2] != "on" && args[2] != "off")) {
			return error("usage: event.watch <name|0xhash> <on|off>");
		}

		if (!events::ready()) {
			return events_unavailable();
		}

		const auto *info = events::find_class(args[1].c_str());
		if (info == nullptr) {
			return error("no event class called " + args[1]);
		}

		events::set_capture(info, args[2] == "on");
		return ok(events::captures(nullptr, 0)["watching"]);
	}

	static auto
	cmd_event_captures(const std::vector<std::string> &args) -> std::string {
		const auto *filter = args.size() > 1 ? args[1].c_str() : nullptr;
		return ok(events::captures(filter, parse_limit(args, 2, 20)));
	}

	// "hero", a number, or number text in any base stoul accepts
	static auto
	json_handle(const nlohmann::json &value, uint32_t &out) -> bool {
		if (value.is_number_integer()) {
			out = value.get<uint32_t>();
			return true;
		}

		if (!value.is_string()) {
			return false;
		}

		const auto text = value.get<std::string>();
		if (text == "hero") {
			out = scene_query::hero();
			return out != 0;
		}

		EngineHandle handle {};
		if (!parse_handle(text, handle)) {
			return false;
		}

		out = handle.value;
		return true;
	}

	// event.send <name> [json]. the json is the rest of the line:
	//   { "target": "hero" | handle, "targets": [...], "sender": ..., "exclude": false,
	//     "broadcast": bool, "radius": 0, "delay": 0, "position": [x, y, z],
	//     "fields": { "ResetCamera": true, "Destination.Position.X": 12.5 } }
	// with no target the event is broadcast, with targets it is not unless asked.
	static auto
	cmd_event_send(const std::string &rest) -> std::string {
		const auto space = rest.find(' ');
		const auto name = rest.substr(0, space);
		if (name.empty()) {
			return error("usage: event.send <name|0xhash> [json]");
		}

		auto options = nlohmann::json::object();
		if (space != std::string::npos && rest.find_first_not_of(' ', space) != std::string::npos) {
			options = nlohmann::json::parse(rest.substr(space + 1), nullptr, false);
			if (options.is_discarded() || !options.is_object()) {
				return error("the options have to be a json object");
			}
		}

		if (!events::ready()) {
			return events_unavailable();
		}

		const auto *info = events::find_class(name.c_str());
		if (info == nullptr) {
			return error("no event class called " + name);
		}

		events::Request request;
		if (options.contains("target")) {
			uint32_t handle = 0;
			if (!json_handle(options["target"], handle)) {
				return error("could not resolve the target");
			}

			request.targets.emplace_back(handle);
		}

		if (options.contains("targets")) {
			if (!options["targets"].is_array()) {
				return error("targets has to be an array");
			}

			for (const auto &target : options["targets"]) {
				uint32_t handle = 0;
				if (!json_handle(target, handle)) {
					return error("could not resolve one of the targets");
				}

				request.targets.emplace_back(handle);
			}
		}

		if (options.contains("sender") && !json_handle(options["sender"], request.sender)) {
			return error("could not resolve the sender");
		}

		request.broadcast = options.value("broadcast", request.targets.empty());
		request.exclude_targets = options.value("exclude", false);
		request.radius = options.value("radius", 0.0f);
		request.delay = options.value("delay", 0.0f);

		if (options.contains("position")) {
			const auto &position = options["position"];
			if (!position.is_array() || position.size() != 3) {
				return error("position has to be [x, y, z]");
			}

			for (size_t axis = 0; axis < 3; ++axis) {
				if (!position[axis].is_number()) {
					return error("position has to be [x, y, z]");
				}

				request.position[axis] = position[axis].get<float>();
			}

			request.has_position = true;
		}

		// the values are checked before anything is queued, so a typo does not send
		// the event out with its defaults
		std::vector<std::pair<std::string, ddl::Value>> writes;
		if (options.contains("fields")) {
			if (!options["fields"].is_object()) {
				return error("fields has to be an object");
			}

			for (const auto &[path, value] : options["fields"].items()) {
				ddl::Value converted {};
				if (value.is_boolean()) {
					converted.kind = ddl::ValueKind::Bool;
					converted.as_bool = value.get<bool>();
				} else if (value.is_number()) {
					converted.kind = ddl::ValueKind::Real;
					converted.as_real = value.get<double>();
				} else {
					return error("field " + path + " has to be a number or a bool");
				}

				if (!events::has_field(info, path.c_str())) {
					return error(std::string(events::class_name(info)) + " has no field " + path);
				}

				writes.emplace_back(path, converted);
			}
		}

		const char *reason = nullptr;
		auto *event = events::queue(info, request, &reason);
		if (event == nullptr) {
			return error(reason != nullptr ? reason : "the event was not queued");
		}

		nlohmann::json::array_t failed;
		for (const auto &[path, value] : writes) {
			const char *why = nullptr;
			if (!events::set_field(info, event, path.c_str(), value, &why)) {
				failed.emplace_back(path + ": " + (why != nullptr ? why : "refused"));
			}
		}

		char address[24];
		_snprintf_s(address, sizeof(address), _TRUNCATE, "%016llx", reinterpret_cast<uintptr_t>(event));

		nlohmann::json result;
		result["class"] = events::class_name(info);
		result["address"] = address;
		result["targets"] = request.targets;
		result["broadcast"] = request.broadcast;
		if (!failed.empty()) {
			result["failed_fields"] = failed;
		}

		return ok(result);
	}

	// ----------------------------------------------------------- time scale --

	static auto
	parse_channel(const std::vector<std::string> &args, const size_t at, int32_t &channel) -> bool {
		channel = time_scale::channel_index(args.size() > at ? args[at].c_str() : "Game");
		return channel >= 0;
	}

	// time.scale <scale> [channel] [ramp]
	static auto
	cmd_time_scale(const std::vector<std::string> &args) -> std::string {
		if (args.size() < 2) {
			return error("usage: time.scale <scale> [channel] [ramp]");
		}

		if (!time_scale::ready()) {
			return error(time_scale::unavailable_reason());
		}

		float scale = 1.0f;
		float ramp = -1.0f;
		try {
			scale = std::stof(args[1]);
			if (args.size() > 3) {
				ramp = std::stof(args[3]);
			}
		} catch (const std::exception &) {
			return error("could not parse a number");
		}

		int32_t channel = -1;
		if (!parse_channel(args, 2, channel)) {
			return error("no time scale channel called " + args[2]);
		}

		const char *reason = nullptr;
		if (!time_scale::set(channel, scale, ramp, &reason)) {
			return error(reason != nullptr ? reason : "refused");
		}

		return ok(time_scale::status());
	}

	// time.clear [channel]
	static auto
	cmd_time_clear(const std::vector<std::string> &args) -> std::string {
		if (!time_scale::ready()) {
			return error(time_scale::unavailable_reason());
		}

		int32_t channel = -1;
		if (!parse_channel(args, 1, channel)) {
			return error("no time scale channel called " + args[1]);
		}

		const char *reason = nullptr;
		if (!time_scale::clear(channel, &reason)) {
			return error(reason != nullptr ? reason : "refused");
		}

		return ok(time_scale::status());
	}

	// camera.fov [scale]
	static auto
	cmd_camera_fov(const std::vector<std::string> &args) -> std::string {
		if (const auto *why = camera::fov_unavailable_reason(); why[0] != '\0') {
			return error(why);
		}

		nlohmann::json result;
		result["was"] = camera::fov_scale();
		if (args.size() > 1) {
			float scale = 1.0f;
			try {
				scale = std::stof(args[1]);
			} catch (const std::exception &) {
				return error("could not parse the scale");
			}

			const char *reason = nullptr;
			if (!camera::set_fov_scale(scale, &reason)) {
				return error(reason != nullptr ? reason : "refused");
			}
		}

		result["now"] = camera::fov_scale();
		return ok(result);
	}

	static auto
	camera_state() -> nlohmann::json {
		nlohmann::json result;
		result["detached"] = camera::detached();
		result["shake_blocked"] = camera::shake_blocked();
		result["shake_override"] = camera::shake_overridden();

		camera::View view;
		if (camera::view(view)) {
			result["position"] = { view.position[0], view.position[1], view.position[2] };
			result["yaw"] = view.yaw;
			result["pitch"] = view.pitch;
			result["fov"] = view.fov;
		}

		return result;
	}

	// camera.get | camera.detach | camera.attach | camera.set x y z [yaw] [pitch] [fov]
	// | camera.shake [on|off]
	static auto
	cmd_camera(const std::vector<std::string> &args) -> std::string {
		const auto &command = args[0];
		if (command == "camera.attach") {
			camera::attach();
			return ok(camera_state());
		}

		if (command == "camera.shake") {
			if (args.size() > 1 && args[1] == "game") {
				camera::release_shake();
			} else if (args.size() > 1) {
				if (args[1] != "on" && args[1] != "off") {
					return error("usage: camera.shake [on|off|game], where on lets shake through and game hands it back to the option");
				}

				const char *reason = nullptr;
				if (!camera::block_shake(args[1] == "off", &reason)) {
					return error(reason != nullptr ? reason : "refused");
				}
			}

			return ok(camera_state());
		}

		if (const auto *why = camera::free_unavailable_reason(); why[0] != '\0') {
			return error(why);
		}

		if (command == "camera.detach") {
			const char *reason = nullptr;
			if (!camera::detach(&reason)) {
				return error(reason != nullptr ? reason : "refused");
			}

			return ok(camera_state());
		}

		if (command == "camera.set") {
			if (args.size() < 4) {
				return error("usage: camera.set <x> <y> <z> [yaw] [pitch] [fov]");
			}

			camera::View view;
			camera::view(view);
			try {
				for (size_t axis = 0; axis < 3; ++axis) {
					view.position[axis] = std::stof(args[1 + axis]);
				}

				if (args.size() > 4) {
					view.yaw = std::stof(args[4]);
				}

				if (args.size() > 5) {
					view.pitch = std::stof(args[5]);
				}

				if (args.size() > 6) {
					view.fov = std::stof(args[6]);
				}
			} catch (const std::exception &) {
				return error("could not parse a number");
			}

			const char *reason = nullptr;
			if (!camera::set_view(view, &reason)) {
				return error(reason != nullptr ? reason : "refused");
			}
		}

		return ok(camera_state());
	}

	// hud.notify <text> | hud.message <type> <seconds> <text>. the text is the rest
	// of the line, spaces and all
	static auto
	cmd_hud(const std::string &line) -> std::string {
		auto type = hud::MessageType::Generic;
		auto duration = 3.0f;
		std::string text;

		constexpr std::string_view notify_prefix = "hud.notify ";
		if (line.starts_with(notify_prefix)) {
			text = line.substr(notify_prefix.size());
		} else {
			const auto args = split(line);
			if (args.size() < 4) {
				return error("usage: hud.message <type> <seconds> <text>");
			}

			if (!hud::message_type(args[1].c_str(), type)) {
				return error("no hud message type called " + args[1]);
			}

			try {
				duration = std::stof(args[2]);
			} catch (const std::exception &) {
				return error("could not parse the duration");
			}

			// everything after the third word, as typed
			size_t at = 0;
			for (auto word = 0; word < 3; ++word) {
				at = line.find_first_not_of(' ', at);
				at = line.find(' ', at);
			}

			text = line.substr(line.find_first_not_of(' ', at));
		}

		const char *reason = nullptr;
		if (!hud::notify(type, text.c_str(), duration, nullptr, &reason)) {
			return error(reason != nullptr ? reason : "refused");
		}

		nlohmann::json result;
		result["shown"] = text;
		return ok(result);
	}

	// vanity.equip <bundle> | vanity.owns <bundle>, on the hero. bundle is the
	// config asset path or its id as 16 hex digits
	static auto
	cmd_vanity(const std::vector<std::string> &args) -> std::string {
		if (args.size() < 2) {
			return error("usage: " + args[0] + " <bundle path or 16 digit hex id>");
		}

		uint64_t bundle = 0;
		if (!vanity::bundle_id(args[1].c_str(), bundle)) {
			return error(args[1] + " is neither a bundle path nor a 16 digit hex id");
		}

		const auto hero = scene_query::hero();
		if (hero == 0) {
			return error("there is no hero right now");
		}

		char id[24];
		_snprintf_s(id, sizeof(id), _TRUNCATE, "%016llx", bundle);

		nlohmann::json result;
		result["bundle"] = id;

		const char *reason = nullptr;
		if (args[0] == "vanity.owns") {
			bool owned = false;
			if (!vanity::has_bundle(hero, bundle, owned, &reason)) {
				return error(reason != nullptr ? reason : "refused");
			}

			result["owned"] = owned;
			return ok(result);
		}

		bool equipped = false;
		if (!vanity::equip_bundle(hero, bundle, equipped, &reason)) {
			return error(reason != nullptr ? reason : "refused");
		}

		result["equipped"] = equipped;
		return ok(result);
	}

	// config.list [type] [limit] | config.get <id|path> | config.set <id|path> <field> <value>
	static auto
	cmd_config(const std::vector<std::string> &args) -> std::string {
		if (const auto *why = configs::unavailable_reason(); why[0] != '\0') {
			return error(why);
		}

		if (args[0] == "config.list") {
			// a lone number is the limit, not a type
			if (args.size() == 2 && !args[1].empty() && std::isdigit(static_cast<unsigned char>(args[1][0]))) {
				return ok(configs::list("", parse_limit(args, 1, 200)));
			}

			return ok(configs::list(args.size() > 1 ? args[1].c_str() : "", parse_limit(args, 2, 200)));
		}

		if (args.size() < 2) {
			return error("usage: " + args[0] + " <config path or 16 digit hex id> ...");
		}

		uint64_t id = 0;
		if (!configs::parse_id(args[1].c_str(), id)) {
			return error(args[1] + " is neither a config path nor a 16 digit hex id");
		}

		configs::Config config;
		if (!configs::find(id, config)) {
			return error("no config " + args[1] + " is loaded");
		}

		char name[0x100];
		if (!ddl::read_string(config.type->name, name, sizeof(name))) {
			name[0] = '\0';
		}

		nlohmann::json result;
		result["type"] = name;

		if (args[0] == "config.get") {
			result["fields"] = configs::values(config);
			return ok(result);
		}

		if (args.size() < 4) {
			return error("usage: config.set <config> <field.path> <value>");
		}

		ddl::Value value {};
		if (args[3] == "true" || args[3] == "false") {
			value.kind = ddl::ValueKind::Bool;
			value.as_bool = args[3] == "true";
		} else {
			try {
				value.kind = ddl::ValueKind::Real;
				value.as_real = std::stod(args[3]);
			} catch (const std::exception &) {
				return error("the value has to be a number, true or false");
			}
		}

		ddl::Value previous {};
		const char *reason = nullptr;
		if (!configs::set(config, args[2].c_str(), value, previous, &reason)) {
			return error(reason != nullptr ? reason : "refused");
		}

		result["field"] = args[2];
		result["was"] = ddl::to_json(previous);
		return ok(result);
	}

	// script.nodes [filter] [limit]
	static auto
	cmd_script_nodes(const std::vector<std::string> &args) -> std::string {
		if (g_SceneManager == nullptr) {
			return error("scene manager is not available");
		}

		g_scan_deadline = GetTickCount64() + 250;

		// a lone number is the limit, not a filter
		if (args.size() == 2 && !args[1].empty() && std::isdigit(static_cast<unsigned char>(args[1][0]))) {
			return ok(script_signal::nodes("", parse_limit(args, 1, 200), scan_expired));
		}

		return ok(script_signal::nodes(args.size() > 1 ? args[1].c_str() : "", parse_limit(args, 2, 200), scan_expired));
	}

	// script.signal <actor> <component class> <plug> [nth], nth 1 based
	static auto
	cmd_script_signal(const std::vector<std::string> &args) -> std::string {
		if (args.size() < 4) {
			return error("usage: script.signal <actor> <component class> <plug> [nth]");
		}

		EngineHandle actor {};
		if (!parse_handle(args[1], actor)) {
			return error("could not parse the actor handle");
		}

		auto nth = 0;
		if (args.size() > 4) {
			try {
				nth = std::stoi(args[4]) - 1;
			} catch (const std::exception &) {
				return error("could not parse nth");
			}
		}

		const char *reason = nullptr;
		const auto node = script_signal::find_node(actor.value, args[2].c_str(), nth, &reason);
		if (node == 0) {
			return error(reason != nullptr ? reason : "no such node");
		}

		const auto plug = script_signal::plug_hash(args[3].c_str());
		if (!script_signal::send(node, plug, &reason)) {
			return error(reason != nullptr ? reason : "refused");
		}

		char plug_text[16];
		_snprintf_s(plug_text, sizeof(plug_text), _TRUNCATE, "0x%08x", plug);

		nlohmann::json result;
		result["component"] = node;
		result["plug"] = plug_text;
		return ok(result);
	}

	// runs on the engine thread, inside pump
	static auto
	execute(const std::string &line) -> std::string {
		// the chunk is the rest of the line, verbatim: splitting it on spaces would
		// take a lua program apart
		constexpr std::string_view exec_prefix = "script.exec ";
		if (line.starts_with(exec_prefix)) {
			return cmd_script_exec(line.substr(exec_prefix.size()));
		}

		if (line.starts_with("hud.notify ") || line.starts_with("hud.message ")) {
			return cmd_hud(line);
		}

		// the json options are the rest of the line, same reason
		constexpr std::string_view send_prefix = "event.send ";
		if (line.starts_with(send_prefix)) {
			return cmd_event_send(line.substr(send_prefix.size()));
		}

		const auto args = split(line);
		if (args.empty()) {
			return error("empty request");
		}

		const auto &command = args[0];
		if (command == "scene.actors") {
			return cmd_scene_actors(args);
		}

		if (command == "actor.get") {
			return cmd_actor_get(args);
		}

		if (command == "actor.set_position") {
			return cmd_actor_set_position(args);
		}

		if (command == "component.detour") {
			return cmd_component_detour(args);
		}

		if (command == "component.detours") {
			return cmd_component_detours();
		}

		if (command == "component.capture") {
			return cmd_component_capture(args);
		}

		if (command == "component.captures") {
			return cmd_component_captures(args);
		}
		if (command == "component.info") {
			return cmd_component_info(args);
		}

		if (command == "mem.watch") {
			return cmd_mem_watch(args);
		}

		if (command == "mem.watches") {
			return ok(watch::results());
		}

		if (command == "actor.uid") {
			return cmd_actor_uid(args);
		}

		if (command == "actor.hero") {
			return cmd_actor_hero();
		}

		if (command == "scene.find_component") {
			return cmd_find_component(args);
		}

		if (command == "actor.groups") {
			return cmd_actor_groups();
		}

		if (command == "actor.dump") {
			return cmd_actor_dump(args);
		}

		if (command == "mem.read") {
			return cmd_mem_read(args);
		}

		if (command == "camera.fov") {
			return cmd_camera_fov(args);
		}

		if (command == "camera.get" || command == "camera.detach" || command == "camera.attach" || command == "camera.set" || command == "camera.shake") {
			return cmd_camera(args);
		}

		if (command == "script.signal") {
			return cmd_script_signal(args);
		}

		if (command == "script.nodes") {
			return cmd_script_nodes(args);
		}

		if (command == "config.list" || command == "config.get" || command == "config.set") {
			return cmd_config(args);
		}

		if (command == "vanity.equip" || command == "vanity.owns") {
			return cmd_vanity(args);
		}

		if (command == "time.status") {
			return ok(time_scale::status());
		}

		if (command == "time.scale") {
			return cmd_time_scale(args);
		}

		if (command == "time.clear") {
			return cmd_time_clear(args);
		}

		if (command == "event.status") {
			return ok(events::status());
		}

		if (command == "event.classes") {
			return cmd_event_classes(args);
		}

		if (command == "event.info") {
			return cmd_event_info(args);
		}

		if (command == "event.tail") {
			return cmd_event_tail(args);
		}

		if (command == "event.watch") {
			return cmd_event_watch(args);
		}

		if (command == "event.captures") {
			return cmd_event_captures(args);
		}

		if (command == "script.status") {
			return ok(scripting::status());
		}

		// a script change costs a restart otherwise, because the dll cannot be
		// replaced while the game holds it
		if (command == "script.reload") {
			scripting::reload();
			return ok(scripting::status());
		}

		return error("unknown command: " + command);
	}

	// proves whether the present hook reaches us at all, without another guess
	static std::atomic_uint64_t g_frames = 0;
	static std::atomic_uint64_t g_served = 0;
	static std::atomic_uint64_t g_last_ms = 0;

	auto
	pump() -> void {
		if (!g_running) {
			return;
		}

		++g_frames;

		std::string request;
		{
			std::lock_guard guard { g_lock };
			if (!g_pending) {
				return;
			}

			request = g_request;
		}

		ddl::reset_readable_cache();

		// nothing may escape into the render thread
		const auto started = GetTickCount64();
		std::string response;
		try {
			response = execute(request);
		} catch (const std::exception &failure) {
			response = error(std::string("exception: ") + failure.what());
		} catch (...) {
			response = error("unknown exception");
		}

		g_last_ms = GetTickCount64() - started;
		++g_served;

		// this all runs on the render thread, so anything slow is a stutter
		if (g_last_ms > 100) {
			g_output << "[bridge] " << request << " took " << g_last_ms << "ms on the engine thread\n";
			g_output.flush();
		}

		{
			std::lock_guard guard { g_lock };
			g_response = std::move(response);
			g_pending = false;
		}

		SetEvent(g_done);
	}

	// --------------------------------------------------------- pipe thread --

	// answered without engine state, so they still work while the game is stalled
	static auto
	execute_local(const std::vector<std::string> &args) -> std::string {
		if (args[0] == "ping") {
			nlohmann::json result;
			result["version"] = RIVET_VERSION;
			result["overlay"] = g_settings.overlay.enabled;
			result["pump"] = game_thread::status();
			return ok(result);
		}

		if (args[0] == "log.tail") {
			auto count = 40;
			if (args.size() > 1) {
				try {
					count = std::stoi(args[1]);
				} catch (const std::exception &) {
					return error("could not parse line count");
				}
			}

			std::ifstream log { "./rivet.log" };
			if (!log.is_open()) {
				return error("rivet.log could not be opened");
			}

			std::vector<std::string> lines;
			std::string line;
			while (std::getline(log, line)) {
				lines.emplace_back(line);
				if (static_cast<int>(lines.size()) > count) {
					lines.erase(lines.begin());
				}
			}

			nlohmann::json result;
			result["lines"] = lines;
			return ok(result);
		}

		if (args[0] == "stats") {
			nlohmann::json result;
			result["frames_pumped"] = g_frames.load();
			result["requests_served"] = g_served.load();
			result["last_ms"] = g_last_ms.load();
			return ok(result);
		}

		if (args[0] == "help") {
			nlohmann::json result;
			result["commands"] = nlohmann::json::array_t {
				"ping", "help", "log.tail <n>", "scene.actors [filter] [limit]", "scene.find_component <class> [limit] [exact]", "actor.hero", "actor.uid <uid>", "actor.groups", "actor.get <handle>", "actor.dump <handle>", "actor.set_position <handle> <x> <y> <z>", "component.info <name>", "component.detour <name> <slot> <on|off>", "component.detours", "component.capture <name> <slot> <on|off>", "component.captures [name] [slot]", "mem.read <address> <length>", "mem.watch <address|+rva|off> [length|exec]", "mem.watches", "script.status", "script.reload", "script.exec <lua chunk>", "event.status", "event.classes [filter] [limit]", "event.info <name|0xhash>", "event.tail [filter] [limit]", "event.watch <name|0xhash> <on|off>", "event.captures [filter] [limit]", "event.send <name|0xhash> [json]", "time.status", "time.scale <scale> [channel] [ramp]", "time.clear [channel]", "camera.fov [scale]", "camera.get", "camera.detach", "camera.attach", "camera.set <x> <y> <z> [yaw] [pitch] [fov]", "camera.shake [on|off|game]", "hud.notify <text>", "hud.message <type> <seconds> <text>", "vanity.equip <bundle>", "vanity.owns <bundle>", "config.list [type] [limit]", "config.get <config>", "config.set <config> <field.path> <value>", "script.nodes [filter] [limit]", "script.signal <actor> <component class> <plug> [nth]"
			};
			return ok(result);
		}

		return {};
	}

	static auto
	read_frame(const HANDLE pipe, std::string &out) -> bool {
		uint32_t length = 0;
		DWORD read = 0;
		if (!ReadFile(pipe, &length, sizeof(length), &read, nullptr) || read != sizeof(length)) {
			return false;
		}

		if (length == 0 || length > MAX_REQUEST) {
			return false;
		}

		out.resize(length);
		uint32_t total = 0;
		while (total < length) {
			if (!ReadFile(pipe, out.data() + total, length - total, &read, nullptr) || read == 0) {
				return false;
			}

			total += read;
		}

		return true;
	}

	static auto
	write_frame(const HANDLE pipe, const std::string &payload) -> bool {
		const auto length = static_cast<uint32_t>(payload.size());
		DWORD written = 0;
		if (!WriteFile(pipe, &length, sizeof(length), &written, nullptr) || written != sizeof(length)) {
			return false;
		}

		uint32_t total = 0;
		while (total < length) {
			if (!WriteFile(pipe, payload.data() + total, length - total, &written, nullptr) || written == 0) {
				return false;
			}

			total += written;
		}

		return true;
	}

	static auto
	serve(const std::string &request) -> std::string {
		const auto args = split(request);
		if (args.empty()) {
			return error("empty request");
		}

		if (auto local = execute_local(args); !local.empty()) {
			return local;
		}

		// reset before publishing the request: pump can finish inside the gap
		// between the two, and resetting afterwards would drop its signal
		ResetEvent(g_done);

		{
			std::lock_guard guard { g_lock };
			if (g_pending) {
				return error("a request is already in flight");
			}

			g_request = request;
			g_response.clear();
			g_pending = true;
		}

		if (WaitForSingleObject(g_done, RESPONSE_TIMEOUT_MS) != WAIT_OBJECT_0) {
			std::lock_guard guard { g_lock };
			g_pending = false;
			return error("timed out waiting for the engine thread, is the game presenting frames?");
		}

		std::lock_guard guard { g_lock };
		return g_response;
	}

	static auto
	run() -> void {
		const auto name = std::string { R"(\\.\pipe\)" } + g_settings.bridge.pipe_name;
		g_output << "[bridge] listening on " << name << "\n";
		g_output.flush();

		while (g_running) {
			g_pipe = CreateNamedPipeA(name.c_str(),
									  PIPE_ACCESS_DUPLEX,
									  PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
									  1,
									  MAX_REQUEST,
									  MAX_REQUEST,
									  0,
									  nullptr);
			if (g_pipe == INVALID_HANDLE_VALUE) {
				g_output << "[bridge] could not create the pipe, error " << GetLastError() << "\n";
				g_output.flush();
				return;
			}

			if (!ConnectNamedPipe(g_pipe, nullptr) && GetLastError() != ERROR_PIPE_CONNECTED) {
				CloseHandle(g_pipe);
				g_pipe = INVALID_HANDLE_VALUE;
				continue;
			}

			std::string request;
			while (g_running && read_frame(g_pipe, request)) {
				const auto response = serve(request);
				if (!write_frame(g_pipe, response)) {
					break;
				}
			}

			DisconnectNamedPipe(g_pipe);
			CloseHandle(g_pipe);
			g_pipe = INVALID_HANDLE_VALUE;
		}
	}

	auto
	init() -> void {
		if (!g_settings.bridge.enabled) {
			return;
		}

		game_thread::install();

		g_done = CreateEvent(nullptr, true, false, nullptr);
		if (g_done == nullptr) {
			g_output << "[bridge] could not create the response event\n";
			return;
		}

		g_running = true;
		g_thread = std::thread(run);
	}

	auto
	fini() -> void {
		if (!g_running) {
			return;
		}

		g_running = false;

		// unblock ConnectNamedPipe by connecting to ourselves
		const auto name = std::string { R"(\\.\pipe\)" } + g_settings.bridge.pipe_name;
		if (const auto wake = CreateFileA(name.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr); wake != INVALID_HANDLE_VALUE) {
			CloseHandle(wake);
		}

		if (g_thread.joinable()) {
			g_thread.join();
		}

		if (g_done != nullptr) {
			CloseHandle(g_done);
			g_done = nullptr;
		}
	}
} // namespace rivet_hook::bridge

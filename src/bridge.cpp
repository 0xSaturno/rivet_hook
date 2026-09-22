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
#include "game/scene_manager.hpp"
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

	static auto
	cmd_scene_actors(const std::vector<std::string> &args) -> std::string {
		if (g_SceneManager == nullptr || g_SceneManager->actors == nullptr) {
			return error("scene manager is not available");
		}

		// an optional substring filter, since a loaded level has thousands of actors
		const auto *filter = args.size() > 1 ? args[1].c_str() : nullptr;
		const auto limit = args.size() > 2 ? std::stoi(args[2]) : 200;

		const auto count = g_SceneManager->actorCount;
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
			entry["handle"] = EngineHandle { .id = static_cast<uint32_t>(index), .type = actor->type }.value;
			entry["index"] = index;
			entry["type"] = actor->type;
			entry["name"] = name;
			entry["components"] = actor->componentCount;
			entry["children"] = actor->childCount;
			actors.emplace_back(entry);
		}

		nlohmann::json result;
		result["returned"] = actors.size();
		result["matched"] = matched;
		result["total"] = count;
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
		result["type"] = actor->type;
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

		memcpy(&actor->object->transform_matrix[3], position, sizeof(position));

		result["now"] = { actor->object->transform_matrix[3][0], actor->object->transform_matrix[3][1], actor->object->transform_matrix[3][2] };
		result["frame"] = actor->object->lastModifiedOnFrame;
		return ok(result);
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
		for (int32_t index = 0; index < g_SceneManager->actorGroupCount; ++index) {
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

	// runs on the engine thread, inside pump
	static auto
	execute(const std::string &line) -> std::string {
		// the chunk is the rest of the line, verbatim: splitting it on spaces would
		// take a lua program apart
		constexpr std::string_view exec_prefix = "script.exec ";
		if (line.starts_with(exec_prefix)) {
			return cmd_script_exec(line.substr(exec_prefix.size()));
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

		if (command == "actor.groups") {
			return cmd_actor_groups();
		}

		if (command == "actor.dump") {
			return cmd_actor_dump(args);
		}

		if (command == "mem.read") {
			return cmd_mem_read(args);
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
				"ping", "help", "log.tail <n>", "scene.actors [filter] [limit]", "actor.groups", "actor.get <handle>", "actor.dump <handle>", "actor.set_position <handle> <x> <y> <z>", "component.info <name>", "component.detour <name> <slot> <on|off>", "component.detours", "component.capture <name> <slot> <on|off>", "component.captures [name] [slot]", "mem.read <address> <length>", "script.status", "script.reload", "script.exec <lua chunk>"
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

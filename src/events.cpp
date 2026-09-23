// SPDX-FileCopyrightText: 2025-2026 Neptuwunium
//
// SPDX-License-Identifier: EUPL-1.2

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <deque>
#include <string>
#include <unordered_map>

#include "events.hpp"

#include "runtime.hpp"
#include "signature.hpp"

using namespace rivet_hook::game;

namespace rivet_hook::events {
	constexpr int32_t MAX_CLASSES = 4096;
	constexpr size_t MAX_TARGETS = 64;
	// how many events one poll will take. a frame queues a few hundred at most, so
	// hitting this means the cursor went wrong, not that the game got busy
	constexpr uint32_t MAX_PER_POLL = 4096;
	constexpr size_t TAIL_SIZE = 1024;
	constexpr size_t CAPTURE_SIZE = 128;
	constexpr int MAX_PARENT_DEPTH = 32;
	// the ring is sized from a config at boot; anything past this is a misread
	constexpr uint32_t MAX_RING = 1u << 20;

	static EventSystem *g_system = nullptr;
	static queue_event_t g_queue_event = nullptr;

	enum class State : uint8_t {
		Unresolved, // init found nothing, or has not run
		Unchecked,	// resolved, the class table has not been read yet
		Ready,
		Broken, // the table did not look like one, see g_broken
	};

	static State g_state = State::Unresolved;
	static const char *g_broken = "not resolved";

	static std::unordered_map<std::string, uint16_t> g_by_name;
	static std::unordered_map<uint32_t, uint16_t> g_by_hash;
	static std::array<const char *, MAX_CLASSES> g_names {};
	// offset of SenderHandle per class, -1 when it has none, -2 until looked up
	static std::array<int32_t, MAX_CLASSES> g_sender_offset {};

	static uint32_t g_cursor = 0;
	static bool g_cursor_valid = false;
	static std::vector<EventEntry> g_fresh;

	struct Seen {
		uint64_t sequence;
		uint32_t frame;
		uint16_t class_id;
		uint16_t flags;
		uint32_t sender;
		uint32_t target_count;
		uint32_t first_target;
		float delay;
		float radius;
	};

	static std::deque<Seen> g_tail;
	static uint64_t g_sequence = 0;
	static uint64_t g_polls = 0;
	static uint64_t g_resets = 0;
	static uint64_t g_queued = 0;
	static uint64_t g_queue_failures = 0;
	static int32_t g_skipped = 0;

	static std::vector<uint16_t> g_capture_classes;
	static std::deque<nlohmann::json> g_captures;

	auto
	init() -> void {
		if (g_state != State::Unresolved) {
			return;
		}

		// both matches of the allocation site load the same global
		g_system = static_cast<EventSystem *>(load_rel_var(find_address(EVENT_SYSTEM_SIGNATURE, 2), EVENT_SYSTEM_ADDRESS));
		g_queue_event = reinterpret_cast<queue_event_t>(find_address(QUEUE_EVENT_SIGNATURE));

		if (g_system == nullptr || g_queue_event == nullptr) {
			g_broken = g_system == nullptr ? "the event system was not found" : "QueueEvent was not found";
			g_output << "[events] " << g_broken << ", events are unavailable\n";
			g_output.flush();
			return;
		}

		g_state = State::Unchecked;
		g_output << "[events] event system at " << static_cast<void *>(g_system) << ", QueueEvent at " << reinterpret_cast<void *>(g_queue_event) << "\n";
		g_output.flush();
	}

	static auto
	fail_check(const char *reason) -> bool {
		g_state = State::Broken;
		g_broken = reason;
		g_output << "[events] " << reason << ", events are unavailable\n";
		g_output.flush();
		return false;
	}

	// the class table is filled once, from the static registrations, while the
	// engine starts. it is read once here and every lookup after answers from the
	// copy, so a bad offset shows up as one refusal instead of a crash per call.
	static auto
	check() -> bool {
		if (!ddl::is_readable(g_system, sizeof(EventSystem))) {
			return fail_check("the event system is not readable");
		}

		const auto count = g_system->class_count;
		if (count == 0) {
			// not registered yet. not an error, the next call asks again
			return false;
		}

		if (count < 0 || count > MAX_CLASSES) {
			return fail_check("the event class count is out of range");
		}

		for (int32_t id = 0; id < count; ++id) {
			const auto &info = g_system->classes[id];
			if (info.id != id) {
				return fail_check("an event class id does not match its slot");
			}

			// one unreadable class is left out rather than taking every event down
			// with it. it cannot be looked up, sent or decoded
			if (info.type_info == nullptr || !ddl::is_readable(info.type_info, sizeof(DDLTypeInfo))) {
				++g_skipped;
				g_output << "[events] class " << id << " has no readable type info, leaving it out\n";
				continue;
			}

			g_names[id] = info.type_info->name;
			g_sender_offset[id] = -2;
			g_by_hash.emplace(info.type_info->type_id, static_cast<uint16_t>(id));

			char name[0x100];
			if (ddl::read_string(info.type_info->name, name, sizeof(name))) {
				g_by_name.emplace(name, static_cast<uint16_t>(id));
			}
		}

		g_state = State::Ready;
		g_output << "[events] " << count << " event classes registered\n";
		g_output.flush();
		return true;
	}

	auto
	ready() -> bool {
		if (g_state == State::Ready) {
			return true;
		}

		if (g_state != State::Unchecked) {
			return false;
		}

		return check();
	}

	auto
	class_at(const uint16_t id) -> const EventClassInfo * {
		// a class check() left out has no name and stays unreachable
		if (!ready() || id >= g_system->class_count || g_names[id] == nullptr) {
			return nullptr;
		}

		return &g_system->classes[id];
	}

	auto
	find_class(const char *name) -> const EventClassInfo * {
		if (name == nullptr || !ready()) {
			return nullptr;
		}

		if (name[0] == '0' && (name[1] == 'x' || name[1] == 'X')) {
			char *end = nullptr;
			const auto hash = static_cast<uint32_t>(strtoul(name, &end, 16));
			if (end != name + 2 && *end == '\0') {
				const auto found = g_by_hash.find(hash);
				return found != g_by_hash.end() ? &g_system->classes[found->second] : nullptr;
			}
		}

		const auto found = g_by_name.find(name);
		return found != g_by_name.end() ? &g_system->classes[found->second] : nullptr;
	}

	auto
	class_name(const EventClassInfo *info) -> const char * {
		if (info == nullptr || info->id >= MAX_CLASSES || g_names[info->id] == nullptr) {
			return "";
		}

		// the names are string literals in the image, checked readable by check()
		return g_names[info->id];
	}

	auto
	is_type(const uint16_t class_id, const uint16_t parent_id) -> bool {
		const auto *info = class_at(class_id);
		for (auto depth = 0; info != nullptr && depth < MAX_PARENT_DEPTH; ++depth) {
			if (info->id == parent_id) {
				return true;
			}

			info = info->parent;
		}

		return false;
	}

	// the engine call on its own, so a fault inside it can be caught without any
	// C++ object on the frame to unwind
	static auto
	call_queue_event(const EventSystem *system, const uint32_t hash, const Request &request, const uint32_t *targets, const int32_t count, bool *faulted) -> uint8_t * {
		*faulted = false;
#ifdef _MSC_VER
		__try {
#endif
			return static_cast<uint8_t *>(g_queue_event(const_cast<EventSystem *>(system), hash, request.sender, targets, count, request.exclude_targets, request.has_position ? request.position : nullptr, 0, request.broadcast, request.radius, request.delay, nullptr, nullptr));
#ifdef _MSC_VER
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			*faulted = true;
			return nullptr;
		}
#endif
	}

	auto
	queue(const EventClassInfo *info, const Request &request, const char **reason) -> uint8_t * {
		const auto fail = [reason](const char *text) -> uint8_t * {
			++g_queue_failures;
			if (reason != nullptr) {
				*reason = text;
			}

			return nullptr;
		};

		if (!ready()) {
			return fail(g_broken);
		}

		if (info == nullptr) {
			return fail("no such event class");
		}

		if (request.targets.size() > MAX_TARGETS) {
			return fail("too many targets");
		}

		const auto *targets = request.targets.empty() ? nullptr : request.targets.data();
		bool faulted = false;
		auto *event = call_queue_event(g_system, info->type_info->type_id, request, targets, static_cast<int32_t>(request.targets.size()), &faulted);
		if (faulted) {
			g_output << "[events] QueueEvent faulted for " << class_name(info) << "\n";
			g_output.flush();
			return fail("QueueEvent faulted");
		}

		if (event == nullptr) {
			return fail("the engine refused the event, its queue may be full");
		}

		++g_queued;
		return event;
	}

	auto
	set_field(const EventClassInfo *info, uint8_t *event, const char *path, const ddl::Value &value, const char **reason) -> bool {
		if (info == nullptr || event == nullptr) {
			if (reason != nullptr) {
				*reason = "no event to write into";
			}

			return false;
		}

		const auto *type = info->type_info;
		const uint8_t *owner = event;
		const auto index = ddl::resolve_path(type, owner, path);
		if (index < 0) {
			if (reason != nullptr) {
				*reason = "the event has no field by that path";
			}

			return false;
		}

		return ddl::write_field(type, const_cast<uint8_t *>(owner), index, 0, value, reason);
	}

	auto
	warp(const uint32_t handle, const float position[3], const char **reason) -> bool {
		static const char *const AXES[] = { "Destination.Position.X", "Destination.Position.Y", "Destination.Position.Z" };

		const auto *info = find_class("PerformWarpEvent");
		if (info == nullptr) {
			if (reason != nullptr) {
				*reason = ready() ? "PerformWarpEvent is not registered" : unavailable_reason();
			}

			return false;
		}

		// once queued the event goes out whatever happens next, and one with its
		// default destination would warp the actor to the origin
		for (const auto *axis : AXES) {
			if (!has_field(info, axis)) {
				if (reason != nullptr) {
					*reason = "PerformWarpEvent has no Destination.Position";
				}

				return false;
			}
		}

		Request request;
		request.targets.emplace_back(handle);
		request.broadcast = false;

		auto *event = queue(info, request, reason);
		if (event == nullptr) {
			return false;
		}

		for (auto axis = 0; axis < 3; ++axis) {
			ddl::Value value {};
			value.kind = ddl::ValueKind::Real;
			value.as_real = position[axis];
			if (!set_field(info, event, AXES[axis], value, reason)) {
				return false;
			}
		}

		return true;
	}

	auto
	has_field(const EventClassInfo *info, const char *path) -> bool {
		if (info == nullptr) {
			return false;
		}

		const auto *type = info->type_info;
		const uint8_t *object = nullptr;
		return ddl::resolve_path(type, object, path) >= 0;
	}

	auto
	get_field(const EventClassInfo *info, const uint8_t *event, const char *path) -> ddl::Value {
		if (info == nullptr || event == nullptr) {
			return ddl::none_value(ddl::FieldType::U8);
		}

		const auto *type = info->type_info;
		const auto index = ddl::resolve_path(type, event, path);
		return ddl::read_field(type, event, index, 0);
	}

	auto
	sender_of(const EventEntry &entry) -> uint32_t {
		if (entry.class_id >= MAX_CLASSES || !ready() || entry.class_id >= g_system->class_count) {
			return 0;
		}

		auto &offset = g_sender_offset[entry.class_id];
		if (offset == -2) {
			const auto *type = g_system->classes[entry.class_id].type_info;
			const auto index = ddl::find_field(type, "SenderHandle");
			offset = index >= 0 ? static_cast<int32_t>(type->field_offsets[index]) : -1;
		}

		if (offset < 0) {
			return 0;
		}

		const auto *at = static_cast<const uint8_t *>(entry.event) + offset;
		return ddl::is_readable(at, sizeof(uint32_t)) ? *reinterpret_cast<const uint32_t *>(at) : 0;
	}

	auto
	decode(const EventEntry &entry) -> nlohmann::json {
		const auto *info = class_at(entry.class_id);
		if (info == nullptr || entry.event == nullptr || !ddl::is_readable(entry.event, info->type_info->allocation_size)) {
			return nullptr;
		}

		return ddl::values_of(info->type_info, static_cast<const uint8_t *>(entry.event));
	}

	static auto
	targets_of(const EventEntry &entry) -> nlohmann::json {
		nlohmann::json::array_t out;
		if (entry.target_count == 1) {
			out.emplace_back(entry.target(0));
		} else if (entry.target_count > 1 && entry.target_count <= MAX_TARGETS && ddl::is_readable(entry.targets, sizeof(uint32_t) * entry.target_count)) {
			for (uint32_t i = 0; i < entry.target_count; ++i) {
				out.emplace_back(entry.targets[i]);
			}
		}

		return out;
	}

	static auto
	record(const EventEntry &entry) -> void {
		Seen seen {};
		seen.sequence = ++g_sequence;
		seen.frame = g_system->frame_id;
		seen.class_id = entry.class_id;
		seen.flags = entry.flags;
		seen.sender = sender_of(entry);
		seen.target_count = entry.target_count;
		seen.first_target = entry.target_count == 1 ? entry.target(0) : 0;
		seen.delay = entry.delay;
		seen.radius = entry.radius;

		if (g_tail.size() >= TAIL_SIZE) {
			g_tail.pop_front();
		}

		g_tail.emplace_back(seen);

		for (const auto captured : g_capture_classes) {
			if (!is_type(entry.class_id, captured)) {
				continue;
			}

			nlohmann::json capture;
			capture["sequence"] = seen.sequence;
			capture["frame"] = seen.frame;
			capture["class"] = class_name(class_at(entry.class_id));
			capture["sender"] = seen.sender;
			capture["targets"] = targets_of(entry);
			capture["broadcast"] = (entry.flags & EVENT_BROADCAST) != 0;
			capture["fields"] = decode(entry);

			if (g_captures.size() >= CAPTURE_SIZE) {
				g_captures.pop_front();
			}

			g_captures.emplace_back(std::move(capture));
			break;
		}
	}

	auto
	poll() -> void {
		g_fresh.clear();

		// nothing can ask for what a poll collects unless one of these is on
		if ((!g_settings.bridge.enabled && !g_settings.scripts.enabled) || !ready()) {
			return;
		}

		++g_polls;

		const auto max = g_system->entry_max;
		const auto next = g_system->entry_next;
		const auto last = g_system->entry_last;
		if (max == 0 || max > MAX_RING || next >= max || last >= max || !ddl::is_readable(g_system->entries, sizeof(EventEntry) * max)) {
			return;
		}

		// the first poll starts from now, not from whatever is still in the ring
		if (!g_cursor_valid) {
			g_cursor = next;
			g_cursor_valid = true;
			return;
		}

		// a cursor that is no longer inside [last, next] was overtaken: more than a
		// ring's worth went by, or the engine reset the queue under a load
		const auto live = (next + max - last) % max;
		const auto ahead = (g_cursor + max - last) % max;
		if (ahead > live) {
			++g_resets;
			g_cursor = last;
		}

		for (uint32_t taken = 0; g_cursor != next && taken < MAX_PER_POLL; ++taken) {
			const auto entry = g_system->entries[g_cursor];
			g_cursor = g_cursor + 1 < max ? g_cursor + 1 : 0;

			if (entry.event == nullptr || entry.class_id >= g_system->class_count) {
				continue;
			}

			g_fresh.emplace_back(entry);
			record(entry);
		}
	}

	auto
	fresh() -> const std::vector<EventEntry> & {
		return g_fresh;
	}

	auto
	tail(const char *filter, const size_t limit) -> nlohmann::json {
		nlohmann::json::array_t out;
		if (!ready()) {
			return out;
		}

		for (auto it = g_tail.rbegin(); it != g_tail.rend() && out.size() < limit; ++it) {
			const auto *name = class_name(class_at(it->class_id));
			if (filter != nullptr && filter[0] != '\0' && strstr(name, filter) == nullptr) {
				continue;
			}

			nlohmann::json entry;
			entry["sequence"] = it->sequence;
			entry["frame"] = it->frame;
			entry["class"] = name;
			entry["sender"] = it->sender;
			entry["targets"] = it->target_count;
			if (it->target_count == 1) {
				entry["target"] = it->first_target;
			}

			entry["broadcast"] = (it->flags & EVENT_BROADCAST) != 0;
			if (it->delay != 0.0f) {
				entry["delay"] = it->delay;
			}

			if (it->radius != 0.0f) {
				entry["radius"] = it->radius;
			}

			out.emplace_back(std::move(entry));
		}

		std::reverse(out.begin(), out.end());
		return out;
	}

	auto
	set_capture(const EventClassInfo *info, const bool enabled) -> void {
		if (info == nullptr) {
			return;
		}

		std::erase(g_capture_classes, info->id);
		if (enabled) {
			g_capture_classes.emplace_back(info->id);
		}
	}

	auto
	captures(const char *filter, const size_t limit) -> nlohmann::json {
		nlohmann::json::array_t out;
		for (auto it = g_captures.rbegin(); it != g_captures.rend() && out.size() < limit; ++it) {
			if (filter != nullptr && filter[0] != '\0' && (*it)["class"].get<std::string>().find(filter) == std::string::npos) {
				continue;
			}

			out.emplace_back(*it);
		}

		std::reverse(out.begin(), out.end());

		nlohmann::json result;
		nlohmann::json::array_t watching;
		for (const auto id : g_capture_classes) {
			watching.emplace_back(class_name(class_at(id)));
		}

		result["watching"] = watching;
		result["events"] = out;
		return result;
	}

	auto
	classes(const char *filter, const size_t limit) -> nlohmann::json {
		nlohmann::json::array_t out;
		if (!ready()) {
			return out;
		}

		for (int32_t id = 0; id < g_system->class_count && out.size() < limit; ++id) {
			const auto &info = g_system->classes[id];
			const auto *name = class_name(&info);
			if (filter != nullptr && filter[0] != '\0' && strstr(name, filter) == nullptr) {
				continue;
			}

			char hash[16];
			_snprintf_s(hash, sizeof(hash), _TRUNCATE, "0x%08x", info.type_info->type_id);

			nlohmann::json entry;
			entry["id"] = id;
			entry["name"] = name;
			entry["hash"] = hash;
			entry["size"] = info.type_info->allocation_size;
			entry["parent"] = info.parent != nullptr ? nlohmann::json(class_name(info.parent)) : nlohmann::json(nullptr);
			out.emplace_back(std::move(entry));
		}

		return out;
	}

	auto
	describe(const EventClassInfo *info) -> nlohmann::json {
		nlohmann::json out;
		if (info == nullptr) {
			return out;
		}

		const auto *type = info->type_info;
		char hash[16];
		_snprintf_s(hash, sizeof(hash), _TRUNCATE, "0x%08x", type->type_id);

		out["id"] = info->id;
		out["name"] = class_name(info);
		out["hash"] = hash;
		out["size"] = type->allocation_size;

		nlohmann::json::array_t parents;
		for (auto *parent = info->parent; parent != nullptr && parents.size() < MAX_PARENT_DEPTH; parent = parent->parent) {
			parents.emplace_back(class_name(parent));
		}

		out["parents"] = parents;

		nlohmann::json::array_t fields;
		for (int32_t index = 0; index < type->field_count; ++index) {
			char name[0x100];
			if (!ddl::read_string(type->field_names[index], name, sizeof(name))) {
				continue;
			}

			nlohmann::json field;
			field["name"] = name;
			field["type"] = type->field_types[index];
			field["array_type"] = type->field_array_types[index];
			field["offset"] = type->field_offsets[index];
			if (static_cast<ddl::FieldType>(type->field_types[index]) == ddl::FieldType::Struct && type->field_type_ids != nullptr) {
				if (const auto *nested = ddl::find_type(type->field_type_ids[index]); nested != nullptr) {
					char nested_name[0x100];
					if (ddl::read_string(nested->name, nested_name, sizeof(nested_name))) {
						field["struct"] = nested_name;
					}
				}
			}

			fields.emplace_back(std::move(field));
		}

		out["fields"] = fields;
		return out;
	}

	auto
	unavailable_reason() -> const char * {
		if (ready()) {
			return "";
		}

		return g_state == State::Unchecked ? "the engine has not registered its event classes yet" : g_broken;
	}

	auto
	status() -> nlohmann::json {
		nlohmann::json out;
		out["ready"] = ready();
		if (g_state != State::Ready) {
			out["reason"] = unavailable_reason();
			return out;
		}

		out["classes"] = g_system->class_count;
		out["classes_skipped"] = g_skipped;
		out["frame"] = g_system->frame_id;
		out["ring"] = { { "max", g_system->entry_max }, { "next", g_system->entry_next }, { "last", g_system->entry_last } };
		out["polls"] = g_polls;
		out["seen"] = g_sequence;
		out["cursor_resets"] = g_resets;
		out["queued"] = g_queued;
		out["queue_failures"] = g_queue_failures;
		return out;
	}
} // namespace rivet_hook::events

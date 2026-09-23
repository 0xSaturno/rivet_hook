// SPDX-FileCopyrightText: 2025-2026 Neptuwunium
//
// SPDX-License-Identifier: EUPL-1.2

#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include <nlohmann/json.hpp>

#include "ddl_visit.hpp"
#include "game/event_system.hpp"

// the engine's event queue: nearly every cross system verb in the game (warps,
// damage, vanity overrides, time scale requests, ui sounds, cinematics) is an
// event, so one way to send them and one way to watch them reaches most of it.
// everything past init runs on the pumping thread.
namespace rivet_hook::events {
	// resolves the event system and QueueEvent. scans only, reads no engine state,
	// so it runs during hook setup. safe to call more than once.
	auto
	init() -> void;

	// true once the class table has been read and checked. the first call does
	// that check, so it has to come from the pumping thread.
	auto
	ready() -> bool;

	// a registered event class by exact name ("PerformWarpEvent") or by its name
	// hash written as hex ("0x38008fe3"). null if there is none.
	auto
	find_class(const char *name) -> const game::EventClassInfo *;

	// the class with this id, null when out of range
	auto
	class_at(uint16_t id) -> const game::EventClassInfo *;

	// the class name, or "" when it is not readable
	auto
	class_name(const game::EventClassInfo *info) -> const char *;

	// true when class_id is parent_id or derives from it
	auto
	is_type(uint16_t class_id, uint16_t parent_id) -> bool;

	struct Request {
		uint32_t sender = 0;
		std::vector<uint32_t> targets;
		bool exclude_targets = false;
		bool broadcast = true;
		float radius = 0.0f;
		float delay = 0.0f;
		bool has_position = false;
		float position[4] {}; // the engine reads a 16 byte vector
	};

	// queues one event through the engine's own QueueEvent and returns the live
	// instance, still writable until it is dispatched later in the frame. null on
	// failure, with the reason in reason.
	auto
	queue(const game::EventClassInfo *info, const Request &request, const char **reason) -> uint8_t *;

	// writes one field of a queued event. path may step into nested structs,
	// "Destination.Position.X".
	auto
	set_field(const game::EventClassInfo *info, uint8_t *event, const char *path, const ddl::Value &value, const char **reason) -> bool;

	// true when the class has a field at this path
	auto
	has_field(const game::EventClassInfo *info, const char *path) -> bool;

	// reads one field of an event, by the same kind of path
	auto
	get_field(const game::EventClassInfo *info, const uint8_t *event, const char *path) -> ddl::Value;

	// picks up the events queued on the main queue since the last poll. called
	// once a pump, before the scripts run.
	auto
	poll() -> void;

	// what the last poll picked up, oldest first. only valid until the next poll.
	auto
	fresh() -> const std::vector<game::EventEntry> &;

	// the sender handle stored in the event, 0 when the class has none
	auto
	sender_of(const game::EventEntry &entry) -> uint32_t;

	// the decoded fields of an event, nested structs as objects
	auto
	decode(const game::EventEntry &entry) -> nlohmann::json;

	// the most recent events seen, newest last, optionally only classes whose name
	// contains filter
	auto
	tail(const char *filter, size_t limit) -> nlohmann::json;

	// keeps the decoded fields of every event of this class (and its subclasses)
	// that goes by, for captures() to hand back
	auto
	set_capture(const game::EventClassInfo *info, bool enabled) -> void;

	auto
	captures(const char *filter, size_t limit) -> nlohmann::json;

	// registered classes whose name contains filter, with hash, parent and size
	auto
	classes(const char *filter, size_t limit) -> nlohmann::json;

	// the fields a class carries, name type and offset, for building a send
	auto
	describe(const game::EventClassInfo *info) -> nlohmann::json;

	// why ready() answers false, as static text
	auto
	unavailable_reason() -> const char *;

	auto
	status() -> nlohmann::json;
} // namespace rivet_hook::events

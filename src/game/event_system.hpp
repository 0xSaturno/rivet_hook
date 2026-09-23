// SPDX-FileCopyrightText: 2025-2026 Neptuwunium
//
// SPDX-License-Identifier: EUPL-1.2

#pragma once

#include <cstdint>

#include "ddl.hpp"

namespace rivet_hook::game {
#pragma pack(push, 1)
	using event_callback_t = void (*)(void *event);

	// one registered event class. the table is indexed by class id, and the ids
	// are handed out in registration order, so entry i has id i.
	struct EventClassInfo {
		uint16_t id;
		uint8_t padding[6];
		event_callback_t post_queue;
		event_callback_t pre_dispatch;
		const EventClassInfo *parent;
		const DDLTypeInfo *type_info; // type_id is the hash QueueEvent looks up
	};

	static_assert(sizeof(EventClassInfo) == 0x28, "EventClassInfo size is not 0x28");

	enum EventEntryFlags : uint16_t {
		EVENT_BROADCAST = 1 << 0,
		EVENT_DELAY_BY_FRAME = 1 << 1,
		EVENT_EXCLUDE_TARGETS = 1 << 2,
	};

	// one queued event. a single target is stored in the pointer itself.
	struct EventEntry {
		void *event;
		uint16_t class_id;
		uint16_t flags;
		uint32_t target_count;
		uint32_t *targets;
		float delay;
		float radius;

		auto
		target(const uint32_t index) const -> uint32_t {
			if (target_count == 1) {
				return static_cast<uint32_t>(reinterpret_cast<uintptr_t>(targets));
			}

			return targets[index];
		}
	};

	static_assert(sizeof(EventEntry) == 0x20, "EventEntry size is not 0x20");

	// the main event queue is a ring: entries [last, next) are live, and each
	// lives two frames before FrameUpdate retires it. events queued from worker
	// threads sit in per thread queues and only pass through this ring if delayed.
	struct EventSystem {
		EventEntry *entries;
		uint32_t entry_max;
		uint32_t entry_next;
		uint32_t entry_last;
		uint8_t unknown[0x11d0 - 0x14];
		uint32_t frame_id;
		uint16_t matched_event_id;
		uint8_t padding[2];
		EventClassInfo classes[4096];
		int32_t class_count;
		uint8_t unknown2[4];
		uint8_t class_lookup[0x30]; // name hash -> class id
		int16_t *parent_lookup;
	};

	static_assert(offsetof(EventSystem, frame_id) == 0x11d0, "EventSystem::frame_id is not at 0x11d0");
	static_assert(offsetof(EventSystem, classes) == 0x11d8, "EventSystem::classes is not at 0x11d8");
	static_assert(offsetof(EventSystem, class_count) == 0x291d8, "EventSystem::class_count is not at 0x291d8");
	static_assert(offsetof(EventSystem, class_lookup) == 0x291e0, "EventSystem::class_lookup is not at 0x291e0");
	static_assert(offsetof(EventSystem, parent_lookup) == 0x29210, "EventSystem::parent_lookup is not at 0x29210");

	using queue_event_t = void *(*)(EventSystem *system, uint32_t name_hash, uint32_t sender, const uint32_t *targets, int32_t target_count, bool exclude_targets, const float *position, int32_t locator_hash, bool broadcast, float radius, float delay, const char *ddl_data, const char *strings);
#pragma pack(pop)
} // namespace rivet_hook::game

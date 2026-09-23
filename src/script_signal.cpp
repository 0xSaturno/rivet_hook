// SPDX-FileCopyrightText: 2025-2026 Neptuwunium
//
// SPDX-License-Identifier: EUPL-1.2

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <array>
#include <cstdlib>
#include <cstring>

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

	auto
	init() -> void {
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

	// the class is ScriptAction or derives from it
	static auto
	is_script_node(const ComponentInfo *type) -> bool {
		const auto named = [](const ComponentInfo *info) {
			char name[0x40];
			return info != nullptr && ddl::is_readable(info, sizeof(ComponentInfo)) && ddl::read_string(info->name, name, sizeof(name)) && strcmp(name, "ScriptAction") == 0;
		};

		if (named(type)) {
			return true;
		}

		for (uint8_t i = 0; i < type->parent_count && i < std::size(type->parent_classes); ++i) {
			if (named(type->parent_classes[i])) {
				return true;
			}
		}

		return false;
	}

	auto
	nodes(const char *filter, const size_t limit, bool (*expired)()) -> nlohmann::json {
		nlohmann::json out;
		nlohmann::json::array_t found;
		auto truncated = false;

		if (g_SceneManager == nullptr || g_SceneManager->actors == nullptr || !ddl::is_readable(g_SceneManager->actors, sizeof(Actor))) {
			out["nodes"] = found;
			out["truncated"] = false;
			return out;
		}

		const auto count = g_SceneManager->actorMax;
		for (int32_t index = 0; index < count; ++index) {
			if ((index & 0x3ff) == 0 && expired != nullptr && expired()) {
				truncated = true;
				break;
			}

			const auto *actor = &g_SceneManager->actors[index];
			// not Actor::IsValid: that wants a scene object, which script actors lack
			if (actor->generation == 0 || (actor->flags & ActorFlag::Allocated) == 0) {
				continue;
			}

			if (actor->components == nullptr || actor->componentCount <= 0 || actor->componentCount > 4 || !ddl::is_readable(actor->components, sizeof(ComponentPointer) * actor->componentCount)) {
				continue;
			}

			for (auto c = 0; c < actor->componentCount; ++c) {
				const auto [type, instance] = actor->components[c];
				if (type == nullptr || instance == nullptr || !ddl::is_readable(type, sizeof(ComponentInfo)) || !is_script_node(type)) {
					continue;
				}

				char name[0x100];
				if (!ddl::read_string(type->name, name, sizeof(name))) {
					continue;
				}

				if (filter != nullptr && filter[0] != '\0' && strstr(name, filter) == nullptr) {
					continue;
				}

				if (found.size() >= limit) {
					truncated = true;
					break;
				}

				char uid[24];
				_snprintf_s(uid, sizeof(uid), _TRUNCATE, "%016llx", scene_query::uid_of(actor));

				nlohmann::json node;
				node["actor"] = EngineHandle { .id = static_cast<uint32_t>(index), .generation = actor->generation }.value;
				node["uid"] = uid;
				node["class"] = name;
				found.emplace_back(std::move(node));
			}

			if (found.size() >= limit && truncated) {
				break;
			}
		}

		out["nodes"] = found;
		out["truncated"] = truncated;
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

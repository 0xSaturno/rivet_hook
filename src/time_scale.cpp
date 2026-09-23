// SPDX-FileCopyrightText: 2025-2026 Neptuwunium
//
// SPDX-License-Identifier: EUPL-1.2

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <array>
#include <cctype>
#include <cmath>
#include <cstring>

#include "time_scale.hpp"

#include "ddl_visit.hpp"
#include "events.hpp"
#include "game_thread.hpp"
#include "runtime.hpp"
#include "signature.hpp"

using namespace rivet_hook::game;

namespace rivet_hook::time_scale {
	// measured on the shipping exe: the scale the game runs at, then one requested
	// scale per channel. the level unload reset writes 1.0 to all of them.
	constexpr uint32_t APPLIED_OFFSET = 0x18;
	constexpr uint32_t CHANNELS_OFFSET = 0x20;
	constexpr uint32_t READ_SIZE = CHANNELS_OFFSET + CHANNEL_COUNT * sizeof(float);

	// the fx type that defers to the channel's own default
	constexpr int32_t FX_CHANNEL_DEFAULT = 0;
	// shows up in the system's per channel context, which audio reads
	constexpr const char *CONTEXT = "rivet";

	using set_channel_t = void (*)(void *system, int32_t channel, float scale, float ramp, const char *context, int32_t fx_type);
	using clear_channel_t = void (*)(void *system, int32_t channel);

	static uint8_t *g_system = nullptr;
	static set_channel_t g_set_channel = nullptr;
	static clear_channel_t g_clear_channel = nullptr;
	static const char *g_broken = "not resolved";

	static bool g_names_resolved = false;
	static std::array<const char *, CHANNEL_COUNT> g_names {};

	auto
	init() -> void {
		if (g_system != nullptr) {
			return;
		}

		g_system = static_cast<uint8_t *>(load_rel_var(find_address(TIME_SCALE_SYSTEM_SIGNATURE), TIME_SCALE_SYSTEM_ADDRESS));
		g_set_channel = reinterpret_cast<set_channel_t>(find_address(SET_CHANNEL_TIME_SCALE_SIGNATURE));
		g_clear_channel = reinterpret_cast<clear_channel_t>(find_address(CLEAR_CHANNEL_TIME_SCALE_SIGNATURE));

		if (g_system == nullptr || g_set_channel == nullptr || g_clear_channel == nullptr) {
			g_broken = g_system == nullptr ? "the time scale system was not found" : "a time scale setter was not found";
			g_system = nullptr;
			g_output << "[time] " << g_broken << ", time scale is unavailable\n";
			g_output.flush();
			return;
		}

		g_output << "[time] time scale system at " << static_cast<void *>(g_system) << "\n";
		g_output.flush();
	}

	auto
	ready() -> bool {
		return g_system != nullptr && ddl::is_readable(g_system, READ_SIZE);
	}

	auto
	unavailable_reason() -> const char * {
		if (g_system == nullptr) {
			return g_broken;
		}

		return ready() ? "" : "the time scale system is not readable";
	}

	// the channel names are the TimeScaleChannel enum, which the shipping DDL
	// carries on every field of that type. TimeScaleCancelEvent has one.
	static auto
	resolve_names() -> void {
		if (g_names_resolved) {
			return;
		}

		const auto *info = events::find_class("TimeScaleCancelEvent");
		if (info == nullptr) {
			return;
		}

		const auto *type = info->type_info;
		const auto index = ddl::find_field(type, "Channel");
		if (index < 0 || type->field_ex == nullptr || type->field_ex[index] == nullptr) {
			return;
		}

		const auto *select = static_cast<const DDLSelectTypeInfo *>(type->field_ex[index]);
		if (!ddl::is_readable(select, sizeof(DDLSelectTypeInfo)) || select->select_info == nullptr || !ddl::is_readable(select->select_info, sizeof(DDLTypeSelectInfo))) {
			return;
		}

		const auto *options = select->select_info;
		if (options->names == nullptr || options->count != CHANNEL_COUNT || !ddl::is_readable(options->names, sizeof(const char *) * CHANNEL_COUNT)) {
			g_output << "[time] TimeScaleChannel has " << options->count << " entries, expected " << CHANNEL_COUNT << "; channels go by number only\n";
			g_output.flush();
			g_names_resolved = true;
			return;
		}

		for (int32_t i = 0; i < CHANNEL_COUNT; ++i) {
			g_names[i] = options->names[i];
		}

		g_names_resolved = true;
	}

	auto
	channel_name(const int32_t channel) -> const char * {
		resolve_names();
		if (channel < 0 || channel >= CHANNEL_COUNT || g_names[channel] == nullptr) {
			return "";
		}

		return g_names[channel];
	}

	auto
	channel_index(const char *name) -> int32_t {
		if (name == nullptr || name[0] == '\0') {
			return -1;
		}

		if (std::isdigit(static_cast<unsigned char>(name[0]))) {
			char *end = nullptr;
			const auto number = strtol(name, &end, 10);
			return *end == '\0' && number >= 0 && number < CHANNEL_COUNT ? static_cast<int32_t>(number) : -1;
		}

		resolve_names();
		for (int32_t i = 0; i < CHANNEL_COUNT; ++i) {
			char text[64];
			if (g_names[i] == nullptr || !ddl::read_string(g_names[i], text, sizeof(text))) {
				continue;
			}

			// "kGame" matches "kGame", "Game" and "game"
			const auto *bare = text[0] == 'k' ? text + 1 : text;
			if (_stricmp(text, name) == 0 || _stricmp(bare, name) == 0) {
				return i;
			}
		}

		return -1;
	}

	auto
	applied() -> float {
		return ready() ? *reinterpret_cast<const float *>(g_system + APPLIED_OFFSET) : 1.0f;
	}

	auto
	channel_scale(const int32_t channel) -> float {
		if (!ready() || channel < 0 || channel >= CHANNEL_COUNT) {
			return 1.0f;
		}

		return reinterpret_cast<const float *>(g_system + CHANNELS_OFFSET)[channel];
	}

	// the engine calls on their own, so a fault is caught with nothing to unwind
	static auto
	call_set(const int32_t channel, const float scale, const float ramp) -> bool {
#ifdef _MSC_VER
		__try {
#endif
			g_set_channel(g_system, channel, scale, ramp, CONTEXT, FX_CHANNEL_DEFAULT);
			return true;
#ifdef _MSC_VER
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			return false;
		}
#endif
	}

	static auto
	call_clear(const int32_t channel) -> bool {
#ifdef _MSC_VER
		__try {
#endif
			g_clear_channel(g_system, channel);
			return true;
#ifdef _MSC_VER
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			return false;
		}
#endif
	}

	static auto
	check_call(const int32_t channel, const char **reason) -> bool {
		const auto fail = [reason](const char *text) {
			if (reason != nullptr) {
				*reason = text;
			}

			return false;
		};

		if (!ready()) {
			return fail(unavailable_reason());
		}

		if (channel < 0 || channel >= CHANNEL_COUNT) {
			return fail("no such time scale channel");
		}

		// clearing destroys components, and a set can too through the level's own
		// callbacks. neither may run beside the game thread's actor update
		if (!game_thread::on_game_thread()) {
			return fail("time scale can only change on the game thread, and it is not pumping (loading?)");
		}

		return true;
	}

	auto
	set(const int32_t channel, const float scale, const float ramp, const char **reason) -> bool {
		if (!check_call(channel, reason)) {
			return false;
		}

		// zero would freeze the game clock, and nothing downstream expects that
		if (!std::isfinite(scale) || scale <= 0.0f || scale > 100.0f) {
			if (reason != nullptr) {
				*reason = "the scale has to be above 0 and at most 100";
			}

			return false;
		}

		if (!call_set(channel, scale, std::isfinite(ramp) ? ramp : -1.0f)) {
			if (reason != nullptr) {
				*reason = "SetChannelTimeScale faulted";
			}

			return false;
		}

		return true;
	}

	auto
	clear(const int32_t channel, const char **reason) -> bool {
		if (!check_call(channel, reason)) {
			return false;
		}

		if (!call_clear(channel)) {
			if (reason != nullptr) {
				*reason = "ClearChannelTimeScale faulted";
			}

			return false;
		}

		return true;
	}

	auto
	status() -> nlohmann::json {
		nlohmann::json out;
		out["ready"] = ready();
		if (!ready()) {
			out["reason"] = unavailable_reason();
			return out;
		}

		out["applied"] = applied();

		// only the channels asking for something other than normal speed
		nlohmann::json channels = nlohmann::json::object();
		for (int32_t i = 0; i < CHANNEL_COUNT; ++i) {
			if (const auto scale = channel_scale(i); scale != 1.0f) {
				const auto *name = channel_name(i);
				channels[name[0] != '\0' ? name : std::to_string(i)] = scale;
			}
		}

		out["channels"] = channels;
		return out;
	}
} // namespace rivet_hook::time_scale

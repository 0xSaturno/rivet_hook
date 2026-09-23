// SPDX-FileCopyrightText: 2025-2026 Neptuwunium
//
// SPDX-License-Identifier: EUPL-1.2

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cmath>
#include <cstring>

#include "hud.hpp"

#include "ddl_visit.hpp"
#include "game_thread.hpp"
#include "runtime.hpp"
#include "signature.hpp"

namespace rivet_hook::hud {
	// the pause tab argument. anything but the map tab also becomes the pause
	// menu's default tab, which a notification has no business doing
	constexpr int32_t PAUSE_TAB_MAP = 0;

	// the game copies into fixed buffers of its own; this only bounds what is
	// handed over
	constexpr size_t MAX_TEXT = 512;

	using show_message_t = void (*)(void *hud, int32_t type, const char *message, float duration, const char *sub_message, const char *prompt, int32_t pause_tab, const char *icon);
	using get_hud_t = void *(*)();

	static show_message_t g_show_message = nullptr;
	static get_hud_t g_get_hud = nullptr;
	static const uint8_t *g_messages_enabled = nullptr;

	struct Named {
		const char *name;
		MessageType type;
	};

	constexpr Named TYPES[] = {
		{ "generic", MessageType::Generic },
		{ "center", MessageType::Center },
		{ "pickup", MessageType::Pickup },
		{ "location", MessageType::Location },
		{ "planet", MessageType::Planet },
		{ "corner", MessageType::Corner },
		{ "tutorial", MessageType::Tutorial },
		{ "arena_wave", MessageType::ArenaWave },
		{ "arena_reward", MessageType::ArenaReward },
	};

	auto
	init() -> void {
		if (g_show_message != nullptr) {
			return;
		}

		const auto show = find_address(HUD_SHOW_MESSAGE_SIGNATURE);
		const auto action = find_address(HUD_MESSAGE_ACTION_SIGNATURE);
		if (show == 0 || action == 0) {
			g_output << "[hud] the hud message call was not found, rivet.notify is unavailable\n";
			g_output.flush();
			return;
		}

		// cmp byte [rip + x], 0: the immediate follows the displacement, so rip
		// is one byte past it
		g_messages_enabled = reinterpret_cast<const uint8_t *>(show + HUD_SHOW_MESSAGE_ENABLED_END + *reinterpret_cast<const int32_t *>(show + HUD_SHOW_MESSAGE_ENABLED_ADDRESS));
		g_get_hud = static_cast<get_hud_t>(load_rel_var(action + HUD_MESSAGE_ACTION_GET_HUD, 1));
		g_show_message = reinterpret_cast<show_message_t>(show);

		g_output << "[hud] ShowMessage at " << reinterpret_cast<void *>(g_show_message) << "\n";
		g_output.flush();
	}

	auto
	message_type(const char *name, MessageType &out) -> bool {
		if (name == nullptr) {
			return false;
		}

		for (const auto &[text, type] : TYPES) {
			if (_stricmp(text, name) == 0) {
				out = type;
				return true;
			}
		}

		return false;
	}

	// the engine calls on their own, so a fault is caught with nothing to unwind
	static auto
	call_get_hud(void **out) -> bool {
#ifdef _MSC_VER
		__try {
#endif
			*out = g_get_hud();
			return true;
#ifdef _MSC_VER
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			return false;
		}
#endif
	}

	static auto
	call_show(void *player_hud, const int32_t type, const char *text, const float duration, const char *sub) -> bool {
#ifdef _MSC_VER
		__try {
#endif
			g_show_message(player_hud, type, text, duration, sub, nullptr, PAUSE_TAB_MAP, nullptr);
			return true;
#ifdef _MSC_VER
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			return false;
		}
#endif
	}

	// the game formats the sub message with it as the format string, so a bare %
	// in script text would read arguments that were never passed
	static auto
	escape_percent(const char *text, char *out, const size_t size) -> void {
		size_t at = 0;
		for (const auto *c = text; *c != '\0' && at + 2 < size; ++c) {
			if (*c == '%') {
				out[at++] = '%';
			}

			out[at++] = *c;
		}

		out[at] = '\0';
	}

	auto
	notify(const MessageType type, const char *text, const float duration, const char *sub, const char **reason) -> bool {
		const auto fail = [reason](const char *why) {
			if (reason != nullptr) {
				*reason = why;
			}

			return false;
		};

		if (g_show_message == nullptr || g_get_hud == nullptr) {
			return fail("the hud message call was not found");
		}

		if (text == nullptr || text[0] == '\0') {
			return fail("there is no text to show");
		}

		if (!std::isfinite(duration) || duration <= 0.0f || duration > 60.0f) {
			return fail("the duration has to be above 0 and at most 60 seconds");
		}

		// the hud is gameplay ui, updated beside the actors
		if (!game_thread::on_game_thread()) {
			return fail("hud messages can only be shown on the game thread, and it is not pumping (loading?)");
		}

		if (g_messages_enabled != nullptr && ddl::is_readable(g_messages_enabled, 1) && *g_messages_enabled == 0) {
			return fail("hud messages are turned off in the game's options");
		}

		void *player_hud = nullptr;
		if (!call_get_hud(&player_hud) || player_hud == nullptr) {
			return fail("the player hud is not loaded");
		}

		char message[MAX_TEXT];
		_snprintf_s(message, sizeof(message), _TRUNCATE, "%s", text);

		char second[MAX_TEXT];
		escape_percent(sub != nullptr ? sub : "", second, sizeof(second));

		if (!call_show(player_hud, static_cast<int32_t>(type), message, duration, second[0] != '\0' ? second : nullptr)) {
			g_output << "[hud] ShowMessage faulted\n";
			g_output.flush();
			return fail("ShowMessage faulted");
		}

		return true;
	}
} // namespace rivet_hook::hud

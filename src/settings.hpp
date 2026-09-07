// SPDX-FileCopyrightText: 2025-2026 Neptuwunium
//
// SPDX-License-Identifier: EUPL-1.2

#pragma once

#include <map>
#include <string>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#include "windows.h"

#include <cstdint>

namespace rivet_hook {
	constexpr static auto settings_name = R"(.\rivet.toml)";

	struct Settings {
		struct Utility {
			bool suppress_crash_handler = true;
			bool attach_context_log = false;
			bool attach_log = false;
			bool unpause_focus = false;
		} utility;

		struct Overlay {
			bool enabled = false;
			int toggle_key = VK_F3;
			int spawn_debug_actor_key = VK_F4;
			int release_key = VK_NUMPAD5;
		} overlay;

		struct DDL {
			bool dump_versions = false;
			bool dump_ddl = false;
			bool dump_components = false;
			bool debug_ddl = false;
		} ddl;

		struct Bridge {
			bool enabled = false;
			std::string pipe_name { "rivet_hook" };
		} bridge;

		struct Scripts {
			bool enabled = false;
			std::string path { "scripts" };
			int reload_key = VK_F6;
			// wall clock cap for one callback, in milliseconds. everything lua runs
			// runs on the render thread, so this is the stutter budget.
			int budget_ms = 8;
			// how many vm instructions between budget checks
			int check_interval = 10000;
			// consecutive failures before a callback is switched off
			int error_limit = 3;
		} scripts;

		struct RenderDoc {
			bool enabled = false;
			std::string dll_path { "renderdoc.dll" };
		} renderdoc;

		struct Assets {
			bool enabled = true;
			std::vector<std::string> paths = { "mods/default" };
			bool disable_dstorage = true;
			bool log = false;
			bool verbose = false;
		} assets;

		struct Log {
			bool cohtml = false;
			bool paths = false;
			bool loose_io = false;
			bool asset_io = false;
			bool id = false;
			bool pointers = false;
		} log;

		struct AddressCache {
			std::string fingerprint;
			std::map<std::string, std::vector<intptr_t>> addresses;
		} address_cache;

		static auto
		load() -> Settings;
		auto
		save() const -> void;
	};
} // namespace rivet_hook

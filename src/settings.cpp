// SPDX-FileCopyrightText: 2025-2026 Neptuwunium
//
// SPDX-License-Identifier: EUPL-1.2

#include <chrono>
#include <filesystem>
#include <format>
#include <fstream>
#include <mutex>
#include <thread>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <nlohmann/json.hpp>
#include <toml.hpp>

#include "runtime.hpp"
#include "settings.hpp"
#include "vk_enum.hpp"

#define LOAD_SETTING(group, type, name) \
if (tbl.contains(#group) && tbl.at(#group).is_table()) { \
	settings.group.name = toml::find_or<type>(tbl.at(#group), #name, settings.group.name); \
}

#define LOAD_SETTING_KEY(group, name) \
if (tbl.contains(#group) && tbl.at(#group).is_table() && tbl.at(#group).contains(#name)) { \
	auto var = tbl.at(#group).at(#name); \
	if (var.is_string()) settings.group.name = StringToVKey(var.as_string()); \
	else if (var.is_integer()) settings.group.name = static_cast<int>(var.as_integer()); \
}

#define SAVE_SETTING(group, name, comment) \
	tbl[#group][#name] = group.name; \
	tbl[#group][#name].comments().clear(); \
	tbl[#group][#name].comments().push_back(" " comment)

#define SAVE_SETTING_KEY(group, name, comment) \
	if(const auto value = VKeyToString(group.name); !value.empty()) tbl[#group][#name] = value; \
	else tbl[#group][#name] = group.name; \
	tbl[#group][#name].comments().clear(); \
	tbl[#group][#name].comments().push_back(" " comment)

#define CREATE_TABLE(group) \
	if (!tbl.contains(#group) || !tbl.at(#group).is_table()) tbl[#group] = toml_table();

using toml_table = toml::basic_value<toml::ordered_type_config>::table_type;

namespace rivet_hook {
	// anchor the file next to the game exe. a bare relative path follows the working
	// directory, which launchers don't always set, and a miss there looks like a reset.
	auto
	settings_path() -> const std::filesystem::path & {
		static const auto path = [] {
			std::wstring buffer(MAX_PATH, L'\0');
			for (;;) {
				const auto length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
				if (length == 0) {
					return std::filesystem::path(settings_name);
				}
				if (length < buffer.size()) {
					buffer.resize(length);
					break;
				}
				buffer.resize(buffer.size() * 2);
			}
			return std::filesystem::path(buffer).parent_path() / std::filesystem::path(settings_name).filename();
		}();
		return path;
	}

	auto
	valid_fingerprint(Settings &settings) -> bool {
		const auto dos = reinterpret_cast<PIMAGE_DOS_HEADER>(g_game_module);

		if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
			return false;
		}

		const auto headers = reinterpret_cast<PIMAGE_NT_HEADERS>(reinterpret_cast<PBYTE>(g_game_module) + dos->e_lfanew);
		if (headers->Signature != IMAGE_NT_SIGNATURE) {
			return false;
		}

		if (const auto fingerprint = std::format("{}:{}#{}?2", headers->OptionalHeader.AddressOfEntryPoint, headers->OptionalHeader.SizeOfImage, headers->FileHeader.TimeDateStamp);
			settings.address_cache.fingerprint.empty() || fingerprint != settings.address_cache.fingerprint) {
			g_output << "[rivet] game version mismatch, invalidating pointers\n";
			settings.address_cache.fingerprint = fingerprint;
			return false;
		}

		return true;
	}

	auto
	Settings::load() -> Settings {
		Settings settings;

		const auto &path = settings_path();
		if (std::error_code ec; std::filesystem::exists(path, ec)) {
			try {
				// an editor or sync tool can hold the file for a moment while saving it,
				// retry a few times before giving up on it
				auto tbl = [&path] {
					for (auto attempt = 0;; ++attempt) {
						try {
							return toml::parse(path);
						} catch (const toml::file_io_error &) {
							if (attempt >= 4) {
								throw;
							}
							std::this_thread::sleep_for(std::chrono::milliseconds(50));
						}
					}
				}();
				if (!tbl.is_table()) {
					tbl = toml_table();
				}

				LOAD_SETTING(utility, bool, suppress_crash_handler);
				LOAD_SETTING(utility, bool, attach_context_log);
				LOAD_SETTING(utility, bool, attach_log);
				LOAD_SETTING(utility, bool, unpause_focus);

				LOAD_SETTING(overlay, bool, enabled);
				LOAD_SETTING_KEY(overlay, toggle_key);
				LOAD_SETTING_KEY(overlay, spawn_debug_actor_key);

				LOAD_SETTING(ddl, bool, dump_versions);
				LOAD_SETTING(ddl, bool, dump_components);
				LOAD_SETTING(ddl, bool, dump_ddl);
				LOAD_SETTING(ddl, bool, debug_ddl);

				LOAD_SETTING(bridge, bool, enabled);
				LOAD_SETTING(bridge, std::string, pipe_name);

				LOAD_SETTING(scripts, bool, enabled);
				LOAD_SETTING(scripts, std::string, path);
				LOAD_SETTING_KEY(scripts, reload_key);
				LOAD_SETTING(scripts, int, budget_ms);
				LOAD_SETTING(scripts, int, check_interval);
				LOAD_SETTING(scripts, int, error_limit);

				LOAD_SETTING(renderdoc, bool, enabled);
				LOAD_SETTING(renderdoc, std::string, dll_path);

				LOAD_SETTING(assets, bool, enabled);
				LOAD_SETTING(assets, bool, log);
				LOAD_SETTING(assets, bool, verbose);
				// LOAD_SETTING(assets, bool, disable_dstorage);
				LOAD_SETTING(assets, std::vector<std::string>, paths);

				LOAD_SETTING(log, bool, cohtml);
				LOAD_SETTING(log, bool, paths);
				LOAD_SETTING(log, bool, loose_io);
				LOAD_SETTING(log, bool, asset_io);
				LOAD_SETTING(log, bool, id);
				LOAD_SETTING(log, bool, pointers);

				settings.address_cache.fingerprint = toml::find_or<std::string>(tbl, "__GAME_ID__", settings.address_cache.fingerprint);
				if (valid_fingerprint(settings) && tbl.contains("address_cache")) {
					if (tbl["address_cache"].is_table()) {
						for (auto &[key, values] : tbl["address_cache"].as_table()) {
							if (!values.is_array()) {
								continue;
							}

							settings.address_cache.addresses[key] = {};

							for (auto &value : values.as_array()) {
								if (!value.is_integer()) {
									continue;
								}
								settings.address_cache.addresses[key].emplace_back(value.as_integer() + reinterpret_cast<intptr_t>(g_game_module));
							}
						}
					}
				}
			} catch (const std::exception &failure) {
				// run on defaults this session, but never write them over the user's file
				settings = Settings {};
				settings.read_failed = true;
				g_output << "[rivet] could not read " << path.string() << ", using defaults and leaving the file untouched: " << failure.what() << "\n";
				g_output.flush();
			} catch (...) {
				settings = Settings {};
				settings.read_failed = true;
				g_output << "[rivet] could not read " << path.string() << ", using defaults and leaving the file untouched\n";
				g_output.flush();
			}
		}

		return settings;
	}

	auto
	Settings::save() const -> void {
		if (read_failed) {
			return;
		}

		toml::basic_value<toml::ordered_type_config> tbl = toml_table();

		CREATE_TABLE(utility);
		CREATE_TABLE(overlay);
		CREATE_TABLE(ddl);
		CREATE_TABLE(bridge);
		CREATE_TABLE(scripts);
		CREATE_TABLE(renderdoc);
		CREATE_TABLE(assets);
		CREATE_TABLE(log);
		CREATE_TABLE(address_cache);

		SAVE_SETTING(utility, suppress_crash_handler, "disable the exception handler allowing for debuggers to attach without invoking the crash handler");
		SAVE_SETTING(utility, attach_context_log, "redirect the internal logger context state to rivet.log; disable by default for clutter reasons");
		SAVE_SETTING(utility, attach_log, "redirect the internal logger to rivet.log; disable by default because the same line is printed frequently");
		SAVE_SETTING(utility, unpause_focus, "prevent the game from pausing when alt tabbed");

		SAVE_SETTING(overlay, enabled, "enable imgui overlay for various in game stuffs");
		SAVE_SETTING_KEY(overlay, toggle_key, "what key to toggle the imgui overlay with");
		SAVE_SETTING_KEY(overlay, spawn_debug_actor_key, "what key to debug spawn an actor with");

		SAVE_SETTING(ddl, dump_versions, "dumps versions to json; disable by default for clutter reasons");
		SAVE_SETTING(ddl, dump_components, "dumps components to json; disable by default for clutter reasons");
		SAVE_SETTING(ddl, dump_ddl, "dumps DDL type structures to json; disable by default for clutter reasons");
		SAVE_SETTING(ddl, debug_ddl, "logs DDL type information; disable by default because log noise");

		SAVE_SETTING(bridge, enabled, "expose a named pipe for external tooling; disable by default because it is an rpc surface inside the game");
		SAVE_SETTING(bridge, pipe_name, R"(name of the pipe, reachable as \\.\pipe\<name>)");

		SAVE_SETTING(scripts, enabled, "run lua scripts from the scripts directory; disable by default because a script runs arbitrary code inside the game");
		SAVE_SETTING(scripts, path, "directory the .lua files are loaded from, relative to the game exe");
		SAVE_SETTING_KEY(scripts, reload_key, "what key to reload every script with");
		SAVE_SETTING(scripts, budget_ms, "wall clock budget for one script callback in milliseconds; scripts run on the render thread so overrunning this is a visible stutter");
		SAVE_SETTING(scripts, check_interval, "how many lua instructions run between budget checks");
		SAVE_SETTING(scripts, error_limit, "consecutive errors before a callback is switched off");

		SAVE_SETTING(renderdoc, enabled, "loads renderdoc.dll into the game; disable by default because it has issues with ReShade");
		SAVE_SETTING(renderdoc, dll_path, "path to renderdoc/dll");

		SAVE_SETTING(assets, enabled, "enables loose asset loading");
		SAVE_SETTING(assets, paths, "list of paths to load assets from, order is priority. first entry is least priority.");
		SAVE_SETTING(assets, log, "logs when mod files are accessed; disable by default because log noise");
		SAVE_SETTING(assets, verbose, "logs the mod pipeline state; disable by default because log noise");
		SAVE_SETTING(assets, disable_dstorage, "force disables directstorage, will have a performance impact. dstorage is currently not supported");

		SAVE_SETTING(log, cohtml, "logs coherent ui url decode requests; disable by default because log noise");
		SAVE_SETTING(log, paths, "logs asset paths as they are loaded; disable by default because log noise");
		SAVE_SETTING(log, loose_io, "logs loose io; disable by default because log noise");
		SAVE_SETTING(log, asset_io, "logs asset paths as they are assets; disable by default because log noise");
		SAVE_SETTING(log, id, "logs asset ids as they are hashed; disable by default because log noise");
		SAVE_SETTING(log, pointers, "logs pointer information; disable by default because log noise");

		tbl["__GAME_ID__"] = address_cache.fingerprint;
		for (auto &[key, values] : address_cache.addresses) {
			auto arr = toml::array();
			for (const auto value : values) {
				arr.emplace_back(value - reinterpret_cast<intptr_t>(g_game_module));
			}
			tbl["address_cache"][key] = arr;
		}

		// write a sibling file and swap it in. truncating in place leaves an empty or
		// half written rivet.toml if the process dies mid save (fini runs during
		// teardown), and the next launch would then parse that as a reset.
		const auto &path = settings_path();
		auto temp = path;
		temp += L".tmp";

		{
			std::ofstream file(temp, std::ios::trunc);
			if (!file.is_open()) {
				return;
			}
			file << tbl;
			file.flush();
			if (!file.good()) {
				file.close();
				std::error_code ec;
				std::filesystem::remove(temp, ec);
				return;
			}
		}

		if (!MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
			std::error_code ec;
			std::filesystem::remove(temp, ec);
		}
	}
} // namespace rivet_hook

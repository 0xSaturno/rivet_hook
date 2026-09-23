// SPDX-FileCopyrightText: 2025-2026 Neptuwunium
//
// SPDX-License-Identifier: EUPL-1.2

#include <algorithm>
#include <filesystem>
#include <ranges>
#include <string>
#include <unordered_map>
#include <utility>

#include <MinHook.h>
#include <minizip/mz.h>
#include <minizip/mz_os.h>
#include <minizip/mz_strm.h> // IWYU pragma: keep (needed by mz_zip_rw.h)
#include <minizip/mz_zip.h>	 // IWYU pragma: keep (needed by mz_zip_rw.h)
#include <minizip/mz_zip_rw.h>

#include "game/asset_pipeline.hpp"
#include "runtime.hpp"
#include "runtime_loader.hpp"

#include "mz_zip.h"
#include "settings.hpp"
#include "signature.hpp"

namespace rivet_hook {
	constexpr int64_t RIVET_SENTINEL = 0x7fffffff'ffffff00;

	using namespace game;

	struct MemoryFile {
		const uint8_t *buffer = nullptr;
		size_t size = 0;
		bool is_stage = false;
		AssetLanguage language = AssetLanguage::None;
		std::filesystem::path original_path;

		explicit MemoryFile(std::filesystem::path path): original_path(std::move(path)) { }

		auto
		open(const uint8_t *data_buffer, const size_t data_size) -> bool {
			if (valid()) {
				return true;
			}

			is_stage = data_buffer != nullptr;
			if (is_stage) {
				buffer = data_buffer;
				size = data_size;
			} else {
				const auto file = CreateFileW(original_path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
				if (file == INVALID_HANDLE_VALUE) {
					g_output << "[io] cannot open " << original_path.string() << " got " << GetLastError() << "\n";
					close();
					return false;
				}

				GetFileSizeEx(file, reinterpret_cast<LARGE_INTEGER *>(&size));

				if (size > 0x1fffffff) {
					g_output << "[io] " << original_path.string() << "is too big\n";
					close();
					return false;
				}

				const auto rw_buffer = new uint8_t[size];
				DWORD bytes_read = 0;
				DWORD offset = 0;
				do {
					if (!ReadFile(file, rw_buffer + offset, size - offset, &bytes_read, {})) {
						g_output << "[io] cannot read " << original_path.string() << " got " << GetLastError() << "\n";
						delete[] rw_buffer;
						close();
						return false;
					}

					offset += bytes_read;
				} while (bytes_read > 0 && offset < size);

				if (offset != size) {
					delete[] rw_buffer;
					close();
					return false;
				}

				buffer = rw_buffer;
			}

			return true;
		}

		MemoryFile(const MemoryFile &) = delete;
		MemoryFile &
		operator=(const MemoryFile &) = delete;

		[[nodiscard]] auto
		valid() const -> bool {
			return buffer != nullptr;
		}

		auto
		close() -> void {
			size = 0;

			if (buffer != nullptr) {
				delete[] buffer;
				buffer = nullptr;
			}
		}
	};

	bool runtime_loader_ready = false;
	using mod_file_list_t = std::unordered_map<AssetId, MemoryFile>;
	using mod_list_t = std::array<mod_file_list_t, static_cast<int32_t>(AssetType::Count)>;
	std::array<mod_list_t, static_cast<int32_t>(AssetLanguage::Count)> mod_files_combined = {};

	std::array known_important_assets = {
		0x8e7f2fafc675d9ef,
		0xa5fd9d73bb4f722e,
		0xb575e9facf1a38bc,
		0xabe81779b7c45edf,
		0x82ce4031e142c7f3,
		0x8323511e0074e322,
		0x98aa90ad5ea29cf5,
	};

	std::array<std::string_view, static_cast<int32_t>(AssetLanguage::Count)> rivet_lang_prefix = {{
		"none", "us", "gb", "dk", "nl", "fi", "fr", "de", "it", "jp", "kr", "no", "pl", "pt", "ru", "es",
		"se", "br", "ar", "tr", "la", "cs", "ct", "fc", "cz", "hu", "el", "ro", "th", "vi", "id", "hr",
	}};

	std::array<std::string_view, static_cast<int32_t>(AssetType::Count)> rivet_exts = {
		"",
		".stream",
		".unk2strm",
		".wem",
		".unk4strm",
		".animstrm",
		".unk6strm",
		".lgstream",
	};

	auto text_language = AssetLanguage::None;
	auto audio_language = AssetLanguage::None;

	create_asset_id_t game_create_asset_id = nullptr;
	is_valid_asset_t game_is_valid_asset = nullptr;
	is_valid_asset_t game_is_installed_asset = nullptr;
	open_file_t game_open_file = nullptr;
	read_file_t game_read_file = nullptr;
	close_file_t game_close_file = nullptr;
	resolve_handle_t game_resolve_handle = nullptr;
	set_file_status_t game_set_file_status = nullptr;
	decode_url_t game_decode_url = nullptr;
	mgr_load_asset_t game_mgr_load_asset = nullptr;
	sort_t game_sort = nullptr;
	mount_archive_t game_mount_archive = nullptr;
	commit_assets_t game_commit_assets = nullptr;
	alloc_asset_t game_alloc_asset = nullptr;
	resolve_asset_t game_resolve_asset = nullptr;
	set_language_t game_set_text_language = nullptr;
	set_language_t game_set_audio_language = nullptr;
	window_init_t game_window_init = nullptr;
	is_asset_valid_t game_is_asset_valid = nullptr;

	create_asset_t *game_create_asset = nullptr;
	void *game_create_asset_data = nullptr;
	LoadOperation *game_load_ops = nullptr;
	SortFunc game_sort_op = {};
	bool *legacy_texture_loading = nullptr;
	bool *disable_directstorage = nullptr;

	auto
	create_asset_id(AssetId *asset_id, const char *asset_name) -> AssetId * {
		const auto result = game_create_asset_id(asset_id, asset_name);

		if (asset_name && *asset_name && asset_id) {
			g_output << "[asset id] " << std::hex << *asset_id << " " << asset_name << "\n";
		}

		return result;
	}

	auto
	decode_url(const char *url, const unsigned int urlLen, char *decoded, unsigned int *decodedSize) -> void {
		if (url != nullptr) {
			g_output << "[cohtml] " << url << "\n";
		}

		game_decode_url(url, urlLen, decoded, decodedSize);
	}

	auto
	mgr_load_asset(const intptr_t self, const AssetId asset_id, const AssetId parent_asset_id, const char *asset_name, const intptr_t referencing_asset, const intptr_t unknown6,
				   const int32_t unknown7) -> intptr_t {
		g_output << "[load asset] " << std::hex << asset_id << " ";

		if (asset_name && *asset_name) {
			g_output << asset_name << " from ";
		} else {
			g_output << "(null) from ";
		}

		if (referencing_asset) {
			if (const auto upper_path = reinterpret_cast<const char **>(referencing_asset + 0x10); upper_path && *upper_path && **upper_path) {
				g_output << *upper_path;
			} else {
				g_output << "(null)";
			}
		} else {
			g_output << "(nowhere)";
		}

		g_output << "\n";

		return game_mgr_load_asset(self, asset_id, parent_asset_id, asset_name, referencing_asset, unknown6, unknown7);
	}

	auto
	hook_cohtml() -> void {
		LPVOID proc;
		if (const auto status = MH_CreateHookApiEx(L"cohtml.WindowsDesktop.dll", decode_url_string_name, reinterpret_cast<LPVOID>(decode_url), reinterpret_cast<LPVOID *>(&game_decode_url), &proc);
			status != MH_OK) {
			g_output << "[cohtml] cannot hook cohtml: " << MH_StatusToString(status) << "\n";
			return;
		}

		MH_EnableHook(proc);
	}

	auto
	find_mod_asset(const AssetId asset_id, AssetType type, AssetLanguage lang) -> MemoryFile * {
		if (lang >= AssetLanguage::Count || type >= AssetType::Count) {
			return nullptr;
		}

		auto &mod_list = mod_files_combined[static_cast<int32_t>(lang)][static_cast<int32_t>(type)];
		const auto &mod_index = mod_list.find(asset_id);

		if (mod_index == mod_list.end()) {
			return nullptr;
		}

		return &mod_index->second;
	}

	auto
	find_mod_asset(const AssetId asset_id, const AssetType type) -> MemoryFile * {
		auto *mod_file = find_mod_asset(asset_id, type, text_language);
		if (mod_file) {
			return mod_file;
		}

		mod_file = find_mod_asset(asset_id, type, audio_language);
		if (mod_file) {
			return mod_file;
		}

		mod_file = find_mod_asset(asset_id, type, AssetLanguage::None);
		if (mod_file) {
			return mod_file;
		}

		return nullptr;
	}

	auto
	has_mod_asset(const AssetId asset_id, AssetType type, AssetLanguage lang) -> bool {
		if (lang >= AssetLanguage::Count || type >= AssetType::Count) {
			return false;
		}

		const auto &mod_list = mod_files_combined[static_cast<int32_t>(lang)][static_cast<int32_t>(type)];
		return mod_list.contains(asset_id);
	}

	auto
	has_mod_asset(const AssetId asset_id, const AssetType type) -> bool {
		return has_mod_asset(asset_id, type, text_language) || has_mod_asset(asset_id, type, audio_language) || has_mod_asset(asset_id, type, AssetLanguage::None);
	}

	auto
	rivet_get_asset_lang(const std::string_view stem) -> AssetLanguage {
		const auto it = std::ranges::find(rivet_lang_prefix, stem);
		return static_cast<AssetLanguage>(it != rivet_lang_prefix.end() ? std::distance(rivet_lang_prefix.begin(), it) : 0);
	}

	auto
	rivet_get_asset_type(const std::string_view ext) -> AssetType {
		const auto it = std::ranges::find(rivet_exts, ext);
		return static_cast<AssetType>(it != rivet_exts.end() ? std::distance(rivet_exts.begin(), it) : 0);
	}

	auto
	populate_mod_asset(const std::filesystem::path &path, const std::string &game_path, AssetId asset_id, AssetType type, AssetLanguage lang, const uint8_t *buffer = nullptr, const size_t size = 0)
		-> void {
		if (buffer == nullptr) {
			const auto relative = std::filesystem::relative(path, std::filesystem::current_path()).string();
			g_output << std::hex << "[loader] " << relative << " resolved to " << game_path << " with asset id " << asset_id << ", type " << static_cast<int32_t>(type) << ", language "
					 << static_cast<int32_t>(lang) << "\n";
		} else {
			g_output << std::hex << "[loader] " << game_path << " resolved to asset id " << asset_id << ", type " << static_cast<int32_t>(type) << ", language " << static_cast<int32_t>(lang) << "\n";
		}

		auto &mod_list = mod_files_combined[static_cast<int32_t>(lang)][static_cast<int32_t>(type)];

		if (const auto &mod_index = mod_list.find(asset_id); mod_index != mod_list.end()) {
			g_output << "[loader] " << path << " has a collision with " << mod_index->second.original_path << "; unloading old...\n";

			mod_index->second.close();
			mod_list.erase(asset_id);
		}

		auto [it, inserted] = mod_list.emplace(asset_id, path);
		if (!inserted) {
			g_output << "[loader] " << path << " did not insert?? \n";
			return;
		}

		auto &created = it->second;
		created.language = lang;

		if (!it->second.open(buffer, size) || !created.valid()) {
			g_output << "[loader] " << path << " failed to init\n";
			created.close();
			mod_list.erase(asset_id);
		}
	}

	auto
	load_mod_assets_rivet(const std::filesystem::path &base_dir) -> void {
		// localization/localization_us.localization -> localization/localization_all.localization (built, us)
		// sound/soundbank/us/init.bnk -> sound/soundbank/init.bnk (soundbank, us)
		// sound/wem/us/1.wem -> E000000000000001 (audio, us)
		// built/misc.texture.stream -> built/misc.texture (texture, none)
		// built/misc.texture -> built/misc.texture (built, none)
		for (const auto &entry : std::filesystem::recursive_directory_iterator(base_dir)) {
			if (!entry.is_regular_file()) {
				continue;
			}

			const auto &mod_path = entry.path();
			auto relative_path = std::filesystem::relative(mod_path, base_dir);

			auto language = AssetLanguage::None;
			const AssetType type = rivet_get_asset_type(relative_path.extension().string());

			if (relative_path.extension() == ".localization" && relative_path.filename() != "localization_all.localization") {
				std::string stem = relative_path.stem().string();

				if (const size_t underscore_pos = stem.find_last_of('_'); underscore_pos != std::string::npos) {
					language = rivet_get_asset_lang(std::string_view(stem).substr(underscore_pos + 1));
				}

				if (language == AssetLanguage::None) {
					continue; // malformed.
				}

				relative_path = relative_path.replace_filename("localization_all.localization");
			} else if (relative_path.has_parent_path()) {
				language = rivet_get_asset_lang(relative_path.parent_path().filename().string());
				if (language != AssetLanguage::None) {
					relative_path = relative_path.parent_path().parent_path() / relative_path.filename();
				}
			}

			AssetId asset_id = 0;
			if (type == AssetType::Audio) {
				try {
					asset_id = 0xE0000000'00000000 | std::stoul(relative_path.stem().string());
				} catch (const std::exception &e) {
					g_output << "[assets/rivet] could not parse asset id for path " << relative_path << ": " << e.what() << "\n";
					continue;
				}
			} else {
				if (auto first_dir = *relative_path.begin(); first_dir == "unknown") {
					try {
						asset_id = std::stoull(relative_path.stem().string(), nullptr, 16);
					} catch (const std::exception &e) {
						g_output << "[assets/rivet] could not parse asset id for path " << relative_path << ": " << e.what() << "\n";
						continue;
					}
				} else {
					if (type != AssetType::Built) {
						relative_path = relative_path.replace_extension("");
					}

					game_create_asset_id(&asset_id, relative_path.string().c_str());
				}
			}

			populate_mod_asset(mod_path, relative_path.string(), asset_id, type, language);
		}
	}

	auto
	load_mod_assets_overstrike(const std::filesystem::path &base_dir) -> void {
		// 0/... -> ... (built, none)
		// 1/... -> ... (texture, none)
		// 8/... -> ... (built, us)
		// 9/... -> ... (texture, us)
		for (const auto &entry : std::filesystem::directory_iterator(base_dir)) {
			if (!entry.is_directory()) {
				continue;
			}

			auto &entry_path = entry.path();

			uint32_t directory_id = 0;
			try {
				directory_id = std::stoul(entry_path.filename().string());
			} catch (const std::exception &e) {
				g_output << "[assets/overstrike] could not parse group id for path " << entry_path << ": " << e.what() << "\n";
				continue;
			}

			if (directory_id > 0xff) {
				g_output << "[assets/overstrike] group id for " << entry_path << " is malformed. skipping\n";
				continue;
			}

			const auto language = static_cast<AssetLanguage>(directory_id / 8);
			const auto type = static_cast<AssetType>(directory_id % 8);

			if (language >= AssetLanguage::Count || type >= AssetType::Count) {
				g_output << "[assets/overstrike] group id for " << entry_path.string() << " is malformed. skipping\n";
				return;
			}

			for (const auto &type_entry : std::filesystem::recursive_directory_iterator(entry_path)) {
				if (!type_entry.is_regular_file()) {
					continue;
				}

				const auto &mod_path = type_entry.path();
				auto relative_path = std::filesystem::relative(mod_path, entry_path);

				AssetId asset_id = 0;
				if (relative_path.extension() == "") {
					try {
						asset_id = std::stoull(relative_path.filename().string(), nullptr, 16);
					} catch (const std::exception &e) {
						g_output << "[assets/overstrike] could not parse asset id for path " << relative_path << ": " << e.what() << "\n";
						continue;
					}
				} else {
					game_create_asset_id(&asset_id, relative_path.string().c_str());
				}

				populate_mod_asset(mod_path, relative_path.string(), asset_id, type, language);
			}
		}
	}

	void
	load_mod_asset_overstrike_stage_entry(const std::filesystem::path &path, void *zip, const mz_zip_file *file_info) {
		if (file_info->filename_size == 0) {
			return;
		}

		const std::filesystem::path entry_path(std::string(file_info->filename, file_info->filename_size));

		if (mz_os_is_dir_separator(file_info->filename[file_info->filename_size - 1])) {
			return;
		}

		if (!entry_path.has_parent_path()) {
			return;
		}

		if (file_info->uncompressed_size > 0x1fffffff) {
			g_output << "[assets/stage] file " << path.string() << "@" << entry_path.string() << " is too big\n";
			return;
		}

		if (entry_path.begin() == entry_path.end()) {
			g_output << "[assets/stage] file " << path.string() << "@" << entry_path.string() << " is too shallow\n";
			return;
		}

		const auto first_part = *entry_path.begin();

		uint32_t directory_id = 0;
		try {
			directory_id = std::stoul(first_part.string());
		} catch (const std::exception &e) {
			g_output << "[assets/stage] could not parse group id for path " << path.string() << "@" << entry_path.string() << ": " << e.what() << "\n";
			return;
		}

		if (directory_id > 0xff) {
			g_output << "[assets/stage] group id for " << path.string() << "@" << entry_path.string() << " is malformed. skipping\n";
			return;
		}

		const auto language = static_cast<AssetLanguage>(directory_id / 8);
		const auto type = static_cast<AssetType>(directory_id % 8);

		if (language >= AssetLanguage::Count || type >= AssetType::Count) {
			g_output << "[assets/stage] group id for " << path.string() << "@" << entry_path.string() << " is malformed. skipping\n";
			return;
		}

		AssetId asset_id = 0;
		const auto final_path = entry_path.lexically_relative(*entry_path.begin()).string();
		if (entry_path.extension() == "") {
			try {
				asset_id = std::stoull(entry_path.filename().string(), nullptr, 16);
			} catch (const std::exception &e) {
				g_output << "[assets/stage] could not parse asset id for path " << path.string() << "@" << entry_path.string() << ": " << e.what() << "\n";
				return;
			}
		} else {
			game_create_asset_id(&asset_id, final_path.c_str());
		}

		const auto size = static_cast<int32_t>(file_info->uncompressed_size);
		auto *buffer = new uint8_t[size];
		int32_t bytes_read = 0;
		int32_t offset = 0;
		do {
			bytes_read = mz_zip_reader_entry_read(zip, buffer + offset, size - offset);

			if (bytes_read > 0) {
				offset += bytes_read;
			} else if (bytes_read < 0) {
				g_output << "[assets/stage] error reading entry " << path.string() << "@" << entry_path.string() << ": " << std::dec << bytes_read << "\n";
				break;
			}
		} while (bytes_read > 0 && offset < size);

		if (size == offset) {
			populate_mod_asset(entry_path, final_path, asset_id, type, language, buffer, size);
		} else {
			delete[] buffer;
		}
	}

	// Buffers behind the ui slots. Registered once in init, then only rewritten
	// in place, so the mod asset map is never mutated while loader threads read
	// it. Null until register_ui_slots has run.
	std::array<uint8_t *, AssetLoader::ui_slot_count> ui_slot_buffers = {};

	// Slots live beside the HUD document so a page can fetch them by bare name.
	// The asset id hash is case insensitive - the stage ships lowercase paths
	// while HUD.html asks for "css/MiniMap.css" and that already resolves - so
	// the lowercase spelling here is fine.
	auto
	register_ui_slots() -> void {
		if (game_create_asset_id == nullptr) {
			g_output << "[loader] no create_asset_id, ui slots unavailable\n";
			return;
		}

		for (auto slot = 0; slot < AssetLoader::ui_slot_count; ++slot) {
			char game_path[64];
			_snprintf_s(game_path, sizeof(game_path), _TRUNCATE, "ui/loaded/exported/hud/mm_%d.json", slot);

			AssetId asset_id = 0;
			game_create_asset_id(&asset_id, game_path);

			// start as valid, empty json so a poll before the first publish
			// parses instead of erroring
			auto *buffer = new uint8_t[AssetLoader::ui_slot_size];
			memset(buffer, ' ', AssetLoader::ui_slot_size);
			memcpy(buffer, "{}", 2);

			populate_mod_asset(game_path, game_path, asset_id, AssetType::Built, AssetLanguage::None, buffer, AssetLoader::ui_slot_size);

			// keep the pointer only if it actually landed in the map
			if (const auto *file = find_mod_asset(asset_id, AssetType::Built, AssetLanguage::None); file != nullptr && file->valid()) {
				ui_slot_buffers[slot] = buffer;
			} else {
				g_output << "[loader] ui slot " << std::dec << slot << " failed to register\n";
			}
		}

		g_output << "[loader] registered " << std::dec << AssetLoader::ui_slot_count << " ui slots of " << AssetLoader::ui_slot_size << " bytes\n";
	}

	auto
	load_mod_assets_overstrike_stage(const std::filesystem::path &path) -> void {
		auto zip = mz_zip_reader_create();
		if (!zip) {
			g_output << "[assets/stage] could open stage file << " << path << ": unknown error\n";
			return;
		}

		auto result = mz_zip_reader_open_file(zip, path.string().c_str());
		if (result != MZ_OK) {
			g_output << "[assets/stage] could open stage file " << path << ": " << std::dec << result << "\n";
			mz_zip_reader_delete(&zip);
			return;
		}

		result = mz_zip_reader_goto_first_entry(zip);
		if (result != MZ_OK) {
			g_output << "[assets/stage] error processing stage file " << path << ": " << std::dec << result << "\n";
			mz_zip_reader_delete(&zip);
			return;
		}

		while (result == MZ_OK) {
			mz_zip_file *file_info = nullptr;
			result = mz_zip_reader_entry_get_info(zip, &file_info);

			if (result != MZ_OK || file_info == nullptr) {
				g_output << "[assets/stage] end of " << path << ": " << std::dec << result << "\n";
				continue;
			}

			result = mz_zip_reader_entry_open(zip);
			if (result == MZ_OK) {
				load_mod_asset_overstrike_stage_entry(path, zip, file_info);
				mz_zip_reader_entry_close(zip);
			}

			result = mz_zip_reader_goto_next_entry(zip);
		}

		mz_zip_reader_delete(&zip);
	}

	auto
	load_mod_assets() -> void {
		const auto cwd = std::filesystem::current_path();

		for (const auto &entry : g_settings.assets.paths) {
			auto path = std::filesystem::path(entry);

			if (!path.is_absolute()) {
				path = cwd / path;
			}

			if (std::filesystem::is_regular_file(path) && path.extension() == ".stage") {
				g_output << "[loader] mod path " << entry << " is overstrike stage format.\n";
				load_mod_assets_overstrike_stage(path);
				continue;
			}

			if (!std::filesystem::is_directory(path)) {
				g_output << "[loader] mod path " << entry << " does not exist! skipping.\n";
				continue;
			}

			if (std::filesystem::exists(path / "info.json")) {
				g_output << "[loader] mod path " << entry << " is overstrike format.\n";
				load_mod_assets_overstrike(path);
			} else {
				g_output << "[loader] mod path " << entry << " is rivet format.\n";
				load_mod_assets_rivet(path);
			}
		}
		g_output.flush();
	}

	auto
	has_mod_asset(const AssetId asset_id) -> bool {
		for (auto type = 0; type < static_cast<int32_t>(AssetType::Count); type++) {
			if (has_mod_asset(asset_id, static_cast<AssetType>(type))) {
				return true;
			}
		}

		return false;
	}

	auto
	open_file(const intptr_t self, AssetFile *file, const AssetId asset_id, AssetType type, const int32_t platform, const uint8_t manager_id) -> void {
		if (g_settings.log.loose_io) {
			g_output << "[loose][open ] " << std::hex << asset_id << " type: " << static_cast<int32_t>(type) << " manager: " << static_cast<uint32_t>(manager_id)
					 << " status: " << static_cast<uint32_t>(file->status) << " padding: " << file->padding << " data: " << file->data << " asset_id: " << file->asset_id << "\n";
			g_output.flush();
		}

		if (type < AssetType::Count) {
			if (const auto *mod_file = find_mod_asset(asset_id, type); mod_file != nullptr) {
				if (g_settings.assets.log) {
					g_output << "[loose][open ] " << std::hex << asset_id << " type: " << static_cast<int32_t>(type) << " manager: " << static_cast<uint32_t>(manager_id)
							 << " status: " << static_cast<uint32_t>(file->status) << " padding: " << file->padding << " data: " << file->data << " asset_id: " << file->asset_id << "\n";
					g_output.flush();
				}

				file->status = AssetFileStatus::OpenComplete;
				file->padding = 0;
				file->data = RIVET_SENTINEL | static_cast<uint8_t>(static_cast<int32_t>(type));
				file->asset_id = asset_id;
				file->size = mod_file->size;
				return;
			}
		}

		game_open_file(self, file, asset_id, type, platform, manager_id);
	}

	auto
	resolve_handle(const intptr_t self, const AssetId asset_id, AssetType type, const int32_t platform, const uint8_t manager_id) -> int64_t {
		if (g_settings.log.loose_io) {
			g_output << "[loose][reslv] " << std::hex << asset_id << " type: " << static_cast<int32_t>(type) << " manager: " << static_cast<uint32_t>(manager_id) << "\n";
			g_output.flush();
		}

		if (type < AssetType::Count) {
			if (has_mod_asset(asset_id, type)) {
				if (g_settings.assets.log) {
					g_output << "[loose][reslv] " << std::hex << asset_id << " is modded\n";
					g_output.flush();
				}

				return static_cast<int64_t>(asset_id & INT64_MAX);
			}
		}

		return game_resolve_handle(self, asset_id, type, platform, manager_id);
	}

	auto
	read_file(const intptr_t self, AssetFile *file, char *buffer, const size_t offset, const size_t size, const int32_t priority, const int32_t unknown2) -> bool {
		if (g_settings.log.loose_io) {
			g_output << "[loose][read ] offset: " << std::hex << offset << " size: " << size << " status: " << static_cast<uint32_t>(file->status) << " padding: " << file->padding
					 << " data: " << file->data << " asset_id: " << file->asset_id << "\n";
			g_output.flush();
		}

		if (const auto type = static_cast<AssetType>(file->data & 0xFF); (file->data & RIVET_SENTINEL) == RIVET_SENTINEL && type < AssetType::Count) {
			if (g_settings.assets.log) {
				g_output << "[loose][read ] offset: " << std::hex << offset << " size: " << size << " status: " << static_cast<uint32_t>(file->status) << " padding: " << file->padding
						 << " data: " << file->data << " asset_id: " << file->asset_id << "\n";
				g_output.flush();
			}

			if (const auto *mod_file = find_mod_asset(file->asset_id, type); mod_file != nullptr) {
				if (offset + size > mod_file->size) {
					game_set_file_status(file, AssetFileStatus::OutOfBounds);
					return false;
				}

				std::copy_n(mod_file->buffer + offset, size, buffer);

				game_set_file_status(file, AssetFileStatus::ReadComplete);
				return true;
			}

			game_set_file_status(file, AssetFileStatus::ReadFailed);
			if (g_settings.assets.log) {
				g_output << "[loose][read ] trying to read something that does not exist.\n";
				g_output.flush();
			}

			return false;
		}

		return game_read_file(self, file, buffer, offset, size, priority, unknown2);
	}

	auto
	close_file(const intptr_t self, AssetFile *file) -> void {
		auto has_mod = false;
		if (const auto type = static_cast<AssetType>(file->data & 0xFF); (file->data & RIVET_SENTINEL) == RIVET_SENTINEL && type < AssetType::Count) {
			game_set_file_status(file, AssetFileStatus::Closed);
			file->data = 0;
			has_mod = g_settings.assets.log;
		} else {
			game_close_file(self, file);
		}

		if (g_settings.log.loose_io || has_mod) {
			g_output << "[loose][close] status: " << static_cast<uint32_t>(file->status) << " padding: " << file->padding << " data: " << file->data << " asset_id: " << file->asset_id << "\n";
			g_output.flush();
		}
	}

	auto
	reimpl_load_ops(ArchiveFileSystem *self, const AssetId *assetIds, const LoadMetadata *metadata, const int32_t assetCount) -> int64_t {
		if (assetCount <= 0) {
			return 0;
		}

		int32_t loadIndex = 0;
		for (int32_t i = 0; i < assetCount; ++i) {
			LoadMetadata meta = metadata[i];
			uint64_t assetId = assetIds[i];

			if (g_settings.log.asset_io) {
				g_output << "[built] " << std::hex << assetId << " type: " << static_cast<uint32_t>(meta.type) << "\n";
			}

			const MemoryFile *mod_file = nullptr;
			if (meta.type == 0xE /* soundbank */) {
				mod_file = find_mod_asset(assetId, AssetType::Built, audio_language);
			}

			if (mod_file == nullptr) {
				mod_file = find_mod_asset(assetId, AssetType::Built);
			}

			if (mod_file != nullptr && mod_file->valid()) {
				if (g_settings.assets.log) {
					g_output << "[built] " << std::hex << assetId << " is modded\n";
				}

				if (g_settings.assets.log && g_settings.assets.verbose) {
					g_output << "[built] " << std::hex << assetId << " create header\n";
				}

				AssetHeader *header = game_alloc_asset(0, 1, assetId, &meta, static_cast<uint8_t>(mod_file->language));

				if (g_settings.assets.log && g_settings.assets.verbose) {
					g_output << "[built] " << std::hex << assetId << " header created\n";
				}

				if (header) {
					if (g_settings.assets.log && g_settings.assets.verbose) {
						g_output << "[built] " << std::hex << assetId << " check valid, ptr " << reinterpret_cast<intptr_t>(mod_file->buffer) << "\n";
					}

					const auto magic = *reinterpret_cast<const uint32_t *>(mod_file->buffer);
					if (g_settings.assets.log && g_settings.assets.verbose) {
						g_output << "[built] " << std::hex << assetId << " magic " << magic << "\n";
					}

					if (mod_file->size <= 0x24 || !game_is_asset_valid(magic, meta.type, assetId)) {
						if (g_settings.assets.log && g_settings.assets.verbose) {
							g_output << "[built] " << std::hex << assetId << " not valid\n";
						}

						header->status = 7;
						goto commit;
					}

					if (g_settings.assets.log && g_settings.assets.verbose) {
						g_output << "[built] " << std::hex << assetId << " valid, create\n";
					}

					if ((*game_create_asset)(header, mod_file->buffer, game_create_asset_data)) {
						if (g_settings.assets.log && g_settings.assets.verbose) {
							g_output << "[built] " << std::hex << assetId << " created\n";
						}

						intptr_t offset = 0x24;
						for (int32_t j = 0; j < header->dataRangeCount; ++j) {
							if (static_cast<size_t>(offset + header->dataRanges[j].size) > mod_file->size) {
								if (g_settings.assets.log && g_settings.assets.verbose) {
									g_output << "[built] " << std::hex << assetId << " out of bounds\n";
								}

								header->status = 6;
								break;
							}

							if (g_settings.assets.log && g_settings.assets.verbose) {
								g_output << "[built] " << std::hex << assetId << " copy\n";
							}

							std::copy_n(mod_file->buffer + offset, header->dataRanges[j].size, header->dataRanges[j].buffer);
							offset += header->dataRanges[j].size;
						}

						if (g_settings.assets.log && g_settings.assets.verbose) {
							g_output << "[built] " << std::hex << assetId << " done\n";
						}

						header->status = 0;
						goto commit;
					}

					if (g_settings.assets.log && g_settings.assets.verbose) {
						g_output << "[built] " << std::hex << assetId << " cant create\n";
					}
					header->status = 4;

				commit:
					if (g_settings.assets.log && g_settings.assets.verbose) {
						g_output << "[built] " << std::hex << assetId << " commit header\n";
					}

					game_commit_assets(1);
				}

				continue;
			}

			const FoundAsset *asset = nullptr;
			AssetLanguage selectedLanguage = audio_language;
			if (meta.type == 0xE /* soundbank */) {
				asset = game_resolve_asset(&self->toc, assetId, audio_language, AssetType::Built);
			}

			if (!asset) {
				asset = game_resolve_asset(&self->toc, assetId, text_language, AssetType::Built);
				selectedLanguage = text_language;
			}

			if (!asset) {
				asset = game_resolve_asset(&self->toc, assetId, audio_language, AssetType::Built);
				selectedLanguage = audio_language;
			}

			if (!asset) {
				asset = game_resolve_asset(&self->toc, assetId, AssetLanguage::None, AssetType::Built);
				selectedLanguage = AssetLanguage::None;
			}

			if (!asset || asset->header == -1) {
				// here in case of crash because i haven't seen this yet
				// there's 3 different ways it fails early prior to this so if it happens here something really bad happened

				g_output << "[built] invalid path " << std::hex << assetId << "\n";

				if (game_alloc_asset(0, 1, assetId, &meta, meta.language)) {
					game_commit_assets(1);
				}

				g_output << "[built] skipped " << std::hex << assetId << "\n";

				continue;
			}

			const ArchiveAsset archiveAsset = asset->asset;

			if (!self->mountedTable[archiveAsset.index]) {
				game_mount_archive(self, archiveAsset.index);
			}

			game_load_ops[loadIndex].index = i;
			game_load_ops[loadIndex].asset = archiveAsset;
			game_load_ops[loadIndex].size = asset->size;
			game_load_ops[loadIndex].header = asset->header;
			game_load_ops[loadIndex].language = static_cast<uint8_t>(selectedLanguage);
			game_load_ops[loadIndex].priority |= 1u;

			if (std::ranges::find(known_important_assets, assetId) != known_important_assets.end()) {
				game_load_ops[loadIndex].priority &= ~1u;
			}

			loadIndex += 1;
		}

		game_sort(reinterpret_cast<intptr_t>(game_load_ops), loadIndex, 0x18, game_sort_op);

		g_output.flush();
		return loadIndex;
	}

	auto
	is_valid_asset(ArchiveFileSystem *self, const AssetId asset_id) -> bool {
		return game_is_valid_asset(self, asset_id) || has_mod_asset(asset_id);
	}

	auto
	is_installed_asset(ArchiveFileSystem *self, const AssetId asset_id) -> bool {
		return game_is_installed_asset(self, asset_id) || has_mod_asset(asset_id);
	}

	auto
	set_text_language(const AssetLanguage lang) -> void {
		game_set_text_language(lang);
		text_language = lang;
	}

	auto
	set_audio_language(const AssetLanguage lang) -> void {
		game_set_audio_language(lang);
		audio_language = lang;
	}

	auto
	window_init(const intptr_t self) -> bool {
		const auto result = game_window_init(self);

		// NOTE: This bricks DirectStorage, need to find a workaround for "next gen" texture fencing.
		*legacy_texture_loading = true;
		*disable_directstorage = true;

		return result;
	}

	auto
	AssetLoader::publish_ui_slot(const int slot, const char *text, const size_t length) -> bool {
		if (slot < 0 || slot >= ui_slot_count || text == nullptr) {
			return false;
		}

		auto *buffer = ui_slot_buffers[slot];
		if (buffer == nullptr || length > ui_slot_size) {
			return false;
		}

		// pad first, so a reader that catches this between the two writes sees
		// trailing spaces rather than the tail of the previous, longer document
		memset(buffer + length, ' ', ui_slot_size - length);
		memcpy(buffer, text, length);
		return true;
	}

	auto
	AssetLoader::asset_id(const char *path, uint64_t &out) -> bool {
		if (game_create_asset_id == nullptr || path == nullptr || path[0] == '\0') {
			return false;
		}

		AssetId id = 0;
		game_create_asset_id(&id, path);
		out = id;
		return true;
	}

	auto
	AssetLoader::init() -> void {
		if (runtime_loader_ready) {
			return;
		}
		runtime_loader_ready = true;

		if (const auto create_asset_id_ptrs = find_addresses(CREATE_ASSET_ID_SIGNATURE); !create_asset_id_ptrs.empty()) {
			if (g_settings.log.id) {
				create_hook("asset ids", reinterpret_cast<LPVOID>(create_asset_id_ptrs[0]), reinterpret_cast<LPVOID>(&create_asset_id), reinterpret_cast<LPVOID *>(&game_create_asset_id));
			} else {
				game_create_asset_id = reinterpret_cast<create_asset_id_t>(create_asset_id_ptrs[0]);
			}
		}

		if (g_settings.log.paths) {
			create_hook(LOAD_ASSET_SIGNATURE, reinterpret_cast<LPVOID>(&mgr_load_asset), reinterpret_cast<LPVOID *>(&game_mgr_load_asset));
		}

		if (g_settings.log.cohtml) {
			hook_cohtml();
		}

		if (!g_settings.assets.enabled) {
			return;
		}

		load_mod_assets();
		register_ui_slots();

		#define LOAD_FUNC_ADDRESS_RAW(var, name, sig) \
			if (var = find_address(sig); !var) { \
				g_output << "[loader] cannot initialize, " name " address is not found\n"; \
				return; \
			}

		#define LOAD_FUNC_ADDRESS(var, type, sig) \
			if (var = reinterpret_cast<type>(find_address(sig)); !var) { \
				g_output << "[loader] cannot initialize, " << sig.name << " address is not found\n"; \
				return; \
			}

		#define LOAD_VAR_ADDRESS(var, type, sig, addr) \
			if (var = reinterpret_cast<type>(load_rel_var(find_address(sig), addr)); !var) { \
				g_output << "[loader] cannot initialize, " << sig.name << " address is not found\n"; \
				return; \
			}

		const auto *archivefs_vtable = static_cast<intptr_t *>(load_rel_var(find_address(ARCHIVEFS_VTABLE_SIGNATURE), ARCHIVEFS_VTABLE_ADDRESS));
		if (!archivefs_vtable) {
			g_output << "[loader] cannot initialize, archivefs address is not found\n";
			return;
		}

		// functions we need to call for reimpl_load_ops
		LOAD_FUNC_ADDRESS(game_resolve_asset, resolve_asset_t, RESOLVE_ASSET_SIGNATURE);
		LOAD_FUNC_ADDRESS(game_set_file_status, set_file_status_t, SET_FILE_STATUS_SIGNATURE);
		LOAD_FUNC_ADDRESS(game_alloc_asset, alloc_asset_t, ALLOC_ASSET_RCRA_SIGNATURE);
		LOAD_FUNC_ADDRESS(game_commit_assets, commit_assets_t, COMMIT_ASSET_RCRA_SIGNATURE);
		LOAD_FUNC_ADDRESS(game_is_asset_valid, is_asset_valid_t, IS_ASSET_HEADER_VALID_RCRA_SIGNATURE);
		LOAD_FUNC_ADDRESS(game_sort, sort_t, SORT_SIGNATURE);

		LOAD_FUNC_ADDRESS_RAW(game_sort_op.func, "sort op", SORT_FUNC_RCRA_SIGNATURE);
		game_sort_op.target = 0;

		game_mount_archive = reinterpret_cast<mount_archive_t>(archivefs_vtable[ARCHIVEFS_VTABLE_MOUNT]);
		if (!game_mount_archive) {
			g_output << "[loader] cannot initialize, mount archive address is not found\n";
		}

		// vars we need to read/write to for reimpl_load_ops
		LOAD_VAR_ADDRESS(game_load_ops, LoadOperation *, LOAD_OPS_SIGNATURE, LOAD_OPS_ADDRESS);

		// vars we need to call/read to for asset header creation
		LOAD_VAR_ADDRESS(game_create_asset, create_asset_t *, CREATE_ASSET_RCRA_SIGNATURE, CREATE_ASSET_RCRA_ADDRESS);
		LOAD_VAR_ADDRESS(game_create_asset_data, void *, CREATE_ASSET_DATA_RCRA_SIGNATURE, CREATE_ASSET_DATA_RCRA_ADDRESS);

		// vars we need to overwrite to disable texture fencing
		LOAD_VAR_ADDRESS(disable_directstorage, bool *, DISABLE_DIRECTSTORAGE_RCRA_SIGNATURE, DISABLE_DIRECTSTORAGE_RCRA_ADDRESS);
		LOAD_VAR_ADDRESS(legacy_texture_loading, bool *, LEGACY_TEXTURE_SIGNATURE, LEGACY_TEXTURE_ADDRESS);

		// language tracking
		LPVOID set_text_lang, set_audio_lang;
		LOAD_VAR_ADDRESS(set_text_lang, LPVOID, REL_SET_TEXT_AUDIO_LANGUAGE_SIGNATURE, REL_SET_TEXT_LANGUAGE_ADDRESS);
		LOAD_VAR_ADDRESS(set_audio_lang, LPVOID, REL_SET_TEXT_AUDIO_LANGUAGE_SIGNATURE, REL_SET_AUDIO_LANGUAGE_ADDRESS);
		create_hook("SET_TEXT_LANGUAGE", set_text_lang, reinterpret_cast<LPVOID>(&set_text_language), reinterpret_cast<LPVOID *>(&game_set_text_language));
		create_hook("SET_AUDIO_LANGUAGE", set_audio_lang, reinterpret_cast<LPVOID>(&set_audio_language), reinterpret_cast<LPVOID *>(&game_set_audio_language));

		// asset io
		create_hook(PRELOAD_LOAD_OP_RCRA_SIGNATURE, reinterpret_cast<LPVOID>(&reimpl_load_ops), nullptr);
		create_hook(IS_ASSET_VALID_RCRA_SIGNATURE, reinterpret_cast<LPVOID>(&is_valid_asset), reinterpret_cast<LPVOID *>(&game_is_valid_asset));
		create_hook(IS_INSTALLED_ASSET_SIGNATURE, reinterpret_cast<LPVOID>(&is_installed_asset), reinterpret_cast<LPVOID *>(&game_is_installed_asset));

		// loose io
		create_hook("ARCHIVEFS_RESOLVE_HANDLE", reinterpret_cast<LPVOID>(archivefs_vtable[ARCHIVEFS_VTABLE_RESOLVEHANDLE]), reinterpret_cast<LPVOID>(&resolve_handle), reinterpret_cast<LPVOID *>(&game_resolve_handle));
		create_hook("ARCHIVEFS_OPEN_FILE", reinterpret_cast<LPVOID>(archivefs_vtable[ARCHIVEFS_VTABLE_OPENFILE]), reinterpret_cast<LPVOID>(&open_file), reinterpret_cast<LPVOID *>(&game_open_file));
		create_hook("ARCHIVEFS_READ_FILE", reinterpret_cast<LPVOID>(archivefs_vtable[ARCHIVEFS_VTABLE_READFILE]), reinterpret_cast<LPVOID>(&read_file), reinterpret_cast<LPVOID *>(&game_read_file));
		create_hook("ARCHIVEFS_CLOSE_FILE", reinterpret_cast<LPVOID>(archivefs_vtable[ARCHIVEFS_VTABLE_CLOSEFILE]), reinterpret_cast<LPVOID>(&close_file), reinterpret_cast<LPVOID *>(&game_close_file));

		// disable fencing
		// NOTE: This bricks DirectStorage, need to find a workaround for "next gen" texture fencing.
		if (g_settings.assets.disable_dstorage) {
			// needed to reset fencing a second time once the game starts.
			create_hook(WINDOW_INIT_RCRA_SIGNATURE, reinterpret_cast<LPVOID>(&window_init), reinterpret_cast<LPVOID *>(&game_window_init));

			*legacy_texture_loading = true;
			*disable_directstorage = true;
		}
	}

	auto
	AssetLoader::fini() -> void {
		if (!runtime_loader_ready) {
			return;
		}
		runtime_loader_ready = false;

		for (auto &type_mod_list : mod_files_combined) {
			for (auto &mod_list : type_mod_list) {
				for (auto &value : mod_list | std::views::values) {
					g_output << "[loader] closing " << value.original_path.string() << "\n";
					value.close();
				}

				mod_list.clear();
			}
		}
	}
} // namespace rivet_hook

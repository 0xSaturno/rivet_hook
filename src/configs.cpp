// SPDX-FileCopyrightText: 2025-2026 Neptuwunium
//
// SPDX-License-Identifier: EUPL-1.2

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cctype>
#include <cstdlib>
#include <cstring>

#include "configs.hpp"

#include "runtime.hpp"
#include "runtime_loader.hpp"
#include "signature.hpp"

using namespace rivet_hook::game;

namespace rivet_hook::configs {
	// config asset manager layout, measured on the shipping exe
	constexpr uint32_t ENTRIES_OFFSET = 0x40; // entry array
	constexpr uint32_t STRIDE_OFFSET = 0x48;  // uint64, bytes per entry
	constexpr uint32_t MAX_OFFSET = 0x68;	  // int32, entries allocated
	constexpr uint32_t MANAGER_READ_SIZE = 0x70;

	// one entry per config asset slot
	constexpr uint32_t ENTRY_STATE_OFFSET = 0x0; // uint8
	constexpr uint32_t ENTRY_ID_OFFSET = 0x8;	 // asset id
	constexpr uint32_t ENTRY_CONFIG_OFFSET = 0x30;
	constexpr uint8_t STATE_LOADED = 4;
	constexpr uint64_t MIN_STRIDE = ENTRY_CONFIG_OFFSET + sizeof(void *);

	// a config is its DDL instance: a vtable, then the fields at the offsets its
	// type describes. slot 1 of the vtable answers the type's hash
	constexpr int TYPE_HASH_SLOT = 1;

	constexpr int32_t MAX_ENTRIES = 1 << 16;
	constexpr int MAX_PARENT_DEPTH = 32;

	static uint8_t *g_manager = nullptr;

	auto
	init() -> void {
		if (g_manager != nullptr) {
			return;
		}

		// HasBundle loads the manager before its first lookup, see signature.hpp
		g_manager = static_cast<uint8_t *>(load_rel_var(find_address(VANITY_HAS_BUNDLE_SIGNATURE), VANITY_HAS_BUNDLE_CONFIGS_ADDRESS));
		if (g_manager == nullptr) {
			g_output << "[configs] the config asset manager was not found, rivet.configs is unavailable\n";
		} else {
			g_output << "[configs] config asset manager at " << static_cast<void *>(g_manager) << "\n";
		}

		g_output.flush();
	}

	template<typename T>
	static auto
	at(const uint8_t *base, const uint32_t offset) -> T {
		return *reinterpret_cast<const T *>(base + offset);
	}

	// the entry array, its stride and length, or false when the manager does not
	// look like one
	static auto
	entries(uint8_t *&array, uint64_t &stride, int32_t &count) -> bool {
		if (g_manager == nullptr || !ddl::is_readable(g_manager, MANAGER_READ_SIZE)) {
			return false;
		}

		array = at<uint8_t *>(g_manager, ENTRIES_OFFSET);
		stride = at<uint64_t>(g_manager, STRIDE_OFFSET);
		count = at<int32_t>(g_manager, MAX_OFFSET);
		if (array == nullptr || stride < MIN_STRIDE || stride > 0x1000 || count <= 0 || count > MAX_ENTRIES) {
			return false;
		}

		return ddl::is_readable(array, stride * static_cast<uint64_t>(count));
	}

	auto
	unavailable_reason() -> const char * {
		if (g_manager == nullptr) {
			return "the config asset manager was not found";
		}

		uint8_t *array = nullptr;
		uint64_t stride = 0;
		int32_t count = 0;
		return entries(array, stride, count) ? "" : "the config asset manager is not readable";
	}

	// the type hash getter on its own, so a fault is caught with nothing to unwind
	static auto
	call_type_hash(void *config, uint32_t *out) -> bool {
		using type_hash_t = uint32_t (*)(void *);
#ifdef _MSC_VER
		__try {
#endif
			const auto *vtable = *static_cast<type_hash_t **>(config);
			*out = vtable[TYPE_HASH_SLOT](config);
			return true;
#ifdef _MSC_VER
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			return false;
		}
#endif
	}

	// the config and its type at one entry, or false when the slot is empty
	static auto
	resolve(uint8_t *entry, Config &out) -> bool {
		if (at<uint8_t>(entry, ENTRY_STATE_OFFSET) != STATE_LOADED) {
			return false;
		}

		auto *config = at<uint8_t *>(entry, ENTRY_CONFIG_OFFSET);
		if (config == nullptr || !ddl::is_readable(config, sizeof(void *)) || !ddl::is_readable(*reinterpret_cast<void **>(config), sizeof(void *) * (TYPE_HASH_SLOT + 1))) {
			return false;
		}

		uint32_t hash = 0;
		if (!call_type_hash(config, &hash)) {
			return false;
		}

		const auto *type = ddl::find_type(hash);
		if (type == nullptr || !ddl::is_readable(config, type->allocation_size)) {
			return false;
		}

		out.id = at<uint64_t>(entry, ENTRY_ID_OFFSET);
		out.type = type;
		out.object = config;
		return true;
	}

	auto
	find(const uint64_t id, Config &out) -> bool {
		uint8_t *array = nullptr;
		uint64_t stride = 0;
		int32_t count = 0;
		if (!entries(array, stride, count)) {
			return false;
		}

		for (int32_t index = 0; index < count; ++index) {
			auto *entry = array + stride * static_cast<uint64_t>(index);
			if (at<uint64_t>(entry, ENTRY_ID_OFFSET) == id && resolve(entry, out)) {
				return true;
			}
		}

		return false;
	}

	auto
	parse_id(const char *text, uint64_t &out) -> bool {
		if (text == nullptr || text[0] == '\0') {
			return false;
		}

		const auto *digits = (text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) ? text + 2 : text;
		if (strlen(digits) == 16) {
			auto hex = true;
			for (const auto *c = digits; *c != '\0'; ++c) {
				hex = hex && std::isxdigit(static_cast<unsigned char>(*c)) != 0;
			}

			if (hex) {
				out = _strtoui64(digits, nullptr, 16);
				return true;
			}
		}

		return AssetLoader::asset_id(text, out);
	}

	static auto
	type_name(const DDLTypeInfo *type, char *out, const size_t size) -> bool {
		return type != nullptr && ddl::read_string(type->name, out, size);
	}

	// the config's class or one it derives from is named exactly name
	static auto
	is_type(const DDLTypeInfo *type, const char *name) -> bool {
		for (auto depth = 0; type != nullptr && depth < MAX_PARENT_DEPTH; ++depth) {
			char text[0x100];
			if (type_name(type, text, sizeof(text)) && strcmp(text, name) == 0) {
				return true;
			}

			type = type->parent_id != 0 ? ddl::find_type(type->parent_id) : nullptr;
		}

		return false;
	}

	auto
	list(const char *type, const size_t limit) -> nlohmann::json {
		nlohmann::json::array_t out;

		uint8_t *array = nullptr;
		uint64_t stride = 0;
		int32_t count = 0;
		if (!entries(array, stride, count)) {
			return out;
		}

		const auto any = type == nullptr || type[0] == '\0';
		for (int32_t index = 0; index < count && out.size() < limit; ++index) {
			Config config;
			if (!resolve(array + stride * static_cast<uint64_t>(index), config)) {
				continue;
			}

			char name[0x100];
			if (!type_name(config.type, name, sizeof(name))) {
				continue;
			}

			if (!any && strstr(name, type) == nullptr && !is_type(config.type, type)) {
				continue;
			}

			char id[24];
			_snprintf_s(id, sizeof(id), _TRUNCATE, "%016llx", config.id);

			nlohmann::json entry;
			entry["id"] = id;
			entry["type"] = name;
			out.emplace_back(std::move(entry));
		}

		return out;
	}

	auto
	values(const Config &config) -> nlohmann::json {
		return ddl::values_of(config.type, config.object);
	}

	auto
	set(const Config &config, const char *path, const ddl::Value &value, ddl::Value &previous, const char **reason) -> bool {
		const auto *type = config.type;
		const uint8_t *owner = config.object;
		const auto index = ddl::resolve_path(type, owner, path);
		if (index < 0) {
			if (reason != nullptr) {
				*reason = "the config has no field by that path";
			}

			return false;
		}

		previous = ddl::read_field(type, owner, index, 0);
		return ddl::write_field(type, const_cast<uint8_t *>(owner), index, 0, value, reason);
	}
} // namespace rivet_hook::configs

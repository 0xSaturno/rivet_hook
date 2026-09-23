// SPDX-FileCopyrightText: 2025-2026 Neptuwunium
//
// SPDX-License-Identifier: EUPL-1.2

#pragma once

#include <cstddef>
#include <cstdint>

#include <nlohmann/json.hpp>

#include "ddl_visit.hpp"

// the game's loaded config assets: authored tuning data (hero traversal, weapons,
// bots, vanity, sound…), each one a DDL instance the visitor can read and write
namespace rivet_hook::configs {
	// resolves the config asset manager. scans only.
	auto
	init() -> void;

	// why configs are unavailable, "" when they are not
	auto
	unavailable_reason() -> const char *;

	// a loaded config by asset id, with its DDL type. null when none is loaded.
	struct Config {
		uint64_t id = 0;
		const game::DDLTypeInfo *type = nullptr;
		uint8_t *object = nullptr;
	};

	auto
	find(uint64_t id, Config &out) -> bool;

	// an asset id from a config path or 16 hex digits
	auto
	parse_id(const char *text, uint64_t &out) -> bool;

	// loaded configs whose class, or any class it derives from, is named type
	// exactly, or whose own class name contains type. an empty type lists all.
	auto
	list(const char *type, size_t limit) -> nlohmann::json;

	// every field of a config, nested structs as objects
	auto
	values(const Config &config) -> nlohmann::json;

	// writes one numeric or boolean field by dotted path. the edit is in place:
	// whatever reads the config from now on sees it, but a component that copied
	// the value when it started keeps its copy. previous receives the old value.
	auto
	set(const Config &config, const char *path, const ddl::Value &value, ddl::Value &previous, const char **reason) -> bool;
} // namespace rivet_hook::configs

// SPDX-FileCopyrightText: 2025-2026 Neptuwunium
//
// SPDX-License-Identifier: EUPL-1.2

#pragma once

#include <cstdint>
#include <string>

#include <nlohmann/json.hpp>

// hardware write watchpoints: which instruction writes an address. one watch
// at a time, in debug register 0 of every thread alive when it is armed.
namespace rivet_hook::watch {
	// length is 1, 2, 4 or 8 and the address must be aligned to it. on failure
	// error receives the reason. execute breaks when the instruction at address
	// runs instead, and keeps the registers of the most recent hits.
	auto
	arm(uintptr_t address, uint32_t length, bool execute, std::string &error) -> bool;

	auto
	disarm() -> void;

	// the writers seen since arming, by return address, with hit counts
	auto
	results() -> nlohmann::json;
} // namespace rivet_hook::watch

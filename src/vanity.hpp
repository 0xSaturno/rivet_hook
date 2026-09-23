// SPDX-FileCopyrightText: 2025-2026 Neptuwunium
//
// SPDX-License-Identifier: EUPL-1.2

#pragma once

#include <cstdint>

// the hero's outfits, through the game's own vanity inventory: the same calls the
// pause menu and level scripts make, so parts load and stay equipped
namespace rivet_hook::vanity {
	// resolves the vanity calls. scans only.
	auto
	init() -> void;

	// a bundle id from an asset path ("configs/vanity/…/bundle.config") or from
	// 16 hex digits, with or without 0x. false when it is neither.
	auto
	bundle_id(const char *text, uint64_t &out) -> bool;

	// whether the actor's vanity inventory owns the bundle. game thread only.
	auto
	has_bundle(uint32_t actor, uint64_t bundle, bool &owned, const char **reason) -> bool;

	// equips an owned bundle on the actor, as the game does. equipped is whether
	// any of its items was not already worn. game thread only.
	auto
	equip_bundle(uint32_t actor, uint64_t bundle, bool &equipped, const char **reason) -> bool;
} // namespace rivet_hook::vanity

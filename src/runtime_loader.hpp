// SPDX-FileCopyrightText: 2025-2026 Neptuwunium
//
// SPDX-License-Identifier: EUPL-1.2

#pragma once

#include <cstddef>
#include <cstdint>

namespace rivet_hook {
	struct AssetLoader {
		static auto
		init() -> void;
		static auto
		fini() -> void;

		// Publishes utf-8 text into one of a fixed set of ui assets so a cohtml
		// document can poll it with fetch and see live data.
		//
		// The slots are registered once during init and afterwards only ever
		// have their bytes rewritten, so publishing never touches the mod asset
		// map and needs no lock against the loader threads reading it.
		//
		// Text shorter than the slot is padded with spaces instead of shortening
		// the buffer: a constant length means a reader can never see a short
		// read, and JSON.parse ignores trailing whitespace. A reader can still
		// catch an update midway and get a torn document, which fails to parse
		// and should simply be skipped.
		static auto
		publish_ui_slot(int slot, const char *text, size_t length) -> bool;

		// the asset id the game derives from an asset path, through its own
		// function. false when that function was not found.
		static auto
		asset_id(const char *path, uint64_t &out) -> bool;

		constexpr static int ui_slot_count = 8;
		constexpr static size_t ui_slot_size = 1024;
	};
} // namespace rivet_hook

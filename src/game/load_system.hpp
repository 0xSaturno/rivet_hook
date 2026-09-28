// SPDX-FileCopyrightText: 2025-2026 Neptuwunium
//
// SPDX-License-Identifier: EUPL-1.2

#pragma once

#include <cstddef>
#include <cstdint>

// the level's checkpoints, as the load system keeps them. every planet is a
// region of one level, and travelling between them is warping to a checkpoint.
// offsets measured on the shipping exe and read back live.
namespace rivet_hook::game {
#pragma pack(push, 1)

	struct CheckpointData {
		float position[3];
		float unknown0c;
		uint64_t unknown10;
		uint64_t actorUid; // the spawn point actor the hero is placed on
		const char *name;
		uint32_t nameHash; // the engine string hash of name
		uint64_t actorAsset; // packed, the spawn point's actor asset
		int32_t region;	  // the region to load, a planet's or one inside it
		uint32_t type;
		float unknown3c;
		uint64_t unknown40;
	};

	static_assert(sizeof(CheckpointData) == 0x48, "CheckpointData size is not 0x48");
	static_assert(offsetof(CheckpointData, name) == 0x20, "CheckpointData name offset is not 0x20");
	static_assert(offsetof(CheckpointData, region) == 0x34, "CheckpointData region offset is not 0x34");

	// followed by a name hash -> index table the hook does not use
	struct CheckpointManager {
		CheckpointData *checkpoints;
		int32_t count;
	};

#pragma pack(pop)
} // namespace rivet_hook::game

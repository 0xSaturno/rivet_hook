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

	// LevelRegion::type
	namespace RegionType {
		constexpr int16_t Global = 0;	 // always loaded
		constexpr int16_t Container = 2; // an instanced area, its units below it
		constexpr int16_t Unit = 3;
		constexpr int16_t OpenWorld = 4; // an open world area, its tiles below it
		constexpr int16_t Tile = 5;		 // streams in and out by distance
		constexpr int16_t Overlay = 6;	 // loads on top of whatever is loaded
	} // namespace RegionType

	// a region loads a list of zones. regions form a tree through parent
	struct LevelRegion {
		uint64_t asset;
		int16_t type; // RegionType
		int16_t nameIndex;
		int16_t lodIndex; // tiles only
		int16_t parent;	  // -1 at the top
		int16_t childStart;
		int16_t childCount;
		int16_t zoneRefStart; // into LevelAsset::zoneRefs
		int16_t zoneRefCount;
		int16_t secondaryRefStart;
		int16_t secondaryRefCount;
		int16_t linkStart;
		int16_t linkCount;
		int16_t gameDataStart;
		int16_t gameDataCount;
	};

	static_assert(sizeof(LevelRegion) == 0x24, "LevelRegion size is not 0x24");

	// where a tile is and how far away it streams
	struct LevelRegionLod {
		float position[3];
		uint32_t tileRadius;
		uint32_t loadDistance;
		uint32_t unloadDistance;
	};

	static_assert(sizeof(LevelRegionLod) == 0x18, "LevelRegionLod size is not 0x18");

	struct LevelZone {
		uint64_t asset;
		int16_t nameIndex;
		uint16_t padding;
	};

	static_assert(sizeof(LevelZone) == 0xc, "LevelZone size is not 0xc");

	// the loaded .level. names are offsets into the level's data file, whose
	// address is packed into the top 48 bits of dataFile
	struct LevelAsset {
		uint8_t unknown0[0x18];
		uint64_t dataFile;
		uint8_t unknown20[0x10];
		int32_t defaultLink;
		int32_t regionCount;
		int32_t zoneRefCount;
		int32_t lodCount;
		int32_t zoneCount;
		int32_t linkCount;
		int32_t gameDataCount;
		int32_t hibernateModelCount;
		int32_t hibernateEffectCount;
		int32_t padding;
		const void *built;
		const LevelRegion *regions;
		const LevelRegionLod *lods;
		const int16_t *zoneRefs;
		const LevelZone *zones;
		const void *links;
		const void *gameData;
		int16_t *invalidSecondaryRefs;
		const uint32_t *regionNames;
		const uint32_t *zoneNames;
		const uint32_t *linkNames;
	};

	static_assert(offsetof(LevelAsset, regionCount) == 0x34, "LevelAsset regionCount offset is not 0x34");
	static_assert(offsetof(LevelAsset, regions) == 0x60, "LevelAsset regions offset is not 0x60");
	static_assert(offsetof(LevelAsset, zones) == 0x78, "LevelAsset zones offset is not 0x78");
	static_assert(offsetof(LevelAsset, zoneNames) == 0xa0, "LevelAsset zoneNames offset is not 0xa0");

	struct LoadSystem {
		uint8_t unknown[0x18];
		const LevelAsset *level;
	};

	namespace OverlayFlag {
		constexpr uint8_t Loaded = 1u << 2;
	} // namespace OverlayFlag

	// the load system's view of one overlay region
	struct OverlayEntry {
		uint64_t asset; // the region's
		uint8_t unknown08[0x2c];
		int16_t region;
		uint8_t flags; // OverlayFlag
		uint8_t unknown37;
	};

	static_assert(sizeof(OverlayEntry) == 0x38, "OverlayEntry size is not 0x38");
	static_assert(offsetof(OverlayEntry, flags) == 0x36, "OverlayEntry flags offset is not 0x36");

	// inside the load system, at the offset RequestOverlayLoad adds
	struct OverlayManager {
		uint8_t unknown[0x10];
		const OverlayEntry *entries;
		int32_t count;
	};

	// one overlay the story drives: the game loads it while its mission state
	// calls for it and unloads it otherwise, whoever asked
	struct CustomOverlay {
		uint8_t unknown[0x10];
		uint64_t asset; // the region's
		uint8_t unknown18[0x10];
	};

	static_assert(sizeof(CustomOverlay) == 0x28, "CustomOverlay size is not 0x28");

	struct CustomOverlaySystem {
		uint8_t unknown[0x18];
		const CustomOverlay *overlays;
		int32_t count;
	};

#pragma pack(pop)
} // namespace rivet_hook::game

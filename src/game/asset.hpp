// SPDX-FileCopyrightText: 2025-2026 Neptuwunium
//
// SPDX-License-Identifier: EUPL-1.2

#pragma once
#include <d3d12.h>

#include "src/assert_helper.hpp"
#include "src/runtime.hpp"

#include <cstdint>

namespace rivet_hook::game {
	using AssetId = uint64_t;

#pragma pack(push, 1)

	enum class AssetType : uint32_t {
		Built = 0,
		TextureStream = 1,
		Unknown2 = 2,
		Audio = 3,
		Unknown4 = 4,
		Animation = 5,
		Unknown6 = 6,
		ZoneGrid = 7,
		Count = 8,
	};

	enum class AssetManagerType : uint8_t {
		Level = 0,
		Zone = 1,
		Actor = 2,
		Conduit = 3,
		Config = 4,
		Cinematic2 = 5,
		Model = 6,
		AnimationClip = 7,
		AnimationSet = 8,
		Material = 9,
		MaterialGraph = 10,
		Texture = 11,
		Atmosphere = 12,
		Effect = 13,
		Soundbank = 14,
		Localization = 15,
		Unknown16 = 16,
		Unknown17 = 17,
		ZoneLighting = 18,
		LevelLighting = 19,
		NodeGraph = 20,
		Unknown21 = 21,
		WwiseLookup = 22,
		Unknown23 = 23,
		Unknown24 = 24,
		None = 0xFF,
	};

	enum class AssetLanguage : uint8_t {
		None = 0,
		English = 1,
		BritishEnglish = 2,
		Danish = 3,
		Dutch = 4,
		Finnish = 5,
		French = 6,
		German = 7,
		Italian = 8,
		Japanese = 9,
		Korean = 10,
		Norwegian = 11,
		Polish = 12,
		Portuguese = 13,
		Russian = 14,
		Spanish = 15,
		Swedish = 16,
		BrazilianPortuguese = 17,
		Arabic = 18,
		Turkish = 19,
		LatinAmericanSpanish = 20,
		SimplifiedChinese = 21,
		TraditionalChinese = 22,
		CanadianFrench = 23,
		Czech = 24,
		Hungarian = 25,
		Greek = 26,
		Romanian = 27,
		Thai = 28,
		Vietnamese = 29,
		Indonesian = 30,
		Croatian = 31,
		Count = 0x20,
	};

	enum class AssetStatus : uint8_t {
		Invalid,
		InQueue,
		Staging,
		Loading,
		Loaded,
		Error,
		Aborted
	};

	struct Asset {
		AssetStatus status;
		uint8_t generation; // bumped on every (re)load
		uint8_t rawType; // AssetManagerType with bit 7 set as a stomp check, read through GetType
		uint8_t flags;
		uint16_t refCount;
		AssetLanguage language;
		uint8_t unknown3;
		AssetId assetId;
		const char *name;
		uint16_t nameOffset;
		uint16_t unknown4[3];
		const char *note; // not a readable pointer in the shipping build
		AssetId loadedFrom;

		__forceinline auto
		GetType() const -> AssetManagerType {
			return static_cast<AssetManagerType>(rawType & 0x7F);
		}

		__forceinline auto
		GetShortName() const -> const char * {
			if (name && *name) {
				if (nameOffset > 1) {
					return name + nameOffset;
				}

				return name;
			}

			return nullptr;
		}

		__forceinline auto
		GetName() const -> const char * {
			if (name && *name) {
				return name;
			}

			return nullptr;
		}
	};

	size_assert(Asset, 0x30);
	offset_assert(Asset, refCount, 0x4);
	offset_assert(Asset, assetId, 0x8);
	offset_assert(Asset, name, 0x10);
	offset_assert(Asset, nameOffset, 0x18);

	struct TextureAsset {
		Asset base;
		uint8_t unknown1[0xC];
		uint32_t maxLOD;
		ID3D12Resource *resource;
		uint8_t unknown2[0x7d];
		uint8_t resourceSize;
	};

	offset_assert(TextureAsset, base.assetId, 0x8);
	offset_assert(TextureAsset, maxLOD, 0x3c);
	offset_assert(TextureAsset, resource, 0x40);
	offset_assert(TextureAsset, resourceSize, 0xc5);

	struct AssetManager {
		void* vtable;
	};

#pragma pack(pop)
} // namespace rivet_hook::game

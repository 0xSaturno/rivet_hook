// SPDX-FileCopyrightText: 2025-2026 Neptuwunium
//
// SPDX-License-Identifier: EUPL-1.2

#pragma once

#include <cstdint>

#include "signature_types.hpp"

namespace rivet_hook {
	constexpr uint32_t REL_ADDRESS_SIZE = 0x4;

	// ddl
	MAKE_SIGNATURE(DDL_HASH_MAP, "0f 57 c0 48 8d ?? ?? ?? ?? ?? 0f 11 05 ?? ?? ?? ?? 0f 11 05 ?? ?? ?? ?? 0f 11 05")
	MAKE_SIGNATURE(DDL_TYPE_LIST, "48 8d ?? ?? ?? ?? ?? 66 89 41 14 8b ?? ?? ?? ?? ?? 48 89 ?? ?? ff c0 89 ?? ?? ?? ?? ?? c3")
	MAKE_SIGNATURE(VERSION, "48 0F BE C1 48 8D 0D ?? ?? ?? ?? 48 8B 04 C1 C3 8B")
	MAKE_SIGNATURE(VERSION_HASH, "48 0F BE C1 48 8D 0D ?? ?? ?? ?? 8B 04 81 C3")
	MAKE_SIGNATURE(COMPONENT_REGISTER, "48 89 81 ?? ?? ?? ?? FF 05")
	MAKE_SIGNATURE(ENGINE_INIT, "48 83 EC 28 E8 ?? ?? ?? ?? 84 C0 75 ?? 48 83 C4 28")

	constexpr uint32_t DDL_HASH_MAP_ADDRESS = 0x6;
	constexpr uint32_t DDL_TYPE_LIST_ADDRESS = 0x3;
	constexpr uint32_t DDL_TYPE_LIST_COUNT_ADDRESS = 0xD;
	constexpr uint32_t COMPONENT_COUNT_ADDRESS = 0x9;
	constexpr uint32_t COMPONENT_REGISTRY_ADDRESS = 0x10;

	// logging
	MAKE_SIGNATURE(CONTEXT_LOG, "65 48 8b 04 25 58 00 00 00 48 85 c9 44 8b 05")
	MAKE_SIGNATURE(LOG, "48 89 54 24 10 33 c0 4c 89 44 24 18 4c 89 4c 24 20")
	MAKE_SIGNATURE(LOAD_ASSET, "48 89 54 24 ?? 53 56 57 41 55 41 56 48 83 ?? ?? 48") // note: only hooked for path logging

	// util
	MAKE_SIGNATURE(REL_NXEXCEPTION_VTABLE, "48 8D 05 ?? ?? ?? ?? 48 8B F1 48 89 01 8B FA 48 8B")
	MAKE_SIGNATURE(UNPAUSE_FOCUS, "48 83 EC 28 8B 41 ?? 85 C0 74")
	// the platform's "running in the background" query. polled once a frame; while
	// it answers true gameplay time is held at zero and the pads are paused
	MAKE_SIGNATURE(UNPAUSE_BACKGROUND, "40 53 48 83 EC 20 E8 ?? ?? ?? ?? 48 8B C8 48 8B 10 FF 52 68 84 C0 0F 94 C3 E8 ?? ?? ?? ?? 48 8B C8 48 8B 10 FF 52 78 0A C3")

	// the callback the frame hands to the actor update, run on the game thread
	// between update passes as (pass, phase)
	MAKE_SIGNATURE(ACTOR_UPDATE_PASS_RUNNING, "83 F9 01 0F 85 ?? ?? ?? ?? 53 48 83 EC 20 80 3D ?? ?? ?? ?? 00 8B DA 75 ?? 85 D2 74 ?? 3B D1")

	constexpr uint32_t NXEXCEPTION_VTABLE_ADDRESS = 0x3;
	constexpr uint32_t NXEXCEPTION_VTABLE_INIT = 0x1;

	// note: find signatures for MSMM, MSMR1, MSM2
	// unhooked asset funcs but called
	MAKE_SIGNATURE(CREATE_ASSET_ID, "40 53 48 83 EC ?? 48 8B C2 48 8B D9 48 85 D2 74 3A 80")
	MAKE_SIGNATURE(RESOLVE_ASSET, "48 89 5C 24 ?? 57 48 83 EC 20 4D 63 C0")
	MAKE_SIGNATURE(SET_FILE_STATUS, "48 89 5C 24 ?? 48 89 74 24 ?? 57 48 83 EC 20 8B FA 48 8B D9 48 8D 35")
	MAKE_SIGNATURE(ALLOC_ASSET_RCRA, "48 89 5C 24 ?? 48 89 74 24 ?? 57 48 83 EC 20 F6 D1")
	MAKE_SIGNATURE(COMMIT_ASSET_RCRA, "40 53 48 83 EC 20 69 D1 B0 00 00 00")
	MAKE_SIGNATURE(IS_ASSET_HEADER_VALID_RCRA, "40 53 48 83 EC 20 8B D9 0F B6 CA")
	MAKE_SIGNATURE(SORT, "48 83 EC 48 4C 89 4C 24 ?? 48 8D 05")
	MAKE_SIGNATURE(SORT_FUNC_RCRA, "41 8B 40 ?? 8B 4A ?? 45 0F B6 48")

	// asset data vars
	MAKE_SIGNATURE(LOAD_OPS, "48 8D 0D ?? ?? ?? ?? 0F 28 44 24 ?? 66 0F 7F 44 24 ?? E8 ?? ?? ?? ?? 8B C3")
	MAKE_SIGNATURE(CREATE_ASSET_RCRA, "FF 15 ?? ?? ?? ?? 84 C0 75 ?? C7 47 ?? 04 00 00 00")
	MAKE_SIGNATURE(CREATE_ASSET_DATA_RCRA, "4C 8B 05 ?? ?? ?? ?? 48 8B D6")
	MAKE_SIGNATURE(DISABLE_DIRECTSTORAGE_RCRA, "40 38 2D ?? ?? ?? ?? 48 8B DA")
	MAKE_SIGNATURE(LEGACY_TEXTURE, "0F B6 05 ?? ?? ?? ?? 48 8D 15 ?? ?? ?? ?? 88 83")
	MAKE_SIGNATURE(ARCHIVEFS_VTABLE, "48 8D 05 ?? ?? ?? ?? 48 8D 4F ?? 48 89 07 48 89 5F")

	constexpr uint32_t LOAD_OPS_ADDRESS = 0x3;
	constexpr uint32_t CREATE_ASSET_RCRA_ADDRESS = 0x2;
	constexpr uint32_t CREATE_ASSET_DATA_RCRA_ADDRESS = 0x3;
	constexpr uint32_t DISABLE_DIRECTSTORAGE_RCRA_ADDRESS = 0x3;
	constexpr uint32_t LEGACY_TEXTURE_ADDRESS = 0x3;

	constexpr uint32_t ARCHIVEFS_VTABLE_ADDRESS = 0x3;
	constexpr uint32_t ARCHIVEFS_VTABLE_OPENFILE = 0x6;
	constexpr uint32_t ARCHIVEFS_VTABLE_READFILE = 0x7;
	constexpr uint32_t ARCHIVEFS_VTABLE_CLOSEFILE = 0x9;
	constexpr uint32_t ARCHIVEFS_VTABLE_RESOLVEHANDLE = 0xA;
	constexpr uint32_t ARCHIVEFS_VTABLE_MOUNT = 0x10;

	// hooked asset funcs
	MAKE_SIGNATURE(REL_SET_TEXT_AUDIO_LANGUAGE, "E8 ?? ?? ?? ?? 8B 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 0F B6 0D ?? ?? ?? ?? E8")
	MAKE_SIGNATURE(PRELOAD_LOAD_OP_RCRA, "48 8B C4 44 89 48 ?? 48 89 48 ?? 53 41 55")
	MAKE_SIGNATURE(IS_ASSET_VALID_RCRA, "48 89 5C 24 ?? 48 89 6C 24 ?? 48 89 74 24 ?? 57 48 83 EC 20 48 8B DA 48 8B F9 E8 ?? ?? ?? ?? 8B F0")
	MAKE_SIGNATURE(IS_INSTALLED_ASSET, "48 89 5C 24 ?? 48 89 6C 24 ?? 48 89 74 24 ?? 48 89 7C 24 ?? 41 56 48 83 EC 20 48 8B DA 48 8B F1")
	MAKE_SIGNATURE(WINDOW_INIT_RCRA, "48 89 5C 24 ?? 48 89 74 24 ?? 57 48 83 EC 30 C7 44 24 ?? 00 00 80 41")

	constexpr uint32_t REL_SET_TEXT_LANGUAGE_ADDRESS = 0x1;
	constexpr uint32_t REL_SET_AUDIO_LANGUAGE_ADDRESS = 0xC;

	// (scene, out handle, uid) -> out. the engine's own probe of the uid table
	MAKE_SIGNATURE(ACTOR_HANDLE_BY_UID, "40 53 48 83 EC 20 48 8B DA C7 02 00 00 00 00 49 8B D0 48 81 C1 D0 74 00 00 E8")

	// actor overlay stuff
	MAKE_SIGNATURE(HERO_SYSTEM, "48 8D 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 33 FF 40 38 B8")
	MAKE_SIGNATURE(SCENE_MANAGER, "48 8D 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 48 63 75")
	MAKE_SIGNATURE(ACTOR_ASSET_MANAGER, "48 8D 0D ?? ?? ?? ?? 48 8B 52 ?? E8 ?? ?? ?? ?? 48 8B D8")
	MAKE_SIGNATURE(SPAWN_BOT, "48 85 D2 0F 84 ?? ?? ?? ?? 48 8B C4 48 89 58 ?? 48 89 70 ?? 48 89 78")
	MAKE_SIGNATURE(LOAD_ACTOR_ASSET, "48 89 5C 24 ?? 48 89 6C 24 ?? 48 89 74 24 ?? 57 48 83 EC 30 49 8B F9 49 8B F0 48 8B DA")
	MAKE_SIGNATURE(SWAPCHAIN_VTABLE, "48 8D 05 ?? ?? ?? ?? 48 89 01 66 C7 41 ?? 00 00 C6 41 ?? 00 48 83 C1 10")

	// events. (event system, namehash, sender, targets, target count, exclude targets,
	// position, locator hash, broadcast, radius, delay, ddl data, strings) -> event
	MAKE_SIGNATURE(QUEUE_EVENT, "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 41 56 41 57 48 81 EC 80 00 00 00 48 8B E9 0F 29 74 24 70 48 81 C1 ?? ?? ?? ?? 4D 8B F9 41 8B D8")
	// the frame allocation QueueEvent and one sibling make, both through the global
	// event system. matches twice, both loading the same address
	MAKE_SIGNATURE(EVENT_SYSTEM, "48 8B 7F 20 48 8D 0D ?? ?? ?? ?? 0F 57 C0 0F 2F F0 8B 57 14")

	constexpr uint32_t EVENT_SYSTEM_ADDRESS = 0x7;

	// time scale. (system, channel, scale, ramp, context, fx type)
	MAKE_SIGNATURE(SET_CHANNEL_TIME_SCALE, "83 FA 18 0F 87 ?? ?? ?? ?? 57 48 83 EC 40 0F 29 74 24 30 48 8B F9 0F 29 7C 24 20 0F 28 F3")
	// (system, channel). also drops the components driving that channel
	MAKE_SIGNATURE(CLEAR_CHANNEL_TIME_SCALE, "48 89 5C 24 08 57 48 83 EC 50 48 63 DA 48 8B F9 83 FB 18 0F 87")
	// a gameplay call site that sets the game channel through the global system
	MAKE_SIGNATURE(TIME_SCALE_SYSTEM, "48 8D 0D ?? ?? ?? ?? 45 33 C0 C7 83 ?? ?? ?? ?? 00 00 80 3F E8")

	constexpr uint32_t TIME_SCALE_SYSTEM_ADDRESS = 0x3;

	// camera. the graphics settings apply stores the fov slider into the camera
	// system twice; the second store is the multiplier the view is built with
	MAKE_SIGNATURE(CAMERA_FOV_SCALE, "F3 0F 59 05 ?? ?? ?? ?? 89 35 ?? ?? ?? ?? F3 0F 11 05 ?? ?? ?? ?? F3 0F 11 05 ?? ?? ?? ??")

	constexpr uint32_t CAMERA_FOV_SCALE_ADDRESS = 0x1A;

	constexpr uint32_t HERO_SYSTEM_ADDRESS = 0x3;
	constexpr uint32_t SCENE_MANAGER_ADDRESS = 0x3;
	constexpr uint32_t ACTOR_ASSET_MANAGER_ADDRESS = 0x3;
	constexpr uint32_t SWAPCHAIN_VTABLE_ADDRESS = 0x3;
	constexpr uint32_t SWAPCHAIN_VTABLE_DTOR = 0x0;
} // namespace rivet_hook

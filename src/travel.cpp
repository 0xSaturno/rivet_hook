// SPDX-FileCopyrightText: 2025-2026 Neptuwunium
//
// SPDX-License-Identifier: EUPL-1.2

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cctype>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "travel.hpp"

#include "ddl_visit.hpp"
#include "game/load_system.hpp"
#include "game/scene_manager.hpp"
#include "game_thread.hpp"
#include "runtime.hpp"
#include "scene_query.hpp"
#include "signature.hpp"

using namespace rivet_hook::game;

namespace rivet_hook {
	// overlay_core owns this, it is resolved once during Overlay::Init
	extern SceneManager *g_SceneManager;
} // namespace rivet_hook

namespace rivet_hook::travel {
	// "leave it as it is" for the hero type and the lighting mode. the hero
	// field idles at 4 (none) between warps, and passing that asks for a swap
	constexpr int32_t KEEP_HERO = -1;
	constexpr int32_t KEEP_LIGHTING = -1;
	constexpr uint32_t NO_DIMENSION = 0;

	using request_warp_t = void (*)(void *manager, uint32_t checkpoint, uint32_t dimension, int32_t hero, int32_t lighting);
	using overlay_t = bool (*)(void *load_system, const uint64_t *region);

	// where the load system instance pointer lives. it is set once the game has
	// a load system, so it is read at use
	static void *const *g_instance = nullptr;
	static uint32_t g_checkpoints_offset = 0;
	static request_warp_t g_request_warp = nullptr;
	static overlay_t g_overlay_load = nullptr;
	static overlay_t g_overlay_unload = nullptr;
	// optional, for telling loaded and story driven overlays apart
	static uint32_t g_overlay_manager_offset = 0;
	static const CustomOverlaySystem *g_custom_overlays = nullptr;
	static const char *g_unavailable = "travel was not initialized";

	auto
	init() -> void {
		if (g_request_warp != nullptr) {
			return;
		}

		const auto site = find_address(LOAD_SYSTEM_CHECKPOINTS_SIGNATURE);
		const auto get_instance = reinterpret_cast<const uint8_t *>(load_rel_var(site, LOAD_SYSTEM_GET_INSTANCE_ADDRESS));

		// the getter is mov rax, [instance]; ret. anything else means the
		// signature landed somewhere it should not have
		if (get_instance != nullptr && get_instance[0] == 0x48 && get_instance[1] == 0x8B && get_instance[2] == 0x05 && get_instance[7] == 0xC3) {
			g_instance = static_cast<void *const *>(load_rel_var(reinterpret_cast<intptr_t>(get_instance), LOAD_SYSTEM_INSTANCE_ADDRESS));
			g_checkpoints_offset = *reinterpret_cast<const uint32_t *>(site + LOAD_SYSTEM_CHECKPOINTS_OFFSET);
		}

		g_request_warp = reinterpret_cast<request_warp_t>(find_address(HERO_REQUEST_CHECKPOINT_WARP_SIGNATURE));

		// overlays are optional: without them travel still warps
		g_overlay_load = reinterpret_cast<overlay_t>(load_rel_var(find_address(LOAD_SYSTEM_OVERLAY_LOAD_SIGNATURE), LOAD_SYSTEM_OVERLAY_LOAD_ADDRESS));
		g_overlay_unload = reinterpret_cast<overlay_t>(load_rel_var(find_address(LOAD_SYSTEM_OVERLAY_UNLOAD_SIGNATURE), LOAD_SYSTEM_OVERLAY_UNLOAD_ADDRESS));
		if (g_overlay_load == nullptr || g_overlay_unload == nullptr) {
			g_overlay_load = nullptr;
			g_overlay_unload = nullptr;
			g_output << "[travel] the overlay requests were not found, rivet.overlay is unavailable\n";
		} else {
			// RequestOverlayLoad is a guard then add rcx, <manager offset>
			const auto *code = reinterpret_cast<const uint8_t *>(g_overlay_load);
			if (code[OVERLAY_LOAD_MANAGER_ADD] == 0x48 && code[OVERLAY_LOAD_MANAGER_ADD + 1] == 0x81 && code[OVERLAY_LOAD_MANAGER_ADD + 2] == 0xC1) {
				g_overlay_manager_offset = *reinterpret_cast<const uint32_t *>(code + OVERLAY_LOAD_MANAGER_OFFSET);
			}

			g_custom_overlays = static_cast<const CustomOverlaySystem *>(load_rel_var(find_address(CUSTOM_OVERLAY_SYSTEM_SIGNATURE), CUSTOM_OVERLAY_SYSTEM_ADDRESS));
		}

		if (g_instance == nullptr || g_request_warp == nullptr) {
			g_request_warp = nullptr;
			g_unavailable = "the load system or the warp request was not found";
			g_output << "[travel] the load system or the warp request was not found, rivet.warp is unavailable\n";
		} else {
			g_unavailable = "";
			g_output << "[travel] checkpoints at load system +0x" << std::hex << g_checkpoints_offset << std::dec << ", warp request at " << reinterpret_cast<void *>(g_request_warp) << "\n";
		}

		g_output.flush();
	}

	auto
	unavailable_reason() -> const char * {
		return g_unavailable;
	}

	static auto
	fail(const char **reason, const char *why) -> bool {
		if (reason != nullptr) {
			*reason = why;
		}

		return false;
	}

	// the level's checkpoint table, or null while no level is loaded
	static auto
	manager(const char **reason) -> const CheckpointManager * {
		if (g_request_warp == nullptr) {
			fail(reason, g_unavailable);
			return nullptr;
		}

		auto *load_system = *g_instance;
		if (load_system == nullptr) {
			fail(reason, "the game has no load system yet");
			return nullptr;
		}

		const auto *table = reinterpret_cast<const CheckpointManager *>(static_cast<const uint8_t *>(load_system) + g_checkpoints_offset);
		if (!ddl::is_readable(table, sizeof(CheckpointManager)) || table->checkpoints == nullptr || table->count <= 0) {
			fail(reason, "no level is loaded");
			return nullptr;
		}

		if (!ddl::is_readable(table->checkpoints, sizeof(CheckpointData) * static_cast<size_t>(table->count))) {
			fail(reason, "the checkpoint table is not readable");
			return nullptr;
		}

		return table;
	}

	// the loaded level, with every table the lookups read checked readable, or
	// null while there is none
	static auto
	level(const char **reason) -> const LevelAsset * {
		if (g_request_warp == nullptr) {
			fail(reason, g_unavailable);
			return nullptr;
		}

		const auto *load_system = static_cast<const LoadSystem *>(*g_instance);
		if (load_system == nullptr || !ddl::is_readable(load_system, sizeof(LoadSystem))) {
			fail(reason, "the game has no load system yet");
			return nullptr;
		}

		const auto *asset = load_system->level;
		if (asset == nullptr || !ddl::is_readable(asset, sizeof(LevelAsset)) || asset->regionCount <= 0 || asset->zoneCount <= 0) {
			fail(reason, "no level is loaded");
			return nullptr;
		}

		const auto regions = static_cast<size_t>(asset->regionCount);
		const auto zones = static_cast<size_t>(asset->zoneCount);
		if (!ddl::is_readable(asset->regions, sizeof(LevelRegion) * regions) || !ddl::is_readable(asset->regionNames, sizeof(uint32_t) * regions)
			|| !ddl::is_readable(asset->zones, sizeof(LevelZone) * zones) || !ddl::is_readable(asset->zoneNames, sizeof(uint32_t) * zones)
			|| !ddl::is_readable(asset->zoneRefs, sizeof(int16_t) * static_cast<size_t>(asset->zoneRefCount))
			|| (asset->lodCount > 0 && !ddl::is_readable(asset->lods, sizeof(LevelRegionLod) * static_cast<size_t>(asset->lodCount)))) {
			fail(reason, "the level's tables are not readable");
			return nullptr;
		}

		return asset;
	}

	static auto
	string_of(const LevelAsset *asset, const uint32_t *offsets, const int32_t index, const int32_t count, char *out, const size_t size) -> bool {
		out[0] = '\0';
		const auto *base = reinterpret_cast<const char *>(asset->dataFile >> 16);
		return index >= 0 && index < count && base != nullptr && ddl::read_string(base + offsets[index], out, size);
	}

	static auto
	region_path(const LevelAsset *asset, const int32_t region, char *out, const size_t size) -> bool {
		return string_of(asset, asset->regionNames, asset->regions[region].nameIndex, asset->regionCount, out, size);
	}

	static auto
	zone_path(const LevelAsset *asset, const int32_t zone, char *out, const size_t size) -> bool {
		return string_of(asset, asset->zoneNames, asset->zones[zone].nameIndex, asset->zoneCount, out, size);
	}

	static auto
	type_name(const int16_t type) -> const char * {
		switch (type) {
			case RegionType::Global: return "global";
			case RegionType::Container: return "container";
			case RegionType::Unit: return "unit";
			case RegionType::OpenWorld: return "open_world";
			case RegionType::Tile: return "tile";
			case RegionType::Overlay: return "overlay";
			default: return "unknown";
		}
	}

	// the region at the top of a region's tree
	static auto
	top_of(const LevelAsset *asset, int32_t region) -> int32_t {
		for (auto depth = 0; depth < 32 && region >= 0 && region < asset->regionCount; ++depth) {
			const auto parent = asset->regions[region].parent;
			if (parent < 0 || parent >= asset->regionCount) {
				return region;
			}

			region = parent;
		}

		return region;
	}

	// what a player would call where a region is: its top region's file name,
	// "Savali" or "Savali (open world)"
	static auto
	area_of(const LevelAsset *asset, const int32_t region, char *out, const size_t size) -> void {
		out[0] = '\0';
		const auto top = top_of(asset, region);
		if (top < 0 || top >= asset->regionCount) {
			return;
		}

		char path[0x100];
		if (!region_path(asset, top, path, sizeof(path))) {
			return;
		}

		const auto *leaf = strrchr(path, '/');
		leaf = leaf != nullptr ? leaf + 1 : path;
		auto length = strlen(leaf);
		if (length > 7 && _stricmp(leaf + length - 7, ".region") == 0) {
			length -= 7;
		}

		_snprintf_s(out, size, _TRUNCATE, "%.*s%s", static_cast<int>(length), leaf, asset->regions[top].type == RegionType::OpenWorld ? " (open world)" : "");
	}

	static auto
	find_data(const uint32_t hash) -> const CheckpointData * {
		const auto *table = manager(nullptr);
		if (table == nullptr || hash == 0) {
			return nullptr;
		}

		for (int32_t i = 0; i < table->count; ++i) {
			if (table->checkpoints[i].nameHash == hash) {
				return &table->checkpoints[i];
			}
		}

		return nullptr;
	}

	auto
	checkpoints(const char *filter, const size_t limit) -> nlohmann::json {
		nlohmann::json out;
		nlohmann::json::array_t found;
		auto truncated = false;

		const char *reason = nullptr;
		const auto *table = manager(&reason);
		const auto *asset = table != nullptr ? level(&reason) : nullptr;
		out["reason"] = reason != nullptr ? reason : "";
		if (table != nullptr && asset != nullptr) {
			for (int32_t i = 0; i < table->count; ++i) {
				const auto &checkpoint = table->checkpoints[i];
				char name[0x80];
				if (!ddl::read_string(checkpoint.name, name, sizeof(name))) {
					continue;
				}

				if (filter != nullptr && filter[0] != '\0' && strstr(name, filter) == nullptr) {
					continue;
				}

				if (found.size() >= limit) {
					truncated = true;
					break;
				}

				char hash[16];
				_snprintf_s(hash, sizeof(hash), _TRUNCATE, "0x%08x", checkpoint.nameHash);

				char area[0x80];
				area_of(asset, checkpoint.region, area, sizeof(area));

				nlohmann::json entry;
				entry["name"] = name;
				entry["hash"] = hash;
				entry["region"] = checkpoint.region;
				entry["area"] = area;
				entry["type"] = checkpoint.type;
				entry["position"] = nlohmann::json::array({ checkpoint.position[0], checkpoint.position[1], checkpoint.position[2] });
				found.emplace_back(std::move(entry));
			}
		}

		out["checkpoints"] = found;
		out["truncated"] = truncated;
		return out;
	}

	auto
	find(const char *name) -> uint32_t {
		if (name == nullptr || name[0] == '\0') {
			return 0;
		}

		if (name[0] == '0' && (name[1] == 'x' || name[1] == 'X')) {
			char *end = nullptr;
			const auto hash = static_cast<uint32_t>(strtoul(name, &end, 16));
			return end != name + 2 && *end == '\0' && find_data(hash) != nullptr ? hash : 0;
		}

		const auto *table = manager(nullptr);
		if (table == nullptr) {
			return 0;
		}

		// by name rather than by hash, so the case does not have to match
		for (int32_t i = 0; i < table->count; ++i) {
			char text[0x80];
			if (ddl::read_string(table->checkpoints[i].name, text, sizeof(text)) && _stricmp(text, name) == 0) {
				return table->checkpoints[i].nameHash;
			}
		}

		return 0;
	}

	// the hero's HeroTransitionManager, or null
	static auto
	transition_manager(const char **reason) -> uint8_t * {
		if (g_SceneManager == nullptr) {
			fail(reason, "the scene manager is not available");
			return nullptr;
		}

		const auto handle = scene_query::hero();
		if (handle == 0) {
			fail(reason, "there is no hero right now");
			return nullptr;
		}

		const auto *actor = g_SceneManager->ResolveActor(EngineHandle { .value = handle });
		if (actor == nullptr || actor->components == nullptr || actor->componentCount <= 0 || !ddl::is_readable(actor->components, sizeof(ComponentPointer) * actor->componentCount)) {
			fail(reason, "the hero has no readable component list");
			return nullptr;
		}

		for (auto index = 0; index < actor->componentCount; ++index) {
			const auto [type, instance] = actor->components[index];
			if (type == nullptr || instance == nullptr || !ddl::is_readable(type, sizeof(ComponentInfo))) {
				continue;
			}

			char name[0x40];
			if (!ddl::read_string(type->name, name, sizeof(name)) || strcmp(name, "HeroTransitionManager") != 0) {
				continue;
			}

			if (!ddl::is_writable(instance, HERO_CHECKPOINT_WARP_PENDING + 1) || instance->IsDestroyed()) {
				continue;
			}

			return reinterpret_cast<uint8_t *>(instance);
		}

		fail(reason, "the hero has no HeroTransitionManager");
		return nullptr;
	}

	// the engine call on its own, so a fault is caught with nothing to unwind
	static auto
	call_request(void *manager, const uint32_t checkpoint) -> bool {
#ifdef _MSC_VER
		__try {
#endif
			g_request_warp(manager, checkpoint, NO_DIMENSION, KEEP_HERO, KEEP_LIGHTING);
			return true;
#ifdef _MSC_VER
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			return false;
		}
#endif
	}

	auto
	warp(const uint32_t checkpoint, const char **reason) -> bool {
		if (g_request_warp == nullptr) {
			return fail(reason, g_unavailable);
		}

		// the warp state loads regions and moves the hero
		if (!game_thread::on_game_thread()) {
			return fail(reason, "warps can only start on the game thread, and it is not pumping (loading?)");
		}

		// the warp state reads the checkpoint's data without checking it exists
		const auto *data = find_data(checkpoint);
		if (data == nullptr) {
			return fail(reason, "the level has no checkpoint with that name or hash");
		}

		auto *manager = transition_manager(reason);
		if (manager == nullptr) {
			return false;
		}

		if (manager[HERO_CHECKPOINT_WARP_PENDING] != 0) {
			return fail(reason, "a warp is already on its way");
		}

		if (!call_request(manager, checkpoint)) {
			return fail(reason, "RequestCheckpointWarp faulted");
		}

		char name[0x80];
		g_output << "[travel] warp to " << (ddl::read_string(data->name, name, sizeof(name)) ? name : "?") << " (region " << std::dec << data->region << ")\n";
		g_output.flush();
		return true;
	}

	// ----------------------------------------------------------------- zones --

	// level paths mix I29 and i29, so every match ignores case
	static auto
	contains(const char *text, const char *fragment) -> bool {
		if (fragment == nullptr || fragment[0] == '\0') {
			return true;
		}

		for (; *text != '\0'; ++text) {
			auto i = 0;
			while (fragment[i] != '\0' && text[i] != '\0' && tolower(static_cast<unsigned char>(text[i])) == tolower(static_cast<unsigned char>(fragment[i]))) {
				++i;
			}

			if (fragment[i] == '\0') {
				return true;
			}
		}

		return false;
	}

	// whether the load system has an overlay loaded: 1, 0, or -1 when that is
	// not known
	static auto
	overlay_loaded(const uint64_t asset) -> int32_t {
		if (g_overlay_manager_offset == 0) {
			return -1;
		}

		const auto *manager = reinterpret_cast<const OverlayManager *>(static_cast<const uint8_t *>(*g_instance) + g_overlay_manager_offset);
		if (!ddl::is_readable(manager, sizeof(OverlayManager)) || manager->entries == nullptr || manager->count <= 0 || !ddl::is_readable(manager->entries, sizeof(OverlayEntry) * static_cast<size_t>(manager->count))) {
			return -1;
		}

		for (int32_t i = 0; i < manager->count; ++i) {
			if (manager->entries[i].asset == asset) {
				return (manager->entries[i].flags & OverlayFlag::Loaded) != 0 ? 1 : 0;
			}
		}

		return -1;
	}

	// whether the story drives an overlay: the game then loads and unloads it
	// with its mission state, and undoes any request of ours
	static auto
	story_overlay(const uint64_t asset) -> bool {
		const auto *system = g_custom_overlays;
		if (system == nullptr || !ddl::is_readable(system, sizeof(CustomOverlaySystem)) || system->overlays == nullptr || system->count <= 0) {
			return false;
		}

		if (!ddl::is_readable(system->overlays, sizeof(CustomOverlay) * static_cast<size_t>(system->count))) {
			return false;
		}

		for (int32_t i = 0; i < system->count; ++i) {
			if (system->overlays[i].asset == asset) {
				return true;
			}
		}

		return false;
	}

	// zone -> the regions listing it, built once per level
	static const LevelAsset *g_indexed = nullptr;
	static std::vector<std::vector<int16_t>> g_zone_regions;

	static auto
	regions_of(const LevelAsset *asset, const int32_t zone) -> const std::vector<int16_t> & {
		if (g_indexed != asset || g_zone_regions.size() != static_cast<size_t>(asset->zoneCount)) {
			g_zone_regions.assign(static_cast<size_t>(asset->zoneCount), {});
			for (int32_t r = 0; r < asset->regionCount; ++r) {
				const auto &region = asset->regions[r];
				if (region.zoneRefStart < 0 || region.zoneRefCount <= 0 || region.zoneRefStart + region.zoneRefCount > asset->zoneRefCount) {
					continue;
				}

				for (auto k = 0; k < region.zoneRefCount; ++k) {
					const auto listed = asset->zoneRefs[region.zoneRefStart + k];
					if (listed >= 0 && listed < asset->zoneCount) {
						g_zone_regions[listed].push_back(static_cast<int16_t>(r));
					}
				}
			}

			g_indexed = asset;
		}

		return g_zone_regions[zone];
	}

	enum class RouteKind : uint8_t {
		None,
		Loaded,
		Checkpoint,
		Overlay,
		Story, // an overlay the story drives, out of reach
	};

	static auto
	kind_name(const RouteKind kind) -> const char * {
		switch (kind) {
			case RouteKind::Loaded: return "loaded";
			case RouteKind::Checkpoint: return "checkpoint";
			case RouteKind::Overlay: return "overlay";
			case RouteKind::Story: return "story";
			default: return "none";
		}
	}

	struct Route {
		RouteKind kind = RouteKind::None;
		int32_t region = -1;
		const CheckpointData *checkpoint = nullptr;
	};

	// a checkpoint that loads a region: one in the region itself, for a tile the
	// nearest one in the same open world (tiles stream by distance), for a
	// container or an open world any one under it
	static auto
	checkpoint_for(const LevelAsset *asset, const CheckpointManager *table, const int32_t region) -> const CheckpointData * {
		const auto &target = asset->regions[region];
		const auto top = top_of(asset, region);
		const auto *lod = target.type == RegionType::Tile && target.lodIndex >= 0 && target.lodIndex < asset->lodCount ? &asset->lods[target.lodIndex] : nullptr;

		const CheckpointData *best = nullptr;
		auto best_distance = 0.0f;
		for (int32_t i = 0; i < table->count; ++i) {
			const auto &checkpoint = table->checkpoints[i];
			if (checkpoint.region == region) {
				return &checkpoint;
			}

			if (lod != nullptr && top_of(asset, checkpoint.region) == top) {
				const auto dx = checkpoint.position[0] - lod->position[0];
				const auto dz = checkpoint.position[2] - lod->position[2];
				if (best == nullptr || dx * dx + dz * dz < best_distance) {
					best = &checkpoint;
					best_distance = dx * dx + dz * dz;
				}
			} else if (best == nullptr && (target.type == RegionType::Container || target.type == RegionType::OpenWorld) && top_of(asset, checkpoint.region) == region) {
				best = &checkpoint;
			}
		}

		return best;
	}

	// how to get a zone loaded, preferring always loaded, then a warp, then an
	// overlay on top of wherever the hero is
	static auto
	route_for(const LevelAsset *asset, const CheckpointManager *table, const int32_t zone) -> Route {
		Route best;
		for (const auto region : regions_of(asset, zone)) {
			const auto type = asset->regions[region].type;
			if (type == RegionType::Global) {
				return { RouteKind::Loaded, region, nullptr };
			}

			if (best.kind == RouteKind::Checkpoint) {
				continue;
			}

			if (type == RegionType::Overlay) {
				const auto kind = story_overlay(asset->regions[region].asset) ? RouteKind::Story : RouteKind::Overlay;
				if (best.kind == RouteKind::None || (best.kind == RouteKind::Story && kind == RouteKind::Overlay)) {
					best = { kind, region, nullptr };
				}

				continue;
			}

			if (const auto *checkpoint = checkpoint_for(asset, table, region); checkpoint != nullptr) {
				best = { RouteKind::Checkpoint, region, checkpoint };
			} else if (best.kind == RouteKind::None) {
				best.region = region;
			}
		}

		return best;
	}

	static auto
	region_json(const LevelAsset *asset, const int32_t region) -> nlohmann::json {
		char path[0x100];
		region_path(asset, region, path, sizeof(path));

		nlohmann::json out;
		out["index"] = region;
		out["path"] = path;
		out["type"] = type_name(asset->regions[region].type);
		return out;
	}

	auto
	regions(const char *filter, const size_t limit) -> nlohmann::json {
		nlohmann::json out;
		nlohmann::json::array_t found;
		auto truncated = false;

		const char *reason = nullptr;
		const auto *asset = level(&reason);
		out["reason"] = reason != nullptr ? reason : "";
		for (int32_t r = 0; asset != nullptr && r < asset->regionCount; ++r) {
			char path[0x100];
			if (!region_path(asset, r, path, sizeof(path)) || !contains(path, filter)) {
				continue;
			}

			if (found.size() >= limit) {
				truncated = true;
				break;
			}

			char area[0x80];
			area_of(asset, r, area, sizeof(area));

			auto entry = region_json(asset, r);
			entry["area"] = area;
			entry["zones"] = asset->regions[r].zoneRefCount;
			if (asset->regions[r].type == RegionType::Overlay) {
				const auto loaded = overlay_loaded(asset->regions[r].asset);
				entry["loaded"] = loaded < 0 ? nlohmann::json(nullptr) : nlohmann::json(loaded == 1);
				entry["story"] = story_overlay(asset->regions[r].asset);
			}

			found.emplace_back(std::move(entry));
		}

		out["regions"] = found;
		out["truncated"] = truncated;
		return out;
	}

	auto
	zones(const char *filter, const size_t limit) -> nlohmann::json {
		nlohmann::json out;
		nlohmann::json::array_t found;
		auto truncated = false;

		const char *reason = nullptr;
		const auto *table = manager(&reason);
		const auto *asset = table != nullptr ? level(&reason) : nullptr;
		out["reason"] = reason != nullptr ? reason : "";
		for (int32_t z = 0; asset != nullptr && z < asset->zoneCount; ++z) {
			char path[0x100];
			if (!zone_path(asset, z, path, sizeof(path)) || !contains(path, filter)) {
				continue;
			}

			if (found.size() >= limit) {
				truncated = true;
				break;
			}

			char id[20];
			_snprintf_s(id, sizeof(id), _TRUNCATE, "%016llx", asset->zones[z].asset);

			nlohmann::json::array_t listed;
			for (const auto region : regions_of(asset, z)) {
				listed.emplace_back(region_json(asset, region));
			}

			const auto route = route_for(asset, table, z);
			nlohmann::json way;
			way["kind"] = kind_name(route.kind);
			way["region"] = route.region;
			char name[0x80] = {};
			if (route.checkpoint != nullptr) {
				ddl::read_string(route.checkpoint->name, name, sizeof(name));
			}

			way["checkpoint"] = name[0] != '\0' ? nlohmann::json(name) : nlohmann::json(nullptr);

			nlohmann::json entry;
			entry["path"] = path;
			entry["asset"] = id;
			entry["regions"] = std::move(listed);
			entry["route"] = std::move(way);
			found.emplace_back(std::move(entry));
		}

		out["zones"] = found;
		out["truncated"] = truncated;
		return out;
	}

	static auto
	is_hex_id(const char *text) -> bool {
		if (text == nullptr || strlen(text) != 16) {
			return false;
		}

		for (const auto *c = text; *c != '\0'; ++c) {
			if (isxdigit(static_cast<unsigned char>(*c)) == 0) {
				return false;
			}
		}

		return true;
	}

	// a zone by asset id, exact path, or a fragment only one path contains
	static auto
	resolve_zone(const LevelAsset *asset, const char *text, const char **reason) -> int32_t {
		if (is_hex_id(text)) {
			const auto id = _strtoui64(text, nullptr, 16);
			for (int32_t z = 0; z < asset->zoneCount; ++z) {
				if (asset->zones[z].asset == id) {
					return z;
				}
			}

			fail(reason, "the level has no zone with that asset id");
			return -1;
		}

		auto match = -1;
		auto matches = 0;
		for (int32_t z = 0; z < asset->zoneCount; ++z) {
			char path[0x100];
			if (!zone_path(asset, z, path, sizeof(path))) {
				continue;
			}

			if (_stricmp(path, text) == 0) {
				return z;
			}

			if (contains(path, text)) {
				match = z;
				++matches;
			}
		}

		if (matches == 1) {
			return match;
		}

		fail(reason, matches == 0 ? "the level has no zone whose path contains that" : "several zones contain that, give more of the path");
		return -1;
	}

	// an overlay region by index, exact path, or a fragment only one overlay
	// path contains
	static auto
	resolve_overlay(const LevelAsset *asset, const char *text, const char **reason) -> int32_t {
		if (text[0] != '\0' && strspn(text, "0123456789") == strlen(text)) {
			const auto index = atoi(text);
			if (index >= 0 && index < asset->regionCount) {
				return index;
			}

			fail(reason, "there is no region with that index");
			return -1;
		}

		auto match = -1;
		auto matches = 0;
		for (int32_t r = 0; r < asset->regionCount; ++r) {
			if (asset->regions[r].type != RegionType::Overlay) {
				continue;
			}

			char path[0x100];
			if (!region_path(asset, r, path, sizeof(path))) {
				continue;
			}

			if (_stricmp(path, text) == 0) {
				return r;
			}

			if (contains(path, text)) {
				match = r;
				++matches;
			}
		}

		if (matches == 1) {
			return match;
		}

		fail(reason, matches == 0 ? "the level has no overlay whose path contains that" : "several overlays contain that, give more of the path");
		return -1;
	}

	// the engine call on its own, so a fault is caught with nothing to unwind
	static auto
	call_overlay(const bool load, const uint64_t region, bool *accepted) -> bool {
#ifdef _MSC_VER
		__try {
#endif
			*accepted = (load ? g_overlay_load : g_overlay_unload)(*g_instance, &region);
			return true;
#ifdef _MSC_VER
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			return false;
		}
#endif
	}

	static auto
	overlay_region(const LevelAsset *asset, const int32_t region, const bool load, char *message, const size_t message_size, const char **reason) -> bool {
		if (asset->regions[region].type != RegionType::Overlay) {
			return fail(reason, "that region is not an overlay");
		}

		// the story's overlay system undoes the request within a frame, and
		// forcing it would fight the mission state
		if (story_overlay(asset->regions[region].asset)) {
			return fail(reason, "the story drives that overlay: the game loads and unloads it with its mission state");
		}

		bool accepted = false;
		if (!call_overlay(load, asset->regions[region].asset, &accepted)) {
			return fail(reason, load ? "RequestOverlayLoad faulted" : "RequestOverlayUnload faulted");
		}

		if (!accepted) {
			return fail(reason, "the load system refused, it is loading or unloading something else");
		}

		char path[0x100];
		region_path(asset, region, path, sizeof(path));
		_snprintf_s(message, message_size, _TRUNCATE, "%s overlay %s", load ? "loading" : "unloading", path);
		g_output << "[travel] " << message << "\n";
		g_output.flush();
		return true;
	}

	auto
	go(const char *zone, char *message, const size_t message_size, const char **reason) -> bool {
		message[0] = '\0';
		if (!game_thread::on_game_thread()) {
			return fail(reason, "travel can only start on the game thread, and it is not pumping (loading?)");
		}

		const auto *table = manager(reason);
		const auto *asset = table != nullptr ? level(reason) : nullptr;
		if (asset == nullptr) {
			return false;
		}

		const auto index = resolve_zone(asset, zone, reason);
		if (index < 0) {
			return false;
		}

		const auto route = route_for(asset, table, index);
		switch (route.kind) {
			case RouteKind::Loaded:
				_snprintf_s(message, message_size, _TRUNCATE, "that zone is always loaded");
				return true;
			case RouteKind::Checkpoint: {
				if (!warp(route.checkpoint->nameHash, reason)) {
					return false;
				}

				char name[0x80];
				ddl::read_string(route.checkpoint->name, name, sizeof(name));
				_snprintf_s(message, message_size, _TRUNCATE, "warping to %s", name);
				return true;
			}
			case RouteKind::Overlay:
				if (g_overlay_load == nullptr) {
					return fail(reason, "the zone is in an overlay, and the overlay requests were not found");
				}

				return overlay_region(asset, route.region, true, message, message_size, reason);
			case RouteKind::Story:
				return fail(reason, "that zone is in a story overlay: the game loads it with the mission state it belongs to, and unloads it again if anything else does");
			default:
				return fail(reason, "no checkpoint loads that zone's region, and it is not in an overlay");
		}
	}

	auto
	overlay(const char *region, const bool load, char *message, const size_t message_size, const char **reason) -> bool {
		message[0] = '\0';
		if (g_overlay_load == nullptr) {
			return fail(reason, "the overlay requests were not found");
		}

		if (!game_thread::on_game_thread()) {
			return fail(reason, "overlays can only load on the game thread, and it is not pumping (loading?)");
		}

		const auto *asset = level(reason);
		if (asset == nullptr) {
			return false;
		}

		const auto index = resolve_overlay(asset, region, reason);
		return index >= 0 && overlay_region(asset, index, load, message, message_size, reason);
	}
} // namespace rivet_hook::travel

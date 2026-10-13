// SPDX-FileCopyrightText: 2025-2026 Neptuwunium
//
// SPDX-License-Identifier: EUPL-1.2

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "collision.hpp"

#include "ddl_visit.hpp"
#include "game/asset.hpp"
#include "game_thread.hpp"
#include "runtime.hpp"
#include "signature.hpp"
#include "travel.hpp"

using namespace rivet_hook::game;

namespace rivet_hook::collision {
	// ------------------------------------------------------------- layouts --

	// offsets, sizes and values measured on the shipping exe and on the baked
	// collision of shipped zones. the hook reads these in engine memory and
	// fills them in in buffers of its own; the one thing it writes that it did
	// not allocate itself is the user data and tag codec of a shape it cooked

	// the exe the layouts were measured on. the signatures can match a later
	// build whose structs moved, so any other exe gets the reading stage only
	constexpr uint32_t MEASURED_TIMESTAMP = 0x6A43E27A;
	constexpr uint32_t MEASURED_IMAGE_SIZE = 0x7155000;
	// a byte the body teardown reads to tell whether a body holds a reference on
	// its shape. at a fixed place, so only read on the measured exe
	constexpr uint32_t BODY_HOLDS_SHAPE_RVA = 0x5DFD2D0;

	// the engine's physics system: its Havok world, the callback it runs on
	// every body cinfo it loads, and the two queues zones load and unload their
	// physics through, which share one capacity
	constexpr uint32_t PHYSICS_WORLD = 0x18;
	constexpr uint32_t PHYSICS_CALLBACK = 0x60;
	constexpr uint32_t PHYSICS_LOAD_QUEUE = 0x1EC18;
	constexpr uint32_t PHYSICS_LOAD_COUNT = 0x1EC20;
	constexpr uint32_t PHYSICS_UNLOAD_QUEUE = 0x1EC28;
	constexpr uint32_t PHYSICS_UNLOAD_COUNT = 0x1EC30;
	constexpr uint32_t PHYSICS_QUEUE_CAPACITY = 0x1EC44;
	constexpr uint32_t PHYSICS_SIZE = 0x1EC48;
	// a load entry is zone, system data or system, and whether it only re-adds.
	// an unload entry is system, file, and whether it only removes
	constexpr uint32_t QUEUE_ENTRY_SIZE = 0x18;
	constexpr uint32_t QUEUE_ENTRY_SECOND = 0x8;
	constexpr uint32_t QUEUE_ENTRY_FLAG = 0x10;
	constexpr int32_t MAX_QUEUE_CAPACITY = 0x1000;

	// the zone manager is an asset manager with one zone per slot of its pool
	constexpr uint32_t ZONE_MANAGER_ASSETS = 0x40;
	constexpr uint32_t ZONE_MANAGER_ASSET_SIZE = 0x48;
	constexpr uint32_t ZONE_MANAGER_LAST = 0x70;
	constexpr uint32_t ZONE_MANAGER_ZONES = 0x120;
	constexpr uint32_t ZONE_MANAGER_SIZE = 0x128;
	constexpr int32_t MAX_ZONES = 0x10000;
	// the manager's bounds checked (manager, index) -> zone, whole. it is the
	// one place the three numbers above stand together
	constexpr uint8_t ZONE_BY_INDEX_CODE[] = {
		0x85, 0xD2, 0x78, 0x17, 0x3B, 0x51, 0x70, 0x7F, 0x12, 0x48, 0x63, 0xC2, 0x48, 0x69, 0xC0, 0x70, 0x02, 0x00, 0x00, 0x48, 0x03, 0x81, 0x20, 0x01, 0x00, 0x00, 0xC3,
	};

	constexpr uint32_t ZONE_SIZE = 0x270;
	constexpr uint32_t ZONE_ASSET = 0x0;
	constexpr uint32_t ZONE_FLAGS = 0x188;
	constexpr uint32_t ZONE_PHYSICS_ROOT = 0x1B8;
	constexpr uint32_t ZONE_PHYSICS_SYSTEM = 0x1C0;
	constexpr uint32_t ZONE_FLAG_PHYSICS = 0x10;
	// the zone is placed by a matrix, which the engine then applies to every
	// body cinfo it loads for it
	constexpr uint32_t ZONE_FLAG_MATRIX = 0x20000;
	constexpr uint32_t ZONE_ASSET_PHYSICS_FILE = 0x38;

	// hkReferencedObject: a size word and a reference count. a size of zero is
	// an object nobody counts, which the reference calls leave alone
	constexpr uint32_t OBJECT_MEM_SIZE = 0x10;
	constexpr uint16_t MEM_SIZE_COUNTED = 0xFFFF;

	// hkArray: pointer, size, capacity with flags. the top bit is storage the
	// array does not own
	constexpr uint32_t ARRAY_SIZE = 0x8;
	constexpr uint32_t ARRAY_CAPACITY = 0xC;
	constexpr uint32_t ARRAY_NOT_OWNED = 0x80000000;

	// hknpPhysicsSystemData
	constexpr uint32_t DATA_MATERIALS = 0x18;
	constexpr uint32_t DATA_MOTION_PROPERTIES = 0x28;
	constexpr uint32_t DATA_CINFOS = 0x38;
	constexpr uint32_t DATA_CONSTRAINTS = 0x48;
	constexpr uint32_t DATA_REFERENCED = 0x58;
	constexpr uint32_t DATA_STEP_MULTIPLIER = 0x70;
	constexpr uint32_t DATA_SIZE = 0x78;

	// hknpPhysicsSystem
	constexpr uint32_t SYSTEM_DATA = 0x18;
	constexpr uint32_t SYSTEM_WORLD = 0x20;
	constexpr uint32_t SYSTEM_BODY_IDS = 0x28;
	constexpr uint32_t SYSTEM_BODY_COUNT = 0x30;
	constexpr uint32_t SYSTEM_OWNS_BODIES = 0x48;

	constexpr uint32_t WORLD_BODIES = 0x28;
	constexpr uint32_t WORLD_BODY_COUNT = 0x30;

	// hknpBody. the low 24 bits of an id are the slot, the rest a serial
	constexpr uint32_t BODY_SIZE = 0xC0;
	constexpr uint32_t BODY_MOTION = 0x40;
	constexpr uint32_t BODY_FLAGS = 0x44;
	constexpr uint32_t BODY_SHAPE = 0x60;
	constexpr uint32_t BODY_QUALITY = 0x69;
	constexpr uint32_t BODY_MATERIAL = 0x6A;
	constexpr uint32_t BODY_FILTER = 0x6C;
	constexpr uint32_t BODY_ID = 0x70;
	constexpr uint32_t BODY_BROADPHASE = 0x78;
	constexpr uint32_t BODY_USER_DATA = 0x98;
	constexpr uint32_t BODY_FLAGS_IN_USE = 0x3;
	constexpr uint32_t BODY_INDEX_MASK = 0xFFFFFF;
	constexpr uint32_t BODY_NONE = 0xFFFFFFFF;

	// the body cinfo a system data holds: hknpBodyCinfo and, past its end, the
	// index of the body it is attached to
	constexpr uint32_t CINFO_SHAPE = 0x0;
	constexpr uint32_t CINFO_FLAGS = 0x8;
	constexpr uint32_t CINFO_FILTER = 0x10;
	constexpr uint32_t CINFO_MATERIAL = 0x14;
	constexpr uint32_t CINFO_QUALITY = 0x16;
	constexpr uint32_t CINFO_USER_DATA = 0x20;
	constexpr uint32_t CINFO_MOTION_TYPE = 0x28;
	constexpr uint32_t CINFO_POSITION = 0x30;
	constexpr uint32_t CINFO_ORIENTATION = 0x40;
	constexpr uint32_t CINFO_ATTACHED = 0xA0;
	constexpr uint32_t CINFO_SIZE = 0xB0;
	// zero would send the system's constructor down its attached body path
	constexpr int32_t CINFO_NOT_ATTACHED = -1;

	constexpr uint32_t MATERIAL_SIZE = 0x70;
	constexpr uint32_t MATERIAL_VALUES = 0x28;

	constexpr uint8_t SHAPE_TYPE_MESH = 9;
	constexpr uint8_t SHAPE_TYPE_COMPOUND = 12;
	constexpr uint16_t SHAPE_FLAGS_MESH = 4;
	constexpr uint8_t SHAPE_DISPATCH_MESH = 3;
	constexpr uint32_t SHAPE_USER_DATA = 0x30;
	constexpr uint32_t SHAPE_CODEC_INFO = 0x40;

	constexpr uint32_t MESH_SIZE = 0x90;
	constexpr uint32_t MESH_DATA = 0x50;
	constexpr uint32_t MESH_TRIANGLES = 0x70;
	constexpr uint32_t MESH_CONVEX_SHAPES = 0x74;
	constexpr uint32_t MESH_DATA_AABB_MIN = 0x30;
	constexpr uint32_t MESH_DATA_AABB_MAX = 0x40;
	constexpr uint32_t MESH_DATA_PRIMITIVE_KEYS = 0x50;
	constexpr uint32_t MESH_DATA_BITS_PER_KEY = 0x54;
	constexpr uint32_t MESH_DATA_PRIMITIVES = 0x70;
	constexpr uint32_t MESH_DATA_RUNS = 0xB0;
	constexpr uint32_t MESH_DATA_NODES = 0xC0;

	// where a compound keeps its instances is read off the baked files, not off
	// code, so inspect also shows the compound's bytes
	constexpr uint32_t COMPOUND_SIZE = 0xC0;
	constexpr uint32_t COMPOUND_INSTANCES = 0x50;
	// four columns first, the last of them the translation
	constexpr uint32_t INSTANCE_SIZE = 0x70;
	constexpr uint32_t INSTANCE_SCALE = 0x40;
	constexpr uint32_t INSTANCE_SHAPE = 0x50;
	constexpr uint32_t INSTANCE_TAG = 0x58;

	// the geometry the default mesh cinfo reads: two arrays the builder does
	// not own. its vtable is never called through and stays null
	constexpr uint32_t GEOMETRY_SIZE = 0x38;
	constexpr uint32_t GEOMETRY_VERTICES = 0x18;
	constexpr uint32_t GEOMETRY_TRIANGLES = 0x28;
	constexpr uint32_t MESH_CINFO_SIZE = 0x58;
	constexpr uint32_t MESH_CINFO_GEOMETRY = 0x40;
	constexpr uint32_t MESH_CINFO_CONVEX = 0x48;
	constexpr uint32_t MESH_CINFO_CONVEX_COUNT = 0x50;

	// what the load queue's call walks from a zone's physics root to its system
	// data: the root's first variant, that variant's scene, the scene's one
	// system data
	constexpr uint32_t ROOT_VARIANT_COUNT = 0x8;
	constexpr uint32_t VARIANT_SIZE = 0x18;
	constexpr uint32_t VARIANT_SCENE = 0x10;
	constexpr uint32_t SCENE_SIZE = 0x30;
	constexpr uint32_t SCENE_SYSTEM_DATAS = 0x18;
	constexpr uint32_t SCENE_SYSTEM_DATA_COUNT = 0x20;

	// the heap behind a thread's memory router, and its two block calls
	constexpr uint32_t ROUTER_HEAP = 0x58;
	constexpr uint32_t HEAP_BLOCK_ALLOC = 1;
	constexpr uint32_t HEAP_BLOCK_FREE = 2;
	// TlsAlloc hands out indices below this
	constexpr uint32_t MAX_TLS_INDEX = 1088;

	constexpr const char *WORLD_CLASS = ".?AVhknpWorld@@";
	constexpr const char *SYSTEM_CLASS = ".?AVhknpPhysicsSystem@@";
	constexpr const char *SYSTEM_DATA_CLASS = ".?AVhknpPhysicsSystemData@@";
	constexpr const char *MATERIAL_CLASS = ".?AVhknpMaterial@@";
	constexpr const char *MESH_CLASS = ".?AVhknpCompressedMeshShape@@";
	constexpr const char *MESH_DATA_CLASS = ".?AUhknpCompressedMeshShapeData@@";
	constexpr const char *COMPOUND_CLASS = ".?AVhknpCompoundShape@@";
	constexpr const char *MESH_CINFO_CLASS = ".?AVhknpCompressedMeshShapeCinfo@@";
	constexpr const char *DEFAULT_MESH_CINFO_CLASS = ".?AVhknpDefaultCompressedMeshShapeCinfo@@";

	// --------------------------------------------------------- conventions --

	// a zone body's collision filter: the layer and object index every shipped
	// zone body has, over the gameplay flags of its triangles
	constexpr uint32_t FILTER_ZONE_BODY = 0xFFF82000;
	constexpr uint32_t FLAGS_MASK = 0x7FF;
	// with this flag a hit reads the shape's user data another way
	constexpr uint32_t FLAG_OTHER_USER_DATA = 0x100;
	// the builder welds vertices closer than this
	constexpr float WELD_TOLERANCE = 0.01f;
	// a level and its broadphase keep within this of the origin on every axis
	constexpr float MAX_COORDINATE = 4096.0f;
	constexpr float DEGREES = 3.14159265358979f / 180.0f;
	constexpr int32_t MAX_TRIANGLES = 0x10000;
	constexpr size_t MAX_OBJ_BYTES = 32u << 20;
	constexpr uint8_t FILL = 0xCC;

	// one field a constructor is known to write
	struct Field {
		uint32_t offset;
		uint32_t size;
		uint64_t value;
	};

	// hknpBodyCinfo's defaults: a static body at the origin, not turned
	constexpr Field BODY_CINFO_DEFAULTS[] = {
		{ 0x00, 8, 0 }, { 0x08, 4, 0 }, { 0x0C, 2, 0 }, { 0x10, 4, 0 }, { 0x14, 2, 0 }, { 0x16, 1, 0xFF }, { 0x18, 8, 0 }, { 0x20, 8, 0 },
		{ 0x28, 1, 0 }, { 0x30, 8, 0 }, { 0x38, 8, 0 }, { 0x40, 8, 0 }, { 0x48, 8, 0x3F80000000000000 }, { 0x50, 8, 0 }, { 0x58, 8, 0 }, { 0x60, 8, 0 },
		{ 0x68, 8, 0 }, { 0x70, 4, 0xBF800000 }, { 0x78, 8, 0 }, { 0x80, 2, 0xFFFF }, { 0x84, 4, 0x00FFFFFF }, { 0x88, 4, 0x7FFFFFFF }, { 0x8C, 4, 0 }, { 0x90, 8, 0 },
	};

	// hknpCompressedMeshShapeCinfo's build defaults, past its vtable
	constexpr Field MESH_CINFO_DEFAULTS[] = {
		{ 0x08, 4, 0 }, { 0x0C, 1, 1 }, { 0x10, 4, 0x3C23D70A }, { 0x14, 1, 1 }, { 0x18, 4, 0x33D6BF95 }, { 0x1C, 4, 0x7F7FFFEE },
		{ 0x20, 4, 0x7F7FFFEE }, { 0x24, 4, 0x00000100 }, { 0x28, 4, 1 }, { 0x30, 8, 0 }, { 0x38, 8, 0 },
	};

	// hknpMaterial past its vtable, and its values from +0x28 on, which are the
	// same bytes the one material of a shipped zone has
	constexpr Field MATERIAL_DEFAULTS[] = { { 0x08, 8, 0 }, { 0x10, 4, 0x0001FFFF }, { 0x20, 8, 0 } };
	constexpr uint8_t MATERIAL_DEFAULT_VALUES[MATERIAL_SIZE - MATERIAL_VALUES] = {
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xFF, 0x00, 0x3F, 0x00, 0x3F, 0x00, 0x00, 0x01, 0x02, 0x00, 0x00, 0xCD, 0xCC, 0x4C, 0x3D,
		0xEE, 0xFF, 0x7F, 0x7F, 0x00, 0x00, 0x80, 0x3F, 0x00, 0x00, 0x80, 0x3F, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xA0, 0x40, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	};

#pragma pack(push, 1)
	// the start every hknpShape has
	struct ShapeHeader {
		const void *vtable;
		uint64_t unknown08;
		uint16_t memSize;
		uint16_t references;
		uint8_t unknown14[0xC];
		uint16_t flags;
		uint8_t type;
		uint8_t keyBits;
		uint8_t dispatch;
		uint8_t unknown25[3];
		float radius;
		uint32_t unknown2c;
		uint64_t userData;
		const void *properties;
		uint32_t codecInfo;
	};

	// one triangle as the default mesh cinfo reads it: three vertex indices,
	// and the tag every hit on it decodes its gameplay flags from
	struct Triangle {
		int32_t a;
		int32_t b;
		int32_t c;
		uint16_t tag;
		uint16_t padding;
	};

	// the locator msvc's rtti keeps in the slot before a vtable's first
	struct ObjectLocator {
		uint32_t signature;
		uint32_t offset;
		uint32_t constructorOffset;
		uint32_t typeDescriptor;
		uint32_t classDescriptor;
		uint32_t self;
	};
#pragma pack(pop)

	static_assert(offsetof(ShapeHeader, references) == 0x12, "ShapeHeader references offset is not 0x12");
	static_assert(offsetof(ShapeHeader, type) == 0x22, "ShapeHeader type offset is not 0x22");
	static_assert(offsetof(ShapeHeader, userData) == 0x30, "ShapeHeader userData offset is not 0x30");
	static_assert(offsetof(ShapeHeader, codecInfo) == 0x40, "ShapeHeader codecInfo offset is not 0x40");
	static_assert(sizeof(Triangle) == 0x10, "Triangle size is not 0x10");

	constexpr uint32_t TYPE_DESCRIPTOR_NAME = 0x10;

	// ---------------------------------------------------------- the engine --

	using construct_t = void *(*) (void *object);
	using cook_t = void *(*) (void *memory, const void *cinfo);
	using release_t = void (*)(const void *object);
	// the four queue calls: the physics system and two pointers
	using queue_t = void (*)(void *physics, void *first, void *second);
	using block_alloc_t = void *(*) (void *heap, int32_t size);
	using block_free_t = void (*)(void *heap, void *memory, int32_t size);

	static construct_t g_mesh_cinfo_ctor = nullptr;
	static cook_t g_mesh_shape_ctor = nullptr;
	static release_t g_remove_reference = nullptr;
	static construct_t g_body_cinfo_ctor = nullptr;
	static construct_t g_material_ctor = nullptr;
	static queue_t g_delay_load = nullptr;
	static queue_t g_delay_enable = nullptr;
	static queue_t g_delay_disable = nullptr;
	static queue_t g_delay_unload = nullptr;

	// the four of them as the log and the refusals name them
	constexpr const char *QUEUE_LOAD = "the zone physics queue (load)";
	constexpr const char *QUEUE_ENABLE = "the zone physics queue (enable)";
	constexpr const char *QUEUE_DISABLE = "the zone physics queue (disable)";
	constexpr const char *QUEUE_UNLOAD = "the zone physics queue (unload)";

	static uint8_t *g_physics = nullptr;
	static const uint8_t *g_zones = nullptr;
	static const uint8_t *g_enabled = nullptr;
	static const uint8_t *g_paused = nullptr;
	static const float *g_timestep = nullptr;
	static const uint32_t *g_tls_index = nullptr;
	static void *const *g_fallback_router = nullptr;
	static void *const *g_tls_get_value = nullptr;
	static const void *g_default_cinfo_vtable = nullptr;
	static const void *g_base_cinfo_vtable = nullptr;
	static const void *g_mesh_vtable = nullptr;
	static const void *g_material_vtable = nullptr;
	static const void *g_system_data_vtable = nullptr;

	static uintptr_t g_image = 0;
	static size_t g_image_size = 0;
	static uint32_t g_timestamp = 0;

	enum : int32_t {
		SCAN_MESH_CINFO_SITE,
		SCAN_MESH_CINFO_CTOR,
		SCAN_MESH_SHAPE_CTOR,
		SCAN_REMOVE_REFERENCE,
		SCAN_BODY_CINFO_CTOR,
		SCAN_MATERIAL_CTOR,
		SCAN_SYSTEM_DATA_SITE,
		SCAN_ENABLE_PHYSICS,
		SCAN_QUEUE_LOAD,
		SCAN_QUEUE_ENABLE,
		SCAN_QUEUE_DISABLE,
		SCAN_QUEUE_UNLOAD,
		SCAN_ZONE_MANAGER,
		SCAN_FRAME_SITE,
		SCAN_UPDATE_FIRST,
		SCAN_ROUTER_SITE,
		SCAN_COUNT,
	};

	struct Scan {
		const hex_signature *signature;
		size_t count = 0;
		intptr_t address = 0;
	};

	// in the order of the enum above
	static Scan g_scans[SCAN_COUNT] = {
		{ &HKNP_MESH_CINFO_SITE_SIGNATURE },
		{ &HKNP_MESH_CINFO_CTOR_SIGNATURE },
		{ &HKNP_MESH_SHAPE_CTOR_SIGNATURE },
		{ &HK_REMOVE_REFERENCE_SIGNATURE },
		{ &HKNP_BODY_CINFO_CTOR_SIGNATURE },
		{ &HKNP_MATERIAL_CTOR_SIGNATURE },
		{ &HKNP_SYSTEM_DATA_VTABLE_SITE_SIGNATURE },
		{ &ZONE_ENABLE_PHYSICS_SIGNATURE },
		{ &PHYSICS_DELAY_LOAD_ZONE_SIGNATURE },
		{ &PHYSICS_DELAY_ENABLE_ZONE_SIGNATURE },
		{ &PHYSICS_DELAY_DISABLE_ZONE_SIGNATURE },
		{ &PHYSICS_DELAY_UNLOAD_ZONE_SIGNATURE },
		{ &ZONE_MANAGER_SITE_SIGNATURE },
		{ &PHYSICS_FRAME_SITE_SIGNATURE },
		{ &PHYSICS_UPDATE_FIRST_SIGNATURE },
		{ &HAVOK_ROUTER_SITE_SIGNATURE },
	};

	// two ways to the same address that have to agree, or a vtable that has to
	// belong to the class it is taken for
	struct Check {
		const char *name;
		bool passed;
		char found[0x40];
	};

	constexpr int32_t MAX_CHECKS = 16;
	static Check g_checks[MAX_CHECKS];
	static int32_t g_check_count = 0;

	// --------------------------------------------------------------- state --

	static int32_t g_stage = 0;
	static bool g_resolved = false;
	// the exe is the one the layouts were measured on
	static bool g_measured = false;
	// why nothing past status works, or empty when the engine calls are there
	static char g_unavailable[0x200] = "the engine calls have not been looked for, the stage has been 0";
	// set by a fault in an engine call, for the rest of the session
	static bool g_latched = false;
	static char g_last_error[0x180] {};
	static char g_refusal[0x180] {};

	// what the engine reads of ours when it makes a body, and reads again when
	// it lets go of it. when its teardown, which can run on a job thread, is
	// done with them has not been measured, so a set is handed out once and
	// kept until the game exits. the zone is not a zone: it is the zeroed
	// stand-in the engine hangs the body's system on
	struct alignas(16) Buffers {
		uint8_t material[MATERIAL_SIZE];
		// read with aligned loads, as is everything else here that holds vectors
		uint8_t cinfo[CINFO_SIZE];
		// rounded up, so that what follows stays aligned
		uint8_t data[DATA_SIZE + 8];
		uint8_t zone[ZONE_SIZE];
	};

	static_assert(offsetof(Buffers, cinfo) % 16 == 0 && offsetof(Buffers, data) % 16 == 0 && offsetof(Buffers, zone) % 16 == 0 && sizeof(Buffers) % 16 == 0, "Buffers members are not 16 aligned");

	enum class Phase : uint8_t {
		// in the engine's load queue, or about to be worked off it
		Queued,
		// the engine made the system and the body
		Built,
		// asked to go: on its way out of the world, the system still there
		Removing,
		// the system is in the queue that lets go of it
		Destroying,
		Gone,
		// its shape's zone went first. out of the world, and left at that
		Orphaned,
		// the engine made something else of it than a body. left alone
		Broken,
		// the world it was in is not the engine's world any more, or its
		// system not the one the engine made. dropped with no call on it
		Lost,
	};

	struct Body {
		int32_t id = 0;
		Phase phase = Phase::Queued;
		Buffers *buffers = nullptr;
		const uint8_t *world = nullptr;
		// the hknpPhysicsSystem the engine made, and its one body's id
		uint8_t *system = nullptr;
		uint32_t body = BODY_NONE;
		const uint8_t *shape = nullptr;
		// the cooked shape it is made of, 0 for a borrowed one
		int32_t cooked = 0;
		// the zone a borrowed shape belongs to, and what its slot held then
		uint64_t donor = 0;
		const uint8_t *donor_zone = nullptr;
		const uint8_t *donor_asset = nullptr;
		const uint8_t *donor_system = nullptr;
		uint32_t filter = 0;
		float position[3] {};
		float yaw = 0.0f;
		// in the world, as last asked for
		bool wanted = true;
		bool was_live = false;
		const char *error = nullptr;
	};

	enum class ShapeState : uint8_t {
		Usable,
		// it failed a check, and no body is made of it
		Rejected,
		Released,
		// dropped with its world, or with a body of it. never freed
		Lost,
	};

	struct Shape {
		int32_t id = 0;
		ShapeState state = ShapeState::Usable;
		uint8_t *shape = nullptr;
		const uint8_t *world = nullptr;
		// a mesh shape holding the hook's one reference when it was cooked,
		// which is what giving that reference up takes
		bool counted = false;
		uint32_t flags = 0;
		int32_t vertices = 0;
		int32_t triangles = 0;
		int32_t cooked = 0;
		// bodies of the hook's made of it and not gone yet
		int32_t users = 0;
		float low[3] {};
		float high[3] {};
		double cook_ms = 0.0;
		char source[0x100] {};
	};

	// ids are an index plus one, and are never handed out twice in a session
	static Buffers g_buffers[MAX_BODIES];
	static Body g_bodies[MAX_BODIES];
	static int32_t g_body_count = 0;
	static Shape g_shapes[MAX_SHAPES];
	static int32_t g_shape_count = 0;

	// ------------------------------------------------------------- plumbing --

	static auto
	fail(const char **reason, const char *why) -> bool {
		if (reason != nullptr) {
			*reason = why;
		}

		return false;
	}

	// a refusal with a number in it. the text is good until the next one
	static auto
	failf(const char **reason, const char *format, ...) -> bool {
		va_list args; // NOLINT(*-init-variables)
		va_start(args, format);
		_vsnprintf_s(g_refusal, sizeof(g_refusal), _TRUNCATE, format, args);
		va_end(args);
		return fail(reason, g_refusal);
	}

	// switches the feature off for the rest of the session: an engine call did
	// something it should not have, and what the engine was left with is not
	// known, so nothing of the engine's is called again
	static auto
	latch(const char *what, const char **reason) -> bool {
		g_latched = true;
		_snprintf_s(g_last_error, sizeof(g_last_error), _TRUNCATE, "%s, collision is off until the game restarts", what);
		g_output << "[collision] " << g_last_error << "\n";
		g_output.flush();
		return fail(reason, g_last_error);
	}

	// a fault in an engine call. it was caught, which undoes nothing
	static auto
	faulted(const char *call, const char **reason) -> bool {
		char what[0x80];
		_snprintf_s(what, sizeof(what), _TRUNCATE, "%s faulted", call);
		return latch(what, reason);
	}

	// a queue call that left more than one entry, or another one than it was
	// asked for, is not the call it was taken for
	static auto
	misqueued(const char *call, const char **reason) -> bool {
		char what[0x80];
		_snprintf_s(what, sizeof(what), _TRUNCATE, "%s queued something else than it was asked to", call);
		return latch(what, reason);
	}

	// the line that goes into the log ahead of every engine call, flushed, so
	// that a crash names the step it happened in
	static auto
	announce(const char *call, const char *of = nullptr, const int32_t id = 0) -> void {
		g_output << "[collision] calling " << call;
		if (of != nullptr) {
			g_output << " for " << of << " " << id;
		}

		g_output << "\n";
		g_output.flush();
	}

	static auto
	in_game(const void *at, const size_t size = 1) -> bool {
		const auto address = reinterpret_cast<uintptr_t>(at);
		return g_image != 0 && address >= g_image && address + size <= g_image + g_image_size;
	}

	// whether an address is in a section of the game the loader maps executable
	static auto
	in_code(const void *at) -> bool {
		if (!in_game(at)) {
			return false;
		}

		const auto *dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(g_image);
		const auto *headers = reinterpret_cast<const IMAGE_NT_HEADERS *>(g_image + dos->e_lfanew);
		const auto *section = IMAGE_FIRST_SECTION(headers);
		const auto rva = reinterpret_cast<uintptr_t>(at) - g_image;
		for (auto i = 0; i < headers->FileHeader.NumberOfSections; ++i, ++section) {
			if ((section->Characteristics & IMAGE_SCN_MEM_EXECUTE) != 0 && rva >= section->VirtualAddress && rva < static_cast<uintptr_t>(section->VirtualAddress) + section->Misc.VirtualSize) {
				return true;
			}
		}

		return false;
	}

	// one read of memory that is not the hook's. what a pointer of the engine's
	// leads to can go between the check and the copy, so the copy is guarded too
	static auto
	read_raw(const void *at, void *out, const size_t size) -> bool {
		if (at == nullptr || !ddl::is_readable(at, size)) {
			return false;
		}

#ifdef _MSC_VER
		__try {
#endif
			memcpy(out, at, size);
			return true;
#ifdef _MSC_VER
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			return false;
		}
#endif
	}

	template <typename T>
	static auto
	read(const void *base, const size_t offset, T &out) -> bool {
		memset(&out, 0, sizeof(T));
		return base != nullptr && read_raw(static_cast<const uint8_t *>(base) + offset, &out, sizeof(T));
	}

	// the same for buffers of the hook's own
	template <typename T>
	static auto
	get(const uint8_t *buffer, const size_t offset) -> T {
		T value;
		memcpy(&value, buffer + offset, sizeof(T));
		return value;
	}

	template <typename T>
	static auto
	put(uint8_t *buffer, const size_t offset, const T value) -> void {
		memcpy(buffer + offset, &value, sizeof(T));
	}

	// the class name msvc's rtti keeps behind a vtable: the locator names its
	// type descriptor by rva, and itself, which is how it is told from noise
	static auto
	class_behind(const void *vtable, char *out, const size_t size) -> bool {
		out[0] = '\0';
		const uint8_t *locator = nullptr;
		ObjectLocator fields {};
		if (!in_game(vtable, sizeof(void *)) || !read(static_cast<const uint8_t *>(vtable) - sizeof(void *), 0, locator) || !in_game(locator, sizeof(fields)) || !read(locator, 0, fields)) {
			return false;
		}

		if (fields.signature != 1 || fields.self != reinterpret_cast<uintptr_t>(locator) - g_image || fields.typeDescriptor >= g_image_size - TYPE_DESCRIPTOR_NAME) {
			return false;
		}

		return ddl::read_string(reinterpret_cast<const char *>(g_image + fields.typeDescriptor + TYPE_DESCRIPTOR_NAME), out, size) && out[0] != '\0';
	}

	static auto
	class_of(const void *object, char *out, const size_t size) -> bool {
		out[0] = '\0';
		const void *vtable = nullptr;
		return read(object, 0, vtable) && class_behind(vtable, out, size);
	}

	static auto
	is_class(const void *object, const char *name) -> bool {
		char found[0x60];
		return class_of(object, found, sizeof(found)) && strcmp(found, name) == 0;
	}

	// ------------------------------------------------------- engine calls --

	// each engine call on its own, in a frame with nothing to unwind, so a
	// fault is caught. catching it does not undo what the call did before it
	// faulted: the caller latches the feature off

	static auto
	call_construct(const construct_t constructor, void *object, void **made) -> bool {
#ifdef _MSC_VER
		__try {
#endif
			*made = constructor(object);
			return true;
#ifdef _MSC_VER
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			return false;
		}
#endif
	}

	static auto
	call_cook(void *memory, const void *cinfo, void **made) -> bool {
#ifdef _MSC_VER
		__try {
#endif
			*made = g_mesh_shape_ctor(memory, cinfo);
			return true;
#ifdef _MSC_VER
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			return false;
		}
#endif
	}

	static auto
	call_release(const void *object) -> bool {
#ifdef _MSC_VER
		__try {
#endif
			g_remove_reference(object);
			return true;
#ifdef _MSC_VER
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			return false;
		}
#endif
	}

	static auto
	call_queue(const queue_t queue, void *first, void *second) -> bool {
#ifdef _MSC_VER
		__try {
#endif
			queue(g_physics, first, second);
			return true;
#ifdef _MSC_VER
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			return false;
		}
#endif
	}

	static auto
	call_allocate(const block_alloc_t allocate, void *heap, const int32_t size, void **memory) -> bool {
#ifdef _MSC_VER
		__try {
#endif
			*memory = allocate(heap, size);
			return true;
#ifdef _MSC_VER
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			return false;
		}
#endif
	}

	static auto
	call_free(const block_free_t release, void *heap, void *memory, const int32_t size) -> bool {
#ifdef _MSC_VER
		__try {
#endif
			release(heap, memory, size);
			return true;
#ifdef _MSC_VER
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			return false;
		}
#endif
	}

	// ------------------------------------------------------------ resolving --

	static auto
	matches(const intptr_t address, const hex_signature &signature) -> bool {
		const auto *code = reinterpret_cast<const uint8_t *>(address);
		if (!in_game(code, signature.size)) {
			return false;
		}

		for (uint32_t i = 0; i < signature.size; ++i) {
			if (!(signature.signature[i] == code[i])) {
				return false;
			}
		}

		return true;
	}

	static auto
	check(const char *name, const bool passed, const char *found = "") -> bool {
		if (g_check_count < MAX_CHECKS) {
			auto &entry = g_checks[g_check_count++];
			entry.name = name;
			entry.passed = passed;
			strncpy_s(entry.found, found, _TRUNCATE);
		}

		return passed;
	}

	static auto
	check_class(const char *name, const void *vtable, const char *expected) -> bool {
		char found[0x40];
		class_behind(vtable, found, sizeof(found));
		return check(name, strcmp(found, expected) == 0, found);
	}

	// finds the engine calls and everything read along with them, once. any
	// miss, any signature that matches more or less than once, and any two
	// sources that disagree leave the whole feature off
	static auto
	resolve() -> void {
		if (g_resolved) {
			return;
		}

		g_resolved = true;
		g_image = reinterpret_cast<uintptr_t>(g_game_module);
		const auto *dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(g_image);
		if (g_image == 0 || dos->e_magic != IMAGE_DOS_SIGNATURE) {
			g_image = 0;
			strncpy_s(g_unavailable, "the game's image could not be read", _TRUNCATE);
			g_output << "[collision] off: " << g_unavailable << "\n";
			g_output.flush();
			return;
		}

		const auto *headers = reinterpret_cast<const IMAGE_NT_HEADERS *>(g_image + dos->e_lfanew);
		g_image_size = headers->OptionalHeader.SizeOfImage;
		g_timestamp = headers->FileHeader.TimeDateStamp;
		g_measured = g_timestamp == MEASURED_TIMESTAMP && g_image_size == MEASURED_IMAGE_SIZE;

		std::string missed;
		for (auto &scan : g_scans) {
			const auto found = find_addresses(*scan.signature);
			scan.count = found.size();
			// the addresses are cached across launches, so the one match is
			// looked at once more where it is now
			if (scan.count == 1 && matches(found[0], *scan.signature)) {
				scan.address = found[0];
				continue;
			}

			missed += missed.empty() ? "" : ", ";
			missed += std::string(scan.signature->name) + " (" + std::to_string(scan.count) + ")";
		}

		if (!missed.empty()) {
			_snprintf_s(g_unavailable, sizeof(g_unavailable), _TRUNCATE, "these signatures did not match exactly once: %s", missed.c_str());
			g_output << "[collision] off: " << g_unavailable << "\n";
			g_output.flush();
			return;
		}

		const auto site = g_scans[SCAN_MESH_CINFO_SITE].address;
		const auto enable = g_scans[SCAN_ENABLE_PHYSICS].address;
		const auto frame = g_scans[SCAN_FRAME_SITE].address;
		const auto step = g_scans[SCAN_UPDATE_FIRST].address;
		const auto router = g_scans[SCAN_ROUTER_SITE].address;
		const auto zones = g_scans[SCAN_ZONE_MANAGER].address;

		g_mesh_cinfo_ctor = reinterpret_cast<construct_t>(g_scans[SCAN_MESH_CINFO_CTOR].address);
		g_mesh_shape_ctor = reinterpret_cast<cook_t>(g_scans[SCAN_MESH_SHAPE_CTOR].address);
		g_remove_reference = reinterpret_cast<release_t>(g_scans[SCAN_REMOVE_REFERENCE].address);
		g_body_cinfo_ctor = reinterpret_cast<construct_t>(g_scans[SCAN_BODY_CINFO_CTOR].address);
		g_material_ctor = reinterpret_cast<construct_t>(g_scans[SCAN_MATERIAL_CTOR].address);
		g_delay_load = reinterpret_cast<queue_t>(g_scans[SCAN_QUEUE_LOAD].address);
		g_delay_enable = reinterpret_cast<queue_t>(g_scans[SCAN_QUEUE_ENABLE].address);
		g_delay_disable = reinterpret_cast<queue_t>(g_scans[SCAN_QUEUE_DISABLE].address);
		g_delay_unload = reinterpret_cast<queue_t>(g_scans[SCAN_QUEUE_UNLOAD].address);

		g_physics = static_cast<uint8_t *>(load_rel_var(enable, ZONE_ENABLE_PHYSICS_SYSTEM_ADDRESS));
		g_zones = static_cast<const uint8_t *>(load_rel_var(zones, ZONE_MANAGER_ADDRESS));
		// both are cmp byte [rip + x], 0: the immediate follows the displacement
		g_enabled = reinterpret_cast<const uint8_t *>(frame + PHYSICS_FRAME_ENABLED_END + *reinterpret_cast<const int32_t *>(frame + PHYSICS_FRAME_ENABLED_ADDRESS));
		g_paused = reinterpret_cast<const uint8_t *>(step + PHYSICS_UPDATE_FIRST_PAUSED_END + *reinterpret_cast<const int32_t *>(step + PHYSICS_UPDATE_FIRST_PAUSED_ADDRESS));
		g_timestep = static_cast<const float *>(load_rel_var(step, PHYSICS_UPDATE_FIRST_TIMESTEP_ADDRESS));
		g_tls_index = static_cast<const uint32_t *>(load_rel_var(site, HKNP_MESH_CINFO_SITE_TLS_INDEX_ADDRESS));
		g_fallback_router = static_cast<void *const *>(load_rel_var(router, HAVOK_ROUTER_FALLBACK_ADDRESS));
		g_tls_get_value = static_cast<void *const *>(load_rel_var(router, HAVOK_ROUTER_TLS_GET_VALUE_ADDRESS));
		g_default_cinfo_vtable = load_rel_var(site, HKNP_MESH_CINFO_SITE_VTABLE_ADDRESS);
		g_base_cinfo_vtable = load_rel_var(g_scans[SCAN_MESH_CINFO_CTOR].address, HKNP_MESH_CINFO_CTOR_VTABLE_ADDRESS);
		g_mesh_vtable = load_rel_var(g_scans[SCAN_MESH_SHAPE_CTOR].address, HKNP_MESH_SHAPE_CTOR_VTABLE_ADDRESS);
		g_material_vtable = load_rel_var(g_scans[SCAN_MATERIAL_CTOR].address, HKNP_MATERIAL_CTOR_VTABLE_ADDRESS);
		g_system_data_vtable = load_rel_var(g_scans[SCAN_SYSTEM_DATA_SITE].address, HKNP_SYSTEM_DATA_VTABLE_ADDRESS);

		const auto *by_index = static_cast<const uint8_t *>(load_rel_var(zones, ZONE_MANAGER_BY_INDEX_ADDRESS));
		const auto inside = in_game(g_physics, PHYSICS_SIZE) && in_game(g_zones, ZONE_MANAGER_SIZE) && in_game(g_enabled) && in_game(g_paused) && in_game(g_timestep, sizeof(float))
			&& in_game(g_tls_index, sizeof(uint32_t)) && in_game(g_fallback_router, sizeof(void *)) && in_game(g_tls_get_value, sizeof(void *));
		auto passed = true;
		passed &= check("the physics system, by the zone and by the frame", g_physics == load_rel_var(frame, PHYSICS_FRAME_SYSTEM_ADDRESS));
		passed &= check("Havok's tls index, by the cook and by the engine", g_tls_index == load_rel_var(router, HAVOK_ROUTER_TLS_INDEX_ADDRESS));
		passed &= check("the enable queue, by the zone and by its signature", load_rel_var(enable, ZONE_ENABLE_PHYSICS_DELAY_ENABLE_ADDRESS) == reinterpret_cast<void *>(g_delay_enable));
		passed &= check("the load queue, by the zone and by its signature", load_rel_var(enable, ZONE_ENABLE_PHYSICS_DELAY_LOAD_ADDRESS) == reinterpret_cast<void *>(g_delay_load));
		passed &= check("the mesh cinfo constructor, by the cook and by its signature", load_rel_var(site, HKNP_MESH_CINFO_SITE_CTOR_ADDRESS) == reinterpret_cast<void *>(g_mesh_cinfo_ctor));
		passed &= check("the zone table's layout", in_game(by_index, sizeof(ZONE_BY_INDEX_CODE)) && memcmp(by_index, ZONE_BY_INDEX_CODE, sizeof(ZONE_BY_INDEX_CODE)) == 0);
		passed &= check("the globals, inside the game's image", inside);
		passed &= check_class("the default mesh cinfo's vtable", g_default_cinfo_vtable, DEFAULT_MESH_CINFO_CLASS);
		passed &= check_class("the mesh cinfo's vtable", g_base_cinfo_vtable, MESH_CINFO_CLASS);
		passed &= check_class("the mesh shape's vtable", g_mesh_vtable, MESH_CLASS);
		passed &= check_class("the material's vtable", g_material_vtable, MATERIAL_CLASS);
		passed &= check_class("the system data's vtable", g_system_data_vtable, SYSTEM_DATA_CLASS);

		if (!passed) {
			std::string failed;
			for (auto i = 0; i < g_check_count; ++i) {
				if (!g_checks[i].passed) {
					failed += failed.empty() ? "" : "; ";
					failed += g_checks[i].name;
				}
			}

			_snprintf_s(g_unavailable, sizeof(g_unavailable), _TRUNCATE, "the signatures matched, but these did not hold: %s", failed.c_str());
			g_output << "[collision] off: " << g_unavailable << "\n";
			g_output.flush();
			return;
		}

		g_unavailable[0] = '\0';
		g_output << "[collision] engine calls resolved, physics system at " << static_cast<void *>(g_physics) << ", mesh cook at " << reinterpret_cast<void *>(g_mesh_shape_ctor);
		if (!g_measured) {
			g_output << ". this exe is another build than the one the layouts were measured on, so only stage 1 works";
		}

		g_output << "\n";
		g_output.flush();
	}

	// why nothing past stage 1, which only reads, is to be had, or null when the
	// stages that call the engine are: they take the calls found, the exe the
	// layouts were measured on, and a build of the hook that catches a fault
	static auto
	reads_only() -> const char * {
#ifndef _MSC_VER
		// the guards around the engine calls are msvc's structured exceptions.
		// without them a fault is a crash, and nothing switches the feature off
		return "this build of the hook cannot catch a fault in an engine call, so it only has stage 1, which reads: the msvc build has the others";
#else
		if (g_unavailable[0] != '\0') {
			return g_unavailable;
		}

		return g_measured ? nullptr : "this exe is another build than the one the layouts were measured on: only stage 1, which reads, works on it";
#endif
	}

	auto
	init() -> void {
		g_stage = g_settings.collision.stage;
		if (g_stage > MAX_STAGE) {
			g_output << "[collision] stage " << g_stage << " is more than there is, taken as " << MAX_STAGE << "\n";
			g_stage = MAX_STAGE;
		}

		if (g_stage <= 0) {
			g_stage = 0;
			return;
		}

		resolve();
		// the limit set_stage has: status is there to read why, nothing else is
		if (const auto *why = reads_only(); g_stage > 1 && why != nullptr) {
			g_output << "[collision] stage " << g_stage << " is not to be had, taken as 1: " << why << "\n";
			g_stage = 1;
		}

		g_output << "[collision] stage " << g_stage << ", experimental\n";
		g_output.flush();
	}

	auto
	stage_info() -> nlohmann::json {
		nlohmann::json out;
		out["stage"] = g_stage;
		out["max_stage"] = MAX_STAGE;
		out["available"] = g_unavailable[0] == '\0';
		out["reason"] = g_unavailable[0] != '\0' ? nlohmann::json(g_unavailable) : nlohmann::json();
		out["measured_build"] = g_resolved ? nlohmann::json(g_measured) : nlohmann::json();
#ifdef _MSC_VER
		out["faults_caught"] = true;
#else
		out["faults_caught"] = false;
#endif
		out["latched"] = g_latched;
		out["last_error"] = g_last_error[0] != '\0' ? nlohmann::json(g_last_error) : nlohmann::json();
		return out;
	}

	// whether the pump still has something to do for a body, or the engine
	// something of its to work off: on its way in or out, in the world or
	// switched out of it, or waiting to be taken out after its zone went
	static auto
	followed(const Body &body) -> bool {
		switch (body.phase) {
			case Phase::Queued:
			case Phase::Built:
			case Phase::Removing:
			case Phase::Destroying: return true;
			case Phase::Orphaned: return body.wanted;
			default: return false;
		}
	}

	auto
	set_stage(const int32_t stage, const char **reason) -> bool {
		if (stage < 0 || stage > MAX_STAGE) {
			return failf(reason, "the stage is 0 to %d", MAX_STAGE);
		}

		if (stage > g_stage && g_latched) {
			return fail(reason, g_last_error);
		}

		if (stage > 0) {
			resolve();
		}

		// status is for reading why, so stage 1 is there whatever was found
		if (const auto *why = reads_only(); stage > 1 && why != nullptr) {
			return fail(reason, why);
		}

		// below stage 3 nothing follows a body: a removal would stop halfway,
		// and a borrowed body's zone would go unwatched. once latched nothing
		// follows them at any stage
		for (auto i = 0; stage < 3 && g_stage >= 3 && !g_latched && i < g_body_count; ++i) {
			if (followed(g_bodies[i])) {
				return fail(reason, "collision bodies are still there, and below stage 3 nothing follows them: remove them first (collision.remove all) and wait for them to be gone");
			}
		}

		if (stage != g_stage) {
			g_output << "[collision] stage " << g_stage << " -> " << stage << " for this session\n";
			g_output.flush();
			g_stage = stage;
		}

		return true;
	}

	// refuses a command above the stage, naming the stage it needs
	static auto
	staged(const int32_t stage, const char **reason) -> bool {
		if (g_stage >= stage) {
			return true;
		}

		return failf(reason, "that needs collision stage %d and the stage is %d: set [collision] stage in rivet.toml, or collision.stage %d for this session", stage, g_stage, stage);
	}

	static auto
	gate(const int32_t stage, const char **reason) -> bool {
		if (!staged(stage, reason)) {
			return false;
		}

		if (g_unavailable[0] != '\0') {
			return fail(reason, g_unavailable);
		}

		if (g_latched) {
			return fail(reason, g_last_error);
		}

		if (const auto *why = reads_only(); stage > 1 && why != nullptr) {
			return fail(reason, why);
		}

		return true;
	}

	// -------------------------------------------------------- preconditions --

	static auto
	current_world() -> const uint8_t * {
		const uint8_t *world = nullptr;
		read(g_physics, PHYSICS_WORLD, world);
		return world;
	}

	static auto
	active(const Body &body) -> bool {
		return body.phase != Phase::Gone && body.phase != Phase::Lost;
	}

	static auto
	held(const Shape &shape) -> bool {
		return shape.state == ShapeState::Usable || shape.state == ShapeState::Rejected;
	}

	// every handle is of the world it was made in. how long a world lives has
	// not been traced, so the engine's is read before each use, and a handle of
	// another world is dropped as it is, with no call made on it
	static auto
	watch_world(const uint8_t *world) -> void {
		auto dropped = 0;
		for (auto i = 0; i < g_body_count; ++i) {
			if (active(g_bodies[i]) && g_bodies[i].world != world) {
				g_bodies[i].phase = Phase::Lost;
				++dropped;
			}
		}

		for (auto i = 0; i < g_shape_count; ++i) {
			if (held(g_shapes[i]) && g_shapes[i].world != world) {
				g_shapes[i].state = ShapeState::Lost;
				++dropped;
			}
		}

		if (dropped > 0) {
			_snprintf_s(g_last_error, sizeof(g_last_error), _TRUNCATE, "the physics world changed, %d bodies and shapes of the old one were dropped without a call", dropped);
			g_output << "[collision] " << g_last_error << "\n";
			g_output.flush();
		}
	}

	struct Havok {
		const uint8_t *world = nullptr;
		uint8_t *router = nullptr;
		uint8_t *heap = nullptr;
		block_alloc_t allocate = nullptr;
		block_free_t release = nullptr;
	};

	// the calling thread's Havok memory router and the heap behind it. a thread
	// Havok never set up has none, and the engine's fallback for that is never
	// set, so an allocation there would go through null
	static auto
	thread_heap(Havok &havok, const char **reason) -> bool {
		const auto index = *g_tls_index;
		havok.router = index < MAX_TLS_INDEX ? static_cast<uint8_t *>(TlsGetValue(index)) : nullptr;
		if (havok.router == nullptr) {
			return fail(reason, "this thread has no Havok memory router");
		}

		void *const *vtable = nullptr;
		if (!read(havok.router, ROUTER_HEAP, havok.heap) || !read(havok.heap, 0, vtable) || !in_game(vtable, sizeof(void *) * (HEAP_BLOCK_FREE + 1))) {
			return fail(reason, "this thread's Havok memory router does not lead to a heap of the game's");
		}

		havok.allocate = reinterpret_cast<block_alloc_t>(vtable[HEAP_BLOCK_ALLOC]);
		havok.release = reinterpret_cast<block_free_t>(vtable[HEAP_BLOCK_FREE]);
		if (!in_code(vtable[HEAP_BLOCK_ALLOC]) || !in_code(vtable[HEAP_BLOCK_FREE])) {
			return fail(reason, "the Havok heap's block calls are not code of the game's");
		}

		return true;
	}

	// what has to hold before any engine call, checked again at every one: the
	// stage, the game thread, a world that is an hknpWorld, and a router. for a
	// queue call also physics switched on, or the queue is never worked off, and
	// the callback the engine calls unchecked for every body it loads
	static auto
	ready(const int32_t stage, const bool queue, Havok &havok, const char **reason) -> bool {
		if (!gate(stage, reason)) {
			return false;
		}

		// the pump moves to the render thread while actor updates are stopped
		if (!game_thread::on_game_thread()) {
			return fail(reason, "collision only works on the game thread, and it is not pumping (loading?)");
		}

		havok.world = current_world();
		watch_world(havok.world);
		if (havok.world == nullptr) {
			return fail(reason, "there is no physics world right now");
		}

		if (!is_class(havok.world, WORLD_CLASS)) {
			return fail(reason, "the physics world is not an hknpWorld");
		}

		if (!thread_heap(havok, reason)) {
			return false;
		}

		if (!queue) {
			return true;
		}

		if (*g_enabled == 0) {
			return fail(reason, "physics is switched off, the engine would never work the queue off");
		}

		const void *callback = nullptr;
		if (!read(g_physics, PHYSICS_CALLBACK, callback) || callback == nullptr) {
			return fail(reason, "the engine has no collision callback set, and calls it for every body it loads");
		}

		return true;
	}

	// the last thing before an engine call: every precondition once more, and
	// then the line in the log
	static auto
	about_to(const int32_t stage, const bool queue, Havok &havok, const char **reason, const char *call, const char *of = nullptr, const int32_t id = 0) -> bool {
		if (!ready(stage, queue, havok, reason)) {
			return false;
		}

		announce(call, of, id);
		return true;
	}

	struct Queue {
		const uint8_t *entries = nullptr;
		int32_t count = 0;
		int32_t capacity = 0;
	};

	static auto
	read_queue(const bool unload, Queue &queue) -> bool {
		return read(g_physics, unload ? PHYSICS_UNLOAD_QUEUE : PHYSICS_LOAD_QUEUE, queue.entries) && read(g_physics, unload ? PHYSICS_UNLOAD_COUNT : PHYSICS_LOAD_COUNT, queue.count)
			&& read(g_physics, PHYSICS_QUEUE_CAPACITY, queue.capacity) && queue.capacity > 0 && queue.capacity <= MAX_QUEUE_CAPACITY && queue.count >= 0 && queue.count <= queue.capacity
			&& ddl::is_readable(queue.entries, static_cast<size_t>(queue.capacity) * QUEUE_ENTRY_SIZE);
	}

	// a queue with room for one more. a full one drops the request silently
	static auto
	queue_room(const bool unload, Queue &queue, const char **reason) -> bool {
		if (!read_queue(unload, queue)) {
			return fail(reason, "the engine's physics queue is not readable");
		}

		return queue.count < queue.capacity || fail(reason, "the engine's physics queue is full, try again in a frame");
	}

	// whether the queue grew by exactly the entry that was asked for
	static auto
	queue_took(const bool unload, const Queue &before, const void *first, const void *second, const uint8_t flag) -> bool {
		Queue after;
		const void *got_first = nullptr;
		const void *got_second = nullptr;
		uint8_t got_flag = 0;
		const auto *entry = before.entries + static_cast<size_t>(before.count) * QUEUE_ENTRY_SIZE;
		return read_queue(unload, after) && after.entries == before.entries && after.count == before.count + 1 && read(entry, 0, got_first) && read(entry, QUEUE_ENTRY_SECOND, got_second)
			&& read(entry, QUEUE_ENTRY_FLAG, got_flag) && got_first == first && got_second == second && got_flag == flag;
	}

	// ---------------------------------------------------------------- json --

	static auto
	hex(const uint64_t value) -> std::string {
		char out[24];
		_snprintf_s(out, sizeof(out), _TRUNCATE, "0x%llx", value);
		return out;
	}

	// addresses and ids as hex text: they are compared by eye, and the wide
	// ones run past what a json number survives in most readers
	static auto
	pointer(const void *at) -> nlohmann::json {
		return at != nullptr ? nlohmann::json(hex(reinterpret_cast<uintptr_t>(at))) : nlohmann::json();
	}

	static auto
	floats(const float *values, const int32_t count) -> nlohmann::json {
		nlohmann::json::array_t out;
		for (auto i = 0; i < count; ++i) {
			out.emplace_back(values[i]);
		}

		return out;
	}

	static auto
	class_json(const void *object) -> nlohmann::json {
		char name[0x60];
		return class_of(object, name, sizeof(name)) ? nlohmann::json(name) : nlohmann::json();
	}

	// ---------------------------------------------------------------- zones --

	struct ZoneRef {
		const uint8_t *zone = nullptr;
		const uint8_t *asset = nullptr;
		int32_t index = -1;
		Asset header {};
	};

	// the zone of a zone asset id, read out of the zone manager: zone i is made
	// of slot i of the manager's asset pool. the engine has a lookup for this,
	// which would be a call
	static auto
	find_zone(const uint64_t id, ZoneRef &out, const char **reason) -> bool {
		const uint8_t *assets = nullptr;
		const uint8_t *zones = nullptr;
		uint64_t stride = 0;
		int32_t last = -1;
		if (id == 0 || !read(g_zones, ZONE_MANAGER_ASSETS, assets) || !read(g_zones, ZONE_MANAGER_ASSET_SIZE, stride) || !read(g_zones, ZONE_MANAGER_LAST, last) || !read(g_zones, ZONE_MANAGER_ZONES, zones)
			|| assets == nullptr || zones == nullptr || stride < sizeof(Asset) || stride > 0x10000 || last < 0 || last >= MAX_ZONES) {
			return fail(reason, id == 0 ? "that is not a zone asset id" : "the zone manager has no zones right now");
		}

		// a slot can keep the id of an asset that is gone, so one that no zone
		// is made of is passed over
		auto known = false;
		for (int32_t index = 0; index <= last; ++index) {
			const auto *asset = assets + static_cast<size_t>(index) * stride;
			const auto *zone = zones + static_cast<size_t>(index) * ZONE_SIZE;
			const uint8_t *made_of = nullptr;
			if (!read(asset, 0, out.header) || out.header.assetId != id) {
				continue;
			}

			known = true;
			if (read(zone, ZONE_ASSET, made_of) && made_of == asset) {
				out.asset = asset;
				out.index = index;
				out.zone = zone;
				return true;
			}
		}

		return fail(reason, known ? "the zone manager knows that zone asset, but has no zone made of it right now" : "no zone with that asset id is loaded");
	}

	static auto
	find_zone(const char *text, ZoneRef &out, uint64_t &id, char *path, const size_t path_size, const char **reason) -> bool {
		if (text == nullptr || text[0] == '\0') {
			return fail(reason, "name a zone: its path, its asset id as 16 hex digits, or a part of the path only it has");
		}

		if (!travel::zone_id(text, id, path, path_size, reason) || !find_zone(id, out, reason)) {
			return false;
		}

		// a zone the level does not list still has the name of its asset
		if (path[0] == '\0') {
			ddl::read_string(out.header.name, path, path_size);
		}

		return true;
	}

	// ---------------------------------------------------------------- status --

	auto
	status(nlohmann::json &out, const char **reason) -> bool {
		out = stage_info();
		if (!staged(1, reason)) {
			return false;
		}

		out["max_triangles"] = g_settings.collision.max_triangles;

		nlohmann::json::array_t signatures;
		for (const auto &scan : g_scans) {
			nlohmann::json entry;
			entry["name"] = scan.signature->name;
			entry["matches"] = scan.count;
			entry["rva"] = scan.address != 0 ? nlohmann::json(hex(static_cast<uintptr_t>(scan.address) - g_image)) : nlohmann::json();
			signatures.emplace_back(std::move(entry));
		}

		nlohmann::json::array_t checks;
		for (auto i = 0; i < g_check_count; ++i) {
			nlohmann::json entry;
			entry["what"] = g_checks[i].name;
			entry["passed"] = g_checks[i].passed;
			if (g_checks[i].found[0] != '\0') {
				entry["class"] = g_checks[i].found;
			}

			checks.emplace_back(std::move(entry));
		}

		out["signatures"] = std::move(signatures);
		out["checks"] = std::move(checks);
		out["exe"] = { { "timestamp", hex(g_timestamp) }, { "image_size", hex(g_image_size) } };
		if (g_unavailable[0] != '\0') {
			return true;
		}

		const auto on_game_thread = game_thread::on_game_thread();
		nlohmann::json thread;
		thread["game_thread"] = on_game_thread;
		thread["tls_index"] = *g_tls_index;
		thread["fallback_router"] = pointer(*g_fallback_router);
		thread["tls_get_value_is_kernel32"] = *g_tls_get_value == reinterpret_cast<void *>(GetProcAddress(GetModuleHandleA("kernel32.dll"), "TlsGetValue"));

		// reads on any thread: the router of the thread this ran on is the
		// answer, and on the render thread it is expected to be missing
		Havok havok;
		const char *why = nullptr;
		const auto has_heap = thread_heap(havok, &why);
		thread["router"] = pointer(havok.router);
		thread["heap"] = has_heap ? pointer(havok.heap) : nlohmann::json();
		thread["heap_error"] = has_heap ? nlohmann::json() : nlohmann::json(why);
		out["thread"] = std::move(thread);

		const auto *world = current_world();
		int32_t slots = 0;
		read(world, WORLD_BODY_COUNT, slots);
		nlohmann::json about_world;
		about_world["address"] = pointer(world);
		about_world["class"] = class_json(world);
		about_world["is_world"] = world != nullptr && is_class(world, WORLD_CLASS);
		about_world["body_slots"] = slots;
		out["world"] = std::move(about_world);

		const void *callback = nullptr;
		read(g_physics, PHYSICS_CALLBACK, callback);
		out["callback"] = pointer(callback);
		out["callback_in_game"] = in_code(callback);
		out["physics_enabled"] = *g_enabled != 0;
		out["paused"] = *g_paused != 0 || !(*g_timestep > 0.0f);
		out["timestep"] = *g_timestep;

		Queue load;
		Queue unload;
		nlohmann::json queues;
		queues["load"] = read_queue(false, load) ? nlohmann::json(load.count) : nlohmann::json();
		queues["unload"] = read_queue(true, unload) ? nlohmann::json(unload.count) : nlohmann::json();
		queues["capacity"] = load.capacity;
		out["queues"] = std::move(queues);

		uint8_t holds = 0;
		out["body_holds_shape_reference"] = g_measured && read(reinterpret_cast<const void *>(g_image + BODY_HOLDS_SHAPE_RVA), 0, holds) ? nlohmann::json(holds) : nlohmann::json();

		auto body_count = 0;
		auto shape_count = 0;
		for (auto i = 0; i < g_body_count; ++i) {
			body_count += active(g_bodies[i]) ? 1 : 0;
		}

		for (auto i = 0; i < g_shape_count; ++i) {
			shape_count += held(g_shapes[i]) ? 1 : 0;
		}

		out["bodies"] = body_count;
		out["shapes"] = shape_count;
		return true;
	}

	// --------------------------------------------------------------- inspect --

	constexpr int32_t MAX_LISTED = 8;
	constexpr int32_t MAX_INSTANCES = 32;
	constexpr int32_t MAX_SHAPE_DEPTH = 3;

	// an hkArray's pointer and size, the size taken for nothing when it makes
	// no sense
	template <typename T>
	static auto
	read_array(const void *base, const size_t offset, const T *&items, int32_t &count) -> bool {
		if (!read(base, offset, items) || !read(base, offset + ARRAY_SIZE, count) || items == nullptr || count <= 0 || count > MAX_ZONES) {
			items = nullptr;
			count = 0;
			return false;
		}

		return true;
	}

	static auto
	describe_shape(const uint8_t *shape, std::vector<const uint8_t *> &described, int32_t depth) -> nlohmann::json;

	static auto
	describe_mesh(const uint8_t *shape, nlohmann::json &out) -> void {
		int32_t triangles = 0;
		int32_t convex = 0;
		const uint8_t *data = nullptr;
		read(shape, MESH_TRIANGLES, triangles);
		read(shape, MESH_CONVEX_SHAPES, convex);
		read(shape, MESH_DATA, data);
		out["triangles"] = triangles;
		out["convex_shapes"] = convex;
		out["data"] = pointer(data);
		out["data_class"] = class_json(data);

		float low[4];
		float high[4];
		if (read(data, MESH_DATA_AABB_MIN, low) && read(data, MESH_DATA_AABB_MAX, high)) {
			out["aabb"] = { { "min", floats(low, 3) }, { "max", floats(high, 3) } };
		}

		int32_t keys = 0;
		int32_t bits = 0;
		if (read(data, MESH_DATA_PRIMITIVE_KEYS, keys) && read(data, MESH_DATA_BITS_PER_KEY, bits)) {
			out["primitive_keys"] = keys;
			out["bits_per_key"] = bits;
		}

		// a primitive is four vertex indices, a quad or a triangle with one
		// repeated: the four bytes of a dword, listed in the order they are in
		// memory. a data run is a triangle tag over a run of primitives. how a
		// run splits into the three is read off the baked files and not off
		// code, so the dword is there as well
		const uint32_t *primitives = nullptr;
		const uint32_t *runs = nullptr;
		const uint8_t *nodes = nullptr;
		int32_t count = 0;
		if (read_array(data, MESH_DATA_PRIMITIVES, primitives, count)) {
			nlohmann::json::array_t listed;
			for (auto i = 0; i < count && i < MAX_LISTED; ++i) {
				uint8_t corners[4];
				if (read(primitives, static_cast<size_t>(i) * sizeof(uint32_t), corners)) {
					listed.emplace_back(nlohmann::json::array_t { corners[0], corners[1], corners[2], corners[3] });
				}
			}

			out["primitive_count"] = count;
			out["primitives"] = std::move(listed);
		}

		if (read_array(data, MESH_DATA_RUNS, runs, count)) {
			nlohmann::json::array_t listed;
			for (auto i = 0; i < count && i < MAX_LISTED; ++i) {
				uint32_t run = 0;
				if (read(runs, static_cast<size_t>(i) * sizeof(uint32_t), run)) {
					listed.emplace_back(nlohmann::json { { "raw", hex(run) }, { "tag", hex(run & 0xFFFF) }, { "first", (run >> 16) & 0xFF }, { "count", run >> 24 } });
				}
			}

			out["run_count"] = count;
			out["runs"] = std::move(listed);
		}

		if (read_array(data, MESH_DATA_NODES, nodes, count)) {
			out["simd_nodes"] = count;
		}
	}

	static auto
	describe_compound(const uint8_t *shape, std::vector<const uint8_t *> &described, const int32_t depth, nlohmann::json &out) -> void {
		out["bytes"] = ddl::hex_dump(shape, COMPOUND_SIZE);

		const uint8_t *instances = nullptr;
		int32_t count = 0;
		if (!read_array(shape, COMPOUND_INSTANCES, instances, count)) {
			return;
		}

		nlohmann::json::array_t listed;
		for (auto i = 0; i < count && i < MAX_INSTANCES; ++i) {
			const auto *instance = instances + static_cast<size_t>(i) * INSTANCE_SIZE;
			float columns[4][4];
			float scale[4];
			const uint8_t *child = nullptr;
			uint16_t tag = 0;
			if (!read(instance, 0, columns) || !read(instance, INSTANCE_SCALE, scale) || !read(instance, INSTANCE_SHAPE, child) || !read(instance, INSTANCE_TAG, tag)) {
				continue;
			}

			nlohmann::json entry;
			entry["index"] = i;
			entry["columns"] = { floats(columns[0], 3), floats(columns[1], 3), floats(columns[2], 3) };
			entry["translation"] = floats(columns[3], 3);
			entry["scale"] = floats(scale, 3);
			entry["tag"] = hex(tag);
			entry["shape"] = describe_shape(child, described, depth + 1);
			listed.emplace_back(std::move(entry));
		}

		out["instance_count"] = count;
		out["instances"] = std::move(listed);
	}

	// what a shape is. a mesh brings its tree's box and counts, a compound its
	// instances; a shape that turns up again is only named
	static auto
	describe_shape(const uint8_t *shape, std::vector<const uint8_t *> &described, const int32_t depth) -> nlohmann::json {
		nlohmann::json out;
		out["address"] = pointer(shape);
		ShapeHeader header {};
		if (!read(shape, 0, header)) {
			out["readable"] = false;
			return out;
		}

		if (std::find(described.begin(), described.end(), shape) != described.end()) {
			out["repeated"] = true;
			return out;
		}

		described.push_back(shape);
		out["class"] = class_json(shape);
		out["type"] = header.type;
		out["flags"] = header.flags;
		out["key_bits"] = header.keyBits;
		out["dispatch"] = header.dispatch;
		out["radius"] = header.radius;
		out["mem_size"] = hex(header.memSize);
		out["references"] = header.references;
		out["user_data"] = hex(header.userData);
		out["properties"] = pointer(header.properties);
		out["codec_info"] = hex(header.codecInfo);
		if (header.type == SHAPE_TYPE_MESH) {
			describe_mesh(shape, out);
		} else if (header.type == SHAPE_TYPE_COMPOUND && depth < MAX_SHAPE_DEPTH) {
			describe_compound(shape, described, depth, out);
		}

		return out;
	}

	static auto
	describe_cinfo(const uint8_t *cinfo) -> nlohmann::json {
		nlohmann::json out;
		const uint8_t *shape = nullptr;
		uint32_t flags = 0;
		uint32_t filter = 0;
		uint16_t material = 0;
		uint8_t quality = 0;
		uint64_t user_data = 0;
		uint8_t motion_type = 0;
		float position[4];
		float orientation[4];
		int32_t attached = 0;
		out["address"] = pointer(cinfo);
		if (!read(cinfo, CINFO_SHAPE, shape) || !read(cinfo, CINFO_FLAGS, flags) || !read(cinfo, CINFO_FILTER, filter) || !read(cinfo, CINFO_MATERIAL, material) || !read(cinfo, CINFO_QUALITY, quality)
			|| !read(cinfo, CINFO_USER_DATA, user_data) || !read(cinfo, CINFO_MOTION_TYPE, motion_type) || !read(cinfo, CINFO_POSITION, position) || !read(cinfo, CINFO_ORIENTATION, orientation)
			|| !read(cinfo, CINFO_ATTACHED, attached)) {
			out["readable"] = false;
			return out;
		}

		out["shape"] = pointer(shape);
		out["flags"] = hex(flags);
		out["filter"] = hex(filter);
		out["material"] = material;
		out["quality"] = quality;
		out["user_data"] = hex(user_data);
		out["motion_type"] = motion_type;
		out["position"] = floats(position, 3);
		out["orientation"] = floats(orientation, 4);
		out["attached"] = attached;
		return out;
	}

	// the slot an id names, whatever is in it
	static auto
	body_slot(const uint8_t *world, const uint32_t id) -> const uint8_t * {
		const auto index = id & BODY_INDEX_MASK;
		const uint8_t *bodies = nullptr;
		uint32_t slots = 0;
		if (index == BODY_INDEX_MASK || !read(world, WORLD_BODIES, bodies) || !read(world, WORLD_BODY_COUNT, slots) || bodies == nullptr || index >= slots) {
			return nullptr;
		}

		return bodies + static_cast<size_t>(index) * BODY_SIZE;
	}

	// the body an id names, or null once its slot holds another body or none
	static auto
	body_of(const uint8_t *world, const uint32_t id) -> const uint8_t * {
		const auto *body = body_slot(world, id);
		uint32_t carried = 0;
		uint32_t flags = 0;
		if (body == nullptr || !read(body, BODY_ID, carried) || !read(body, BODY_FLAGS, flags) || carried != id || (flags & BODY_FLAGS_IN_USE) == 0) {
			return nullptr;
		}

		return body;
	}

	static auto
	describe_body(const uint8_t *world, const uint32_t id) -> nlohmann::json {
		nlohmann::json out;
		const auto *body = body_slot(world, id);
		out["id"] = hex(id);
		out["address"] = pointer(body);
		out["valid"] = body_of(world, id) != nullptr;

		uint32_t flags = 0;
		uint32_t filter = 0;
		uint32_t broadphase = 0;
		uint32_t motion = 0;
		uint16_t material = 0;
		uint8_t quality = 0;
		uint64_t user_data = 0;
		const uint8_t *shape = nullptr;
		if (!read(body, BODY_FLAGS, flags) || !read(body, BODY_FILTER, filter) || !read(body, BODY_BROADPHASE, broadphase) || !read(body, BODY_MOTION, motion) || !read(body, BODY_MATERIAL, material)
			|| !read(body, BODY_QUALITY, quality) || !read(body, BODY_USER_DATA, user_data) || !read(body, BODY_SHAPE, shape)) {
			out["readable"] = false;
			return out;
		}

		out["flags"] = hex(flags);
		out["filter"] = hex(filter);
		out["broadphase"] = hex(broadphase);
		out["in_world"] = broadphase != BODY_NONE;
		out["motion"] = hex(motion);
		out["material"] = material;
		out["quality"] = quality;
		// the engine stamps a zone body with 0xffff0000 over its zone's index
		out["user_data"] = hex(user_data);
		out["shape"] = pointer(shape);
		return out;
	}

	auto
	inspect(const char *zone, nlohmann::json &out, const char **reason) -> bool {
		out = nlohmann::json::object();
		if (!gate(1, reason)) {
			return false;
		}

		// a zone can be taken down while the render thread stands in
		if (!game_thread::on_game_thread()) {
			return fail(reason, "collision only reads zones on the game thread, and it is not pumping (loading?)");
		}

		ZoneRef found;
		uint64_t id = 0;
		char path[0x100];
		if (!find_zone(zone, found, id, path, sizeof(path), reason)) {
			return false;
		}

		char asset_id[20];
		_snprintf_s(asset_id, sizeof(asset_id), _TRUNCATE, "%016llx", id);

		uint32_t flags = 0;
		const uint8_t *root = nullptr;
		const uint8_t *system = nullptr;
		const uint8_t *file = nullptr;
		read(found.zone, ZONE_FLAGS, flags);
		read(found.zone, ZONE_PHYSICS_ROOT, root);
		read(found.zone, ZONE_PHYSICS_SYSTEM, system);
		read(found.asset, ZONE_ASSET_PHYSICS_FILE, file);

		const auto *world = current_world();
		nlohmann::json about;
		about["path"] = path;
		about["asset"] = asset_id;
		about["index"] = found.index;
		about["address"] = pointer(found.zone);
		about["asset_status"] = static_cast<uint8_t>(found.header.status);
		about["flags"] = hex(flags);
		about["physics_enabled"] = (flags & ZONE_FLAG_PHYSICS) != 0;
		about["placed_by_matrix"] = (flags & ZONE_FLAG_MATRIX) != 0;
		about["physics_file"] = pointer(file);
		about["physics_root"] = pointer(root);
		about["system"] = pointer(system);
		out["zone"] = std::move(about);
		out["world"] = pointer(world);
		if (system == nullptr) {
			return true;
		}

		uint32_t counted = 0;
		const uint8_t *data = nullptr;
		const uint8_t *of_world = nullptr;
		const uint32_t *ids = nullptr;
		int32_t count = 0;
		uint8_t owns = 0;
		read(system, OBJECT_MEM_SIZE, counted);
		read(system, SYSTEM_DATA, data);
		read(system, SYSTEM_WORLD, of_world);
		read(system, SYSTEM_BODY_IDS, ids);
		read(system, SYSTEM_BODY_COUNT, count);
		read(system, SYSTEM_OWNS_BODIES, owns);

		nlohmann::json about_system;
		about_system["class"] = class_json(system);
		about_system["mem_size_and_references"] = hex(counted);
		about_system["data"] = pointer(data);
		about_system["world"] = pointer(of_world);
		about_system["in_this_world"] = of_world != nullptr && of_world == world;
		about_system["body_count"] = count;
		about_system["owns_bodies"] = owns;
		out["system"] = std::move(about_system);

		const uint8_t *materials = nullptr;
		const uint8_t *cinfos = nullptr;
		int32_t material_count = 0;
		int32_t cinfo_count = 0;
		read(data, OBJECT_MEM_SIZE, counted);
		read_array(data, DATA_MATERIALS, materials, material_count);
		read_array(data, DATA_CINFOS, cinfos, cinfo_count);

		nlohmann::json::array_t listed_materials;
		for (auto i = 0; i < material_count && i < MAX_LISTED; ++i) {
			uint8_t values[sizeof(MATERIAL_DEFAULT_VALUES)];
			nlohmann::json entry;
			if (read(materials, static_cast<size_t>(i) * MATERIAL_SIZE + MATERIAL_VALUES, values)) {
				entry["values"] = ddl::hex_dump(values, sizeof(values));
				entry["is_default"] = memcmp(values, MATERIAL_DEFAULT_VALUES, sizeof(values)) == 0;
			}

			listed_materials.emplace_back(std::move(entry));
		}

		std::vector<const uint8_t *> roots;
		nlohmann::json::array_t listed_cinfos;
		for (auto i = 0; i < cinfo_count && i < MAX_LISTED; ++i) {
			const auto *cinfo = cinfos + static_cast<size_t>(i) * CINFO_SIZE;
			const uint8_t *shape = nullptr;
			if (read(cinfo, CINFO_SHAPE, shape) && shape != nullptr && std::find(roots.begin(), roots.end(), shape) == roots.end()) {
				roots.push_back(shape);
			}

			listed_cinfos.emplace_back(describe_cinfo(cinfo));
		}

		nlohmann::json about_data;
		about_data["class"] = class_json(data);
		about_data["mem_size_and_references"] = hex(counted);
		about_data["material_count"] = material_count;
		about_data["materials"] = std::move(listed_materials);
		about_data["cinfo_count"] = cinfo_count;
		about_data["cinfos"] = std::move(listed_cinfos);
		out["data"] = std::move(about_data);

		nlohmann::json::array_t listed_bodies;
		for (auto i = 0; ids != nullptr && i < count && i < MAX_LISTED; ++i) {
			uint32_t body_id = BODY_NONE;
			if (!read(ids, static_cast<size_t>(i) * sizeof(uint32_t), body_id)) {
				break;
			}

			const uint8_t *shape = nullptr;
			if (read(body_of(world, body_id), BODY_SHAPE, shape) && shape != nullptr && std::find(roots.begin(), roots.end(), shape) == roots.end()) {
				roots.push_back(shape);
			}

			listed_bodies.emplace_back(describe_body(world, body_id));
		}

		out["bodies"] = std::move(listed_bodies);

		std::vector<const uint8_t *> described;
		nlohmann::json::array_t trees;
		for (const auto *shape : roots) {
			trees.emplace_back(describe_shape(shape, described, 0));
		}

		out["shapes"] = std::move(trees);
		return true;
	}

	// -------------------------------------------------------------- selftest --

	static auto
	fields_of(const Field *defaults, const size_t count, const void *vtable) -> std::vector<Field> {
		std::vector<Field> fields(defaults, defaults + count);
		if (vtable != nullptr) {
			fields.push_back({ 0, sizeof(void *), reinterpret_cast<uintptr_t>(vtable) });
		}

		return fields;
	}

	// runs a constructor on a buffer of the hook's filled with a pattern, and
	// compares what it left: the fields it is known to write with their values,
	// and every other byte, past the object's end too, with the pattern. none
	// of the three reads what it is handed
	static auto
	selftest_constructor(const char *name, const construct_t constructor, const uint32_t size, const std::vector<Field> &fields, nlohmann::json &out, const char **reason) -> bool {
		alignas(16) uint8_t buffer[CINFO_SIZE];
		Havok havok;
		void *made = nullptr;
		memset(buffer, FILL, sizeof(buffer));
		if (!about_to(2, false, havok, reason, name)) {
			return false;
		}

		if (!call_construct(constructor, buffer, &made)) {
			return faulted(name, reason);
		}

		bool written[sizeof(buffer)] {};
		nlohmann::json::array_t differences;
		for (const auto &field : fields) {
			uint64_t found = 0;
			memcpy(&found, buffer + field.offset, field.size);
			std::fill_n(written + field.offset, field.size, true);
			if (found != field.value) {
				differences.emplace_back("+" + hex(field.offset) + " is " + hex(found) + ", expected " + hex(field.value));
			}
		}

		for (size_t at = 0; at < sizeof(buffer); ++at) {
			if (!written[at] && buffer[at] != FILL) {
				differences.emplace_back("+" + hex(at) + " was written, and should not have been");
			}
		}

		if (made != buffer) {
			differences.emplace_back("it did not return the object it was handed");
		}

		out["passed"] = differences.empty();
		out["differences"] = std::move(differences);
		out["bytes"] = ddl::hex_dump(buffer, size);
		return true;
	}

	// the three constructors the later stages rely on. one that leaves other
	// bytes than it should is a wrong address or a wrong prototype, and the
	// next is not tried on top of that
	static auto
	selftest_constructors(nlohmann::json &out, const char **reason) -> bool {
		auto material = fields_of(MATERIAL_DEFAULTS, std::size(MATERIAL_DEFAULTS), g_material_vtable);
		for (uint32_t at = 0; at < sizeof(MATERIAL_DEFAULT_VALUES); at += sizeof(uint64_t)) {
			material.push_back({ MATERIAL_VALUES + at, sizeof(uint64_t), get<uint64_t>(MATERIAL_DEFAULT_VALUES, at) });
		}

		struct Test {
			const char *key;
			const char *name;
			construct_t constructor;
			uint32_t size;
			std::vector<Field> fields;
		};

		const Test tests[] = {
			{ "body_cinfo", "the body cinfo constructor", g_body_cinfo_ctor, CINFO_SIZE, fields_of(BODY_CINFO_DEFAULTS, std::size(BODY_CINFO_DEFAULTS), nullptr) },
			{ "material", "the material constructor", g_material_ctor, MATERIAL_SIZE, material },
			{ "mesh_cinfo", "the mesh cinfo constructor", g_mesh_cinfo_ctor, MESH_CINFO_SIZE, fields_of(MESH_CINFO_DEFAULTS, std::size(MESH_CINFO_DEFAULTS), g_base_cinfo_vtable) },
		};

		auto passed = true;
		for (const auto &test : tests) {
			if (!passed) {
				out[test.key] = "not run";
				continue;
			}

			nlohmann::json result;
			if (!selftest_constructor(test.name, test.constructor, test.size, test.fields, result, reason)) {
				return false;
			}

			passed = result["passed"].get<bool>();
			out[test.key] = std::move(result);
		}

		out["passed"] = passed;
		return true;
	}

	// one block the size of a mesh shape off the thread's Havok heap, written,
	// read back and returned
	static auto
	selftest_heap(nlohmann::json &out, const char **reason) -> bool {
		Havok havok;
		void *memory = nullptr;
		if (!about_to(2, false, havok, reason, "the Havok heap's blockAlloc")) {
			return false;
		}

		if (!call_allocate(havok.allocate, havok.heap, MESH_SIZE, &memory)) {
			return faulted("the Havok heap's blockAlloc", reason);
		}

		out["address"] = pointer(memory);
		out["passed"] = memory != nullptr;
		if (memory == nullptr) {
			return true;
		}

		// not handed back: a heap that answers like this is not one to call again
		if ((reinterpret_cast<uintptr_t>(memory) & 0xF) != 0 || !ddl::is_writable(memory, MESH_SIZE)) {
			return latch("the Havok heap handed out a block that is not 16 aligned and writable", reason);
		}

		auto *bytes = static_cast<uint8_t *>(memory);
		memset(bytes, FILL, MESH_SIZE);
		out["passed"] = bytes[0] == FILL && bytes[MESH_SIZE - 1] == FILL;

		// a block that cannot be handed back right now stays with the heap
		if (!about_to(2, false, havok, reason, "the Havok heap's blockFree")) {
			return false;
		}

		if (!call_free(havok.release, havok.heap, memory, MESH_SIZE)) {
			return faulted("the Havok heap's blockFree", reason);
		}

		return true;
	}

	auto
	selftest(const char *what, nlohmann::json &out, const char **reason) -> bool {
		out = nlohmann::json::object();
		const auto constructors = what == nullptr || what[0] == '\0' || strcmp(what, "ctor") == 0;
		auto heap = what == nullptr || what[0] == '\0' || strcmp(what, "heap") == 0;
		if (!constructors && !heap) {
			return fail(reason, "the self tests are ctor and heap");
		}

		Havok havok;
		if (!ready(2, false, havok, reason)) {
			return false;
		}

		if (constructors) {
			out["ctor"] = nlohmann::json::object();
			if (!selftest_constructors(out["ctor"], reason)) {
				return false;
			}

			heap = heap && out["ctor"]["passed"].get<bool>();
		}

		if (heap) {
			out["heap"] = nlohmann::json::object();
			if (!selftest_heap(out["heap"], reason)) {
				return false;
			}
		}

		return true;
	}

	// ---------------------------------------------------------------- bodies --

	static auto
	find_body(const int32_t id, const char **reason) -> Body * {
		if (id < 1 || id > g_body_count || g_bodies[id - 1].phase == Phase::Gone) {
			fail(reason, "there is no body with that number, collision.bodies lists them");
			return nullptr;
		}

		return &g_bodies[id - 1];
	}

	static auto
	shape_of(const int32_t id) -> Shape * {
		return id >= 1 && id <= g_shape_count ? &g_shapes[id - 1] : nullptr;
	}

	static auto
	broadphase_of(const Body &body, uint32_t &broadphase) -> bool {
		broadphase = BODY_NONE;
		return read(body_of(body.world, body.body), BODY_BROADPHASE, broadphase);
	}

	static auto
	in_world(const Body &body) -> bool {
		uint32_t broadphase = BODY_NONE;
		return broadphase_of(body, broadphase) && broadphase != BODY_NONE;
	}

	// whether the engine still has a request for this body in one of its
	// queues. the next one waits until that is worked off: two of them in one
	// pass are run in the queues' order, not in the order they were asked for
	static auto
	queued(const Body &body) -> bool {
		Queue queue;
		if (!read_queue(false, queue)) {
			return true;
		}

		for (auto i = 0; i < queue.count; ++i) {
			const uint8_t *zone = nullptr;
			if (!read(queue.entries, static_cast<size_t>(i) * QUEUE_ENTRY_SIZE, zone) || zone == body.buffers->zone) {
				return true;
			}
		}

		if (!read_queue(true, queue)) {
			return true;
		}

		for (auto i = 0; i < queue.count; ++i) {
			const uint8_t *system = nullptr;
			if (!read(queue.entries, static_cast<size_t>(i) * QUEUE_ENTRY_SIZE, system) || (body.system != nullptr && system == body.system)) {
				return true;
			}
		}

		return false;
	}

	static auto
	state_name(const Body &body) -> const char * {
		switch (body.phase) {
			case Phase::Queued: return "queued";
			case Phase::Built: return body.wanted ? (in_world(body) ? "live" : "created") : (in_world(body) ? "disabling" : "disabled");
			case Phase::Removing:
			case Phase::Destroying: return "removing";
			case Phase::Gone: return "gone";
			case Phase::Orphaned: return "orphaned";
			case Phase::Broken: return "broken";
			case Phase::Lost: return "lost";
		}

		return "?";
	}

	static auto
	body_json(const Body &body) -> nlohmann::json {
		nlohmann::json out;
		out["body"] = body.id;
		out["state"] = state_name(body);
		out["owner"] = "stand-in";
		out["shape"] = body.cooked != 0 ? nlohmann::json(body.cooked) : nlohmann::json();
		out["filter"] = hex(body.filter);
		out["position"] = floats(body.position, 3);
		out["yaw"] = body.yaw;
		out["error"] = body.error != nullptr ? nlohmann::json(body.error) : nlohmann::json();
		if (body.donor != 0) {
			char donor[20];
			_snprintf_s(donor, sizeof(donor), _TRUNCATE, "%016llx", body.donor);
			out["donor"] = donor;
		}

		// what is left of a lost body is of a world that is gone
		uint32_t broadphase = BODY_NONE;
		uint64_t user_data = 0;
		if (body.phase == Phase::Lost || body.body == BODY_NONE || !broadphase_of(body, broadphase)) {
			return out;
		}

		out["id"] = hex(body.body);
		out["broadphase"] = hex(broadphase);
		if (read(body_of(body.world, body.body), BODY_USER_DATA, user_data)) {
			out["user_data"] = hex(user_data);
		}

		return out;
	}

	// takes the next free body and its buffers, zeroed
	static auto
	new_body(const Havok &havok, const char **reason) -> Body * {
		if (g_body_count >= MAX_BODIES) {
			fail(reason, "this session has made as many bodies as it keeps buffers for, restart the game for more");
			return nullptr;
		}

		auto &body = g_bodies[g_body_count];
		body = {};
		body.id = g_body_count + 1;
		body.buffers = &g_buffers[g_body_count];
		body.world = havok.world;
		memset(body.buffers, 0, sizeof(Buffers));
		return &body;
	}

	// describes one static body in the body's buffers and puts it in the load
	// queue: the default material, a body cinfo with the shape, the filter and
	// where it goes, a system data of the two, and a zeroed zone for the engine
	// to store the system in. nothing is counted until the engine took it
	static auto
	push(Body &body, const char **reason) -> bool {
		auto &buffers = *body.buffers;
		Havok havok;
		void *made = nullptr;
		if (!about_to(3, true, havok, reason, "the material constructor", "body", body.id)) {
			return false;
		}

		if (!call_construct(g_material_ctor, buffers.material, &made)) {
			return faulted("the material constructor", reason);
		}

		if (made != buffers.material || get<const void *>(buffers.material, 0) != g_material_vtable || memcmp(buffers.material + MATERIAL_VALUES, MATERIAL_DEFAULT_VALUES, sizeof(MATERIAL_DEFAULT_VALUES)) != 0) {
			return fail(reason, "the material constructor left something else than the default material, nothing was queued");
		}

		if (!about_to(3, true, havok, reason, "the body cinfo constructor", "body", body.id)) {
			return false;
		}

		if (!call_construct(g_body_cinfo_ctor, buffers.cinfo, &made)) {
			return faulted("the body cinfo constructor", reason);
		}

		for (const auto &field : BODY_CINFO_DEFAULTS) {
			uint64_t found = 0;
			memcpy(&found, buffers.cinfo + field.offset, field.size);
			if (made != buffers.cinfo || found != field.value) {
				return fail(reason, "the body cinfo constructor left something else than its defaults, nothing was queued");
			}
		}

		put(buffers.cinfo, CINFO_SHAPE, body.shape);
		put(buffers.cinfo, CINFO_FILTER, body.filter);
		put(buffers.cinfo, CINFO_ATTACHED, CINFO_NOT_ATTACHED);
		memcpy(buffers.cinfo + CINFO_POSITION, body.position, sizeof(body.position));
		// about the up axis, 0 facing +z and turning toward +x. left as the
		// constructor had it for no turn at all
		if (body.yaw != 0.0f) {
			const float orientation[4] = { 0.0f, std::sin(body.yaw * DEGREES * 0.5f), 0.0f, std::cos(body.yaw * DEGREES * 0.5f) };
			memcpy(buffers.cinfo + CINFO_ORIENTATION, orientation, sizeof(orientation));
		}

		// one material and one body, in arrays the system data does not own.
		// its own size word stays zero, so Havok neither counts nor frees it
		put(buffers.data, 0, g_system_data_vtable);
		put(buffers.data, DATA_MATERIALS, static_cast<const uint8_t *>(buffers.material));
		put<int32_t>(buffers.data, DATA_MATERIALS + ARRAY_SIZE, 1);
		put<uint32_t>(buffers.data, DATA_MATERIALS + ARRAY_CAPACITY, ARRAY_NOT_OWNED | 1);
		put<uint32_t>(buffers.data, DATA_MOTION_PROPERTIES + ARRAY_CAPACITY, ARRAY_NOT_OWNED);
		put(buffers.data, DATA_CINFOS, static_cast<const uint8_t *>(buffers.cinfo));
		put<int32_t>(buffers.data, DATA_CINFOS + ARRAY_SIZE, 1);
		put<uint32_t>(buffers.data, DATA_CINFOS + ARRAY_CAPACITY, ARRAY_NOT_OWNED | 1);
		put<uint32_t>(buffers.data, DATA_CONSTRAINTS + ARRAY_CAPACITY, ARRAY_NOT_OWNED);
		put<uint32_t>(buffers.data, DATA_REFERENCED + ARRAY_CAPACITY, ARRAY_NOT_OWNED);
		put<uint8_t>(buffers.data, DATA_STEP_MULTIPLIER, 1);

		// a physics root with one variant, whose scene holds the one system
		// data. the queue call reads them and keeps the system data's address
		const uint8_t *system_data = buffers.data;
		uint8_t scene[SCENE_SIZE] {};
		uint8_t variant[VARIANT_SIZE] {};
		uint8_t root[0x10] {};
		put(scene, SCENE_SYSTEM_DATAS, &system_data);
		put<int32_t>(scene, SCENE_SYSTEM_DATA_COUNT, 1);
		put(variant, VARIANT_SCENE, static_cast<const uint8_t *>(scene));
		put(root, 0, static_cast<const uint8_t *>(variant));
		put<int32_t>(root, ROOT_VARIANT_COUNT, 1);

		Queue before;
		if (!ready(3, true, havok, reason) || !queue_room(false, before, reason)) {
			return false;
		}

		if (havok.world != body.world) {
			return fail(reason, "the physics world changed while the body was put together, nothing was queued");
		}

		announce(QUEUE_LOAD, "body", body.id);
		if (!call_queue(g_delay_load, root, buffers.zone)) {
			return faulted(QUEUE_LOAD, reason);
		}

		Queue after;
		if (read_queue(false, after) && after.count == before.count) {
			return fail(reason, "the engine did not take the body: its load queue was full, or it did not find the system data");
		}

		if (!queue_took(false, before, buffers.zone, buffers.data, 0)) {
			return misqueued(QUEUE_LOAD, reason);
		}

		return true;
	}

	// whether a system is the one the engine made of a body's description: of
	// the body's own system data, which no other system can be made of, in the
	// body's world, with one body. id is that body's
	static auto
	made_of(const Body &body, const uint8_t *system, uint32_t &id) -> bool {
		const uint8_t *data = nullptr;
		const uint8_t *world = nullptr;
		const uint32_t *ids = nullptr;
		int32_t count = 0;
		id = BODY_NONE;
		return is_class(system, SYSTEM_CLASS) && read(system, SYSTEM_DATA, data) && data == body.buffers->data && read(system, SYSTEM_WORLD, world) && world == body.world && read(system, SYSTEM_BODY_COUNT, count) && count == 1
			&& read(system, SYSTEM_BODY_IDS, ids) && read(ids, 0, id);
	}

	// whether the world has the body an id names, made of this body's shape
	static auto
	shaped(const Body &body, const uint32_t id) -> bool {
		const uint8_t *shape = nullptr;
		return read(body_of(body.world, id), BODY_SHAPE, shape) && shape == body.shape;
	}

	// the last look at a body before its system is handed to one of the
	// engine's queues: the engine works the queue off with calls on the system,
	// and on the way out lets go of it. that the world is at the address it was
	// at does not say it is the world it was, and a system of a world that is
	// gone is freed memory. so the system has to be what it was when the engine
	// made it. one that is not is dropped as it is, and its shape with it
	static auto
	still_there(Body &body, const char **reason) -> bool {
		uint32_t id = BODY_NONE;
		if (body.system != nullptr && made_of(body, body.system, id) && id == body.body && shaped(body, id)) {
			return true;
		}

		if (auto *shape = shape_of(body.cooked); shape != nullptr && held(*shape)) {
			shape->state = ShapeState::Lost;
		}

		body.phase = Phase::Lost;
		body.error = "its system is not the one the engine made for it any more, it was dropped without a call";
		_snprintf_s(g_last_error, sizeof(g_last_error), _TRUNCATE, "body %d: %s", body.id, body.error);
		g_output << "[collision] " << g_last_error << "\n";
		g_output.flush();
		return fail(reason, body.error);
	}

	// queues a body's way out of the world, the system kept
	static auto
	take_out(Body &body, const char **reason) -> bool {
		Havok havok;
		Queue before;
		if (!ready(3, true, havok, reason) || !queue_room(true, before, reason) || !still_there(body, reason)) {
			return false;
		}

		announce(QUEUE_DISABLE, "body", body.id);
		if (!call_queue(g_delay_disable, body.system, nullptr)) {
			return faulted(QUEUE_DISABLE, reason);
		}

		if (!queue_took(true, before, body.system, nullptr, 1)) {
			return misqueued(QUEUE_DISABLE, reason);
		}

		body.wanted = false;
		return true;
	}

	// queues a body's way back into the world
	static auto
	put_back(Body &body, const char **reason) -> bool {
		Havok havok;
		Queue before;
		if (!ready(3, true, havok, reason) || !queue_room(false, before, reason) || !still_there(body, reason)) {
			return false;
		}

		announce(QUEUE_ENABLE, "body", body.id);
		if (!call_queue(g_delay_enable, body.system, body.buffers->zone)) {
			return faulted(QUEUE_ENABLE, reason);
		}

		if (!queue_took(false, before, body.buffers->zone, body.system, 1)) {
			return misqueued(QUEUE_ENABLE, reason);
		}

		body.wanted = true;
		return true;
	}

	static auto
	broken(Body &body, const char *why) -> void {
		body.phase = Phase::Broken;
		body.error = why;
		_snprintf_s(g_last_error, sizeof(g_last_error), _TRUNCATE, "body %d: %s", body.id, why);
		g_output << "[collision] " << g_last_error << "\n";
		g_output.flush();
	}

	// a queued body once the engine has worked the queue off: the system it
	// stored in the stand-in zone has to be made of this body's system data, in
	// this body's world, with the one body, of this body's shape
	static auto
	adopt(Body &body) -> void {
		auto *system = get<uint8_t *>(body.buffers->zone, ZONE_PHYSICS_SYSTEM);
		if (system == nullptr) {
			return;
		}

		uint32_t id = BODY_NONE;
		if (!made_of(body, system, id)) {
			broken(body, "the engine stored something in its owner that is not a system made of this body's description");
			return;
		}

		if (!shaped(body, id)) {
			broken(body, "the engine made the system, but no body of this shape came of it");
			return;
		}

		body.system = system;
		body.body = id;
		body.phase = Phase::Built;
		g_output << "[collision] body " << body.id << " created, id 0x" << std::hex << id << std::dec << "\n";
		g_output.flush();
	}

	// the second step of a removal: once the body is out of the world and the
	// engine holds no request for it, its system goes into the queue that lets
	// go of it, which destroys the body
	static auto
	advance(Body &body, const char **reason) -> bool {
		if (body.phase != Phase::Removing || queued(body) || in_world(body)) {
			return true;
		}

		Havok havok;
		Queue before;
		if (!ready(3, true, havok, reason) || !queue_room(true, before, reason) || !still_there(body, reason)) {
			return false;
		}

		announce(QUEUE_UNLOAD, "body", body.id);
		if (!call_queue(g_delay_unload, body.system, nullptr)) {
			return faulted(QUEUE_UNLOAD, reason);
		}

		if (!queue_took(true, before, body.system, nullptr, 0)) {
			return misqueued(QUEUE_UNLOAD, reason);
		}

		// the engine frees the system when it works the queue off, so it is
		// only compared from here on, never read
		body.phase = Phase::Destroying;
		return true;
	}

	// a body the engine has destroyed: its slot carries another id or none
	static auto
	finish(Body &body) -> void {
		if (queued(body) || body_of(body.world, body.body) != nullptr) {
			return;
		}

		// the stand-in is the hook's own, and what it points at is freed
		put<uint64_t>(body.buffers->zone, ZONE_PHYSICS_SYSTEM, 0);
		if (auto *shape = shape_of(body.cooked); shape != nullptr && shape->users > 0) {
			--shape->users;
		}

		body.phase = Phase::Gone;
		body.system = nullptr;
		g_output << "[collision] body " << body.id << " is gone\n";
		g_output.flush();
	}

	// a borrowed shape lives in its zone's physics file. when the zone's slot
	// stops holding what it held, that file is on its way out, and all that is
	// left to do for the body is to get it out of the world. it is not
	// destroyed after: that would hand a freed shape to the reference calls
	static auto
	watch_donor(Body &body) -> void {
		const uint8_t *asset = nullptr;
		const uint8_t *system = nullptr;
		if (body.donor == 0 || (read(body.donor_zone, ZONE_ASSET, asset) && asset == body.donor_asset && read(body.donor_zone, ZONE_PHYSICS_SYSTEM, system) && system == body.donor_system)) {
			return;
		}

		body.phase = Phase::Orphaned;
		body.error = "the zone its shape belongs to was unloaded first";
		_snprintf_s(g_last_error, sizeof(g_last_error), _TRUNCATE, "body %d: %s, it is taken out of the world and left there", body.id, body.error);
		g_output << "[collision] " << g_last_error << "\n";
		g_output.flush();
	}

	static auto
	follow(Body &body) -> void {
		switch (body.phase) {
			case Phase::Queued: adopt(body); break;
			case Phase::Built:
				watch_donor(body);
				if (body.phase == Phase::Built && !body.was_live && in_world(body)) {
					body.was_live = true;
					g_output << "[collision] body " << body.id << " is live\n";
					g_output.flush();
				}

				break;
			case Phase::Removing:
				watch_donor(body);
				if (body.phase == Phase::Removing) {
					advance(body, nullptr);
				}

				break;
			case Phase::Destroying: finish(body); break;
			default: break;
		}

		if (body.phase == Phase::Orphaned && body.wanted && !queued(body)) {
			take_out(body, nullptr);
		}
	}

	auto
	pump() -> void {
		if (g_unavailable[0] != '\0' || (g_body_count == 0 && g_shape_count == 0)) {
			return;
		}

		// the world is looked at first, on whichever thread this is and at any
		// stage: the game thread does not pump through a load, which is when a
		// world would be taken down, and a new one can come up at the address
		// of the old. one look that finds it missing or another drops every
		// handle for good. it reads a pointer out of the game's own image
		watch_world(current_world());
		if (g_stage < 3 || g_latched || !game_thread::on_game_thread()) {
			return;
		}

		auto following = false;
		for (auto i = 0; i < g_body_count && !following; ++i) {
			following = followed(g_bodies[i]);
		}

		if (!following) {
			return;
		}

		// a cached readable region must never outlive the frame it was taken in
		ddl::reset_readable_cache();
		for (auto i = 0; i < g_body_count && !g_latched; ++i) {
			follow(g_bodies[i]);
		}
	}

	auto
	in_the_way(const bool travel) -> const char * {
		auto found = false;
		// nothing is removed once a fault has switched the feature off
		auto stuck = g_latched;
		for (auto i = 0; i < g_body_count; ++i) {
			const auto &body = g_bodies[i];
			// an unload on its own only matters to a body of a shape that is
			// still its zone's
			if (!active(body) || (!travel && (body.donor == 0 || body.phase == Phase::Orphaned))) {
				continue;
			}

			found = true;
			stuck = stuck || body.phase == Phase::Broken || body.phase == Phase::Orphaned;
		}

		if (!found) {
			return nullptr;
		}

		if (travel && stuck) {
			return "a collision test body is still there and cannot be removed any more (collision.status and collision.bodies say why): restart the game before travelling with the hook";
		}

		if (travel) {
			return "collision test bodies are still there, and none has been through a planet change yet: remove them first (collision.remove all) and wait for them to be gone";
		}

		if (stuck) {
			return "a collision test body borrows the shape of a loaded zone and cannot be removed any more (collision.status and collision.bodies say why): restart the game rather than unload that zone under it";
		}

		return "a collision test body borrows the shape of a loaded zone: remove it first (collision.remove all) and wait for it to be gone";
	}

	static auto
	check_position(const float *position, const char **reason) -> bool {
		for (auto axis = 0; axis < 3; ++axis) {
			// written so that a nan fails it too
			if (!(std::fabs(position[axis]) <= MAX_COORDINATE)) {
				return fail(reason, "coordinates have to be finite and within 4096 of the origin on every axis, the physics world reaches no further");
			}
		}

		return true;
	}

	auto
	borrow(const char *zone, const float *offset, nlohmann::json &out, const char **reason) -> bool {
		out = nlohmann::json::object();
		Havok havok;
		if (!ready(3, true, havok, reason) || !check_position(offset, reason)) {
			return false;
		}

		ZoneRef found;
		uint64_t id = 0;
		char path[0x100];
		if (!find_zone(zone, found, id, path, sizeof(path), reason)) {
			return false;
		}

		// the zone's own system, in this world, with a body in it whose shape
		// is the one its first cinfo names: that shape is the zone's root
		uint32_t flags = 0;
		uint8_t *system = nullptr;
		const uint8_t *world = nullptr;
		const uint8_t *data = nullptr;
		const uint8_t *cinfos = nullptr;
		const uint32_t *ids = nullptr;
		const uint8_t *shape = nullptr;
		const uint8_t *named = nullptr;
		int32_t count = 0;
		int32_t cinfo_count = 0;
		uint32_t body_id = BODY_NONE;
		uint32_t filter = 0;
		if (found.header.status != AssetStatus::Loaded || !read(found.zone, ZONE_FLAGS, flags) || (flags & ZONE_FLAG_PHYSICS) == 0) {
			return fail(reason, "that zone is not loaded with its physics on");
		}

		// the engine has moved and turned such a zone's body by the matrix,
		// while the copy is described from nothing: it would land at the
		// offset from the world's origin, not from the zone's body
		if ((flags & ZONE_FLAG_MATRIX) != 0) {
			return fail(reason, "that zone is placed by a matrix (placed_by_matrix in collision.inspect), and a copy of its body would not land at an offset from it: borrow from a zone that is not");
		}

		if (!read(found.zone, ZONE_PHYSICS_SYSTEM, system) || !is_class(system, SYSTEM_CLASS) || !read(system, SYSTEM_WORLD, world) || world != havok.world) {
			return fail(reason, "that zone has no physics system in the world");
		}

		if (!read(system, SYSTEM_BODY_COUNT, count) || count < 1 || !read(system, SYSTEM_BODY_IDS, ids) || !read(ids, 0, body_id) || !read(body_of(havok.world, body_id), BODY_SHAPE, shape) || shape == nullptr) {
			return fail(reason, "that zone's physics system has no body in the world");
		}

		if (!read(system, SYSTEM_DATA, data) || !read_array(data, DATA_CINFOS, cinfos, cinfo_count) || !read(cinfos, CINFO_SHAPE, named) || named != shape || !read(cinfos, CINFO_FILTER, filter)) {
			return fail(reason, "that zone's body is not made of the shape its system data names");
		}

		if (!is_class(shape, COMPOUND_CLASS) && !is_class(shape, MESH_CLASS)) {
			return fail(reason, "that zone's root shape is neither a compound nor a mesh");
		}

		auto *made = new_body(havok, reason);
		if (made == nullptr) {
			return false;
		}

		made->shape = shape;
		made->filter = filter;
		made->donor = id;
		made->donor_zone = found.zone;
		made->donor_asset = found.asset;
		made->donor_system = system;
		memcpy(made->position, offset, sizeof(made->position));
		if (!push(*made, reason)) {
			return false;
		}

		++g_body_count;
		g_output << "[collision] body " << made->id << " queued: the root shape of " << path << ", offset by " << offset[0] << " " << offset[1] << " " << offset[2] << "\n";
		g_output.flush();
		out = body_json(*made);
		return true;
	}

	auto
	bodies(nlohmann::json &out, const char **reason) -> bool {
		out = nlohmann::json::object();
		if (!gate(3, reason)) {
			return false;
		}

		if (!game_thread::on_game_thread()) {
			return fail(reason, "collision only works on the game thread, and it is not pumping (loading?)");
		}

		watch_world(current_world());
		nlohmann::json::array_t listed;
		for (auto i = 0; i < g_body_count; ++i) {
			if (g_bodies[i].phase != Phase::Gone) {
				listed.emplace_back(body_json(g_bodies[i]));
			}
		}

		out["made"] = g_body_count;
		out["limit"] = MAX_BODIES;
		// a body is created whatever the game does, but only a physics step
		// puts it in the world
		out["paused"] = *g_paused != 0 || !(*g_timestep > 0.0f);
		out["bodies"] = std::move(listed);
		return true;
	}

	// the body a switch or a removal is about, once the engine is done with
	// whatever was asked of it before
	static auto
	settled_body(const int32_t id, const char **reason) -> Body * {
		auto *body = find_body(id, reason);
		if (body == nullptr) {
			return nullptr;
		}

		if (body->phase == Phase::Queued) {
			adopt(*body);
		}

		switch (body->phase) {
			case Phase::Built: break;
			case Phase::Queued: fail(reason, "the engine has not made that body yet"); return nullptr;
			case Phase::Removing:
			case Phase::Destroying: fail(reason, "that body is being removed"); return nullptr;
			default: fail(reason, body->error != nullptr ? body->error : "that body is of a physics world that is gone"); return nullptr;
		}

		if (queued(*body)) {
			fail(reason, "the engine has not worked off the last request for that body yet (is physics running?)");
			return nullptr;
		}

		if (body_of(body->world, body->body) == nullptr) {
			broken(*body, "its body is not in the world's body table any more");
			fail(reason, body->error);
			return nullptr;
		}

		return body;
	}

	auto
	enable(const int32_t id, const bool on, nlohmann::json &out, const char **reason) -> bool {
		out = nlohmann::json::object();
		Havok havok;
		if (!ready(3, true, havok, reason)) {
			return false;
		}

		auto *body = settled_body(id, reason);
		if (body == nullptr) {
			return false;
		}

		// putting a body back that is on its way in already would add it twice
		if (on == body->wanted) {
			return fail(reason, on ? (in_world(*body) ? "that body is in the world already" : "that body is on its way into the world already (is the game paused?)") : "that body is out of the world already");
		}

		if (!(on ? put_back(*body, reason) : take_out(*body, reason))) {
			return false;
		}

		out = body_json(*body);
		return true;
	}

	static auto
	start_removal(Body &body, const char **reason) -> bool {
		// out of the world first, through the queue a zone's physics is
		// switched off with
		if (body.wanted && !take_out(body, reason)) {
			return false;
		}

		body.phase = Phase::Removing;
		g_output << "[collision] removing body " << body.id << "\n";
		g_output.flush();
		// a body that was out already goes straight on. one that cannot yet is
		// the pump's to carry on with
		advance(body, nullptr);
		return !g_latched || fail(reason, g_last_error);
	}

	auto
	remove(const int32_t id, nlohmann::json &out, const char **reason) -> bool {
		out = nlohmann::json::object();
		Havok havok;
		if (!ready(3, true, havok, reason)) {
			return false;
		}

		auto *body = settled_body(id, reason);
		if (body == nullptr || !start_removal(*body, reason)) {
			return false;
		}

		out = body_json(*body);
		return true;
	}

	auto
	remove_all(nlohmann::json &out, const char **reason) -> bool {
		out = nlohmann::json::object();
		Havok havok;
		if (!ready(3, true, havok, reason)) {
			return false;
		}

		auto removing = 0;
		auto left = 0;
		for (auto i = 0; i < g_body_count && !g_latched; ++i) {
			if (g_bodies[i].phase != Phase::Queued && g_bodies[i].phase != Phase::Built) {
				continue;
			}

			if (auto *settled = settled_body(g_bodies[i].id, nullptr); settled != nullptr && start_removal(*settled, nullptr)) {
				++removing;
			} else {
				++left;
			}
		}

		if (g_latched) {
			return fail(reason, g_last_error);
		}

		out["removing"] = removing;
		// not made yet, or with a request still in the engine's queue
		out["not_yet"] = left;
		return true;
	}

	// ----------------------------------------------------------------- meshes --

	struct Corner {
		int32_t vertex[3];
		int32_t line;
	};

	struct Mesh {
		// x y z w for every vertex a triangle uses, w zero
		std::vector<float> vertices;
		std::vector<Triangle> triangles;
		float low[3] {};
		float high[3] {};
	};

	static auto
	max_triangles() -> int32_t {
		return g_settings.collision.max_triangles < 1 ? 1 : g_settings.collision.max_triangles > MAX_TRIANGLES ? MAX_TRIANGLES : g_settings.collision.max_triangles;
	}

	static auto
	is_blank(const char c) -> bool {
		return c == ' ' || c == '\t' || c == '\r';
	}

	// one number off the front of what is left of a line. from_chars, since the
	// game's locale is not the hook's to rely on
	template <typename T>
	static auto
	take_number(const char *&at, const char *end, T &out) -> bool {
		while (at < end && is_blank(*at)) {
			++at;
		}

		if (at < end && *at == '+') {
			++at;
		}

		const auto result = std::from_chars(at, end, out);
		if (result.ec != std::errc {}) {
			return false;
		}

		at = result.ptr;
		return true;
	}

	// checks what a file or the test quad came to and turns it into what the
	// builder is handed: only the vertices a triangle uses, every triangle
	// tagged with the flags. the builder takes anything, so what would only
	// show as a missing or a snagging triangle later is refused here
	static auto
	finish_mesh(const std::vector<float> &positions, const std::vector<Corner> &faces, const uint32_t flags, Mesh &mesh, const char **reason) -> bool {
		const auto count = static_cast<int64_t>(positions.size() / 3);
		if (faces.empty()) {
			return fail(reason, "the mesh has no triangles");
		}

		// where each vertex goes: nowhere, until a triangle is seen to use it
		std::vector<int32_t> slot(static_cast<size_t>(count), -1);
		for (size_t index = 0; index < faces.size(); ++index) {
			const auto &face = faces[index];
			const float *corner[3] {};
			for (auto i = 0; i < 3; ++i) {
				if (face.vertex[i] < 0 || face.vertex[i] >= count) {
					return failf(reason, "triangle %zu (line %d) uses a vertex the mesh does not have", index + 1, face.line);
				}

				corner[i] = &positions[static_cast<size_t>(face.vertex[i]) * 3];
				for (auto axis = 0; axis < 3; ++axis) {
					// written so that a nan fails it too
					if (!(std::fabs(corner[i][axis]) <= MAX_COORDINATE)) {
						return failf(reason, "triangle %zu (line %d) has a coordinate that is not a number or is more than 4096 from the origin", index + 1, face.line);
					}
				}
			}

			// two corners the builder would weld into one, or three in a line
			const float ab[3] = { corner[1][0] - corner[0][0], corner[1][1] - corner[0][1], corner[1][2] - corner[0][2] };
			const float ac[3] = { corner[2][0] - corner[0][0], corner[2][1] - corner[0][1], corner[2][2] - corner[0][2] };
			const float bc[3] = { corner[2][0] - corner[1][0], corner[2][1] - corner[1][1], corner[2][2] - corner[1][2] };
			const float cross[3] = { ab[1] * ac[2] - ab[2] * ac[1], ab[2] * ac[0] - ab[0] * ac[2], ab[0] * ac[1] - ab[1] * ac[0] };
			const auto ab2 = ab[0] * ab[0] + ab[1] * ab[1] + ab[2] * ab[2];
			const auto ac2 = ac[0] * ac[0] + ac[1] * ac[1] + ac[2] * ac[2];
			const auto bc2 = bc[0] * bc[0] + bc[1] * bc[1] + bc[2] * bc[2];
			const auto area2 = cross[0] * cross[0] + cross[1] * cross[1] + cross[2] * cross[2];
			constexpr float SHORTEST2 = WELD_TOLERANCE * WELD_TOLERANCE;
			constexpr float FLATTEST2 = 1.0e-8f;
			if (ab2 <= SHORTEST2 || ac2 <= SHORTEST2 || bc2 <= SHORTEST2 || area2 <= FLATTEST2 * ab2 * ac2) {
				return failf(reason, "triangle %zu (line %d) is degenerate: two corners within a centimetre of each other, or all three in a line", index + 1, face.line);
			}

			for (const auto vertex : face.vertex) {
				slot[static_cast<size_t>(vertex)] = 0;
			}
		}

		// the vertices stay in the order the mesh has them, and the triangles
		// in theirs, with their corners as they were: what the builder makes of
		// a mesh is compared with the baked collision of the same mesh, and a
		// difference should be the builder's
		for (size_t vertex = 0; vertex < slot.size(); ++vertex) {
			if (slot[vertex] < 0) {
				continue;
			}

			const auto *position = &positions[vertex * 3];
			const auto first = mesh.vertices.empty();
			slot[vertex] = static_cast<int32_t>(mesh.vertices.size() / 4);
			mesh.vertices.insert(mesh.vertices.end(), { position[0], position[1], position[2], 0.0f });
			for (auto axis = 0; axis < 3; ++axis) {
				mesh.low[axis] = first || position[axis] < mesh.low[axis] ? position[axis] : mesh.low[axis];
				mesh.high[axis] = first || position[axis] > mesh.high[axis] ? position[axis] : mesh.high[axis];
			}
		}

		mesh.triangles.reserve(faces.size());
		for (const auto &face : faces) {
			Triangle triangle {};
			triangle.a = slot[static_cast<size_t>(face.vertex[0])];
			triangle.b = slot[static_cast<size_t>(face.vertex[1])];
			triangle.c = slot[static_cast<size_t>(face.vertex[2])];
			triangle.tag = static_cast<uint16_t>(flags);
			mesh.triangles.push_back(triangle);
		}

		return true;
	}

	// quad:<size>, the two triangles the smallest shipped collision mesh is made
	// of, its vertices in the same order and wound the same way: flat, facing
	// up, size metres on a side
	static auto
	load_quad(const char *size_text, const uint32_t flags, Mesh &mesh, const char **reason) -> bool {
		const auto *at = size_text;
		const auto *end = size_text + strlen(size_text);
		float size = 0.0f;
		if (!take_number(at, end, size) || at != end || !(size >= 0.1f && size <= MAX_COORDINATE * 2.0f)) {
			return fail(reason, "the quad's size is in metres, 0.1 to 8192: quad:4");
		}

		const auto half = size * 0.5f;
		const std::vector<float> positions = { half, 0.0f, -half, -half, 0.0f, -half, -half, 0.0f, half, half, 0.0f, half };
		const std::vector<Corner> faces = { { { 2, 3, 0 }, 0 }, { { 2, 0, 1 }, 0 } };
		return finish_mesh(positions, faces, flags, mesh, reason);
	}

	static auto
	game_directory() -> std::filesystem::path {
		wchar_t buffer[0x400];
		const auto length = GetModuleFileNameW(g_game_module, buffer, static_cast<DWORD>(std::size(buffer)));
		return length == 0 || length >= std::size(buffer) ? std::filesystem::path() : std::filesystem::path(buffer).parent_path();
	}

	// a Wavefront .obj, as much of one as a collision mesh needs: "v x y z"
	// lines and "f a b c" lines, 1 based, negative ones counting back from the
	// last vertex, with anything after a slash in a corner ignored. every other
	// line is skipped, and a face with more than three corners is refused
	static auto
	load_obj(const char *source, const uint32_t flags, Mesh &mesh, const char **reason) -> bool {
		auto path = std::filesystem::path(std::u8string(reinterpret_cast<const char8_t *>(source)));
		if (_wcsicmp(path.extension().c_str(), L".obj") != 0) {
			return fail(reason, "a mesh is quad:<size> or the path of a .obj file");
		}

		if (path.is_relative()) {
			path = game_directory() / path;
		}

		std::ifstream file { path, std::ios::binary | std::ios::ate };
		if (!file.is_open()) {
			return fail(reason, "the mesh file could not be opened");
		}

		const auto size = static_cast<size_t>(file.tellg());
		if (size == 0 || size > MAX_OBJ_BYTES) {
			return fail(reason, "the mesh file is empty, or larger than 32 MiB");
		}

		std::string text(size, '\0');
		file.seekg(0);
		file.read(text.data(), static_cast<std::streamsize>(size));
		if (!file.good()) {
			return fail(reason, "the mesh file could not be read");
		}

		const auto limit = static_cast<size_t>(max_triangles());
		std::vector<float> positions;
		std::vector<Corner> faces;
		auto line_number = 0;
		for (const auto *line = text.data(), *end = text.data() + text.size(); line < end;) {
			const auto *stop = static_cast<const char *>(memchr(line, '\n', static_cast<size_t>(end - line)));
			stop = stop != nullptr ? stop : end;
			const auto *at = line;
			line = stop < end ? stop + 1 : end;
			++line_number;

			while (at < stop && is_blank(*at)) {
				++at;
			}

			if (stop - at < 2 || !is_blank(at[1]) || (at[0] != 'v' && at[0] != 'f')) {
				continue;
			}

			const auto is_vertex = at[0] == 'v';
			++at;
			if (is_vertex) {
				float position[3];
				if (!take_number(at, stop, position[0]) || !take_number(at, stop, position[1]) || !take_number(at, stop, position[2]) || (at < stop && !is_blank(*at))) {
					return failf(reason, "line %d: a vertex is v and three numbers", line_number);
				}

				positions.insert(positions.end(), position, position + 3);
				continue;
			}

			Corner face {};
			face.line = line_number;
			auto corners = 0;
			for (;; ++corners) {
				while (at < stop && is_blank(*at)) {
					++at;
				}

				if (at == stop) {
					break;
				}

				int64_t index = 0;
				if (corners == 3) {
					return failf(reason, "line %d: a face with more than three corners, triangulate the mesh first", line_number);
				}

				if (!take_number(at, stop, index) || index == 0 || (at < stop && !is_blank(*at) && *at != '/')) {
					return failf(reason, "line %d: a face is f and three vertex numbers", line_number);
				}

				// what follows a slash is a texture coordinate or a normal
				while (at < stop && !is_blank(*at)) {
					++at;
				}

				const auto so_far = static_cast<int64_t>(positions.size() / 3);
				index = index < 0 ? so_far + index : index - 1;
				face.vertex[corners] = index < 0 || index > INT32_MAX ? -1 : static_cast<int32_t>(index);
			}

			if (corners != 3) {
				return failf(reason, "line %d: a face is f and three vertex numbers", line_number);
			}

			if (faces.size() >= limit) {
				return failf(reason, "the mesh has more than %zu triangles, which is where [collision] max_triangles has the limit", limit);
			}

			faces.push_back(face);
		}

		return finish_mesh(positions, faces, flags, mesh, reason);
	}

	// ------------------------------------------------------------------ cook --

	// hands a mesh to the builder: a geometry of the two arrays, the default
	// mesh cinfo over it, 0x90 bytes of the thread's Havok heap, and the mesh
	// shape's constructor, which is the cook. it runs to the end on this thread
	// and reads nothing of the mesh after it returns
	static auto
	build(Havok &havok, Mesh &mesh, uint8_t *&shape, double &cook_ms, const char **reason) -> bool {
		const auto vertices = static_cast<int32_t>(mesh.vertices.size() / 4);
		const auto triangles = static_cast<int32_t>(mesh.triangles.size());

		alignas(16) uint8_t geometry[GEOMETRY_SIZE] {};
		put(geometry, GEOMETRY_VERTICES, mesh.vertices.data());
		put(geometry, GEOMETRY_VERTICES + ARRAY_SIZE, vertices);
		put(geometry, GEOMETRY_VERTICES + ARRAY_CAPACITY, ARRAY_NOT_OWNED | static_cast<uint32_t>(vertices));
		put(geometry, GEOMETRY_TRIANGLES, mesh.triangles.data());
		put(geometry, GEOMETRY_TRIANGLES + ARRAY_SIZE, triangles);
		put(geometry, GEOMETRY_TRIANGLES + ARRAY_CAPACITY, ARRAY_NOT_OWNED | static_cast<uint32_t>(triangles));

		alignas(16) uint8_t cinfo[MESH_CINFO_SIZE] {};
		void *made = nullptr;
		if (!about_to(4, false, havok, reason, "the mesh cinfo constructor")) {
			return false;
		}

		if (!call_construct(g_mesh_cinfo_ctor, cinfo, &made)) {
			return faulted("the mesh cinfo constructor", reason);
		}

		// every build default is left as it is, so they have to be the known ones
		for (const auto &field : MESH_CINFO_DEFAULTS) {
			uint64_t found = 0;
			memcpy(&found, cinfo + field.offset, field.size);
			if (made != cinfo || get<const void *>(cinfo, 0) != g_base_cinfo_vtable || found != field.value) {
				return fail(reason, "the mesh cinfo constructor left something else than its defaults, nothing was cooked");
			}
		}

		// the base class cannot be read from. the default one reads the geometry
		put(cinfo, 0, g_default_cinfo_vtable);
		put(cinfo, MESH_CINFO_GEOMETRY, static_cast<const uint8_t *>(geometry));
		put<uint64_t>(cinfo, MESH_CINFO_CONVEX, 0);
		put<int32_t>(cinfo, MESH_CINFO_CONVEX_COUNT, 0);

		void *memory = nullptr;
		if (!about_to(4, false, havok, reason, "the Havok heap's blockAlloc")) {
			return false;
		}

		if (!call_allocate(havok.allocate, havok.heap, MESH_SIZE, &memory)) {
			return faulted("the Havok heap's blockAlloc", reason);
		}

		if (memory == nullptr) {
			return fail(reason, "the Havok heap had no block for the shape");
		}

		// not handed back: a heap that answers like this is not one to call again
		if ((reinterpret_cast<uintptr_t>(memory) & 0xF) != 0 || !ddl::is_writable(memory, MESH_SIZE)) {
			return latch("the Havok heap handed out a block that is not 16 aligned and writable", reason);
		}

		// a block that is not cooked into after all stays with the heap
		if (!ready(4, false, havok, reason)) {
			return false;
		}

		LARGE_INTEGER frequency;
		LARGE_INTEGER started;
		LARGE_INTEGER ended;
		QueryPerformanceFrequency(&frequency);
		g_output << "[collision] calling the mesh shape constructor with " << vertices << " vertices and " << triangles << " triangles\n";
		g_output.flush();
		QueryPerformanceCounter(&started);
		if (!call_cook(memory, cinfo, &made)) {
			return faulted("the mesh shape constructor", reason);
		}

		QueryPerformanceCounter(&ended);
		cook_ms = static_cast<double>(ended.QuadPart - started.QuadPart) * 1000.0 / static_cast<double>(frequency.QuadPart);
		if (made != memory) {
			return latch("the mesh shape constructor returned another address than it was given", reason);
		}

		shape = static_cast<uint8_t *>(memory);
		return true;
	}

	// the checks a fresh shape has to pass before a body is made of it. counted
	// is whether it is a mesh shape holding one reference, the hook's, which is
	// all that giving it up again needs
	static auto
	examine(const uint8_t *shape, const Mesh &mesh, nlohmann::json &checks, bool &counted, int32_t &cooked) -> bool {
		ShapeHeader header {};
		const auto readable = read(shape, 0, header);
		const auto is_mesh = readable && is_class(shape, MESH_CLASS);
		counted = is_mesh && header.memSize == MEM_SIZE_COUNTED && header.references == 1;

		const uint8_t *data = nullptr;
		float low[4];
		float high[4];
		read(shape, MESH_TRIANGLES, cooked);
		const auto has_data = read(shape, MESH_DATA, data) && is_class(data, MESH_DATA_CLASS);
		auto encloses = has_data && read(data, MESH_DATA_AABB_MIN, low) && read(data, MESH_DATA_AABB_MAX, high);
		for (auto axis = 0; encloses && axis < 3; ++axis) {
			// written so that a nan fails it too
			encloses = low[axis] <= mesh.low[axis] + WELD_TOLERANCE && high[axis] >= mesh.high[axis] - WELD_TOLERANCE;
		}

		checks["class"] = is_mesh;
		checks["one_reference"] = counted;
		checks["type"] = readable && header.type == SHAPE_TYPE_MESH;
		checks["shape_flags"] = readable && header.flags == SHAPE_FLAGS_MESH;
		checks["dispatch"] = readable && header.dispatch == SHAPE_DISPATCH_MESH;
		checks["triangles"] = cooked > 0 && cooked <= static_cast<int32_t>(mesh.triangles.size());
		checks["data_class"] = has_data;
		checks["aabb_encloses_input"] = encloses;
		for (const auto &passed : checks) {
			if (!passed.get<bool>()) {
				return false;
			}
		}

		return true;
	}

	static auto
	state_name(const ShapeState state) -> const char * {
		switch (state) {
			case ShapeState::Usable: return "usable";
			case ShapeState::Rejected: return "rejected";
			case ShapeState::Released: return "released";
			case ShapeState::Lost: return "lost";
		}

		return "?";
	}

	static auto
	shape_json(const Shape &shape) -> nlohmann::json {
		nlohmann::json out;
		out["shape"] = shape.id;
		out["state"] = state_name(shape.state);
		out["source"] = shape.source;
		out["flags"] = hex(shape.flags);
		out["vertices"] = shape.vertices;
		out["triangles_in"] = shape.triangles;
		out["triangles_out"] = shape.cooked;
		out["input_aabb"] = { { "min", floats(shape.low, 3) }, { "max", floats(shape.high, 3) } };
		out["cook_ms"] = shape.cook_ms;
		out["bodies"] = shape.users;

		// a released shape is freed memory, a lost one of a world that is gone
		ShapeHeader header {};
		if (held(shape) && read(shape.shape, 0, header)) {
			out["address"] = pointer(shape.shape);
			out["references"] = header.references;
		}

		return out;
	}

	auto
	cook(const char *source, const uint32_t flags, nlohmann::json &out, const char **reason) -> bool {
		out = nlohmann::json::object();
		Havok havok;
		if (!ready(4, false, havok, reason)) {
			return false;
		}

		if (flags > FLAGS_MASK || (flags & FLAG_OTHER_USER_DATA) != 0) {
			return fail(reason, "the flags are the gameplay flags of a surface, at most 0x7ff, and without 0x100: with that one a hit reads the shape's user data another way");
		}

		if (g_shape_count >= MAX_SHAPES) {
			return fail(reason, "this session has cooked as many shapes as it keeps track of, restart the game for more");
		}

		if (source == nullptr || source[0] == '\0') {
			return fail(reason, "a mesh is quad:<size> or the path of a .obj file");
		}

		if (strlen(source) >= sizeof(Shape::source)) {
			return fail(reason, "the mesh path is too long");
		}

		Mesh mesh;
		try {
			constexpr const char *QUAD = "quad:";
			if (!(_strnicmp(source, QUAD, strlen(QUAD)) == 0 ? load_quad(source + strlen(QUAD), flags, mesh, reason) : load_obj(source, flags, mesh, reason))) {
				return false;
			}
		} catch (const std::exception &) {
			return fail(reason, "the mesh could not be read");
		}

		if (mesh.triangles.size() > static_cast<size_t>(max_triangles())) {
			return failf(reason, "the mesh has more than %d triangles, which is where [collision] max_triangles has the limit", max_triangles());
		}

		uint8_t *made = nullptr;
		double cook_ms = 0.0;
		if (!build(havok, mesh, made, cook_ms, reason)) {
			return false;
		}

		auto &shape = g_shapes[g_shape_count];
		shape = {};
		shape.id = ++g_shape_count;
		shape.shape = made;
		shape.world = havok.world;
		shape.flags = flags;
		shape.vertices = static_cast<int32_t>(mesh.vertices.size() / 4);
		shape.triangles = static_cast<int32_t>(mesh.triangles.size());
		shape.cook_ms = cook_ms;
		memcpy(shape.low, mesh.low, sizeof(shape.low));
		memcpy(shape.high, mesh.high, sizeof(shape.high));
		strncpy_s(shape.source, source, _TRUNCATE);

		nlohmann::json checks;
		const auto passed = examine(made, mesh, checks, shape.counted, shape.cooked);
		shape.state = passed ? ShapeState::Usable : ShapeState::Rejected;

		// read out as the builder left it, before the hook writes into it
		std::vector<const uint8_t *> described;
		auto measured = describe_shape(made, described, 0);
		nlohmann::json written;
		if (passed) {
			// what a baked mesh has here: no tag codec, and the gameplay flags
			// as user data, over a material index of zero
			put<uint32_t>(made, SHAPE_CODEC_INFO, 0);
			put<uint64_t>(made, SHAPE_USER_DATA, flags);
			written = { { "codec_info", hex(0) }, { "user_data", hex(flags) } };
		}

		out = shape_json(shape);
		out["usable"] = passed;
		out["checks"] = std::move(checks);
		out["measured"] = std::move(measured);
		out["written"] = std::move(written);
		g_output << "[collision] shape " << shape.id << " cooked from " << source << " in " << cook_ms << " ms: " << shape.triangles << " triangles in, " << shape.cooked << " out, " << (passed ? "usable" : "rejected, see its checks") << "\n";
		g_output.flush();
		return true;
	}

	auto
	shapes(nlohmann::json &out, const char **reason) -> bool {
		out = nlohmann::json::object();
		if (!gate(4, reason)) {
			return false;
		}

		if (!game_thread::on_game_thread()) {
			return fail(reason, "collision only works on the game thread, and it is not pumping (loading?)");
		}

		watch_world(current_world());
		nlohmann::json::array_t listed;
		for (auto i = 0; i < g_shape_count; ++i) {
			if (g_shapes[i].state != ShapeState::Released) {
				listed.emplace_back(shape_json(g_shapes[i]));
			}
		}

		out["cooked"] = g_shape_count;
		out["limit"] = MAX_SHAPES;
		out["max_triangles"] = max_triangles();
		out["shapes"] = std::move(listed);
		return true;
	}

	// gives up the hook's reference, which has to be the last one: no body of
	// the hook's made of the shape, and the shape itself saying one
	static auto
	release_shape(Shape &shape, const char **reason) -> bool {
		if (!held(shape)) {
			return fail(reason, shape.state == ShapeState::Released ? "that shape was released already" : "that shape was dropped, with the physics world it was cooked in or with a body of it, and is left alone");
		}

		if (shape.users > 0) {
			return fail(reason, "a body still uses that shape, remove it first and wait for it to be gone");
		}

		ShapeHeader header {};
		if (!shape.counted || !read(shape.shape, 0, header) || !is_class(shape.shape, MESH_CLASS) || header.memSize != MEM_SIZE_COUNTED) {
			return fail(reason, "that shape is not a counted mesh shape, it is left alone");
		}

		if (header.references != 1) {
			return failf(reason, "that shape has %u references and the hook holds one: something of the engine's still uses it", static_cast<uint32_t>(header.references));
		}

		Havok havok;
		if (!ready(4, false, havok, reason)) {
			return false;
		}

		announce("removeReference", "shape", shape.id);
		if (!call_release(shape.shape)) {
			return faulted("removeReference", reason);
		}

		shape.state = ShapeState::Released;
		return true;
	}

	auto
	release(const int32_t id, nlohmann::json &out, const char **reason) -> bool {
		out = nlohmann::json::object();
		Havok havok;
		if (!ready(4, false, havok, reason)) {
			return false;
		}

		auto *shape = shape_of(id);
		if (shape == nullptr) {
			return fail(reason, "there is no shape with that number, collision.shapes lists them");
		}

		if (!release_shape(*shape, reason)) {
			return false;
		}

		out = shape_json(*shape);
		return true;
	}

	auto
	release_all(nlohmann::json &out, const char **reason) -> bool {
		out = nlohmann::json::object();
		Havok havok;
		if (!ready(4, false, havok, reason)) {
			return false;
		}

		auto released = 0;
		auto kept = 0;
		for (auto i = 0; i < g_shape_count && !g_latched; ++i) {
			if (!held(g_shapes[i])) {
				continue;
			}

			if (release_shape(g_shapes[i], nullptr)) {
				++released;
			} else {
				++kept;
			}
		}

		if (g_latched) {
			return fail(reason, g_last_error);
		}

		out["released"] = released;
		// in use by a body, or not a shape that can be given up
		out["kept"] = kept;
		return true;
	}

	auto
	add(const int32_t id, const float *position, const float yaw, nlohmann::json &out, const char **reason) -> bool {
		out = nlohmann::json::object();
		Havok havok;
		if (!ready(5, true, havok, reason) || !check_position(position, reason)) {
			return false;
		}

		if (!std::isfinite(yaw)) {
			return fail(reason, "the yaw must be finite");
		}

		auto *shape = shape_of(id);
		if (shape == nullptr) {
			return fail(reason, "there is no shape with that number, collision.shapes lists them");
		}

		if (shape->state != ShapeState::Usable) {
			return fail(reason, shape->state == ShapeState::Rejected ? "that shape failed its checks when it was cooked, no body is made of it" : "that shape is released, or was dropped with its physics world or a body of it");
		}

		// still the shape it was cooked as, with the hook's reference in it
		ShapeHeader header {};
		if (!read(shape->shape, 0, header) || !is_class(shape->shape, MESH_CLASS) || header.type != SHAPE_TYPE_MESH || header.references < 1 || header.userData != shape->flags) {
			return fail(reason, "that shape is not what it was when it was cooked, no body is made of it");
		}

		// turned about the up axis the mesh sweeps a circle, which has to stay
		// inside what the broadphase covers as well
		auto reach = 0.0f;
		for (const auto axis : { 0, 2 }) {
			reach = std::fmax(reach, std::fmax(std::fabs(shape->low[axis]), std::fabs(shape->high[axis])));
		}

		reach *= 1.41421357f;
		if (std::fabs(position[0]) + reach > MAX_COORDINATE || std::fabs(position[2]) + reach > MAX_COORDINATE || position[1] + shape->low[1] < -MAX_COORDINATE || position[1] + shape->high[1] > MAX_COORDINATE) {
			return fail(reason, "the shape would reach more than 4096 from the origin there, the physics world reaches no further");
		}

		auto *made = new_body(havok, reason);
		if (made == nullptr) {
			return false;
		}

		made->shape = shape->shape;
		made->cooked = shape->id;
		// what a zone's own body has, over the flags the triangles are tagged with
		made->filter = FILTER_ZONE_BODY | shape->flags;
		made->yaw = yaw;
		memcpy(made->position, position, sizeof(made->position));
		if (!push(*made, reason)) {
			return false;
		}

		++g_body_count;
		++shape->users;
		g_output << "[collision] body " << made->id << " queued: shape " << shape->id << " at " << position[0] << " " << position[1] << " " << position[2] << ", yaw " << yaw << "\n";
		g_output.flush();
		out = body_json(*made);
		return true;
	}
} // namespace rivet_hook::collision

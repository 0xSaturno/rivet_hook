// SPDX-FileCopyrightText: 2025-2026 Neptuwunium
//
// SPDX-License-Identifier: EUPL-1.2

#pragma once

#include "asset.hpp"
#include "ddl.hpp"

#include <DirectXMath.h>

namespace rivet_hook::game {
#pragma pack(push, 1)

	struct EngineHandle {
		union {
			struct {
				uint32_t id : 20;
				// bumped every time the slot is reused, 0 is never a live handle
				uint32_t generation : 12;
			};

			uint32_t value;
		};

		__forceinline auto
		IsValid() const -> bool {
			return value > 0 && value < UINT32_MAX;
		}

		__forceinline auto
		operator ==(const EngineHandle &other) const -> bool {
			return other.value == value;
		}

		__forceinline auto
		operator !=(const EngineHandle &other) const -> bool {
			return other.value != value;
		}
	};

	static_assert(sizeof(EngineHandle) == 4, "ActorHandle size is not 4");

	static constexpr auto INVALID_ENGINE_HANDLE = EngineHandle { .value = 0xFFFFFFFF };

	struct SceneObject;
	struct Component;

	namespace ActorFlag {
		constexpr uint32_t Activated = 1u << 0;
		constexpr uint32_t Template = 1u << 2;
		constexpr uint32_t StartedActive = 1u << 3;
		// the slot is in use. freed slots keep their stale scene object pointer
		constexpr uint32_t Allocated = 1u << 9;
		constexpr uint32_t Destroying = 1u << 10;
		// components tick
		constexpr uint32_t UpdateEnabled = 1u << 13;
	} // namespace ActorFlag

	constexpr const char *ACTOR_FLAG_NAMES[] = {
		"Activated",
		"Hatched",
		"Template",
		"StartedActive",
		"PriusPermanent",
		"EnableActivationEvents",
		"ActivateEventsBroadcast",
		"DestroyOnUnloadZone",
		"DestroyOnUnloadLevel",
		"Allocated",
		"Destroying",
		"IgnoreZoneVisibilityChanges",
		"IsActivatingOrDeactivating",
		"UpdateEnabled",
		"TimeScaleUpdated",
		"ZoneUnloading",
		nullptr,
		"FXSpawned",
		"IsSynced",
		"IsMovingSurface",
		"IsMovingSurfaceChild",
	};

	inline auto
	DescribeActorFlags(const uint32_t flags) -> std::string {
		return DescribeFlags(flags, ACTOR_FLAG_NAMES, std::size(ACTOR_FLAG_NAMES));
	}

	struct ComponentPointer {
		ComponentInfo* componentType;
		Component* instance;
	};

	static_assert(sizeof(ComponentPointer) == 0x10, "ComponentPointer size is not 0x10");

	// updateParent and updateChildren are update order dependencies, not the
	// transform hierarchy
	struct Actor {
		SceneObject *object;
		uint16_t generation;
		int16_t zoneIndex; // -1 when the actor belongs to no zone
		uint32_t sceneIndex;
		uint32_t flags; // ActorFlag
		EngineHandle updateParent;
		uint16_t updateBucket[2]; // current, default
		int16_t parentChildrenIndex; // this actor's slot in updateParent's updateChildren
		uint16_t unknown4;
		EngineHandle *updateChildren;
		uint16_t updateChildrenCount;
		uint16_t updateChildrenMax;
		uint32_t unknown5;
		Asset *actorAsset;
		uint64_t unknown6[4];
		ComponentPointer firstComponent;
		ComponentPointer* components;
		uint16_t componentCount;
		uint16_t unknown10;
		uint16_t componentRemain;
		uint16_t unknown11;
		intptr_t unknown12;
		ComponentPointer* componentLookup;
		uint16_t componentLookupCount;
		uint16_t componentLookupCapacity;
		uint32_t unknown13;
		float timeScale;
		float timeScaleRequested; // copied into timeScale once a frame
		double timeScaleUpdateTime;
		double timeAccumulator;
		uint64_t unknown14;
		const char *name;
		uint64_t unknown15;

		__forceinline auto
		GetName() const -> const char * {
			if (name && *name) {
				return name;
			}

			if (actorAsset && actorAsset->name && *actorAsset->name) {
				return actorAsset->GetShortName();
			}

			return nullptr;
		}

		__forceinline auto
		HasUpdateChildren() const -> bool {
			return updateChildren != nullptr && updateChildrenCount > 0;
		}

		// a live, allocated slot. a freed slot can keep its old scene object
		// pointer, so the object alone does not prove anything.
		__forceinline auto
		IsValid() const -> bool {
			return generation != 0 && (flags & ActorFlag::Allocated) != 0 && object != nullptr;
		}
	};

	static_assert(sizeof(Actor) == 0xC0, "Actor size is not 0xC0");
	static_assert(offsetof(Actor, actorAsset) == 0x30, "Actor actorAsset offset is not 0x30");
	static_assert(offsetof(Actor, components) == 0x68, "Actor components offset is not 0x68");
	static_assert(offsetof(Actor, componentLookup) == 0x80, "Actor componentLookup offset is not 0x80");
	static_assert(offsetof(Actor, name) == 0xb0, "Actor name offset is not 0xb0");

	struct alignas(16) SceneObject {
		union alignas(16) {
			DirectX::XMMATRIX transform;
			float transform_matrix[4][4];
		};
		DirectX::XMVECTORF32 unknownVector;
		DirectX::XMFLOAT3 extents;
		uint32_t objectFlags : 24;
		uint32_t objectType : 8;
		uint32_t unknown1;
		EngineHandle sceneHandle;
		EngineHandle actorHandle;
		uint32_t lastModifiedOnFrame;
		DirectX::XMFLOAT3 scale;
		uint32_t createdOnFrame;
	};

	static_assert(sizeof(SceneObject) == 0x80, "SceneObject size is 0x80");
	static_assert(offsetof(SceneObject, unknownVector) == 0x40, "SceneObject unknownVector offset is not 0x40");
	static_assert(offsetof(SceneObject, extents) == 0x50, "SceneObject radius offset is not 0x4C");
	static_assert(offsetof(SceneObject, scale) == 0x70, "SceneObject radius offset is not 0x70");

	// slot 9 (+0x48), the one the update dispatcher calls through
	constexpr int COMPONENT_VTABLE_GET_TYPE_INFO = 0x9;

	namespace ComponentFlag {
		constexpr uint8_t Active = 1u << 0;
		// awaiting cleanup: still in the actor's list but no longer alive
		constexpr uint8_t Destroyed = 1u << 1;
	} // namespace ComponentFlag

	struct Component {
		intptr_t* vtable;
		void* ddlPriusData;
		Actor* actor;
		float timeScale; // mirrored from the owning actor
		EngineHandle handle;
		uint32_t syncHandle;
		uint8_t childCount;
		uint8_t flags; // ComponentFlag
		uint16_t padding;
		uint32_t updateListIndex[0x6]; // slot in each update stage's list
		uint32_t updateFlags; // bit n = update stage n enabled
		EngineHandle parentComponent;

		__forceinline auto
		IsDestroyed() const -> bool {
			return (flags & ComponentFlag::Destroyed) != 0;
		}
	};

	static_assert(sizeof(Component) == 0x48, "Component size is not 0x48");
	static_assert(offsetof(Component, flags) == 0x25, "Component flags offset is not 0x25");
	static_assert(offsetof(Component, updateFlags) == 0x40, "Component updateFlags offset is not 0x40");

	struct ActorGroup {
		EngineHandle *handles;
		const char *name;
		uint64_t unknown;
		uint16_t count;
		uint16_t capacity;
		uint16_t flags;
		uint16_t type;
	};

	static_assert(sizeof(ActorGroup) == 0x20, "ActorGroup size is not 0x20");

#pragma pack(pop)
} // namespace rivet_hook::game

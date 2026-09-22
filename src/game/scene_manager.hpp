// SPDX-FileCopyrightText: 2025-2026 Neptuwunium
//
// SPDX-License-Identifier: EUPL-1.2

#pragma once

#include <cstdint>

#include "actor.hpp"

namespace rivet_hook::game {
#pragma pack(push, 1)

	struct SceneComponent {
		Component* component;
		uint32_t generation;
		int32_t lookupIndex;
	};

	static_assert(sizeof(SceneComponent) == 0x10, "SceneComponent size is not 0x10");

	// every live component with its class, packed: entries past
	// componentLookupCount are stale
	struct ComponentLookup {
		Component* component;
		const ComponentInfo* type;
	};

	static_assert(sizeof(ComponentLookup) == 0x10, "ComponentLookup size is not 0x10");

	// the *Max fields bound the arrays, the *Count fields are how many are live.
	// live entries are scattered through the array, so walks go to Max and skip
	// the free slots.
	struct SceneManager {
		uint8_t unknown[0x1398];
		SceneComponent* components;
		uint8_t unknown2[0x10];
		int32_t componentCount;
		int32_t componentMaxAllocated;
		int32_t componentMax;
		uint8_t unknown3[0xc];
		ComponentLookup* componentLookups;
		uint8_t unknown3b[0x1c];
		int32_t componentLookupCount;
		uint8_t unknown3c[0x8];
		Actor* actors;
		uint8_t unknown4[0x10];
		int32_t actorCount;
		int32_t actorMax;
		uint8_t unknown5[0x6100];
		ActorGroup* actorGroups;
		uint8_t unknown6[0x10];
		int32_t actorGroupCount;
		int32_t actorGroupMax;

		__forceinline auto
		ResolveComponent(const EngineHandle handle) const -> Component* {
			if (static_cast<int32_t>(handle.id) >= componentMax) {
				return nullptr;
			}

			const auto sceneComponent = components[handle.id];
			if (sceneComponent.generation != handle.generation) {
				return nullptr;
			}

			return sceneComponent.component;
		}

		__forceinline auto
		ResolveActor(const EngineHandle handle) const -> Actor* {
			if (handle.generation == 0 || static_cast<int32_t>(handle.id) >= actorMax) {
				return nullptr;
			}

			const auto actor = &actors[handle.id];
			if (actor->generation != handle.generation) {
				return nullptr;
			}

			return actor;
		}

		__forceinline auto
		ResolveActorGroup(const EngineHandle handle) const -> ActorGroup* {
			if (handle.generation == 0 || static_cast<int32_t>(handle.id) >= actorGroupMax) {
				return nullptr;
			}

			const auto actorGroup = &actorGroups[handle.id];
			if (actorGroup->type != handle.generation) {
				return nullptr;
			}

			return actorGroup;
		}
	};

	static_assert(offsetof(SceneManager, components) == 0x1398, "SceneManager components offset is not 0x1398");
	static_assert(offsetof(SceneManager, componentCount) == 0x13b0, "SceneManager componentCount offset is not 0x13b0");
	static_assert(offsetof(SceneManager, componentMax) == 0x13b8, "SceneManager componentMax offset is not 0x13b8");
	static_assert(offsetof(SceneManager, componentLookups) == 0x13c8, "SceneManager componentLookups offset is not 0x13c8");
	static_assert(offsetof(SceneManager, componentLookupCount) == 0x13ec, "SceneManager componentLookupCount offset is not 0x13ec");
	static_assert(offsetof(SceneManager, actors) == 0x13f8, "SceneManager actors offset is not 0x13f8");
	static_assert(offsetof(SceneManager, actorCount) == 0x1410, "SceneManager actorCount offset is not 0x1410");
	static_assert(offsetof(SceneManager, actorMax) == 0x1414, "SceneManager actorMax offset is not 0x1414");
	static_assert(offsetof(SceneManager, actorGroups) == 0x7518, "SceneManager actorGroups offset is not 0x7518");
	static_assert(offsetof(SceneManager, actorGroupCount) == 0x7530, "SceneManager actorGroupCount offset is not 0x7530");
	static_assert(offsetof(SceneManager, actorGroupMax) == 0x7534, "SceneManager actorGroupMax offset is not 0x7534");

#pragma pack(pop)
} // namespace rivet_hook::game

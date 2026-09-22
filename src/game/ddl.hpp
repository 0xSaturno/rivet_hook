// SPDX-FileCopyrightText: 2025-2026 Neptuwunium
//
// SPDX-License-Identifier: EUPL-1.2

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <string>

namespace rivet_hook::game {
#pragma pack(push, 1)
	using ddl_call_t = void *(void *);

	struct DDLTypeInfo {
		const char *name;
		uint32_t type_id;
		uint32_t function_id;
		uint32_t parent_id;
		uint32_t allocation_size;
		int16_t field_count;
		std::array<uint8_t, 38> unknowns;
		const void **field_ex;
		uint32_t *field_ids;
		const char **field_names;
		uint8_t *field_types;
		uint8_t *field_array_types;
		uint8_t *field_map_types;
		uint32_t *field_array_sizes;
		uint32_t *field_offsets;
		uint32_t *field_type_ids;
		const char **field_descriptions;
		const char **field_labels;
		const char **field_names2;
		uint32_t footer_unknown1;
		uint32_t footer_unknown2;
		uint64_t footer_unknown3;
		uint64_t footer_unknown4;
		ddl_call_t *constructor_ptr;	// sets vtable
		ddl_call_t *copy_ptr;			// seems to copy data
		ddl_call_t *init_defaults_ptr;	// calls allocators
		ddl_call_t *reset_defaults_ptr; // also calls deallocators
		ddl_call_t *reset_field_ptr;	// resets specific index
		ddl_call_t *destructor_ptr;		// calls deallocators
		ddl_call_t *hash_test_ptr;		// compares hashes?
	};

	struct DDLBitSetTypeInfo {
		uint32_t count;
		uint32_t unknown1;
		intptr_t unknown2;
		intptr_t unknown3;
		uint32_t *values;
		const char **names;
		uint32_t *ids;
	};

	struct DDLTypeSelectInfo {
		uint32_t type_id;
		uint32_t count;
		intptr_t unknown1;
		intptr_t unknown2;
		uint32_t *ids;
		const char **names;
		const char **descriptions;
		const char **labels;
	};

	struct DDLSelectTypeInfo {
		const DDLTypeSelectInfo *select_info;
	};

	struct DDLTypeDescriptor;

	struct DDLTypeDescriptor {
		intptr_t **vtable;
		const char *name;
		uint32_t type_id;
		uint32_t index;
		const DDLTypeDescriptor **parent;
	};

	struct DDLHashMap {
		intptr_t *buckets;
		const DDLTypeInfo **values;
		uint32_t count;
		uint32_t capacity;
	};

	struct DDLRuntimeString {
		const char *value;
		int32_t length;
		uint32_t hash;
	};

	static_assert(sizeof(DDLRuntimeString) == 0x10, "DDLRuntimeString size is not 0x10");

	// asset_id is packed directly after length, unaligned, and the struct is padded
	// out to 0x18 at the end. live field offsets put consecutive file fields 0x18
	// apart, and reading asset_id at +0x10 instead yields ids with a zero high half.
	struct DDLRuntimeFile {
		const char *value;
		int32_t length;
		uint64_t asset_id;
		uint32_t padding;
	};

	static_assert(sizeof(DDLRuntimeFile) == 0x18, "DDLRuntimeFile size is not 0x18");

	struct ComponentPriusInfo {
		ddl_call_t *init;
		ddl_call_t *json;
		ddl_call_t *copy;
		ddl_call_t *create;
		ddl_call_t *destroy;
		int64_t size;
	};

	// renders the set bits of a flag word by name, "bitN" for the ones without one
	inline auto
	DescribeFlags(const uint32_t value, const char *const *names, const size_t count) -> std::string {
		std::string text;
		for (uint32_t bit = 0; bit < 32; ++bit) {
			if ((value & (1u << bit)) == 0) {
				continue;
			}

			if (!text.empty()) {
				text += '|';
			}

			if (bit < count && names[bit] != nullptr) {
				text += names[bit];
			} else {
				text += "bit" + std::to_string(bit);
			}
		}

		return text;
	}

	// ComponentInfo::class_flags. bits 0-5 are the six update stages, in slot order
	constexpr const char *COMPONENT_CLASS_FLAG_NAMES[] = {
		"HasUpdateFirst",
		"HasUpdateFirstResults",
		"HasUpdateMiddle",
		"HasUpdateLast",
		"HasUpdateAsync",
		"HasUpdateAsyncResults",
		"HasDerivedClasses",
		"AllowMultiple",
		"IsBaseOnly",
		"CacheQueries",
		"OnActivateThreadSafe",
		"OnDeactivateThreadSafe",
		"OnDestroyThreadSafe",
		"HasDerivedDebugDisplay",
	};

	// how the component's prius is held. a ReadOnly prius is one allocation
	// shared by every instance, so writing it changes all of them.
	enum class PriusBehavior : uint8_t {
		ReadOnly = 0,
		Local = 1,
		ReadWrite = 2,
	};

	constexpr auto
	PriusBehaviorName(const PriusBehavior behavior) -> const char * {
		switch (behavior) {
			case PriusBehavior::ReadOnly: return "ReadOnly";
			case PriusBehavior::Local: return "Local";
			case PriusBehavior::ReadWrite: return "ReadWrite";
			default: return "Unknown";
		}
	}

	struct ComponentInfo {
		ddl_call_t *update_first;
		ddl_call_t *update_first_results;
		ddl_call_t *update_middle;
		ddl_call_t *update_last;
		ddl_call_t *update_async;
		ddl_call_t *update_async_results;
		ComponentPriusInfo prius_info;
		const char *name;
		void *sort_helper;
		void *sync_class;
		int32_t size;
		uint32_t id; // name hash
		// the full ancestor chain, parent_count entries long
		const ComponentInfo *parent_classes[9];
		DDLTypeInfo *prius;
		ddl_call_t *create;
		int32_t cache_index_offset;
		uint16_t index; // registry index
		uint16_t update_order; // lower runs earlier
		uint16_t cache_index;
		uint16_t block_count;
		uint16_t class_flags; // COMPONENT_CLASS_FLAG_NAMES
		uint8_t parent_count;
		PriusBehavior prius_behavior;

		auto
		DerivesFrom(const ComponentInfo *other) const -> bool {
			for (uint8_t i = 0; i < parent_count && i < std::size(parent_classes); ++i) {
				if (parent_classes[i] == other) {
					return true;
				}
			}

			return false;
		}
	};

	static_assert(offsetof(ComponentInfo, name) == 0x60, "ComponentInfo name offset is not 0x60");
	static_assert(offsetof(ComponentInfo, parent_classes) == 0x80, "ComponentInfo parent_classes offset is not 0x80");
	static_assert(offsetof(ComponentInfo, prius) == 0xc8, "ComponentInfo prius offset is not 0xc8");
	static_assert(offsetof(ComponentInfo, cache_index) == 0xe0, "ComponentInfo cache_index offset is not 0xe0");
	static_assert(offsetof(ComponentInfo, class_flags) == 0xe4, "ComponentInfo class_flags offset is not 0xe4");
	static_assert(sizeof(ComponentInfo) == 0xe8, "ComponentInfo size is not 0xe8");

#pragma pack(pop)
} // namespace rivet_hook::game

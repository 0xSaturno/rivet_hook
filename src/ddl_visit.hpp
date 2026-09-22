// SPDX-FileCopyrightText: 2025-2026 Neptuwunium
//
// SPDX-License-Identifier: EUPL-1.2

#pragma once

#include <cstdint>

#include <nlohmann/json.hpp>

#include "game/ddl.hpp"

namespace rivet_hook::ddl {
	// raw values of DDLTypeInfo::field_types
	enum class FieldType : uint8_t {
		U8 = 0,
		U16 = 1,
		U32 = 2,
		U64 = 3,
		I8 = 4,
		I16 = 5,
		I32 = 6,
		I64 = 7,
		F32 = 8,
		F64 = 9,
		String = 10,
		Enum = 11,
		BitSet = 12,
		Struct = 13,
		Bool = 15,
		File = 16,
		TUID = 17,
		Instance = 20,
	};

	// raw values of DDLTypeInfo::field_array_types
	enum class ArrayType : uint8_t {
		Scalar = 0,
		FixedArray = 1,
		DynamicArray = 2,
		Map = 3,
	};

	// which member of a Value's payload is live
	enum class ValueKind : uint8_t {
		None, // null, unreadable, or a field type the walk does not decode
		Unsigned,
		Signed,
		Real,
		Bool,
		String, // text, text_id is the string hash
		File,	// text, text_id is the asset id
	};

	auto
	kind_of(FieldType type) -> ValueKind;

	// one scalar read out of a live DDL object
	struct Value {
		FieldType type {};
		ValueKind kind = ValueKind::None;

		union {
			uint64_t as_unsigned = 0;
			int64_t as_signed;
			double as_real;
			bool as_bool;
		};

		const char *text = nullptr;
		uint64_t text_id = 0;
	};

	auto
	none_value(FieldType type) -> Value;

	// the field being walked. owner and index locate it in the type's field arrays.
	struct Field {
		const game::DDLTypeInfo *owner = nullptr;
		int32_t index = 0;
		FieldType type {};
		ArrayType array_type = ArrayType::Scalar;

		auto
		name() const -> const char *;
	};

	// sink for the walk. OnValue is the only required override, the rest bracket
	// the element calls for arrays and maps.
	struct Visitor {
		Visitor() = default;
		Visitor(const Visitor &) = delete;
		Visitor(Visitor &&) = delete;
		virtual ~Visitor() = default;

		auto
		operator=(const Visitor &) -> Visitor & = delete;

		auto
		operator=(Visitor &&) -> Visitor & = delete;

		// a scalar field, or one element of an array or map
		virtual auto
		OnValue(const Field &field, const Value &value) -> void = 0;

		// brackets the OnValue calls for a fixed array. returning false skips the elements.
		virtual auto
		BeginArray(const Field &, int32_t) -> bool {
			return true;
		}

		virtual auto
		EndArray(const Field &) -> void { }

		// brackets a map. each entry is an OnMapKey followed by an OnValue.
		virtual auto
		BeginMap(const Field &, int32_t) -> bool {
			return true;
		}

		virtual auto
		OnMapKey(const Field &, const Value &) -> void { }

		virtual auto
		EndMap(const Field &) -> void { }

		// the field holds data the walk cannot decode. no OnValue follows.
		virtual auto
		OnUnhandled(const Field &, const char *) -> void { }
	};

	// reads one field out of a live instance and reports it to the visitor.
	// object points at the instance, offset is the field's byte offset into it.
	auto
	visit_field(Visitor &visitor, const uint8_t *object, uint32_t offset, const Field &field, int32_t index = 0) -> void;

	// reads every field of a live instance of type_info.
	auto
	visit(Visitor &visitor, const game::DDLTypeInfo *type_info, const uint8_t *object) -> void;

	// the json a scalar is dumped as
	auto
	to_json(const Value &value) -> nlohmann::json;

	// true when [ptr, ptr + size) is committed, readable, and inside a single
	// region. the dump owns the instance it walks, but anything reading a live
	// engine pointer must check it first.
	auto
	is_readable(const void *ptr, size_t size) -> bool;

	// is_readable without the frame cache. that cache is only safe on the thread
	// that resets it, so anything running on an engine worker thread - a component
	// update detour, say - has to ask the kernel directly instead.
	auto
	is_readable_uncached(const void *ptr, size_t size) -> bool;

	// the same, for pages that also accept a store. a write into live engine data
	// has to ask this separately: plenty of readable engine memory is mapped read
	// only, and a blind store into it faults instead of failing.
	auto
	is_writable(const void *ptr, size_t size) -> bool;

	// drops the cached readable regions is_readable keeps. called once a frame
	// so a cached entry can never outlive the frame it was taken in.
	auto
	reset_readable_cache() -> void;

	// copies a nul terminated string without ever reading past the end of its
	// memory region, so a stale pointer truncates instead of crashing.
	// returns false when the pointer is not readable at all.
	auto
	read_string(const char *text, char *out, size_t out_size) -> bool;

	// space separated hex bytes, for raw layout checks. reads nothing it has not
	// confirmed readable, and returns an empty string if it cannot read at all.
	auto
	hex_dump(const uint8_t *object, uint32_t size) -> std::string;

	// the registered DDL type with this id, or null. resolved from the engine's
	// own type hash map on first use, so struct fields can be sized and walked.
	auto
	find_type(uint32_t type_id) -> const game::DDLTypeInfo *;

	// Component::ddlPriusData does not point at the instance. it points at a small
	// holder, { void *vtable; int32_t count; T *data; }, and the fields the type
	// describes live behind data.
	constexpr uint32_t PRIUS_HOLDER_COUNT = 8;
	constexpr uint32_t PRIUS_HOLDER_DATA = 16;
	constexpr uint32_t PRIUS_HOLDER_SIZE = PRIUS_HOLDER_DATA + sizeof(void *);

	// how many instances the holder carries, or 0 when it is not readable. count is
	// 32 bit: reading it as 64 picks up the neighbouring field, which on
	// ConduitComponentPrius is the ascii "acte".
	auto
	prius_count(const void *holder) -> int32_t;

	// the instance a prius type's field offsets apply to, or null. allocation_size
	// is the type's, and the whole instance is checked before it is handed back.
	auto
	prius_data(const void *holder, uint32_t allocation_size) -> uint8_t *;

	// the Field descriptor for one index of a type. the caller has already bounds
	// checked index against field_count.
	auto
	field_at(const game::DDLTypeInfo *type_info, int32_t index) -> Field;

	// index of the field with this exact name, or -1.
	auto
	find_field(const game::DDLTypeInfo *type_info, const char *name) -> int32_t;

	// one scalar out of a live instance, without going through a visitor. element
	// selects an entry of a fixed array; dynamic arrays and maps are not read this
	// way and come back as a None value.
	auto
	read_field(const game::DDLTypeInfo *type_info, const uint8_t *object, int32_t index, int32_t element = 0) -> Value;

	// writes one scalar back into a live instance, converted to the field's own
	// width. only the numeric and bool field types are writable: a string or file
	// field stores a pointer whose target would have to outlive the write, and a
	// 64 bit id does not survive a round trip through a double. on failure reason
	// receives a short static explanation.
	auto
	write_field(const game::DDLTypeInfo *type_info, uint8_t *object, int32_t index, int32_t element, const Value &value, const char **reason) -> bool;

	// every field of a live instance as json, with metadata and decoded value,
	// plus the enum and bitset option tables behind field_ex.
	auto
	dump_object(const game::DDLTypeInfo *type_info, const uint8_t *object, int depth = 0) -> nlohmann::json;

	// writes the "default" key the ddl dump emits for a field
	struct JsonVisitor final : Visitor {
		explicit JsonVisitor(nlohmann::json &field) : field_(field) { }
		JsonVisitor(JsonVisitor &&) = delete; // field_ is a reference

		auto
		OnValue(const Field &field, const Value &value) -> void override;

		auto
		BeginArray(const Field &field, int32_t count) -> bool override;

		auto
		EndArray(const Field &field) -> void override;

		auto
		BeginMap(const Field &field, int32_t count) -> bool override;

		auto
		OnMapKey(const Field &field, const Value &key) -> void override;

		auto
		EndMap(const Field &field) -> void override;

		auto
		OnUnhandled(const Field &field, const char *reason) -> void override;

	private:
		nlohmann::json &field_;
		nlohmann::json::array_t elements_ {};
		nlohmann::json pending_key_ {};
		bool in_array_ = false;
		bool in_map_ = false;
	};
} // namespace rivet_hook::ddl

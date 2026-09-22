// SPDX-FileCopyrightText: 2025-2026 Neptuwunium
//
// SPDX-License-Identifier: EUPL-1.2

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>

#include "ddl_visit.hpp"

#include "runtime.hpp"
#include "signature.hpp"

using namespace rivet_hook::game;

namespace rivet_hook::ddl {
	auto
	kind_of(const FieldType type) -> ValueKind {
		switch (type) {
			case FieldType::U8:
			case FieldType::U16:
			case FieldType::U32:
			case FieldType::U64:
			case FieldType::Enum:
			case FieldType::BitSet:
			case FieldType::TUID:
			case FieldType::Instance: return ValueKind::Unsigned;
			case FieldType::I8:
			case FieldType::I16:
			case FieldType::I32:
			case FieldType::I64: return ValueKind::Signed;
			case FieldType::F32:
			case FieldType::F64: return ValueKind::Real;
			case FieldType::Bool: return ValueKind::Bool;
			case FieldType::String: return ValueKind::String;
			case FieldType::File: return ValueKind::File;
			default: return ValueKind::None;
		}
	}

	auto
	none_value(const FieldType type) -> Value {
		Value value {};
		value.type = type;
		value.kind = ValueKind::None;
		return value;
	}

	auto
	Field::name() const -> const char * {
		if (owner == nullptr || owner->field_names == nullptr) {
			return nullptr;
		}

		return owner->field_names[index];
	}

	static auto
	read_scalar(const uint8_t *object, const uint32_t offset, const FieldType type, const int32_t index) -> Value {
		Value value {};
		value.type = type;
		value.kind = kind_of(type);

		const auto *data = object + offset;

		switch (type) {
			case FieldType::U8: value.as_unsigned = data[index]; return value;
			case FieldType::U16: value.as_unsigned = reinterpret_cast<const uint16_t *>(data)[index]; return value;
			case FieldType::U32: value.as_unsigned = reinterpret_cast<const uint32_t *>(data)[index]; return value;
			case FieldType::U64: value.as_unsigned = reinterpret_cast<const uint64_t *>(data)[index]; return value;
			case FieldType::I8: value.as_signed = reinterpret_cast<const int8_t *>(data)[index]; return value;
			case FieldType::I16: value.as_signed = reinterpret_cast<const int16_t *>(data)[index]; return value;
			case FieldType::I32: value.as_signed = reinterpret_cast<const int32_t *>(data)[index]; return value;
			case FieldType::I64: value.as_signed = reinterpret_cast<const int64_t *>(data)[index]; return value;
			case FieldType::F32: value.as_real = reinterpret_cast<const float *>(data)[index]; return value;
			case FieldType::F64: value.as_real = reinterpret_cast<const double *>(data)[index]; return value;
			case FieldType::Enum:																						// enum
			case FieldType::BitSet: value.as_unsigned = reinterpret_cast<const uint32_t *>(data)[index]; return value;	// bitset
			case FieldType::Bool: value.as_bool = reinterpret_cast<const bool *>(data)[index]; return value;
			case FieldType::TUID:																						// tuid
			case FieldType::Instance: value.as_unsigned = reinterpret_cast<const uint64_t *>(data)[index]; return value; // instance
			case FieldType::String:
				{
					if (const auto str = reinterpret_cast<const DDLRuntimeString *>(data)[index]; str.value != nullptr) {
						value.text = str.value;
						value.text_id = str.hash;
					} else {
						value.kind = ValueKind::None;
					}

					return value;
				}
			case FieldType::File:
				{
					if (const auto str = reinterpret_cast<const DDLRuntimeFile *>(data)[index]; str.value != nullptr) {
						value.text = str.value;
						value.text_id = str.asset_id;
					} else {
						value.kind = ValueKind::None;
					}

					return value;
				}
			default: value.kind = ValueKind::None; return value;
		}
	}

	// stride of one element of a dynamic array. zero means the walk cannot size it,
	// which is every struct element type - those need the nested DDLTypeInfo.
	static auto
	element_size(const FieldType type) -> uint32_t {
		switch (type) {
			case FieldType::U8:
			case FieldType::I8:
			case FieldType::Bool: return 1;
			case FieldType::U16:
			case FieldType::I16: return 2;
			case FieldType::U32:
			case FieldType::I32:
			case FieldType::F32:
			case FieldType::Enum:
			case FieldType::BitSet: return 4;
			case FieldType::U64:
			case FieldType::I64:
			case FieldType::F64:
			case FieldType::TUID:
			case FieldType::Instance: return 8;
			case FieldType::String: return sizeof(DDLRuntimeString);
			case FieldType::File: return sizeof(DDLRuntimeFile);
			default: return 0;
		}
	}

	// a garbage count must not turn into a gigabyte of reads
	constexpr int32_t MAX_ARRAY_ELEMENTS = 4096;

	// nested structs can in principle reference each other
	constexpr int MAX_STRUCT_DEPTH = 6;

	// scalar read plus the diagnostic the dump logs for field types it does not decode
	static auto
	read_scalar_logged(const uint8_t *object, const uint32_t offset, const Field &field, const int32_t index) -> Value {
		const auto value = read_scalar(object, offset, field.type, index);

		if (kind_of(field.type) == ValueKind::None && g_settings.ddl.debug_ddl && (object + offset)[index] != 0 && field.owner != nullptr) {
			g_output << "[DDL] " << field.owner->name << " field " << field.name() << " (index " << index << ", type " << static_cast<int>(field.type)
					 << ") has non-zero value that is not handled\n";
		}

		return value;
	}

	auto
	visit_field(Visitor &visitor, const uint8_t *object, const uint32_t offset, const Field &field, const int32_t index) -> void {
		if (field.array_type == ArrayType::Scalar) {
			visitor.OnValue(field, read_scalar_logged(object, offset, field, index));
			return;
		}

		if (field.owner == nullptr || index != 0) {
			g_output << "[DDL] hit unreachable state";
			visitor.OnUnhandled(field, "unreachable state");
			return;
		}

		auto element = field;
		element.array_type = ArrayType::Scalar;

		if (field.array_type == ArrayType::FixedArray) {
			const auto count = static_cast<int32_t>(field.owner->field_array_sizes[field.index]);
			if (count <= 0) {
				visitor.OnValue(field, none_value(field.type));
				return;
			}

			if (!visitor.BeginArray(field, count)) {
				return;
			}

			for (int32_t array_index = 0; array_index < count; ++array_index) {
				visit_field(visitor, object, offset, element, array_index);
			}

			visitor.EndArray(field);
			return;
		}

		// { T *data; int32_t count; int32_t capacity }, confirmed against live
		// instances: element stride matches sizeof, and a file entry's length
		// field matches the string it points at exactly
		if (field.array_type == ArrayType::DynamicArray) {
			const auto count = reinterpret_cast<const int32_t *>(object + offset + sizeof(intptr_t) * 1)[0];
			if (count <= 0) {
				visitor.OnValue(field, none_value(field.type));
				return;
			}

			const auto *ptr_values = reinterpret_cast<const uint8_t *const *>(object + offset)[0];
			if (ptr_values == nullptr) {
				visitor.OnValue(field, none_value(field.type));
				return;
			}

			if (count > MAX_ARRAY_ELEMENTS) {
				visitor.OnUnhandled(field, "dynamic array count is implausible");
				return;
			}

			const auto stride = element_size(field.type);
			if (stride == 0) {
				// struct elements need the nested type behind field_ex to be sized
				visitor.OnUnhandled(field, "dynamic array of struct elements");
				return;
			}

			if (!is_readable(ptr_values, static_cast<size_t>(count) * stride)) {
				visitor.OnUnhandled(field, "dynamic array storage is not readable");
				return;
			}

			if (!visitor.BeginArray(field, count)) {
				return;
			}

			for (int32_t array_index = 0; array_index < count; ++array_index) {
				visit_field(visitor, ptr_values, 0, element, array_index);
			}

			visitor.EndArray(field);
			return;
		}

		if (field.array_type == ArrayType::Map) {
			const auto count = reinterpret_cast<const int32_t *>(object + offset + sizeof(intptr_t) * 2)[0];
			if (count <= 0) {
				visitor.OnValue(field, none_value(field.type));
				return;
			}

			const auto *const *ptrs = reinterpret_cast<const uint8_t *const *>(object + offset);
			const auto *ptr_keys = ptrs[0];
			const auto *ptr_values = ptrs[1];
			if (ptr_keys == nullptr || ptr_values == nullptr) {
				visitor.OnValue(field, none_value(field.type));
				return;
			}

			if (!visitor.BeginMap(field, count)) {
				return;
			}

			auto key = field;
			key.array_type = ArrayType::Scalar;
			key.type = static_cast<FieldType>(field.owner->field_map_types[field.index]);

			for (int32_t array_index = 0; array_index < count; ++array_index) {
				visitor.OnMapKey(field, read_scalar_logged(ptr_keys, 0, key, array_index));
				visit_field(visitor, ptr_values, 0, element, array_index);
			}

			visitor.EndMap(field);
			return;
		}

		if (g_settings.ddl.debug_ddl && reinterpret_cast<const uint64_t *>(object + offset)[0] != 0) {
			g_output << "[DDL] " << field.owner->name << " field " << field.name() << " (type " << static_cast<int>(field.type) << ", array type " << static_cast<int>(field.array_type)
					 << ") has non-zero value that is not handled\n";
		}

		visitor.OnUnhandled(field, "array type");
	}

	auto
	field_at(const DDLTypeInfo *type_info, const int32_t index) -> Field {
		Field field {};
		field.owner = type_info;
		field.index = index;
		field.type = static_cast<FieldType>(type_info->field_types[index]);
		field.array_type = static_cast<ArrayType>(type_info->field_array_types[index]);
		return field;
	}

	auto
	visit(Visitor &visitor, const DDLTypeInfo *type_info, const uint8_t *object) -> void {
		if (type_info == nullptr || object == nullptr) {
			return;
		}

		for (int32_t index = 0; index < type_info->field_count; ++index) {
			visit_field(visitor, object, type_info->field_offsets[index], field_at(type_info, index));
		}
	}

	auto
	find_field(const DDLTypeInfo *type_info, const char *name) -> int32_t {
		if (type_info == nullptr || type_info->field_names == nullptr || name == nullptr) {
			return -1;
		}

		for (int32_t index = 0; index < type_info->field_count; ++index) {
			char text[0x100];
			if (read_string(type_info->field_names[index], text, sizeof(text)) && strcmp(text, name) == 0) {
				return index;
			}
		}

		return -1;
	}

	auto
	read_field(const DDLTypeInfo *type_info, const uint8_t *object, const int32_t index, const int32_t element) -> Value {
		if (type_info == nullptr || object == nullptr || index < 0 || index >= type_info->field_count) {
			return none_value(FieldType::U8);
		}

		const auto field = field_at(type_info, index);
		if (field.array_type == ArrayType::Scalar) {
			return element == 0 ? read_scalar(object, type_info->field_offsets[index], field.type, 0) : none_value(field.type);
		}

		// a dynamic array or a map lives behind a pointer that only the visitor walk
		// knows how to bracket, so neither is read this way
		if (field.array_type != ArrayType::FixedArray || type_info->field_array_sizes == nullptr) {
			return none_value(field.type);
		}

		if (element < 0 || static_cast<uint32_t>(element) >= type_info->field_array_sizes[index]) {
			return none_value(field.type);
		}

		return read_scalar(object, type_info->field_offsets[index], field.type, element);
	}

	// widths of the field types a write is allowed to touch. string, file, struct
	// and the 64 bit id types are absent on purpose, see the header.
	static auto
	writable_width(const FieldType type) -> uint32_t {
		switch (type) {
			case FieldType::U8:
			case FieldType::I8:
			case FieldType::Bool: return 1;
			case FieldType::U16:
			case FieldType::I16: return 2;
			case FieldType::U32:
			case FieldType::I32:
			case FieldType::F32:
			case FieldType::Enum:
			case FieldType::BitSet: return 4;
			case FieldType::U64:
			case FieldType::I64:
			case FieldType::F64: return 8;
			default: return 0;
		}
	}

	// whatever the caller handed over, as one double
	static auto
	as_number(const Value &value) -> double {
		switch (value.kind) {
			case ValueKind::Unsigned: return static_cast<double>(value.as_unsigned);
			case ValueKind::Signed: return static_cast<double>(value.as_signed);
			case ValueKind::Real: return value.as_real;
			case ValueKind::Bool: return value.as_bool ? 1.0 : 0.0;
			default: return 0.0;
		}
	}

	static auto
	in_range(const double number, const double low, const double high) -> bool {
		return std::isfinite(number) && number >= low && number <= high;
	}

	// 2^53 is where a double stops counting by ones. past it a 64 bit target would
	// be storing a rounded value while reporting success.
	constexpr double EXACT_INTEGER_LIMIT = 9007199254740992.0;

	auto
	write_field(const DDLTypeInfo *type_info, uint8_t *object, const int32_t index, const int32_t element, const Value &value, const char **reason) -> bool {
		const auto fail = [reason](const char *text) {
			if (reason != nullptr) {
				*reason = text;
			}

			return false;
		};

		if (type_info == nullptr || object == nullptr) {
			return fail("no instance to write into");
		}

		if (index < 0 || index >= type_info->field_count) {
			return fail("field index is out of range");
		}

		const auto field = field_at(type_info, index);
		if (field.array_type == ArrayType::DynamicArray || field.array_type == ArrayType::Map) {
			return fail("only scalar and fixed array fields can be written");
		}

		if (field.array_type == ArrayType::FixedArray) {
			if (type_info->field_array_sizes == nullptr || element < 0 || static_cast<uint32_t>(element) >= type_info->field_array_sizes[index]) {
				return fail("array element is out of range");
			}
		} else if (element != 0) {
			return fail("that field is not an array");
		}

		const auto width = writable_width(field.type);
		if (width == 0) {
			return fail("that field type cannot be written");
		}

		if (value.kind == ValueKind::None || value.kind == ValueKind::String || value.kind == ValueKind::File) {
			return fail("only numbers and booleans can be written");
		}

		auto *at = object + type_info->field_offsets[index] + static_cast<size_t>(element) * width;
		if (!is_writable(at, width)) {
			return fail("the field is not in writable memory");
		}

		const auto number = as_number(value);
		switch (field.type) {
			case FieldType::Bool:
				{
					*reinterpret_cast<bool *>(at) = value.kind == ValueKind::Bool ? value.as_bool : number != 0.0;
					return true;
				}
			case FieldType::F32:
				{
					if (!std::isfinite(number)) {
						return fail("the value is not finite");
					}

					*reinterpret_cast<float *>(at) = static_cast<float>(number);
					return true;
				}
			case FieldType::F64:
				{
					if (!std::isfinite(number)) {
						return fail("the value is not finite");
					}

					*reinterpret_cast<double *>(at) = number;
					return true;
				}
			case FieldType::U8:
				{
					if (!in_range(number, 0, UINT8_MAX)) {
						return fail("the value does not fit the field");
					}

					*at = static_cast<uint8_t>(number);
					return true;
				}
			case FieldType::U16:
				{
					if (!in_range(number, 0, UINT16_MAX)) {
						return fail("the value does not fit the field");
					}

					*reinterpret_cast<uint16_t *>(at) = static_cast<uint16_t>(number);
					return true;
				}
			case FieldType::U32:
			case FieldType::Enum:
			case FieldType::BitSet:
				{
					if (!in_range(number, 0, UINT32_MAX)) {
						return fail("the value does not fit the field");
					}

					*reinterpret_cast<uint32_t *>(at) = static_cast<uint32_t>(number);
					return true;
				}
			case FieldType::U64:
				{
					if (!in_range(number, 0, EXACT_INTEGER_LIMIT)) {
						return fail("the value does not fit the field");
					}

					*reinterpret_cast<uint64_t *>(at) = static_cast<uint64_t>(number);
					return true;
				}
			case FieldType::I8:
				{
					if (!in_range(number, INT8_MIN, INT8_MAX)) {
						return fail("the value does not fit the field");
					}

					*reinterpret_cast<int8_t *>(at) = static_cast<int8_t>(number);
					return true;
				}
			case FieldType::I16:
				{
					if (!in_range(number, INT16_MIN, INT16_MAX)) {
						return fail("the value does not fit the field");
					}

					*reinterpret_cast<int16_t *>(at) = static_cast<int16_t>(number);
					return true;
				}
			case FieldType::I32:
				{
					if (!in_range(number, INT32_MIN, INT32_MAX)) {
						return fail("the value does not fit the field");
					}

					*reinterpret_cast<int32_t *>(at) = static_cast<int32_t>(number);
					return true;
				}
			case FieldType::I64:
				{
					if (!in_range(number, -EXACT_INTEGER_LIMIT, EXACT_INTEGER_LIMIT)) {
						return fail("the value does not fit the field");
					}

					*reinterpret_cast<int64_t *>(at) = static_cast<int64_t>(number);
					return true;
				}
			default: return fail("that field type cannot be written");
		}
	}

	auto
	prius_count(const void *holder) -> int32_t {
		if (!is_readable(holder, PRIUS_HOLDER_SIZE)) {
			return 0;
		}

		return *reinterpret_cast<const int32_t *>(static_cast<const uint8_t *>(holder) + PRIUS_HOLDER_COUNT);
	}

	auto
	prius_data(const void *holder, const uint32_t allocation_size) -> uint8_t * {
		if (!is_readable(holder, PRIUS_HOLDER_SIZE)) {
			return nullptr;
		}

		auto *data = *reinterpret_cast<uint8_t *const *>(static_cast<const uint8_t *>(holder) + PRIUS_HOLDER_DATA);
		if (!is_readable(data, allocation_size)) {
			return nullptr;
		}

		return data;
	}

	auto
	to_json(const Value &value) -> nlohmann::json {
		switch (value.kind) {
			case ValueKind::Unsigned: return value.as_unsigned;
			case ValueKind::Signed: return value.as_signed;
			case ValueKind::Real: return value.as_real;
			case ValueKind::Bool: return value.as_bool;
			case ValueKind::String:
			case ValueKind::File:
				{
					// never hand nlohmann a raw engine pointer: it strlens whatever
					// it is given, and a live instance can hold a stale one
					char buffer[0x400];
					nlohmann::json text;
					text["value"] = read_string(value.text, buffer, sizeof(buffer)) ? nlohmann::json(buffer) : nlohmann::json(nullptr);
					text["id"] = value.text_id;
					return text;
				}
			default: return nullptr;
		}
	}

	auto
	JsonVisitor::OnValue(const Field &, const Value &value) -> void {
		if (in_map_) {
			nlohmann::json entry;
			entry["key"] = pending_key_;
			entry["value"] = to_json(value);
			elements_.push_back(entry);
			return;
		}

		if (in_array_) {
			elements_.push_back(to_json(value));
			return;
		}

		field_["default"] = to_json(value);
	}

	auto
	JsonVisitor::BeginArray(const Field &, const int32_t) -> bool {
		elements_.clear();
		in_array_ = true;
		return true;
	}

	auto
	JsonVisitor::EndArray(const Field &) -> void {
		field_["default"] = elements_;
		elements_.clear();
		in_array_ = false;
	}

	auto
	JsonVisitor::BeginMap(const Field &, const int32_t) -> bool {
		elements_.clear();
		in_map_ = true;
		return true;
	}

	auto
	JsonVisitor::OnMapKey(const Field &, const Value &key) -> void {
		pending_key_ = to_json(key);
	}

	auto
	JsonVisitor::EndMap(const Field &) -> void {
		field_["default"] = elements_;
		elements_.clear();
		in_map_ = false;
	}

	// a field the walk refused is not the same as a field that is empty, and both
	// used to serialise as a bare null
	auto
	JsonVisitor::OnUnhandled(const Field &, const char *reason) -> void {
		field_["undecoded"] = reason != nullptr ? reason : "unknown";
	}

	// VirtualQuery measured ~0.15ms in a loaded level, because the address space
	// is fragmented enough that the kernel walks a large region tree. Calling it
	// per actor turned a scan into a multi second stall, so successful lookups
	// are cached. The cache is cleared once per frame by reset_readable_cache,
	// which bounds how stale an entry can be to the frame it was taken in.
	namespace {
		struct ReadableRegion {
			uintptr_t begin;
			uintptr_t end;
			bool writable;
		};

		constexpr int READABLE_CACHE_SIZE = 64;
		ReadableRegion g_readable_cache[READABLE_CACHE_SIZE] {};
		int g_readable_cached = 0;
		int g_readable_next = 0;
	} // namespace

	auto
	reset_readable_cache() -> void {
		g_readable_cached = 0;
		g_readable_next = 0;
	}

	// the committed readable region containing ptr, or null. the single point where
	// VirtualQuery is called, so is_readable, is_writable and read_string share the
	// cache rather than each paying their own lookup. the entry it returns points
	// into the ring buffer, so it is only good until the next call.
	static auto
	region_of(const void *ptr) -> const ReadableRegion * {
		if (ptr == nullptr) {
			return nullptr;
		}

		const auto begin = reinterpret_cast<uintptr_t>(ptr);
		for (auto i = 0; i < g_readable_cached; ++i) {
			if (begin >= g_readable_cache[i].begin && begin < g_readable_cache[i].end) {
				return &g_readable_cache[i];
			}
		}

		MEMORY_BASIC_INFORMATION mbi {};
		if (VirtualQuery(ptr, &mbi, sizeof(mbi)) == 0 || mbi.State != MEM_COMMIT) {
			return nullptr;
		}

		constexpr DWORD readable = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
		if ((mbi.Protect & readable) == 0 || (mbi.Protect & PAGE_GUARD) != 0) {
			return nullptr;
		}

		// a copy on write page accepts a store, but the store is private to this
		// process' copy and does not reach whatever else maps it. that is still the
		// behaviour a script write wants, so it counts as writable.
		constexpr DWORD writable = PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;

		const auto region_end = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;

		// only successes are cached: a failure is rare and must stay authoritative
		if (g_readable_cached < READABLE_CACHE_SIZE) {
			++g_readable_cached;
		}

		auto *entry = &g_readable_cache[g_readable_next];
		*entry = { reinterpret_cast<uintptr_t>(mbi.BaseAddress), region_end, (mbi.Protect & writable) != 0 };
		g_readable_next = (g_readable_next + 1) % READABLE_CACHE_SIZE;
		return entry;
	}

	// end of the region holding ptr, or 0
	static auto
	readable_region_end(const void *ptr) -> uintptr_t {
		const auto *region = region_of(ptr);
		return region == nullptr ? 0 : region->end;
	}

	auto
	is_readable(const void *ptr, const size_t size) -> bool {
		if (ptr == nullptr || size == 0) {
			return false;
		}

		const auto region_end = readable_region_end(ptr);
		return region_end != 0 && reinterpret_cast<uintptr_t>(ptr) + size <= region_end;
	}

	auto
	is_readable_uncached(const void *ptr, const size_t size) -> bool {
		if (ptr == nullptr || size == 0) {
			return false;
		}

		MEMORY_BASIC_INFORMATION mbi {};
		if (VirtualQuery(ptr, &mbi, sizeof(mbi)) == 0 || mbi.State != MEM_COMMIT) {
			return false;
		}

		constexpr DWORD readable = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
		if ((mbi.Protect & readable) == 0 || (mbi.Protect & PAGE_GUARD) != 0) {
			return false;
		}

		return reinterpret_cast<uintptr_t>(ptr) + size <= reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
	}

	auto
	is_writable(const void *ptr, const size_t size) -> bool {
		if (ptr == nullptr || size == 0) {
			return false;
		}

		const auto *region = region_of(ptr);
		return region != nullptr && region->writable && reinterpret_cast<uintptr_t>(ptr) + size <= region->end;
	}

	auto
	read_string(const char *text, char *out, const size_t out_size) -> bool {
		if (out == nullptr || out_size == 0) {
			return false;
		}

		out[0] = '\0';

		// never scan for the terminator past the end of the region holding it
		const auto region_end = readable_region_end(text);
		if (region_end == 0) {
			return false;
		}

		const auto available = static_cast<size_t>(region_end - reinterpret_cast<uintptr_t>(text));
		const auto limit = available < out_size - 1 ? available : out_size - 1;

		size_t length = 0;
		while (length < limit && text[length] != '\0') {
			++length;
		}

		memcpy(out, text, length);
		out[length] = '\0';
		return true;
	}

	auto
	find_type(const uint32_t type_id) -> const DDLTypeInfo * {
		static std::unordered_map<uint32_t, const DDLTypeInfo *> registry;
		static auto resolved = false;

		if (!resolved) {
			resolved = true;

			const auto pointer = find_address(DDL_HASH_MAP_SIGNATURE);
			if (pointer == 0) {
				g_output << "[DDL] could not resolve the type hash map, struct fields will not be walked\n";
				g_output.flush();
			} else if (const auto *map = static_cast<const DDLHashMap *>(load_rel_var(pointer, DDL_HASH_MAP_ADDRESS)); map != nullptr && map->values != nullptr) {
				for (auto i = 0u; i < map->capacity; ++i) {
					if (const auto *type = map->values[i]; type != nullptr) {
						registry.emplace(type->type_id, type);
					}
				}

				g_output << "[DDL] indexed " << registry.size() << " types for struct walking\n";
				g_output.flush();
			}
		}

		const auto entry = registry.find(type_id);
		return entry == registry.end() ? nullptr : entry->second;
	}

	auto
	hex_dump(const uint8_t *object, const uint32_t size) -> std::string {
		if (!is_readable(object, size)) {
			return {};
		}

		std::string hex;
		hex.reserve(static_cast<size_t>(size) * 3);
		for (auto i = 0u; i < size; ++i) {
			char byte[4];
			sprintf_s(byte, "%02x ", object[i]);
			hex += byte;
		}

		return hex;
	}

	// bounded read of an engine string for json. every name, label and
	// description in a live type is a pointer we do not own.
	static auto
	safe_text(const char *text) -> nlohmann::json {
		char buffer[0x400];
		if (!read_string(text, buffer, sizeof(buffer))) {
			return nullptr;
		}

		return buffer;
	}

	// the option table behind an enum or bitset field, so a value that fails to
	// resolve in game can still be diagnosed from the dump
	static auto
	dump_field_ex(nlohmann::json &entry, const DDLTypeInfo *type_info, const int32_t index) -> void {
		if (type_info->field_ex == nullptr) {
			return;
		}

		const auto *extra = type_info->field_ex[index];
		if (extra == nullptr) {
			return;
		}

		const auto type = static_cast<FieldType>(type_info->field_types[index]);

		if (type == FieldType::Enum) {
			const auto *select = static_cast<const DDLSelectTypeInfo *>(extra);
			if (select->select_info == nullptr) {
				return;
			}

			const auto *info = select->select_info;
			nlohmann::json options;
			options["type_id"] = info->type_id;
			options["count"] = info->count;

			nlohmann::json::array_t values;
			for (auto i = 0u; i < info->count && info->ids != nullptr && info->names != nullptr; ++i) {
				nlohmann::json option;
				option["id"] = info->ids[i];
				option["name"] = safe_text(info->names[i]);
				values.emplace_back(option);
			}

			options["values"] = values;
			entry["enum"] = options;
			return;
		}

		if (type == FieldType::BitSet) {
			const auto *bits = static_cast<const DDLBitSetTypeInfo *>(extra);
			nlohmann::json options;
			options["count"] = bits->count;

			nlohmann::json::array_t values;
			for (auto i = 0u; i < bits->count && bits->values != nullptr && bits->names != nullptr; ++i) {
				nlohmann::json option;
				option["value"] = bits->values[i];
				option["id"] = bits->ids != nullptr ? bits->ids[i] : 0;
				option["name"] = safe_text(bits->names[i]);
				values.emplace_back(option);
			}

			options["values"] = values;
			entry["bitset"] = options;
			return;
		}

		if (type == FieldType::Struct) {
			const auto *nested = static_cast<const DDLTypeInfo *>(extra);
			nlohmann::json info;
			info["name"] = safe_text(nested->name);
			info["id"] = nested->type_id;
			entry["struct"] = info;
		}
	}

	// struct fields hold nested DDL objects, laid out exactly like a prius instance:
	// a vtable at 0 and fields at their declared offsets, sized by allocation_size.
	// verified live against WaterImpulseData, whose elements repeat every 32 bytes
	// with a shared vtable.
	static auto
	dump_struct_field(nlohmann::json &entry, const DDLTypeInfo *type_info, const uint8_t *object, const int32_t index, const int depth) -> void {
		const auto *nested = find_type(type_info->field_type_ids[index]);
		if (nested == nullptr) {
			entry["undecoded"] = "struct type is not registered";
			return;
		}

		entry["struct"] = nlohmann::json { { "name", safe_text(nested->name) }, { "id", nested->type_id }, { "size", nested->allocation_size } };

		const auto *at = object + type_info->field_offsets[index];
		const auto array_type = static_cast<ArrayType>(type_info->field_array_types[index]);

		if (array_type == ArrayType::Scalar) {
			if (!is_readable(at, nested->allocation_size)) {
				entry["undecoded"] = "struct storage is not readable";
				return;
			}

			entry["value"] = dump_object(nested, at, depth + 1);
			entry.erase("undecoded");
			return;
		}

		if (array_type != ArrayType::DynamicArray) {
			return;
		}

		const auto count = reinterpret_cast<const int32_t *>(at + sizeof(intptr_t))[0];
		const auto *data = reinterpret_cast<const uint8_t *const *>(at)[0];
		if (count <= 0 || data == nullptr) {
			return;
		}

		if (count > MAX_ARRAY_ELEMENTS || !is_readable(data, static_cast<size_t>(count) * nested->allocation_size)) {
			entry["undecoded"] = "struct array storage is not readable";
			return;
		}

		nlohmann::json::array_t elements;
		for (int32_t i = 0; i < count; ++i) {
			elements.emplace_back(dump_object(nested, data + static_cast<size_t>(i) * nested->allocation_size, depth + 1));
		}

		entry["value"] = elements;
		entry.erase("undecoded");
	}

	auto
	dump_object(const DDLTypeInfo *type_info, const uint8_t *object, const int depth) -> nlohmann::json {
		nlohmann::json out;
		if (type_info == nullptr) {
			return out;
		}

		out["name"] = safe_text(type_info->name);
		out["id"] = type_info->type_id;
		out["parent_id"] = type_info->parent_id;
		out["size"] = type_info->allocation_size;
		out["field_count"] = type_info->field_count;

		if (object == nullptr) {
			return out;
		}

		// the first bytes of the instance, unmodified, for layout checks
		out["raw_header"] = hex_dump(object, type_info->allocation_size < 64 ? type_info->allocation_size : 64u);

		nlohmann::json::array_t fields;
		for (int32_t index = 0; index < type_info->field_count; ++index) {
			nlohmann::json entry;
			entry["name"] = type_info->field_names != nullptr ? safe_text(type_info->field_names[index]) : nlohmann::json(nullptr);
			entry["id"] = type_info->field_ids != nullptr ? type_info->field_ids[index] : 0;
			entry["type"] = type_info->field_types[index];
			entry["array_type"] = type_info->field_array_types[index];
			entry["map_type"] = type_info->field_map_types != nullptr ? type_info->field_map_types[index] : 0;
			entry["offset"] = type_info->field_offsets[index];
			entry["type_id"] = type_info->field_type_ids != nullptr ? type_info->field_type_ids[index] : 0;
			entry["fixed_size"] = type_info->field_array_sizes != nullptr ? type_info->field_array_sizes[index] : 0;

			if (type_info->field_descriptions != nullptr && type_info->field_descriptions[index] != nullptr) {
				entry["description"] = safe_text(type_info->field_descriptions[index]);
			}

			if (type_info->field_labels != nullptr && type_info->field_labels[index] != nullptr) {
				entry["label"] = safe_text(type_info->field_labels[index]);
			}

			dump_field_ex(entry, type_info, index);

			const auto field = field_at(type_info, index);

			nlohmann::json decoded;
			JsonVisitor visitor { decoded };
			visit_field(visitor, object, type_info->field_offsets[index], field);
			entry["value"] = decoded.contains("default") ? decoded["default"] : nlohmann::json(nullptr);
			if (decoded.contains("undecoded")) {
				entry["undecoded"] = decoded["undecoded"];
			}

			// dynamic arrays are { T *data; int32_t count; int32_t capacity }. the walk
			// does not read the elements yet, but the shape is worth recording.
			if (field.array_type == ArrayType::DynamicArray) {
				const auto *at = object + type_info->field_offsets[index];
				nlohmann::json array;
				char pointer[32];
				sprintf_s(pointer, "0x%016llx", *reinterpret_cast<const uint64_t *>(at));
				array["data"] = pointer;
				array["count"] = *reinterpret_cast<const int32_t *>(at + sizeof(void *));
				array["capacity"] = *reinterpret_cast<const int32_t *>(at + sizeof(void *) + sizeof(int32_t));
				entry["array"] = array;
			}

			// the raw bytes, so a value the walk misreads can still be recovered
			if (const auto offset = type_info->field_offsets[index]; offset + sizeof(uint64_t) <= type_info->allocation_size) {
				char raw[32];
				sprintf_s(raw, "0x%016llx", *reinterpret_cast<const uint64_t *>(object + offset));
				entry["raw"] = raw;
			}

			// nested structs, single or arrayed, that the scalar walk cannot size
			if (field.type == FieldType::Struct && depth < MAX_STRUCT_DEPTH) {
				dump_struct_field(entry, type_info, object, index, depth);
			}

			fields.emplace_back(entry);
		}

		out["fields"] = fields;
		return out;
	}
} // namespace rivet_hook::ddl

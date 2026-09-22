// SPDX-FileCopyrightText: 2025-2026 Neptuwunium
//
// SPDX-License-Identifier: EUPL-1.2

#include <cstdio>
#include <cstring>
#include <fstream>

#include <imgui.h>

#include "ddl_inspector.hpp"

#include "ddl_visit.hpp"
#include "runtime.hpp"

using namespace rivet_hook::game;

namespace rivet_hook {
	// field_ex[index] is a DDLSelectTypeInfo for enum fields
	static auto
	EnumName(const ddl::Field &field, const uint64_t value) -> const char * {
		if (field.owner == nullptr || field.owner->field_ex == nullptr) {
			return nullptr;
		}

		const auto *extra = static_cast<const DDLSelectTypeInfo *>(field.owner->field_ex[field.index]);
		if (extra == nullptr || extra->select_info == nullptr || extra->select_info->ids == nullptr || extra->select_info->names == nullptr) {
			return nullptr;
		}

		// the stored value is an ordinal into the option table, not one of the
		// name hashes in ids[] - live values are 0, 1, 4 while the ids are crc32s
		const auto *info = extra->select_info;
		if (value >= info->count) {
			return nullptr;
		}

		return info->names[value];
	}

	// field_ex[index] is a DDLBitSetTypeInfo for bitset fields
	static auto
	BitSetNames(const ddl::Field &field, const uint64_t value, char *out, const size_t size) -> bool {
		if (field.owner == nullptr || field.owner->field_ex == nullptr) {
			return false;
		}

		const auto *extra = static_cast<const DDLBitSetTypeInfo *>(field.owner->field_ex[field.index]);
		if (extra == nullptr || extra->values == nullptr || extra->names == nullptr) {
			return false;
		}

		out[0] = '\0';
		auto written = false;
		for (auto i = 0u; i < extra->count; ++i) {
			if ((value & extra->values[i]) == 0 || extra->names[i] == nullptr) {
				continue;
			}

			if (written) {
				strcat_s(out, size, " | ");
			}

			strcat_s(out, size, extra->names[i]);
			written = true;
		}

		return written;
	}

	static auto
	FormatValue(const ddl::Field &field, const ddl::Value &value, char *out, const size_t size) -> void {
		switch (value.kind) {
			case ddl::ValueKind::Unsigned:
				{
					if (value.type == ddl::FieldType::Enum) {
						if (const auto *name = EnumName(field, value.as_unsigned); name != nullptr) {
							_snprintf_s(out, size, _TRUNCATE, "%s (%llu)", name, value.as_unsigned);
							return;
						}

						// unresolved: show the hash form too, the json dump carries the option table
						_snprintf_s(out, size, _TRUNCATE, "%llu (0x%08llx, unresolved)", value.as_unsigned, value.as_unsigned);
						return;
					}

					if (value.type == ddl::FieldType::BitSet) {
						char flags[0x200];
						if (BitSetNames(field, value.as_unsigned, flags, sizeof(flags))) {
							_snprintf_s(out, size, _TRUNCATE, "%s (0x%llx)", flags, value.as_unsigned);
							return;
						}
					}

					if (value.type == ddl::FieldType::TUID || value.type == ddl::FieldType::Instance) {
						_snprintf_s(out, size, _TRUNCATE, "0x%016llx", value.as_unsigned);
						return;
					}

					_snprintf_s(out, size, _TRUNCATE, "%llu", value.as_unsigned);
					return;
				}
			case ddl::ValueKind::Signed: _snprintf_s(out, size, _TRUNCATE, "%lld", value.as_signed); return;
			case ddl::ValueKind::Real: _snprintf_s(out, size, _TRUNCATE, "%g", value.as_real); return;
			case ddl::ValueKind::Bool: _snprintf_s(out, size, _TRUNCATE, "%s", value.as_bool ? "true" : "false"); return;
			case ddl::ValueKind::String:
			case ddl::ValueKind::File:
				{
					const auto *noun = value.kind == ddl::ValueKind::String ? "id" : "asset";

					char text[0x180];
					if (!ddl::read_string(value.text, text, sizeof(text))) {
						_snprintf_s(out, size, _TRUNCATE, "<unreadable %p> (%s 0x%016llx)", value.text, noun, value.text_id);
						return;
					}

					_snprintf_s(out, size, _TRUNCATE, "\"%s\" (%s 0x%016llx)", text, noun, value.text_id);
					return;
				}
			default: _snprintf_s(out, size, _TRUNCATE, "null"); return;
		}
	}

	static auto
	FieldTooltip(const ddl::Field &field) -> void {
		if (field.owner == nullptr || !ImGui::IsItemHovered()) {
			return;
		}

		if (!ImGui::BeginTooltip()) {
			return;
		}

		ImGui::Text("type %d, array type %d", static_cast<int>(field.type), static_cast<int>(field.array_type));
		if (field.owner->field_offsets != nullptr) {
			ImGui::Text("offset 0x%04x", field.owner->field_offsets[field.index]);
		}

		if (field.owner->field_type_ids != nullptr) {
			ImGui::Text("type id 0x%08x", field.owner->field_type_ids[field.index]);
		}

		if (field.owner->field_descriptions != nullptr) {
			if (const auto *description = field.owner->field_descriptions[field.index]; description != nullptr && *description) {
				ImGui::Separator();
				ImGui::TextUnformatted(description);
			}
		}

		ImGui::EndTooltip();
	}

	// renders each ddl field as a row of a two column table
	struct DDLInspector final : ddl::Visitor {
		DDLInspector() = default;
		DDLInspector(DDLInspector &&) = delete;

		auto
		OnValue(const ddl::Field &field, const ddl::Value &value) -> void override {
			char label[0x100];
			Row(Label(field, label, sizeof(label)));
			FieldTooltip(field);

			char text[0x300];
			FormatValue(field, value, text, sizeof(text));
			ImGui::TableSetColumnIndex(1);
			ImGui::TextUnformatted(text);
		}

		auto
		BeginArray(const ddl::Field &field, const int32_t count) -> bool override {
			return BeginNode(field, count, "items");
		}

		auto
		EndArray(const ddl::Field &) -> void override {
			EndNode();
		}

		auto
		BeginMap(const ddl::Field &field, const int32_t count) -> bool override {
			in_map = true;
			return BeginNode(field, count, "entries");
		}

		auto
		OnMapKey(const ddl::Field &field, const ddl::Value &key) -> void override {
			FormatValue(field, key, pending_key, sizeof(pending_key));
		}

		auto
		EndMap(const ddl::Field &) -> void override {
			EndNode();
			in_map = false;
		}

		auto
		OnUnhandled(const ddl::Field &field, const char *reason) -> void override {
			char label[0x100];
			Row(Label(field, label, sizeof(label)));
			FieldTooltip(field);

			ImGui::TableSetColumnIndex(1);
			ImGui::TextDisabled("<%s not decoded>", reason);
		}

	private:
		// the name of a top level field, or the position of an array or map element
		auto
		Label(const ddl::Field &field, char *out, const size_t size) -> const char * {
			if (!nested) {
				const auto *name = field.name();
				return name != nullptr && *name ? name : "<unnamed>";
			}

			if (in_map) {
				_snprintf_s(out, size, _TRUNCATE, "%s", pending_key);
			} else {
				_snprintf_s(out, size, _TRUNCATE, "[%d]", element);
			}

			++element;
			return out;
		}

		auto
		Row(const char *label) -> void {
			ImGui::TableNextRow();
			ImGui::TableSetColumnIndex(0);
			ImGui::TextUnformatted(label);
		}

		auto
		BeginNode(const ddl::Field &field, const int32_t count, const char *noun) -> bool {
			const auto *name = field.name();

			ImGui::TableNextRow();
			ImGui::TableSetColumnIndex(0);
			// short collections are worth seeing without a click, long ones would flood the table
			const auto flags = ImGuiTreeNodeFlags_SpanFullWidth | (count <= 8 ? ImGuiTreeNodeFlags_DefaultOpen : 0);
			nested = ImGui::TreeNodeEx(name != nullptr && *name ? name : "<unnamed>", flags);
			FieldTooltip(field);

			ImGui::TableSetColumnIndex(1);
			ImGui::TextDisabled("%d %s", count, noun);

			element = 0;
			return nested;
		}

		auto
		EndNode() -> void {
			if (nested) {
				ImGui::TreePop();
			}

			nested = false;
			element = 0;
		}

		int32_t element = 0;
		bool nested = false;
		bool in_map = false;
		char pending_key[0x100] {};
	};

	auto
	DrawComponentPrius(const ComponentInfo *type, const Component *instance) -> void {
		if (type == nullptr || instance == nullptr) {
			return;
		}

		const auto *prius = type->prius;
		if (prius == nullptr || instance->ddlPriusData == nullptr) {
			return;
		}

		// this guard has to come first: the header below dereferences prius, and
		// an actor can hand us a hundred of these
		if (!ddl::is_readable(prius, sizeof(DDLTypeInfo)) || prius->field_count <= 0) {
			return;
		}

		char name[0x80];
		if (!ddl::read_string(prius->name, name, sizeof(name))) {
			strcpy_s(name, "prius");
		}

		char header[0x120];
		_snprintf_s(header, sizeof(header), _TRUNCATE, "%s (%d fields)", name, prius->field_count);
		if (!ImGui::CollapsingHeader(header)) {
			// only the expanded component pays for the instance check below
			return;
		}

		const auto *data = ddl::prius_data(instance->ddlPriusData, prius->allocation_size);
		if (data == nullptr) {
			ImGui::TextDisabled("prius data is not readable");
			return;
		}

		ImGui::TextDisabled("holder %p -> %p, count %d", instance->ddlPriusData, static_cast<const void *>(data), ddl::prius_count(instance->ddlPriusData));

		constexpr auto flags = ImGuiTableFlags_Resizable | ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_NoSavedSettings;
		if (ImGui::BeginTable("prius_fields", 2, flags)) {
			ImGui::TableSetupColumn("Field", ImGuiTableColumnFlags_WidthStretch, 0.45f);
			ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch, 0.55f);
			ImGui::TableHeadersRow();

			DDLInspector inspector;
			ddl::visit(inspector, prius, data);

			ImGui::EndTable();
		}
	}

	// strips anything that cannot go in a file name
	static auto
	SanitiseName(const char *name, char *out, const size_t size) -> void {
		size_t written = 0;
		for (size_t i = 0; name != nullptr && name[i] != '\0' && written < size - 1; ++i) {
			const auto c = name[i];
			out[written++] = (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_' || c == '-' ? c : '_';
		}

		out[written] = '\0';

		if (written == 0) {
			strcpy_s(out, size, "actor");
		}
	}

	// bounded read of an engine string for json, see ddl::safe_text
	static auto
	SafeText(const char *text) -> nlohmann::json {
		char buffer[0x400];
		if (!ddl::read_string(text, buffer, sizeof(buffer))) {
			return nullptr;
		}

		return buffer;
	}

	static auto
	DumpActorImpl(const Actor *actor, char *path, const size_t path_size) -> bool {
		nlohmann::json dump;
		dump["rivet_version"] = RIVET_VERSION;

		char actor_name[0x100];
		if (!ddl::read_string(actor->GetName(), actor_name, sizeof(actor_name))) {
			actor_name[0] = '\0';
		}

		dump["name"] = actor_name;
		dump["generation"] = actor->generation;
		dump["zone_index"] = actor->zoneIndex;
		dump["scene_index"] = actor->sceneIndex;
		dump["flags"] = actor->flags;
		dump["flag_names"] = DescribeActorFlags(actor->flags);
		dump["update_parent"] = actor->updateParent.value;
		dump["time_scale"] = actor->timeScale;
		dump["component_count"] = actor->componentCount;

		if (actor->object != nullptr && ddl::is_readable(actor->object, sizeof(SceneObject))) {
			nlohmann::json object;
			object["position"] = { actor->object->transform_matrix[3][0], actor->object->transform_matrix[3][1], actor->object->transform_matrix[3][2] };
			object["scale"] = { actor->object->scale.x, actor->object->scale.y, actor->object->scale.z };
			// bitfields cannot bind to nlohmann's templated assignment directly
			object["flags"] = static_cast<uint32_t>(actor->object->objectFlags);
			object["object_type"] = static_cast<uint32_t>(actor->object->objectType);
			dump["object"] = object;
		}

		nlohmann::json::array_t components;
		for (auto index = 0; index < actor->componentCount; ++index) {
			const auto [type, instance] = actor->components[index];
			if (type == nullptr || instance == nullptr || instance->IsDestroyed()) {
				continue;
			}

			nlohmann::json component;
			component["name"] = SafeText(type->name);
			component["id"] = type->id;
			component["size"] = type->size;
			component["class_flags"] = DescribeFlags(type->class_flags, COMPONENT_CLASS_FLAG_NAMES, std::size(COMPONENT_CLASS_FLAG_NAMES));
			component["prius_behavior"] = PriusBehaviorName(type->prius_behavior);
			component["active"] = (instance->flags & ComponentFlag::Active) != 0;
			component["handle"] = instance->handle.value;
			component["parent_handle"] = instance->parentComponent.value;
			component["prius_size"] = type->prius_info.size;

			if (const auto *prius = type->prius; prius != nullptr && ddl::is_readable(prius, sizeof(DDLTypeInfo))) {
				// the holder itself, so the { vtable, count, data } shape stays checkable
				if (ddl::is_readable(instance->ddlPriusData, ddl::PRIUS_HOLDER_SIZE)) {
					component["prius_holder"] = ddl::hex_dump(static_cast<const uint8_t *>(instance->ddlPriusData), ddl::PRIUS_HOLDER_SIZE);
					component["prius_count"] = ddl::prius_count(instance->ddlPriusData);
				}

				if (const auto *data = ddl::prius_data(instance->ddlPriusData, prius->allocation_size); data != nullptr) {
					component["prius"] = ddl::dump_object(prius, data);
				} else {
					// still worth the type metadata even when the instance is unreadable
					component["prius"] = ddl::dump_object(prius, nullptr);
					component["prius_unreadable"] = true;
				}
			}

			components.emplace_back(component);
		}

		dump["components"] = components;

		char safe[0x80];
		SanitiseName(actor_name, safe, sizeof(safe));
		_snprintf_s(path, path_size, _TRUNCATE, "./rivet_actor_%s.json", safe);

		std::ofstream file;
		file.open(path);
		if (!file.is_open()) {
			g_output << "[dump] could not open " << path << "\n";
			g_output.flush();
			return false;
		}

		// live instance data can hold bytes that are not valid utf-8, and dump()
		// throws on those by default. replace them rather than lose the dump.
		const auto text = dump.dump(2, ' ', false, nlohmann::json::error_handler_t::replace);
		file.write(text.c_str(), static_cast<std::streamsize>(text.size()));
		file.flush();
		file.close();

		g_output << "[dump] wrote " << path << " (" << text.size() << " bytes, " << components.size() << " components)\n";
		g_output.flush();
		return true;
	}

	auto
	DumpActor(const Actor *actor) -> std::string {
		if (actor == nullptr || actor->components == nullptr) {
			return {};
		}

		// this runs on the present hook. nothing may escape into the render thread.
		char path[0x120] = {};
		try {
			if (!DumpActorImpl(actor, path, sizeof(path))) {
				return {};
			}
		} catch (const std::exception &error) {
			g_output << "[dump] failed: " << error.what() << "\n";
			g_output.flush();
			return {};
		} catch (...) {
			g_output << "[dump] failed with an unknown exception\n";
			g_output.flush();
			return {};
		}

		return path;
	}
} // namespace rivet_hook

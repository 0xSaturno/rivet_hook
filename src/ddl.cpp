// SPDX-FileCopyrightText: 2025-2026 Neptuwunium
//
// SPDX-License-Identifier: EUPL-1.2

#include <thread>
#include <unordered_set>
#include <vector>

#include "ddl.hpp"
#include "ddl_visit.hpp"
#include "runtime.hpp"
#include "signature.hpp"
#include "signature_engine.hpp"

#include <nlohmann/json.hpp>

using namespace rivet_hook::game;

namespace rivet_hook::ddl {
	auto
	dump_ddl() -> void {
		g_output << "[rivet] dumping DDL structures\n";
		using namespace std::chrono_literals;

		const auto hm_pointer = find_address(DDL_HASH_MAP_SIGNATURE);
		const auto tl_pointer = find_address(DDL_TYPE_LIST_SIGNATURE);

		if (hm_pointer == 0 || tl_pointer == 0) {
			return;
		}

		g_output << "[DDL] dumping...\n";

		const auto *type_hash_map = static_cast<const DDLHashMap *>(load_rel_var(hm_pointer, DDL_HASH_MAP_ADDRESS));
		const auto **type_list = static_cast<const DDLTypeDescriptor **>(load_rel_var(tl_pointer, DDL_TYPE_LIST_ADDRESS));
		const auto type_count = *static_cast<const uint32_t *>(load_rel_var(tl_pointer, DDL_TYPE_LIST_COUNT_ADDRESS));

		std::unordered_set<uint32_t> enum_ids;
		std::unordered_set<uint32_t> bitset_ids;

		nlohmann::json::array_t enums;
		nlohmann::json::array_t bitsets;
		nlohmann::json::array_t roots;
		nlohmann::json::array_t types;

		for (auto i = 0u; i < type_count; ++i) {
			const auto &type_ptr = type_list[i];
			if (type_ptr == nullptr) {
				continue;
			}

			nlohmann::json root_type;
			g_output << "[ddl] processing " << std::hex << type_ptr->name << " " << type_ptr->type_id << "\n";
			root_type["name"] = type_ptr->name;
			root_type["id"] = type_ptr->type_id;
			if (type_ptr->parent != nullptr) {
				nlohmann::json parent_type;
				const auto *parent_ptr = *type_ptr->parent;
				parent_type["name"] = parent_ptr->name;
				parent_type["id"] = parent_ptr->type_id;
				root_type["parent"] = parent_type;
			}

			roots.push_back(root_type);
		}

		for (auto i = 0u; i < type_hash_map->capacity; ++i) {
			const auto &type_ptr = type_hash_map->values[i];
			if (type_ptr == nullptr) {
				continue;
			}

			void *ddl_inst_this = calloc(type_ptr->allocation_size, 1);
			auto type_ctor = type_ptr->constructor_ptr;
			auto type_dtor = type_ptr->destructor_ptr;
			if (auto type_init = type_ptr->init_defaults_ptr; type_ctor != nullptr && type_init != nullptr) {
				if (auto temp = type_ctor(ddl_inst_this); temp != nullptr) {
					type_init(temp);
				} else {
					free(ddl_inst_this);
					ddl_inst_this = nullptr;
				}
			} else {
				free(ddl_inst_this);
				ddl_inst_this = nullptr;
			}

			if (g_settings.ddl.debug_ddl && ddl_inst_this != nullptr) {
				std::ofstream ddl_bin;
				ddl_bin.open("./ddl/" + std::string(type_ptr->name) + ".bin", std::ios::app | std::ios::binary);
				ddl_bin.write(static_cast<char *>(ddl_inst_this), type_ptr->allocation_size + 16);
				ddl_bin.flush();
				ddl_bin.close();
			}

			nlohmann::json type_info;
			type_info["name"] = type_ptr->name;
			type_info["id"] = type_ptr->type_id;
			type_info["component_id"] = type_ptr->function_id;
			type_info["parent_id"] = type_ptr->parent_id;
			type_info["size"] = type_ptr->allocation_size;

			nlohmann::json::array_t fields;

			for (auto fi = 0; fi < type_ptr->field_count; ++fi) {
				nlohmann::json field;
				field["name"] = type_ptr->field_names[fi];
				field["id"] = type_ptr->field_ids[fi];
				field["type_id"] = type_ptr->field_type_ids[fi];
				field["label"] = type_ptr->field_labels[fi];
				field["display_label"] = type_ptr->field_names2[fi];
				field["description"] = type_ptr->field_descriptions[fi];
				field["type"] = type_ptr->field_types[fi];
				field["array_type"] = type_ptr->field_array_types[fi];
				field["map_type"] = type_ptr->field_map_types[fi];
				field["fized_size"] = type_ptr->field_array_sizes[fi];
				field["offset"] = type_ptr->field_offsets[fi];

				if (ddl_inst_this != nullptr) {
					Field walk_field {};
					walk_field.owner = type_ptr;
					walk_field.index = fi;
					walk_field.type = static_cast<FieldType>(type_ptr->field_types[fi]);
					walk_field.array_type = static_cast<ArrayType>(type_ptr->field_array_types[fi]);

					JsonVisitor visitor { field };
					visit_field(visitor, static_cast<const uint8_t *>(ddl_inst_this), type_ptr->field_offsets[fi], walk_field);
				}

				if (const auto *extra = type_ptr->field_ex[fi]; extra != nullptr) {
					auto field_type = type_ptr->field_types[fi];
					auto type_id = type_ptr->field_type_ids[fi];
					if (field_type == 13) {
						const auto *ex_13 = static_cast<const DDLTypeInfo *>(extra);
						nlohmann::json struct_type;
						struct_type["name"] = ex_13->name;
						struct_type["id"] = ex_13->type_id;
						field["struct"] = struct_type;
					} else if (field_type == 12) {
						if (!bitset_ids.contains(type_id)) {
							bitset_ids.emplace(type_id);
							const auto *ex_12 = static_cast<const DDLBitSetTypeInfo *>(extra);
							nlohmann::json bitset;
							bitset["id"] = type_id;
							nlohmann::json::array_t bitset_values;
							for (auto bi = 0u; bi < ex_12->count; ++bi) {
								nlohmann::json bitset_value;
								bitset_value["value"] = ex_12->values[bi];
								bitset_value["id"] = ex_12->ids[bi];
								bitset_value["name"] = ex_12->names[bi];
								bitset_values.emplace_back(bitset_value);
							}

							bitset["values"] = bitset_values;
							bitsets.push_back(bitset);
						}
					} else if (field_type == 11) {
						const auto *ex_11 = static_cast<const DDLSelectTypeInfo *>(extra);
						field["enum_type_id"] = ex_11->select_info->type_id;
						if (!enum_ids.contains(ex_11->select_info->type_id)) {
							enum_ids.emplace(ex_11->select_info->type_id);
							nlohmann::json enuminfo;
							enuminfo["id"] = ex_11->select_info->type_id;
							enuminfo["id2"] = type_id;
							nlohmann::json::array_t enum_values;
							for (auto bi = 0u; bi < ex_11->select_info->count; ++bi) {
								nlohmann::json enum_value;
								enum_value["id"] = ex_11->select_info->ids[bi];
								enum_value["name"] = ex_11->select_info->names[bi];
								enum_value["description"] = ex_11->select_info->descriptions[bi];
								enum_value["label"] = ex_11->select_info->labels[bi];
								enum_values.emplace_back(enum_value);
							}

							enuminfo["values"] = enum_values;
							enums.push_back(enuminfo);
						}
					}
				}

				fields.push_back(field);
			}

			type_info["fields"] = fields;
			types.push_back(type_info);

			if (g_settings.ddl.debug_ddl) {
				std::ofstream ddl_json_data;
				ddl_json_data.open("./ddl/" + std::string(type_ptr->name) + ".json");
				auto ddl_json_text = type_info.dump(4);
				ddl_json_data.write(ddl_json_text.c_str(), static_cast<std::streamsize>(ddl_json_text.size()));
				ddl_json_data.flush();
				ddl_json_data.close();
			}

			if (type_dtor != nullptr && ddl_inst_this != nullptr) {
				type_dtor(ddl_inst_this);
				free(ddl_inst_this);
			}
		}

		nlohmann::json ddl_dump {};
		ddl_dump["enums"] = enums;
		ddl_dump["bitsets"] = bitsets;
		ddl_dump["roots"] = roots;
		ddl_dump["types"] = types;

		std::ofstream json_data;

		json_data.open("./ddl.json");
		auto json_text = ddl_dump.dump();
		json_data.write(json_text.c_str(), static_cast<std::streamsize>(json_text.size()));
		json_data.flush();
		json_data.close();

		g_output << "[DDL] done\n";
		g_output << "[DDL] found " << enums.size() << " enums\n";
		g_output << "[DDL] found " << bitsets.size() << " bitsets\n";
		g_output << "[DDL] found " << roots.size() << " roots\n";
		g_output << "[DDL] found " << types.size() << " types\n";
	}

	auto
	dump_versions() -> void {
		g_output << "[rivet] dumping versions\n";
		using namespace std::chrono_literals;
		using version_str_t = const char *(*) (uint32_t index);
		using version_hash_t = uint32_t (*)(uint32_t index);

		auto function_ptr = find_address(VERSION_SIGNATURE);
		auto hash_function_ptr = find_address(VERSION_HASH_SIGNATURE);

		if (function_ptr == 0 || hash_function_ptr == 0) {
			return;
		}

		g_output << "[version] dumping...\n";
		auto func1 = reinterpret_cast<version_str_t>(function_ptr);
		auto func2 = reinterpret_cast<version_hash_t>(hash_function_ptr);

		uint32_t index = 0;
		nlohmann::json versions = nlohmann::json::array_t();
		while (true) {
			auto version_str = func1(index++);
			if (static_cast<int32_t>(reinterpret_cast<intptr_t>(version_str)) == -1) {
				break;
			}

			auto hash = func2(index);
			nlohmann::json version;
			std::stringstream str_stream;
			str_stream << std::hex << std::setfill('0') << std::setw(8) << hash;
			version["id"] = hash;
			version["version"] = version_str;
			versions.emplace_back(version);
			g_output << "[ver] " << version << " = " << str_stream.str() << "\n";
		}

		std::ofstream json_data;

		json_data.open("./versions.json");
		auto json_text = versions.dump();
		json_data.write(json_text.c_str(), static_cast<std::streamsize>(json_text.size()));
		json_data.flush();
		json_data.close();
	}

	auto
	dump_components() -> void {
		g_output << "[rivet] dumping components\n";

		auto component_registry = load_rel_var(find_address(COMPONENT_REGISTER_SIGNATURE), COMPONENT_REGISTRY_ADDRESS);
		auto component_count = load_rel_var(find_address(COMPONENT_REGISTER_SIGNATURE), COMPONENT_COUNT_ADDRESS);

		if (component_registry == nullptr || component_count == nullptr) {
			return;
		}

		const auto component_infos = *static_cast<ComponentInfo***>(component_registry);
		const auto count = *static_cast<int32_t*>(component_count);

		nlohmann::json components = nlohmann::json::array_t();
		for (int i = 0; i < count; i++) {
			const auto component_info_p = component_infos[i];
			if (!component_info_p) {
				continue;
			}

			const auto component_info = *component_info_p;
			nlohmann::json component;
			g_output << "[component] processing " << std::hex << component_info.name << " " << component_info.id << "\n";
			component["id"] = component_info.id;
			component["name"] = component_info.name ? component_info.name : "";
			component["size"] = component_info.size;
			component["prius_size"] = component_info.prius_info.size;
			if (component_info.prius) {
				auto prius_json = nlohmann::json::array_t();
				prius_json.emplace_back(component_info.prius->name ? component_info.prius->name : "");
				prius_json.emplace_back(component_info.prius->type_id);
				component["prius"] = prius_json;
			}
			component["class_flags"] = component_info.class_flags;
			component["class_flag_names"] = DescribeFlags(component_info.class_flags, COMPONENT_CLASS_FLAG_NAMES, std::size(COMPONENT_CLASS_FLAG_NAMES));
			component["prius_behavior"] = PriusBehaviorName(component_info.prius_behavior);
			component["index"] = component_info.index;
			component["update_order"] = component_info.update_order;
			component["cache_index"] = component_info.cache_index;
			component["block_count"] = component_info.block_count;
			component["parent_count"] = component_info.parent_count;
			auto bases = nlohmann::json::array_t();

			// the chain is parent_count long, the tail past it is not cleared
			for (int j = 0; j < component_info.parent_count && j < static_cast<int>(std::size(component_info.parent_classes)); ++j) {
				const auto *base = component_info.parent_classes[j];
				if (base == nullptr) {
					continue;
				}

				auto base_json = nlohmann::json::array_t();
				base_json.emplace_back(base->name ? base->name : "");
				base_json.emplace_back(base->id);
				bases.emplace_back(base_json);
			}

			if (!bases.empty()) {
				component["base"] = bases;
			}

			components.emplace_back(component);
		}

		std::ofstream json_data;
		json_data.open("./components.json");
		auto json_text = components.dump();
		json_data.write(json_text.c_str(), static_cast<std::streamsize>(json_text.size()));
		json_data.flush();
		json_data.close();
	}

	auto
	dump() -> void {
		if (WaitForSingleObject(g_game_inited, INFINITE) == WAIT_FAILED) {
			g_output << "[DDL] wait failed??\n";
			return;
		}

		if (g_settings.ddl.dump_ddl) {
			if (g_settings.ddl.debug_ddl) {
				std::filesystem::create_directory("./ddl");
			}
			dump_ddl();
		}

		if (g_settings.ddl.dump_versions) {
			dump_versions();
		}

		if (g_settings.ddl.dump_components) {
			dump_components();
		}

		g_output.flush();
	}
} // namespace rivet_hook::ddl

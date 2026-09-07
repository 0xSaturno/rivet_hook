// SPDX-FileCopyrightText: 2025-2026 Neptuwunium
//
// SPDX-License-Identifier: EUPL-1.2

#include <chrono>
#include <cstdio>
#include <fstream>
#include <memory>
#include <ostream>
#include <thread>
#include <filesystem>
#include <mutex>

#ifndef RIVET_ONLY_LOADER
#include "ddl.hpp"
#include "bridge.hpp"
#include "overlay.hpp"
#include "scripting.hpp"
#endif

#include "runtime.hpp"
#include "runtime_loader.hpp"
#include "settings.hpp"
#include "signature.hpp"
#include "signature_engine.hpp"

#include <MinHook.h>

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wmicrosoft-cast"

namespace {
	HMODULE g_renderdoc = nullptr;
	bool g_minhook_initialized = false;
	bool has_exited = false;
} // namespace

namespace rivet_hook {
	std::ofstream g_output;
	Settings g_settings;
	HMODULE g_game_module = nullptr;
	HANDLE g_game_inited = nullptr;

#pragma clang diagnostic push
#pragma ide diagnostic ignored "cppcoreguidelines-pro-bounds-pointer-arithmetic"
	using context_log_t = const char *(*) (const char *, const char *);
	using game_init_t = bool (*)(void* self);
	game_init_t game_engine_init = nullptr;
	context_log_t game_context_log = nullptr;
	std::string last_context;
	std::string last_message;
	std::mutex address_cache_mutex;

	auto
	find_addresses(const hex_signature &signature) -> std::vector<intptr_t> {
		std::vector<intptr_t> pointers;
		{
			std::lock_guard guard(address_cache_mutex);

			if (const auto cacheKey = std::format("{}_{:016X}", signature.name, signature.hash); !g_settings.address_cache.addresses.contains(cacheKey)) {
				if (g_settings.log.pointers) {
					g_output << "[rivet] searching for " << signature.name << " pointers\n";
				}
				pointers = scan(g_game_module, signature);
				g_settings.address_cache.addresses.emplace(cacheKey, pointers);
			} else {
				pointers = g_settings.address_cache.addresses[cacheKey];
			}
		}

		if (pointers.empty()) {
			g_output << "[rivet] could not find " << signature.name << " pointer, aborting\n";
			return {};
		}

		if (g_settings.log.pointers) {
			g_output << "[rivet] found " << pointers.size() << " " << signature.name << " pointers" << std::dec << "\n";
		}

		return pointers;
	}

	auto
	find_address(const hex_signature &signature, const size_t limit, const int select) -> intptr_t {
		const auto pointers = find_addresses(signature);

		if (pointers.empty()) {
			return 0;
		}

		if (pointers.size() > limit) {
			g_output << "[rivet] found " << pointers.size() << " " << signature.name << " pointers, too many. aborting\n";
			return 0;
		}

		const auto pointer = pointers[select];

		if (g_settings.log.pointers) {
			g_output << "[rivet] found " << signature.name << " pointer at " << std::hex << pointer << std::dec << "\n";
		}

		return pointer;
	}

	auto
	load_rel_var(const intptr_t ptr, const int rel_address) -> void * {
		if (ptr == 0) {
			return nullptr;
		}

		const auto rip = reinterpret_cast<uint8_t*>(ptr) + rel_address + REL_ADDRESS_SIZE;
		const auto target = *reinterpret_cast<uint32_t *>(ptr + rel_address);
		return rip + target;
	}

	// ReSharper disable twice CppParameterMayBeConst
	auto
	create_hook(const std::string_view &name, LPVOID pointer, LPVOID detour, LPVOID *original) -> void {
		if (!g_minhook_initialized) {
			if (MH_Initialize() != MH_OK) {
				g_output << "[rivet] failed to initialize minhook\n";
				return;
			}
			g_minhook_initialized = true;
		}

		if (MH_CreateHook(pointer, detour, original) != MH_OK) {
			g_output << "[rivet] failed to create " << name << " hook\n";
			return;
		}

		if (MH_EnableHook(pointer) != MH_OK) {
			g_output << "[rivet] failed to enable " << name << " hook\n";
			return;
		}

		if (g_settings.log.pointers) {
			g_output << "[rivet] created " << name << " hook\n";
		}
	}

	// ReSharper disable once CppParameterMayBeConst
	auto
	create_hook(const hex_signature &signature, LPVOID detour, LPVOID *original, const size_t limit, const int select) -> void {
		const auto pointer = find_address(signature, limit, select);
		if (pointer == 0) {
			return;
		}

		create_hook(signature.name, reinterpret_cast<LPVOID>(pointer), detour, original);
	}

	auto
	null_func() -> void { }

	auto
	return_true() -> bool {
		return true;
	}

	auto
	return_false() -> bool {
		return false;
	}

	auto
	context_log(const char *context, const char *message) -> const char * {
		const auto valid = context != nullptr && context[0] != 0 && context[0] != '?' && message != nullptr && message[0] != 0 && message[0] != '?';
		const auto *result = game_context_log(context, message);
		if (valid) {
			const auto current_context = std::string(context);

			if (const auto current_message = std::string(message); current_context != last_context || current_message != last_message) {
				last_context = current_context;
				last_message = current_message;
				g_output << "[ctx] [" << (context == nullptr ? "?" : context) << "] " << (message == nullptr ? "" : message) << "\n";
			}
		}
		return result;
	}

	auto
	log(const char *message, ...) -> void * { // NOLINT(*-dcl50-cpp)
		if (message != nullptr) {
			va_list args; // NOLINT(*-init-variables)
			va_start(args, message);
			const auto buffer_size = vsnprintf(nullptr, 0, message, args) + 1;
			const auto buffer = std::make_unique<char[]>(buffer_size); // NOLINT(*-avoid-c-arrays)
			vsnprintf(buffer.get(), buffer_size, message, args);	   // NOLINT(*-err33-c)
			va_end(args);
			const std::string buffer_str(buffer.get());
			g_output << "[log] " << buffer_str;
			if (buffer_str.back() != '\n') {
				g_output << "\n";
			} else {
			}
		}

		return nullptr;
	}

	auto
	engine_init(void* self) -> bool {
		const auto result = game_engine_init(self);
		SetEvent(g_game_inited);
		return result;
	}

#pragma clang diagnostic pop

	namespace runtime {
		auto
		init() -> void {
			// this runs on the main thread

			g_output.open("./rivet.log");
			g_output << "[rivet] init\n";
			g_output << "[rivet] version " << RIVET_VERSION;

			#ifdef RIVET_ONLY_LOADER
			g_output << "+loader\n";
			#endif

			if (atexit(fini)) {
				g_output << "[rivet] atexit cannot be registered\n";
			}

			if (!g_game_inited) {
				g_game_inited = CreateEvent(nullptr, true, false, nullptr);
			}

			if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_PIN, nullptr, &g_game_module)) {
				g_output << "[rivet] unable to get the executable handle.\n";
				return;
			}

			char module_name[MAX_PATH] = {};
			if (GetModuleFileNameA(g_game_module, module_name, sizeof(module_name)) > 0) {
				if (const auto module_name_str = std::string(module_name);
					module_name_str.ends_with("/crs-handler.exe") || module_name_str.ends_with("/crs-video.exe")) {
					g_output << "[rivet] why am i crs handler!!\n";
					return;
				}
			}
			g_output << "[rivet] game = " << module_name << "\n";

			g_settings = Settings::load();
			g_settings.save();

			create_hook(ENGINE_INIT_SIGNATURE, reinterpret_cast<LPVOID>(&engine_init), reinterpret_cast<LPVOID *>(&game_engine_init));

			if (g_settings.utility.suppress_crash_handler) {
				const auto nxe_vtable = load_rel_var(find_address(REL_NXEXCEPTION_VTABLE_SIGNATURE), NXEXCEPTION_VTABLE_ADDRESS);
				const auto crash_handler = static_cast<void**>(nxe_vtable)[NXEXCEPTION_VTABLE_INIT];

				create_hook("CRASH_HANDLER", crash_handler, reinterpret_cast<LPVOID>(&null_func), nullptr);
			}

			AssetLoader::init();

			#ifndef RIVET_ONLY_LOADER
			Overlay::Init();
			bridge::init();
			scripting::init();
			g_output << "[rivet] starting ddl thread\n";
			std::thread(ddl::dump).detach();

			if (g_settings.renderdoc.enabled) {
				g_output << "[rivet] loading renderdoc\n";
				if (std::filesystem::exists("renderdoc.dll")) {
					g_output << "[rivet] loaded local renderdoc\n";
					g_renderdoc = LoadLibraryA("renderdoc.dll");
				} else {
					if (const auto renderdoc_path = std::filesystem::path(g_settings.renderdoc.dll_path.data()); renderdoc_path.empty()) {
						g_output << "[rivet] renderdoc.dll not found\n";
					} else {
						if (std::filesystem::exists(renderdoc_path)) {
							g_output << "[rivet] loaded " << renderdoc_path << "\n";
							g_renderdoc = LoadLibraryA(g_settings.renderdoc.dll_path.data());
						} else {
							g_output << "[rivet] renderdoc.dll not found\n";
						}
					}
				}
			}
			#endif

			if (g_settings.utility.attach_context_log) {
				create_hook(CONTEXT_LOG_SIGNATURE, reinterpret_cast<LPVOID>(&context_log), reinterpret_cast<LPVOID *>(&game_context_log));
			}

			if (g_settings.utility.attach_log) {
				create_hook(LOG_SIGNATURE, reinterpret_cast<LPVOID>(&log), nullptr);
			}

			if (g_settings.utility.unpause_focus) {
				create_hook(UNPAUSE_FOCUS_SIGNATURE, reinterpret_cast<LPVOID>(&return_true), nullptr);
			}

			g_output << "[rivet] init complete\n";
			g_settings.save();
		}

		auto
		fini() -> void {
			if (has_exited) {
				return;
			}

			has_exited = true;

			g_output << "[rivet] fini\n";

			MH_DisableHook(MH_ALL_HOOKS);
			MH_Uninitialize();

			g_output.flush();
			g_settings.save();

			if (g_renderdoc != nullptr) {
				g_output << "[rivet] unloading renderdoc\n";
				g_output.flush();
				FreeLibrary(g_renderdoc);
			}

			#ifndef RIVET_ONLY_LOADER
			g_output << "[rivet] Overlay fini\n";
			g_output.flush();
			scripting::fini();
			bridge::fini();
			Overlay::Fini();
			#endif
			g_output << "[rivet] AssetLoader fini\n";
			g_output.flush();
			AssetLoader::fini();

			g_output << "[rivet] fini complete\n";
			g_output.flush();
			g_output.close();
		}
	} // namespace runtime
} // namespace rivet_hook

#pragma clang diagnostic pop

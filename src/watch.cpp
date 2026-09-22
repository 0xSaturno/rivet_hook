// SPDX-FileCopyrightText: 2025-2026 Neptuwunium
//
// SPDX-License-Identifier: EUPL-1.2

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tlhelp32.h>

#include <atomic>
#include <cstring>
#include <string>
#include <thread>

#include "watch.hpp"

#include "runtime.hpp"

namespace rivet_hook::watch {
	constexpr int MAX_WRITERS = 32;

	struct Writer {
		std::atomic<uintptr_t> address { 0 };
		std::atomic_uint64_t hits { 0 };
	};

	// registers at the most recent execute hits, newest last
	constexpr int MAX_HITS = 16;

	struct Hit {
		uint64_t tick;
		uint32_t thread;
		DWORD64 regs[17]; // rax rbx rcx rdx rsi rdi rbp rsp r8-r15 rip
	};

	static Hit g_hits[MAX_HITS];
	static std::atomic_uint64_t g_hit_count = 0;
	static bool g_execute = false;

	static Writer g_writers[MAX_WRITERS];
	static std::atomic_uint64_t g_dropped = 0;
	static std::atomic<uintptr_t> g_watched = 0;
	static uint32_t g_length = 0;
	static PVOID g_handler = nullptr;
	static int g_threads_armed = 0;

	// a data breakpoint traps after the write, so Rip is the instruction after it
	static auto
	record(const uintptr_t rip) -> void {
		for (auto &writer : g_writers) {
			auto current = writer.address.load(std::memory_order_acquire);
			if (current == 0 && writer.address.compare_exchange_strong(current, rip)) {
				current = rip;
			}

			if (current == rip) {
				++writer.hits;
				return;
			}
		}

		++g_dropped;
	}

	static auto CALLBACK
	on_exception(EXCEPTION_POINTERS *info) -> LONG {
		if (info->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP) {
			return EXCEPTION_CONTINUE_SEARCH;
		}

		auto *context = info->ContextRecord;
		if ((context->Dr6 & 1) == 0) {
			return EXCEPTION_CONTINUE_SEARCH;
		}

		record(context->Rip);

		if (g_execute) {
			// an instruction breakpoint faults before the instruction runs, so
			// resume flag it past the breakpoint or it fires forever
			const auto slot = g_hit_count.fetch_add(1) % MAX_HITS;
			auto &hit = g_hits[slot];
			hit.tick = GetTickCount64();
			hit.thread = GetCurrentThreadId();
			const DWORD64 regs[17] = { context->Rax, context->Rbx, context->Rcx, context->Rdx, context->Rsi, context->Rdi, context->Rbp, context->Rsp,
				context->R8, context->R9, context->R10, context->R11, context->R12, context->R13, context->R14, context->R15, context->Rip };
			memcpy(hit.regs, regs, sizeof(regs));
			context->EFlags |= 0x10000;
		}

		context->Dr6 = 0;
		return EXCEPTION_CONTINUE_EXECUTION;
	}

	static auto
	dr7_for(const uint32_t length) -> DWORD64 {
		// an execute breakpoint is rw 00 with length 00
		if (g_execute) {
			return 1ull;
		}

		// local enable for dr0, break on write (01), and the length code
		DWORD64 len_bits = 0;
		switch (length) {
			case 1: len_bits = 0; break;
			case 2: len_bits = 1; break;
			case 8: len_bits = 2; break;
			default: len_bits = 3; break;
		}

		return 1ull | (1ull << 16) | (len_bits << 18);
	}

	// sets dr0/dr7 on every thread but the caller. run from a helper thread so
	// the game thread, which executes bridge commands, is covered too.
	static auto
	apply(const uintptr_t address, const uint32_t length) -> int {
		const auto snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
		if (snapshot == INVALID_HANDLE_VALUE) {
			return 0;
		}

		const auto self = GetCurrentThreadId();
		const auto process = GetCurrentProcessId();
		int armed = 0;

		THREADENTRY32 entry { .dwSize = sizeof(entry) };
		for (auto more = Thread32First(snapshot, &entry); more; more = Thread32Next(snapshot, &entry)) {
			if (entry.th32OwnerProcessID != process || entry.th32ThreadID == self) {
				continue;
			}

			const auto thread = OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT | THREAD_SUSPEND_RESUME, FALSE, entry.th32ThreadID);
			if (thread == nullptr) {
				continue;
			}

			if (SuspendThread(thread) != static_cast<DWORD>(-1)) {
				CONTEXT context {};
				context.ContextFlags = CONTEXT_DEBUG_REGISTERS;
				if (GetThreadContext(thread, &context)) {
					context.Dr0 = address;
					context.Dr6 = 0;
					context.Dr7 = address == 0 ? (context.Dr7 & ~0xF0003ull) : ((context.Dr7 & ~0xF0003ull) | dr7_for(length));
					if (SetThreadContext(thread, &context)) {
						++armed;
					}
				}

				ResumeThread(thread);
			}

			CloseHandle(thread);
		}

		CloseHandle(snapshot);
		return armed;
	}

	auto
	arm(const uintptr_t address, const uint32_t length, const bool execute, std::string &error) -> bool {
		if (length != 1 && length != 2 && length != 4 && length != 8) {
			error = "length must be 1, 2, 4 or 8";
			return false;
		}

		if (address == 0 || (!execute && address % length != 0)) {
			error = "the address must be non zero and aligned to the length";
			return false;
		}

		if (g_handler == nullptr) {
			g_handler = AddVectoredExceptionHandler(1, on_exception);
		}

		for (auto &writer : g_writers) {
			writer.address = 0;
			writer.hits = 0;
		}

		g_dropped = 0;
		g_hit_count = 0;
		g_execute = execute;
		g_length = length;
		g_watched = address;

		std::thread worker([&] { g_threads_armed = apply(address, length); });
		worker.join();

		g_output << "[watch] armed 0x" << std::hex << address << std::dec << " (" << length << " bytes) on " << g_threads_armed << " threads\n";
		g_output.flush();
		return true;
	}

	auto
	disarm() -> void {
		if (g_watched == 0) {
			return;
		}

		std::thread worker([] { apply(0, 0); });
		worker.join();
		g_watched = 0;
	}

	auto
	results() -> nlohmann::json {
		const auto base = reinterpret_cast<uintptr_t>(g_game_module);

		nlohmann::json::array_t writers;
		for (const auto &writer : g_writers) {
			const auto address = writer.address.load();
			if (address == 0) {
				continue;
			}

			char text[64];
			if (address >= base && address - base < 0x10000000) {
				sprintf_s(text, "RiftApart+0x%llx", address - base);
			} else {
				sprintf_s(text, "0x%016llx", address);
			}

			nlohmann::json entry;
			entry["after"] = text;
			entry["hits"] = writer.hits.load();
			writers.emplace_back(entry);
		}

		char watched[32];
		sprintf_s(watched, "0x%016llx", g_watched.load());

		nlohmann::json result;
		result["watching"] = g_watched.load() != 0 ? nlohmann::json(watched) : nlohmann::json(nullptr);
		result["length"] = g_length;
		result["threads"] = g_threads_armed;
		result["dropped"] = g_dropped.load();
		result["writers"] = writers;
		result["execute"] = g_execute;

		if (g_execute) {
			static constexpr const char *names[17] = { "rax", "rbx", "rcx", "rdx", "rsi", "rdi", "rbp", "rsp", "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15", "rip" };
			const auto count = g_hit_count.load();
			const auto first = count > MAX_HITS ? count - MAX_HITS : 0;

			nlohmann::json::array_t hits;
			for (auto i = first; i < count; ++i) {
				const auto &hit = g_hits[i % MAX_HITS];
				nlohmann::json entry;
				entry["tick"] = hit.tick;
				entry["thread"] = hit.thread;
				for (auto r = 0; r < 17; ++r) {
					char value[24];
					sprintf_s(value, "0x%llx", hit.regs[r]);
					entry[names[r]] = value;
				}

				hits.emplace_back(entry);
			}

			result["hit_count"] = count;
			result["hits"] = hits;
		}

		return result;
	}
} // namespace rivet_hook::watch

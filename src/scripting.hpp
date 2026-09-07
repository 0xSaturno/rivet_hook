// SPDX-FileCopyrightText: 2025-2026 Neptuwunium
//
// SPDX-License-Identifier: EUPL-1.2

#pragma once

#include <cstddef>

#include <nlohmann/json.hpp>

namespace rivet_hook::scripting {
	// arms the host when [scripts] enabled is set. no lua runs here: the vm is
	// built on the first pump, so every chunk executes on the render thread.
	auto
	init() -> void;

	auto
	fini() -> void;

	// dispatches the queued keys and then the frame callbacks. called once per
	// presented frame from present, which is the only place engine state is safe
	// to touch.
	auto
	pump() -> void;

	// queues a key for the next pump. called from the raw input hook, which is not
	// the render thread, so nothing on this path may touch the vm.
	auto
	on_key_event(int vk) -> void;

	// asks the next pump to rebuild the vm. safe from any thread.
	auto
	request_reload() -> void;

	// rebuilds the vm and reloads every script. render thread only.
	auto
	reload() -> void;

	// compiles and runs one chunk. on success out receives the first return value
	// rendered as text, on failure the error message. render thread only.
	auto
	exec(const char *source, char *out, size_t out_size) -> bool;

	// what loaded, what is registered, and what has been dispatched so far. this is
	// how a callback is proven to have fired, rather than by eyeballing the game.
	auto
	status() -> nlohmann::json;
} // namespace rivet_hook::scripting

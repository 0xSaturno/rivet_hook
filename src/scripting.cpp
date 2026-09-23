// SPDX-FileCopyrightText: 2025-2026 Neptuwunium
//
// SPDX-License-Identifier: EUPL-1.2

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <string>
#include <unordered_set>
#include <vector>

extern "C" {
#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>
}

#include "scripting.hpp"

#include "bridge.hpp"
#include "ddl_inspector.hpp"
#include "ddl_visit.hpp"
#include "events.hpp"
#include "game/scene_manager.hpp"
#include "game_thread.hpp"
#include "runtime.hpp"
#include "runtime_loader.hpp"
#include "scene_query.hpp"
#include "vk_enum.hpp"

using namespace rivet_hook::game;

namespace rivet_hook {
	// overlay_core owns this, it is resolved once during Overlay::Init
	extern SceneManager *g_SceneManager;
} // namespace rivet_hook

namespace rivet_hook::scripting {
	// Everything here runs on the game thread between actor update passes, from
	// the same pump the bridge uses (game_thread.cpp). That is also why the
	// instruction budget below is not optional: a script that loops is a frozen
	// game, not a slow script.
	//
	// The second rule this file lives by: lua is compiled as C, so an error raised
	// inside the vm leaves through longjmp and does not run C++ destructors on the
	// way out. No lua_CFunction below may own a non trivial object while it can
	// still raise. Fixed buffers only, and anything that has to use a std:: type
	// finishes with it before the first call that can fail.

	constexpr int MAX_CALLBACKS = 64;
	constexpr int MAX_KEY_QUEUE = 64;
	// the actor scan checks its budget every this many + 1 entries
	constexpr int32_t SCAN_CHECK_MASK = 0x3FF;

	// cap on rivet.read, which formats three characters per byte
	constexpr uint32_t MAX_READ_BYTES = 512;

	// cap on rivet.write. deliberately small: this is for poking a field to see
	// what it does, not for transplanting a struct.
	constexpr uint32_t MAX_WRITE_BYTES = 64;

	struct Callback {
		int ref = LUA_NOREF;
		int key = 0; // meaningless for a frame callback
		int errors = 0;
		bool disabled = false;
		char source[96] {}; // where it was registered, so a report can name it
	};

	struct Script {
		std::string name;
		std::string error;
		bool ok = false;
	};

	static lua_State *g_state = nullptr;
	static std::atomic_bool g_reload_pending = false;

	static Callback g_frame_callbacks[MAX_CALLBACKS];
	static int g_frame_count = 0;
	static Callback g_key_callbacks[MAX_CALLBACKS];
	static int g_key_count = 0;
	// key is the event class id
	static Callback g_event_callbacks[MAX_CALLBACKS];
	static int g_event_count = 0;

	// keys arrive on the input thread and are dispatched by the next pump
	static std::mutex g_key_lock;
	static int g_key_queue[MAX_KEY_QUEUE];
	static int g_key_queued = 0;
	static uint64_t g_keys_dropped = 0;

	static std::vector<Script> g_scripts;

	// counters, so "the callback fired" can be proven rather than eyeballed
	static uint64_t g_frames = 0;
	static uint64_t g_dispatched_frame = 0;
	static uint64_t g_dispatched_key = 0;
	static uint64_t g_dispatched_event = 0;
	static uint64_t g_errors = 0;
	static double g_last_ms = 0.0;
	static double g_peak_ms = 0.0;
	static char g_last_error[0x400] {};

	static int64_t g_qpc_frequency = 0;
	static int64_t g_started_at = 0;
	static int64_t g_last_frame_at = 0;
	static int64_t g_deadline = 0;

	// ------------------------------------------------------------- plumbing --

	static auto
	now_ticks() -> int64_t {
		LARGE_INTEGER counter {};
		QueryPerformanceCounter(&counter);
		return counter.QuadPart;
	}

	static auto
	ticks_to_ms(const int64_t ticks) -> double {
		return g_qpc_frequency > 0 ? static_cast<double>(ticks) * 1000.0 / static_cast<double>(g_qpc_frequency) : 0.0;
	}

	// bounded append. strcat_s aborts the process on overflow rather than
	// truncating, which is not a thing a log line may do.
	static auto
	append(char *out, const size_t size, size_t &at, const char *text, const size_t length) -> void {
		if (out == nullptr || size == 0 || at + 1 >= size) {
			return;
		}

		const auto room = size - 1 - at;
		const auto take = length < room ? length : room;
		memcpy(out + at, text, take);
		at += take;
		out[at] = '\0';
	}

	static auto
	is_hex(const char c) -> bool {
		return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
	}

	static auto
	hex_value(const char c) -> int {
		if (c >= '0' && c <= '9') {
			return c - '0';
		}

		return (c >= 'a' ? c - 'a' : c - 'A') + 10;
	}

	static auto
	budget_ms() -> int {
		return g_settings.scripts.budget_ms > 0 ? g_settings.scripts.budget_ms : 8;
	}

	static auto
	budget_expired() -> bool {
		return g_deadline != 0 && now_ticks() > g_deadline;
	}

	// LUA_MASKCOUNT gives the wall clock check a place to run from. the count alone
	// is not the budget: how long a given number of instructions takes depends
	// entirely on what they call into.
	static auto
	budget_hook(lua_State *L, lua_Debug *) -> void {
		if (!budget_expired()) {
			return;
		}

		// clear the hook before raising: the error unwinds through lua's own code
		// and tripping the hook again from inside that would be a second longjmp
		// out of the same spot
		lua_sethook(L, nullptr, 0, 0);
		luaL_error(L, "ran past the %d ms frame budget", budget_ms());
	}

	static auto
	arm_budget(lua_State *L) -> void {
		const auto interval = g_settings.scripts.check_interval > 0 ? g_settings.scripts.check_interval : 10000;
		g_deadline = now_ticks() + g_qpc_frequency * budget_ms() / 1000;
		lua_sethook(L, budget_hook, LUA_MASKCOUNT, interval);
	}

	static auto
	disarm_budget(lua_State *L) -> void {
		lua_sethook(L, nullptr, 0, 0);
		g_deadline = 0;
	}

	static auto
	record_error(const char *message) -> void {
		++g_errors;
		_snprintf_s(g_last_error, sizeof(g_last_error), _TRUNCATE, "%s", message != nullptr ? message : "unknown error");
		g_output << "[script] " << g_last_error << "\n";
		g_output.flush();
	}

	// appends a traceback while the erroring stack is still standing
	static auto
	l_traceback(lua_State *L) -> int {
		const auto *message = lua_tostring(L, 1);
		if (message == nullptr) {
			message = "(error object is not a string)";
		}

		luaL_traceback(L, L, message, 1);
		return 1;
	}

	// the only way into the vm. the callable and its arguments are already on the
	// stack. on failure the message is in g_last_error and the stack is restored.
	static auto
	protected_call(lua_State *L, const int nargs, const int nresults) -> bool {
		const auto base = lua_gettop(L) - nargs;
		lua_pushcfunction(L, l_traceback);
		lua_insert(L, base);

		arm_budget(L);
		const auto status = lua_pcall(L, nargs, nresults, base);
		disarm_budget(L);

		lua_remove(L, base);

		if (status == LUA_OK) {
			return true;
		}

		record_error(lua_tostring(L, -1));
		lua_pop(L, 1);
		return false;
	}

	// runs one callback and counts the run. push_args pushes its arguments and
	// returns how many.
	template<typename Push>
	static auto
	run_callback(Callback &entry, uint64_t &counter, Push push_args) -> void {
		if (entry.disabled || entry.ref == LUA_NOREF) {
			return;
		}

		++counter;
		lua_rawgeti(g_state, LUA_REGISTRYINDEX, entry.ref);
		if (protected_call(g_state, push_args(g_state), 0)) {
			entry.errors = 0;
			return;
		}

		// a script that fails every frame would otherwise fill the log with the
		// same traceback sixty times a second
		const auto limit = g_settings.scripts.error_limit > 0 ? g_settings.scripts.error_limit : 3;
		if (++entry.errors >= limit) {
			entry.disabled = true;
			g_output << "[script] switching off the callback from " << entry.source << " after " << entry.errors << " consecutive errors\n";
			g_output.flush();
		}
	}

	// ------------------------------------------------------- engine helpers --

	static auto
	scene_ready() -> bool {
		return g_SceneManager != nullptr && g_SceneManager->actors != nullptr && g_SceneManager->actorMax > 0 && ddl::is_readable(g_SceneManager->actors, sizeof(Actor));
	}

	static auto
	handle_of(const int32_t index, const Actor *actor) -> uint32_t {
		return EngineHandle { .id = static_cast<uint32_t>(index), .generation = actor->generation }.value;
	}

	// resolves an actor handle, raising rather than returning null. ResolveActor is
	// not used because it does not confirm the entry is mapped.
	static auto
	resolve_actor(lua_State *L, const int arg) -> Actor * {
		const auto value = static_cast<uint32_t>(luaL_checkinteger(L, arg));
		if (!scene_ready()) {
			luaL_error(L, "the scene manager is not available yet");
		}

		EngineHandle handle {};
		handle.value = value;

		// lua_pushfstring, which luaL_error formats through, only understands
		// %s %d %f %p %c %U and %%. A %x there raises "invalid option" and the
		// real message is lost, so the handle is rendered before it is passed.
		char handle_text[16];
		_snprintf_s(handle_text, sizeof(handle_text), _TRUNCATE, "0x%08x", value);

		if (static_cast<int32_t>(handle.id) >= g_SceneManager->actorMax) {
			luaL_error(L, "actor handle %s is past the end of the scene", handle_text);
		}

		auto *actor = &g_SceneManager->actors[handle.id];
		if (handle.generation == 0 || !ddl::is_readable(actor, sizeof(Actor)) || actor->generation != handle.generation) {
			luaL_error(L, "no actor for handle %s", handle_text);
		}

		return actor;
	}

	// the named component on an actor, with the live prius instance behind it.
	// prius is left null when the component carries no ddl data.
	static auto
	find_component(const Actor *actor, const char *name, const DDLTypeInfo **prius, uint8_t **data, const ComponentInfo **out_type = nullptr) -> const Component * {
		*prius = nullptr;
		*data = nullptr;
		if (out_type != nullptr) {
			*out_type = nullptr;
		}

		if (actor->components == nullptr || actor->componentCount <= 0) {
			return nullptr;
		}

		if (!ddl::is_readable(actor->components, sizeof(ComponentPointer) * actor->componentCount)) {
			return nullptr;
		}

		for (auto index = 0; index < actor->componentCount; ++index) {
			const auto [type, instance] = actor->components[index];
			if (type == nullptr || instance == nullptr || !ddl::is_readable(type, sizeof(ComponentInfo))) {
				continue;
			}

			char text[0x100];
			if (!ddl::read_string(type->name, text, sizeof(text)) || strcmp(text, name) != 0) {
				continue;
			}

			// a destroyed component stays in the list until cleanup, and a live one
			// of the same class can follow it
			if (!ddl::is_readable(instance, sizeof(Component)) || instance->IsDestroyed()) {
				continue;
			}

			if (const auto *info = type->prius; info != nullptr && ddl::is_readable(info, sizeof(DDLTypeInfo)) && info->field_count > 0) {
				*prius = info;
				*data = ddl::prius_data(instance->ddlPriusData, info->allocation_size);
			}

			if (out_type != nullptr) {
				*out_type = type;
			}

			return instance;
		}

		return nullptr;
	}

	// resolves actor + component + field for the two prius bindings, raising with
	// the reason on any miss. no non trivial object may be alive in the caller.
	static auto
	resolve_field(lua_State *L, const DDLTypeInfo **prius, uint8_t **data, const ComponentInfo **type = nullptr) -> int32_t {
		const auto *actor = resolve_actor(L, 1);
		const auto *component = luaL_checkstring(L, 2);
		const auto *name = luaL_checkstring(L, 3);

		if (find_component(actor, component, prius, data, type) == nullptr) {
			luaL_error(L, "this actor has no %s component", component);
		}

		if (*prius == nullptr) {
			luaL_error(L, "%s carries no prius data", component);
		}

		if (*data == nullptr) {
			luaL_error(L, "the %s prius instance is not readable", component);
		}

		const auto index = ddl::find_field(*prius, name);
		if (index < 0) {
			luaL_error(L, "%s has no field called %s", component, name);
		}

		return index;
	}

	// one decoded value. a string or file field also returns its hash or asset id,
	// because neither is recoverable from the text. that id is handed back as hex
	// text, not a number: asset ids run past 2^53, where a lua number stops
	// counting by ones and starts rounding silently.
	static auto
	push_value(lua_State *L, const ddl::Value &value) -> int {
		switch (value.kind) {
			case ddl::ValueKind::Unsigned:
				{
					if (value.as_unsigned <= static_cast<uint64_t>(INT64_MAX)) {
						lua_pushinteger(L, static_cast<lua_Integer>(value.as_unsigned));
					} else {
						lua_pushnumber(L, static_cast<lua_Number>(value.as_unsigned));
					}

					return 1;
				}
			case ddl::ValueKind::Signed: lua_pushinteger(L, static_cast<lua_Integer>(value.as_signed)); return 1;
			case ddl::ValueKind::Real: lua_pushnumber(L, value.as_real); return 1;
			case ddl::ValueKind::Bool: lua_pushboolean(L, value.as_bool ? 1 : 0); return 1;
			case ddl::ValueKind::String:
			case ddl::ValueKind::File:
				{
					char text[0x300];
					if (!ddl::read_string(value.text, text, sizeof(text))) {
						lua_pushnil(L);
					} else {
						lua_pushstring(L, text);
					}

					char id[24];
					_snprintf_s(id, sizeof(id), _TRUNCATE, "%016llx", value.text_id);
					lua_pushstring(L, id);
					return 2;
				}
			default: lua_pushnil(L); return 1;
		}
	}

	// whatever lua was handed, as a ddl value the writer can convert
	static auto
	check_value(lua_State *L, const int arg) -> ddl::Value {
		ddl::Value value {};
		if (lua_isboolean(L, arg)) {
			value.kind = ddl::ValueKind::Bool;
			value.as_bool = lua_toboolean(L, arg) != 0;
			return value;
		}

		value.kind = ddl::ValueKind::Real;
		value.as_real = static_cast<double>(luaL_checknumber(L, arg));
		return value;
	}

	// ------------------------------------------------------------ the rivet --

	static auto
	l_log(lua_State *L) -> int {
		const auto count = lua_gettop(L);

		char line[0x400];
		line[0] = '\0';
		size_t at = 0;

		for (auto i = 1; i <= count; ++i) {
			if (i > 1) {
				append(line, sizeof(line), at, "\t", 1);
			}

			size_t length = 0;
			const auto *text = luaL_tolstring(L, i, &length);
			append(line, sizeof(line), at, text, length);
			lua_pop(L, 1);
		}

		g_output << "[script] " << line << "\n";
		g_output.flush();
		return 0;
	}

	static auto
	describe_caller(lua_State *L, char *out, const size_t size) -> void {
		lua_Debug frame {};
		if (lua_getstack(L, 1, &frame) && lua_getinfo(L, "Sl", &frame)) {
			_snprintf_s(out, size, _TRUNCATE, "%s:%d", frame.short_src, frame.currentline);
			return;
		}

		_snprintf_s(out, size, _TRUNCATE, "%s", "an unknown script");
	}

	static auto
	add_callback(lua_State *L, Callback *list, int &count, const int key, const int argument) -> void {
		luaL_checktype(L, argument, LUA_TFUNCTION);
		if (count >= MAX_CALLBACKS) {
			luaL_error(L, "too many callbacks registered, the limit is %d", MAX_CALLBACKS);
		}

		auto &entry = list[count];
		entry.key = key;
		entry.errors = 0;
		entry.disabled = false;
		describe_caller(L, entry.source, sizeof(entry.source));

		lua_pushvalue(L, argument);
		entry.ref = luaL_ref(L, LUA_REGISTRYINDEX);
		++count;
	}

	static auto
	l_on_frame(lua_State *L) -> int {
		add_callback(L, g_frame_callbacks, g_frame_count, 0, 1);
		return 0;
	}

	static auto
	l_on_key(lua_State *L) -> int {
		const auto key = static_cast<int>(luaL_checkinteger(L, 1));
		add_callback(L, g_key_callbacks, g_key_count, key, 2);
		return 0;
	}

	static auto
	l_is_key_down(lua_State *L) -> int {
		const auto key = static_cast<int>(luaL_checkinteger(L, 1));
		lua_pushboolean(L, (GetAsyncKeyState(key) & 0x8000) != 0 ? 1 : 0);
		return 1;
	}

	// the same names rivet.toml accepts, so a script and the config agree
	static auto
	l_key(lua_State *L) -> int {
		const auto *name = luaL_checkstring(L, 1);

		const auto key = StringToVKey(name);
		if (key == 0) {
			luaL_error(L, "there is no virtual key called %s", name);
		}

		lua_pushinteger(L, key);
		return 1;
	}

	static auto
	l_scene_ready(lua_State *L) -> int {
		lua_pushboolean(L, scene_ready() ? 1 : 0);
		return 1;
	}

	static auto
	l_time(lua_State *L) -> int {
		lua_pushnumber(L, ticks_to_ms(now_ticks() - g_started_at) / 1000.0);
		return 1;
	}

	static auto
	l_frame(lua_State *L) -> int {
		lua_pushinteger(L, static_cast<lua_Integer>(g_frames));
		return 1;
	}

	// an exact name wins over a substring, and only the first substring match is
	// kept as the fallback. a bare substring is not good enough on its own: in
	// Megalopolis "Rivet" matches test_npc_Rivet_Cine first, which sits earlier in
	// the array and carries no components at all, so every lookup for the hero
	// found the wrong actor.
	//
	// a miss and a scene that is not up yet are both reported as nil, because a
	// script does the same thing either way: wait and look again. raising instead
	// would switch off any per frame lookup during a level load, since three
	// consecutive errors disable a callback. rivet.scene_ready tells them apart.
	static auto
	l_find_actor(lua_State *L) -> int {
		const auto *wanted = luaL_checkstring(L, 1);
		if (!scene_ready()) {
			lua_pushnil(L);
			return 1;
		}

		auto fallback = -1;
		const Actor *fallback_actor = nullptr;

		const auto count = g_SceneManager->actorMax;
		for (int32_t index = 0; index < count; ++index) {
			// a loaded level holds tens of thousands of actors and this runs inside
			// the frame, so the scan answers to the same budget everything else does
			if ((index & SCAN_CHECK_MASK) == 0 && budget_expired()) {
				luaL_error(L, "the actor scan ran past the frame budget after %d of %d actors", index, count);
			}

			const auto *actor = &g_SceneManager->actors[index];
			if (!actor->IsValid()) {
				continue;
			}

			char name[0x100];
			if (!ddl::read_string(actor->GetName(), name, sizeof(name))) {
				continue;
			}

			if (strcmp(name, wanted) == 0) {
				lua_pushinteger(L, handle_of(index, actor));
				return 1;
			}

			if (fallback < 0 && strstr(name, wanted) != nullptr) {
				fallback = index;
				fallback_actor = actor;
			}
		}

		if (fallback_actor != nullptr) {
			lua_pushinteger(L, handle_of(fallback, fallback_actor));
			return 1;
		}

		lua_pushnil(L);
		return 1;
	}

	// the actor the game itself treats as the player, straight from the hero
	// system rather than by name. nil while there is none (menus, loads).
	static auto
	l_hero(lua_State *L) -> int {
		const auto handle = scene_ready() ? scene_query::hero() : 0;
		if (handle == 0) {
			lua_pushnil(L);
			return 1;
		}

		lua_pushinteger(L, handle);
		return 1;
	}

	// every actor holding a live component of the class, or of a class derived
	// from it unless exact is set. walks the engine's component index, not the
	// actor slots, so it costs a fraction of find_actor.
	static auto
	l_find_component(lua_State *L) -> int {
		constexpr int32_t MAX_FOUND = 1024;

		const auto *name = luaL_checkstring(L, 1);
		const auto limit = static_cast<int32_t>(luaL_optinteger(L, 2, 64));
		const auto exact = lua_toboolean(L, 3) != 0;
		if (limit < 1 || limit > MAX_FOUND) {
			luaL_error(L, "limit must be between 1 and %d", MAX_FOUND);
		}

		lua_newtable(L);
		if (!scene_ready()) {
			return 1;
		}

		const auto *type = scene_query::find_class(name);
		if (type == nullptr) {
			luaL_error(L, "there is no component class called %s", name);
		}

		uint32_t found[MAX_FOUND];
		const auto count = scene_query::actors_with(type, !exact, found, limit, budget_expired);
		if (count < 0) {
			luaL_error(L, "the component scan ran past the frame budget");
		}

		for (int32_t i = 0; i < count; ++i) {
			lua_pushinteger(L, found[i]);
			lua_rawseti(L, -2, i + 1);
		}

		return 1;
	}

	// the actor's uid as 16 digit hex text, or nil for an actor without one.
	// text for the same reason asset ids are: uids run past 2^53.
	static auto
	l_uid(lua_State *L) -> int {
		const auto *actor = resolve_actor(L, 1);
		const auto uid = scene_query::uid_of(actor);
		if (uid == 0) {
			lua_pushnil(L);
			return 1;
		}

		char text[24];
		_snprintf_s(text, sizeof(text), _TRUNCATE, "%016llx", uid);
		lua_pushstring(L, text);
		return 1;
	}

	// the loaded actor with this uid, or nil. takes the hex text rivet.uid hands
	// out (a 0x prefix is fine) or an integer.
	static auto
	l_find_uid(lua_State *L) -> int {
		uint64_t uid = 0;
		if (lua_type(L, 1) == LUA_TNUMBER) {
			uid = static_cast<uint64_t>(luaL_checkinteger(L, 1));
		} else {
			const auto *text = luaL_checkstring(L, 1);
			char *end = nullptr;
			uid = _strtoui64(text, &end, 16);
			if (end == text || *end != '\0') {
				luaL_error(L, "could not parse the uid, it has to be hex");
			}
		}

		const auto handle = scene_ready() ? scene_query::actor_by_uid(uid) : 0;
		if (handle == 0) {
			lua_pushnil(L);
			return 1;
		}

		lua_pushinteger(L, handle);
		return 1;
	}

	static auto
	l_actors(lua_State *L) -> int {
		const auto *filter = lua_isnoneornil(L, 1) ? nullptr : luaL_checkstring(L, 1);
		const auto limit = static_cast<int32_t>(luaL_optinteger(L, 2, 64));

		lua_newtable(L);
		if (!scene_ready()) {
			return 1;
		}

		const auto count = g_SceneManager->actorMax;
		int32_t matched = 0;
		for (int32_t index = 0; index < count && matched < limit; ++index) {
			if ((index & SCAN_CHECK_MASK) == 0 && budget_expired()) {
				luaL_error(L, "the actor scan ran past the frame budget after %d of %d actors", index, count);
			}

			const auto *actor = &g_SceneManager->actors[index];
			if (!actor->IsValid()) {
				continue;
			}

			if (filter != nullptr) {
				char name[0x100];
				if (!ddl::read_string(actor->GetName(), name, sizeof(name)) || strstr(name, filter) == nullptr) {
					continue;
				}
			}

			lua_pushinteger(L, handle_of(index, actor));
			lua_rawseti(L, -2, ++matched);
		}

		return 1;
	}

	static auto
	l_name(lua_State *L) -> int {
		const auto *actor = resolve_actor(L, 1);

		char name[0x100];
		if (!ddl::read_string(actor->GetName(), name, sizeof(name))) {
			lua_pushnil(L);
			return 1;
		}

		lua_pushstring(L, name);
		return 1;
	}

	static auto
	l_position(lua_State *L) -> int {
		const auto *actor = resolve_actor(L, 1);
		if (actor->object == nullptr || !ddl::is_readable(actor->object, sizeof(SceneObject))) {
			luaL_error(L, "this actor has no readable scene object");
		}

		for (auto axis = 0; axis < 3; ++axis) {
			lua_pushnumber(L, actor->object->transform_matrix[3][axis]);
		}

		return 3;
	}

	// The actor's orientation, as the three basis rows of its transform. Row 3 is
	// the translation that rivet.position reads, so rows 0..2 are the axes.
	// All three are returned rather than a single 'forward' because which row is
	// forward is a convention question, and a script can settle it by looking at
	// the numbers instead of the caller guessing.
	static auto
	l_basis(lua_State *L) -> int {
		const auto *actor = resolve_actor(L, 1);
		if (actor->object == nullptr || !ddl::is_readable(actor->object, sizeof(SceneObject))) {
			luaL_error(L, "this actor has no readable scene object");
		}

		for (auto row = 0; row < 3; ++row) {
			for (auto axis = 0; axis < 3; ++axis) {
				lua_pushnumber(L, actor->object->transform_matrix[row][axis]);
			}
		}

		return 9;
	}

	// the transform is a byproduct: the write lands and the engine stamps over it
	// within the frame. useful for a nudge, not for holding a position.
	static auto
	l_set_position(lua_State *L) -> int {
		auto *actor = resolve_actor(L, 1);

		float position[3];
		for (auto axis = 0; axis < 3; ++axis) {
			position[axis] = static_cast<float>(luaL_checknumber(L, 2 + axis));
		}

		if (actor->object == nullptr || !ddl::is_writable(actor->object, sizeof(SceneObject))) {
			luaL_error(L, "this actor has no writable scene object");
		}

		memcpy(&actor->object->transform_matrix[3], position, sizeof(position));
		return 0;
	}

	static auto
	l_components(lua_State *L) -> int {
		const auto *actor = resolve_actor(L, 1);
		if (actor->components == nullptr || actor->componentCount <= 0 || !ddl::is_readable(actor->components, sizeof(ComponentPointer) * actor->componentCount)) {
			luaL_error(L, "this actor has no readable component list");
		}

		lua_newtable(L);

		int32_t written = 0;
		for (auto index = 0; index < actor->componentCount; ++index) {
			const auto [type, instance] = actor->components[index];
			if (type == nullptr || !ddl::is_readable(type, sizeof(ComponentInfo))) {
				continue;
			}

			if (instance == nullptr || !ddl::is_readable(instance, sizeof(Component)) || instance->IsDestroyed()) {
				continue;
			}

			char name[0x100];
			if (!ddl::read_string(type->name, name, sizeof(name))) {
				continue;
			}

			lua_pushstring(L, name);
			lua_rawseti(L, -2, ++written);
		}

		return 1;
	}

	static auto
	l_field(lua_State *L) -> int {
		const DDLTypeInfo *prius = nullptr;
		uint8_t *data = nullptr;
		const auto index = resolve_field(L, &prius, &data);
		const auto element = static_cast<int32_t>(luaL_optinteger(L, 4, 1)) - 1;

		return push_value(L, ddl::read_field(prius, data, index, element));
	}

	// prius is authored config that nothing recomputes per frame, so unlike the
	// transform these writes stick. what consumes them is another question: a field
	// read once at component init will not notice.
	static auto
	l_set_field(lua_State *L) -> int {
		const DDLTypeInfo *prius = nullptr;
		uint8_t *data = nullptr;
		const ComponentInfo *type = nullptr;
		const auto index = resolve_field(L, &prius, &data, &type);
		const auto value = check_value(L, 4);
		const auto element = static_cast<int32_t>(luaL_optinteger(L, 5, 1)) - 1;

		const auto previous = ddl::read_field(prius, data, index, element);

		const char *reason = "the write was refused";
		if (!ddl::write_field(prius, data, index, element, value, &reason)) {
			luaL_error(L, "%s", reason);
		}

		// a ReadOnly prius is one allocation behind every instance of the actor, so
		// the write just changed all of them. said once per class, not per frame.
		if (type != nullptr && type->prius_behavior == PriusBehavior::ReadOnly) {
			static std::unordered_set<const ComponentInfo *> warned;
			if (warned.insert(type).second) {
				g_output << "[script] set_field: " << lua_tostring(L, 2) << " has a shared prius, the write changes every instance that uses it\n";
				g_output.flush();
			}
		}

		return push_value(L, previous);
	}

	// where a component instance actually lives. plenty of runtime state - which
	// skin is equipped, for one - is held in the instance rather than in the
	// authored prius, and rivet.field cannot see any of it. addresses come back as
	// hex text because they run past what a lua number counts exactly.
	static auto
	l_component(lua_State *L) -> int {
		const auto *actor = resolve_actor(L, 1);
		const auto *name = luaL_checkstring(L, 2);

		const DDLTypeInfo *prius = nullptr;
		uint8_t *data = nullptr;
		const ComponentInfo *type = nullptr;
		const auto *instance = find_component(actor, name, &prius, &data, &type);
		if (instance == nullptr) {
			luaL_error(L, "this actor has no %s component", name);
		}

		char text[32];
		lua_newtable(L);

		_snprintf_s(text, sizeof(text), _TRUNCATE, "%016llx", reinterpret_cast<uintptr_t>(instance));
		lua_pushstring(L, text);
		lua_setfield(L, -2, "address");

		lua_pushinteger(L, type != nullptr ? type->size : 0);
		lua_setfield(L, -2, "size");

		lua_pushinteger(L, instance->handle.value);
		lua_setfield(L, -2, "handle");

		if (type != nullptr) {
			lua_pushstring(L, PriusBehaviorName(type->prius_behavior));
			lua_setfield(L, -2, "prius_behavior");
			lua_pushboolean(L, type->prius_behavior == PriusBehavior::ReadOnly ? 1 : 0);
			lua_setfield(L, -2, "shared");
		}

		if (data != nullptr) {
			_snprintf_s(text, sizeof(text), _TRUNCATE, "%016llx", reinterpret_cast<uintptr_t>(data));
			lua_pushstring(L, text);
			lua_setfield(L, -2, "prius");
			lua_pushinteger(L, prius->allocation_size);
			lua_setfield(L, -2, "prius_size");
		}

		return 1;
	}

	// raw bytes as hex text, for diffing a live instance against itself over time.
	// bounded hard: this runs inside the frame and is meant for watching a struct,
	// not for trawling the address space.
	static auto
	l_read(lua_State *L) -> int {
		const auto *address_text = luaL_checkstring(L, 1);
		const auto length = static_cast<uint32_t>(luaL_checkinteger(L, 2));
		if (length == 0 || length > MAX_READ_BYTES) {
			luaL_error(L, "length must be between 1 and %d", MAX_READ_BYTES);
		}

		char *end = nullptr;
		const auto address = static_cast<uintptr_t>(_strtoui64(address_text, &end, 16));
		if (end == address_text || address == 0) {
			luaL_error(L, "could not parse the address, it has to be hex");
		}

		char text[MAX_READ_BYTES * 3 + 1];
		{
			// hex_dump owns a std::string, so it is copied out and gone before
			// anything below can raise
			const auto dump = ddl::hex_dump(reinterpret_cast<const uint8_t *>(address), length);
			_snprintf_s(text, sizeof(text), _TRUNCATE, "%s", dump.c_str());
		}

		if (text[0] == '\0') {
			lua_pushnil(L);
			return 1;
		}

		lua_pushstring(L, text);
		return 1;
	}

	// writes raw bytes into a live instance. this is the blunt counterpart to
	// set_field: set_field knows the field's width and range and refuses anything
	// that does not fit, and this knows nothing at all. it exists because runtime
	// state that no prius describes cannot be reached any other way, and proving
	// what a field does means writing it. confirmed writable before the store, but
	// nothing checks that the bytes mean anything.
	static auto
	l_write(lua_State *L) -> int {
		const auto *address_text = luaL_checkstring(L, 1);
		const auto *bytes_text = luaL_checkstring(L, 2);

		char *parsed = nullptr;
		const auto address = static_cast<uintptr_t>(_strtoui64(address_text, &parsed, 16));
		if (parsed == address_text || address == 0) {
			luaL_error(L, "could not parse the address, it has to be hex");
		}

		uint8_t bytes[MAX_WRITE_BYTES];
		uint32_t count = 0;
		for (const auto *cursor = bytes_text; *cursor != '\0';) {
			if (*cursor == ' ') {
				++cursor;
				continue;
			}

			if (count >= MAX_WRITE_BYTES) {
				luaL_error(L, "at most %d bytes per write", static_cast<int>(MAX_WRITE_BYTES));
			}

			if (!is_hex(cursor[0]) || !is_hex(cursor[1])) {
				luaL_error(L, "the bytes have to be space separated hex pairs");
			}

			bytes[count++] = static_cast<uint8_t>(hex_value(cursor[0]) * 16 + hex_value(cursor[1]));
			cursor += 2;
		}

		if (count == 0) {
			luaL_error(L, "there were no bytes to write");
		}

		auto *at = reinterpret_cast<uint8_t *>(address);
		if (!ddl::is_writable(at, count)) {
			luaL_error(L, "%s is not writable", address_text);
		}

		memcpy(at, bytes, count);
		lua_pushinteger(L, count);
		return 1;
	}

	static auto
	l_detour(lua_State *L) -> int {
		const auto *component = luaL_checkstring(L, 1);
		const auto *slot = luaL_checkstring(L, 2);
		const auto skip = lua_toboolean(L, 3) != 0;

		char error[0x200];
		if (!bridge::set_detour(component, slot, skip, error, sizeof(error))) {
			luaL_error(L, "%s", error);
		}

		lua_pushboolean(L, 1);
		return 1;
	}

	static auto
	l_dump(lua_State *L) -> int {
		auto *actor = resolve_actor(L, 1);

		// DumpActor hands back a std::string, so it is copied out and destroyed
		// before anything below can raise
		char path[0x120];
		{
			const auto written = DumpActor(actor);
			_snprintf_s(path, sizeof(path), _TRUNCATE, "%s", written.c_str());
		}

		if (path[0] == '\0') {
			luaL_error(L, "the dump failed, see rivet.log");
		}

		lua_pushstring(L, path);
		return 1;
	}

	// publishes text into a ui slot the hud document can poll. a cohtml view
	// cannot be pushed to from here - there is no View pointer to call
	// TriggerEvent on - so the page fetches a file instead and this is what
	// keeps that file current. answers whether the slot took it.
	static auto
	l_ui_publish(lua_State *L) -> int {
		const auto slot = static_cast<int>(luaL_checkinteger(L, 1));
		size_t length = 0;
		const auto *text = luaL_checklstring(L, 2, &length);
		lua_pushboolean(L, AssetLoader::publish_ui_slot(slot, text, length) ? 1 : 0);
		return 1;
	}

	// ----------------------------------------------------------------- events --

	constexpr int MAX_EVENT_TARGETS = 64;
	constexpr int MAX_EVENT_FIELDS = 32;
	constexpr int MAX_EVENT_DEPTH = 4;

	// pushes a live DDL object as a table of its readable fields, nested structs as
	// tables. dynamic arrays and maps are left out: rivet.event_dump has them.
	static auto
	push_object(lua_State *L, const DDLTypeInfo *type, const uint8_t *object, const int depth) -> void {
		lua_newtable(L);
		if (type == nullptr || object == nullptr || type->field_names == nullptr) {
			return;
		}

		for (int32_t index = 0; index < type->field_count; ++index) {
			char name[0x100];
			if (!ddl::read_string(type->field_names[index], name, sizeof(name))) {
				continue;
			}

			const auto field = ddl::field_at(type, index);
			if (field.array_type != ddl::ArrayType::Scalar) {
				continue;
			}

			if (field.type == ddl::FieldType::Struct) {
				if (depth >= MAX_EVENT_DEPTH || type->field_type_ids == nullptr) {
					continue;
				}

				const auto *nested = ddl::find_type(type->field_type_ids[index]);
				const auto *at = object + type->field_offsets[index];
				if (nested == nullptr || !ddl::is_readable(at, nested->allocation_size)) {
					continue;
				}

				push_object(L, nested, at, depth + 1);
				lua_setfield(L, -2, name);
				continue;
			}

			// a string field pushes its text and its hash; the table keeps the text
			const auto pushed = push_value(L, ddl::read_field(type, object, index));
			if (pushed > 1) {
				lua_pop(L, pushed - 1);
			}

			lua_setfield(L, -2, name);
		}
	}

	// the table an rivet.on_event callback receives
	static auto
	push_event(lua_State *L, const EventEntry &entry) -> void {
		const auto *info = events::class_at(entry.class_id);

		lua_newtable(L);

		lua_pushstring(L, events::class_name(info));
		lua_setfield(L, -2, "class");

		lua_pushinteger(L, events::sender_of(entry));
		lua_setfield(L, -2, "sender");

		lua_newtable(L);
		const auto count = entry.target_count <= MAX_EVENT_TARGETS ? entry.target_count : 0;
		if (count == 1 || (count > 1 && ddl::is_readable(entry.targets, sizeof(uint32_t) * count))) {
			for (uint32_t i = 0; i < count; ++i) {
				lua_pushinteger(L, entry.target(i));
				lua_rawseti(L, -2, static_cast<lua_Integer>(i) + 1);
			}
		}

		lua_setfield(L, -2, "targets");

		lua_pushboolean(L, (entry.flags & EVENT_BROADCAST) != 0 ? 1 : 0);
		lua_setfield(L, -2, "broadcast");

		char address[24];
		_snprintf_s(address, sizeof(address), _TRUNCATE, "%016llx", reinterpret_cast<uintptr_t>(entry.event));
		lua_pushstring(L, address);
		lua_setfield(L, -2, "address");

		if (info != nullptr && ddl::is_readable(entry.event, info->type_info->allocation_size)) {
			push_object(L, info->type_info, static_cast<const uint8_t *>(entry.event), 0);
		} else {
			lua_newtable(L);
		}

		lua_setfield(L, -2, "fields");
	}

	static auto
	check_event_class(lua_State *L, const int arg) -> const EventClassInfo * {
		const auto *name = luaL_checkstring(L, arg);
		if (!events::ready()) {
			luaL_error(L, "events are unavailable: %s", events::unavailable_reason());
		}

		const auto *info = events::find_class(name);
		if (info == nullptr) {
			luaL_error(L, "there is no event class called %s", name);
		}

		return info;
	}

	// rivet.on_event(name, fn): fn(ev) for every event of the class, or of a class
	// derived from it, that goes through the main queue. "EventBase" sees them all.
	static auto
	l_on_event(lua_State *L) -> int {
		const auto *info = check_event_class(L, 1);
		add_callback(L, g_event_callbacks, g_event_count, info->id, 2);
		return 0;
	}

	static auto
	check_handle(lua_State *L, const int index, const char *what) -> uint32_t {
		if (!lua_isinteger(L, index)) {
			luaL_error(L, "%s has to be an actor handle", what);
		}

		return static_cast<uint32_t>(lua_tointeger(L, index));
	}

	// rivet.queue_event(name, {target, targets, sender, broadcast, exclude, radius,
	// delay, position = {x, y, z}, fields = {["Destination.Position.X"] = 1}}).
	// returns the address of the queued event as hex text.
	static auto
	l_queue_event(lua_State *L) -> int {
		const auto *info = check_event_class(L, 1);

		// everything is read into plain storage before the request is built: a
		// raise from inside a std::vector's lifetime would skip its destructor
		uint32_t targets[MAX_EVENT_TARGETS];
		int target_count = 0;
		uint32_t sender = 0;
		bool broadcast = true;
		bool broadcast_given = false;
		bool exclude = false;
		float radius = 0.0f;
		float delay = 0.0f;
		bool has_position = false;
		float position[4] {};
		const char *paths[MAX_EVENT_FIELDS];
		ddl::Value values[MAX_EVENT_FIELDS];
		int field_count = 0;

		if (!lua_isnoneornil(L, 2)) {
			luaL_checktype(L, 2, LUA_TTABLE);

			if (lua_getfield(L, 2, "target") != LUA_TNIL) {
				targets[target_count++] = check_handle(L, -1, "target");
			}

			lua_pop(L, 1);

			if (lua_getfield(L, 2, "targets") != LUA_TNIL) {
				luaL_checktype(L, -1, LUA_TTABLE);
				const auto count = static_cast<int>(lua_rawlen(L, -1));
				if (count + target_count > MAX_EVENT_TARGETS) {
					luaL_error(L, "at most %d targets", MAX_EVENT_TARGETS);
				}

				for (auto i = 1; i <= count; ++i) {
					lua_rawgeti(L, -1, i);
					targets[target_count++] = check_handle(L, -1, "every target");
					lua_pop(L, 1);
				}
			}

			lua_pop(L, 1);

			if (lua_getfield(L, 2, "sender") != LUA_TNIL) {
				sender = check_handle(L, -1, "sender");
			}

			lua_pop(L, 1);

			if (lua_getfield(L, 2, "broadcast") != LUA_TNIL) {
				broadcast = lua_toboolean(L, -1) != 0;
				broadcast_given = true;
			}

			lua_pop(L, 1);

			lua_getfield(L, 2, "exclude");
			exclude = lua_toboolean(L, -1) != 0;
			lua_pop(L, 1);

			if (lua_getfield(L, 2, "radius") != LUA_TNIL) {
				radius = static_cast<float>(luaL_checknumber(L, -1));
			}

			lua_pop(L, 1);

			if (lua_getfield(L, 2, "delay") != LUA_TNIL) {
				delay = static_cast<float>(luaL_checknumber(L, -1));
			}

			lua_pop(L, 1);

			if (lua_getfield(L, 2, "position") != LUA_TNIL) {
				luaL_checktype(L, -1, LUA_TTABLE);
				for (auto axis = 0; axis < 3; ++axis) {
					lua_rawgeti(L, -1, axis + 1);
					position[axis] = static_cast<float>(luaL_checknumber(L, -1));
					lua_pop(L, 1);
				}

				has_position = true;
			}

			lua_pop(L, 1);

			// the path strings stay alive: the fields table is left on the stack
			// until the writes are done
			if (lua_getfield(L, 2, "fields") != LUA_TNIL) {
				luaL_checktype(L, -1, LUA_TTABLE);
				lua_pushnil(L);
				while (lua_next(L, -2) != 0) {
					if (lua_type(L, -2) != LUA_TSTRING) {
						luaL_error(L, "fields are keyed by name");
					}

					if (field_count >= MAX_EVENT_FIELDS) {
						luaL_error(L, "at most %d fields", MAX_EVENT_FIELDS);
					}

					const auto *path = lua_tostring(L, -2);
					if (!events::has_field(info, path)) {
						luaL_error(L, "%s has no field %s", events::class_name(info), path);
					}

					if (!lua_isboolean(L, -1) && !lua_isnumber(L, -1)) {
						luaL_error(L, "field %s has to be a number or a boolean", path);
					}

					paths[field_count] = path;
					values[field_count] = check_value(L, -1);
					++field_count;
					lua_pop(L, 1);
				}
			}
		}

		if (!broadcast_given) {
			broadcast = target_count == 0;
		}

		const char *reason = nullptr;
		uint8_t *event = nullptr;
		{
			events::Request request;
			request.sender = sender;
			request.targets.assign(targets, targets + target_count);
			request.broadcast = broadcast;
			request.exclude_targets = exclude;
			request.radius = radius;
			request.delay = delay;
			request.has_position = has_position;
			memcpy(request.position, position, sizeof(position));
			event = events::queue(info, request, &reason);
		}

		if (event == nullptr) {
			luaL_error(L, "%s was not queued: %s", events::class_name(info), reason != nullptr ? reason : "unknown reason");
		}

		for (auto i = 0; i < field_count; ++i) {
			const char *why = "the write was refused";
			if (!events::set_field(info, event, paths[i], values[i], &why)) {
				luaL_error(L, "%s was queued, but %s could not be set: %s", events::class_name(info), paths[i], why);
			}
		}

		char address[24];
		_snprintf_s(address, sizeof(address), _TRUNCATE, "%016llx", reinterpret_cast<uintptr_t>(event));
		lua_pushstring(L, address);
		return 1;
	}

	static const luaL_Reg g_api[] = {
		{ "log", l_log },
		{ "on_frame", l_on_frame },
		{ "on_key", l_on_key },
		{ "is_key_down", l_is_key_down },
		{ "key", l_key },
		{ "time", l_time },
		{ "frame", l_frame },
		{ "scene_ready", l_scene_ready },
		{ "find_actor", l_find_actor },
		{ "actors", l_actors },
		{ "hero", l_hero },
		{ "uid", l_uid },
		{ "find_uid", l_find_uid },
		{ "find_component", l_find_component },
		{ "name", l_name },
		{ "position", l_position },
		{ "basis", l_basis },
		{ "set_position", l_set_position },
		{ "components", l_components },
		{ "field", l_field },
		{ "set_field", l_set_field },
		{ "component", l_component },
		{ "read", l_read },
		{ "write", l_write },
		{ "detour", l_detour },
		{ "dump", l_dump },
		{ "ui_publish", l_ui_publish },
		{ "on_event", l_on_event },
		{ "queue_event", l_queue_event },
		{ nullptr, nullptr },
	};

	// ---------------------------------------------------------------- the vm --

	// package, io and debug stay shut. a script that wants the disk or a process
	// is not what this is for, and require of a c module would pull an arbitrary
	// dll into the game's address space.
	static auto
	open_libraries(lua_State *L) -> void {
		static const luaL_Reg libraries[] = {
			{ LUA_GNAME, luaopen_base },
			{ LUA_TABLIBNAME, luaopen_table },
			{ LUA_STRLIBNAME, luaopen_string },
			{ LUA_MATHLIBNAME, luaopen_math },
			{ LUA_COLIBNAME, luaopen_coroutine },
			{ LUA_UTF8LIBNAME, luaopen_utf8 },
			{ LUA_OSLIBNAME, luaopen_os },
		};

		for (const auto &library : libraries) {
			luaL_requiref(L, library.name, library.func, 1);
			lua_pop(L, 1);
		}

		// os is opened for its clocks. the rest of it reaches outside the process.
		static const char *const os_removals[] = { "execute", "exit", "remove", "rename", "tmpname", "getenv", "setlocale" };
		lua_getglobal(L, LUA_OSLIBNAME);
		for (const auto *name : os_removals) {
			lua_pushnil(L);
			lua_setfield(L, -2, name);
		}

		lua_pop(L, 1);
	}

	// building the vm goes through pcall too: luaL_requiref and the table writes
	// can raise, and an error with no handler in place makes lua abort the process.
	static auto
	l_open(lua_State *L) -> int {
		open_libraries(L);

		luaL_newlib(L, g_api);
		lua_setglobal(L, "rivet");

		// there is no stdout to print to, so print goes where everything else goes
		lua_pushcfunction(L, l_log);
		lua_setglobal(L, "print");

		return 0;
	}

	static auto
	shutdown_state() -> void {
		if (g_state != nullptr) {
			lua_close(g_state);
			g_state = nullptr;
		}

		for (auto &entry : g_frame_callbacks) {
			entry = {};
		}

		for (auto &entry : g_key_callbacks) {
			entry = {};
		}

		for (auto &entry : g_event_callbacks) {
			entry = {};
		}

		g_frame_count = 0;
		g_key_count = 0;
		g_event_count = 0;
		g_scripts.clear();
	}

	static auto
	create_state() -> bool {
		g_state = luaL_newstate();
		if (g_state == nullptr) {
			record_error("could not create the lua state");
			return false;
		}

		lua_pushcfunction(g_state, l_open);
		if (!protected_call(g_state, 0, 0)) {
			shutdown_state();
			return false;
		}

		return true;
	}

	static auto
	load_scripts() -> void {
		std::error_code code;
		const std::filesystem::path directory { g_settings.scripts.path.empty() ? "scripts" : g_settings.scripts.path };
		if (!std::filesystem::is_directory(directory, code)) {
			g_output << "[script] there is no " << directory.string() << " directory, nothing to load\n";
			g_output.flush();
			return;
		}

		// sorted, so two scripts that touch the same thing do it in an order the
		// author can predict from the file names
		std::vector<std::filesystem::path> files;
		for (const auto &entry : std::filesystem::directory_iterator { directory, code }) {
			if (entry.is_regular_file(code) && entry.path().extension() == ".lua") {
				files.emplace_back(entry.path());
			}
		}

		std::sort(files.begin(), files.end());

		for (const auto &file : files) {
			Script record;
			record.name = file.filename().string();

			if (luaL_loadfile(g_state, file.string().c_str()) != LUA_OK) {
				const auto *message = lua_tostring(g_state, -1);
				record.error = message != nullptr ? message : "could not load the file";
				lua_pop(g_state, 1);
				record_error(record.error.c_str());
			} else if (protected_call(g_state, 0, 0)) {
				record.ok = true;
			} else {
				record.error = g_last_error;
			}

			g_scripts.emplace_back(std::move(record));
		}
	}

	// ------------------------------------------------------------- interface --

	auto
	reload() -> void {
		g_reload_pending = false;
		shutdown_state();

		// the frame cost describes the scripts that are loaded, so a high water
		// mark left over from a set that has just been replaced is worse than no
		// number at all. the lifetime counters stay: they describe the host.
		g_peak_ms = 0.0;
		g_last_ms = 0.0;

		if (!g_settings.scripts.enabled || !create_state()) {
			return;
		}

		load_scripts();

		auto loaded = 0;
		for (const auto &script : g_scripts) {
			loaded += script.ok ? 1 : 0;
		}

		g_output << "[script] loaded " << loaded << " of " << g_scripts.size() << " scripts, " << g_frame_count << " frame and " << g_key_count << " key callbacks\n";
		g_output.flush();
	}

	auto
	pump() -> void {
		if (!g_settings.scripts.enabled) {
			return;
		}

		if (g_reload_pending) {
			reload();
		}

		if (g_state == nullptr) {
			return;
		}

		// a cached readable region must never outlive the frame it was taken in
		ddl::reset_readable_cache();

		const auto started = now_ticks();
		const auto delta = g_last_frame_at == 0 ? 0.0 : ticks_to_ms(started - g_last_frame_at) / 1000.0;
		g_last_frame_at = started;
		++g_frames;

		int keys[MAX_KEY_QUEUE];
		auto queued = 0;
		{
			std::lock_guard guard { g_key_lock };
			queued = g_key_queued;
			memcpy(keys, g_key_queue, sizeof(int) * static_cast<size_t>(queued));
			g_key_queued = 0;
		}

		// the counts are read once: a callback that registers another one during
		// dispatch has it run from the next frame, not from inside this loop
		const auto key_count = g_key_count;
		for (auto i = 0; i < queued; ++i) {
			for (auto index = 0; index < key_count; ++index) {
				if (g_key_callbacks[index].key != keys[i]) {
					continue;
				}

				run_callback(g_key_callbacks[index], g_dispatched_key, [](lua_State *) { return 0; });
			}
		}

		// what events::poll picked up this pump. the entries stay valid until the
		// next poll, and the events they point at live two frames
		const auto event_count = g_event_count;
		if (event_count > 0) {
			for (const auto &entry : events::fresh()) {
				for (auto index = 0; index < event_count; ++index) {
					if (!events::is_type(entry.class_id, static_cast<uint16_t>(g_event_callbacks[index].key))) {
						continue;
					}

					run_callback(g_event_callbacks[index], g_dispatched_event, [&entry](lua_State *L) {
						push_event(L, entry);
						return 1;
					});
				}
			}
		}

		const auto frame_count = g_frame_count;
		for (auto index = 0; index < frame_count; ++index) {
			run_callback(g_frame_callbacks[index], g_dispatched_frame, [delta](lua_State *L) {
				lua_pushnumber(L, delta);
				return 1;
			});
		}

		g_last_ms = ticks_to_ms(now_ticks() - started);
		g_peak_ms = g_last_ms > g_peak_ms ? g_last_ms : g_peak_ms;
	}

	auto
	exec(const char *source, char *out, const size_t out_size) -> bool {
		const auto report = [out, out_size](const char *text) {
			_snprintf_s(out, out_size, _TRUNCATE, "%s", text);
			return false;
		};

		if (!g_settings.scripts.enabled) {
			return report("scripts are disabled, set [scripts] enabled = true in rivet.toml");
		}

		if (g_reload_pending) {
			reload();
		}

		if (g_state == nullptr) {
			return report("the script vm is not running, see rivet.log");
		}

		ddl::reset_readable_cache();

		if (luaL_loadstring(g_state, source) != LUA_OK) {
			const auto *message = lua_tostring(g_state, -1);
			_snprintf_s(out, out_size, _TRUNCATE, "%s", message != nullptr ? message : "could not compile the chunk");
			lua_pop(g_state, 1);
			return false;
		}

		if (!protected_call(g_state, 0, 1)) {
			return report(g_last_error);
		}

		if (lua_isnil(g_state, -1)) {
			_snprintf_s(out, out_size, _TRUNCATE, "ok");
		} else if (const auto *text = lua_tostring(g_state, -1); text != nullptr) {
			_snprintf_s(out, out_size, _TRUNCATE, "%s", text);
		} else {
			_snprintf_s(out, out_size, _TRUNCATE, "<%s>", luaL_typename(g_state, -1));
		}

		lua_pop(g_state, 1);
		return true;
	}

	static auto
	describe_callbacks(const Callback *list, const int count) -> nlohmann::json {
		nlohmann::json::array_t entries;
		for (auto index = 0; index < count; ++index) {
			nlohmann::json entry;
			entry["source"] = list[index].source;
			entry["errors"] = list[index].errors;
			entry["disabled"] = list[index].disabled;
			if (list[index].key != 0) {
				entry["key"] = list[index].key;
			}

			entries.emplace_back(entry);
		}

		return entries;
	}

	auto
	status() -> nlohmann::json {
		nlohmann::json result;
		result["enabled"] = g_settings.scripts.enabled;
		result["running"] = g_state != nullptr;
		result["path"] = g_settings.scripts.path;
		result["budget_ms"] = budget_ms();
		result["frames_pumped"] = g_frames;
		result["frame_callbacks_run"] = g_dispatched_frame;
		result["key_callbacks_run"] = g_dispatched_key;
		result["event_callbacks_run"] = g_dispatched_event;
		result["keys_dropped"] = g_keys_dropped;
		result["errors"] = g_errors;
		result["last_ms"] = g_last_ms;
		result["peak_ms"] = g_peak_ms;
		result["last_error"] = g_last_error;

		nlohmann::json::array_t scripts;
		for (const auto &script : g_scripts) {
			nlohmann::json entry;
			entry["name"] = script.name;
			entry["ok"] = script.ok;
			if (!script.error.empty()) {
				entry["error"] = script.error;
			}

			scripts.emplace_back(entry);
		}

		result["scripts"] = scripts;
		result["on_frame"] = describe_callbacks(g_frame_callbacks, g_frame_count);
		result["on_key"] = describe_callbacks(g_key_callbacks, g_key_count);

		auto on_event = describe_callbacks(g_event_callbacks, g_event_count);
		for (auto index = 0; index < g_event_count; ++index) {
			on_event[index].erase("key");
			on_event[index]["class"] = events::class_name(events::class_at(static_cast<uint16_t>(g_event_callbacks[index].key)));
		}

		result["on_event"] = on_event;
		return result;
	}

	auto
	on_key_event(const int vk) -> void {
		if (!g_settings.scripts.enabled) {
			return;
		}

		std::lock_guard guard { g_key_lock };
		if (g_key_queued >= MAX_KEY_QUEUE) {
			++g_keys_dropped;
			return;
		}

		g_key_queue[g_key_queued++] = vk;
	}

	auto
	request_reload() -> void {
		g_reload_pending = true;
	}

	auto
	init() -> void {
		LARGE_INTEGER frequency {};
		QueryPerformanceFrequency(&frequency);
		g_qpc_frequency = frequency.QuadPart;
		g_started_at = now_ticks();

		if (!g_settings.scripts.enabled) {
			return;
		}

		game_thread::install();

		// nothing is built here. the vm and every chunk in it come up on the first
		// pump, so a script's top level code runs on the render thread with the
		// same guarantees its callbacks get.
		g_reload_pending = true;
		g_output << "[script] enabled, loading from " << g_settings.scripts.path << " on the first frame\n";
		g_output.flush();
	}

	auto
	fini() -> void {
		shutdown_state();
	}
} // namespace rivet_hook::scripting

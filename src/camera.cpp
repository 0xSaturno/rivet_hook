// SPDX-FileCopyrightText: 2025-2026 Neptuwunium
//
// SPDX-License-Identifier: EUPL-1.2

#include <cmath>
#include <cstring>

#include "camera.hpp"

#include "ddl_visit.hpp"
#include "runtime.hpp"
#include "signature.hpp"

namespace rivet_hook::camera {
	// wide enough for any sane view, narrow enough that a typo cannot turn the
	// projection inside out
	constexpr float MIN_FOV_SCALE = 0.1f;
	constexpr float MAX_FOV_SCALE = 4.0f;

	// camera system layout, measured on the shipping exe
	constexpr uint32_t ROOTS_OFFSET = 0x0;		   // node *[view contexts], the game's cameras
	constexpr uint32_t DEBUG_ROOTS_OFFSET = 0x78;  // node *[view contexts], preferred by the renderer when set
	constexpr uint32_t VIEW_COUNT_OFFSET = 0xb0;   // int32
	constexpr uint32_t SHAKE_ALLOWED_OFFSET = 0xf8; // uint32, one bit per view context
	constexpr uint32_t SYSTEM_READ_SIZE = 0x100;

	// camera tree node layout. nodes live in pools with a 0x100 byte stride
	constexpr uint32_t NODE_SIZE = 0x100;
	constexpr uint32_t NODE_TYPE_OFFSET = 0x8; // 0 base, 1 camera, 2 blend
	constexpr uint32_t NODE_MATRIX_OFFSET = 0xc; // 4x4 floats: right, up, forward, position rows
	constexpr uint32_t NODE_FOV_OFFSET = 0xa4;	 // radians
	// a base node is the one type the engine's per frame refresh leaves alone,
	// and it has no camera actor behind it, so nothing mistakes it for a game one
	constexpr int32_t NODE_TYPE_BASE = 0;

	// the player's view
	constexpr int32_t VIEW_CONTEXT = 0;

	constexpr float DEGREES = 3.14159265358979f / 180.0f;
	constexpr float MAX_PITCH = 89.0f;

	static float *g_fov_scale = nullptr;
	static uint8_t *g_system = nullptr;

	// the free camera's node. static, so it outlives anything that might still
	// hold the pointer: the engine never frees a debug root, and neither does this
	alignas(16) static uint8_t g_node[NODE_SIZE] {};

	enum class ShakeOverride : uint8_t {
		None,
		Block,
		Allow,
	};

	static ShakeOverride g_shake_override = ShakeOverride::None;

	auto
	init() -> void {
		if (g_fov_scale == nullptr) {
			g_fov_scale = static_cast<float *>(load_rel_var(find_address(CAMERA_FOV_SCALE_SIGNATURE), CAMERA_FOV_SCALE_ADDRESS));
			if (g_fov_scale == nullptr) {
				g_output << "[camera] the fov scale was not found, rivet.fov_scale is unavailable\n";
			} else {
				g_output << "[camera] fov scale at " << static_cast<void *>(g_fov_scale) << "\n";
			}
		}

		if (g_system == nullptr) {
			// both matches load the same global
			g_system = static_cast<uint8_t *>(load_rel_var(find_address(CAMERA_SYSTEM_SIGNATURE, 2), CAMERA_SYSTEM_ADDRESS));
			if (g_system == nullptr) {
				g_output << "[camera] the camera system was not found, the free camera is unavailable\n";
			} else {
				g_output << "[camera] camera system at " << static_cast<void *>(g_system) << "\n";
			}
		}

		g_output.flush();
	}

	auto
	fov_unavailable_reason() -> const char * {
		if (g_fov_scale == nullptr) {
			return "the fov scale was not found";
		}

		return ddl::is_writable(g_fov_scale, sizeof(float)) ? "" : "the fov scale is not writable";
	}

	auto
	fov_scale() -> float {
		return fov_unavailable_reason()[0] == '\0' ? *g_fov_scale : 1.0f;
	}

	auto
	set_fov_scale(const float scale, const char **reason) -> bool {
		if (const auto *why = fov_unavailable_reason(); why[0] != '\0') {
			if (reason != nullptr) {
				*reason = why;
			}

			return false;
		}

		if (!std::isfinite(scale) || scale < MIN_FOV_SCALE || scale > MAX_FOV_SCALE) {
			if (reason != nullptr) {
				*reason = "the fov scale has to be between 0.1 and 4";
			}

			return false;
		}

		*g_fov_scale = scale;
		return true;
	}

	// ------------------------------------------------------------ free camera --

	template<typename T>
	static auto
	at(uint8_t *base, const uint32_t offset) -> T & {
		return *reinterpret_cast<T *>(base + offset);
	}

	// the slot holding the player's debug root, or null when the system does not
	// look like one
	static auto
	debug_slot() -> uint8_t ** {
		if (g_system == nullptr || !ddl::is_writable(g_system, SYSTEM_READ_SIZE)) {
			return nullptr;
		}

		if (at<int32_t>(g_system, VIEW_COUNT_OFFSET) <= VIEW_CONTEXT) {
			return nullptr;
		}

		auto **slots = at<uint8_t **>(g_system, DEBUG_ROOTS_OFFSET);
		if (!ddl::is_writable(slots + VIEW_CONTEXT, sizeof(uint8_t *))) {
			return nullptr;
		}

		return slots + VIEW_CONTEXT;
	}

	// the game's own camera node for the player's view
	static auto
	game_root() -> uint8_t * {
		if (debug_slot() == nullptr) {
			return nullptr;
		}

		auto **roots = at<uint8_t **>(g_system, ROOTS_OFFSET);
		if (!ddl::is_readable(roots + VIEW_CONTEXT, sizeof(uint8_t *))) {
			return nullptr;
		}

		auto *root = roots[VIEW_CONTEXT];
		return ddl::is_readable(root, NODE_SIZE) ? root : nullptr;
	}

	auto
	free_unavailable_reason() -> const char * {
		if (g_system == nullptr) {
			return "the camera system was not found";
		}

		if (debug_slot() == nullptr) {
			return "the camera system is not readable";
		}

		return game_root() == nullptr ? "the game camera is not readable" : "";
	}

	auto
	detached() -> bool {
		const auto *slot = debug_slot();
		return slot != nullptr && *slot == g_node;
	}

	static auto
	read_view(uint8_t *node, View &out) -> void {
		const auto *matrix = reinterpret_cast<const float *>(node + NODE_MATRIX_OFFSET);
		const auto *forward = matrix + 8;
		const auto *position = matrix + 12;

		out.position[0] = position[0];
		out.position[1] = position[1];
		out.position[2] = position[2];

		const auto up = forward[1] < -1.0f ? -1.0f : (forward[1] > 1.0f ? 1.0f : forward[1]);
		out.pitch = std::asin(up) / DEGREES;
		out.yaw = std::atan2(forward[0], forward[2]) / DEGREES;
		out.fov = at<float>(node, NODE_FOV_OFFSET) / DEGREES;
	}

	auto
	view(View &out) -> bool {
		if (detached()) {
			read_view(g_node, out);
			return true;
		}

		auto *root = game_root();
		if (root == nullptr) {
			return false;
		}

		read_view(root, out);
		return true;
	}

	auto
	detach(const char **reason) -> bool {
		const auto fail = [reason](const char *text) {
			if (reason != nullptr) {
				*reason = text;
			}

			return false;
		};

		if (const auto *why = free_unavailable_reason(); why[0] != '\0') {
			return fail(why);
		}

		if (detached()) {
			return true;
		}

		auto **slot = debug_slot();
		if (*slot != nullptr) {
			return fail("another debug camera already owns the view");
		}

		// a copy of the live camera carries a valid vtable, clip planes and fov.
		// turning it into a base node is what keeps the engine's hands off it
		memcpy(g_node, game_root(), NODE_SIZE);
		at<int32_t>(g_node, NODE_TYPE_OFFSET) = NODE_TYPE_BASE;

		*slot = g_node;
		return true;
	}

	auto
	attach() -> void {
		if (auto **slot = debug_slot(); slot != nullptr && *slot == g_node) {
			*slot = nullptr;
		}
	}

	auto
	set_view(const View &view, const char **reason) -> bool {
		if (!detached()) {
			if (reason != nullptr) {
				*reason = "the camera is not detached";
			}

			return false;
		}

		for (const auto value : { view.position[0], view.position[1], view.position[2], view.yaw, view.pitch, view.fov }) {
			if (!std::isfinite(value)) {
				if (reason != nullptr) {
					*reason = "every value has to be finite";
				}

				return false;
			}
		}

		const auto pitch = (view.pitch > MAX_PITCH ? MAX_PITCH : (view.pitch < -MAX_PITCH ? -MAX_PITCH : view.pitch)) * DEGREES;
		const auto yaw = view.yaw * DEGREES;

		// forward from yaw and pitch, right = world up x forward, up = forward x right.
		// the same construction reproduces the game camera's own rows exactly
		const float forward[3] = { std::sin(yaw) * std::cos(pitch), std::sin(pitch), std::cos(yaw) * std::cos(pitch) };
		const auto flat = std::sqrt(forward[0] * forward[0] + forward[2] * forward[2]);
		const float right[3] = { forward[2] / flat, 0.0f, -forward[0] / flat };
		const float up[3] = {
			forward[1] * right[2] - forward[2] * right[1],
			forward[2] * right[0] - forward[0] * right[2],
			forward[0] * right[1] - forward[1] * right[0],
		};

		const float matrix[16] = {
			right[0], right[1], right[2], 0.0f,
			up[0], up[1], up[2], 0.0f,
			forward[0], forward[1], forward[2], 0.0f,
			view.position[0], view.position[1], view.position[2], 1.0f,
		};

		memcpy(g_node + NODE_MATRIX_OFFSET, matrix, sizeof(matrix));
		if (view.fov > 0.0f) {
			at<float>(g_node, NODE_FOV_OFFSET) = (view.fov < 5.0f ? 5.0f : (view.fov > 170.0f ? 170.0f : view.fov)) * DEGREES;
		}

		return true;
	}

	static auto
	apply_shake() -> void {
		if (g_shake_override == ShakeOverride::None || g_system == nullptr || !ddl::is_writable(g_system, SYSTEM_READ_SIZE)) {
			return;
		}

		auto &allowed = at<uint32_t>(g_system, SHAKE_ALLOWED_OFFSET);
		constexpr auto bit = 1u << VIEW_CONTEXT;
		allowed = g_shake_override == ShakeOverride::Block ? allowed & ~bit : allowed | bit;
	}

	auto
	block_shake(const bool blocked, const char **reason) -> bool {
		if (g_system == nullptr || !ddl::is_writable(g_system, SYSTEM_READ_SIZE)) {
			if (reason != nullptr) {
				*reason = "the camera system is not available";
			}

			return false;
		}

		g_shake_override = blocked ? ShakeOverride::Block : ShakeOverride::Allow;
		apply_shake();
		return true;
	}

	auto
	release_shake() -> void {
		g_shake_override = ShakeOverride::None;
	}

	auto
	shake_overridden() -> bool {
		return g_shake_override != ShakeOverride::None;
	}

	auto
	pump() -> void {
		apply_shake();
	}

	auto
	shake_blocked() -> bool {
		if (g_system == nullptr || !ddl::is_readable(g_system, SYSTEM_READ_SIZE)) {
			return false;
		}

		return (at<uint32_t>(g_system, SHAKE_ALLOWED_OFFSET) & (1u << VIEW_CONTEXT)) == 0;
	}
} // namespace rivet_hook::camera

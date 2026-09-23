// SPDX-FileCopyrightText: 2025-2026 Neptuwunium
//
// SPDX-License-Identifier: EUPL-1.2

#include <cmath>

#include "camera.hpp"

#include "ddl_visit.hpp"
#include "runtime.hpp"
#include "signature.hpp"

namespace rivet_hook::camera {
	// wide enough for any sane view, narrow enough that a typo cannot turn the
	// projection inside out
	constexpr float MIN_FOV_SCALE = 0.1f;
	constexpr float MAX_FOV_SCALE = 4.0f;

	static float *g_fov_scale = nullptr;

	auto
	init() -> void {
		if (g_fov_scale != nullptr) {
			return;
		}

		g_fov_scale = static_cast<float *>(load_rel_var(find_address(CAMERA_FOV_SCALE_SIGNATURE), CAMERA_FOV_SCALE_ADDRESS));
		if (g_fov_scale == nullptr) {
			g_output << "[camera] the fov scale was not found, rivet.fov_scale is unavailable\n";
		} else {
			g_output << "[camera] fov scale at " << static_cast<void *>(g_fov_scale) << "\n";
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
} // namespace rivet_hook::camera

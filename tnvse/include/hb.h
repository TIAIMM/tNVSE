#pragma once

// HarfBuzz was removed from the game-font pipeline.  A few private runtime
// structures still carry the old opaque types while the legacy encoded-unit
// layout path is compiled, so keep only the ABI-free declarations required to
// build those structures.  No shaping implementation or external library is
// present.

#include <cstdint>

using hb_bool_t = int;

struct hb_font_t;

struct hb_feature_t
{
	std::uint32_t tag = 0;
	std::uint32_t value = 0;
	std::uint32_t start = 0;
	std::uint32_t end = 0;
};

inline hb_bool_t hb_feature_from_string(const char*, int, hb_feature_t*)
{
	return 0;
}

inline void hb_font_destroy(hb_font_t*)
{
}

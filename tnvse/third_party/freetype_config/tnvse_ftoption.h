#pragma once

#include <freetype/config/ftoption.h>

// tNVSE loads uncompressed outline fonts and does not use external renderers.
#undef FT_CONFIG_OPTION_USE_LZW
#undef FT_CONFIG_OPTION_USE_ZLIB
#undef FT_CONFIG_OPTION_USE_BZIP2
#undef FT_CONFIG_OPTION_USE_PNG
#undef FT_CONFIG_OPTION_USE_HARFBUZZ
#undef FT_CONFIG_OPTION_USE_HARFBUZZ_DYNAMIC
#undef FT_CONFIG_OPTION_USE_BROTLI
#undef FT_CONFIG_OPTION_SVG

#pragma once

#include "hb.h"

// The FreeType runtime no longer owns a HarfBuzz font.  This no-op remains only
// until the obsolete opaque member is removed from the private runtime model.
inline void hb_ft_font_changed(hb_font_t*)
{
}

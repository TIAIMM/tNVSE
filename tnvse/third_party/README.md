# Third-party software

## FreeType 2.14.3

tNVSE builds a reduced FreeType 2.14.3 configuration as a Win32 static
library for the in-game IME composition and candidate overlay.

- Project: https://freetype.org/
- Submodule: https://github.com/freetype/freetype.git
- Tag: `VER-2-14-3`
- Commit: `0a0221a1347e2f1e07c395263540026e9a0aa7c7`
- License selected for tNVSE: FreeType License (FTL)
- License text: `freetype-2.14.3/docs/FTL.TXT`

The local build configuration disables optional compression, PNG, SVG, and
HarfBuzz dependencies. The upstream source is kept as a pinned Git submodule;
the custom static-library project and module configuration remain in this
repository.

This software is based in part on the work of the FreeType Team. Portions of
this software are copyright (C) 2026 The FreeType Project
(https://freetype.org). All rights reserved.

## libtess2

tNVSE uses libtess2 to triangulate FreeType glyph outlines for configured
in-game vector fonts.

- Project: https://github.com/memononen/libtess2
- Commit: `8dbd6483e920311a58c9af10a10beb278efebc36`
- License: SGI Free Software License B, version 2.0
- License text: `libtess2/LICENSE.txt`

## Initialize submodules

After cloning or changing revisions, initialize both source trees with:

```text
git submodule update --init --recursive
```

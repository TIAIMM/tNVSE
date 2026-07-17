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
FreeType-internal HarfBuzz dependencies. The upstream source is kept as a pinned Git submodule;
the custom static-library project and module configuration remain in this
repository.

This software is based in part on the work of the FreeType Team. Portions of
this software are copyright (C) 2026 The FreeType Project
(https://freetype.org). All rights reserved.

## HarfBuzz 14.2.1

tNVSE statically links HarfBuzz for optional OpenType GSUB/GPOS shaping of
configured FreeType game fonts. The local project enables FreeType integration
and the built-in Unicode data without adding a runtime DLL.

- Project: https://github.com/harfbuzz/harfbuzz
- Tag: `14.2.1`
- Commit: `56feae4035bdd48f62ba2b8d8c16232d4d89b3a4`
- License: MIT-style HarfBuzz license
- License text: `harfbuzz/COPYING`

## stb_rect_pack 1.01

tNVSE vendors the single-header `stb_rect_pack` skyline implementation for
deterministic atlas snapshot repacking. Runtime atlas pages retain their live
placements; complete pure-SDF profiles are repacked only while snapshot files
are being produced.

- Project: https://github.com/nothings/stb
- Upstream commit: `31c1ad37456438565541f4919958214b6e762fb4`
- Header: `stb/stb_rect_pack.h`
- License selected for tNVSE: MIT
- The MIT license text is included at the end of the vendored header.

## libunibreak 7.0

tNVSE statically links libunibreak for the optional per-font UAX #14 line-break
mode. The local Visual Studio project builds the upstream C sources as a Win32
static library and does not add a runtime DLL.

- Project: https://github.com/adah1972/libunibreak
- Tag: `libunibreak_7_0`
- Commit: `3ce4bfa3129ff3738046a44a6db533d2ce25af2b`
- License: zlib/libpng
- License text: `libunibreak/LICENCE`

## Initialize submodules

After cloning or changing revisions, initialize all source trees with:

```text
git submodule update --init --recursive
```

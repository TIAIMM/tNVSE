# Third-party software

## FreeType 2.14.3

tNVSE builds a reduced FreeType 2.14.3 configuration as a Win32 static
library for the game-font renderer and in-game IME composition/candidate overlay.

- Project: https://freetype.org/
- Submodule: https://github.com/freetype/freetype.git
- Tag: `VER-2-14-3`
- Commit: `0a0221a1347e2f1e07c395263540026e9a0aa7c7`
- License selected for tNVSE: FreeType License (FTL)
- License text: `freetype-2.14.3/docs/FTL.TXT`

The local build configuration disables optional compression, PNG, SVG, and
external shaping dependencies. The upstream source is kept as a pinned Git
submodule; the custom static-library project and module configuration remain in
this repository.

This software is based in part on the work of the FreeType Team. Portions of
this software are copyright (C) 2026 The FreeType Project
(https://freetype.org). All rights reserved.

## msdfgen 1.13

tNVSE uses the core MTSDF generator and the FreeType outline bridge from
msdfgen as Win32 static libraries. The standalone program, OpenMP, Skia, SVG,
PNG, and vcpkg integration are disabled. Runtime font outlines are colored and
converted directly to four-channel MTSDF bitmaps; no msdfgen file importer or
image writer participates in the game path.

- Project: https://github.com/Chlumsky/msdfgen
- Submodule: https://github.com/Chlumsky/msdfgen.git
- Tag: `v1.13`
- Commit: `1874bcf7d9624ccc85b4bc9a85d78116f690f35b`
- License: MIT
- License text: `msdfgen/LICENSE.txt`

## stb_rect_pack 1.01

tNVSE vendors the single-header `stb_rect_pack` skyline implementation for
deterministic atlas snapshot repacking. Runtime atlas pages retain their live
placements; complete pure-MTSDF profiles are repacked only while snapshot files
are being produced.

- Project: https://github.com/nothings/stb
- Upstream commit: `31c1ad37456438565541f4919958214b6e762fb4`
- Header: `stb/stb_rect_pack.h`
- License selected for tNVSE: MIT
- The MIT license text is included at the end of the vendored header.

## Initialize submodules

After cloning or changing revisions, initialize all source trees with:

```text
git submodule update --init --recursive
```

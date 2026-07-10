# Third-party software

## FreeType 2.14.3

tNVSE builds a reduced FreeType 2.14.3 configuration as a Win32 static
library for the in-game IME composition and candidate overlay.

- Project: https://freetype.org/
- Source: https://download.savannah.gnu.org/releases/freetype/freetype-2.14.3.tar.xz
- SHA-256: `36bc4f1cc413335368ee656c42afca65c5a3987e8768cc28cf11ba775e785a5f`
- License selected for tNVSE: FreeType License (FTL)
- License text: `freetype-2.14.3/docs/FTL.TXT`

The local build configuration disables optional compression, PNG, SVG, and
HarfBuzz dependencies. The upstream source is otherwise kept in its official
archive layout.

This software is based in part on the work of the FreeType Team. Portions of
this software are copyright (C) 2026 The FreeType Project
(https://freetype.org). All rights reserved.

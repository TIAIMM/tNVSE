# Source layout

The plugin sources are grouped by responsibility. Plugin entry,
configuration, dependency checks, globals, and native-call helpers stay at the
`Src` root because they are shared across every subsystem.

- `dictionary`: translation dictionary loading, preparation, matching, and effects.
- `font`: font orchestration, text encoding/decoding, layout, hooks, and
  performance support.
  - `a8`: native A8 render hooks.
  - `atlas`: atlas packing, caching, resources, shaping, and snapshots.
    Snapshot orchestration stays in `font_atlas_snapshot.cpp`; format and file
    validation, packing, loading, and saving are isolated in the corresponding
    `font_atlas_snapshot_*` modules. `font_atlas_resource.cpp` owns managed-pool
    resources, while `font_atlas_pixels.cpp` owns pixel/mipmap streaming and
    `font_atlas_default_pool.cpp` owns DEFAULT-pool device lifecycle. Direct
    atlas runtime queries stay in `font_atlas_direct.cpp`; cache restore and
    sealed-profile construction live in `font_atlas_direct_cache.cpp` and
    `font_atlas_direct_build.cpp`.
  - `native`: native shader packets, rings, accumulation, and fallback.
  - `vector`: FreeType configuration, direct encoded-unit layout, rasterization, and caches.
    Persistent-cache route and flush orchestration stays in
    `font_vector_persistent_cache.cpp`; bitmap records and glyph manifests are
    implemented by `font_vector_persistent_bitmap_cache.cpp` and
    `font_vector_glyph_manifest.cpp`.
- `game`: game-facing hooks and save-name handling.
- `input`: multibyte input, IME/TSF, menu integration, and candidate overlays.
- `nvse`: the NVSE plugin API declaration.

The build keeps these directories as private include roots so existing local
header names remain stable while the physical layout stays navigable in CMake
Targets View.

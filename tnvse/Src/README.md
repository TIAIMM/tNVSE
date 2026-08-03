# Source layout

The plugin sources are grouped by responsibility. Plugin entry,
configuration, dependency checks, globals, and native-call helpers stay at the
`Src` root because they are shared across every subsystem.

- `dictionary`: translation dictionary loading, preparation, matching, and effects.
- `font`: font orchestration, text encoding/decoding, layout, hooks, and
  performance support.
  - `a8`: native A8 render hooks.
    `font_a8_render.cpp` publishes the generation-scoped readiness snapshot and
    owns singleton payload publication plus metadata-map capacity maintenance;
    hot consumers only use the snapshot after exact hook/device/atlas checks.
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
    `font_native_tile_constants.cpp` owns the reverse-verified Tile slot-31
    transient-tail specialization and the translation-only VS c0-c3 update
    used after exact retained-constant proofs. `font_native_instancing.cpp`
    owns final-order lightweight transient admission, immutable glyph-sidecar
    consumption, word-normalized admission compatibility keys with one retained
    property suffix per accepted batch, physically separate immutable admission
    plans and preallocated batch-local transient/snapshot scratch, D3D9 resources,
    the draw-free bind/restore proof bracket, and slot-27 fail-open
    leader/follower execution.
    Its diagnostics also retain first-field compatibility mismatch counters and
    per-stage timing histograms. `font_native_accumulator.cpp` builds the compact
    final-order execution skeleton while first scanning the sorted accumulator;
    `font_native_command_buffer.cpp` and instancing reuse the same readiness and
    resource epochs without changing stock barriers or fallback order.
    `font_native_diagnostics.cpp` owns the bounded, logging-only D3D9 binding,
    Tile-constant, ring-upload/packet-range, and final indexed-submit probes
    used to compare direct and slot-27 draw paths without reading WRITEONLY GPU
    buffers.
  - `vector`: FreeType configuration, direct encoded-unit layout, rasterization, and caches.
    Persistent-cache route and flush orchestration stays in
    `font_vector_persistent_cache.cpp`; bitmap records and glyph manifests are
    implemented by `font_vector_persistent_bitmap_cache.cpp` and
    `font_vector_glyph_manifest.cpp`.
- `game`: game-facing hooks and save-name handling. The list viewport hook keeps
  a non-owning, identity-validated topology descriptor; all alpha, clipwindow,
  bounds, transform, and visibility traits remain live reads and uncertainty is
  fail-open.
- `input`: multibyte input, IME/TSF, menu integration, and candidate overlays.
- `nvse`: the NVSE plugin API declaration.

The build keeps these directories as private include roots so existing local
header names remain stable while the physical layout stays navigable in CMake
Targets View.

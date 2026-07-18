# Source layout

The plugin sources are grouped by responsibility. Plugin entry,
configuration, dependency checks, globals, and native-call helpers stay at the
`Src` root because they are shared across every subsystem.

- `dictionary`: translation dictionary loading, preparation, matching, and effects.
- `font`: font orchestration, text encoding/decoding, layout, hooks, and
  performance support.
  - `a8`: native A8 render hooks.
  - `atlas`: atlas packing, caching, resources, shaping, and snapshots.
  - `native`: native shader packets, rings, accumulation, and fallback.
  - `vector`: FreeType/HarfBuzz configuration, shaping, rasterization, and caches.
- `game`: game-facing hooks and save-name handling.
- `input`: multibyte input, IME/TSF, menu integration, and candidate overlays.
- `nvse`: the NVSE plugin API declaration.

The build keeps these directories as private include roots so existing local
header names remain stable while the physical layout stays navigable in CMake
Targets View.

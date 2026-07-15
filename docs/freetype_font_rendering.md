# FreeType game font rendering

This document describes runtime behavior and implementation details. The
comment block in `tnvse_fonts.xml` is the concise configuration-item reference.

## Enable the renderer

The feature is disabled by default. Enable both options in `tnvse.ini`:

```ini
[Main]
bEnableMultibyteFontHook=1
bEnableFreeTypeFontRendering=1
bEnableFreeTypeFontRenderingLog=0
bEnableFreeTypeDevicePixelScale=1
fFreeTypeFontResolutionScale=1.0
```

## Raster scale and UIO

`bEnableFreeTypeDevicePixelScale=1` rasterizes glyphs at the physical screen
pixel density reported by the UI `resolutionconverter` trait. That device
density is the single source resolution used by ordinary and UIO 2.30
`CreateText` calls. UIO zoom remains a scene-node transform and selects from
the shared atlas mip chain instead of producing another bitmap/atlas profile.
Layout, wrapping, alignment, and returned dimensions remain in game UI units.
Both policies always generate the UIO `1.0` source profile. UIO zoom remains a
scene-node transform and never creates another bitmap or atlas profile. Set
`bEnableFreeTypeDevicePixelScale=0` to replace the device-pixel source scale
with `fFreeTypeFontResolutionScale`. Its default is `1.0`, its valid range is
`0.1-10.0`, and it is ignored while device-pixel scaling is enabled. Manual
`1.0` matches the original grayscale renderer's ordinary UIO `1.0` source
resolution; values such as `1.5` or `2.0` increase source resolution without
changing layout or displayed font size, at the cost of larger CPU masks and
atlas usage. Grayscale and SDF bodies use the same resolved source scale; their
cache keys include the resulting effective pixel dimensions, so changing the
multiplier selects a new compatible cache profile.

Set `bEnableFreeTypeFontRenderingLog=1` while diagnosing configuration or font
loading. The log records the XML path, resolved face paths, FreeType errors,
font-ID activation, and the first atlas-rendered glyph for each byte class.

Font IDs are configured under `<fonts>` in
`Data\NVSE\plugins\tnvse_fonts.xml`. Only listed IDs are replaced. Other
fonts continue to use the original `.fnt` and `.tex` files.

## Configuration model and vertical metrics

```xml
<tNVSE>
  <fonts>
    <font id="1"
          prewarm="none"
          shaping="1"
          features="kern,liga,clig,calt"
          pixelSize="24"
          fontColor="#FFFFFF"
          fontAlpha="1"
          tracking="0"
          scaleX="1"
          scaleY="1"
          embolden="0"
          slant="0"
          baselineOffset="0"
          baseline="0"
          curveTolerance="0.35">

      <!-- Optional defaults inherited by both byte classes. -->
      <face path="Data/Fonts/Default.ttf" index="0"/>
      <fallback path="Data/Fonts/Symbols.ttf" index="0"/>

      <singleByte pixelSize="20" baselineOffset="0">
        <face path="Data/Fonts/RobotoCondensed-Regular.ttf" index="0"/>
      </singleByte>

      <doubleByte pixelSize="28" baselineOffset="0">
        <face path="Data/Fonts/NotoSansSC-Regular.otf" index="0"/>
        <fallback path="Data/Fonts/NotoSansSymbols2-Regular.ttf" index="0"/>
      </doubleByte>

      <glow enabled="1" inner="0" outer="4" power="2"
            color="#66FF99" alpha="0.35"/>
      <outline enabled="1" width="1" softness="0.5"
               color="#000000" alpha="1"/>
      <shadow enabled="1" x="1" y="1" blur="2" power="2"
              color="#000000" alpha="0.65"/>
    </font>
  </fonts>
</tNVSE>
```

`singleByte` and `doubleByte` inherit scalar attributes from `<font>` and may
override them independently. If a byte-class node contains a `<face>` or
`<fallback>`, its complete face chain replaces the parent chain. A resolved
byte-class configuration must have one primary face and a positive
`pixelSize`. The independently overridable attributes are `pixelSize`,
`tracking`, `fixedWidth`, `scaleX`, `scaleY`, `embolden`, `slant`, and
`baselineOffset`.

Paths may be absolute or relative to the Fallout New Vegas directory. `index`
selects a face in TTC/OTC files. `slant` is measured in degrees; other style
dimensions are pixels. `baseline="0"` derives the shared Fallout `fBaseLine`
from both primary faces in `verticalMetrics="freetype"`, or inherits the
original `.fnt` baseline in `verticalMetrics="original"`. A positive value
sets that baseline manually and is rounded upward to the game's integer metric.
Original-metrics mode retains automatic visual-center correction between byte
classes even with a manual baseline; FreeType-metrics mode applies that
correction only to an automatically derived baseline. Glow, outline, and
shadow are disabled when their node is absent; when a node is present,
`enabled` defaults to `1`.

`curveTolerance` is consumed only by the libtess2 vector fallback. Atlas-backed
grayscale and SDF paths do not use it.

`baseline` controls Fallout's line rise and therefore the distance between
lines. It is not a general glyph Y offset. Use the independently inheritable
`baselineOffset` on `singleByte` or `doubleByte` to move that byte class within
the line; positive values move glyphs upward. In manual baseline mode the
renderer keeps the FreeType body top/drop metrics. The visual-center rule then
depends on `verticalMetrics` as described above.

`fontColor="#RRGGBB"` and `fontAlpha` optionally override the fill color for
the font ID. When `fontColor` is omitted, fill geometry keeps the color supplied
by the game. `glow`, `outline`, and `shadow` belong to the font ID and are
shared by both byte classes. `effectQuality="fast|balanced|high"` selects the
PS 3.0 SDF sampling preset and defaults to `balanced`; the presets use 1, 4,
or 8 subpixel samples. `fillRenderMode="grayscale"` uses the hinted FreeType
coverage bitmap, while `fillRenderMode="sdf"` selects the hinted outline-to-SDF
body. Glow uses `inner`, `outer`, and `power`; legacy `width` is accepted as
an alias for `outer` only when `outer` is absent. Outline accepts a
non-negative `softness`. Shadow accepts a non-negative `blur` and a positive
`power`; `blur=0` preserves the exact hinted offset mask. Every configured
alpha is multiplied by the game text alpha, so visibility and fade animations
continue to work.

## Shaping and fixed-width layout

`shaping="0"` is the default. It uses FreeType 26.6 advances and
`FT_Get_Kerning()` without rounding every glyph in advance. `shaping="1"`
enables HarfBuzz GSUB/GPOS for ordinary `Font` text. The optional
comma-separated `features` attribute uses HarfBuzz feature syntax, for example
`kern,liga,ss01=1,-dlig`; unspecified features keep HarfBuzz's script defaults.
`features` is invalid unless shaping is enabled. Rich text keeps one game
`CharData` per encoded character and therefore uses precise FreeType kerning
without GSUB substitutions.

A positive `fixedWidth` centers each glyph body in a logical cell, disables
kerning and HarfBuzz shaping for that byte class, and makes its final advance
`fixedWidth + tracking`. This matches the grid-oriented DCFGCF behavior used by
interfaces such as the terminal hacking screen. A value of zero retains
proportional advances.

## Blocking prewarm and persistent caches

`prewarm="none"` is the default and preserves fully demand-driven atlas
generation. `prewarm="common"` prepares valid single-byte units and a common
double-byte repertoire. Under CP936 that repertoire is the complete GB2312
set; other supported code pages retain their limit of up to 7000 valid
double-byte units. `prewarm="codepage"` follows DCFGCF's explicit encoding
ranges for the current `uiEncoding` code page, with CP936 using the complete
GBK profile. Prewarming begins after the
configured fonts are activated. On the first game-loop callback where the
final device scale is available, tNVSE synchronously drains the complete queue;
the game remains blocked until every queued profile reports `complete`,
`atlas-full`, or `cancelled`. Both device-pixel and manual resolution modes
prewarm one canonical UIO `1.0` source size. UIO-derived calls reuse that mask
and atlas profile instead of generating per-zoom variants.
While this startup barrier is active, a non-activating English progress window
runs on a separate UI thread. It shows the current font ID, grayscale/SDF mode,
the active scan or snapshot stage, and overall progress. The window remains
responsive while FreeType work blocks the game thread and closes automatically
before control returns to the game. It is owned by the Fallout window rather
than being system-topmost, so Windows manages its minimize and Z-order behavior
together with the game without a polling timer.
A font task allocates additional atlas pages when its complete set cannot fit
one 4096x4096 page. It reports `atlas-full` only if one incoming batch cannot
fit an empty maximum-size page, the page-count safety limit is reached, or a
texture allocation/upload fails. Full code-page prewarming generates the selected fill mask for
every valid unit. Consequently an SDF fill is prewarmed for the complete code
page and its hard shadow reuses the same mask. SDF masks needed only by effects
remain limited to single-byte and common double-byte characters.

Generated grayscale and SDF masks are persisted under
`Data\NVSE\plugins\tnvse\fontdata`. Each mask profile begins with a dense
glyph-index table, so a glyph lookup is one fixed-offset read rather than a
record scan or hash-table rebuild. On later launches tNVSE validates and loads
those CPU mask records instead of rasterizing the same glyphs with FreeType. A
persistent profile is keyed by the font file's content hash, actual collection
face, effective pixel dimensions, embolden, slant, stroke width, SDF spread,
and mask type. It can therefore be shared safely across equivalent font IDs,
while any font or rendering change selects a different profile automatically.
Files are versioned and record-checksummed; an invalid header is rebuilt and a
truncated or corrupt record falls back to normal rasterization. Cache names use
`<game-font-id>_<font-file-name>_<profile-hash>.tnvfmask`. Lookup accepts only
that numeric-font-ID form and uses the hash suffix, so equivalent masks remain
shareable across font IDs. Older hash-only names are intentionally ignored.

The same directory also contains three startup-oriented cache layers. A
`.tnvfhash` record reuses the font content hash when file identity, size and
last-write time still match. A dense 65536-entry `.tnvfmanifest` stores the
encoded unit's Unicode value, fallback face/glyph identity, and serialized
`FontLetter` metrics. One `_p<page>.tnvfatlas` snapshot per atlas page stores
the shelf cursor state and stable glyph-ID placement map. Pure SDF pages store
only the placed level-zero rectangles; other pages retain their complete mip
chain. Each page records and validates the total page count. After a successful
full prewarm every page is written atomically, then the manifest is marked
complete. A later launch restores the complete page set directly and skips
code-page enumeration, per-glyph mask loading, packing, and mip generation.
Every layer includes its schema/layout/mask/font/code-page inputs in its hash;
stale files are ignored rather than migrated.

`bDeleteUnusedFreeTypeFontCache=1` removes stale `.tnvfmask`, `.tnvfhash`,
`.tnvfmanifest`, and `.tnvfatlas` files that were not accessed by the current
run after every configured prewarm atlas has been generated or restored
successfully. Cleanup is skipped if any prewarm job fails or is cancelled, and
unknown files in `fontdata` are never removed. The option defaults to `0`.

Cache identity is split by responsibility. The layout hash covers font faces,
metrics, advances, shaping, and fallback identity. Persistent manifests store
effect-independent body metrics and add the current visual effect extents when
loaded. The mask-generation hash covers only outline inputs that change glyph
pixels. The atlas-content hash is resolved at the final raster scale from that
mask identity, the actual mask-type combination, the quantized SDF spread, and
CPU fallback stroke widths. Shader colors, offsets, powers, inner thresholds,
and quality selection use a separate shader-effect hash and do not invalidate
an SDF atlas. Consequently an effect edit selects a new prewarm snapshot only
when it changes the final SDF spread or the masks that the atlas must contain.
The grayscale CPU fallback remains content-sensitive: independently generated
coverage masks include the enabled glow, outline, and shadow parameters after
physical-pixel quantization (including stroke, blur, softness, and power) in the
atlas identity. Effect offsets remain draw geometry and do not invalidate mask
pixels. Disabled-effect values are ignored. The ARGB fallback additionally
hashes every baked layer/color variant because its RGB and alpha modifiers are
stored in atlas pixels rather than supplied only as shader constants.

## Encoding and fallback routing

Routing follows the selected `uiEncoding`: valid one-byte units use
`singleByte`, while valid DBCS pairs use `doubleByte`. Shift-JIS half-width
katakana therefore use the single-byte font. Missing glyph lookup stays inside
the selected byte-class fallback chain and then tries `U+FFFD`, `?`, and the
primary face's `.notdef` glyph.

## Atlas and Tile shader routing

The normal rendering path rasterizes hinted grayscale glyphs with FreeType at
the effective display size. When Fallout Shader Loader 1.40 or newer,
`tnvse_freetype_a8.pso`, and a real `D3DFMT_A8` texture are available, each
font/style/effective-size profile uses a one-byte A8 atlas. A shape-specific
pixel shader reads the atlas alpha as coverage and follows the game's
Tile contract for the fill: RGB is `c0.rgb` multiplied by the per-glyph fill
modifier. Effect ranges use their configured XML RGB directly and inherit the
live Tile alpha only. All ranges output alpha as coverage multiplied by `c0.a`
and their per-layer alpha. The bridge therefore sets effect `c0.rgb` to white,
restores the complete original `c0` before every fill range, and restores it
again on every exit path. The shaders do not consume a `COLOR0` vertex stream,
and RGB remains straight rather than premultiplied by alpha. tNVSE does not
replace the global TileShader. If any dependency or runtime validation fails,
that profile automatically uses the existing
`A8R8G8B8` atlas path, so FreeType rendering itself does not require Shader
Loader. In that fallback, XML layer RGB and alpha are baked into the 32-bit
atlas. The range bridge neutralizes Tile RGB only for effect ranges; the fill
continues to use the original Tile color and all ranges retain dynamic Tile
alpha.

Range routing is installed independently of Shader Loader and is attempted
synchronously while a shape is created and again at the first Tile render.
This is required for `start_menu.xml`, `HUDMainMenu`, and other persistent UI
created before NVSE `DeferredInit`. Such shapes retain all effect/fill ranges
even when the D3D device is not ready yet; the Tile accumulator call-site route
then establishes the exact shape context before the engine chooses its normal
or fast draw branch. Shader discovery is staged and retryable, so a temporary
startup ordering difference cannot permanently cache a fill-only shape.

## Vanilla UI Plus compatibility

Vanilla UI Plus implements its optional text treatment in
`Menus/Prefabs/VUI+/outline.xml` by cloning the source text into the named
`VUI+Shadow` and `VUI+Outline` `TileText` nodes. tNVSE recognizes only those
two exact proxy names while their `TileText::MakeNode` call is active. Their
FreeType body and live VUI+ Tile color are retained, but configured tNVSE
shadow, glow, and outline layers are omitted on all shader, CPU-atlas, and
vector-fallback routes. The original sibling and every unrelated dark or
startup text remain unaffected, and no VUI+ XML file is modified.

## SDF effects and draw-state isolation

The base A8 shader and all effect variants use `ps_3_0`. A `grayscale` body
uses the hinted grayscale mask. An `sdf` body, glow, outline, and blurred
shadow share a FreeType distance field generated directly from the hinted
outline; hard shadow reuses the selected body mask. FreeType overlap handling
is enabled only when the loaded outline carries `FT_OUTLINE_OVERLAP`. Both
masks can occupy the same A8 atlas. Effects
execute global shadow, glow, outline, and fill passes over one `NiTriShape`,
which prevents a later glyph effect from covering an earlier glyph fill. SDF
passes use bilinear MIN/MAG sampling at atlas LOD 0 and derivative-based edge
antialiasing; they never consume the coverage-averaged atlas mip chain.
SDF draw ranges also preserve fractional pen positions, shaped advances, and
effect offsets in their quad coordinates. Grayscale coverage ranges remain
snapped to the resolved source-pixel grid so their coverage texels stay aligned.
Grayscale masks can still use trilinear mip sampling when scene scaling needs
it. Glow keeps
full intensity through `inner`, then decays to zero at `outer` according to
`power`; outline uses `width` plus `softness`; blurred shadow uses `blur` and
`power`. The physical SDF spread is derived from the largest enabled radius
and must remain in FreeType's supported 2-32 pixel range. An unsupported
spread causes the complete text batch to use the CPU effect path rather than
silently reducing the requested effect.
Because an SDF body requires the custom A8 shader, failure to establish that
route resolves the body through the hinted grayscale CPU/atlas path instead of
sampling SDF with the stock Tile shader.
When an effect shader is unavailable, the renderer retains the CPU mask path
with the same global layer order. When `NVSE_PLUGIN_PATH` is defined, an
ordinary project build copies all compiled PSOs to `Data\Shaders\Loose`.
Custom draws snapshot the authoritative D3D device state, switch only the
pixel shader, private constants, sampling, and pass-local write state, then
restore everything after the draw. Effect passes preserve the caller's RGB
blend. If the target permits alpha writes, they use a separate source-over
alpha blend so shadows outside the fill survive off-screen UI compositing
without reducing existing destination alpha. They also suppress stencil and
depth writes while preserving the caller's tests. A detected contract or
state mismatch forwards the original Tile draw instead of leaking state or
dropping the text body.

## Atlas allocation, mipmaps, and memory

Persistent atlas pages start at 512x512 and grow without moving existing glyphs.
Missing glyphs are rasterized as one batch and uploaded through one dirty
rectangle. Every atlas allocation has four transparent padding pixels per side,
which isolates the 1/4 mip's bilinear footprint even when glyph dimensions are
not multiples of four. Repeated text also reuses cached layout and
vertex/UV/index templates. A8 and 32-bit profiles use separate cache keys and
may coexist when text was created before Shader Loader initialization.

Generated grayscale masks, layouts, and batch templates are cached in process
memory. Equivalent masks are shared across font IDs when the resolved font
file/face, glyph, effective raster size, emboldening, slant, stroke or SDF
parameters, and mask type match. Baseline placement remains per font ID and is
not baked into the shared mask. `uiFreeTypeFontMemoryCacheMB` controls those
CPU-side caches. When
`bEnableFreeTypeDefaultPoolAtlas=1`, tNVSE creates dynamic `D3DPOOL_DEFAULT`
atlas textures and retains only the masks used by each live atlas generation;
it does not retain a complete CPU copy of the atlas. The current and retired
generations are restored after a D3D9 device reset. A pure SDF v5 snapshot is
uploaded directly to this path and keeps its packed placement payload as reset
backing; glyphs added later retain only their individual masks. If direct
DEFAULT-pool creation is unavailable, snapshot restore and normal atlas
creation fall back to the engine-managed implementation.

`uiFreeTypeFontGpuAtlasCacheMB` controls the soft GPU atlas budget. A value of
zero selects one eighth of the available texture memory, rounded to 16 MB and
clamped to 64-256 MB; 128 MB is used when the device does not report a reliable
value. A nonzero value is used directly. Atlas generations still referenced by
visible game shapes cannot be evicted, so live usage may temporarily exceed the
soft budget. The resolved value is written to `tnvse.log` at initialization and
when a device reset changes the automatic result. Validated pure SDF snapshots
restore directly to the DEFAULT pool when enabled; other snapshots use the
engine-managed path. Non-SDF font atlases contain three mip levels (1x,
1/2x, and 1/4x); the cache budget and upload counters include all levels.
Limiting the chain to three levels together with four-pixel per-side packing
padding prevents the coarsest bilinear footprint from reaching a neighboring
glyph. Text spanning pages is sorted into contiguous layer/page draw ranges;
the range bridge selects the corresponding texture before each submission and
restores the caller's original texture afterward. Custom draws force mip level zero as the base and a zero LOD bias, then
restore the caller's complete sampler state.

## Scope and fallbacks

FreeType rasterization remains CPU based. Adding Skia, D3D11, or D3D12 would
require a readback or cross-API copy before Fallout New Vegas can consume the
result through D3D9, increasing synchronization cost and reducing DXVK/Wine
compatibility. The GPU is therefore used for persistent atlas sampling and
quad rendering, while hinted masks are generated once on the CPU.

When device-pixel mode is enabled and UIO 2.30 scales a TileText call, tNVSE
keeps the mask at the canonical device raster size and lets the existing world
transform minify or magnify the mipmapped atlas. Trilinear sampling is enabled
for scaled A8 grayscale shapes. Manual resolution mode likewise keeps a
canonical UIO `1.0` profile at its configured source multiplier; UIO zoom is
applied by the existing transform. If atlas creation fails, the
renderer falls back to the libtess2 outline path. HarfBuzz
shaping is limited to horizontal LTR text in the configured DBCS code pages;
bidirectional layout, LCD subpixel rendering, color-font rendering, and
variable-font axis controls are outside this feature. Invalid or unavailable
configurations leave that entire font ID on the original `.fnt`/`.tex` renderer.

# FreeType game font rendering

This document describes runtime behavior and implementation details. The
comment block in `tnvse_fonts.xml` is the concise configuration-item reference.

## Enable the renderer

FreeType has its own strict INI section and does not depend on the multibyte
font hook:

```ini
[FreeTypeFont]
bEnableFreeTypeFontRendering=1
bEnableFreeTypeFontRenderingLog=0
fFreeTypeFontResolutionScale=1.0
```

With `[Multibyte] bEnableMultibyteFontHook=1`, FreeType uses the configured
DBCS parser and byte-pair layout; this mode retains its existing conversion,
wrapping, rich-text merge, and pagination logic. With that switch disabled,
configured FreeType font IDs use a dedicated tNVSE Windows-1252 layout path
derived from the reverse-engineered vanilla rules. Spaces are removable word
breaks, `~` is a discretionary hyphen, unbroken words use vanilla-style hard
hyphenation, and line limits/control bytes follow the original single-byte
semantics, but every decision uses final FreeType advances. Rich text feeds
those advances into `TextLine::AddChar` before line/page topology is selected;
the final traversal only normalizes positions and aggregate widths. Non-FreeType
font IDs remain wholly on the original `.fnt`/`.tex` path. A configured font
can additionally set `unicodeLineBreaking="1"` to add libunibreak UAX #14
opportunities to either FreeType layout mode.

## Raster scale and UIO

`fFreeTypeFontResolutionScale` is the sole source multiplier used by startup
prewarm, demand-generated glyphs, grayscale and SDF masks, atlas pages, and
persistent caches. Its default is `1.0`, its valid range is `0.1-10.0`, and it
is canonicalized to the nearest `0.001`. Output resolution and the UI
`resolutionconverter` trait do not change the source profile. A display density
below the configured multiplier minifies that profile; a higher density
magnifies it without generating another mask or atlas.

`1.0` matches the original grayscale renderer's ordinary UIO `1.0` source
resolution. Values such as `1.5` for a 1440p-oriented source or `2.25` for a
4K-oriented source increase source resolution without changing layout or
displayed font size, at the cost of larger CPU masks, persistent files and atlas
usage. Layout, wrapping, alignment, and returned dimensions remain in game UI
units. UIO zoom remains a scene-node transform and selects from the shared
atlas profile rather than producing a bitmap or atlas variant. Changing the
configured multiplier intentionally selects one new compatible cache profile.

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
          unicodeLineBreaking="1"
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
		  baseline="0">

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
            colorMode="fill" alpha="0.35"/>
      <outline enabled="1" width="1" softness="0.5"
               colorMode="fixed" color="#000000" alpha="1"/>
      <shadow enabled="1" x="1" y="1" blur="2" power="2"
              colorMode="fixed" color="#000000" alpha="0.65"/>
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
`power`; `blur=0` preserves the exact hinted offset mask.

Every effect accepts `colorMode="fixed|fill"`. The backward-compatible default
is `fixed`: the native profile neutralizes the live Tile RGB (`c0.rgb = 1`) and
the vertex carries the effect's configured `color`. `fill` keeps the live Tile
RGB and gives the effect vertex the same RGB modifier as the body, including a
configured `fontColor`; the effect's `color` RGB is then ignored. In either
mode, the effect's own `alpha` remains independent of `fontAlpha` and is
multiplied by the game text alpha, so visibility and fade animations continue
to work.

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

## Unicode line breaking

`unicodeLineBreaking="0"` is the default and preserves the existing prepared
text rules. Setting it to `1` on one `<font>` enables libunibreak for that font
ID only. tNVSE decodes the prepared byte string with the active FreeType text
code page (Windows-1252, GBK, Big5, Shift-JIS, or UHC), supplies `zh`, `ja`,
`ko`, or `en` language context as appropriate, and maps the UAX #14 results
back to encoded-byte boundaries. A break is never inserted inside a DBCS pair
or HarfBuzz cluster.

The existing explicit line separator, `~` discretionary hyphen, removable
single-byte space, and hard-wrap fallback remain available. The switch is part
of the layout identity, so changing it cannot reuse prepared-text or layout
cache entries made under the other setting. It has no effect on font IDs that
remain on the original `.fnt`/`.tex` renderer.

## Blocking prewarm and persistent caches

`prewarm="none"` is the default and preserves fully demand-driven atlas
generation. In FreeType-only mode, both `prewarm="common"` and
`prewarm="codepage"` enumerate exactly the 224 Windows-1252 byte units
`0x20-0xFF`; no double-byte scan is performed. With the multibyte hook active,
`common` prepares the valid single-byte units and a common double-byte
repertoire. Under CP936 that repertoire is the complete GB2312 set; other
supported DBCS code pages retain their limit of up to 7000 valid double-byte
units. `codepage` follows DCFGCF's explicit encoding ranges, with CP936 using
the complete GBK profile. Prewarming begins after the configured fonts are activated. On
the first game-loop callback, tNVSE synchronously drains the complete queue at
`fFreeTypeFontResolutionScale`; it does not wait for a menu root or device
scale. The game remains blocked until every queued profile reports `complete`,
`atlas-full`, or `cancelled`. Prewarm and demand rendering share one canonical
source scale. UIO-derived calls reuse that mask and atlas profile
instead of generating per-zoom variants.
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
the stable glyph-ID placement map. Snapshot v8 uses `stb_rect_pack` skyline
packing for a complete pure-SDF profile in deterministic
height/width/glyph-ID order, can reduce the page count, and shrinks every page
to the smallest usable power-of-two dimensions. The live atlas is not
rearranged, so shapes created in the current process keep their original UVs;
the compact layout takes effect on the next restore. A restored skyline page
starts runtime shelf appends below its packed extent rather than reusing skyline
holes. Pure SDF pages store only the placed level-zero rectangles;
other pages retain their complete mip chain. Each page records and validates
the total page count. After a successful full prewarm every page is written
atomically, then the manifest is marked complete. A later launch restores the
complete page set directly and skips code-page enumeration, per-glyph mask
loading, packing, and mip generation.
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

When the multibyte hook is active, routing follows the selected DBCS
`uiEncoding`: valid one-byte units use `singleByte`, while valid pairs use
`doubleByte`. Shift-JIS half-width katakana therefore use the single-byte font.
In FreeType-only mode the effective FreeType code page is always 1252, even if
`uiEncoding=1-4` remains configured, and every byte uses `singleByte`.
Runtime glyph, layout, kerning, mask, manifest and atlas-snapshot identities all
use that effective code page. Code-page-0 font caches are legacy and are never
restored. Missing glyph lookup stays inside the selected byte-class fallback
chain and then tries `U+FFFD`, `?`, and the primary face's `.notdef` glyph.

## Atlas and Tile shader routing

The normal rendering path rasterizes hinted grayscale glyphs with FreeType at
the effective display size. When Fallout Shader Loader 1.40 or newer, the
native FreeType shader set, and a real `D3DFMT_A8` texture are available, each
font/style/effective-size profile uses a one-byte A8 atlas. The visible text is
represented by one facade in the stock Tile alpha list so the game retains its
normal UI sorting. At the sorted Tile pass tNVSE expands that facade into native
Gamebryo geometry packets grouped by layer, atlas page, shader class, and
sampling contract. Those packets use `TileShader`, the renderer-owned shader
declaration, and the renderer's `NiGeometryBufferData`. There is no private
D3D draw path or whole-shape range fallback. If a marked facade cannot complete
through the native route, that submission is suppressed and logged with a
concrete `submission-suppressed` reason. Atlas-shape construction failure also
returns an empty shape instead of rebuilding the text as legacy outline
geometry, so visible FreeType text must have reached the native packet route.

The native pixel shaders read atlas alpha as coverage and follow the game's
Tile contract for the fill: RGB is `c0.rgb` multiplied by the per-glyph fill
modifier. Effect packets use their configured XML RGB directly and inherit the
live Tile alpha only. All packets output alpha as coverage multiplied by `c0.a`
and their per-layer alpha. RGB remains straight rather than premultiplied by
alpha, and tNVSE does not replace the global TileShader.

Native packet buffers are requested at the sorted Tile submission, not while a
text shape is being produced. When that submission runs on
`TESMain::uiMainThreadID`, tNVSE preserves the installed virtual
`PrecacheGeometry` and public `PerformPrecache` owners. This is the normal stock
route and also lets NVTF retain its main-thread synchronization policy.

`FinishAccumulating_Tiles` can instead execute on the renderer thread. NVTF
deliberately sends a non-main-thread virtual precache request to its private
worker, but that worker is paused while the frame is being rendered; such a
request therefore cannot become drawable during the same Tile pass. For this
case tNVSE sends only its own missing packets through the unvirtualized stock
`PrecacheGeometry` entry and completes the stock `PrePackObject` queue before
drawing. No hook or vtable slot is replaced. Both the stock geometry entry and
the continuation of `PerformPrecache` are checked against the FalloutNV
1.4.0.525 instruction bytes first. The continuation is the same post-detour
entry used by NVTF itself. The stock functions acquire their own renderer
critical sections; tNVSE no longer wraps a second blocking renderer lock around
the public owner.

The stock completion continuation is invoked through an explicit indirect x86
ABI boundary, and the thread-local recursion guard is released by a separate
constant-store routine. This is intentional: link-time code generation must not
infer from the naked tail thunk that the game continuation preserves volatile
registers. Without that boundary, a caller-saved return register can poison the
guard after a successful completion and falsely classify the next packet group
as recursive.

Every packet is revalidated after completion. `packet-completion` records the
chosen route, submitting thread, game main-thread ID, installed virtual entry,
and whether the remaining state is `external-queue` or `renderer-packing`.
Suppression records carry the same thread identity. If either audited stock
body has been modified by another executable patch, tNVSE refuses to jump into
unknown code and suppresses the affected marked submission. Incomplete shader
generations, invalid atlas pages, property or hook conflicts, device-reset
windows, and native runtime faults follow the same fail-closed policy. Startup
discovery remains staged and retryable for `start_menu.xml`, `HUDMainMenu`, and
other persistent UI created before NVSE `DeferredInit`; a temporary startup
ordering difference does not permanently cache a fill-only shape.

## Vanilla UI Plus compatibility

Vanilla UI Plus implements its optional text treatment in
`Menus/Prefabs/VUI+/outline.xml` by cloning the source text into the named
`VUI+Shadow` and `VUI+Outline` `TileText` nodes. tNVSE recognizes only those
two exact proxy names while their `TileText::MakeNode` call is active. Their
FreeType body and live VUI+ Tile color are retained, but configured tNVSE
shadow, glow, and outline layers are omitted on all shader, CPU-atlas, and
native and CPU-atlas routes. The original sibling and every unrelated dark or
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
ordinary project build copies the native shader set to `Data\Shaders\Loose`.
Native packets use immutable `TileShader` profiles and the game's normal render
submission. Effect passes preserve the live Tile color/alpha contract and use
separate source-over alpha where the target supports alpha writes, so shadows
outside the fill survive off-screen UI compositing without reducing existing
destination alpha. A detected profile, packet, or device-state mismatch marks
the native generation faulty and suppresses the affected marked submission.

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
generations are restored after a D3D9 device reset. A pure SDF v8 snapshot is
uploaded directly to this path and keeps its packed placement payload as reset
backing. Its page-content fingerprint covers dimensions, format, placed
coordinates, and exact texels. A hash match is followed by byte-for-byte
comparison before equivalent pages share one D3D9 texture allocation through
independent Gamebryo wrappers. A later glyph insertion first detaches that page,
and a device reset rebuilds each wrapper independently. Glyphs added later
retain only their individual masks. If direct
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
glyph. Text spanning pages is sorted into contiguous layer/page native packets;
each packet owns the corresponding atlas property and texture. SDF packets use
mip level zero and a zero LOD bias through their immutable shader profile.

## Scope and fallbacks

FreeType rasterization remains CPU based. Adding Skia, D3D11, or D3D12 would
require a readback or cross-API copy before Fallout New Vegas can consume the
result through D3D9, increasing synchronization cost and reducing DXVK/Wine
compatibility. The GPU is therefore used for persistent atlas sampling and
quad rendering, while hinted masks are generated once on the CPU.

When output resolution or UIO 2.30 scales a TileText call, tNVSE keeps the mask
at the single configured source multiplier and lets the existing world
transform minify or magnify the atlas. Trilinear sampling is enabled for scaled
A8 grayscale shapes; SDF ranges retain level-zero derivative-based sampling.
Neither case creates a resolution- or zoom-specific profile. If atlas creation
fails, the affected FreeType shape is empty and the detailed build diagnostic
identifies the failed stage. HarfBuzz
shaping is limited to horizontal LTR text in the active DBCS path; the
FreeType-only rich-text topology keeps one Windows-1252 byte per `CharData`,
uses pair layout without GSUB before wrapping/pagination, and then normalizes
the resulting positions. Bidirectional layout,
LCD subpixel rendering, color-font rendering, and
variable-font axis controls are outside this feature. Invalid or unavailable
configurations leave that entire font ID on the original `.fnt`/`.tex` renderer.

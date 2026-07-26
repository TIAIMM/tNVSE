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
bEnableFreeTypeFontAggressivePerformanceMode=0
uiFreeTypeFontDistanceFieldMode=1
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
font IDs remain wholly on the original `.fnt`/`.tex` path.

## Raster scale and UIO

`fFreeTypeFontResolutionScale` is the sole source multiplier used by startup
prewarm, demand-generated glyphs, SDF and ARGB-fallback masks, atlas pages, and
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

In MTSDF mode, compatible `doubleByte` styles are grouped automatically. When
the largest and smallest `pixelSize` in a compatible group differ by no more
than 8 pixels, only the largest style is rasterized and stored in the
double-byte atlas. Smaller logical fonts scale that source atlas at draw time,
including distance/effect units, while retaining their own advance, tracking,
fixed-width, baseline, and line metrics. Face chain and indices, `scaleX`,
`scaleY`, `embolden`, and `slant` must match; a larger size span is split into
independent atlas groups. True-SDF mode disables this sharing and generates an
independent double-byte atlas profile at each configured font's own pixel size.

## Base and JIP extended font IDs

The retail `FontManager` exposes IDs `1-8`. When JIP LN NVSE is loaded and its
`SetFontFile` command is registered, tNVSE also resolves JIP's extended IDs
`10-89`; ID `9` is deliberately absent because offset `0x20` is the retail
`bUseNewFonts` field rather than a font pointer. Extended lookup uses JIP's
separate 80-pointer allocation and never reads beyond the retail manager when
JIP is absent. Stewie Tweaks' menu font is covered by the same path because it
registers ID `42` through `SetFontFile` before constructing StewMenu.

This registry resolution is shared by Tile font-trait handling, VUI+ effect
proxy elimination, configured-font activation, and tNVSE string measurement.
The requested slot and the resolved `Font::iFontNum` remain distinct: JIP may
alias an unassigned extended slot to a base font, and tNVSE treats the resolved
font object as the rendering identity.

The stock rich-text ABI is a separate constraint. `TextPage::pCharsPerFont`
and `TextDoc::Render` contain fixed eight-element arrays, so rich-text
`CharData::iFontIndex` remains limited to the retail base fonts. Treating this
layout field as an 89-font registry index would corrupt the `TextPage` and
renderer stack; ordinary `TileText` and `CalculateStringDimensions` do support
registered extended fonts.

## Configuration model and vertical metrics

```xml
<tNVSE>
  <fonts>
    <font id="1"
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
      <outline enabled="1" width="0.2" softness="0.7"
               colorMode="fixed" color="#333333" alpha="0.7"/>
      <shadow enabled="1" x="1" y="1" blur="0" power="2"
              includeGlow="0" includeOutline="0"
              colorMode="fixed" color="#1A1A1A" alpha="0.55"/>
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

Automatic visual alignment is based on the visible hinted coverage produced at
`fFreeTypeFontResolutionScale`, rather than on the font-wide ascender and
descender alone. The renderer measures robust Latin cap and code-page-specific
Chinese, Japanese, or Korean reference sets, rejects the outer 20 percent of
sample centers, and preserves the resulting fractional offset. Each fallback
face is calibrated independently when it contains enough reference glyphs;
otherwise it inherits its byte class primary-face correction. This changes the
glyph-relative offset while retaining one shared baseline. That baseline is
still inherited or derived by the selected `verticalMetrics` policy, preserving
the Fallout line-rise and glyph-drop contract used by rich text and pagination.

`baseline` controls Fallout's line rise and therefore the distance between
lines. It is not a general glyph Y offset. Use the independently inheritable
`baselineOffset` on `singleByte` or `doubleByte` to move that byte class within
the line; positive values move glyphs upward. This configured value is applied
after automatic visual alignment, so it remains an explicit final adjustment.
Fractional values are retained by the Shader Loader SDF path; ARGB fallback output remains
quantized to its source-pixel grid. In manual baseline mode the renderer keeps
the FreeType body top/drop metrics. The visual-center rule then depends on
`verticalMetrics` as described above.

`fontColor="#RRGGBB"` and `fontAlpha` optionally override the fill color for
the font ID. When `fontColor` is omitted, fill geometry keeps the color supplied
by the game. `glow`, `outline`, and `shadow` belong to the font ID and are
shared by both byte classes. `effectQuality="fast|balanced|high"` selects the
PS 3.0 SDF sampling preset and defaults to `balanced`; the presets use 1, 4,
or 8 fractional-position samples. The Shader Loader route always uses an outline-to-SDF
body; `fillRenderMode` is no longer a configuration option. Glow uses `inner`,
`outer`, and `power`; legacy `width` is accepted as
an alias for `outer` only when `outer` is absent. Outline is a VUI+-style dark
proxy drawn behind the fill: `width` gives it a small outside reach and a
non-negative `softness` adds feather without cutting the proxy into a solid
hollow ring. The later fill pass covers the proxy interior, leaving its
filtered edge. Shadow accepts a non-negative `blur` and a positive
`power`; `blur=0` preserves the exact hinted offset mask. On a hard shadow,
`includeGlow="1"` and `includeOutline="1"` reproduce the currently enabled
effect coverage in that offset mask. Copied coverage retains the source effect
alpha, is combined with source-over alpha, and is uniformly tinted by the
shadow color. Both switches default to `0` and are ignored when blur is
non-zero.

Every effect accepts `colorMode="fixed|fill"`. The backward-compatible default
is `fixed`: the pixel shader selects identity for the live Tile and per-glyph
base RGB, then applies the effect's configured packet color. `fill` keeps the
live Tile RGB and gives the effect the same RGB modifier as the body, including
a configured `fontColor`; the effect's `color` RGB is then ignored. In either
mode, the effect's own `alpha` remains independent of `fontAlpha` and is
multiplied by the game text alpha, so visibility and fade animations continue
to work.

## Encoded-unit and fixed-width layout

Layout decodes and positions each encoded single-byte or DBCS unit in one pass.
Each runtime face keeps sparse glyph-index pages for the base-size advance and
fixed-cell offset. Code-page prewarm also fills these metrics when a persistent
manifest already supplies glyph identity, so the first visible menu does not
repeat hundreds of FreeType metric loads. An uncacheable glyph index or
allocation failure still reads the live FreeType slot for that unit and never
substitutes a zero advance.

A positive `fixedWidth` centers each glyph body in a logical cell and makes its
final advance `fixedWidth + tracking`. This matches the grid-oriented DCFGCF
behavior used by interfaces such as the terminal hacking screen. A value of
zero retains proportional advances.

## Blocking prewarm and persistent caches

Startup prewarming is mandatory for every configured FreeType font and has no
XML mode switch. In FreeType-only mode it enumerates the 224 visible
Windows-1252 byte units `0x20-0xFF`; no double-byte scan is performed. With the
multibyte hook active, it walks the same complete, validated encoded-unit table
used by the persistent manifest. This includes every Windows-decodable pair in
the active CP936, CP950, CP932, or CP949 code page rather than a GB2312/common
subset or DCFGCF range approximation. Each unit resolves through its complete
single-byte or double-byte face/fallback chain. Prewarming begins after the
configured fonts are activated. On
the first game-loop callback, tNVSE synchronously drains the complete queue at
`fFreeTypeFontResolutionScale`; it does not wait for a menu root or device
scale. The game remains blocked until every queued profile reports `complete`,
`atlas-full`, or `cancelled`. Prewarm and demand rendering share one canonical
source scale. UIO-derived calls reuse that mask and atlas profile
instead of generating per-zoom variants.
The full-table coverage contract uses persistent completion identity 3. Older
mode-2 manifests and their DCFG-range atlas snapshots cannot satisfy it, so the
first launch after this change discards those construction artifacts and builds
the complete table once.
While this startup barrier is active, a non-activating English progress window
runs on a separate UI thread. It shows the current font ID, SDF/ARGB-fallback route,
the active scan or snapshot stage, and overall progress. The window remains
responsive while FreeType work blocks the game thread and closes automatically
before control returns to the game. It is owned by the Fallout window rather
than being system-topmost, so Windows manages its minimize and Z-order behavior
together with the game without a polling timer.
A font task allocates additional atlas pages when its complete set cannot fit
one 4096x4096 page. It reports `atlas-full` only if one incoming batch cannot
fit an empty maximum-size page, the page-count safety limit is reached, or a
texture allocation/upload fails. Full code-page prewarming generates every mask
that runtime rendering can request for every valid unit. Consequently the SDF
fill is prewarmed for the complete code page and every SDF effect or hard shadow
reuses that mask. When Shader Loader is unavailable, prewarm generates only the
coverage/effect masks needed by the ARGB fallback.

Full-code-page construction does not create or read `.tnvfmask`. The atlas-only
transaction begins as soon as the first configured font is queued, so an early
draw between activation and the blocking prewarm pump cannot create a
short-lived persistent mask profile. Distance-field pixels and aggressive A8
Fill/Glow/Outline/Shadow coverage are rasterized into bounded in-memory batches
and written directly into streamed `_p<page>.tnvfatlas` snapshots. Aggressive
coverage uses its `A8 + CpuEffects` atlas identity and never aliases an MTSDF
double-byte owner. The transaction scope restores the ordinary persistent-mask
policy automatically after success, cancellation, or failure.

The `.tnvfmask` format remains available for demand-only rendering outside that
transaction. A persistent profile is keyed by the font file's content hash,
actual collection face, effective pixel dimensions, embolden, slant, stroke
width, SDF spread, and mask type. If every configured profile completes the
mandatory pass, every runtime is ready, both byte-role atlas profiles survive
the final reread/repack, and the manifest is complete, tNVSE closes any legacy
bitmap profiles and deletes every managed `.tnvfmask` left by older versions.
Persistent bitmap creation is then disabled for every covered font ID and its
aliases for the rest of the process. An incomplete transaction leaves legacy
files available to the restored demand-rendering policy, but the failed prewarm
does not publish a newly generated `.tnvfmask`.

If startup validation finds an incomplete manifest, a missing or corrupt atlas
page, a snapshot without the final global-repack marker, or a failed
global-repack generation, tNVSE does not resume or repair that partial
transaction. It first evicts the affected resident atlas generation, deletes
its manifest and atlas snapshots, clears all shared construction-mask profiles,
and then performs a new code-page pass from an empty cache state. A stream or
finalization failure applies the same cleanup immediately, so half-published
files are not candidates on the next launch. Global repacking is allowed only
inside the current generation transaction after all streamed pages have been
published.

The same directory also contains three startup-oriented cache layers. A
`.tnvfhash` record reuses the font content hash when file identity, size and
last-write time still match. A v9 `.tnvfmanifest` stores the encoded unit's
Unicode value, fallback face/glyph identity, and serialized `FontLetter` metrics
as a sorted sparse table containing the 256 single-byte values plus only valid
double-byte units from the active code page. A complete manifest is validated
once into a direct encoded-unit index, and its double-byte `FontLetter` metrics
are copied into the runtime direct table. Runtime fonts with the same
`manifestHash` share one file
handle, mapping handle, and mapped view instead of mapping that file once per
font ID. One `_p<page>.tnvfatlas` snapshot per atlas page stores
the stable glyph-ID placement map. Snapshot v16 records the byte role, the
selected distance-field method, and the validated runtime UV subset explicitly,
and stores A8 true-SDF, BGRA MTSDF, or CPU-effect rectangle payloads. The
CPU-effect coverage revision is scoped into the page-content identity, so
revised effects do not invalidate unrelated true-SDF/MTSDF snapshots.
Its placed level-zero payload stores the raw per-glyph rectangle texels directly;
`storedPixelBytes` must equal `pixelBytes`. Payload checksums and page-content
identities are calculated from those same raw texels.
Older snapshot layouts are not read or migrated.
Distance-field profiles are packed in deterministic height/width/glyph-ID order
within each bounded raster batch. The streaming writer carries its open shelf across
batches, closes pages at 2048x2048, shrinks the tail to the smallest usable
power-of-two dimensions, and publishes that layout as an intermediate
transaction generation. Finalization stages only its headers, placements, and
source-file identities in the atlas index, with zero temporary GPU bytes. The
global skyline repacker is retained: it reads one bounded source page at a time
from disk, materializes one destination page at a time, and rewrites the
globally repacked snapshots before the manifest is committed. Only the final
repacked generation is uploaded. This preserves the page-count and tail-page
VRAM/disk savings of global repacking while removing the former
upload-repack-discard-upload peak. Distance-field pages store only the placed
level-zero rectangles;
other pages retain their complete mip chain. Each page records and validates
the total page count. After a successful full prewarm every page is written
through temporary files, globally repacked, and then the manifest is marked
complete. Only after the final page set has been validated and both roles are
resident may complete
code-page mask files be deleted. A later launch restores the
complete page set directly and skips code-page enumeration, per-glyph mask
loading, packing, and mip generation.
An MTSDF byte-role alias validates and, if necessary, restores only the shared
double-byte role that it consumes. The owner's unrelated single-byte profile may
be evicted by the GPU LRU without making the alias transaction incomplete. This
allows every alias manifest to commit under the configured atlas budget, so a
later unchanged launch remains on the snapshot path instead of repeating its
single-byte construction.
Every layer includes its schema/layout/mask/font/code-page inputs in its hash;
stale files are ignored rather than migrated.
Configured face chains hash the separator-normalized path written in XML, not
the filesystem path after a relative `Data\...` value has been expanded with
the current game directory. Runtime manifest and atlas identities then combine
that portable configuration identity with the ordered loaded-face count,
collection face index, and actual font-file content hash. Consequently an
unchanged relative-path configuration can reuse its cache after the game is
moved or installed under a different root directory. Absolute paths in XML
remain explicit configuration identities, and their referenced file contents
must still match.

`bDeleteUnusedFreeTypeFontCache=1` removes stale `.tnvfmask`, `.tnvfhash`,
`.tnvfmanifest`, and `.tnvfatlas` files that were not accessed by the current
run after every configured font atlas has been generated or restored
successfully. If a prewarm job fails or is cancelled, cleanup switches to a
safe partial scope: caches identified as the inactive true-SDF/MTSDF method,
unreadable managed cache headers, and orphaned `.tmp`/`.stream.tmp` transaction
files are removed, while current-method and mode-neutral caches are retained.
Manifest headers carry an explicit cache-domain and distance-field identity so
the partial cleanup does not infer their route or method from an opaque filename
hash. Unknown files in `fontdata` are never removed. The option defaults to `0`.

The final native route is synchronized during deferred initialization, before
configured game fonts enter blocking prewarm. Aggressive A8 coverage and the
stock-shader ARGB fallback both select the CPU-coverage cache domain. Selecting
that domain forcibly closes in-process distance-field bitmap/manifest mappings
and invalidates every normal true-SDF/MTSDF `.tnvfmask`, manifest, atlas
snapshot, and incomplete atlas transaction, independently of
`bDeleteUnusedFreeTypeFontCache`. CPU-coverage manifests have a separate domain
identity and are retained on later CPU-route launches. If a font was activated
before Shader Loader initialization, a manifest from the provisional route is
detached when the final route is synchronized, and a queued prewarm job whose
route identity is stale is cancelled rather than being executed under the new
route.

Cache identity is split by responsibility. The layout hash covers font faces,
metrics, advances, and fallback identity. Persistent manifests store
effect-independent body metrics and add the current visual effect extents when
loaded. The mask-generation hash covers only outline inputs that change glyph
pixels. The atlas-content hash is resolved at the final raster scale from that
mask identity, the actual mask-type combination, the quantized SDF spread, and
CPU fallback stroke widths. Shader colors, offsets, powers, inner thresholds,
and quality selection use a separate shader-effect hash and do not invalidate
a distance-field atlas. Consequently an effect edit selects a new prewarm
snapshot only when it changes the final distance-field spread or the masks that the atlas
must contain.
The ARGB CPU fallback remains content-sensitive: independently generated
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
Atlas identity follows that routing boundary: each raster profile has independent
`singleByte` and `doubleByte` page sets, profile indices, LRU entries, and
snapshot files. Mixed text can bind pages from both roles in one shape, but a
`cacheId` lookup never crosses roles. Each role hashes only its own configured
style/face chain and the content identities of the font files actually loaded
for that chain. Replacing a double-byte font therefore leaves a compatible
single-byte atlas resident and its disk snapshot reusable, and vice versa.
In FreeType-only mode the effective FreeType code page is always 1252, even if
`uiEncoding=1-4` remains configured, and every byte uses `singleByte`.
Runtime glyph, layout, mask, manifest and atlas-snapshot identities all
use that effective code page. Code-page-0 font caches are legacy and are never
restored. Missing glyph lookup stays inside the selected byte-class fallback
chain and then tries `U+FFFD`, `?`, and the primary face's `.notdef` glyph.

## Atlas and Tile shader routing

When Fallout Shader Loader 1.40 or newer and the complete native FreeType
shader set are available, `uiFreeTypeFontDistanceFieldMode` selects the native
distance-field representation. `0` generates msdfgen true SDF into a
level-zero `D3DFMT_A8` atlas. `1` (the default) generates MTSDF into
`D3DFMT_A8R8G8B8`; D3D9 memory is BGRA, sampled RGB carries the
multi-channel Fill field, and sampled Alpha carries true signed distance for
effects.

`bEnableFreeTypeFontAggressivePerformanceMode=1` overrides that selection.
FreeType hinting rasterizes Fill once. A bounded CPU distance transform then
evaluates Glow falloff and outer feather, Outline softness, blurred Shadow
power, and hard-Shadow Glow/Outline source-over composition with the same
configured parameters as the distance-field shaders. The resulting masks
remain in level-zero `D3DFMT_A8` atlas pages, and a dedicated `ps_3_0` coverage
shader performs one texture instruction. Effect RGB, opacity, and the
fixed-versus-live Tile RGB selector are carried in the vertex stream, so
adjacent same-page layer and color ranges merge without changing their
Shadow/Glow/Outline/Fill order. A common single-page text facade consequently
uses one packet and one draw. This mode intentionally gives up distance-field
magnification quality in exchange for stock-like sampling, packet, and atlas
cost; it no longer substitutes opaque FreeType strokes for configured effect
coverage.
The startup prewarm publishes those generated coverage masks as globally
repacked `.tnvfatlas` pages. A later launch validates and restores that atlas
profile directly, so aggressive mode does not need `.tnvfmask` restoration to
avoid rerasterizing the complete code page.

The aggressive mode never removes the fallback boundary. Without Fallout
Shader Loader, with an old Loader version, with missing Loader exports, with a
missing coverage shader, or when native initialization is unavailable, route
selection occurs before shape construction and tNVSE builds the same
distance-aware hinted coverage and effect masks into an `A8R8G8B8` atlas with
baked colors for the stock Tile shader. Fixed effect RGB is compensated for the
stock shader's live Tile-color multiplication at shape construction. No A8
native facade is created in that state.

The visible text on a native route is
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

The native vertex stream carries one per-glyph base modifier in `COLOR0`.
Pixel constant `c1` carries the packet's uniform layer modifier, while `c0`
remains the game's live Tile color. Fill and `colorMode="fill"` packets multiply
all three RGB sources. Fixed-color effects ignore the base and live Tile RGB,
but still multiply the base alpha, packet alpha, and live Tile alpha so colored
text and menu fades retain their previous opacity contract. RGB remains
straight rather than premultiplied by alpha. Each immutable native shader
profile preserves the packet's complete `c1-c4` block byte-for-byte; resetting
`c1` to an identity color would collapse every configured effect color to white.
tNVSE does not replace the global TileShader.

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
two exact proxy names. Every recognized proxy completes the chained
`TileText::MakeNode` path under recursive-effect suppression so its width and
height traits remain valid for XML sibling expressions. When the proxy's active
FreeType font already has a configured shadow, glow, or outline effect, tNVSE
culls the finished proxy scene node after layout; the proxy therefore remains a
layout participant without being drawn a second time. If no tNVSE effect is
enabled, or if the font cannot be resolved reliably, the original proxy remains
visible with recursive tNVSE effects suppressed. Unrelated dark or startup text
remains unaffected, and no VUI+ XML file is modified.

## True-SDF/MTSDF fill, effects, and draw-state isolation

Both modes load the same unhinted transformed FreeType outline, normalize
contour polarity, preserve the FreeType nonzero/even-odd fill rule, quantize
through msdfgen's 8-bit simulation, and use derivative-based `screenPxRange`
antialiasing at atlas LOD 0. True SDF reconstructs Fill and effects from the
single Alpha distance. It uses one byte per atlas texel, but acute corners and
intersecting edges can be rounder than MTSDF at the same source resolution.

Both distance-field body shader families and all effect variants use `ps_3_0`.
In MTSDF mode the generator uses deterministic `edgeColoringSimple`, enables
overlap support, applies the FreeType fill rule through msdfgen's scanline sign
correction, reruns compatible edge-priority error correction, and calls
`simulate8bit` before packing the texture bytes. This follows the
[msdfgen library and shader contract](https://github.com/Chlumsky/msdfgen).
RGB median reconstruction owns
Fill topology. Alpha true signed distance owns glow, outline, shadow blur, and
effect-radius decisions. Effects may read RGB only for body exclusion and
under-fill composition; Alpha never replaces RGB as the Fill contour.

The shaders use msdfgen's UV-derivative `screenPxRange`, force texture data to
linear rather than sRGB space, and clamp the range to at least one. Fast Fill
uses one RGB sample, Balanced uses a four-point sub-texel grid, and High uses an
eight-point rotated grid. Effect variants use one or four samples within the
`ps_3_0` instruction budget. Alpha test is disabled for native packets because
thresholding the shader's continuous coverage would recreate edge pixels after
distance-field reconstruction; ordinary source-over alpha blending remains
enabled.

Hard shadow reuses the selected body mask and can analytically copy the active
glow and outline masks. The native payload stores each unshifted body quad only
once and lets the glow, outline, and distance-field fill packets reference
the same vertex interval. Shadow owns a second interval only when its configured
offset is nonzero. Thus native `Shadow + Glow + Outline + Fill` uses two geometry
quads per drawable glyph instead of four, and the same stack without an offset
shadow uses one. Effects
execute global shadow, glow, outline, and fill passes over one `NiTriShape`,
which prevents a later glyph effect from covering an earlier glyph fill. Native
passes use bilinear MIN/MAG sampling at atlas LOD 0 and derivative-based edge
antialiasing; they never consume the coverage-averaged atlas mip chain.
Distance-field draw ranges also preserve fractional pen positions, encoded-unit
advances, and effect offsets in their quad coordinates. The separate ARGB fallback remains
snapped to the resolved source-pixel grid and may use trilinear mip sampling.

Glow keeps
full intensity through `inner`, then decays to zero at `outer` according to
`power`; outline uses `width` plus `softness`; blurred shadow uses `blur` and
`power`. The physical distance-field spread is derived from the largest enabled radius
and must remain in tNVSE's supported 2-32 pixel range. An unsupported
spread causes the complete text batch to use the CPU effect path rather than
silently reducing the requested effect.
Because a distance-field body requires the custom native shader, failure to establish or
complete that route rebuilds the batch through the ARGB CPU fallback instead of
sampling distance data with the stock Tile shader. No failure path treats the
true-SDF A8 texture as ordinary coverage. When `NVSE_PLUGIN_PATH` is defined, an
ordinary project build copies the native shader set to `Data\Shaders\Loose`.
Native packets use immutable `TileShader` profiles and the game's normal render
submission. Effect passes preserve the live Tile color/alpha contract and use
separate source-over alpha where the target supports alpha writes, so shadows
outside the fill survive off-screen UI compositing without reducing existing
destination alpha. A detected profile, packet, or device-state mismatch marks
the native generation faulty and suppresses the affected marked submission.
The tNVSE-owned pixel constants `c1-c4` are captured once for the complete
native text group, then restored and verified once after its final packet.
After stock `TileShader::UpdateConstants` refreshes Gamebryo's maps, each native
profile mirrors the retail `c0` definition from the current proxy property state
and uploads `c0-c4` in one device call. This is required because the native pixel
programs and `c1-c4` bypass the stock constant-map upload path; relying on its
change tracking alone can leave a stale RGB register. Fixed effects ignore c0
RGB in HLSL, and group cleanup deliberately leaves c0 at the final Tile value.
Individual shadow, glow, outline, and fill packets therefore do not perform
redundant save/restore cycles. Native packet
preflight is cached per shape while the shader generation, scaled-fill sampling
class, alpha-blending class, and referenced D3D atlas textures remain unchanged.
Any device reset, sampling/alpha transition, or page-resource replacement falls
back to full packet and texture validation before the cache is refreshed.
Per-frame native submission also keeps thread-local hot entries for shape
metadata, static vertex residency, dynamic ring residency, static-promotion
candidates, and the preferred proxy slot. Metadata entries are protected by a
sharded mutation generation, while ring entries require the exact immutable
payload owner plus the current resource serial and upload epoch. Consequently a
shape deletion, pointer reuse, ring discard, device reset, or generation change
cannot reuse stale state. A proxy remembers its stable Tile property and its
last atlas property, texture, and shader; transforms, alpha, scissor state, and
other live Tile values are still copied each submission, but unchanged
reference-counted properties and bindings are not assigned again.

Native vertices store `float3` position, `float2` UV, and one packed
`D3DCOLOR` base color. The D3D declaration expands that 24-byte record to the
vertex shader's existing normalized `float4 COLOR0`; the packet-uniform layer
modifier remains in pixel constant `c1`. Compared with the previous four-float
base color this reduces cached native geometry and dynamic/static VB traffic by
one third without adding packets or draw calls. After the stock Tile list has
been sorted, tNVSE preflights its FreeType facades, deduplicates their immutable
text artifacts, and promotes all eligible artifacts with one static-VB
Lock/copy sequence/Unlock. Tiles are not persistently merged: stock depth/order,
per-Tile transform, scissor, alpha, and shader constants remain independent.
The dynamic ring retains its two-maximum-payload capacity, while the static VB
starts at approximately 4 MiB instead of reserving its approximately 12 MiB
packed-format maximum. When it is full, a safe submission boundary with no
other active proxy compacts live weakly owned artifacts into a replacement
buffer and doubles capacity up to that fixed maximum. If the sorted-call hook is
unavailable or replaced, the existing per-shape promotion route remains active.
Concurrent groups defer optional promotion and continue through the dynamic
ring; expired static entries are discarded during compaction.

## Atlas allocation, mipmaps, and memory

Persistent atlas pages start at 512x512 and grow without moving existing glyphs.
Missing glyphs are rasterized as one batch and uploaded through one dirty
rectangle. Level-zero-only true-SDF/A8, aggressive hinted-coverage A8, and
MTSDF/BGRA pages use one
outside-distance padding pixel per side, which is sufficient to isolate their
bilinear footprint because
the distance spread and an additional guard texel are already inside each glyph
bitmap. ARGB fallback pages retain four
transparent pixels per side, isolating the 1/4 mip even when glyph dimensions
are not multiples of four. Repeated text also reuses cached layout and unique
text artifacts. One artifact owns the packed vertices, bound, atlas-page
property/texture references, and merged contiguous packet descriptors that used
to live in separate batch and packet-template caches. Geometry, per-glyph base
colors, layer constants, baked-coverage mode, and referenced page identities
therefore form one
validated cache identity; an atlas wrapper address cannot revive an artifact
whose retained property or texture differs. True SDF, MTSDF, and 32-bit fallback
profiles use separate cache keys; aggressive A8 coverage additionally has a
distinct pixel/render profile and prewarm identity. Changing
`uiFreeTypeFontDistanceFieldMode`
therefore selects new bitmap, manifest, snapshot, and shader identities without
requiring manual cache deletion. CPU coverage and distance-field manifests
cannot alias, and final CPU-route selection removes normal distance-field cache
files instead of retaining a provisional native/fallback mixture.

Generated distance fields and ARGB-fallback masks and their supporting CPU
objects are cached in process memory. Equivalent masks are shared across font
IDs when the resolved font file/face, glyph, effective raster size, emboldening, slant, stroke or SDF
spread parameters, and mask type match. Baseline placement remains per font ID and is
not baked into the shared mask.

At identical atlas dimensions, MTSDF uses four bytes per texel and true-SDF A8
uses one. GPU pages, compact snapshot rectangle payloads, streamed prewarm
buffers, and live distance-field glyph bitmaps therefore use approximately
four times the texel storage in MTSDF mode. The existing CPU and GPU budgets remain
authoritative, so a fixed budget can retain fewer MTSDF pages; it does not
silently exceed the configured ceiling to preserve the old page count.

`uiFreeTypeFontMemoryCacheMB` is one aggregate CPU-memory ceiling shared by
glyph bitmaps, layout runs, prepared text, unified text artifacts, atlas
metadata/backing data, persistent file mappings, runtime font metadata, and the
retained CPU maps/scratch buffers used by native submission. Cached/shared
objects and static-promotion candidates hold category leases for their actual
lifetime. Removing an LRU or map key therefore does not pretend to reclaim an
object that a live shape or thread-local hot entry still owns, and the old
per-cache fractions are only preferred local targets constrained by the
remaining global headroom, not independent budgets. When the total is above the
ceiling, tNVSE trims prepared text, optional native residency/candidate maps,
unified text artifacts, layouts, and glyph bitmaps in reconstructibility order.
Memory still referenced by active shapes, atlases, font runtimes, static GPU
residency, or required mappings is reported as `pinned-overcommit` instead of
being invalidated silently.

During blocking prewarm, the bitmap LRU keeps a preferred one-quarter working
target so wide raster batches do not churn, subject to the aggregate ceiling.
After every queued profile finishes successfully from a snapshot or a newly
saved atlas, tNVSE releases any legacy persistent-mask mappings and immediately
lowers the bitmap LRU target to one-sixteenth of the configured memory, clamped
to 8-16 MiB and never above its original share. With the default 192 MiB setting
this target is 12 MiB. A later genuine prewarm job restores the larger working
target before rasterization. If any profile is cancelled, fills its atlas, or
fails to save, automatic shrinking is skipped because demand rendering may need
the in-memory masks while its normal persistent policy resumes.

When
`bEnableFreeTypeDefaultPoolAtlas=1`, tNVSE creates dynamic `D3DPOOL_DEFAULT`
atlas textures and retains only the masks used by each live atlas generation;
it does not retain a complete CPU copy of the atlas. The current and retired
generations are restored after a D3D9 device reset. A version-16 snapshot
records A8 true SDF, BGRA MTSDF, or CPU-effect coverage and is uploaded
directly to this path.
Its cache identity includes the persistent
glyph-manifest ABI, so an incompatible or newly revised manifest cannot make an
old atlas look restorable and then force a shared-font regeneration. Streamed
prewarm caps distance-field pages at 2048x2048 (4 MiB for A8 true SDF or
16 MiB for BGRA MTSDF)
to avoid late 64 MiB page allocations and transient vector-growth peaks in the
32-bit process; large code pages are split across additional snapshot pages.
Before restoring a role, tNVSE inspects all page headers, reserves its
worst-case GPU footprint by evicting older LRU pages, and immediately releases
unreferenced retired generations. Restore retains only headers and placement
tables in CPU memory. It reads and checksum-verifies one bounded snapshot page
at a time, copies that page into the locked D3D9 texture without per-row file
calls, and releases the page buffer before creating the next texture. Once
upload succeeds, tNVSE retains only placements plus the validated snapshot
path/header identity. Device reset, page detachment/growth, and snapshot rewrite
read the raw packed rectangles from `_p<page>.tnvfatlas` and stream them by row.
The payload checksum is verified before the texels are accepted into the locked
texture. Its page-content
fingerprint covers dimensions, format, placed
coordinates, and exact texels. A hash match is followed by byte-for-byte
comparison, loading temporary source pixels only during that comparison, before
equivalent pages share one D3D9 texture allocation through independent Gamebryo
wrappers. A later glyph insertion first detaches that page, and a device reset
rebuilds each wrapper independently. Glyphs added later retain only their
individual masks. If direct
DEFAULT-pool creation is unavailable, snapshot restore and normal atlas
creation fall back to the engine-managed implementation.

The prewarm batch estimator counts one byte per true-SDF texel or four bytes per
MTSDF texel. The writer reuses one preallocated 4 MiB or 16 MiB page buffer per
byte role, eliminating
vector-growth peaks and repeated large allocations, then releases those buffers
before D3D9 restore. A worker-thread allocation exception is returned to the
main prewarm transaction instead of terminating the process; the current batch
is rolled back and retried at half size, up to the one-glyph limit.
Configurations with identical layout/mask inputs and the same maximum effect
radius share one distance-field prewarm even when their colors, offsets, powers,
or shader sampling quality differ; those properties do not alter atlas texels. CPU-baked
fallback profiles continue to include the complete effect hash.

A restored page uses one contiguous glyph-record vector sorted by `cacheId`.
Each record keeps the ID, rectangle, an index into the page's compact snapshot
metadata, and an optional live bitmap. The profile-level page index remains the
single hash lookup that chooses a page; lookup inside that page is a binary
search. Restore therefore performs one contiguous glyph-record allocation per page, does not
allocate an empty `GlyphBitmap` or hash node per glyph, and does not mirror the
same IDs in separate placement and resident-bitmap hash tables. Metadata objects
are materialized lazily only for restored glyphs that text actually requests;
runtime-added glyphs attach their existing bitmap directly to the same record.

`uiFreeTypeFontGpuAtlasCacheMB` controls the soft GPU atlas budget. A value of
zero selects one eighth of the available texture memory, rounded to 16 MB and
clamped to 64-256 MB; 128 MB is used when the device does not report a reliable
value. A nonzero value is used directly. Atlas generations still referenced by
visible game shapes cannot be evicted, so live usage may temporarily exceed the
soft budget. The resolved value is written to `tnvse.log` at initialization and
when a device reset changes the automatic result. Validated distance-field
snapshots restore directly to the DEFAULT pool when enabled. ARGB fallback atlases are
runtime-only and contain three mip levels (1x,
1/2x, and 1/4x); the cache budget and upload counters include all levels.
Limiting the chain to three levels together with four-pixel per-side packing
padding prevents the coarsest bilinear footprint from reaching a neighboring
glyph. Text spanning pages is sorted into contiguous layer/page native packets;
each packet owns the corresponding atlas property and texture. Distance-field
packets use mip level zero and a zero LOD bias through their immutable shader profile.

## Scope and fallbacks

Glyph rasterization remains CPU based: FreeType produces hinted fallback
coverage and unhinted vector outlines, while the statically linked msdfgen core
converts native-route outlines to true SDF or MTSDF.
Adding Skia, D3D11, or D3D12 would require a readback or cross-API copy before
Fallout New Vegas can consume the result through D3D9, increasing
synchronization cost and reducing DXVK/Wine compatibility. The GPU is therefore
used for persistent atlas sampling and quad rendering, while masks are
generated once on the CPU.

When output resolution or UIO 2.30 scales a TileText call, tNVSE keeps the mask
at the single configured source multiplier and lets the existing world
transform minify or magnify the atlas. Trilinear sampling is enabled for scaled
ARGB fallback shapes; distance-field ranges retain level-zero derivative-based
sampling.
Neither case creates a resolution- or zoom-specific profile. If atlas creation
fails, the affected FreeType shape is empty and the detailed build diagnostic
identifies the failed stage. Ordinary and rich-text layout retain one output
glyph per encoded single-byte or DBCS unit and do not perform OpenType shaping
or bidirectional reordering. Color-font rendering and variable-font axis
controls are outside this feature.
Invalid or unavailable
configurations leave that entire font ID on the original `.fnt`/`.tex` renderer.

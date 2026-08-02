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
bEnableFreeTypeFontCommandBuffer=0
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
Deferred FreeType diagnostics use dynamically sized line storage, so periodic
performance records are written in full as counters are added. The fixed
128-line queue and 16-lines-per-flush limit still bound diagnostic backlog and
per-frame disk work; only the former 1024-byte per-message truncation has been
removed.

Successful per-shape metadata allocation/deletion audits and successful
per-text atlas-batch records are intentionally omitted. Their aggregate values
remain available in the performance counters, while metadata integrity failures
and real route fallbacks still emit complete records. The temporary font-8
singleton binding/driver trace has been removed now that it established that
the missing MAPMO text reached a successful draw and was subsequently covered
by another equal-depth UI item.

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
          prewarmEncoding="gbk"
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

After a complete manifest has populated the generated code-page metric table,
ordinary layout, measurement, wrapping, `MakeString`, and `CreateText` resolve
single-byte entries directly and DBCS entries through the dense generated table.
Decoded render glyphs retain their encoded unit, so atlas compilation does not
repeat code-page decoding or derive a second glyph identity.

Rich-text rendering uses fixed font-ID builder slots for the normal retail and
registered-font range. Pointer-keyed builders remain only for an out-of-range
font, slot collision, or another ambiguous lifetime case. Prepared-text lookup
has an eight-slot hash-indexed TLS front keyed by the already computed
text/config signature. A miss compares at most one resident hash before the
exact collision check. Admission is three-phase: the first observation bypasses
the global cache, the second builds the resident value without a guaranteed-empty
global lookup, and the store publishes that value directly into the TLS slot.
Only a later TLS miss for an already observed key probes the global table.
Cache-generation changes clear the TLS front, and a value that cannot be stored
resets its admission state
and receives an eight-observation retry cooldown. Unsupported or
budget-rejected text therefore stays on the lock-free layout path instead of
repeatedly locking on a known global miss.

## Bounded-throughput prewarm and persistent caches

Startup prewarming is mandatory for every configured FreeType font. In
FreeType-only mode it enumerates the 224 visible Windows-1252 byte units
`0x20-0xFF`; no double-byte scan is performed. With the multibyte hook active,
each font normally walks the complete validated encoded-unit table used by its
persistent manifest. Under CP936, the per-font
`prewarmEncoding="gb2312"` setting restricts startup generation to the
assigned, round-trippable GB2312 units in the A1-F7/A1-FE byte zone;
`prewarmEncoding="gbk"` is the default and enumerates every Windows-decodable
CP936 pair. This setting changes only
startup coverage: text is still decoded as CP936, and an encountered GBK
extension outside a GB2312 profile takes the demand-generation route for that
text without invalidating the font's sealed GB2312 profile. CP950, CP932, and
CP949 always use their complete code-page tables. Each unit resolves through
its complete single-byte or double-byte face/fallback chain. Prewarming begins
after the configured fonts are activated and the final native rendering/cache
route has been synchronized. The persistent queue uses
`fFreeTypeFontResolutionScale` and remains active until every queued profile
reports `complete`, `atlas-full`, or `cancelled`.
Prewarm and demand rendering share one canonical source scale. UIO-derived calls
reuse that mask and atlas profile instead of generating per-zoom variants.
The selected-table coverage contract uses persistent completion identity 3. Older
mode-2 manifests and their DCFG-range atlas snapshots cannot satisfy it, so the
first launch after this change discards those construction artifacts and builds
the selected table once.
Prewarm is advanced by the persistent state machine while tNVSE remains inside
the NVSE `kMessage_DeferredInit` callback. Each loop iteration performs at most
one snapshot restore/validation, one adaptive throughput batch, one font publication
step, or one cleanup step. The game main thread therefore does not return from
deferred initialization until the state reaches `Completed` or the queue is
empty. No `StartMenu::Create` detour, replay, timeout, synthetic Menu ID, or
input handler is needed: the normal startup sequence cannot create an operable
main menu while NVSE is still dispatching `DeferredInit`.

`Data\Menus\prefabs\tNVSE\FontPrewarmOverlay.xml` supplies the native full-screen
shade, current font/route text, stage text, progress bar, and percentage. The
component is generation-only: snapshot validation and successful persistent
cache reads/restores do not load or display it. tNVSE creates it only after a
cache miss has been confirmed and the first streamed glyph-generation job is
ready to begin, then destroys it immediately after the last generated font is
published, before cache verification and cleanup continue. Like Cell Offset
Generator, tNVSE loads this single-root `rect` directly beneath the stock
`LoadingMenu::pRootTile` obtained from the retail LoadingMenu singleton at
`0x11DA0C0`. The component is not a standalone Menu and does not attempt to
capture input. Startup remains blocked by the `DeferredInit` call boundary
during both visible generation and invisible cache validation/restoration, not
by Tile target or stacking traits.

Fallout's existing LoadingMenu update/render thread continues its
`Update`/`ShowChanges` work while the game thread advances bounded prewarm
steps. tNVSE yields with `Sleep(0)` between active steps so the loading thread
can consume current Tile traits. Generation batches target approximately
250 ms and progress-trait writes are rate-limited to 10 Hz; this keeps the
LoadingMenu responsive without paying Tile rebuild and worker-startup overhead
for thousands of one-glyph steps. After the final generation/publication step,
the component is hidden and deleted from the LoadingMenu tree; any remaining
verification and cleanup finish invisibly before `DeferredInit` returns.
Subsequent `kMessage_MainGameLoop` callbacks perform normal A8, DEFAULT-pool,
and performance-cache maintenance; they no longer drive startup prewarm.

If the LoadingMenu root is unavailable, or the XML/component tree is missing or
invalid, tNVSE logs the condition once and completes the same blocking cache
transaction without a visual progress component. It never falls back to
`InterfaceManager::pMenuRoot`, a StartMenu hook, or a Win32/GDI window.
Snapshot restore or final publication can replace the atlas generation used by
font slot 1, so the service invalidates and rebuilds all prewarm `TileText`
geometry after those steps. It also reads the live title/detail/stage/percentage
heights and reflows the panel, progress bar, and labels, rather than assuming a
24-pixel font slot. All four prewarm `TileText` nodes use numeric `zoom=80`, so
UIO applies the same compact font-slot scaling as the IME overlay before those
live metrics are measured. There is no auxiliary Win32 window, prewarm UI
thread, GDI renderer, event, mutex, or window-message pump.
A font task allocates additional atlas pages when its selected set cannot fit
one 4096x4096 page. It reports `atlas-full` only if one incoming batch cannot
fit an empty maximum-size page, the page-count safety limit is reached, or a
texture allocation/upload fails. GBK and non-936 full-code-page profiles
generate every mask that runtime rendering can request for every valid unit.
GB2312 profiles generate the same masks for their selected byte zone and leave
GBK extensions for demand generation. Every SDF effect or hard shadow reuses
the generated fill mask. When Shader Loader is unavailable, prewarm generates
only the coverage/effect masks needed by the ARGB fallback.

Selected-table construction does not create or read `.tnvfmask`. The atlas-only
transaction begins when the first cache-miss font enters its generation step
and ends on completion, cancellation, shutdown, or failure. Distance-field
pixels and aggressive BGRA
composite glyphs are rasterized into bounded in-memory batches and written
directly into streamed `_p<page>.tnvfatlas` snapshots. An aggressive composite
contains the final Shadow, Glow, Outline, and Fill source-over result in one
rectangle and uses its `ARGB32 + CpuEffects` atlas identity; it never aliases an
MTSDF double-byte owner. The transaction scope restores the ordinary
persistent-mask policy automatically after success, cancellation, or failure.

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

The same directory also contains four startup-oriented cache layers. A
`.tnvfhash` record reuses the font content hash when file identity, size and
last-write time still match. A v12 `.tnvfmanifest` stores the encoded unit's
Unicode value, fallback face/glyph identity, and serialized `FontLetter` metrics
as a sorted sparse table containing the 256 single-byte values plus only valid
double-byte units from the active code page. A complete manifest is validated
once into a direct encoded-unit index, and its double-byte `FontLetter` metrics
are copied into the runtime direct table. Runtime fonts with the same
`manifestHash` share one file
handle, mapping handle, and mapped view instead of mapping that file once per
font ID. Version 12 removes the unused per-glyph 16-band collision profile, so
prewarm no longer scans every Fill/distance-field texel a second time and each
manifest record is 52 bytes instead of 124 bytes. One `_p<page>.tnvfatlas`
snapshot per atlas page stores
the stable glyph-ID placement map. Snapshot v23 records the byte role, the
selected distance-field method, and the validated runtime UV subset explicitly,
and stores A8 true-SDF, BGRA MTSDF, or BGRA composite rectangle payloads. The
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
global skyline repacker is retained: it evaluates every supported power-of-two
target width with both deterministic bottom-left and best-fit skyline
heuristics. Complete plans are compared lexicographically: the fewest physical
pages wins, then the smallest total GPU storage, then the smallest maximum page
edge. Thus a complete font remains on one texture whenever either evaluated
skyline heuristic finds a legal one-page layout, while that one page may be
`8192x4096`, `4096x8192`, or a smaller power-of-two rectangle instead of
inheriting the role's square upper bound.
NPOT dimensions are deliberately not used. The selected plan still reads one
bounded source page at a time from disk, materializes one destination page at a
time, and rewrites the globally repacked snapshots before the manifest is
committed. Only the final repacked generation is uploaded. This preserves the
page-count and tail-page VRAM/disk savings of global repacking while removing
the former upload-repack-discard-upload peak. The packing revision, effective
device limits, and aspect-ratio capability participate in snapshot identity;
the intermediate streaming writer and the final snapshot loader use the same
identity builder so the staged filenames cannot drift from the restore paths.
v22 atlases and their dependent direct tables are rebuilt, while source-font
hashes and glyph masks remain reusable. Distance-field pages store only the
placed level-zero rectangles;
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
single-byte construction. Owner profiles are ordered before their aliases in
both snapshot validation and generation queues, and route-time profile
deduplication retains the physical owner when an owner and alias become
equivalent. If a dependency is still
pending, the alias is requeued behind it rather than being marked failed.
Once the owner role is resident, an alias still builds its own layout manifest
and direct table, but its double-byte metric-only scan advances in batches of
up to 4096 encoded units without issuing duplicate bitmap work. Sealing a
direct table releases the large per-placement CPU lookup index, but that sealed
table remains sufficient proof that the shared GPU profile is complete.
Subsequent aliases therefore reuse the resident double-byte texture without
reloading its physical payload or reserving its full GPU size again. A direct
table is reused only when its layout, effect, atlas, code-page, and role
identities match; aliases with a different logical size build a new compact
table while retaining the same physical atlas.
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
`.tnvfmanifest`, `.tnvfatlas`, and `.tnvfdirect` files that were not accessed by
the current
run after every configured font atlas has been generated or restored
successfully. If a prewarm job fails or is cancelled, cleanup switches to a
safe partial scope: caches identified as the inactive true-SDF/MTSDF method,
unreadable managed cache headers, and orphaned `.tmp`/`.stream.tmp` transaction
files are removed, while current-method and mode-neutral caches are retained.
Manifest headers carry an explicit cache-domain and distance-field identity so
the partial cleanup does not infer their route or method from an opaque filename
hash. Unknown files in `fontdata` are never removed. The option defaults to `0`.

The final native route is synchronized during deferred initialization, before
configured game fonts enter bounded-throughput prewarm. Aggressive BGRA composite glyphs
and the stock-shader ARGB fallback both select the CPU-coverage cache domain. Selecting
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
The ARGB CPU routes remain content-sensitive. In aggressive mode the composite
identity includes every enabled effect's quantized shape, offset, fixed/fill
color mode, RGB, and alpha because all four layers are precomposed into atlas
pixels. The non-aggressive ARGB fallback continues to hash every independently
baked layer/color variant.

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

When jointly packed single-/double-byte roles or physical-font-group roles use
the same placements and texels, v23 keeps one complete physical `.tnvfatlas`
and writes each other logical role as a checked 200-byte alias. The alias keeps
that role's independent snapshot, mask, atlas-content, and byte-class identity,
but points to the physical file by its content-addressed snapshot hash and page
index. Restore validates both headers, the alias record, the complete payload
contract, placement count, sizes, payload checksum, and page-content hash
before streaming pixels from the physical file. Alias chains and external paths
are rejected. This removes duplicate disk payloads without changing atlas
dimensions, page count, texture bindings, or draw calls; NPOT remains disabled.
The `atlas snapshot saved` log reports `physicalAliasFiles` and
`diskBytesSaved`. The v23 format causes one rebuild after upgrading, while an
unchanged following launch restores the aliases directly. Because this is an
ownership-container change rather than a glyph-payload change, v23 retains the
v22 payload path identity and replaces each configured v22 file in place. The
old full copies therefore do not remain beside the aliases when
`bDeleteUnusedFreeTypeFontCache=0`; dependent direct-table files follow the
same in-place replacement rule while still validating the v23 header version.

After every configured profile has been verified, snapshot v23 also considers
physical font groups. A group requires at least two distinct single-byte atlas
profiles plus one exactly matching double-byte pixel profile and double-byte
layout profile. The repacker filters already joint role snapshots back to
their logical contents, keeps every unique single-byte glyph set, keeps the
double-byte glyph set once, and removes duplicate content IDs.

If that complete union fits one device-supported power-of-two page, the same
placed snapshot payload is published for every member role. Content-addressed
DEFAULT-pool page reuse then gives all wrappers the same D3D9 texture, and the
sealed, dynamic-direct, and compatibility compilers all collapse logical
wrappers by their underlying D3D9 texture identity. Mixed single-/double-byte
text therefore keeps one physical page ordinal and the one-page draw path even
after a sealed direct profile is invalidated and rebuilt lazily. Runtime group
validation checks complete logical profiles, the physical-group snapshot marker,
snapshot content identity, and the actual shared D3D9 texture.

Direct-batch diagnostics report `pages` as the number of distinct physical
atlas ordinals referenced by drawable glyphs in that batch, rather than the
total atlas-owner count retained by the sealed font profile.

This feature never enables NPOT dimensions. If the union needs more than one
page or its POT
allocation would exceed the group's current de-duplicated physical GPU storage,
the original per-font profiles remain active. That fallback decision is
recorded in their current snapshot generation so later launches do not repeat
the expensive group repack. Whole-font-identical profiles continue to use the
existing exact-page de-duplication path instead.

For CPU-precomposed ARGB profiles, double-byte compatibility includes the
baked effect geometry and colors because those values alter final texels.
Distance-field profiles can share when their raster identities match even
when live shader colors differ. The log reports `physical atlas group active`
or `reused` with the single physical page's `size`, `gpuBytes`, and
`pageContentHash`; `fallback reused` confirms the persistent safe fallback.

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
FreeType rasterizes Fill once from the same unhinted scalable outline used to
generate the distance field. A bounded CPU distance transform mirrors the SDF
formulas for Glow falloff, Outline softness, blurred Shadow power, and
hard-Shadow Glow/Outline inclusion, then composites Shadow, Glow, Outline, and
Fill into one straight-alpha BGRA rectangle in that order. The ARGB fallback
derives its separate masks from that same unhinted Fill. This keeps CPU body and
effect weight close to SDF instead of inheriting a heavier grid-fitted contour.
Effect colors and alpha are baked into the aggressive rectangle. Runtime Tile
transform, scissor, total alpha, and whole-text RGB modulation remain live, but
the individual effect and fill colors can no longer be changed independently
after the profile is built.

Each visible aggressive glyph therefore contributes exactly one quad. A
single-page batch uses one stock `NiTriShape`; a multi-page batch uses one stock
shape per physical atlas page under the same destination `NiNode`. Companion
page shapes inherit the primary shape's final transform, scissor, Tile
color/alpha/fade, alpha/material, and object flags after the stock caller has
configured it; only each page's Tile shade object, source texture, texture path,
and texturing property remain page-specific. No four-mask A8 geometry,
per-layer packet construction, or custom ARGB facade remains in a complete
aggressive profile. Per-glyph precomposition cannot reproduce SDF's global
effect-before-Fill ordering where neighbouring glyph effect rectangles overlap,
and stock one-texture modulation cannot independently preserve live Fill RGB
and fixed effect RGB in the same pixel rectangle. These are retained one-quad
limitations. The mode also gives up distance-field magnification quality in
exchange for `.fnt`-like CPU and geometry cost.
The startup prewarm publishes those generated composite glyphs as globally
repacked `.tnvfatlas` pages. A later launch validates and restores that atlas
profile directly, so aggressive mode does not need `.tnvfmask` restoration to
avoid rerasterizing the complete code page.

The aggressive mode never removes the fallback boundary. Without Fallout
Shader Loader, with an old Loader version, with missing Loader exports, with a
missing ARGB shader, or when native initialization is unavailable, no complete
aggressive direct profile is published and the whole text batch enters the
existing compatibility path. A missing direct record, page replacement,
generation mismatch, or invalid snapshot identity applies the same batch-wide
fallback; direct and bitmap records are never mixed in one submission.

Every native text artifact still defines packets grouped by layer, atlas page,
shader class, and sampling contract. A compatibility submission represents the
complete artifact with one facade in the stock Tile alpha list and expands it
after UI sorting. Once the artifact is sealed in the static vertex arena, the
virtual-stock backend can instead represent every frozen packet with a real
`NiTriShape`. Those shapes enter the original accumulator, original Tile
ordering, original `TileShader::UpdateConstants`, and original
`RenderImmediate`; the plugin no longer replaces a complete facade descriptor
or selects a proxy shape for each such packet.

Both forms use `TileShader`, the renderer-owned native declaration, and
Gamebryo `NiGeometryBufferData`. There is no private D3D draw path or
whole-shape range fallback. If a marked compatibility facade cannot complete
through the native route, that submission is suppressed and logged with a
concrete `submission-suppressed` reason. Atlas-shape construction failure also
returns an empty shape instead of rebuilding the text as legacy outline
geometry, so visible FreeType text must have reached one of the validated
native forms.

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
`TileText::MakeNode` path under recursive-effect suppression so its XML traits
and sibling position remain valid. When the proxy's active FreeType font already
has a configured shadow, glow, or outline effect, tNVSE also enters a
measurement-only scope. The ordinary `Font::CreateText` path still prepares the
text and publishes its final width and height, but returns a degenerate empty
shape without compiling glyph geometry. The rich-text path still prepares its
`TextDoc` layout but skips `TextDoc::Render` glyph emission. The finished proxy
node is app-culled as a final safeguard, so it remains a layout participant
without atlas lookup, effect geometry, or a second text draw. If no tNVSE effect
is enabled, or if the font cannot be resolved reliably, the original proxy
remains visible with recursive tNVSE effects suppressed. `GLOW_BRANCH`,
`NOGLOW_BRANCH`, `_glow`, and image-outline prefabs are not proxy matches because
they also control menu content, font selection, alignment, or image decoration.
Unrelated dark or startup text remains unaffected, and no VUI+ XML file is
modified.

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
redundant save/restore cycles. Native packet preflight is cached per shape
while the shader generation, scaled-fill sampling
class, alpha-blending class, and referenced D3D atlas textures remain unchanged.
Any device reset, sampling/alpha transition, or page-resource replacement falls
back to full packet and texture validation before the cache is refreshed.
Each immutable packet also seals both alpha-class profile hashes when its
artifact is built. After the first profile-map resolution, the packet retains a
process-lifetime profile pointer; a generation, sampling, and alpha-class check
then replaces repeated profile hashing and unordered-map lookup without allowing
a profile from an older D3D device generation to pass.
Per-frame native submission also keeps thread-local hot entries for shape
metadata, static vertex residency, dynamic ring residency, static-promotion
candidates, and the preferred proxy slot. Shape metadata uses a 16384-entry,
eight-way set-associative weak TLS cache. Its pointer hash is shared with 16384
sharded mutation generations so allocator-neighbouring facade addresses do not
continually evict or invalidate one another. The weak owner and generation check
still reject deletion, pointer reuse, and re-registration without retaining menu
payloads. Ring entries require the exact immutable payload owner plus the current
resource serial and upload epoch. Consequently a shape deletion, pointer reuse,
ring discard, device reset, or generation change cannot reuse stale state. A
proxy remembers its stable Tile property and its last atlas property, texture,
and shader; transforms, alpha, scissor state, and other live Tile values are
still copied each submission, but unchanged reference-counted properties and
bindings are not assigned again.

Native vertices store `float3` position, `float2` UV, one packed `D3DCOLOR`
base color, per-glyph distance parameters, and physical glyph bounds as an
ordinary `FLOAT4` in a 52-byte record. The universal float declaration avoids
the optional `D3DDECLTYPE_USHORT4N` capability, which some native D3D9 drivers
and wrappers reject. The D3D declaration expands `D3DCOLOR` to the vertex
shader's normalized `float4 COLOR0`; packet-uniform layer modifiers remain in
pixel constants. Compared with a four-float base color, the packed color field
still saves twelve bytes per vertex without adding packets or draw calls. After
the stock Tile list has been prepared, tNVSE preflights compatibility facades
and virtual-stock primaries, deduplicates their immutable text artifacts, and
promotes all eligible artifacts with one static-VB Lock/copy
sequence/Unlock. Tiles are not persistently merged: stock depth/order,
per-Tile transform, scissor, alpha, and shader constants remain independent.
The retail and symbolized test builds both append accepted Tile geometry to the
accumulator and then apply the same depth-only unstable quicksort. In retail,
`BSShaderAccumulator::FinishAccumulating_Tiles` at `0xB65E80` loads virtual
slot `0xA4`, clears `m_pGeometryList` at `0xB65E95`, calls
`NiAlphaAccumulator::Sort` (`0xA9B570`), and enters the backwards
`RenderAlphaGeometry` traversal at `0xB65EA0 -> 0xB64F90`. The symbolized
August 22 test build confirms the same sequence in
`FinishAccumulating_Interface` (`0x82260438`), the interface-depth source
`world.translate.y` in `NiAlphaAccumulator::Sort` (`0x8221B778`), the exact
pivot/partition recursion in `SortObjectsByDepth` (`0x82276BC8`) and
`ChoosePivot` (`0x82276CA0`), and the backwards traversal at `0x8223F1D0`.

tNVSE now treats the successfully committed stock `AddTail` ordinal as the
original-order anchor. It records an ordinal only when the predecessor grows
`m_kItems` by exactly one and the new tail is the same facade. The nine stock
bytes at `0xB65E95` are replaced as one guarded block by a call that receives
the already loaded sort target in EDX. The fast path is entered only when that
target is still the retail stock sort, interface sorting is active, every depth
is finite, and all facade identity, registration-cycle, duplicate, contiguous
Virtual-stock block, slot, and item-count proofs succeed. A changed call site or
vtable predecessor is never overwritten or bypassed.

An attempted FreeType registration that did not commit an exact `AddTail`
ordinal no longer rejects the anchor by itself. It is ignored as a candidate,
then the source-wide identity pass still requires every FreeType facade that
actually entered the list to have one exact committed ordinal. This removes
culled and duplicate attempts from the proof without allowing an untracked
facade into the reordered output.

The anchored sorter copies the `AddTail` list directly into the engine arrays
while carrying a sidecar ordinal through a decision-equivalent copy of the
stock pivot, partition, swap, recursion, and tail-iteration decisions. It then
uses linear ordinal/run tables to change only a contiguous exact-equal-depth run
that contains both stock and FreeType geometry. Logical blocks are stored by
descending registration ordinal because the renderer consumes the array
backwards; members of one multi-slot Virtual-stock block remain ascending in
the stored array, so reverse submission still draws slot 0 through primary.
Pure-stock, pure-FreeType, and unequal-depth permutations remain exactly those
produced by the retail quicksort. This removes the normal path's post-sort list
snapshot, pointer hash construction, sorted-to-registration reconstruction,
and per-run `std::sort`.

The sort hook now separates the retail-sort proof from the stricter FreeType
topology proof. Once the stock predecessor, source list, finite depths, storage,
and item count prove that the decision-equivalent quicksort is safe, its exact
registration-ordinal sidecar remains authoritative even if the later topology
proof fails. For ordinary facades and true Virtual-stock singletons, a
validation pass first proves that sidecar to be a permutation and identifies
only changed exact-equal-depth mixed runs; a second pass commits those compact
runs in original painter order. This path does not call the predecessor again
and does not build the compatibility source snapshot, pointer hash, full block
table, or sorted-to-registration reconstruction. The two-pass commit leaves the
stock-equivalent array untouched if any validation fails.

A real multi-slot Virtual-stock group is deliberately excluded from this
single-block sidecar path. It retains the older conservative post-sort repair,
including its primary/follower contiguous-block proof. Only failures of the
retail-sort envelope itself (non-stock predecessor, non-interface sort, invalid
source list, non-finite depth, item/storage limit) call the EDX predecessor.
An uncertain topology can therefore never be promoted into a guessed block or
anchored frame.

The initialization line reports `sortAnchorHook=1` when the guarded call-site
patch is current. The aggregate `tnvse_freetype_sort` line reports
`original_anchor_sorts`, `anchor_items`, changed `anchor_mixed_runs`, and
`anchor_fallbacks` (split into non-stock predecessor and proof failures);
`mixed_equal_depth_runs_restored`, `items_restored`, and `restore_rejected`
then describe only compatibility-path work. `tnvse_freetype_sort_proof` reports
stock-equivalent sorts/items, ordinal-sidecar recoveries and changed runs,
multi-slot legacy handoffs, and the exact failed proof stage: gate, count,
storage, source, depth, metadata, registration, group, singleton, coverage, or
final apply.

Before preflight/upload work, each complete compatibility facade or
virtual-stock group receives a read-only state test for exact zero
Tile/material alpha under a provably no-op alpha blend. A sorted facade rejected
at this stage does not enter shader/page preflight or static/dynamic VB
preparation. Its Tile callback re-evaluates the condition before suppressing the
submission, so a same-frame alpha change cannot lose text. This early test does
not guess clip or renderer state: those values are not authoritative for a
particular Tile during sorted preflight.

Long clipped lists also have a conservative subtree gate before scene traversal
can register any of the row's geometry. It is installed only on an exact retail
`NiNode` produced for a `TileRect`, `TileImage`, or `TileText` that already has a
nonnegative integral `listindex`, enabled `clips`, and a finite ancestor
`clipwindow`. An existing executable MakeNode hook is retained as the
predecessor. An unreadable or non-executable entry, failed write verification,
custom output-node vtable, or missing reciprocal `Tileptr` extra data leaves the
candidate node unmodified.

The hot visible-row path re-reads the live traits and compares only the row's
finite positive vertical interval with its current clipwindow, expanded by 96
UI units. An overlapping interval returns immediately to stock and does not
walk the subtree. Only a root already outside that guard band starts the deep
proof. The exact integer list index remains the installation classifier; on an
already-proxied node the live proof accepts any finite nonnegative value because
the index is not a geometric input. The deep proof is bounded to 32 levels and
256 Tiles and still requires an unrotated identity-zoom chain through the
clipwindow, finite nonnegative heights and absolute positions, reciprocal
Tile/node identities, intact Tile parent links, and every Tile-owned live child
node to remain below the candidate scene node. A child with a scene node must
have a supported drawable Tile type; an unfamiliar logical container with no
scene node may contribute its completely proved children but cannot contribute
an assumed bound. App-culled branches contribute no bound. The root interval
and identity already established before recursion seed the deep union, so the
proof does not repeat root trait, Tileptr, or scene-parent validation. The
complete union must remain outside the padded clip interval; limits,
transforms, malformed links, missing bounds, or an injected Tile that extends
back toward the window all fail open. No visibility result or Tile pointer is
cached across frames.

An exact root `NiNode::GetAppCulled()` bit returns before the same stock
`OnVisible` traversal that would honor it; `app_culled` reports this separate
engine-authored case. No Tile trait is interpreted as an equivalent app-cull.

A proved miss returns before the stock `NiNode::OnVisible`, so the complete row
avoids culling traversal, `RegisterObject`, accumulator insertion/sorting,
FreeType preflight, and command construction. A kept row calls the saved
predecessor unchanged. This gate neither inserts nor reorders geometry, so the
original-order anchor and its conservative compatibility fallback remain the
only paths that alter equal-depth sort output for MapMO/UIO and other injected
elements.

The final per-facade boundary remains independent and exact. The symbolized test
build shows `Tile::UpdateClipwindows` propagating the `clipwindow` relationship
and `Tile::ReClipChildren` resolving it, together with each child's `clips`
trait, into the live `TileShaderProperty` scissor. Retail PC
`TileShader::SetupGeometryConstants` at `0xBCA980` consumes that property,
publishes the current model transform, and installs the hardware scissor;
`TileShader::PostGeometry` at `0xBCAC60` restores scissor/stencil state. The
native path never uses the independent `BSScissorTriShape` tail as a substitute
for resolved Tile state.

The retail matrix path is reproduced rather than inferred from the renderer's
previous world matrix. `0xBCA980` calls `0xE6FBB0` with
`currentPass->pGeometry->m_kWorld`, and `0xE6FBB0` calls
`NiD3DUtility::GetD3DFromNi` at `0xB71A40`. For a Ni rotation `R`, uniform scale
`s`, translation `t`, and renderer position adjustment `p`, the PC D3D matrix
written by that function is

```text
W = [ sR00 sR10 sR20 0 ]
    [ sR01 sR11 sR21 0 ]
    [ sR02 sR12 sR22 0 ]
    [ tx-px ty-py tz-pz 1 ]
```

The symbolized Xenon test build independently names
`TileShader::SetupGeometryConstants` at `0x82250E58`, its
`NiXenonRenderer::SetModelTransform` call at `0x8224ADF8`, and
`NiD3DUtility::GetD3DFromNi` at `0x8224AE08`, including the same uniform-scale
and `NiRenderer::PosAdjust` contract. Tile's vertex constant map requests
`WorldViewProjTranspose`; retail `NiD3DShaderConstantMap` at `0xE85D10`
constructs `(W * View) * Projection` with two `D3DXMatrixMultiply` calls and
then transposes it. All 16 decompiled shader packages contain the same three
Tile position programs (`TILE1000/1001/1002`): VS c0-c3 are four `dp4`
operations against the input position. The pre-slot proof uses the same world
layout, the same two D3DX calls and association order, and the untransposed
matrix columns that those four shader rows represent.

Around a guarded native pass, tNVSE arms a thread-local visibility scope. A
fully classified Standard-lite pass first proves exact current-pass geometry,
property, payload, renderer, and retail slot-31 identity. It can then test the
immutable whole-text bound against the already-resolved live scissor before
slot 31. The sphere bound is expanded to an enclosing cube and evaluated with
four homogeneous half-space intervals; a pass is suppressed only when the
complete cube is strictly outside one edge of a two-pixel-expanded scissor.
Term-magnitude-relative slack is applied before cancellation, and every point
of the cube must be strictly in front of `w=0`. A successful proof is consumed
through the ordinary immediate validation hook, but slots 30-35, geometry
preparation, the optional non-first-pass slot 68, and the driver draw are all
skipped. Because slot 31 never pushed scissor/stencil state, slot 35 is
intentionally not called.

Other guarded routes, and Standard-lite calls rejected before exact pre-slot
input evaluation, retain the original fallback: the exact retail slot 31 runs,
the scope verifies the published renderer world matrix and pass-local
identities, only the immediate driver draw is skipped, and slot 35 restores the
stock scissor/stencil stack. Once Standard-lite has evaluated the exact same
matrix/scissor inputs and kept the draw, the redundant post-slot calculation is
elided; slot 31 does not mutate any of those inputs.

Every ambiguous case fails open: disabled or malformed scissor, non-finite
bound/transform/position-adjust/matrix, a cube touching or crossing `w=0`,
viewport mismatch, the retail special scissor-scaling mode, a replaced slot 31,
pass/property/payload/renderer/device identity mismatch, or an edge within the
numeric safety slack all keep the original path. Command-buffer execution
treats either kind of proved miss as a successful consumed command, and a
Virtual-stock singleton marks its frame `Culled` rather than reporting a
resource fault. A non-fused multi-slot group tests each physical slot
independently. Its fused retained span deliberately fails open because separate
stock shells can carry independently live Tile properties; one shell's scissor
is not accepted as proof for every follower.

The periodic performance line reports `visibility_checks`, `culled`, `alpha`,
`scissor`, `scissor_pre31`, `scissor_post31`, `preflight_skipped`,
`packets_saved`, and `vertices_saved`. `scissor` is the total and the two phase
counters partition successfully consumed scissor culls. Alpha culls can
increase `preflight_skipped`. Both scissor paths occur after preflight, but the
pre-slot path additionally avoids all six Tile callbacks and geometry setup;
it also avoids optional slot 68 on a non-first pass. The post-slot fallback
saves only the driver submission. `app` and `clip`
remain reserved fail-open compatibility counters. In a clipped Tweak/list
menu, the desired result is nonzero `scissor_pre31`, a reduced
`stock_constant_updates`, and corresponding saved packets/vertices.
`scissor_post31` shows safe fallback coverage. `scissor=0` means neither final
conservative proof fired, not that the `clips`/`clipwindow` traits were absent.
The separate `tnvse_freetype_viewport_cull` line reports installed `nodes`,
`install_failed`, total `checks`, cheap `fast_visible` returns, `deep_checks`,
the number of `deep_tiles` inspected, proved `culled` subtrees, exact
`app_culled`, and `fail_open`. Failure leaves split list-index, clips,
clipwindow, root-bounds, transform, node-identity, subtree-topology, and
subtree-bounds uncertainty;
`deep_overlap` counts complete proofs whose descendant union reaches the padded
window and therefore stays on the stock path. For useful list culling, `culled`
should be substantial while `deep_tiles / culled` remains small; the split
failures identify which compatibility structures deliberately retain stock
without weakening another proof gate.

The dynamic ring retains its two-maximum-payload capacity, while the static VB
starts at approximately 4 MiB instead of reserving its approximately 12 MiB
packed-format maximum. When it is full, a safe submission boundary with no
other active proxy compacts live weakly owned artifacts into a replacement
buffer and doubles capacity up to that fixed maximum. If the sorted-call hook is
unavailable or replaced, the existing per-shape promotion route remains active.
Concurrent groups defer optional promotion and continue through the dynamic
ring; expired static entries are discarded during compaction. The map published
after a sorted frame's batch upload is retained only until its packets finish,
because those packet submissions need stable vertex ranges.

When a sorted FreeType facade has exactly one active packet, it can skip the
packet proxy entirely. The command-backed route accepts an arbitrary sealed
packet vertex subrange and any referenced physical atlas page; it no longer
requires page zero or a packet covering the complete immutable payload. For
the duration of that one stock Tile pass, the facade's existing geometry-buffer
descriptor is rebound directly to that packet's static/dynamic VB range,
canonical INDEX16 buffer, native declaration, and resolved shader. The facade
itself remains `RenderPass::pGeometry`, so the retail
`TileShader::UpdateConstants` reads its live scissor, overlay color, Tile and
material alpha, blend, cull, and stencil state without a proxy-state copy.
The payload origin is temporarily folded into the facade transform. If the
packet references another physical page, the retained atlas texturing property
and Tile source texture are installed for the pass. The original property,
source texture, model bound, alpha-test flag, shader, transforms, and every
modified buffer/chip field are held by RAII and restored synchronously after
`RenderImmediate`.

This direct-shape route is published only inside the validated sorted-frame
ring lease. It requires exact generation, resource serial/upload epoch, a
prepared geometry buffer, a valid retained atlas property/texture, and a
four-vertex-aligned packet range. Without a valid command, the older
mutation-free page-zero/full-range eligibility remains the fallback fast path.
A failed check occurs before the retail pass and falls back to the unchanged
packet/proxy path; once the retail pass has started it is never replayed.
Device reset and shader-generation replacement therefore invalidate it through
the existing lease rules. It creates no per-facade D3D allocation, changes no
atlas or cache format, and does not enable NPOT textures. The periodic
performance lines report `direct_shape_candidates`, `direct_shape_draws`,
`direct_shape_vertices`, `direct_shape_fallback`, and command-backed
`direct_range_replays`; the last counter advances only when page/range
eligibility is broader than the legacy page-zero/full-payload case.
Each direct fallback now also increments exactly one first-failure stage:
`command`, `submission`, `binding_input`, `binding_topology`, `binding_atlas`,
`binding_facade`, `binding_property`, `binding_texture`, `binding_shader`, or
`runtime`. The dedicated `direct_fallback_total` line reports `classified` as
the sum of those stages; the two values must match for every reporting window.
`binding_facade` remains a compatibility aggregate and is split into the
mutually exclusive `facade_model_data`, `facade_alpha_property`,
`facade_buffer_data`, `facade_tile_property`, `facade_stream_count`,
`facade_vertex_stride`, `facade_vertex_chip_array`, and `facade_vertex_chip`
leaves. `facade_classified` is their sum and must equal `binding_facade`;
`classified` counts the leaves instead of the aggregate so facade failures are
not double-counted.
When a stock facade has no `NiGeometryBufferData`, direct replay now installs a
stack-local, non-owning descriptor for the synchronous retail
`NiTriShape::RenderImmediate` call. It borrows the command VB, IB, declaration,
stride, and chip view, restores the original null descriptor immediately after
the pass, and clears all borrowed fields before invoking the retail
non-deleting destructor. This path performs no per-draw heap allocation and is
reported by `synthetic_buffers`.

### Virtual-stock FreeType shapes

The virtual-stock backend freezes the ordinary or Composite packet topology
when the text shape is created. Composite is selected only when every required
profile resolves in the current shader generation; otherwise the ordinary
topology is frozen. A one-packet artifact returns one real shape.
For a multi-packet artifact, TileText capture and RichText rendering expose the
destination `NiNode`; tNVSE returns the last packet as the ABI primary and
inserts the remaining shapes immediately after its actual child index in
reverse packet order. This avoids first-available child holes and keeps
multiple captured text groups contiguous without reordering unrelated children.
Every shape has the complete text bound and the same sort state. The retail
sorted Tile routine at `0xB64F90` traverses its already prepared item array
from the last entry to the first, so a contiguous group is
drawn as packet 0 through packet N without changing primitive order. Unknown
callers, more than 64 packets, a missing parent, insufficient CPU budget, or a
different topology selected by shader preflight retain the one-facade route.
There is no global `NiNode::AttachChild` hook.

A one-packet artifact uses the dedicated `VirtualStockSingleton` backend. Its
derived metadata and backend state share one `make_shared` allocation and the
sole slot, registration proof, prepared binding, and command-build record are
embedded values. It creates no `VirtualStockShapeGroup`, slot or command
vector, registration-index array, group-registry node, weak group owner, or
group mutex. Creation also bypasses the temporary packet-shape vector. The
ordinary metadata registry remains the lifetime authority for the real shape.

For multi-packet artifacts, the group primary owns the immutable payload,
atlas references, and complete
facade fallback. Shape metadata keeps a raw group identity plus a weak owner;
locking that owner replaces a global metadata-map lock and lookup while still
rejecting an expired, unpublished, or pointer-reused group. Followers hold no
strong group ownership. Before registration the primary copies its current
transform, object flags, hardware scissor, overlay RGB, Tile and material
alpha, and blend/cull/stencil properties and full world bound to the
page-specific siblings, then performs the
conservative whole-group visibility test once. All slots must be registered
into the same accumulator in descending slot order at consecutive final item
positions. The per-shape registration hook records submission order only:
stock's Tiles registration function may insert at a sorted position rather
than append after the previous item. Immediately before the stock sorted
traversal, tNVSE therefore scans the completed accumulator once with a
pointer-only lookup and resolves every virtual slot's unique final position. A
missing, duplicate, reordered, or non-contiguous slot prevents direct
publication: only the primary executes the compatibility packet set and every
follower is skipped. This final-layout scan does not build facade frame
entries, acquire per-item metadata ownership, or run compatibility preflight
for frames whose tracked FreeType objects are all virtual-stock primaries.
The singleton registration route is render-thread serialized and records the
stock registration result directly. It rejects a second registration in the
same accumulator cycle, so it needs neither the group lock nor the completed
accumulator slot lookup/layout scan. The sorted traversal retains the existing
metadata owner, validates the exact shape identity and registration cycle, and
publishes the singleton only after its payload and sole slot are ready.

Each direct slot owns a small `NiGeometryBufferData`/`NiVBChip` descriptor but
does not own its D3D buffers. The descriptor points at
`payloadBaseVertex + packet.firstVertex` in either the shared immutable static
arena or the traversal-sealed dynamic arena, plus the canonical INDEX16 buffer.
Virtual-stock now reuses the same frame-packet residency resolver as retained
commands instead of rejecting an otherwise valid dynamic upload as
`static_not_ready`. A dynamic binding records the sorted lease upload epoch and
is valid only while that lease prevents ring overwrite or resource release.
The sorted lease resolves the singleton directly or resolves every multi-slot
group entry first, and publishes the backend only after
all textures, shaders, generations, atlas epochs, item positions, residency
kinds, upload epochs, and ranges agree. During its Tile callback the
shape changes only its packet bound, folds the artifact origin into its live
transform, and temporarily disables alpha test; descriptor, declaration,
shader, texture, scissor, color, alpha, blend, cull, and stencil state remain
the real shape's state. The normal pixel shaders and pass-local constant
ownership remain unchanged. A stable slot is fully validated and reused
without rewriting its descriptor; only an actual resource, atlas, shader, or
descriptor change performs a rebind. Atlas validation includes both the
texturing-property wrapper and the Tile shade property's page-specific source
texture.
Multi-slot binding resolution uses per-thread fixed-capacity scratch whose used entries
are overwritten before consumption, avoiding zero-initialization of 64 packet
bindings for each group. A stable descriptor is checked once during sorted
preparation; a changed descriptor is checked after publication. The direct
draw path retains the primary payload owner only for follower slots and reads
the immutable packet template in place, avoiding a shared-owner increment and
packet copy for the usual primary draw. The singleton reads its embedded slot
and metadata-owned payload directly. Periodic counters distinguish
`virtual_stock_static_hits` from `virtual_stock_dynamic_hits`.

Static/dynamic-buffer replacement, dynamic upload-epoch change, device reset,
shader reload, and atlas epoch changes revoke every borrowed descriptor before
the underlying resource can be overwritten or released. Shape destruction
restores the original shell buffer and shader before deleting the plugin-owned
descriptor. A primary destroyed early retires the group so surviving followers
cannot submit it. Singleton destruction retires its embedded state without a
group-map removal. A virtual backend failure falls back or suppresses that group
and does not fault the complete FreeType shader generation; failure to restore
shared shader constants retains the existing generation-fault rule.
If validation fails before any packet in the group has drawn, the descriptors
are restored and the primary executes the complete facade in that same
traversal. Once any packet has reached `RenderImmediate`, the group is never
replayed because doing so could duplicate already drawn layers.

This backend removes facade expansion, per-packet proxy selection, repeated
descriptor replacement, and unnecessary full-list preflight work. It does not
merge packets or facades: one packet still produces one
`DrawIndexedPrimitive`. It adds no shader file, cache-format field, INI option,
NPOT texture, or external API. The diagnostic line beginning
`tnvse_freetype_perf: virtual_stock_` reports candidates, singletons, groups, shapes,
draws, static hits, rebinds, revokes, facade fallbacks, skipped followers,
saved preflight/proxy work, and categorized fallback reasons.

### Retained text command buffer

`bEnableFreeTypeFontCommandBuffer` is the master switch for the production
command path used by FreeType A8 shapes in the validated sorted Tile traversal.
It is independent of `bEnableFreeTypeFontAggressivePerformanceMode` and
`bEnableFreeTypeFontCompositePass`; the active ordinary or Composite packet
topology is compiled separately. The default is `0`.

- `0` retains the current submission path.
- `1` enables command recording, retained bridge submission, and guarded
  native replay as one unit. One reverse-verified
  `BSBatchRenderer::RenderPassImmediately_Standard` bootstrap supplies
  live Tile WVP, color/alpha, scissor, viewport, render-target state, and the
  paired slot-35 cleanup for an eligible logical span. Remaining packets keep
  their original order and use tNVSE's generation-bound binder for shader,
  atlas page, packet constants, pass state, and draw range.

The former `uiFreeTypeFontCommandBufferMode` option is deprecated. When the new
switch is absent, any nonzero legacy mode enables the complete command path for
that run and logs a migration notice. Once the new switch is present, it is
authoritative. The retired shadow-only stage and its D3D `Get*` captures are not
part of the production path.

Guarded replay publishes the retail current-pass globals, invokes `B99390`
when the selected TileShader/pass must change, applies the special alpha-test
state used by `BSBatchRenderer::RenderPassImmediately`, and enters
`BSBatchRenderer::RenderPassImmediately_Standard` only when every
reverse-verified default-path predicate agrees.
`BSBatchRenderer::RenderPassImmediately_Skinned` multi-pass,
skin/light/special passes,
unknown shader or geometry vtables, and forced shader-selection passes retain
the stock path. A stock `TileShader::UpdateConstants` call is still required
once per span.

Ordinary dedicated single-packet commands additionally use a staged
`RenderPassImmediately_Standard-lite` specialization. Stage 1 proves the same
default-pass envelope
without executing `E72C20` or the particle/line virtual predicates: formal
`E72C20` and the symbolized test build both show that an already resident
`m_pkBuffData` makes the former return false immediately, while the exact
tNVSE-owned `NiTriShape` vtable proves the latter two stock null-casts. Full
preflight compiles those immutable facts into a
`NativeA8StandardPassLiteDispatch` owned by `NativeA8TileRetainedText`. It
retains the Tile/property identity, renderer, shader, generation-owned program,
and resolved slot table for the Tile lifetime; frame commands carry only a
non-owning pointer to it. Stage 1 therefore checks the live `RenderPass`
envelope plus that retained identity instead of reconstructing a local dispatch
and rereading the shader vtable, renderer/device, model data, and hook vtable on
every submission. Stage 2 accepts the current buffer already proved by direct
or Virtual-stock binding; only an unproven compatibility caller performs the
full VB/IB/declaration/range comparison. Stage 3 mirrors the confirmed standard order:
publish renderer property/effect state; invoke slots 30, 31, conditional
32/33/68, optional 34, slot 27, `RenderImmediateAlt`, and slot 35. It omits only
the geometry-group helper whose resident-buffer branch has no side effect.
Failure in stages 1 or 2 re-enters the complete guarded decision, selecting
full `BSBatchRenderer::RenderPassImmediately_Standard` when it still qualifies
and otherwise the unchanged `BSBatchRenderer::RenderPassImmediately`; a
prelude failure also selects `BSBatchRenderer::RenderPassImmediately`. No
fallback is attempted after an immediate draw.

Dedicated one-packet Standard-lite replays also use
`BSBatchRenderer::RenderPassImmediately_Standard` v2, a traversal-local
slot-delta executor shared across adjacent Tiles in the same validated command
execution segment. Retail PC and the symbolized test build agree on the
relevant slot effects: Tile slot 30 publishes programs, the declaration,
texture stages, and effective clamp mode; slot 31 publishes live
transform/color constants plus optional scissor/stencil state; slot 32
publishes blend enable/function; slot 33 publishes alpha-test
function/reference; slot 34 publishes cull mode and alpha-test enable; and slot
35 restores only the scissor and stencil enables established by slot 31. The
formal implementations at `BCA760`, `BCA980`, `BE1FF0`, `BE20B0`, `BE20E0`,
and `BCAC60` establish those disjoint output categories.

Before Standard-lite slot 30, the reconstructed world-to-scissor proof may
consume the complete pass. This early exit calls neither slot 31 nor slot 35,
so it cannot unbalance the retail scissor/stencil restore stack. If its strict
identity or matrix-construction gate fails, the exact slot-31 path remains
unchanged; the post-slot fallback can still suppress the immediate driver draw
and then lets slot 35 perform the required restore. A completed pre-slot test
that cannot prove the cube outside simply keeps the draw and is not repeated.

The cache therefore tracks each slot independently. A texture-page change can
require slot 30 without forcing identical blend and drawmode callbacks to run;
an alpha/fade change can require slot 32 without republishing programs or
textures. Keys describe effective output rather than raw input identity:
blend alpha values collapse to the exact enable/function tuple, alpha-test
flags collapse to function/reference, and stencil/alpha flags collapse to
cull mode plus alpha-test enable. Tile fields not read by the corresponding
retail function are excluded. Slot 30 reuse is restricted to ordinary
one-source, no-alpha-texture native atlas commands; each later slot retains its
own applicability rules.

V2 additionally caches slot 31 only for the strict non-transient subset: the
program, world transform, renderer view/projection and camera inputs, Tile
overlay/tile alpha/texture transform, material alpha, depth range, render
target, and viewport must all match, and neither Tile scissor nor enabled
stencil may be present. That rule preserves both low constant-register output
and `SetModelTransform`'s renderer mirrors. A transient packet still executes
the exact stock slot-31/slot-35 pair; v2 does not nest or retain the stock
scissor/stencil restore stack. For a verified retail slot 35 with no transient
state, the callback is known to be a no-op and is omitted. Slot 27, the draw,
and any required cleanup remain mandatory.

Each generation records a retained identity proof for slots 30 through 35.
Delta scheduling is enabled only when all six callbacks have
reverse-verified implementations and an exact effective-state key. This
includes retail and the separately classified Alpha Fixes 2.52 blend callback.
If another plugin replaces any slot with an unknown implementation,
Standard-lite returns to the complete stock pass before drawing and does not
carry its unknown effects into the next Tile.

The cache stamp contains the command validation token, renderer/device
identity and shader generation, atlas/resource/upload epochs, render-target
group, viewport, and the command segment plus external-mutation epochs. A
non-FreeType item, nested traversal, stock/full-standard fallback, shader or
ring mutation, device reset, render-target/viewport transition, runtime fault,
or a new sorted traversal starts a new cache head. `B99390` shader/pass
selection also starts a new head because its teardown/setup callbacks are
outside the cached slots and may republish their states. The PC-only
pre-standard `B98540` call remains mandatory on every applicable pass, but it
does not invalidate these proofs: the formal build shows that it publishes
only the vendor alpha-to-coverage extension through render state 154 with
`A2M0`/`A2M1`, or state 181 with zero/`ATOC`. It does not touch the
alpha-test function/reference states owned by slot 33, or the cull and
alpha-test-enable states owned by slot 34. This cache never retains a Tile or
COM object and never allows device-state reuse across a command-validation
boundary.

The immutable Text Artifact retains shared packet geometry, profile
hashes/classes, atlas-page topology, and vertex ranges, but no resolved Tile
program. `NativeA8TileRetainedText` is instead owned by the
`A8ShapeMetadata::nativePayload` associated with the live Tile facade. Full
preflight builds its packet and run skeleton only when the Tile's packet
topology, sampling/alpha class, or shader program changes. Atlas/resource-only
preflight changes refresh the validity stamp while retaining the same skeleton.
The custom `NiTriShape::DeleteThis` route invalidates that metadata before the
stock Tile geometry is destroyed, so retained text cannot remain usable after
its Tile lifetime ends.

Every published A8 metadata object carries a monotonic allocation ID, its own
address, and its owning shape address. The shape registry stores a second
publication-time copy outside the `shared_ptr`. Healthy allocation and deletion
remain silent; the identity fields are retained for correlation with targeted
diagnostics and crash analysis. Destruction emits
`metadata-delete-integrity-failure` only when registry identity, pointer,
allocation ID, self pointer, or shape identity validation fails. Integrity
failures are logged even when verbose rendering logs are disabled (capped at 64
entries); unsafe retained/Virtual-stock cleanup is then skipped so the audit
itself does not dereference a mismatched object.

Each generation-owned shader profile owns one immutable compiled packet
program containing its already resolved shader, VS/PS handles, slot methods,
constants profile, and replay flags. Tile-retained text stores non-owning
packet/program views and static vertex/page offsets, but no transform,
scissor, material state, render target, viewport, VB/IB/declaration, texture
COM ownership, or other mutable frame state. A later traversal therefore
materializes only current sealed residency and atlas texture views instead of
recovering the profile, validating all immutable packet fields, and rebuilding
run topology for every frame. The retained Standard-lite dispatch follows the
same rule: its buffer argument is injected from the traversal-local, already
validated residency view, so a stack-local synthetic compatibility buffer can
never escape into Tile-retained data. Atlas/resource-only preflight refreshes
reuse the dispatch; Tile destruction, topology/program replacement, shader
generation changes, or hard retained invalidation clear or replace it.

After registration, preflight, static/dynamic VB residency, and Virtual-stock
topology are frozen, each sorted traversal builds a temporary command table
containing only validated non-owning views plus frame-local buffer ranges. It
is cleared before the ring lease ends. Command vectors, per-shape
program-pointer arrays, and Tile-retained packet/run capacity participate in
`RuntimeMetadata` accounting. Frame storage retains at most 16384 command
slots and 8192 run/span slots between traversals and releases all retained
capacity when the aggregate CPU budget remains exceeded; Tile-retained
capacity is released with its owning Tile metadata.

Compatibility facades retain their original position relative to non-FreeType
items. A Virtual-stock group can fuse only when every slot is registered
exactly once at consecutive final accumulator positions and each adjacent
packet has byte-identical live Tile, transform, scissor, alpha, blend, cull,
and stencil state. Traversal reaches slot 0 first; that leader executes the
span, while followers consume only a token- and geometry-validated skip marker.
A one-packet facade or dedicated Virtual-stock singleton still performs one
stock bootstrap through its direct command lookup. The singleton command keeps
the metadata identity and points at the embedded prepared draw; it does not
retain a group identity or allocate span/run topology.

Full validation is owned by a contiguous FreeType execution segment rather than
by each logical text span. The first command after traversal activation, a
non-FreeType Tile, nested traversal, or explicit native-state invalidation
checks the sorted validation token, nesting serial, all three hook identities,
renderer/device and shader generation, atlas epoch, the sealed ring resource
serial/upload epoch, render-target identity, and viewport identity. Adjacent
FreeType spans reuse that result while both the local segment epoch and the
cross-thread external-mutation epoch remain unchanged. Any external mutation
after command compilation invalidates the remaining command table for that
traversal; it is never accepted merely by opening a later segment.

The command path obtains render-target-group and viewport identity from
`NiDX9Renderer`'s software mirrors, so it does not use
`IDirect3DDevice9::GetRenderTarget` or `GetViewport`. Shader constants use a
pass-ownership contract rather than a device snapshot. The symbolized test
build shows that `BSBatchRenderer::RenderPassImmediately_Standard` invokes
`SetupGeometryConstants` before `PrepareGeometryForRendering` and the geometry
draw, while `NiD3DShaderConstantMap::SetShaderConstants` submits every live
reflected entry. Both the official executable and the symbolized test build
show that `TileShader::CreateConstantMaps` binds `tintcolor` to PS c0,
`WorldViewProjTranspose` to VS c0-c3, and `TexScroll` to VS c4. A scan of every
decompiled shipped shader package finds that reflected constant tables,
including relatively indexed arrays, end at PS c24 and VS c120; shader-local
pixel `def` literals extend only to c30. Native A8 consequently leaves all
stock registers intact. Its immutable packet block occupies the middle-high
reserved PS c176-c183 band and its analytic-AA input occupies VS c208. This
leaves 40 pixel and 47 vertex float registers above tNVSE's highest register,
avoiding both the audited low/middle footprint and the SM3 register-file edge.
PS c0 remains the live stock Tile color and VS c0-c4 remains entirely
stock-owned. No D3D
`GetPixelShaderConstantF` or
`GetVertexShaderConstantF` snapshot is taken and no stale snapshot is written
back. Initialization also rejects a device whose advertised vertex constant
count cannot address c208. Device-loss and native submission failures still
follow the existing runtime-fault handling.

The separation also removes a formerly unavoidable-looking per-packet write.
Both reverse targets show that the stock slot-31 call computes live Tile RGB
and alpha and then submits the PS constant map whose `tintcolor` entry is c0.
The native wrapper therefore does not reconstruct that value and call
`SetPixelShaderConstantF(0, ..., 1)` a second time when the preserved slot is
the verified retail `TileShader::SetupGeometryConstants`. This saves one D3D
constant publication for every stock bootstrap or ordinary native packet,
including the simple Coverage/ARGB path. If another plugin replaces slot 31,
the proof no longer applies and the wrapper retains the explicit c0
publication as a compatibility path.

The Standard-v2 state cache also treats slot 31's constant prefix separately
from its transient suffix. The official PC implementation at `0xBCA980` and
the symbolized test implementation of `TileShader::SetupGeometryConstants`
agree that model/constant publication finishes before scissor and stencil are
enabled; both `PostGeometry` implementations restore only those two transient
states. Once an execution segment proves the complete transform, camera,
Tile/material alpha, texture-transform, and native-program key unchanged,
`NativeTileConstantsLite` skips the constant-map prefix and replays only the
same retail render-state entry points before the draw. Slot 35 still runs and
remains exactly paired. Resolution-scaled scissor is deliberately outside this
proof and falls back to the complete stock slot 31/35 pair.

A second specialization admits a world-translation-only delta after proving
the program, rotation, scale, view/projection and camera state, Tile/material
color and alpha, texture transform, depth inputs, device, render target, and
mutation epochs unchanged. Formal PC `BCA980` calls `E6FBB0`, whose render-state
callback depends only on scale and whose model-camera updates depend only on
rotation and scale. The symbolized test implementation independently shows
`SetModelTransform` publishing the same D3D world matrix before the same two
constant maps. The translation path therefore rebuilds the complete retail
world mirror, preserves the exact `(W * View) * Projection` association and
transpose, and directly publishes only `WorldViewProjTranspose` at VS c0-c3.
It leaves the proved-resident TexScroll c4, pixel tint c0, model-camera vectors,
normal-normalization state, and private native registers untouched. Optional
scissor/stencil state is then installed through the same suffix helpers and
remains paired with slot 35. Non-finite input, device failure, unknown identity,
or resolution-scaled scissor conservatively re-enters the complete slot 31/35
path before drawing.

The high-register ABI was also checked against common shader plugins.
NewVegas Reloaded's explicit New Vegas ranges end at c145, its D3D9 device
proxy forwards constant writes unchanged, and a separately compiled
high-pressure `WetWorld` pixel entry uses external constants only through c79
and shader-local literals through c103. Its explicit New Vegas vertex ranges
end at c145. The same source tree's Oblivion-only grass path occupies VS
c20-c252 and would overlap c208, but that package is not selected by the New
Vegas build; the compatibility claim is deliberately scoped to the supplied
New Vegas runtime. Fallout Dynamic Reflections adds only PS c1/c27 and c3/c11
variants to 3D lighting passes. FNV Depth Resolve's replacement DOF shaders
use VS and PS c0-c2 and hook the main 3D accumulator rather than the sorted
Tile traversal. Those New Vegas plugin paths therefore do not overlap PS
c176-c183 or VS c208. Their real rendering occurs outside a native sorted
execution segment; every traversal start clears the local constant shadow,
while nested, reset, device/generation, and unknown external transitions
invalidate it. A verified stock Tile transition still terminates retained
command and sampler/pass-state reuse, but preserves the private-register shadow:
the reverse-confirmed stock maps cannot write PS c176-c183 or VS c208. The
cached viewport width/height is checked at that boundary and c208 alone is
invalidated if its analytic-AA dimensions changed.

Retained packet binding also treats
`TileShader::SetupGeometryTextures` as the owner of VS/PS publication, as
confirmed by the symbolized test build, and does not submit the same VS/PS a
second time; `NiD3DRenderState`'s current-program mirror validates that setup
without a driver query. Same-profile retained packets now consult that mirror
directly and skip both setup callbacks when the two program handles remain
current; a mismatch repairs the setup instead of trusting the local profile
pointer.

Stage-zero texture identity is read directly from
`NiD3DRenderState::m_apkTextureStageTextures`, which retail
`NiDX9RenderState::SetTexture` updates before its driver call. An exact mirror
match is therefore a complete zero-driver-query reuse proof even when a stock
bootstrap did not prime the traversal-local bit. Only a real page change enters
the engine setter.

Retail `NiDX9RenderState::SetSamplerState` maps only `ADDRESSU`, `ADDRESSV`,
`MAGFILTER`, `MINFILTER`, and `MIPFILTER` into its five software slots;
`SRGBTEXTURE` is not tracked and passing it through the wrapper is a no-op. The
native Tile path never publishes that state and D3D9 initializes it to false.
The level-zero contract consequently checks the mirrored `MIPFILTER` slot
directly and calls the engine setter only when it is not already
`D3DTEXF_NONE`. A stock setup preserves sampler readiness when its resulting
mirror is already correct. A same-profile command whose program, packet
constant, vertex-AA, and sampler mirrors are all current bypasses the three
binder helpers entirely while recording their reuse counters. Packet
c176-c183 uploads compare the
traversal-local constant shadow and submit only the changed contiguous register
range. That shadow now retains the process-lifetime immutable
`NativeShaderProfile` identity directly. It no longer copies the 32-float
packet block into both sorted and facade TLS on every packet; pointer identity
proves an exact reuse, while a profile transition compares against the previous
immutable profile only once to find the minimal changed range. The profile also
retains its shader-visible prefix length: Body uploads c176-c177, Effect uploads
c176-c179, and only Composite uploads the complete c176-c183 block. The shadow
tracks how much of the previous immutable profile is actually resident, so an
unused tail is never uploaded but can never be mistaken for valid state on a
later wider profile. The original
Tile vertex constant maps are no longer specialized or mutated:
`TileShader::UpdateConstants` continues to publish both
`WorldViewProjTranspose` at c0-c3 and `TexScroll` at c4, while the native vertex
shader reads its AA profile only from c208. Consequently c208 can be reused
after stock updates whenever the sorted/facade mirror still proves the same
device, generation, viewport, and raster scale; external shaders, nested
traversal, reset, generation changes, and explicit state invalidation still
force a new publication. Ordinary interleaved stock Tiles are the narrow
exception described above and can now carry the disjoint private state across
the command-segment boundary.

Packet admission now produces a short-lived binding proof before entering the
immediate callback. A direct facade proof comes from the binding scope that has
just installed the command's exact descriptor; an ordinary retained proof comes
from `PrepareNativeA8RingPacket`; and a Virtual-stock singleton reuses the
complete live slot/buffer/atlas check already required by its direct route.
Fused Virtual-stock commands are validated as one contiguous execution range:
the segment/token/renderer guard is paid once, then the exact live geometry
binding is checked once for each distinct slot in a cache-friendly loop.

With that proof, each immediate callback performs only the irreducible
execution-state, renderer/geometry identity, and acquire-load mutation-epoch
guard. It does not reread program/payload/atlas state or the complete
VB/IB/declaration descriptor, and the standard-pass-lite dispatcher does not
repeat the already proven binding comparison. A route without a binding proof
retains the complete packet validator. Device reset, shader publication or
fault, atlas mutation, ring resource replacement/discard, shape destruction,
Virtual-stock binding invalidation, nested traversal, and every transition
through a non-FreeType Tile advance one of the epochs. A mutation during a span
therefore faults its next packet before drawing. A failure before any draw
re-enters the unchanged current path; after any packet reaches the driver, the
span is marked faulted and followers are consumed without replay, preventing
duplicate layers.

The periodic command line reports recorded spans/packets, span hits/misses,
retained bridge draws, guarded native replays, saved stock bootstraps, fused
Virtual-stock spans/followers, arbitrary-range direct replays, legacy
per-span-full/light/render-target validation counts, packet epoch guards,
full packet-state validation elisions, Virtual-stock range validations and
their packet coverage, successful execution segments, segment full
validations/reuses/invalidations, retained-program hits/misses, and fallbacks
by token, generation, atlas, resource, topology, hook, nesting, render target,
and state.
The main performance line reports `constant_ownership_segments`, segment
reuses/releases, and the snapshot Get and restore Set calls elided by pass
ownership. It also reports `private_reuses`,
`stock_c0_republish_elided`, `compat_republishes`,
`private_registers_uploaded`, `full_tail_elided`, and
`stock_tile_private_preserves`. With an unmodified retail
slot 31, `stock_c0_republish_elided` should track
`stock_constant_updates` exactly and `compat_republishes` should remain zero;
any compatibility republish means another component supplied a non-retail
slot-31 implementation. `full_tail_elided` measures the 8-register-block tail
not sent by first/full Body and Effect publications. The command-state line
reports the corresponding `registers_uploaded` and `full_tail_elided` values
for retained binding. The `state_shadow_` line retains the old mirror/driver
constant-capture and `state_shadow_driver_gets` fields so a runtime log proves
that the former path stayed inactive, followed by program/texture/packet and
vertex-AA reuse counters. Program reuse is counted as two avoided
publications, one VS plus one PS. A `vertex_aa_stock_preserved` count records
stock updates that leave the already published native c208 outside their
c0-c4 range. A healthy interval has nonzero, balanced ownership
segments/releases, `snapshot_gets_elided == 2 * constant_ownership_segments`,
`restore_sets_elided == 2 * releases`, and zero
`state_shadow_driver_gets`, driver captures, and `isolation_bypass`.
The timing line adds `command_build` and `command_submit` while preserving
`submit`. Runtime validation should confirm nonzero `native_replays` and
`direct_range_replays`, `span_full_validations=0`,
`render_target_validations` tracking `segment_full_validations` rather than
logical spans or packets, substantial `segment_validation_reuses`,
`packet_epoch_guards` tracking submitted command packets,
`packet_state_elisions` covering all proven direct/ring packets, and
`light_validations` remaining only for exact Virtual-stock range coverage or
unproven compatibility paths. Unexpected fallbacks must remain zero, and
`stock_constant_updates` should not exceed the logical-span count; Standard v2
may reduce it further by exactly `constants_reuses`, without visual or runtime
faults. `constants_lite_replays` is the subset of those reuses that still had
to install scissor or stencil state; it should remain paired with
`post_calls`. `constants_lite_fallbacks` should be zero unless the retained
proof becomes inapplicable, while `constants_lite_scaled_fallbacks` identifies
the intentionally stock-only resolution-scaled scissor case. Build success
alone does not establish runtime correctness or the CPU-performance thresholds.
The following `tnvse_freetype_constants_mismatch` line classifies exactly the
first differing field in the existing short-circuit order for every non-exact
constant-key comparison. Fields after the first mismatch are normally not
evaluated or counted, which keeps the diagnostic to at most one relaxed counter
increment per failed hot-path check. Translation-only world changes are the
deliberate exception: later fields are compared to prove the light path, but
world remains the sole first-mismatch counter.
When `world` is that first mismatch, the adjacent
`tnvse_freetype_constants_world_mismatch` line decomposes it into rotation,
translation, and scale. Seven mutually exclusive bit-mask buckets preserve
which components changed together; their sum is `total`, while the three
component totals may overlap. Classification still performs only one relaxed
counter increment per failed world comparison. `unclassified` is a conservative
representation-change guard and should remain zero.
`tnvse_freetype_constants_translation_lite` reports successful direct c0-c3
replays, the subset that also installed transient state, and categorized
fallbacks. `replays + fallbacks` is the number of fully proved translation-only
relations; every fallback must execute the complete stock/native slot before
the draw. In a healthy finite run, fallback causes should remain zero and the
replay count should account for most `translation_only` mismatches that were
not separated by another later-key change.

The command-build diagnostic additionally reports `tile_retained_builds`,
`refreshes`, `hits`, `misses`, and `packet_reuses`. After a menu reaches steady
state, builds should track newly created or program-changed Tile text,
atlas/resource-only preflight changes may increment refreshes without rebuilding
the skeleton, hits should track commandized Tile traversals, misses should
remain zero, and packet reuse should closely track recorded command packets.

The adjacent `standard_pass_lite_` line exposes stage invariants for the dedicated
single-packet subset. A healthy fully eligible retail run has
`candidates = stage1_eligible = stage2_resident = stage3_replays`,
`standard_v2_replays = stage3_replays`, `standard_v2_compat=0`,
`retained_hits = candidates`, `retained_misses=0`, `stock_fallbacks=0`, and
every categorized fallback at zero. Standard v2 accepts the six retail slot
implementations plus the signature- and PE-identity-verified
`Alpha Fixes.dll` 2.52 slot-32 implementation. The latter receives its own
blend-state key because it suppresses `fAlpha`-driven blending when
`BSShaderProperty::Vertex_Alpha` is set. A nonzero `standard_v2_compat` means
at least one retained shader generation did not match a fully classified
six-slot table; it increments `fallback_program` and returns to stock `B994F0`
before any lite prelude or draw, rather than executing an unknown callback
inside the delta cache. `retained_builds` counts new Tile/program
dispatches, while `retained_reuses` counts full preflights that retained the
same Tile/program dispatch instead of rebuilding it; neither should scale with
steady-state packet submissions. When fallbacks are present,
`stock_fallbacks` equals the sum of `fallback_envelope`, `program`, `renderer`,
`geometry`, `binding`, and `prelude`; the retained hit/miss pair distinguishes
a missing or invalidated Tile dispatch from a dynamic pass-envelope rejection.
The following `segment_device_state_` line reports cache starts/reuses and
set/reuse pairs for texture/program, constants, blend, alpha-test, and drawmode
callbacks, followed by actual slot-35 calls and verified no-op elisions.
In a long contiguous menu run, `starts` should track execution-segment or stock
fallback boundaries rather than one-packet replays, while nonzero category
`reuses` directly count callbacks skipped across distinct Tiles.
`constants_reuses` proves slot 31 was skipped only for identical
non-transient state; `post_elisions` normally covers every verified packet
without scissor/stencil, including packets whose constants changed. Alpha-test
sets/reuses may remain zero because the native A8 direct route normally
disables stock alpha testing.

## Atlas allocation, mipmaps, and memory

Persistent atlas pages start at 512x512 and grow without moving existing glyphs.
Missing glyphs are rasterized as one batch and uploaded through one dirty
rectangle. Level-zero-only true-SDF/A8 and MTSDF/BGRA pages use one
outside-distance padding pixel per side, which is sufficient to isolate their
bilinear footprint because
the distance spread and an additional guard texel are already inside each glyph
bitmap. Aggressive composite and ARGB fallback pages retain four
transparent pixels per side, isolating the 1/4 mip even when glyph dimensions
are not multiples of four. Repeated text also reuses cached layout and unique
text artifacts. Text artifacts use a two-observation coarse-signature admission
filter: a one-shot menu string is returned directly to its shape without
entering the global map/LRU, while a warmed signature may create a fully
validated cache resident. A small
thread-local weak front serves recent resident or still-live artifacts without
taking the global cache mutex and does not pin an artifact after its real owners
release it. The first observation uses a constant-cost geometry/effect
signature and skips the full per-quad geometry/color fingerprint and
range/effect identity hash.
One artifact owns the packed
vertices, bound, atlas-page
property/texture references, and merged contiguous packet descriptors that used
to live in separate batch and packet-template caches. Geometry, per-glyph base
colors, layer constants, composite mode, and referenced page identities
therefore form one
validated cache identity; an atlas wrapper address cannot revive an artifact
whose retained property or texture differs. True SDF, MTSDF, and 32-bit fallback
profiles use separate cache keys; aggressive BGRA composite additionally has a
distinct pixel/render profile and prewarm identity. Changing
`uiFreeTypeFontDistanceFieldMode`
therefore selects new bitmap, manifest, snapshot, and shader identities without
requiring manual cache deletion. CPU coverage and distance-field manifests
cannot alias, and final CPU-route selection removes normal distance-field cache
files instead of retaining a provisional native/fallback mixture.

Runtime cache-key work is amortized at the point where the corresponding
identity becomes immutable:

- Prepared text performs one source scan for length, escape detection, and the
  source hash. Escape expansion hashes its resolved bytes while copying them.
  Lookup hashing consumes those saved hashes and lengths, while exact string
  comparison remains the collision check. A small thread-local metric-signature
  cache is reused only after byte-exact validation of the mutable font metrics.
  The periodic performance line keeps `prepared_text_hit` and its following
  `miss`, and additionally reports `promotion_bypass`, `probe`, `probe_miss`,
  `admitted`, `evicted`, `admission_rejected`, and `backoff_bypass`. A healthy
  repeated-text path normally records one promotion and admission followed by
  TLS hits with no probe; `probe_miss`, `admission_rejected`, or sustained
  `backoff_bypass` identifies collision, unsupported, or memory-pressure churn
  that still needs investigation.
- A one-shot text artifact computes only the constant-cost admission signature.
  Once admitted, one quad traversal computes geometry and color fingerprints
  together; effect/range identity consumes that color fingerprint instead of
  scanning the quads again.
- Distance-field atlas keys are derived directly from the sealed byte-role
  raster profile. The MTSDF/true-SDF route therefore avoids rescanning the
  bitmap list and building, sorting, and deduplicating an empty baked-color
  variant vector. Mixed or non-distance-field routes retain the generic scan.
- A resolved bitmap key computes its revision-aware stable hash once. The same
  value supplies the glyph cache ID, persistent-cache identity, batch
  deduplication, and the folded in-memory unordered-map hash.

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

During bounded-throughput prewarm, the bitmap LRU keeps a preferred one-quarter working
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
generations are restored after a D3D9 device reset. A version-23 snapshot
records A8 true SDF, BGRA MTSDF, or BGRA composite glyphs and is uploaded
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
wrappers. A later glyph insertion first detaches that page. Device reset rebuilds
each unique immutable payload once and reattaches the other logical wrappers to
that physical D3D9 texture; GPU-budget accounting likewise counts unique texture
identities rather than wrappers. Glyphs added later retain only their individual
masks. If direct
DEFAULT-pool creation is unavailable, snapshot restore and normal atlas
creation fall back to the engine-managed implementation.
Complete globally repacked snapshots are also validated and restored directly
when DEFAULT-pool atlases are disabled. The backend choice participates in the
snapshot identity, so a managed profile cannot accidentally reuse a
single-atlas DEFAULT-pool layout. The managed route therefore pays a bounded
snapshot restore on an unchanged launch instead of rerasterizing the code page.

The prewarm batch estimator separates memory retained for every glyph in the
batch from scratch memory live only in an active worker. True SDF therefore
retains one byte per texel and reserves a four-byte float field per worker;
MTSDF retains four bytes and reserves a sixteen-byte float field. The fallback
route retains Fill plus each enabled A8 effect but reserves only one rendered
body and chamfer field per worker. Aggressive composite retains one BGRA result
while reserving the body/effect masks and the second BGRA target used by
tight-bound cropping. Per-glyph request/result metadata and a fixed worker-local
FreeType/shape allowance are included as well. The estimated peak is bounded by
24 MiB, one eighth of the configured aggregate budget, or half the currently
available headroom after reserving one streamed page, whichever is smallest
(with a one-glyph emergency path when even that estimate cannot fit).
The writer reuses one preallocated 4 MiB or 16 MiB page buffer for the active
byte role, accounts its real capacity in the CPU budget, seals the single-byte
role before double-byte pixels begin, and releases that first buffer
immediately. This prevents two role buffers from remaining live during the
large DBCS pass while still avoiding vector-growth peaks.

A raster batch starts at up to 128 glyphs, adapts toward approximately 250 ms,
and can grow to 1024 glyphs when the memory estimate permits. Time-based
adaptation never reduces a normal batch below 64 glyphs; only an allocation
failure can lower that parallel floor. Distance-field, composite, glow,
outline, and shadow work begins parallel execution at eight cache misses,
while inexpensive Fill-only work retains a 64-miss threshold and processes
small chunks to reduce atomic scheduling overhead. Parallel raster work remains
capped at twelve workers so high-core-count hosts cannot silently expand the
32-bit address-space peak. Worker creation failure
falls back to the threads that did start plus the caller thread. Under memory
pressure the scan position and counters are rolled back, the memory-derived
maximum is reduced, and the batch is retried at half size down to the
one-glyph emergency limit.
Configurations with identical layout/mask inputs and the same maximum effect
radius share one distance-field prewarm even when their colors, offsets, powers,
or shader sampling quality differ; those properties do not alter atlas texels. CPU-baked
fallback profiles continue to include the complete effect hash.

A restored page uses one contiguous glyph-record vector sorted by `cacheId`.
Each record keeps the ID, rectangle, an index into the page's compact snapshot
metadata, and an optional live bitmap. After a complete code-page profile is
published, tNVSE builds and atomically saves an `.fnt`-style `.tnvfdirect` table
for each byte role. The single-byte table has 256 fixed records; the DBCS table
has 24,066 fixed records covering lead bytes `0x81-0xFE` and trail bytes
`0x40-0xFE`. Invalid encoding slots use a fixed invalid flag. Each valid
`DirectCachedLetter` is deliberately the same 56 bytes as an original
`FontLetter`: 24 bytes hold the encoded slot, flags, and layout metrics, while
four fixed 8-byte layer references hold only a page slot, mask type, and
snapshot-placement index. Four slots cover Fill, Outline, Glow, and Shadow;
aggressive composite and distance-field profiles use one. Rectangle, bearing,
spread, cache/content identity, and UV data remain in the immutable compact
snapshot and are not duplicated for every font/byte-role table. Spaces are
marked known-empty. Direct-cache format version 3 identifies this compact
layout, so version-2 `.tnvfdirect` files are rebuilt once without invalidating
the atlas bitmap snapshots.

The direct file is written through a flushed temporary file and atomic replace.
Its header and record block have independent checksums and include the manifest,
snapshot, render-route, scale, padding, page-count, and page-content identity.
On an unchanged second launch the complete fixed table is read contiguously
after page validation; atlas mapping does not re-traverse the manifest,
recompute every cache ID, or reconstruct an encoded-code placement map.
Equivalent profile
owners share the same table and atlas pages rather than saving one copy per font
alias.

Complete direct batches first acquire one batch-lifetime view of the dense
tables. Each used weak page reference is locked and checked once; compact layer
references are resolved to immutable snapshot placements and compact page
ordinals for that batch. Fixed
`[kind][64 pages]` count and cursor arrays determine the final allocation. The
second pass writes `NativeA8GpuVertex` records straight into their final
page-contiguous locations and emits ranges in Shadow, Glow, Outline, Fill order.
It does not create a `GlyphBitmapRequest`, `GlyphSource`, `PreparedGlyph`, or
`PendingQuad`, build a cache-ID page map, hash the completed quads, or sort the
batch. The distance-field route shares the body quad across Fill, Glow, Outline,
and an unshifted Shadow; an offset Shadow adds at most one second quad. Its
shared MTSDF raster profile is resolved at most once for each byte role, rather
than once or twice per glyph. The same role-level reuse also applies when an
incomplete direct profile enters the compatibility compiler.

The aggressive route always has one composite quad per visible glyph. A
single-page batch follows the original `.fnt` geometry pattern: it allocates the
final `NiTriShape` once and writes positions, UVs, colors, and canonical indices
directly. A multi-page batch writes the final native vertex/range payload once
and keeps only the one-quad engine facade required by the sorted Tile hook.

The placement index never depends on the mutable/sorted live glyph vector.
Direct-table creation or contiguous-file loading validates every stored
snapshot placement. A later text batch therefore checks the complete-profile
marker, current page-content identity, format, dimensions, texture, and table
owner once per used page instead of duplicating snapshot metadata in every
glyph record. The direct table holds weak page references so a nonzero GPU
budget can still evict unused pages. Page insertion, replacement, eviction, snapshot
publication, or removal of the complete-profile marker clears the table
atomically. The resolved page owners stay alive through geometry compilation;
the native payload then retains the properties and textures and validates
resource serial, upload epoch, and vertex/index ranges during submission. A
missing or expired entry makes the complete batch use the compatibility lookup;
snapshot placement pointers exist only during the owner-retaining geometry
batch and are never stored in a persistent table or native payload.

`uiFreeTypeFontGpuAtlasCacheMB` controls the soft GPU atlas budget. A value of
zero selects full-resident mode: every configured font snapshot is restored
during the bounded-throughput prewarm transaction, and GPU atlas pages are not evicted to
satisfy a software budget. Obsolete generations that are no longer referenced
are still reclaimed. A nonzero value is used directly as the soft budget.
Atlas generations still referenced by visible game shapes cannot be evicted,
so live usage may temporarily exceed a nonzero soft budget. The selected policy
is written to `tnvse.log` at initialization. When
`bEnableFreeTypeDefaultPoolAtlas=1`, validated level-zero distance-field,
coverage, and aggressive BGRA composite snapshots all restore directly to the
DEFAULT pool. Runtime atlas creation follows the same rule for every pixel and
render mode. A DEFAULT-pool creation failure rejects that atlas generation
instead of silently retaining a full engine-managed CPU backing. ARGB fallback
atlases that use a mip chain remain runtime-only and contain three mip levels
(1x, 1/2x, and 1/4x); the cache budget and upload counters include all levels.
Limiting the chain to three levels together with four-pixel per-side packing
padding prevents the coarsest bilinear footprint from reaching a neighboring
glyph. Text spanning pages is sorted into contiguous layer/page native packets;
each packet owns the corresponding atlas property and texture. Distance-field
packets use mip level zero and a zero LOD bias through their immutable shader profile.

## Scope and fallbacks

Glyph rasterization remains CPU based. FreeType produces fallback coverage and
the native-route vector outline from the same unhinted scalable outline; this
prevents grid fitting from thickening the aggressive/fallback body before glow,
outline, and shadow are derived. The statically linked msdfgen core converts the
same outline basis to true SDF or MTSDF. The CPU rasterizer and distance-field
shader still use different sampling representations, so exact subpixel equality
is not guaranteed.
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

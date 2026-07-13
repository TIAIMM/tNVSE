# FreeType game font rendering

The feature is disabled by default. Enable both options in `tnvse.ini`:

```ini
[Main]
bEnableMultibyteFontHook=1
bEnableFreeTypeFontRendering=1
bEnableFreeTypeFontRenderingLog=0
bEnableFreeTypeDevicePixelScale=1
```

`bEnableFreeTypeDevicePixelScale=1` rasterizes glyphs at the physical screen
pixel density reported by the UI `resolutionconverter` trait. UIO 2.30 zoom is
multiplied into that device scale for affected `CreateText` calls. Layout,
wrapping, alignment, and returned dimensions remain in game UI units. Set the
option to `0` to retain the previous 1.0 device raster scale.

Set `bEnableFreeTypeFontRenderingLog=1` while diagnosing configuration or font
loading. The log records the XML path, resolved face paths, FreeType errors,
font-ID activation, and the first atlas-rendered glyph for each byte class.

Font IDs are configured under `<fonts>` in
`Data\NVSE\plugins\tnvse_fonts.xml`. Only listed IDs are replaced. Other
fonts continue to use the original `.fnt` and `.tex` files.

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

      <glow enabled="1" width="2" color="#66FF99" alpha="0.35"/>
      <outline enabled="1" width="1" color="#000000" alpha="1"/>
      <shadow enabled="1" x="1" y="1" color="#000000" alpha="0.65"/>
    </font>
  </fonts>
</tNVSE>
```

`singleByte` and `doubleByte` inherit scalar attributes from `<font>` and may
override them independently. If a byte-class node contains a `<face>` or
`<fallback>`, its complete face chain replaces the parent chain. A resolved
byte-class configuration must have one primary face and a positive
`pixelSize`. The independently overridable attributes are `pixelSize`,
`tracking`, `scaleX`, `scaleY`, `embolden`, `slant`, and `baselineOffset`.

Paths may be absolute or relative to the Fallout New Vegas directory. `index`
selects a face in TTC/OTC files. `slant` is measured in degrees; other style
dimensions are pixels. `baseline="0"` derives the shared Fallout `fBaseLine`
from both primary faces. A positive value sets that baseline manually, is
rounded upward to the game's integer metric, and disables automatic
visual-center correction between the byte classes. Glow, outline, and shadow
are disabled when their node is absent; when a node is present, `enabled`
defaults to `1`.

`baseline` controls Fallout's line rise and therefore the distance between
lines. It is not a general glyph Y offset. Use the independently inheritable
`baselineOffset` on `singleByte` or `doubleByte` to move that byte class within
the line; positive values move glyphs upward. In manual baseline mode the
renderer keeps the FreeType body top/drop metrics and does not apply automatic
single/double-byte visual-center correction.

`fontColor="#RRGGBB"` and `fontAlpha` optionally override the fill color for
the font ID. When `fontColor` is omitted, fill geometry keeps the color supplied
by the game. `glow`, `outline`, and `shadow` belong to the font ID and are
shared by both byte classes. `effectQuality="fast|balanced|high"` selects the
PS 3.0 sampling preset and defaults to `balanced`. Shadow accepts an optional
non-negative `blur` radius; zero preserves a hard offset shadow. Every
configured alpha is multiplied by the game text alpha, so visibility and fade
animations continue to work.

`shaping="0"` is the default. It uses FreeType 26.6 advances and
`FT_Get_Kerning()` without rounding every glyph in advance. `shaping="1"`
enables HarfBuzz GSUB/GPOS for ordinary `Font` text. The optional
comma-separated `features` attribute uses HarfBuzz feature syntax, for example
`kern,liga,ss01=1,-dlig`; unspecified features keep HarfBuzz's script defaults.
`features` is invalid unless shaping is enabled. Rich text keeps one game
`CharData` per encoded character and therefore uses precise FreeType kerning
without GSUB substitutions.

`prewarm="none"` is the default and preserves fully demand-driven atlas
generation. `prewarm="common"` prepares valid single-byte units plus up to
7000 valid double-byte units from the code page's standard common-character
region, while `prewarm="codepage"` follows DCFGCF's explicit encoding ranges
for the current `uiEncoding` code page. CP936 uses DCFGCF's complete GBK
profile instead of its smaller GB2312 profile. Prewarming begins after the configured font is
activated and processes at most eight valid encoded units per game frame, so
it does not block startup with one large rasterization pass. It only prepares
the base raster scale of `1.0`; UIO-derived sizes remain demand-driven. A font
task stops and reports `atlas-full` if its complete set cannot fit the maximum
atlas size.

Routing follows the selected `uiEncoding`: valid one-byte units use
`singleByte`, while valid DBCS pairs use `doubleByte`. Shift-JIS half-width
katakana therefore use the single-byte font. Missing glyph lookup stays inside
the selected byte-class fallback chain and then tries `U+FFFD`, `?`, and the
primary face's `.notdef` glyph.

The normal rendering path rasterizes hinted grayscale glyphs with FreeType at
the effective display size. When Fallout Shader Loader 1.40 or newer,
`tnvse_freetype_a8.pso`, and a real `D3DFMT_A8` texture are available, each
font/style/effective-size profile uses a one-byte A8 atlas. A shape-specific
pixel shader reads the atlas alpha as coverage and follows the game's
uniform Tile contract: RGB is `c0.rgb` multiplied by the per-layer XML color
modifier, while alpha is coverage multiplied by `c0.a` and the per-layer XML
alpha. The shaders do not consume a `COLOR0` vertex stream, and RGB remains
straight rather than premultiplied by alpha. This same ABI is shared by fill,
shadow, glow, outline, and future FreeType effects. tNVSE does not replace the
global TileShader.
If any dependency
or runtime validation fails, that profile automatically uses the existing
`A8R8G8B8` atlas path, so FreeType rendering itself does not require Shader
Loader. In that fallback, XML layer RGB and alpha are baked into the 32-bit
atlas and the original Tile shader still supplies the dynamic Tile color and
alpha.

The base A8 shader and all effect variants use `ps_3_0`. Shader effects reuse
the fill mask and execute global shadow, glow, outline, and fill passes over
one `NiTriShape`; this prevents a later glyph effect from covering an earlier
glyph fill. Glow is a soft outer halo and outline is an outer-only dilation.
When an effect shader is unavailable, the renderer retains the CPU mask path
with the same global layer order. When `NVSE_PLUGIN_PATH` is defined, an
ordinary project build copies all compiled PSOs to `Data\Shaders\Loose`.
Custom draws switch pixel shader and sampler state through Gamebryo's render
state manager and restore private constants after every draw. A detected
Gamebryo/D3D state mismatch disables the custom pass for that draw instead of
leaking state into later UI rendering.

Persistent atlases start at 512x512 and grow without moving existing glyphs.
Missing glyphs are rasterized as one batch and uploaded through one dirty
rectangle. CPU-effect atlas regions have two transparent padding pixels.
Shader-effect profiles reserve a larger transparent gutter based on the final
device-pixel radius, preventing neighboring glyphs from entering effect
samples. Mipmaps are disabled and sampling is nearest. Repeated text also
reuses cached layout and vertex/UV/index templates. A8 and 32-bit profiles use
separate cache keys and may coexist when text was created before Shader Loader
initialization.

Generated grayscale masks, layouts, and batch templates are cached in process
memory. `uiFreeTypeFontMemoryCacheMB` controls those CPU-side caches. When
`bEnableFreeTypeDefaultPoolAtlas=1`, tNVSE creates dynamic `D3DPOOL_DEFAULT`
atlas textures and retains only the masks used by each live atlas generation;
it does not retain a complete CPU copy of the atlas. The current and retired
generations are restored from those masks after a D3D9 device reset. If the
DEFAULT-pool path is unavailable, that profile falls back to the engine-managed
atlas implementation.

`uiFreeTypeFontGpuAtlasCacheMB` controls the soft GPU atlas budget. A value of
zero selects one eighth of the available texture memory, rounded to 16 MB and
clamped to 64-256 MB; 128 MB is used when the device does not report a reliable
value. A nonzero value is used directly. Atlas generations still referenced by
visible game shapes cannot be evicted, so live usage may temporarily exceed the
soft budget. The resolved value is written to `tnvse.log` at initialization and
when a device reset changes the automatic result. Cache misses are handled at
runtime; tNVSE does not perform glyph-cache file I/O.

FreeType rasterization remains CPU based. Adding Skia, D3D11, or D3D12 would
require a readback or cross-API copy before Fallout New Vegas can consume the
result through D3D9, increasing synchronization cost and reducing DXVK/Wine
compatibility. The GPU is therefore used for persistent atlas sampling and
quad rendering, while hinted masks are generated once on the CPU.

When UIO 2.30 scales a TileText call, tNVSE includes the validated UIO scale in
the effective raster size and aligns glyph geometry to final screen pixels.
Other UIO versions and ordinary calls use a raster scale of `1.0`. If atlas
creation fails, the renderer falls back to the libtess2 outline path. HarfBuzz
shaping is limited to horizontal LTR text in the configured DBCS code pages;
bidirectional layout, LCD subpixel rendering, color-font rendering, and
variable-font axis controls are outside this feature. Invalid or unavailable
configurations leave that entire font ID on the original `.fnt`/`.tex` renderer.

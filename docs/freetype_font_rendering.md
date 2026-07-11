# FreeType game font rendering

The feature is disabled by default. Enable both options in `tnvse.ini`:

```ini
[Main]
bEnableMultibyteFontHook=1
bEnableFreeTypeFontRendering=1
bEnableFreeTypeFontRenderingLog=0
```

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
shared by both byte classes. The renderer emits shadow, glow, outline, and fill
geometry in that order. Every configured alpha is multiplied by the game text
alpha, so visibility and fade animations continue to work. Glow and outline
are generated as grayscale masks and share the same text atlas as the fill.

`shaping="0"` is the default. It uses FreeType 26.6 advances and
`FT_Get_Kerning()` without rounding every glyph in advance. `shaping="1"`
enables HarfBuzz GSUB/GPOS for ordinary `Font` text. The optional
comma-separated `features` attribute uses HarfBuzz feature syntax, for example
`kern,liga,ss01=1,-dlig`; unspecified features keep HarfBuzz's script defaults.
`features` is invalid unless shaping is enabled. Rich text keeps one game
`CharData` per encoded character and therefore uses precise FreeType kerning
without GSUB substitutions.

Routing follows the selected `uiEncoding`: valid one-byte units use
`singleByte`, while valid DBCS pairs use `doubleByte`. Shift-JIS half-width
katakana therefore use the single-byte font. Missing glyph lookup stays inside
the selected byte-class fallback chain and then tries `U+FFFD`, `?`, and the
primary face's `.notdef` glyph.

The normal rendering path rasterizes hinted grayscale glyphs with FreeType at
the effective display size. Each font/style/effective-size profile owns a
persistent `A8R8G8B8` D3D9 atlas that starts at 512x512 and grows without
moving existing glyphs. Missing glyphs are rasterized as one batch and uploaded
through one dirty rectangle. Atlas regions have two transparent padding pixels,
mipmaps are disabled, and sampling is nearest. Repeated text also reuses cached
layout and vertex/UV/index templates. Shadow, glow, outline, and fill are
emitted into one `NiTriShape` using the shared atlas.

Generated grayscale masks, layouts, batch templates, and persistent atlas pages
are cached in process memory. `uiFreeTypeFontMemoryCacheMB` in `tnvse.ini`
controls their combined budget. Cache misses are handled at runtime; tNVSE does
not perform background code-page generation or glyph-cache file I/O.

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

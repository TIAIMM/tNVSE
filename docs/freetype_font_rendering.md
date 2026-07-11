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
font-ID activation, and the first vector-rendered glyph for each byte class.

Font IDs are configured under `<fonts>` in
`Data\NVSE\plugins\tnvse_fonts.xml`. Only listed IDs are replaced. Other
fonts continue to use the original `.fnt` and `.tex` files.

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
          renderMode="gray"
          embolden="0"
          slant="0"
          baselineOffset="0"
          lineHeight="0"
          curveTolerance="0.35">

      <!-- Optional defaults inherited by both byte classes. -->
      <face path="Data/Fonts/Default.ttf" index="0"/>
      <fallback path="Data/Fonts/Symbols.ttf" index="0"/>

      <singleByte pixelSize="20" baselineOffset="0" renderMode="lcd-rgb">
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
`tracking`, `scaleX`, `scaleY`, `embolden`, `slant`, `baselineOffset`, and
`renderMode`.

`renderMode` accepts `gray`, `lcd-rgb`, and `lcd-bgr`. It defaults to `gray`.
LCD mode is explicit because the game cannot reliably detect the monitor's
physical subpixel order. `lcd-rgb` and `lcd-bgr` use FreeType's horizontal LCD
filter and exchange the red/blue coverage channels as requested.

LCD rendering requires Fallout Shader Loader 1.40 and these files:

```text
Data\Shaders\Loose\tnvse_freetype_lcd_r.pso
Data\Shaders\Loose\tnvse_freetype_lcd_g.pso
Data\Shaders\Loose\tnvse_freetype_lcd_b.pso
```

The files are built from `tnvse/shaders/freetype_lcd.hlsl`. If Shader Loader,
its `CreatePixelShader` export, or any shader file is unavailable, LCD styles
fall back to the grayscale atlas renderer; other FreeType functionality stays
enabled.

Paths may be absolute or relative to the Fallout New Vegas directory. `index`
selects a face in TTC/OTC files. `slant` is measured in degrees; other style
dimensions are pixels. `lineHeight="0"` derives the shared line metrics from
both primary faces. Glow, outline, and shadow are disabled when their node is
absent; when a node is present, `enabled` defaults to `1`.

`fontColor="#RRGGBB"` and `fontAlpha` optionally override the fill color for
the font ID. When `fontColor` is omitted, fill geometry keeps the color supplied
by the game. `glow`, `outline`, and `shadow` belong to the font ID and are
shared by both byte classes. The renderer emits shadow, glow, outline, and fill
geometry in that order. Every configured alpha is multiplied by the game text
alpha, so visibility and fade animations continue to work. Glow is a solid
outer vector stroke; it is not a blurred bitmap effect.

Routing follows the selected `uiEncoding`: valid one-byte units use
`singleByte`, while valid DBCS pairs use `doubleByte`. Shift-JIS half-width
katakana therefore use the single-byte font. Missing glyph lookup stays inside
the selected byte-class fallback chain and then tries `U+FFFD`, `?`, and the
primary face's `.notdef` glyph.

Normal rendering uses FreeType hinted grayscale or LCD bitmaps cached by final
effective pixel size in a 64 MB process-wide LRU. A text batch packs its masks
into one `A8R8G8B8` atlas and returns one `NiTriShape`. Gray batches use the
standard TileShader pass. LCD batches draw that same shape three times with
independent red, green, and blue write masks; outline, glow, and shadow remain
grayscale masks in the same atlas. UIO scaling is included in the effective
pixel size only when the detected UIO plugin version is exactly 2.30.

The old libtess2 outline renderer remains the atlas failure fallback. HarfBuzz
shaping, kerning, RTL layout, color-font rendering, and variable-font axis
controls are outside this feature. Invalid or unavailable configurations leave
that entire font ID on the original `.fnt`/`.tex` renderer.

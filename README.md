# tNVSE

tNVSE provides configurable FreeType font rendering, East Asian multibyte text
support, IME input, and runtime dictionary translation for Fallout: New Vegas.
The repository includes default, English, Simplified Chinese, and Russian
configuration examples.

## Configuration examples

Choose one configuration from `docs`:

| Directory | FreeType rendering | UI encoding | Multibyte font hook | IME input | Dictionary translation |
| --- | --- | --- | --- | --- | --- |
| [default config](docs/default%20config) | Disabled | Windows-1252 | Disabled | Disabled | Disabled |
| [english config](docs/english%20config) | Enabled | Windows-1252 | Disabled | Disabled | Disabled |
| [chinese config](docs/chinese%20config) | Enabled | GBK/CP936 | Enabled | Enabled | Enabled |
| [russian config](docs/russian%20config) | Enabled | Windows-1251 | Disabled | Disabled | Disabled |

`tnvse.ini` controls rendering, encoding, input, and dictionary options.
`tnvse_fonts.xml` configures individual font slots. The default XML contains a
commented example with no active fonts; the other three XML files configure
slots 1-8 and 42. The default and Chinese directories also provide
`tnvse_dictionary.xml`; dictionary sources must be registered there before they
can be used. Configuration values in these examples are preset choices, and
may differ from the defaults used when a key is absent.

## Installation

Install tNVSE's runtime files under the game's `Data` directory, then copy the
selected configuration's `tnvse.ini`, `tnvse_fonts.xml`, and `fonts` directory
to `Data\NVSE\plugins`. Copy `tnvse_dictionary.xml` when using dictionary
translation, together with the source files it references.

For MO2, use the same directory structure inside the mod. Enable one
configuration preset and let it override the base package's configuration
files.

```text
Data/
├── NVSE/plugins/
│   ├── tnvse.dll
│   ├── tnvse.ini
│   ├── tnvse_fonts.xml
│   ├── tnvse_dictionary.xml       # When using dictionary translation
│   └── fonts/                     # Files referenced by the selected XML
├── Shaders/Loose/
│   └── tnvse_freetype_native_*.pso / *.vso
├── Menus/prefabs/tNVSE/
│   ├── FontPrewarmOverlay.xml
│   └── ImeOverlay.xml
└── Textures/Interface/tNVSE/
    └── font_prewarm_label.dds
```

The shader files come from the runtime package or a source build. Overlay XML
and the label texture are supplied under `docs/Menus` and `docs/Textures`.
Restart the game after changing the encoding or font configuration.

## UI encoding

Set `uiEncoding` under `[Multibyte]`:

| Value | Encoding | Layout |
| --- | --- | --- |
| 0 | Windows-1252 | Single byte |
| 1 | GBK, Windows code page 936 | Double byte |
| 2 | Big5, Windows code page 950 | Double byte |
| 3 | Shift-JIS, Windows code page 932 | Double byte |
| 4 | UHC, Windows code page 949 | Double byte |
| 5 | Windows-1251 | Single byte |

FreeType rendering is controlled independently by
`bEnableFreeTypeFontRendering` under `[FreeTypeFont]`. East Asian layouts 1-4
also require `bEnableMultibyteFontHook=1`. With that hook disabled, FreeType
uses Windows-1252 unless `uiEncoding=5` selects Windows-1251.

The Russian preset uses:

```ini
[FreeTypeFont]
bEnableFreeTypeFontRendering = 1

[Multibyte]
bEnableMultibyteFontHook = 0
uiEncoding = 5
bUTF8 = 0
```

Windows-1251 decodes Cyrillic text through the single-byte font configuration.
It does not enable the East Asian UTF-8 conversion, IME, or dictionary paths.
The source text must use the selected code page and the configured face must
contain the required glyphs. Selecting an encoding does not translate the
game's text.

`bUTF8=1` enables supported UTF-8 source detection and conversion for East
Asian modes 1-4. `bMultibyteInput=1` requires those modes and successfully
installed multibyte font hooks.

## Fonts

The active font references in the supplied presets are:

| Preset | Slots 1-4, 6-7, 42 | Slot 5 | Slot 8 |
| --- | --- | --- | --- |
| English | `monofonto.ttf` | `FixedsysExcelsior.ttf` | `Futura BdCn BT Bold.ttf` |
| Chinese, single byte | `monofonto.ttf` | `FixedsysExcelsior.ttf` | `Futura BdCn BT Bold.ttf` |
| Chinese, double byte | `SarasaUiSC-Bold.ttf` | `WenQuanYi Bitmap Song 16px.ttf` | `HarmonyOS_Sans_SC_Black.ttf` |
| Russian, single byte | `Barlow Condensed-Medium.ttf` | `Fixedsys Excelsior 3.01-Regular.ttf` | `Barlow Condensed-Bold.ttf` |

The English font directory contains `FixedsysExcelsior.ttf`. The Chinese
directory contains that font and the three CJK faces listed above, plus
`SourceHanMonoSC-Heavy.otf`, which the current preset does not reference.
The Russian directory contains its three referenced fonts. Its Barlow
Condensed faces are Cyrillic versions prepared by Yggge.

`monofonto.ttf` and `Futura BdCn BT Bold.ttf` are referenced by the English
and Chinese presets but are not included in their source directories. Supply
these files from your font package or update the XML paths to suitable faces.

`<singleByte>` handles valid one-byte units, including Windows-1252 and
Windows-1251 characters. `<doubleByte>` handles valid East Asian DBCS pairs.
A `<face>` and optional `<fallback>` chain declared directly under `<font>`
is inherited by both classes. A class-specific chain replaces the inherited
chain. Paths are absolute or relative to `FalloutNV.exe`, for example
`Data\NVSE\plugins\fonts\SarasaUiSC-Bold.ttf`.

Font slots, sizes, tracking, fixed widths, baseline adjustments, and glow,
outline, and shadow effects are configured in `tnvse_fonts.xml`. The supplied
font XML files include the attribute reference. Only listed slots are
configured; the last entry wins when a slot is repeated. Slot 42 targets the
Stewie Tweaks menu font registered through JIP LN NVSE. Extended slots 10-89
require registration through JIP; slot 9 is invalid.

`prewarmEncoding="gbk"` or `"gb2312"` controls CP936 startup coverage only.
It does not select the text encoding and has no effect on Windows-1251.

## Rendering and optional integrations

`uiFreeTypeFontDistanceFieldMode` selects baked effects (0), TSDF (1), or
MTSDF (2). Fallout Shader Loader 1.40 or newer enables the optional native
shader route; an ARGB compatibility route is used when it is unavailable.
The supplied presets select mode 2.

IME adapters can integrate with Stewie Tweaks, MCM Extender, Dialogue History,
and Modern Help Menu according to the `[MultibyteInput]` options. The Stewie
input integration requires version 9.95 or newer. JIP Big Guns replacement
and JIP key-event suppression are specific to JIP LN NVSE 57.30; enable them
only for the matching setup.

## Dictionary translation

The dictionary path requires an East Asian encoding, multibyte font hooks,
and `bEnableDictionaryTranslation=1`. Register text, XML, JSON, or directory
sources in `Data\NVSE\plugins\tnvse_dictionary.xml`. The supplied dictionary
XML files contain registration examples.

Exact and ID matches are supported alongside optional wildcard, regular
expression, and structured UI translation. Mixed DBCS/ASCII wildcard, regex,
and Perk translation additionally require
`bEnableDictionaryMixedSourceTranslation=1` and the corresponding route
option. That mixed-source option is disabled in the supplied presets.
`<perkrequirements>` configures the translated requirement labels and logic
words. Console-open dictionary translation is bypassed by default through
`bDisableDictionaryTranslationInConsole=1`.

## Building from source

Use Visual Studio 2026 with MSVC v145, the Win32 target, and CMake 4.3.1 or
newer. Initialize the FreeType and msdfgen submodules and restore the NuGet
packages before configuring:

```powershell
git submodule update --init --recursive
nuget restore tnvse/packages.config -PackagesDirectory tnvse/packages
cmake --preset vs2026-win32
cmake --build --preset vs2026-win32-release
```

The Release DLL and PDB are written to
`out/build/vs2026-win32/bin/Release`. Compiled shaders are written to
`tnvse/shaders/compiled`.

For live deployment, set `NVSE_PLUGIN_PATH` to the destination `NVSE/plugins`
directory before configuring. The build deploys the DLL, PDB, native shaders,
overlay XML, and overlay texture into the corresponding mod directories.
Copy the selected INI, font XML, dictionary XML, and font files separately.

The current CMake install rules still reference the old `docs/tnvse_fonts.xml`
and `docs/fonts` locations rather than selecting a preset from `docs`.
Use the manual configuration installation above; the install target does not
automatically install a language preset.

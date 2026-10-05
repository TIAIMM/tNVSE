# Russian CP1251 Configuration for tNVSE

This directory contains a ready-to-use Russian configuration for tNVSE with native Windows-1251 (CP1251) text encoding support.

## Features

* Native Windows-1251 text encoding
* Cyrillic text rendering without the Multibyte Font Hook
* Russian tNVSE configuration
* FreeType font rendering configuration
* Cyrillic-compatible fonts
* CMake installation support

## Encoding configuration

The Russian configuration uses:

```ini
[Multibyte]
bEnableMultibyteFontHook = 0
uiEncoding = 5
bUTF8 = 0
```

`uiEncoding = 5` selects Windows-1251.

The Multibyte Font Hook is not required for native CP1251 rendering.

When tNVSE is running with this configuration, the runtime information should report:

```text
g_bEnableMultibyteFontHook: 0
Encoding: uiEncoding=5 codePage=1251
EnableUTF8: 0
```

Cyrillic text has been tested directly in Fallout: New Vegas.

## Installation

Copy the contents of this directory to:

```text
Fallout New Vegas\Data\NVSE\plugins\
```

The resulting structure should be:

```text
Data\
└── NVSE\
    └── plugins\
        ├── tnvse.ini
        ├── tnvse_fonts.xml
        └── fonts\
            ├── Barlow Condensed-Bold.ttf
            ├── Barlow Condensed-Medium.ttf
            └── Fixedsys Excelsior 3.01-Regular.ttf
```

The configuration and fonts can also be installed automatically using the project's CMake installation target.

## Included fonts

The configuration includes three fonts with Cyrillic support:

1. **Barlow Condensed Bold** — Cyrillic version prepared by Yggge
2. **Barlow Condensed Medium** — Cyrillic version prepared by Yggge
3. **Fixedsys Excelsior 3.01 Regular** — font with Cyrillic support

The Barlow Condensed fonts are modified versions prepared specifically for Russian-language use in Fallout: New Vegas.

## Russian configuration

The included `tnvse.ini` contains Russian translations of the configuration comments and descriptions.

The configuration is intended to provide sensible defaults for Russian-language Fallout: New Vegas installations while preserving the original tNVSE functionality.

## Requirements

This configuration requires a version of tNVSE containing Windows-1251 support.

It is intended for the `cp1251-support` branch and later versions containing the same functionality.

## Verification

Tested with:

```text
uiEncoding = 5
codePage = 1251
bEnableMultibyteFontHook = 0
bUTF8 = 0
```

Cyrillic text renders correctly without enabling the Multibyte Font Hook.

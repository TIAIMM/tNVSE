# FalloutNV Chinese input hook plan

## Goal

Make editable in-game text fields accept Chinese input without requiring the real game-side storage to become UTF-8. The committed text should enter the game as the current UI codepage multibyte string, such as CP936/GBK, and then use the existing tNVSE font/rendering hooks to display CJK glyphs.

This document is separate from the rich-text pipeline and save-display-name mapping work. Rich text and save-list display solve rendering and display metadata; Chinese input needs an input pipeline hook.

## Current project state

tNVSE already has these relevant pieces:

- Regular font/text rendering can render multibyte glyphs through `FontEx` and extra glyph tables.
- Rich text has DBCS-aware parser/render hooks in `font_manager.cpp`.
- Quest/location and selected UI text paths use `Tile::SetString` hooks in `text_hooks.cpp`.
- Save name sanitizer at `0x8518BB` can capture the original candidate before the actual `.fos` name is sanitized.
- Save display names can be stored in `.nvse` co-save through the `SVDN` mapping.

What is still missing:

- A Windows IME input source. Fallout New Vegas primarily consumes keyboard state/ASCII-style input; IME composition/result strings are not naturally delivered into game edit buffers.
- A DBCS-aware editable string model. Backspace, delete, cursor movement, selection, max length, and visual caret position must treat a lead+trail pair as one logical character.
- Hook points for active editable controls, especially save-name entry and `TextEditMenu`-style text fields.

## High-level design

Use Windows IME for input acquisition, convert committed text to the current UI codepage, and inject it into the active game edit buffer through a small DBCS-aware editing layer.

Do not store UTF-8 in game edit buffers unless the active UI encoding is actually UTF-8-capable end to end. For the current tNVSE font path, the practical target is:

```text
IME UTF-16 result -> current Windows codepage bytes -> game editable string -> existing font hooks
```

For CP936/GBK, committed Chinese text becomes GBK bytes. The existing font glyph map then resolves `(lead << 8) | trail`.

## Components

### 1. IME message capture

Add a small module, for example:

```text
tnvse/Src/chinese_input.h
tnvse/Src/chinese_input.cpp
```

Install a window procedure hook once the FalloutNV main HWND is available:

- Prefer subclassing the game window with `SetWindowLongPtrA(hwnd, GWLP_WNDPROC, ...)`.
- Keep the original WndProc and always forward unhandled messages with `CallWindowProcA`.
- Install after the main window exists, not from `DllMain`.
- Remove or restore only on process detach if the module owns the subclass.

Messages to handle:

- `WM_IME_STARTCOMPOSITION`: mark composition active.
- `WM_IME_COMPOSITION`: read `GCS_COMPSTR` for optional preview and `GCS_RESULTSTR` for committed text.
- `WM_IME_ENDCOMPOSITION`: clear preview state.
- `WM_CHAR`: fallback for non-IME Unicode/ANSI text and pasted basic characters.
- Optional: `WM_PASTE` if clipboard paste into edit fields should be supported.

Use Unicode IME APIs:

```cpp
HIMC context = ImmGetContext(hwnd);
LONG bytes = ImmGetCompositionStringW(context, GCS_RESULTSTR, nullptr, 0);
std::wstring text(bytes / sizeof(wchar_t), L'\0');
ImmGetCompositionStringW(context, GCS_RESULTSTR, text.data(), bytes);
ImmReleaseContext(hwnd, context);
```

Then convert:

```text
UTF-16 -> WideCharToMultiByte(g_usingWinEncoding)
```

If `g_usingWinEncoding == 0`, reject IME commit for multibyte mode or fall back to UTF-8 only if the whole target field/render path has been proven UTF-8-safe.

### 2. Active edit target tracking

IME commit should only be consumed when a known editable field is active. Do not globally inject text whenever the player has an IME open.

Targets to support first:

- Save-name text entry.
- Character/name entry if it uses the same or similar `TextEditMenu` path.
- Other `TextEditMenu` fields only after the buffer layout and commit path are verified.

Known references:

- `RTTI_TextEditMenu` exists in commonlib at `0x0119F704`.
- Save name sanitizer callsite currently hooked at `0x8518BB`.
- `Tile::SetString` is `0xA01350`, but final input injection should not be a global `Tile::SetString` hook.

Recommended reverse targets:

- `TextEditMenu` constructor/destructor/open/close to identify the active edit object.
- Per-frame or key-event handler that mutates the edit buffer.
- Function that commits the edit buffer to game code.
- Save-name menu entry handler and its backing buffer.
- Tile update callsites that display the current edit buffer.

The active target object should expose or wrap:

```cpp
struct ChineseInputTarget
{
    void* owner;
    char* buffer;
    UInt32 capacity;
    UInt32* caretByteOffset;
    UInt32* selectionStartByteOffset;
    UInt32* selectionEndByteOffset;
    bool (*Commit)(ChineseInputTarget* target);
    bool (*RefreshDisplay)(ChineseInputTarget* target);
};
```

The actual structure does not have to match this shape. This is the adapter tNVSE should build around reverse-verified fields/functions.

### 3. DBCS-aware edit buffer

Once a target is active, all insert/delete/caret operations must use byte offsets that never land inside a DBCS pair.

Use existing encoding helpers where possible:

- `IsLeadByte(UInt8 c)`
- `IsTrailByte(UInt8 c)`
- `TryDecodeDoubleByte`-style logic from the font/rich-text path

Required helpers:

```cpp
bool IsCharBoundary(const char* text, size_t len, size_t offset);
size_t PrevCharBoundary(const char* text, size_t len, size_t offset);
size_t NextCharBoundary(const char* text, size_t len, size_t offset);
size_t CountLogicalChars(const char* text, size_t len);
bool InsertTextAtCaret(Target&, std::string_view mbText);
bool DeletePreviousChar(Target&);
bool DeleteNextChar(Target&);
```

Rules:

- If `text[i]` is a valid lead byte and `text[i + 1]` is a valid trail byte, treat both bytes as one character.
- ASCII remains one byte.
- Invalid lead bytes should be handled conservatively: either replace with `?`/space or skip as one byte.
- Never split a DBCS pair when enforcing max length.
- Backspace from after a Chinese character removes both bytes.
- Left/right arrow should move by logical character, not by byte.
- Home/end can still move to byte offset `0` / `strlen`.

### 4. Composition preview

First implementation should be commit-only:

```text
IME candidate/composition UI is handled by Windows.
Only GCS_RESULTSTR is inserted into the game buffer.
```

This avoids building an in-game preedit overlay before the core input path is stable.

Optional later improvement:

- Read `GCS_COMPSTR`.
- Draw a temporary preview next to the edit caret with a custom tile or overlay string.
- Keep preview out of the real edit buffer until `GCS_RESULTSTR` arrives.

If using the native Windows IME candidate window, call `ImmSetCompositionWindow` when caret position can be resolved. Without this, candidate UI may appear in a default corner or outside the expected field.

### 5. Display refresh

After inserting committed multibyte text:

- Update the target buffer.
- Refresh the target tile/string using the original target-specific function.
- Let existing font hooks render the bytes.

Do not solve input display with a global `Tile::SetString` hook. Use global logging only for locating callsites. Final implementation should patch target-specific callsites.

Editable fields may display through ordinary `Font::PrepText`; if so, existing regular font hooks should be enough. If a specific edit field emits one byte at a time, reuse the same lead/trail staging approach used by `TileSetStringHookForQueueText`, but localize it to that edit field.

### 6. Save-name integration

For save names, separate three names:

```text
edit display string: multibyte text shown while typing
actual save key: vanilla-sanitized ASCII-safe filename
load-menu display name: co-save SVDN mapping
```

When the player commits a Chinese save name:

1. The edit field shows the multibyte candidate.
2. The save path sanitizer still produces the actual `.fos` key.
3. `CaptureSaveDisplayName(originalName, actualName)` stores the multibyte display name as pending.
4. The save callback writes `SVDN` into `.nvse`.
5. The load menu displays `SVDN` if present.

Do not allow CJK bytes into the real `.fos` filename.

## Hook plan

### Stage A: discovery hooks

Add temporary debug hooks only:

- Log `TextEditMenu` open/close and active object pointer.
- Log save-name entry/menu open/close.
- Log edit-buffer mutation functions: before/after string, caret byte offset, key code.
- Log target display refresh callsites.
- Log IME messages at low volume:
  - message type
  - result string byte length after conversion
  - active target type

Expected result:

```text
IME commit "测试" -> converted bytes C测试 in CP936 -> active target receives one insert operation -> displayed text updates
```

The exact address list should be filled in after IDA verification. Do not ship the discovery logs in normal builds.

### Stage B: commit-only input

Implement:

- WndProc subclass.
- `GCS_RESULTSTR` capture.
- UTF-16 to current-codepage conversion.
- Active target detection for save-name field.
- Insert committed bytes into target buffer.
- Refresh display.

This stage does not need composition preview.

### Stage C: DBCS editing behavior

Patch or wrap:

- Backspace.
- Delete.
- Left/right caret movement.
- Selection deletion, if supported by the target field.
- Max-length enforcement.

All operations must use char-boundary helpers.

### Stage D: additional text fields

After save-name input is stable, extend to other fields:

- Character/player name.
- TextEditMenu prompts used by mods.
- Console input only if explicitly desired; it may have different command parsing constraints.

Each field should be added only after its commit/storage path is understood.

## Configuration

Suggested config keys:

```ini
[Main]
bChineseInput=0
bChineseInputDebug=0
bChineseInputCompositionPreview=0
```

Default `bChineseInput=0` is safer until save-name input and at least one general `TextEditMenu` path are verified in game.

If enabled, Chinese input should require:

- `g_usingWinEncoding != 0`
- matching generated font extra glyphs for the selected codepage
- existing font hooks enabled

## Failure behavior

If any of these conditions fails, do not consume IME messages:

- No active editable target.
- No configured Windows codepage.
- Conversion fails.
- Target buffer/capacity/caret pointer is not verified.
- Insertion would exceed capacity and cannot be safely truncated at a DBCS boundary.

Forward the original Windows message to the original WndProc unless tNVSE successfully consumes a committed text result for a known target.

## Risks

- Fullscreen IME behavior varies by Windows version and GPU mode.
- Candidate window placement may be wrong until caret screen coordinates are known.
- Some edit fields may count bytes as characters; max length must be tested with DBCS input.
- Some game commit paths may sanitize text immediately after editing; save names are expected to sanitize actual filenames, but character names may not want that.
- If a target renders one byte at a time, Chinese text may appear as missing glyphs unless that path gets a local DBCS staging hook.
- Clipboard paste can inject text faster than normal key input and must use the same boundary/capacity checks.
- Hooking global WndProc can conflict with overlay/plugin input hooks if messages are swallowed too aggressively.

## Test plan

### CP936/GBK

- Type `测试中文输入`.
- Type mixed ASCII/CJK: `Save-测试-01`.
- Backspace over Chinese characters.
- Move caret left/right across Chinese characters.
- Insert ASCII inside Chinese text.
- Insert Chinese inside ASCII text.
- Paste Chinese text.
- Hit max-length boundary with Chinese text and verify no half character remains.
- Save with Chinese display name:
  - `.fos` stays ASCII-safe.
  - `.nvse` contains `SVDN`.
  - load menu displays Chinese.
  - load operation still uses the real save key.

### UTF-8 mode

- If `bUTF8=1`, IME commit still enters through UTF-16 and should be converted once to the configured UI codepage.
- Verify the buffer is not double-converted as UTF-8 later.

### Regression

- English-only input behaves unchanged.
- Controller/keyboard navigation still works.
- Esc/Enter commit/cancel behavior is unchanged.
- Autosave/quicksave unaffected.
- Existing rich text, terminal, quest, location, HUD rendering unchanged.

## Completion criteria

The Chinese input hook can be considered complete when:

- Chinese IME commit inserts correct multibyte bytes into at least save-name entry.
- The edit field displays Chinese while typing.
- Backspace/delete/caret movement do not split DBCS pairs.
- Save names still use vanilla-safe actual filenames.
- Load menu display uses `SVDN` mapping for Chinese names.
- No global `Tile::SetString` or WndProc debug logging remains in normal builds.
- Missing IME/config/font support falls back without breaking normal input.

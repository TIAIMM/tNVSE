# tNVSE Hook ownership and inventory

This document describes the process-wide hooks in the FalloutNV.exe 1.4.0.525
build targeted by tNVSE. The source declarations remain authoritative.

## Design adopted from the NVSE plugin ecosystem

- Better Transitions keeps member-function identity visible by passing
  `&Class::Method` directly to its `ReplaceCallEx`/`WriteRelCallEx` helpers.
- Stewie Tweaks 9.95 keeps hook code and exact `__cdecl`/`__fastcall`/naked ABI
  close to each feature. Its optional `SafeWriteRecorder` can report a write
  later replaced by another plugin.
- ShowOff's `CallDetour` records the previous call/vtable target so the hook can
  form a real predecessor chain.
- itr-nvse verifies that the live site still points to its own target before
  restoring the predecessor.

tNVSE keeps the same source-level form. Every installation remains visible in
its feature source as `WriteRelCall`, `WriteRelCallEx`, `WriteRelJumpEx`,
`SafeWrite32`, `SafeWrite32IfEqual`, or `SafeSetWindowLongPtrA`. There is no
generic installation loop or `Install*` wrapper hiding whether a site is a
CALL, entry JMP, vtable slot, atomic callback, or WndProc.

`tnvse/Src/hook_site.h` is deliberately limited to identity and rollback
metadata. It verifies expected bytes/targets and performs compare-before-
restore, but it is not a second patch engine. Both publication and restoration
ultimately use the CommonLib/xNVSE `SafeWrite` backend.

## Identity and rollback records

| Record | Exact machine form | Validation/ownership contract | Rollback contract |
|---|---|---|---|
| `RelCallHook` | existing `E8 rel32` | validate the expected target or the explicitly accepted executable predecessor | restore only if the live `E8` still calls tNVSE |
| `EntryJumpHook` | function entry changed to `E9 rel32`, optional NOP tail | verify the full original prologue and installed JMP/NOP image | restore the complete prologue only while the entry still jumps to tNVSE |
| `VTableHook` | 32-bit function-pointer slot | validate the live slot and retain an explicitly accepted predecessor | restore only while the slot still contains tNVSE |
| `InstructionCallHook` | non-CALL instruction replaced by `E8 rel32` | verify the complete original instruction/signature | restore the original instruction only while the generated CALL is still tNVSE's |
| `BytePatch` | fixed instruction/data bytes | exact original/installed image comparison | restore only from the exact installed image |
| `WindowProcHook` | Win32 subclass chain | retain the predecessor returned by explicit `SafeSetWindowLongPtrA` publication | detach only while tNVSE is the top WndProc; otherwise retain the chain |

The native accumulator callback is intentionally even more direct: its dynamic
owner can change concurrently, so the feature source publishes it with
`SafeWrite32IfEqual(address, hook, observedPredecessor)` instead of placing a
non-atomic vtable-style wrapper around the operation.

There is no standard NVSE plugin unload callback. Rollback therefore protects
failed installation transactions and the features which support dynamic
detach; it is not an assertion that the DLL can be unloaded arbitrarily.

## Complete tNVSE feature inventory

There are 67 unique process-wide sites in the tNVSE feature layer. At most 66
can be active together because the FreeType-only `0xA1BDE2` call and the 27-site
multibyte call group are mutually exclusive.

### Retail font entry and layout graph (`game/game_hooks.cpp`)

Function entries (`E9 rel32`, original call remains available through a verified
trampoline):

| Address | Original function | Typed target / ABI |
|---|---|---|
| `0xA12020` | `Font::Font` | `&FontEx::FontConstructor`, C++ member / retail `__thiscall` |
| `0xA15320` | `Font::Load` | `&FontEx::Load`, C++ member / retail `__thiscall` |
| `0xA12880` | `Font::CreateText` | `&FontEx::CreateText`, C++ member / retail `__thiscall` |
| `0xA12460` | `Font::MakeString` | `&FontEx::MakeString`, C++ member / retail `__thiscall` |
| `0xA1B020` | `FontManager::CalculateStringDimensions` | `&FontManagerEx::CalculateStringDimensions`, C++ member / retail `__thiscall` |
| `0xA12FB0` | `Font::PrepText` (multibyte mode only) | `&FontEx::PrepText`, C++ member / retail `__thiscall` |

Non-CALL instruction replacement:

| Address | Original image | Typed target / ABI |
|---|---|---|
| `0xA154D8` | `8B 50 28 FF D2` (`mov edx,[eax+28h]; call edx`) | `GetBoundedFontDataReadSize`, `__fastcall`, `ECX=BSFile*` |

Always-installed font CALL group:

| Address | Expected target | Typed target / ABI |
|---|---|---|
| `0xA18F4A` | `0xA18A30` | `&FontManagerEx::PrepText`, member / `__thiscall` |
| `0xA18F63` | `0xA19060` | `&FontManagerEx::TextDocRender`, member / `__thiscall` |
| `0xA19622` | `0xA142D0` | `&FontEx::TextDocRenderAddChar`, member / `__thiscall` |
| `0x759281` | `0xA12FB0` | `&FontEx::PrepTextForTerminal`, member / `__thiscall` |
| `0xA19C80` | `0xA19F70` | `&FontManagerEx::TextLineAddChar`, member / `__thiscall` |

FreeType-only CALL group:

| Address | Expected target | Typed target / ABI |
|---|---|---|
| `0xA1BDE2` | `0xA19F70` | `&FontManagerEx::TextLineAddChar`, member / `__thiscall` |

Multibyte CALL group:

| Address(es) | Expected target | Typed target / ABI |
|---|---|---|
| `0x6FFFEE` | `0x401460` | `CopyAnimatingTextEncodedUnits`, `__cdecl` |
| `0xA18ACC` | `0xA17390` | `&FontManagerEx::PrepHypertext`, member / `__thiscall` |
| `0xA1772D`, `0xA17835`, `0xA17A1E`, `0xA17B65`, `0xA17BB1`, `0xA17CFE` | `0xA16EA0` | `FontManagerEx::CollectTo`, static `__fastcall` thiscall shim |
| `0xA17D5D`, `0xA17DE9` | `0xA16EA0` | `FontManagerEx::CollectToAttributeValue`, static `__fastcall` thiscall shim |
| `0xA18F7D` | `0xA1B990` | `&FontManagerEx::TextDocDestructor`, member / `__thiscall` |
| `0xA178A4`, `0xA179D9`, `0xA17FC2`, `0xA18D7C` | `0xA19A10` | `&FontManagerEx::TextDocAddChar`, member / `__thiscall` |
| `0xA19A6F`, `0xA1BD1C` | `0xA19C00` | `&FontManagerEx::TextPageAddChar`, member / `__thiscall` |
| `0xA17898`, `0xA179CD`, `0xA17FB6`, `0xA18D73` | `0xA1B660` | `FontManagerEx::CharDataCopy`, static `__fastcall` thiscall shim |
| `0x77AF4B`, `0x772B5E` | `0xA01350` | `TileSetStringHookForQuestAndLocationText`, explicit shim |
| `0x7591AC` | `0x559450` | `BSString_c_strHook`, explicit shim |
| `0x772B4B` | `0x438EB0` | `BSString_GetCStringOrEmptyHook`, explicit shim |
| `0x77ACCC`, `0x77ACF8` | `0x406D30` | `strcpy_sHook`, `__cdecl` |

Other font/game sites:

| Site | Form | Typed target / policy |
|---|---|---|
| `0x1094880` | `TileText::MakeNode` vtable | `TileTextMakeNodeHook`, `__fastcall` thiscall shim; chains any executable predecessor |
| `0x777006 -> 0x406D00` | `E8 rel32` | CHS/KOR `BSsprintf` hook, `__cdecl`; exact stock target only |
| `0x753E39: 74 -> EB` | one-byte branch patch | plural handling; exact-image only |
| JIP-translated `0x100113BE` | `MOV EDX, imm32` operand | converted Big Guns description pointer; exact opcode and original pointer required |

### Native IME/prewarm overlay (`game/native_tile_overlay.cpp`)

| Site | Form | Typed target / ABI | Conflict policy |
|---|---|---|---|
| `0x7079A3` | `E8 rel32` menu factory | `CreateMenuByClassHook`, `__fastcall` thiscall shim | chain current executable target |
| `0x10780B8` | `FOPipboyManager::Draw` vtable | `PipboyRenderedMenuDrawHook`, `__fastcall` thiscall shim | chain current executable target |
| `0x78D552 -> 0x789820` | `E8 rel32` | `LoadingMenuUpdateHook`, `__fastcall` thiscall shim | chain current executable target; separately verify adjacent `0x78D557 -> 0x78D080` |

The copied private IME `Menu` vtable is object construction, not a global hook
slot, and therefore is not counted above.

### Save display names (`game/save_display_name.cpp`)

| Site | Expected/chained target | Typed target / ABI |
|---|---|---|
| `0x850545` | current executable owner (Stewie-compatible chain) | `OpenSaveFileWithSafeName`, `__fastcall` thiscall shim |
| `0x8518BB -> 0x8518D0` | stock only | `ScrubFileNameAndCaptureDisplayName`, `__fastcall` |
| `0x851AAE` | current executable owner | `IsSaveFileNameGeneratedWithDisplayName`, `__fastcall` |

The last two sites install as one transaction. A failed verification restores
only a site still calling tNVSE and never overwrites a later owner.

### Input hooks (`input/*.cpp`)

| Site | Form | Typed target / ABI |
|---|---|---|
| `0x7AB740 -> 0x7E6320` | `E8 rel32` | `TextEditMenuEx::Open`, static `__fastcall` |
| `0x7E6685 -> 0x716B00` | `E8 rel32` | `TextEditStateEx::Input`, static `__fastcall` |
| `0x1070064` | `TextEditMenu::HandleKeyboardInput` vtable | JIP adapter `JipTextInputAdapterEx::Input`, static `__fastcall` |
| `0x10739E4`, `0x1070004`, `0x1074D74`, `0x10721DC`, `0x107071C`, `0x1073D0C`, `0x10704BC`, `0x1076D4C` | eight vanilla-menu input vtables | eight `StewieMenuSearchInputTargetEx` static `__fastcall` thiscall shims |
| runtime StewMenu vtable `+0x30` | dynamic vtable | `StewieTweaksInputTargetEx::StewMenuKeyboardInput`, static `__fastcall` thiscall shim |
| `jip_nvse.dll + 0x13C59` | five-byte compare replaced by `E8 rel32` | `JipRawKeyStateCompareHook`, `__declspec(naked)`; full JIP 57.30 signature required |
| game main `HWND`, `GWLP_WNDPROC` | Win32 subclass | `MultibyteInputWndProc`, `CALLBACK` / `__stdcall` |

The fixed TextEdit CALL pair is transactional. Dynamic Stewie/JIP vtable hooks
retain the observed predecessor and refuse to republish above an unknown
successor, preventing two-node recursion.

### Native FreeType render route

| Site | Form | Typed target / ABI |
|---|---|---|
| `0x11F9FA8` | Tile render-mode callback slot | `NativeFontRegisterObject`, `__cdecl`; atomic compare/exchange |
| `0xB65EA0 -> 0xB64F90` | `E8 rel32` | `NativeFontRenderAlphaGeometry`, `__fastcall` thiscall shim; compatible predecessor chain |
| `0xB64FD1 -> 0xB994F0` | `E8 rel32` | `NativeFontRenderPassImmediately`, `__cdecl`; stock-only |

Private/cloned FreeType shape, texture and shader vtables are per-object
implementation data, not process-global hook slots, and are not counted.

## CommonLib allocator bootstrap (outside the 67 feature sites)

`commonlib_nv/Src/Bethesda/BSMemory.cpp` has two mutually exclusive runtime
descriptors. Only one can be selected, and neither is written if the engine heap
already exists:

| Runtime | Heap slot | CALL site | Typed target |
|---|---|---|---|
| FalloutNV | `0xF9907C` | `0xC62B21` | `CreateHeapStub`, `__cdecl` |
| GECK | `0x12705BC` | `0xECC3CB` | `CreateHeapStub`, `__cdecl` |

The descriptor calls the original heap creator first, records the live `E8`
target, and publishes the stub with an explicit CommonLib `WriteRelCall`.
Its compare-before-restore path uses `ReplaceCall` only while the CALL still
targets tNVSE. It is process-lifetime bootstrap state rather than a tNVSE
feature hook.

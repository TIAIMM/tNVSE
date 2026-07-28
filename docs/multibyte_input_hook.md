# FalloutNV 多字节字符输入 Hook 方案

## 目标

让游戏内可编辑文本框支持多字节字符输入，同时不要求游戏内部存储全面改成 UTF-8。

推荐的数据方向是：

```text
Windows IME UTF-16 提交串
  -> 当前 UI Windows codepage 多字节串，例如 CP936/GBK
  -> 游戏原有 char 编辑缓冲区
  -> tNVSE 现有 Font / FontManager 多字节渲染 hook
```

也就是说，输入层负责把 IME 结果变成当前 `uiEncoding` 对应的多字节文本；渲染层继续复用已有字体 hook 和 extra glyph 表。除非未来证明整条 UI 输入、存储、渲染链路都能安全处理 UTF-8，否则不要把 UTF-8 直接写入游戏编辑缓冲区。

本文只讨论“输入管线”。富文本渲染、普通字体渲染、词典翻译、存档列表显示名映射是相关前置能力，但不是同一个 hook 点。

## 当前工程状态

tNVSE 已经具备的相关能力：

- 普通文本路径通过 `FontEx` 和 extra glyph 表渲染多字节 CJK glyph。
- 富文本路径已经在 `font_manager.cpp` 中加入 DBCS-aware parser/render hook。
- Quest/location 等部分 UI 文本通过 `Tile::SetString` 局部 hook 进入现有文本转换路径。
- 存档名 sanitizer call site `0x8518BB` 已经能在实际 `.fos` 文件名被清洗前捕获原始候选名。
- 存档显示名映射已经改为 `Data\NVSE\plugins\tnvse\save_display_names.dat` 单文件 sidecar，不修改 `.fos`、`.nvse` 或 `SaveGameData::pName`。

当前已完成的输入层能力：

- Windows IME `GCS_RESULTSTR` 提交串进入当前 `uiEncoding` 对应的多字节 edit buffer。
- 原版 `TextEditMenu` 通过 `0x7E6620 -> 0x716B00` 的内部 call site `0x7E6685` 接管编辑核心，避免改写全局 vtable，保留 Confirm 分支和 Stewie Tweaks 在 `0x7E6627` 的补丁。
- 原版 `TextEditMenu` 的 ASCII 插入、IME commit、退格、删除、左右移动、Home/End 均走 DBCS-aware 编辑层。
- 玩家名输入保留 validator 特例；`0x7AB740` 只包装 `TextEditMenu::Open` 来替换玩家名 validator，不改变通用打开逻辑。
- JIP LN `ShowTextInputMenu` 已按 JIP 自定义字段布局单独处理，使用 `JipTextInputAdapterEx` 临时链回 JIP 写入 `0x1070064` 的 input handler，不再把 JIP 的 `inputRect` / `minLength` / `maxLength` 误当成原版 `TextEditState` 字段。
- Stewie Tweaks 9.90+ 已单独作为 `StewieTweaksInputTarget` 适配；不改 Stewie DLL，覆盖 StewMenu 搜索、StewMenu 字符串子设置输入，以及 Stewie MenuSearch 在常见菜单中的搜索框。
- 正式版静态 xref 显示 `0x7E6320` 当前由 `PlayerNameEntryMenu` 调用；存档名显示链路是下游保存名生成和 sidecar 映射，不应把它误写成已经确认的 `TextEditMenu` 保存名输入框。

## 参考项目结论

本节参考 `E:\NVSEStuff\ChineseInputSKSE64-master` 和 `E:\NVSEStuff\ChineseInput-F4SE-master`。两者的输入系统都不是 FNV，但可以借鉴输入法状态机和消息处理边界。

### ChineseInputSKSE64

SKSE64 项目的主要路径是“自带 Rime 后端 + 游戏输入事件注入”：

- `Hook_GameInput.cpp` hook `BSWin32KeyboardDeviceEx::ProcessKeyboardInput`，在 Rime 组字期间把方向键、翻页、回车等命令键交给 Rime，而不是让 UI 同时响应。
- `MenuControlsEx::ProcessCommonInputEvent_Hook` 在 `INPUT_EVENT_TYPE::kChar` 收到字符事件时把 `unicode` 投递给 `RimeManager`；组字期间会吞掉普通 UI key event。
- `RimeManager` 维护 `RimeIndicator`，字段包括 `composition`、`commit`、`candidateList`、当前页和高亮候选；Rime commit 是 UTF-8，项目先转 UTF-16，再逐字符发送。
- `Hook_DX11.cpp` 用 ImGui 渲染 composition 和 candidate list，避免依赖系统候选窗位置。

可借鉴的部分：

- 输入后端和游戏编辑目标解耦：先得到 commit，再按目标类型注入。
- 组字期间必须吞掉会误触发菜单的键。
- composition/candidate/commit 状态分离，commit 才进入真实文本。
- Rime 可以作为未来可选后端，但不应作为第一阶段默认依赖。

不能照搬的部分：

- `GFxCharEvent`、`UI::SendBSUIScaleformData(topMenu, ...)` 是 Skyrim/Scaleform 路径，FNV 的旧 UI 没有等价的安全注入点。
- DX11 ImGui overlay 不能直接用于 FNV；若之后需要自绘候选窗，应另写 DX9 overlay 或 tile-based overlay。

### ChineseInput-F4SE

F4SE 项目的主要路径是“Windows IME/TSF + FO4 Unicode char event 注入”：

- `Hooks.cpp` 的 WndProc hook 处理 `WM_IME_STARTCOMPOSITION`、`WM_IME_COMPOSITION`、`WM_IME_ENDCOMPOSITION`、`WM_CHAR`。
- `InputUtil.cpp` 用 `ImmGetCompositionStringW(..., GCS_COMPSTR)` 读取预编辑文本，用 `GCS_RESULTSTR` 读取最终提交文本。
- `InputUtil::SendUnicodeMessage` 把提交的 UTF-16 字符送入 FO4 的 `ProcessCharEvent`。
- `Cicero.cpp` 通过 `ITfUIElementSink` / `ITfInputProcessorProfileActivationSink` 读取 TSF candidate UI 和当前输入法状态。
- `InputMenu.cpp` 自绘 composition、candidate list 和输入法状态；`AllowTextInput_Hook` 控制什么时候启用 IME。

可借鉴的部分：

- tNVSE 的真实写入仍应只采用 `WM_IME_COMPOSITION + GCS_RESULTSTR` 的 commit-only 路径。
- `GCS_COMPSTR`、IMM32 candidate list 和 TSF/Cicero 只作为 composition preview / candidate window 增强，不应写入真实 edit buffer。
- 必须有 text-input gate：composition/候选预览只在当前输入菜单对象存在时消费；真实提交只写入已确认的原版 active `TextEditMenu`，或字段布局仍有效的 JIP `ShowTextInputMenu`。
- `WM_CHAR` 只能作为非 IME fallback，并且必须避免和 `GCS_RESULTSTR` 双插入。

不能照搬的部分：

- FO4 的 `ProcessCharEvent` 是 Unicode 输入事件；FNV 的 `TextEditMenu` 是 `char` / codepage / byte-offset 编辑模型。
- F4SE 的 D3D11/FW1FontWrapper overlay 不能直接用于 FNV。

### ChineseInput-F4SE 后续结构化改进

当前实现进一步吸收了 ChineseInput-F4SE 的统一 text-input gate 和 TSF
观察模型，但没有复制它的 `WM_INPUT` 接管、延迟候选查询线程或 Unicode
`ProcessCharEvent` 注入：

- `multibyte_input_broker.cpp` 统一解析原版 `TextEditMenu`、JIP
  `ShowTextInputMenu`、Stewie、Dialogue History 和 MCM Extender target。
  WndProc 捕获事件保存 target kind、对象身份和 session generation；主循环
  处理前必须仍匹配当前 token。输入框关闭或切换后排队中的旧
  composition/result 因此不会写入新的输入框。
- broker 只缓存主循环验证过的 target。WndProc 读取缓存 token，不为了
  target 判定扫描 Menu/Tile。原版/JIP/Stewie 等仍保留独立数据 adapter，
  UTF-16 到当前 codepage、DBCS caret 和容量检查没有被统一入口绕过。
- `FilterGameInput` 统一处理已确认 IME commit key、composition ASCII 和
  composition control 三类抑制。原版 TextEdit、JIP、Stewie、MCM 和
  Dialogue History adapter 使用同一状态机；没有接管或重放 FNV 的
  `WM_INPUT`。
- TSF sink 除 `ITfUIElementSink` 外，还实现
  `ITfInputProcessorProfileActivationSink`。profile callback 只发布
  pending flag，当前输入法名称和 overlay 状态由下一次主循环刷新。
- TSF sink 通过 `ITfThreadMgrEventSink` 跟随 focused document context，
  并用 `ITfTextEditSink::OnEndEdit` 镜像 composition range。该文本仅在
  IMM `GCS_COMPSTR` 为空时作为 preview fallback；真实 edit buffer 仍然
  只接受 `WM_IME_COMPOSITION + GCS_RESULTSTR`。
- TSF composition update 携带 text-input session generation。过期
  document/context 的回调会在主循环被丢弃；target/session 变化也会推进
  TSF candidate generation、清除预览和 commit-key latch。

## 反编译依据

本节基于正式版 `FalloutNV.exe` IDB 反编译核对。2026-07-06 追加用 IDA 9.3 `idalib` 对 `D:\Codex\NV逆向\Official Version\FalloutNV.exe.i64` 做只读导出，重点验证 `0x7E6320`、`0x7E6620`、`0x716B00`、`0x7170A0`、`0x7E6700` 和相关 helper 的伪代码/反汇编。

### 测试版 PDB 对照

Aug 22, 2010 Xbox Release Beta PDB 能提供 `PlayerNameEntryMenu` 的命名依据：

- `PlayerNameEntryMenu::IsValidName(char const*)`
- `PlayerNameEntryMenu::Finish(char const*)`
- `PlayerNameEntryMenu::PlayerNameEntryMenu()`
- `PlayerNameEntryMenu::DoIdle()`

但 Xbox 测试版的玩家名输入走 `XVirtualKeyboard`，不是 PC 正式版的 `TextEditMenu.xml`。因此当前代码只把 `PlayerNameEntryMenu` 的函数名和参数类型按测试版 PDB 恢复；`TextEditState`、`TextEditMenu` 的结构体字段、helper 地址和调用约定仍以 PC 正式版反编译为准。

PC 正式版对应关系：

| 语义名称 | 正式版地址 | 依据 |
| --- | --- | --- |
| `PlayerNameEntryMenu::IsValidName(char const*)` | `0x7AB820` | `0x7AB690` 传给 `TextEditMenu::Open` 的 validator，行为与测试版 PDB 同名函数一致 |
| `PlayerNameEntryMenu::Finish(char const*)` | `0x7AB9A0` | 写 `TESNPC + 0xD0` full name、更新 StatsMenu、关闭菜单，行为与测试版 PDB 同名函数一致 |
| `TextEditMenu::Open(char const*, char const*, ValidateTextCallback)` | `0x7E6320` | PC 正式版加载 `Data\Menus\Dialog\TextEditMenu.xml` |
| `TextEditMenu::Refresh()` | `0x7E6700` | PC 正式版刷新 edit tile string trait `4036` 并调用 validator |
| `TextEditState::InputUnk01(SInt32, SInt32)` | `0x716B00` | PC 正式版原始单字节编辑核心；真实调用约定为 `ECX=this, EDX=key, stack=char` |

### TextEditMenu 打开路径

`0x7E6320` 是当前最重要的 `TextEditMenu` 打开函数。反编译显示它会：

- 创建或切换 menu id `1051`。
- 加载 `Data\Menus\Dialog\TextEditMenu.xml`。
- 将当前 menu cast 为 `TextEditMenu`。
- 写入标题、编辑框初始文本、确认按钮文本。
- 设置编辑状态和校验回调。

关键行为摘要：

```cpp
sub_A01B00("Data\\Menus\\Dialog\\TextEditMenu.xml");
dword_11DAEC4 = DynamicCast(menu, Menu, TextEditMenu);
Tile::SetString(*(dword_11DAEC4 + 0x30), 4036, title, 1);
Tile::SetString(*(dword_11DAEC4 + 0x28), 4036, initialText, 1);
Tile::SetString(*(dword_11DAEC4 + 0x2C), 4036, okText, 1);
*(dword_11DAEC4 + 0x58) = validatorCallback;
sub_7E6700(dword_11DAEC4);
```

idalib 反汇编确认，伪代码中省略的编辑状态 `thiscall` receiver 均来自 `dword_11DAEC4 + 0x34`：

```asm
007E6499  mov ecx, dword_11DAEC4
007E649F  add ecx, 34h
007E64A2  call sub_716AA0        ; 设置宽度限制

007E64BB  mov ecx, dword_11DAEC4
007E64C1  add ecx, 34h
007E64C4  call sub_716A70        ; 设置初始文本

007E64CB  mov ecx, dword_11DAEC4
007E64D1  add ecx, 34h
007E64D4  call sub_717010        ; 激活编辑状态

007E64DB  mov ecx, dword_11DAEC4
007E64E1  add ecx, 34h
007E64E4  call sub_7E6580        ; 设置 clear-on-next-type

007E6508  mov ecx, dword_11DAEC4
007E6511  mov [ecx+58h], edx     ; validator callback
007E6514  mov ecx, dword_11DAEC4
007E651A  call sub_7E6700
```

字段语义按目前反编译可这样理解：

- `TextEditMenu` vtable：`0x1070034`。
- `RTTI_TextEditMenu`：`0x0119F704`。
- `dword_11DAEC4`：当前 `TextEditMenu` 实例指针。
- `this + 0x28`：编辑文本显示 tile。
- `this + 0x2C`：确认按钮 tile。
- `this + 0x30`：标题 tile。
- `this + 0x34`：内嵌编辑状态对象。
- `this + 0x58`：文本校验回调；`0x7E6700` 会把当前文本传给它，并把结果写到 tile float `4015`。

这些 offset 不应直接作为最终 ABI 承诺；实际代码中应封装为本地结构/适配器，并保留地址注释。

idalib 静态 xref 还确认，正式版 `0x7E6320` 只有一个代码引用：`0x7AB690`。该调用方构造 `PlayerNameEntryMenu`，读取 `TESNPC + 0xD0` 的当前名字作为初始文本，并把 `sub_7AB820` 作为 validator 传给 `TextEditMenu`。因此第一阶段已经确认的真实输入目标是玩家名 `TextEditMenu`，不是保存菜单里的自定义存档名输入框。

### TextEditMenu 构造和编辑状态

`0x71FA80` 构造 `TextEditMenu`：

```cpp
sub_A1C4A0(this);
*this = &TextEditMenu::vftable;
sub_716980(this + 13);       // this + 0x34
sub_403D30(this + 10, 0, 12);
BSStringT_char::Set(&stru_11DAED8, 0, 0);
*(this + 22) = 0;            // this + 0x58
```

`0x716980` 初始化编辑状态对象：

```cpp
BSString init at state + 0;
BSString init at state + 8;
*(state + 16) = 0;       // caret byte offset
*(state + 20) = -1;      // max pixel width, -1 means unlimited
*(state + 24) = 1;       // font id used by width validation
*(state + 28) = 0;       // caret blink timestamp
*(state + 32) = 0;       // caret blink visible flag
*(state + 33) = 0;       // edit active flag
```

idalib 导出的 `0x716980` 没有显示写入 `state + 34`；`state + 34` 是 clear-on-next-type 标志，由 `0x7E6580(state, 1)` 设置，并由 `InputUnk01` 在第一次真实输入、退格、删除等路径里清零。

相关 helper 行为：

```cpp
// 0x716AA0
state->maxPixelWidth = tileFloat4026AsUInt - 5;
if (state->maxPixelWidth < 0)
    state->maxPixelWidth = defaultTextEditWidth();

// 0x717010
if (!state->active && enable)
    state->caretByteOffset = strlen(state->text);
state->active = enable;

// 0x7E6580
state->clearOnNextType = enable;
```

`0x717230(state, candidate)` 会用 `FontManager::CalculateStringDimensions(candidate, state + 24 fontId, hugeWidth, 0)` 计算候选字符串宽度，再比较 `ConditionalFloatToUInt(width) <= state + 20`。所以 `state + 20` 不是 byte capacity，而是原版文本框宽度限制。实现多字节字符输入时仍要额外保护原版临时栈缓冲的 byte 上限。

原始反编译中可见的结构摘要：

```cpp
*(state + 16) = 0;       // caret byte offset
*(state + 20) = -1;      // max pixel width
*(state + 24) = 1;
*(state + 28) = 0;       // caret blink timestamp
*(state + 32) = 0;       // caret blink visible flag
*(state + 33) = 0;       // edit active flag
```

`0x717010(state, 1)` 会激活编辑状态，并在第一次激活时把 caret 放到当前字符串末尾。`0x7E6580(state, 1)` 会设置下一次输入前清空现有文本的状态，这解释了打开文本框后第一次输入会替换默认内容的行为。

### TextEditMenu 显示刷新

`0x7170A0(state)` 会生成用于显示的字符串：

- 复制 `state + 0` 的真实文本。
- 如果 `state + 33` 为 active，在 `state + 16` 指定的 byte offset 处插入 caret marker：
  - `0x7C` (`'|'`) 或
  - `0x7F`。
- 把结果写入 `state + 8` 并返回 `state + 8` 的 `c_str()`。

idalib 伪代码显示该函数使用两个固定栈缓冲：

```cpp
_BYTE source[1024];
char display[1028];
while (srcIndex <= sourceLen) {
    if (*(state + 33) && srcIndex == *(state + 16))
        display[dstIndex++] = *(state + 32) ? 0x7C : 0x7F;
    display[dstIndex++] = source[srcIndex++];
}
```

这说明 `state + 16` 是 byte offset，不是字符 index。多字节字符输入层必须保证它只落在合法 DBCS 边界上，同时应把真实文本限制到 `source[1024]` 可安全复制的范围内；建议最大可见文本不超过 1023 bytes，再加结尾 `NUL`。

`0x7E6700(this)` 刷新显示：

```cpp
char* display = sub_7170A0(this + 0x34);
Tile::SetString(*(this + 0x28), 4036, display, 1);
if (*(this + 0x58)) {
    char* rawText = BSStringT_char::c_str(this + 0x34);
    bool valid = (*(validatorCallback))(rawText);
    Tile::Set(4015, valid);
}
```

因此多字节字符输入 hook 在成功修改 edit buffer 后，应该调用原版刷新函数 `0x7E6700`，而不是全局 hook `Tile::SetString`。

反汇编确认 `0x7E6700` 也是先取 `this + 0x34` 调 `0x7170A0`，再对 `this + 0x28` 的 edit tile 写 string trait `4036`。如果 `this + 0x58` validator 非空，它会从 `this + 0x34` 取 raw text 传给 callback，并把结果写到当前 tile 的 float trait `4015`。

### TextEditMenu 输入处理

`TextEditMenu` vtable 第 12 项对应 `0x7E6620`。它是当前确认到的菜单输入处理入口：

```cpp
if (key == -2147483640) {
    if (Tile::GetFloat(4015))
        this->Accept(...);
    return 1;
}
if (sub_716AE0(this + 0x34)) {
    InputUnk01(this + 0x34, key, key);
    sub_7E6700(this);
    return 1;
}
return 0;
```

`0x716B00 InputUnk01(state, key, n9)` 是原版编辑核心。它明确是单字节编辑模型：

- 普通字符：如果 `n9 > 9`，只插入一个 byte。
- 插入时从 caret 位置开始逐 byte 后移一位。
- Backspace 从 `caret - 1` 删除一个 byte。
- Delete 从 `caret` 删除一个 byte。
- Left/right 只让 `state + 16` 加减 1。
- Home/end 设置为 `0` 或当前字节长度。

反汇编中最关键的证据是：

```asm
; 0x7E6620 输入路径
007E6669  mov ecx, [this]
007E666C  add ecx, 34h
007E666F  call sub_716AE0
...
007E667F  mov ecx, [this]
007E6682  add ecx, 34h
007E6685  call InputUnk01
007E668D  call sub_7E6700

; 0x716B00 普通字符插入路径
00716F52  mov ecx, insertionIndex
00716F58  mov dl, byte ptr [n9]
00716F5B  mov [buffer + insertionIndex], dl
00716FB0  mov [state + 10h], caret + 1

; 0x716B00 左右移动
00716DC0  sub edx, 1      ; left
00716DF5  add edx, 1      ; right
```

也就是说，即使 WndProc 把 GBK/Big5/SJIS/CP949 的两个 byte 分两次送进原版输入函数，原版 caret、delete、display caret 都仍然只知道 byte，不知道逻辑字符。最终实现不能通过“逐 byte 调原版 `InputUnk01`”来宣称支持多字节字符输入。

已确认的按键语义：

```text
-2147483648  Backspace
-2147483647  Left
-2147483646  Right
-2147483643  Home
-2147483642  End
-2147483641  Delete
-2147483640  Confirm / close path depending on caller
```

这就是多字节字符输入必须重写或包裹编辑操作的核心原因：原版 `InputUnk01` 会把 GBK/Big5/SJIS/CP949 的 lead/trail pair 当成两个独立字符处理。

### 保存名生成和显示链路

保存文件名最终由保存文件名生成路径创建。正式版 `0x8517C0` 反编译摘要：

```cpp
BSsprintf(out, 260, "%s %i - %s, %s, %s",
    localizedSavePrefix,
    saveNumber,
    playerName,
    locationName,
    playTime);

if (!tempSave) {
    if (FastStrlen(out) > 0xFF)
        out[255] = 0;
    return sub_8518D0(out);     // filename sanitizer
}
```

tNVSE 当前在 `0x8518BB` 替换 sanitizer call site：先捕获原始候选名，再执行原版等价 sanitizer。这里的原则仍然适用于多字节字符输入：

```text
上游可编辑文本：当前 codepage 多字节文本
实际 .fos basename：原版 sanitizer 后的 ASCII-safe 文本
载入/保存列表显示：由原版 save header 解析出的 Location - Size 摘要
中文 display candidate：写入 tNVSE sidecar，仅用于判断该存档仍应按手动存档处理
```

不要让 CJK 字节进入真实 `.fos` 文件名。

保存列表反编译确认：

- `0x851980` 用本地化 `Save ` 前缀识别标准手动存档。
- `0x846900` 载入 save header 后，如果实际 basename 不被识别为标准 manual/autosave/quicksave/systemsave，会用 raw basename 覆盖 entry offset `0x14` 的显示字符串。
- tNVSE 当前在 `0x851AAE` 的 call site 扩展手动存档识别，允许 sidecar-backed 多字节保存名继续走原版解析出的 `Location - Size` 显示。
- `0x84FF30` 构造存档目录并确保目录存在。
- `0x84FF90` 根据 actual basename 构造完整 `.fos` 路径；tNVSE 使用它确保 sidecar lookup 和游戏路径一致。

多字节字符输入不应改动这些已稳定路径。它只需要让玩家名等上游可编辑文本能以当前 codepage 多字节形式进入游戏；保存名 sidecar 继续在 `0x8518BB` 捕获下游格式化结果。

## 当前实现

### 1. 新增输入模块

已新增：

```text
tnvse/Src/input/multibyte_input.h
tnvse/Src/input/multibyte_input.cpp
```

新增配置项：

```ini
[MultibyteInput]
bMultibyteInput = 0
bMultibyteInputLog = 0
bMultibyteInputCompositionPreview = 0
bMultibyteInputHideSystemCandidateWindow = 1
bMultibyteInputUseTSFCandidates = 1
bMultibyteInputStewieTweaks = 1
bMultibyteInputMCMExtender = 1
bMultibyteInputDialogueHistory = 1
bSuppressJIPKeyEventsDuringMultibyteInput = 1
```

默认关闭更安全。启用条件：

- `[Multibyte] bEnableMultibyteFontHook=1`，且多字节字体能力通过入口签名验证并实际安装
- `uiEncoding=1, 2, 3 or 4`（`IsEastAsianUiMode()`）
- 当前字体包含对应 codepage 的 extra glyph
- 至少玩家名 `TextEditMenu` 路径已验证

初始化行为：

- 在 `LoadConfig()` 后读取上述配置。
- 在 `NVSEPlugin_Load` 中安装 hook；不从 `DllMain` 安装 WndProc。
- `bMultibyteInput=0` 时不 subclass WndProc，不安装 `TextEditMenu` hook。
- `bMultibyteInput=1` 但多字节字体能力未实际安装，或 `uiEncoding=0` 时打印一次日志并跳过初始化；FreeType 能力不满足这个依赖。
- `bMultibyteInputStewieTweaks=0` 时不检测或 hook Stewie Tweaks 输入框；该开关仍受 `bMultibyteInput` 总开关约束。
- `bMultibyteInputMCMExtender=1` 时，只有 JIP 57.30 键盘事件隔离已经成功安装、xNVSE event/console interface 可用、且 MCM Extender 的三个既有 UDF 均存在时才启用 MCM 搜索适配；不会修改 MCM Extender 文件。
- `bMultibyteInputDialogueHistory=1` 时，只有上述 JIP 隔离、xNVSE event/console interface 和 Dialogue History 的三个原 UDF 均可用时才启用搜索适配；不会修改 Dialogue History 文件。
- `bSuppressJIPKeyEventsDuringMultibyteInput=1` 时，仅在 tNVSE 已确认的多字节文本输入会话内屏蔽 JIP 57.30 的键盘 `OnKeyDown/OnKeyUp` 派发；鼠标、手柄以及尚未由 tNVSE 接管的脚本输入不受影响。

### 2. WndProc / IME 捕获

在主窗口 HWND 可用后 subclass 游戏窗口：

```cpp
SetWindowLongPtrA(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(MultibyteInputWndProc));
```

不要在 `DllMain` 中安装。所有未消费消息必须转发给原 WndProc。当前
WndProc 是捕获层，不是菜单适配层：

- 鼠标、非客户区鼠标、hit-test、cursor、capture 消息在第一条分支无条件调用前驱 WndProc；raw input、pointer、touch、gesture 等不在输入捕获 allowlist 中的消息也会在读取 tNVSE 输入状态前直接转发。
- 回调只复制消息参数、Ctrl/Win 修饰键快照以及必须在消息存活期读取的 `GCS_RESULTSTR`、`GCS_COMPSTR`、IMM candidate list，并把这些数据写入固定容量队列。回调中不查找 Menu，不遍历 Tile，不安装 hook，不写 vtable，不更新游戏 UI，也不写诊断日志。
- `WM_IME_SETCONTEXT` 是唯一同步策略分支；它只依据主循环已经发布的 input-session latch 决定是否调用 `DefWindowProc(..., lParam=0)`，不查询游戏 UI。
- 主循环先安装/维护各 keyboard-input adapter、轮询 target 状态，再排空捕获队列。目标解析、DBCS shadow 编辑、IME commit、Tile/UDF 写回、IME association 和 candidate overlay 更新都在这里完成。
- TSF `BeginUIElement` / `UpdateUIElement` / `EndUIElement` 同样只镜像 candidate 数据并设置待刷新标志；共享原生 Tile overlay 由下一次主循环刷新，TSF COM 回调不会重入 Gamebryo 菜单。

当前实际处理的消息：

- `WM_IME_COMPOSITION`：回调立即复制 `GCS_COMPSTR` / `GCS_RESULTSTR`；主循环更新预览并仅在目标仍有效时提交真实 edit buffer。有 input-session latch 时该消息由 tNVSE 消费，不再落回系统默认 composition UI。
- `WM_IME_STARTCOMPOSITION` / `WM_IME_ENDCOMPOSITION`：回调只维护必要的 composition 捕获标志并排队，完整状态清理由主循环完成。
- `WM_IME_NOTIFY`：在 `IMN_OPENCANDIDATE` / `IMN_CHANGECANDIDATE` / `IMN_SETCANDIDATEPOS` 时只通过 `ImmGetCandidateListW` 镜像旧式候选列表；现代 IME 的候选优先由 TSF `ITfCandidateListUIElement` sink 更新；`IMN_CLOSECANDIDATE` 清空 IMM32 fallback 候选。这里不能调用 `ImmSetCandidateWindow`，否则会再次触发 `IMN_SETCANDIDATEPOS` 并形成 notify 循环。
- `WM_INPUTLANGCHANGEREQUEST`：不进入捕获队列，直接交给既有 WndProc 链，让 `Win+Space` / 输入法热键切换窗口输入语言。
- `WM_INPUTLANGCHANGE`：排队到主循环刷新当前键盘布局、IME 名称和 context。
- `WM_IME_SETCONTEXT`：只使用缓存的 input-session latch；启用游戏内候选窗替代时调用 `DefWindowProc(..., lParam=0)`，否则直接转发。
- `WM_CHAR`：回调只排队并按缓存会话决定是否消费；未组字 ASCII、非 ASCII fallback、composition ASCII 抑制和真实 target 写入都在主循环处理。
- `WM_NCDESTROY`：先转发给前驱 WndProc，再丢弃待处理捕获数据并清除窗口链指针。
- `WM_PASTE`：后续可选，必须走同一套 DBCS 边界、宽度限制和 byte 上限检查。

IME 结果用 Unicode API 读取：

```cpp
HIMC context = ImmGetContext(hwnd);
LONG bytes = ImmGetCompositionStringW(context, GCS_RESULTSTR, nullptr, 0);
std::wstring text(bytes / sizeof(wchar_t), L'\0');
ImmGetCompositionStringW(context, GCS_RESULTSTR, text.data(), bytes);
ImmReleaseContext(hwnd, context);
```

然后转换：

```text
UTF-16 -> WideCharToMultiByte(g_usingWinEncoding)
```

不要把 IME 的 UTF-16 result 先转 UTF-8 再交给普通文本转换；输入管线应只转换一次。

消息消费规则：

- 正常游玩、没有任何已适配输入目标时，主循环结束 input session、取消当前 composition，并用 `ImmAssociateContext(hwnd, nullptr)` 解绑游戏窗口的 HIMC。WndProc 只读取这个缓存会话状态；它不会为判定目标而扫描菜单。
- 进入输入菜单时启动 input session latch，并用 `ImmAssociateContextEx(hwnd, nullptr, IACE_DEFAULT)` 重新绑定当前 HKL 的默认 IME context；不要恢复之前解绑时返回的旧 `HIMC`，否则 Alt-Tab 或输入法切换后可能继续使用旧输入法状态。输入会话开始时必须显式重建一次 context，即使当前窗口看起来尚未解绑，也要刷新 open/native 状态和首键 guard。
- 输入菜单会话中收到 `WM_INPUTLANGCHANGEREQUEST` 时必须交给 `DefWindowProc`，否则 `Win+Space` / 输入法热键可能不会真正切换当前窗口的输入语言；收到 `WM_INPUTLANGCHANGE` 后用该消息携带的新 `HKL` 重建默认 IME context，并刷新 overlay 的输入法名称和模式。这个新 `HKL` 只用于本次重建，不能保存为长期 fallback。
- 部分全屏/插件组合下 `Win+Space` 可能不会向游戏窗口投递 `WM_INPUTLANGCHANGEREQUEST`。当前实现额外捕获 `VK_LWIN/VK_RWIN + VK_SPACE` 及当时的 Win 键快照，在主循环确认系统没有完成切换后才调用 `ActivateKeyboardLayout(HKL_NEXT, KLF_SETFORPROCESS)` 并重建默认 IME context，避免延迟处理时读取已经松开的修饰键。
- 输入菜单会话中收到 `WM_SETFOCUS`、`WM_ACTIVATEAPP`、`WM_ACTIVATE` 回到激活状态时，也重建默认 IME context。这样 Alt-Tab 到外部程序切换输入法再回游戏时，不会继续沿用离焦前的 stale context。
- 某些 IME 在 Alt-Tab、输入法切换或刚打开输入菜单后，会先让第一个拼音字母经过游戏输入路径，然后才投递 `WM_IME_STARTCOMPOSITION` / `GCS_COMPSTR`。当前实现对与 `uiEncoding` 匹配的输入语言 layout 调用 `ImmSetOpenStatus(TRUE)`，必要时补 `IME_CMODE_NATIVE`；只要最终状态是 open/native，就刷新约 1 秒的 ASCII guard，直到 composition 正常接管。若首字母仍已经抢先进入真实 buffer，则在本次 composition 的第一条非空 `GCS_COMPSTR` 到达时，只检查一次 caret 前一字节：它必须是单字节 ASCII 且与 composition 首字符一致，才会被删除。这里按 `LANG_CHINESE` / `LANG_JAPANESE` / `LANG_KOREAN` 与 `uiEncoding` 匹配判定，不依赖 `ImmIsIME()`，因为 Windows 10/11 的 TSF 输入法不一定稳定通过该 API 表现为 legacy IME。
- `IsConfiguredImeLayout` 只能按当前窗口线程 `HKL`，或 `WM_INPUTLANGCHANGE` 本次传入的 `HKL`，判断是否匹配当前 `uiEncoding`。不能用“最近一次中文/日文/韩文 HKL”兜底；否则切到系统英文 `00000409` 后仍会继承中文 IME 的 open/native 状态，导致 overlay 显示 `00000409 ON 中文 半角`，并把普通 ASCII 当作拼音吞掉。
- 有 overlay target 且原生 Tile 宿主已经就绪时，`WM_IME_COMPOSITION` 总是返回已处理，避免系统默认预编辑小窗绘制；只有 `GCS_RESULTSTR` 成功转换且目标仍可写时才修改真实文本。
- `GCS_COMPSTR` 不写 edit buffer；`bMultibyteInputCompositionPreview=1` 时只写入游戏内预览。
- `WM_CHAR` 在 active `TextEditMenu` 下直接处理可打印 ASCII；若游戏输入管线随后又发出同一 ASCII input，`0x7E6620` 内部输入 hook 会用短期 suppress 防止双插入。`WM_IME_CHAR` 在输入菜单存在时直接消费，避免 IME result 又走一次系统字符路径。
- IME composition active，或 IMM context 处于 open/native 且存在预编辑串时，ASCII `WM_CHAR` 视为拼音/假名等预编辑输入并直接消费，不进入真实 edit buffer。
- 游戏原本 `TextEditMenu::HandleKeyboardInput` 路径也必须应用同一规则；正式版日志确认拼音字母可能先从该 vtable 路径到达，而不是只从 `WM_CHAR` 到达。
- `TextEditMenu::HandleKeyboardInput` 在 composition active 时还应吞掉 Backspace/Delete/Left/Right/Home/End/Confirm 等控制输入，避免用户编辑 IME 预编辑串时误删或提交游戏真实文本。
- `GCS_RESULTSTR` 后用短期 suppress 计数避免同一提交又以 `WM_CHAR` 形式插入一次。
- 所有未明确消费的消息都调用原 WndProc。

### 2.1 游戏内 IME 状态和候选窗预览

当前实现新增 `ImeCandidateState`，字段包括：

```cpp
bool composing;
bool imeOpen;
DWORD conversionMode;
DWORD sentenceMode;
DWORD selection;
DWORD pageStart;
DWORD pageSize;
bool candidatesFromTsf;
std::wstring imeName;
std::wstring composition;
std::vector<std::wstring> candidates;
```

预览层只镜像系统 IME 状态，不接管候选选择逻辑：

- `composition` 来自 `ImmGetCompositionStringW(..., GCS_COMPSTR, ...)`，它只代表拼音/假名等预编辑串，不是候选汉字列表。
- `candidates` 优先来自 TSF `ITfCandidateListUIElement`；TSF 初始化失败、关闭或未返回候选时，再用 `ImmGetCandidateListW` 作为 fallback，最多显示 9 项。
- `imeName` 优先通过 TSF active profile description；失败时退回当前 `HKL` 的 `ImmGetDescriptionW`；再失败退回 `GetKeyboardLayoutNameW` / `IME`。
- `imeOpen/conversionMode/sentenceMode` 来自 `ImmGetOpenStatus` 和 `ImmGetConversionStatus`，但只有当前 `HKL` 匹配 `uiEncoding` 时才采信；系统英文等不匹配布局即使旧 HIMC 仍报告 open/native，也按 `OFF` 处理。
- composition、候选和状态文本在主循环中转换为当前 UI codepage 后写入原生 `TileText`；只有 `GCS_RESULTSTR` 的真实提交结果才以 `WC_NO_BEST_FIT_CHARS` 转码后写入 edit buffer。

当前预览实现使用共享原生 Tile 宿主：

- `FontPrewarmOverlay.xml` 仍是启动期挂到 `pMenuRoot` 的单根 `rect` 组件；`ImeOverlay.xml` 则是 code `0x544E56`（十进制 `5525078`，ASCII “TNV”）的独立原生 `<menu>`。该值远离原版及常见插件使用的 `1001-1084` 区间，同时小于 `2^24`，经 XML 的单精度 `float` trait 传递后仍能精确还原为整数。tNVSE 链式包装 `Interface::CreateMenuByClass` 的唯一调用点，只在解析自身 IME XML 的动态作用域内为该 code 创建一个由原版 `Menu` 构造函数初始化的最小对象，其他 class 和作用域全部交给当时的前驱工厂。该对象复制原版 Menu vtable，只覆盖 `GetID()`，析构、Tile 绑定和父根注册仍走原版生命周期。
- 逆向确认 `Menu::SetMenuByClass`、`TileMenu::PostParse` 的 `pMenusVisible` 写入和 `Menu::~Menu` 清理都先检查 `1001-1084`/`xMenuList` 容量；超范围 code 只是不进入原版按 code 索引的全局表，不会越界。IME 服务始终从 `pMenuRoot` 直接持有并验证自己的根，不调用 `GetMenuByType`，因此不依赖该全局表。运行时仍扫描直接 Menu 子节点；若已有同 code 的非 tNVSE 根则 fail-open，保留系统候选窗。
- IME Menu 使用 `&does_not_stack;`、根及全部子节点 `target=false`。它是 `pMenuRoot` 的直接 Menu 子节点，因此由 `Interface::IsolateMenuElements` 按原版屏幕/Pip-Boy pass 隔离，不继承 `HUDMainMenu` 在菜单、哔哔小子 render-to-texture 或 glow/noglow pass 中的临时裁剪状态，也不会进入活动菜单栈或接管鼠标。
- 两棵树分别维护宿主身份和 ready/fail 状态；预热 XML 失败不会禁用 IME，IME XML/工厂安装失败也不会取消字体预热。只有 IME Menu 完整解析且 `GetID()==0x544E56` 时才允许隐藏系统候选窗。
- 根身份变化时只丢弃旧指针并在新根上重新加载，绝不接触可能已经释放的旧树；同一根下每次主循环还验证组件及全部固定命名子节点的父子身份，若被其他 UI reload 替换则重新解析或重载。成功加载但节点不完整的组件立即销毁，正常 shutdown 也在确认父子身份后销毁组件。
- XML 预先定义状态行、composition 行、9 个候选槽和 9 个高亮槽。未使用槽只切换 `visible`，运行期间不反复创建或销毁 Tile。`TileText` 的 `<height>` 不用于强制缩放字体；服务在写入文本后读取字体槽 1 回写的真实高度，以可见行最大高度加间距重排文本、高亮和根背景，因此大字号/VUI+ 字体配置不会按固定 24 像素行距重叠。
- 所有字符串、选择状态、尺寸和可见性 trait 只在 `kMessage_MainGameLoop` 更新，并位于 TSF/IMM 状态泵和输入目标同步之后。WndProc 与 TSF callback 只更新受保护的候选快照及 dirty 标志。
- 候选根通过只读扫描 `pMenuRoot` 的其他直接 Menu 子节点，按原版 `menu root depth + Menu::iMenuThickness + 2` 规则抬到前景；扫描排除 IME Menu 自身，不会调用 `0xA1DFB0 Menu::GetMaxDepth`。逆向确认该函数同时改写 cursor Tile 的 `depth` 和 cursor NiNode 的 Y 平移，原版只在 Menu 创建/显示生命周期边界调用，不能把它当作普通查询用于候选内容刷新。IME Menu 的 `iMenuThickness` 设为负值，使原版以后创建菜单时也不会把 overlay 深度反向计入最大菜单深度。服务每帧可重新计算目标深度，但只有数值实际变化时才写入根 trait。
- 服务是 IME 根 `visible` 的唯一状态源。渲染隔离或其他 pass 暂时裁剪 NiNode 时，不会反读根 trait 并清空 `imeVisible`/内容 key；只有输入目标、IME open 状态、预热屏障、树身份或显式隐藏请求能够结束显示。
- 文本使用原生字体槽 1，由当前 tNVSE 字体配置接管；候选层不再持有独立 FreeType library、D3D texture、state block 或屏幕空间 quad。
- 正常游玩期没有输入菜单对象时，tNVSE 不只是隐藏系统 IME UI，而是解绑游戏窗口 IME context；这会阻止系统在左上角绘制 composition 小窗，也避免拼音预编辑串干扰快捷键/普通游玩。输入菜单对象存在时用 input session latch 保持 IME context enabled，并在输入语言变化后重建默认 context，因此 `Win+Space` / Alt-Tab 后切换输入法不会继续沿用旧 `HIMC`。
- 只有原生 Tile 宿主就绪时，`WM_IME_SETCONTEXT` 才以 `lParam=0` 交给 `DefWindowProc`，并在 composition/setcontext 入口把 IMM32 composition/candidate window 移到屏幕外；`WM_IME_COMPOSITION` 也在这一条件下返回 0。
- TSF `ITfUIElementSink::BeginUIElement` 只有在调用链传入的 `*pbShow` 原本为真、同一配置开启、原生宿主就绪且当前输入目标仍有效时，才记录该 UI element 并设置 `*pbShow = FALSE`。XML 缺失或解析失败时，系统候选窗保持可见，且只记录一次明确错误。
- overlay 的显示 gate 只要求当前 `TextEditMenu` 对象仍存在并且 IME 处于 open 状态，不要求 `TextEditState::IsActive()` 为 true。这样用户把编辑文本删空、validator 暂时禁用 OK 按钮、或原版 edit state 短暂切换状态时，composition/candidate overlay 不会被误隐藏。
- 当前菜单对象丢失、IME 关闭或预览配置关闭时只隐藏共享候选根，不修改 active text input 自身的 Tile。

### 3. Active target 追踪

IME result 只有在已知可编辑目标有效时才允许写入。当前实现覆盖通用 active 原版 `TextEditMenu`：只要 `dword_11DAEC4` 指向 `TextEditMenu` vtable、`TextEditState` active，且 vtable 输入槽仍指向原版 `0x7E6620`，就允许 WndProc 和函数体输入 hook 接管输入。

- `0x7AB740` 是 `PlayerNameEntryMenu` 调用 `TextEditMenu::Open` 的 call 指令；当前只包装它来替换玩家名 validator。
- 包装函数调用原版 `TextEditMenu::Open(0x7E6320)`，并在原始 validator 是 `PlayerNameEntryMenu::IsValidName(0x7AB820)` 时替换为 DBCS-aware validator；原因是原版 validator 按单字节查 base font 宽度，DBCS high/trail byte 会导致 OK 按钮保持 disabled。
- `0x1070064` 是 `TextEditMenu` vtable 第 12 项；tNVSE 不再改写该 vtable 槽，避免和 JIP `ShowTextInputMenu` 生命周期 hook 冲突。
- 当前改为替换 `0x7E6620` 内部的 `InputUnk01` call site `0x7E6685`，让原版 confirm 分支和 Stewie Tweaks 在 `0x7E6627` 的补丁继续保留。
- `TextEditStateEx::Input` 接管 ASCII 插入、Backspace、Delete、Left、Right、Home、End；Confirm 仍由原版 `0x7E6620` 分支处理。
- WndProc 每次使用前校验 `dword_11DAEC4` 当前对象仍是 `TextEditMenu` vtable，`0x1070064` 仍是 `0x7E6620`，且 `sub_716AE0(target + 0x34)` 为 true。
- 注意：上述严格 active 校验用于原版 `TextEditMenu` 真实文本写入和编辑键处理。IME overlay/candidate 刷新使用更宽松的 `GetOverlayTextInputMenu()`，只确认当前输入菜单对象存在；因此候选窗跟随“当前输入菜单存在”，而不是跟随“当前可提交文本 active”。若只有 overlay target 而真实 edit target 暂时不可写，且当前 `HKL` 与 `uiEncoding` 匹配，composition/candidate/IME native 状态会吞掉 ASCII，避免拼音或选词数字泄漏到原 handler。若当前布局是系统英文等不匹配布局，则不吞 ASCII，保证普通字母输入仍可用。JIP `ShowTextInputMenu` 的 `GCS_RESULTSTR` 额外允许在 `+0x34` 之后的 JIP 字段布局仍有效时写入，因为 JIP 的 `+0x55 isActive` 在 IME 组字期间可能短暂为 false。
- 通过 `this + 0x34` 访问编辑状态对象。
- 插入文本后调用 `0x7E6700(this)` 刷新显示和校验状态。

当前代码通过 `TextEditMenu` / `TextEditState` 结构体访问字段，等价适配层语义如下：

```cpp
struct MultibyteInputTarget
{
    void* owner;              // TextEditMenu*
    void* editState;          // owner + 0x34
    BSStringT<char>* text;    // editState + 0
    BSStringT<char>* display; // editState + 8
    UInt32* caretOffset;      // editState + 0x10
    SInt32* maxPixelWidth;    // editState + 0x14, -1 means unlimited
    UInt32* fontId;           // editState + 0x18
    UInt8* caretVisible;      // editState + 0x20
    UInt8* active;            // editState + 0x21
    UInt8* clearOnNextType;   // editState + 0x22
};
```

不要让 WndProc 直接硬写一堆 offset；当前 offset 集中在 `ui_decode.h` 的结构体定义和 `multibyte_input.cpp` 的 helper 中。helper 负责：

- 校验 target 是否仍 active。
- 读取 raw text 和 caret。
- 执行 DBCS-aware 编辑。
- 调 `0x717230` 或等价宽度计算做候选文本验证。
- 用 `TextEditState::SetText(0x716A70)` 写回真实文本。
- 调 `0x7E6700(owner)` 刷新显示和 validator。

### 3.1 JIP ShowTextInputMenu 适配

JIP LN 的 `ShowTextInputMenu` 复用 `TextEditMenu` vtable 和 `dword_11DAEC4`，但把 `TextEditMenu + 0x34` 之后的字段按自己的文本输入结构解释。因此不能用原版 `TextEditState` helper 处理 JIP 菜单。

JIP 源码和反汇编对应的字段布局：

```cpp
struct JipTextInputView
{
    Tile* editText;              // +0x28
    Tile* okButton;              // +0x2C
    Tile* title;                 // +0x30
    BSStringT<char> currentText; // +0x34
    BSStringT<char> displayed;   // +0x3C
    UInt32 cursorIndex;          // +0x44, byte offset
    UInt16 minLength;            // +0x48
    UInt16 maxLength;            // +0x4A
    Tile* inputRect;             // +0x4C
    UInt32 cursorBlink;          // +0x50
    UInt8 cursorVisible;         // +0x54
    UInt8 isActive;              // +0x55
    UInt8 miscFlags;             // +0x57
    Script* callback;            // +0x58
};
```

当前实现的 JIP 策略：

- 当 `TextEditMenu::GetCurrent()` 仍是 `0x1070034` vtable、`currentText/displayedText` 已初始化、`+0x4A` max length 合法、`+0x4C` inputRect 非空，且 `0x1070064` 不再是原版 `0x7E6620` 时，判定为 JIP TextInput 存储布局有效；普通编辑键接管仍要求 `+0x55` active。
- 首次检测到 JIP active 时保存 `0x1070064` 当前值作为 `s_jipOriginalInputHandler`，再把 `0x1070064` 临时写成 `JipTextInputAdapterEx::Input`。
- `JipTextInputAdapterEx::Input` 自己处理 ASCII、多字节 commit 后的缓冲插入、Backspace/Delete/Left/Right/Home/End/PageUp/PageDown，并在 composition active 时即使 `+0x55 active` 暂时为 false 也吞掉拼音 ASCII 和候选选择数字/控制键，防止退回 JIP 原 handler 后把预编辑串写进 `currentText`。
- Enter 在 `miscFlags & 2` 时链回 JIP 原 handler，保留 JIP 的 OK 按钮、关闭菜单和脚本 callback 语义。
- JIP 的刷新不调用原版 `TextEditMenu::Refresh(0x7E6700)`；当前代码按 JIP 字段重建 `displayedText`，更新 edit tile `string`、OK tile `target`，并同步 `inputRect user1 -> user2`。
- JIP 关闭时它自己的 close hook 会恢复 `0x1070064` 到 `0x7E6620`；tNVSE 在 main loop 中看到槽位恢复后清掉保存的原 handler。

这个 adapter 的核心边界是：只在 JIP 字段布局有效时临时接管 JIP 的 input handler；真实普通编辑仍要求 JIP active，IME commit 则允许在布局有效但 active flag 短暂为 false 时写入。不改变 JIP 的打开、关闭、脚本回调、XML 布局或原版 `TextEditMenu` 路径。

### 3.2 Stewie Tweaks 输入适配

Stewie Tweaks 的搜索框不是原版 `TextEditMenu`，也不是 JIP 的 `ShowTextInputMenu` 字段布局。tNVSE 因此把它作为第三类 target：`StewieTweaksInputTarget`。这个 target 只复用统一的 IME 捕获、TSF/IMM32 候选窗、UTF-16 到当前 codepage 转换、DBCS 边界 helper；不复用原版 `TextEditState` 或 JIP adapter。

启用条件：

- `[MultibyteInput] bMultibyteInput=1`。
- `[MultibyteInput] bMultibyteInputStewieTweaks=1`。
- NVSE 插件表中存在 `lStewieAl's Tweaks`，且版本大于等于 9.90 / `990`。低于该版本不主动启用，避免字段布局或 vtable patch 变化导致误写。

当前覆盖范围：

- StewMenu `MENU_ID=1069` 的 `STW_SearchBar`，tile id `5`。
- StewMenu 字符串子设置输入，文本 tile id `103`，并额外扫描 Stewie `InputField` 的 `inputType`，只在 `inputType == 0` 的 string 输入项接管。
- Stewie MenuSearch XML 的搜索 tile id 为 `87698483`，但 Gamebryo 把数值 trait 保存为单精度 `float`，其运行时整数值为 `87698480`。所有按 id 查找都先执行相同的 float 规范化；直接比较 XML 整数会导致搜索 Tile 永远无法被发现。当前 hook 的菜单包括 Inventory、Stats、Map、Container、Barter、LevelUp、Recipe、Save/Load `StartMenu`。
- 数字、浮点、十六进制、Hotkey 输入不接管；RaceMenu preset 文件名输入也不在本轮范围内。

hook 策略：

- MenuSearch 使用 Stewie 已经写入的菜单 keyboard handler 入口做链式 hook；tNVSE 保存当前 handler 作为 original，再写入自己的 wrapper。
- MenuSearch Tile 只从 `kMessage_MainGameLoop` 中已打开的菜单树按 id 发现；不再 hook `Tile::ReadXML`，也不从 XML 解析、WndProc 或 TSF 回调中遍历 Tile。
- 菜单相关写入点仅为 `HandleKeyboardInput` 槽：八个 MenuSearch 菜单使用其固定 keyboard vtable entry，StewMenu 使用菜单实例 vtable `+0x30`。安装与稳定性检测全部在主循环。
- StewMenu 是自定义菜单，目标校验使用虚函数 `Menu::GetID()` 返回的 `1069`，不要用原版菜单常用的 `uiID` 字段假设。
- StewMenu 的 handler 由菜单实例 vtable `+0x30` 动态定位，菜单打开后链式替换；不是写死一个全局原版地址。
- StewMenu 搜索框和字符串子设置都优先从菜单对象内的 `InputField` 反查 active 状态和 `inputType`。搜索框 id `5` 反查失败时才回退到 root `_IsSearchActive` / tile `_IsActive`；字符串子设置 id `103` 会在列表项模板中重复出现，必须反查到 active `InputField` 且 `inputType == 0` 才接管。
- `Ctrl` 组合键直接链回 Stewie original，并清掉 tNVSE shadow，保留 `Ctrl-F`、`Ctrl-R` 等 Stewie 原行为。
- MenuSearch 的会话状态以已经链回 Stewie 且返回 handled 的 `Ctrl-F`/`Ctrl-R` 结果为准，并在菜单关闭、菜单对象消失或搜索 Tile 被真正替换时结束。搜索 Tile 的 `visible`/`alpha` 是 Stewie 刷新筛选和 Gamebryo pass 可能瞬时改写的渲染 trait，只用于诊断，不能作为每帧输入会话存活条件；否则 composition 开始后一次瞬时 `visible=0` 就会触发 target 丢失、候选层隐藏和下一帧重新激活，形成高频闪烁。

`StewMenu::subSettingInput` 使用 Stewie 自己的 `InputField`，当前按 Stewie 9.90+ 源码布局只读判断：

```cpp
struct InputField
{
    Tile* tile;          // +0x00
    String input;        // +0x04, char* + UInt16 length + UInt16 capacity
    bool isActive;       // +0x0C
    bool isCaretShown;   // +0x0D
    SInt16 caretIndex;   // +0x0E
    UInt32 lastCaretUpdateTime; // +0x10
    UInt8 inputType;     // +0x14, 0=string, 1=int, 2=float, 3=hex
};
```

只有 `isActive == true` 且 `inputType == 0` 时接管；否则交还 Stewie 原 handler。

编辑策略：

- tNVSE 为当前 Stewie target 维护一份 shadow buffer，shadow 的 caret 是 byte offset，但移动和删除都按 DBCS 边界处理。
- ASCII、IME commit、Backspace、Delete、Left、Right、Home、End 都先修改 shadow。
- shadow 修改后，tNVSE 用 Stewie original handler 清空当前 Stewie 输入框，再按 byte 重放完整 shadow，最后把 caret 左移回目标位置。这样 Stewie 自己的刷新、过滤、确认逻辑仍由原 DLL 执行，tNVSE 不复制它的搜索实现。
- 组合输入期间，拼音/假名 ASCII 和候选选择键由 Stewie target 吞掉；只有 `GCS_RESULTSTR` 成功转成当前 `uiEncoding` codepage 后才写入 shadow。
- 原生 Tile candidate overlay 的 target gate 也包含 Stewie target，因此候选窗可以跟随 StewMenu / MenuSearch 搜索框显示。

这个 adapter 的边界是：不修改 Stewie DLL，不改 Stewie 菜单 XML，不接管搜索匹配语义。Stewie 仍然按它自己的 codepage byte substring 逻辑过滤列表；tNVSE 只保证输入框里的多字节文本不会被按单 byte 删除、移动或被 IME 预编辑串污染。

### 3.3 JIP OnKeyDown/OnKeyUp 隔离

JIP LN 57.30 的 `LN_ProcessEvents` 不读取菜单 handler 的返回值，而是直接轮询 xNVSE `DIHookControl::rawState`。因此 Stewie、原版 `TextEditMenu` 或 JIP `ShowTextInputMenu` 已经消费文本按键时，其他脚本注册的 JIP `OnKeyDown` 仍可能同时收到同一个物理键。

`bSuppressJIPKeyEventsDuringMultibyteInput=1` 使用固定版本兼容 hook：

- 只接受插件表版本 `5730`，并校验 JIP `.text` RVA `0x13C59` 开始的固定操作码以及按实际模块基址重定位后的 `lastKeyState` 操作数；任一条件不符都不修改 JIP。
- 把五字节 `cmp byte ptr [ecx+eax+4], 0` 替换为相对调用，保留原寄存器，并为后续 `JE` 恢复等价零标志。
- 文本输入会话激活时，读取真实 `rawState`，同时把同一状态写入 JIP RVA `0x772E0` 的 `lastKeyState[key]`。JIP 后续比较因此看不到需要派发的边沿，但游戏、Stewie 和其他 DirectInput 读取者仍看到真实状态。
- 会话结束时快照仍按下的键，并持续静默同步到物理松开，避免关闭输入框的按键变成延迟 `OnKeyDown`，或只产生没有对应按下事件的 `OnKeyUp`。
- 只过滤 DirectInput 键码 `<256` 的键盘项；JIP 鼠标和控制器事件保持原样。

这个 hook 屏蔽的是 JIP 的全局键盘事件观察点，不修改具体模组脚本。启用 MCM Extender 或 Dialogue History 适配后，对应搜索会话也会打开此过滤条件，由独立 target 接管键盘文本编辑；鼠标和手柄仍由原模组脚本处理。

### 3.4 MCM Extender 搜索适配

`bMultibyteInputMCMExtender=1` 增加第四类独立 target，完全由 tNVSE 运行时接入：

- target guard 与 MCM Extender 原 `MenuSearchInput.gek` 对齐：控制台未打开、JIP 语义的顶部活动菜单为 `StartMenu`（1013），并且 `_MCMExt+Search == 1`。顶部菜单判定同时检查 `pActiveMenu` 和 `InterfaceManager::menuStack`，不会在 StartMenu 可见但 `pActiveMenu == nullptr` 时错误结束会话。
- 不再把 `_MCMExt+Open`、`MCM/_TargetMain` 或 `MCM/MCM_Search` 的瞬时可见性当作输入存活条件；`MenuSearch.gek` 重建列表和重置 MCM 内部状态时会短暂改变这些值，原 MCM 输入 guard 也没有依赖它们。
- 激活前要求固定版本 JIP 57.30 键盘事件隔离成功安装，避免 MCM 原 `SetOnKeyDownEventHandler` 与 tNVSE 同时写入；不满足条件时保留 MCM 原输入路径。
- tNVSE 维护当前 codepage byte buffer 和 byte caret；Backspace、Delete、Left、Right、Home、End 均通过 `PrevCharBoundary` / `NextCharBoundary`，不会拆分 DBCS lead+trail。
- `WM_CHAR` 负责普通 ASCII，`GCS_RESULTSTR` 负责 IME 提交；composition ASCII、重复 `WM_IME_CHAR` 和输入法切换热键 Space 继续走统一抑制逻辑。
- `_search_text_1`、`_search_cursor_1`、`_search_cursor_2` 由 tNVSE 更新。WndProc/IME 回调只复制输入数据；主循环排空事件时修改本地 shadow 并排队，Tile 写入、`MenuSearch.gek` 及 Esc/Tab/Ctrl-F/Enter UDF 调用也只在主循环执行，避免在 Windows/TSF 回调中重入 MCM 的列表重建和 quest-stage 状态重置。
- StartMenu 键盘处理链会在 MCM target 活动时消费同一物理键对应的 DirectInput 文本和编辑副本，避免 Stewie MenuSearch 或原版 StartMenu 再处理一次。`WM_KEYDOWN` 是 Backspace、Delete、Left、Right、Home、End 和 Enter 的唯一键盘编辑来源，Windows 的重复 `WM_KEYDOWN` 负责按住连发；不再用时间窗把延迟到达的 DirectInput 副本误判为第二次编辑。上下翻页等无关输入以及鼠标、手柄路径仍保留给原处理器。
- Esc、Tab、Ctrl-F 和 Enter 通过另外两个私有 runtime event 调用原 `OnKeyDown.gek` / `MenuSearchInput.gek`；不会复制这些控制键的脚本语义。
- 不安装 `Sv_Find` hook。匹配仍是 xNVSE/MCM Extender 原有的 byte substring 行为；该适配保证输入和编辑的 DBCS 完整性，但不改变高位 byte 的大小写折叠或 trail-byte 起点匹配语义。
- 不写入、替换或生成 MCM Extender 的 `.gek`、XML、ESP 或其他文件。

### 3.5 Dialogue History 搜索适配

`bMultibyteInputDialogueHistory=1` 为 Dialogue History 的 NPC 名称搜索增加独立 target：

- 仅在控制台关闭、JIP 语义的顶部菜单为 `StartMenu`（1013）、`_DiaHist+Visible == 1` 且 `_DiaHist+Search == 1` 时激活；要求 `DialogueHistory/Search` 的 `_search_text_1`、`_search_cursor_1` 和 `_search_cursor_2` traits 存在。
- 激活前要求 JIP 57.30 键盘事件隔离和 Dialogue History 原 `Search.gek`、`SearchInput.gek`、`OnKeyDown.gek` 均可用；缺少任一条件时不安装部分适配，保留 mod 原输入路径。
- 输入缓冲区使用当前 `uiEncoding` 对应的 Windows codepage，光标移动、Backspace 和 Delete 只落在完整 DBCS 字符边界上。`WM_CHAR`、IME commit、composition echo 与输入法切换 Space 使用和 MCM target 相同的统一输入规则。
- WndProc 只捕获输入数据，shadow、Tile 文本和光标均在主循环更新；最后一次文字变更后等待 500 ms，再通过私有 runtime event 调用原 `DialogueHistory\Search.gek`，保留 `*DiaHist_Search` 的防抖语义。Enter、Tab、Ctrl-F 会先强制提交待处理搜索，再分别调用原 `SearchInput.gek` / `OnKeyDown.gek`。
- StartMenu DirectInput 文本与编辑副本仅被消费，键盘编辑及按住连发只由 `WM_KEYDOWN` 驱动；鼠标、控制器和无关 StartMenu 输入继续交给原处理器。
- 复用已经过 handler 稳定期安装的 StartMenu 链，不为 Dialogue History 安装新的固定地址 hook；不会形成 tNVSE 与 Stewie handler 互为前驱的递归链。
- 不安装 `Sv_Find` hook，NPC 名称过滤仍由 Dialogue History 原 `Search.gek` 完成；不修改该 mod 的 `.gek`、XML、JSON 或其他文件。

### 4. DBCS-aware 编辑层

需要复用现有 encoding helper：

- `TryDecodeDoubleByte`
- `IsLeadByte`
- `IsTrailByte`

当前已经实现这些边界 helper：

```cpp
bool IsCharBoundary(const std::string& text, size_t offset);
size_t PrevCharBoundary(const std::string& text, size_t offset);
size_t NextCharBoundary(const std::string& text, size_t offset);
size_t ClampToPrevBoundary(const std::string& text, size_t offset);
bool InsertTextAtCaret(TextEditMenu*, std::string_view mbText);
bool DeletePreviousChar(TextEditState&);
bool DeleteNextChar(TextEditState&);
bool MoveCaretPrevious(TextEditState&);
bool MoveCaretNext(TextEditState&);
```

规则：

- 有效 DBCS lead+trail 作为一个逻辑字符。
- ASCII 仍是一个 byte 字符。
- 无效 lead byte 保守按单 byte 处理，不跨越未知内存。
- `state + 0x10` 在任何操作后必须是 `IsCharBoundary(text, len, caret)`。
- 写回前必须保证真实文本长度不超过 1023 bytes，避免 `0x7170A0` 的 `source[1024]` 栈缓冲溢出。
- 如果 `state + 0x14 != -1`，写回前必须调用 `0x717230(editState, candidate)` 或复用同等 `FontManager::CalculateStringDimensions` 校验；失败时拒绝插入，不做自动截断。
- Backspace 删除前一个逻辑字符。
- Delete 删除后一个逻辑字符。
- Left/right 按逻辑字符移动。
- Home/end 可直接到 byte offset `0` / `strlen`。
- 插入和截断必须保证最终字符串不以半个 DBCS 字符结尾。
- `clearOnNextType` 为 true 时，第一次插入应先清空真实文本、caret 置 0、清掉该标志，然后再插入 commit。

不要直接调用原版 `InputUnk01` 插入中文。`InputUnk01` 只适合 ASCII fallback。

### 5. 显示和 caret marker

原版显示刷新 `0x7170A0` 会把 caret marker 插入到 byte offset。多字节字符输入层必须保证 `state + 0x10` 永远位于合法字符边界，否则 caret marker 会插入 lead/trail 中间，渲染路径会出现乱码或丢字。

如果 edit field 走普通 `Font::PrepText`，现有普通字体 hook 应能渲染多字节文本。如果某个字段逐 byte 发射字符，需要复用富文本/Tile 文本中已验证的 lead/trail staging 思路，但应局部作用于该字段，不要全局 hook `Tile::SetString`。

### 6. 保存名集成

多字节字符输入本身不应修改保存名生成逻辑。正式版 `TextEditMenu` 当前确认的是玩家名输入；玩家名、地点名等多字节文本之后可能进入保存名格式化路径。保存名下游行为应保持当前稳定设计：

1. `TextEditMenu` 中显示多字节玩家名或其他已确认可编辑字段。
2. 保存生成路径 `0x8517C0` 仍拼出原始候选名。
3. `0x8518BB` hook 捕获原始候选名。
4. 原版等价 sanitizer 继续生成 ASCII-safe `.fos` basename。
5. `CaptureSaveDisplayName(originalName, actualName)` 生成 pending record。
6. `kMessage_SaveGame` 用实际 `.fos` path 更新 `Data\NVSE\plugins\tnvse\save_display_names.dat`。
7. 保存/读取列表中，`0x851AAE` 手动存档识别 hook 只阻止 raw basename 覆盖原版 save header 摘要。

注意：sidecar 中的 display name 是当前 UI codepage 多字节，不是 UTF-8。若输入源是 UTF-8 或 IME UTF-16，都应在进入 record 前转换为当前 codepage。

## Hook 阶段

### 当前诊断日志

设置 `[MultibyteInput] bMultibyteInputLog=1` 后，会打印
`tnvse_multibyte_input_event` 日志，用于区分输入来源：

- `source=WndProc.WM_CHAR`：Windows 字符消息路径。
- `source=WndProc.WM_IME_COMPOSITION`：IME composition/result 路径。
- `source=TextEditState::Input`：原版 `TextEditMenu::HandleKeyboardInput(0x7E6620)` 内部 `InputUnk01(0x716B00)` call site 路径。
- `source=JipTextInputAdapter::Input`：JIP `ShowTextInputMenu` 的临时 vtable adapter 路径。
- `source=StewieTweaksInputTarget`：Stewie Tweaks 搜索框或 string 子设置输入 target 路径。
- `action=insert_ascii` 表示该路径实际写入 ASCII。
- `action=suppress_composition_ascii` 表示组字期间拼音/假名 ASCII 被吞掉；该动作可能来自 `WM_CHAR`，也可能来自 `TextEditMenu::HandleKeyboardInput`。
- `action=suppress_composition_control` 表示组字期间 Backspace/Delete/Left/Right/Home/End/Confirm 等游戏编辑控制输入被吞掉，由 IME 自己处理预编辑串。
- `action=result_inserted` 表示 `GCS_RESULTSTR` 最终提交串已写入。

当前日志已确认一个关键行为：拼音字母可以在 `composing=1` 时由游戏输入处理路径写入真实文本，而不一定只经过 `WM_CHAR`。因此原版路径必须在 `0x7E6620` 内部输入 hook 中吞掉 composition ASCII；JIP 路径必须由 `JipTextInputAdapterEx` 吞掉 composition ASCII。只处理 `WM_CHAR` 不足以避免拼音泄漏。如果拼音仍进入真实文本，需要查看拼音字母对应日志是否仍为 `insert_ascii`，以及当时 IMM open/native 状态是否没有被识别。

若只泄漏拼音开头第一个字母，通常说明该字母早于 composition 消息进入 `TextEditState::Input` / JIP adapter。回归日志应看到输入会话开始或 focus restore 后先出现 `prepared configured IME ... guard=1`；若仍发生抢先写入，第一条非空 composition 后应出现 `source=IMECompositionEcho action=remove_ascii_echo`，然后最终 `GCS_RESULTSTR` 只提交候选文字。

切到系统英文输入法的回归日志应满足：当前 layout 为 `00000409` 时，`IsConfiguredImeLayout` 为 false，`imeOpen` 在 overlay 状态中被钳成 false，不设置 ASCII guard，不出现 `suppress_composition_ascii`，普通 `WM_CHAR` 或 JIP adapter ASCII 输入应走 `insert_ascii` / 原 handler。

### 提交结果和预览层

当前实现已经支持 `GCS_RESULTSTR` 提交和独立候选窗预览：

- 捕获 IME commit。
- 转当前 codepage。
- 如果 active target 是 `TextEditMenu`，用 tNVSE 自己的 DBCS-aware insert 写入 `state + 0` 字符串。
- 更新 `state + 0x10` caret byte offset。
- 校验 1023-byte 安全上限和 `0x717230` 宽度限制。
- 调 `0x7E6700` 刷新。
- 消费该 IME message。
- `GCS_COMPSTR` 只用于 composition 行，不进入真实 buffer。
- TSF `ITfCandidateListUIElement` 优先提供候选汉字；`ImmGetCandidateListW` 只作为旧 IME fallback。
- 输入法名称优先来自 TSF profile description，失败才退回 `ImmGetDescriptionW` / keyboard layout。

### DBCS 编辑键

拦截或包裹：

- Backspace
- Delete
- Left/right
- Home/end 边界修正
- Select-all / clear-on-next-type 状态
- byte 上限和宽度限制截断

ASCII 输入可以继续走原版 `InputUnk01`，但只要当前 buffer 含 DBCS，就必须在原版返回后校正 caret boundary，或者统一改用 tNVSE 编辑层。

### 扩展字段

原版 `TextEditMenu`、JIP `ShowTextInputMenu`、Stewie Tweaks StewMenu/MenuSearch、MCM Extender 搜索、Dialogue History 搜索已由各自独立 target 覆盖。后续扩展应继续按“每种输入框单独 adapter”的方式处理，不要把不同字段布局强行合并：

- Console 输入只在明确需要时处理，因为命令解析和普通 UI 文本不同。
- 其他 XML 菜单搜索如果不是现有 target，需要先确认 tile、handler 和内部 buffer，再决定是否做单独 adapter。
- Rime 后端、Console 输入属于后续扩展；当前原生 Tile overlay 和 TSF/IMM32 候选读取不影响 commit-only 写入层。

每个字段都要单独确认提交路径是否会 sanitize、是否写入存档、是否要求 ASCII。

## 失败处理

出现以下情况时不要消费输入：

- 没有 active editable target。
- 当前不是东亚 UI 模式（`uiEncoding` 不在 `1-4`），或多字节字体能力未实际安装。
- IME result 转码失败。
- target 指针、edit state、caret offset、宽度限制或 byte 上限没有通过反编译确认。
- 插入后会超过 byte 上限，且无法在 DBCS 边界安全截断。
- 插入后超过 `0x717230` 原版宽度限制。
- 当前菜单不是 `TextEditMenu` 或目标已关闭。

未消费的消息必须交回原 WndProc，避免破坏快捷键、控制器、overlay 和其他插件输入。

## 风险

- 全屏/窗口化下系统 IME candidate/composition window 行为不同；当前主路径是在没有输入菜单对象时解绑游戏窗口 HIMC，输入期再恢复 HIMC。输入期仍通过 `WM_IME_SETCONTEXT -> DefWindowProc(lParam=0)`、IMM32 offscreen composition/candidate forms、以及 TSF `BeginUIElement(*pbShow=FALSE)` 隐藏系统 IME UI。注意 IMM32 offscreen 设置不能放在 `WM_IME_NOTIFY/IMN_SETCANDIDATEPOS` 路径内，否则 `ImmSetCandidateWindow` 会反复触发候选位置通知。
- 原生宿主依赖 code `0x544E56` 工厂 hook、`pMenuRoot` 和 IME XML 都成功建立；宿主暂时不可用、code 已被不兼容菜单占用、XML 缺失或 UI 重建破坏树身份时必须保留系统候选窗，不能静默隐藏系统回退。IMM form 快照绑定到当时的 `HWND/HIMC/HKL`，且候选槽只有在成功取得原始 form 后才会移到屏幕外。恢复逐槽检查 `ImmSet*` 的结果，失败项保留到下一主循环重试；上下文或布局身份已变化时丢弃旧快照，绝不把旧 IME 的位置写进新 IME。`WM_INPUTLANGCHANGEREQUEST` 在切换前先恢复 IMM form，但不会提前清除仍受抑制的 TSF/context 记录；切换请求被下游拒绝时，后续 fail-open 仍能找到这些记录。`WM_NCDESTROY` 只清状态，不在窗口销毁中同步发送消息。宿主失效但窗口仍处于前台输入会话时，代码直接调用默认窗口过程恢复当前 IMM UI；它不会用 `SendMessage(WM_IME_SETCONTEXT)` 重入完整 subclass 链。对 tNVSE 实际改成隐藏的 TSF UI element，恢复路径通过 `ITfUIElementMgr::GetUIElement` 取得当前对象并调用 `Show(TRUE)`；成功后才清除该 element 的 suppression 标记，失败则保留到后续主循环重试。输入会话关闭时先原子发布关闭 gate，再执行恢复，从而阻止并发 `BeginUIElement` 在恢复完成后重新隐藏候选窗。
- 原版 `TextEditMenu` caret marker 是单 byte 插入；如果 caret byte offset 错误，会破坏 DBCS。
- 有些输入字段可能用 byte length 当字符数，最大长度要实测。
- `0x7170A0` 和 `InputUnk01` 使用 1024/1028 bytes 级栈缓冲，tNVSE 不能写入超长文本后再交给原版刷新。
- 剪贴板粘贴会一次插入大量文本，必须和 IME result 使用同一套 byte 上限、宽度限制和 DBCS 边界检查。
- WndProc subclass 仍可能和 overlay、输入法增强、其他 NVSE 插件冲突，因此卸载时只在 tNVSE 仍是最上层 subclass 时恢复前驱；如果后装插件位于其上，则保留链节点直到窗口销毁，避免截断后装插件保存的前驱。鼠标、hit-test、capture、raw-input、pointer/touch/gesture 和所有非捕获消息始终在任何菜单/Tile/IME/UI 操作前直接交回原 WndProc。

## 测试计划

### CP936/GBK

- 输入 `测试多字节字符输入`。
- 输入混合文本 `Save-测试-01`。
- 输入 GBK trail byte 覆盖 ASCII 范围的样本，包含 `@ [ ] ' } ~ < > = " & ;` 等风险字符，确认编辑层不把 trail 当 delimiter 或控制键。
- 在中文前、中、后插入 ASCII。
- 在 ASCII 前、中、后插入中文。
- Backspace 删除一个中文字符，确认删除两个 byte。
- Delete 删除一个中文字符。
- Left/right 跨中文字符时一次移动一个逻辑字符。
- 光标在中文前后闪烁时不出现乱码。
- 超过最大长度时不留下半个中文字符。
- 超过文本框宽度时插入被拒绝或按 DBCS 边界安全截断。

### TextEditMenu / 玩家名

- 创建角色时在玩家名输入框输入中文。
- 初始默认名在第一次输入时被 `clearOnNextType` 正确替换。
- 光标移动、删除、确认后，最终玩家名保持当前 codepage 多字节。
- `0x7E6700` validator 仍能控制确认按钮有效状态。

### JIP ShowTextInputMenu

- 使用 JIP 的 `ShowTextInputMenu` 打开通用文本输入框。
- 中文/日文/韩文 IME commit 能进入 JIP `currentText`，拼音/假名预编辑 ASCII 不残留。
- 英文 ASCII 输入仍能进入 JIP 文本框，且不出现 WndProc + JIP adapter 双插入。
- Backspace/Delete/Left/Right 在 CJK 字符上按逻辑字符移动或删除。
- Enter 确认仍触发 JIP 原 handler、关闭菜单并执行脚本 callback。
- `SetTextInputExtendedProps` 的 min/max length、numeric-only 和 Enter-OK 行为不被破坏。

### Stewie Tweaks

- Stewie Tweaks 版本大于等于 9.90 时，日志出现一次 `Stewie Tweaks version ... detected` 和对应 handler chain 日志。
- StewMenu 中 `Ctrl-F` 打开搜索，输入中文后候选选择只提交汉字，拼音/假名预编辑串不残留。
- StewMenu 搜索框中 Backspace/Delete/Left/Right/Home/End 不拆分多字节字符，`Ctrl-R` 清空仍走 Stewie 原逻辑。
- StewMenu 字符串子设置输入只在 string 输入项启用；数字、浮点、十六进制、Hotkey 子设置仍由 Stewie 原逻辑处理。
- Inventory、Stats、Map、Container、Barter、LevelUp、Recipe、Save/Load 的 Stewie MenuSearch 搜索框能输入当前 codepage 多字节文本，关闭搜索和刷新列表行为保持 Stewie 原样。
- 英文 ASCII 输入不出现 WndProc + Stewie handler 双插入，Win+Space 切换输入法后仍能在 Stewie 搜索框中 commit 多字节字符。

### MCM Extender

- 启动日志出现 `MCM Extender runtime input bridge installed`；JIP 版本或签名不匹配时明确记录适配禁用，并保留 MCM 原输入。
- `Ctrl-F` 或搜索按钮打开 MCM mod-list 搜索后，中文/日文/韩文 IME commit 能写入 `_search_text_1`，预编辑字母不会残留。
- Backspace、Delete、Left、Right、Home、End 在 DBCS 字符边界操作；按住编辑键时 Windows repeat 不产生半字符。
- 文本变化后仍由原 `MenuSearch.gek` 刷新 `MCMMods`；Esc、Tab、Ctrl-F、Enter 保持原脚本行为。
- Item Inspect Menu 等注册 JIP 键事件的 mod 在 MCM 搜索输入期间不响应键盘快捷键，关闭输入后也不出现延迟 key-down 或孤立 key-up。
- 鼠标点击、滚轮和控制器事件不受隔离；关闭 `bMultibyteInputMCMExtender` 时完全回到 MCM 原输入路径。
- 验证普通中文搜索结果；同时确认没有 `Sv_Find` hook，含特殊 trail byte 的严格匹配限制仍按原 xNVSE 行为记录，而不误报为已修复。

### Dialogue History

- 启动日志出现 `Dialogue History runtime input bridge installed`；关闭配置、缺少原 UDF 或 JIP 版本/签名不匹配时不建立部分接管状态。
- 在 StartMenu 打开 Dialogue History 的搜索框后，中文/日文/韩文 IME commit 能写入搜索栏，预编辑字母不残留。
- Backspace、Delete、Left、Right、Home、End 按 DBCS 字符边界编辑；单次 Backspace 不删除两个汉字，按住时只按 Windows repeat 连发。
- 连续输入期间不逐键重建列表；停止输入约 500 ms 后由原 `Search.gek` 刷新 NPC 名称过滤结果。
- Enter、Tab、Ctrl-F 会先提交待处理搜索，再走原 `SearchInput.gek` / `OnKeyDown.gek` 的结束或关闭语义。
- 输入期间其他 JIP 键盘快捷键不触发，关闭搜索后没有延迟 key-down 或孤立 key-up；鼠标、滚轮和控制器路径不受影响。
- 关闭 `bMultibyteInputDialogueHistory` 时回到 Dialogue History 原输入路径；确认 mod 目录内容未被修改且没有安装 `Sv_Find` hook。

### 保存名

- 使用中文玩家名后创建手动存档。
- `.fos` basename 仍为原版 sanitizer 后的 ASCII-safe 文本。
- `Data\NVSE\plugins\tnvse\save_display_names.dat` 写入 mapping。
- 载入/保存列表显示原版摘要，例如 `清泉镇 - 3.0 MB`，而不是 raw sanitized filename。
- 覆盖已有中文映射时 display name 不丢。
- 删除/重命名存档时 sidecar record 正确删除/移动。

### UTF-8 / IME 转码

- `bUTF8=1` 且 `uiEncoding=1-4` 时，UTF-8 输入源只转换一次到当前 codepage；IME UTF-16 result 同样只在已启用的东亚输入模式中提交。`uiEncoding=0` 不启动输入层，也不执行 UTF-8→Windows-1252 转换。
- store 中保存当前 codepage bytes，不保存 UTF-8。
- 列表读取时不二次 UTF-8 转换。

### 回归

- 英文输入行为不变。
- Esc/Enter 提交/取消行为不变。
- 键盘、手柄导航不变。
- 自动存档、快速存档不受影响。
- 富文本、终端、任务、地点、HUD 渲染不受影响。
- 未启用 `bMultibyteInput` 时不 subclass WndProc，不影响其他输入插件。

### 候选窗 / 输入法后端

- `bMultibyteInputCompositionPreview=1` 时，composition、候选项和当前输入法名称显示在共享原生 Tile overlay，不修改 active text input 自身的 Tile。
- `bMultibyteInputHideSystemCandidateWindow=1` 时，只有 XML 宿主成功加载才隐藏系统候选窗；删除或破坏 XML 后应明确回退为系统窗口。
- `bMultibyteInputUseTSFCandidates=1` 时，优先测试 Microsoft Pinyin / Sogou / Japanese IME / Korean IME 下 TSF candidate UI 是否能返回候选；关闭该配置时再验证 `ImmGetCandidateListW` fallback。
- Rime 后端如作为可选项，应测试组字期间吞键、commit-only 写入、候选翻页和 ASCII mode 切换。

## 完成标准

多字节字符输入 hook 可认为完成的条件：

- 至少玩家名 `TextEditMenu` 能接收 IME 多字节字符 commit。
- 开启 `bMultibyteInputCompositionPreview=1` 时，能显示当前输入法名称、中文/英文或对应语言模式、全角/半角、composition 和 TSF/IMM32 candidate list；关闭时回到 commit-only 行为。
- 编辑框打字期间能正确显示当前 `uiEncoding` 对应字符。
- Backspace/delete/left/right 不拆 DBCS。
- Stewie Tweaks 9.90+ 的 StewMenu 搜索、string 子设置输入和常见 MenuSearch 搜索框不残留预编辑串，编辑键不拆多字节字符。
- MCM Extender 搜索由纯 tNVSE target 接管编辑并复用原 UDF，且没有修改 mod 文件或 hook `Sv_Find`。
- Dialogue History 搜索保留 500 ms 防抖并复用原 UDF，且没有修改 mod 文件或 hook `Sv_Find`。
- 实际 `.fos` 文件名仍由原版 sanitizer 生成。
- 载入/保存列表继续走原版 save header 摘要显示。
- 没有正常构建中的全局 `Tile::SetString` 或 WndProc 调试日志。
- 缺少 IME、配置或字体支持时能安全回退到原版输入。

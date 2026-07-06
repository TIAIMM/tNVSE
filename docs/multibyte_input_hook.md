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
- 存档显示名映射已经改为 `Data\plugins\tnvse\save_display_names.dat` 单文件 sidecar，不修改 `.fos`、`.nvse` 或 `SaveGameData::pName`。

尚未完成的输入层能力：

- Windows IME 提交串没有自然进入游戏 edit buffer。
- `TextEditMenu` 的原版编辑模型按单字节处理光标、退格、删除和插入，会拆开 DBCS。
- 尚未建立“当前可编辑目标”的统一追踪。
- 玩家名输入、mod 使用的 `TextEditMenu` 字段需要分别确认打开、编辑、提交和显示刷新路径。
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

- tNVSE 第一阶段应优先采用 `WM_IME_COMPOSITION + GCS_RESULTSTR` 的 commit-only 路径。
- `GCS_COMPSTR` 和 TSF/Cicero 只作为 composition preview / candidate window 增强，不应写入真实 edit buffer。
- 必须有 text-input gate：只有当前 active target 是已知可编辑控件时才消费 IME 消息。
- `WM_CHAR` 只能作为非 IME fallback，并且必须避免和 `GCS_RESULTSTR` 双插入。

不能照搬的部分：

- FO4 的 `ProcessCharEvent` 是 Unicode 输入事件；FNV 的 `TextEditMenu` 是 `char` / codepage / byte-offset 编辑模型。
- F4SE 的 D3D11/FW1FontWrapper overlay 不能直接用于 FNV。

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

## 当前首版实现

### 1. 新增输入模块

已新增：

```text
tnvse/Src/multibyte_input.h
tnvse/Src/multibyte_input.cpp
```

新增配置项：

```ini
[Main]
bMultibyteInput = 0
bMultibyteInputDebug = 0
bMultibyteInputCompositionPreview = 0
```

默认关闭更安全。启用条件：

- `bEnableMultibyteFontHook=1`
- `g_usingWinEncoding != 0`
- 当前字体包含对应 codepage 的 extra glyph
- 至少玩家名 `TextEditMenu` 路径已验证

初始化行为：

- 在 `LoadConfig()` 后读取上述配置。
- 在 `NVSEPlugin_Load` 中安装 hook；不从 `DllMain` 安装 WndProc。
- `bMultibyteInput=0` 时不 subclass WndProc，不安装 `TextEditMenu` hook。
- `bMultibyteInput=1` 但字体 hook 未启用或 `uiEncoding=0` 时打印一次日志并跳过初始化。

### 2. WndProc / IME 捕获

在主窗口 HWND 可用后 subclass 游戏窗口：

```cpp
SetWindowLongPtrA(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(MultibyteInputWndProc));
```

不要在 `DllMain` 中安装。所有未消费消息必须转发给原 WndProc。

首版实际处理的消息：

- `WM_IME_COMPOSITION`：只读取 `GCS_RESULTSTR`，提交成功后写入 active `TextEditMenu`。
- `WM_IME_STARTCOMPOSITION` / `WM_IME_ENDCOMPOSITION`：维护 composition 状态。
- `WM_CHAR`：未组字时在 active `TextEditMenu` 下接管可打印 ASCII 和非 ASCII fallback；组字期间吞掉拼音等 ASCII `WM_CHAR`，避免预编辑串写入真实 buffer。
- `WM_NCDESTROY`：恢复原 WndProc。
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

- 只有 `GCS_RESULTSTR` 成功转换且成功插入 active target 时才返回已处理。
- `GCS_COMPSTR` 首版不写 edit buffer；`bMultibyteInputCompositionPreview` 目前只作为后续预览阶段的保留开关。
- `WM_CHAR` 在 active `TextEditMenu` 下直接处理可打印 ASCII；若游戏输入管线随后又发出同一 ASCII input，vtable 包装会用短期 suppress 防止双插入。
- IME composition active，或 IMM context 处于 open/native 且存在预编辑串时，ASCII `WM_CHAR` 视为拼音/假名等预编辑输入并直接消费，不进入真实 edit buffer。
- 游戏原本 `TextEditMenu::HandleKeyboardInput` 路径也必须应用同一规则；正式版日志确认拼音字母可能先从该 vtable 路径到达，而不是只从 `WM_CHAR` 到达。
- `TextEditMenu::HandleKeyboardInput` 在 composition active 时还应吞掉 Backspace/Delete/Left/Right/Home/End/Confirm 等控制输入，避免用户编辑 IME 预编辑串时误删或提交游戏真实文本。
- `GCS_RESULTSTR` 后用短期 suppress 计数避免同一提交又以 `WM_CHAR` 形式插入一次。
- 所有未明确消费的消息都调用原 WndProc。

### 3. Active target 追踪

IME result 只有在已知可编辑目标 active 时才允许消费。第一阶段只支持正式版已确认的 `TextEditMenu` 玩家名输入路径：

- `0x7AB740` 是 `PlayerNameEntryMenu` 调用 `TextEditMenu::Open` 的 call 指令；首版用 `WriteRelCall` 包装它。
- 包装函数调用原版 `TextEditMenu::Open(0x7E6320)`，成功后记录 `TextEditMenu::GetCurrent()`。
- 当原始 validator 是 `PlayerNameEntryMenu::IsValidName(0x7AB820)` 时，首版替换为 DBCS-aware validator；原因是原版 validator 按单字节查 base font 宽度，DBCS high/trail byte 会导致 OK 按钮保持 disabled。
- `0x1070064` 是 `TextEditMenu` vtable 第 12 项；首版用 `ReplaceVirtualFuncEx` 替换为 `TextEditMenuEx::HandleKeyboardInput`。
- `TextEditMenuEx::HandleKeyboardInput` 包装原版 `0x7E6620`，但 ASCII 插入、Backspace、Delete、Left、Right、Home、End 走 DBCS-aware helper；Confirm 仍交给原版并在成功后清空 current target。
- WndProc 每次使用前校验 `dword_11DAEC4` 仍等于 current target 且 `sub_716AE0(target + 0x34)` 为 true。
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

### 4. DBCS-aware 编辑层

需要复用现有 encoding helper：

- `TryDecodeDoubleByte`
- `IsLeadByte`
- `IsTrailByte`

首版已经实现这些边界 helper：

```cpp
bool IsCharBoundary(const std::string& text, size_t offset);
size_t PrevCharBoundary(const std::string& text, size_t offset);
size_t NextCharBoundary(const std::string& text, size_t offset);
size_t ClampToPrevBoundary(const std::string& text, size_t offset);
bool InsertTextAtCaret(TextEditMenu*, std::string_view mbText);
bool DeletePreviousChar(TextEditMenu*);
bool DeleteNextChar(TextEditMenu*);
```

规则：

- 有效 DBCS lead+trail 作为一个逻辑字符。
- ASCII 仍是一个 byte 字符。
- 无效 lead byte 保守按单 byte 处理，不跨越未知内存。
- `state + 0x10` 在任何操作后必须是 `IsCharBoundary(text, len, caret)`。
- 写回前必须保证真实文本长度不超过 1023 bytes，避免 `0x7170A0` 的 `source[1024]` 栈缓冲溢出。
- 如果 `state + 0x14 != -1`，写回前必须调用 `0x717230(editState, candidate)` 或复用同等 `FontManager::CalculateStringDimensions` 校验；首版失败时拒绝插入，不做自动截断。
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
6. `kMessage_SaveGame` 用实际 `.fos` path 更新 `Data\plugins\tnvse\save_display_names.dat`。
7. 保存/读取列表中，`0x851AAE` 手动存档识别 hook 只阻止 raw basename 覆盖原版 save header 摘要。

注意：sidecar 中的 display name 是当前 UI codepage 多字节，不是 UTF-8。若输入源是 UTF-8 或 IME UTF-16，都应在进入 record 前转换为当前 codepage。

## Hook 阶段

### 当前诊断日志

设置 `bMultibyteInputDebug=1` 后，首版会打印 `tnvse_multibyte_input_event` 日志，用于区分输入来源：

- `source=WndProc.WM_CHAR`：Windows 字符消息路径。
- `source=WndProc.WM_IME_COMPOSITION`：IME composition/result 路径。
- `source=TextEditMenu::HandleKeyboardInput`：游戏原本键盘输入路径。
- `action=insert_ascii` 表示该路径实际写入 ASCII。
- `action=suppress_composition_ascii` 表示组字期间拼音/假名 ASCII 被吞掉；该动作可能来自 `WM_CHAR`，也可能来自 `TextEditMenu::HandleKeyboardInput`。
- `action=suppress_composition_control` 表示组字期间 Backspace/Delete/Left/Right/Home/End/Confirm 等游戏编辑控制输入被吞掉，由 IME 自己处理预编辑串。
- `action=result_inserted` 表示 `GCS_RESULTSTR` 最终提交串已写入。

当前日志已确认一个关键行为：拼音字母可以在 `composing=1` 时由 `TextEditMenu::HandleKeyboardInput action=insert_ascii` 写入真实文本。因此实现必须在 vtable 包装层吞掉 composition ASCII；只处理 `WM_CHAR` 不足以避免拼音泄漏。如果拼音仍进入真实文本，需要查看拼音字母对应日志是否仍为 `insert_ascii`，以及当时 IMM open/native 状态是否没有被识别。

### 阶段 A：只读/低量日志定位

临时日志建议：

- `TextEditMenu` open：`this`、title、initial text、callback。
- `TextEditMenu` caller：至少记录 `0x7AB690` 玩家名路径；若发现其他调用方，再单独分类。
- `TextEditMenu` close/destruct：`this`。
- `0x7E6620` 输入事件：key code、active flag、caret byte offset、文本长度。
- `InputUnk01` 前后：仅 ASCII 测试时记录，确认原版行为。
- IME result：UTF-16 length、转换后 byte length、active target 类型。

日志不要在正常构建长期保留。

### 阶段 B：仅提交结果的 IME 输入

先只支持 `GCS_RESULTSTR`：

- 捕获 IME commit。
- 转当前 codepage。
- 如果 active target 是 `TextEditMenu`，用 tNVSE 自己的 DBCS-aware insert 写入 `state + 0` 字符串。
- 更新 `state + 0x10` caret byte offset。
- 校验 1023-byte 安全上限和 `0x717230` 宽度限制。
- 调 `0x7E6700` 刷新。
- 消费该 IME message。

不做 composition preview，不调整候选窗位置。

### 阶段 C：DBCS 编辑键

拦截或包裹：

- Backspace
- Delete
- Left/right
- Home/end 边界修正
- Select-all / clear-on-next-type 状态
- byte 上限和宽度限制截断

ASCII 输入可以继续走原版 `InputUnk01`，但只要当前 buffer 含 DBCS，就必须在原版返回后校正 caret boundary，或者统一改用 tNVSE 编辑层。

### 阶段 D：扩展字段

玩家名 `TextEditMenu` 稳定后再扩展：

- 其他角色名/玩家名入口，若它们不共用 `0x7E6320`。
- mod 打开的通用 `TextEditMenu` 文本框。
- Stewie Tweaks Menu Search：不改 Stewie 源码时，需要单独用 active search bar gate + codepage byte replay 或 shadow buffer 方案；不要混进 `TextEditMenu` adapter。
- Console 输入只在明确需要时处理，因为命令解析和普通 UI 文本不同。
- Rime 后端、自绘候选窗、TSF/Cicero candidate list 都属于第二阶段增强，不影响第一阶段 commit-only 设计。

每个字段都要单独确认提交路径是否会 sanitize、是否写入存档、是否要求 ASCII。

## 失败处理

出现以下情况时不要消费输入：

- 没有 active editable target。
- `g_usingWinEncoding == 0`。
- IME result 转码失败。
- target 指针、edit state、caret offset、宽度限制或 byte 上限没有通过反编译确认。
- 插入后会超过 byte 上限，且无法在 DBCS 边界安全截断。
- 插入后超过 `0x717230` 原版宽度限制。
- 当前菜单不是 `TextEditMenu` 或目标已关闭。

未消费的消息必须交回原 WndProc，避免破坏快捷键、控制器、overlay 和其他插件输入。

## 风险

- 全屏/窗口化下 IME candidate window 行为不同。
- 如果不调用 `ImmSetCompositionWindow`，候选窗可能出现在默认位置。
- 原版 `TextEditMenu` caret marker 是单 byte 插入；如果 caret byte offset 错误，会破坏 DBCS。
- 有些输入字段可能用 byte length 当字符数，最大长度要实测。
- `0x7170A0` 和 `InputUnk01` 使用 1024/1028 bytes 级栈缓冲，tNVSE 不能写入超长文本后再交给原版刷新。
- 剪贴板粘贴会一次插入大量文本，必须和 IME result 使用同一套 byte 上限、宽度限制和 DBCS 边界检查。
- WndProc subclass 可能和 overlay、输入法增强、其他 NVSE 插件冲突，必须只在 active target 时消费确定的 commit。

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

### 保存名

- 使用中文玩家名后创建手动存档。
- `.fos` basename 仍为原版 sanitizer 后的 ASCII-safe 文本。
- `Data\plugins\tnvse\save_display_names.dat` 写入 mapping。
- 载入/保存列表显示原版摘要，例如 `清泉镇 - 3.0 MB`，而不是 raw sanitized filename。
- 覆盖已有中文映射时 display name 不丢。
- 删除/重命名存档时 sidecar record 正确删除/移动。

### UTF-8 / IME 转码

- `bUTF8=1` 时，UTF-8 输入源或 IME UTF-16 result 只转换一次到当前 codepage。
- store 中保存当前 codepage bytes，不保存 UTF-8。
- 列表读取时不二次 UTF-8 转换。

### 回归

- 英文输入行为不变。
- Esc/Enter 提交/取消行为不变。
- 键盘、手柄导航不变。
- 自动存档、快速存档不受影响。
- 富文本、终端、任务、地点、HUD 渲染不受影响。
- 未启用 `bMultibyteInput` 时不 subclass WndProc，不影响其他输入插件。

### 可选候选窗 / 输入法后端

- Windows 系统候选窗在窗口化和全屏下是否可用。
- 如果不可用，再测试 tile/DX9 overlay 的 composition preview。
- Rime 后端如作为可选项，应测试组字期间吞键、commit-only 写入、候选翻页和 ASCII mode 切换。

## 完成标准

多字节字符输入 hook 可认为完成的条件：

- 至少玩家名 `TextEditMenu` 能接收 IME 多字节字符 commit。
- 编辑框打字期间能正确显示当前 `uiEncoding` 对应字符。
- Backspace/delete/left/right 不拆 DBCS。
- 实际 `.fos` 文件名仍由原版 sanitizer 生成。
- 载入/保存列表继续走原版 save header 摘要显示。
- 没有正常构建中的全局 `Tile::SetString` 或 WndProc 调试日志。
- 缺少 IME、配置或字体支持时能安全回退到原版输入。

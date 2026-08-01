#include "font_engine.h"
#include "font_manager.h"
#include "font_vector.h"
#include "game_hooks.h"
#include "hook_identity.h"
#include "load_config.h"
#include "SafeWrite.h"
#include "text_hooks.h"
#include "tnvse.h"

#include "NiExtraData.hpp"
#include "TileImage.hpp"
#include "TileRect.hpp"
#include "TileText.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>

namespace fonthook
{
	namespace implementation::game_hooks {}
	using namespace implementation::game_hooks;

	namespace implementation::game_hooks
	{
		using hook_identity::Rel32Opcode;
		using hook_identity::Rel32Site;

		inline constexpr SIZE_T kFontConstructor = 0xA12020;
		inline constexpr SIZE_T kFontLoad = 0xA15320;
		inline constexpr SIZE_T kFontCreateText = 0xA12880;
		inline constexpr SIZE_T kFontMakeString = 0xA12460;
		inline constexpr SIZE_T kCalculateStringDimensions = 0xA1B020;
		inline constexpr SIZE_T kFontPrepText = 0xA12FB0;
		inline constexpr Rel32Site kDoorPromptCallSite = {
			0x777006, 0x406D00, "Door prompt -> BSsprintf"
		};

		constexpr std::array<Rel32Site, 5> kCommonFontCallSites = {{
			{ 0xA18F4A, 0xA18A30, "FontManager::CreateText -> FontManager::PrepText" },
			{ 0xA18F63, 0xA19060, "FontManager::CreateText -> TextDoc::Render" },
			{ 0xA19622, 0xA142D0, "TextDoc::Render -> Font::AddChar" },
			{ 0x759281, 0xA12FB0, "Terminal text -> Font::PrepText" },
			{ 0xA19C80, 0xA19F70, "TextLine wrap -> TextLine::AddChar" },
		}};

		constexpr std::array<Rel32Site, 1> kFreeTypeOnlyCallSites = {{
			{ 0xA1BDE2, 0xA19F70, "TextLine constructor -> TextLine::AddChar" },
		}};

		constexpr std::array<Rel32Site, 27> kMultibyteFontCallSites = {{
			{ 0x6FFFEE, 0x401460, "AnimatingText::Update -> memcpy" },
			{ 0xA18ACC, 0xA17390, "FontManager::PrepText -> PrepHypertext" },
			{ 0xA1772D, 0xA16EA0, "PrepHypertext CollectTo[0]" },
			{ 0xA17835, 0xA16EA0, "PrepHypertext CollectTo[1]" },
			{ 0xA17A1E, 0xA16EA0, "PrepHypertext CollectTo[2]" },
			{ 0xA17B65, 0xA16EA0, "PrepHypertext CollectTo[3]" },
			{ 0xA17BB1, 0xA16EA0, "PrepHypertext CollectTo[4]" },
			{ 0xA17CFE, 0xA16EA0, "PrepHypertext CollectTo[5]" },
			{ 0xA17D5D, 0xA16EA0, "PrepHypertext CollectTo attribute[0]" },
			{ 0xA17DE9, 0xA16EA0, "PrepHypertext CollectTo attribute[1]" },
			{ 0xA18F7D, 0xA1B990, "FontManager::CreateText -> TextDoc::~TextDoc" },
			{ 0xA178A4, 0xA19A10, "PrepHypertext TextDoc::AddChar[0]" },
			{ 0xA179D9, 0xA19A10, "PrepHypertext TextDoc::AddChar[1]" },
			{ 0xA17FC2, 0xA19A10, "PrepHypertext TextDoc::AddChar[2]" },
			{ 0xA18D7C, 0xA19A10, "PrepText TextDoc::AddChar" },
			{ 0xA19A6F, 0xA19C00, "TextDoc::AddChar -> TextPage::AddChar" },
			{ 0xA1BD1C, 0xA19C00, "TextPage constructor -> TextPage::AddChar" },
			{ 0xA17898, 0xA1B660, "PrepHypertext CharData::Copy[0]" },
			{ 0xA179CD, 0xA1B660, "PrepHypertext CharData::Copy[1]" },
			{ 0xA17FB6, 0xA1B660, "PrepHypertext CharData::Copy[2]" },
			{ 0xA18D73, 0xA1B660, "PrepText CharData::Copy" },
			{ 0x77AF4B, 0xA01350, "Quest text -> Tile::SetString" },
			{ 0x772B5E, 0xA01350, "Location text -> Tile::SetString" },
			{ 0x7591AC, 0x559450, "Terminal text -> BSStringT<char>::c_str" },
			{ 0x772B4B, 0x438EB0, "Location text -> BSStringT<char>::GetCStringOrEmpty" },
			{ 0x77ACCC, 0x406D30, "Quest text -> strcpy_s[0]" },
			{ 0x77ACF8, 0x406D30, "Quest text -> strcpy_s[1]" },
		}};

		constexpr std::array<UInt8, 8> kFontPrepTextPrologue = {
			0x55, 0x8B, 0xEC, 0x81, 0xEC, 0xE0, 0x07, 0x00
		};

		template <size_t N>
		bool ValidateStockCallSites(
			const std::array<Rel32Site, N>& sites)
		{
			bool valid = true;
			for (const Rel32Site& site : sites)
			{
				SIZE_T actualTarget = 0;
				if (!hook_identity::ReadRel32Target(
					site.address, Rel32Opcode::Call, actualTarget))
				{
					gLog.FormattedMessage(
						"tnvse_font_hook: identity mismatch site=%s address=%08X expected=CALL rel32",
						site.name, static_cast<UInt32>(site.address));
					valid = false;
					continue;
				}
				if (actualTarget != site.stockTarget)
				{
					gLog.FormattedMessage(
						"tnvse_font_hook: identity mismatch site=%s address=%08X expectedTarget=%08X actualTarget=%08X",
						site.name, static_cast<UInt32>(site.address),
						static_cast<UInt32>(site.stockTarget),
						static_cast<UInt32>(actualTarget));
					valid = false;
				}
			}
			return valid;
		}

		bool ValidateRequiredFontHookSites()
		{
			bool valid = ValidateStockCallSites(kCommonFontCallSites);
			if (g_bEnableMultibyteFontHook)
			{
				valid = ValidateStockCallSites(kMultibyteFontCallSites)
					&& valid;
				if (!hook_identity::IsAccessibleRegion(
					kFontPrepText, kFontPrepTextPrologue.size(), true)
					|| std::memcmp(reinterpret_cast<const void*>(kFontPrepText),
						kFontPrepTextPrologue.data(),
						kFontPrepTextPrologue.size()) != 0)
				{
					gLog.FormattedMessage(
						"tnvse_font_hook: identity mismatch site=Font::PrepText address=%08X length=%u",
						static_cast<UInt32>(kFontPrepText),
						static_cast<UInt32>(kFontPrepTextPrologue.size()));
					valid = false;
				}
			}
			else
			{
				valid = ValidateStockCallSites(kFreeTypeOnlyCallSites)
					&& valid;
			}
			return valid;
		}

		using FontInitFn = Font* (__thiscall*)(Font*, int, char*, bool);
		using FontLoadFn = void (__thiscall*)(Font*);
		using FontCreateTextFn = void (__thiscall*)(Font*, BSStringT<char>*,
			int*, int*, int, int, int, char, const NiColorA*, NiTriShape**, NiTriShape**);
		using FontMakeStringFn = NiAVObject* (__thiscall*)(Font*, float, float,
			float, BSStringT<char>*, int*, bool, const NiColorA*, bool, bool);
		using CalculateStringDimensionsFn = NiPoint3* (__thiscall*)(FontManager*,
			NiPoint3*, const char*, UInt32, float, UInt32);

		FontInitFn s_originalFontInit = nullptr;
		FontLoadFn s_originalFontLoad = nullptr;
		FontCreateTextFn s_originalFontCreateText = nullptr;
		FontMakeStringFn s_originalFontMakeString = nullptr;
		CalculateStringDimensionsFn s_originalCalculateStringDimensions = nullptr;
		FontHookInstallState s_fontHookInstallState;

		void* __cdecl CopyAnimatingTextEncodedUnits(
			void* destination, const void* source, SIZE_T unitCount)
		{
			const char* encodedSource = static_cast<const char*>(source);
			SIZE_T byteCount = 0;
			for (SIZE_T unitIndex = 0;
				unitIndex < unitCount && encodedSource[byteCount]; ++unitIndex)
			{
				UInt32 doubleByteCode = 0;
				byteCount += TryDecodeDoubleByte(
					encodedSource + byteCount, doubleByteCode) ? 2 : 1;
			}
			return std::memcpy(destination, source, byteCount);
		}

		constexpr std::array<UInt8, 5> kFontInitPrologue = {
			0x55, 0x8B, 0xEC, 0x6A, 0xFF
		};
		constexpr std::array<UInt8, 5> kFontLoadPrologue = {
			0x55, 0x8B, 0xEC, 0x6A, 0xFF
		};
		constexpr std::array<UInt8, 5> kFontCreateTextPrologue = {
			0x55, 0x8B, 0xEC, 0x6A, 0xFF
		};
		constexpr std::array<UInt8, 9> kFontMakeStringPrologue = {
			0x55, 0x8B, 0xEC, 0x81, 0xEC, 0xAC, 0x00, 0x00, 0x00
		};
		constexpr std::array<UInt8, 6> kCalculateDimensionsPrologue = {
			0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x4C
		};

		struct PendingTrampoline
		{
			SIZE_T source = 0;
			const UInt8* expected = nullptr;
			SIZE_T length = 0;
			void* code = nullptr;
		};

		template <class C, class Ret, class... Args>
		SIZE_T MemberFunctionAddress(Ret(C::*target)(Args...))
		{
			static_assert(sizeof(target) == sizeof(SIZE_T),
				"retail x86 hooks require a single-inheritance member pointer");
			union
			{
				Ret(C::*member)(Args...);
				SIZE_T address;
			} conversion = {};
			conversion.member = target;
			return conversion.address;
		}

		bool HasNopTail(SIZE_T source, SIZE_T patchedLength)
		{
			for (SIZE_T offset = 5; offset < patchedLength; ++offset)
			{
				if (*reinterpret_cast<const UInt8*>(source + offset) != 0x90)
					return false;
			}
			return true;
		}

		bool BuildTrampoline(PendingTrampoline& trampoline)
		{
			if (!trampoline.source || !trampoline.expected || trampoline.length < 5
				|| !hook_identity::IsAccessibleRegion(
					trampoline.source, trampoline.length, true)
				|| std::memcmp(reinterpret_cast<const void*>(trampoline.source),
					trampoline.expected, trampoline.length) != 0)
			{
				gLog.FormattedMessage(
					"tnvse_font_hook: original entry signature mismatch address=%08X length=%u",
					static_cast<UInt32>(trampoline.source),
					static_cast<UInt32>(trampoline.length));
				return false;
			}

			UInt8* code = static_cast<UInt8*>(VirtualAlloc(nullptr,
				trampoline.length + 5, MEM_COMMIT | MEM_RESERVE,
				PAGE_EXECUTE_READWRITE));
			if (!code)
			{
				gLog.FormattedMessage(
					"tnvse_font_hook: trampoline allocation failed address=%08X",
					static_cast<UInt32>(trampoline.source));
				return false;
			}

			std::memcpy(code, reinterpret_cast<const void*>(trampoline.source),
				trampoline.length);
			code[trampoline.length] = 0xE9;
			*reinterpret_cast<UInt32*>(code + trampoline.length + 1) =
				static_cast<UInt32>(trampoline.source + trampoline.length
					- reinterpret_cast<SIZE_T>(code + trampoline.length + 5));
			FlushInstructionCache(GetCurrentProcess(), code, trampoline.length + 5);
			trampoline.code = code;
			return true;
		}

		bool InstallCoreFontEntryHooks()
		{
			std::array<PendingTrampoline, 5> trampolines = {{
				{ kFontConstructor, kFontInitPrologue.data(), kFontInitPrologue.size() },
				{ kFontLoad, kFontLoadPrologue.data(), kFontLoadPrologue.size() },
				{ kFontCreateText, kFontCreateTextPrologue.data(), kFontCreateTextPrologue.size() },
				{ kFontMakeString, kFontMakeStringPrologue.data(), kFontMakeStringPrologue.size() },
				{ kCalculateStringDimensions, kCalculateDimensionsPrologue.data(), kCalculateDimensionsPrologue.size() }
			}};

			for (PendingTrampoline& trampoline : trampolines)
			{
				if (BuildTrampoline(trampoline))
					continue;
				for (PendingTrampoline& allocated : trampolines)
				{
					if (allocated.code)
						VirtualFree(allocated.code, 0, MEM_RELEASE);
				}
				return false;
			}

			s_originalFontInit = reinterpret_cast<FontInitFn>(trampolines[0].code);
			s_originalFontLoad = reinterpret_cast<FontLoadFn>(trampolines[1].code);
			s_originalFontCreateText = reinterpret_cast<FontCreateTextFn>(trampolines[2].code);
			s_originalFontMakeString = reinterpret_cast<FontMakeStringFn>(trampolines[3].code);
			s_originalCalculateStringDimensions =
				reinterpret_cast<CalculateStringDimensionsFn>(trampolines[4].code);

			WriteRelJumpEx(kFontConstructor, &FontEx::FontInit);
			WriteRelJumpEx(kFontLoad, &FontEx::Load);
			WriteRelJump(kFontCreateText,
				reinterpret_cast<UInt32>(&FreeTypeCreateTextEntryHook));
			WriteRelJumpEx(kFontMakeString, &FontEx::MakeString);
			PatchMemoryNop(kFontMakeString + 5, kFontMakeStringPrologue.size() - 5);
			WriteRelJumpEx(kCalculateStringDimensions,
				&FontManagerEx::CalculateStringDimensions);
			PatchMemoryNop(kCalculateStringDimensions + 5,
				kCalculateDimensionsPrologue.size() - 5);

			const std::array<SIZE_T, 5> hookTargets = {{
				MemberFunctionAddress(&FontEx::FontInit),
				MemberFunctionAddress(&FontEx::Load),
				reinterpret_cast<SIZE_T>(&FreeTypeCreateTextEntryHook),
				MemberFunctionAddress(&FontEx::MakeString),
				MemberFunctionAddress(
					&FontManagerEx::CalculateStringDimensions),
			}};
			bool installed = true;
			for (size_t i = 0; i < trampolines.size(); ++i)
			{
				installed = hook_identity::MatchesRel32Target(
					trampolines[i].source,
					Rel32Opcode::Jump,
					hookTargets[i]) && installed;
				if (trampolines[i].length > 5)
				{
					installed = HasNopTail(
						trampolines[i].source,
						trampolines[i].length) && installed;
				}
			}
			if (!installed)
			{
				for (PendingTrampoline& trampoline : trampolines)
				{
					SafeWriteBuf(trampoline.source,
						trampoline.expected, trampoline.length);
					VirtualFree(trampoline.code, 0, MEM_RELEASE);
				}
				s_originalFontInit = nullptr;
				s_originalFontLoad = nullptr;
				s_originalFontCreateText = nullptr;
				s_originalFontMakeString = nullptr;
				s_originalCalculateStringDimensions = nullptr;
				gLog.FormattedMessage(
					"tnvse_font_hook: core entry write verification failed; restored stock prologues");
				return false;
			}
			return true;
		}

		constexpr SIZE_T kTileTextMakeNodeVTableEntry = 0x1094880;
		constexpr SIZE_T kVanillaTileTextMakeNode = 0xA21AF0;
		constexpr SIZE_T kTileRectMakeNodeVTableEntry = 0x106ED78;
		constexpr SIZE_T kVanillaTileRectMakeNode = 0xA1F3B0;
		constexpr SIZE_T kTileImageMakeNodeVTableEntry = 0x106F024;
		constexpr SIZE_T kVanillaTileImageMakeNode = 0xA1FD50;
		constexpr SIZE_T kVanillaNiNodeVTable = 0x109B5AC;
		constexpr SIZE_T kVanillaNiNodeOnVisible = 0xA5DBE0;
		constexpr SIZE_T kTileNodeExtraVTable = 0x1094CFC;
		constexpr SIZE_T kTileAbsoluteY = 0xA01440;
		constexpr size_t kNiNodeVirtualCount = 64;
		constexpr size_t kNiNodeOnVisibleSlot = 53;
		constexpr UInt32 kMaximumTileAncestorDepth = 64;
		constexpr UInt32 kMaximumViewportSubtreeDepth = 32;
		constexpr UInt32 kMaximumViewportSubtreeTiles = 256;
		constexpr float kMaximumExactFloatInteger = 16777216.0f;
		constexpr float kViewportCullSafetyPadding = 96.0f;
		constexpr float kIdentityTransformEpsilon = 0.001f;

		using TileTextMakeNodeFn = NiNode* (__thiscall*)(TileText*);
		using TileRectMakeNodeFn = NiNode* (__thiscall*)(TileRect*);
		using TileImageMakeNodeFn = NiNode* (__thiscall*)(TileImage*);
		using NiNodeOnVisibleFn =
			void (__thiscall*)(NiNode*, NiCullingProcess*);
		using TileAbsoluteYFn = float (__thiscall*)(Tile*);

		struct TileNodeExtraView
		{
			UInt8 base[0x0C];
			Tile* tile = nullptr;
			NiNode* node = nullptr;
		};

		struct ViewportNodeVTableProxy
		{
			void** sourceVTable = nullptr;
			NiNodeOnVisibleFn predecessor = nullptr;
			void* completeObjectLocator = nullptr;
			std::array<void*, kNiNodeVirtualCount> entries = {};
		};

		struct TileVerticalBounds
		{
			float top = std::numeric_limits<float>::max();
			float bottom = std::numeric_limits<float>::lowest();
			UInt32 visited = 0;
			bool hasArea = false;
		};

		struct ViewportCullDecision
		{
			bool checked = false;
			bool culled = false;
			bool failOpen = false;
			bool fastVisible = false;
			bool deepCheck = false;
			UInt32 visitedTiles = 0;
		};

		static_assert(sizeof(NiExtraData) == 0x0C,
			"Tileptr extra-data layout requires the retail NiExtraData ABI");
		static_assert(offsetof(TileNodeExtraView, tile) == 0x0C,
			"Tileptr Tile offset changed");
		static_assert(offsetof(TileNodeExtraView, node) == 0x10,
			"Tileptr NiNode offset changed");
		static_assert(offsetof(ViewportNodeVTableProxy, entries)
			== offsetof(ViewportNodeVTableProxy, completeObjectLocator)
				+ sizeof(void*),
			"RTTI locator must immediately precede the proxy vtable");

		TileTextMakeNodeFn s_tileTextMakeNode = nullptr;
		TileRectMakeNodeFn s_tileRectMakeNode = nullptr;
		TileImageMakeNodeFn s_tileImageMakeNode = nullptr;
		ViewportNodeVTableProxy s_viewportNodeVTable;
		thread_local UInt32 s_effectSuppressionDepth = 0;
		thread_local UInt32 s_vuiProxyMeasureOnlyDepth = 0;
		bool s_loggedViewportNodeVTableConflict = false;

		void __fastcall ViewportListNodeOnVisibleHook(
			NiNode* node, void*, NiCullingProcess* culler);

		bool IsFiniteTraitValue(const Tile::Value* value)
		{
			return value && std::isfinite(value->fNum);
		}

		bool IsNonnegativeIntegralListIndex(float value)
		{
			return std::isfinite(value) && value >= 0.0f
				&& value <= kMaximumExactFloatInteger
				&& std::floor(value) == value;
		}

		Tile* FindNearestClipWindow(Tile* tile, bool& valid)
		{
			valid = true;
			UInt32 depth = 0;
			for (Tile* current = tile ? tile->pParent : nullptr;
				current; current = current->pParent)
			{
				if (++depth > kMaximumTileAncestorDepth)
				{
					valid = false;
					return nullptr;
				}
				Tile::Value* clipWindow =
					current->GetValue(Tile::kTileValue_clipwindow);
				if (!clipWindow)
					continue;
				if (!std::isfinite(clipWindow->fNum))
				{
					valid = false;
					return nullptr;
				}
				if (clipWindow->fNum > 0.5f)
					return current;
			}
			return nullptr;
		}

		bool IsViewportListItemCandidate(Tile* tile)
		{
			if (!tile)
				return false;
			Tile::Value* listIndex =
				tile->GetValue(Tile::kTileValue_listindex);
			if (!listIndex
				|| !IsNonnegativeIntegralListIndex(listIndex->fNum))
			{
				return false;
			}
			Tile::Value* clips = tile->GetValue(Tile::kTileValue_clips);
			if (!IsFiniteTraitValue(clips) || clips->fNum <= 0.5f)
				return false;
			bool valid = true;
			return FindNearestClipWindow(tile, valid) && valid;
		}

		Tile* FindTileForNode(NiNode* node)
		{
			if (!node || !node->m_ppkExtra || !node->m_usExtraDataSize
				|| node->m_usExtraDataSize > 64)
			{
				return nullptr;
			}
			for (UInt16 index = 0;
				index < node->m_usExtraDataSize; ++index)
			{
				NiExtraData* extra = node->m_ppkExtra[index];
				if (!extra
					|| *reinterpret_cast<const SIZE_T*>(extra)
						!= kTileNodeExtraVTable)
				{
					continue;
				}
				const TileNodeExtraView* view =
					reinterpret_cast<const TileNodeExtraView*>(extra);
				if (view->node == node && view->tile
					&& view->tile->spNiNode == node)
				{
					return view->tile;
				}
			}
			return nullptr;
		}

		bool HasIdentityTileTransform(Tile* tile)
		{
			if (!tile)
				return false;
			if (Tile::Value* rotation =
				tile->GetValue(Tile::kTileValue_rotateangle))
			{
				if (!std::isfinite(rotation->fNum)
					|| std::fabs(rotation->fNum)
						> kIdentityTransformEpsilon)
				{
					return false;
				}
			}
			if (Tile::Value* zoom =
				tile->GetValue(Tile::kTileValue_zoom))
			{
				if (!std::isfinite(zoom->fNum))
					return false;
				// Retail uses zero as the uninitialized/default zoom sentinel and
				// 100 as an explicit identity value.
				const float value = zoom->fNum;
				if (std::fabs(value) > kIdentityTransformEpsilon
					&& std::fabs(value - 100.0f)
						> kIdentityTransformEpsilon)
				{
					return false;
				}
			}
			return true;
		}

		bool HasSafeTileChain(Tile* tile, Tile* clipWindow)
		{
			UInt32 depth = 0;
			for (Tile* current = tile; current;
				current = current->pParent)
			{
				if (++depth > kMaximumTileAncestorDepth
					|| !HasIdentityTileTransform(current))
				{
					return false;
				}
				if (current == clipWindow)
					return true;
			}
			return false;
		}

		bool IsSupportedViewportTileType(Tile* tile)
		{
			if (!tile)
				return false;
			switch (tile->GetType())
			{
			case Tile::kTileID_rect:
			case Tile::kTileID_image:
			case Tile::kTileID_text:
			case Tile::kTileID_hotrect:
			case Tile::kTileID_window:
				return true;
			default:
				return false;
			}
		}

		float GetAbsoluteTileY(Tile* tile)
		{
			return reinterpret_cast<TileAbsoluteYFn>(
				kTileAbsoluteY)(tile);
		}

		bool IsSceneNodeWithinRoot(NiNode* node, NiNode* root)
		{
			UInt32 depth = 0;
			for (NiNode* current = node; current;
				current = current->m_pkParent)
			{
				if (++depth > kMaximumTileAncestorDepth)
					return false;
				if (current == root)
					return true;
			}
			return false;
		}

		bool AccumulateViewportSubtreeBounds(
			Tile* tile, Tile* expectedParent, NiNode* rootNode,
			UInt32 depth, TileVerticalBounds& bounds)
		{
			if (!tile || tile->pParent != expectedParent
				|| depth > kMaximumViewportSubtreeDepth
				|| ++bounds.visited > kMaximumViewportSubtreeTiles)
			{
				return false;
			}

			NiNode* tileNode = tile->spNiNode;
			if (tileNode && tileNode->GetAppCulled())
				return true;
			if (!IsSupportedViewportTileType(tile)
				|| !HasIdentityTileTransform(tile))
			{
				return false;
			}
			if (tileNode
				&& (FindTileForNode(tileNode) != tile
					|| !IsSceneNodeWithinRoot(tileNode, rootNode)))
			{
				return false;
			}

			Tile::Value* height = tile->GetValue(Tile::kTileValue_height);
			if (!height)
			{
				if (tileNode)
					return false;
			}
			else
			{
				if (!std::isfinite(height->fNum) || height->fNum < 0.0f)
					return false;
				if (height->fNum > 0.0f)
				{
					const float top = GetAbsoluteTileY(tile);
					const float bottom = top + height->fNum;
					if (!std::isfinite(top) || !std::isfinite(bottom)
						|| bottom < top)
					{
						return false;
					}
					bounds.top = std::min(bounds.top, top);
					bounds.bottom = std::max(bounds.bottom, bottom);
					bounds.hasArea = true;
				}
			}

			for (Tile* child : tile->kChildren)
			{
				if (child && !AccumulateViewportSubtreeBounds(
					child, tile, rootNode, depth + 1, bounds))
				{
					return false;
				}
			}
			return true;
		}

		bool IsOutsidePaddedViewport(
			float top, float bottom, float clipTop, float clipBottom)
		{
			const double padding = kViewportCullSafetyPadding;
			return static_cast<double>(bottom)
					< static_cast<double>(clipTop) - padding
				|| static_cast<double>(top)
					> static_cast<double>(clipBottom) + padding;
		}

		ViewportCullDecision ShouldCullViewportListNode(NiNode* node)
		{
			ViewportCullDecision decision;
			Tile* tile = FindTileForNode(node);
			if (!tile)
				return decision;

			Tile::Value* listIndex =
				tile->GetValue(Tile::kTileValue_listindex);
			if (!listIndex || listIndex->fNum < 0.0f)
				return decision;
			decision.checked = true;
			if (!std::isfinite(listIndex->fNum))
			{
				decision.failOpen = true;
				return decision;
			}

			Tile::Value* clips = tile->GetValue(Tile::kTileValue_clips);
			if (!clips || clips->fNum <= 0.5f)
				return decision;
			if (!std::isfinite(clips->fNum))
			{
				decision.failOpen = true;
				return decision;
			}

			bool chainValid = true;
			Tile* clipWindow = FindNearestClipWindow(tile, chainValid);
			Tile::Value* clipHeight = clipWindow
				? clipWindow->GetValue(Tile::kTileValue_height) : nullptr;
			Tile::Value* rootHeight =
				tile->GetValue(Tile::kTileValue_height);
			if (!chainValid || !clipWindow
				|| !IsFiniteTraitValue(clipHeight)
				|| clipHeight->fNum <= 0.0f
				|| !IsFiniteTraitValue(rootHeight)
				|| rootHeight->fNum <= 0.0f)
			{
				decision.failOpen = true;
				return decision;
			}

			const float clipTop = GetAbsoluteTileY(clipWindow);
			const float clipBottom = clipTop + clipHeight->fNum;
			const float rootTop = GetAbsoluteTileY(tile);
			const float rootBottom = rootTop + rootHeight->fNum;
			if (!std::isfinite(clipTop) || !std::isfinite(clipBottom)
				|| !std::isfinite(rootTop) || !std::isfinite(rootBottom)
				|| clipBottom < clipTop || rootBottom < rootTop)
			{
				decision.failOpen = true;
				return decision;
			}

			// This is the hot visible-row path. It deliberately avoids transform
			// validation and recursive trait/node traversal because returning to
			// stock never needs an invisibility proof.
			if (!IsOutsidePaddedViewport(
				rootTop, rootBottom, clipTop, clipBottom))
			{
				decision.fastVisible = true;
				return decision;
			}

			decision.deepCheck = true;
			if (!IsNonnegativeIntegralListIndex(listIndex->fNum)
				|| !HasSafeTileChain(tile, clipWindow)
				|| tile->spNiNode != node)
			{
				decision.failOpen = true;
				return decision;
			}

			TileVerticalBounds bounds;
			const bool complete = AccumulateViewportSubtreeBounds(
				tile, tile->pParent, node, 0, bounds);
			decision.visitedTiles = bounds.visited;
			if (!complete || !bounds.hasArea)
			{
				decision.failOpen = true;
				return decision;
			}
			decision.culled = IsOutsidePaddedViewport(
				bounds.top, bounds.bottom, clipTop, clipBottom);
			return decision;
		}

		bool InitializeViewportNodeVTable()
		{
			if (s_viewportNodeVTable.sourceVTable)
				return true;

			constexpr SIZE_T tableStart =
				kVanillaNiNodeVTable - sizeof(void*);
			constexpr SIZE_T tableBytes =
				(kNiNodeVirtualCount + 1) * sizeof(void*);
			if (!hook_identity::IsAccessibleRegion(
				tableStart, tableBytes, false))
			{
				return false;
			}
			void** source = reinterpret_cast<void**>(kVanillaNiNodeVTable);
			const SIZE_T predecessor = reinterpret_cast<SIZE_T>(
				source[kNiNodeOnVisibleSlot]);
			const SIZE_T hook = reinterpret_cast<SIZE_T>(
				&ViewportListNodeOnVisibleHook);
			if (!source[-1] || predecessor == hook
				|| !hook_identity::IsExecutableTarget(predecessor))
			{
				return false;
			}

			s_viewportNodeVTable.predecessor =
				reinterpret_cast<NiNodeOnVisibleFn>(predecessor);
			s_viewportNodeVTable.completeObjectLocator = source[-1];
			std::memcpy(s_viewportNodeVTable.entries.data(), source,
				sizeof(void*) * s_viewportNodeVTable.entries.size());
			s_viewportNodeVTable.entries[kNiNodeOnVisibleSlot] =
				reinterpret_cast<void*>(&ViewportListNodeOnVisibleHook);
			s_viewportNodeVTable.sourceVTable = source;
			gLog.FormattedMessage(
				"tnvse_freetype_font: conservative list viewport subtree culling active source_onvisible=%08X chained=%d safety_padding=%.1f max_depth=%u max_tiles=%u",
				static_cast<UInt32>(predecessor),
				predecessor != kVanillaNiNodeOnVisible ? 1 : 0,
				kViewportCullSafetyPadding,
				kMaximumViewportSubtreeDepth,
				kMaximumViewportSubtreeTiles);
			return true;
		}

		void InstallViewportCullForTileNode(Tile* tile, NiNode* node)
		{
			if (!node || !IsViewportListItemCandidate(tile))
				return;

			bool installed = false;
			if (FindTileForNode(node) == tile
				&& InitializeViewportNodeVTable())
			{
				void*** objectVTable = reinterpret_cast<void***>(node);
				void** current = *objectVTable;
				void** proxy = s_viewportNodeVTable.entries.data();
				if (current == proxy)
					return;
				if (current == s_viewportNodeVTable.sourceVTable)
				{
					void* observed = InterlockedCompareExchangePointer(
						reinterpret_cast<void* volatile*>(objectVTable),
						proxy, current);
					installed = observed == current && *objectVTable == proxy;
				}
				else if (!s_loggedViewportNodeVTableConflict)
				{
					s_loggedViewportNodeVTableConflict = true;
					gLog.FormattedMessage(
						"tnvse_freetype_font: list viewport candidate kept on original path because its NiNode vtable is not the retail table current=%08X expected=%08X",
						static_cast<UInt32>(
							reinterpret_cast<SIZE_T>(current)),
						static_cast<UInt32>(kVanillaNiNodeVTable));
				}
			}
			RecordFreeTypeViewportNodeInstallResult(installed);
		}

		void __fastcall ViewportListNodeOnVisibleHook(
			NiNode* node, void*, NiCullingProcess* culler)
		{
			const ViewportCullDecision decision =
				ShouldCullViewportListNode(node);
			if (decision.checked && g_bEnableFreeTypeFontRenderingLog)
			{
				RecordFreeTypeViewportCullResult(
					decision.culled, decision.failOpen,
					decision.fastVisible, decision.deepCheck,
					decision.visitedTiles);
			}
			if (decision.culled)
				return;

			NiNodeOnVisibleFn next = s_viewportNodeVTable.predecessor;
			if (!next || reinterpret_cast<SIZE_T>(next)
				== reinterpret_cast<SIZE_T>(
					&ViewportListNodeOnVisibleHook))
			{
				next = reinterpret_cast<NiNodeOnVisibleFn>(
					kVanillaNiNodeOnVisible);
			}
			next(node, culler);
		}
		Font* ResolveVuiEffectProxyFont(TileText* tile)
		{
			if (!tile)
				return nullptr;
			UInt32 fontId = 0;
			if (!TryResolveGameFontId(
				tile->GetValueFloat(Tile::kTileValue_font), fontId))
			{
				return nullptr;
			}
			return ResolveGameFont(FontManager::GetSingleton(), fontId);
		}

		bool IsVuiEffectProxy(const TileText* tile)
		{
			// VUI+'s Prefabs/VUI+/outline.xml implements its original-style dark
			// shadow/outline by cloning the source text into these two named tiles.
			// Always let those proxy tiles complete text preparation so their width
			// and height traits remain valid for anonymous sibling expressions.  When
			// tNVSE already supplies the effect, skip their glyph emission and cull
			// only the finished scene node.
			if (!tile)
				return false;
			const char* name = tile->strName.c_str();
			if (!name)
				return false;
			return _stricmp(name, "VUI+Shadow") == 0
				|| _stricmp(name, "VUI+Outline") == 0;
		}

		class ScopedEffectSuppression
		{
		public:
			explicit ScopedEffectSuppression(bool suppress) : m_suppress(suppress)
			{
				if (m_suppress)
					++s_effectSuppressionDepth;
			}

			~ScopedEffectSuppression()
			{
				if (m_suppress)
					--s_effectSuppressionDepth;
			}

		private:
			bool m_suppress;
		};

		class ScopedVuiProxyMeasureOnly
		{
		public:
			explicit ScopedVuiProxyMeasureOnly(bool enabled) : m_enabled(enabled)
			{
				if (m_enabled)
					++s_vuiProxyMeasureOnlyDepth;
			}

			~ScopedVuiProxyMeasureOnly()
			{
				if (m_enabled)
					--s_vuiProxyMeasureOnlyDepth;
			}

		private:
			bool m_enabled;
		};

		NiNode* __fastcall TileTextMakeNodeHook(TileText* tile, void*)
		{
			const bool suppress = IsVuiEffectProxy(tile);
			Font* font = suppress ? ResolveVuiEffectProxyFont(tile) : nullptr;
			const bool replaceProxy = suppress && HasEnabledFreeTypeFontEffects(font);

			ScopedEffectSuppression scope(suppress);
			ScopedVuiProxyMeasureOnly measureOnly(replaceProxy);
			BeginFreeTypeStockPageShapeCapture();
			NiNode* node = s_tileTextMakeNode ? s_tileTextMakeNode(tile) : nullptr;
			EndFreeTypeStockPageShapeCapture(node);

			if (replaceProxy && node)
				node->SetAppCulled(true);
			InstallViewportCullForTileNode(tile, node);
			return node;
		}

		NiNode* __fastcall TileRectMakeNodeHook(TileRect* tile, void*)
		{
			NiNode* node = s_tileRectMakeNode
				? s_tileRectMakeNode(tile) : nullptr;
			InstallViewportCullForTileNode(tile, node);
			return node;
		}

		NiNode* __fastcall TileImageMakeNodeHook(TileImage* tile, void*)
		{
			NiNode* node = s_tileImageMakeNode
				? s_tileImageMakeNode(tile) : nullptr;
			InstallViewportCullForTileNode(tile, node);
			return node;
		}

		bool InstallVuiEffectProxyCompatibility()
		{
			if (!hook_identity::IsAccessibleRegion(
				kTileTextMakeNodeVTableEntry, sizeof(SIZE_T), false))
			{
				gLog.FormattedMessage(
					"tnvse_freetype_font: VUI+ effect proxy compatibility skipped; TileText::MakeNode vtable entry is unreadable entry=%08X",
					static_cast<UInt32>(kTileTextMakeNodeVTableEntry));
				return false;
			}
			const SIZE_T current = *reinterpret_cast<const SIZE_T*>(
				kTileTextMakeNodeVTableEntry);
			const SIZE_T hook = reinterpret_cast<SIZE_T>(&TileTextMakeNodeHook);
			if (current == hook)
				return s_tileTextMakeNode != nullptr;
			if (!hook_identity::IsExecutableTarget(current))
			{
				gLog.FormattedMessage(
					"tnvse_freetype_font: VUI+ effect proxy compatibility skipped; TileText::MakeNode target is not executable target=%08X",
					static_cast<UInt32>(current));
				return false;
			}
			s_tileTextMakeNode = reinterpret_cast<TileTextMakeNodeFn>(current);
			SafeWrite32(kTileTextMakeNodeVTableEntry, hook);
			if (*reinterpret_cast<const SIZE_T*>(
				kTileTextMakeNodeVTableEntry) != hook)
			{
				SafeWrite32(kTileTextMakeNodeVTableEntry, current);
				s_tileTextMakeNode = nullptr;
				gLog.FormattedMessage(
					"tnvse_freetype_font: VUI+ effect proxy compatibility write verification failed entry=%08X",
					static_cast<UInt32>(kTileTextMakeNodeVTableEntry));
				return false;
			}
			gLog.FormattedMessage(
				"tnvse_freetype_font: VUI+ effect proxy compatibility installed entry=%08X target=%08X chained=%d",
				static_cast<UInt32>(kTileTextMakeNodeVTableEntry),
				static_cast<UInt32>(current), current != kVanillaTileTextMakeNode ? 1 : 0);
			return true;
		}

		template <class MakeNodeFn>
		bool InstallViewportMakeNodeHook(
			SIZE_T entry, SIZE_T stockTarget, const char* tileType,
			SIZE_T hookTarget, MakeNodeFn& predecessor)
		{
			if (!hook_identity::IsAccessibleRegion(
				entry, sizeof(SIZE_T), false))
			{
				gLog.FormattedMessage(
					"tnvse_freetype_font: list viewport %s hook skipped; MakeNode vtable entry is unreadable entry=%08X",
					tileType, static_cast<UInt32>(entry));
				return false;
			}
			const SIZE_T current =
				*reinterpret_cast<const SIZE_T*>(entry);
			if (current == hookTarget)
				return predecessor != nullptr;
			if (!hook_identity::IsExecutableTarget(current))
			{
				gLog.FormattedMessage(
					"tnvse_freetype_font: list viewport %s hook skipped; MakeNode target is not executable target=%08X",
					tileType, static_cast<UInt32>(current));
				return false;
			}

			predecessor = reinterpret_cast<MakeNodeFn>(current);
			SafeWrite32(entry, hookTarget);
			if (*reinterpret_cast<const SIZE_T*>(entry) != hookTarget)
			{
				SafeWrite32(entry, current);
				predecessor = nullptr;
				gLog.FormattedMessage(
					"tnvse_freetype_font: list viewport %s hook write verification failed entry=%08X",
					tileType, static_cast<UInt32>(entry));
				return false;
			}
			gLog.FormattedMessage(
				"tnvse_freetype_font: list viewport %s hook installed entry=%08X target=%08X chained=%d",
				tileType, static_cast<UInt32>(entry),
				static_cast<UInt32>(current),
				current != stockTarget ? 1 : 0);
			return true;
		}

		void InstallViewportListSubtreeCulling()
		{
			if (!InitializeViewportNodeVTable())
			{
				gLog.FormattedMessage(
					"tnvse_freetype_font: conservative list viewport subtree culling skipped; retail NiNode vtable identity is unavailable");
				return;
			}

			InstallViewportMakeNodeHook(
				kTileRectMakeNodeVTableEntry, kVanillaTileRectMakeNode,
				"TileRect", reinterpret_cast<SIZE_T>(&TileRectMakeNodeHook),
				s_tileRectMakeNode);
			InstallViewportMakeNodeHook(
				kTileImageMakeNodeVTableEntry, kVanillaTileImageMakeNode,
				"TileImage", reinterpret_cast<SIZE_T>(&TileImageMakeNodeHook),
				s_tileImageMakeNode);
		}
	}

	Font* CallOriginalFontInit(Font* font, int fontNum, char* filename, bool load)
	{
		return s_originalFontInit
			? s_originalFontInit(font, fontNum, filename, load) : nullptr;
	}

	void CallOriginalFontLoad(Font* font)
	{
		if (s_originalFontLoad)
			s_originalFontLoad(font);
	}

	void CallOriginalFontCreateText(
		Font* font, BSStringT<char>* text, int* width, int* height,
		int lineStart, int lineEnd, int flags, char lineBreak,
		const NiColorA* color, NiTriShape** textShape, NiTriShape** iconShape)
	{
		if (s_originalFontCreateText)
		{
			s_originalFontCreateText(font, text, width, height, lineStart,
				lineEnd, flags, lineBreak, color, textShape, iconShape);
		}
	}

	NiAVObject* CallOriginalFontMakeString(
		Font* font, float startX, float startY, float z,
		BSStringT<char>* text, int* width, bool prepareObject,
		const NiColorA* color, bool upperLeftCorner, bool prepareObjectFinal)
	{
		return s_originalFontMakeString
			? s_originalFontMakeString(font, startX, startY, z, text, width,
				prepareObject, color, upperLeftCorner, prepareObjectFinal) : nullptr;
	}

	NiPoint3* CallOriginalCalculateStringDimensions(
		FontManager* manager, NiPoint3* dimensions, const char* text,
		UInt32 fontId, float maxWrapWidth, UInt32 startCharIndex)
	{
		return s_originalCalculateStringDimensions
			? s_originalCalculateStringDimensions(manager, dimensions, text,
				fontId, maxWrapWidth, startCharIndex) : dimensions;
	}

	bool AreMultibyteFontHooksInstalled()
	{
		return s_fontHookInstallState.multibyte;
	}

	bool AreFreeTypeFontHooksInstalled()
	{
		return s_fontHookInstallState.freeType;
	}

	bool IsFreeTypeEffectSuppressionActive()
	{
		return s_effectSuppressionDepth != 0;
	}

	bool IsFreeTypeVuiProxyMeasureOnlyActive()
	{
		return s_vuiProxyMeasureOnlyDepth != 0;
	}

	void InitBigGunsDescHooks()
	{
		constexpr SIZE_T kJipInstruction = 0x100113BD;
		constexpr SIZE_T kJipImmediate = kJipInstruction + 1;
		constexpr SIZE_T kJipStockDescription = 0x1005D130;
		const SIZE_T instruction = GetJIPAddress(kJipInstruction);
		const SIZE_T immediate = GetJIPAddress(kJipImmediate);
		const UInt32 stockDescription = static_cast<UInt32>(
			GetJIPAddress(kJipStockDescription));
		if (!hook_identity::IsAccessibleRegion(instruction, 5, true)
			|| *reinterpret_cast<const UInt8*>(instruction) != 0xBA
			|| *reinterpret_cast<const UInt32*>(immediate)
				!= stockDescription)
		{
			gLog.FormattedMessage(
				"tnvse_font_hook: JIP Big Guns description signature mismatch instruction=%08X; code left untouched",
				static_cast<UInt32>(instruction));
			return;
		}

		static std::string sConvertedBigGunsDesc = IsEastAsianUiMode()
			? UTF8ToMultiByteStr(g_sNewBigGunsDesc, g_usingWinEncoding)
			: g_sNewBigGunsDesc;
		const UInt32 replacement = reinterpret_cast<UInt32>(
			sConvertedBigGunsDesc.c_str());
		SafeWrite32(immediate, replacement);
		if (*reinterpret_cast<const UInt32*>(immediate) != replacement)
		{
			SafeWrite32(immediate, stockDescription);
			gLog.FormattedMessage(
				"tnvse_font_hook: JIP Big Guns description write verification failed; restored stock operand");
		}
	}

	static bool InstallDoorPromptHook(SIZE_T hook, const char* mode)
	{
		const std::array<Rel32Site, 1> sites = {{ kDoorPromptCallSite }};
		if (!ValidateStockCallSites(sites))
			return false;
		WriteRelCall(kDoorPromptCallSite.address, hook);
		if (hook_identity::MatchesRel32Target(
			kDoorPromptCallSite.address, Rel32Opcode::Call, hook))
		{
			return true;
		}
		WriteRelCall(kDoorPromptCallSite.address,
			kDoorPromptCallSite.stockTarget);
		gLog.FormattedMessage(
			"tnvse_font_hook: door prompt %s hook write verification failed; restored BSsprintf",
			mode);
		return false;
	}

	void InitDoorPromptHooksCHS()
	{
		InstallDoorPromptHook(
			reinterpret_cast<SIZE_T>(&BSsprintfHookCHS), "CHS");
	}

	void InitDoorPromptHooksKOR()
	{
		InstallDoorPromptHook(
			reinterpret_cast<SIZE_T>(&BSsprintfHookKOR), "KOR");
	}

	void InitPluralHooks()
	{
		constexpr SIZE_T kPluralBranch = 0x753E39;
		if (!hook_identity::IsAccessibleRegion(
				kPluralBranch, sizeof(UInt8), true)
			|| *reinterpret_cast<const UInt8*>(kPluralBranch) != 0x74)
		{
			const UInt32 actual = hook_identity::IsAccessibleRegion(
				kPluralBranch, sizeof(UInt8), true)
				? *reinterpret_cast<const UInt8*>(kPluralBranch)
				: 0xFFFFFFFFu;
			gLog.FormattedMessage(
				"tnvse_font_hook: identity mismatch site=plural branch address=00753E39 expected=74 actual=%08X",
				actual);
			return;
		}
		SafeWrite8(kPluralBranch, 0xEB);
		if (*reinterpret_cast<const UInt8*>(kPluralBranch) != 0xEB)
		{
			SafeWrite8(kPluralBranch, 0x74);
			gLog.FormattedMessage(
				"tnvse_font_hook: plural branch write verification failed; restored stock branch");
		}
	}

	FontHookInstallState InitFontHooks()
	{
		s_fontHookInstallState = {};
		if (!g_bEnableMultibyteFontHook && !g_bEnableFreeTypeFontRendering)
		{
			gLog.FormattedMessage("tnvse_font_hook: all font hooks disabled by tnvse.ini");
			return s_fontHookInstallState;
		}
		if (!ValidateRequiredFontHookSites())
		{
			gLog.FormattedMessage(
				"tnvse_font_hook: installation aborted before patching because the retail hook graph does not match FalloutNV.exe 1.4.0.525");
			return s_fontHookInstallState;
		}
		if (!InstallCoreFontEntryHooks())
		{
			if (g_bEnableMultibyteFontHook)
				gLog.FormattedMessage(
					"tnvse_font_hook: multibyte capability unavailable because core entry validation failed");
			if (g_bEnableFreeTypeFontRendering)
				gLog.FormattedMessage(
					"tnvse_font_hook: freetype capability unavailable because core entry validation failed");
			return s_fontHookInstallState;
		}

		s_fontHookInstallState.multibyte = g_bEnableMultibyteFontHook;
		s_fontHookInstallState.freeType = g_bEnableFreeTypeFontRendering;
		if (s_fontHookInstallState.freeType)
		{
			InstallVuiEffectProxyCompatibility();
			InstallViewportListSubtreeCulling();
		}

		// FontManager::CreateText -> FontManager::PrepText
		WriteRelCallEx(0xA18F4A, &FontManagerEx::PrepText);
		// FontManager::CreateText -> TextDoc::Render
		WriteRelCallEx(0xA18F63, &FontManagerEx::TextDocRender);
		// TextDoc::Render -> Font::AddChar
		WriteRelCallEx(0xA19622, &FontEx::TextDocRenderAddChar);
		// Terminal text needs the custom single-byte FreeType preparation path
		// even when the global DBCS parser is disabled. Non-FreeType fonts are
		// delegated by FontEx::PrepTextForTerminal.
		WriteRelCallEx(0x759281, &FontEx::PrepTextForTerminal);
		if (s_fontHookInstallState.multibyte)
		{
			// AnimatingText::Update normally treats its elapsed-interval count as
			// a byte count.  Interpret it as encoded units at the single memcpy
			// call site so a DBCS lead byte is never published on its own.
			WriteRelCall(0x6FFFEE, &CopyAnimatingTextEncodedUnits);
		}
		// Feed final FreeType widths into word wrapping before TextLine chooses
		// whether to retain the character, move a word, or create another line.
		WriteRelCallEx(0xA19C80, &FontManagerEx::TextLineAddChar);

		if (!s_fontHookInstallState.multibyte)
		{
			// TextLine's constructor inserts the first character through a
			// separate call site. Patch it only for FreeType-only mode so every
			// line starts with the same final-width contract, while the enabled
			// multibyte path remains byte-for-byte on its existing hook set.
			WriteRelCallEx(0xA1BDE2, &FontManagerEx::TextLineAddChar);
			gLog.FormattedMessage(
				"tnvse_font_hook: installed mode=freetype-custom-single-byte configuredCodePage=%u freeTypeCodePage=%u",
				g_usingWinEncoding, GetFreeTypeTextCodePage());
			return s_fontHookInstallState;
		}

		WriteRelJumpEx(kFontPrepText, &FontEx::PrepText);

		// FontManager::PrepText -> FontManager::PrepHypertext
		WriteRelCallEx(0xA18ACC, &FontManagerEx::PrepHypertext);

		// FontManager::PrepHypertext -> CollectTo
		WriteRelCall(0xA1772D, &FontManagerEx::CollectTo);
		WriteRelCall(0xA17835, &FontManagerEx::CollectTo);
		WriteRelCall(0xA17A1E, &FontManagerEx::CollectTo);
		WriteRelCall(0xA17B65, &FontManagerEx::CollectTo);
		WriteRelCall(0xA17BB1, &FontManagerEx::CollectTo);
		WriteRelCall(0xA17CFE, &FontManagerEx::CollectTo);
		WriteRelCall(0xA17D5D, &FontManagerEx::CollectToAttributeValue);
		WriteRelCall(0xA17DE9, &FontManagerEx::CollectToAttributeValue);

		// FontManager::CreateText -> TextDoc::Destroy
		WriteRelCallEx(0xA18F7D, &FontManagerEx::TextDocDestroy);

		// FontManager::PrepHypertext -> TextDoc::AddChar
		WriteRelCallEx(0xA178A4, &FontManagerEx::TextDocAddChar);
		WriteRelCallEx(0xA179D9, &FontManagerEx::TextDocAddChar);
		WriteRelCallEx(0xA17FC2, &FontManagerEx::TextDocAddChar);
		// FontManager::PrepText -> TextDoc::AddChar
		WriteRelCallEx(0xA18D7C, &FontManagerEx::TextDocAddChar);

		// TextDoc::AddChar -> TextPage::AddChar
		WriteRelCallEx(0xA19A6F, &FontManagerEx::TextPageAddChar);
		// TextPage::TextPage -> TextPage::AddChar
		WriteRelCallEx(0xA1BD1C, &FontManagerEx::TextPageAddChar);

		// FontManager::PrepHypertext -> CharData::Copy
		WriteRelCall(0xA17898, &FontManagerEx::CharDataCopy);
		WriteRelCall(0xA179CD, &FontManagerEx::CharDataCopy);
		WriteRelCall(0xA17FB6, &FontManagerEx::CharDataCopy);
		// FontManager::PrepText -> CharData::Copy
		WriteRelCall(0xA18D73, &FontManagerEx::CharDataCopy);

		// Tile::SetString - Quest Text
		WriteRelCall(0x77AF4B, &TileSetStringHookForQueueText);
		// Tile::SetString - Location Text
		WriteRelCall(0x772B5E, &TileSetStringHookForQueueText);

		// BSStringT<char>::c_str - Terminal UTF8 conversion
		WriteRelCall(0x7591AC, &BSString_c_strHook);

		// BSStringT<char>::GetCStringOrEmpty - Location Text UTF8 conversion
		WriteRelCall(0x772B4B, &BSString_GetCStringOrEmptyHook);

		// strcpy_s - Quest Text UTF8 conversion
		WriteRelCall(0x77ACCC, &strcpy_sHook);
		WriteRelCall(0x77ACF8, &strcpy_sHook);

		gLog.FormattedMessage(
			"tnvse_font_hook: installed mode=%s configuredCodePage=%u freeTypeCodePage=%u",
			s_fontHookInstallState.freeType ? "multibyte-freetype" : "multibyte-original",
			g_usingWinEncoding, GetFreeTypeTextCodePage());
		return s_fontHookInstallState;
	}

} // namespace fonthook

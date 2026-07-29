#include "font_engine.h"
#include "font_manager.h"
#include "font_vector.h"
#include "game_hooks.h"
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
#include <cstddef>
#include <cstring>
#include <limits>

namespace fonthook
{
	namespace
	{
		using FontInitFn = Font* (__thiscall*)(Font*, int, char*, bool);
		using FontLoadFn = void (__thiscall*)(Font*);
		using FontCreateTextFn = UInt32 (__thiscall*)(Font*, BSStringT<char>*,
			int*, int*, int, int, int, char, const NiColorA*, NiTriShape**, NiTriShape**);
		using FontMakeStringFn = NiTriShape* (__thiscall*)(Font*, float, float,
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

		bool BuildTrampoline(PendingTrampoline& trampoline)
		{
			if (!trampoline.source || !trampoline.expected || trampoline.length < 5
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
				{ 0xA12020, kFontInitPrologue.data(), kFontInitPrologue.size() },
				{ 0xA15320, kFontLoadPrologue.data(), kFontLoadPrologue.size() },
				{ 0xA12880, kFontCreateTextPrologue.data(), kFontCreateTextPrologue.size() },
				{ 0xA12460, kFontMakeStringPrologue.data(), kFontMakeStringPrologue.size() },
				{ 0xA1B020, kCalculateDimensionsPrologue.data(), kCalculateDimensionsPrologue.size() }
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

			WriteRelJumpEx(0xA12020, &FontEx::FontInit);
			WriteRelJumpEx(0xA15320, &FontEx::Load);
			WriteRelJump(0xA12880,
				reinterpret_cast<UInt32>(&FreeTypeCreateTextEntryHook));
			WriteRelJumpEx(0xA12460, &FontEx::MakeString);
			PatchMemoryNop(0xA12465, kFontMakeStringPrologue.size() - 5);
			WriteRelJumpEx(0xA1B020, &FontManagerEx::CalculateStringDimensions);
			PatchMemoryNop(0xA1B025, kCalculateDimensionsPrologue.size() - 5);
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
		constexpr size_t kNiNodeVirtualCount = 64;
		constexpr size_t kNiNodeOnVisibleSlot = 53;
		constexpr UInt32 kMaximumTileAncestorDepth = 64;
		constexpr UInt32 kMaximumViewportSubtreeDepth = 32;
		constexpr UInt32 kMaximumViewportSubtreeTiles = 256;
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
			Tile* tile;
			NiNode* node;
		};

		struct ViewportNodeVTableProxy
		{
			void** sourceVTable = nullptr;
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
		bool s_loggedVuiShadowBypass = false;
		bool s_loggedVuiOutlineBypass = false;
		bool s_loggedVuiShadowFallback = false;
		bool s_loggedVuiOutlineFallback = false;
		bool s_loggedViewportNodeVTableConflict = false;

		void __fastcall ViewportListNodeOnVisibleHook(
			NiNode* node, void*, NiCullingProcess* culler);

		bool IsFiniteTraitValue(const Tile::Value* value)
		{
			return value && std::isfinite(value->fNum);
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
			if (!tile || !tile->GetValue(Tile::kTileValue_listindex))
				return false;
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
			for (UInt16 i = 0; i < node->m_usExtraDataSize; ++i)
			{
				NiExtraData* extra = node->m_ppkExtra[i];
				if (!extra
					|| *reinterpret_cast<const SIZE_T*>(extra)
						!= kTileNodeExtraVTable)
				{
					continue;
				}
				const TileNodeExtraView* view =
					reinterpret_cast<const TileNodeExtraView*>(extra);
				if (view->node == node)
					return view->tile;
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
			return reinterpret_cast<TileAbsoluteYFn>(0xA01440)(tile);
		}

		bool AccumulateViewportSubtreeBounds(
			Tile* tile, UInt32 depth, TileVerticalBounds& bounds)
		{
			if (!tile || depth > kMaximumViewportSubtreeDepth
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

			Tile::Value* height =
				tile->GetValue(Tile::kTileValue_height);
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
					if (!std::isfinite(top) || !std::isfinite(bottom))
						return false;
					bounds.top = std::min(bounds.top, top);
					bounds.bottom = std::max(bounds.bottom, bottom);
					bounds.hasArea = true;
				}
			}

			for (Tile* child : tile->kChildren)
			{
				if (child && !AccumulateViewportSubtreeBounds(
					child, depth + 1, bounds))
				{
					return false;
				}
			}
			return true;
		}

		bool IsOutsidePaddedViewport(
			float top, float bottom, float clipTop, float clipBottom)
		{
			return bottom < clipTop - kViewportCullSafetyPadding
				|| top > clipBottom + kViewportCullSafetyPadding;
		}

		bool ShouldCullViewportListNode(
			NiNode* node, bool& checked, bool& failOpen)
		{
			checked = false;
			failOpen = false;
			Tile* tile = FindTileForNode(node);
			if (!tile)
				return false;

			Tile::Value* listIndex =
				tile->GetValue(Tile::kTileValue_listindex);
			if (!listIndex)
				return false;
			if (!std::isfinite(listIndex->fNum))
			{
				checked = true;
				failOpen = true;
				return false;
			}
			if (listIndex->fNum < 0.0f)
				return false;

			Tile::Value* clips = tile->GetValue(Tile::kTileValue_clips);
			if (!clips || clips->fNum <= 0.5f)
				return false;
			checked = true;
			if (!std::isfinite(clips->fNum))
			{
				failOpen = true;
				return false;
			}

			bool chainValid = true;
			Tile* clipWindow = FindNearestClipWindow(tile, chainValid);
			if (!chainValid || !clipWindow
				|| !HasSafeTileChain(tile, clipWindow))
			{
				failOpen = true;
				return false;
			}

			Tile::Value* clipHeight =
				clipWindow->GetValue(Tile::kTileValue_height);
			if (!IsFiniteTraitValue(clipHeight)
				|| clipHeight->fNum <= 0.0f)
			{
				failOpen = true;
				return false;
			}
			const float clipTop = GetAbsoluteTileY(clipWindow);
			const float clipBottom = clipTop + clipHeight->fNum;
			if (!std::isfinite(clipTop) || !std::isfinite(clipBottom))
			{
				failOpen = true;
				return false;
			}

			Tile::Value* rootHeight =
				tile->GetValue(Tile::kTileValue_height);
			if (rootHeight && std::isfinite(rootHeight->fNum)
				&& rootHeight->fNum > 0.0f)
			{
				const float rootTop = GetAbsoluteTileY(tile);
				const float rootBottom = rootTop + rootHeight->fNum;
				if (!std::isfinite(rootTop) || !std::isfinite(rootBottom))
				{
					failOpen = true;
					return false;
				}
				if (!IsOutsidePaddedViewport(rootTop, rootBottom,
					clipTop, clipBottom))
				{
					return false;
				}
			}

			TileVerticalBounds bounds;
			if (!AccumulateViewportSubtreeBounds(tile, 0, bounds)
				|| !bounds.hasArea)
			{
				failOpen = true;
				return false;
			}
			return IsOutsidePaddedViewport(
				bounds.top, bounds.bottom, clipTop, clipBottom);
		}

		bool InitializeViewportNodeVTable()
		{
			if (s_viewportNodeVTable.sourceVTable)
				return true;

			void** source =
				reinterpret_cast<void**>(kVanillaNiNodeVTable);
			const SIZE_T hook =
				reinterpret_cast<SIZE_T>(&ViewportListNodeOnVisibleHook);
			if (!source || !source[kNiNodeOnVisibleSlot]
				|| reinterpret_cast<SIZE_T>(
					source[kNiNodeOnVisibleSlot]) == hook)
			{
				return false;
			}

			s_viewportNodeVTable.sourceVTable = source;
			s_viewportNodeVTable.completeObjectLocator = source[-1];
			std::memcpy(s_viewportNodeVTable.entries.data(), source,
				sizeof(void*) * s_viewportNodeVTable.entries.size());
			s_viewportNodeVTable.entries[kNiNodeOnVisibleSlot] =
				reinterpret_cast<void*>(&ViewportListNodeOnVisibleHook);
			gLog.FormattedMessage(
				"tnvse_freetype_font: list viewport subtree culling active source_onvisible=%08X chained=%d safety_padding=%.1f",
				static_cast<UInt32>(reinterpret_cast<SIZE_T>(
					source[kNiNodeOnVisibleSlot])),
				reinterpret_cast<SIZE_T>(
					source[kNiNodeOnVisibleSlot])
					!= kVanillaNiNodeOnVisible ? 1 : 0,
				kViewportCullSafetyPadding);
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
				void*** objectVTable =
					reinterpret_cast<void***>(node);
				void** current = *objectVTable;
				if (current == s_viewportNodeVTable.entries.data())
					return;
				if (current == s_viewportNodeVTable.sourceVTable)
				{
					*objectVTable = s_viewportNodeVTable.entries.data();
					installed = true;
				}
				else if (!s_loggedViewportNodeVTableConflict)
				{
					s_loggedViewportNodeVTableConflict = true;
					gLog.FormattedMessage(
						"tnvse_freetype_font: list viewport subtree culling left a candidate on its original path because its NiNode vtable is not the retail table current=%08X expected=%08X",
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
			bool checked = false;
			bool failOpen = false;
			const bool culled = ShouldCullViewportListNode(
				node, checked, failOpen);
			if (checked && g_bEnableFreeTypeFontRenderingLog)
				RecordFreeTypeViewportCullResult(culled, failOpen);
			if (culled)
				return;

			NiNodeOnVisibleFn next = nullptr;
			if (s_viewportNodeVTable.sourceVTable)
			{
				next = reinterpret_cast<NiNodeOnVisibleFn>(
					s_viewportNodeVTable.sourceVTable[
						kNiNodeOnVisibleSlot]);
			}
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

		bool IsVuiEffectProxy(const TileText* tile, bool& isOutline)
		{
			// VUI+'s Prefabs/VUI+/outline.xml implements its original-style dark
			// shadow/outline by cloning the source text into these two named tiles.
			// Always let those proxy tiles complete text preparation so their width
			// and height traits remain valid for anonymous sibling expressions.  When
			// tNVSE already supplies the effect, skip their glyph emission and cull
			// only the finished scene node.
			isOutline = false;
			if (!tile)
				return false;
			const char* name = tile->strName.c_str();
			if (!name)
				return false;
			if (_stricmp(name, "VUI+Shadow") == 0)
				return true;
			if (_stricmp(name, "VUI+Outline") == 0)
			{
				isOutline = true;
				return true;
			}
			return false;
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
			bool isOutline = false;
			const bool suppress = IsVuiEffectProxy(tile, isOutline);
			Font* font = suppress ? ResolveVuiEffectProxyFont(tile) : nullptr;
			const bool replaceProxy = suppress && HasEnabledFreeTypeFontEffects(font);

			ScopedEffectSuppression scope(suppress);
			ScopedVuiProxyMeasureOnly measureOnly(replaceProxy);
			BeginFreeTypeStockPageShapeCapture();
			NiNode* node = s_tileTextMakeNode ? s_tileTextMakeNode(tile) : nullptr;
			EndFreeTypeStockPageShapeCapture(node);

			if (replaceProxy && node)
			{
				node->SetAppCulled(true);
				bool& logged = isOutline
					? s_loggedVuiOutlineBypass : s_loggedVuiShadowBypass;
				if (!logged)
				{
					logged = true;
					gLog.FormattedMessage(
						"tnvse_freetype_font: preserved layout metrics and culled VUI+ effect proxy tile=%s font=%d",
						tile->strName.c_str(), font->iFontNum);
				}
			}
			else if (suppress)
			{
				bool& logged = isOutline
					? s_loggedVuiOutlineFallback : s_loggedVuiShadowFallback;
				if (!logged)
				{
					logged = true;
					gLog.FormattedMessage(
						"tnvse_freetype_font: continuing chained font pipeline with recursive effects suppressed for VUI+ proxy tile=%s",
						tile->strName.c_str());
				}
			}
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

		void InstallVuiEffectProxyCompatibility()
		{
			const SIZE_T current = *reinterpret_cast<const SIZE_T*>(
				kTileTextMakeNodeVTableEntry);
			const SIZE_T hook = reinterpret_cast<SIZE_T>(&TileTextMakeNodeHook);
			if (current == hook)
				return;
			if (!current)
			{
				gLog.FormattedMessage(
					"tnvse_freetype_font: VUI+ effect proxy compatibility skipped; TileText::MakeNode is null");
				return;
			}
			s_tileTextMakeNode = reinterpret_cast<TileTextMakeNodeFn>(current);
			SafeWrite32(kTileTextMakeNodeVTableEntry, hook);
			gLog.FormattedMessage(
				"tnvse_freetype_font: VUI+ effect proxy compatibility installed entry=%08X target=%08X chained=%d",
				static_cast<UInt32>(kTileTextMakeNodeVTableEntry),
				static_cast<UInt32>(current), current != kVanillaTileTextMakeNode ? 1 : 0);
		}

		void InstallViewportListSubtreeCulling()
		{
			const SIZE_T rectCurrent =
				*reinterpret_cast<const SIZE_T*>(
					kTileRectMakeNodeVTableEntry);
			const SIZE_T rectHook =
				reinterpret_cast<SIZE_T>(&TileRectMakeNodeHook);
			if (rectCurrent != rectHook)
			{
				if (rectCurrent)
				{
					s_tileRectMakeNode =
						reinterpret_cast<TileRectMakeNodeFn>(
							rectCurrent);
					SafeWrite32(kTileRectMakeNodeVTableEntry, rectHook);
					gLog.FormattedMessage(
						"tnvse_freetype_font: list viewport TileRect hook installed entry=%08X target=%08X chained=%d",
						static_cast<UInt32>(
							kTileRectMakeNodeVTableEntry),
						static_cast<UInt32>(rectCurrent),
						rectCurrent != kVanillaTileRectMakeNode ? 1 : 0);
				}
				else
				{
					gLog.FormattedMessage(
						"tnvse_freetype_font: list viewport TileRect hook skipped because MakeNode is null");
				}
			}

			const SIZE_T imageCurrent =
				*reinterpret_cast<const SIZE_T*>(
					kTileImageMakeNodeVTableEntry);
			const SIZE_T imageHook =
				reinterpret_cast<SIZE_T>(&TileImageMakeNodeHook);
			if (imageCurrent != imageHook)
			{
				if (imageCurrent)
				{
					s_tileImageMakeNode =
						reinterpret_cast<TileImageMakeNodeFn>(
							imageCurrent);
					SafeWrite32(kTileImageMakeNodeVTableEntry, imageHook);
					gLog.FormattedMessage(
						"tnvse_freetype_font: list viewport TileImage hook installed entry=%08X target=%08X chained=%d",
						static_cast<UInt32>(
							kTileImageMakeNodeVTableEntry),
						static_cast<UInt32>(imageCurrent),
						imageCurrent != kVanillaTileImageMakeNode ? 1 : 0);
				}
				else
				{
					gLog.FormattedMessage(
						"tnvse_freetype_font: list viewport TileImage hook skipped because MakeNode is null");
				}
			}
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

	UInt32 CallOriginalFontCreateText(
		Font* font, BSStringT<char>* text, int* width, int* height,
		int lineStart, int lineEnd, int flags, char lineBreak,
		const NiColorA* color, NiTriShape** textShape, NiTriShape** iconShape)
	{
		return s_originalFontCreateText
			? s_originalFontCreateText(font, text, width, height, lineStart,
				lineEnd, flags, lineBreak, color, textShape, iconShape) : 0;
	}

	NiTriShape* CallOriginalFontMakeString(
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
		static std::string sConvertedBigGunsDesc = IsEastAsianUiMode()
			? UTF8ToMultiByteStr(g_sNewBigGunsDesc, g_usingWinEncoding)
			: g_sNewBigGunsDesc;
		SafeWrite32(GetJIPAddress(0x100113BD + 1), (UInt32)sConvertedBigGunsDesc.c_str());
		gLog.FormattedMessage("g_sNewBigGunsDesc: %s", sConvertedBigGunsDesc.c_str());
	}

	void InitDoorPromptHooksCHS()
	{
		WriteRelCall(0x777006, &BSsprintfHookCHS);
	}

	void InitDoorPromptHooksKOR()
	{
		WriteRelCall(0x777006, &BSsprintfHookKOR);
	}

	void InitPluralHooks()
	{
		SafeWrite8(0x753E39, 0xEB);
	}

	void InitVertSpacingHook()
	{
		// FontManager::GetLinePadding
		//WriteRelJump(0xA1B3A0, &VertSpacingAdjust);
	}

	FontHookInstallState InitFontHooks()
	{
		s_fontHookInstallState = {};
		if (!g_bEnableMultibyteFontHook && !g_bEnableFreeTypeFontRendering)
		{
			gLog.FormattedMessage("tnvse_font_hook: all font hooks disabled by tnvse.ini");
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

		WriteRelJumpEx(0xA12FB0, &FontEx::PrepText);

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

#include "native_tile_overlay.h"

#include "BSMemory.hpp"
#include "font_glyphs.h"
#include "font_manager.h"
#include "InterfaceManager.hpp"
#include "Menu.hpp"
#include "SafeWrite.h"
#include "Tile.hpp"
#include "TileMenu.hpp"
#include "load_config.h"
#include "tnvse.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <limits>
#include <string_view>
#include <vector>

namespace fonthook
{
	namespace
	{
		constexpr const char* kImeOverlayXmlPath =
			"Data\\Menus\\prefabs\\tNVSE\\ImeOverlay.xml";
		constexpr const char* kPrewarmOverlayXmlPath =
			"Data\\Menus\\prefabs\\tNVSE\\FontPrewarmOverlay.xml";
		constexpr size_t kImeLineCount = 11;
		constexpr size_t kImeHighlightCount = 9;
		constexpr float kImeMinimumLineHeight = 24.0f;
		constexpr float kImeLineGap = 2.0f;
		constexpr float kImeVerticalPadding = 10.0f;
		constexpr float kImeMinimumWidth = 320.0f;
		constexpr float kImeMaximumWidth = 620.0f;
		constexpr float kImeHorizontalPadding = 20.0f;
		constexpr UInt32 kImeVisualBoundsLogLimit = 64;
		constexpr float kPrewarmMinimumTextHeight = 24.0f;
		constexpr float kPrewarmMaximumPanelWidth = 620.0f;
		constexpr float kPrewarmMinimumPanelWidth = 320.0f;
		constexpr UInt32 kReadTileXml = 0xA01B00;
		constexpr UInt32 kReleaseTileTree = 0x9FF690;
		constexpr UInt32 kRegisterMenuTile = 0xA1DC70;
		constexpr SIZE_T kLoadingMenuSingleton = 0x11DA0C0;
		// TileMenu::PostParse and the stock visibility table only accept
		// Menu Codes 1001-1084. 1079 is the highest unused gap immediately
		// below SlotMachineMenu (1080), and keeps the overlay inside the
		// engine's native menu ownership without colliding with stock menus.
		constexpr UInt32 kImeMenuClass = 1079;
		static_assert(kImeMenuClass >= 1001 && kImeMenuClass <= 1084);
		constexpr SIZE_T kCreateMenuByClassCall = 0x7079A3;
		constexpr SIZE_T kMenuConstructor = 0xA1C4A0;
		constexpr SIZE_T kMenuBaseVtable = 0x1095484;
		constexpr size_t kMenuBaseVtableEntryCount = 18;
		constexpr size_t kMenuGetIdVtableIndex = 13;
		constexpr SInt32 kImeMenuDepthContribution = -1000000;
		// FOPipboyManager vtable slot 3 is FORenderedMenu::Render
		// (FalloutNV.exe 0x7FBA00). It captures the UI into the Pip-Boy's
		// 1280x960 render target before the ordinary screen-space UI pass.
		constexpr SIZE_T kPipboyRenderVtableEntry = 0x10780B8;
		constexpr SIZE_T kVanillaRenderedMenuRender = 0x7FBA00;

		using CreateMenuByClassFn =
			Menu* (__thiscall*)(void*, UInt32);
		using RenderedMenuRenderFn =
			char* (__thiscall*)(void*, int, int, int);

		struct NativeTileOverlayState
		{
			Tile* imeParent = nullptr;
			Tile* prewarmParent = nullptr;

			Tile* imeRoot = nullptr;
			Tile* imeBackground = nullptr;
			std::array<Tile*, kImeLineCount> imeLines = {};
			std::array<Tile*, kImeHighlightCount> imeHighlights = {};
			std::wstring imeKey;
			float imeLineHeight = 0.0f;
			float imeContentWidth = 0.0f;
			bool imeLoadFailed = false;
			bool imeVisible = false;

			Tile* prewarmRoot = nullptr;
			Tile* prewarmShade = nullptr;
			Tile* prewarmPanel = nullptr;
			Tile* prewarmTrack = nullptr;
			Tile* prewarmDetail = nullptr;
			Tile* prewarmStage = nullptr;
			Tile* prewarmFill = nullptr;
			Tile* prewarmPercent = nullptr;
			Tile* prewarmTitle = nullptr;
			std::array<float, 6> prewarmLayoutSignature = {};
			float prewarmProgressWidth = 520.0f;
			bool prewarmLoadFailed = false;
			bool prewarmParentUnavailableLogged = false;
			bool prewarmTileVisible = false;
		};

		NativeTileOverlayState s_state;
		std::atomic_bool s_imeReady = false;
		std::atomic_bool s_prewarmReady = false;
		std::atomic_bool s_prewarmActive = false;
		// Preserve the MSVC complete-object locator at vtable[-1] as well as
		// the 18 virtual entries used by Menu.
		std::array<SIZE_T, kMenuBaseVtableEntryCount + 1>
			s_imeMenuVtable = {};
		CreateMenuByClassFn s_originalCreateMenuByClass = nullptr;
		RenderedMenuRenderFn s_originalPipboyRender = nullptr;
		bool s_imeMenuFactoryInstalled = false;
		bool s_imeMenuFactoryInstallFailed = false;
		bool s_pipboyRenderHookInstalled = false;
		bool s_pipboyRenderHookInstallFailed = false;
		bool s_loggedPipboyRttExclusion = false;
		bool s_creatingImeMenu = false;
		UInt32 s_imeVisualBoundsLogCount = 0;

		UInt32 __fastcall ImeMenuGetId(Menu*, void*)
		{
			return kImeMenuClass;
		}

		Menu* CreateImeMenu()
		{
			void* storage = BSNew(sizeof(Menu));
			if (!storage)
				return nullptr;

			Menu* menu = ThisStdCall<Menu*>(
				kMenuConstructor, storage);
			if (!menu)
			{
				BSFree(storage);
				return nullptr;
			}

			*reinterpret_cast<SIZE_T**>(menu) =
				s_imeMenuVtable.data() + 1;
			// Menu::GetMaxDepth adds this signed field to the root Tile
			// depth. Keep the screen-space overlay above the current menus
			// without making it raise every menu created afterwards.
			menu->unk18 =
				static_cast<UInt32>(kImeMenuDepthContribution);
			return menu;
		}

		Menu* __fastcall CreateMenuByClassHook(
			void* factory,
			void*,
			UInt32 menuClass)
		{
			if (menuClass == kImeMenuClass && s_creatingImeMenu)
				return CreateImeMenu();
			return s_originalCreateMenuByClass
				? s_originalCreateMenuByClass(factory, menuClass)
				: nullptr;
		}

		char* __fastcall PipboyRenderedMenuRenderHook(
			void* renderedMenu,
			void*,
			int arg2,
			int arg3,
			int arg4)
		{
			// The dedicated IME Menu is a normal pMenuRoot child, so the
			// Pip-Boy's rendered-menu pass would otherwise capture it and the
			// later screen-space UI pass would draw the same node a second
			// time. App-cull only for this RTT call, preserving its prior state
			// and restoring it before normal UI composition.
			NiNode* imeNode = nullptr;
			bool wasAppCulled = false;
			if (s_imeReady.load(std::memory_order_acquire)
				&& s_state.imeVisible
				&& s_state.imeRoot)
			{
				imeNode = s_state.imeRoot->spNiNode;
				if (imeNode)
				{
					wasAppCulled = imeNode->GetAppCulled();
					if (!wasAppCulled)
						imeNode->SetAppCulled(true);
				}
			}

			char* result = s_originalPipboyRender
				? s_originalPipboyRender(
					renderedMenu, arg2, arg3, arg4)
				: nullptr;

			if (imeNode && !wasAppCulled)
			{
				imeNode->SetAppCulled(false);
				if (!s_loggedPipboyRttExclusion)
				{
					s_loggedPipboyRttExclusion = true;
					gLog.FormattedMessage(
						"tnvse_native_overlay: excluded IME Menu node=%p from Pip-Boy rendered-menu RTT; screen-space pass remains enabled",
						imeNode);
				}
			}
			return result;
		}

		bool EnsurePipboyRenderExclusionHook()
		{
			if (s_pipboyRenderHookInstalled)
				return true;
			if (s_pipboyRenderHookInstallFailed)
				return false;

			const SIZE_T currentTarget =
				*reinterpret_cast<const SIZE_T*>(
					kPipboyRenderVtableEntry);
			if (currentTarget == reinterpret_cast<SIZE_T>(
					&PipboyRenderedMenuRenderHook))
			{
				s_pipboyRenderHookInstalled =
					s_originalPipboyRender != nullptr;
				return s_pipboyRenderHookInstalled;
			}
			if (!currentTarget)
			{
				s_pipboyRenderHookInstallFailed = true;
				gLog.FormattedMessage(
					"tnvse_native_overlay: cannot install Pip-Boy RTT exclusion hook; empty vtable entry at 0x%08X",
					static_cast<UInt32>(kPipboyRenderVtableEntry));
				return false;
			}

			s_originalPipboyRender =
				reinterpret_cast<RenderedMenuRenderFn>(currentTarget);
			SafeWrite32(
				kPipboyRenderVtableEntry,
				reinterpret_cast<SIZE_T>(
					&PipboyRenderedMenuRenderHook));
			s_pipboyRenderHookInstalled = true;
			gLog.FormattedMessage(
				"tnvse_native_overlay: installed Pip-Boy RTT exclusion hook chainedTarget=0x%08X vanilla=%d",
				static_cast<UInt32>(currentTarget),
				currentTarget == kVanillaRenderedMenuRender ? 1 : 0);
			return true;
		}

		bool EnsureImeMenuFactory()
		{
			if (!EnsurePipboyRenderExclusionHook())
				return false;
			if (s_imeMenuFactoryInstalled)
				return true;
			if (s_imeMenuFactoryInstallFailed)
				return false;

			if (*reinterpret_cast<const UInt8*>(
					kCreateMenuByClassCall) != 0xE8)
			{
				s_imeMenuFactoryInstallFailed = true;
				gLog.FormattedMessage(
					"tnvse_native_overlay: cannot install IME Menu factory hook; expected CALL at 0x%08X",
					static_cast<UInt32>(kCreateMenuByClassCall));
				return false;
			}

			const SIZE_T currentTarget =
				GetRelJumpAddr(kCreateMenuByClassCall);
			if (currentTarget == reinterpret_cast<SIZE_T>(
					&CreateMenuByClassHook))
			{
				s_imeMenuFactoryInstalled =
					s_originalCreateMenuByClass != nullptr;
				return s_imeMenuFactoryInstalled;
			}

			const SIZE_T* vanillaVtable =
				reinterpret_cast<const SIZE_T*>(kMenuBaseVtable);
			s_imeMenuVtable.front() = vanillaVtable[-1];
			std::copy_n(
				vanillaVtable,
				kMenuBaseVtableEntryCount,
				s_imeMenuVtable.begin() + 1);
			s_imeMenuVtable[kMenuGetIdVtableIndex + 1] =
				reinterpret_cast<SIZE_T>(&ImeMenuGetId);
			s_originalCreateMenuByClass =
				reinterpret_cast<CreateMenuByClassFn>(currentTarget);
			WriteRelCall(
				kCreateMenuByClassCall,
				&CreateMenuByClassHook);
			s_imeMenuFactoryInstalled = true;
			gLog.FormattedMessage(
				"tnvse_native_overlay: installed dedicated IME Menu factory class=%u chainedTarget=0x%08X",
				kImeMenuClass,
				static_cast<UInt32>(currentTarget));
			return true;
		}

		Tile* FindDirectChild(Tile* parent, const char* name)
		{
			if (!parent || !name)
				return nullptr;
			for (Tile* child : parent->kChildren)
			{
				if (child && !_stricmp(child->strName.c_str(), name))
					return child;
			}
			return nullptr;
		}

		bool IsDirectChild(const Tile* parent, const Tile* child)
		{
			if (!parent || !child)
				return false;
			for (const Tile* candidate : parent->kChildren)
			{
				if (candidate == child)
					return true;
			}
			return false;
		}

		bool IsNamedDirectChild(
			const Tile* parent,
			const Tile* child,
			const char* name)
		{
			return IsDirectChild(parent, child)
				&& name
				&& !_stricmp(child->strName.c_str(), name);
		}

		Tile* FindDirectMenuByClass(Tile* parent, UInt32 menuClass)
		{
			if (!parent)
				return nullptr;
			for (Tile* child : parent->kChildren)
			{
				Menu* menu = child ? child->GetMenu() : nullptr;
				if (menu && menu->GetID() == menuClass)
					return child;
			}
			return nullptr;
		}

		std::string WideToUiText(std::wstring_view value)
		{
			if (value.empty())
				return {};
			const int length = WideCharToMultiByte(
				g_usingWinEncoding,
				0,
				value.data(),
				static_cast<int>(value.size()),
				nullptr,
				0,
				nullptr,
				nullptr);
			if (length <= 0)
				return {};
			std::string result(static_cast<size_t>(length), '\0');
			const int written = WideCharToMultiByte(
				g_usingWinEncoding,
				0,
				value.data(),
				static_cast<int>(value.size()),
				result.data(),
				length,
				nullptr,
				nullptr);
			if (written <= 0)
				return {};
			return result;
		}

		struct ImeTextVisualBounds
		{
			float top = 0.0f;
			float height = 0.0f;
			bool valid = false;
		};

		ImeTextVisualBounds MeasureImeTextVisualBounds(
			std::string_view encoded,
			float measuredTextHeight)
		{
			ImeTextVisualBounds result;
			if (encoded.empty()
				|| !std::isfinite(measuredTextHeight)
				|| measuredTextHeight <= 0.0f)
			{
				return result;
			}

			Font* font = ResolveGameFont(FontManager::GetSingleton(), 1);
			if (!font || !font->pFontData)
				return result;

			const float sourceLineHeight =
				font->pFontData->pFontLetters[' '].fHeight;
			if (!std::isfinite(sourceLineHeight)
				|| sourceLineHeight <= 0.0f)
			{
				return result;
			}

			float visualTop = std::numeric_limits<float>::infinity();
			float visualBottom = -std::numeric_limits<float>::infinity();
			const float lineOriginZ = 2.0f
				* (font->pFontData->fBaseLine - font->fFontHeight);
			ExtraGlyphStore* extraGlyphs =
				GetExtraGlyphs(font->iFontNum);
			for (size_t offset = 0; offset < encoded.size();)
			{
				const UInt8 current =
					static_cast<UInt8>(encoded[offset]);
				FontLetter* glyph = nullptr;
				size_t unitLength = 1;
				UInt32 dbcsCode = 0;
				if (offset + 1 < encoded.size()
					&& TryDecodeDoubleByte(
						encoded.data() + offset,
						dbcsCode))
				{
					glyph = LookupDBGlyph(extraGlyphs, dbcsCode);
					unitLength = 2;
				}
				else if (current >= 0x20 && current != 0x7F
					&& current != static_cast<UInt8>(' '))
				{
					glyph =
						&font->pFontData->pFontLetters[current];
				}

				if (glyph
					&& std::isfinite(glyph->fTopEdge)
					&& std::isfinite(glyph->fHeight)
					&& glyph->fHeight > 0.0f)
				{
					// Font::CreateText starts the first line at
					// 2 * (baseline - fontHeight). FontLetter::fTopEdge
					// is then added in Z, while screen-space Y is -Z.
					// Convert that exact geometry convention to a
					// downward-positive offset from TileText::y.
					const float glyphTop =
						-(lineOriginZ + glyph->fTopEdge);
					const float glyphBottom =
						glyphTop + glyph->fHeight;
					if (std::isfinite(glyphTop)
						&& std::isfinite(glyphBottom)
						&& glyphBottom > glyphTop)
					{
						visualTop =
							std::min(visualTop, glyphTop);
						visualBottom =
							std::max(visualBottom, glyphBottom);
					}
				}
				offset += unitLength;
			}

			if (!std::isfinite(visualTop)
				|| !std::isfinite(visualBottom)
				|| visualBottom <= visualTop)
			{
				return result;
			}

			// TileText::height is the post-UIO height returned to the Tile.
			// Relate the raw FontLetter coordinates to that value instead of
			// assuming that the optional TileText zoom patch is installed.
			const float scale = measuredTextHeight / sourceLineHeight;
			if (!std::isfinite(scale) || scale <= 0.0f)
				return result;

			result.top = visualTop * scale;
			result.height = (visualBottom - visualTop) * scale;
			result.valid = std::isfinite(result.top)
				&& std::isfinite(result.height)
				&& result.height > 0.0f;
			return result;
		}

		void SetVisible(Tile* tile, bool visible)
		{
			if (tile)
				tile->SetValueFloat(
					Tile::kTileValue_visible, visible ? 1.0f : 0.0f, true);
		}

		void SetText(Tile* tile, std::wstring_view value)
		{
			if (!tile)
				return;
			const std::string encoded = WideToUiText(value);
			tile->SetValueString(Tile::kTileValue_string, encoded.c_str(), true);
		}

		void RebuildTextGeometry(Tile* tile)
		{
			if (!tile)
				return;
			const std::string text =
				tile->GetValueString(Tile::kTileValue_string);
			tile->SetValueString(Tile::kTileValue_string, "", true);
			tile->SetValueString(
				Tile::kTileValue_string, text.c_str(), true);
		}

		bool TryGetReadOnlyMaximumMenuDepth(float& result)
		{
			InterfaceManager* manager = InterfaceManager::GetSingleton();
			Tile* menusRoot = manager ? manager->pMenuRoot : nullptr;
			if (!menusRoot)
				return false;

			// FalloutNV.exe 0xA1DFB0 is Menu::GetMaxDepth, despite the old
			// address-only name used here. Its return value is the maximum
			// top-level menu depth/thickness plus two, but the function also
			// rewrites the cursor Tile depth and cursor NiNode translation.
			// Overlay refreshes must reproduce only the read-only scan.
			float maximumDepth = 0.0f;
			for (Tile* child : menusRoot->kChildren)
			{
				if (!child || child == s_state.imeRoot)
					continue;
				Menu* menu = child->GetMenu();
				if (!menu)
					continue;

				const float menuDepth =
					child->GetValueFloat(Tile::kTileValue_depth);
				const float menuThickness = static_cast<float>(
					static_cast<SInt32>(menu->unk18));
				const float candidate = menuDepth + menuThickness;
				if (std::isfinite(candidate))
					maximumDepth = std::max(maximumDepth, candidate);
			}

			result = maximumDepth + 2.0f;
			return std::isfinite(result);
		}

		void SynchronizeOverlayDepth(Tile* root)
		{
			if (!root)
				return;

			float depth = 0.0f;
			if (!TryGetReadOnlyMaximumMenuDepth(depth))
				return;

			const float current =
				root->GetValueFloat(Tile::kTileValue_depth);
			if (std::isfinite(current)
				&& std::fabs(current - depth) <= 0.001f)
				return;

			// Avoid clearing/rebuilding the depth trait for every candidate
			// update. It changes only when the actual top-level menu depth does.
			root->SetValueFloat(Tile::kTileValue_depth, depth, true);
		}

		void ClearImeResolvedTiles()
		{
			s_imeReady.store(false, std::memory_order_release);
			s_state.imeRoot = nullptr;
			s_state.imeBackground = nullptr;
			s_state.imeLines.fill(nullptr);
			s_state.imeHighlights.fill(nullptr);
			s_state.imeKey.clear();
			s_state.imeLineHeight = 0.0f;
			s_state.imeContentWidth = 0.0f;
			s_state.imeVisible = false;
		}

		void ClearPrewarmResolvedTiles()
		{
			s_prewarmReady.store(false, std::memory_order_release);
			s_state.prewarmRoot = nullptr;
			s_state.prewarmShade = nullptr;
			s_state.prewarmPanel = nullptr;
			s_state.prewarmTrack = nullptr;
			s_state.prewarmDetail = nullptr;
			s_state.prewarmStage = nullptr;
			s_state.prewarmFill = nullptr;
			s_state.prewarmPercent = nullptr;
			s_state.prewarmTitle = nullptr;
			s_state.prewarmLayoutSignature.fill(0.0f);
			s_state.prewarmProgressWidth = 520.0f;
			s_state.prewarmTileVisible = false;
		}

		void ResetImeForParent(Tile* parent)
		{
			// A changed pMenuRoot means every pointer under the old host
			// may already have been released. Never dereference or destroy it
			// here.
			ClearImeResolvedTiles();
			s_state.imeParent = parent;
			s_state.imeLoadFailed = false;
		}

		void ResetPrewarmForParent(Tile* parent)
		{
			// A changed LoadingMenu root means the engine may already have
			// released the old Tile tree. Never dereference the stale pointers.
			ClearPrewarmResolvedTiles();
			s_state.prewarmParent = parent;
			s_state.prewarmLoadFailed = false;
		}

		Tile* SynchronizeImeParent()
		{
			InterfaceManager* manager = InterfaceManager::GetSingleton();
			// The IME tree is a dedicated non-stacking Menu and a direct
			// pMenuRoot child. The Pip-Boy RTT hook temporarily culls its
			// NiNode while that root is captured, then restores screen drawing.
			Tile* parent = manager ? manager->pMenuRoot : nullptr;
			if (s_state.imeParent != parent)
				ResetImeForParent(parent);
			return parent;
		}

		Tile* GetLoadingMenuRoot()
		{
			Menu* loadingMenu =
				*reinterpret_cast<Menu**>(kLoadingMenuSingleton);
			return loadingMenu && loadingMenu->pRootTile
				? static_cast<Tile*>(loadingMenu->pRootTile)
				: nullptr;
		}

		Tile* SynchronizePrewarmParent()
		{
			// Match Cell Offset Generator: the prewarm component belongs to the
			// stock LoadingMenu tree, whose own thread continues Update and
			// ShowChanges while the game thread remains inside DeferredInit.
			Tile* parent = GetLoadingMenuRoot();
			if (s_state.prewarmParent != parent)
				ResetPrewarmForParent(parent);
			return parent;
		}

		void ReleaseAndDestroyAttachedRoot(
			Tile* parent,
			Tile* root,
			const char* expectedName)
		{
			if (IsNamedDirectChild(parent, root, expectedName))
			{
				SetVisible(root, false);
				ThisStdCall<void>(kReleaseTileTree, root);
				delete root;
			}
		}

		bool IsImeMenuRegistered(Tile* root)
		{
			Menu* menu = root ? root->GetMenu() : nullptr;
			return menu
				&& menu->GetID() == kImeMenuClass
				&& menu->uiID == kImeMenuClass
				&& menu->pRootTile == static_cast<TileMenu*>(root)
				&& InterfaceManager::GetMenuByType(kImeMenuClass) == root;
		}

		bool RegisterImeMenuRoot(Tile* root)
		{
			Menu* menu = root ? root->GetMenu() : nullptr;
			if (!menu || menu->GetID() != kImeMenuClass)
				return false;

			// TileMenu::PostParse binds once when it encounters <class>,
			// before the remaining root traits have been parsed. Repeat the
			// stock Create() finalization after ReadXML completes, matching
			// built-in menus and Stewie Tweaks' injected menu lifecycle.
			menu->uiID = kImeMenuClass;
			ThisStdCall<void>(
				kRegisterMenuTile,
				menu,
				static_cast<TileMenu*>(root),
				false);
			return IsImeMenuRegistered(root);
		}

		Tile* NormalizeOwnedImeRoots(Tile* parent)
		{
			if (!parent)
				return nullptr;

			std::vector<Tile*> ownedRoots;
			for (Tile* child : parent->kChildren)
			{
				if (child
					&& !_stricmp(
						child->strName.c_str(), "tNVSE_IME"))
				{
					ownedRoots.push_back(child);
				}
			}

			Tile* selected = nullptr;
			for (Tile* root : ownedRoots)
			{
				Menu* menu = root->GetMenu();
				if (!selected
					&& menu
					&& menu->GetID() == kImeMenuClass)
				{
					selected = root;
					continue;
				}
				ReleaseAndDestroyAttachedRoot(
					parent, root, "tNVSE_IME");
			}

			if (ownedRoots.size() > (selected ? 1u : 0u))
			{
				gLog.FormattedMessage(
					"tnvse_native_overlay: removed %u stale or duplicate IME Menu root(s) before singleton bind",
					static_cast<UInt32>(
						ownedRoots.size() - (selected ? 1u : 0u)));
			}
			return selected;
		}

		bool ResolveImeTiles(Tile* root)
		{
			if (!root || _stricmp(root->strName.c_str(), "tNVSE_IME"))
				return false;
			Menu* menu = root->GetMenu();
			if (!menu || menu->GetID() != kImeMenuClass)
				return false;

			Tile* background =
				FindDirectChild(root, "tNVSE_IME_Background");
			std::array<Tile*, kImeLineCount> lines = {};
			std::array<Tile*, kImeHighlightCount> highlights = {};
			for (size_t i = 0; i < kImeLineCount; ++i)
			{
				char lineName[32] = {};
				_snprintf_s(lineName, _countof(lineName), _TRUNCATE,
					"tNVSE_IME_Line%02u", static_cast<UInt32>(i));
				lines[i] = FindDirectChild(root, lineName);
				if (!lines[i])
					return false;
			}
			for (size_t i = 0; i < kImeHighlightCount; ++i)
			{
				char highlightName[40] = {};
				_snprintf_s(highlightName, _countof(highlightName), _TRUNCATE,
					"tNVSE_IME_Highlight%02u", static_cast<UInt32>(i));
				highlights[i] = FindDirectChild(root, highlightName);
				if (!highlights[i])
					return false;
			}
			if (!background)
				return false;

			s_state.imeRoot = root;
			s_state.imeBackground = background;
			s_state.imeLines = lines;
			s_state.imeHighlights = highlights;
			return true;
		}

		bool ResolvePrewarmTiles(Tile* root)
		{
			if (!root || _stricmp(root->strName.c_str(), "tNVSE_Prewarm"))
				return false;

			Tile* shade =
				FindDirectChild(root, "tNVSE_Prewarm_Shade");
			Tile* panel =
				FindDirectChild(root, "tNVSE_Prewarm_Panel");
			Tile* track =
				FindDirectChild(root, "tNVSE_Prewarm_Track");
			Tile* detail =
				FindDirectChild(root, "tNVSE_Prewarm_Detail");
			Tile* stage =
				FindDirectChild(root, "tNVSE_Prewarm_Stage");
			Tile* fill =
				FindDirectChild(root, "tNVSE_Prewarm_Fill");
			Tile* percent =
				FindDirectChild(root, "tNVSE_Prewarm_Percent");
			Tile* title =
				FindDirectChild(root, "tNVSE_Prewarm_Title");
			if (!shade || !panel || !track || !detail || !stage || !fill
				|| !percent || !title)
				return false;

			s_state.prewarmRoot = root;
			s_state.prewarmShade = shade;
			s_state.prewarmPanel = panel;
			s_state.prewarmTrack = track;
			s_state.prewarmDetail = detail;
			s_state.prewarmStage = stage;
			s_state.prewarmFill = fill;
			s_state.prewarmPercent = percent;
			s_state.prewarmTitle = title;
			return true;
		}

		bool IsResolvedImeTreeAttached(Tile* parent)
		{
			if (!IsNamedDirectChild(
					parent, s_state.imeRoot, "tNVSE_IME")
				|| !IsNamedDirectChild(
					s_state.imeRoot,
					s_state.imeBackground,
					"tNVSE_IME_Background"))
			{
				return false;
			}
			for (size_t i = 0; i < s_state.imeLines.size(); ++i)
			{
				char name[32] = {};
				_snprintf_s(name, _countof(name), _TRUNCATE,
					"tNVSE_IME_Line%02u", static_cast<UInt32>(i));
				if (!IsNamedDirectChild(
						s_state.imeRoot, s_state.imeLines[i], name))
					return false;
			}
			for (size_t i = 0; i < s_state.imeHighlights.size(); ++i)
			{
				char name[40] = {};
				_snprintf_s(name, _countof(name), _TRUNCATE,
					"tNVSE_IME_Highlight%02u", static_cast<UInt32>(i));
				if (!IsNamedDirectChild(
						s_state.imeRoot, s_state.imeHighlights[i], name))
					return false;
			}
			return true;
		}

		bool IsResolvedPrewarmTreeAttached(Tile* parent)
		{
			if (!IsNamedDirectChild(
					parent, s_state.prewarmRoot, "tNVSE_Prewarm"))
			{
				return false;
			}
			const std::array<Tile*, 8> required = {
				s_state.prewarmShade,
				s_state.prewarmPanel,
				s_state.prewarmTrack,
				s_state.prewarmDetail,
				s_state.prewarmStage,
				s_state.prewarmFill,
				s_state.prewarmPercent,
				s_state.prewarmTitle,
			};
			const std::array<const char*, 8> names = {
				"tNVSE_Prewarm_Shade",
				"tNVSE_Prewarm_Panel",
				"tNVSE_Prewarm_Track",
				"tNVSE_Prewarm_Detail",
				"tNVSE_Prewarm_Stage",
				"tNVSE_Prewarm_Fill",
				"tNVSE_Prewarm_Percent",
				"tNVSE_Prewarm_Title",
			};
			for (size_t i = 0; i < required.size(); ++i)
			{
				if (!IsNamedDirectChild(
						s_state.prewarmRoot, required[i], names[i]))
					return false;
			}
			return true;
		}

		void ResetImePresentationState()
		{
			s_state.imeKey.clear();
			s_state.imeLineHeight = 0.0f;
			s_state.imeContentWidth = 0.0f;
			s_state.imeVisible = false;
		}

		void ResetPrewarmPresentationState()
		{
			s_state.prewarmLayoutSignature.fill(0.0f);
			s_state.prewarmProgressWidth = 520.0f;
			s_state.prewarmTileVisible = false;
		}

		bool EnsureImeHost(Tile* parent)
		{
			if (s_state.imeRoot)
			{
				Tile* root = s_state.imeRoot;
				if (IsResolvedImeTreeAttached(parent))
				{
					if (IsImeMenuRegistered(root)
						|| RegisterImeMenuRoot(root))
					{
						s_imeReady.store(true, std::memory_order_release);
						return true;
					}
				}
				if (IsDirectChild(parent, root)
					&& ResolveImeTiles(root)
					&& RegisterImeMenuRoot(root))
				{
					ResetImePresentationState();
					SetVisible(root, false);
					s_imeReady.store(true, std::memory_order_release);
					gLog.FormattedMessage(
						"tnvse_native_overlay: rebound IME Menu after child-tree replacement host=%p parent=%p",
						root, parent);
					return true;
				}
				ClearImeResolvedTiles();
				s_state.imeLoadFailed = false;
				ReleaseAndDestroyAttachedRoot(
					parent, root, "tNVSE_IME");
				gLog.FormattedMessage(
					"tnvse_native_overlay: IME Menu was detached, malformed, or lost its native registration; reloading");
			}
			if (s_state.imeLoadFailed)
				return false;

			if (Tile* existing = NormalizeOwnedImeRoots(parent))
			{
				if (ResolveImeTiles(existing)
					&& RegisterImeMenuRoot(existing))
				{
					ResetImePresentationState();
					SetVisible(existing, false);
					s_imeReady.store(true, std::memory_order_release);
					gLog.FormattedMessage(
						"tnvse_native_overlay: adopted existing IME Menu class=%u host=%p parent=%p",
						kImeMenuClass,
						existing,
						parent);
					return true;
				}

				ClearImeResolvedTiles();
				ReleaseAndDestroyAttachedRoot(
					parent, existing, "tNVSE_IME");
				gLog.FormattedMessage(
					"tnvse_native_overlay: discarded malformed owned IME Menu class=%u before reloading",
					kImeMenuClass);
			}

			Tile* foreign =
				FindDirectMenuByClass(parent, kImeMenuClass);
			if (!foreign)
				foreign =
					InterfaceManager::GetMenuByType(kImeMenuClass);
			if (foreign)
			{
				const bool attached = IsDirectChild(parent, foreign);
				s_state.imeLoadFailed = true;
				gLog.FormattedMessage(
					"tnvse_native_overlay: Menu Code %u is already owned by foreign Tile host=%p name='%s'; native IME overlay unavailable and system candidate UI remains suppressed",
					kImeMenuClass,
					foreign,
					attached
						? foreign->strName.c_str()
						: "<registered outside current pMenuRoot>");
				return false;
			}
			if (!EnsureImeMenuFactory())
			{
				s_state.imeLoadFailed = true;
				gLog.FormattedMessage(
					"tnvse_native_overlay: IME Menu factory unavailable; native IME overlay unavailable and system candidate UI remains suppressed");
				return false;
			}

			s_creatingImeMenu = true;
			Tile* root = ThisStdCall<Tile*>(
				kReadTileXml, parent, kImeOverlayXmlPath);
			s_creatingImeMenu = false;
			if (!root || !IsDirectChild(parent, root)
				|| !ResolveImeTiles(root)
				|| !RegisterImeMenuRoot(root))
			{
				ClearImeResolvedTiles();
				ReleaseAndDestroyAttachedRoot(
					parent, root, "tNVSE_IME");
				s_state.imeLoadFailed = true;
				gLog.FormattedMessage(
					"tnvse_native_overlay: failed to load, resolve, or register IME Menu path='%s'; native IME overlay unavailable and system candidate UI remains suppressed",
					kImeOverlayXmlPath);
				return false;
			}

			SetVisible(s_state.imeRoot, false);
			s_imeReady.store(true, std::memory_order_release);
			gLog.FormattedMessage(
				"tnvse_native_overlay: loaded IME Menu path='%s' host=%p parent=%p class=%u",
				kImeOverlayXmlPath,
				s_state.imeRoot,
				parent,
				kImeMenuClass);
			return true;
		}

		bool EnsurePrewarmHost(Tile* parent)
		{
			if (s_state.prewarmRoot)
			{
				Tile* root = s_state.prewarmRoot;
				if (IsResolvedPrewarmTreeAttached(parent))
				{
					const bool tileVisible =
						root->GetValueFloat(Tile::kTileValue_visible) > 0.5f;
					if (s_state.prewarmTileVisible && !tileVisible)
						ResetPrewarmPresentationState();
					else if (!s_state.prewarmTileVisible && tileVisible)
						SetVisible(root, false);
					s_prewarmReady.store(true, std::memory_order_release);
					return true;
				}
				if (IsDirectChild(parent, root) && ResolvePrewarmTiles(root))
				{
					ResetPrewarmPresentationState();
					SetVisible(root, false);
					s_prewarmReady.store(true, std::memory_order_release);
					gLog.FormattedMessage(
						"tnvse_native_overlay: rebound prewarm Tile component after child-tree replacement host=%p parent=%p",
						root, parent);
					return true;
				}
				ClearPrewarmResolvedTiles();
				s_state.prewarmLoadFailed = false;
				ReleaseAndDestroyAttachedRoot(
					parent, root, "tNVSE_Prewarm");
				gLog.FormattedMessage(
					"tnvse_native_overlay: prewarm Tile component was detached or malformed; reloading component");
			}
			if (s_state.prewarmLoadFailed)
				return false;

			Tile* root = ThisStdCall<Tile*>(
				kReadTileXml, parent, kPrewarmOverlayXmlPath);
			if (!root || !IsDirectChild(parent, root)
				|| !ResolvePrewarmTiles(root))
			{
				ClearPrewarmResolvedTiles();
				ReleaseAndDestroyAttachedRoot(
					parent, root, "tNVSE_Prewarm");
				s_state.prewarmLoadFailed = true;
				gLog.FormattedMessage(
					"tnvse_native_overlay: failed to load or resolve prewarm component path='%s'; font prewarm continues without Tile progress UI",
					kPrewarmOverlayXmlPath);
				return false;
			}

			SetVisible(s_state.prewarmRoot, false);
			s_prewarmReady.store(true, std::memory_order_release);
			gLog.FormattedMessage(
				"tnvse_native_overlay: loaded prewarm Tile component path='%s' host=%p parent=%p",
				kPrewarmOverlayXmlPath, s_state.prewarmRoot, parent);
			return true;
		}

		std::wstring BuildImeKey(
			const std::vector<NativeTileOverlayLine>& lines)
		{
			std::wstring result;
			for (const NativeTileOverlayLine& line : lines)
			{
				result.push_back(line.highlighted ? L'\1' : L'\0');
				result += line.text;
				result.push_back(L'\n');
			}
			return result;
		}

		float ReadTextHeight(Tile* tile)
		{
			const float height = tile
				? tile->GetValueFloat(Tile::kTileValue_height)
				: 0.0f;
			return std::isfinite(height) && height > 0.0f
				? std::max(kPrewarmMinimumTextHeight, height)
				: kPrewarmMinimumTextHeight;
		}

		void LayoutPrewarmOverlay()
		{
			if (!s_state.prewarmRoot || !s_state.prewarmPanel)
				return;
			const float rootWidth =
				s_state.prewarmRoot->GetValueFloat(
					Tile::kTileValue_width);
			const float rootHeight =
				s_state.prewarmRoot->GetValueFloat(
					Tile::kTileValue_height);
			if (!std::isfinite(rootWidth)
				|| !std::isfinite(rootHeight)
				|| rootWidth <= 0.0f
				|| rootHeight <= 0.0f)
			{
				return;
			}

			const float titleHeight = ReadTextHeight(
				s_state.prewarmTitle);
			const float detailHeight = ReadTextHeight(
				s_state.prewarmDetail);
			const float stageHeight = ReadTextHeight(
				s_state.prewarmStage);
			const float percentHeight = ReadTextHeight(
				s_state.prewarmPercent);
			const std::array<float, 6> signature = {
				rootWidth,
				rootHeight,
				titleHeight,
				detailHeight,
				stageHeight,
				percentHeight,
			};
			bool changed = false;
			for (size_t i = 0; i < signature.size(); ++i)
			{
				if (std::fabs(
						signature[i]
							- s_state.prewarmLayoutSignature[i])
					> 0.25f)
				{
					changed = true;
					break;
				}
			}
			if (!changed)
				return;
			s_state.prewarmLayoutSignature = signature;

			const float panelWidth = std::min(
				kPrewarmMaximumPanelWidth,
				std::max(
					kPrewarmMinimumPanelWidth,
					rootWidth - 24.0f));
			const float panelHeight = std::max(
				190.0f,
				20.0f + titleHeight
					+ 6.0f + detailHeight
					+ 18.0f + 16.0f
					+ 10.0f + std::max(stageHeight, percentHeight)
					+ 20.0f);
			const float panelX = std::max(
				12.0f, (rootWidth - panelWidth) * 0.5f);
			const float panelY = std::max(
				12.0f, (rootHeight - panelHeight) * 0.5f);
			const float titleY = panelY + 20.0f;
			const float detailY =
				titleY + titleHeight + 6.0f;
			const float trackX = panelX + 50.0f;
			const float trackY =
				detailY + detailHeight + 18.0f;
			const float progressWidth =
				std::max(220.0f, panelWidth - 100.0f);
			const float stageY = trackY + 16.0f + 10.0f;

			s_state.prewarmPanel->SetValueFloat(
				Tile::kTileValue_x, panelX, true);
			s_state.prewarmPanel->SetValueFloat(
				Tile::kTileValue_y, panelY, true);
			s_state.prewarmPanel->SetValueFloat(
				Tile::kTileValue_width, panelWidth, true);
			s_state.prewarmPanel->SetValueFloat(
				Tile::kTileValue_height, panelHeight, true);
			s_state.prewarmTitle->SetValueFloat(
				Tile::kTileValue_x, panelX + 30.0f, true);
			s_state.prewarmTitle->SetValueFloat(
				Tile::kTileValue_y, titleY, true);
			s_state.prewarmDetail->SetValueFloat(
				Tile::kTileValue_x, panelX + 30.0f, true);
			s_state.prewarmDetail->SetValueFloat(
				Tile::kTileValue_y, detailY, true);
			s_state.prewarmDetail->SetValueFloat(
				Tile::kTileValue_wrapwidth,
				std::max(1.0f, panelWidth - 60.0f),
				true);
			s_state.prewarmTrack->SetValueFloat(
				Tile::kTileValue_x, trackX, true);
			s_state.prewarmTrack->SetValueFloat(
				Tile::kTileValue_y, trackY, true);
			s_state.prewarmTrack->SetValueFloat(
				Tile::kTileValue_width, progressWidth, true);
			s_state.prewarmFill->SetValueFloat(
				Tile::kTileValue_x, trackX, true);
			s_state.prewarmFill->SetValueFloat(
				Tile::kTileValue_y, trackY, true);
			s_state.prewarmStage->SetValueFloat(
				Tile::kTileValue_x, trackX, true);
			s_state.prewarmStage->SetValueFloat(
				Tile::kTileValue_y, stageY, true);
			s_state.prewarmStage->SetValueFloat(
				Tile::kTileValue_wrapwidth,
				std::max(1.0f, panelWidth - 200.0f),
				true);
			s_state.prewarmPercent->SetValueFloat(
				Tile::kTileValue_x,
				panelX + panelWidth - 120.0f,
				true);
			s_state.prewarmPercent->SetValueFloat(
				Tile::kTileValue_y, stageY, true);
			s_state.prewarmProgressWidth = progressWidth;
		}
	}

	bool EnsureNativeImeOverlayHost()
	{
		Tile* parent = SynchronizeImeParent();
		if (!parent)
			return false;
		return EnsureImeHost(parent);
	}

	bool EnsureNativePrewarmOverlayHost()
	{
		Tile* parent = SynchronizePrewarmParent();
		if (!parent)
		{
			if (!s_state.prewarmParentUnavailableLogged)
			{
				s_state.prewarmParentUnavailableLogged = true;
				gLog.FormattedMessage(
					"tnvse_native_overlay: LoadingMenu root unavailable; font prewarm continues without Tile progress UI");
			}
			return false;
		}
		s_state.prewarmParentUnavailableLogged = false;
		return EnsurePrewarmHost(parent);
	}

	bool IsNativeImeOverlayHostReady()
	{
		return s_imeReady.load(std::memory_order_acquire);
	}

	bool IsNativePrewarmOverlayHostReady()
	{
		return s_prewarmReady.load(std::memory_order_acquire);
	}

	void UpdateNativeImeOverlay(
		const std::vector<NativeTileOverlayLine>& lines)
	{
		EnsureNativeImeOverlayHost();
		if (!IsNativeImeOverlayHostReady()
			|| s_prewarmActive.load(std::memory_order_acquire))
		{
			HideNativeImeOverlay();
			return;
		}
		if (lines.empty())
		{
			HideNativeImeOverlay();
			return;
		}

		const std::wstring key = BuildImeKey(lines);
		const bool contentChanged = key != s_state.imeKey;
		const size_t visibleCount = std::min(lines.size(), kImeLineCount);
		if (contentChanged)
		{
			s_state.imeKey = key;
			for (Tile* highlight : s_state.imeHighlights)
				SetVisible(highlight, false);
			for (size_t i = 0; i < kImeLineCount; ++i)
			{
				const bool visible = i < visibleCount;
				SetVisible(s_state.imeLines[i], visible);
				if (visible)
					SetText(s_state.imeLines[i], lines[i].text);
			}
		}

		float lineHeight = kImeMinimumLineHeight;
		float contentWidth = kImeMinimumWidth;
		std::array<ImeTextVisualBounds, kImeLineCount> visualBounds = {};
		for (size_t i = 0; i < visibleCount; ++i)
		{
			const float measuredHeight =
				s_state.imeLines[i]->GetValueFloat(
					Tile::kTileValue_height);
			if (std::isfinite(measuredHeight) && measuredHeight > 0.0f)
				lineHeight = std::max(lineHeight, measuredHeight);

			const std::string encoded = WideToUiText(lines[i].text);
			visualBounds[i] =
				MeasureImeTextVisualBounds(encoded, measuredHeight);
			const float estimatedWidth =
				static_cast<float>(encoded.size()) * 12.0f
				+ kImeHorizontalPadding;
			contentWidth = std::max(contentWidth, estimatedWidth);
			const float measuredWidth =
				s_state.imeLines[i]->GetValueFloat(
					Tile::kTileValue_width);
			if (std::isfinite(measuredWidth) && measuredWidth > 0.0f)
			{
				contentWidth = std::max(
					contentWidth,
					measuredWidth + kImeHorizontalPadding);
			}
		}
		contentWidth = std::clamp(
			contentWidth, kImeMinimumWidth, kImeMaximumWidth);
		const bool metricsChanged =
			std::fabs(lineHeight - s_state.imeLineHeight) > 0.25f;
		const bool widthChanged =
			std::fabs(contentWidth - s_state.imeContentWidth) > 0.25f;
		if (contentChanged || metricsChanged || widthChanged)
		{
			s_state.imeLineHeight = lineHeight;
			s_state.imeContentWidth = contentWidth;
			const float left =
				(kImeMaximumWidth - contentWidth) * 0.5f;
			const float lineStride = lineHeight + kImeLineGap;
			const float height = kImeVerticalPadding * 2.0f
				+ static_cast<float>(visibleCount) * lineHeight
				+ static_cast<float>(
					visibleCount > 0 ? visibleCount - 1 : 0)
					* kImeLineGap;
			s_state.imeRoot->SetValueFloat(
				Tile::kTileValue_height, height, true);
			s_state.imeBackground->SetValueFloat(
				Tile::kTileValue_x, left, true);
			s_state.imeBackground->SetValueFloat(
				Tile::kTileValue_height, height, true);
			s_state.imeBackground->SetValueFloat(
				Tile::kTileValue_width, contentWidth, true);
			for (size_t i = 0; i < visibleCount; ++i)
			{
				const float lineY = kImeVerticalPadding
					+ static_cast<float>(i) * lineStride;
				s_state.imeLines[i]->SetValueFloat(
					Tile::kTileValue_x, left + 10.0f, true);
				s_state.imeLines[i]->SetValueFloat(
					Tile::kTileValue_user0,
					lineY,
					true);
				s_state.imeLines[i]->SetValueFloat(
					Tile::kTileValue_user1,
					lineHeight,
					true);
				const float measuredHeight =
					s_state.imeLines[i]->GetValueFloat(
						Tile::kTileValue_height);
				s_state.imeLines[i]->SetValueFloat(
					Tile::kTileValue_user2,
					visualBounds[i].valid
						? visualBounds[i].top : 0.0f,
					true);
				s_state.imeLines[i]->SetValueFloat(
					Tile::kTileValue_user3,
					visualBounds[i].valid
						? visualBounds[i].height
						: std::max(0.0f, measuredHeight),
					true);
				if (g_bEnableFreeTypeFontRenderingLog
					&& s_imeVisualBoundsLogCount
						< kImeVisualBoundsLogLimit)
				{
					++s_imeVisualBoundsLogCount;
					gLog.FormattedMessage(
						"tnvse_native_overlay: IME visual center line=%u rowTop=%.2f rowHeight=%.2f tileHeight=%.2f glyphTop=%.2f glyphHeight=%.2f xmlOffset=%.2f valid=%d",
						static_cast<UInt32>(i),
						lineY,
						lineHeight,
						measuredHeight,
						visualBounds[i].valid
							? visualBounds[i].top : 0.0f,
						visualBounds[i].valid
							? visualBounds[i].height
							: std::max(0.0f, measuredHeight),
						visualBounds[i].valid
							? (lineHeight - visualBounds[i].height)
								* 0.5f - visualBounds[i].top
							: (lineHeight
								- std::max(0.0f, measuredHeight))
								* 0.5f,
						visualBounds[i].valid ? 1 : 0);
				}
				s_state.imeLines[i]->SetValueFloat(
					Tile::kTileValue_wrapwidth,
					std::max(1.0f,
						contentWidth - kImeHorizontalPadding),
					true);
			}
			size_t highlightIndex = 0;
			for (size_t i = 0; i < visibleCount; ++i)
			{
				if (!lines[i].highlighted
					|| highlightIndex >= kImeHighlightCount)
				{
					continue;
				}
				Tile* highlight =
					s_state.imeHighlights[highlightIndex++];
				highlight->SetValueFloat(
					Tile::kTileValue_y,
					kImeVerticalPadding
						+ static_cast<float>(i) * lineStride,
					true);
				highlight->SetValueFloat(
					Tile::kTileValue_height, lineHeight, true);
				SetVisible(highlight, true);
			}
			for (Tile* highlight : s_state.imeHighlights)
			{
				highlight->SetValueFloat(
					Tile::kTileValue_x, left + 8.0f, true);
				highlight->SetValueFloat(
					Tile::kTileValue_width,
					std::max(1.0f, contentWidth - 16.0f),
					true);
			}
		}

		SynchronizeOverlayDepth(s_state.imeRoot);
		const bool tileVisible =
			s_state.imeRoot->GetValueFloat(
				Tile::kTileValue_visible) > 0.5f;
		if (!s_state.imeVisible || !tileVisible)
		{
			SetVisible(s_state.imeRoot, true);
			s_state.imeVisible = true;
		}
	}

	void HideNativeImeOverlay()
	{
		Tile* parent = SynchronizeImeParent();
		const bool attached =
			IsNamedDirectChild(parent, s_state.imeRoot, "tNVSE_IME");
		if (s_state.imeVisible
			&& IsNativeImeOverlayHostReady()
			&& attached)
			SetVisible(s_state.imeRoot, false);
		if (s_state.imeRoot && !attached)
			ClearImeResolvedTiles();
		s_state.imeVisible = false;
		s_state.imeKey.clear();
	}

	void ShowNativePrewarmOverlay()
	{
		s_prewarmActive.store(true, std::memory_order_release);
		EnsureNativePrewarmOverlayHost();
		HideNativeImeOverlay();
		if (!IsNativePrewarmOverlayHostReady())
			return;
		if (!s_state.prewarmTileVisible)
		{
			SetVisible(s_state.prewarmRoot, true);
			s_state.prewarmTileVisible = true;
		}
	}

	void UpdateNativePrewarmOverlay(
		const std::wstring& detail,
		const std::wstring& stage,
		float progress)
	{
		ShowNativePrewarmOverlay();
		if (!IsNativePrewarmOverlayHostReady())
			return;
		progress = std::clamp(progress, 0.0f, 1.0f);
		SetText(s_state.prewarmDetail, detail);
		SetText(s_state.prewarmStage, stage);
		LayoutPrewarmOverlay();
		s_state.prewarmFill->SetValueFloat(
			Tile::kTileValue_width,
			s_state.prewarmProgressWidth * progress,
			true);
		wchar_t percent[16] = {};
		_snwprintf_s(percent, _countof(percent), _TRUNCATE, L"%u%%",
			static_cast<UInt32>(std::lround(progress * 100.0f)));
		SetText(s_state.prewarmPercent, percent);
	}

	void RefreshNativePrewarmOverlayTextGeometry()
	{
		Tile* parent = SynchronizePrewarmParent();
		if (!IsNativePrewarmOverlayHostReady()
			|| !IsResolvedPrewarmTreeAttached(parent))
		{
			return;
		}
		RebuildTextGeometry(s_state.prewarmTitle);
		RebuildTextGeometry(s_state.prewarmDetail);
		RebuildTextGeometry(s_state.prewarmStage);
		RebuildTextGeometry(s_state.prewarmPercent);
		s_state.prewarmLayoutSignature.fill(0.0f);
		LayoutPrewarmOverlay();
	}

	void HideNativePrewarmOverlay()
	{
		s_prewarmActive.store(false, std::memory_order_release);
		Tile* parent = SynchronizePrewarmParent();
		Tile* root = s_state.prewarmRoot;
		const bool attached =
			IsNamedDirectChild(
				parent, root, "tNVSE_Prewarm");
		if (s_state.prewarmTileVisible
			&& IsNativePrewarmOverlayHostReady()
			&& attached)
			SetVisible(root, false);
		if (root && attached)
		{
			// Match the stock/Cell Offset component teardown: release the
			// imported Tile tree under the UI lock, then destroy its root.
			ReleaseAndDestroyAttachedRoot(
				parent, root, "tNVSE_Prewarm");
		}
		ClearPrewarmResolvedTiles();
	}

	bool IsNativePrewarmOverlayActive()
	{
		return s_prewarmActive.load(std::memory_order_acquire);
	}

	void ShutdownNativeTileOverlayHost()
	{
		// Stop publishing either Tile tree before destruction. System candidate
		// UI suppression is owned by the IME hooks and deliberately does not
		// depend on these readiness atomics.
		s_imeReady.store(false, std::memory_order_release);
		s_prewarmReady.store(false, std::memory_order_release);
		s_prewarmActive.store(false, std::memory_order_release);
		InterfaceManager* manager = InterfaceManager::GetSingleton();
		Tile* currentImeParent =
			manager ? manager->pMenuRoot : nullptr;
		Tile* currentPrewarmParent = s_state.prewarmRoot
			? GetLoadingMenuRoot() : nullptr;
		if (currentImeParent
			&& currentImeParent == s_state.imeParent)
		{
			ReleaseAndDestroyAttachedRoot(
				currentImeParent,
				s_state.imeRoot,
				"tNVSE_IME");
		}
		if (currentPrewarmParent
			&& currentPrewarmParent == s_state.prewarmParent)
		{
			ReleaseAndDestroyAttachedRoot(
				currentPrewarmParent,
				s_state.prewarmRoot,
				"tNVSE_Prewarm");
		}
		ResetImeForParent(nullptr);
		ResetPrewarmForParent(nullptr);
	}
}

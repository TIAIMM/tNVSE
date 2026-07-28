#include "native_tile_overlay.h"

#include "BSMemory.hpp"
#include "InterfaceManager.hpp"
#include "Menu.hpp"
#include "SafeWrite.h"
#include "Tile.hpp"
#include "load_config.h"
#include "tnvse.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <string_view>

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
		constexpr float kPrewarmMinimumTextHeight = 24.0f;
		constexpr float kPrewarmMaximumPanelWidth = 620.0f;
		constexpr float kPrewarmMinimumPanelWidth = 320.0f;
		constexpr UInt32 kReadTileXml = 0xA01B00;
		// "TNV" remains exactly representable in the float-backed XML trait
		// while staying far outside the vanilla 1001-1084 Menu Code range.
		// This non-stacking Menu is intentionally not registered in xMenuList.
		constexpr UInt32 kImeMenuClass = 0x00544E56;
		static_assert(kImeMenuClass > 1084);
		static_assert(kImeMenuClass <= (1u << 24));
		constexpr SIZE_T kCreateMenuByClassCall = 0x7079A3;
		constexpr SIZE_T kMenuConstructor = 0xA1C4A0;
		constexpr SIZE_T kMenuBaseVtable = 0x1095484;
		constexpr size_t kMenuBaseVtableEntryCount = 18;
		constexpr size_t kMenuGetIdVtableIndex = 13;
		constexpr SInt32 kImeMenuDepthContribution = -1000000;

		using CreateMenuByClassFn =
			Menu* (__thiscall*)(void*, UInt32);

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
		bool s_imeMenuFactoryInstalled = false;
		bool s_imeMenuFactoryInstallFailed = false;
		bool s_creatingImeMenu = false;

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

		bool EnsureImeMenuFactory()
		{
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
			// Prewarm remains attached to pMenuRoot so it can be shown before
			// HUDMainMenu is available. The startup-only component is never
			// active while the Pip-Boy input overlay is in use.
			ClearPrewarmResolvedTiles();
			s_state.prewarmParent = parent;
			s_state.prewarmLoadFailed = false;
		}

		Tile* SynchronizeImeParent()
		{
			InterfaceManager* manager = InterfaceManager::GetSingleton();
			// The IME tree is a dedicated non-stacking Menu. As a direct
			// pMenuRoot child it participates in the engine's menu isolation
			// exactly once and is independent of HUDMainMenu/Pip-Boy culling.
			Tile* parent = manager ? manager->pMenuRoot : nullptr;
			if (s_state.imeParent != parent)
				ResetImeForParent(parent);
			return parent;
		}

		Tile* SynchronizePrewarmParent()
		{
			InterfaceManager* manager = InterfaceManager::GetSingleton();
			Tile* parent = manager ? manager->pMenuRoot : nullptr;
			if (s_state.prewarmParent != parent)
				ResetPrewarmForParent(parent);
			return parent;
		}

		void DestroyAttachedRoot(
			Tile* parent,
			Tile* root,
			const char* expectedName)
		{
			if (IsNamedDirectChild(parent, root, expectedName))
				delete root;
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
					s_imeReady.store(true, std::memory_order_release);
					return true;
				}
				if (IsDirectChild(parent, root) && ResolveImeTiles(root))
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
				DestroyAttachedRoot(parent, root, "tNVSE_IME");
				gLog.FormattedMessage(
					"tnvse_native_overlay: IME Menu was detached or malformed; reloading");
			}
			if (s_state.imeLoadFailed)
				return false;
			if (Tile* existing =
					FindDirectMenuByClass(parent, kImeMenuClass))
			{
				if (ResolveImeTiles(existing))
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

				s_state.imeLoadFailed = true;
				gLog.FormattedMessage(
					"tnvse_native_overlay: IME Menu class=%u is already owned by Tile '%s'; system IME UI remains enabled for this UI root",
					kImeMenuClass,
					existing->strName.c_str());
				return false;
			}
			if (!EnsureImeMenuFactory())
			{
				s_state.imeLoadFailed = true;
				gLog.FormattedMessage(
					"tnvse_native_overlay: IME Menu factory unavailable; system IME UI remains enabled for this UI root");
				return false;
			}

			s_creatingImeMenu = true;
			Tile* root = ThisStdCall<Tile*>(
				kReadTileXml, parent, kImeOverlayXmlPath);
			s_creatingImeMenu = false;
			if (!root || !IsDirectChild(parent, root)
				|| !ResolveImeTiles(root))
			{
				ClearImeResolvedTiles();
				DestroyAttachedRoot(parent, root, "tNVSE_IME");
				s_state.imeLoadFailed = true;
				gLog.FormattedMessage(
					"tnvse_native_overlay: failed to load or resolve IME Menu path='%s'; system IME UI remains enabled for this UI root",
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
				DestroyAttachedRoot(parent, root, "tNVSE_Prewarm");
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
				DestroyAttachedRoot(parent, root, "tNVSE_Prewarm");
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
			return false;
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
		for (size_t i = 0; i < visibleCount; ++i)
		{
			const float measuredHeight =
				s_state.imeLines[i]->GetValueFloat(
					Tile::kTileValue_height);
			if (std::isfinite(measuredHeight) && measuredHeight > 0.0f)
				lineHeight = std::max(lineHeight, measuredHeight);

			const std::string encoded = WideToUiText(lines[i].text);
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
					Tile::kTileValue_y, lineY, true);
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
		SynchronizeOverlayDepth(s_state.prewarmRoot);
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
		const bool attached =
			IsNamedDirectChild(
				parent, s_state.prewarmRoot, "tNVSE_Prewarm");
		if (s_state.prewarmTileVisible
			&& IsNativePrewarmOverlayHostReady()
			&& attached)
			SetVisible(s_state.prewarmRoot, false);
		if (s_state.prewarmRoot && !attached)
			ClearPrewarmResolvedTiles();
		s_state.prewarmTileVisible = false;
	}

	bool IsNativePrewarmOverlayActive()
	{
		return s_prewarmActive.load(std::memory_order_acquire);
	}

	void ShutdownNativeTileOverlayHost()
	{
		// Publish fail-open before deleting either tree. TSF callbacks only
		// inspect these atomics and must never hide system UI while destruction
		// is in progress.
		s_imeReady.store(false, std::memory_order_release);
		s_prewarmReady.store(false, std::memory_order_release);
		s_prewarmActive.store(false, std::memory_order_release);
		InterfaceManager* manager = InterfaceManager::GetSingleton();
		Tile* currentImeParent =
			manager ? manager->pMenuRoot : nullptr;
		Tile* currentPrewarmParent =
			manager ? manager->pMenuRoot : nullptr;
		if (currentImeParent
			&& currentImeParent == s_state.imeParent)
		{
			Tile* imeRoot = s_state.imeRoot;
			if (IsNamedDirectChild(
					currentImeParent, imeRoot, "tNVSE_IME"))
			{
				SetVisible(imeRoot, false);
				delete imeRoot;
			}
		}
		if (currentPrewarmParent
			&& currentPrewarmParent == s_state.prewarmParent)
		{
			Tile* prewarmRoot = s_state.prewarmRoot;
			if (IsNamedDirectChild(
					currentPrewarmParent,
					prewarmRoot,
					"tNVSE_Prewarm"))
			{
				SetVisible(prewarmRoot, false);
				delete prewarmRoot;
			}
		}
		ResetImeForParent(nullptr);
		ResetPrewarmForParent(nullptr);
	}
}

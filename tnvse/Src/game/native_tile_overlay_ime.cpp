#include "native_tile_overlay_detail.h"

namespace fonthook
{
	namespace implementation::native_tile_overlay {}
	using namespace implementation::native_tile_overlay;

	namespace implementation::native_tile_overlay
	{
		Tile* SynchronizeImeParent()
		{
			InterfaceManager* manager = InterfaceManager::GetSingleton();
			// The IME tree is a dedicated non-stacking Menu and a direct
			// pMenuRoot child. The Pip-Boy RTT hook temporarily culls its
			// NiNode while that root is captured, then restores screen drawing.
			Tile* parent = manager ? manager->pMenuRoot : nullptr;
			if (OverlayRuntime().state.imeParent != parent)
				ResetImeForParent(parent);
			return parent;
		}

		void ClearImeResolvedTiles()
		{
			const bool hadResolvedTiles = OverlayRuntime().state.imeRoot
				|| OverlayRuntime().state.imeBackground
				|| std::any_of(
					OverlayRuntime().state.imeLines.begin(),
					OverlayRuntime().state.imeLines.end(),
					[](Tile* tile) { return tile != nullptr; })
				|| std::any_of(
					OverlayRuntime().state.imeHighlights.begin(),
					OverlayRuntime().state.imeHighlights.end(),
					[](Tile* tile) { return tile != nullptr; });
			OverlayRuntime().imeReady.store(false, std::memory_order_release);
			OverlayRuntime().state.imeRoot = nullptr;
			OverlayRuntime().state.imeBackground = nullptr;
			OverlayRuntime().state.imeLines.fill(nullptr);
			OverlayRuntime().state.imeHighlights.fill(nullptr);
			OverlayRuntime().state.imeKey.clear();
			OverlayRuntime().state.imeVisibleLineCount = 0;
			OverlayRuntime().state.imeLineHeight = 0.0f;
			OverlayRuntime().state.imeContentWidth = 0.0f;
			OverlayRuntime().state.imeVisible = false;
			if (hadResolvedTiles)
				AdvanceImeHostGeneration();
		}


		void ResetImeForParent(Tile* parent)
		{
			// A changed pMenuRoot means every pointer under the old host
			// may already have been released. Never dereference or destroy it
			// here.
			ClearImeResolvedTiles();
			OverlayRuntime().state.imeParent = parent;
			OverlayRuntime().state.imeLoadFailed = false;
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
			// vanilla Create() finalization after ReadXML completes, matching
			// built-in menus and Stewie Tweaks' injected menu lifecycle.
			menu->uiID = kImeMenuClass;
			ThisStdCall<void>(
				kMenuSetMenuTile,
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

			const bool changed = OverlayRuntime().state.imeRoot != root
				|| OverlayRuntime().state.imeBackground != background
				|| OverlayRuntime().state.imeLines != lines
				|| OverlayRuntime().state.imeHighlights != highlights;
			OverlayRuntime().state.imeRoot = root;
			OverlayRuntime().state.imeBackground = background;
			OverlayRuntime().state.imeLines = lines;
			OverlayRuntime().state.imeHighlights = highlights;
			if (changed)
				AdvanceImeHostGeneration();
			return true;
		}


		bool IsResolvedImeTreeAttached(Tile* parent)
		{
			if (!IsNamedDirectChild(
					parent, OverlayRuntime().state.imeRoot, "tNVSE_IME")
				|| !IsNamedDirectChild(
					OverlayRuntime().state.imeRoot,
					OverlayRuntime().state.imeBackground,
					"tNVSE_IME_Background"))
			{
				return false;
			}
			for (size_t i = 0; i < OverlayRuntime().state.imeLines.size(); ++i)
			{
				char name[32] = {};
				_snprintf_s(name, _countof(name), _TRUNCATE,
					"tNVSE_IME_Line%02u", static_cast<UInt32>(i));
				if (!IsNamedDirectChild(
						OverlayRuntime().state.imeRoot, OverlayRuntime().state.imeLines[i], name))
					return false;
			}
			for (size_t i = 0; i < OverlayRuntime().state.imeHighlights.size(); ++i)
			{
				char name[40] = {};
				_snprintf_s(name, _countof(name), _TRUNCATE,
					"tNVSE_IME_Highlight%02u", static_cast<UInt32>(i));
				if (!IsNamedDirectChild(
						OverlayRuntime().state.imeRoot, OverlayRuntime().state.imeHighlights[i], name))
					return false;
			}
			return true;
		}


		void ResetImePresentationState()
		{
			OverlayRuntime().state.imeKey.clear();
			OverlayRuntime().state.imeVisibleLineCount = 0;
			OverlayRuntime().state.imeLineHeight = 0.0f;
			OverlayRuntime().state.imeContentWidth = 0.0f;
			OverlayRuntime().state.imeVisible = false;
		}


		bool EnsureImeHost(Tile* parent)
		{
			if (OverlayRuntime().state.imeRoot)
			{
				Tile* root = OverlayRuntime().state.imeRoot;
				if (IsResolvedImeTreeAttached(parent))
				{
					if (IsImeMenuRegistered(root)
						|| RegisterImeMenuRoot(root))
					{
						OverlayRuntime().imeReady.store(true, std::memory_order_release);
						return true;
					}
				}
				if (IsDirectChild(parent, root)
					&& ResolveImeTiles(root)
					&& RegisterImeMenuRoot(root))
				{
					ResetImePresentationState();
					SetVisible(root, false);
					OverlayRuntime().imeReady.store(true, std::memory_order_release);
					gLog.FormattedMessage(
						"tnvse_native_overlay: rebound IME Menu after child-tree replacement host=%p parent=%p",
						root, parent);
					return true;
				}
				ClearImeResolvedTiles();
				OverlayRuntime().state.imeLoadFailed = false;
				ReleaseAndDestroyAttachedRoot(
					parent, root, "tNVSE_IME");
				gLog.FormattedMessage(
					"tnvse_native_overlay: IME Menu was detached, malformed, or lost its native registration; reloading");
			}
			if (OverlayRuntime().state.imeLoadFailed)
				return false;

			if (Tile* existing = NormalizeOwnedImeRoots(parent))
			{
				if (ResolveImeTiles(existing)
					&& RegisterImeMenuRoot(existing))
				{
					ResetImePresentationState();
					SetVisible(existing, false);
					OverlayRuntime().imeReady.store(true, std::memory_order_release);
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
				OverlayRuntime().state.imeLoadFailed = true;
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
				OverlayRuntime().state.imeLoadFailed = true;
				gLog.FormattedMessage(
					"tnvse_native_overlay: IME Menu factory unavailable; native IME overlay unavailable and system candidate UI remains suppressed");
				return false;
			}

			OverlayRuntime().creatingImeMenu = true;
			Tile* root = ThisStdCall<Tile*>(
				kTileReadFile, parent, kImeOverlayXmlPath);
			OverlayRuntime().creatingImeMenu = false;
			if (!root || !IsDirectChild(parent, root)
				|| !ResolveImeTiles(root)
				|| !RegisterImeMenuRoot(root))
			{
				ClearImeResolvedTiles();
				ReleaseAndDestroyAttachedRoot(
					parent, root, "tNVSE_IME");
				OverlayRuntime().state.imeLoadFailed = true;
				gLog.FormattedMessage(
					"tnvse_native_overlay: failed to load, resolve, or register IME Menu path='%s'; native IME overlay unavailable and system candidate UI remains suppressed",
					kImeOverlayXmlPath);
				return false;
			}

			SetVisible(OverlayRuntime().state.imeRoot, false);
			OverlayRuntime().imeReady.store(true, std::memory_order_release);
			gLog.FormattedMessage(
				"tnvse_native_overlay: loaded IME Menu path='%s' host=%p parent=%p class=%u",
				kImeOverlayXmlPath,
				OverlayRuntime().state.imeRoot,
				parent,
				kImeMenuClass);
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

	}

}

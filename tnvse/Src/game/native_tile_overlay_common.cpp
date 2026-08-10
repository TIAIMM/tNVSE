#include "native_tile_overlay_detail.h"

namespace fonthook
{
	namespace implementation::native_tile_overlay {}
	using namespace implementation::native_tile_overlay;

	namespace implementation::native_tile_overlay
	{
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

		void PublishTextGeometry(
			Tile* tile,
			std::wstring_view value,
			bool forceRefresh)
		{
			if (!tile)
				return;
			const std::string encoded = WideToUiText(value);
			if (forceRefresh)
			{
				// Tile::SetValueString may leave an equal string untouched. An
				// IME status row can therefore retain geometry that was created
				// while its Menu ancestor was hidden. Clear it once on logical
				// activation/repair so the replacement shape is built with the
				// now-visible ancestor and current TileShader alpha.
				tile->SetValueString(
					Tile::kTileValue_string, "", true);
			}
			tile->SetValueString(
				Tile::kTileValue_string, encoded.c_str(), true);
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
				if (!child || child == OverlayRuntime().state.imeRoot)
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

		bool HasExpectedImeLinePresentation(size_t visibleCount)
		{
			if (!visibleCount || visibleCount > kImeLineCount)
				return false;
			for (size_t i = 0; i < kImeLineCount; ++i)
			{
				Tile* line = OverlayRuntime().state.imeLines[i];
				if (!line)
					return false;
				const bool expectedVisible = i < visibleCount;
				const bool visible =
					line->GetValueFloat(
						Tile::kTileValue_visible) > 0.5f;
				if (visible != expectedVisible)
					return false;
				if (expectedVisible
					&& !line->GetValueString(
						Tile::kTileValue_string)[0])
				{
					return false;
				}
			}
			return true;
		}


		void ReleaseAndDestroyAttachedRoot(
			Tile* parent,
			Tile* root,
			const char* expectedName)
		{
			if (IsNamedDirectChild(parent, root, expectedName))
			{
				SetVisible(root, false);
				ThisStdCall<void>(kTileRelease, root);
				delete root;
			}
		}

	}

}

#pragma once

#include "globals.h"
#include "native_calls.h"

#include <cmath>

namespace fonthook
{
	using ExtraGlyphMap = std::unordered_map<UInt32, FontLetter>;

	enum class LayoutWrapKind : UInt8
	{
		None,
		Soft,
		Hard
	};

	struct LayoutWrapResult
	{
		LayoutWrapKind kind = LayoutWrapKind::None;
		double completedWidth = 0.0;
	};

	struct LayoutWrapState
	{
		double currentLineWidth = 0.0;
		double previousUnitWidth = 0.0;
		UInt32 currentLineUnitCount = 0;
		double softWrapWidth = 0.0;
		UInt32 unitsAtSoftWrap = 0;
		bool hasSoftWrap = false;

		void ResetLine()
		{
			currentLineWidth = 0;
			previousUnitWidth = 0;
			currentLineUnitCount = 0;
			ClearSoftWrap();
		}

		void ClearSoftWrap()
		{
			softWrapWidth = 0;
			unitsAtSoftWrap = 0;
			hasSoftWrap = false;
		}

		void MarkSoftWrap()
		{
			if (!currentLineUnitCount)
				return;
			softWrapWidth = currentLineWidth;
			unitsAtSoftWrap = currentLineUnitCount;
			hasSoftWrap = true;
		}

		void AdvanceTab(UInt32 tabWidth)
		{
			if (tabWidth)
			{
				const double remainder = std::fmod(currentLineWidth, static_cast<double>(tabWidth));
				currentLineWidth += static_cast<double>(tabWidth) - remainder;
			}
		}

		LayoutWrapResult AddUnit(double unitWidth, float maxWidth)
		{
			LayoutWrapResult result;
			currentLineWidth += unitWidth;
			const UInt32 totalUnits = currentLineUnitCount + 1;

			if (static_cast<float>(currentLineWidth) > maxWidth)
			{
				if (hasSoftWrap)
				{
					result.kind = LayoutWrapKind::Soft;
					result.completedWidth = softWrapWidth;
					currentLineWidth -= softWrapWidth;
					currentLineUnitCount = totalUnits >= unitsAtSoftWrap
						? totalUnits - unitsAtSoftWrap : 0;
					ClearSoftWrap();
				}
				else if (currentLineUnitCount)
				{
					result.kind = LayoutWrapKind::Hard;
					const double movedWidth = previousUnitWidth + unitWidth;
					result.completedWidth = currentLineWidth >= movedWidth
						? currentLineWidth - movedWidth : 0;
					currentLineWidth = movedWidth;
					currentLineUnitCount = 2;
					ClearSoftWrap();
				}
				else
				{
					currentLineUnitCount = totalUnits;
				}
			}
			else
			{
				currentLineUnitCount = totalUnits;
			}

			previousUnitWidth = unitWidth;
			return result;
		}
	};

	inline bool HasAnyExtraGlyphs()
	{
		return !gNumberedExtraLetters.empty();
	}

	inline ExtraGlyphMap* GetExtraGlyphs(int fontNum)
	{
		auto it = gNumberedExtraLetters.find(fontNum);
		return it != gNumberedExtraLetters.end() ? &it->second : nullptr;
	}

	inline bool HasExtraGlyphsForFont(int fontNum)
	{
		return GetExtraGlyphs(fontNum) != nullptr;
	}

	inline FontLetter* LookupDBGlyph(ExtraGlyphMap* extraGlyphs, UInt32 code)
	{
		if (!extraGlyphs)
			return nullptr;

		auto it = extraGlyphs->find(code);
		return it != extraGlyphs->end() ? &it->second : nullptr;
	}

	// The original code consumes FontLetter spacing differently in render,
	// CharData layout, and string measurement paths. Keep those rules separate.
	inline float GetGlyphRenderAdvance(const FontLetter* glyph)
	{
		if (!glyph)
			return 0.0f;

		return glyph->fLeadingEdge + glyph->fWidth +
			(glyph->fWidth > 0.0f ? glyph->fSpacing : 0.0f);
	}

	inline float GetGlyphCharDataWidth(const FontLetter* glyph)
	{
		if (!glyph)
			return 0.0f;

		return glyph->fWidth +
			(glyph->fWidth > 0.0f ? glyph->fLeadingEdge + glyph->fSpacing : 0.0f);
	}

	inline float GetGlyphMeasureWidth(const FontLetter* glyph)
	{
		return glyph ? glyph->fLeadingEdge + glyph->fWidth + glyph->fSpacing : 0.0f;
	}

	inline UInt32 GetGlyphLayoutWidth(const FontLetter* glyph)
	{
		return ConditionalFloatToUInt(GetGlyphRenderAdvance(glyph));
	}

	inline UInt32 GetGlyphCharDataLayoutWidth(const FontLetter* glyph)
	{
		return ConditionalFloatToUInt(GetGlyphCharDataWidth(glyph));
	}

	inline float GetGlyphLineHeight(const FontData* fontData, const FontLetter* glyph)
	{
		return (fontData && glyph) ? fontData->fBaseLine - glyph->fTopEdge + glyph->fHeight : 0.0f;
	}

	inline UInt32 GetGlyphLayoutLineHeight(const FontData* fontData, const FontLetter* glyph)
	{
		return ConditionalFloatToUInt(GetGlyphLineHeight(fontData, glyph));
	}
}

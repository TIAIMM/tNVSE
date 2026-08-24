#pragma once

// Private declarations shared by direct-atlas cache, build, and runtime
// implementation units. The public direct-atlas surface remains declared by
// the existing font internal headers.

namespace fonthook::vectorfont
{
	namespace implementation::font_atlas_direct
	{
		inline constexpr UInt32 kFirstLeadByte = 0x81;
		inline constexpr UInt32 kLastLeadByte = 0xFE;
		inline constexpr UInt32 kFirstTrailByte = 0x40;
		inline constexpr UInt32 kLastTrailByte = 0xFE;
		inline constexpr UInt32 kGlyphsPerDoubleByteRow =
			kLastTrailByte - kFirstTrailByte + 1;
		inline constexpr UInt32 kDoubleByteGlyphSlots =
			(kLastLeadByte - kFirstLeadByte + 1)
				* kGlyphsPerDoubleByteRow;
		inline constexpr size_t kDirectBuildBatchGlyphs = 512;

		const DirectAtlasGlyphLayer* FindDirectLayer(
			const DirectCachedLetter& letter, GlyphMaskType mask);
		DirectAtlasGlyphLayer* FindOrCreateDirectLayer(
			DirectCachedLetter& letter, GlyphMaskType mask);
		UInt64 HashDirectBytes(const void* data, size_t size,
			UInt64 hash = 1469598103934665603ull);
		UInt64 BuildDirectProfileIdentity(const AtlasCacheKey& key,
			const DirectAtlasGlyphTable& table,
			const std::vector<std::shared_ptr<AtlasResource>>& pages);
		UInt64 BuildDirectPageIdentityChecksum(
			const std::vector<std::shared_ptr<AtlasResource>>& pages);
		UInt8 BuildDirectEffectLayerMask(
			const std::vector<GlyphMaskType>& masks);
		size_t GetDirectTableStorageBytes(
			const DirectAtlasGlyphTable& table);
		std::wstring GetDirectCachedLetterPath(
			UInt64 identity, VectorFontByteClass byteClass);
		void NormalizeKnownEmptyAdvance(float& width, float& spacing);
		void NormalizeKnownEmptyAdvance(FontLetter& letter);
		void NormalizeKnownEmptyAdvance(DirectCachedLetter& letter);
		bool TryLoadDirectCachedLetters(const std::wstring& path,
			UInt64 identity, VectorFontByteClass byteClass,
			const AtlasCacheKey& key,
			const std::vector<std::shared_ptr<AtlasResource>>& pages,
			const std::vector<GlyphMaskType>& masks,
			DirectAtlasGlyphTable& table);
		bool SaveDirectCachedLetters(const std::wstring& path,
			UInt64 identity, VectorFontByteClass byteClass,
			const AtlasCacheKey& key,
			const std::vector<std::shared_ptr<AtlasResource>>& pages,
			const std::vector<GlyphMaskType>& masks,
			const DirectAtlasGlyphTable& table);
		size_t GetDirectGlyphSlotCount(VectorFontByteClass byteClass);
		bool ResolveDirectGlyphSlot(VectorFontByteClass byteClass,
			UInt32 encodedCode, size_t& slot);
		UInt32 EncodeDirectGlyphSlot(
			VectorFontByteClass byteClass, size_t slot);
		bool ResolveDirectGlyphSource(const DirectAtlasGlyphTable& table,
			const VectorEncodedGlyph& glyph, GlyphMaskType maskType,
			PendingQuad::GlyphSource& result);
		bool IsSealedDirectProfileValid(
			const SealedDirectFontProfile& sealed);
	}
}

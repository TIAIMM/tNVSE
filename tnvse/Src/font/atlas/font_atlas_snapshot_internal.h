#pragma once

#include "font_atlas_internal.h"

namespace fonthook::vectorfont
{
	namespace implementation::font_atlas_snapshot
	{
		inline constexpr UInt32 kPhysicalAtlasGroupVersion = 2;
		inline constexpr UInt32 kPhysicalAtlasPoolVersion = 3;

		struct PhysicalAtlasGroupMember
		{
			const FontConfig* config = nullptr;
			AtlasCacheKey singleByteKey;
			AtlasCacheKey doubleByteKey;
		};

		struct PhysicalAtlasGroup
		{
			UInt32 version = kPhysicalAtlasGroupVersion;
			UInt64 identity = 0;
			UInt32 ownerFontId = 0;
			std::vector<PhysicalAtlasGroupMember> members;
			std::vector<AtlasProfileKey> uniqueSingleByteProfiles;
			std::vector<AtlasProfileKey> uniqueDoubleByteProfiles;
			std::vector<UInt64> uniqueDoubleByteLayoutHashes;
		};

		struct PhysicalAtlasGroupPreview
		{
			bool evaluated = false;
			bool feasible = false;
			UInt32 pageCount = 0;
			UInt32 width = 0;
			UInt32 height = 0;
			UInt64 placementCount = 0;
			UInt64 sourceGpuBytes = 0;
			UInt64 candidateGpuBytes = 0;
		};

		struct SnapshotPackingCaps
		{
			UInt32 singleByteMaximum = kSingleByteAtlasHardLimit;
			UInt32 doubleByteMaximum = kDoubleByteAtlasHardLimit;
			UInt32 maximumAspectRatio = 0;
		};

		struct SnapshotPixelSource
		{
			std::shared_ptr<const CompactAtlasSnapshot> snapshot;
			size_t sourceOffset = 0;
			size_t destinationOffset = 0;
			size_t bytes = 0;
		};

		struct SnapshotPageData
		{
			AtlasCacheKey key;
			AtlasSnapshotHeader header;
			std::vector<AtlasSnapshotPlacement> placements;
			std::vector<UInt8> pixels;
			std::vector<SnapshotPixelSource> pixelSources;
			std::wstring path;
		};

		struct SnapshotPayloadSource
		{
			std::wstring path;
			AtlasSnapshotHeader header;
			UInt64 fileBytes = 0;
		};

		UInt64 HashAtlasBytes(const void* data, size_t size,
			UInt64 hash = 1469598103934665603ull);
		bool BuildPhysicalAtlasGroup(const FontConfig& anchor,
			UInt32 scaleMilli, PhysicalAtlasGroup& group);
		bool BuildPhysicalAtlasPool(
			const std::vector<PhysicalAtlasGroupMember>& members,
			PhysicalAtlasGroup& pool);
		bool IsPhysicalAtlasGroupResidentLocked(const PhysicalAtlasGroup& group,
			std::shared_ptr<AtlasResource>* sharedResource = nullptr,
			const char** failureReason = nullptr);
		bool IsPhysicalAtlasGroupFallbackMarkedLocked(
			const PhysicalAtlasGroup& group);
		SnapshotPackingCaps GetSnapshotPackingCaps();
		UInt32 GetSnapshotMaximumSize(const SnapshotPackingCaps& caps,
			VectorFontByteClass byteClass);
		bool IsSnapshotPageShapeValid(UInt32 width, UInt32 height,
			UInt32 maximumSize, const SnapshotPackingCaps& caps);
		bool TryEnableSparseFile(HANDLE file);
		bool WriteSequentialFileBytes(HANDLE file, const void* data, size_t size);
		bool IsMissingFileError(DWORD error);
		bool WriteSparseFileBytes(HANDLE file, const UInt8* data, size_t size,
			bool sparse);
		UInt64 ComputeAtlasSnapshotIdentityHash(const AtlasCacheKey& key,
			UInt64 maskContentHash, const FontConfig& config);
		std::wstring GetAtlasSnapshotPath(RuntimeFont& runtime,
			const AtlasCacheKey& key, UInt64& snapshotHash,
			UInt64& maskContentHash);
		bool GetPlacedLevelZeroSnapshotBytes(
			const std::vector<AtlasSnapshotPlacement>& placements,
			UInt32 width, UInt32 height, AtlasPixelMode pixelMode,
			size_t& bytes);
		bool UsesPlacedLevelZeroSnapshot(const AtlasCacheKey& key);
		bool MakeSnapshotPlacement(const AtlasResource& resource, UInt64 cacheId,
			const AtlasRect& rect, AtlasSnapshotPlacement& placement);
		bool IsValidSnapshotPlacement(const AtlasSnapshotPlacement& placement);
		bool MatchesDefaultPoolSnapshotLayout(const AtlasSnapshotHeader& header);
		bool IsCompleteAtlasProfileResidentLocked(AtlasState& state,
			const AtlasCacheKey& baseKey, UInt16* pageCount = nullptr,
			UInt64* placementCount = nullptr);
		bool IsGloballyRepackedAtlasProfileResidentLocked(AtlasState& state,
			const AtlasCacheKey& baseKey);
		bool TryReuseCompleteAtlasProfile(const AtlasCacheKey& key);
		bool BuildAtlasSnapshotPixels(const AtlasResource& resource,
			const std::vector<AtlasSnapshotPlacement>& placements,
			AtlasSnapshotStorage storageMode, std::vector<UInt8>& pixels);
		UInt64 ComputeAtlasPageContentHash(const AtlasSnapshotHeader& header,
			const std::vector<AtlasSnapshotPlacement>& placements,
			const std::vector<UInt8>& pixels);
		bool BuildRepackedSnapshotPages(const AtlasCacheKey& baseKey,
			const std::vector<std::pair<AtlasCacheKey,
				std::shared_ptr<AtlasResource>>>& resources,
			std::vector<SnapshotPageData>& pages, UInt64& originalGpuBytes,
			VectorFontByteClass packingByteClass,
			size_t maximumAcceptedPages = 0,
			bool emitDiagnostics = true);
		bool DecodeAtlasSnapshotPixels(const AtlasSnapshotHeader& header,
			const std::vector<AtlasSnapshotPlacement>& placements,
			const UInt8* storedPixels, std::vector<UInt8>& pixels);
		bool ReadSnapshotBytesExact(HANDLE file, void* destination, size_t size);
		bool WriteRepackedSnapshotPixels(HANDLE file,
			const SnapshotPageData& page, UInt64& payloadChecksum,
			UInt64& writtenBytes);
		bool ReadSnapshotMetadata(const std::wstring& path,
			AtlasSnapshotHeader& header,
			std::vector<AtlasSnapshotPlacement>* placements,
			SnapshotPayloadSource& payload);
		bool InspectSnapshotRoleStorage(RuntimeFont& runtime,
			const AtlasCacheKey& baseKey, size_t& storageBytes);
		bool MarkPhysicalAtlasGroupFallback(RuntimeFont& runtime,
			const AtlasCacheKey& baseKey, UInt32 pageCount);
	}

	bool LoadGlyphAtlasSnapshotRole(RuntimeFont& runtime,
		VectorFontByteClass byteClass, float rasterScale, bool metadataOnly);
	bool SaveGlyphAtlasSnapshotRole(RuntimeFont& runtime,
		VectorFontByteClass byteClass, float rasterScale,
		bool* jointRolePublished = nullptr,
		const implementation::font_atlas_snapshot::PhysicalAtlasGroup*
			physicalGroup = nullptr,
		bool* physicalGroupFallback = nullptr,
		implementation::font_atlas_snapshot::PhysicalAtlasGroupPreview*
			physicalGroupPreview = nullptr);
}

from pathlib import Path


def replace_once(path: str, old: str, new: str) -> None:
    file = Path(path)
    text = file.read_text(encoding="utf-8")
    count = text.count(old)
    if count != 1:
        raise RuntimeError(f"{path}: expected one match, found {count}")
    file.write_text(text.replace(old, new, 1), encoding="utf-8")


internal = "tnvse/Src/font/atlas/font_atlas_internal.h"
stream = "tnvse/Src/font/atlas/font_atlas_stream.cpp"
snapshot = "tnvse/Src/font/atlas/font_atlas_snapshot.cpp"

replace_once(
    internal,
    '#include <atomic>\n#include <list>',
    '#include <atomic>\n#include <cmath>\n#include <list>')

replace_once(
    internal,
    '''\tinline bool IsAtlasGlyphPlacementCurrent(const AtlasGlyphPlacement& placement,
\t\tconst AtlasResource& atlas, UInt16 pageIndex)
\t{
\t\treturn placement.atlasIdentity == reinterpret_cast<uintptr_t>(&atlas)
\t\t\t&& placement.atlasGeneration == atlas.generation
\t\t\t&& placement.atlasWidth == atlas.width
\t\t\t&& placement.atlasHeight == atlas.height
\t\t\t&& placement.pageIndex == pageIndex
\t\t\t&& placement.inverseWidth > 0.0f
\t\t\t&& placement.inverseHeight > 0.0f;
\t}

\tinline bool CacheAtlasGlyphPlacement(AtlasGlyphRecord& glyph,
\t\tconst AtlasResource& atlas, UInt16 pageIndex)
\t{
\t\tif (!atlas.width || !atlas.height || !glyph.rect.width || !glyph.rect.height)
\t\t\treturn false;
\t\tif (IsAtlasGlyphPlacementCurrent(glyph.placement, atlas, pageIndex))
\t\t\treturn true;
\t\tAtlasGlyphPlacement placement;
\t\tplacement.atlasIdentity = reinterpret_cast<uintptr_t>(&atlas);
\t\tplacement.atlasGeneration = atlas.generation;
\t\tplacement.atlasWidth = atlas.width;
\t\tplacement.atlasHeight = atlas.height;
\t\tplacement.pageIndex = pageIndex;
\t\tplacement.inverseWidth = 1.0f / static_cast<float>(atlas.width);
\t\tplacement.inverseHeight = 1.0f / static_cast<float>(atlas.height);
\t\tplacement.u0 = static_cast<float>(glyph.rect.x) * placement.inverseWidth;
\t\tplacement.v0 = static_cast<float>(glyph.rect.y) * placement.inverseHeight;
\t\tplacement.u1 = static_cast<float>(glyph.rect.x + glyph.rect.width)
\t\t\t* placement.inverseWidth;
\t\tplacement.v1 = static_cast<float>(glyph.rect.y + glyph.rect.height)
\t\t\t* placement.inverseHeight;
\t\tglyph.placement = placement;
\t\treturn true;
\t}
''',
    '''\tinline bool IsAtlasGlyphPlacementForAtlas(
\t\tconst AtlasGlyphPlacement& placement, const AtlasResource& atlas)
\t{
\t\treturn placement.atlasIdentity == reinterpret_cast<uintptr_t>(&atlas)
\t\t\t&& placement.atlasGeneration == atlas.generation
\t\t\t&& placement.atlasWidth == atlas.width
\t\t\t&& placement.atlasHeight == atlas.height
\t\t\t&& placement.inverseWidth > 0.0f
\t\t\t&& placement.inverseHeight > 0.0f;
\t}

\tinline bool IsAtlasGlyphPlacementCurrent(const AtlasGlyphPlacement& placement,
\t\tconst AtlasResource& atlas, UInt16 pageIndex)
\t{
\t\treturn IsAtlasGlyphPlacementForAtlas(placement, atlas)
\t\t\t&& placement.pageIndex == pageIndex;
\t}

\tinline bool CacheAtlasGlyphPlacement(AtlasGlyphRecord& glyph,
\t\tconst AtlasResource& atlas, UInt16 pageIndex)
\t{
\t\tif (!atlas.width || !atlas.height || !glyph.rect.width || !glyph.rect.height)
\t\t\treturn false;
\t\tif (IsAtlasGlyphPlacementCurrent(glyph.placement, atlas, pageIndex))
\t\t\treturn true;
\t\t// Snapshot records store profile-local page numbers. Text batches compact the
\t\t// pages from both byte roles into one list, so only the runtime page ordinal
\t\t// needs rebinding when the same atlas object and generation are still active.
\t\tif (IsAtlasGlyphPlacementForAtlas(glyph.placement, atlas))
\t\t{
\t\t\tglyph.placement.pageIndex = pageIndex;
\t\t\treturn true;
\t\t}
\t\tAtlasGlyphPlacement placement;
\t\tplacement.atlasIdentity = reinterpret_cast<uintptr_t>(&atlas);
\t\tplacement.atlasGeneration = atlas.generation;
\t\tplacement.atlasWidth = atlas.width;
\t\tplacement.atlasHeight = atlas.height;
\t\tplacement.pageIndex = pageIndex;
\t\tplacement.inverseWidth = 1.0f / static_cast<float>(atlas.width);
\t\tplacement.inverseHeight = 1.0f / static_cast<float>(atlas.height);
\t\tplacement.u0 = static_cast<float>(glyph.rect.x) * placement.inverseWidth;
\t\tplacement.v0 = static_cast<float>(glyph.rect.y) * placement.inverseHeight;
\t\tplacement.u1 = static_cast<float>(glyph.rect.x + glyph.rect.width)
\t\t\t* placement.inverseWidth;
\t\tplacement.v1 = static_cast<float>(glyph.rect.y + glyph.rect.height)
\t\t\t* placement.inverseHeight;
\t\tglyph.placement = placement;
\t\treturn true;
\t}
''')

replace_once(
    internal,
    '''\tconstexpr UInt32 kAtlasSnapshotVersion = 10;
\t// This identity-only revision invalidates the old partial codepage snapshot
\t// without forcing complete SDF-fill or unrelated atlas profiles to rebuild.
''',
    '''\tconstexpr UInt32 kAtlasSnapshotVersion = 11;
\t// Version 11 persists the stable page, inverse-size, and normalized UV subset
\t// of AtlasGlyphPlacement. Runtime-only atlas identity and generation are rebound
\t// after the validated prewarm snapshot has created its current AtlasResource.
''')

replace_once(
    internal,
    '''\tstruct AtlasSnapshotPlacement
\t{
\t\tUInt64 cacheId = 0;
\t\tAtlasRect rect;
\t\tSInt32 left = 0;
\t\tSInt32 top = 0;
\t\tSInt32 effectiveWidth = 0;
\t\tSInt32 effectiveHeight = 0;
\t\tSInt32 strokeWidth26Dot6 = 0;
\t\tUInt32 atlasRgb = 0x00FFFFFF;
\t\tUInt32 bakedRgba = 0;
\t\tUInt8 maskType = 0;
\t\tUInt8 sdfSpread = 0;
\t\tUInt8 colorBaked = 0;
\t\tUInt8 bakedLayer = 0;
\t};
''',
    '''\tstruct AtlasSnapshotGlyphPlacement
\t{
\t\tUInt32 atlasWidth = 0;
\t\tUInt32 atlasHeight = 0;
\t\tUInt16 pageIndex = std::numeric_limits<UInt16>::max();
\t\tUInt16 reserved = 0;
\t\tfloat inverseWidth = 0.0f;
\t\tfloat inverseHeight = 0.0f;
\t\tfloat u0 = 0.0f;
\t\tfloat v0 = 0.0f;
\t\tfloat u1 = 0.0f;
\t\tfloat v1 = 0.0f;
\t};

\tstruct AtlasSnapshotPlacement
\t{
\t\tUInt64 cacheId = 0;
\t\tAtlasRect rect;
\t\tSInt32 left = 0;
\t\tSInt32 top = 0;
\t\tSInt32 effectiveWidth = 0;
\t\tSInt32 effectiveHeight = 0;
\t\tSInt32 strokeWidth26Dot6 = 0;
\t\tUInt32 atlasRgb = 0x00FFFFFF;
\t\tUInt32 bakedRgba = 0;
\t\tUInt8 maskType = 0;
\t\tUInt8 sdfSpread = 0;
\t\tUInt8 colorBaked = 0;
\t\tUInt8 bakedLayer = 0;
\t\tAtlasSnapshotGlyphPlacement glyphPlacement;
\t};
''')

replace_once(
    internal,
    '''#pragma pack(pop)

\tstruct CompactAtlasSnapshot
''',
    '''#pragma pack(pop)

\tinline bool CacheAtlasSnapshotGlyphPlacement(
\t\tAtlasSnapshotPlacement& snapshot, UInt32 atlasWidth, UInt32 atlasHeight,
\t\tUInt16 pageIndex)
\t{
\t\tconst AtlasRect& rect = snapshot.rect;
\t\tif (!atlasWidth || !atlasHeight || !rect.width || !rect.height
\t\t\t|| rect.x > atlasWidth || rect.width > atlasWidth - rect.x
\t\t\t|| rect.y > atlasHeight || rect.height > atlasHeight - rect.y)
\t\t\treturn false;
\t\tAtlasSnapshotGlyphPlacement placement;
\t\tplacement.atlasWidth = atlasWidth;
\t\tplacement.atlasHeight = atlasHeight;
\t\tplacement.pageIndex = pageIndex;
\t\tplacement.inverseWidth = 1.0f / static_cast<float>(atlasWidth);
\t\tplacement.inverseHeight = 1.0f / static_cast<float>(atlasHeight);
\t\tplacement.u0 = static_cast<float>(rect.x) * placement.inverseWidth;
\t\tplacement.v0 = static_cast<float>(rect.y) * placement.inverseHeight;
\t\tplacement.u1 = static_cast<float>(rect.x + rect.width)
\t\t\t* placement.inverseWidth;
\t\tplacement.v1 = static_cast<float>(rect.y + rect.height)
\t\t\t* placement.inverseHeight;
\t\tsnapshot.glyphPlacement = placement;
\t\treturn true;
\t}

\tinline bool IsValidAtlasSnapshotGlyphPlacement(
\t\tconst AtlasSnapshotPlacement& snapshot, UInt32 atlasWidth,
\t\tUInt32 atlasHeight, UInt16 pageIndex)
\t{
\t\tconst AtlasSnapshotGlyphPlacement& cached = snapshot.glyphPlacement;
\t\tif (cached.atlasWidth != atlasWidth || cached.atlasHeight != atlasHeight
\t\t\t|| cached.pageIndex != pageIndex || !std::isfinite(cached.inverseWidth)
\t\t\t|| !std::isfinite(cached.inverseHeight) || !std::isfinite(cached.u0)
\t\t\t|| !std::isfinite(cached.v0) || !std::isfinite(cached.u1)
\t\t\t|| !std::isfinite(cached.v1) || cached.inverseWidth <= 0.0f
\t\t\t|| cached.inverseHeight <= 0.0f || cached.u0 < 0.0f
\t\t\t|| cached.v0 < 0.0f || cached.u1 <= cached.u0
\t\t\t|| cached.v1 <= cached.v0 || cached.u1 > 1.0f || cached.v1 > 1.0f)
\t\t\treturn false;
\t\tconst float inverseWidth = 1.0f / static_cast<float>(atlasWidth);
\t\tconst float inverseHeight = 1.0f / static_cast<float>(atlasHeight);
\t\tconst AtlasRect& rect = snapshot.rect;
\t\tconstexpr float epsilon = 1.0e-6f;
\t\tauto matches = [epsilon](float left, float right)
\t\t{
\t\t\treturn std::fabs(left - right) <= epsilon;
\t\t};
\t\treturn matches(cached.inverseWidth, inverseWidth)
\t\t\t&& matches(cached.inverseHeight, inverseHeight)
\t\t\t&& matches(cached.u0, static_cast<float>(rect.x) * inverseWidth)
\t\t\t&& matches(cached.v0, static_cast<float>(rect.y) * inverseHeight)
\t\t\t&& matches(cached.u1,
\t\t\t\tstatic_cast<float>(rect.x + rect.width) * inverseWidth)
\t\t\t&& matches(cached.v1,
\t\t\t\tstatic_cast<float>(rect.y + rect.height) * inverseHeight);
\t}

\tinline bool RestoreAtlasSnapshotGlyphPlacement(
\t\tconst AtlasSnapshotPlacement& snapshot, const AtlasResource& atlas,
\t\tUInt16 snapshotPageIndex, UInt16 runtimePageIndex,
\t\tAtlasGlyphPlacement& placement)
\t{
\t\tif (!IsValidAtlasSnapshotGlyphPlacement(snapshot, atlas.width, atlas.height,
\t\t\tsnapshotPageIndex))
\t\t\treturn false;
\t\tconst AtlasSnapshotGlyphPlacement& cached = snapshot.glyphPlacement;
\t\tplacement.atlasIdentity = reinterpret_cast<uintptr_t>(&atlas);
\t\tplacement.atlasGeneration = atlas.generation;
\t\tplacement.atlasWidth = atlas.width;
\t\tplacement.atlasHeight = atlas.height;
\t\tplacement.pageIndex = runtimePageIndex;
\t\tplacement.inverseWidth = cached.inverseWidth;
\t\tplacement.inverseHeight = cached.inverseHeight;
\t\tplacement.u0 = cached.u0;
\t\tplacement.v0 = cached.v0;
\t\tplacement.u1 = cached.u1;
\t\tplacement.v1 = cached.v1;
\t\treturn true;
\t}

\tstruct CompactAtlasSnapshot
''')

replace_once(
    internal,
    '''\tstatic_assert(sizeof(AtlasSnapshotHeader) == 120);
\tstatic_assert(sizeof(AtlasSnapshotPlacement) == 56);
''',
    '''\tstatic_assert(sizeof(AtlasSnapshotHeader) == 120);
\tstatic_assert(sizeof(AtlasSnapshotGlyphPlacement) == 36);
\tstatic_assert(sizeof(AtlasSnapshotPlacement) == 92);
''')

replace_once(
    stream,
    '''\t\t\theader.placementCount = static_cast<UInt32>(page.placements.size());
\t\t\theader.pixelBytes = page.pixels.size();
\t\t\tUInt64 payloadHash = HashBytes(page.placements.data(),
''',
    '''\t\t\theader.placementCount = static_cast<UInt32>(page.placements.size());
\t\t\theader.pixelBytes = page.pixels.size();
\t\t\tfor (AtlasSnapshotPlacement& placement : page.placements)
\t\t\t{
\t\t\t\tif (!CacheAtlasSnapshotGlyphPlacement(
\t\t\t\t\tplacement, header.width, header.height, header.pageIndex))
\t\t\t\t\treturn false;
\t\t\t}
\t\t\tUInt64 payloadHash = HashBytes(page.placements.data(),
''')

replace_once(
    snapshot,
    '''\t\t\t\t\t|| rect.y > header.height || rect.height > header.height - rect.y
\t\t\t\t\t|| !IsValidSnapshotPlacement(placementList[index]))
''',
    '''\t\t\t\t\t|| rect.y > header.height || rect.height > header.height - rect.y
\t\t\t\t\t|| !IsValidSnapshotPlacement(placementList[index])
\t\t\t\t\t|| !IsValidAtlasSnapshotGlyphPlacement(placementList[index],
\t\t\t\t\t\theader.width, header.height, header.pageIndex))
''')

replace_once(
    snapshot,
    '''\t\t\tRefreshAtlasResourceCpuMemory(*resource);
\t\t\tresource->generation = 1;
\t\t\ttotalBytes += header.pixelBytes;
''',
    '''\t\t\tRefreshAtlasResourceCpuMemory(*resource);
\t\t\tresource->generation = 1;
\t\t\tfor (AtlasGlyphRecord& glyph : resource->glyphs)
\t\t\t{
\t\t\t\tif (glyph.snapshotPlacementIndex >= compactSnapshot->placements.size()
\t\t\t\t\t|| !RestoreAtlasSnapshotGlyphPlacement(
\t\t\t\t\t\tcompactSnapshot->placements[glyph.snapshotPlacementIndex],
\t\t\t\t\t\t*resource, header.pageIndex, header.pageIndex,
\t\t\t\t\t\tglyph.placement))
\t\t\t\t{
\t\t\t\t\treturn false;
\t\t\t\t}
\t\t\t}
\t\t\ttotalBytes += header.pixelBytes;
''')

replace_once(
    snapshot,
    '''\t\t\t\tpage.header.pageCount = static_cast<UInt16>(pages.size());
\t\t\t\tpage.header.placementCount = static_cast<UInt32>(page.placements.size());
\t\t\t\tpage.header.pixelBytes = page.pixels.size();
\t\t\t\tUInt64 payloadHash = page.placements.empty() ? 1469598103934665603ull
''',
    '''\t\t\t\tpage.header.pageCount = static_cast<UInt16>(pages.size());
\t\t\t\tpage.header.placementCount = static_cast<UInt32>(page.placements.size());
\t\t\t\tpage.header.pixelBytes = page.pixels.size();
\t\t\t\tfor (AtlasSnapshotPlacement& placement : page.placements)
\t\t\t\t{
\t\t\t\t\tif (!CacheAtlasSnapshotGlyphPlacement(placement,
\t\t\t\t\t\tpage.header.width, page.header.height, page.header.pageIndex))
\t\t\t\t\t\treturn false;
\t\t\t\t}
\t\t\t\tUInt64 payloadHash = page.placements.empty() ? 1469598103934665603ull
''')

for temporary in (
    Path('.github/workflows/tnvse-atlas-placement-cache-patch.yml'),
    Path('.github/workflows/apply-atlas-placement-cache-v2.yml'),
    Path('.github/scripts/apply_atlas_placement_cache_patch.py'),
):
    if temporary.exists():
        temporary.unlink()

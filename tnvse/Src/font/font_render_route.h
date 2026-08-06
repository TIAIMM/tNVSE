#pragma once

#include <cstdint>

namespace fonthook::vectorfont
{
	enum class FontAtlasRoute : std::uint8_t
	{
		ShaderDistanceField,
		ShaderA8Coverage,
		ArgbFallback
	};

	enum class PersistentFontCacheDomain : std::uint8_t
	{
		DistanceField,
		CpuCoverage
	};

	constexpr FontAtlasRoute ResolveFontAtlasRoute(
		bool shaderLoaderRouteAvailable, bool bakedEffectMode)
	{
		if (!shaderLoaderRouteAvailable)
			return FontAtlasRoute::ArgbFallback;
		return bakedEffectMode
			? FontAtlasRoute::ShaderA8Coverage
			: FontAtlasRoute::ShaderDistanceField;
	}

	constexpr PersistentFontCacheDomain ResolvePersistentFontCacheDomain(
		FontAtlasRoute route)
	{
		return route == FontAtlasRoute::ShaderDistanceField
			? PersistentFontCacheDomain::DistanceField
			: PersistentFontCacheDomain::CpuCoverage;
	}

	static_assert(ResolveFontAtlasRoute(false, false)
		== FontAtlasRoute::ArgbFallback);
	static_assert(ResolveFontAtlasRoute(false, true)
		== FontAtlasRoute::ArgbFallback);
	static_assert(ResolveFontAtlasRoute(true, false)
		== FontAtlasRoute::ShaderDistanceField);
	static_assert(ResolveFontAtlasRoute(true, true)
		== FontAtlasRoute::ShaderA8Coverage);
	static_assert(ResolvePersistentFontCacheDomain(
		FontAtlasRoute::ShaderDistanceField)
		== PersistentFontCacheDomain::DistanceField);
	static_assert(ResolvePersistentFontCacheDomain(
		FontAtlasRoute::ArgbFallback)
		== PersistentFontCacheDomain::CpuCoverage);
}

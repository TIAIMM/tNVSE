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

	constexpr FontAtlasRoute ResolveFontAtlasRoute(
		bool shaderLoaderRouteAvailable, bool aggressivePerformanceMode)
	{
		if (!shaderLoaderRouteAvailable)
			return FontAtlasRoute::ArgbFallback;
		return aggressivePerformanceMode
			? FontAtlasRoute::ShaderA8Coverage
			: FontAtlasRoute::ShaderDistanceField;
	}

	static_assert(ResolveFontAtlasRoute(false, false)
		== FontAtlasRoute::ArgbFallback);
	static_assert(ResolveFontAtlasRoute(false, true)
		== FontAtlasRoute::ArgbFallback);
	static_assert(ResolveFontAtlasRoute(true, true)
		== FontAtlasRoute::ShaderA8Coverage);
}

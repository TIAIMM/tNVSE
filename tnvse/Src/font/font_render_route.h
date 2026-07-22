#pragma once

#include <cstdint>

namespace fonthook::vectorfont
{
	enum class FontAtlasRoute : std::uint8_t
	{
		ShaderMtsdf,
		ArgbFallback
	};

	constexpr FontAtlasRoute ResolveFontAtlasRoute(bool shaderLoaderRouteAvailable)
	{
		return shaderLoaderRouteAvailable
			? FontAtlasRoute::ShaderMtsdf
			: FontAtlasRoute::ArgbFallback;
	}
}

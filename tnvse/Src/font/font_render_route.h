#pragma once

#include <cstdint>

namespace fonthook::vectorfont
{
	enum class FontAtlasRoute : std::uint8_t
	{
		ShaderSdf,
		ArgbFallback
	};

	constexpr FontAtlasRoute ResolveFontAtlasRoute(bool shaderLoaderRouteAvailable)
	{
		return shaderLoaderRouteAvailable
			? FontAtlasRoute::ShaderSdf
			: FontAtlasRoute::ArgbFallback;
	}
}

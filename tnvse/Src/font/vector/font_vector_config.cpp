#include "font_vector_internal.h"

#include "load_config.h"

#include <pugixml/pugixml.hpp>

#include <hb.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <cwctype>

namespace fonthook::vectorfont
{
	std::unordered_map<UInt32, FontConfig> g_configs;

	namespace
	{

		std::wstring GetGameDirectory()
		{
			std::array<wchar_t, MAX_PATH> path = {};
			const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
			if (!length || length >= path.size())
				return {};

			std::wstring result(path.data(), length);
			const size_t slash = result.find_last_of(L"\\/");
			return slash == std::wstring::npos ? std::wstring() : result.substr(0, slash);
		}

		std::wstring Utf8ToWide(const char* value)
		{
			if (!value || !*value)
				return {};
			const int required = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value, -1, nullptr, 0);
			if (required <= 1)
				return {};
			std::wstring result(static_cast<size_t>(required), L'\0');
			MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value, -1, result.data(), required);
			result.pop_back();
			return result;
		}

		bool IsAbsolutePath(const std::wstring& path)
		{
			return (path.size() >= 2 && path[1] == L':')
				|| (path.size() >= 2 && path[0] == L'\\' && path[1] == L'\\');
		}

		std::wstring NormalizeConfiguredFontPath(const char* value)
		{
			std::wstring path = Utf8ToWide(value);
			std::replace(path.begin(), path.end(), L'/', L'\\');
			return path;
		}

		std::wstring ResolveFontPath(const std::wstring& path)
		{
			if (path.empty() || IsAbsolutePath(path))
				return path;
			const std::wstring gameDirectory = GetGameDirectory();
			return gameDirectory.empty() ? path : gameDirectory + L"\\" + path;
		}

		bool ReadFaceConfig(pugi::xml_node node, FaceConfig& face)
		{
			face = {};
			face.configuredPath = NormalizeConfiguredFontPath(
				node.attribute("path").as_string());
			face.path = ResolveFontPath(face.configuredPath);
			face.faceIndex = node.attribute("index").as_int(0);
			return !face.path.empty() && face.faceIndex >= 0;
		}

		void ReadStyleAttributes(pugi::xml_node node, ByteStyle& style)
		{
			if (node.attribute("pixelSize"))
				style.pixelSize = node.attribute("pixelSize").as_float(style.pixelSize);
			if (node.attribute("tracking"))
				style.tracking = node.attribute("tracking").as_float(style.tracking);
			if (node.attribute("scaleX"))
				style.scaleX = node.attribute("scaleX").as_float(style.scaleX);
			if (node.attribute("scaleY"))
				style.scaleY = node.attribute("scaleY").as_float(style.scaleY);
			if (node.attribute("embolden"))
				style.embolden = node.attribute("embolden").as_float(style.embolden);
			if (node.attribute("slant"))
				style.slantDegrees = node.attribute("slant").as_float(style.slantDegrees);
			if (node.attribute("baselineOffset"))
				style.baselineOffset = node.attribute("baselineOffset").as_float(style.baselineOffset);
			if (node.attribute("fixedWidth"))
				style.fixedWidth = node.attribute("fixedWidth").as_float(style.fixedWidth);
		}

		bool IsValidStyle(const ByteStyle& style)
		{
			return std::isfinite(style.pixelSize) && style.pixelSize > 0.0f
				&& std::isfinite(style.tracking)
				&& std::isfinite(style.scaleX) && style.scaleX > 0.0f
				&& std::isfinite(style.scaleY) && style.scaleY > 0.0f
				&& std::isfinite(style.embolden) && style.embolden >= 0.0f
				&& std::isfinite(style.slantDegrees)
				&& std::isfinite(style.baselineOffset)
				&& std::isfinite(style.fixedWidth) && style.fixedWidth >= 0.0f;
		}

		bool ReadFaceChain(pugi::xml_node node, std::vector<FaceConfig>& faces)
		{
			faces.clear();
			pugi::xml_node primary = node.child("face");
			if (!primary)
				return false;

			FaceConfig face;
			if (!ReadFaceConfig(primary, face))
				return false;
			faces.push_back(std::move(face));

			for (pugi::xml_node fallback : node.children("fallback"))
			{
				FaceConfig fallbackFace;
				if (ReadFaceConfig(fallback, fallbackFace))
					faces.push_back(std::move(fallbackFace));
			}
			return true;
		}

		bool HasFaceChildren(pugi::xml_node node)
		{
			return node.child("face") || node.child("fallback");
		}

		bool ParseHexColor(const char* value, NiColorA& color)
		{
			if (!value || value[0] != '#' || std::strlen(value) != 7)
				return false;
			char* end = nullptr;
			const unsigned long rgb = std::strtoul(value + 1, &end, 16);
			if (!end || *end)
				return false;
			color.r = static_cast<float>((rgb >> 16) & 0xFF) / 255.0f;
			color.g = static_cast<float>((rgb >> 8) & 0xFF) / 255.0f;
			color.b = static_cast<float>(rgb & 0xFF) / 255.0f;
			return true;
		}

		enum class EffectKind
		{
			Glow,
			Outline,
			Shadow,
		};

		const char* EffectColorModeName(EffectColorMode mode)
		{
			return mode == EffectColorMode::Fill ? "fill" : "fixed";
		}

		bool ReadEffect(pugi::xml_node node, EffectKind kind, EffectStyle& result,
			std::string& reason)
		{
			result = {};
			if (!node)
				return true;
			result.enabled = node.attribute("enabled").as_bool(true);
			const std::string colorMode = node.attribute("colorMode").as_string("fixed");
			if (colorMode == "fixed")
				result.colorMode = EffectColorMode::Fixed;
			else if (colorMode == "fill")
				result.colorMode = EffectColorMode::Fill;
			else
			{
				reason = "effect colorMode must be fixed or fill";
				return false;
			}
			const float alpha = node.attribute("alpha").as_float(1.0f);
			if (!std::isfinite(alpha))
			{
				reason = "effect alpha must be finite";
				return false;
			}
			result.color.a = std::clamp(alpha, 0.0f, 1.0f);
			if (!ParseHexColor(node.attribute("color").as_string("#000000"), result.color))
			{
				reason = "effect color must be #RRGGBB";
				return false;
			}

			if (kind == EffectKind::Glow)
			{
				const bool hasOuter = node.attribute("outer");
				const bool hasWidth = node.attribute("width");
				if (hasOuter && hasWidth)
				{
					reason = "glow cannot specify both outer and legacy width";
					return false;
				}
				result.inner = node.attribute("inner").as_float(0.0f);
				result.outer = hasOuter
					? node.attribute("outer").as_float(0.0f)
					: node.attribute("width").as_float(0.0f);
				result.width = result.outer;
				result.power = node.attribute("power").as_float(2.0f);
				if (!std::isfinite(result.inner) || result.inner < 0.0f
					|| !std::isfinite(result.outer) || result.outer < 0.0f
					|| !std::isfinite(result.power) || result.power <= 0.0f)
				{
					reason = "glow inner/outer must be finite and non-negative, and power must be positive";
					return false;
				}
				if (result.enabled && result.outer <= result.inner)
				{
					reason = "enabled glow outer must be greater than inner";
					return false;
				}
				result.enabled = result.enabled && result.color.a > 0.0f;
				return true;
			}

			if (kind == EffectKind::Outline)
			{
				result.width = node.attribute("width").as_float(0.0f);
				result.softness = node.attribute("softness").as_float(0.5f);
				if (!std::isfinite(result.width) || result.width < 0.0f
					|| !std::isfinite(result.softness) || result.softness < 0.0f)
				{
					reason = "outline width and softness must be finite and non-negative";
					return false;
				}
				if (result.enabled && result.width <= 0.0f)
				{
					reason = "enabled outline width must be greater than zero";
					return false;
				}
				result.enabled = result.enabled && result.color.a > 0.0f;
				return true;
			}

			if (kind == EffectKind::Shadow)
			{
				result.includeGlow = node.attribute("includeGlow").as_bool(false);
				result.includeOutline = node.attribute("includeOutline").as_bool(false);
				result.x = node.attribute("x").as_float(0.0f);
				result.y = node.attribute("y").as_float(0.0f);
				result.blur = node.attribute("blur").as_float(0.0f);
				result.power = node.attribute("power").as_float(2.0f);
				if (!std::isfinite(result.x) || !std::isfinite(result.y)
					|| !std::isfinite(result.blur) || result.blur < 0.0f
					|| !std::isfinite(result.power) || result.power <= 0.0f)
				{
					reason = "shadow offsets/blur must be finite, blur non-negative, and power positive";
					return false;
				}
				result.enabled = result.enabled && result.color.a > 0.0f;
				return true;
			}

			return false;
		}

		bool ReadFontColor(pugi::xml_node node, FontColorStyle& style)
		{
			pugi::xml_attribute colorAttribute = node.attribute("fontColor");
			if (!colorAttribute)
				return true;
			if (!ParseHexColor(colorAttribute.as_string(), style.color))
				return false;
			const float alpha = node.attribute("fontAlpha").as_float(1.0f);
			if (!std::isfinite(alpha))
				return false;
			style.color.a = std::clamp(alpha, 0.0f, 1.0f);
			style.configured = true;
			return true;
		}

		void HashBytes(UInt64& hash, const void* data, size_t size)
		{
			const UInt8* bytes = static_cast<const UInt8*>(data);
			for (size_t i = 0; i < size; ++i)
			{
				hash ^= bytes[i];
				hash *= 1099511628211ull;
			}
		}

		void HashFaceStyles(UInt64& hash, const FontConfig& config,
			bool includeLayoutFields)
		{
			for (const ByteStyle& style : config.styles)
			{
				HashBytes(hash, &style.pixelSize, sizeof(style.pixelSize));
				HashBytes(hash, &style.scaleX, sizeof(style.scaleX));
				HashBytes(hash, &style.scaleY, sizeof(style.scaleY));
				HashBytes(hash, &style.embolden, sizeof(style.embolden));
				HashBytes(hash, &style.slantDegrees, sizeof(style.slantDegrees));
				if (includeLayoutFields)
				{
					HashBytes(hash, &style.tracking, sizeof(style.tracking));
					HashBytes(hash, &style.baselineOffset, sizeof(style.baselineOffset));
					HashBytes(hash, &style.fixedWidth, sizeof(style.fixedWidth));
				}
				const UInt32 faceCount = static_cast<UInt32>(style.faces.size());
				HashBytes(hash, &faceCount, sizeof(faceCount));
				for (const FaceConfig& face : style.faces)
				{
					// Do not hash the game-directory-expanded filesystem path. The
					// normalized XML identity keeps relative Data paths portable, while
					// runtime disk identities additionally hash every loaded font's bytes.
					const UInt32 pathLength = static_cast<UInt32>(face.configuredPath.size());
					HashBytes(hash, &pathLength, sizeof(pathLength));
					HashBytes(hash, face.configuredPath.data(),
						face.configuredPath.size() * sizeof(wchar_t));
					HashBytes(hash, &face.faceIndex, sizeof(face.faceIndex));
				}
			}
		}

		UInt64 BuildLayoutHash(const FontConfig& config)
		{
			// This is a content hash, not a runtime-font identity. Callers that require
			// isolation carry fontId separately; excluding it lets identical font nodes
			// share persistent manifests across Gamebryo font IDs.
			UInt64 hash = 1469598103934665603ull;
			HashBytes(hash, &config.verticalMetrics, sizeof(config.verticalMetrics));
			HashBytes(hash, &config.shaping, sizeof(config.shaping));
			HashBytes(hash, &config.unicodeLineBreaking, sizeof(config.unicodeLineBreaking));
			for (const std::string& feature : config.shapingFeatures)
			{
				const size_t featureLength = feature.size();
				HashBytes(hash, &featureLength, sizeof(featureLength));
				HashBytes(hash, feature.data(), feature.size());
			}
			HashBytes(hash, &config.baseline, sizeof(config.baseline));
			HashFaceStyles(hash, config, true);
			return hash;
		}

		UInt64 BuildMaskGenerationHash(const FontConfig& config)
		{
			// Bitmap/atlas content is independent of the Gamebryo font slot. Atlas cache
			// keys still include fontId in memory, while disk snapshots use this content
			// identity together with the resolved font-file hashes.
			UInt64 hash = 1469598103934665603ull;
			HashFaceStyles(hash, config, false);
			return hash;
		}

		UInt64 BuildShaderEffectHash(const FontConfig& config)
		{
			UInt64 hash = 1469598103934665603ull;
			HashBytes(hash, &config.fontColor.configured, sizeof(config.fontColor.configured));
			HashBytes(hash, &config.fontColor.color, sizeof(config.fontColor.color));
			HashBytes(hash, &config.fillRenderMode, sizeof(config.fillRenderMode));
			HashBytes(hash, &config.effectQuality, sizeof(config.effectQuality));
			auto hashEffect = [&](const EffectStyle& effect)
			{
				HashBytes(hash, &effect.enabled, sizeof(effect.enabled));
				HashBytes(hash, &effect.includeGlow, sizeof(effect.includeGlow));
				HashBytes(hash, &effect.includeOutline, sizeof(effect.includeOutline));
				HashBytes(hash, &effect.width, sizeof(effect.width));
				HashBytes(hash, &effect.blur, sizeof(effect.blur));
				HashBytes(hash, &effect.inner, sizeof(effect.inner));
				HashBytes(hash, &effect.outer, sizeof(effect.outer));
				HashBytes(hash, &effect.power, sizeof(effect.power));
				HashBytes(hash, &effect.softness, sizeof(effect.softness));
				HashBytes(hash, &effect.x, sizeof(effect.x));
				HashBytes(hash, &effect.y, sizeof(effect.y));
				HashBytes(hash, &effect.color, sizeof(effect.color));
				HashBytes(hash, &effect.colorMode, sizeof(effect.colorMode));
			};
			hashEffect(config.glow);
			hashEffect(config.outline);
			hashEffect(config.shadow);
			return hash;
		}

		bool ParseFontNode(pugi::xml_node node, FontConfig& config, std::string& reason)
		{
			config = {};
			reason.clear();
			config.fontId = node.attribute("id").as_uint(0);
			if (!config.fontId)
			{
				reason = "missing or zero id";
				return false;
			}
			const std::string prewarm = node.attribute("prewarm").as_string("none");
			if (prewarm == "none")
				config.prewarm = FontPrewarmMode::None;
			else if (prewarm == "common")
				config.prewarm = FontPrewarmMode::Common;
			else if (prewarm == "codepage")
				config.prewarm = FontPrewarmMode::CodePage;
			else
			{
				reason = "prewarm must be none, common, or codepage";
				return false;
			}

			const std::string verticalMetrics = node.attribute("verticalMetrics").as_string("freetype");
			if (verticalMetrics == "freetype")
				config.verticalMetrics = VerticalMetricsMode::FreeType;
			else if (verticalMetrics == "original")
				config.verticalMetrics = VerticalMetricsMode::Original;
			else
			{
				reason = "verticalMetrics must be freetype or original";
				return false;
			}

			const std::string effectQuality = node.attribute("effectQuality").as_string("balanced");
			const std::string fillRenderMode = node.attribute("fillRenderMode").as_string("grayscale");
			if (fillRenderMode == "grayscale")
				config.fillRenderMode = FillRenderMode::Grayscale;
			else if (fillRenderMode == "sdf")
				config.fillRenderMode = FillRenderMode::Sdf;
			else
			{
				reason = "fillRenderMode must be grayscale or sdf";
				return false;
			}

			if (effectQuality == "fast")
				config.effectQuality = EffectQuality::Fast;
			else if (effectQuality == "balanced")
				config.effectQuality = EffectQuality::Balanced;
			else if (effectQuality == "high")
				config.effectQuality = EffectQuality::High;
			else
			{
				reason = "effectQuality must be fast, balanced, or high";
				return false;
			}

			ByteStyle defaults;
			ReadStyleAttributes(node, defaults);
			std::vector<FaceConfig> defaultFaces;
			ReadFaceChain(node, defaultFaces);

			const char* roleNames[] = { "singleByte", "doubleByte" };
			for (size_t i = 0; i < config.styles.size(); ++i)
			{
				ByteStyle style = defaults;
				style.faces = defaultFaces;
				pugi::xml_node role = node.child(roleNames[i]);
				if (role)
				{
					ReadStyleAttributes(role, style);
					if (HasFaceChildren(role) && !ReadFaceChain(role, style.faces))
					{
						reason = std::string(roleNames[i]) + " requires a valid <face path=... index=...>";
						return false;
					}
				}
				if (!IsValidStyle(style))
				{
					reason = std::string(roleNames[i]) + " has invalid style values";
					return false;
				}
				if (style.faces.empty())
				{
					reason = std::string(roleNames[i]) + " has no face chain";
					return false;
				}
				style.embolden = std::max(0.0f, style.embolden);
				style.slantDegrees = std::clamp(style.slantDegrees, -45.0f, 45.0f);
				config.styles[i] = std::move(style);
			}

			config.baseline = node.attribute("baseline").as_float(0.0f);
			if (!std::isfinite(config.baseline) || config.baseline < 0.0f)
			{
				reason = "baseline must be finite and zero or greater";
				return false;
			}
			config.shaping = node.attribute("shaping").as_bool(false);
			config.unicodeLineBreaking = node.attribute("unicodeLineBreaking").as_bool(false);
			const std::string features = node.attribute("features").as_string();
			if (!features.empty())
			{
				if (!config.shaping)
				{
					reason = "features requires shaping=1";
					return false;
				}
				size_t begin = 0;
				while (begin < features.size())
				{
					const size_t comma = features.find(',', begin);
					const size_t end = comma == std::string::npos ? features.size() : comma;
					size_t first = begin;
					size_t last = end;
					while (first < last && std::isspace(static_cast<unsigned char>(features[first])))
						++first;
					while (last > first && std::isspace(static_cast<unsigned char>(features[last - 1])))
						--last;
					if (first == last)
					{
						reason = "features contains an empty token";
						return false;
					}
					std::string token = features.substr(first, last - first);
					hb_feature_t parsed = {};
					if (!hb_feature_from_string(token.data(), static_cast<int>(token.size()), &parsed))
					{
						reason = "invalid HarfBuzz feature: " + token;
						return false;
					}
					config.shapingFeatures.push_back(std::move(token));
					if (comma == std::string::npos)
						break;
					begin = comma + 1;
				}
			}
			if (!ReadFontColor(node, config.fontColor))
			{
				reason = "fontColor must be #RRGGBB and fontAlpha must be finite";
				return false;
			}
			if (!ReadEffect(node.child("glow"), EffectKind::Glow, config.glow, reason)
				|| !ReadEffect(node.child("outline"), EffectKind::Outline, config.outline, reason)
				|| !ReadEffect(node.child("shadow"), EffectKind::Shadow, config.shadow, reason))
				return false;
			config.layoutHash = BuildLayoutHash(config);
			config.maskGenerationHash = BuildMaskGenerationHash(config);
			config.shaderEffectHash = BuildShaderEffectHash(config);
			return true;
		}

		void LogFontConfig(const FontConfig& config)
		{
			if (!g_bEnableFreeTypeFontRenderingLog)
				return;
			FreeTypeFontDebugLog(
				"tnvse_freetype_font: config font id=%u prewarm=%u verticalMetrics=%s shaping=%d unicodeLineBreaking=%d features=%u baseline=%.2f fontColor=%d fillRenderMode=%s effectQuality=%u glow=%d colorMode=%s inner=%.2f outer=%.2f power=%.2f outline=%d colorMode=%s width=%.2f softness=%.2f shadow=%d colorMode=%s blur=%.2f power=%.2f includeGlow=%d includeOutline=%d",
				config.fontId, static_cast<UInt32>(config.prewarm),
				config.verticalMetrics == VerticalMetricsMode::Original ? "original" : "freetype",
				config.shaping ? 1 : 0,
				config.unicodeLineBreaking ? 1 : 0,
				static_cast<UInt32>(config.shapingFeatures.size()),
				config.baseline,
				config.fontColor.configured,
				config.fillRenderMode == FillRenderMode::Sdf ? "sdf" : "grayscale",
				static_cast<UInt32>(config.effectQuality),
				config.glow.enabled, EffectColorModeName(config.glow.colorMode),
				config.glow.inner, config.glow.outer, config.glow.power,
				config.outline.enabled, EffectColorModeName(config.outline.colorMode),
				config.outline.width, config.outline.softness,
				config.shadow.enabled, EffectColorModeName(config.shadow.colorMode),
				config.shadow.blur, config.shadow.power,
				config.shadow.includeGlow, config.shadow.includeOutline);
			FreeTypeFontDebugLog(
				"tnvse_freetype_font:   hashes layout=%016llX mask=%016llX shader=%016llX",
				static_cast<unsigned long long>(config.layoutHash),
				static_cast<unsigned long long>(config.maskGenerationHash),
				static_cast<unsigned long long>(config.shaderEffectHash));
			if (config.fontColor.configured)
			{
				FreeTypeFontDebugLog(
					"tnvse_freetype_font:   font color=(%.3f,%.3f,%.3f,%.3f)",
					config.fontColor.color.r, config.fontColor.color.g,
					config.fontColor.color.b, config.fontColor.color.a);
			}
			const char* roleNames[] = { "singleByte", "doubleByte" };
			for (size_t roleIndex = 0; roleIndex < config.styles.size(); ++roleIndex)
			{
				const ByteStyle& style = config.styles[roleIndex];
				FreeTypeFontDebugLog(
					"tnvse_freetype_font:   %s size=%.2f tracking=%.2f fixedWidth=%.2f scale=(%.3f,%.3f) embolden=%.2f slant=%.2f baseline=%.2f faces=%u",
					roleNames[roleIndex], style.pixelSize, style.tracking,
					style.fixedWidth, style.scaleX, style.scaleY, style.embolden, style.slantDegrees,
					style.baselineOffset, static_cast<UInt32>(style.faces.size()));
				for (size_t faceIndex = 0; faceIndex < style.faces.size(); ++faceIndex)
				{
					const FaceConfig& face = style.faces[faceIndex];
					const DWORD attributes = GetFileAttributesW(face.path.c_str());
					const bool exists = attributes != INVALID_FILE_ATTRIBUTES
						&& !(attributes & FILE_ATTRIBUTE_DIRECTORY);
					FreeTypeFontDebugLog(
						"tnvse_freetype_font:     face[%u] path=%ls index=%ld exists=%d",
						static_cast<UInt32>(faceIndex), face.path.c_str(), face.faceIndex, exists);
				}
			}
		}
	}

	const FontConfig* FindConfig(UInt32 auiFontId)
	{
		auto it = g_configs.find(auiFontId);
		return it == g_configs.end() ? nullptr : &it->second;
	}

	bool UsesSdfFill(const FontConfig& arConfig)
	{
		return arConfig.fillRenderMode == FillRenderMode::Sdf;
	}

	bool HardShadowIncludesGlow(const FontConfig& arConfig)
	{
		return arConfig.shadow.enabled && arConfig.shadow.blur <= 0.001f
			&& arConfig.shadow.includeGlow && arConfig.glow.enabled;
	}

	bool HardShadowIncludesOutline(const FontConfig& arConfig)
	{
		return arConfig.shadow.enabled && arConfig.shadow.blur <= 0.001f
			&& arConfig.shadow.includeOutline && arConfig.outline.enabled;
	}

	bool HasSdfEffects(const FontConfig& arConfig)
	{
		return arConfig.glow.enabled
			|| arConfig.outline.enabled
			|| (arConfig.shadow.enabled && arConfig.shadow.blur > 0.0f)
			|| HardShadowIncludesGlow(arConfig)
			|| HardShadowIncludesOutline(arConfig);
	}

	bool NeedsSdfMask(const FontConfig& arConfig)
	{
		return UsesSdfFill(arConfig) || HasSdfEffects(arConfig);
	}

	bool ResolveSdfSpread(const FontConfig& arConfig, float afRasterScale, UInt32& arSpread,
		bool abIncludeEffects)
	{
		arSpread = 0;
		const bool needsMask = UsesSdfFill(arConfig)
			|| (abIncludeEffects && HasSdfEffects(arConfig));
		if (!needsMask || !std::isfinite(afRasterScale) || afRasterScale <= 0.0f)
			return false;

		float radius = 0.0f;
		if (abIncludeEffects && arConfig.glow.enabled)
			radius = std::max(radius, arConfig.glow.outer);
		if (abIncludeEffects && arConfig.outline.enabled)
			radius = std::max(radius, arConfig.outline.width + arConfig.outline.softness);
		if (abIncludeEffects && arConfig.shadow.enabled && arConfig.shadow.blur > 0.0f)
			radius = std::max(radius, arConfig.shadow.blur);

		float physicalSpread = std::ceil(radius * afRasterScale) + 2.0f;
		if (UsesSdfFill(arConfig))
			physicalSpread = std::max(physicalSpread, 4.0f);
		if (!std::isfinite(physicalSpread) || physicalSpread < 2.0f || physicalSpread > 32.0f)
			return false;
		arSpread = static_cast<UInt32>(physicalSpread);
		return true;
	}
}

namespace fonthook
{
	void LoadFreeTypeFontConfig()
	{
		vectorfont::g_configs.clear();
		if (!g_bEnableFreeTypeFontRendering)
		{
			gLog.FormattedMessage("tnvse_freetype_font: disabled by tnvse.ini");
			return;
		}

		std::array<char, MAX_PATH> modulePath = {};
		GetModuleFileNameA(nullptr, modulePath.data(), static_cast<DWORD>(modulePath.size()));
		char* slash = std::strrchr(modulePath.data(), '\\');
		if (!slash)
			return;
		strcpy_s(slash + 1, modulePath.size() - static_cast<size_t>(slash + 1 - modulePath.data()),
			"Data\\nvse\\plugins\\tnvse_fonts.xml");
		if (g_bEnableFreeTypeFontRenderingLog)
			FreeTypeFontDebugLog("tnvse_freetype_font: loading config path=%s", modulePath.data());

		pugi::xml_document document;
		const pugi::xml_parse_result parsed = document.load_file(modulePath.data());
		if (!parsed)
		{
			gLog.FormattedMessage(
				"tnvse_freetype_font: failed to load config path=%s error=%s offset=%lld",
				modulePath.data(), parsed.description(), static_cast<long long>(parsed.offset));
			return;
		}

		pugi::xml_node root = document.child("tNVSE");
		if (!root)
		{
			gLog.FormattedMessage("tnvse_freetype_font: config root must be <tNVSE>");
			return;
		}
		pugi::xml_node fonts = root.child("fonts");
		if (!fonts)
		{
			gLog.FormattedMessage("tnvse_freetype_font: no <fonts> configuration");
			return;
		}

		UInt32 rejected = 0;
		for (pugi::xml_node node : fonts.children("font"))
		{
			vectorfont::FontConfig config;
			std::string reason;
			if (!vectorfont::ParseFontNode(node, config, reason))
			{
				++rejected;
				gLog.FormattedMessage("tnvse_freetype_font: rejected font id=%u reason=%s",
					node.attribute("id").as_uint(0), reason.c_str());
				continue;
			}
			vectorfont::LogFontConfig(config);
			vectorfont::g_configs[config.fontId] = std::move(config);
		}

		gLog.FormattedMessage("tnvse_freetype_font: loaded %u font configurations, rejected %u",
			static_cast<UInt32>(vectorfont::g_configs.size()), rejected);
	}
}

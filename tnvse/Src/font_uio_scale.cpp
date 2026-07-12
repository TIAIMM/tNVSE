#include "font_engine.h"
#include "font_vector.h"
#include "load_config.h"
#include "tnvse.h"
#include "InterfaceManager.hpp"

#include <Windows.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace
{
	constexpr UInt32 kSupportedUioVersion = 230;
	constexpr float kMinimumUioScale = 0.1f;
	constexpr float kMaximumUioScale = 10.0f;
	constexpr float kMinimumRasterScale = 0.1f;
	constexpr float kMaximumRasterScale = 10.0f;
	constexpr LONG kRasterScalePrecision = 1000;
	constexpr UInt8 kUioReturnSignature[] = { 0x8B, 0x95, 0x50, 0xFF, 0xFF, 0xFF };

	enum UioCompatibilityState : LONG
	{
		kUioUnchecked = 0,
		kUioCompatible = 1,
		kUioIncompatible = 2,
		kUioMissingFinal = 3,
	};

	volatile LONG s_uioState = kUioUnchecked;
	volatile LONG s_failureLogMask = 0;
	volatile LONG s_deviceScaleMilli = 0;
	volatile LONG s_lastCombinedScaleKey = 0;
	volatile LONG s_combinedScaleLogCount = 0;
	thread_local float s_createTextScale = 1.0f;

	enum UioFailureLog : LONG
	{
		kLogMissing = 1 << 0,
		kLogVersion = 1 << 1,
		kLogModule = 1 << 2,
		kLogCaller = 1 << 3,
		kLogFrame = 1 << 4,
		kLogScale = 1 << 5,
		kLogDeviceScaleMissing = 1 << 6,
		kLogDeviceScaleInvalid = 1 << 7,
		kLogDeviceScaleClamped = 1 << 8,
	};

	enum class DeviceScaleReadResult
	{
		Success,
		Missing,
		Invalid,
	};

	void LogFailureOnce(LONG bit, const char* message)
	{
		const LONG previous = InterlockedOr(&s_failureLogMask, bit);
		if (!(previous & bit) && g_bEnableFreeTypeFontRenderingLog)
			fonthook::FreeTypeFontDebugLog("tnvse_freetype_font: %s", message);
	}

	bool QueryUioCompatibility(bool finalize)
	{
		const LONG state = InterlockedCompareExchange(&s_uioState, 0, 0);
		if (state != kUioUnchecked)
			return state == kUioCompatible;

		if (!g_cmdTableInterface || !g_cmdTableInterface->GetPluginInfoByDLLName)
		{
			if (finalize)
				InterlockedCompareExchange(&s_uioState, kUioMissingFinal, kUioUnchecked);
			return false;
		}

		const PluginInfo* info = g_cmdTableInterface->GetPluginInfoByDLLName("ui_organizer.dll");
		if (!info)
		{
			if (finalize)
			{
				InterlockedCompareExchange(&s_uioState, kUioMissingFinal, kUioUnchecked);
				LogFailureOnce(kLogMissing, "UIO is not loaded; effective raster scale is 1.0");
			}
			return false;
		}

		const bool compatible = info->infoVersion == PluginInfo::kInfoVersion
			&& info->version == kSupportedUioVersion;
		InterlockedCompareExchange(&s_uioState,
			compatible ? kUioCompatible : kUioIncompatible, kUioUnchecked);
		if (compatible)
		{
			if (g_bEnableFreeTypeFontRenderingLog)
				fonthook::FreeTypeFontDebugLog("tnvse_freetype_font: UIO version 230 detected; stack scale bridge enabled");
		}
		else
		{
			LogFailureOnce(kLogVersion,
				"UIO plugin version is not 230; stack scale bridge disabled");
		}
		return compatible;
	}

	bool GetModuleRange(HMODULE module, uintptr_t& begin, uintptr_t& end)
	{
		if (!module)
			return false;
		const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(module);
		if (dos->e_magic != IMAGE_DOS_SIGNATURE)
			return false;
		const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(
			reinterpret_cast<const UInt8*>(module) + dos->e_lfanew);
		if (nt->Signature != IMAGE_NT_SIGNATURE || !nt->OptionalHeader.SizeOfImage)
			return false;
		begin = reinterpret_cast<uintptr_t>(module);
		end = begin + nt->OptionalHeader.SizeOfImage;
		return end > begin;
	}

	LONG CanonicalScaleMilli(float scale)
	{
		const float clamped = std::max(kMinimumRasterScale,
			std::min(kMaximumRasterScale, scale));
		return std::max<LONG>(1, static_cast<LONG>(std::lround(
			clamped * static_cast<float>(kRasterScalePrecision))));
	}

	DeviceScaleReadResult ReadDevicePixelScale(
		float& resolutionConverter, float& deviceScale, bool& clamped)
	{
		resolutionConverter = 0.0f;
		deviceScale = 1.0f;
		clamped = false;

		InterfaceManager* manager = InterfaceManager::GetSingleton();
		if (!manager || !manager->pMenuRoot)
			return DeviceScaleReadResult::Missing;

		Tile::Value* value = manager->pMenuRoot->GetValue(
			Tile::kTileValue_resolutionconverter);
		if (!value)
			return DeviceScaleReadResult::Missing;

		resolutionConverter = value->fNum;
		if (!std::isfinite(resolutionConverter) || resolutionConverter <= 0.0f)
			return DeviceScaleReadResult::Invalid;

		const float rawScale = 1.0f / resolutionConverter;
		if (!std::isfinite(rawScale) || rawScale <= 0.0f)
			return DeviceScaleReadResult::Invalid;

		deviceScale = std::max(kMinimumRasterScale,
			std::min(kMaximumRasterScale, rawScale));
		clamped = std::fabs(deviceScale - rawScale) > 0.0001f;
		return DeviceScaleReadResult::Success;
	}

	void RefreshDevicePixelScale()
	{
		if (!g_bEnableFreeTypeDevicePixelScale)
		{
			InterlockedExchange(&s_deviceScaleMilli, kRasterScalePrecision);
			return;
		}

		float resolutionConverter = 0.0f;
		float deviceScale = 1.0f;
		bool clamped = false;
		const DeviceScaleReadResult result = ReadDevicePixelScale(
			resolutionConverter, deviceScale, clamped);
		if (result != DeviceScaleReadResult::Success)
		{
			InterlockedExchange(&s_deviceScaleMilli, 0);
			LogFailureOnce(result == DeviceScaleReadResult::Missing
				? kLogDeviceScaleMissing : kLogDeviceScaleInvalid,
				result == DeviceScaleReadResult::Missing
					? "screen resolutionconverter is not available; device scale uses 1.0 and prewarm is deferred"
					: "screen resolutionconverter is invalid; device scale uses 1.0 and prewarm is deferred");
			return;
		}

		if (clamped)
			LogFailureOnce(kLogDeviceScaleClamped,
				"device pixel scale was clamped to the supported 0.1-10.0 range");

		const LONG canonical = CanonicalScaleMilli(deviceScale);
		const LONG previous = InterlockedExchange(&s_deviceScaleMilli, canonical);
		if (g_bEnableFreeTypeFontRenderingLog && previous != canonical)
		{
			fonthook::FreeTypeFontDebugLog(
				"tnvse_freetype_font: device pixel scale resolutionconverter=%.6f raw=%.6f canonical=%.3f",
				resolutionConverter, 1.0f / resolutionConverter,
				static_cast<float>(canonical) / kRasterScalePrecision);
		}
	}
}

extern "C" void __cdecl tnvse_SetFreeTypeCreateTextScale(
	const void* callerFrame, const void* returnAddress)
{
	s_createTextScale = 1.0f;
	if (!QueryUioCompatibility(false))
		return;

	HMODULE module = GetModuleHandleA("ui_organizer.dll");
	uintptr_t moduleBegin = 0;
	uintptr_t moduleEnd = 0;
	if (!GetModuleRange(module, moduleBegin, moduleEnd))
	{
		LogFailureOnce(kLogModule, "UIO module range validation failed");
		return;
	}

	const uintptr_t caller = reinterpret_cast<uintptr_t>(returnAddress);
	if (caller < moduleBegin || caller + sizeof(kUioReturnSignature) > moduleEnd)
		return;
	if (std::memcmp(returnAddress, kUioReturnSignature, sizeof(kUioReturnSignature)) != 0)
	{
		LogFailureOnce(kLogCaller, "UIO CreateText return signature does not match version 2.30");
		return;
	}

	__try
	{
		const UInt8* frame = static_cast<const UInt8*>(callerFrame);
		const bool scaled = *reinterpret_cast<const UInt8*>(frame - 0x29) != 0;
		const float scale = *reinterpret_cast<const float*>(frame - 0x1C);
		if (scaled && std::isfinite(scale)
			&& scale >= kMinimumUioScale && scale <= kMaximumUioScale)
		{
			s_createTextScale = scale;
			if (std::fabs(scale - 1.0f) > 0.0001f)
			{
				const LONG previous = InterlockedOr(&s_failureLogMask, kLogScale);
				if (!(previous & kLogScale) && g_bEnableFreeTypeFontRenderingLog)
					fonthook::FreeTypeFontDebugLog(
						"tnvse_freetype_font: first UIO effective raster scale=%.3f", scale);
			}
		}
		else if (scaled)
		{
			LogFailureOnce(kLogFrame, "UIO stack scale is invalid; using 1.0");
		}
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		LogFailureOnce(kLogFrame, "UIO stack frame is unreadable; using 1.0");
	}
}

extern "C" UInt32 __fastcall tnvse_FreeTypeCreateTextDispatch(
	fonthook::FontEx* font, void*, BSStringT<char>* text, int* width, int* height,
	int lineStart, int lineEnd, int flags, char lineBreak,
	const NiColorA* color, NiTriShape** textShape, NiTriShape** iconShape)
{
	return font->CreateText(text, width, height, lineStart, lineEnd, flags,
		lineBreak, color, textShape, iconShape);
}

namespace fonthook
{
	void FinalizeFreeTypeUioDetection()
	{
		QueryUioCompatibility(true);
		RefreshDevicePixelScale();
	}

	void UpdateFreeTypeDevicePixelScale()
	{
		RefreshDevicePixelScale();
	}

	bool TryGetFreeTypeDevicePixelScale(float& scale)
	{
		if (!g_bEnableFreeTypeDevicePixelScale)
		{
			scale = 1.0f;
			return true;
		}

		LONG milli = InterlockedCompareExchange(&s_deviceScaleMilli, 0, 0);
		if (!milli)
		{
			RefreshDevicePixelScale();
			milli = InterlockedCompareExchange(&s_deviceScaleMilli, 0, 0);
		}
		if (!milli)
		{
			scale = 1.0f;
			return false;
		}

		scale = static_cast<float>(milli) / kRasterScalePrecision;
		return true;
	}

	float ResolveFreeTypeRasterScale(float localScale)
	{
		if (!std::isfinite(localScale)
			|| localScale < kMinimumRasterScale || localScale > kMaximumRasterScale)
		{
			localScale = 1.0f;
		}

		float deviceScale = 1.0f;
		TryGetFreeTypeDevicePixelScale(deviceScale);
		const float rawCombined = deviceScale * localScale;
		const LONG finalMilli = CanonicalScaleMilli(rawCombined);
		const LONG localMilli = CanonicalScaleMilli(localScale);
		const LONG logKey = localMilli * 10001 + finalMilli;
		const LONG previousKey = InterlockedExchange(&s_lastCombinedScaleKey, logKey);
		if (g_bEnableFreeTypeFontRenderingLog && previousKey != logKey
			&& InterlockedIncrement(&s_combinedScaleLogCount) <= 64)
		{
			FreeTypeFontDebugLog(
				"tnvse_freetype_font: raster scale device=%.3f uio=%.3f combined=%.6f canonical=%.3f%s",
				deviceScale, localScale, rawCombined,
				static_cast<float>(finalMilli) / kRasterScalePrecision,
				std::fabs(rawCombined - static_cast<float>(finalMilli)
					/ kRasterScalePrecision) > 0.0005f ? " clamped-or-rounded" : "");
		}
		return static_cast<float>(finalMilli) / kRasterScalePrecision;
	}

	float ConsumeFreeTypeCreateTextScale()
	{
		const float localScale = s_createTextScale;
		s_createTextScale = 1.0f;
		return ResolveFreeTypeRasterScale(localScale);
	}

	__declspec(naked) void FreeTypeCreateTextEntryHook()
	{
		__asm
		{
			pushfd
			pushad
			mov eax, [esp + 36]
			push eax
			push ebp
			call tnvse_SetFreeTypeCreateTextScale
			add esp, 8
			popad
			popfd
			jmp tnvse_FreeTypeCreateTextDispatch
		}
	}
}

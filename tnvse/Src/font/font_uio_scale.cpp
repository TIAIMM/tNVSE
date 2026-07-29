#include "font_engine.h"
#include "font_vector.h"
#include "load_config.h"
#include "tnvse.h"

#include <Windows.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace fonthook::implementation::font_uio_scale {}
using namespace fonthook::implementation::font_uio_scale;

namespace fonthook::implementation::font_uio_scale
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
	};

	void LogFailureOnce(LONG bit, const char* message)
	{
		if (!g_bEnableFreeTypeFontRenderingLog)
			return;
		const LONG previous = InterlockedOr(&s_failureLogMask, bit);
		if (!(previous & bit))
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
				LogFailureOnce(kLogMissing,
					"UIO is not loaded; configured FreeType source scale remains active");
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
				fonthook::FreeTypeFontDebugLog(
					"tnvse_freetype_font: UIO version 230 detected; stack scale diagnostics enabled");
		}
		else
		{
			LogFailureOnce(kLogVersion,
				"UIO plugin version is not 230; stack scale integration disabled");
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
		if (!std::isfinite(scale))
			scale = 1.0f;
		const float clamped = std::max(kMinimumRasterScale,
			std::min(kMaximumRasterScale, scale));
		return std::max<LONG>(1, static_cast<LONG>(std::lround(
			clamped * static_cast<float>(kRasterScalePrecision))));
	}

	float CanonicalConfiguredRasterScale()
	{
		return static_cast<float>(CanonicalScaleMilli(
			g_fFreeTypeFontResolutionScale)) / kRasterScalePrecision;
	}
}

extern "C" void __cdecl tnvse_SetFreeTypeCreateTextScale(
	const void* callerFrame, const void* returnAddress)
{
	s_createTextScale = 1.0f;
	// UIO zoom intentionally remains a scene transform. Its stack scale is only
	// used by diagnostics, so avoid PE/frame inspection in normal rendering.
	if (!g_bEnableFreeTypeFontRenderingLog)
		return;
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
	}

	float GetCanonicalFreeTypeRasterScale()
	{
		return CanonicalConfiguredRasterScale();
	}

	static float ResolveConfiguredRasterScale(float localScale)
	{
		const float sourceScale = GetCanonicalFreeTypeRasterScale();
		const LONG sourceMilli = CanonicalScaleMilli(sourceScale);
		if (g_bEnableFreeTypeFontRenderingLog)
		{
			if (!std::isfinite(localScale)
				|| localScale < kMinimumRasterScale || localScale > kMaximumRasterScale)
			{
				localScale = 1.0f;
			}
			const LONG localMilli = CanonicalScaleMilli(localScale);
			const LONG logKey = localMilli * 10001 + sourceMilli;
			const LONG previousKey = InterlockedExchange(&s_lastCombinedScaleKey, logKey);
			if (previousKey != logKey
				&& InterlockedIncrement(&s_combinedScaleLogCount) <= 64)
			{
				FreeTypeFontDebugLog(
					"tnvse_freetype_font: raster scale base=%.3f uio=%.3f source=%.3f policy=%s",
					sourceScale, localScale,
					static_cast<float>(sourceMilli) / kRasterScalePrecision,
					"fixed-configured-uio-1");
			}
		}
		return static_cast<float>(sourceMilli) / kRasterScalePrecision;
	}

	float ConsumeFreeTypeCreateTextScale()
	{
		const float localScale = s_createTextScale;
		s_createTextScale = 1.0f;
		return ResolveConfiguredRasterScale(localScale);
	}

	__declspec(naked) void FreeTypeCreateTextEntryHook()
	{
		__asm
		{
			cmp byte ptr [g_bEnableFreeTypeFontRenderingLog], 0
			je dispatch
			pushfd
			pushad
			mov eax, [esp + 36]
			push eax
			push ebp
			call tnvse_SetFreeTypeCreateTextScale
			add esp, 8
			popad
			popfd
		dispatch:
			jmp tnvse_FreeTypeCreateTextDispatch
		}
	}
}

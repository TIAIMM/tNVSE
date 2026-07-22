#include "font_vector_internal.h"
#include "font_render_route.h"
#include "font_atlas_stream.h"

#include "encoding.h"
#include "load_config.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <deque>
#include <limits>
#include <mutex>
#include <new>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace fonthook::vectorfont
{
	namespace
	{
		constexpr UInt32 kMaximumCandidatesPerBatch = 32768;
		constexpr UInt32 kMaximumGlyphsPerBatch = 4096;
		constexpr size_t kMaximumPrewarmBatchBytes = 24u * 1024u * 1024u;
		constexpr UINT kPrewarmProgressRefreshMessage = WM_APP + 0x531;
		constexpr int kPrewarmProgressWidth = 560;
		constexpr int kPrewarmProgressHeight = 176;

		struct PrewarmProgressState
		{
			std::wstring detail;
			std::wstring stage;
			float progress = 0.0f;
			HWND owner = nullptr;
		};

		std::mutex s_progressMutex;
		PrewarmProgressState s_progressState;
		std::atomic<HWND> s_progressWindow{ nullptr };
		HANDLE s_progressThread = nullptr;
		HANDLE s_progressReadyEvent = nullptr;

		bool IsCurrentProcessForeground()
		{
			HWND foreground = GetForegroundWindow();
			DWORD processId = 0;
			if (foreground)
				GetWindowThreadProcessId(foreground, &processId);
			return processId == GetCurrentProcessId();
		}

		void DrawPrewarmProgress(HWND window)
		{
			PAINTSTRUCT paint = {};
			HDC device = BeginPaint(window, &paint);
			if (!device)
				return;
			RECT client = {};
			GetClientRect(window, &client);
			HBRUSH background = CreateSolidBrush(RGB(20, 22, 25));
			FillRect(device, &client, background);
			DeleteObject(background);

			PrewarmProgressState state;
			{
				std::lock_guard<std::mutex> lock(s_progressMutex);
				state = s_progressState;
			}
			state.progress = std::clamp(state.progress, 0.0f, 1.0f);
			SetBkMode(device, TRANSPARENT);
			SetTextColor(device, RGB(244, 244, 244));
			HFONT titleFont = CreateFontW(24, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE,
				FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
				CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
			HFONT bodyFont = CreateFontW(17, 0, 0, 0, FW_NORMAL, FALSE, FALSE,
				FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
				CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
			HGDIOBJ oldFont = SelectObject(device, titleFont);
			RECT titleRect = { 28, 18, client.right - 28, 50 };
			DrawTextW(device, L"Preparing font cache", -1, &titleRect,
				DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
			SelectObject(device, bodyFont);
			RECT detailRect = { 28, 52, client.right - 28, 78 };
			DrawTextW(device, state.detail.c_str(), -1, &detailRect,
				DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);

			const RECT bar = { 28, 88, client.right - 28, 112 };
			HBRUSH barBackground = CreateSolidBrush(RGB(48, 52, 58));
			FillRect(device, &bar, barBackground);
			DeleteObject(barBackground);
			RECT fill = bar;
			fill.right = fill.left + static_cast<LONG>(
				std::lround((bar.right - bar.left) * state.progress));
			if (fill.right > fill.left)
			{
				HBRUSH fillBrush = CreateSolidBrush(RGB(214, 151, 45));
				FillRect(device, &fill, fillBrush);
				DeleteObject(fillBrush);
			}
			HPEN borderPen = CreatePen(PS_SOLID, 1, RGB(125, 128, 132));
			HGDIOBJ oldPen = SelectObject(device, borderPen);
			HGDIOBJ oldBrush = SelectObject(device, GetStockObject(NULL_BRUSH));
			Rectangle(device, bar.left, bar.top, bar.right, bar.bottom);
			SelectObject(device, oldBrush);
			SelectObject(device, oldPen);
			DeleteObject(borderPen);

			wchar_t percent[16] = {};
			_snwprintf_s(percent, _countof(percent), _TRUNCATE, L"%u%%",
				static_cast<UInt32>(std::lround(state.progress * 100.0f)));
			SetTextColor(device, RGB(229, 229, 229));
			RECT stageRect = { 28, 122, client.right - 90, 150 };
			DrawTextW(device, state.stage.c_str(), -1, &stageRect,
				DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
			RECT percentRect = { client.right - 88, 122, client.right - 28, 150 };
			DrawTextW(device, percent, -1, &percentRect,
				DT_RIGHT | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);

			SelectObject(device, oldFont);
			DeleteObject(bodyFont);
			DeleteObject(titleFont);
			EndPaint(window, &paint);
		}

		LRESULT CALLBACK PrewarmProgressWindowProc(HWND window, UINT message,
			WPARAM wParam, LPARAM lParam)
		{
			switch (message)
			{
			case WM_ERASEBKGND:
				return 1;
			case WM_PAINT:
				DrawPrewarmProgress(window);
				return 0;
			case kPrewarmProgressRefreshMessage:
				InvalidateRect(window, nullptr, FALSE);
				UpdateWindow(window);
				return 0;
			case WM_ACTIVATEAPP:
				ShowWindowAsync(window, wParam ? SW_SHOWNOACTIVATE : SW_HIDE);
				return DefWindowProcW(window, message, wParam, lParam);
			case WM_CLOSE:
				DestroyWindow(window);
				return 0;
			case WM_DESTROY:
				PostQuitMessage(0);
				return 0;
			default:
				return DefWindowProcW(window, message, wParam, lParam);
			}
		}

		DWORD WINAPI PrewarmProgressThreadProc(void*)
		{
			const wchar_t* className = L"tNVSEFontPrewarmProgress";
			HINSTANCE instance = GetModuleHandleW(nullptr);
			WNDCLASSW windowClass = {};
			windowClass.lpfnWndProc = PrewarmProgressWindowProc;
			windowClass.hInstance = instance;
			windowClass.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32514));
			windowClass.lpszClassName = className;
			RegisterClassW(&windowClass);
			HWND owner = nullptr;
			{
				std::lock_guard<std::mutex> lock(s_progressMutex);
				owner = s_progressState.owner;
			}
			RECT ownerRect = {};
			if (!owner || !GetWindowRect(owner, &ownerRect))
			{
				ownerRect.left = 0;
				ownerRect.top = 0;
				ownerRect.right = GetSystemMetrics(SM_CXSCREEN);
				ownerRect.bottom = GetSystemMetrics(SM_CYSCREEN);
			}
			const int x = ownerRect.left + ((ownerRect.right - ownerRect.left)
				- kPrewarmProgressWidth) / 2;
			const int y = ownerRect.top + ((ownerRect.bottom - ownerRect.top)
				- kPrewarmProgressHeight) / 2;
			HWND window = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
				className, L"tNVSE Font Cache", WS_POPUP,
				x, y, kPrewarmProgressWidth, kPrewarmProgressHeight, owner,
				nullptr, instance, nullptr);
			s_progressWindow.store(window, std::memory_order_release);
			if (window)
			{
				ShowWindow(window, SW_SHOWNOACTIVATE);
				UpdateWindow(window);
			}
			if (s_progressReadyEvent)
				SetEvent(s_progressReadyEvent);
			MSG message = {};
			while (window && GetMessageW(&message, nullptr, 0, 0) > 0)
			{
				TranslateMessage(&message);
				DispatchMessageW(&message);
			}
			s_progressWindow.store(nullptr, std::memory_order_release);
			return 0;
		}

		BOOL CALLBACK FindCurrentProcessTopLevelWindow(HWND window, LPARAM result)
		{
			DWORD processId = 0;
			GetWindowThreadProcessId(window, &processId);
			if (processId != GetCurrentProcessId() || !IsWindowVisible(window)
				|| GetWindow(window, GW_OWNER))
				return TRUE;
			*reinterpret_cast<HWND*>(result) = window;
			return FALSE;
		}

		HWND FindPrewarmProgressOwner()
		{
			HWND owner = GetForegroundWindow();
			DWORD processId = 0;
			if (owner)
				GetWindowThreadProcessId(owner, &processId);
			if (processId == GetCurrentProcessId())
				return owner;
			owner = nullptr;
			EnumWindows(FindCurrentProcessTopLevelWindow,
				reinterpret_cast<LPARAM>(&owner));
			return owner;
		}

		void StartPrewarmProgress()
		{
			if (s_progressThread)
				return;
			DisableProcessWindowsGhosting();
			{
				std::lock_guard<std::mutex> lock(s_progressMutex);
				s_progressState = {};
				s_progressState.detail = L"Generating missing font cache...";
				s_progressState.stage = L"Preparing glyphs...";
				s_progressState.owner = FindPrewarmProgressOwner();
			}
			s_progressReadyEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
			s_progressThread = CreateThread(nullptr, 0,
				PrewarmProgressThreadProc, nullptr, 0, nullptr);
			if (s_progressThread && s_progressReadyEvent)
				WaitForSingleObject(s_progressReadyEvent, 2000);
		}

		void UpdatePrewarmProgress(const std::wstring& detail,
			const std::wstring& stage, float progress)
		{
			{
				std::lock_guard<std::mutex> lock(s_progressMutex);
				s_progressState.detail = detail;
				s_progressState.stage = stage;
				s_progressState.progress = progress;
			}
			if (HWND window = s_progressWindow.load(std::memory_order_acquire))
			{
				PostMessageW(window, kPrewarmProgressRefreshMessage, 0, 0);
				if (IsCurrentProcessForeground() && !IsWindowVisible(window))
					ShowWindowAsync(window, SW_SHOWNOACTIVATE);
			}
		}

		void StopPrewarmProgress()
		{
			if (HWND window = s_progressWindow.load(std::memory_order_acquire))
				PostMessageW(window, WM_CLOSE, 0, 0);
			if (s_progressThread)
			{
				WaitForSingleObject(s_progressThread, 3000);
				CloseHandle(s_progressThread);
				s_progressThread = nullptr;
			}
			if (s_progressReadyEvent)
			{
				CloseHandle(s_progressReadyEvent);
				s_progressReadyEvent = nullptr;
			}
		}

		struct PrewarmJob
		{
			UInt32 fontId = 0;
			UInt64 layoutHash = 0;
			UInt64 maskGenerationHash = 0;
			UInt64 shaderEffectHash = 0;
			UInt32 codePage = 0;
			const std::vector<UInt16>* encodedUnits = nullptr;
			size_t encodedUnitIndex = 0;
			size_t encodedUnitStart = 0;
			UInt32 validDoubleByteCount = 0;
			UInt32 rasterizedGlyphCount = 0;
			UInt32 mtsdfGlyphCount = 0;
			UInt32 targetUnitCount = 0;
			UInt32 rasterScaleMilli = 0;
		};

		struct SessionMaskProfile
		{
			UInt32 ownerFontId = 0;
			bool atlasReady = false;
			std::unordered_map<UInt64, GlyphCollisionProfile> collisions;
		};

		struct CompleteCodePageAtlasOnlyScope
		{
			bool active = false;

			explicit CompleteCodePageAtlasOnlyScope(bool enable) : active(enable)
			{
				if (active)
					BeginCompleteCodePageAtlasOnlyPrewarm();
			}

			~CompleteCodePageAtlasOnlyScope()
			{
				if (active)
					EndCompleteCodePageAtlasOnlyPrewarm();
			}
		};

		std::deque<PrewarmJob> s_jobs;
		std::unordered_set<UInt64> s_scheduledProfiles;
		bool s_configuredFontsQueued = false;
		bool s_atlasOnlyPrewarmPending = false;

		UInt64 BuildProfileKey(const FontConfig& config)
		{
			UInt64 hash = 1469598103934665603ull;
			auto add = [&](const void* data, size_t size)
			{
				const UInt8* bytes = static_cast<const UInt8*>(data);
				for (size_t index = 0; index < size; ++index)
				{
					hash ^= bytes[index];
					hash *= 1099511628211ull;
				}
			};
			add(&config.layoutHash, sizeof(config.layoutHash));
			add(&config.maskGenerationHash, sizeof(config.maskGenerationHash));
			add(&config.shaderEffectHash, sizeof(config.shaderEffectHash));
			add(&kCompleteCodePagePrewarmIdentity,
				sizeof(kCompleteCodePagePrewarmIdentity));
			const UInt32 codePage = GetFreeTypeTextCodePage();
			add(&codePage, sizeof(codePage));
			return hash;
		}

		UInt64 BuildSessionMaskProfileKey(const FontConfig& config,
			UInt32 rasterScaleMilli, UInt32 sdfSpread)
		{
			UInt64 hash = 1469598103934665603ull;
			auto add = [&](const void* data, size_t size)
			{
				const UInt8* bytes = static_cast<const UInt8*>(data);
				for (size_t index = 0; index < size; ++index)
				{
					hash ^= bytes[index];
					hash *= 1099511628211ull;
				}
			};
			add(&config.maskGenerationHash, sizeof(config.maskGenerationHash));
			add(&rasterScaleMilli, sizeof(rasterScaleMilli));
			add(&sdfSpread, sizeof(sdfSpread));
			const UInt32 codePage = GetFreeTypeTextCodePage();
			add(&codePage, sizeof(codePage));
			add(&kCompleteCodePagePrewarmIdentity,
				sizeof(kCompleteCodePagePrewarmIdentity));
			add(&kMtsdfGeneratorRevision, sizeof(kMtsdfGeneratorRevision));
			return hash;
		}

		bool MatchesPrewarmProfile(const PrewarmJob& job, const FontConfig& config)
		{
			return job.layoutHash == config.layoutHash
				&& job.maskGenerationHash == config.maskGenerationHash
				&& job.shaderEffectHash == config.shaderEffectHash
				&& job.codePage == GetFreeTypeTextCodePage();
		}

		bool NextEncodedUnit(PrewarmJob& job, std::array<char, 2>& bytes,
			size_t& length)
		{
			if (!job.encodedUnits)
				return false;
			const std::vector<UInt16>& units = *job.encodedUnits;
			if (job.encodedUnitIndex >= units.size())
				return false;
			const UInt16 encoded = units[job.encodedUnitIndex++];
			bytes[0] = static_cast<char>(encoded > 0xFF
				? encoded >> 8 : encoded);
			bytes[1] = static_cast<char>(encoded & 0xFF);
			length = encoded > 0xFF ? 2 : 1;
			return true;
		}

		void ResetPrewarmScan(PrewarmJob& job, UInt32 rasterScaleMilli)
		{
			const std::vector<UInt16>& units = GetCompleteCodePageEncodedUnits();
			job.encodedUnits = &units;
			job.encodedUnitStart = static_cast<size_t>(std::lower_bound(
				units.begin(), units.end(), static_cast<UInt16>(0x20)) - units.begin());
			job.encodedUnitIndex = job.encodedUnitStart;
			job.validDoubleByteCount = 0;
			job.rasterizedGlyphCount = 0;
			job.mtsdfGlyphCount = 0;
			job.rasterScaleMilli = rasterScaleMilli;
			job.targetUnitCount = static_cast<UInt32>(
				units.size() - job.encodedUnitStart);
		}

		size_t SaturatingMultiply(size_t left, size_t right)
		{
			return !left || right <= std::numeric_limits<size_t>::max() / left
				? left * right : std::numeric_limits<size_t>::max();
		}

		UInt32 ResolvePrewarmGlyphBatchLimit(const FontConfig& config,
			float rasterScale, bool shaderMtsdf, UInt32 sdfSpread)
		{
			size_t worstBytes = 1;
			for (const ByteStyle& style : config.styles)
			{
				const size_t bodyWidth = static_cast<size_t>(std::max(1.0f,
					std::ceil(style.pixelSize * style.scaleX * rasterScale)));
				const size_t bodyHeight = static_cast<size_t>(std::max(1.0f,
					std::ceil(style.pixelSize * style.scaleY * rasterScale)));
				float effectRadius = 2.0f;
				if (shaderMtsdf)
					effectRadius += static_cast<float>(sdfSpread);
				else
				{
					if (config.glow.enabled)
						effectRadius = std::max(effectRadius,
							config.glow.outer * rasterScale + 3.0f);
					if (config.outline.enabled)
						effectRadius = std::max(effectRadius,
							(config.outline.width + config.outline.softness)
							* rasterScale + 3.0f);
				}
				const size_t expansion = static_cast<size_t>(std::ceil(effectRadius)) * 2u + 2u;
				const size_t width = bodyWidth + expansion;
				const size_t height = bodyHeight + expansion;
				// MTSDF has one logical mask but four resident bytes per texel.
				// Keep the batch limiter tied to actual bitmap memory so a full
				// code-page prewarm cannot overshoot its target by roughly 4x.
				size_t masks = shaderMtsdf ? 4u : 1u;
				if (!shaderMtsdf)
					masks += (config.glow.enabled ? 1u : 0u)
						+ (config.outline.enabled ? 1u : 0u);
				worstBytes = std::max(worstBytes,
					SaturatingMultiply(SaturatingMultiply(width, height), masks));
			}
			const size_t configuredBudget = GetCpuMemoryBudget();
			const size_t targetBytes = std::max<size_t>(4u * 1024u * 1024u,
				std::min(kMaximumPrewarmBatchBytes, configuredBudget / 8u));
			const size_t resolved = worstBytes ? targetBytes / worstBytes : 1;
			return static_cast<UInt32>(std::clamp<size_t>(resolved, 1,
				kMaximumGlyphsPerBatch));
		}

		float GetPrewarmJobProgress(const PrewarmJob& job)
		{
			const size_t completed = job.encodedUnitIndex >= job.encodedUnitStart
				? job.encodedUnitIndex - job.encodedUnitStart : 0;
			return job.targetUnitCount
				? std::min(1.0f, static_cast<float>(completed) / job.targetUnitCount)
				: 0.0f;
		}

		void ReportPrewarmProgress(const PrewarmJob& job, UInt32 fontOrdinal,
			UInt32 fontCount, UInt32 finishedFonts, const wchar_t* stage,
			float minimumJobProgress = 0.0f)
		{
			wchar_t detail[160] = {};
			const wchar_t* renderMode = ResolveFontAtlasRoute(IsA8RendererAvailable())
				== FontAtlasRoute::ShaderMtsdf ? L"MTSDF" : L"ARGB fallback";
			_snwprintf_s(detail, _countof(detail), _TRUNCATE,
				L"Font %u of %u  |  ID %u  |  %ls",
				fontOrdinal, fontCount, job.fontId, renderMode);
			const float jobProgress = std::max(minimumJobProgress,
				GetPrewarmJobProgress(job));
			const float overall = fontCount
				? (static_cast<float>(finishedFonts) + jobProgress) / fontCount
				: 1.0f;
			UpdatePrewarmProgress(detail, stage ? stage : L"Preparing glyphs...", overall);
		}

		void FinishJob(const PrewarmJob& job, const char* status)
		{
			gLog.FormattedMessage(
				"tnvse_freetype_font: prewarm font=%u coverage=full-codepage scale=%.3f glyphs=%u doubleByte=%u mtsdfGlyphs=%u status=%s",
				job.fontId,
				job.rasterScaleMilli ? job.rasterScaleMilli / 1000.0f : 0.0f,
				job.rasterizedGlyphCount, job.validDoubleByteCount,
				job.mtsdfGlyphCount, status);
		}

		bool TryReuseSessionMaskProfile(PrewarmJob& job, RuntimeFont& runtime,
			const SessionMaskProfile& profile, float rasterScale,
			UInt32 rasterScaleMilli, UInt32 sdfSpread, UInt32 batchGlyphLimit,
			UInt32 candidateLimit,
			UInt32 fontOrdinal, UInt32 fontCount, UInt32 finishedFonts)
		{
			if (!profile.atlasReady || profile.collisions.empty())
				return false;

			ResetPrewarmScan(job, rasterScaleMilli);
			std::vector<VectorEncodedGlyph> glyphs;
			std::vector<GlyphBitmapRequest> requests;
			std::vector<UInt64> cacheIds;
			bool exhausted = false;
			while (!exhausted)
			{
				glyphs.clear();
				requests.clear();
				glyphs.reserve(batchGlyphLimit);
				requests.reserve(batchGlyphLimit);
				UInt32 candidates = 0;
				while (candidates < candidateLimit
					&& requests.size() < batchGlyphLimit)
				{
					std::array<char, 2> bytes = {};
					size_t length = 0;
					if (!NextEncodedUnit(job, bytes, length))
					{
						exhausted = true;
						break;
					}
					++candidates;
					VectorEncodedGlyph glyph;
					if (!ResolvePrewarmGlyph(runtime, bytes.data(), length, glyph))
						continue;
					if (length == 2)
						++job.validDoubleByteCount;
					glyphs.push_back(glyph);
					requests.push_back({ &glyphs.back(),
						GlyphMaskType::DistanceField,
						static_cast<UInt8>(sdfSpread) });
					++job.rasterizedGlyphCount;
					++job.mtsdfGlyphCount;
				}

				if (requests.empty())
					continue;
				ResolveGlyphBitmapCacheIds(runtime, requests, rasterScale, cacheIds);
				if (cacheIds.size() != requests.size())
					return false;
				for (size_t index = 0; index < requests.size(); ++index)
				{
					const auto collision = profile.collisions.find(cacheIds[index]);
					if (!cacheIds[index] || collision == profile.collisions.end()
						|| !requests[index].glyph
						|| !StoreGlyphCollisionProfile(runtime,
							*requests[index].glyph, collision->second))
					{
						return false;
					}
				}
				ReportPrewarmProgress(job, fontOrdinal, fontCount, finishedFonts,
					L"Reusing streamed MTSDF profile...");
			}

			MarkGlyphManifestComplete(runtime);
			return TryLoadGloballyRepackedGlyphAtlasSnapshot(runtime, rasterScale);
		}
	}

	void QueueFontPrewarm(UInt32 fontId)
	{
		const FontConfig* config = FindConfig(fontId);
		if (!config)
			return;
		if (!EnsureRuntimeFont(fontId))
		{
			gLog.FormattedMessage(
				"tnvse_freetype_font: prewarm deferred because runtime initialization failed font=%u",
				fontId);
			return;
		}

		const UInt64 key = BuildProfileKey(*config);
		if (!s_scheduledProfiles.insert(key).second)
		{
			const auto shared = std::find_if(s_jobs.begin(), s_jobs.end(),
				[config](const PrewarmJob& job)
				{
					return MatchesPrewarmProfile(job, *config);
				});
			if (shared != s_jobs.end())
			{
				gLog.FormattedMessage(
					"tnvse_freetype_font: prewarm profile alias font=%u owner=%u",
					fontId, shared->fontId);
			}
			return;
		}

		PrewarmJob job;
		job.fontId = fontId;
		job.layoutHash = config->layoutHash;
		job.maskGenerationHash = config->maskGenerationHash;
		job.shaderEffectHash = config->shaderEffectHash;
		job.codePage = GetFreeTypeTextCodePage();
		ResetPrewarmScan(job, 0);
		const UInt32 targetUnitCount = job.targetUnitCount;
		s_jobs.push_back(std::move(job));
		if (!s_atlasOnlyPrewarmPending)
		{
			// Fonts can be asked to draw between activation and the first game-loop
			// prewarm pump. Start the MTSDF atlas-only policy at queue time so those
			// early requests cannot create a short-lived .tnvfmask first.
			BeginCompleteCodePageAtlasOnlyPrewarm();
			s_atlasOnlyPrewarmPending = true;
		}
		SetBitmapCacheReducedAfterPrewarm(false);
		gLog.FormattedMessage(
			"tnvse_freetype_font: queued prewarm font=%u coverage=full-codepage codePage=%u units=%u",
			fontId, GetFreeTypeTextCodePage(), targetUnitCount);
	}

	void QueueConfiguredFontPrewarms()
	{
		if (s_configuredFontsQueued)
			return;
		s_configuredFontsQueued = true;
		std::vector<UInt32> fontIds;
		fontIds.reserve(g_configs.size());
		for (const auto& entry : g_configs)
			fontIds.push_back(entry.first);
		std::sort(fontIds.begin(), fontIds.end());
		for (UInt32 fontId : fontIds)
			QueueFontPrewarm(fontId);
	}

	void PumpFontPrewarm()
	{
		if (!g_bEnableFreeTypeFontRendering)
		{
			if (s_atlasOnlyPrewarmPending)
			{
				EndCompleteCodePageAtlasOnlyPrewarm();
				s_atlasOnlyPrewarmPending = false;
			}
			return;
		}
		QueueConfiguredFontPrewarms();
		if (s_jobs.empty())
			return;

		const float rasterScale = GetCanonicalFreeTypeRasterScale();
		const UInt32 rasterScaleMilli = static_cast<UInt32>(std::lround(
			rasterScale * 1000.0f));
		const UInt32 queuedFonts = static_cast<UInt32>(s_jobs.size());
		std::vector<UInt32> verifiedCodePageFonts;
		verifiedCodePageFonts.reserve(queuedFonts);
		const ULONGLONG started = GetTickCount64();
		UInt32 batches = 0;
		UInt32 completedFonts = 0;
		UInt32 streamFailedFonts = 0;
		UInt32 cancelledFonts = 0;
		UInt32 finishedFonts = 0;
		bool everyGeneratedProfileUsedMtsdfAtlas = true;

		gLog.FormattedMessage(
			"tnvse_freetype_font: blocking streamed prewarm begin fonts=%u scale=%.3f batchTargetMiB=%.2f",
			queuedFonts, rasterScale,
			kMaximumPrewarmBatchBytes / (1024.0 * 1024.0));

		// Restore validated snapshots first. DEFAULT-pool pages do not retain a full
		// CPU atlas copy; managed pages are intentionally not bulk-restored in this
		// 32-bit process because that would recreate the address-space failure that
		// streaming prewarm is designed to avoid.
		std::deque<PrewarmJob> cacheMisses;
		while (!s_jobs.empty())
		{
			PrewarmJob job = std::move(s_jobs.front());
			s_jobs.pop_front();
			if (job.rasterScaleMilli != rasterScaleMilli)
				ResetPrewarmScan(job, rasterScaleMilli);
			const FontConfig* config = FindConfig(job.fontId);
			RuntimeFont* runtime = FindRuntimeFont(job.fontId);
			if (!config || !runtime || config->layoutHash != job.layoutHash
				|| config->maskGenerationHash != job.maskGenerationHash
				|| config->shaderEffectHash != job.shaderEffectHash
				|| job.codePage != GetFreeTypeTextCodePage())
			{
				if (runtime)
					CancelStreamingPrewarmAtlas(*runtime);
				FinishJob(job, "cancelled");
				++cancelledFonts;
				++finishedFonts;
				continue;
			}
			if (g_bEnableFreeTypeDefaultPoolAtlas
				&& TryLoadGloballyRepackedGlyphAtlasSnapshot(*runtime, rasterScale))
			{
				FinishJob(job, "snapshot");
				verifiedCodePageFonts.push_back(job.fontId);
				++completedFonts;
				++finishedFonts;
				continue;
			}
			// Any failed completeness check makes the old construction transaction
			// unusable. Discard every dependent artifact before this job enters the
			// generator; partial manifests, masks and atlas pages are never mixed with
			// the fresh code-page pass.
			CancelStreamingPrewarmAtlas(*runtime);
			const bool atlasDiscarded = DiscardGlyphAtlasSnapshot(*runtime,
				rasterScale);
			const bool persistentDiscarded =
				ResetPersistentFontCachesForRegeneration(*runtime);
			ResetPrewarmScan(job, rasterScaleMilli);
			gLog.FormattedMessage(
				"tnvse_freetype_font: cache miss regenerated from empty state font=%u atlas=%s persistent=%s",
				job.fontId, atlasDiscarded ? "discarded" : "delete-failed",
				persistentDiscarded ? "discarded" : "invalidate-failed");
			cacheMisses.push_back(std::move(job));
		}
		s_jobs.swap(cacheMisses);

		if (!s_jobs.empty())
		{
			StartPrewarmProgress();
			UpdatePrewarmProgress(L"Generating missing font cache...",
				L"Preparing streamed glyph batches...",
				static_cast<float>(finishedFonts) / queuedFonts);
		}

		// Streamed full-codepage construction is the durable bitmap tier. Disable
		// MTSDF .tnvfmask lookup and creation only for this transaction; normal demand
		// rendering regains the persistent bitmap policy when the scope exits.
		CompleteCodePageAtlasOnlyScope atlasOnlyScope(
			s_atlasOnlyPrewarmPending || !s_jobs.empty());
		s_atlasOnlyPrewarmPending = false;
		std::unordered_map<UInt64, SessionMaskProfile> sessionMaskProfiles;
		std::unordered_map<UInt64, UInt32> sessionMaskProfileRemainingUses;
		for (const PrewarmJob& queued : s_jobs)
		{
			const FontConfig* queuedConfig = FindConfig(queued.fontId);
			if (!queuedConfig)
				continue;
			EffectQuality queuedQuality = queuedConfig->effectQuality;
			const bool queuedMtsdf = ResolveFontAtlasRoute(IsA8RendererAvailable())
				== FontAtlasRoute::ShaderMtsdf
				&& ResolveA8EffectQuality(queuedConfig->effectQuality, queuedQuality);
			UInt32 queuedSpread = 0;
			if (!queuedMtsdf
				|| !ResolveSdfSpread(*queuedConfig, rasterScale, queuedSpread))
			{
				continue;
			}
			++sessionMaskProfileRemainingUses[BuildSessionMaskProfileKey(
				*queuedConfig, rasterScaleMilli, queuedSpread)];
		}
		UInt32 sharedSessionProfiles = 0;
		for (const auto& usage : sessionMaskProfileRemainingUses)
			sharedSessionProfiles += usage.second > 1 ? 1u : 0u;
		gLog.FormattedMessage(
			"tnvse_freetype_font: session MTSDF reuse plan generatedProfiles=%u sharedProfiles=%u",
			static_cast<UInt32>(sessionMaskProfileRemainingUses.size()),
			sharedSessionProfiles);
		auto consumeSessionMaskProfile = [&](UInt64 key)
		{
			if (!key)
				return;
			const auto remaining = sessionMaskProfileRemainingUses.find(key);
			if (remaining == sessionMaskProfileRemainingUses.end())
				return;
			if (remaining->second > 1)
			{
				--remaining->second;
				return;
			}
			sessionMaskProfileRemainingUses.erase(remaining);
			sessionMaskProfiles.erase(key);
		};

		std::vector<VectorEncodedGlyph> requestedGlyphs;
		std::vector<GlyphBitmapRequest> bitmapRequests;
		std::vector<std::shared_ptr<const GlyphBitmap>> bitmapResults;

		while (!s_jobs.empty())
		{
			PrewarmJob job = std::move(s_jobs.front());
			s_jobs.pop_front();
			const FontConfig* config = FindConfig(job.fontId);
			RuntimeFont* runtime = FindRuntimeFont(job.fontId);
			const UInt32 fontOrdinal = std::min(queuedFonts, finishedFonts + 1);
			if (!config || !runtime || config->layoutHash != job.layoutHash
				|| config->maskGenerationHash != job.maskGenerationHash
				|| config->shaderEffectHash != job.shaderEffectHash
				|| job.codePage != GetFreeTypeTextCodePage())
			{
				if (runtime)
					CancelStreamingPrewarmAtlas(*runtime);
				FinishJob(job, "cancelled");
				++cancelledFonts;
				++finishedFonts;
				continue;
			}

			// A preceding font may have reduced the bitmap LRU to reserve address
			// space for atlas publication. Restore the bounded prewarm working target
			// while this font rasterizes; it will be reduced again before publishing.
			SetBitmapCacheReducedAfterPrewarm(false);
			EffectQuality resolvedQuality = config->effectQuality;
			bool shaderMtsdf = ResolveFontAtlasRoute(IsA8RendererAvailable())
				== FontAtlasRoute::ShaderMtsdf
				&& ResolveA8EffectQuality(config->effectQuality, resolvedQuality);
			UInt32 sdfSpread = 0;
			if (shaderMtsdf && !ResolveSdfSpread(*config, rasterScale, sdfSpread))
				shaderMtsdf = false;
			if (!shaderMtsdf)
				everyGeneratedProfileUsedMtsdfAtlas = false;
			const UInt32 batchGlyphLimit = ResolvePrewarmGlyphBatchLimit(
				*config, rasterScale, shaderMtsdf, sdfSpread);
			const UInt32 candidateLimit = std::min(kMaximumCandidatesPerBatch,
				std::max<UInt32>(256, batchGlyphLimit * 8u));
			const UInt64 sessionMaskProfileKey = shaderMtsdf
				? BuildSessionMaskProfileKey(*config, rasterScaleMilli, sdfSpread) : 0;
			const auto remainingSessionUses = sessionMaskProfileRemainingUses.find(
				sessionMaskProfileKey);
			const bool retainSessionCollisions = shaderMtsdf
				&& remainingSessionUses != sessionMaskProfileRemainingUses.end()
				&& remainingSessionUses->second > 1;
			if (shaderMtsdf)
			{
				const auto reusable = sessionMaskProfiles.find(sessionMaskProfileKey);
				if (reusable != sessionMaskProfiles.end() && reusable->second.atlasReady)
				{
					if (TryReuseSessionMaskProfile(job, *runtime, reusable->second,
						rasterScale, rasterScaleMilli, sdfSpread, batchGlyphLimit,
						candidateLimit, fontOrdinal, queuedFonts, finishedFonts))
					{
						gLog.FormattedMessage(
							"tnvse_freetype_font: session MTSDF profile reused font=%u owner=%u collisions=%u",
							job.fontId, reusable->second.ownerFontId,
							static_cast<UInt32>(reusable->second.collisions.size()));
						verifiedCodePageFonts.push_back(job.fontId);
						FinishJob(job, "session-atlas");
						++completedFonts;
						++finishedFonts;
						consumeSessionMaskProfile(sessionMaskProfileKey);
						continue;
					}
					gLog.FormattedMessage(
						"tnvse_freetype_font: session MTSDF profile reuse incomplete font=%u owner=%u; falling back to streamed generation",
						job.fontId, reusable->second.ownerFontId);
					ResetPrewarmScan(job, rasterScaleMilli);
				}
				if (retainSessionCollisions)
				{
					SessionMaskProfile& session =
						sessionMaskProfiles[sessionMaskProfileKey];
					if (!session.ownerFontId)
						session.ownerFontId = job.fontId;
				}
			}
			const bool needsGrayFill = !shaderMtsdf;
			bool exhausted = false;
			bool failed = false;

			while (!exhausted && !failed)
			{
				requestedGlyphs.clear();
				bitmapRequests.clear();
				bitmapResults.clear();
				requestedGlyphs.reserve(batchGlyphLimit);
				bitmapRequests.reserve(static_cast<size_t>(batchGlyphLimit) * 3u);
				UInt32 candidates = 0;
				UInt32 glyphCount = 0;
				while (candidates < candidateLimit && glyphCount < batchGlyphLimit)
				{
					std::array<char, 2> bytes = {};
					size_t length = 0;
					if (!NextEncodedUnit(job, bytes, length))
					{
						exhausted = true;
						break;
					}
					++candidates;
					VectorEncodedGlyph glyph;
					if (!ResolvePrewarmGlyph(*runtime, bytes.data(), length, glyph))
						continue;
					if (length == 2)
						++job.validDoubleByteCount;
					requestedGlyphs.push_back(glyph);
					const VectorEncodedGlyph* requestedGlyph = &requestedGlyphs.back();
					if (needsGrayFill)
						bitmapRequests.push_back({ requestedGlyph,
							GlyphMaskType::Fill, 0 });
					if (shaderMtsdf)
					{
						bitmapRequests.push_back({ requestedGlyph,
							GlyphMaskType::DistanceField, sdfSpread });
						++job.mtsdfGlyphCount;
					}
					if (config->glow.enabled && !shaderMtsdf)
						bitmapRequests.push_back({ requestedGlyph,
							GlyphMaskType::Glow, 0 });
					if (config->outline.enabled && !shaderMtsdf)
						bitmapRequests.push_back({ requestedGlyph,
							GlyphMaskType::Outline, 0 });
					++glyphCount;
					++job.rasterizedGlyphCount;
				}

				if (!bitmapRequests.empty())
				{
					try
					{
						GetPrewarmGlyphBitmaps(*runtime, bitmapRequests,
							rasterScale, bitmapResults);
					}
					catch (const std::bad_alloc&)
					{
						gLog.FormattedMessage(
							"tnvse_freetype_font: streamed prewarm main-thread allocation failed font=%u scale=%.3f batchGlyphs=%u",
							job.fontId, rasterScale, glyphCount);
						failed = true;
					}
					catch (...)
					{
						gLog.FormattedMessage(
							"tnvse_freetype_font: streamed prewarm batch raised an unexpected exception font=%u",
							job.fontId);
						failed = true;
					}
					if (!failed && bitmapResults.size() != bitmapRequests.size())
						failed = true;
				}

				if (!failed)
				{
					for (size_t index = 0; index < bitmapRequests.size(); ++index)
					{
						if (bitmapRequests[index].glyph && bitmapResults[index]
							&& (bitmapRequests[index].maskType == GlyphMaskType::Fill
								|| bitmapRequests[index].maskType
									== GlyphMaskType::DistanceField))
						{
							StoreGlyphCollisionProfile(*runtime,
								*bitmapRequests[index].glyph, *bitmapResults[index],
								rasterScale);
							if (retainSessionCollisions
								&& bitmapRequests[index].maskType
									== GlyphMaskType::DistanceField)
							{
								GlyphCollisionProfile collision;
								if (LoadGlyphCollisionProfile(*runtime,
									*bitmapRequests[index].glyph, collision))
								{
									sessionMaskProfiles[sessionMaskProfileKey].collisions
										.emplace(bitmapResults[index]->cacheId, collision);
								}
							}
						}
					}
					failed = !AppendStreamingPrewarmAtlas(*runtime,
						bitmapRequests, bitmapResults, rasterScale);
				}

				// Release every strong bitmap reference before the next batch. The stream
				// owns only one packed MTSDF page and its placement metadata; the bitmap LRU
				// is then free to obey the aggregate CPU budget.
				bitmapResults.clear();
				bitmapRequests.clear();
				requestedGlyphs.clear();
				EnforceCpuMemoryBudget("prewarm-stream-batch");
				++batches;
				ReportPrewarmProgress(job, fontOrdinal, queuedFonts, finishedFonts,
					shaderMtsdf ? L"Streaming MTSDF glyphs to disk..."
						: L"Generating bounded fallback masks...");
			}

			if (failed)
			{
				CancelStreamingPrewarmAtlas(*runtime);
				DiscardGlyphAtlasSnapshot(*runtime, rasterScale);
				ResetPersistentFontCachesForRegeneration(*runtime);
				sessionMaskProfiles.erase(sessionMaskProfileKey);
				consumeSessionMaskProfile(sessionMaskProfileKey);
				FinishJob(job, "stream-failed");
				++streamFailedFonts;
				++finishedFonts;
				continue;
			}

			ReportPrewarmProgress(job, fontOrdinal, queuedFonts, finishedFonts,
				L"Publishing streamed atlas pages...", 0.995f);
			// MTSDF publishing temporarily needs both the compact placed-rectangle
			// payload and an upload/repack buffer. Atlas-only mode prevents new mask
			// profiles, while this release also removes mappings that may predate the
			// transaction before the 32-bit address-space peak.
			const UInt64 releasedMappingBytes = ReleaseGlyphBitmapDiskCacheMappings();
			SetBitmapCacheReducedAfterPrewarm(true);
			EnforceCpuMemoryBudget("prewarm-before-atlas-publish");
			if (releasedMappingBytes)
			{
				gLog.FormattedMessage(
					"tnvse_freetype_font: atlas publish address-space reserve font=%u releasedMaskMappings=%llu",
					job.fontId,
					static_cast<unsigned long long>(releasedMappingBytes));
			}
			bool finalized = false;
			try
			{
				finalized = FinalizeStreamingPrewarmAtlas(*runtime, rasterScale);
			}
			catch (const std::bad_alloc&)
			{
				gLog.FormattedMessage(
					"tnvse_freetype_font: streamed atlas publish allocation failed font=%u scale=%.3f",
					job.fontId, rasterScale);
			}
			catch (...)
			{
				gLog.FormattedMessage(
					"tnvse_freetype_font: streamed atlas publish raised an unexpected exception font=%u",
					job.fontId);
			}
			if (!finalized)
			{
				CancelStreamingPrewarmAtlas(*runtime);
				DiscardGlyphAtlasSnapshot(*runtime, rasterScale);
				ResetPersistentFontCachesForRegeneration(*runtime);
				sessionMaskProfiles.erase(sessionMaskProfileKey);
				consumeSessionMaskProfile(sessionMaskProfileKey);
				FinishJob(job, "stream-finalize-failed");
				++streamFailedFonts;
				++finishedFonts;
				continue;
			}
			if (g_bEnableFreeTypeDefaultPoolAtlas)
				verifiedCodePageFonts.push_back(job.fontId);
			if (retainSessionCollisions)
			{
				SessionMaskProfile& session =
					sessionMaskProfiles[sessionMaskProfileKey];
				session.atlasReady = true;
				gLog.FormattedMessage(
					"tnvse_freetype_font: session MTSDF profile published font=%u collisions=%u",
					job.fontId, static_cast<UInt32>(session.collisions.size()));
			}
			FinishJob(job, "complete");
			++completedFonts;
			++finishedFonts;
			consumeSessionMaskProfile(sessionMaskProfileKey);
			UpdatePrewarmProgress(L"Streamed font atlas is ready.",
				L"Preparing the next font...",
				static_cast<float>(finishedFonts) / queuedFonts);
		}

		gLog.FormattedMessage(
			"tnvse_freetype_font: blocking streamed prewarm end fonts=%u complete=%u streamFailed=%u cancelled=%u batches=%u elapsedMs=%llu",
			queuedFonts, completedFonts, streamFailedFonts, cancelledFonts, batches,
			static_cast<unsigned long long>(GetTickCount64() - started));
		FlushGlyphBitmapDiskCache();
		ReleaseGlyphBitmapDiskCacheMappings();
		std::unordered_set<UInt64> verifiedProfileKeys;
		for (UInt32 fontId : verifiedCodePageFonts)
		{
			if (const FontConfig* config = FindConfig(fontId))
				verifiedProfileKeys.insert(BuildProfileKey(*config));
		}
		std::unordered_set<UInt64> configuredProfileKeys;
		std::vector<UInt32> atlasOnlyFontIds;
		atlasOnlyFontIds.reserve(g_configs.size());
		UInt32 readyConfiguredRuntimes = 0;
		for (const auto& entry : g_configs)
		{
			const UInt64 profileKey = BuildProfileKey(entry.second);
			configuredProfileKeys.insert(profileKey);
			if (FindRuntimeFont(entry.first))
			{
				++readyConfiguredRuntimes;
				if (verifiedProfileKeys.count(profileKey))
					atlasOnlyFontIds.push_back(entry.first);
			}
		}
		std::sort(atlasOnlyFontIds.begin(), atlasOnlyFontIds.end());
		const bool everyConfiguredJobCompleted = completedFonts == queuedFonts
			&& queuedFonts == static_cast<UInt32>(configuredProfileKeys.size())
			&& readyConfiguredRuntimes == static_cast<UInt32>(g_configs.size());
		const bool everyConfiguredProfileVerified = everyConfiguredJobCompleted
			&& verifiedCodePageFonts.size() == queuedFonts
			&& verifiedProfileKeys.size() == configuredProfileKeys.size();
		if (everyConfiguredProfileVerified && everyGeneratedProfileUsedMtsdfAtlas)
		{
			if (!DeleteCompleteCodePageGlyphBitmapDiskCaches(atlasOnlyFontIds))
			{
				gLog.FormattedMessage(
					"tnvse_freetype_font: complete codepage mask cleanup incomplete; residual files will not be reused in this process");
			}
		}
		else if (!everyConfiguredProfileVerified)
		{
			gLog.FormattedMessage(
				"tnvse_freetype_font: complete codepage atlas-only transaction incomplete complete=%u verifiedAtlas=%u queued=%u verifiedProfiles=%u configuredProfiles=%u readyRuntimes=%u configuredFonts=%u; runtime persistent-mask policy will resume",
				completedFonts, static_cast<UInt32>(verifiedCodePageFonts.size()),
				queuedFonts, static_cast<UInt32>(verifiedProfileKeys.size()),
				static_cast<UInt32>(configuredProfileKeys.size()),
				readyConfiguredRuntimes, static_cast<UInt32>(g_configs.size()));
		}
		else
		{
			gLog.FormattedMessage(
				"tnvse_freetype_font: persistent fallback masks retained because at least one completed profile used the ARGB route");
		}
		if (everyConfiguredJobCompleted)
		{
			SetBitmapCacheReducedAfterPrewarm(true);
			EnforceCpuMemoryBudget("post-prewarm");
		}
		else
		{
			gLog.FormattedMessage(
				"tnvse_freetype_font: bitmap cache post-prewarm shrink skipped because streamed prewarm did not complete successfully complete=%u queued=%u",
				completedFonts, queuedFonts);
		}
		if (g_bDeleteUnusedFreeTypeFontCache && everyConfiguredJobCompleted)
			DeleteUnusedFreeTypeFontCacheFiles();
		else if (g_bDeleteUnusedFreeTypeFontCache)
		{
			gLog.FormattedMessage(
				"tnvse_freetype_font: unused persistent cache cleanup skipped because streamed prewarm did not complete successfully");
		}
		if (s_progressThread)
		{
			UpdatePrewarmProgress(everyConfiguredJobCompleted
					? L"Font cache is ready."
					: L"Font cache generation was incomplete.",
				everyConfiguredJobCompleted
					? L"Starting the game..."
					: L"Starting with runtime fallback...", 1.0f);
			StopPrewarmProgress();
		}
	}
}

namespace fonthook
{
	void PumpFreeTypeFontPrewarm()
	{
		vectorfont::PumpFontPrewarm();
	}
}

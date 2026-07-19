#include "font_vector_internal.h"

#include "encoding.h"
#include "load_config.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <deque>
#include <iterator>
#include <mutex>
#include <string>
#include <unordered_set>

namespace fonthook::vectorfont
{
	namespace
	{
		constexpr UInt32 kCommonDoubleByteLimit = 7000;
		// Full code-page prewarm is a startup barrier, so favor wide CPU batches.
		// Atlas construction is deferred until one font is fully rasterized; a 4096
		// glyph batch therefore only bounds worker and persistent-write memory.
		constexpr UInt32 kMaximumCandidatesPerBatch = 32768;
		constexpr UInt32 kMaximumGlyphsPerBatch = 4096;
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
			state.progress = std::max(0.0f, std::min(1.0f, state.progress));
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
			// Blocking prewarm intentionally keeps the game-window thread busy. A
			// ghost window would freeze the initial progress frame above the live
			// owned progress window and can leave it hidden after reactivation.
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
			FontPrewarmMode mode = FontPrewarmMode::None;
			UInt32 singleByte = 0x20;
			UInt32 leadByte = 0x81;
			UInt32 leadByteEnd = 0xFE;
			UInt32 trailByte = 0x40;
			UInt32 trailByteStart = 0x40;
			UInt32 validDoubleByteCount = 0;
			UInt32 rasterizedGlyphCount = 0;
			UInt32 sdfGlyphCount = 0;
			UInt32 effectOnlySdfGlyphCount = 0;
			UInt32 targetUnitCount = 0;
			UInt32 rasterScaleMilli = 0;
			UInt64 snapshotRevision = 0;
			bool scanningDoubleByte = false;
			bool snapshotAttempted = false;
			std::array<std::vector<std::shared_ptr<const GlyphBitmap>>, 2> atlasBitmaps;
			std::array<std::unordered_set<UInt64>, 2> atlasBitmapIds;
		};

		std::deque<PrewarmJob> s_jobs;
		std::unordered_set<UInt64> s_scheduledProfiles;
		UInt64 s_snapshotRevision = 0;
		bool s_configuredFontsQueued = false;

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
			add(&config.prewarm, sizeof(config.prewarm));
			const UInt32 codePage = GetFreeTypeTextCodePage();
			add(&codePage, sizeof(codePage));
			return hash;
		}

		bool MatchesPrewarmProfile(const PrewarmJob& job, const FontConfig& config)
		{
			return job.layoutHash == config.layoutHash &&
				job.maskGenerationHash == config.maskGenerationHash &&
				job.shaderEffectHash == config.shaderEffectHash &&
				job.codePage == GetFreeTypeTextCodePage() &&
				job.mode == config.prewarm;
		}

		void ConfigureCommonRange(PrewarmJob& job)
		{
			if (job.mode != FontPrewarmMode::Common)
				return;
			switch (job.codePage)
			{
			case 936:
				job.leadByte = 0xA1;
				job.leadByteEnd = 0xF7;
				job.trailByte = job.trailByteStart = 0xA1;
				break;
			case 950:
				job.leadByte = 0xA1;
				job.leadByteEnd = 0xC6;
				break;
			case 932:
				job.leadByte = 0x81;
				job.leadByteEnd = 0xEA;
				break;
			case 949:
				job.leadByte = 0xA1;
				job.leadByteEnd = 0xC8;
				job.trailByte = job.trailByteStart = 0xA1;
				break;
			default:
				job.leadByteEnd = 0;
				break;
			}
		}

		bool IsInRange(UInt32 value, UInt32 first, UInt32 last)
		{
			return value >= first && value <= last;
		}

		bool IsGb2312DoubleByteUnit(UInt32 lead, UInt32 trail)
		{
			if (lead == 0xA1 || lead == 0xA3)
				return IsInRange(trail, 0xA1, 0xFE);
			if (lead == 0xA2)
				return IsInRange(trail, 0xB1, 0xE2)
					|| IsInRange(trail, 0xE5, 0xEE)
					|| IsInRange(trail, 0xF1, 0xFC);
			if (lead == 0xA4)
				return IsInRange(trail, 0xA1, 0xF3);
			if (lead == 0xA5)
				return IsInRange(trail, 0xA1, 0xF6);
			if (lead == 0xA6)
				return IsInRange(trail, 0xA1, 0xB8)
					|| IsInRange(trail, 0xC1, 0xD8);
			if (lead == 0xA7)
				return IsInRange(trail, 0xA1, 0xC1)
					|| IsInRange(trail, 0xD1, 0xF1);
			if (lead == 0xA8)
				return IsInRange(trail, 0xA1, 0xBA)
					|| IsInRange(trail, 0xC5, 0xE9);
			if (lead == 0xA9)
				return IsInRange(trail, 0xA4, 0xEF);
			if (IsInRange(lead, 0xB0, 0xD6)
				|| IsInRange(lead, 0xD8, 0xF7))
			{
				return IsInRange(trail, 0xA1, 0xFE);
			}
			return lead == 0xD7 && IsInRange(trail, 0xA1, 0xF9);
		}

		bool IsCommonCodePageUnit(UInt32 codePage, UInt32 lead, UInt32 trail)
		{
			if (codePage == 936)
				return IsGb2312DoubleByteUnit(lead, trail);
			const char bytes[2] = {
				static_cast<char>(lead), static_cast<char>(trail)
			};
			UInt32 decoded = 0;
			return TryDecodeDoubleByteForCodePage(bytes, codePage, decoded);
		}

		bool HasCommonDoubleByteLimit(const PrewarmJob& job)
		{
			return job.mode == FontPrewarmMode::Common && job.codePage != 936;
		}

		constexpr bool ShouldPrewarmSdfMask(FontPrewarmMode mode,
			bool masksAvailable)
		{
			// Every unit selected by a blocking prewarm profile must carry every
			// SDF mask that runtime rendering can request. In particular, codepage
			// mode must not silently fall back to the old common-character limit
			// when the SDF is needed only by effects.
			return masksAvailable && mode != FontPrewarmMode::None;
		}

		static_assert(ShouldPrewarmSdfMask(FontPrewarmMode::CodePage, true),
			"codepage prewarm must include effect-only SDF masks");

		bool IsDcfgCodePageUnit(UInt32 codePage, UInt32 lead, UInt32 trail)
		{
			const UInt32 encoded = (lead << 8) | trail;
			switch (codePage)
			{
			case 936: // DCFGCF GB2312(true): complete GBK profile.
				if (IsInRange(lead, 0x81, 0xA0)
					&& IsInRange(trail, 0x40, 0xFE) && trail != 0x7F)
					return true;
				if (IsInRange(encoded, 0xA6A1, 0xA6B8)
					|| IsInRange(encoded, 0xA6C1, 0xA6D8)
					|| IsInRange(encoded, 0xA6E0, 0xA6EB)
					|| IsInRange(encoded, 0xA6EE, 0xA6F2)
					|| IsInRange(encoded, 0xA6F4, 0xA6F5)
					|| IsInRange(encoded, 0xA7A1, 0xA7C1)
					|| IsInRange(encoded, 0xA7D1, 0xA7F1)
					|| IsInRange(encoded, 0xA840, 0xA895)
					|| IsInRange(encoded, 0xA8A1, 0xA8BB)
					|| IsInRange(encoded, 0xA8BD, 0xA8BE)
					|| encoded == 0xA8C1
					|| IsInRange(encoded, 0xA8C5, 0xA8E9)
					|| IsInRange(encoded, 0xA940, 0xA957)
					|| IsInRange(encoded, 0xA959, 0xA95A)
					|| encoded == 0xA95C
					|| IsInRange(encoded, 0xA960, 0xA988)
					|| encoded == 0xA996)
					return trail != 0x7F;
				if (IsInRange(lead, 0xAA, 0xFC)
					&& IsInRange(trail, 0x40, 0xA0) && trail != 0x7F)
					return true;
				if (IsInRange(encoded, 0xFE40, 0xFE4F))
					return true;
				if (lead == 0xA1 && IsInRange(trail, 0xA1, 0xFE))
					return true;
				if (lead == 0xA3 && IsInRange(trail, 0xA1, 0xFE))
					return true;
				if (lead == 0xA4 && IsInRange(trail, 0xA1, 0xF3))
					return true;
				if (lead == 0xA5 && IsInRange(trail, 0xA1, 0xF6))
					return true;
				return IsInRange(lead, 0xB0, 0xF7)
					&& IsInRange(trail, 0xA1, 0xFE);

			case 950: // DCFGCF Big5().
				if (IsInRange(lead, 0xA1, 0xA2))
					return IsInRange(trail, 0x40, 0x7E)
						|| IsInRange(trail, 0xA1, 0xFE);
				if (lead == 0xA3)
					return IsInRange(trail, 0x40, 0x7E)
						|| IsInRange(trail, 0xA1, 0xBF) || trail == 0xE1;
				if (IsInRange(lead, 0xA4, 0xC5)
					|| IsInRange(lead, 0xC9, 0xF9))
					return IsInRange(trail, 0x40, 0x7E)
						|| IsInRange(trail, 0xA1, 0xFE);
				return lead == 0xC6 && IsInRange(trail, 0x40, 0x7E);

			case 932: // DCFGCF SJIS().
				if (lead == 0x81)
					return IsInRange(encoded, 0x8140, 0x81AC)
						|| IsInRange(encoded, 0x81B8, 0x81BF)
						|| IsInRange(encoded, 0x81C8, 0x81CE)
						|| IsInRange(encoded, 0x81DA, 0x81E8)
						|| IsInRange(encoded, 0x81F0, 0x81F7)
						|| encoded == 0x81FC;
				if (lead == 0x82)
					return IsInRange(encoded, 0x824F, 0x8258)
						|| IsInRange(encoded, 0x8260, 0x8279)
						|| IsInRange(encoded, 0x8281, 0x829A)
						|| IsInRange(encoded, 0x829F, 0x82F1);
				if (lead == 0x83)
					return IsInRange(encoded, 0x8340, 0x8396)
						|| IsInRange(encoded, 0x839F, 0x83B6)
						|| IsInRange(encoded, 0x83BF, 0x83D6);
				if (lead == 0x84)
					return IsInRange(encoded, 0x8440, 0x8460)
						|| IsInRange(encoded, 0x8470, 0x8491)
						|| IsInRange(encoded, 0x849F, 0x84BE);
				if (lead == 0x87)
					return IsInRange(encoded, 0x8740, 0x875D)
						|| IsInRange(encoded, 0x875F, 0x8775)
						|| encoded == 0x877E
						|| IsInRange(encoded, 0x8780, 0x879C);
				if (lead == 0x88)
					return IsInRange(encoded, 0x889F, 0x88FC);
				if (IsInRange(lead, 0x89, 0x9F)
					|| IsInRange(lead, 0xE0, 0xE9)
					|| IsInRange(lead, 0xFA, 0xFB))
					return IsInRange(trail, 0x40, 0x7E)
						|| IsInRange(trail, 0x80, 0xFC);
				if (lead == 0xEA)
					return IsInRange(trail, 0x40, 0x7E)
						|| IsInRange(trail, 0x80, 0xA4);
				if (lead == 0xED)
					return IsInRange(trail, 0x40, 0x7E)
						|| IsInRange(trail, 0x80, 0xEC)
						|| IsInRange(trail, 0xEF, 0xFC);
				if (lead == 0xEE)
					return IsInRange(trail, 0x40, 0x7E)
						|| IsInRange(trail, 0x80, 0xFC);
				return lead == 0xFC && IsInRange(trail, 0x40, 0x4B);

			case 949: // DCFGCF Korea().
				if (IsInRange(lead, 0x81, 0xC5))
					return IsInRange(trail, 0x41, 0x5A)
						|| IsInRange(trail, 0x60, 0x7A)
						|| IsInRange(trail, 0x81, 0xFE);
				if (lead == 0xC6)
					return IsInRange(trail, 0x41, 0x52)
						|| IsInRange(trail, 0xA1, 0xFE);
				return IsInRange(lead, 0xC7, 0xC8)
					&& IsInRange(trail, 0xA1, 0xFE);
			default:
				return false;
			}
		}

		bool NextEncodedUnit(PrewarmJob& job, std::array<char, 2>& bytes,
			size_t& length)
		{
			while (!job.scanningDoubleByte)
			{
				if (job.singleByte <= 0xFF)
				{
					bytes[0] = static_cast<char>(job.singleByte++);
					length = 1;
					return true;
				}
				job.scanningDoubleByte = true;
			}

			while (job.leadByte <= job.leadByteEnd)
			{
				while (job.trailByte <= 0xFE)
				{
					bytes[0] = static_cast<char>(job.leadByte);
					bytes[1] = static_cast<char>(job.trailByte++);
					const bool selected = job.mode == FontPrewarmMode::CodePage
						? IsDcfgCodePageUnit(job.codePage, job.leadByte,
							static_cast<UInt8>(bytes[1]))
						: IsCommonCodePageUnit(job.codePage, job.leadByte,
							static_cast<UInt8>(bytes[1]));
					if (selected)
					{
						length = 2;
						return true;
					}
				}
				++job.leadByte;
				job.trailByte = job.trailByteStart;
			}
			return false;
		}

		void AddBitmap(std::vector<std::shared_ptr<const GlyphBitmap>>& bitmaps,
			std::unordered_set<UInt64>& unique,
			const std::shared_ptr<const GlyphBitmap>& bitmap)
		{
			if (!bitmap || bitmap->width <= 0 || bitmap->height <= 0
				|| bitmap->alpha.empty() || !unique.insert(bitmap->cacheId).second)
			{
				return;
			}
			bitmaps.push_back(bitmap);
		}

		void FinishJob(const PrewarmJob& job, const char* status)
		{
			gLog.FormattedMessage(
				"tnvse_freetype_font: prewarm font=%u mode=%s scale=%.3f glyphs=%u doubleByte=%u sdfGlyphs=%u effectOnlySdfGlyphs=%u status=%s",
				job.fontId,
				job.mode == FontPrewarmMode::CodePage ? "codepage" : "common",
				job.rasterScaleMilli ? job.rasterScaleMilli / 1000.0f : 0.0f,
				job.rasterizedGlyphCount, job.validDoubleByteCount,
				job.sdfGlyphCount, job.effectOnlySdfGlyphCount, status);
		}

		void ResetPrewarmScan(PrewarmJob& job, UInt32 rasterScaleMilli)
		{
			job.singleByte = 0x20;
			job.leadByte = 0x81;
			job.leadByteEnd = 0xFE;
			job.trailByte = 0x40;
			job.trailByteStart = 0x40;
			job.validDoubleByteCount = 0;
			job.rasterizedGlyphCount = 0;
			job.sdfGlyphCount = 0;
			job.effectOnlySdfGlyphCount = 0;
			job.rasterScaleMilli = rasterScaleMilli;
			job.snapshotRevision = s_snapshotRevision;
			job.scanningDoubleByte = false;
			job.snapshotAttempted = false;
			for (size_t roleIndex = 0; roleIndex < job.atlasBitmaps.size(); ++roleIndex)
			{
				job.atlasBitmaps[roleIndex].clear();
				job.atlasBitmapIds[roleIndex].clear();
			}
			if (!IsDbcsCodePage(job.codePage))
				job.leadByteEnd = 0;
			ConfigureCommonRange(job);
		}

		UInt32 CountPrewarmDoubleByteUnits(const PrewarmJob& job)
		{
			UInt32 count = 0;
			for (UInt32 lead = job.leadByte; lead <= job.leadByteEnd; ++lead)
			{
				for (UInt32 trail = job.trailByteStart; trail <= 0xFE; ++trail)
				{
					const bool selected = job.mode == FontPrewarmMode::CodePage
						? IsDcfgCodePageUnit(job.codePage, lead, trail)
						: IsCommonCodePageUnit(job.codePage, lead, trail);
					if (selected && ++count >= kCommonDoubleByteLimit
						&& HasCommonDoubleByteLimit(job))
						return count;
				}
			}
			return count;
		}

		float GetPrewarmJobProgress(const PrewarmJob& job)
		{
			constexpr UInt32 singleByteUnits = 0x100 - 0x20;
			const UInt32 completedSingle = job.scanningDoubleByte
				? singleByteUnits
				: std::min(singleByteUnits,
					job.singleByte > 0x20 ? job.singleByte - 0x20 : 0);
			const UInt32 completed = completedSingle + job.validDoubleByteCount;
			return job.targetUnitCount
				? std::min(1.0f, static_cast<float>(completed) / job.targetUnitCount)
				: 0.0f;
		}

		void ReportPrewarmProgress(const PrewarmJob& job, UInt32 fontOrdinal,
			UInt32 fontCount, UInt32 finishedFonts, const wchar_t* stage,
			float minimumJobProgress = 0.0f)
		{
			wchar_t detail[160] = {};
			const FontConfig* config = FindConfig(job.fontId);
			const wchar_t* renderMode = config && UsesSdfFill(*config)
				? L"SDF" : L"grayscale";
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
	}

	void QueueFontPrewarm(UInt32 fontId)
	{
		const FontConfig* config = FindConfig(fontId);
		if (!config || config->prewarm == FontPrewarmMode::None)
			return;
		// Every configured font needs its own runtime even when its immutable
		// prewarm profile is shared with another font id. Tweak/JIP fonts may not
		// otherwise be initialized until their menu is first opened.
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
			const auto shared = std::find_if(
				s_jobs.begin(),
				s_jobs.end(),
				[config](const PrewarmJob& job) { return MatchesPrewarmProfile(job, *config); });
			if (shared != s_jobs.end())
			{
				const UInt32 ownerFontId = shared->fontId;
				const bool promoted = shared != std::prev(s_jobs.end());
				if (promoted)
				{
					PrewarmJob sharedJob = std::move(*shared);
					s_jobs.erase(shared);
					s_jobs.push_back(std::move(sharedJob));
				}
				gLog.FormattedMessage(
					"tnvse_freetype_font: prewarm profile alias font=%u owner=%u promoted=%u",
					fontId,
					ownerFontId,
					promoted ? 1u : 0u);
			}
			return;
		}
		PrewarmJob job;
		job.fontId = fontId;
		job.layoutHash = config->layoutHash;
		job.maskGenerationHash = config->maskGenerationHash;
		job.shaderEffectHash = config->shaderEffectHash;
		job.codePage = GetFreeTypeTextCodePage();
		job.mode = config->prewarm;
		ResetPrewarmScan(job, 0);
		job.targetUnitCount = (0x100 - 0x20) + CountPrewarmDoubleByteUnits(job);
		s_jobs.push_back(std::move(job));
		SetBitmapCacheReducedAfterPrewarm(false);
		gLog.FormattedMessage(
			"tnvse_freetype_font: queued prewarm font=%u mode=%s codePage=%u",
			fontId, config->prewarm == FontPrewarmMode::CodePage
				? "codepage" : "common", job.codePage);
	}

	void QueueConfiguredFontPrewarms()
	{
		if (s_configuredFontsQueued)
			return;
		s_configuredFontsQueued = true;

		// FontManager's eight base slots activate during startup, whereas the
		// extended slots owned by JIP/Tweaks are commonly lazy.  Enumerating the
		// validated configuration makes both kinds participate in the same
		// profile de-duplication and blocking startup prewarm.
		std::vector<UInt32> fontIds;
		fontIds.reserve(g_configs.size());
		for (const auto& entry : g_configs)
		{
			if (entry.second.prewarm != FontPrewarmMode::None)
				fontIds.push_back(entry.first);
		}
		std::sort(fontIds.begin(), fontIds.end());
		for (UInt32 fontId : fontIds)
			QueueFontPrewarm(fontId);
	}

	void PumpFontPrewarm()
	{
		if (!g_bEnableFreeTypeFontRendering)
			return;
		if (s_configuredFontsQueued && s_jobs.empty())
			return;

		const float rasterScale = GetCanonicalFreeTypeRasterScale();
		QueueConfiguredFontPrewarms();
		if (s_jobs.empty())
			return;
		const UInt32 rasterScaleMilli = static_cast<UInt32>(std::lround(
			rasterScale * 1000.0f));

		const UInt32 queuedFonts = static_cast<UInt32>(s_jobs.size());
		const ULONGLONG started = GetTickCount64();
		UInt32 batches = 0;
		UInt32 completedFonts = 0;
		UInt32 fullFonts = 0;
		UInt32 saveFailedFonts = 0;
		UInt32 rebuildFailedFonts = 0;
		UInt32 cancelledFonts = 0;
		UInt32 finishedFonts = 0;
		gLog.FormattedMessage(
			"tnvse_freetype_font: blocking prewarm begin fonts=%u scale=%.3f",
			queuedFonts, rasterScale);
		std::vector<VectorEncodedGlyph> requestedGlyphs;
		requestedGlyphs.reserve(kMaximumGlyphsPerBatch);
		std::vector<GlyphBitmapRequest> bitmapRequests;
		bitmapRequests.reserve(kMaximumGlyphsPerBatch * 4);
		std::vector<std::shared_ptr<const GlyphBitmap>> bitmapResults;
		bitmapResults.reserve(kMaximumGlyphsPerBatch * 4);

		// Restore every valid snapshot before creating the progress window. This
		// keeps the normal cache-hit startup path silent and ensures a mixed hit/
		// miss queue only displays progress while missing atlases are generated.
		std::deque<PrewarmJob> cacheMisses;
		while (!s_jobs.empty())
		{
			PrewarmJob job = std::move(s_jobs.front());
			s_jobs.pop_front();
			++batches;
			if (job.rasterScaleMilli != rasterScaleMilli)
				ResetPrewarmScan(job, rasterScaleMilli);
			const float rasterScale = job.rasterScaleMilli / 1000.0f;
			const FontConfig* config = FindConfig(job.fontId);
			RuntimeFont* runtime = FindRuntimeFont(job.fontId);
			if (!config || !runtime || config->layoutHash != job.layoutHash
				|| config->maskGenerationHash != job.maskGenerationHash
				|| config->shaderEffectHash != job.shaderEffectHash
				|| job.codePage != GetFreeTypeTextCodePage())
			{
				FinishJob(job, "cancelled");
				++cancelledFonts;
				++finishedFonts;
				continue;
			}
			job.snapshotAttempted = true;
			job.snapshotRevision = s_snapshotRevision;
			if (TryLoadGlyphAtlasSnapshot(*runtime, rasterScale))
			{
				FinishJob(job, "snapshot");
				++completedFonts;
				++finishedFonts;
				continue;
			}
			cacheMisses.push_back(std::move(job));
		}
		s_jobs.swap(cacheMisses);
		if (!s_jobs.empty())
		{
			StartPrewarmProgress();
			UpdatePrewarmProgress(L"Generating missing font cache...",
				L"Preparing glyphs...",
				static_cast<float>(finishedFonts) / queuedFonts);
		}

		// Deliberately drain the complete queue on the first game-loop callback
		// at the single configured source scale. This is an experimental
		// startup barrier: control does not return to the game until every queued
		// profile completes, reaches the atlas limit, or becomes invalid.
		while (!s_jobs.empty())
		{
			PrewarmJob job = std::move(s_jobs.front());
			s_jobs.pop_front();
			++batches;
			if (job.rasterScaleMilli != rasterScaleMilli)
				ResetPrewarmScan(job, rasterScaleMilli);
			const float rasterScale = job.rasterScaleMilli / 1000.0f;
			const FontConfig* config = FindConfig(job.fontId);
			RuntimeFont* runtime = FindRuntimeFont(job.fontId);
			const UInt32 fontOrdinal = std::min(queuedFonts, finishedFonts + 1);
			if (!config || !runtime || config->layoutHash != job.layoutHash
				|| config->maskGenerationHash != job.maskGenerationHash
				|| config->shaderEffectHash != job.shaderEffectHash
				|| job.codePage != GetFreeTypeTextCodePage())
			{
				FinishJob(job, "cancelled");
				++cancelledFonts;
				++finishedFonts;
				continue;
			}
			if (!job.snapshotAttempted || job.snapshotRevision != s_snapshotRevision)
			{
				job.snapshotAttempted = true;
				job.snapshotRevision = s_snapshotRevision;
				ReportPrewarmProgress(job, fontOrdinal, queuedFonts, finishedFonts,
					L"Checking cached atlas...");
				if (TryLoadGlyphAtlasSnapshot(*runtime, rasterScale))
				{
					FinishJob(job, "snapshot");
					++completedFonts;
					++finishedFonts;
					UpdatePrewarmProgress(L"Restored cached font atlas.",
						L"Preparing the next font...",
						static_cast<float>(finishedFonts) / queuedFonts);
					continue;
				}
			}

			if (job.atlasBitmaps[0].empty() && job.atlasBitmaps[1].empty())
			{
				const size_t expected = std::max<size_t>(4096,
					static_cast<size_t>(job.targetUnitCount));
				// Shader effect-only profiles retain the grayscale fill and add one
				// SDF mask per glyph. Reserve both complete code-page mask sets.
				job.atlasBitmaps[0].reserve(512);
				job.atlasBitmapIds[0].reserve(512);
				job.atlasBitmaps[1].reserve(expected * 2);
				job.atlasBitmapIds[1].reserve(expected * 2);
			}
			EffectQuality resolvedQuality = config->effectQuality;
			bool shaderEffects = IsA8RendererAvailable()
				&& (config->shadow.enabled || config->glow.enabled || config->outline.enabled
					|| UsesSdfFill(*config))
				&& ResolveA8EffectQuality(config->effectQuality, resolvedQuality);
			UInt32 sdfSpread = 0;
			const bool configuredNeedsSdf = NeedsSdfMask(*config);
			const bool needsSdf = shaderEffects && configuredNeedsSdf;
			if (needsSdf && !ResolveSdfSpread(*config, rasterScale, sdfSpread))
				shaderEffects = false;
			const bool resolvedSdfFill = shaderEffects && UsesSdfFill(*config);
			const bool prewarmSdfMasks = shaderEffects && configuredNeedsSdf;
			const bool hasCommonDoubleByteLimit = HasCommonDoubleByteLimit(job);
			// The shader path reuses an SDF fill as the exact source for a hard
			// shadow. Do not prewarm a second grayscale mask that runtime rendering
			// will never request. Effect-only profiles retain that grayscale fill,
			// but codepage mode now prewarms their SDF for every selected unit too.
			const bool needsGrayFill = !resolvedSdfFill;
			requestedGlyphs.clear();
			bitmapRequests.clear();
			UInt32 candidates = 0;
			UInt32 glyphs = 0;
			bool exhausted = false;
			while (candidates < kMaximumCandidatesPerBatch
				&& glyphs < kMaximumGlyphsPerBatch)
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
				{
					if (hasCommonDoubleByteLimit
						&& job.validDoubleByteCount >= kCommonDoubleByteLimit)
					{
						exhausted = true;
						break;
					}
					++job.validDoubleByteCount;
				}
				requestedGlyphs.push_back(glyph);
				const VectorEncodedGlyph* requestedGlyph = &requestedGlyphs.back();

				if (needsGrayFill)
					bitmapRequests.push_back({ requestedGlyph, GlyphMaskType::Fill, 0 });
				const bool prewarmSdf = ShouldPrewarmSdfMask(
					job.mode, prewarmSdfMasks);
				if (prewarmSdf)
				{
					bitmapRequests.push_back({ requestedGlyph,
						GlyphMaskType::DistanceField, sdfSpread });
					++job.sdfGlyphCount;
					if (!resolvedSdfFill)
						++job.effectOnlySdfGlyphCount;
				}
				if (config->glow.enabled && !shaderEffects)
					bitmapRequests.push_back({ requestedGlyph, GlyphMaskType::Glow, 0 });
				if (config->outline.enabled && !shaderEffects)
					bitmapRequests.push_back({ requestedGlyph, GlyphMaskType::Outline, 0 });
				++glyphs;
				++job.rasterizedGlyphCount;
			}
			GetPrewarmGlyphBitmaps(*runtime, bitmapRequests, rasterScale, bitmapResults);
			for (size_t index = 0; index < bitmapRequests.size()
				&& index < bitmapResults.size(); ++index)
			{
				if (bitmapRequests[index].glyph && bitmapResults[index]
					&& (bitmapRequests[index].maskType == GlyphMaskType::Fill
						|| bitmapRequests[index].maskType == GlyphMaskType::DistanceField))
				{
					StoreGlyphCollisionProfile(*runtime, *bitmapRequests[index].glyph,
						*bitmapResults[index], rasterScale);
				}
			}
			for (size_t index = 0; index < bitmapResults.size()
				&& index < bitmapRequests.size(); ++index)
			{
				if (!bitmapRequests[index].glyph)
					continue;
				const size_t roleIndex = static_cast<size_t>(
					bitmapRequests[index].glyph->byteClass);
				AddBitmap(job.atlasBitmaps[roleIndex], job.atlasBitmapIds[roleIndex],
					bitmapResults[index]);
			}
			const wchar_t* rasterStage = resolvedSdfFill
				? L"Generating SDF glyphs..."
				: prewarmSdfMasks ? L"Generating grayscale + SDF glyphs..."
					: L"Generating grayscale glyphs...";
			ReportPrewarmProgress(job, fontOrdinal, queuedFonts, finishedFonts,
				rasterStage);
			if (exhausted)
			{
				ReportPrewarmProgress(job, fontOrdinal, queuedFonts, finishedFonts,
					L"Packing glyph atlas...", 0.99f);
				bool atlasPacked = true;
				for (size_t roleIndex = 0; roleIndex < job.atlasBitmaps.size(); ++roleIndex)
				{
					if (!job.atlasBitmaps[roleIndex].empty()
						&& !PrewarmGlyphAtlas(*runtime,
							static_cast<VectorFontByteClass>(roleIndex),
							job.atlasBitmaps[roleIndex], rasterScale))
					{
						atlasPacked = false;
						break;
					}
				}
				if (!atlasPacked)
				{
					FinishJob(job, "atlas-full");
					++fullFonts;
					++finishedFonts;
					continue;
				}
				ReportPrewarmProgress(job, fontOrdinal, queuedFonts, finishedFonts,
					L"Saving atlas snapshot...", 0.995f);
				if (!SaveGlyphAtlasSnapshot(*runtime, rasterScale))
				{
					FinishJob(job, "atlas-save-failed");
					++saveFailedFonts;
					++finishedFonts;
					continue;
				}
				MarkGlyphManifestComplete(*runtime, job.mode);
				ReportPrewarmProgress(job, fontOrdinal, queuedFonts, finishedFonts,
					L"Rebuilding compact atlas...", 0.998f);
				if (!RebuildGlyphAtlasFromSnapshot(*runtime, rasterScale))
				{
					bool liveAtlasRecovered = true;
					for (size_t roleIndex = 0;
						roleIndex < job.atlasBitmaps.size(); ++roleIndex)
					{
						if (!job.atlasBitmaps[roleIndex].empty()
							&& !PrewarmGlyphAtlas(*runtime,
								static_cast<VectorFontByteClass>(roleIndex),
								job.atlasBitmaps[roleIndex], rasterScale))
						{
							liveAtlasRecovered = false;
						}
					}
					gLog.FormattedMessage(
						"tnvse_freetype_font: atlas post-prewarm rebuild recovery font=%u result=%s",
						job.fontId, liveAtlasRecovered ? "live-atlas" : "failed");
					FinishJob(job, "atlas-rebuild-failed");
					++rebuildFailedFonts;
					++finishedFonts;
					continue;
				}
				for (size_t roleIndex = 0; roleIndex < job.atlasBitmaps.size(); ++roleIndex)
				{
					std::vector<std::shared_ptr<const GlyphBitmap>>().swap(
						job.atlasBitmaps[roleIndex]);
					std::unordered_set<UInt64>().swap(job.atlasBitmapIds[roleIndex]);
				}
				++s_snapshotRevision;
				FinishJob(job, "complete");
				++completedFonts;
				++finishedFonts;
				UpdatePrewarmProgress(L"Rebuilt compact font atlas.",
					L"Preparing the next font...",
					static_cast<float>(finishedFonts) / queuedFonts);
				continue;
			}
			// Complete one font's page set before starting the next. Interleaving all
			// configured fonts can evict early pages from the bounded atlas cache before
			// their snapshot is serialized.
			s_jobs.push_front(std::move(job));
		}

		gLog.FormattedMessage(
			"tnvse_freetype_font: blocking prewarm end fonts=%u complete=%u atlasFull=%u saveFailed=%u rebuildFailed=%u cancelled=%u batches=%u elapsedMs=%llu",
			queuedFonts, completedFonts, fullFonts, saveFailedFonts, rebuildFailedFonts,
			cancelledFonts, batches,
			static_cast<unsigned long long>(GetTickCount64() - started));
		FlushGlyphBitmapDiskCache();
		ReleaseGlyphBitmapDiskCacheMappings();
		if (completedFonts == queuedFonts)
		{
			SetBitmapCacheReducedAfterPrewarm(true);
			EnforceCpuMemoryBudget("post-prewarm");
		}
		else
		{
			gLog.FormattedMessage(
				"tnvse_freetype_font: bitmap cache post-prewarm shrink skipped because prewarm did not complete successfully complete=%u queued=%u",
				completedFonts, queuedFonts);
		}
		if (g_bDeleteUnusedFreeTypeFontCache && completedFonts == queuedFonts)
			DeleteUnusedFreeTypeFontCacheFiles();
		else if (g_bDeleteUnusedFreeTypeFontCache)
		{
			gLog.FormattedMessage(
				"tnvse_freetype_font: unused persistent cache cleanup skipped because prewarm did not complete successfully");
		}
		if (s_progressThread)
		{
			UpdatePrewarmProgress(L"Font cache is ready.",
				L"Starting the game...", 1.0f);
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

#include "font_vector.h"

#include "load_config.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <string>
#include <utility>

namespace fonthook
{
	namespace
	{
		constexpr size_t kDeferredLogLineCount = 128;
		// Log mode is diagnostic, but it must not turn a menu that creates
		// hundreds of text artifacts into a single-frame synchronous disk burst.
		constexpr size_t kMaximumLogLinesPerFlush = 16;

		using DeferredLogLine = std::string;

		std::array<DeferredLogLine, kDeferredLogLineCount> s_deferredLogLines = {};
		std::mutex s_deferredLogMutex;
		std::atomic<bool> s_deferredLogPending = false;
		size_t s_deferredLogRead = 0;
		size_t s_deferredLogWrite = 0;
		size_t s_deferredLogCount = 0;
		size_t s_droppedLogCount = 0;
		bool s_firstLogFlush = true;
	}

	void FreeTypeFontDebugLog(const char* apFormat, ...)
	{
		if (!g_bEnableFreeTypeFontRenderingLog || !apFormat)
			return;

		va_list args;
		va_start(args, apFormat);
		va_list measureArgs;
		va_copy(measureArgs, args);
		const int requiredCharacters = _vscprintf(apFormat, measureArgs);
		va_end(measureArgs);
		if (requiredCharacters < 0)
		{
			va_end(args);
			return;
		}

		DeferredLogLine line(
			static_cast<size_t>(requiredCharacters) + 1, '\0');
		_vsnprintf_s(line.data(), line.size(), _TRUNCATE, apFormat, args);
		va_end(args);
		line.resize(static_cast<size_t>(requiredCharacters));

		std::lock_guard<std::mutex> lock(s_deferredLogMutex);
		if (s_deferredLogCount < s_deferredLogLines.size())
		{
			s_deferredLogLines[s_deferredLogWrite] = std::move(line);
			s_deferredLogWrite =
				(s_deferredLogWrite + 1) % s_deferredLogLines.size();
			++s_deferredLogCount;
		}
		else
		{
			++s_droppedLogCount;
		}
		s_deferredLogPending.store(true, std::memory_order_release);
	}

	void FlushFreeTypeFontDebugLog()
	{
		if (!s_deferredLogPending.load(std::memory_order_acquire))
			return;

		std::array<DeferredLogLine, kMaximumLogLinesPerFlush> batch = {};
		size_t batchCount = 0;
		size_t droppedCount = 0;
		{
			std::lock_guard<std::mutex> lock(s_deferredLogMutex);
			if (!g_bEnableFreeTypeFontRenderingLog)
			{
				for (DeferredLogLine& line : s_deferredLogLines)
					DeferredLogLine().swap(line);
				s_deferredLogRead = 0;
				s_deferredLogWrite = 0;
				s_deferredLogCount = 0;
				s_droppedLogCount = 0;
				s_deferredLogPending.store(false, std::memory_order_release);
				return;
			}
			batchCount = std::min(
				s_deferredLogCount, kMaximumLogLinesPerFlush);
			for (size_t index = 0; index < batchCount; ++index)
			{
				batch[index].swap(
					s_deferredLogLines[s_deferredLogRead]);
				s_deferredLogRead =
					(s_deferredLogRead + 1) % s_deferredLogLines.size();
			}
			s_deferredLogCount -= batchCount;
			droppedCount = s_droppedLogCount;
			s_droppedLogCount = 0;
			s_deferredLogPending.store(
				s_deferredLogCount != 0, std::memory_order_release);
		}

		if (!batchCount && !droppedCount)
			return;

		if (s_firstLogFlush)
		{
			gLog.FormattedMessage(
				"tnvse_freetype_font: flushing %u deferred startup diagnostics",
				static_cast<UInt32>(batchCount));
			s_firstLogFlush = false;
		}
		for (size_t index = 0; index < batchCount; ++index)
			gLog.Message(batch[index].c_str());
		if (droppedCount)
		{
			gLog.FormattedMessage(
				"tnvse_freetype_font: dropped %u deferred diagnostics because the fixed queue was full",
				static_cast<UInt32>(droppedCount));
		}
	}
}

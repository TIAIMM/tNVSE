#include "font_vector.h"

#include "load_config.h"

#include <array>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <mutex>

namespace fonthook
{
	namespace
	{
		constexpr size_t kDeferredLogLineCount = 128;
		constexpr size_t kDeferredLogLineLength = 1024;

		using DeferredLogLine = std::array<char, kDeferredLogLineLength>;

		std::array<DeferredLogLine, kDeferredLogLineCount> s_deferredLogLines = {};
		std::mutex s_deferredLogMutex;
		size_t s_deferredLogCount = 0;
		size_t s_droppedLogCount = 0;
		bool s_firstLogFlush = true;
	}

	void FreeTypeFontDebugLog(const char* apFormat, ...)
	{
		if (!g_bEnableFreeTypeFontRenderingLog || !apFormat)
			return;

		DeferredLogLine line = {};
		va_list args;
		va_start(args, apFormat);
		_vsnprintf_s(line.data(), line.size(), _TRUNCATE, apFormat, args);
		va_end(args);

		std::lock_guard<std::mutex> lock(s_deferredLogMutex);
		if (s_deferredLogCount < s_deferredLogLines.size())
		{
			s_deferredLogLines[s_deferredLogCount++] = line;
		}
		else
		{
			++s_droppedLogCount;
		}
	}

	void FlushFreeTypeFontDebugLog()
	{
		std::lock_guard<std::mutex> lock(s_deferredLogMutex);
		if (!g_bEnableFreeTypeFontRenderingLog)
		{
			s_deferredLogCount = 0;
			s_droppedLogCount = 0;
			return;
		}
		if (!s_deferredLogCount && !s_droppedLogCount)
			return;

		if (s_firstLogFlush)
		{
			gLog.FormattedMessage(
				"tnvse_freetype_font: flushing %u deferred startup diagnostics",
				static_cast<UInt32>(s_deferredLogCount));
			s_firstLogFlush = false;
		}
		for (size_t i = 0; i < s_deferredLogCount; ++i)
			gLog.Message(s_deferredLogLines[i].data());
		if (s_droppedLogCount)
		{
			gLog.FormattedMessage(
				"tnvse_freetype_font: dropped %u startup diagnostics because the fixed queue was full",
				static_cast<UInt32>(s_droppedLogCount));
		}
		s_deferredLogCount = 0;
		s_droppedLogCount = 0;
	}
}

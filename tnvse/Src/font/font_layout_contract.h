#pragma once

#include <cmath>
#include <cstdint>

namespace fonthook
{
	enum class FreeTypeBreakKind : std::uint8_t
	{
		None,
		Whitespace,
		SoftHyphen
	};

	struct FreeTypeBreakOpportunity
	{
		FreeTypeBreakKind kind = FreeTypeBreakKind::None;
		std::uint32_t outputPosition = 0;
		std::uint32_t sourceConsumedEnd = 0;
		double prefixWidth = 0.0;
		double consumedWidth = 0.0;

		std::uint32_t GetCompletedLineSourceEnd(
			std::uint32_t scannedSourceEnd) const
		{
			return kind == FreeTypeBreakKind::None
				? scannedSourceEnd : sourceConsumedEnd;
		}

		void Clear()
		{
			kind = FreeTypeBreakKind::None;
			outputPosition = 0;
			sourceConsumedEnd = 0;
			prefixWidth = 0.0;
			consumedWidth = 0.0;
		}
	};

	// Font::MakeString in the original executable advances tabs against the
	// absolute local X coordinate, including its signed fmod behaviour.
	inline double GetOriginalAbsoluteTabStop(double currentX, double tabWidth)
	{
		return currentX + tabWidth - std::fmod(currentX, tabWidth);
	}
}

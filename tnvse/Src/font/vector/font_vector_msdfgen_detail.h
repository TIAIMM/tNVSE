#pragma once

#include "font_vector_msdfgen.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

#include <msdfgen.h>
#include <core/ShapeDistanceFinder.h>

#ifndef MSDFGEN_USE_CPP11
#error "true-SDF repair budgets require move-enabled msdfgen Scanline vectors"
#endif

namespace fonthook::vectorfont::implementation::font_vector_msdfgen
{
	inline constexpr int kTrueSdfRepairDistanceSamplesPerAxis = 4;
	inline constexpr int kTrueSdfRepairSignSamplesPerAxis = 8;
	inline constexpr int kTrueSdfRepairScanlinesPerRow = 32;
	inline constexpr double kTrueSdfRepairSevereDistanceError = 0.125;
	inline constexpr double kTrueSdfRepairErrorTolerance = 1e-10;
	inline constexpr std::array<double, 4> kTrueSdfRepairAntialiasWidths = {
		0.25, 0.5, 0.75, 1.0
	};
	inline constexpr std::size_t kMaximumTrueSdfRepairPixels = 65536;
	inline constexpr int kMaximumTrueSdfRepairWidth = 1024;
	inline constexpr int kMaximumTrueSdfRepairEdges = 512;
	inline constexpr std::uint64_t kMaximumTrueSdfRepairEdgeTests = 64000000;
	inline constexpr std::uint64_t kMaximumTrueSdfRepairScanlines = 2000000;
	inline constexpr std::uint64_t kMaximumTrueSdfRepairSortWork = 64000000;
	inline constexpr std::size_t kMaximumTrueSdfContinuousCandidates = 1024;
	inline constexpr std::size_t kMaximumTrueSdfActiveCells = 2048;
	inline constexpr std::size_t kMaximumTrueSdfRepairCandidateTrials = 256;
	inline constexpr std::uint64_t kMaximumTrueSdfShapeWidthWork = 8000000;
	inline constexpr std::size_t kTrueSdfRepairDynamicBudgetBytes = 64u * 1024u;

	inline constexpr int kMtsdfRepairDistanceSamplesPerAxis = 4;
	inline constexpr int kMtsdfRepairSignSamplesPerAxis = 8;
	inline constexpr int kMtsdfRepairScanlinesPerRow = 32;
	inline constexpr double kMtsdfRepairDeficitThreshold = 0.125;
	inline constexpr double kMtsdfRepairAlphaPrefilterMargin = 0.5;
	inline constexpr double kMtsdfRepairErrorTolerance = 1e-12;
	inline constexpr std::size_t kMaximumMtsdfRepairPixels = 65536;
	inline constexpr int kMaximumMtsdfRepairEdges = 1024;
	inline constexpr std::size_t kMaximumMtsdfRescueContours = 1024;
	inline constexpr std::uint64_t kMaximumMtsdfRepairEdgeTests = 64000000;
	inline constexpr std::size_t kMaximumMtsdfRepairCandidateTrials = 64;
	inline constexpr int kMtsdfRescueDenseSamplesPerAxis = 8;
	inline constexpr int kMtsdfRescueAuditSamplesPerAxis = 16;
	inline constexpr std::array<double, 4> kMtsdfRescueAntialiasWidths = {
		0.25, 0.5, 0.75, 1.0
	};
	inline constexpr std::size_t kMaximumMtsdfRescueActiveCells =
		4u * kMaximumMtsdfRepairCandidateTrials;
	inline constexpr int kMaximumMtsdfRescueFieldWidth = 4096;
	inline constexpr std::uint64_t kMaximumMtsdfRescueScanlineEdgeTests = 64000000;
	inline constexpr std::uint64_t kMaximumMtsdfRescueFieldScanWork = 64000000;
	inline constexpr std::uint64_t kMaximumMtsdfRescueSortWork = 64000000;
	inline constexpr std::size_t kMtsdfRescueDynamicBudgetMarginBytes =
		128u * 1024u;

	using ExactShapeDistanceFinder = msdfgen::ShapeDistanceFinder<
		msdfgen::OverlappingContourCombiner<msdfgen::TrueDistanceSelector>>;

	static_assert(sizeof(msdfgen::Scanline::Intersection) == 16,
		"v145 msdfgen scanline-intersection layout changed");
	static_assert(sizeof(msdfgen::TrueDistanceSelector::EdgeCache) == 24,
		"v145 msdfgen true-distance edge cache layout changed");
	static_assert(sizeof(msdfgen::TrueDistanceSelector) == 32,
		"v145 msdfgen true-distance selector layout changed");

	struct MtsdfRepairErrors
	{
		std::size_t falseOutside = 0;
		std::size_t falseInside = 0;
		std::size_t severeDistanceErrors = 0;
		std::size_t sampleCount = 0;
		double absoluteDistanceErrorSum = 0.0;
		double maximumAbsoluteDistanceError = 0.0;
	};

	struct MtsdfRepairComparison
	{
		MtsdfRepairErrors before;
		MtsdfRepairErrors after;
		bool everySampleSafe = true;
		bool preservesCorrectSigns = true;
		bool finite = true;
	};

	struct MtsdfRepairSignComparison
	{
		std::size_t beforeFalseOutside = 0;
		std::size_t beforeFalseInside = 0;
		std::size_t afterFalseOutside = 0;
		std::size_t afterFalseInside = 0;
		bool preservesCorrectSigns = true;
		bool finite = true;
	};

	float MedianMtsdfRgb(float red, float green, float blue);
	bool IsEncodableMtsdfPixel(const float* rgba);
	bool PrepareShape(FT_Outline& outline, msdfgen::Shape& shape);
	bool EqualizeMtsdfRgbToOwnMedian(float* rgb);
	MtsdfRepairComparison CompareMtsdfRepairCandidate(
		const msdfgen::Bitmap<float, 4>& field,
		const msdfgen::Shape& shape,
		const msdfgen::Projection& projection,
		msdfgen::FillRule fillRule,
		std::uint8_t spread,
		ExactShapeDistanceFinder& distanceFinder,
		int texelX, int texelY, const float equalized[3]);
	MtsdfRepairSignComparison CompareMtsdfRepairCandidateSigns(
		const msdfgen::Bitmap<float, 4>& field,
		const msdfgen::Shape& shape,
		const msdfgen::Projection& projection,
		msdfgen::FillRule fillRule,
		int texelX, int texelY, const float equalized[3]);
	double EstimateMtsdfAffectedRowsError(
		const msdfgen::Bitmap<float, 4>& field,
		const msdfgen::Shape& shape,
		const msdfgen::Projection& projection,
		msdfgen::FillRule fillRule,
		int texelY);
	void RepairMtsdfRgbInterpolationDeficits(
		msdfgen::Bitmap<float, 4>& field,
		const msdfgen::Shape& shape,
		const msdfgen::Projection& projection,
		msdfgen::FillRule fillRule, std::uint8_t spread);
	void RescueMtsdfRgbInterpolationDeficits(
		msdfgen::Bitmap<float, 4>& field,
		const msdfgen::Shape& shape,
		const msdfgen::Projection& projection,
		msdfgen::FillRule fillRule, std::uint8_t spread) noexcept;
	void RepairTrueSdfQuantization(
		std::vector<std::uint8_t>& bytes,
		msdfgen::Bitmap<float, 1>& continuousField,
		const msdfgen::Shape& shape,
		const msdfgen::Projection& projection,
		msdfgen::FillRule fillRule, std::uint8_t spread) noexcept;
	bool ResolveFieldBounds(const msdfgen::Shape& shape,
		std::uint8_t spread, std::size_t maximumBytes,
		std::uint32_t bytesPerPixel, int& fieldLeft, int& fieldBottom,
		int& fieldTop, int& fieldWidth, int& fieldHeight);
}

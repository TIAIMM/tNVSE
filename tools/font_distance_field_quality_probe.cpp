#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_OUTLINE_H

#include <msdfgen.h>
#include <ext/import-font.h>
#include <core/bitmap-interpolation.hpp>
#include <core/rasterization.h>
#include <core/sdf-error-estimation.h>
#include <core/ShapeDistanceFinder.h>

#include "../tnvse/Src/font/vector/font_vector_msdfgen.h"

namespace
{
	constexpr double kCornerAngleThreshold = 3.0;
	constexpr int kScanlinesPerRow = 32;
	constexpr int kPreviewScale = 8;
	int gShapeSamplesPerAxis = 8;

	enum class Coloring
	{
		Simple,
		InkTrap,
		ByDistance,
	};

	enum class Correction
	{
		Fast,
		EdgePriority105,
		EdgePriority102,
		EdgePriority101,
		EdgePriority100,
		IndiscriminateFast,
		EdgeOnlyFast,
		Mixed,
		PostQuantControl,
		AlphaCopyExperiment,
		ExactShapeRepairReference,
		ExactShapeRepair,
	};

	struct ShapeGridErrors
	{
		std::size_t falseOutside = 0;
		std::size_t falseInside = 0;
		std::size_t severeDistanceErrors = 0;
		std::size_t sampleCount = 0;
		double absoluteDistanceErrorSum = 0.0;
		double maximumAbsoluteDistanceError = 0.0;
	};

	using ExactShapeDistanceFinder = msdfgen::ShapeDistanceFinder<
		msdfgen::OverlappingContourCombiner<msdfgen::TrueDistanceSelector>>;

	struct GlyphCase
	{
		std::filesystem::path fontPath;
		std::string label;
		std::uint32_t codePoint = 0;
		FT_UInt pixelWidth = 0;
		FT_UInt pixelHeight = 0;
		std::uint8_t spread = 4;
		double previewScale = 1.0;
		bool writePreviews = true;
		bool compareColorings = false;
	};

	struct Bounds
	{
		int left = 0;
		int bottom = 0;
		int top = 0;
		int width = 0;
		int height = 0;
	};

	struct MtsdfResult
	{
		msdfgen::Bitmap<float, 4> field;
		msdfgen::Shape shape;
		msdfgen::Projection projection;
		msdfgen::FillRule fillRule = msdfgen::FILL_NONZERO;
		Bounds bounds;
		double error = 0.0;
		double alphaError = 0.0;
		std::size_t centerSignErrors = 0;
		double maximumCoverageDeficit = 0.0;
		double severeCoverageDeficitFraction = 0.0;
		double meanAbsoluteCoverageDifference = 0.0;
		double rgbAlphaSignDisagreementFraction = 0.0;
		std::size_t modifiedTexelCount = 0;
		std::size_t candidateTexelCount = 0;
		std::size_t phase2ModifiedTexelCount = 0;
		std::size_t phase2CandidateTexelCount = 0;
		std::size_t phase2NoRescueByteParityFailures = 0;
		std::size_t alphaByteChanges = 0;
		std::size_t falseOutsideSamples = 0;
		std::size_t falseInsideSamples = 0;
		double exactDistanceMae = 0.0;
		double exactSevereDistanceFraction = 0.0;
		double exactMaximumDistanceError = 0.0;
		bool exactShapeRepairFallback = false;
		bool phase2Fallback = false;
		double milliseconds = 0.0;
	};

	struct SdfResult
	{
		msdfgen::Bitmap<float, 1> field;
		msdfgen::Shape shape;
		msdfgen::Projection projection;
		msdfgen::FillRule fillRule = msdfgen::FILL_NONZERO;
		Bounds bounds;
		double error = 0.0;
		std::size_t centerSignErrors = 0;
		std::size_t modifiedTexelCount = 0;
		std::size_t candidateTexelCount = 0;
		std::size_t falseOutsideSamples = 0;
		std::size_t falseInsideSamples = 0;
		double exactDistanceMae = 0.0;
		double exactSevereDistanceFraction = 0.0;
		double exactMaximumDistanceError = 0.0;
		bool repairFallback = false;
		double milliseconds = 0.0;
	};

	enum class TsdfQuantization
	{
		Runtime255,
		Runtime256,
		Runtime256Repaired,
	};

	std::uint8_t QuantizeTsdf(float encoded, TsdfQuantization quantization)
	{
		const double clamped = std::clamp(
			static_cast<double>(encoded), 0.0, 1.0);
		if (quantization == TsdfQuantization::Runtime255)
			return msdfgen::pixelFloatToByte(static_cast<float>(clamped));
		const long rounded = std::lround(256.0 * clamped);
		return static_cast<std::uint8_t>(std::min<long>(rounded, 255));
	}

	constexpr double kTsdfReferenceTolerance = 1e-10;
	constexpr double kTsdfReferenceSevereDistanceError = 0.125;
	constexpr std::array<double, 4> kTsdfReferenceAntialiasWidths = {
		0.25, 0.5, 0.75, 1.0
	};
	constexpr std::size_t kTsdfReferenceMaximumPixels = 65536;
	constexpr int kTsdfReferenceMaximumWidth = 1024;
	constexpr int kTsdfReferenceMaximumEdges = 512;
	constexpr std::uint64_t kTsdfReferenceMaximumEdgeTests = 64000000;
	constexpr std::uint64_t kTsdfReferenceMaximumScanlines = 2000000;
	constexpr std::uint64_t kTsdfReferenceMaximumSortWork = 64000000;
	constexpr std::uint64_t kTsdfReferenceMaximumShapeWidthWork = 8000000;
	constexpr std::size_t kTsdfReferenceMaximumContinuousCandidates = 1024;
	constexpr std::size_t kTsdfReferenceMaximumActiveCells = 2048;
	constexpr std::size_t kTsdfReferenceMaximumTrials = 256;

	struct TsdfReferenceGrid
	{
		int samplesPerAxis = 0;
		int cellsWide = 0;
		int cellsHigh = 0;
		std::vector<std::uint8_t> activeCells;
		std::vector<float> signedDistances;
		std::vector<std::uint8_t> inside;

		std::size_t CellIndex(int cellX, int cellY) const
		{
			return static_cast<std::size_t>(cellY) * cellsWide + cellX;
		}

		std::size_t Index(int cellX, int cellY, int sampleX, int sampleY) const
		{
			return ((CellIndex(cellX, cellY) * samplesPerAxis + sampleY)
				* samplesPerAxis + sampleX);
		}
	};

	struct TsdfReferenceMetrics
	{
		std::uint64_t samples = 0;
		std::uint64_t falseOutside = 0;
		std::uint64_t falseInside = 0;
		std::uint64_t severeDistanceErrors = 0;
		double absoluteDistanceError = 0.0;
		double maximumDistanceError = 0.0;
		std::array<double, kTsdfReferenceAntialiasWidths.size()>
			absoluteCoverageError = {};
		std::array<double, kTsdfReferenceAntialiasWidths.size()>
			maximumCoverageError = {};
	};

	struct TsdfReferenceSignMetrics
	{
		std::uint64_t falseOutside = 0;
		std::uint64_t falseInside = 0;
	};

	struct TsdfReferenceCandidate
	{
		std::uint32_t pixelIndex = 0;
		int delta = 0;
		bool centerInside = false;
		double score = 0.0;
	};

	struct TsdfReferenceRepairStats
	{
		std::size_t candidates = 0;
		std::size_t accepted = 0;
		bool fallback = false;
	};

	double ClampTsdfReferenceDistance(double distance, std::uint8_t spread)
	{
		return std::clamp(distance, -static_cast<double>(spread),
			static_cast<double>(spread));
	}

	std::uint64_t CeilTsdfReferenceLog2(std::uint64_t value)
	{
		if (value <= 1)
			return 0;
		std::uint64_t exponent = 0;
		for (--value; value; value >>= 1)
			++exponent;
		return exponent;
	}

	bool TsdfReferenceSortWorkWithinBudget(std::uint64_t shapeScanlines,
		std::uint64_t fieldShapeRows, int width, int edgeCount)
	{
		if (width <= 0 || edgeCount <= 0)
			return false;
		const std::uint64_t shapeItems = 3u
			* static_cast<std::uint64_t>(edgeCount);
		const std::uint64_t shapeLog = CeilTsdfReferenceLog2(shapeItems);
		const std::uint64_t fieldItems = static_cast<std::uint64_t>(width);
		const std::uint64_t fieldLog = CeilTsdfReferenceLog2(fieldItems);
		if ((shapeLog && shapeItems
				> kTsdfReferenceMaximumSortWork / shapeLog)
			|| (fieldLog && fieldItems
				> kTsdfReferenceMaximumSortWork / fieldLog))
		{
			return false;
		}
		const std::uint64_t shapeWorkPerScanline = shapeItems * shapeLog;
		const std::uint64_t fieldWorkPerRow = fieldItems * fieldLog;
		if ((shapeWorkPerScanline && shapeScanlines
				> kTsdfReferenceMaximumSortWork / shapeWorkPerScanline)
			|| (fieldWorkPerRow && fieldShapeRows
				> kTsdfReferenceMaximumSortWork / fieldWorkPerRow))
		{
			return false;
		}
		const std::uint64_t shapeWork = shapeScanlines * shapeWorkPerScanline;
		const std::uint64_t fieldWork = fieldShapeRows * fieldWorkPerRow;
		return fieldWork <= kTsdfReferenceMaximumSortWork - shapeWork;
	}

	double DecodeTsdfReferenceByte(double byteValue, std::uint8_t spread)
	{
		return ClampTsdfReferenceDistance(
			(byteValue / 128.0 - 1.0) * spread, spread);
	}

	double TsdfReferenceCoverage(double distance, double antialiasWidth)
	{
		return std::clamp(0.5 + distance / (2.0 * antialiasWidth), 0.0, 1.0);
	}

	double BilinearTsdfReferenceByte(const std::vector<std::uint8_t>& bytes,
		int width, int cellX, int cellY, double fractionX, double fractionY)
	{
		const auto at = [&](int x, int y)
		{
			return static_cast<double>(bytes[static_cast<std::size_t>(y)
				* width + x]);
		};
		return std::lerp(
			std::lerp(at(cellX, cellY), at(cellX + 1, cellY), fractionX),
			std::lerp(at(cellX, cellY + 1), at(cellX + 1, cellY + 1),
				fractionX), fractionY);
	}

	double BilinearTsdfReferenceCandidateByte(
		const std::vector<std::uint8_t>& bytes, int width,
		int cellX, int cellY, double fractionX, double fractionY,
		int texelX, int texelY, int delta)
	{
		const auto at = [&](int x, int y)
		{
			const int value = bytes[static_cast<std::size_t>(y) * width + x];
			return static_cast<double>(value
				+ (x == texelX && y == texelY ? delta : 0));
		};
		return std::lerp(
			std::lerp(at(cellX, cellY), at(cellX + 1, cellY), fractionX),
			std::lerp(at(cellX, cellY + 1), at(cellX + 1, cellY + 1),
				fractionX), fractionY);
	}

	bool AccumulateTsdfReferenceSample(TsdfReferenceMetrics& metrics,
		double exactDistance, double decodedDistance, bool expectedInside)
	{
		const double error = std::abs(exactDistance - decodedDistance);
		if (!std::isfinite(exactDistance) || !std::isfinite(decodedDistance)
			|| !std::isfinite(error))
		{
			return false;
		}
		++metrics.samples;
		metrics.absoluteDistanceError += error;
		metrics.maximumDistanceError = std::max(metrics.maximumDistanceError, error);
		metrics.severeDistanceErrors += error > kTsdfReferenceSevereDistanceError;
		const bool actualInside = decodedDistance > 0.0;
		metrics.falseOutside += expectedInside && !actualInside;
		metrics.falseInside += !expectedInside && actualInside;
		for (std::size_t index = 0;
			index < kTsdfReferenceAntialiasWidths.size(); ++index)
		{
			const double coverageError = std::abs(
				TsdfReferenceCoverage(exactDistance,
					kTsdfReferenceAntialiasWidths[index])
				- TsdfReferenceCoverage(decodedDistance,
					kTsdfReferenceAntialiasWidths[index]));
			if (!std::isfinite(coverageError))
				return false;
			metrics.absoluteCoverageError[index] += coverageError;
			metrics.maximumCoverageError[index] = std::max(
				metrics.maximumCoverageError[index], coverageError);
		}
		return std::isfinite(metrics.absoluteDistanceError);
	}

	bool TsdfReferenceMetricsDoNotRegress(const TsdfReferenceMetrics& before,
		const TsdfReferenceMetrics& after)
	{
		if (after.falseOutside > before.falseOutside
			|| after.falseInside > before.falseInside
			|| after.severeDistanceErrors > before.severeDistanceErrors
			|| after.maximumDistanceError
				> before.maximumDistanceError + kTsdfReferenceTolerance
			|| after.absoluteDistanceError
				> before.absoluteDistanceError + kTsdfReferenceTolerance)
		{
			return false;
		}
		for (std::size_t index = 0;
			index < kTsdfReferenceAntialiasWidths.size(); ++index)
		{
			if (after.absoluteCoverageError[index]
					> before.absoluteCoverageError[index] + kTsdfReferenceTolerance
				|| after.maximumCoverageError[index]
					> before.maximumCoverageError[index] + kTsdfReferenceTolerance)
			{
				return false;
			}
		}
		return true;
	}

	bool MarkTsdfReferenceCells(std::vector<std::uint8_t>& activeCells,
		int width, int height, int texelX, int texelY,
		std::size_t& activeCellCount)
	{
		const int cellsWide = width - 1;
		for (int cellY = std::max(texelY - 1, 0);
			cellY <= std::min(texelY, height - 2); ++cellY)
		{
			for (int cellX = std::max(texelX - 1, 0);
				cellX <= std::min(texelX, width - 2); ++cellX)
			{
				std::uint8_t& active = activeCells[static_cast<std::size_t>(cellY)
					* cellsWide + cellX];
				if (active)
					continue;
				if (activeCellCount >= kTsdfReferenceMaximumActiveCells)
					return false;
				active = 1;
				++activeCellCount;
			}
		}
		return true;
	}

	bool BuildTsdfReferenceExactGrid(const msdfgen::Shape& shape,
		const msdfgen::Projection& projection, msdfgen::FillRule fillRule,
		int width, int height, std::uint8_t spread, int samplesPerAxis,
		const std::vector<std::uint8_t>& activeCells, TsdfReferenceGrid& grid)
	{
		if (width < 2 || height < 2 || samplesPerAxis < 1
			|| activeCells.size() != static_cast<std::size_t>(width - 1)
				* (height - 1))
		{
			return false;
		}
		grid.samplesPerAxis = samplesPerAxis;
		grid.cellsWide = width - 1;
		grid.cellsHigh = height - 1;
		grid.activeCells = activeCells;
		const std::size_t sampleCount = activeCells.size()
			* samplesPerAxis * samplesPerAxis;
		grid.signedDistances.assign(sampleCount, 0.0f);
		grid.inside.assign(sampleCount, 0);
		ExactShapeDistanceFinder distanceFinder(shape);
		for (int cellY = 0; cellY < grid.cellsHigh; ++cellY)
		{
			bool activeRow = false;
			for (int cellX = 0; cellX < grid.cellsWide; ++cellX)
				activeRow |= activeCells[grid.CellIndex(cellX, cellY)] != 0;
			if (!activeRow)
				continue;
			for (int sampleY = 0; sampleY < samplesPerAxis; ++sampleY)
			{
				const double fractionY = (sampleY + 0.5) / samplesPerAxis;
				const double fieldY = cellY + 0.5 + fractionY;
				const double shapeY = projection.unprojectY(fieldY);
				if (!std::isfinite(shapeY))
					return false;
				msdfgen::Scanline scanline;
				shape.scanline(scanline, shapeY);
				for (int cellX = 0; cellX < grid.cellsWide; ++cellX)
				{
					if (!activeCells[grid.CellIndex(cellX, cellY)])
						continue;
					for (int sampleX = 0; sampleX < samplesPerAxis; ++sampleX)
					{
						const double fractionX = (sampleX + 0.5) / samplesPerAxis;
						const double fieldX = cellX + 0.5 + fractionX;
						const double shapeX = projection.unprojectX(fieldX);
						if (!std::isfinite(shapeX))
							return false;
						const bool inside = scanline.filled(shapeX, fillRule);
						const double rawDistance = distanceFinder.distance(
							projection.unproject(msdfgen::Point2(fieldX, fieldY)));
						if (!std::isfinite(rawDistance))
							return false;
						const double unsignedDistance = std::min<double>(
							spread, std::abs(rawDistance));
						const std::size_t index = grid.Index(
							cellX, cellY, sampleX, sampleY);
						grid.signedDistances[index] = static_cast<float>(
							inside ? unsignedDistance : -unsignedDistance);
						grid.inside[index] = static_cast<std::uint8_t>(inside);
					}
				}
			}
		}
		return true;
	}

	bool BuildTsdfReferenceSignGrid(const msdfgen::Shape& shape,
		const msdfgen::Projection& projection, msdfgen::FillRule fillRule,
		int width, int height, int samplesPerAxis,
		const std::vector<std::uint8_t>& activeCells, TsdfReferenceGrid& grid)
	{
		grid.samplesPerAxis = samplesPerAxis;
		grid.cellsWide = width - 1;
		grid.cellsHigh = height - 1;
		grid.activeCells = activeCells;
		grid.inside.assign(activeCells.size() * samplesPerAxis
			* samplesPerAxis, 0);
		grid.signedDistances.clear();
		for (int cellY = 0; cellY < grid.cellsHigh; ++cellY)
		{
			bool activeRow = false;
			for (int cellX = 0; cellX < grid.cellsWide; ++cellX)
				activeRow |= activeCells[grid.CellIndex(cellX, cellY)] != 0;
			if (!activeRow)
				continue;
			for (int sampleY = 0; sampleY < samplesPerAxis; ++sampleY)
			{
				const double fractionY = (sampleY + 0.5) / samplesPerAxis;
				const double shapeY = projection.unprojectY(
					cellY + 0.5 + fractionY);
				if (!std::isfinite(shapeY))
					return false;
				msdfgen::Scanline scanline;
				shape.scanline(scanline, shapeY);
				for (int cellX = 0; cellX < grid.cellsWide; ++cellX)
				{
					if (!activeCells[grid.CellIndex(cellX, cellY)])
						continue;
					for (int sampleX = 0; sampleX < samplesPerAxis; ++sampleX)
					{
						const double shapeX = projection.unprojectX(cellX + 0.5
							+ (sampleX + 0.5) / samplesPerAxis);
						if (!std::isfinite(shapeX))
							return false;
						grid.inside[grid.Index(cellX, cellY,
							sampleX, sampleY)] = static_cast<std::uint8_t>(
							scanline.filled(shapeX, fillRule));
					}
				}
			}
		}
		return true;
	}

	bool MeasureTsdfReferenceGrid(const std::vector<std::uint8_t>& bytes,
		int width, std::uint8_t spread, const TsdfReferenceGrid& grid,
		TsdfReferenceMetrics& metrics)
	{
		metrics = {};
		for (int cellY = 0; cellY < grid.cellsHigh; ++cellY)
		{
			for (int cellX = 0; cellX < grid.cellsWide; ++cellX)
			{
				if (!grid.activeCells[grid.CellIndex(cellX, cellY)])
					continue;
				for (int sampleY = 0; sampleY < grid.samplesPerAxis; ++sampleY)
				{
					const double fractionY = (sampleY + 0.5)
						/ grid.samplesPerAxis;
					for (int sampleX = 0;
						sampleX < grid.samplesPerAxis; ++sampleX)
					{
						const double fractionX = (sampleX + 0.5)
							/ grid.samplesPerAxis;
						const std::size_t index = grid.Index(
							cellX, cellY, sampleX, sampleY);
						if (!AccumulateTsdfReferenceSample(metrics,
							grid.signedDistances[index],
							DecodeTsdfReferenceByte(BilinearTsdfReferenceByte(
								bytes, width, cellX, cellY,
								fractionX, fractionY), spread),
							grid.inside[index] != 0))
						{
							return false;
						}
					}
				}
			}
		}
		return true;
	}

	bool MeasureTsdfReferenceSigns(const std::vector<std::uint8_t>& bytes,
		int width, std::uint8_t spread, const TsdfReferenceGrid& grid,
		TsdfReferenceSignMetrics& metrics)
	{
		metrics = {};
		for (int cellY = 0; cellY < grid.cellsHigh; ++cellY)
		{
			for (int cellX = 0; cellX < grid.cellsWide; ++cellX)
			{
				if (!grid.activeCells[grid.CellIndex(cellX, cellY)])
					continue;
				for (int sampleY = 0; sampleY < grid.samplesPerAxis; ++sampleY)
				{
					const double fractionY = (sampleY + 0.5)
						/ grid.samplesPerAxis;
					for (int sampleX = 0;
						sampleX < grid.samplesPerAxis; ++sampleX)
					{
						const double fractionX = (sampleX + 0.5)
							/ grid.samplesPerAxis;
						const std::size_t index = grid.Index(
							cellX, cellY, sampleX, sampleY);
						const bool expectedInside = grid.inside[index] != 0;
						const bool actualInside = DecodeTsdfReferenceByte(
							BilinearTsdfReferenceByte(bytes, width, cellX, cellY,
								fractionX, fractionY), spread) > 0.0;
						metrics.falseOutside += expectedInside && !actualInside;
						metrics.falseInside += !expectedInside && actualInside;
					}
				}
			}
		}
		return true;
	}

	bool EvaluateTsdfReferenceDistanceCandidate(
		const std::vector<std::uint8_t>& bytes, int width, int height,
		std::uint8_t spread, const TsdfReferenceGrid& grid,
		int texelX, int texelY, int delta, double& improvement)
	{
		TsdfReferenceMetrics before;
		TsdfReferenceMetrics after;
		bool everySampleSafe = true;
		bool preservesCorrectSigns = true;
		for (int cellY = std::max(texelY - 1, 0);
			cellY <= std::min(texelY, height - 2); ++cellY)
		{
			for (int cellX = std::max(texelX - 1, 0);
				cellX <= std::min(texelX, width - 2); ++cellX)
			{
				if (!grid.activeCells[grid.CellIndex(cellX, cellY)])
					return false;
				for (int sampleY = 0; sampleY < grid.samplesPerAxis; ++sampleY)
				{
					const double fractionY = (sampleY + 0.5)
						/ grid.samplesPerAxis;
					for (int sampleX = 0;
						sampleX < grid.samplesPerAxis; ++sampleX)
					{
						const double fractionX = (sampleX + 0.5)
							/ grid.samplesPerAxis;
						const std::size_t index = grid.Index(
							cellX, cellY, sampleX, sampleY);
						const bool expectedInside = grid.inside[index] != 0;
						const double exactDistance = grid.signedDistances[index];
						const double beforeDistance = DecodeTsdfReferenceByte(
							BilinearTsdfReferenceByte(bytes, width, cellX, cellY,
								fractionX, fractionY), spread);
						const double afterDistance = DecodeTsdfReferenceByte(
							BilinearTsdfReferenceCandidateByte(bytes, width,
								cellX, cellY, fractionX, fractionY,
								texelX, texelY, delta), spread);
						if (!AccumulateTsdfReferenceSample(before, exactDistance,
								beforeDistance, expectedInside)
							|| !AccumulateTsdfReferenceSample(after, exactDistance,
								afterDistance, expectedInside))
						{
							return false;
						}
						everySampleSafe = everySampleSafe
							&& std::abs(exactDistance - afterDistance)
								<= std::abs(exactDistance - beforeDistance)
									+ kTsdfReferenceTolerance;
						preservesCorrectSigns = preservesCorrectSigns
							&& ((beforeDistance > 0.0) != expectedInside
								|| (afterDistance > 0.0) == expectedInside);
					}
				}
			}
		}
		if (!everySampleSafe || !preservesCorrectSigns
			|| after.falseOutside > before.falseOutside
			|| after.falseInside > before.falseInside
			|| after.severeDistanceErrors > before.severeDistanceErrors
			|| after.maximumDistanceError
				> before.maximumDistanceError + kTsdfReferenceTolerance)
		{
			return false;
		}
		for (std::size_t index = 0;
			index < kTsdfReferenceAntialiasWidths.size(); ++index)
		{
			if (after.absoluteCoverageError[index]
					> before.absoluteCoverageError[index] + kTsdfReferenceTolerance
				|| after.maximumCoverageError[index]
					> before.maximumCoverageError[index] + kTsdfReferenceTolerance)
			{
				return false;
			}
		}
		improvement = before.absoluteDistanceError - after.absoluteDistanceError;
		return std::isfinite(improvement)
			&& improvement > kTsdfReferenceTolerance;
	}

	bool EvaluateTsdfReferenceSignCandidate(
		const std::vector<std::uint8_t>& bytes, int width, int height,
		std::uint8_t spread, const TsdfReferenceGrid& grid,
		int texelX, int texelY, int delta)
	{
		TsdfReferenceSignMetrics before;
		TsdfReferenceSignMetrics after;
		bool preservesCorrectSigns = true;
		for (int cellY = std::max(texelY - 1, 0);
			cellY <= std::min(texelY, height - 2); ++cellY)
		{
			for (int cellX = std::max(texelX - 1, 0);
				cellX <= std::min(texelX, width - 2); ++cellX)
			{
				for (int sampleY = 0; sampleY < grid.samplesPerAxis; ++sampleY)
				{
					const double fractionY = (sampleY + 0.5)
						/ grid.samplesPerAxis;
					for (int sampleX = 0;
						sampleX < grid.samplesPerAxis; ++sampleX)
					{
						const double fractionX = (sampleX + 0.5)
							/ grid.samplesPerAxis;
						const bool expectedInside = grid.inside[grid.Index(
							cellX, cellY, sampleX, sampleY)] != 0;
						const bool beforeInside = DecodeTsdfReferenceByte(
							BilinearTsdfReferenceByte(bytes, width, cellX, cellY,
								fractionX, fractionY), spread) > 0.0;
						const bool afterInside = DecodeTsdfReferenceByte(
							BilinearTsdfReferenceCandidateByte(bytes, width,
								cellX, cellY, fractionX, fractionY,
								texelX, texelY, delta), spread) > 0.0;
						before.falseOutside += expectedInside && !beforeInside;
						before.falseInside += !expectedInside && beforeInside;
						after.falseOutside += expectedInside && !afterInside;
						after.falseInside += !expectedInside && afterInside;
						preservesCorrectSigns = preservesCorrectSigns
							&& (beforeInside != expectedInside
								|| afterInside == expectedInside);
					}
				}
			}
		}
		return preservesCorrectSigns
			&& after.falseOutside <= before.falseOutside
			&& after.falseInside <= before.falseInside;
	}

	bool ContinuousTsdfReferenceCenterSafe(
		const std::vector<std::uint8_t>& bytes,
		const std::vector<float>& continuousField, int width,
		int texelX, int texelY, int delta, std::uint8_t spread,
		bool expectedInside)
	{
		const std::size_t pixelIndex = static_cast<std::size_t>(texelY)
			* width + texelX;
		const int beforeByte = bytes[pixelIndex];
		const int afterByte = beforeByte + delta;
		if (afterByte < 0 || afterByte > 255)
			return false;
		const double encoded = continuousField[pixelIndex];
		if (!std::isfinite(encoded))
			return false;
		const double referenceDistance = ClampTsdfReferenceDistance(
			(encoded - 0.5) * 2.0 * spread, spread);
		const double beforeDistance = DecodeTsdfReferenceByte(beforeByte, spread);
		const double afterDistance = DecodeTsdfReferenceByte(afterByte, spread);
		return std::abs(referenceDistance - afterDistance)
				<= std::abs(referenceDistance - beforeDistance)
					+ kTsdfReferenceTolerance
			&& ((beforeDistance > 0.0) != expectedInside
				|| (afterDistance > 0.0) == expectedInside);
	}

	bool ExactTsdfReferenceCenterSafe(const std::vector<std::uint8_t>& bytes,
		int width, int texelX, int texelY, int delta, std::uint8_t spread,
		bool expectedInside, const msdfgen::Projection& projection,
		ExactShapeDistanceFinder& distanceFinder, bool& safe)
	{
		safe = false;
		const std::size_t pixelIndex = static_cast<std::size_t>(texelY)
			* width + texelX;
		const int beforeByte = bytes[pixelIndex];
		const int afterByte = beforeByte + delta;
		if (afterByte < 0 || afterByte > 255)
			return true;
		const msdfgen::Point2 shapePoint = projection.unproject(
			msdfgen::Point2(texelX + 0.5, texelY + 0.5));
		if (!std::isfinite(shapePoint.x) || !std::isfinite(shapePoint.y))
			return false;
		const double rawDistance = distanceFinder.distance(shapePoint);
		if (!std::isfinite(rawDistance))
			return false;
		const double unsignedDistance = std::min<double>(
			spread, std::abs(rawDistance));
		const double exactDistance = expectedInside
			? unsignedDistance : -unsignedDistance;
		const double beforeDistance = DecodeTsdfReferenceByte(beforeByte, spread);
		const double afterDistance = DecodeTsdfReferenceByte(afterByte, spread);
		if (std::abs(exactDistance - afterDistance)
			> std::abs(exactDistance - beforeDistance) + kTsdfReferenceTolerance)
		{
			return true;
		}
		safe = (beforeDistance > 0.0) != expectedInside
			|| (afterDistance > 0.0) == expectedInside;
		return true;
	}

	bool CountTsdfReferenceCenterErrors(
		const std::vector<std::uint8_t>& bytes, int width, int height,
		std::uint8_t spread, const msdfgen::Shape& shape,
		const msdfgen::Projection& projection, msdfgen::FillRule fillRule,
		std::size_t& errors)
	{
		errors = 0;
		for (int y = 0; y < height; ++y)
		{
			const double shapeY = projection.unprojectY(y + 0.5);
			if (!std::isfinite(shapeY))
				return false;
			msdfgen::Scanline scanline;
			shape.scanline(scanline, shapeY);
			for (int x = 0; x < width; ++x)
			{
				const double shapeX = projection.unprojectX(x + 0.5);
				if (!std::isfinite(shapeX))
					return false;
				const bool expectedInside = scanline.filled(shapeX, fillRule);
				const bool actualInside = DecodeTsdfReferenceByte(
					bytes[static_cast<std::size_t>(y) * width + x], spread) > 0.0;
				errors += expectedInside != actualInside;
			}
		}
		return true;
	}

	double MeasureTsdfReferenceShapeError(
		const std::vector<std::uint8_t>& bytes, int width, int height,
		const msdfgen::Shape& shape, const msdfgen::Projection& projection,
		msdfgen::FillRule fillRule)
	{
		msdfgen::Bitmap<float, 1> runtimeField(width, height);
		for (int y = 0; y < height; ++y)
			for (int x = 0; x < width; ++x)
				*runtimeField(x, y) = bytes[static_cast<std::size_t>(y)
					* width + x] / 256.0f;
		return msdfgen::estimateSDFError(runtimeField, shape, projection,
			kScanlinesPerRow, fillRule);
	}

	bool ApplyTsdfReferenceRepair(std::vector<std::uint8_t>& bytes,
		const std::vector<float>& continuousField, int width, int height,
		std::uint8_t spread, const msdfgen::Shape& shape,
		const msdfgen::Projection& projection, msdfgen::FillRule fillRule,
		TsdfReferenceRepairStats& stats)
	{
		stats = {};
		const std::vector<std::uint8_t> baseline = bytes;
		const int edgeCount = shape.edgeCount();
		const std::uint64_t pixelCount = static_cast<std::uint64_t>(width)
			* height;
		if (fillRule == msdfgen::FILL_ODD || width < 3 || height < 3
			|| width > kTsdfReferenceMaximumWidth
			|| edgeCount <= 0 || edgeCount > kTsdfReferenceMaximumEdges
			|| pixelCount > kTsdfReferenceMaximumPixels
			|| bytes.size() != pixelCount || continuousField.size() != pixelCount)
		{
			return true;
		}
		const std::uint64_t maximumDistanceQueries =
			kTsdfReferenceMaximumContinuousCandidates
			+ kTsdfReferenceMaximumActiveCells * 16u
			+ kTsdfReferenceMaximumTrials;
		const std::uint64_t maximumScanlines = 2u
			* static_cast<std::uint64_t>(height)
			+ static_cast<std::uint64_t>(height - 1) * (4u + 8u + 64u);
		if (!maximumDistanceQueries || !maximumScanlines
			|| maximumScanlines > kTsdfReferenceMaximumScanlines
			|| static_cast<std::uint64_t>(edgeCount)
				> kTsdfReferenceMaximumEdgeTests / maximumDistanceQueries
			|| static_cast<std::uint64_t>(edgeCount)
				> kTsdfReferenceMaximumEdgeTests / maximumScanlines
			|| !TsdfReferenceSortWorkWithinBudget(maximumScanlines,
				2u * static_cast<std::uint64_t>(height - 1) * 32u,
				width, edgeCount))
		{
			return true;
		}

		std::vector<TsdfReferenceCandidate> candidates;
		candidates.reserve(kTsdfReferenceMaximumContinuousCandidates);
		std::vector<std::uint8_t> activeCells(
			static_cast<std::size_t>(width - 1) * (height - 1), 0);
		std::size_t activeCellCount = 0;
		std::size_t continuousCandidateCount = 0;
		std::size_t baselineCenterErrors = 0;
		ExactShapeDistanceFinder centerDistanceFinder(shape);
		for (int y = 0; y < height; ++y)
		{
			const double shapeY = projection.unprojectY(y + 0.5);
			if (!std::isfinite(shapeY))
				return false;
			msdfgen::Scanline scanline;
			shape.scanline(scanline, shapeY);
			for (int x = 0; x < width; ++x)
			{
				const double shapeX = projection.unprojectX(x + 0.5);
				if (!std::isfinite(shapeX))
					return false;
				const bool centerInside = scanline.filled(shapeX, fillRule);
				const std::size_t pixelIndex = static_cast<std::size_t>(y)
					* width + x;
				baselineCenterErrors += centerInside
					!= (DecodeTsdfReferenceByte(bytes[pixelIndex], spread) > 0.0);
				for (int delta : { -1, 1 })
				{
					if (!ContinuousTsdfReferenceCenterSafe(bytes, continuousField,
						width, x, y, delta, spread, centerInside))
					{
						continue;
					}
					if (continuousCandidateCount
						>= kTsdfReferenceMaximumContinuousCandidates)
					{
						return true;
					}
					++continuousCandidateCount;
					bool exactSafe = false;
					if (!ExactTsdfReferenceCenterSafe(bytes, width, x, y, delta,
						spread, centerInside, projection, centerDistanceFinder,
						exactSafe))
					{
						return false;
					}
					if (!exactSafe)
						continue;
					candidates.push_back({ static_cast<std::uint32_t>(pixelIndex),
						delta, centerInside, 0.0 });
					if (!MarkTsdfReferenceCells(activeCells, width, height,
						x, y, activeCellCount))
					{
						return true;
					}
				}
			}
		}
		if (candidates.empty())
			return true;

		std::size_t activeRowCount = 0;
		for (int y = 0; y < height - 1; ++y)
		{
			bool activeRow = false;
			for (int x = 0; x < width - 1; ++x)
				activeRow |= activeCells[static_cast<std::size_t>(y)
					* (width - 1) + x] != 0;
			activeRowCount += activeRow;
		}
		const std::uint64_t trialBudget = std::min<std::size_t>(
			candidates.size(), kTsdfReferenceMaximumTrials);
		const std::uint64_t distanceQueries = continuousCandidateCount
			+ activeCellCount * 16u + trialBudget;
		const std::uint64_t scanlineQueries = 2u
			* static_cast<std::uint64_t>(height)
			+ activeRowCount * (4u + 8u + 64u);
		const std::uint64_t shapeWidthWork = 2u
			* static_cast<std::uint64_t>(width) * activeRowCount * 32u;
		const std::uint64_t fieldShapeRows = 2u * activeRowCount * 32u;
		if (!distanceQueries || !scanlineQueries
			|| scanlineQueries > kTsdfReferenceMaximumScanlines
			|| shapeWidthWork > kTsdfReferenceMaximumShapeWidthWork
			|| !TsdfReferenceSortWorkWithinBudget(scanlineQueries,
				fieldShapeRows, width, edgeCount)
			|| static_cast<std::uint64_t>(edgeCount)
				> kTsdfReferenceMaximumEdgeTests / distanceQueries
			|| static_cast<std::uint64_t>(edgeCount)
				> kTsdfReferenceMaximumEdgeTests / scanlineQueries)
		{
			return true;
		}

		TsdfReferenceGrid distanceGrid;
		TsdfReferenceGrid signGrid;
		if (!BuildTsdfReferenceExactGrid(shape, projection, fillRule,
				width, height, spread, 4, activeCells, distanceGrid)
			|| !BuildTsdfReferenceSignGrid(shape, projection, fillRule,
				width, height, 8, activeCells, signGrid))
		{
			return false;
		}
		TsdfReferenceMetrics baselineDistanceMetrics;
		TsdfReferenceSignMetrics baselineSignMetrics;
		if (!MeasureTsdfReferenceGrid(bytes, width, spread,
				distanceGrid, baselineDistanceMetrics)
			|| !MeasureTsdfReferenceSigns(bytes, width, spread,
				signGrid, baselineSignMetrics))
		{
			return false;
		}
		const double baselineShapeError = MeasureTsdfReferenceShapeError(
			bytes, width, height, shape, projection, fillRule);
		if (!std::isfinite(baselineShapeError))
			return false;

		for (TsdfReferenceCandidate& candidate : candidates)
		{
			const int x = static_cast<int>(candidate.pixelIndex % width);
			const int y = static_cast<int>(candidate.pixelIndex / width);
			double improvement = 0.0;
			if (!EvaluateTsdfReferenceDistanceCandidate(bytes, width, height,
					spread, distanceGrid, x, y, candidate.delta, improvement)
				|| !EvaluateTsdfReferenceSignCandidate(bytes, width, height,
					spread, signGrid, x, y, candidate.delta))
			{
				candidate.delta = 0;
				continue;
			}
			candidate.score = improvement;
		}
		candidates.erase(std::remove_if(candidates.begin(), candidates.end(),
			[](const TsdfReferenceCandidate& candidate)
			{
				return !candidate.delta || !std::isfinite(candidate.score);
			}), candidates.end());
		if (candidates.empty())
			return true;
		std::sort(candidates.begin(), candidates.end(),
			[](const TsdfReferenceCandidate& left,
				const TsdfReferenceCandidate& right)
			{
				if (left.pixelIndex != right.pixelIndex)
					return left.pixelIndex < right.pixelIndex;
				if (left.score != right.score)
					return left.score > right.score;
				return left.delta < right.delta;
			});
		std::size_t uniqueCount = 0;
		for (std::size_t index = 0; index < candidates.size(); ++index)
		{
			const TsdfReferenceCandidate candidate = candidates[index];
			if (uniqueCount
				&& candidates[uniqueCount - 1].pixelIndex == candidate.pixelIndex)
			{
				continue;
			}
			candidates[uniqueCount++] = candidate;
		}
		candidates.resize(uniqueCount);
		std::sort(candidates.begin(), candidates.end(),
			[](const TsdfReferenceCandidate& left,
				const TsdfReferenceCandidate& right)
			{
				if (left.score != right.score)
					return left.score > right.score;
				if (left.pixelIndex != right.pixelIndex)
					return left.pixelIndex < right.pixelIndex;
				return left.delta < right.delta;
			});
		stats.candidates = candidates.size();
		const std::size_t trialCount = std::min(candidates.size(),
			kTsdfReferenceMaximumTrials);
		for (std::size_t trial = 0; trial < trialCount; ++trial)
		{
			const TsdfReferenceCandidate& candidate = candidates[trial];
			const int x = static_cast<int>(candidate.pixelIndex % width);
			const int y = static_cast<int>(candidate.pixelIndex / width);
			bool centerSafe = false;
			if (!ExactTsdfReferenceCenterSafe(bytes, width, x, y,
					candidate.delta, spread, candidate.centerInside,
					projection, centerDistanceFinder, centerSafe))
			{
				return false;
			}
			if (!centerSafe)
				continue;
			double improvement = 0.0;
			if (!EvaluateTsdfReferenceDistanceCandidate(bytes, width, height,
					spread, distanceGrid, x, y, candidate.delta, improvement)
				|| !EvaluateTsdfReferenceSignCandidate(bytes, width, height,
					spread, signGrid, x, y, candidate.delta))
			{
				continue;
			}
			bytes[candidate.pixelIndex] = static_cast<std::uint8_t>(
				static_cast<int>(bytes[candidate.pixelIndex]) + candidate.delta);
			++stats.accepted;
		}
		if (!stats.accepted)
			return true;

		TsdfReferenceMetrics finalDistanceMetrics;
		TsdfReferenceSignMetrics finalSignMetrics;
		std::size_t finalCenterErrors = 0;
		const double finalShapeError = MeasureTsdfReferenceShapeError(
			bytes, width, height, shape, projection, fillRule);
		const bool keep = MeasureTsdfReferenceGrid(bytes, width, spread,
				distanceGrid, finalDistanceMetrics)
			&& MeasureTsdfReferenceSigns(bytes, width, spread,
				signGrid, finalSignMetrics)
			&& CountTsdfReferenceCenterErrors(bytes, width, height, spread,
				shape, projection, fillRule, finalCenterErrors)
			&& TsdfReferenceMetricsDoNotRegress(
				baselineDistanceMetrics, finalDistanceMetrics)
			&& finalSignMetrics.falseOutside <= baselineSignMetrics.falseOutside
			&& finalSignMetrics.falseInside <= baselineSignMetrics.falseInside
			&& finalCenterErrors <= baselineCenterErrors
			&& std::isfinite(finalShapeError)
			&& finalShapeError <= baselineShapeError + kTsdfReferenceTolerance;
		if (!keep)
		{
			bytes = baseline;
			stats.accepted = 0;
			stats.fallback = true;
		}
		return true;
	}

	bool AuditTsdfReferenceAnalytic8(
		const std::vector<std::uint8_t>& baseline,
		const std::vector<std::uint8_t>& repaired,
		int width, int height, std::uint8_t spread,
		const msdfgen::Shape& shape, const msdfgen::Projection& projection,
		msdfgen::FillRule fillRule, TsdfReferenceMetrics& baselineMetrics,
		TsdfReferenceMetrics& repairedMetrics)
	{
		std::vector<std::uint8_t> allCells(
			static_cast<std::size_t>(width - 1) * (height - 1), 1);
		TsdfReferenceGrid auditGrid;
		return BuildTsdfReferenceExactGrid(shape, projection, fillRule,
				width, height, spread, 8, allCells, auditGrid)
			&& MeasureTsdfReferenceGrid(baseline, width, spread,
				auditGrid, baselineMetrics)
			&& MeasureTsdfReferenceGrid(repaired, width, spread,
				auditGrid, repairedMetrics);
	}

	bool MatchesProductionMtsdf(FT_Outline& outline, std::uint8_t spread,
		const MtsdfResult& expected)
	{
		fonthook::vectorfont::MsdfgenMtsdfBitmap production;
		if (!fonthook::vectorfont::GenerateMsdfgenMtsdf(
			outline, spread, production, 64u * 1024u * 1024u)
			|| production.width != expected.field.width()
			|| production.height != expected.field.height()
			|| production.left != expected.bounds.left
			|| production.top != expected.bounds.top
			|| production.bgra.size() != static_cast<std::size_t>(
				expected.field.width()) * expected.field.height() * 4u)
		{
			return false;
		}
		for (int y = 0; y < expected.field.height(); ++y)
		{
			const int destinationY = expected.field.height() - 1 - y;
			for (int x = 0; x < expected.field.width(); ++x)
			{
				const float* source = expected.field(x, y);
				const std::uint8_t expectedBgra[4] = {
					msdfgen::pixelFloatToByte(source[2]),
					msdfgen::pixelFloatToByte(source[1]),
					msdfgen::pixelFloatToByte(source[0]),
					msdfgen::pixelFloatToByte(source[3]),
				};
				const std::size_t offset = (static_cast<std::size_t>(destinationY)
					* expected.field.width() + x) * 4u;
				for (int channel = 0; channel < 4; ++channel)
				{
					if (production.bgra[offset + channel]
						!= expectedBgra[channel])
					{
						return false;
					}
				}
			}
		}
		return true;
	}

	bool MatchesProductionTrueSdf(FT_Outline& outline, std::uint8_t spread,
		const std::vector<std::uint8_t>& expectedBottomUp,
		const Bounds& expectedBounds, double& productionMilliseconds)
	{
		const auto started = std::chrono::steady_clock::now();
		fonthook::vectorfont::MsdfgenSdfBitmap production;
		if (!fonthook::vectorfont::GenerateMsdfgenTrueSdf(
			outline, spread, production, 64u * 1024u * 1024u)
			|| production.width != expectedBounds.width
			|| production.height != expectedBounds.height
			|| production.left != expectedBounds.left
			|| production.top != expectedBounds.top
			|| production.pixels.size() != expectedBottomUp.size())
		{
			return false;
		}
		productionMilliseconds = std::chrono::duration<double, std::milli>(
			std::chrono::steady_clock::now() - started).count();
		for (int y = 0; y < expectedBounds.height; ++y)
		{
			const int destinationY = expectedBounds.height - 1 - y;
			for (int x = 0; x < expectedBounds.width; ++x)
			{
				const std::size_t offset = static_cast<std::size_t>(destinationY)
					* expectedBounds.width + x;
				if (production.pixels[offset] != expectedBottomUp[
					static_cast<std::size_t>(y) * expectedBounds.width + x])
				{
					return false;
				}
			}
		}
		return true;
	}

	const char* ColoringName(Coloring coloring)
	{
		switch (coloring)
		{
		case Coloring::Simple:
			return "simple";
		case Coloring::InkTrap:
			return "inktrap";
		case Coloring::ByDistance:
			return "distance";
		}
		return "unknown";
	}

	float Median(float red, float green, float blue)
	{
		return std::max(std::min(red, green),
			std::min(std::max(red, green), blue));
	}

	bool PrepareShape(FT_Outline& outline, msdfgen::Shape& shape)
	{
		if (msdfgen::readFreetypeOutline(shape, &outline, 1.0 / 64.0)
			|| shape.contours.empty())
		{
			return false;
		}
		const FT_Orientation orientation = FT_Outline_Get_Orientation(&outline);
		if (orientation == FT_ORIENTATION_POSTSCRIPT)
		{
			for (msdfgen::Contour& contour : shape.contours)
				contour.reverse();
		}
		else if (orientation != FT_ORIENTATION_TRUETYPE)
		{
			shape.orientContours();
		}
		shape.normalize();
		return shape.validate();
	}

	bool ResolveBounds(const msdfgen::Shape& shape, std::uint8_t spread,
		Bounds& output)
	{
		const msdfgen::Shape::Bounds source = shape.getBounds();
		if (!std::isfinite(source.l) || !std::isfinite(source.b)
			|| !std::isfinite(source.r) || !std::isfinite(source.t)
			|| source.l > source.r || source.b > source.t)
		{
			return false;
		}
		const int guard = spread + 1;
		output.left = static_cast<int>(std::floor(source.l)) - guard;
		output.bottom = static_cast<int>(std::floor(source.b)) - guard;
		output.top = static_cast<int>(std::ceil(source.t)) + guard;
		const int right = static_cast<int>(std::ceil(source.r)) + guard;
		output.width = right - output.left;
		output.height = output.top - output.bottom;
		return output.width > 1 && output.height > 1
			&& output.width <= 4096 && output.height <= 4096;
	}

	void ColorShape(msdfgen::Shape& shape, Coloring coloring)
	{
		switch (coloring)
		{
		case Coloring::Simple:
			msdfgen::edgeColoringSimple(shape, kCornerAngleThreshold, 0);
			break;
		case Coloring::InkTrap:
			msdfgen::edgeColoringInkTrap(shape, kCornerAngleThreshold, 0);
			break;
		case Coloring::ByDistance:
			msdfgen::edgeColoringByDistance(shape, kCornerAngleThreshold, 0);
			break;
		}
	}

	bool LoadOutline(FT_Face face, const GlyphCase& glyph, bool hinted)
	{
		if (FT_Set_Pixel_Sizes(face, glyph.pixelWidth, glyph.pixelHeight))
			return false;
		FT_Matrix identity = { 1L << 16, 0, 0, 1L << 16 };
		FT_Set_Transform(face, &identity, nullptr);
		const FT_UInt glyphIndex = FT_Get_Char_Index(face, glyph.codePoint);
		if (!glyphIndex)
			return false;
		FT_Int32 flags = FT_LOAD_DEFAULT | FT_LOAD_NO_BITMAP | FT_LOAD_NO_SVG;
		if (!hinted)
			flags |= FT_LOAD_NO_HINTING;
		return !FT_Load_Glyph(face, glyphIndex, flags)
			&& face->glyph->format == FT_GLYPH_FORMAT_OUTLINE;
	}

	msdfgen::MSDFGeneratorConfig CorrectionConfig(Correction correction)
	{
		msdfgen::MSDFGeneratorConfig config;
		config.overlapSupport = true;
		config.errorCorrection.mode = correction == Correction::IndiscriminateFast
			? msdfgen::ErrorCorrectionConfig::INDISCRIMINATE
			: correction == Correction::EdgeOnlyFast
				? msdfgen::ErrorCorrectionConfig::EDGE_ONLY
				: msdfgen::ErrorCorrectionConfig::EDGE_PRIORITY;
		config.errorCorrection.distanceCheckMode = correction == Correction::Mixed
			? msdfgen::ErrorCorrectionConfig::CHECK_DISTANCE_AT_EDGE
			: msdfgen::ErrorCorrectionConfig::DO_NOT_CHECK_DISTANCE;
		switch (correction)
		{
		case Correction::EdgePriority105:
			config.errorCorrection.minDeviationRatio = 1.05;
			break;
		case Correction::EdgePriority102:
			config.errorCorrection.minDeviationRatio = 1.02;
			break;
		case Correction::EdgePriority101:
			config.errorCorrection.minDeviationRatio = 1.01;
			break;
		case Correction::EdgePriority100:
			config.errorCorrection.minDeviationRatio = 1.0;
			break;
		default:
			break;
		}
		return config;
	}

	std::size_t ApplyAlphaCopyExperiment(msdfgen::Bitmap<float, 4>& field,
		const msdfgen::Shape& shape, const msdfgen::Projection& projection,
		msdfgen::FillRule fillRule, double sourceScale, std::uint8_t spread)
	{
		constexpr int samplesPerAxis = 8;
		constexpr double correctionThreshold = 0.25;
		const double antialiasWidth = 0.5 / std::max(sourceScale, 0.0001);
		constexpr std::size_t maximumCorrectedTexels = 32;
		std::vector<double> correctionScore(static_cast<std::size_t>(
			field.width()) * field.height());
		msdfgen::Bitmap<float, 1> centerFill(field.width(), field.height());
		msdfgen::rasterize(centerFill, shape, projection, fillRule);
		const msdfgen::BitmapConstSection<float, 4> fieldSection = field;
		for (int y = 0; y < field.height() - 1; ++y)
		{
			for (int sampleY = 0; sampleY < samplesPerAxis; ++sampleY)
			{
				const double fy = (sampleY + 0.5) / samplesPerAxis;
				const double fieldY = y + 0.5 + fy;
				for (int x = 0; x < field.width() - 1; ++x)
				{
					for (int sampleX = 0; sampleX < samplesPerAxis; ++sampleX)
					{
						const double fx = (sampleX + 0.5) / samplesPerAxis;
						const double fieldX = x + 0.5 + fx;
						float sample[4] = {};
						msdfgen::interpolate(sample, fieldSection,
							msdfgen::Point2(fieldX, fieldY));
						const double rgb = Median(sample[0], sample[1], sample[2]);
						const double alpha = sample[3];
						const double rgbCoverage = std::clamp(0.5
							+ (rgb - 0.5) * spread / antialiasWidth, 0.0, 1.0);
						const double alphaCoverage = std::clamp(0.5
							+ (alpha - 0.5) * spread / antialiasWidth, 0.0, 1.0);
						if (alphaCoverage - rgbCoverage <= correctionThreshold)
							continue;

						const double weights[4] = {
							(1.0 - fx) * (1.0 - fy),
							fx * (1.0 - fy),
							(1.0 - fx) * fy,
							fx * fy,
						};
						const int cornerX[4] = { x, x + 1, x, x + 1 };
						const int cornerY[4] = { y, y, y + 1, y + 1 };
						for (int corner = 0; corner < 4; ++corner)
						{
							if (weights[corner] < 0.125
								|| *centerFill(cornerX[corner], cornerY[corner]) < 0.5f)
							{
								continue;
							}
							const std::size_t index =
								static_cast<std::size_t>(cornerY[corner])
								* field.width() + cornerX[corner];
							correctionScore[index] = std::max(correctionScore[index],
								(alphaCoverage - rgbCoverage) * weights[corner]);
						}
					}
				}
			}
		}

		std::vector<std::pair<double, std::size_t>> ranked;
		ranked.reserve(correctionScore.size());
		for (std::size_t index = 0; index < correctionScore.size(); ++index)
		{
			if (correctionScore[index] > 0.0)
				ranked.emplace_back(correctionScore[index], index);
		}
		std::sort(ranked.begin(), ranked.end(),
			[](const auto& left, const auto& right)
			{
				if (left.first != right.first)
					return left.first > right.first;
				return left.second < right.second;
			});
		const std::size_t correctionCount = std::min(
			ranked.size(), maximumCorrectedTexels);
		for (std::size_t rank = 0; rank < correctionCount; ++rank)
		{
			const std::size_t index = ranked[rank].second;
			const int x = static_cast<int>(index % field.width());
			const int y = static_cast<int>(index / field.width());
			float* pixel = field(x, y);
			pixel[0] = pixel[1] = pixel[2] = pixel[3];
		}
		return correctionCount;
	}

	void MeasureRgbAlphaCoverage(const msdfgen::Bitmap<float, 4>& field,
		double sourceScale, std::uint8_t spread,
		double& maximumDeficit, double& severeDeficitFraction,
		double& meanAbsoluteDifference, double& signDisagreementFraction)
	{
		constexpr int samplesPerAxis = 8;
		constexpr double severeDeficit = 0.25;
		const msdfgen::BitmapConstSection<float, 4> fieldSection = field;
		const double antialiasWidth = 0.5 / std::max(sourceScale, 0.0001);
		std::uint64_t sampleCount = 0;
		std::uint64_t severeCount = 0;
		std::uint64_t signDisagreementCount = 0;
		double absoluteDifferenceSum = 0.0;
		maximumDeficit = 0.0;
		for (int y = 0; y < field.height() - 1; ++y)
		{
			for (int x = 0; x < field.width() - 1; ++x)
			{
				for (int sampleY = 0; sampleY < samplesPerAxis; ++sampleY)
				{
					for (int sampleX = 0; sampleX < samplesPerAxis; ++sampleX)
					{
						float sample[4] = {};
						msdfgen::interpolate(sample, fieldSection, msdfgen::Point2(
							x + (sampleX + 0.5) / samplesPerAxis + 0.5,
							y + (sampleY + 0.5) / samplesPerAxis + 0.5));
						const double rgb = Median(sample[0], sample[1], sample[2]);
						const double alpha = sample[3];
						const double rgbDistance = (rgb - 0.5) * 2.0 * spread;
						const double alphaDistance = (alpha - 0.5) * 2.0 * spread;
						const double rgbCoverage = std::clamp(
							0.5 + rgbDistance / (2.0 * antialiasWidth), 0.0, 1.0);
						const double alphaCoverage = std::clamp(
							0.5 + alphaDistance / (2.0 * antialiasWidth), 0.0, 1.0);
						const double deficit = alphaCoverage - rgbCoverage;
						maximumDeficit = std::max(maximumDeficit, deficit);
						severeCount += deficit > severeDeficit;
						signDisagreementCount += (rgb > 0.5) != (alpha > 0.5);
						absoluteDifferenceSum += std::abs(deficit);
						++sampleCount;
					}
				}
			}
		}
		const double denominator = static_cast<double>(
			std::max<std::uint64_t>(sampleCount, 1));
		severeDeficitFraction = severeCount / denominator;
		meanAbsoluteDifference = absoluteDifferenceSum / denominator;
		signDisagreementFraction = signDisagreementCount / denominator;
	}

	std::size_t CountCenterSignErrors(const msdfgen::Bitmap<float, 4>& field,
		const msdfgen::Shape& shape, const msdfgen::Projection& projection,
		msdfgen::FillRule fillRule)
	{
		msdfgen::Bitmap<float, 1> reference(field.width(), field.height());
		msdfgen::rasterize(reference, shape, projection, fillRule);
		std::size_t errors = 0;
		for (int y = 0; y < field.height(); ++y)
		{
			for (int x = 0; x < field.width(); ++x)
			{
				const float* pixel = field(x, y);
				const bool actual = Median(pixel[0], pixel[1], pixel[2]) > 0.5f;
				const bool expected = *reference(x, y) > 0.5f;
				errors += actual != expected;
			}
		}
		return errors;
	}

	std::size_t CountCenterSignErrors(const msdfgen::Bitmap<float, 1>& field,
		const msdfgen::Shape& shape, const msdfgen::Projection& projection,
		msdfgen::FillRule fillRule)
	{
		msdfgen::Bitmap<float, 1> reference(field.width(), field.height());
		msdfgen::rasterize(reference, shape, projection, fillRule);
		std::size_t errors = 0;
		for (int y = 0; y < field.height(); ++y)
		{
			for (int x = 0; x < field.width(); ++x)
				errors += (*field(x, y) > 0.5f) != (*reference(x, y) > 0.5f);
		}
		return errors;
	}

	std::vector<std::uint8_t> CaptureAlphaBytes(
		const msdfgen::Bitmap<float, 4>& field)
	{
		std::vector<std::uint8_t> alpha(static_cast<std::size_t>(
			field.width()) * field.height());
		for (int y = 0; y < field.height(); ++y)
		{
			for (int x = 0; x < field.width(); ++x)
			{
				alpha[static_cast<std::size_t>(y) * field.width() + x] =
					msdfgen::pixelFloatToByte(field(x, y)[3]);
			}
		}
		return alpha;
	}

	std::size_t CountAlphaByteChanges(
		const msdfgen::Bitmap<float, 4>& field,
		const std::vector<std::uint8_t>& baseline)
	{
		if (baseline.size() != static_cast<std::size_t>(field.width())
			* field.height())
		{
			return std::numeric_limits<std::size_t>::max();
		}
		std::size_t changes = 0;
		for (int y = 0; y < field.height(); ++y)
		{
			for (int x = 0; x < field.width(); ++x)
			{
				const std::size_t index = static_cast<std::size_t>(y)
					* field.width() + x;
				changes += msdfgen::pixelFloatToByte(field(x, y)[3])
					!= baseline[index];
			}
		}
		return changes;
	}

	std::vector<std::uint8_t> CaptureMtsdfRgbaBytes(
		const msdfgen::Bitmap<float, 4>& field)
	{
		std::vector<std::uint8_t> bytes(static_cast<std::size_t>(
			field.width()) * field.height() * 4u);
		for (int y = 0; y < field.height(); ++y)
		{
			for (int x = 0; x < field.width(); ++x)
			{
				const float* pixel = field(x, y);
				const std::size_t offset = (static_cast<std::size_t>(y)
					* field.width() + x) * 4u;
				for (int channel = 0; channel < 4; ++channel)
					bytes[offset + channel] = msdfgen::pixelFloatToByte(pixel[channel]);
			}
		}
		return bytes;
	}

	std::size_t CountMtsdfRgbaByteChanges(
		const msdfgen::Bitmap<float, 4>& field,
		const std::vector<std::uint8_t>& baseline)
	{
		const std::vector<std::uint8_t> current = CaptureMtsdfRgbaBytes(field);
		if (current.size() != baseline.size())
			return std::numeric_limits<std::size_t>::max();
		std::size_t changes = 0;
		for (std::size_t index = 0; index < current.size(); ++index)
			changes += current[index] != baseline[index];
		return changes;
	}

	void AccumulateShapeGridErrors(ShapeGridErrors& total,
		const ShapeGridErrors& value)
	{
		total.falseOutside += value.falseOutside;
		total.falseInside += value.falseInside;
		total.severeDistanceErrors += value.severeDistanceErrors;
		total.sampleCount += value.sampleCount;
		total.absoluteDistanceErrorSum += value.absoluteDistanceErrorSum;
		total.maximumAbsoluteDistanceError = std::max(
			total.maximumAbsoluteDistanceError,
			value.maximumAbsoluteDistanceError);
	}

	ShapeGridErrors CountCellShapeGridErrors(
		const msdfgen::Bitmap<float, 4>& field,
		const msdfgen::Shape& shape, const msdfgen::Projection& projection,
		msdfgen::FillRule fillRule, std::uint8_t spread,
		ExactShapeDistanceFinder& distanceFinder,
		int cellX, int cellY)
	{
		const int samplesPerAxis = gShapeSamplesPerAxis;
		constexpr double severeDistanceError = 0.125;
		ShapeGridErrors errors;
		if (cellX < 0 || cellY < 0 || cellX >= field.width() - 1
			|| cellY >= field.height() - 1)
		{
			return errors;
		}
		const msdfgen::BitmapConstSection<float, 4> section = field;
		for (int sampleY = 0; sampleY < samplesPerAxis; ++sampleY)
		{
			const double fieldY = cellY + 0.5
				+ (sampleY + 0.5) / samplesPerAxis;
			msdfgen::Scanline scanline;
			shape.scanline(scanline, projection.unprojectY(fieldY));
			for (int sampleX = 0; sampleX < samplesPerAxis; ++sampleX)
			{
				const double fieldX = cellX + 0.5
					+ (sampleX + 0.5) / samplesPerAxis;
				float sample[4] = {};
				msdfgen::interpolate(sample, section,
					msdfgen::Point2(fieldX, fieldY));
				const bool actual = Median(sample[0], sample[1], sample[2])
					> 0.5f;
				const bool expected = scanline.filled(
					projection.unprojectX(fieldX), fillRule);
				errors.falseOutside += expected && !actual;
				errors.falseInside += !expected && actual;
				const double rawDistance = distanceFinder.distance(
					projection.unproject(msdfgen::Point2(fieldX, fieldY)));
				if (!std::isfinite(rawDistance))
				{
					errors.absoluteDistanceErrorSum =
						std::numeric_limits<double>::infinity();
					errors.maximumAbsoluteDistanceError =
						std::numeric_limits<double>::infinity();
					return errors;
				}
				const double unsignedDistance = std::min<double>(
					spread, std::abs(rawDistance));
				const double signedDistance = expected
					? unsignedDistance : -unsignedDistance;
				const double rgbDistance = std::clamp(
					(Median(sample[0], sample[1], sample[2]) - 0.5)
					* 2.0 * spread, -static_cast<double>(spread),
					static_cast<double>(spread));
				const double distanceError = std::abs(
					signedDistance - rgbDistance);
				errors.severeDistanceErrors +=
					distanceError > severeDistanceError;
				errors.absoluteDistanceErrorSum += distanceError;
				errors.maximumAbsoluteDistanceError = std::max(
					errors.maximumAbsoluteDistanceError, distanceError);
				++errors.sampleCount;
			}
		}
		return errors;
	}

	ShapeGridErrors CountAffectedShapeGridErrors(
		const msdfgen::Bitmap<float, 4>& field,
		const msdfgen::Shape& shape, const msdfgen::Projection& projection,
		msdfgen::FillRule fillRule, std::uint8_t spread,
		int texelX, int texelY)
	{
		ShapeGridErrors total;
		ExactShapeDistanceFinder distanceFinder(shape);
		for (int cellY = std::max(texelY - 1, 0);
			cellY <= std::min(texelY, field.height() - 2); ++cellY)
		{
			for (int cellX = std::max(texelX - 1, 0);
				cellX <= std::min(texelX, field.width() - 2); ++cellX)
			{
				const ShapeGridErrors cell = CountCellShapeGridErrors(field,
					shape, projection, fillRule, spread,
					distanceFinder, cellX, cellY);
				AccumulateShapeGridErrors(total, cell);
			}
		}
		return total;
	}

	bool AffectedSampleDistanceErrorsDoNotIncrease(
		const msdfgen::Bitmap<float, 4>& before,
		const msdfgen::Bitmap<float, 4>& after,
		const msdfgen::Shape& shape, const msdfgen::Projection& projection,
		msdfgen::FillRule fillRule, std::uint8_t spread,
		int texelX, int texelY)
	{
		const int samplesPerAxis = gShapeSamplesPerAxis;
		constexpr double tolerance = 1e-12;
		const msdfgen::BitmapConstSection<float, 4> beforeSection = before;
		const msdfgen::BitmapConstSection<float, 4> afterSection = after;
		ExactShapeDistanceFinder distanceFinder(shape);
		for (int cellY = std::max(texelY - 1, 0);
			cellY <= std::min(texelY, before.height() - 2); ++cellY)
		{
			for (int sampleY = 0; sampleY < samplesPerAxis; ++sampleY)
			{
				const double fieldY = cellY + 0.5
					+ (sampleY + 0.5) / samplesPerAxis;
				msdfgen::Scanline scanline;
				shape.scanline(scanline, projection.unprojectY(fieldY));
				for (int cellX = std::max(texelX - 1, 0);
					cellX <= std::min(texelX, before.width() - 2); ++cellX)
				{
					for (int sampleX = 0; sampleX < samplesPerAxis; ++sampleX)
					{
						const double fieldX = cellX + 0.5
							+ (sampleX + 0.5) / samplesPerAxis;
						const bool inside = scanline.filled(
							projection.unprojectX(fieldX), fillRule);
						const double rawDistance = distanceFinder.distance(
							projection.unproject(msdfgen::Point2(fieldX, fieldY)));
						if (!std::isfinite(rawDistance))
							return false;
						const double unsignedDistance = std::min<double>(
							spread, std::abs(rawDistance));
						const double exactDistance = inside
							? unsignedDistance : -unsignedDistance;
						float beforeSample[4] = {};
						float afterSample[4] = {};
						msdfgen::interpolate(beforeSample, beforeSection,
							msdfgen::Point2(fieldX, fieldY));
						msdfgen::interpolate(afterSample, afterSection,
							msdfgen::Point2(fieldX, fieldY));
						const double beforeDistance = std::clamp(
							(Median(beforeSample[0], beforeSample[1], beforeSample[2])
								- 0.5) * 2.0 * spread,
							-static_cast<double>(spread),
							static_cast<double>(spread));
						const double afterDistance = std::clamp(
							(Median(afterSample[0], afterSample[1], afterSample[2])
								- 0.5) * 2.0 * spread,
							-static_cast<double>(spread),
							static_cast<double>(spread));
						if (std::abs(exactDistance - afterDistance)
							> std::abs(exactDistance - beforeDistance) + tolerance)
						{
							return false;
						}
					}
				}
			}
		}
		return true;
	}

	ShapeGridErrors CountShapeGridErrors(
		const msdfgen::Bitmap<float, 4>& field,
		const msdfgen::Shape& shape, const msdfgen::Projection& projection,
		msdfgen::FillRule fillRule, std::uint8_t spread)
	{
		ShapeGridErrors total;
		ExactShapeDistanceFinder distanceFinder(shape);
		for (int y = 0; y < field.height() - 1; ++y)
		{
			for (int x = 0; x < field.width() - 1; ++x)
			{
				const ShapeGridErrors cell = CountCellShapeGridErrors(field,
					shape, projection, fillRule, spread,
					distanceFinder, x, y);
				AccumulateShapeGridErrors(total, cell);
			}
		}
		return total;
	}

	bool EqualizeRgbToOwnMedian(float* pixel)
	{
		const float median = Median(pixel[0], pixel[1], pixel[2]);
		if (pixel[0] == median && pixel[1] == median && pixel[2] == median)
			return false;
		pixel[0] = pixel[1] = pixel[2] = median;
		return true;
	}

	struct CandidateShapeComparison
	{
		ShapeGridErrors before;
		ShapeGridErrors after;
		bool everySampleSafe = true;
		bool lostCorrectSign = false;
		bool valid = true;
	};

	CandidateShapeComparison CompareAffectedShapeGridErrors(
		const msdfgen::Bitmap<float, 4>& field,
		const msdfgen::Shape& shape, const msdfgen::Projection& projection,
		msdfgen::FillRule fillRule, std::uint8_t spread,
		ExactShapeDistanceFinder& distanceFinder,
		int texelX, int texelY, const float equalized[3])
	{
		constexpr int samplesPerAxis = 4;
		constexpr double severeDistanceError = 0.125;
		constexpr double tolerance = 1e-12;
		CandidateShapeComparison comparison;
		const msdfgen::BitmapConstSection<float, 4> section = field;
		for (int cellY = std::max(texelY - 1, 0);
			cellY <= std::min(texelY, field.height() - 2); ++cellY)
		{
			for (int sampleY = 0; sampleY < samplesPerAxis; ++sampleY)
			{
				const double fy = (sampleY + 0.5) / samplesPerAxis;
				const double fieldY = cellY + 0.5 + fy;
				msdfgen::Scanline scanline;
				shape.scanline(scanline, projection.unprojectY(fieldY));
				for (int cellX = std::max(texelX - 1, 0);
					cellX <= std::min(texelX, field.width() - 2); ++cellX)
				{
					for (int sampleX = 0; sampleX < samplesPerAxis; ++sampleX)
					{
						const double fx = (sampleX + 0.5) / samplesPerAxis;
						const double fieldX = cellX + 0.5 + fx;
						float beforeSample[4] = {};
						msdfgen::interpolate(beforeSample, section,
							msdfgen::Point2(fieldX, fieldY));
						float afterSample[3] = {};
						for (int channel = 0; channel < 3; ++channel)
						{
							const auto replacement = [&](int px, int py)
							{
								return px == texelX && py == texelY
									? equalized[channel] : field(px, py)[channel];
							};
							const float bottom = msdfgen::mix(
								replacement(cellX, cellY),
								replacement(cellX + 1, cellY), fx);
							const float top = msdfgen::mix(
								replacement(cellX, cellY + 1),
								replacement(cellX + 1, cellY + 1), fx);
							afterSample[channel] = msdfgen::mix(bottom, top, fy);
						}
						for (float value : beforeSample)
						{
							if (!std::isfinite(value))
							{
								comparison.valid = false;
								return comparison;
							}
						}
						for (float value : afterSample)
						{
							if (!std::isfinite(value))
							{
								comparison.valid = false;
								return comparison;
							}
						}
						const bool expected = scanline.filled(
							projection.unprojectX(fieldX), fillRule);
						const bool beforeActual = Median(beforeSample[0],
							beforeSample[1], beforeSample[2]) > 0.5f;
						const bool afterActual = Median(afterSample[0],
							afterSample[1], afterSample[2]) > 0.5f;
						if (beforeActual == expected && afterActual != expected)
						{
							comparison.lostCorrectSign = true;
							return comparison;
						}
						comparison.before.falseOutside += expected && !beforeActual;
						comparison.before.falseInside += !expected && beforeActual;
						comparison.after.falseOutside += expected && !afterActual;
						comparison.after.falseInside += !expected && afterActual;
						const double rawDistance = distanceFinder.distance(
							projection.unproject(msdfgen::Point2(fieldX, fieldY)));
						if (!std::isfinite(rawDistance))
						{
							comparison.valid = false;
							return comparison;
						}
						const double unsignedDistance = std::min<double>(
							spread, std::abs(rawDistance));
						const double exactDistance = expected
							? unsignedDistance : -unsignedDistance;
						const double beforeDistance = std::clamp(
							(Median(beforeSample[0], beforeSample[1], beforeSample[2])
								- 0.5) * 2.0 * spread,
							-static_cast<double>(spread), static_cast<double>(spread));
						const double afterDistance = std::clamp(
							(Median(afterSample[0], afterSample[1], afterSample[2])
								- 0.5) * 2.0 * spread,
							-static_cast<double>(spread), static_cast<double>(spread));
						const double beforeError = std::abs(
							exactDistance - beforeDistance);
						const double afterError = std::abs(
							exactDistance - afterDistance);
						if (!std::isfinite(unsignedDistance)
							|| !std::isfinite(exactDistance)
							|| !std::isfinite(beforeDistance)
							|| !std::isfinite(afterDistance)
							|| !std::isfinite(beforeError)
							|| !std::isfinite(afterError))
						{
							comparison.valid = false;
							return comparison;
						}
						comparison.before.severeDistanceErrors +=
							beforeError > severeDistanceError;
						comparison.after.severeDistanceErrors +=
							afterError > severeDistanceError;
						comparison.before.absoluteDistanceErrorSum += beforeError;
						comparison.after.absoluteDistanceErrorSum += afterError;
						comparison.before.maximumAbsoluteDistanceError = std::max(
							comparison.before.maximumAbsoluteDistanceError, beforeError);
						comparison.after.maximumAbsoluteDistanceError = std::max(
							comparison.after.maximumAbsoluteDistanceError, afterError);
						++comparison.before.sampleCount;
						++comparison.after.sampleCount;
						comparison.everySampleSafe = comparison.everySampleSafe
							&& afterError <= beforeError + tolerance;
						if (!std::isfinite(comparison.before.absoluteDistanceErrorSum)
							|| !std::isfinite(comparison.after.absoluteDistanceErrorSum)
							|| !std::isfinite(
								comparison.before.maximumAbsoluteDistanceError)
							|| !std::isfinite(
								comparison.after.maximumAbsoluteDistanceError))
						{
							comparison.valid = false;
							return comparison;
						}
					}
				}
			}
		}
		return comparison;
	}

	struct CandidateSignComparison
	{
		std::size_t beforeFalseOutside = 0;
		std::size_t beforeFalseInside = 0;
		std::size_t afterFalseOutside = 0;
		std::size_t afterFalseInside = 0;
		bool valid = true;
		bool lostCorrectSign = false;
	};

	CandidateSignComparison CompareAffectedDenseGridSigns(
		const msdfgen::Bitmap<float, 4>& field,
		const msdfgen::Shape& shape, const msdfgen::Projection& projection,
		msdfgen::FillRule fillRule, int texelX, int texelY,
		const float equalized[3], int samplesPerAxis)
	{
		CandidateSignComparison comparison;
		if (samplesPerAxis <= 0)
		{
			comparison.valid = false;
			return comparison;
		}
		const msdfgen::BitmapConstSection<float, 4> section = field;
		for (int cellY = std::max(texelY - 1, 0);
			cellY <= std::min(texelY, field.height() - 2); ++cellY)
		{
			for (int sampleY = 0; sampleY < samplesPerAxis; ++sampleY)
			{
				const double fy = (sampleY + 0.5) / samplesPerAxis;
				const double fieldY = cellY + 0.5 + fy;
				const double shapeY = projection.unprojectY(fieldY);
				if (!std::isfinite(shapeY))
				{
					comparison.valid = false;
					return comparison;
				}
				msdfgen::Scanline scanline;
				shape.scanline(scanline, shapeY);
				for (int cellX = std::max(texelX - 1, 0);
					cellX <= std::min(texelX, field.width() - 2); ++cellX)
				{
					for (int sampleX = 0; sampleX < samplesPerAxis; ++sampleX)
					{
						const double fx = (sampleX + 0.5) / samplesPerAxis;
						const double fieldX = cellX + 0.5 + fx;
						const double shapeX = projection.unprojectX(fieldX);
						if (!std::isfinite(shapeX))
						{
							comparison.valid = false;
							return comparison;
						}
						float beforeSample[4] = {};
						msdfgen::interpolate(beforeSample, section,
							msdfgen::Point2(fieldX, fieldY));
						float afterSample[3] = {};
						for (int channel = 0; channel < 3; ++channel)
						{
							const auto replacement = [&](int px, int py)
							{
								return px == texelX && py == texelY
									? equalized[channel] : field(px, py)[channel];
							};
							const float bottom = msdfgen::mix(
								replacement(cellX, cellY),
								replacement(cellX + 1, cellY), fx);
							const float top = msdfgen::mix(
								replacement(cellX, cellY + 1),
								replacement(cellX + 1, cellY + 1), fx);
							afterSample[channel] = msdfgen::mix(bottom, top, fy);
						}
						if (!std::isfinite(beforeSample[0])
							|| !std::isfinite(beforeSample[1])
							|| !std::isfinite(beforeSample[2])
							|| !std::isfinite(afterSample[0])
							|| !std::isfinite(afterSample[1])
							|| !std::isfinite(afterSample[2]))
						{
							comparison.valid = false;
							return comparison;
						}
						const bool expected = scanline.filled(shapeX, fillRule);
						const bool beforeActual = Median(beforeSample[0],
							beforeSample[1], beforeSample[2]) > 0.5f;
						const bool afterActual = Median(afterSample[0],
							afterSample[1], afterSample[2]) > 0.5f;
						if (beforeActual == expected && afterActual != expected)
						{
							comparison.lostCorrectSign = true;
							return comparison;
						}
						comparison.beforeFalseOutside += expected && !beforeActual;
						comparison.beforeFalseInside += !expected && beforeActual;
						comparison.afterFalseOutside += expected && !afterActual;
						comparison.afterFalseInside += !expected && afterActual;
					}
				}
			}
		}
		return comparison;
	}

	double EstimateAffectedRowsError(
		const msdfgen::Bitmap<float, 4>& field,
		const msdfgen::Shape& shape, const msdfgen::Projection& projection,
		msdfgen::FillRule fillRule, int texelY)
	{
		if (field.width() <= 1 || field.height() <= 1)
			return 0.0;
		const int firstRow = std::max(texelY - 1, 0);
		const int lastRow = std::min(texelY, field.height() - 2);
		const double subRowSize = 1.0 / kScanlinesPerRow;
		const double xFrom = projection.unprojectX(0.5);
		const double xTo = projection.unprojectX(field.width() - 0.5);
		const double overlapFactor = 1.0 / (xTo - xFrom);
		if (!std::isfinite(xFrom) || !std::isfinite(xTo)
			|| !std::isfinite(overlapFactor) || !(xTo > xFrom))
		{
			return std::numeric_limits<double>::quiet_NaN();
		}
		double error = 0.0;
		msdfgen::Scanline referenceScanline;
		msdfgen::Scanline fieldScanline;
		const msdfgen::BitmapConstSection<float, 4> section = field;
		for (int row = firstRow; row <= lastRow; ++row)
		{
			for (int subRow = 0; subRow < kScanlinesPerRow; ++subRow)
			{
				const double bt = (subRow + 0.5) * subRowSize;
				const double y = projection.unprojectY(row + bt + 0.5);
				if (!std::isfinite(y))
					return std::numeric_limits<double>::quiet_NaN();
				shape.scanline(referenceScanline, y);
				msdfgen::scanlineSDF(fieldScanline, section, projection, y,
					shape.getYAxisOrientation());
				const double contribution = 1.0 - overlapFactor
					* msdfgen::Scanline::overlap(
					referenceScanline, fieldScanline, xFrom, xTo, fillRule);
				if (!std::isfinite(contribution))
					return std::numeric_limits<double>::quiet_NaN();
				error += contribution;
				if (!std::isfinite(error))
					return std::numeric_limits<double>::quiet_NaN();
			}
		}
		return error / ((field.height() - 1.0) * kScanlinesPerRow);
	}

	std::size_t ApplyExactShapeRgbMedianRepair(
		msdfgen::Bitmap<float, 4>& field, const msdfgen::Shape& shape,
		const msdfgen::Projection& projection, msdfgen::FillRule fillRule,
		std::uint8_t spread, int denseSignSamplesPerAxis,
		std::size_t& candidateCount, bool& fallback)
	{
		// This variant is byte-for-byte parity checked against production, whose
		// exact-distance grid is fixed at 4x4 and its sign-only safety grid at
		// 8x8. Reference and final metrics may use the CLI-selected density.
		constexpr int samplesPerAxis = 4;
		constexpr double correctionThreshold = 0.125;
		// Alpha is the independently generated scalar SDF. This deliberately
		// permissive margin rejects obvious non-deficits before exact distance
		// lookup while retaining quantization/interpolation uncertainty.
		constexpr double alphaPrefilterMargin = 0.5;
		constexpr std::size_t maximumCandidateTrials = 64;
		constexpr double errorTolerance = 1e-12;
		constexpr std::uint64_t maximumPixels = 65536;
		constexpr std::uint64_t maximumEdges = 1024;
		constexpr std::uint64_t maximumWorstCaseEdgeTests = 64000000;
		candidateCount = 0;
		fallback = false;
		if (fillRule == msdfgen::FILL_ODD
			|| field.width() < 3 || field.height() < 3)
		{
			return 0;
		}
		const std::uint64_t width = static_cast<std::uint64_t>(field.width());
		const std::uint64_t height = static_cast<std::uint64_t>(field.height());
		const std::uint64_t pixelCount = width * height;
		const int signedEdgeCount = shape.edgeCount();
		if (pixelCount > maximumPixels || signedEdgeCount <= 0
			|| static_cast<std::uint64_t>(signedEdgeCount) > maximumEdges)
		{
			return 0;
		}
		const std::uint64_t samplesPerCell = static_cast<std::uint64_t>(
			samplesPerAxis) * samplesPerAxis;
		const std::uint64_t scoringDistanceQueries =
			(width - 1) * (height - 1) * samplesPerCell;
		const std::uint64_t localDistanceQueries = maximumCandidateTrials
			* 4 * samplesPerCell;
		const std::uint64_t worstDistanceQueries = scoringDistanceQueries
			+ localDistanceQueries;
		if (!worstDistanceQueries
			|| static_cast<std::uint64_t>(signedEdgeCount)
				> maximumWorstCaseEdgeTests / worstDistanceQueries)
		{
			return 0;
		}
		for (int y = 0; y < field.height(); ++y)
		{
			for (int x = 0; x < field.width(); ++x)
			{
				const float* pixel = field(x, y);
				if (!std::isfinite(pixel[0]) || !std::isfinite(pixel[1])
					|| !std::isfinite(pixel[2]) || !std::isfinite(pixel[3]))
				{
					return 0;
				}
			}
		}
		const double baselineError = msdfgen::estimateSDFError(field, shape,
			projection, kScanlinesPerRow, fillRule);
		if (!std::isfinite(baselineError))
			return 0;
		const std::size_t baselineCenters = CountCenterSignErrors(field,
			shape, projection, fillRule);

		std::vector<double> scores(static_cast<std::size_t>(field.width())
			* field.height());
		const msdfgen::BitmapConstSection<float, 4> section = field;
		ExactShapeDistanceFinder distanceFinder(shape);
		for (int y = 0; y < field.height() - 1; ++y)
		{
			for (int sampleY = 0; sampleY < samplesPerAxis; ++sampleY)
			{
				const double fy = (sampleY + 0.5) / samplesPerAxis;
				const double fieldY = y + 0.5 + fy;
				msdfgen::Scanline scanline;
				shape.scanline(scanline, projection.unprojectY(fieldY));
				for (int x = 0; x < field.width() - 1; ++x)
				{
					for (int sampleX = 0; sampleX < samplesPerAxis; ++sampleX)
					{
						const double fx = (sampleX + 0.5) / samplesPerAxis;
						const double fieldX = x + 0.5 + fx;
						float sample[4] = {};
						msdfgen::interpolate(sample, section,
							msdfgen::Point2(fieldX, fieldY));
						if (!std::isfinite(sample[0]) || !std::isfinite(sample[1])
							|| !std::isfinite(sample[2]) || !std::isfinite(sample[3]))
						{
							return 0;
						}
						if (!scanline.filled(projection.unprojectX(fieldX), fillRule))
							continue;
						const double rgbDistance = std::clamp(
							(Median(sample[0], sample[1], sample[2]) - 0.5)
								* 2.0 * spread,
							-static_cast<double>(spread), static_cast<double>(spread));
						const double alphaDistance = std::clamp(
							(sample[3] - 0.5) * 2.0 * spread,
							-static_cast<double>(spread), static_cast<double>(spread));
						if (!std::isfinite(rgbDistance)
							|| !std::isfinite(alphaDistance))
						{
							return 0;
						}
						if (alphaDistance - rgbDistance
							<= correctionThreshold - alphaPrefilterMargin)
						{
							continue;
						}

						const double weights[4] = {
							(1.0 - fx) * (1.0 - fy), fx * (1.0 - fy),
							(1.0 - fx) * fy, fx * fy,
						};
						const int cornerX[4] = { x, x + 1, x, x + 1 };
						const int cornerY[4] = { y, y, y + 1, y + 1 };
						double changedDistances[4] = {};
						bool hasPotentialImprovement = false;
						for (int corner = 0; corner < 4; ++corner)
						{
							const float* pixel = field(cornerX[corner], cornerY[corner]);
							float equalized[4] = {
								pixel[0], pixel[1], pixel[2], pixel[3]
							};
							if (!EqualizeRgbToOwnMedian(equalized))
							{
								changedDistances[corner] = rgbDistance;
								continue;
							}
							float changedSample[3] = {};
							for (int channel = 0; channel < 3; ++channel)
							{
								changedSample[channel] = static_cast<float>(
									sample[channel] + weights[corner]
									* (equalized[channel] - pixel[channel]));
							}
							changedDistances[corner] = std::clamp(
								(Median(changedSample[0], changedSample[1],
									changedSample[2]) - 0.5) * 2.0 * spread,
								-static_cast<double>(spread), static_cast<double>(spread));
							if (!std::isfinite(changedDistances[corner]))
								return 0;
							hasPotentialImprovement = hasPotentialImprovement
								|| changedDistances[corner] > rgbDistance;
						}
						if (!hasPotentialImprovement)
							continue;

						const double rawDistance = distanceFinder.distance(
							projection.unproject(msdfgen::Point2(fieldX, fieldY)));
						if (!std::isfinite(rawDistance))
							return 0;
						const double unsignedDistance = std::min<double>(
							spread, std::abs(rawDistance));
						if (unsignedDistance - rgbDistance <= correctionThreshold)
							continue;
						const double beforeError = std::abs(
							unsignedDistance - rgbDistance);
						for (int corner = 0; corner < 4; ++corner)
						{
							const double improvement = beforeError - std::abs(
								unsignedDistance - changedDistances[corner]);
							if (!std::isfinite(improvement))
								return 0;
							if (improvement <= 0.0)
								continue;
							const std::size_t index = static_cast<std::size_t>(
								cornerY[corner]) * field.width() + cornerX[corner];
							scores[index] = std::max(scores[index], improvement);
						}
					}
				}
			}
		}

		std::vector<std::pair<double, std::size_t>> ranked;
		ranked.reserve(scores.size());
		for (std::size_t index = 0; index < scores.size(); ++index)
		{
			if (scores[index] > 0.0)
				ranked.emplace_back(scores[index], index);
		}
		candidateCount = ranked.size();
		const std::size_t trials = std::min(ranked.size(), maximumCandidateTrials);
		std::partial_sort(ranked.begin(), ranked.begin() + trials, ranked.end(),
			[](const auto& left, const auto& right)
			{
				return left.first != right.first
					? left.first > right.first : left.second < right.second;
			});

		struct AcceptedPixel
		{
			std::size_t index = 0;
			float rgb[3] = {};
		};
		std::vector<AcceptedPixel> acceptedPixels;
		acceptedPixels.reserve(trials);
		const auto rollbackAccepted = [&]()
		{
			for (auto accepted = acceptedPixels.rbegin();
				accepted != acceptedPixels.rend(); ++accepted)
			{
				float* pixel = field(static_cast<int>(accepted->index % field.width()),
					static_cast<int>(accepted->index / field.width()));
				pixel[0] = accepted->rgb[0];
				pixel[1] = accepted->rgb[1];
				pixel[2] = accepted->rgb[2];
			}
			acceptedPixels.clear();
		};
		for (std::size_t rank = 0; rank < trials; ++rank)
		{
			const std::size_t index = ranked[rank].second;
			const int x = static_cast<int>(index % field.width());
			const int y = static_cast<int>(index / field.width());
			const float* pixel = field(x, y);
			float equalized[3] = { pixel[0], pixel[1], pixel[2] };
			if (!EqualizeRgbToOwnMedian(equalized))
				continue;
			const CandidateShapeComparison comparison =
				CompareAffectedShapeGridErrors(field, shape, projection,
					fillRule, spread, distanceFinder, x, y, equalized);
			const char* rejection = nullptr;
			if (!comparison.valid)
			{
				fallback = true;
				rollbackAccepted();
				return 0;
			}
			if (comparison.lostCorrectSign)
				rejection = "lost-correct-sign";
			else if (comparison.after.falseOutside > comparison.before.falseOutside)
				rejection = "false-outside";
			else if (comparison.after.falseInside > comparison.before.falseInside)
				rejection = "false-inside";
			else if (comparison.after.severeDistanceErrors
				> comparison.before.severeDistanceErrors)
				rejection = "severe";
			else if (comparison.after.maximumAbsoluteDistanceError
				> comparison.before.maximumAbsoluteDistanceError + errorTolerance)
				rejection = "max";
			else if (comparison.after.absoluteDistanceErrorSum
				>= comparison.before.absoluteDistanceErrorSum - errorTolerance)
				rejection = "mae";
			else if (!comparison.everySampleSafe)
				rejection = "per-sample";
			if (rejection)
				continue;
			if (denseSignSamplesPerAxis > samplesPerAxis)
			{
				const CandidateSignComparison denseComparison =
					CompareAffectedDenseGridSigns(field, shape, projection,
						fillRule, x, y, equalized, denseSignSamplesPerAxis);
				if (!denseComparison.valid)
				{
					fallback = true;
					rollbackAccepted();
					return 0;
				}
				if (denseComparison.lostCorrectSign
					|| denseComparison.afterFalseOutside
						> denseComparison.beforeFalseOutside
					|| denseComparison.afterFalseInside
						> denseComparison.beforeFalseInside)
					continue;
			}

			AcceptedPixel accepted;
			accepted.index = index;
			accepted.rgb[0] = pixel[0];
			accepted.rgb[1] = pixel[1];
			accepted.rgb[2] = pixel[2];
			const double beforeContourError = EstimateAffectedRowsError(
				field, shape, projection, fillRule, y);
			float* mutablePixel = field(x, y);
			mutablePixel[0] = equalized[0];
			mutablePixel[1] = equalized[1];
			mutablePixel[2] = equalized[2];
			const double afterContourError = EstimateAffectedRowsError(
				field, shape, projection, fillRule, y);
			if (!std::isfinite(beforeContourError)
				|| !std::isfinite(afterContourError))
			{
				mutablePixel[0] = accepted.rgb[0];
				mutablePixel[1] = accepted.rgb[1];
				mutablePixel[2] = accepted.rgb[2];
				fallback = true;
				rollbackAccepted();
				return 0;
			}
			if (afterContourError > beforeContourError + errorTolerance)
			{
				mutablePixel[0] = accepted.rgb[0];
				mutablePixel[1] = accepted.rgb[1];
				mutablePixel[2] = accepted.rgb[2];
				continue;
			}
			acceptedPixels.push_back(accepted);
		}

		const double finalError = msdfgen::estimateSDFError(field, shape,
			projection, kScanlinesPerRow, fillRule);
		const std::size_t finalCenters = CountCenterSignErrors(field,
			shape, projection, fillRule);
		fallback = !std::isfinite(finalError)
			|| finalError > baselineError + errorTolerance
			|| finalCenters > baselineCenters;
		if (fallback)
		{
			rollbackAccepted();
			return 0;
		}
		return acceptedPixels.size();
	}

	// Independent revision-7 reference. The existing function above is the
	// complete revision-6 phase and is deliberately left untouched; phase 2
	// starts from that frozen result and records only its own RGB writes.
	constexpr int kProbeMtsdfPhase2RankSamples = 4;
	constexpr int kProbeMtsdfPhase2DenseSamples = 8;
	constexpr int kProbeMtsdfPhase2AuditSamples = 16;
	constexpr double kProbeMtsdfPhase2SevereError = 0.125;
	constexpr double kProbeMtsdfPhase2AlphaMargin = 0.5;
	constexpr double kProbeMtsdfPhase2Tolerance = 1e-12;
	constexpr std::array<double, 4> kProbeMtsdfPhase2CoverageWidths = {
		0.25, 0.5, 0.75, 1.0
	};
	constexpr std::size_t kProbeMtsdfPhase2MaximumPixels = 65536;
	constexpr int kProbeMtsdfPhase2MaximumEdges = 1024;
	constexpr std::size_t kProbeMtsdfPhase2MaximumContours = 1024;
	constexpr std::size_t kProbeMtsdfPhase2MaximumTrials = 64;
	constexpr std::size_t kProbeMtsdfPhase2MaximumActiveCells =
		4u * kProbeMtsdfPhase2MaximumTrials;
	constexpr int kProbeMtsdfPhase2MaximumWidth = 4096;
	constexpr std::uint64_t kProbeMtsdfPhase2MaximumEdgeTests = 64000000;
	constexpr std::uint64_t kProbeMtsdfPhase2MaximumScanlineEdgeTests = 64000000;
	constexpr std::uint64_t kProbeMtsdfPhase2MaximumFieldScanWork = 64000000;
	constexpr std::uint64_t kProbeMtsdfPhase2MaximumSortWork = 64000000;

	struct ProbeMtsdfPhase2Candidate
	{
		double score = 0.0;
		std::uint32_t pixelIndex = 0;
	};

	struct ProbeMtsdfPhase2AcceptedPixel
	{
		std::uint32_t pixelIndex = 0;
		float rgb[3] = {};
		float alpha = 0.0f;
	};

	struct ProbeMtsdfPhase2CoverageErrors
	{
		double absoluteErrorSum = 0.0;
		double maximumAbsoluteError = 0.0;
		std::size_t sampleCount = 0;
	};

	struct ProbeMtsdfPhase2ExactComparison
	{
		ShapeGridErrors before;
		ShapeGridErrors after;
		std::array<ProbeMtsdfPhase2CoverageErrors,
			kProbeMtsdfPhase2CoverageWidths.size()> beforeCoverage = {};
		std::array<ProbeMtsdfPhase2CoverageErrors,
			kProbeMtsdfPhase2CoverageWidths.size()> afterCoverage = {};
		double maximumErrorIncrease = 0.0;
		double maximumWorsenedAfterError = 0.0;
		std::size_t worsenedSamples = 0;
		bool preservesCorrectSigns = true;
		bool finite = true;
	};

	struct ProbeMtsdfPhase2CenterComparison
	{
		std::size_t beforeFalseOutside = 0;
		std::size_t beforeFalseInside = 0;
		std::size_t afterFalseOutside = 0;
		std::size_t afterFalseInside = 0;
		bool preservesCorrectSigns = true;
		bool finite = true;
	};

	struct ProbeMtsdfPhase2WorkBudget
	{
		std::uint64_t distanceEdgeTests = 0;
		std::uint64_t scanlineEdgeTests = 0;
		std::uint64_t fieldScanWork = 0;
		std::uint64_t sortWork = 0;
	};

	static_assert(sizeof(msdfgen::Scanline::Intersection) == 16,
		"probe expects the v145 msdfgen scanline-intersection layout");
	static_assert(sizeof(msdfgen::TrueDistanceSelector::EdgeCache) == 24,
		"probe expects the v145 true-distance edge-cache layout");
	static_assert(sizeof(msdfgen::TrueDistanceSelector) == 32,
		"probe expects the v145 true-distance selector layout");
	constexpr std::size_t kProbeMtsdfPhase2FinderPayloadBytes =
		kProbeMtsdfPhase2MaximumEdges
			* sizeof(msdfgen::TrueDistanceSelector::EdgeCache)
		+ kProbeMtsdfPhase2MaximumContours
			* (sizeof(msdfgen::TrueDistanceSelector) + sizeof(int));
	constexpr std::size_t kProbeMtsdfPhase2ShapeScanlinePayloadBytes =
		3u * kProbeMtsdfPhase2MaximumEdges
			* sizeof(msdfgen::Scanline::Intersection);
	constexpr std::size_t kProbeMtsdfPhase2FieldScanlinePayloadBytes =
		(3u * (kProbeMtsdfPhase2MaximumWidth - 1u) + 1u)
			* sizeof(msdfgen::Scanline::Intersection);
	constexpr std::size_t kProbeMtsdfPhase2FixedPayloadBytes =
		kProbeMtsdfPhase2MaximumTrials
			* (sizeof(ProbeMtsdfPhase2Candidate)
				+ sizeof(ProbeMtsdfPhase2AcceptedPixel))
		+ kProbeMtsdfPhase2MaximumActiveCells * sizeof(std::uint32_t);
	constexpr std::size_t kProbeMtsdfPhase2DynamicMarginBytes = 128u * 1024u;
	constexpr std::size_t kProbeMtsdfPhase2RankingScratchBytes =
		kProbeMtsdfPhase2MaximumPixels * sizeof(double)
		+ kProbeMtsdfPhase2FinderPayloadBytes
		+ kProbeMtsdfPhase2ShapeScanlinePayloadBytes
		+ kProbeMtsdfPhase2FixedPayloadBytes
		+ kProbeMtsdfPhase2DynamicMarginBytes;
	constexpr std::size_t kProbeMtsdfPhase2RowScratchBytes =
		kProbeMtsdfPhase2FinderPayloadBytes
		+ kProbeMtsdfPhase2ShapeScanlinePayloadBytes
		+ kProbeMtsdfPhase2FieldScanlinePayloadBytes
		+ kProbeMtsdfPhase2FixedPayloadBytes
		+ kProbeMtsdfPhase2DynamicMarginBytes;
	static_assert(kProbeMtsdfPhase2RankingScratchBytes
		<= fonthook::vectorfont::kMtsdfRescuePerWorkerScratchBudgetBytes,
		"probe MTSDF phase-2 ranking exceeds the production worker budget");
	static_assert(kProbeMtsdfPhase2RowScratchBytes
		<= fonthook::vectorfont::kMtsdfRescuePerWorkerScratchBudgetBytes,
		"probe MTSDF phase-2 row audit exceeds the production worker budget");

	std::uint64_t ProbeMtsdfPhase2CeilLog2(std::uint64_t value)
	{
		std::uint64_t result = 0;
		std::uint64_t power = 1;
		while (power < value)
		{
			power <<= 1;
			++result;
		}
		return result;
	}

	bool ProbeMtsdfPhase2ConsumeProduct(std::uint64_t& consumed,
		std::uint64_t left, std::uint64_t right, std::uint64_t maximum)
	{
		if (consumed > maximum || (left && right > (maximum - consumed) / left))
			return false;
		consumed += left * right;
		return true;
	}

	bool ProbeMtsdfPhase2ConsumeDistanceQueries(
		ProbeMtsdfPhase2WorkBudget& budget, std::uint64_t queries, int edgeCount)
	{
		return edgeCount > 0 && ProbeMtsdfPhase2ConsumeProduct(
			budget.distanceEdgeTests, queries,
			static_cast<std::uint64_t>(edgeCount),
			kProbeMtsdfPhase2MaximumEdgeTests);
	}

	bool ProbeMtsdfPhase2ConsumeScanlines(ProbeMtsdfPhase2WorkBudget& budget,
		std::uint64_t shapeScanlines, std::uint64_t fieldScanlines,
		int width, int edgeCount)
	{
		if (width <= 0 || width > kProbeMtsdfPhase2MaximumWidth
			|| edgeCount <= 0 || edgeCount > kProbeMtsdfPhase2MaximumEdges)
		{
			return false;
		}
		if (!ProbeMtsdfPhase2ConsumeProduct(budget.scanlineEdgeTests,
			shapeScanlines, static_cast<std::uint64_t>(edgeCount),
			kProbeMtsdfPhase2MaximumScanlineEdgeTests)
			|| !ProbeMtsdfPhase2ConsumeProduct(budget.fieldScanWork,
				fieldScanlines, static_cast<std::uint64_t>(width),
				kProbeMtsdfPhase2MaximumFieldScanWork))
		{
			return false;
		}
		const std::uint64_t shapeItems = 3u * static_cast<std::uint64_t>(edgeCount);
		const std::uint64_t fieldItems =
			3u * static_cast<std::uint64_t>(width - 1) + 1u;
		return ProbeMtsdfPhase2ConsumeProduct(budget.sortWork, shapeScanlines,
			shapeItems * ProbeMtsdfPhase2CeilLog2(shapeItems),
			kProbeMtsdfPhase2MaximumSortWork)
			&& ProbeMtsdfPhase2ConsumeProduct(budget.sortWork, fieldScanlines,
				fieldItems * ProbeMtsdfPhase2CeilLog2(fieldItems),
				kProbeMtsdfPhase2MaximumSortWork);
	}

	bool ProbeMtsdfPhase2CandidatePrecedes(
		const ProbeMtsdfPhase2Candidate& left,
		const ProbeMtsdfPhase2Candidate& right)
	{
		return left.score != right.score
			? left.score > right.score : left.pixelIndex < right.pixelIndex;
	}

	void ProbeMtsdfPhase2InsertCandidate(
		std::array<ProbeMtsdfPhase2Candidate,
			kProbeMtsdfPhase2MaximumTrials>& ranked,
		std::size_t& rankedCount, const ProbeMtsdfPhase2Candidate& candidate)
	{
		std::size_t position = 0;
		while (position < rankedCount
			&& ProbeMtsdfPhase2CandidatePrecedes(ranked[position], candidate))
		{
			++position;
		}
		if (position >= ranked.size())
			return;
		const std::size_t newCount = std::min(rankedCount + 1, ranked.size());
		for (std::size_t index = newCount - 1; index > position; --index)
			ranked[index] = ranked[index - 1];
		ranked[position] = candidate;
		rankedCount = newCount;
	}

	bool ProbeMtsdfPhase2BuildRanking(
		const msdfgen::Bitmap<float, 4>& field, const msdfgen::Shape& shape,
		const msdfgen::Projection& projection, msdfgen::FillRule fillRule,
		std::uint8_t spread,
		std::array<ProbeMtsdfPhase2Candidate,
			kProbeMtsdfPhase2MaximumTrials>& ranked,
		std::size_t& rankedCount)
	{
		rankedCount = 0;
		const int width = field.width();
		const int height = field.height();
		std::vector<double> scores(static_cast<std::size_t>(width) * height);
		const msdfgen::BitmapConstSection<float, 4> section = field;
		ExactShapeDistanceFinder distanceFinder(shape);
		for (int y = 0; y < height - 1; ++y)
		{
			for (int sampleY = 0; sampleY < kProbeMtsdfPhase2RankSamples; ++sampleY)
			{
				const double fy = (sampleY + 0.5) / kProbeMtsdfPhase2RankSamples;
				const double fieldY = y + 0.5 + fy;
				const double shapeY = projection.unprojectY(fieldY);
				if (!std::isfinite(shapeY))
					return false;
				msdfgen::Scanline scanline;
				shape.scanline(scanline, shapeY);
				for (int x = 0; x < width - 1; ++x)
				{
					for (int sampleX = 0; sampleX < kProbeMtsdfPhase2RankSamples; ++sampleX)
					{
						const double fx = (sampleX + 0.5) / kProbeMtsdfPhase2RankSamples;
						const double fieldX = x + 0.5 + fx;
						const double shapeX = projection.unprojectX(fieldX);
						if (!std::isfinite(shapeX))
							return false;
						float sample[4] = {};
						msdfgen::interpolate(sample, section,
							msdfgen::Point2(fieldX, fieldY));
						for (float channel : sample)
							if (!std::isfinite(channel))
								return false;
						if (!scanline.filled(shapeX, fillRule))
							continue;
						const double rgbDistance = std::clamp(
							(Median(sample[0], sample[1], sample[2]) - 0.5)
								* 2.0 * spread,
							-static_cast<double>(spread), static_cast<double>(spread));
						const double alphaDistance = std::clamp(
							(sample[3] - 0.5) * 2.0 * spread,
							-static_cast<double>(spread), static_cast<double>(spread));
						if (!std::isfinite(rgbDistance) || !std::isfinite(alphaDistance))
							return false;
						if (alphaDistance - rgbDistance <= kProbeMtsdfPhase2SevereError
							- kProbeMtsdfPhase2AlphaMargin)
						{
							continue;
						}
						const double weights[4] = {
							(1.0 - fx) * (1.0 - fy), fx * (1.0 - fy),
							(1.0 - fx) * fy, fx * fy,
						};
						const int cornerX[4] = { x, x + 1, x, x + 1 };
						const int cornerY[4] = { y, y, y + 1, y + 1 };
						double changedDistances[4] = {};
						bool hasPotentialImprovement = false;
						for (int corner = 0; corner < 4; ++corner)
						{
							const float* pixel = field(cornerX[corner], cornerY[corner]);
							float equalized[3] = { pixel[0], pixel[1], pixel[2] };
							if (!EqualizeRgbToOwnMedian(equalized))
							{
								changedDistances[corner] = rgbDistance;
								continue;
							}
							float changedSample[3] = {};
							for (int channel = 0; channel < 3; ++channel)
							{
								changedSample[channel] = static_cast<float>(sample[channel]
									+ weights[corner]
										* (equalized[channel] - pixel[channel]));
							}
							const float changedMedian = Median(changedSample[0],
								changedSample[1], changedSample[2]);
							if (!std::isfinite(changedMedian))
								return false;
							changedDistances[corner] = std::clamp(
								(changedMedian - 0.5) * 2.0 * spread,
								-static_cast<double>(spread), static_cast<double>(spread));
							hasPotentialImprovement = hasPotentialImprovement
								|| changedDistances[corner] > rgbDistance;
						}
						if (!hasPotentialImprovement)
							continue;
						const double rawDistance = distanceFinder.distance(
							projection.unproject(msdfgen::Point2(fieldX, fieldY)));
						if (!std::isfinite(rawDistance))
							return false;
						const double exactDistance = std::min<double>(spread,
							std::abs(rawDistance));
						if (exactDistance - rgbDistance <= kProbeMtsdfPhase2SevereError)
							continue;
						const double beforeError = std::abs(exactDistance - rgbDistance);
						for (int corner = 0; corner < 4; ++corner)
						{
							const double improvement = beforeError - std::abs(
								exactDistance - changedDistances[corner]);
							if (!std::isfinite(improvement))
								return false;
							if (improvement <= 0.0)
								continue;
							const std::size_t index = static_cast<std::size_t>(
								cornerY[corner]) * width + cornerX[corner];
							scores[index] = std::max(scores[index], improvement);
						}
					}
				}
			}
		}
		for (std::size_t index = 0; index < scores.size(); ++index)
		{
			if (scores[index] > 0.0 && std::isfinite(scores[index]))
			{
				ProbeMtsdfPhase2InsertCandidate(ranked, rankedCount,
					{ scores[index], static_cast<std::uint32_t>(index) });
			}
		}
		return true;
	}

	ProbeMtsdfPhase2ExactComparison ProbeMtsdfPhase2CompareCandidate(
		const msdfgen::Bitmap<float, 4>& field, const msdfgen::Shape& shape,
		const msdfgen::Projection& projection, msdfgen::FillRule fillRule,
		std::uint8_t spread, ExactShapeDistanceFinder& distanceFinder,
		int texelX, int texelY, const float replacement[3], int samplesPerAxis)
	{
		ProbeMtsdfPhase2ExactComparison comparison;
		if (!replacement || samplesPerAxis <= 0)
		{
			comparison.finite = false;
			return comparison;
		}
		const msdfgen::BitmapConstSection<float, 4> section = field;
		for (int cellY = std::max(texelY - 1, 0);
			cellY <= std::min(texelY, field.height() - 2); ++cellY)
		{
			for (int sampleY = 0; sampleY < samplesPerAxis; ++sampleY)
			{
				const double fy = (sampleY + 0.5) / samplesPerAxis;
				const double fieldY = cellY + 0.5 + fy;
				const double shapeY = projection.unprojectY(fieldY);
				if (!std::isfinite(shapeY))
				{
					comparison.finite = false;
					return comparison;
				}
				msdfgen::Scanline scanline;
				shape.scanline(scanline, shapeY);
				for (int cellX = std::max(texelX - 1, 0);
					cellX <= std::min(texelX, field.width() - 2); ++cellX)
				{
					for (int sampleX = 0; sampleX < samplesPerAxis; ++sampleX)
					{
						const double fx = (sampleX + 0.5) / samplesPerAxis;
						const double fieldX = cellX + 0.5 + fx;
						const double shapeX = projection.unprojectX(fieldX);
						if (!std::isfinite(shapeX))
						{
							comparison.finite = false;
							return comparison;
						}
						float beforeSample[4] = {};
						msdfgen::interpolate(beforeSample, section,
							msdfgen::Point2(fieldX, fieldY));
						float afterSample[3] = {};
						for (int channel = 0; channel < 3; ++channel)
						{
							const auto value = [&](int x, int y)
							{
								return x == texelX && y == texelY
									? replacement[channel] : field(x, y)[channel];
							};
							const float bottom = msdfgen::mix(value(cellX, cellY),
								value(cellX + 1, cellY), fx);
							const float top = msdfgen::mix(value(cellX, cellY + 1),
								value(cellX + 1, cellY + 1), fx);
							afterSample[channel] = msdfgen::mix(bottom, top, fy);
						}
						const float beforeMedian = Median(beforeSample[0],
							beforeSample[1], beforeSample[2]);
						const float afterMedian = Median(afterSample[0],
							afterSample[1], afterSample[2]);
						const double rawDistance = distanceFinder.distance(
							projection.unproject(msdfgen::Point2(fieldX, fieldY)));
						if (!std::isfinite(beforeMedian) || !std::isfinite(afterMedian)
							|| !std::isfinite(rawDistance))
						{
							comparison.finite = false;
							return comparison;
						}
						const bool expected = scanline.filled(shapeX, fillRule);
						const bool beforeActual = beforeMedian > 0.5f;
						const bool afterActual = afterMedian > 0.5f;
						comparison.before.falseOutside += expected && !beforeActual;
						comparison.before.falseInside += !expected && beforeActual;
						comparison.after.falseOutside += expected && !afterActual;
						comparison.after.falseInside += !expected && afterActual;
						comparison.preservesCorrectSigns = comparison.preservesCorrectSigns
							&& (beforeActual != expected || afterActual == expected);
						const double unsignedDistance = std::min<double>(spread,
							std::abs(rawDistance));
						const double exactDistance = expected
							? unsignedDistance : -unsignedDistance;
						const double beforeDistance = std::clamp(
							(beforeMedian - 0.5) * 2.0 * spread,
							-static_cast<double>(spread), static_cast<double>(spread));
						const double afterDistance = std::clamp(
							(afterMedian - 0.5) * 2.0 * spread,
							-static_cast<double>(spread), static_cast<double>(spread));
						const double beforeError = std::abs(exactDistance - beforeDistance);
						const double afterError = std::abs(exactDistance - afterDistance);
						if (!std::isfinite(beforeError) || !std::isfinite(afterError))
						{
							comparison.finite = false;
							return comparison;
						}
						comparison.before.severeDistanceErrors +=
							beforeError > kProbeMtsdfPhase2SevereError;
						comparison.after.severeDistanceErrors +=
							afterError > kProbeMtsdfPhase2SevereError;
						comparison.before.absoluteDistanceErrorSum += beforeError;
						comparison.after.absoluteDistanceErrorSum += afterError;
						comparison.before.maximumAbsoluteDistanceError = std::max(
							comparison.before.maximumAbsoluteDistanceError, beforeError);
						comparison.after.maximumAbsoluteDistanceError = std::max(
							comparison.after.maximumAbsoluteDistanceError, afterError);
						++comparison.before.sampleCount;
						++comparison.after.sampleCount;
						const double increase = afterError - beforeError;
						if (increase > kProbeMtsdfPhase2Tolerance)
						{
							++comparison.worsenedSamples;
							comparison.maximumErrorIncrease = std::max(
								comparison.maximumErrorIncrease, increase);
							comparison.maximumWorsenedAfterError = std::max(
								comparison.maximumWorsenedAfterError, afterError);
						}
					}
				}
			}
		}
		comparison.finite = comparison.finite
			&& std::isfinite(comparison.before.absoluteDistanceErrorSum)
			&& std::isfinite(comparison.after.absoluteDistanceErrorSum)
			&& std::isfinite(comparison.before.maximumAbsoluteDistanceError)
			&& std::isfinite(comparison.after.maximumAbsoluteDistanceError)
			&& std::isfinite(comparison.maximumErrorIncrease)
			&& std::isfinite(comparison.maximumWorsenedAfterError);
		return comparison;
	}

	bool ProbeMtsdfPhase2StrictAggregateSafe(
		const CandidateShapeComparison& comparison)
	{
		return comparison.valid && !comparison.lostCorrectSign
			&& comparison.after.falseOutside <= comparison.before.falseOutside
			&& comparison.after.falseInside <= comparison.before.falseInside
			&& comparison.after.severeDistanceErrors
				<= comparison.before.severeDistanceErrors
			&& comparison.after.maximumAbsoluteDistanceError
				<= comparison.before.maximumAbsoluteDistanceError
					+ kProbeMtsdfPhase2Tolerance
			&& comparison.after.absoluteDistanceErrorSum
				< comparison.before.absoluteDistanceErrorSum
					- kProbeMtsdfPhase2Tolerance;
	}

	bool ProbeMtsdfPhase2BoundedSafe(
		const ProbeMtsdfPhase2ExactComparison& comparison, std::uint8_t spread)
	{
		// Half of one decoded BGRA8 distance step: .5*(2*spread/255).
		const double limit = static_cast<double>(spread) / 255.0;
		return comparison.finite && comparison.preservesCorrectSigns
			&& comparison.after.falseOutside <= comparison.before.falseOutside
			&& comparison.after.falseInside <= comparison.before.falseInside
			&& comparison.after.severeDistanceErrors
				<= comparison.before.severeDistanceErrors
			&& comparison.after.maximumAbsoluteDistanceError
				<= comparison.before.maximumAbsoluteDistanceError
					+ kProbeMtsdfPhase2Tolerance
			&& comparison.after.absoluteDistanceErrorSum
				< comparison.before.absoluteDistanceErrorSum
					- kProbeMtsdfPhase2Tolerance
			&& (!comparison.worsenedSamples
				|| (comparison.maximumErrorIncrease
						<= limit + kProbeMtsdfPhase2Tolerance
					&& comparison.maximumWorsenedAfterError
						<= limit + kProbeMtsdfPhase2Tolerance));
	}

	class ScopedProbeMtsdfPhase2Trial
	{
	public:
		ScopedProbeMtsdfPhase2Trial(float* pixel, const float replacement[3])
			: pixel_(pixel)
		{
			for (int channel = 0; channel < 3; ++channel)
			{
				old_[channel] = pixel_[channel];
				pixel_[channel] = replacement[channel];
			}
		}

		~ScopedProbeMtsdfPhase2Trial()
		{
			for (int channel = 0; channel < 3; ++channel)
				pixel_[channel] = old_[channel];
		}

		ScopedProbeMtsdfPhase2Trial(const ScopedProbeMtsdfPhase2Trial&) = delete;
		ScopedProbeMtsdfPhase2Trial& operator=(
			const ScopedProbeMtsdfPhase2Trial&) = delete;

	private:
		float* pixel_ = nullptr;
		float old_[3] = {};
	};

	float ProbeMtsdfPhase2BaselineChannel(
		const msdfgen::Bitmap<float, 4>& field,
		const std::array<ProbeMtsdfPhase2AcceptedPixel,
			kProbeMtsdfPhase2MaximumTrials>& acceptedPixels,
		std::size_t acceptedCount, int x, int y, int channel)
	{
		const std::uint32_t pixelIndex = static_cast<std::uint32_t>(
			static_cast<std::size_t>(y) * field.width() + x);
		for (std::size_t index = 0; index < acceptedCount; ++index)
		{
			if (acceptedPixels[index].pixelIndex == pixelIndex)
				return acceptedPixels[index].rgb[channel];
		}
		return field(x, y)[channel];
	}

	bool ProbeMtsdfPhase2AddActiveCell(
		std::array<std::uint32_t, kProbeMtsdfPhase2MaximumActiveCells>& cells,
		std::size_t& cellCount, std::uint32_t cellIndex)
	{
		for (std::size_t index = 0; index < cellCount; ++index)
			if (cells[index] == cellIndex)
				return true;
		if (cellCount >= cells.size())
			return false;
		cells[cellCount++] = cellIndex;
		return true;
	}

	bool ProbeMtsdfPhase2BuildActiveUnion(
		const std::array<ProbeMtsdfPhase2AcceptedPixel,
			kProbeMtsdfPhase2MaximumTrials>& acceptedPixels,
		std::size_t acceptedCount, int width, int height,
		std::array<std::uint32_t, kProbeMtsdfPhase2MaximumActiveCells>& cells,
		std::size_t& cellCount)
	{
		cellCount = 0;
		for (std::size_t accepted = 0; accepted < acceptedCount; ++accepted)
		{
			const int x = static_cast<int>(acceptedPixels[accepted].pixelIndex % width);
			const int y = static_cast<int>(acceptedPixels[accepted].pixelIndex / width);
			for (int cellY = std::max(y - 1, 0);
				cellY <= std::min(y, height - 2); ++cellY)
			{
				for (int cellX = std::max(x - 1, 0);
					cellX <= std::min(x, width - 2); ++cellX)
				{
					if (!ProbeMtsdfPhase2AddActiveCell(cells, cellCount,
						static_cast<std::uint32_t>(
							static_cast<std::size_t>(cellY) * (width - 1) + cellX)))
					{
						return false;
					}
				}
			}
		}
		std::sort(cells.begin(), cells.begin() + cellCount);
		return true;
	}

	ProbeMtsdfPhase2ExactComparison ProbeMtsdfPhase2CompareActiveUnion(
		const msdfgen::Bitmap<float, 4>& field, const msdfgen::Shape& shape,
		const msdfgen::Projection& projection, msdfgen::FillRule fillRule,
		std::uint8_t spread,
		const std::array<ProbeMtsdfPhase2AcceptedPixel,
			kProbeMtsdfPhase2MaximumTrials>& acceptedPixels,
		std::size_t acceptedCount,
		const std::array<std::uint32_t,
			kProbeMtsdfPhase2MaximumActiveCells>& activeCells,
		std::size_t activeCellCount, int samplesPerAxis)
	{
		ProbeMtsdfPhase2ExactComparison comparison;
		if (!acceptedCount || !activeCellCount || samplesPerAxis <= 0)
		{
			comparison.finite = false;
			return comparison;
		}
		const int cellWidth = field.width() - 1;
		const msdfgen::BitmapConstSection<float, 4> section = field;
		ExactShapeDistanceFinder distanceFinder(shape);
		for (std::size_t active = 0; active < activeCellCount; ++active)
		{
			const int cellX = static_cast<int>(activeCells[active] % cellWidth);
			const int cellY = static_cast<int>(activeCells[active] / cellWidth);
			for (int sampleY = 0; sampleY < samplesPerAxis; ++sampleY)
			{
				const double fy = (sampleY + 0.5) / samplesPerAxis;
				const double fieldY = cellY + 0.5 + fy;
				const double shapeY = projection.unprojectY(fieldY);
				if (!std::isfinite(shapeY))
				{
					comparison.finite = false;
					return comparison;
				}
				msdfgen::Scanline scanline;
				shape.scanline(scanline, shapeY);
				for (int sampleX = 0; sampleX < samplesPerAxis; ++sampleX)
				{
					const double fx = (sampleX + 0.5) / samplesPerAxis;
					const double fieldX = cellX + 0.5 + fx;
					const double shapeX = projection.unprojectX(fieldX);
					if (!std::isfinite(shapeX))
					{
						comparison.finite = false;
						return comparison;
					}
					float afterSample[4] = {};
					msdfgen::interpolate(afterSample, section,
						msdfgen::Point2(fieldX, fieldY));
					float beforeSample[3] = {};
					for (int channel = 0; channel < 3; ++channel)
					{
						const float bottom = msdfgen::mix(
							ProbeMtsdfPhase2BaselineChannel(field, acceptedPixels,
								acceptedCount, cellX, cellY, channel),
							ProbeMtsdfPhase2BaselineChannel(field, acceptedPixels,
								acceptedCount, cellX + 1, cellY, channel), fx);
						const float top = msdfgen::mix(
							ProbeMtsdfPhase2BaselineChannel(field, acceptedPixels,
								acceptedCount, cellX, cellY + 1, channel),
							ProbeMtsdfPhase2BaselineChannel(field, acceptedPixels,
								acceptedCount, cellX + 1, cellY + 1, channel), fx);
						beforeSample[channel] = msdfgen::mix(bottom, top, fy);
					}
					const float beforeMedian = Median(beforeSample[0],
						beforeSample[1], beforeSample[2]);
					const float afterMedian = Median(afterSample[0],
						afterSample[1], afterSample[2]);
					const double rawDistance = distanceFinder.distance(
						projection.unproject(msdfgen::Point2(fieldX, fieldY)));
					if (!std::isfinite(beforeMedian) || !std::isfinite(afterMedian)
						|| !std::isfinite(afterSample[3])
						|| !std::isfinite(rawDistance))
					{
						comparison.finite = false;
						return comparison;
					}
					const bool expected = scanline.filled(shapeX, fillRule);
					const bool beforeActual = beforeMedian > 0.5f;
					const bool afterActual = afterMedian > 0.5f;
					comparison.before.falseOutside += expected && !beforeActual;
					comparison.before.falseInside += !expected && beforeActual;
					comparison.after.falseOutside += expected && !afterActual;
					comparison.after.falseInside += !expected && afterActual;
					comparison.preservesCorrectSigns = comparison.preservesCorrectSigns
						&& (beforeActual != expected || afterActual == expected);
					const double unsignedDistance = std::min<double>(spread,
						std::abs(rawDistance));
					const double exactDistance = expected
						? unsignedDistance : -unsignedDistance;
					const double beforeDistance = std::clamp(
						(beforeMedian - 0.5) * 2.0 * spread,
						-static_cast<double>(spread), static_cast<double>(spread));
					const double afterDistance = std::clamp(
						(afterMedian - 0.5) * 2.0 * spread,
						-static_cast<double>(spread), static_cast<double>(spread));
					const double beforeError = std::abs(exactDistance - beforeDistance);
					const double afterError = std::abs(exactDistance - afterDistance);
					if (!std::isfinite(beforeError) || !std::isfinite(afterError))
					{
						comparison.finite = false;
						return comparison;
					}
					comparison.before.severeDistanceErrors +=
						beforeError > kProbeMtsdfPhase2SevereError;
					comparison.after.severeDistanceErrors +=
						afterError > kProbeMtsdfPhase2SevereError;
					comparison.before.absoluteDistanceErrorSum += beforeError;
					comparison.after.absoluteDistanceErrorSum += afterError;
					comparison.before.maximumAbsoluteDistanceError = std::max(
						comparison.before.maximumAbsoluteDistanceError, beforeError);
					comparison.after.maximumAbsoluteDistanceError = std::max(
						comparison.after.maximumAbsoluteDistanceError, afterError);
					++comparison.before.sampleCount;
					++comparison.after.sampleCount;
					for (std::size_t coverage = 0;
						coverage < kProbeMtsdfPhase2CoverageWidths.size(); ++coverage)
					{
						const double width = kProbeMtsdfPhase2CoverageWidths[coverage];
						const double exactCoverage = std::clamp(
							0.5 + exactDistance / (2.0 * width), 0.0, 1.0);
						const double beforeCoverage = std::clamp(
							0.5 + beforeDistance / (2.0 * width), 0.0, 1.0);
						const double afterCoverage = std::clamp(
							0.5 + afterDistance / (2.0 * width), 0.0, 1.0);
						const double beforeCoverageError = std::abs(
							exactCoverage - beforeCoverage);
						const double afterCoverageError = std::abs(
							exactCoverage - afterCoverage);
						if (!std::isfinite(beforeCoverageError)
							|| !std::isfinite(afterCoverageError))
						{
							comparison.finite = false;
							return comparison;
						}
						auto& before = comparison.beforeCoverage[coverage];
						auto& after = comparison.afterCoverage[coverage];
						before.absoluteErrorSum += beforeCoverageError;
						after.absoluteErrorSum += afterCoverageError;
						before.maximumAbsoluteError = std::max(
							before.maximumAbsoluteError, beforeCoverageError);
						after.maximumAbsoluteError = std::max(
							after.maximumAbsoluteError, afterCoverageError);
						++before.sampleCount;
						++after.sampleCount;
					}
				}
			}
		}
		comparison.finite = comparison.finite
			&& std::isfinite(comparison.before.absoluteDistanceErrorSum)
			&& std::isfinite(comparison.after.absoluteDistanceErrorSum)
			&& std::isfinite(comparison.before.maximumAbsoluteDistanceError)
			&& std::isfinite(comparison.after.maximumAbsoluteDistanceError);
		for (std::size_t coverage = 0;
			coverage < kProbeMtsdfPhase2CoverageWidths.size(); ++coverage)
		{
			comparison.finite = comparison.finite
				&& std::isfinite(comparison.beforeCoverage[coverage].absoluteErrorSum)
				&& std::isfinite(comparison.afterCoverage[coverage].absoluteErrorSum)
				&& std::isfinite(
					comparison.beforeCoverage[coverage].maximumAbsoluteError)
				&& std::isfinite(
					comparison.afterCoverage[coverage].maximumAbsoluteError);
		}
		return comparison;
	}

	bool ProbeMtsdfPhase2ActiveUnionSafe(
		const ProbeMtsdfPhase2ExactComparison& comparison)
	{
		if (!comparison.finite || !comparison.preservesCorrectSigns
			|| comparison.after.falseOutside > comparison.before.falseOutside
			|| comparison.after.falseInside > comparison.before.falseInside
			|| comparison.after.severeDistanceErrors
				> comparison.before.severeDistanceErrors
			|| comparison.after.maximumAbsoluteDistanceError
				> comparison.before.maximumAbsoluteDistanceError
					+ kProbeMtsdfPhase2Tolerance
			|| comparison.after.absoluteDistanceErrorSum
				> comparison.before.absoluteDistanceErrorSum
					+ kProbeMtsdfPhase2Tolerance)
		{
			return false;
		}
		for (std::size_t coverage = 0;
			coverage < kProbeMtsdfPhase2CoverageWidths.size(); ++coverage)
		{
			if (comparison.afterCoverage[coverage].absoluteErrorSum
					> comparison.beforeCoverage[coverage].absoluteErrorSum
						+ kProbeMtsdfPhase2Tolerance
				|| comparison.afterCoverage[coverage].maximumAbsoluteError
					> comparison.beforeCoverage[coverage].maximumAbsoluteError
						+ kProbeMtsdfPhase2Tolerance)
			{
				return false;
			}
		}
		return true;
	}

	ProbeMtsdfPhase2CenterComparison ProbeMtsdfPhase2CompareCenters(
		const msdfgen::Bitmap<float, 4>& field, const msdfgen::Shape& shape,
		const msdfgen::Projection& projection, msdfgen::FillRule fillRule,
		const std::array<ProbeMtsdfPhase2AcceptedPixel,
			kProbeMtsdfPhase2MaximumTrials>& acceptedPixels,
		std::size_t acceptedCount)
	{
		ProbeMtsdfPhase2CenterComparison comparison;
		for (int y = 0; y < field.height(); ++y)
		{
			const double shapeY = projection.unprojectY(y + 0.5);
			if (!std::isfinite(shapeY))
			{
				comparison.finite = false;
				return comparison;
			}
			msdfgen::Scanline scanline;
			shape.scanline(scanline, shapeY);
			for (int x = 0; x < field.width(); ++x)
			{
				const double shapeX = projection.unprojectX(x + 0.5);
				if (!std::isfinite(shapeX))
				{
					comparison.finite = false;
					return comparison;
				}
				const float beforeMedian = Median(
					ProbeMtsdfPhase2BaselineChannel(field, acceptedPixels,
						acceptedCount, x, y, 0),
					ProbeMtsdfPhase2BaselineChannel(field, acceptedPixels,
						acceptedCount, x, y, 1),
					ProbeMtsdfPhase2BaselineChannel(field, acceptedPixels,
						acceptedCount, x, y, 2));
				const float* pixel = field(x, y);
				const float afterMedian = Median(pixel[0], pixel[1], pixel[2]);
				if (!std::isfinite(beforeMedian) || !std::isfinite(afterMedian)
					|| !std::isfinite(pixel[3]))
				{
					comparison.finite = false;
					return comparison;
				}
				const bool expected = scanline.filled(shapeX, fillRule);
				const bool beforeActual = beforeMedian > 0.5f;
				const bool afterActual = afterMedian > 0.5f;
				comparison.beforeFalseOutside += expected && !beforeActual;
				comparison.beforeFalseInside += !expected && beforeActual;
				comparison.afterFalseOutside += expected && !afterActual;
				comparison.afterFalseInside += !expected && afterActual;
				comparison.preservesCorrectSigns = comparison.preservesCorrectSigns
					&& (beforeActual != expected || afterActual == expected);
			}
		}
		return comparison;
	}

	bool ProbeMtsdfPhase2CentersSafe(
		const ProbeMtsdfPhase2CenterComparison& comparison)
	{
		return comparison.finite && comparison.preservesCorrectSigns
			&& comparison.afterFalseOutside <= comparison.beforeFalseOutside
			&& comparison.afterFalseInside <= comparison.beforeFalseInside;
	}

	double ProbeMtsdfPhase2EstimateRows(
		const msdfgen::Bitmap<float, 4>& field, const msdfgen::Shape& shape,
		const msdfgen::Projection& projection, msdfgen::FillRule fillRule,
		int texelY)
	{
		if (field.width() <= 1 || field.height() <= 1)
			return std::numeric_limits<double>::quiet_NaN();
		const int firstRow = std::max(texelY - 1, 0);
		const int lastRow = std::min(texelY, field.height() - 2);
		const double subRowSize = 1.0 / kScanlinesPerRow;
		const double xFrom = projection.unprojectX(0.5);
		const double xTo = projection.unprojectX(field.width() - 0.5);
		if (!std::isfinite(xFrom) || !std::isfinite(xTo) || !(xTo > xFrom))
			return std::numeric_limits<double>::quiet_NaN();
		const double overlapFactor = 1.0 / (xTo - xFrom);
		double error = 0.0;
		msdfgen::Scanline referenceScanline;
		msdfgen::Scanline fieldScanline;
		const msdfgen::BitmapConstSection<float, 4> section = field;
		for (int row = firstRow; row <= lastRow; ++row)
		{
			for (int subRow = 0; subRow < kScanlinesPerRow; ++subRow)
			{
				const double y = projection.unprojectY(row + 0.5
					+ (subRow + 0.5) * subRowSize);
				if (!std::isfinite(y))
					return std::numeric_limits<double>::quiet_NaN();
				shape.scanline(referenceScanline, y);
				msdfgen::scanlineSDF(fieldScanline, section, projection, y,
					shape.getYAxisOrientation());
				const double contribution = 1.0 - overlapFactor
					* msdfgen::Scanline::overlap(referenceScanline, fieldScanline,
						xFrom, xTo, fillRule);
				if (!std::isfinite(contribution))
					return std::numeric_limits<double>::quiet_NaN();
				error += contribution;
				if (!std::isfinite(error))
					return std::numeric_limits<double>::quiet_NaN();
			}
		}
		const double normalized = error
			/ ((field.height() - 1.0) * kScanlinesPerRow);
		return std::isfinite(normalized)
			? normalized : std::numeric_limits<double>::quiet_NaN();
	}

	bool ApplyProbeMtsdfPhase2Impl(
		msdfgen::Bitmap<float, 4>& field, const msdfgen::Shape& shape,
		const msdfgen::Projection& projection, msdfgen::FillRule fillRule,
		std::uint8_t spread,
		std::array<ProbeMtsdfPhase2AcceptedPixel,
			kProbeMtsdfPhase2MaximumTrials>& acceptedPixels,
		std::size_t& acceptedCount, std::size_t& candidateCount)
	{
		acceptedCount = 0;
		candidateCount = 0;
		const int width = field.width();
		const int height = field.height();
		const int edgeCount = shape.edgeCount();
		if (fillRule == msdfgen::FILL_ODD || width < 3 || height < 3
			|| width > kProbeMtsdfPhase2MaximumWidth
			|| edgeCount <= 0 || edgeCount > kProbeMtsdfPhase2MaximumEdges
			|| shape.contours.size() > kProbeMtsdfPhase2MaximumContours)
		{
			return true;
		}
		const std::uint64_t pixelCount = static_cast<std::uint64_t>(width)
			* static_cast<std::uint64_t>(height);
		const std::uint64_t cellCount = static_cast<std::uint64_t>(width - 1)
			* static_cast<std::uint64_t>(height - 1);
		if (pixelCount > kProbeMtsdfPhase2MaximumPixels || !cellCount)
			return true;
		for (int y = 0; y < height; ++y)
		{
			for (int x = 0; x < width; ++x)
			{
				const float* pixel = field(x, y);
				if (!std::isfinite(pixel[0]) || !std::isfinite(pixel[1])
					|| !std::isfinite(pixel[2]) || !std::isfinite(pixel[3]))
				{
					return true;
				}
			}
		}

		ProbeMtsdfPhase2WorkBudget workBudget;
		const std::uint64_t rankingDistanceQueries = cellCount
			* kProbeMtsdfPhase2RankSamples * kProbeMtsdfPhase2RankSamples;
		const std::uint64_t rankingShapeScanlines =
			static_cast<std::uint64_t>(height - 1)
			* kProbeMtsdfPhase2RankSamples;
		if (!ProbeMtsdfPhase2ConsumeDistanceQueries(workBudget,
				rankingDistanceQueries, edgeCount)
			|| !ProbeMtsdfPhase2ConsumeScanlines(workBudget,
				rankingShapeScanlines, 0, width, edgeCount))
		{
			return true;
		}

		std::array<ProbeMtsdfPhase2Candidate,
			kProbeMtsdfPhase2MaximumTrials> ranked = {};
		std::size_t rankedCount = 0;
		if (!ProbeMtsdfPhase2BuildRanking(field, shape, projection, fillRule,
			spread, ranked, rankedCount))
		{
			return false;
		}
		candidateCount = rankedCount;
		if (!rankedCount)
			return true;

		ExactShapeDistanceFinder distanceFinder(shape);
		for (std::size_t rank = 0; rank < rankedCount; ++rank)
		{
			const std::uint32_t pixelIndex = ranked[rank].pixelIndex;
			if (pixelIndex >= pixelCount)
				return false;
			const int x = static_cast<int>(pixelIndex % width);
			const int y = static_cast<int>(pixelIndex / width);
			float* pixel = field(x, y);
			float replacement[3] = { pixel[0], pixel[1], pixel[2] };
			if (!EqualizeRgbToOwnMedian(replacement))
				continue;
			const std::uint64_t affectedColumns = static_cast<std::uint64_t>(x > 0)
				+ static_cast<std::uint64_t>(x < width - 1);
			const std::uint64_t affectedRows = static_cast<std::uint64_t>(y > 0)
				+ static_cast<std::uint64_t>(y < height - 1);
			const std::uint64_t affectedCells = affectedColumns * affectedRows;
			if (!affectedCells || !affectedRows)
				return false;

			const std::uint64_t strictQueries = affectedCells
				* kProbeMtsdfPhase2RankSamples * kProbeMtsdfPhase2RankSamples;
			const std::uint64_t strictScanlines = affectedRows
				* kProbeMtsdfPhase2RankSamples;
			if (!ProbeMtsdfPhase2ConsumeDistanceQueries(
					workBudget, strictQueries, edgeCount)
				|| !ProbeMtsdfPhase2ConsumeScanlines(workBudget,
					strictScanlines, 0, width, edgeCount))
			{
				return false;
			}
			const CandidateShapeComparison strict = CompareAffectedShapeGridErrors(
				field, shape, projection, fillRule, spread,
				distanceFinder, x, y, replacement);
			if (!strict.valid)
				return false;
			if (!ProbeMtsdfPhase2StrictAggregateSafe(strict))
				continue;

			bool rescueReason = false;
			if (!strict.everySampleSafe)
			{
				if (!ProbeMtsdfPhase2ConsumeDistanceQueries(
						workBudget, strictQueries, edgeCount)
					|| !ProbeMtsdfPhase2ConsumeScanlines(workBudget,
						strictScanlines, 0, width, edgeCount))
				{
					return false;
				}
				const ProbeMtsdfPhase2ExactComparison bounded4 =
					ProbeMtsdfPhase2CompareCandidate(field, shape, projection,
						fillRule, spread, distanceFinder, x, y, replacement,
						kProbeMtsdfPhase2RankSamples);
				if (!bounded4.finite)
					return false;
				if (!ProbeMtsdfPhase2BoundedSafe(bounded4, spread))
					continue;
				rescueReason = true;
			}
			else
			{
				const std::uint64_t contourScanlines = 2u * affectedRows
					* kScanlinesPerRow;
				if (!ProbeMtsdfPhase2ConsumeScanlines(workBudget,
						contourScanlines, contourScanlines, width, edgeCount))
				{
					return false;
				}
				const double beforeContourError = ProbeMtsdfPhase2EstimateRows(
					field, shape, projection, fillRule, y);
				double afterContourError = std::numeric_limits<double>::quiet_NaN();
				{
					ScopedProbeMtsdfPhase2Trial trial(pixel, replacement);
					afterContourError = ProbeMtsdfPhase2EstimateRows(
						field, shape, projection, fillRule, y);
				}
				if (!std::isfinite(beforeContourError)
					|| !std::isfinite(afterContourError))
				{
					return false;
				}
				if (afterContourError
					<= beforeContourError + kProbeMtsdfPhase2Tolerance)
				{
					continue;
				}
				rescueReason = true;
			}
			if (!rescueReason)
				continue;

			const std::uint64_t denseQueries = affectedCells
				* kProbeMtsdfPhase2DenseSamples * kProbeMtsdfPhase2DenseSamples;
			const std::uint64_t denseScanlines = affectedRows
				* kProbeMtsdfPhase2DenseSamples;
			if (!ProbeMtsdfPhase2ConsumeDistanceQueries(
					workBudget, denseQueries, edgeCount)
				|| !ProbeMtsdfPhase2ConsumeScanlines(workBudget,
					denseScanlines, 0, width, edgeCount))
			{
				return false;
			}
			const ProbeMtsdfPhase2ExactComparison independent8 =
				ProbeMtsdfPhase2CompareCandidate(field, shape, projection,
					fillRule, spread, distanceFinder, x, y, replacement,
					kProbeMtsdfPhase2DenseSamples);
			if (!independent8.finite)
				return false;
			if (!ProbeMtsdfPhase2BoundedSafe(independent8, spread))
				continue;
			if (acceptedCount >= acceptedPixels.size())
				return false;
			ProbeMtsdfPhase2AcceptedPixel& accepted = acceptedPixels[acceptedCount++];
			accepted.pixelIndex = pixelIndex;
			accepted.rgb[0] = pixel[0];
			accepted.rgb[1] = pixel[1];
			accepted.rgb[2] = pixel[2];
			accepted.alpha = pixel[3];
			pixel[0] = replacement[0];
			pixel[1] = replacement[1];
			pixel[2] = replacement[2];
		}
		if (!acceptedCount)
			return true;

		std::array<std::uint32_t,
			kProbeMtsdfPhase2MaximumActiveCells> activeCells = {};
		std::size_t activeCellCount = 0;
		if (!ProbeMtsdfPhase2BuildActiveUnion(acceptedPixels, acceptedCount,
			width, height, activeCells, activeCellCount) || !activeCellCount)
		{
			return false;
		}
		const std::uint64_t audit8Queries = activeCellCount
			* kProbeMtsdfPhase2DenseSamples * kProbeMtsdfPhase2DenseSamples;
		const std::uint64_t audit16Queries = activeCellCount
			* kProbeMtsdfPhase2AuditSamples * kProbeMtsdfPhase2AuditSamples;
		const std::uint64_t audit8Scanlines = activeCellCount
			* kProbeMtsdfPhase2DenseSamples;
		const std::uint64_t audit16Scanlines = activeCellCount
			* kProbeMtsdfPhase2AuditSamples;
		if (!ProbeMtsdfPhase2ConsumeDistanceQueries(
				workBudget, audit8Queries, edgeCount)
			|| !ProbeMtsdfPhase2ConsumeDistanceQueries(
				workBudget, audit16Queries, edgeCount)
			|| !ProbeMtsdfPhase2ConsumeScanlines(workBudget,
				audit8Scanlines + audit16Scanlines
					+ static_cast<std::uint64_t>(height),
				0, width, edgeCount))
		{
			return false;
		}
		const ProbeMtsdfPhase2ExactComparison audit8 =
			ProbeMtsdfPhase2CompareActiveUnion(field, shape, projection,
				fillRule, spread, acceptedPixels, acceptedCount,
				activeCells, activeCellCount, kProbeMtsdfPhase2DenseSamples);
		if (!ProbeMtsdfPhase2ActiveUnionSafe(audit8))
			return false;
		const ProbeMtsdfPhase2ExactComparison audit16 =
			ProbeMtsdfPhase2CompareActiveUnion(field, shape, projection,
				fillRule, spread, acceptedPixels, acceptedCount,
				activeCells, activeCellCount, kProbeMtsdfPhase2AuditSamples);
		if (!ProbeMtsdfPhase2ActiveUnionSafe(audit16))
			return false;
		const ProbeMtsdfPhase2CenterComparison centers =
			ProbeMtsdfPhase2CompareCenters(field, shape, projection, fillRule,
				acceptedPixels, acceptedCount);
		if (!ProbeMtsdfPhase2CentersSafe(centers))
			return false;
		for (std::size_t index = 0; index < acceptedCount; ++index)
		{
			const ProbeMtsdfPhase2AcceptedPixel& accepted = acceptedPixels[index];
			if (accepted.pixelIndex >= pixelCount)
				return false;
			const float* acceptedPixel = field(
				static_cast<int>(accepted.pixelIndex % width),
				static_cast<int>(accepted.pixelIndex / width));
			if (acceptedPixel[3] != accepted.alpha)
				return false;
		}
		return true;
	}

	std::size_t ApplyProbeMtsdfPhase2(
		msdfgen::Bitmap<float, 4>& field, const msdfgen::Shape& shape,
		const msdfgen::Projection& projection, msdfgen::FillRule fillRule,
		std::uint8_t spread, std::size_t& candidateCount, bool& fallback) noexcept
	{
		std::array<ProbeMtsdfPhase2AcceptedPixel,
			kProbeMtsdfPhase2MaximumTrials> acceptedPixels = {};
		std::size_t acceptedCount = 0;
		candidateCount = 0;
		fallback = false;
		bool keepChanges = false;
		try
		{
			keepChanges = ApplyProbeMtsdfPhase2Impl(field, shape, projection,
				fillRule, spread, acceptedPixels, acceptedCount, candidateCount);
		}
		catch (...)
		{
			keepChanges = false;
		}
		if (keepChanges)
			return acceptedCount;
		fallback = true;
		while (acceptedCount)
		{
			const ProbeMtsdfPhase2AcceptedPixel& accepted =
				acceptedPixels[--acceptedCount];
			const int width = field.width();
			if (width <= 0 || accepted.pixelIndex
				>= static_cast<std::uint64_t>(width) * field.height())
			{
				continue;
			}
			float* pixel = field(
				static_cast<int>(accepted.pixelIndex % width),
				static_cast<int>(accepted.pixelIndex / width));
			pixel[0] = accepted.rgb[0];
			pixel[1] = accepted.rgb[1];
			pixel[2] = accepted.rgb[2];
		}
		return 0;
	}

	std::size_t ApplyExactShapeRgbMedianRepairReference(
		msdfgen::Bitmap<float, 4>& field, const msdfgen::Shape& shape,
		const msdfgen::Projection& projection, msdfgen::FillRule fillRule,
		std::uint8_t spread,
		const std::vector<std::uint8_t>& baselineAlpha,
		std::size_t& candidateCount, bool& fallback)
	{
		const int samplesPerAxis = gShapeSamplesPerAxis;
		constexpr double correctionThreshold = 0.125;
		constexpr std::size_t maximumCandidateTrials = 64;
		constexpr double errorTolerance = 1e-12;
		const msdfgen::Bitmap<float, 4> baseline(field);
		const double baselineError = msdfgen::estimateSDFError(field, shape,
			projection, kScanlinesPerRow, fillRule);
		const std::size_t baselineCenters = CountCenterSignErrors(field,
			shape, projection, fillRule);
		const ShapeGridErrors baselineGrid = CountShapeGridErrors(field,
			shape, projection, fillRule, spread);

		std::vector<double> scores(static_cast<std::size_t>(field.width())
			* field.height());
		const msdfgen::BitmapConstSection<float, 4> section = field;
		ExactShapeDistanceFinder distanceFinder(shape);
		for (int y = 0; y < field.height() - 1; ++y)
		{
			for (int sampleY = 0; sampleY < samplesPerAxis; ++sampleY)
			{
				const double fy = (sampleY + 0.5) / samplesPerAxis;
				const double fieldY = y + 0.5 + fy;
				msdfgen::Scanline scanline;
				shape.scanline(scanline, projection.unprojectY(fieldY));
				for (int x = 0; x < field.width() - 1; ++x)
				{
					for (int sampleX = 0; sampleX < samplesPerAxis; ++sampleX)
					{
						const double fx = (sampleX + 0.5) / samplesPerAxis;
						const double fieldX = x + 0.5 + fx;
						float sample[4] = {};
						msdfgen::interpolate(sample, section,
							msdfgen::Point2(fieldX, fieldY));
						const double rgb = Median(sample[0], sample[1], sample[2]);
						const bool exactInside = scanline.filled(
							projection.unprojectX(fieldX), fillRule);
						const double rgbDistance = std::clamp(
							(rgb - 0.5) * 2.0 * spread,
							-static_cast<double>(spread),
							static_cast<double>(spread));
						const double rawDistance = distanceFinder.distance(
							projection.unproject(msdfgen::Point2(fieldX, fieldY)));
						if (!std::isfinite(rawDistance))
							return 0;
						const double unsignedDistance = std::min<double>(
							spread, std::abs(rawDistance));
						const double signedDistance = exactInside
							? unsignedDistance : -unsignedDistance;
						if (!exactInside
							|| signedDistance - rgbDistance <= correctionThreshold)
							continue;
						const double weights[4] = {
							(1.0 - fx) * (1.0 - fy), fx * (1.0 - fy),
							(1.0 - fx) * fy, fx * fy,
						};
						const int cornerX[4] = { x, x + 1, x, x + 1 };
						const int cornerY[4] = { y, y, y + 1, y + 1 };
						for (int corner = 0; corner < 4; ++corner)
						{
							const float* pixel = field(
								cornerX[corner], cornerY[corner]);
							float equalized[4] = {
								pixel[0], pixel[1], pixel[2], pixel[3]
							};
							if (!EqualizeRgbToOwnMedian(equalized))
								continue;
							float changedSample[3] = {};
							for (int channel = 0; channel < 3; ++channel)
							{
								changedSample[channel] = static_cast<float>(
									sample[channel] + weights[corner]
									* (equalized[channel] - pixel[channel]));
							}
							const double changedRgb = Median(changedSample[0],
								changedSample[1], changedSample[2]);
							const double changedDistance = std::clamp(
								(changedRgb - 0.5) * 2.0 * spread,
								-static_cast<double>(spread),
								static_cast<double>(spread));
							const double improvement =
								std::abs(signedDistance - rgbDistance)
								- std::abs(signedDistance - changedDistance);
							if (improvement <= 0.0)
								continue;
							const std::size_t index = static_cast<std::size_t>(
								cornerY[corner]) * field.width() + cornerX[corner];
							scores[index] = std::max(scores[index], improvement);
						}
					}
				}
			}
		}

		std::vector<std::pair<double, std::size_t>> ranked;
		for (std::size_t index = 0; index < scores.size(); ++index)
		{
			if (scores[index] > 0.0)
				ranked.emplace_back(scores[index], index);
		}
		std::sort(ranked.begin(), ranked.end(),
			[](const auto& left, const auto& right)
			{
				return left.first != right.first
					? left.first > right.first : left.second < right.second;
			});
		candidateCount = ranked.size();

		double currentError = baselineError;
		std::size_t currentCenters = baselineCenters;
		std::size_t accepted = 0;
		const std::size_t trials = std::min(
			ranked.size(), maximumCandidateTrials);
		for (std::size_t rank = 0; rank < trials; ++rank)
		{
			const std::size_t index = ranked[rank].second;
			const int x = static_cast<int>(index % field.width());
			const int y = static_cast<int>(index / field.width());
			const ShapeGridErrors before = CountAffectedShapeGridErrors(
				field, shape, projection, fillRule, spread, x, y);
			msdfgen::Bitmap<float, 4> trial(field);
			float* trialPixel = trial(x, y);
			if (!EqualizeRgbToOwnMedian(trialPixel))
				continue;
			const ShapeGridErrors after = CountAffectedShapeGridErrors(
				trial, shape, projection, fillRule, spread, x, y);
			const bool everySampleSafe =
				AffectedSampleDistanceErrorsDoNotIncrease(field, trial,
					shape, projection, fillRule, spread, x, y);
			const std::size_t alphaChanges = CountAlphaByteChanges(
				trial, baselineAlpha);
			const std::size_t trialCenters = CountCenterSignErrors(trial,
				shape, projection, fillRule);
			const double trialError = msdfgen::estimateSDFError(trial, shape,
				projection, kScanlinesPerRow, fillRule);
			const char* rejection = nullptr;
			if (after.falseOutside > before.falseOutside)
				rejection = "false-outside";
			else if (after.falseInside > before.falseInside)
				rejection = "false-inside";
			else if (after.severeDistanceErrors > before.severeDistanceErrors)
				rejection = "severe";
			else if (after.maximumAbsoluteDistanceError
				> before.maximumAbsoluteDistanceError + errorTolerance)
				rejection = "max";
			else if (after.absoluteDistanceErrorSum
				>= before.absoluteDistanceErrorSum - errorTolerance)
				rejection = "mae";
			else if (!everySampleSafe)
				rejection = "per-sample";
			else if (alphaChanges)
				rejection = "alpha";
			else if (trialCenters > currentCenters)
				rejection = "center-sign";
			else if (trialError > currentError + errorTolerance)
				rejection = "shape-error";

			if (rejection)
				continue;
			field = std::move(trial);
			currentError = trialError;
			currentCenters = trialCenters;
			++accepted;
		}

		const ShapeGridErrors finalGrid = CountShapeGridErrors(field,
			shape, projection, fillRule, spread);
		const double finalError = msdfgen::estimateSDFError(field, shape,
			projection, kScanlinesPerRow, fillRule);
		const std::size_t finalCenters = CountCenterSignErrors(field,
			shape, projection, fillRule);
		fallback = CountAlphaByteChanges(field, baselineAlpha)
			|| finalError > baselineError + errorTolerance
			|| finalCenters > baselineCenters
			|| finalGrid.falseOutside > baselineGrid.falseOutside
			|| finalGrid.falseInside > baselineGrid.falseInside
			|| finalGrid.severeDistanceErrors
				> baselineGrid.severeDistanceErrors
			|| finalGrid.maximumAbsoluteDistanceError
				> baselineGrid.maximumAbsoluteDistanceError + errorTolerance
			|| finalGrid.absoluteDistanceErrorSum
				> baselineGrid.absoluteDistanceErrorSum + errorTolerance;
		if (fallback)
		{
			field = baseline;
			return 0;
		}
		return accepted;
	}

	bool GenerateMtsdf(FT_Face face, const GlyphCase& glyph, bool hinted,
		Coloring coloring, Correction correction, MtsdfResult& output)
	{
		const auto started = std::chrono::steady_clock::now();
		if (!LoadOutline(face, glyph, hinted)
			|| !PrepareShape(face->glyph->outline, output.shape)
			|| !ResolveBounds(output.shape, glyph.spread, output.bounds))
		{
			return false;
		}
		ColorShape(output.shape, coloring);
		output.field = msdfgen::Bitmap<float, 4>(
			output.bounds.width, output.bounds.height);
		output.projection = msdfgen::Projection(msdfgen::Vector2(1.0),
			msdfgen::Vector2(-output.bounds.left, -output.bounds.bottom));
		const msdfgen::SDFTransformation transformation(output.projection,
			msdfgen::Range(-static_cast<double>(glyph.spread),
				static_cast<double>(glyph.spread)));
		output.fillRule = face->glyph->outline.flags & FT_OUTLINE_EVEN_ODD_FILL
			? msdfgen::FILL_ODD : msdfgen::FILL_NONZERO;

		msdfgen::MSDFGeneratorConfig generationConfig;
		generationConfig.overlapSupport = true;
		generationConfig.errorCorrection.mode =
			msdfgen::ErrorCorrectionConfig::DISABLED;
		msdfgen::generateMTSDF(output.field, output.shape, transformation,
			generationConfig);
		msdfgen::distanceSignCorrection(output.field, output.shape,
			output.projection, 0.5f, output.fillRule);
		const msdfgen::MSDFGeneratorConfig correctionConfig =
			CorrectionConfig(correction);
		msdfgen::msdfErrorCorrection(output.field, output.shape,
			transformation, correctionConfig);
		msdfgen::simulate8bit(output.field);
		const std::vector<std::uint8_t> baselineAlpha =
			CaptureAlphaBytes(output.field);
		if (correction == Correction::PostQuantControl)
		{
			msdfgen::distanceSignCorrection(output.field, output.shape,
				output.projection, 0.5f, output.fillRule);
			msdfgen::msdfErrorCorrection(output.field, output.shape,
				transformation, CorrectionConfig(Correction::Fast));
			msdfgen::simulate8bit(output.field);
		}
		else if (correction == Correction::AlphaCopyExperiment
			&& (output.modifiedTexelCount = ApplyAlphaCopyExperiment(
				output.field, output.shape,
				output.projection, output.fillRule, glyph.previewScale,
				glyph.spread)))
		{
			msdfgen::distanceSignCorrection(output.field, output.shape,
				output.projection, 0.5f, output.fillRule);
			msdfgen::msdfErrorCorrection(output.field, output.shape,
				transformation, CorrectionConfig(Correction::Fast));
			msdfgen::simulate8bit(output.field);
		}
		else if (correction == Correction::ExactShapeRepairReference)
		{
			output.modifiedTexelCount = ApplyExactShapeRgbMedianRepairReference(
				output.field,
				output.shape, output.projection, output.fillRule,
				glyph.spread, baselineAlpha,
				output.candidateTexelCount, output.exactShapeRepairFallback);
		}
		else if (correction == Correction::ExactShapeRepair)
		{
			output.modifiedTexelCount = ApplyExactShapeRgbMedianRepair(
				output.field, output.shape, output.projection, output.fillRule,
				glyph.spread, 8, output.candidateTexelCount,
				output.exactShapeRepairFallback);
			// Freeze the complete revision-6 phase above, then run the independent
			// revision-7 rescue reference. Phase 2 assigns only RGB; the Alpha
			// snapshot/byte count below verifies that Alpha remains unchanged.
			const std::vector<std::uint8_t> frozenPhase1Bytes =
				CaptureMtsdfRgbaBytes(output.field);
			output.phase2ModifiedTexelCount = ApplyProbeMtsdfPhase2(
				output.field, output.shape, output.projection, output.fillRule,
				glyph.spread, output.phase2CandidateTexelCount,
				output.phase2Fallback);
			if (!output.phase2ModifiedTexelCount)
			{
				output.phase2NoRescueByteParityFailures =
					CountMtsdfRgbaByteChanges(output.field, frozenPhase1Bytes);
				if (output.phase2NoRescueByteParityFailures)
				{
					std::cerr << "phase-2 no-rescue byte parity failure: "
						<< glyph.label << " changes="
						<< output.phase2NoRescueByteParityFailures << '\n';
					return false;
				}
			}
			output.modifiedTexelCount += output.phase2ModifiedTexelCount;
			output.candidateTexelCount += output.phase2CandidateTexelCount;
			// Production always uses Simple edge coloring.  InkTrap and
			// ByDistance remain deliberately different comparison candidates,
			// so byte parity is meaningful only for the production coloring.
			if (coloring == Coloring::Simple && !MatchesProductionMtsdf(
				face->glyph->outline, glyph.spread, output))
			{
				std::cerr << "production MTSDF mismatch: "
					<< glyph.label << '\n';
				return false;
			}
		}
		output.alphaByteChanges = CountAlphaByteChanges(
			output.field, baselineAlpha);

		output.error = msdfgen::estimateSDFError(output.field, output.shape,
			output.projection, kScanlinesPerRow, output.fillRule);
		msdfgen::Bitmap<float, 1> alpha(
			output.field.width(), output.field.height());
		for (int y = 0; y < output.field.height(); ++y)
		{
			for (int x = 0; x < output.field.width(); ++x)
				*alpha(x, y) = output.field(x, y)[3];
		}
		output.alphaError = msdfgen::estimateSDFError(alpha, output.shape,
			output.projection, kScanlinesPerRow, output.fillRule);
		output.centerSignErrors = CountCenterSignErrors(output.field,
			output.shape, output.projection, output.fillRule);
		MeasureRgbAlphaCoverage(output.field, glyph.previewScale, glyph.spread,
			output.maximumCoverageDeficit,
			output.severeCoverageDeficitFraction,
			output.meanAbsoluteCoverageDifference,
			output.rgbAlphaSignDisagreementFraction);
		const ShapeGridErrors shapeGrid = CountShapeGridErrors(output.field,
			output.shape, output.projection, output.fillRule, glyph.spread);
		output.falseOutsideSamples = shapeGrid.falseOutside;
		output.falseInsideSamples = shapeGrid.falseInside;
		const double shapeSampleCount = static_cast<double>(
			std::max<std::size_t>(shapeGrid.sampleCount, 1));
		output.exactDistanceMae = shapeGrid.absoluteDistanceErrorSum
			/ shapeSampleCount;
		output.exactSevereDistanceFraction = shapeGrid.severeDistanceErrors
			/ shapeSampleCount;
		output.exactMaximumDistanceError =
			shapeGrid.maximumAbsoluteDistanceError;
		output.milliseconds = std::chrono::duration<double, std::milli>(
			std::chrono::steady_clock::now() - started).count();
		return true;
	}

	bool GenerateSdf(FT_Face face, const GlyphCase& glyph, bool hinted,
		TsdfQuantization quantization, SdfResult& output)
	{
		const auto started = std::chrono::steady_clock::now();
		if (!LoadOutline(face, glyph, hinted)
			|| !PrepareShape(face->glyph->outline, output.shape)
			|| !ResolveBounds(output.shape, glyph.spread, output.bounds))
		{
			return false;
		}
		output.field = msdfgen::Bitmap<float, 1>(
			output.bounds.width, output.bounds.height);
		output.projection = msdfgen::Projection(msdfgen::Vector2(1.0),
			msdfgen::Vector2(-output.bounds.left, -output.bounds.bottom));
		const msdfgen::SDFTransformation transformation(output.projection,
			msdfgen::Range(-static_cast<double>(glyph.spread),
				static_cast<double>(glyph.spread)));
		output.fillRule = face->glyph->outline.flags & FT_OUTLINE_EVEN_ODD_FILL
			? msdfgen::FILL_ODD : msdfgen::FILL_NONZERO;
		msdfgen::generateSDF(output.field, output.shape, transformation,
			msdfgen::GeneratorConfig(true));
		msdfgen::distanceSignCorrection(output.field, output.shape,
			output.projection, 0.5f, output.fillRule);
		const int width = output.field.width();
		const int height = output.field.height();
		std::vector<float> continuousField(
			static_cast<std::size_t>(width) * height);
		std::vector<std::uint8_t> bytes(continuousField.size());
		for (int y = 0; y < height; ++y)
		{
			for (int x = 0; x < width; ++x)
			{
				const float encoded = *output.field(x, y);
				if (!std::isfinite(encoded))
					return false;
				const std::size_t index = static_cast<std::size_t>(y) * width + x;
				continuousField[index] = encoded;
				bytes[index] = QuantizeTsdf(encoded, quantization);
			}
		}
		const std::vector<std::uint8_t> baselineBytes = bytes;
		TsdfReferenceMetrics baselineAnalytic;
		TsdfReferenceMetrics finalAnalytic;
		if (quantization == TsdfQuantization::Runtime256Repaired)
		{
			TsdfReferenceRepairStats repairStats;
			if (!ApplyTsdfReferenceRepair(bytes, continuousField,
					width, height, glyph.spread, output.shape,
					output.projection, output.fillRule, repairStats)
				|| !AuditTsdfReferenceAnalytic8(baselineBytes, bytes,
					width, height, glyph.spread, output.shape,
					output.projection, output.fillRule,
					baselineAnalytic, finalAnalytic))
			{
				return false;
			}
			if (!TsdfReferenceMetricsDoNotRegress(
				baselineAnalytic, finalAnalytic))
			{
				std::cerr << "independent analytic8 TSDF regression: "
					<< glyph.label << '\n';
				return false;
			}
			output.candidateTexelCount = repairStats.candidates;
			output.repairFallback = repairStats.fallback;
			for (std::size_t index = 0; index < bytes.size(); ++index)
				output.modifiedTexelCount += bytes[index] != baselineBytes[index];
			double productionMilliseconds = 0.0;
			if (!MatchesProductionTrueSdf(face->glyph->outline, glyph.spread,
				bytes, output.bounds, productionMilliseconds))
			{
				std::cerr << "production/repaired-reference true-SDF byte mismatch: "
					<< glyph.label << '\n';
				return false;
			}
			output.milliseconds = productionMilliseconds;
		}
		else if (quantization == TsdfQuantization::Runtime256)
		{
			if (!AuditTsdfReferenceAnalytic8(bytes, bytes, width, height,
				glyph.spread, output.shape, output.projection, output.fillRule,
				baselineAnalytic, finalAnalytic))
			{
				return false;
			}
		}
		// The deployed TSDF shader decodes a texel byte as
		// (byte/128-1)*spread. Preserve the D3D-normalized byte in output.field
		// for previews; map byte/256 to msdfgen's fixed 0.5 threshold so shape
		// error and center signs use the exact shader zero at byte 128.
		msdfgen::Bitmap<float, 1> runtimeField(
			width, height);
		for (int y = 0; y < height; ++y)
		{
			for (int x = 0; x < width; ++x)
			{
				const std::uint8_t byte = bytes[static_cast<std::size_t>(y)
					* width + x];
				*output.field(x, y) = byte / 255.0f;
				*runtimeField(x, y) = byte / 256.0f;
			}
		}
		if (quantization != TsdfQuantization::Runtime255)
		{
			output.falseOutsideSamples = finalAnalytic.falseOutside;
			output.falseInsideSamples = finalAnalytic.falseInside;
			const double samples = static_cast<double>(
				std::max<std::uint64_t>(finalAnalytic.samples, 1));
			output.exactDistanceMae = finalAnalytic.absoluteDistanceError / samples;
			output.exactSevereDistanceFraction =
				finalAnalytic.severeDistanceErrors / samples;
			output.exactMaximumDistanceError =
				finalAnalytic.maximumDistanceError;
		}
		output.error = msdfgen::estimateSDFError(runtimeField, output.shape,
			output.projection, kScanlinesPerRow, output.fillRule);
		output.centerSignErrors = CountCenterSignErrors(runtimeField,
			output.shape, output.projection, output.fillRule);
		if (quantization != TsdfQuantization::Runtime256Repaired)
		{
			output.milliseconds = std::chrono::duration<double, std::milli>(
				std::chrono::steady_clock::now() - started).count();
		}
		return true;
	}

	template <int Channels, typename Decoder>
	bool WritePreview(const std::filesystem::path& path,
		const msdfgen::Bitmap<float, Channels>& field,
		std::uint8_t spread, double sourceScale, bool trueSdf, Decoder decoder)
	{
		const int width = static_cast<int>(std::ceil(
			field.width() * kPreviewScale * sourceScale));
		const int height = static_cast<int>(std::ceil(
			field.height() * kPreviewScale * sourceScale));
		if (width <= 0 || height <= 0)
			return false;
		std::ofstream stream(path, std::ios::binary);
		if (!stream)
			return false;
		stream << "P5\n" << width << ' ' << height << "\n255\n";
		const msdfgen::BitmapConstSection<float, Channels> fieldSection = field;
		std::vector<std::uint8_t> row(static_cast<std::size_t>(width));
		const double samplingScale = kPreviewScale * sourceScale;
		const double antialiasWidth = 0.5 / std::max(sourceScale, 0.0001);
		for (int outputY = height - 1; outputY >= 0; --outputY)
		{
			for (int outputX = 0; outputX < width; ++outputX)
			{
				float sample[Channels] = {};
				msdfgen::interpolate(sample, fieldSection, msdfgen::Point2(
					(outputX + 0.5) / samplingScale,
					(outputY + 0.5) / samplingScale));
				const double encoded = decoder(sample);
				const double distance = trueSdf
					? (encoded * (255.0 / 128.0) - 1.0) * spread
					: (encoded - 0.5) * 2.0 * spread;
				const double coverage = std::clamp(
					0.5 + distance / (2.0 * antialiasWidth), 0.0, 1.0);
				row[outputX] = static_cast<std::uint8_t>(
					std::lround(coverage * 255.0));
			}
			stream.write(reinterpret_cast<const char*>(row.data()), row.size());
		}
		return static_cast<bool>(stream);
	}

	std::string VariantName(bool hinted, Coloring coloring,
		Correction correction)
	{
		const char* suffix = nullptr;
		switch (correction)
		{
		case Correction::Fast: suffix = "fast"; break;
		case Correction::EdgePriority105: suffix = "edge_priority_1_05"; break;
		case Correction::EdgePriority102: suffix = "edge_priority_1_02"; break;
		case Correction::EdgePriority101: suffix = "edge_priority_1_01"; break;
		case Correction::EdgePriority100: suffix = "edge_priority_1_00"; break;
		case Correction::IndiscriminateFast: suffix = "indiscriminate_fast"; break;
		case Correction::EdgeOnlyFast: suffix = "edge_only_fast"; break;
		case Correction::Mixed: suffix = "mixed"; break;
		case Correction::PostQuantControl: suffix = "post_quant_control"; break;
		case Correction::AlphaCopyExperiment: suffix = "alpha_copy_experiment"; break;
		case Correction::ExactShapeRepairReference:
			suffix = "exact_shape_repair_reference";
			break;
		case Correction::ExactShapeRepair: suffix = "exact_shape_repair"; break;
		}
		return std::string(hinted ? "hinted_" : "unhinted_")
			+ ColoringName(coloring)
			+ '_' + suffix;
	}

	bool RunCase(FT_Library library, const GlyphCase& glyph,
		const std::filesystem::path& outputDirectory, std::ofstream& csv,
		bool regressionOnly)
	{
		FT_Face face = nullptr;
		const std::string path = glyph.fontPath.string();
		if (FT_New_Face(library, path.c_str(), 0, &face))
		{
			std::cerr << "Failed to load font: " << path << '\n';
			return false;
		}
		if (FT_Select_Charmap(face, FT_ENCODING_UNICODE))
		{
			FT_Done_Face(face);
			return false;
		}

		bool ok = true;
		const std::vector<bool> hintingModes = regressionOnly
			? std::vector<bool>{ false }
			: std::vector<bool>{ false, true };
		for (bool hinted : hintingModes)
		{
			const std::vector<Coloring> colorings = regressionOnly
				? std::vector<Coloring>{ Coloring::Simple }
				:
				glyph.writePreviews || glyph.compareColorings
				? std::vector<Coloring>{ Coloring::Simple, Coloring::InkTrap,
					Coloring::ByDistance }
				: std::vector<Coloring>{ Coloring::Simple };
			const std::vector<Correction> corrections = regressionOnly
				? std::vector<Correction>{ Correction::Fast,
					Correction::ExactShapeRepairReference,
					Correction::ExactShapeRepair }
				: glyph.writePreviews
				? std::vector<Correction>{ Correction::Fast,
					Correction::EdgePriority105, Correction::EdgePriority102,
					Correction::EdgePriority101, Correction::EdgePriority100,
					Correction::IndiscriminateFast, Correction::EdgeOnlyFast,
					Correction::Mixed, Correction::PostQuantControl,
					Correction::AlphaCopyExperiment,
					Correction::ExactShapeRepairReference,
					Correction::ExactShapeRepair }
				: std::vector<Correction>{ Correction::Fast,
					Correction::EdgePriority105, Correction::EdgePriority102,
					Correction::EdgePriority101, Correction::EdgePriority100,
					Correction::IndiscriminateFast, Correction::EdgeOnlyFast,
					Correction::PostQuantControl,
					Correction::AlphaCopyExperiment,
					Correction::ExactShapeRepairReference,
					Correction::ExactShapeRepair };
			for (Coloring coloring : colorings)
			{
				for (Correction correction : corrections)
				{
					MtsdfResult result;
					const std::string variant = VariantName(
						hinted, coloring, correction);
					if (!GenerateMtsdf(face, glyph, hinted, coloring,
						correction, result))
					{
						std::cerr << "MTSDF generation failed: " << glyph.label
							<< ' ' << variant << '\n';
						ok = false;
						continue;
					}
					csv << glyph.label << ",mtsdf," << variant << ','
						<< std::setprecision(12) << result.error << ','
						<< result.alphaError << ',' << result.centerSignErrors << ','
						<< result.maximumCoverageDeficit << ','
						<< result.severeCoverageDeficitFraction << ','
						<< result.meanAbsoluteCoverageDifference << ','
						<< result.rgbAlphaSignDisagreementFraction << ','
						<< result.modifiedTexelCount << ','
						<< result.candidateTexelCount << ','
						<< result.phase2ModifiedTexelCount << ','
						<< result.phase2CandidateTexelCount << ','
						<< static_cast<int>(result.phase2Fallback) << ','
						<< result.phase2NoRescueByteParityFailures << ','
						<< result.alphaByteChanges << ','
						<< result.falseOutsideSamples << ','
						<< result.falseInsideSamples << ','
						<< result.exactDistanceMae << ','
						<< result.exactSevereDistanceFraction << ','
						<< result.exactMaximumDistanceError << ','
						<< static_cast<int>(result.exactShapeRepairFallback) << ','
						<< result.milliseconds << ',' << result.bounds.width << ','
						<< result.bounds.height << '\n';
					if (glyph.writePreviews)
					{
						const std::filesystem::path preview = outputDirectory
							/ (glyph.label + "_" + variant + ".pgm");
						ok = WritePreview(preview, result.field, glyph.spread,
							glyph.previewScale, false,
							[](const float* sample)
							{
								return Median(sample[0], sample[1], sample[2]);
							}) && ok;
					}
				}
			}
			const std::vector<TsdfQuantization> tsdfQuantizations = {
				TsdfQuantization::Runtime255,
				TsdfQuantization::Runtime256,
				TsdfQuantization::Runtime256Repaired,
			};
			for (TsdfQuantization quantization : tsdfQuantizations)
			{
				SdfResult result;
				const char* quantizationName = quantization
					== TsdfQuantization::Runtime255 ? "runtime255"
					: quantization == TsdfQuantization::Runtime256
						? "runtime256_unrepaired" : "runtime256_repaired";
				const std::string variant = std::string(
					hinted ? "hinted_" : "unhinted_")
					+ quantizationName;
				if (!GenerateSdf(face, glyph, hinted, quantization, result))
				{
					std::cerr << "TSDF generation failed: " << glyph.label
						<< ' ' << variant << '\n';
					ok = false;
					continue;
				}
				csv << glyph.label << ",tsdf," << variant << ','
					<< std::setprecision(12) << result.error << ",0,"
					<< result.centerSignErrors
					<< ",0,0,0,0," << result.modifiedTexelCount << ','
					<< result.candidateTexelCount << ",0,0,0,0,0,"
					<< result.falseOutsideSamples << ','
					<< result.falseInsideSamples << ','
					<< result.exactDistanceMae << ','
					<< result.exactSevereDistanceFraction << ','
					<< result.exactMaximumDistanceError << ','
					<< static_cast<int>(result.repairFallback) << ','
					<< result.milliseconds << ','
					<< result.bounds.width << ',' << result.bounds.height << '\n';
				if (glyph.writePreviews)
				{
					const std::filesystem::path preview = outputDirectory
						/ (glyph.label + "_tsdf_" + variant + ".pgm");
					ok = WritePreview(preview, result.field, glyph.spread,
						glyph.previewScale, true,
						[](const float* sample) { return sample[0]; }) && ok;
				}
			}
		}
		FT_Done_Face(face);
		return ok;
	}
}

int main(int argc, char** argv)
{
	if (argc != 6 && argc != 7)
	{
		std::cerr << "Usage: tnvse_font_quality_probe <output-dir> "
			"<FixedsysExcelsior.ttf> <SarasaUiSC-Bold.ttf> <monofonto.ttf> "
			"<Futura.ttf> [--focused|--targets|--targets4|"
			"--regression|--regression4]\n";
		return 2;
	}
	const bool focusedOnly = argc == 7
		&& std::string_view(argv[6]) == "--focused";
	const std::string_view option = argc == 7
		? std::string_view(argv[6]) : std::string_view();
	const bool targetsOnly = option == "--targets" || option == "--targets4";
	const bool regressionOnly = option == "--regression"
		|| option == "--regression4";
	gShapeSamplesPerAxis = option == "--targets4"
		|| option == "--regression4" ? 4 : 8;
	if (argc == 7 && !focusedOnly && !targetsOnly && !regressionOnly)
	{
		std::cerr << "Unknown option: " << argv[6] << '\n';
		return 2;
	}
	const std::filesystem::path outputDirectory = argv[1];
	std::error_code directoryError;
	std::filesystem::create_directories(outputDirectory, directoryError);
	if (directoryError)
	{
		std::cerr << "Failed to create output directory: "
			<< directoryError.message() << '\n';
		return 2;
	}

	FT_Library library = nullptr;
	if (FT_Init_FreeType(&library))
	{
		std::cerr << "FT_Init_FreeType failed\n";
		return 2;
	}
	std::ofstream csv(outputDirectory / "metrics.csv", std::ios::trunc);
	if (!csv)
	{
		FT_Done_FreeType(library);
		return 2;
	}
	csv << "glyph,method,variant,shape_error,alpha_error,center_sign_errors,"
		"max_coverage_deficit,severe_deficit_fraction,rgb_alpha_coverage_mae,"
		"rgb_alpha_sign_disagreement,modified_texels,candidate_texels,"
		"phase2_modified_texels,phase2_candidate_texels,phase2_fallback,"
		"phase2_no_rescue_byte_parity_failures,"
		"alpha_byte_changes,false_outside_samples,false_inside_samples,"
		"exact_distance_mae,exact_severe_distance_fraction,"
		"exact_maximum_distance_error,exact_shape_repair_fallback,"
		"milliseconds,width,height\n";

	std::vector<GlyphCase> cases = {
		{ argv[2], "fixedsys_t_32", static_cast<std::uint32_t>('t'), 32, 32, 4, 1.0 },
		{ argv[2], "fixedsys_r_32", static_cast<std::uint32_t>('r'), 32, 32, 4, 1.0 },
		{ argv[3], "sarasa_7e41_26", 0x7e41u, 26, 26, 5, 1.0 },
		{ argv[3], "sarasa_7e41_26_to_24", 0x7e41u, 26, 26, 5, 24.0 / 26.0 },
		{ argv[3], "sarasa_7e41_26_to_22", 0x7e41u, 26, 26, 5, 22.0 / 26.0 },
		{ argv[4], "monofonto_t_26", static_cast<std::uint32_t>('t'), 26, 26, 4, 1.0 },
		{ argv[4], "monofonto_r_26", static_cast<std::uint32_t>('r'), 26, 26, 4, 1.0 },
		{ argv[4], "monofonto_t_27", static_cast<std::uint32_t>('t'), 27, 27, 4, 1.0 },
		{ argv[4], "monofonto_r_27", static_cast<std::uint32_t>('r'), 27, 27, 4, 1.0 },
		{ argv[4], "monofonto_t_30", static_cast<std::uint32_t>('t'), 30, 30, 4, 1.0 },
		{ argv[4], "monofonto_r_30", static_cast<std::uint32_t>('r'), 30, 30, 4, 1.0 },
		{ argv[4], "monofonto_t_32", static_cast<std::uint32_t>('t'), 32, 32, 4, 1.0 },
		{ argv[4], "monofonto_r_32", static_cast<std::uint32_t>('r'), 32, 32, 4, 1.0 },
	};
	if (regressionOnly)
		cases.clear();
	if (targetsOnly)
	{
		cases.erase(std::remove_if(cases.begin(), cases.end(),
			[](const GlyphCase& glyph)
			{
				return glyph.label.rfind("monofonto_", 0) != 0;
			}), cases.end());
		for (GlyphCase& glyph : cases)
		{
			glyph.writePreviews = false;
			glyph.compareColorings = false;
		}
	}
	const auto appendAsciiCases = [&cases](const std::filesystem::path& path,
		std::string_view family, FT_UInt size)
	{
		for (std::uint32_t codePoint = 0x21; codePoint <= 0x7e; ++codePoint)
		{
			std::ostringstream label;
			label << "batch_" << family << '_' << size << "_U+"
				<< std::uppercase << std::hex << std::setw(4)
				<< std::setfill('0') << codePoint;
			cases.push_back({ path, label.str(), codePoint, size, size,
				4, 1.0, false, false });
		}
	};
	if (!focusedOnly && !targetsOnly)
	{
		for (FT_UInt size : { 26u, 27u, 30u, 32u })
			appendAsciiCases(argv[4], "monofonto", size);
		appendAsciiCases(argv[2], "fixedsys", 32);
		appendAsciiCases(argv[5], "futura", 52);
		appendAsciiCases(argv[3], "sarasa", 26);
		for (std::uint32_t index = 0; index < 64; ++index)
		{
			const std::uint32_t codePoint = 0x4e00u
				+ index * ((0x9fffu - 0x4e00u) / 63u);
			std::ostringstream label;
			label << "batch_sarasa_cjk_26_U+" << std::uppercase << std::hex
				<< std::setw(4) << std::setfill('0') << codePoint;
			cases.push_back({ argv[3], label.str(), codePoint, 26, 26,
				5, 1.0, false, true });
		}
	}
	bool ok = true;
	for (const GlyphCase& glyph : cases)
		ok = RunCase(library, glyph, outputDirectory, csv,
			regressionOnly) && ok;
	FT_Done_FreeType(library);
	return ok ? 0 : 1;
}

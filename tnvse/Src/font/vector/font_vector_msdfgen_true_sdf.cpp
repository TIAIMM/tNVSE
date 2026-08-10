#include "font_vector_msdfgen_detail.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <utility>
#include <vector>

#include <core/bitmap-interpolation.hpp>
#include <core/rasterization.h>
#include <core/sdf-error-estimation.h>

namespace fonthook::vectorfont::implementation::font_vector_msdfgen
{
		struct TrueSdfRepairCandidate
		{
			double score = 0.0;
			std::uint32_t pixelIndex = 0;
			std::int8_t delta = 0;
			std::uint8_t centerInside = 0;
		};

		struct TrueSdfActiveCell
		{
			std::uint32_t cellIndex = 0;
			std::array<float, 16> signedDistances = {};
			std::uint16_t distanceInsideBits = 0;
			std::uint64_t signInsideBits = 0;
		};

		struct TrueSdfGridMetrics
		{
			std::uint64_t samples = 0;
			std::uint64_t falseOutside = 0;
			std::uint64_t falseInside = 0;
			std::uint64_t severeDistanceErrors = 0;
			double absoluteDistanceError = 0.0;
			double maximumDistanceError = 0.0;
			std::array<double, kTrueSdfRepairAntialiasWidths.size()>
				absoluteCoverageError = {};
			std::array<double, kTrueSdfRepairAntialiasWidths.size()>
				maximumCoverageError = {};
		};

		struct TrueSdfSignMetrics
		{
			std::uint64_t falseOutside = 0;
			std::uint64_t falseInside = 0;
		};

		struct AcceptedTrueSdfByte
		{
			std::uint32_t pixelIndex = 0;
			std::uint8_t oldByte = 0;
		};

		static_assert(sizeof(TrueSdfRepairCandidate) == 16,
			"v145 true-SDF candidate layout changed");
		static_assert(sizeof(TrueSdfActiveCell) == 80,
			"v145 true-SDF active-cell layout changed");
		static_assert(sizeof(AcceptedTrueSdfByte) == 8,
			"v145 true-SDF accepted-byte layout changed");
		static_assert(kMaximumTrueSdfContinuousCandidates
				* sizeof(TrueSdfRepairCandidate)
			+ kMaximumTrueSdfActiveCells
				* (sizeof(TrueSdfActiveCell) + sizeof(std::uint32_t))
			+ kMaximumTrueSdfRepairCandidateTrials
				* sizeof(AcceptedTrueSdfByte)
			+ kTrueSdfRepairDynamicBudgetBytes
			<= kTrueSdfRepairPerWorkerScratchBudgetBytes,
			"v145 true-SDF owned-payload budget exceeded");

		double ClampTrueSdfDistance(double distance, std::uint8_t spread)
		{
			return std::clamp(distance, -static_cast<double>(spread),
				static_cast<double>(spread));
		}

		std::uint64_t CeilTrueSdfLog2(std::uint64_t value)
		{
			if (value <= 1)
				return 0;
			std::uint64_t exponent = 0;
			for (--value; value; value >>= 1)
				++exponent;
			return exponent;
		}

		bool TrueSdfSortWorkWithinBudget(std::uint64_t shapeScanlines,
			std::uint64_t fieldShapeRows, int width, int edgeCount)
		{
			if (width <= 0 || edgeCount <= 0)
				return false;
			const std::uint64_t shapeItems = 3u
				* static_cast<std::uint64_t>(edgeCount);
			const std::uint64_t shapeLog = CeilTrueSdfLog2(shapeItems);
			const std::uint64_t fieldItems = static_cast<std::uint64_t>(width);
			const std::uint64_t fieldLog = CeilTrueSdfLog2(fieldItems);
			if ((shapeLog && shapeItems
					> kMaximumTrueSdfRepairSortWork / shapeLog)
				|| (fieldLog && fieldItems
					> kMaximumTrueSdfRepairSortWork / fieldLog))
			{
				return false;
			}
			const std::uint64_t shapeWorkPerScanline = shapeItems * shapeLog;
			const std::uint64_t fieldWorkPerRow = fieldItems * fieldLog;
			if ((shapeWorkPerScanline && shapeScanlines
					> kMaximumTrueSdfRepairSortWork / shapeWorkPerScanline)
				|| (fieldWorkPerRow && fieldShapeRows
					> kMaximumTrueSdfRepairSortWork / fieldWorkPerRow))
			{
				return false;
			}
			const std::uint64_t shapeWork =
				shapeScanlines * shapeWorkPerScanline;
			const std::uint64_t fieldWork = fieldShapeRows * fieldWorkPerRow;
			return fieldWork <= kMaximumTrueSdfRepairSortWork - shapeWork;
		}

		double DecodeTrueSdfByte(double byteValue, std::uint8_t spread)
		{
			return ClampTrueSdfDistance(
				(byteValue / 128.0 - 1.0) * spread, spread);
		}

		double TrueSdfCoverage(double distance, double antialiasWidth)
		{
			return std::clamp(0.5 + distance / (2.0 * antialiasWidth),
				0.0, 1.0);
		}

		double BilinearTrueSdfByte(const std::vector<std::uint8_t>& bytes,
			int width, int cellX, int cellY, double fractionX, double fractionY)
		{
			const auto at = [&](int x, int y)
			{
				return static_cast<double>(bytes[static_cast<std::size_t>(y)
					* width + x]);
			};
			const double bottom = std::lerp(
				at(cellX, cellY), at(cellX + 1, cellY), fractionX);
			const double top = std::lerp(
				at(cellX, cellY + 1), at(cellX + 1, cellY + 1), fractionX);
			return std::lerp(bottom, top, fractionY);
		}

		double BilinearTrueSdfCandidateByte(
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
			// Keep the exact nested interpolation order used by the final field.
			// Delta reconstruction can round differently under /fp:fast and admit
			// a candidate that the byte-resampled field itself would reject.
			const double bottom = std::lerp(
				at(cellX, cellY), at(cellX + 1, cellY), fractionX);
			const double top = std::lerp(
				at(cellX, cellY + 1), at(cellX + 1, cellY + 1), fractionX);
			return std::lerp(bottom, top, fractionY);
		}

		bool AccumulateTrueSdfSample(TrueSdfGridMetrics& metrics,
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
			metrics.maximumDistanceError = std::max(
				metrics.maximumDistanceError, error);
			metrics.severeDistanceErrors +=
				error > kTrueSdfRepairSevereDistanceError;
			const bool actualInside = decodedDistance > 0.0;
			metrics.falseOutside += expectedInside && !actualInside;
			metrics.falseInside += !expectedInside && actualInside;
			for (std::size_t index = 0;
				index < kTrueSdfRepairAntialiasWidths.size(); ++index)
			{
				const double coverageError = std::abs(
					TrueSdfCoverage(exactDistance,
						kTrueSdfRepairAntialiasWidths[index])
					- TrueSdfCoverage(decodedDistance,
						kTrueSdfRepairAntialiasWidths[index]));
				if (!std::isfinite(coverageError))
					return false;
				metrics.absoluteCoverageError[index] += coverageError;
				metrics.maximumCoverageError[index] = std::max(
					metrics.maximumCoverageError[index], coverageError);
			}
			return std::isfinite(metrics.absoluteDistanceError);
		}

		bool TrueSdfMetricsDoNotRegress(const TrueSdfGridMetrics& before,
			const TrueSdfGridMetrics& after)
		{
			if (after.falseOutside > before.falseOutside
				|| after.falseInside > before.falseInside
				|| after.severeDistanceErrors > before.severeDistanceErrors
				|| after.maximumDistanceError
					> before.maximumDistanceError + kTrueSdfRepairErrorTolerance
				|| after.absoluteDistanceError
					> before.absoluteDistanceError + kTrueSdfRepairErrorTolerance)
			{
				return false;
			}
			for (std::size_t index = 0;
				index < kTrueSdfRepairAntialiasWidths.size(); ++index)
			{
				if (after.absoluteCoverageError[index]
						> before.absoluteCoverageError[index]
							+ kTrueSdfRepairErrorTolerance
					|| after.maximumCoverageError[index]
						> before.maximumCoverageError[index]
							+ kTrueSdfRepairErrorTolerance)
				{
					return false;
				}
			}
			return true;
		}

		bool MarkTrueSdfCandidateCells(std::vector<std::uint32_t>& cellIndices,
			int width, int height, int texelX, int texelY)
		{
			const int cellsWide = width - 1;
			for (int cellY = std::max(texelY - 1, 0);
				cellY <= std::min(texelY, height - 2); ++cellY)
			{
				for (int cellX = std::max(texelX - 1, 0);
					cellX <= std::min(texelX, width - 2); ++cellX)
				{
					const std::uint32_t cellIndex = static_cast<std::uint32_t>(
						static_cast<std::size_t>(cellY) * cellsWide + cellX);
					if (std::find(cellIndices.begin(), cellIndices.end(), cellIndex)
						!= cellIndices.end())
					{
						continue;
					}
					if (cellIndices.size() >= kMaximumTrueSdfActiveCells)
						return false;
					cellIndices.push_back(cellIndex);
				}
			}
			return true;
		}

		const TrueSdfActiveCell* FindTrueSdfActiveCell(
			const std::vector<TrueSdfActiveCell>& activeCells,
			std::uint32_t cellIndex)
		{
			const auto found = std::lower_bound(activeCells.begin(), activeCells.end(),
				cellIndex, [](const TrueSdfActiveCell& cell, std::uint32_t index)
				{
					return cell.cellIndex < index;
				});
			return found != activeCells.end() && found->cellIndex == cellIndex
				? &*found : nullptr;
		}

		bool ContinuousTrueSdfCenterPrefilter(
			const std::vector<std::uint8_t>& bytes,
			const msdfgen::Bitmap<float, 1>& continuousField,
			int width, int texelX, int texelY, int delta,
			std::uint8_t spread, bool expectedInside)
		{
			const std::size_t pixelIndex = static_cast<std::size_t>(texelY)
				* width + texelX;
			const int beforeByte = bytes[pixelIndex];
			const int afterByte = beforeByte + delta;
			if (afterByte < 0 || afterByte > 255)
				return false;
			const double encoded = *continuousField(texelX, texelY);
			if (!std::isfinite(encoded))
				return false;
			const double referenceDistance = ClampTrueSdfDistance(
				(encoded - 0.5) * 2.0 * spread, spread);
			const double beforeDistance = DecodeTrueSdfByte(beforeByte, spread);
			const double afterDistance = DecodeTrueSdfByte(afterByte, spread);
			return std::abs(referenceDistance - afterDistance)
					<= std::abs(referenceDistance - beforeDistance)
						+ kTrueSdfRepairErrorTolerance
				&& ((beforeDistance > 0.0) != expectedInside
					|| (afterDistance > 0.0) == expectedInside);
		}

		bool CheckExactTrueSdfCenterCandidate(
			const std::vector<std::uint8_t>& bytes,
			int width, int texelX, int texelY, int delta,
			std::uint8_t spread, bool expectedInside,
			const msdfgen::Projection& projection,
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
			const double beforeDistance = DecodeTrueSdfByte(beforeByte, spread);
			const double afterDistance = DecodeTrueSdfByte(afterByte, spread);
			if (!std::isfinite(exactDistance)
				|| std::abs(exactDistance - afterDistance)
					> std::abs(exactDistance - beforeDistance)
						+ kTrueSdfRepairErrorTolerance)
			{
				return std::isfinite(exactDistance);
			}
			safe = (beforeDistance > 0.0) != expectedInside
				|| (afterDistance > 0.0) == expectedInside;
			return true;
		}

		bool BuildTrueSdfActiveCellSamples(
			std::vector<TrueSdfActiveCell>& activeCells,
			const msdfgen::Shape& shape,
			const msdfgen::Projection& projection,
			msdfgen::FillRule fillRule, int width, std::uint8_t spread)
		{
			const int cellsWide = width - 1;
			ExactShapeDistanceFinder distanceFinder(shape);
			std::size_t rowBegin = 0;
			while (rowBegin < activeCells.size())
			{
				const int cellY = static_cast<int>(
					activeCells[rowBegin].cellIndex / cellsWide);
				std::size_t rowEnd = rowBegin + 1;
				while (rowEnd < activeCells.size()
					&& static_cast<int>(activeCells[rowEnd].cellIndex / cellsWide)
						== cellY)
				{
					++rowEnd;
				}
				for (int sampleY = 0;
					sampleY < kTrueSdfRepairDistanceSamplesPerAxis; ++sampleY)
				{
					const double fractionY = (sampleY + 0.5)
						/ kTrueSdfRepairDistanceSamplesPerAxis;
					const double fieldY = cellY + 0.5 + fractionY;
					const double shapeY = projection.unprojectY(fieldY);
					if (!std::isfinite(shapeY))
						return false;
					msdfgen::Scanline scanline;
					shape.scanline(scanline, shapeY);
					for (std::size_t cellPosition = rowBegin;
						cellPosition < rowEnd; ++cellPosition)
					{
						TrueSdfActiveCell& cell = activeCells[cellPosition];
						const int cellX = static_cast<int>(cell.cellIndex % cellsWide);
						for (int sampleX = 0;
							sampleX < kTrueSdfRepairDistanceSamplesPerAxis; ++sampleX)
						{
							const double fractionX = (sampleX + 0.5)
								/ kTrueSdfRepairDistanceSamplesPerAxis;
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
							const int sampleIndex = sampleY
								* kTrueSdfRepairDistanceSamplesPerAxis + sampleX;
							cell.signedDistances[sampleIndex] = static_cast<float>(
								inside ? unsignedDistance : -unsignedDistance);
							if (inside)
								cell.distanceInsideBits |= static_cast<std::uint16_t>(
									std::uint16_t{ 1 } << sampleIndex);
						}
					}
				}
				for (int sampleY = 0;
					sampleY < kTrueSdfRepairSignSamplesPerAxis; ++sampleY)
				{
					const double fractionY = (sampleY + 0.5)
						/ kTrueSdfRepairSignSamplesPerAxis;
					const double fieldY = cellY + 0.5 + fractionY;
					const double shapeY = projection.unprojectY(fieldY);
					if (!std::isfinite(shapeY))
						return false;
					msdfgen::Scanline scanline;
					shape.scanline(scanline, shapeY);
					for (std::size_t cellPosition = rowBegin;
						cellPosition < rowEnd; ++cellPosition)
					{
						TrueSdfActiveCell& cell = activeCells[cellPosition];
						const int cellX = static_cast<int>(cell.cellIndex % cellsWide);
						for (int sampleX = 0;
							sampleX < kTrueSdfRepairSignSamplesPerAxis; ++sampleX)
						{
							const double fractionX = (sampleX + 0.5)
								/ kTrueSdfRepairSignSamplesPerAxis;
							const double shapeX = projection.unprojectX(
								cellX + 0.5 + fractionX);
							if (!std::isfinite(shapeX))
								return false;
							const int sampleIndex = sampleY
								* kTrueSdfRepairSignSamplesPerAxis + sampleX;
							if (scanline.filled(shapeX, fillRule))
								cell.signInsideBits |= std::uint64_t{ 1 } << sampleIndex;
						}
					}
				}
				rowBegin = rowEnd;
			}
			return true;
		}

		bool EvaluateTrueSdfCandidateOnDistanceGrid(
			const std::vector<std::uint8_t>& bytes, int width, int height,
			std::uint8_t spread,
			const std::vector<TrueSdfActiveCell>& activeCells,
			int texelX, int texelY, int delta, double& improvement)
		{
			TrueSdfGridMetrics before;
			TrueSdfGridMetrics after;
			bool preservesCorrectSigns = true;
			bool everySampleSafe = true;
			const int cellsWide = width - 1;
			for (int cellY = std::max(texelY - 1, 0);
				cellY <= std::min(texelY, height - 2); ++cellY)
			{
				for (int cellX = std::max(texelX - 1, 0);
					cellX <= std::min(texelX, width - 2); ++cellX)
				{
					const std::uint32_t cellIndex = static_cast<std::uint32_t>(
						static_cast<std::size_t>(cellY) * cellsWide + cellX);
					const TrueSdfActiveCell* cell = FindTrueSdfActiveCell(
						activeCells, cellIndex);
					if (!cell)
						return false;
					for (int sampleY = 0;
						sampleY < kTrueSdfRepairDistanceSamplesPerAxis; ++sampleY)
					{
						const double fractionY = (sampleY + 0.5)
							/ kTrueSdfRepairDistanceSamplesPerAxis;
						for (int sampleX = 0;
							sampleX < kTrueSdfRepairDistanceSamplesPerAxis; ++sampleX)
						{
							const double fractionX = (sampleX + 0.5)
								/ kTrueSdfRepairDistanceSamplesPerAxis;
							const int sampleIndex = sampleY
								* kTrueSdfRepairDistanceSamplesPerAxis + sampleX;
							const bool expectedInside = (cell->distanceInsideBits
								& (std::uint16_t{ 1 } << sampleIndex)) != 0;
							const double exactDistance = cell->signedDistances[sampleIndex];
							const double beforeDistance = DecodeTrueSdfByte(
								BilinearTrueSdfByte(bytes, width, cellX, cellY,
									fractionX, fractionY), spread);
							const double afterDistance = DecodeTrueSdfByte(
								BilinearTrueSdfCandidateByte(bytes, width,
									cellX, cellY, fractionX, fractionY,
									texelX, texelY, delta), spread);
							if (!AccumulateTrueSdfSample(before, exactDistance,
								beforeDistance, expectedInside)
								|| !AccumulateTrueSdfSample(after, exactDistance,
									afterDistance, expectedInside))
							{
								return false;
							}
							const double beforeError = std::abs(
								exactDistance - beforeDistance);
							const double afterError = std::abs(
								exactDistance - afterDistance);
							everySampleSafe = everySampleSafe
								&& afterError <= beforeError
									+ kTrueSdfRepairErrorTolerance;
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
					> before.maximumDistanceError + kTrueSdfRepairErrorTolerance)
			{
				return false;
			}
			for (std::size_t index = 0;
				index < kTrueSdfRepairAntialiasWidths.size(); ++index)
			{
				if (after.absoluteCoverageError[index]
						> before.absoluteCoverageError[index]
							+ kTrueSdfRepairErrorTolerance
					|| after.maximumCoverageError[index]
						> before.maximumCoverageError[index]
							+ kTrueSdfRepairErrorTolerance)
				{
					return false;
				}
			}
			improvement = before.absoluteDistanceError - after.absoluteDistanceError;
			return std::isfinite(improvement)
				&& improvement > kTrueSdfRepairErrorTolerance;
		}

		bool EvaluateTrueSdfCandidateOnSignGrid(
			const std::vector<std::uint8_t>& bytes, int width, int height,
			std::uint8_t spread,
			const std::vector<TrueSdfActiveCell>& activeCells,
			int texelX, int texelY, int delta)
		{
			TrueSdfSignMetrics before;
			TrueSdfSignMetrics after;
			bool preservesCorrectSigns = true;
			const int cellsWide = width - 1;
			for (int cellY = std::max(texelY - 1, 0);
				cellY <= std::min(texelY, height - 2); ++cellY)
			{
				for (int cellX = std::max(texelX - 1, 0);
					cellX <= std::min(texelX, width - 2); ++cellX)
				{
					const std::uint32_t cellIndex = static_cast<std::uint32_t>(
						static_cast<std::size_t>(cellY) * cellsWide + cellX);
					const TrueSdfActiveCell* cell = FindTrueSdfActiveCell(
						activeCells, cellIndex);
					if (!cell)
						return false;
					for (int sampleY = 0;
						sampleY < kTrueSdfRepairSignSamplesPerAxis; ++sampleY)
					{
						const double fractionY = (sampleY + 0.5)
							/ kTrueSdfRepairSignSamplesPerAxis;
						for (int sampleX = 0;
							sampleX < kTrueSdfRepairSignSamplesPerAxis; ++sampleX)
						{
							const double fractionX = (sampleX + 0.5)
								/ kTrueSdfRepairSignSamplesPerAxis;
							const int sampleIndex = sampleY
								* kTrueSdfRepairSignSamplesPerAxis + sampleX;
							const bool expectedInside = (cell->signInsideBits
								& (std::uint64_t{ 1 } << sampleIndex)) != 0;
							const double beforeDistance = DecodeTrueSdfByte(
								BilinearTrueSdfByte(bytes, width, cellX, cellY,
									fractionX, fractionY), spread);
							const double afterDistance = DecodeTrueSdfByte(
								BilinearTrueSdfCandidateByte(bytes, width,
									cellX, cellY, fractionX, fractionY,
									texelX, texelY, delta), spread);
							if (!std::isfinite(beforeDistance)
								|| !std::isfinite(afterDistance))
							{
								return false;
							}
							const bool beforeInside = beforeDistance > 0.0;
							const bool afterInside = afterDistance > 0.0;
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

		bool MeasureActiveTrueSdfDistanceGrid(
			const std::vector<std::uint8_t>& bytes, int width,
			std::uint8_t spread,
			const std::vector<TrueSdfActiveCell>& activeCells,
			TrueSdfGridMetrics& metrics)
		{
			metrics = {};
			const int cellsWide = width - 1;
			for (const TrueSdfActiveCell& cell : activeCells)
			{
				const int cellX = static_cast<int>(cell.cellIndex % cellsWide);
				const int cellY = static_cast<int>(cell.cellIndex / cellsWide);
				for (int sampleY = 0;
					sampleY < kTrueSdfRepairDistanceSamplesPerAxis; ++sampleY)
				{
					const double fractionY = (sampleY + 0.5)
						/ kTrueSdfRepairDistanceSamplesPerAxis;
					for (int sampleX = 0;
						sampleX < kTrueSdfRepairDistanceSamplesPerAxis; ++sampleX)
					{
						const double fractionX = (sampleX + 0.5)
							/ kTrueSdfRepairDistanceSamplesPerAxis;
						const int sampleIndex = sampleY
							* kTrueSdfRepairDistanceSamplesPerAxis + sampleX;
						if (!AccumulateTrueSdfSample(metrics,
							cell.signedDistances[sampleIndex],
							DecodeTrueSdfByte(BilinearTrueSdfByte(bytes, width,
								cellX, cellY, fractionX, fractionY), spread),
							(cell.distanceInsideBits
								& (std::uint16_t{ 1 } << sampleIndex)) != 0))
						{
							return false;
						}
					}
				}
			}
			return true;
		}

		bool MeasureActiveTrueSdfSignGrid(
			const std::vector<std::uint8_t>& bytes, int width,
			std::uint8_t spread,
			const std::vector<TrueSdfActiveCell>& activeCells,
			TrueSdfSignMetrics& metrics)
		{
			metrics = {};
			const int cellsWide = width - 1;
			for (const TrueSdfActiveCell& cell : activeCells)
			{
				const int cellX = static_cast<int>(cell.cellIndex % cellsWide);
				const int cellY = static_cast<int>(cell.cellIndex / cellsWide);
				for (int sampleY = 0;
					sampleY < kTrueSdfRepairSignSamplesPerAxis; ++sampleY)
				{
					const double fractionY = (sampleY + 0.5)
						/ kTrueSdfRepairSignSamplesPerAxis;
					for (int sampleX = 0;
						sampleX < kTrueSdfRepairSignSamplesPerAxis; ++sampleX)
					{
						const double fractionX = (sampleX + 0.5)
							/ kTrueSdfRepairSignSamplesPerAxis;
						const int sampleIndex = sampleY
							* kTrueSdfRepairSignSamplesPerAxis + sampleX;
						const bool expectedInside = (cell.signInsideBits
							& (std::uint64_t{ 1 } << sampleIndex)) != 0;
						const double distance = DecodeTrueSdfByte(
							BilinearTrueSdfByte(bytes, width, cellX, cellY,
								fractionX, fractionY), spread);
						if (!std::isfinite(distance))
							return false;
						const bool actualInside = distance > 0.0;
						metrics.falseOutside += expectedInside && !actualInside;
						metrics.falseInside += !expectedInside && actualInside;
					}
				}
			}
			return true;
		}

		bool CountTrueSdfCenterSignErrors(
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
					const bool actualInside = DecodeTrueSdfByte(
						bytes[static_cast<std::size_t>(y) * width + x], spread) > 0.0;
					errors += expectedInside != actualInside;
				}
			}
			return true;
		}

		double EstimateTrueSdfAffectedRowsError(
			const msdfgen::Bitmap<float, 1>& runtimeField,
			const msdfgen::Shape& shape,
			const msdfgen::Projection& projection, msdfgen::FillRule fillRule,
			const std::vector<TrueSdfActiveCell>& activeCells)
		{
			if (runtimeField.width() <= 1 || runtimeField.height() <= 1
				|| activeCells.empty())
			{
				return std::numeric_limits<double>::quiet_NaN();
			}
			const double xFrom = projection.unprojectX(0.5);
			const double xTo = projection.unprojectX(runtimeField.width() - 0.5);
			if (!std::isfinite(xFrom) || !std::isfinite(xTo) || !(xTo > xFrom))
				return std::numeric_limits<double>::quiet_NaN();
			const double overlapFactor = 1.0 / (xTo - xFrom);
			const double subRowSize = 1.0 / kTrueSdfRepairScanlinesPerRow;
			const int cellsWide = runtimeField.width() - 1;
			const msdfgen::BitmapConstSection<float, 1> section = runtimeField;
			double error = 0.0;
			int previousRow = -1;
			for (const TrueSdfActiveCell& cell : activeCells)
			{
				const int row = static_cast<int>(cell.cellIndex / cellsWide);
				if (row == previousRow)
					continue;
				previousRow = row;
				for (int subRow = 0;
					subRow < kTrueSdfRepairScanlinesPerRow; ++subRow)
				{
					const double shapeY = projection.unprojectY(row + 0.5
						+ (subRow + 0.5) * subRowSize);
					if (!std::isfinite(shapeY))
						return std::numeric_limits<double>::quiet_NaN();
					msdfgen::Scanline referenceScanline;
					msdfgen::Scanline fieldScanline;
					shape.scanline(referenceScanline, shapeY);
					msdfgen::scanlineSDF(fieldScanline, section, projection, shapeY,
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
				/ ((runtimeField.height() - 1.0) * kTrueSdfRepairScanlinesPerRow);
			return std::isfinite(normalized)
				? normalized : std::numeric_limits<double>::quiet_NaN();
		}

		bool RepairTrueSdfQuantizationImpl(
			std::vector<std::uint8_t>& bytes,
			msdfgen::Bitmap<float, 1>& continuousField,
			const msdfgen::Shape& shape,
			const msdfgen::Projection& projection,
			msdfgen::FillRule fillRule, std::uint8_t spread,
			std::array<AcceptedTrueSdfByte,
				kMaximumTrueSdfRepairCandidateTrials>& acceptedBytes,
			std::size_t& acceptedCount)
		{
			acceptedCount = 0;
			const int width = continuousField.width();
			const int height = continuousField.height();
			const int edgeCount = shape.edgeCount();
			if (fillRule == msdfgen::FILL_ODD || width < 3 || height < 3
				|| width > kMaximumTrueSdfRepairWidth
				|| edgeCount <= 0 || edgeCount > kMaximumTrueSdfRepairEdges)
			{
				return true;
			}
			const std::uint64_t pixelCount = static_cast<std::uint64_t>(width)
				* static_cast<std::uint64_t>(height);
			if (pixelCount > kMaximumTrueSdfRepairPixels
				|| bytes.size() != pixelCount
				|| static_cast<std::uint64_t>(continuousField.width())
					* continuousField.height() != pixelCount)
			{
				return true;
			}
			const std::uint64_t maximumDistanceQueries =
				kMaximumTrueSdfContinuousCandidates
				+ kMaximumTrueSdfActiveCells
					* kTrueSdfRepairDistanceSamplesPerAxis
					* kTrueSdfRepairDistanceSamplesPerAxis
				+ kMaximumTrueSdfRepairCandidateTrials;
			if (!maximumDistanceQueries
				|| static_cast<std::uint64_t>(edgeCount)
					> kMaximumTrueSdfRepairEdgeTests / maximumDistanceQueries)
			{
				return true;
			}
			const std::uint64_t maximumScanlines = 2u
				* static_cast<std::uint64_t>(height)
				+ static_cast<std::uint64_t>(height - 1)
					* (kTrueSdfRepairDistanceSamplesPerAxis
						+ kTrueSdfRepairSignSamplesPerAxis
						+ 2u * kTrueSdfRepairScanlinesPerRow);
			if (!maximumScanlines
				|| maximumScanlines > kMaximumTrueSdfRepairScanlines
				|| static_cast<std::uint64_t>(edgeCount)
					> kMaximumTrueSdfRepairEdgeTests / maximumScanlines
				|| !TrueSdfSortWorkWithinBudget(maximumScanlines,
					2u * static_cast<std::uint64_t>(height - 1)
						* kTrueSdfRepairScanlinesPerRow,
					width, edgeCount))
				return true;

			for (int y = 0; y < height; ++y)
				for (int x = 0; x < width; ++x)
					if (!std::isfinite(*continuousField(x, y)))
						return true;

			std::vector<TrueSdfRepairCandidate> candidates;
			candidates.reserve(kMaximumTrueSdfContinuousCandidates);
			std::vector<std::uint32_t> activeCellIndices;
			activeCellIndices.reserve(kMaximumTrueSdfActiveCells);
			if (candidates.capacity() > kMaximumTrueSdfContinuousCandidates
				|| activeCellIndices.capacity() > kMaximumTrueSdfActiveCells)
			{
				return true;
			}
			std::size_t continuousCandidateCount = 0;
			std::size_t baselineCenterSignErrors = 0;
			{
				ExactShapeDistanceFinder collectionDistanceFinder(shape);
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
						baselineCenterSignErrors += centerInside
							!= (DecodeTrueSdfByte(bytes[pixelIndex], spread) > 0.0);
						for (int delta : { -1, 1 })
						{
							if (!ContinuousTrueSdfCenterPrefilter(bytes,
								continuousField, width, x, y, delta,
								spread, centerInside))
							{
								continue;
							}
							if (continuousCandidateCount
								>= kMaximumTrueSdfContinuousCandidates)
							{
								return true;
							}
							++continuousCandidateCount;
							bool exactSafe = false;
							if (!CheckExactTrueSdfCenterCandidate(bytes, width, x, y,
								delta, spread, centerInside, projection,
								collectionDistanceFinder, exactSafe))
							{
								return false;
							}
							if (!exactSafe)
								continue;
							TrueSdfRepairCandidate candidate;
							candidate.pixelIndex = static_cast<std::uint32_t>(pixelIndex);
							candidate.delta = static_cast<std::int8_t>(delta);
							candidate.centerInside = static_cast<std::uint8_t>(centerInside);
							candidates.push_back(candidate);
							if (!MarkTrueSdfCandidateCells(activeCellIndices,
								width, height, x, y))
							{
								return true;
							}
						}
					}
				}
			}
			if (candidates.empty())
				return true;

			std::sort(activeCellIndices.begin(), activeCellIndices.end());
			std::size_t activeRowCount = 0;
			int previousRow = -1;
			for (std::uint32_t cellIndex : activeCellIndices)
			{
				const int row = static_cast<int>(cellIndex / (width - 1));
				if (row != previousRow)
				{
					++activeRowCount;
					previousRow = row;
				}
			}
			const std::uint64_t trialBudget = std::min<std::size_t>(
				candidates.size(), kMaximumTrueSdfRepairCandidateTrials);
			const std::uint64_t distanceQueries = continuousCandidateCount
				+ activeCellIndices.size()
					* kTrueSdfRepairDistanceSamplesPerAxis
					* kTrueSdfRepairDistanceSamplesPerAxis
				+ trialBudget;
			const std::uint64_t scanlineQueries = 2u
				* static_cast<std::uint64_t>(height)
				+ activeRowCount
					* (kTrueSdfRepairDistanceSamplesPerAxis
						+ kTrueSdfRepairSignSamplesPerAxis)
				+ 2u * activeRowCount * kTrueSdfRepairScanlinesPerRow;
			const std::uint64_t shapeWidthWork = 2u
				* static_cast<std::uint64_t>(width) * activeRowCount
				* kTrueSdfRepairScanlinesPerRow;
			const std::uint64_t fieldShapeRows = 2u * activeRowCount
				* kTrueSdfRepairScanlinesPerRow;
			if (!distanceQueries || !scanlineQueries
				|| scanlineQueries > kMaximumTrueSdfRepairScanlines
				|| shapeWidthWork > kMaximumTrueSdfShapeWidthWork
				|| !TrueSdfSortWorkWithinBudget(scanlineQueries,
					fieldShapeRows, width, edgeCount)
				|| static_cast<std::uint64_t>(edgeCount)
					> kMaximumTrueSdfRepairEdgeTests / distanceQueries
				|| static_cast<std::uint64_t>(edgeCount)
					> kMaximumTrueSdfRepairEdgeTests / scanlineQueries)
			{
				return true;
			}

			std::vector<TrueSdfActiveCell> activeCells;
			activeCells.reserve(activeCellIndices.size());
			if (activeCells.capacity() > kMaximumTrueSdfActiveCells)
				return true;
			const std::size_t scratchPayloadBytes =
				candidates.capacity() * sizeof(TrueSdfRepairCandidate)
				+ activeCellIndices.capacity() * sizeof(std::uint32_t)
				+ activeCells.capacity() * sizeof(TrueSdfActiveCell)
				+ acceptedBytes.size() * sizeof(AcceptedTrueSdfByte)
				+ kTrueSdfRepairDynamicBudgetBytes;
			if (scratchPayloadBytes > kTrueSdfRepairPerWorkerScratchBudgetBytes)
				return true;
			for (std::uint32_t cellIndex : activeCellIndices)
			{
				TrueSdfActiveCell cell;
				cell.cellIndex = cellIndex;
				activeCells.push_back(cell);
			}
			std::vector<std::uint32_t>().swap(activeCellIndices);
			if (!BuildTrueSdfActiveCellSamples(activeCells, shape, projection,
				fillRule, width, spread))
			{
				return false;
			}

			TrueSdfGridMetrics baselineDistanceMetrics;
			TrueSdfSignMetrics baselineSignMetrics;
			if (!MeasureActiveTrueSdfDistanceGrid(bytes, width, spread,
					activeCells, baselineDistanceMetrics)
				|| !MeasureActiveTrueSdfSignGrid(bytes, width, spread,
					activeCells, baselineSignMetrics))
			{
				return false;
			}
			for (int y = 0; y < height; ++y)
				for (int x = 0; x < width; ++x)
					*continuousField(x, y) = bytes[static_cast<std::size_t>(y)
						* width + x] / 256.0f;
			const double baselineShapeError = EstimateTrueSdfAffectedRowsError(
				continuousField, shape, projection, fillRule, activeCells);
			if (!std::isfinite(baselineShapeError))
				return false;

			for (TrueSdfRepairCandidate& candidate : candidates)
			{
				const int texelX = static_cast<int>(candidate.pixelIndex % width);
				const int texelY = static_cast<int>(candidate.pixelIndex / width);
				double improvement = 0.0;
				if (!EvaluateTrueSdfCandidateOnDistanceGrid(bytes, width, height,
						spread, activeCells, texelX, texelY, candidate.delta,
						improvement)
					|| !EvaluateTrueSdfCandidateOnSignGrid(bytes, width, height,
						spread, activeCells, texelX, texelY, candidate.delta))
				{
					candidate.delta = 0;
					continue;
				}
				candidate.score = improvement;
			}
			candidates.erase(std::remove_if(candidates.begin(), candidates.end(),
				[](const TrueSdfRepairCandidate& candidate)
				{
					return !candidate.delta || !std::isfinite(candidate.score);
				}), candidates.end());
			if (candidates.empty())
				return true;
			std::sort(candidates.begin(), candidates.end(),
				[](const TrueSdfRepairCandidate& left,
					const TrueSdfRepairCandidate& right)
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
				const TrueSdfRepairCandidate candidate = candidates[index];
				if (uniqueCount && candidates[uniqueCount - 1].pixelIndex
					== candidate.pixelIndex)
				{
					continue;
				}
				candidates[uniqueCount++] = candidate;
			}
			candidates.resize(uniqueCount);
			std::sort(candidates.begin(), candidates.end(),
				[](const TrueSdfRepairCandidate& left,
					const TrueSdfRepairCandidate& right)
				{
					if (left.score != right.score)
						return left.score > right.score;
					if (left.pixelIndex != right.pixelIndex)
						return left.pixelIndex < right.pixelIndex;
					return left.delta < right.delta;
				});

			const std::size_t trialCount = std::min(candidates.size(),
				kMaximumTrueSdfRepairCandidateTrials);
			{
				ExactShapeDistanceFinder trialDistanceFinder(shape);
				for (std::size_t trial = 0; trial < trialCount; ++trial)
				{
					const TrueSdfRepairCandidate& candidate = candidates[trial];
					const int texelX = static_cast<int>(candidate.pixelIndex % width);
					const int texelY = static_cast<int>(candidate.pixelIndex / width);
					bool centerSafe = false;
					if (!CheckExactTrueSdfCenterCandidate(bytes, width,
							texelX, texelY, candidate.delta, spread,
							candidate.centerInside != 0, projection,
							trialDistanceFinder, centerSafe))
					{
						return false;
					}
					if (!centerSafe)
						continue;
					double improvement = 0.0;
					if (!EvaluateTrueSdfCandidateOnDistanceGrid(bytes, width,
							height, spread, activeCells, texelX, texelY,
							candidate.delta, improvement)
						|| !EvaluateTrueSdfCandidateOnSignGrid(bytes, width,
							height, spread, activeCells, texelX, texelY,
							candidate.delta))
					{
						continue;
					}
					AcceptedTrueSdfByte& accepted = acceptedBytes[acceptedCount++];
					accepted.pixelIndex = candidate.pixelIndex;
					accepted.oldByte = bytes[candidate.pixelIndex];
					bytes[candidate.pixelIndex] = static_cast<std::uint8_t>(
						static_cast<int>(accepted.oldByte) + candidate.delta);
				}
			}
			if (!acceptedCount)
				return true;

			TrueSdfGridMetrics finalDistanceMetrics;
			TrueSdfSignMetrics finalSignMetrics;
			if (!MeasureActiveTrueSdfDistanceGrid(bytes, width, spread,
					activeCells, finalDistanceMetrics)
				|| !MeasureActiveTrueSdfSignGrid(bytes, width, spread,
					activeCells, finalSignMetrics))
			{
				return false;
			}
			std::size_t finalCenterSignErrors = 0;
			if (!CountTrueSdfCenterSignErrors(bytes, width, height, spread,
				shape, projection, fillRule, finalCenterSignErrors))
			{
				return false;
			}
			for (std::size_t index = 0; index < acceptedCount; ++index)
			{
				const std::uint32_t pixelIndex = acceptedBytes[index].pixelIndex;
				*continuousField(static_cast<int>(pixelIndex % width),
					static_cast<int>(pixelIndex / width)) = bytes[pixelIndex] / 256.0f;
			}
			const double finalShapeError = EstimateTrueSdfAffectedRowsError(
				continuousField, shape, projection, fillRule, activeCells);
			return TrueSdfMetricsDoNotRegress(
					baselineDistanceMetrics, finalDistanceMetrics)
				&& finalSignMetrics.falseOutside <= baselineSignMetrics.falseOutside
				&& finalSignMetrics.falseInside <= baselineSignMetrics.falseInside
				&& finalCenterSignErrors <= baselineCenterSignErrors
				&& std::isfinite(finalShapeError)
				&& finalShapeError <= baselineShapeError
					+ kTrueSdfRepairErrorTolerance;
		}

		void RepairTrueSdfQuantization(
			std::vector<std::uint8_t>& bytes,
			msdfgen::Bitmap<float, 1>& continuousField,
			const msdfgen::Shape& shape,
			const msdfgen::Projection& projection,
			msdfgen::FillRule fillRule, std::uint8_t spread) noexcept
		{
			std::array<AcceptedTrueSdfByte,
				kMaximumTrueSdfRepairCandidateTrials> acceptedBytes = {};
			std::size_t acceptedCount = 0;
			bool keepChanges = false;
			try
			{
				keepChanges = RepairTrueSdfQuantizationImpl(bytes, continuousField,
					shape, projection, fillRule, spread,
					acceptedBytes, acceptedCount);
			}
			catch (...)
			{
				keepChanges = false;
			}
			if (keepChanges)
				return;
			while (acceptedCount)
			{
				const AcceptedTrueSdfByte& accepted = acceptedBytes[--acceptedCount];
				if (accepted.pixelIndex < bytes.size())
					bytes[accepted.pixelIndex] = accepted.oldByte;
			}
		}
}

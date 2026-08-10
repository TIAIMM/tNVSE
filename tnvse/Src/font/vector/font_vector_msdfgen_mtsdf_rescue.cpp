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
		struct MtsdfRescueCandidate
		{
			double score = 0.0;
			std::uint32_t pixelIndex = 0;
		};

		struct AcceptedMtsdfRescuePixel
		{
			std::uint32_t pixelIndex = 0;
			float rgb[3] = {};
			float alpha = 0.0f;
		};

		struct MtsdfRescueCoverageErrors
		{
			double absoluteErrorSum = 0.0;
			double maximumAbsoluteError = 0.0;
			std::size_t sampleCount = 0;
		};

		struct MtsdfRescueExactComparison
		{
			MtsdfRepairErrors before;
			MtsdfRepairErrors after;
			std::array<MtsdfRescueCoverageErrors,
				kMtsdfRescueAntialiasWidths.size()> beforeCoverage = {};
			std::array<MtsdfRescueCoverageErrors,
				kMtsdfRescueAntialiasWidths.size()> afterCoverage = {};
			double maximumErrorIncrease = 0.0;
			double maximumWorsenedAfterError = 0.0;
			std::size_t worsenedSamples = 0;
			bool preservesCorrectSigns = true;
			bool finite = true;
		};

		struct MtsdfRescueCenterComparison
		{
			std::size_t beforeFalseOutside = 0;
			std::size_t beforeFalseInside = 0;
			std::size_t afterFalseOutside = 0;
			std::size_t afterFalseInside = 0;
			bool preservesCorrectSigns = true;
			bool finite = true;
		};

		struct MtsdfRescueWorkBudget
		{
			std::uint64_t distanceEdgeTests = 0;
			std::uint64_t scanlineEdgeTests = 0;
			std::uint64_t fieldScanWork = 0;
			std::uint64_t sortWork = 0;
		};

		constexpr std::size_t kMaximumMtsdfRescueFinderPayloadBytes =
			kMaximumMtsdfRepairEdges
				* sizeof(msdfgen::TrueDistanceSelector::EdgeCache)
			+ kMaximumMtsdfRescueContours
				* (sizeof(msdfgen::TrueDistanceSelector) + sizeof(int));
		constexpr std::size_t kMaximumMtsdfRescueShapeScanlinePayloadBytes =
			3u * kMaximumMtsdfRepairEdges
			* sizeof(msdfgen::Scanline::Intersection);
		constexpr std::size_t kMaximumMtsdfRescueFieldScanlinePayloadBytes =
			(3u * (kMaximumMtsdfRescueFieldWidth - 1u) + 1u)
			* sizeof(msdfgen::Scanline::Intersection);
		constexpr std::size_t kMtsdfRescueFixedPayloadBytes =
			kMaximumMtsdfRepairCandidateTrials
				* (sizeof(MtsdfRescueCandidate)
					+ sizeof(AcceptedMtsdfRescuePixel))
			+ kMaximumMtsdfRescueActiveCells * sizeof(std::uint32_t);
		constexpr std::size_t kMaximumMtsdfRescueRankingScratchBytes =
			kMaximumMtsdfRepairPixels * sizeof(double)
			+ kMaximumMtsdfRescueFinderPayloadBytes
			+ kMaximumMtsdfRescueShapeScanlinePayloadBytes
			+ kMtsdfRescueFixedPayloadBytes
			+ kMtsdfRescueDynamicBudgetMarginBytes;
		constexpr std::size_t kMaximumMtsdfRescueRowScratchBytes =
			kMaximumMtsdfRescueFinderPayloadBytes
			+ kMaximumMtsdfRescueShapeScanlinePayloadBytes
			+ kMaximumMtsdfRescueFieldScanlinePayloadBytes
			+ kMtsdfRescueFixedPayloadBytes
			+ kMtsdfRescueDynamicBudgetMarginBytes;
		static_assert(kMaximumMtsdfRescueRankingScratchBytes
			<= kMtsdfRescuePerWorkerScratchBudgetBytes,
			"MTSDF rescue ranking exceeds its per-worker scratch budget");
		static_assert(kMaximumMtsdfRescueRowScratchBytes
			<= kMtsdfRescuePerWorkerScratchBudgetBytes,
			"MTSDF rescue row audit exceeds its per-worker scratch budget");

		std::uint64_t MtsdfRescueCeilLog2(std::uint64_t value)
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

		bool ConsumeMtsdfRescueProduct(std::uint64_t& consumed,
			std::uint64_t left, std::uint64_t right,
			std::uint64_t maximum)
		{
			if (consumed > maximum || (left && right > (maximum - consumed) / left))
				return false;
			consumed += left * right;
			return true;
		}

		bool ConsumeMtsdfRescueDistanceQueries(MtsdfRescueWorkBudget& budget,
			std::uint64_t queries, int edgeCount)
		{
			return edgeCount > 0 && ConsumeMtsdfRescueProduct(
				budget.distanceEdgeTests, queries,
				static_cast<std::uint64_t>(edgeCount),
				kMaximumMtsdfRepairEdgeTests);
		}

		bool ConsumeMtsdfRescueScanlines(MtsdfRescueWorkBudget& budget,
			std::uint64_t shapeScanlines, std::uint64_t fieldScanlines,
			int width, int edgeCount)
		{
			if (width <= 0 || width > kMaximumMtsdfRescueFieldWidth
				|| edgeCount <= 0 || edgeCount > kMaximumMtsdfRepairEdges)
			{
				return false;
			}
			if (!ConsumeMtsdfRescueProduct(budget.scanlineEdgeTests,
				shapeScanlines, static_cast<std::uint64_t>(edgeCount),
				kMaximumMtsdfRescueScanlineEdgeTests)
				|| !ConsumeMtsdfRescueProduct(budget.fieldScanWork,
					fieldScanlines, static_cast<std::uint64_t>(width),
					kMaximumMtsdfRescueFieldScanWork))
			{
				return false;
			}
			const std::uint64_t maximumShapeIntersections =
				3u * static_cast<std::uint64_t>(edgeCount);
			const std::uint64_t maximumFieldIntersections =
				3u * static_cast<std::uint64_t>(width - 1) + 1u;
			return ConsumeMtsdfRescueProduct(budget.sortWork,
				shapeScanlines, maximumShapeIntersections
					* MtsdfRescueCeilLog2(maximumShapeIntersections),
				kMaximumMtsdfRescueSortWork)
				&& ConsumeMtsdfRescueProduct(budget.sortWork,
					fieldScanlines, maximumFieldIntersections
						* MtsdfRescueCeilLog2(maximumFieldIntersections),
					kMaximumMtsdfRescueSortWork);
		}

		bool MtsdfRescueCandidatePrecedes(
			const MtsdfRescueCandidate& left,
			const MtsdfRescueCandidate& right)
		{
			return left.score != right.score
				? left.score > right.score
				: left.pixelIndex < right.pixelIndex;
		}

		void InsertRankedMtsdfRescueCandidate(
			std::array<MtsdfRescueCandidate,
				kMaximumMtsdfRepairCandidateTrials>& ranked,
			std::size_t& rankedCount,
			const MtsdfRescueCandidate& candidate)
		{
			std::size_t position = 0;
			while (position < rankedCount
				&& MtsdfRescueCandidatePrecedes(ranked[position], candidate))
			{
				++position;
			}
			if (position >= kMaximumMtsdfRepairCandidateTrials)
				return;
			const std::size_t newCount = std::min<std::size_t>(
				rankedCount + 1, kMaximumMtsdfRepairCandidateTrials);
			for (std::size_t index = newCount - 1; index > position; --index)
				ranked[index] = ranked[index - 1];
			ranked[position] = candidate;
			rankedCount = newCount;
		}

		bool BuildMtsdfRescueRanking(
			const msdfgen::Bitmap<float, 4>& field,
			const msdfgen::Shape& shape,
			const msdfgen::Projection& projection,
			msdfgen::FillRule fillRule, std::uint8_t spread,
			std::array<MtsdfRescueCandidate,
				kMaximumMtsdfRepairCandidateTrials>& ranked,
			std::size_t& rankedCount)
		{
			rankedCount = 0;
			const int width = field.width();
			const int height = field.height();
			const std::size_t pixelCount = static_cast<std::size_t>(width)
				* static_cast<std::size_t>(height);
			std::vector<double> scores(pixelCount);
			const msdfgen::BitmapConstSection<float, 4> section = field;
			ExactShapeDistanceFinder distanceFinder(shape);
			for (int y = 0; y < height - 1; ++y)
			{
				for (int sampleY = 0;
					sampleY < kMtsdfRepairDistanceSamplesPerAxis; ++sampleY)
				{
					const double fy = (sampleY + 0.5)
						/ kMtsdfRepairDistanceSamplesPerAxis;
					const double fieldY = y + 0.5 + fy;
					const double shapeY = projection.unprojectY(fieldY);
					if (!std::isfinite(shapeY))
						return false;
					msdfgen::Scanline scanline;
					shape.scanline(scanline, shapeY);
					for (int x = 0; x < width - 1; ++x)
					{
						for (int sampleX = 0;
							sampleX < kMtsdfRepairDistanceSamplesPerAxis; ++sampleX)
						{
							const double fx = (sampleX + 0.5)
								/ kMtsdfRepairDistanceSamplesPerAxis;
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
								(MedianMtsdfRgb(sample[0], sample[1], sample[2])
									- 0.5) * 2.0 * spread,
								-static_cast<double>(spread),
								static_cast<double>(spread));
							const double alphaDistance = std::clamp(
								(sample[3] - 0.5) * 2.0 * spread,
								-static_cast<double>(spread),
								static_cast<double>(spread));
							if (!std::isfinite(rgbDistance)
								|| !std::isfinite(alphaDistance))
							{
								return false;
							}
							if (alphaDistance - rgbDistance
								<= kMtsdfRepairDeficitThreshold
									- kMtsdfRepairAlphaPrefilterMargin)
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
								const float* pixel = field(
									cornerX[corner], cornerY[corner]);
								float equalized[3] = {
									pixel[0], pixel[1], pixel[2]
								};
								if (!EqualizeMtsdfRgbToOwnMedian(equalized))
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
								const float changedMedian = MedianMtsdfRgb(
									changedSample[0], changedSample[1], changedSample[2]);
								if (!std::isfinite(changedMedian))
									return false;
								changedDistances[corner] = std::clamp(
									(changedMedian - 0.5) * 2.0 * spread,
									-static_cast<double>(spread),
									static_cast<double>(spread));
								hasPotentialImprovement = hasPotentialImprovement
									|| changedDistances[corner] > rgbDistance;
							}
							if (!hasPotentialImprovement)
								continue;
							const double rawDistance = distanceFinder.distance(
								projection.unproject(msdfgen::Point2(fieldX, fieldY)));
							if (!std::isfinite(rawDistance))
								return false;
							const double unsignedDistance = std::min<double>(
								spread, std::abs(rawDistance));
							if (unsignedDistance - rgbDistance
								<= kMtsdfRepairDeficitThreshold)
							{
								continue;
							}
							const double beforeError = std::abs(
								unsignedDistance - rgbDistance);
							for (int corner = 0; corner < 4; ++corner)
							{
								const double improvement = beforeError - std::abs(
									unsignedDistance - changedDistances[corner]);
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
					MtsdfRescueCandidate candidate;
					candidate.score = scores[index];
					candidate.pixelIndex = static_cast<std::uint32_t>(index);
					InsertRankedMtsdfRescueCandidate(
						ranked, rankedCount, candidate);
				}
			}
			return true;
		}

		MtsdfRescueExactComparison CompareMtsdfRescueCandidateExact(
			const msdfgen::Bitmap<float, 4>& field,
			const msdfgen::Shape& shape,
			const msdfgen::Projection& projection,
			msdfgen::FillRule fillRule, std::uint8_t spread,
			ExactShapeDistanceFinder& distanceFinder,
			int texelX, int texelY, const float replacement[3],
			int samplesPerAxis)
		{
			MtsdfRescueExactComparison comparison;
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
								const float bottom = msdfgen::mix(
									value(cellX, cellY), value(cellX + 1, cellY), fx);
								const float top = msdfgen::mix(
									value(cellX, cellY + 1),
									value(cellX + 1, cellY + 1), fx);
								afterSample[channel] = msdfgen::mix(bottom, top, fy);
							}
							const float beforeMedian = MedianMtsdfRgb(
								beforeSample[0], beforeSample[1], beforeSample[2]);
							const float afterMedian = MedianMtsdfRgb(
								afterSample[0], afterSample[1], afterSample[2]);
							const double rawDistance = distanceFinder.distance(
								projection.unproject(msdfgen::Point2(fieldX, fieldY)));
							if (!std::isfinite(beforeMedian)
								|| !std::isfinite(afterMedian)
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
							comparison.preservesCorrectSigns =
								comparison.preservesCorrectSigns
								&& (beforeActual != expected || afterActual == expected);
							const double unsignedDistance = std::min<double>(
								spread, std::abs(rawDistance));
							const double exactDistance = expected
								? unsignedDistance : -unsignedDistance;
							const double beforeDistance = std::clamp(
								(beforeMedian - 0.5) * 2.0 * spread,
								-static_cast<double>(spread),
								static_cast<double>(spread));
							const double afterDistance = std::clamp(
								(afterMedian - 0.5) * 2.0 * spread,
								-static_cast<double>(spread),
								static_cast<double>(spread));
							const double beforeError = std::abs(
								exactDistance - beforeDistance);
							const double afterError = std::abs(
								exactDistance - afterDistance);
							if (!std::isfinite(beforeError) || !std::isfinite(afterError))
							{
								comparison.finite = false;
								return comparison;
							}
							comparison.before.severeDistanceErrors +=
								beforeError > kMtsdfRepairDeficitThreshold;
							comparison.after.severeDistanceErrors +=
								afterError > kMtsdfRepairDeficitThreshold;
							comparison.before.absoluteDistanceErrorSum += beforeError;
							comparison.after.absoluteDistanceErrorSum += afterError;
							comparison.before.maximumAbsoluteDistanceError = std::max(
								comparison.before.maximumAbsoluteDistanceError, beforeError);
							comparison.after.maximumAbsoluteDistanceError = std::max(
								comparison.after.maximumAbsoluteDistanceError, afterError);
							++comparison.before.sampleCount;
							++comparison.after.sampleCount;
							const double increase = afterError - beforeError;
							if (increase > kMtsdfRepairErrorTolerance)
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

		bool MtsdfRescueAggregateSafe(
			const MtsdfRepairComparison& comparison)
		{
			return comparison.finite && comparison.preservesCorrectSigns
				&& comparison.after.falseOutside <= comparison.before.falseOutside
				&& comparison.after.falseInside <= comparison.before.falseInside
				&& comparison.after.severeDistanceErrors
					<= comparison.before.severeDistanceErrors
				&& comparison.after.maximumAbsoluteDistanceError
					<= comparison.before.maximumAbsoluteDistanceError
						+ kMtsdfRepairErrorTolerance
				&& comparison.after.absoluteDistanceErrorSum
					< comparison.before.absoluteDistanceErrorSum
						- kMtsdfRepairErrorTolerance;
		}

		bool MtsdfRescueBoundedCandidateSafe(
			const MtsdfRescueExactComparison& comparison,
			std::uint8_t spread)
		{
			// This is half of one BGRA8 decoded-distance step:
			// .5 * (2*spread/255) == spread/255.
			const double limit = static_cast<double>(spread) / 255.0;
			return comparison.finite && comparison.preservesCorrectSigns
				&& comparison.after.falseOutside <= comparison.before.falseOutside
				&& comparison.after.falseInside <= comparison.before.falseInside
				&& comparison.after.severeDistanceErrors
					<= comparison.before.severeDistanceErrors
				&& comparison.after.maximumAbsoluteDistanceError
					<= comparison.before.maximumAbsoluteDistanceError
						+ kMtsdfRepairErrorTolerance
				&& comparison.after.absoluteDistanceErrorSum
					< comparison.before.absoluteDistanceErrorSum
						- kMtsdfRepairErrorTolerance
				&& (!comparison.worsenedSamples
					|| (comparison.maximumErrorIncrease
							<= limit + kMtsdfRepairErrorTolerance
						&& comparison.maximumWorsenedAfterError
							<= limit + kMtsdfRepairErrorTolerance));
		}

		class ScopedMtsdfRgbTrial
		{
		public:
			ScopedMtsdfRgbTrial(float* pixel, const float replacement[3])
				: pixel_(pixel)
			{
				for (int channel = 0; channel < 3; ++channel)
				{
					old_[channel] = pixel_[channel];
					pixel_[channel] = replacement[channel];
				}
			}

			~ScopedMtsdfRgbTrial()
			{
				for (int channel = 0; channel < 3; ++channel)
					pixel_[channel] = old_[channel];
			}

			ScopedMtsdfRgbTrial(const ScopedMtsdfRgbTrial&) = delete;
			ScopedMtsdfRgbTrial& operator=(const ScopedMtsdfRgbTrial&) = delete;

		private:
			float* pixel_ = nullptr;
			float old_[3] = {};
		};

		float BaselineMtsdfRescueChannel(
			const msdfgen::Bitmap<float, 4>& field,
			const std::array<AcceptedMtsdfRescuePixel,
				kMaximumMtsdfRepairCandidateTrials>& acceptedPixels,
			std::size_t acceptedCount, int x, int y, int channel)
		{
			const std::uint32_t pixelIndex = static_cast<std::uint32_t>(
				static_cast<std::size_t>(y) * field.width() + x);
			for (std::size_t index = 0; index < acceptedCount; ++index)
				if (acceptedPixels[index].pixelIndex == pixelIndex)
					return acceptedPixels[index].rgb[channel];
			return field(x, y)[channel];
		}

		bool AddMtsdfRescueActiveCell(
			std::array<std::uint32_t,
				kMaximumMtsdfRescueActiveCells>& activeCells,
			std::size_t& activeCellCount, std::uint32_t cellIndex)
		{
			for (std::size_t index = 0; index < activeCellCount; ++index)
				if (activeCells[index] == cellIndex)
					return true;
			if (activeCellCount >= activeCells.size())
				return false;
			activeCells[activeCellCount++] = cellIndex;
			return true;
		}

		bool BuildMtsdfRescueActiveCells(
			const std::array<AcceptedMtsdfRescuePixel,
				kMaximumMtsdfRepairCandidateTrials>& acceptedPixels,
			std::size_t acceptedCount, int width, int height,
			std::array<std::uint32_t,
				kMaximumMtsdfRescueActiveCells>& activeCells,
			std::size_t& activeCellCount)
		{
			activeCellCount = 0;
			for (std::size_t accepted = 0; accepted < acceptedCount; ++accepted)
			{
				const int x = static_cast<int>(
					acceptedPixels[accepted].pixelIndex % width);
				const int y = static_cast<int>(
					acceptedPixels[accepted].pixelIndex / width);
				for (int cellY = std::max(y - 1, 0);
					cellY <= std::min(y, height - 2); ++cellY)
				{
					for (int cellX = std::max(x - 1, 0);
						cellX <= std::min(x, width - 2); ++cellX)
					{
						if (!AddMtsdfRescueActiveCell(activeCells, activeCellCount,
							static_cast<std::uint32_t>(
								static_cast<std::size_t>(cellY) * (width - 1)
									+ cellX)))
						{
							return false;
						}
					}
				}
			}
			std::sort(activeCells.begin(),
				activeCells.begin() + activeCellCount);
			return true;
		}

		MtsdfRescueExactComparison CompareMtsdfRescueActiveUnion(
			const msdfgen::Bitmap<float, 4>& field,
			const msdfgen::Shape& shape,
			const msdfgen::Projection& projection,
			msdfgen::FillRule fillRule, std::uint8_t spread,
			const std::array<AcceptedMtsdfRescuePixel,
				kMaximumMtsdfRepairCandidateTrials>& acceptedPixels,
			std::size_t acceptedCount,
			const std::array<std::uint32_t,
				kMaximumMtsdfRescueActiveCells>& activeCells,
			std::size_t activeCellCount, int samplesPerAxis)
		{
			MtsdfRescueExactComparison comparison;
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
								BaselineMtsdfRescueChannel(field, acceptedPixels,
									acceptedCount, cellX, cellY, channel),
								BaselineMtsdfRescueChannel(field, acceptedPixels,
									acceptedCount, cellX + 1, cellY, channel), fx);
							const float top = msdfgen::mix(
								BaselineMtsdfRescueChannel(field, acceptedPixels,
									acceptedCount, cellX, cellY + 1, channel),
								BaselineMtsdfRescueChannel(field, acceptedPixels,
									acceptedCount, cellX + 1, cellY + 1, channel), fx);
							beforeSample[channel] = msdfgen::mix(bottom, top, fy);
						}
						const float beforeMedian = MedianMtsdfRgb(
							beforeSample[0], beforeSample[1], beforeSample[2]);
						const float afterMedian = MedianMtsdfRgb(
							afterSample[0], afterSample[1], afterSample[2]);
						const double rawDistance = distanceFinder.distance(
							projection.unproject(msdfgen::Point2(fieldX, fieldY)));
						if (!std::isfinite(beforeMedian)
							|| !std::isfinite(afterMedian)
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
						comparison.preservesCorrectSigns =
							comparison.preservesCorrectSigns
							&& (beforeActual != expected || afterActual == expected);
						const double unsignedDistance = std::min<double>(
							spread, std::abs(rawDistance));
						const double exactDistance = expected
							? unsignedDistance : -unsignedDistance;
						const double beforeDistance = std::clamp(
							(beforeMedian - 0.5) * 2.0 * spread,
							-static_cast<double>(spread),
							static_cast<double>(spread));
						const double afterDistance = std::clamp(
							(afterMedian - 0.5) * 2.0 * spread,
							-static_cast<double>(spread),
							static_cast<double>(spread));
						const double beforeError = std::abs(
							exactDistance - beforeDistance);
						const double afterError = std::abs(
							exactDistance - afterDistance);
						if (!std::isfinite(beforeError) || !std::isfinite(afterError))
						{
							comparison.finite = false;
							return comparison;
						}
						comparison.before.severeDistanceErrors +=
							beforeError > kMtsdfRepairDeficitThreshold;
						comparison.after.severeDistanceErrors +=
							afterError > kMtsdfRepairDeficitThreshold;
						comparison.before.absoluteDistanceErrorSum += beforeError;
						comparison.after.absoluteDistanceErrorSum += afterError;
						comparison.before.maximumAbsoluteDistanceError = std::max(
							comparison.before.maximumAbsoluteDistanceError, beforeError);
						comparison.after.maximumAbsoluteDistanceError = std::max(
							comparison.after.maximumAbsoluteDistanceError, afterError);
						++comparison.before.sampleCount;
						++comparison.after.sampleCount;
						for (std::size_t coverage = 0;
							coverage < kMtsdfRescueAntialiasWidths.size(); ++coverage)
						{
							const double width = kMtsdfRescueAntialiasWidths[coverage];
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
							MtsdfRescueCoverageErrors& before =
								comparison.beforeCoverage[coverage];
							MtsdfRescueCoverageErrors& after =
								comparison.afterCoverage[coverage];
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
				coverage < kMtsdfRescueAntialiasWidths.size(); ++coverage)
			{
				comparison.finite = comparison.finite
					&& std::isfinite(
						comparison.beforeCoverage[coverage].absoluteErrorSum)
					&& std::isfinite(
						comparison.afterCoverage[coverage].absoluteErrorSum)
					&& std::isfinite(
						comparison.beforeCoverage[coverage].maximumAbsoluteError)
					&& std::isfinite(
						comparison.afterCoverage[coverage].maximumAbsoluteError);
			}
			return comparison;
		}

		bool MtsdfRescueActiveUnionDoesNotRegress(
			const MtsdfRescueExactComparison& comparison)
		{
			if (!comparison.finite || !comparison.preservesCorrectSigns
				|| comparison.after.falseOutside > comparison.before.falseOutside
				|| comparison.after.falseInside > comparison.before.falseInside
				|| comparison.after.severeDistanceErrors
					> comparison.before.severeDistanceErrors
				|| comparison.after.maximumAbsoluteDistanceError
					> comparison.before.maximumAbsoluteDistanceError
						+ kMtsdfRepairErrorTolerance
				|| comparison.after.absoluteDistanceErrorSum
					> comparison.before.absoluteDistanceErrorSum
						+ kMtsdfRepairErrorTolerance)
			{
				return false;
			}
			for (std::size_t coverage = 0;
				coverage < kMtsdfRescueAntialiasWidths.size(); ++coverage)
			{
				if (comparison.afterCoverage[coverage].absoluteErrorSum
						> comparison.beforeCoverage[coverage].absoluteErrorSum
							+ kMtsdfRepairErrorTolerance
					|| comparison.afterCoverage[coverage].maximumAbsoluteError
						> comparison.beforeCoverage[coverage].maximumAbsoluteError
							+ kMtsdfRepairErrorTolerance)
				{
					return false;
				}
			}
			return true;
		}

		MtsdfRescueCenterComparison CompareMtsdfRescueCenters(
			const msdfgen::Bitmap<float, 4>& field,
			const msdfgen::Shape& shape,
			const msdfgen::Projection& projection,
			msdfgen::FillRule fillRule,
			const std::array<AcceptedMtsdfRescuePixel,
				kMaximumMtsdfRepairCandidateTrials>& acceptedPixels,
			std::size_t acceptedCount)
		{
			MtsdfRescueCenterComparison comparison;
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
					const float beforeMedian = MedianMtsdfRgb(
						BaselineMtsdfRescueChannel(field, acceptedPixels,
							acceptedCount, x, y, 0),
						BaselineMtsdfRescueChannel(field, acceptedPixels,
							acceptedCount, x, y, 1),
						BaselineMtsdfRescueChannel(field, acceptedPixels,
							acceptedCount, x, y, 2));
					const float* pixel = field(x, y);
					const float afterMedian = MedianMtsdfRgb(
						pixel[0], pixel[1], pixel[2]);
					if (!std::isfinite(beforeMedian)
						|| !std::isfinite(afterMedian) || !std::isfinite(pixel[3]))
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
					comparison.preservesCorrectSigns =
						comparison.preservesCorrectSigns
						&& (beforeActual != expected || afterActual == expected);
				}
			}
			return comparison;
		}

		bool MtsdfRescueCentersDoNotRegress(
			const MtsdfRescueCenterComparison& comparison)
		{
			return comparison.finite && comparison.preservesCorrectSigns
				&& comparison.afterFalseOutside <= comparison.beforeFalseOutside
				&& comparison.afterFalseInside <= comparison.beforeFalseInside;
		}

		bool RescueMtsdfRgbInterpolationDeficitsImpl(
			msdfgen::Bitmap<float, 4>& field,
			const msdfgen::Shape& shape,
			const msdfgen::Projection& projection,
			msdfgen::FillRule fillRule, std::uint8_t spread,
			std::array<AcceptedMtsdfRescuePixel,
				kMaximumMtsdfRepairCandidateTrials>& acceptedPixels,
			std::size_t& acceptedCount)
		{
			acceptedCount = 0;
			const int width = field.width();
			const int height = field.height();
			const int edgeCount = shape.edgeCount();
			if (fillRule == msdfgen::FILL_ODD || width < 3 || height < 3
				|| width > kMaximumMtsdfRescueFieldWidth
				|| edgeCount <= 0 || edgeCount > kMaximumMtsdfRepairEdges
				|| shape.contours.size() > kMaximumMtsdfRescueContours)
			{
				return true;
			}
			const std::uint64_t pixelCount = static_cast<std::uint64_t>(width)
				* static_cast<std::uint64_t>(height);
			const std::uint64_t cellCount = static_cast<std::uint64_t>(width - 1)
				* static_cast<std::uint64_t>(height - 1);
			if (pixelCount > kMaximumMtsdfRepairPixels || !cellCount)
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

			MtsdfRescueWorkBudget workBudget;
			const std::uint64_t rankingDistanceQueries = cellCount
				* kMtsdfRepairDistanceSamplesPerAxis
				* kMtsdfRepairDistanceSamplesPerAxis;
			const std::uint64_t rankingShapeScanlines =
				static_cast<std::uint64_t>(height - 1)
				* kMtsdfRepairDistanceSamplesPerAxis;
			if (!ConsumeMtsdfRescueDistanceQueries(
					workBudget, rankingDistanceQueries, edgeCount)
				|| !ConsumeMtsdfRescueScanlines(workBudget,
					rankingShapeScanlines, 0, width, edgeCount))
			{
				return true;
			}

			std::array<MtsdfRescueCandidate,
				kMaximumMtsdfRepairCandidateTrials> ranked = {};
			std::size_t rankedCount = 0;
			if (!BuildMtsdfRescueRanking(field, shape, projection, fillRule,
				spread, ranked, rankedCount))
			{
				return false;
			}
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
				if (!EqualizeMtsdfRgbToOwnMedian(replacement))
					continue;
				const std::uint64_t affectedColumns =
					static_cast<std::uint64_t>(x > 0)
					+ static_cast<std::uint64_t>(x < width - 1);
				const std::uint64_t affectedRows =
					static_cast<std::uint64_t>(y > 0)
					+ static_cast<std::uint64_t>(y < height - 1);
				const std::uint64_t affectedCells = affectedColumns * affectedRows;
				if (!affectedCells || !affectedRows)
					return false;

				const std::uint64_t strictQueries = affectedCells
					* kMtsdfRepairDistanceSamplesPerAxis
					* kMtsdfRepairDistanceSamplesPerAxis;
				const std::uint64_t strictScanlines = affectedRows
					* kMtsdfRepairDistanceSamplesPerAxis;
				if (!ConsumeMtsdfRescueDistanceQueries(
						workBudget, strictQueries, edgeCount)
					|| !ConsumeMtsdfRescueScanlines(workBudget,
						strictScanlines, 0, width, edgeCount))
				{
					return false;
				}
				const MtsdfRepairComparison strictComparison =
					CompareMtsdfRepairCandidate(field, shape, projection,
						fillRule, spread, distanceFinder, x, y, replacement);
				if (!strictComparison.finite)
					return false;
				if (!MtsdfRescueAggregateSafe(strictComparison))
					continue;

				bool rescueReason = false;
				if (!strictComparison.everySampleSafe)
				{
					if (!ConsumeMtsdfRescueDistanceQueries(
							workBudget, strictQueries, edgeCount)
						|| !ConsumeMtsdfRescueScanlines(workBudget,
							strictScanlines, 0, width, edgeCount))
					{
						return false;
					}
					const MtsdfRescueExactComparison bounded4 =
						CompareMtsdfRescueCandidateExact(field, shape, projection,
							fillRule, spread, distanceFinder, x, y, replacement,
							kMtsdfRepairDistanceSamplesPerAxis);
					if (!bounded4.finite)
						return false;
					if (!MtsdfRescueBoundedCandidateSafe(bounded4, spread))
						continue;
					rescueReason = true;
				}
				else
				{
					const std::uint64_t contourScanlines = 2u * affectedRows
						* kMtsdfRepairScanlinesPerRow;
					if (!ConsumeMtsdfRescueScanlines(workBudget,
							contourScanlines, contourScanlines, width, edgeCount))
					{
						return false;
					}
					const double beforeContourError = EstimateMtsdfAffectedRowsError(
						field, shape, projection, fillRule, y);
					double afterContourError =
						std::numeric_limits<double>::quiet_NaN();
					{
						ScopedMtsdfRgbTrial trial(pixel, replacement);
						afterContourError = EstimateMtsdfAffectedRowsError(
							field, shape, projection, fillRule, y);
					}
					if (!std::isfinite(beforeContourError)
						|| !std::isfinite(afterContourError))
					{
						return false;
					}
					if (afterContourError
						<= beforeContourError + kMtsdfRepairErrorTolerance)
					{
						continue;
					}
					rescueReason = true;
				}
				if (!rescueReason)
					continue;

				const std::uint64_t denseQueries = affectedCells
					* kMtsdfRescueDenseSamplesPerAxis
					* kMtsdfRescueDenseSamplesPerAxis;
				const std::uint64_t denseScanlines = affectedRows
					* kMtsdfRescueDenseSamplesPerAxis;
				if (!ConsumeMtsdfRescueDistanceQueries(
						workBudget, denseQueries, edgeCount)
					|| !ConsumeMtsdfRescueScanlines(workBudget,
							denseScanlines, 0, width, edgeCount))
				{
					return false;
				}
				const MtsdfRescueExactComparison independent8 =
					CompareMtsdfRescueCandidateExact(field, shape, projection,
						fillRule, spread, distanceFinder, x, y, replacement,
						kMtsdfRescueDenseSamplesPerAxis);
				if (!independent8.finite)
					return false;
				if (!MtsdfRescueBoundedCandidateSafe(independent8, spread))
					continue;
				if (acceptedCount >= acceptedPixels.size())
					return false;
				AcceptedMtsdfRescuePixel& accepted =
					acceptedPixels[acceptedCount++];
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
				kMaximumMtsdfRescueActiveCells> activeCells = {};
			std::size_t activeCellCount = 0;
			if (!BuildMtsdfRescueActiveCells(acceptedPixels, acceptedCount,
				width, height, activeCells, activeCellCount)
				|| !activeCellCount)
			{
				return false;
			}
			const std::uint64_t audit8Queries = activeCellCount
				* kMtsdfRescueDenseSamplesPerAxis
				* kMtsdfRescueDenseSamplesPerAxis;
			const std::uint64_t audit16Queries = activeCellCount
				* kMtsdfRescueAuditSamplesPerAxis
				* kMtsdfRescueAuditSamplesPerAxis;
			const std::uint64_t audit8Scanlines = activeCellCount
				* kMtsdfRescueDenseSamplesPerAxis;
			const std::uint64_t audit16Scanlines = activeCellCount
				* kMtsdfRescueAuditSamplesPerAxis;
			if (!ConsumeMtsdfRescueDistanceQueries(
					workBudget, audit8Queries, edgeCount)
				|| !ConsumeMtsdfRescueDistanceQueries(
					workBudget, audit16Queries, edgeCount)
				|| !ConsumeMtsdfRescueScanlines(workBudget,
					audit8Scanlines + audit16Scanlines
						+ static_cast<std::uint64_t>(height),
					0, width, edgeCount))
			{
				return false;
			}
			const MtsdfRescueExactComparison audit8 =
				CompareMtsdfRescueActiveUnion(field, shape, projection,
					fillRule, spread, acceptedPixels, acceptedCount,
					activeCells, activeCellCount,
					kMtsdfRescueDenseSamplesPerAxis);
			if (!MtsdfRescueActiveUnionDoesNotRegress(audit8))
				return false;
			const MtsdfRescueExactComparison audit16 =
				CompareMtsdfRescueActiveUnion(field, shape, projection,
					fillRule, spread, acceptedPixels, acceptedCount,
					activeCells, activeCellCount,
					kMtsdfRescueAuditSamplesPerAxis);
			if (!MtsdfRescueActiveUnionDoesNotRegress(audit16))
				return false;
			const MtsdfRescueCenterComparison centers =
				CompareMtsdfRescueCenters(field, shape, projection,
					fillRule, acceptedPixels, acceptedCount);
			if (!MtsdfRescueCentersDoNotRegress(centers))
				return false;
			for (std::size_t index = 0; index < acceptedCount; ++index)
			{
				const AcceptedMtsdfRescuePixel& accepted = acceptedPixels[index];
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

		void RescueMtsdfRgbInterpolationDeficits(
			msdfgen::Bitmap<float, 4>& field,
			const msdfgen::Shape& shape,
			const msdfgen::Projection& projection,
			msdfgen::FillRule fillRule, std::uint8_t spread) noexcept
		{
			std::array<AcceptedMtsdfRescuePixel,
				kMaximumMtsdfRepairCandidateTrials> acceptedPixels = {};
			std::size_t acceptedCount = 0;
			bool keepChanges = false;
			try
			{
				keepChanges = RescueMtsdfRgbInterpolationDeficitsImpl(
					field, shape, projection, fillRule, spread,
					acceptedPixels, acceptedCount);
			}
			catch (...)
			{
				keepChanges = false;
			}
			if (keepChanges)
				return;
			while (acceptedCount)
			{
				const AcceptedMtsdfRescuePixel& accepted =
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
		}
}

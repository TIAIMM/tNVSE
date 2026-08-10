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
		bool EqualizeMtsdfRgbToOwnMedian(float* rgb)
		{
			if (!rgb)
				return false;
			const float median = MedianMtsdfRgb(rgb[0], rgb[1], rgb[2]);
			if (!std::isfinite(median)
				|| (rgb[0] == median && rgb[1] == median && rgb[2] == median))
			{
				return false;
			}
			rgb[0] = rgb[1] = rgb[2] = median;
			return true;
		}

		std::size_t CountMtsdfCenterSignErrors(
			const msdfgen::Bitmap<float, 4>& field,
			const msdfgen::Shape& shape,
			const msdfgen::Projection& projection,
			msdfgen::FillRule fillRule)
		{
			msdfgen::Bitmap<float, 1> reference(
				field.width(), field.height());
			msdfgen::rasterize(reference, shape, projection, fillRule);
			std::size_t errors = 0;
			for (int y = 0; y < field.height(); ++y)
			{
				for (int x = 0; x < field.width(); ++x)
				{
					const float* pixel = field(x, y);
					const float median = MedianMtsdfRgb(
						pixel[0], pixel[1], pixel[2]);
					if (!std::isfinite(median))
						return std::numeric_limits<std::size_t>::max();
					errors += (median > 0.5f)
						!= (*reference(x, y) > 0.5f);
				}
			}
			return errors;
		}

		MtsdfRepairComparison CompareMtsdfRepairCandidate(
			const msdfgen::Bitmap<float, 4>& field,
			const msdfgen::Shape& shape,
			const msdfgen::Projection& projection,
			msdfgen::FillRule fillRule,
			std::uint8_t spread,
			ExactShapeDistanceFinder& distanceFinder,
			int texelX, int texelY, const float equalized[3])
		{
			MtsdfRepairComparison comparison;
			const msdfgen::BitmapConstSection<float, 4> section = field;
			for (int cellY = std::max(texelY - 1, 0);
				cellY <= std::min(texelY, field.height() - 2); ++cellY)
			{
				for (int sampleY = 0;
					sampleY < kMtsdfRepairDistanceSamplesPerAxis; ++sampleY)
				{
					const double fy = (sampleY + 0.5)
						/ kMtsdfRepairDistanceSamplesPerAxis;
					const double fieldY = cellY + 0.5 + fy;
					msdfgen::Scanline scanline;
					shape.scanline(scanline, projection.unprojectY(fieldY));
					for (int cellX = std::max(texelX - 1, 0);
						cellX <= std::min(texelX, field.width() - 2); ++cellX)
					{
						for (int sampleX = 0;
							sampleX < kMtsdfRepairDistanceSamplesPerAxis; ++sampleX)
						{
							const double fx = (sampleX + 0.5)
								/ kMtsdfRepairDistanceSamplesPerAxis;
							const double fieldX = cellX + 0.5 + fx;
							float beforeSample[4] = {};
							msdfgen::interpolate(beforeSample, section,
								msdfgen::Point2(fieldX, fieldY));
							float afterSample[3] = {};
							for (int channel = 0; channel < 3; ++channel)
							{
								const auto replacement = [&](int x, int y)
								{
									return x == texelX && y == texelY
										? equalized[channel]
										: field(x, y)[channel];
								};
								// Preserve msdfgen's exact nested mix order. Rewriting
								// this as before + weight * delta changes float
								// rounding and can incorrectly admit a candidate.
								const float bottom = msdfgen::mix(
									replacement(cellX, cellY),
									replacement(cellX + 1, cellY), fx);
								const float top = msdfgen::mix(
									replacement(cellX, cellY + 1),
									replacement(cellX + 1, cellY + 1), fx);
								afterSample[channel] = msdfgen::mix(
									bottom, top, fy);
							}

							const float beforeMedian = MedianMtsdfRgb(
								beforeSample[0], beforeSample[1], beforeSample[2]);
							const float afterMedian = MedianMtsdfRgb(
								afterSample[0], afterSample[1], afterSample[2]);
							const double rawDistance = distanceFinder.distance(
								projection.unproject(msdfgen::Point2(fieldX, fieldY)));
							if (!std::isfinite(rawDistance))
							{
								comparison.finite = false;
								comparison.everySampleSafe = false;
								return comparison;
							}
							const double unsignedDistance = std::min<double>(
								spread, std::abs(rawDistance));
							if (!std::isfinite(beforeMedian)
								|| !std::isfinite(afterMedian)
								|| !std::isfinite(unsignedDistance))
							{
								comparison.finite = false;
								comparison.everySampleSafe = false;
								return comparison;
							}
							const bool expected = scanline.filled(
								projection.unprojectX(fieldX), fillRule);
							const bool beforeActual = beforeMedian > 0.5f;
							const bool afterActual = afterMedian > 0.5f;
							comparison.before.falseOutside +=
								expected && !beforeActual;
							comparison.before.falseInside +=
								!expected && beforeActual;
							comparison.after.falseOutside +=
								expected && !afterActual;
							comparison.after.falseInside +=
								!expected && afterActual;
							comparison.preservesCorrectSigns =
								comparison.preservesCorrectSigns
								&& (beforeActual != expected || afterActual == expected);

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
							if (!std::isfinite(beforeError)
								|| !std::isfinite(afterError))
							{
								comparison.finite = false;
								comparison.everySampleSafe = false;
								return comparison;
							}
							comparison.before.severeDistanceErrors +=
								beforeError > kMtsdfRepairDeficitThreshold;
							comparison.after.severeDistanceErrors +=
								afterError > kMtsdfRepairDeficitThreshold;
							comparison.before.absoluteDistanceErrorSum += beforeError;
							comparison.after.absoluteDistanceErrorSum += afterError;
							comparison.before.maximumAbsoluteDistanceError = std::max(
								comparison.before.maximumAbsoluteDistanceError,
								beforeError);
							comparison.after.maximumAbsoluteDistanceError = std::max(
								comparison.after.maximumAbsoluteDistanceError,
								afterError);
							++comparison.before.sampleCount;
							++comparison.after.sampleCount;
							comparison.everySampleSafe =
								comparison.everySampleSafe
								&& afterError <= beforeError
									+ kMtsdfRepairErrorTolerance;
						}
					}
				}
			}
			return comparison;
		}

		MtsdfRepairSignComparison CompareMtsdfRepairCandidateSigns(
			const msdfgen::Bitmap<float, 4>& field,
			const msdfgen::Shape& shape,
			const msdfgen::Projection& projection,
			msdfgen::FillRule fillRule,
			int texelX, int texelY, const float equalized[3])
		{
			MtsdfRepairSignComparison comparison;
			const msdfgen::BitmapConstSection<float, 4> section = field;
			for (int cellY = std::max(texelY - 1, 0);
				cellY <= std::min(texelY, field.height() - 2); ++cellY)
			{
				for (int sampleY = 0;
					sampleY < kMtsdfRepairSignSamplesPerAxis; ++sampleY)
				{
					const double fy = (sampleY + 0.5)
						/ kMtsdfRepairSignSamplesPerAxis;
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
						for (int sampleX = 0;
							sampleX < kMtsdfRepairSignSamplesPerAxis; ++sampleX)
						{
							const double fx = (sampleX + 0.5)
								/ kMtsdfRepairSignSamplesPerAxis;
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
								const auto replacement = [&](int x, int y)
								{
									return x == texelX && y == texelY
										? equalized[channel]
										: field(x, y)[channel];
								};
								// Match msdfgen's nested interpolation order exactly;
								// algebraic delta reconstruction can round differently.
								const float bottom = msdfgen::mix(
									replacement(cellX, cellY),
									replacement(cellX + 1, cellY), fx);
								const float top = msdfgen::mix(
									replacement(cellX, cellY + 1),
									replacement(cellX + 1, cellY + 1), fx);
								afterSample[channel] = msdfgen::mix(
									bottom, top, fy);
							}

							const float beforeMedian = MedianMtsdfRgb(
								beforeSample[0], beforeSample[1], beforeSample[2]);
							const float afterMedian = MedianMtsdfRgb(
								afterSample[0], afterSample[1], afterSample[2]);
							if (!std::isfinite(beforeMedian)
								|| !std::isfinite(afterMedian))
							{
								comparison.finite = false;
								return comparison;
							}
							const bool expected = scanline.filled(shapeX, fillRule);
							const bool beforeActual = beforeMedian > 0.5f;
							const bool afterActual = afterMedian > 0.5f;
							comparison.preservesCorrectSigns =
								comparison.preservesCorrectSigns
								&& (beforeActual != expected || afterActual == expected);
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

		double EstimateMtsdfAffectedRowsError(
			const msdfgen::Bitmap<float, 4>& field,
			const msdfgen::Shape& shape,
			const msdfgen::Projection& projection,
			msdfgen::FillRule fillRule,
			int texelY)
		{
			if (field.width() <= 1 || field.height() <= 1)
				return std::numeric_limits<double>::quiet_NaN();
			const int firstRow = std::max(texelY - 1, 0);
			const int lastRow = std::min(texelY, field.height() - 2);
			const double subRowSize = 1.0 / kMtsdfRepairScanlinesPerRow;
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
				for (int subRow = 0;
					subRow < kMtsdfRepairScanlinesPerRow; ++subRow)
				{
					const double y = projection.unprojectY(row + 0.5
						+ (subRow + 0.5) * subRowSize);
					if (!std::isfinite(y))
						return std::numeric_limits<double>::quiet_NaN();
					shape.scanline(referenceScanline, y);
					msdfgen::scanlineSDF(fieldScanline, section, projection, y,
						shape.getYAxisOrientation());
					const double contribution = 1.0 - overlapFactor
						* msdfgen::Scanline::overlap(
						referenceScanline, fieldScanline,
						xFrom, xTo, fillRule);
					if (!std::isfinite(contribution))
						return std::numeric_limits<double>::quiet_NaN();
					error += contribution;
					if (!std::isfinite(error))
						return std::numeric_limits<double>::quiet_NaN();
				}
			}
			const double normalized = error
				/ ((field.height() - 1.0) * kMtsdfRepairScanlinesPerRow);
			return std::isfinite(normalized)
				? normalized : std::numeric_limits<double>::quiet_NaN();
		}

		void RepairMtsdfRgbInterpolationDeficits(
			msdfgen::Bitmap<float, 4>& field,
			const msdfgen::Shape& shape,
			const msdfgen::Projection& projection,
			msdfgen::FillRule fillRule,
			std::uint8_t spread)
		{
			const int width = field.width();
			const int height = field.height();
			const int edgeCount = shape.edgeCount();
			if (fillRule == msdfgen::FILL_ODD
				|| width < 3 || height < 3 || edgeCount <= 0
				|| edgeCount > kMaximumMtsdfRepairEdges)
			{
				return;
			}
			const std::uint64_t pixelCount = static_cast<std::uint64_t>(width)
				* static_cast<std::uint64_t>(height);
			const std::uint64_t cellCount = static_cast<std::uint64_t>(width - 1)
				* static_cast<std::uint64_t>(height - 1);
			const std::uint64_t scoringQueries = cellCount
				* kMtsdfRepairDistanceSamplesPerAxis
				* kMtsdfRepairDistanceSamplesPerAxis;
			const std::uint64_t candidateQueries =
				kMaximumMtsdfRepairCandidateTrials * 4u
				* kMtsdfRepairDistanceSamplesPerAxis
				* kMtsdfRepairDistanceSamplesPerAxis;
			const std::uint64_t distanceQueries =
				scoringQueries + candidateQueries;
			if (pixelCount > kMaximumMtsdfRepairPixels
				|| !distanceQueries
				|| static_cast<std::uint64_t>(edgeCount)
					> kMaximumMtsdfRepairEdgeTests / distanceQueries)
			{
				return;
			}
			for (int y = 0; y < height; ++y)
			{
				for (int x = 0; x < width; ++x)
				{
					const float* pixel = field(x, y);
					if (!std::isfinite(pixel[0]) || !std::isfinite(pixel[1])
						|| !std::isfinite(pixel[2]) || !std::isfinite(pixel[3]))
					{
						return;
					}
				}
			}

			std::vector<double> scores(static_cast<std::size_t>(pixelCount));
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
					msdfgen::Scanline scanline;
					shape.scanline(scanline, projection.unprojectY(fieldY));
					for (int x = 0; x < width - 1; ++x)
					{
						for (int sampleX = 0;
							sampleX < kMtsdfRepairDistanceSamplesPerAxis; ++sampleX)
						{
							const double fx = (sampleX + 0.5)
								/ kMtsdfRepairDistanceSamplesPerAxis;
							const double fieldX = x + 0.5 + fx;
							float sample[4] = {};
							msdfgen::interpolate(sample, section,
								msdfgen::Point2(fieldX, fieldY));
							const float rgb = MedianMtsdfRgb(
								sample[0], sample[1], sample[2]);
							if (!std::isfinite(sample[0])
								|| !std::isfinite(sample[1])
								|| !std::isfinite(sample[2])
								|| !std::isfinite(sample[3]))
							{
								return;
							}
							if (!scanline.filled(
									projection.unprojectX(fieldX), fillRule))
							{
								continue;
							}
							const double rgbDistance = std::clamp(
								(rgb - 0.5) * 2.0 * spread,
								-static_cast<double>(spread),
								static_cast<double>(spread));
							const double alphaDistance = std::clamp(
								(sample[3] - 0.5) * 2.0 * spread,
								-static_cast<double>(spread),
								static_cast<double>(spread));
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
									changedSample[0], changedSample[1],
									changedSample[2]);
								if (!std::isfinite(changedMedian))
									return;
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
								return;
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
									return;
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

			std::vector<std::pair<double, std::size_t>> ranked;
			ranked.reserve(scores.size());
			for (std::size_t index = 0; index < scores.size(); ++index)
			{
				if (scores[index] > 0.0 && std::isfinite(scores[index]))
					ranked.emplace_back(scores[index], index);
			}
			const std::size_t trials = std::min(
				ranked.size(), kMaximumMtsdfRepairCandidateTrials);
			if (!trials)
				return;
			std::partial_sort(ranked.begin(), ranked.begin() + trials, ranked.end(),
				[](const auto& left, const auto& right)
				{
					return left.first != right.first
						? left.first > right.first : left.second < right.second;
				});

			const double baselineError = msdfgen::estimateSDFError(
				field, shape, projection,
				kMtsdfRepairScanlinesPerRow, fillRule);
			const std::size_t baselineCenters = CountMtsdfCenterSignErrors(
				field, shape, projection, fillRule);
			if (!std::isfinite(baselineError)
				|| baselineCenters == std::numeric_limits<std::size_t>::max())
			{
				return;
			}

			struct AcceptedMtsdfPixel
			{
				std::size_t index = 0;
				float rgb[3] = {};
			};
			std::vector<AcceptedMtsdfPixel> acceptedPixels;
			acceptedPixels.reserve(trials);
			const auto rollbackAccepted = [&]()
			{
				for (auto accepted = acceptedPixels.rbegin();
					accepted != acceptedPixels.rend(); ++accepted)
				{
					float* pixel = field(
						static_cast<int>(accepted->index % width),
						static_cast<int>(accepted->index / width));
					pixel[0] = accepted->rgb[0];
					pixel[1] = accepted->rgb[1];
					pixel[2] = accepted->rgb[2];
				}
				acceptedPixels.clear();
			};
			for (std::size_t rank = 0; rank < trials; ++rank)
			{
				const std::size_t index = ranked[rank].second;
				const int x = static_cast<int>(index % width);
				const int y = static_cast<int>(index / width);
				const float* pixel = field(x, y);
				float equalized[3] = { pixel[0], pixel[1], pixel[2] };
				if (!EqualizeMtsdfRgbToOwnMedian(equalized))
					continue;
				const MtsdfRepairComparison comparison =
					CompareMtsdfRepairCandidate(field, shape, projection,
						fillRule, spread, distanceFinder, x, y, equalized);
				if (!comparison.finite)
				{
					rollbackAccepted();
					return;
				}
				if (!comparison.everySampleSafe || !comparison.preservesCorrectSigns
					|| comparison.after.falseOutside
						> comparison.before.falseOutside
					|| comparison.after.falseInside
						> comparison.before.falseInside
					|| comparison.after.severeDistanceErrors
						> comparison.before.severeDistanceErrors
					|| comparison.after.maximumAbsoluteDistanceError
						> comparison.before.maximumAbsoluteDistanceError
							+ kMtsdfRepairErrorTolerance
					|| comparison.after.absoluteDistanceErrorSum
						>= comparison.before.absoluteDistanceErrorSum
							- kMtsdfRepairErrorTolerance)
				{
					continue;
				}
				const MtsdfRepairSignComparison signComparison =
					CompareMtsdfRepairCandidateSigns(field, shape, projection,
						fillRule, x, y, equalized);
				if (!signComparison.finite)
				{
					rollbackAccepted();
					return;
				}
				if (!signComparison.preservesCorrectSigns
					|| signComparison.afterFalseOutside
						> signComparison.beforeFalseOutside
					|| signComparison.afterFalseInside
						> signComparison.beforeFalseInside)
				{
					continue;
				}

				AcceptedMtsdfPixel accepted;
				accepted.index = index;
				accepted.rgb[0] = pixel[0];
				accepted.rgb[1] = pixel[1];
				accepted.rgb[2] = pixel[2];
				const double beforeContourError = EstimateMtsdfAffectedRowsError(
					field, shape, projection, fillRule, y);
				if (!std::isfinite(beforeContourError))
				{
					rollbackAccepted();
					return;
				}
				float* mutablePixel = field(x, y);
				mutablePixel[0] = equalized[0];
				mutablePixel[1] = equalized[1];
				mutablePixel[2] = equalized[2];
				const double afterContourError = EstimateMtsdfAffectedRowsError(
					field, shape, projection, fillRule, y);
				if (!std::isfinite(afterContourError)
					|| afterContourError > beforeContourError
						+ kMtsdfRepairErrorTolerance)
				{
					mutablePixel[0] = accepted.rgb[0];
					mutablePixel[1] = accepted.rgb[1];
					mutablePixel[2] = accepted.rgb[2];
					if (!std::isfinite(afterContourError))
					{
						rollbackAccepted();
						return;
					}
					continue;
				}
				acceptedPixels.push_back(accepted);
			}

			if (acceptedPixels.empty())
				return;
			const double finalError = msdfgen::estimateSDFError(
				field, shape, projection,
				kMtsdfRepairScanlinesPerRow, fillRule);
			const std::size_t finalCenters = CountMtsdfCenterSignErrors(
				field, shape, projection, fillRule);
			if (std::isfinite(finalError)
				&& finalError <= baselineError + kMtsdfRepairErrorTolerance
				&& finalCenters <= baselineCenters)
			{
				return;
			}
			rollbackAccepted();
		}
}

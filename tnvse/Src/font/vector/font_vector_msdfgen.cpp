#include "font_vector_msdfgen.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <utility>
#include <vector>

#include <msdfgen.h>
#include <ext/import-font.h>
#include <core/bitmap-interpolation.hpp>
#include <core/rasterization.h>
#include <core/sdf-error-estimation.h>
#include <core/ShapeDistanceFinder.h>

#ifndef MSDFGEN_USE_CPP11
#error "true-SDF repair budgets require move-enabled msdfgen Scanline vectors"
#endif

namespace fonthook::vectorfont
{
	namespace implementation::font_vector_msdfgen {}
	using namespace implementation::font_vector_msdfgen;

	namespace implementation::font_vector_msdfgen
	{
		constexpr int kTrueSdfRepairDistanceSamplesPerAxis = 4;
		constexpr int kTrueSdfRepairSignSamplesPerAxis = 8;
		constexpr int kTrueSdfRepairScanlinesPerRow = 32;
		constexpr double kTrueSdfRepairSevereDistanceError = 0.125;
		constexpr double kTrueSdfRepairErrorTolerance = 1e-10;
		constexpr std::array<double, 4> kTrueSdfRepairAntialiasWidths = {
			0.25, 0.5, 0.75, 1.0
		};
		constexpr std::size_t kMaximumTrueSdfRepairPixels = 65536;
		constexpr int kMaximumTrueSdfRepairWidth = 1024;
		constexpr int kMaximumTrueSdfRepairEdges = 512;
		constexpr std::uint64_t kMaximumTrueSdfRepairEdgeTests = 64000000;
		constexpr std::uint64_t kMaximumTrueSdfRepairScanlines = 2000000;
		constexpr std::uint64_t kMaximumTrueSdfRepairSortWork = 64000000;
		constexpr std::size_t kMaximumTrueSdfContinuousCandidates = 1024;
		constexpr std::size_t kMaximumTrueSdfActiveCells = 2048;
		constexpr std::size_t kMaximumTrueSdfRepairCandidateTrials = 256;
		constexpr std::uint64_t kMaximumTrueSdfShapeWidthWork = 8000000;
		constexpr std::size_t kTrueSdfRepairDynamicBudgetBytes = 64u * 1024u;

		constexpr int kMtsdfRepairDistanceSamplesPerAxis = 4;
		constexpr int kMtsdfRepairSignSamplesPerAxis = 8;
		constexpr int kMtsdfRepairScanlinesPerRow = 32;
		constexpr double kMtsdfRepairDeficitThreshold = 0.125;
		constexpr double kMtsdfRepairAlphaPrefilterMargin = 0.5;
		constexpr double kMtsdfRepairErrorTolerance = 1e-12;
		constexpr std::size_t kMaximumMtsdfRepairPixels = 65536;
		constexpr int kMaximumMtsdfRepairEdges = 1024;
		constexpr std::size_t kMaximumMtsdfRescueContours = 1024;
		constexpr std::uint64_t kMaximumMtsdfRepairEdgeTests = 64000000;
		constexpr std::size_t kMaximumMtsdfRepairCandidateTrials = 64;
		constexpr int kMtsdfRescueDenseSamplesPerAxis = 8;
		constexpr int kMtsdfRescueAuditSamplesPerAxis = 16;
		constexpr std::array<double, 4> kMtsdfRescueAntialiasWidths = {
			0.25, 0.5, 0.75, 1.0
		};
		constexpr std::size_t kMaximumMtsdfRescueActiveCells =
			4u * kMaximumMtsdfRepairCandidateTrials;
		constexpr int kMaximumMtsdfRescueFieldWidth = 4096;
		constexpr std::uint64_t kMaximumMtsdfRescueScanlineEdgeTests = 64000000;
		constexpr std::uint64_t kMaximumMtsdfRescueFieldScanWork = 64000000;
		constexpr std::uint64_t kMaximumMtsdfRescueSortWork = 64000000;
		constexpr std::size_t kMtsdfRescueDynamicBudgetMarginBytes =
			128u * 1024u;

		using ExactShapeDistanceFinder = msdfgen::ShapeDistanceFinder<
			msdfgen::OverlappingContourCombiner<
				msdfgen::TrueDistanceSelector>>;
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

		float MedianMtsdfRgb(float red, float green, float blue)
		{
			return std::max(std::min(red, green),
				std::min(std::max(red, green), blue));
		}

		bool IsEncodableMtsdfPixel(const float* rgba)
		{
			if (!rgba || std::isnan(rgba[0]) || std::isnan(rgba[1])
				|| std::isnan(rgba[2]) || !std::isfinite(rgba[3]))
			{
				return false;
			}
			// An unused edge-color channel may be infinite. Saturating that
			// channel is valid when the RGB median and Alpha remain finite.
			return std::isfinite(MedianMtsdfRgb(
				rgba[0], rgba[1], rgba[2]));
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

		bool ResolveFieldBounds(const msdfgen::Shape& shape,
			std::uint8_t spread, std::size_t maximumBytes,
			std::uint32_t bytesPerPixel, int& fieldLeft, int& fieldBottom,
			int& fieldTop, int& fieldWidth, int& fieldHeight)
		{
			const msdfgen::Shape::Bounds bounds = shape.getBounds();
			if (!std::isfinite(bounds.l) || !std::isfinite(bounds.b)
				|| !std::isfinite(bounds.r) || !std::isfinite(bounds.t)
				|| bounds.l > bounds.r || bounds.b > bounds.t
				|| bounds.l < std::numeric_limits<int>::min() + 64.0
				|| bounds.b < std::numeric_limits<int>::min() + 64.0
				|| bounds.r > std::numeric_limits<int>::max() - 64.0
				|| bounds.t > std::numeric_limits<int>::max() - 64.0)
			{
				return false;
			}
			const std::int64_t guard = static_cast<std::int64_t>(spread) + 1;
			const std::int64_t fieldLeft64 =
				static_cast<std::int64_t>(std::floor(bounds.l)) - guard;
			const std::int64_t fieldBottom64 =
				static_cast<std::int64_t>(std::floor(bounds.b)) - guard;
			const std::int64_t fieldRight64 =
				static_cast<std::int64_t>(std::ceil(bounds.r)) + guard;
			const std::int64_t fieldTop64 =
				static_cast<std::int64_t>(std::ceil(bounds.t)) + guard;
			const std::int64_t fieldWidth64 = fieldRight64 - fieldLeft64;
			const std::int64_t fieldHeight64 = fieldTop64 - fieldBottom64;
			if (!bytesPerPixel || maximumBytes < bytesPerPixel
				|| fieldWidth64 <= 0 || fieldHeight64 <= 0
				|| fieldWidth64 > 4096 || fieldHeight64 > 4096
				|| static_cast<std::uint64_t>(fieldWidth64)
					* static_cast<std::uint64_t>(fieldHeight64)
					> maximumBytes / bytesPerPixel)
			{
				return false;
			}
			fieldLeft = static_cast<int>(fieldLeft64);
			fieldBottom = static_cast<int>(fieldBottom64);
			fieldTop = static_cast<int>(fieldTop64);
			fieldWidth = static_cast<int>(fieldWidth64);
			fieldHeight = static_cast<int>(fieldHeight64);
			return true;
		}
	}

	bool GenerateMsdfgenMtsdf(
		FT_Outline& outline,
		std::uint8_t spread,
		MsdfgenMtsdfBitmap& output,
		std::size_t maximumBytes)
	{
		output = {};
		if (spread < kDistanceFieldMinimumSpread
			|| spread > kDistanceFieldMaximumSpread
			|| maximumBytes < 4)
			return false;
		if (!outline.n_points || !outline.n_contours)
			return true;

		msdfgen::Shape shape;
		if (!PrepareShape(outline, shape))
			return false;
		// Follow msdfgen's documented library sequence. A fixed seed makes disk
		// caches and atlas snapshots reproducible.
		msdfgen::edgeColoringSimple(
			shape, kMtsdfCornerAngleThreshold, 0);

		int fieldLeft = 0;
		int fieldBottom = 0;
		int fieldTop = 0;
		int fieldWidth = 0;
		int fieldHeight = 0;
		if (!ResolveFieldBounds(shape, spread, maximumBytes, 4u,
			fieldLeft, fieldBottom, fieldTop, fieldWidth, fieldHeight))
			return false;

		msdfgen::Bitmap<float, 4> field(fieldWidth, fieldHeight);
		const msdfgen::Projection projection(
			msdfgen::Vector2(1.0),
			msdfgen::Vector2(-fieldLeft, -fieldBottom));
		const msdfgen::SDFTransformation transformation(
			projection, msdfgen::Range(
				-static_cast<double>(spread),
				static_cast<double>(spread)));
		const msdfgen::FillRule fillRule =
			outline.flags & FT_OUTLINE_EVEN_ODD_FILL
			? msdfgen::FILL_ODD : msdfgen::FILL_NONZERO;

		// The scanline pass preserves FreeType's fill rule, including even-odd
		// holes. As in msdfgen's standalone pipeline, generate without correction,
		// correct signs, then run the compatible edge-priority pass.
		msdfgen::MSDFGeneratorConfig generationConfig;
		generationConfig.overlapSupport = true;
		generationConfig.errorCorrection.mode =
			msdfgen::ErrorCorrectionConfig::DISABLED;
		msdfgen::generateMTSDF(
			field, shape, transformation, generationConfig);
		msdfgen::distanceSignCorrection(
			field, shape, projection, 0.5f, fillRule);
		msdfgen::MSDFGeneratorConfig correctionConfig;
		correctionConfig.overlapSupport = true;
		correctionConfig.errorCorrection.mode =
			msdfgen::ErrorCorrectionConfig::EDGE_PRIORITY;
		correctionConfig.errorCorrection.distanceCheckMode =
			msdfgen::ErrorCorrectionConfig::DO_NOT_CHECK_DISTANCE;
		msdfgen::msdfErrorCorrection(
			field, shape, transformation, correctionConfig);
		// The renderer samples BGRA8, so all validation and later shader tests
		// must see the same quantized field.
		msdfgen::simulate8bit(field);
		// Quantization plus bilinear RGB-median reconstruction can depress a
		// narrow interior connection even when every texel-center sign is right.
		// Repair only candidates that improve exact-shape distance at every
		// affected sample and preserve the scanline contour. Alpha is never
		// touched, so effects retain the independently generated true SDF.
		RepairMtsdfRgbInterpolationDeficits(
			field, shape, projection, fillRule, spread);
		// Revision 7 appends a strictly separate rescue phase. The revision-6
		// result above is its rollback baseline: only rescue-owned RGB writes are
		// recorded, and every failure restores those original values. Alpha
		// remains the independently generated true SDF.
		RescueMtsdfRgbInterpolationDeficits(
			field, shape, projection, fillRule, spread);

		for (int y = 0; y < fieldHeight; ++y)
		{
			for (int x = 0; x < fieldWidth; ++x)
			{
				if (!IsEncodableMtsdfPixel(field(x, y)))
					return false;
			}
		}
		output.bgra.resize(static_cast<size_t>(fieldWidth)
			* fieldHeight * 4u);
		output.width = fieldWidth;
		output.height = fieldHeight;
		output.left = fieldLeft;
		output.top = fieldTop;
		for (int y = 0; y < fieldHeight; ++y)
		{
			const int sourceY = fieldHeight - 1 - y;
			for (int x = 0; x < fieldWidth; ++x)
			{
				const float* source = field(x, sourceY);
				std::uint8_t* destination = output.bgra.data()
					+ (static_cast<size_t>(y) * fieldWidth + x) * 4u;
				// D3DFMT_A8R8G8B8 is BGRA in little-endian memory.
				destination[0] = msdfgen::pixelFloatToByte(source[2]);
				destination[1] = msdfgen::pixelFloatToByte(source[1]);
				destination[2] = msdfgen::pixelFloatToByte(source[0]);
				destination[3] = msdfgen::pixelFloatToByte(source[3]);
			}
		}
		return true;
	}

	bool GenerateMsdfgenTrueSdf(
		FT_Outline& outline,
		std::uint8_t spread,
		MsdfgenSdfBitmap& output,
		std::size_t maximumBytes)
	{
		output = {};
		if (spread < kDistanceFieldMinimumSpread
			|| spread > kDistanceFieldMaximumSpread
			|| maximumBytes < 1)
			return false;
		if (!outline.n_points || !outline.n_contours)
			return true;

		msdfgen::Shape shape;
		if (!PrepareShape(outline, shape))
			return false;
		int fieldLeft = 0;
		int fieldBottom = 0;
		int fieldTop = 0;
		int fieldWidth = 0;
		int fieldHeight = 0;
		if (!ResolveFieldBounds(shape, spread, maximumBytes, 1u,
			fieldLeft, fieldBottom, fieldTop, fieldWidth, fieldHeight))
		{
			return false;
		}

		msdfgen::Bitmap<float, 1> field(fieldWidth, fieldHeight);
		const msdfgen::Projection projection(
			msdfgen::Vector2(1.0),
			msdfgen::Vector2(-fieldLeft, -fieldBottom));
		const msdfgen::SDFTransformation transformation(
			projection, msdfgen::Range(
				-static_cast<double>(spread),
				static_cast<double>(spread)));
		const msdfgen::FillRule fillRule =
			outline.flags & FT_OUTLINE_EVEN_ODD_FILL
			? msdfgen::FILL_ODD : msdfgen::FILL_NONZERO;
		msdfgen::generateSDF(
			field, shape, transformation, msdfgen::GeneratorConfig(true));
		msdfgen::distanceSignCorrection(
			field, shape, projection, 0.5f, fillRule);
		for (int y = 0; y < fieldHeight; ++y)
			for (int x = 0; x < fieldWidth; ++x)
				if (!std::isfinite(*field(x, y)))
					return false;

		// Keep the single output allocation in msdfgen's bottom-up coordinate
		// system until repair is complete, then invert rows in place.
		output.pixels.resize(static_cast<std::size_t>(fieldWidth) * fieldHeight);
		output.width = fieldWidth;
		output.height = fieldHeight;
		output.left = fieldLeft;
		output.top = fieldTop;
		for (int y = 0; y < fieldHeight; ++y)
		{
			for (int x = 0; x < fieldWidth; ++x)
			{
				const float encoded = *field(x, y);
				// The true-SDF shader decodes byte N as
				// (N / 128 - 1) * spread, with byte 128 as exact zero.
				// Its nearest-integer inverse is round(256 * encoded), not
				// the conventional normalized-UNORM round(255 * encoded).
				// Quantize to that deployed contract here; MTSDF remains on
				// the conventional 255 scale used by its 0.5-centered decoder.
				const long runtimeByte = std::lround(
					std::clamp(static_cast<double>(encoded), 0.0, 1.0)
					* 256.0);
				output.pixels[static_cast<std::size_t>(y) * fieldWidth + x] =
					static_cast<std::uint8_t>(std::clamp(runtimeByte, 0L, 255L));
			}
		}
		// Byte-128-centered quantization is not uniformly optimal under the
		// shader's bilinear decoder.  Before the top-down output flip, consider
		// only +/-1 byte corrections that pass continuous-center prefiltering,
		// exact-center confirmation, exact 4x distance/coverage checks, an
		// independent 8x sign check, and the final contour-error rollback gate.
		RepairTrueSdfQuantization(output.pixels, field, shape, projection,
			fillRule, spread);

		for (int y = 0; y < fieldHeight / 2; ++y)
		{
			std::swap_ranges(output.pixels.begin()
					+ static_cast<std::size_t>(y) * fieldWidth,
				output.pixels.begin()
					+ static_cast<std::size_t>(y + 1) * fieldWidth,
				output.pixels.begin()
					+ static_cast<std::size_t>(fieldHeight - 1 - y) * fieldWidth);
		}
		return true;
	}
}

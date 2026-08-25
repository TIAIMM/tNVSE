#include "font_atlas_internal.h"

#include "encoding.h"
#include "globals.h"
#include "load_config.h"
#include "native_tile_overlay.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <vector>

#include "font_atlas_direct_internal.h"

namespace fonthook::vectorfont
{
	namespace implementation::font_atlas_direct {}
	using namespace implementation::font_atlas_direct;

	namespace implementation::font_atlas_direct
	{
		std::atomic<UInt64> s_directProfileFailureTotal{ 0 };
		std::atomic<UInt64> s_directProfileFailureSuppressedTotal{ 0 };
		std::atomic<UInt64> s_directProfileRecoveryTotal{ 0 };
		std::atomic<ULONGLONG> s_directProfileLastFailureAt{ 0 };
		std::atomic<ULONGLONG> s_directProfileLastRecoveryAt{ 0 };
		std::atomic<UInt64> s_directProfileActiveFailureSignature{ 0 };
		std::atomic<UInt32> s_directProfileLastFontId{ 0 };
		std::atomic<UInt32> s_directProfileLastStatus{
			static_cast<UInt32>(DirectProfileAcquireStatus::NotAttempted)
		};

		void MixDirectProfileFailureSignature(UInt64& hash, UInt64 value) noexcept
		{
			hash ^= value;
			hash *= 1099511628211ull;
		}

		UInt64 MakeDirectProfileFailureSignature(
			UInt32 fontId, const DirectProfileAcquireResult& result) noexcept
		{
			UInt64 hash = 1469598103934665603ull;
			MixDirectProfileFailureSignature(hash, fontId);
			MixDirectProfileFailureSignature(hash,
				static_cast<UInt32>(result.status));
			MixDirectProfileFailureSignature(hash, result.failureMask);
			MixDirectProfileFailureSignature(hash, result.slotPresent ? 1u : 0u);
			// Epoch and layout identities are printed with each retained event, but
			// are deliberately excluded from the coalescing key. Their numeric values
			// can change at every rebuild even when the failure class is unchanged.
			MixDirectProfileFailureSignature(hash, result.codePageExpected);
			MixDirectProfileFailureSignature(hash, result.codePageActual);
			MixDirectProfileFailureSignature(hash,
				result.rasterScaleMilliExpected);
			MixDirectProfileFailureSignature(hash,
				result.rasterScaleMilliActual);
			return hash ? hash : 1u;
		}

	}

	DirectProfileDiagnosticSnapshot
		GetDirectProfileDiagnosticSnapshot() noexcept
	{
		DirectProfileDiagnosticSnapshot snapshot;
		snapshot.totalFailures = s_directProfileFailureTotal.load(
			std::memory_order_relaxed);
		snapshot.totalSuppressed = s_directProfileFailureSuppressedTotal.load(
			std::memory_order_relaxed);
		snapshot.totalRecoveries = s_directProfileRecoveryTotal.load(
			std::memory_order_relaxed);
		snapshot.lastFailureAt = s_directProfileLastFailureAt.load(
			std::memory_order_acquire);
		snapshot.lastRecoveryAt = s_directProfileLastRecoveryAt.load(
			std::memory_order_acquire);
		snapshot.activeFailureSignature =
			s_directProfileActiveFailureSignature.load(
				std::memory_order_acquire);
		snapshot.lastFontId = s_directProfileLastFontId.load(
			std::memory_order_acquire);
		snapshot.lastStatus = static_cast<DirectProfileAcquireStatus>(
			s_directProfileLastStatus.load(std::memory_order_acquire));
		return snapshot;
	}

	const char* DirectProfileInvalidationReasonName(
		DirectProfileInvalidationReason reason) noexcept
	{
		switch (reason)
		{
		case DirectProfileInvalidationReason::AcquireEpochMismatch:
			return "acquire-epoch-mismatch";
		case DirectProfileInvalidationReason::AcquireLayoutIdentityMismatch:
			return "acquire-layout-identity-mismatch";
		case DirectProfileInvalidationReason::AcquireCodePageMismatch:
			return "acquire-codepage-mismatch";
		case DirectProfileInvalidationReason::AcquireProfileInvalid:
			return "acquire-profile-invalid";
		case DirectProfileInvalidationReason::DecodeProfileValidationFailed:
			return "decode-profile-validation-failed";
		case DirectProfileInvalidationReason::DecodeGlyphInvalid:
			return "decode-glyph-invalid";
		case DirectProfileInvalidationReason::ProfilePublicationFailed:
			return "profile-publication-failed";
		case DirectProfileInvalidationReason::SealedBatchProfileContractMismatch:
			return "sealed-batch-profile-contract-mismatch";
		case DirectProfileInvalidationReason::SealedBatchCommandInvalid:
			return "sealed-batch-command-invalid";
		case DirectProfileInvalidationReason::SealedBatchSlotMismatch:
			return "sealed-batch-slot-mismatch";
		case DirectProfileInvalidationReason::SealedBatchMaskIncompatible:
			return "sealed-batch-mask-incompatible";
		case DirectProfileInvalidationReason::SealedBatchTextureIndexInvalid:
			return "sealed-batch-texture-index-invalid";
		case DirectProfileInvalidationReason::SealedBatchGlyphRecordMismatch:
			return "sealed-batch-glyph-record-mismatch";
		case DirectProfileInvalidationReason::SealedBatchLayerInvalid:
			return "sealed-batch-layer-invalid";
		case DirectProfileInvalidationReason::SealedBatchPageOrdinalInvalid:
			return "sealed-batch-page-ordinal-invalid";
		case DirectProfileInvalidationReason::SealedBatchSnapshotPlacementInvalid:
			return "sealed-batch-snapshot-placement-invalid";
		case DirectProfileInvalidationReason::SealedBatchPlacementMaskMismatch:
			return "sealed-batch-placement-mask-mismatch";
		case DirectProfileInvalidationReason::DirectBatchProfileContractMismatch:
			return "direct-batch-profile-contract-mismatch";
		case DirectProfileInvalidationReason::DirectBatchTableMissing:
			return "direct-batch-table-missing";
		case DirectProfileInvalidationReason::DirectBatchSlotOutOfRange:
			return "direct-batch-slot-out-of-range";
		case DirectProfileInvalidationReason::DirectBatchMaskIncompatible:
			return "direct-batch-mask-incompatible";
		case DirectProfileInvalidationReason::DirectBatchTextureIndexInvalid:
			return "direct-batch-texture-index-invalid";
		case DirectProfileInvalidationReason::DirectBatchGlyphRecordMismatch:
			return "direct-batch-glyph-record-mismatch";
		case DirectProfileInvalidationReason::DirectBatchLayerInvalid:
			return "direct-batch-layer-invalid";
		case DirectProfileInvalidationReason::DirectBatchPageOrdinalInvalid:
			return "direct-batch-page-ordinal-invalid";
		case DirectProfileInvalidationReason::DirectBatchSnapshotPlacementInvalid:
			return "direct-batch-snapshot-placement-invalid";
		case DirectProfileInvalidationReason::DirectBatchPlacementMaskMismatch:
			return "direct-batch-placement-mask-mismatch";
		case DirectProfileInvalidationReason::VectorBuilderGlyphMetricsUnavailable:
			return "vector-builder-glyph-metrics-unavailable";
		case DirectProfileInvalidationReason::VectorBuilderLookupInvalid:
			return "vector-builder-lookup-invalid";
		case DirectProfileInvalidationReason::VectorBuilderPreparedBatchInvalid:
			return "vector-builder-prepared-batch-invalid";
		case DirectProfileInvalidationReason::AtlasShapeBatchUnavailable:
			return "atlas-shape-batch-unavailable";
		case DirectProfileInvalidationReason::AtlasShapeBuildFailed:
			return "atlas-shape-build-failed";
		case DirectProfileInvalidationReason::AtlasRenderUnsupportedProfile:
			return "atlas-render-unsupported-profile";
		case DirectProfileInvalidationReason::AtlasRenderShapeFailed:
			return "atlas-render-shape-failed";
		case DirectProfileInvalidationReason::LayoutHyphenInvalid:
			return "layout-hyphen-invalid";
		case DirectProfileInvalidationReason::LayoutGlyphInvalid:
			return "layout-glyph-invalid";
		case DirectProfileInvalidationReason::LayoutSpaceInvalid:
			return "layout-space-invalid";
		default:
			return "unknown";
		}
	}

	static UInt8 GetDirectProfileValidationFailureMask(
		const std::shared_ptr<const SealedDirectFontProfile>& profile,
		UInt32 epochExpected, UInt64 layoutExpected,
		UInt32 codePageExpected)
	{
		if (!profile)
			return 0;
		UInt8 mask = 0;
		if (profile->validityEpoch != epochExpected)
			mask |= kDirectProfileFailureEpoch;
		if (profile->layoutIdentity != layoutExpected)
			mask |= kDirectProfileFailureLayoutIdentity;
		if (profile->codePage != codePageExpected)
			mask |= kDirectProfileFailureCodePage;
		if (!IsSealedDirectProfileValid(*profile))
			mask |= kDirectProfileFailureProfileInvalid;
		return mask;
	}

	static void PopulateDirectProfileAcquireSnapshot(
		RuntimeFont& runtime,
		const std::shared_ptr<const SealedDirectFontProfile>& profile,
		DirectProfileAcquireResult& result)
	{
		AtlasState& state = State();
		result.slotPresent = profile != nullptr;
		result.epochExpected = state.directProfileEpoch.load(
			std::memory_order_acquire);
		result.layoutIdentityExpected =
			GetRuntimeDirectLayoutIdentity(runtime);
		result.codePageExpected = GetFreeTypeTextCodePage();
		if (!profile)
			return;
		result.epochActual = profile->validityEpoch;
		result.layoutIdentityActual = profile->layoutIdentity;
		result.codePageActual = profile->codePage;
		result.rasterScaleMilliActual = profile->scaleMilli;
	}

	static void LogDirectProfileAcquireFailure(RuntimeFont& runtime,
		const DirectProfileAcquireResult& result)
	{
		if (!g_bEnableFreeTypeFontRenderingLog)
			return;
		const FreeTypeLongTextTrace* trace = GetActiveFreeTypeLongTextTrace();
		// An empty slot is the normal state before this runtime publishes its first
		// sealed profile. It used to emit once per layout probe during startup,
		// even though there was no trace to diagnose. Keep it observable for an
		// active long-text trace and after a previously published slot disappears,
		// while retaining unconditional diagnostics for real mismatches.
		if (result.status == DirectProfileAcquireStatus::MissingRuntimeSlot
			&& !trace
			&& !HasRuntimePublishedSealedDirectProfile(runtime))
		{
			return;
		}
		const UInt32 fontId = GetRuntimeConfig(runtime).fontId;
		const UInt64 signature = MakeDirectProfileFailureSignature(fontId, result);
		const ULONGLONG now = GetTickCount64();
		const DirectProfileFailureLogDecision decision =
			RecordRuntimeDirectProfileFailure(
				runtime, signature, result.status, now);
		s_directProfileFailureTotal.fetch_add(1u, std::memory_order_relaxed);
		s_directProfileLastFailureAt.store(now, std::memory_order_release);
		if (!decision.shouldLog)
		{
			s_directProfileFailureSuppressedTotal.fetch_add(
				1u, std::memory_order_relaxed);
			return;
		}
		s_directProfileActiveFailureSignature.store(
			signature, std::memory_order_release);
		s_directProfileLastFontId.store(fontId, std::memory_order_release);
		s_directProfileLastStatus.store(
			static_cast<UInt32>(result.status), std::memory_order_release);
		gLog.FormattedMessage(
			"tnvse_freetype_font: sealed direct profile acquire failure state font=%u transition=%llu traceId=%u status=%s signature=%016llX stateChanged=%u occurrences=%llu suppressedSinceLog=%llu previousOccurrences=%llu previousSuppressed=%llu mismatchMask=0x%02X slotPresent=%u epochExpected=%u epochActual=%u layoutExpected=%016llX layoutActual=%016llX codePageExpected=%u codePageActual=%u scaleMilliExpected=%u scaleMilliActual=%u thread=%u policy=state-change-and-milestones",
			fontId,
			static_cast<unsigned long long>(
				GetNativeLoadingTransitionTraceId()),
			trace ? trace->traceId : 0,
			DirectProfileAcquireStatusName(result.status),
			static_cast<unsigned long long>(signature),
			decision.stateChanged ? 1u : 0u,
			static_cast<unsigned long long>(decision.occurrences),
			static_cast<unsigned long long>(decision.suppressedSinceLog),
			static_cast<unsigned long long>(decision.previousOccurrences),
			static_cast<unsigned long long>(decision.previousSuppressed),
			static_cast<UInt32>(result.failureMask),
			result.slotPresent ? 1u : 0u,
			result.epochExpected, result.epochActual,
			static_cast<unsigned long long>(result.layoutIdentityExpected),
			static_cast<unsigned long long>(result.layoutIdentityActual),
			result.codePageExpected, result.codePageActual,
			result.rasterScaleMilliExpected,
			result.rasterScaleMilliActual, GetCurrentThreadId());
	}

	static void LogDirectProfileAcquireRecovery(RuntimeFont& runtime,
		const std::shared_ptr<const SealedDirectFontProfile>& profile,
		bool allowRasterScaleMismatch)
	{
		if (!g_bEnableFreeTypeFontRenderingLog)
			return;
		const DirectProfileFailureRecovery recovery =
			ConsumeRuntimeDirectProfileFailureRecovery(
				runtime, allowRasterScaleMismatch);
		if (!recovery.recovered)
			return;
		const ULONGLONG now = GetTickCount64();
		s_directProfileRecoveryTotal.fetch_add(1u, std::memory_order_relaxed);
		s_directProfileLastRecoveryAt.store(now, std::memory_order_release);
		UInt64 expected = recovery.signature;
		const bool clearedActiveFailure =
			s_directProfileActiveFailureSignature.compare_exchange_strong(
			expected, 0u, std::memory_order_acq_rel,
			std::memory_order_acquire);
		if (clearedActiveFailure)
		{
			s_directProfileLastFontId.store(
				GetRuntimeConfig(runtime).fontId, std::memory_order_release);
			s_directProfileLastStatus.store(
				static_cast<UInt32>(DirectProfileAcquireStatus::Acquired),
				std::memory_order_release);
		}
		gLog.FormattedMessage(
			"tnvse_freetype_font: sealed direct profile acquire recovered font=%u transition=%llu signature=%016llX occurrences=%llu suppressed=%llu failureAgeMs=%llu profileIdentity=%016llX epoch=%u thread=%u",
			GetRuntimeConfig(runtime).fontId,
			static_cast<unsigned long long>(
				GetNativeLoadingTransitionTraceId()),
			static_cast<unsigned long long>(recovery.signature),
			static_cast<unsigned long long>(recovery.occurrences),
			static_cast<unsigned long long>(recovery.suppressed),
			static_cast<unsigned long long>(recovery.startedAt
				&& now >= recovery.startedAt ? now - recovery.startedAt : 0),
			static_cast<unsigned long long>(profile ? profile->identity : 0),
			profile ? profile->validityEpoch : 0,
			GetCurrentThreadId());
	}

	static void LogDirectProfileInvalidation(RuntimeFont& runtime,
		const std::shared_ptr<const SealedDirectFontProfile>& candidate,
		const std::shared_ptr<const SealedDirectFontProfile>& slotBefore,
		const DirectProfileInvalidationContext& context,
		bool conditional, bool cleared)
	{
		if (!g_bEnableFreeTypeFontRenderingLog)
			return;
		AtlasState& state = State();
		const UInt32 epochExpected = state.directProfileEpoch.load(
			std::memory_order_acquire);
		const UInt64 layoutExpected =
			GetRuntimeDirectLayoutIdentity(runtime);
		const UInt32 codePageExpected = GetFreeTypeTextCodePage();
		const UInt8 mismatchMask = context.validationFailureMask
			| GetDirectProfileValidationFailureMask(candidate,
				epochExpected, layoutExpected, codePageExpected);
		const FreeTypeLongTextTrace* trace = GetActiveFreeTypeLongTextTrace();
		const bool candidateCurrent = candidate
			&& slotBefore.get() == candidate.get();
		const char* encodedRole = !context.hasEncodedGlyph ? "none"
			: context.byteClass
				== static_cast<UInt8>(VectorFontByteClass::DoubleByte)
					? "doubleByte" : "singleByte";
		gLog.FormattedMessage(
			"tnvse_freetype_font: sealed direct profile invalidated font=%u transition=%llu traceId=%u api=%s reason=%s failureStage=%s failureStageCode=%u encodedPresent=%u encoded=0x%04X encodedBytes=%u encodedRole=%s mismatchMask=0x%02X slotPresent=%u candidateCurrent=%u cleared=%u slotIdentity=%016llX profileIdentity=%016llX epochExpected=%u epochActual=%u layoutExpected=%016llX layoutActual=%016llX codePageExpected=%u codePageActual=%u profilesAvailable=%u profileValid=%u thread=%u",
			GetRuntimeConfig(runtime).fontId,
			static_cast<unsigned long long>(
				GetNativeLoadingTransitionTraceId()),
			trace ? trace->traceId : 0,
			conditional ? "if-current" : "force",
			DirectProfileInvalidationReasonName(context.reason),
			DirectShapeFailureStageName(context.shapeFailureStage),
			static_cast<UInt32>(context.shapeFailureStage),
			context.hasEncodedGlyph ? 1u : 0u, context.encodedCode,
			static_cast<UInt32>(context.byteLength), encodedRole,
			static_cast<UInt32>(mismatchMask), slotBefore ? 1u : 0u,
			candidateCurrent ? 1u : 0u, cleared ? 1u : 0u,
			static_cast<unsigned long long>(
				slotBefore ? slotBefore->identity : 0),
			static_cast<unsigned long long>(
				candidate ? candidate->identity : 0),
			epochExpected, candidate ? candidate->validityEpoch : 0,
			static_cast<unsigned long long>(layoutExpected),
			static_cast<unsigned long long>(
				candidate ? candidate->layoutIdentity : 0),
			codePageExpected, candidate ? candidate->codePage : 0,
			state.directProfilesAvailable.load(
				std::memory_order_acquire) ? 1u : 0u,
			candidate && IsSealedDirectProfileValid(*candidate) ? 1u : 0u,
			GetCurrentThreadId());
	}

	void InvalidateSealedDirectFontProfile(RuntimeFont& runtime,
		const DirectProfileInvalidationContext& context)
	{
		const std::shared_ptr<const SealedDirectFontProfile> current =
			LoadRuntimeSealedDirectProfile(runtime);
		StoreRuntimeSealedDirectProfile(runtime, {});
		LogDirectProfileInvalidation(runtime, current, current,
			context, false, current != nullptr);
	}

	void InvalidateSealedDirectFontProfileIfCurrent(RuntimeFont& runtime,
		const std::shared_ptr<const SealedDirectFontProfile>& expected,
		const DirectProfileInvalidationContext& context)
	{
		const std::shared_ptr<const SealedDirectFontProfile> slotBefore =
			LoadRuntimeSealedDirectProfile(runtime);
		const bool cleared = ClearRuntimeSealedDirectProfileIfCurrent(
			runtime, expected);
		LogDirectProfileInvalidation(runtime, expected, slotBefore,
			context, true, cleared);
	}

	bool IsSealedDirectFontProfileUsable(RuntimeFont& runtime,
		const std::shared_ptr<const SealedDirectFontProfile>& sealed,
		float rasterScale)
	{
		if (!sealed || !std::isfinite(rasterScale) || rasterScale <= 0.0f)
			return false;
		const UInt32 scaleMilli = static_cast<UInt32>(std::lround(
			rasterScale * 1000.0f));
		AtlasState& state = State();
		return scaleMilli
			&& state.directProfilesAvailable.load(std::memory_order_acquire)
			&& sealed->validityEpoch == state.directProfileEpoch.load(
				std::memory_order_acquire)
			&& sealed->layoutIdentity == GetRuntimeDirectLayoutIdentity(runtime)
			&& sealed->scaleMilli == scaleMilli
			&& sealed->codePage == GetFreeTypeTextCodePage()
			&& IsSealedDirectProfileValid(*sealed);
	}

	DirectProfileAcquireResult
		AcquireSealedDirectLayoutProfile(RuntimeFont& runtime)
	{
		DirectProfileAcquireResult result;
		const std::shared_ptr<const SealedDirectFontProfile> sealed =
			LoadRuntimeSealedDirectProfile(runtime);
		PopulateDirectProfileAcquireSnapshot(runtime, sealed, result);
		if (!sealed)
		{
			result.status = DirectProfileAcquireStatus::MissingRuntimeSlot;
			LogDirectProfileAcquireFailure(runtime, result);
			return result;
		}
		result.failureMask = GetDirectProfileValidationFailureMask(
			sealed, result.epochExpected,
			result.layoutIdentityExpected, result.codePageExpected);
		if (result.failureMask)
		{
			DirectProfileInvalidationReason reason =
				DirectProfileInvalidationReason::AcquireProfileInvalid;
			if (result.failureMask & kDirectProfileFailureEpoch)
			{
				result.status = DirectProfileAcquireStatus::EpochMismatch;
				reason = DirectProfileInvalidationReason::AcquireEpochMismatch;
			}
			else if (result.failureMask
				& kDirectProfileFailureLayoutIdentity)
			{
				result.status =
					DirectProfileAcquireStatus::LayoutIdentityMismatch;
				reason = DirectProfileInvalidationReason::
					AcquireLayoutIdentityMismatch;
			}
			else if (result.failureMask & kDirectProfileFailureCodePage)
			{
				result.status = DirectProfileAcquireStatus::CodePageMismatch;
				reason = DirectProfileInvalidationReason::AcquireCodePageMismatch;
			}
			else
			{
				result.status = DirectProfileAcquireStatus::ProfileInvalid;
			}
			InvalidateSealedDirectFontProfileIfCurrent(runtime, sealed,
				MakeDirectProfileInvalidation(reason, result.failureMask));
			LogDirectProfileAcquireFailure(runtime, result);
			return result;
		}
		result.status = DirectProfileAcquireStatus::Acquired;
		result.profile = sealed;
		LogDirectProfileAcquireRecovery(runtime, sealed, false);
		return result;
	}

	DirectProfileAcquireResult
		AcquireSealedDirectFontProfile(RuntimeFont& runtime,
			float rasterScale)
	{
		DirectProfileAcquireResult result =
			AcquireSealedDirectLayoutProfile(runtime);
		if (!result.profile)
			return result;
		const UInt32 scaleMilli =
			std::isfinite(rasterScale) && rasterScale > 0.0f
				? static_cast<UInt32>(std::lround(
					rasterScale * 1000.0f))
				: 0;
		result.rasterScaleMilliExpected = scaleMilli;
		if (!scaleMilli || result.profile->scaleMilli != scaleMilli)
		{
			result.status = DirectProfileAcquireStatus::RasterScaleMismatch;
			result.failureMask |= kDirectProfileFailureRasterScale;
			result.profile.reset();
			LogDirectProfileAcquireFailure(runtime, result);
		}
		else
		{
			LogDirectProfileAcquireRecovery(runtime, result.profile, true);
		}
		return result;
	}

	SealedDirectGlyphLookup DecodeSealedDirectGlyph(
		const SealedDirectFontProfile& sealed,
		const char* text, VectorEncodedGlyph& glyph)
	{
		glyph = {};
		if (!text || !*text)
			return SealedDirectGlyphLookup::Invalid;
		UInt32 encodedCode = 0;
		if (text[1]
			&& TryDecodeFreeTypeDoubleByte(text, encodedCode))
		{
			glyph.byteLength = 2;
			glyph.byteClass =
				VectorFontByteClass::DoubleByte;
			glyph.codePoint = 0x10000u;
		}
		else
		{
			encodedCode = static_cast<UInt8>(text[0]);
			glyph.byteLength = 1;
			glyph.byteClass =
				VectorFontByteClass::SingleByte;
			glyph.codePoint = encodedCode;
		}
		glyph.encodedCode = encodedCode;
		const size_t roleIndex =
			static_cast<size_t>(glyph.byteClass);
		if (roleIndex >= sealed.tables.size())
		{
			return SealedDirectGlyphLookup::Invalid;
		}
		if (!sealed.tables[roleIndex])
		{
			// A sealed profile may legitimately omit an encoded role that the
			// current layout never prewarmed. This glyph cannot use the profile,
			// but the profile remains valid for its published roles.
			return SealedDirectGlyphLookup::Unavailable;
		}
		const DirectAtlasGlyphTable& table =
			*sealed.tables[roleIndex];
		size_t slot = 0;
		if (!ResolveDirectGlyphSlot(glyph.byteClass,
				encodedCode, slot))
		{
			return SealedDirectGlyphLookup::Unavailable;
		}
		if (slot > std::numeric_limits<UInt16>::max())
		{
			return SealedDirectGlyphLookup::Invalid;
		}
		if (slot >= table.SlotCount())
		{
			// Slot-domain coverage is a capability boundary. The immutable table
			// is still usable by every encoded glyph that it does cover.
			return SealedDirectGlyphLookup::Unavailable;
		}
		glyph.directSlot = static_cast<UInt16>(slot);
		if (slot >= table.faceIndices.size())
			return SealedDirectGlyphLookup::Invalid;
		glyph.faceIndex = table.faceIndices[slot];
		if (table.recordKind
			== DirectCachedLetterKind::VanillaFontLetter)
		{
			const FontLetter& letter = table.vanillaGlyphs[slot];
			if (letter.iTextureIndex == -1)
				return SealedDirectGlyphLookup::Unavailable;
			if (letter.iTextureIndex < -2)
				return SealedDirectGlyphLookup::Invalid;
			glyph.knownEmpty = letter.iTextureIndex == -2;
			glyph.directWidth = letter.fWidth;
			glyph.directHeight = letter.fHeight;
			glyph.directLeadingEdge = letter.fLeadingEdge;
			glyph.directSpacing = letter.fSpacing;
			glyph.directTopEdge = letter.fTopEdge;
		}
		else
		{
			const DirectCachedLetter& letter =
				table.glyphs[slot];
			if (!letter.flags)
				return SealedDirectGlyphLookup::Unavailable;
			if (!(letter.flags & kDirectCachedLetterValid)
				|| letter.encodedCode != encodedCode
				|| letter.byteClass
					!= static_cast<UInt8>(glyph.byteClass))
			{
				return SealedDirectGlyphLookup::Invalid;
			}
			glyph.knownEmpty =
				(letter.flags & kDirectCachedLetterKnownEmpty) != 0;
			glyph.directWidth = letter.width;
			glyph.directHeight = letter.height;
			glyph.directLeadingEdge = letter.leadingEdge;
			glyph.directSpacing = letter.spacing;
			glyph.directTopEdge = letter.topEdge;
		}
		const auto& baselines =
			sealed.faceBaselineOffsets[roleIndex];
		glyph.directBaselineOffset =
			glyph.faceIndex < baselines.size()
				? baselines[glyph.faceIndex]
				: sealed.roleBaselineOffsets[roleIndex];
		glyph.hasDirectMetrics = true;
		return SealedDirectGlyphLookup::Resolved;
	}

	SealedDirectGlyphLookup DecodeSealedDirectGlyph(RuntimeFont& runtime,
		const char* text, VectorEncodedGlyph& glyph)
	{
		glyph = {};
		if (!text || !*text)
			return SealedDirectGlyphLookup::Invalid;
		std::shared_ptr<const SealedDirectFontProfile> sealed =
			LoadRuntimeSealedDirectProfile(runtime);
		if (!sealed)
			return SealedDirectGlyphLookup::Unavailable;
		AtlasState& state = State();
		if (!state.directProfilesAvailable.load(
			std::memory_order_acquire))
		{
			// During a DEFAULT-pool reset the immutable owner is deliberately
			// retained while GPU publication is unavailable. Do not reopen FT.
			return SealedDirectGlyphLookup::Invalid;
		}
		if (sealed->validityEpoch
				!= state.directProfileEpoch.load(
					std::memory_order_acquire)
			|| sealed->layoutIdentity
				!= GetRuntimeDirectLayoutIdentity(runtime)
			|| sealed->codePage != GetFreeTypeTextCodePage()
			|| !IsSealedDirectProfileValid(*sealed))
		{
			const UInt32 epochExpected = state.directProfileEpoch.load(
				std::memory_order_acquire);
			const UInt64 layoutExpected =
				GetRuntimeDirectLayoutIdentity(runtime);
			const UInt32 codePageExpected = GetFreeTypeTextCodePage();
			const UInt8 failureMask = GetDirectProfileValidationFailureMask(
				sealed, epochExpected, layoutExpected,
				codePageExpected);
			VectorEncodedGlyph diagnosticGlyph;
			if (text && *text)
			{
				UInt32 encodedCode = 0;
				if (text[1]
					&& TryDecodeFreeTypeDoubleByte(text, encodedCode))
				{
					diagnosticGlyph.encodedCode = encodedCode;
					diagnosticGlyph.byteLength = 2;
					diagnosticGlyph.byteClass =
						VectorFontByteClass::DoubleByte;
				}
				else
				{
					diagnosticGlyph.encodedCode =
						static_cast<UInt8>(text[0]);
					diagnosticGlyph.byteLength = 1;
					diagnosticGlyph.byteClass =
						VectorFontByteClass::SingleByte;
				}
			}
			InvalidateSealedDirectFontProfile(runtime,
				MakeDirectProfileInvalidation(
					DirectProfileInvalidationReason::
						DecodeProfileValidationFailed,
					diagnosticGlyph, failureMask));
			return SealedDirectGlyphLookup::Unavailable;
		}
		const SealedDirectGlyphLookup result =
			DecodeSealedDirectGlyph(*sealed, text, glyph);
		if (result == SealedDirectGlyphLookup::Invalid)
		{
			InvalidateSealedDirectFontProfile(runtime,
				MakeDirectProfileInvalidation(
					DirectProfileInvalidationReason::DecodeGlyphInvalid,
					glyph));
		}
		return result;
	}

	bool GetSealedDirectAtlasGlyphBatch(RuntimeFont& runtime,
		const std::shared_ptr<const SealedDirectFontProfile>& sealed,
		const std::vector<DirectGlyphCommand>& glyphs,
		GlyphMaskType maskType, float rasterScale,
		AtlasPixelMode pixelMode, AtlasRenderMode renderMode,
		UInt32 padding, DirectAtlasGlyphBatch& result)
	{
		result.Clear();
		if (!sealed)
			return false;
		if (glyphs.empty())
		{
			result.sealed = sealed;
			result.tables = sealed->tables;
			return true;
		}
		const size_t maskIndex = static_cast<size_t>(maskType);
		if (maskIndex >= kDirectAtlasMaskCount)
			return false;

		const std::shared_ptr<const SealedDirectFontProfile> published =
			LoadRuntimeSealedDirectProfile(runtime);
		if (published.get() != sealed.get()
			|| !IsSealedDirectFontProfileUsable(runtime, sealed, rasterScale)
			|| sealed->pixelMode != pixelMode
			|| sealed->renderMode != renderMode
			|| sealed->padding != padding)
		{
			// This builder/profile pairing is stale or incompatible. Acquisition
			// performs the authoritative epoch/layout/codepage validity checks;
			// raster/configuration mismatches are local to this draw.
			return false;
		}
		const UInt8 requestedMask = static_cast<UInt8>(
			1u << static_cast<UInt8>(maskType));
		if (!(sealed->effectLayerMask & requestedMask))
			return false;

		result.sealed = sealed;
		result.tables = sealed->tables;
		result.glyphs.resize(glyphs.size());
		for (size_t glyphIndex = 0;
			glyphIndex < glyphs.size(); ++glyphIndex)
		{
			const DirectGlyphCommand& glyph = glyphs[glyphIndex];
			const size_t roleIndex = glyph.byteClass;
			if (roleIndex >= sealed->tables.size()
				|| !sealed->tables[roleIndex]
				|| !glyph.byteLength
				|| glyph.directSlot
					== std::numeric_limits<UInt16>::max())
			{
				// A malformed prepared command invalidates this synchronous batch,
				// not the immutable profile from which other sidecars were built.
				result.Clear();
				return false;
			}
			const DirectAtlasGlyphTable& table =
				*sealed->tables[roleIndex];
			size_t expectedSlot = 0;
			const VectorFontByteClass byteClass =
				static_cast<VectorFontByteClass>(roleIndex);
			if (!ResolveDirectGlyphSlot(byteClass,
					glyph.encodedCode, expectedSlot)
				|| expectedSlot != glyph.directSlot
				|| expectedSlot >= table.SlotCount())
			{
				// The encoded-slot witness belongs to the prepared command. Reject a
				// stale command without revoking the still-valid shared profile.
				result.Clear();
				return false;
			}

			DirectAtlasBatchGlyph& output =
				result.glyphs[glyphIndex];
			output.byteClass = glyph.byteClass;
			UInt16 localPage = kInvalidDirectAtlasPageSlot;
			if (table.recordKind
				== DirectCachedLetterKind::VanillaFontLetter)
			{
				if (maskType != GlyphMaskType::Composite)
				{
					// Vanilla-letter tables have no distance-field layer. This is a
					// caller capability mismatch, not profile corruption.
					result.Clear();
					return false;
				}
				const FontLetter& letter =
					table.vanillaGlyphs[expectedSlot];
				if (letter.iTextureIndex == -2)
				{
					output.knownEmpty = true;
					continue;
				}
				if (letter.iTextureIndex < 0
					|| letter.iTextureIndex
						>= kMaximumAtlasSnapshotPages)
				{
					InvalidateSealedDirectFontProfileIfCurrent(runtime, sealed,
						MakeDirectProfileInvalidation(
							DirectProfileInvalidationReason::
								SealedBatchTextureIndexInvalid,
							glyph.encodedCode, glyph.byteLength,
							glyph.byteClass));
					result.Clear();
					return false;
				}
				localPage =
					static_cast<UInt16>(letter.iTextureIndex);
				output.vanillaLetter = &letter;
			}
			else
			{
				const DirectCachedLetter& letter =
					table.glyphs[expectedSlot];
				if (!(letter.flags & kDirectCachedLetterValid)
					|| letter.encodedCode != glyph.encodedCode
					|| letter.byteClass != glyph.byteClass)
				{
					InvalidateSealedDirectFontProfileIfCurrent(runtime, sealed,
						MakeDirectProfileInvalidation(
							DirectProfileInvalidationReason::
								SealedBatchGlyphRecordMismatch,
							glyph.encodedCode, glyph.byteLength,
							glyph.byteClass));
					result.Clear();
					return false;
				}
				if (letter.flags & kDirectCachedLetterKnownEmpty)
				{
					output.knownEmpty = true;
					continue;
				}
				const DirectAtlasGlyphLayer* layer =
					FindDirectLayer(letter, maskType);
				if (!layer || layer->pageSlot
						>= kMaximumAtlasSnapshotPages)
				{
					InvalidateSealedDirectFontProfileIfCurrent(runtime, sealed,
						MakeDirectProfileInvalidation(
							DirectProfileInvalidationReason::
								SealedBatchLayerInvalid,
							glyph.encodedCode, glyph.byteLength,
							glyph.byteClass));
					result.Clear();
					return false;
				}
				localPage = layer->pageSlot;
				output.snapshotPlacementIndex =
					layer->snapshotPlacementIndex;
			}
			const UInt16 ordinal =
				sealed->pageOrdinals[roleIndex][localPage];
			if (ordinal >= sealed->atlases.size())
			{
				InvalidateSealedDirectFontProfileIfCurrent(runtime, sealed,
					MakeDirectProfileInvalidation(
						DirectProfileInvalidationReason::
							SealedBatchPageOrdinalInvalid,
						glyph.encodedCode, glyph.byteLength,
						glyph.byteClass));
				result.Clear();
				return false;
			}
			output.atlasPage = ordinal;
			if (output.vanillaLetter)
				continue;
			const auto& page = sealed->atlases[ordinal];
			if (!page || !page->compactSnapshot
				|| output.snapshotPlacementIndex
					>= page->compactSnapshot->placements.size())
			{
				InvalidateSealedDirectFontProfileIfCurrent(runtime, sealed,
					MakeDirectProfileInvalidation(
						DirectProfileInvalidationReason::
							SealedBatchSnapshotPlacementInvalid,
						glyph.encodedCode, glyph.byteLength,
						glyph.byteClass));
				result.Clear();
				return false;
			}
			const AtlasSnapshotPlacement& placement =
				page->compactSnapshot->placements[
					output.snapshotPlacementIndex];
			if (placement.maskType
				!= static_cast<UInt8>(maskType))
			{
				InvalidateSealedDirectFontProfileIfCurrent(runtime, sealed,
					MakeDirectProfileInvalidation(
						DirectProfileInvalidationReason::
							SealedBatchPlacementMaskMismatch,
						glyph.encodedCode, glyph.byteLength,
						glyph.byteClass));
				result.Clear();
				return false;
			}
			output.placement = &placement;
		}
		RecordFreeTypePerf(
			FreeTypePerfCounter::GpuResidentGlyphHit,
			static_cast<UInt64>(glyphs.size()));
		return true;
	}

	bool GetDirectAtlasGlyphBatch(RuntimeFont& runtime,
		const std::vector<AtlasGlyphInstance>& glyphs,
		GlyphMaskType maskType, float rasterScale,
		AtlasPixelMode pixelMode, AtlasRenderMode renderMode,
		UInt32 padding, DirectAtlasGlyphBatch& result)
	{
		result.Clear();
		if (glyphs.empty())
			return true;
		const size_t maskIndex = static_cast<size_t>(maskType);
		if (maskIndex >= kDirectAtlasMaskCount)
			return false;

		std::shared_ptr<const SealedDirectFontProfile> sealed =
			LoadRuntimeSealedDirectProfile(runtime);
		AtlasState& state = State();
		if (sealed
			&& IsSealedDirectFontProfileUsable(
				runtime, sealed, rasterScale)
			&& sealed->pixelMode == pixelMode
			&& sealed->renderMode == renderMode
			&& sealed->padding == padding)
		{
			const UInt8 requestedMask =
				static_cast<UInt8>(1u << static_cast<UInt8>(
					maskType));
			if (!(sealed->effectLayerMask & requestedMask))
			{
				return false;
			}
			result.sealed = sealed;
			result.tables = sealed->tables;
			result.glyphs.resize(glyphs.size());
			for (size_t glyphIndex = 0;
				glyphIndex < glyphs.size(); ++glyphIndex)
			{
				const VectorEncodedGlyph& glyph =
					glyphs[glyphIndex].glyph;
				const size_t roleIndex =
					static_cast<size_t>(glyph.byteClass);
				if (roleIndex >= sealed->tables.size()
					|| !sealed->tables[roleIndex])
				{
					// A generic glyph may target a role that was not part of this
					// profile. Let the dynamic atlas handle the batch locally.
					result.Clear();
					return false;
				}
				const DirectAtlasGlyphTable& table =
					*sealed->tables[roleIndex];
				// A generic decode deliberately lacks a sealed slot when a code point
				// was not part of the immutable prewarm table. That is an ordinary
				// direct-cache miss, not profile corruption.
				if (!glyph.hasDirectMetrics
					|| glyph.directSlot
						== std::numeric_limits<UInt16>::max())
				{
					result.Clear();
					return false;
				}
				const size_t glyphSlot = glyph.directSlot;
				if (glyphSlot >= table.SlotCount())
				{
					// directSlot is carried by this decoded glyph. An out-of-range
					// witness is a local cache miss, not evidence against the table.
					result.Clear();
					return false;
				}
				size_t expectedSlot = 0;
				if (!ResolveDirectGlyphSlot(glyph.byteClass,
						glyph.encodedCode, expectedSlot)
					|| expectedSlot != glyphSlot)
				{
					result.Clear();
					return false;
				}
				DirectAtlasBatchGlyph& output =
					result.glyphs[glyphIndex];
				output.byteClass =
					static_cast<UInt8>(glyph.byteClass);
				UInt16 localPage = kInvalidDirectAtlasPageSlot;
				if (table.recordKind
					== DirectCachedLetterKind::VanillaFontLetter)
				{
					if (maskType != GlyphMaskType::Composite)
					{
						// The requested representation is absent from a vanilla
						// table; retain the table for compatible draws.
						result.Clear();
						return false;
					}
					const FontLetter& letter =
						table.vanillaGlyphs[glyphSlot];
					if (letter.iTextureIndex == -2)
					{
						output.knownEmpty = true;
						continue;
					}
					if (letter.iTextureIndex < 0
						|| letter.iTextureIndex
							>= kMaximumAtlasSnapshotPages)
					{
						InvalidateSealedDirectFontProfileIfCurrent(
							runtime, sealed,
							MakeDirectProfileInvalidation(
								DirectProfileInvalidationReason::
									DirectBatchTextureIndexInvalid,
								glyph));
						result.Clear();
						return false;
					}
					localPage = static_cast<UInt16>(
						letter.iTextureIndex);
					output.vanillaLetter = &letter;
				}
				else
				{
					const DirectCachedLetter& letter =
						table.glyphs[glyphSlot];
					if (!(letter.flags & kDirectCachedLetterValid)
						|| letter.encodedCode != glyph.encodedCode
						|| letter.byteClass
							!= static_cast<UInt8>(glyph.byteClass))
					{
						InvalidateSealedDirectFontProfileIfCurrent(
							runtime, sealed,
							MakeDirectProfileInvalidation(
								DirectProfileInvalidationReason::
									DirectBatchGlyphRecordMismatch,
								glyph));
						result.Clear();
						return false;
					}
					if (letter.flags & kDirectCachedLetterKnownEmpty)
					{
						output.knownEmpty = true;
						continue;
					}
					const DirectAtlasGlyphLayer* layer =
						FindDirectLayer(letter, maskType);
					if (!layer || layer->pageSlot
							>= kMaximumAtlasSnapshotPages)
					{
						InvalidateSealedDirectFontProfileIfCurrent(
							runtime, sealed,
							MakeDirectProfileInvalidation(
								DirectProfileInvalidationReason::
									DirectBatchLayerInvalid,
								glyph));
						result.Clear();
						return false;
					}
					localPage = layer->pageSlot;
					output.snapshotPlacementIndex =
						layer->snapshotPlacementIndex;
				}
				const UInt16 ordinal =
					sealed->pageOrdinals[roleIndex][localPage];
				if (ordinal >= sealed->atlases.size())
				{
					InvalidateSealedDirectFontProfileIfCurrent(
						runtime, sealed,
						MakeDirectProfileInvalidation(
							DirectProfileInvalidationReason::
								DirectBatchPageOrdinalInvalid,
							glyph));
					result.Clear();
					return false;
				}
				output.atlasPage = ordinal;
				if (output.vanillaLetter)
					continue;
				const auto& page = sealed->atlases[ordinal];
				if (!page || !page->compactSnapshot
					|| output.snapshotPlacementIndex
						>= page->compactSnapshot->placements.size())
				{
					InvalidateSealedDirectFontProfileIfCurrent(
						runtime, sealed,
						MakeDirectProfileInvalidation(
							DirectProfileInvalidationReason::
								DirectBatchSnapshotPlacementInvalid,
							glyph));
					result.Clear();
					return false;
				}
				const AtlasSnapshotPlacement& placement =
					page->compactSnapshot->placements[
						output.snapshotPlacementIndex];
				if (placement.maskType
					!= static_cast<UInt8>(maskType))
				{
					InvalidateSealedDirectFontProfileIfCurrent(
						runtime, sealed,
						MakeDirectProfileInvalidation(
							DirectProfileInvalidationReason::
								DirectBatchPlacementMaskMismatch,
							glyph));
					result.Clear();
					return false;
				}
				output.placement = &placement;
			}
			RecordFreeTypePerf(
				FreeTypePerfCounter::GpuResidentGlyphHit,
				static_cast<UInt64>(glyphs.size()));
			return true;
		}
		if (sealed)
		{
			// Most commonly this is a raster-scale/configuration mismatch. Keep
			// the sealed slot for draws matching its immutable contract and use
			// the dynamic atlas for this batch.
			return false;
		}

		std::array<bool, 2> roleUsed = {};
		for (const AtlasGlyphInstance& instance : glyphs)
		{
			const size_t roleIndex =
				static_cast<size_t>(instance.glyph.byteClass);
			if (roleIndex >= roleUsed.size())
				return false;
			roleUsed[roleIndex] = true;
		}

		const FontConfig& config = GetRuntimeConfig(runtime);
		std::array<AtlasCacheKey, 2> baseKeys;
		std::array<std::array<bool, kMaximumAtlasSnapshotPages>, 2>
			usedPages = {};
		std::array<std::array<UInt16, kMaximumAtlasSnapshotPages>, 2>
			pageOrdinals;
		for (auto& role : pageOrdinals)
			role.fill(kInvalidDirectAtlasPageSlot);

		std::lock_guard<std::mutex> lock(state.atlasMutex);
		auto fail = [&](const char* stage)
		{
			result.Clear();
			const UInt64 logKey = 0x4000000000000000ull
				^ (static_cast<UInt64>(config.fontId) << 32)
				^ (static_cast<UInt64>(std::lround(
					rasterScale * 1000.0f)) << 8)
				^ (static_cast<UInt64>(pixelMode) << 4)
				^ static_cast<UInt64>(renderMode);
			if (g_bEnableFreeTypeFontRenderingLog
				&& state.loggedDirectGlyphBatches.insert(logKey).second)
			{
				gLog.FormattedMessage(
					"tnvse_freetype_font: direct atlas geometry batch unavailable font=%u sourceScale=%.3f mode=%u render=%u stage=%s",
					config.fontId, rasterScale,
					static_cast<UInt32>(pixelMode),
					static_cast<UInt32>(renderMode), stage);
			}
			return false;
		};

		for (size_t roleIndex = 0; roleIndex < roleUsed.size(); ++roleIndex)
		{
			if (!roleUsed[roleIndex])
				continue;
			const VectorFontByteClass byteClass =
				static_cast<VectorFontByteClass>(roleIndex);
			if (!ResolvePrewarmAtlasKey(config, byteClass,
					rasterScale, baseKeys[roleIndex])
				|| baseKeys[roleIndex].pixelMode != pixelMode
				|| baseKeys[roleIndex].renderMode != renderMode
				|| baseKeys[roleIndex].padding != padding)
			{
				return fail("profile-key");
			}
			const AtlasProfileKey profileKey =
				MakeAtlasProfileKey(baseKeys[roleIndex]);
			const auto profile = state.atlasProfiles.find(profileKey);
			if (profile == state.atlasProfiles.end()
				|| state.completeAtlasProfiles.find(profileKey)
					== state.completeAtlasProfiles.end()
				|| !profile->second.directGlyphs)
			{
				return fail("profile-table");
			}
			result.tables[roleIndex] = profile->second.directGlyphs;
		}

		result.glyphs.resize(glyphs.size());
		for (size_t glyphIndex = 0; glyphIndex < glyphs.size(); ++glyphIndex)
		{
			const VectorEncodedGlyph& glyph = glyphs[glyphIndex].glyph;
			const size_t roleIndex = static_cast<size_t>(glyph.byteClass);
			if (roleIndex >= result.tables.size()
				|| !result.tables[roleIndex])
			{
				return fail("table-role");
			}
			const DirectAtlasGlyphTable& table =
				*result.tables[roleIndex];
			size_t glyphSlot = 0;
			if (!ResolveDirectGlyphSlot(glyph.byteClass,
					glyph.encodedCode, glyphSlot)
				|| glyphSlot >= table.glyphs.size())
			{
				return fail("encoded-slot");
			}
			const DirectCachedLetter& letter = table.glyphs[glyphSlot];
			if (!(letter.flags & kDirectCachedLetterValid)
				|| letter.encodedCode != glyph.encodedCode
				|| letter.byteClass != static_cast<UInt8>(glyph.byteClass))
			{
				return fail("letter-identity");
			}

			DirectAtlasBatchGlyph& output = result.glyphs[glyphIndex];
			output.byteClass = static_cast<UInt8>(glyph.byteClass);
			if (letter.flags & kDirectCachedLetterKnownEmpty)
			{
				if (!glyph.knownEmpty)
					return fail("known-empty");
				output.knownEmpty = true;
				continue;
			}

			const DirectAtlasGlyphLayer* layer =
				FindDirectLayer(letter, maskType);
			if (!layer || layer->reserved
				|| layer->pageSlot >= table.pages.size()
				|| layer->pageSlot >= kMaximumAtlasSnapshotPages)
			{
				return fail("direct-layer");
			}
			usedPages[roleIndex][layer->pageSlot] = true;
			output.snapshotPlacementIndex =
				layer->snapshotPlacementIndex;
			// This is a role-local page until the deterministic page pass below.
			output.atlasPage = layer->pageSlot;
		}

		for (size_t roleIndex = 0; roleIndex < roleUsed.size(); ++roleIndex)
		{
			if (!roleUsed[roleIndex])
				continue;
			const DirectAtlasGlyphTable& table =
				*result.tables[roleIndex];
			for (UInt16 pageSlot = 0;
				pageSlot < kMaximumAtlasSnapshotPages; ++pageSlot)
			{
				if (!usedPages[roleIndex][pageSlot])
					continue;
				if (pageSlot >= table.pages.size())
					return fail("page-slot");
				std::shared_ptr<AtlasResource> page =
					table.pages[pageSlot].lock();
				const AtlasCacheKey& key = baseKeys[roleIndex];
				if (!page || !page->compactSnapshot
					|| !page->pageContentHash
					|| page->compactSnapshot->sourceHeader.pageContentHash
						!= page->pageContentHash
					|| page->pixelMode != key.pixelMode
					|| page->renderMode != key.renderMode
					|| page->padding != key.padding
					|| page->levelZeroOnly != key.levelZeroOnly
					|| !page->width || !page->height || !page->property
					|| !GetAtlasTexture(*page))
				{
					return fail("page-resource");
				}

				UInt16 ordinal = kInvalidDirectAtlasPageSlot;
				for (UInt16 candidate = 0;
					candidate < result.atlases.size(); ++candidate)
				{
					if (result.atlases[candidate].get() == page.get()
						|| AreAtlasResourcesBackedBySameTexture(
							*result.atlases[candidate], *page))
					{
						ordinal = candidate;
						break;
					}
				}
				if (ordinal == kInvalidDirectAtlasPageSlot)
				{
					if (result.atlases.size()
						>= kMaximumAtlasSnapshotPages)
					{
						return fail("page-limit");
					}
					ordinal = static_cast<UInt16>(
						result.atlases.size());
					result.atlases.push_back(std::move(page));
				}
				pageOrdinals[roleIndex][pageSlot] = ordinal;
			}
		}

		for (DirectAtlasBatchGlyph& glyph : result.glyphs)
		{
			if (glyph.knownEmpty)
				continue;
			const size_t roleIndex = glyph.byteClass;
			if (roleIndex >= pageOrdinals.size()
				|| glyph.atlasPage >= kMaximumAtlasSnapshotPages)
			{
				return fail("page-remap-source");
			}
			const UInt16 ordinal =
				pageOrdinals[roleIndex][glyph.atlasPage];
			if (ordinal >= result.atlases.size())
				return fail("page-remap-target");
			const std::shared_ptr<AtlasResource>& page =
				result.atlases[ordinal];
			if (!page || !page->compactSnapshot
				|| glyph.snapshotPlacementIndex
					>= page->compactSnapshot->placements.size())
			{
				return fail("placement-remap-source");
			}
			const AtlasSnapshotPlacement& placement =
				page->compactSnapshot->placements[
					glyph.snapshotPlacementIndex];
			if (placement.maskType != static_cast<UInt8>(maskType)
				|| !IsValidAtlasSnapshotGlyphPlacement(placement,
					page->width, page->height,
					page->compactSnapshot->sourceHeader.pageIndex))
			{
				return fail("placement-remap-target");
			}
			glyph.atlasPage = ordinal;
			glyph.placement = &placement;
		}

		RecordFreeTypePerf(FreeTypePerfCounter::GpuResidentGlyphHit,
			static_cast<UInt64>(glyphs.size()));
		const UInt64 logKey = 0x8000000000000000ull
			^ (static_cast<UInt64>(config.fontId) << 32)
			^ (static_cast<UInt64>(std::lround(
				rasterScale * 1000.0f)) << 8)
			^ (static_cast<UInt64>(pixelMode) << 4)
			^ static_cast<UInt64>(renderMode);
		if (!result.atlases.empty()
			&& state.loggedDirectGlyphBatches.insert(logKey).second)
		{
			gLog.FormattedMessage(
				"tnvse_freetype_font: direct atlas geometry batch font=%u sourceScale=%.3f mode=%u render=%u glyphs=%u pages=%u source=dense-cached-letter",
				config.fontId, rasterScale,
				static_cast<UInt32>(pixelMode),
				static_cast<UInt32>(renderMode),
				static_cast<UInt32>(glyphs.size()),
				static_cast<UInt32>(result.atlases.size()));
		}
		return true;
	}

	bool GetDirectAtlasGlyphSources(RuntimeFont& runtime,
		const std::vector<GlyphBitmapRequest>& requests, float rasterScale,
		AtlasPixelMode pixelMode, AtlasRenderMode renderMode, UInt32 padding,
		std::vector<PendingQuad::GlyphSource>& results)
	{
		results.assign(requests.size(), {});
		if (requests.empty())
			return true;

		const FontConfig& config = GetRuntimeConfig(runtime);
		std::array<AtlasCacheKey, 2> baseKeys;
		std::array<std::shared_ptr<const DirectAtlasGlyphTable>, 2> tables;
		AtlasState& state = State();
		std::lock_guard<std::mutex> lock(state.atlasMutex);
		for (size_t roleIndex = 0; roleIndex < baseKeys.size(); ++roleIndex)
		{
			const VectorFontByteClass byteClass =
				static_cast<VectorFontByteClass>(roleIndex);
			if (!ResolvePrewarmAtlasKey(config, byteClass,
				rasterScale, baseKeys[roleIndex])
				|| baseKeys[roleIndex].pixelMode != pixelMode
				|| baseKeys[roleIndex].renderMode != renderMode
				|| baseKeys[roleIndex].padding != padding)
			{
				return false;
			}
			const auto profile = state.atlasProfiles.find(
				MakeAtlasProfileKey(baseKeys[roleIndex]));
			if (profile != state.atlasProfiles.end()
				&& state.completeAtlasProfiles.find(
					MakeAtlasProfileKey(baseKeys[roleIndex]))
					!= state.completeAtlasProfiles.end())
				tables[roleIndex] = profile->second.directGlyphs;
		}

		for (size_t requestIndex = 0;
			requestIndex < requests.size(); ++requestIndex)
		{
			const GlyphBitmapRequest& request = requests[requestIndex];
			if (!request.glyph)
				return false;
			const size_t roleIndex =
				static_cast<size_t>(request.glyph->byteClass);
			if (roleIndex >= tables.size() || !tables[roleIndex])
				return false;
			const std::shared_ptr<const DirectAtlasGlyphTable>& table =
				tables[roleIndex];
			size_t glyphSlot = 0;
			if (!ResolveDirectGlyphSlot(request.glyph->byteClass,
				request.glyph->encodedCode, glyphSlot)
				|| glyphSlot >= table->glyphs.size())
			{
				return false;
			}
			if (static_cast<size_t>(request.maskType)
				>= kDirectAtlasMaskCount)
				return false;
			const DirectCachedLetter& letter =
				table->glyphs[glyphSlot];
			if (!(letter.flags & kDirectCachedLetterValid)
				|| letter.encodedCode != request.glyph->encodedCode
				|| letter.byteClass
					!= static_cast<UInt8>(request.glyph->byteClass))
			{
				return false;
			}
			if (letter.flags & kDirectCachedLetterKnownEmpty)
			{
				if (!request.glyph->knownEmpty)
					return false;
				results[requestIndex].knownEmpty = true;
				continue;
			}
			if (!ResolveDirectGlyphSource(*table, *request.glyph,
				request.maskType, results[requestIndex]))
				return false;
		}
		RecordFreeTypePerf(FreeTypePerfCounter::GpuResidentGlyphHit,
			static_cast<UInt64>(requests.size()));
		const UInt64 logKey =
			(static_cast<UInt64>(config.fontId) << 32)
			| (static_cast<UInt64>(std::lround(rasterScale * 1000.0f))
				<< 8)
			| (static_cast<UInt64>(pixelMode) << 4)
			| static_cast<UInt64>(renderMode);
		if (state.loggedDirectGlyphBatches.insert(logKey).second)
		{
			gLog.FormattedMessage(
				"tnvse_freetype_font: direct atlas glyph batch font=%u sourceScale=%.3f mode=%u render=%u glyphs=%u source=immutable-snapshot-placement",
				config.fontId, rasterScale,
				static_cast<UInt32>(pixelMode),
				static_cast<UInt32>(renderMode),
				static_cast<UInt32>(requests.size()));
		}
		return true;
	}

	bool GetDirectAtlasGlyphSources(RuntimeFont& runtime,
		const std::vector<AtlasGlyphInstance>& glyphs,
		GlyphMaskType maskType, float rasterScale,
		AtlasPixelMode pixelMode, AtlasRenderMode renderMode,
		UInt32 padding, std::vector<PendingQuad::GlyphSource>& results)
	{
		results.assign(glyphs.size(), {});
		if (glyphs.empty())
			return true;
		if (static_cast<size_t>(maskType) >= kDirectAtlasMaskCount)
			return false;

		const FontConfig& config = GetRuntimeConfig(runtime);
		std::array<AtlasCacheKey, 2> baseKeys;
		std::array<std::shared_ptr<const DirectAtlasGlyphTable>, 2> tables;
		AtlasState& state = State();
		std::lock_guard<std::mutex> lock(state.atlasMutex);
		for (size_t roleIndex = 0; roleIndex < baseKeys.size();
			++roleIndex)
		{
			const VectorFontByteClass byteClass =
				static_cast<VectorFontByteClass>(roleIndex);
			if (!ResolvePrewarmAtlasKey(config, byteClass,
				rasterScale, baseKeys[roleIndex])
				|| baseKeys[roleIndex].pixelMode != pixelMode
				|| baseKeys[roleIndex].renderMode != renderMode
				|| baseKeys[roleIndex].padding != padding)
			{
				return false;
			}
			const auto profile = state.atlasProfiles.find(
				MakeAtlasProfileKey(baseKeys[roleIndex]));
			if (profile != state.atlasProfiles.end()
				&& state.completeAtlasProfiles.find(
					MakeAtlasProfileKey(baseKeys[roleIndex]))
					!= state.completeAtlasProfiles.end())
				tables[roleIndex] = profile->second.directGlyphs;
		}

		for (size_t glyphIndex = 0; glyphIndex < glyphs.size();
			++glyphIndex)
		{
			const VectorEncodedGlyph& glyph =
				glyphs[glyphIndex].glyph;
			const size_t roleIndex =
				static_cast<size_t>(glyph.byteClass);
			if (roleIndex >= tables.size() || !tables[roleIndex])
				return false;
			size_t glyphSlot = 0;
			if (!ResolveDirectGlyphSlot(glyph.byteClass,
				glyph.encodedCode, glyphSlot)
				|| glyphSlot >= tables[roleIndex]->glyphs.size())
				return false;
			const DirectCachedLetter& letter =
				tables[roleIndex]->glyphs[glyphSlot];
			if ((letter.flags & kDirectCachedLetterKnownEmpty)
				&& (letter.flags & kDirectCachedLetterValid))
			{
				if (!glyph.knownEmpty)
					return false;
				results[glyphIndex].knownEmpty = true;
				continue;
			}
			if (ResolveDirectGlyphSource(*tables[roleIndex], glyph,
				maskType, results[glyphIndex]))
				continue;
			return false;
		}
		RecordFreeTypePerf(FreeTypePerfCounter::GpuResidentGlyphHit,
			static_cast<UInt64>(glyphs.size()));
		const UInt64 logKey =
			(static_cast<UInt64>(config.fontId) << 32)
			| (static_cast<UInt64>(std::lround(
				rasterScale * 1000.0f)) << 8)
			| (static_cast<UInt64>(pixelMode) << 4)
			| static_cast<UInt64>(renderMode);
		if (state.loggedDirectGlyphBatches.insert(logKey).second)
		{
			gLog.FormattedMessage(
				"tnvse_freetype_font: direct atlas glyph batch font=%u sourceScale=%.3f mode=%u render=%u glyphs=%u source=persistent-direct-cached-letter",
				config.fontId, rasterScale,
				static_cast<UInt32>(pixelMode),
				static_cast<UInt32>(renderMode),
				static_cast<UInt32>(glyphs.size()));
		}
		return true;
	}
}

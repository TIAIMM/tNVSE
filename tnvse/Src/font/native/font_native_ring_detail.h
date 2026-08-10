#pragma once

#include "font_native_shape_internal.h"
#include "font_native_internal.h"

#include "BSShaderProperty.hpp"
#include "NiAlphaProperty.hpp"
#include "NiDX9Renderer.hpp"
#include "NiDX9ShaderDeclaration.hpp"
#include "NiGeometryBufferData.hpp"
#include "NiMemory.hpp"
#include "NiPoint4.hpp"
#include "NiTexturingProperty.hpp"
#include "NiTriShapeData.hpp"
#include "NiVBChip.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace fonthook::vectorfont::implementation::font_native_ring
{
		inline constexpr UInt32 kScissorTriShapeSize = 0xD4;
		inline constexpr UInt32 kScissorTailOffset = 0xC4;
		inline constexpr UInt32 kScissorTailSize = 0x10;
		inline constexpr UInt32 kProxyPoolSize = 4;
		inline constexpr UInt32 kRingTargetVertexCapacity =
			kNativeFontMaximumQuads * 4u * 2u;
		inline constexpr UInt32 kStaticTargetVertexCapacity =
			kRingTargetVertexCapacity * 4u;
		inline constexpr UInt32 kStaticInitialVertexBytes = 4u * 1024u * 1024u;
		inline constexpr UInt32 kStaticInitialVertexCapacity =
			(kStaticInitialVertexBytes / sizeof(NativeFontGpuVertex)) & ~3u;
		inline constexpr UInt32 kStaticPromotionMinimumFrameCount = 2;
		inline constexpr UInt32 kStaticPromotionMaximumBaseFrameCount = 16;
		inline constexpr UInt32 kStaticPromotionMaximumOversizeFrameCount = 32;
		inline constexpr UInt32 kStaticPromotionMaximumFrameGap = 2;
		inline constexpr UInt32 kStaticPromotionRetryFrames = 8;
		inline constexpr UInt32 kStaticPromotionBudgetBytes = 1024u * 1024u;
		inline constexpr UInt32 kStaticPromotionPayloadLimit = 128;
		inline constexpr UInt32 kStaticCandidateInactiveFrames = 600;
		inline constexpr UInt32 kStaticCandidateSweepIntervalFrames = 60;
		inline constexpr UInt32 kStaticCompactionCooldownFrames = 2;
		inline constexpr UInt32 kStaticCompactionReserveDivisor = 8;
		inline constexpr size_t kStaticCandidateLimit = 4096;
		inline constexpr UInt32 kCanonicalIndexCount =
			kNativeFontMaximumQuads * 6u;
		inline constexpr UInt32 kCanonicalIndexBytes =
			kCanonicalIndexCount * sizeof(UInt16);
		inline constexpr UInt32 kCanonicalArrayCount = 1;

		struct TileShaderPropertyView : BSShaderProperty
		{
			NiTexturePtr sourceTexture;
			NiTexturePtr alphaTexture;
			NiColorA overlayColor;
			float tileAlpha = 1.0f;
			NiPoint4 textureTransform;
			NiTexturingProperty::ClampMode clampMode =
				NiTexturingProperty::CLAMP_S_CLAMP_T;
			bool byte90 = false;
			bool rotates = false;
			bool hasVertexColors = false;
			bool noTexture = false;
			BSStringT<char> texturePath;
			RECT scissorRect = {};
			bool useScissorTest = false;
		};

		static_assert(sizeof(NiTriShape) == kScissorTailOffset);
		static_assert(kScissorTailOffset + kScissorTailSize
			== kScissorTriShapeSize);
		static_assert(sizeof(TileShaderPropertyView) == 0xB0);
		// Retail TileShader::SetupGeometryConstants reads the live color at +0x68 and
		// tile alpha at +0x78. Keep the proxy view tied to that executable ABI;
		// a layout drift here would silently turn every shader-side color fix into
		// reads from the wrong fields.
		static_assert(offsetof(TileShaderPropertyView, overlayColor) == 0x68);
		static_assert(offsetof(TileShaderPropertyView, tileAlpha) == 0x78);
		static_assert(offsetof(TileShaderPropertyView, textureTransform) == 0x7C);
		static_assert(offsetof(TileShaderPropertyView, clampMode) == 0x8C);
		static_assert(offsetof(TileShaderPropertyView, rotates) == 0x91);
		static_assert(offsetof(TileShaderPropertyView, hasVertexColors) == 0x92);

		struct NativeFontProxy
		{
			NiTriShapePtr shape;
			NiAlphaPropertyPtr alphaProperty;
			NiGeometryBufferData* buffer = nullptr;
			NiVBChip* chip = nullptr;
			TileShaderPropertyView* tile = nullptr;
			NiTexturingProperty* atlasProperty = nullptr;
			NiTexture* atlasTexture = nullptr;
			BSShader* shader = nullptr;
			bool inUse = false;
		};

		struct NativeFontUploadedPayload
		{
			std::weak_ptr<const NativeFontPayloadTemplate> owner;
			UInt32 baseVertex = 0;
			UInt32 vertexCount = 0;
			UInt32 epoch = 0;
			UInt32 writeSerial = 0;
			UInt32 discardSerial = 0;
			UInt64 payloadHash = 0;
		};

		struct NativeFontStaticPayload
		{
			std::weak_ptr<const NativeFontPayloadTemplate> owner;
			UInt32 baseVertex = 0;
			UInt32 vertexCount = 0;
			UInt32 writeSerial = 0;
			UInt64 payloadHash = 0;
		};

		struct NativeFontStaticCandidate
		{
			CpuMemoryLease cpuMemory;
			std::weak_ptr<const NativeFontPayloadTemplate> owner;
			UInt32 firstObservedFrame = 0;
			UInt32 lastObservedFrame = 0;
			UInt32 activeObservedFrames = 0;
			UInt32 dynamicUploadEpochCount = 0;
			UInt32 lastDynamicUploadEpoch = 0;
			UInt32 lastDynamicUploadResourceSerial = 0;
			UInt32 nextRetryFrame = 0;
			bool observationFrameValid = false;
			bool promotionDisabled = false;
		};

		struct NativeFontStaticHotEntry
		{
			const NativeFontPayloadTemplate* key = nullptr;
			std::weak_ptr<const NativeFontPayloadTemplate> owner;
			UInt32 baseVertex = 0;
			UInt32 vertexCount = 0;
			UInt32 resourceSerial = 0;
		};

		struct NativeFontUploadHotEntry
		{
			const NativeFontPayloadTemplate* key = nullptr;
			NativeFontPayloadTemplatePtr owner;
			UInt32 baseVertex = 0;
			UInt32 vertexCount = 0;
			UInt32 epoch = 0;
			UInt32 resourceSerial = 0;
		};

		struct NativeFontCandidateHotEntry
		{
			const NativeFontPayloadTemplate* key = nullptr;
			std::weak_ptr<const NativeFontPayloadTemplate> owner;
			std::shared_ptr<NativeFontStaticCandidate> candidate;
			UInt32 resourceSerial = 0;
		};

		struct NativeFontRingThreadState
		{
			UInt32 preferredProxy = std::numeric_limits<UInt32>::max();
			NativeFontStaticHotEntry staticPayload;
			NativeFontUploadHotEntry uploadedPayload;
			NativeFontCandidateHotEntry staticCandidate;
		};
		struct NativeFontRingState
		{
			std::mutex mutex;
			std::array<NativeFontProxy, kProxyPoolSize> proxies;
			UInt32 proxyCount = 0;
			NiDX9Renderer* renderer = nullptr;
			IDirect3DDevice9* device = nullptr;
			IDirect3DVertexBuffer9* vertexBuffer = nullptr;
			IDirect3DVertexBuffer9* staticVertexBuffer = nullptr;
			IDirect3DIndexBuffer9* indexBuffer = nullptr;
			IDirect3DVertexDeclaration9* declaration = nullptr;
			UInt32 generation = 0;
			UInt32 vertexCapacity = 0;
			UInt32 nextVertex = 0;
			UInt32 staticVertexCapacity = 0;
			UInt32 nextStaticVertex = 0;
			UInt32 uploadEpoch = 1;
			UInt32 dynamicWriteSerial = 0;
			UInt32 dynamicDiscardSerial = 0;
			UInt32 staticWriteSerial = 0;
			std::unordered_map<const NativeFontPayloadTemplate*,
				NativeFontUploadedPayload> uploadedPayloads;
			std::unordered_map<const NativeFontPayloadTemplate*,
				NativeFontStaticPayload> staticPayloads;
			std::unordered_map<const NativeFontPayloadTemplate*,
				std::shared_ptr<NativeFontStaticCandidate>> staticCandidates;
			CpuMemoryLease cpuMemory;
			std::atomic<UInt32> resourceSerial = 1;
			// Proxy shapes live for the process lifetime.  Once the complete pool is
			// published, registration no longer needs the ring mutex; device reset
			// only clears/rebinds their borrowed GPU resources.
			std::atomic<bool> proxyPoolReady = false;
			std::atomic<UInt32> sortedFrameLeases = 0;
			std::atomic<UInt32> activeSubmissions = 0;
			std::atomic<bool> releasePending = false;
			UInt32 lastStaticCompactionFrame = 0;
			UInt32 lastStaticCompactionDeferredLogFrame = 0;
			UInt32 lastStaticCandidateSweepFrame = 0;
			UInt32 staticPromotionBudgetFrame = 0;
			UInt32 staticPromotionGlobalRetryFrame = 0;
			UInt32 staticPromotionBytesThisFrame = 0;
			UInt32 staticPromotionPayloadsThisFrame = 0;
			bool staticCompactionFrameValid = false;
			bool staticCompactionDeferredLogFrameValid = false;
			bool staticCandidateSweepFrameValid = false;
			bool staticPromotionBudgetFrameValid = false;
			bool loggedReady = false;
		};

	struct NativeFontSortedRingLease
		{
			NativeFontRingState* state = nullptr;
			IDirect3DVertexBuffer9* dynamicVertexBuffer = nullptr;
			IDirect3DVertexBuffer9* staticVertexBuffer = nullptr;
			IDirect3DIndexBuffer9* indexBuffer = nullptr;
			IDirect3DVertexDeclaration9* declaration = nullptr;
			UInt32 generation = 0;
			UInt32 resourceSerial = 0;
			UInt32 uploadEpoch = 0;
			bool active = false;
		};

	enum class StaticPromotionReadiness : UInt8
	{
		Ready = 0,
		Disabled,
		Lifecycle,
		UploadHistory,
		Retry,
	};

	template <class Map>
	size_t EstimateUnorderedMapBytes(const Map& map)
	{
		return map.bucket_count() * sizeof(void*)
			+ map.size() * (sizeof(typename Map::value_type)
				+ 3u * sizeof(void*));
	}

	NativeFontRingState& RingState();
	NativeFontRingThreadState& RingThread();
	NativeFontSortedRingLease& SortedRingLease();

	UInt64 HashDiagnosticBytes(const void* data, size_t size);
	UInt64 HashDiagnosticPayload(
		const NativeFontPayloadTemplate& payloadTemplate);
	UInt32 AdvanceDiagnosticSerial(UInt32& serial);
	void RefreshRingCpuMemoryLocked(NativeFontRingState& state);
	std::shared_ptr<NativeFontStaticCandidate> CreateStaticCandidate(
		const NativeFontPayloadTemplatePtr& payloadTemplate);
	TileShaderPropertyView* GetTileProperty(NiTriShape* shape);
	const TileShaderPropertyView* GetTileProperty(const NiTriShape* shape);
	bool InstallProxyVertexColors(NiTriShapeData& data);
	bool AttachProxyBuffer(NativeFontProxy& proxy);
	UInt32 AdvanceResourceSerialLocked(NativeFontRingState& state);
	void AdvanceUploadEpochLocked(NativeFontRingState& state);
	void ReleaseRingResourcesLocked(NativeFontRingState& state);
	bool EnsureRingResourcesLocked(NativeFontRingState& state,
		UInt32 preparedGeneration, UInt32 requiredVertices,
		const char*& operation, HRESULT& result);
	bool BindPacketAtlasPage(NativeFontProxy& proxy,
		const NativeFontPayloadTemplate& artifact, UInt16 page);
	bool SyncProxyState(const NiTriShape& facade, NativeFontProxy& proxyState,
		const NiPoint3& geometryOrigin);
	UInt32 AcquireProxyLocked(NativeFontRingState& state,
		NativeFontRingThreadState& thread);
	UInt32 GetStaticObservationFrame(const NativeFontRingState& state);
	StaticPromotionReadiness GetStaticPromotionReadiness(
		const NativeFontRingState& state,
		const NativeFontStaticCandidate& candidate, UInt32 vertexCount,
		UInt32 frame, UInt32 maturityMultiplier = 1);
	bool FitsStaticPromotionBudget(NativeFontRingState& state,
		const NativeFontStaticCandidate& candidate, UInt32 vertexCount,
		UInt32 pendingBytes, UInt32 pendingPayloads);
	void CommitStaticPromotionBudget(NativeFontRingState& state,
		UInt32 bytes, UInt32 payloads);
	void DeferStaticPromotionsLocked(NativeFontRingState& state,
		UInt32 frame);
	bool TryGrowStaticVertexBufferLocked(NativeFontRingState& state,
		UInt32 requiredVertices, bool& permanentFailure);
	void RecordStaticPromotionDeferral(
		StaticPromotionReadiness readiness, UInt64 amount = 1);
	void MarkStaticPayloadUsedLocked(NativeFontRingState& state,
		const NativeFontPayloadTemplatePtr& payloadTemplate);
	bool ResolveStaticPayloadLocked(NativeFontRingState& state,
		const NativeFontPayloadTemplatePtr& payloadTemplate,
		UInt32 vertexCount, UInt32& baseVertex);
	bool IsStaticPayloadCurrentLocked(const NativeFontRingState& state,
		const NativeFontPayloadTemplatePtr& payloadTemplate,
		UInt32 vertexCount);
	NativeFontStaticCandidate* ResolveStaticCandidateLocked(
		NativeFontRingState& state,
		const NativeFontPayloadTemplatePtr& payloadTemplate,
		UInt32 vertexCount, bool allowCreate);
	NativeFontStaticCandidate* ObserveStaticCandidateLocked(
		NativeFontRingState& state,
		const NativeFontPayloadTemplatePtr& payloadTemplate,
		UInt32 vertexCount, bool allowCreate);
	void NoteStaticCandidateDynamicUploadLocked(NativeFontRingState& state,
		const NativeFontPayloadTemplatePtr& payloadTemplate,
		UInt32 vertexCount);
	bool PromoteStaticPayloadLocked(NativeFontRingState& state,
		const NativeFontPayloadTemplatePtr& payloadTemplate,
		UInt32 vertexCount, UInt32& baseVertex);
	bool ResolveUploadedPayloadLocked(NativeFontRingState& state,
		const NativeFontPayloadTemplatePtr& payloadTemplate,
		UInt32 vertexCount, UInt32& baseVertex);
	bool HasDirectStaticPayloadLocked(const NativeFontRingState& state,
		const NativeFontPayloadTemplate& payloadTemplate,
		UInt32 vertexCount);
	bool HasDirectUploadedPayloadLocked(const NativeFontRingState& state,
		const NativeFontPayloadTemplate& payloadTemplate,
		UInt32 vertexCount);
	void PublishUploadedPayloadLocked(NativeFontRingState& state,
		const NativeFontPayloadTemplatePtr& payloadTemplate,
		UInt32 baseVertex, UInt32 vertexCount);
	bool ResolveSortedLeaseResidency(
		const NativeFontRingState& state,
		const NativeFontPayloadTemplate& artifact, UInt32 vertexCount,
		UInt32 resourceSerial, UInt32 uploadEpoch,
		UInt32& baseVertex, bool& staticResident);
	bool PublishSortedRingLeaseLocked(NativeFontRingState& state,
		const std::vector<NativeFontPayloadTemplatePtr>& payloadTemplates,
		UInt32 generation, bool residencyAlreadyValidated = false);
	bool TryBeginSortedRingSubmission(NiTriShape* facade,
		NativeFontShapePayload& payload,
		NativeFontRingSubmission& submission,
		NativeFontFallbackReason& result);
}

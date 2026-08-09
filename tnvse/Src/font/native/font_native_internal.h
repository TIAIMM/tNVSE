#pragma once

// Private Gamebryo-native FreeType rendering model.

#include "font_vector_internal.h"

#include "BSShaderAccumulator.hpp"
#include "NiAlphaProperty.hpp"
#include "NiTriShape.hpp"
#include "TileShader.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <limits>
#include <memory>
#include <type_traits>
#include <utility>
#include <vector>

class NiGeometryBufferData;
class NiVBChip;
class NiDX9Renderer;
class NiD3DRenderState;
class NiRenderTargetGroup;
class NiPropertyState;

namespace fonthook::vectorfont
{
	struct NativeFontShapeMetadata;
	struct NativeFontCompiledPacketCommand;
	inline constexpr UInt32 kNativeFontMaximumQuads =
		std::numeric_limits<UInt16>::max() / 4u;

	template <class T, size_t InlineCapacity = 1>
	class NativeFontInlineVector
	{
	public:
		using value_type = T;
		using iterator = T*;
		using const_iterator = const T*;

		bool empty() const { return size() == 0; }
		size_t size() const
		{
			return m_heapMode ? m_heap.size() : m_inlineSize;
		}
		size_t capacity() const
		{
			return m_heapMode ? m_heap.capacity() : InlineCapacity;
		}
		size_t heap_capacity() const
		{
			return m_heapMode ? m_heap.capacity() : 0;
		}
		bool uses_inline_storage() const { return !m_heapMode; }
		void force_heap_storage()
		{
			if (!m_heapMode)
				Promote(m_inlineSize);
		}

		T* data()
		{
			return m_heapMode ? m_heap.data() : m_inline.data();
		}
		const T* data() const
		{
			return m_heapMode ? m_heap.data() : m_inline.data();
		}
		iterator begin() { return data(); }
		const_iterator begin() const { return data(); }
		iterator end() { return data() + size(); }
		const_iterator end() const { return data() + size(); }

		T& operator[](size_t index) { return data()[index]; }
		const T& operator[](size_t index) const { return data()[index]; }
		T& front() { return data()[0]; }
		const T& front() const { return data()[0]; }
		T& back() { return data()[size() - 1u]; }
		const T& back() const { return data()[size() - 1u]; }

		void clear()
		{
			if (m_heapMode)
				m_heap.clear();
			else
				m_inlineSize = 0;
		}

		void reserve(size_t requested)
		{
			if (requested <= InlineCapacity && !m_heapMode)
				return;
			Promote(requested);
		}

		void assign(size_t count, const T& value)
		{
			if (m_heapMode || count > InlineCapacity)
			{
				Promote(count);
				m_heap.assign(count, value);
				return;
			}
			for (size_t index = 0; index < count; ++index)
				m_inline[index] = value;
			m_inlineSize = count;
		}

		void push_back(const T& value)
		{
			if (!m_heapMode && m_inlineSize < InlineCapacity)
			{
				m_inline[m_inlineSize++] = value;
				return;
			}
			Promote(size() + 1u);
			m_heap.push_back(value);
		}

		void push_back(T&& value)
		{
			if (!m_heapMode && m_inlineSize < InlineCapacity)
			{
				m_inline[m_inlineSize++] = std::move(value);
				return;
			}
			Promote(size() + 1u);
			m_heap.push_back(std::move(value));
		}

		template <class... Args>
		T& emplace_back(Args&&... args)
		{
			if (!m_heapMode && m_inlineSize < InlineCapacity)
			{
				m_inline[m_inlineSize] = T(std::forward<Args>(args)...);
				return m_inline[m_inlineSize++];
			}
			Promote(size() + 1u);
			m_heap.emplace_back(std::forward<Args>(args)...);
			return m_heap.back();
		}

	private:
		void Promote(size_t requested)
		{
			if (m_heapMode)
			{
				if (m_heap.capacity() < requested)
					m_heap.reserve(requested);
				return;
			}
			m_heap.reserve(std::max(requested, m_inlineSize));
			for (size_t index = 0; index < m_inlineSize; ++index)
				m_heap.push_back(std::move(m_inline[index]));
			m_inlineSize = 0;
			m_heapMode = true;
		}

		std::array<T, InlineCapacity> m_inline = {};
		std::vector<T> m_heap;
		size_t m_inlineSize = 0;
		bool m_heapMode = false;
	};
	inline constexpr size_t kNativeFontPacketConstantRegisterCount = 8;
	inline constexpr size_t kNativeFontPacketConstantFloatCount =
		kNativeFontPacketConstantRegisterCount * 4;
	// Keep every tNVSE-owned float constant above the complete shipped and
	// audited New Vegas plugin shader footprint, while retaining guard space
	// below the SM3 register-file boundary. Pixel c0 remains the vanilla Tile
	// color; vertex c0-c4 remain the vanilla WVP/TexScroll range.
	inline constexpr UInt32 kNativeFontPixelConstantBaseRegister = 176;
	inline constexpr UInt32 kNativeFontPixelConstantLastRegister =
		kNativeFontPixelConstantBaseRegister
		+ static_cast<UInt32>(kNativeFontPacketConstantRegisterCount) - 1u;
	inline constexpr UInt32 kNativeFontVertexAaConstantRegister = 208;
	inline constexpr UInt32 kNativeFontVanillaLayoutGlyphConstantRegister = 209;
	static_assert(kNativeFontPixelConstantLastRegister <= 223);
	static_assert(kNativeFontVertexAaConstantRegister <= 255);
	static_assert(kNativeFontVanillaLayoutGlyphConstantRegister <= 255);
	static_assert(kNativeFontVanillaLayoutGlyphConstantRegister
		== kNativeFontVertexAaConstantRegister + 1u);

	enum class NativeFontShaderClass : UInt8
	{
		Body,
		Effect,
		Composite,
		Coverage,
		Argb
	};

	enum class NativeFontSampling : UInt8
	{
		Point,
		LinearMipmapped,
		LinearLod0
	};

	enum class NativeFontFallbackReason : UInt8
	{
		None,
		ShaderGeneration,
		PacketBuild,
		PacketPrepare,
		AtlasGeneration,
		PageTexture,
		PropertySync,
		AccumulatorConflict,
		TileRouteConflict,
		DirectImmediate,
		DeviceReset,
		RuntimeFault
	};

	enum class NativeFontPacketPrepareFailure : UInt8
	{
		None,
		Generation,
		Geometry,
		ShaderBinding,
		Declaration,
		ProxyUnavailable,
		RingCapacity,
		IndexBuffer,
		VertexBuffer
	};

	struct NativeFontGpuVertex
	{
		// Direct compilers allocate their complete random-access output before
		// filling page/layer ranges. Default construction therefore deliberately
		// leaves this payload untouched; publication is allowed only after the
		// compiler proves that every allocated quad range was written completely.
		// Other call sites must use the full-value constructor or copy an already
		// initialized vertex.
		NativeFontGpuVertex() noexcept {}

		NativeFontGpuVertex(float xValue, float yValue, float zValue,
			float uValue, float vValue, UInt32 colorValue,
			float sdfSpreadValue, float distanceParameterScaleValue,
			float layerMaskValue, float glyphU0Value, float glyphV0Value,
			float glyphU1Value, float glyphV1Value) noexcept
			: x(xValue), y(yValue), z(zValue), u(uValue), v(vValue),
			  color(colorValue), sdfSpread(sdfSpreadValue),
			  distanceParameterScale(distanceParameterScaleValue),
			  layerMask(layerMaskValue), glyphU0(glyphU0Value),
			  glyphV0(glyphV0Value), glyphU1(glyphU1Value),
			  glyphV1(glyphV1Value)
		{
		}

		float x;
		float y;
		float z;
		float u;
		float v;
		// D3DDECLTYPE_D3DCOLOR expands this packed ARGB value to the shader's
		// normalized float4 COLOR0 input. Distance-field profiles retain their
		// per-packet layer color in c1; baked coverage instead places the complete
		// base/layer modifier here so different effects can share one packet.
		UInt32 color;
		// Per-glyph distance-field data must not participate in packet identity.
		// Shared MTSDF double-byte atlases can mix source sizes in one text run;
		// carrying these values in TEXCOORD1 keeps those glyphs in the same
		// layer/page packet without changing their reconstruction parameters.
		float sdfSpread;
		float distanceParameterScale;
		// Exact integer mask (bits 0..3: Shadow/Glow/Outline/Fill) for distance
		// fields. Baked A8 coverage profiles instead store their per-quad live
		// Tile RGB selector here (0=fixed RGB, 1=live Tile RGB).
		float layerMask;
		// Exact physical glyph rectangle. The composite quad can extrapolate its
		// UVs to cover an offset shadow; the pixel shader bounds every sample to
		// this rectangle so atlas neighbours never bleed into that union.
		// Use an ordinary FLOAT4 declaration for compatibility with native D3D9
		// drivers and wrappers that reject the optional USHORT4N declaration
		// type. The HLSL input remains an exact normalized atlas rectangle.
		float glyphU0;
		float glyphV0;
		float glyphU1;
		float glyphV1;
	};
	static_assert(std::is_standard_layout_v<NativeFontGpuVertex>);
	static_assert(std::is_trivially_copyable_v<NativeFontGpuVertex>);
	static_assert(offsetof(NativeFontGpuVertex, u) == 3 * sizeof(float));
	static_assert(offsetof(NativeFontGpuVertex, color) == 5 * sizeof(float));
	static_assert(offsetof(NativeFontGpuVertex, sdfSpread) == 6 * sizeof(float));
	static_assert(offsetof(NativeFontGpuVertex, glyphU0) == 9 * sizeof(float));
	static_assert(sizeof(NativeFontGpuVertex) == 13 * sizeof(float));

	// Exact interleaved stream consumed by freetype_native_vanilla_layout_vs.
	// The retail declaration packer cannot source TEXCOORD1/2 independently:
	// NiGeometryData::GetTextureSet ignores the requested set.  This stream is
	// therefore written directly only after the retail renderer has completed
	// its queued native pack and retired the incompatible CPU UV source.
	struct NativeFontVanillaLayoutVertex
	{
		float x = 0.0f;
		float y = 0.0f;
		float z = 0.0f;
		float u = 0.0f;
		float v = 0.0f;
		UInt32 color = 0xFFFFFFFFu;
		float glyphU0 = 0.0f;
		float glyphV0 = 0.0f;
		float glyphU1 = 0.0f;
		float glyphV1 = 0.0f;
	};
	static_assert(offsetof(NativeFontVanillaLayoutVertex, u)
		== 3 * sizeof(float));
	static_assert(offsetof(NativeFontVanillaLayoutVertex, color)
		== 5 * sizeof(float));
	static_assert(offsetof(NativeFontVanillaLayoutVertex, glyphU0)
		== 6 * sizeof(float));
	static_assert(sizeof(NativeFontVanillaLayoutVertex) == 40u);

	// Extended stream used only when one single-page composite packet needs
	// per-glyph reconstruction parameters. Every texture semantic remains FLOAT2
	// so the retail declaration packer never reads beyond its NiPoint2 UV source;
	// the certified post-pack upload replaces all placeholder values atomically.
	struct NativeFontVanillaParametricVertex
	{
		float x = 0.0f;
		float y = 0.0f;
		float z = 0.0f;
		float u = 0.0f;
		float v = 0.0f;
		UInt32 color = 0xFFFFFFFFu;
		float sdfSpread = 0.0f;
		float distanceParameterScale = 1.0f;
		float glyphU0 = 0.0f;
		float glyphV0 = 0.0f;
		float glyphU1 = 0.0f;
		float glyphV1 = 0.0f;
	};
	static_assert(offsetof(NativeFontVanillaParametricVertex, u)
		== 3 * sizeof(float));
	static_assert(offsetof(NativeFontVanillaParametricVertex, color)
		== 5 * sizeof(float));
	static_assert(offsetof(NativeFontVanillaParametricVertex, sdfSpread)
		== 6 * sizeof(float));
	static_assert(offsetof(NativeFontVanillaParametricVertex, glyphU0)
		== 8 * sizeof(float));
	static_assert(sizeof(NativeFontVanillaParametricVertex) == 48u);

	struct NativeFontPacketShaderCacheEntry
	{
		// Native shader profiles are process-lifetime objects. Keep this opaque in
		// the shared packet model; the shader implementation validates generation
		// and sampling before using the cached profile.
		std::atomic<void*> profile{ nullptr };

		NativeFontPacketShaderCacheEntry() = default;
		NativeFontPacketShaderCacheEntry(
			const NativeFontPacketShaderCacheEntry&) noexcept
		{
		}
		NativeFontPacketShaderCacheEntry(
			NativeFontPacketShaderCacheEntry&&) noexcept
		{
		}
		NativeFontPacketShaderCacheEntry& operator=(
			const NativeFontPacketShaderCacheEntry&) noexcept
		{
			profile.store(nullptr, std::memory_order_relaxed);
			return *this;
		}
		NativeFontPacketShaderCacheEntry& operator=(
			NativeFontPacketShaderCacheEntry&&) noexcept
		{
			profile.store(nullptr, std::memory_order_relaxed);
			return *this;
		}
	};

	struct NativeFontPacketTemplate
	{
		UInt32 firstVertex = 0;
		UInt32 vertexCount = 0;
		NiBound bound;
		std::array<float, kNativeFontPacketConstantFloatCount> constants = {};
		NativeFontShaderClass shaderClass = NativeFontShaderClass::Body;
		NativeFontSampling sampling = NativeFontSampling::Point;
		EffectQuality quality = EffectQuality::Balanced;
		DistanceFieldMethod distanceFieldMethod =
			GetConfiguredDistanceFieldMethod();
		UInt32 layer = 3;
		UInt16 atlasPage = 0;
		// Zero keeps the generic per-glyph mask shader. A non-zero value proves
		// that every quad in this composite packet carries the same exact mask
		// and permits an immutable distance-field pixel-shader profile.
		UInt8 staticCompositeLayerMask = 0;
		bool compositeShiftedShadow = false;
		bool staticSmoothSampling = false;
		bool usesLiveTileRgb = true;
		// Packet-wide reconstruction values used by the compact 40-byte target.
		// Zero means the value is not uniform; the 48-byte parameterized target may
		// still carry the corresponding positive value per vertex.
		float uniformSdfSpread = 0.0f;
		float uniformDistanceParameterScale = 0.0f;
		// Profile hashes are immutable artifact data. The two slots correspond to
		// separate-alpha disabled/enabled. Resolved profiles are validated against
		// the current generation and reset automatically on packet copy or move.
		std::array<size_t, 2> profileHashes = {};
		mutable std::array<NativeFontPacketShaderCacheEntry, 2>
			resolvedShaders;
		// Vanilla-layout and native-facade profiles intentionally have distinct
		// keys and shaders. Keep a second packet-local cache so the retail-layout
		// hot path does not reload the generation profile map on every draw.
		mutable std::array<NativeFontPacketShaderCacheEntry, 2>
			vanillaLayoutResolvedShaders;
	};

	struct NativeFontCompositeSpan
	{
		UInt32 firstVertex = 0;
		UInt32 vertexCount = 0;
		UInt16 atlasPage = 0;
		bool fused = false;
	};

	struct NativeFontCompiledPacketCommand;

	// Renderer-owned VB locations are mutable cache data attached to the immutable
	// text artifact. Every read is protected by the ring mutex and validated
	// against the current resource serial/epoch before the location is used.
	struct NativeFontPayloadResidencyCache
	{
		UInt32 staticResourceSerial = 0;
		UInt32 staticBaseVertex = 0;
		UInt32 staticVertexCount = 0;
		UInt32 staticLastUsedFrame = 0;
		UInt32 dynamicResourceSerial = 0;
		UInt32 dynamicUploadEpoch = 0;
		UInt32 dynamicBaseVertex = 0;
		UInt32 dynamicVertexCount = 0;
	};

	// BuildNativeFontPayloadTemplate publishes the text artifact through a
	// shared_ptr<const>.  Once published, every field below is immutable except
	// for the separately synchronized residency cache.  Record the complete
	// structural validation once at construction so registering another live
	// Tile does not rescan every glyph vertex and packet.
	struct NativeFontPayloadValidationSeal
	{
		static constexpr UInt32 kAbi = 3;

		UInt32 abi = 0;
		UInt32 pageCount = 0;
		UInt32 quadCount = 0;
		UInt32 sourceRangeCount = 0;
		UInt32 glyphCount = 0;
		UInt32 colorContractAbi = 0;
		UInt32 vertexCount = 0;
		UInt32 packetCount = 0;
		UInt32 compositePacketCount = 0;
		bool vanillaLikeBitmapPackets = false;
		NativeFontVanillaLayoutKind vanillaLayoutKind =
			NativeFontVanillaLayoutKind::None;
	};

	struct NativeFontPayloadTemplate
	{
		CpuMemoryLease cpuMemory;
		UInt32 pageCount = 0;
		UInt32 quadCount = 0;
		UInt32 sourceRangeCount = 0;
		UInt32 glyphCount = 0;
		NativeFontShapeColorContract colorContract;
		NiBound bound;
		std::vector<NiTexturingPropertyPtr> atlasProperties;
		std::vector<NiTexturePtr> atlasTextures;
		std::vector<NativeFontGpuVertex> gpuVertices;
		std::vector<NativeFontPacketTemplate> packets;
		// Optional single-pass distance-field representation. The ordinary packet
		// list remains available if that shader class is unavailable.
		std::vector<NativeFontPacketTemplate> compositePackets;
		mutable NativeFontPayloadResidencyCache residency;
		// Tail-only construction certificate.  Count copies make the read path
		// fail closed if an accidental internal mutation ever invalidates the
		// otherwise immutable artifact.
		NativeFontPayloadValidationSeal validationSeal;
	};
	using NativeFontPayloadTemplatePtr =
		std::shared_ptr<const NativeFontPayloadTemplate>;

	inline bool HasNativeFontPayloadValidationSeal(
		const NativeFontPayloadTemplate& payloadTemplate)
	{
		const NativeFontPayloadValidationSeal& seal =
			payloadTemplate.validationSeal;
		const bool validVanillaLayoutKind =
			static_cast<UInt8>(seal.vanillaLayoutKind)
				<= static_cast<UInt8>(
					NativeFontVanillaLayoutKind::Parametric48);
		return seal.abi == NativeFontPayloadValidationSeal::kAbi
			&& validVanillaLayoutKind
			&& seal.pageCount != 0
			&& seal.pageCount == payloadTemplate.pageCount
			&& seal.quadCount != 0
			&& seal.quadCount == payloadTemplate.quadCount
			&& seal.sourceRangeCount != 0
			&& seal.sourceRangeCount == payloadTemplate.sourceRangeCount
			&& seal.glyphCount == payloadTemplate.glyphCount
			&& seal.colorContractAbi
				== NativeFontShapeColorContract::kTileUniformColorAbi
			&& seal.colorContractAbi
				== payloadTemplate.colorContract.abiVersion
			&& seal.vertexCount == payloadTemplate.gpuVertices.size()
			&& seal.packetCount != 0
			&& seal.packetCount == payloadTemplate.packets.size()
			&& seal.compositePacketCount
				== payloadTemplate.compositePackets.size()
			&& (!UsesNativeFontVanillaLayout(seal.vanillaLayoutKind)
				|| (seal.pageCount == 1
					&& seal.compositePacketCount == 1));
	}

	// The Standard-lite call program is resolved once for a live Tile and shader
	// generation. It deliberately excludes VB/IB/declaration residency: ordinary
	// single-packet facades may attach a short-lived synthetic buffer while the
	// Tile, property state, shader program, and renderer remain stable.
	struct NativeFontStandardPassLiteDispatch
	{
		NiTriShape* geometry = nullptr;
		const NiPropertyState* properties = nullptr;
		NiDX9Renderer* renderer = nullptr;
		TileShader* shader = nullptr;
		const NativeFontCompiledPacketCommand* program = nullptr;
		UInt32 generation = 0;
		bool standardV2Ready = false;
		bool ready = false;
	};

	// The Text Artifact owns immutable geometry and packet data. The resolved
	// replay program belongs to one live Tile facade instead: shader/profile
	// selection depends on that Tile's alpha and sampling class. These packet
	// skeletons retain no renderer state or D3D COM ownership; traversal-local
	// commands add the current atlas and VB/IB residency after sorted preflight.
	struct NativeFontTileRetainedPacket
	{
		const NativeFontPacketTemplate* packet = nullptr;
		const NativeFontCompiledPacketCommand* program = nullptr;
		UInt32 packetIndex = 0;
		UInt32 firstVertex = 0;
		UInt32 vertexCount = 0;
		UInt16 atlasPage = 0;
	};

	struct NativeFontTileRetainedRun
	{
		UInt32 firstPacket = 0;
		UInt32 packetCount = 0;
		bool bridgeEligible = false;
		bool continuesBridgeSpan = false;
	};

	struct NativeFontTileRetainedText
	{
		NiTriShape* ownerTile = nullptr;
		const NativeFontPayloadTemplate* artifact = nullptr;
		NativeFontInlineVector<NativeFontTileRetainedPacket> packets;
		NativeFontInlineVector<NativeFontTileRetainedRun> runs;
		// Standard-lite is currently a dedicated single-packet specialization,
		// so one Tile-lifetime dispatch is sufficient for this retained text.
		NativeFontStandardPassLiteDispatch standardPassLite;
		UInt32 generation = 0;
		UInt32 atlasTextureEpoch = 0;
		bool useCompositePackets = false;
		bool bridgeEligible = false;
		bool ready = false;
	};

	struct NativeFontShapePayload
	{
		NativeFontPayloadTemplatePtr payloadTemplate;
		NiPoint3 geometryOrigin;
		// Packet geometry, constants, page identity, and profile keys live in the
		// shared text artifact. The Tile instance retains its generation-bound
		// shader/program skeleton until that Tile is destroyed or preflight is
		// invalidated.
		NativeFontInlineVector<TileShader*> packetShaders;
		NativeFontInlineVector<const NativeFontCompiledPacketCommand*> packetPrograms;
		NativeFontTileRetainedText retainedText;
		std::atomic<bool> suppressNextSubmit = false;
		std::atomic<NativeFontFallbackReason> stickyReason =
			NativeFontFallbackReason::None;
		std::atomic<NativeFontPacketPrepareFailure> packetPrepareFailure =
			NativeFontPacketPrepareFailure::None;
		// A successful preflight is reusable while the shader generation, scaled
		// fill sampling class, and the referenced page textures remain unchanged.
		// Null entries belong to atlas pages that no packet in this payload uses.
		NativeFontInlineVector<const void*> preflightAtlasTextures;
		UInt32 preparedGeneration = 0;
		UInt32 compositeAttemptGeneration = 0;
		UInt32 preflightAtlasTextureEpoch = 0;
		bool preflightScaledFillSampling = false;
		bool preflightAlphaBlending = false;
		bool useCompositePackets = false;
		bool topologyObserved = false;
		bool lastTopologyComposite = false;
		bool compositeUnavailable = false;
		bool vanillaLikeBitmapPackets = false;
		bool buildComplete = false;
	};

	// Render-thread-confined proof that one vanilla-layout shape has already passed
	// the complete native readiness audit.  Every pointer is a non-owning identity
	// witness only: the native-font metadata owns this token, shader generations retain
	// their sidecars for process lifetime, and no D3D/COM reference is acquired.
	// A draw may reuse the proof only while every recorded identity and scalar
	// layout field still matches exactly.
	struct NativeFontVanillaLayoutDrawToken
	{
		const NiTriShape* shapeIdentity = nullptr;
		TileShader* shaderIdentity = nullptr;
		const void* shaderVtableIdentity = nullptr;
		const void* profileIdentity = nullptr;
		const void* generationIdentity = nullptr;
		const void* generationDeclarationIdentity = nullptr;
		const void* modelDataIdentity = nullptr;
		const void* bufferIdentity = nullptr;
		const void* bufferDeclarationIdentity = nullptr;
		const void* geometryGroupIdentity = nullptr;
		const void* strideArrayIdentity = nullptr;
		const void* vertexChipIdentity = nullptr;
		const void* vertexBufferIdentity = nullptr;
		const void* indexBufferIdentity = nullptr;
		const void* arrayLengthsIdentity = nullptr;
		const void* indexArrayIdentity = nullptr;
		const void* payloadIdentity = nullptr;
		const void* artifactIdentity = nullptr;
		const void* packetIdentity = nullptr;
		const NativeFontCompiledPacketCommand* standardLiteProgramIdentity =
			nullptr;
		UInt32 generation = 0;
		UInt32 deviceEpoch = 0;
		UInt32 bufferFlags = 0;
		UInt32 streamCount = 0;
		UInt32 stride = 0;
		UInt32 bufferVertexCount = 0;
		UInt32 dataVertexCount = 0;
		UInt32 baseVertexIndex = 0;
		UInt32 vertexChipOffset = 0;
		UInt32 vertexChipSize = 0;
		UInt32 indexCount = 0;
		UInt32 indexBufferSize = 0;
		UInt32 arrayCount = 0;
		UInt32 uploadedByteOffset = 0;
		UInt32 uploadedByteCount = 0;
		UInt16 nativePackDataFlags = 0;
		UInt16 nativePackDirtyFlags = 0;
		UInt8 nativePackKeepFlags = 0;
		NativeFontVanillaLayoutKind layoutKind =
			NativeFontVanillaLayoutKind::None;
		bool nativePackCompleted = false;
		bool priorGenerationDeclaration = false;
		bool payloadUploaded = false;
		bool everCertified = false;
		bool valid = false;

		void Invalidate()
		{
			valid = false;
			payloadUploaded = false;
			nativePackCompleted = false;
		}
	};

	inline const std::vector<NativeFontPacketTemplate>& GetNativeFontPackets(
		const NativeFontPayloadTemplate& payloadTemplate, bool useComposite)
	{
		return useComposite && !payloadTemplate.compositePackets.empty()
			? payloadTemplate.compositePackets : payloadTemplate.packets;
	}

	inline bool UsesOnlyVanillaLikeBitmapPackets(
		const std::vector<NativeFontPacketTemplate>& packets)
	{
		if (packets.empty())
			return false;
		for (const NativeFontPacketTemplate& packet : packets)
		{
			if (packet.shaderClass != NativeFontShaderClass::Argb
				&& packet.shaderClass != NativeFontShaderClass::Coverage)
			{
				return false;
			}
		}
		return true;
	}

	enum class NativeFontVisibilityCull : UInt8
	{
		None = 0,
		AppCulled,
		ZeroAlpha,
		Clip,
		Scissor
	};

	enum class NativeFontVisibilityProofStatus : UInt8
	{
		Unproven = 0,
		Overlap,
		Outside
	};

	struct NativeFontVisibilityProofWitness
	{
		NativeFontVisibilityProofWitness() noexcept
			: valid(false)
		{
		}

		NativeFontVisibilityProofWitness(
			const NativeFontVisibilityProofWitness& other) noexcept
			: valid(false)
		{
			CopyPublishedFrom(other);
		}

		NativeFontVisibilityProofWitness(
			NativeFontVisibilityProofWitness&& other) noexcept
			: valid(false)
		{
			CopyPublishedFrom(other);
		}

		NativeFontVisibilityProofWitness& operator=(
			const NativeFontVisibilityProofWitness& other) noexcept
		{
			if (this != &other)
				CopyPublishedFrom(other);
			return *this;
		}

		NativeFontVisibilityProofWitness& operator=(
			NativeFontVisibilityProofWitness&& other) noexcept
		{
			if (this != &other)
				CopyPublishedFrom(other);
			return *this;
		}

		// Exact source values that authorized the frame-local clip/scissor proof.
		// Dispatch compares these values directly instead of depending on the
		// bounded cross-frame proof cache still retaining this facade's entry.
		// Payload members intentionally remain indeterminate until valid becomes
		// true. Constructors and assignments copy them only from a published source;
		// this prevents container moves or return-value copies from reading an
		// unpublished object while avoiding a full clear for every frame entry.
		const NiDX9Renderer* renderer;
		std::array<UInt32, 13> transformBits;
		std::array<UInt32, 4> boundBits;
		RECT tileScissorRect;
		UInt64 cameraEpoch;
		NativeFontVisibilityProofStatus status;
		NativeFontVisibilityCull cullReason;
		bool tileUsesScissor;
		bool valid;

	private:
		void CopyPublishedFrom(
			const NativeFontVisibilityProofWitness& other) noexcept
		{
			valid = false;
			if (!other.valid)
				return;
			renderer = other.renderer;
			transformBits = other.transformBits;
			boundBits = other.boundBits;
			tileScissorRect = other.tileScissorRect;
			cameraEpoch = other.cameraEpoch;
			status = other.status;
			cullReason = other.cullReason;
			tileUsesScissor = other.tileUsesScissor;
			valid = true;
		}
	};
	static_assert(sizeof(NativeFontVisibilityProofWitness) == 0x68);
	static_assert(offsetof(NativeFontVisibilityProofWitness, valid) == 0x63);

	struct NativeFontVisibilityPreflight
	{
		UInt64 frameToken = 0;
		NativeFontVisibilityProofStatus status =
			NativeFontVisibilityProofStatus::Unproven;
		NativeFontVisibilityCull cull = NativeFontVisibilityCull::None;
		NativeFontVisibilityProofWitness proofWitness;
	};

	struct NativeFontSortedFrameEntryView
	{
		const NativeFontShapeMetadata* metadata = nullptr;
		NativeFontShapePayload* payload = nullptr;
		NativeFontFallbackReason preflightResult =
			NativeFontFallbackReason::RuntimeFault;
		// Points into the active SortedFrameEntry. Keeping this as a view avoids
		// copying the complete proof witness on every immediate dispatch.
		const NativeFontVisibilityPreflight* visibility = nullptr;
		UInt32 generation = 0;
		UInt64 validationToken = 0;
		UInt32 commandSpanIndex = std::numeric_limits<UInt32>::max();
		UInt32 singlePacketCommandIndex =
			std::numeric_limits<UInt32>::max();
		// Set only when retail B64F90's live m_iCurrItem, sorted slot and
		// captured frame entry all agree while B64FD1 still calls tNVSE
		// directly. Hash-fallback lookups intentionally leave this false.
		SInt32 retailSortedItemIndex = -1;
		UInt64 nestedTraversalSerial = 0;
		bool retailSortedItemMatched = false;
	};

	inline constexpr UInt32 kInvalidNativeFontCommandIndex =
		std::numeric_limits<UInt32>::max();

	enum class NativeFontCommandSpanState : UInt8
	{
		Ready = 0,
		Executing,
		Consumed,
		Fault
	};

	enum class NativeFontCommandFallback : UInt8
	{
		None = 0,
		Token,
		Generation,
		Atlas,
		Resource,
		Topology,
		Hook,
		Nested,
		RenderTarget,
		State
	};

	struct NativeFontFramePacketBinding
	{
		IDirect3DVertexBuffer9* vertexBuffer = nullptr;
		IDirect3DIndexBuffer9* indexBuffer = nullptr;
		IDirect3DVertexDeclaration9* declaration = nullptr;
		UInt32 baseVertex = 0;
		UInt32 vertexCount = 0;
		UInt32 indexBytes = 0;
		UInt32 generation = 0;
		UInt32 resourceSerial = 0;
		UInt32 uploadEpoch = 0;
		bool staticResident = false;
		bool active = false;
	};

	// Traversal-sealed residency shared by every packet in one payload. Packet
	// commands derive only their range from this view instead of resolving the
	// same payload residency once per packet.
	struct NativeFontFramePayloadBinding
	{
		IDirect3DVertexBuffer9* vertexBuffer = nullptr;
		IDirect3DIndexBuffer9* indexBuffer = nullptr;
		IDirect3DVertexDeclaration9* declaration = nullptr;
		UInt32 payloadBaseVertex = 0;
		UInt32 payloadVertexCount = 0;
		UInt32 indexBytes = 0;
		UInt32 generation = 0;
		UInt32 resourceSerial = 0;
		UInt32 uploadEpoch = 0;
		bool staticResident = false;
		bool active = false;
	};

	// Shader profiles stay private to font_native_shader.cpp. This immutable,
	// generation-owned program is published once with its profile. Traversal-local
	// commands retain only a non-owning pointer and validate the generation before
	// every execution.
	enum class NativeFontStandardBlendSemantics : UInt8
	{
		Unknown = 0,
		Retail,
		NativeOwned
	};

	struct NativeFontBlendState
	{
		UInt8 sourceFunction = static_cast<UInt8>(
			NiAlphaProperty::ALPHA_SRCALPHA);
		UInt8 destinationFunction = static_cast<UInt8>(
			NiAlphaProperty::ALPHA_INVSRCALPHA);
		bool enabled = false;
	};

	NativeFontBlendState ComputeNativeFontOwnedBlendState(
		const NiPropertyState* properties);

	constexpr bool HasPredictableNativeFontBlendSemantics(
		NativeFontStandardBlendSemantics semantics)
	{
		return semantics == NativeFontStandardBlendSemantics::Retail
			|| semantics == NativeFontStandardBlendSemantics::NativeOwned;
	}

	struct NativeFontCompiledPacketCommand
	{
		// Standard v2 elides a TileShader callback only when the generation was
		// built from a reverse-verified retail implementation or a deterministic
		// tNVSE-owned implementation for that slot.
		// The live slot-31 entry is NativeSetupGeometryConstants, so its proof bit
		// describes the vanilla callback retained by the native vtable sidecar.
		static constexpr UInt8 kStandardSlot30Proof = 1u << 0;
		static constexpr UInt8 kStandardSlot31Proof = 1u << 1;
		static constexpr UInt8 kStandardSlot32Proof = 1u << 2;
		static constexpr UInt8 kStandardSlot33Proof = 1u << 3;
		static constexpr UInt8 kStandardSlot34Proof = 1u << 4;
		static constexpr UInt8 kStandardSlot35Proof = 1u << 5;
		static constexpr UInt8 kStandardV2RequiredProofs =
			kStandardSlot30Proof | kStandardSlot31Proof
			| kStandardSlot32Proof | kStandardSlot33Proof
			| kStandardSlot34Proof | kStandardSlot35Proof;
		static_assert(kStandardV2RequiredProofs == 0x3Fu);

		void* profile = nullptr;
		TileShader* shader = nullptr;
		void** shaderVtable = nullptr;
		IDirect3DDevice9* device = nullptr;
		IDirect3DVertexShader9* vertexShader = nullptr;
		IDirect3DPixelShader9* pixelShader = nullptr;
		// Reverse-verified BSBatchRenderer::RenderPassImmediately_Standard call
		// table. The command buffer
		// may use these generation-owned pointers only after proving that the
		// live native TileShader still publishes the same vtable entries.
		void* prepareGeometryForRendering = nullptr;
		void* setupGeometryTextures = nullptr;
		void* setupGeometryConstants = nullptr;
		void* setupGeometryAlphaBlending = nullptr;
		void* setupGeometryAlphaTesting = nullptr;
		void* setupGeometryRenderStates = nullptr;
		void* postGeometry = nullptr;
		void* setupNonFirstPass = nullptr;
		UInt32 generation = 0;
		UInt8 standardV2SlotProofs = 0;
		NativeFontStandardBlendSemantics standardBlendSemantics =
			NativeFontStandardBlendSemantics::Unknown;
		// The immutable native shader vtable retains the exact retail PC slot-27
		// binder and the side-effect-free retail FirstPass callback. A live draw
		// still proves its geometry, renderer and resident descriptor separately.
		bool directDrawLiteReady = false;
		bool simpleColor = false;
		bool active = false;
	};

	struct NativeFontCommandBindState
	{
		bool applyBlend = false;
		bool applyAlphaTest = false;
		bool applyRenderStates = false;
		bool firstPass = false;
	};

	struct NativeFontFrameStamp
	{
		BSShaderAccumulator* accumulator = nullptr;
		NiDX9Renderer* renderer = nullptr;
		IDirect3DDevice9* device = nullptr;
		// Gamebryo mirrors both values in NiDX9Renderer. Retaining the engine
		// render-target-group identity avoids IDirect3DDevice9::GetRenderTarget
		// and its COM AddRef/Release pair during command validation.
		NiRenderTargetGroup* renderTargetGroup = nullptr;
		D3DVIEWPORT9 viewport = {};
		UInt64 validationToken = 0;
		UInt32 generation = 0;
		UInt32 atlasTextureEpoch = 0;
		UInt32 resourceSerial = 0;
		UInt32 uploadEpoch = 0;
		UInt64 nestedTraversalSerial = 0;
		bool renderTargetReady = false;
		bool viewportReady = false;
	};

	// A non-owning execution proof for device-state reuse inside one validated
	// command segment. Exact retail Standard vanilla-Tile passes may retain the
	// segment while narrowly invalidating the state categories they publish. The
	// command buffer assigns the
	// two execution epochs only after validating that segment.
	// Render-target and viewport values are copied rather than retained through
	// COM pointers or mutable renderer state.
	struct NativeFontSegmentDeviceStateStamp
	{
		NiDX9Renderer* renderer = nullptr;
		IDirect3DDevice9* device = nullptr;
		NiRenderTargetGroup* renderTargetGroup = nullptr;
		D3DVIEWPORT9 viewport = {};
		UInt64 validationToken = 0;
		UInt32 generation = 0;
		UInt32 atlasTextureEpoch = 0;
		UInt32 resourceSerial = 0;
		UInt32 uploadEpoch = 0;
		UInt32 executionSegmentEpoch = 0;
		UInt32 externalMutationEpoch = 0;
		bool ready = false;
	};

	// A reverse-verified specialization of TileShader slot 31. It is eligible
	// only after the caller has proved that the complete vanilla/native constant
	// input key is already resident. The implementation therefore replays only
	// the scissor/stencil suffix which slot 35 must subsequently restore.
	enum class NativeTileConstantsLiteResult : UInt8
	{
		Applied = 0,
		NotApplicable,
		ScaledScissor
	};

	NativeTileConstantsLiteResult ApplyNativeTileConstantsLite(
		const NiTriShape* geometry, const NiPropertyState* properties);

	// Translation-only specialization of the same verified retail slot. The
	// caller must prove every other slot-31 input unchanged. It republishes the
	// renderer world mirror and WorldViewProjTranspose at VS c0-c3, then applies
	// the same optional transient suffix as NativeTileConstantsLite.
	enum class NativeTileConstantsTranslationLiteResult : UInt8
	{
		Applied = 0,
		AppliedTransient,
		NotApplicable,
		ScaledScissor,
		NonFinite,
		DeviceFailure
	};

	NativeTileConstantsTranslationLiteResult
		ApplyNativeTileConstantsTranslationLite(
			const NiTriShape* geometry,
			const NiPropertyState* properties,
			NiDX9Renderer* renderer, IDirect3DDevice9* device);

	void ApplyNativeFontGeometryOrigin(NiTransform& destination,
		const NiTransform& source, const NiPoint3& origin);
	bool IsNativeFontPayloadOutsideScissorForWorld(
		const NativeFontShapePayload& payload,
		const NiPropertyState* properties,
		const NiDX9Renderer* renderer,
		const NiTransform& effectiveWorld);
	struct NativeFontDrawCommand
	{
		NiTriShape* sourceGeometry = nullptr;
		NiTriShape* expectedGeometry = nullptr;
		NativeFontShapePayload* payload = nullptr;
		const NativeFontPacketTemplate* packet = nullptr;
		const void* atlasTexture = nullptr;
		NativeFontFramePacketBinding binding;
		const NativeFontCompiledPacketCommand* program = nullptr;
		const NativeFontStandardPassLiteDispatch* standardPassLite = nullptr;
		UInt32 packetIndex = 0;
	};

	struct NativeFontFrameCommandRun
	{
		UInt32 firstCommand = 0;
		UInt32 commandCount = 0;
		bool bridgeEligible = false;
		// Binder runs keep exact shader profiles. Retained replay may continue
		// across an adjacent profile run when the live Tile state is proven
		// identical.
		bool continuesBridgeSpan = false;
	};

	struct NativeFontCommandSpan
	{
		NiTriShape* facade = nullptr;
		const NativeFontShapeMetadata* metadata = nullptr;
		NativeFontShapePayload* payload = nullptr;
		UInt32 firstCommand = 0;
		UInt32 commandCount = 0;
		UInt32 firstRun = 0;
		UInt32 runCount = 0;
		UInt32 generation = 0;
		UInt32 atlasTextureEpoch = 0;
		UInt64 validationToken = 0;
		UInt64 executionValidationToken = 0;
		// Execution-only epochs are assigned when the span enters a validated
		// safe segment and cleared as soon as it is consumed.
		UInt32 executionSegmentEpoch = 0;
		UInt32 executionExternalMutationEpoch = 0;
		NativeFontCommandSpanState state = NativeFontCommandSpanState::Ready;
		bool bridgeEligible = false;
		bool partialDraw = false;
		bool useCompositePackets = false;
	};

	// Ordinary one-packet Tile submissions do not need run/span topology.
	// This traversal-local command embeds its sole draw and carries only the
	// execution state required to share full validation with the current safe
	// segment.
	struct NativeFontSinglePacketCommand
	{
		NiTriShape* facade = nullptr;
		NativeFontShapePayload* payload = nullptr;
		const NativeFontPayloadTemplate* artifact = nullptr;
		NativeFontDrawCommand draw;
		UInt32 generation = 0;
		UInt32 atlasTextureEpoch = 0;
		UInt64 validationToken = 0;
		UInt64 executionValidationToken = 0;
		UInt32 executionSegmentEpoch = 0;
		UInt32 executionExternalMutationEpoch = 0;
		NativeFontCommandSpanState state = NativeFontCommandSpanState::Ready;
		bool partialDraw = false;
		bool useCompositePackets = false;
	};

	// A direct singleton facade needs neither retained-run topology nor a span
	// state machine. Its metadata-owned backend snapshot already
	// contains the exact geometry and frame binding for the sole packet.
	struct NativeFontDirectFacadeSinglePacketCommand
	{
		const NativeFontShapeMetadata* singletonMetadata = nullptr;
		NiTriShape* geometry = nullptr;
		NativeFontShapePayload* payload = nullptr;
		const NativeFontPayloadTemplate* artifact = nullptr;
		const NativeFontDrawCommand* draw = nullptr;
		UInt32 generation = 0;
		UInt32 atlasTextureEpoch = 0;
		UInt64 validationToken = 0;
		UInt64 executionValidationToken = 0;
		UInt32 executionSegmentEpoch = 0;
		UInt32 executionExternalMutationEpoch = 0;
		NativeFontCommandSpanState state = NativeFontCommandSpanState::Ready;
		bool partialDraw = false;
		bool useCompositePackets = false;
	};

	struct NativeFontCommandSpanView
	{
		const NativeFontFrameStamp* stamp = nullptr;
		const NativeFontCommandSpan* span = nullptr;
		const NativeFontDrawCommand* commands = nullptr;
		const NativeFontFrameCommandRun* runs = nullptr;
		UInt32 spanIndex = kInvalidNativeFontCommandIndex;
	};

	struct NativeFontSinglePacketCommandView
	{
		const NativeFontFrameStamp* stamp = nullptr;
		const NativeFontSinglePacketCommand* command = nullptr;
		UInt32 commandIndex = kInvalidNativeFontCommandIndex;
	};

	struct NativeFontDirectFacadeSinglePacketCommandView
	{
		const NativeFontFrameStamp* stamp = nullptr;
		const NativeFontDirectFacadeSinglePacketCommand* command = nullptr;
		UInt32 commandIndex = kInvalidNativeFontCommandIndex;
	};

	const char* NativeFontFallbackReasonName(NativeFontFallbackReason reason);
	const char* NativeFontPacketPrepareFailureName(
		NativeFontPacketPrepareFailure failure);

	NativeFontPayloadTemplatePtr BuildNativeFontPayloadTemplate(
		std::vector<NativeFontGpuVertex>&& vertices,
		UInt32 quadCount, UInt32 glyphCount,
		const NativeFontShapeColorContract& colorContract,
		const NativeFontEffectShapeConfig& effects, const NiBound& bound,
		std::vector<NativeFontCompositeSpan>&& compositeSpans);
	bool InitializeNativeFontShapePayload(Font& font,
		NiTriShape* facade, const NativeFontShapeMetadata& metadata,
		NativeFontPayloadTemplatePtr payloadTemplate,
		const NiPoint3& geometryOrigin, NativeFontShapePayload& payload);
	size_t GetNativeFontPayloadTemplateBytes(
		const NativeFontPayloadTemplate& payloadTemplate);
	size_t GetNativeFontTileRetainedCapacityBytes(
		const NativeFontShapePayload& payload);
	void InvalidateNativeFontTileRetainedText(
		NativeFontShapePayload& payload,
		bool preserveStandardPassLite = false);
	bool BuildNativeFontTileRetainedText(NiTriShape* ownerTile,
		NativeFontShapePayload& payload, UInt32 generation,
		UInt32 atlasTextureEpoch);
	bool IsNativeFontTileRetainedTextCurrent(
		const NativeFontShapePayload& payload, const NiTriShape* ownerTile,
		UInt32 generation, UInt32 atlasTextureEpoch);
	bool BuildNativeFontStandardPassLiteDispatch(
		NiTriShape* geometry,
		const NativeFontCompiledPacketCommand* program,
		UInt32 generation,
		NativeFontStandardPassLiteDispatch& dispatch);
	bool IsNativeFontStandardPassLiteDispatchCurrent(
		const NativeFontStandardPassLiteDispatch& dispatch,
		const NiTriShape* geometry,
		const NativeFontCompiledPacketCommand* program,
		UInt32 generation);
	void InvalidateNativeFontStandardPassLiteDispatch(
		NativeFontStandardPassLiteDispatch& dispatch);
	void InvalidateNativeFontRingResources(NativeFontFallbackReason reason);

	struct NativeFontRingSubmission
	{
		NiTriShape* proxyShape = nullptr;
		NiGeometryBufferData* proxyBuffer = nullptr;
		NiVBChip* proxyChip = nullptr;
		IDirect3DVertexBuffer9* vertexBuffer = nullptr;
		UInt32 proxyIndex = std::numeric_limits<UInt32>::max();
		UInt32 generation = 0;
		UInt32 resourceSerial = 0;
		UInt32 nextPacket = 0;
		UInt32 payloadBaseVertex = 0;
		UInt32 endVertex = 0;
		bool staticResident = false;
		bool active = false;
	};

	// A sorted single-packet shape can borrow the sealed ring/static resources
	// directly for the duration of its vanilla Tile render pass. Unlike
	// NativeFontRingSubmission this lease owns no proxy and copies no Tile state:
	// the actual facade remains the render-pass geometry, so its live transform,
	// scissor, alpha, material, cull, and stencil state stay authoritative.
	struct NativeFontDirectShapeSubmission
	{
		IDirect3DVertexBuffer9* vertexBuffer = nullptr;
		IDirect3DIndexBuffer9* indexBuffer = nullptr;
		IDirect3DVertexDeclaration9* declaration = nullptr;
		UInt32 baseVertex = 0;
		UInt32 vertexCount = 0;
		UInt32 indexBytes = 0;
		UInt32 generation = 0;
		UInt32 resourceSerial = 0;
		bool staticResident = false;
		bool active = false;
	};

	// A singleton-facade shape keeps a plugin-owned geometry descriptor and borrows
	// either immutable static residency or a traversal-sealed dynamic range for
	// the complete sorted Tile traversal. The sorted-frame lease owns the D3D
	// resources; this value is a validated non-owning view and must never outlive
	// that traversal.
	struct NativeFontDirectFacadePacketBinding
	{
		IDirect3DVertexBuffer9* vertexBuffer = nullptr;
		IDirect3DIndexBuffer9* indexBuffer = nullptr;
		IDirect3DVertexDeclaration9* declaration = nullptr;
		UInt32 baseVertex = 0;
		UInt32 vertexCount = 0;
		UInt32 indexBytes = 0;
		UInt32 generation = 0;
		UInt32 resourceSerial = 0;
		UInt32 uploadEpoch = 0;
		UInt32 atlasTextureEpoch = 0;
		bool staticResident = false;
		bool active = false;
	};

	bool EnsureNativeFontProxyPool();
	NativeFontFallbackReason BeginNativeFontDirectShapeSubmission(
		NiTriShape* facade, NativeFontShapePayload& payload,
		NativeFontDirectShapeSubmission& submission);
	void EndNativeFontDirectShapeSubmission(
		NativeFontDirectShapeSubmission& submission);
	NativeFontFallbackReason ResolveNativeFontDirectFacadePacketBinding(
		NativeFontShapePayload& payload, UInt32 packetIndex,
		NativeFontDirectFacadePacketBinding& binding);
	bool IsNativeFontDirectFacadePacketBindingCurrent(
		const NativeFontDirectFacadePacketBinding& binding);
	bool IsNativeFontDirectFacadePacketAtlasCurrent(
		const NiTriShape* shape, const NativeFontShapePayload& payload,
		UInt32 packetIndex);
	NativeFontFallbackReason BeginNativeFontRingSubmission(
		NiTriShape* facade, NativeFontShapePayload& payload,
		NativeFontRingSubmission& submission);
	NativeFontFallbackReason PrepareNativeFontRingPacket(
		NiTriShape* facade, NativeFontShapePayload& payload,
		NativeFontRingSubmission& submission, UInt32 packetIndex,
		NiTriShape*& proxyShape);
	NativeFontFallbackReason SkipNativeFontRingPacket(
		NativeFontShapePayload& payload,
		NativeFontRingSubmission& submission, UInt32 packetIndex);
	void EndNativeFontRingSubmission(NativeFontRingSubmission& submission);
	void ReleaseNativeFontRingResources();
	void PrepareSortedNativeFontPayloads(
		std::vector<NativeFontPayloadTemplatePtr>& payloadTemplates,
		UInt32 generation);
	void EndNativeFontSortedRingFrame();
	bool ResolveNativeFontFramePacketBinding(
		const NativeFontShapePayload& payload, UInt32 packetIndex,
		NativeFontFramePacketBinding& binding);
	bool ResolveNativeFontFramePayloadBinding(
		const NativeFontShapePayload& payload,
		NativeFontFramePayloadBinding& binding);
	bool IsNativeFontFramePacketBindingCurrent(
		const NativeFontFramePacketBinding& binding);
	bool IsNativeFontFrameResourceStampCurrent(
		UInt32 generation, UInt32 resourceSerial, UInt32 uploadEpoch);
	void TrimNativeFontCpuCachesForTotalBudget();
	bool FindNativeFontSortedFrameEntry(NiTriShape* facade,
		NativeFontSortedFrameEntryView& view);
	UInt64 GetNativeFontSortedFrameValidationToken();
	UInt64 GetNativeFontSortedNestedTraversalSerial();
	NativeFontVisibilityCull EvaluateNativeFontSubmissionVisibility(
		const NiTriShape* facade);
	NativeFontVisibilityCull EvaluateNativeFontSubmissionVisibility(
		const NiTriShape* facade, const NativeFontShapePayload& payload);
	void BeginNativeFontVisibilityFrame();
	void CompleteNativeFontVisibilityPreflight();
	void EndNativeFontVisibilityFrame();
	// Resolve the Tile property once for zero-alpha and clip/scissor evaluation.
	// The destination must be freshly value-initialized, as with the clip-only
	// in-place form below.
	void EvaluateNativeFontSortedVisibilityInPlace(
		const NiTriShape* facade, NativeFontVisibilityPreflight& visibility);
	NativeFontVisibilityPreflight EvaluateNativeFontPreflightClipVisibility(
		const NiTriShape* facade);
	// The destination must be freshly value-initialized. This form writes the
	// proof witness directly into its owning frame entry without constructing a
	// second NativeFontVisibilityPreflight and copying the complete witness.
	void EvaluateNativeFontPreflightClipVisibilityInPlace(
		const NiTriShape* facade, NativeFontVisibilityPreflight& visibility);
	NativeFontVisibilityPreflight EvaluateNativeFontPreflightClipVisibility(
		const NiTriShape* facade, const NativeFontShapePayload& payload);
	bool HonorNativeFontPreflightClipCull(const NiTriShape* facade,
		const NativeFontVisibilityPreflight& preflight,
		bool reuseCertifiedCamera);
	bool ReuseNativeFontPreflightClipOverlap(
		const NativeFontVisibilityPreflight& preflight);
	void RecordNativeFontVisibilityCull(NativeFontVisibilityCull reason,
		const NativeFontShapePayload& payload);
	void RecordNativeFontVisibilityCull(NativeFontVisibilityCull reason);
	UInt32 GetNativeFontAtlasTextureEpoch();
	void NotifyNativeFontAtlasTextureMutation();

	bool InitializeNativeFontRenderer(bool forceAttempt, bool reportFailures);
	void HandleNativeFontShaderRendererMainLoop();
	void HandleNativeFontShaderLoaderMessage(UInt32 messageType);
	bool IsNativeFontShaderRendererAvailable();
	struct NativeFontRendererReadinessView
	{
		NiDX9Renderer* renderer = nullptr;
		IDirect3DDevice9* device = nullptr;
		UInt32 generation = 0;
		bool ready = false;
	};
	bool GetNativeFontRendererReadinessFast(
		NativeFontRendererReadinessView& view);
	void MarkNativeFontGenerationFault(UInt32 generation,
		const char* operation, HRESULT result);
	UInt32 GetNativeFontShaderGeneration();
	IDirect3DVertexDeclaration9* GetNativeFontD3DDeclaration(UInt32 generation);
	bool IsNativeFontShaderGenerationCurrent(UInt32 generation);
	void BeginNativeFontSortedShaderBatch();
	void EndNativeFontSortedShaderBatch();
	void InvalidateNativeFontSortedShaderState();
	void InvalidateNativeFontSortedShaderStateWithinExecutionSegment();
	void InvalidateNativeFontSortedShaderStateForForeignRenderPass();
	UInt64 BeginNativeFontVanillaLayoutShaderTransition(
		TileShader* shader, UInt32 currentPass);
	bool EndNativeFontVanillaLayoutShaderTransition(
		UInt64 token, TileShader* shader);
	void BeginNativeFontFacadeShaderBatch();
	void EndNativeFontFacadeShaderBatch();
	TileShader* ResolveNativeFontPacketShader(const NativeFontPacketTemplate& packet,
		const NiTriShape* facade, bool scaledFillSampling,
		NativeFontVanillaLayoutKind vanillaLayoutKind =
			NativeFontVanillaLayoutKind::None);
	bool RequestNativeFontVanillaLayoutShapePrecache(NiTriShape* shape,
		TileShader* shader);
	bool EnsureNativeFontVanillaLayoutShapeReady(const NiTriShape* shape,
		TileShader* shader, const NativeFontShapePayload& payload,
		NativeFontVanillaLayoutDrawToken& drawToken, bool& drawTokenHit);
	bool ResolveNativeFontRetainedPacketProgram(
		const NativeFontPacketTemplate& packet,
		TileShader* shader, UInt32 generation,
		const NativeFontCompiledPacketCommand*& program);
	bool BindNativeFontCommandPacket(
		const NativeFontCompiledPacketCommand& command,
		const void* atlasTexture, bool publishPrograms,
		const NiPropertyState* properties,
		const NativeFontCommandBindState& bindState,
		const char*& operation, HRESULT& result);
	NativeFontFallbackReason PrepareNativeFontFacade(NiTriShape* facade,
		const NativeFontShapeMetadata& metadata, NativeFontShapePayload& payload);

	void BeginNativeFontFrameCommandBuffer(BSShaderAccumulator* accumulator,
		UInt64 validationToken, UInt32 generation, UInt32 atlasTextureEpoch);
	void ReserveNativeFontFrameCommandBuffer(size_t ordinaryEntryCount,
		size_t directFacadeCount);
	UInt32 AddNativeFontFrameSinglePacketCommand(NiTriShape* facade,
		const NativeFontShapeMetadata* metadata, NativeFontShapePayload* payload);
	UInt32 AddNativeFontFrameDirectFacadeCommand(
		const NativeFontShapeMetadata* metadata);
	UInt32 AddNativeFontFrameCommandSpan(NiTriShape* facade,
		const NativeFontShapeMetadata* metadata, NativeFontShapePayload* payload);
	void ActivateNativeFontFrameCommandBuffer();
	void EndNativeFontFrameCommandBuffer();
	void InvalidateNativeFontCommandExecutionSegment(
		NativeFontCommandFallback reason = NativeFontCommandFallback::State);
	void NotifyNativeFontCommandExternalMutation(
		NativeFontCommandFallback reason);
	void InvalidateNativeFontCommandGeometry(NiTriShape* geometry);
	bool FindNativeFontCommandSpan(UInt32 spanIndex, UInt64 validationToken,
		NativeFontCommandSpanView& view);
	bool BeginNativeFontCommandSpanExecution(UInt32 spanIndex,
		NiTriShape* geometry, NativeFontCommandSpanView& view);
	void EndNativeFontCommandSpanExecution(UInt32 spanIndex,
		bool success, bool drewPacket);
	bool ValidateNativeFontCommand(UInt32 spanIndex,
		UInt32 commandOffset, NiTriShape* geometry, NiRenderer* renderer);
	bool GuardNativeFontCommand(UInt32 spanIndex,
		UInt32 commandOffset, NiTriShape* geometry, NiRenderer* renderer);
	bool FindNativeFontSinglePacketCommand(UInt32 commandIndex,
		UInt64 validationToken, NativeFontSinglePacketCommandView& view);
	bool BeginNativeFontSinglePacketCommandExecution(UInt32 commandIndex,
		NiTriShape* geometry, NativeFontSinglePacketCommandView& view);
	void EndNativeFontSinglePacketCommandExecution(UInt32 commandIndex,
		bool success, bool drewPacket);
	void AbandonNativeFontSinglePacketCommandExecution(UInt32 commandIndex);
	bool IsNativeFontSinglePacketCommandConsumed(
		UInt32 commandIndex, UInt64 validationToken);
	bool ValidateNativeFontSinglePacketCommand(UInt32 commandIndex,
		NiTriShape* geometry, NiRenderer* renderer);
	bool GuardNativeFontSinglePacketCommand(UInt32 commandIndex,
		NiTriShape* geometry, NiRenderer* renderer);
	bool FindNativeFontDirectFacadeSinglePacketCommand(UInt32 commandIndex,
		UInt64 validationToken,
		NativeFontDirectFacadeSinglePacketCommandView& view);
	bool BeginNativeFontDirectFacadeSinglePacketCommandExecution(
		UInt32 commandIndex, const NativeFontShapeMetadata* singletonMetadata,
		NiTriShape* geometry,
		NativeFontDirectFacadeSinglePacketCommandView& view);
	void EndNativeFontDirectFacadeSinglePacketCommandExecution(
		UInt32 commandIndex, bool success, bool drewPacket);
	void AbandonNativeFontDirectFacadeSinglePacketCommandExecution(
		UInt32 commandIndex);
	bool IsNativeFontDirectFacadeSinglePacketCommandConsumed(
		UInt32 commandIndex, UInt64 validationToken);
	bool ValidateNativeFontDirectFacadeSinglePacketCommand(UInt32 commandIndex,
		NiTriShape* geometry, NiRenderer* renderer);
	bool GuardNativeFontDirectFacadeSinglePacketCommand(UInt32 commandIndex,
		NiTriShape* geometry, NiRenderer* renderer);
	void RecordNativeFontCommandFallback(NativeFontCommandFallback reason);

	bool HookNativeFontAccumulator();
	bool IsNativeFontAccumulatorHookCurrent();
	bool IsNativeFontRenderAlphaGeometryHookCurrent();
	bool IsNativeFontRegistrationHookChainCurrent();
	bool IsNativeFontRegistrationHookChainCurrentFast();

	void RecordNativeFontSuppression(NiTriShape* shape,
		const NativeFontShapeMetadata& metadata, NativeFontFallbackReason reason,
		const char* phase);
	void MarkNativeFontRuntimeFault(const NativeFontShapeMetadata& metadata,
		NativeFontShapePayload& payload,
		NativeFontFallbackReason reason);

}

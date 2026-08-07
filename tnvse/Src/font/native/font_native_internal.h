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
	struct A8ShapeMetadata;
	struct NativeA8CompiledPacketCommand;
	inline constexpr UInt32 kNativeA8MaximumQuads =
		std::numeric_limits<UInt16>::max() / 4u;

	template <class T, size_t InlineCapacity = 1>
	class NativeA8InlineVector
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
	inline constexpr size_t kNativeA8PacketConstantRegisterCount = 8;
	inline constexpr size_t kNativeA8PacketConstantFloatCount =
		kNativeA8PacketConstantRegisterCount * 4;
	// Keep every tNVSE-owned float constant above the complete shipped and
	// audited New Vegas plugin shader footprint, while retaining guard space
	// below the SM3 register-file boundary. Pixel c0 remains the stock Tile
	// color; vertex c0-c4 remain the stock WVP/TexScroll range.
	inline constexpr UInt32 kNativeA8PixelConstantBaseRegister = 176;
	inline constexpr UInt32 kNativeA8PixelConstantLastRegister =
		kNativeA8PixelConstantBaseRegister
		+ static_cast<UInt32>(kNativeA8PacketConstantRegisterCount) - 1u;
	inline constexpr UInt32 kNativeA8VertexAaConstantRegister = 208;
	inline constexpr UInt32 kNativeA8StockLayoutGlyphConstantRegister = 209;
	static_assert(kNativeA8PixelConstantLastRegister <= 223);
	static_assert(kNativeA8VertexAaConstantRegister <= 255);
	static_assert(kNativeA8StockLayoutGlyphConstantRegister <= 255);
	static_assert(kNativeA8StockLayoutGlyphConstantRegister
		== kNativeA8VertexAaConstantRegister + 1u);

	enum class NativeA8ShaderClass : UInt8
	{
		Body,
		Effect,
		Composite,
		Coverage,
		Argb
	};

	enum class NativeA8Sampling : UInt8
	{
		Point,
		LinearMipmapped,
		LinearLod0
	};

	enum class NativeA8FallbackReason : UInt8
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

	enum class NativeA8PacketPrepareFailure : UInt8
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

	struct NativeA8GpuVertex
	{
		float x = 0.0f;
		float y = 0.0f;
		float z = 0.0f;
		float u = 0.0f;
		float v = 0.0f;
		// D3DDECLTYPE_D3DCOLOR expands this packed ARGB value to the shader's
		// normalized float4 COLOR0 input. Distance-field profiles retain their
		// per-packet layer color in c1; baked coverage instead places the complete
		// base/layer modifier here so different effects can share one packet.
		UInt32 color = 0xFFFFFFFFu;
		// Per-glyph distance-field data must not participate in packet identity.
		// Shared MTSDF double-byte atlases can mix source sizes in one text run;
		// carrying these values in TEXCOORD1 keeps those glyphs in the same
		// layer/page packet without changing their reconstruction parameters.
		float sdfSpread = 0.0f;
		float distanceParameterScale = 1.0f;
		// Exact integer mask (bits 0..3: Shadow/Glow/Outline/Fill) for distance
		// fields. Baked A8 coverage profiles instead store their per-quad live
		// Tile RGB selector here (0=fixed RGB, 1=live Tile RGB).
		float layerMask = 8.0f;
		// Exact physical glyph rectangle. The composite quad can extrapolate its
		// UVs to cover an offset shadow; the pixel shader bounds every sample to
		// this rectangle so atlas neighbours never bleed into that union.
		// Use an ordinary FLOAT4 declaration for compatibility with native D3D9
		// drivers and wrappers that reject the optional USHORT4N declaration
		// type. The HLSL input remains an exact normalized atlas rectangle.
		float glyphU0 = 0.0f;
		float glyphV0 = 0.0f;
		float glyphU1 = 0.0f;
		float glyphV1 = 0.0f;
	};
	static_assert(offsetof(NativeA8GpuVertex, u) == 3 * sizeof(float));
	static_assert(offsetof(NativeA8GpuVertex, color) == 5 * sizeof(float));
	static_assert(offsetof(NativeA8GpuVertex, sdfSpread) == 6 * sizeof(float));
	static_assert(offsetof(NativeA8GpuVertex, glyphU0) == 9 * sizeof(float));
	static_assert(sizeof(NativeA8GpuVertex) == 13 * sizeof(float));

	struct NativeA8PacketShaderCacheEntry
	{
		// Native shader profiles are process-lifetime objects. Keep this opaque in
		// the shared packet model; the shader implementation validates generation
		// and sampling before using the cached profile.
		std::atomic<void*> profile{ nullptr };

		NativeA8PacketShaderCacheEntry() = default;
		NativeA8PacketShaderCacheEntry(
			const NativeA8PacketShaderCacheEntry&) noexcept
		{
		}
		NativeA8PacketShaderCacheEntry(
			NativeA8PacketShaderCacheEntry&&) noexcept
		{
		}
		NativeA8PacketShaderCacheEntry& operator=(
			const NativeA8PacketShaderCacheEntry&) noexcept
		{
			profile.store(nullptr, std::memory_order_relaxed);
			return *this;
		}
		NativeA8PacketShaderCacheEntry& operator=(
			NativeA8PacketShaderCacheEntry&&) noexcept
		{
			profile.store(nullptr, std::memory_order_relaxed);
			return *this;
		}
	};

	struct NativeA8PacketTemplate
	{
		UInt32 firstVertex = 0;
		UInt32 vertexCount = 0;
		NiBound bound;
		std::array<float, kNativeA8PacketConstantFloatCount> constants = {};
		NativeA8ShaderClass shaderClass = NativeA8ShaderClass::Body;
		NativeA8Sampling sampling = NativeA8Sampling::Point;
		EffectQuality quality = EffectQuality::Balanced;
		DistanceFieldMethod distanceFieldMethod =
			GetConfiguredDistanceFieldMethod();
		UInt32 layer = 3;
		UInt16 atlasPage = 0;
		// Zero keeps the generic per-glyph mask shader. A non-zero value proves
		// that every quad in this composite packet carries the same exact mask
		// and permits an immutable MTSDF pixel-shader profile.
		UInt8 staticCompositeLayerMask = 0;
		bool compositeShiftedShadow = false;
		bool staticSmoothSampling = false;
		bool usesLiveTileRgb = true;
		// Packet-wide source spread used only by the stock-layout SDF target.
		// Zero means the packet is not eligible; the ordinary native pipeline
		// continues to read per-vertex spread and keeps its historical profile key.
		float uniformSdfSpread = 0.0f;
		// Profile hashes are immutable artifact data. The two slots correspond to
		// separate-alpha disabled/enabled. Resolved profiles are validated against
		// the current generation and reset automatically on packet copy or move.
		std::array<size_t, 2> profileHashes = {};
		mutable std::array<NativeA8PacketShaderCacheEntry, 2>
			resolvedShaders;
		// Stock-layout and native-facade profiles intentionally have distinct
		// keys and shaders. Keep a second packet-local cache so the retail-layout
		// hot path does not reload the generation profile map on every draw.
		mutable std::array<NativeA8PacketShaderCacheEntry, 2>
			stockLayoutResolvedShaders;
	};

	struct NativeA8CompositeSpan
	{
		UInt32 firstVertex = 0;
		UInt32 vertexCount = 0;
		UInt16 atlasPage = 0;
		bool fused = false;
	};

	struct NativeA8CompiledPacketCommand;

	// Renderer-owned VB locations are mutable cache data attached to the immutable
	// text artifact. Every read is protected by the ring mutex and validated
	// against the current resource serial/epoch before the location is used.
	struct NativeA8PayloadResidencyCache
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

	// BuildNativeA8PayloadTemplate publishes the text artifact through a
	// shared_ptr<const>.  Once published, every field below is immutable except
	// for the separately synchronized residency cache.  Record the complete
	// structural validation once at construction so registering another live
	// Tile does not rescan every glyph vertex and packet.
	struct NativeA8PayloadValidationSeal
	{
		static constexpr UInt32 kAbi = 1;

		UInt32 abi = 0;
		UInt32 pageCount = 0;
		UInt32 quadCount = 0;
		UInt32 sourceRangeCount = 0;
		UInt32 vertexCount = 0;
		UInt32 packetCount = 0;
		UInt32 compositePacketCount = 0;
		bool stockLikeBitmapPackets = false;
	};

	struct NativeA8PayloadTemplate
	{
		CpuMemoryLease cpuMemory;
		UInt32 pageCount = 0;
		UInt32 quadCount = 0;
		UInt32 sourceRangeCount = 0;
		NiBound bound;
		std::vector<NiTexturingPropertyPtr> atlasProperties;
		std::vector<NiTexturePtr> atlasTextures;
		std::vector<NativeA8GpuVertex> gpuVertices;
		std::vector<NativeA8PacketTemplate> packets;
		// Optional single-pass distance-field representation. The ordinary packet
		// list remains available if that shader class is unavailable.
		std::vector<NativeA8PacketTemplate> compositePackets;
		mutable NativeA8PayloadResidencyCache residency;
		// Tail-only construction certificate.  Count copies make the read path
		// fail closed if an accidental internal mutation ever invalidates the
		// otherwise immutable artifact.
		NativeA8PayloadValidationSeal validationSeal;
	};
	using NativeA8PayloadTemplatePtr =
		std::shared_ptr<const NativeA8PayloadTemplate>;

	inline bool HasNativeA8PayloadValidationSeal(
		const NativeA8PayloadTemplate& payloadTemplate)
	{
		const NativeA8PayloadValidationSeal& seal =
			payloadTemplate.validationSeal;
		return seal.abi == NativeA8PayloadValidationSeal::kAbi
			&& seal.pageCount != 0
			&& seal.pageCount == payloadTemplate.pageCount
			&& seal.quadCount != 0
			&& seal.quadCount == payloadTemplate.quadCount
			&& seal.sourceRangeCount != 0
			&& seal.sourceRangeCount == payloadTemplate.sourceRangeCount
			&& seal.vertexCount == payloadTemplate.gpuVertices.size()
			&& seal.packetCount != 0
			&& seal.packetCount == payloadTemplate.packets.size()
			&& seal.compositePacketCount
				== payloadTemplate.compositePackets.size();
	}

	// The Standard-lite call program is resolved once for a live Tile and shader
	// generation. It deliberately excludes VB/IB/declaration residency: ordinary
	// single-packet facades may attach a short-lived synthetic buffer while the
	// Tile, property state, shader program, and renderer remain stable.
	struct NativeA8StandardPassLiteDispatch
	{
		NiTriShape* geometry = nullptr;
		const NiPropertyState* properties = nullptr;
		NiDX9Renderer* renderer = nullptr;
		TileShader* shader = nullptr;
		const NativeA8CompiledPacketCommand* program = nullptr;
		UInt32 generation = 0;
		bool standardV2Ready = false;
		bool ready = false;
	};

	// The Text Artifact owns immutable geometry and packet data. The resolved
	// replay program belongs to one live Tile facade instead: shader/profile
	// selection depends on that Tile's alpha and sampling class. These packet
	// skeletons retain no renderer state or D3D COM ownership; traversal-local
	// commands add the current atlas and VB/IB residency after sorted preflight.
	struct NativeA8TileRetainedPacket
	{
		const NativeA8PacketTemplate* packet = nullptr;
		const NativeA8CompiledPacketCommand* program = nullptr;
		UInt32 packetIndex = 0;
		UInt32 firstVertex = 0;
		UInt32 vertexCount = 0;
		UInt16 atlasPage = 0;
	};

	struct NativeA8TileRetainedRun
	{
		UInt32 firstPacket = 0;
		UInt32 packetCount = 0;
		bool bridgeEligible = false;
		bool continuesBridgeSpan = false;
	};

	struct NativeA8TileRetainedText
	{
		NiTriShape* ownerTile = nullptr;
		const NativeA8PayloadTemplate* artifact = nullptr;
		NativeA8InlineVector<NativeA8TileRetainedPacket> packets;
		NativeA8InlineVector<NativeA8TileRetainedRun> runs;
		// Standard-lite is currently a dedicated single-packet specialization,
		// so one Tile-lifetime dispatch is sufficient for this retained text.
		NativeA8StandardPassLiteDispatch standardPassLite;
		UInt32 generation = 0;
		UInt32 atlasTextureEpoch = 0;
		bool useCompositePackets = false;
		bool bridgeEligible = false;
		bool ready = false;
	};

	struct NativeA8ShapePayload
	{
		NativeA8PayloadTemplatePtr payloadTemplate;
		NiPoint3 geometryOrigin;
		// Packet geometry, constants, page identity, and profile keys live in the
		// shared text artifact. The Tile instance retains its generation-bound
		// shader/program skeleton until that Tile is destroyed or preflight is
		// invalidated.
		NativeA8InlineVector<TileShader*> packetShaders;
		NativeA8InlineVector<const NativeA8CompiledPacketCommand*> packetPrograms;
		NativeA8TileRetainedText retainedText;
		std::atomic<bool> suppressNextSubmit = false;
		std::atomic<NativeA8FallbackReason> stickyReason =
			NativeA8FallbackReason::None;
		std::atomic<NativeA8PacketPrepareFailure> packetPrepareFailure =
			NativeA8PacketPrepareFailure::None;
		// A successful preflight is reusable while the shader generation, scaled
		// fill sampling class, and the referenced page textures remain unchanged.
		// Null entries belong to atlas pages that no packet in this payload uses.
		NativeA8InlineVector<const void*> preflightAtlasTextures;
		UInt32 preparedGeneration = 0;
		UInt32 compositeAttemptGeneration = 0;
		UInt32 preflightAtlasTextureEpoch = 0;
		bool preflightScaledFillSampling = false;
		bool preflightAlphaBlending = false;
		bool useCompositePackets = false;
		bool topologyObserved = false;
		bool lastTopologyComposite = false;
		bool compositeUnavailable = false;
		bool stockLikeBitmapPackets = false;
		bool buildComplete = false;
	};

	inline const std::vector<NativeA8PacketTemplate>& GetNativeA8Packets(
		const NativeA8PayloadTemplate& payloadTemplate, bool useComposite)
	{
		return useComposite && !payloadTemplate.compositePackets.empty()
			? payloadTemplate.compositePackets : payloadTemplate.packets;
	}

	inline bool UsesOnlyStockLikeBitmapPackets(
		const std::vector<NativeA8PacketTemplate>& packets)
	{
		if (packets.empty())
			return false;
		for (const NativeA8PacketTemplate& packet : packets)
		{
			if (packet.shaderClass != NativeA8ShaderClass::Argb
				&& packet.shaderClass != NativeA8ShaderClass::Coverage)
			{
				return false;
			}
		}
		return true;
	}

	enum class NativeA8VisibilityCull : UInt8
	{
		None = 0,
		AppCulled,
		ZeroAlpha,
		Clip,
		Scissor
	};

	enum class NativeA8VisibilityProofStatus : UInt8
	{
		Unproven = 0,
		Overlap,
		Outside
	};

	struct NativeA8VisibilityPreflight
	{
		UInt64 frameToken = 0;
		NativeA8VisibilityProofStatus status =
			NativeA8VisibilityProofStatus::Unproven;
		NativeA8VisibilityCull cull = NativeA8VisibilityCull::None;
	};

	struct NativeA8SortedFrameEntryView
	{
		const A8ShapeMetadata* metadata = nullptr;
		NativeA8ShapePayload* payload = nullptr;
		NativeA8FallbackReason preflightResult =
			NativeA8FallbackReason::RuntimeFault;
		NativeA8VisibilityPreflight visibility;
		UInt32 generation = 0;
		UInt64 validationToken = 0;
		UInt32 commandSpanIndex = std::numeric_limits<UInt32>::max();
		UInt32 singlePacketCommandIndex =
			std::numeric_limits<UInt32>::max();
	};

	inline constexpr UInt32 kInvalidNativeA8CommandIndex =
		std::numeric_limits<UInt32>::max();

	enum class NativeA8CommandSpanState : UInt8
	{
		Ready = 0,
		Executing,
		Consumed,
		Fault
	};

	enum class NativeA8CommandFallback : UInt8
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

	struct NativeA8FramePacketBinding
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
	struct NativeA8FramePayloadBinding
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
	enum class NativeA8StandardBlendSemantics : UInt8
	{
		Unknown = 0,
		Retail,
		NativeOwned
	};

	struct NativeA8BlendState
	{
		UInt8 sourceFunction = static_cast<UInt8>(
			NiAlphaProperty::ALPHA_SRCALPHA);
		UInt8 destinationFunction = static_cast<UInt8>(
			NiAlphaProperty::ALPHA_INVSRCALPHA);
		bool enabled = false;
	};

	NativeA8BlendState ComputeNativeA8OwnedBlendState(
		const NiPropertyState* properties);

	constexpr bool HasPredictableNativeA8BlendSemantics(
		NativeA8StandardBlendSemantics semantics)
	{
		return semantics == NativeA8StandardBlendSemantics::Retail
			|| semantics == NativeA8StandardBlendSemantics::NativeOwned;
	}

	struct NativeA8CompiledPacketCommand
	{
		// Standard v2 elides a TileShader callback only when the generation was
		// built from a reverse-verified retail implementation or a deterministic
		// tNVSE-owned implementation for that slot.
		// The live slot-31 entry is NativeUpdateConstants, so its proof bit
		// describes the stock callback retained by the native vtable sidecar.
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
		void* prepareGeometry = nullptr;
		void* setupPass = nullptr;
		void* updateConstants = nullptr;
		void* setupBlend = nullptr;
		void* setupAlphaTest = nullptr;
		void* setupDrawmode = nullptr;
		void* postGeometry = nullptr;
		void* setupNonFirstPass = nullptr;
		UInt32 generation = 0;
		UInt8 standardV2SlotProofs = 0;
		NativeA8StandardBlendSemantics standardBlendSemantics =
			NativeA8StandardBlendSemantics::Unknown;
		// The immutable native shader vtable retains the exact retail PC slot-27
		// binder and the side-effect-free retail FirstPass callback. A live draw
		// still proves its geometry, renderer and resident descriptor separately.
		bool directDrawLiteReady = false;
		bool simpleColor = false;
		bool active = false;
	};

	struct NativeA8CommandBindState
	{
		bool applyBlend = false;
		bool applyAlphaTest = false;
		bool applyDrawmode = false;
		bool firstPass = false;
	};

	struct NativeA8FrameStamp
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
	// command segment. Exact retail Standard stock-Tile passes may retain the
	// segment while narrowly invalidating the state categories they publish. The
	// command buffer assigns the
	// two execution epochs only after validating that segment.
	// Render-target and viewport values are copied rather than retained through
	// COM pointers or mutable renderer state.
	struct NativeA8SegmentDeviceStateStamp
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
	// only after the caller has proved that the complete stock/native constant
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

	void ApplyNativeA8GeometryOrigin(NiTransform& destination,
		const NiTransform& source, const NiPoint3& origin);
	bool IsNativeA8PayloadOutsideScissorForWorld(
		const NativeA8ShapePayload& payload,
		const NiPropertyState* properties,
		const NiDX9Renderer* renderer,
		const NiTransform& effectiveWorld);
	struct NativeA8DrawCommand
	{
		NiTriShape* sourceGeometry = nullptr;
		NiTriShape* expectedGeometry = nullptr;
		NativeA8ShapePayload* payload = nullptr;
		const NativeA8PacketTemplate* packet = nullptr;
		const void* atlasTexture = nullptr;
		NativeA8FramePacketBinding binding;
		const NativeA8CompiledPacketCommand* program = nullptr;
		const NativeA8StandardPassLiteDispatch* standardPassLite = nullptr;
		UInt32 packetIndex = 0;
	};

	struct NativeA8FrameCommandRun
	{
		UInt32 firstCommand = 0;
		UInt32 commandCount = 0;
		bool bridgeEligible = false;
		// Binder runs keep exact shader profiles. Retained replay may continue
		// across an adjacent profile run when the live Tile state is proven
		// identical.
		bool continuesBridgeSpan = false;
	};

	struct NativeA8CommandSpan
	{
		NiTriShape* facade = nullptr;
		const A8ShapeMetadata* metadata = nullptr;
		NativeA8ShapePayload* payload = nullptr;
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
		NativeA8CommandSpanState state = NativeA8CommandSpanState::Ready;
		bool bridgeEligible = false;
		bool partialDraw = false;
		bool useCompositePackets = false;
	};

	// Ordinary one-packet Tile submissions do not need run/span topology.
	// This traversal-local command embeds its sole draw and carries only the
	// execution state required to share full validation with the current safe
	// segment.
	struct NativeA8SinglePacketCommand
	{
		NiTriShape* facade = nullptr;
		NativeA8ShapePayload* payload = nullptr;
		const NativeA8PayloadTemplate* artifact = nullptr;
		NativeA8DrawCommand draw;
		UInt32 generation = 0;
		UInt32 atlasTextureEpoch = 0;
		UInt64 validationToken = 0;
		UInt64 executionValidationToken = 0;
		UInt32 executionSegmentEpoch = 0;
		UInt32 executionExternalMutationEpoch = 0;
		NativeA8CommandSpanState state = NativeA8CommandSpanState::Ready;
		bool partialDraw = false;
		bool useCompositePackets = false;
	};

	// A direct singleton facade needs neither retained-run topology nor a span
	// state machine. Its metadata-owned backend snapshot already
	// contains the exact geometry and frame binding for the sole packet.
	struct NativeA8DirectFacadeSinglePacketCommand
	{
		const A8ShapeMetadata* singletonMetadata = nullptr;
		NiTriShape* geometry = nullptr;
		NativeA8ShapePayload* payload = nullptr;
		const NativeA8PayloadTemplate* artifact = nullptr;
		const NativeA8DrawCommand* draw = nullptr;
		UInt32 generation = 0;
		UInt32 atlasTextureEpoch = 0;
		UInt64 validationToken = 0;
		UInt64 executionValidationToken = 0;
		UInt32 executionSegmentEpoch = 0;
		UInt32 executionExternalMutationEpoch = 0;
		NativeA8CommandSpanState state = NativeA8CommandSpanState::Ready;
		bool partialDraw = false;
		bool useCompositePackets = false;
	};

	struct NativeA8CommandSpanView
	{
		const NativeA8FrameStamp* stamp = nullptr;
		const NativeA8CommandSpan* span = nullptr;
		const NativeA8DrawCommand* commands = nullptr;
		const NativeA8FrameCommandRun* runs = nullptr;
		UInt32 spanIndex = kInvalidNativeA8CommandIndex;
	};

	struct NativeA8SinglePacketCommandView
	{
		const NativeA8FrameStamp* stamp = nullptr;
		const NativeA8SinglePacketCommand* command = nullptr;
		UInt32 commandIndex = kInvalidNativeA8CommandIndex;
	};

	struct NativeA8DirectFacadeSinglePacketCommandView
	{
		const NativeA8FrameStamp* stamp = nullptr;
		const NativeA8DirectFacadeSinglePacketCommand* command = nullptr;
		UInt32 commandIndex = kInvalidNativeA8CommandIndex;
	};

	const char* NativeA8FallbackReasonName(NativeA8FallbackReason reason);
	const char* NativeA8PacketPrepareFailureName(
		NativeA8PacketPrepareFailure failure);

	NativeA8PayloadTemplatePtr BuildNativeA8PayloadTemplate(
		std::vector<NativeA8GpuVertex>&& vertices, UInt32 quadCount,
		const A8EffectShapeConfig& effects, const NiBound& bound,
		std::vector<NativeA8CompositeSpan>&& compositeSpans);
	bool InitializeNativeA8ShapePayload(Font& font,
		NiTriShape* facade, const A8ShapeMetadata& metadata,
		NativeA8PayloadTemplatePtr payloadTemplate,
		const NiPoint3& geometryOrigin, NativeA8ShapePayload& payload);
	size_t GetNativeA8PayloadTemplateBytes(
		const NativeA8PayloadTemplate& payloadTemplate);
	size_t GetNativeA8TileRetainedCapacityBytes(
		const NativeA8ShapePayload& payload);
	void InvalidateNativeA8TileRetainedText(
		NativeA8ShapePayload& payload,
		bool preserveStandardPassLite = false);
	bool BuildNativeA8TileRetainedText(NiTriShape* ownerTile,
		NativeA8ShapePayload& payload, UInt32 generation,
		UInt32 atlasTextureEpoch);
	bool IsNativeA8TileRetainedTextCurrent(
		const NativeA8ShapePayload& payload, const NiTriShape* ownerTile,
		UInt32 generation, UInt32 atlasTextureEpoch);
	bool BuildNativeA8StandardPassLiteDispatch(
		NiTriShape* geometry,
		const NativeA8CompiledPacketCommand* program,
		UInt32 generation,
		NativeA8StandardPassLiteDispatch& dispatch);
	bool IsNativeA8StandardPassLiteDispatchCurrent(
		const NativeA8StandardPassLiteDispatch& dispatch,
		const NiTriShape* geometry,
		const NativeA8CompiledPacketCommand* program,
		UInt32 generation);
	void InvalidateNativeA8StandardPassLiteDispatch(
		NativeA8StandardPassLiteDispatch& dispatch);
	void InvalidateNativeA8RingResources(NativeA8FallbackReason reason);

	struct NativeA8RingSubmission
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
	// directly for the duration of its stock Tile render pass. Unlike
	// NativeA8RingSubmission this lease owns no proxy and copies no Tile state:
	// the actual facade remains the render-pass geometry, so its live transform,
	// scissor, alpha, material, cull, and stencil state stay authoritative.
	struct NativeA8DirectShapeSubmission
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
	struct NativeA8DirectFacadePacketBinding
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

	bool EnsureNativeA8ProxyPool(Font& font);
	NativeA8FallbackReason BeginNativeA8DirectShapeSubmission(
		NiTriShape* facade, NativeA8ShapePayload& payload,
		NativeA8DirectShapeSubmission& submission);
	void EndNativeA8DirectShapeSubmission(
		NativeA8DirectShapeSubmission& submission);
	NativeA8FallbackReason ResolveNativeA8DirectFacadePacketBinding(
		NativeA8ShapePayload& payload, UInt32 packetIndex,
		NativeA8DirectFacadePacketBinding& binding);
	bool IsNativeA8DirectFacadePacketBindingCurrent(
		const NativeA8DirectFacadePacketBinding& binding);
	bool IsNativeA8DirectFacadePacketAtlasCurrent(
		const NiTriShape* shape, const NativeA8ShapePayload& payload,
		UInt32 packetIndex);
	NativeA8FallbackReason BeginNativeA8RingSubmission(
		NiTriShape* facade, NativeA8ShapePayload& payload,
		NativeA8RingSubmission& submission);
	NativeA8FallbackReason PrepareNativeA8RingPacket(
		NiTriShape* facade, NativeA8ShapePayload& payload,
		NativeA8RingSubmission& submission, UInt32 packetIndex,
		NiTriShape*& proxyShape);
	NativeA8FallbackReason SkipNativeA8RingPacket(
		NativeA8ShapePayload& payload,
		NativeA8RingSubmission& submission, UInt32 packetIndex);
	void EndNativeA8RingSubmission(NativeA8RingSubmission& submission);
	void ReleaseNativeA8RingResources();
	void PrepareSortedNativeA8Payloads(
		std::vector<NativeA8PayloadTemplatePtr>& payloadTemplates,
		UInt32 generation);
	void EndNativeA8SortedRingFrame();
	bool ResolveNativeA8FramePacketBinding(
		const NativeA8ShapePayload& payload, UInt32 packetIndex,
		NativeA8FramePacketBinding& binding);
	bool ResolveNativeA8FramePayloadBinding(
		const NativeA8ShapePayload& payload,
		NativeA8FramePayloadBinding& binding);
	bool IsNativeA8FramePacketBindingCurrent(
		const NativeA8FramePacketBinding& binding);
	bool IsNativeA8FrameResourceStampCurrent(
		UInt32 generation, UInt32 resourceSerial, UInt32 uploadEpoch);
	void TrimNativeA8CpuCachesForTotalBudget();
	bool FindNativeA8SortedFrameEntry(NiTriShape* facade,
		NativeA8SortedFrameEntryView& view);
	UInt64 GetNativeA8SortedFrameValidationToken();
	UInt64 GetNativeA8SortedNestedTraversalSerial();
	NativeA8VisibilityCull EvaluateNativeA8SubmissionVisibility(
		const NiTriShape* facade);
	NativeA8VisibilityCull EvaluateNativeA8SubmissionVisibility(
		const NiTriShape* facade, const NativeA8ShapePayload& payload);
	void BeginNativeA8VisibilityFrame();
	void CompleteNativeA8VisibilityPreflight();
	void EndNativeA8VisibilityFrame();
	NativeA8VisibilityPreflight EvaluateNativeA8PreflightClipVisibility(
		const NiTriShape* facade);
	NativeA8VisibilityPreflight EvaluateNativeA8PreflightClipVisibility(
		const NiTriShape* facade, const NativeA8ShapePayload& payload);
	bool HonorNativeA8PreflightClipCull(const NiTriShape* facade,
		const NativeA8VisibilityPreflight& preflight);
	bool ReuseNativeA8PreflightClipOverlap(
		const NativeA8VisibilityPreflight& preflight);
	void RecordNativeA8VisibilityCull(NativeA8VisibilityCull reason,
		const NativeA8ShapePayload& payload);
	void RecordNativeA8VisibilityCull(NativeA8VisibilityCull reason);
	UInt32 GetNativeA8AtlasTextureEpoch();
	void NotifyNativeA8AtlasTextureMutation();

	bool InitializeNativeA8Renderer(bool forceAttempt, bool reportFailures);
	void HandleNativeA8RendererMainLoop();
	void HandleNativeA8ShaderLoaderMessage(UInt32 messageType);
	bool IsNativeA8RendererAvailable();
	struct NativeA8RendererReadinessView
	{
		NiDX9Renderer* renderer = nullptr;
		IDirect3DDevice9* device = nullptr;
		UInt32 generation = 0;
		bool ready = false;
	};
	bool GetNativeA8RendererReadinessFast(
		NativeA8RendererReadinessView& view);
	void MarkNativeA8GenerationFault(UInt32 generation,
		const char* operation, HRESULT result);
	UInt32 GetNativeA8ShaderGeneration();
	IDirect3DVertexDeclaration9* GetNativeA8D3DDeclaration(UInt32 generation);
	bool IsNativeA8ShaderGenerationCurrent(UInt32 generation);
	void BeginNativeA8SortedShaderBatch();
	void EndNativeA8SortedShaderBatch();
	void InvalidateNativeA8SortedShaderState();
	void InvalidateNativeA8SortedShaderStateWithinExecutionSegment();
	void AdvanceNativeA8SortedShaderStateAcrossStockTile();
	void ValidateNativeA8SortedShaderStateAfterStockTile();
	UInt64 BeginNativeA8StockLayoutShaderTransition(
		TileShader* shader, UInt32 currentPass);
	bool EndNativeA8StockLayoutShaderTransition(
		UInt64 token, TileShader* shader);
	void BeginNativeA8FacadeShaderBatch();
	void EndNativeA8FacadeShaderBatch();
	TileShader* ResolveNativeA8PacketShader(const NativeA8PacketTemplate& packet,
		const NiTriShape* facade, bool scaledFillSampling,
		bool stockLayoutSdf = false);
	bool RequestNativeA8StockLayoutShapePrecache(NiTriShape* shape,
		TileShader* shader, bool& immediateReady);
	bool IsNativeA8StockLayoutShapeReady(const NiTriShape* shape,
		TileShader* shader);
	bool ResolveNativeA8RetainedPacketProgram(
		const NativeA8PacketTemplate& packet,
		TileShader* shader, UInt32 generation,
		const NativeA8CompiledPacketCommand*& program);
	bool BindNativeA8CommandPacket(
		const NativeA8CompiledPacketCommand& command,
		const void* atlasTexture, bool publishPrograms,
		const NiPropertyState* properties,
		const NativeA8CommandBindState& bindState,
		const char*& operation, HRESULT& result);
	NativeA8FallbackReason PrepareNativeA8Facade(NiTriShape* facade,
		const A8ShapeMetadata& metadata, NativeA8ShapePayload& payload);

	void BeginNativeA8FrameCommandBuffer(BSShaderAccumulator* accumulator,
		UInt64 validationToken, UInt32 generation, UInt32 atlasTextureEpoch);
	void ReserveNativeA8FrameCommandBuffer(size_t ordinaryEntryCount,
		size_t directFacadeCount);
	UInt32 AddNativeA8FrameSinglePacketCommand(NiTriShape* facade,
		const A8ShapeMetadata* metadata, NativeA8ShapePayload* payload);
	UInt32 AddNativeA8FrameDirectFacadeCommand(
		const A8ShapeMetadata* metadata);
	UInt32 AddNativeA8FrameCommandSpan(NiTriShape* facade,
		const A8ShapeMetadata* metadata, NativeA8ShapePayload* payload);
	void ActivateNativeA8FrameCommandBuffer();
	void EndNativeA8FrameCommandBuffer();
	void InvalidateNativeA8CommandExecutionSegment(
		NativeA8CommandFallback reason = NativeA8CommandFallback::State);
	void NotifyNativeA8CommandExternalMutation(
		NativeA8CommandFallback reason);
	void InvalidateNativeA8CommandGeometry(NiTriShape* geometry);
	bool FindNativeA8CommandSpan(UInt32 spanIndex, UInt64 validationToken,
		NativeA8CommandSpanView& view);
	bool BeginNativeA8CommandSpanExecution(UInt32 spanIndex,
		NiTriShape* geometry, NativeA8CommandSpanView& view);
	void EndNativeA8CommandSpanExecution(UInt32 spanIndex,
		bool success, bool drewPacket);
	bool ValidateNativeA8Command(UInt32 spanIndex,
		UInt32 commandOffset, NiTriShape* geometry, NiRenderer* renderer);
	bool GuardNativeA8Command(UInt32 spanIndex,
		UInt32 commandOffset, NiTriShape* geometry, NiRenderer* renderer);
	bool FindNativeA8SinglePacketCommand(UInt32 commandIndex,
		UInt64 validationToken, NativeA8SinglePacketCommandView& view);
	bool BeginNativeA8SinglePacketCommandExecution(UInt32 commandIndex,
		NiTriShape* geometry, NativeA8SinglePacketCommandView& view);
	void EndNativeA8SinglePacketCommandExecution(UInt32 commandIndex,
		bool success, bool drewPacket);
	void AbandonNativeA8SinglePacketCommandExecution(UInt32 commandIndex);
	bool IsNativeA8SinglePacketCommandConsumed(
		UInt32 commandIndex, UInt64 validationToken);
	bool ValidateNativeA8SinglePacketCommand(UInt32 commandIndex,
		NiTriShape* geometry, NiRenderer* renderer);
	bool GuardNativeA8SinglePacketCommand(UInt32 commandIndex,
		NiTriShape* geometry, NiRenderer* renderer);
	bool FindNativeA8DirectFacadeSinglePacketCommand(UInt32 commandIndex,
		UInt64 validationToken,
		NativeA8DirectFacadeSinglePacketCommandView& view);
	bool BeginNativeA8DirectFacadeSinglePacketCommandExecution(
		UInt32 commandIndex, const A8ShapeMetadata* singletonMetadata,
		NiTriShape* geometry,
		NativeA8DirectFacadeSinglePacketCommandView& view);
	void EndNativeA8DirectFacadeSinglePacketCommandExecution(
		UInt32 commandIndex, bool success, bool drewPacket);
	void AbandonNativeA8DirectFacadeSinglePacketCommandExecution(
		UInt32 commandIndex);
	bool IsNativeA8DirectFacadeSinglePacketCommandConsumed(
		UInt32 commandIndex, UInt64 validationToken);
	bool ValidateNativeA8DirectFacadeSinglePacketCommand(UInt32 commandIndex,
		NiTriShape* geometry, NiRenderer* renderer);
	bool GuardNativeA8DirectFacadeSinglePacketCommand(UInt32 commandIndex,
		NiTriShape* geometry, NiRenderer* renderer);
	void RecordNativeA8CommandFallback(NativeA8CommandFallback reason);

	bool HookNativeA8Accumulator();
	bool IsNativeA8AccumulatorHookCurrent();
	bool IsNativeA8RenderAlphaGeometryHookCurrent();
	bool IsNativeA8RegistrationHookChainCurrent();
	bool IsNativeA8RegistrationHookChainCurrentFast();

	void RecordNativeA8Suppression(NiTriShape* shape,
		const A8ShapeMetadata& metadata, NativeA8FallbackReason reason,
		const char* phase);
	void MarkNativeA8RuntimeFault(const A8ShapeMetadata& metadata,
		NativeA8ShapePayload& payload,
		NativeA8FallbackReason reason);

}

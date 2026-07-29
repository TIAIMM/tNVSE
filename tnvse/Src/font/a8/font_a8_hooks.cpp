#include "font_a8_internal.h"

#include "load_config.h"
#include "tnvse.h"

#include "NiGeometryBufferData.hpp"
#include "NiRenderer.hpp"
#include "NiTriShapeData.hpp"
#include "NiVBChip.hpp"
#include "Utils/SafeWrite.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <optional>

namespace fonthook::vectorfont
{
	namespace
	{
		struct A8MetadataHotEntry
		{
			const NiTriShape* shape = nullptr;
			UInt64 generation = 0;
			std::weak_ptr<const A8ShapeMetadata> metadata;
		};

		inline constexpr size_t kMetadataHotWayCount = 8;
		inline constexpr size_t kMetadataHotSetCount = 2048;
		static_assert((kMetadataHotSetCount & (kMetadataHotSetCount - 1)) == 0,
			"metadata hot-cache set count must remain a power of two");

		struct A8MetadataHotSet
		{
			std::array<A8MetadataHotEntry, kMetadataHotWayCount> ways;
			UInt8 nextVictim = 0;
		};

		// Eight ways keep allocator-neighbouring facades from evicting one another,
		// while weak ownership preserves menu/shape destruction semantics. 16384
		// entries cover the observed multi-page Pip-Boy working set without adding
		// any process-wide locks to the render path.
		thread_local std::unique_ptr<
			std::array<A8MetadataHotSet, kMetadataHotSetCount>>
			s_metadataHotSets;

		A8MetadataHotSet& GetMetadataHotSet(const NiTriShape* shape)
		{
			if (!s_metadataHotSets)
			{
				s_metadataHotSets = std::make_unique<
					std::array<A8MetadataHotSet, kMetadataHotSetCount>>();
			}
			const size_t index = HashMetadataShapeAddress(shape)
				& (kMetadataHotSetCount - 1);
			return (*s_metadataHotSets)[index];
		}

		A8MetadataHotEntry& SelectMetadataHotVictim(A8MetadataHotSet& set)
		{
			for (A8MetadataHotEntry& entry : set.ways)
			{
				if (!entry.shape)
					return entry;
			}
			for (A8MetadataHotEntry& entry : set.ways)
			{
				if (entry.metadata.expired())
				{
					entry = {};
					return entry;
				}
			}
			A8MetadataHotEntry& victim =
				set.ways[set.nextVictim % kMetadataHotWayCount];
			set.nextVictim = static_cast<UInt8>(
				(set.nextVictim + 1) % kMetadataHotWayCount);
			victim = {};
			return victim;
		}

		class NativePixelConstantScope
		{
		public:
			// The native profile mirrors the stock Tile value into c0 for its final
			// packet and deliberately leaves it there, matching an ordinary Tile
			// draw. tNVSE-owned pixel c1-c8 and vertex c4 need isolation from the
			// next shader; VS c4 carries viewport/raster data for analytic AA.
			static constexpr UINT kFirstRegister = 1;
			static constexpr UINT kRegisterCount =
				static_cast<UINT>(kNativeA8PacketConstantRegisterCount);
			static constexpr size_t kFloatCount = kRegisterCount * 4;
			static constexpr UINT kVertexRegister =
				static_cast<UINT>(kNativeA8VertexAaConstantRegister);
			static constexpr UINT kVertexRegisterCount = 1;
			static constexpr size_t kVertexFloatCount = 4;

			explicit NativePixelConstantScope(IDirect3DDevice9* device)
				: m_device(device)
			{
				if (!m_device)
				{
					m_result = D3DERR_INVALIDCALL;
					m_operation = "capture-pixel-constants";
					return;
				}
				m_result = m_device->GetPixelShaderConstantF(kFirstRegister,
					m_original.data(), kRegisterCount);
				if (FAILED(m_result))
				{
					m_operation = "capture-pixel-constants";
					return;
				}
				m_result = m_device->GetVertexShaderConstantF(kVertexRegister,
					m_originalVertex.data(), kVertexRegisterCount);
				if (FAILED(m_result))
				{
					m_operation = "capture-vertex-aa-constant";
					return;
				}
				m_captured = true;
				m_operation = "none";
			}

			~NativePixelConstantScope()
			{
				if (m_captured && !m_finished)
					RestoreAndVerify();
			}

			bool Captured() const { return m_captured; }
			HRESULT Result() const { return m_result; }
			const char* Operation() const { return m_operation; }
			SInt32 MismatchRegister() const { return m_mismatchRegister; }

			bool RestoreAndVerify()
			{
				if (!m_captured)
					return false;
				if (m_finished)
					return SUCCEEDED(m_result);
				m_finished = true;

				m_result = m_device->SetPixelShaderConstantF(kFirstRegister,
					m_original.data(), kRegisterCount);
				if (FAILED(m_result))
				{
					m_operation = "restore-pixel-constants";
					return false;
				}
				m_result = m_device->SetVertexShaderConstantF(kVertexRegister,
					m_originalVertex.data(), kVertexRegisterCount);
				if (FAILED(m_result))
				{
					m_operation = "restore-vertex-aa-constant";
					return false;
				}
				// The Set calls already report a failed restore. Readback is useful
				// for diagnostics but forces driver round trips, so retain it only
				// in detailed logging mode.
				if (!g_bEnableFreeTypeFontRenderingLog)
				{
					m_operation = "none";
					m_result = D3D_OK;
					return true;
				}
				m_result = m_device->GetPixelShaderConstantF(kFirstRegister,
					m_verify.data(), kRegisterCount);
				if (FAILED(m_result))
				{
					m_operation = "verify-pixel-constants";
					return false;
				}
				for (size_t index = 0; index < kFloatCount; ++index)
				{
					if (std::memcmp(&m_original[index], &m_verify[index],
						sizeof(float)) != 0)
					{
						m_operation = "pixel-constant-mismatch";
						m_mismatchRegister = static_cast<SInt32>(
							kFirstRegister + index / 4);
						m_result = E_FAIL;
						return false;
					}
				}
				m_result = m_device->GetVertexShaderConstantF(kVertexRegister,
					m_verifyVertex.data(), kVertexRegisterCount);
				if (FAILED(m_result))
				{
					m_operation = "verify-vertex-aa-constant";
					return false;
				}
				for (size_t index = 0; index < kVertexFloatCount; ++index)
				{
					if (std::memcmp(&m_originalVertex[index],
						&m_verifyVertex[index], sizeof(float)) != 0)
					{
						m_operation = "vertex-aa-constant-mismatch";
						m_mismatchRegister =
							static_cast<SInt32>(kVertexRegister);
						m_result = E_FAIL;
						return false;
					}
				}
				m_operation = "none";
				m_result = D3D_OK;
				return true;
			}

		private:
			IDirect3DDevice9* m_device = nullptr;
			std::array<float, kFloatCount> m_original = {};
			std::array<float, kFloatCount> m_verify = {};
			std::array<float, kVertexFloatCount> m_originalVertex = {};
			std::array<float, kVertexFloatCount> m_verifyVertex = {};
			HRESULT m_result = D3DERR_INVALIDCALL;
			const char* m_operation = "capture-pixel-constants";
			SInt32 m_mismatchRegister = -1;
			bool m_captured = false;
			bool m_finished = false;
		};

		class NativePixelConstantBatch
		{
		public:
			void BeginFrame()
			{
				m_frameActive = true;
			}

			bool FrameActive() const
			{
				return m_frameActive;
			}

			bool EnsureCaptured(IDirect3DDevice9* device)
			{
				if (!m_frameActive || !device)
					return SetFailure("capture-pixel-constants",
						D3DERR_INVALIDCALL);
				if (m_captured)
				{
					if (m_device == device)
					{
						RecordFreeTypePerf(
							FreeTypePerfCounter::ConstantBatchReuse);
						return true;
					}
					if (!Flush())
						return false;
				}
				return Capture(device);
			}

			bool Flush()
			{
				if (!m_captured)
					return true;
				m_captured = false;
				RecordFreeTypePerf(
					FreeTypePerfCounter::ConstantBatchFlush);
				m_result = m_device->SetPixelShaderConstantF(
					NativePixelConstantScope::kFirstRegister,
					m_original.data(),
					NativePixelConstantScope::kRegisterCount);
				if (FAILED(m_result))
					return SetFailure("restore-pixel-constants", m_result);
				m_result = m_device->SetVertexShaderConstantF(
					NativePixelConstantScope::kVertexRegister,
					m_originalVertex.data(),
					NativePixelConstantScope::kVertexRegisterCount);
				if (FAILED(m_result))
					return SetFailure(
						"restore-vertex-aa-constant", m_result);
				if (g_bEnableFreeTypeFontRenderingLog)
				{
					m_result = m_device->GetPixelShaderConstantF(
						NativePixelConstantScope::kFirstRegister,
						m_verify.data(),
						NativePixelConstantScope::kRegisterCount);
					if (FAILED(m_result))
						return SetFailure("verify-pixel-constants", m_result);
					for (size_t index = 0;
						index < NativePixelConstantScope::kFloatCount; ++index)
					{
						if (std::memcmp(&m_original[index], &m_verify[index],
							sizeof(float)) != 0)
						{
							m_mismatchRegister = static_cast<SInt32>(
								NativePixelConstantScope::kFirstRegister
									+ index / 4);
							return SetFailure(
								"pixel-constant-mismatch", E_FAIL);
						}
					}
					m_result = m_device->GetVertexShaderConstantF(
						NativePixelConstantScope::kVertexRegister,
						m_verifyVertex.data(),
						NativePixelConstantScope::kVertexRegisterCount);
					if (FAILED(m_result))
						return SetFailure(
							"verify-vertex-aa-constant", m_result);
					for (size_t index = 0;
						index < NativePixelConstantScope::kVertexFloatCount;
						++index)
					{
						if (std::memcmp(&m_originalVertex[index],
							&m_verifyVertex[index], sizeof(float)) != 0)
						{
							m_mismatchRegister = static_cast<SInt32>(
								NativePixelConstantScope::kVertexRegister);
							return SetFailure(
								"vertex-aa-constant-mismatch", E_FAIL);
						}
					}
				}
				m_device = nullptr;
				m_operation = "none";
				m_result = D3D_OK;
				m_mismatchRegister = -1;
				return true;
			}

			void EndFrame()
			{
				m_frameActive = false;
			}

			HRESULT Result() const { return m_result; }
			const char* Operation() const { return m_operation; }
			SInt32 MismatchRegister() const { return m_mismatchRegister; }
			UInt32 Generation() const { return m_generation; }

		private:
			bool Capture(IDirect3DDevice9* device)
			{
				m_device = device;
				m_generation = GetNativeA8ShaderGeneration();
				m_mismatchRegister = -1;
				m_result = device->GetPixelShaderConstantF(
					NativePixelConstantScope::kFirstRegister,
					m_original.data(),
					NativePixelConstantScope::kRegisterCount);
				if (FAILED(m_result))
					return SetFailure("capture-pixel-constants", m_result);
				m_result = device->GetVertexShaderConstantF(
					NativePixelConstantScope::kVertexRegister,
					m_originalVertex.data(),
					NativePixelConstantScope::kVertexRegisterCount);
				if (FAILED(m_result))
					return SetFailure(
						"capture-vertex-aa-constant", m_result);
				m_captured = true;
				RecordFreeTypePerf(
					FreeTypePerfCounter::ConstantBatchCapture);
				m_operation = "none";
				return true;
			}

			bool SetFailure(const char* operation, HRESULT result)
			{
				m_operation = operation;
				m_result = result;
				m_device = nullptr;
				m_captured = false;
				return false;
			}

			IDirect3DDevice9* m_device = nullptr;
			std::array<float, NativePixelConstantScope::kFloatCount> m_original = {};
			std::array<float, NativePixelConstantScope::kFloatCount> m_verify = {};
			std::array<float,
				NativePixelConstantScope::kVertexFloatCount>
				m_originalVertex = {};
			std::array<float,
				NativePixelConstantScope::kVertexFloatCount>
				m_verifyVertex = {};
			HRESULT m_result = D3D_OK;
			const char* m_operation = "none";
			SInt32 m_mismatchRegister = -1;
			UInt32 m_generation = 0;
			bool m_frameActive = false;
			bool m_captured = false;
		};

		thread_local NativePixelConstantBatch s_pixelConstantBatch;

		bool FlushNativePixelConstantBatch(const char* phase)
		{
			if (s_pixelConstantBatch.Flush())
				return true;
			MarkNativeA8GenerationFault(s_pixelConstantBatch.Generation(),
				s_pixelConstantBatch.Operation(),
				s_pixelConstantBatch.Result());
			gLog.FormattedMessage(
				"tnvse_freetype_native: batched shader-constant isolation fault phase=%s operation=%s hr=0x%08X register=%d generation=%u",
				phase ? phase : "unknown",
				s_pixelConstantBatch.Operation(),
				static_cast<UInt32>(s_pixelConstantBatch.Result()),
				s_pixelConstantBatch.MismatchRegister(),
				s_pixelConstantBatch.Generation());
			return false;
		}

		class NativeTilePacketScope
		{
		public:
			explicit NativeTilePacketScope(BSShaderProperty::RenderPass* pass)
				: m_pass(pass), m_facade(pass ? pass->pGeometry : nullptr)
			{
			}

			~NativeTilePacketScope()
			{
				if (m_pass)
					m_pass->pGeometry = m_facade;
			}

			void Select(NiGeometry* geometry)
			{
				m_pass->pGeometry = geometry;
			}

		private:
			BSShaderProperty::RenderPass* m_pass = nullptr;
			NiGeometry* m_facade = nullptr;
		};

		class NativeRingSubmissionScope
		{
		public:
			~NativeRingSubmissionScope()
			{
				EndNativeA8RingSubmission(submission);
			}

			NativeA8RingSubmission submission;
		};

		class NativeDirectShapeSubmissionScope
		{
		public:
			~NativeDirectShapeSubmissionScope()
			{
				EndNativeA8DirectShapeSubmission(submission);
			}

			NativeA8DirectShapeSubmission submission;
		};

		struct NativeDirectImmediateContext
		{
			NiTriShape* shape = nullptr;
			bool invoked = false;
		};

		thread_local NativeDirectImmediateContext*
			s_nativeDirectImmediateContext = nullptr;

		class NativeDirectImmediateScope
		{
		public:
			explicit NativeDirectImmediateScope(NiTriShape* shape)
				: m_previous(s_nativeDirectImmediateContext)
			{
				m_context.shape = shape;
				s_nativeDirectImmediateContext = &m_context;
			}

			~NativeDirectImmediateScope()
			{
				s_nativeDirectImmediateContext = m_previous;
			}

			bool Invoked() const
			{
				return m_context.invoked;
			}

		private:
			NativeDirectImmediateContext m_context;
			NativeDirectImmediateContext* m_previous = nullptr;
		};

		class NativeFacadeShaderBatchScope
		{
		public:
			NativeFacadeShaderBatchScope()
			{
				BeginNativeA8FacadeShaderBatch();
			}

			~NativeFacadeShaderBatchScope()
			{
				EndNativeA8FacadeShaderBatch();
			}
		};

		void ApplyNativeGeometryOrigin(NiTransform& destination,
			const NiTransform& source, const NiPoint3& origin)
		{
			destination = source;
			if (origin.x != 0.0f || origin.y != 0.0f || origin.z != 0.0f)
				destination.m_Translate = source * origin;
		}

		class NativeDirectShapeBinding
		{
		public:
			NativeDirectShapeBinding(NiTriShape* shape,
				NativeA8ShapePayload& payload,
				const NativeA8DirectShapeSubmission& submission)
				: m_shape(shape)
			{
				if (!m_shape || m_shape->GetSkinInstance()
					|| !payload.payloadTemplate
					|| payload.packetShaders.size() != 1
					|| !payload.packetShaders[0]
					|| !submission.vertexBuffer || !submission.indexBuffer
					|| !submission.declaration || !submission.vertexCount
					|| (submission.vertexCount & 3u))
				{
					return;
				}
				const std::vector<NativeA8PacketTemplate>& packets =
					GetNativeA8Packets(*payload.payloadTemplate,
						payload.useCompositePackets);
				if (packets.size() != 1
					|| packets[0].vertexCount != submission.vertexCount)
				{
					return;
				}

				m_data = m_shape->GetModelData();
				m_alpha = m_shape->GetAlphaProperty();
				m_buffer = m_data ? m_data->m_pkBuffData : nullptr;
				if (!m_data || !m_alpha || !m_buffer
					|| !m_buffer->m_uiStreamCount
					|| !m_buffer->m_puiVertexStride
					|| !m_buffer->m_ppkVBChip
					|| !m_buffer->m_ppkVBChip[0])
				{
					return;
				}
				m_chip = m_buffer->m_ppkVBChip[0];

				m_local = m_shape->m_kLocal;
				m_world = m_shape->m_kWorld;
				m_bound = m_data->m_kBound;
				m_shader = m_shape->GetShader();
				m_alphaFlags = m_alpha->m_usFlags;
				m_alphaTestRef = m_alpha->m_ucAlphaTestRef;

				m_bufferFlags = m_buffer->m_uiFlags;
				m_geometryGroup = m_buffer->m_pkGeometryGroup;
				m_fvf = m_buffer->m_uiFVF;
				m_declaration = m_buffer->m_hDeclaration;
				m_softwareVertexProcessing =
					m_buffer->m_bSoftwareVP;
				m_vertexCount = m_buffer->m_uiVertCount;
				m_maxVertexCount = m_buffer->m_uiMaxVertCount;
				m_streamCount = m_buffer->m_uiStreamCount;
				m_stride = m_buffer->m_puiVertexStride[0];
				m_indexCount = m_buffer->m_uiIndexCount;
				m_indexBytes = m_buffer->m_uiIBSize;
				m_indexBuffer = m_buffer->m_pkIB;
				m_baseVertex = m_buffer->m_uiBaseVertexIndex;
				m_primitiveType = m_buffer->m_eType;
				m_triangleCount = m_buffer->m_uiTriCount;
				m_maxTriangleCount = m_buffer->m_uiMaxTriCount;
				m_arrayCount = m_buffer->m_uiNumArrays;
				m_arrayLengths = m_buffer->m_pusArrayLengths;
				m_indexArray = m_buffer->m_pusIndexArray;

				m_chipIndex = m_chip->m_uiIndex;
				m_vertexBuffer = m_chip->m_pkVB;
				m_chipOffset = m_chip->m_uiOffset;
				m_chipLockFlags = m_chip->m_uiLockFlags;
				m_chipSize = m_chip->m_uiSize;

				const UInt32 quadCount = submission.vertexCount / 4u;
				ApplyNativeGeometryOrigin(m_shape->m_kLocal, m_local,
					payload.geometryOrigin);
				ApplyNativeGeometryOrigin(m_shape->m_kWorld, m_world,
					payload.geometryOrigin);
				m_data->m_kBound = packets[0].bound;
				m_alpha->SetAlphaTesting(false);
				m_shape->SetShader(payload.packetShaders[0]);

				m_buffer->m_uiFlags = 0;
				m_buffer->m_pkGeometryGroup = nullptr;
				m_buffer->m_uiFVF = 0;
				m_buffer->m_hDeclaration = submission.declaration;
				m_buffer->m_bSoftwareVP = false;
				m_buffer->m_uiVertCount = submission.vertexCount;
				m_buffer->m_uiMaxVertCount = submission.vertexCount;
				m_buffer->m_uiStreamCount = 1;
				m_buffer->m_puiVertexStride[0] =
					sizeof(NativeA8GpuVertex);
				m_buffer->m_uiIndexCount = quadCount * 6u;
				m_buffer->m_uiIBSize = submission.indexBytes;
				m_buffer->m_pkIB = submission.indexBuffer;
				m_buffer->m_uiBaseVertexIndex = submission.baseVertex;
				m_buffer->m_eType = D3DPT_TRIANGLELIST;
				m_buffer->m_uiTriCount = quadCount * 2u;
				m_buffer->m_uiMaxTriCount = quadCount * 2u;
				m_buffer->m_uiNumArrays = 1;
				m_buffer->m_pusArrayLengths = nullptr;
				m_buffer->m_pusIndexArray = nullptr;

				m_chip->m_uiIndex = 0;
				m_chip->m_pkVB = submission.vertexBuffer;
				m_chip->m_uiOffset = 0;
				m_chip->m_uiLockFlags = 0;
				m_chip->m_uiSize = submission.vertexCount
					* sizeof(NativeA8GpuVertex);

				if (m_shape->GetShader() != payload.packetShaders[0])
				{
					Restore();
					return;
				}
				m_active = true;
			}

			~NativeDirectShapeBinding()
			{
				if (m_active)
					Restore();
			}

			bool Active() const
			{
				return m_active;
			}

		private:
			void Restore()
			{
				if (!m_shape || !m_data || !m_alpha || !m_buffer || !m_chip)
					return;
				m_chip->m_uiIndex = m_chipIndex;
				m_chip->m_pkVB = m_vertexBuffer;
				m_chip->m_uiOffset = m_chipOffset;
				m_chip->m_uiLockFlags = m_chipLockFlags;
				m_chip->m_uiSize = m_chipSize;

				m_buffer->m_uiFlags = m_bufferFlags;
				m_buffer->m_pkGeometryGroup = m_geometryGroup;
				m_buffer->m_uiFVF = m_fvf;
				m_buffer->m_hDeclaration = m_declaration;
				m_buffer->m_bSoftwareVP =
					m_softwareVertexProcessing;
				m_buffer->m_uiVertCount = m_vertexCount;
				m_buffer->m_uiMaxVertCount = m_maxVertexCount;
				m_buffer->m_uiStreamCount = m_streamCount;
				m_buffer->m_puiVertexStride[0] = m_stride;
				m_buffer->m_uiIndexCount = m_indexCount;
				m_buffer->m_uiIBSize = m_indexBytes;
				m_buffer->m_pkIB = m_indexBuffer;
				m_buffer->m_uiBaseVertexIndex = m_baseVertex;
				m_buffer->m_eType = m_primitiveType;
				m_buffer->m_uiTriCount = m_triangleCount;
				m_buffer->m_uiMaxTriCount = m_maxTriangleCount;
				m_buffer->m_uiNumArrays = m_arrayCount;
				m_buffer->m_pusArrayLengths = m_arrayLengths;
				m_buffer->m_pusIndexArray = m_indexArray;

				m_shape->SetShader(m_shader);
				m_alpha->m_usFlags = m_alphaFlags;
				m_alpha->m_ucAlphaTestRef = m_alphaTestRef;
				m_data->m_kBound = m_bound;
				m_shape->m_kLocal = m_local;
				m_shape->m_kWorld = m_world;
				m_active = false;
			}

			NiTriShape* m_shape = nullptr;
			NiTriShapeData* m_data = nullptr;
			NiAlphaProperty* m_alpha = nullptr;
			NiGeometryBufferData* m_buffer = nullptr;
			NiVBChip* m_chip = nullptr;
			NiTransform m_local;
			NiTransform m_world;
			NiBound m_bound;
			BSShader* m_shader = nullptr;
			Bitfield16 m_alphaFlags;
			UInt8 m_alphaTestRef = 0;
			UInt32 m_bufferFlags = 0;
			NiGeometryGroup* m_geometryGroup = nullptr;
			UInt32 m_fvf = 0;
			void* m_declaration = nullptr;
			bool m_softwareVertexProcessing = false;
			UInt32 m_vertexCount = 0;
			UInt32 m_maxVertexCount = 0;
			UInt32 m_streamCount = 0;
			UInt32 m_stride = 0;
			UInt32 m_indexCount = 0;
			UInt32 m_indexBytes = 0;
			IDirect3DIndexBuffer9* m_indexBuffer = nullptr;
			UInt32 m_baseVertex = 0;
			D3DPRIMITIVETYPE m_primitiveType = D3DPT_FORCE_DWORD;
			UInt32 m_triangleCount = 0;
			UInt32 m_maxTriangleCount = 0;
			UInt32 m_arrayCount = 0;
			const UInt16* m_arrayLengths = nullptr;
			const UInt16* m_indexArray = nullptr;
			UInt32 m_chipIndex = 0;
			IDirect3DVertexBuffer9* m_vertexBuffer = nullptr;
			UInt32 m_chipOffset = 0;
			UInt32 m_chipLockFlags = 0;
			UInt32 m_chipSize = 0;
			bool m_active = false;
		};

		struct NativePacketDrawResult
		{
			bool runtimeFault = false;
			bool drewPacket = false;
			bool directShapeRoute = false;
			bool stockLikeBitmapRoute = false;
			bool constantStateFault = false;
			NativeA8FallbackReason failure =
				NativeA8FallbackReason::RuntimeFault;
			const char* operation = "generation-changed-after-packet";
			HRESULT result = D3DERR_DEVICELOST;
			SInt32 mismatchRegister = -1;
		};

		NativePacketDrawResult DrawNativePacketSet(
			BSShaderProperty::RenderPass* pass, UInt32 currentPass,
			bool setupDrawmode, NiTriShape* facade,
			NativeA8ShapePayload& payload)
		{
			FreeTypePerfScope perf(
				FreeTypePerfPhase::Submit);
			NativePacketDrawResult draw;
			NativeRingSubmissionScope ringScope;
			NativeA8FallbackReason failure = BeginNativeA8RingSubmission(
				facade, payload, ringScope.submission);
			if (failure != NativeA8FallbackReason::None)
			{
				draw.runtimeFault = true;
				draw.failure = failure;
				draw.operation = "ring-submission";
			}
			NativeTilePacketScope packetScope(pass);
			draw.stockLikeBitmapRoute =
				payload.stockLikeBitmapPackets;
			NiDX9Renderer* renderer = draw.stockLikeBitmapRoute
				? nullptr : NiDX9Renderer::GetSingleton();
			IDirect3DDevice9* device = renderer
				? renderer->GetD3DDevice() : nullptr;
			if (!draw.stockLikeBitmapRoute && !device)
			{
				draw.runtimeFault = true;
				draw.constantStateFault = true;
				draw.operation = "capture-pixel-constants";
				draw.result = D3DERR_DEVICELOST;
			}
			// Retail NiDX9Renderer's indexed-array loop cannot express
			// triangle-list page boundaries: m_pusArrayLengths is interpreted as
			// strip lengths (primitiveCount = length - 2). Keep the page split at
			// the one stock sorted Tile callsite instead. Each packet selects a
			// Font::MakeTriShape proxy and then executes the untouched
			// TileRenderPass -> NiTriShape::RenderImmediate renderer path.
			//
			// Final ARGB and baked-coverage bitmaps use only c0. Skipping the
			// distance-field c1-c8/VS-c4 isolation and facade bookkeeping removes
			// the only per-facade driver readback/writeback from this stock-like
			// multipage route.
			const bool isolatePacketConstants =
				!draw.stockLikeBitmapRoute;
			const bool batchedConstants = isolatePacketConstants
				&& s_pixelConstantBatch.FrameActive();
			std::optional<NativePixelConstantScope> localConstants;
			std::optional<NativeFacadeShaderBatchScope> shaderBatch;
			if (!draw.runtimeFault)
			{
				if (isolatePacketConstants)
					shaderBatch.emplace();
				// Retail 0xB64F90 calls 0xB994F0 once per sorted item with no
				// intervening draw. Capture PS c1-c8 and VS c4 once for that batch and
				// preserve the local scope for direct/non-sorted submissions.
				if (batchedConstants)
				{
					if (!s_pixelConstantBatch.EnsureCaptured(device))
					{
						draw.runtimeFault = true;
						draw.constantStateFault = true;
						draw.operation = s_pixelConstantBatch.Operation();
						draw.result = s_pixelConstantBatch.Result();
						draw.mismatchRegister =
							s_pixelConstantBatch.MismatchRegister();
					}
				}
				else if (isolatePacketConstants)
				{
					localConstants.emplace(device);
					if (!localConstants->Captured())
					{
						draw.runtimeFault = true;
						draw.constantStateFault = true;
						draw.operation = localConstants->Operation();
						draw.result = localConstants->Result();
					}
				}
				for (UInt32 packetIndex = 0; !draw.runtimeFault
					&& packetIndex < payload.packetShaders.size(); ++packetIndex)
				{
					NiTriShape* proxyShape = nullptr;
					const NativeA8FallbackReason packetFailure =
						PrepareNativeA8RingPacket(facade, payload,
							ringScope.submission, packetIndex, proxyShape);
					if (packetFailure != NativeA8FallbackReason::None
						|| !proxyShape)
					{
						draw.runtimeFault = true;
						draw.failure =
							packetFailure != NativeA8FallbackReason::None
								? packetFailure
								: NativeA8FallbackReason::RuntimeFault;
						draw.operation = "ring-packet";
						break;
					}
					packetScope.Select(proxyShape);
					State().originalTileRenderPass(pass, currentPass, false,
						true, setupDrawmode);
					draw.drewPacket = true;
					RecordFreeTypePerf(FreeTypePerfCounter::TilePass);
					const std::vector<NativeA8PacketTemplate>& activePackets =
						GetNativeA8Packets(*payload.payloadTemplate,
							payload.useCompositePackets);
					if (packetIndex < activePackets.size()
						&& activePackets[packetIndex].shaderClass
							== NativeA8ShaderClass::Composite)
					{
						RecordFreeTypePerf(
							FreeTypePerfCounter::CompositeDraw);
					}
					if (!IsNativeA8ShaderGenerationCurrent(
						payload.preparedGeneration))
					{
						draw.runtimeFault = true;
						break;
					}
				}
				if (isolatePacketConstants && !batchedConstants
					&& localConstants
					&& !localConstants->RestoreAndVerify())
				{
					draw.runtimeFault = true;
					draw.constantStateFault = true;
					draw.operation = localConstants->Operation();
					draw.result = localConstants->Result();
					draw.mismatchRegister =
						localConstants->MismatchRegister();
				}
			}
			if (isolatePacketConstants && batchedConstants && draw.runtimeFault
				&& !FlushNativePixelConstantBatch("native-runtime-fault"))
			{
				draw.constantStateFault = true;
				draw.operation = s_pixelConstantBatch.Operation();
				draw.result = s_pixelConstantBatch.Result();
				draw.mismatchRegister =
					s_pixelConstantBatch.MismatchRegister();
			}
			return draw;
		}

		bool TryDrawNativeSinglePacketDirect(
			BSShaderProperty::RenderPass* pass, UInt32 currentPass,
			bool setupDrawmode, NiTriShape* facade,
			NativeA8ShapePayload& payload,
			NativePacketDrawResult& draw)
		{
			if (!pass || !facade || !payload.buildComplete
				|| !payload.payloadTemplate
				|| payload.packetShaders.size() != 1)
			{
				return false;
			}
			const NativeA8PayloadTemplate& artifact =
				*payload.payloadTemplate;
			const std::vector<NativeA8PacketTemplate>& packets =
				GetNativeA8Packets(artifact, payload.useCompositePackets);
			if (artifact.pageCount != 1 || packets.size() != 1
				|| packets[0].atlasPage != 0
				|| packets[0].firstVertex != 0
				|| packets[0].vertexCount != artifact.gpuVertices.size())
			{
				return false;
			}

			FreeTypePerfScope perf(FreeTypePerfPhase::Submit);
			RecordFreeTypePerf(
				FreeTypePerfCounter::SinglePacketDirectCandidate);
			NativeDirectShapeSubmissionScope submissionScope;
			const NativeA8FallbackReason submissionFailure =
				BeginNativeA8DirectShapeSubmission(facade, payload,
					submissionScope.submission);
			if (submissionFailure != NativeA8FallbackReason::None)
			{
				RecordFreeTypePerf(
					FreeTypePerfCounter::SinglePacketDirectFallback);
				return false;
			}

			NativeDirectShapeBinding binding(facade, payload,
				submissionScope.submission);
			if (!binding.Active())
			{
				RecordFreeTypePerf(
					FreeTypePerfCounter::SinglePacketDirectFallback);
				return false;
			}

			draw.directShapeRoute = true;
			draw.stockLikeBitmapRoute =
				payload.stockLikeBitmapPackets;
			NiDX9Renderer* renderer = draw.stockLikeBitmapRoute
				? nullptr : NiDX9Renderer::GetSingleton();
			IDirect3DDevice9* device = renderer
				? renderer->GetD3DDevice() : nullptr;
			if (!draw.stockLikeBitmapRoute && !device)
			{
				draw.runtimeFault = true;
				draw.constantStateFault = true;
				draw.operation = "capture-pixel-constants";
				draw.result = D3DERR_DEVICELOST;
			}

			const bool isolatePacketConstants =
				!draw.stockLikeBitmapRoute;
			const bool batchedConstants = isolatePacketConstants
				&& s_pixelConstantBatch.FrameActive();
			std::optional<NativePixelConstantScope> localConstants;
			std::optional<NativeFacadeShaderBatchScope> shaderBatch;
			if (!draw.runtimeFault)
			{
				if (isolatePacketConstants)
					shaderBatch.emplace();
				if (batchedConstants)
				{
					if (!s_pixelConstantBatch.EnsureCaptured(device))
					{
						draw.runtimeFault = true;
						draw.constantStateFault = true;
						draw.operation = s_pixelConstantBatch.Operation();
						draw.result = s_pixelConstantBatch.Result();
						draw.mismatchRegister =
							s_pixelConstantBatch.MismatchRegister();
					}
				}
				else if (isolatePacketConstants)
				{
					localConstants.emplace(device);
					if (!localConstants->Captured())
					{
						draw.runtimeFault = true;
						draw.constantStateFault = true;
						draw.operation = localConstants->Operation();
						draw.result = localConstants->Result();
					}
				}
			}

			if (!draw.runtimeFault)
			{
				NativeDirectImmediateScope immediateScope(facade);
				State().originalTileRenderPass(pass, currentPass, false,
					true, setupDrawmode);
				if (immediateScope.Invoked())
				{
					draw.drewPacket = true;
					RecordFreeTypePerf(
						FreeTypePerfCounter::SinglePacketDirectDraw);
					RecordFreeTypePerf(
						FreeTypePerfCounter::SinglePacketDirectVertex,
						submissionScope.submission.vertexCount);
					RecordFreeTypePerf(FreeTypePerfCounter::TilePass);
					if (packets[0].shaderClass
						== NativeA8ShaderClass::Composite)
					{
						RecordFreeTypePerf(
							FreeTypePerfCounter::CompositeDraw);
					}
				}
				else
				{
					draw.runtimeFault = true;
					draw.failure = NativeA8FallbackReason::RuntimeFault;
					draw.operation = "direct-shape-immediate-not-invoked";
					draw.result = E_FAIL;
				}
				if (!IsNativeA8ShaderGenerationCurrent(
					payload.preparedGeneration))
				{
					draw.runtimeFault = true;
					draw.failure = NativeA8FallbackReason::RuntimeFault;
					draw.operation =
						"generation-changed-after-direct-shape";
					draw.result = D3DERR_DEVICELOST;
				}
			}

			if (isolatePacketConstants && !batchedConstants
				&& localConstants
				&& !localConstants->RestoreAndVerify())
			{
				draw.runtimeFault = true;
				draw.constantStateFault = true;
				draw.operation = localConstants->Operation();
				draw.result = localConstants->Result();
				draw.mismatchRegister =
					localConstants->MismatchRegister();
			}
			if (isolatePacketConstants && batchedConstants
				&& draw.runtimeFault
				&& !FlushNativePixelConstantBatch(
					"direct-shape-runtime-fault"))
			{
				draw.constantStateFault = true;
				draw.operation = s_pixelConstantBatch.Operation();
				draw.result = s_pixelConstantBatch.Result();
				draw.mismatchRegister =
					s_pixelConstantBatch.MismatchRegister();
			}
			if (draw.runtimeFault)
			{
				RecordFreeTypePerf(
					FreeTypePerfCounter::SinglePacketDirectFallback);
			}
			// Once the stock Tile pass has been entered, this route owns the item
			// even if the immediate callback was unexpectedly skipped. Replaying
			// through a proxy could duplicate a draw whose driver result is opaque.
			return true;
		}

		void LogMissingMetadata(NiTriShape* shape, const char* phase)
		{
			if (!g_bEnableFreeTypeFontRenderingLog)
				return;
			static std::atomic<UInt32> logCount = 0;
			constexpr UInt32 kMaximumLogs = 8;
			const UInt32 ordinal = logCount.fetch_add(1, std::memory_order_relaxed);
			if (ordinal < kMaximumLogs)
			{
				gLog.FormattedMessage(
					"tnvse_freetype_native: submission-suppressed reason=metadata-missing phase=%s shape=%p thread=%u",
					phase ? phase : "unknown", shape, GetCurrentThreadId());
			}
			else if (ordinal == kMaximumLogs)
			{
				gLog.FormattedMessage(
					"tnvse_freetype_native: metadata-missing render logs capped at %u entries",
					kMaximumLogs);
			}
		}

		void SuppressImmediateRoute(NiTriShape* shape, const char* phase)
		{
			const A8ShapeMetadataPtr metadata = FindA8ShapeMetadata(shape);
			if (!metadata)
			{
				LogMissingMetadata(shape, phase);
				return;
			}
			RecordNativeA8Suppression(shape, *metadata,
				metadata->nativePayload.buildComplete
					? NativeA8FallbackReason::DirectImmediate
					: NativeA8FallbackReason::PacketBuild,
				phase);
		}

		void __fastcall A8DeleteThis(NiTriShape* shape, void*)
		{
			A8State& state = State();
			A8ShapeMetadataPtr retiredMetadata;
			{
				std::lock_guard<std::mutex> lock(state.metadataMutex);
				const auto found = state.shapeMetadata.find(shape);
				if (found != state.shapeMetadata.end())
				{
					state.metadataGenerations[GetMetadataGenerationSlot(shape)].fetch_add(1,
						std::memory_order_release);
					retiredMetadata = std::move(found->second);
					state.shapeMetadata.erase(found);
				}
			}
			retiredMetadata.reset();
			state.originalDeleteThis(shape);
		}
	}

	void BeginA8SortedTileConstantBatch()
	{
		s_pixelConstantBatch.BeginFrame();
	}

	void EndA8SortedTileConstantBatch()
	{
		FlushNativePixelConstantBatch("sorted-frame-end");
		s_pixelConstantBatch.EndFrame();
	}

	A8ShapeMetadataPtr FindA8ShapeMetadata(const NiTriShape* shape)
	{
		if (!shape)
			return {};
		A8State& state = State();
		const size_t generationSlot = GetMetadataGenerationSlot(shape);
		const UInt64 generation = state.metadataGenerations[generationSlot].load(
			std::memory_order_acquire);
		A8MetadataHotSet& hotSet = GetMetadataHotSet(shape);
		A8MetadataHotEntry* replacement = nullptr;
		for (A8MetadataHotEntry& hot : hotSet.ways)
		{
			if (hot.shape != shape)
				continue;
			if (hot.generation == generation)
			{
				A8ShapeMetadataPtr metadata = hot.metadata.lock();
				if (metadata)
				{
					RecordFreeTypePerf(FreeTypePerfCounter::MetadataHotHit);
					return metadata;
				}
			}
			hot = {};
			replacement = &hot;
		}

		RecordFreeTypePerf(FreeTypePerfCounter::MetadataLockedLookup);
		std::lock_guard<std::mutex> lock(state.metadataMutex);
		const auto found = state.shapeMetadata.find(shape);
		if (found == state.shapeMetadata.end())
			return {};
		A8MetadataHotEntry& hot = replacement
			? *replacement : SelectMetadataHotVictim(hotSet);
		hot.shape = shape;
		hot.generation = state.metadataGenerations[generationSlot].load(
			std::memory_order_relaxed);
		hot.metadata = found->second;
		return found->second;
	}

	TileRenderPassFn ReadTileRenderPassCallTarget()
	{
		const UInt8* call = reinterpret_cast<const UInt8*>(kTileRenderPassCallSite);
		if (!call || call[0] != 0xE8)
			return nullptr;
		SInt32 displacement = 0;
		std::memcpy(&displacement, call + 1, sizeof(displacement));
		return reinterpret_cast<TileRenderPassFn>(
			kTileRenderPassCallSite + 5 + displacement);
	}

	bool IsA8TileRenderPassHookCurrent()
	{
		return State().originalTileRenderPass
			&& ReadTileRenderPassCallTarget() == &A8TileRenderPass;
	}

	void __cdecl A8TileRenderPass(BSShaderProperty::RenderPass* pass,
		UInt32 currentPass, bool testAlpha, bool blendAlpha, bool setupDrawmode)
	{
		A8State& state = State();
		if (!state.originalTileRenderPass)
			return;
		NiTriShape* shape = pass
			? reinterpret_cast<NiTriShape*>(pass->pGeometry) : nullptr;
		if (!IsA8AtlasShape(shape))
		{
			if (s_pixelConstantBatch.FrameActive())
				FlushNativePixelConstantBatch("before-stock-tile");
			InvalidateNativeA8SortedShaderState();
			state.originalTileRenderPass(pass, currentPass, testAlpha,
				blendAlpha, setupDrawmode);
			return;
		}

		NativeA8SortedFrameEntryView frameEntry;
		const bool sortedFrameHit =
			FindNativeA8SortedFrameEntry(shape, frameEntry);
		A8ShapeMetadataPtr metadataOwner;
		const A8ShapeMetadata* metadata = nullptr;
		NativeA8ShapePayload* payload = nullptr;
		if (sortedFrameHit)
		{
			metadata = frameEntry.metadata;
			payload = frameEntry.payload;
		}
		else
		{
			metadataOwner = FindA8ShapeMetadata(shape);
			metadata = metadataOwner.get();
			if (metadata && metadata->nativePayload.buildComplete)
				payload = &metadata->nativePayload;
		}
		if (!metadata)
		{
			LogMissingMetadata(shape, "tile-render-pass");
			return;
		}
		if (payload)
		{
			const bool needsVisibilityCheck = !sortedFrameHit
				|| frameEntry.visibilityCull
					!= NativeA8VisibilityCull::None;
			if (needsVisibilityCheck)
			{
				const NativeA8VisibilityCull visibilityCull =
					EvaluateNativeA8SubmissionVisibility(shape, *payload);
				if (visibilityCull != NativeA8VisibilityCull::None)
				{
					RecordNativeA8VisibilityCull(
						visibilityCull, *payload);
					return;
				}
			}
		}
		NativeA8FallbackReason failure = NativeA8FallbackReason::None;
		if (!payload)
			failure = NativeA8FallbackReason::PacketBuild;
		else if (payload->suppressNextSubmit.exchange(false,
			std::memory_order_acq_rel))
		{
			failure = payload->stickyReason.exchange(
				NativeA8FallbackReason::None, std::memory_order_acq_rel);
			if (failure == NativeA8FallbackReason::None)
				failure = NativeA8FallbackReason::RuntimeFault;
		}
		else if (sortedFrameHit
			&& frameEntry.preflightResult == NativeA8FallbackReason::None
			&& frameEntry.validationToken
			&& frameEntry.generation == payload->preparedGeneration
			)
		{
			// NativeA8RenderSorted retained the metadata owner and validated this
			// exact payload immediately before the stock sorted Tile traversal.
			failure = NativeA8FallbackReason::None;
		}
		else
			failure = PrepareNativeA8Group(shape, *metadata, *payload);

		if (failure == NativeA8FallbackReason::None)
		{
			NativeA8ShapePayload* const sourcePayload = payload;
			NativePacketDrawResult draw;
			const bool directShapeHandled = sortedFrameHit
				&& TryDrawNativeSinglePacketDirect(pass, currentPass,
					setupDrawmode, shape, *sourcePayload, draw);
			if (!directShapeHandled)
			{
				draw = DrawNativePacketSet(pass, currentPass,
					setupDrawmode, shape, *sourcePayload);
			}
			if (!draw.runtimeFault)
			{
				if (g_bEnableFreeTypeFontRenderingLog
					&& !state.loggedTileRenderPassHit)
				{
					state.loggedTileRenderPassHit = true;
					gLog.FormattedMessage(
						"tnvse_freetype_native: native Tile group route hit shape=%p font=%u pass=%u packets=%u ranges=%u route=%s",
						shape, metadata->fontId, currentPass,
						static_cast<UInt32>(
							sourcePayload->packetShaders.size()),
						sourcePayload->payloadTemplate
							? sourcePayload->payloadTemplate->sourceRangeCount
							: 0u,
						draw.directShapeRoute
							? "direct-single-packet-shape"
							: (draw.stockLikeBitmapRoute
								? "stock-like-bitmap-pages"
								: "effect-packets"));
				}
				return;
			}
			InvalidateNativeA8SortedShaderState();
			if (draw.constantStateFault)
			{
				MarkNativeA8GenerationFault(
					sourcePayload->preparedGeneration,
					draw.operation, draw.result);
				gLog.FormattedMessage(
					"tnvse_freetype_native: shader-constant isolation fault operation=%s hr=0x%08X register=%d shape=%p font=%u generation=%u drewPacket=%u action=suppress-native-group",
					draw.operation, static_cast<UInt32>(draw.result),
					draw.mismatchRegister, shape, metadata->fontId,
					sourcePayload->preparedGeneration,
					draw.drewPacket ? 1 : 0);
			}
			if (draw.drewPacket)
			{
				MarkNativeA8RuntimeFault(*metadata, *sourcePayload,
					draw.failure);
				return;
			}
			failure = draw.failure;
		}

		RecordNativeA8Suppression(shape, *metadata, failure, "tile-render-pass");
	}

	bool HookTileRenderPass()
	{
		TileRenderPassFn current = ReadTileRenderPassCallTarget();
		const TileRenderPassFn hook = &A8TileRenderPass;
		if (current == hook)
		{
			State().tileRenderPassHookInstalled = State().originalTileRenderPass != nullptr;
			return State().tileRenderPassHookInstalled;
		}
		if (!current)
		{
			if (!State().loggedTileRenderPassHookConflict)
			{
				State().loggedTileRenderPassHookConflict = true;
				gLog.FormattedMessage(
					"tnvse_freetype_native: Tile accumulator call site is not CALL rel32; native route unavailable");
			}
			return false;
		}
		if (State().tileRenderPassHookInstalled)
		{
			if (!State().loggedTileRenderPassHookConflict)
			{
				State().loggedTileRenderPassHookConflict = true;
				gLog.FormattedMessage(
					"tnvse_freetype_native: Tile accumulator native route was replaced; marked groups will be suppressed");
			}
			return false;
		}
		if (reinterpret_cast<UInt32>(current) != kStockTileRenderPassImmediately)
		{
			if (!State().loggedTileRenderPassHookConflict)
			{
				State().loggedTileRenderPassHookConflict = true;
				gLog.FormattedMessage(
					"tnvse_freetype_native: Tile accumulator call site already has a non-stock target=%p; leaving it untouched",
					current);
			}
			return false;
		}

		State().originalTileRenderPass = current;
		WriteRelCall(kTileRenderPassCallSite, hook);
		State().tileRenderPassHookInstalled = ReadTileRenderPassCallTarget() == hook;
		if (!State().tileRenderPassHookInstalled)
		{
			State().originalTileRenderPass = nullptr;
			return false;
		}
		if (g_bEnableFreeTypeFontRenderingLog)
		{
			gLog.FormattedMessage(
				"tnvse_freetype_native: installed Tile accumulator native route original=%p stock=1",
				current);
		}
		return true;
	}

	bool IsA8AtlasShape(const NiTriShape* shape)
	{
		return shape && *reinterpret_cast<void* const* const*>(shape)
			== &State().triShapeVtable[1];
	}

	void __fastcall A8RenderImmediate(NiTriShape* shape, void*,
		NiRenderer* renderer)
	{
		if (s_nativeDirectImmediateContext
			&& s_nativeDirectImmediateContext->shape == shape
			&& State().originalRenderImmediate)
		{
			s_nativeDirectImmediateContext->invoked = true;
			State().originalRenderImmediate(shape, renderer);
			return;
		}
		SuppressImmediateRoute(shape, "shape-immediate");
	}

	void __fastcall A8RenderImmediateAlt(NiTriShape* shape, void*,
		NiRenderer* renderer)
	{
		if (s_nativeDirectImmediateContext
			&& s_nativeDirectImmediateContext->shape == shape
			&& State().originalRenderImmediateAlt)
		{
			s_nativeDirectImmediateContext->invoked = true;
			State().originalRenderImmediateAlt(shape, renderer);
			return;
		}
		SuppressImmediateRoute(shape, "shape-immediate-alt");
	}

	bool InitializeA8TriShapeVtable(NiTriShape* shape)
	{
		void** source = shape ? *reinterpret_cast<void***>(shape) : nullptr;
		if (!source)
			return false;
		if (source == &State().triShapeVtable[1])
			return true;
		if (State().originalTriShapeVtable)
			return source == State().originalTriShapeVtable;

		State().originalTriShapeVtable = source;
		State().triShapeVtable[0] = source[-1];
		std::copy(source, source + kCopiedTriShapeVtableEntries,
			State().triShapeVtable.begin() + 1);
		State().originalRenderImmediate = reinterpret_cast<RenderImmediateFn>(
			State().triShapeVtable[kRenderImmediateSlot + 1]);
		State().originalRenderImmediateAlt = reinterpret_cast<RenderImmediateFn>(
			State().triShapeVtable[kRenderImmediateAltSlot + 1]);
		State().originalDeleteThis = reinterpret_cast<DeleteThisFn>(
			State().triShapeVtable[kDeleteThisSlot + 1]);
		State().triShapeVtable[kDeleteThisSlot + 1]
			= reinterpret_cast<void*>(&A8DeleteThis);
		State().triShapeVtable[kRenderImmediateSlot + 1]
			= reinterpret_cast<void*>(&A8RenderImmediate);
		State().triShapeVtable[kRenderImmediateAltSlot + 1]
			= reinterpret_cast<void*>(&A8RenderImmediateAlt);
		return State().originalRenderImmediate && State().originalRenderImmediateAlt
			&& State().originalDeleteThis;
	}
}

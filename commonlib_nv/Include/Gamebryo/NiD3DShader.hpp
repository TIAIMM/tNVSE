#pragma once

#include "NiD3DShaderInterface.hpp"
#include "NiD3DShaderDeclaration.hpp"
#include "NiD3DShaderConstantMap.hpp"
#include "NiD3DPass.hpp"
#include "NiPropertyState.hpp"

class NiD3DRenderStateGroup;
class NiD3DShaderConstantMap;

class NiD3DShader : public NiD3DShaderInterface {
public:
	NiD3DShader();
	virtual ~NiD3DShader();

	virtual void ResetPasses();

	bool						m_bUsesNiRenderState;
	bool						m_bUsesNiLightState;
	NiD3DShaderDeclarationPtr	m_spShaderDecl;
	NiD3DRenderStateGroup*		m_pkRenderStateGroup;
	NiD3DShaderConstantMapPtr	m_spPixelConstantMap;
	NiD3DShaderConstantMapPtr	m_spVertexConstantMap;
	UInt32						m_uiCurrentPass;

	static NiD3DPass* GetCurrentPass();
	static void SetCurrentPass(NiD3DPass* apPass);
	void SetBlendAlphaEx(const NiPropertyState* apProperties);
	UInt32 SetupTransformationsEx(NiGeometry* apGeometry, const NiSkinInstance* apSkin, const NiSkinPartition::Partition* apPartition, NiGeometryBufferData* apRendererData, const NiPropertyState* apProperties, const NiDynamicEffectState* apEffects, const NiTransform& arWorld, const NiBound& arWorldBound);
	NiGeometryBufferData* PrepareGeometryForRendering(NiGeometry* apGeometry, const NiSkinPartition::Partition* apPartition, NiGeometryBufferData* apRendererData, const NiPropertyState* apProperties);

	NiD3DShaderConstantMap* GetPixelConstantMap() const;
	void SetPixelConstantMap(NiD3DShaderConstantMap* apPixelConstantMap);

	NiD3DShaderConstantMap* GetVertexConstantMap() const;
	void SetVertexConstantMap(NiD3DShaderConstantMap* apVertexConstantMap);

	void SetPixelShaderConstants(NiGeometry* apGeometry, NiSkinInstance* apSkin, NiSkinPartition::Partition* apPartition, NiGeometryBufferData* apBuffData, const NiPropertyState* apProperties, NiDynamicEffectState* apEffects, NiTransform& akWorld, NiBound& akWorldBound, UInt32 auiPass, bool abGlobal = true) const;
	void SetVertexShaderConstants(NiGeometry* apGeometry, NiSkinInstance* apSkin, NiSkinPartition::Partition* apPartition, NiGeometryBufferData* apBuffData, const NiPropertyState* apProperties, NiDynamicEffectState* apEffects, NiTransform& akWorld, NiBound& akWorldBound, UInt32 auiPass, bool abGlobal = true) const;

	void ConfigureTextureStagesEx();

	bool InitializeEx();

	static bool bSetupConsistency;
};

ASSERT_SIZE(NiD3DShader, 0x3C);
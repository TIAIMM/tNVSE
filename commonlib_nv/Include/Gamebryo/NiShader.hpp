#pragma once

#include "NiRefObject.hpp"
#include "NiFixedString.hpp"
#include "NiRTTI.hpp"
#include "NiGeometry.hpp"

class NiDynamicEffectState;
class NiGeometryBufferData;
class NiShaderConstantMap;
class NiGPUProgram;
class NiSkinInstance;

class NiShader : public NiRefObject {
public:
	NiShader();
	virtual ~NiShader();

	virtual NiRTTI*						GetRTTI();
	virtual NiNode*						IsNiNode();
	virtual BSFadeNode*					IsFadeNode();
	virtual BSMultiBoundNode*			IsMultiBoundNode();
	virtual NiGeometry*					IsGeometry();
	virtual NiTriBasedGeom*				IsTriBasedGeometry();
	virtual NiTriStrips*				IsTriStrips();
	virtual NiTriShape*					IsTriShape();
	virtual BSSegmentedTriShape*		IsSegmentedTriShape();
	virtual BSResizableTriShape*		IsResizableTriShape();
	virtual NiParticles*				IsParticlesGeom();
	virtual NiLines*					IsLinesGeom();
	virtual bhkNiCollisionObject*		IsBhkNiCollisionObject();
	virtual bhkBlendCollisionObject*	IsBhkBlendCollisionObject();
	virtual bhkRigidBody*				IsBhkRigidBody();
	virtual bhkLimitedHingeConstraint*	IsBhkLimitedHingeConstraint();
	virtual bool						IsInitialized();
	virtual void						Initialize();
	virtual bool						SetupGeometry(NiGeometry* apGeometry);
	virtual UInt32						PreProcessPipeline(NiGeometry* apGeometry, const NiSkinInstance* apSkin, NiGeometryBufferData* apRendererData, const NiPropertyState* apProperties, const NiDynamicEffectState* apEffects, const NiTransform& arWorld, const NiBound& arWorldBound);
	virtual void						UpdatePipeline(NiGeometry* apGeometry, const NiSkinInstance* apSkin, NiGeometryBufferData* apRendererData, const NiPropertyState* apProperties, const NiDynamicEffectState* apEffects, const NiTransform& arWorld, const NiBound& arWorldBound);
	virtual void						SetupRenderingPass(NiGeometry* apGeometry, const NiSkinInstance* apSkin, NiGeometryBufferData* apRendererData, const NiPropertyState* apProperties, const NiDynamicEffectState* apEffects, const NiTransform& arWorld, const NiBound& arWorldBound);
	virtual UInt32						SetupTransformations(NiGeometry* apGeometry, const NiSkinInstance* apSkin, const NiSkinPartition::Partition* apPartition, NiGeometryBufferData* apRendererData, const NiPropertyState* apProperties, const NiDynamicEffectState* apEffects, const NiTransform& arWorld, const NiBound& arWorldBound);
	virtual UInt32						SetupTransformationsAlt(NiGeometry* apGeometry, const NiSkinInstance* apSkin, const NiSkinPartition::Partition* apPartition, NiGeometryBufferData* apRendererData, const NiPropertyState* apProperties, const NiDynamicEffectState* apEffects, const NiTransform& arWorld, const NiBound& arWorldBound);
	virtual UInt32						SetupShaderPrograms(NiGeometry* apGeometry, const NiSkinInstance* apSkin, const NiSkinPartition::Partition* apPartition, NiGeometryBufferData* apRendererData, const NiPropertyState* apProperties, const NiDynamicEffectState* apEffects, const NiTransform& arWorld, const NiBound& arWorldBound);
	// Do not use
	virtual NiGeometryBufferData*		vPrepareGeometryForRendering(NiGeometry* apGeometry, const NiSkinPartition::Partition* apPartition, NiGeometryBufferData* apRendererData, const NiPropertyState* apProperties);
	virtual UInt32						PostRender(NiGeometry* apGeometry, const NiSkinInstance* apSkin, const NiSkinPartition::Partition* apPartition, NiGeometryBufferData* apRendererData, const NiPropertyState* apProperties, const NiDynamicEffectState* apEffects, const NiTransform& arWorld, const NiBound& arWorldBound);
	virtual UInt32						PostProcessPipeline(NiGeometry* apGeometry, const NiSkinInstance* apSkin, NiGeometryBufferData* apRendererData, const NiPropertyState* apProperties, const NiDynamicEffectState* apEffects, const NiTransform& arWorld, const NiBound& arWorldBound);
	virtual void						SetupTextures(const NiPropertyState* apProperties);
	virtual void						UpdateConstants(const NiPropertyState* apProperties);
	virtual void						SetBlendAlpha(const NiPropertyState* apProperties);
	virtual void						SetTestAlpha(const NiPropertyState* apProperties);
	virtual void						PreRenderSetup(const NiPropertyState* apProperties, bool abFirstPass);
	virtual void						PostRenderSetup(const NiPropertyState* apProperties);
	virtual int							HasPasses();
	virtual int							EndPass();
	virtual void						StreamCanSkip();
	virtual NiShaderDeclaration*		CreateShaderDeclaration(UInt32 auiMaxStreamEntryCount, UInt32 auiStreamCount = 1);
	virtual bool						AppendRenderPass(UInt32& auiPassId);
	virtual bool						SetAlphaOverride(UInt32 auiPassId, bool abAlphaBlend, bool abUsePreviousSrcBlendMode, NiAlphaProperty::AlphaFunction aeSrcBlendMode, bool abUsePreviousDestBlendMode, NiAlphaProperty::AlphaFunction aeDestBlendMode);
	virtual void						SetUsesNiRenderState(bool abUses);
	virtual bool						SetGPUProgram(UInt32 auiPassId, NiGPUProgram* apProgram, UInt32& auiProgramId);
	virtual NiShaderConstantMap*		CreateShaderConstantMap(UInt32 auiPassId, UInt32 auiProgramId);
	virtual bool						AppendTextureSampler(UInt32 auiPassId, UInt32& auiSamplerId, const NiFixedString& arSemantic, const NiFixedString& arVariableName, UInt32 auiInstance = 0);
	virtual void*						GetShaderInstanceDesc();

	NiFixedString	m_kName;
	UInt32			m_uiImplementation;
	bool			m_bInitialized;

	bool InitializeEx();
};

ASSERT_SIZE(NiShader, 0x14);
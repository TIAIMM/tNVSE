#pragma once

#include "NiGPUProgram.hpp"
#include "NiShaderConstantMap.hpp"
#include "NiShaderConstantMapEntry.hpp"
#include "NiSkinPartition.hpp"
#include "NiTArray.hpp"

class NiD3DRenderState;
class NiD3DShaderProgram;
class NiGeometry;
class NiSkinInstance;
class NiGeometryBufferData;
class NiPropertyState;
class NiDynamicEffectState;
class NiSkinPartition;
class NiDX9Renderer;
class NiTransform;
class NiExtraData;

NiSmartPointer(NiD3DShaderConstantMap);

class NiD3DShaderConstantMap : public NiShaderConstantMap {
public:
	NiD3DShaderConstantMap();
	virtual ~NiD3DShaderConstantMap();

	virtual NiRTTI						GetRTTI();
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
	virtual UInt32						AddPredefinedEntry(const char* apszKey, UInt32 auiExtra, UInt32 auiShaderRegister = 0, const char* apszVariableName = "", UInt32 auiDataSize = 0, UInt32 auiDataStride = 0, void* pvDataSource = 0, bool bCopyData = false);
	virtual UInt32						RemoveEntry(const char* apszKey);
	virtual NiShaderConstantMapEntry*	GetEntry(const char* apszKey);
	virtual int							GetEntryAtIndex(UInt32 auiIndex);
	virtual UInt32						SetShaderConstants(NiD3DShaderProgram* apShaderProgram, NiGeometry* apGeometry, const NiSkinInstance* apSkin, const NiSkinPartition::Partition* apPartition, NiGeometryBufferData* apBuffData, const NiPropertyState* apProperties, const NiDynamicEffectState* apEffects, const NiTransform& arWorld, const NiBound& arWorldBound, UInt32 auiPass = 0, bool abGlobal = false);
	virtual UInt32						GetEntryIndex(const char* apszKey);
	virtual UInt32						InsertEntry(NiShaderConstantMapEntry* apEntry);
	virtual UInt32						InsertDefinedEntry(NiShaderConstantMapEntry* apEntry);
	virtual UInt32						SetupPredefinedEntry(NiShaderConstantMapEntry* apEntry);
	virtual UInt32						SetDefinedConstant(NiD3DShaderProgram* apShaderProgram, NiShaderConstantMapEntry* apEntry, NiGeometry* apGeometry, const NiSkinInstance* apSkin, const NiSkinPartition::Partition* apPartition, NiGeometryBufferData* apBuffData, const NiPropertyState* apProperties, const NiDynamicEffectState* apEffects, const NiTransform& arWorld, const NiBound& arWorldBound, UInt32 auiPass);
	virtual UInt32						SetConstantConstant(NiD3DShaderProgram* apShaderProgram, NiShaderConstantMapEntry* apEntry, UInt32 auiPass);
	virtual UInt32						SetAttributeConstant(NiD3DShaderProgram* apShaderProgram, NiShaderConstantMapEntry* apEntry, NiGeometry* apGeometry, const NiSkinInstance* apSkin, const NiSkinPartition::Partition* apPartition, NiGeometryBufferData* apBuffData, const NiPropertyState* apProperties, const NiDynamicEffectState* apEffects, const NiTransform& arWorld, const NiBound& arWorldBound, UInt32 auiPass, bool abGlobal, NiExtraData* apExtraData);
	virtual UInt32						SetGlobalConstant(NiD3DShaderProgram* apShaderProgram, NiShaderConstantMapEntry* apEntry, UInt32 auiPass);
	virtual UInt32						SetOperatorConstant(NiD3DShaderProgram* apShaderProgram, NiShaderConstantMapEntry* apEntry, NiGeometry* apGeometry, const NiSkinInstance* apSkin, const NiPropertyState* apProperties, const NiDynamicEffectState* apEffects, const NiTransform& arWorld, const NiBound& arWorldBound, UInt32 auiPass);
	virtual UInt32						SetupObjectEntry(NiShaderConstantMapEntry* apEntry);
	virtual UInt32						SetObjectConstant(NiD3DShaderProgram* apShaderProgram, NiShaderConstantMapEntry* apEntry, NiGeometry* apGeometry, const NiSkinInstance* apSkin, const NiSkinPartition::Partition* apPartition, NiGeometryBufferData* apBuffData, const NiPropertyState* apProperties, const NiDynamicEffectState* apEffects, const NiTransform& arWorld, const NiBound& arWorldBound, UInt32 auiPass);

	NiTObjectArray<NiShaderConstantMapEntryPtr>	m_aspEntries;
	NiTObjectArray<NiShaderConstantMapEntryPtr>	m_aspDefinedEntries;
	bool										m_bModified;
	NiD3DShaderProgram*							m_pkLastShaderProgram;
	UInt32										m_eLastError;
	LPDIRECT3DDEVICE9							m_pkD3DDevice;
	NiDX9Renderer*								m_pkD3DRenderer;
	NiD3DRenderState*							m_pkD3DRenderState;

	static NiD3DShaderConstantMap* Create(NiDX9Renderer* apRenderer, NiGPUProgram::ProgramType aeType);
	static NiD3DShaderConstantMap* CreatePixelConstantMap(NiDX9Renderer* apRenderer);
	static NiD3DShaderConstantMap* CreateVertexConstantMap(NiDX9Renderer* apRenderer);

	UInt32 SetShaderConstantsEx(NiD3DShaderProgram* apShaderProgram, NiGeometry* apGeometry, NiSkinInstance* apSkin, NiSkinPartition::Partition* apPartition, NiGeometryBufferData* apBuffData, const NiPropertyState* apProperties, NiDynamicEffectState* apEffects, NiTransform& arWorld, NiBound& arWorldBound, UInt32 auiPass, bool abGlobal);
};

ASSERT_SIZE(NiD3DShaderConstantMap, 0x44)
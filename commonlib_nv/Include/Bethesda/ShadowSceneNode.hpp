#pragma once

#include "BSCompoundFrustum.hpp"
#include "BSCubeMapCamera.hpp"
#include "BSFogProperty.hpp"
#include "NiNode.hpp"
#include "NiTriBasedGeom.hpp"
#include "ShadowSceneLight.hpp"

class BSCullingProcess;
class NiDirectionalLight;

NiSmartPointer(ShadowSceneNode);

class ShadowSceneNode : public NiNode {
public:
	UInt32									unk0AC;
	UInt32									unk0B0;
	NiTPointerList<ShadowSceneLightPtr>		kLights;
	NiTPointerList<ShadowSceneLightPtr>		kShadowLights;
	NiTListIterator							kShadowLightsIter;
	NiTPointerList<ShadowSceneLightPtr>*	pListNode0D0;
	NiTPointerList<ShadowSceneLightPtr>*	pListNode0D4;
	ShadowSceneLightPtr						spDummyLight[2]; // Empty lights to fill kShadowLights
	ShadowSceneLight*						pSunLight;		
	NiTPointerList<ShadowSceneLightPtr>		kLightQueueAdd;
	NiTPointerList<ShadowSceneLightPtr>		kLightQueueRemove;
	NiTPointerList<ShadowSceneLightPtr>		kDynamicLights;
	NiTPointerList<NiAVObjectPtr>			kList108;
	NiTPointerList<NiAVObjectPtr>			kLitActors;
	UInt8									cSceneGraphIndex;
	NiObjectPtr								spObject124;
	NiPointer<BSCubeMapCamera>				spShadowMapCubeCam;
	UInt32									unk12C;
	bool									bDisableLightUpdate;
	bool									bWireframe;
	BSFogPropertyPtr						spFog;
	BSCompoundFrustum						kCompoundFrustum;
	bool									bVisibleUnboundSpace;
	BSPortalGraph*							pBSPortalGraph;
	NiPoint3								kLightingOffset;
	NiPoint3								kCameraPos;
	bool									bAllowLightRemoveQueues;

	NIRTTI_ADDRESS(0x11F9E80);

#if _EDITOR
	BSMultiBoundNodePtr spMultiBound224;
	ShadowSceneLightPtr spTestLight;
	NiNodePtr			spDebugNode;
	UInt32				uiDebugFlags;
	NiCameraPtr			spFrozenCamera;
	NiCameraPtr			spRestoreCamera;
#endif

	static NiPointer<NiCamera>				spRestoreCamera;
	static NiPointer<NiCamera>				spFrozenCamera;

	static BSSpinLock* const pLightQueueLock;
	static BSSpinLock* const pLitObjectsLock;

	BSPortalGraph* GetPortalGraph() const { return pBSPortalGraph; };

	static ShadowSceneNode* Create();

	void OnVisibleEx(BSCullingProcess* apCuller);

	static BSFogProperty* GetFogProperty(UInt32 aeType);

	void DisableLightUpdate(bool abDisable);

	double GetLuminanceAtPoint(NiPoint3 aPoint, UInt32& auiNumberlights, float& afAmbientLum, float& afSunLum, NiLight* apIgnore);

	void SetShadowCastToLight(NiLight* apLight, bool bEnableShadow) const;

	void PreOnVisible(BSCullingProcess* apCuller);

	ShadowSceneLight* GetFirstShadowLight();
	ShadowSceneLight* GetNextShadowLight();
	ShadowSceneLight* AddShadowLight(NiAVObject* apCaster);

	void UpdateDynamicShadows(BSCullingProcess* apCuller);
	void DisableAllShadowLights() const;

	void ProcessQueuedLights();

	static void RemoveLightsFromGeom(NiTriBasedGeom* apGeometry);

	ShadowSceneLight* AddLight(NiLight* apLight, bool abDynamic);
	void RemoveLight(const NiLight* apLight);
	void RemoveLight(const ShadowSceneLightPtr& arLight);
	void RemoveAllLights();
	void RemoveAllShadowLights();

	void FindAndCullLight(NiNode* apNode, bool abRemove);
	void FindAndCullLightRecurse(NiNode* apNode, bool abRemove);

	UInt32 AddAllLightsFromNode(NiNode* apNode, float afRadius);

	void BuildSharedCompoundFrustum(NiCamera* apCamera, BSPortal* apPortal);

	void UpdateMultiBoundVisibility(BSMultiBoundNode* apNode, NiPoint3 akViewPoint, BSCullingProcess& arCuller) const;
	void UpdateRoomAndPortalVisibility(BSMultiBoundRoom* apRoom, NiCamera* apCamera, NiFrustumPlanes* apPlanes, bool abAddIntersection);

	void AddQueuedShadowLight(ShadowSceneLight* apLight);
	void AddQueuedLight(ShadowSceneLight* apLight);
	void AddObject(NiAVObject* apObject, bool abIsActor);
	void AddVisibleObject(NiAVObject* apObject);

	void UpdateDynamicLight(ShadowSceneLight* apLight);
	void UpdateLight(ShadowSceneLight* apLight);

	void SetSunLight(NiDirectionalLight* apLight = nullptr);
};

ASSERT_SIZE(ShadowSceneNode, 0x200);
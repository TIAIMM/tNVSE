#include "NiAVObject.hpp"
#include "NiCullingProcess.hpp"
#include "NiGeomMorpherController.hpp"
#include "NiNode.hpp"
#include "bhkNiCollisionObject.hpp"

BSSpinLock* NiAVObject::pPropertyStateLock = (BSSpinLock*)0x11F42A0;

NiNode* NiAVObject::GetParent() {
	return m_pkParent;
}

const NiNode* NiAVObject::GetParent() const {
	return m_pkParent;
}

// 0xA59F90
void NiAVObject::UpdateDownwardPassEx(NiUpdateData& arData, UInt32 auiFlags) {
	if (arData.bUpdateControllers)
		UpdateObjectControllers(arData, true);

	UpdateWorldData(arData);
	UpdateWorldBound();
}

// 0xA68C60
void NiAVObject::UpdateWorldDataEx(NiUpdateData& arData) {
	if (GetCollisionObject())
		GetCollisionObject()->UpdateWorldData(arData);
	else
		UpdateWorldTransform();
}

NiTransform* NiAVObject::GetLocalTransform()
{
	return &m_kLocal;
}

NiTransform* NiAVObject::GetWorldTransform(){
	return &m_kWorld;
}

const float NiAVObject::GetLocalScale() const {
	return m_kLocal.m_fScale;
}

const float NiAVObject::GetWorldScale() const {
	return m_kWorld.m_fScale;
}

void NiAVObject::SetLocalTranslate(const NiPoint3& pos) {
	m_kLocal.m_Translate = pos;
}

void NiAVObject::SetLocalTranslate(const NiPoint3* pos) {
	m_kLocal.m_Translate = *pos;
}
const NiPoint3& NiAVObject::GetLocalTranslate() const {
	return m_kLocal.m_Translate;
}

void NiAVObject::SetWorldTranslate(const NiPoint3& pos) {
	m_kWorld.m_Translate = pos;
}

void NiAVObject::SetWorldTranslate(const NiPoint3* pos) {
	m_kWorld.m_Translate = *pos;
}

const NiPoint3& NiAVObject::GetWorldTranslate() const {
	return m_kWorld.m_Translate;
}

void NiAVObject::SetLocalRotate(float x, float y, float z) {
	m_kLocal.m_Rotate.FromEulerAnglesPRY(x, y, z);
}

void NiAVObject::SetLocalRotate(const NiMatrix3& kRot) {
	m_kLocal.m_Rotate = kRot;
}

void NiAVObject::SetLocalRotate(const NiMatrix3* rot) {
	m_kLocal.m_Rotate = *rot;
}

void NiAVObject::SetLocalRotateDeg(float x, float y, float z) {
	m_kLocal.m_Rotate.FromEulerAnglesPRY(x * 0.0174527f, y * 0.0174527f, z * 0.0174527f);
}

void NiAVObject::SetWorldRotateDeg(float x, float y, float z) {
	m_kWorld.m_Rotate.FromEulerAnglesPRY(x * 0.0174527f, y * 0.0174527f, z * 0.0174527f);
}

const NiMatrix3& NiAVObject::GetLocalRotate() const {
	return m_kLocal.m_Rotate;
}

void NiAVObject::SetWorldRotate(const NiMatrix3& rot) {
	m_kWorld.m_Rotate = rot;
}

void NiAVObject::SetWorldRotate(const NiMatrix3* rot) {
	m_kWorld.m_Rotate = *rot;
}

const NiMatrix3& NiAVObject::GetWorldRotate() const {
	return m_kWorld.m_Rotate;
}



// FLAGS

void NiAVObject::SetBit(UInt32 auData, bool abVal) {
	m_uiFlags.SetBit(auData, abVal);
}

bool NiAVObject::GetBit(UInt32 auData) const {
	return m_uiFlags.GetBit(auData);
}

void NiAVObject::SetAppCulled(bool abVal) {
	SetBit(APP_CULLED, abVal);
}

bool NiAVObject::GetAppCulled() const {
	return GetBit(APP_CULLED);
}

void NiAVObject::SetSelectiveUpdate(bool abVal) {
	SetBit(SELECTIVE_UPDATE, abVal);
}

bool NiAVObject::GetSelectiveUpdate() const {
	return GetBit(SELECTIVE_UPDATE);
}

void NiAVObject::SetSelUpdTransforms(bool abVal) {
	SetBit(SELECTIVE_UPDATE_TRANSFORMS, abVal);
}

bool NiAVObject::GetSelUpdTransforms() const {
	return GetBit(SELECTIVE_UPDATE_TRANSFORMS);
}

void NiAVObject::SetSelUpdController(bool abVal) {
	SetBit(SELECTIVE_UPDATE_CONTROLLER, abVal);
}

bool NiAVObject::GetSelUpdController() const {
	return GetBit(SELECTIVE_UPDATE_CONTROLLER);
}

void NiAVObject::SetSelUpdRigid(bool abVal) {
	SetBit(SELECTIVE_UPDATE_RIGID, abVal);
}

bool NiAVObject::GetSelUpdRigid() const {
	return GetBit(SELECTIVE_UPDATE_RIGID);
}

void NiAVObject::SetDisplayObject(bool abVal) {
	SetBit(DISPLAY_OBJECT, abVal);
}

bool NiAVObject::GetDisplayObject() const {
	return GetBit(DISPLAY_OBJECT);
}

void NiAVObject::SetDisableSorting(bool abVal) {
	SetBit(DISABLE_SORTING, abVal);
}

bool NiAVObject::GetDisableSorting() const {
	return GetBit(DISABLE_SORTING);
}

void NiAVObject::SetSelUpdTransformsOverride(bool abVal) {
	SetBit(SELECTIVE_UPDATE_TRANSFORMS_OVERRIDE, abVal);
}

bool NiAVObject::GetSelUpdTransformsOverride() const {
	return GetBit(SELECTIVE_UPDATE_TRANSFORMS_OVERRIDE);
}

void NiAVObject::SetSaveExternalGeomData(bool abVal) {
	SetBit(SAVE_EXTERNAL_GEOM_DATA, abVal);
}

bool NiAVObject::GetSaveExternalGeomData() const {
	return GetBit(SAVE_EXTERNAL_GEOM_DATA);
}

void NiAVObject::SetNoDecals(bool abVal) {
	SetBit(NO_DECALS, abVal);
}

bool NiAVObject::GetNoDecals() const {
	return GetBit(NO_DECALS);
}

void NiAVObject::SetAlwaysDraw(bool abVal) {
	SetBit(ALWAYS_DRAW, abVal);
}

bool NiAVObject::GetAlwaysDraw() const {
	return GetBit(ALWAYS_DRAW);
}

void NiAVObject::SetActorNode(bool abVal) {
	SetBit(ACTOR_NODE, abVal);
}

bool NiAVObject::GetActorNode() const {
	return GetBit(ACTOR_NODE);
}

void NiAVObject::SetFixedBound(bool abVal) {
	SetBit(FIXED_BOUND, abVal);
}

bool NiAVObject::GetFixedBound() const {
	return GetBit(FIXED_BOUND);
}

void NiAVObject::SetFadedIn(bool abVal) {
	SetBit(FADED_IN, abVal);
}

bool NiAVObject::GetFadedIn() const {
	return GetBit(FADED_IN);
}

void NiAVObject::SetIgnoreFade(bool abVal) {
	SetBit(IGNORE_FADE, abVal);
}

bool NiAVObject::GetIgnoreFade() const {
	return GetBit(IGNORE_FADE);
}

void NiAVObject::SetLODFadingOut(bool abVal) {
	SetBit(LOD_FADING_OUT, abVal);
}

bool NiAVObject::GetLODFadingOut() const {
	return GetBit(LOD_FADING_OUT);
}

void NiAVObject::SetHasMovingSound(bool abVal) {
	SetBit(HAS_MOVING_SOUND, abVal);
}

bool NiAVObject::GetHasMovingSound() const {
	return GetBit(HAS_MOVING_SOUND);
}

void NiAVObject::SetHasPropertyController(bool abVal) {
	SetBit(HAS_PROPERTY_CONTROLLER, abVal);
}

bool NiAVObject::GetHasPropertyController() const {
	return GetBit(HAS_PROPERTY_CONTROLLER);
}

void NiAVObject::SetHasBound(bool abVal) {
	SetBit(HAS_BOUND, abVal);
}

bool NiAVObject::GetHasBound() const {
	return GetBit(HAS_BOUND);
}

void NiAVObject::SetActorCulled(bool abVal) {
	SetBit(ACTOR_CULLED, abVal);
}

bool NiAVObject::GetActorCulled() const {
	return GetBit(ACTOR_CULLED);
}

void NiAVObject::SetIgnoresPicking(bool abVal) {
	SetBit(IGNORES_PICKING, abVal);
}

bool NiAVObject::GetIgnoresPicking() const {
	return GetBit(IGNORES_PICKING);
}

void NiAVObject::SetNoShadows(bool abVal) {
	SetBit(NO_SHADOWS, abVal);
}

bool NiAVObject::GetNoShadows() const {
	return GetBit(NO_SHADOWS);
}

void NiAVObject::SetPlayerBone(bool abVal) {
	SetBit(PLAYER_BONE, abVal);
}

bool NiAVObject::GetIsPlayerBone() const {
	return GetBit(PLAYER_BONE);
}

void NiAVObject::SetImposterLoaded(bool abVal) {
	SetBit(IMPOSTER_LOADED, abVal);
}

bool NiAVObject::GetImposterLoaded() const {
	return GetBit(IMPOSTER_LOADED);
}

// 0xA59C60
void NiAVObject::Update() {
	NiUpdateData updateData{};
	Update(updateData);
}

// 0xA59C60
void NiAVObject::Update(NiUpdateData& arData) {
	UpdateDownwardPass(arData, 0);
	if (m_pkParent) {
		m_pkParent->UpdateUpwardPass();
	}
}

// 0xA59D30
NiProperty* NiAVObject::GetProperty(UInt32 iType) const {
	if (iType > NiProperty::MAX_TYPES)
		return nullptr;

	NiTListIterator kPos = m_kPropertyList.GetHeadPos();
	while (kPos) {
		NiProperty* pkProperty = m_kPropertyList.GetNext(kPos);
		if (pkProperty && pkProperty->Type() == iType)
			return pkProperty;
	}

	return nullptr;
}

// 0x6838B0
bhkNiCollisionObject* NiAVObject::GetCollisionObject() const {
	return m_spCollisionObject.m_pObject;
}

// 0x62BC90
void NiAVObject::SetCollisionObject(bhkNiCollisionObject* apCollisionObject) {
	m_spCollisionObject = apCollisionObject;
	if (m_spCollisionObject && m_spCollisionObject->m_pkSceneObject != this)
		m_spCollisionObject->SetSceneGraphObject(this);
}

// 0xA59E00
void NiAVObject::Cull(NiCullingProcess* apCuller) {
	if (!GetAppCulled())
		apCuller->Process(this);
}

NiNode* NiAVObject::FindRootNode() {
	NiNode* parent = m_pkParent;
	if (!parent)
		if(IS_NODE(this))
			return static_cast<NiNode*>(this);
		else
			return nullptr;

	while (parent->m_pkParent) {
		parent = parent->m_pkParent;
	}
	return parent;
}

// 0xA5A110
bool NiAVObject::HasPropertyController() const {
	NiTListIterator kIter = m_kPropertyList.GetHeadPos();
	if (!kIter)
		return false;

	while (kIter) {
		NiProperty* pProp = m_kPropertyList.GetNext(kIter);
		if (pProp && pProp->m_spControllers.m_pObject)
			return true;
	}
	return false;
}

void NiAVObject::AttachProperty(NiProperty* apProperty) {
	m_kPropertyList.AddHead(apProperty);
}

// 0x5467E0
void NiAVObject::CreateWorldBoundIfMissing() {
	if (m_pWorldBound)
		return;

	NiBound* bound = new NiBound;
	SetBound(bound);
	m_pWorldBound->m_fRadius = 0.0f;
	m_pWorldBound->m_kCenter = NiPoint3::ZERO;
}

// 0x5467E0
void NiAVObject::CreateWorldBoundIfMissing(bool abAlwaysDraw) {
	SetAlwaysDraw(abAlwaysDraw);
	CreateWorldBoundIfMissing();
	if (abAlwaysDraw)
		m_pWorldBound->m_fRadius = 1.0f;
}

// 0x439410
void NiAVObject::AddProperty(NiProperty* apProperty) {
	ThisStdCall(0x439410, this, apProperty);
}

// 0xA5B230
void NiAVObject::RemoveProperty(UInt32 auiPropertyType) {
	NiTListIterator kIter = m_kPropertyList.GetHeadPos();
	while (kIter) {
		NiPropertyPtr spProperty = m_kPropertyList.GetNext(kIter);
		if (spProperty && spProperty->Type() == auiPropertyType)
			m_kPropertyList.Remove(spProperty);
	}
}

// 0x654640
void NiAVObject::DetachProperty(NiProperty* apProperty) {
	NiTListIterator kIter = m_kPropertyList.FindPos(apProperty);
	if (kIter)
		m_kPropertyList.RemovePos(kIter);
}

// 0xA5A040
void NiAVObject::UpdateProperties() {
	pPropertyStateLock->Lock();

	NiPropertyState* pNewState = nullptr;
	if (GetParent())
		GetParent()->UpdatePropertiesUpward(pNewState);
	else
		pNewState = new NiPropertyState();

	UpdatePropertiesDownward(pNewState);

	if (pNewState)
		delete pNewState;

	pPropertyStateLock->Unlock();
}

// 0xA5A170
void NiAVObject::PushLocalProperties(NiPropertyState* apParentState, bool abCopyOnChange, NiPropertyState*& apOut) {
	ThisStdCall(0xA5A170, this, apParentState, abCopyOnChange, &apOut);
}

// 0x4EFF50
void NiAVObject::UpdateObjectControllers(NiUpdateData& arData, bool abUpdateProperties) {
	ThisStdCall(0x4EFF50, this, arData, abUpdateProperties);
}

// 0xA68BF0
void NiAVObject::UpdateWorldTransform() {
	if (m_pkParent) {
		if (GetSaveExternalGeomData())
			m_kWorld = m_pkParent->m_kWorld;
		else
			m_kWorld = m_pkParent->m_kWorld * m_kLocal;
	}
	else
		m_kWorld = m_kLocal;
}

// 0xB57E30
void NiAVObject::PrepareObject(bool a, bool b) {
	CdeclCall(0xB57E30, this, a, b);
}

// 0xA6D2D0
void NiAVObject::StartAnimations() {
	CdeclCall(0xA6D2D0, this);
}
void NiAVObject::UpdateTransformAndBounds() {
	UpdateTransformAndBounds({});
}

// 0x552790
void __cdecl NiAVObject::UpdateGeomMorphTimeRecurse(NiAVObject* apNode) {
	if (!apNode)
		return;

	NiGeomMorpherController* pGeoMorphController = apNode->GetController<NiGeomMorpherController>();
	if (pGeoMorphController) {
		NiUpdateData pUpdateData = NiUpdateData(*(float*)0x11C3C08, 1, 0);
		pGeoMorphController->Update(pUpdateData);
	}

	if (!IS_NODE(apNode))
		return;

	NiNode* pNode = static_cast<NiNode*>(apNode);
	for (UInt32 i = 0; i < pNode->m_kChildren.m_usSize; ++i)
		NiAVObject::UpdateGeomMorphTimeRecurse(pNode->GetAt(i));
}
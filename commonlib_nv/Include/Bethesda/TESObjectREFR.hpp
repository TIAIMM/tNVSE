#pragma once
#include "bhkNiCollisionObject.hpp"
#include "TESForm.hpp"
#include "NiPoint3.hpp"
#include "TESChildCell.hpp"
#include <optional>
#include <string>

#include "GameRTTI.h"

class hkpRigidBody;
class NiNode;
class NiAVObject;
class NiGeometry;
class Actor;
class ActorCause;
class BSFaceGenNiNode;
class BSFaceGenAnimationData;
class BSAnimNoteReceiver;
class ExtraScript;
class ExtraDroppedItemList;
class ExtraLockData;
class TESContainer;
class TESSound;
class BipedAnim;

struct AnimData;
struct ScriptEventList;

struct LoadedRefData {
    TESObjectREFR* pCurrentWaterType;
    UInt32 iUnderwaterCount;
    Float32 fRelevantWaterHeight;
    Float32 fRevealDistance;
    UInt32 eFlags;
    NiPointer<NiNode> spNode;
    NiNode* pkNiNode18;
};

class TESObjectREFR : public TESForm, TESChildCell {
public:
    static inline auto bs_rtti = RTTI_TESObjectREFR;

    TESSound* pkLoopSound;
    TESForm* pBaseForm;
    NiPoint3 kRotation;
    NiPoint3 kPosition;
    Float32 fRefScale;
    TESObjectCELL* pkParentCell;
    ExtraDataList kExtraList;
    LoadedRefData* pkLoadedData;

    TESObjectREFR();
    ~TESObjectREFR() override;

    virtual bool SetStartingPosition(NiPoint3& arPos, NiPoint3& arRot, TESForm* apParentLocation,
                                     TESObjectCELL* apCell);
    virtual void VoiceSoundFunction();
    virtual void Unk_50();
    virtual void DamageObject(float afDamage, bool abForce);
    virtual bool IsCastShadows();
    virtual void SetCastShadows();
    virtual void GetMotionBlur();
    virtual void Unk_55();
    virtual void Unk_56();
    virtual void IsObstacle();
    virtual void IsQuestObject();
    virtual void SetActorCause(ActorCause* apActorCause);
    virtual ActorCause* GetActorCause();
    virtual NiPoint3* GetStartingAngle(NiPoint3& arRot);
    virtual NiPoint3* GetStartingPos(NiPoint3& arPos);
    virtual void SetStartingPos(NiPoint3 akPos);
    virtual void UpdateLight();
    virtual TESObjectREFR* RemoveItem(TESForm* toRemove, BaseExtraList* extraList, UInt32 count, UInt32 unk3,
                                      UInt32 unk4, TESObjectREFR* destRef, UInt32 unk6, UInt32 unk7, UInt32 unk8,
                                      UInt8 unk9);
    virtual void RemoveByType(int type, char keepOwner, int count);
    virtual bool EquipObject(TESForm* item, UInt32 count, ExtraDataList* xData, bool lockEquip);
    virtual void Unk_62();
    virtual void Unk_63();
    virtual void AddItem(TESForm* apkItem, ExtraDataList* apkExtraDataList, UInt32 auiQuantity);
    virtual NiPoint3* GetBoundCenter(NiPoint3& arBoundCenter);
    virtual void GetMagicCaster();
    virtual void GetMagicTarget();
    virtual bool GetIsChildSize(bool checkHeight);
    virtual UInt32 GetActorUnk0148();
    virtual void SetActorUnk0148(UInt32 arg0);
    virtual NiNode* GetFaceNodeBiped(UInt32 unk = 0);
    virtual BSFaceGenNiNode* GetFaceNodeSkinned(UInt32 unk = 0);
    virtual BSFaceGenNiNode* GetFaceNode(UInt32 unk = 0);
    virtual BSFaceGenAnimationData* CreateFaceAnimationData();
    virtual bool ClampToGround();
    virtual void Unload3D();
    virtual void InitHavok();
    virtual void Load3D(bool abBackgroundLoading);
    virtual void Set3D(NiNode* niNode, bool unloadArt);
    virtual NiNode* Get3D() const;
    virtual void IsBaseFormCastsShadows();
    virtual NiPoint3* GetMinBounds(NiPoint3* apMin);
    virtual NiPoint3* GetMaxBounds(NiPoint3* apMax);
    virtual void UpdateAnimation();
    virtual AnimData* GetAnimData();
    virtual BipedAnim* GetValidBip01Names();
    virtual BipedAnim* CallGetValidBip01Names();
    virtual void SetValidBip01Names(BipedAnim validBip01Names);
    virtual NiPoint3* GetPos() const;
    virtual void Unk_7E(UInt32 arg0);
    virtual void ResetHavokSimulation(bool abRecursive);
    virtual void Unk_80(UInt32 arg0);
    virtual void Unk_81(UInt32 arg0);
    virtual void Unk_82();
    virtual NiNode* GetProjectileNode();
    virtual void Unk_84(UInt32 arg0);
    virtual UInt32 GetSitSleepState();
    virtual bool IsCharacter();
    virtual bool IsCreature();
    virtual bool IsExplosion();
    virtual bool IsProjectile();
    virtual void SetParentCell();
    virtual bool IsDying(bool essentialUnconcious = false);
    virtual bool GetHasKnockedState();
    virtual bool Unk_8D();
    virtual BSAnimNoteReceiver* CreateExtraAnimNoteReceiver();
    virtual BSAnimNoteReceiver* GetExtraDataAnimNoteReceiver();

    bool MoveToEditorLocation(TESObjectCELL* cell) {
        return ThisStdCall<bool>(0x561EF0, this, cell);
    }

    static TESObjectREFR* FindReferenceFor3D(NiAVObject* apObject) {
        return CdeclCall<TESObjectREFR*>(0x56F930, apObject);
    }

    TESObjectCELL* GetNearbyWaterContainingCell(float afRadius) {
        return ThisStdCall<TESObjectCELL*>(0x93BBA0, this, afRadius);
    }

    NiMatrix3* GetOrientation(NiMatrix3 *arRot) {
        return ThisStdCall<NiMatrix3*>(0x56FA00, this, arRot);
    }

    void SetLocationOnReference(const NiPoint3 *position) {
        ThisStdCall(0x575830, this, position);
    }

    void SetPos(const NiPoint3& pos, bool update3d = false);
    void SetRotation(const NiPoint3& rotation, bool update3d = false);
    void SetScale(const float scale);

    void SetNifBlockRotation(const std::string &blockName, const NiPoint3 &rotation) {
	    if (const auto nif = this->Get3D()) {
		    if (const auto block = nif->GetObjectByName(blockName.c_str())) {
                float worldX, worldY, worldZ;
                block->GetWorldRotate().ToEulerAnglesXYZ(worldX, worldY, worldZ);

                block->SetLocalRotateDeg(rotation.x - worldX, rotation.y - worldY, rotation.z - worldZ);
		    }
	    }
    }

    std::optional<NiPoint3> GetNifBlockPos(const std::string &blockName) const {
        if (const auto nif = this->Get3D()) {
            if (const auto block = nif->GetObjectByName(blockName.c_str())) {
                return block->GetWorldTranslate();
            }
        }

        return std::nullopt;
    }

    TESObjectREFR* PlaceAtMe(TESForm *form, int count, float distance, int direction) {
        return CdeclCall<TESObjectREFR*>(0x5C4B30, this, form, count, distance, direction, 1.0f);
    }

    double GetScaledHeight() const {
        return ThisStdCall<double>(0x567400, this);
    }

    TESObjectREFR* GetActionRef() const {
        return ThisStdCall<TESObjectREFR*>(0x572E30, this);
    }

    int GetCalcLevel(char a2) {
        return ThisStdCall<int>(0x567E10, this, a2);
    }

    TESContainer* GetContainer() {
        return ThisStdCall<TESContainer*>(0x55D310, this);
    }

    InventoryChanges* GetContainerChanges() {
        if (!GetContainer()) {
            return nullptr;
        }

        return CdeclCall<InventoryChanges*>(0x4BF220, this);
    }

    void Disable() {
        ThisStdCall(0x574400, this);
    }

    bool Enable() {
        return ThisStdCall<bool>(0x573F40, this);
    }
};

static_assert(sizeof(TESObjectREFR) == 0x68);

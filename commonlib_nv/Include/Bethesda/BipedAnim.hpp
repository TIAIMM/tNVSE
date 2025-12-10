#pragma once

class Actor;
class TESForm;
class TESModelTextureSwap;
class TESObjectARMO;
class TESObjectWEAP;
class TESRace;
class NiNode;

class BipedAnim {
public:
	enum eOptionalBoneType {
		kOptionalBone_Bip01Head = 0,
		kOptionalBone_Weapon = 1,
		kOptionalBone_Bip01LForeTwist = 2,
		kOptionalBone_Bip01Spine2 = 3,
		kOptionalBone_Bip01Neck1 = 4,
	};

	// 008
	struct OptionalBone
	{
		bool	bExists;
		NiNode* pBone;
	};

	// 010
	struct VB01Data
	{
		union
		{
			TESForm*		pItem;
			TESObjectARMO*	pArmor;
			TESObjectWEAP*	pWeapon;
			TESRace*		pRace;
		};
		TESModelTextureSwap* pModelTexture;
		NiNode* pBoneNode;
		UInt32 unk00C;
	};

	NiNode* pRoot;
	OptionalBone kBones[5];
	VB01Data slotData[20];
	VB01Data unk016C[20];
	UInt32 unk2AC;
	Actor* pActor;
};
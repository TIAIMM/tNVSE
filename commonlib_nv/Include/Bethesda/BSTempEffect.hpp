#pragma once

#include "NiObject.hpp"

class BGSSaveGameBuffer;
class BGSLoadGameBuffer;
class BGSLoadFormBuffer;
class TESObjectCELL;
class NiNode;

NiSmartPointer(BSTempEffect);

class BSTempEffect : public NiObject {
public:
	enum Type : UInt32 {
		GEO_DECAL			= 1,
		PARTICLE			= 2,
		SIMPLE_DECAL		= 3,
		MAGIC_HIT			= 4,
		MAGIC_MODEL_HIT		= 5,
		MAGIC_SHADER_HIT	= 6,
	};

	BSTempEffect();
	virtual ~BSTempEffect();

	virtual void			Initialize();								// 35
	virtual BSTempEffect*	GetSelf();									// 36 | ???? Used by particles
	virtual bool			Update(float afTime);						// 37 | Returns true if effect is finished
	virtual NiNode*			Get3D();									// 38 | Returns geometry
	virtual Type			GetType();									// 39 |	Returns 3 for base
	virtual bool			CheckShouldSave();							// 40
	virtual UInt32			GetSaveSize();								// 41
	virtual void			SaveGameBGS(BGSSaveGameBuffer* apBuffer);	// 42
	virtual void			SaveGameTES();								// 43
	virtual void 			LoadGameBGS(BGSLoadGameBuffer* apBuffer);	// 44
	virtual bool			LoadGameTES();								// 45
	virtual void			SetTarget(void*);							// 46 | Used by MagicHitEffect
	virtual void			Unk_47(BGSLoadFormBuffer* apBuffer);		// 47 | ????
	virtual bool			IsFirstPerson() const;						// 48 | Used by shell casings

	float			fLifetime;
	TESObjectCELL*	pCell;
	float			fAge;
	bool			bInitialized;
};

ASSERT_SIZE(BSTempEffect, 0x18);
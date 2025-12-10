#pragma once

#include "NiAVObject.hpp"
#include "NiTPointerList.hpp"

class NiNode;

typedef NiTPointerList<NiNode*> NiNodeList;

class NiDynamicEffect : public NiAVObject {
public:
	NiDynamicEffect();
	virtual ~NiDynamicEffect();

    enum EffectType {
        AMBIENT_LIGHT	= 0,
        POINT_LIGHT		= 2,
        DIR_LIGHT		= 3,
        SPOT_LIGHT		= 4,
        TEXTURE_EFFECT	= 5,
    };

	bool		m_bOn;
	UInt8		m_ucEffectType;
	union {
		struct {
			bool		bResetTraits;
			Bitfield8   ucExtraFlags;
		};
		bool	bCastShadows;
		bool	bCanCarry;
	};
	SInt32		m_iIndex;
	UInt32		m_uiPushCount;
	UInt32		m_uiRevID;
	NiNodeList	m_pkShadowGenerator;
	NiNodeList	m_kUnaffectedNodeList;

	__forceinline bool IsAmbient()		const { return m_ucEffectType == AMBIENT_LIGHT;  }
	__forceinline bool IsPoint()		const { return m_ucEffectType == POINT_LIGHT;	 }
	__forceinline bool IsDirectional()	const { return m_ucEffectType == DIR_LIGHT;		 }
	__forceinline bool IsSpot()			const { return m_ucEffectType == SPOT_LIGHT;	 }
	__forceinline bool IsTexture()		const { return m_ucEffectType == TEXTURE_EFFECT; }

	void IncRevisionID() { m_uiRevID++; }
};

ASSERT_SIZE(NiDynamicEffect, 0xC4)
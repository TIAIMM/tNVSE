#pragma once

#include "NiInterpolator.hpp"
#include "NiTimeController.hpp"
#include "NiFixedString.hpp"
#include "BSAnimNote.hpp"
#include "NiInterpController.hpp"

class NiTextKeyExtraData;
class NiControllerManager;
class NiStringPalette;
class NiBlendInterpolator;
class NiInterpController;

NiSmartPointer(NiControllerSequence);

class NiControllerSequence : public NiObject {
public:
	NiControllerSequence();
	virtual ~NiControllerSequence();
	virtual bool Deactivate(float afEaseOutTime, bool);

	enum AnimState : UInt32 {
		INACTIVE,
		ANIMATING,
		EASEIN,
		EASEOUT,
		TRANSSOURCE,
		TRANSDEST,
		MORPHSOURCE
	};

	struct InterpArrayItem {
		NiInterpolatorPtr		m_spInterpolator;
		NiInterpControllerPtr	m_spInterpCtlr;
		NiBlendInterpolator*	m_pkBlendInterp;
		UInt8					m_ucBlendIdx;
		UInt8					m_ucPriority;
	};

	struct IDTag {
		NiFixedString m_kAVObjectName;
		NiFixedString m_kPropertyType;
		NiFixedString m_kCtlrType;
		NiFixedString m_kCtlrID;
		NiFixedString m_kInterpolatorID;
	};

	NiFixedString					m_kName;
	UInt32							m_uiArraySize;
	UInt32							m_uiArrayGrowBy;
	InterpArrayItem*				m_pkInterpArray;
	IDTag*							m_pkIDTagArray;
	float							m_fSeqWeight;
	NiTextKeyExtraData*				m_spTextKeys;
	NiTimeController::CycleType		m_eCycleType;
	float							m_fFrequency;
	float							m_fBeginKeyTime;
	float							m_fEndKeyTime;
	float							m_fLastTime;
	float							m_fWeightedLastTime;
	float							m_fLastScaledTime;
	NiControllerManager*			m_pkOwner;
	AnimState						m_eState;
	float							m_fOffset;
	float							m_fStartTime;
	float							m_fEndTime;
	float							m_fDestFrame;
	NiControllerSequence*			m_pkPartnerSequence;
	NiFixedString					m_kAccumRootName;
	NiAVObject*						m_pkAccumRoot;
	NiObjectPtr						m_spDeprecatedStringPalette;
	UInt16							usCurAnimNIdx;
	BSAnimNotePtr*					spAnimNotes;
	UInt16							usNumNotes;
	bool							bRemovableObjects;

	CREATE_OBJECT(NiControllerSequence, 0xA326C0);
	NIRTTI_ADDRESS(0x11F36DC);

	static NiControllerSequence* Create(const NiFixedString& arName, UInt32 auiArraySize = 12, UInt32 auiArrayGrowBy = 12);

	bool Activate(char acPriority, bool abStartOver, float afWeight, float afEaseInTime, NiControllerSequence* apTimeSyncSeq, bool asbTransition);

	bool StartMorph(NiControllerSequence* apDestSequence, float afDuration, SInt32 aiPriority, float afSourceWeight, float afDestWeight);
	bool StartBlend(NiControllerSequence* apDestSequence, float afDuration, float afDestFrame, SInt32 aiPriority, float afSourceWeight, float afDestWeight, NiControllerSequence* apTimeSyncSeq);

	void GetInterpInfoAt(UInt32 auiIndex, const char*& pcAVObjectName, const char*& pcPropertyType, const char*& pcCtlrType, const char*& pcCtlrID, const char*& pcInterpolatorID);

	NiAVObject* GetAccumRoot() const;

	UInt32 AddInterpolator(NiInterpolator* apInterpolator, IDTag& arTag, UInt8 aucPriority);
	NiInterpolator* GetInterpolatorAt(UInt32 auiIndex) const;
	void SetControllerAt(NiInterpController* apInterpCtlr, UInt32 auiIndex);
	void ReplaceInterpolatorAt(NiInterpolator* apInterp, UInt32 auiIndex);
	NiInterpController* GetControllerAt(UInt32 auiIndex) const;

	NiControllerManager* GetOwner() const;
	bool SetOwner(NiControllerManager* pkOwner);

	void SetName(const char* apName);
	const char* GetName() const;

	void Update(float afTime, bool abUpdateInterpolators) {
		ThisStdCall(0xA34BA0, this, afTime, abUpdateInterpolators);
	}
};

ASSERT_SIZE(NiControllerSequence, 0x74);
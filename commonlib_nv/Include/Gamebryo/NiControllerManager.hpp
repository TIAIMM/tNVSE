#pragma once

#include "NiTimeController.hpp"
#include "NiTArray.hpp"
#include "NiTStringMap.hpp"
#include "NiControllerSequence.hpp"
#include "NiDefaultAVObjectPalette.hpp"
#include "NiTSet.hpp"
#include "BSAnimNoteListener.hpp"

class NiControllerManager : public NiTimeController {
public:
	NiTObjectArray<NiPointer<NiControllerSequence>>		m_kSequenceArray;
	NiTPrimitiveSet<NiControllerSequence*>				m_kActiveSequences;
	NiTStringPointerMap<NiControllerSequence*>			m_kIndexMap;
	BSAnimNoteListener*									pListener;
	bool												m_bCumulative;
	NiTObjectSet<NiPointer<NiControllerSequence> >		m_kTempBlendSeqs;
	NiDefaultAVObjectPalettePtr							m_spObjectPalette;

	CREATE_OBJECT(NiControllerManager, 0xA2F6C0);
	NIRTTI_ADDRESS(0x11F36AC);

	static NiControllerManager* Create(NiAVObject* apTarget, bool abCumulativeAnimations = false);

	NiAVObject* GetAccumRoot() const;
	NiControllerSequence* GetSequenceByName(const NiFixedString& arName);
	NiControllerSequence* GetSequenceAt(UInt32 auiIndex) const;

	UInt32 GetSequenceCount() const;

	bool Morph(NiControllerSequence* apSourceSequence, NiControllerSequence* apDestSequence, float afDuration, SInt32 aiPriority = 0, float afSourceWeight = 1.0f, float afDestWeight = 1.0f);
	bool CrossFade(NiControllerSequence* apSourceSequence, NiControllerSequence* apDestSequence, float afDuration, SInt32 aiPriority = 0, bool abStartOver = false, float afWeight = 1.0f, NiControllerSequence* apTimeSyncSeq = nullptr);
	
	NiControllerSequence* CreateTempBlendSequence(NiControllerSequence* apSequence, NiControllerSequence* apSequenceToSynchronize);
	bool BlendFromPose(NiControllerSequence* apSequence, float afDestFrame, float afDuration, SInt32 aiPriority = 0, NiControllerSequence* apSequenceToSynchronize = nullptr);

	bool AddSequence(NiControllerSequence* apSequence, const NiFixedString& arName, bool abStoreTargets = true);
	void RemoveSequence(NiControllerSequence* apSequence);
	void RemoveSequence(NiControllerSequence* apSequence, NiControllerSequencePtr& arOut);

	bool ActivateSequence(NiControllerSequence* apSequence, char acPriority, bool abStartOver, float afWeight, float afEaseInTime, NiControllerSequence* apTimeSyncSeq);
	bool DeactivateSequence(NiControllerSequence* apSequence, float afEaseOutTime);
	bool DeactivateTransSourceSequence(float afEaseOutTime);
	void DeactivateAll(float afEaseOutTime);
};

ASSERT_SIZE(NiControllerManager, 0x7C);
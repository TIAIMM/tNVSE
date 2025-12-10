#pragma once

#include "TESBoundAnimObject.hpp"
#include "TESSoundFile.hpp"

class BSSoundHandle;

class TESSound : public TESBoundAnimObject, public TESSoundFile {
public:
	TESSound();
	~TESSound();

	enum TESSoundFlags
	{
		kFlag_RandomFrequencyShift = 1,
		kFlag_PlayAtRandom = 2,
		kFlag_EnvironmentIgnored = 4,
		kFlag_RandomLocation = 8,
		kFlag_Loop = 16,
		kFlag_MenuSound = 32,
		kFlag_2D = 64,
		kFlag_360LFE = 128,
		kFlag_DialogueSound = 256,
		kFlag_EnvelopeFast = 512,
		kFlag_EnvelopeSlow = 1024,
		kFlag_2DRadius = 2048,
		kFlag_MuteWhenSubmerged = 4096,
		kFlag_StartAtRandomPosition = 8192,
	};

	struct Data {
		UInt8		ucMinAttenuationDist;
		UInt8		ucMaxAttenuationDist;
		UInt8		ucFrequencyAdj;
		UInt8		byte03;
		Bitfield32	uiSoundFlags;
		UInt16		usStaticAttenuation;
		union {
			UInt16 usStartEnd;
			struct {
				UInt8		ucEndsAt;
				UInt8		ucStartsAt;
			};
		};
		UInt16		usAttenuationCurve[5];
		UInt16		usReverbAttenuation;
		UInt32		uiPriority;
		UInt32		uiLoopPointBegin;
		UInt32		uiLoopPointEnd;

		UInt32 ConvertFlags() const;
	};

	BSString	strEditorID;
	Data		kData;
	UInt8		ucRngChance;

	void SetFlag(UInt32 pFlag, bool bMod) {
		kData.uiSoundFlags.SetBit(pFlag, bMod);
	}

	bool GetFlag(UInt32 pFlag) const {
		return kData.uiSoundFlags.IsSet(pFlag);
	}

	UInt32 GetStartsAt() const;
	UInt32 GetEndsAt() const;
	TESSound::Data& GetData(TESSound::Data& arData) const;

	const char* GetFilePath() const;

	static TESSound* GetNoActivateSound();

	static BSSoundHandle* GetUnderwaterAmbiance();
	static void SetUnderwaterAmbiance(BSSoundHandle& arHandle);
	static void SetUnderwaterAmbiance(BSSoundHandle* apHandle);

	static void InitFootstepSounds();
	static void ResetPlayingWeaponSounds();

	static void UpdateRegionSounds();
};

ASSERT_SIZE(TESSound, 0x6C);
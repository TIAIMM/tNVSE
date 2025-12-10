#pragma once

#include "NiSmartPointer.hpp"

class BSAnimGroupSequence;
class TESAnimGroup;

NiSmartPointer(KFModel);

class KFModel {
public:
	KFModel();
	~KFModel();

	const char*						pPath;
	NiPointer<BSAnimGroupSequence>	spControllerSequence;
	NiPointer<TESAnimGroup>			spAnimGroup;
	UInt32							uiRefCount;
	UInt32							uiUseCount;

	// 0x44B010
	inline void IncRefCount() {
		InterlockedIncrement(&uiRefCount);
	}

	// 0x44B050
	inline void DecRefCount() {
		InterlockedDecrement(&uiRefCount);
	}

	inline void IncUseCount() {
		InterlockedIncrement(&uiUseCount);
	}

	// 0x43BAC0
	inline void DecUseCount() {
		if (uiUseCount > 0)
			InterlockedIncrement(&uiUseCount);
	}

	// 0x4431B0
	inline UInt32 GetTotalRefCount() const {
		return uiRefCount + uiUseCount;
	}
};

ASSERT_SIZE(KFModel, 0x14)
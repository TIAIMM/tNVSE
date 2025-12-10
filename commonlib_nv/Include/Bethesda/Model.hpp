#pragma once

#include "NiNode.hpp"

class BSStream;

NiSmartPointer(Model);

class Model : public BSMemObject {
public:
	Model();
	Model(const char* apPath, BSStream& arStream, bool abAssignShaders, bool abKeepUV);
	~Model();

	const char* pcPath;
	UInt32		uiRefCount;
	UInt32		uiUseCount;
	NiNodePtr	spNode;

	inline void IncRefCount() {
		InterlockedIncrement(&uiRefCount);
	}

	// 0x40C130
	inline void DecRefCount() {
		InterlockedDecrement(&uiRefCount);
	}

	inline void IncUseCount() {
		InterlockedIncrement(&uiUseCount);
	}

	inline void DecUseCount() {
		if (uiUseCount > 0)
			InterlockedDecrement(&uiUseCount);
	}

	inline UInt32 GetTotalRefCount() const {
		return uiUseCount + uiRefCount;
	}

	void Initialize(const char* apPath, BSStream &arStream, bool abAssignShaders, bool abKeepUV);
};

ASSERT_SIZE(Model, 0x10)
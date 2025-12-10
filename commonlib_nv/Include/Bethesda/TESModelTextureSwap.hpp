#pragma once

#include "TESModel.hpp"
#include "BSSimpleList.hpp"

class TESModelTextureSwap : public TESModel {
public:
	TESModelTextureSwap();
	~TESModelTextureSwap();

	struct Texture {
		UInt32	uiTextureID;
		UInt32	uiIndex3D;
		char	cTextureName[128];
	};

	BSSimpleList<Texture*> kTextureList;
};

ASSERT_SIZE(TESModelTextureSwap, 0x20);
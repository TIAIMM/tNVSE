#pragma once

#include "TESTexture.hpp"

class TESIcon : public TESTexture {
public:
	static inline auto bs_rtti = RTTI_TESIcon;

	TESIcon();
	~TESIcon();

	void SetPath(const char* newPath) { strTextureName.Set(newPath); }
};
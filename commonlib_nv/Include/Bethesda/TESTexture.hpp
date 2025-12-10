#pragma once

#include "BaseFormComponent.hpp"
#include "BSString.hpp"

class TESTexture : public BaseFormComponent {
public:
	TESTexture();
	~TESTexture();

	virtual UInt32		GetMaxAllowedSize() const;
	virtual const char* GetAsNormalFile(BSString& arStr) const;
	virtual const char* GetDefaultPath() const;

	BSString strTextureName;

	const char* GetTextureName();
	UInt32 GetTextureNameLength();
};
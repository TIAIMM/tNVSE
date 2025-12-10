#pragma once

#include "BSShaderProperty.hpp"
#include "BSString.hpp"

NiSmartPointer(BSShaderNoLightingProperty);

class BSShaderNoLightingProperty : public BSShaderProperty {
public:
	BSShaderNoLightingProperty();
	virtual ~BSShaderNoLightingProperty();

	virtual UInt32							GetClampD3D() const;
	virtual void							SetClampD3D(UInt32 auiClampMode);
	virtual NiTexturingProperty::ClampMode	GetClampNi() const;
	virtual void							SetClampNi(NiTexturingProperty::ClampMode aeClampMode);

	struct FalloffData {
		float fFalloffStartAngle;
		float fFalloffStopAngle;
		float fFalloffStartOpacity;
		float fFalloffStopOpacity;
	};

	NiTexturePtr					spTexture;
	BSString						strTexturePath;
	NiTexturingProperty::ClampMode	eClampMode;
	FalloffData						kFalloffData;

	CREATE_OBJECT(BSShaderNoLightingProperty, 0xB6FE80);

	static bool bCullFakeShadows;

	void SetTexture(NiTexture* apTexture) {
		spTexture = apTexture;
		iLastRenderPassState = 0;
	}

	RenderPassArray* GetRenderPassesEx(NiGeometry* apGeometry, UInt32 auiEnabledPasses, WORD* pusPassCount, UInt32 aeRenderMode, BSShaderAccumulator* apAccumulator, bool abAddPass);
};

ASSERT_SIZE(BSShaderNoLightingProperty, 0x80);
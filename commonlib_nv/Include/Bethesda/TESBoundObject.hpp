#pragma once

#include "TESObject.hpp"

class NiNode;

class TESBoundObject : public TESObject {
public:
	static inline auto bs_rtti = RTTI_TESBoundObject;

	TESBoundObject();
	~TESBoundObject();

	virtual NiNode* Clone3DAlt(TESObjectREFR* apRequester);
	virtual bool	ReplaceModelAlt(const char* apPath);

	struct Bounds {
		Bounds() : x(0), y(0), z(0) {};
		Bounds(SInt16 aX, SInt16 aY, SInt16 aZ) : x(aX), y(aY), z(aZ) {};
		SInt16 x = 0;
		SInt16 y = 0;
		SInt16 z = 0;

		Bounds& operator-(const Bounds& aOther) const;

		float Length() const;
	};

	struct BoundData {
		Bounds BoundMin;
		Bounds BoundMax;
	};

	BoundData kBoundData;

	float GetBoundSize() const;
};

ASSERT_SIZE(TESBoundObject, 0x30)
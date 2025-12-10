#pragma once

#include "BSSimpleList.hpp"
#include "FastModifierCollection.hpp"
#include "Modifier.hpp"

struct ModifierList : public BSSimpleList<Modifier*> {
public:
	ModifierList() : bAllowEmpty(false), pFastModifiers(nullptr) {}
	~ModifierList() { DeleteAllModifiers(); }

	enum ClampStyle : UInt32 {
		CLAMP_ABOVE_ZERO = 0, // Clamps values > 0 to 0
		CLAMP_BELOW_ZERO = 1, // Clamps values < 0 to 0
		CLAMP_NONE		 = 2, // No clamping
	};

	bool					bAllowEmpty;
	FastModifierCollection* pFastModifiers;

	Modifier* GetModifierItem(UInt8 aucActorValue) const;
	float GetModifier(UInt8 aucActorValue, bool& abFound) const;
	float GetModifier(UInt8 aucActorValue) const;

	void SetModifier(UInt8 aucActorValue, float afValue);
	void AdjustModifier(UInt8 aucActorValue, float afValue, ClampStyle aeClamp);

	void AddModifier(Modifier* apModifier);
	void AddFastModifier(UInt8 aucActorValue, Modifier* apModifier);

	void DeleteModifier(Modifier* apModifier);
	void DeleteAllModifiers();

	static float ModifyAndClamp(float afValue, float afDelta, ClampStyle aeClamp);
};

ASSERT_SIZE(ModifierList, 0x10);
#pragma once

#include "NiProperty.hpp"

NiSmartPointer(NiStencilProperty);

class NiStencilProperty : public NiProperty {
public:
	NiStencilProperty();
	virtual ~NiStencilProperty();

	enum TestFunc {
		TEST_NEVER,
		TEST_LESS,
		TEST_EQUAL,
		TEST_LESSEQUAL,
		TEST_GREATER,
		TEST_NOTEQUAL,
		TEST_GREATEREQUAL,
		TEST_ALWAYS,
		TEST_MAX
	};

	enum Action {
		ACTION_KEEP,
		ACTION_ZERO,
		ACTION_REPLACE,
		ACTION_INCREMENT,
		ACTION_DECREMENT,
		ACTION_INVERT,
		ACTION_MAX
	};

	enum {
		ENABLE_MASK			= 0x1,
		FAILACTION_MASK		= 0xE,
		FAILACTION_POS		= 0x1,
		ZFAILACTION_MASK	= 0x70,
		ZFAILACTION_POS		= 0x4,
		PASSACTION_MASK		= 0x380,
		PASSACTION_POS		= 0x7,
		DRAWMODE_MASK		= 0xC00,
		DRAWMODE_POS		= 0xA,
		TESTFUNC_MASK		= 0xF000,
		TESTFUNC_POS		= 0xC,
	};

	enum DrawMode {
		DRAW_CCW_OR_BOTH	= 0,
		DRAW_CCW			= 1,
		DRAW_CW				= 2,
		DRAW_BOTH			= 3,
		DRAW_MAX,
	};

	Bitfield16	m_usFlags;
	UInt32		m_uiRef;
	UInt32		m_uiMask;

	CREATE_OBJECT(NiStencilProperty, 0xA6F410);

	bool IsEnabled() const {
		return m_usFlags.GetBit(ENABLE_MASK);
	}

	void SetDrawMode(NiStencilProperty::DrawMode aeDraw) {
		m_usFlags.SetField(aeDraw, DRAWMODE_MASK, DRAWMODE_POS);
	}

	NiStencilProperty::DrawMode GetDrawMode() const {
		return (NiStencilProperty::DrawMode)m_usFlags.GetField(DRAWMODE_MASK, DRAWMODE_POS);
	}
};

ASSERT_SIZE(NiStencilProperty, 0x24);
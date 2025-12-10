#pragma once

#include "NiProperty.hpp"

NiSmartPointer(NiAlphaProperty);

class NiAlphaProperty : public NiProperty {
public:
	NiAlphaProperty();
	virtual ~NiAlphaProperty();

	enum AlphaFlags {
		ALPHA_BLEND_MASK	= 0x0001,
		SRC_BLEND_MASK		= 0x001e,
		SRC_BLEND_POS		= 1,
		DEST_BLEND_MASK		= 0x01e0,
		DEST_BLEND_POS		= 5,

		TEST_ENABLE_MASK	= 0x0200,
		TEST_FUNC_MASK		= 0x1c00,
		TEST_FUNC_POS		= 10,
		ALPHA_NOSORTER_MASK = 0x2000
	};

	enum AlphaFunction
	{
		ALPHA_ONE			= 0,
		ALPHA_ZERO			= 1,
		ALPHA_SRCCOLOR		= 2,
		ALPHA_INVSRCCOLOR	= 3,
		ALPHA_DESTCOLOR		= 4,
		ALPHA_INVDESTCOLOR	= 5,
		ALPHA_SRCALPHA		= 6,
		ALPHA_INVSRCALPHA	= 7,
		ALPHA_DESTALPHA		= 8,
		ALPHA_INVDESTALPHA	= 9,
		ALPHA_SRCALPHASAT	= 10,
		ALPHA_MAX_MODES
	};

	enum TestFunction {
		TEST_ALWAYS			= 0,
		TEST_LESS			= 1,
		TEST_EQUAL			= 2,
		TEST_LESSEQUAL		= 3,
		TEST_GREATER		= 4,
		TEST_NOTEQUAL		= 5,
		TEST_GREATEREQUAL	= 6,
		TEST_NEVER			= 7,
		TEST_MAX_MODES
	};


	Bitfield16	m_usFlags;
	UInt8		m_ucAlphaTestRef;

	CREATE_OBJECT(NiAlphaProperty, 0xA5CEB0);
	NIRTTI_ADDRESS(0x11F4408);

	bool GetAlphaBlending() const {
		return m_usFlags.GetBit(ALPHA_BLEND_MASK);
	}

	bool HasAlphaTest() const {
		return m_usFlags.GetBit(TEST_ENABLE_MASK);
	}

	bool HasAlphaSorter() const {
		return m_usFlags.GetBit(ALPHA_NOSORTER_MASK);
	}

	UInt8 GetAlphaTestRef() const {
		return m_ucAlphaTestRef;
	}

	void SetAlphaTestRef(UInt8 ucRef) {
		m_ucAlphaTestRef = ucRef;
	}

	void SetAlphaBlending(bool abBlend) {
		m_usFlags.SetBit(ALPHA_BLEND_MASK, abBlend);
	}

	void SetAlphaTesting(bool abTest) {
		m_usFlags.SetBit(TEST_ENABLE_MASK, abTest);
	}

	void SetSrcBlendMode(AlphaFunction aeSrcBlend) {
		m_usFlags.SetField(aeSrcBlend, SRC_BLEND_MASK, ALPHA_BLEND_MASK);
	}

	void SetDestBlendMode(AlphaFunction eDestBlend) {
		m_usFlags.SetField(eDestBlend, DEST_BLEND_MASK, DEST_BLEND_POS);
	}

	void SetTestRef(UInt8 aucRef) {
		m_ucAlphaTestRef = aucRef;
	}

	void SetTestMode(TestFunction aeTestFunc) {
		m_usFlags.SetField(aeTestFunc, TEST_FUNC_MASK, TEST_FUNC_POS);
	}
};

ASSERT_SIZE(NiAlphaProperty, 0x01C);
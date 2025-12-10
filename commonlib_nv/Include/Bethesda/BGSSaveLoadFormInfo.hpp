#pragma once

class BGSSaveLoadFormInfo {
public:
	UInt8 ucData;

	void	SetFormType(UInt32 aeFormType);
	UInt32	GetFormType();
};

ASSERT_SIZE(BGSSaveLoadFormInfo, 0x1);
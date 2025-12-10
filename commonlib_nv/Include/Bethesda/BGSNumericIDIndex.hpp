#pragma once

struct BGSNumericIDIndex {
	UInt8 ucData1;
	UInt8 ucData2;
	UInt8 ucData3;

	void SetNumericID(UInt32 auiID);
	UInt32 GetNumericID() const;
};

ASSERT_SIZE(BGSNumericIDIndex, 0x3);
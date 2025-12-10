#pragma once

#include "BSHash.hpp"

class BSFileEntry : public BSHash {
public:
	UInt32 uiSize;
	UInt32 uiOffset;

	UInt32 GetFileSize() const {
		return uiSize & 0x3FFFFFFF;
	}

	UInt32 GetFileOffset() const {
		return uiOffset & 0x7FFFFFFF;
	}
	
	bool IsCompressed() const {
		return (uiSize & 0x40000000) == 0;
	}

	bool IsInvalidated() const {
		return (uiSize & 0x80000000) != 0;
	}
};

ASSERT_SIZE(BSFileEntry, 0x10);
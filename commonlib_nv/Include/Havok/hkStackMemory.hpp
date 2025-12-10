#pragma once

class hkStackMemory {
public:
	hkStackMemory() {};
	~hkStackMemory() {};

	void* m_next	= nullptr;
	void* m_start	= nullptr;
	void* m_end		= nullptr;

	void initMemory(void* p, UInt32 size);
};

ASSERT_SIZE(hkStackMemory, 0xC);
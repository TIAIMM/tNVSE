#pragma once

#include "hkMemoryAllocator.hpp"
#include "hkStackMemory.hpp"

class hkMemoryRouter {
public:
	hkMemoryRouter() {};
	~hkMemoryRouter() {};

	hkStackMemory		m_stack;
	hkMemoryAllocator*	m_temp		= nullptr;
	hkMemoryAllocator*	m_heap		= nullptr;
	hkMemoryAllocator*	m_debug		= nullptr;
	hkMemoryAllocator*	m_solver	= nullptr;
	hkMemoryAllocator*	m_global	= nullptr;
	void*				m_userData	= nullptr;
};

ASSERT_SIZE(hkMemoryRouter, 0x24);
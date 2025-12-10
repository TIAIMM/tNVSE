#pragma once

#include "BSMemObject.hpp"
#include "ThreadSafeStructures.hpp"

class InterfacedClass : public BSMemObject {
public:
	class BaseInterface {
		InterfacedClass* pOwner;
	};

	InterfacedClass();
	virtual					~InterfacedClass();
	virtual BaseInterface*	AllocateInterface(UInt32 auiThread);		// not implemented
};

ASSERT_SIZE(InterfacedClass, 0x4);
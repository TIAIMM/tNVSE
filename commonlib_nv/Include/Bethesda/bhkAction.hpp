#pragma once

#include "bhkSerializable.hpp"

class bhkAction : public bhkSerializable {
public:
	virtual void Update(float afTime);		// 49
};
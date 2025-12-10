#pragma once

#include "bhkSerializable.hpp"
#include "bhkCharacterPointCollector.hpp"

class hkpCharacterProxy;

class bhkCharacterProxy : public bhkSerializable {
public:
	bhkCharacterPointCollector kPointCollector;

	hkpCharacterProxy* GethkObject() const;
};

ASSERT_SIZE(bhkCharacterProxy, 0x3E0);
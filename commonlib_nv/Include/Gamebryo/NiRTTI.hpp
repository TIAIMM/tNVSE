#pragma once

#include "NiMemObject.hpp"

class NiRTTI : public NiMemObject {
public:
	NiRTTI() { m_pcName = NULL; m_pkBaseRTTI = NULL; }
	NiRTTI(const char* name, const NiRTTI* const base) { m_pcName = name; m_pkBaseRTTI = base; }
	NiRTTI(const char* name, const NiRTTI& base) { m_pcName = name; m_pkBaseRTTI = &base; }

	const char* m_pcName;
	const NiRTTI* m_pkBaseRTTI;

	const char* GetName() const { return m_pcName; }
	const NiRTTI* GetBase() const { return m_pkBaseRTTI; }

	bool IsKindOf(const NiRTTI& apRTTI) const {
		for (const NiRTTI* i = this; i; i = i->GetBase()) {
			if (i == &apRTTI)
				return true;
		}
		return false;
	}

	bool IsKindOf(const NiRTTI* apRTTI) const {
		for (const NiRTTI* i = this; i; i = i->GetBase()) {
			if (i == apRTTI)
				return true;
		}
		return false;
	}

	template <typename T_RTTI>
	bool IsKindOf() const {
		return IsKindOf(T_RTTI::ms_RTTI);
	}

	bool IsExactKindOf(const NiRTTI* const apRTTI) const {
		return this == apRTTI;
	}

	bool IsExactKindOf(const NiRTTI& apRTTI) const {
		return this == &apRTTI;
	}

	template <typename T_RTTI>
	bool IsExactKindOf() const {
		return IsExactKindOf(T_RTTI::ms_RTTI);
	}
};

ASSERT_SIZE(NiRTTI, 0x8);

#define NiImplementRootRTTI(rootclassname) \
NiRTTI rootclassname::ms_RTTI(#rootclassname, 0);

#define NiImplementRTTI(classname, baseclassname) \
static inline const NiRTTI ms_RTTI = NiRTTI(#classname, baseclassname##::ms_RTTI);

#define NiImplementGetRTTI(classname) \
inline const NiRTTI* GetRTTIEx() {return &classname##::ms_RTTI;}

#define NiNewRTTI(classname, baseclassname) \
NiImplementRTTI(classname, baseclassname) \
NiImplementGetRTTI(classname)

#define NIRTTI_ADDRESS(address) \
static inline const NiRTTI* const ms_RTTI = (NiRTTI*)address;
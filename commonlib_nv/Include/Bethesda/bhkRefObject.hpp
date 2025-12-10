#pragma once

#include "NiObject.hpp"
#include "hkReferencedObject.hpp"

class NiAVObject;
class hkVector4;
class NiPoint3;

NiSmartPointer(bhkRefObject);

class bhkRefObject : public NiObject {
public:
	bhkRefObject();
	virtual ~bhkRefObject();

	virtual void SethkObject(hkReferencedObject* aphkObject);	// 35
	virtual void Link(bool abAttach);							// 36			

	hkReferencedObject* phkObject;

	hkReferencedObject* GethkObject() const;

	NiAVObject* GetAVObject(NiRTTI* apRTTI = nullptr) const;

	void SetLinearVelocity(const hkVector4& arVector);
	void SetAngularVelocity(const hkVector4& arVector);

	hkVector4& GetLinearVelocity() const;
	NiPoint3& GetLinearVelocity(NiPoint3& arVector) const;

	hkVector4& GetAngularVelocity() const;
	NiPoint3& GetAngularVelocity(NiPoint3& arVector) const;
};

ASSERT_SIZE(bhkRefObject, 0xC);
#pragma once

class hkpEntity;

class hkpEntityListener {
public:
	hkpEntityListener();
	virtual		 ~hkpEntityListener();
	virtual void entityAddedCallback(hkpEntity* entity);
	virtual void entityRemovedCallback(hkpEntity* entity);
	virtual void entityShapeSetCallback(hkpEntity* entity);
	virtual void entitySetMotionTypeCallback(hkpEntity* entity);
	virtual void entityDeletedCallback(hkpEntity* entity);
};

ASSERT_SIZE(hkpEntityListener, 0x4);
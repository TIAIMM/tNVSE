#pragma once

#include "QueuedKF.hpp"
#include "NiObject.hpp"

class Actor;

NiSmartPointer(QueuedAnimIdle);
NiSmartPointer(AnimIdle);

class QueuedAnimIdle : public QueuedKF {
public:
	QueuedAnimIdle();
	~QueuedAnimIdle();

	Actor*		pActor;
	AnimIdlePtr spAnimation;
};

ASSERT_SIZE(QueuedAnimIdle, 0x40)
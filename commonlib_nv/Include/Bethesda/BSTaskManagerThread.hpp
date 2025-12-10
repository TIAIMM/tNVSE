#pragma once

#include "BSTaskThread.hpp"

template <typename T>
class BSTaskManager;

template <typename T>
class BSTaskManagerThread : public BSTaskThread {
public:
	BSTaskManager<T>* pOwner;
};

ASSERT_SIZE(BSTaskManagerThread<SInt64>, 0x34);
#pragma once

#include "QueuedFileEntry.hpp"
#include "KFModel.hpp"

class QueuedKF : public QueuedFileEntry {
public:
	QueuedKF();
	~QueuedKF();

	KFModelPtr	spKFModel;
	UInt8		unk034;
};

ASSERT_SIZE(QueuedKF, 0x38)
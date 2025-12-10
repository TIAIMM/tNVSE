#pragma once

#include "BaseFormComponent.hpp"
#include "QueuedFile.hpp"

class BGSPreloadable : public BaseFormComponent {
public:
	BGSPreloadable();
	virtual ~BGSPreloadable();

	virtual void	Preload(IO_TASK_PRIORITY aePriority, QueuedFile* apFile);
};
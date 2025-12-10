#include "IOTask.hpp"
#include "IOManager.hpp"

bool IOTask::CompareState(BS_TASK_STATE Comperand, BS_TASK_STATE Exchange) {
    return InterlockedCompareExchange(reinterpret_cast<LONG*>(&eState), LONG(Exchange), LONG(Comperand)) == Comperand;
}

bool IOTask::IsCancelled() const {
    return eState == BS_TASK_STATE_CANCELED;
}

void IOTask::AddToPostProcessQueue() {
    IOManager::GetSingleton()->AddPostProcessTask(this);
}

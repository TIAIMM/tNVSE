#include "BSPackedTaskQueue.hpp"

// GAME - 0x87A890
bool BSPackedTaskQueue::AddTask(BSPackedTask& akTask) {
    return Queue.Push(&akTask);
}

// GAME - 0x86CDD0
void BSPackedTaskQueue::Wait() {
    Queue.kSemaphore.Wait();
}

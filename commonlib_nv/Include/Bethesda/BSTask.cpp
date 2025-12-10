#include "BSTask.hpp"

UInt32* kBSTaskCounter = (UInt32*)0x0011C3B38;

UInt32* GetCounterSingleton() {
	return kBSTaskCounter;
}
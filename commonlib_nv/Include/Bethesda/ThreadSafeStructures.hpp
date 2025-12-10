#pragma once


class ThreadSafeStructures {

	static bool Compare(void* destination, void* exchange, void* compare) {
		return InterlockedCompareExchange((LONG*)destination, (LONG)exchange, (LONG)compare) == (LONG)compare;
	}

};
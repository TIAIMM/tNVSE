#include "hkpPhantom.hpp"

// GAME - 0x4B5950
hkpPhantom* hkpGetPhantom(const hkpCollidable* apCollidable) {
	return CdeclCall<hkpPhantom*>(0x4B5950, apCollidable);
}

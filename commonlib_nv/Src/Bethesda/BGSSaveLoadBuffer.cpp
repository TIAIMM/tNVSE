#include "BGSSaveLoadBuffer.hpp"

BGSSaveLoadBuffer::BGSSaveLoadBuffer() {
	pBuffer = nullptr;
}

BGSSaveLoadBuffer::~BGSSaveLoadBuffer() {
	DeleteBuffer();
}

void BGSSaveLoadBuffer::AllocateBuffer(UInt32 auiSize) {
	pBuffer = new char[auiSize];
}

void BGSSaveLoadBuffer::CopyBuffer(const char* apBuffer, UInt32 auiSize) {
	AllocateBuffer(auiSize);
	memcpy_s(pBuffer, auiSize, apBuffer, auiSize);
}

void BGSSaveLoadBuffer::DeleteBuffer() {
	if (pBuffer) {
		delete[] pBuffer;
		pBuffer = nullptr;
	}
}

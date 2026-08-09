#pragma once
#include <Windows.h>

#include <cstdint>

#include "IDebugLog.h"
#include "ITypes.h"

#define __HOOK __declspec(naked) void

// From Better Transitions.  These functions are the common low-level backend
// for process code, function-pointer and window-procedure hook publication.
// Hook identity, ABI, predecessor chaining and rollback policy stay with the
// typed hook that owns each site.

DECLSPEC_NOINLINE bool __fastcall SafeWrite8(SIZE_T addr, SIZE_T data);
DECLSPEC_NOINLINE bool __fastcall SafeWrite16(SIZE_T addr, SIZE_T data);
DECLSPEC_NOINLINE bool __fastcall SafeWrite32(SIZE_T addr, SIZE_T data);
DECLSPEC_NOINLINE bool __fastcall SafeWriteBuf(
	SIZE_T addr, const void* data, SIZE_T len);

// Atomically publishes a 32-bit hook target only while the slot still contains
// the expected predecessor. observed receives the value present at the CAS.
DECLSPEC_NOINLINE bool __fastcall SafeWrite32IfEqual(
	SIZE_T addr, SIZE_T data, SIZE_T expected, SIZE_T* observed = nullptr);

// SetWindowLongPtrA has special zero-return/error semantics. Keep them in the
// same backend so WndProc hook installation and restoration have one checked
// publication primitive without disguising the operation as a code patch.
DECLSPEC_NOINLINE bool __fastcall SafeSetWindowLongPtrA(
	HWND window, int index, LONG_PTR data, LONG_PTR* previous);

// 5 bytes
DECLSPEC_NOINLINE bool __fastcall WriteRelJump(
	SIZE_T jumpSrc, SIZE_T jumpTgt);
DECLSPEC_NOINLINE bool __fastcall WriteRelCall(
	SIZE_T jumpSrc, SIZE_T jumpTgt);


// 6 bytes
DECLSPEC_NOINLINE bool __fastcall WriteRelJnz(
	SIZE_T jumpSrc, SIZE_T jumpTgt);
DECLSPEC_NOINLINE bool __fastcall WriteRelJle(
	SIZE_T jumpSrc, SIZE_T jumpTgt);

DECLSPEC_NOINLINE bool __fastcall PatchMemoryNop(
	ULONG_PTR address, SIZE_T size);
bool __fastcall PatchMemoryNopRange(
	ULONG_PTR startAddress, ULONG_PTR endAddress);

template <typename T>
bool __fastcall WriteRelCall(SIZE_T jumpSrc, T jumpTgt) {
	return WriteRelCall(jumpSrc, (SIZE_T)jumpTgt);
}

template <typename T>
bool __fastcall WriteRelJump(SIZE_T jumpSrc, T jumpTgt) {
	return WriteRelJump(jumpSrc, (SIZE_T)jumpTgt);
}

DECLSPEC_NOINLINE bool __fastcall ReplaceCall(
	SIZE_T jumpSrc, SIZE_T jumpTgt);

template <typename T>
bool __fastcall ReplaceCall(SIZE_T jumpSrc, T jumpTgt) {
	return ReplaceCall(jumpSrc, (SIZE_T)jumpTgt);
}

bool __fastcall ReplaceVirtualFunc(SIZE_T jumpSrc, void* jumpTgt);

// Stores the function-to-call before overwriting it, to allow calling the overwritten function after our hook is over.
// Thanks Demorome and lStewieAl

// Taken from lStewieAl.
// Returns the address of the jump/called function, assuming there is one.
static inline SIZE_T GetRelJumpAddr(SIZE_T jumpSrc) {
	return *(SIZE_T*)(jumpSrc + 1) + jumpSrc + 5;
}

static inline SIZE_T GetWriteAddr(SIZE_T writeAddr) {
	return *(SIZE_T*)(writeAddr);
}

// Specialization for member function pointers
template <typename C, typename Ret, typename... Args>
bool __fastcall WriteRelJumpEx(SIZE_T source, Ret(C::* const target)(Args...) const) {
	union
	{
		Ret(C::* tgt)(Args...) const;
		SIZE_T funcPtr;
	} conversion;
	conversion.tgt = target;

	return WriteRelJump(source, conversion.funcPtr);
}

template <typename C, typename Ret, typename... Args>
bool __fastcall WriteRelJumpEx(SIZE_T source, Ret(C::* const target)(Args...)) {
	union
	{
		Ret(C::* tgt)(Args...);
		SIZE_T funcPtr;
	} conversion;
	conversion.tgt = target;

	return WriteRelJump(source, conversion.funcPtr);
}

template <typename C, typename Ret, typename... Args>
bool __fastcall WriteRelCallEx(SIZE_T source, Ret(C::* const target)(Args...) const) {
	union
	{
		Ret(C::* tgt)(Args...) const;
		SIZE_T funcPtr;
	} conversion;
	conversion.tgt = target;

	return WriteRelCall(source, conversion.funcPtr);
}

template <typename C, typename Ret, typename... Args>
bool __fastcall WriteRelCallEx(SIZE_T source, Ret(C::* const target)(Args...)) {
	union
	{
		Ret(C::* tgt)(Args...);
		SIZE_T funcPtr;
	} conversion;
	conversion.tgt = target;

	return WriteRelCall(source, conversion.funcPtr);
}

template <typename C, typename Ret, typename... Args>
bool __fastcall ReplaceCallEx(SIZE_T source, Ret(C::* const target)(Args...) const) {
	union
	{
		Ret(C::* tgt)(Args...) const;
		SIZE_T funcPtr;
	} conversion;
	conversion.tgt = target;

	return ReplaceCall(source, conversion.funcPtr);
}

template <typename C, typename Ret, typename... Args>
bool __fastcall ReplaceCallEx(SIZE_T source, Ret(C::* const target)(Args...)) {
	union
	{
		Ret(C::* tgt)(Args...);
		SIZE_T funcPtr;
	} conversion;
	conversion.tgt = target;

	return ReplaceCall(source, conversion.funcPtr);
}

template <typename C, typename Ret, typename... Args>
bool __fastcall ReplaceVirtualFuncEx(SIZE_T source, Ret(C::* const target)(Args...) const) {
	union
	{
		Ret(C::* tgt)(Args...) const;
		SIZE_T funcPtr;
	} conversion;
	conversion.tgt = target;

	return SafeWrite32(source, conversion.funcPtr);
}

template <typename C, typename Ret, typename... Args>
bool __fastcall ReplaceVirtualFuncEx(SIZE_T source, Ret(C::* const target)(Args...)) {
	union
	{
		Ret(C::* tgt)(Args...);
		SIZE_T funcPtr;
	} conversion;
	conversion.tgt = target;

	return SafeWrite32(source, conversion.funcPtr);
}

template <typename C, typename Ret, typename... Args>
bool __fastcall ReplaceVTableEntry(void** apVTable, uint32_t auiPosition, Ret(C::* const target)(Args...) const) {
	union {
		Ret(C::* tgt)(Args...) const;
		SIZE_T funcPtr;
	} conversion;
	conversion.tgt = target;

	return SafeWrite32(
		reinterpret_cast<SIZE_T>(&apVTable[auiPosition]), conversion.funcPtr);
}

template <typename C, typename Ret, typename... Args>
bool __fastcall ReplaceVTableEntry(void** apVTable, uint32_t auiPosition, Ret(C::* const target)(Args...)) {
	union {
		Ret(C::* tgt)(Args...);
		SIZE_T funcPtr;
	} conversion;
	conversion.tgt = target;

	return SafeWrite32(
		reinterpret_cast<SIZE_T>(&apVTable[auiPosition]), conversion.funcPtr);
}

class CallDetour {
	SIZE_T overwritten_addr = 0;
public:
	DECLSPEC_NOINLINE bool __fastcall WriteRelCall(SIZE_T jumpSrc, void* jumpTgt)
	{
		__assume(jumpSrc != 0);
		__assume(jumpTgt != nullptr);
		if (*reinterpret_cast<uint8_t*>(jumpSrc) != 0xE8) {
			char cTextBuffer[72];
			sprintf_s(cTextBuffer, "Cannot write detour; jumpSrc is not a function call. (0x%08X)", jumpSrc);
			MessageBoxA(nullptr, cTextBuffer, "WriteRelCall", MB_OK | MB_ICONERROR);
			return false;
		}
		const SIZE_T predecessor = GetRelJumpAddr(jumpSrc);
		if (!::WriteRelCall(jumpSrc, jumpTgt))
			return false;
		overwritten_addr = predecessor;
		return true;
	}

	template <typename T>
	DECLSPEC_NOINLINE bool __fastcall ReplaceCall(SIZE_T jumpSrc, T jumpTgt) {
		__assume(jumpSrc != 0);
		if (*reinterpret_cast<uint8_t*>(jumpSrc) != 0xE8) {
			char cTextBuffer[72];
			sprintf_s(cTextBuffer, "Cannot write detour; jumpSrc is not a function call. (0x%08X)", jumpSrc);
			MessageBoxA(nullptr, cTextBuffer, "WriteRelCall", MB_OK | MB_ICONERROR);
			return false;
		}
		const SIZE_T predecessor = GetRelJumpAddr(jumpSrc);
		if (!::ReplaceCall(jumpSrc, (SIZE_T)jumpTgt))
			return false;
		overwritten_addr = predecessor;
		return true;
	}

	template <typename C, typename Ret, typename... Args>
	bool __fastcall ReplaceCallEx(SIZE_T source, Ret(C::* const target)(Args...) const) {
		union
		{
			Ret(C::* tgt)(Args...) const;
			SIZE_T funcPtr;
		} conversion;
		conversion.tgt = target;

		return ReplaceCall(source, conversion.funcPtr);
	}

	template <typename C, typename Ret, typename... Args>
	bool __fastcall ReplaceCallEx(SIZE_T source, Ret(C::* const target)(Args...)) {
		union
		{
			Ret(C::* tgt)(Args...);
			SIZE_T funcPtr;
		} conversion;
		conversion.tgt = target;

		return ReplaceCall(source, conversion.funcPtr);
	}

	template <typename T>
	bool SafeWrite32(SIZE_T jumpSrc, T jumpTgt) {
		__assume(jumpSrc != 0);
		const SIZE_T predecessor = GetWriteAddr(jumpSrc);
		if (!::SafeWrite32(jumpSrc, (SIZE_T)jumpTgt))
			return false;
		overwritten_addr = predecessor;
		return true;
	}

	[[nodiscard]] SIZE_T GetOverwrittenAddr() const { return overwritten_addr; }
};

class VirtFuncDetour {
protected:
	SIZE_T overwritten_addr = 0;

public:
	template <typename C, typename Ret, typename... Args>
	bool __fastcall ReplaceVirtualFuncEx(SIZE_T source, Ret(C::* const target)(Args...)) {
		union
		{
			Ret(C::* tgt)(Args...);
			SIZE_T funcPtr;
		} conversion;
		conversion.tgt = target;

		const SIZE_T predecessor = *(uint32_t*)source;
		if (!::SafeWrite32(source, conversion.funcPtr))
			return false;
		overwritten_addr = predecessor;
		return true;
	}

	[[nodiscard]] SIZE_T GetOverwrittenAddr() const { return overwritten_addr; }
};

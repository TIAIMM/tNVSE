#pragma once
#include <intrin.h>
#include <cstdint>
#include <utility>

template <typename T_Ret = uint32_t, typename... Args>
T_Ret ThisStdCall(uintptr_t _addr, const void* _this, Args... args) {
	return reinterpret_cast<T_Ret (__thiscall *)(const void*, Args...)>(_addr)(_this, std::forward<Args>(args)...);
}

template <typename T_Ret = void, typename... Args>
T_Ret StdCall(uintptr_t _addr, Args... args) {
	return reinterpret_cast<T_Ret (__stdcall *)(Args...)>(_addr)(std::forward<Args>(args)...);
}

template <typename T_Ret = void, typename... Args>
T_Ret CdeclCall(uintptr_t _addr, Args... args) {
	return reinterpret_cast<T_Ret (__cdecl *)(Args...)>(_addr)(std::forward<Args>(args)...);
}

inline UInt8* GetParentBasePtr(void* addressOfReturnAddress, const bool lambda = false) {
	auto* basePtr = static_cast<UInt8*>(addressOfReturnAddress) - 4;
#if _DEBUG
	// in debug mode, lambdas are wrapped inside a closure wrapper function, so one more step needed
	if (lambda) {
		basePtr = *reinterpret_cast<UInt8**>(basePtr);
	}
#endif
	return *reinterpret_cast<UInt8**>(basePtr);
}

#define GetParentVar(T, offset)\
	*reinterpret_cast<T*>((GetParentBasePtr(_AddressOfReturnAddress())) + (offset))  // NOLINT(bugprone-macro-parentheses)

#define GetParentVarPtr(T, offset)\
	reinterpret_cast<T*>((GetParentBasePtr(_AddressOfReturnAddress())) + (offset))  // NOLINT(bugprone-macro-parentheses)
#pragma once

template <class T>
class StackObject {
private:
	UInt8 data[sizeof(T)];

public:
	StackObject() {
		memset(data, 0, sizeof(T));
	}

	~StackObject() {
		reinterpret_cast<T*>(data)->~T();
	}

	T* operator->() {
		return reinterpret_cast<T*>(data);
	}
	T& operator*() {
		return *reinterpret_cast<T*>(data);
	}

	T& Get() {
		return *reinterpret_cast<T*>(data);
	}

	T* GetPtr() {
		return reinterpret_cast<T*>(data);
	}
};
// Align D3DXMATRIX
typedef __declspec(align(16)) D3DXMATRIX    D3DXALIGNEDMATRIX;
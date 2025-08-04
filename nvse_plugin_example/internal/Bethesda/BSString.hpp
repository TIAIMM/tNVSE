#pragma once

#ifndef BSSTRINGT_H
#define BSSTRINGT_H

#include "BSMemObject.hpp"

template <typename T>
class BSStringT : BSMemObject {
public:
	BSStringT() {};
	BSStringT(const T* apText) { Set(apText); }
	BSStringT(const BSStringT& aSrc) { Set(aSrc.c_str()); }
	~BSStringT();

	T*		pString = 0;
	UInt16	sLen	= 0;
	UInt16	sMaxLen = 0;

	inline UInt32			GetLength()		const;
	inline void				SetLength(UInt32 auiLen);
	inline UInt16			GetMaxLength()	const { return sMaxLen; }
	inline void				SetMaxLength(UInt16 auiLen) { sMaxLen = auiLen; }

	inline bool				Set(const T* apText, UInt32 auiLength = 0);
	inline BSStringT<T>*	operator+=(const T* apText);

	inline void				Format(const T* fmt, ...);
	inline void				ApplyFormat(const T* fmt, va_list args);

	inline bool				Includes(const char* toFind) const;
	inline bool				Replace(const char* toReplace, const char* replaceWith); // replaces instance of toReplace with replaceWith
	inline double			Compare(const BSStringT& compareTo, bool caseSensitive = false) const;

	inline const T*			c_str() const;
};

#include "BSString.inl"

#endif

typedef BSStringT<char>		BSString;
typedef BSStringT<wchar_t>	BSWideString;
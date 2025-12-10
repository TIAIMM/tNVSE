#pragma once

#include "NiBinaryStream.hpp"

class NiFile : public NiBinaryStream {
public:
	enum OpenMode {
		READ_ONLY = 0x0,
		WRITE_ONLY = 0x1,
		APPEND_ONLY = 0x2,
	};

	enum FileMethod : __int32
	{
		FILE_USE_POINTER = 0x0,
		FILE_USE_HANDLE = 0x1,
		FILE_USE_MASK = 0xF,
		FILE_USE_DOUBLEBUFFER = 0x80,
	};

	struct OverlappedContext
	{
		unsigned int m_uiFilePointer;
		unsigned int m_uiBytesExpected;
		bool m_bIOPending;
		_OVERLAPPED m_kOverlapped;
	};


	NiFile();
	NiFile(const char* apFileName, OpenMode aeMode, UInt32 auiBufferAllocSize);
	~NiFile() override;

	virtual void		Seek(SInt32 aiOffset, SInt32 aiWhence);
	virtual const char*	GetFilename() const;
	virtual UInt32		GetFileSize();

	unsigned int m_uiBufferAllocSize;
	unsigned int m_uiBufferReadSize;
	unsigned int m_uiPos;
	unsigned int m_uiCurrentFilePos;
	char* m_pBuffer;
	NiFile::FileMethod m_eFileMethod;
	bool m_bUseDoubleBuffer;
	_iobuf* m_pFile;
	void* m_hFile;
	unsigned int m_uiStreamBuffer;
	unsigned int m_uiReadBuffer;
	unsigned __int8* m_apBuffers[2];
	unsigned int m_uiDiskReads;
	NiFile::OverlappedContext m_kOLContext;
	bool bIsDDX;
	NiFile::OpenMode m_eMode;
	bool m_bGood;

	operator bool() override;
	void	Seek(SInt32 aiNumBytes) override;
	void	SetEndianSwap(bool abDoSwap) override;

	bool Flush();

	char* GetBuffer() const;

	static UInt32 __cdecl ReadAndSwap(NiBinaryStream* apThis, void* apvBuffer, UInt32 auiBytes, UInt32* apuiComponentSizes, UInt32 auiNumComponents);
	static UInt32 __cdecl WriteAndSwap(NiBinaryStream* apThis, const void* apvBuffer, UInt32 auiBytes, UInt32* apuiComponentSizes, UInt32 auiNumComponents);
	
	static UInt32 __cdecl ReadNoSwap(NiBinaryStream* apThis, void* apvBuffer, UInt32 auiBytes, UInt32* apuiComponentSizes, UInt32 auiNumComponents);
	static UInt32 __cdecl WriteNoSwap(NiBinaryStream* apThis, const void* apvBuffer, UInt32 auiBytes, UInt32* apuiComponentSizes, UInt32 auiNumComponents);


	UInt32 FileRead(void* apBuffer, UInt32 auiBytes);
	UInt32 FileWrite(const void* apBuffer, UInt32 auiBytes);

	UInt32 DiskRead(void* apBuffer, UInt32 auiBytes);
	UInt32 DiskWrite(const void* apBuffer, UInt32 auiBytes);

	UInt32 ReadBuffer(void* pvBuffer, UInt32 uiBytes, UInt32* puiComponentSizes, UInt32 uiNumComponents);
};

static_assert(sizeof(NiFile) == 0x74);
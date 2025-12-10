#pragma once

#include "QueuedFile.hpp"
#include "Archive.hpp"

class QueuedFileEntry : public QueuedFile {
public:
	QueuedFileEntry();
	~QueuedFileEntry();

	virtual bool Unk_0B();

	char*			pFileName;
	BSFileEntry*	pData;

	void LookupFileInBSA(ARCHIVE_TYPE_INDEX aeArchiveTypeIndex);
	bool CreateDescription(char* apDest, size_t auiSize, const char* apTaskName);

	ArchiveFile* OpenBSFile(ARCHIVE_TYPE_INDEX aeArchiveTypeIndex, SInt32 aiArchiveType);
};
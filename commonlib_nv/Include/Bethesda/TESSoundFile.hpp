#pragma once

#include "BaseFormComponent.hpp"
#include <BSString.hpp>

class TESSoundFile : public BaseFormComponent {
public:
	TESSoundFile();
	~TESSoundFile();

	virtual void	Set(const char* str);

	BSString	strPath;

	const char* GetPath() const;
};
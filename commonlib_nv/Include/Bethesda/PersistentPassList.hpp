#pragma once

#include "BSShaderProperty.hpp"
#include "NiTPointerAllocator.hpp"
#include "NiTPointerListBase.hpp"

// BSTPersistentList<NiTPointerAllocator, BSShaderProperty::RenderPass*> - PS3 - custom class?

class PersistentPassListAllocator : public NiTPointerAllocator<size_t> {
public:
	NiTListItem<BSShaderProperty::RenderPass*>* pPassGroupNext = nullptr;
};

class PersistentPassList : public NiTPointerListBase<PersistentPassListAllocator, BSShaderProperty::RenderPass*> {
public:
	PersistentPassList();
	virtual ~PersistentPassList();

	void RemoveAll();

	void Render(bool bBlendAlpha);
	void Sort(SInt32(__cdecl* Sort)(BSShaderProperty::RenderPass** apPassPtrOne, BSShaderProperty::RenderPass** apPassPtrTwo));
	void AddHead(BSShaderProperty::RenderPass*& appRenderPass);
	void AddTail(BSShaderProperty::RenderPass*& appRenderPass);

	void RemoveHead();

	void Reset();
};

ASSERT_SIZE(PersistentPassList, 0x14);

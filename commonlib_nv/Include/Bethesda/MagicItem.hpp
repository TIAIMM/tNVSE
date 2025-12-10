#pragma once

#include "TESFullName.hpp"
#include "EffectItemList.hpp"

class TESFile;

class MagicItem : public TESFullName, public EffectItemList {
public:
	MagicItem();
	~MagicItem();

	class Data {
	public:
		SInt32 iCostOverride;
		UInt32 uiFlags;
	};

	virtual bool					IsAutoCalc() const;
	virtual void					SetAutoCalc(bool abVal);
	virtual MagicSystem::SpellType	GetSpellType() const;
	virtual bool					Unk_07();
	virtual bool					Unk_08();
	virtual UInt32					GetChunkID() const;
	virtual Data*					GetData() const;
	virtual UInt32					GetDataSize();
	virtual bool					CompareMagicItemData(MagicItem* apItem);
	virtual void					CopyMagicItemData(MagicItem* apItem);
	virtual void					SaveMagicItemComponents();
	virtual void					Endian();
	virtual void					LoadMagicItemChunk(TESFile* apFile, UInt32 aeChunkID);

	UInt32 GetMagicItemFormID() const;
};

ASSERT_SIZE(MagicItem, 0x1C + 4);
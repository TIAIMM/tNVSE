#pragma once

class TESFile;

class TESPackageData {
public:
	TESPackageData();
	virtual			~TESPackageData();
	virtual void	CopyFrom(TESPackageData* apPackageData);
	virtual void	Unk_02(void);
	virtual void	Save(TESFile* apFile);
	virtual void	Unk_04(void);
	virtual void	Unk_05(void);
	virtual void	Unk_06(void);
	virtual void	Unk_07(void);
};
#pragma once

#include "TESForm.hpp"

#define SCRIPT_SIZE 0x54
static const UInt32 kScript_ExecuteFnAddr = 0x005AC1E0;

class ScriptLocals;
struct ScriptCompileData;
struct ScriptOperator;

class TESQuest;

class Script : public TESForm
{
public:
	Script();
	virtual ~Script() {}

	// members

	class RefVariable : public BSMemObject {
	public:
		BSString	strName;
		TESForm*	pForm;
		UInt32		uiVarID;

		//void Resolve(ScriptLocals* eventList);

		Script* GetReferencedScript() const;
	};

	enum VariableType : UInt8
	{
		eVarType_Float = 0, // ref is also zero
		eVarType_Integer,

		// NVSE, return values only
		eVarType_String,
		eVarType_Array,
		eVarType_Ref,

		eVarType_Invalid
	};

	// 14
	struct ScriptInfo
	{
		UInt32	uiUnusedVariableCount;
		UInt32	uiNumRefs;
		UInt32	uiDataLength;
		UInt32	uiVarCount;
		UInt16	usType;
		bool	bCompiled;
		UInt8	unk13;
	};

	enum
	{
		eType_Object = 0,
		eType_Quest = 1,
		eType_Magic = 0x100,
		eType_Unk = 0x10000,
	};

	ScriptInfo						kInfo;
	char*							pText;
	UInt8*							pData;
	float							fExecutionTime;
	float							fQuestDelayTimeCounter;
	float							fSecondsPassed;
	TESQuest*						pQuest;
	BSSimpleList<RefVariable*>		kReferences;

	RefVariable* GetRefFromRefList(UInt32 refIdx);
	//VariableInfo* GetVariableInfo(UInt32 idx);

	UInt32 AddVariable(TESForm* form);
	void CleanupVariables(void);

	UInt32 Type() const { return kInfo.usType; }
	bool IsObjectScript() const { return kInfo.usType == eType_Object; }
	bool IsQuestScript() const { return kInfo.usType == eType_Quest; }
	bool IsMagicScript() const { return kInfo.usType == eType_Magic; }
	bool IsUnkScript() const { return kInfo.usType == eType_Unk; }

	//VariableInfo* GetVariableByName(const char* varName);

	bool IsUserDefinedFunction() const;

	static bool RunScriptLine(const char* text, TESObjectREFR* object = NULL);
	static bool RunScriptLine2(const char* text, TESObjectREFR* object = NULL, bool bSuppressOutput = true);

	ScriptLocals* CreateEventList();
	UInt32 GetVarCount() const;
	UInt32 GetRefCount() const;

	//BSSimpleList<VariableInfo*>* GetVars();
	BSSimpleList<RefVariable*>* GetRefList();

	bool Compile(ScriptCompileData* buffer);
};
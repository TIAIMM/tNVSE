#pragma once

class NiDX9RenderState;

class NiD3DTextureStageGroup {
public:
	bool	m_bRendererOwned;
	UInt32	m_uiNumSavedStages;
	UInt32	m_uiSavedStageArrayIter;
	UInt32	m_auiSavedStageArray[8];
	bool	m_abSavedStageValid[8];
	UInt32	m_uiNumStages;
	UInt32	m_uiStageArrayIter;
	UInt32	m_auiStageArray[8];
	bool	m_abStageValid[8];
	UInt32	m_uiNumSavedSamplers;
	UInt32	m_uiSavedSamplerArrayIter;
	UInt32	m_auiSavedSamplerArray[5];
	bool	m_abSavedSamplerValid[5];
	UInt32	m_uiNumSamplers;
	UInt32	m_uiSamplerArrayIter;
	UInt32	m_auiSamplerArray[5];
	bool	m_abSamplerValid[5];
	UInt32	m_uiNumUseMapValue;
	bool	m_abSamplerUseMapValue[5];

	static NiDX9RenderState* GetRenderState();

	static D3DTEXTURESTAGESTATETYPE GetTextureStageState(UInt32 auiState);
	static D3DSAMPLERSTATETYPE GetSamplerState(UInt32 auiState);

	UInt32 SetTextureStage(UInt32 auiStage) const;
	UInt32 SetAllStageStates(UInt32 auiStage) const;
	UInt32 SetAllStageSamplers(UInt32 auiStage) const;
	bool GetStageState(UInt32 auiState, UInt32& auiValue, bool& abSave);

	static NiD3DTextureStageGroup* GetFreeTextureStageGroup();
};

ASSERT_SIZE(NiD3DTextureStageGroup, 0xB8);
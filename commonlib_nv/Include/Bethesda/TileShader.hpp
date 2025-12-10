#pragma once

#include "BSShader.hpp"

class TileShader : public BSShader {
public:
	virtual void Func_79();
	virtual void LoadVertexShaders();
	virtual void LoadPixelShaders();
	virtual bool LoadStagesAndPasses();
	virtual bool Func_83(); // Ret1

	NiD3DPassPtr				spPasses[1];
	NiD3DVertexShaderPtr		spVertexShaders[3]; // 1 = rotation, 2 = alpha
	NiD3DPixelShaderPtr			spPixelShaders[3];
	NiD3DShaderDeclarationPtr	spShaderDeclarations[2]; // 1 for vertex colors

	NIRTTI_ADDRESS(0x12021B8);

	static TileShader* __stdcall CreateShader();
	
	bool InitializeEx();
	void SetupTexturesEx(const NiPropertyState* apProperties);
	void UpdateConstantsEx(const NiPropertyState* apProperties);
	void PostRenderSetupEx(const NiPropertyState* apProperties);
	void RecreateRendererDataEx();
	void SetRenderPassEx(BSShaderManager::RenderPassType aeRenderPass);
	void Func_63Ex();
	void InitShaderConstantsEx();
	void LoadShadersEx();
	void LoadVertexShadersEx();
	void LoadPixelShadersEx();
	void LoadStagesAndPassesEx();

};

ASSERT_SIZE(TileShader, 0x90);
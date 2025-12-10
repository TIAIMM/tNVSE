#pragma once

#include <d3dx9math.h>

#include "NiD3DDefaultShader.hpp"
#include "NiDX9ShaderDeclaration.hpp"
#include "BSShaderManager.hpp"
#include "BSShaderProperty.hpp"

class NiD3DPass;

class BSShader : public NiD3DDefaultShader {
public:
	enum ShaderType {
		TYPE_ShadowLightShader		= 1,
		TYPE_HairShader				= 2,
		TYPE_ParallaxShader			= 3,
		TYPE_SkinShader				= 4,
		TYPE_SpeedTreeBranchShader	= 5,
		TYPE_TallGrassShader		= 6,
		TYPE_DistantLODShader		= 7,
		TYPE_SpeedTreeFrondShader	= 8,
		TYPE_SpeedTreeLeafShader	= 9,
		TYPE_DebugShader			= 10,
		TYPE_SkyShader				= 11,
		TYPE_GeometryDecalShader	= 12,
		TYPE_WaterShader			= 13,
		TYPE_ParticleShader			= 14,
		TYPE_BoltShader				= 15,
		TYPE_BeamShader				= 16,
		TYPE_Lighting30Shader		= 17,
		TYPE_PrecipitationShader	= 18,
		TYPE_VolumetricFogShader	= 19,
		TYPE_TileShader				= 20,
		//							  21
		TYPE_BSShaderNoLighting		= 22,
		TYPE_BSShaderBloodSplatter	= 23,
		TYPE_ImagespaceShader		= 24,
		TYPE_BSDistantTreeShader	= 25,
		
		TYPE_PBRShader				= 26,
	};

	virtual void SetRenderPass(UInt32 aeType);
	virtual void SetShaders(BSShaderManager::RenderPassType aeType = BSShaderManager::BSSM_EMPTY);
	virtual void SetupStaticTextures(BSShaderManager::RenderPassType aeType);
	// Seems to focus on the fog in most shaders
	virtual void Func_63(BSShaderManager::RenderPassType aeType);
	virtual void RestoreSavedStates(UInt32 aeType);
	virtual void RestoreTechnique(UInt32 uiLastPass);
	// Checks for bUnk64
	virtual void ConfigureTextureStages();
	virtual void ConfigureAllTextureStages();
	virtual void StartMultiPass(const NiPropertyState* apProperties);
	virtual void StopMultiPass();
	virtual NiDX9ShaderDeclaration* GetShaderDeclaration(NiGeometry*, BSShaderProperty* property);
	virtual void InitShaderConstants();
	// Reloads/ loads shader files and setups passes
	virtual void Reinitialize();
	virtual void ClearPassStages();
	virtual void ClearPass(NiD3DPass* apPass);
	virtual void Func_75();
	virtual void CreateNewPass();
	virtual void Func_77();
	virtual void Func_78(UInt32* apeType, int a2);

	NiD3DPassPtr	spPass;
	void*			pUnk60;
	bool			bUnk64;
	UInt32			iShaderType;

	bool IsShadowLightShader() const { return iShaderType == TYPE_ShadowLightShader; }
	bool IsHairShader() const { return iShaderType == TYPE_HairShader; }
	bool IsParallaxShader() const { return iShaderType == TYPE_ParallaxShader; }
	bool IsSkinShader() const { return iShaderType == TYPE_SkinShader; }
	bool IsSpeedTreeBranchShader() const { return iShaderType == TYPE_SpeedTreeBranchShader; }
	bool IsTallGrassShader() const { return iShaderType == TYPE_TallGrassShader; }
	bool IsDistantLODShader() const { return iShaderType == TYPE_DistantLODShader; }
	bool IsSpeedTreeFrondShader() const { return iShaderType == TYPE_SpeedTreeFrondShader; }
	bool IsSpeedTreeLeafShader() const { return iShaderType == TYPE_SpeedTreeLeafShader; }
	bool IsDebugShader() const { return iShaderType == TYPE_DebugShader; }
	bool IsSkyShader() const { return iShaderType == TYPE_SkyShader; }
	bool IsGeometryDecalShader() const { return iShaderType == TYPE_GeometryDecalShader; }
	bool IsWaterShader() const { return iShaderType == TYPE_WaterShader; }
	bool IsParticleShader() const { return iShaderType == TYPE_ParticleShader; }
	bool IsBoltShader() const { return iShaderType == TYPE_BoltShader; }
	bool IsBeamShader() const { return iShaderType == TYPE_BeamShader; }
	bool IsLighting30Shader() const { return iShaderType == TYPE_Lighting30Shader; }
	bool IsPrecipitationShader() const { return iShaderType == TYPE_PrecipitationShader; }
	bool IsVolumetricFogShader() const { return iShaderType == TYPE_VolumetricFogShader; }
	bool IsTileShader() const { return iShaderType == TYPE_TileShader; }
	bool IsBSShaderNoLighting() const { return iShaderType == TYPE_BSShaderNoLighting; }
	bool IsBSShaderBloodSplatter() const { return iShaderType == TYPE_BSShaderBloodSplatter; }
	bool IsImagespaceShader() const { return iShaderType == TYPE_ImagespaceShader; }
	bool IsBSDistantTreeShader() const { return iShaderType == TYPE_BSDistantTreeShader; }

	bool InitializeEx();
	void SetShadersEx(BSShaderManager::RenderPassType aeType);
	void ClearPassEx(NiD3DPass* apPass);

	static void CalculateTransformMatrix(NiTransform& arTransform, NiSkinInstance* apSkinInstance, D3DXMATRIX& pOut);
	void SetWorldViewProjTranspose(NiGeometry* apGeometry, const NiTransform& arTransform);
	void SetLightDirection(D3DXMATRIX& arMatrix);

	static NiD3DVertexShader* CreateVertexShader(const char* apFilename);
	static NiD3DPixelShader* CreatePixelShader(const char* apFilename);

	NiD3DVertexShader* CreateVertexShaderEx(const char* apPath, D3DXMACRO* apMacro, const char* apShaderVersion, const char* apFilename);
	NiD3DPixelShader* CreatePixelShaderEx(const char* apPath, D3DXMACRO* apMacro, const char* apShaderVersion, const char* apFilename);

	static void SetLightData(UInt32 auiNumLights, ShadowSceneLight* apLight, float afDimmer);

	void Setup_ADT_Opt(const NiPropertyState* apProperties);
	void Setup_ADT_PxOpt(const NiPropertyState* apProperties);
	void Setup_ADT4_Opt(const NiPropertyState* apProperties);
	void Setup_ADTS10_Opt(const NiPropertyState* apProperties);

	struct FogProperties {
		struct FogParameters {
			float fDistFar;
			float fDistNear;
			float fPower;
			float fUnknown;
		};

		FogParameters Parameters;
		NiColorA Color;
	};

	static float fLightMult;
	static float fEmissiveMult;
	static float fNoLightingMult;
	static float fSunMult;
	static float fAmbientMult;
};

ASSERT_SIZE(BSShader, 0x6C);
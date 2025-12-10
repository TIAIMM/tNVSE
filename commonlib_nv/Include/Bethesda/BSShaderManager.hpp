#pragma once

#include <d3dx9shader.h>

#include "NiColor.hpp"
#include "NiPoint2.hpp"
#include "NiPoint3.hpp"
#include "NiPoint4.hpp"
#include "NiTexture.hpp"
#include "NiSourceTexture.hpp"
#include "NiRenderedTexture.hpp"
#include "NiRenderedCubeMap.hpp"
#include "NiTStringMap.hpp"
#include "NiRect.hpp"
#include "NiLight.hpp"
#include "NiTexturingProperty.hpp"
#include "NiGeometry.hpp"

#define USE_PBR 1

class BSShader;
class ShadowSceneNode;
class BSRenderedTexture;
class BSShaderAccumulator;
class NiDX9Renderer;

class BSShaderManager {
public:
	enum SceneGraphType : UInt32 {
		BSSM_SSN_WORLD			= 0,
		BSSM_SSN_MENU_OBJECT_3D = 1,
		BSSM_SSN_UNK_2			= 2,
		BSSM_SSN_MENU_PLAYER_3D = 3,
		BSSM_SSN_COUNT			= 4,
	};

	enum ShaderType : UInt32 {
		BSSM_SHADER_DEFAULT			= 0,
		BSSM_SHADER_SHADOWLIGHT		= 1,
		BSSM_SHADER_TALL_GRASS		= 2,
		BSSM_SHADER_DISTANT_LOD		= 3,
		BSSM_SHADER_ST_BRANCH		= 4,
		BSSM_SHADER_ST_FROND		= 5,
		BSSM_SHADER_ST_LEAF			= 6,
		BSSM_SHADER_BLOOD_SPLATTER	= 7,
		BSSM_SHADER_DISTANT_TREE	= 8,
		BSSM_SHADER_DEBUG			= 9,
		BSSM_SHADER_SKY				= 10,
		//							  11
		//							  12
		BSSM_SHADER_HAIR			= 13,
		BSSM_SHADER_SKIN			= 14,
		BSSM_SHADER_PARALLAX		= 15,
		BSSM_SHADER_GEOM_DECAL		= 16,
		BSSM_SHADER_WATER			= 17,
		// 						      18
		// 						      19
		// 						      20
		// 						      21
		// 						      22
		// 						      23
		BSSM_SHADER_PARTICLE		= 24,
		BSSM_SHADER_BOLT			= 25,
		BSSM_SHADER_BEAM			= 26,
		//							  27,
		//							  28,
		BSSM_SHADER_LIGHTING30		= 29,
		BSSM_SHADER_PRECIPITATION	= 30,
		// 						      31
		BSSM_SHADER_TILE			= 32,
		BSSM_SHADER_NOLIGHTING		= 33,
		BSSM_SHADER_VOLUMETRIC_FOG	= 34,

#if USE_PBR
		BSSM_SHADER_PBR				= 27, // CUSTOM
#endif
	};

	enum RenderPassType : UInt32 {
		BSSM_EMPTY																= 0,
		BSSM_ZONLY																= 1,   // Z-pass
		BSSM_ZONLY_At															= 2,   // Z-pass, alpha test
		BSSM_ZONLY_S															= 3,   // Z-pass, skinned
		BSSM_ZONLY_TEXEFFECT													= 4,   // Z-pass, texture effect
		BSSM_ZONLY_TEXEFFECT_S													= 5,   // Z-pass, texture effect, skinned
		BSSM_CONSTALPHA															= 6,   // Constant alpha
		BSSM_CONSTALPHA_At														= 7,   // Constant alpha, alpha test
		BSSM_CONSTALPHA_S														= 8,   // Constant alpha, skinned
		BSSM_AMBIENT_OCCLUSION													= 9,   // Occlusion query
		BSSM_3XZONLY															= 10,  // 3.0 Z-pass
		BSSM_3XZONLY_S															= 11,  // 3.0 Z-pass, skinned
		BSSM_3XZONLY_Vc															= 12,  // 3.0 Z-pass, vertex colors
		BSSM_3XZONLY_SVc														= 13,  // 3.0 Z-pass, skinned, vertex colors
		BSSM_3XZONLY_TEXEFFECT													= 14,  // 3.0 Z-pass, texture effect
		BSSM_3XZONLY_TEXEFFECT_S												= 15,  // 3.0 Z-pass, texture effect, skinned
		BSSM_DEPTHMAP															= 16,  // Depth map
		BSSM_DEPTHMAP_At														= 17,  // Depth map, Alpha test
		BSSM_DEPTHMAP_S															= 18,  // Depth map, Skinned
		BSSM_DEPTHMAP_SAt														= 19,  // Depth map, Skinned, Alpha test
		BSSM_SELFILLUMALPHABLOCK												= 20,  // Self illumination, Alpha block
		BSSM_SELFILLUMALPHABLOCK_S												= 21,  // Self illumination, Alpha block, Skinned
		BSSM_GRASS_NOALPHA_DIRONLY_LF											= 22,  // Grass, No alpha test, Directional light,
		BSSM_GRASS_NOALPHA_DIRONLY_LV											= 23,  // Grass, No alpha test, Directional light,
		BSSM_GRASS_NOALPHA_DIRONLY_LFS											= 24,  // Grass, No alpha test, Directional light,
		BSSM_GRASS_NOALPHA_DIRONLY_LVS											= 25,  // Grass, No alpha test, Directional light,
		BSSM_GRASS_NOALPHA_DIRONLY_LFP											= 26,  // Grass, No alpha test, Directional light,
		BSSM_GRASS_NOALPHA_DIRONLY_LVP											= 27,  // Grass, No alpha test, Directional light,
		BSSM_GRASS_NOALPHA_DIRONLY_LFSP											= 28,  // Grass, No alpha test, Directional light,
		BSSM_GRASS_NOALPHA_DIRONLY_LVSP											= 29,  // Grass, No alpha test, Directional light,
		BSSM_GRASS_NOALPHA_DIRONLY_LFShp										= 30,  // Grass, No alpha test, Directional light,
		BSSM_GRASS_NOALPHA_DIRONLY_LVShp										= 31,  // Grass, No alpha test, Directional light,
		BSSM_GRASS_NOALPHA_DIRONLY_LFSShp										= 32,  // Grass, No alpha test, Directional light,
		BSSM_GRASS_NOALPHA_DIRONLY_LVSShp										= 33,  // Grass, No alpha test, Directional light,
		BSSM_GRASS_NOALPHA_DIRONLY_LFPShp										= 34,  // Grass, No alpha test, Directional light,
		BSSM_GRASS_NOALPHA_DIRONLY_LVPShp										= 35,  // Grass, No alpha test, Directional light,
		BSSM_GRASS_NOALPHA_DIRONLY_LFSPShp										= 36,  // Grass, No alpha test, Directional light,
		BSSM_GRASS_NOALPHA_DIRONLY_LVSPShp										= 37,  // Grass, No alpha test, Directional light,
		BSSM_GRASS_NOALPHA_DIRONLY_LFB											= 38,  // Grass, No alpha test, Directional light,
		BSSM_GRASS_NOALPHA_DIRONLY_LFSB											= 39,  // Grass, No alpha test, Directional light,
		BSSM_GRASS_NOALPHA_DIRONLY_LFPB											= 40,  // Grass, No alpha test, Directional light,
		BSSM_GRASS_NOALPHA_DIRONLY_LFSPB										= 41,  // Grass, No alpha test, Directional light,
		BSSM_GRASS_NOALPHA_1POINT_LF											= 42,  // Grass, No alpha test, Point light,
		BSSM_GRASS_NOALPHA_1POINT_LV											= 43,  // Grass, No alpha test, Point light,
		BSSM_GRASS_NOALPHA_1POINT_LFS											= 44,  // Grass, No alpha test, Point light,
		BSSM_GRASS_NOALPHA_1POINT_LVS											= 45,  // Grass, No alpha test, Point light,
		BSSM_GRASS_NOALPHA_1POINT_LFP											= 46,  // Grass, No alpha test, Point light,
		BSSM_GRASS_NOALPHA_1POINT_LVP											= 47,  // Grass, No alpha test, Point light,
		BSSM_GRASS_NOALPHA_1POINT_LFSP											= 48,  // Grass, No alpha test, Point light,
		BSSM_GRASS_NOALPHA_1POINT_LVSP											= 49,  // Grass, No alpha test, Point light,
		BSSM_GRASS_NOALPHA_1POINT_LFShp											= 50,  // Grass, No alpha test, Point light,
		BSSM_GRASS_NOALPHA_1POINT_LVShp											= 51,  // Grass, No alpha test, Point light,
		BSSM_GRASS_NOALPHA_1POINT_LFSShp										= 52,  // Grass, No alpha test, Point light,
		BSSM_GRASS_NOALPHA_1POINT_LVSShp										= 53,  // Grass, No alpha test, Point light,
		BSSM_GRASS_NOALPHA_1POINT_LFPShp										= 54,  // Grass, No alpha test, Point light,
		BSSM_GRASS_NOALPHA_1POINT_LVPShp										= 55,  // Grass, No alpha test, Point light,
		BSSM_GRASS_NOALPHA_1POINT_LFSPShp										= 56,  // Grass, No alpha test, Point light,
		BSSM_GRASS_NOALPHA_1POINT_LVSPShp										= 57,  // Grass, No alpha test, Point light,
		BSSM_GRASS_NOALPHA_1POINT_LFB											= 58,  // Grass, No alpha test, Point light,
		BSSM_GRASS_NOALPHA_1POINT_LFSB											= 59,  // Grass, No alpha test, Point light,
		BSSM_GRASS_NOALPHA_1POINT_LFPB											= 60,  // Grass, No alpha test, Point light,
		BSSM_GRASS_NOALPHA_1POINT_LFSPB											= 61,  // Grass, No alpha test, Point light,
		BSSM_LEAVES_AD															= 62,  // Leaf, Ambient, Directional light
		BSSM_LEAVES_ADPt														= 63,  // Leaf, Ambient, Directional light, Point Light
		BSSM_FRONDS																= 64,  // Fronds
		BSSM_PARTICLE_PREPASS													= 65,  // Particle Pre-pass
		BSSM_NOLIGHTING															= 66,  // No lighting
		BSSM_NOLIGHTING_VC														= 67,  // No lighting, Vertex colors
		BSSM_NOLIGHTING_TexVC													= 68,  // No lighting, Texture, Vertex colors
		BSSM_NOLIGHTING_MAT														= 69,  // No lighting, Material
		BSSM_NOLIGHTING_S														= 70,  // No lighting, Skinned
		BSSM_NOLIGHTING_VC_S													= 71,  // No lighting, Vertex colors, Skinned
		BSSM_NOLIGHTING_TexVC_S													= 72,  // No lighting, Texture, Vertex colors, Skinned
		BSSM_NOLIGHTING_MAT_S													= 73,  // No lighting, Material, Skinned
		BSSM_NOLIGHTING_FALLOFF													= 74,  // No lighting, Falloff
		BSSM_NOLIGHTING_VC_FALLOFF												= 75,  // No lighting, Vertex colors, Falloff
		BSSM_NOLIGHTING_TexVC_FALLOFF											= 76,  // No lighting, Texture, Vertex colors, Falloff
		BSSM_NOLIGHTING_MAT_FALLOFF												= 77,  // No lighting, Material, Falloff
		BSSM_NOLIGHTING_FALLOFF_S												= 78,  // No lighting, Falloff, Skinned
		BSSM_NOLIGHTING_VC_FALLOFF_S											= 79,  // No lighting, Vertex colors, Falloff, Skinned
		BSSM_NOLIGHTING_TexVC_FALLOFF_S											= 80,  // No lighting, Texture, Vertex colors, Falloff Skinned
		BSSM_NOLIGHTING_MAT_FALLOFF_S											= 81,  // No lighting, Material, Falloff, Skinned
		BSSM_NOLIGHTING_PSYS													= 82,  // No lighting, Particle system,
		BSSM_NOLIGHTING_PSYS_SUBTEX_OFFSET										= 83,  // No lighting, Particle system, Subtexture offset
		BSSM_NOLIGHTING_PSYS_PREMULT_ALPHA										= 84,  // No lighting, Particle system, Premultiplied alpha
		BSSM_NOLIGHTING_PSYS_SUBTEX_OFFSET_PREMULT_ALPHA						= 85,  // No lighting, Particle system, Subtexture offset, Premultiplied alpha
		BSSM_NOLIGHTING_STRIP_PSYS												= 86,  // No lighting, Strip particle system,
		BSSM_NOLIGHTING_STRIP_PSYS_SUBTEX										= 87,  // No lighting, Strip particle system, Subtexture
		BSSM_AMBIENT															= 88,  // Ambient
		BSSM_AMBIENT_G															= 89,  // Ambient, Glow map
		BSSM_AMBIENT_At															= 90,  // Ambient, Alpha Test
		BSSM_AMBIENT_GAt														= 91,  // Ambient, Glow map, Alpha test
		BSSM_AMBIENT_S															= 92,  // Ambient, Skinned
		BSSM_AMBIENT_SG															= 93,  // Ambient, Skinned, Glow map
		BSSM_AMBIENT_SAt														= 94,  // Ambient, Skinned, Alpha test
		BSSM_AMBIENT_SGAt														= 95,  // Ambient, Skinned, Glow map, Alpha test
		BSSM_AMBIENT_Sb															= 96,  // Ambient, SpeedTree branch
		BSSM_AMBIENT_Mn															= 97,  // Ambient, Model space normals
		BSSM_AMBIENT_F															= 98,  // Ambient, Fog
		BSSM_AMBIENT_FAt														= 99,  // Ambient, Fog, Alpha test
		BSSM_AMBDIFFTEX															= 100, // Ambient, Diffuse texture
		BSSM_AMBDIFFTEX_Vc														= 101, // Ambient, Diffuse texture, Vertex colors
		BSSM_AMBDIFFTEX_G														= 102, // Ambient, Diffuse texture, Glow map
		BSSM_AMBDIFFTEX_GVc														= 103, // Ambient, Diffuse texture, Glow map, Vertex colors
		BSSM_AMBDIFFTEX_Fg														= 104, // Ambient, Diffuse texture, FaceGen
		BSSM_AMBDIFFTEX_A														= 105, // Ambient, Diffuse texture, Alpha
		BSSM_AMBDIFFTEX_AVc														= 106, // Ambient, Diffuse texture, Alpha, Vertex colors
		BSSM_AMBDIFFTEX_GA														= 107, // Ambient, Diffuse texture, Glow map, Alpha
		BSSM_AMBDIFFTEX_GAVc													= 108, // Ambient, Diffuse texture, Glow map, Alpha, Vertex colors
		BSSM_AMBDIFFTEX_FgA														= 109, // Ambient, Diffuse texture, FaceGen, Alpha
		BSSM_AMBDIFFTEX_GFgA													= 110, // Ambient, Diffuse texture, Glow map, FaceGen, Alpha
		BSSM_AMBDIFFTEX_S														= 111, // Ambient, Diffuse texture, Skinned
		BSSM_AMBDIFFTEX_SVc														= 112, // Ambient, Diffuse texture, Skinned, Vertex colors
		BSSM_AMBDIFFTEX_SG														= 113, // Ambient, Diffuse texture, Skinned, Glow map
		BSSM_AMBDIFFTEX_SGVc													= 114, // Ambient, Diffuse texture, Skinned, Glow map, Vertex colors
		BSSM_AMBDIFFTEX_SFg														= 115, // Ambient, Diffuse texture, Skinned, FaceGen
		BSSM_AMBDIFFTEX_SA														= 116, // Ambient, Diffuse texture, Skinned, Alpha
		BSSM_AMBDIFFTEX_SAVc													= 117, // Ambient, Diffuse texture, Skinned, Alpha, Vertex colors
		BSSM_AMBDIFFTEX_SGA														= 118, // Ambient, Diffuse texture, Skinned, Glow map, Alpha
		BSSM_AMBDIFFTEX_SGAVc													= 119, // Ambient, Diffuse texture, Skinned, Glow map, Alpha, Vertex colors
		BSSM_AMBDIFFTEX_SFgA													= 120, // Ambient, Diffuse texture, Skinned, FaceGen, Alpha
		BSSM_AMBDIFFTEX_SGFgA													= 121, // Ambient, Diffuse texture, Skinned, Glow map, FaceGen, Alpha
		BSSM_AMBDIFFTEX_Sb														= 122, // Ambient, Diffuse texture, SpeedTree branch
		BSSM_AMBDIFFTEX_SbF														= 123, // Ambient, Diffuse texture, SpeedTree branch, Frond
		BSSM_AMBDIFFDIRANDPT													= 124, // Ambient, Diffuse texture, Directional light, Point light
		BSSM_AMBDIFFDIRANDPT_S													= 125, // Ambient, Diffuse texture, Directional light, Point light, Skinned
		BSSM_AMBDIFFDIRANDPT_Sb													= 126, // Ambient, Diffuse texture, Directional light, Point light, SpeedTree branch
		BSSM_AMBDIFFTEX_F														= 127, // Ambient, Diffuse texture, Frond
		BSSM_AMBDIFFTEX_FVc														= 128, // Ambient, Diffuse texture,	Frond, Vertex colors
		BSSM_AMBDIFFTEX_FG														= 129, // Ambient, Diffuse texture,	Frond, Glow map
		BSSM_AMBDIFFTEX_FGVc													= 130, // Ambient, Diffuse texture,	Frond, Glow map, Vertex colors
		BSSM_AMBDIFFTEX_FA														= 131, // Ambient, Diffuse texture,	Frond, Alpha
		BSSM_AMBDIFFTEX_FAVc													= 132, // Ambient, Diffuse texture,	Frond, Alpha, Vertex colors
		BSSM_AMBDIFFTEX_FFg														= 133, // Ambient, Diffuse texture,	Frond, FaceGen
		BSSM_AMBDIFFTEX_FGA														= 134, // Ambient, Diffuse texture,	Frond, Glow map, Alpha
		BSSM_AMBDIFFTEX_FGAVc													= 135, // Ambient, Diffuse texture,	Frond, Glow map, Alpha, Vertex colors
		BSSM_AMBDIFFTEX_FGFgA													= 136, // Ambient, Diffuse texture,	Frond, Glow map, FaceGen, Alpha
		BSSM_AMBDIFFTEX_SF														= 137, // Ambient, Diffuse texture,	Skinned, Frond
		BSSM_AMBDIFFTEX_SFVc													= 138, // Ambient, Diffuse texture,	Skinned, Frond, Vertex colors
		BSSM_AMBDIFFTEX_SFG														= 139, // Ambient, Diffuse texture,	Skinned, Frond, Glow map
		BSSM_AMBDIFFTEX_SFGVc													= 140, // Ambient, Diffuse texture,	Skinned, Frond, Glow map, Vertex colors
		BSSM_AMBDIFFTEX_SFA														= 141, // Ambient, Diffuse texture,	Skinned, Frond, Alpha
		BSSM_AMBDIFFTEX_SFAVc													= 142, // Ambient, Diffuse texture,	Skinned, Frond, Alpha, Vertex colors
		BSSM_AMBDIFFTEX_SFFg													= 143, // Ambient, Diffuse texture,	Skinned, Frond, FaceGen
		BSSM_AMBDIFFTEX_SFGA													= 144, // Ambient, Diffuse texture,	Skinned, Frond, Glow map, Alpha
		BSSM_AMBDIFFTEX_SFGAVc													= 145, // Ambient, Diffuse texture,	Skinned, Frond, Glow map, Alpha, Vertex colors
		BSSM_AMBDIFFTEX_SFGFgA													= 146, // Ambient, Diffuse texture,	Skinned, Frond, Glow map, FaceGen, Alpha
		BSSM_LANDAD																= 147, // Landscape, Ambient, Diffuse
		BSSM_LANDAD_Shp															= 148, // Landscape, Ambient, Diffuse, Shadow pass
		BSSM_ADTS																= 149, // Ambient, Diffuse, Toggles, Specular
		BSSM_ADTS_G																= 150, // Ambient, Diffuse, Toggles, Specular, Glow map
		BSSM_ADTS_H																= 151, // Ambient, Diffuse, Toggles, Specular, Hair
		BSSM_ADTS_Fg															= 152, // Ambient, Diffuse, Toggles, Specular, FaceGen
		BSSM_ADTS_Px															= 153, // Ambient, Diffuse, Toggles, Specular, Parallax
		BSSM_ADTS_GPx															= 154, // Ambient, Diffuse, Toggles, Specular, Glow map, Parallax
		BSSM_ADTS_S																= 155, // Ambient, Diffuse, Toggles, Specular, Skinned
		BSSM_ADTS_SG															= 156, // Ambient, Diffuse, Toggles, Specular, Skinned, Glow map
		BSSM_ADTS_SH															= 157, // Ambient, Diffuse, Toggles, Specular, Skinned, Hair
		BSSM_ADTS_SFg															= 158, // Ambient, Diffuse, Toggles, Specular, Skinned, FaceGen
		BSSM_ADTS_Sb															= 159, // Ambient, Diffuse, Toggles, Specular, SpeedTree branch
		BSSM_ADTS_Shp															= 160, // Ambient, Diffuse, Toggles, Specular, Shadow pass
		BSSM_ADTS_GShp															= 161, // Ambient, Diffuse, Toggles, Specular, Glow map, Shadow pass
		BSSM_ADTS_HShp															= 162, // Ambient, Diffuse, Toggles, Specular, Hair, Shadow pass
		BSSM_ADTS_FgShp															= 163, // Ambient, Diffuse, Toggles, Specular, FaceGen, Shadow pass
		BSSM_ADTS_PxShp															= 164, // Ambient, Diffuse, Toggles, Specular, Parallax, Shadow pass
		BSSM_ADTS_GPxShp														= 165, // Ambient, Diffuse, Toggles, Specular, Glow map, Parallax, Shadow pass
		BSSM_ADTS_SShp															= 166, // Ambient, Diffuse, Toggles, Specular, Skinned, Shadow pass
		BSSM_ADTS_SGShp															= 167, // Ambient, Diffuse, Toggles, Specular, Skinned, Glow map, Shadow pass
		BSSM_ADTS_SHShp															= 168, // Ambient, Diffuse, Toggles, Specular, Skinned, Hair, Shadow pass
		BSSM_ADTS_SFgShp														= 169, // Ambient, Diffuse, Toggles, Specular, Skinned, FaceGen, Shadow pass
		BSSM_ADTS_SbShp															= 170, // Ambient, Diffuse, Toggles, Specular, SpeedTree branch, Shadow pass
		BSSM_ADTS2																= 171, // Ambient, Diffuse, Toggles, Specular, 2 Lights
		BSSM_ADTS2_G															= 172, // Ambient, Diffuse, Toggles, Specular, 2 Lights, Glow map
		BSSM_ADTS2_H															= 173, // Ambient, Diffuse, Toggles, Specular, 2 Lights, Hair
		BSSM_ADTS2_Fg															= 174, // Ambient, Diffuse, Toggles, Specular, 2 Lights, FaceGen
		BSSM_ADTS2_Px															= 175, // Ambient, Diffuse, Toggles, Specular, 2 Lights, Parallax
		BSSM_ADTS2_GPx															= 176, // Ambient, Diffuse, Toggles, Specular, 2 Lights, Glow map, Parallax
		BSSM_ADTS2_S															= 177, // Ambient, Diffuse, Toggles, Specular, 2 Lights, Skinned
		BSSM_ADTS2_SG															= 178, // Ambient, Diffuse, Toggles, Specular, 2 Lights, Skinned, Glow map
		BSSM_ADTS2_SH															= 179, // Ambient, Diffuse, Toggles, Specular, 2 Lights, Skinned, Hair
		BSSM_ADTS2_SFg															= 180, // Ambient, Diffuse, Toggles, Specular, 2 Lights, Skinned, FaceGen
		BSSM_ADTS2_Sb															= 181, // Ambient, Diffuse, Toggles, Specular, 2 Lights, SpeedTree branch
		BSSM_ADTS2_Shp															= 182, // Ambient, Diffuse, Toggles, Specular, 2 Lights, Shadow pass
		BSSM_ADTS2_GShp															= 183, // Ambient, Diffuse, Toggles, Specular, 2 Lights, Glow map, Shadow pass
		BSSM_ADTS2_HShp															= 184, // Ambient, Diffuse, Toggles, Specular, 2 Lights, Hair, Shadow pass
		BSSM_ADTS2_FgShp														= 185, // Ambient, Diffuse, Toggles, Specular, 2 Lights, FaceGen, Shadow pass
		BSSM_ADTS2_PxShp														= 186, // Ambient, Diffuse, Toggles, Specular, 2 Lights, Parallax, Shadow pass
		BSSM_ADTS2_GPxShp														= 187, // Ambient, Diffuse, Toggles, Specular, 2 Lights, Glow map, Parallax, Shadow pass
		BSSM_ADTS2_SShp															= 188, // Ambient, Diffuse, Toggles, Specular, 2 Lights, Skinned, Shadow pass
		BSSM_ADTS2_SGShp														= 189, // Ambient, Diffuse, Toggles, Specular, 2 Lights, Skinned, Glow map, Shadow pass
		BSSM_ADTS2_SHShp														= 190, // Ambient, Diffuse, Toggles, Specular, 2 Lights, Skinned, Hair, Shadow pass
		BSSM_ADTS2_SFgShp														= 191, // Ambient, Diffuse, Toggles, Specular, 2 Lights, Skinned, FaceGen, Shadow pass
		BSSM_ADTS2_SbShp														= 192, // Ambient, Diffuse, Toggles, Specular, 2 Lights, SpeedTree branch, Shadow pass
		BSSM_ADTS_ONELIGHT														= 193, // Ambient, Diffuse, Toggles, Specular, 1 Light
		BSSM_ADTS_DIRANDPT														= 194, // Ambient, Diffuse, Toggles, Specular, Directional and point light
		BSSM_ADT10																= 195, // Ambient, Diffuse, Toggles, 10 Lights
		BSSM_ADT10_G															= 196, // Ambient, Diffuse, Toggles, 10 Lights, Glow map
		BSSM_ADT10_Fg															= 197, // Ambient, Diffuse, Toggles, 10 Lights, FaceGen
		BSSM_ADT10_S															= 198, // Ambient, Diffuse, Toggles, 10 Lights, Skinned
		BSSM_ADT10_SG															= 199, // Ambient, Diffuse, Toggles, 10 Lights, Skinned, Glow map
		BSSM_ADT10_SFg															= 200, // Ambient, Diffuse, Toggles, 10 Lights, Skinned, FaceGen
		BSSM_ADT4																= 201, // Ambient, Diffuse, Toggles, 4 Lights
		BSSM_ADT4_Opt															= 202, // Ambient, Diffuse, Toggles, 4 Lights, Optimized
		BSSM_ADT4_G																= 203, // Ambient, Diffuse, Toggles, 4 Lights, Glow map
		BSSM_ADT4_Fg															= 204, // Ambient, Diffuse, Toggles, 4 Lights, FaceGen
		BSSM_ADT4_S																= 205, // Ambient, Diffuse, Toggles, 4 Lights, Skinned
		BSSM_ADT4_SG															= 206, // Ambient, Diffuse, Toggles, 4 Lights, Skinned, Glow map
		BSSM_ADT4_SFg															= 207, // Ambient, Diffuse, Toggles, 4 Lights, Skinned, FaceGen
		BSSM_ADTS10																= 208, // Ambient, Diffuse, Toggles, Specular
		BSSM_ADTS10_Opt															= 209, // Ambient, Diffuse, Toggles, Specular, Optimized
		BSSM_ADTS10_G															= 210, // Ambient, Diffuse, Toggles, Specular, Glow map
		BSSM_ADTS10_Fg															= 211, // Ambient, Diffuse, Toggles, Specular, FaceGen
		BSSM_ADTS10_S															= 212, // Ambient, Diffuse, Toggles, Specular, Skinned
		BSSM_ADTS10_SG															= 213, // Ambient, Diffuse, Toggles, Specular, Skinned, Glow map
		BSSM_ADTS10_SFg															= 214, // Ambient, Diffuse, Toggles, Specular, Skinned, FaceGen
		BSSM_AD2																= 215, // Ambient, Diffuse, 2 Lights
		BSSM_AD2_G																= 216, // Ambient, Diffuse, 2 Lights, Glow map
		BSSM_AD2_Px																= 217, // Ambient, Diffuse, 2 Lights, Parallax
		BSSM_AD2_GPx															= 218, // Ambient, Diffuse, 2 Lights, Glow map, Parallax
		BSSM_AD2_Fg																= 219, // Ambient, Diffuse, 2 Lights, FaceGen
		BSSM_AD2_S																= 220, // Ambient, Diffuse, 2 Lights, Skinned
		BSSM_AD2_SG																= 221, // Ambient, Diffuse, 2 Lights, Skinned, Glow map
		BSSM_AD2_SFg															= 222, // Ambient, Diffuse, 2 Lights, Skinned, FaceGen
		BSSM_AD2_Sb																= 223, // Ambient, Diffuse, 2 Lights, SpeedTree branch
		BSSM_AD2_Shp															= 224, // Ambient, Diffuse, 2 Lights, Shadow pass
		BSSM_AD2_GShp															= 225, // Ambient, Diffuse, 2 Lights, Glow map, Shadow pass
		BSSM_AD2_PxShp															= 226, // Ambient, Diffuse, 2 Lights, Parallax, Shadow pass
		BSSM_AD2_GPxShp															= 227, // Ambient, Diffuse, 2 Lights, Glow map, Parallax, Shadow pass
		BSSM_AD2_FgShp															= 228, // Ambient, Diffuse, 2 Lights, FaceGen, Shadow pass
		BSSM_AD2_SShp															= 229, // Ambient, Diffuse, 2 Lights, Skinned, Shadow pass
		BSSM_AD2_SGShp															= 230, // Ambient, Diffuse, 2 Lights, Skinned, Glow map, Shadow pass
		BSSM_AD2_SFgShp															= 231, // Ambient, Diffuse, 2 Lights, Skinned, FaceGen, Shadow pass
		BSSM_AD2_SbShp															= 232, // Ambient, Diffuse, 2 Lights, SpeedTree branch, Shadow pass
		BSSM_AD3																= 233, // Ambient, Diffuse, 3 Lights
		BSSM_AD3_G																= 234, // Ambient, Diffuse, 3 Lights, Glow map
		BSSM_AD3_Px																= 235, // Ambient, Diffuse, 3 Lights, Parallax
		BSSM_AD3_GPx															= 236, // Ambient, Diffuse, 3 Lights, Glow map, Parallax
		BSSM_AD3_Fg																= 237, // Ambient, Diffuse, 3 Lights, FaceGen
		BSSM_AD3_S																= 238, // Ambient, Diffuse, 3 Lights, Skinned
		BSSM_AD3_SG																= 239, // Ambient, Diffuse, 3 Lights, Skinned, Glow map
		BSSM_AD3_SFg															= 240, // Ambient, Diffuse, 3 Lights, Skinned, FaceGen
		BSSM_AD3_Sb																= 241, // Ambient, Diffuse, 3 Lights, SpeedTree branch
		BSSM_AD3_Shp															= 242, // Ambient, Diffuse, 3 Lights, Shadow pass
		BSSM_AD3_GShp															= 243, // Ambient, Diffuse, 3 Lights, Glow map, Shadow pass
		BSSM_AD3_PxShp															= 244, // Ambient, Diffuse, 3 Lights, Parallax, Shadow pass
		BSSM_AD3_GPxShp															= 245, // Ambient, Diffuse, 3 Lights, Glow map, Parallax, Shadow pass
		BSSM_AD3_FgShp															= 246, // Ambient, Diffuse, 3 Lights, FaceGen, Shadow pass
		BSSM_AD3_SShp															= 247, // Ambient, Diffuse, 3 Lights, Skinned, Shadow pass
		BSSM_AD3_SGShp															= 248, // Ambient, Diffuse, 3 Lights, Skinned, Glow map, Shadow pass
		BSSM_AD3_SFgShp															= 249, // Ambient, Diffuse, 3 Lights, Skinned, FaceGen, Shadow pass
		BSSM_AD3_SbShp															= 250, // Ambient, Diffuse, 3 Lights, SpeedTree branch, Shadow pass
		BSSM_ADT																= 251, // Ambient, Diffuse, Toggles
		BSSM_ADT_Opt															= 252, // Ambient, Diffuse, Toggles, Optimized
		BSSM_ADT_OptLOD															= 253, // Ambient, Diffuse, Toggles, Optimized, LOD
		BSSM_ADT_Mn																= 254, // Ambient, Diffuse, Toggles, Model space normals
		BSSM_ADT_G																= 255, // Ambient, Diffuse, Toggles, Glow map
		BSSM_ADT_Fg																= 256, // Ambient, Diffuse, Toggles, FaceGen
		BSSM_ADT_Px																= 257, // Ambient, Diffuse, Toggles, Parallax
		BSSM_ADT_PxOpt															= 258, // Ambient, Diffuse, Toggles, Parallax, Optimized
		BSSM_ADT_GPx															= 259, // Ambient, Diffuse, Toggles, Glow map, Parallax
		BSSM_ADT_H																= 260, // Ambient, Diffuse, Toggles, Hair
		BSSM_ADT_S																= 261, // Ambient, Diffuse, Toggles, Skinned
		BSSM_ADT_SG																= 262, // Ambient, Diffuse, Toggles, Skinned, Glow map
		BSSM_ADT_SFg															= 263, // Ambient, Diffuse, Toggles, Skinned, FaceGen
		BSSM_ADT_Sb																= 264, // Ambient, Diffuse, Toggles, SpeedTree branch
		BSSM_ADT_SH																= 265, // Ambient, Diffuse, Toggles, Skinned, Hair
		BSSM_ADT_Shp															= 266, // Ambient, Diffuse, Toggles, Shadow pass
		BSSM_ADT_MnShp															= 267, // Ambient, Diffuse, Toggles, Model space normals, Shadow pass
		BSSM_ADT_GShp															= 268, // Ambient, Diffuse, Toggles, Glow map, Shadow pass
		BSSM_ADT_FgShp															= 269, // Ambient, Diffuse, Toggles, FaceGen, Shadow pass
		BSSM_ADT_PxShp															= 270, // Ambient, Diffuse, Toggles, Parallax, Shadow pass
		BSSM_ADT_GPxShp															= 271, // Ambient, Diffuse, Toggles, Glow map, Parallax, Shadow pass
		BSSM_ADT_HShp															= 272, // Ambient, Diffuse, Toggles, Hair, Shadow pass
		BSSM_ADT_SShp															= 273, // Ambient, Diffuse, Toggles, Skinned, Shadow pass 
		BSSM_ADT_SGShp															= 274, // Ambient, Diffuse, Toggles, Skinned, Glow map, Shadow pass 
		BSSM_ADT_SFgShp															= 275, // Ambient, Diffuse, Toggles, Skinned, FaceGen, Shadow pass 
		BSSM_ADT_SbShp															= 276, // Ambient, Diffuse, Toggles, SpeedTree branch, Shadow pass 
		BSSM_ADT_SHShp															= 277, // Ambient, Diffuse, Toggles, Skinned, Hair, Shadow pass 
		BSSM_ADT2																= 278, // Ambient, Diffuse, Toggles, 2 Lights
		BSSM_ADT2_G																= 279, // Ambient, Diffuse, Toggles, 2 Lights, Glow map
		BSSM_ADT2_Fg															= 280, // Ambient, Diffuse, Toggles, 2 Lights, FaceGen
		BSSM_ADT2_Px															= 281, // Ambient, Diffuse, Toggles, 2 Lights, Parallax
		BSSM_ADT2_GPx															= 282, // Ambient, Diffuse, Toggles, 2 Lights, Glow map, Parallax
		BSSM_ADT2_H																= 283, // Ambient, Diffuse, Toggles, 2 Lights, Hair
		BSSM_ADT2_S																= 284, // Ambient, Diffuse, Toggles, 2 Lights, Skinned
		BSSM_ADT2_SG															= 285, // Ambient, Diffuse, Toggles, 2 Lights, Skinned, Glow map
		BSSM_ADT2_SFg															= 286, // Ambient, Diffuse, Toggles, 2 Lights, Skinned, FaceGen
		BSSM_ADT2_Sb															= 287, // Ambient, Diffuse, Toggles, 2 Lights, SpeedTree branch
		BSSM_ADT2_SH															= 288, // Ambient, Diffuse, Toggles, 2 Lights, Skinned, Hair
		BSSM_ADT2_Shp															= 289, // Ambient, Diffuse, Toggles, 2 Lights, Shadow pass
		BSSM_ADT2_GShp															= 290, // Ambient, Diffuse, Toggles, 2 Lights, Glow map, Shadow pass
		BSSM_ADT2_FgShp															= 291, // Ambient, Diffuse, Toggles, 2 Lights, FaceGen, Shadow pass
		BSSM_ADT2_PxShp															= 292, // Ambient, Diffuse, Toggles, 2 Lights, Parallax, Shadow pass
		BSSM_ADT2_GPxShp														= 293, // Ambient, Diffuse, Toggles, 2 Lights, Glow map, Parallax, Shadow pass
		BSSM_ADT2_HShp															= 294, // Ambient, Diffuse, Toggles, 2 Lights, Hair, Shadow pass
		BSSM_ADT2_SShp															= 295, // Ambient, Diffuse, Toggles, 2 Lights, Skinned, Shadow pass
		BSSM_ADT2_SGShp															= 296, // Ambient, Diffuse, Toggles, 2 Lights, Skinned, Glow map, Shadow pass
		BSSM_ADT2_SFgShp														= 297, // Ambient, Diffuse, Toggles, 2 Lights, Skinned, FaceGen, Shadow pass
		BSSM_ADT2_SbShp															= 298, // Ambient, Diffuse, Toggles, 2 Lights, SpeedTree branch, Shadow pass
		BSSM_ADT2_SHShp															= 299, // Ambient, Diffuse, Toggles, 2 Lights, Skinned, Hair, Shadow pass
		BSSM_DIFFUSEDIR															= 300, // Diffuse, Directional light
		BSSM_DIFFUSEPT															= 301, // Diffuse, Point light
		BSSM_DIFFUSEDIR_S														= 302, // Diffuse, Directional light, Skinned
		BSSM_DIFFUSEPT_S														= 303, // Diffuse, Point light, Skinned
		BSSM_DIFFUSEDIR_Sb														= 304, // Diffuse, Directional light, SpeedTree branch
		BSSM_DIFFUSEPT_Sb														= 305, // Diffuse, Point light, SpeedTree branch
		BSSM_DIFFUSEPT2															= 306, // Diffuse, 2 Point lights
		BSSM_DIFFUSEPT2_Fg														= 307, // Diffuse, 2 Point lights, FaceGen
		BSSM_DIFFUSEPT2_Px														= 308, // Diffuse, 2 Point lights, Parallax
		BSSM_DIFFUSEPT2_S														= 309, // Diffuse, 2 Point lights, Skinned
		BSSM_DIFFUSEPT2_SFg														= 310, // Diffuse, 2 Point lights, Skinned, FaceGen
		BSSM_DIFFUSEPT2_Sb														= 311, // Diffuse, 2 Point lights, SpeedTree branch
		BSSM_DIFFUSEPT2_Shp														= 312, // Diffuse, 2 Point lights, Shadow pass
		BSSM_DIFFUSEPT2_FgShp													= 313, // Diffuse, 2 Point lights, FaceGen, Shadow pass
		BSSM_DIFFUSEPT2_PxShp													= 314, // Diffuse, 2 Point lights, Parallax, Shadow pass
		BSSM_DIFFUSEPT2_SShp													= 315, // Diffuse, 2 Point lights, Skinned, Shadow pass
		BSSM_DIFFUSEPT2_SFgShp													= 316, // Diffuse, 2 Point lights, Skinned, FaceGen, Shadow pass
		BSSM_DIFFUSEPT2_SbShp													= 317, // Diffuse, 2 Point lights, SpeedTree branch, Shadow pass
		BSSM_DIFFUSEPT3															= 318, // Diffuse, 3 Point lights
		BSSM_DIFFUSEPT3_Fg														= 319, // Diffuse, 3 Point lights, FaceGen
		BSSM_DIFFUSEPT3_Px														= 320, // Diffuse, 3 Point lights, Parallax
		BSSM_DIFFUSEPT3_S														= 321, // Diffuse, 3 Point lights, Skinned
		BSSM_DIFFUSEPT3_SFg														= 322, // Diffuse, 3 Point lights, Skinned, FaceGen
		BSSM_DIFFUSEPT3_Sb														= 323, // Diffuse, 3 Point lights, SpeedTree branch
		BSSM_DIFFUSEPT3_Shp														= 324, // Diffuse, 3 Point lights, Shadow pass
		BSSM_DIFFUSEPT3_FgShp													= 325, // Diffuse, 3 Point lights, FaceGen, Shadow pass
		BSSM_DIFFUSEPT3_PxShp													= 326, // Diffuse, 3 Point lights, Parallax, Shadow pass
		BSSM_DIFFUSEPT3_SShp													= 327, // Diffuse, 3 Point lights, Skinned, Shadow pass
		BSSM_DIFFUSEPT3_SFgShp													= 328, // Diffuse, 3 Point lights, Skinned, FaceGen, Shadow pass
		BSSM_DIFFUSEPT3_SbShp													= 329, // Diffuse, 3 Point lights, SpeedTree branch, Shadow pass
		BSSM_TEXTURE															= 330, // Texture
		BSSM_TEXTURE_Fg															= 331, // Texture, FaceGen
		BSSM_TEXTURE_H															= 332, // Texture, Hair
		BSSM_TEXTURE_S															= 333, // Texture, Skinned
		BSSM_TEXTURE_Vc															= 334, // Texture, Vertex colors
		BSSM_TEXTURE_SVc														= 335, // Texture, Skinned, Vertex colors
		BSSM_TEXTURE_SFg														= 336, // Texture, Skinned, FaceGen
		BSSM_TEXTURE_Sb															= 337, // Texture, SpeedTree branch
		BSSM_TEXTURE_SH															= 338, // Texture, Skinned, Hair
		BSSM_TEXTURE_Px															= 339, // Texture, Parallax
		BSSM_SPECULARDIR														= 340, // Specular, Directional light
		BSSM_SPECULARPT															= 341, // Specular, Point light
		BSSM_SPECULARDIR_S														= 342, // Specular, Directional light, Skinned
		BSSM_SPECULARPT_S														= 343, // Specular, Point light, Skinned
		BSSM_UNK_344															= 344, //
		BSSM_UNK_345															= 345, //
		BSSM_2x_SPECULARDIR														= 346, // 2.0 Specular, Directional light
		BSSM_2x_SPECULARDIR_H													= 347, // 2.0 Specular, Directional light, Hair
		BSSM_2x_SPECULARDIR_Px													= 348, // 2.0 Specular, Directional light, Parallax
		BSSM_2x_SPECULARDIR_S													= 349, // 2.0 Specular, Directional light, Skinned
		BSSM_2x_SPECULARDIR_SH													= 350, // 2.0 Specular, Directional light, Skinned, Hair
		BSSM_2x_SPECULARDIR_Sb													= 351, // 2.0 Specular, Directional light, SpeedTree branch
		BSSM_2x_SPECULARDIR_Shp													= 352, // 2.0 Specular, Directional light, Shadow pass
		BSSM_2x_SPECULARDIR_HShp												= 353, // 2.0 Specular, Directional light, Hair, Shadow pass
		BSSM_2x_SPECULARDIR_PxShp												= 354, // 2.0 Specular, Directional light, Parallax, Shadow pass
		BSSM_2x_SPECULARDIR_SShp												= 355, // 2.0 Specular, Directional light, Skinned, Shadow pass
		BSSM_2x_SPECULARDIR_SHShp												= 356, // 2.0 Specular, Directional light, Skinned, Hair, Shadow pass
		BSSM_2x_SPECULARDIR_SbShp												= 357, // 2.0 Specular, Directional light, SpeedTree branch, Shadow pass
		BSSM_2x_SPECULARPT														= 358, // 2.0 Specular, Point light
		BSSM_2x_SPECULARPT_H													= 359, // 2.0 Specular, Point light, Hair
		BSSM_2x_SPECULARPT_Px													= 360, // 2.0 Specular, Point light, Parallax
		BSSM_2x_SPECULARPT_S													= 361, // 2.0 Specular, Point light, Skinned
		BSSM_2x_SPECULARPT_SH													= 362, // 2.0 Specular, Point light, Skinned, Hair
		BSSM_2x_SPECULARPT_Sb													= 363, // 2.0 Specular, Point light, SpeedTree branch
		BSSM_2x_SPECULARPT2														= 364, // 2.0 Specular, 2 Point lights
		BSSM_2x_SPECULARPT2_H													= 365, // 2.0 Specular, 2 Point lights, Hair
		BSSM_2x_SPECULARPT2_Px													= 366, // 2.0 Specular, 2 Point lights, Parallax
		BSSM_2x_SPECULARPT2_S													= 367, // 2.0 Specular, 2 Point lights, Skinned
		BSSM_2x_SPECULARPT2_SH													= 368, // 2.0 Specular, 2 Point lights, Skinned, Hair
		BSSM_2x_SPECULARPT2_Sb													= 369, // 2.0 Specular, 2 Point lights, SpeedTree branch
		BSSM_2x_SPECULARPT3														= 370, // 2.0 Specular, 3 Point lights
		BSSM_2x_SPECULARPT3_H													= 371, // 2.0 Specular, 3 Point lights, Hair
		BSSM_2x_SPECULARPT3_Px													= 372, // 2.0 Specular, 3 Point lights, Parallax
		BSSM_2x_SPECULARPT3_S													= 373, // 2.0 Specular, 3 Point lights, Skinned
		BSSM_2x_SPECULARPT3_SH													= 374, // 2.0 Specular, 3 Point lights, Skinned, Hair
		BSSM_2x_SPECULARPT3_Sb													= 375, // 2.0 Specular, 3 Point lights, SpeedTree branch
		BSSM_3XOCCLUSION														= 376, // 3.0 Occlusion query
		BSSM_UNK_377															= 377, //
		BSSM_UNK_378															= 378, //
		BSSM_UNK_379															= 379, //
		BSSM_UNK_380															= 380, //
		BSSM_UNK_381															= 381, //
		BSSM_UNK_382															= 382, //
		BSSM_3XLIGHTING															= 383, // 3.0 Lighting
		BSSM_3XLIGHTING_S														= 384, // 3.0 Lighting, Skinned
		BSSM_3XLIGHTING_H														= 385, // 3.0 Lighting, Hair
		BSSM_3XLIGHTING_Px														= 386, // 3.0 Lighting, Parallax
		BSSM_3XLIGHTING_Pxo														= 387, // 3.0 Lighting, Parallax occlusion
		BSSM_3XLIGHTING_Fg														= 388, // 3.0 Lighting, FaceGen
		BSSM_3XLIGHTING_SFg														= 389, // 3.0 Lighting, Skinned, FaceGen
		BSSM_3XLIGHTING_G														= 390, // 3.0 Lighting, Glow map
		BSSM_3XLIGHTING_SG														= 391, // 3.0 Lighting, Skinned, Glow map
		BSSM_3XLIGHTING_Vc														= 392, // 3.0 Lighting, Vertex colors
		BSSM_3XLIGHTING_VcS														= 393, // 3.0 Lighting, Vertex colors, Skinned
		BSSM_3XLIGHTING_VcPx													= 394, // 3.0 Lighting, Vertex colors, Parallax
		BSSM_3XLIGHTING_VcPxo													= 395, // 3.0 Lighting, Vertex colors, Parallax occlusion
		BSSM_3XLIGHTING_VcG														= 396, // 3.0 Lighting, Vertex colors, Glow map
		BSSM_3XLIGHTING_VcSG													= 397, // 3.0 Lighting, Vertex colors, Skinned, Glow map
		BSSM_3XLIGHTING_Spc														= 398, // 3.0 Lighting, Specular
		BSSM_3XLIGHTING_SSpc													= 399, // 3.0 Lighting, Skinned, Specular
		BSSM_3XLIGHTING_HSpc													= 400, // 3.0 Lighting, Hair, Specular
		BSSM_3XLIGHTING_PxSpc													= 401, // 3.0 Lighting, Parallax, Specular
		BSSM_3XLIGHTING_PxoSpc													= 402, // 3.0 Lighting, Parallax occlusion, Specular
		BSSM_3XLIGHTING_FgSpc													= 403, // 3.0 Lighting, FaceGen, Specular
		BSSM_3XLIGHTING_SFgSpc													= 404, // 3.0 Lighting, Skinned, FaceGen, Specular
		BSSM_3XLIGHTING_GSpc													= 405, // 3.0 Lighting, Glow map, Specular
		BSSM_3XLIGHTING_SGSpc													= 406, // 3.0 Lighting, Skinned, Glow map, Specular
		BSSM_3XLIGHTING_VcSpc													= 407, // 3.0 Lighting, Vertex colors, Specular
		BSSM_3XLIGHTING_VcSSpc													= 408, // 3.0 Lighting, Vertex colors, Skinned, Specular
		BSSM_3XLIGHTING_VcPxSpc													= 409, // 3.0 Lighting, Vertex colors, Parallax, Specular
		BSSM_3XLIGHTING_VcPxoSpc												= 410, // 3.0 Lighting, Vertex colors, Parallax Occlusion, Specular
		BSSM_3XLIGHTING_VcGSpc													= 411, // 3.0 Lighting, Vertex colors, Glow map, Specular
		BSSM_3XLIGHTING_VcSGSpc													= 412, // 3.0 Lighting, Vertex colors, Skinned, Glow map, Specular
		BSSM_3XLIGHTING_Shp														= 413, // 3.0 Lighting, Shadow pass
		BSSM_3XLIGHTING_SShp													= 414, // 3.0 Lighting, Skinned, Shadow pass
		BSSM_3XLIGHTING_HShp													= 415, // 3.0 Lighting, Hair, Shadow pass
		BSSM_3XLIGHTING_PxShp													= 416, // 3.0 Lighting, Parallax, Shadow pass
		BSSM_3XLIGHTING_PxoShp													= 417, // 3.0 Lighting, Parallax occlusion, Shadow pass
		BSSM_3XLIGHTING_FgShp													= 418, // 3.0 Lighting, FaceGen, Shadow pass
		BSSM_3XLIGHTING_SFgShp													= 419, // 3.0 Lighting, Skinned, FaceGen, Shadow pass
		BSSM_3XLIGHTING_GShp													= 420, // 3.0 Lighting, Glow map, Shadow pass
		BSSM_3XLIGHTING_SGShp													= 421, // 3.0 Lighting, Skinned, Glow map, Shadow pass
		BSSM_3XLIGHTING_VcShp													= 422, // 3.0 Lighting, Vertex colors, Shadow pass
		BSSM_3XLIGHTING_VcSShp													= 423, // 3.0 Lighting, Vertex colors, Skinned, Shadow pass
		BSSM_3XLIGHTING_VcPxShp													= 424, // 3.0 Lighting, Vertex colors, Parallax, Shadow pass
		BSSM_3XLIGHTING_VcPxoShp												= 425, // 3.0 Lighting, Vertex colors, Parallax occlusion, Shadow pass
		BSSM_3XLIGHTING_VcGShp													= 426, // 3.0 Lighting, Vertex colors, Glow map, Shadow pass
		BSSM_3XLIGHTING_VcSGShp													= 427, // 3.0 Lighting, Vertex colors, Skinned, Glow map, Shadow pass
		BSSM_3XLIGHTING_ShpSpc													= 428, // 3.0 Lighting, Shadow pass, Specular 
		BSSM_3XLIGHTING_SShpSpc													= 429, // 3.0 Lighting, Skinned, Shadow pass, Specular
		BSSM_3XLIGHTING_HShpSpc													= 430, // 3.0 Lighting, Hair, Shadow pass, Specular
		BSSM_3XLIGHTING_PxShpSpc												= 431, // 3.0 Lighting, Parallax, Shadow pass, Specular
		BSSM_3XLIGHTING_PxoShpSpc												= 432, // 3.0 Lighting, Parallax occlusion, Shadow pass, Specular
		BSSM_3XLIGHTING_FgShpSpc												= 433, // 3.0 Lighting, FaceGen, Shadow pass, Specular
		BSSM_3XLIGHTING_SFgShpSpc												= 434, // 3.0 Lighting, Skinned, FaceGen, Shadow pass, Specular
		BSSM_3XLIGHTING_GShpSpc													= 435, // 3.0 Lighting, Glow map, Shadow pass, Specular
		BSSM_3XLIGHTING_SGShpSpc												= 436, // 3.0 Lighting, Skinned, Glow map, Shadow pass, Specular
		BSSM_3XLIGHTING_VcShpSpc												= 437, // 3.0 Lighting, Vertex colors, Shadow pass, Specular
		BSSM_3XLIGHTING_VcSShpSpc												= 438, // 3.0 Lighting, Vertex colors, Skinned, Shadow pass, Specular
		BSSM_3XLIGHTING_VcPxShpSpc												= 439, // 3.0 Lighting, Vertex colors, Parallax, Shadow pass, Specular
		BSSM_3XLIGHTING_VcPxoShpSpc												= 440, // 3.0 Lighting, Vertex colors, Parallax occlusion, Shadow pass, Specular
		BSSM_3XLIGHTING_VcGShpSpc												= 441, // 3.0 Lighting, Vertex colors, Glow map, Shadow pass, Specular
		BSSM_3XLIGHTING_VcSGShpSpc												= 442, // 3.0 Lighting, Vertex colors, Skinned, Glow map, Shadow pass, Specular
		BSSM_3XSILHOUETTE														= 443, // 3.0 Silhouette
		BSSM_3XSILHOUETTE_Vc													= 444, // 3.0 Silhouette, Vertex colors
		BSSM_3XENVMAP															= 445, // 3.0 Environment map
		BSSM_3XENVMAP_W															= 446, // 3.0 Environment map, Window
		BSSM_3XENVMAP_Vc														= 447, // 3.0 Environment map, Vertex colors
		BSSM_3XENVMAP_WVc														= 448, // 3.0 Environment map, Window, Vertex colors
		BSSM_3XENVMAP_S															= 449, // 3.0 Environment map, Skinned
		BSSM_3XENVMAP_SVc														= 450, // 3.0 Environment map, Skinned, Vertex colors
		BSSM_3XENVMAP_EYE														= 451, // 3.0 Environment map, Eye
		BSSM_3XLIGHTING_SIMPLESHADOW											= 452, // 3.0 Simple shadow
		BSSM_3XLIGHTING_SIMPLESHADOW_Vc											= 453, // 3.0 Simple shadow, Vertex colors
		BSSM_3XLIGHTING_SIMPLESHADOW_S											= 454, // 3.0 Simple shadow, Skinned
		BSSM_3XLIGHTING_SIMPLESHADOW_VcS										= 455, // 3.0 Simple shadow, Vertex colors, Skinned
		BSSM_3XDEPTHMAP															= 456, // 3.0 Depth map
		BSSM_3XDEPTHMAP_Vc														= 457, // 3.0 Depth map, Vertex colors
		BSSM_3XDEPTHMAP_S														= 458, // 3.0 Depth map, Skinned
		BSSM_3XDEPTHMAP_VcS														= 459, // 3.0 Depth map, Vertex colors, Skinned
		BSSM_3XRENDERNORMALS													= 460, // 3.0 Normals
		BSSM_3XRENDERNORMALS_Vc													= 461, // 3.0 Normals, Vertex colors
		BSSM_3XRENDERNORMALS_S													= 462, // 3.0 Normals, Skinned
		BSSM_3XRENDERNORMALS_VcS												= 463, // 3.0 Normals, Vertex colors, Skinned
		BSSM_3XRENDERNORMALS_FIRE												= 464, // 3.0 Normals, Fire
		BSSM_3XRENDERNORMALS_CLEAR												= 465, // 3.0 Normals, Clear
		BSSM_3XRENDERNORMALS_CLEAR_S											= 466, // 3.0 Normals, Clear, Skinned
		BSSM_3XCONSTALPHA														= 467, // 3.0 Constant alpha
		BSSM_3XCONSTALPHA_Vc													= 468, // 3.0 Constant alpha, Vertex colors
		BSSM_3XCONSTALPHA_S														= 469, // 3.0 Constant alpha, Skinned
		BSSM_3XCONSTALPHA_SVc													= 470, // 3.0 Constant alpha, Skinned, Vertex colors
		BSSM_3XDEPTH															= 471, // 3.0 Depth
		BSSM_3XDEPTH_Vc															= 472, // 3.0 Depth, Vertex colors
		BSSM_3XDEPTH_S															= 473, // 3.0 Depth, Skinned
		BSSM_3XDEPTH_SVc														= 474, // 3.0 Depth, Skinned, Vertex colors
		BSSM_3XVELOCITY															= 475, // 3.0 Velocity
		BSSM_3XVELOCITY_S														= 476, // 3.0 Velocity, Skinned
		BSSM_3XLOCALMAP															= 477, // 3.0 Local map
		BSSM_3XLOCALMAP_S														= 478, // 3.0 Local map, Skinned
		BSSM_3XLOCALMAP_CLEAR													= 479, // 3.0 Local map, Clear
		BSSM_3XTEXEFFECT														= 480, // 3.0 Texture effect
		BSSM_3XTEXEFFECT_S														= 481, // 3.0 Texture effect, Skinned
		BSSM_RENDERNORMALS														= 482, // Normals
		BSSM_RENDERNORMALS_S													= 483, // Normals, Skinned
		BSSM_RENDERNORMALS_FIRE													= 484, // Normals, Fire
		BSSM_RENDERNORMALS_CLEAR												= 485, // Normals, Clear
		BSSM_RENDERNORMALS_CLEAR_S												= 486, // Normals, Clear, Skinned
		BSSM_DEPTH																= 487, // Depth
		BSSM_DEPTH_S															= 488, // Depth, Skinned
		BSSM_DEPTH_Mn															= 489, // Depth, Model space normals
		BSSM_VELOCITY															= 490, // Velocity
		BSSM_VELOCITY_S															= 491, // Velocity, Skinned
		BSSM_LOCALMAP															= 492, // Local map
		BSSM_LOCALMAP_S															= 493, // Local map, Skinned
		BSSM_LOCALMAP_CLEAR														= 494, // Local map, Clear
		BSSM_LANDDIFF															= 495, // Landscape, Diffuse
		BSSM_LAND2xDIFF															= 496, // Landscape, 2.0 Diffuse
		BSSM_LAND_G																= 497, // Landscape, Glow map
		BSSM_LANDAD_A															= 498, // Landscape, Ambient, Diffuse, Alpha
		BSSM_LANDAD_AShp														= 499, // Landscape, Ambient, Diffuse, Alpha, Shadow pass
		BSSM_LAND_GA															= 500, // Landscape, Glow map, Alpha
		BSSM_LANDDIFF_A															= 501, // Landscape, Diffuse, Alpha
		BSSM_LAND2xDIFF_A														= 502, // Landscape, 2.0 Diffuse, Alpha
		BSSM_LAND1O																= 503, // Landscape, 1 Texture
		BSSM_LAND1O_Shp															= 504, // Landscape, 1 Texture, Shadow pass
		BSSM_LAND1O_Simple														= 505, // Landscape, 1 Texture, Simple
		BSSM_LAND1O_SimpleShp													= 506, // Landscape, 1 Texture, Simple, Shadow pass
		BSSM_LAND1O4															= 507, // Landscape, 1 Texture, 4 Lights
		BSSM_LAND1O4_Shp														= 508, // Landscape, 1 Texture, 4 Lights, Shadow pass
		BSSM_LAND1O4_Simple														= 509, // Landscape, 1 Texture, 4 Lights, Simple
		BSSM_LAND1O4_SimpleShp													= 510, // Landscape, 1 Texture, 4 Lights, Simple, Shadow pass
		BSSM_LAND2O																= 511, // Landscape, 2 Textures
		BSSM_LAND2O_Shp															= 512, // Landscape, 2 Textures, Shadow pass
		BSSM_LAND2O_Simple														= 513, // Landscape, 2 Textures, Simple
		BSSM_LAND2O_SimpleShp													= 514, // Landscape, 2 Textures, Simple, Shadow pass
		BSSM_LAND2O4															= 515, // Landscape, 2 Textures, 4 Lights
		BSSM_LAND2O4_Shp														= 516, // Landscape, 2 Textures, 4 Lights, Shadow pass
		BSSM_LAND2O4_Simple														= 517, // Landscape, 2 Textures, 4 Lights, Simple
		BSSM_LAND2O4_SimpleShp													= 518, // Landscape, 2 Textures, 4 Lights, Simple, Shadow pass
		BSSM_LAND3O																= 519, // Landscape, 3 Textures
		BSSM_LAND3O_Shp															= 520, // Landscape, 3 Textures, Shadow pass
		BSSM_LAND3O_Simple														= 521, // Landscape, 3 Textures, Simple
		BSSM_LAND3O_SimpleShp													= 522, // Landscape, 3 Textures, Simple, Shadow pass
		BSSM_LAND3O4															= 523, // Landscape, 3 Textures, 4 Lights
		BSSM_LAND3O4_Shp														= 524, // Landscape, 3 Textures, 4 Lights, Shadow pass
		BSSM_LAND3O4_Simple														= 525, // Landscape, 3 Textures, 4 Lights, Simple
		BSSM_LAND3O4_SimpleShp													= 526, // Landscape, 3 Textures, 4 Lights, Simple, Shadow pass
		BSSM_LAND4O																= 527, // Landscape, 4 Textures
		BSSM_LAND4O_Shp															= 528, // Landscape, 4 Textures, Shadow pass
		BSSM_LAND4O_Simple														= 529, // Landscape, 4 Textures, Simple
		BSSM_LAND4O_SimpleShp													= 530, // Landscape, 4 Textures, Simple, Shadow pass
		BSSM_LAND4O4															= 531, // Landscape, 4 Textures, 4 Lights
		BSSM_LAND4O4_Shp														= 532, // Landscape, 4 Textures, 4 Lights, Simple
		BSSM_LAND4O4_Simple														= 533, // Landscape, 4 Textures, 4 Lights, Simple
		BSSM_LAND4O4_SimpleShp													= 534, // Landscape, 4 Textures, 4 Lights, Simple, Shadow pass
		BSSM_LAND5O																= 535, // Landscape, 5 Textures
		BSSM_LAND5O_Shp															= 536, // Landscape, 5 Textures, Shadow pass
		BSSM_LAND5O_Simple														= 537, // Landscape, 5 Textures, Simple
		BSSM_LAND5O_SimpleShp													= 538, // Landscape, 5 Textures, Simple, Shadow pass
		BSSM_LAND5O4															= 539, // Landscape, 5 Textures, 4 Lights
		BSSM_LAND5O4_Shp														= 540, // Landscape, 5 Textures, 4 Lights, Shadow pass
		BSSM_LAND5O4_Simple														= 541, // Landscape, 5 Textures, 4 Lights, Simple
		BSSM_LAND5O4_SimpleShp													= 542, // Landscape, 5 Textures, 4 Lights, Simple, Shadow pass
		BSSM_LAND6O																= 543, // Landscape, 6 Textures
		BSSM_LAND6O_Shp															= 544, // Landscape, 6 Textures, Shadow pass
		BSSM_LAND6O_Simple														= 545, // Landscape, 6 Textures, Simple
		BSSM_LAND6O_SimpleShp													= 546, // Landscape, 6 Textures, Simple, Shadow pass
		BSSM_LAND6O4															= 547, // Landscape, 6 Textures, 4 Lights
		BSSM_LAND6O4_Shp														= 548, // Landscape, 6 Textures, 4 Lights, Shadow pass
		BSSM_LAND6O4_Simple														= 549, // Landscape, 6 Textures, 4 Lights, Simple
		BSSM_LAND6O4_SimpleShp													= 550, // Landscape, 6 Textures, 4 Lights, Simple, Shadow pass
		BSSM_LAND7O																= 551, // Landscape, 7 Textures
		BSSM_LAND7O_Shp															= 552, // Landscape, 7 Textures, Shadow pass
		BSSM_LAND7O_Simple														= 553, // Landscape, 7 Textures, Simple
		BSSM_LAND7O_SimpleShp													= 554, // Landscape, 7 Textures, Simple, Shadow pass
		BSSM_LAND7O4															= 555, // Landscape, 7 Textures, 4 Lights
		BSSM_LAND7O4_Shp														= 556, // Landscape, 7 Textures, 4 Lights, Shadow pass
		BSSM_LAND7O4_Simple														= 557, // Landscape, 7 Textures, 4 Lights, Simple
		BSSM_LAND7O4_SimpleShp													= 558, // Landscape, 7 Textures, 4 Lights, Simple, Shadow pass
		BSSM_LANDO_Am															= 559, // Landscape, Ambient
		BSSM_LANDLO_A															= 560, // Landscape, Alpha (LOD fade)
		BSSM_2x_SIMPLESHADOW													= 561, // 2.0 Simple shadow
		BSSM_2x_SIMPLESHADOW_S													= 562, // 2.0 Simple shadow, Skinned
		BSSM_2x_SIMPLESHADOW_LAND												= 563, // 2.0 Simple shadow, Landscape
		BSSM_2x_SIMPLESHADOW_Sb													= 564, // 2.0 Simple shadow, SpeedTree branch
		BSSM_ADT_Sbb															= 565, // Ambient, Diffuse, Toggles, ???
		BSSM_DISTANT_TREE_BASE													= 566, // Distant Tree, Base
		BSSM_DISTANT_TREE_SILHOUETTE											= 567, // Distant Tree, Silhouette
		BSSM_SKYBASEPRE															= 568, // Sky, Pre-pass
		BSSM_SKY																= 569, // Sky
		BSSM_SKY_MOON_STARS_MASK												= 570, // Sky, Moon and stars mask
		BSSM_SKY_STARS															= 571, // Sky, Stars
		BSSM_SKY_TEXTURE														= 572, // Sky, Texture
		BSSM_SKY_CLOUDS															= 573, // Sky, Clouds
		BSSM_SKY_CLOUDS_UNDERWATER												= 574, // Sky, Clouds, Underwater
		BSSM_WATER_LOD															= 575, // Water LOD
		BSSM_WATER_LOD_NO_REFLECTIONS											= 576, // Water LOD, No reflections
		BSSM_PARTICLE															= 577, // Particles
		BSSM_BOLT																= 578, // Bolts
		BSSM_BEAM																= 579, // Beams
		BSSM_ENVMAP																= 580, // Environment map
		BSSM_ENVMAP_Vc															= 581, // Environment map, Vertex colors
		BSSM_ENVMAP_S															= 582, // Environment map, Skinned
		BSSM_ENVMAP_SVc															= 583, // Environment map, Skinned, Vertex colors
		BSSM_2x_ENVMAP															= 584, // 2.0 Environment map
		BSSM_2x_ENVMAP_S														= 585, // 2.0 Environment map, Skinned
		BSSM_2x_ENVMAP_W														= 586, // 2.0 Environment map, Window
		BSSM_2x_ENVMAP_EYE														= 587, // 2.0 Environment map, Eye
		BSSM_GEOMDECAL															= 588, // Geometry decal
		BSSM_GEOMDECAL_S														= 589, // Geometry decal, Skinned
		BSSM_TEXEFFECT															= 590, // Texture effect
		BSSM_TEXEFFECT_S														= 591, // Texture effect, Skinned
		BSSM_2x_TEXEFFECT														= 592, // 2.0 Texture effect 
		BSSM_2x_TEXEFFECT_S														= 593, // 2.0 Texture effect, Skinned
		BSSM_FOG																= 594, // Fog
		BSSM_FOG_A																= 595, // Fog, Alpha
		BSSM_FOG_S																= 596, // Fog, Skinned
		BSSM_FOG_SA																= 597, // Fog, Skinned, Alpha
		BSSM_FOG_Sb																= 598, // Fog, SpeedTree branch
		BSSM_UNK_599															= 599, //
		BSSM_UNK_600															= 600, //
		BSSM_VFOG_1																= 601, // Volumetric fog
		BSSM_VFOG_2																= 602, // Volumetric fog
		BSSM_VFOG_3																= 603, // Volumetric fog
		BSSM_GRASS_DIRONLY_LF													= 604, // Grass, Directional light, ???, Flat lighting
		BSSM_GRASS_DIRONLY_LV													= 605, // Grass, Directional light, ???, Vertex lighting
		BSSM_GRASS_DIRONLY_LFS													= 606, // Grass, Directional light, ???, Flat lighting, Fit slope
		BSSM_GRASS_DIRONLY_LVS													= 607, // Grass, Directional light, ???, Vertex lighting, Fit slope
		BSSM_GRASS_DIRONLY_LFP													= 608, // Grass, Directional light, ???, Flat lighting, ???
		BSSM_GRASS_DIRONLY_LVP													= 609, // Grass, Directional light, ???, Vertex lighting, ???
		BSSM_GRASS_DIRONLY_LFSP													= 610, // Grass, Directional light, ???, Flat lighting, Fit slope, ???
		BSSM_GRASS_DIRONLY_LVSP													= 611, // Grass, Directional light, ???, Vertex lighting, Fit slope, ???
		BSSM_GRASS_DIRONLY_LFShp												= 612, // Grass, Directional light, ???, Flat lighting, Shadow pass
		BSSM_GRASS_DIRONLY_LVShp												= 613, // Grass, Directional light, ???, Vertex lighting, Fit slope, Shadow pass
		BSSM_GRASS_DIRONLY_LFSShp												= 614, // Grass, Directional light, ???, Flat lighting, Fit slope, Shadow pass
		BSSM_GRASS_DIRONLY_LVSShp												= 615, // Grass, Directional light, ???, Vertex lighting, Fit slope, Shadow pass
		BSSM_GRASS_DIRONLY_LFPShp												= 616, // Grass, Directional light, ???, Flat lighting, ???, Shadow pass
		BSSM_GRASS_DIRONLY_LVPShp												= 617, // Grass, Directional light, ???, Vertex lighting, ???, Shadow pass
		BSSM_GRASS_DIRONLY_LFSPShp												= 618, // Grass, Directional light, ???, Flat lighting, Fit slope, ???, Shadow pass
		BSSM_GRASS_DIRONLY_LVSPShp												= 619, // Grass, Directional light, ???, Vertex lighting, Fit slope, ???, Shadow pass
		BSSM_GRASS_DIRONLY_LFB													= 620, // Grass, Directional light, ???, Flat lighting, Billboard
		BSSM_GRASS_DIRONLY_LFSB													= 621, // Grass, Directional light, ???, Flat lighting, Fit slope, Billboard
		BSSM_GRASS_DIRONLY_LFPB													= 622, // Grass, Directional light, ???, Flat lighting, ???, Billboard
		BSSM_GRASS_DIRONLY_LFSPB												= 623, // Grass, Directional light, ???, Flat lighting, Fit slope, ???, Billboard
		BSSM_GRASS_1POINT_LF													= 624, // Grass, Point light, ???, Flat lighting
		BSSM_GRASS_1POINT_LV													= 625, // Grass, Point light, ???, Vertex lighting
		BSSM_GRASS_1POINT_LFS													= 626, // Grass, Point light, ???, Flat lighting, Fit slope
		BSSM_GRASS_1POINT_LVS													= 627, // Grass, Point light, ???, Vertex lighting, Fit slope
		BSSM_GRASS_1POINT_LFP													= 628, // Grass, Point light, ???, Flag lighting, ???
		BSSM_GRASS_1POINT_LVP													= 629, // Grass, Point light, ???, Vertex lighting, ???
		BSSM_GRASS_1POINT_LFSP													= 630, // Grass, Point light, ???, Flat lighting, Fit slope, ???
		BSSM_GRASS_1POINT_LVSP													= 631, // Grass, Point light, ???, Vertex lighting, Fit slope, ???
		BSSM_GRASS_1POINT_LFShp													= 632, // Grass, Point light, ???, Flat lighting, Shadow pass
		BSSM_GRASS_1POINT_LVShp													= 633, // Grass, Point light, ???, Vertex lighting, Shadow pass
		BSSM_GRASS_1POINT_LFSShp												= 634, // Grass, Point light, ???, Flat lighting, Fit slope, Shadow pass
		BSSM_GRASS_1POINT_LVSShp												= 635, // Grass, Point light, ???, Vertex lighting, Fit slope, Shadow pass
		BSSM_GRASS_1POINT_LFPShp												= 636, // Grass, Point light, ???, Flat lighting, ???, Shadow pass
		BSSM_GRASS_1POINT_LVPShp												= 637, // Grass, Point light, ???, Vertex lighting, ???, Shadow pass
		BSSM_GRASS_1POINT_LFSPShp												= 638, // Grass, Point light, ???, Flat lighting, Fit slope, ???, Shadow pass
		BSSM_GRASS_1POINT_LVSPShp												= 639, // Grass, Point light, ???, Vertex lighting, Fit slope, ???, Shadow pass
		BSSM_GRASS_1POINT_LFB													= 640, // Grass, Point light, ???, Flat lighting, Billboard
		BSSM_GRASS_1POINT_LFSB													= 641, // Grass, Point light, ???, Flat lighting, Fit slope, Billboard
		BSSM_GRASS_1POINT_LFPB													= 642, // Grass, Point light, ???, Flat lighting, ???, Billboard
		BSSM_GRASS_1POINT_LFSPB													= 643, // Grass, Point light, ???, Flat lighting, Fit slope, ???, Billboard
		BSSM_GRASS_SHADOW_L														= 644, // Grass, Shadow, ???
		BSSM_GRASS_SHADOW_LS													= 645, // Grass, Shadow, ???, Fit slope
		BSSM_GRASS_SHADOW_LB													= 646, // Grass, Shadow, ???, Billboard
		BSSM_GRASS_SHADOW_LSB													= 647, // Grass, Shadow, ???, Fit slope, Billboard
		BSSM_WATER_STENCIL														= 648, // Water, Stencil
		BSSM_WATER_STENCIL_Vc													= 649, // Water, Stencil, Vertex colors
		BSSM_WATER_WADING														= 650, // Water, Wading
		BSSM_WATER_WADING_NO_REFLECTIONS_NO_REFRACTIONS							= 651, // Water, Wading, No reflections, No refractions
		BSSM_WATER_WADING_NO_REFRACTIONS										= 652, // Water, Wading, No refractions
		BSSM_WATER_WADING_NO_REFLECTIONS										= 653, // Water, Wading, No reflections
		BSSM_WATER_WADING_NO_DEPTH												= 654, // Water, Wading, No depth
		BSSM_WATER_WADING_NO_DEPTH_NO_REFLECTIONS_NO_REFRACTIONS				= 655, // Water, Wading, No depth, No reflections, No refractions
		BSSM_WATER_WADING_NO_DEPTH_NO_REFRACTIONS								= 656, // Water, Wading, No depth, No refractions
		BSSM_WATER_WADING_NO_DEPTH_NO_REFLECTIONS								= 657, // Water, Wading, No depth, No reflections
		BSSM_WATER_INTERIOR_WADING												= 658, // Water, Interior, Wading
		BSSM_WATER_INTERIOR_WADING_NO_REFLECTIONS_NO_REFRACTIONS				= 659, // Water, Interior, Wading, No reflections, No refractions
		BSSM_WATER_INTERIOR_WADING_NO_REFRACTIONS								= 660, // Water, Interior, Wading, No refractions
		BSSM_WATER_INTERIOR_WADING_NO_REFLECTIONS								= 661, // Water, Interior, Wading, No reflections
		BSSM_WATER_INTERIOR_WADING_NO_DEPTH										= 662, // Water, Interior, Wading, No depth
		BSSM_WATER_INTERIOR_WADING_NO_DEPTH_NO_REFLECTIONS_NO_REFRACTIONS		= 663, // Water, Interior, Wading, No depth, No reflections, No refractions
		BSSM_WATER_INTERIOR_WADING_NO_DEPTH_NO_REFRACTIONS						= 664, // Water, Interior, Wading, No depth, No refractions
		BSSM_WATER_INTERIOR_WADING_NO_DEPTH_NO_REFLECTIONS						= 665, // Water, Interior, Wading, No depth, No reflections
		BSSM_WATER_WADING_SPECULAR_LIGHTING										= 666, // Water, Wading, Specular lighting
		BSSM_WATER_WADING_Vc													= 667, // Water, Wading, Vertex colors
		BSSM_WATER_WADING_NO_REFLECTIONS_Vc										= 668, // Water, Wading, No reflections, Vertex colors
		BSSM_WATER_WADING_NO_REFRACTIONS_Vc										= 669, // Water, Wading, No refractions, Vertex colors
		BSSM_WATER_WADING_NO_REFLECTIONS_NO_REFRACTIONS_Vc						= 670, // Water, Wading, No reflections, No refractions, Vertex colors
		BSSM_WATER_WADING_NO_DEPTH_Vc											= 671, // Water, Wading, No depth, Vertex colors
		BSSM_WATER_WADING_NO_DEPTH_NO_REFLECTIONS_Vc							= 672, // Water, Wading, No depth, No reflections, Vertex colors
		BSSM_WATER_WADING_NO_DEPTH_NO_REFRACTIONS_Vc							= 673, // Water, Wading, No depth, No refractions, Vertex colors
		BSSM_WATER_WADING_NO_DEPTH_NO_REFLECTIONS_NO_REFRACTIONS_Vc				= 674, // Water, Wading, No depth, No reflections, No refractions, Vertex colors
		BSSM_WATER_INTERIOR_WADING_Vc											= 675, // Water, Wading, Vertex colors
		BSSM_WATER_INTERIOR_WADING_NO_REFLECTIONS_Vc							= 676, // Water, Interior, Wading, No reflections, Vertex colors
		BSSM_WATER_INTERIOR_WADING_NO_REFRACTIONS_Vc							= 677, // Water, Interior, Wading, No refractions, Vertex colors
		BSSM_WATER_INTERIOR_WADING_NO_REFLECTIONS_NO_REFRACTIONS_Vc				= 678, // Water, Interior, Wading, No reflections, No refractions, Vertex colors
		BSSM_WATER_INTERIOR_WADING_NO_DEPTH_Vc									= 679, // Water, Interior, Wading, No depth, Vertex colors
		BSSM_WATER_INTERIOR_WADING_NO_DEPTH_NO_REFLECTIONS_Vc					= 680, // Water, Interior, Wading, No depth, No reflections, Vertex colors
		BSSM_WATER_INTERIOR_WADING_NO_DEPTH_NO_REFRACTIONS_Vc					= 681, // Water, Interior, Wading, No depth, No refractions, Vertex colors
		BSSM_WATER_INTERIOR_WADING_NO_DEPTH_NO_REFLECTIONS_NO_REFRACTIONS_Vc	= 682, // Water, Interior, Wading, No depth, No reflections, No refractions, Vertex colors
		BSSM_WATER_WADING_SPECULAR_LIGHTING_Vc									= 683, // Water, Wading, Specular lighting
		BSSM_WATER																= 684, // Water
		BSSM_WATER_NO_REFLECTIONS_NO_REFRACTIONS								= 685, // Water, No reflections, No refractions
		BSSM_WATER_NO_REFRACTIONS												= 686, // Water, No refractions
		BSSM_WATER_NO_REFLECTIONS												= 687, // Water, No reflections
		BSSM_WATER_INTERIOR														= 688, // Water, Interior
		BSSM_WATER_INTERIOR_NO_REFLECTIONS_NO_REFRACTIONS						= 689, // Water, Interior, No reflections, No refractions
		BSSM_WATER_INTERIOR_NO_REFRACTIONS										= 690, // Water, Interior, No refractions
		BSSM_WATER_INTERIOR_NO_REFLECTIONS										= 691, // Water, Interior, No reflections
		BSSM_WATER_NODEPTH														= 692, // Water, No depth
		BSSM_WATER_NODEPTH_NO_REFLECTIONS										= 693, // Water, No depth, No reflections
		BSSM_WATER_NODEPTH_NO_REFRACTIONS										= 694, // Water, No depth, No refractions
		BSSM_WATER_NODEPTH_NO_REFLECTIONS_NO_REFRACTIONS						= 695, // Water, No depth, No reflections, No refractions
		BSSM_WATER_INTERIOR_NODEPTH												= 696, // Water, Interior, No depth
		BSSM_WATER_INTERIOR_NODEPTH_NO_REFLECTIONS_NO_REFRACTIONS				= 697, // Water, Interior, No depth, No reflections, No refractions
		BSSM_WATER_INTERIOR_NODEPTH_NO_REFRACTIONS								= 698, // Water, Interior, No depth, No refractions
		BSSM_WATER_INTERIOR_NODEPTH_NO_REFLECTIONS								= 699, // Water, Interior, No depth, No reflections
		BSSM_WATER_SPECULAR_LIGHTING											= 700, // Water, Specular lighting
		BSSM_WATER_UNDERWATER													= 701, // Water, Underwater
		BSSM_WATER_Vc															= 702, // Water, Vertex colors
		BSSM_WATER_NO_REFLECTIONS_NO_REFRACTIONS_Vc								= 703, // Water, No reflections, No refractions, Vertex colors
		BSSM_WATER_NO_REFRACTIONS_Vc											= 704, // Water, No refractions, Vertex colors
		BSSM_WATER_NO_REFLECTIONS_Vc											= 705, // Water, No reflections, Vertex colors
		BSSM_WATER_INTERIOR_Vc													= 706, // Water, Interior, Vertex colors
		BSSM_WATER_INTERIOR_NO_REFLECTIONS_NO_REFRACTIONS_Vc					= 707, // Water, Interior, No reflections, No refractions, Vertex colors
		BSSM_WATER_INTERIOR_NO_REFRACTIONS_Vc									= 708, // Water, Interior, No refractions, Vertex colors
		BSSM_WATER_INTERIOR_NO_REFLECTIONS_Vc									= 709, // Water, Interior, No reflections, Vertex colors
		BSSM_WATER_NODEPTH_Vc													= 710, // Water, No depth, Vertex colors
		BSSM_WATER_NODEPTH_NO_REFLECTIONS_Vc									= 711, // Water, No depth, No reflections, Vertex colors
		BSSM_WATER_NODEPTH_NO_REFRACTIONS_Vc									= 712, // Water, No depth, No refractions, Vertex colors
		BSSM_WATER_NODEPTH_NO_REFLECTIONS_NO_REFRACTIONS_Vc						= 713, // Water, No depth, No reflections, No refractions, Vertex colors
		BSSM_WATER_INTERIOR_NODEPTH_Vc											= 714, // Water, Interior, No depth, Vertex colors
		BSSM_WATER_INTERIOR_NODEPTH_NO_REFLECTIONS_NO_REFRACTIONS_Vc			= 715, // Water, Interior, No depth, No reflections, No refractions, Vertex colors
		BSSM_WATER_INTERIOR_NODEPTH_NO_REFRACTIONS_Vc							= 716, // Water, Interior, No depth, No refractions, Vertex colors
		BSSM_WATER_INTERIOR_NODEPTH_NO_REFLECTIONS_Vc							= 717, // Water, Interior, No depth, No reflections, Vertex colors
		BSSM_WATER_SPECULAR_LIGHTING_Vc											= 718, // Water, Specular lighting, Vertex colors
		BSSM_PRECIPITATION_RAIN													= 719, // Precipitation, rain
		BSSM_SKYBASEPOST														= 720, // Sky, Post-pass
		BSSM_SELFILLUM_SKY														= 721, // Self illumination, Sky
		BSSM_SKY_SUNGLARE														= 722, // Sky, Sun glare
		BSSM_SELFILLUM_SKY_SUN													= 723, // Self illumination, Sky, Sun
		BSSM_SELFILLUM_SKY_CLOUDS												= 724, // Self illumination, Sky, Clouds
		BSSM_SELFILLUM_SKY_SKY_QUAD												= 725, // Self illumination, Sky, Sky quad
		BSSM_SELFILLUMALPHA														= 726, // Self illumination, Alpha
		BSSM_SELFILLUMALPHA_S													= 727, // Self illumination, Alpha
		BSSM_WATER_DEPTH														= 728, // Water depth
		BSSM_WATER_DEPTH_S														= 729, // Water depth, Skinned
		BSSM_3XWATER_DEPTH														= 730, // 3.0 Water depth
		BSSM_3XWATER_DEPTH_S													= 731, // 3.0 Water depth, Skinned
		BSSM_NLWATER_DEPTH														= 732, // No lighting water depth
		BSSM_NLWATER_DEPTH_S													= 733, // No lighting water depth, Skinned
		BSSM_WATER_DEPTH_At														= 734, // Water depth, Alpha test
		BSSM_WATER_DEPTH_SAt													= 735, // Water depth, Skinned, Alpha test
		BSSM_3XWATER_DEPTH_At													= 736, // 3.0 Water depth, Alpha test
		BSSM_3XWATER_DEPTH_SAt													= 737, // 3.0 Water depth, Skinned, Alpha test
		BSSM_NLWATER_DEPTH_At													= 738, // No lighting water depth, Alpha test
		BSSM_NLWATER_DEPTH_At_S													= 739, // No lighting water depth, Alpha test, Skinned
		BSSM_WATER_DEPTH_Vc														= 740, // Water depth, Vertex colors
		BSSM_WATER_DEPTH_SVc													= 741, // Water depth, Skinned, Vertex colors
		BSSM_3XWATER_DEPTH_Vc													= 742, // 3.0 Water depth, Vertex colors
		BSSM_3XWATER_DEPTH_SVc													= 743, // 3.0 Water depth, Skinned, Vertex colors
		BSSM_NLWATER_DEPTH_Vc													= 744, // No lighting water depth, Vertex colors
		BSSM_NLWATER_DEPTH_Vc_S													= 745, // No lighting water depth, Vertex colors, Skinned
		BSSM_WATER_DEPTH_AtVc													= 746, // Water depth, Alpha test, Vertex colors
		BSSM_WATER_DEPTH_SAtVc													= 747, // Water depth, Skinned, Alpha test, Vertex colors
		BSSM_3XWATER_DEPTH_AtVc													= 748, // 3.0 Water depth, Alpha test, Vertex colors
		BSSM_3XWATER_DEPTH_SAtVc												= 749, // 3.0 Water depth, Skinned, Alpha test, Vertex colors
		BSSM_NLWATER_DEPTH_AtVc													= 750, // No lighting water depth, Alpha test, Vertex colors
		BSSM_NLWATER_DEPTH_AtVc_S												= 751, // No lighting water depth, Alpha test, Vertex colors, Skinned
		BSSM_BLOOD_SPLATTER_FLARE												= 752, // Blood splatter, lens flare
		BSSM_BLOOD_SPLATTER														= 753, // Blood splatter, main
		BSSM_SHADOWVOLUME_BACK													= 754, // Shadow volume, Back
		BSSM_SHADOWVOLUME_FRONT													= 755, // Shadow volume, Front
		BSSM_SHADOWVOLUME_WIRE													= 756, // Shadow volume, Wireframe
		BSSM_TILE																= 757, // Interface tile
		BSSM_IMAGESPACE															= 758, // ImageSpace
		BSSM_FINAL_PASS															= 759,
		BSSM_PASS_COUNT,

#if USE_PBR
		BSSM_PBR_STATIC		= 377,
		BSSM_PBR_SKINNED	= 378,
		BSSM_PBR_LAND		= 379,
		BSSM_PBR_LOD_LAND	= 380,
		BSSM_PBR_GRASS		= 381,
		BSSM_PBR_5			= 382,
#endif
	};

	enum LightingPasses : UInt32 {
		BSSM_PASS_NONE			= 0,
		BSSM_PASS_AMBIENT		= 1 << 0,
		BSSM_PASS_DIFFUSE		= 1 << 1,
		BSSM_PASS_TEXTURE		= 1 << 2,
		BSSM_PASS_SPECULAR		= 1 << 3,
		BSSM_PASS_SHADOW		= 1 << 4,
		BSSM_PASS_CANOPYSHADOW	= 1 << 5,
		BSSM_PASS_OPT			= 0x100,
		BSSM_PASS_ALL			= BSSM_PASS_AMBIENT | BSSM_PASS_DIFFUSE | BSSM_PASS_TEXTURE | BSSM_PASS_SPECULAR | BSSM_PASS_SHADOW | BSSM_PASS_CANOPYSHADOW | BSSM_PASS_OPT,
	};

	enum RenderMode : UInt32 {
		BSSM_RENDER_NORMAL			= 0,	// Renders everything as you'd expect
		BSSM_RENDER_UNK1			= 1,
		BSSM_RENDER_UNK2			= 2,
		BSSM_RENDER_UNK3			= 3,
		BSSM_RENDER_UNK4			= 4,
		BSSM_RENDER_UNK5			= 5,
		BSSM_RENDER_UNK6			= 6,
		BSSM_RENDER_DEPTH_MAP		= 7,
		BSSM_RENDER_CONST_ALPHA		= 8,	// Everything is green
		BSSM_RENDER_LOCAL_MAP		= 9,	// Renders vertex normals, skips landscape
		BSSM_RENDER_TILES			= 10,
		BSSM_RENDER_UNK11			= 11,
		BSSM_RENDER_NO_WATER_PASS	= 12,	// Skips water rendering
		BSSM_RENDER_UNK13			= 13,
		BSSM_RENDER_WATER_DEPTH		= 14,
		BSSM_RENDER_SILHOUETTE		= 15,	// Everything is black except for grass, sky and emissives
	};

	struct ShaderPackage {
		struct RawShader {
			char	cFileName[260];
			DWORD	shader;
		};

		const char*					pcPackagePath;
		DWORD						dword4;
		NiTStringMap<RawShader*>	kShaders;

		static ShaderPackage* GetSingleton() { return *(ShaderPackage**)0x11F9498; }
		RawShader* GetShader(const char* apcShaderName) {
			RawShader* pShader = nullptr;
			kShaders.GetAt(apcShaderName, pShader);
			return pShader;
		}
	};

	struct BSShaderMacro {
		const char* pcPath;
		D3DXMACRO	kMacros[9];
	};

	static UInt32 uiDebugMode;
	static bool bNoMips;
	static bool bHDRMult;

#ifdef _DEBUG
	static NiPoint3 kLastCameraPosition;
#endif

	static bool* const bIsHDR;
	static bool* const bIsLiteBrite;
	static bool* const bIsInInterior;
	static bool* const bImageSpaceEffects;
	static bool* const bUseRefractionShader;
	static bool* const bLODFadeInProgress;
	static bool* const bUseDepthBias;
	static bool* const bUseOptPasses;

	static float* const fDepthBias;
	static float* const fLODLandDrop;
	static float* const fLODFadeTimer;
	static float* const fTimers;

	static UInt32* const eShadowMode;
	static UInt32* const eGPUVendor;

	static Bitfield32* const uiEnabledPasses;
	static NiRect<float>* const kFramebufferViewport;
	static NiTexturePtr* const spCanopyShadowTexture;
	static NiTexturePtr* const spCanopyShadowMask;
	static NiPoint3* const kCullerCameraPos;
	static NiPoint3* const kCameraPos;
	static NiPoint4* const kLoadedRange;
	static NiCamera** const pCamera;

	static RenderMode* const eRenderMode;

	static BSRenderedTexture** pWaterRefractionTexture;

	static NiPointer<BSShader>* pspShaders;

#ifdef _DEBUG
	static bool bIgnoredPasses[BSSM_PASS_COUNT];
#endif

#if USE_PBR
	static bool bPBR;
#endif

	static void SetCamera(NiCamera* apCamera);

	static ShadowSceneNode* GetShadowSceneNode(const UInt32 aeType) {
		return reinterpret_cast<ShadowSceneNode**>(0x11F91C8)[aeType];
	};

	static BSShaderAccumulator* GetAccumulator();
	static void SetAccumulator(BSShaderAccumulator* apAccumulator);

	static BSShaderAccumulator* GetCurrentAccumulator();

	static NiDX9Renderer* GetRenderer();

	static BSShader* GetShader(ShaderType aiShaderIndex);

	static void SetRenderMode15ShaderAccumulator(BSShaderAccumulator* apAccumulator);

	static UInt32 GetShaderModel(bool abPixel);

	static bool CanUseRefractionShader();

	static bool IsShadowPass();

	static void GetTexture(const char* apFilename, bool abLoadFile, NiPointer<NiTexture>& aspTexture, bool abCheckMips = true, bool abCubeMap = false);

	static void SetShader(NiGeometry* apGeometry, UInt32 aeFadeType, bool abKeepUV = false, bool abNoPrecache = false);
	static void PrepareObjectRecurse(NiAVObject* apObject, UInt32 aeFadeType, bool abKeepUV = false, bool abNoPrecache = false);
	static void PrepareObject(NiAVObject* apObject, bool abKeepUV = false, bool abNoPrecache = false);

	static UInt32 GetFadeType(NiAVObject* apObject);

	static SceneGraphType GetFogIndex();

	static void SetFogIndex(SceneGraphType acIndex);

	static void SetRenderMode(RenderMode auiRenderMode) {
		CdeclCall(0xB4F450, auiRenderMode);
	}

	static double GetTimer(bool abDelta);

	static void SetTimer(bool abDelta, double afTime);

	static void ClearShadowAccumCount();
	static BSShaderAccumulator* GetNextShadowAccumulator();

	static bool __cdecl SetLightingPasses(void* paramInfo, void* scriptData, void* thisObj, void* containingObj, void* scriptObj, void* eventList, int, void* opcodeOffsetPtr);

	static void __cdecl SetExternalEmittanceRecurse(NiAVObject* apObject, NiColor* apExternalEmittance);
	static void* CreateTangentSpaceSimple(NiGeometry* apGeometry);
	static void ProcessQueuedLights();

	static bool ShouldShowCanopyShadowTexture();

	static const char* GetPassName(UInt32 uiRenderPass);

	static NiTexturePtr* const GetHideSecretMap()			{ return (NiTexturePtr*)0x11F9528; }
	static NiTexturePtr* const GetAnisoMap()				{ return (NiTexturePtr*)0x11F9530; }
	static NiTexturePtr* const GetNormalMap()				{ return (NiTexturePtr*)0x11F9510; }
	static NiTexturePtr* const GetNormalGlossMap()			{ return (NiTexturePtr*)0x11F9518; }
	static NiTexturePtr* const GetArmorReflectionCubeMap()	{ return (NiTexturePtr*)0x11F95D4; }
	static NiTexturePtr* const GetEyeReflectionCubeMap()	{ return (NiTexturePtr*)0x11F9544; }
	static NiTexturePtr* const GetErrorMap()				{ return (NiTexturePtr*)0x11F9514; }
	static NiTexturePtr* const GetHeightMap()				{ return (NiTexturePtr*)0x11F951C; }
	static NiTexturePtr* const GetTexEffectMap()			{ return (NiTexturePtr*)0x11F952C; }
	static NiTexturePtr* const GetLODNoiseMap()				{ return (NiTexturePtr*)0x11F953C; }
	static NiTexturePtr* const GetEmptyNoiseMap()			{ return (NiTexturePtr*)0x11F9540; }
	static NiTexturePtr* const GetAttenuationXYZ()			{ return (NiTexturePtr*)0x11F9534; }
	static NiTexturePtr* const GetAttenuationXYZ_Copy()		{ return (NiTexturePtr*)0x11F9538; }

	static bool bDisabledMips[RenderPassType::BSSM_PASS_COUNT];
};
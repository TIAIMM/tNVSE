#pragma once

#include "NiDX9Renderer.hpp"
#include "NiPoint3.hpp"
enum TextureFilterMode
{
	TEXTURE_FILTER_MODE_NEAREST		= 0,
	TEXTURE_FILTER_MODE_BILERP		= 1,
	TEXTURE_FILTER_MODE_TRILERP		= 2,
	TEXTURE_FILTER_MODE_ANISO		= 3,
	TEXTURE_FILTER_MODE_COMP_BILERP = 4,
	TEXTURE_FILTER_MODE_UNK_5		= 5,
	TEXTURE_FILTER_MODE_UNK_6		= 6,
	TEXTURE_FILTER_MODE_UNK_7		= 7,
	TEXTURE_FILTER_MODE_COUNT,
	TEXTURE_FILTER_MODE_DEFAULT		= 3,
};

enum TextureAddressMode
{
	TEXTURE_ADDRESS_MODE_CLAMP_S_CLAMP_T	= 0,
	TEXTURE_ADDRESS_MODE_CLAMP_S_WRAP_T		= 1,
	TEXTURE_ADDRESS_MODE_WRAP_S_CLAMP_T		= 2,
	TEXTURE_ADDRESS_MODE_WRAP_S_WRAP_T		= 3,
	TEXTURE_ADDRESS_MODE_COUNT				= 4,
	TEXTURE_ADDRESS_MODE_DEFAULT			= 0,
};

class BSGraphics {
public:
	class OcclusionQuery : public BSMemObject {
	public:
		OcclusionQuery();
		~OcclusionQuery();

		IDirect3DQuery9* pOcclusionQuery;

		bool Begin();
		void End();

		bool GetOcclusionQueryResults(void* apData, bool abFlush);
		bool GetData(void* apData, DWORD adwGetDataFlags);
	};

	static NiPoint3* GetCameraWorldTranslate() {
		return &(*(NiPoint3*)0x11F474C);
	}

	static bool* const bLetterBox;
	static bool* const bOcclusionQueryActive;

	static bool IsLetterBox(); // Need to verify in 1000%, but should be this

	static bool IsEyefinity() {
		return *(bool*)0x11C70ED && *(bool*)0x11C70EE;
	}
};
#pragma once

#include "BSMemObject.hpp"
#include "BSSimpleList.hpp"
#include "BSString.hpp"
#include "NiFixedString.hpp"
#include "NiTList.hpp"

class TileMenu;
class Tile;
class TileExtra;

enum MenuSpecialKeyboardInputCode
{
	kMenu_Enter = -2,
	kMenu_UpArrow = 0x1,
	kMenu_DownArrow = 0x2,
	kMenu_RightArrow = 0x3,
	kMenu_LeftArrow = 0x4,
	kMenu_Space = 0x9,
	kMenu_Tab = 0xA,
	kMenu_ShiftEnter = 0xB,
	kMenu_AltEnter = 0xC,
	kMenu_ShiftLeft = 0xD,
	kMenu_ShiftRight = 0xE,
	kMenu_PageUp = 0xF,
	kMenu_PageDown = 0x10,
};

class Menu : public BSMemObject {
public:
	virtual			~Menu();
	virtual void	AttachTileByID(UInt32 auiTileID, Tile* apTile);
	virtual void	HandleLeftClickPress(UInt32 auiTileID, Tile* apCctiveTile); // called when the mouse has moved and left click is pressed
	virtual void	HandleClick(SInt32 auiTileID, Tile* apClickedTile);
	virtual void	HandleMouseover(UInt32 auiTileID, Tile* apActiveTile);
	virtual void	HandleUnmouseover(UInt32 auiTileID, Tile* apTile);
	virtual void	PostDragTileChange(UInt32 auiTileID, Tile* apNewTile, Tile* apActiveTile);
	virtual void	PreDragTileChange(UInt32 auiTileID, Tile* apOldTile, Tile* apActiveTile);
	virtual void	HandleActiveMenuClickHeld(UInt32 auiTileID, Tile* apActiveTile);
	virtual void	OnClickHeld(UInt32 auiTileID, Tile* apActiveTile);
	virtual void	HandleMousewheel(UInt32 auiTileID, Tile* apTile);
	virtual void	DoIdle();
	virtual bool	HandleKeyboardInput(UInt32 auiInputChar);
	virtual UInt32	GetID();
	virtual bool	HandleSpecialKeyInput(MenuSpecialKeyboardInputCode aeCode, float afKeyState);
	virtual bool	HandleControllerInput(int aiInput, Tile* apTile);
	virtual void    OnUpdateUserTrait(int aitileVal);
	virtual void	HandleControllerConnectOrDisconnect(bool abIsControllerConnected);

	struct TemplateInstance {
		UInt32		unk00;
		float		flt04;
		BSString	strTileName;
		Tile*		pTile;
	};

	// 14
	struct TemplateData {
		NiFixedString					kTemplateName;
		TileExtra*						pTileExtra;
		NiTList<TemplateInstance*>		kInstances;
	};

	TileMenu*							pRootTile;
	BSSimpleList<Menu::TemplateData*>	kMenuTemplates;
	UInt32								unk10;
	UInt32								unk14;
	UInt32								unk18;
	UInt8								byte1C;
	UInt8								byte1D;
	UInt8								byte1E;
	UInt8								byte1F;
	UInt32								uiID;
	UInt32								uiVisibilityState;
};

ASSERT_SIZE(Menu, 0x28);
ASSERT_SIZE(Menu::TemplateData, 0x14);
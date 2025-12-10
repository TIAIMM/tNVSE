#pragma once

#include <string>

#include "BSSimpleArray.hpp"
#include "BSSimpleList.hpp"
#include "BSString.hpp"
#include "NiNode.hpp"
#include "NiTList.hpp"
#include <vector>

class Menu;
class NiColorA;
class TileShaderProperty;

class Tile : public BSMemObject {
public:
	static inline auto bs_rtti = RTTI_Tile;

	Tile();

	enum eTileValue {
		kTileValue_x = 0x0FA1,
		kTileValue_y,
		kTileValue_visible,
		kTileValue_class,
		kTileValue_clipwindow = 0x0FA6,
		kTileValue_stackingtype,
		kTileValue_locus,
		kTileValue_alpha,
		kTileValue_id,
		kTileValue_disablefade,
		kTileValue_listindex,
		kTileValue_depth,
		kTileValue_clips,
		kTileValue_target,
		kTileValue_height,
		kTileValue_width,
		kTileValue_red,
		kTileValue_green,
		kTileValue_blue,
		kTileValue_tile,
		kTileValue_childcount,
		kTileValue_child_count = kTileValue_childcount,
		kTileValue_justify,
		kTileValue_zoom,
		kTileValue_font,
		kTileValue_wrapwidth,
		kTileValue_wraplimit,
		kTileValue_wraplines,
		kTileValue_pagenum,
		kTileValue_ishtml,
		kTileValue_cropoffsety,
		kTileValue_cropy = kTileValue_cropoffsety,
		kTileValue_cropoffsetx,
		kTileValue_cropx = kTileValue_cropoffsetx,
		kTileValue_menufade,
		kTileValue_explorefade,
		kTileValue_mouseover,
		kTileValue_string,
		kTileValue_shiftclicked,
		kTileValue_clicked = 0x0FC7,
		kTileValue_clicksound = 0x0FCB,
		kTileValue_filename,
		kTileValue_filewidth,
		kTileValue_fileheight,
		kTileValue_repeatvertical,
		kTileValue_repeathorizontal,
		kTileValue_animation = 0x0FD2,
		kTileValue_linecount = 0x0DD4,
		kTileValue_pagecount,
		kTileValue_xdefault,
		kTileValue_xup,
		kTileValue_xdown,
		kTileValue_xleft,
		kTileValue_xright,
		kTileValue_xbuttona = 0x0FDD,
		kTileValue_xbuttonb,
		kTileValue_xbuttonx,
		kTileValue_xbuttony,
		kTileValue_xbuttonlt,
		kTileValue_xbuttonrt,
		kTileValue_xbuttonlb,
		kTileValue_xbuttonrb,
		kTileValue_xbuttonstart = 0x0FE7,
		kTileValue_mouseoversound,
		kTileValue_draggable,
		kTileValue_dragstartx,
		kTileValue_dragstarty,
		kTileValue_dragoffsetx,
		kTileValue_dragoffsety,
		kTileValue_dragdeltax,
		kTileValue_dragdeltay,
		kTileValue_dragx,
		kTileValue_dragy,
		kTileValue_wheelable,
		kTileValue_wheelmoved,
		kTileValue_systemcolor,
		kTileValue_brightness,
		kTileValue_linegap = 0x0FF7,
		kTileValue_resolutionconverter,
		kTileValue_texatlas,
		kTileValue_rotateangle,
		kTileValue_rotateaxisx,
		kTileValue_rotateaxisy,

		kTileValue_user0 = 0x01004,
		kTileValue_user1,
		kTileValue_user2,
		kTileValue_user3,
		kTileValue_user4,
		kTileValue_user5,
		kTileValue_user6,
		kTileValue_user7,
		kTileValue_user8,
		kTileValue_user9,
		kTileValue_user10,
		kTileValue_user11,
		kTileValue_user12,
		kTileValue_user13,
		kTileValue_user14,
		kTileValue_user15,
		kTileValue_user16,

		kTileValue_max
	};

	enum eTileID {
		kTileID_rect = 0x0385,
		kTileID_image,
		kTileID_text,
		kTileID_3D,
		kTileID_nif = kTileID_3D,
		kTileID_menu,

		// Not a Tile descendend
		kTileID_hotrect,
		kTileID_window,
		// This one descend from TileImage
		kTileID_radial,

		kTileID_max
	};

	virtual						~Tile();
	virtual void				Init(Tile* parent, const char* name, Tile* replacedChild);
	virtual NiNode*				MakeNode();
	virtual UInt32				GetType() const;
	virtual const char*			GetTypeName() const;
	virtual bool				Unk_05(UInt32 arg0, UInt32 arg1);
	virtual UInt32				PostParse(int, float, char const*);
	virtual void				Unk_07();
	virtual TileShaderProperty*	GetShaderProperty() const;
	virtual void				UpdateColor(NiNode* apNode, float afAlpha, NiColorA& arColor);

	struct Value;

	enum ActionType
	{
		kAction_copy = 0x7D0,
		kAction_add,
		kAction_sub,
		kAction_mul,
		kAction_div,
		kAction_min,
		kAction_max,
		kAction_mod,
		kAction_floor,
		kAction_ceil,
		kAction_abs,
		kAction_round,
		kAction_gt,
		kAction_gte,
		kAction_eq,
		kAction_neq,
		kAction_lt,
		kAction_lte,
		kAction_and,
		kAction_or,
		kAction_not,
		kAction_onlyif,
		kAction_onlyifnot,
		kAction_ref,
		kAction_begin,
		kAction_end,
	};

	// 0C
	class Action {
	public:
		Action();
		~Action();

		virtual float	GetFloat();
		virtual Value*	GetValue();

		UInt32	uiType;
		Action* pNext;
	};

	// 10
	class RefValueAction : public Action {
	public:
		RefValueAction();
		~RefValueAction();

		Value* pTileValue;
	};

	// 10
	class FloatAction : public Action {
	public:
		FloatAction();
		~FloatAction();

		float	fValue;
	};

	// 14
	struct Value {
		UInt32	uiID;	
		Tile*	pParent;
		float	fNum;	
		char*	pcText;	
		Action* pAction;
	};

	NiTList<Tile*>				kChildren;
	BSSimpleArray<Value*>		kValues;
	BSString					strName;
	Tile*						pParent;
	NiNodePtr					spNiNode;
	UInt32						uiFlags;
	UInt8						unk34;
	bool						bShouldPaintRed;

	void SetValue(UInt32 auiID, const char* apString, bool abPropagate);
	void SetValue(UInt32 auiID, float afValue, bool abPropagate);

	static UInt32 TraitNameToID(const char* apTraitName);
	Value* GetValue(UInt32 auiID);

	Tile* GetByTraitName(const std::string&& name) const {
		std::string realName = name;
		uint32_t index = 0;

		const auto separator = name.find(':');
		if (separator != std::string::npos) {
			realName = name.substr(0, separator);

			try {
				index = std::stoi(name.substr(separator + 1));
			}
			catch (std::exception& _ex) {
				(void)_ex;
				index = 0;
			}
		}

		for (const auto &child : kChildren) {
			if (name == "*" || !strcmp(realName.c_str(), child->strName.c_str())) {
				if (index == 0) {
					return child;
				}

				index--;
			}
		}

		return nullptr;
	}

	Tile* GetTileByPath(std::string&& path) const {
		const auto separator = path.find('/');
		if (separator == std::string::npos) {
			return GetByTraitName(path.c_str());
		}

		if (separator != path.size()) {
			const auto left = path.substr(0, separator);
			if (const auto tile = GetByTraitName(left.c_str())) {
				return tile->GetTileByPath(path.substr(separator + 1));
			}
		}

		return nullptr;
	}

	std::string GetName() const {
		const auto name = this->strName.c_str();
		auto index = 0;

		// Find offset in parent
		if (pParent) {
			for (const auto &sibling : pParent->kChildren) {
				if (sibling == this) {
					break;
				}

				if (sibling && !strcmp(sibling->strName.c_str(), name)) {
					index++;
				}
			}
		}

		return std::string(name) + ":" + std::to_string(index);
	}

	std::string GetFullPath() const {
		std::string fullPath{ GetName() };
		auto parent = this->pParent;
		while (parent) {
			const auto parentName = parent->GetName();
			if (parentName == "MenuRoot:0") {
				break;
			}

			fullPath = parentName + "/" + fullPath;
			parent = parent->pParent;
		}

		return fullPath;
	}

	std::vector<Tile*> GetChildren() {
		std::vector<Tile*> result{};
		for (const auto& child : this->kChildren) {
			result.push_back(child);
		}
		return result;
	}

	float GetValueFloat(uint32_t id) {
		if (const auto val = this->GetValue(id)) {
			return val->fNum;
		}

		return 0;
	}

	const char* GetValueString(uint32_t id) {
		if (const auto val = this->GetValue(id)) {
			if (val->pcText) {
				return val->pcText;
			}
		}

		return "";
	}

	auto SetValueFloat(const uint32_t id, const float val, const bool propogate) {
		this->SetValue(id, val, propogate);
	}

	auto SetValueString(const uint32_t id, const char* val, const bool propogate) {
		this->SetValue(id, val, propogate);
	}

	Menu* GetMenu() const {
		return ThisStdCall<Menu*>(0xA03C90, this);
	}

	Tile* TileFromTemplate(const char* pcTemplate) const {
		return ThisStdCall<Tile*>(0xA1DDB0, this->GetMenu(), this, pcTemplate, 0);
	}
};

ASSERT_SIZE(Tile, 0x38);
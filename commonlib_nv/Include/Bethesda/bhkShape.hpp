#pragma once

#include "bhkSerializable.hpp"
#include "NiColor.hpp"

class bhkShapeCollection;
class hkpShape;

enum HavokMaterialType {
	BHK_MATERIAL_STONE				= 0,
	BHK_MATERIAL_CLOTH				= 1,
	BHK_MATERIAL_DIRT				= 2,
	BHK_MATERIAL_GLASS				= 3,
	BHK_MATERIAL_GRASS				= 4,
	BHK_MATERIAL_METAL				= 5,
	BHK_MATERIAL_ORGANIC			= 6,
	BHK_MATERIAL_SKIN				= 7,
	BHK_MATERIAL_WATER				= 8,
	BHK_MATERIAL_WOOD				= 9,
	BHK_MATERIAL_HEAVYSTONE			= 10,
	BHK_MATERIAL_HEAVYMETAL			= 11,
	BHK_MATERIAL_HEAVYWOOD			= 12,
	BHK_MATERIAL_CHAIN				= 13,
	BHK_MATERIAL_SNOW				= 14,
	BHK_MATERIAL_ELEVATOR			= 15,
	BHK_MATERIAL_HOLLOWMETAL		= 16,
	BHK_MATERIAL_SHEETMETAL			= 17,
	BHK_MATERIAL_SAND				= 18,
	BHK_MATERIAL_BROKENCONCRETE		= 19,
	BHK_MATERIAL_VEHICLEBODY		= 20,
	BHK_MATERIAL_VEHICLEPARTSOLID	= 21,
	BHK_MATERIAL_VEHICLEPARTHOLLOW	= 22,
	BHK_MATERIAL_BARREL				= 23,
	BHK_MATERIAL_BOTTLE				= 24,
	BHK_MATERIAL_SODACAN			= 25,
	BHK_MATERIAL_PISTOL				= 26,
	BHK_MATERIAL_RIFLE				= 27,
	BHK_MATERIAL_SHOPPINGCART		= 28,
	BHK_MATERIAL_LUNCHBOX			= 29,
	BHK_MATERIAL_BABYRATTLE			= 30,
	BHK_MATERIAL_RUBBERBALL			= 31,
	BHK_MATERIAL_CHAINLINK			= 32,
	BHK_MATERIAL_TILE				= 33,
	BHK_MATERIAL_CARPET				= 34,
	BHK_MATERIAL_TUMBLEWEED			= 35,
	BHK_MATERIAL_MAX,
};

class bhkShape : public bhkSerializable {
public:
	virtual void CopyMembers(bhkSerializable* apDestination, NiCloningProcess* apCloneProc);							// 49
	virtual bool CalcMass(void* apMassProperty);																		// 50
	virtual bhkShapeCollection* FindShapeCollection(); 																	// 51
	virtual bool CanShare(NiCloningProcess* apCloneProc);																// 52
	virtual void BuildDisplayGeometry(NiNode* apNode, NiColor* apColor);												// 53
	virtual void DestroyDisplayGeometry();																				// 54
	virtual void BuildDisplayFromhkGeometry(NiNode* apNode, void* apGeometry, NiColor* apColor, const char* apName);	// 55

	HavokMaterialType eMaterialType;

	hkpShape* GethkObject() const;
};


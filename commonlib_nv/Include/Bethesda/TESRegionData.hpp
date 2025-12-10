#pragma once

#include "BSMemObject.hpp"
#include "BSSimpleList.hpp"

class TESRegion;

enum RegionDataID {
	REGION_DATA_NONE			= 0,
	REGION_DATA_GENERAL_ID		= 1,
	REGION_DATA_OBJECTS_ID		= 2,
	REGION_DATA_WEATHER_ID		= 3,
	REGION_DATA_MAP_ID			= 4,
	REGION_DATA_LANDSCAPE_ID	= 5,
	REGION_DATA_GRASS_ID		= 6,
	REGION_DATA_SOUND_ID		= 7,
	REGION_DATA_IMPOSTER		= 8,
	REGION_DATA_COUNT			= 9,
};


struct RegionData {
	RegionDataID	eDataTypeID;
	bool			bOverride;
	UInt8			cPriority;
};


class TESRegionData : public BSMemObject {
public:
	TESRegionData();
	virtual					~TESRegionData();
	virtual void			Save();
	virtual bool			LoadRegionData(RegionData* apData);
	virtual void			Initialize(TESRegion* apRegion);
	virtual RegionDataID	GetID() const;
	virtual TESRegionData*	Copy();
	virtual TESRegionData*	Blend(TESRegionData* apRegionData);
	virtual void			BlendInto(TESRegionData* apRegionData, UInt32 auiTotalBlending);
	virtual bool			Validate() const;

	bool	bOverride;
	bool	bIgnore;
	UInt8	cPriority;
};

class TESRegionDataList : public BSSimpleList<TESRegionData*> {
public:
	bool bOwnsDatamemory;

	TESRegionData* Find(RegionDataID aeID) const;
};

ASSERT_SIZE(TESRegionData, 0x8)
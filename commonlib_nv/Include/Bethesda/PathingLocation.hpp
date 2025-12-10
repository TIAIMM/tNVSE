#pragma once

#include "BSSimpleArray.hpp"
#include "NiPoint3.hpp"

class TESObjectCELL;
class TESWorldSpace;
class BGSSaveFormBuffer;
class NavMeshInfo;

class PathingLocation {
public:
	virtual void  Write(BGSSaveFormBuffer* apBuffer);
	virtual void  Read(BGSSaveFormBuffer* apBuffer);

	enum Flags {
		ALL_MESHES_REACHABLE = 1 << 0,
		UNUSED				 = 1 << 1,
	};

	NiPoint3						kLocation;
	NavMeshInfo*					pNavMeshInfo;
	BSSimpleArray<NavMeshInfo*>*	pNavMeshInfos;
	TESObjectCELL*					pCell;
	TESWorldSpace*					pWorldSpace;
	UInt32							uiCellCoords;
	UInt16							usTriangle;
	Bitfield8						ucFlags;
	UInt8							ucClientData;
};

ASSERT_SIZE(PathingLocation, 0x28);
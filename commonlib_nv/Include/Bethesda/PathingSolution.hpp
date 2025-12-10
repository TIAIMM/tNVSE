#pragma once

#include "NiRefObject.hpp"
#include "BSSimpleArray.hpp"
#include "PathingNode.hpp"
#include "VirtualPathingNode.hpp"

NiSmartPointer(PathingSolution);

class PathingSolution : public NiRefObject {
public:
	BSSimpleArray<VirtualPathingNode>	kVirtualPathingNodes;
	SInt32								iFirstLoadedVirtualNodeIndex;
	SInt32								iLastLoadedVirtualNodeIndex;
	BSSimpleArray<PathingNode>			kCurrentPathingNodes;
	BSSimpleArray<UInt32>				kPreviousPathingNodes;
	BYTE								byte40;
};

ASSERT_SIZE(PathingSolution, 0x44);
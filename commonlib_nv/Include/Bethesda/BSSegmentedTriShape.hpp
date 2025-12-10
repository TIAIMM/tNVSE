#pragma once

#include "NiTriShape.hpp"

NiSmartPointer(BSSegmentedTriShape);

class BSSegmentedTriShape : public NiTriShape {
public:
	BSSegmentedTriShape();
	~BSSegmentedTriShape();

	struct Segment {
		UInt32	uiStartIndex;
		UInt32	uiNumPrimitives;
		bool	bIsEnabled;
		UInt32	uiTriCount;
		bool	bVisible;
	};

	Segment*	pSegments;
	UInt32		uiNumSegments;
	bool		bSegmentsChanged;
	bool		bIgnoreSegments;

	CREATE_OBJECT(BSSegmentedTriShape, 0xA87140);

	void RenderImmediateEx(NiDX9Renderer* apRenderer);
	void RenderImmediateAltEx(NiDX9Renderer* apRenderer);

	void EnableSegment(UInt32 uiLevel);
	void DisableSegment(UInt32 uiLevel);
	bool IsSegmentEmpty(UInt32 uiLevel) const;
	UInt32 GetNumSegments() const;
	void EnableAllSegments();
	void UpdateDrawData();
};

ASSERT_SIZE(BSSegmentedTriShape, 0xD0);
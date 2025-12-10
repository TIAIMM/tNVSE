#pragma once

#include "NiObject.hpp"
#include "NiPoint3.hpp"
#include "NiColor.hpp"

class BSMultiBound;
class NiFrustumPlanes;

NiSmartPointer(BSMultiBoundShape);

class BSMultiBoundShape : public NiObject {
public:
	enum CullResult {
		BS_CULL_UNTESTED	= 0,
		BS_CULL_VISIBLE		= 1,
		BS_CULL_CULLED		= 2,
		BS_CULL_OCCLUDED	= 3,
	};

	enum ShapeType {
		BSMB_SHAPE_NONE		= 0,
		BSMB_SHAPE_AABB		= 1,
		BSMB_SHAPE_OBB		= 2,
		BSMB_SHAPE_SPHERE	= 3,
		BSMB_SHAPE_CAPSULE	= 4,
	};

	enum IntersectResult
	{
		BS_INTERSECT_NONE			= 0,
		BS_INTERSECT_PARTIAL		= 1,
		BS_INTERSECT_CONTAINSTARGET = 2
	};

	virtual ShapeType		GetType() const;
	virtual double			GetRadius() const;
	virtual IntersectResult	CheckBSBound(BSMultiBound& arTargetBound) const;
	virtual IntersectResult	CheckBound(NiBound& arTargetBound) const;
	virtual bool			WithinFrustum(NiFrustumPlanes& arPlanes) const;
	virtual bool			CompletelyWithinFrustum(NiFrustumPlanes& arPlanes) const;
	virtual void			GetNiBound(NiBound& arBound) const;
	virtual void			CreateDebugGeometry(NiLines* apLines, NiTriShape* apGeometry, NiColorA akColor);
	virtual UInt32			GetDebugGeomLineSize() const;
	virtual UInt32			GetDebugGeomShapeSize() const;
	virtual bool			GetPointWithin(NiPoint3& arPoint) const;
	virtual void			SetCenter(NiPoint3& arCenter);

	struct BoundVertices {
		NiPoint3 point[8];
	};

	CullResult eCullResult;

	inline void ResetCullResult() {
		eCullResult = BS_CULL_UNTESTED;
	};
};

ASSERT_SIZE(BSMultiBoundShape, 0xC)
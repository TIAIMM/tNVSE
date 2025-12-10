#pragma once

class hkpPhantom;

class hkpPhantomListener {
public:
	hkpPhantomListener();
	virtual		 ~hkpPhantomListener();
	virtual void phantomAddedCallback(hkpPhantom* phantom);
	virtual void phantomRemovedCallback(hkpPhantom* phantom);
	virtual void phantomShapeSetCallback(hkpPhantom* phantom);
	virtual void phantomDeletedCallback(hkpPhantom* phantom);
};

ASSERT_SIZE(hkpPhantomListener, 0x4);
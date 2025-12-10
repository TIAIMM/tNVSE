#pragma once

class TESGlobal;

class Calendar {
public:
	enum Season : SInt32 {
		NONE	= -1,
		SPRING	= 0,
		SUMMER	= 1,
		FALL	= 2,
		WINTER	= 3,
		COUNT	= 4,
	};


	TESGlobal*	pGameYear;
	TESGlobal*	pGameMonth;
	TESGlobal*	pGameDay;
	TESGlobal*	pGameHour;
	TESGlobal*	pGameDaysPassed;
	TESGlobal*	pTimeScale;
	UInt32		iMidnightsPassed;
	bool		bGameLoaded;
	UInt32		unk20;
	UInt32		unk24;
	UInt32		unk28;
	float		fLastUpdHour;
	UInt32		initialized;

	static Calendar* GetSingleton();

	UInt32 GetYear() const;
	UInt32 GetMonth() const;
	UInt32 GetDay() const;
	double GetHour() const;
	double GetMinutesPassed() const;
	UInt32 GetHoursPassed() const;
	double GetGameDaysPassed() const;
	Season GetSeason() const;
};
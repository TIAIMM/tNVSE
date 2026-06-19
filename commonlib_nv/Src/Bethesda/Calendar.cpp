#include "Calendar.hpp"
#include "TESGlobal.hpp"

Calendar* Calendar::GetSingleton() {
    return &*(Calendar*)0x11DE7B8;
}

// GAME - 0x867C60
UInt32 Calendar::GetYear() const {
	if (pGameYear)
		return pGameYear->fData;
	return 77;
}

// GAME - 0x867D20
UInt32 Calendar::GetMonth() const {
	if (pGameMonth)
		return pGameMonth->iData;
	return 7;
}

// GAME - 0x867D60
UInt32 Calendar::GetDay() const {
	if (pGameDay)
		return pGameDay->fData;
	return 17;
}

// GAME - 0x867DA0
double Calendar::GetHour() const {
	if (pGameHour)
		return pGameHour->fData;
	return 12.0;
}

// GAME - 0x867EA0
double Calendar::GetMinutesPassed() const {
	return GetGameDaysPassed() * 24 * 60;
}

// GAME - 0x867E30
UInt32 Calendar::GetHoursPassed() const {
	return GetGameDaysPassed() * 24;
}

// GAME - 0x867DE0
double Calendar::GetGameDaysPassed() const {
	if (pGameDaysPassed)
		return pGameDaysPassed->fData;
	return 1;
}

// GAME - 0x867CA0
Calendar::Season Calendar::GetSeason() const {
    Season eEason = SPRING;
    switch (GetMonth()) {
    case 0:
    case 1:
    case 11:
        eEason = WINTER;
        break;
    case 2:
    case 3:
    case 4:
        eEason = SPRING;
        break;
    case 5:
    case 6:
    case 7:
        eEason = SUMMER;
        break;
    case 8:
    case 9:
    case 10:
        eEason = FALL;
        break;
    default:
        return eEason;
    }
    return eEason;
}

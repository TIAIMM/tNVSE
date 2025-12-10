#include "BSPackedTask.hpp"

BSPackedTask::BSPackedTask() {
	memset(Data, 0, sizeof(Data));
}

BSPackedTask::~BSPackedTask() {
}

const char* BSPackedTask::GetTaskName() const {
	switch (Data[0].iData) {
	case TASK_EXAMPLE_PRINT_FLOAT:
		return "Print FLOAT";
	case TASK_EXAMPLE_PRINT_INT:
		return "Print INT";
	case TASK_SKY_EMITTANCE_UPDATE:
		return "Emittance Update";
	case TASK_UNK_3:
		return "UNK 3";
	case TASK_CELL_TRANSFORMS_UPDATE:
		return "Cell Transforms Update";
	case TASK_UNK_5:
		return "UNK 5";
	case TASK_GEOMORPH_UPDATE:
		return "GeoMorph Update";
	case TASK_ATTACH_NODE:
		return "Attach Node";
	case TASK_DETACH_NODE:
		return "Detach Node";
	case TASK_CLEAR_LIGHTS:
		return "Clear Lights";
	case TASK_UPDATE_3D:
		return "Update 3D";
	case TASK_ACTOR_CREATEBLOOD:
		return "Create Blood";
	case TASK_DAMAGE_OBJECT:
		return "Damage Object";
	case TASK_UPDATE_DESTRUCTIBLEOBJECT:
		return "Update Destructible Object";
	case TASK_KILL_ACTOR:
		return "Kill Actor";
	case TASK_ACTOR_MELEEATTACK:
		return "Actor Melee Attack";
	case TASK_ACTOR_PUSHAWAY:
		return "Push Away";
	case TASK_ACTOR_HITME:
		return "Actor Hit me";
	case TASK_MHE_INSTANTIATE_HIT_EFFECTS:
		return "Instantiate Hit Effects";
	case TASK_3D_HAVOKADD:
		return "Havok Add";
	case TASK_3D_HAVOKREMOVE:
		return "Havok Remove";
	case TASK_ATTACH_WEAPON:
		return "ATTACH WEAPON";
	case TASK_REMOVE_WEAPON:
		return "REMOVE WEAPON";
	case TASK_SET_3D_NULL:
		return "Set empty 3D";
	case TASK_ACTOR_DISMEMBER:
		return "TASK ACTOR DISMEMBER";
	case TASK_WATER_25:
		return "Water 25";
	case TASK_WATER_26:
		return "Water 26";
	case TASK_ADD_OBSTACLE_DOOR_OPEN:
		return "Add Obstacle on Door Open";
	case TASK_ADD_OBSTACLE_DOOR_CLOSE:
		return "Add Obstacle on Door Close";
	case TASK_ACTOR_UPDATE_APPEARANCE:
		return "TASK ACTOR UPDATE APPEARANCE";
	case TASK_TOGGLE_DOOR:
		return "Toggle Door";
	case TASK_MOVE_TO_MARKER:
		return "Move to Marker";
	case TASK_HANDLE_FIRED_WEAPON:
		return "TASK HANDLE FIRED WEAPON";
	case TASK_ACTOR_SETPOSITION:
		return "Actor: Set Position";
	case TASK_HAVOK_SET_MOTION:
		return "Havok: Set Motion";
	case TASK_UNK_35:
		return "UNK 35";
	case TASK_WATER_SPLASH_EFFECTS:
		return "Water: Splash Effects";
	case TASK_WATER_ADD_RIPPLE:
		return "Water: Add Ripple";
	case TASK_ACTOR_TRIGGERPAIN:
		return "Actor: Trigger Pain";
	case TASK_ACTOR_APPLYCRITSTAGE:
		return "Actor: Apply Critical Stage";
	case TASK_UNK_40:
		return "UNK 40";
	case TASK_UNK_41:
		return "UNK 41";
	case TASK_UNLOAD_3D:
		return "Unload 3D";
	case TASK_UNK_43:
		return "UNK 43";
	case TASK_HAVOK_UPDATEPOS:
		return "Havok: Update Position";
	case TASK_HAVOKINIT:
		return "Havok: Init";
	case TASK_DESTROY_EFFECT:
		return "Destroy effect";
	case TASK_REMOVE_NODE:
		return "Remove Node";
	case TASK_REMOVE_PICKEDUP_OBJECT:
		return "Remove Picked Up Object";
	case TASK_UNK_49:
		return "UNK 49";
	default:
		return "UNKNOWN TASK";
	}
}

void BSPackedTask::SetTaskThreadName(HANDLE apThread) const {
	wchar_t wideName[1024];
	const char* pName = GetTaskName();
	MultiByteToWideChar(CP_UTF8, 0, pName, -1, wideName, 1024);

	// Append "[FNV]" to the beginning of the thread name
	wchar_t newWideName[1024];
	wcsncpy_s(newWideName, L"[FNV] Task Thread - ", _TRUNCATE);
	wcsncat_s(newWideName, wideName, 1024 - wcslen(newWideName) - 1);
	SetThreadDescription(apThread, (PCWSTR)newWideName);
}
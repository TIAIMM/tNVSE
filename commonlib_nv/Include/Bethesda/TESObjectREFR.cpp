#include "TESObjectREFR.hpp"
#include "MobileObject.hpp"
#include "bhkCharacterController.hpp"
#include "BSPackedTask.hpp"
#include "TaskQueueInterface.hpp"

void TESObjectREFR::SetPos(const NiPoint3& pos, bool update3d) {
	SetLocationOnReference(&pos);

	if (const auto mobileObject = DYNAMIC_CAST<TESObjectREFR, MobileObject>(this)) {
		if (const auto controller = mobileObject->GetCharacterController()) {
			if (!controller->IsFlying()) {
				controller->SetPosition(&pos);
			}
		}
	}

	if (!update3d) {
		return;
	}

	if (const auto nif = this->Get3D()) {
		nif->SetLocalTranslate(pos);
		bhkNiCollisionObject::ResetSim(nif, true);

		NiUpdateData update {};
		update.bUpdateControllers = update.bIsMultiThreaded = false;
		nif->Update(update);
	}
}

void TESObjectREFR::SetRotation(const NiPoint3& rotation, bool update3d) {
	this->kRotation = rotation;
	this->AddChange(0x2);

	if (!update3d) {
		return;
	}

	if (NiNode* node = this->Get3D()) {
		node->SetLocalRotate(rotation.x, rotation.y, rotation.z);
		bhkNiCollisionObject::ResetSim(node, true);
		if (!GetAnimData()) {
			NiUpdateData update(0, false, false, false, false, false);
			node->Update(update);
		}
	}
}

void TESObjectREFR::SetScale(const float scale) {
	if (const auto taskInterface = TaskQueueInterface::GetSingleton()) {
		BSPackedTask task{};
		task.Data[0].iData = 31;
		task.Data[1].iData = 4412;
		task.Data[2].iData = uiFormID;
		task.Data[3].fData = scale;
		taskInterface->QueueIfActive(task);
	}
}
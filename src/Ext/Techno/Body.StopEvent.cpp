#include "Body.h"

void TechnoExt::SendStopPassengersTar(TechnoClass* pThis)
{
	auto pFoot = abstract_cast<FootClass*>(pThis);

	EventExt event;
	event.Type = EventTypeExt::SyncPassengersTar;
	event.HouseIndex = (char)HouseClass::CurrentPlayer->ArrayIndex;
	event.Frame = Unsorted::CurrentFrame;

	event.AddEvent();
}

void TechnoExt::HandleStopPassengersTar(EventExt* event)
{
	int technoUniqueID = event->SyncPassengersTar.TechnoUniqueID;

	for (auto pTechno : *TechnoClass::Array)
	{
		if (pTechno && pTechno->UniqueID == technoUniqueID)
		{
			auto pExtType = TechnoTypeExt::ExtMap.Find(pTechno->GetTechnoType());

			if (pExtType->OpenTopped_TransferPassengerStopCommand)
			{
				for (auto pNext = pTechno->Passengers.FirstPassenger; pNext; pNext = abstract_cast<FootClass*>(pNext->NextObject))
				{
					pNext->SetTarget(nullptr);
					pNext->SpawnManager->ResetTarget();
				}
			}

			break;
		}
	}
}

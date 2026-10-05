#include "Body.h"

#include <SpawnManagerClass.h>

void TechnoExt::SendStopPassengersTar(TechnoClass* pThis)
{
	if (!pThis)
		return;

	EventExt event {};
	event.Type = EventTypeExt::SyncPassengersTar;
	event.HouseIndex = (char)HouseClass::CurrentPlayer->ArrayIndex;
	event.Frame = Unsorted::CurrentFrame;
	event.SyncPassengersTar.TechnoUniqueID = pThis->UniqueID;

	event.AddEvent();
}

void TechnoExt::HandleStopPassengersTar(EventExt* event)
{
	DWORD technoUniqueID = event->SyncPassengersTar.TechnoUniqueID;

	for (auto pTechno : TechnoClass::Array)
	{
		if (pTechno && pTechno->UniqueID == technoUniqueID)
		{
			auto pExtType = TechnoTypeExt::Fetch(pTechno->GetTechnoType());

			if (pExtType->OpenTopped_TransferPassengerStopCommand)
			{
				for (FootClass* pNext = pTechno->Passengers.FirstPassenger; pNext; pNext = abstract_cast<FootClass*>(pNext->NextObject))
				{
					pNext->SetTarget(nullptr);
					if (pNext->SpawnManager)
						pNext->SpawnManager->ResetTarget();
				}
			}

			break;
		}
	}
}

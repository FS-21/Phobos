#include "AircraftPut.h"

#include <Ext/Techno/Body.h>
#include <Ext/TechnoType/Body.h>

void TechnoExt::ApplyAircraftPutData(TechnoClass* pThis)
{
	if (!pThis)
		return;

	if (auto pFoot = abstract_cast<FootClass*>(pThis))
	{
		if (pFoot->WhatAmI() == AbstractType::Aircraft)
		{

			auto pExt = TechnoExt::ExtMap.Find(pThis);
			auto pType = pThis->Type();
			auto pTypeExt = TechnoTypeExt::ExtMap.Find(pType);

			if (pExt->aircraftPutOffsetFlag && pTypeExt->AirPutData)
			{
				pExt->aircraftPutOffsetFlag = false;
				auto loc = pFoot->GetCoords();
				CoordStruct pos = loc + pTypeExt->AirPutData.PutOffset.Get({ 0,0,12 });
				pFoot->SetLocation(pos);
				pFoot->vt_entry_548();
			}
		}
	}
}

namespace Count
{
	int AircraftCount(HouseClass* pHouse, TypeList<AircraftTypeClass*> &nList)
	{
		int count = 0;

		for (auto TechArr : *TechnoClass::Array)
		{
			if (TechArr)
			{
				auto pAircraft = specific_cast<AircraftClass*>(TechArr);
				
				if (pAircraft
					&& TechArr->Owner == pHouse
					&& TechArr->IsAlive
					&& TechArr->IsActive()
					)
				{
					if (nList.FindItemIndex(pAircraft->Type) != -1)
					{
						count++;
					}
				}
			}
		}

		return count;
	}

	bool Contains(TypeList<AircraftTypeClass*> &nList, AircraftTypeClass* &pAir)
	{
		return nList.FindItemIndex(pAir) != -1;
	}
}

void TechnoExt::ApplyAircraftPutData(TechnoClass* pThis, CoordStruct* pCoord)
{
	if (!pThis)
		return;

	auto pAircraft = specific_cast<AircraftClass*>(pThis);

	if (pAircraft
		&& !pAircraft->Spawned
		&& RulesClass::Instance->PadAircraft.Count > 0 //.empty()
		&& pAircraft->Owner
		)
	{
		auto pType = pAircraft->Type;
		auto pTypeExt = TechnoTypeExt::ExtMap.Find(pType);
		if (pTypeExt->AirPutData)
		{
			if (Count::Contains(RulesClass::Instance->PadAircraft, pType))
			{
				if (pAircraft->Type->AirportBound && pTypeExt->AirPutData.RemoveIfNoDock.Get())
				{
					if (pAircraft->Owner->AirportDocks <= 0 || pAircraft->Owner->AirportDocks < Count::AircraftCount(pAircraft->Owner, RulesClass::Instance->PadAircraft))
					{
						pAircraft->Owner->GiveMoney(abs(pThis->Type()->Cost));
						pAircraft->Limbo();
						pAircraft->UnInit();

						return;
					}
				}
			}

			auto pExt = TechnoExt::ExtMap.Find(pThis);
			pExt->aircraftPutOffsetFlag = true;
			if (!pTypeExt->AirPutData.Force.Get())
			{
				if (auto pCell = MapClass::Instance->TryGetCellAt(*pCoord))
				{
					if (auto pBuilding = pCell->GetBuilding())
					{
						if (pBuilding->Type->Helipad)
						{
							pExt->aircraftPutOffsetFlag = false;
						}
					}

				}
			}
			*pCoord += pTypeExt->AirPutData.PutOffset.Get();
		}
	}
}

DEFINE_HOOK(0x6F6CA0, TechnoClass_Put_AirPut, 0x7)
{
	GET(TechnoClass* const, pThis, ECX);
	GET_STACK(CoordStruct*, pCoord, 0x4);
	//	GET_STACK(unsigned int, Direction, 0x8);

	TechnoExt::ApplyAircraftPutData(pThis, pCoord);

	return 0;
}

DEFINE_HOOK(0x6F9E50, TechnoClass_AI_AirPut, 0x5)
{
	GET(TechnoClass*, pThis, ECX);

	TechnoExt::ApplyAircraftPutData(pThis);

	return 0;
}

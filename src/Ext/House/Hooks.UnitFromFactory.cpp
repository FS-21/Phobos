#include <FactoryClass.h>
#include <BuildingClass.h>

#include <Ext/Techno/Body.h>
#include <Ext/Building/Body.h>
#include <Ext/House/Body.h>

DEFINE_HOOK(0x4FB63A, HouseClass_UnitFromFactory_DisablingEVAUnitReady, 0xF)
{
	if (!RulesExt::Global()->IsVoiceCreatedGlobal.Get())
		VoxClass::Play(GameStrings::EVA_UnitReady);

	return 0x4FB649;
}

DEFINE_HOOK(0x4FB64B, HouseClass_UnitFromFactory_VoiceCreated, 0x5)
{
	GET(TechnoClass* const, pThisTechno, ESI);
	GET(FactoryClass* const, pThisFactory, EBX);

	if (pThisTechno && pThisTechno->Owner && pThisFactory)
	{
		BuildingClass* pFactoryBld = nullptr;

		for (auto pBld : pThisTechno->Owner->Buildings)
		{
			if (pBld && pBld->Factory == pThisFactory)
			{
				pFactoryBld = pBld;
				break;
			}
		}

		if (!pFactoryBld)
		{
			AbstractType abs = pThisTechno->WhatAmI();
			AbstractType factType = AbstractType::None;

			if (abs == AbstractType::Infantry)
				factType = AbstractType::InfantryType;
			else if (abs == AbstractType::Unit)
				factType = AbstractType::UnitType;
			else if (abs == AbstractType::Aircraft)
				factType = AbstractType::AircraftType;

			if (factType != AbstractType::None)
			{
				bool isNaval = (abs == AbstractType::Unit) && specific_cast<UnitClass*>(pThisTechno)->Type->Naval;

				for (auto pBld : pThisTechno->Owner->Buildings)
				{
					if (pBld && pBld->IsAlive && !pBld->InLimbo && pBld->Type)
					{
						bool match = (pBld->Type->Factory == factType);
						if (abs == AbstractType::Unit)
							match = match && (pBld->Type->Naval == isNaval);
						else if (abs == AbstractType::Aircraft)
							match = match || pBld->Type->Helipad;

						if (match && (pBld->IsPrimaryFactory || !pFactoryBld))
							pFactoryBld = pBld;
					}
				}
			}
		}

		if (pFactoryBld)
		{
			auto const pBldExt = BuildingExt::Fetch(pFactoryBld);

			pBldExt->LastProducedTechno = pThisTechno;
			pBldExt->LastProducedType = pThisTechno->GetTechnoType();
		}
	}

	auto const pThisTechnoType = TechnoExt::Fetch(pThisTechno)->TypeExtData;

	if (pThisTechno->Owner->IsControlledByCurrentPlayer() && pThisTechnoType->VoiceCreated.isset())
	{
		if (RulesExt::Global()->IsVoiceCreatedGlobal.Get())
			pThisTechno->QueueVoice(pThisTechnoType->VoiceCreated);
		else
			VocClass::PlayAt(pThisTechnoType->VoiceCreated, pThisTechno->Location);
	}

	pThisFactory->CompletedProduction();

	return 0x4FB650;
}

DEFINE_HOOK(0x4FB6B0, HouseClass_JustBuilt_TrackLastProduced, 0x5)
{
	GET(HouseClass* const, pThis, ECX);
	GET_STACK(TechnoClass*, pTechno, 0x4);

	if (pThis && pTechno)
	{
		auto const pHouseExt = HouseExt::Fetch(pThis);
		AbstractType absType = pTechno->WhatAmI();
		TechnoTypeClass* pType = pTechno->GetTechnoType();

		if (absType == AbstractType::Building)
		{
			auto pBld = static_cast<BuildingClass*>(pTechno);

			if (pBld->Type && pBld->Type->BuildCat == BuildCat::Combat)
			{
				pHouseExt->LastProducedDefense = pTechno;
				pHouseExt->LastProducedDefenseType = pType;
			}
			else
			{
				pHouseExt->LastProducedBuilding = pTechno;
				pHouseExt->LastProducedBuildingType = pType;
			}

			// Also associate with the ConYard / Construction Yard
			BuildingClass* pConYard = nullptr;

			if (pThis->Primary_ForBuildings && pThis->Primary_ForBuildings->Owner == pThis)
			{
				for (auto pB : pThis->Buildings)
				{
					if (pB && pB->Factory == pThis->Primary_ForBuildings)
					{
						pConYard = pB;
						break;
					}
				}
			}

			if (!pConYard)
				pConYard = pHouseExt->Factory_BuildingType;

			if (!pConYard)
			{
				for (auto pB : pThis->Buildings)
				{
					if (pB && pB->Type && pB->Type->Factory == AbstractType::BuildingType && pB->IsAlive && !pB->InLimbo)
					{
						if (pB->IsPrimaryFactory || !pConYard)
							pConYard = pB;
					}
				}
			}

			if (pConYard)
			{
				auto const pConYardExt = BuildingExt::Fetch(pConYard);

				pConYardExt->LastProducedTechno = pBld;
				pConYardExt->LastProducedType = pBld->Type;
			}
		}
		else if (absType == AbstractType::Unit)
		{
			pHouseExt->LastProducedUnit = pTechno;
			pHouseExt->LastProducedUnitType = pType;
		}
		else if (absType == AbstractType::Infantry)
		{
			pHouseExt->LastProducedInfantry = pTechno;
			pHouseExt->LastProducedInfantryType = pType;
		}
		else if (absType == AbstractType::Aircraft)
		{
			pHouseExt->LastProducedAircraft = pTechno;
			pHouseExt->LastProducedAircraftType = pType;
		}
	}

	return 0;
}

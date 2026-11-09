#include "Body.h"

#include <Ext/House/Body.h>

#include <AircraftClass.h>
#include <FlyLocomotionClass.h>
#include <MapClass.h>
#include <Unsorted.h>

DEFINE_HOOK(0x4401BB, BuildingClass_AI_PickWithFreeDocks, 0x6)
{
	enum { SkipGameCode = 0x4401D2 };

	GET(BuildingClass*, pBuilding, ESI);

	if (pBuilding->IsUnderEMP())
		return SkipGameCode;

	auto const pOwner = pBuilding->Owner;
	const int index = pOwner->ProducingAircraftTypeIndex;
	auto const pType = index >= 0 ? AircraftTypeClass::Array.GetItem(index) : nullptr;

	// An aircraft that needs no dock comes out of any air base, full or not.
	if (pType && !pType->AirportBound)
		return 0;

	if (RulesExt::Global()->AllowParallelAIQueues && !RulesExt::Global()->ForbidParallelAIQueues_Aircraft && (!pType || !TechnoTypeExt::Fetch(pType)->ForbidParallelAIQueues))
		return 0;

	if (pOwner->IsCurrentPlayer() || pOwner->IsNeutral())
		return 0;

	if (pBuilding->Type->Factory == AbstractType::AircraftType)
	{
		if (pBuilding->Factory
			&& !BuildingExt::HasFreeDocks(pBuilding))
		{
			if (auto const pBldExt = BuildingExt::TryFetch(pBuilding))
				pBldExt->UpdatePrimaryFactoryAI();
		}
	}

	return 0;
}

DEFINE_HOOK(0x4502F4, BuildingClass_Update_Factory_Phobos, 0x6)
{
	GET(BuildingClass*, pThis, ESI);
	const HouseClass* pOwner = pThis->Owner;

	if (pOwner->Production && RulesExt::Global()->AllowParallelAIQueues)
	{
		auto const pOwnerExt = HouseExt::Fetch(pOwner);
		auto const pFactory = pThis->Type->Factory;
		const bool naval = pThis->Type->Naval;
		BuildingClass** currFactory = nullptr;

		switch (pFactory)
		{
		case AbstractType::BuildingType:
			currFactory = &pOwnerExt->Factory_BuildingType;
			break;
		case AbstractType::UnitType:
			currFactory = naval ? &pOwnerExt->Factory_NavyType : &pOwnerExt->Factory_VehicleType;
			break;
		case AbstractType::InfantryType:
			currFactory = &pOwnerExt->Factory_InfantryType;
			break;
		case AbstractType::AircraftType:
			currFactory = &pOwnerExt->Factory_AircraftType;
			break;
		default:
			break;
		}

		if (!*currFactory)
		{
			*currFactory = pThis;
			return 0;
		}
		else if (*currFactory != pThis)
		{
			enum { Skip = 0x4503CA };


			TechnoTypeClass* pType = nullptr;
			int index = -1;

			switch (pFactory)
			{
			case AbstractType::BuildingType:
				if (RulesExt::Global()->ForbidParallelAIQueues_Building)
					return Skip;

				index = pOwner->ProducingBuildingTypeIndex;
				pType = index >= 0 ? BuildingTypeClass::Array.GetItem(index) : nullptr;
				break;
			case AbstractType::InfantryType:
				if (RulesExt::Global()->ForbidParallelAIQueues_Infantry)
					return Skip;

				index = pOwner->ProducingInfantryTypeIndex;
				pType = index >= 0 ? InfantryTypeClass::Array.GetItem(index) : nullptr;
				break;
			case AbstractType::AircraftType:
				if (RulesExt::Global()->ForbidParallelAIQueues_Aircraft)
					return Skip;

				index = pOwner->ProducingAircraftTypeIndex;
				pType = index >= 0 ? AircraftTypeClass::Array.GetItem(index) : nullptr;
				break;
			case AbstractType::UnitType:
				if (naval ? RulesExt::Global()->ForbidParallelAIQueues_Navy : RulesExt::Global()->ForbidParallelAIQueues_Vehicle)
					return Skip;

				index = naval ? HouseExt::Fetch(pOwner)->ProducingNavalUnitTypeIndex : pOwner->ProducingUnitTypeIndex;
				pType = index >= 0 ? UnitTypeClass::Array.GetItem(index) : nullptr;
				break;
			default:
				break;
			}

			if (pType && TechnoTypeExt::Fetch(pType)->ForbidParallelAIQueues)
				return Skip;
		}
	}

	return 0;
}

//const byte old_empty_log[] = { 0xC3 };
DEFINE_JUMP(CALL, 0x4CA016, 0x4CA19F); // randomly chosen 0xC3

DEFINE_HOOK(0x4CA07A, FactoryClass_AbandonProduction_Phobos, 0x8)
{
	GET(FactoryClass*, pFactory, ESI);
	GET_STACK(DWORD const, calledby, 0x18);

	auto const pTechno = pFactory->Object;

	if (calledby < 0x7F0000) // Replace the old log with this to figure out where keeps flushing the stream
	{
		Debug::LogGame("(%08x) : %s is abandoning production of %s[%s]\n"
			, calledby - 5
			, pFactory->Owner->PlainName
			, pTechno->GetType()->Name
			, pTechno->get_ID());
	}

	if (!RulesExt::Global()->AllowParallelAIQueues)
		return 0;

	auto const pOwnerExt = HouseExt::Fetch(pFactory->Owner);
	auto const pTypeExt = TechnoExt::Fetch(pTechno)->TypeExtData;
	auto const pType = pTypeExt->OwnerObject();
	const bool forbid = pTypeExt->ForbidParallelAIQueues;

	switch (pTechno->WhatAmI())
	{
	case AbstractType::Building:
		if (RulesExt::Global()->ForbidParallelAIQueues_Building || forbid)
			pOwnerExt->Factory_BuildingType = nullptr;
		break;
	case AbstractType::Unit:
		if (!pType->Naval)
		{
			if (RulesExt::Global()->ForbidParallelAIQueues_Vehicle || forbid)
				pOwnerExt->Factory_VehicleType = nullptr;
		}
		else
		{
			if (RulesExt::Global()->ForbidParallelAIQueues_Navy || forbid)
				pOwnerExt->Factory_NavyType = nullptr;
		}
		break;
	case AbstractType::Infantry:
		if (RulesExt::Global()->ForbidParallelAIQueues_Infantry || forbid)
			pOwnerExt->Factory_InfantryType = nullptr;
		break;
	case AbstractType::Aircraft:
		if (RulesExt::Global()->ForbidParallelAIQueues_Aircraft || forbid)
			pOwnerExt->Factory_AircraftType = nullptr;
		break;
	default:
		break;
	}

	return 0;
}

DEFINE_HOOK(0x444119, BuildingClass_KickOutUnit_UnitType_Phobos, 0x6)
{
	GET(UnitClass*, pUnit, EDI);
	GET(BuildingClass*, pFactory, ESI);

	auto const pHouseExt = HouseExt::Fetch(pFactory->Owner);

	if (pUnit->Type->Naval && pHouseExt->Factory_NavyType == pFactory)
		pHouseExt->Factory_NavyType = nullptr;
	else if (pHouseExt->Factory_VehicleType == pFactory)
		pHouseExt->Factory_VehicleType = nullptr;

	return 0;
}

DEFINE_HOOK(0x444131, BuildingClass_KickOutUnit_InfantryType_Phobos, 0x6)
{
	GET(BuildingClass*, pFactory, ESI);

	auto const pHouseExt = HouseExt::Fetch(pFactory->Owner);

	if (pHouseExt->Factory_InfantryType == pFactory)
		pHouseExt->Factory_InfantryType = nullptr;

	return 0;
}

DEFINE_HOOK(0x44531F, BuildingClass_KickOutUnit_BuildingType_Phobos, 0xA)
{
	GET(BuildingClass*, pFactory, ESI);

	auto const pHouseExt = HouseExt::Fetch(pFactory->Owner);

	if (pHouseExt->Factory_BuildingType == pFactory)
		pHouseExt->Factory_BuildingType = nullptr;

	return 0;
}

DEFINE_HOOK(0x443CCA, BuildingClass_KickOutUnit_AircraftType_Phobos, 0xA)
{
	GET(BuildingClass*, pFactory, ESI);

	auto const pHouseExt = HouseExt::Fetch(pFactory->Owner);

	if (pHouseExt->Factory_AircraftType == pFactory)
		pHouseExt->Factory_AircraftType = nullptr;

	return 0;
}

DEFINE_HOOK(0x4449FB, BuildingClass_KickOutUnit_CloningVats, 0x8)
{
	enum { SkipGameCode = 0x444A53 };

	GET(BuildingClass*, pFactory, ESI);
	GET(TechnoTypeClass*, pProductionType, EAX);
	const auto pOwner = pFactory->Owner;
	auto info = std::make_pair(pProductionType, pOwner);

	for (const auto pVat : pOwner->CloningVats)
		BuildingExt::KickOutClone(info, 0, pVat);

	return SkipGameCode;
}

// An aircraft that needs no dock (AirportBound=no) leaves its factory landed at the height of a
// docked aircraft (the factory's ground plus the Z of its first docking offset), then takes off to
// the rally point, or next to the factory, and lands there. It never takes a dock, so the docks stay
// free for the aircraft that must land on them.
bool BuildingExt::KickOutUndockedAircraft(BuildingClass* pFactory, AircraftClass* pAircraft)
{
	auto const pType = pAircraft->Type;
	const int typeFlightLevel = pType->GetFlightLevel();
	const int flightLevel = typeFlightLevel > 0 ? typeFlightLevel : RulesClass::Instance->FlightLevel;
	const int dockHeight = pFactory->Type->NumberOfDocks > 0 ? std::max(0, pFactory->Type->DockingOffsets[0].Z) : 0;

	CoordStruct coords = pFactory->GetCoords();
	coords.Z += dockHeight;

	const DirStruct facing = pFactory->PrimaryFacing.Current().Raw != 0 ? pFactory->PrimaryFacing.Current() : DirStruct(128);
	bool unlimboed = false;

	{
		++Unsorted::ScenarioInit;
		unlimboed = pAircraft->Unlimbo(coords, facing.GetDir());
		--Unsorted::ScenarioInit;
	}

	if (!unlimboed)
		return false;

	pAircraft->SetLocation(coords);
	pAircraft->InAir = false;
	pAircraft->IsALoaner = false;
	pAircraft->PrimaryFacing.SetCurrent(facing);
	pAircraft->PrimaryFacing.SetDesired(facing);
	pAircraft->SetHeight(dockHeight);

	if (auto const pFlyLoco = locomotion_cast<FlyLocomotionClass*>(pAircraft->Locomotor))
	{
		pFlyLoco->FlightLevel = flightLevel;
		pFlyLoco->CurrentSpeed = 0.0;
		pFlyLoco->TargetSpeed = 0.0;
		pFlyLoco->IsTakingOff = 0;
		pFlyLoco->IsLanding = false;
	}

	AbstractClass* pDestination = pFactory->ArchiveTarget;

	if (!pDestination)
	{
		CellStruct cell = CellStruct::Empty;
		pAircraft->NearbyLocation(&cell, pFactory);

		if (cell != CellStruct::Empty)
			pDestination = MapClass::Instance.TryGetCellAt(cell);
	}

	if (pDestination)
	{
		pAircraft->SetDestination(pDestination, true);
		pAircraft->QueueMission(Mission::Move, true);
	}

	return true;
}

// Aircraft that need no dock (AirportBound=no) never take one, even a free one: they leave landed
// over the factory and take off from there (KickOutUndockedAircraft). The original code docked
// them when a dock was free and otherwise brought them in from the map edge. During an ion storm
// they keep the original way, since aircraft cannot fly then.
DEFINE_HOOK(0x443CD9, BuildingClass_KickOutUnit_UndockedAircraft, 0x8)
{
	enum { ToDock = 0x443F54, NoFreeDock = 0x443CE1, Succeeded = 0x444971, Failed = 0x445696 };

	GET(BuildingClass*, pFactory, ESI);
	GET(AircraftClass*, pAircraft, EBP);

	const bool hasFreeDock = R->AL() != 0;

	if (!pAircraft || pAircraft->Type->AirportBound || (pFactory->Owner && pFactory->Owner->IonSensitivesShouldBeOffline()))
		return hasFreeDock ? ToDock : NoFreeDock;

	if (!BuildingExt::KickOutUndockedAircraft(pFactory, pAircraft))
		return Failed;

	// The common success exit lowers ScenarioInit once, as it does after the original ways out.
	++Unsorted::ScenarioInit;
	return Succeeded;
}

// Production start (0x4FA350): Ares' FindFactory takes an air base whose docks are all taken for no
// factory at all. A click on the cameo then started nothing, and an item started from the queue
// fell back to the occupied air base and began suspended as if the player had paused it, which a
// new click cannot undo, so the production stayed stuck. An aircraft that needs no dock
// (AirportBound=no) takes the occupied air base as a normal factory instead.
DEFINE_HOOK(0x4FA43E, HouseClass_BeginProduction_UndockedAircraft, 0x7)
{
	enum { FactoryFound = 0x4FA46A, NoFactory = 0x4FA447 };

	GET(BuildingClass*, pFactory, EAX);
	GET(TechnoTypeClass*, pType, EBP);
	GET(HouseClass*, pHouse, EDI);

	R->Stack(0x1C, false);

	if (!pFactory && pType && pType->WhatAmI() == AbstractType::AircraftType && !static_cast<AircraftTypeClass*>(pType)->AirportBound)
	{
		pFactory = pType->FindFactory(true, true, true, pHouse);
		R->EAX(pFactory);
	}

	return pFactory ? FactoryFound : NoFactory;
}

#include "Body.h"

#include <Ext/Foot/Body.h>
#include <Ext/House/Body.h>
#include <Ext/TechnoType/Body.h>
#include <AircraftClass.h>
#include <AnimClass.h>
#include <CellClass.h>
#include <FlyLocomotionClass.h>
#include <JumpjetLocomotionClass.h>
#include <MapClass.h>
#include <Unsorted.h>
#include <Utilities/Debug.h>
#include <FactoryClass.h>
#include <New/Type/ResourceTypeClass.h>
#include <Misc/NeuralAI/NeuralAI.h>

DEFINE_HOOK(0x4C9BD5, FactoryClass_AI_ProcessProductionStep, 0x31)
{
	GET(FactoryClass*, pFactory, ESI);
	GET(int, moneyCost, EDI);

	const auto pOwner = pFactory->Owner;
	if (pOwner)
	{
		const auto pHouseExt = HouseExt::TryFetch(pOwner);
		const int availableMoney = pOwner->Available_Money();
		bool canAfford = (moneyCost <= availableMoney);

		for (size_t resIdx = 0; resIdx < ResourceTypeClass::Array.size(); ++resIdx)
		{
			const auto pResource = ResourceTypeClass::Array[resIdx].get();
			if (pResource && pResource->IsMoneyResource())
			{
				if (pHouseExt && !pHouseExt->IsResourceEnabled(static_cast<int>(resIdx)))
				{
					canAfford = false;
				}
				break;
			}
		}

		TechnoTypeClass* pType = pFactory->Object ? pFactory->Object->GetTechnoType() : (pFactory->QueuedObjects.Count > 0 ? pFactory->QueuedObjects[0] : nullptr);
		const auto pTypeExt = TechnoTypeExt::TryFetch(pType);

		std::vector<int> resDues;
		if (pTypeExt && pHouseExt && !pTypeExt->ResourceCosts.empty())
		{
			const int stage = pFactory->Production.Value;
			const int stepsRemaining = 54 - stage;
			resDues.resize(pTypeExt->ResourceCosts.size(), 0);

			for (size_t i = 0; i < pTypeExt->ResourceCosts.size(); ++i)
			{
				const int totalCost = pTypeExt->ResourceCosts[i];
				if (totalCost > 0)
				{
					const int spent = pHouseExt->GetFactoryResourceSpent(pFactory, i);
					const int remaining = totalCost - spent;
					int resCost = (stage >= 54) ? remaining : (remaining / (stepsRemaining > 0 ? stepsRemaining : 1));
					resCost = std::clamp(resCost, 0, remaining);

					if (resCost > 0)
					{
						resDues[i] = resCost;
						if (!pHouseExt->CanAffordResource(static_cast<int>(i), resCost))
						{
							canAfford = false;
						}
					}
				}
			}
		}

		if (canAfford)
		{
			// Deduct money
			if (moneyCost > 0)
			{
				pOwner->TakeMoney(moneyCost);
			}
			pFactory->Balance -= moneyCost;
			pFactory->OnHold = false;

			// Deduct custom resources
			if (pHouseExt && !resDues.empty())
			{
				for (size_t i = 0; i < resDues.size(); ++i)
				{
					if (resDues[i] > 0)
					{
						pHouseExt->UpdateResourceAmount(static_cast<int>(i), -resDues[i]);
						pHouseExt->AddFactoryResourceSpent(pFactory, i, resDues[i]);
					}
				}
			}
		}
		else
		{
			// Insufficient money or resources: put on hold and revert step
			pFactory->OnHold = true;
			pFactory->Production.Value = std::max(0, pFactory->Production.Value - 1);
		}
	}

	return 0x4C9C06;
}

DEFINE_HOOK(0x4C9C28, FactoryClass_AI_FinishProduction, 0x13)
{
	GET(FactoryClass*, pFactory, ESI);

	if (pFactory->Owner)
	{
		const int remainingMoney = pFactory->Balance;
		if (remainingMoney > 0)
		{
			pFactory->Owner->TakeMoney(remainingMoney);
		}
		pFactory->Balance = 0;

		if (const auto pHouseExt = HouseExt::TryFetch(pFactory->Owner))
		{
			pHouseExt->ClearFactoryResourceState(pFactory);
		}
	}

	return 0x4C9C3B;
}

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

	if (pOwner->IsCurrentPlayer() || pOwner->IsNeutral() || NeuralAI::IsEnabledForHouse(pOwner))
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
	enum { Skip = 0x4503CA, Continue = 0x4502FA };

	GET(BuildingClass*, pThis, ESI);
	HouseClass* const pOwner = pThis->Owner;

	if (pOwner && pOwner->Production && NeuralAI::IsEnabledForHouse(pOwner))
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

		// Only an aircraft that must land on a dock needs an air base with a free one.
		const int aircraftIndex = pOwner->ProducingAircraftTypeIndex;
		auto const pAircraftType = aircraftIndex >= 0 ? AircraftTypeClass::Array.GetItem(aircraftIndex) : nullptr;
		const bool needsFreeDock = !pAircraftType || pAircraftType->AirportBound;

		BuildingClass* pDesignatedPrimary = nullptr;
		BuildingClass* pFallbackPrimary = nullptr;
		for (auto const pBld : pOwner->Buildings)
		{
			if (pBld && pBld->IsAlive && !pBld->InLimbo && pBld->Type &&
				pBld->Type->Factory == pFactory && pBld->Type->Naval == naval &&
				!pBld->IsUnderEMP() && !pBld->Deactivated &&
				pBld->CurrentMission != Mission::Selling && pBld->QueuedMission != Mission::Selling &&
				pBld->CurrentMission != Mission::Construction)
			{
				if (pFactory == AbstractType::AircraftType && needsFreeDock && !BuildingExt::HasFreeDocks(pBld))
					continue;
				if (!pFallbackPrimary)
					pFallbackPrimary = pBld;
				if (pBld->IsPrimaryFactory)
				{
					pDesignatedPrimary = pBld;
					break;
				}
			}
		}

		if (!pDesignatedPrimary && pFallbackPrimary)
		{
			pFallbackPrimary->IsPrimaryFactory = true;
			pDesignatedPrimary = pFallbackPrimary;
		}

		if (pDesignatedPrimary)
		{
			if (currFactory)
				*currFactory = pDesignatedPrimary;
			if (pThis != pDesignatedPrimary)
				return Skip;
			R->EAX(pOwner);
			return Continue;
		}
	}

	if (pOwner->Production)
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

		if (currFactory)
		{
			if (*currFactory && *currFactory != pThis)
			{
				auto const pCurr = *currFactory;
				if (!pCurr->IsAlive || pCurr->InLimbo || pCurr->Owner != pOwner
					|| pCurr->IsUnderEMP() || pCurr->Deactivated
					|| pCurr->CurrentMission == Mission::Selling || pCurr->QueuedMission == Mission::Selling
					|| !pCurr->Factory)
				{
					*currFactory = nullptr;
				}
			}

			if (!*currFactory)
			{
				*currFactory = pThis;
				R->EAX(pOwner);
				return Continue;
			}
			else if (*currFactory != pThis)
			{
				if (!RulesExt::Global()->AllowParallelAIQueues)
					return Skip;

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

					index = naval ? pOwnerExt->ProducingNavalUnitTypeIndex : pOwner->ProducingUnitTypeIndex;
					pType = index >= 0 ? UnitTypeClass::Array.GetItem(index) : nullptr;
					break;
				default:
					break;
				}

				if (pType && TechnoTypeExt::Fetch(pType)->ForbidParallelAIQueues)
					return Skip;
			}
		}
	}

	R->EAX(pOwner);
	return Continue;
}

//const byte old_empty_log[] = { 0xC3 };
DEFINE_JUMP(CALL, 0x4CA016, 0x4CA19F); // randomly chosen 0xC3

DEFINE_HOOK(0x4CA07A, FactoryClass_AbandonProduction_Phobos, 0x8)
{
	GET(FactoryClass*, pFactory, ESI);
	GET_STACK(DWORD const, calledby, 0x18);

	if (pFactory && pFactory->Owner)
	{
		if (const auto pHouseExt = HouseExt::TryFetch(pFactory->Owner))
		{
			for (size_t i = 0; i < ResourceTypeClass::Array.size(); ++i)
			{
				const int spent = pHouseExt->GetFactoryResourceSpent(pFactory, i);
				if (spent > 0)
				{
					pHouseExt->UpdateResourceAmount(static_cast<int>(i), spent);
				}
			}
			pHouseExt->ClearFactoryResourceState(pFactory);
		}
	}

	auto const pTechno = pFactory->Object;

	if (calledby < 0x7F0000) // Replace the old log with this to figure out where keeps flushing the stream
	{
		Debug::LogGame("(%08x) : %s is abandoning production of %s[%s]\n"
			, calledby - 5
			, pFactory->Owner->PlainName
			, pTechno->GetType()->Name
			, pTechno->get_ID());
	}

	auto const pOwnerExt = HouseExt::Fetch(pFactory->Owner);
	auto const pTypeExt = TechnoExt::Fetch(pTechno)->TypeExtData;
	auto const pType = pTypeExt->OwnerObject();

	switch (pTechno->WhatAmI())
	{
	case AbstractType::Building:
		pOwnerExt->Factory_BuildingType = nullptr;
		break;
	case AbstractType::Unit:
		if (!pType->Naval)
			pOwnerExt->Factory_VehicleType = nullptr;
		else
			pOwnerExt->Factory_NavyType = nullptr;
		break;
	case AbstractType::Infantry:
		pOwnerExt->Factory_InfantryType = nullptr;
		break;
	case AbstractType::Aircraft:
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

KickOutResult BuildingExt::TrySpawnFlyingProduction(BuildingClass* pFactory, TechnoClass* pProduction)
{
	if (!pFactory || !pProduction || pFactory->GetCurrentMission() == Mission::Construction)
		return KickOutResult::Failed;

	if (pProduction->WhatAmI() == AbstractType::Building)
		return KickOutResult::Failed;

	auto const pType = pProduction->GetTechnoType();
	if (!pType)
		return KickOutResult::Failed;

	auto const pTypeExt = TechnoTypeExt::Fetch(pType);
	if (!pTypeExt || !pTypeExt->IsFlyingProductionEnabled())
		return KickOutResult::Failed;

	// Only an aircraft that must land on a dock (AirportBound) waits for a free one and takes it.
	auto const pProducedAircraft = abstract_cast<AircraftClass*>(pProduction);
	const bool needsDock = pProducedAircraft && pProducedAircraft->Type->AirportBound;

	// Resolve which building should physically spawn the unit
	BuildingClass* pSpawnBuilding = nullptr;

	if (!pTypeExt->FlyingProduction_SpawnAt.empty())
	{
		for (auto const pTargetType : pTypeExt->FlyingProduction_SpawnAt)
		{
			if (!pTargetType)
				continue;

			// If the factory itself matches this target type and has free dock (or doesn't need docks), it takes immediate precedence
			if (pFactory->Type == pTargetType)
			{
				if (!needsDock || pFactory->Type->NumberOfDocks <= 0 || pFactory->HasFreeLink())
				{
					pSpawnBuilding = pFactory;
					break;
				}
			}

			BuildingClass* pPrimaryCandidate = nullptr;
			BuildingClass* pClosestCandidate = nullptr;
			int minDistance = (std::numeric_limits<int>::max)();

			for (auto const pBld : pFactory->Owner->Buildings)
			{
				if (!pBld || !pBld->IsAlive || pBld->InLimbo || pBld->Health <= 0 || pBld->GetCurrentMission() == Mission::Selling)
					continue;

				if (pBld->Type == pTargetType)
				{
					if (needsDock && pBld->Type->NumberOfDocks > 0 && !pBld->HasFreeLink())
						continue;

					if (pBld->IsPrimaryFactory)
					{
						pPrimaryCandidate = pBld;
						break;
					}

					const int dist = pFactory->DistanceFrom(pBld);
					if (dist < minDistance)
					{
						minDistance = dist;
						pClosestCandidate = pBld;
					}
				}
			}

			if (auto const pCandidate = pPrimaryCandidate ? pPrimaryCandidate : pClosestCandidate)
			{
				pSpawnBuilding = pCandidate;
				break;
			}
		}
	}

	if (!pSpawnBuilding)
		pSpawnBuilding = pFactory;

	// If this is an aircraft and the spawn building has docks, ensure there is an available dock slot
	if (needsDock && pSpawnBuilding->Type->NumberOfDocks > 0)
	{
		if (!pSpawnBuilding->HasFreeLink())
			return KickOutResult::Busy;
	}

	auto const pSpawnBldTypeExt = BuildingTypeExt::Fetch(pSpawnBuilding->Type);

	// Determine spawn height, offset, and facing
	int height = pTypeExt->GetFlyingProductionSpawnHeight();
	if (pSpawnBldTypeExt && pSpawnBldTypeExt->FlyingProduction_SpawnHeight.isset())
		height = pSpawnBldTypeExt->FlyingProduction_SpawnHeight.Get();

	if (height <= 0)
		return KickOutResult::Failed;

	CoordStruct spawnCoords = pSpawnBuilding->GetCoords();
	if (pSpawnBldTypeExt && pSpawnBldTypeExt->FlyingProduction_SpawnOffset.isset())
	{
		const auto& offset = pSpawnBldTypeExt->FlyingProduction_SpawnOffset.Get();
		spawnCoords.X += offset.X;
		spawnCoords.Y += offset.Y;
	}
	spawnCoords.Z += height;

	DirStruct facing = DirStruct(128); // Default facing South
	if (pSpawnBldTypeExt && pSpawnBldTypeExt->FlyingProduction_SpawnFacing.isset())
		facing = DirStruct(pSpawnBldTypeExt->FlyingProduction_SpawnFacing.Get());
	else if (pSpawnBuilding->PrimaryFacing.Current().Raw != 0)
		facing = pSpawnBuilding->PrimaryFacing.Current();

	// Unlimbo unit in the air safely using ScenarioInitGuard
	pProduction->SetOwningHouse(pFactory->Owner, true);

	bool unlimboSuccess = false;
	{
		struct ScenarioInitGuard
		{
			ScenarioInitGuard() { ++Unsorted::ScenarioInit; }
			~ScenarioInitGuard() { --Unsorted::ScenarioInit; }
		} guard;

		unlimboSuccess = pProduction->Unlimbo(spawnCoords, facing.GetDir());
	}

	if (!unlimboSuccess)
		return KickOutResult::Busy;

	// Establish aerial state and position
	pProduction->SetLocation(spawnCoords);
	pProduction->Location = spawnCoords;
	pProduction->InAir = true;
	pProduction->IsALoaner = false;
	pProduction->UnmarkAllOccupationBits(spawnCoords);
	pProduction->PrimaryFacing.SetCurrent(facing);
	pProduction->PrimaryFacing.SetDesired(facing);

	// Configure locomotor (Jumpjet / Aircraft)
	if (auto const pFoot = abstract_cast<FootClass*>(pProduction))
	{
		if (auto const pJJLoco = locomotion_cast<JumpjetLocomotionClass*>(pFoot->Locomotor))
		{
			pJJLoco->CurrentHeight = height;
			pJJLoco->LocomotionFacing.SetCurrent(facing);
			pJJLoco->LocomotionFacing.SetDesired(facing);
			pFoot->Jumpjet_OccupyCell(CellClass::Coord2Cell(spawnCoords));

			if (height < pJJLoco->Height)
			{
				pJJLoco->State = JumpjetLocomotionClass::State::Ascending;
				FootExt::Fetch(pFoot)->JumpjetStraightAscend = true;
			}
			else
			{
				const bool wasOnMap = pFoot->IsOnMap;
				pFoot->Mark(MarkType::Up);
				pFoot->IsOnMap = wasOnMap;
				pJJLoco->State = JumpjetLocomotionClass::State::Hovering;
			}
		}
		else if (auto const pFlyLoco = locomotion_cast<FlyLocomotionClass*>(pFoot->Locomotor))
		{
			const auto pFootType = pFoot->GetTechnoType();
			const int cruiseFlightLevel = pFootType->GetFlightLevel();
			const int targetFlightLevel = cruiseFlightLevel > 0 ? cruiseFlightLevel : (RulesClass::Instance ? RulesClass::Instance->FlightLevel : height);
			pFlyLoco->FlightLevel = (height > targetFlightLevel) ? height : targetFlightLevel;
			pFlyLoco->CurrentSpeed = 0.0;
			pFlyLoco->TargetSpeed = 0.0;
			pFlyLoco->IsTakingOff = height < targetFlightLevel ? 1 : 0;
			pFlyLoco->IsLanding = false;
		}
	}

	if (auto const pAircraft = abstract_cast<AircraftClass*>(pProduction))
	{
		if (needsDock && pSpawnBuilding->Type->NumberOfDocks > 0)
		{
			pSpawnBuilding->SendCommand(RadioCommand::RequestLink, pAircraft);
			pSpawnBuilding->SendCommand(RadioCommand::RequestTether, pAircraft);
			pAircraft->DockNowHeadingTo = pSpawnBuilding;

			if (!pSpawnBldTypeExt || !pSpawnBldTypeExt->FlyingProduction_SpawnOffset.isset())
			{
				CoordStruct dockCoords = spawnCoords;
				pSpawnBuilding->GetDockCoords(&dockCoords, pAircraft);
				dockCoords.Z = pSpawnBuilding->GetCoords().Z + height;
				pAircraft->SetLocation(dockCoords);
				pAircraft->Location = dockCoords;
				spawnCoords = dockCoords;
			}
		}

		pAircraft->SetHeight(height);
	}

	// Spawn visual effects
	if (auto const pAnimType = pTypeExt->FlyingProduction_SpawnAnim.Get())
	{
		if (auto const pAnim = GameCreate<AnimClass>(pAnimType, spawnCoords))
		{
			if (pTypeExt->FlyingProduction_SpawnAnim_AttachedToObject.Get())
				pAnim->SetOwnerObject(pProduction);
		}
	}

	// Direct unit towards rally point or nudge away from factory
	AbstractClass* pRallyTarget = nullptr;
	if (pTypeExt->FlyingProduction_RallyPointFromSpawnBuilding.Get() && pSpawnBuilding->ArchiveTarget)
		pRallyTarget = pSpawnBuilding->ArchiveTarget;
	else
		pRallyTarget = pFactory->ArchiveTarget;

	if (auto const pFoot = abstract_cast<FootClass*>(pProduction))
	{
		auto const pFootExt = FootExt::Fetch(pFoot);
		int cruiseHeight = 0;

		if (auto const pJJLoco = locomotion_cast<JumpjetLocomotionClass*>(pFoot->Locomotor))
			cruiseHeight = pJJLoco->Height;
		else if (auto const pFlyLoco = locomotion_cast<FlyLocomotionClass*>(pFoot->Locomotor))
		{
			const int cfl = pFoot->GetTechnoType()->GetFlightLevel();
			cruiseHeight = cfl > 0 ? cfl : (RulesClass::Instance ? RulesClass::Instance->FlightLevel : 0);
		}

		if (cruiseHeight > 0 && height > cruiseHeight)
		{
			// Unit spawned higher than cruise altitude: descend straight down in place first
			pFootExt->FlyingProduction_Descending = true;
			pFootExt->FlyingProduction_TargetHeight = cruiseHeight;
			pFootExt->FlyingProduction_RallyTarget = pRallyTarget;
			pFootExt->FlyingProduction_SpawnBuilding = pSpawnBuilding;
			pFootExt->FlyingProduction_ExitFacing = facing;

			pProduction->QueueMission(Mission::Guard, true);
		}
		else
		{
			pFootExt->FlyingProduction_DispatchMove(pRallyTarget, pSpawnBuilding, facing);
		}
	}
	else
	{
		if (pRallyTarget && pRallyTarget != pSpawnBuilding)
		{
			pProduction->SetDestination(pRallyTarget, true);
			pProduction->QueueMission(Mission::Move, true);
		}
	}

	// Trigger factory door/unload animation if requested
	if (pTypeExt->FlyingProduction_PlayFactoryAnim.Get())
		pFactory->QueueMission(Mission::Unload, false);

	// Reset house production indices, factory pointers, and replicate via cloning vats
	if (auto const pOwner = pFactory->Owner)
	{
		auto const pHouseExt = HouseExt::Fetch(pOwner);
		auto const abs = pProduction->WhatAmI();

		if (abs == AbstractType::Unit)
		{
			if (auto const pUnit = abstract_cast<UnitClass*>(pProduction))
			{
				if (pUnit->Type->Naval)
				{
					if (pHouseExt)
						pHouseExt->ProducingNavalUnitTypeIndex = -1;
				}
				else
				{
					pOwner->ProducingUnitTypeIndex = -1;
				}
			}
		}
		else if (abs == AbstractType::Aircraft)
		{
			pOwner->ProducingAircraftTypeIndex = -1;
		}
		else if (abs == AbstractType::Infantry)
		{
			pOwner->ProducingInfantryTypeIndex = -1;
		}
		else if (abs == AbstractType::Building)
		{
			pOwner->ProducingBuildingTypeIndex = -1;
		}

		if (pHouseExt)
		{
			if (pHouseExt->Factory_VehicleType == pFactory)
				pHouseExt->Factory_VehicleType = nullptr;
			if (pHouseExt->Factory_NavyType == pFactory)
				pHouseExt->Factory_NavyType = nullptr;
			if (pHouseExt->Factory_InfantryType == pFactory)
				pHouseExt->Factory_InfantryType = nullptr;
			if (pHouseExt->Factory_AircraftType == pFactory)
				pHouseExt->Factory_AircraftType = nullptr;
			if (pHouseExt->Factory_BuildingType == pFactory)
				pHouseExt->Factory_BuildingType = nullptr;
		}

		if (!pFactory->Type->Cloning)
		{
			auto info = std::make_pair(pType, pOwner);
			for (const auto pVat : pOwner->CloningVats)
				BuildingExt::KickOutClone(info, 0, pVat);
		}

		BuildingClass* pRecordBld = pSpawnBuilding ? pSpawnBuilding : pFactory;

		if (pRecordBld)
		{
			auto const pRecordBldExt = BuildingExt::Fetch(pRecordBld);

			pRecordBldExt->LastProducedTechno = pProduction;
			pRecordBldExt->LastProducedType = pType;
		}
	}

	return KickOutResult::Succeeded;
}

DEFINE_HOOK(0x443C60, BuildingClass_KickOutUnit_FlyingProduction, 0x6)
{
	enum { DoneSucceeded = 0x4456A2 };

	GET(BuildingClass*, pFactory, ECX);
	GET_STACK(TechnoClass*, pProduction, 0x4);

	if (pFactory && pProduction)
	{
		auto const pBldExt = BuildingExt::Fetch(pFactory);

		pBldExt->LastProducedTechno = pProduction;
		pBldExt->LastProducedType = pProduction->GetTechnoType();
	}

	if (!pProduction || pProduction->WhatAmI() == AbstractType::Building)
		return 0;

	const auto result = BuildingExt::TrySpawnFlyingProduction(pFactory, pProduction);
	if (result != KickOutResult::Failed)
	{
		R->EAX(static_cast<int>(result));
		return DoneSucceeded;
	}

	return 0;
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

DEFINE_HOOK(0x455DA0, BuildingClass_IsUnitFactory_HasRallyPoint, 0x6)
{
	enum { ReturnFalse = 0x455DCB, ReturnTrue = 0x455DCD };

	GET(BuildingClass*, pThis, ECX);

	if (auto const pTypeExt = BuildingTypeExt::Fetch(pThis->Type))
	{
		if (pTypeExt->HasRallyPoint.isset())
			return pTypeExt->HasRallyPoint.Get() ? ReturnTrue : ReturnFalse;
	}

	return 0;
}




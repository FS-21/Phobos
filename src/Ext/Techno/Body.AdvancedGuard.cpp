#include "Body.h"

#include <Ext/WarheadType/Body.h>

void TechnoExt::SendEngineerGuardStopTarNav(TechnoClass* pThis)
{
	auto pFoot = abstract_cast<FootClass*>(pThis);

	EventExt event;
	event.Type = EventTypeExt::SyncEngineerGuardStopTarNav;
	event.HouseIndex = (char)HouseClass::CurrentPlayer->ArrayIndex;
	event.Frame = Unsorted::CurrentFrame;
	event.SyncWeaponizedEngineerGuard.TechnoUniqueID = pThis->UniqueID;

	event.AddEvent();
}

void TechnoExt::HandleEngineerGuardStopTarNav(EventExt* event)
{
	int technoUniqueID = event->SyncEngineerGuardStopTarNav.TechnoUniqueID;

	for (auto pTechno : *TechnoClass::Array)
	{
		// Find the object, update/sync
		if (pTechno && pTechno->UniqueID == technoUniqueID)
		{
			auto const pExt = TechnoExt::ExtMap.Find(pTechno);
			pExt->WeaponizedEngineer_GuardDestination = nullptr;

			pTechno->SetDestination(nullptr, true);
			pTechno->ForceMission(Mission::Guard);

			//auto pFoot = abstract_cast<FootClass*>(pTechno);

			//if (pFoot->Locomotor->Is_Moving_Now())
				//pFoot->StopMoving();

			//pTechno->QueueMission(Mission::Guard, false);

			break;
		}
	}
}

void TechnoExt::SendEngineerGuardDestination(TechnoClass* pThis, AbstractClass* pDestination)
{
	if (!pDestination)
		return;

	EventExt event;
	event.Type = EventTypeExt::SyncEngineerGuardDestination;
	event.HouseIndex = (char)HouseClass::CurrentPlayer->ArrayIndex;
	event.Frame = Unsorted::CurrentFrame;
	event.SyncEngineerGuardDestination.TechnoUniqueID = pThis->UniqueID;
	event.SyncEngineerGuardDestination.GuardDestinationID = pDestination->UniqueID;

	event.AddEvent();
}

void TechnoExt::HandleEngineerGuardDestination(EventExt* event)
{
	int technoUniqueID = event->SyncEngineerGuardDestination.TechnoUniqueID;
	int guardDestinationID = event->SyncEngineerGuardDestination.GuardDestinationID;
	//AbstractClass* pAbstract = event->SyncEngineerGuardDestination.GuardDestination;

	//auto guardDestination = static_cast<TechnoClass*>(pAbstract);
	//if (!guardDestination)
		//return;

	/*for (auto pTechno : *TechnoClass::Array)
	{
		if (pTechno && pTechno->UniqueID == technoUniqueID)
		{
			auto const pExt = TechnoExt::ExtMap.Find(pTechno);

			if (TechnoExt::IsValidTechno(guardDestination))
				pExt->WeaponizedEngineer_GuardDestination = guardDestination;
			else
				pExt->WeaponizedEngineer_GuardDestination = nullptr;

			break;
		}
	}*/

	for (auto pTechno : *TechnoClass::Array)
	{
		if (pTechno && pTechno->UniqueID == guardDestinationID)
		{
			auto const pExt = TechnoExt::ExtMap.Find(pTechno);

			if (TechnoExt::IsValidTechno(pTechno))
				pExt->WeaponizedEngineer_GuardDestination = pTechno;
			else
				pExt->WeaponizedEngineer_GuardDestination = nullptr;

			break;
		}
	}
}

void TechnoExt::SendWeaponizedEngineerGuard(TechnoClass* pThis)
{
	auto pFoot = abstract_cast<FootClass*>(pThis);

	EventExt event;
	event.Type = EventTypeExt::SyncWeaponizedEngineerGuard;
	event.HouseIndex = (char)HouseClass::CurrentPlayer->ArrayIndex;
	event.Frame = Unsorted::CurrentFrame;//currentFrame + Game::Network::MaxAhead;

	event.SyncWeaponizedEngineerGuard.TechnoUniqueID = pThis->UniqueID;

	event.AddEvent();
}

void TechnoExt::HandleWeaponizedEngineerGuard(EventExt* event)
{
	int technoUniqueID = event->SyncWeaponizedEngineerGuard.TechnoUniqueID;

	for (auto pTechno : *TechnoClass::Array)
	{
		if (pTechno && pTechno->UniqueID == technoUniqueID)
		{
			TechnoExt::ProcessWeaponizedEngineerGuard(pTechno);
			break;
		}
	}
}

void TechnoExt::ProcessWeaponizedEngineerGuard(TechnoClass* pThis)
{
	if (!pThis || !TechnoExt::IsValidTechno(pThis))
		return;

	auto const pType = pThis->GetTechnoType();
	auto const pTypeExt = TechnoTypeExt::ExtMap.Find(pType);

	if (!pTypeExt || !pTypeExt->Engineer_CheckFriendlyWeapons)
		return;

	auto const pExt = TechnoExt::ExtMap.Find(pThis);

	if (pThis->CurrentMission != Mission::Area_Guard && pThis->CurrentMission != Mission::Guard && pExt->WeaponizedEngineer_GuardDestination)
		pExt->WeaponizedEngineer_GuardDestination = nullptr;

	auto pFoot = abstract_cast<FootClass*>(pThis);

	// Move to the target until the unit reaches the weapons's range
	if (pFoot->Destination && pThis->Owner->IsAlliedWith(pFoot->Destination))
	{
		if (pThis->CurrentMission == Mission::Capture)
			return;

		auto const pCurrentTarget = pThis->Target;

		if (pThis->IsCloseEnoughToAttack(pFoot->Destination))
		{
			auto const pCurrentMission = pThis->CurrentMission;
			pThis->CurrentMission = pCurrentMission;
			pFoot->Destination = nullptr;
		}

		pThis->Target = pCurrentTarget; // Since SetTarget(...) isn't used here the target's info won't dissappear in the next frames so maybe I should remove this line and "pFoot->StopMoving()" doesn't look that triggers this weird "bug".

		return;
	}

	TechnoClass* const pTarget = pThis->Target ? abstract_cast<TechnoClass*>(pThis->Target) : nullptr;

	// Stop the unit movement if is inside the weapon's range so it can start attacking the target
	/*if (pTarget && pThis->IsCloseEnoughToAttack(pTarget))
	{
		if (pFoot->Locomotor->Is_Moving_Now())
			pFoot->StopMoving();

		pThis->Target = pTarget;

		return;
	}*/
	if (pTarget)
	{
		if (pThis->IsCloseEnoughToAttack(pTarget))
		{
			if (pFoot->Locomotor->Is_Moving_Now())
				pFoot->StopMoving();

			pThis->Target = pTarget;
		}
		else
		{
			if (!pFoot->Locomotor->Is_Moving_Now() && !pFoot->Destination)
			{
				// If the unit must move for reaching the target then pick the closest cell around the target
				auto pCell = pThis->GetCell();
				bool allowBridges = GroundType::Array[static_cast<int>(LandType::Clear)].Cost[static_cast<int>(pType->SpeedType)] > 0.0;
				bool isBridge = allowBridges && pCell->ContainsBridge();
				auto nCell = MapClass::Instance->NearByLocation(CellClass::Coord2Cell(pTarget->Location), pType->SpeedType, -1, pType->MovementZone, isBridge, 1, 1, true, false, false, isBridge, CellStruct::Empty, false, false);
				pCell = MapClass::Instance->TryGetCellAt(nCell);
				pThis->SetDestination(pCell, false);
				pThis->Target = nullptr;
			}
		}

		return;
	}

	// The search function only works if the unit is awaiting orders in guard mode
	if (pThis->CurrentMission != Mission::Area_Guard && pThis->CurrentMission != Mission::Guard)
		return;

	TechnoClass* seletedTarget = nullptr;
	int bestVal = -1;
	int range = -1;

	// Looks for the closest valid target
	for (auto const pTechno : *TechnoClass::Array)
	{
		int value = pThis->DistanceFrom(pTechno); // Note: distance is in leptons (*256)
		int weaponIndex = pThis->SelectWeapon(pTechno);
		auto weaponType = pThis->GetWeapon(weaponIndex)->WeaponType;
		auto const pWHExt = WarheadTypeExt::ExtMap.Find(weaponType->Warhead);
		bool canDisarmBombs = pWHExt->CanDisarmBombs && pTechno->AttachedBomb && pThis->Owner->IsAlliedWith(pTechno);
		double versusArmor = GeneralUtils::GetWarheadVersusArmor(weaponType->Warhead, pTechno->GetTechnoType()->Armor);
		int realDamage = static_cast<int>(weaponType->Damage * versusArmor * pThis->FirepowerMultiplier);
		bool isHealerWeapon = realDamage < 0;
		bool healingCondition = (isHealerWeapon && pTechno->Health < pTechno->GetTechnoType()->Strength && pThis->Owner->IsAlliedWith(pTechno));
		//bool offensiveCondition = (!isHealerWeapon && !pThis->Owner->IsAlliedWith(pTechno));
		int guardRange = pThis->CurrentMission == Mission::Area_Guard || pType->DefaultToGuardArea ? pThis->GetGuardRange(1) : weaponType->Range;

		if ((pTechno != pThis
			&& pTechno->IsAlive
			&& pTechno->Health > 0
			&& !pTechno->InLimbo
			&& value <= guardRange
			&& (healingCondition || canDisarmBombs)// || !offensiveCondition
			&& !pTechno->GetTechnoType()->Immune
			&& !pTechno->BeingWarpedOut
			&& !pTechno->TemporalTargetingMe
			&& pTechno->InWhichLayer() != Layer::Underground
			))
		{
			if (value < bestVal || bestVal < 0)
			{
				bestVal = value;
				seletedTarget = pTechno;
			}
		}
	}

	if (!seletedTarget && !pFoot->Destination && pExt->WeaponizedEngineer_GuardDestination)
		seletedTarget = static_cast<TechnoClass*>(pExt->WeaponizedEngineer_GuardDestination);

	if (seletedTarget && seletedTarget != pThis->Target)
	{
		pThis->SetTarget(seletedTarget);

		// Prevent involuntary movement produced by SetTarget() method if the unit is in weapon's range
		if (pThis->IsCloseEnoughToAttack(seletedTarget))
		{
			pThis->SetDestination(nullptr, true);
			return;
		}

		// If the unit must move for reaching the target then pick the closest cell around the target
		auto pCell = pThis->GetCell();
		bool allowBridges = GroundType::Array[static_cast<int>(LandType::Clear)].Cost[static_cast<int>(pType->SpeedType)] > 0.0;
		bool isBridge = allowBridges && pCell->ContainsBridge();
		auto nCell = MapClass::Instance->NearByLocation(CellClass::Coord2Cell(seletedTarget->Location), pType->SpeedType, -1, pType->MovementZone, isBridge, 1, 1, true, false, false, isBridge, CellStruct::Empty, false, false);
		pCell = MapClass::Instance->TryGetCellAt(nCell);
		pThis->SetDestination(pCell, false);
	}
}

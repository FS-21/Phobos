// NeuralAI.Air.inl: Safe air lanes, routed pickups and pending cargo.
// Part of NeuralAI.cpp (included in order; one translation unit).

namespace
{
	// Next hop of a flying carrier toward pGoal along a lane that avoids known enemy anti-air
	// (FindSafeFerryWaypoint in airborne mode). The detour is recomputed when the goal moves or
	// every 450 frames, dropped once reached; close to the goal it flies straight in.
	CellClass* GetSafeAirHop(HouseClass* pHouse, FootClass* pCarrier, CellClass* pGoal)
	{
		if (!pHouse || !pCarrier || !pGoal)
			return pGoal;

		auto const pCarrierType = pCarrier->GetTechnoType();
		const CellStruct from = pCarrier->GetMapCoords();

		if (!pCarrierType || from.DistanceFrom(pGoal->MapCoords) <= 12.0)
		{
			AirRoutes.erase(pCarrier->UniqueID);
			return pGoal;
		}

		if (AirRoutes.size() > 256)
		{
			std::erase_if(AirRoutes, [](const auto& entry)
				{ return entry.second.Frame > Unsorted::CurrentFrame || Unsorted::CurrentFrame - entry.second.Frame > 1800; });
		}

		auto& route = AirRoutes[pCarrier->UniqueID];

		if (route.Target == CellStruct::Empty || route.Target.DistanceFrom(pGoal->MapCoords) > 6.0
			|| Unsorted::CurrentFrame - route.Frame > 450 || route.Frame > Unsorted::CurrentFrame)
		{
			route.Target = pGoal->MapCoords;
			route.Frame = Unsorted::CurrentFrame;
			route.Waypoint = FindSafeFerryWaypoint(pHouse, from, pGoal->MapCoords, pCarrierType);

			if (route.Waypoint != CellStruct::Empty)
				CountDecision(NeuralDecision::SafeLaneDetour, pHouse->ArrayIndex);
		}

		if (route.Waypoint != CellStruct::Empty)
		{
			if (from.DistanceFrom(route.Waypoint) <= 4.0)
				route.Waypoint = CellStruct::Empty;
			else if (auto const pWaypointCell = MapClass::Instance.TryGetCellAt(route.Waypoint))
				return pWaypointCell;
		}

		return pGoal;
	}

	// Pickup leg of an empty carryall: while the safe air lane still needs a detour it flies
	// the detour hop; once the lane is direct (or within 12 cells) it gets the real pickup order, a
	// move onto the cargo. The engine's carryall aircraft lifts a vehicle on its move mission sent to
	// the vehicle itself, as the player's tote order does (AircraftClass::Do_MISSION_MOVE_Carryall:
	// it radios the vehicle to hold still, comes down over it and takes it aboard); told to capture
	// its cargo it sat on its pad (14 of 14 carryalls of the transport operations, 2026-11-16). The
	// Phobos unit carryall lifts the same way. Returns true when the real pickup order was given.
	bool AdvanceRoutedPickup(HouseClass* pHouse, FootClass* pCarrier, UnitClass* pCargo)
	{
		if (!pHouse || !pCarrier || !pCargo)
			return false;

		CellClass* const pCargoCell = pCargo->GetCell();
		CellClass* const pHop = pCargoCell ? GetSafeAirHop(pHouse, pCarrier, pCargoCell) : nullptr;

		if (!pHop || pHop == pCargoCell)
		{
			AirPickups.erase(pCarrier->UniqueID);
			pCarrier->SetTarget(nullptr);
			pCarrier->SetDestination(pCargo, true);
			pCarrier->QueueMission(Mission::Move, true);

			return true;
		}

		if (AirPickups.size() > 256)
		{
			std::erase_if(AirPickups, [](const auto& entry)
				{ return entry.second.Frame > Unsorted::CurrentFrame || Unsorted::CurrentFrame - entry.second.Frame > 2700; });
		}

		auto& record = AirPickups[pCarrier->UniqueID];

		if (record.CargoID != pCargo->UniqueID)
		{
			record.CargoID = pCargo->UniqueID;
			record.Frame = Unsorted::CurrentFrame;
			CountDecision(NeuralDecision::PickupDetour, pHouse->ArrayIndex);
		}

		IssueMoveOrder(pCarrier, pHop, 3.0);

		return false;
	}

	// Cargo of a pickup still flying its detour (AdvanceRoutedPickup), or nullptr when there is
	// none or it went stale (cargo gone, changed owner, or more than 2700 frames old).
	UnitClass* FindPendingPickupCargo(HouseClass* pHouse, FootClass* pCarrier)
	{
		if (!pHouse || !pCarrier)
			return nullptr;

		auto const it = AirPickups.find(pCarrier->UniqueID);

		if (it == AirPickups.end())
			return nullptr;

		const int age = Unsorted::CurrentFrame - it->second.Frame;

		if (age >= 0 && age <= 2700)
		{
			for (auto const pUnit : UnitClass::Array)
			{
				if (pUnit && pUnit->UniqueID == it->second.CargoID)
				{
					if (pUnit->IsAlive && !pUnit->InLimbo && pUnit->Owner == pHouse)
						return pUnit;

					break;
				}
			}
		}

		AirPickups.erase(it);

		return nullptr;
	}


	// =====================================================================================
	// Strategic Plan Library
	// -------------------------------------------------------------------------------------
	// Coordinated multi-unit plans with preconditions, a recruited group, progress tracking
	// and an explicit success/failure verdict. Each plan has a trained preference slot
	// (NeuralPlanSlots) that ES learns like any other output, plus a context utility from
	// the fog-fair opponent model; SlotGate turns both into a deterministic start decision.
	// Plan members are excluded from the generic idle-army recruitment, and this update runs
	// after the tactical micro so a plan keeps the last word on its own units.
	// =====================================================================================

} // namespace

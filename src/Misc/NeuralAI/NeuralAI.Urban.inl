// NeuralAI.Urban.inl: Expert 9 (Urban Garrison & Allied Coalition Warfare): fog-fair memory of
// occupiable buildings, which of them are worth holding, garrison orders and evacuations, the
// enemy garrisons to clear and the coalition work (allied rescues, joint attacks, shared techs)
// that feed the expert selector.
// Part of NeuralAI.cpp (included in order; one translation unit).

namespace
{
	// Cells from our structures (10 less from a garrison of ours) within which a building is ours
	// to take without the army standing next to it.
	constexpr int UrbanReachCells = 28;
	// Seats the barracks are asked to fill at a time: a squad for the best positions, or a city.
	constexpr int UrbanSquadSeats = 12;
	constexpr int UrbanCitySeats = 60;
	// A tank for an empty tank bunker is built on purpose only with this much money left after it.
	constexpr int GarrisonTrainingReserve = 600;
	// Below this health a building is not garrisoned (it throws its soldiers out before it falls).
	constexpr int UrbanGarrisonMinHealthPercent = 45;
	// An engineer repairs a garrison held by us or an ally below this share of health. An empty
	// building is repaired only when idle occupiers within UrbanRepairReachCells of it asked for it
	// in the last UrbanRepairWantedFrames: they have no other building to take and only its damage
	// keeps them out (user: never a repair for its own sake).
	constexpr double UrbanRepairHeldBelow = 0.5;
	constexpr int UrbanRepairReachCells = 40;
	constexpr int UrbanRepairWantedFrames = 900;
	// A spy or a vehicle thief is trained only with this many times its price in the bank.
	constexpr int SpyLuxuryMoneyFactor = 2;
	// Frames a soldier may spend on one capture order before it is called off, and frames a
	// soldier whose orders failed is left out of the garrisons.
	constexpr int UrbanCaptureOrderFrames = 600;
	constexpr int UrbanBenchFrames = 2700;

	// An occupiable building (civilian building or battle bunker) this house knows (from the map or
	// seen), as it looked the last time it was in sight.
	struct NeuralKnownUrbanBuilding
	{
		DWORD ID { 0 };
		CellStruct Cell { CellStruct::Empty };
		int MaxOccupants { 0 };
		// House of the occupants when last in sight; -1 while it was empty.
		int OccupantOwner { -1 };
		int Occupants { 0 };
		int HealthPercent { 100 };
		int SeenFrame { 0 };
		// Value of holding it now (0: not worth it or unsafe), refreshed by every scan.
		int Worth { 0 };
		// The same with its health back: a building too damaged to take soldiers is worth an
		// engineer only when we would garrison it afterwards.
		int WorthIfRepaired { 0 };
		int BlockedUntilFrame { 0 };
		// Orders that did not get a soldier in (it was ordered there again soon after).
		int Failures { 0 };
		// Last frame an enemy harvester was seen in current sight within 7 cells (a garrison
		// there cuts the enemy's supply line).
		int EnemySupplySeenFrame { -100000 };
		// Cells to our nearest structure, or to the nearest garrison of ours plus 10, at the last scan.
		int ReachDist { 999 };
	};

	// The last garrison order of one soldier.
	struct NeuralUrbanOrder
	{
		DWORD InfantryID { 0 };
		DWORD BuildingID { 0 };
		int Frame { 0 };
		// Orders of this soldier into this building in a row (each within 900 frames of the last)
		// that brought it no nearer.
		int Tries { 1 };
		// Cells to the building when the order was given.
		int Cells { 0 };
	};

	// Infantry walking into an occupiable building (ours or an ally's), counted per building.
	struct NeuralUrbanEntering
	{
		DWORD BuildingID { 0 };
		int Own { 0 };
		int Allied { 0 };
	};

	struct NeuralUrbanState
	{
		int MatchSerial { -1 };
		int LastScanFrame { -100000 };
		// Sorted by ID (binary search, deterministic order).
		std::vector<NeuralKnownUrbanBuilding> Buildings {};
		// The civilian buildings the map starts with were taken into the memory.
		bool MapBuildingsKnown { false };
		// The live building of each entry of Buildings (same index, null once gone), valid in
		// LiveFrame only.
		std::vector<BuildingClass*> Live {};
		int LiveFrame { -1 };
		// Rebuilt every AI tick, sorted by building ID.
		std::vector<NeuralUrbanEntering> Entering {};
		// Sorted by infantry ID; orders older than 900 frames are dropped.
		std::vector<NeuralUrbanOrder> Orders {};
		// Soldiers whose garrison orders kept failing (soldier ID, frame it may try again), sorted
		// by ID: a soldier that cannot leave its spot would block one building after another.
		std::vector<std::pair<DWORD, int>> Benched {};
		// Buildings an allied engineer is walking into or an allied Neural AI plans to capture,
		// rebuilt every AI tick, sorted.
		std::vector<DWORD> AllyTechClaims {};
		// Summary of the last scan.
		bool OccupierAvailable { false };
		bool ClearerAvailable { false };
		int OwnOccupierInfantry { 0 };
		int OwnGarrisons { 0 };
		int OwnOccupants { 0 };
		int GarrisonWorth { 0 };
		// Known buildings worth holding at the last scan; 6 or more make an urban map.
		int WorthyBuildings { 0 };
		// Seats of those buildings, and the ones nobody was in when last seen.
		int WorthySeats { 0 };
		int WorthyFreeSeats { 0 };
		int ThreatGarrisons { 0 };
		int BunkersToMan { 0 };
		int GarrisonsInDanger { 0 };
		int Allies { 0 };
		int AllyRescue { 0 };
		int AllyAttackPressure { 0 };
		int Opportunity { 0 };
		// Results this match (logs and tests).
		int GarrisonOrders { 0 };
		int Evacuations { 0 };
		int ClearOrders { 0 };
		int PeakGarrisons { 0 };
		int PeakOccupants { 0 };
		int LastReportFrame { 0 };
		// Garrison tries since the last report: idle occupiers looked at, over the share, no building.
		int TriesSinceReport { 0 };
		int OverCapSinceReport { 0 };
		int NoBuildingSinceReport { 0 };
		// Squads sent since the last report, and new buildings left alone for want of soldiers.
		int SquadsSinceReport { 0 };
		int SmallSquadSinceReport { 0 };
		// Zones where no building was available this frame: the other idle soldiers there skip the
		// search (with every tick from the start, 150 idle soldiers would repeat the same answer).
		int NoBuildingFrame { -1 };
		std::vector<int> NoBuildingZones {};
		// The building idle occupiers would take if it were not too damaged, and when they last
		// asked for it: the one an engineer may repair.
		DWORD RepairWantedID { 0 };
		int RepairWantedFrame { -100000 };
	};

	std::array<NeuralUrbanState, 32> UrbanStates {};

	NeuralUrbanState& GetUrbanState(const HouseClass* pHouse)
	{
		auto& state = UrbanStates[std::clamp(pHouse ? pHouse->ArrayIndex : 0, 0, 31)];

		if (state.MatchSerial != NeuralMatchSerial)
		{
			state = NeuralUrbanState {};
			state.MatchSerial = NeuralMatchSerial;
		}

		return state;
	}

	NeuralKnownUrbanBuilding* FindKnownUrbanBuilding(NeuralUrbanState& state, DWORD id)
	{
		auto const it = std::lower_bound(state.Buildings.begin(), state.Buildings.end(), id,
			[](const NeuralKnownUrbanBuilding& known, DWORD key) { return known.ID < key; });

		return (it != state.Buildings.end() && it->ID == id) ? &*it : nullptr;
	}

	// Leave a soldier out of the garrisons for a while.
	void BenchUrbanSoldier(NeuralUrbanState& state, DWORD infantryID)
	{
		auto const it = std::lower_bound(state.Benched.begin(), state.Benched.end(), infantryID,
			[](const std::pair<DWORD, int>& entry, DWORD key) { return entry.first < key; });

		if (it == state.Benched.end() || it->first != infantryID)
			state.Benched.insert(it, { infantryID, Unsorted::CurrentFrame + UrbanBenchFrames });
	}

	NeuralUrbanEntering* FindUrbanEntering(NeuralUrbanState& state, DWORD id)
	{
		auto const it = std::lower_bound(state.Entering.begin(), state.Entering.end(), id,
			[](const NeuralUrbanEntering& entering, DWORD key) { return entering.BuildingID < key; });

		return (it != state.Entering.end() && it->BuildingID == id) ? &*it : nullptr;
	}

	// Does any weapon of this type kill the occupants of a building it shoots at (Phobos
	// PenetratesGarrison)? PenetratesBunker is no help: it only hits units inside tank bunkers.
	bool HasGarrisonPenetratingWeapon(TechnoTypeClass* pType)
	{
		if (!pType)
			return false;

		for (int i = 0; i < 2; ++i)
		{
			for (auto const pWS : { pType->GetWeapon(i), pType->GetEliteWeapon(i) })
			{
				auto const pWH = (pWS && pWS->WeaponType) ? pWS->WeaponType->Warhead : nullptr;
				auto const pWHExt = pWH ? WarheadTypeExt::TryFetch(pWH) : nullptr;

				if (pWHExt && pWHExt->PenetratesGarrison)
					return true;
			}
		}

		return false;
	}

	// Can this soldier empty an enemy garrison without destroying the building: an Assaulter walks
	// in and kills the occupants, a PenetratesGarrison weapon shoots through the walls.
	bool IsGarrisonClearerType(TechnoTypeClass* pType)
	{
		if (!pType)
			return false;

		if (pType->WhatAmI() == AbstractType::InfantryType && static_cast<InfantryTypeClass*>(pType)->Assaulter)
			return true;

		return HasGarrisonPenetratingWeapon(pType);
	}

	bool IsGarrisonOccupierType(const InfantryTypeClass* pType)
	{
		return pType && pType->Occupier && !pType->Engineer && !IsSpyInfantryType(pType);
	}

	// Firepower of a soldier inside a building: its OccupyWeapon when it has one, else its primary,
	// as damage per 60 frames.
	int GarrisonFirepowerOf(InfantryTypeClass* pType)
	{
		if (!pType)
			return 0;

		auto pWeapon = pType->OccupyWeapon.WeaponType;

		if (!pWeapon && pType->GetWeapon(0))
			pWeapon = pType->GetWeapon(0)->WeaponType;

		if (!pWeapon || pWeapon->Damage <= 0)
			return 0;

		return pWeapon->Damage * 60 / std::max(10, pWeapon->ROF);
	}

	// Value of holding this building, from what it covers: our base or outposts, the way the
	// enemy comes, techs and refineries near it. Zero when it is deep in enemy fire, about to fall
	// or out of our reach: beyond 28 cells of our structures only where our own army already
	// stands (the front, up to 72 cells; a lone soldier walking to the middle of the map holds
	// nothing). Fog-fair: enemy buildings from the known cache, enemy units in current sight only.
	int RateUrbanBuilding(HouseClass* pHouse, NeuralKnownUrbanBuilding& known, const CellStruct& home,
		int mainAttackSector, const CellStruct& nearestEnemyCell, bool asIfRepaired = false)
	{
		known.ReachDist = 999;

		if (known.OccupantOwner >= 0 && known.OccupantOwner != pHouse->ArrayIndex)
			return 0;

		if ((!asIfRepaired && known.HealthPercent < UrbanGarrisonMinHealthPercent) || home == CellStruct::Empty)
			return 0;

		const int homeDist = static_cast<int>(home.DistanceFrom(known.Cell));
		int anchorDist = homeDist;
		// Our own garrisons hold the ground around them too: a city is taken block by block from the
		// buildings already held (10 cells further than from a structure).
		int reachDist = homeDist;

		for (auto const pBld : pHouse->Buildings)
		{
			if (!pBld || !pBld->IsAlive || pBld->InLimbo || !pBld->Type)
				continue;

			const int dist = static_cast<int>(pBld->GetMapCoords().DistanceFrom(known.Cell));

			if (!pBld->Type->CanBeOccupied)
			{
				anchorDist = std::min(anchorDist, dist);
				reachDist = std::min(reachDist, dist);
			}
			else if (pBld->Occupants.Count > 0 && pBld->UniqueID != known.ID)
			{
				reachDist = std::min(reachDist, dist + 10);
			}
		}

		int techBonus = 0;

		for (auto const& tech : GetSpecOpsState(pHouse).Techs)
		{
			// A captured structure of ours is guarded by the garrison next to it (user).
			if ((tech.OwnerIndex < 0 || tech.OwnerIndex == pHouse->ArrayIndex) && tech.Cell.DistanceFrom(known.Cell) <= 9.0)
				techBonus = std::max(techBonus, tech.OwnerIndex == pHouse->ArrayIndex ? 120 : 60);
		}

		known.ReachDist = reachDist;

		if (std::min(anchorDist, reachDist) > 72 || IsCellInKnownEnemyDefenseReach(pHouse, known.Cell, 1))
			return 0;

		// A full garrison is a free bunker: on an enemy supply line (an enemy harvester seen within 7
		// cells in the last 4500 frames) it is worth holding further out too.
		const bool onEnemySupplyLine = Unsorted::CurrentFrame - known.EnemySupplySeenFrame <= 4500;

		if (reachDist > UrbanReachCells && !onEnemySupplyLine && OwnDefenseNear(pHouse, known.Cell, 8) < 300)
			return 0;

		int enemyDist = 999;

		for (auto const& info : GetKnownEnemyBuildingCache(pHouse).Entries)
			enemyDist = std::min(enemyDist, static_cast<int>(info.Coords.DistanceFrom(known.Cell)));

		if (enemyDist <= 9)
			return 0;

		int worth = 20 * std::clamp(known.MaxOccupants, 1, 6) + techBonus;

		if (anchorDist <= 16)
			worth += 80;
		else if (anchorDist <= 28)
			worth += 40;

		if (mainAttackSector >= 0 && homeDist <= 32 && AttackSectorOf(home, known.Cell) == mainAttackSector)
			worth += 80;

		// On the way to the nearest known enemy building, nearer to us than to them.
		if (nearestEnemyCell != CellStruct::Empty)
		{
			const int span = static_cast<int>(home.DistanceFrom(nearestEnemyCell));

			if (span > 0 && (homeDist + enemyDist) * 10 <= span * 12 && homeDist * 10 <= span * 6)
				worth += 60;
		}

		for (auto const pBld : pHouse->Buildings)
		{
			if (pBld && pBld->IsAlive && pBld->Type && pBld->Type->Refinery && pBld->GetMapCoords().DistanceFrom(known.Cell) <= 8.0)
			{
				worth += 40;
				break;
			}
		}

		if (onEnemySupplyLine)
			worth += 80;

		if (VisibleEnemyThreatNear(pHouse, known.Cell, 6) > 0)
			worth /= 2;

		return worth;
	}

	// Refresh the memory of occupiable buildings from what is in sight now (every 150 frames).
	// Fog-fair: a building in the fog keeps the occupants and health it had when last seen.
	void UpdateUrbanMemory(HouseClass* pHouse, NeuralUrbanState& state)
	{
		const int now = Unsorted::CurrentFrame;
		std::vector<DWORD> inSightIDs;

		// The civilian buildings a map starts with are map knowledge (MapKnowledgeFrames): where
		// they stand is known from the first frames, as the map has them. Who went in since, or
		// whether they still stand, is learned only by seeing them.
		const bool mapKnowledge = !state.MapBuildingsKnown && now <= MapKnowledgeFrames;
		state.MapBuildingsKnown = true;

		for (auto const pBld : BuildingClass::Array)
		{
			if (!pBld || pBld->InLimbo || !pBld->IsAlive || !pBld->Type || !pBld->Type->CanBeOccupied
				|| pBld->Type->InvisibleInGame)
			{
				continue;
			}

			const CellStruct cell = pBld->GetMapCoords();
			const bool ownOrAllied = pBld->Owner && (pBld->Owner == pHouse || pHouse->IsAlliedWith(pBld->Owner));
			const bool inSight = ownOrAllied || IsCellInSightOfAI(pHouse, cell);

			if (!inSight && !(mapKnowledge && pBld->Owner && pBld->Owner->IsNeutral()))
				continue;

			if (inSight)
				inSightIDs.push_back(pBld->UniqueID);
			NeuralKnownUrbanBuilding* pKnown = FindKnownUrbanBuilding(state, pBld->UniqueID);

			if (!pKnown)
			{
				NeuralKnownUrbanBuilding known;
				known.ID = pBld->UniqueID;
				known.Cell = cell;
				known.MaxOccupants = std::clamp(pBld->Type->MaxNumberOccupants, 1, 10);

				auto const it = std::lower_bound(state.Buildings.begin(), state.Buildings.end(), known.ID,
					[](const NeuralKnownUrbanBuilding& entry, DWORD key) { return entry.ID < key; });
				pKnown = &*state.Buildings.insert(it, known);
			}

			auto const pFirst = pBld->Occupants.Count > 0 ? pBld->Occupants.GetItem(0) : nullptr;
			pKnown->Occupants = pBld->Occupants.Count;
			pKnown->OccupantOwner = (pFirst && pFirst->Owner) ? pFirst->Owner->ArrayIndex : -1;
			pKnown->HealthPercent = static_cast<int>(pBld->GetHealthPercentage() * 100.0);
			pKnown->SeenFrame = now;
		}

		// A remembered building whose cell is in sight but that is gone was destroyed.
		std::sort(inSightIDs.begin(), inSightIDs.end());
		state.Buildings.erase(std::remove_if(state.Buildings.begin(), state.Buildings.end(),
			[&](const NeuralKnownUrbanBuilding& known)
			{
				return IsCellInSightOfAI(pHouse, known.Cell) && !std::binary_search(inSightIDs.begin(), inSightIDs.end(), known.ID);
			}), state.Buildings.end());
	}

	// Is an enemy that kills occupants (Assaulter, PenetratesGarrison weapon) in sight within its
	// reach (+ 2 cells) of this building?
	bool IsGarrisonThreatenedByClearer(HouseClass* pHouse, BuildingClass* pBld)
	{
		const CellStruct cell = pBld->GetMapCoords();
		bool threatened = false;

		ForEachTechnoNear(cell, 10, [&](TechnoClass* pTech) -> bool
		{
			if (!pTech->Owner || pTech->Owner == pHouse || pHouse->IsAlliedWith(pTech->Owner) || pTech->Owner->IsNeutral()
				|| pTech->WhatAmI() == AbstractType::Building || !IsCellInSightOfAI(pHouse, pTech->GetMapCoords()))
			{
				return true;
			}

			auto const pType = pTech->GetTechnoType();

			if (!IsGarrisonClearerType(pType))
				return true;

			const int reach = std::max(GroundRangeOf(pType), Unsorted::LeptonsPerCell) + 2 * Unsorted::LeptonsPerCell;

			if (pTech->DistanceFrom(pBld) <= reach)
			{
				threatened = true;
				return false;
			}

			return true;
		});

		return threatened;
	}

	// Per AI tick: memory and opportunity (every 150 frames), who is walking into which building,
	// allied tech claims, evacuations.
	void UpdateUrban(HouseClass* pHouse)
	{
		if (!pHouse || pHouse->ArrayIndex < 0 || pHouse->ArrayIndex >= 32)
			return;

		auto& state = GetUrbanState(pHouse);
		const int now = Unsorted::CurrentFrame;
		const CellStruct home = GetHouseHomeCell(pHouse);

		// Who walks into which occupiable building now, and which techs allied engineers go for.
		state.Entering.clear();
		state.AllyTechClaims.clear();

		auto const noteEntering = [&state](DWORD buildingID, bool own)
		{
			auto pEntering = FindUrbanEntering(state, buildingID);

			if (!pEntering)
			{
				NeuralUrbanEntering entering;
				entering.BuildingID = buildingID;

				auto const it = std::lower_bound(state.Entering.begin(), state.Entering.end(), entering.BuildingID,
					[](const NeuralUrbanEntering& entry, DWORD key) { return entry.BuildingID < key; });
				pEntering = &*state.Entering.insert(it, entering);
			}

			if (own)
				pEntering->Own++;
			else
				pEntering->Allied++;
		};

		for (auto const pInf : InfantryClass::Array)
		{
			if (!pInf || !pInf->IsAlive || pInf->InLimbo || !pInf->Owner || !pInf->Type
				|| (pInf->Owner != pHouse && !pHouse->IsAlliedWith(pInf->Owner)))
			{
				continue;
			}

			// A soldier of ours still walking to the building it was ordered into (a far one walks to
			// a cell next to it first) is on its way in too.
			if (pInf->Owner == pHouse && pInf->CurrentMission == Mission::Move)
			{
				auto const itOrder = std::lower_bound(state.Orders.begin(), state.Orders.end(), pInf->UniqueID,
					[](const NeuralUrbanOrder& order, DWORD key) { return order.InfantryID < key; });

				if (itOrder != state.Orders.end() && itOrder->InfantryID == pInf->UniqueID && now >= itOrder->Frame
					&& now - itOrder->Frame <= 900)
				{
					noteEntering(itOrder->BuildingID, true);

					continue;
				}
			}

			if (pInf->CurrentMission != Mission::Enter && pInf->QueuedMission != Mission::Enter
				&& pInf->CurrentMission != Mission::Capture)
			{
				continue;
			}

			auto pGoal = abstract_cast<BuildingClass*>(pInf->Destination);

			if (!pGoal)
				pGoal = abstract_cast<BuildingClass*>(pInf->ArchiveTarget);

			if (!pGoal && pInf->Target)
				pGoal = abstract_cast<BuildingClass*>(pInf->Target);

			if (!pGoal || !pGoal->Type)
				continue;

			if (pInf->Owner != pHouse && pInf->Type->Engineer && pGoal->Type->Capturable)
				state.AllyTechClaims.push_back(pGoal->UniqueID);

			if (!pGoal->Type->CanBeOccupied)
				continue;

			// A soldier of ours that stays on its capture order cannot get in (it is shut in, or the
			// way is cut), and the engine looks for its path again every frame: it is called off and
			// left out of the garrisons for a while.
			if (pInf->Owner == pHouse && pInf->CurrentMission == Mission::Capture && IsGarrisonOccupierType(pInf->Type))
			{
				auto const itOrder = std::lower_bound(state.Orders.begin(), state.Orders.end(), pInf->UniqueID,
					[](const NeuralUrbanOrder& order, DWORD key) { return order.InfantryID < key; });
				const bool ordered = itOrder != state.Orders.end() && itOrder->InfantryID == pInf->UniqueID && now >= itOrder->Frame;

				if (!ordered)
				{
					const int cells = pInf->DistanceFrom(pGoal) / Unsorted::LeptonsPerCell;
					state.Orders.insert(itOrder, NeuralUrbanOrder { pInf->UniqueID, pGoal->UniqueID, now, 1, cells });
				}
				else if (now - itOrder->Frame > UrbanCaptureOrderFrames)
				{
					state.Orders.erase(itOrder);
					BenchUrbanSoldier(state, pInf->UniqueID);
					pInf->SetTarget(nullptr);
					pInf->SetDestination(nullptr, true);
					pInf->QueueMission(Mission::Guard, true);

					continue;
				}
			}

			noteEntering(pGoal->UniqueID, pInf->Owner == pHouse);
		}

		// Captures an allied Neural AI has picked but whose engineer is not walking yet.
		for (auto const pAlly : HouseClass::Array)
		{
			if (!pAlly || pAlly == pHouse || pAlly->Defeated || !pHouse->IsAlliedWith(pAlly) || pAlly->ArrayIndex < 0
				|| pAlly->ArrayIndex >= 32 || pAlly->IsNeutral())
			{
				continue;
			}

			for (auto const& capture : GetSpecOpsState(pAlly).Captures)
				state.AllyTechClaims.push_back(capture.TargetID);
		}

		std::sort(state.AllyTechClaims.begin(), state.AllyTechClaims.end());

		// Our garrisons: an enemy that kills occupants is close, or the building is about to fall
		// under fire: everybody out before it is too late. The engine itself only empties a
		// building at red health, often with the occupants already dead.
		state.OwnGarrisons = 0;
		state.OwnOccupants = 0;
		state.GarrisonsInDanger = 0;
		state.BunkersToMan = 0;

		for (auto const pBld : pHouse->Buildings)
		{
			if (!pBld || pBld->InLimbo || !pBld->IsAlive || !pBld->Type || !pBld->Type->CanBeOccupied)
				continue;

			const int maxOccupants = std::clamp(pBld->Type->MaxNumberOccupants, 1, 10);

			if (pBld->Occupants.Count <= 0)
			{
				if (pBld->Type->IsBaseDefense)
					state.BunkersToMan++;

				continue;
			}

			if (pBld->Type->IsBaseDefense && pBld->Occupants.Count < maxOccupants)
				state.BunkersToMan++;

			state.OwnGarrisons++;
			state.OwnOccupants += pBld->Occupants.Count;

			auto const pKnown = FindKnownUrbanBuilding(state, pBld->UniqueID);

			if (pKnown && now < pKnown->BlockedUntilFrame)
				continue;

			const bool clearerNear = IsGarrisonThreatenedByClearer(pHouse, pBld);
			const bool falling = pBld->GetHealthPercentage() < 0.40 && VisibleEnemyThreatNear(pHouse, pBld->GetMapCoords(), 8) > 0;

			if (!clearerNear && !falling)
				continue;

			state.GarrisonsInDanger++;

			if (pBld->CurrentMission != Mission::Unload)
			{
				pBld->QueueMission(Mission::Unload, false);
				state.Evacuations++;
				CountDecision(NeuralDecision::UrbanEvacuate, pHouse->ArrayIndex);
				Debug::Log("[NeuralAI] House %d Urban: evacuate %s at (%d,%d), %d inside, %s, frame %d.\n",
					pHouse->ArrayIndex, pBld->Type->ID, pBld->GetMapCoords().X, pBld->GetMapCoords().Y,
					pBld->Occupants.Count, clearerNear ? "clearer near" : "falling", now);
			}

			if (pKnown)
				pKnown->BlockedUntilFrame = now + 1200;
		}

		state.PeakGarrisons = std::max(state.PeakGarrisons, state.OwnGarrisons);
		state.PeakOccupants = std::max(state.PeakOccupants, state.OwnOccupants);

		if (now - state.LastScanFrame < 150 && now >= state.LastScanFrame)
			return;

		state.LastScanFrame = now;
		UpdateUrbanMemory(pHouse, state);
		state.LiveFrame = -1;

		// Who can garrison or clear: owned now or trainable.
		state.OccupierAvailable = false;
		state.ClearerAvailable = false;
		state.OwnOccupierInfantry = state.OwnOccupants;

		for (auto const pInf : InfantryClass::Array)
		{
			if (pInf && pInf->Owner == pHouse && pInf->IsAlive && !pInf->InLimbo && IsGarrisonOccupierType(pInf->Type))
				state.OwnOccupierInfantry++;
		}

		for (auto const pType : InfantryTypeClass::Array)
		{
			if (!pType || (!IsGarrisonOccupierType(pType) && !IsGarrisonClearerType(pType)))
				continue;

			if (pHouse->CountOwnedNow(pType) <= 0 && !CanBuildUnderHumanRules(pHouse, pType))
				continue;

			if (IsGarrisonOccupierType(pType))
				state.OccupierAvailable = true;

			if (IsGarrisonClearerType(pType))
				state.ClearerAvailable = true;
		}

		if (!state.ClearerAvailable)
		{
			for (auto const pUnit : UnitClass::Array)
			{
				if (pUnit && pUnit->Owner == pHouse && pUnit->IsAlive && !pUnit->InLimbo && HasGarrisonPenetratingWeapon(pUnit->Type))
				{
					state.ClearerAvailable = true;
					break;
				}
			}
		}

		// Nearest known enemy building to home: the way the enemy comes when no attack side stands out.
		CellStruct nearestEnemyCell = CellStruct::Empty;
		int nearestEnemyDist = 999;

		if (home != CellStruct::Empty)
		{
			for (auto const& info : GetKnownEnemyBuildingCache(pHouse).Entries)
			{
				const int dist = static_cast<int>(home.DistanceFrom(info.Coords));

				if (dist < nearestEnemyDist)
				{
					nearestEnemyDist = dist;
					nearestEnemyCell = info.Coords;
				}
			}
		}

		// Enemy supply lines, fog-fair: enemy harvesters in current sight mark the buildings within 7
		// cells of them.
		for (auto const pUnit : UnitClass::Array)
		{
			if (!pUnit || !pUnit->IsAlive || pUnit->InLimbo || !pUnit->Type || !pUnit->Owner || pUnit->Owner == pHouse
				|| pHouse->IsAlliedWith(pUnit->Owner) || pUnit->Owner->IsNeutral() || !IsCreditEconomicType(pUnit->Type)
				|| !pUnit->Type->ResourceGatherer)
			{
				continue;
			}

			const CellStruct harvCell = pUnit->GetMapCoords();

			if (!IsCellInSightOfAI(pHouse, harvCell))
				continue;

			for (auto& known : state.Buildings)
			{
				if (known.Cell.DistanceFrom(harvCell) <= 7.0)
					known.EnemySupplySeenFrame = now;
			}
		}

		const int mainAttackSector = GetMainAttackSector(pHouse);
		std::array<int, 3> bestWorth {};
		state.ThreatGarrisons = 0;
		state.WorthyBuildings = 0;
		state.WorthySeats = 0;
		state.WorthyFreeSeats = 0;

		for (auto& known : state.Buildings)
		{
			known.Worth = RateUrbanBuilding(pHouse, known, home, mainAttackSector, nearestEnemyCell);
			known.WorthIfRepaired = known.HealthPercent < UrbanGarrisonMinHealthPercent
				? RateUrbanBuilding(pHouse, known, home, mainAttackSector, nearestEnemyCell, true)
				: known.Worth;

			auto const pOccupantOwner = (known.OccupantOwner >= 0 && known.OccupantOwner < HouseClass::Array.Count)
				? HouseClass::Array.GetItem(known.OccupantOwner)
				: nullptr;
			const bool enemyHeld = pOccupantOwner && pOccupantOwner != pHouse && !pHouse->IsAlliedWith(pOccupantOwner);

			// Enemy garrisons that matter: seen in the last 300 frames and covering our base (30
			// cells) or one of our buildings (10 cells).
			if (enemyHeld && now - known.SeenFrame <= 300 && home != CellStruct::Empty)
			{
				bool nearOurs = home.DistanceFrom(known.Cell) <= 30.0;

				for (auto const pBld : pHouse->Buildings)
				{
					if (nearOurs)
						break;

					if (pBld && pBld->IsAlive && !pBld->InLimbo && pBld->GetMapCoords().DistanceFrom(known.Cell) <= 10.0)
						nearOurs = true;
				}

				if (nearOurs)
					state.ThreatGarrisons++;
			}

			const int room = known.MaxOccupants - known.Occupants;

			if (known.Worth > 0)
			{
				state.WorthyBuildings++;
				state.WorthySeats += known.MaxOccupants;
				state.WorthyFreeSeats += std::max(0, room);
			}

			if (known.Worth <= 0 || room <= 0 || now < known.BlockedUntilFrame)
				continue;

			auto const pWorst = std::min_element(bestWorth.begin(), bestWorth.end());

			if (known.Worth > *pWorst)
				*pWorst = known.Worth;
		}

		state.GarrisonWorth = bestWorth[0] + bestWorth[1] + bestWorth[2];

		// Coalition: allies in the match, their critical buildings under attack, their attacks under way.
		state.Allies = 0;
		state.AllyRescue = 0;
		state.AllyAttackPressure = 0;

		for (auto const pAlly : HouseClass::Array)
		{
			if (pAlly && pAlly != pHouse && !pAlly->Defeated && !pAlly->IsNeutral() && !pAlly->IsObserver()
				&& pHouse->IsAlliedWith(pAlly))
			{
				state.Allies++;
			}
		}

		if (state.Allies > 0)
		{
			for (auto const pBld : BuildingClass::Array)
			{
				if (!pBld || pBld->InLimbo || !pBld->IsAlive || !pBld->Owner || pBld->Owner == pHouse || !pBld->Type
					|| !pHouse->IsAlliedWith(pBld->Owner) || pBld->Owner->IsNeutral())
				{
					continue;
				}

				if ((pBld->Type->ConstructionYard || pBld->Type->Factory != AbstractType::None || pBld->Type->SuperWeapon != -1)
					&& pBld->HasBeenAttacked && pBld->GetHealthPercentage() < 0.85
					&& VisibleEnemyThreatNear(pHouse, pBld->GetMapCoords(), 8) > 0)
				{
					state.AllyRescue++;
				}
			}

			for (auto const pFoot : FootClass::Array)
			{
				if (!pFoot || !pFoot->IsAlive || pFoot->InLimbo || !pFoot->Owner || pFoot->Owner == pHouse
					|| !pHouse->IsAlliedWith(pFoot->Owner) || pFoot->Owner->IsNeutral()
					|| pFoot->WhatAmI() == AbstractType::Aircraft)
				{
					continue;
				}

				auto const pTargetBld = abstract_cast<BuildingClass*>(pFoot->Target);

				if (pTargetBld && pTargetBld->Owner && !pHouse->IsAlliedWith(pTargetBld->Owner) && !pTargetBld->Owner->IsNeutral()
					&& IsEnemyBuildingKnown(pHouse, pTargetBld))
				{
					state.AllyAttackPressure++;
				}
			}
		}

		// Selector prior of Expert 9: real urban or coalition work right now.
		int opportunity = 0;

		if (state.OccupierAvailable)
			opportunity += std::min(400, state.GarrisonWorth / 2);

		opportunity += std::min(330, state.ThreatGarrisons * (state.ClearerAvailable ? 110 : 60));
		opportunity += std::min(160, state.BunkersToMan * 80);
		opportunity += std::min(180, state.GarrisonsInDanger * 90);
		opportunity += std::min(80, state.OwnGarrisons * 20);
		opportunity += state.Allies > 0 ? 40 : 0;
		opportunity += state.AllyRescue > 0 ? 180 : 0;
		opportunity += state.AllyAttackPressure >= 4 ? 120 : 0;
		state.Opportunity = std::min(720, opportunity);

		if (now - state.LastReportFrame >= 1800)
		{
			state.LastReportFrame = now;
			Debug::Log("[NeuralAI] House %d Urban: %d garrisons, %d occupants (peak %d/%d), known %d (worthy %d), worth %d, enemy garrisons %d, "
				"allies %d, opportunity %d, orders %d, evacuations %d, clears %d; occupiers %d, tries %d (over share %d, no building %d, squads %d, too few %d), "
				"seats %d (free %d), frame %d.\n",
				pHouse->ArrayIndex, state.OwnGarrisons, state.OwnOccupants, state.PeakGarrisons, state.PeakOccupants,
				static_cast<int>(state.Buildings.size()), state.WorthyBuildings, state.GarrisonWorth, state.ThreatGarrisons, state.Allies,
				state.Opportunity, state.GarrisonOrders, state.Evacuations, state.ClearOrders, state.OwnOccupierInfantry,
				state.TriesSinceReport, state.OverCapSinceReport, state.NoBuildingSinceReport, state.SquadsSinceReport,
				state.SmallSquadSinceReport, state.WorthySeats, state.WorthyFreeSeats, now);
			state.TriesSinceReport = 0;
			state.SquadsSinceReport = 0;
			state.SmallSquadSinceReport = 0;
			state.OverCapSinceReport = 0;
			state.NoBuildingSinceReport = 0;
		}
	}

	int UrbanOpportunity(HouseClass* pHouse)
	{
		return GetUrbanState(pHouse).Opportunity;
	}

	// An urban map for this house: 6 or more known buildings worth holding.
	bool IsUrbanMapFor(HouseClass* pHouse)
	{
		return GetUrbanState(pHouse).WorthyBuildings >= 6;
	}

	// Is this tech building claimed by an ally (its engineer walks in or its capture is planned)?
	bool IsTechClaimedByAlly(HouseClass* pHouse, DWORD buildingID)
	{
		auto const& claims = GetUrbanState(pHouse).AllyTechClaims;

		return std::binary_search(claims.begin(), claims.end(), buildingID);
	}

	// Garrison demand for the infantry selector: seats left in buildings worth holding beyond the
	// occupiers we have outside (0 when nothing is worth it), and whether an enemy garrison near us
	// waits for a clearer we do not own. On an urban map every seat counts (the city is filled);
	// elsewhere 12 at most, the squad for the best positions.
	void GetUrbanProductionNeeds(HouseClass* pHouse, int& outGarrisonRoom, bool& outNeedClearer)
	{
		auto& state = GetUrbanState(pHouse);
		const bool urbanMap = state.WorthyBuildings >= 6;
		int room = 0;

		for (auto const& known : state.Buildings)
		{
			if (known.Worth >= (urbanMap ? 1 : 100) && Unsorted::CurrentFrame >= known.BlockedUntilFrame)
				room += std::max(0, known.MaxOccupants - known.Occupants);
		}

		outGarrisonRoom = std::max(0, std::min(room, urbanMap ? UrbanCitySeats : UrbanSquadSeats)
			- std::max(0, state.OwnOccupierInfantry - state.OwnOccupants));
		outNeedClearer = state.ThreatGarrisons > 0;

		if (outNeedClearer)
		{
			for (auto const pInf : InfantryClass::Array)
			{
				if (pInf && pInf->Owner == pHouse && pInf->IsAlive && !pInf->InLimbo && IsGarrisonClearerType(pInf->Type))
				{
					outNeedClearer = false;
					break;
				}
			}
		}
	}

	// Live pointers of the remembered buildings for this frame (one pass over the building array
	// instead of one search per candidate and soldier).
	void RefreshUrbanLivePointers(NeuralUrbanState& state)
	{
		if (state.LiveFrame == Unsorted::CurrentFrame && state.Live.size() == state.Buildings.size())
			return;

		state.LiveFrame = Unsorted::CurrentFrame;
		state.Live.assign(state.Buildings.size(), nullptr);

		for (auto const pBld : BuildingClass::Array)
		{
			if (!pBld || !pBld->IsAlive || pBld->InLimbo || !pBld->Type || !pBld->Type->CanBeOccupied)
				continue;

			auto const it = std::lower_bound(state.Buildings.begin(), state.Buildings.end(), pBld->UniqueID,
				[](const NeuralKnownUrbanBuilding& known, DWORD key) { return known.ID < key; });

			if (it != state.Buildings.end() && it->ID == pBld->UniqueID)
				state.Live[static_cast<size_t>(it - state.Buildings.begin())] = pBld;
		}
	}

	// Garrison order for one idle occupier soldier: the best known building worth holding within
	// reach (up to 64 cells while Expert 9 is in charge, else rangeCells), with room
	// left after the soldiers (ours and allied) already walking in.
	// No distance limit (user: reachable ground is what counts, not how far): the building must be
	// enterable from the soldier's zone and worth holding (RateUrbanBuilding already drops what
	// covers nothing of ours); distance only orders the choice (rangeCells, the slot 23 range,
	// sets how fast the score falls with it; 16 under Expert 9).
	// A full garrison is a free bunker (user): buildings we hold or already walk into are filled
	// first, a new building is started only while none of those within reach has room. Starting new
	// ones stops at a share of our occupiers inside: 60% under Expert 9, 40% otherwise. An urban map
	// (6+ known buildings worth holding) has no share (user: a city is filled with soldiers): every
	// idle occupier goes in while a building worth holding has room, and the barracks train the
	// soldiers for the seats left (GetUrbanProductionNeeds). Filling a building we hold ignores the
	// share anywhere.
	// A building is taken by a squad (user: many soldiers at once, not one or two; once it is full
	// the next building, the rest back to the pool): the soldier picks the building and the idle
	// occupiers within 20 cells of it go along, as many as there is room for. A building out of the
	// reach of our structures and garrisons (28 cells) that nobody of ours holds or walks into needs
	// a squad of 3 (or its room when smaller), else the soldiers stay free for other missions; within
	// that reach one soldier starts it and the next ones fill it. Soldiers that find it full are idle
	// again and go for the next one.
	// isBusy: the caller's test for a soldier already attacking or heading to the enemy.
	template <typename DispatchFn, typename BusyFn>
	bool RunUrbanGarrison(HouseClass* pHouse, InfantryClass* pInf, bool expertActive, int rangeCells, DispatchFn&& dispatch,
		BusyFn&& isBusy)
	{
		if (!IsGarrisonOccupierType(pInf->Type) || pInf->GetHealthPercentage() < 0.50)
			return false;

		auto& state = GetUrbanState(pHouse);
		int pendingTotal = 0;

		for (auto const& entering : state.Entering)
			pendingTotal += entering.Own;

		const bool urbanMap = state.WorthyBuildings >= 6;
		const int sharePercent = expertActive ? 60 : 40;
		const int cap = std::max(expertActive ? 4 : 2, state.OwnOccupierInfantry * sharePercent / 100);
		const bool overCap = !urbanMap && state.OwnOccupants + pendingTotal >= cap;
		state.TriesSinceReport++;

		if (overCap)
			state.OverCapSinceReport++;

		const int now = Unsorted::CurrentFrame;
		auto const itBenched = std::lower_bound(state.Benched.begin(), state.Benched.end(), pInf->UniqueID,
			[](const std::pair<DWORD, int>& entry, DWORD key) { return entry.first < key; });

		if (itBenched != state.Benched.end() && itBenched->first == pInf->UniqueID)
		{
			if (now < itBenched->second)
				return false;

			state.Benched.erase(itBenched);
		}

		// The soldier's own ground: its zones link what it can walk, a vehicle's do not (a city block
		// only soldiers get into reads as another island).
		const MovementZone moveZone = pInf->Type->MovementZone;
		const int infZone = ObjectZone(pInf, moveZone);

		if (state.NoBuildingFrame != Unsorted::CurrentFrame)
		{
			state.NoBuildingFrame = Unsorted::CurrentFrame;
			state.NoBuildingZones.clear();
		}
		else if (std::find(state.NoBuildingZones.begin(), state.NoBuildingZones.end(), infZone) != state.NoBuildingZones.end())
		{
			return false;
		}

		RefreshUrbanLivePointers(state);
		const CellStruct infCell = pInf->GetMapCoords();
		// Score per cell of walk: 4 at a 10-cell slot range, less with a longer one.
		const int distanceScale = std::max(6, expertActive ? 16 : (urbanMap ? std::max(rangeCells, 16) : rangeCells));
		BuildingClass* pBest = nullptr;
		NeuralKnownUrbanBuilding* pBestKnown = nullptr;
		int bestScore = 0;
		int bestTaken = 0;
		// The best building we already hold with room left (filled first).
		BuildingClass* pBestHeld = nullptr;
		NeuralKnownUrbanBuilding* pBestHeldKnown = nullptr;
		int bestHeldScore = 0;
		int bestHeldTaken = 0;

		for (size_t i = 0; i < state.Buildings.size(); ++i)
		{
			auto& known = state.Buildings[i];

			if (known.Worth <= 0 || now < known.BlockedUntilFrame)
				continue;

			const int dist = static_cast<int>(infCell.DistanceFrom(known.Cell));
			auto const pBld = state.Live[i];

			if (!pBld || !pBld->CanBeOccupiedBy(pInf))
				continue;

			// Live occupants for our own buildings and those in sight, the remembered ones for the rest.
			const bool own = pBld->Owner == pHouse;
			const bool seenNow = own || IsCellInSightOfAI(pHouse, known.Cell);
			const int inside = seenNow ? pBld->Occupants.Count : known.Occupants;
			auto const pFirstInside = (seenNow && pBld->Occupants.Count > 0) ? pBld->Occupants.GetItem(0) : nullptr;

			if (pFirstInside && pFirstInside->Owner != pHouse)
				continue;

			auto const pEntering = FindUrbanEntering(state, known.ID);
			const int coming = pEntering ? pEntering->Own + pEntering->Allied : 0;

			if (!own && known.OccupantOwner >= 0)
				continue;

			if (pEntering && pEntering->Allied > 0 && !own)
				continue;

			if (inside + coming >= known.MaxOccupants)
				continue;

			// Ours already: soldiers of ours inside or on their way in. Over the share only such a
			// building is still filled up.
			const bool partlyOurs = (own && inside > 0) || (pEntering && pEntering->Own > 0);

			if (overCap && !partlyOurs)
				continue;

			if (infZone >= 0 && !IsBuildingEnterableFromZone(pBld, infZone, moveZone))
				continue;

			const int score = known.Worth * 100 / (100 + dist * 40 / distanceScale) * (partlyOurs ? 2 : 1);

			if (partlyOurs && score > bestHeldScore)
			{
				bestHeldScore = score;
				pBestHeld = pBld;
				pBestHeldKnown = &known;
				bestHeldTaken = inside + coming;
			}

			if (score > bestScore)
			{
				bestScore = score;
				pBest = pBld;
				pBestKnown = &known;
				bestTaken = inside + coming;
			}
		}

		// Fill what we hold before starting another building.
		if (pBestHeld)
		{
			pBest = pBestHeld;
			pBestKnown = pBestHeldKnown;
			bestTaken = bestHeldTaken;
		}

		if (!pBest)
		{
			// Soldiers ready for a garrison and no building to take: the best one within their reach
			// that only its damage keeps them out of is the one an engineer repairs (user: never a
			// repair for its own sake, only for a building we are about to man).
			if (!overCap)
			{
				int bestRepairWorth = 0;

				for (size_t i = 0; i < state.Buildings.size(); ++i)
				{
					auto const& known = state.Buildings[i];
					auto const pBld = state.Live[i];

					if (pBld && known.Worth <= 0 && known.WorthIfRepaired > bestRepairWorth && known.OccupantOwner < 0
						&& known.HealthPercent < UrbanGarrisonMinHealthPercent && now >= known.BlockedUntilFrame
						&& infCell.DistanceFrom(known.Cell) <= UrbanRepairReachCells && IsBuildingEnterableFromZone(pBld, infZone, moveZone))
					{
						bestRepairWorth = known.WorthIfRepaired;
						state.RepairWantedID = known.ID;
						state.RepairWantedFrame = now;
					}
				}
			}

			state.NoBuildingSinceReport++;
			state.NoBuildingZones.push_back(infZone);
			return false;
		}

		// The squad: this soldier and the idle occupiers near it, nearest first (unique ID on a tie),
		// up to the room left in the building.
		const int room = pBestKnown->MaxOccupants - bestTaken;
		std::vector<std::pair<int, InfantryClass*>> mates;

		if (room > 1)
		{
			for (auto const pMate : InfantryClass::Array)
			{
				if (!pMate || pMate == pInf || pMate->Owner != pHouse || !pMate->IsAlive || pMate->InLimbo || !pMate->Type
					|| !IsGarrisonOccupierType(pMate->Type) || pMate->GetHealthPercentage() < 0.50)
				{
					continue;
				}

				// Idle as the caller sees it: guarding, or a finished Move.
				const bool mateMoveOver = pMate->CurrentMission == Mission::Move && pMate->MegaMission != Mission::AttackMove
					&& (!pMate->Destination
						|| CellClass::Coord2Cell(pMate->Destination->GetCoords()).DistanceFrom(pMate->GetMapCoords()) <= 1.5);

				if ((pMate->CurrentMission != Mission::Guard && pMate->CurrentMission != Mission::Area_Guard && !mateMoveOver)
					|| isBusy(pMate) || IsPlanMember(pHouse, pMate->UniqueID) || IsOnRepairTrip(pHouse, pMate->UniqueID))
				{
					continue;
				}

				const int dist = pMate->DistanceFrom(pInf);

				if (dist > 20 * Unsorted::LeptonsPerCell || !pBest->CanBeOccupiedBy(pMate)
					|| (infZone >= 0 && ObjectZone(pMate, moveZone) != infZone))
				{
					continue;
				}

				mates.emplace_back(dist, pMate);
			}

			std::sort(mates.begin(), mates.end(), [](const auto& a, const auto& b)
				{ return a.first != b.first ? a.first < b.first : a.second->UniqueID < b.second->UniqueID; });

			if (static_cast<int>(mates.size()) > room - 1)
				mates.resize(static_cast<size_t>(room - 1));
		}

		if (bestTaken == 0 && pBestKnown->ReachDist > UrbanReachCells && static_cast<int>(mates.size()) + 1 < std::min(3, room))
		{
			state.SmallSquadSinceReport++;
			return false;
		}

		state.Orders.erase(std::remove_if(state.Orders.begin(), state.Orders.end(),
			[now](const NeuralUrbanOrder& order) { return now - order.Frame > 900 || now < order.Frame; }), state.Orders.end());

		// One soldier in. Ordered to the same building three times in a row (each within 900 frames)
		// without getting nearer, it does not get in: the soldier is left out of the garrisons for a
		// while (it may be shut in somewhere), and two such soldiers block the building (the way is
		// cut, or the engine refuses). A squad walking in counts once per soldier, not as failures.
		auto const sendIn = [&](InfantryClass* pSoldier) -> bool
		{
			const int cellsNow = pSoldier->DistanceFrom(pBest) / Unsorted::LeptonsPerCell;
			auto itOrder = std::lower_bound(state.Orders.begin(), state.Orders.end(), pSoldier->UniqueID,
				[](const NeuralUrbanOrder& order, DWORD key) { return order.InfantryID < key; });

			if (itOrder != state.Orders.end() && itOrder->InfantryID == pSoldier->UniqueID)
			{
				if (itOrder->BuildingID == pBest->UniqueID)
				{
					// TEMP diagnostic: why a soldier sent in is idle again.
					static int reorderLogSerial = -1;
					static int reorderLogCount = 0;

					if (reorderLogSerial != NeuralMatchSerial)
					{
						reorderLogSerial = NeuralMatchSerial;
						reorderLogCount = 0;
					}

					if (reorderLogCount++ < 60)
					{
						Debug::Log("[NeuralAI] House %d Urban DIAG: %s %u re-sent to %s owner %d after %d frames, mission %d mega %d dest %s at %d cells (was %d), own %d, inside %d; "
							"soldier (%d,%d) vehicle zone %d own zone %d bridge %d, building (%d,%d).\n",
							pHouse->ArrayIndex, pSoldier->GetTechnoType()->ID, pSoldier->UniqueID, pBest->Type->ID, pBest->Owner ? pBest->Owner->ArrayIndex : -1,
							now - itOrder->Frame, static_cast<int>(pSoldier->CurrentMission), static_cast<int>(pSoldier->MegaMission),
							pSoldier->Destination ? (pSoldier->Destination == pBest ? "building" : "other") : "none",
							cellsNow, itOrder->Cells, pBest->Owner == pHouse ? 1 : 0, pBest->Occupants.Count,
							pSoldier->GetMapCoords().X, pSoldier->GetMapCoords().Y, ObjectZone(pSoldier, MovementZone::Normal),
							ObjectZone(pSoldier, pSoldier->Type->MovementZone), pSoldier->OnBridge ? 1 : 0, pBestKnown->Cell.X, pBestKnown->Cell.Y);
					}
				}

				// A soldier walking there from far is ordered again on the way; only an order that
				// brought it no nearer counts against it.
				const bool noNearer = itOrder->BuildingID == pBest->UniqueID && cellsNow > itOrder->Cells - 2;

				if (noNearer && ++itOrder->Tries >= 3)
				{
					state.Orders.erase(itOrder);

					BenchUrbanSoldier(state, pSoldier->UniqueID);

					if (++pBestKnown->Failures >= 2)
					{
						pBestKnown->Failures = 0;
						pBestKnown->BlockedUntilFrame = now + 3600;
						Debug::Log("[NeuralAI] House %d Urban: %s at (%d,%d) blocked, soldiers ordered in keep failing, frame %d.\n",
							pHouse->ArrayIndex, pBest->Type->ID, pBestKnown->Cell.X, pBestKnown->Cell.Y, now);
					}

					return false;
				}

				if (itOrder->BuildingID != pBest->UniqueID)
					itOrder->Tries = 1;

				itOrder->BuildingID = pBest->UniqueID;
				itOrder->Frame = now;
				itOrder->Cells = cellsNow;
			}
			else
			{
				state.Orders.insert(itOrder, NeuralUrbanOrder { pSoldier->UniqueID, pBest->UniqueID, now, 1, cellsNow });
			}

			// The garrison order of a player is the capture mission (FootClass::Mission_Capture has the
			// occupier branch). Enter is dropped by the engine: at once into a building we hold, and
			// into a neutral one too (repro: soldiers back in Guard with no destination 3-9 cells from
			// an empty bunker, re-sent until the building was blocked). Enter (through the dispatcher)
			// is kept only for a soldier that needs a ferry to get there.
			const MovementZone soldierMoveZone = pSoldier->Type->MovementZone;
			const int soldierZone = ObjectZone(pSoldier, soldierMoveZone);

			// The capture order is dropped from far away (repro: soldiers 60 cells off back in Area_Guard
			// with no destination): beyond 12 cells the soldier walks there first and gets the
			// capture order when it is idle again next to the building.
			if (soldierZone >= 0 && IsBuildingEnterableFromZone(pBest, soldierZone, soldierMoveZone)
				&& pSoldier->DistanceFrom(pBest) > 12 * Unsorted::LeptonsPerCell)
			{
				// To a free cell next to the building in the soldier's zone (a Move with the building as
				// its destination was dropped: match #7106, soldiers 54 cells off back in Area_Guard 15
				// frames later, re-sent until the building was blocked).
				const CellStruct bldCell = pBest->GetMapCoords();
				const int bw = pBest->Type->GetFoundationWidth();
				const int bh = pBest->Type->GetFoundationHeight(false);
				const CellStruct soldierCell = pSoldier->GetMapCoords();
				CellClass* pApproach = nullptr;
				double approachDist = 1e9;

				for (int dy = -1; dy <= bh; ++dy)
				{
					for (int dx = -1; dx <= bw; ++dx)
					{
						if (dx >= 0 && dx < bw && dy >= 0 && dy < bh)
							continue;

						const CellStruct c { static_cast<short>(bldCell.X + dx), static_cast<short>(bldCell.Y + dy) };
						auto const pCell = MapClass::Instance.CoordinatesLegal(c) ? MapClass::Instance.TryGetCellAt(c) : nullptr;

						if (!pCell || pCell->GetBuilding()
							|| MapClass::Instance.GetMovementZoneType(c, soldierMoveZone, false) != soldierZone)
						{
							continue;
						}

						const double d = soldierCell.DistanceFrom(c);

						if (d < approachDist)
						{
							approachDist = d;
							pApproach = pCell;
						}
					}
				}

				if (!pApproach)
					return false;

				// Started at once, like the capture order below, so the rest of this tick sees a soldier
				// on its way and not an idle one.
				pSoldier->ClearMegaMissionData();
				pSoldier->SetTarget(nullptr);
				pSoldier->SetDestination(pApproach, true);
				pSoldier->QueueMission(Mission::Move, false);
				pSoldier->NextMission();
			}
			else if (soldierZone >= 0 && IsBuildingEnterableFromZone(pBest, soldierZone, soldierMoveZone))
			{
				pSoldier->ClearMegaMissionData();
				pSoldier->SetTarget(nullptr);
				pSoldier->SetDestination(pBest, true);
				pSoldier->QueueMission(Mission::Capture, false);
				pSoldier->NextMission();
			}
			else if (!dispatch(pSoldier, pBest, Mission::Enter, false))
			{
				return false;
			}

			state.GarrisonOrders++;
			CountDecision(NeuralDecision::UrbanGarrison, pHouse->ArrayIndex);

			auto pEntering = FindUrbanEntering(state, pBest->UniqueID);

			if (!pEntering)
			{
				NeuralUrbanEntering entering;
				entering.BuildingID = pBest->UniqueID;

				auto const it = std::lower_bound(state.Entering.begin(), state.Entering.end(), entering.BuildingID,
					[](const NeuralUrbanEntering& entry, DWORD key) { return entry.BuildingID < key; });
				pEntering = &*state.Entering.insert(it, entering);
			}

			pEntering->Own++;
			return true;
		};

		if (!sendIn(pInf))
			return false;

		int sent = 1;

		for (auto const& [dist, pMate] : mates)
		{
			if (sendIn(pMate))
				++sent;
		}

		if (sent > 1)
		{
			state.SquadsSinceReport++;
			CountDecision(NeuralDecision::GarrisonSquad, pHouse->ArrayIndex);
		}

		Debug::Log("[NeuralAI] House %d Urban: garrison %s %u (+%d) -> %s at (%d,%d), worth %d, %d/%d inside, frame %d.\n",
			pHouse->ArrayIndex, pInf->Type->ID, pInf->UniqueID, sent - 1, pBest->Type->ID, pBestKnown->Cell.X, pBestKnown->Cell.Y,
			pBestKnown->Worth, pBest->Occupants.Count, pBestKnown->MaxOccupants, now);

		return true;
	}

	void NoteUrbanClearOrder(HouseClass* pHouse, TechnoClass* pClearer, BuildingClass* pTarget)
	{
		auto& state = GetUrbanState(pHouse);
		state.ClearOrders++;
		CountDecision(NeuralDecision::UrbanClear, pHouse->ArrayIndex);

		if (state.ClearOrders <= 40)
		{
			Debug::Log("[NeuralAI] House %d Urban: clear %s at (%d,%d) with %s %u, %d inside, frame %d.\n",
				pHouse->ArrayIndex, pTarget->Type->ID, pTarget->GetMapCoords().X, pTarget->GetMapCoords().Y,
				pClearer->GetTechnoType()->ID, pClearer->UniqueID, pTarget->Occupants.Count, Unsorted::CurrentFrame);
		}
	}
}

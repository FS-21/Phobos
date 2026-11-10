// NeuralAI.Urban.inl: Expert 9 (Urban Garrison & Allied Coalition Warfare): fog-fair memory of
// occupiable buildings, which of them are worth holding, garrison orders and evacuations, the
// enemy garrisons to clear and the coalition work (allied rescues, joint attacks, shared techs)
// that feed the expert selector.
// Part of NeuralAI.cpp (included in order; one translation unit).

namespace
{
	// An occupiable building (civilian building or battle bunker) this house has seen, as it looked
	// the last time it was in sight.
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
		int BlockedUntilFrame { 0 };
		// Orders that did not get a soldier in (it was ordered there again soon after).
		int Failures { 0 };
	};

	// The last garrison order of one soldier.
	struct NeuralUrbanOrder
	{
		DWORD InfantryID { 0 };
		DWORD BuildingID { 0 };
		int Frame { 0 };
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
		// The live building of each entry of Buildings (same index, null once gone), valid in
		// LiveFrame only.
		std::vector<BuildingClass*> Live {};
		int LiveFrame { -1 };
		// Rebuilt every AI tick, sorted by building ID.
		std::vector<NeuralUrbanEntering> Entering {};
		// Sorted by infantry ID; orders older than 900 frames are dropped.
		std::vector<NeuralUrbanOrder> Orders {};
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
	int RateUrbanBuilding(HouseClass* pHouse, const NeuralKnownUrbanBuilding& known, const CellStruct& home,
		int mainAttackSector, const CellStruct& nearestEnemyCell)
	{
		if (known.OccupantOwner >= 0 && known.OccupantOwner != pHouse->ArrayIndex)
			return 0;

		if (known.HealthPercent < 45 || home == CellStruct::Empty)
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
			if ((tech.OwnerIndex < 0 || tech.OwnerIndex == pHouse->ArrayIndex) && tech.Cell.DistanceFrom(known.Cell) <= 7.0)
			{
				techBonus = tech.OwnerIndex == pHouse->ArrayIndex ? 80 : 60;
				break;
			}
		}

		if (std::min(anchorDist, reachDist) > 72 || IsCellInKnownEnemyDefenseReach(pHouse, known.Cell, 1))
			return 0;

		if (reachDist > 28 && OwnDefenseNear(pHouse, known.Cell, 8) < 300)
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

		for (auto const pBld : BuildingClass::Array)
		{
			if (!pBld || pBld->InLimbo || !pBld->IsAlive || !pBld->Type || !pBld->Type->CanBeOccupied
				|| pBld->Type->InvisibleInGame)
			{
				continue;
			}

			const CellStruct cell = pBld->GetMapCoords();
			const bool ownOrAllied = pBld->Owner && (pBld->Owner == pHouse || pHouse->IsAlliedWith(pBld->Owner));

			if (!ownOrAllied && !IsCellInSightOfAI(pHouse, cell))
				continue;

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

		for (auto const pInf : InfantryClass::Array)
		{
			if (!pInf || !pInf->IsAlive || pInf->InLimbo || !pInf->Owner || !pInf->Type
				|| (pInf->Owner != pHouse && !pHouse->IsAlliedWith(pInf->Owner)))
			{
				continue;
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

			auto pEntering = FindUrbanEntering(state, pGoal->UniqueID);

			if (!pEntering)
			{
				NeuralUrbanEntering entering;
				entering.BuildingID = pGoal->UniqueID;

				auto const it = std::lower_bound(state.Entering.begin(), state.Entering.end(), entering.BuildingID,
					[](const NeuralUrbanEntering& entry, DWORD key) { return entry.BuildingID < key; });
				pEntering = &*state.Entering.insert(it, entering);
			}

			if (pInf->Owner == pHouse)
				pEntering->Own++;
			else
				pEntering->Allied++;
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

		const int mainAttackSector = GetMainAttackSector(pHouse);
		std::array<int, 3> bestWorth {};
		state.ThreatGarrisons = 0;
		state.WorthyBuildings = 0;

		for (auto& known : state.Buildings)
		{
			known.Worth = RateUrbanBuilding(pHouse, known, home, mainAttackSector, nearestEnemyCell);

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
				state.WorthyBuildings++;

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
				"allies %d, opportunity %d, orders %d, evacuations %d, clears %d; occupiers %d, tries %d (over share %d, no building %d), frame %d.\n",
				pHouse->ArrayIndex, state.OwnGarrisons, state.OwnOccupants, state.PeakGarrisons, state.PeakOccupants,
				static_cast<int>(state.Buildings.size()), state.WorthyBuildings, state.GarrisonWorth, state.ThreatGarrisons, state.Allies,
				state.Opportunity, state.GarrisonOrders, state.Evacuations, state.ClearOrders, state.OwnOccupierInfantry,
				state.TriesSinceReport, state.OverCapSinceReport, state.NoBuildingSinceReport, now);
			state.TriesSinceReport = 0;
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

	// Garrison demand for the infantry selector: room in buildings worth holding beyond the
	// occupiers we have (0 when nothing is worth it), and whether an enemy garrison near us waits
	// for a clearer we do not own.
	void GetUrbanProductionNeeds(HouseClass* pHouse, int& outGarrisonRoom, bool& outNeedClearer)
	{
		auto& state = GetUrbanState(pHouse);
		int room = 0;

		for (auto const& known : state.Buildings)
		{
			if (known.Worth >= 100 && Unsorted::CurrentFrame >= known.BlockedUntilFrame)
				room += std::max(0, known.MaxOccupants - known.Occupants);
		}

		outGarrisonRoom = std::max(0, std::min(room, 12) - std::max(0, state.OwnOccupierInfantry - state.OwnOccupants));
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
	// left after the soldiers (ours and allied) already walking in. At most a share of our
	// occupiers sits in buildings: 55% under Expert 9, 30% otherwise; on an urban map (6+ known
	// buildings worth holding) 70% / 50%, and up to 32 cells away without Expert 9. A building we already hold scores double and is filled up
	// even past that share: full garrisons instead of one or two soldiers in many buildings.
	template <typename DispatchFn>
	bool RunUrbanGarrison(HouseClass* pHouse, InfantryClass* pInf, bool expertActive, int rangeCells, DispatchFn&& dispatch)
	{
		if (!IsGarrisonOccupierType(pInf->Type) || pInf->GetHealthPercentage() < 0.50)
			return false;

		auto& state = GetUrbanState(pHouse);
		int pendingTotal = 0;

		for (auto const& entering : state.Entering)
			pendingTotal += entering.Own;

		const bool urbanMap = state.WorthyBuildings >= 6;
		const int sharePercent = expertActive ? (urbanMap ? 70 : 55) : (urbanMap ? 50 : 30);
		const int cap = std::max(expertActive ? 4 : 2, state.OwnOccupierInfantry * sharePercent / 100);
		const bool overCap = state.OwnOccupants + pendingTotal >= cap;
		state.TriesSinceReport++;

		if (overCap)
			state.OverCapSinceReport++;

		RefreshUrbanLivePointers(state);
		const int infZone = ObjectZone(pInf, MovementZone::Normal);
		const CellStruct infCell = pInf->GetMapCoords();
		// On an urban map the city is worth a walk: 32 cells without Expert 9 (its slot range is
		// 6..16 cells, the next block of a city is often further).
		const int maxRange = expertActive ? 64 : (urbanMap ? std::max(rangeCells, 32) : rangeCells);
		const int now = Unsorted::CurrentFrame;
		BuildingClass* pBest = nullptr;
		NeuralKnownUrbanBuilding* pBestKnown = nullptr;
		int bestScore = 0;

		for (size_t i = 0; i < state.Buildings.size(); ++i)
		{
			auto& known = state.Buildings[i];

			if (known.Worth <= 0 || now < known.BlockedUntilFrame)
				continue;

			const int dist = static_cast<int>(infCell.DistanceFrom(known.Cell));

			if (dist > maxRange)
				continue;

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

			// Over the share only a building we already hold is still filled up.
			const bool partlyOurs = own && inside + coming > 0;

			if (overCap && !partlyOurs)
				continue;

			if (infZone >= 0 && !IsBuildingEnterableFromZone(pBld, infZone))
				continue;

			const int score = known.Worth * 100 / (100 + dist * 4) * (partlyOurs ? 2 : 1);

			if (score > bestScore)
			{
				bestScore = score;
				pBest = pBld;
				pBestKnown = &known;
			}
		}

		if (!pBest)
		{
			state.NoBuildingSinceReport++;
			return false;
		}

		// Ordered to the same building again soon after: the last order did not get it in. Three
		// such failures block the building for a while (the way is cut, or the engine refuses).
		state.Orders.erase(std::remove_if(state.Orders.begin(), state.Orders.end(),
			[now](const NeuralUrbanOrder& order) { return now - order.Frame > 900 || now < order.Frame; }), state.Orders.end());

		auto itOrder = std::lower_bound(state.Orders.begin(), state.Orders.end(), pInf->UniqueID,
			[](const NeuralUrbanOrder& order, DWORD key) { return order.InfantryID < key; });

		if (itOrder != state.Orders.end() && itOrder->InfantryID == pInf->UniqueID)
		{
			if (itOrder->BuildingID == pBest->UniqueID && ++pBestKnown->Failures >= 3)
			{
				pBestKnown->Failures = 0;
				pBestKnown->BlockedUntilFrame = now + 3600;
				Debug::Log("[NeuralAI] House %d Urban: %s at (%d,%d) blocked, soldiers ordered in keep failing, frame %d.\n",
					pHouse->ArrayIndex, pBest->Type->ID, pBestKnown->Cell.X, pBestKnown->Cell.Y, now);
				state.Orders.erase(itOrder);
				return false;
			}

			itOrder->BuildingID = pBest->UniqueID;
			itOrder->Frame = now;
		}
		else
		{
			state.Orders.insert(itOrder, NeuralUrbanOrder { pInf->UniqueID, pBest->UniqueID, now });
		}

		// A building we already hold refuses Enter (the destination is dropped at once); more
		// soldiers go in through the capture mission, like the garrison order of a player.
		if (pBest->Owner == pHouse)
		{
			pInf->ClearMegaMissionData();
			pInf->SetTarget(nullptr);
			pInf->SetDestination(pBest, true);
			pInf->QueueMission(Mission::Capture, false);
			pInf->NextMission();
		}
		else if (!dispatch(pInf, pBest, Mission::Enter, false))
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

		Debug::Log("[NeuralAI] House %d Urban: garrison %s %u -> %s at (%d,%d), worth %d, %d/%d inside, frame %d.\n",
			pHouse->ArrayIndex, pInf->Type->ID, pInf->UniqueID, pBest->Type->ID, pBestKnown->Cell.X, pBestKnown->Cell.Y,
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

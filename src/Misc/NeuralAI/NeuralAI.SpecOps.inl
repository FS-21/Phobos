// NeuralAI.SpecOps.inl: Expert 4 (Special Operations): fog-fair memory of neutral tech
// buildings, capture / spy / bridge opportunities that feed the expert selector, escorted
// engineer captures and the bookkeeping of engineers and spies sent out.
// Part of NeuralAI.cpp (included in order; one translation unit).

namespace
{
	// A capturable building this house has seen, as it looked the last time it was in sight.
	struct NeuralKnownTech
	{
		DWORD ID { 0 };
		CellStruct Cell { CellStruct::Empty };
		int Value { 0 };
		bool Cash { false };
		// Owner index when last seen; -1 while it was neutral.
		int OwnerIndex { -1 };
		// Movement zones (Normal) of the cells around the foundation, where an engineer enters from.
		std::array<int, 4> Zones { -1, -1, -1, -1 };
		int SeenFrame { 0 };
		int Failures { 0 };
		int BlockedUntilFrame { 0 };
	};

	// One engineer on a capture, with its escort.
	struct NeuralSpecOpsCapture
	{
		DWORD EngineerID { 0 };
		DWORD TargetID { 0 };
		CellStruct TargetCell { CellStruct::Empty };
		int StartFrame { 0 };
		bool Dispatched { false };
		int DispatchFrame { 0 };
		std::vector<DWORD> Escorts {};
	};

	// One spy sent to infiltrate a building.
	struct NeuralSpecOpsSpy
	{
		DWORD SpyID { 0 };
		DWORD TargetID { 0 };
		CellStruct TargetCell { CellStruct::Empty };
		int TargetOwner { -1 };
		int LastDistance { 9999 };
		// Distance (cells) from the target's map cell at which a vanished spy counts as gone in.
		int DoorDistance { 3 };
		int StartFrame { 0 };
		// House whose colors the spy wore on its last tick in sight (-1: none of an enemy).
		int DisguiseOwner { -1 };
	};

	struct NeuralSpecOpsState
	{
		int MatchSerial { -1 };
		int LastScanFrame { -100000 };
		// Sorted by ID (binary search, deterministic order).
		std::vector<NeuralKnownTech> Techs {};
		std::vector<NeuralSpecOpsCapture> Captures {};
		std::vector<NeuralSpecOpsSpy> Spies {};
		// Opportunity summary of the last scan.
		InfantryTypeClass* EngineerType { nullptr };
		bool SpyAvailable { false };
		int KnownNeutralTechs { 0 };
		int KnownNeutralCash { 0 };
		int SafeTechValue { 0 };
		int ContestedTechValue { 0 };
		int SpyTargets { 0 };
		bool BridgeRepairTask { false };
		bool BridgeDemolitionTask { false };
		int Opportunity { 0 };
		// Results this match (logs and tests).
		int CaptureCount { 0 };
		int InfiltrationCount { 0 };
		// Frames of the last spies killed on their way (oldest first, 4 at most).
		std::vector<int> SpyLossFrames {};
		// Spy UniqueID -> disguise kit shots ordered (key access only); after 3 the spy stops trying.
		std::unordered_map<DWORD, int> KitShots {};
	};

	std::array<NeuralSpecOpsState, 32> SpecOpsStates {};

	NeuralSpecOpsState& GetSpecOpsState(const HouseClass* pHouse)
	{
		auto& state = SpecOpsStates[std::clamp(pHouse ? pHouse->ArrayIndex : 0, 0, 31)];

		if (state.MatchSerial != NeuralMatchSerial)
		{
			state = NeuralSpecOpsState {};
			state.MatchSerial = NeuralMatchSerial;
		}

		return state;
	}

	NeuralKnownTech* FindKnownTech(NeuralSpecOpsState& state, DWORD id)
	{
		auto const it = std::lower_bound(state.Techs.begin(), state.Techs.end(), id,
			[](const NeuralKnownTech& tech, DWORD key) { return tech.ID < key; });

		return (it != state.Techs.end() && it->ID == id) ? &*it : nullptr;
	}

	// What owning this tech building is worth, from its rules only: income first, then what it
	// produces, heals or reveals.
	int SpecOpsTechValue(const BuildingTypeClass* pType)
	{
		if (!pType)
			return 0;

		int value = 1;

		if (pType->ProduceCashAmount > 0 || pType->ProduceCashStartup > 0)
			value += 3;

		if (pType->Factory != AbstractType::None)
			value += 2;

		if (pType->Hospital || pType->Armory || pType->UnitRepair || pType->Radar || pType->SpySat)
			value += 2;

		if (pType->PowerBonus > pType->PowerDrain)
			value += 1;

		if (pType->Cost >= 1000)
			value += 1;

		return std::min(value, 6);
	}

	// Can a ground unit standing in this movement zone (Normal) walk up to the building? The cells
	// under a building often carry another zone than the ground around it (map tech placed at map
	// start), so the zone of the building's own cell says nothing; the cells around it do.
	bool IsBuildingEnterableFromZone(const BuildingClass* pBld, int zone)
	{
		if (!pBld || !pBld->Type || zone < 0)
			return false;

		const CellStruct cell = pBld->GetMapCoords();
		const int width = pBld->Type->GetFoundationWidth();
		const int height = pBld->Type->GetFoundationHeight(false);

		for (int dy = -2; dy <= height + 1; ++dy)
		{
			for (int dx = -2; dx <= width + 1; ++dx)
			{
				if (dx >= 0 && dx < width && dy >= 0 && dy < height)
					continue;

				const CellStruct around { static_cast<short>(cell.X + dx), static_cast<short>(cell.Y + dy) };

				if (MapClass::Instance.CoordinatesLegal(around)
					&& MapClass::Instance.GetMovementZoneType(around, MovementZone::Normal, false) == zone)
				{
					return true;
				}
			}
		}

		return false;
	}

	// Has this house seen the building as neutral the last time it was in sight?
	bool IsKnownNeutralTech(HouseClass* pHouse, const BuildingClass* pBld)
	{
		if (!pHouse || !pBld)
			return false;

		auto& state = GetSpecOpsState(pHouse);
		auto const pTech = FindKnownTech(state, pBld->UniqueID);

		return pTech && pTech->OwnerIndex < 0;
	}

	bool IsSpecOpsCaptureTarget(HouseClass* pHouse, DWORD targetID, DWORD exceptEngineer)
	{
		for (auto const& capture : GetSpecOpsState(pHouse).Captures)
		{
			if (capture.TargetID == targetID && capture.EngineerID != exceptEngineer)
				return true;
		}

		return false;
	}

	// May any engineer of ours walk to this neutral building now? Known as neutral, not blocked by
	// recent failures, not taken by an Expert 4 capture and out of known enemy defense reach.
	bool IsSpecOpsTechAvailable(HouseClass* pHouse, const BuildingClass* pBld, DWORD engineerID)
	{
		if (!pHouse || !pBld)
			return false;

		auto& state = GetSpecOpsState(pHouse);
		auto const pTech = FindKnownTech(state, pBld->UniqueID);

		return pTech && pTech->OwnerIndex < 0 && Unsorted::CurrentFrame >= pTech->BlockedUntilFrame
			&& !IsSpecOpsCaptureTarget(pHouse, pTech->ID, engineerID) && !IsCellInKnownEnemyDefenseReach(pHouse, pTech->Cell, 2);
	}

	bool CanEngineerReachTech(HouseClass* pHouse, const NeuralKnownTech& tech, int engineerZone, const InfantryTypeClass* pEngineerType)
	{
		for (const int zone : tech.Zones)
		{
			if (zone >= 0 && zone == engineerZone)
				return true;
		}

		return pEngineerType && CanHouseFerryTypeAcrossWater(pHouse, pEngineerType);
	}

	// Refresh the tech memory from what is in sight now and log captures. Fog-fair: a tech in the
	// fog keeps the owner it had when last seen.
	void UpdateSpecOpsTechMemory(HouseClass* pHouse, NeuralSpecOpsState& state)
	{
		const int now = Unsorted::CurrentFrame;
		std::vector<DWORD> inSightIDs;

		// Insignificant is no filter here: many maps' oil derricks are Insignificant tech buildings.
		for (auto const pBld : BuildingClass::Array)
		{
			if (!pBld || pBld->InLimbo || !pBld->IsAlive || !pBld->Type || !pBld->Owner || !pBld->Type->Capturable
				|| pBld->Type->InvisibleInGame || pBld->Type->BridgeRepairHut)
			{
				continue;
			}

			const CellStruct cell = pBld->GetMapCoords();

			if (pBld->Owner != pHouse && !IsCellInSightOfAI(pHouse, cell))
				continue;

			inSightIDs.push_back(pBld->UniqueID);
			const int ownerIndex = pBld->Owner->IsNeutral() ? -1 : pBld->Owner->ArrayIndex;
			NeuralKnownTech* pTech = FindKnownTech(state, pBld->UniqueID);

			if (!pTech)
			{
				// Only buildings first seen neutral are tech; capturable enemy structures are the
				// engineer assault plan's business.
				if (ownerIndex >= 0)
					continue;

				NeuralKnownTech tech;
				tech.ID = pBld->UniqueID;
				tech.Cell = cell;
				tech.Value = SpecOpsTechValue(pBld->Type);
				tech.Cash = pBld->Type->ProduceCashAmount > 0 || pBld->Type->ProduceCashStartup > 0;

				const int width = pBld->Type->GetFoundationWidth();
				const int height = pBld->Type->GetFoundationHeight(false);
				size_t zoneCount = 0;

				for (int dy = -2; dy <= height + 1 && zoneCount < tech.Zones.size(); ++dy)
				{
					for (int dx = -2; dx <= width + 1 && zoneCount < tech.Zones.size(); ++dx)
					{
						if (dx >= 0 && dx < width && dy >= 0 && dy < height)
							continue;

						const CellStruct around { static_cast<short>(cell.X + dx), static_cast<short>(cell.Y + dy) };

						if (!MapClass::Instance.CoordinatesLegal(around))
							continue;

						const int zone = MapClass::Instance.GetMovementZoneType(around, MovementZone::Normal, false);

						if (zone <= 1 || std::find(tech.Zones.begin(), tech.Zones.begin() + zoneCount, zone) != tech.Zones.begin() + zoneCount)
							continue;

						tech.Zones[zoneCount++] = zone;
					}
				}

				auto const it = std::lower_bound(state.Techs.begin(), state.Techs.end(), tech.ID,
					[](const NeuralKnownTech& known, DWORD key) { return known.ID < key; });
				pTech = &*state.Techs.insert(it, tech);
			}

			if (pTech->OwnerIndex != ownerIndex)
			{
				if (ownerIndex == pHouse->ArrayIndex)
				{
					state.CaptureCount++;
					Debug::Log("[NeuralAI] House %d SpecOps: captured %s at (%d,%d), frame %d (captures %d).\n",
						pHouse->ArrayIndex, pBld->Type->ID, cell.X, cell.Y, now, state.CaptureCount);
				}

				pTech->OwnerIndex = ownerIndex;
			}

			pTech->SeenFrame = now;
		}

		// A remembered tech whose cell is in sight but that is gone was destroyed.
		std::sort(inSightIDs.begin(), inSightIDs.end());
		state.Techs.erase(std::remove_if(state.Techs.begin(), state.Techs.end(),
			[&](const NeuralKnownTech& tech)
			{
				return IsCellInSightOfAI(pHouse, tech.Cell) && !std::binary_search(inSightIDs.begin(), inSightIDs.end(), tech.ID);
			}), state.Techs.end());
	}

	int CountKnownDisguiseDetectorBuildings(HouseClass* pHouse, const CellStruct& cell, int marginCells);

	// Enemy buildings this house knows and would gain something from spying now.
	int CountSpecOpsSpyTargets(HouseClass* pHouse)
	{
		int targets = 0;

		for (auto const& info : GetKnownEnemyBuildingCache(pHouse).Entries)
		{
			auto const pType = info.Building->Type;

			if (!pType->Spyable || CountKnownDisguiseDetectorBuildings(pHouse, info.Building->GetMapCoords(), 1) > 0)
				continue;

			auto const pOwner = info.Building->Owner;
			const bool worth = (pType->Factory == AbstractType::UnitType && !pType->Naval && !pHouse->WarFactoryInfiltrated)
				|| (pType->Factory == AbstractType::InfantryType && !pHouse->BarracksInfiltrated)
				|| ((pType->Radar || pType->SpySat) && !pHouse->SpySatActive && !pOwner->RadarVisibleTo.Contains(pHouse->ArrayIndex))
				|| pType->SuperWeapon != -1;

			if (worth && ++targets >= 3)
				break;
		}

		return targets;
	}

	// The enemy house whose colors a spy wears, or nullptr. A new spy shows the default disguise of
	// its own house, which fools nobody; copying a soldier takes that soldier's house.
	HouseClass* SpyDisguiseHouse(const InfantryClass* pSpy)
	{
		if (!pSpy || !pSpy->Disguised || !pSpy->Owner)
			return nullptr;

		auto const pAs = pSpy->DisguisedAsHouse;

		if (!pAs || pAs == pSpy->Owner || pSpy->Owner->IsAlliedWith(pAs))
			return nullptr;

		return pAs;
	}

	// A disguise only fools the copied house and its allies: in a free for all, the soldiers of a
	// third house still shoot a spy dressed as somebody else.
	bool IsSpyDisguisedFor(const InfantryClass* pSpy, HouseClass* pVictim)
	{
		auto const pAs = SpyDisguiseHouse(pSpy);

		return pAs && pVictim && (pAs == pVictim || pVictim->IsAlliedWith(pAs));
	}

	// A soldier of the victim's side (the victim or an ally of it, never ours) in our sight right
	// now, nearest to the spy first (UniqueID on ties): a player can only copy a soldier in sight.
	InfantryClass* FindSpyDisguiseSource(HouseClass* pHouse, InfantryClass* pSpy, HouseClass* pVictim)
	{
		InfantryClass* pBest = nullptr;
		int bestDist = INT_MAX;

		for (auto const pInf : InfantryClass::Array)
		{
			if (!pInf || pInf->InLimbo || !pInf->IsAlive || !pInf->Owner || !pInf->Type || pInf->Type->Doggie
				|| pInf->Type->Slaved || pInf->IsDisguised())
			{
				continue;
			}

			if ((pInf->Owner != pVictim && !pVictim->IsAlliedWith(pInf->Owner)) || pHouse->IsAlliedWith(pInf->Owner)
				|| pInf->Owner->IsNeutral() || !IsCellInSightOfAI(pHouse, pInf->GetMapCoords()))
			{
				continue;
			}

			const int dist = pSpy->DistanceFrom(pInf);

			if (dist < bestDist || (dist == bestDist && pInf->UniqueID < pBest->UniqueID))
			{
				bestDist = dist;
				pBest = pInf;
			}
		}

		return pBest;
	}

	// The spy carries a disguise kit: a weapon whose warhead copies the target (MakesDisguise=).
	bool HasDisguiseKit(InfantryTypeClass* pType)
	{
		for (int i = 0; pType && i < 2; ++i)
		{
			auto const pSlot = pType->GetWeapon(i);
			auto const pWeapon = pSlot ? pSlot->WeaponType : nullptr;

			if (pWeapon && pWeapon->Warhead && pWeapon->Warhead->MakesDisguise)
				return true;
		}

		return false;
	}

	// Copies a soldier's colors: at once when the engine allows a direct disguise, otherwise the
	// way a player does it, firing the disguise kit (a MakesDisguise= warhead, e.g. MakeupKit with
	// an endless range and FireOnce) at that soldier; 3 shots at most per spy. True while the copy
	// is done or under way.
	bool CopySoldierDisguise(HouseClass* pHouse, InfantryClass* pSpy, InfantryClass* pSoldier)
	{
		if (!pSpy || !pSoldier)
			return false;

		if (pSpy->CanDisguiseAs(pSoldier))
		{
			pSpy->DisguiseAs(pSoldier);
			return true;
		}

		if (pSpy->Target == pSoldier)
			return true;

		int& shots = GetSpecOpsState(pHouse).KitShots[pSpy->UniqueID];

		if (!HasDisguiseKit(pSpy->Type) || shots >= 3)
			return false;

		shots++;
		pSpy->SetTarget(pSoldier);
		pSpy->QueueMission(Mission::Attack, false);

		return true;
	}

	// Puts on the victim's colors as soon as one of its soldiers is in sight. True when the spy
	// fools the victim now or is copying one of its soldiers.
	bool DisguiseSpyFor(HouseClass* pHouse, InfantryClass* pSpy, HouseClass* pVictim)
	{
		if (!pHouse || !pSpy || !pVictim || !pSpy->Type || !pSpy->Type->CanDisguise || pSpy->InLimbo)
			return false;

		if (IsSpyDisguisedFor(pSpy, pVictim))
			return true;

		auto const pSource = FindSpyDisguiseSource(pHouse, pSpy, pVictim);

		return pSource && CopySoldierDisguise(pHouse, pSpy, pSource);
	}

	// The enemy house a spy of ours is going to: its building target, or the one SpecOps noted.
	HouseClass* GetSpyVictim(HouseClass* pHouse, InfantryClass* pSpy)
	{
		auto const pGoal = abstract_cast<BuildingClass*>(pSpy->Destination ? pSpy->Destination : pSpy->ArchiveTarget);

		if (pGoal && pGoal->IsAlive && pGoal->Owner && !pGoal->Owner->IsNeutral() && !pHouse->IsAlliedWith(pGoal->Owner))
			return pGoal->Owner;

		for (auto const& spy : GetSpecOpsState(pHouse).Spies)
		{
			if (spy.SpyID == pSpy->UniqueID && spy.TargetOwner >= 0 && spy.TargetOwner < HouseClass::Array.Count)
			{
				auto const pOwner = HouseClass::Array.GetItem(spy.TargetOwner);

				return (pOwner && !pOwner->Defeated && !pHouse->IsAlliedWith(pOwner)) ? pOwner : nullptr;
			}
		}

		return nullptr;
	}

	// Known enemy buildings that see through disguises (DetectDisguise=, radius DetectDisguiseRange=
	// in cells) covering a cell, plus marginCells. Their power is not always visible, so all count.
	int CountKnownDisguiseDetectorBuildings(HouseClass* pHouse, const CellStruct& cell, int marginCells)
	{
		int count = 0;

		for (auto const& info : GetKnownEnemyBuildingCache(pHouse).Entries)
		{
			auto const pType = info.Building->Type;

			if (!pType->DetectDisguise || pType->DetectDisguiseRange <= 0)
				continue;

			if (info.Building->GetMapCoords().DistanceFrom(cell) <= static_cast<double>(pType->DetectDisguiseRange + marginCells))
				count++;
		}

		return count;
	}

	// A disguise is no use in this cell: a known detector building covers it, or an enemy unit or
	// soldier that sees through disguises (DetectDisguise=, dogs, Sensors=) is in sight near it.
	bool IsCellWatchedByDisguiseDetector(HouseClass* pHouse, const CellStruct& cell, int marginCells)
	{
		if (CountKnownDisguiseDetectorBuildings(pHouse, cell, marginCells) > 0)
			return true;

		bool watched = false;

		ForEachTechnoNear(cell, 12 + marginCells, [&](TechnoClass* pTech) -> bool
		{
			if (!pTech->Owner || pHouse->IsAlliedWith(pTech->Owner) || pTech->Owner->IsNeutral()
				|| pTech->WhatAmI() == AbstractType::Building || !IsCellInSightOfAI(pHouse, pTech->GetMapCoords()))
			{
				return true;
			}

			auto const pType = pTech->GetTechnoType();
			auto const pInfType = abstract_cast<InfantryTypeClass*>(pType);

			if (!pType || !(pType->DetectDisguise || pType->Sensors || (pInfType && pInfType->Doggie)))
				return true;

			const int range = (pType->DetectDisguiseRange > 0 ? pType->DetectDisguiseRange : 6) + marginCells;

			if (pTech->GetMapCoords().DistanceFrom(cell) <= static_cast<double>(range))
			{
				watched = true;
				return false;
			}

			return true;
		});

		return watched;
	}

	// Combat value of the house's own armed units within radius of a cell.
	int OwnArmedValueNear(HouseClass* pHouse, const CellStruct& cell, int radius)
	{
		int value = 0;

		ForEachTechnoNear(cell, radius, [&](TechnoClass* pTech) -> bool
		{
			if (pTech->Owner != pHouse || pTech->WhatAmI() == AbstractType::Building)
				return true;

			auto const pType = pTech->GetTechnoType();

			if (pType && GroundRangeOf(pType) > 0)
				value += GetCombatValueOfType(pType, false);

			return true;
		});

		return value;
	}

	FootClass* FindOwnFootByID(HouseClass* pHouse, DWORD id)
	{
		if (!id)
			return nullptr;

		for (auto const pFoot : FootClass::Array)
		{
			if (pFoot && pFoot->UniqueID == id)
				return (pFoot->Owner == pHouse && pFoot->IsAlive) ? pFoot : nullptr;
		}

		return nullptr;
	}

	// A failed capture blocks that tech for a while (longer after each failure) and its neighbours
	// within 6 cells as long, since whatever stopped the engineer is likely still around them.
	void FailSpecOpsCapture(NeuralSpecOpsState& state, DWORD targetID, int baseBlockFrames)
	{
		auto const pTech = FindKnownTech(state, targetID);

		if (!pTech)
			return;

		pTech->Failures++;
		const int until = Unsorted::CurrentFrame + baseBlockFrames * std::min(pTech->Failures, 3);
		const CellStruct cell = pTech->Cell;

		for (auto& tech : state.Techs)
		{
			if (tech.Cell.DistanceFrom(cell) <= 6.0)
			{
				tech.BlockedUntilFrame = std::max(tech.BlockedUntilFrame, until);

				if (&tech != pTech)
					tech.Failures = std::max(tech.Failures, 1);
			}
		}
	}

	// Per AI tick: tech memory (every 150 frames), opportunity summary, captures and spies upkeep.
	void UpdateSpecOps(HouseClass* pHouse)
	{
		if (!pHouse || pHouse->ArrayIndex < 0 || pHouse->ArrayIndex >= 32)
			return;

		auto& state = GetSpecOpsState(pHouse);
		const int now = Unsorted::CurrentFrame;
		const CellStruct home = GetHouseHomeCell(pHouse);

		if (now - state.LastScanFrame >= 150 || now < state.LastScanFrame)
		{
			state.LastScanFrame = now;
			UpdateSpecOpsTechMemory(pHouse, state);

			state.EngineerType = nullptr;
			state.SpyAvailable = false;

			for (auto const pType : InfantryTypeClass::Array)
			{
				if (!pType || (!pType->Engineer && !IsSpyInfantryType(pType)))
					continue;

				const bool usable = pHouse->CountOwnedNow(pType) > 0 || CanBuildUnderHumanRules(pHouse, pType);

				if (!usable)
					continue;

				if (pType->Engineer && !state.EngineerType)
					state.EngineerType = pType;
				else if (IsSpyInfantryType(pType))
					state.SpyAvailable = true;
			}

			const int homeZone = (home != CellStruct::Empty)
				? MapClass::Instance.GetMovementZoneType(home, MovementZone::Normal, false)
				: -1;

			state.KnownNeutralTechs = 0;
			state.KnownNeutralCash = 0;
			state.SafeTechValue = 0;
			state.ContestedTechValue = 0;

			// Worth of each reachable tech out of known defense reach, full up to 40 cells from home and
			// fading beyond; contested ones (enemies in sight near them) at half. Only the three best
			// count: a map with dozens of derricks has no more engineer work at a time than one with three.
			std::array<int, 3> bestWorth {};

			for (auto const& tech : state.Techs)
			{
				if (tech.OwnerIndex >= 0)
					continue;

				state.KnownNeutralTechs++;
				state.KnownNeutralCash += tech.Cash ? 1 : 0;

				if (now < tech.BlockedUntilFrame || !CanEngineerReachTech(pHouse, tech, homeZone, state.EngineerType)
					|| IsCellInKnownEnemyDefenseReach(pHouse, tech.Cell, 2))
				{
					continue;
				}

				const bool contested = VisibleEnemyThreatNear(pHouse, tech.Cell, 7) > 0;
				const int dist = (home != CellStruct::Empty) ? static_cast<int>(home.DistanceFrom(tech.Cell)) : 40;
				int worth = tech.Value * 30 * 40 / std::max(40, dist);

				if (contested)
				{
					worth /= 2;
					state.ContestedTechValue += tech.Value;
				}
				else
				{
					state.SafeTechValue += tech.Value;
				}

				auto const pWorst = std::min_element(bestWorth.begin(), bestWorth.end());

				if (worth > *pWorst)
					*pWorst = worth;
			}

			// Two spies killed on the way within 9000 frames: the way in is watched, ask for no more.
			const bool spiesWasted = state.SpyLossFrames.size() >= 2
				&& now - state.SpyLossFrames[state.SpyLossFrames.size() - 2] < 9000;
			state.SpyTargets = (state.SpyAvailable && !spiesWasted) ? CountSpecOpsSpyTargets(pHouse) : 0;

			auto const& bridges = GetBridgeState(pHouse);
			state.BridgeRepairTask = state.EngineerType && (bridges.RouteSpan >= 0 || bridges.FleetBlockSpan >= 0);
			state.BridgeDemolitionTask = AreBridgesDestroyable()
				&& (bridges.AmbushSpan >= 0 || bridges.SeaLaneSpan >= 0 || bridges.MoatSpan >= 0);

			// Selector prior of Expert 4: real work for it on this map right now. Techs only count
			// with an engineer we own or can train; ops in flight keep it in charge until they end.
			int opportunity = 0;

			if (state.EngineerType)
				opportunity += std::min(560, bestWorth[0] + bestWorth[1] + bestWorth[2]);

			opportunity += std::min(180, state.SpyTargets * 60);
			opportunity += state.BridgeRepairTask ? 260 : 0;
			opportunity += state.BridgeDemolitionTask ? 200 : 0;
			opportunity += state.Captures.empty() ? 0 : 160;
			state.Opportunity = std::min(780, opportunity);
		}

		// Captures upkeep.
		for (size_t i = 0; i < state.Captures.size();)
		{
			auto& capture = state.Captures[i];
			auto const pEngineer = FindOwnFootByID(pHouse, capture.EngineerID);
			auto const pTarget = FindBuildingByID(capture.TargetID);
			bool done = false;

			if (pTarget && pTarget->Owner == pHouse)
			{
				done = true;
			}
			else if (!pEngineer)
			{
				// Killed on the way (or entered a building that changed hands meanwhile).
				FailSpecOpsCapture(state, capture.TargetID, 2700);
				Debug::Log("[NeuralAI] House %d SpecOps: engineer %u lost on the way to (%d,%d), frame %d.\n",
					pHouse->ArrayIndex, capture.EngineerID, capture.TargetCell.X, capture.TargetCell.Y, now);
				done = true;
			}
			else if (!pTarget || !IsKnownNeutralTech(pHouse, pTarget) || now - capture.StartFrame > 7200)
			{
				if (pTarget && now - capture.StartFrame > 7200)
					FailSpecOpsCapture(state, capture.TargetID, 1800);

				if (pEngineer->Destination == pTarget || pEngineer->CurrentMission == Mission::Enter)
				{
					pEngineer->SetDestination(nullptr, true);
					pEngineer->QueueMission(Mission::Guard, true);
				}

				done = true;
			}
			else if (capture.Dispatched && !pEngineer->InLimbo && home != CellStruct::Empty)
			{
				// On the way: turn back when the visible enemies around the engineer outweigh our
				// armed units there, unless it is already at the door.
				const CellStruct engCell = pEngineer->GetMapCoords();

				if (engCell.DistanceFrom(capture.TargetCell) > 5.0 && now - capture.DispatchFrame >= 150)
				{
					const int threat = VisibleEnemyThreatNear(pHouse, engCell, 5);

					if (threat > 0 && threat > OwnArmedValueNear(pHouse, engCell, 6))
					{
						FailSpecOpsCapture(state, capture.TargetID, 1800);

						if (auto const pHomeCell = MapClass::Instance.TryGetCellAt(home))
							IssueMoveOrder(pEngineer, pHomeCell, 4.0);

						Debug::Log("[NeuralAI] House %d SpecOps: engineer %u turns back from (%d,%d) (threat %d), frame %d.\n",
							pHouse->ArrayIndex, capture.EngineerID, capture.TargetCell.X, capture.TargetCell.Y, threat, now);
						done = true;
					}
				}
			}

			if (done)
				state.Captures.erase(state.Captures.begin() + static_cast<std::ptrdiff_t>(i));
			else
				++i;
		}

		// Spies upkeep: a spy that vanished at the door went in.
		for (size_t i = 0; i < state.Spies.size();)
		{
			auto& spy = state.Spies[i];
			auto const pSpy = FindOwnFootByID(pHouse, spy.SpyID);
			bool done = false;

			if (!pSpy)
			{
				if (spy.LastDistance <= spy.DoorDistance)
				{
					state.InfiltrationCount++;
					Debug::Log("[NeuralAI] House %d SpecOps: spy %u infiltrated the building at (%d,%d) of house %d disguised as house %d, frame %d (infiltrations %d).\n",
						pHouse->ArrayIndex, spy.SpyID, spy.TargetCell.X, spy.TargetCell.Y, spy.TargetOwner, spy.DisguiseOwner, now,
						state.InfiltrationCount);
				}
				else
				{
					state.SpyLossFrames.push_back(now);

					if (state.SpyLossFrames.size() > 4)
						state.SpyLossFrames.erase(state.SpyLossFrames.begin());

					Debug::Log("[NeuralAI] House %d SpecOps: spy %u lost %d cells from (%d,%d) of house %d disguised as house %d, frame %d.\n",
						pHouse->ArrayIndex, spy.SpyID, spy.LastDistance, spy.TargetCell.X, spy.TargetCell.Y, spy.TargetOwner,
						spy.DisguiseOwner, now);
				}

				done = true;
			}
			else if (now - spy.StartFrame > 9000)
			{
				done = true;
			}
			else if (!pSpy->InLimbo)
			{
				spy.LastDistance = static_cast<int>(pSpy->GetMapCoords().DistanceFrom(spy.TargetCell));
				auto const pDisguiseHouse = SpyDisguiseHouse(abstract_cast<InfantryClass*>(pSpy));
				spy.DisguiseOwner = pDisguiseHouse ? pDisguiseHouse->ArrayIndex : -1;
			}

			if (done)
				state.Spies.erase(state.Spies.begin() + static_cast<std::ptrdiff_t>(i));
			else
				++i;
		}
	}

	// The engineer is on a SpecOps capture (Plans leave it there).
	bool IsSpecOpsEngineerBusy(HouseClass* pHouse, DWORD engineerID)
	{
		for (auto const& capture : GetSpecOpsState(pHouse).Captures)
		{
			if (capture.EngineerID == engineerID)
				return true;
		}

		return false;
	}

	int SpecOpsOpportunity(HouseClass* pHouse)
	{
		return GetSpecOpsState(pHouse).Opportunity;
	}

	void GetSpecOpsWork(HouseClass* pHouse, int& outTechValue, int& outSpyTargets, bool& outBridgeRepair, bool& outBridgeDemolition)
	{
		auto const& state = GetSpecOpsState(pHouse);
		outTechValue = state.EngineerType ? state.SafeTechValue + state.ContestedTechValue : 0;
		outSpyTargets = state.SpyTargets;
		outBridgeRepair = state.BridgeRepairTask;
		outBridgeDemolition = state.BridgeDemolitionTask;
	}

	void NoteSpecOpsSpy(HouseClass* pHouse, FootClass* pSpy, BuildingClass* pTarget)
	{
		if (!pHouse || !pSpy || !pTarget)
			return;

		auto& state = GetSpecOpsState(pHouse);

		for (auto& spy : state.Spies)
		{
			if (spy.SpyID == pSpy->UniqueID)
			{
				if (spy.TargetID != pTarget->UniqueID)
				{
					spy.TargetID = pTarget->UniqueID;
					spy.TargetCell = pTarget->GetMapCoords();
					spy.TargetOwner = pTarget->Owner ? pTarget->Owner->ArrayIndex : -1;
					spy.DoorDistance = 2 + std::max<int>(pTarget->Type->GetFoundationWidth(), pTarget->Type->GetFoundationHeight(false));
					spy.StartFrame = Unsorted::CurrentFrame;
				}

				return;
			}
		}

		NeuralSpecOpsSpy spy;
		spy.SpyID = pSpy->UniqueID;
		spy.TargetID = pTarget->UniqueID;
		spy.TargetCell = pTarget->GetMapCoords();
		spy.TargetOwner = pTarget->Owner ? pTarget->Owner->ArrayIndex : -1;
		spy.DoorDistance = 2 + (pTarget->Type ? std::max<int>(pTarget->Type->GetFoundationWidth(), pTarget->Type->GetFoundationHeight(false)) : 1);
		spy.LastDistance = static_cast<int>(pSpy->GetMapCoords().DistanceFrom(spy.TargetCell));
		spy.StartFrame = Unsorted::CurrentFrame;
		auto const pDisguiseHouse = SpyDisguiseHouse(abstract_cast<InfantryClass*>(pSpy));
		spy.DisguiseOwner = pDisguiseHouse ? pDisguiseHouse->ArrayIndex : -1;
		state.Spies.push_back(spy);
		Debug::Log("[NeuralAI] House %d SpecOps: spy %u (%s) -> %s of house %d at (%d,%d) disguised as house %d, frame %d.\n",
			pHouse->ArrayIndex, spy.SpyID, pSpy->GetTechnoType() ? pSpy->GetTechnoType()->ID : "?",
			pTarget->Type ? pTarget->Type->ID : "?", spy.TargetOwner, spy.TargetCell.X, spy.TargetCell.Y, spy.DisguiseOwner,
			spy.StartFrame);
	}

	// Best known neutral tech for this engineer: worth over distance, reachable, out of known
	// enemy defense reach, not blocked by recent failures nor taken by another engineer.
	// outContested: enemies were seen near it (an escort should go first).
	BuildingClass* PickSpecOpsCaptureTarget(HouseClass* pHouse, FootClass* pEngineer, bool& outContested)
	{
		outContested = false;
		auto& state = GetSpecOpsState(pHouse);
		const int now = Unsorted::CurrentFrame;
		const int engineerZone = ObjectZone(pEngineer, MovementZone::Normal);
		const CellStruct from = pEngineer->GetMapCoords();
		BuildingClass* pBest = nullptr;
		int bestScore = 0;

		for (auto const pBld : BuildingClass::Array)
		{
			if (!pBld || pBld->InLimbo || !pBld->IsAlive || !pBld->Type || !pBld->Type->Capturable || !pBld->Owner
				|| !pBld->Owner->IsNeutral())
			{
				continue;
			}

			auto const pTech = FindKnownTech(state, pBld->UniqueID);

			if (!pTech || pTech->OwnerIndex >= 0 || now < pTech->BlockedUntilFrame
				|| IsSpecOpsCaptureTarget(pHouse, pTech->ID, pEngineer->UniqueID)
				|| !CanEngineerReachTech(pHouse, *pTech, engineerZone, abstract_cast<InfantryClass*>(pEngineer)
					? abstract_cast<InfantryClass*>(pEngineer)->Type : nullptr)
				|| IsCellInKnownEnemyDefenseReach(pHouse, pTech->Cell, 2))
			{
				continue;
			}

			const int threat = VisibleEnemyThreatNear(pHouse, pTech->Cell, 7);
			// After a loss there (or next to it) the next engineer goes with an escort.
			const bool contested = threat > 0 || pTech->Failures > 0;

			// A tech held by a visible force much bigger than an escort could answer is not worth a walk.
			if (threat > 2400)
				continue;

			const int dist = static_cast<int>(from.DistanceFrom(pTech->Cell));
			const int score = pTech->Value * 1000 / (dist + 12) - (contested ? 15 : 0);

			if (score > bestScore)
			{
				bestScore = score;
				pBest = pBld;
				outContested = contested;
			}
		}

		return pBest;
	}

	// Idle armed ground units and soldiers of ours near a tech, nearest first (UniqueID on ties),
	// not in a plan nor escorting another capture. Specialists (engineers, spies, thieves) and
	// harvesters never escort.
	void CollectSpecOpsEscorts(HouseClass* pHouse, const CellStruct& cell, int maxCount, std::vector<FootClass*>& out)
	{
		out.clear();
		auto const& state = GetSpecOpsState(pHouse);
		std::vector<std::pair<int, FootClass*>> candidates;

		auto consider = [&](FootClass* pFoot)
		{
			// Guarding, or just repositioning (a move with no target, out of any plan, healthy):
			// units entering, rescuing or retreating to repair keep their job.
			const bool repositioning = pFoot->CurrentMission == Mission::Move && pFoot->GetHealthPercentage() >= 0.6;

			if (pFoot->Target || IsPlanMember(pHouse, pFoot->UniqueID)
				|| (pFoot->CurrentMission != Mission::Guard && pFoot->CurrentMission != Mission::Area_Guard && !repositioning))
			{
				return;
			}

			for (auto const& capture : state.Captures)
			{
				if (std::find(capture.Escorts.begin(), capture.Escorts.end(), pFoot->UniqueID) != capture.Escorts.end())
					return;
			}

			const int dist = static_cast<int>(pFoot->GetMapCoords().DistanceFrom(cell));

			if (dist <= 45)
				candidates.emplace_back(dist, pFoot);
		};

		for (auto const pUnit : UnitClass::Array)
		{
			if (pUnit && pUnit->Owner == pHouse && pUnit->IsAlive && !pUnit->InLimbo && pUnit->Type && !pUnit->Type->Naval
				&& !pUnit->Type->Harvester && !pUnit->Type->ResourceGatherer && !pUnit->Type->DeploysInto
				&& GroundRangeOf(pUnit->Type) > 0)
			{
				consider(pUnit);
			}
		}

		for (auto const pInf : InfantryClass::Array)
		{
			if (pInf && pInf->Owner == pHouse && pInf->IsAlive && !pInf->InLimbo && pInf->Type && !pInf->Type->Engineer
				&& !IsSpyInfantryType(pInf->Type) && !pInf->Type->Agent && !pInf->Type->Thief && !pInf->Type->VehicleThief
				&& !pInf->Type->Slaved && GroundRangeOf(pInf->Type) > 0)
			{
				consider(pInf);
			}
		}

		std::sort(candidates.begin(), candidates.end(), [](const auto& a, const auto& b)
		{
			return a.first != b.first ? a.first < b.first : a.second->UniqueID < b.second->UniqueID;
		});

		for (auto const& [dist, pFoot] : candidates)
		{
			if (static_cast<int>(out.size()) >= maxCount)
				break;

			out.push_back(pFoot);
		}
	}

	// Expert 4 engineer: take (or keep) a capture op, send the escort first when enemies were
	// seen near the tech and walk in once the way is clear. Returns true when the engineer got
	// its job for this tick (wait or go). dispatch is the micro's order helper (ferries included).
	template <typename DispatchFn>
	bool RunSpecOpsEngineer(HouseClass* pHouse, FootClass* pEngineer, bool expertActive, DispatchFn&& dispatch)
	{
		auto& state = GetSpecOpsState(pHouse);
		const int now = Unsorted::CurrentFrame;
		NeuralSpecOpsCapture* pCapture = nullptr;

		for (auto& capture : state.Captures)
		{
			if (capture.EngineerID == pEngineer->UniqueID)
			{
				pCapture = &capture;
				break;
			}
		}

		BuildingClass* pTarget = pCapture ? FindBuildingByID(pCapture->TargetID) : nullptr;

		if (!pCapture)
		{
			if (!expertActive)
				return false;

			bool contested = false;
			pTarget = PickSpecOpsCaptureTarget(pHouse, pEngineer, contested);

			if (!pTarget)
				return false;

			NeuralSpecOpsCapture capture;
			capture.EngineerID = pEngineer->UniqueID;
			capture.TargetID = pTarget->UniqueID;
			capture.TargetCell = pTarget->GetMapCoords();
			capture.StartFrame = now;

			if (contested)
			{
				std::vector<FootClass*> escorts;
				CollectSpecOpsEscorts(pHouse, capture.TargetCell, 3, escorts);

				// No engineer walks alone where enemies were seen or engineers were lost: with
				// nobody free to escort it, that tech waits and another one is picked next tick.
				if (escorts.empty())
				{
					if (auto const pTech = FindKnownTech(state, capture.TargetID))
						pTech->BlockedUntilFrame = std::max(pTech->BlockedUntilFrame, now + 900);

					return false;
				}

				auto const pEscortGoal = MapClass::Instance.TryGetCellAt(capture.TargetCell);

				for (auto const pEscort : escorts)
				{
					capture.Escorts.push_back(pEscort->UniqueID);

					if (pEscortGoal)
						dispatch(pEscort, pEscortGoal, Mission::AttackMove, false);
				}
			}

			state.Captures.push_back(capture);
			pCapture = &state.Captures.back();
			Debug::Log("[NeuralAI] House %d SpecOps: engineer %u -> %s at (%d,%d)%s, %d escorts, frame %d.\n",
				pHouse->ArrayIndex, capture.EngineerID, pTarget->Type->ID, capture.TargetCell.X, capture.TargetCell.Y,
				contested ? " (contested)" : "", static_cast<int>(capture.Escorts.size()), now);
		}
		else if (!pTarget || pCapture->Dispatched)
		{
			// Upkeep closes ops on lost targets; a dispatched engineer back on guard tries again.
			if (!pTarget)
				return false;

			pCapture->Dispatched = false;
		}

		auto const pTargetCell = MapClass::Instance.TryGetCellAt(pCapture->TargetCell);
		int escortsAlive = 0;
		bool escortArrived = false;

		for (const DWORD id : pCapture->Escorts)
		{
			auto const pEscort = FindOwnFootByID(pHouse, id);

			if (!pEscort)
				continue;

			escortsAlive++;

			if (pEscort->GetMapCoords().DistanceFrom(pCapture->TargetCell) <= 6.0)
				escortArrived = true;
			else if (pTargetCell && !pEscort->Target && (pEscort->CurrentMission == Mission::Guard || pEscort->CurrentMission == Mission::Area_Guard))
				dispatch(pEscort, pTargetCell, Mission::AttackMove, false);
		}

		const int threat = VisibleEnemyThreatNear(pHouse, pCapture->TargetCell, 7);
		const bool waitForEscort = escortsAlive > 0 && !escortArrived && now - pCapture->StartFrame < 1800;
		const bool go = (threat == 0 && !waitForEscort) || escortArrived
			|| (escortsAlive == 0 && now - pCapture->StartFrame >= 900 && threat < 600);

		if (go)
		{
			pCapture->Dispatched = dispatch(pEngineer, pTarget, Mission::Enter, false);
			pCapture->DispatchFrame = now;

			if (!pCapture->Dispatched)
				FailSpecOpsCapture(state, pCapture->TargetID, 1800);
		}
		else if (escortsAlive == 0 && now - pCapture->StartFrame >= 900)
		{
			// No escort and too many enemies there: try another tech later.
			FailSpecOpsCapture(state, pCapture->TargetID, 1800);
			state.Captures.erase(state.Captures.begin() + (pCapture - state.Captures.data()));

			return false;
		}

		return true;
	}
}

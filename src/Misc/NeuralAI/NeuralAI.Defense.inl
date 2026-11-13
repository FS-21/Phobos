// NeuralAI.Defense.inl: Defense ledger, defense budget and Expert 7 (Forward Defense & Chokepoint Siege):
// how many defenses pay and where. Part of NeuralAI.cpp (included in order; one translation unit).

namespace
{
	// How long a defense destroyed without a kill keeps the next tower away from its spot.
	constexpr int WastedDefenseSpotFrames = 9000;

	bool IsLedgerDefense(const BuildingTypeClass* pType)
	{
		return pType && pType->IsBaseDefense && !pType->InvisibleInGame && !IsTunnelNetworkBuilding(pType);
	}

	bool IsPureAADefense(BuildingTypeClass* pType)
	{
		auto const pWS = pType ? pType->GetWeapon(0) : nullptr;
		auto const pW = pWS ? pWS->WeaponType : nullptr;

		return pW && pW->Projectile && pW->Projectile->AA && !pW->Projectile->AG;
	}

	int ChebyshevCells(const CellStruct& a, const CellStruct& b)
	{
		return std::max(std::abs(a.X - b.X), std::abs(a.Y - b.Y));
	}

	// Armed combat unit that counts as army (not a gatherer, deployer, engineer or spawned aircraft).
	bool IsArmyTechnoType(TechnoTypeClass* pType)
	{
		if (!pType || pType->Spawned)
			return false;

		if (auto const pUnitType = abstract_cast<UnitTypeClass*>(pType))
		{
			if (pUnitType->Harvester || pUnitType->ResourceGatherer || pUnitType->DeploysInto)
				return false;
		}

		if (auto const pInfType = abstract_cast<InfantryTypeClass*>(pType))
		{
			if (pInfType->Engineer)
				return false;
		}

		auto const pWS = pType->GetWeapon(0);

		return pWS && pWS->WeaponType && pWS->WeaponType->Damage > 0;
	}

	void RecordDefensePlaced(HouseClass* pHouse, BuildingTypeClass* pType)
	{
		if (!pHouse || !IsLedgerDefense(pType))
			return;

		auto& macro = GetMacroState(pHouse);
		macro.DefenseSpent += pType->GetActualCost(pHouse);
		macro.DefensesBuilt++;
		// The budget must see the new tower before the next production pick.
		macro.DefenseBudgetFrame = -100000;
	}

	// Defense budget, payback and Expert 7 pressure, refreshed every 60 frames. Fog-fair: our own
	// objects, the enemy sightings memory and the enemy structures this house has seen.
	//
	// Budget: our defenses may be worth 2500 + 3/4 of our armed army + 700 per refinery, scaled by
	// their payback (enemy value they killed against their own losses, 30..150%), cut by the seen
	// artillery share when it outranges every tower we can build, plus 2000 while our structures are
	// being hit. Towers lost lately count against it too (rebuilding what keeps dying is spending).
	// Towers beyond that took the money the army and economy needed.
	//
	// Pressure (grades Expert 7): enemy forces seen near our structures in the last 450 frames (by
	// combat value, up to 360), our structures hit in the last 900 frames (80 each, up to 4),
	// harvesters under fire (110 each, up to 2), known enemy defenses facing our structures (40 each,
	// up to 3: artillery answers those, not towers). Two thirds when our army at home already doubles
	// the threat, cut by the seen artillery share (20%+) when it outranges every tower we can build,
	// half when the budget is spent, scaled by the payback below 100%, none when broke.
	void RefreshDefenseBudget(HouseClass* pHouse)
	{
		const NeuralSlowStep slowStep { "RefreshDefenseBudget" };

		auto& macro = GetMacroState(pHouse);
		const int now = Unsorted::CurrentFrame;

		if (now - macro.DefenseBudgetFrame < 60 && now >= macro.DefenseBudgetFrame)
			return;

		macro.DefenseBudgetFrame = now;

		if (now - macro.RecentDefenseLossDecayFrame >= 900 || now < macro.RecentDefenseLossDecayFrame)
		{
			macro.RecentDefenseLossDecayFrame = now;
			macro.RecentDefenseLossValue = macro.RecentDefenseLossValue * 3 / 4;
		}

		if (now - macro.BestTowerRangeFrame >= 900 || now < macro.BestTowerRangeFrame)
		{
			macro.BestTowerRangeFrame = now;
			int bestRange = 0;

			for (auto const pType : BuildingTypeClass::Array)
			{
				if (IsLedgerDefense(pType) && !IsPureAADefense(pType) && CanBuildUnderHumanRules(pHouse, pType))
					bestRange = std::max(bestRange, GroundRangeOf(pType) / Unsorted::LeptonsPerCell);
			}

			macro.BestTowerRangeCells = bestRange;
		}

		int defenseValue = 0;
		int refineries = 0;
		std::vector<CellStruct> assetCells;

		for (auto const pBld : pHouse->Buildings)
		{
			if (!pBld || pBld->InLimbo || !pBld->IsAlive || !pBld->Type || pBld->Type->InvisibleInGame)
				continue;

			if (IsLedgerDefense(pBld->Type))
			{
				defenseValue += pBld->Type->GetActualCost(pHouse);
			}
			else if (!pBld->Type->IsBaseDefense && !pBld->Type->Insignificant)
			{
				assetCells.push_back(pBld->GetMapCoords());

				if (pBld->Type->Refinery || pBld->Type->ResourceDestination)
					refineries++;
			}
		}

		const CellStruct home = GetHouseHomeCell(pHouse);
		int armyValue = 0;
		int armyNearHome = 0;
		std::vector<CellStruct> damagedHarvesters;

		auto countArmy = [&](TechnoClass* pTechno)
		{
			if (!pTechno || pTechno->Owner != pHouse || pTechno->InLimbo || !pTechno->IsAlive)
				return;

			auto const pType = pTechno->GetTechnoType();

			if (!pType)
				return;

			if (auto const pUnitType = abstract_cast<UnitTypeClass*>(pType))
			{
				if ((pUnitType->Harvester || pUnitType->ResourceGatherer) && pTechno->GetHealthPercentage() < 0.85)
					damagedHarvesters.push_back(pTechno->GetMapCoords());
			}

			if (!IsArmyTechnoType(pType))
				return;

			armyValue += pType->GetActualCost(pHouse);

			if (home != CellStruct::Empty && ChebyshevCells(pTechno->GetMapCoords(), home) <= 20)
				armyNearHome += GetCombatValueOfType(pType, false);
		};

		for (auto const pUnit : UnitClass::Array)
			countArmy(pUnit);

		for (auto const pInf : InfantryClass::Array)
			countArmy(pInf);

		for (auto const pAir : AircraftClass::Array)
			countArmy(pAir);

		const int efficiency = std::clamp((macro.DefenseKillValue + 2000) * 100 / (macro.DefenseLostValue + 2000), 30, 200);

		int recentAttacks = 0;

		for (auto const& rec : macro.AttackedBuildingsLIFO)
		{
			if (now - rec.AttackFrame <= 900 && now >= rec.AttackFrame)
				recentAttacks++;
		}

		int cap = 2500 + armyValue * 3 / 4 + refineries * 700;
		cap = cap * std::min(efficiency, 150) / 100;

		if (recentAttacks > 0)
			cap += 2000;

		// Seen enemy forces: value near our structures (recent sightings), harassed harvesters, and
		// the artillery share and reach (sums and maxima only, so the map order does not matter).
		int homeThreat = 0;
		int totalValue = 0;
		int artilleryValue = 0;
		int artilleryRange = 0;
		std::vector<bool> harvesterHarassed(damagedHarvesters.size(), false);

		for (auto const& [uid, sighting] : macro.EnemySightings)
		{
			if (!sighting.Type)
				continue;

			const int age = now - sighting.LastSeenFrame;

			if (age < 0 || age > EnemySightingMemoryFrames)
				continue;

			const int weighted = sighting.Value * (EnemySightingMemoryFrames - age) / EnemySightingMemoryFrames;
			totalValue += weighted;

			if (sighting.Category == EnemyForceCategory::Artillery)
			{
				artilleryValue += weighted;
				artilleryRange = std::max(artilleryRange,
					GroundRangeOf(const_cast<TechnoTypeClass*>(sighting.Type)) / Unsorted::LeptonsPerCell);
			}

			if (sighting.LastCoords == CellStruct::Empty)
				continue;

			if (age <= 450)
			{
				for (auto const& ac : assetCells)
				{
					if (ChebyshevCells(ac, sighting.LastCoords) <= 16)
					{
						homeThreat += sighting.Value;
						break;
					}
				}
			}

			if (age <= 150 && sighting.Category != EnemyForceCategory::Air)
			{
				for (size_t i = 0; i < damagedHarvesters.size(); ++i)
				{
					if (ChebyshevCells(damagedHarvesters[i], sighting.LastCoords) <= 8)
						harvesterHarassed[i] = true;
				}
			}
		}

		const int harassed = static_cast<int>(std::count(harvesterHarassed.begin(), harvesterHarassed.end(), true));
		int frontDefenses = 0;

		for (auto const& info : GetKnownEnemyBuildingCache(pHouse).Entries)
		{
			if (!info.IsDefense || info.IsAA)
				continue;

			for (auto const& ac : assetCells)
			{
				if (ChebyshevCells(ac, info.Coords) <= 20)
				{
					frontDefenses++;
					break;
				}
			}

			if (frontDefenses >= 3)
				break;
		}

		int pressure = std::min(360, homeThreat) + std::min(4, recentAttacks) * 80 + std::min(2, harassed) * 110
			+ std::min(3, frontDefenses) * 40;

		if (homeThreat > 0 && armyNearHome >= homeThreat * 2)
			pressure = pressure * 2 / 3;

		const int artilleryShare = totalValue > 0 ? artilleryValue * 100 / totalValue : 0;

		// Enemy artillery that outranges every tower we can build shells them from outside their
		// reach: the more of it, the less towers are the answer (budget and pressure).
		if (artilleryShare >= 20 && artilleryRange > macro.BestTowerRangeCells)
		{
			cap = cap * (100 - artilleryShare) / 100;
			pressure = pressure * (100 - artilleryShare) / 100;
		}

		if (defenseValue >= cap)
			pressure /= 2;

		pressure = pressure * std::min(efficiency, 100) / 100;

		if (macro.BrokeHoldActive || pHouse->OwnedBuildings < 5)
			pressure = 0;

		macro.OwnDefenseValue = defenseValue;
		macro.OwnArmyValue = armyValue;
		macro.DefenseValueCap = cap;
		macro.DefenseEfficiencyPct = efficiency;
		macro.SeenArtilleryShare = artilleryShare;
		macro.SeenArtilleryRangeCells = artilleryRange;
		macro.DefensePressure = pressure;
	}

	// Production gate and score shift of a defense type. Blocked while broke and once the budget
	// would be exceeded (the first two ground towers and the first AA one always pass). Less eager
	// above 60% of the budget and when our towers die more than they kill; ground towers that seen
	// enemy artillery outranges lose up to 600 points.
	int DefenseBudgetShift(HouseClass* pHouse, BuildingTypeClass* pType, bool isPureAA, int totalGroundDefenses,
		int totalAADefenses, bool& blocked)
	{
		blocked = false;
		RefreshDefenseBudget(pHouse);

		auto const& macro = GetMacroState(pHouse);

		if (macro.BrokeHoldActive)
		{
			blocked = true;
			return 0;
		}

		const int cost = pType->GetActualCost(pHouse);
		const bool firstOnes = isPureAA ? (totalAADefenses < 1) : (totalGroundDefenses < 2);

		if (!firstOnes && macro.OwnDefenseValue + macro.RecentDefenseLossValue + cost > macro.DefenseValueCap)
		{
			blocked = true;
			return 0;
		}

		int shift = 0;

		if (macro.DefenseValueCap > 0)
		{
			const int usage = (macro.OwnDefenseValue + macro.RecentDefenseLossValue + cost) * 100 / macro.DefenseValueCap;

			if (usage > 60)
				shift -= (usage - 60) * 10;
		}

		if (macro.DefenseEfficiencyPct < 100)
			shift -= (100 - macro.DefenseEfficiencyPct) * 6;

		if (!isPureAA && macro.SeenArtilleryShare >= 15
			&& GroundRangeOf(pType) / Unsorted::LeptonsPerCell < macro.SeenArtilleryRangeCells)
		{
			shift -= std::min(600, macro.SeenArtilleryShare * 12);
		}

		return shift;
	}

	// Known enemy defenses that hit the ground, with their reach in cells (fog-fair: seen ones).
	void CollectEnemyDefenseReach(HouseClass* pHouse, std::vector<std::pair<CellStruct, int>>& out)
	{
		out.clear();

		for (auto const& info : GetKnownEnemyBuildingCache(pHouse).Entries)
		{
			if (!info.IsDefense)
				continue;

			const int reach = GroundRangeOf(info.Building->Type) / Unsorted::LeptonsPerCell;

			if (reach > 0)
				out.emplace_back(info.Coords, reach);
		}
	}

	// Placement shift of a defense cell: inside the reach of a known enemy defense that outranges it
	// (or that it cannot answer from there) it would only be shelled; near a spot where one of our
	// towers died without a kill it is likely to die the same way.
	int DefenseSpotShift(HouseClass* pHouse, const CellStruct& cell, int towerRangeCells,
		const std::vector<std::pair<CellStruct, int>>& enemyReach)
	{
		int shift = 0;

		for (auto const& [enemyCell, reach] : enemyReach)
		{
			const int d = ChebyshevCells(cell, enemyCell);

			if (d <= reach + 1 && (reach > towerRangeCells || d > towerRangeCells))
				shift -= 2500 + (reach + 1 - d) * 100;
		}

		const int now = Unsorted::CurrentFrame;

		for (auto const& spot : GetMacroState(pHouse).WastedDefenseSpots)
		{
			const int age = now - spot.Frame;

			if (age < 0 || age > WastedDefenseSpotFrames)
				continue;

			if (ChebyshevCells(cell, spot.Coords) <= std::max(3, spot.KillerRangeCells / 2))
				shift -= 1500 * (WastedDefenseSpotFrames - age) / WastedDefenseSpotFrames;
		}

		return shift;
	}

	// Expert 7 anchor: our structure most in need of a tower, from what really came at us. Each
	// structure sums the value of enemy forces seen within 14 cells (age-decayed sightings; air for AA
	// towers, ground and naval otherwise) plus 150 per hit it took in the last 2700 frames, times 3
	// for refineries and ConYards, 2 for factories, divided by 1 + our matching towers within 7 cells.
	// Enemy units that outrange a ground tower are left out. The threat cell is the value-weighted
	// centroid of what came. Needs 60 value at least.
	bool FindDefenseNeed(HouseClass* pHouse, bool pureAA, int towerRangeCells, CellStruct& outAsset, CellStruct& outThreat)
	{
		auto const& macro = GetMacroState(pHouse);
		const int now = Unsorted::CurrentFrame;

		struct SeenThreat
		{
			CellStruct Coords;
			int Value;
		};

		std::vector<SeenThreat> seen;

		for (auto const& [uid, sighting] : macro.EnemySightings)
		{
			if (!sighting.Type || sighting.LastCoords == CellStruct::Empty)
				continue;

			if ((sighting.Category == EnemyForceCategory::Air) != pureAA)
				continue;

			const int age = now - sighting.LastSeenFrame;

			if (age < 0 || age > EnemySightingMemoryFrames)
				continue;

			// A ground tower cannot answer what outranges it: that is a job for the army.
			if (!pureAA && towerRangeCells > 0
				&& GroundRangeOf(const_cast<TechnoTypeClass*>(sighting.Type)) / Unsorted::LeptonsPerCell > towerRangeCells + 1)
			{
				continue;
			}

			const int weighted = sighting.Value * (EnemySightingMemoryFrames - age) / EnemySightingMemoryFrames;

			if (weighted > 0)
				seen.push_back({ sighting.LastCoords, weighted });
		}

		if (seen.empty() && macro.AttackedBuildingsLIFO.empty())
			return false;

		std::vector<CellStruct> ownTowers;

		for (auto const pBld : pHouse->Buildings)
		{
			if (pBld && !pBld->InLimbo && pBld->IsAlive && IsLedgerDefense(pBld->Type) && IsPureAADefense(pBld->Type) == pureAA)
				ownTowers.push_back(pBld->GetMapCoords());
		}

		long long bestScore = 0;

		for (auto const pBld : pHouse->Buildings)
		{
			if (!pBld || pBld->InLimbo || !pBld->IsAlive || !pBld->Type || pBld->Type->IsBaseDefense
				|| pBld->Type->Insignificant || pBld->Type->InvisibleInGame)
			{
				continue;
			}

			auto const pType = pBld->Type;
			const CellStruct bc = pBld->GetMapCoords();
			long long threat = 0;
			long long sumX = 0;
			long long sumY = 0;

			for (auto const& s : seen)
			{
				if (ChebyshevCells(bc, s.Coords) <= 14)
				{
					threat += s.Value;
					sumX += static_cast<long long>(s.Coords.X) * s.Value;
					sumY += static_cast<long long>(s.Coords.Y) * s.Value;
				}
			}

			for (auto const& rec : macro.AttackedBuildingsLIFO)
			{
				if (now - rec.AttackFrame > 2700 || rec.AttackerCoords == CellStruct::Empty
					|| (rec.Threat == NeuralThreatCategory::Air) != pureAA || ChebyshevCells(rec.BuildingCoords, bc) > 3)
				{
					continue;
				}

				threat += 150;
				sumX += static_cast<long long>(rec.AttackerCoords.X) * 150;
				sumY += static_cast<long long>(rec.AttackerCoords.Y) * 150;
			}

			if (threat < 60)
				continue;

			const int weight = (pType->Refinery || pType->ResourceDestination || pType->ConstructionYard) ? 3
				: (pType->Factory != AbstractType::None ? 2 : 1);
			int cover = 0;

			for (auto const& tc : ownTowers)
			{
				if (ChebyshevCells(tc, bc) <= 7)
					cover++;
			}

			const long long score = threat * weight / (1 + cover);

			if (score > bestScore)
			{
				bestScore = score;
				outAsset = bc;
				outThreat = CellStruct { static_cast<short>(sumX / threat), static_cast<short>(sumY / threat) };
			}
		}

		return bestScore > 0;
	}
}

// Defense ledger: kills of our towers (by UniqueID) and towers lost, from the engine's destruction
// record of every techno (same on every client).
void NeuralAI::OnTechnoDestroyed(TechnoClass* pVictim, TechnoClass* pKiller)
{
	if (!pVictim)
		return;

	auto const pVictimOwner = pVictim->Owner;

	if (pKiller && pKiller->WhatAmI() == AbstractType::Building && pKiller->Owner && pKiller->Owner != pVictimOwner
		&& (!pVictimOwner || !pKiller->Owner->IsAlliedWith(pVictimOwner)) && NeuralAI::IsEnabledForHouse(pKiller->Owner))
	{
		if (IsLedgerDefense(static_cast<BuildingClass*>(pKiller)->Type))
		{
			auto& macro = GetMacroState(pKiller->Owner);
			auto const pVictimType = pVictim->GetTechnoType();
			macro.DefenseKills++;
			macro.DefenseKillValue += pVictimType ? pVictimType->Cost : 0;
			macro.DefenseKillsById[pKiller->UniqueID]++;
		}
	}

	if (pVictim->WhatAmI() != AbstractType::Building || !pVictimOwner || !NeuralAI::IsEnabledForHouse(pVictimOwner))
		return;

	auto const pVictimType = static_cast<BuildingClass*>(pVictim)->Type;

	if (!IsLedgerDefense(pVictimType))
		return;

	auto& macro = GetMacroState(pVictimOwner);
	macro.DefensesLost++;
	macro.DefenseLostValue += pVictimType->GetActualCost(pVictimOwner);
	macro.RecentDefenseLossValue += pVictimType->GetActualCost(pVictimOwner);

	auto const it = macro.DefenseKillsById.find(pVictim->UniqueID);
	const bool killedNothing = it == macro.DefenseKillsById.end() || it->second <= 0;

	if (it != macro.DefenseKillsById.end())
		macro.DefenseKillsById.erase(it);

	if (!killedNothing)
		return;

	macro.DefensesLostNoKill++;

	NeuralWastedDefenseSpot spot;
	spot.Coords = pVictim->GetMapCoords();
	spot.Frame = Unsorted::CurrentFrame;
	spot.KillerRangeCells = (pKiller && pKiller->GetTechnoType())
		? GroundRangeOf(pKiller->GetTechnoType()) / Unsorted::LeptonsPerCell
		: 0;
	macro.WastedDefenseSpots.push_back(spot);

	if (macro.WastedDefenseSpots.size() > 24)
		macro.WastedDefenseSpots.erase(macro.WastedDefenseSpots.begin());
}

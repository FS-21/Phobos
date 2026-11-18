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

	// TEMP diagnostic (tank census, 2026-11-18): a main battle tank of ours died: what it was doing,
	// what killed it, how far from our sites and after how long (user: why do the houses stay below
	// the tank floor in mid game?). Log only; the totals go out with the tank floor line.
	void NoteMainTankDeath(HouseClass* pOwner, UnitClass* pTank, TechnoClass* pKiller)
	{
		constexpr double TankCensusHomeCells = 25.0;
		constexpr int TankCensusDeathLines = 80;
		static const char* const roleNames[] = { "guard", "intercept", "plan", "wave", "scout", "repair", "home", "away" };
		static const char* const killerNames[] = { "vehicle", "infantry", "defense", "aircraft", "other" };

		auto& macro = GetMacroState(pOwner);
		const DWORD uid = pTank->UniqueID;
		const CellStruct at = pTank->GetMapCoords();
		const CellStruct destCell = pTank->Destination ? CellClass::Coord2Cell(pTank->Destination->GetCoords()) : at;
		double siteCells = -1.0;
		double destSiteCells = -1.0;

		for (auto const pBld : pOwner->Buildings)
		{
			if (pBld && pBld->IsAlive && !pBld->InLimbo && pBld->Type
				&& (pBld->Type->ConstructionYard || pBld->Type->Refinery || pBld->Type->Factory != AbstractType::None))
			{
				const CellStruct bldCell = pBld->GetMapCoords();
				const double distance = bldCell.DistanceFrom(at);
				const double destDistance = bldCell.DistanceFrom(destCell);

				if (siteCells < 0.0 || distance < siteCells)
					siteCells = distance;

				if (destSiteCells < 0.0 || destDistance < destSiteCells)
					destSiteCells = destDistance;
			}
		}

		const bool home = siteCells >= 0.0 && siteCells <= TankCensusHomeCells;
		const char* planName = "";
		size_t role = home ? 6 : 7;

		if (macro.TankGuard.contains(uid))
		{
			role = 0;
		}
		else if (macro.PlanMembers.contains(uid))
		{
			role = 2;

			for (size_t i = 0; i < macro.Plans.size(); ++i)
			{
				auto const& plan = macro.Plans[i];

				if (plan.Active && std::find(plan.Members.begin(), plan.Members.end(), uid) != plan.Members.end())
				{
					planName = NeuralPlanNames[i];
					role = i == static_cast<size_t>(NeuralPlanId::BaseThreatIntercept) ? 1 : 2;
					break;
				}
			}
		}
		else if (IsActiveScout(pOwner, uid))
		{
			role = 4;
		}
		else if (IsOnRepairTrip(pOwner, uid) || IsInTransportOp(pOwner, uid))
		{
			role = 5;
		}
		else if (pTank->MegaMission == Mission::AttackMove)
		{
			role = 3;
		}

		size_t killer = 4;

		if (pKiller)
		{
			switch (pKiller->WhatAmI())
			{
			case AbstractType::Building:
				killer = static_cast<BuildingClass*>(pKiller)->Type->IsBaseDefense ? 2 : 4;
				break;
			case AbstractType::Aircraft:
				killer = 3;
				break;
			case AbstractType::Infantry:
				killer = 1;
				break;
			case AbstractType::Unit:
				killer = 0;
				break;
			default:
				break;
			}
		}

		macro.TankCensusLost[role]++;
		macro.TankCensusKillers[killer]++;
		macro.TankCensusLostAtHome += home ? 1 : 0;
		int lived = -1;

		if (auto const itBirth = macro.TankCensusBirth.find(uid); itBirth != macro.TankCensusBirth.end())
		{
			lived = Unsorted::CurrentFrame - itBirth->second;
			macro.TankCensusLifeFrames += lived;
			macro.TankCensusLifeCount++;
			macro.TankCensusBirth.erase(itBirth);
		}

		static int deathLogSerial = -1;
		static int deathLogCount = 0;

		if (deathLogSerial != NeuralMatchSerial)
		{
			deathLogSerial = NeuralMatchSerial;
			deathLogCount = 0;
		}

		if (deathLogCount++ < TankCensusDeathLines)
		{
			// The last plan or wave that sent it and how that plan ended, what the engine had it doing,
			// how far its destination (and that from our sites) and its target were, how far the killer
			// stood and the nearest enemy building (user, 2026-11-18: what sends the tanks lost away
			// from home with no role there?).
			static const char* const missionNames[] = { "none", "sleep", "attack", "move", "qmove", "retreat", "guard", "sticky",
				"enter", "capture", "eaten", "harvest", "areaGuard", "return", "stop", "ambush", "hunt", "unload", "sabotage",
				"construction", "selling", "repair", "rescue", "missile", "harmless", "open", "patrol", "paradropApproach",
				"paradropOverfly", "wait", "attackMove", "spyplaneApproach", "spyplaneOverfly" };

			auto missionName = [](Mission mission)
			{
				const int index = static_cast<int>(mission) + 1;
				return index >= 0 && index < static_cast<int>(std::size(missionNames)) ? missionNames[index] : "?";
			};

			auto const& tags = CreditTags[std::clamp(pOwner->ArrayIndex, 0, 31)];
			auto const itTag = tags.find(uid);
			const int source = itTag != tags.end() ? itTag->second.CommandSource : -1;
			const int orderAge = itTag != tags.end() ? Unsorted::CurrentFrame - itTag->second.CommandFrame : -1;
			char order[128] = "none";

			if (source == CreditSourceWave)
			{
				sprintf_s(order, "wave %d frames ago", orderAge);
			}
			else if (source >= 0 && source < static_cast<int>(NeuralPlanId::Count))
			{
				const size_t id = static_cast<size_t>(source);
				const int endFrame = macro.PlanEndFrame[id];

				if (endFrame > 0 && endFrame >= itTag->second.CommandFrame)
				{
					sprintf_s(order, "%s %d frames ago, it ended %s %d frames ago", NeuralPlanNames[id], orderAge,
						macro.PlanEndedWell[id] ? "well" : "not well", Unsorted::CurrentFrame - endFrame);
				}
				else
				{
					sprintf_s(order, "%s %d frames ago, still on", NeuralPlanNames[id], orderAge);
				}
			}

			double enemyCells = -1.0;

			for (auto const pBld : BuildingClass::Array)
			{
				if (!pBld || !pBld->IsAlive || pBld->InLimbo || !pBld->Owner || pBld->Owner == pOwner || pOwner->IsAlliedWith(pBld->Owner)
					|| pBld->Owner->IsNeutral())
				{
					continue;
				}

				const double distance = pBld->GetMapCoords().DistanceFrom(at);

				if (enemyCells < 0.0 || distance < enemyCells)
					enemyCells = distance;
			}

			// The code that gave its last move, attack-move or dispatch order (NoteOrderCaller), and how
			// long ago (user, 2026-11-18: what sends the tanks into the enemy bases on a plain move?).
			char caller[128] = "none";
			auto const& callers = LastOrderCaller[std::clamp(pOwner->ArrayIndex, 0, 31)];

			if (auto const itCaller = callers.find(uid); itCaller != callers.end() && itCaller->second.File && itCaller->second.Kind)
			{
				sprintf_s(caller, "%s from %s:%d %d frames ago", itCaller->second.Kind, itCaller->second.File, itCaller->second.Line,
					Unsorted::CurrentFrame - itCaller->second.Frame);
			}

			Debug::Log("[NeuralAI] TEMP tank death: house %d %s %s%s%s killed by %s %s of house %d, %d cells from our nearest site, lived %d frames, frame %d;"
				" last order %s, mission %s/%s, destination %d cells away (%d from our nearest site), target %d cells away, killer %d cells away,"
				" nearest enemy building %d cells, last order given by %s.\n",
				pOwner->ArrayIndex, pTank->Type->ID, roleNames[role], *planName ? " " : "", planName, killerNames[killer],
				pKiller && pKiller->GetTechnoType() ? pKiller->GetTechnoType()->ID : "-", pKiller && pKiller->Owner ? pKiller->Owner->ArrayIndex : -1,
				static_cast<int>(siteCells), lived, Unsorted::CurrentFrame, order, missionName(pTank->CurrentMission), missionName(pTank->MegaMission),
				pTank->Destination ? static_cast<int>(destCell.DistanceFrom(at)) : -1, pTank->Destination ? static_cast<int>(destSiteCells) : -1,
				pTank->Target ? static_cast<int>(CellClass::Coord2Cell(pTank->Target->GetCoords()).DistanceFrom(at)) : -1,
				pKiller ? static_cast<int>(pKiller->GetMapCoords().DistanceFrom(at)) : -1, static_cast<int>(enemyCells), caller);
		}
	}
}

// Defense ledger: kills of our towers (by UniqueID) and towers lost, from the engine's destruction
// record of every techno (same on every client).
void NeuralAI::OnTechnoDestroyed(TechnoClass* pVictim, TechnoClass* pKiller)
{
	if (!pVictim)
		return;

	auto const pVictimOwner = pVictim->Owner;

	// TEMP diagnostic (tank census): the main battle tanks we lose.
	if (pVictimOwner && pVictim->WhatAmI() == AbstractType::Unit && NeuralAI::IsEnabledForHouse(pVictimOwner))
	{
		auto const pTank = static_cast<UnitClass*>(pVictim);

		if (pTank->Type && IsMainBattleTankType(pTank->Type))
			NoteMainTankDeath(pVictimOwner, pTank, pKiller);
	}

	// TEMP diagnostic (NoteOrderCaller): the record of a dead unit goes with it.
	if (pVictimOwner && pVictimOwner->ArrayIndex >= 0 && pVictimOwner->ArrayIndex < 32)
		LastOrderCaller[pVictimOwner->ArrayIndex].erase(pVictim->UniqueID);

	// TEMP diagnostic (rotatingRaids, 2026-11-18): who kills raid members (a unit, a defense, an
	// aircraft) and where the member stood against its group and its goal (user: units or defenses
	// in range?). Log only.
	if (pVictimOwner && pVictim->WhatAmI() != AbstractType::Building && NeuralAI::IsEnabledForHouse(pVictimOwner))
	{
		auto const& raid = GetMacroState(pVictimOwner).Plans[static_cast<size_t>(NeuralPlanId::RotatingRaids)];
		auto const itMember = raid.Active ? std::find(raid.Members.begin(), raid.Members.end(), pVictim->UniqueID) : raid.Members.end();

		if (raid.Active && itMember != raid.Members.end())
		{
			const size_t split = std::min<size_t>(raid.Members.size(), static_cast<size_t>(std::max(0, raid.AuxBaseline)));
			const size_t index = static_cast<size_t>(itMember - raid.Members.begin());
			const bool groupB = index >= split;
			const CellStruct goal = groupB ? raid.StagingCell : raid.TargetCell;
			const CellStruct victimCell = pVictim->GetMapCoords();
			int sumX = 0;
			int sumY = 0;
			int others = 0;

			for (auto const pTech : TechnoClass::Array)
			{
				if (!pTech || pTech == pVictim || !pTech->IsAlive || pTech->InLimbo || pTech->Owner != pVictimOwner)
					continue;

				auto const itOther = std::find(raid.Members.begin(), raid.Members.end(), pTech->UniqueID);

				if (itOther == raid.Members.end() || (static_cast<size_t>(itOther - raid.Members.begin()) >= split) != groupB)
					continue;

				const CellStruct cell = pTech->GetMapCoords();
				sumX += cell.X;
				sumY += cell.Y;
				others++;
			}

			const char* killerKind = "nobody";
			const char* killerType = "-";
			int killerHouse = -1;
			int killerCells = -1;
			int killerGoalCells = -1;

			if (pKiller)
			{
				switch (pKiller->WhatAmI())
				{
				case AbstractType::Building:
					killerKind = static_cast<BuildingClass*>(pKiller)->Type->IsBaseDefense ? "defense" : "building";
					break;
				case AbstractType::Aircraft:
					killerKind = "aircraft";
					break;
				case AbstractType::Infantry:
					killerKind = "infantry";
					break;
				default:
					killerKind = "vehicle";
					break;
				}

				killerType = pKiller->GetTechnoType() ? pKiller->GetTechnoType()->ID : "?";
				killerHouse = pKiller->Owner ? pKiller->Owner->ArrayIndex : -1;
				killerCells = static_cast<int>(pKiller->GetMapCoords().DistanceFrom(victimCell));
				killerGoalCells = goal != CellStruct::Empty ? static_cast<int>(pKiller->GetMapCoords().DistanceFrom(goal)) : -1;
			}

			const CellStruct centre = others > 0
				? CellStruct { static_cast<short>(sumX / others), static_cast<short>(sumY / others) }
				: CellStruct::Empty;

			Debug::Log("[NeuralAI] TEMP raid death: house %d member %s of group %c killed by %s %s of house %d %d cells away (%d from the goal); "
				"member %d cells from its goal, group centre %d from the goal and %d from the member (%d others), %d frames into the plan, "
				"arrived %d, had a target %d.\n",
				pVictimOwner->ArrayIndex, pVictim->GetTechnoType() ? pVictim->GetTechnoType()->ID : "?", groupB ? 'B' : 'A', killerKind,
				killerType, killerHouse, killerCells, killerGoalCells,
				goal != CellStruct::Empty ? static_cast<int>(victimCell.DistanceFrom(goal)) : -1,
				centre != CellStruct::Empty && goal != CellStruct::Empty ? static_cast<int>(centre.DistanceFrom(goal)) : -1,
				centre != CellStruct::Empty ? static_cast<int>(centre.DistanceFrom(victimCell)) : -1, others,
				Unsorted::CurrentFrame - raid.StartFrame, raid.Arrived ? 1 : 0, pVictim->Target ? 1 : 0);
		}
	}

	// Plan kills: the combat value of enemy units destroyed by a member of an active plan (Bait
	// Ambush is scored by what its members killed, not by what they saw).
	if (pKiller && pKiller->Owner && pVictimOwner && pKiller->Owner != pVictimOwner && pVictim->WhatAmI() != AbstractType::Building
		&& !pVictimOwner->IsNeutral() && !pKiller->Owner->IsAlliedWith(pVictimOwner) && NeuralAI::IsEnabledForHouse(pKiller->Owner))
	{
		auto& killerMacro = GetMacroState(pKiller->Owner);

		if (killerMacro.PlanMembers.find(pKiller->UniqueID) != killerMacro.PlanMembers.end())
		{
			for (auto& plan : killerMacro.Plans)
			{
				if (plan.Active && std::find(plan.Members.begin(), plan.Members.end(), pKiller->UniqueID) != plan.Members.end())
				{
					plan.KillValue += GetCombatValueOfType(pVictim->GetTechnoType(), false);
					break;
				}
			}
		}
	}

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

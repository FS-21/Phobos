// NeuralAI.AirForce.inl: the air force and Expert 8 (Airforce & Sky Dominance).
// Strike wings under any expert: the strike aircraft wait on their pads until all the ready ones
// together destroy a known target in one pass, fly there along safe lanes and come home the same
// way. Expert 8 adds the air work of a house (selector prior), interceptors against enemy air in
// sight and patrol stops over the front. Everything is fog-fair: known enemy buildings, enemy
// units on ground this house knows (IsCellKnownToAI), and the places where our own aircraft were
// shot down.
// Part of NeuralAI.cpp (included in order; one translation unit).

namespace
{
	int AntiAirRangeCells(TechnoTypeClass* pType);
	int AirStrikeWorthOf(const BuildingTypeClass* pType);

	struct NeuralAirStrikeOrder
	{
		DWORD TargetID { 0 };
		int Frame { 0 };
		CellStruct TargetCell { CellStruct::Empty };
		// The aircraft fired at the target (its load is no longer full): the pass is flown, it is on
		// its way home and joins the next wing once it lands.
		bool Returning { false };
	};

	// TEMP diagnostic: what the AI last had each aircraft do, for the loss and kill lines.
	enum class NeuralAirTask : unsigned char
	{
		None,
		Wing,
		Intercept,
		Harass,
		MindDive,
		HarvesterDive,
		Solo,
		Patrol,
		Pad,
		SafeHome
	};

	constexpr const char* NeuralAirTaskNames[] = { "none", "wing", "intercept", "harass", "mindDive", "harvesterDive", "solo", "patrol",
		"pad", "safeHome" };

	struct NeuralAirTaskRecord
	{
		NeuralAirTask Task { NeuralAirTask::None };
		int Frame { 0 };
	};

	// A wing on its strike, judged once none of it flies the strike any more (the wing lesson): its
	// target, the building type, its members and the frame it left.
	struct NeuralAirWingFlight
	{
		DWORD TargetID { 0 };
		const TechnoTypeClass* TargetType { nullptr };
		bool IsBuildingTarget { true };
		std::vector<DWORD> Members {};
		int Frame { 0 };
		// A pass of a wing of attrition (AirWingAttrition*): the target standing after it is no failure.
		bool Attrition { false };
	};

	TechnoClass* FindAirStrikeTechnoByID(DWORD uid)
	{
		if (uid == 0)
			return nullptr;

		for (auto const pTech : TechnoClass::Array)
		{
			if (pTech && pTech->UniqueID == uid && pTech->IsAlive && !pTech->InLimbo && pTech->Health > 0)
				return pTech;
		}

		return nullptr;
	}

	struct NeuralAirForceState
	{
		int MatchSerial { -1 };
		int SummaryFrame { -1 };
		// Combat value of our armed aircraft that hit the ground / the air (not spawned, no transports).
		int StrikeFleetValue { 0 };
		int InterceptorValue { 0 };
		int StrikeAircraft { 0 };
		// Armed air force aircraft out of the factory.
		int FleetCount { 0 };
		// Aircraft lost lately, 100 per aircraft, x7/8 every 900 frames.
		int RecentLosses { 0 };
		int LossDecayFrame { 0 };
		// Sum of the three best strike targets the current fleet could hit at its risk tolerance.
		int StrikeWork { 0 };
		int InterceptWork { 0 };
		bool CanProduceStriker { false };
		bool CanProduceInterceptor { false };
		bool CanBuildAirfield { false };
		// Centroid of our ground units fighting enemies we know of (Empty when there is no fight).
		CellStruct FrontCell { CellStruct::Empty };
		// Aircraft UniqueID -> the strike it flies for a wing. Key access only.
		std::unordered_map<DWORD, NeuralAirStrikeOrder> Orders {};
		// Aircraft UniqueID -> the building it last went for alone, outside a wing. Key access only.
		std::unordered_map<DWORD, NeuralAirStrikeOrder> SoloStrikes {};
		// Aircraft fielded this match, by UniqueID (sorted: the lost count never depends on order).
		std::map<DWORD, int> Fielded {};
		int WingsLaunched { 0 };
		// First frame the ready strike aircraft could destroy a target in one pass while others of
		// the fleet were still rearming or on their way home.
		int WingMassFrame { -1 };
		// TEMP diagnostic: aircraft UniqueID -> its last task (key access only), the last wait line,
		// and the enemy buildings our aircraft destroyed this match.
		std::unordered_map<DWORD, NeuralAirTaskRecord> Tasks {};
		int WingWaitLogFrame { -1 };
		int BuildingsDestroyed { 0 };
		// The wing lesson (AirWingLesson*): the wings on their strike, and the steps of the lesson with
		// the frame of the last failure.
		std::vector<NeuralAirWingFlight> Flights {};
		int WingLessonSteps { 0 };
		int WingLessonFrame { -1 };
	};

	std::array<NeuralAirForceState, 32> AirForceStates {};

	// Air force in an island war (user approved, 2026-11-20, A2 of the island air proposal, with the wing
	// lesson: «ok, A2»). On a map whose starting bases no land links, or with the enemy across the water,
	// the air force is a main arm and goes in wings only. Over 40 matches of 2026-11-20, 85% of the
	// aircraft of naval maps were lost, nearly all outside a strike of ours, in wings of 1 or 2; of 142
	// strike aircraft lost attacking while they waited for a wing, 84 had no order of ours (the engine puts
	// an armed aircraft on Guard to Attack, AircraftClass::Mission_Guard) and the rest had been sent home or
	// scouting. So in an island war:
	// - a wing leaves with AirWingMinAircraft ready at least, or the whole strike fleet when it is smaller;
	// - a wing that left its target standing, or lost half of its aircraft or more, asks the next ones for
	//   AirWingLessonPercent more of the damage the target needs per step (AirWingLessonMaxSteps at most, a
	//   step fading every AirWingLessonFadeFrames without a new failure); one that took its target ends it;
	// - a strike aircraft waiting on its pad sleeps instead of guarding (WaitOnAirPad), and one outside a
	//   strike or plan of ours that attacks something on the ground farther than AirPadGuardCells from our
	//   nearest helipad goes back to its pad.
	constexpr int AirWingMinAircraft = 4;
	constexpr int AirWingLessonPercent = 100;
	constexpr int AirWingLessonMaxSteps = 2;
	constexpr int AirWingLessonFadeFrames = 5400;
	constexpr int AirPadGuardCells = 12;

	// Wings of attrition (user, 2026-11-25): a building at no known anti-air risk of an enemy who has few
	// buildings left is struck pass after pass when the ready aircraft bring it down in
	// AirWingAttritionPasses passes or fewer; its standing after the pass is no lesson. Two aircraft that
	// delivered 314 of the 1500 of the last naval yard of an enemy waited on their pads for 32000 frames
	// (match #18296).
	constexpr int AirWingAttritionPasses = 6;
	constexpr int AirWingAttritionBuildingsMost = 8;

	// How long a wing keeps flying the same strike.
	constexpr int AirStrikeOrderFrames = 4500;
	// The ready aircraft that could destroy a target in one pass wait this long for the rest of the
	// fleet still rearming or flying home.
	constexpr int AirWingGatherFrames = 900;
	// A strike aircraft below this strength stays out of the wings (and on its pad).
	constexpr double AirWingMinHealth = 0.5;
	// A lone strike is what an aircraft was doing when it is lost within this long of the order.
	constexpr int AirSoloStrikeLossFrames = 1800;
	// A lone strike needs this much of the aircraft's strength, like a wing's.
	constexpr double AirSoloStrikeMinHealth = 0.5;

	NeuralAirForceState& GetAirForceState(const HouseClass* pHouse)
	{
		auto& state = AirForceStates[std::clamp(pHouse ? pHouse->ArrayIndex : 0, 0, 31)];

		if (state.MatchSerial != NeuralMatchSerial)
			state = NeuralAirForceState { NeuralMatchSerial };

		return state;
	}

	// An island war for the air force (AirWing*): the starting bases are linked by no land, or the
	// production judged the enemy across the water.
	bool IsAirIslandWar(HouseClass* pHouse)
	{
		return (MapInfo.Computed && !MapInfo.LandLinked) || GetMacroState(pHouse).EnemyAcrossWater;
	}

	// The steps of the wing lesson now: one fades every AirWingLessonFadeFrames since the last failure.
	int AirWingLessonStepsNow(const NeuralAirForceState& state)
	{
		const int now = Unsorted::CurrentFrame;

		if (state.WingLessonSteps <= 0 || state.WingLessonFrame < 0 || now < state.WingLessonFrame)
			return std::max(0, state.WingLessonSteps);

		return std::max(0, state.WingLessonSteps - (now - state.WingLessonFrame) / AirWingLessonFadeFrames);
	}

	// Cells from a cell to the nearest helipad of the house (-1: none).
	int CellsFromNearestHelipad(HouseClass* pHouse, const CellStruct& cell)
	{
		int cells = -1;

		for (auto const pBld : pHouse->Buildings)
		{
			if (pBld && pBld->IsAlive && !pBld->InLimbo && pBld->Type && pBld->Type->Helipad)
			{
				const int distance = static_cast<int>(pBld->GetMapCoords().DistanceFrom(cell));
				cells = cells < 0 ? distance : std::min(cells, distance);
			}
		}

		return cells;
	}

	// An armed aircraft of the air force proper: no spawned missiles or fighters of a carrier, no
	// carryalls or passenger transports.
	bool IsAirForceCraft(const AircraftTypeClass* pType)
	{
		return pType && !pType->Spawned && !pType->Carryall && pType->Passengers <= 0;
	}

	bool AircraftHitsGround(AircraftTypeClass* pType)
	{
		return GroundRangeOf(pType) > 0;
	}

	bool AircraftHitsAir(AircraftTypeClass* pType)
	{
		return GetCombatValueOfType(pType, true) > 0;
	}

	// TEMP diagnostic: notes the task the AI gives an aircraft (the frame only when it changes).
	void NoteAirTask(HouseClass* pHouse, AircraftClass* pAir, NeuralAirTask task)
	{
		auto& record = GetAirForceState(pHouse).Tasks[pAir->UniqueID];

		if (record.Task != task)
			record = NeuralAirTaskRecord { task, Unsorted::CurrentFrame };
	}

	// A strike aircraft of the wings: an air force aircraft whose ground weapon hurts (more than 10%)
	// the armor of at least half of the building types worth an air strike. A gas bomber that does
	// nothing to buildings is no wing aircraft: it hunts units. From the rules only, kept per type.
	bool IsWingStrikeType(AircraftTypeClass* pType)
	{
		if (!pType || !IsAirForceCraft(pType))
			return false;

		static int cacheSerial = -1;
		static std::vector<signed char> cache;

		if (cacheSerial != NeuralMatchSerial)
		{
			cacheSerial = NeuralMatchSerial;
			cache.assign(static_cast<size_t>(AircraftTypeClass::Array.Count), -1);
		}

		const size_t index = static_cast<size_t>(pType->ArrayIndex);

		if (index >= cache.size())
			cache.resize(index + 1, -1);

		if (cache[index] >= 0)
			return cache[index] > 0;

		int worthTypes = 0;
		int hurtTypes = 0;

		for (auto const pBuildingType : BuildingTypeClass::Array)
		{
			if (!pBuildingType || pBuildingType->Strength <= 0 || AirStrikeWorthOf(pBuildingType) <= 0)
				continue;

			worthTypes++;

			for (int i = 0; i < 2; ++i)
			{
				auto const pWS = pType->GetWeapon(i);
				auto const pWeapon = pWS ? pWS->WeaponType : nullptr;

				if (WeaponHitsGround(pWeapon) && pWeapon->Warhead && CanWarheadDamageEnemy(pWeapon->Warhead)
					&& GetEffectiveVersusAgainstTypePermille(pWeapon->Warhead, pBuildingType) > 100)
				{
					hurtTypes++;
					break;
				}
			}
		}

		const bool wing = worthTypes > 0 && hurtTypes * 2 >= worthTypes;
		cache[index] = wing ? 1 : 0;

		return wing;
	}

	// Free for a wing: docked or guarding, on a patrol stop with nothing to shoot, or hovering at the
	// end of a move.
	bool IsAircraftIdleForOrders(AircraftClass* pAir)
	{
		const Mission mission = pAir->CurrentMission;

		return mission == Mission::Guard || mission == Mission::Sleep || (mission == Mission::Area_Guard && pAir->Target == nullptr)
			|| (mission == Mission::Move && pAir->Target == nullptr
				&& (!pAir->Destination || pAir->DistanceFrom(pAir->Destination) <= 384));
	}

	bool HasFullAirLoad(AircraftClass* pAir)
	{
		return HasEndlessAirLoad(pAir->Type) ? pAir->Ammo != 0 : pAir->Ammo >= pAir->Type->Ammo;
	}

	// Worth of a known enemy building as an air strike target, from its rules only.
	int AirStrikeWorthOf(const BuildingTypeClass* pType)
	{
		if (!pType || pType->Wall || pType->Insignificant || pType->InvisibleInGame)
			return 0;

		if (pType->HasSuperWeapon())
			return 400;

		if (pType->ConstructionYard)
			return 360;

		if (pType->PowerBonus > 0)
			return 300;

		if (pType->Refinery || pType->ResourceDestination)
			return 280;

		if (pType->Factory != AbstractType::None)
			return 250;

		if (pType->Radar)
			return 180;

		if (pType->Cost >= 1500)
			return 160;

		return pType->IsBaseDefense ? 135 : 95;
	}

	// Worth of a visible enemy ground or surface unit for a strike wing: prioritizes cutting
	// down enemy firepower (artillery, main battle tanks, mind controllers, heavy warships),
	// exposed MCVs/harvesters, and remnant units when an enemy has no buildings left.
	int AirUnitStrikeWorth(HouseClass* pHouse, TechnoClass* pTech, const CellStruct& front, bool noEnemyBuildingsKnown)
	{
		if (!pTech || !pTech->IsAlive || pTech->InLimbo || pTech->Health <= 0 || !pTech->Owner
			|| pTech->Owner == pHouse || pHouse->IsAlliedWith(pTech->Owner) || pTech->Owner->IsNeutral()
			|| pTech->Owner->Defeated || pTech->Disguised || pTech->IsInAir())
		{
			return 0;
		}

		const AbstractType kind = pTech->WhatAmI();

		if (kind != AbstractType::Unit && kind != AbstractType::Infantry)
			return 0;

		auto const pCell = pTech->GetCell();

		if (!pCell || !IsCellVisibleToAI(pHouse, pCell))
			return 0;

		if (pTech->CloakState != CloakState::Uncloaked
			&& !pCell->Sensors_InclHouse(static_cast<unsigned int>(pHouse->ArrayIndex)))
		{
			return 0;
		}

		auto const pType = pTech->GetTechnoType();

		if (!pType || pType->Immune || (pType->Naval && pType->Underwater))
			return 0;

		const int groundRangeCells = GroundRangeOf(pType) / Unsorted::LeptonsPerCell;
		const bool shootsUp = GetCombatValueOfType(pType, true) > 0;

		// Dedicated anti-air vehicles are avoided like AA towers unless mopping up remnants.
		if (shootsUp && groundRangeCells <= 0 && !noEnemyBuildingsKnown)
			return 0;

		bool hasMindOrTemporal = false;
		bool hasSpawner = pType->Spawns != nullptr;

		for (int i = 0; i < 2; ++i)
		{
			auto const pWS = pType->GetWeapon(i);
			auto const pW = pWS ? pWS->WeaponType : nullptr;

			if (!pW)
				continue;

			if (pW->Spawner)
				hasSpawner = true;

			if (pW->Warhead && (pW->Warhead->MindControl || pW->Warhead->Temporal))
				hasMindOrTemporal = true;
		}

		const int firepower = VehicleFirepowerScore(pType);
		int worth = 0;

		if (kind == AbstractType::Unit)
		{
			auto const pUnitType = static_cast<const UnitTypeClass*>(pType);

			if (pUnitType->DeploysInto && pUnitType->DeploysInto->ConstructionYard)
				worth = 280;
			else if (hasMindOrTemporal || hasSpawner || groundRangeCells >= 8)
				worth = 225;
			else if (IsMainBattleTankType(pType) || (pType->Cost >= 900 && firepower > 0))
				worth = 165;
			else if (pUnitType->Harvester || pUnitType->ResourceGatherer)
				worth = 160;
			else if (firepower > 0)
				worth = 100;
		}
		else
		{
			auto const pInfType = static_cast<const InfantryTypeClass*>(pType);

			if (hasMindOrTemporal || IsCommandoInfantryType(pInfType) || pType->Cost >= 800)
				worth = 175;
		}

		if (worth > 0)
		{
			worth += std::min(150, firepower / 3);

			if (pTech->Veterancy.IsElite())
				worth += 110;
			else if (pTech->Veterancy.IsVeteran())
				worth += 55;

			if (front != CellStruct::Empty && pTech->GetMapCoords().DistanceFrom(front) <= 16.0)
				worth += 140;
		}

		if (noEnemyBuildingsKnown || pTech->Owner->OwnedBuildings == 0)
			worth = std::max(worth, 260);

		return worth;
	}

	// Known enemy anti-air defenses that cover a cell: within their anti-air range plus 2 cells, 9 at
	// least (a gattling cannon reaches aircraft 12 cells away).
	int CountKnownAntiAirCovering(HouseClass* pHouse, const CellStruct& cell)
	{
		int count = 0;

		for (auto const& info : GetKnownEnemyBuildingCache(pHouse).Entries)
		{
			if (info.IsAA && info.Coords.DistanceFrom(cell) <= std::max(9, AntiAirRangeCells(info.Building->Type) + 2))
				count++;
		}

		return count;
	}

	// Anti-air risk for aircraft over a cell: known enemy anti-air defenses that cover it count 2
	// each, enemy anti-air units we know of within 8 cells 1 each, and the spots where our
	// aircraft were shot down in the last 5400 frames within 6 cells 1 each (2 at most).
	int AirStrikeRiskAt(HouseClass* pHouse, const CellStruct& cell)
	{
		int risk = CountKnownAntiAirCovering(pHouse, cell) * 2;

		ForEachTechnoNear(cell, 8, [&](TechnoClass* pTech) -> bool
		{
			if (!pTech->IsAlive || pTech->InLimbo || !pTech->Owner || pTech->Owner == pHouse || pHouse->IsAlliedWith(pTech->Owner)
				|| pTech->Owner->IsNeutral() || pTech->WhatAmI() == AbstractType::Building
				|| !IsCellKnownToAI(pHouse, pTech->GetMapCoords()))
			{
				return true;
			}

			if (GetCombatValueOfType(pTech->GetTechnoType(), true) > 0)
				risk++;

			return risk < 12;
		});

		int danger = 0;

		for (auto const& [spot, frame] : GetMacroState(pHouse).AirDangerSpots)
		{
			if (spot.DistanceFrom(cell) <= 6.0 && Unsorted::CurrentFrame - frame <= 5400)
				danger++;
		}

		return risk + std::min(2, danger);
	}

	// Damage one sortie of this aircraft does to a ground or surface target (ground weapon,
	// burst, full load, against the target's armor).
	int AirSortieDamageVs(AircraftClass* pAir, TechnoClass* pTarget);
	int AirSortieDamageVs(AircraftClass* pAir, BuildingClass* pTarget)
	{
		return AirSortieDamageVs(pAir, static_cast<TechnoClass*>(pTarget));
	}

	int AirSortieDamageVs(AircraftClass* pAir, TechnoClass* pTarget)
	{
		auto const pTargetType = pTarget ? pTarget->GetTechnoType() : nullptr;

		if (!pTargetType || pTargetType->Immune)
			return 0;

		int best = 0;

		for (int i = 0; i < 2; ++i)
		{
			auto const pWS = pAir->Type->GetWeapon(i);
			auto const pWeapon = pWS ? pWS->WeaponType : nullptr;

			if (!WeaponHitsGround(pWeapon) || !pWeapon->Warhead || !CanWarheadDamageEnemy(pWeapon->Warhead))
				continue;

			if (auto const pTargetExt = TechnoExt::TryFetch(pTarget))
			{
				auto const pShield = pTargetExt->Shield.get();

				if (pShield && pShield->IsActive() && !pShield->CanBeTargeted(const_cast<WeaponTypeClass*>(pWeapon)))
					continue;
			}

			const double verses = GeneralUtils::GetWarheadVersusArmor(pWeapon->Warhead, pTarget, pTargetType);
			best = std::max(best, static_cast<int>(pWeapon->Damage * std::max(1, pWeapon->Burst) * verses));
		}

		return best * AirSortieShots(pAir->Type);
	}

	// Score of a known enemy building as a strike target from a cell: worth (more near our fighting
	// army, where defenses become worth hitting too) lowered by risk and distance, more for a
	// damaged one and one of the house we fight. 0 when it is no target: an anti-air site, nothing
	// worth a strike, or more risk than the tolerance.
	int AirStrikeTargetScore(HouseClass* pHouse, const KnownEnemyBuildingInfo& info, const CellStruct& from, const CellStruct& front,
		const std::array<int, 8>& airValues, int tolerance, int& risk)
	{
		auto const pType = info.Building->Type;

		if (info.IsAA || pType->Immune)
			return 0;

		int worth = AirStrikeWorthOf(pType) + AirBaseTargetBonus(airValues, info.Building, AirBaseStrikePercent, AirBaseStrikeMost);

		if (front != CellStruct::Empty && info.Coords.DistanceFrom(front) <= 14.0)
			worth = info.IsDefense ? std::max(worth, 220) : (worth > 0 ? worth + 120 : 0);

		if (worth <= 0)
			return 0;

		risk = AirStrikeRiskAt(pHouse, info.Coords);

		if (risk > tolerance)
			return 0;

		int score = worth * 100 / (100 + risk * 60) - static_cast<int>(from.DistanceFrom(info.Coords)) / 2;

		if (info.Building->Health * 2 < pType->Strength)
			score += score / 4;

		if (info.Building->Owner->ArrayIndex == pHouse->EnemyHouseIndex)
			score += 30;

		return std::max(0, score);
	}

	// Best known enemy building for a strike of tolerance + 1 aircraft from a cell (known anti-air
	// sites themselves are never picked; ties keep the first in building array order). Targets the
	// sorties already sent at them cover (pCommitted: target UniqueID -> damage, with a 20% margin)
	// are left out; with pStriker, only buildings it can damage count (some bombs do nothing
	// against structures).
	BuildingClass* PickAirStrikeTarget(HouseClass* pHouse, const CellStruct& from, int tolerance, const CellStruct& front,
		int* pOutScore = nullptr, int* pOutRisk = nullptr, const std::unordered_map<DWORD, int>* pCommitted = nullptr,
		AircraftClass* pStriker = nullptr)
	{
		BuildingClass* pBest = nullptr;
		int bestScore = 0;
		int bestRisk = 0;
		const std::array<int, 8> airValues = EnemyAirValueByOwner(pHouse);

		for (auto const& info : GetKnownEnemyBuildingCache(pHouse).Entries)
		{
			if (pCommitted)
			{
				auto const it = pCommitted->find(info.Building->UniqueID);

				if (it != pCommitted->end() && it->second >= info.Building->Health + info.Building->Health / 5)
					continue;
			}

			int risk = 0;
			const int score = AirStrikeTargetScore(pHouse, info, from, front, airValues, tolerance, risk);

			if (score <= bestScore || (pStriker && AirSortieDamageVs(pStriker, info.Building) <= 0))
				continue;

			bestScore = score;
			bestRisk = risk;
			pBest = info.Building;
		}

		if (pOutScore)
			*pOutScore = bestScore;

		if (pOutRisk)
			*pOutRisk = bestRisk;

		return pBest;
	}

	struct NeuralAirWingPick
	{
		TechnoClass* Target { nullptr };
		int Score { 0 };
		int Risk { 0 };
		// Sortie damage of the aircraft expected to reach the target, and the health left to destroy.
		int Damage { 0 };
		int Need { 0 };
		// Brought down in several passes (AirWingAttrition*).
		bool Attrition { false };
		// TEMP diagnostic: the best target the ready aircraft are still too few for.
		TechnoClass* ShortTarget { nullptr };
		int ShortDamage { 0 };
		int ShortNeed { 0 };
		int ShortRisk { 0 };
	};

	// Best known enemy structure or high-firepower ground/surface unit the ready aircraft can
	// destroy in one pass, cutting down enemy firepower as well as bases.
	NeuralAirWingPick PickAirWingTarget(HouseClass* pHouse, const std::vector<AircraftClass*>& ready, const CellStruct& front,
		const std::unordered_map<DWORD, int>& committed, int lessonPercent)
	{
		NeuralAirWingPick pick;

		if (ready.empty())
			return pick;

		const CellStruct from = ready.front()->GetMapCoords();
		const int tolerance = static_cast<int>(ready.size()) - 1;
		const std::array<int, 8> airValues = EnemyAirValueByOwner(pHouse);
		auto const& knownBlds = GetKnownEnemyBuildingCache(pHouse).Entries;
		const bool noEnemyBuildingsKnown = knownBlds.empty();
		int shortScore = 0;

		// The known buildings of each enemy: one with few left is worn down pass after pass.
		int knownBuildingsOf[32] {};

		for (auto const& info : knownBlds)
		{
			if (const int owner = info.Building->Owner->ArrayIndex; owner >= 0 && owner < 32)
				knownBuildingsOf[owner]++;
		}

		auto evaluateCandidate = [&](TechnoClass* pCand, int score, int risk, int extraMarginPct, bool mayWearDown)
		{
			if (score <= 0 || (score <= pick.Score && score <= shortScore))
				return;

			auto const it = committed.find(pCand->UniqueID);
			const int need = pCand->Health - (it != committed.end() ? it->second : 0);

			if (need <= 0)
				return;

			int damage = 0;
			int strikers = 0;

			for (auto const pAir : ready)
			{
				const int sortie = AirSortieDamageVs(pAir, pCand);

				if (sortie > 0)
				{
					damage += sortie;
					strikers++;
				}
			}

			if (strikers <= risk)
				return;

			const int delivered = damage / strikers * (strikers - risk);
			const bool onePass = delivered * 100 >= need * (100 + extraMarginPct + lessonPercent);
			const int candOwner = pCand->Owner ? pCand->Owner->ArrayIndex : -1;
			const bool wearsDown = !onePass && mayWearDown && risk == 0 && candOwner >= 0 && candOwner < 32
				&& knownBuildingsOf[candOwner] <= AirWingAttritionBuildingsMost
				&& delivered * AirWingAttritionPasses * 100 >= need * (100 + extraMarginPct);

			if (onePass || wearsDown)
			{
				if (score > pick.Score)
				{
					pick.Target = pCand;
					pick.Score = score;
					pick.Risk = risk;
					pick.Damage = delivered;
					pick.Need = need;
					pick.Attrition = wearsDown;
				}
			}
			else if (score > shortScore)
			{
				shortScore = score;
				pick.ShortTarget = pCand;
				pick.ShortDamage = delivered;
				pick.ShortNeed = need;
				pick.ShortRisk = risk;
			}
		};

		for (auto const& info : knownBlds)
		{
			int risk = 0;
			const int score = AirStrikeTargetScore(pHouse, info, from, front, airValues, tolerance, risk);
			evaluateCandidate(info.Building, score, risk, 15, true);
		}

		auto considerEnemyUnit = [&](TechnoClass* pTech)
		{
			const int worth = AirUnitStrikeWorth(pHouse, pTech, front, noEnemyBuildingsKnown);

			if (worth <= 0)
				return;

			const CellStruct coords = pTech->GetMapCoords();
			const int risk = AirStrikeRiskAt(pHouse, coords);

			if (risk > tolerance)
				return;

			int score = worth * 100 / (100 + risk * 65) - static_cast<int>(from.DistanceFrom(coords)) / 2;
			auto const pType = pTech->GetTechnoType();

			if (pType && pTech->Health * 2 < pType->Strength)
				score += score / 4;

			if (pTech->Owner->ArrayIndex == pHouse->EnemyHouseIndex)
				score += 30;

			evaluateCandidate(pTech, std::max(0, score), risk, 25, false);
		};

		for (auto const pUnit : UnitClass::Array)
			considerEnemyUnit(pUnit);

		for (auto const pInf : InfantryClass::Array)
			considerEnemyUnit(pInf);

		return pick;
	}

	// Centroid of our armed ground units that are fighting an enemy we know of; needs 3 of them.
	CellStruct FindAirSupportFront(HouseClass* pHouse)
	{
		int sumX = 0;
		int sumY = 0;
		int count = 0;

		auto consider = [&](FootClass* pFoot)
		{
			if (!pFoot || pFoot->Owner != pHouse || !pFoot->IsAlive || pFoot->InLimbo || !pFoot->IsArmed())
				return;

			auto const pTarget = abstract_cast<TechnoClass*>(pFoot->Target);

			if (!pTarget || !pTarget->Owner || pTarget->Owner == pHouse || pHouse->IsAlliedWith(pTarget->Owner)
				|| pTarget->Owner->IsNeutral() || !IsCellKnownToAI(pHouse, pTarget->GetMapCoords()))
			{
				return;
			}

			const CellStruct c = pFoot->GetMapCoords();
			sumX += c.X;
			sumY += c.Y;
			count++;
		};

		for (auto const pUnit : UnitClass::Array)
		{
			if (pUnit && pUnit->Type && !pUnit->Type->Harvester && !IsAirborneThreatTechno(pUnit))
				consider(pUnit);
		}

		for (auto const pInf : InfantryClass::Array)
			consider(pInf);

		if (count < 3)
			return CellStruct::Empty;

		return CellStruct { static_cast<short>(sumX / count), static_cast<short>(sumY / count) };
	}

	// Refreshes the air force summary every 150 frames: fleet, what it could build, strike work at
	// the fleet's risk tolerance, enemy air seen, the front, and the fielded / lost aircraft counters.
	NeuralAirForceState& RefreshAirForceSummary(HouseClass* pHouse)
	{
		auto& state = GetAirForceState(pHouse);
		const int now = Unsorted::CurrentFrame;

		if (state.SummaryFrame >= 0 && now >= state.SummaryFrame && now - state.SummaryFrame < 150)
			return state;

		state.SummaryFrame = now;
		state.StrikeFleetValue = 0;
		state.InterceptorValue = 0;
		state.StrikeAircraft = 0;
		state.StrikeWork = 0;
		state.InterceptWork = 0;
		state.CanProduceStriker = false;
		state.CanProduceInterceptor = false;
		state.CanBuildAirfield = false;
		state.FleetCount = 0;

		if (now - state.LossDecayFrame >= 900 || now < state.LossDecayFrame)
		{
			state.LossDecayFrame = now;
			state.RecentLosses = state.RecentLosses * 7 / 8;
		}

		std::unordered_set<DWORD> alive;
		CellStruct fleetCell = CellStruct::Empty;

		for (auto const pAir : AircraftClass::Array)
		{
			if (!pAir || pAir->Owner != pHouse || !pAir->IsAlive || !IsAirForceCraft(pAir->Type) || !pAir->IsArmed())
				continue;

			alive.insert(pAir->UniqueID);

			// Aircraft still in the factory are in limbo: counted once they are out.
			if (pAir->InLimbo)
				continue;

			state.FleetCount++;

			if (state.Fielded.emplace(pAir->UniqueID, pAir->Type->ArrayIndex).second)
			{
				CountDecision(NeuralDecision::AircraftFielded, pHouse->ArrayIndex);
				Debug::Log("[NeuralAI] House %d AirForce: fielded %s %u, frame %d.\n", pHouse->ArrayIndex, pAir->Type->ID,
					pAir->UniqueID, now);
			}

			if (AircraftHitsGround(pAir->Type))
			{
				state.StrikeFleetValue += GetCombatValueOfType(pAir->Type, false);
				state.StrikeAircraft++;

				if (fleetCell == CellStruct::Empty)
					fleetCell = pAir->GetMapCoords();
			}

			if (AircraftHitsAir(pAir->Type))
				state.InterceptorValue += GetCombatValueOfType(pAir->Type, true);
		}

		for (auto it = state.Fielded.begin(); it != state.Fielded.end();)
		{
			if (alive.contains(it->first))
			{
				++it;
				continue;
			}

			CountDecision(NeuralDecision::AircraftLost, pHouse->ArrayIndex);
			state.RecentLosses += 100;

			// What it was striking when it was lost: the target is marked dangerous as well as the cell
			// where the aircraft fell, which is often farther from the target than the reach the danger
			// memory is read with (the next aircraft would go for the same building).
			auto const itWing = state.Orders.find(it->first);
			auto const itSolo = state.SoloStrikes.find(it->first);
			const NeuralAirStrikeOrder* pOrder = nullptr;
			const char* kind = "no";

			if (itWing != state.Orders.end())
			{
				pOrder = &itWing->second;
				kind = "a wing";
			}
			else if (itSolo != state.SoloStrikes.end() && now >= itSolo->second.Frame && now - itSolo->second.Frame <= AirSoloStrikeLossFrames)
			{
				pOrder = &itSolo->second;
				kind = "a lone";
			}

			if (pOrder && pOrder->TargetCell != CellStruct::Empty)
				GetMacroState(pHouse).AirDangerSpots.emplace_back(pOrder->TargetCell, now);

			auto const pLostType = AircraftTypeClass::Array.GetItemOrDefault(it->second);
			Debug::Log("[NeuralAI] House %d AirForce: lost %s %u on %s strike, target at (%d,%d), frame %d.\n", pHouse->ArrayIndex,
				pLostType ? pLostType->ID : "?", it->first, kind, pOrder ? pOrder->TargetCell.X : -1, pOrder ? pOrder->TargetCell.Y : -1, now);

			state.Orders.erase(it->first);
			state.SoloStrikes.erase(it->first);
			state.Tasks.erase(it->first);
			it = state.Fielded.erase(it);
		}

		if (state.SoloStrikes.size() > 256)
		{
			std::erase_if(state.SoloStrikes, [now](const auto& entry)
				{ return now < entry.second.Frame || now - entry.second.Frame > AirSoloStrikeLossFrames; });
		}

		if (pHouse->NumAirpads > 0)
		{
			for (auto const pType : AircraftTypeClass::Array)
			{
				if (!IsAirForceCraft(pType) || !CanBuildUnderHumanRules(pHouse, pType))
					continue;

				state.CanProduceStriker = state.CanProduceStriker || AircraftHitsGround(pType);
				state.CanProduceInterceptor = state.CanProduceInterceptor || AircraftHitsAir(pType);
			}
		}
		else if (HouseCanEverFieldAircraft(pHouse))
		{
			for (auto const pType : BuildingTypeClass::Array)
			{
				if (pType && pType->Factory == AbstractType::AircraftType && CanBuildUnderHumanRules(pHouse, pType))
				{
					state.CanBuildAirfield = true;
					break;
				}
			}
		}

		state.FrontCell = FindAirSupportFront(pHouse);

		// Strike work: the three best targets for the fleet we have (one aircraft more of
		// tolerance per aircraft, three at most); a house still building its air force plans for one.
		if (state.StrikeAircraft > 0 || state.CanProduceStriker || state.CanBuildAirfield)
		{
			const CellStruct home = pHouse->ConYards.Count > 0 && pHouse->ConYards.GetItem(0)
				? pHouse->ConYards.GetItem(0)->GetMapCoords()
				: pHouse->BaseSpawnCell;
			const CellStruct from = fleetCell != CellStruct::Empty ? fleetCell : home;
			const int tolerance = std::clamp(state.StrikeAircraft - 1, 0, 3);
			std::array<int, 3> best {};
			const std::array<int, 8> airValues = EnemyAirValueByOwner(pHouse);

			for (auto const& info : GetKnownEnemyBuildingCache(pHouse).Entries)
			{
				if (info.IsAA)
					continue;

				int worth = AirStrikeWorthOf(info.Building->Type)
					+ AirBaseTargetBonus(airValues, info.Building, AirBaseStrikePercent, AirBaseStrikeMost);

				if (state.FrontCell != CellStruct::Empty && info.Coords.DistanceFrom(state.FrontCell) <= 14.0)
					worth = info.IsDefense ? std::max(worth, 220) : (worth > 0 ? worth + 120 : 0);

				if (worth <= 0)
					continue;

				const int risk = AirStrikeRiskAt(pHouse, info.Coords);

				if (risk > tolerance)
					continue;

				const int score = std::max(0, worth * 100 / (100 + risk * 60) - static_cast<int>(from.DistanceFrom(info.Coords)) / 2);

				if (score > best[2])
				{
					best[2] = score;
					std::sort(best.begin(), best.end(), [](int a, int b) { return a > b; });
				}
			}

			const bool noEnemyBlds = GetKnownEnemyBuildingCache(pHouse).Entries.empty();
			auto addUnitWork = [&](TechnoClass* pTech)
			{
				const int worth = AirUnitStrikeWorth(pHouse, pTech, state.FrontCell, noEnemyBlds);

				if (worth <= 0)
					return;

				const CellStruct coords = pTech->GetMapCoords();
				const int risk = AirStrikeRiskAt(pHouse, coords);

				if (risk > tolerance)
					return;

				const int score = std::max(0, worth * 100 / (100 + risk * 65) - static_cast<int>(from.DistanceFrom(coords)) / 2);

				if (score > best[2])
				{
					best[2] = score;
					std::sort(best.begin(), best.end(), [](int a, int b) { return a > b; });
				}
			};

			for (auto const pUnit : UnitClass::Array)
				addUnitWork(pUnit);

			for (auto const pInf : InfantryClass::Array)
				addUnitWork(pInf);

			state.StrikeWork = best[0] + best[1] + best[2];
		}

		// Enemy air seen (remembered tracks, decayed): full weight while our anti-air does not
		// cover it, half otherwise.
		auto const& macro = GetMacroState(pHouse);

		if (macro.AirThreatValue > 0)
		{
			state.InterceptWork = macro.AirThreatValue > macro.OwnAACapacity
				? macro.AirThreatValue
				: macro.AirThreatValue / 2;
		}

		return state;
	}

	// How much of its air force a house keeps (percent, 25..100): fleet against the aircraft lost
	// lately. An air force that is being shot down as fast as it is built is not worth more of it.
	int AirForceSurvivalPercent(const NeuralAirForceState& state)
	{
		const int fleet = (state.FleetCount + 1) * 100;

		return std::clamp(fleet * 100 / (fleet + state.RecentLosses), 25, 100);
	}

	// Selector prior of Expert 8: the air force we own and the work it has (strike targets the
	// fleet can reach past known anti-air, enemy air an interceptor can answer). Work counts in
	// full with a fleet, less while it still has to be built, and less while our aircraft are
	// being shot down. 0 = no air work at all (veto).
	int AirForceOpportunity(HouseClass* pHouse)
	{
		auto const& state = RefreshAirForceSummary(pHouse);
		const bool canStrike = state.StrikeFleetValue > 0 || state.CanProduceStriker || state.CanBuildAirfield;
		const bool canIntercept = state.InterceptorValue > 0 || state.CanProduceInterceptor;
		const int strikePart = canStrike ? std::min(320, state.StrikeWork / 3) : 0;
		const int interceptPart = canIntercept ? std::min(280, state.InterceptWork) : 0;
		const int fleet = state.StrikeFleetValue + state.InterceptorValue;

		if (fleet == 0 && strikePart == 0 && interceptPart == 0)
			return 0;

		const int fleetPart = std::min(240, fleet / 2);
		const int readiness = fleet > 0
			? std::clamp(50 + fleet / 4, 50, 100)
			: (state.CanBuildAirfield ? 30 : 40);

		const int prior = std::min(760, fleetPart + (strikePart + interceptPart) * readiness / 100);

		return std::max(1, prior * AirForceSurvivalPercent(state) / 100);
	}

	// Next leg of a wing strike: the safe lane hop, then the attack itself. False once the aircraft
	// has fired (it flies home and joins the next wing), and false with the order dropped when the
	// target is gone or no longer known, the order is too old, or the target is now covered by more
	// anti-air than the aircraft flying for it can take.
	bool ContinueAirStrike(HouseClass* pHouse, AircraftClass* pAir)
	{
		auto& state = GetAirForceState(pHouse);
		auto const it = state.Orders.find(pAir->UniqueID);

		if (it == state.Orders.end() || it->second.Returning)
			return false;

		const DWORD targetID = it->second.TargetID;
		const int age = Unsorted::CurrentFrame - it->second.Frame;
		auto const pTarget = FindAirStrikeTechnoByID(targetID);
		const bool isBldTarget = pTarget && pTarget->WhatAmI() == AbstractType::Building;
		auto const pTargetCell = pTarget ? pTarget->GetCell() : nullptr;
		const bool targetKnownAndVisible = pTarget && pTargetCell
			&& (isBldTarget
				? IsEnemyBuildingKnown(pHouse, static_cast<BuildingClass*>(pTarget))
				: (!pTarget->IsInAir() && IsCellVisibleToAI(pHouse, pTargetCell)
					&& (pTarget->CloakState == CloakState::Uncloaked
						|| pTargetCell->Sensors_InclHouse(static_cast<unsigned int>(pHouse->ArrayIndex)))));

		if (!pTarget || !pTarget->Owner || pTarget->Owner == pHouse || pHouse->IsAlliedWith(pTarget->Owner)
			|| pTarget->Owner->IsNeutral() || !targetKnownAndVisible || age < 0 || age > AirStrikeOrderFrames)
		{
			AirRoutes.erase(pAir->UniqueID);
			state.Orders.erase(it);
			return false;
		}

		it->second.TargetCell = pTarget->GetMapCoords();

		int wing = 0;

		for (auto const& [uid, order] : state.Orders)
		{
			if (order.TargetID == targetID)
				wing++;
		}

		if (AirStrikeRiskAt(pHouse, pTarget->GetMapCoords()) > wing - 1)
		{
			AirRoutes.erase(pAir->UniqueID);
			state.Orders.erase(it);
			return false;
		}

		auto const pHop = GetSafeAirHop(pHouse, pAir, pTargetCell);

		if (!pHop || pHop == pTargetCell)
			OrderAircraftAttack(pAir, pTarget);
		else
			IssueMoveOrder(pAir, pHop, 3.0);

		return true;
	}

	bool ContinueSoloAirStrike(HouseClass* pHouse, AircraftClass* pAir)
	{
		auto& state = GetAirForceState(pHouse);
		auto const it = state.SoloStrikes.find(pAir->UniqueID);

		if (it == state.SoloStrikes.end())
			return false;

		if (!HasEndlessAirLoad(pAir->Type) && pAir->Ammo < pAir->Type->Ammo)
			it->second.Returning = true;

		if (it->second.Returning)
		{
			if (!pAir->IsInAir())
				state.SoloStrikes.erase(it);

			return false;
		}

		const int age = Unsorted::CurrentFrame - it->second.Frame;
		auto const pTarget = abstract_cast<BuildingClass*>(FindAirStrikeTechnoByID(it->second.TargetID));
		auto const pTargetCell = pTarget ? pTarget->GetCell() : nullptr;

		if (!pTarget || !pTargetCell || !pTarget->Owner || pTarget->Owner == pHouse || pHouse->IsAlliedWith(pTarget->Owner)
			|| pTarget->Owner->IsNeutral() || !IsEnemyBuildingKnown(pHouse, pTarget)
			|| AirStrikeRiskAt(pHouse, pTarget->GetMapCoords()) > 0 || age < 0 || age > AirSoloStrikeLossFrames)
		{
			AirRoutes.erase(pAir->UniqueID);
			state.SoloStrikes.erase(it);
			return false;
		}

		it->second.TargetCell = pTarget->GetMapCoords();
		auto const pHop = GetSafeAirHop(pHouse, pAir, pTargetCell);

		if (!pHop || pHop == pTargetCell)
			OrderAircraftAttack(pAir, pTarget);
		else
			IssueMoveOrder(pAir, pHop, 3.0);

		return true;
	}

	// A lone aircraft's strike on a building, for the aircraft that are not of the wings (the strike
	// aircraft wait for theirs): flown along the safe lane like a wing's, and remembered so that a
	// loss on it marks the target.
	void FlySoloAirStrike(HouseClass* pHouse, AircraftClass* pAir, BuildingClass* pTarget)
	{
		GetAirForceState(pHouse).SoloStrikes[pAir->UniqueID]
			= NeuralAirStrikeOrder { pTarget->UniqueID, Unsorted::CurrentFrame, pTarget->GetMapCoords() };

		auto const pTargetCell = pTarget->GetCell();
		auto const pHop = GetSafeAirHop(pHouse, pAir, pTargetCell);

		if (!pHop || pHop == pTargetCell)
			OrderAircraftAttack(pAir, pTarget);
		else
			IssueMoveOrder(pAir, pHop, 3.0);
	}

	// Strike wings, under any expert in charge (user, 2026-11-19: four Black Eagles take out most
	// power plants, more take a factory in one pass; in match #11501 one house fielded 14 of them and
	// lost all 14 one by one). The strike aircraft (IsWingStrikeType) with at least AirWingMinHealth
	// wait on their pads. Once all the ready ones (full load, idle) together destroy a known target in
	// one pass (PickAirWingTarget), every one of them that can damage it flies there; with some of the
	// fleet still rearming or flying home they wait for those up to AirWingGatherFrames first. An
	// aircraft that has fired flies home and joins the next wing once it lands, so a target that
	// stands after the pass is struck again by the whole fleet, not by each aircraft as it rearms.
	void UpdateAirWings(HouseClass* pHouse)
	{
		auto& state = RefreshAirForceSummary(pHouse);
		const int now = Unsorted::CurrentFrame;
		std::vector<AircraftClass*> ready;
		int strikeFleet = 0;

		// Sortie damage the wings flying out send at each target (key access only), and the targets
		// already looked up.
		std::unordered_map<DWORD, int> committedByTarget;
		std::unordered_map<DWORD, TechnoClass*> targetsByID;
		// Our strike aircraft alive now, for the wing lesson. Lookup only.
		std::unordered_set<DWORD> strikeAircraftAlive;

		for (auto const pAir : AircraftClass::Array)
		{
			if (!pAir || pAir->Owner != pHouse || !pAir->IsAlive || pAir->InLimbo || !IsWingStrikeType(pAir->Type) || !pAir->IsArmed())
				continue;

			strikeAircraftAlive.insert(pAir->UniqueID);
			const bool member = !IsPlanMember(pHouse, pAir->UniqueID) && !IsInTransportOp(pHouse, pAir->UniqueID)
				&& pAir->GetHealthPercentage() >= AirWingMinHealth;

			if (auto const it = state.Orders.find(pAir->UniqueID); it != state.Orders.end())
			{
				auto& order = it->second;

				if (!order.Returning && !HasEndlessAirLoad(pAir->Type) && pAir->Ammo < pAir->Type->Ammo)
					order.Returning = true;

				// Landed after its pass (on its pad, rearming): the strike is over.
				if ((order.Returning && !pAir->IsInAir()) || now < order.Frame || now - order.Frame > 2 * AirStrikeOrderFrames)
				{
					state.Orders.erase(it);
				}
				else
				{
					if (order.Returning)
					{
						strikeFleet += member ? 1 : 0;
					}
					else
					{
						auto const known = targetsByID.find(order.TargetID);
						auto const pTarget = known != targetsByID.end() ? known->second : FindAirStrikeTechnoByID(order.TargetID);
						targetsByID[order.TargetID] = pTarget;

						if (pTarget)
							committedByTarget[pTarget->UniqueID] += AirSortieDamageVs(pAir, pTarget);
					}

					continue;
				}
			}

			if (!member)
				continue;

			strikeFleet++;

			if (HasFullAirLoad(pAir) && IsAircraftIdleForOrders(pAir))
				ready.push_back(pAir);
		}

		// The wing lesson (AirWingLesson*): a wing none of whose aircraft flies its strike any more is
		// judged. It failed when its target stands or it lost half of its aircraft or more.
		std::erase_if(state.Flights, [&state, &strikeAircraftAlive, now, pHouse](const NeuralAirWingFlight& flight)
			{
				const bool flying = now >= flight.Frame && now - flight.Frame <= 2 * AirStrikeOrderFrames
					&& std::any_of(flight.Members.begin(), flight.Members.end(),
						[&state, &strikeAircraftAlive](DWORD id) { return strikeAircraftAlive.contains(id) && state.Orders.contains(id); });

				if (flying)
					return false;

				auto const pTarget = FindAirStrikeTechnoByID(flight.TargetID);
				const bool stands = pTarget && pTarget->IsAlive && !pTarget->InLimbo;
				const int size = static_cast<int>(flight.Members.size());
				const int lost = static_cast<int>(std::count_if(flight.Members.begin(), flight.Members.end(),
					[&strikeAircraftAlive](DWORD id) { return !strikeAircraftAlive.contains(id); }));
				const bool failed = (flight.IsBuildingTarget && stands && !flight.Attrition) || lost * 2 >= size;
				const int stepsBefore = AirWingLessonStepsNow(state);

				if (failed)
				{
					state.WingLessonSteps = std::min(stepsBefore + 1, AirWingLessonMaxSteps);
					state.WingLessonFrame = now;
				}
				else
				{
					state.WingLessonSteps = 0;
				}

				Debug::Log("[NeuralAI] House %d AirWing verdict: %d aircraft at %s %s, %d lost, lesson steps %d -> %d, island war %d, frame %d.\n",
					pHouse->ArrayIndex, size, flight.TargetType ? flight.TargetType->ID : "?", stands ? "standing" : "taken", lost, stepsBefore,
					state.WingLessonSteps, IsAirIslandWar(pHouse) ? 1 : 0, now);
				return true;
			});

		if (ready.empty())
		{
			state.WingMassFrame = -1;
			return;
		}

		// In an island war a wing leaves with AirWingMinAircraft ready at least (the whole strike fleet
		// when it is smaller), and asks for the damage of the wing lesson.
		const bool islandWar = IsAirIslandWar(pHouse);
		const int wingMin = islandWar ? std::max(1, std::min(AirWingMinAircraft, strikeFleet)) : 1;
		const int lessonPercent = islandWar ? AirWingLessonStepsNow(state) * AirWingLessonPercent : 0;

		if (static_cast<int>(ready.size()) < wingMin)
		{
			state.WingMassFrame = -1;
			return;
		}

		// The strike aircraft spend most of their time ready on their pads: the target search (every
		// known building against every ready aircraft) runs every 45 frames at most.
		if (!IsAITickWindow(45, 1))
			return;

		std::sort(ready.begin(), ready.end(), [](AircraftClass* a, AircraftClass* b) { return a->UniqueID < b->UniqueID; });

		const int pending = strikeFleet - static_cast<int>(ready.size());
		bool waited = false;

		while (!ready.empty())
		{
			if (static_cast<int>(ready.size()) < wingMin)
				return;

			const NeuralAirWingPick pick = PickAirWingTarget(pHouse, ready, state.FrontCell, committedByTarget, lessonPercent);

			if (!pick.Target)
			{
				// TEMP diagnostic: why the ready aircraft wait on their pads, every 1800 frames at most.
				if (!waited && (state.WingWaitLogFrame < 0 || now < state.WingWaitLogFrame || now - state.WingWaitLogFrame >= 1800))
				{
					state.WingWaitLogFrame = now;
					auto const pShortType = pick.ShortTarget ? pick.ShortTarget->GetTechnoType() : nullptr;
					Debug::Log("[NeuralAI] House %d AirWing waits: %d ready of %d (%d pending), best target %s at risk %d needs %d, they deliver %d,"
						" frame %d.\n", pHouse->ArrayIndex, static_cast<int>(ready.size()), strikeFleet, pending,
						pShortType ? pShortType->ID : "none", pick.ShortRisk, pick.ShortNeed, pick.ShortDamage, now);
				}

				if (!waited)
					state.WingMassFrame = -1;

				return;
			}

			auto const pTarget = pick.Target;
			auto const pTargetType = pTarget->GetTechnoType();
			const bool isBuildingTarget = pTarget->WhatAmI() == AbstractType::Building;
			const CellStruct targetCell = pTarget->GetMapCoords();

			// Generous attrition & repair cushion so a wing never flies tight: assumes 1..3 aircraft
			// may fall en route on top of known target AA risk, and delivers 165% (+ lesson) of remaining HP
			// before splitting surplus ready aircraft into additional simultaneous wings.
			const int flightDistCells = static_cast<int>(ready.front()->GetMapCoords().DistanceFrom(targetCell));
			const int attritionReserve = 1 + (flightDistCells >= 40 ? 1 : 0) + (pick.Risk >= 2 ? 1 : 0);
			const int cushionedNeed = std::max(1, pick.Need * (165 + lessonPercent) / 100);
			const int desiredMinPlanes = std::max({ wingMin, isBuildingTarget ? 3 : 2, pick.Risk + attritionReserve + 1 });

			std::vector<AircraftClass*> candidates;
			candidates.reserve(ready.size());

			for (auto const pCandidate : ready)
			{
				if (AirSortieDamageVs(pCandidate, pTarget) > 0)
					candidates.push_back(pCandidate);
			}

			std::sort(candidates.begin(), candidates.end(), [&targetCell](AircraftClass* a, AircraftClass* b)
			{
				const int da = static_cast<int>(a->GetMapCoords().DistanceFrom(targetCell));
				const int db = static_cast<int>(b->GetMapCoords().DistanceFrom(targetCell));
				return da != db ? da < db : a->UniqueID < b->UniqueID;
			});

			std::vector<AircraftClass*> wing;
			int wingRawDamage = 0;

			for (auto const pCandidate : candidates)
			{
				wing.push_back(pCandidate);
				wingRawDamage += AirSortieDamageVs(pCandidate, pTarget);
				const int countNow = static_cast<int>(wing.size());

				if (countNow >= desiredMinPlanes)
				{
					const int expectedSurvivors = std::max(1, countNow - pick.Risk - attritionReserve);
					const int cushionedDelivered = wingRawDamage / countNow * expectedSurvivors;

					if (cushionedDelivered >= cushionedNeed)
						break;
				}
			}

			if (static_cast<int>(wing.size()) < wingMin)
				return;

			// Only hold for stragglers still rearming when this target consumes nearly our whole small fleet.
			const bool needsFullSmallFleet = static_cast<int>(ready.size()) < 12
				&& static_cast<int>(wing.size()) * 4 >= static_cast<int>(ready.size()) * 3;
			if (!waited && pending > 0 && needsFullSmallFleet)
			{
				if (state.WingMassFrame < 0 || now < state.WingMassFrame)
					state.WingMassFrame = now;

				if (now - state.WingMassFrame < AirWingGatherFrames)
					return;
			}

			waited = true;
			state.WingMassFrame = -1;

			const int wingCount = static_cast<int>(wing.size());
			const int wingDelivered = wingCount > pick.Risk ? (wingRawDamage / wingCount * (wingCount - pick.Risk)) : 0;
			state.WingsLaunched++;
			CountDecision(NeuralDecision::AirWing, pHouse->ArrayIndex);
			Debug::Log("[NeuralAI] House %d AirWing: %d %s -> %s of house %d at (%d,%d), risk %d, score %d, front %s, frame %d (wings %d);"
				" %d ready of %d (%d pending), they deliver %d of %d%s.\n", pHouse->ArrayIndex, wingCount, wing.front()->Type->ID,
				pTargetType ? pTargetType->ID : "?", pTarget->Owner->ArrayIndex, targetCell.X, targetCell.Y, pick.Risk, pick.Score,
				state.FrontCell != CellStruct::Empty ? "yes" : "no", now, state.WingsLaunched, static_cast<int>(ready.size()), strikeFleet, pending,
				wingDelivered, pick.Need, pick.Attrition ? ", a pass of attrition" : "");

			NeuralAirWingFlight flight { pTarget->UniqueID, pTargetType, isBuildingTarget, {}, now, pick.Attrition };

			// Every order of the wing first, then the flights: ContinueAirStrike weighs the risk at the target
			// against the aircraft bound for it, and given one by one each member saw a wing of itself
			// alone and dropped its order (2026-11-20: of 184 wings at a target with known anti-air, 175
			// were over at the next tick, against 1 of 159 with none).
			for (auto const pMember : wing)
			{
				committedByTarget[pTarget->UniqueID] += AirSortieDamageVs(pMember, pTarget);
				state.Orders[pMember->UniqueID] = NeuralAirStrikeOrder { pTarget->UniqueID, now, targetCell };
				CountDecision(NeuralDecision::AirWingSortie, pHouse->ArrayIndex);
				NoteAirTask(pHouse, pMember, NeuralAirTask::Wing);
				flight.Members.push_back(pMember->UniqueID);
			}

			for (auto const pMember : wing)
				ContinueAirStrike(pHouse, pMember);

			state.Flights.push_back(std::move(flight));

			std::erase_if(ready, [&wing](AircraftClass* pAir) { return std::find(wing.begin(), wing.end(), pAir) != wing.end(); });
		}
	}

	// The aircraft waits on its pad for its wing: in the air (a strike called off, a patrol) it
	// drops its guard point and target and the engine flies it to a pad (AircraftClass::EnterIdleMode).
	// On its pad in an island war it sleeps: on Guard the engine sends an aircraft of a computer house at
	// a juicy target anywhere on the map (AircraftClass::Mission_Guard, HouseClass::Find_Juicy_Target),
	// and it was lost there alone (AirWing*).
	void WaitOnAirPad(HouseClass* pHouse, AircraftClass* pAir)
	{
		NoteAirTask(pHouse, pAir, NeuralAirTask::Pad);

		if (!pAir->IsInAir() && pAir->CurrentMission == Mission::Guard && IsAirIslandWar(pHouse))
		{
			pAir->QueueMission(Mission::Sleep, false);
			return;
		}

		if (!pAir->IsInAir() || pAir->CurrentMission == Mission::Enter)
			return;

		pAir->SetArchiveTarget(nullptr);
		pAir->SetTarget(nullptr);
		pAir->SetDestination(nullptr, true);
		pAir->EnterIdleMode(false, true);
	}

	// After its pass the engine flies an aircraft straight back to its pad (Mission::Enter), over the
	// base it has just struck: two Black Eagles that struck together were shot down that way in match
	// #11501. It flies home along the safe lane instead (GetSafeAirHop), hop by hop; from the last hop
	// the engine flies it in. A hop counts as reached within 8 cells, so a jet that overshoots it is
	// not sent back. True when it was sent to a hop.
	bool FlyAircraftHomeSafely(HouseClass* pHouse, AircraftClass* pAir)
	{
		if (!pAir->IsInAir())
			return false;

		auto& state = GetAirForceState(pHouse);

		if (auto const itOrd = state.Orders.find(pAir->UniqueID); itOrd != state.Orders.end() && !itOrd->second.Returning)
			return false;

		if (auto const itSolo = state.SoloStrikes.find(pAir->UniqueID); itSolo != state.SoloStrikes.end() && !itSolo->second.Returning)
			return false;

		auto const itTask = state.Tasks.find(pAir->UniqueID);
		const bool continuingSafeHome = pAir->CurrentMission == Mission::Move && itTask != state.Tasks.end()
			&& itTask->second.Task == NeuralAirTask::SafeHome;

		if (pAir->CurrentMission != Mission::Enter && !continuingSafeHome)
			return false;

		auto pDock = abstract_cast<BuildingClass*>(pAir->Destination);

		if (!pDock)
			pDock = abstract_cast<BuildingClass*>(pAir->GetNthLink(0));

		const CellStruct from = pAir->GetMapCoords();

		if (!pDock)
		{
			double bestDockDist = 1e18;

			for (auto const pBld : pHouse->Buildings)
			{
				if (!pBld || !pBld->IsAlive || pBld->InLimbo || !pBld->Type || !pBld->Type->Helipad)
					continue;

				const double d = pBld->GetMapCoords().DistanceFromSquared(from);

				if (d < bestDockDist)
				{
					bestDockDist = d;
					pDock = pBld;
				}
			}
		}

		if (!pDock || !pDock->Owner || (pDock->Owner != pHouse && !pHouse->IsAlliedWith(pDock->Owner)))
			return false;

		if (auto const itRoute = AirRoutes.find(pAir->UniqueID);
			itRoute != AirRoutes.end() && itRoute->second.Waypoint != CellStruct::Empty && from.DistanceFrom(itRoute->second.Waypoint) <= 9.0)
		{
			itRoute->second.Waypoint = CellStruct::Empty;
		}

		auto const pDockCell = pDock->GetCell();
		auto const pHop = GetSafeAirHop(pHouse, pAir, pDockCell);

		if (!pHop || pHop == pDockCell)
		{
			if (continuingSafeHome && pAir->CurrentMission != Mission::Enter)
			{
				pAir->SetDestination(nullptr, true);
				pAir->EnterIdleMode(false, true);
				return true;
			}

			return false;
		}

		if (!IssueMoveOrder(pAir, pHop, 3.0))
			return false;

		NoteAirTask(pHouse, pAir, NeuralAirTask::SafeHome);

		// TEMP diagnostic: the first safe flights home of a match.
		static int logSerial = -1;
		static int logCount = 0;

		if (logSerial != NeuralMatchSerial)
		{
			logSerial = NeuralMatchSerial;
			logCount = 0;
		}

		if (logCount++ < 60)
		{
			Debug::Log("[NeuralAI] TEMP air safe home: house %d %s %u at (%d,%d) -> hop (%d,%d) for its pad at (%d,%d), frame %d.\n",
				pHouse->ArrayIndex, pAir->Type->ID, pAir->UniqueID, from.X, from.Y, pHop->MapCoords.X, pHop->MapCoords.Y,
				pDockCell->MapCoords.X, pDockCell->MapCoords.Y, Unsorted::CurrentFrame);
		}

		return true;
	}

	// TEMP diagnostic (air wings, 2026-11-19): the task the AI last gave an aircraft and its wing order.
	void DescribeAirTask(HouseClass* pHouse, AircraftClass* pAir, char* buffer, size_t size)
	{
		auto& state = GetAirForceState(pHouse);
		const char* task = "none";
		int taskAge = -1;

		if (auto const itTask = state.Tasks.find(pAir->UniqueID); itTask != state.Tasks.end())
		{
			task = NeuralAirTaskNames[static_cast<size_t>(itTask->second.Task)];
			taskAge = Unsorted::CurrentFrame - itTask->second.Frame;
		}

		const char* wing = "none";

		if (auto const itOrder = state.Orders.find(pAir->UniqueID); itOrder != state.Orders.end())
			wing = itOrder->second.Returning ? "back" : "out";

		sprintf_s(buffer, size, "task %s %d frames ago, wing %s, plan %d", task, taskAge, wing, IsPlanMember(pHouse, pAir->UniqueID) ? 1 : 0);
	}

	// TEMP diagnostic: an air force aircraft of a Neural AI house is destroyed.
	void NoteAirForceDeath(HouseClass* pHouse, AircraftClass* pAir, TechnoClass* pKiller)
	{
		if (!pAir->Type || !IsAirForceCraft(pAir->Type))
			return;

		static int logSerial = -1;
		static int logCount = 0;

		if (logSerial != NeuralMatchSerial)
		{
			logSerial = NeuralMatchSerial;
			logCount = 0;
		}

		if (logCount++ >= 200)
			return;

		static const char* const missionNames[] = { "none", "sleep", "attack", "move", "qmove", "retreat", "guard", "sticky", "enter",
			"capture", "eaten", "harvest", "areaGuard", "return", "stop", "ambush", "hunt", "unload", "sabotage", "construction", "selling",
			"repair", "rescue", "missile", "harmless", "open", "patrol", "paradropApproach", "paradropOverfly", "wait", "attackMove",
			"spyplaneApproach", "spyplaneOverfly" };

		const int missionIndex = static_cast<int>(pAir->CurrentMission) + 1;
		const char* mission = missionIndex >= 0 && missionIndex < static_cast<int>(std::size(missionNames)) ? missionNames[missionIndex] : "?";
		const CellStruct at = pAir->GetMapCoords();
		char task[96];
		DescribeAirTask(pHouse, pAir, task, sizeof(task));

		const char* targetKind = "-";
		const char* targetID = "-";
		int targetCells = -1;

		if (auto const pTarget = abstract_cast<TechnoClass*>(pAir->Target))
		{
			auto const pTargetType = pTarget->GetTechnoType();
			targetID = pTargetType ? pTargetType->ID : "?";
			targetKind = pTarget->WhatAmI() == AbstractType::Building ? "building"
				: (pTarget->WhatAmI() == AbstractType::Aircraft ? (pTarget->IsInAir() ? "flyer" : "parked aircraft") : "ground unit");
			targetCells = static_cast<int>(pTarget->GetMapCoords().DistanceFrom(at));
		}

		int padCells = -1;

		for (auto const pBld : pHouse->Buildings)
		{
			if (pBld && pBld->IsAlive && !pBld->InLimbo && pBld->Type && pBld->Type->Helipad)
			{
				const int cells = static_cast<int>(pBld->GetMapCoords().DistanceFrom(at));
				padCells = padCells < 0 ? cells : std::min(padCells, cells);
			}
		}

		char caller[128] = "none";
		auto const& callers = LastOrderCaller[std::clamp(pHouse->ArrayIndex, 0, 31)];

		if (auto const itCaller = callers.find(pAir->UniqueID); itCaller != callers.end() && itCaller->second.File && itCaller->second.Kind)
		{
			sprintf_s(caller, "%s from %s:%d %d frames ago", itCaller->second.Kind, itCaller->second.File, itCaller->second.Line,
				Unsorted::CurrentFrame - itCaller->second.Frame);
		}

		auto const pKillerType = pKiller ? pKiller->GetTechnoType() : nullptr;

		Debug::Log("[NeuralAI] TEMP air death: house %d %s %u %s, mission %s, in air %d, at (%d,%d) %d cells from our nearest pad, target %s %s"
			" %d cells away, killer %s of house %d %d cells away, last order given by %s, frame %d.\n",
			pHouse->ArrayIndex, pAir->Type->ID, pAir->UniqueID, task, mission, pAir->IsInAir() ? 1 : 0, at.X, at.Y, padCells, targetKind,
			targetID, targetCells, pKillerType ? pKillerType->ID : "-", pKiller && pKiller->Owner ? pKiller->Owner->ArrayIndex : -1,
			pKiller ? static_cast<int>(pKiller->GetMapCoords().DistanceFrom(at)) : -1, caller, Unsorted::CurrentFrame);
	}

	// TEMP diagnostic: an aircraft of a Neural AI house destroys an enemy building.
	void NoteAirForceKill(HouseClass* pHouse, AircraftClass* pAir, BuildingClass* pVictim)
	{
		if (!pAir->Type || !pVictim->Type || pVictim->Type->Wall || pVictim->Type->Insignificant || pVictim->Type->InvisibleInGame)
			return;

		auto& state = GetAirForceState(pHouse);
		state.BuildingsDestroyed++;

		if (state.BuildingsDestroyed > 200)
			return;

		char task[96];
		DescribeAirTask(pHouse, pAir, task, sizeof(task));
		const CellStruct at = pVictim->GetMapCoords();

		Debug::Log("[NeuralAI] TEMP air kill: house %d %s %u destroyed %s of house %d at (%d,%d), %s, frame %d (destroyed %d).\n",
			pHouse->ArrayIndex, pAir->Type->ID, pAir->UniqueID, pVictim->Type->ID, pVictim->Owner ? pVictim->Owner->ArrayIndex : -1, at.X, at.Y,
			task, Unsorted::CurrentFrame, state.BuildingsDestroyed);
	}

	// Production shift of an aircraft type while Expert 8 is in charge: strike craft while there
	// are targets they can reach, interceptors while enemy air is seen (more while our anti-air does
	// not cover it). 0 under any other expert.
	int AirForceProductionBonus(HouseClass* pHouse, AircraftTypeClass* pType)
	{
		auto const pExt = HouseExt::Fetch(pHouse);

		if (!pExt || pExt->NeuralAI_ActiveExpert != 8 || !IsAirForceCraft(pType))
			return 0;

		auto const& state = RefreshAirForceSummary(pHouse);
		int bonus = 0;

		if (state.StrikeWork > 0 && AircraftHitsGround(pType))
			bonus += 250 + std::min(250, state.StrikeWork / 4);

		if (state.InterceptWork > 0 && AircraftHitsAir(pType))
			bonus += std::min(600, state.InterceptWork * 3);

		// Aircraft that die fast: sturdier types first, and less push for more of them.
		const int survival = AirForceSurvivalPercent(state);

		if (survival < 60)
			bonus += pType->Strength / 2 - 200;

		return bonus * survival / 100;
	}

	// Patrol stops Expert 8 puts first: the front of our army (close air support) and the target
	// of an airborne raid under way (cover for the drop), each only when its anti-air risk is low.
	void CollectAirForcePriorityStops(HouseClass* pHouse, std::vector<CellStruct>& out)
	{
		out.clear();

		auto const pExt = HouseExt::Fetch(pHouse);

		if (!pExt || pExt->NeuralAI_ActiveExpert != 8)
			return;

		auto const& state = RefreshAirForceSummary(pHouse);

		if (state.FrontCell != CellStruct::Empty && AirStrikeRiskAt(pHouse, state.FrontCell) <= 1)
			out.push_back(state.FrontCell);

		auto const& raid = GetMacroState(pHouse).Plans[static_cast<size_t>(NeuralPlanId::AirborneFront)];

		if (raid.Active && raid.TargetCell != CellStruct::Empty && AirStrikeRiskAt(pHouse, raid.TargetCell) <= 1)
			out.push_back(raid.TargetCell);
	}
}

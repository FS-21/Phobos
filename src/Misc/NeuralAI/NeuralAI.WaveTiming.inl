// NeuralAI.WaveTiming.inl: Fog-fair enemy strength estimate and the main wave launch gate.
// Part of NeuralAI.cpp (included in order; one translation unit).

namespace
{
	// Sightings seen this recently far (> 35 cells) from their house's known base count as an
	// army away from home; losses we watched are remembered this long.
	constexpr int WaveAwayRecentFrames = 450;
	constexpr int WaveLossMemoryFrames = 1800;

	// Smallest main wave worth sending (army value, one main battle tank is about 70): 150 at the
	// start, +10 per game minute, up to 450.
	int WaveMinimumValue(int frame)
	{
		return std::clamp(150 + frame / 90, 150, 450);
	}

	// Army value that launches the main wave even without a clear advantage (unless clearly
	// outmatched): 300 at the start, +15 per game minute, up to 900.
	int WaveCostFloor(int frame)
	{
		return std::clamp(300 + frame / 60, 300, 900);
	}

	void AddDecisionAmount(NeuralDecision decision, int houseIndex, int amount)
	{
		if (houseIndex >= 0 && houseIndex < 32 && amount > 0)
			DecisionCounts[houseIndex][static_cast<size_t>(decision)] += static_cast<uint32_t>(amount);
	}

	// Combat value of one of our ground fighters scaled by its health; 0 for unarmed, naval,
	// harvesters and units in limbo.
	int OwnGroundFighterValue(TechnoClass* pTechno)
	{
		if (!pTechno || !pTechno->IsAlive || pTechno->InLimbo)
			return 0;

		const auto what = pTechno->WhatAmI();

		if (what != AbstractType::Unit && what != AbstractType::Infantry)
			return 0;

		auto const pType = pTechno->GetTechnoType();

		if (!pType || pType->Naval || pType->Strength <= 0)
			return 0;

		if (auto const pUnitType = abstract_cast<UnitTypeClass*>(pType))
		{
			if (pUnitType->Harvester || pUnitType->ResourceGatherer)
				return 0;
		}

		int rate = 0;

		for (int i = 0; i < 2; ++i)
		{
			auto const pWS = pType->GetWeapon(i);
			rate = std::max(rate, pWS ? GetWeaponThreatRate(pWS->WeaponType) : 0);
		}

		if (rate <= 0)
			return 0;

		return GetCombatValueOfType(pType, false) * std::max(0, pTechno->Health) / pType->Strength;
	}

	// Every AI tick after the opponent model: rebuilds the strength of each enemy house from what
	// this house knows (known buildings, remembered sightings), records the sighted enemy units
	// we watched die (seen at the last update, gone now, last cell still known to us), and
	// compares our army with the strongest known enemy. Fog-fair; only order-free sums.
	void UpdateEnemyStrength(HouseClass* pHouse)
	{
		if (!pHouse)
			return;

		auto& state = GetWaveTimingState(pHouse);
		auto& macro = GetMacroState(pHouse);
		const int now = Unsorted::CurrentFrame;
		const int previousUpdate = state.LastUpdateFrame > now ? now : state.LastUpdateFrame;

		std::array<NeuralEnemyStrength, 32> enemies {};
		std::array<int, 32> sumX {};
		std::array<int, 32> sumY {};

		for (auto const pBld : BuildingClass::Array)
		{
			if (!pBld || pBld->InLimbo || !pBld->IsAlive || !pBld->Type || !pBld->Owner || pBld->Owner == pHouse
				|| pHouse->IsAlliedWith(pBld->Owner) || pBld->Owner->IsNeutral() || pBld->Type->Insignificant
				|| !IsEnemyBuildingKnown(pHouse, pBld))
			{
				continue;
			}

			const int idx = std::clamp(pBld->Owner->ArrayIndex, 0, 31);
			const CellStruct bc = pBld->GetMapCoords();
			enemies[idx].KnownBuildings++;
			sumX[idx] += bc.X;
			sumY[idx] += bc.Y;

			if (pBld->Type->IsBaseDefense)
				enemies[idx].DefenseValue += GetCombatValueOfType(pBld->Type, false);
		}

		for (int i = 0; i < 32; ++i)
		{
			const int count = enemies[i].KnownBuildings;

			if (count > 0)
				enemies[i].BaseCenter = { static_cast<short>(sumX[i] / count), static_cast<short>(sumY[i] / count) };
		}

		// Enemy units alive now (keyed lookups only) and our own army value.
		std::unordered_set<DWORD> alive;
		int ownArmy = 0;

		for (auto const pTech : TechnoClass::Array)
		{
			if (!pTech || !pTech->IsAlive || !pTech->Owner || pTech->WhatAmI() == AbstractType::Building)
				continue;

			if (pTech->Owner == pHouse)
				ownArmy += OwnGroundFighterValue(pTech);
			else if (!pHouse->IsAlliedWith(pTech->Owner))
				alive.insert(pTech->UniqueID);
		}

		for (auto const& [uid, sighting] : macro.EnemySightings)
		{
			if (sighting.OwnerIndex < 0 || sighting.OwnerIndex >= 32 || sighting.Category == EnemyForceCategory::Naval
				|| state.SeenDead.contains(uid))
			{
				continue;
			}

			if (!alive.contains(uid) && sighting.LastSeenFrame >= previousUpdate
				&& IsCellKnownToAI(pHouse, sighting.LastCoords))
			{
				state.SeenDead.insert(uid);
				state.Losses.push_back({ now, sighting.OwnerIndex, sighting.Value });
				continue;
			}

			const int age = now - sighting.LastSeenFrame;
			const int weighted = sighting.Value * std::max(0, EnemySightingMemoryFrames - age) / EnemySightingMemoryFrames;
			auto& enemy = enemies[sighting.OwnerIndex];
			enemy.Army += weighted;

			if (age <= WaveAwayRecentFrames && enemy.BaseCenter != CellStruct::Empty
				&& sighting.LastCoords.DistanceFrom(enemy.BaseCenter) > 35.0)
			{
				enemy.ArmyAway += weighted;
			}
		}

		// Forget the dead once their sighting expired, and losses older than the memory.
		std::erase_if(state.SeenDead, [&macro](DWORD uid) { return !macro.EnemySightings.contains(uid); });
		std::erase_if(state.Losses, [now](const NeuralEnemyLoss& loss) { return now - loss.Frame > WaveLossMemoryFrames; });

		for (auto const& loss : state.Losses)
			enemies[std::clamp(loss.OwnerIndex, 0, 31)].RecentLosses += loss.Value;

		// Advantage against the strongest known enemy (army + defenses), never below the
		// minimum wave value, so an unseen enemy is not taken for an empty one.
		int strongest = 0;

		for (auto const& enemy : enemies)
			strongest = std::max(strongest, enemy.Army + enemy.DefenseValue);

		state.Enemies = enemies;
		state.OwnArmyValue = ownArmy;
		state.AdvantageRatio = std::clamp(ownArmy * 100 / std::max(strongest, WaveMinimumValue(now)), 0, 1000);
		state.LastUpdateFrame = now;
	}

	struct NeuralWaveCheck
	{
		bool Launch { false };
		bool Window { false };
		bool Floor { false };
		int Ratio { 0 };
		int OurValue { 0 };
		int EnemyValue { 0 };
		int HeldFrames { 0 };
		const char* Reason { "" };
	};

	// Should the main wave go to target now? Our value (readyValue: idle fighters on their way,
	// plus our fighters already within 14 cells of the target) against the enemy expected there:
	// known defenses within 14 cells (x1.5), remembered enemy units within 25 cells (any house),
	// and the rest of the target owner's army (half, a quarter when it was just seen far from its
	// base). Goes at 130% of that, 10 points less per minute already held (down to 90%, so two
	// equal armies do not wait for each other forever), 90% in a window (army away, heavy recent
	// losses, weak known defenses near the target, our counterattack or blackout window), at the
	// cost floor unless outmatched (60%), or at 1.5x the floor regardless. With an offensive bank
	// (HasOffensiveBank) losses are bought back at once: 100% (75% held, 70% in a window) and the
	// floor from 45%. The smallest value and the floor follow the house's learned wave mass
	// (waveMass, WaveMassPermille); the ratios do not.
	NeuralWaveCheck EvaluateWaveLaunch(HouseClass* pHouse, const CellStruct& target, int readyValue, bool tacticalWindow, int waveMass)
	{
		NeuralWaveCheck check {};

		if (!pHouse || target == CellStruct::Empty)
			return check;

		auto const& state = GetWaveTimingState(pHouse);
		auto const& macro = GetMacroState(pHouse);
		const int now = Unsorted::CurrentFrame;

		int owner = -1;
		double ownerDist = 20.0;
		int defenseNear = 0;
		int buildingsNear = 0;

		for (auto const pBld : BuildingClass::Array)
		{
			if (!pBld || pBld->InLimbo || !pBld->IsAlive || !pBld->Type || !pBld->Owner || pBld->Owner == pHouse
				|| pHouse->IsAlliedWith(pBld->Owner) || pBld->Owner->IsNeutral() || pBld->Type->Insignificant
				|| !IsEnemyBuildingKnown(pHouse, pBld))
			{
				continue;
			}

			const double d = pBld->GetMapCoords().DistanceFrom(target);

			if (d < ownerDist)
			{
				ownerDist = d;
				owner = std::clamp(pBld->Owner->ArrayIndex, 0, 31);
			}

			if (d <= 14.0)
			{
				buildingsNear++;

				if (pBld->Type->IsBaseDefense)
					defenseNear += GetCombatValueOfType(pBld->Type, false);
			}
		}

		// No known building near the target (scouting a start spot, an ally rescue): the army that
		// answers is the largest one we remember.
		if (owner < 0)
		{
			int largest = 0;

			for (int i = 0; i < 32; ++i)
			{
				if (state.Enemies[i].Army > largest)
				{
					largest = state.Enemies[i].Army;
					owner = i;
				}
			}
		}

		int armyNear = 0;
		int ownerArmyNear = 0;

		for (auto const& [uid, sighting] : macro.EnemySightings)
		{
			if (sighting.OwnerIndex < 0 || sighting.OwnerIndex >= 32 || sighting.Category == EnemyForceCategory::Naval
				|| state.SeenDead.contains(uid) || sighting.LastCoords.DistanceFrom(target) > 25.0)
			{
				continue;
			}

			const int age = now - sighting.LastSeenFrame;
			const int weighted = sighting.Value * std::max(0, EnemySightingMemoryFrames - age) / EnemySightingMemoryFrames;
			armyNear += weighted;

			if (sighting.OwnerIndex == owner)
				ownerArmyNear += weighted;
		}

		int enemyValue = defenseNear * 3 / 2 + armyNear;
		bool armyAway = false;
		bool heavyLosses = false;

		if (owner >= 0)
		{
			auto const& enemy = state.Enemies[owner];
			const int away = std::min(enemy.ArmyAway, std::max(0, enemy.Army - ownerArmyNear));
			const int elsewhere = std::max(0, enemy.Army - ownerArmyNear - away);
			enemyValue += elsewhere / 2 + away / 4;
			armyAway = away >= 150 && away * 100 >= enemy.Army * 40;
			heavyLosses = enemy.RecentLosses >= 150 && enemy.RecentLosses * 100 >= (enemy.Army + enemy.RecentLosses) * 35;
		}

		int engaged = 0;

		for (auto const pTech : TechnoClass::Array)
		{
			if (pTech && pTech->Owner == pHouse && pTech->GetMapCoords().DistanceFrom(target) <= 14.0)
				engaged += OwnGroundFighterValue(pTech);
		}

		const bool weakDefenses = buildingsNear >= 2 && defenseNear <= 60;
		check.Window = armyAway || heavyLosses || weakDefenses || tacticalWindow;
		check.OurValue = readyValue + engaged;
		check.EnemyValue = std::max(1, enemyValue);
		check.Ratio = std::clamp(check.OurValue * 100 / check.EnemyValue, 0, 1000);

		const int minimum = WaveMinimumValue(now) * waveMass / 1000;
		const int floor = WaveCostFloor(now) * waveMass / 1000;
		check.HeldFrames = state.Holding ? now - state.HoldSinceFrame : 0;
		const bool offensiveBank = HasOffensiveBank(pHouse);
		const int required = offensiveBank
			? (check.Window ? 70 : std::max(75, 100 - check.HeldFrames / 90))
			: (check.Window ? 90 : std::max(90, 130 - check.HeldFrames / 90));
		const bool byRatio = check.OurValue >= minimum && check.Ratio >= required;
		check.Floor = !byRatio && ((check.OurValue >= floor && check.Ratio >= (offensiveBank ? 45 : 60)) || check.OurValue >= floor * 3 / 2);
		check.Launch = byRatio || check.Floor;
		check.Reason = check.Floor ? "floor"
			: (!check.Launch ? "held"
				: (armyAway ? "armyAway" : (heavyLosses ? "losses" : (weakDefenses ? "weakDefenses"
					: (tacticalWindow ? "tactical" : (offensiveBank ? "bank" : "ratio"))))));

		return check;
	}

	// Whether a unit of this type shoots at aircraft: one of its two weapons has an anti-air
	// projectile.
	bool ShootsAtAircraft(TechnoTypeClass* pType)
	{
		if (!pType)
			return false;

		for (int i = 0; i < 2; ++i)
		{
			auto const pWeapon = pType->GetWeapon(i) ? pType->GetWeapon(i)->WeaponType : nullptr;

			if (pWeapon && pWeapon->Projectile && pWeapon->Projectile->AA)
				return true;
		}

		return false;
	}

	// Known enemy buildings within WaveVerdictCells of a cell.
	int CountKnownEnemyBuildingsNear(HouseClass* pHouse, const CellStruct& cell)
	{
		int count = 0;

		for (auto const pBld : BuildingClass::Array)
		{
			if (pBld && !pBld->InLimbo && pBld->IsAlive && pBld->Type && pBld->Owner && pBld->Owner != pHouse
				&& !pHouse->IsAlliedWith(pBld->Owner) && !pBld->Owner->IsNeutral() && !pBld->Type->Insignificant
				&& IsEnemyBuildingKnown(pHouse, pBld) && pBld->GetMapCoords().DistanceFrom(cell) <= WaveVerdictCells)
			{
				count++;
			}
		}

		return count;
	}

	// Waves in a row that took no building, as the lesson stands now: a step fades every
	// WaveLessonStepFrames since the last one.
	int WaveLessonStepsNow(HouseClass* pHouse)
	{
		auto const& macro = GetMacroState(pHouse);
		const int age = Unsorted::CurrentFrame - macro.WaveLessonFrame;

		return age < 0 ? 0 : std::max(0, macro.WaveLessonSteps - age / WaveLessonStepFrames);
	}

	// Unit count a wave of the tank mass waits for: TankMassBaseWave, WaveLessonPercent of it for
	// each wave in a row that took no building, up to TankMassWaveCap.
	int TankMassWaveUnits(HouseClass* pHouse)
	{
		int units = TankMassBaseWave;

		for (int step = 0; step < WaveLessonStepsNow(pHouse) && units < TankMassWaveCap; ++step)
			units = units * WaveLessonPercent / 100;

		return std::min(units, TankMassWaveCap);
	}

	// Verdict of the wave under way, for the lesson of the next one. It took a building when fewer
	// known enemy buildings stand near its target than when it left. The verdict comes once a unit
	// of ours got there and none is left WaveVerdictQuietFrames after the last dispatch, after
	// WaveVerdictFrames, or at once when the next wave starts (now).
	void UpdateWaveVerdict(HouseClass* pHouse, bool now)
	{
		auto& macro = GetMacroState(pHouse);

		if (!macro.WaveVerdictPending)
			return;

		const int frame = Unsorted::CurrentFrame;
		bool over = now || frame - macro.WaveStartFrame >= WaveVerdictFrames;

		if (!over)
		{
			if (!IsAITickWindow(150, 0))
				return;

			bool there = false;

			for (auto const pTech : TechnoClass::Array)
			{
				if (pTech && pTech->Owner == pHouse && pTech->GetMapCoords().DistanceFrom(macro.WaveVerdictCell) <= WaveVerdictCells
					&& OwnGroundFighterValue(pTech) > 0)
				{
					there = true;
					break;
				}
			}

			if (there)
				macro.WaveVerdictArrived = true;

			over = !there && macro.WaveVerdictArrived && frame - macro.LastWaveDispatchFrame >= WaveVerdictQuietFrames;
		}

		if (!over)
			return;

		const int standing = CountKnownEnemyBuildingsNear(pHouse, macro.WaveVerdictCell);
		const bool tookBuilding = standing < macro.WaveVerdictBuildings;
		const int stepsBefore = WaveLessonStepsNow(pHouse);

		if (tookBuilding)
		{
			macro.WaveLessonSteps = 0;
			macro.WaveFailuresInARow = 0;
		}
		else
		{
			macro.WaveLessonSteps = std::min(WaveLessonStepsNow(pHouse) + 1, WaveLessonMaxSteps);
			macro.WaveLessonFrame = frame;
			macro.WaveFailuresInARow++;
		}

		// Siege mode: the places our waves did not take (Siege*). A building taken there clears the place.
		{
			auto& places = macro.ResistedPlaces;
			std::erase_if(places, [frame](const NeuralResistedPlace& place)
				{ return frame - place.Frame > SiegeMemoryFrames || frame < place.Frame; });
			auto const itPlace = std::find_if(places.begin(), places.end(), [&macro](const NeuralResistedPlace& place)
				{ return place.Cell.DistanceFrom(macro.WaveVerdictCell) <= WaveVerdictCells; });

			if (tookBuilding)
			{
				if (itPlace != places.end())
					places.erase(itPlace);
			}
			else if (itPlace != places.end())
			{
				itPlace->Failures++;
				itPlace->Frame = frame;
				itPlace->Owner = macro.WaveVerdictOwner;
			}
			else
			{
				places.push_back({ macro.WaveVerdictCell, macro.WaveVerdictOwner, 1, frame });
			}
		}

		// The verdict is the result of the mission the wave left for (Reprisal, Clear Our Ground).
		const bool onMission = macro.WaveVerdictMission >= 0 && macro.WaveVerdictMission < static_cast<int>(NeuralPlanId::Count);

		if (onMission)
		{
			CountPlanOutcome(pHouse->ArrayIndex, static_cast<NeuralPlanId>(macro.WaveVerdictMission),
				tookBuilding ? NeuralPlanOutcome::Succeeded : NeuralPlanOutcome::Failed);
		}

		Debug::Log("[NeuralAI] Frame %d | House #%d wave verdict at (%d,%d): %s, %d of %d known buildings stand, %s, mission %s, "
			"lesson steps %d -> %d, next wave waits for %d units, %d failures in a row\n",
			frame, pHouse->ArrayIndex, macro.WaveVerdictCell.X, macro.WaveVerdictCell.Y, tookBuilding ? "took a building" : "took none",
			standing, macro.WaveVerdictBuildings, macro.WaveVerdictArrived ? "arrived" : "never arrived",
			onMission ? NeuralPlanNames[macro.WaveVerdictMission] : "none", stepsBefore, macro.WaveLessonSteps, TankMassWaveUnits(pHouse),
			macro.WaveFailuresInARow);
		macro.WaveVerdictPending = false;
		macro.WaveVerdictMission = -1;
	}

	// A new wave of the tank mass leaves for a target with known enemy buildings: it waits for its
	// verdict. The wave before it gets its own first. mission: the plan id of the mission the
	// target stands for (Reprisal, Clear Our Ground), -1 for none; the wave counts as its start.
	// owner: the house of the target building (siege mode), -1 for none.
	// A wave to about the same place as the one waiting for its verdict joins it, and the verdict
	// waits for both. One to another place before the last got there replaces it with no verdict:
	// judged at once, every change of target was a failure (match #12285: house 2 sent 8 waves of
	// 30 to 50 units at 153% to 248% of the enemy expected to 8 targets in 1545 frames, each one
	// "never arrived", 20 failures in a row, and the next wave asked for 32 units).
	void NoteWaveForVerdict(HouseClass* pHouse, const CellStruct& target, int mission, int owner)
	{
		auto& macro = GetMacroState(pHouse);

		if (macro.WaveVerdictPending)
		{
			if (target.DistanceFrom(macro.WaveVerdictCell) <= WaveVerdictCells)
				return;

			if (!macro.WaveVerdictArrived && Unsorted::CurrentFrame - macro.WaveStartFrame < WaveVerdictFrames)
			{
				Debug::Log("[NeuralAI] Frame %d | House #%d wave to (%d,%d) replaced by one to (%d,%d) before it got there, no verdict\n",
					Unsorted::CurrentFrame, pHouse->ArrayIndex, macro.WaveVerdictCell.X, macro.WaveVerdictCell.Y, target.X, target.Y);
				macro.WaveVerdictPending = false;
				macro.WaveVerdictMission = -1;
			}
		}

		UpdateWaveVerdict(pHouse, true);

		const int buildings = CountKnownEnemyBuildingsNear(pHouse, target);

		if (buildings <= 0)
			return;

		macro.WaveVerdictPending = true;
		macro.WaveVerdictArrived = false;
		macro.WaveVerdictCell = target;
		macro.WaveVerdictBuildings = buildings;
		macro.WaveVerdictMission = mission;
		macro.WaveVerdictOwner = owner;

		if (mission >= 0 && mission < static_cast<int>(NeuralPlanId::Count))
			CountPlanOutcome(pHouse->ArrayIndex, static_cast<NeuralPlanId>(mission), NeuralPlanOutcome::Started);
	}

	// Count a hold (once per hold, plus every held tick) or a new wave launch for the telemetry;
	// a passed check ends the current hold.
	void NoteWaveDecision(HouseClass* pHouse, const NeuralWaveCheck& check, bool newWave, int units)
	{
		auto& state = GetWaveTimingState(pHouse);
		const int idx = pHouse->ArrayIndex;

		if (!check.Launch)
		{
			CountDecision(NeuralDecision::WaveHeldTick, idx);

			if (!state.Holding)
			{
				state.Holding = true;
				state.HoldSinceFrame = Unsorted::CurrentFrame;
				CountDecision(NeuralDecision::WaveHeld, idx);
				Debug::Log("[NeuralAI] Frame %d | House #%d holds its wave: value %d vs %d expected (ratio %d%%)\n",
					Unsorted::CurrentFrame, idx, check.OurValue, check.EnemyValue, check.Ratio);
			}

			return;
		}

		if (!newWave)
		{
			state.Holding = false;
			return;
		}

		CountDecision(NeuralDecision::WaveLaunched, idx);
		AddDecisionAmount(NeuralDecision::WaveLaunchRatioSum, idx, check.Ratio);
		AddDecisionAmount(NeuralDecision::WaveLaunchUnitsSum, idx, units);

		if (check.Floor)
			CountDecision(NeuralDecision::WaveFloorLaunch, idx);
		else if (check.Window)
			CountDecision(NeuralDecision::WaveWindowLaunch, idx);

		Debug::Log("[NeuralAI] Frame %d | House #%d wave start: %d units, value %d vs %d expected (ratio %d%%, %s, held %d frames)\n",
			Unsorted::CurrentFrame, idx, units, check.OurValue, check.EnemyValue, check.Ratio, check.Reason,
			check.HeldFrames);
		state.Holding = false;
	}

	// Army nobody uses. The Micro reports the size of its idle pools at every AI tick; the smallest
	// of each kind over a window of ArmyIdleWindowFrames is what stood idle the whole time, since a
	// unit that a wave, a plan or a transport took left the pool. Production reads the last
	// finished window (IsArmyKindUnused).
	void NoteIdleArmy(HouseClass* pHouse, int idleVehicles, int idleInfantry, int idleShips)
	{
		auto& macro = GetMacroState(pHouse);
		const int now = Unsorted::CurrentFrame;
		const std::array<int, 3> idle { idleVehicles, idleInfantry, idleShips };

		if (macro.ArmyIdleWindowStart < 0 || now < macro.ArmyIdleWindowStart)
		{
			macro.ArmyIdleWindowStart = now;
			macro.ArmyIdleFloor = idle;
			return;
		}

		for (size_t i = 0; i < idle.size(); ++i)
			macro.ArmyIdleFloor[i] = std::min(macro.ArmyIdleFloor[i], idle[i]);

		if (now - macro.ArmyIdleWindowStart < ArmyIdleWindowFrames)
			return;

		macro.ArmyUnused = macro.ArmyIdleFloor;
		macro.ArmyIdleFloor = idle;
		macro.ArmyIdleWindowStart = now;
	}

	// Idle warships with nothing in reach (no coastal building their gun hits from their sea, no enemy
	// ship in it), reported at every AI tick by the coastal bombardment: the fewest over a window of
	// ArmyIdleWindowFrames is what had nothing to do the whole time (IsArmyKindUnused).
	void NoteShipsWithoutTarget(HouseClass* pHouse, int ships)
	{
		auto& macro = GetMacroState(pHouse);
		const int now = Unsorted::CurrentFrame;

		if (macro.ShipsWithoutTargetWindowStart < 0 || now < macro.ShipsWithoutTargetWindowStart)
		{
			macro.ShipsWithoutTargetWindowStart = now;
			macro.ShipsWithoutTargetFloor = ships;

			return;
		}

		macro.ShipsWithoutTargetFloor = std::min(macro.ShipsWithoutTargetFloor, ships);

		if (now - macro.ShipsWithoutTargetWindowStart < ArmyIdleWindowFrames)
			return;

		const int before = macro.ShipsWithoutTarget;
		macro.ShipsWithoutTarget = macro.ShipsWithoutTargetFloor;
		macro.ShipsWithoutTargetFloor = ships;
		macro.ShipsWithoutTargetWindowStart = now;

		if ((before >= ShipsWithoutTargetBrake) != (macro.ShipsWithoutTarget >= ShipsWithoutTargetBrake))
		{
			Debug::Log("[NeuralAI] Frame %d | House #%d warships %s: %d idle with nothing in reach over %d frames (pause at %d)\n",
				now, pHouse->ArrayIndex, macro.ShipsWithoutTarget >= ShipsWithoutTargetBrake ? "paused" : "resumed",
				macro.ShipsWithoutTarget, ArmyIdleWindowFrames, ShipsWithoutTargetBrake);
		}
	}

	bool IsArmyKindUnused(HouseClass* pHouse, ArmyKind kind)
	{
		const size_t i = static_cast<size_t>(kind);
		auto const& macro = GetMacroState(pHouse);

		return macro.ArmyUnused[i] >= ArmyIdleBrake[i]
			|| (kind == ArmyKind::Ships && macro.ShipsWithoutTarget >= ShipsWithoutTargetBrake);
	}

	// Sea control (SeaControl*), once per production pass: no enemy ship in sight for
	// SeaControlQuietFrames, no enemy shipyard this house has seen still standing, no submarine
	// threat or naval remnant hunt, and a shipyard of ours. Counts our warships that neither shell
	// from afar nor carry passengers (PlainWarships). Only what the house has seen is read.
	void UpdateSeaControl(HouseClass* pHouse)
	{
		auto& macro = GetMacroState(pHouse);
		const int now = Unsorted::CurrentFrame;
		int plainWarships = 0;

		for (auto const pOwnU : UnitClass::Array)
		{
			if (pOwnU && pOwnU->Owner == pHouse && pOwnU->IsAlive && !pOwnU->InLimbo && pOwnU->Type && pOwnU->Type->Naval
				&& pOwnU->Type->GetWeapon(0) && pOwnU->Type->GetWeapon(0)->WeaponType
				&& !IsAmphibiousOrAirTransport(pOwnU->Type) && !IsLongRangeSiegeShipType(pOwnU->Type))
			{
				plainWarships++;
			}
		}

		macro.PlainWarships = plainWarships;

		bool control = pHouse->NumShipyards > 0 && now - macro.LastEnemyNavySeenFrame >= SeaControlQuietFrames
			&& macro.SubThreatUntil <= now && macro.NavalRemnantUntil <= now;

		if (control)
		{
			for (auto const pBld : BuildingClass::Array)
			{
				if (pBld && pBld->IsAlive && !pBld->InLimbo && pBld->Type && pBld->Type->Naval
					&& pBld->Type->Factory == AbstractType::UnitType && pBld->Owner && pBld->Owner != pHouse
					&& !pHouse->IsAlliedWith(pBld->Owner) && !pBld->Owner->IsNeutral() && !pBld->Owner->Defeated
					&& IsEnemyBuildingKnown(pHouse, pBld))
				{
					control = false;
					break;
				}
			}
		}

		if (control != macro.SeaControl)
		{
			Debug::Log("[NeuralAI] Frame %d | House #%d sea control %s: last enemy ship seen %d frames ago, %d plain warships (keeps %d)\n",
				now, pHouse->ArrayIndex, control ? "gained" : "lost", now - macro.LastEnemyNavySeenFrame, plainWarships,
				SeaControlWarships);
			macro.SeaControl = control;
		}
	}

	// Land control (LandControl*), once per production pass: the house has seen enemy buildings and
	// none it knows of stands on land, no enemy unit on the ground was in sight for
	// LandControlQuietFrames, and an enemy ship was in sight in the last LandControlNavyFrames or an
	// enemy shipyard it knows of stands. Only what the house has seen is read.
	void UpdateLandControl(HouseClass* pHouse)
	{
		auto& macro = GetMacroState(pHouse);
		const int now = Unsorted::CurrentFrame;
		bool control = !macro.SeenEnemyBuildings.empty() && now - macro.LastEnemyGroundSeenFrame >= LandControlQuietFrames;
		bool knownEnemyShipyard = false;

		if (control)
		{
			for (auto const pBld : BuildingClass::Array)
			{
				if (!pBld || !pBld->IsAlive || pBld->InLimbo || !pBld->Type || !pBld->Owner || pBld->Owner == pHouse
					|| pHouse->IsAlliedWith(pBld->Owner) || pBld->Owner->IsNeutral() || pBld->Owner->Defeated
					|| !IsEnemyBuildingKnown(pHouse, pBld))
				{
					continue;
				}

				if (!pBld->Type->Naval)
				{
					control = false;
					break;
				}

				knownEnemyShipyard = true;
			}
		}

		control = control && (knownEnemyShipyard
			|| (macro.LastEnemyNavySeenFrame > 0 && now - macro.LastEnemyNavySeenFrame <= LandControlNavyFrames));

		if (control != macro.LandControl)
		{
			Debug::Log("[NeuralAI] Frame %d | House #%d land control %s: last enemy on the ground seen %d frames ago, last enemy ship %d "
				"frames ago, known enemy shipyard %d\n", now, pHouse->ArrayIndex, control ? "gained" : "lost",
				now - macro.LastEnemyGroundSeenFrame, now - macro.LastEnemyNavySeenFrame, knownEnemyShipyard ? 1 : 0);
			macro.LandControl = control;
		}
	}

	// Offensive bank (HasOffensiveBank): against an enemy across the water the harassment from range
	// (naval shelling, artillery on the economy, air strikes and corridors, rotating raids, siege
	// batteries) gets +350 when its context is there; on shared land the main assaults get +200.
	int OffensiveBankPlanUtility(HouseClass* pHouse, NeuralPlanId plan)
	{
		if (!HasOffensiveBank(pHouse))
			return 0;

		if (GetMacroState(pHouse).EnemyAcrossWater)
		{
			switch (plan)
			{
			case NeuralPlanId::NavalStrikeGroup:
			case NeuralPlanId::ArtilleryEcoSiege:
			case NeuralPlanId::AirStrike:
			case NeuralPlanId::AirCorridor:
			case NeuralPlanId::RotatingRaids:
			case NeuralPlanId::SiegeBattery:
				return 350;
			default:
				return 0;
			}
		}

		switch (plan)
		{
		case NeuralPlanId::TimingRush:
		case NeuralPlanId::Pincer:
		case NeuralPlanId::ThreeFrontAssault:
		case NeuralPlanId::RollingSiege:
			return 200;
		default:
			return 0;
		}
	}

	// Main assault plans (everything idle against a base) weigh our advantage: up to +150 when
	// we clearly lead, down to -200 when outmatched. Raids and harassment plans keep their own
	// small groups and are not touched.
	int MainAssaultAdvantageUtility(int houseIndex, NeuralPlanId plan)
	{
		if (plan != NeuralPlanId::TimingRush && plan != NeuralPlanId::Pincer && plan != NeuralPlanId::ThreeFrontAssault
			&& plan != NeuralPlanId::AllyJointAttack)
		{
			return 0;
		}

		return std::clamp(WaveTimingStates[std::clamp(houseIndex, 0, 31)].AdvantageRatio - 120, -200, 150);
	}

	// A seated fighting vehicle (PassengerFill*): open-topped with seats, whose passengers fire from it,
	// or a gunner vehicle, whose weapon comes from its passenger; one we may load ourselves.
	bool IsSeatedFightingVehicleType(const TechnoTypeClass* pType)
	{
		return pType && pType->WhatAmI() == AbstractType::UnitType && !pType->Naval
			&& ((pType->OpenTopped && pType->Passengers > 0) || pType->Gunner)
			&& !HasNoManualEnter(pType) && !HasNoManualUnload(pType);
	}

	// Seats a seated fighting vehicle still has: a gunner vehicle takes one passenger.
	int SeatedFightingVehicleSeatsLeft(const UnitClass* pUnit)
	{
		const int taken = pUnit->Passengers.NumPassengers;

		return pUnit->Type->Gunner ? (taken == 0 ? 1 : 0) : std::max(0, pUnit->Type->Passengers - taken);
	}

	// A soldier worth a seat (PassengerFill*): a gun that hits from PassengerGunMinRange or farther and
	// no special job.
	bool IsFitPassengerSoldierType(InfantryTypeClass* pType)
	{
		if (!pType || pType->Engineer || pType->Doggie || pType->Thief || pType->VehicleThief || pType->Ivan || pType->C4
			|| IsSpyInfantryType(pType))
		{
			return false;
		}

		for (int i = 0; i < 2; ++i)
		{
			auto const pWS = pType->GetWeapon(i);
			auto const pWeapon = pWS ? pWS->WeaponType : nullptr;

			if (pWeapon && pWeapon->Damage > 0 && pWeapon->Range >= PassengerGunMinRange)
				return true;
		}

		return false;
	}

	// Held to fill at home (PassengerFill*): the waves leave it out.
	bool IsPassengerFillHeld(HouseClass* pHouse, DWORD uid)
	{
		return GetMacroState(pHouse).PassengerFillHolds.contains(uid);
	}

	bool OrderPlanSoldierAboard(FootClass* pSoldier, UnitClass* pVehicle);

	// TEMP passenger diagnostic (P4): how the tracked boarding orders ended since the last check.
	void ResolvePassengerBoardTrack(HouseClass* pHouse)
	{
		auto& macro = GetMacroState(pHouse);

		if (macro.PassengerBoardTrack.empty())
			return;

		const int now = Unsorted::CurrentFrame;
		std::unordered_map<DWORD, InfantryClass*> soldiers;
		std::unordered_map<DWORD, UnitClass*> vehicles;

		for (auto const pInf : InfantryClass::Array)
		{
			if (pInf && pInf->Owner == pHouse && pInf->IsAlive)
				soldiers[pInf->UniqueID] = pInf;
		}

		for (auto const pUnit : UnitClass::Array)
		{
			if (pUnit && pUnit->Owner == pHouse && pUnit->IsAlive && !pUnit->InLimbo)
				vehicles[pUnit->UniqueID] = pUnit;
		}

		auto const& callers = LastOrderCaller[std::clamp(pHouse->ArrayIndex, 0, 31)];

		std::erase_if(macro.PassengerBoardTrack, [&](const auto& entry)
			{
				auto const itSoldier = soldiers.find(entry.first);

				if (itSoldier == soldiers.end())
				{
					macro.PassengerBoardSoldierLost++;
					return true;
				}

				auto const pInf = itSoldier->second;

				if (pInf->Transporter)
				{
					(pInf->Transporter->UniqueID == entry.second.first ? macro.PassengerBoardAboard : macro.PassengerBoardElsewhere)++;
					return true;
				}

				auto const itVehicle = vehicles.find(entry.second.first);

				if (itVehicle == vehicles.end())
				{
					macro.PassengerBoardVehicleLost++;
					return true;
				}

				auto const pVehicle = itVehicle->second;
				const bool entering = (pInf->CurrentMission == Mission::Enter || pInf->QueuedMission == Mission::Enter)
					&& (pInf->Destination == pVehicle || pInf->ArchiveTarget == pVehicle || pInf->ContainsLink(pVehicle)
						|| (pInf->Destination && CellClass::Coord2Cell(pInf->Destination->GetCoords()).DistanceFrom(pVehicle->GetMapCoords()) <= 3.5));

				if (entering)
				{
					if (now - entry.second.second < PassengerBoardTrackFrames && now >= entry.second.second)
						return false;

					macro.PassengerBoardTimedOut++;
					return true;
				}

				macro.PassengerBoardDiverted++;
				auto const itCaller = callers.find(entry.first);
				std::string caller = "engine";

				if (itCaller != callers.end() && itCaller->second.Frame >= entry.second.second && itCaller->second.File)
				{
					caller = std::string(itCaller->second.Kind ? itCaller->second.Kind : "?") + " " + itCaller->second.File + ":"
						+ std::to_string(itCaller->second.Line);
				}

				macro.PassengerDivertCallers[caller]++;
				return true;
			});
	}

	// Seated fighting vehicles take soldiers before they go (user, 2026-11-21, P1 and P2): a new one at
	// home is held out of the waves until full or PassengerFillHoldFrames have gone, and our nearest fit
	// soldiers board it; back home empty it is held again (PassengerFillReholdFrames). One of a wave or
	// of a plan standing at its gathering point takes the fit soldiers of its wave or plan by it. Runs
	// before the tactical micro, so the wave dispatch reads the holds.
	void UpdatePassengerFill(HouseClass* pHouse)
	{
		auto& macro = GetMacroState(pHouse);
		const int now = Unsorted::CurrentFrame;

		if (macro.PassengerFillFrame >= 0 && now >= macro.PassengerFillFrame && now - macro.PassengerFillFrame < PassengerFillCheckFrames)
			return;

		macro.PassengerFillFrame = now;
		ResolvePassengerBoardTrack(pHouse);
		const CellStruct home = GetHouseHomeCell(pHouse);

		// The active plan of each plan member, by UniqueID.
		std::unordered_map<DWORD, int> memberPlan;

		for (int planIndex = 0; planIndex < static_cast<int>(macro.Plans.size()); ++planIndex)
		{
			if (macro.Plans[planIndex].Active)
			{
				for (const DWORD uid : macro.Plans[planIndex].Members)
					memberPlan.emplace(uid, planIndex);
			}
		}

		auto const planOf = [&memberPlan](DWORD uid)
		{
			auto const it = memberPlan.find(uid);

			return it != memberPlan.end() ? it->second : -1;
		};
		int seatsWanted = 0;
		int vehicles = 0;
		int full = 0;
		int partly = 0;
		int empty = 0;
		int emptyInWave = 0;

		// Soldiers on their way aboard a vehicle, by vehicle.
		auto const boardingFor = [pHouse](UnitClass* pVehicle)
		{
			int count = 0;

			for (auto const pInf : InfantryClass::Array)
			{
				if (pInf && pInf->Owner == pHouse && pInf->IsAlive && !pInf->InLimbo
					&& (pInf->CurrentMission == Mission::Enter || pInf->QueuedMission == Mission::Enter) && pInf->Destination == pVehicle)
				{
					count++;
				}
			}

			return count;
		};

		// The fit soldiers within reach of a vehicle, nearest first. group: PassengerGroupHome for the
		// home fill (standing soldiers out of the plans, or wave soldiers on their way),
		// PassengerGroupWave for the soldiers of a wave, or a plan index for that plan's soldiers, these
		// standing or walking.
		constexpr int PassengerGroupHome = -2;
		constexpr int PassengerGroupWave = -1;

		auto const soldiersFor = [&](UnitClass* pVehicle, double reachCells, int group)
		{
			std::vector<std::pair<int, InfantryClass*>> found;
			const int zone = ObjectZone(pVehicle, MovementZone::Normal);

			for (auto const pInf : InfantryClass::Array)
			{
				if (!pInf || pInf->Owner != pHouse || !pInf->IsAlive || pInf->InLimbo || pInf->Transporter || !IsFitPassengerSoldierType(pInf->Type)
					|| pInf->Target || pInf->HasAnyLink() || pInf->CurrentMission == Mission::Enter || pInf->QueuedMission == Mission::Enter
					|| IsInTransportOp(pHouse, pInf->UniqueID))
				{
					continue;
				}

				const Mission mission = pInf->CurrentMission;
				const bool standing = mission == Mission::Guard || mission == Mission::Area_Guard || mission == Mission::Sleep;
				const bool walking = mission == Mission::Move || mission == Mission::AttackMove;
				const bool inWave = macro.WaveGoals.contains(pInf->UniqueID);
				const int soldierPlan = IsPlanMember(pHouse, pInf->UniqueID) ? planOf(pInf->UniqueID) : -1;

				if (group >= 0)
				{
					if (soldierPlan != group || !(standing || walking))
						continue;
				}
				else if (IsPlanMember(pHouse, pInf->UniqueID))
				{
					continue;
				}
				else if (group == PassengerGroupWave ? !(inWave && (standing || walking)) : !(standing || (inWave && walking)))
				{
					continue;
				}

				const int distance = pInf->DistanceFrom(pVehicle);

				if (distance > static_cast<int>(reachCells * Unsorted::LeptonsPerCell) || ObjectZone(pInf, MovementZone::Normal) != zone)
					continue;

				if (!CanPassengerFitInTransport(pVehicle, pInf->Type))
					continue;

				found.push_back({ distance, pInf });
			}

			std::stable_sort(found.begin(), found.end(), [](const auto& a, const auto& b)
				{ return a.first < b.first || (a.first == b.first && a.second->UniqueID < b.second->UniqueID); });

			return found;
		};

		auto const board = [&](UnitClass* pVehicle, int seats, double reachCells, int group)
		{
			for (auto const& [distance, pInf] : soldiersFor(pVehicle, reachCells, group))
			{
				if (seats <= 0)
					break;

				// A soldier no code may order now (order leases) stays where it is, in its wave.
				if (!(group >= 0 ? OrderPlanSoldierAboard(pInf, pVehicle) : OrderFootToEnter(pInf, pVehicle)))
					continue;

				macro.WaveGoals.erase(pInf->UniqueID);
				macro.PassengerBoardOrders++;
				macro.PassengerBoardTrack[pInf->UniqueID] = { pVehicle->UniqueID, now };

				if (group >= 0)
					macro.PassengerPlanBoardOrders++;

				seats--;
			}
		};

		for (auto const pUnit : UnitClass::Array)
		{
			if (!pUnit || pUnit->Owner != pHouse || !pUnit->IsAlive || pUnit->InLimbo || !IsSeatedFightingVehicleType(pUnit->Type))
				continue;

			const DWORD uid = pUnit->UniqueID;
			const int seatsLeft = SeatedFightingVehicleSeatsLeft(pUnit);
			const bool inWave = macro.WaveGoals.contains(uid);
			vehicles++;
			(seatsLeft == 0 ? full : (pUnit->Passengers.NumPassengers > 0 ? partly : empty))++;

			if (seatsLeft > 0 && pUnit->Passengers.NumPassengers == 0 && inWave)
				emptyInWave++;

			auto const itHold = macro.PassengerFillHolds.find(uid);
			// Standing still with no destination: a transport on its way turns its passengers away (the
			// docking request is refused while it moves or has a destination, and they give up boarding).
			const bool vehicleStanding = (pUnit->CurrentMission == Mission::Guard || pUnit->CurrentMission == Mission::Area_Guard)
				&& !pUnit->Destination && !pUnit->Locomotor->Is_Moving();
			const int vehiclePlan = IsPlanMember(pHouse, uid) ? planOf(uid) : -1;

			// Held at home: the first time it is there with seats left; again, standing out of the plans
			// with fit soldiers within reach, PassengerFillReholdFrames after its last hold.
			auto const mayHoldAtHome = [&]()
			{
				auto const itDone = macro.PassengerFillDone.find(uid);

				if (itDone == macro.PassengerFillDone.end())
					return true;

				if (IsPlanMember(pHouse, uid) || !vehicleStanding || pUnit->Target
					|| (now - itDone->second < PassengerFillReholdFrames && now >= itDone->second))
				{
					return false;
				}

				return seatsLeft - boardingFor(pUnit) > 0 && !soldiersFor(pUnit, PassengerFillReachCells, PassengerGroupHome).empty();
			};

			if (itHold != macro.PassengerFillHolds.end())
			{
				// The hold ends once full or after PassengerFillHoldFrames.
				if (seatsLeft == 0 || now - itHold->second >= PassengerFillHoldFrames || now < itHold->second)
				{
					macro.PassengerFillHolds.erase(itHold);
					macro.PassengerFillDone[uid] = now;
					continue;
				}
			}
			else if (seatsLeft > 0 && !inWave && !IsInTransportOp(pHouse, uid)
				&& home != CellStruct::Empty && pUnit->GetMapCoords().DistanceFrom(home) <= PassengerFillHomeCells && mayHoldAtHome())
			{
				if (macro.PassengerFillDone.contains(uid))
					macro.PassengerReholds++;

				macro.PassengerFillHolds[uid] = now;
			}
			else if (seatsLeft > 0 && (inWave || vehiclePlan >= 0) && !IsInTransportOp(pHouse, uid) && vehicleStanding)
			{
				// A wave or a plan standing at its gathering point.
				const int seats = seatsLeft - boardingFor(pUnit);

				if (seats > 0)
					board(pUnit, seats, PassengerStagingReachCells, inWave ? PassengerGroupWave : vehiclePlan);

				continue;
			}
			else
			{
				continue;
			}

			const int seats = seatsLeft - boardingFor(pUnit);
			seatsWanted += std::max(0, seats);

			if (seats > 0)
				board(pUnit, seats, PassengerFillReachCells, PassengerGroupHome);
		}

		// Holds of vehicles gone (destroyed, sold, taken).
		std::erase_if(macro.PassengerFillHolds, [pHouse](const auto& entry)
			{
				return std::none_of(UnitClass::Array.begin(), UnitClass::Array.end(), [pHouse, &entry](const UnitClass* pUnit)
					{ return pUnit && pUnit->UniqueID == entry.first && pUnit->Owner == pHouse && pUnit->IsAlive; });
			});

		macro.PassengerSeatsWanted = seatsWanted;

		// TEMP diagnostic: the seated fighting vehicles and how full they go, and how the boarding orders
		// ended (P4), with the codes that gave the diverted soldiers their next order, most first.
		if (vehicles > 0 && (macro.PassengerLogFrame < 0 || now < macro.PassengerLogFrame || now - macro.PassengerLogFrame >= PassengerLogFrames))
		{
			macro.PassengerLogFrame = now;
			std::vector<std::pair<int, std::string>> divertCallers;

			for (auto const& [caller, count] : macro.PassengerDivertCallers)
				divertCallers.push_back({ count, caller });

			std::stable_sort(divertCallers.begin(), divertCallers.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
			std::string callerText;

			for (size_t i = 0; i < divertCallers.size() && i < 4; ++i)
				callerText += (i ? ", " : "") + divertCallers[i].second + " x" + std::to_string(divertCallers[i].first);

			Debug::Log("[NeuralAI] House %d passenger DIAG: seated fighting vehicles %d (full %d, partly %d, empty %d; empty in a wave %d), held %zu, "
				"seats wanted %d, boarding orders %d since the last line (%d to a plan vehicle, held again %d), frame %d.\n", pHouse->ArrayIndex, vehicles,
				full, partly, empty, emptyInWave, macro.PassengerFillHolds.size(), seatsWanted, macro.PassengerBoardOrders,
				macro.PassengerPlanBoardOrders, macro.PassengerReholds, now);
			Debug::Log("[NeuralAI] House %d passenger boarding DIAG: aboard %d, in another vehicle %d, soldier lost %d, vehicle lost %d, timed out %d, "
				"diverted %d (%s), still going %zu, frame %d.\n", pHouse->ArrayIndex, macro.PassengerBoardAboard, macro.PassengerBoardElsewhere,
				macro.PassengerBoardSoldierLost, macro.PassengerBoardVehicleLost, macro.PassengerBoardTimedOut, macro.PassengerBoardDiverted,
				callerText.empty() ? "-" : callerText.c_str(), macro.PassengerBoardTrack.size(), now);
			macro.PassengerBoardOrders = 0;
			macro.PassengerPlanBoardOrders = 0;
			macro.PassengerReholds = 0;
			macro.PassengerBoardAboard = 0;
			macro.PassengerBoardElsewhere = 0;
			macro.PassengerBoardSoldierLost = 0;
			macro.PassengerBoardVehicleLost = 0;
			macro.PassengerBoardTimedOut = 0;
			macro.PassengerBoardDiverted = 0;
			macro.PassengerDivertCallers.clear();
		}
	}

} // namespace

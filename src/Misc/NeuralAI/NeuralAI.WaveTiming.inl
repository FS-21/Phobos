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
	// we watched die (seen at the last update, gone now, last cell still in our sight), and
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
				&& IsCellInSightOfAI(pHouse, sighting.LastCoords))
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
	// floor from 45%.
	NeuralWaveCheck EvaluateWaveLaunch(HouseClass* pHouse, const CellStruct& target, int readyValue, bool tacticalWindow)
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

		const int minimum = WaveMinimumValue(now);
		const int floor = WaveCostFloor(now);
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

} // namespace

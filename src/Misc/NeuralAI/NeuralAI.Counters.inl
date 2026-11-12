// NeuralAI.Counters.inl: Counter composition: which buildable types beat the enemy army seen so far and what attacks the base now.
// Part of NeuralAI.cpp (included in order; one translation unit).

namespace
{
	// One slice of the seen enemy army: an armor type in one force category, by remembered value.
	struct NeuralCounterTarget
	{
		int ArmorIndex { 0 };
		EnemyForceCategory Category { EnemyForceCategory::Vehicle };
		int Value { 0 };
		// It attacked our buildings lately (its value already weighs more).
		bool AttackingNow { false };
	};

	// The seen enemy army as counter targets (fog-fair: built from the sightings memory only).
	struct NeuralCounterView
	{
		std::vector<NeuralCounterTarget> Targets {};
		int TotalValue { 0 };
		// Value-weighted mean main weapon range (leptons) of the seen ground and naval units, and
		// mean speed of the seen artillery.
		int GroundRange { 0 };
		int ArtillerySpeed { 0 };
		int VehicleShare { 0 };
		int ArtilleryShare { 0 };
		// Something attacked our buildings within CounterThreatNowFrames.
		bool UnderAttack { false };
	};

	// What hit our buildings within CounterThreatNowFrames counts CounterThreatNowWeight times in
	// the view: the answer to the threat at the door comes before the answer to an army only seen.
	// Experts other than the counter strategist take CounterShareOtherExperts percent of the shift
	// in peace and all of it under attack.
	constexpr int CounterThreatNowFrames = 1800;
	constexpr int CounterThreatNowWeight = 3;
	constexpr int CounterShareOtherExperts = 50;
	// While the base is under attack what can wait (engineers, spies, a harvester beyond one per
	// refinery, an expansion MCV, a deployer) loses this much production score: the answer to the
	// threat is built first.
	constexpr int ThreatFirstPenalty = 3000;

	// Production queues compared separately: a counter is chosen among what one factory can build.
	enum class NeuralCounterQueue : int
	{
		Infantry,
		Ground,
		Naval,
		Air,
		Defense,
		Count
	};

	NeuralCounterView BuildCounterView(HouseClass* pHouse)
	{
		NeuralCounterView view {};
		long long rangeSum = 0;
		long long rangeWeight = 0;
		long long speedSum = 0;
		int artilleryValue = 0;
		int vehicleValue = 0;

		for (auto const& [uid, sighting] : GetMacroState(pHouse).EnemySightings)
		{
			if (!sighting.Type)
				continue;

			// Same decay as the opponent profile, so older sightings count less.
			const int age = std::clamp(Unsorted::CurrentFrame - sighting.LastSeenFrame, 0, EnemySightingMemoryFrames);
			const int weighted = sighting.Value * (EnemySightingMemoryFrames - age) / EnemySightingMemoryFrames;

			if (weighted <= 0)
				continue;

			const int armorIndex = static_cast<int>(sighting.Type->Armor);
			auto const it = std::find_if(view.Targets.begin(), view.Targets.end(),
				[&](const NeuralCounterTarget& target) { return target.ArmorIndex == armorIndex && target.Category == sighting.Category; });

			if (it != view.Targets.end())
				it->Value += weighted;
			else
				view.Targets.push_back({ armorIndex, sighting.Category, weighted });

			view.TotalValue += weighted;

			if (sighting.Category != EnemyForceCategory::Air)
			{
				auto const pWS = const_cast<TechnoTypeClass*>(sighting.Type)->GetWeapon(0);
				auto const pWeapon = pWS ? pWS->WeaponType : nullptr;

				if (pWeapon && pWeapon->Range > 0)
				{
					rangeSum += static_cast<long long>(pWeapon->Range) * weighted;
					rangeWeight += weighted;
				}
			}

			if (sighting.Category == EnemyForceCategory::Artillery)
			{
				speedSum += static_cast<long long>(sighting.Type->Speed) * weighted;
				artilleryValue += weighted;
			}
			else if (sighting.Category == EnemyForceCategory::Vehicle)
			{
				vehicleValue += weighted;
			}
		}

		// The attackers of our buildings: their armor and kind weigh more, and one never sighted
		// (it shot from the fog, or the sighting faded) enters the view as a quarter of it.
		for (auto const& rec : GetMacroState(pHouse).AttackedBuildingsLIFO)
		{
			if (!rec.AttackerType || rec.Threat == NeuralThreatCategory::None
				|| Unsorted::CurrentFrame - rec.AttackFrame > CounterThreatNowFrames || Unsorted::CurrentFrame < rec.AttackFrame)
			{
				continue;
			}

			view.UnderAttack = true;
			const int armorIndex = static_cast<int>(rec.AttackerType->Armor);
			const bool byAir = rec.Threat == NeuralThreatCategory::Air;
			const bool byInfantry = rec.Threat == NeuralThreatCategory::Infantry;
			bool found = false;

			for (auto& target : view.Targets)
			{
				const bool sameKind = byAir ? target.Category == EnemyForceCategory::Air
					: byInfantry ? target.Category == EnemyForceCategory::Infantry
					: (target.Category != EnemyForceCategory::Air && target.Category != EnemyForceCategory::Infantry);

				if (target.ArmorIndex != armorIndex || !sameKind)
					continue;

				found = true;

				if (!target.AttackingNow)
				{
					target.AttackingNow = true;
					view.TotalValue += target.Value * (CounterThreatNowWeight - 1);
					target.Value *= CounterThreatNowWeight;
				}
			}

			if (!found)
			{
				const int value = std::max(rec.AttackerType->Cost, view.TotalValue / 4);
				const EnemyForceCategory category = byAir ? EnemyForceCategory::Air
					: byInfantry ? EnemyForceCategory::Infantry : EnemyForceCategory::Vehicle;
				view.Targets.push_back({ armorIndex, category, value, true });
				view.TotalValue += value;

				if (category == EnemyForceCategory::Vehicle)
					vehicleValue += value;
			}
		}

		if (view.TotalValue > 0)
		{
			view.GroundRange = rangeWeight > 0 ? static_cast<int>(rangeSum / rangeWeight) : 0;
			view.ArtillerySpeed = artilleryValue > 0 ? static_cast<int>(speedSum / artilleryValue) : 0;
			view.VehicleShare = vehicleValue * 100 / view.TotalValue;
			view.ArtilleryShare = artilleryValue * 100 / view.TotalValue;
		}

		return view;
	}

	// The weapons a type fights with: primary and secondary, or for a carrier the weapons of the
	// aircraft it launches (the launcher weapon itself deals no real damage). Healing and
	// damage-less weapons do not count.
	int CollectCounterWeapons(TechnoTypeClass* pType, std::array<WeaponTypeClass*, 4>& out)
	{
		int count = 0;

		auto add = [&](WeaponTypeClass* pWeapon)
		{
			if (pWeapon && pWeapon->Warhead && pWeapon->Damage > 0 && count < 4)
				out[count++] = pWeapon;
		};

		for (int i = 0; i < 2; ++i)
		{
			auto const pWS = pType->GetWeapon(i);
			auto const pWeapon = pWS ? pWS->WeaponType : nullptr;

			if (!pWeapon)
				continue;

			if (pWeapon->Spawner && pType->Spawns)
			{
				for (int j = 0; j < 2; ++j)
				{
					auto const pSpawnWS = pType->Spawns->GetWeapon(j);
					add(pSpawnWS ? pSpawnWS->WeaponType : nullptr);
				}

				continue;
			}

			add(pWeapon);
		}

		return count;
	}

	bool CounterWeaponHits(const WeaponTypeClass* pWeapon, EnemyForceCategory category)
	{
		auto const pProjectile = pWeapon->Projectile;

		if (category == EnemyForceCategory::Air)
			return pProjectile && pProjectile->AA;

		return !pProjectile || pProjectile->AG;
	}

	// Which production queue a type competes in as a combat unit, or -1 when it is no combat
	// candidate (economy, deployers, engineers, spies, unarmed support).
	int CounterQueueOf(TechnoTypeClass* pType)
	{
		if (!pType)
			return -1;

		std::array<WeaponTypeClass*, 4> weapons {};

		if (CollectCounterWeapons(pType, weapons) == 0)
			return -1;

		switch (pType->WhatAmI())
		{
		case AbstractType::InfantryType:
		{
			auto const pInf = static_cast<InfantryTypeClass*>(pType);

			if (pInf->Engineer || IsSpyInfantryType(pInf))
				return -1;

			return static_cast<int>(NeuralCounterQueue::Infantry);
		}
		case AbstractType::UnitType:
		{
			auto const pUnit = static_cast<UnitTypeClass*>(pType);

			if (pUnit->Harvester || pUnit->ResourceGatherer || pUnit->DeploysInto)
				return -1;

			return static_cast<int>(pUnit->Naval ? NeuralCounterQueue::Naval : NeuralCounterQueue::Ground);
		}
		case AbstractType::AircraftType:
			return pType->Spawned ? -1 : static_cast<int>(NeuralCounterQueue::Air);
		case AbstractType::BuildingType:
		{
			auto const pBld = static_cast<BuildingTypeClass*>(pType);

			if (!pBld->IsBaseDefense || IsTunnelNetworkBuilding(pBld))
				return -1;

			return static_cast<int>(NeuralCounterQueue::Defense);
		}
		default:
			return -1;
		}
	}

	// How well a type answers the seen enemy army, in percent: the value-weighted best Verses of
	// its weapons against each seen armor (0 where none of its weapons can hit that target), with
	// area damage worth more against infantry, one-shot weapons worth half, and land units worth
	// half against ships they can only reach from the shore. Outranging the enemy guns while it fields armor, and speed with a
	// ground weapon against artillery (flankers and air strikes hunt it), add to it.
	int CounterFitOf(TechnoTypeClass* pType, const NeuralCounterView& view)
	{
		std::array<WeaponTypeClass*, 4> weapons {};
		const int count = CollectCounterWeapons(pType, weapons);

		if (count == 0 || view.TotalValue <= 0)
			return 0;

		const bool isAircraft = pType->WhatAmI() == AbstractType::AircraftType;
		const bool isLandBound = !isAircraft && !pType->Naval && pType->WhatAmI() != AbstractType::BuildingType
			&& pType->MovementZone != MovementZone::Amphibious && pType->MovementZone != MovementZone::AmphibiousDestroyer
			&& pType->MovementZone != MovementZone::AmphibiousCrusher && pType->MovementZone != MovementZone::Fly
			&& !pType->JumpJet;
		int groundRange = 0;

		for (int w = 0; w < count; ++w)
		{
			if (CounterWeaponHits(weapons[w], EnemyForceCategory::Vehicle))
				groundRange = std::max(groundRange, weapons[w]->Range);
		}

		long long sum = 0;

		for (auto const& target : view.Targets)
		{
			int best = 0;

			for (int w = 0; w < count; ++w)
			{
				auto const pWeapon = weapons[w];

				if (!CounterWeaponHits(pWeapon, target.Category))
					continue;

				const double verses = GeneralUtils::GetWarheadVersusArmor(pWeapon->Warhead, static_cast<Armor>(target.ArmorIndex));
				int pct = std::clamp(static_cast<int>(verses * 100.0 + 0.5), 0, 200);

				if (target.Category == EnemyForceCategory::Infantry)
					pct = pct * (100 + std::clamp(static_cast<int>(pWeapon->Warhead->CellSpread * 35.0f), 0, 70)) / 100;

				// A one-shot weapon (suicide charge, planted bomb) is spent on one target, not an army.
				if (pWeapon->Suicide || pWeapon->Warhead->IvanBomb)
					pct /= 2;

				best = std::max(best, pct);
			}

			if (target.Category == EnemyForceCategory::Naval && isLandBound)
				best /= 2;

			sum += static_cast<long long>(best) * target.Value;
		}

		int fit = static_cast<int>(sum / view.TotalValue);

		if (view.GroundRange > 0 && groundRange >= view.GroundRange + 2 * Unsorted::LeptonsPerCell)
			fit += view.VehicleShare * 30 / 100;

		if (view.ArtilleryShare >= 10 && groundRange > 0
			&& (isAircraft || (pType->WhatAmI() != AbstractType::BuildingType && pType->Speed >= std::max(6, view.ArtillerySpeed + 2))))
		{
			fit += view.ArtilleryShare / 2;
		}

		return fit;
	}

	// Every 300 frames, for every expert (countering the threat is not one expert's business; the
	// counter strategist, Expert 2, takes all of the shift, the others CounterShareOtherExperts
	// percent in peace and all of it under attack): scores every buildable combat type of each queue
	// against the enemy army seen so far. A type gets +12 points per fit point above the mean of its
	// queue (defenses +8), so the best counter of a factory leads and a type that cannot hurt what
	// the enemy fields falls behind; scaled by the opponent model confidence, so a few early
	// sightings barely move it. A missing building that would unlock a type fitting at least 10
	// points better than any combat type we can build now (defenses: than any defense) gets 15
	// points per fit point gained (up to 900) on top of its tech unlock priority. Respects the
	// progressive tier: types above it are neither scored nor worth unlocking yet.
	void UpdateCounterComposition(HouseClass* pHouse)
	{
		if (!pHouse || !HouseExt::Fetch(pHouse))
			return;

		auto& macro = GetMacroState(pHouse);
		const int now = Unsorted::CurrentFrame;

		if (now - macro.CounterFrame < 300 && now >= macro.CounterFrame)
			return;

		macro.CounterFrame = now;
		macro.CounterBonus.clear();
		macro.CounterUnlockBonus.clear();

		const NeuralCounterView view = BuildCounterView(pHouse);
		macro.CounterUnderAttack = view.UnderAttack;
		// What attacks the base is certain, however little of the enemy army was seen.
		const int confidence = view.UnderAttack ? 100 : macro.OpponentProfile.Confidence();

		if (confidence <= 0 || view.TotalValue <= 0)
			return;

		struct Candidate
		{
			const TechnoTypeClass* Type;
			int Queue;
			int Fit;
		};

		constexpr int queueCount = static_cast<int>(NeuralCounterQueue::Count);
		constexpr int defenseQueue = static_cast<int>(NeuralCounterQueue::Defense);
		std::vector<Candidate> candidates;
		std::array<int, queueCount> fitSum {};
		std::array<int, queueCount> fitCount {};
		std::array<int, queueCount> fitMax {};

		auto consider = [&](TechnoTypeClass* pType)
		{
			const int queue = CounterQueueOf(pType);

			if (queue < 0 || (queue != defenseQueue && !IsUnitTierAllowed(pHouse, pType)) || !CanBuildUnderHumanRules(pHouse, pType))
				return;

			const int fit = CounterFitOf(pType, view);
			candidates.push_back({ pType, queue, fit });
			fitSum[queue] += fit;
			fitCount[queue]++;
			fitMax[queue] = std::max(fitMax[queue], fit);
		};

		for (auto const pType : InfantryTypeClass::Array)
			consider(pType);

		for (auto const pType : UnitTypeClass::Array)
			consider(pType);

		for (auto const pType : AircraftTypeClass::Array)
			consider(pType);

		for (auto const pType : BuildingTypeClass::Array)
		{
			if (pType && pType->IsBaseDefense)
				consider(pType);
		}

		for (auto const& candidate : candidates)
		{
			if (fitCount[candidate.Queue] < 2)
				continue;

			const int mean = fitSum[candidate.Queue] / fitCount[candidate.Queue];
			const bool isDefense = candidate.Queue == defenseQueue;
			const int bonus = std::clamp((candidate.Fit - mean) * (isDefense ? 8 : 12), isDefense ? -400 : -600, isDefense ? 600 : 900)
				* confidence / 100;

			if (bonus != 0)
				macro.CounterBonus[candidate.Type] = bonus;
		}

		// Unlocks are measured against what we can field now; with nothing buildable in a group yet
		// (no factory) there is nothing to beat, and the plain tech unlock priority decides.
		int bestCombatFit = 0;
		int combatCandidates = 0;

		for (int q = 0; q < queueCount; ++q)
		{
			if (q != defenseQueue)
			{
				bestCombatFit = std::max(bestCombatFit, fitMax[q]);
				combatCandidates += fitCount[q];
			}
		}

		for (auto const& [buildingIdx, types] : macro.TechUnlockTypes)
		{
			if (buildingIdx < 0 || buildingIdx >= BuildingTypeClass::Array.Count)
				continue;

			int best = 0;

			for (auto const pType : types)
			{
				const int queue = CounterQueueOf(pType);

				if (queue < 0 || (queue != defenseQueue && !IsUnitTierAllowed(pHouse, pType))
					|| (queue == defenseQueue ? fitCount[defenseQueue] : combatCandidates) == 0)
				{
					continue;
				}

				const int gain = CounterFitOf(pType, view) - (queue == defenseQueue ? fitMax[defenseQueue] : bestCombatFit);

				if (gain >= 10)
					best = std::max(best, std::min(900, gain * 15));
			}

			if (best > 0)
				macro.CounterUnlockBonus[buildingIdx] = best * confidence / 100;
		}
	}

	// Production score shift of a type from the counter composition. All of it for Expert 2, for the
	// defenses of Expert 7, for the aircraft of Expert 8 and for everyone while the base is under
	// attack; CounterShareOtherExperts percent otherwise.
	int CounterBonusFor(HouseClass* pHouse, const TechnoTypeClass* pType)
	{
		auto const pExt = pHouse ? HouseExt::Fetch(pHouse) : nullptr;

		if (!pExt || !pType)
			return 0;

		auto const& macro = GetMacroState(pHouse);
		auto const it = macro.CounterBonus.find(pType);

		if (it == macro.CounterBonus.end())
			return 0;

		const int expert = pExt->NeuralAI_ActiveExpert;
		auto const pBldType = abstract_cast<const BuildingTypeClass*>(pType);
		const bool ownField = expert == 2 || (expert == 7 && pBldType && pBldType->IsBaseDefense)
			|| (expert == 8 && pType->WhatAmI() == AbstractType::AircraftType);

		return ownField || macro.CounterUnderAttack ? it->second : it->second * CounterShareOtherExperts / 100;
	}

	// Extra tech priority of a missing building that unlocks a better counter: for Expert 2, and for
	// everyone while the base is under attack (half of the houses raided from the air that never got
	// an anti-air gun had none they could build yet).
	int CounterUnlockBonusFor(HouseClass* pHouse, const BuildingTypeClass* pBld)
	{
		auto const pExt = pHouse ? HouseExt::Fetch(pHouse) : nullptr;

		if (!pExt || !pBld || (pExt->NeuralAI_ActiveExpert != 2 && !GetMacroState(pHouse).CounterUnderAttack))
			return 0;

		auto const& bonus = GetMacroState(pHouse).CounterUnlockBonus;
		auto const it = bonus.find(pBld->ArrayIndex);

		return it != bonus.end() ? it->second : 0;
	}
}

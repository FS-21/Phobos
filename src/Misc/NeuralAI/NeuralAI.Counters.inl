// NeuralAI.Counters.inl: Counter composition: which buildable types beat the enemy army seen so far and what attacks the base now.
// Part of NeuralAI.cpp (included in order; one translation unit).

namespace
{
	// One slice of the seen enemy army: an armor type (and optional Phobos shield type) in one force category, by remembered value.
	struct NeuralCounterTarget
	{
		int ArmorIndex { 0 };
		EnemyForceCategory Category { EnemyForceCategory::Vehicle };
		int Value { 0 };
		bool AttackingNow { false };
		bool Submerged { false };
		bool Organic { false };
		bool FastClose { false };
		int ShieldArmorIndex { -1 };
		const TechnoTypeClass* SampleType { nullptr };
	};

	// The seen enemy army as counter targets (fog-fair: built from the sightings memory only).
	struct NeuralCounterView
	{
		std::vector<NeuralCounterTarget> Targets {};
		int TotalValue { 0 };
		int GroundRange { 0 };
		int ArtillerySpeed { 0 };
		int VehicleShare { 0 };
		int ArtilleryShare { 0 };
		int PsionicShare { 0 };
		std::vector<int> EnemyThreatVsArmor {};
		int MeanEnemyThreat { 0 };
		std::array<int16_t, 4> RoleScores {};
		bool UnderAttack { false };
		bool UnderGroundAttack { false };
	};

	// What hit our buildings within CounterThreatNowFrames counts CounterThreatNowWeight times in
	// the view: the answer to the threat at the door comes before the answer to an army only seen.
	// Experts other than the counter strategist take CounterShareOtherExperts percent of the shift
	// in peace and all of it under attack.
	constexpr int CounterThreatNowFrames = 1800;
	constexpr int CounterThreatNowWeight = 3;
	constexpr int CounterShareOtherExperts = 50;
	// Most a unit gains from being the counter, and under attack (user approved, 2026-11-20, «de tus 5
	// propuestas hazlas todas»; match #12322: Nod's anti-air vehicle won the pick 2 times in 20 against
	// the learned artillery slot, 2900 to 3750 against 600 to 1830, while missiles and 296 soldiers
	// came at it).
	constexpr int CounterUnitBonusMost = 900;
	constexpr int CounterUnitBonusUnderAttack = 1800;
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
		auto const& macro = GetMacroState(pHouse);
		view.RoleScores = macro.CounterRoleScores;
		const int armorCount = GetRegisteredArmorCount();
		view.EnemyThreatVsArmor.assign(armorCount, 0);
		long long rangeSum = 0;
		long long rangeWeight = 0;
		long long speedSum = 0;
		int artilleryValue = 0;
		int vehicleValue = 0;
		int psionicValue = 0;
		std::vector<long long> armorThreatSum(armorCount, 0);
		long long armorThreatWeight = 0;

		auto shieldIndexOf = [](const TechnoTypeClass* pTargetType) -> int
		{
			Armor shieldArmor = Armor::None;
			int shieldStrength = 0;

			if (GetActiveTypeShield(pTargetType, shieldArmor, shieldStrength) && shieldStrength > 0)
				return static_cast<int>(shieldArmor);

			return -1;
		};

		auto recordEnemyWeaponThreat = [&](const TechnoTypeClass* pEnemyType, int weight)
		{
			if (!pEnemyType || weight <= 0)
				return;

			auto const pWS = const_cast<TechnoTypeClass*>(pEnemyType)->GetWeapon(0);
			auto const pWpn = pWS ? pWS->WeaponType : nullptr;

			if (!pWpn || !pWpn->Warhead || pWpn->Damage <= 0 || !CanWarheadDamageEnemy(pWpn->Warhead))
				return;

			if (pWpn->Warhead->MindControl || pWpn->Warhead->PsychicDamage)
				psionicValue += weight;

			for (int a = 0; a < armorCount; ++a)
			{
				const double rawVerses = GeneralUtils::GetWarheadVersusArmor(pWpn->Warhead, static_cast<Armor>(a));
				const WarheadFlags flags = GetWarheadVersusFlags(pWpn->Warhead, static_cast<Armor>(a));
				const int pct = (rawVerses > 0.0 && (flags.ForceFire || rawVerses > 0.0001))
					? std::clamp(static_cast<int>(rawVerses * 100.0 + 0.5), 0, 220)
					: 0;
				armorThreatSum[a] += static_cast<long long>(pct) * weight;
			}

			armorThreatWeight += weight;
		};

		for (auto const& [uid, sighting] : macro.EnemySightings)
		{
			if (!sighting.Type)
				continue;

			// Same decay as the opponent profile, so older sightings count less.
			const int age = std::clamp(Unsorted::CurrentFrame - sighting.LastSeenFrame, 0, EnemySightingMemoryFrames);
			const int weighted = sighting.Value * (EnemySightingMemoryFrames - age) / EnemySightingMemoryFrames;

			if (weighted <= 0)
				continue;

			const int armorIndex = static_cast<int>(sighting.Type->Armor);
			const int shieldIndex = shieldIndexOf(sighting.Type);
			const bool submerged = sighting.Category == EnemyForceCategory::Naval && sighting.Type->Underwater;
			const bool organic = sighting.Type->Organic || sighting.Category == EnemyForceCategory::Infantry;
			auto const pSightWS = const_cast<TechnoTypeClass*>(sighting.Type)->GetWeapon(0);
			auto const pSightWpn = pSightWS ? pSightWS->WeaponType : nullptr;
			const bool fastClose = sighting.Category != EnemyForceCategory::Air
				&& sighting.Type->Speed >= 7 && (!pSightWpn || pSightWpn->Range <= 5 * Unsorted::LeptonsPerCell);

			auto const it = std::find_if(view.Targets.begin(), view.Targets.end(),
				[&](const NeuralCounterTarget& target)
				{
					return target.ArmorIndex == armorIndex && target.ShieldArmorIndex == shieldIndex
						&& target.Category == sighting.Category && target.Submerged == submerged && target.Organic == organic;
				});

			if (it != view.Targets.end())
			{
				it->Value += weighted;
				it->FastClose = it->FastClose || fastClose;
			}
			else
			{
				view.Targets.push_back({ armorIndex, sighting.Category, weighted, false, submerged, organic, fastClose,
					shieldIndex, sighting.Type });
			}

			view.TotalValue += weighted;
			recordEnemyWeaponThreat(sighting.Type, weighted);

			// A launcher of aircraft also asks for what shoots its spawns down (AircraftSpawnOf); the
			// launcher itself stays in the view as what it is, to be beaten on its own ground or sea.
			if (auto const pSpawns = sighting.Category != EnemyForceCategory::Air ? AircraftSpawnOf(sighting.Type) : nullptr)
			{
				const int spawnWeighted = AircraftSpawnThreatOf(sighting.Type) * (EnemySightingMemoryFrames - age) / EnemySightingMemoryFrames;
				const int spawnArmor = static_cast<int>(pSpawns->Armor);
				const int spawnShield = shieldIndexOf(pSpawns);

				if (spawnWeighted > 0)
				{
					auto const itAir = std::find_if(view.Targets.begin(), view.Targets.end(),
						[&](const NeuralCounterTarget& target)
						{
							return target.ArmorIndex == spawnArmor && target.ShieldArmorIndex == spawnShield
								&& target.Category == EnemyForceCategory::Air;
						});

					if (itAir != view.Targets.end())
					{
						itAir->Value += spawnWeighted;
					}
					else
					{
						view.Targets.push_back({ spawnArmor, EnemyForceCategory::Air, spawnWeighted, false, false, false, false,
							spawnShield, pSpawns });
					}

					view.TotalValue += spawnWeighted;
				}
			}

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
			const int shieldIndex = shieldIndexOf(rec.AttackerType);
			const bool byAir = rec.Threat == NeuralThreatCategory::Air;
			view.UnderGroundAttack = view.UnderGroundAttack || !byAir;
			const bool byInfantry = rec.Threat == NeuralThreatCategory::Infantry;
			bool found = false;

			for (auto& target : view.Targets)
			{
				const bool sameKind = byAir ? target.Category == EnemyForceCategory::Air
					: byInfantry ? target.Category == EnemyForceCategory::Infantry
					: (target.Category != EnemyForceCategory::Air && target.Category != EnemyForceCategory::Infantry);

				if (target.ArmorIndex != armorIndex || target.ShieldArmorIndex != shieldIndex || !sameKind)
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
				const bool submerged = rec.AttackerType->Naval && rec.AttackerType->Underwater;
				const bool organic = rec.AttackerType->Organic || byInfantry;
				view.Targets.push_back({ armorIndex, category, value, true, submerged, organic, rec.AttackerType->Speed >= 7,
					shieldIndex, rec.AttackerType });
				view.TotalValue += value;
				recordEnemyWeaponThreat(rec.AttackerType, value);

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
			view.PsionicShare = std::clamp(psionicValue * 100 / view.TotalValue, 0, 100);

			if (armorThreatWeight > 0)
			{
				long long sumThreat = 0;
				int activeArmors = 0;

				for (int a = 0; a < armorCount; ++a)
				{
					view.EnemyThreatVsArmor[a] = static_cast<int>(armorThreatSum[a] / armorThreatWeight);

					if (a < 9 || (GetArmorDomainMask(a) & (ArmorDomainInfantry | ArmorDomainVehicle | ArmorDomainShield)) != 0)
					{
						sumThreat += view.EnemyThreatVsArmor[a];
						activeArmors++;
					}
				}

				view.MeanEnemyThreat = activeArmors > 0 ? static_cast<int>(sumThreat / activeArmors) : 0;
			}
		}

		return view;
	}

	struct CounterWeaponSlot
	{
		WeaponTypeClass* Weapon { nullptr };
		const TechnoTypeClass* Platform { nullptr };
		int Slot { 0 };
	};

	// The weapons a type fights with: primary and secondary, or for a carrier the weapons of the
	// aircraft it launches (the launcher weapon itself deals no real damage). Healing, ally-only,
	// fake-engineer capture and damage-less weapons do not count.
	int CollectCounterWeapons(TechnoTypeClass* pType, std::array<CounterWeaponSlot, 4>& out)
	{
		int count = 0;

		auto add = [&](WeaponTypeClass* pWeapon, const TechnoTypeClass* pPlatform, int slot)
		{
			if (pWeapon && !pWeapon->NeverUse && pWeapon->Warhead && pWeapon->Damage > 0
				&& CanWarheadDamageEnemy(pWeapon->Warhead) && count < 4)
			{
				out[count++] = { pWeapon, pPlatform, slot };
			}
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
					add(pSpawnWS ? pSpawnWS->WeaponType : nullptr, pType->Spawns, j);
				}

				continue;
			}

			add(pWeapon, pType, i);
		}

		return count;
	}

	bool CounterWeaponHits(const CounterWeaponSlot& cw, const NeuralCounterTarget& target)
	{
		auto const pWeapon = cw.Weapon;
		auto const pPlatform = cw.Platform;
		auto const pProjectile = pWeapon->Projectile;

		if (target.Category == EnemyForceCategory::Air)
			return pProjectile && pProjectile->AA;

		if (pProjectile && !pProjectile->AG)
			return false;

		if (!pPlatform)
			return true;

		if (target.Category == EnemyForceCategory::Naval)
		{
			const NavalTargetingType nt = pPlatform->NavalTargeting;

			if (nt == NavalTargetingType::Naval_None)
				return false;

			if (target.Submerged)
			{
				if (nt == NavalTargetingType::Underwater_Never)
					return false;

				if (nt == NavalTargetingType::Organic_Secondary && !target.Organic)
					return false;

				if (nt == NavalTargetingType::Underwater_Secondary && cw.Slot == 0)
					return false;

				if (nt == NavalTargetingType::Naval_Primary && cw.Slot != 0)
					return false;

				return true;
			}

			return nt != NavalTargetingType::Underwater_Only;
		}

		return LandTargetingAllows(pPlatform, cw.Slot);
	}

	// Which production queue a type competes in as a combat unit, or -1 when it is no combat
	// candidate (economy, deployers, engineers, spies, unarmed support).
	int CounterQueueOf(TechnoTypeClass* pType)
	{
		if (!pType)
			return -1;

		std::array<CounterWeaponSlot, 4> weapons {};

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
	// its weapons against each seen custom armor and Phobos shield (0 where none of its weapons can
	// hit that target), with area damage worth more against infantry, one-shot weapons worth half,
	// and land units worth half against ships they can only reach from the shore.
	int CounterFitOf(TechnoTypeClass* pType, const NeuralCounterView& view)
	{
		std::array<CounterWeaponSlot, 4> weapons {};
		const int count = CollectCounterWeapons(pType, weapons);

		if (count == 0 || view.TotalValue <= 0)
			return 0;

		const bool isAircraft = pType->WhatAmI() == AbstractType::AircraftType;
		const bool isBuilding = pType->WhatAmI() == AbstractType::BuildingType;
		const bool isLandBound = !isAircraft && !pType->Naval && !isBuilding
			&& pType->MovementZone != MovementZone::Amphibious && pType->MovementZone != MovementZone::AmphibiousDestroyer
			&& pType->MovementZone != MovementZone::AmphibiousCrusher && pType->MovementZone != MovementZone::Fly
			&& !pType->JumpJet;
		int groundRange = 0;
		const NeuralCounterTarget groundProbe { 0, EnemyForceCategory::Vehicle, 1, false, false, false, false, -1, nullptr };

		for (int w = 0; w < count; ++w)
		{
			if (CounterWeaponHits(weapons[w], groundProbe))
				groundRange = std::max(groundRange, weapons[w].Weapon->Range);
		}

		const int costNorm = std::clamp(pType->Cost, 150, 3200);
		long long sum = 0;

		for (auto const& target : view.Targets)
		{
			int best = 0;

			for (int w = 0; w < count; ++w)
			{
				if (!CounterWeaponHits(weapons[w], target))
					continue;

				auto const pWeapon = weapons[w].Weapon;
				auto const pWH = pWeapon->Warhead;
				int pct = 0;

				if (target.SampleType)
				{
					pct = std::clamp((GetEffectiveVersusAgainstTypePermille(pWH, target.SampleType) + 5) / 10, 0, 250);
				}
				else
				{
					const Armor tgtArmor = static_cast<Armor>(target.ArmorIndex);
					const double verses = GeneralUtils::GetWarheadVersusArmor(pWH, tgtArmor);
					const WarheadFlags flags = GetWarheadVersusFlags(pWH, tgtArmor);

					if (verses > 0.0 && (flags.ForceFire || verses > 0.0001))
					{
						pct = std::clamp(static_cast<int>(verses * 100.0 + 0.5), 0, 220);

						if (!flags.PassiveAcquire)
							pct = pct * 55 / 100;
					}
				}

				if (pct <= 0)
					continue;

				// Weight Verses by real DPS efficiency (Damage * Burst / ROF normalized against Cost) so a
				// weak low-DPS gun with 100% Verses never beats a true high-DPS counter.
				const int burst = std::clamp(pWeapon->Burst, 1, 12);
				const int rof = std::max(10, pWeapon->ROF);
				const int rawDps = pWeapon->Damage * burst * 60 / rof;
				const int dpsPerCost = rawDps * 100 / std::max(20, costNorm / 10);
				const int dpsScale = std::clamp(65 + dpsPerCost * 2 / 5, 65, 175);
				pct = pct * dpsScale / 110;

				if (target.Category == EnemyForceCategory::Infantry)
					pct = pct * (100 + std::clamp(static_cast<int>(pWH->CellSpread * 40.0f), 0, 80)) / 100;

				if (pWH->EMEffect && (target.Category == EnemyForceCategory::Vehicle || target.Category == EnemyForceCategory::Artillery
					|| target.Category == EnemyForceCategory::Naval))
				{
					pct = pct * 135 / 100;
				}

				if (pWH->MindControl || pWH->Temporal)
				{
					pct = (target.Category == EnemyForceCategory::Infantry) ? (pct * 70 / 100) : (pct * 130 / 100);
				}

				if (pWeapon->MinimumRange >= 2 * Unsorted::LeptonsPerCell && target.FastClose)
					pct = pct * 70 / 100;

				if (pWeapon->Suicide || pWH->IvanBomb)
					pct /= 2;

				best = std::max(best, pct);
			}

			if (target.Category == EnemyForceCategory::Naval && isLandBound)
				best = target.Submerged ? 0 : (best / 2);

			sum += static_cast<long long>(best) * target.Value;
		}

		int fit = static_cast<int>(sum / view.TotalValue);

		// Survivability across all custom [ArmorTypes] and [ShieldTypes]: prefer units whose hull and
		// shield armors resist what the enemy shoots, and penalize fragile units unless they outrange it.
		const int ownArmor = static_cast<int>(pType->Armor);
		Armor ownShieldArmor = pType->Armor;
		int ownShieldStrength = 0;
		GetActiveTypeShield(pType, ownShieldArmor, ownShieldStrength);
		const bool outrangesEnemy = view.GroundRange > 0 && groundRange >= view.GroundRange + 2 * Unsorted::LeptonsPerCell;

		if (view.MeanEnemyThreat > 0 && ownArmor >= 0 && ownArmor < static_cast<int>(view.EnemyThreatVsArmor.size()))
		{
			int incoming = view.EnemyThreatVsArmor[ownArmor];
			const int ownShieldIdx = static_cast<int>(ownShieldArmor);

			if (ownShieldStrength > 0 && ownShieldIdx >= 0 && ownShieldIdx < static_cast<int>(view.EnemyThreatVsArmor.size()))
			{
				const int hullHP = std::max(1, pType->Strength);
				incoming = static_cast<int>((static_cast<long long>(incoming) * hullHP
					+ static_cast<long long>(view.EnemyThreatVsArmor[ownShieldIdx]) * ownShieldStrength)
					/ (hullHP + ownShieldStrength));
			}

			if (incoming < view.MeanEnemyThreat)
				fit += std::min(22, (view.MeanEnemyThreat - incoming) / 3);
			else if (!outrangesEnemy && !isAircraft)
				fit -= std::min(24, (incoming - view.MeanEnemyThreat) / 3);
		}

		if (view.PsionicShare > 0 && (pType->ImmuneToPsionics || pType->ImmuneToPsionicWeapons))
			fit += view.PsionicShare * 45 / 100;

		if (outrangesEnemy)
			fit += view.VehicleShare * 35 / 100 + std::clamp(static_cast<int>(view.RoleScores[2]) / 30, -8, 14);

		if (view.ArtilleryShare >= 10 && groundRange > 0
			&& (isAircraft || (!isBuilding && pType->Speed >= std::max(6, view.ArtillerySpeed + 2))))
		{
			fit += view.ArtilleryShare / 2 + std::clamp(static_cast<int>(view.RoleScores[0]) / 25, -8, 16);
		}

		if (groundRange >= 7 * Unsorted::LeptonsPerCell)
			fit += std::clamp(static_cast<int>(view.RoleScores[1]) / 30, -8, 14);

		return std::max(0, fit);
	}

	// One enemy force as counter targets (Counter Squad): the units of one house and one force
	// category seen within radius of a cell no longer than maxAge ago, by value, with the mean
	// speed of its artillery (fast units hunt it down) and no range term (a squad goes at the force,
	// it does not stand off from it).
	NeuralCounterView BuildForceCounterView(HouseClass* pHouse, int owner, EnemyForceCategory category, const CellStruct& center,
		double radius, int maxAge)
	{
		NeuralCounterView view {};
		long long speedSum = 0;

		for (auto const& [uid, sighting] : GetMacroState(pHouse).EnemySightings)
		{
			if (!sighting.Type || sighting.OwnerIndex != owner || sighting.Category != category || sighting.Value <= 0
				|| Unsorted::CurrentFrame - sighting.LastSeenFrame > maxAge || sighting.LastCoords.DistanceFrom(center) > radius)
			{
				continue;
			}

			const int armorIndex = static_cast<int>(sighting.Type->Armor);
			Armor sArmor = Armor::None;
			int sStrength = 0;
			const int shieldIndex = (GetActiveTypeShield(sighting.Type, sArmor, sStrength) && sStrength > 0)
				? static_cast<int>(sArmor)
				: -1;
			const bool submerged = category == EnemyForceCategory::Naval && sighting.Type->Underwater;
			const bool organic = sighting.Type->Organic || category == EnemyForceCategory::Infantry;
			auto const it = std::find_if(view.Targets.begin(), view.Targets.end(),
				[armorIndex, shieldIndex, submerged, organic](const NeuralCounterTarget& target)
				{
					return target.ArmorIndex == armorIndex && target.ShieldArmorIndex == shieldIndex
						&& target.Submerged == submerged && target.Organic == organic;
				});

			if (it != view.Targets.end())
			{
				it->Value += sighting.Value;
			}
			else
			{
				view.Targets.push_back({ armorIndex, category, sighting.Value, false, submerged, organic,
					sighting.Type->Speed >= 7, shieldIndex, sighting.Type });
			}

			view.TotalValue += sighting.Value;
			speedSum += static_cast<long long>(sighting.Type->Speed) * sighting.Value;
		}

		if (view.TotalValue > 0 && category == EnemyForceCategory::Artillery)
		{
			view.ArtilleryShare = 100;
			view.ArtillerySpeed = static_cast<int>(speedSum / view.TotalValue);
		}

		return view;
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
		macro.CounterUnderGroundAttack = view.UnderGroundAttack;
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
			const int bonus = std::clamp((candidate.Fit - mean) * (isDefense ? 8 : 12), isDefense ? -400 : -600,
				isDefense ? 600 : (view.UnderAttack ? CounterUnitBonusUnderAttack : CounterUnitBonusMost))
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

// NeuralAI.Intel.inl: Scouting, threat and air threat tracking, opponent model, zones and placement safety.
// Part of NeuralAI.cpp (included in order; one translation unit).

namespace
{
	CellStruct FindNextStartLocationToScout(HouseClass* pHouse, const CellStruct& fromCell)
	{
		if (!pHouse || !ScenarioClass::Instance)
			return CellStruct::Empty;

		auto& macro = GetMacroState(pHouse);
		CellStruct best = CellStruct::Empty;
		double bestDist = 1e9;

		for (int i = 0; i < MaxStartWaypoints; ++i)
		{
			if ((macro.ScoutedStartMask & (1u << i)) != 0 || !ScenarioClass::Instance->IsDefinedWaypoint(i))
				continue;

			const CellStruct startCell = ScenarioClass::Instance->GetWaypointCoords(i);

			if (startCell == CellStruct::Empty)
				continue;

			bool isFriendlyStart = false;

			for (auto const pOther : HouseClass::Array)
			{
				if (pOther && (pOther == pHouse || pHouse->IsAlliedWith(pOther)) && pOther->BaseSpawnCell != CellStruct::Empty
					&& pOther->BaseSpawnCell.DistanceFrom(startCell) <= 6.0)
				{
					isFriendlyStart = true;
					break;
				}
			}

			auto const pStartCell = MapClass::Instance.TryGetCellAt(startCell);
			bool seenEmpty = false;

			if (!isFriendlyStart && pStartCell && IsCellVisibleToAI(pHouse, pStartCell))
			{
				seenEmpty = true;

				for (auto const pBld : BuildingClass::Array)
				{
					if (pBld && !pBld->InLimbo && pBld->IsAlive && pBld->Owner && pBld->Owner != pHouse
						&& !pHouse->IsAlliedWith(pBld->Owner) && !pBld->Owner->IsNeutral()
						&& IsEnemyBuildingKnown(pHouse, pBld) && pBld->GetMapCoords().DistanceFrom(startCell) <= 12.0)
					{
						seenEmpty = false;
						break;
					}
				}
			}

			if (isFriendlyStart || seenEmpty)
			{
				macro.ScoutedStartMask |= (1u << i);
				continue;
			}

			const double dist = fromCell.DistanceFrom(startCell);

			if (dist < bestDist)
			{
				bestDist = dist;
				best = startCell;
			}
		}

		return best;
	}

	constexpr int AirThreatMemoryFrames = 900;
	constexpr int AirThreatTrackCapacity = 64;

	// Rough damage-per-minute of a weapon (Damage x Burst per ROF frames).
	int GetWeaponThreatRate(const WeaponTypeClass* pWeapon)
	{
		if (!pWeapon || pWeapon->Damage <= 0)
			return 0;

		return pWeapon->Damage * std::max(1, pWeapon->Burst) * 60 / std::max(1, pWeapon->ROF);
	}

	// Firepower of a weapon for the vehicle pick (Firepower*): damage per 60 frames, times its mean
	// Verses over the first FirepowerArmorCount armors, more with area damage, times its range.
	int WeaponFirepowerOf(WeaponTypeClass* pWeapon)
	{
		const int rate = GetWeaponThreatRate(pWeapon);

		if (rate <= 0 || !pWeapon->Warhead)
			return 0;

		int versesSum = 0;

		for (int armor = 0; armor < FirepowerArmorCount; ++armor)
		{
			const double verses = GeneralUtils::GetWarheadVersusArmor(pWeapon->Warhead, static_cast<Armor>(armor));
			versesSum += std::clamp(static_cast<int>(verses * 100.0 + 0.5), 0, 300);
		}

		const long long verses = versesSum / FirepowerArmorCount;
		const long long area = 100 + std::clamp(static_cast<int>(pWeapon->Warhead->CellSpread * 35.0f), 0, 70);
		const long long range = std::clamp(pWeapon->Range * 100 / (FirepowerRangeRefCells * Unsorted::LeptonsPerCell),
			FirepowerRangeMinPercent, FirepowerRangeMostPercent);

		return static_cast<int>(rate * verses * area * range / 1000000);
	}

	// The firepower bonus of a vehicle type in the pick: its best weapon (Firepower*).
	int VehicleFirepowerScore(TechnoTypeClass* pType)
	{
		int best = 0;

		for (int i = 0; i < 2; ++i)
		{
			auto const pWS = pType ? pType->GetWeapon(i) : nullptr;
			best = std::max(best, pWS ? WeaponFirepowerOf(pWS->WeaponType) : 0);
		}

		return std::min(FirepowerScoreMost, best * FirepowerScorePerPoint);
	}

	// A main battle tank: a ground vehicle the rules call an armored fighting vehicle (Category=AFV)
	// whose first weapon fights head on: it hits the ground, is no suicide (MainTankMinRangeCells),
	// parasite, mind control or berserk gas, and does not wither against heavy armor (MainTankHeavyVersus); a
	// weapon whose damage comes another way (Damage up to MainTankSpecialDamage: sonic waves, webs,
	// dummy weapons) counts as fit, and so does any gun of an open-topped one with passengers: they
	// fire from it (the Battle Fortress). Its passengers, its range or an anti-air projectile do not
	// make it a transport, artillery or anti-air (user: the Overlord, the Avatars, the Centurion, the
	// sonic tanks are tanks; the Gattling Tank is not).
	bool IsMainBattleTankType(TechnoTypeClass* pType)
	{
		if (!pType || pType->WhatAmI() != AbstractType::UnitType || pType->Naval || pType->Category != Category::AFV)
			return false;

		auto const pWS = pType->GetWeapon(0);
		auto const pWeapon = pWS ? pWS->WeaponType : nullptr;

		if (!pWeapon || !pWeapon->Warhead || pWeapon->Suicide || pWeapon->Warhead->Parasite || pWeapon->Warhead->MindControl
			|| pWeapon->Warhead->Psychedelic
			|| (pWeapon->Projectile && !pWeapon->Projectile->AG)
			|| pWeapon->Range <= static_cast<int>(MainTankMinRangeCells * Unsorted::LeptonsPerCell))
		{
			return false;
		}

		return pWeapon->Damage <= MainTankSpecialDamage || (pType->OpenTopped && pType->Passengers > 0)
			|| GeneralUtils::GetWarheadVersusArmor(pWeapon->Warhead, Armor::Heavy) >= MainTankHeavyVersus;
	}

	// Threat of a unit type weighted by cost, durability and firepower, so one
	// Kirov Airship weighs far more than a scout plane.
	int GetCombatValueOfType(const TechnoTypeClass* pType, bool requireAntiAir)
	{
		if (!pType)
			return 0;

		int rate = 0;

		for (int i = 0; i < 2; ++i)
		{
			// TechnoTypeClass::GetWeapon is not const-qualified in YRpp.
			auto const pWS = const_cast<TechnoTypeClass*>(pType)->GetWeapon(i);
			auto const pWeapon = pWS ? pWS->WeaponType : nullptr;

			if (!pWeapon || (requireAntiAir && (!pWeapon->Projectile || !pWeapon->Projectile->AA)))
				continue;

			rate = std::max(rate, GetWeaponThreatRate(pWeapon));
		}

		if (requireAntiAir && rate <= 0)
			return 0;

		return std::clamp(pType->Cost / 25 + pType->Strength / 25 + std::clamp(rate / 4, 0, 100), 4, 260);
	}

	bool IsAirborneThreatTechno(TechnoClass* pTechno)
	{
		if (!pTechno)
			return false;

		if (pTechno->WhatAmI() == AbstractType::Aircraft)
			return true;

		auto const pType = pTechno->GetTechnoType();

		return pType
			&& (pType->JumpJet || pType->MovementZone == MovementZone::Fly || pType->ConsideredAircraft
				|| pTechno->IsInAir());
	}

	// Fog-fair local balance around a unit: enemy combat value seen within radius in the last
	// 150 frames (opponent model) against our own armed units there. True when the enemy side
	// is clearly stronger (> 1.5x), i.e. this unit is losing the fight it is in.
	bool IsFootOutmatched(HouseClass* pHouse, FootClass* pFoot, double radius)
	{
		if (!pHouse || !pFoot)
			return false;

		const CellStruct center = pFoot->GetMapCoords();
		int enemyValue = 0;

		for (auto const& [uid, sighting] : GetMacroState(pHouse).EnemySightings)
		{
			if (Unsorted::CurrentFrame - sighting.LastSeenFrame <= 150 && sighting.LastCoords.DistanceFrom(center) <= radius)
				enemyValue += sighting.Value;
		}

		if (enemyValue <= 0)
			return false;

		int ownValue = 0;

		for (auto const pTech : TechnoClass::Array)
		{
			if (!pTech || pTech->Owner != pHouse || !pTech->IsAlive || pTech->InLimbo
				|| pTech->WhatAmI() == AbstractType::Building || pTech->GetMapCoords().DistanceFrom(center) > radius)
			{
				continue;
			}

			ownValue += GetCombatValueOfType(pTech->GetTechnoType(), false);
		}

		return enemyValue * 2 > ownValue * 3;
	}

	// Visible enemy mobile anti-air (vehicles, infantry, aircraft) within radius of a cell.
	int CountVisibleEnemyMobileAANear(HouseClass* pHouse, const CellStruct& cell, double radius)
	{
		int count = 0;

		for (auto const pTech : TechnoClass::Array)
		{
			if (!pTech || !pTech->IsAlive || pTech->InLimbo || !pTech->Owner || pTech->WhatAmI() == AbstractType::Building
				|| pTech->Owner == pHouse || pHouse->IsAlliedWith(pTech->Owner) || pTech->Owner->IsNeutral()
				|| pTech->GetMapCoords().DistanceFrom(cell) > radius || !IsCellVisibleToAI(pHouse, pTech->GetCell()))
			{
				continue;
			}

			if (GetCombatValueOfType(pTech->GetTechnoType(), true) > 0)
				count++;
		}

		return count;
	}

	// The aircraft a launcher sends (a weapon with Spawner=yes and an aircraft as Spawns: the
	// dreadnought's missiles, the carrier's planes, the V3 rocket), or null.
	AircraftTypeClass* AircraftSpawnOf(const TechnoTypeClass* pType)
	{
		auto const pSpawns = pType ? pType->Spawns : nullptr;

		if (!pSpawns)
			return nullptr;

		for (int i = 0; i < 2; ++i)
		{
			auto const pWS = const_cast<TechnoTypeClass*>(pType)->GetWeapon(i);

			if (pWS && pWS->WeaponType && pWS->WeaponType->Spawner)
				return pSpawns;
		}

		return nullptr;
	}

	// Air threat of a launcher (user, 2026-11-20: seeing a dreadnought means attacks from the air,
	// its weapons are aircraft, like the Allied carrier's): what it sends, SpawnsNumber times. It
	// asks for anti-air before the spawns arrive; the launcher itself stays a target of its own kind.
	int AircraftSpawnThreatOf(const TechnoTypeClass* pType)
	{
		auto const pSpawns = AircraftSpawnOf(pType);

		return pSpawns ? GetCombatValueOfType(pSpawns, false) * std::max(1, pType->SpawnsNumber) : 0;
	}

	void RecordAirThreatSighting(HouseClass* pHouse, TechnoClass* pTechno)
	{
		auto const pType = pTechno ? pTechno->GetTechnoType() : nullptr;

		if (!pHouse || !pType)
			return;

		auto& tracks = GetMacroState(pHouse).AirThreatTracks;
		const CellStruct coords = pTechno->GetMapCoords();

		for (auto& track : tracks)
		{
			if (track.UniqueID != pTechno->UniqueID)
				continue;

			if (track.LastSeenFrame != Unsorted::CurrentFrame)
			{
				track.PrevCoords = track.LastCoords;
				track.PrevSeenFrame = track.LastSeenFrame;
				track.LastCoords = coords;
				track.LastSeenFrame = Unsorted::CurrentFrame;
			}

			return;
		}

		if (tracks.size() >= AirThreatTrackCapacity)
		{
			auto const oldest = std::min_element(tracks.begin(), tracks.end(),
				[](const NeuralAirThreatTrack& a, const NeuralAirThreatTrack& b) { return a.LastSeenFrame < b.LastSeenFrame; });
			tracks.erase(oldest);
		}

		NeuralAirThreatTrack track {};
		track.UniqueID = pTechno->UniqueID;
		track.LastCoords = coords;
		track.LastSeenFrame = Unsorted::CurrentFrame;
		// A launcher on the ground or at sea counts as what it sends.
		const bool airborne = IsAirborneThreatTechno(pTechno);
		auto const pSpawns = airborne ? nullptr : AircraftSpawnOf(pType);
		track.ThreatValue = pSpawns ? AircraftSpawnThreatOf(pType) : GetCombatValueOfType(pType, false);
		track.IsHeavy = (pSpawns ? pSpawns->Strength : pType->Strength) >= 600;
		tracks.push_back(track);
	}

	// Extrapolates a track along its last observed heading.
	CellStruct PredictAirThreatPosition(const NeuralAirThreatTrack& track, int framesAhead)
	{
		if (track.PrevSeenFrame < 0 || track.PrevCoords == CellStruct::Empty)
			return track.LastCoords;

		const int dt = std::max(1, track.LastSeenFrame - track.PrevSeenFrame);
		const int ahead = std::clamp((Unsorted::CurrentFrame - track.LastSeenFrame) + framesAhead, 0, 1800);

		return CellStruct {
			static_cast<short>(track.LastCoords.X + (track.LastCoords.X - track.PrevCoords.X) * ahead / dt),
			static_cast<short>(track.LastCoords.Y + (track.LastCoords.Y - track.PrevCoords.Y) * ahead / dt)
		};
	}

	// Frames until the track flies within radiusCells of any of our buildings
	// following its current heading (-1 when not heading to our base in time).
	int EstimateAirThreatArrival(HouseClass* pHouse, const NeuralAirThreatTrack& track, double radiusCells,
		int maxFramesAhead)
	{
		if (!pHouse)
			return -1;

		for (int ahead = 0; ahead <= maxFramesAhead; ahead += 60)
		{
			const CellStruct predicted = PredictAirThreatPosition(track, ahead);

			for (auto const pOwnB : pHouse->Buildings)
			{
				if (pOwnB && !pOwnB->InLimbo && pOwnB->IsAlive
					&& predicted.DistanceFrom(pOwnB->GetMapCoords()) <= radiusCells)
				{
					return ahead;
				}
			}

			// A track seen only once has no heading to extrapolate.
			if (track.PrevSeenFrame < 0)
				break;
		}

		return -1;
	}

	// Ages the air threat memory and sums the remembered threat against our own
	// anti-air capacity. Only sightings feed the memory, so fog is respected.
	void RefreshAirThreatSummary(HouseClass* pHouse)
	{
		auto& macro = GetMacroState(pHouse);
		auto& tracks = macro.AirThreatTracks;

		std::erase_if(tracks, [](const NeuralAirThreatTrack& track)
			{ return (Unsorted::CurrentFrame - track.LastSeenFrame) > AirThreatMemoryFrames; });

		int threatValue = 0;
		int heavyThreats = 0;

		for (auto const& track : tracks)
		{
			const int age = Unsorted::CurrentFrame - track.LastSeenFrame;
			threatValue += track.ThreatValue * (AirThreatMemoryFrames - age) / AirThreatMemoryFrames;

			if (track.IsHeavy)
				heavyThreats++;
		}

		int ownAACapacity = 0;

		for (auto const pOwn : TechnoClass::Array)
		{
			if (!pOwn || pOwn->Owner != pHouse || pOwn->InLimbo || !pOwn->IsAlive)
				continue;

			ownAACapacity += GetCombatValueOfType(pOwn->GetTechnoType(), true);
		}

		macro.AirThreatValue = threatValue;
		macro.HeavyAirThreats = heavyThreats;
		macro.OwnAACapacity = ownAACapacity;

		// AI ticks spent under a serious air threat (telemetry for the trainer alarms).
		if (threatValue >= 120)
			CountDecision(NeuralDecision::AirThreatTick, pHouse->ArrayIndex);
	}

	constexpr int EnemySightingMemoryFrames = 2700; // ~3 minutes at 15 logic frames per second.

	EnemyForceCategory ClassifyEnemyForce(TechnoClass* pTechno)
	{
		auto const pType = pTechno ? pTechno->GetTechnoType() : nullptr;

		if (!pType)
			return EnemyForceCategory::Vehicle;

		if (pTechno->WhatAmI() == AbstractType::Aircraft || pType->JumpJet || pType->MovementZone == MovementZone::Fly
			|| pType->ConsideredAircraft)
		{
			return EnemyForceCategory::Air;
		}

		if (pType->Naval)
			return EnemyForceCategory::Naval;

		if (pTechno->WhatAmI() == AbstractType::Infantry)
			return EnemyForceCategory::Infantry;

		auto const pWS = pType->GetWeapon(0);
		auto const pWeapon = pWS ? pWS->WeaponType : nullptr;

		if (pWeapon && pWeapon->Range >= 8 * Unsorted::LeptonsPerCell && (!pWeapon->Projectile || pWeapon->Projectile->AG))
			return EnemyForceCategory::Artillery;

		return EnemyForceCategory::Vehicle;
	}

	void RecordEnemySighting(HouseClass* pHouse, TechnoClass* pTechno)
	{
		if (!pHouse || !pTechno || pTechno->WhatAmI() == AbstractType::Building)
			return;

		auto const pType = pTechno->GetTechnoType();

		// Harvesters and unarmed support units say little about the enemy's fighting style.
		if (!pType || GetCombatValueOfType(pType, false) <= 0)
			return;

		if (auto const pUnitType = abstract_cast<UnitTypeClass*>(pType))
		{
			if (pUnitType->Harvester || pUnitType->ResourceGatherer)
				return;
		}

		auto const pW0 = pType->GetWeapon(0);
		if (!pW0 || !pW0->WeaponType || pW0->WeaponType->Damage <= 0)
			return;

		const CellStruct coords = pTechno->GetMapCoords();
		auto& macro = GetMacroState(pHouse);

		// Where it was at its previous sighting, for the route heat (closing in or not).
		CellStruct prevCoords = CellStruct::Empty;
		int prevFrame = -1;

		if (auto const known = macro.EnemySightings.find(pTechno->UniqueID); known != macro.EnemySightings.end())
		{
			prevCoords = known->second.LastCoords;
			prevFrame = known->second.LastSeenFrame;
		}

		double distNow = 1.0e9;
		double distPrev = 1.0e9;

		for (auto const pOwnB : pHouse->Buildings)
		{
			if (!pOwnB || pOwnB->InLimbo || !pOwnB->IsAlive)
				continue;

			const CellStruct bc = pOwnB->GetMapCoords();
			distNow = std::min(distNow, coords.DistanceFrom(bc));

			if (prevCoords != CellStruct::Empty)
				distPrev = std::min(distPrev, prevCoords.DistanceFrom(bc));
		}

		const bool nearOurBase = distNow <= 18.0;
		auto& sighting = macro.EnemySightings[pTechno->UniqueID];
		sighting.Category = ClassifyEnemyForce(pTechno);
		sighting.Value = GetCombatValueOfType(pType, false);
		sighting.LastSeenFrame = Unsorted::CurrentFrame;
		sighting.AntiAir = GetCombatValueOfType(pType, true) > 0;
		sighting.OwnerIndex = pTechno->Owner ? pTechno->Owner->ArrayIndex : -1;
		sighting.Type = pType;

		// Attack route memory: the side this enemy unit reached our base from, aircraft apart.
		if (nearOurBase && !sighting.NearOurBase)
		{
			const int sector = AttackSectorOf(GetHouseHomeCell(pHouse), coords);

			if (sector >= 0)
			{
				if (sighting.Category == EnemyForceCategory::Air)
					macro.AirAttackSectorWeight[sector] += sighting.Value;
				else
					macro.AttackSectorWeight[sector] += sighting.Value;
			}
		}

		if (sighting.Category != EnemyForceCategory::Air && sighting.Category != EnemyForceCategory::Naval)
			RecordRouteHeatSighting(pHouse, pTechno, coords, prevCoords, prevFrame, distNow, distPrev);

		sighting.NearOurBase = sighting.NearOurBase || nearOurBase;
		sighting.LastCoords = coords;

		if (sighting.StandCoords == CellStruct::Empty || coords.DistanceFrom(sighting.StandCoords) > EnemyStandCells)
		{
			sighting.StandCoords = coords;
			sighting.StandSinceFrame = Unsorted::CurrentFrame;
		}
	}

	// Ages the sightings and rebuilds the enemy force profile: value by category,
	// known enemy base defenses, and early pressure near our base (rush detection).
	void RefreshOpponentModel(HouseClass* pHouse)
	{
		auto& macro = GetMacroState(pHouse);
		auto& sightings = macro.EnemySightings;

		if (Unsorted::CurrentFrame - macro.AttackSectorDecayFrame >= 2700 || macro.AttackSectorDecayFrame > Unsorted::CurrentFrame)
		{
			macro.AttackSectorDecayFrame = Unsorted::CurrentFrame;

			for (auto& weight : macro.AttackSectorWeight)
				weight = weight * 9 / 10;

			for (auto& weight : macro.AirAttackSectorWeight)
				weight = weight * 9 / 10;
		}

		std::erase_if(sightings, [](const auto& entry)
			{ return (Unsorted::CurrentFrame - entry.second.LastSeenFrame) > EnemySightingMemoryFrames; });

		NeuralOpponentProfile profile {};

		for (auto const& [uid, sighting] : sightings)
		{
			const int age = Unsorted::CurrentFrame - sighting.LastSeenFrame;
			const int weighted = sighting.Value * (EnemySightingMemoryFrames - age) / EnemySightingMemoryFrames;
			profile.Value[static_cast<size_t>(sighting.Category)] += weighted;
			profile.TotalValue += weighted;

			if (sighting.AntiAir)
				profile.AntiAirValue += weighted;

			if (sighting.NearOurBase && Unsorted::CurrentFrame <= 13500) // first ~15 minutes
				profile.RushPressure += weighted;
		}

		for (auto const pBld : BuildingClass::Array)
		{
			if (pBld && !pBld->InLimbo && pBld->IsAlive && pBld->Type && pBld->Type->IsBaseDefense && pBld->Owner
				&& pBld->Owner != pHouse && !pHouse->IsAlliedWith(pBld->Owner) && !pBld->Owner->IsNeutral()
				&& IsEnemyBuildingKnown(pHouse, pBld))
			{
				profile.KnownDefenses++;

				if (GetCombatValueOfType(pBld->Type, true) > 0)
					profile.KnownAADefenses++;
			}
		}

		macro.OpponentProfile = profile;
	}

	// Movement zone of an object's cell. A unit on a high bridge answers for the bridge deck (the
	// zone of the two banks it joins), not for the ground under it: asked without the bridge flag,
	// a unit crossing a bridge over water reads as the impassable zone (1), so the AI took every
	// unit on a bridge for one stranded on another island.
	int ObjectZone(const ObjectClass* pObject, MovementZone movementZone)
	{
		if (!pObject)
			return -1;

		return MapClass::Instance.GetMovementZoneType(pObject->GetMapCoords(), movementZone, pObject->OnBridge);
	}

	// Do the unit's own movement rules link its cell with this one? The island tests compare the
	// zones of a vehicle, and a soldier walks ground no vehicle can (city blocks, stairs, narrow
	// passes): along such a way those zones differ although the soldier gets there on foot.
	bool IsSameGroundForFoot(const FootClass* pFoot, const CellStruct& cell)
	{
		auto const pType = pFoot ? pFoot->GetTechnoType() : nullptr;

		if (!pType || pType->MovementZone == MovementZone::Normal)
			return false;

		const int ownZone = ObjectZone(pFoot, pType->MovementZone);

		return ownZone >= 0 && ownZone == MapClass::Instance.GetMovementZoneType(cell, pType->MovementZone, false);
	}

	// The ground zone a transport parked on a shore serves: the land zone most cells within 2 hold
	// (the shore cell under a hovercraft often carries a zone of its own, so it took nobody while
	// 49 soldiers waited next to it; user's own game, islands). Water: the cell's own zone.
	int TransportLandZone(const CellStruct& cell, bool onBridge)
	{
		const int ownZone = MapClass::Instance.GetMovementZoneType(cell, MovementZone::Normal, onBridge);
		auto const pOwnCell = MapClass::Instance.TryGetCellAt(cell);

		if (!pOwnCell || pOwnCell->LandType == LandType::Water)
			return ownZone;

		int bestZone = ownZone;
		int bestCount = 0;
		int ownCount = 0;
		std::array<std::pair<int, int>, 25> counts {};
		size_t used = 0;

		for (int dy = -2; dy <= 2; ++dy)
		{
			for (int dx = -2; dx <= 2; ++dx)
			{
				const CellStruct c { static_cast<short>(cell.X + dx), static_cast<short>(cell.Y + dy) };
				auto const pCell = MapClass::Instance.CoordinatesLegal(c) ? MapClass::Instance.TryGetCellAt(c) : nullptr;

				if (!pCell || pCell->LandType == LandType::Water || pCell->GetBuilding())
					continue;

				const int zone = MapClass::Instance.GetMovementZoneType(c, MovementZone::Normal, false);

				if (zone < 0)
					continue;

				size_t i = 0;

				while (i < used && counts[i].first != zone)
					++i;

				if (i == used)
					counts[used++] = { zone, 0 };

				const int count = ++counts[i].second;

				if (zone == ownZone)
					ownCount = count;

				if (count > bestCount || (count == bestCount && zone < bestZone))
				{
					bestCount = count;
					bestZone = zone;
				}
			}
		}

		// The cell's own zone wins whenever it is as common as any other.
		return (ownZone >= 0 && ownCount >= bestCount) ? ownZone : bestZone;
	}

	// The first ground zone (IsGroundZone) other than notZone of a land cell under a transport or
	// next to it (its own cell first, then the 8 around in a fixed order; cells with a building do
	// not count), or -1. On a narrow strip the cells of no ground around outnumber the strip
	// (TransportLandZone answers zone 1), so a transport beached on the strip it sailed for never saw
	// its goal (user approved, 2026-11-19, match #11475: a landing craft with an MCV stood there 50000
	// frames).
	int TransportTouchedGroundZone(const CellStruct& cell, int notZone)
	{
		static constexpr int Offsets[9][2] = { { 0, 0 }, { -1, -1 }, { 0, -1 }, { 1, -1 }, { -1, 0 }, { 1, 0 }, { -1, 1 }, { 0, 1 }, { 1, 1 } };

		for (auto const& offset : Offsets)
		{
			const CellStruct c { static_cast<short>(cell.X + offset[0]), static_cast<short>(cell.Y + offset[1]) };
			auto const pCell = MapClass::Instance.CoordinatesLegal(c) ? MapClass::Instance.TryGetCellAt(c) : nullptr;

			if (!pCell || pCell->LandType == LandType::Water || pCell->GetBuilding())
				continue;

			const int zone = MapClass::Instance.GetMovementZoneType(c, MovementZone::Normal, false);

			if (IsGroundZone(zone) && zone != notZone)
				return zone;
		}

		return -1;
	}

	// A building that stands on water: a naval one, or one whose foundation needs water (SpeedType
	// Float), as the base placement search reads it. Its deployer takes water sites only.
	bool IsWaterBuildingType(const BuildingTypeClass* pType)
	{
		return pType && (pType->Naval || pType->SpeedType == SpeedType::Float);
	}

	// Does the land cell under a transport, or one of the 8 around it, belong to this ground zone
	// (see TransportTouchedGroundZone)? Arrival by touch.
	bool TransportTouchesLandZone(const CellStruct& cell, int zone)
	{
		if (!IsGroundZone(zone))
			return false;

		for (int dy = -1; dy <= 1; ++dy)
		{
			for (int dx = -1; dx <= 1; ++dx)
			{
				const CellStruct c { static_cast<short>(cell.X + dx), static_cast<short>(cell.Y + dy) };
				auto const pCell = MapClass::Instance.CoordinatesLegal(c) ? MapClass::Instance.TryGetCellAt(c) : nullptr;

				if (pCell && pCell->LandType != LandType::Water && !pCell->GetBuilding()
					&& MapClass::Instance.GetMovementZoneType(c, MovementZone::Normal, false) == zone)
				{
					return true;
				}
			}
		}

		return false;
	}

	bool AreZonesConnected(CellStruct fromCell, CellStruct toCell, MovementZone movementZone)
	{
		if (!MapClass::Instance.CoordinatesLegal(fromCell) || !MapClass::Instance.CoordinatesLegal(toCell))
			return false;

		const int z1 = MapClass::Instance.GetMovementZoneType(fromCell, movementZone, false);
		const int z2 = MapClass::Instance.GetMovementZoneType(toCell, movementZone, false);

		return z1 != -1 && z1 == z2;
	}

	void GetDefenseCapabilities(BuildingTypeClass* pBld, bool& outAntiInf, bool& outAntiArmor, bool& outAntiAir,
		bool& outPureAA, bool& outUnarmedBunker)
	{
		outAntiInf = false;
		outAntiArmor = false;
		outAntiAir = false;
		outPureAA = false;
		outUnarmedBunker = false;

		if (!pBld)
			return;

		const auto pWS0 = pBld->GetWeapon(0);
		const auto pW0 = pWS0 ? pWS0->WeaponType : nullptr;
		const auto pWS1 = pBld->GetWeapon(1);
		const auto pW1 = pWS1 ? pWS1->WeaponType : nullptr;

		outUnarmedBunker = (pBld->CanBeOccupied || pBld->Bunker) && (pW0 == nullptr || pW0->Damage <= 0)
			&& (pW1 == nullptr || pW1->Damage <= 0);

		const bool w0AA = pW0 && pW0->Projectile && pW0->Projectile->AA;
		const bool w0AG = pW0 && (!pW0->Projectile || pW0->Projectile->AG);
		const bool w1AA = pW1 && pW1->Projectile && pW1->Projectile->AA;
		const bool w1AG = pW1 && (!pW1->Projectile || pW1->Projectile->AG);

		outAntiAir = (pBld->AntiAirValue > 0) || w0AA || w1AA;
		outPureAA = (w0AA || w1AA) && !w0AG && !w1AG && (pBld->AntiInfantryValue <= 0 && pBld->AntiArmorValue <= 0);

		if (pBld->AntiInfantryValue > 0 || pBld->AntiArmorValue > 0)
		{
			outAntiInf = (pBld->AntiInfantryValue > 0);
			outAntiArmor = (pBld->AntiArmorValue > 0);
		}
		else if (!outPureAA && !outUnarmedBunker && (w0AG || w1AG))
		{
			const auto pGndW = w0AG ? pW0 : pW1;
			const bool highTierOrHeavy = (pGndW && (pGndW->Damage >= 65 || pGndW->Range >= 6 * 256))
				|| pBld->Cost >= 800 || pBld->TechLevel >= 4;

			outAntiArmor = highTierOrHeavy;
			outAntiInf = !highTierOrHeavy || (pGndW && pGndW->Damage >= 65);
		}
	}

	void RecordUnsafePlacementZone(HouseClass* pHouse, CellStruct coords, int durationFrames = 2700)
	{
		if (!pHouse || coords == CellStruct::Empty)
			return;

		auto& state = GetMacroState(pHouse);
		const int expFrame = Unsorted::CurrentFrame + durationFrames;

		for (auto& z : state.UnsafePlacementZones)
		{
			if (z.Coords.DistanceFromSquared(coords) <= 36.0)
			{
				z.Coords = coords;
				z.ExpirationFrame = std::max(z.ExpirationFrame, expFrame);
				return;
			}
		}

		if (state.UnsafePlacementZones.size() >= 16)
			state.UnsafePlacementZones.erase(state.UnsafePlacementZones.begin());

		state.UnsafePlacementZones.push_back({ coords, expFrame });
	}

	void PruneAttackedBuildingsLIFO(HouseClass* pHouse)
	{
		if (!pHouse)
			return;

		auto& state = GetMacroState(pHouse);

		if (!state.UnsafePlacementZones.empty())
		{
			state.UnsafePlacementZones.erase(
				std::remove_if(state.UnsafePlacementZones.begin(), state.UnsafePlacementZones.end(),
					[&](const NeuralUnsafeZone& z)
					{
						if (z.Coords == CellStruct::Empty || Unsorted::CurrentFrame >= z.ExpirationFrame)
							return true;

						// Once a ground defense turret secures this zone (within 7 cells), lift the unsafe penalty!
						for (auto const pDef : pHouse->Buildings)
						{
							if (!pDef || !pDef->IsAlive || pDef->InLimbo || !pDef->Type || !pDef->Type->IsBaseDefense
								|| IsTunnelNetworkBuilding(pDef->Type))
							{
								continue;
							}

							if (pDef->GetMapCoords().DistanceFromSquared(z.Coords) <= 49.0)
								return true;
						}

						return false;
					}),
				state.UnsafePlacementZones.end());
		}

		if (state.FrontlineThreatNeedsDefenses > 0 && Unsorted::CurrentFrame > state.FrontlineThreatActiveFrames)
		{
			state.FrontlineThreatNeedsDefenses = 0;
			state.FrontlineThreatCoords = CellStruct::Empty;
			state.FrontlineThreatBuildingCoords = CellStruct::Empty;
		}

		if (state.AttackedBuildingsLIFO.empty())
			return;

		state.AttackedBuildingsLIFO.erase(
			std::remove_if(state.AttackedBuildingsLIFO.begin(), state.AttackedBuildingsLIFO.end(),
				[&](const NeuralAttackedBuildingRecord& rec)
				{
					if (Unsorted::CurrentFrame - rec.AttackFrame > 1800)
						return true;

					auto const pCell = MapClass::Instance.TryGetCellAt(rec.BuildingCoords);

					if (!pCell)
						return true;

					auto const pBld = pCell->GetBuilding();

					if (!pBld || !pBld->IsAlive || pBld->InLimbo
						|| (pBld->Owner != pHouse && !pHouse->IsAlliedWith(pBld->Owner)))
					{
						return true;
					}

					// AdvAI LIFO pruning: remove the record once a base defense with the
					// matching counter capability exists within 8.0 cells of the building
					for (auto const pDef : pHouse->Buildings)
					{
						if (!pDef || !pDef->IsAlive || pDef->InLimbo || !pDef->Type || !pDef->Type->IsBaseDefense
							|| IsTunnelNetworkBuilding(pDef->Type))
						{
							continue;
						}

						if (pDef->GetMapCoords().DistanceFromSquared(rec.BuildingCoords) > 64.0)
							continue;

						bool antiInf = false;
						bool antiArmor = false;
						bool antiAir = false;
						bool pureAA = false;
						bool unarmedBunker = false;
						GetDefenseCapabilities(pDef->Type, antiInf, antiArmor, antiAir, pureAA, unarmedBunker);

						if (unarmedBunker && pDef->Occupants.Count == 0 && pDef->BunkerLinkedItem == nullptr)
							continue;

						if (rec.Threat == NeuralThreatCategory::Air && antiAir)
							return true;

						if (rec.Threat == NeuralThreatCategory::Vehicle && (antiArmor || (!pureAA && unarmedBunker)))
							return true;

						if (rec.Threat == NeuralThreatCategory::Infantry && (antiInf || (!pureAA && unarmedBunker)))
							return true;

						if (rec.Threat == NeuralThreatCategory::None && !pureAA)
							return true;
					}

					return false;
				}),
			state.AttackedBuildingsLIFO.end());
	}

	NeuralThreatCategory GetActiveThreatCategory(HouseClass* pHouse, CellStruct threatCenter)
	{
		if (!pHouse || threatCenter == CellStruct::Empty)
			return NeuralThreatCategory::None;

		int infantryCount = 0;
		int vehicleCount = 0;
		int airCount = 0;
		constexpr double radiusSq = 225.0; // 15 cells

		for (auto const pFoot : FootClass::Array)
		{
			if (!pFoot || !pFoot->IsAlive || pFoot->InLimbo)
				continue;

			HouseClass* const pOwner = pFoot->Owner;

			if (!pOwner || pOwner == pHouse || pHouse->IsAlliedWith(pOwner) || pOwner->IsNeutral()
				|| pOwner->IsObserver())
			{
				continue;
			}

			if (pFoot->GetMapCoords().DistanceFromSquared(threatCenter) > radiusSq)
				continue;

			auto const pTType = pFoot->GetTechnoType();

			if (pFoot->IsInAir() || pFoot->WhatAmI() == AbstractType::Aircraft || (pTType && pTType->JumpJet))
				airCount++;
			else if (pFoot->WhatAmI() == AbstractType::Infantry)
				infantryCount++;
			else if (pFoot->WhatAmI() == AbstractType::Unit)
				vehicleCount++;
		}

		for (auto const pBld : BuildingClass::Array)
		{
			if (!pBld || !pBld->IsAlive || pBld->InLimbo || !pBld->Type || pBld->Type->Insignificant
				|| pBld->Type->InvisibleInGame)
			{
				continue;
			}

			HouseClass* const pOwner = pBld->Owner;

			if (!pOwner || pOwner == pHouse || pHouse->IsAlliedWith(pOwner) || !IsEnemyBuildingKnown(pHouse, pBld) || pOwner->IsNeutral()
				|| pOwner->IsObserver())
			{
				continue;
			}

			if (pBld->GetMapCoords().DistanceFromSquared(threatCenter) <= radiusSq)
				vehicleCount++;
		}

		if (infantryCount == 0 && vehicleCount == 0 && airCount == 0)
			return NeuralThreatCategory::None;

		if (airCount >= vehicleCount && airCount >= infantryCount)
			return NeuralThreatCategory::Air;

		if (vehicleCount >= infantryCount)
			return NeuralThreatCategory::Vehicle;

		return NeuralThreatCategory::Infantry;
	}

	// Enemy air power as seen by this house (fog-fair memory, see RefreshAirThreatSummary),
	// scaled so a Kirov is ~26 and a light attack plane ~15.
	int CalculateEnemyAircraftValue(HouseClass* pHouse)
	{
		if (!pHouse)
			return 0;

		return GetMacroState(pHouse).AirThreatValue / 8;
	}

	// Guns that shoot up a key asset wants within CoverageDefenseCells against this enemy air value.
	int AirCoverGunsPerAsset(int enemyAirValue)
	{
		return std::clamp(1 + enemyAirValue / AirCoverValuePerGun, 1, AirCoverMostPerAsset);
	}

	// Enemy ground and naval forces that came at our base (seen within reach of our buildings) and,
	// at GroundThreatApproachPercent, those seen within GroundThreatWatchCells of home: a blitz
	// asks for its towers before it reaches the base. Remembered for EnemySightingMemoryFrames,
	// fading with age, in ground defense points.
	int EnemyGroundThreatValue(HouseClass* pHouse)
	{
		const CellStruct home = GetHouseHomeCell(pHouse);
		int value = 0;

		for (auto const& [uid, sighting] : GetMacroState(pHouse).EnemySightings)
		{
			const int age = Unsorted::CurrentFrame - sighting.LastSeenFrame;

			if (sighting.Category == EnemyForceCategory::Air || age < 0 || age > EnemySightingMemoryFrames || !sighting.Type)
				continue;

			// Harvesters and other unarmed units ask for no tower (TechnoTypeClass::GetWeapon is not const-qualified in YRpp).
			auto const pSeenType = const_cast<TechnoTypeClass*>(sighting.Type);
			auto const pW0 = pSeenType->GetWeapon(0);
			auto const pW1 = pSeenType->GetWeapon(1);

			if (!(pW0 && pW0->WeaponType) && !(pW1 && pW1->WeaponType))
				continue;

			const int weighted = sighting.Value * (EnemySightingMemoryFrames - age) / EnemySightingMemoryFrames;

			if (sighting.NearOurBase)
				value += weighted;
			else if (home != CellStruct::Empty && sighting.LastCoords.DistanceFrom(home) <= GroundThreatWatchCells)
				value += weighted * GroundThreatApproachPercent / 100;
		}

		return value / GroundThreatValuePerPoint;
	}

	// Ground towers a key asset wants within its cover against this ground threat (points).
	int GroundCoverTowersPerAsset(int groundThreat)
	{
		return std::clamp(1 + groundThreat / GroundCoverPointsPerTower, 1, GroundCoverMostPerAsset);
	}

	// Reach within which the towers of a key asset count as its cover.
	int GroundCoverCellsFor(const BuildingTypeClass* pType)
	{
		return pType && pType->ToProtect ? ToProtectCoverCells : CoverageDefenseCells;
	}

	// Units of this anti-air type it takes to cover the remembered enemy air threat our anti-air
	// (units, guns and aircraft that shoot up, OwnAACapacity) leaves uncovered; 0 without one.
	int AntiAirUnitsMissing(HouseClass* pHouse, const TechnoTypeClass* pType)
	{
		auto const& macro = GetMacroState(pHouse);
		const int uncovered = macro.AirThreatValue - macro.OwnAACapacity;
		const int worth = GetCombatValueOfType(pType, true);

		return uncovered > 0 && worth > 0 ? (uncovered + worth - 1) / worth : 0;
	}

	// The long ramp of an anti-air slot of the Expert priors on top of its short one (AirDeficitSlot*).
	int AirDeficitSlotLongRamp(int airDeficit)
	{
		return std::clamp(airDeficit / AirDeficitSlotDivisor, 0, AirDeficitSlotLongMost);
	}

	// A building an air raid goes for: yards, factories, refineries, superweapons, ToProtect and
	// captured tech or cash buildings.
	bool IsAirCoverAssetType(const BuildingTypeClass* pType)
	{
		return pType && !pType->IsBaseDefense && !pType->InvisibleInGame
			&& (pType->ConstructionYard || pType->Factory != AbstractType::None || pType->Refinery || pType->SuperWeapon != -1
				|| pType->ToProtect || pType->ProduceCashAmount > 0 || pType->ProduceCashStartup > 0
				|| (pType->Capturable && pType->NeedsEngineer));
	}

	// Value of the aircraft of each enemy this house has seen within EnemySightingMemoryFrames.
	std::array<int, 8> EnemyAirValueByOwner(HouseClass* pHouse)
	{
		std::array<int, 8> values {};

		for (auto const& [uid, sighting] : GetMacroState(pHouse).EnemySightings)
		{
			const int age = Unsorted::CurrentFrame - sighting.LastSeenFrame;

			if (sighting.Category == EnemyForceCategory::Air && sighting.OwnerIndex >= 0 && sighting.OwnerIndex < 8 && age >= 0
				&& age <= EnemySightingMemoryFrames)
			{
				values[static_cast<size_t>(sighting.OwnerIndex)] += sighting.Value;
			}
		}

		return values;
	}

	// What an airfield or a helipad gains as a target: `percent` of the value of its owner's
	// aircraft seen, `most` at most.
	int AirBaseTargetBonus(const std::array<int, 8>& airValues, const BuildingClass* pBld, int percent, int most)
	{
		if (!pBld || !pBld->Type || !pBld->Owner || (pBld->Type->Factory != AbstractType::AircraftType && !pBld->Type->Helipad))
			return 0;

		const int owner = pBld->Owner->ArrayIndex;

		return owner >= 0 && owner < 8 ? std::min(most, airValues[static_cast<size_t>(owner)] * percent / 100) : 0;
	}

	// Defined in NeuralAI.Placement.inl: does this war factory keep a two-way road out of the base?
	bool HasFactoryRoadOut(HouseClass* pHouse, const BuildingClass* pFactory);

	// Capture barracks (NeuralAI.SpecOps.inl): the barracks of ours an engineer in training comes out of.
	CellStruct CaptureEngineerGoal(HouseClass* pHouse);
	void NoteCaptureEngineerRouted(HouseClass* pHouse, const TechnoClass* pEngineer, const BuildingClass* pBarracks);

	// Updates PrimaryFactory for Infantry, Vehicles, and Naval units to the factory
	// closest & zone-connected to the frontline target. Vehicles leave by the primary war factory:
	// one closed in by the base (HasFactoryRoadOut) is taken only when none has a road out.
	void UpdatePrimaryFactories(HouseClass* pHouse, CellStruct targetCoords)
	{
		if (!pHouse || targetCoords == CellStruct::Empty)
			return;

		auto updateCategory = [&](AbstractType type, bool isNaval, CellStruct categoryGoal)
		{
			const CellStruct goalCoords = (categoryGoal != CellStruct::Empty) ? categoryGoal : targetCoords;
			if (goalCoords == CellStruct::Empty)
				return;

			BuildingClass* pBestFactory = nullptr;
			double bestDistanceSq = 1e18;

			for (auto const pBld : pHouse->Buildings)
			{
				if (!pBld || !pBld->IsAlive || pBld->InLimbo || !pBld->Type)
					continue;

				if (pBld->Type->Factory != type || pBld->Type->Naval != isNaval)
					continue;

				if (pBld->CurrentMission == Mission::Selling || pBld->QueuedMission == Mission::Selling)
					continue;

				if (type == AbstractType::AircraftType && !BuildingExt::HasFreeDocks(pBld))
					continue;

				if (!isNaval && type != AbstractType::AircraftType
					&& !AreZonesConnected(pBld->GetMapCoords(), goalCoords, MovementZone::Normal))
				{
					continue;
				}

				double distSq = pBld->GetMapCoords().DistanceFromSquared(goalCoords);

				// Farther than any war factory with a road out, whatever the distances.
				if (type == AbstractType::UnitType && !isNaval && !HasFactoryRoadOut(pHouse, pBld))
					distSq += ClosedInFactoryDistanceSq;

				if (distSq < bestDistanceSq)
				{
					bestDistanceSq = distSq;

					pBestFactory = pBld;
				}
			}

			if (!pBestFactory)
			{
				for (auto const pBld : pHouse->Buildings)
				{
					if (!pBld || !pBld->IsAlive || pBld->InLimbo || !pBld->Type)
						continue;

					if (pBld->Type->Factory != type || pBld->Type->Naval != isNaval)
						continue;

					const double distSq = pBld->GetMapCoords().DistanceFromSquared(goalCoords);

					if (distSq < bestDistanceSq)
					{
						bestDistanceSq = distSq;

						pBestFactory = pBld;
					}
				}
			}

			if (!pBestFactory || pBestFactory->IsPrimaryFactory)
				return;

			for (auto const pBld : pHouse->Buildings)
			{
				if (pBld && pBld->Type && pBld->Type->Factory == type && pBld->Type->Naval == isNaval)
					pBld->IsPrimaryFactory = (pBld == pBestFactory);
			}

			if (auto const pExt = HouseExt::Fetch(pHouse))
			{
				if (type == AbstractType::UnitType)
				{
					if (isNaval)
						pExt->Factory_NavyType = pBestFactory;
					else
						pExt->Factory_VehicleType = pBestFactory;
				}
				else if (type == AbstractType::InfantryType)
				{
					pExt->Factory_InfantryType = pBestFactory;
				}
				else if (type == AbstractType::AircraftType)
				{
					pExt->Factory_AircraftType = pBestFactory;
				}
				else if (type == AbstractType::BuildingType)
				{
					pExt->Factory_BuildingType = pBestFactory;
				}
			}

			FactoryClass* pActiveFactory = pHouse->GetPrimaryFactory(type, isNaval, BuildCat::DontCare);

			if (!pActiveFactory)
			{
				for (auto const pBld : pHouse->Buildings)
				{
					if (pBld && pBld->IsAlive && pBld->Type && pBld->Type->Factory == type
						&& pBld->Type->Naval == isNaval && pBld->Factory != nullptr)
					{
						pActiveFactory = pBld->Factory;

						break;
					}
				}
			}

			if (pActiveFactory)
			{
				for (auto const pBld : pHouse->Buildings)
				{
					if (pBld && pBld->Type && pBld->Type->Factory == type && pBld->Type->Naval == isNaval)
					{
						pBld->IsPrimaryFactory = (pBld == pBestFactory);

						if (pBld == pBestFactory)
							pBld->Factory = pActiveFactory;
						else if (pBld->Factory == pActiveFactory)
							pBld->Factory = nullptr;
					}
				}

				pHouse->SetPrimaryFactory(pActiveFactory, type, isNaval, BuildCat::DontCare);
			}
		};

		auto const pExt = HouseExt::Fetch(pHouse);
		const CellStruct homeCenter
			= pHouse->BaseCenter != CellStruct::Empty ? pHouse->BaseCenter : pHouse->BaseSpawnCell;
		PruneAttackedBuildingsLIFO(pHouse);
		auto& macroState = GetMacroState(pHouse);
		CellStruct defenseGoal = CellStruct::Empty;
		if (!macroState.AttackedBuildingsLIFO.empty()
			&& (Unsorted::CurrentFrame - macroState.AttackedBuildingsLIFO.back().AttackFrame) <= 270)
		{
			defenseGoal = macroState.AttackedBuildingsLIFO.back().BuildingCoords;
		}

		CellStruct vehGoal = (defenseGoal != CellStruct::Empty) ? defenseGoal : targetCoords;
		const UnitTypeClass* pCurUnitType = nullptr;
		if (FactoryClass* pVF = pHouse->GetPrimaryFactory(AbstractType::UnitType, false, BuildCat::DontCare))
		{
			if (pVF->Object)
			{
				pCurUnitType
					= abstract_cast<UnitClass*>(pVF->Object) ? abstract_cast<UnitClass*>(pVF->Object)->Type : nullptr;
			}
		}

		if (!pCurUnitType && pExt && pExt->NeuralAI_NextUnitTarget >= 0
			&& pExt->NeuralAI_NextUnitTarget < UnitTypeClass::Array.Count)
		{
			pCurUnitType = UnitTypeClass::Array.GetItem(pExt->NeuralAI_NextUnitTarget);
		}

		if (!pCurUnitType && pHouse->ProducingUnitTypeIndex >= 0
			&& pHouse->ProducingUnitTypeIndex < UnitTypeClass::Array.Count)
		{
			pCurUnitType = UnitTypeClass::Array.GetItem(pHouse->ProducingUnitTypeIndex);
		}

		if (pCurUnitType)
		{
			if (pCurUnitType->Harvester || pCurUnitType->ResourceGatherer)
			{
				CellStruct bestRefCell = CellStruct::Empty;
				int minNearbyHarvs = 9999;
				for (auto const pRef : pHouse->Buildings)
				{
					if (!pRef || !pRef->IsAlive || pRef->InLimbo || !pRef->Type || !pRef->Type->Refinery)
						continue;

					const CellStruct rc = pRef->GetMapCoords();
					int nearHarvs = 0;
					for (auto const pU : UnitClass::Array)
					{
						if (pU && pU->Owner == pHouse && pU->IsAlive && !pU->InLimbo && pU->Type
							&& (pU->Type->Harvester || pU->Type->ResourceGatherer))
						{
							if (pU->GetMapCoords().DistanceFrom(rc) <= 18.0)
								nearHarvs++;
						}
					}

					if (nearHarvs < minNearbyHarvs)
					{
						minNearbyHarvs = nearHarvs;
						bestRefCell = rc;
					}
				}

				if (bestRefCell != CellStruct::Empty)
					vehGoal = bestRefCell;
			}
			else if (defenseGoal == CellStruct::Empty && homeCenter != CellStruct::Empty
				&& pCurUnitType->DeploysInto != nullptr
				&& (pCurUnitType->DeploysInto->ConstructionYard || pCurUnitType->DeploysInto->BaseNormal
					|| pCurUnitType->DeploysInto->Refinery))
			{
				CellStruct bestExpOre = CellStruct::Empty;
				int bestOreDist = 999999;
				for (int dy = -48; dy <= 48; dy += 4)
				{
					for (int dx = -48; dx <= 48; dx += 4)
					{
						const CellStruct tc { static_cast<short>(homeCenter.X + dx),
							static_cast<short>(homeCenter.Y + dy) };
						auto const pC = MapClass::Instance.TryGetCellAt(tc);
						if (!pC || pC->GetContainedTiberiumValue() <= 0)
							continue;

						bool nearHub = false;
						for (auto const pOwnB : pHouse->Buildings)
						{
							if (pOwnB && pOwnB->IsAlive && !pOwnB->InLimbo && pOwnB->Type
								&& (pOwnB->Type->Refinery || pOwnB->Type->ConstructionYard || pOwnB->Type->BaseNormal))
							{
								const CellStruct bc = pOwnB->GetMapCoords();
								if (std::max(std::abs(tc.X - bc.X), std::abs(tc.Y - bc.Y)) < 14)
								{
									nearHub = true;
									break;
								}
							}
						}

						if (nearHub)
							continue;

						const int d = std::abs(dx) + std::abs(dy);
						if (d < bestOreDist)
						{
							bestOreDist = d;
							bestExpOre = tc;
						}
					}
				}

				if (bestExpOre != CellStruct::Empty)
					vehGoal = bestExpOre;
			}
		}

		CellStruct infGoal = (defenseGoal != CellStruct::Empty) ? defenseGoal : targetCoords;
		const InfantryTypeClass* pCurInfType = nullptr;
		if (FactoryClass* pIF = pHouse->GetPrimaryFactory(AbstractType::InfantryType, false, BuildCat::DontCare))
		{
			if (pIF->Object)
			{
				pCurInfType = abstract_cast<InfantryClass*>(pIF->Object)
					? abstract_cast<InfantryClass*>(pIF->Object)->Type
					: nullptr;
			}
		}

		if (!pCurInfType && pExt && pExt->NeuralAI_NextInfantryTarget >= 0
			&& pExt->NeuralAI_NextInfantryTarget < InfantryTypeClass::Array.Count)
		{
			pCurInfType = InfantryTypeClass::Array.GetItem(pExt->NeuralAI_NextInfantryTarget);
		}

		if (!pCurInfType && pHouse->ProducingInfantryTypeIndex >= 0
			&& pHouse->ProducingInfantryTypeIndex < InfantryTypeClass::Array.Count)
		{
			pCurInfType = InfantryTypeClass::Array.GetItem(pHouse->ProducingInfantryTypeIndex);
		}

		if (pHouse->HasLowPower() || (pHouse->PowerOutput - pHouse->PowerDrain) < 350)
		{
			for (auto const pBld : pHouse->Buildings)
			{
				if (pBld && pBld->IsAlive && !pBld->InLimbo && pBld->Type && pBld->Type->InfantryAbsorb
					&& (pBld->Type->PowerBonus > 0 || pBld->Type->ExtraPowerBonus > 0)
					&& pBld->Passengers.NumPassengers < pBld->Type->Passengers)
				{
					infGoal = pBld->GetMapCoords();
					break;
				}
			}
		}

		// An engineer comes out of the barracks of ours on the land of the tech it is trained for
		// (UpdateCaptureBarracks), so that it walks there instead of crossing the water; without such a
		// tech, out of the barracks nearest the neutral tech nearest home.
		const CellStruct captureGoal = IsCaptureInfantryType(pCurInfType) ? CaptureEngineerGoal(pHouse) : CellStruct::Empty;

		if (captureGoal != CellStruct::Empty)
		{
			infGoal = captureGoal;
		}
		else if (IsCaptureInfantryType(pCurInfType) && homeCenter != CellStruct::Empty)
		{
			CellStruct bestCapCell = CellStruct::Empty;
			int bestCapScore = 999999;
			for (auto const pBld : BuildingClass::Array)
			{
				if (!pBld || !pBld->IsAlive || pBld->InLimbo || !pBld->Type || !pBld->Owner)
					continue;

				if (!pBld->Type->Capturable || !pBld->Owner->IsNeutral())
					continue;

				const CellStruct bc = pBld->GetMapCoords();
				const int dist = std::abs(bc.X - homeCenter.X) + std::abs(bc.Y - homeCenter.Y);
				const int effScore = (pBld->Type->ProduceCashAmount > 0) ? (dist / 2) : dist;
				if (effScore < bestCapScore)
				{
					bestCapScore = effScore;
					bestCapCell = bc;
				}
			}

			if (bestCapCell != CellStruct::Empty)
				infGoal = bestCapCell;
		}

		updateCategory(AbstractType::InfantryType, false, infGoal);

		if (captureGoal != CellStruct::Empty)
		{
			for (auto const pBld : pHouse->Buildings)
			{
				if (!pBld || !pBld->IsAlive || !pBld->Type || pBld->Type->Factory != AbstractType::InfantryType || !pBld->IsPrimaryFactory
					|| pBld->GetMapCoords() != captureGoal || !pBld->Factory)
				{
					continue;
				}

				auto const pInTraining = abstract_cast<InfantryClass*>(pBld->Factory->Object);

				if (pInTraining && IsCaptureInfantryType(pInTraining->Type))
					NoteCaptureEngineerRouted(pHouse, pInTraining, pBld);

				break;
			}
		}

		updateCategory(AbstractType::UnitType, false, vehGoal);

		updateCategory(AbstractType::UnitType, true, targetCoords);
		updateCategory(AbstractType::AircraftType, false, targetCoords);
	}
} // namespace

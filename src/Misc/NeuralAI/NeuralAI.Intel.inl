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
		track.ThreatValue = GetCombatValueOfType(pType, false);
		track.IsHeavy = pType->Strength >= 600;
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

		// Attack route memory: the side this enemy unit reached our base from.
		if (nearOurBase && !sighting.NearOurBase && sighting.Category != EnemyForceCategory::Air)
		{
			const int sector = AttackSectorOf(GetHouseHomeCell(pHouse), coords);

			if (sector >= 0)
				macro.AttackSectorWeight[sector] += sighting.Value;
		}

		if (sighting.Category != EnemyForceCategory::Air && sighting.Category != EnemyForceCategory::Naval)
			RecordRouteHeatSighting(pHouse, pTechno, coords, prevCoords, prevFrame, distNow, distPrev);

		sighting.NearOurBase = sighting.NearOurBase || nearOurBase;
		sighting.LastCoords = coords;
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

	// Updates PrimaryFactory for Infantry, Vehicles, and Naval units to the factory
	// closest & zone-connected to the frontline target
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

				const double distSq = pBld->GetMapCoords().DistanceFromSquared(goalCoords);

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

		if (pCurInfType && pCurInfType->Engineer && homeCenter != CellStruct::Empty)
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

		updateCategory(AbstractType::UnitType, false, vehGoal);

		updateCategory(AbstractType::UnitType, true, targetCoords);
		updateCategory(AbstractType::AircraftType, false, targetCoords);
	}
} // namespace

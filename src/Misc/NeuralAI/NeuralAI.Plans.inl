// NeuralAI.Plans.inl: Strategic plan library and its helpers.
// Part of NeuralAI.cpp (included in order; one translation unit).

namespace
{
	int AirSortieDamageVs(AircraftClass* pAir, BuildingClass* pTarget);
	bool IsWingStrikeType(AircraftTypeClass* pType);

	bool IsPlanMember(HouseClass* pHouse, DWORD uid)
	{
		if (!pHouse)
			return false;

		auto const& members = GetMacroState(pHouse).PlanMembers;

		return members.find(uid) != members.end();
	}

	// The kind of a building, for the follow-up attack of a plan that took one (PlanFollowUp*): 0 a defense,
	// 1 a construction yard, 2 a factory, 3 a refinery or resource building, 4 a power plant, 5 any other.
	int PlanBuildingKindOf(const BuildingTypeClass* pType)
	{
		if (!pType)
			return -1;

		if (pType->IsBaseDefense)
			return 0;

		if (pType->ConstructionYard)
			return 1;

		if (pType->Factory != AbstractType::None)
			return 2;

		if (pType->Refinery || pType->ResourceDestination || pType->ResourceGatherer)
			return 3;

		return pType->PowerBonus > 0 ? 4 : 5;
	}

	bool WeaponHitsGround(const WeaponTypeClass* pWeapon)
	{
		return pWeapon && pWeapon->Damage > 0 && (!pWeapon->Projectile || pWeapon->Projectile->AG);
	}

	int GroundRangeOf(TechnoTypeClass* pType)
	{
		if (!pType)
			return 0;

		int range = 0;

		for (int i = 0; i < 2; ++i)
		{
			auto const pWS = pType->GetWeapon(i);

			if (pWS && LandTargetingAllows(pType, i) && WeaponHitsGround(pWS->WeaponType))
				range = std::max(range, pWS->WeaponType->Range);
		}

		return range;
	}

	// Dead zone of a type in leptons: the smallest minimum range among its weapons that hit the
	// ground, 0 when one of them fires point-blank (or it has none).
	int GroundDeadZoneOf(TechnoTypeClass* pType)
	{
		if (!pType)
			return 0;

		int deadZone = INT_MAX;

		for (int i = 0; i < 2; ++i)
		{
			auto const pWS = pType->GetWeapon(i);

			if (pWS && LandTargetingAllows(pType, i) && WeaponHitsGround(pWS->WeaponType))
				deadZone = std::min(deadZone, pWS->WeaponType->MinimumRange);
		}

		return deadZone == INT_MAX ? 0 : deadZone;
	}

	// A long-range gun tower with a dead zone (a Grand Cannon): a defense that reaches
	// ArtilleryRangeCells or more on the ground and cannot fire at what stands within a cell of it.
	// It out-reaches most artillery and is helpless next to an enemy, so it is no first line.
	bool IsDeadZoneTower(BuildingTypeClass* pType)
	{
		return pType && pType->IsBaseDefense && GroundRangeOf(pType) >= ArtilleryRangeCells * Unsorted::LeptonsPerCell
			&& GroundDeadZoneOf(pType) >= Unsorted::LeptonsPerCell;
	}

	// One of the ground weapons of this type does damage to that building's armor: a dog or a gun
	// that only hits infantry makes no house answer for its building (Bait Ambush, Feint).
	bool HurtsBuilding(TechnoTypeClass* pType, BuildingClass* pBld)
	{
		if (!pType || !pBld || !pBld->Type)
			return false;

		for (int i = 0; i < 2; ++i)
		{
			auto const pWS = pType->GetWeapon(i);
			auto const pWeapon = pWS ? pWS->WeaponType : nullptr;

			if (pWeapon && pWeapon->Warhead && LandTargetingAllows(pType, i) && WeaponHitsGround(pWeapon)
				&& GeneralUtils::GetWarheadVersusArmor(pWeapon->Warhead, pBld, pBld->Type) > 0.0)
			{
				return true;
			}
		}

		return false;
	}

	// A structure a siege unit unfolded into: armed against the ground, packs up again
	// (UndeploysInto), not a ConYard, refinery, slave miner or base extender. Nod artillery,
	// Juggernaut, Tick Tank in C&C Reloaded; nothing is matched by name.
	bool IsDeployedSiegeBuildingType(BuildingTypeClass* pType)
	{
		return pType && pType->UndeploysInto && !pType->ConstructionYard && !pType->Refinery && !pType->ResourceGatherer
			&& !pType->BaseNormal && pType->Enslaves == nullptr && GroundRangeOf(pType) > 0;
	}

	// A unit that unfolds into such a structure and back into the same unit.
	bool IsDeployableSiegeUnitType(UnitTypeClass* pType)
	{
		return pType && pType->DeploysInto && IsDeployedSiegeBuildingType(pType->DeploysInto)
			&& pType->DeploysInto->UndeploysInto == pType;
	}

	// Frames a siege unit may sit on its unfold order before it counts as stuck: the unfold itself
	// takes far less.
	constexpr int SiegeUnfoldStuckFrames = 150;

	// An unfold that cannot happen (its ground is taken or unfit) leaves the unit on the order for
	// good, out of the fight.
	bool IsSiegeUnfoldStuck(UnitClass* pUnit)
	{
		return IsDeployableSiegeUnitType(pUnit->Type) && pUnit->CurrentMission == Mission::Unload && !pUnit->IsDeploying
			&& Unsorted::CurrentFrame - pUnit->CurrentMissionStartTime >= SiegeUnfoldStuckFrames;
	}

	// Ground range a siege plan can count on: for a deployable siege unit the longer of its own
	// gun and the gun of the structure it unfolds into.
	int SiegeRangeOf(TechnoTypeClass* pType)
	{
		int range = GroundRangeOf(pType);

		if (pType && pType->WhatAmI() == AbstractType::UnitType)
		{
			auto const pUnitType = static_cast<UnitTypeClass*>(pType);

			if (IsDeployableSiegeUnitType(pUnitType))
				range = std::max(range, GroundRangeOf(pUnitType->DeploysInto));
		}

		return range;
	}

	// A vehicle the tank floor gives an escort to (the production check's ownedArtilleryVehicles,
	// ownedEscortedAntiAirVehicles and ownedEscortedSupportVehicles): artillery, anti-air, or an
	// unarmed support vehicle; no main battle tank, gatherer, construction vehicle or transport.
	bool IsTankEscortedVehicle(FootClass* pFoot)
	{
		auto const pUnit = abstract_cast<UnitClass*>(pFoot);
		auto const pType = pUnit ? pUnit->Type : nullptr;

		if (!pType || pType->Naval || pType->Harvester || pType->ResourceGatherer || IsMainBattleTankType(pType)
			|| (pType->DeploysInto && pType->DeploysInto->ConstructionYard) || IsAmphibiousOrAirTransport(pType)
			|| (TechnoTypeExt::Fetch(pType) && TechnoTypeExt::Fetch(pType)->Carryall))
		{
			return false;
		}

		auto const pWS = pType->GetWeapon(0);
		auto const pWeapon = pWS ? pWS->WeaponType : nullptr;

		if (!pWeapon)
			return pType->Passengers == 0;

		return pWeapon->Range >= ArtilleryRangeCells * Unsorted::LeptonsPerCell || ShootsAtAircraft(pType);
	}

	// A gun the siege screen stands for (SiegeScreen*): a ground unit that is no main battle tank with
	// SiegeRangeOf of ArtilleryRangeCells or more, on the map.
	bool IsSiegeScreenGun(FootClass* pFoot)
	{
		auto const pUnit = abstract_cast<UnitClass*>(pFoot);

		return pUnit && pUnit->Type && !pUnit->InLimbo && pUnit->IsAlive && !pUnit->Type->Naval && !IsMainBattleTankType(pUnit->Type)
			&& SiegeRangeOf(pUnit->Type) >= ArtilleryRangeCells * Unsorted::LeptonsPerCell;
	}

	// The cell a ground unit waits on out of every known enemy defense's reach + SiegeScreenMarginCells
	// (siege screen, push gate): the last cell of its straight way to `goal` out of every such reach
	// or, from inside one, the first cell out of it on the way to `back` (SiegeScreenBackCells at
	// most); then the first cell within SiegeScreenRingCells of that point on its own ground, out of
	// every such reach, with no building, bridge, tube, passage mouth or narrow passage, and farther
	// than SiegeScreenTransportCells from every transport of ours on the ground (user: it must not
	// hinder the transports). nullptr: neither its cell nor its goal is in such a reach, or no cell.
	CellClass* DefenseReachWaitCell(HouseClass* pHouse, FootClass* pUnit, const CellStruct& goal, const CellStruct& back)
	{
		auto const pType = pUnit ? pUnit->GetTechnoType() : nullptr;

		if (!pType || goal == CellStruct::Empty || back == CellStruct::Empty)
			return nullptr;

		auto const inReach = [pHouse](const CellStruct& cell) { return IsCellInKnownEnemyDefenseReach(pHouse, cell, SiegeScreenMarginCells); };
		const CellStruct here = pUnit->GetMapCoords();
		const bool hereInReach = inReach(here);

		if (!hereInReach && !inReach(goal))
			return nullptr;

		// The point to wait at: the last cell of the straight way to the goal out of every known reach,
		// or, from inside one, the first cell out of it on the way back.
		const CellStruct to = hereInReach ? back : goal;
		const int dx = to.X - here.X;
		const int dy = to.Y - here.Y;
		const int steps = std::max(std::abs(dx), std::abs(dy));
		CellStruct point = hereInReach ? CellStruct::Empty : here;

		for (int i = 1; i <= steps && (!hereInReach || i <= SiegeScreenBackCells); ++i)
		{
			const CellStruct cell { static_cast<short>(here.X + dx * i / steps), static_cast<short>(here.Y + dy * i / steps) };

			if (!hereInReach)
			{
				if (inReach(cell))
					break;

				point = cell;
			}
			else if (!inReach(cell))
			{
				point = cell;
				break;
			}
		}

		if (point == CellStruct::Empty)
			return nullptr;

		// The transports of ours on the ground (user: the screen must not hinder the transports of the
		// units it guards).
		std::vector<CellStruct> transports;

		for (auto const pOther : UnitClass::Array)
		{
			if (pOther && pOther->Owner == pHouse && pOther->IsAlive && !pOther->InLimbo && pOther->Type && !pOther->IsInAir()
				&& (IsAmphibiousOrAirTransport(pOther->Type) || (TechnoTypeExt::Fetch(pOther->Type) && TechnoTypeExt::Fetch(pOther->Type)->Carryall)
					|| (pOther->Type->Passengers > 0 && !IsMainBattleTankType(pOther->Type))))
			{
				transports.push_back(pOther->GetMapCoords());
			}
		}

		const int zone = MapClass::Instance.GetMovementZoneType(here, pType->MovementZone, pUnit->OnBridge);

		NeuralWaitAsker asker {};
		asker.Speed = pType->SpeedType;
		asker.Movement = pType->MovementZone;
		asker.Zone = zone;
		NeuralWaitSearch search {};
		search.MaxRing = SiegeScreenRingCells;

		return FindWaitCellNear(point, search, asker,
			WaitLegal | WaitNoBuilding | WaitOffDeck | WaitOffMouth | WaitZone | WaitOffNarrow,
			[&](const CellStruct& cell, const CellClass*)
			{
				return inReach(cell) || std::any_of(transports.begin(), transports.end(),
					[&cell](const CellStruct& at) { return at.DistanceFrom(cell) <= SiegeScreenTransportCells; });
			});
	}

	// Siege screen (SiegeScreen*): the cell a main battle tank of an archetype 7 house, marching with
	// guns centred on gunsCenter, waits on instead of going on to goal. nullptr: no screen for it (no
	// gun, no known defense covering its goal or its cell, no cell to wait on): it goes on.
	CellClass* SiegeScreenHoldCell(HouseClass* pHouse, FootClass* pFoot, const CellStruct& goal, const CellStruct& gunsCenter)
	{
		auto const pUnit = abstract_cast<UnitClass*>(pFoot);
		auto const pExt = pHouse ? HouseExt::Fetch(pHouse) : nullptr;

		if (!pUnit || !pExt || pExt->NeuralAI_Archetype != SiegeScreenArchetype || !pUnit->Type || pUnit->Type->Naval || pUnit->InLimbo
			|| !IsMainBattleTankType(pUnit->Type) || gunsCenter == CellStruct::Empty || goal == CellStruct::Empty
			|| IsInTransportOp(pHouse, pUnit->UniqueID) || IsHeldByTransportOp(pUnit))
		{
			return nullptr;
		}

		return DefenseReachWaitCell(pHouse, pUnit, goal, gunsCenter);
	}

	// Gives a main battle tank its siege screen order (SiegeScreenHoldCell). True when the screen holds
	// it: the group's own order is not for it this time.
	bool HoldSiegeScreen(HouseClass* pHouse, FootClass* pFoot, const CellStruct& goal, const CellStruct& gunsCenter)
	{
		auto const pHold = SiegeScreenHoldCell(pHouse, pFoot, goal, gunsCenter);

		if (!pHold)
			return false;

		// Fighting a unit that came at it: it goes on with that.
		if (pFoot->Target && pFoot->Target->WhatAmI() != AbstractType::Building)
			return true;

		auto& macro = GetMacroState(pHouse);
		const double away = pFoot->GetMapCoords().DistanceFrom(pHold->MapCoords);

		if (away > SiegeScreenArrivedCells)
		{
			OrderFootToAttackMove(pFoot, pHold);
		}
		else if (pFoot->Target || pFoot->Destination || pFoot->MegaDestination)
		{
			pFoot->SetTarget(nullptr);
			pFoot->SetDestination(nullptr, true);
			pFoot->ClearMegaMissionData();
			pFoot->QueueMission(Mission::Guard, false);
		}

		// TEMP diagnostic: the siege screen's wait orders (SiegeScreenLogLines per house and match).
		if (macro.SiegeScreenLogCount < SiegeScreenLogLines)
		{
			macro.SiegeScreenLogCount++;
			Debug::Log("[NeuralAI] Frame %d | House #%d siege screen: %s waits at (%d,%d), %d cells from it, its goal (%d,%d) %d cells away, guns at (%d,%d)\n",
				Unsorted::CurrentFrame, pHouse->ArrayIndex, pFoot->GetTechnoType()->ID, pHold->MapCoords.X, pHold->MapCoords.Y,
				static_cast<int>(away), goal.X, goal.Y, static_cast<int>(pFoot->GetMapCoords().DistanceFrom(goal)), gunsCenter.X, gunsCenter.Y);
		}

		return true;
	}

	// Per-tick cache of the enemy buildings a house knows (alive, not ours/allied/neutral, seen),
	// rebuilt once per frame and house (and right after each AI tick's fog scan, see
	// InvalidateKnownEnemyBuildings), so the many "known defenses near X" queries of the plans,
	// waves and air lanes stop scanning the whole building array. Kept in BuildingClass::Array
	// order: every answer is identical to a direct scan (no desync).
	struct KnownEnemyBuildingInfo
	{
		BuildingClass* Building;
		CellStruct Coords;
		bool IsDefense;
		bool IsAA;
	};

	struct KnownEnemyBuildingCache
	{
		int Frame { -1 };
		std::vector<KnownEnemyBuildingInfo> Entries {};
		std::vector<BuildingClass*> Buildings {};
	};

	std::array<KnownEnemyBuildingCache, 32> KnownEnemyBuildingCaches {};

	KnownEnemyBuildingCache& GetKnownEnemyBuildingCache(HouseClass* pHouse)
	{
		auto& cache = KnownEnemyBuildingCaches[std::clamp(pHouse ? pHouse->ArrayIndex : 0, 0, 31)];

		if (cache.Frame != Unsorted::CurrentFrame)
		{
			cache.Frame = Unsorted::CurrentFrame;
			cache.Entries.clear();
			cache.Buildings.clear();

			for (auto const pBld : BuildingClass::Array)
			{
				if (!pHouse || !pBld || pBld->InLimbo || !pBld->IsAlive || !pBld->Type || !pBld->Owner || pBld->Owner == pHouse
					|| pHouse->IsAlliedWith(pBld->Owner) || pBld->Owner->IsNeutral() || !IsEnemyBuildingKnown(pHouse, pBld))
				{
					continue;
				}

				cache.Entries.push_back({ pBld, pBld->GetMapCoords(), pBld->Type->IsBaseDefense, GetCombatValueOfType(pBld->Type, true) > 0 });
				cache.Buildings.push_back(pBld);
			}
		}

		return cache;
	}

	// A wave march still under way (user, 2026-11-19: «a menos que termine su misión no debería estorbar
	// una misión en curso»; «el enemigo principal no debería forzar el cambio de misión a medio ataque a
	// menos que haya aparecido un pacto»): the goal a wave gave this unit (WaveGoals) while the unit is
	// farther than WaveGoalKeepCells from it, a known building of a house still at war with us stands
	// within that distance of the goal (no ally, truce partner or defeated house), the goal's path is not
	// dead and the unit has not stood still WaveMarchHeldFrames (a blocked way). No time limit but the
	// safety WaveMarchMostFrames: before, the goal went after WaveGoalKeepFrames (2700) and the next wave,
	// to the new main enemy, turned the unit round (match #11501: the main enemy of house 7 changed 21
	// times between houses 6, 4 and 2 and its LTNK went back and forth between (255,217) and (175,268)).
	// Whether a goal stands is read once per goal, frame and house.
	bool IsOnWaveMarch(HouseClass* pHouse, const FootClass* pFoot, CellStruct* pGoal = nullptr, int* pZone = nullptr)
	{
		if (!pHouse || !pFoot)
			return false;

		auto& goals = GetMacroState(pHouse).WaveGoals;
		auto const it = goals.find(pFoot->UniqueID);
		const int now = Unsorted::CurrentFrame;

		if (it == goals.end() || now < it->second.Frame || now - it->second.Frame > WaveMarchMostFrames)
			return false;

		const CellStruct goal = it->second.Cell;
		const double distance = pFoot->GetMapCoords().DistanceFrom(goal);

		if (goal == CellStruct::Empty || distance <= WaveGoalKeepCells || IsFootHeldStill(pHouse, pFoot, WaveMarchHeldFrames))
			return false;

		// Progress: WaveMarchProgressCells nearer than the best so far renews it; none in
		// WaveMarchStallFrames ends the march (a way it cannot get through).
		auto& march = it->second;

		if (march.BestDistance < 0.0 || distance <= march.BestDistance - WaveMarchProgressCells)
		{
			march.BestDistance = distance;
			march.BestFrame = now;
		}
		else if (now - march.BestFrame > WaveMarchStallFrames)
		{
			return false;
		}

		static int standsFrame = -1;
		static int standsHouse = -1;
		static std::vector<std::pair<CellStruct, bool>> stands;

		if (standsFrame != now || standsHouse != pHouse->ArrayIndex)
		{
			standsFrame = now;
			standsHouse = pHouse->ArrayIndex;
			stands.clear();
		}

		auto known = std::find_if(stands.begin(), stands.end(), [&goal](auto const& entry) { return entry.first == goal; });

		if (known == stands.end())
		{
			bool enemyThere = false;

			for (auto const& info : GetKnownEnemyBuildingCache(pHouse).Entries)
			{
				auto const pOwner = info.Building ? info.Building->Owner : nullptr;

				if (pOwner && !pOwner->Defeated && info.Coords.DistanceFrom(goal) <= WaveGoalKeepCells
					&& !IsTacitTrucePartner(pHouse, pOwner->ArrayIndex))
				{
					enemyThere = true;
					break;
				}
			}

			// A goal our units find no path to is given up at once.
			stands.push_back({ goal, enemyThere && !IsPathDeadGoal(pHouse, goal) });
			known = stands.end() - 1;
		}

		if (!known->second)
			return false;

		if (pGoal)
			*pGoal = goal;

		if (pZone)
			*pZone = it->second.Zone;

		return true;
	}

	// Enemy air presence a wave must expect: aircraft of anyone seen lately (the air threat
	// memory), or a known airfield of the house it goes for (of any enemy house with owner -1).
	bool EnemyAirPresenceKnown(HouseClass* pHouse, int owner)
	{
		if (GetMacroState(pHouse).AirThreatValue > 0)
			return true;

		for (auto const& info : GetKnownEnemyBuildingCache(pHouse).Entries)
		{
			if (info.Building->Type->Factory == AbstractType::AircraftType
				&& (owner < 0 || info.Building->Owner->ArrayIndex == owner))
			{
				return true;
			}
		}

		return false;
	}

	// Whether the house can build now something for the ground army that shoots at aircraft: a
	// vehicle with a war factory of ours, or a soldier with a barracks.
	bool CanBuildAntiAirEscort(HouseClass* pHouse)
	{
		if (pHouse->NumWarFactories > 0)
		{
			for (auto const pType : UnitTypeClass::Array)
			{
				if (pType && !pType->Naval && ShootsAtAircraft(pType) && CanBuildUnderHumanRules(pHouse, pType))
					return true;
			}
		}

		if (pHouse->NumBarracks > 0)
		{
			for (auto const pType : InfantryTypeClass::Array)
			{
				if (pType && ShootsAtAircraft(pType) && CanBuildUnderHumanRules(pHouse, pType))
					return true;
			}
		}

		return false;
	}

	void InvalidateKnownEnemyBuildings(HouseClass* pHouse)
	{
		KnownEnemyBuildingCaches[std::clamp(pHouse ? pHouse->ArrayIndex : 0, 0, 31)].Frame = -1;
	}

	bool MoveTransportToLoadingCell(HouseClass* pHouse, UnitClass* pTransport, const CellStruct& from, int landZone, bool passengersWaiting);

	int CountKnownEnemyDefensesNear(HouseClass* pHouse, const CellStruct& cell, double radius, bool antiAirOnly)
	{
		int count = 0;

		for (auto const& info : GetKnownEnemyBuildingCache(pHouse).Entries)
		{
			if ((antiAirOnly ? info.IsAA : info.IsDefense) && info.Coords.DistanceFrom(cell) <= radius)
				count++;
		}

		return count;
	}

	// Should a loaded carrier on its way to pDrop turn back home? Yes when it is badly damaged
	// and still far away, or when the drop zone itself is now covered by known/visible anti-air.
	bool ShouldAbortAirDrop(HouseClass* pHouse, FootClass* pCarrier, CellClass* pDrop)
	{
		if (!pHouse || !pCarrier || !pDrop)
			return false;

		const double distToDrop = pCarrier->GetMapCoords().DistanceFrom(pDrop->MapCoords);

		if (distToDrop <= 6.0)
			return false; // Committed: dropping now beats turning around under fire.

		if (pCarrier->GetHealthPercentage() < 0.35 && distToDrop > 12.0)
			return true;

		return CountKnownEnemyDefensesNear(pHouse, pDrop->MapCoords, 8.0, true)
			+ CountVisibleEnemyMobileAANear(pHouse, pDrop->MapCoords, 8.0) >= 3;
	}

	// Where a loaded carrier should actually drop: pDrop normally; home (FindCarrierHomeCell)
	// for 1800 frames once the drop was aborted (ShouldAbortAirDrop), so the cargo is brought
	// back and unloaded at the base instead of bouncing between the two goals.
	CellClass* ResolveAirDropCell(HouseClass* pHouse, FootClass* pCarrier, CellClass* pDrop)
	{
		if (!pHouse || !pCarrier || !pDrop)
			return pDrop;

		auto const it = AirDropAborts.find(pCarrier->UniqueID);

		if (it != AirDropAborts.end())
		{
			const int age = Unsorted::CurrentFrame - it->second;

			if (age >= 0 && age <= 1800)
			{
				auto const pHome = FindCarrierHomeCell(pHouse, pCarrier);
				return pHome ? pHome : pDrop;
			}

			AirDropAborts.erase(it);
		}

		if (!ShouldAbortAirDrop(pHouse, pCarrier, pDrop))
			return pDrop;

		auto const pHome = FindCarrierHomeCell(pHouse, pCarrier);

		if (!pHome || pHome->MapCoords.DistanceFrom(pDrop->MapCoords) <= 12.0)
			return pDrop;

		if (AirDropAborts.size() > 256)
			AirDropAborts.clear();

		AirDropAborts[pCarrier->UniqueID] = Unsorted::CurrentFrame;
		GetMacroState(pHouse).LastAirDropAbortFrame = Unsorted::CurrentFrame;
		CountDecision(NeuralDecision::DropAbort, pHouse->ArrayIndex);

		return pHome;
	}

	// Idle flyer strike: an armed flying unit that is not an aircraft (airship, jumpjet bomber, no
	// ammo / airpad cycle) idling with no target and no plan attacks the best known enemy structure
	// (valuable types first, fewer known anti-air, closer; within 30 cells first, else anywhere)
	// instead of hovering in place. Every 150 frames per house.
	// A transport that stays in Mission::Unload on the same cell for 450 frames cannot drop its
	// cargo there (a jumpjet that cannot settle, a blocked or steep cell): empty, it is just put on
	// guard; loaded, it moves to the nearest free cell within 6 where it can settle and its cargo
	// can stand (no building, no object, land, clear for both), and unloads again from there. Every
	// 150 frames per house.
	void UpdateStuckUnloads(HouseClass* pHouse)
	{
		if (!pHouse || !IsAITickWindow(150, 15))
			return;

		auto& macro = GetMacroState(pHouse);
		const int now = Unsorted::CurrentFrame;
		const int unloadPrevious = BeginClockSample(macro, NeuralClock::Unload);

		auto const handle = [&](FootClass* pTrans)
		{
			auto const pType = pTrans ? pTrans->GetTechnoType() : nullptr;

			if (!pType || pType->Passengers <= 0 || pTrans->Owner != pHouse || !pTrans->IsAlive || pTrans->InLimbo
				|| pTrans->CurrentMission != Mission::Unload)
			{
				return;
			}

			const CellStruct here = pTrans->GetMapCoords();
			NeuralClockState watch { here, now };

			if (auto const pCarried = CarriedClock(macro, pTrans->UniqueID, NeuralClock::Unload, unloadPrevious); pCarried && pCarried->Anchor == here)
				watch = *pCarried;

			KeepClock(macro, pTrans->UniqueID, NeuralClock::Unload, watch);

			if (now - watch.Since < 450)
				return;

			KeepClock(macro, pTrans->UniqueID, NeuralClock::Unload, { here, now });

			if (pTrans->Passengers.NumPassengers <= 0)
			{
				pTrans->ForceMission(Mission::Guard);
				return;
			}

			auto const pCargoType = pTrans->Passengers.GetFirstPassenger()
				? pTrans->Passengers.GetFirstPassenger()->GetTechnoType()
				: nullptr;

			for (int r = 1; r <= 6; ++r)
			{
				for (int dy = -r; dy <= r; ++dy)
				{
					for (int dx = -r; dx <= r; ++dx)
					{
						if (std::abs(dx) != r && std::abs(dy) != r)
							continue;

						const CellStruct c { static_cast<short>(here.X + dx), static_cast<short>(here.Y + dy) };
						auto const pCell = MapClass::Instance.TryGetCellAt(c);

						if (!pCell || pCell->GetBuilding() || pCell->FirstObject || pCell->LandType == LandType::Water
							|| pCell->LandType == LandType::Rock || pCell->LandType == LandType::Wall
							|| !pCell->IsClearToMove(pType->SpeedType, false, false, -1, pType->MovementZone, -1, false)
							|| (pCargoType && !pCell->IsClearToMove(pCargoType->SpeedType, false, false, -1, pCargoType->MovementZone, -1, false)))
						{
							continue;
						}

						pTrans->SetDestination(pCell, true);
						pTrans->QueueMission(Mission::Move, true);
						NoteOrderCaller(pTrans, "move");
						CountDecision(NeuralDecision::StuckUnloadMoved, pHouse->ArrayIndex);

						return;
					}
				}
			}
		};

		for (auto const pUnit : UnitClass::Array)
			handle(pUnit);

		for (auto const pAir : AircraftClass::Array)
			handle(pAir);
	}

	void UpdateIdleAirborneStrikers(HouseClass* pHouse)
	{
		if (!pHouse)
			return;

		auto& macro = GetMacroState(pHouse);
		const int now = Unsorted::CurrentFrame;

		if (now - macro.LastIdleFlyerCheckFrame < 150)
			return;

		macro.LastIdleFlyerCheckFrame = now;
		auto const& knownBuildings = GetKnownEnemyBuildingCache(pHouse).Buildings;

		if (knownBuildings.empty())
			return;

		for (auto const pUnit : UnitClass::Array)
		{
			if (!pUnit || pUnit->Owner != pHouse || !pUnit->IsAlive || pUnit->InLimbo || !pUnit->Type
				|| !IsAirborneThreatTechno(pUnit) || GroundRangeOf(pUnit->Type) <= 0 || pUnit->Target != nullptr
				|| macro.PlanMembers.contains(pUnit->UniqueID) || macro.CapitalHeld.contains(pUnit->UniqueID)
				|| IsInTransportOp(pHouse, pUnit->UniqueID) || pUnit->GetHealthPercentage() < 0.25)
			{
				continue;
			}

			// Armed transports and carryalls (a transport helicopter, a Nighthawk) ferry first: they only
			// strike while empty (no passengers, nothing slung), with nobody waiting for a ferry (no fresh
			// demand) and only targets within 30 cells. A far strike left them
			// targeting it while the transport logic kept them home (Move / Unload), doing neither
			// (matches #6164, #6176).
			// Parked empty on Unload counts as idle for them.
			auto const pStrikerTypeExt = TechnoTypeExt::Fetch(pUnit->Type);
			const bool armedTransport = IsAmphibiousOrAirTransport(pUnit->Type) || (pStrikerTypeExt && pStrikerTypeExt->Carryall);

			if (armedTransport)
			{
				auto const pCarryState = UnitExt::Fetch(pUnit);
				const bool empty = pUnit->Passengers.NumPassengers == 0
					&& (!pCarryState || (!pCarryState->CarryallCargo && pCarryState->CarryallState == JumpjetCarryallState::Idle));
				bool ferryWanted = false;

				if (macro.FerryDemandFrame >= 0 && now - macro.FerryDemandFrame <= 600 && now >= macro.FerryDemandFrame)
				{
					for (const int demand : macro.FerryDemandSizeBySize)
						ferryWanted = ferryWanted || demand > 0;
				}

				if (!empty || ferryWanted)
					continue;
			}

			const bool parkedAfterMove = pUnit->CurrentMission == Mission::Move
				&& (!pUnit->Destination || pUnit->DistanceFrom(pUnit->Destination) <= 384);

			if (pUnit->CurrentMission != Mission::Guard && pUnit->CurrentMission != Mission::Area_Guard
				&& pUnit->CurrentMission != Mission::Sleep && pUnit->CurrentMission != Mission::None && !parkedAfterMove
				&& !(armedTransport && pUnit->CurrentMission == Mission::Unload && !pUnit->Destination))
			{
				continue;
			}

			const CellStruct here = pUnit->GetMapCoords();
			BuildingClass* pBest = nullptr;
			int bestScore = INT_MIN;

			for (auto const pBld : knownBuildings)
			{
				if (!pBld || !pBld->IsAlive || pBld->InLimbo || !pBld->Type || !pBld->Owner || pBld->Owner == pHouse
					|| pHouse->IsAlliedWith(pBld->Owner) || pBld->Owner->IsNeutral() || pBld->Type->Insignificant
					|| pBld->Type->InvisibleInGame || pBld->Type->Wall)
				{
					continue;
				}

				const CellStruct bc = pBld->GetMapCoords();
				const int dist = std::max(std::abs(bc.X - here.X), std::abs(bc.Y - here.Y));

				if (armedTransport && dist > 30)
					continue;

				auto const pT = pBld->Type;
				const int worth = (pT->SuperWeapon != -1) ? 400
					: (pT->ConstructionYard || pT->Factory != AbstractType::None) ? 300
					: (pT->PowerBonus > 0) ? 280
					: (pT->Refinery || pT->ResourceDestination) ? 260
					: std::min(200, std::max(0, pT->Cost) / 10);
				// Beyond 30 cells only when nothing nearer is known: those come first.
				const int score = worth - CountKnownEnemyDefensesNear(pHouse, bc, 8.0, true) * 150 - dist * 4
					- (dist > 30 ? 1000 : 0);

				if (score > bestScore)
				{
					bestScore = score;
					pBest = pBld;
				}
			}

			if (pBest)
			{
				pUnit->SetTarget(pBest);
				pUnit->QueueMission(Mission::Attack, true);
				CountDecision(NeuralDecision::IdleFlyerStrike, pHouse->ArrayIndex);
			}
		}
	}

	// May a plan aircraft be given a strike order now? Only one that rests (Guard, Area Guard,
	// Sleep). An aircraft back on its pad keeps the target of its last strike, and with a target
	// the engine neither reloads it nor sends it again: it slept there until its plan ran out of
	// time (59 Superweapon Race and 45 Tech Tree Cut plans of the focus batch of 2026-11-13 ended
	// with every aircraft on Sleep, 100 cells and more from the target). Empty, or on its dock
	// with less than its full load, it drops the target so it reloads, and is not ready; armed,
	// it drops it so the new order is a new one.
	bool ReadyPlanAircraft(AircraftClass* pAir)
	{
		if (!pAir || (pAir->CurrentMission != Mission::Guard && pAir->CurrentMission != Mission::Area_Guard
			&& pAir->CurrentMission != Mission::Sleep))
		{
			return false;
		}

		if ((pAir->Ammo == 0 && pAir->Type->Ammo >= 0) || IsAircraftReloadingOnDock(pAir))
		{
			if (pAir->Target)
			{
				pAir->SetTarget(nullptr);
				pAir->SetDestination(nullptr, true);

				if (!pAir->IsInAir())
					pAir->EnterIdleMode(false, true);
			}

			return false;
		}

		if (pAir->Target)
			pAir->SetTarget(nullptr);

		return true;
	}

	BuildingClass* FindBuildingByID(DWORD uid)
	{
		for (auto const pBld : BuildingClass::Array)
		{
			if (pBld && pBld->UniqueID == uid && pBld->IsAlive && !pBld->InLimbo)
				return pBld;
		}

		return nullptr;
	}

	// Capital units: the late-game heavyweights that work in numbers (airships, super-heavy tanks,
	// carriers, battleships, missile subs...). By rules only: an armed combat vehicle costing 1500+
	// that is also tough (Strength 600+), long-ranged (10+ cells against the ground) or launches
	// spawns; never a harvester, a transport or a deployer into a base structure.
	bool IsCapitalUnitType(UnitTypeClass* pType)
	{
		if (!pType || pType->Cost < 1500 || pType->Harvester || pType->ResourceGatherer || IsAmphibiousOrAirTransport(pType)
			|| (pType->DeploysInto && !IsDeployableSiegeUnitType(pType)))
		{
			return false;
		}

		const int range = GroundRangeOf(pType);
		const bool spawns = pType->Spawns != nullptr;

		return (range > 0 || spawns) && (pType->Strength >= 600 || range >= 10 * Unsorted::LeptonsPerCell || spawns);
	}

	// 0 ground, 1 air (airships, jumpjets), 2 sea.
	int CapitalDomainOf(UnitTypeClass* pType)
	{
		if (pType->Naval)
			return 2;

		return (pType->JumpJet || pType->MovementZone == MovementZone::Fly || pType->ConsideredAircraft) ? 1 : 0;
	}

	// Defined in NeuralAI.Scouting.inl: a unit on a scouting trip of this house.
	bool IsActiveScout(HouseClass* pHouse, DWORD uid);

	// Tank guard (TankGuard*): part of the main battle tanks guards our sites and hunts the enemy
	// siege near them instead of leaving with the waves.
	void UpdateTankGuard(HouseClass* pHouse)
	{
		if (!pHouse || !IsAITickWindow(TankGuardCheckFrames, 45))
			return;

		auto& macro = GetMacroState(pHouse);
		const int now = Unsorted::CurrentFrame;
		const std::unordered_set<DWORD> before = macro.TankGuard;
		macro.TankGuard.clear();
		macro.TankGuardEscorts.clear();

		// While the base is under attack the guard is free for its defense.
		if (pHouse->LATime > 0 && now - pHouse->LATime < TankGuardBaseAttackFrames)
			return;

		// Our sites: construction yards, refineries, factories, one per TankGuardPostCells.
		std::vector<CellStruct> sites;

		for (auto const pBld : pHouse->Buildings)
		{
			if (!pBld || !pBld->IsAlive || pBld->InLimbo || !pBld->Type
				|| (!pBld->Type->ConstructionYard && !pBld->Type->Refinery && pBld->Type->Factory == AbstractType::None))
			{
				continue;
			}

			const CellStruct at = pBld->GetMapCoords();

			if (std::none_of(sites.begin(), sites.end(), [&at](const CellStruct& site) { return site.DistanceFrom(at) <= TankGuardPostCells * 2; }))
				sites.push_back(at);
		}

		if (sites.empty())
			return;

		auto const nearestSite = [&sites](const CellStruct& cell, size_t* pIndex = nullptr)
		{
			double best = 1e9;

			for (size_t i = 0; i < sites.size(); ++i)
			{
				const double distance = sites[i].DistanceFrom(cell);

				if (distance < best)
				{
					best = distance;

					if (pIndex)
						*pIndex = i;
				}
			}

			return best;
		};

		// The main battle tanks free for the guard, those that guarded already first, then the nearest
		// to a site.
		std::vector<std::pair<double, UnitClass*>> candidates;
		int mainTanks = 0;

		for (auto const pUnit : UnitClass::Array)
		{
			if (!pUnit || pUnit->Owner != pHouse || !pUnit->IsAlive || pUnit->InLimbo || !pUnit->Type || !IsMainBattleTankType(pUnit->Type))
				continue;

			++mainTanks;
			const DWORD uid = pUnit->UniqueID;

			if (macro.PlanMembers.contains(uid) || macro.CapitalHeld.contains(uid) || IsInTransportOp(pHouse, uid) || IsOnRepairTrip(pHouse, uid)
				|| IsActiveScout(pHouse, uid) || pUnit->GetHealthPercentage() < 0.4)
			{
				continue;
			}

			const double distance = nearestSite(pUnit->GetMapCoords());

			if (distance <= TankGuardSiteCells)
				candidates.push_back({ before.contains(uid) ? -1.0 : distance, pUnit });
		}

		// The wide guard (TankGuardWideArchetype): the floor's base part and escort part. Out of an
		// attack on the base (above) the part that stays home is at most a TankGuardPeaceShare-th of
		// the main battle tanks (match #11465: 16 of 39 tanks guarded a base nobody attacked).
		const int peaceGuard = std::max(std::min(TankGuardMin, mainTanks), mainTanks / TankGuardPeaceShare);
		const bool wide = TankGuardWideArchetype(HouseExt::Fetch(pHouse)->NeuralAI_Archetype) && macro.TankFloorBasePart > 0;
		const int basePart = std::min(macro.TankFloorBasePart, peaceGuard);
		const int wanted = wide
			? std::min(std::min(TankFloorMost, basePart + macro.TankFloorEscortPart), static_cast<int>(candidates.size()))
			: std::min({ TankGuardMost, std::max(std::min(TankGuardMin, mainTanks), mainTanks / TankGuardShare), peaceGuard,
				static_cast<int>(candidates.size()) });

		if (wanted <= 0)
		{
			macro.TankGuardPosts.clear();
			return;
		}

		std::stable_sort(candidates.begin(), candidates.end(), [](const auto& a, const auto& b)
			{ return a.first < b.first || (a.first == b.first && a.second->UniqueID < b.second->UniqueID); });
		candidates.resize(static_cast<size_t>(wanted));

		for (size_t i = 0; i < candidates.size(); ++i)
		{
			macro.TankGuard.insert(candidates[i].second->UniqueID);

			if (wide && static_cast<int>(i) >= basePart)
				macro.TankGuardEscorts.insert(candidates[i].second->UniqueID);
		}

		std::erase_if(macro.TankGuardPosts, [&macro](const auto& entry) { return !macro.TankGuard.contains(entry.first); });

		// A known enemy siege unit near one of our sites, the nearest to a site first.
		TechnoClass* pSiege = nullptr;
		double siegeDistance = TankGuardSiegeCells;

		for (auto const pTech : TechnoClass::Array)
		{
			if (!pTech || !pTech->IsAlive || pTech->InLimbo || !pTech->Owner || pTech->Owner == pHouse || pHouse->IsAlliedWith(pTech->Owner)
				|| pTech->Owner->IsNeutral() || pTech->CloakState == CloakState::Cloaked || pTech->IsInAir())
			{
				continue;
			}

			bool siege = false;

			if (auto const pBld = abstract_cast<BuildingClass*>(pTech))
			{
				siege = IsDeployedSiegeBuildingType(pBld->Type);
			}
			else if (auto const pEnemyUnit = abstract_cast<UnitClass*>(pTech))
			{
				siege = !pEnemyUnit->Type->Naval && !IsMainBattleTankType(pEnemyUnit->Type)
					&& GroundRangeOf(pEnemyUnit->Type) >= ArtilleryRangeCells * Unsorted::LeptonsPerCell;
			}

			if (!siege || !IsCellKnownToAI(pHouse, pTech->GetMapCoords()))
				continue;

			const double distance = nearestSite(pTech->GetMapCoords());

			if (distance <= siegeDistance)
			{
				siegeDistance = distance;
				pSiege = pTech;
			}
		}

		int hunting = 0;
		int moved = 0;

		for (auto const& [distance, pUnit] : candidates)
		{
			if (pSiege)
			{
				if (pUnit->Target != pSiege)
				{
					OrderFootAttack(pUnit, pSiege);
					++moved;
				}

				++hunting;
				continue;
			}

			// Fighting already: it goes on.
			if (pUnit->Target || pUnit->IsFiring)
				continue;

			auto& [post, since] = macro.TankGuardPosts.try_emplace(pUnit->UniqueID,
				std::pair<int, int> { static_cast<int>(pUnit->UniqueID % sites.size()), now }).first->second;

			if (now - since >= TankGuardPostFrames || now < since || post >= static_cast<int>(sites.size()))
			{
				post = (post + 1) % static_cast<int>(sites.size());
				since = now;
			}

			const CellStruct postCell = sites[static_cast<size_t>(post)];
			const bool idle = pUnit->CurrentMission == Mission::Guard || pUnit->CurrentMission == Mission::Area_Guard
				|| pUnit->CurrentMission == Mission::Sleep;

			if (idle && pUnit->GetMapCoords().DistanceFrom(postCell) > TankGuardPostCells)
			{
				if (auto const pCell = MapClass::Instance.TryGetCellAt(postCell))
				{
					OrderFootToAttackMove(pUnit, pCell);
					++moved;
				}
			}
		}

		// TEMP diagnostic: the guard every 1800 frames per house.
		if (now - macro.TankGuardLogFrame >= 1800 || now < macro.TankGuardLogFrame)
		{
			macro.TankGuardLogFrame = now;
			Debug::Log("[NeuralAI] Frame %d | House #%d tank guard: %d of %d main battle tanks, %d sites, %s, %d orders, %s %d escorts (floor base %d, escorts %d)\n", now,
				pHouse->ArrayIndex, static_cast<int>(candidates.size()), mainTanks, static_cast<int>(sites.size()),
				pSiege ? (std::string("hunting ") + pSiege->GetTechnoType()->ID).c_str() : "patrolling", moved, wide ? "wide," : "half,",
				static_cast<int>(macro.TankGuardEscorts.size()), macro.TankFloorBasePart, macro.TankFloorEscortPart);
		}
	}

	// Capital groups: capital units are held back from waves and lone strikes until their domain
	// has 3 (2 with an offensive bank) at the rally point (our first ConYard, else base center),
	// then go together at one target: the best known enemy building their domain can reach (air:
	// anywhere, fewer known anti-air first; ground: their movement zone; sea: a water cell of their
	// sea within 8 cells), valuable types first, nearer first. Airships and long-range members
	// (8+ cells) attack it directly, so the engine stops them at range; the rest attack-move. The
	// group takes the next target when one falls and pulls back to the rally point once it is down
	// to 40% of its starting value; capital units built meanwhile gather for the next group.
	// Units in strategic plans or on repair trips are left out. Every 90 frames.
	void UpdateCapitalGroups(HouseClass* pHouse)
	{
		if (!pHouse || !IsAITickWindow(90, 15))
			return;

		auto& macro = GetMacroState(pHouse);
		macro.CapitalHeld.clear();

		CellStruct home = pHouse->BaseCenter;

		if (pHouse->ConYards.Count > 0 && pHouse->ConYards.GetItem(0))
			home = pHouse->ConYards.GetItem(0)->GetMapCoords();

		auto const pRally = home != CellStruct::Empty ? MapClass::Instance.TryGetCellAt(home) : nullptr;

		if (!pRally)
			return;

		const size_t need = HasOffensiveBank(pHouse) ? 2 : 3;
		std::array<std::vector<UnitClass*>, 3> pool;

		for (auto const pUnit : UnitClass::Array)
		{
			// A capital unit a transport operation carries joins a group once it is handed back.
			if (pUnit && pUnit->Owner == pHouse && pUnit->IsAlive && !pUnit->InLimbo && IsCapitalUnitType(pUnit->Type)
				&& !macro.PlanMembers.contains(pUnit->UniqueID) && !IsOnRepairTrip(pHouse, pUnit->UniqueID)
				&& !IsInTransportOp(pHouse, pUnit->UniqueID))
			{
				pool[static_cast<size_t>(CapitalDomainOf(pUnit->Type))].push_back(pUnit);
			}
		}

		auto const worthOf = [](BuildingTypeClass* pT)
		{
			return (pT->SuperWeapon != -1) ? 400
				: (pT->ConstructionYard || pT->Factory != AbstractType::None) ? 300
				: (pT->PowerBonus > 0) ? 280
				: (pT->Refinery || pT->ResourceDestination) ? 260
				: (pT->IsBaseDefense ? 220 : std::min(200, std::max(0, pT->Cost) / 10));
		};

		auto const pickTarget = [&](int domain, const std::vector<UnitClass*>& group) -> BuildingClass*
		{
			auto const pLead = group.front();
			const CellStruct from = pLead->GetMapCoords();
			const MovementZone zone = pLead->Type->MovementZone;
			const int leadZone = ObjectZone(pLead, zone);
			int maxRangeCells = 0;

			for (auto const pUnit : group)
				maxRangeCells = std::max(maxRangeCells, SiegeRangeOf(pUnit->Type) / Unsorted::LeptonsPerCell);

			BuildingClass* pBest = nullptr;
			int bestScore = INT_MIN;

			for (auto const pBld : GetKnownEnemyBuildingCache(pHouse).Buildings)
			{
				if (!pBld || !pBld->IsAlive || pBld->InLimbo || !pBld->Type || !pBld->Owner || pBld->Owner == pHouse
					|| pHouse->IsAlliedWith(pBld->Owner) || pBld->Owner->IsNeutral() || pBld->Type->Insignificant
					|| pBld->Type->InvisibleInGame || pBld->Type->Wall)
				{
					continue;
				}

				const CellStruct bc = pBld->GetMapCoords();
				bool reachable = domain == 1;

				for (int r = 0; r <= (domain == 2 ? std::min(8, std::max(2, maxRangeCells - 1)) : 2) && !reachable; r += (domain == 2 ? 2 : 1))
				{
					for (int dy = -r; dy <= r && !reachable; ++dy)
					{
						for (int dx = -r; dx <= r && !reachable; ++dx)
						{
							if (std::abs(dx) != r && std::abs(dy) != r)
								continue;

							const CellStruct c { static_cast<short>(bc.X + dx), static_cast<short>(bc.Y + dy) };

							if (MapClass::Instance.CoordinatesLegal(c) && leadZone > 1
								&& MapClass::Instance.GetMovementZoneType(c, zone, false) == leadZone)
							{
								reachable = true;
							}
						}
					}
				}

				if (!reachable)
					continue;

				const int dist = static_cast<int>(from.DistanceFrom(bc));
				const int score = worthOf(pBld->Type) - dist
					- CountKnownEnemyDefensesNear(pHouse, bc, 8.0, domain == 1) * (domain == 1 ? 150 : 40);

				if (score > bestScore || (score == bestScore && pBest && pBld->UniqueID < pBest->UniqueID))
				{
					bestScore = score;
					pBest = pBld;
				}
			}

			return pBest;
		};

		auto const isIdle = [](UnitClass* pUnit)
		{
			return pUnit->Target == nullptr
				&& (pUnit->CurrentMission == Mission::Guard || pUnit->CurrentMission == Mission::Area_Guard
					|| pUnit->CurrentMission == Mission::Sleep || pUnit->CurrentMission == Mission::None);
		};

		auto const gather = [&](UnitClass* pUnit)
		{
			macro.CapitalHeld.insert(pUnit->UniqueID);

			if (isIdle(pUnit) && pUnit->GetMapCoords().DistanceFrom(home) > 8.0)
				IssueMoveOrder(pUnit, pRally, 6.0);
		};

		for (int domain = 0; domain < 3; ++domain)
		{
			auto& group = macro.CapitalGroups[static_cast<size_t>(domain)];
			auto& units = pool[static_cast<size_t>(domain)];
			std::sort(units.begin(), units.end(), [](UnitClass* a, UnitClass* b) { return a->UniqueID < b->UniqueID; });

			std::vector<UnitClass*> attackers;
			std::vector<UnitClass*> others;

			for (auto const pUnit : units)
			{
				if (group.Attacking && std::find(group.Members.begin(), group.Members.end(), pUnit->UniqueID) != group.Members.end())
					attackers.push_back(pUnit);
				else
					others.push_back(pUnit);
			}

			int value = 0;

			for (auto const pUnit : attackers)
				value += GetCombatValueOfType(pUnit->Type, false);

			if (group.Attacking && (attackers.empty() || value * 100 < group.StartValue * 40))
			{
				group.Attacking = false;
				group.Members.clear();
				CountDecision(NeuralDecision::CapitalGroupRetreat, pHouse->ArrayIndex);

				for (auto const pUnit : attackers)
				{
					pUnit->SetTarget(nullptr);
					IssueMoveOrder(pUnit, pRally, 6.0);
				}

				others.insert(others.end(), attackers.begin(), attackers.end());
				attackers.clear();
			}

			// Launch a new group from the units gathered at the rally point.
			if (!group.Attacking)
			{
				std::vector<UnitClass*> ready;

				for (auto const pUnit : others)
				{
					if (pUnit->GetMapCoords().DistanceFrom(home) <= 12.0 && pUnit->GetHealthPercentage() >= 0.6)
						ready.push_back(pUnit);
				}

				if (ready.size() >= need)
				{
					if (auto const pTarget = pickTarget(domain, ready))
					{
						group.Attacking = true;
						group.TargetID = pTarget->UniqueID;
						group.Members.clear();
						group.StartValue = 0;

						for (auto const pUnit : ready)
						{
							group.Members.push_back(pUnit->UniqueID);
							group.StartValue += GetCombatValueOfType(pUnit->Type, false);
							others.erase(std::find(others.begin(), others.end(), pUnit));
						}

						attackers = ready;
						CountDecision(NeuralDecision::CapitalGroupLaunched, pHouse->ArrayIndex);
					}
				}
			}

			for (auto const pUnit : others)
				gather(pUnit);

			if (!group.Attacking)
				continue;

			auto pTarget = FindBuildingByID(group.TargetID);

			if (!pTarget)
			{
				pTarget = attackers.empty() ? nullptr : pickTarget(domain, attackers);

				if (!pTarget)
				{
					group.Attacking = false;
					group.Members.clear();

					for (auto const pUnit : attackers)
						gather(pUnit);

					continue;
				}

				group.TargetID = pTarget->UniqueID;
			}

			for (auto const pUnit : attackers)
			{
				macro.CapitalHeld.insert(pUnit->UniqueID);

				if (pUnit->Target == pTarget)
					continue;

				// Already fighting something in its reach on the way: let it finish.
				if (auto const pBusy = abstract_cast<TechnoClass*>(pUnit->Target);
					pBusy && pBusy->IsAlive && !pBusy->InLimbo && pUnit->DistanceFrom(pBusy) <= SiegeRangeOf(pUnit->Type))
				{
					continue;
				}

				if (domain == 1 || SiegeRangeOf(pUnit->Type) >= 8 * Unsorted::LeptonsPerCell)
				{
					pUnit->SetTarget(pTarget);
					pUnit->QueueMission(Mission::Attack, true);
				}
				else
				{
					OrderFootToAttackMove(pUnit, pTarget);
				}
			}
		}
	}

	// Our cloaked ships (submarines: Naval, Cloakable) keep out of the sonar we know of: known enemy
	// buildings with Sensors and enemy units with Sensors in our sight now (their SensorsSight + 1).
	// One that is not attacking and stands in such a reach moves to the nearest water cell of its
	// sea outside all of them (within 12); one sent to a destination inside a reach is sent to the
	// nearest cell outside it instead (within 8). Attacking subs are left alone. Every 60 frames.
	void UpdateSubmarineStealth(HouseClass* pHouse)
	{
		if (!pHouse || !IsAITickWindow(60, 15))
			return;

		struct Sonar
		{
			CellStruct Cell;
			int Range;
		};

		std::vector<Sonar> sonars;

		for (auto const pBld : GetKnownEnemyBuildingCache(pHouse).Buildings)
		{
			if (pBld && pBld->IsAlive && !pBld->InLimbo && pBld->Type && pBld->Type->Sensors && pBld->Type->SensorsSight > 0)
				sonars.push_back({ pBld->GetMapCoords(), pBld->Type->SensorsSight + 1 });
		}

		for (auto const pUnit : UnitClass::Array)
		{
			if (pUnit && pUnit->IsAlive && !pUnit->InLimbo && pUnit->Owner && pUnit->Owner != pHouse
				&& !pHouse->IsAlliedWith(pUnit->Owner) && !pUnit->Owner->IsNeutral() && pUnit->Type && pUnit->Type->Sensors
				&& pUnit->Type->SensorsSight > 0 && IsCellKnownToAI(pHouse, pUnit->GetMapCoords()))
			{
				sonars.push_back({ pUnit->GetMapCoords(), pUnit->Type->SensorsSight + 1 });
			}
		}

		if (sonars.empty())
			return;

		auto const inSonar = [&](const CellStruct& c)
		{
			for (auto const& sonar : sonars)
			{
				if (c.DistanceFrom(sonar.Cell) <= sonar.Range)
					return true;
			}

			return false;
		};

		auto const nearestHidden = [&](UnitClass* pSub, const CellStruct& from, int radius) -> CellClass*
		{
			const int zone = ObjectZone(pSub, pSub->Type->MovementZone);

			for (int r = 1; r <= radius; ++r)
			{
				for (int dy = -r; dy <= r; ++dy)
				{
					for (int dx = -r; dx <= r; ++dx)
					{
						if (std::abs(dx) != r && std::abs(dy) != r)
							continue;

						const CellStruct c { static_cast<short>(from.X + dx), static_cast<short>(from.Y + dy) };
						auto const pCell = MapClass::Instance.CoordinatesLegal(c) ? MapClass::Instance.TryGetCellAt(c) : nullptr;

						if (pCell && !inSonar(c) && MapClass::Instance.GetMovementZoneType(c, pSub->Type->MovementZone, false) == zone
							&& pCell->IsClearToMove(pSub->Type->SpeedType, false, false, -1, pSub->Type->MovementZone, -1, false))
						{
							return pCell;
						}
					}
				}
			}

			return nullptr;
		};

		// A sub with a job (a plan's member, an attack-move) keeps its destination: a sub that surfaces
		// to fire gains nothing by hiding, and moving a plan's firing cells to the sonar's edge kept
		// every naval strike of a house out of range until it timed out (match #11450 Rocky Surf,
		// 2026-11-19: Boomers held 27 cells off a Mobile Sensor Array, "arrived 0" in every plan).
		for (auto const pSub : UnitClass::Array)
		{
			if (!pSub || pSub->Owner != pHouse || !pSub->IsAlive || pSub->InLimbo || !pSub->Type || !pSub->Type->Naval
				|| !pSub->Type->Cloakable || pSub->Target != nullptr || pSub->CurrentMission == Mission::Attack
				|| pSub->CurrentMission == Mission::AttackMove || pSub->MegaMission == Mission::AttackMove
				|| IsPlanMember(pHouse, pSub->UniqueID))
			{
				continue;
			}

			const CellStruct here = pSub->GetMapCoords();
			auto const pDestCell = abstract_cast<CellClass*>(pSub->Destination);

			if (pDestCell && inSonar(pDestCell->MapCoords))
			{
				if (auto const pSafe = nearestHidden(pSub, pDestCell->MapCoords, 8))
				{
					pSub->SetDestination(pSafe, true);
					NoteOrderCaller(pSub, "move");
					CountDecision(NeuralDecision::SubmarineSonarAvoided, pHouse->ArrayIndex);
				}
			}
			else if (!pDestCell && inSonar(here))
			{
				if (auto const pSafe = nearestHidden(pSub, here, 12))
				{
					pSub->SetDestination(pSafe, true);
					pSub->QueueMission(Mission::Move, true);
					NoteOrderCaller(pSub, "move");
					CountDecision(NeuralDecision::SubmarineSonarAvoided, pHouse->ArrayIndex);
				}
			}
		}
	}

	bool CanBuildUnderHumanRules(HouseClass* pHouse, TechnoTypeClass* pType);

	// Map obstacles the AI clears, every 450 frames, with up to N idle armed ground units of ours
	// near them (not in plans or capital groups, nearest first):
	// - A veinhole monster we have seen within 35 cells of our buildings, when we cannot harvest
	//   veins (no Weeder owned or buildable): its veins spread and hurt what they touch. 6 units.
	// - A destroyable cliff: a neutral, Insignificant structure of 50 strength or less with
	//   impassable rows, seen, whose surroundings join our land to another land zone that holds
	//   ore or a known enemy building (destroying it opens that way). 2 units, one cliff at a time.
	void UpdateMapObstacles(HouseClass* pHouse)
	{
		if (!pHouse || !IsAITickWindow(450, 15))
			return;

		auto& macro = GetMacroState(pHouse);
		CellStruct home = pHouse->BaseCenter;

		if (pHouse->ConYards.Count > 0 && pHouse->ConYards.GetItem(0))
			home = pHouse->ConYards.GetItem(0)->GetMapCoords();

		if (home == CellStruct::Empty)
			return;

		const int homeZone = MapClass::Instance.GetMovementZoneType(home, MovementZone::Normal, false);

		auto const sendAttackers = [&](ObjectClass* pTarget, size_t count, int decision)
		{
			const CellStruct tc = pTarget->GetMapCoords();
			std::vector<std::pair<int, UnitClass*>> candidates;

			for (auto const pUnit : UnitClass::Array)
			{
				if (!pUnit || pUnit->Owner != pHouse || !pUnit->IsAlive || pUnit->InLimbo || !pUnit->Type || pUnit->Type->Naval
					|| pUnit->Type->Harvester || pUnit->Type->ResourceGatherer || pUnit->Type->DeploysInto
					|| IsAirborneThreatTechno(pUnit) || GroundRangeOf(pUnit->Type) <= 0 || macro.PlanMembers.contains(pUnit->UniqueID)
					|| macro.CapitalHeld.contains(pUnit->UniqueID) || pUnit->Target == pTarget)
				{
					continue;
				}

				const int d = static_cast<int>(pUnit->GetMapCoords().DistanceFrom(tc));

				if (d > 50 || pUnit->Target != nullptr
					|| (pUnit->CurrentMission != Mission::Guard && pUnit->CurrentMission != Mission::Area_Guard))
				{
					continue;
				}

				candidates.emplace_back(d, pUnit);
			}

			std::sort(candidates.begin(), candidates.end(), [](const auto& a, const auto& b)
				{
					return a.first != b.first ? a.first < b.first : a.second->UniqueID < b.second->UniqueID;
				});

			size_t sent = 0;

			for (auto const& [d, pUnit] : candidates)
			{
				if (sent >= count)
					break;

				pUnit->SetTarget(pTarget);
				pUnit->QueueMission(Mission::Attack, true);
				sent++;
			}

			if (sent > 0)
				CountDecision(static_cast<NeuralDecision>(decision), pHouse->ArrayIndex);
		};

		// Veinhole monsters.
		bool canHarvestVeins = false;

		for (auto const pType : UnitTypeClass::Array)
		{
			if (pType && pType->Weeder && (pHouse->CountOwnedNow(pType) > 0 || CanBuildUnderHumanRules(pHouse, pType)))
			{
				canHarvestVeins = true;
				break;
			}
		}

		if (!canHarvestVeins)
		{
			for (auto const pMonster : VeinholeMonsterClass::Array)
			{
				if (!pMonster || !pMonster->IsAlive || pMonster->InLimbo || !IsCellVisibleToAI(pHouse, pMonster->GetCell()))
					continue;

				const CellStruct mc = pMonster->GetMapCoords();
				bool nearUs = false;

				for (auto const pBld : pHouse->Buildings)
				{
					if (pBld && pBld->IsAlive && !pBld->InLimbo && pBld->GetMapCoords().DistanceFrom(mc) <= 35.0)
					{
						nearUs = true;
						break;
					}
				}

				if (nearUs)
					sendAttackers(pMonster, 6, static_cast<int>(NeuralDecision::VeinholeAttack));
			}
		}

		// Destroyable cliffs.
		if (homeZone <= 1)
			return;

		for (auto const pBld : BuildingClass::Array)
		{
			if (!pBld || !pBld->IsAlive || pBld->InLimbo || !pBld->Type || !pBld->Owner || !pBld->Owner->IsNeutral()
				|| !pBld->Type->Insignificant || pBld->Type->Strength > 50 || pBld->Type->NumberImpassableRows <= 0
				|| pBld->Type->Capturable || !IsCellVisibleToAI(pHouse, pBld->GetCell()))
			{
				continue;
			}

			const CellStruct bc = pBld->GetMapCoords();
			const int w = pBld->Type->GetFoundationWidth();
			const int h = pBld->Type->GetFoundationHeight(false);
			bool touchesHome = false;
			std::vector<int> otherZones;

			for (int dy = -2; dy <= h + 1; ++dy)
			{
				for (int dx = -2; dx <= w + 1; ++dx)
				{
					if (dx >= 0 && dx < w && dy >= 0 && dy < h)
						continue;

					const CellStruct c { static_cast<short>(bc.X + dx), static_cast<short>(bc.Y + dy) };

					if (!MapClass::Instance.CoordinatesLegal(c))
						continue;

					const int z = MapClass::Instance.GetMovementZoneType(c, MovementZone::Normal, false);

					if (z == homeZone)
						touchesHome = true;
					else if (z > 1 && std::find(otherZones.begin(), otherZones.end(), z) == otherZones.end())
						otherZones.push_back(z);
				}
			}

			if (!touchesHome || otherZones.empty())
				continue;

			bool worth = false;

			for (auto const pEnemy : GetKnownEnemyBuildingCache(pHouse).Buildings)
			{
				if (pEnemy && pEnemy->IsAlive
					&& std::find(otherZones.begin(), otherZones.end(),
						MapClass::Instance.GetMovementZoneType(pEnemy->GetMapCoords(), MovementZone::Normal, pEnemy->OnBridge)) != otherZones.end())
				{
					worth = true;
					break;
				}
			}

			for (int dy = -20; dy <= 20 && !worth; dy += 2)
			{
				for (int dx = -20; dx <= 20 && !worth; dx += 2)
				{
					const CellStruct c { static_cast<short>(bc.X + dx), static_cast<short>(bc.Y + dy) };
					auto const pCell = MapClass::Instance.CoordinatesLegal(c) ? MapClass::Instance.TryGetCellAt(c) : nullptr;

					if (pCell && pCell->GetContainedTiberiumValue() > 0 && IsCellVisibleToAI(pHouse, pCell)
						&& std::find(otherZones.begin(), otherZones.end(), MapClass::Instance.GetMovementZoneType(c, MovementZone::Normal, false))
							!= otherZones.end())
					{
						worth = true;
					}
				}
			}

			if (worth)
			{
				sendAttackers(pBld, 2, static_cast<int>(NeuralDecision::CliffOpened));
				break;
			}
		}
	}

	bool IsSpecOpsEngineerBusy(HouseClass* pHouse, DWORD engineerID);

	// Whether a building of type idxType satisfies prerequisite entry idx: a building type index,
	// or (negative) a group of [GenericPrerequisites], nested groups included.
	bool BuildingTypeSatisfiesPrerequisite(int idxType, int idx, int depth = 0)
	{
		if (idx >= 0)
			return idx == idxType;

		auto const& groups = RulesExt::Global()->GenericPrerequisites;
		const int group = -idx;

		if (depth > 4 || group >= groups.Count)
			return false;

		for (const int item : groups.GetItem(group))
		{
			if (BuildingTypeSatisfiesPrerequisite(idxType, item, depth + 1))
				return true;
		}

		return false;
	}

	// The alternative prerequisite lists of a type: Prerequisite= and every Ares
	// Prerequisite.ListN= (meeting any one of them is enough).
	std::vector<std::vector<int>> PrerequisiteListsOf(TechnoTypeClass* pType)
	{
		std::vector<std::vector<int>> lists;
		auto const pTypeExt = TechnoTypeExt::Fetch(pType);

		if (!pTypeExt)
			return lists;

		if (!pTypeExt->Prerequisite.empty())
			lists.push_back(pTypeExt->Prerequisite);

		if (pTypeExt->Prerequisite_Lists.Get() > 0)
		{
			for (auto const& list : pTypeExt->Prerequisite_ListVector)
			{
				if (list.Count > 0)
					lists.emplace_back(list.begin(), list.end());
			}
		}

		if (lists.empty())
			lists.emplace_back();

		return lists;
	}

	// MCV types this house may own (owners, tech level), in array order.
	std::vector<UnitTypeClass*> HouseMCVTypes(HouseClass* pHouse)
	{
		std::vector<UnitTypeClass*> types;

		for (auto const pType : UnitTypeClass::Array)
		{
			if (pType && pType->DeploysInto && pType->DeploysInto->ConstructionYard && pType->TechLevel >= 0
				&& pType->TechLevel <= pHouse->TechLevel && HouseExt::IsAvailableToHouse(pHouse, pType))
			{
				types.push_back(pType);
			}
		}

		return types;
	}

	// A building that one of our MCV prerequisite lists names: selling it (Survival) would leave the
	// house unable to build an MCV again.
	bool IsMCVPrerequisiteType(HouseClass* pHouse, BuildingTypeClass* pType)
	{
		for (auto const pMcv : HouseMCVTypes(pHouse))
		{
			for (auto const& list : PrerequisiteListsOf(pMcv))
			{
				for (const int idx : list)
				{
					if (BuildingTypeSatisfiesPrerequisite(pType->ArrayIndex, idx))
						return true;
				}
			}
		}

		return false;
	}

	// The cheapest building type we can build now that satisfies prerequisite entry idx (ties by
	// array index), or nullptr when none is buildable (a tech building we can only capture).
	BuildingTypeClass* CheapestBuildableForPrerequisite(HouseClass* pHouse, int idx)
	{
		BuildingTypeClass* pBest = nullptr;
		int bestCost = 0;

		for (auto const pType : BuildingTypeClass::Array)
		{
			if (!pType || pType->Naval || !BuildingTypeSatisfiesPrerequisite(pType->ArrayIndex, idx)
				|| !CanBuildUnderHumanRules(pHouse, pType))
			{
				continue;
			}

			const int cost = pType->GetActualCost(pHouse);

			if (!pBest || cost < bestCost)
			{
				pBest = pType;
				bestCost = cost;
			}
		}

		return pBest;
	}

	// The cheapest ground factory of the given kind (UnitType: war factory, InfantryType: barracks)
	// we can build now, or nullptr.
	BuildingTypeClass* CheapestBuildableGroundFactory(HouseClass* pHouse, AbstractType kind)
	{
		BuildingTypeClass* pBest = nullptr;
		int bestCost = 0;

		for (auto const pType : BuildingTypeClass::Array)
		{
			if (!pType || pType->Factory != kind || pType->Naval || pType->UndeploysInto || pType->ConstructionYard
				|| !CanBuildUnderHumanRules(pHouse, pType))
			{
				continue;
			}

			const int cost = pType->GetActualCost(pHouse);

			if (!pBest || cost < bestCost)
			{
				pBest = pType;
				bestCost = cost;
			}
		}

		return pBest;
	}

	// Our buildings standing next to an ally's base (within 18 cells of its ConYard).
	std::vector<BuildingClass*> OwnBuildingsAtAllySite(HouseClass* pHouse, const CellStruct& yardCell)
	{
		std::vector<BuildingClass*> found;

		for (auto const pBld : pHouse->Buildings)
		{
			if (pBld && pBld->IsAlive && !pBld->InLimbo && pBld->Type && pBld->CurrentMission != Mission::Selling
				&& pBld->GetMapCoords().DistanceFrom(yardCell) <= 18.0)
			{
				found.push_back(pBld);
			}
		}

		return found;
	}

	// What an ally backup site still lacks to build an MCV there once every ConYard and MCV is
	// lost: the cheapest of our MCV prerequisite lists not yet met by our buildings at the site,
	// each missing entry as the cheapest buildable type, plus a ground war factory when the list
	// names none. Empty with ok = true when the site is complete; ok = false when no list can be
	// completed (prerequisites we can only capture, or no MCV we could build today).
	std::vector<BuildingTypeClass*> MissingForAllyBackup(HouseClass* pHouse, const CellStruct& yardCell, bool& ok)
	{
		ok = false;
		std::vector<BuildingTypeClass*> best;
		int bestCost = 0;
		auto const siteBuildings = OwnBuildingsAtAllySite(pHouse, yardCell);

		auto metAtSite = [&siteBuildings](int idx)
		{
			return std::any_of(siteBuildings.begin(), siteBuildings.end(), [idx](BuildingClass* pBld)
				{
					return BuildingTypeSatisfiesPrerequisite(pBld->Type->ArrayIndex, idx);
				});
		};

		for (auto const pMcv : HouseMCVTypes(pHouse))
		{
			// Only an MCV we could build today: the backup copies a base that works.
			if (!HouseExt::PrerequisitesMet(pHouse, pMcv))
				continue;

			for (auto const& list : PrerequisiteListsOf(pMcv))
			{
				std::vector<BuildingTypeClass*> missing;
				bool listOk = true;
				bool namesFactory = false;
				int cost = 0;

				for (const int idx : list)
				{
					auto const pResolved = CheapestBuildableForPrerequisite(pHouse, idx);

					if (pResolved && pResolved->Factory == AbstractType::UnitType)
						namesFactory = true;

					if (metAtSite(idx))
						continue;

					if (!pResolved)
					{
						listOk = false;
						break;
					}

					if (std::find(missing.begin(), missing.end(), pResolved) == missing.end())
					{
						missing.push_back(pResolved);
						cost += pResolved->GetActualCost(pHouse);
					}
				}

				if (!listOk)
					continue;

				if (!namesFactory)
				{
					const bool hasFactory = std::any_of(siteBuildings.begin(), siteBuildings.end(), [](BuildingClass* pBld)
						{
							return pBld->Type->Factory == AbstractType::UnitType && !pBld->Type->Naval;
						});

					if (!hasFactory)
					{
						auto const pFactory = CheapestBuildableGroundFactory(pHouse, AbstractType::UnitType);

						if (!pFactory)
							continue;

						missing.push_back(pFactory);
						cost += pFactory->GetActualCost(pHouse);
					}
				}

				// War factory first: it is the part that produces the MCV.
				std::stable_sort(missing.begin(), missing.end(), [](BuildingTypeClass* a, BuildingTypeClass* b)
					{
						return (a->Factory == AbstractType::UnitType) > (b->Factory == AbstractType::UnitType);
					});

				if (!ok || cost < bestCost)
				{
					ok = true;
					best = std::move(missing);
					bestCost = cost;
				}
			}
		}

		return best;
	}

	// Whether the ally keeps room to grow if we put a building of pType next to its buildings
	// around `around`: on a cramped map every free cell near its base is one it needs, so we only
	// take a small share. Counts the cells within 12 where pType could stand (free, buildable).
	bool AllyKeepsRoomAround(const CellStruct& around, HouseClass* pAlly, BuildingTypeClass* pType)
	{
		const CellStruct yc = around;
		int freeCells = 0;

		for (short dy = -12; dy <= 12; ++dy)
		{
			for (short dx = -12; dx <= 12; ++dx)
			{
				const CellStruct c { static_cast<short>(yc.X + dx), static_cast<short>(yc.Y + dy) };

				if (!MapClass::Instance.CoordinatesLegal(c))
					continue;

				auto const pCell = MapClass::Instance.TryGetCellAt(c);

				if (pCell && !pCell->GetBuilding() && pCell->CanThisExistHere(pType->SpeedType, pType, pAlly))
					freeCells++;
			}
		}

		const int footprint = (pType->GetFoundationWidth() + 2) * (pType->GetFoundationHeight(false) + 2);

		return freeCells >= 4 * footprint + 60;
	}

	bool AllyKeepsRoomFor(BuildingClass* pAllyYard, BuildingTypeClass* pType)
	{
		return AllyKeepsRoomAround(pAllyYard->GetMapCoords(), pAllyYard->Owner, pType);
	}

	// Ally ConYards we may build next to (BuildOffAlly), sorted by UniqueID.
	std::vector<BuildingClass*> AllyYardsForSites(HouseClass* pHouse)
	{
		std::vector<BuildingClass*> yards;

		if (!GameModeOptionsClass::Instance.BuildOffAlly)
			return yards;

		for (auto const pBld : BuildingClass::Array)
		{
			if (!pBld || !pBld->IsAlive || pBld->InLimbo || !pBld->Type || !pBld->Owner || pBld->Owner == pHouse
				|| !pBld->Type->ConstructionYard || !pBld->Type->EligibileForAllyBuilding || !pHouse->IsAlliedWith(pBld->Owner)
				|| pBld->Owner->IsNeutral() || pBld->Owner->IsObserver() || pBld->Owner->Defeated)
			{
				continue;
			}

			yards.push_back(pBld);
		}

		std::sort(yards.begin(), yards.end(), [](BuildingClass* a, BuildingClass* b) { return a->UniqueID < b->UniqueID; });

		return yards;
	}

	// Where our site starts next to an ally's base: just past the edge of its buildings around
	// pYard, toward `reference` (or straight away from it when `away`), shifted `side` cells, on a
	// legal cell. Placement looks for room around it and keeps a free cell from every ally building.
	CellStruct AllySiteCellFor(BuildingClass* pYard, const CellStruct& reference, bool away, int side)
	{
		const CellStruct yc = pYard->GetMapCoords();
		double extent = 4.0;

		for (auto const pBld : pYard->Owner->Buildings)
		{
			if (pBld && pBld->IsAlive && !pBld->InLimbo)
			{
				const double d = pBld->GetMapCoords().DistanceFrom(yc);

				if (d <= 20.0)
					extent = std::max(extent, d);
			}
		}

		CellStruct goal = reference;

		if (away)
			goal = CellStruct { static_cast<short>(2 * yc.X - reference.X), static_cast<short>(2 * yc.Y - reference.Y) };

		const double dx = static_cast<double>(goal.X - yc.X);
		const double dy = static_cast<double>(goal.Y - yc.Y);
		const double len = std::max(1.0, std::sqrt(dx * dx + dy * dy));
		const double cells = extent + 3.0;
		const CellStruct site { static_cast<short>(yc.X + dx / len * cells - dy / len * side),
			static_cast<short>(yc.Y + dy / len * cells + dx / len * side) };

		return MapClass::Instance.CoordinatesLegal(site) ? site : yc;
	}

	// Flying transports that dropped a raid team keep the safe air lane home even after their plan
	// is over (target gone or captured): UniqueIDs in drop order, one list per house.
	struct NeuralSafeReturnState
	{
		int MatchSerial { -1 };
		std::vector<DWORD> Flights {};
	};

	std::array<NeuralSafeReturnState, 32> SafeReturnStates {};

	NeuralSafeReturnState& GetSafeReturnState(const HouseClass* pHouse)
	{
		auto& state = SafeReturnStates[std::clamp(pHouse ? pHouse->ArrayIndex : 0, 0, 31)];

		if (state.MatchSerial != NeuralMatchSerial)
			state = NeuralSafeReturnState { NeuralMatchSerial };

		return state;
	}

	// Island Hold: a defense counts for the island yard within this many cells of it, on its
	// landmass; the yard wants this many that hit the ground.
	constexpr double IslandHoldDefenseRadius = 12.0;
	constexpr int IslandHoldGroundDefenses = 3;
	// Frames the plan lets go of its defenses after placement found no cell for one next to the yard.
	constexpr int IslandHoldNoRoomFrames = 900;

	// This defense type is one the yard of the Island Hold plan still lacks. Building production
	// and placement both ask here, so what gets chosen for the yard is what gets placed next to it
	// and what the plan counts.
	bool IslandHoldWants(HouseClass* pHouse, BuildingTypeClass* pType)
	{
		if (!pHouse || !pType || !pType->IsBaseDefense || IsTunnelNetworkBuilding(pType))
			return false;

		auto const& macro = GetMacroState(pHouse);

		if (macro.IslandHoldUntilFrame <= Unsorted::CurrentFrame || macro.IslandHoldCell == CellStruct::Empty
			|| Unsorted::CurrentFrame < macro.IslandHoldNoRoomUntilFrame)
		{
			return false;
		}

		return (macro.IslandHoldNeedsGround && GroundRangeOf(pType) > 0)
			|| (macro.IslandHoldNeedsAntiAir && GetCombatValueOfType(pType, true) > 0);
	}

	void UpdateStrategicPlans(HouseClass* pHouse, const std::array<int16_t, NeuralAI::OutputVectorSize>& scores)
	{
		// Orders of this part lease their units at this priority (order leases).
		const NeuralOrderPriorityScope orderPriority { NeuralOrderPriority::Operation };

		if (!pHouse || pHouse->Defeated)
			return;

		auto& macro = GetMacroState(pHouse);
		const int houseIdx = pHouse->ArrayIndex;
		const auto& foe = macro.OpponentProfile;

		CellStruct homeCell = pHouse->BaseSpawnCell;

		if (pHouse->ConYards.Count > 0 && pHouse->ConYards.GetItem(0))
			homeCell = pHouse->ConYards.GetItem(0)->GetMapCoords();

		if (homeCell == CellStruct::Empty)
			return;

		const int homeZone = MapClass::Instance.GetMovementZoneType(homeCell, MovementZone::Normal, false);

		// Resolve our own units by UniqueID once per tick. The units and transports of the transport
		// operations are left out: no plan gives them orders (257 orders of the plans found on them in
		// 13 matches), and none is a plan member (a plan recruits no unit of an operation).
		std::unordered_map<DWORD, FootClass*> ownFoots;

		for (auto const pTech : TechnoClass::Array)
		{
			if (pTech && pTech->Owner == pHouse && pTech->IsAlive && !pTech->InLimbo && !IsInTransportOp(pHouse, pTech->UniqueID))
			{
				if (auto const pFoot = abstract_cast<FootClass*>(pTech))
					ownFoots[pFoot->UniqueID] = pFoot;
			}
		}

		// Our siege units deployed into structures (IsDeployedSiegeBuildingType), by UniqueID: they
		// stay plan members while deployed.
		std::unordered_map<DWORD, BuildingClass*> ownSiegeBuildings;

		for (auto const pBld : pHouse->Buildings)
		{
			if (pBld && pBld->IsAlive && !pBld->InLimbo && IsDeployedSiegeBuildingType(pBld->Type))
				ownSiegeBuildings[pBld->UniqueID] = pBld;
		}

		// A member that unfolds into a structure or packs up again is a new object: follow it. The
		// successor is our object of the type it turns into (DeploysInto / UndeploysInto), newer
		// than the member (higher UniqueID), within 3 cells of where the member was last seen
		// (within 300 frames), not in any plan yet; the lowest such UniqueID.
		{
			const int now = Unsorted::CurrentFrame;
			auto& traces = macro.PlanMemberTraces;
			std::unordered_set<DWORD> inPlans;

			for (auto const& plan : macro.Plans)
			{
				if (plan.Active)
					inPlans.insert(plan.Members.begin(), plan.Members.end());
			}

			for (auto& plan : macro.Plans)
			{
				if (!plan.Active)
					continue;

				for (DWORD& uid : plan.Members)
				{
					if (auto const itFoot = ownFoots.find(uid); itFoot != ownFoots.end())
					{
						traces[uid] = { itFoot->second->GetTechnoType(), itFoot->second->GetMapCoords(), now };
						continue;
					}

					if (auto const itBld = ownSiegeBuildings.find(uid); itBld != ownSiegeBuildings.end())
					{
						traces[uid] = { itBld->second->Type, itBld->second->GetMapCoords(), now };
						continue;
					}

					auto const itTrace = traces.find(uid);

					if (itTrace == traces.end() || now - itTrace->second.Frame > 300 || !itTrace->second.Type)
						continue;

					auto const pOldType = itTrace->second.Type;
					const CellStruct oldCell = itTrace->second.Cell;
					DWORD successor = 0;
					const TechnoTypeClass* pNewType = nullptr;
					CellStruct newCell = CellStruct::Empty;

					if (pOldType->WhatAmI() == AbstractType::UnitType)
					{
						auto const pInto = static_cast<const UnitTypeClass*>(pOldType)->DeploysInto;

						for (auto const& [bldID, pBld] : ownSiegeBuildings)
						{
							if (pInto && pBld->Type == pInto && bldID > uid && !inPlans.contains(bldID)
								&& pBld->GetMapCoords().DistanceFrom(oldCell) <= 3.0 && (successor == 0 || bldID < successor))
							{
								successor = bldID;
								pNewType = pBld->Type;
								newCell = pBld->GetMapCoords();
							}
						}
					}
					else if (pOldType->WhatAmI() == AbstractType::BuildingType)
					{
						auto const pInto = static_cast<const BuildingTypeClass*>(pOldType)->UndeploysInto;

						for (auto const& [footID, pFoot] : ownFoots)
						{
							if (pInto && pFoot->GetTechnoType() == pInto && footID > uid && !inPlans.contains(footID)
								&& pFoot->GetMapCoords().DistanceFrom(oldCell) <= 3.0 && (successor == 0 || footID < successor))
							{
								successor = footID;
								pNewType = pInto;
								newCell = pFoot->GetMapCoords();
							}
						}
					}

					if (successor != 0)
					{
						inPlans.erase(uid);
						inPlans.insert(successor);
						traces[successor] = { pNewType, newCell, now };
						uid = successor;
					}
				}
			}

			std::erase_if(traces, [now](const auto& entry) { return now - entry.second.Frame > 600 || entry.second.Frame > now; });
		}

		// Plan deadlines are fixed lengths, and a group crossing a large map is often still on its
		// way when its deadline comes. Every 600 frames each plan with members checks whether they
		// got at least 6 cells nearer to the target; a group still closing in and not there yet
		// (over 10 cells away) gets 900 more frames when its deadline comes, up to twice the
		// length the plan started with. A group that stopped closing in keeps its deadline.
		for (auto& plan : macro.Plans)
		{
			if (!plan.Active || plan.Members.empty() || plan.TargetCell == CellStruct::Empty || plan.BaseDuration <= 0)
				continue;

			int alive = 0;
			int distanceSum = 0;

			for (const DWORD uid : plan.Members)
			{
				auto const it = ownFoots.find(uid);

				if (it == ownFoots.end())
					continue;

				alive++;
				distanceSum += static_cast<int>(it->second->GetMapCoords().DistanceFrom(plan.TargetCell));
			}

			if (alive == 0)
				continue;

			const int distance = distanceSum / alive;

			if (plan.ProgressCheckDistance < 0)
			{
				plan.ProgressCheckDistance = distance;
				plan.ProgressCheckFrame = Unsorted::CurrentFrame;
			}
			else if (Unsorted::CurrentFrame - plan.ProgressCheckFrame >= 600)
			{
				plan.ClosingIn = plan.ProgressCheckDistance - distance >= 6;
				plan.ProgressCheckDistance = distance;
				plan.ProgressCheckFrame = Unsorted::CurrentFrame;
			}

			if (Unsorted::CurrentFrame >= plan.Deadline && plan.ClosingIn && distance > 10
				&& plan.Deadline - plan.StartFrame + 900 <= plan.BaseDuration * 2)
			{
				plan.Deadline += 900;
				CountDecision(NeuralDecision::PlanDeadlineExtended, houseIdx);
			}
			// A group that destroyed something lately keeps going (PlanCombat*, NeuralAI.State.inl).
			else if (Unsorted::CurrentFrame >= plan.Deadline && plan.LastKillFrame >= 0
				&& Unsorted::CurrentFrame - plan.LastKillFrame <= PlanCombatWindowFrames
				&& plan.Deadline - plan.StartFrame + PlanCombatExtendFrames <= plan.BaseDuration * PlanCombatExtendMostTimes)
			{
				plan.Deadline += PlanCombatExtendFrames;
				CountDecision(NeuralDecision::PlanDeadlineExtended, houseIdx);
			}
		}

		auto memberValue = [&](const NeuralPlanInstance& plan) -> int
		{
			int value = 0;

			for (const DWORD uid : plan.Members)
			{
				if (auto const it = ownFoots.find(uid); it != ownFoots.end())
					value += GetCombatValueOfType(it->second->GetTechnoType(), false);
				else if (auto const itBld = ownSiegeBuildings.find(uid); itBld != ownSiegeBuildings.end())
					value += GetCombatValueOfType(itBld->second->Type, false);
			}

			return value;
		};

		// Siege plan member that is or can become a structure. Deployed: fires at the target when
		// it is in reach, else packs up (it comes back as a unit and walks to the stage). A
		// deployable unit at its stage with the target in reach of the deployed gun unfolds there.
		// True when the member was handled; otherwise the plan gives its usual unit orders.
		auto handleSiegeDeployMember = [&](DWORD uid, BuildingClass* pTarget, CellClass* pStage) -> bool
		{
			if (auto const itBld = ownSiegeBuildings.find(uid); itBld != ownSiegeBuildings.end())
			{
				auto const pBld = itBld->second;

				if (pBld->CurrentMission == Mission::Selling || pBld->QueuedMission == Mission::Selling
					|| pBld->CurrentMission == Mission::Construction)
				{
					return true;
				}

				if (pTarget && pBld->DistanceFrom(pTarget) <= GroundRangeOf(pBld->Type))
				{
					if (pBld->Target != pTarget)
						pBld->SetTarget(pTarget);
				}
				else if (pStage)
				{
					pBld->SetArchiveTarget(pStage);
					pBld->Sell(1);
				}

				return true;
			}

			auto const itFoot = ownFoots.find(uid);
			auto const pUnit = itFoot != ownFoots.end() ? abstract_cast<UnitClass*>(itFoot->second) : nullptr;

			if (!pUnit || !pTarget || !pStage || !IsDeployableSiegeUnitType(pUnit->Type)
				|| pUnit->GetMapCoords().DistanceFrom(pStage->MapCoords) > 3.0
				|| pUnit->DistanceFrom(pTarget) > GroundRangeOf(pUnit->Type->DeploysInto))
			{
				return false;
			}

			if (pUnit->CurrentMission != Mission::Unload)
			{
				pUnit->SetDestination(nullptr, true);
				pUnit->ClearNavigationList();
				pUnit->SetTarget(nullptr);
				pUnit->QueueMission(Mission::Unload, true);
			}

			return true;
		};

		auto isFreeForPlan = [&](FootClass* pFoot) -> bool
		{
			// A unit or transport of a transport operation is the operation's until it hands it back.
			if (!pFoot || macro.PlanMembers.contains(pFoot->UniqueID) || IsOnRepairTrip(pHouse, pFoot->UniqueID)
				|| IsInTransportOp(pHouse, pFoot->UniqueID) || macro.TankGuard.contains(pFoot->UniqueID))
				return false;

			// A slave obeys its slave master only: it goes back to work whatever a plan orders, and
			// the plan waits for it until its deadline.
			if (pFoot->SlaveOwner)
				return false;

			// Heavy flyers that are not aircraft (airships, jumpjet bombers) cannot land at a repair
			// depot and often self-heal only up to a cap below 60%, so their health floor is lower.
			const bool heavyFlyer = pFoot->WhatAmI() != AbstractType::Aircraft && IsAirborneThreatTechno(pFoot)
				&& pFoot->GetTechnoType() && pFoot->GetTechnoType()->Strength >= 600;

			if (pFoot->GetHealthPercentage() < (heavyFlyer ? 0.35 : 0.6))
				return false;

			const bool idle = (pFoot->CurrentMission == Mission::Guard || pFoot->CurrentMission == Mission::Area_Guard
				|| pFoot->CurrentMission == Mission::Sleep) && pFoot->Target == nullptr;

			// Units marching in a generic wave (attack-move, no target yet) are available too: the
			// waves keep most of the army on the move, and plans needing big groups (Three-Front 15,
			// Feint 8, Timing Rush 6) recruit from them. Plans keep the last word on their members.
			// Not one whose wave mission is still under way (IsOnWaveMarch; user, 2026-11-19: a mission
			// in progress is not disturbed until it ends): a Base Threat Intercept took LTNK of house 7
			// 26 to 39 cells from home off their march (match #11501).
			const bool marchingInWave = pFoot->MegaMission == Mission::AttackMove && pFoot->Target == nullptr
				&& (pFoot->CurrentMission == Mission::Move || pFoot->CurrentMission == Mission::Guard);

			return idle || (marchingInWave && !IsOnWaveMarch(pHouse, pFoot));
		};

		// Armed strike aircraft a plan may take: idle, or still on the ground with an attack order
		// it has not flown yet. The air micro runs first and gives every idle aircraft a harass
		// target, so requiring no target at all kept every air plan without aircraft; a landed
		// aircraft has not committed to that harass, and the plan keeps the last word on it.
		auto isFreeAirStriker = [&](AircraftClass* pAir) -> bool
		{
			if (!pAir || !pAir->Type || pAir->Type->Carryall || pAir->Type->Passengers > 0 || pAir->Type->Spawned
				|| GroundRangeOf(pAir->Type) <= 0 || macro.PlanMembers.contains(pAir->UniqueID)
				|| (pAir->Ammo == 0 && pAir->Type->Ammo >= 0) || pAir->GetHealthPercentage() < 0.6)
			{
				return false;
			}

			if (pAir->CurrentMission == Mission::Guard || pAir->CurrentMission == Mission::Sleep
				|| pAir->CurrentMission == Mission::Area_Guard)
			{
				return pAir->Target == nullptr || !pAir->IsInAir();
			}

			return pAir->CurrentMission == Mission::Attack && !pAir->IsInAir();
		};

		// Rotating Raids lesson against an enemy house: the doubling steps left after the fading.
		auto raidLessonSteps = [&](int owner) -> int
		{
			if (owner < 0 || owner >= 32)
				return 0;

			const int faded = (Unsorted::CurrentFrame - macro.RaidLessonFrame[owner]) / RaidLessonStepFrames;
			return std::clamp(macro.RaidLessonSteps[owner] - std::max(0, faded), 0, RaidLessonMaxSteps);
		};

		// Units a Rotating Raids group against that house sets out with: RaidBaseGroup doubled per step.
		auto rotatingRaidsSize = [&](int owner) -> size_t
		{
			return static_cast<size_t>(std::min(RaidMostGroup, RaidBaseGroup << raidLessonSteps(owner)));
		};

		auto finishPlan = [&](NeuralPlanId id, bool success, bool sendHome, int rule = __builtin_LINE())
		{
			auto& plan = macro.Plans[static_cast<size_t>(id)];

			// TEMP diagnostic: how every plan ends. The rule is the source line of the call that ended
			// it, so the match logs tell which condition fails each plan (a deadline with the group
			// still far, a group that lost its strength on the way, a target that was never reached).
			{
				int alive = 0;
				int nearest = -1;
				int lostDead = 0;
				int lostOffMap = 0;
				int lostTraced = 0;
				int lostDistance = 0;

				for (const DWORD uid : plan.Members)
				{
					if (auto const it = ownFoots.find(uid); it != ownFoots.end())
					{
						alive++;
						const int distance = static_cast<int>(it->second->GetMapCoords().DistanceFrom(plan.TargetCell));

						if (nearest < 0 || distance < nearest)
							nearest = distance;

						continue;
					}

					if (ownSiegeBuildings.contains(uid))
					{
						alive++;
						continue;
					}

					// A missing member is dead, or alive off the map (loaded into a transport, inside a
					// building): the plan lost it either way, but the second is our own doing.
					bool offMap = false;

					for (auto const pTech : TechnoClass::Array)
					{
						if (pTech && pTech->UniqueID == uid)
						{
							offMap = pTech->IsAlive && pTech->InLimbo && pTech->Owner == pHouse;
							break;
						}
					}

					if (offMap)
						lostOffMap++;
					else
						lostDead++;

					if (auto const itTrace = macro.PlanMemberTraces.find(uid); itTrace != macro.PlanMemberTraces.end())
					{
						lostTraced++;
						lostDistance += static_cast<int>(itTrace->second.Cell.DistanceFrom(plan.TargetCell));
					}
				}

				Debug::Log("[NeuralAI] House %d plan %s ended %s at rule %d after %d of %d frames: members %d of %d, value %d of %d, "
					"arrived %d, nearest member %d cells from the target (%d,%d), lost %d dead and %d off the map, last seen %d cells from the target (%d traced).\n",
					houseIdx, NeuralPlanNames[static_cast<size_t>(id)], success ? "well" : (plan.Neutral ? "neutral" : "badly"), rule,
					Unsorted::CurrentFrame - plan.StartFrame, plan.Deadline - plan.StartFrame, alive,
					static_cast<int>(plan.Members.size()), memberValue(plan), plan.InitialValue, plan.Arrived ? 1 : 0, nearest,
					plan.TargetCell.X, plan.TargetCell.Y, lostDead, lostOffMap, lostTraced > 0 ? lostDistance / lostTraced : -1, lostTraced);
			}

			// A plan that took its building target hands its armed ground units to a follow-up attack
			// (PlanFollowUp*, NeuralAI.State.inl) instead of sending them home.
			std::vector<DWORD> followUpMembers;

			if (success && sendHome && plan.FallenTargetKind >= 0 && plan.TargetOwner >= 0 && plan.TargetOwner != houseIdx)
			{
				int followUpValue = 0;

				for (const DWORD uid : plan.Members)
				{
					auto const it = ownFoots.find(uid);

					if (it == ownFoots.end() || IsOnRepairTrip(pHouse, uid))
						continue;

					auto const pFoot = it->second;
					auto const pFootType = pFoot->GetTechnoType();
					const AbstractType footKind = pFoot->WhatAmI();

					if ((footKind != AbstractType::Unit && footKind != AbstractType::Infantry) || !pFootType || pFootType->Naval
						|| GroundRangeOf(pFootType) <= 0)
					{
						continue;
					}

					if (auto const pInf = abstract_cast<InfantryClass*>(pFoot); pInf && pInf->Type && (pInf->Type->Engineer || pInf->Type->Agent))
						continue;

					followUpMembers.push_back(uid);
					followUpValue += GetCombatValueOfType(pFootType, false);
				}

				if (followUpMembers.size() >= PlanFollowUpMinMembers)
				{
					NeuralFollowUp followUp;
					followUp.Members = followUpMembers;
					followUp.Owner = plan.TargetOwner;
					followUp.Kind = plan.FallenTargetKind;
					followUp.Plan = static_cast<int>(id);
					followUp.StartFrame = Unsorted::CurrentFrame;
					followUp.Until = Unsorted::CurrentFrame + PlanFollowUpFrames;
					followUp.InitialValue = std::max(1, followUpValue);
					followUp.Center = plan.TargetCell;
					macro.FollowUps.push_back(std::move(followUp));
				}
				else
				{
					followUpMembers.clear();
				}
			}

			if (sendHome)
			{
				// Staggered retreat (point 25): every second unit holds 120 frames to cover the others.
				const bool staggered = plan.Members.size() >= 4 && SlotGate(scores[77], -200, 77);
				size_t order = 0;

				if (auto const pHome = MapClass::Instance.TryGetCellAt(homeCell))
				{
					for (const DWORD uid : plan.Members)
					{
						auto const it = ownFoots.find(uid);

						if (it == ownFoots.end() || it->second->WhatAmI() == AbstractType::Aircraft
							|| it->second->GetTechnoType()->Naval
							|| std::find(followUpMembers.begin(), followUpMembers.end(), uid) != followUpMembers.end())
						{
							continue;
						}

						if (staggered && (order++ % 2) == 1)
							macro.DelayedRetreats.push_back({ uid, Unsorted::CurrentFrame + 120 });
						else
							IssueMoveOrder(it->second, pHome, 4.0);
					}
				}
			}

			// The plan's ships get no move home (their own logic takes them back), but a ship left on the
			// plan's move counts as busy for the idle naval logic, which then never gave it work again
			// (match #11450: Boomers sat on their last move off the enemy coast). Each one on a plain
			// move with no target stops where it is, free for it.
			for (const DWORD uid : plan.Members)
			{
				auto const it = ownFoots.find(uid);

				if (it == ownFoots.end() || it->second->WhatAmI() == AbstractType::Aircraft)
					continue;

				auto const pShip = it->second;
				auto const pShipType = pShip->GetTechnoType();

				if (!pShipType || !pShipType->Naval || pShip->Target != nullptr || pShip->CurrentMission != Mission::Move)
					continue;

				pShip->SetDestination(nullptr, true);
				pShip->QueueMission(Mission::Guard, true);
				CountDecision(NeuralDecision::NavalPlanShipsFreed, houseIdx);
			}

			// A group that failed against a target leaves a lesson: that much was not enough there
			// (targetLessonAllows).
			if (!success && plan.TargetID != 0 && !plan.Members.empty() && plan.InitialValue > 1)
			{
				auto& lesson = macro.PlanTargetLessons[plan.TargetID];
				const bool fresh = lesson.Frame > 0 && Unsorted::CurrentFrame - lesson.Frame <= PlanLessonFrames;
				lesson.Strength = fresh ? std::max(lesson.Strength, plan.InitialValue) : plan.InitialValue;
				lesson.Frame = Unsorted::CurrentFrame;
			}

			// A group beaten on its way (it never arrived and has less than PlanEnRouteLossPercent of
			// its value left) leaves the same lesson for every building of that enemy house (raid gate).
			if (!success && !plan.Arrived && plan.TargetOwner >= 0 && plan.TargetOwner < 32 && plan.InitialValue > 1
				&& memberValue(plan) * 100 < plan.InitialValue * PlanEnRouteLossPercent)
			{
				auto& houseLesson = macro.PlanHouseLessons[plan.TargetOwner];
				const bool freshLesson = houseLesson.Frame > 0 && Unsorted::CurrentFrame - houseLesson.Frame <= PlanLessonFrames;
				houseLesson.Strength = freshLesson ? std::max(houseLesson.Strength, plan.InitialValue) : plan.InitialValue;
				houseLesson.Frame = Unsorted::CurrentFrame;
			}

			// A Rotating Raids group the house beat (it lost value and destroyed no more than that) asks
			// the next one against it for twice as many units; one that destroyed more ends the lesson.
			if (id == NeuralPlanId::RotatingRaids && plan.TargetOwner >= 0 && plan.TargetOwner < 32 && plan.TargetOwner != houseIdx)
			{
				const int raidLost = std::max(0, plan.InitialValue - memberValue(plan));
				const int owner = plan.TargetOwner;

				if (plan.KillValue > raidLost)
				{
					macro.RaidLessonSteps[owner] = 0;
				}
				else if (!success && raidLost > 0)
				{
					macro.RaidLessonSteps[owner] = std::min(RaidLessonMaxSteps, raidLessonSteps(owner) + 1);
					macro.RaidLessonFrame[owner] = Unsorted::CurrentFrame;
					Debug::Log("[NeuralAI] House %d plan rotatingRaids beaten by house %d (destroyed %d, lost %d): the next one sets out with %d units, frame %d.\n",
						houseIdx, owner, plan.KillValue, raidLost, static_cast<int>(rotatingRaidsSize(owner)), Unsorted::CurrentFrame);
				}
			}

			// What the plan did against the house it went for weighs its next starts this match
			// (PlanEnemyWeight): judged by the value it destroyed and lost, and whether its target fell.
			const bool enemyTarget = plan.TargetOwner >= 0 && plan.TargetOwner < 32 && plan.TargetOwner != houseIdx;

			if (const int enemy = enemyTarget ? plan.TargetOwner : pHouse->EnemyHouseIndex; enemy >= 0 && enemy < 32)
			{
				bool targetFell = false;

				if (enemyTarget && plan.TargetID != 0)
				{
					targetFell = true;

					for (auto const pTech : TechnoClass::Array)
					{
						if (pTech && pTech->UniqueID == plan.TargetID)
						{
							targetFell = !pTech->IsAlive;
							break;
						}
					}
				}

				const int lost = std::max(0, plan.InitialValue - memberValue(plan));
				const int destroyed = plan.KillValue;
				int& weight = macro.PlanEnemyWeight[static_cast<size_t>(id)][enemy];
				const int before = weight;

				if (plan.Neutral)
				{
					// No defeat: its targets fell to others while it destroyed and lost nothing.
				}
				else if (targetFell)
					weight += PlanEnemyWeightFallStep;
				else if (success && destroyed > lost)
					weight += PlanEnemyWeightUpStep;
				else if (!success || lost > destroyed)
					weight -= PlanEnemyWeightDownStep;

				weight = std::clamp(weight, PlanEnemyWeightMin, PlanEnemyWeightMax);
				macro.PlanLastEnemy[static_cast<size_t>(id)] = enemy;

				if (weight != before)
				{
					Debug::Log("[NeuralAI] House %d plan %s against house %d: weight %d -> %d (destroyed %d, lost %d, target fell %d), frame %d.\n",
						houseIdx, NeuralPlanNames[static_cast<size_t>(id)], enemy, before, weight, destroyed, lost, targetFell ? 1 : 0,
						Unsorted::CurrentFrame);
				}
			}

			const bool neutral = plan.Neutral;

			if (!neutral)
				CountPlanOutcome(houseIdx, id, success ? NeuralPlanOutcome::Succeeded : NeuralPlanOutcome::Failed);

			if (!success && !neutral && Unsorted::CurrentFrame >= plan.Deadline)
				CountPlanOutcome(houseIdx, id, NeuralPlanOutcome::TimedOut);

			macro.PlanEndFrame[static_cast<size_t>(id)] = Unsorted::CurrentFrame;
			macro.PlanEndedWell[static_cast<size_t>(id)] = success;
			plan = NeuralPlanInstance {};
			macro.PlanNextAttemptFrame[static_cast<size_t>(id)] = Unsorted::CurrentFrame + ((success || neutral) ? 1800 : 2700);
		};

		// Early abort of a plan whose expected outcome turned bad (the seen enemy strength at the
		// target grew, the group lost most of its strength, the target is gone): the members go home
		// before the losses grow, the plan counts as failed, the reason goes to the telemetry.
		auto abortPlan = [&](NeuralPlanId id, NeuralDecision reason, int strength)
		{
			auto const& plan = macro.Plans[static_cast<size_t>(id)];
			Debug::Log("[NeuralAI] House %d plan %s aborted (%s) after %d frames, strength %d of %d.\n", houseIdx,
				NeuralPlanNames[static_cast<size_t>(id)], NeuralDecisionNames[static_cast<size_t>(reason)],
				Unsorted::CurrentFrame - plan.StartFrame, strength, plan.InitialStrength);
			CountDecision(reason, houseIdx);
			finishPlan(id, false, true);
		};

		// Plans whose learned gate opened this tick and that have not started yet (telemetry NoGroup).
		std::array<bool, static_cast<size_t>(NeuralPlanId::Count)> gateOpenedUnstarted {};

		// Bait Ambush and Feint play against human players only (user, 2026-11-15): they need an
		// enemy that chases a lone attacker or pulls its defenders to it, which a human does and
		// the Neural AI hardly ever does (its intercept answered 19 of 666 baits and 4 of 80
		// decoys), so against an AI they went well in 7% and 6% of the plans.
		bool humanEnemy = false;

		for (auto const pOther : HouseClass::Array)
		{
			if (pOther && pOther != pHouse && pOther->IsControlledByHuman() && !pOther->IsNeutral() && !pOther->IsObserver()
				&& !pOther->IsInitiallyObserver() && !pOther->Defeated && !pHouse->IsAlliedWith(pOther))
			{
				humanEnemy = true;
				break;
			}
		}

		// Whether a deception plan may aim at a building of this house: a human's, or anyone's in
		// a training focus match that asks for the plan (to measure it against the Neural AI).
		auto deceptionTargets = [](NeuralPlanId id, const HouseClass* pOwner) -> bool
		{
			return TrainingFocus().Plans[static_cast<size_t>(id)] || (pOwner && pOwner->IsControlledByHuman());
		};

		// Shelved plans start only in a training focus match that asks for them: Airborne Front
		// went well in none of 34 plans behind a main attack and is being redesigned (user,
		// 2026-11-14), so ordinary matches do not spend units or learning on it meanwhile. Bait
		// Ambush and Feint are shelved too unless a human enemy is in the match.
		auto planShelved = [humanEnemy](NeuralPlanId id) -> bool
		{
			if (TrainingFocus().Plans[static_cast<size_t>(id)])
				return false;

			if (id == NeuralPlanId::BaitAmbush || id == NeuralPlanId::Feint)
				return !humanEnemy;

			return id == NeuralPlanId::AirborneFront;
		};

		auto shouldTryStart = [&](NeuralPlanId id, int contextUtility) -> bool
		{
			const size_t i = static_cast<size_t>(id);

			if (macro.Plans[i].Active || Unsorted::CurrentFrame < macro.PlanNextAttemptFrame[i] || planShelved(id))
				return false;

			// 300..600 frames between attempts, varying per match and per plan (point 30).
			macro.PlanNextAttemptFrame[i] = Unsorted::CurrentFrame + 300 + StyleJitter(houseIdx, static_cast<int>(i), 450, 301);

			if (contextUtility > 0)
				CountPlanOutcome(houseIdx, id, NeuralPlanOutcome::Eligible);

			const int slot = NeuralPlanSlots[i];
			// Combo chains (point 56) boost the plans they lead into for 900 frames.
			const int boost = macro.PlanBoostUntilFrame[i] > Unsorted::CurrentFrame ? macro.PlanBoost[i] : 0;
			// Main assaults also weigh our army advantage (see MainAssaultAdvantageUtility), and an
			// offensive bank pushes harassment or assaults (OffensiveBankPlanUtility).
			const int advantage = contextUtility > 0 ? MainAssaultAdvantageUtility(houseIdx, id) + OffensiveBankPlanUtility(pHouse, id) : 0;
			// The plan priorities of this house's archetype (ArchetypePlanOffset).
			const int archetypeOffset = contextUtility > 0
				? ArchetypePlanOffset(HouseExt::Fetch(pHouse)->NeuralAI_Archetype, id, GetMacroState(pHouse).SiegeOwnersMask != 0u)
				: 0;
			// What this plan did this match against the house it last went for (the main enemy
			// before its first end) scales its utility (PlanEnemyWeight).
			const int weightEnemy = macro.PlanLastEnemy[i] >= 0 ? macro.PlanLastEnemy[i] : pHouse->EnemyHouseIndex;
			const int weightedUtility = (contextUtility > 0 && weightEnemy >= 0 && weightEnemy < 32)
				? contextUtility * macro.PlanEnemyWeight[i][weightEnemy] / 100
				: contextUtility;

			bool open = SlotGate(static_cast<int>(scores[slot]) + weightedUtility + (contextUtility > 0 ? boost : 0) + advantage
				+ archetypeOffset, 300, slot);

			// Training focus: the plans the trainer asked to watch start whenever they have a target
			// and a group; the others wait, except the ones that defend the base, so the watched
			// plans find free units.
			if (const auto& focus = TrainingFocus(); focus.AnyPlan)
			{
				if (focus.Plans[i])
					open = contextUtility > 0;
				else if (id != NeuralPlanId::BaseThreatIntercept && id != NeuralPlanId::Survival && id != NeuralPlanId::DelayingAction)
					open = false;
			}

			if (open && contextUtility > 0)
				gateOpenedUnstarted[i] = true;

			return open;
		};

		// Raid gate. Every plan that sends a ground group at a distant enemy target is weighed here,
		// whatever way it picked its group (Factory Decapitation picked nine units with no check at
		// all and started 300 times in 16 matches, 6% of them well). The ground units of the group
		// must be worth PlanStrikeAdvantagePercent of what is known on their way in (the defenses and
		// the units seen in the last PlanStrikeGuardFrames within PlanStrikeGuardCells of the target
		// or of the last PlanStrikeApproachCells of the straight way from home), and
		// PlanLessonAdvantagePercent of the last raid group this enemy house beat on its way in the
		// last PlanLessonFrames: what waits behind the fog shows only by what it did to the last
		// group, so each beaten raid asks for half as much again (user: the bigger group wins). A
		// group of aircraft alone is not weighed. Feint weighs its way in itself: its decoy is small
		// on purpose and need not win, and the lesson held back 24 of 26 decoys (2026-11-15).
		auto raidMayStart = [&](NeuralPlanId id, std::vector<FootClass*> const& group, const CellStruct& target, int targetOwner) -> bool
		{
			constexpr NeuralPlanId gated[] = { NeuralPlanId::EcoRaid, NeuralPlanId::Blackout,
				NeuralPlanId::TimingRush, NeuralPlanId::ExpansionDenial, NeuralPlanId::SuperweaponRace, NeuralPlanId::HarvesterAmbush,
				NeuralPlanId::PreemptiveStrike, NeuralPlanId::RotatingRaids, NeuralPlanId::ArtilleryEcoSiege,
				NeuralPlanId::FactoryDecapitation, NeuralPlanId::CutReinforcements, NeuralPlanId::TechTreeCut };

			if (std::find(std::begin(gated), std::end(gated), id) == std::end(gated) || target == CellStruct::Empty)
				return true;

			int groundValue = 0;

			for (auto const pFoot : group)
			{
				if (pFoot && pFoot->WhatAmI() != AbstractType::Aircraft && !pFoot->IsInAir() && !pFoot->GetTechnoType()->Naval)
					groundValue += GetCombatValueOfType(pFoot->GetTechnoType(), false);
			}

			if (groundValue <= 0)
				return true;

			std::vector<CellStruct> points { target };
			const double length = homeCell.DistanceFrom(target);

			if (length > 1.0)
			{
				for (double done = PlanStrikeApproachStepCells; done <= std::min(length, PlanStrikeApproachCells); done += PlanStrikeApproachStepCells)
				{
					points.push_back(CellStruct { static_cast<short>(target.X + (homeCell.X - target.X) * done / length),
						static_cast<short>(target.Y + (homeCell.Y - target.Y) * done / length) });
				}
			}

			auto const onWay = [&points](const CellStruct& cell)
			{
				return std::any_of(points.begin(), points.end(),
					[&cell](const CellStruct& point) { return point.DistanceFrom(cell) <= PlanStrikeGuardCells; });
			};

			int strength = 0;

			for (auto const& [uid, sighting] : macro.EnemySightings)
			{
				if (Unsorted::CurrentFrame - sighting.LastSeenFrame <= PlanStrikeGuardFrames && onWay(sighting.LastCoords))
					strength += sighting.Value;
			}

			for (auto const pBld : GetKnownEnemyBuildingCache(pHouse).Buildings)
			{
				if (pBld && pBld->IsAlive && !pBld->InLimbo && pBld->Type && pBld->Type->IsBaseDefense && pBld->Owner
					&& !pHouse->IsAlliedWith(pBld->Owner) && onWay(pBld->GetMapCoords()))
				{
					strength += GetCombatValueOfType(pBld->Type, false);
				}
			}

			const bool outgunned = groundValue * 100 < strength * PlanStrikeAdvantagePercent;
			bool taught = false;

			if (targetOwner >= 0 && targetOwner < 32)
			{
				auto const& lesson = macro.PlanHouseLessons[targetOwner];
				taught = lesson.Frame > 0 && Unsorted::CurrentFrame - lesson.Frame <= PlanLessonFrames
					&& groundValue * 100 < lesson.Strength * PlanLessonAdvantagePercent;
			}

			if (!outgunned && !taught)
				return true;

			return false;
		};

		auto startPlan = [&](NeuralPlanId id, std::vector<FootClass*> const& group, const CellStruct& target,
			const CellStruct& staging, DWORD targetID, int durationFrames)
		{
			auto const pTargetBuilding = targetID != 0 ? FindBuildingByID(targetID) : nullptr;
			const int targetOwner = (pTargetBuilding && pTargetBuilding->Owner) ? pTargetBuilding->Owner->ArrayIndex : -1;

			// Held back by the raid gate: the plan is weighed again after PlanGateRetryFrames.
			if (!raidMayStart(id, group, target, targetOwner))
			{
				macro.PlanNextAttemptFrame[static_cast<size_t>(id)] = Unsorted::CurrentFrame + PlanGateRetryFrames;
				return;
			}

			// An attack whose target or staging cell the path finder does not reach (IsPathDeadGoal)
			// waits too: its ground units would set out, fail to path and stand where they are
			// (27500 path failures in 36000 frames of a test match on Mount Yamantau, nearly all of
			// plan members). The plans that defend are not held.
			constexpr NeuralPlanId marching[] = { NeuralPlanId::EcoRaid, NeuralPlanId::Blackout, NeuralPlanId::Feint,
				NeuralPlanId::TimingRush, NeuralPlanId::ExpansionDenial, NeuralPlanId::SuperweaponRace, NeuralPlanId::HarvesterAmbush,
				NeuralPlanId::PreemptiveStrike, NeuralPlanId::RotatingRaids, NeuralPlanId::ArtilleryEcoSiege,
				NeuralPlanId::FactoryDecapitation, NeuralPlanId::CutReinforcements, NeuralPlanId::TechTreeCut,
				NeuralPlanId::RollingSiege, NeuralPlanId::SiegeBattery, NeuralPlanId::BaitAmbush, NeuralPlanId::Pincer,
				NeuralPlanId::ThreeFrontAssault, NeuralPlanId::AllyJointAttack, NeuralPlanId::CounterSquad, NeuralPlanId::EdgeBlitz };

			const bool marches = std::find(std::begin(marching), std::end(marching), id) != std::end(marching);
			auto const walks = [](FootClass* pFoot)
			{
				return pFoot && pFoot->WhatAmI() != AbstractType::Aircraft && !pFoot->IsInAir() && !pFoot->GetTechnoType()->Naval;
			};

			if (marches
				&& ((target != CellStruct::Empty && IsPathDeadGoal(pHouse, target))
					|| (staging != CellStruct::Empty && IsPathDeadGoal(pHouse, staging)))
				&& std::any_of(group.begin(), group.end(), walks))
			{
				macro.PlanNextAttemptFrame[static_cast<size_t>(id)] = Unsorted::CurrentFrame + PlanGateRetryFrames;
				return;
			}

			// Nor does such a plan start for a target or a staging cell on ground one of its walking
			// members does not get to by itself (FootReachesAlone): another island, a plateau. The
			// transports take idle units, never plan members, so the group stood at home until the
			// plan ran out (14 of 15 Factory Decapitation plans of a house in match #9885, every
			// member alive, 50 to 110 cells from a target across the water).
			if (marches)
			{
				auto const strands = [&](const CellStruct& cell, const BuildingClass* pBuilding)
				{
					return cell != CellStruct::Empty && std::any_of(group.begin(), group.end(), [&](FootClass* pFoot)
						{ return walks(pFoot) && !FootReachesAlone(pFoot, cell, pBuilding); });
				};

				if (strands(target, pTargetBuilding) || strands(staging, nullptr))
				{
					macro.PlanNextAttemptFrame[static_cast<size_t>(id)] = Unsorted::CurrentFrame + PlanGateRetryFrames;
					return;
				}
			}

			auto& plan = macro.Plans[static_cast<size_t>(id)];
			plan = NeuralPlanInstance {};
			plan.TargetOwner = targetOwner;
			plan.Active = true;
			plan.StartFrame = Unsorted::CurrentFrame;
			plan.Deadline = Unsorted::CurrentFrame + durationFrames;
			plan.BaseDuration = durationFrames;
			plan.ProgressCheckFrame = Unsorted::CurrentFrame;
			plan.TargetCell = target;
			plan.StagingCell = staging;
			plan.TargetID = targetID;

			NoteExpertPlanStart(pHouse, id);

			for (auto const pFoot : group)
			{
				plan.Members.push_back(pFoot->UniqueID);
				macro.PlanMembers.insert(pFoot->UniqueID);
				NoteExpertCommand(pHouse, pFoot, static_cast<int16_t>(id));
			}

			// Tank escort (PlanTankEscorts): a marching group with walking members and no main battle
			// tank, of a house that is no tank mass, takes the nearest free ones on its land.
			auto const isTankFoot = [](FootClass* pFoot)
			{
				auto const pUnit = abstract_cast<UnitClass*>(pFoot);
				return pUnit && IsMainBattleTankType(pUnit->Type);
			};

			if (marches && HouseExt::Fetch(pHouse)->NeuralAI_Archetype != TankMassArchetype
				&& std::any_of(group.begin(), group.end(), walks) && std::none_of(group.begin(), group.end(), isTankFoot))
			{
				int sumX = 0;
				int sumY = 0;
				int walkers = 0;
				int walkZone = -1;

				for (auto const pFoot : group)
				{
					if (!walks(pFoot))
						continue;

					const CellStruct at = pFoot->GetMapCoords();
					sumX += at.X;
					sumY += at.Y;
					walkers++;
					walkZone = walkZone < 0 ? ObjectZone(pFoot, MovementZone::Normal) : walkZone;
				}

				const CellStruct center { static_cast<short>(sumX / walkers), static_cast<short>(sumY / walkers) };
				std::vector<std::pair<double, FootClass*>> tanks;

				// The wide guard's escort tanks (TankGuardEscorts) go with a group holding vehicles the
				// floor gives an escort to, up to one for each and PlanTankEscorts at least.
				const bool wideGuard = TankGuardWideArchetype(HouseExt::Fetch(pHouse)->NeuralAI_Archetype);
				const size_t escorted = wideGuard
					? static_cast<size_t>(std::count_if(group.begin(), group.end(), [](FootClass* pFoot) { return IsTankEscortedVehicle(pFoot); }))
					: 0;
				const size_t mostEscorts = std::max(PlanTankEscorts, escorted);

				for (auto const& [uid, pFoot] : ownFoots)
				{
					const bool guardEscort = escorted > 0 && macro.TankGuardEscorts.contains(uid) && !macro.PlanMembers.contains(uid)
						&& !IsOnRepairTrip(pHouse, uid) && !IsInTransportOp(pHouse, uid);

					if (!isTankFoot(pFoot) || !(guardEscort || isFreeForPlan(pFoot)) || pFoot->GetHealthPercentage() < 0.5
						|| ObjectZone(pFoot, MovementZone::Normal) != walkZone)
					{
						continue;
					}

					tanks.push_back({ pFoot->GetMapCoords().DistanceFrom(center), pFoot });
				}

				std::sort(tanks.begin(), tanks.end(), [](const auto& a, const auto& b)
					{ return a.first < b.first || (a.first == b.first && a.second->UniqueID < b.second->UniqueID); });

				for (size_t i = 0; i < tanks.size() && i < mostEscorts; ++i)
				{
					auto const pTank = tanks[i].second;
					plan.Members.push_back(pTank->UniqueID);
					macro.PlanMembers.insert(pTank->UniqueID);
					macro.TankGuard.erase(pTank->UniqueID);
					macro.TankGuardEscorts.erase(pTank->UniqueID);
					macro.TankGuardPosts.erase(pTank->UniqueID);
					NoteExpertCommand(pHouse, pTank, static_cast<int16_t>(id));
				}

				// TEMP diagnostic: the tank escorts of the plans (30 per match).
				static int escortLogSerial = -1;
				static int escortLogCount = 0;

				if (escortLogSerial != NeuralMatchSerial)
				{
					escortLogSerial = NeuralMatchSerial;
					escortLogCount = 0;
				}

				if (escortLogCount++ < 30)
				{
					const size_t taken = std::min(tanks.size(), mostEscorts);
					Debug::Log("[NeuralAI] House %d plan %s: %d main battle tanks escort its %d members (%d free on its land, the farthest taken %d cells away, %d escorted vehicles), frame %d.\n",
						houseIdx, NeuralPlanNames[static_cast<size_t>(id)], static_cast<int>(taken), static_cast<int>(group.size()),
						static_cast<int>(tanks.size()), taken ? static_cast<int>(tanks[taken - 1].first) : 0, static_cast<int>(escorted),
						Unsorted::CurrentFrame);
				}
			}

			plan.InitialValue = std::max(1, memberValue(plan));
			plan.TargetSeenFrame = Unsorted::CurrentFrame;
			int strength = 0;

			for (auto const pFoot : group)
				strength += static_cast<int>(GetCombatValueOfType(pFoot->GetTechnoType(), false) * pFoot->GetHealthPercentage());

			plan.InitialStrength = std::max(1, strength);
			CountPlanOutcome(houseIdx, id, NeuralPlanOutcome::Started);
			gateOpenedUnstarted[static_cast<size_t>(id)] = false;
		};

		auto groupValueOf = [](const std::vector<FootClass*>& group) -> int
		{
			int value = 0;

			for (auto const pFoot : group)
				value += GetCombatValueOfType(pFoot->GetTechnoType(), false);

			return value;
		};

		// May a group of this value go for the target? Not while the lesson of a failed group stands
		// and this one is not clearly stronger.
		auto targetLessonAllows = [&](DWORD targetID, int groupValue) -> bool
		{
			auto const it = macro.PlanTargetLessons.find(targetID);

			return it == macro.PlanTargetLessons.end() || Unsorted::CurrentFrame - it->second.Frame > PlanLessonFrames
				|| groupValue * 100 >= it->second.Strength * PlanLessonAdvantagePercent;
		};

		// ---------------------------------------------------------------- Plan 1: Eco Raid
		// A small group of the fastest idle armed units attack-moves into a known enemy
		// refinery that has no known defenses around it, kills harvesters, then goes home.
		{
			auto& plan = macro.Plans[static_cast<size_t>(NeuralPlanId::EcoRaid)];

			if (plan.Active)
			{
				const int value = memberValue(plan);
				auto const pTargetCell = MapClass::Instance.TryGetCellAt(plan.TargetCell);

				if (value * 100 < plan.InitialValue * 40 || !pTargetCell)
				{
					finishPlan(NeuralPlanId::EcoRaid, false, true);
				}
				else if (Unsorted::CurrentFrame >= plan.Deadline
					|| (plan.Arrived && Unsorted::CurrentFrame - plan.ArrivedFrame >= 900))
				{
					// Survived the raid window: success when at least half the group is alive.
					finishPlan(NeuralPlanId::EcoRaid, value * 100 >= plan.InitialValue * 50, true);
				}
				else
				{
					int sumX = 0;
					int sumY = 0;
					int alive = 0;

					for (const DWORD uid : plan.Members)
					{
						auto const it = ownFoots.find(uid);

						if (it == ownFoots.end())
							continue;

						auto const pFoot = it->second;
						const CellStruct fc = pFoot->GetMapCoords();
						sumX += fc.X;
						sumY += fc.Y;
						alive++;

						// Re-issue only when the unit dropped out of its attack-move (the order resolves a
						// building cell to an adjacent free cell, so comparing destinations would churn).
						if (pFoot->Target == nullptr && pFoot->MegaMission != Mission::AttackMove && !IsOnRepairTrip(pHouse, uid))
							OrderFootToAttackMove(pFoot, pTargetCell);
					}

					if (alive > 0 && !plan.Arrived
						&& CellStruct { static_cast<short>(sumX / alive), static_cast<short>(sumY / alive) }.DistanceFrom(plan.TargetCell) <= 6.0)
					{
						plan.Arrived = true;
						plan.ArrivedFrame = Unsorted::CurrentFrame;
					}
				}
			}
			else if (Unsorted::CurrentFrame >= 4500)
			{
				BuildingClass* pBestRefinery = nullptr;
				double bestDist = 1e9;

				for (auto const pBld : BuildingClass::Array)
				{
					if (!pBld || pBld->InLimbo || !pBld->IsAlive || !pBld->Type || !pBld->Owner || pBld->Owner == pHouse
						|| pHouse->IsAlliedWith(pBld->Owner) || pBld->Owner->IsNeutral()
						|| (!pBld->Type->Refinery && !pBld->Type->ResourceDestination) || !IsEnemyBuildingKnown(pHouse, pBld))
					{
						continue;
					}

					const CellStruct rc = pBld->GetMapCoords();

					if (MapClass::Instance.GetMovementZoneType(rc, MovementZone::Normal, pBld->OnBridge) != homeZone
						|| CountKnownEnemyDefensesNear(pHouse, rc, 9.0, false) > 0)
					{
						continue;
					}

					const double d = rc.DistanceFrom(homeCell);

					if (d < bestDist)
					{
						bestDist = d;
						pBestRefinery = pBld;
					}
				}

				// Raids pay off against greedy, lightly defended economies.
				const int utility = pBestRefinery ? (250 + std::clamp(120 - foe.KnownDefenses * 15, 0, 120)) : -1000;

				if (pBestRefinery && shouldTryStart(NeuralPlanId::EcoRaid, utility))
				{
					std::vector<FootClass*> candidates;

					// Armed vehicles and armed infantry: many sides field their army on foot and keep few
					// vehicles besides harvesters and deployers, which never fit a raid.
					for (auto const& [uid, pFoot] : ownFoots)
					{
						if (!isFreeForPlan(pFoot) || IsAirborneThreatTechno(pFoot) || ObjectZone(pFoot, MovementZone::Normal) != homeZone)
							continue;

						if (auto const pUnit = abstract_cast<UnitClass*>(pFoot))
						{
							if (!pUnit->Type || pUnit->Type->Harvester || pUnit->Type->ResourceGatherer || pUnit->Type->Naval
								|| pUnit->Type->DeploysInto || IsAmphibiousOrAirTransport(pUnit->Type) || GroundRangeOf(pUnit->Type) <= 0)
							{
								continue;
							}

							candidates.push_back(pUnit);
						}
						else if (auto const pInf = abstract_cast<InfantryClass*>(pFoot))
						{
							if (!pInf->Type || pInf->Type->Engineer || IsSpyInfantryType(pInf->Type) || GroundRangeOf(pInf->Type) <= 0)
								continue;

							candidates.push_back(pInf);
						}
					}

					// Fastest first; ties by UniqueID keep the choice identical on every client.
					std::sort(candidates.begin(), candidates.end(), [](FootClass* a, FootClass* b)
						{
							const int sa = a->GetTechnoType()->Speed;
							const int sb = b->GetTechnoType()->Speed;
							return sa != sb ? sa > sb : a->UniqueID < b->UniqueID;
						});

					// Three vehicles make a raid; with soldiers among the fastest five it takes four of
					// them and up to six, since they hit harvesters softer.
					size_t raidSize = std::min<size_t>(candidates.size(), 5);
					bool onFoot = false;

					for (size_t i = 0; i < raidSize; ++i)
						onFoot = onFoot || candidates[i]->WhatAmI() == AbstractType::Infantry;

					if (onFoot)
						raidSize = std::min<size_t>(candidates.size(), 6);

					if (raidSize >= (onFoot ? 4u : 3u))
					{
						candidates.resize(raidSize);
						startPlan(NeuralPlanId::EcoRaid, candidates, pBestRefinery->GetMapCoords(), CellStruct::Empty,
							pBestRefinery->UniqueID, 3600);
					}
				}
			}
		}

		// Shared helpers for the plans below.
		auto valuableBuildingWorth = [](BuildingTypeClass* pT) -> int
		{
			if (pT->SuperWeapon != -1)
				return 400;
			if (pT->PowerBonus > 0)
				return 300;
			if (pT->Refinery || pT->ResourceDestination)
				return 280;
			if (pT->Factory != AbstractType::None || pT->ConstructionYard)
				return 250;
			return 0;
		};

		auto isKnownEnemyBuilding = [&](BuildingClass* pBld) -> bool
		{
			return pBld && !pBld->InLimbo && pBld->IsAlive && pBld->Type && pBld->Owner && pBld->Owner != pHouse
				&& !pHouse->IsAlliedWith(pBld->Owner) && !pBld->Owner->IsNeutral() && IsEnemyBuildingKnown(pHouse, pBld);
		};

		// The known enemy buildings of this tick (same order as BuildingClass::Array): the plan
		// loops below scan this short list instead of every building on the map.
		const std::vector<BuildingClass*> knownEnemyBuildingList = GetKnownEnemyBuildingCache(pHouse).Buildings;

		auto collectIdleGround = [&](bool requireAntiAir, bool allowSiegeDeployers = false) -> std::vector<FootClass*>
		{
			std::vector<FootClass*> result;

			for (auto const& [uid, pFoot] : ownFoots)
			{
				auto const pType = pFoot->GetTechnoType();
				auto const pUnit = abstract_cast<UnitClass*>(pFoot);

				if (!pType || pFoot->WhatAmI() == AbstractType::Aircraft || pType->Naval || IsAirborneThreatTechno(pFoot)
					|| (pUnit && (pUnit->Type->Harvester || pUnit->Type->ResourceGatherer
						|| (pUnit->Type->DeploysInto && !(allowSiegeDeployers && IsDeployableSiegeUnitType(pUnit->Type)))
						|| IsAmphibiousOrAirTransport(pUnit->Type) || IsRepairerUnit(pUnit)))
					|| !isFreeForPlan(pFoot)
					|| ObjectZone(pFoot, MovementZone::Normal) != homeZone)
				{
					continue;
				}

				if (auto const pInf = abstract_cast<InfantryClass*>(pFoot))
				{
					if (pInf->Type->Engineer || IsSpyInfantryType(pInf->Type))
						continue;
				}

				if (requireAntiAir ? GetCombatValueOfType(pType, true) > 0 : GroundRangeOf(pType) > 0)
					result.push_back(pFoot);
			}

			std::sort(result.begin(), result.end(), [](FootClass* a, FootClass* b) { return a->UniqueID < b->UniqueID; });

			return result;
		};

		// Can this air group destroy the building while its plan lasts? PlanAirStrikeSorties sorties
		// of every aircraft's full load must add up to the building's health.
		auto airGroupCanDestroy = [](const std::vector<FootClass*>& group, BuildingClass* pTarget) -> bool
		{
			int damage = 0;

			for (auto const pFoot : group)
			{
				if (auto const pAir = abstract_cast<AircraftClass*>(pFoot))
					damage += AirSortieDamageVs(pAir, pTarget);
			}

			return damage * PlanAirStrikeSorties >= pTarget->Health;
		};

		// Enemy strength a ground group meets on its straight way from `from` to a target: the known
		// defenses (unless withDefenses is false) and the units seen not longer ago than maxAge within
		// PlanStrikeGuardCells of the target or of the last PlanStrikeApproachCells of the way, each
		// counted once.
		auto knownEnemyStrengthOnWay = [&](const CellStruct& from, const CellStruct& to, int maxAge, bool withDefenses = true) -> int
		{
			std::vector<CellStruct> points { to };
			const double length = from.DistanceFrom(to);

			if (from != CellStruct::Empty && length > 1.0)
			{
				for (double done = PlanStrikeApproachStepCells; done <= std::min(length, PlanStrikeApproachCells); done += PlanStrikeApproachStepCells)
				{
					points.push_back(CellStruct { static_cast<short>(to.X + (from.X - to.X) * done / length),
						static_cast<short>(to.Y + (from.Y - to.Y) * done / length) });
				}
			}

			auto const onWay = [&points](const CellStruct& cell)
			{
				return std::any_of(points.begin(), points.end(),
					[&cell](const CellStruct& point) { return point.DistanceFrom(cell) <= PlanStrikeGuardCells; });
			};

			int strength = 0;

			for (auto const& [uid, sighting] : macro.EnemySightings)
			{
				if (Unsorted::CurrentFrame - sighting.LastSeenFrame <= maxAge && onWay(sighting.LastCoords))
					strength += sighting.Value;
			}

			for (auto const pBld : knownEnemyBuildingList)
			{
				if (withDefenses && isKnownEnemyBuilding(pBld) && pBld->Type->IsBaseDefense && onWay(pBld->GetMapCoords()))
					strength += GetCombatValueOfType(pBld->Type, false);
			}

			return strength;
		};

		// ---------------------------------------------------------------- Plan 5: Feint
		// While our main attack is on its way to an enemy house, a small fast decoy fires at a building
		// of that house far from where the attack is aimed, so the house sends its defenders there
		// instead. A Neural AI house answers whoever fires at its buildings with one intercept group at
		// a time, worth 1.3x the attacker (Base Threat Intercept), and the decoy pulls back once that
		// answer is around it. The old feint had no attack behind it and sent the four cheapest units
		// to the far side of the enemy base: they arrived 4 times in 100 (2026-11-13), and nothing used
		// the defenders they drew. Success = enemy value of the decoy's own seen coming around it near
		// its target. AnchorCell = where the main attack was aimed, AuxBaseline = enemy value around the
		// decoy when it first came within FeintNearCells of its target (-1 before), AuxPeak = the most
		// seen around it above that.
		{
			auto& plan = macro.Plans[static_cast<size_t>(NeuralPlanId::Feint)];

			auto enemyValueNear = [&](const CellStruct& cell, double radius) -> int
			{
				int value = 0;

				for (auto const& [uid, sighting] : macro.EnemySightings)
				{
					if (Unsorted::CurrentFrame - sighting.LastSeenFrame <= 150 && sighting.LastCoords.DistanceFrom(cell) <= radius)
						value += sighting.Value;
				}

				return value;
			};

			auto endFeint = [&](bool success, int rule = __builtin_LINE())
			{
				finishPlan(NeuralPlanId::Feint, success, true, rule);
			};

			if (plan.Active)
			{
				auto const pTarget = FindBuildingByID(plan.TargetID);
				const bool targetStands = pTarget && pTarget->IsAlive && !pTarget->InLimbo;
				const int value = memberValue(plan);
				int sumX = 0;
				int sumY = 0;
				int alive = 0;

				for (const DWORD uid : plan.Members)
				{
					if (auto const it = ownFoots.find(uid); it != ownFoots.end())
					{
						const CellStruct fc = it->second->GetMapCoords();
						sumX += fc.X;
						sumY += fc.Y;
						alive++;
					}
				}

				const CellStruct centre = alive > 0
					? CellStruct { static_cast<short>(sumX / alive), static_cast<short>(sumY / alive) }
					: CellStruct::Empty;

				// What stands around the building already is no answer: only what comes after the decoy
				// is near counts.
				if (centre != CellStruct::Empty && centre.DistanceFrom(plan.TargetCell) <= FeintNearCells)
				{
					const int around = enemyValueNear(centre, FeintDrawCells);

					if (plan.AuxBaseline < 0)
						plan.AuxBaseline = around;

					plan.AuxPeak = std::max(plan.AuxPeak, around - plan.AuxBaseline);
				}

				if (!plan.Arrived && centre != CellStruct::Empty && centre.DistanceFrom(plan.TargetCell) <= 8.0)
				{
					plan.Arrived = true;
					plan.ArrivedFrame = Unsorted::CurrentFrame;
				}

				if (plan.AuxPeak > 0 && plan.AuxPeak * 100 >= plan.InitialValue * FeintDrawPercent)
				{
					endFeint(true);
				}
				else if (!targetStands)
				{
					endFeint(false);
				}
				else if (alive == 0 || value * 100 < plan.InitialValue * FeintLossPercent)
				{
					endFeint(false);
				}
				else if (Unsorted::CurrentFrame >= plan.Deadline)
				{
					endFeint(false);
				}
				else if (plan.Arrived && Unsorted::CurrentFrame - plan.ArrivedFrame >= FeintAnswerWaitFrames)
				{
					endFeint(false);
				}
				else
				{
					// The decoy fires at the building: a unit that only walks near it is no threat to
					// any house.
					for (const DWORD uid : plan.Members)
					{
						auto const it = ownFoots.find(uid);

						if (it != ownFoots.end() && it->second->Target != pTarget)
						{
							it->second->SetTarget(pTarget);
							it->second->QueueMission(Mission::Attack, true);
						}
					}
				}
			}
			else if (!planShelved(NeuralPlanId::Feint)
				&& Unsorted::CurrentFrame >= macro.PlanNextAttemptFrame[static_cast<size_t>(NeuralPlanId::Feint)])
			{
				// Where our main attack is aimed and at whom: a main attack plan on its way, or a wave
				// sent out within FeintWaveFrames (its owner is that of the known building nearest to it).
				constexpr NeuralPlanId mainAttacks[] = { NeuralPlanId::TimingRush, NeuralPlanId::Pincer, NeuralPlanId::BaseTrade,
					NeuralPlanId::AllyJointAttack, NeuralPlanId::ThreeFrontAssault, NeuralPlanId::FactoryDecapitation };
				CellStruct front = CellStruct::Empty;
				int frontOwner = -1;

				for (const NeuralPlanId id : mainAttacks)
				{
					auto const& attack = macro.Plans[static_cast<size_t>(id)];

					if (attack.Active && !attack.Arrived && attack.TargetCell != CellStruct::Empty)
					{
						front = attack.TargetCell;
						frontOwner = attack.TargetOwner;
						break;
					}
				}

				if (front == CellStruct::Empty && macro.LastWaveTargetCell != CellStruct::Empty
					&& Unsorted::CurrentFrame - macro.LastWaveDispatchFrame <= FeintWaveFrames)
				{
					front = macro.LastWaveTargetCell;
				}

				if (front != CellStruct::Empty && frontOwner < 0)
				{
					double nearest = 1e9;

					for (auto const pBld : knownEnemyBuildingList)
					{
						if (isKnownEnemyBuilding(pBld) && pBld->GetMapCoords().DistanceFrom(front) < nearest)
						{
							nearest = pBld->GetMapCoords().DistanceFrom(front);
							frontOwner = pBld->Owner->ArrayIndex;
						}
					}
				}

				std::vector<FootClass*> idleArmy;

				// Only a human is drawn away from the main attack (deceptionTargets).
				if (frontOwner >= 0 && frontOwner < HouseClass::Array.Count
					&& !deceptionTargets(NeuralPlanId::Feint, HouseClass::Array.GetItem(frontOwner)))
				{
					frontOwner = -1;
				}

				if (frontOwner >= 0)
				{
					// Fastest first, so the decoy is there before the main attack; never an elite unit.
					idleArmy = collectIdleGround(false);
					std::erase_if(idleArmy, [](FootClass* pFoot) { return pFoot->Veterancy.IsElite(); });
					std::stable_sort(idleArmy.begin(), idleArmy.end(), [](FootClass* a, FootClass* b)
						{ return a->GetTechnoType()->Speed > b->GetTechnoType()->Speed; });
				}

				// Decoy target: the building of that house nearest to our home, far enough from the
				// main attack to pull its defenders away from it, with no known defense near (the decoy
				// would only die there) and a decoy worth PlanStrikeAdvantagePercent of what is known on
				// its way in (the raid gate does not weigh Feint).
				BuildingClass* pDecoy = nullptr;
				std::vector<FootClass*> decoyGroup;
				double bestTrip = 1e9;

				if (idleArmy.size() >= FeintIdleMin)
				{
					// Not a building our units are already firing at, nor a unit folded into a building:
					// either is gone before the decoy gets there (32 of 80 feints ended with their
					// target gone, 2026-11-15).
					std::unordered_set<const AbstractClass*> underOurFire;

					for (auto const& [uid, pOwn] : ownFoots)
					{
						if (pOwn->Target)
							underOurFire.insert(pOwn->Target);
					}

					for (auto const pBld : knownEnemyBuildingList)
					{
						if (!isKnownEnemyBuilding(pBld) || pBld->Owner->ArrayIndex != frontOwner || pBld->Type->CanBeOccupied
							|| pBld->Type->Insignificant || pBld->Type->UndeploysInto || underOurFire.contains(pBld))
						{
							continue;
						}

						const CellStruct bc = pBld->GetMapCoords();
						const double gap = bc.DistanceFrom(front);
						const double trip = bc.DistanceFrom(homeCell);

						if (gap < FeintGapCells || gap > FeintReachCells || trip >= bestTrip
							|| MapClass::Instance.GetMovementZoneType(bc, MovementZone::Normal, pBld->OnBridge) != homeZone
							|| CountKnownEnemyDefensesNear(pHouse, bc, FeintTowerClearCells, false) > 0)
						{
							continue;
						}

						std::vector<FootClass*> group;

						for (auto const pFoot : idleArmy)
						{
							if (group.size() < FeintDecoySize && HurtsBuilding(pFoot->GetTechnoType(), pBld))
								group.push_back(pFoot);
						}

						const int value = groupValueOf(group);

						if (group.size() < FeintDecoySize || !targetLessonAllows(pBld->UniqueID, value)
							|| value * 100 < knownEnemyStrengthOnWay(homeCell, bc, PlanStrikeGuardFrames) * PlanStrikeAdvantagePercent)
						{
							continue;
						}

						bestTrip = trip;
						pDecoy = pBld;
						decoyGroup = std::move(group);
					}
				}

				const int utility = pDecoy ? (200 + std::clamp(static_cast<int>(idleArmy.size()) * 15, 0, 200)) : -1000;

				if (pDecoy && shouldTryStart(NeuralPlanId::Feint, utility))
				{
					startPlan(NeuralPlanId::Feint, decoyGroup, pDecoy->GetMapCoords(), CellStruct::Empty, pDecoy->UniqueID, FeintPlanFrames);

					if (plan.Active)
					{
						plan.AnchorCell = front;
						plan.AuxBaseline = -1;
					}
				}
			}
		}

		// ---------------------------------------------------------------- Plan 6: Timing Rush
		// Early in the match, against a known base with almost no defenses and a small seen
		// army, hit its production (factory or ConYard) with everything idle before it scales.
		{
			auto& plan = macro.Plans[static_cast<size_t>(NeuralPlanId::TimingRush)];

			if (plan.Active)
			{
				auto const pTarget = FindBuildingByID(plan.TargetID);
				const int value = memberValue(plan);

				if (!pTarget)
					finishPlan(NeuralPlanId::TimingRush, true, false);
				else if (value * 100 < plan.InitialValue * 40 || Unsorted::CurrentFrame >= plan.Deadline)
					finishPlan(NeuralPlanId::TimingRush, false, true);
				else if (auto const pTargetCell = pTarget->GetCell())
				{
					for (const DWORD uid : plan.Members)
					{
						auto const it = ownFoots.find(uid);

						if (it != ownFoots.end() && it->second->Target == nullptr && it->second->MegaMission != Mission::AttackMove)
							OrderFootToAttackMove(it->second, pTargetCell);
					}
				}
			}
			else if (Unsorted::CurrentFrame <= 13500)
			{
				std::vector<FootClass*> idleArmy = collectIdleGround(false);
				int armyValue = 0;

				for (auto const pFoot : idleArmy)
					armyValue += GetCombatValueOfType(pFoot->GetTechnoType(), false);

				BuildingClass* pTarget = nullptr;
				double bestDist = 1e9;
				int knownBuildings = 0;

				for (auto const pBld : knownEnemyBuildingList)
				{
					if (!isKnownEnemyBuilding(pBld))
						continue;

					knownBuildings++;

					if ((pBld->Type->Factory == AbstractType::None && !pBld->Type->ConstructionYard)
						|| ObjectZone(pBld, MovementZone::Normal) != homeZone)
					{
						continue;
					}

					const double d = pBld->GetMapCoords().DistanceFrom(homeCell);

					if (d < bestDist)
					{
						bestDist = d;
						pTarget = pBld;
					}
				}

				const bool weakEnemy = foe.KnownDefenses <= 1 && foe.TotalValue * 10 <= armyValue * 6;
				const int utility = (pTarget && knownBuildings >= 3 && weakEnemy && idleArmy.size() >= 6)
					? (260 + std::clamp(armyValue - foe.TotalValue, 0, 240))
					: -1000;

				if (utility > 0 && shouldTryStart(NeuralPlanId::TimingRush, utility))
				{
					idleArmy.resize(std::min<size_t>(idleArmy.size(), 12));
					startPlan(NeuralPlanId::TimingRush, idleArmy, pTarget->GetMapCoords(), CellStruct::Empty, pTarget->UniqueID,
						4500);
				}
			}
		}

		// ---------------------------------------------------------------- Plan 7: Heavy Air Push
		// A heavy, slow flying unit (Strength >= 600 with a ground weapon, e.g. an airship)
		// pushes into a valuable known building with mobile anti-air escorts following below.
		{
			auto& plan = macro.Plans[static_cast<size_t>(NeuralPlanId::HeavyAirPush)];

			if (plan.Active)
			{
				auto const pTarget = FindBuildingByID(plan.TargetID);
				auto const itHeavy = plan.Members.empty() ? ownFoots.end() : ownFoots.find(plan.Members.front());

				if (!pTarget)
				{
					finishPlan(NeuralPlanId::HeavyAirPush, true, false);
				}
				else if (itHeavy == ownFoots.end() || Unsorted::CurrentFrame >= plan.Deadline)
				{
					finishPlan(NeuralPlanId::HeavyAirPush, false, true);
				}
				else
				{
					auto const pHeavy = itHeavy->second;

					if (pHeavy->Target != pTarget)
					{
						pHeavy->SetTarget(pTarget);
						pHeavy->QueueMission(Mission::Attack, true);
					}

					// Escorts walk under the heavy flyer so its air cover moves with it.
					if (auto const pBelow = MapClass::Instance.TryGetCellAt(pHeavy->GetMapCoords()))
					{
						for (size_t m = 1; m < plan.Members.size(); ++m)
						{
							auto const it = ownFoots.find(plan.Members[m]);

							if (it != ownFoots.end() && it->second->Target == nullptr)
								IssueMoveOrder(it->second, pBelow, 4.0);
						}
					}
				}
			}
			else
			{
				FootClass* pHeavy = nullptr;

				for (auto const& [uid, pFoot] : ownFoots)
				{
					auto const pType = pFoot->GetTechnoType();

					if (!pType || pType->Strength < 600 || !IsAirborneThreatTechno(pFoot) || GroundRangeOf(pType) <= 0
						|| macro.PlanMembers.contains(uid) || pFoot->Target != nullptr || pFoot->GetHealthPercentage() < 0.35)
					{
						continue;
					}

					if (!pHeavy || uid < pHeavy->UniqueID)
						pHeavy = pFoot;
				}

				BuildingClass* pTarget = nullptr;
				int bestScore = INT_MIN;

				if (pHeavy)
				{
					for (auto const pBld : knownEnemyBuildingList)
					{
						if (!isKnownEnemyBuilding(pBld))
							continue;

						const int worth = valuableBuildingWorth(pBld->Type);

						if (worth <= 0)
							continue;

						const CellStruct bc = pBld->GetMapCoords();
						const int score = worth - CountKnownEnemyDefensesNear(pHouse, bc, 10.0, true) * 120
							- static_cast<int>(bc.DistanceFrom(pHeavy->GetMapCoords()));

						if (score > bestScore)
						{
							bestScore = score;
							pTarget = pBld;
						}
					}
				}

				const int utility = pTarget ? (230 + std::clamp(bestScore / 2, 0, 200)) : -1000;

				if (pTarget && shouldTryStart(NeuralPlanId::HeavyAirPush, utility))
				{
					std::vector<FootClass*> group { pHeavy };
					std::vector<FootClass*> escorts = collectIdleGround(true);

					for (size_t e = 0; e < escorts.size() && e < 3; ++e)
						group.push_back(escorts[e]);

					startPlan(NeuralPlanId::HeavyAirPush, group, pTarget->GetMapCoords(), CellStruct::Empty, pTarget->UniqueID,
						5400 + static_cast<int>(pTarget->GetMapCoords().DistanceFrom(pHeavy->GetMapCoords())) * 45);
				}
			}
		}

		// ---------------------------------------------------------------- Plan 3: Air Strike
		// Idle armed aircraft hit a valuable known building with the fewest known anti-air
		// defenses around it (power, refinery, factory, tech, superweapon).
		{
			auto& plan = macro.Plans[static_cast<size_t>(NeuralPlanId::AirStrike)];

			if (plan.Active)
			{
				auto const pTarget = FindBuildingByID(plan.TargetID);
				const int value = memberValue(plan);

				if (!pTarget)
				{
					finishPlan(NeuralPlanId::AirStrike, true, false);
				}
				else if (value * 100 < plan.InitialValue * 50 || Unsorted::CurrentFrame >= plan.Deadline)
				{
					finishPlan(NeuralPlanId::AirStrike, false, false);
				}
				else
				{
					for (const DWORD uid : plan.Members)
					{
						auto const it = ownFoots.find(uid);

						if (it == ownFoots.end())
							continue;

						auto const pAir = abstract_cast<AircraftClass*>(it->second);

						// Re-arm and strike again while the target stands (aircraft return to pads by themselves).
						if (ReadyPlanAircraft(pAir))
							OrderAircraftAttack(pAir, pTarget);
					}
				}
			}
			else
			{
				std::vector<FootClass*> strikers;

				// The strike aircraft of the wings fly with their wing only (UpdateAirWings, user approved,
				// 2026-11-19: «Sí, hazlo»): this plan took them from their pads in pairs, 23 times for 46
				// aircraft in two matches of b0ec09525, 41 of them lost.
				for (auto const& [uid, pFoot] : ownFoots)
				{
					auto const pAir = abstract_cast<AircraftClass*>(pFoot);

					if (isFreeAirStriker(pAir) && !IsWingStrikeType(pAir->Type))
						strikers.push_back(pAir);
				}

				BuildingClass* pBestTarget = nullptr;
				int bestTargetScore = INT_MIN;

				if (strikers.size() >= 2)
				{
					for (auto const pBld : BuildingClass::Array)
					{
						if (!pBld || pBld->InLimbo || !pBld->IsAlive || !pBld->Type || !pBld->Owner || pBld->Owner == pHouse
							|| pHouse->IsAlliedWith(pBld->Owner) || pBld->Owner->IsNeutral() || !IsEnemyBuildingKnown(pHouse, pBld))
						{
							continue;
						}

						auto const pT = pBld->Type;
						int worth = 0;

						if (pT->SuperWeapon != -1)
							worth = 400;
						else if (pT->PowerBonus > 0)
							worth = 300;
						else if (pT->Refinery || pT->ResourceDestination)
							worth = 280;
						else if (pT->Factory != AbstractType::None || pT->ConstructionYard)
							worth = 250;

						if (worth <= 0)
							continue;

						const int aaNear = CountKnownEnemyDefensesNear(pHouse, pBld->GetMapCoords(), 10.0, true);
						const int targetScore = worth - aaNear * 220;

						// Late game nearly every valuable building has 2+ AA around it: a bigger wing may
						// still go (up to one AA site per two strikers), the score keeps the least covered.
						if (aaNear > std::max<int>(1, static_cast<int>(strikers.size()) / 2) || targetScore <= bestTargetScore)
							continue;

						// At least two of the strikers must damage it (some bombs do nothing to structures),
						// and the wing that would fly (the first six of them by id) must be able to destroy it.
						std::vector<FootClass*> wing;

						for (auto const pStriker : strikers)
						{
							if (AirSortieDamageVs(static_cast<AircraftClass*>(pStriker), pBld) > 0)
								wing.push_back(pStriker);
						}

						const size_t able = wing.size();
						std::sort(wing.begin(), wing.end(), [](FootClass* a, FootClass* b) { return a->UniqueID < b->UniqueID; });
						wing.resize(std::min<size_t>(wing.size(), 6));

						if (able >= 2 && airGroupCanDestroy(wing, pBld))
						{
							bestTargetScore = targetScore;
							pBestTarget = pBld;
						}
					}
				}

				const int utility = pBestTarget ? (200 + bestTargetScore / 2) : -1000;

				if (pBestTarget && shouldTryStart(NeuralPlanId::AirStrike, utility))
				{
					std::erase_if(strikers, [pBestTarget](FootClass* pFoot)
					{
						return AirSortieDamageVs(static_cast<AircraftClass*>(pFoot), pBestTarget) <= 0;
					});

					std::sort(strikers.begin(), strikers.end(), [](FootClass* a, FootClass* b) { return a->UniqueID < b->UniqueID; });
					strikers.resize(std::min<size_t>(strikers.size(), 6));
					startPlan(NeuralPlanId::AirStrike, strikers, pBestTarget->GetMapCoords(), CellStruct::Empty,
						pBestTarget->UniqueID, 2700);

					for (auto const pStriker : strikers)
						OrderFootAttack(pStriker, pBestTarget);
				}
			}
		}

		// =================================================================================
		// Plans 8..22 (slots 96..110). Shared helpers first; every choice is sorted or keyed by
		// UniqueID so all clients decide the same, and only fog-fair knowledge is used.
		// =================================================================================
		auto findOwnFoot = [&](DWORD uid) -> FootClass*
		{
			auto const it = ownFoots.find(uid);
			return it != ownFoots.end() ? it->second : nullptr;
		};

		// A plan member on a repair trip follows the trip (UpdateRepairTrips); the group orders below
		// take it again once it is back.
		auto findOrderableFoot = [&](DWORD uid) -> FootClass*
		{
			return IsOnRepairTrip(pHouse, uid) ? nullptr : findOwnFoot(uid);
		};

		// Siege screen (SiegeScreen*): the centre of the guns, on its ground, of the active plan a main
		// battle tank of an archetype 7 house marches with (unfolded siege guns too). Empty: none.
		const bool screenHouse = HouseExt::Fetch(pHouse)->NeuralAI_Archetype == SiegeScreenArchetype;

		auto screenGunsCenter = [&](FootClass* pFoot) -> CellStruct
		{
			auto const pTank = abstract_cast<UnitClass*>(pFoot);

			if (!screenHouse || !pTank || !pTank->Type || !IsMainBattleTankType(pTank->Type) || !macro.PlanMembers.contains(pTank->UniqueID))
				return CellStruct::Empty;

			const int zone = ObjectZone(pTank, MovementZone::Normal);

			for (auto const& plan : macro.Plans)
			{
				if (!plan.Active || std::find(plan.Members.begin(), plan.Members.end(), pTank->UniqueID) == plan.Members.end())
					continue;

				int sumX = 0;
				int sumY = 0;
				int guns = 0;

				for (const DWORD uid : plan.Members)
				{
					CellStruct at = CellStruct::Empty;

					if (auto const pMember = findOwnFoot(uid); pMember && IsSiegeScreenGun(pMember) && ObjectZone(pMember, MovementZone::Normal) == zone)
						at = pMember->GetMapCoords();
					else if (auto const itBld = ownSiegeBuildings.find(uid); itBld != ownSiegeBuildings.end() && itBld->second)
						at = itBld->second->GetMapCoords();

					if (at != CellStruct::Empty)
					{
						sumX += at.X;
						sumY += at.Y;
						guns++;
					}
				}

				return guns > 0 ? CellStruct { static_cast<short>(sumX / guns), static_cast<short>(sumY / guns) } : CellStruct::Empty;
			}

			return CellStruct::Empty;
		};

		// True when the siege screen holds this member (HoldSiegeScreen): the group order is not for it.
		auto siegeScreenHolds = [&](FootClass* pFoot, const CellStruct& goal)
		{
			if (!screenHouse)
				return false;

			const CellStruct guns = screenGunsCenter(pFoot);

			return guns != CellStruct::Empty && HoldSiegeScreen(pHouse, pFoot, goal, guns);
		};

		auto findTechnoByID = [](DWORD uid) -> TechnoClass*
		{
			for (auto const pTech : TechnoClass::Array)
			{
				if (pTech && pTech->UniqueID == uid)
					return (pTech->IsAlive && !pTech->InLimbo) ? pTech : nullptr;
			}

			return nullptr;
		};

		auto zoneOf = [](const CellStruct& cell) -> int
		{
			return MapClass::Instance.GetMovementZoneType(cell, MovementZone::Normal, false);
		};

		auto groupValue = [&](const std::vector<DWORD>& uids) -> int
		{
			int value = 0;

			for (const DWORD uid : uids)
			{
				if (auto const pFoot = findOwnFoot(uid))
					value += GetCombatValueOfType(pFoot->GetTechnoType(), false);
			}

			return value;
		};

		// What a group brings to its fight right now: combat value weighted by health, without the
		// members away on a repair trip. The early abort checks compare it with the start value.
		auto groupStrength = [&](const std::vector<DWORD>& uids) -> int
		{
			int value = 0;

			for (const DWORD uid : uids)
			{
				if (auto const pFoot = findOrderableFoot(uid))
					value += static_cast<int>(GetCombatValueOfType(pFoot->GetTechnoType(), false) * pFoot->GetHealthPercentage());
			}

			return value;
		};

		auto groupCentroid = [&](const std::vector<DWORD>& uids) -> CellStruct
		{
			int sumX = 0;
			int sumY = 0;
			int alive = 0;

			for (const DWORD uid : uids)
			{
				if (auto const pFoot = findOwnFoot(uid))
				{
					const CellStruct c = pFoot->GetMapCoords();
					sumX += c.X;
					sumY += c.Y;
					alive++;
				}
			}

			return alive > 0 ? CellStruct { static_cast<short>(sumX / alive), static_cast<short>(sumY / alive) } : CellStruct::Empty;
		};

		auto attackMoveGroup = [&](const std::vector<DWORD>& uids, CellClass* pCell)
		{
			if (!pCell)
				return;

			for (const DWORD uid : uids)
			{
				auto const pFoot = findOrderableFoot(uid);

				if (pFoot && siegeScreenHolds(pFoot, pCell->MapCoords))
					continue;

				if (pFoot && pFoot->Target == nullptr && pFoot->MegaMission != Mission::AttackMove)
					OrderFootToAttackMove(pFoot, pCell);
			}
		};

		// A member that fights (its target within its ground range, at least 6 cells, and PlanFightLeashCells
		// more) is left to its fight: the group's move each AI tick took it away from it and the engine back
		// (Mastermind and Yuri Prime against the engine: about 750 turns in 7 matches of b498ac350).
		auto memberFights = [&](FootClass* pFoot) -> bool
		{
			if (!pFoot || !pFoot->Target)
				return false;

			const int range = std::max(GroundRangeOf(pFoot->GetTechnoType()), 6 * 256);

			return pFoot->DistanceFrom(pFoot->Target) <= range + PlanFightLeashCells * 256;
		};

		auto moveGroup = [&](const std::vector<DWORD>& uids, CellClass* pCell, double keepWithin)
		{
			if (!pCell)
				return;

			for (const DWORD uid : uids)
			{
				if (auto const pFoot = findOrderableFoot(uid); pFoot && !siegeScreenHolds(pFoot, pCell->MapCoords) && !memberFights(pFoot))
					IssueMoveOrder(pFoot, pCell, keepWithin);
			}
		};

		auto attackTargetGroup = [&](const std::vector<DWORD>& uids, TechnoClass* pTarget)
		{
			for (const DWORD uid : uids)
			{
				auto const pFoot = findOrderableFoot(uid);

				if (pFoot && pTarget && pFoot->Target != pTarget)
					OrderFootAttack(pFoot, pTarget);
			}
		};

		auto seenEnemyValueNear = [&](const CellStruct& cell, double radius, int maxAge) -> int
		{
			int value = 0;

			for (auto const& [uid, sighting] : macro.EnemySightings)
			{
				if (Unsorted::CurrentFrame - sighting.LastSeenFrame <= maxAge && sighting.LastCoords.DistanceFrom(cell) <= radius)
					value += sighting.Value;
			}

			return value;
		};

		auto seenEnemyCentroidNear = [&](const CellStruct& cell, double radius, int maxAge) -> CellStruct
		{
			long long sumX = 0;
			long long sumY = 0;
			long long weight = 0;

			for (auto const& [uid, sighting] : macro.EnemySightings)
			{
				if (Unsorted::CurrentFrame - sighting.LastSeenFrame <= maxAge && sighting.LastCoords.DistanceFrom(cell) <= radius)
				{
					const int w = std::max(1, sighting.Value);
					sumX += static_cast<long long>(sighting.LastCoords.X) * w;
					sumY += static_cast<long long>(sighting.LastCoords.Y) * w;
					weight += w;
				}
			}

			return weight > 0 ? CellStruct { static_cast<short>(sumX / weight), static_cast<short>(sumY / weight) } : CellStruct::Empty;
		};

		// Nearest legal land cell of a movement zone around a point (ring search, fixed order).
		auto landCellInZone = [](const CellStruct& center, int zone, int searchRadius) -> CellClass*
		{
			// The first legal land cell from the center out, of the zone when one is given (FindWaitCellNear).
			NeuralWaitAsker asker {};
			asker.Zone = zone;
			NeuralWaitSearch search {};
			search.MaxRing = searchRadius;

			return FindWaitCellNear(center, search, asker, WaitLegal | WaitLand | (zone >= 0 ? WaitZone : 0u),
				[](const CellStruct&, const CellClass*) { return false; });
		};

		// Point at `cells` from `from` toward `to`, shifted `side` cells to the left of that line.
		auto pointAlong = [](const CellStruct& from, const CellStruct& to, double cells, double side) -> CellStruct
		{
			const double dx = static_cast<double>(to.X - from.X);
			const double dy = static_cast<double>(to.Y - from.Y);
			const double len = std::max(1.0, std::sqrt(dx * dx + dy * dy));
			const double ux = dx / len;
			const double uy = dy / len;

			return CellStruct { static_cast<short>(from.X + ux * cells - uy * side), static_cast<short>(from.Y + uy * cells + ux * side) };
		};

		auto isArmedGroundFoot = [&](FootClass* pFoot) -> bool
		{
			auto const pType = pFoot ? pFoot->GetTechnoType() : nullptr;

			// Slaves work for their slave master and take no other orders.
			if (!pType || pFoot->SlaveOwner || pFoot->WhatAmI() == AbstractType::Aircraft || pType->Naval || IsAirborneThreatTechno(pFoot)
				|| GroundRangeOf(pType) <= 0)
			{
				return false;
			}

			if (auto const pUnit = abstract_cast<UnitClass*>(pFoot))
			{
				if (pUnit->Type->Harvester || pUnit->Type->ResourceGatherer || pUnit->Type->DeploysInto
					|| IsAmphibiousOrAirTransport(pUnit->Type))
				{
					return false;
				}
			}
			else if (auto const pInf = abstract_cast<InfantryClass*>(pFoot))
			{
				if (pInf->Type->Engineer || IsSpyInfantryType(pInf->Type))
					return false;
			}

			return true;
		};

		auto sortFastest = [](std::vector<FootClass*>& feet)
		{
			std::sort(feet.begin(), feet.end(), [](FootClass* a, FootClass* b)
				{
					const int sa = a->GetTechnoType()->Speed;
					const int sb = b->GetTechnoType()->Speed;
					return sa != sb ? sa > sb : a->UniqueID < b->UniqueID;
				});
		};

		auto feetValue = [](const std::vector<FootClass*>& feet) -> int
		{
			int value = 0;

			for (auto const pFoot : feet)
				value += GetCombatValueOfType(pFoot->GetTechnoType(), false);

			return value;
		};

		// Known enemy buildings on our landmass: centroid and count.
		int knownEnemyHomeBuildings = 0;
		CellStruct knownEnemyCentroid = CellStruct::Empty;
		{
			long long sumX = 0;
			long long sumY = 0;

			for (auto const pBld : knownEnemyBuildingList)
			{
				if (!isKnownEnemyBuilding(pBld))
					continue;

				const CellStruct bc = pBld->GetMapCoords();

				if (zoneOf(bc) != homeZone)
					continue;

				sumX += bc.X;
				sumY += bc.Y;
				knownEnemyHomeBuildings++;
			}

			if (knownEnemyHomeBuildings > 0)
			{
				knownEnemyCentroid = CellStruct { static_cast<short>(sumX / knownEnemyHomeBuildings),
					static_cast<short>(sumY / knownEnemyHomeBuildings) };
			}
		}

		// Our armed base defenses (ground range) near home: count and centroid (rally point).
		int ownHomeDefenses = 0;
		CellStruct ownDefenseCentroid = CellStruct::Empty;
		{
			int sumX = 0;
			int sumY = 0;

			for (auto const pBld : pHouse->Buildings)
			{
				if (!pBld || pBld->InLimbo || !pBld->IsAlive || !pBld->Type || !pBld->Type->IsBaseDefense
					|| IsTunnelNetworkBuilding(pBld->Type) || GroundRangeOf(pBld->Type) <= 0)
				{
					continue;
				}

				const CellStruct bc = pBld->GetMapCoords();

				if (bc.DistanceFrom(homeCell) > 25.0 || zoneOf(bc) != homeZone)
					continue;

				sumX += bc.X;
				sumY += bc.Y;
				ownHomeDefenses++;
			}

			if (ownHomeDefenses > 0)
				ownDefenseCentroid = CellStruct { static_cast<short>(sumX / ownHomeDefenses), static_cast<short>(sumY / ownHomeDefenses) };
		}

		auto planReady = [&](NeuralPlanId id) -> bool
		{
			const size_t i = static_cast<size_t>(id);
			return !macro.Plans[i].Active && Unsorted::CurrentFrame >= macro.PlanNextAttemptFrame[i];
		};

		auto planOf = [&](NeuralPlanId id) -> NeuralPlanInstance& { return macro.Plans[static_cast<size_t>(id)]; };

		// ---------------------------------------------------------------- Outgunned raids turn back
		// Raids and strikes that send a ground group at a distant target. A group that, still far
		// from its goal, has more enemy strength around it than it has itself (units seen near it,
		// known defenses that reach it) goes home instead of walking on until it is dead, and its
		// plan waits before it tries again. In the focus matches of 2026-11-13 the groups of Feint,
		// Superweapon Race and Tech Tree Cut lost three of every four members 30 to 50 cells short
		// of the target, and Feint arrived 4 times in 100. The siege batteries turn back the same
		// way: Rolling Siege and Siege Battery marched on until a third of the battery was left, 30
		// to 57 cells short of the defense they were after (76 of 96 and 280 of 471 plans,
		// 2026-11-14). Feint is not in the list: the enemy coming at its decoy is what it is after,
		// and it pulls back on its own.
		{
			constexpr NeuralPlanId raidPlans[] = { NeuralPlanId::EcoRaid, NeuralPlanId::Blackout,
				NeuralPlanId::TimingRush, NeuralPlanId::ExpansionDenial, NeuralPlanId::SuperweaponRace, NeuralPlanId::HarvesterAmbush,
				NeuralPlanId::PreemptiveStrike, NeuralPlanId::RotatingRaids, NeuralPlanId::ArtilleryEcoSiege,
				NeuralPlanId::FactoryDecapitation, NeuralPlanId::CutReinforcements, NeuralPlanId::TechTreeCut,
				NeuralPlanId::RollingSiege, NeuralPlanId::SiegeBattery, NeuralPlanId::CounterSquad };

			for (const NeuralPlanId id : raidPlans)
			{
				auto& plan = planOf(id);

				if (!plan.Active || plan.TargetCell == CellStruct::Empty)
					continue;

				// The groups of the plan and the goal of each. Rotating Raids sends its first AuxBaseline
				// members at the target and the rest at the staging cell: each group is judged where it
				// is (the centre of both lay between them, far from either goal and from the enemy, and
				// 208 of its 266 turn backs of 2026-11-17 saw no enemy at all). While its outgunned group
				// falls back to the other one (group B then), only the other one is judged.
				const bool twoGroups = id == NeuralPlanId::RotatingRaids && plan.StagingCell != CellStruct::Empty;
				const bool regrouping = twoGroups && plan.RegroupFrame >= 0;
				const size_t split = twoGroups ? std::min<size_t>(plan.Members.size(), static_cast<size_t>(std::max(0, plan.AuxBaseline)))
					: plan.Members.size();
				bool anyCommitted = false;
				bool outgunned = false;
				int alive = 0;
				int worstEnemy = -1;
				int worstUnits = 0;
				int worstStrength = 0;
				double worstToGoal = 0.0;
				CellStruct worstCentre = CellStruct::Empty;
				std::array<bool, 2> groupOutgunned {};
				std::array<int, 2> groupAlive {};
				std::array<CellStruct, 2> groupCentre { CellStruct::Empty, CellStruct::Empty };

				for (int leg = 0; leg < (twoGroups ? 2 : 1); ++leg)
				{
					if (regrouping && leg == 1)
						continue;

					int sumX = 0;
					int sumY = 0;
					int legAlive = 0;
					int strength = 0;

					for (size_t i = leg == 0 ? 0 : split; i < (leg == 0 ? split : plan.Members.size()); ++i)
					{
						auto const pFoot = findOwnFoot(plan.Members[i]);

						if (!pFoot || pFoot->WhatAmI() == AbstractType::Aircraft || pFoot->GetTechnoType()->Naval || pFoot->IsInAir())
							continue;

						const CellStruct cell = pFoot->GetMapCoords();
						sumX += cell.X;
						sumY += cell.Y;
						legAlive++;
						strength += static_cast<int>(GetCombatValueOfType(pFoot->GetTechnoType(), false) * pFoot->GetHealthPercentage());
					}

					if (legAlive == 0)
						continue;

					alive += legAlive;
					const CellStruct centre { static_cast<short>(sumX / legAlive), static_cast<short>(sumY / legAlive) };
					groupAlive[leg] = legAlive;
					groupCentre[leg] = centre;
					double toGoal = centre.DistanceFrom(twoGroups && leg == 1 ? plan.StagingCell : plan.TargetCell);

					if (!twoGroups && plan.StagingCell != CellStruct::Empty)
						toGoal = std::min(toGoal, centre.DistanceFrom(plan.StagingCell));

					if (toGoal <= PlanOutgunnedCommittedCells)
					{
						anyCommitted = true;
						continue;
					}

					const int units = seenEnemyValueNear(centre, PlanOutgunnedSightCells, PlanOutgunnedSightFrames);
					int enemy = units;

					for (auto const pBld : knownEnemyBuildingList)
					{
						if (!isKnownEnemyBuilding(pBld) || !pBld->Type->IsBaseDefense)
							continue;

						const int range = GroundRangeOf(pBld->Type);

						if (range > 0 && pBld->GetMapCoords().DistanceFrom(centre) * Unsorted::LeptonsPerCell <= range + 2 * Unsorted::LeptonsPerCell)
							enemy += GetCombatValueOfType(pBld->Type, false);
					}

					groupOutgunned[leg] = enemy * 100 > strength * PlanOutgunnedRatioPercent;
					outgunned = outgunned || groupOutgunned[leg];

					// The group worst off (enemy against strength) speaks for the plan.
					if (worstEnemy < 0 || static_cast<long long>(enemy) * std::max(1, worstStrength)
						> static_cast<long long>(worstEnemy) * std::max(1, strength))
					{
						worstEnemy = enemy;
						worstUnits = units;
						worstStrength = strength;
						worstToGoal = toGoal;
						worstCentre = centre;
					}
				}

				if (alive == 0 || worstEnemy < 0)
					continue;

				// Losses before arriving tell the same when the shooters are out of the group's sight
				// (long guns, aircraft, defenses it does not know yet). Not once a group of Rotating
				// Raids is at its goal or has been: what it loses there is the raid.
				const bool bleeding = !(twoGroups && (anyCommitted || plan.Arrived))
					&& memberValue(plan) * 100 < plan.InitialValue * PlanEnRouteLossPercent;

				if (!outgunned && !bleeding)
					continue;

				// Rotating Raids: a group outgunned while the other one is not falls back to it instead of
				// ending the raid (RaidRegroupCells). The members are put in order: the holders first
				// (group A from now on), then the group that falls back (group B); both share the
				// holders' goal.
				if (twoGroups && !regrouping && !bleeding)
				{
					const int bad = groupOutgunned[0] ? 0 : 1;
					const int good = 1 - bad;

					if (groupOutgunned[bad] && !groupOutgunned[good] && groupAlive[good] > 0)
					{
						const CellStruct goal = good == 0 ? plan.TargetCell : plan.StagingCell;
						std::vector<DWORD> members(plan.Members.begin() + (good == 0 ? 0 : split), plan.Members.begin() + (good == 0 ? split : plan.Members.size()));
						const size_t holders = members.size();
						members.insert(members.end(), plan.Members.begin() + (bad == 0 ? 0 : split), plan.Members.begin() + (bad == 0 ? split : plan.Members.size()));
						Debug::Log("[NeuralAI] House %d plan rotatingRaids group %c outgunned %d cells from its goal: enemy %d against %d around (%d,%d) "
							"(units %d, defenses %d); it falls back to group %c at (%d,%d) to go on to (%d,%d), frame %d.\n", houseIdx, bad == 0 ? 'A' : 'B',
							static_cast<int>(worstToGoal), worstEnemy, worstStrength, worstCentre.X, worstCentre.Y, worstUnits, worstEnemy - worstUnits,
							good == 0 ? 'A' : 'B', groupCentre[good].X, groupCentre[good].Y, goal.X, goal.Y, Unsorted::CurrentFrame);
						// The group that falls back leaves its fight: a move order keeps the target and the
						// attack-move of a unit.
						for (size_t i = holders; i < members.size(); ++i)
						{
							if (auto const pFoot = findOwnFoot(members[i]))
							{
								pFoot->SetTarget(nullptr);
								pFoot->ClearMegaMissionData();
							}
						}

						plan.Members = std::move(members);
						plan.AuxBaseline = static_cast<int>(holders);
						plan.TargetCell = goal;
						plan.StagingCell = goal;
						plan.RegroupFrame = Unsorted::CurrentFrame;
						continue;
					}
				}

				const int strength = worstStrength;
				Debug::Log("[NeuralAI] House %d plan %s %s %d cells from its goal: enemy %d against %d around (%d,%d) (units %d, defenses %d).\n", houseIdx,
					NeuralPlanNames[static_cast<size_t>(id)], outgunned ? "outgunned" : "losing members", static_cast<int>(worstToGoal), worstEnemy,
					strength, worstCentre.X, worstCentre.Y, worstUnits, worstEnemy - worstUnits);
				abortPlan(id, outgunned ? NeuralDecision::PlanAbortEnemyGrew : NeuralDecision::PlanAbortValueLost, strength);
				macro.PlanNextAttemptFrame[static_cast<size_t>(id)] = Unsorted::CurrentFrame + PlanOutgunnedRetryFrames;
			}
		}

		// ---------------------------------------------------------------- Plan 41: Base Threat Intercept
		// Visible enemy ground units with orders against our or an ally's base are met on the way by
		// an idle group sized to beat them, before they get there. A unit is a base threat when its
		// target is one of those buildings or harvesters, when its move or attack-move destination
		// lies next to one of those buildings, or when its course over the last sightings leads into
		// them. Only units we know of count. The group comes from free units near the
		// threat (never plan members or units marching in a wave), leaves a home reserve when the
		// fight is away from our own buildings, and stops at known enemy defenses or once the threat
		// is gone. TargetID = lead threat, StagingCell = the building it was after, AuxBaseline =
		// threat value at the start, AuxPeak = its distance to that building at the start (cells).
		// It runs before plans 23..38 so it gets the idle units first.
		{
			auto& plan = planOf(NeuralPlanId::BaseThreatIntercept);

			// Long-range members (artillery: 9+ cells against the ground) attack the lead threat from
			// where they are, so the engine only brings them into range; the rest attack-move.
			auto isStandoffFoot = [](FootClass* pFoot)
			{
				return pFoot && GroundRangeOf(pFoot->GetTechnoType()) >= 9 * Unsorted::LeptonsPerCell;
			};

			auto orderInterceptor = [&](FootClass* pFoot, TechnoClass* pLead, CellClass* pCell, bool reaim)
			{
				if (!pFoot || pFoot->Target != nullptr)
					return;

				if (isStandoffFoot(pFoot))
				{
					if (pLead)
					{
						pFoot->SetTarget(pLead);
						pFoot->QueueMission(Mission::Attack, true);
					}
				}
				else if (pCell && (reaim || pFoot->MegaMission != Mission::AttackMove))
				{
					OrderFootToAttackMove(pFoot, pCell);
				}
			};

			auto endIntercept = [&](bool success)
			{
				Debug::Log("[NeuralAI] House %d BaseThreatIntercept: %s after %d frames, group value %d of %d, frame %d.\n", houseIdx,
					success ? "repelled" : "failed", Unsorted::CurrentFrame - plan.StartFrame, memberValue(plan), plan.InitialValue,
					Unsorted::CurrentFrame);
				finishPlan(NeuralPlanId::BaseThreatIntercept, success, true);

				// Base threats come one after another: be ready for the next one soon.
				macro.PlanNextAttemptFrame[static_cast<size_t>(NeuralPlanId::BaseThreatIntercept)] = Unsorted::CurrentFrame + 150;

				if (success)
					CountDecision(NeuralDecision::BaseThreatRepelled, houseIdx);
			};

			if (plan.Active)
			{
				auto const pLead = findTechnoByID(plan.TargetID);
				const bool leadSeen = pLead && IsCellKnownToAI(pHouse, pLead->GetMapCoords());
				const CellStruct center = leadSeen ? pLead->GetMapCoords() : seenEnemyCentroidNear(plan.TargetCell, 10.0, 150);
				const CellStruct ourCenter = groupCentroid(plan.Members);

				if (!plan.Arrived && ourCenter != CellStruct::Empty && center != CellStruct::Empty && ourCenter.DistanceFrom(center) <= 7.0)
				{
					plan.Arrived = true;
					plan.ArrivedFrame = Unsorted::CurrentFrame;
				}

				if (memberValue(plan) * 100 < plan.InitialValue * 40 || Unsorted::CurrentFrame >= plan.Deadline)
				{
					endIntercept(false);
				}
				else if (center == CellStruct::Empty || center.DistanceFrom(plan.StagingCell) > plan.AuxPeak + 8.0
					|| CountKnownEnemyDefensesNear(pHouse, center, 8.0, false) > 0)
				{
					// Killed, or turned back out of sight or toward its own defenses: the base is safe and
					// the group does not chase it any further.
					endIntercept(true);
				}
				else if (plan.Arrived && seenEnemyValueNear(center, 8.0, 90) * 10 <= plan.AuxBaseline * 3)
				{
					endIntercept(true);
				}
				else if (auto const pCenterCell = MapClass::Instance.TryGetCellAt(center))
				{
					// Follow the threat: aim the attack-move again once it moved away from the last aim point.
					const bool moved = center.DistanceFrom(plan.TargetCell) > 4.0;

					if (moved)
						plan.TargetCell = center;

					for (const DWORD uid : plan.Members)
						orderInterceptor(findOwnFoot(uid), leadSeen ? pLead : nullptr, pCenterCell, moved);
				}
			}
			else
			{
				auto& tracks = macro.GroundThreatTracks;
				std::erase_if(tracks, [](const NeuralAirThreatTrack& track) { return Unsorted::CurrentFrame - track.LastSeenFrame > 300; });

				// Our and our allies' buildings worth defending (array order), with whether they are ours.
				std::vector<std::pair<CellStruct, bool>> sites;

				for (auto const pBld : BuildingClass::Array)
				{
					if (!pBld || pBld->InLimbo || !pBld->IsAlive || !pBld->Type || !pBld->Owner || pBld->Type->Insignificant
						|| pBld->Type->InvisibleInGame || pBld->Owner->IsNeutral() || pBld->Owner->IsObserver() || pBld->Owner->Defeated
						|| (pBld->Owner != pHouse && !pHouse->IsAlliedWith(pBld->Owner)))
					{
						continue;
					}

					sites.emplace_back(pBld->GetMapCoords(), pBld->Owner == pHouse);
				}

				// Index of the protected building nearest to a cell within maxDist (-1 when none).
				auto nearestSite = [&](const CellStruct& cell, double maxDist) -> int
				{
					int best = -1;
					double bestDist = maxDist + 0.001;

					for (size_t i = 0; i < sites.size(); ++i)
					{
						const double d = sites[i].first.DistanceFrom(cell);

						if (d < bestDist)
						{
							bestDist = d;
							best = static_cast<int>(i);
						}
					}

					return best;
				};

				// Owner of our or an ally's building or harvester (walls and other insignificant
				// structures do not count), nullptr for anything else.
				auto protectedOwner = [&](AbstractClass* pTarget) -> HouseClass*
				{
					auto const pTechno = abstract_cast<TechnoClass*>(pTarget);

					if (!pTechno || !pTechno->IsAlive || !pTechno->Owner || pTechno->Owner->IsNeutral()
						|| (pTechno->Owner != pHouse && !pHouse->IsAlliedWith(pTechno->Owner)))
					{
						return nullptr;
					}

					if (auto const pBld = abstract_cast<BuildingClass*>(pTechno))
						return pBld->Type && !pBld->Type->Insignificant ? pBld->Owner : nullptr;

					auto const pUnit = abstract_cast<UnitClass*>(pTechno);

					return pUnit && pUnit->Type && (pUnit->Type->Harvester || pUnit->Type->ResourceGatherer) ? pUnit->Owner : nullptr;
				};

				struct BaseThreat
				{
					TechnoClass* Techno;
					CellStruct Cell;
					CellStruct Goal;
					int Value;
					double GoalDistance;
					bool OwnGoal;
					const char* Reason;
					// What the threats are sorted by: the distance to the goal, less for the ones met first.
					double Rank;
				};

				std::vector<BaseThreat> threats;
				const bool tankMass = HouseExt::Fetch(pHouse)->NeuralAI_Archetype == TankMassArchetype;

				// Visible enemy ground units with a target, destination or course into a protected building.
				for (auto const pTech : TechnoClass::Array)
				{
					if (sites.empty())
						break;

					auto const pFoot = abstract_cast<FootClass*>(pTech);
					auto const pType = pFoot ? pFoot->GetTechnoType() : nullptr;

					if (!pType || !pFoot->IsAlive || pFoot->InLimbo || !pFoot->Owner || pFoot->Owner == pHouse
						|| pHouse->IsAlliedWith(pFoot->Owner) || pFoot->Owner->IsNeutral() || pType->Naval
						|| IsAirborneThreatTechno(pFoot) || pFoot->CloakState == CloakState::Cloaked || pFoot->Disguised)
					{
						continue;
					}

					const CellStruct cell = pFoot->GetMapCoords();

					if (!IsCellKnownToAI(pHouse, cell) || ObjectZone(pFoot, MovementZone::Normal) != homeZone)
						continue;

					// Unarmed engineers and spies walking into our buildings are threats too.
					auto const pInf = abstract_cast<InfantryClass*>(pFoot);
					const bool saboteur = pInf && (pInf->Type->Engineer || IsSpyInfantryType(pInf->Type));
					const bool armed = GroundRangeOf(pType) > 0;
					const int here = (armed || saboteur) ? nearestSite(cell, 45.0) : -1;

					if (here < 0)
						continue;

					const int value = std::max(GetCombatValueOfType(pType, false), saboteur ? 40 : 0);
					CellStruct goal = CellStruct::Empty;
					bool ownGoal = false;
					const char* reason = "target";

					for (auto const pOrder : { pFoot->Target, pFoot->MegaTarget, pFoot->MegaDestination, pFoot->Destination })
					{
						if (!pOrder)
							continue;

						if (auto const pOwner = protectedOwner(pOrder))
						{
							goal = CellClass::Coord2Cell(pOrder->GetCoords());
							ownGoal = pOwner == pHouse;
							break;
						}

						// A cell next to one of those buildings: the unit is on its way there. Other units
						// are not places (chasing one of ours is the tactical micro's business).
						if (!abstract_cast<TechnoClass*>(pOrder))
						{
							const int site = nearestSite(CellClass::Coord2Cell(pOrder->GetCoords()), 6.0);

							if (site >= 0)
							{
								goal = sites[site].first;
								ownGoal = sites[site].second;
								reason = "destination";
								break;
							}
						}
					}

					// Course memory: two sightings at least 45 frames apart give the heading.
					auto itTrack = std::find_if(tracks.begin(), tracks.end(),
						[&](const NeuralAirThreatTrack& track) { return track.UniqueID == pFoot->UniqueID; });

					if (itTrack == tracks.end())
					{
						if (tracks.size() >= 64)
						{
							tracks.erase(std::min_element(tracks.begin(), tracks.end(),
								[](const NeuralAirThreatTrack& a, const NeuralAirThreatTrack& b) { return a.LastSeenFrame < b.LastSeenFrame; }));
						}

						NeuralAirThreatTrack track {};
						track.UniqueID = pFoot->UniqueID;
						track.LastCoords = cell;
						track.LastSeenFrame = Unsorted::CurrentFrame;
						track.ThreatValue = value;
						tracks.push_back(track);
						itTrack = tracks.end() - 1;
					}
					else if (Unsorted::CurrentFrame - itTrack->LastSeenFrame >= 45)
					{
						itTrack->PrevCoords = itTrack->LastCoords;
						itTrack->PrevSeenFrame = itTrack->LastSeenFrame;
						itTrack->LastCoords = cell;
						itTrack->LastSeenFrame = Unsorted::CurrentFrame;
					}

					// No order against us but closing in: where it will be in 30 seconds on this course.
					if (goal == CellStruct::Empty && armed && itTrack->PrevSeenFrame >= 0 && sites[here].first.DistanceFrom(cell) > 6.0)
					{
						const int site = nearestSite(PredictAirThreatPosition(*itTrack, 450), 5.0);

						if (site >= 0
							&& itTrack->PrevCoords.DistanceFrom(sites[site].first) >= itTrack->LastCoords.DistanceFrom(sites[site].first) + 1.5)
						{
							goal = sites[site].first;
							ownGoal = sites[site].second;
							reason = "course";
						}
					}

					// The tank mass keeps its area of influence clear: an armed enemy unit standing within
					// TankMassInfluenceCells of a protected building is met whatever its orders (a gun
					// that outranges our towers, a unit camping on our ore).
					if (goal == CellStruct::Empty && armed && tankMass && sites[here].first.DistanceFrom(cell) <= TankMassInfluenceCells)
					{
						goal = sites[here].first;
						ownGoal = sites[here].second;
						reason = "area";
					}

					// Far from the building it is after, it is not a base threat yet (it may never come).
					// The mass goes for artillery first: a gun ranks as half as far from its goal as it is.
					if (goal != CellStruct::Empty && cell.DistanceFrom(goal) <= 35.0)
					{
						const double goalDistance = cell.DistanceFrom(goal);
						const bool gunFirst = tankMass && GroundRangeOf(pType) >= ArtilleryRangeCells * Unsorted::LeptonsPerCell;
						threats.push_back({ pFoot, cell, goal, value, goalDistance, ownGoal, reason, gunFirst ? goalDistance / 2.0 : goalDistance });
					}
				}

				if (!threats.empty())
				{
					CountDecision(NeuralDecision::BaseThreatTick, houseIdx);

					// The most urgent first: closest to the building it is after, ties by UniqueID.
					std::sort(threats.begin(), threats.end(), [](const BaseThreat& a, const BaseThreat& b)
						{
							return a.Rank != b.Rank ? a.Rank < b.Rank : a.Techno->UniqueID < b.Techno->UniqueID;
						});

					// The tank guard (UpdateTankGuard) meets the base threats too: it is held from the other
					// plans and the waves only.
					std::vector<FootClass*> idle = collectIdleGround(false);

					for (auto const& [uid, pFoot] : ownFoots)
					{
						if (macro.TankGuard.contains(uid) && !macro.PlanMembers.contains(uid) && !IsOnRepairTrip(pHouse, uid)
							&& ObjectZone(pFoot, MovementZone::Normal) == homeZone)
						{
							idle.push_back(pFoot);
						}
					}

					std::sort(idle.begin(), idle.end(), [](FootClass* a, FootClass* b) { return a->UniqueID < b->UniqueID; });
					const size_t leads = std::min<size_t>(threats.size(), 4);

					for (size_t li = 0; li < leads; ++li)
					{
						auto const& lead = threats[li];
						int threatValue = 0;

						for (auto const& other : threats)
						{
							if (other.Cell.DistanceFrom(lead.Cell) <= 8.0)
								threatValue += other.Value;
						}

						threatValue = std::max(threatValue, seenEnemyValueNear(lead.Cell, 8.0, 60));

						// Our armed units already next to it fight it anyway: skip it when they suffice.
						int engaged = 0;

						for (auto const& [uid, pOwn] : ownFoots)
						{
							if (isArmedGroundFoot(pOwn) && pOwn->GetMapCoords().DistanceFrom(lead.Cell) <= 6.0)
								engaged += GetCombatValueOfType(pOwn->GetTechnoType(), false);
						}

						if (engaged * 10 >= threatValue * 13)
							continue;

						// Meet it a few cells ahead of where it is, on its way to the building.
						const CellStruct aim = lead.GoalDistance > 4.0 ? pointAlong(lead.Cell, lead.Goal, 3.0, 0.0) : lead.Cell;
						auto const pAimCell = landCellInZone(aim, homeZone, 3);

						if (!pAimCell)
							continue;

						const CellStruct aimCell = pAimCell->MapCoords;

						// Our base defenses covering the meeting point take part in the fight.
						int towers = 0;

						for (auto const pBld : pHouse->Buildings)
						{
							if (pBld && pBld->IsAlive && !pBld->InLimbo && pBld->Type && pBld->Type->IsBaseDefense
								&& GroundRangeOf(pBld->Type) > 0 && pBld->GetMapCoords().DistanceFrom(aimCell) <= 7.0)
							{
								towers += GetCombatValueOfType(pBld->Type, false);
							}
						}

						// At least its own value even under our towers: a lone unit sent against a stronger
						// threat only dies on the way.
						const int need = std::max(threatValue * 13 / 10 - engaged - towers / 2, threatValue);
						std::vector<FootClass*> pool;

						for (auto const pFoot : idle)
						{
							if (pFoot->MegaMission != Mission::AttackMove && pFoot->GetMapCoords().DistanceFrom(aimCell) <= 40.0)
								pool.push_back(pFoot);
						}

						// Nearest first, ties by UniqueID.
						std::sort(pool.begin(), pool.end(), [&](FootClass* a, FootClass* b)
							{
								const CellStruct ca = a->GetMapCoords();
								const CellStruct cb = b->GetMapCoords();
								const int da = (ca.X - aimCell.X) * (ca.X - aimCell.X) + (ca.Y - aimCell.Y) * (ca.Y - aimCell.Y);
								const int db = (cb.X - aimCell.X) * (cb.X - aimCell.X) + (cb.Y - aimCell.Y) * (cb.Y - aimCell.Y);
								return da != db ? da < db : a->UniqueID < b->UniqueID;
							});

						// Unless the fight is at our own buildings, a third of the free value at home stays there.
						const bool atOwnBuildings = lead.OwnGoal && aimCell.DistanceFrom(lead.Goal) <= 12.0;
						int homeValue = 0;

						for (auto const pFoot : pool)
						{
							if (pFoot->GetMapCoords().DistanceFrom(homeCell) <= 18.0)
								homeValue += GetCombatValueOfType(pFoot->GetTechnoType(), false);
						}

						const int maxFromHome = atOwnBuildings ? INT_MAX : homeValue * 2 / 3;
						std::vector<FootClass*> group;
						int interceptValue = 0;
						int fromHome = 0;

						for (auto const pFoot : pool)
						{
							if (interceptValue >= need || group.size() >= 14)
								break;

							const int value = GetCombatValueOfType(pFoot->GetTechnoType(), false);
							const bool atHome = pFoot->GetMapCoords().DistanceFrom(homeCell) <= 18.0;

							if (atHome && fromHome + value > maxFromHome)
								continue;

							group.push_back(pFoot);
							interceptValue += value;

							if (atHome)
								fromHome += value;
						}

						if (interceptValue < need)
							continue;

						const int utility = 380 + std::min(200, threatValue / 2) + (lead.OwnGoal ? 80 : 0);

						if (shouldTryStart(NeuralPlanId::BaseThreatIntercept, utility))
						{
							startPlan(NeuralPlanId::BaseThreatIntercept, group, aimCell, lead.Goal, lead.Techno->UniqueID, 1800);
							planOf(NeuralPlanId::BaseThreatIntercept).AuxBaseline = threatValue;
							planOf(NeuralPlanId::BaseThreatIntercept).AuxPeak = static_cast<int>(lead.GoalDistance);
							for (auto const pFoot : group)
								orderInterceptor(pFoot, lead.Techno, pAimCell, false);

							CountDecision(NeuralDecision::BaseThreatIntercept, houseIdx);
							Debug::Log("[NeuralAI] House %d BaseThreatIntercept: %s of house %d (%s) %d cells from %s building at (%d,%d), "
								"threat %d, %d units value %d meet it at (%d,%d), frame %d.\n", houseIdx, lead.Techno->GetTechnoType()->ID,
								lead.Techno->Owner->ArrayIndex, lead.Reason, static_cast<int>(lead.GoalDistance), lead.OwnGoal ? "our" : "an ally",
								lead.Goal.X, lead.Goal.Y, threatValue, static_cast<int>(group.size()), interceptValue, aimCell.X, aimCell.Y,
								Unsorted::CurrentFrame);
						}

						break;
					}
				}
			}
		}

		// =================================================================================
		// Plans 23..38 (slots 111..126; 127 stays free) and plan-side helpers of points 25, 35 and 56.
		// =================================================================================
		auto isAirStriker = [&](FootClass* pFoot) -> bool
		{
			auto const pAir = abstract_cast<AircraftClass*>(pFoot);

			return isFreeAirStriker(pAir);
		};

		auto collectAirStrikers = [&]() -> std::vector<FootClass*>
		{
			std::vector<FootClass*> strikers;

			for (auto const& [uid, pFoot] : ownFoots)
			{
				if (isAirStriker(pFoot))
					strikers.push_back(pFoot);
			}

			std::sort(strikers.begin(), strikers.end(), [](FootClass* a, FootClass* b) { return a->UniqueID < b->UniqueID; });

			return strikers;
		};

		// Aircraft re-strike their target after re-arming; ground members attack it when close.
		auto strikeWithMembers = [&](const std::vector<DWORD>& uids, TechnoClass* pTarget)
		{
			if (!pTarget)
				return;

			for (const DWORD uid : uids)
			{
				auto const pFoot = findOwnFoot(uid);

				if (!pFoot)
					continue;

				if (auto const pAir = abstract_cast<AircraftClass*>(pFoot))
				{
					if (ReadyPlanAircraft(pAir))
						OrderAircraftAttack(pAir, pTarget);
				}
				else if (siegeScreenHolds(pFoot, pTarget->GetMapCoords()))
				{
					continue;
				}
				else if (pFoot->GetMapCoords().DistanceFrom(pTarget->GetMapCoords()) <= 9.0)
				{
					if (pFoot->Target == nullptr)
					{
						pFoot->SetTarget(pTarget);
						pFoot->QueueMission(Mission::Attack, true);
					}
				}
				else if (pFoot->Target == nullptr && pFoot->MegaMission != Mission::AttackMove)
				{
					OrderFootToAttackMove(pFoot, pTarget->GetCell());
				}
			}
		};

		auto knownAARiskAt = [&](const CellStruct& cell) -> int
		{
			return CountKnownEnemyDefensesNear(pHouse, cell, 10.0, true) + CountVisibleEnemyMobileAANear(pHouse, cell, 10.0);
		};

		// Strike group for a building target: >= 2 idle aircraft when its AA risk allows, else
		// minGround..maxGround idle ground units when it is on our landmass (with hurtersOnly, only
		// the ones whose weapons do damage to that building, HurtsBuilding).
		auto pickStrikeGroup = [&](BuildingClass* pTarget, size_t minGround, size_t maxGround, bool hurtersOnly = false) -> std::vector<FootClass*>
		{
			std::vector<FootClass*> group;

			if (!pTarget)
				return group;

			if (knownAARiskAt(pTarget->GetMapCoords()) <= 2)
			{
				group = collectAirStrikers();

				// Some bombs do nothing against structures (Chaos gas, mind control): only aircraft that
				// damage this building fly the strike.
				std::erase_if(group, [pTarget](FootClass* pFoot)
				{
					auto const pAir = abstract_cast<AircraftClass*>(pFoot);

					return !pAir || AirSortieDamageVs(pAir, pTarget) <= 0;
				});

				group.resize(std::min<size_t>(group.size(), 6));

				if (group.size() >= 2 && targetLessonAllows(pTarget->UniqueID, groupValueOf(group)) && airGroupCanDestroy(group, pTarget))
					return group;
			}

			group.clear();

			if (ObjectZone(pTarget, MovementZone::Normal) == homeZone)
			{
				group = collectIdleGround(false);

				if (hurtersOnly)
					std::erase_if(group, [pTarget](FootClass* pFoot) { return !HurtsBuilding(pFoot->GetTechnoType(), pTarget); });

				sortFastest(group);
				group.resize(std::min(group.size(), std::max(maxGround, group.size() / PlanStrikeIdleShare)));

				// A ground strike needs the strength to beat what is known to guard the target: a
				// group that cannot is not sent.
				if (group.size() < minGround || !targetLessonAllows(pTarget->UniqueID, groupValueOf(group)) || groupValueOf(group) * 100
					< knownEnemyStrengthOnWay(homeCell, pTarget->GetMapCoords(), PlanStrikeGuardFrames) * PlanStrikeAdvantagePercent)
				{
					group.clear();
				}
			}

			return group;
		};

		auto isElite = [](FootClass* pFoot) { return pFoot && pFoot->Veterancy.IsElite(); };

		auto maxKnownDefenseRangeNear = [&](const CellStruct& cell, double radius) -> int
		{
			int range = 0;

			for (auto const pBld : knownEnemyBuildingList)
			{
				if (isKnownEnemyBuilding(pBld) && pBld->Type->IsBaseDefense && pBld->GetMapCoords().DistanceFrom(cell) <= radius)
					range = std::max(range, GroundRangeOf(pBld->Type));
			}

			return range;
		};

		// Is this cell inside the reach (+2 cells) of a known enemy defense other than pIgnore?
		auto inKnownDefenseReach = [&](const CellStruct& cell, BuildingClass* pIgnore) -> bool
		{
			for (auto const pBld : knownEnemyBuildingList)
			{
				if (pBld == pIgnore || !isKnownEnemyBuilding(pBld) || !pBld->Type->IsBaseDefense)
					continue;

				const int range = GroundRangeOf(pBld->Type);

				if (range > 0 && pBld->GetMapCoords().DistanceFrom(cell) * Unsorted::LeptonsPerCell <= range + 2 * Unsorted::LeptonsPerCell)
					return true;
			}

			return false;
		};

		// A plan's ground column keeps together on its way to `goal`: a member more than
		// PlanMarchBandCells nearer the goal than the column's rear stops and waits (it still fights
		// what comes at it), until the rear is within PlanMarchCommittedCells of the goal. The rear is
		// the member a quarter of the way from the back, so one late or stuck unit holds nobody.
		// Fast escorts used to run ahead alone: in the focus batch of 2026-11-15 Rolling Siege lost
		// its escorts 8 to 17 cells from the defense while the battery was 40 to 70 cells behind, and
		// the watchdog turned 65 of 88 plans back. A member that would wait inside the reach of a
		// known defense falls back to the rear instead: standing there it is shot without answering
		// (match #9520: three tanks of one Superweapon Race lost 20 to 22 cells from the target, in the
		// reach of two known defenses).
		// Returns the members that take the plan's own orders this tick (aircraft included).
		auto columnMovingOn = [&](const std::vector<DWORD>& uids, const CellStruct& goal) -> std::vector<DWORD>
		{
			std::vector<std::pair<double, FootClass*>> column;

			for (const DWORD uid : uids)
			{
				auto const pFoot = findOwnFoot(uid);

				if (pFoot && pFoot->WhatAmI() != AbstractType::Aircraft && !pFoot->GetTechnoType()->Naval && !pFoot->IsInAir())
					column.emplace_back(pFoot->GetMapCoords().DistanceFrom(goal), pFoot);
			}

			if (column.size() < 2)
				return uids;

			// Nearest the goal first (ties by id).
			std::sort(column.begin(), column.end(), [](auto const& a, auto const& b)
				{
					return a.first < b.first || (a.first == b.first && a.second->UniqueID < b.second->UniqueID);
				});

			auto const& [rear, pRear] = column[column.size() - 1 - column.size() / 4];

			if (rear <= PlanMarchCommittedCells)
				return uids;

			auto const pRearCell = MapClass::Instance.TryGetCellAt(pRear->GetMapCoords());
			std::vector<DWORD> held;

			for (auto const& [d, pFoot] : column)
			{
				if (d >= rear - PlanMarchBandCells)
					break;

				held.push_back(pFoot->UniqueID);

				if (pFoot->Target != nullptr)
					continue;

				if (pRearCell && inKnownDefenseReach(pFoot->GetMapCoords(), nullptr))
				{
					if (pFoot->Destination != pRearCell)
					{
						pFoot->ClearMegaMissionData();
						pFoot->SetDestination(pRearCell, true);
						pFoot->QueueMission(Mission::Move, false);
						NoteOrderCaller(pFoot, "move");
					}
				}
				else if (pFoot->Destination || pFoot->MegaDestination)
				{
					pFoot->SetDestination(nullptr, true);
					pFoot->ClearMegaMissionData();
					pFoot->QueueMission(Mission::Guard, false);
				}
			}

			std::vector<DWORD> movingOn;

			for (const DWORD uid : uids)
			{
				if (std::find(held.begin(), held.end(), uid) == held.end())
					movingOn.push_back(uid);
			}

			return movingOn;
		};

		auto boostPlan = [&](NeuralPlanId id, int boost)
		{
			const size_t i = static_cast<size_t>(id);
			macro.PlanBoost[i] = std::max(macro.PlanBoost[i], boost);
			macro.PlanBoostUntilFrame[i] = Unsorted::CurrentFrame + 900;

			if (!macro.Plans[i].Active)
				macro.PlanNextAttemptFrame[i] = std::min(macro.PlanNextAttemptFrame[i], Unsorted::CurrentFrame);
		};

		// ---------------------------------------------------------------- Point 25: staggered retreat
		// Units sent home by a finished plan leave in two halves (finishPlan): the second half
		// holds for 120 frames covering the first, then follows.
		{
			std::vector<NeuralDelayedRetreat> still;

			for (auto const& entry : macro.DelayedRetreats)
			{
				if (Unsorted::CurrentFrame < entry.Frame && entry.Frame - Unsorted::CurrentFrame <= 300)
				{
					still.push_back(entry);
					continue;
				}

				auto const pFoot = findOwnFoot(entry.ID);

				if (pFoot && !macro.PlanMembers.contains(entry.ID))
				{
					if (auto const pHome = MapClass::Instance.TryGetCellAt(homeCell))
						IssueMoveOrder(pFoot, pHome, 4.0);
				}
			}

			macro.DelayedRetreats = std::move(still);
		}

		// ---------------------------------------------------------------- Plan 4: Blackout
		// The enemy relies on powered defenses: take out a known power plant and open a window in
		// which our main wave launches earlier, while those defenses are offline. The group is the
		// one the other strikes at a building take (pickStrikeGroup): aircraft whose load hurts the
		// plant, when its AA risk allows and their sorties can bring it down; else, for a plant no
		// known defense covers, the idle ground units that hurt it, BlackoutMinGround at least and
		// one in PlanStrikeIdleShare of the idle army when that is more than BlackoutMaxGround, with
		// the strength to beat what is known on their way in. They march as one column that fights
		// its way there and attacks the plant from 9 cells (columnMovingOn, strikeWithMembers). It
		// took the five fastest idle units on their own straight attack order, and any two aircraft:
		// 1 of 14 plans ended well in 40 training matches of 2026-11-15, 8 of the 13 others lost their
		// strength 60 to 170 cells from the plant with no enemy seen around them. The block runs
		// after the strike helpers it uses.
		{
			auto& plan = planOf(NeuralPlanId::Blackout);

			if (plan.Active)
			{
				auto const pPlant = FindBuildingByID(plan.TargetID);

				if (!pPlant)
				{
					macro.BlackoutUntilFrame = Unsorted::CurrentFrame + 1200;
					finishPlan(NeuralPlanId::Blackout, true, true);
				}
				else if (memberValue(plan) * 100 < plan.InitialValue * 40 || Unsorted::CurrentFrame >= plan.Deadline)
				{
					finishPlan(NeuralPlanId::Blackout, false, true);
				}
				else
				{
					strikeWithMembers(columnMovingOn(plan.Members, plan.TargetCell), pPlant);
				}
			}
			else
			{
				int poweredDefenses = 0;

				for (auto const pBld : knownEnemyBuildingList)
				{
					if (isKnownEnemyBuilding(pBld) && pBld->Type->IsBaseDefense && pBld->Type->Powered)
						poweredDefenses++;
				}

				BuildingClass* pBestPlant = nullptr;
				int bestPlantScore = INT_MIN;

				if (poweredDefenses >= 2)
				{
					for (auto const pBld : knownEnemyBuildingList)
					{
						if (!isKnownEnemyBuilding(pBld) || pBld->Type->PowerBonus <= 0 || pBld->Type->SuperWeapon != -1)
							continue;

						const CellStruct pc = pBld->GetMapCoords();
						const int plantScore = pBld->Type->PowerBonus - CountKnownEnemyDefensesNear(pHouse, pc, 10.0, true) * 150
							- CountKnownEnemyDefensesNear(pHouse, pc, 9.0, false) * 80;

						if (plantScore > bestPlantScore)
						{
							bestPlantScore = plantScore;
							pBestPlant = pBld;
						}
					}
				}

				const int utility = pBestPlant ? (220 + std::clamp(poweredDefenses * 60, 0, 400)) : -1000;

				if (pBestPlant && shouldTryStart(NeuralPlanId::Blackout, utility))
				{
					const CellStruct plantCell = pBestPlant->GetMapCoords();
					std::vector<FootClass*> group = pickStrikeGroup(pBestPlant, BlackoutMinGround, BlackoutMaxGround, true);

					// A ground group goes only at a plant no known defense covers.
					if (!group.empty() && group.front()->WhatAmI() != AbstractType::Aircraft
						&& CountKnownEnemyDefensesNear(pHouse, plantCell, 9.0, false) > 0)
					{
						group.clear();
					}

					if (!group.empty())
					{
						startPlan(NeuralPlanId::Blackout, group, plantCell, CellStruct::Empty, pBestPlant->UniqueID, 3600);
						strikeWithMembers(planOf(NeuralPlanId::Blackout).Members, pBestPlant);
					}
				}
			}
		}

		// ---------------------------------------------------------------- Plan 37: Combo Blackout Push
		// Chain (point 56): while a successful Blackout keeps the enemy defenses offline, the
		// multi-front attacks and the airborne front are started at once (cooldowns skipped,
		// utility boosted). Success when one of them started.
		{
			auto& plan = planOf(NeuralPlanId::ComboBlackoutPush);
			const bool blackoutOpen = macro.BlackoutUntilFrame > Unsorted::CurrentFrame;
			const bool anyFollowUp = planOf(NeuralPlanId::Pincer).Active || planOf(NeuralPlanId::ThreeFrontAssault).Active
				|| planOf(NeuralPlanId::AirborneFront).Active || planOf(NeuralPlanId::FactoryDecapitation).Active;

			if (plan.Active)
			{
				if (anyFollowUp)
					finishPlan(NeuralPlanId::ComboBlackoutPush, true, false);
				else if (!blackoutOpen || Unsorted::CurrentFrame >= plan.Deadline)
					finishPlan(NeuralPlanId::ComboBlackoutPush, false, false);
				else
				{
					boostPlan(NeuralPlanId::ThreeFrontAssault, 400);
					boostPlan(NeuralPlanId::Pincer, 400);
					boostPlan(NeuralPlanId::AirborneFront, 400);
					boostPlan(NeuralPlanId::FactoryDecapitation, 300);
				}
			}
			else if (planReady(NeuralPlanId::ComboBlackoutPush) && blackoutOpen && !anyFollowUp
				&& shouldTryStart(NeuralPlanId::ComboBlackoutPush, 350))
			{
				startPlan(NeuralPlanId::ComboBlackoutPush, std::vector<FootClass*> {}, homeCell, CellStruct::Empty, 0, 1200);
				boostPlan(NeuralPlanId::ThreeFrontAssault, 400);
				boostPlan(NeuralPlanId::Pincer, 400);
				boostPlan(NeuralPlanId::AirborneFront, 400);
				boostPlan(NeuralPlanId::FactoryDecapitation, 300);
			}
		}

		// ---------------------------------------------------------------- Watch and strike cases
		// What Preemptive Strike (Plan 19) and Hold Under Towers (Plan 12) would act on this tick, so
		// Combo Watch Strike (Plan 38) starts only for a force one of them answers. Ground sightings
		// only: both answer forces on foot or on wheels, and aircraft and ships neither come under the
		// towers nor can be chased by a ground group.
		auto isGroundSighting = [](const EnemyUnitSighting& sighting)
		{
			return sighting.Category != EnemyForceCategory::Air && sighting.Category != EnemyForceCategory::Naval;
		};

		auto seenGroundEnemyValueNear = [&](const CellStruct& cell, double radius, int maxAge) -> int
		{
			int value = 0;

			for (auto const& [uid, sighting] : macro.EnemySightings)
			{
				if (isGroundSighting(sighting) && Unsorted::CurrentFrame - sighting.LastSeenFrame <= maxAge
					&& sighting.LastCoords.DistanceFrom(cell) <= radius)
				{
					value += sighting.Value;
				}
			}

			return value;
		};

		auto seenGroundEnemyCentroidNear = [&](const CellStruct& cell, double radius, int maxAge) -> CellStruct
		{
			long long sumX = 0;
			long long sumY = 0;
			long long weight = 0;

			for (auto const& [uid, sighting] : macro.EnemySightings)
			{
				if (isGroundSighting(sighting) && Unsorted::CurrentFrame - sighting.LastSeenFrame <= maxAge
					&& sighting.LastCoords.DistanceFrom(cell) <= radius)
				{
					const int w = std::max(1, sighting.Value);
					sumX += static_cast<long long>(sighting.LastCoords.X) * w;
					sumY += static_cast<long long>(sighting.LastCoords.Y) * w;
					weight += w;
				}
			}

			return weight > 0 ? CellStruct { static_cast<short>(sumX / weight), static_cast<short>(sumY / weight) } : CellStruct::Empty;
		};

		// Value of the ground units seen near a cell that have stood where they are for
		// PreemptiveGatherFrames.
		auto standingGroundEnemyValueNear = [&](const CellStruct& cell, double radius) -> int
		{
			int value = 0;

			for (auto const& [uid, sighting] : macro.EnemySightings)
			{
				if (isGroundSighting(sighting) && Unsorted::CurrentFrame - sighting.LastSeenFrame <= 150
					&& Unsorted::CurrentFrame - sighting.StandSinceFrame >= PreemptiveGatherFrames
					&& sighting.LastCoords.DistanceFrom(cell) <= radius)
				{
					value += sighting.Value;
				}
			}

			return value;
		};

		struct PreemptiveCase
		{
			CellStruct Center { CellStruct::Empty };
			int Value { 0 };
			std::vector<FootClass*> Group {};
		};

		// A ground force gathering outside its defenses, on our landmass and not at our base, and the
		// free units nearest to it that are worth more and close enough to strike it before it moves.
		auto findPreemptiveCase = [&]() -> PreemptiveCase
		{
			PreemptiveCase result;
			CellStruct bestSeed = CellStruct::Empty;
			int bestStanding = 0;

			for (auto const& [uid, seed] : macro.EnemySightings)
			{
				if (!isGroundSighting(seed) || Unsorted::CurrentFrame - seed.LastSeenFrame > 150 || seed.NearOurBase || seed.Value < 20
					|| Unsorted::CurrentFrame - seed.StandSinceFrame < PreemptiveGatherFrames)
				{
					continue;
				}

				const int standing = standingGroundEnemyValueNear(seed.LastCoords, PreemptiveClusterCells);

				// Ties broken by position so the pick does not depend on the map's iteration order.
				if (standing > bestStanding
					|| (standing == bestStanding && standing > 0
						&& (seed.LastCoords.X < bestSeed.X || (seed.LastCoords.X == bestSeed.X && seed.LastCoords.Y < bestSeed.Y))))
				{
					bestStanding = standing;
					bestSeed = seed.LastCoords;
				}
			}

			if (bestStanding < PreemptiveMinValue)
				return result;

			const CellStruct center = seenGroundEnemyCentroidNear(bestSeed, PreemptiveClusterCells, 150);

			if (center == CellStruct::Empty || zoneOf(center) != homeZone || CountKnownEnemyDefensesNear(pHouse, center, 10.0, false) > 0
				|| center.DistanceFrom(homeCell) <= 22.0)
			{
				return result;
			}

			// The whole force the group would meet there, standing or not.
			const int value = seenGroundEnemyValueNear(center, PreemptiveClusterCells, 150);
			std::vector<std::pair<double, FootClass*>> inReach;

			for (auto const pFoot : collectIdleGround(false))
			{
				const double distance = pFoot->GetMapCoords().DistanceFrom(center);

				if (distance <= PreemptiveReachCells)
					inReach.emplace_back(distance, pFoot);
			}

			std::sort(inReach.begin(), inReach.end(), [](auto const& a, auto const& b)
				{
					return a.first != b.first ? a.first < b.first : a.second->UniqueID < b.second->UniqueID;
				});

			std::vector<FootClass*> group;

			for (auto const& [distance, pFoot] : inReach)
			{
				if (group.size() >= PreemptiveMaxMembers)
					break;

				group.push_back(pFoot);
			}

			if (group.size() < PreemptiveMinMembers || feetValue(group) * 100 < value * 125)
				return result;

			result.Center = center;
			result.Value = value;
			result.Group = std::move(group);

			return result;
		};

		struct HoldTowersCase
		{
			int EnemyNear { 0 };
			std::vector<FootClass*> Army {};
			CellClass* Rally { nullptr };
			bool RallyOnRoute { false };
		};

		// A ground force clearly stronger than our free army is near home, and that army is worth
		// keeping out of the open: it would hold just behind the hottest approach our towers cover
		// (route heat), else at the middle of our home defenses.
		auto findHoldTowersCase = [&]() -> HoldTowersCase
		{
			HoldTowersCase result;

			if (ownHomeDefenses < 2)
				return result;

			const int enemyNear = seenGroundEnemyValueNear(homeCell, HoldTowersEnemyCells, 150);

			if (enemyNear < HoldTowersMinEnemyValue)
				return result;

			std::vector<FootClass*> army;

			for (auto const& [uid, pFoot] : ownFoots)
			{
				if (isArmedGroundFoot(pFoot) && !macro.PlanMembers.contains(uid) && ObjectZone(pFoot, MovementZone::Normal) == homeZone
					&& pFoot->GetMapCoords().DistanceFrom(homeCell) <= HoldTowersArmyCells)
				{
					army.push_back(pFoot);
				}
			}

			if (army.size() < HoldTowersMinMembers || enemyNear * 100 <= feetValue(army) * HoldTowersOddsPercent)
				return result;

			std::sort(army.begin(), army.end(), [](FootClass* a, FootClass* b) { return a->UniqueID < b->UniqueID; });
			CellClass* pRally = nullptr;

			if (auto const pHoldSpot = BestRouteHoldSpot(pHouse, homeCell, 26))
				pRally = landCellInZone(pointAlong(pHoldSpot->Cell, homeCell, 4.0, 0.0), homeZone, 4);

			result.RallyOnRoute = pRally != nullptr;

			if (!pRally)
				pRally = landCellInZone(ownDefenseCentroid, homeZone, 4);

			if (!pRally)
				return HoldTowersCase {};

			// Only a fight the towers turn: the army and our defenses that shoot at the ground near
			// the rally together worth HoldTowersStandPercent of the force.
			int towers = 0;

			for (auto const pBld : pHouse->Buildings)
			{
				if (pBld && pBld->IsAlive && !pBld->InLimbo && pBld->Type && pBld->Type->IsBaseDefense
					&& pBld->GetMapCoords().DistanceFrom(pRally->MapCoords) <= HoldTowersDefenseCells)
				{
					towers += GetCombatValueOfType(pBld->Type, false);
				}
			}

			if ((feetValue(army) + towers) * 100 < enemyNear * HoldTowersStandPercent)
				return HoldTowersCase {};

			result.EnemyNear = enemyNear;
			result.Army = std::move(army);
			result.Rally = pRally;

			return result;
		};

		// ---------------------------------------------------------------- Plan 38: Combo Watch Strike
		// Chain (point 56): a sentry of ours spots an enemy force that Preemptive Strike or Hold Under
		// Towers would answer right now, and that follow-up is pushed once (cooldown skipped, utility
		// raised for the 900 frames of the boost). Pushed again every tick, a strike the raid gate
		// held was weighed again almost every tick of the window (frames 28830, 28875, 28950 of a
		// focus match) instead of after PlanGateRetryFrames.
		// The chain is judged by the plan it led into: well when that plan ends well, badly when it
		// ends badly or does not start within ComboWatchStartFrames. AuxBaseline: the follow-ups
		// pushed (1 strike, 2 hold); Progress: the one that started (1, 2); ArrivedFrame: its start
		// frame.
		{
			auto& plan = planOf(NeuralPlanId::ComboWatchStrike);
			auto& pickets = planOf(NeuralPlanId::Pickets);
			constexpr NeuralPlanId followUps[] = { NeuralPlanId::PreemptiveStrike, NeuralPlanId::HoldUnderTowers };

			auto pushFollowUps = [&](int which)
			{
				if (which & 1)
					boostPlan(NeuralPlanId::PreemptiveStrike, 350);

				if (which & 2)
					boostPlan(NeuralPlanId::HoldUnderTowers, 250);
			};

			// A live sentry has a ground enemy of the force (within `radius` of `center`) in sight.
			auto sentrySees = [&](const CellStruct& center, double radius) -> bool
			{
				for (const DWORD uid : pickets.Members)
				{
					auto const pSentry = findOwnFoot(uid);

					if (!pSentry)
						continue;

					const CellStruct sentryCell = pSentry->GetMapCoords();

					for (auto const& [enemyID, sighting] : macro.EnemySightings)
					{
						if (isGroundSighting(sighting) && Unsorted::CurrentFrame - sighting.LastSeenFrame <= ComboWatchSightFrames
							&& sighting.LastCoords.DistanceFrom(sentryCell) <= ComboWatchSightCells
							&& sighting.LastCoords.DistanceFrom(center) <= radius)
						{
							return true;
						}
					}
				}

				return false;
			};

			if (plan.Active)
			{
				for (size_t i = 0; plan.Progress == 0 && i < std::size(followUps); ++i)
				{
					auto const& followUp = planOf(followUps[i]);

					if ((plan.AuxBaseline & (1 << i)) && followUp.Active)
					{
						plan.Progress = static_cast<int>(i) + 1;
						plan.ArrivedFrame = followUp.StartFrame;
					}
				}

				if (plan.Progress == 0)
				{
					if (Unsorted::CurrentFrame >= plan.Deadline)
						finishPlan(NeuralPlanId::ComboWatchStrike, false, false);
				}
				else
				{
					const size_t f = static_cast<size_t>(followUps[plan.Progress - 1]);
					auto const& followUp = macro.Plans[f];

					if (followUp.Active && followUp.StartFrame == plan.ArrivedFrame)
						plan.Deadline = std::max(plan.Deadline, followUp.Deadline + ComboWatchStartFrames);
					else
						finishPlan(NeuralPlanId::ComboWatchStrike, macro.PlanEndFrame[f] >= plan.ArrivedFrame && macro.PlanEndedWell[f], false);
				}
			}
			else if (planReady(NeuralPlanId::ComboWatchStrike) && pickets.Active && pickets.AuxPeak > pickets.AuxBaseline)
			{
				// Something new was seen by a sentry: is it a force a follow-up would act on?
				pickets.AuxBaseline = pickets.AuxPeak;
				int which = 0;

				if (!planOf(NeuralPlanId::PreemptiveStrike).Active)
				{
					auto const strike = findPreemptiveCase();

					if (!strike.Group.empty() && sentrySees(strike.Center, PreemptiveClusterCells))
						which |= 1;
				}

				if (!planOf(NeuralPlanId::HoldUnderTowers).Active)
				{
					auto const hold = findHoldTowersCase();

					if (!hold.Army.empty() && sentrySees(homeCell, HoldTowersEnemyCells))
						which |= 2;
				}

				if (which != 0 && shouldTryStart(NeuralPlanId::ComboWatchStrike, 320))
				{
					startPlan(NeuralPlanId::ComboWatchStrike, std::vector<FootClass*> {}, homeCell, CellStruct::Empty, 0, ComboWatchStartFrames);
					planOf(NeuralPlanId::ComboWatchStrike).AuxBaseline = which;
					pushFollowUps(which);
				}
			}
		}

		// ---------------------------------------------------------------- Plan 8: Island Hold
		// A ConYard of ours on another landmass (colonized by ferry or airlift) is fortified first:
		// the defenses it still lacks (IslandHoldNeedsGround, IslandHoldNeedsAntiAir) are the next
		// buildings the house makes and they are placed next to it (IslandHoldWants), and our idle
		// units already on that island guard it, until it has 3 ground defenses (+1 anti-air when
		// enemy air has been seen).
		{
			auto& plan = planOf(NeuralPlanId::IslandHold);
			const bool enemyAirSeen = foe.Share(EnemyForceCategory::Air) >= 15 || macro.AirThreatValue > 0;
			const int spawnZone = pHouse->BaseSpawnCell != CellStruct::Empty ? zoneOf(pHouse->BaseSpawnCell) : homeZone;

			auto countYardDefenses = [&](BuildingClass* pYard, int& ground, int& antiAir)
			{
				ground = 0;
				antiAir = 0;
				const CellStruct yc = pYard->GetMapCoords();
				const int yz = zoneOf(yc);

				for (auto const pB : pHouse->Buildings)
				{
					if (!pB || pB->InLimbo || !pB->IsAlive || !pB->Type || !pB->Type->IsBaseDefense || IsTunnelNetworkBuilding(pB->Type))
						continue;

					const CellStruct bc = pB->GetMapCoords();

					if (bc.DistanceFrom(yc) > IslandHoldDefenseRadius || zoneOf(bc) != yz)
						continue;

					if (GetCombatValueOfType(pB->Type, true) > 0)
						antiAir++;

					if (GroundRangeOf(pB->Type) > 0)
						ground++;
				}
			};

			auto clearHold = [&]()
			{
				macro.IslandHoldCell = CellStruct::Empty;
				macro.IslandHoldUntilFrame = -1;
				macro.IslandHoldNeedsGround = false;
				macro.IslandHoldNeedsAntiAir = false;
				macro.IslandHoldNoRoomUntilFrame = -1;
			};

			auto holdYard = [&](BuildingClass* pYard, int deadline, int ground, int antiAir)
			{
				macro.IslandHoldCell = pYard->GetMapCoords();
				macro.IslandHoldUntilFrame = deadline;
				macro.IslandHoldNeedsGround = ground < IslandHoldGroundDefenses;
				macro.IslandHoldNeedsAntiAir = enemyAirSeen && antiAir < 1;
			};

			if (plan.Active)
			{
				auto const pYard = FindBuildingByID(plan.TargetID);
				int ground = 0;
				int antiAir = 0;

				if (pYard)
					countYardDefenses(pYard, ground, antiAir);

				if (!pYard || pYard->Owner != pHouse)
				{
					clearHold();
					finishPlan(NeuralPlanId::IslandHold, false, false);
				}
				else if (ground >= IslandHoldGroundDefenses && (!enemyAirSeen || antiAir >= 1))
				{
					clearHold();
					finishPlan(NeuralPlanId::IslandHold, true, false);
				}
				else if (Unsorted::CurrentFrame >= plan.Deadline)
				{
					clearHold();
					finishPlan(NeuralPlanId::IslandHold, false, false);
				}
				else
				{
					const CellStruct yc = pYard->GetMapCoords();
					const int yz = zoneOf(yc);
					holdYard(pYard, plan.Deadline, ground, antiAir);

					// Idle armed units already on that island stand guard next to the yard.
					std::vector<FootClass*> islanders;

					for (auto const& [uid, pFoot] : ownFoots)
					{
						if (isArmedGroundFoot(pFoot) && isFreeForPlan(pFoot) && ObjectZone(pFoot, MovementZone::Normal) == yz
							&& pFoot->GetMapCoords().DistanceFrom(yc) > 8.0)
						{
							islanders.push_back(pFoot);
						}
					}

					std::sort(islanders.begin(), islanders.end(), [](FootClass* a, FootClass* b) { return a->UniqueID < b->UniqueID; });

					if (auto const pGuardCell = landCellInZone(CellStruct { yc.X, static_cast<short>(yc.Y + 4) }, yz, 4))
					{
						for (size_t i = 0; i < islanders.size() && i < 6; ++i)
							IssueMoveOrder(islanders[i], pGuardCell, 5.0);
					}
				}
			}
			else if (planReady(NeuralPlanId::IslandHold))
			{
				BuildingClass* pTarget = nullptr;
				int targetGround = 0;
				int targetAA = 0;

				for (int yi = 0; yi < pHouse->ConYards.Count; ++yi)
				{
					auto const pCY = pHouse->ConYards.GetItem(yi);

					if (!pCY || pCY->InLimbo || !pCY->IsAlive)
						continue;

					const int cz = ObjectZone(pCY, MovementZone::Normal);

					if (cz < 0 || cz == spawnZone)
						continue;

					int ground = 0;
					int antiAir = 0;
					countYardDefenses(pCY, ground, antiAir);

					if (ground < IslandHoldGroundDefenses || (enemyAirSeen && antiAir < 1))
					{
						pTarget = pCY;
						targetGround = ground;
						targetAA = antiAir;
						break;
					}
				}

				const int utility = pTarget
					? (350 + ((enemyAirSeen && targetAA == 0) ? 120 : 0)
						+ (IslandHoldGroundDefenses - std::min(IslandHoldGroundDefenses, targetGround)) * 40)
					: -1000;

				if (pTarget && shouldTryStart(NeuralPlanId::IslandHold, utility))
				{
					startPlan(NeuralPlanId::IslandHold, std::vector<FootClass*> {}, pTarget->GetMapCoords(), CellStruct::Empty,
						pTarget->UniqueID, 5400);
					macro.IslandHoldNoRoomUntilFrame = -1;
					holdYard(pTarget, planOf(NeuralPlanId::IslandHold).Deadline, targetGround, targetAA);
				}
			}
		}

		// Safe return flights: a transport out of its plan that is still flying home empty keeps
		// the air lane around known anti-air; a new job (cargo, another plan, an attack) or home ends it.
		{
			auto& flights = GetSafeReturnState(pHouse).Flights;

			for (size_t i = 0; i < flights.size();)
			{
				auto const pFlyer = findOwnFoot(flights[i]);
				auto const pHome = pFlyer ? FindCarrierHomeCell(pHouse, pFlyer) : nullptr;
				const bool inRaid = planOf(NeuralPlanId::AirborneFront).Active && macro.PlanMembers.contains(flights[i]);
				const bool done = !pFlyer || !pHome || pFlyer->Passengers.NumPassengers > 0 || pFlyer->Target
					|| (!inRaid && macro.PlanMembers.contains(flights[i]))
					|| pFlyer->GetMapCoords().DistanceFrom(pHome->MapCoords) <= 4.0;

				if (done)
				{
					flights.erase(flights.begin() + static_cast<std::ptrdiff_t>(i));
					continue;
				}

				// While its raid runs, the plan flies it home itself.
				if (!inRaid)
					IssueMoveOrder(pFlyer, GetSafeAirHop(pHouse, pFlyer, pHome), 4.0);

				++i;
			}
		}

		// ---------------------------------------------------------------- Plan 9: Airborne Front
		// While our ground army attacks an enemy base, a transport (passenger aircraft, or a flying
		// or burrowing transport vehicle) drops a team on a known valuable building of
		// that base, away from where the army is aimed and on the side of it where the least is
		// known to stand. Members[0] = transport, the rest = the team.
		// AuxBaseline = phase: 0 boarding, 1 on the way, 2 dropped, 3 standing down. Progress =
		// targets taken after the first one fell before the drop.
		{
			auto& plan = planOf(NeuralPlanId::AirborneFront);

			struct AirborneFrontLine
			{
				CellStruct Cell { CellStruct::Empty };
				int Owner { -1 };
			};

			struct AirborneGoal
			{
				BuildingClass* Target { nullptr };
				CellClass* Drop { nullptr };
				int Score { INT_MIN };
			};

			// Where the main attack is aimed and at whom: the target of the attack plan under way, or
			// the middle of our armed ground units engaged near the known enemy base; the owner is
			// the plan's, or that of the known building nearest to that point. No cell: no main attack.
			auto findFront = [&]() -> AirborneFrontLine
			{
				AirborneFrontLine front {};
				constexpr NeuralPlanId mainAttacks[] = { NeuralPlanId::TimingRush, NeuralPlanId::Pincer, NeuralPlanId::BaseTrade,
					NeuralPlanId::AllyJointAttack, NeuralPlanId::ThreeFrontAssault, NeuralPlanId::FactoryDecapitation };

				for (const NeuralPlanId id : mainAttacks)
				{
					auto const& attack = planOf(id);

					if (attack.Active && attack.TargetCell != CellStruct::Empty)
					{
						front.Cell = attack.TargetCell;
						front.Owner = attack.TargetOwner;
						break;
					}
				}

				if (front.Cell == CellStruct::Empty && knownEnemyHomeBuildings > 0)
				{
					int engaged = 0;
					int sumX = 0;
					int sumY = 0;

					for (auto const& [uid, pFoot] : ownFoots)
					{
						if (isArmedGroundFoot(pFoot) && (pFoot->Target != nullptr || pFoot->MegaMission == Mission::AttackMove)
							&& pFoot->GetMapCoords().DistanceFrom(knownEnemyCentroid) <= 18.0)
						{
							engaged++;
							sumX += pFoot->GetMapCoords().X;
							sumY += pFoot->GetMapCoords().Y;
						}
					}

					if (engaged >= 6)
						front.Cell = CellStruct { static_cast<short>(sumX / engaged), static_cast<short>(sumY / engaged) };
				}

				if (front.Cell != CellStruct::Empty && front.Owner < 0)
				{
					double nearest = 0.0;

					for (auto const pBld : knownEnemyBuildingList)
					{
						if (!isKnownEnemyBuilding(pBld))
							continue;

						const double dist = pBld->GetMapCoords().DistanceFrom(front.Cell);

						if (front.Owner < 0 || dist < nearest)
						{
							front.Owner = pBld->Owner->ArrayIndex;
							nearest = dist;
						}
					}
				}

				return front;
			};

			// The drop cell: one of eight around the target, AirborneDropRingCells from it, on free
			// land of its zone; the one with the fewest defenses and the least enemy value known within
			// AirborneDropGuardCells, and among equals the one farthest from the main attack. The cell
			// on the way home, taken before, was the side of the base that faces our army.
			auto pickDropCell = [&](const CellStruct& bc, const CellStruct& frontCell) -> CellClass*
			{

				constexpr int ringX[] = { 1, 1, 0, -1, -1, -1, 0, 1 };
				constexpr int ringY[] = { 0, 1, 1, 1, 0, -1, -1, -1 };
				const int zone = zoneOf(bc);
				CellClass* pBest = nullptr;
				int bestRisk = 0;
				double bestGap = 0.0;

				for (size_t i = 0; i < std::size(ringX); ++i)
				{
					const double reach = (ringX[i] != 0 && ringY[i] != 0) ? AirborneDropRingCells * 0.7 : AirborneDropRingCells;
					const CellStruct point { static_cast<short>(bc.X + ringX[i] * reach), static_cast<short>(bc.Y + ringY[i] * reach) };
					auto const pCell = landCellInZone(point, zone, 2);

					if (!pCell || pCell->GetBuilding())
						continue;

					const int risk = CountKnownEnemyDefensesNear(pHouse, pCell->MapCoords, AirborneDropGuardCells, false) * 1000
						+ seenEnemyValueNear(pCell->MapCoords, AirborneDropGuardCells, PlanStrikeGuardFrames);
					const double gap = frontCell == CellStruct::Empty ? 0.0 : pCell->MapCoords.DistanceFrom(frontCell);

					if (!pBest || risk < bestRisk || (risk == bestRisk && gap > bestGap))
					{
						pBest = pCell;
						bestRisk = risk;
						bestGap = gap;
					}
				}

				return pBest;
			};

			// The building for a drop: known, worth the trip, of the house the main attack is on (a
			// third house has its whole garrison at home), at least AirborneFrontGapCells from where
			// that attack is aimed (nearer, the army brought it down before the drop: 9 of 34 plans)
			// and AirborneFrontReachCells at most (farther, the attack keeps nobody busy there: 14 of
			// the 25 transports that left for such a target were shot down or badly hit on the way,
			// 4 of 22 nearer),
			// with one anti-air defense and one mobile anti-air unit known near it at most, with the
			// team worth PlanStrikeAdvantagePercent of the strength known around it, as for a raid,
			// and within what the team can destroy or one of its specialists can enter. workersFor tells how many of
			// our specialists have a job inside a building type; a team already on its way (from)
			// prefers the nearer building.
			auto pickGoal = [&](const AirborneFrontLine& front, const std::vector<FootClass*>& team, auto const& workersFor,
				const CellStruct& from) -> AirborneGoal
			{
				AirborneGoal goal {};
				const int teamValue = feetValue(team);

				// What the team does to a building in the time it gets on the ground: every member
				// fires its best ground weapon against the building's armor at its rate of fire
				// for a part of that time (1 in AirborneAssaultShare: it also walks and is shot
				// at). One that brings the building down by itself (C4, a bomber, a unit that
				// blows up) counts for all of it.
				auto assaultDamage = [&team](BuildingClass* pBld) -> int
				{
					int damage = 0;

					for (auto const pFoot : team)
					{
						auto const pInf = abstract_cast<InfantryClass*>(pFoot);
						auto const pUnit = abstract_cast<UnitClass*>(pFoot);

						if ((pInf && (IsCommandoInfantryType(pInf->Type) || pInf->Type->Ivan || IsSuicideInfantryType(pInf->Type)))
							|| (pUnit && IsVehicleRaiderType(pUnit->Type)))
						{
							return pBld->Health;
						}

						int best = 0;

						for (int i = 0; i < 2; ++i)
						{
							auto const pSlot = pFoot->GetTechnoType()->GetWeapon(i);
							auto const pWeapon = pSlot ? pSlot->WeaponType : nullptr;

							if (!WeaponHitsGround(pWeapon) || !pWeapon->Warhead)
								continue;

							const double verses = GeneralUtils::GetWarheadVersusArmor(pWeapon->Warhead, pBld, pBld->Type);
							best = std::max(best, static_cast<int>(pWeapon->Damage * std::max(1, pWeapon->Burst) * verses
								* AirborneAssaultFrames / std::max(1, pWeapon->ROF)));
						}

						damage += best / AirborneAssaultShare;
					}

					return damage;
				};

				for (auto const pBld : knownEnemyBuildingList)
				{
					if (!isKnownEnemyBuilding(pBld) || pBld->Owner->ArrayIndex != front.Owner)
						continue;

					const int workers = workersFor(pBld->Type);
					const int worth = valuableBuildingWorth(pBld->Type) + workers * 150;
					const CellStruct bc = pBld->GetMapCoords();
					const double gap = front.Cell == CellStruct::Empty ? AirborneFrontReachCells : bc.DistanceFrom(front.Cell);

					// A building nobody of the team goes into is one the team can bring down: 4 of 27
					// teams stood by theirs for 2700 frames, two of them without a scratch on it.
					if (workers == 0 && assaultDamage(pBld) < pBld->Health)
						continue;

					if (worth <= 0 || gap < AirborneFrontGapCells || gap > AirborneFrontReachCells
						|| CountKnownEnemyDefensesNear(pHouse, bc, 9.0, true) > 1
						|| CountVisibleEnemyMobileAANear(pHouse, bc, 9.0) > 1)
					{
						continue;
					}

					const int guard = knownEnemyStrengthOnWay(CellStruct::Empty, bc, PlanStrikeGuardFrames);

					if (guard * PlanStrikeAdvantagePercent > teamValue * 100)
						continue;

					const int score = worth + static_cast<int>(gap) * 4 - guard / 20
						- (from == CellStruct::Empty ? 0 : static_cast<int>(bc.DistanceFrom(from)) * 3);

					if (score > goal.Score)
					{
						if (auto const pCell = pickDropCell(bc, front.Cell))
							goal = AirborneGoal { pBld, pCell, score };
					}
				}

				return goal;
			};

			if (plan.Active)
			{
				FootClass* const pTransport = plan.Members.empty() ? nullptr : findOwnFoot(plan.Members[0]);
				auto const pTarget = FindBuildingByID(plan.TargetID);
				auto const pDrop = MapClass::Instance.TryGetCellAt(plan.StagingCell);
				const std::vector<DWORD> infantry(plan.Members.empty() ? plan.Members.end() : plan.Members.begin() + 1, plan.Members.end());
				auto const pTransportUnit = abstract_cast<UnitClass*>(pTransport);

				// Only a transport that flies takes the safe air lane: its waypoints lie where the
				// anti-air is not, reachable or not by land or sea, and an APC sent along them made 12
				// cells in 3000 frames (30 in the 900 after its plan ended).
				const bool flies = pTransport && (pTransport->WhatAmI() == AbstractType::Aircraft || pTransport->GetTechnoType()->JumpJet
					|| pTransport->GetTechnoType()->MovementZone == MovementZone::Fly);
				const bool flyer = pTransportUnit && (pTransportUnit->Type->JumpJet || pTransportUnit->Type->MovementZone == MovementZone::Fly);
				const bool aloft = pTransport && IsFerryAloft(pTransport);
				auto const pLanding = (flyer && plan.AnchorCell != CellStruct { 0, 0 }) ? MapClass::Instance.TryGetCellAt(plan.AnchorCell) : nullptr;

				// A free land cell of a zone for a flying transport to come down on: the nearest to
				// `center` from `firstRing` cells out (ring search, fixed order), two cells or more
				// from every soldier of the team on the map, other than the one tried last.
				auto pickLandingCell = [&](const CellStruct& center, int zone, int firstRing, const CellStruct& tried) -> CellClass*
				{
					for (int ring = firstRing; ring <= BoardingLandingRingCells; ++ring)
					{
						for (int dy = -ring; dy <= ring; ++dy)
						{
							for (int dx = -ring; dx <= ring; ++dx)
							{
								if (std::max(std::abs(dx), std::abs(dy)) != ring)
									continue;

								const CellStruct c { static_cast<short>(center.X + dx), static_cast<short>(center.Y + dy) };
								auto const pCell = MapClass::Instance.CoordinatesLegal(c) ? MapClass::Instance.TryGetCellAt(c) : nullptr;

								if (!pCell || c == tried || pCell->LandType == LandType::Water || pCell->LandType == LandType::Beach
									|| pCell->GetBuilding() || pCell->FirstObject
									|| MapClass::Instance.GetMovementZoneType(c, MovementZone::Normal, false) != zone
									|| !pCell->IsClearToMove(SpeedType::Track, false, false, -1, MovementZone::Normal, -1, false))
								{
									continue;
								}

								const bool clear = std::none_of(infantry.begin(), infantry.end(), [&](DWORD uid)
									{
										auto const pFoot = findOwnFoot(uid);

										return pFoot && pFoot->GetMapCoords().DistanceFrom(c) < 2.0;
									});

								if (clear)
									return pCell;
							}
						}
					}

					return nullptr;
				};

				constexpr const char* PhaseNames[] = { "boarding", "on the way", "dropped", "standing down" };
				auto logEnd = [&](const char* reason)
				{
					Debug::Log("[NeuralAI] House %d AirborneFront: ends while %s (%s), frame %d.\n", pHouse->ArrayIndex,
						PhaseNames[std::clamp(plan.AuxBaseline, 0, 3)], reason, Unsorted::CurrentFrame);
				};

				// Ends badly a plan whose team may still be aboard: the transport takes those inside
				// home and lets them out (phase 3) before the plan lets go of it. Left to itself it
				// kept them inside.
				auto standDown = [&](const char* reason)
				{
					logEnd(reason);

					if (pTransport && pTransport->CurrentMission == Mission::Sleep)
						pTransport->ForceMission(Mission::Guard);

					if (pTransport && pTransport->Passengers.NumPassengers > 0)
					{
						plan.AuxBaseline = 3;
						plan.Deadline = Unsorted::CurrentFrame + AirborneStandDownFrames;
					}
					else
					{
						finishPlan(NeuralPlanId::AirborneFront, false, true);
					}
				};

				if (plan.AuxBaseline == 3)
				{
					int homeDist = INT_MAX;
					auto const pHome = pTransport ? FindCarrierHomeCell(pHouse, pTransport, &homeDist) : nullptr;

					if (!pTransport || pTransport->Passengers.NumPassengers == 0 || Unsorted::CurrentFrame >= plan.Deadline)
					{
						Debug::Log("[NeuralAI] House %d AirborneFront: stood down with %d still aboard, frame %d.\n", pHouse->ArrayIndex,
							pTransport ? pTransport->Passengers.NumPassengers : -1, Unsorted::CurrentFrame);
						finishPlan(NeuralPlanId::AirborneFront, false, true);
					}
					else if (pHome && homeDist > 8 * Unsorted::LeptonsPerCell)
						IssueMoveOrder(pTransport, flies ? GetSafeAirHop(pHouse, pTransport, pHome) : pHome, 4.0);
					else if (!aloft && PutAshoreNow(pTransport) > 0)
					{
						// Out: the next tick finds it empty, or with those that had no cell to stand on.
					}
					else if (pTransport->CurrentMission != Mission::Unload)
						pTransport->QueueMission(Mission::Unload, true);
				}
				else if (!pTarget)
				{
					// Before the drop the team takes another building instead of going home: behind a
					// main attack the army often brings the target down first. The next one is chosen
					// as the first was, the nearer to the transport the better, AirborneRetargets times
					// at most. Progress counts them. With the main attack over, the house of the old
					// target stands for the one under attack.
					AirborneGoal next {};

					if (plan.AuxBaseline < 2 && plan.Progress < AirborneRetargets && pTransport)
					{
						AirborneFrontLine front = findFront();

						if (front.Cell == CellStruct::Empty)
							front.Owner = plan.TargetOwner;

						// The team is aboard (in limbo) by now, or still on the map while it boards.
						std::vector<FootClass*> team;

						for (auto pAboard = pTransport->Passengers.GetFirstPassenger(); pAboard;
							pAboard = abstract_cast<FootClass*>(pAboard->NextObject))
						{
							team.push_back(pAboard);
						}

						for (const DWORD uid : infantry)
						{
							if (auto const pFoot = findOwnFoot(uid))
								team.push_back(pFoot);
						}

						next = pickGoal(front, team, [](BuildingTypeClass*) { return 0; }, pTransport->GetMapCoords());
					}

					if (next.Target)
					{
						plan.Progress++;
						plan.TargetID = next.Target->UniqueID;
						plan.TargetCell = next.Target->GetMapCoords();
						plan.StagingCell = next.Drop->MapCoords;
						plan.TargetOwner = next.Target->Owner ? next.Target->Owner->ArrayIndex : -1;
						plan.Deadline = std::max(plan.Deadline, Unsorted::CurrentFrame + AirborneRetargetFrames);
						Debug::Log("[NeuralAI] House %d AirborneFront: target gone while %s, the team takes %s at (%d,%d), frame %d.\n",
							pHouse->ArrayIndex, PhaseNames[std::clamp(plan.AuxBaseline, 0, 3)], next.Target->Type->ID, plan.TargetCell.X,
							plan.TargetCell.Y, Unsorted::CurrentFrame);
					}
					else if (plan.AuxBaseline >= 2)
					{
						logEnd("target gone");
						finishPlan(NeuralPlanId::AirborneFront, true, true);
					}
					else
					{
						standDown("target gone");
					}
				}
				else if (Unsorted::CurrentFrame >= plan.Deadline || !pDrop)
				{
					standDown("out of time");
				}
				else if (plan.AuxBaseline < 2 && (!pTransport || pTransport->Health <= 0 || pTransport->IsCrashing || pTransport->IsSinking))
				{
					// Shot down (or lost) before the drop. One still on the map while it goes down has
					// no passengers left either, and was taken for one that had let its team out.
					logEnd("transport lost");
					finishPlan(NeuralPlanId::AirborneFront, false, false);
				}
				else if (plan.AuxBaseline == 0)
				{
					// Boarding: the transport comes to meet its soldiers and parks, they walk up to it,
					// and each one that stands next to it at rest goes aboard at once (TakeAboardNow).
					// Asking to load over the radio, one soldier at a time through the transport's single
					// link, left the team outside in a quarter to a half of the plans, whatever was done
					// to the order (2026-11-13 and 14). Aboard they are in limbo, hence not found.
					// A jumpjet transport comes down only onto a free cell at the end of a move: over a
					// cell one of our soldiers stands on it climbs again and tries the next one, for as
					// long as they stand under it (JumpjetLocomotionClass, descent: 5 of 8 plans ended
					// with a SHAD or a HIND hanging over its team). It is sent to a free cell two cells
					// from every soldier (AnchorCell; AuxPeak = when), and they keep off that cell until
					// it is down.
					int waiting = 0;
					FootClass* pNearest = nullptr;
					int nearestDist = INT_MAX;

					// Where a soldier walks to: the cell next to the transport nearest to it that it can
					// stand on (fixed order among equals; with none it waits). Sent onto the transport's
					// own cell it makes the transport give way, and one that flies takes off to do so
					// (two SHAD parked by their team were found in the air again with nobody aboard).
					auto boardingCellFor = [&](FootClass* pBoarder) -> CellClass*
					{
						const CellStruct at = pTransport->GetMapCoords();
						const SpeedType speed = pBoarder->GetTechnoType()->SpeedType;
						const MovementZone zone = pBoarder->GetTechnoType()->MovementZone;
						CellClass* pBest = nullptr;
						int bestDist = INT_MAX;

						for (int dy = -1; dy <= 1; ++dy)
						{
							for (int dx = -1; dx <= 1; ++dx)
							{
								const CellStruct c { static_cast<short>(at.X + dx), static_cast<short>(at.Y + dy) };
								auto const pCell = (dx != 0 || dy != 0) && MapClass::Instance.CoordinatesLegal(c)
									? MapClass::Instance.TryGetCellAt(c) : nullptr;

								if (!pCell || pCell->GetBuilding() || !pCell->IsClearToMove(speed, true, false, -1, zone, -1, false))
									continue;

								const int dist = pBoarder->DistanceFrom(pCell);

								if (dist < bestDist)
								{
									bestDist = dist;
									pBest = pCell;
								}
							}
						}

						return pBest;
					};

					// A parked transport found on the move again is stopped before its soldiers are looked
					// at. An LCRF came to every tick with a destination, so the soldier next to it was
					// refused, and ended every tick parked again by the lines below: nobody aboard in
					// 1500 frames. One that flies sleeps instead (see below).
					if (plan.BoardingParked && !flyer && (pTransport->Destination || pTransport->CurrentMission == Mission::Move))
					{
						pTransport->SetDestination(nullptr, true);
						pTransport->ForceMission(Mission::Guard);
					}

					for (const DWORD uid : infantry)
					{
						auto const pInf = findOwnFoot(uid);

						if (!pInf)
							continue;

						if (TakeAboardNow(pTransport, pInf))
						{
							ownFoots.erase(uid);
							continue;
						}

						waiting++;
						const int dist = pTransport->DistanceFrom(pInf);

						if (dist < nearestDist || (dist == nearestDist && pInf->UniqueID < pNearest->UniqueID))
						{
							nearestDist = dist;
							pNearest = pInf;
						}

						// They walk to the cell the transport stands on, not to the transport: a move onto
						// a transport is an order to enter it, and the transport answers that one by
						// setting out for the soldier (two HIND down by their team took off again and
						// were still flying 1500 frames later).
						const bool entering = pInf->CurrentMission == Mission::Enter || pInf->QueuedMission == Mission::Enter;
						const bool beside = std::abs(pInf->Location.X - pTransport->Location.X) < BoardingReachLeptons
							&& std::abs(pInf->Location.Y - pTransport->Location.Y) < BoardingReachLeptons;

						if (!aloft && !entering)
						{
							if (auto const pCell = beside ? nullptr : boardingCellFor(pInf))
								IssueMoveOrder(pInf, pCell, 0.5);
						}
						else if (aloft && !entering && pLanding && pInf->GetMapCoords().DistanceFrom(plan.AnchorCell) > BoardingMeetCells)
							IssueMoveOrder(pInf, pLanding, 2.0);
						else if (entering || pInf->CurrentMission == Mission::Move)
						{
							if (pInf->ContainsLink(pTransport))
								pInf->SendCommand(RadioCommand::NotifyUnlink, pTransport);

							pInf->SetDestination(nullptr, true);
							pInf->ClearNavigationList();
							pInf->ForceMission(Mission::Guard);
						}
					}

					if (pNearest && pTransport->CurrentMission != Mission::Unload)
					{
						// A hover-attack flyer with a target stays in the air to shoot at it and never lands
						// for its soldiers: while it loads it has none.
						if (pTransport->Target)
							pTransport->SetTarget(nullptr);

						auto const pCarrier = abstract_cast<UnitClass*>(pTransport);
						const int soldierZone = MapClass::Instance.GetMovementZoneType(pNearest->GetMapCoords(), MovementZone::Normal, pNearest->OnBridge);

						// Once it has parked by its soldiers it stays: it sets out again only when the nearest
						// one is BoardingParkedReachCells away. A transport that follows every soldier takes
						// nobody in: only one at rest does.
						const int reachCells = plan.BoardingParked ? BoardingParkedReachCells : BoardingMeetCells;

						// The cell it flies to is the one it was sent to or one next to it (the locomotor
						// moves the goal to a free spot near). Any other destination is not ours: one on
						// Area Guard flew back to its old post and was waited for until the plan gave up.
						auto const pGoing = abstract_cast<CellClass*>(pTransport->Destination);
						const bool inbound = pLanding && aloft && pGoing
							&& pGoing->MapCoords.DistanceFrom(plan.AnchorCell) <= BoardingLandingNearCells;

						if (flyer)
						{
							// Parked means down with no move left to make. One on the ground with a destination
							// (a move from before the plan, or ours to its landing cell) is not: put to sleep
							// with it, a HIND was refused as a transport on the move and the engine took the
							// move up again; and taking the destination from a jumpjet sends it to a cell
							// near (Stop_Moving). It finishes the move, or gets a landing cell below.
							const bool onTheMove = pTransport->Destination || pTransport->CurrentMission == Mission::Move
								|| pTransport->QueuedMission == Mission::Move;

							if (!aloft && !onTheMove && nearestDist <= BoardingParkedReachCells * Unsorted::LeptonsPerCell)
							{
								// Down within a short walk of its soldiers: it stays and they come. It
								// sleeps meanwhile: on Guard a transport with a gun takes off for whatever
								// comes into its range, and soldiers on their way to it found it in the
								// air again (6 SHAD and HIND plans with nobody aboard after 1500 frames).
								plan.BoardingParked = true;

								if (plan.BoardingParkedFrame < 0)
									plan.BoardingParkedFrame = Unsorted::CurrentFrame;

								if (pTransport->CurrentMission != Mission::Sleep)
									pTransport->ForceMission(Mission::Sleep);
							}
							else if (inbound && pTransport->GetMapCoords().DistanceFrom(plan.AnchorCell) > BoardingLandingNearCells)
							{
								// On its way to the cell: the time it gets to come down runs from its arrival.
								plan.AuxPeak = Unsorted::CurrentFrame;
							}
							else if (pLanding && Unsorted::CurrentFrame - plan.AuxPeak < BoardingLandingFrames)
							{
								// Just sent, or coming down on the cell.
							}
							else if (auto const pCell = pickLandingCell(pNearest->GetMapCoords(), soldierZone, 2, plan.AnchorCell))
							{
								plan.AnchorCell = pCell->MapCoords;
								plan.AuxPeak = Unsorted::CurrentFrame;
								plan.BoardingParked = false;

								if (pTransport->CurrentMission == Mission::Sleep)
									pTransport->ForceMission(Mission::Guard);

								pTransport->SetArchiveTarget(nullptr);
								pTransport->SetDestination(pCell, true);
								pTransport->QueueMission(Mission::Move, true);
								NoteOrderCaller(pTransport, "move");
							}
						}
						else if (nearestDist > reachCells * Unsorted::LeptonsPerCell)
						{
							plan.BoardingParked = false;
							IssueMoveOrder(pTransport, pNearest->GetCell(), 2.0);
						}
						else if (pCarrier && MoveTransportToLoadingCell(pHouse, pCarrier, pCarrier->GetMapCoords(), soldierZone, true))
						{
							// On its way to a cell the soldiers can step next to: a flyer that hung over them
							// (the cells below taken) lands beside them instead, the ferry loader's way.
						}
						else
						{
							plan.BoardingParked = true;

							if (plan.BoardingParkedFrame < 0)
								plan.BoardingParkedFrame = Unsorted::CurrentFrame;

							if (pTransport->CurrentMission == Mission::Move || pTransport->Destination)
							{
								pTransport->SetDestination(nullptr, true);
								pTransport->ForceMission(Mission::Guard);
							}
							else if (!IsFerryAloft(pTransport) && pTransport->Locomotor->Is_Moving()
								&& IsFootHeldStill(pHouse, pTransport, BoardingSettleFrames))
							{
								// On Guard with no destination, on the same cell for BoardingSettleFrames, and its
								// locomotor still says it moves: it is told to stop.
								pTransport->Locomotor->Stop_Moving();
							}
						}
					}

					const int aboard = pTransport->Passengers.NumPassengers;

					// The waits run from the moment the transport first parked by its soldiers: counted from
					// the start of the plan they ran out while a transport that came from far was still
					// landing. One that has not parked in BoardingApproachFrames is given up as well.
					const int parkedFrames = plan.BoardingParkedFrame >= 0 ? Unsorted::CurrentFrame - plan.BoardingParkedFrame : -1;
					const bool nobodyCame = parkedFrames >= 0 ? parkedFrames >= BoardingWaitFrames
						: Unsorted::CurrentFrame - plan.StartFrame >= BoardingApproachFrames;

					// It leaves with all of them, or after BoardingPartialLoadFrames with a team of the
					// size it needs to start and half of its own at least (one left with 2 of its 11
					// soldiers; one with 7 of 12 lost them all at the drop).
					const int enough = std::max(std::min(AirborneTeamMin, pTransport->GetTechnoType()->Passengers),
						(static_cast<int>(infantry.size()) + 1) / 2);

					if (aboard > 0 && (waiting == 0 || (parkedFrames >= BoardingPartialLoadFrames && aboard >= enough)))
					{
						// It leaves. Those still outside go back to the army: as members they were sent
						// on foot to the target once the others had been dropped.
						const DWORD transportID = pTransport->UniqueID;

						std::erase_if(plan.Members, [&](DWORD uid)
							{
								if (uid == transportID || !findOwnFoot(uid))
									return false;

								auto const pLeft = findOwnFoot(uid);
								pLeft->SetDestination(nullptr, true);
								pLeft->ForceMission(Mission::Guard);
								macro.PlanMembers.erase(uid);

								return true;
							});

						if (pTransport->CurrentMission == Mission::Sleep)
							pTransport->ForceMission(Mission::Guard);

						plan.AuxBaseline = 1;
						plan.Deadline = Unsorted::CurrentFrame + AirborneTripFrames;
						plan.AnchorCell = CellStruct { 0, 0 };
						plan.AuxPeak = 0;
					}
					else if (nobodyCame)
					{
						// Not this transport again for a while: the same failure would repeat.
						auto const pTransportCell = pTransport->GetCell();
						Debug::Log("[NeuralAI] House %d AirborneFront: %s %u not boarded at (%d,%d)%s, nearest soldier %d cells away in mission %d.\n",
							pHouse->ArrayIndex, pTransport->GetTechnoType()->ID, pTransport->UniqueID, pTransport->GetMapCoords().X,
							pTransport->GetMapCoords().Y, (pTransportCell && pTransportCell->LandType == LandType::Water) ? " on water" : "",
							pNearest ? nearestDist / Unsorted::LeptonsPerCell : -1, pNearest ? static_cast<int>(pNearest->CurrentMission) : -1);

						macro.RaidTransportBanUntil[pTransport->UniqueID] = Unsorted::CurrentFrame + 9000;
						standDown(aboard > 0 ? "too few boarded" : "nobody boarded");
					}
				}
				else if (plan.AuxBaseline == 1)
				{
					const double toDrop = pTransport->GetMapCoords().DistanceFrom(pDrop->MapCoords);

					if (pTransport->Passengers.NumPassengers == 0)
					{
						plan.AuxBaseline = 2;
						plan.Arrived = true;
						plan.ArrivedFrame = Unsorted::CurrentFrame;
						plan.Deadline = Unsorted::CurrentFrame + AirborneAssaultFrames;
						Debug::Log("[NeuralAI] House %d AirborneFront: team dropped at (%d,%d) by %s, frame %d.\n", pHouse->ArrayIndex,
							plan.StagingCell.X, plan.StagingCell.Y, pTransport->GetTechnoType()->ID, Unsorted::CurrentFrame);

						if (flies)
							GetSafeReturnState(pHouse).Flights.push_back(pTransport->UniqueID);
					}
					else if (toDrop <= 3.0)
					{
						// At the drop cell. On the ground its team gets out at once (PutAshoreNow): the
						// unload mission waits for the locomotor to report no movement, which a
						// hovercraft does not while it floats (3 of 27 plans ran out of time with one
						// 4 to 6 cells from the target and the team aboard). In the air it is told to
						// unload, which brings it down, and has no target meanwhile: one with a gun
						// (HoverAttack) stays up to shoot (3 more of those 27). Still loaded
						// AirborneDropRetryFrames later, it is sent to the nearest free cell instead.
						// AuxPeak = when it was told to unload. Made to land first and unload after,
						// 7 of 13 transports were shot down.
						if (pTransport->Target)
							pTransport->SetTarget(nullptr);

						// While passengers keep getting out the wait starts again: the other cell is
						// for a transport that lets nobody out, not for one that takes its time (it
						// was sent away in the middle of unloading; from 8 of 15 plans well to 4 of 21).
						const int aboardNow = pTransport->Passengers.NumPassengers;

						if (plan.AuxPeak != 0 && aboardNow < plan.UnloadAboard)
							plan.AuxPeak = Unsorted::CurrentFrame;

						plan.UnloadAboard = aboardNow;

						if (!aloft && PutAshoreNow(pTransport) > 0)
						{
							// Out: the next tick finds it empty.
						}
						else if (plan.AuxPeak != 0 && Unsorted::CurrentFrame - plan.AuxPeak >= AirborneDropRetryFrames)
						{
							plan.AuxPeak = Unsorted::CurrentFrame;

							if (auto const pCell = pickLandingCell(pDrop->MapCoords, zoneOf(pDrop->MapCoords), 1, plan.StagingCell))
							{
								plan.StagingCell = pCell->MapCoords;
								pTransport->SetDestination(pCell, true);
								pTransport->QueueMission(Mission::Unload, true);
								NoteOrderCaller(pTransport, "unload");
							}
						}
						else if (pTransport->CurrentMission != Mission::Unload)
						{
							if (plan.AuxPeak == 0)
								plan.AuxPeak = Unsorted::CurrentFrame;

							pTransport->SetDestination(pDrop, true);
							pTransport->QueueMission(Mission::Unload, true);
							NoteOrderCaller(pTransport, "unload");
						}
					}
					else if (pTransport->GetHealthPercentage() < 0.35 && toDrop > AirbornePressOnCells)
					{
						// Badly hit far from the drop it turns back; near it, it goes on: its soldiers
						// are worth more on the ground there than aboard on the way home (4 of 15 plans
						// ended here with the team aboard).
						standDown("transport badly hit");
					}
					else
					{
						IssueMoveOrder(pTransport, flies ? GetSafeAirHop(pHouse, pTransport, pDrop) : pDrop, 2.0);
					}
				}
				else
				{
					// Dropped: specialists go into the target (capture, infiltrate), area damage
					// soldiers deploy among the enemy, vehicle thieves are left to hunt vehicles, the
					// rest attack; the transport flies home.
					std::vector<DWORD> fighters;
					int alive = 0;

					for (const DWORD uid : infantry)
					{
						auto const pFoot = findOwnFoot(uid);
						auto const pInf = abstract_cast<InfantryClass*>(pFoot);

						if (!pFoot)
							continue;

						alive++;

						if (pInf && SpecialistWorksBuilding(pInf->Type, pTarget->Type))
							OrderFootToEnter(pFoot, pTarget);
						else if (pInf && pInf->Type->VehicleThief)
							continue;
						else if (pInf && pInf->Type->Deployer && pInf->Type->DeployFire && !pInf->IsDeployed())
						{
							if (pFoot->GetMapCoords().DistanceFrom(pTarget->GetMapCoords()) <= 6.0)
								pFoot->QueueMission(Mission::Unload, true);
							else
								fighters.push_back(uid);
						}
						else if (!(pInf && pInf->IsDeployed()))
							fighters.push_back(uid);
					}

					attackTargetGroup(fighters, pTarget);

					if (pTransport)
					{
						if (auto const pHome = FindCarrierHomeCell(pHouse, pTransport))
							IssueMoveOrder(pTransport, flies ? GetSafeAirHop(pHouse, pTransport, pHome) : pHome, 4.0);
					}

					// Captured (engineers) or the whole team gone.
					if (pTarget->Owner && pHouse->IsAlliedWith(pTarget->Owner))
					{
						Debug::Log("[NeuralAI] House %d AirborneFront: %s at (%d,%d) captured, frame %d.\n", pHouse->ArrayIndex,
							pTarget->Type->ID, plan.TargetCell.X, plan.TargetCell.Y, Unsorted::CurrentFrame);
						logEnd("captured");
						finishPlan(NeuralPlanId::AirborneFront, true, false);
					}
					else if (alive == 0)
					{
						logEnd("team gone");
						finishPlan(NeuralPlanId::AirborneFront, false, false);
					}
				}
			}
			else if (!planShelved(NeuralPlanId::AirborneFront) && planReady(NeuralPlanId::AirborneFront))
			{
				// Only behind a main attack. Dropped with no attack to hide behind (a lone raider or
				// specialist, a strike squad), the team met the whole garrison: 4 of 149 such raids
				// went well, whatever the size of the team (2026-11-14; user: limit it to a main
				// attack in progress).
				const AirborneFrontLine front = findFront();
				FootClass* pTransport = nullptr;

				for (auto const& [uid, pFoot] : ownFoots)
				{
					if (front.Cell == CellStruct::Empty || macro.PlanMembers.contains(uid) || pFoot->Passengers.NumPassengers != 0
						|| pFoot->GetHealthPercentage() < 0.6 || (pTransport && pTransport->UniqueID < uid))
					{
						continue;
					}

					// A carryall slinging a vehicle (or on its way to one) takes no soldiers.
					if (auto const pCarryUnit = abstract_cast<UnitClass*>(pFoot))
					{
						auto const pCarryState = UnitExt::Fetch(pCarryUnit);

						if (pCarryState && (pCarryState->CarryallCargo || pCarryState->CarryallState != JumpjetCarryallState::Idle))
							continue;
					}

					if (auto const ban = macro.RaidTransportBanUntil.find(uid);
						ban != macro.RaidTransportBanUntil.end() && Unsorted::CurrentFrame < ban->second)
					{
						continue;
					}

					bool isCandidate = false;

					if (auto const pAir = abstract_cast<AircraftClass*>(pFoot))
					{
						isCandidate = pAir->Type && !pAir->Type->Carryall && pAir->Type->Passengers >= 3
							&& (pAir->CurrentMission == Mission::Guard || pAir->CurrentMission == Mission::Sleep
								|| pAir->CurrentMission == Mission::Area_Guard);
					}
					else if (auto const pUnit = abstract_cast<UnitClass*>(pFoot))
					{
						// Flying transport vehicles (HIND, transport disk), which take the safe air lane, or
						// burrowing ones, which travel unseen under the defenses. Not the ones that go by
						// land or sea (hovercraft, amphibious carriers): they drive their team through
						// whatever stands on the way, and 4 of some 33 such plans went well against 10 of
						// 14 by air or underground, with every transport lost on the way one of theirs.
						const bool flying = pUnit->Type && IsAmphibiousOrAirTransport(pUnit->Type)
							&& (pUnit->Type->JumpJet || pUnit->Type->ConsideredAircraft || pUnit->Type->MovementZone == MovementZone::Fly);
						const bool burrows = IsUndergroundTransportType(pUnit->Type);

						// An empty transport waiting on a pickup shore keeps a ferry record that has not left
						// (Departed=no) for as long as some ferry demand exists, even on a land map; only a
						// trip under way holds it. The ferry loop skips plan members.
						// One that holds a radio link is already taking somebody in (an MCV on its way to an
						// island, a unit the ferry loader sent).
						auto const pFerryTrip = FindFerryTrip(uid);
						isCandidate = pUnit->Type && (flying || burrows)
							&& pUnit->Type->Passengers >= (burrows ? 1 : 3) && isFreeForPlan(pUnit)
							&& !(pFerryTrip && pFerryTrip->Departed) && !pUnit->GetNthLink(0);
					}

					if (isCandidate)
						pTransport = pFoot;
				}

				// A specialist (engineer, spy) rides along only to a building it can work (capture,
				// infiltrate); vehicle thieves only fill seats and hunt vehicles once down.
				const int transportZone = pTransport ? ObjectZone(pTransport, MovementZone::Normal) : -1;
				std::vector<FootClass*> specialists;
				std::vector<FootClass*> soldiers;
				std::vector<FootClass*> vehicles;

				auto isRaider = [](FootClass* pFoot)
				{
					if (auto const pInf = abstract_cast<InfantryClass*>(pFoot))
						return IsRaiderInfantryType(pInf->Type);

					auto const pUnit = abstract_cast<UnitClass*>(pFoot);

					return pUnit && IsVehicleRaiderType(pUnit->Type);
				};

				for (auto const& [uid, pFoot] : ownFoots)
				{
					if (!pTransport || pFoot == pTransport || ObjectZone(pFoot, MovementZone::Normal) != transportZone)
						continue;

					auto const pInf = abstract_cast<InfantryClass*>(pFoot);
					auto const pUnit = abstract_cast<UnitClass*>(pFoot);

					if (pInf && IsSpecialistInfantryType(pInf->Type))
					{
						if (!macro.PlanMembers.contains(uid) && !(pInf->Type->Engineer && IsSpecOpsEngineerBusy(pHouse, uid))
							&& pFoot->GetHealthPercentage() >= 0.6
							&& (pFoot->CurrentMission == Mission::Guard || pFoot->CurrentMission == Mission::Area_Guard))
						{
							specialists.push_back(pFoot);
						}
					}
					else if (!isFreeForPlan(pFoot))
						continue;
					else if (pInf && isArmedGroundFoot(pFoot))
						soldiers.push_back(pFoot);
					else if (pUnit && IsVehicleRaiderType(pUnit->Type)
						&& pUnit->GetTechnoType()->Size <= pTransport->GetTechnoType()->SizeLimit)
					{
						vehicles.push_back(pFoot);
					}
				}

				// The team is taken from our bases, within AirborneHomeCells of a building of ours:
				// soldiers out at the front are in the fight the drop is meant to help, and a team
				// picked there, 56 cells from its transport, was gone before the transport came.
				auto awayFromHome = [&](FootClass* pFoot)
				{
					const CellStruct at = pFoot->GetMapCoords();

					for (auto const pBld : pHouse->Buildings)
					{
						if (pBld && pBld->IsAlive && !pBld->InLimbo && pBld->GetMapCoords().DistanceFrom(at) <= AirborneHomeCells)
							return false;
					}

					return true;
				};

				std::erase_if(specialists, awayFromHome);
				std::erase_if(soldiers, awayFromHome);
				std::erase_if(vehicles, awayFromHome);

				// It musters around the soldier nearest to the transport.
				FootClass* pAnchor = nullptr;

				for (auto const pFoot : soldiers)
				{
					const int dist = pTransport->DistanceFrom(pFoot);
					const int anchorDist = pAnchor ? pTransport->DistanceFrom(pAnchor) : INT_MAX;

					if (dist < anchorDist || (dist == anchorDist && pFoot->UniqueID < pAnchor->UniqueID))
						pAnchor = pFoot;
				}

				// A transport that does not fly meets them only from within AirborneSurfaceMeetCells
				// (one came from 120 cells away and was lost on the way to its soldiers).
				if (pAnchor && !pTransport->GetTechnoType()->JumpJet && pTransport->GetTechnoType()->MovementZone != MovementZone::Fly
					&& pTransport->WhatAmI() != AbstractType::Aircraft
					&& pTransport->GetMapCoords().DistanceFrom(pAnchor->GetMapCoords()) > AirborneSurfaceMeetCells)
				{
					pAnchor = nullptr;
				}

				auto farFromAnchor = [&](FootClass* pFoot)
				{
					return !pAnchor || pFoot->GetMapCoords().DistanceFrom(pAnchor->GetMapCoords()) > AirborneMusterCells;
				};

				std::erase_if(specialists, farFromAnchor);
				std::erase_if(soldiers, farFromAnchor);
				std::erase_if(vehicles, farFromAnchor);

				auto byID = [](FootClass* a, FootClass* b) { return a->UniqueID < b->UniqueID; };
				std::sort(specialists.begin(), specialists.end(), byID);
				std::sort(vehicles.begin(), vehicles.end(), byID);
				std::sort(soldiers.begin(), soldiers.end(), [&](FootClass* a, FootClass* b)
					{
						const bool ra = isRaider(a);
						const bool rb = isRaider(b);

						return ra != rb ? ra : a->UniqueID < b->UniqueID;
					});

				if (specialists.size() > 3)
					specialists.resize(3);

				// Specialists of ours that have a job inside this building.
				auto specialistsFor = [&](BuildingTypeClass* pBldType)
				{
					int count = 0;

					for (auto const pFoot : specialists)
					{
						if (SpecialistWorksBuilding(static_cast<InfantryClass*>(pFoot)->Type, pBldType))
							count++;
					}

					return count;
				};

				// The team for a building (none: no specialist has a job). Boarding order: raiders
				// (soldiers, then demolition vehicles that fit), the specialists with a job there, the
				// other soldiers by UniqueID, then vehicle thieves; seats are counted by passenger
				// Size, each passenger within the transport's SizeLimit.
				auto seatTeam = [&](BuildingTypeClass* pBldType) -> std::vector<FootClass*>
				{
					std::vector<FootClass*> order;
					auto const firstOther = std::find_if(soldiers.begin(), soldiers.end(), [&](FootClass* pFoot) { return !isRaider(pFoot); });
					order.assign(soldiers.begin(), firstOther);
					order.insert(order.end(), vehicles.begin(), vehicles.end());

					for (auto const pFoot : specialists)
					{
						if (pBldType && SpecialistWorksBuilding(static_cast<InfantryClass*>(pFoot)->Type, pBldType))
							order.push_back(pFoot);
					}

					order.insert(order.end(), firstOther, soldiers.end());

					for (auto const pFoot : specialists)
					{
						if (static_cast<InfantryClass*>(pFoot)->Type->VehicleThief)
							order.push_back(pFoot);
					}

					const int capacity = pTransport ? std::max(0, pTransport->GetTechnoType()->Passengers) : 0;
					int usedSeats = 0;
					std::vector<FootClass*> team;

					for (auto const pFoot : order)
					{
						const int size = std::max(1, static_cast<int>(pFoot->GetTechnoType()->Size));

						if (usedSeats + size > capacity)
							continue;

						usedSeats += size;
						team.push_back(pFoot);
					}

					return team;
				};

				const AirborneGoal goal = pTransport
					? pickGoal(front, seatTeam(nullptr), specialistsFor, CellStruct::Empty) : AirborneGoal {};
				std::vector<FootClass*> infantry;
				int raiders = 0;
				int workersAboard = 0;

				if (goal.Target)
				{
					infantry = seatTeam(goal.Target->Type);

					for (auto const pFoot : infantry)
					{
						auto const pInf = abstract_cast<InfantryClass*>(pFoot);

						if (isRaider(pFoot))
							raiders++;
						else if (pInf && SpecialistWorksBuilding(pInf->Type, goal.Target->Type))
							workersAboard++;
					}
				}

				// The team: AirborneTeamMin strong, or as many as a smaller transport holds.
				const bool teamReady = goal.Target
					&& static_cast<int>(infantry.size()) >= std::min(AirborneTeamMin, pTransport->GetTechnoType()->Passengers);
				const int utility = (goal.Target && teamReady) ? (320 + goal.Score / 6 + raiders * 150 + workersAboard * 120) : -1000;

				if (utility > 0 && shouldTryStart(NeuralPlanId::AirborneFront, utility))
				{
					std::vector<FootClass*> group { pTransport };
					group.insert(group.end(), infantry.begin(), infantry.end());
					startPlan(NeuralPlanId::AirborneFront, group, goal.Target->GetMapCoords(), goal.Drop->MapCoords, goal.Target->UniqueID, 4500);

					// The raid gate may have held it back.
					if (plan.Active)
					{
						ClearFerryTrip(pTransport->UniqueID);
						plan.AuxBaseline = 0;

						Debug::Log("[NeuralAI] House %d AirborneFront: %s %u carries %d soldiers (%d raiders, %d specialists) to %s of house %d, "
							"dropped at (%d,%d), frame %d.\n", pHouse->ArrayIndex, pTransport->GetTechnoType()->ID, pTransport->UniqueID,
							static_cast<int>(infantry.size()), raiders, workersAboard, goal.Target->Type->ID,
							goal.Target->Owner ? goal.Target->Owner->ArrayIndex : -1, goal.Drop->MapCoords.X, goal.Drop->MapCoords.Y,
							Unsorted::CurrentFrame);
					}
				}
			}
		}

		// ---------------------------------------------------------------- Plan 10: Expansion Denial
		// A visible enemy MCV on the move, or a known isolated new enemy outpost (ConYard or
		// refinery with no known defenses, away from the rest of its owner's known base), is hit
		// at once by a fast group before it can dig in. A hunted MCV that deploys is followed to
		// its new ConYard.
		{
			auto& plan = planOf(NeuralPlanId::ExpansionDenial);

			if (plan.Active)
			{
				TechnoClass* pTarget = findTechnoByID(plan.TargetID);

				if (!pTarget && plan.AuxPeak == 0)
				{
					for (auto const pBld : knownEnemyBuildingList)
					{
						if (isKnownEnemyBuilding(pBld) && pBld->Type->ConstructionYard && pBld->GetMapCoords().DistanceFrom(plan.TargetCell) <= 5.0)
						{
							plan.TargetID = pBld->UniqueID;
							plan.AuxPeak = 1;
							pTarget = pBld;
							break;
						}
					}
				}

				if (!pTarget)
				{
					finishPlan(NeuralPlanId::ExpansionDenial, true, true);
				}
				else if (memberValue(plan) * 100 < plan.InitialValue * 40 || Unsorted::CurrentFrame >= plan.Deadline)
				{
					finishPlan(NeuralPlanId::ExpansionDenial, false, true);
				}
				else if (pTarget->WhatAmI() == AbstractType::Building || IsCellVisibleToAI(pHouse, pTarget->GetCell()))
				{
					// One column that fights its way there and attacks the target from 9 cells. Every
					// member used to go straight for it on its own attack order, the fastest ahead,
					// none answering what shot at it on the way: 11 of 26 plans of 39 matches
					// (2026-11-15) lost their strength with no enemy seen around the group.
					plan.TargetCell = pTarget->GetMapCoords();
					strikeWithMembers(columnMovingOn(plan.Members, plan.TargetCell), pTarget);
				}
				else
				{
					// Out of sight: head for where it was last seen.
					attackMoveGroup(columnMovingOn(plan.Members, plan.TargetCell), MapClass::Instance.TryGetCellAt(plan.TargetCell));
				}
			}
			else if (planReady(NeuralPlanId::ExpansionDenial))
			{
				TechnoClass* pBest = nullptr;
				double bestDist = 1e9;
				bool bestIsMCV = false;

				for (auto const pUnit : UnitClass::Array)
				{
					if (!pUnit || !pUnit->IsAlive || pUnit->InLimbo || !pUnit->Type || !pUnit->Owner || pUnit->Owner == pHouse
						|| pHouse->IsAlliedWith(pUnit->Owner) || pUnit->Owner->IsNeutral() || !pUnit->Type->DeploysInto
						|| !pUnit->Type->DeploysInto->ConstructionYard || !IsCellVisibleToAI(pHouse, pUnit->GetCell()))
					{
						continue;
					}

					const CellStruct uc = pUnit->GetMapCoords();
					const double d = uc.DistanceFrom(homeCell);

					if (zoneOf(uc) == homeZone && d < bestDist)
					{
						bestDist = d;
						pBest = pUnit;
						bestIsMCV = true;
					}
				}

				if (!pBest)
				{
					for (auto const pBld : knownEnemyBuildingList)
					{
						if (!isKnownEnemyBuilding(pBld)
							|| (!pBld->Type->ConstructionYard && !pBld->Type->Refinery && !pBld->Type->ResourceDestination))
						{
							continue;
						}

						const CellStruct bc = pBld->GetMapCoords();

						if (zoneOf(bc) != homeZone || CountKnownEnemyDefensesNear(pHouse, bc, 9.0, false) > 0)
							continue;

						// Isolated: at most one other known building around it, while its owner has
						// known buildings elsewhere (its main base), so this is an expansion.
						int neighbours = 0;
						bool ownerHasMainBase = false;

						for (auto const pOther : knownEnemyBuildingList)
						{
							if (pOther == pBld || !isKnownEnemyBuilding(pOther) || pOther->Owner != pBld->Owner)
								continue;

							const double d = pOther->GetMapCoords().DistanceFrom(bc);

							if (d <= 10.0)
								neighbours++;
							else if (d >= 22.0)
								ownerHasMainBase = true;
						}

						const double d = bc.DistanceFrom(homeCell);

						if (neighbours <= 1 && ownerHasMainBase && d < bestDist)
						{
							bestDist = d;
							pBest = pBld;
						}
					}
				}

				std::vector<FootClass*> group;

				if (pBest)
				{
					group = collectIdleGround(false);
					sortFastest(group);
					group.resize(std::min<size_t>(group.size(), 6));
				}

				const int utility = (pBest && group.size() >= 3) ? (330 + (bestIsMCV ? 120 : 0)) : -1000;

				if (utility > 0 && shouldTryStart(NeuralPlanId::ExpansionDenial, utility))
					startPlan(NeuralPlanId::ExpansionDenial, group, pBest->GetMapCoords(), CellStruct::Empty, pBest->UniqueID, 3600);
			}
		}

		// ---------------------------------------------------------------- Plan 11: Base Trade
		// Most of the enemy army we know of is attacking our base and our defenses can hold:
		// instead of only defending, a strike group hits the enemy's now empty base.
		{
			auto& plan = planOf(NeuralPlanId::BaseTrade);

			if (plan.Active)
			{
				auto const pTarget = FindBuildingByID(plan.TargetID);

				if (!pTarget)
					finishPlan(NeuralPlanId::BaseTrade, true, true);
				else if (memberValue(plan) * 100 < plan.InitialValue * 40 || Unsorted::CurrentFrame >= plan.Deadline
					|| pHouse->NumConYards == 0)
					finishPlan(NeuralPlanId::BaseTrade, false, true);
				else
					attackMoveGroup(plan.Members, pTarget->GetCell());
			}
			else if (planReady(NeuralPlanId::BaseTrade))
			{
				const int enemyAtBase = seenEnemyValueNear(homeCell, 22.0, 300);
				const bool enemyCommitted = enemyAtBase >= 150 && enemyAtBase * 10 >= foe.TotalValue * 6 && ownHomeDefenses >= 3;
				BuildingClass* pTarget = nullptr;
				int bestScore = INT_MIN;

				if (enemyCommitted)
				{
					for (auto const pBld : knownEnemyBuildingList)
					{
						if (!isKnownEnemyBuilding(pBld))
							continue;

						const CellStruct bc = pBld->GetMapCoords();
						const int worth = valuableBuildingWorth(pBld->Type);
						const int defenses = CountKnownEnemyDefensesNear(pHouse, bc, 9.0, false);

						if (worth <= 0 || defenses > 2 || zoneOf(bc) != homeZone)
							continue;

						const int score = worth - defenses * 40 - static_cast<int>(bc.DistanceFrom(homeCell)) / 3;

						if (score > bestScore)
						{
							bestScore = score;
							pTarget = pBld;
						}
					}
				}

				std::vector<FootClass*> group;

				if (pTarget)
				{
					group = collectIdleGround(false);
					sortFastest(group);
					group.resize(std::min<size_t>(group.size(), std::min<size_t>(8, (group.size() + 1) / 2)));
				}

				const int utility = (pTarget && group.size() >= 3) ? (300 + std::min(200, enemyAtBase / 4)) : -1000;

				if (utility > 0 && shouldTryStart(NeuralPlanId::BaseTrade, utility))
					startPlan(NeuralPlanId::BaseTrade, group, pTarget->GetMapCoords(), CellStruct::Empty, pTarget->UniqueID, 4500);
			}
		}

		// ---------------------------------------------------------------- Plan 12: Hold Under Towers
		// A clearly stronger ground force approaches (findHoldTowersCase): our ground army falls back
		// under our base defenses (AuxBaseline 0), lets the enemy bleed there, then counterattacks the
		// survivors (AuxBaseline 1) once the seen enemy value dropped to 60% of its peak or below
		// ours. It went well when that force is down to HoldTowersRepelledPercent of its peak, killed
		// or gone, and the group kept HoldTowersKeptPercent of its value. AuxPeak = the peak of the
		// ground enemy value near home; Arrived = a counterattack fell back under the towers.
		{
			auto& plan = planOf(NeuralPlanId::HoldUnderTowers);

			if (plan.Active)
			{
				auto const pRally = MapClass::Instance.TryGetCellAt(plan.StagingCell);
				// Same radius as the start check, so a force 27..30 cells out that started the plan is
				// not read as one that never came.
				const int enemyNear = seenGroundEnemyValueNear(homeCell, HoldTowersEnemyCells, 150);
				const int ourValue = memberValue(plan);
				const int strength = groupStrength(plan.Members);
				plan.AuxPeak = std::max(plan.AuxPeak, enemyNear);
				const bool repelled = enemyNear * 100 <= plan.AuxPeak * HoldTowersRepelledPercent;
				const bool kept = ourValue * 100 >= plan.InitialValue * HoldTowersKeptPercent;

				// The enemy is still at our towers when the time is up and the group holds: hold on
				// (once, 1800 frames) instead of releasing the army in the middle of the defense.
				if (pRally && Unsorted::CurrentFrame >= plan.Deadline && plan.Progress == 0 && !repelled
					&& strength * 100 >= plan.InitialStrength * 40)
				{
					plan.Progress = 1;
					plan.Deadline += 1800;
				}

				if (ourValue <= 0)
				{
					// The whole group fell: it ended there, not at the deadline (27 of 53 holds ran out of
					// time with nobody left, 2026-11-15).
					finishPlan(NeuralPlanId::HoldUnderTowers, false, false);
				}
				else if (!pRally || Unsorted::CurrentFrame >= plan.Deadline)
				{
					// The base held with the army: the attack is over, or what stays of it is no longer
					// that much stronger than the group.
					finishPlan(NeuralPlanId::HoldUnderTowers, kept && (repelled || enemyNear * 100 <= strength * HoldTowersOddsPercent), false);
				}
				else if (repelled)
				{
					finishPlan(NeuralPlanId::HoldUnderTowers, kept, false);
				}
				else if (plan.AuxBaseline == 0)
				{
					moveGroup(plan.Members, pRally, 5.0);

					// After a counterattack that fell back, only a group stronger than what is left
					// goes out again.
					if ((!plan.Arrived && enemyNear * 10 <= plan.AuxPeak * 6) || strength * 10 >= enemyNear * 12)
						plan.AuxBaseline = 1;
				}
				else
				{
					const CellStruct enemyCenter = seenGroundEnemyCentroidNear(homeCell, HoldTowersEnemyCells, 150);

					// The counterattack meets more than it can take: back under the towers.
					if (enemyCenter != CellStruct::Empty && seenGroundEnemyValueNear(enemyCenter, 10.0, 150) * 10 > strength * 12)
					{
						plan.AuxBaseline = 0;
						plan.Arrived = true;
						moveGroup(plan.Members, pRally, 5.0);
					}
					else if (enemyCenter != CellStruct::Empty)
					{
						attackMoveGroup(plan.Members, MapClass::Instance.TryGetCellAt(enemyCenter));
					}
				}
			}
			else if (planReady(NeuralPlanId::HoldUnderTowers))
			{
				auto const hold = findHoldTowersCase();

				if (!hold.Army.empty() && shouldTryStart(NeuralPlanId::HoldUnderTowers, 380))
				{
					startPlan(NeuralPlanId::HoldUnderTowers, hold.Army, homeCell, hold.Rally->MapCoords, 0, 3600);
					auto& started = planOf(NeuralPlanId::HoldUnderTowers);
					started.AuxBaseline = 0;
					started.AuxPeak = hold.EnemyNear;

					if (hold.RallyOnRoute)
						CountDecision(NeuralDecision::RouteAmbushSpot, pHouse->ArrayIndex);
				}
			}
		}

		// ---------------------------------------------------------------- Plan 13: Superweapon Race
		// An enemy superweapon with a public countdown (ShowTimer=yes, shown to every player)
		// is about to fire and we know its building: strike it before the timer runs out, by
		// air when the target is lightly covered and the wing can bring it down, else with a ground
		// group (pickStrikeGroup, as the other strikes at a building) that marches as one column
		// (columnMovingOn): its members were lost 48 cells from the target at the median while the
		// group's centre was still 52 cells from it, and the watchdog turned 36 of 42 plans back
		// (focus batch of 2026-11-15).
		{
			auto& plan = planOf(NeuralPlanId::SuperweaponRace);

			if (plan.Active)
			{
				auto const pTarget = FindBuildingByID(plan.TargetID);

				if (!pTarget)
					finishPlan(NeuralPlanId::SuperweaponRace, true, true);
				else if (memberValue(plan) * 100 < plan.InitialValue * 40 || Unsorted::CurrentFrame >= plan.Deadline)
					finishPlan(NeuralPlanId::SuperweaponRace, false, true);
				else
					strikeWithMembers(columnMovingOn(plan.Members, plan.TargetCell), pTarget);
			}
			else if (planReady(NeuralPlanId::SuperweaponRace))
			{
				BuildingClass* pTarget = nullptr;
				int bestLeft = INT_MAX;

				for (auto const pEnemy : HouseClass::Array)
				{
					if (!pEnemy || pEnemy == pHouse || pHouse->IsAlliedWith(pEnemy) || pEnemy->IsNeutral() || pEnemy->Defeated
						|| pEnemy->IsObserver())
					{
						continue;
					}

					for (int swIndex = 0; swIndex < pEnemy->Supers.Count; ++swIndex)
					{
						auto const pSuper = pEnemy->Supers.GetItem(swIndex);

						if (!pSuper || !pSuper->Type || !pSuper->IsPresent || !pSuper->Type->ShowTimer)
							continue;

						const int left = pSuper->IsReady ? 0 : pSuper->RechargeTimer.GetTimeLeft();

						if (left > 5400 || left >= bestLeft)
							continue;

						for (auto const pBld : BuildingClass::Array)
						{
							if (pBld && pBld->Owner == pEnemy && isKnownEnemyBuilding(pBld)
								&& (pBld->Type->SuperWeapon == swIndex || pBld->Type->SuperWeapon2 == swIndex))
							{
								pTarget = pBld;
								bestLeft = left;
								break;
							}
						}
					}
				}

				// The air wing must be able to bring the building down in its sorties: two aircraft were
				// sent at whatever they could reach, and the superweapon buildings outlast them (in the
				// archive of 2026-11-14 night both wings of two were shot down at the target, the
				// superweapon standing); Air Strike went from 14% to 42% well with that test. The
				// superweapon stands deep in the base: the ground group goes only with the strength to
				// beat what is known on its way in, and only units that hurt the building go: the fastest
				// idle units took dogs, terror drones and mind controllers that cannot scratch it along
				// (18 of the 87 members lost in the focus batch of 2026-11-15).
				std::vector<FootClass*> group = pickStrikeGroup(pTarget, SuperweaponRaceMinGround, SuperweaponRaceMaxGround, true);
				const int utility = (pTarget && !group.empty()) ? (400 + (5400 - bestLeft) / 20) : -1000;

				if (utility > 0 && shouldTryStart(NeuralPlanId::SuperweaponRace, utility))
				{
					// Deadline from the trip, not only the timer, so a weapon that is already ready can
					// still be reached and destroyed before it fires again.
					int travelFrames = 0;

					for (auto const pFoot : group)
					{
						const int framesPerCell = pFoot->WhatAmI() == AbstractType::Aircraft ? 12 : 40;
						travelFrames = std::max(travelFrames,
							static_cast<int>(pFoot->GetMapCoords().DistanceFrom(pTarget->GetMapCoords())) * framesPerCell);
					}

					startPlan(NeuralPlanId::SuperweaponRace, group, pTarget->GetMapCoords(), CellStruct::Empty, pTarget->UniqueID,
						std::max(std::max(0, bestLeft), travelFrames) + 1800);
					strikeWithMembers(planOf(NeuralPlanId::SuperweaponRace).Members, pTarget);
				}
			}
		}

		// ---------------------------------------------------------------- Plan 14: Pincer
		// Two real groups hit the same known enemy production building from two angles: the
		// direct group stages 14 cells before it on our side, the flank group 16 cells to the
		// side; when both are in place (or after 1500 frames) both attack together.
		// AuxBaseline = size of the direct group (Members[0..AuxBaseline)), AuxPeak = phase.
		{
			auto& plan = planOf(NeuralPlanId::Pincer);

			if (plan.Active)
			{
				auto const pTarget = FindBuildingByID(plan.TargetID);
				const size_t split = std::min<size_t>(plan.Members.size(), static_cast<size_t>(std::max(0, plan.AuxBaseline)));
				const std::vector<DWORD> direct(plan.Members.begin(), plan.Members.begin() + split);
				const std::vector<DWORD> flank(plan.Members.begin() + split, plan.Members.end());

				if (!pTarget)
				{
					finishPlan(NeuralPlanId::Pincer, true, true);
				}
				else if (memberValue(plan) * 100 < plan.InitialValue * 40 || Unsorted::CurrentFrame >= plan.Deadline)
				{
					finishPlan(NeuralPlanId::Pincer, false, true);
				}
				else if (plan.AuxPeak == 0)
				{
					auto const pDirectStage = landCellInZone(pointAlong(plan.TargetCell, homeCell, 14.0, 0.0), homeZone, 4);
					auto const pFlankStage = MapClass::Instance.TryGetCellAt(plan.StagingCell);
					moveGroup(direct, pDirectStage, 3.0);
					moveGroup(flank, pFlankStage, 3.0);

					const CellStruct dc = groupCentroid(direct);
					const CellStruct fc = groupCentroid(flank);
					const bool directReady = dc == CellStruct::Empty || (pDirectStage && dc.DistanceFrom(pDirectStage->MapCoords) <= 5.0);
					const bool flankReady = fc == CellStruct::Empty || (pFlankStage && fc.DistanceFrom(pFlankStage->MapCoords) <= 5.0);

					if ((directReady && flankReady) || Unsorted::CurrentFrame - plan.StartFrame >= 1500)
						plan.AuxPeak = 1;
				}
				else
				{
					attackMoveGroup(plan.Members, pTarget->GetCell());
				}
			}
			else if (planReady(NeuralPlanId::Pincer))
			{
				std::vector<FootClass*> army = collectIdleGround(false);
				BuildingClass* pTarget = nullptr;
				double bestDist = 1e9;

				if (army.size() >= 10)
				{
					for (auto const pBld : knownEnemyBuildingList)
					{
						if (!isKnownEnemyBuilding(pBld)
							|| (pBld->Type->Factory == AbstractType::None && !pBld->Type->ConstructionYard && !pBld->Type->Refinery))
						{
							continue;
						}

						const CellStruct bc = pBld->GetMapCoords();
						const double d = bc.DistanceFrom(homeCell);

						if (zoneOf(bc) == homeZone && d >= 24.0 && d < bestDist)
						{
							bestDist = d;
							pTarget = pBld;
						}
					}
				}

				CellClass* pFlankStage = nullptr;

				if (pTarget)
				{
					// Flank on the side with fewer known defenses.
					const CellStruct tc = pTarget->GetMapCoords();
					auto const pLeft = landCellInZone(pointAlong(tc, homeCell, 10.0, 16.0), homeZone, 4);
					auto const pRight = landCellInZone(pointAlong(tc, homeCell, 10.0, -16.0), homeZone, 4);
					const int leftDefs = pLeft ? CountKnownEnemyDefensesNear(pHouse, pLeft->MapCoords, 9.0, false) : 99;
					const int rightDefs = pRight ? CountKnownEnemyDefensesNear(pHouse, pRight->MapCoords, 9.0, false) : 99;
					pFlankStage = (leftDefs <= rightDefs) ? pLeft : pRight;
				}

				const int utility = (pTarget && pFlankStage) ? (300 + std::min(200, static_cast<int>(army.size()) * 10)) : -1000;

				if (utility > 0 && shouldTryStart(NeuralPlanId::Pincer, utility))
				{
					army.resize(std::min<size_t>(army.size(), 16));
					const int directCount = static_cast<int>(army.size() / 2);
					startPlan(NeuralPlanId::Pincer, army, pTarget->GetMapCoords(), pFlankStage->MapCoords, pTarget->UniqueID, 5400);
					planOf(NeuralPlanId::Pincer).AuxBaseline = directCount;
					planOf(NeuralPlanId::Pincer).AuxPeak = 0;
				}
			}
		}

		// ---------------------------------------------------------------- Plan 15: Harvester Ambush
		// A few fast (preferably cloakable) vehicles wait on the route between a known enemy
		// refinery and its nearest resource field, away from its defenses, and shoot the
		// harvesters going by. AuxPeak counts AI ticks with an enemy harvester in sight there.
		{
			auto& plan = planOf(NeuralPlanId::HarvesterAmbush);

			auto enemyHarvestersNear = [&](const CellStruct& cell, double radius) -> int
			{
				int count = 0;

				for (auto const pUnit : UnitClass::Array)
				{
					if (pUnit && pUnit->IsAlive && !pUnit->InLimbo && pUnit->Type && pUnit->Owner && pUnit->Owner != pHouse
						&& !pHouse->IsAlliedWith(pUnit->Owner) && !pUnit->Owner->IsNeutral()
						&& (pUnit->Type->Harvester || pUnit->Type->ResourceGatherer)
						&& pUnit->GetMapCoords().DistanceFrom(cell) <= radius && IsCellVisibleToAI(pHouse, pUnit->GetCell()))
					{
						count++;
					}
				}

				return count;
			};

			if (plan.Active)
			{
				auto const pAmbush = MapClass::Instance.TryGetCellAt(plan.StagingCell);
				const int value = memberValue(plan);

				if (enemyHarvestersNear(plan.StagingCell, 7.0) > 0)
					plan.AuxPeak++;

				if (!pAmbush || value * 100 < plan.InitialValue * 40)
				{
					finishPlan(NeuralPlanId::HarvesterAmbush, false, true);
				}
				else if (Unsorted::CurrentFrame >= plan.Deadline
					|| seenEnemyValueNear(plan.StagingCell, 9.0, 150) * 2 > value * 3)
				{
					// Time is up, or defenders came for us: leave while the group is still whole.
					finishPlan(NeuralPlanId::HarvesterAmbush, plan.AuxPeak > 0 && value * 100 >= plan.InitialValue * 50, true);
				}
				else
				{
					moveGroup(plan.Members, pAmbush, 3.0);
				}
			}
			else if (planReady(NeuralPlanId::HarvesterAmbush) && Unsorted::CurrentFrame >= 6000)
			{
				CellClass* pAmbush = nullptr;
				BuildingClass* pRefinery = nullptr;
				double bestDist = 1e9;

				for (auto const pBld : knownEnemyBuildingList)
				{
					if (!isKnownEnemyBuilding(pBld) || (!pBld->Type->Refinery && !pBld->Type->ResourceDestination))
						continue;

					const CellStruct rc = pBld->GetMapCoords();
					const double d = rc.DistanceFrom(homeCell);

					if (zoneOf(rc) != homeZone || d >= bestDist)
						continue;

					// Nearest resource cell 8..24 cells from the refinery, on our landmass.
					CellStruct oreCell = CellStruct::Empty;
					double oreDist = 1e9;

					for (int dy = -24; dy <= 24; dy += 2)
					{
						for (int dx = -24; dx <= 24; dx += 2)
						{
							const CellStruct c { static_cast<short>(rc.X + dx), static_cast<short>(rc.Y + dy) };
							const double od = c.DistanceFrom(rc);

							if (od < 8.0 || od > 24.0 || od >= oreDist)
								continue;

							auto const pCell = MapClass::Instance.TryGetCellAt(c);

							if (pCell && pCell->GetContainedTiberiumValue() > 0 && zoneOf(c) == homeZone)
							{
								oreCell = c;
								oreDist = od;
							}
						}
					}

					if (oreCell == CellStruct::Empty)
						continue;

					auto const pCell = landCellInZone(pointAlong(rc, oreCell, oreDist * 0.55, 0.0), homeZone, 3);

					if (!pCell || CountKnownEnemyDefensesNear(pHouse, pCell->MapCoords, 9.0, false) > 0)
						continue;

					bool nearOtherBuilding = false;

					for (auto const pOther : knownEnemyBuildingList)
					{
						if (isKnownEnemyBuilding(pOther) && pOther->GetMapCoords().DistanceFrom(pCell->MapCoords) < 6.0)
						{
							nearOtherBuilding = true;
							break;
						}
					}

					if (!nearOtherBuilding)
					{
						bestDist = d;
						pAmbush = pCell;
						pRefinery = pBld;
					}
				}

				std::vector<FootClass*> group;
				bool anyCloak = false;

				if (pAmbush)
				{
					for (auto const pFoot : collectIdleGround(false))
					{
						if (abstract_cast<UnitClass*>(pFoot))
							group.push_back(pFoot);
					}

					std::sort(group.begin(), group.end(), [](FootClass* a, FootClass* b)
						{
							auto const ta = a->GetTechnoType();
							auto const tb = b->GetTechnoType();

							if (ta->Cloakable != tb->Cloakable)
								return ta->Cloakable;

							return ta->Speed != tb->Speed ? ta->Speed > tb->Speed : a->UniqueID < b->UniqueID;
						});

					group.resize(std::min<size_t>(group.size(), 4));

					for (auto const pFoot : group)
						anyCloak = anyCloak || pFoot->GetTechnoType()->Cloakable;
				}

				const int utility = (pAmbush && group.size() >= 3) ? (280 + (anyCloak ? 80 : 0)) : -1000;

				if (utility > 0 && shouldTryStart(NeuralPlanId::HarvesterAmbush, utility))
				{
					startPlan(NeuralPlanId::HarvesterAmbush, group, pRefinery->GetMapCoords(), pAmbush->MapCoords,
						pRefinery->UniqueID, 3600);
					planOf(NeuralPlanId::HarvesterAmbush).AuxPeak = 0;
				}
			}
		}

		// ---------------------------------------------------------------- Plan 16: Pickets
		// Cheap units (sensors / cloak first) stand watch on the approaches between our base and
		// the known enemy base (45% of the way, straight and +-35 degrees), so attacks are seen
		// coming; what they see feeds the opponent model. AuxPeak = ticks with enemies spotted.
		{
			auto& plan = planOf(NeuralPlanId::Pickets);

			// A sentry that saw nothing for 3000 frames moves on to the next post of its line (45%, 30%
			// and 60% of the way): a post off every route watches nothing for the whole plan.
			auto picketPoint = [&](size_t index, int shift) -> CellClass*
			{
				// Each sentry watches its own hot enemy route (route heat), a few cells off the path, until
				// it moves on for seeing nothing there.
				const CellStruct routeWatch = shift % 2 == 0 ? RouteAmbushCell(pHouse, index, 3) : CellStruct::Empty;

				if (routeWatch != CellStruct::Empty)
				{
					if (auto const pWatch = landCellInZone(routeWatch, homeZone, 3))
						return pWatch;
				}

				// Else the first one watches the side we are really attacked from (attack route memory).
				if (index == 0 && shift % 2 == 0)
				{
					const int sector = GetMainAttackSector(pHouse);

					if (sector >= 0)
					{
						const int reach = (AttackSectorDX[sector] != 0 && AttackSectorDY[sector] != 0) ? 18 : 25;
						const CellStruct watch { static_cast<short>(homeCell.X + AttackSectorDX[sector] * reach),
							static_cast<short>(homeCell.Y + AttackSectorDY[sector] * reach) };

						if (auto const pWatch = landCellInZone(watch, homeZone, 4))
							return pWatch;
					}
				}

				if (knownEnemyCentroid == CellStruct::Empty)
					return nullptr;

				constexpr double Distances[] = { 0.45, 0.3, 0.6 };
				const double dist = homeCell.DistanceFrom(knownEnemyCentroid) * Distances[static_cast<size_t>(shift) % 3];
				constexpr double Sides[] = { 0.0, 0.7, -0.7 }; // tan(35 deg) ~= 0.7

				return landCellInZone(pointAlong(homeCell, knownEnemyCentroid, dist, dist * Sides[index % 3]), homeZone, 4);
			};

			if (plan.Active)
			{
				int alive = 0;
				// MemberFrames: per sentry the last frame it saw an enemy (or arrived), then the frame
				// until which it holds back from a stronger enemy; plan.Targets: its post shift.
				plan.MemberFrames.resize(plan.Members.size() * 2, Unsorted::CurrentFrame);
				plan.Targets.resize(plan.Members.size(), 0);

				for (size_t i = 0; i < plan.Members.size(); ++i)
				{
					auto const pFoot = findOwnFoot(plan.Members[i]);

					if (!pFoot)
						continue;

					alive++;
					const CellStruct fc = pFoot->GetMapCoords();

					if (seenEnemyValueNear(fc, 9.0, 30) > 0)
					{
						plan.AuxPeak++;
						plan.MemberFrames[i] = Unsorted::CurrentFrame;
					}

					if (IsOnRepairTrip(pHouse, plan.Members[i]))
						continue;

					// A sentry has done its job once it sees them coming: it steps back from a force that
					// would kill it (twice its value within 7 cells) and keeps watching from there.
					if (seenEnemyValueNear(fc, 7.0, 60) > GetCombatValueOfType(pFoot->GetTechnoType(), false) * 2)
						plan.MemberFrames[plan.Members.size() + i] = Unsorted::CurrentFrame + 450;

					if (Unsorted::CurrentFrame - plan.MemberFrames[i] >= 3000)
					{
						plan.Targets[i]++;
						plan.MemberFrames[i] = Unsorted::CurrentFrame;
					}

					auto const pPost = picketPoint(i, static_cast<int>(plan.Targets[i]));

					if (!pPost)
						continue;

					// Held back: 6 cells from its post toward home.
					if (Unsorted::CurrentFrame < plan.MemberFrames[plan.Members.size() + i])
					{
						if (auto const pBack = landCellInZone(pointAlong(pPost->MapCoords, homeCell, 6.0, 0.0), homeZone, 3))
							IssueMoveOrder(pFoot, pBack, 2.0);

						continue;
					}

					IssueMoveOrder(pFoot, pPost, 2.0);
				}

				if (alive == 0 || Unsorted::CurrentFrame >= plan.Deadline)
					finishPlan(NeuralPlanId::Pickets, plan.AuxPeak > 0, alive > 0);
			}
			else if (planReady(NeuralPlanId::Pickets) && Unsorted::CurrentFrame >= 3600 && knownEnemyCentroid != CellStruct::Empty
				&& homeCell.DistanceFrom(knownEnemyCentroid) >= 40.0)
			{
				std::vector<FootClass*> idle = collectIdleGround(false);
				std::vector<FootClass*> cheap;

				for (auto const pFoot : idle)
				{
					if (pFoot->GetTechnoType()->Cost <= 600 && !pFoot->Veterancy.IsElite())
						cheap.push_back(pFoot);
				}

				std::sort(cheap.begin(), cheap.end(), [](FootClass* a, FootClass* b)
					{
						auto const ta = a->GetTechnoType();
						auto const tb = b->GetTechnoType();
						const int sa = (ta->Sensors ? 2 : 0) + (ta->Cloakable ? 1 : 0);
						const int sb = (tb->Sensors ? 2 : 0) + (tb->Cloakable ? 1 : 0);

						if (sa != sb)
							return sa > sb;

						return ta->Speed != tb->Speed ? ta->Speed > tb->Speed : a->UniqueID < b->UniqueID;
					});

				cheap.resize(std::min<size_t>(cheap.size(), 3));

				// Never strip a small army for sentries.
				const int utility = (cheap.size() >= 2 && idle.size() >= 6 && picketPoint(0, 0)) ? 240 : -1000;

				if (utility > 0 && shouldTryStart(NeuralPlanId::Pickets, utility))
				{
					startPlan(NeuralPlanId::Pickets, cheap, knownEnemyCentroid, CellStruct::Empty, 0, 9000);
					planOf(NeuralPlanId::Pickets).AuxPeak = 0;

					if (RouteAmbushCell(pHouse, 0, 3) != CellStruct::Empty)
						CountDecision(NeuralDecision::RouteAmbushSpot, pHouse->ArrayIndex);
				}
			}
		}

		// ---------------------------------------------------------------- Plan 17: Key Point Hold
		// A contested resource field roughly halfway between us and the known enemy base, with
		// nobody's buildings on it yet, is held by a sturdy group (our expansion logic can then
		// claim it); they leave if a clearly stronger force comes.
		{
			auto& plan = planOf(NeuralPlanId::KeyPointHold);

			if (plan.Active)
			{
				auto const pPoint = MapClass::Instance.TryGetCellAt(plan.StagingCell);
				const int value = memberValue(plan);
				const int strength = groupStrength(plan.Members);

				// Leave when most of the group is dead or a clearly stronger force (1.6x the group)
				// comes to the point. The group walks there with a plain move: fighting everything on
				// the way cost it the strength to hold the point.
				if (pPoint && value * 100 < plan.InitialValue * 40)
				{
					abortPlan(NeuralPlanId::KeyPointHold, NeuralDecision::PlanAbortValueLost, strength);
				}
				else if (pPoint && seenEnemyValueNear(plan.StagingCell, 10.0, 150) * 10 > value * 16)
				{
					abortPlan(NeuralPlanId::KeyPointHold, NeuralDecision::PlanAbortEnemyGrew, strength);
				}
				else if (!pPoint)
				{
					finishPlan(NeuralPlanId::KeyPointHold, false, true);
				}
				else if (Unsorted::CurrentFrame >= plan.Deadline)
				{
					finishPlan(NeuralPlanId::KeyPointHold, value * 100 >= plan.InitialValue * 50, false);
				}
				else
				{
					moveGroup(plan.Members, pPoint, 4.0);
				}
			}
			else if (planReady(NeuralPlanId::KeyPointHold) && Unsorted::CurrentFrame >= 9000
				&& knownEnemyCentroid != CellStruct::Empty)
			{
				const double baseDist = homeCell.DistanceFrom(knownEnemyCentroid);
				const CellStruct mid = pointAlong(homeCell, knownEnemyCentroid, baseDist * 0.5, 0.0);
				const int radius = std::clamp(static_cast<int>(baseDist * 0.35), 6, 30);
				CellStruct bestCell = CellStruct::Empty;
				int bestScore = INT_MIN;

				for (int dy = -radius; dy <= radius; dy += 3)
				{
					for (int dx = -radius; dx <= radius; dx += 3)
					{
						const CellStruct c { static_cast<short>(mid.X + dx), static_cast<short>(mid.Y + dy) };
						auto const pCell = MapClass::Instance.TryGetCellAt(c);

						if (!pCell || pCell->GetContainedTiberiumValue() <= 0 || zoneOf(c) != homeZone)
							continue;

						const double dHome = c.DistanceFrom(homeCell);
						const double dEnemy = c.DistanceFrom(knownEnemyCentroid);

						if (std::abs(dHome - dEnemy) > baseDist * 0.3)
							continue;

						bool claimed = false;

						for (auto const pBld : BuildingClass::Array)
						{
							if (!pBld || !pBld->IsAlive || pBld->InLimbo || !pBld->Owner)
								continue;

							const double d = pBld->GetMapCoords().DistanceFrom(c);

							if ((pBld->Owner == pHouse && d < 12.0) || (isKnownEnemyBuilding(pBld) && d < 15.0))
							{
								claimed = true;
								break;
							}
						}

						if (claimed)
							continue;

						int richness = 0;

						for (int oy = -1; oy <= 1; ++oy)
						{
							for (int ox = -1; ox <= 1; ++ox)
							{
								if (auto const pN = MapClass::Instance.TryGetCellAt(CellStruct { static_cast<short>(c.X + ox), static_cast<short>(c.Y + oy) }))
									richness += pN->GetContainedTiberiumValue();
							}
						}

						// A field on a hot enemy route draws their army onto the holders (route heat).
						const int score = richness - static_cast<int>(std::abs(dHome - dEnemy) * 10.0)
							- std::min(RouteHeatNear(pHouse, c, 1), 3000) / 2;

						if (score > bestScore)
						{
							bestScore = score;
							bestCell = c;
						}
					}
				}

				std::vector<FootClass*> group;
				CellClass* pPoint = nullptr;

				if (bestCell != CellStruct::Empty)
				{
					pPoint = landCellInZone(bestCell, homeZone, 3);
					group = collectIdleGround(false);

					if (group.size() >= 8)
					{
						// The six best fighters (combat value, not hit points: the toughest units are often
						// support vehicles that hold nothing).
						std::sort(group.begin(), group.end(), [](FootClass* a, FootClass* b)
							{
								const int sa = GetCombatValueOfType(a->GetTechnoType(), false);
								const int sb = GetCombatValueOfType(b->GetTechnoType(), false);
								return sa != sb ? sa > sb : a->UniqueID < b->UniqueID;
							});

						group.resize(6);
					}
					else
					{
						group.clear();
					}
				}

				// Never a point the enemy army walks through or covers with defenses (enemies seen there
				// in the last 900 frames worth more than 60% of the group, or a known defense within 10
				// cells): the group would meet a stronger force there and fall back.
				if (!group.empty())
				{
					int groupStrengthNow = 0;

					for (auto const pFoot : group)
						groupStrengthNow += static_cast<int>(GetCombatValueOfType(pFoot->GetTechnoType(), false) * pFoot->GetHealthPercentage());

					if (seenEnemyValueNear(bestCell, 14.0, 900) * 10 > groupStrengthNow * 6
						|| CountKnownEnemyDefensesNear(pHouse, bestCell, 10.0, false) > 0)
					{
						group.clear();
					}
				}

				const int utility = (pPoint && !group.empty()) ? 260 : -1000;

				if (utility > 0 && shouldTryStart(NeuralPlanId::KeyPointHold, utility))
					startPlan(NeuralPlanId::KeyPointHold, group, bestCell, pPoint->MapCoords, 0, 5400);
			}
		}

		// ---------------------------------------------------------------- Plan 18: Survival
		// No ConYard and no MCV left. With a war factory but not enough money for a new MCV, sell
		// non-essential buildings (one per 150 frames) until it is affordable (AuxBaseline 0);
		// with no way to rebuild at all, the remaining army goes out fighting (AuxBaseline 1).
		{
			auto& plan = planOf(NeuralPlanId::Survival);
			bool ownsMCV = false;

			for (auto const pUnit : UnitClass::Array)
			{
				if (pUnit && pUnit->Owner == pHouse && pUnit->IsAlive && pUnit->Type && pUnit->Type->DeploysInto
					&& pUnit->Type->DeploysInto->ConstructionYard)
				{
					ownsMCV = true;
					break;
				}
			}

			// An MCV already being built counts too (its cost is already being paid).
			if (FactoryClass* const pUnitFactory = pHouse->GetPrimaryFactory(AbstractType::UnitType, false, BuildCat::DontCare))
			{
				auto const pBuilding = abstract_cast<UnitClass*>(pUnitFactory->Object);

				if (pBuilding && pBuilding->Type && pBuilding->Type->DeploysInto && pBuilding->Type->DeploysInto->ConstructionYard)
					ownsMCV = true;
			}

			int mcvCost = 0;

			for (auto const pUnitType : UnitTypeClass::Array)
			{
				if (pUnitType && pUnitType->DeploysInto && pUnitType->DeploysInto->ConstructionYard
					&& CanBuildUnderHumanRules(pHouse, pUnitType))
				{
					const int cost = pUnitType->GetActualCost(pHouse);

					if (cost > 0 && (mcvCost == 0 || cost < mcvCost))
						mcvCost = cost;
				}
			}

			const bool hasWarFactory = pHouse->NumWarFactories > 0;
			const int money = pHouse->Available_Money();

			auto nearestKnownEnemyBuilding = [&](const CellStruct& from) -> BuildingClass*
			{
				BuildingClass* pBest = nullptr;
				double bestDist = 1e9;

				for (auto const pBld : knownEnemyBuildingList)
				{
					if (!isKnownEnemyBuilding(pBld))
						continue;

					const double d = pBld->GetMapCoords().DistanceFrom(from);

					if (d < bestDist)
					{
						bestDist = d;
						pBest = pBld;
					}
				}

				return pBest;
			};

			if (plan.Active)
			{
				if (pHouse->NumConYards > 0 || ownsMCV)
				{
					finishPlan(NeuralPlanId::Survival, true, false);
				}
				else if (Unsorted::CurrentFrame >= plan.Deadline)
				{
					finishPlan(NeuralPlanId::Survival, false, false);
				}
				else if (plan.AuxBaseline == 0)
				{
					if (mcvCost > 0 && money >= mcvCost)
					{
						finishPlan(NeuralPlanId::Survival, true, false);
					}
					else if (Unsorted::CurrentFrame >= macro.SurvivalNextSellFrame)
					{
						// Most refund first among buildings we can live without.
						BuildingClass* pSell = nullptr;

						for (auto const pBld : pHouse->Buildings)
						{
							if (!pBld || pBld->InLimbo || !pBld->IsAlive || !pBld->Type || pBld->Type->Unsellable
								|| pBld->Type->Insignificant || pBld->Type->InvisibleInGame || pBld->Type->ConstructionYard
								|| (pBld->Type->Factory == AbstractType::UnitType && !pBld->Type->Naval) || pBld->Type->Refinery
								|| pBld->Type->ResourceDestination || pBld->Type->Cost < 300 || IsMCVPrerequisiteType(pHouse, pBld->Type)
								|| (pBld->Type->PowerBonus > 0 && pHouse->PowerOutput - pBld->Type->PowerBonus < pHouse->PowerDrain))
							{
								continue;
							}

							auto better = [](BuildingClass* a, BuildingClass* b)
							{
								if (a->Type->IsBaseDefense != b->Type->IsBaseDefense)
									return !a->Type->IsBaseDefense;

								return a->Type->Cost != b->Type->Cost ? a->Type->Cost > b->Type->Cost : a->UniqueID < b->UniqueID;
							};

							if (!pSell || better(pBld, pSell))
								pSell = pBld;
						}

						if (pSell)
						{
							pSell->Sell(1);
							macro.SurvivalNextSellFrame = Unsorted::CurrentFrame + 150;
						}
						else
						{
							finishPlan(NeuralPlanId::Survival, false, false);
						}
					}
				}
				else if (hasWarFactory)
				{
					finishPlan(NeuralPlanId::Survival, true, false);
				}
				else
				{
					for (const DWORD uid : plan.Members)
					{
						auto const pFoot = findOwnFoot(uid);

						if (!pFoot || pFoot->Target != nullptr || pFoot->MegaMission == Mission::AttackMove)
							continue;

						if (auto const pTarget = nearestKnownEnemyBuilding(pFoot->GetMapCoords()))
							OrderFootToAttackMove(pFoot, pTarget->GetCell());
					}
				}
			}
			else if (planReady(NeuralPlanId::Survival) && pHouse->NumConYards == 0 && !ownsMCV)
			{
				if (hasWarFactory && mcvCost > 0 && money < mcvCost)
				{
					if (shouldTryStart(NeuralPlanId::Survival, 600))
					{
						startPlan(NeuralPlanId::Survival, std::vector<FootClass*> {}, homeCell, CellStruct::Empty, 0, 2700);
						planOf(NeuralPlanId::Survival).AuxBaseline = 0;
					}
				}
				else if (!hasWarFactory && nearestKnownEnemyBuilding(homeCell))
				{
					std::vector<FootClass*> army;

					for (auto const& [uid, pFoot] : ownFoots)
					{
						if (isArmedGroundFoot(pFoot) && !macro.PlanMembers.contains(uid))
							army.push_back(pFoot);
					}

					std::sort(army.begin(), army.end(), [](FootClass* a, FootClass* b) { return a->UniqueID < b->UniqueID; });

					if (!army.empty() && shouldTryStart(NeuralPlanId::Survival, 500))
					{
						startPlan(NeuralPlanId::Survival, army, homeCell, CellStruct::Empty, 0, 9000);
						planOf(NeuralPlanId::Survival).AuxBaseline = 1;
					}
				}
			}
		}

		// ---------------------------------------------------------------- Plan 19: Preemptive Strike
		// An enemy force is seen gathering outside its defenses, standing and not yet at our base
		// (findPreemptiveCase): the free units nearest to it meet PreemptiveStageCells short of it
		// (StagingCell) and strike it together (Progress 1) before it is complete. AuxBaseline = its
		// value at the start, AnchorCell = where it was gathering.
		{
			auto& plan = planOf(NeuralPlanId::PreemptiveStrike);

			if (plan.Active)
			{
				const CellStruct current = seenGroundEnemyCentroidNear(plan.TargetCell, 10.0, 150);
				double nearestToForce = 1.0e9;

				if (current != CellStruct::Empty)
				{
					for (const DWORD uid : plan.Members)
					{
						if (auto const pMember = findOwnFoot(uid))
							nearestToForce = std::min(nearestToForce, pMember->GetMapCoords().DistanceFrom(current));
					}
				}

				// The plan follows the force PreemptiveLeashCells from where it gathered and no
				// farther, unless it came at the group: a force that has set out elsewhere is no longer
				// a gathering, and the group that followed it ended across the map (37 of 102 plans ran
				// out of time 27 to 107 cells from a target that kept moving, 50 more were beaten on
				// the way, 2026-11-14).
				const bool engaged = nearestToForce <= PreemptiveEngageCells;
				const bool forceLeft = current != CellStruct::Empty && plan.AnchorCell != CellStruct::Empty
					&& current.DistanceFrom(plan.AnchorCell) > PreemptiveLeashCells && !engaged;

				if (current != CellStruct::Empty && !forceLeft)
					plan.TargetCell = current;

				// Arrived with half of the group at the force: the centre of a group strung out
				// along its way stays behind while its head is already fighting.
				int membersAlive = 0;
				int membersThere = 0;
				int membersStaged = 0;

				for (const DWORD uid : plan.Members)
				{
					if (auto const pMember = findOwnFoot(uid))
					{
						membersAlive++;
						const CellStruct cell = pMember->GetMapCoords();

						if (cell.DistanceFrom(plan.TargetCell) <= 8.0)
							membersThere++;

						if (plan.StagingCell != CellStruct::Empty && cell.DistanceFrom(plan.StagingCell) <= PreemptiveStageNearCells)
							membersStaged++;
					}
				}

				if (!plan.Arrived && membersThere > 0 && membersThere * 2 >= membersAlive)
				{
					plan.Arrived = true;
					plan.ArrivedFrame = Unsorted::CurrentFrame;
				}

				// Strike together: once two thirds of the group met at the staging spot, after
				// PreemptiveStageWaitFrames, or at once when the fight has begun.
				if (plan.Progress == 0 && (plan.StagingCell == CellStruct::Empty || membersStaged * 3 >= membersAlive * 2
					|| Unsorted::CurrentFrame - plan.StartFrame >= PreemptiveStageWaitFrames || engaged))
				{
					plan.Progress = 1;
				}

				if (memberValue(plan) * 100 < plan.InitialValue * 40 || Unsorted::CurrentFrame >= plan.Deadline)
					finishPlan(NeuralPlanId::PreemptiveStrike, false, true);
				else if (forceLeft && !plan.Arrived)
					finishPlan(NeuralPlanId::PreemptiveStrike, false, true); // It left before the group got there.
				else if (plan.Arrived && seenGroundEnemyValueNear(plan.TargetCell, 10.0, 150) * 10 <= plan.AuxBaseline * 4)
					finishPlan(NeuralPlanId::PreemptiveStrike, true, true);
				else if (plan.Progress == 0)
				{
					auto const pStage = MapClass::Instance.TryGetCellAt(plan.StagingCell);

					for (const DWORD uid : plan.Members)
					{
						auto const pFoot = findOrderableFoot(uid);

						// A member already in a fight finishes it there.
						if (!pFoot || !pStage || pFoot->Target)
							continue;

						// A member taken from a marching wave still has the wave's attack-move: it would walk
						// on past the meeting spot and miss the strike order (attackMoveGroup leaves units on
						// an attack-move alone).
						if (pFoot->MegaMission == Mission::AttackMove)
							pFoot->ClearMegaMissionData();

						IssueMoveOrder(pFoot, pStage, 3.0);
					}
				}
				else
				{
					attackMoveGroup(plan.Members, MapClass::Instance.TryGetCellAt(plan.TargetCell));
				}
			}
			else if (planReady(NeuralPlanId::PreemptiveStrike))
			{
				auto const strike = findPreemptiveCase();
				const int utility = !strike.Group.empty() ? 320 : -1000;

				if (utility > 0 && shouldTryStart(NeuralPlanId::PreemptiveStrike, utility))
				{
					// The meeting spot: PreemptiveStageCells short of the force, on the side the group
					// comes from.
					int sumX = 0;
					int sumY = 0;

					for (auto const pFoot : strike.Group)
					{
						const CellStruct cell = pFoot->GetMapCoords();
						sumX += cell.X;
						sumY += cell.Y;
					}

					const int count = static_cast<int>(strike.Group.size());
					const CellStruct groupCenter { static_cast<short>(sumX / count), static_cast<short>(sumY / count) };
					auto const pStage = landCellInZone(pointAlong(strike.Center, groupCenter, PreemptiveStageCells, 0.0), homeZone, 3);

					startPlan(NeuralPlanId::PreemptiveStrike, strike.Group, strike.Center, pStage ? pStage->MapCoords : CellStruct::Empty, 0, 2400);
					auto& started = planOf(NeuralPlanId::PreemptiveStrike);

					if (started.Active)
					{
						started.AuxBaseline = strike.Value;
						started.AnchorCell = strike.Center;
					}
				}
			}
		}

		// ---------------------------------------------------------------- Plan 44: Counter Squad
		// An enemy house gathers CounterSquadForceUnits or more units of one force category: our free
		// units that counter that category go at the largest such force we know of together, instead
		// of being spread through the waves (Counters only shifts what we build). A fixed rule with no
		// learned gate. A land squad meets short of the force (StagingCell) and strikes together
		// (Progress 1); ships strike at once. AuxBaseline = value of the force at the start, AnchorCell =
		// where it was then, ForceOwner and ForceCategory = whose force and of what.
		{
			auto& plan = planOf(NeuralPlanId::CounterSquad);
			static constexpr const char* forceNames[] = { "infantry", "vehicles", "artillery", "ships", "aircraft" };
			const int now = Unsorted::CurrentFrame;

			auto forceName = [](int category) -> const char*
			{
				return category >= 0 && category < static_cast<int>(std::size(forceNames)) ? forceNames[category] : "units";
			};

			// The units of one house and force category seen within radius of a cell no longer than
			// maxAge ago: their value and value-weighted centroid.
			auto forceNear = [&](int owner, int category, const CellStruct& cell, double radius, int maxAge, CellStruct* pCentroid) -> int
			{
				long long sumX = 0;
				long long sumY = 0;
				int value = 0;

				for (auto const& [uid, sighting] : macro.EnemySightings)
				{
					if (sighting.OwnerIndex != owner || static_cast<int>(sighting.Category) != category
						|| now - sighting.LastSeenFrame > maxAge || sighting.LastCoords.DistanceFrom(cell) > radius)
					{
						continue;
					}

					const int w = std::max(1, sighting.Value);
					sumX += static_cast<long long>(sighting.LastCoords.X) * w;
					sumY += static_cast<long long>(sighting.LastCoords.Y) * w;
					value += w;
				}

				if (pCentroid)
				{
					*pCentroid = value > 0 ? CellStruct { static_cast<short>(sumX / value), static_cast<short>(sumY / value) }
						: CellStruct::Empty;
				}

				return value;
			};

			auto endSquad = [&](bool success, const char* why, int rule = __builtin_LINE())
			{
				Debug::Log("[NeuralAI] House %d plan counterSquad ended %s (%s): %s of house %d worth %d of %d left near (%d,%d), "
					"kills %d, squad %d of %d, frame %d.\n", houseIdx, success ? "well" : "badly", why, forceName(plan.ForceCategory),
					plan.ForceOwner, forceNear(plan.ForceOwner, plan.ForceCategory, plan.TargetCell, CounterSquadClusterCells,
						CounterSquadTrackFrames, nullptr), plan.AuxBaseline, plan.TargetCell.X, plan.TargetCell.Y, plan.KillValue,
					memberValue(plan), plan.InitialValue, now);
				finishPlan(NeuralPlanId::CounterSquad, success, !success, rule);
			};

			if (plan.Active)
			{
				CellStruct current = CellStruct::Empty;
				forceNear(plan.ForceOwner, plan.ForceCategory, plan.TargetCell, CounterSquadClusterCells, CounterSquadTrackFrames, &current);

				if (current != CellStruct::Empty)
				{
					plan.TargetCell = current;
					plan.TargetSeenFrame = now;
				}

				int membersAlive = 0;
				int membersThere = 0;
				int membersStaged = 0;
				double nearestToForce = 1.0e9;

				for (const DWORD uid : plan.Members)
				{
					if (auto const pMember = findOwnFoot(uid))
					{
						membersAlive++;
						const CellStruct cell = pMember->GetMapCoords();
						const double distance = cell.DistanceFrom(plan.TargetCell);
						nearestToForce = std::min(nearestToForce, distance);

						if (distance <= CounterSquadEngageCells)
							membersThere++;

						if (plan.StagingCell != CellStruct::Empty && cell.DistanceFrom(plan.StagingCell) <= CounterSquadStageNearCells)
							membersStaged++;
					}
				}

				const bool engaged = current != CellStruct::Empty && nearestToForce <= CounterSquadEngageCells;

				if (!plan.Arrived && membersThere > 0 && membersThere * 2 >= membersAlive)
				{
					plan.Arrived = true;
					plan.ArrivedFrame = now;
				}

				if (plan.Progress == 0 && (plan.StagingCell == CellStruct::Empty || membersStaged * 3 >= membersAlive * 2
					|| now - plan.StartFrame >= CounterSquadStageWaitFrames || engaged))
				{
					plan.Progress = 1;
				}

				const int forceLeft = forceNear(plan.ForceOwner, plan.ForceCategory, plan.TargetCell, CounterSquadClusterCells,
					CounterSquadTrackFrames, nullptr);

				if (plan.KillValue * 100 >= plan.AuxBaseline * CounterSquadKillPercent)
				{
					endSquad(true, "kills");
				}
				else if (plan.Arrived && forceLeft * 100 <= plan.AuxBaseline * CounterSquadBrokenPercent)
				{
					endSquad(true, "force broken");
				}
				else if (memberValue(plan) * 100 < plan.InitialValue * CounterSquadKeptPercent)
				{
					endSquad(false, "squad lost");
				}
				else if (now - plan.TargetSeenFrame >= CounterSquadLostFrames)
				{
					endSquad(false, "force lost from sight");
				}
				else if (now >= plan.Deadline)
				{
					endSquad(false, "time");
				}
				else if (plan.Progress == 0)
				{
					auto const pStage = MapClass::Instance.TryGetCellAt(plan.StagingCell);

					for (const DWORD uid : plan.Members)
					{
						auto const pFoot = findOrderableFoot(uid);

						// A member already in a fight finishes it there.
						if (!pFoot || !pStage || pFoot->Target)
							continue;

						// A member taken from a marching wave still has the wave's attack-move, which would take
						// it past the meeting spot.
						if (pFoot->MegaMission == Mission::AttackMove)
							pFoot->ClearMegaMissionData();

						IssueMoveOrder(pFoot, pStage, 3.0);
					}
				}
				else if (auto const pForceCell = MapClass::Instance.TryGetCellAt(plan.TargetCell))
				{
					for (const DWORD uid : plan.Members)
					{
						auto const pFoot = findOrderableFoot(uid);

						if (!pFoot || pFoot->Target)
							continue;

						// Sent again only when the force moved away from where the member is going.
						if (pFoot->MegaMission == Mission::AttackMove)
						{
							auto const pGoal = abstract_cast<CellClass*>(pFoot->MegaDestination);

							if (pGoal && pGoal->MapCoords.DistanceFrom(plan.TargetCell) <= CounterSquadReaimCells)
								continue;

							pFoot->ClearMegaMissionData();
						}

						OrderFootToAttackMove(pFoot, pForceCell);
					}
				}
			}
			else if (planReady(NeuralPlanId::CounterSquad))
			{
				macro.PlanNextAttemptFrame[static_cast<size_t>(NeuralPlanId::CounterSquad)] = now + CounterSquadRetryFrames;

				auto enemyOwner = [&](int owner) -> bool
				{
					if (owner < 0 || owner >= HouseClass::Array.Count || owner == houseIdx || IsTacitTrucePartner(pHouse, owner))
						return false;

					auto const pOwner = HouseClass::Array.GetItem(owner);

					return pOwner && !pOwner->Defeated && !pOwner->IsNeutral() && !pHouse->IsAlliedWith(pOwner);
				};

				// The fresh sightings in UniqueID order, so every pick below is the same on every client.
				std::vector<std::pair<DWORD, const EnemyUnitSighting*>> fresh;

				for (auto const& [uid, sighting] : macro.EnemySightings)
				{
					if (sighting.Type && sighting.Value > 0 && now - sighting.LastSeenFrame <= CounterSquadSeenFrames
						&& enemyOwner(sighting.OwnerIndex))
					{
						fresh.emplace_back(uid, &sighting);
					}
				}

				std::sort(fresh.begin(), fresh.end(), [](auto const& a, auto const& b) { return a.first < b.first; });

				struct ForceSeed
				{
					DWORD ID;
					int Owner;
					int Category;
					int Units;
					int Value;
					CellStruct Cell;
				};

				// Every gathering of one house and category around a seed sighting, largest first; a seed
				// within CounterSquadClusterCells of a larger gathering of the same house and category is
				// that gathering.
				std::vector<ForceSeed> seeds;

				for (auto const& [seedID, pSeed] : fresh)
				{
					int units = 0;
					int value = 0;

					for (auto const& [otherID, pOther] : fresh)
					{
						if (pOther->OwnerIndex == pSeed->OwnerIndex && pOther->Category == pSeed->Category
							&& pOther->LastCoords.DistanceFrom(pSeed->LastCoords) <= CounterSquadClusterCells)
						{
							units++;
							value += pOther->Value;
						}
					}

					if (units >= CounterSquadForceUnits)
						seeds.push_back({ seedID, pSeed->OwnerIndex, static_cast<int>(pSeed->Category), units, value, pSeed->LastCoords });
				}

				std::sort(seeds.begin(), seeds.end(), [](const ForceSeed& a, const ForceSeed& b)
					{
						return a.Units != b.Units ? a.Units > b.Units : a.Value != b.Value ? a.Value > b.Value : a.ID < b.ID;
					});

				std::vector<ForceSeed> forces;

				for (auto const& seed : seeds)
				{
					const bool known = std::any_of(forces.begin(), forces.end(), [&seed](const ForceSeed& force)
						{
							return force.Owner == seed.Owner && force.Category == seed.Category
								&& force.Cell.DistanceFrom(seed.Cell) <= CounterSquadClusterCells;
						});

					if (!known)
						forces.push_back(seed);
				}

				if (!forces.empty())
					CountPlanOutcome(houseIdx, NeuralPlanId::CounterSquad, NeuralPlanOutcome::Eligible);

				bool started = false;

				for (auto const& force : forces)
				{
					CellStruct center = CellStruct::Empty;
					forceNear(force.Owner, force.Category, force.Cell, CounterSquadClusterCells, CounterSquadSeenFrames, &center);
					auto const pCenterCell = center != CellStruct::Empty ? MapClass::Instance.TryGetCellAt(center) : nullptr;

					if (!pCenterCell)
						continue;

					// Land squads go at a force on our landmass, ships at a force on the water of their sea.
					const bool atSea = pCenterCell->LandType == LandType::Water;

					if (!atSea && zoneOf(center) != homeZone)
						continue;

					int enemyThere = seenEnemyValueNear(center, CounterSquadClusterCells, CounterSquadSeenFrames);

					for (auto const pBld : knownEnemyBuildingList)
					{
						if (isKnownEnemyBuilding(pBld) && pBld->Type->IsBaseDefense
							&& pBld->GetMapCoords().DistanceFrom(center) <= CounterSquadClusterCells)
						{
							enemyThere += GetCombatValueOfType(pBld->Type, false);
						}
					}

					const NeuralCounterView view = BuildForceCounterView(pHouse, force.Owner, static_cast<EnemyForceCategory>(force.Category),
						center, CounterSquadClusterCells, CounterSquadSeenFrames);

					if (view.TotalValue <= 0)
						continue;

					std::unordered_map<const TechnoTypeClass*, int> fitOf;

					auto fitFor = [&](TechnoTypeClass* pType) -> int
					{
						if (auto const it = fitOf.find(pType); it != fitOf.end())
							return it->second;

						const int fit = CounterQueueOf(pType) >= 0 ? CounterFitOf(pType, view) : 0;
						fitOf[pType] = fit;
						return fit;
					};

					// Our whole army against this force, value-weighted: a counter is clearly better than that.
					long long armyFitSum = 0;
					long long armyValue = 0;

					for (auto const& [uid, pFoot] : ownFoots)
					{
						auto const pType = pFoot->GetTechnoType();

						if (!pType || CounterQueueOf(pType) < 0)
							continue;

						const int value = GetCombatValueOfType(pType, false);
						armyFitSum += static_cast<long long>(fitFor(pType)) * value;
						armyValue += value;
					}

					const int armyFit = armyValue > 0 ? static_cast<int>(armyFitSum / armyValue) : 0;
					const int fitNeeded = std::max(CounterSquadFitMin, armyFit + CounterSquadFitEdge);
					std::vector<FootClass*> pool;

					if (atSea)
					{
						for (auto const& [uid, pFoot] : ownFoots)
						{
							auto const pType = pFoot->GetTechnoType();

							if (pType && pType->Naval && pFoot->WhatAmI() != AbstractType::Aircraft && !IsAmphibiousOrAirTransport(pType)
								&& isFreeForPlan(pFoot)
								&& ObjectZone(pFoot, pType->MovementZone) == MapClass::Instance.GetMovementZoneType(center, pType->MovementZone, false))
							{
								pool.push_back(pFoot);
							}
						}
					}
					else
					{
						pool = collectIdleGround(force.Category == static_cast<int>(EnemyForceCategory::Air));
					}

					struct Counter
					{
						FootClass* Foot;
						int Fit;
						int Distance;
					};

					std::vector<Counter> counters;

					for (auto const pFoot : pool)
					{
						const int fit = fitFor(pFoot->GetTechnoType());

						if (fit >= fitNeeded)
							counters.push_back({ pFoot, fit, static_cast<int>(pFoot->GetMapCoords().DistanceFrom(center)) });
					}

					std::sort(counters.begin(), counters.end(), [](const Counter& a, const Counter& b)
						{
							return a.Fit != b.Fit ? a.Fit > b.Fit
								: a.Distance != b.Distance ? a.Distance < b.Distance : a.Foot->UniqueID < b.Foot->UniqueID;
						});

					// Unless the force is at our own buildings, CounterSquadHomeKeepPercent of the free value
					// at home stays there, as with the base threat intercept.
					bool atOwnBuildings = center.DistanceFrom(homeCell) <= CounterSquadHomeCells;

					for (auto const pBld : pHouse->Buildings)
					{
						if (atOwnBuildings)
							break;

						if (pBld && pBld->IsAlive && !pBld->InLimbo && pBld->GetMapCoords().DistanceFrom(center) <= CounterSquadOwnBuildingCells)
							atOwnBuildings = true;
					}

					int homeValue = 0;

					for (auto const pFoot : pool)
					{
						if (pFoot->GetMapCoords().DistanceFrom(homeCell) <= CounterSquadHomeCells)
							homeValue += GetCombatValueOfType(pFoot->GetTechnoType(), false);
					}

					const int maxFromHome = atOwnBuildings ? INT_MAX : homeValue * (100 - CounterSquadHomeKeepPercent) / 100;
					std::vector<FootClass*> squad;
					int weighted = 0;
					int plain = 0;
					int fromHome = 0;

					for (auto const& counter : counters)
					{
						if (squad.size() >= CounterSquadMaxMembers || weighted * 100 >= enemyThere * CounterSquadEnoughPercent)
							break;

						if (counter.Foot->GetMapCoords().DistanceFrom(homeCell) <= CounterSquadHomeCells)
						{
							const int homeShare = GetCombatValueOfType(counter.Foot->GetTechnoType(), false);

							if (fromHome + homeShare > maxFromHome)
								continue;

							fromHome += homeShare;
						}

						const int value = static_cast<int>(GetCombatValueOfType(counter.Foot->GetTechnoType(), false)
							* counter.Foot->GetHealthPercentage());
						squad.push_back(counter.Foot);
						weighted += value * counter.Fit / 100;
						plain += value;
					}

					if (squad.size() < CounterSquadMinMembers || weighted * 100 < enemyThere * CounterSquadAdvantagePercent
						|| plain * PlanOutgunnedRatioPercent < enemyThere * 100)
					{
						continue;
					}

					// The meeting spot of a land squad: CounterSquadStageCells short of the force, on the side
					// the squad comes from, unless the squad is about that near already.
					CellStruct staging = CellStruct::Empty;

					if (!atSea)
					{
						int sumX = 0;
						int sumY = 0;

						for (auto const pFoot : squad)
						{
							const CellStruct cell = pFoot->GetMapCoords();
							sumX += cell.X;
							sumY += cell.Y;
						}

						const int count = static_cast<int>(squad.size());
						const CellStruct squadCenter { static_cast<short>(sumX / count), static_cast<short>(sumY / count) };

						if (squadCenter.DistanceFrom(center) > CounterSquadStageCells + CounterSquadStageNearCells)
						{
							if (auto const pStage = landCellInZone(pointAlong(center, squadCenter, CounterSquadStageCells, 0.0), homeZone, 3))
								staging = pStage->MapCoords;
						}
					}

					startPlan(NeuralPlanId::CounterSquad, squad, center, staging, 0, CounterSquadDurationFrames);

					if (plan.Active)
					{
						plan.AuxBaseline = std::max(1, view.TotalValue);
						plan.AnchorCell = center;
						plan.ForceOwner = force.Owner;
						plan.ForceCategory = force.Category;
						started = true;
						Debug::Log("[NeuralAI] House %d plan counterSquad started: %d %s of house %d at (%d,%d) worth %d, %d there; "
							"%d counters worth %d by fit and %d plain (fit %d+, army %d), %s, staging (%d,%d), frame %d.\n", houseIdx,
							force.Units, forceName(force.Category), force.Owner, center.X, center.Y, view.TotalValue, enemyThere,
							static_cast<int>(squad.size()), weighted, plain, fitNeeded, armyFit, atSea ? "ships" : "land", staging.X,
							staging.Y, now);
					}

					// Started, or held by the start checks (they set the next attempt).
					break;
				}

				if (!started && !forces.empty() && !plan.Active)
					CountPlanOutcome(houseIdx, NeuralPlanId::CounterSquad, NeuralPlanOutcome::NoGroup);
			}
		}

		// ---------------------------------------------------------------- Plan 20: Bait Ambush
		// The ambush group waits on guard BaitAmbushCells from a known enemy building on the way home,
		// out of reach of the defenses known there; the fastest unit (Members[0]) fires at the building
		// and runs back through the ambush once it is hit or enemies come near it. A Neural AI house
		// answers whoever fires at its buildings with an intercept group worth 1.3x the attacker that
		// follows it until it is 8 cells farther from the building than when the intercept began, so
		// the answer runs into an ambush at 12 cells, never into one 23 cells or more away. The old bait
		// walked to a cell 8 cells short of the building without firing, which no house takes for a
		// threat: 25 of 36 plans drew no intercept at all and 1 went well (40 matches, 2026-11-15).
		// Success = enemy units worth BaitKillValue destroyed by the members, no less than the value
		// the plan lost. AuxBaseline = phase (0 set up, 1 lure, 2 run back, 3 sprung), AuxPeak = peak
		// enemy value seen at the ambush, ArrivedFrame = start of the current wait, Arrived = the bait
		// came within range of the building.
		{
			auto& plan = planOf(NeuralPlanId::BaitAmbush);

			auto endBait = [&](bool success, int rule = __builtin_LINE())
			{
				finishPlan(NeuralPlanId::BaitAmbush, success, true, rule);
			};

			auto ambushPaid = [&]()
			{
				return plan.KillValue >= BaitKillValue && plan.KillValue >= plan.InitialValue - memberValue(plan);
			};

			if (plan.Active)
			{
				FootClass* const pBait = plan.Members.empty() ? nullptr : findOwnFoot(plan.Members[0]);
				const std::vector<DWORD> ambushers(plan.Members.empty() ? plan.Members.end() : plan.Members.begin() + 1, plan.Members.end());
				auto const pAmbush = MapClass::Instance.TryGetCellAt(plan.StagingCell);
				auto const pLure = FindBuildingByID(plan.TargetID);
				const bool lureStands = pLure && pLure->IsAlive && !pLure->InLimbo;
				const int enemyAtAmbush = seenEnemyValueNear(plan.StagingCell, BaitAmbushSightCells, 150);
				const int ambushValue = groupValue(ambushers);
				plan.AuxPeak = std::max(plan.AuxPeak, enemyAtAmbush);

				if (!pAmbush || ambushValue * 100 < plan.InitialValue * 35)
				{
					endBait(false);
				}
				else if (Unsorted::CurrentFrame >= plan.Deadline)
				{
					endBait(ambushPaid());
				}
				else if (plan.AuxBaseline == 0)
				{
					// The ambush takes its place first. The bait, the fastest of the group, keeps to the
					// middle of the ambushers on the way: sent to the ambush cell at once, it got there
					// alone and died in 24 of 65 plans (focus batch of 2026-11-15, lures 60 to 350 cells
					// from home).
					moveGroup(ambushers, pAmbush, 3.0);
					int inPlace = 0;
					int ambushAlive = 0;

					for (const DWORD uid : ambushers)
					{
						if (auto const pFoot = findOwnFoot(uid))
						{
							ambushAlive++;

							if (pFoot->GetMapCoords().DistanceFrom(plan.StagingCell) <= BaitInPlaceCells)
								inPlace++;
						}
					}

					if (!pBait || !lureStands)
					{
						endBait(false);
					}
					else if (inPlace * 2 >= ambushAlive || Unsorted::CurrentFrame - plan.StartFrame >= BaitSetFrames)
					{
						plan.AuxBaseline = 1;
						plan.ArrivedFrame = Unsorted::CurrentFrame;
						plan.LureHealth = static_cast<int>(pBait->GetHealthPercentage() * 100);
					}
					else
					{
						const CellStruct middle = groupCentroid(ambushers);
						auto const pWith = middle != CellStruct::Empty ? landCellInZone(middle, homeZone, 3) : nullptr;
						IssueMoveOrder(pBait, pWith ? pWith : pAmbush, 3.0);
					}
				}
				else if (plan.AuxBaseline == 1)
				{
					moveGroup(ambushers, pAmbush, 3.0);
					const bool hurt = !pBait || static_cast<int>(pBait->GetHealthPercentage() * 100) <= plan.LureHealth - BaitHurtPercent;
					const bool answered = pBait && seenEnemyValueNear(pBait->GetMapCoords(), BaitThreatCells, 60) >= BaitThreatValue;

					if (hurt || answered || !lureStands)
					{
						plan.AuxBaseline = 2;
						plan.ArrivedFrame = Unsorted::CurrentFrame;
					}
					else
					{
						// Fire at the building: only a unit with orders against a building is a threat the
						// house answers.
						if (pBait->Target != pLure)
						{
							pBait->SetTarget(pLure);
							pBait->QueueMission(Mission::Attack, true);
						}

						const bool inRange = pBait->GetMapCoords().DistanceFrom(plan.TargetCell) * Unsorted::LeptonsPerCell
							<= GroundRangeOf(pBait->GetTechnoType()) + 2 * Unsorted::LeptonsPerCell;

						if (!plan.Arrived && inRange)
						{
							plan.Arrived = true;
							plan.ArrivedFrame = Unsorted::CurrentFrame;
						}
						else if (plan.Arrived ? Unsorted::CurrentFrame - plan.ArrivedFrame >= BaitLureWaitFrames
							: Unsorted::CurrentFrame - plan.ArrivedFrame >= BaitApproachFrames)
						{
							// It fired and nobody came out, or it never got within range.
							endBait(false);
							macro.PlanNextAttemptFrame[static_cast<size_t>(NeuralPlanId::BaitAmbush)] = Unsorted::CurrentFrame + BaitRetryFrames;
						}
					}
				}
				else if (plan.AuxBaseline == 2)
				{
					moveGroup(ambushers, pAmbush, 3.0);
					bool baitBack = true;

					if (pBait)
					{
						if (auto const pBehind = landCellInZone(pointAlong(plan.StagingCell, homeCell, 4.0, 0.0), homeZone, 3))
						{
							if (pBait->Target)
								pBait->SetTarget(nullptr);

							IssueMoveOrder(pBait, pBehind, 2.0);
							baitBack = pBait->GetMapCoords().DistanceFrom(pBehind->MapCoords) <= 5.0;
						}
					}

					// Whoever chases the bait is on its heels: the wait for them starts when the bait is
					// back behind the ambush and lasts BaitChaseWaitFrames.
					if (!baitBack)
						plan.ArrivedFrame = Unsorted::CurrentFrame;

					if (enemyAtAmbush >= BaitThreatValue)
					{
						plan.AuxBaseline = 3;
					}
					else if (Unsorted::CurrentFrame - plan.ArrivedFrame >= BaitChaseWaitFrames)
					{
						endBait(ambushPaid());
						macro.PlanNextAttemptFrame[static_cast<size_t>(NeuralPlanId::BaitAmbush)] = Unsorted::CurrentFrame + BaitRetryFrames;
					}
				}
				else
				{
					// Sprung: everyone fights what came, as far as BaitFightCells from the ambush.
					const CellStruct enemyCenter = seenEnemyCentroidNear(plan.StagingCell, BaitFightCells, 150);

					if (enemyCenter == CellStruct::Empty)
						endBait(ambushPaid());
					else
						attackMoveGroup(plan.Members, MapClass::Instance.TryGetCellAt(enemyCenter));
				}
			}
			else if (!planShelved(NeuralPlanId::BaitAmbush) && planReady(NeuralPlanId::BaitAmbush) && knownEnemyHomeBuildings > 0)
			{
				std::vector<FootClass*> idle = collectIdleGround(false);
				BuildingClass* pLureTarget = nullptr;
				CellClass* pAmbush = nullptr;
				double bestDist = 1e9;

				// The lure: the known enemy building nearest to our home, BaitLureMinCells away or more,
				// with no known defense near it or near its ambush cell (a tower would kill the bait and
				// shell the ambush, and its owner need not send anyone).
				if (idle.size() >= BaitIdleMin)
				{
					// Not a building our units are already firing at, nor a unit folded into a building:
					// the lure was gone before the ambush stood in place in 107 of 314 plans (the
					// building nearest to our home is the one our waves go for, 2026-11-15).
					std::unordered_set<const AbstractClass*> underOurFire;

					for (auto const& [uid, pOwn] : ownFoots)
					{
						if (pOwn->Target)
							underOurFire.insert(pOwn->Target);
					}

					for (auto const pBld : knownEnemyBuildingList)
					{
						if (!isKnownEnemyBuilding(pBld) || ObjectZone(pBld, MovementZone::Normal) != homeZone || pBld->Type->CanBeOccupied
							|| pBld->Type->Insignificant || pBld->Type->UndeploysInto || underOurFire.contains(pBld)
							|| !deceptionTargets(NeuralPlanId::BaitAmbush, pBld->Owner))
						{
							continue;
						}

						const CellStruct bc = pBld->GetMapCoords();
						const double d = bc.DistanceFrom(homeCell);

						if (d < BaitLureMinCells || d >= bestDist || CountKnownEnemyDefensesNear(pHouse, bc, BaitTowerClearCells, false) > 0)
							continue;

						auto const pCell = landCellInZone(pointAlong(bc, homeCell, BaitAmbushCells, 0.0), homeZone, 4);

						if (!pCell || CountKnownEnemyDefensesNear(pHouse, pCell->MapCoords, BaitTowerClearCells, false) > 0)
							continue;

						bestDist = d;
						pLureTarget = pBld;
						pAmbush = pCell;
					}
				}

				// The bait: the fastest unit that is not elite and can hurt that building.
				FootClass* pBaitFoot = nullptr;
				std::vector<FootClass*> ambushGroup;

				if (pLureTarget)
				{
					std::vector<FootClass*> fastest = idle;
					sortFastest(fastest);
					auto const baitIt = std::find_if(fastest.begin(), fastest.end(), [pLureTarget](FootClass* pFoot)
						{ return !pFoot->Veterancy.IsElite() && HurtsBuilding(pFoot->GetTechnoType(), pLureTarget); });

					if (baitIt != fastest.end())
						pBaitFoot = *baitIt;

					for (auto const pFoot : idle)
					{
						if (pFoot != pBaitFoot && ambushGroup.size() < BaitAmbushSize)
							ambushGroup.push_back(pFoot);
					}
				}

				// The ambush has to get to its cell: what is known on its way and around it.
				const bool strongEnough = pBaitFoot && ambushGroup.size() + 1 >= BaitIdleMin
					&& groupValueOf(ambushGroup) * 100 >= knownEnemyStrengthOnWay(homeCell, pAmbush->MapCoords, PlanStrikeGuardFrames) * PlanStrikeAdvantagePercent;
				const int utility = strongEnough ? 270 : -1000;

				if (utility > 0 && shouldTryStart(NeuralPlanId::BaitAmbush, utility))
				{
					std::vector<FootClass*> group { pBaitFoot };
					group.insert(group.end(), ambushGroup.begin(), ambushGroup.end());
					startPlan(NeuralPlanId::BaitAmbush, group, pLureTarget->GetMapCoords(), pAmbush->MapCoords, pLureTarget->UniqueID, BaitPlanFrames);

					if (planOf(NeuralPlanId::BaitAmbush).Active)
					{
						planOf(NeuralPlanId::BaitAmbush).AuxBaseline = 0;
					}
				}
			}
		}

		// ---------------------------------------------------------------- Plan 21: Ally Joint Attack
		// Team games: an ally is attacking a known enemy building with several units, so our idle
		// group joins that same attack (today we only helped allies defend).
		{
			auto& plan = planOf(NeuralPlanId::AllyJointAttack);

			if (plan.Active)
			{
				auto const pTarget = FindBuildingByID(plan.TargetID);

				if (!pTarget)
					finishPlan(NeuralPlanId::AllyJointAttack, true, true);
				else if (memberValue(plan) * 100 < plan.InitialValue * 40 || Unsorted::CurrentFrame >= plan.Deadline)
					finishPlan(NeuralPlanId::AllyJointAttack, false, true);
				else
					attackMoveGroup(plan.Members, pTarget->GetCell());
			}
			else if (planReady(NeuralPlanId::AllyJointAttack) && knownEnemyHomeBuildings > 0)
			{
				// Allied attackers near each known enemy building, keyed by building UniqueID.
				std::map<DWORD, int> allyPressure;

				for (auto const pTech : TechnoClass::Array)
				{
					auto const pFoot = abstract_cast<FootClass*>(pTech);

					if (!pFoot || !pFoot->IsAlive || pFoot->InLimbo || !pFoot->Owner || pFoot->Owner == pHouse
						|| !pHouse->IsAlliedWith(pFoot->Owner) || pFoot->Owner->IsNeutral() || pFoot->Owner->IsObserver()
						|| pFoot->WhatAmI() == AbstractType::Aircraft)
					{
						continue;
					}

					const bool attacking = pFoot->Target != nullptr || pFoot->MegaMission == Mission::AttackMove
						|| pFoot->CurrentMission == Mission::Hunt || pFoot->CurrentMission == Mission::Attack;

					if (!attacking || GroundRangeOf(pFoot->GetTechnoType()) <= 0)
						continue;

					const CellStruct fc = pFoot->GetMapCoords();

					for (auto const pBld : knownEnemyBuildingList)
					{
						if (isKnownEnemyBuilding(pBld) && pBld->GetMapCoords().DistanceFrom(fc) <= 12.0)
						{
							allyPressure[pBld->UniqueID]++;
							break;
						}
					}
				}

				DWORD bestID = 0;
				int bestCount = 0;

				for (auto const& [uid, count] : allyPressure)
				{
					if (count > bestCount)
					{
						bestCount = count;
						bestID = uid;
					}
				}

				// Expert 9 (coalition) joins smaller allied attacks, with a bigger group and more eagerly.
				auto const pCoalitionExt = HouseExt::Fetch(pHouse);
				const bool coalitionExpert = pCoalitionExt && pCoalitionExt->NeuralAI_ActiveExpert == 9;
				auto const pTarget = bestCount >= (coalitionExpert ? 3 : 4) ? FindBuildingByID(bestID) : nullptr;
				std::vector<FootClass*> group;

				if (pTarget && ObjectZone(pTarget, MovementZone::Normal) == homeZone)
				{
					group = collectIdleGround(false);
					group.resize(std::min<size_t>(group.size(), coalitionExpert ? 14 : 10));

					if (group.size() < (coalitionExpert ? 3u : 4u))
						group.clear();
				}

				const int utility = !group.empty() ? (300 + std::min(200, bestCount * 20) + (coalitionExpert ? 250 : 0)) : -1000;

				if (utility > 0 && shouldTryStart(NeuralPlanId::AllyJointAttack, utility))
					startPlan(NeuralPlanId::AllyJointAttack, group, pTarget->GetMapCoords(), CellStruct::Empty, pTarget->UniqueID, 4500);
			}
		}

		// ---------------------------------------------------------------- Plan 22: Forward Repair
		// Our only repair station is far from the front: ask for a second one near our most
		// forward structures (ForwardRepairCell, read by the building selector, which puts it ahead
		// of its routine picks, and by placement), so damaged units repair close to the fight
		// instead of driving home.
		{
			auto& plan = planOf(NeuralPlanId::ForwardRepair);
			int repairStations = 0;
			BuildingClass* pFirstRepair = nullptr;

			for (auto const pBld : pHouse->Buildings)
			{
				if (pBld && pBld->IsAlive && !pBld->InLimbo && pBld->Type && pBld->Type->UnitRepair && !pBld->Type->Naval)
				{
					repairStations++;

					if (!pFirstRepair || pBld->UniqueID < pFirstRepair->UniqueID)
						pFirstRepair = pBld;
				}
			}

			auto clearForward = [&]()
			{
				macro.ForwardRepairCell = CellStruct::Empty;
				macro.ForwardRepairUntilFrame = -1;
			};

			// The station counted above may be one the house cannot build (a captured Tech Outpost),
			// and a lost factory takes the prerequisites of its own: the plan asks only while a
			// station type is buildable now. In the focus batch of 2026-11-15 11 of 26 plans that ran
			// out of time had none buildable in any sample of their 5400 frames, and the same house
			// started the plan again and again (9 times in match #9552).
			auto stationBuildable = [pHouse]()
			{
				for (auto const pType : BuildingTypeClass::Array)
				{
					if (pType && pType->UnitRepair && !pType->Naval && CanBuildUnderHumanRules(pHouse, pType))
						return true;
				}

				return false;
			};

			if (plan.Active)
			{
				if (repairStations >= 2)
				{
					clearForward();
					finishPlan(NeuralPlanId::ForwardRepair, true, false);
				}
				else if (repairStations == 0 || Unsorted::CurrentFrame >= plan.Deadline || !stationBuildable())
				{
					clearForward();
					finishPlan(NeuralPlanId::ForwardRepair, false, false);
				}
				else
				{
					macro.ForwardRepairCell = plan.StagingCell;
					macro.ForwardRepairUntilFrame = plan.Deadline;
				}
			}
			else if (planReady(NeuralPlanId::ForwardRepair) && Unsorted::CurrentFrame >= 9000 && repairStations == 1
				&& pFirstRepair && knownEnemyCentroid != CellStruct::Empty && pHouse->Available_Money() >= 2000 && stationBuildable())
			{
				const CellStruct rc = pFirstRepair->GetMapCoords();
				const double frontDist = rc.DistanceFrom(knownEnemyCentroid);
				const int utility = frontDist > 45.0 ? 260 : -1000;

				if (utility > 0 && shouldTryStart(NeuralPlanId::ForwardRepair, utility))
				{
					const CellStruct forward = pointAlong(rc, knownEnemyCentroid, frontDist * 0.45, 0.0);
					startPlan(NeuralPlanId::ForwardRepair, std::vector<FootClass*> {}, knownEnemyCentroid, forward, 0, 5400);
					macro.ForwardRepairCell = forward;
					macro.ForwardRepairUntilFrame = planOf(NeuralPlanId::ForwardRepair).Deadline;
				}
			}
		}

		// ---------------------------------------------------------------- Plans 39 and 40: ally sites
		// Team games with BuildOffAlly: a few of our buildings next to an ally's base, one at a time
		// (AllySite*, read by the building selector and placement). Never while the ally would be
		// left short of room (AllyKeepsRoomFor), and placement keeps a free cell from its buildings.
		// - Ally Backup (39, contingency): late in the match, at an ally base away from ours, the
		//   cheapest MCV prerequisite list (war factory first), on the side of its base away from the
		//   enemy: losing every ConYard and MCV no longer ends the match (Survival builds the MCV).
		// - Ally Forward Outpost (40, chosen by Experts 3, 7 and 9): an ally base much closer to the
		//   known enemy than our war factories gets a war factory and a barracks of ours on its flank
		//   toward the enemy; UpdatePrimaryFactories then builds from them, near the front.
		{
			auto& backup = planOf(NeuralPlanId::AllyBackup);
			auto& outpost = planOf(NeuralPlanId::AllyForwardOutpost);
			auto const pSiteExt = HouseExt::Fetch(pHouse);
			const int activeExpert = pSiteExt ? pSiteExt->NeuralAI_ActiveExpert : -1;
			auto const allyYards = AllyYardsForSites(pHouse);

			auto clearSite = [&]()
			{
				macro.AllySiteBuildType = -1;
				macro.AllySiteCell = CellStruct::Empty;
				macro.AllySiteYardCell = CellStruct::Empty;
				macro.AllySiteUntilFrame = -1;
				macro.AllySiteReservedCell = CellStruct::Empty;
				macro.AllySiteReservedType = -1;
				macro.AllySiteReservedFrame = -1;
			};

			// Placement failed for the wanted building (cooldown set by the placement code): the
			// site has no room left that suits both of us.
			auto placementBlocked = [&](BuildingTypeClass* pType)
			{
				auto const it = macro.BuildingPlacementCooldownUntilFrame.find(pType->ArrayIndex);

				return it != macro.BuildingPlacementCooldownUntilFrame.end() && Unsorted::CurrentFrame < it->second;
			};

			// Forward outpost needs: a war factory and a barracks of ours at the site.
			auto missingForOutpost = [&](const CellStruct& yardCell)
			{
				std::vector<BuildingTypeClass*> missing;
				auto const siteBuildings = OwnBuildingsAtAllySite(pHouse, yardCell);

				for (const AbstractType kind : { AbstractType::UnitType, AbstractType::InfantryType })
				{
					const bool has = std::any_of(siteBuildings.begin(), siteBuildings.end(), [kind](BuildingClass* pBld)
						{
							return pBld->Type->Factory == kind && !pBld->Type->Naval;
						});

					if (!has)
					{
						if (auto const pType = CheapestBuildableGroundFactory(pHouse, kind))
							missing.push_back(pType);
					}
				}

				return missing;
			};

			// Drives an active site plan: the next missing building, or the end of the plan.
			auto runSite = [&](NeuralPlanId id, NeuralPlanInstance& plan, std::vector<BuildingTypeClass*> const& missing, bool ok)
			{
				auto const pYard = FindBuildingByID(plan.TargetID);

				if (!pYard || !pYard->Owner || !pHouse->IsAlliedWith(pYard->Owner) || pHouse->ConYards.Count == 0
					|| Unsorted::CurrentFrame >= plan.Deadline || !ok || macro.AllySiteFailedFrame >= plan.StartFrame)
				{
					clearSite();
					finishPlan(id, false, false);
					return;
				}

				if (missing.empty())
				{
					clearSite();
					finishPlan(id, true, false);
					return;
				}

				auto const pNext = missing.front();

				if (placementBlocked(pNext) || !AllyKeepsRoomFor(pYard, pNext))
				{
					clearSite();
					finishPlan(id, false, false);
					return;
				}

				macro.AllySiteBuildType = pNext->ArrayIndex;
				macro.AllySiteCell = plan.StagingCell;
				macro.AllySiteYardCell = pYard->GetMapCoords();
				macro.AllySiteUntilFrame = plan.Deadline;

				// Before paying for it: a cell must exist at the site now (checked again every 600
				// frames while we wait). None means no room that suits both of us: the plan ends and
				// the money stays.
				if (macro.AllySiteReservedType != pNext->ArrayIndex || macro.AllySiteReservedCell == CellStruct::Empty
					|| Unsorted::CurrentFrame - macro.AllySiteReservedFrame >= 600)
				{
					macro.AllySiteReservedType = pNext->ArrayIndex;
					macro.AllySiteReservedFrame = Unsorted::CurrentFrame;
					macro.AllySiteReservedCell = CellStruct::Empty;
					macro.AllySiteReservedCell = NeuralAI::FindSpatialTetrisBuildCell(pHouse, pNext);

					if (macro.AllySiteReservedCell == CellStruct::Empty)
					{
						clearSite();
						finishPlan(id, false, false);
					}
				}
			};

			if (backup.Active)
			{
				auto const pYard = FindBuildingByID(backup.TargetID);
				bool ok = pYard != nullptr;
				auto const missing = pYard ? MissingForAllyBackup(pHouse, pYard->GetMapCoords(), ok) : std::vector<BuildingTypeClass*> {};
				runSite(NeuralPlanId::AllyBackup, backup, missing, ok);
			}
			else if (outpost.Active)
			{
				auto const pYard = FindBuildingByID(outpost.TargetID);
				auto const missing = pYard ? missingForOutpost(pYard->GetMapCoords()) : std::vector<BuildingTypeClass*> {};
				runSite(NeuralPlanId::AllyForwardOutpost, outpost, missing, pYard != nullptr);
			}
			else if (!allyYards.empty() && pHouse->ConYards.Count > 0)
			{
				clearSite();
				const int money = pHouse->Available_Money();

				// Backup: the ally ConYard at least 30 cells from all of ours, farthest from the
				// known enemy (else from us).
				if (planReady(NeuralPlanId::AllyBackup) && Unsorted::CurrentFrame >= 27000 && money >= 3000)
				{
					const CellStruct threat = knownEnemyCentroid != CellStruct::Empty ? knownEnemyCentroid : homeCell;
					BuildingClass* pBest = nullptr;
					double bestDist = -1.0;

					for (auto const pYard : allyYards)
					{
						const CellStruct yc = pYard->GetMapCoords();
						bool farFromOurs = true;

						for (auto const pOwnYard : pHouse->ConYards)
						{
							if (pOwnYard && pOwnYard->GetMapCoords().DistanceFrom(yc) < 30.0)
								farFromOurs = false;
						}

						const double d = yc.DistanceFrom(threat);

						if (farFromOurs && d > bestDist)
						{
							bestDist = d;
							pBest = pYard;
						}
					}

					bool ok = false;
					auto const missing = pBest ? MissingForAllyBackup(pHouse, pBest->GetMapCoords(), ok) : std::vector<BuildingTypeClass*> {};
					int missingCost = 0;

					for (auto const pType : missing)
						missingCost += pType->GetActualCost(pHouse);

					const bool fits = ok && !missing.empty() && money >= missingCost + 1000 && AllyKeepsRoomFor(pBest, missing.front());
					const int utility = fits
						? 380 + (activeExpert == 9 || activeExpert == 1 ? 200 : 0) + (pHouse->ConYards.Count == 1 ? 150 : 0)
						: -1000;

					if (utility > 0 && shouldTryStart(NeuralPlanId::AllyBackup, utility))
					{
						const CellStruct site = AllySiteCellFor(pBest, threat, true, 0);
						startPlan(NeuralPlanId::AllyBackup, std::vector<FootClass*> {}, pBest->GetMapCoords(), site, pBest->UniqueID, 9000);
					}
				}

				// Forward outpost: the ally ConYard on our landmass nearest the known enemy, at least
				// 25 cells closer to it than our nearest ground war factory.
				if (!planOf(NeuralPlanId::AllyBackup).Active && planReady(NeuralPlanId::AllyForwardOutpost)
					&& (activeExpert == 3 || activeExpert == 7 || activeExpert == 9) && knownEnemyCentroid != CellStruct::Empty
					&& money >= 4000)
				{
					double ourFront = -1.0;

					for (auto const pBld : pHouse->Buildings)
					{
						if (pBld && pBld->IsAlive && !pBld->InLimbo && pBld->Type && pBld->Type->Factory == AbstractType::UnitType
							&& !pBld->Type->Naval)
						{
							const double d = pBld->GetMapCoords().DistanceFrom(knownEnemyCentroid);

							if (ourFront < 0.0 || d < ourFront)
								ourFront = d;
						}
					}

					BuildingClass* pBest = nullptr;
					double bestDist = 0.0;

					for (auto const pYard : allyYards)
					{
						const CellStruct yc = pYard->GetMapCoords();
						const double d = yc.DistanceFrom(knownEnemyCentroid);

						if (ourFront >= 0.0 && zoneOf(yc) == homeZone && d <= ourFront - 25.0 && (!pBest || d < bestDist))
						{
							bestDist = d;
							pBest = pYard;
						}
					}

					auto const missing = pBest ? missingForOutpost(pBest->GetMapCoords()) : std::vector<BuildingTypeClass*> {};
					int missingCost = 0;

					for (auto const pType : missing)
						missingCost += pType->GetActualCost(pHouse);

					const bool fits = !missing.empty() && money >= missingCost + 1500 && AllyKeepsRoomFor(pBest, missing.front());
					const int utility = fits ? 330 + 150 + static_cast<int>(std::min(120.0, ourFront - bestDist)) : -1000;

					if (utility > 0 && shouldTryStart(NeuralPlanId::AllyForwardOutpost, utility))
					{
						// On the flank toward the enemy, not in the ally's line of fire or its exits.
						const int side = (houseIdx % 2 == 0) ? 6 : -6;
						const CellStruct site = AllySiteCellFor(pBest, knownEnemyCentroid, false, side);
						startPlan(NeuralPlanId::AllyForwardOutpost, std::vector<FootClass*> {}, pBest->GetMapCoords(), site, pBest->UniqueID, 9000);
					}
				}
			}
			else
			{
				clearSite();
			}
		}

		// ---------------------------------------------------------------- Plan 23: Naval Strike Group
		// Our long-range ships (ground weapon >= 10 cells, or carriers / missile launchers) shell
		// a known building reachable from the sea, from water cells outside the reach of the
		// other known defenses, with warships screening them. Coastal defenses are taken out first.
		// The whole free fleet goes but for a reserve by our naval yard, each shelling ship to its
		// own firing cell (NavalStrikeMaxShips and the other Naval* constants). Damaged ships
		// (< 40%) head back to our naval yard, and free healthy ones take their place. With fewer
		// than two long-range ships, NavalMassStrikeShips short-range gun ships of one sea or more
		// go as a mass strike, all of them shelling one target.
		{
			auto& plan = planOf(NeuralPlanId::NavalStrikeGroup);

			// How far a ship reaches land: its longest gun that hits the ground, whatever else it
			// launches; only a ship with no such gun counts on its aircraft or missiles (18 cells). A
			// destroyer counted as a carrier for its anti-submarine plane was sent to a firing cell
			// 17 cells from the building and lost its target there again and again (17 of 22
			// destroyers at their firing cell had none, 2026-11-14).
			auto landReachOf = [](UnitTypeClass* pType) -> int
			{
				int gun = 0;
				bool launches = pType->Spawns != nullptr;

				for (int i = 0; i < 2; ++i)
				{
					auto const pWS = pType->GetWeapon(i);
					auto const pWeapon = pWS ? pWS->WeaponType : nullptr;

					if (!pWeapon)
						continue;

					if (pWeapon->Spawner)
						launches = true;
					else if (LandTargetingAllows(pType, i) && WeaponHitsGround(pWeapon))
						gun = std::max(gun, pWeapon->Range);
				}

				return gun > 0 ? gun : (launches ? 18 * Unsorted::LeptonsPerCell : 0);
			};

			auto isLongRangeShip = [&landReachOf](FootClass* pFoot) -> bool
			{
				auto const pUnit = abstract_cast<UnitClass*>(pFoot);

				return pUnit && pUnit->Type && pUnit->Type->Naval && landReachOf(pUnit->Type) >= 10 * Unsorted::LeptonsPerCell;
			};

			// A ship whose gun (or aircraft) hits land at all. In a mass strike (plan.MassStrike) every one of them
			// shells, not only the long-range ones.
			auto isGunShip = [&landReachOf](FootClass* pFoot) -> bool
			{
				auto const pUnit = abstract_cast<UnitClass*>(pFoot);

				return pUnit && pUnit->Type && pUnit->Type->Naval && landReachOf(pUnit->Type) > 0;
			};

			auto shellsIn = [&](FootClass* pFoot) -> bool
			{
				return isLongRangeShip(pFoot) || (plan.MassStrike && isGunShip(pFoot));
			};

			CellClass* pNavalYard = nullptr;

			for (auto const pBld : pHouse->Buildings)
			{
				if (pBld && pBld->IsAlive && !pBld->InLimbo && pBld->Type && pBld->Type->Naval && pBld->Type->Factory != AbstractType::None)
				{
					pNavalYard = pBld->GetCell();
					break;
				}
			}

			if (plan.Active)
			{
				// The squads' targets (plan.Targets): the plan goes on while one of them stands.
				std::vector<BuildingClass*> standing;
				int firstStanding = -1;

				for (const DWORD targetId : plan.Targets)
				{
					standing.push_back(FindBuildingByID(targetId));

					if (standing.back() && firstStanding < 0)
						firstStanding = static_cast<int>(standing.size()) - 1;
				}

				BuildingClass* pTarget = FindBuildingByID(plan.TargetID);

				if (!pTarget && firstStanding >= 0)
					pTarget = standing[static_cast<size_t>(firstStanding)];

				auto const pFiring = MapClass::Instance.TryGetCellAt(plan.StagingCell);

				if (!pTarget)
				{
					finishPlan(NeuralPlanId::NavalStrikeGroup, true, false);
				}
				else if (!pFiring || memberValue(plan) * 100 < plan.InitialValue * 40 || Unsorted::CurrentFrame >= plan.Deadline)
				{
					finishPlan(NeuralPlanId::NavalStrikeGroup, false, false);
				}
				else
				{
					// Each shelling ship goes to its own firing cell (plan.Cells, in the order of the members).
					size_t shellingIndex = 0;
					int healthyShelling = 0;

					for (const DWORD uid : plan.Members)
					{
						auto const pShip = findOwnFoot(uid);

						if (!pShip)
							continue;

						// A shelling ship: its squad's target and its own cell by it (plan.Cells, ship i in squad
						// i % squads); with its squad's target down, the first one standing, from the cell of the
						// same rank in that squad.
						const bool shells = shellsIn(pShip);
						CellStruct ownCell = plan.StagingCell;
						BuildingClass* pShipTarget = pTarget;

						if (shells && !plan.Cells.empty())
						{
							const size_t index = shellingIndex++ % plan.Cells.size();
							ownCell = plan.Cells[index];

							if (!standing.empty())
							{
								const size_t squads = standing.size();
								const size_t squad = index % squads;
								pShipTarget = standing[squad];

								if (!pShipTarget && firstStanding >= 0)
								{
									const size_t other = static_cast<size_t>(firstStanding);
									const size_t moved = (index / squads) * squads + other;
									pShipTarget = standing[other];
									ownCell = plan.Cells[moved < plan.Cells.size() ? moved : other];
								}

								if (!pShipTarget)
									pShipTarget = pTarget;
							}
						}

						auto const pOwnCell = ownCell == plan.StagingCell ? pFiring : MapClass::Instance.TryGetCellAt(ownCell);

						if (shells && pShip->GetHealthPercentage() >= 0.4)
							healthyShelling++;

						if (pShip->GetHealthPercentage() < 0.4)
						{
							if (pNavalYard)
								IssueMoveOrder(pShip, pNavalYard, 5.0);

							continue;
						}

						const bool siege = shells;
						const double distToFiring = pShip->GetMapCoords().DistanceFrom(ownCell);
						// A ship that has its target in reach (the engine's own test: the weapon it would
						// pick, its range and minimum range) fires from where it is, short of its firing
						// cell or not (match #11450: Boomers held off their cells never fired).
						const bool inReach = siege && pShipTarget && distToFiring > 4.0 && pShip->IsCloseEnoughToAttack(pShipTarget);

						if (siege && (distToFiring <= 4.0 || inReach))
						{
							if (pShip->Target != pShipTarget)
							{
								if (inReach)
									CountDecision(NeuralDecision::NavalStrikeInRange, pHouse->ArrayIndex);

								pShip->SetTarget(pShipTarget);
								pShip->QueueMission(Mission::Attack, true);
							}
						}
						else if (pShip->Target == nullptr)
						{
							// Escorts screen around the first firing position (they engage on their own).
							IssueMoveOrder(pShip, pOwnCell ? pOwnCell : pFiring, siege ? 2.0 : 4.0);
						}
					}

					// Relief (user): while fewer healthy ships shell than the plan took, a free healthy one of
					// the same sea joins, one per AI tick, the farthest from our naval yard. The free ships nearest
					// to the yard stay there as its reserve.
					if (healthyShelling < plan.AuxBaseline && pFiring)
					{
						std::vector<FootClass*> fresh;
						const int seaZone = MapClass::Instance.GetMovementZoneType(plan.StagingCell, MovementZone::Water, false);

						for (auto const& [uid, pFoot] : ownFoots)
						{
							auto const pUnit = abstract_cast<UnitClass*>(pFoot);

							if (pUnit && pUnit->Type && pUnit->Type->Naval && shellsIn(pUnit) && isFreeForPlan(pUnit)
								&& !IsAmphibiousOrAirTransport(pUnit->Type) && pUnit->GetHealthPercentage() >= 0.9
								&& ObjectZone(pUnit, MovementZone::Water) == seaZone)
							{
								fresh.push_back(pUnit);
							}
						}

						const int reserve = (pNavalYard && static_cast<int>(fresh.size()) > NavalHomeReserveMin + 2)
							? std::max(NavalHomeReserveMin, static_cast<int>(fresh.size()) * NavalHomeReservePercent / 100)
							: 0;

						if (static_cast<int>(fresh.size()) > reserve)
						{
							const CellStruct yard = pNavalYard ? pNavalYard->MapCoords : plan.StagingCell;
							std::sort(fresh.begin(), fresh.end(), [](FootClass* a, FootClass* b) { return a->UniqueID < b->UniqueID; });
							std::stable_sort(fresh.begin(), fresh.end(), [&yard](FootClass* a, FootClass* b)
								{
									return a->GetMapCoords().DistanceFrom(yard) > b->GetMapCoords().DistanceFrom(yard);
								});
							auto const pJoin = fresh.front();
							plan.Members.push_back(pJoin->UniqueID);
							macro.PlanMembers.insert(pJoin->UniqueID);
							Debug::Log("[NeuralAI] House %d navalStrike: %s %u relieves a damaged ship (%d of %d shelling healthy), frame %d.\n",
								houseIdx, pJoin->GetTechnoType()->ID, pJoin->UniqueID, healthyShelling, plan.AuxBaseline, Unsorted::CurrentFrame);
						}
					}
				}
			}
			else if (planReady(NeuralPlanId::NavalStrikeGroup))
			{
				std::vector<FootClass*> siegeShips;
				std::vector<FootClass*> escorts;
				// Healthy (40% or more) ships whose guns hit land at under 10 cells (a mass strike's).
				std::vector<FootClass*> gunShips;
				int fleetRange = 0;
				int fleetMinRange = 0;
				int fleetZone = -1;

				for (auto const& [uid, pFoot] : ownFoots)
				{
					auto const pUnit = abstract_cast<UnitClass*>(pFoot);

					if (!pUnit || !pUnit->Type || !pUnit->Type->Naval || !isFreeForPlan(pUnit) || IsAmphibiousOrAirTransport(pUnit->Type))
						continue;

					if (isLongRangeShip(pUnit))
					{
						siegeShips.push_back(pUnit);
					}
					else
					{
						if (GetCombatValueOfType(pUnit->Type, false) > 0)
							escorts.push_back(pUnit);

						if (isGunShip(pUnit) && pUnit->GetHealthPercentage() >= 0.4)
							gunShips.push_back(pUnit);
					}
				}

				auto byID = [](FootClass* a, FootClass* b) { return a->UniqueID < b->UniqueID; };
				std::sort(siegeShips.begin(), siegeShips.end(), byID);
				std::sort(escorts.begin(), escorts.end(), byID);
				std::sort(gunShips.begin(), gunShips.end(), byID);

				// Mass strike (user approved, 2026-11-19): with fewer than two long-range ships, the gun ships of the sea
				// that holds the most of them (the lower zone among equals) shell together, with the long-range ship of
				// that sea, when they are NavalMassStrikeShips or more: 8-cell destroyers never started this plan, and 77
				// of them sat idle in match #11499.
				bool massStrike = false;

				if (siegeShips.size() < 2)
				{
					std::map<int, size_t> gunShipsBySea;

					for (auto const pShip : gunShips)
						gunShipsBySea[ObjectZone(pShip, MovementZone::Water)]++;

					int massSea = -1;
					size_t most = 0;

					for (auto const& [sea, count] : gunShipsBySea)
					{
						if (sea >= 0 && count > most)
						{
							most = count;
							massSea = sea;
						}
					}

					if (most >= NavalMassStrikeShips)
					{
						massStrike = true;
						auto const otherSea = [massSea](FootClass* pShip) { return ObjectZone(pShip, MovementZone::Water) != massSea; };
						std::erase_if(gunShips, otherSea);
						std::erase_if(siegeShips, otherSea);
						siegeShips.insert(siegeShips.end(), gunShips.begin(), gunShips.end());
						std::sort(siegeShips.begin(), siegeShips.end(), byID);
						std::erase_if(escorts, [&gunShips](FootClass* pShip)
							{ return std::find(gunShips.begin(), gunShips.end(), pShip) != gunShips.end(); });
					}
				}

				// A mass strike's firing cells are within the reach most of its ships have (the lower median): the shortest
				// gun of a mixed fleet pulled them all to the shore. Its ships with a shorter gun close in on their target
				// under their attack order.
				std::vector<int> reaches;

				for (auto const pShip : siegeShips)
				{
					auto const pShipType = abstract_cast<UnitClass*>(pShip)->Type;
					// The firing cell is within the shortest reach of the shelling ships, so all of them fire.
					const int reach = landReachOf(pShipType);
					fleetRange = fleetRange == 0 ? reach : std::min(fleetRange, reach);
					reaches.push_back(reach);

					// Minimum range of its ground weapons (e.g. a missile sub's launcher): the firing
					// spot keeps outside the largest one of the fleet.
					for (int i = 0; i < 2; ++i)
					{
						auto const pWS = pShipType->GetWeapon(i);

						if (pWS && pWS->WeaponType && WeaponHitsGround(pWS->WeaponType))
							fleetMinRange = std::max(fleetMinRange, pWS->WeaponType->MinimumRange);
					}
				}

				if (massStrike && !reaches.empty())
				{
					std::sort(reaches.begin(), reaches.end());
					fleetRange = reaches[(reaches.size() - 1) / 2];
				}

				// A reserve stays by our naval yard to defend it: the ships nearest to the yard,
				// NavalHomeReservePercent of the fleet and NavalHomeReserveMin at least, none for a small one.
				auto const keepReserve = [&](std::vector<FootClass*>& ships) -> int
				{
					const int count = static_cast<int>(ships.size());

					if (!pNavalYard || count <= NavalHomeReserveMin + 2)
						return 0;

					const int reserve = std::max(NavalHomeReserveMin, count * NavalHomeReservePercent / 100);
					const CellStruct yard = pNavalYard->MapCoords;
					std::stable_sort(ships.begin(), ships.end(), [&yard](FootClass* a, FootClass* b)
						{
							return a->GetMapCoords().DistanceFrom(yard) < b->GetMapCoords().DistanceFrom(yard);
						});
					ships.erase(ships.begin(), ships.begin() + reserve);
					std::sort(ships.begin(), ships.end(), byID);

					return reserve;
				};

				const int reserveShips = keepReserve(siegeShips);
				keepReserve(escorts);

				if (!siegeShips.empty())
					fleetZone = ObjectZone(siegeShips.front(), MovementZone::Water);

				BuildingClass* pTarget = nullptr;
				CellStruct firing = CellStruct::Empty;
				int bestScore = INT_MIN;
				const int rangeCells = std::clamp(fleetRange / Unsorted::LeptonsPerCell, 6, 26);

				// Enemy ships we know of: the firing spot stays out of their reach too.
				std::vector<std::pair<CellStruct, int>> enemyShips;

				for (auto const pEnemy : UnitClass::Array)
				{
					if (pEnemy && pEnemy->IsAlive && !pEnemy->InLimbo && pEnemy->Type && pEnemy->Type->Naval && pEnemy->Owner
						&& pEnemy->Owner != pHouse && !pHouse->IsAlliedWith(pEnemy->Owner) && !pEnemy->Owner->IsNeutral()
						&& IsCellKnownToAI(pHouse, pEnemy->GetMapCoords()))
					{
						const int reach = GroundRangeOf(pEnemy->Type) / Unsorted::LeptonsPerCell;

						if (reach > 0)
							enemyShips.emplace_back(pEnemy->GetMapCoords(), reach + 2);
					}
				}

				auto const inEnemyShipReach = [&enemyShips](const CellStruct& c)
				{
					return std::any_of(enemyShips.begin(), enemyShips.end(),
						[&c](const auto& ship) { return c.DistanceFrom(ship.first) <= ship.second; });
				};

				// Firing cells by a building: water cells of our fleet's sea within range, outside the reach of
				// the other known defenses and of the enemy ships, NavalFiringSpacingCells from every cell
				// already in cells; wanted of them at most, added to cells.
				const int minRangeCells = std::max(4, fleetMinRange / Unsorted::LeptonsPerCell + 1);

				auto const firingCellsFor = [&](BuildingClass* pBld, size_t wanted, std::vector<CellStruct>& cells)
				{
					const CellStruct bc = pBld->GetMapCoords();
					const size_t first = cells.size();

					for (int r = rangeCells - 1; r >= minRangeCells && cells.size() - first < wanted; --r)
					{
						for (int k = 0; k < 16 && cells.size() - first < wanted; ++k)
						{
							constexpr int DirX[16] = { 10, 9, 7, 4, 0, -4, -7, -9, -10, -9, -7, -4, 0, 4, 7, 9 };
							constexpr int DirY[16] = { 0, 4, 7, 9, 10, 9, 7, 4, 0, -4, -7, -9, -10, -9, -7, -4 };
							const CellStruct c { static_cast<short>(bc.X + DirX[k] * r / 10), static_cast<short>(bc.Y + DirY[k] * r / 10) };
							auto const pCell = MapClass::Instance.CoordinatesLegal(c) ? MapClass::Instance.TryGetCellAt(c) : nullptr;

							if (!pCell || pCell->LandType != LandType::Water
								|| MapClass::Instance.GetMovementZoneType(c, MovementZone::Water, false) != fleetZone
								|| inKnownDefenseReach(c, pBld) || inEnemyShipReach(c))
							{
								continue;
							}

							if (std::none_of(cells.begin(), cells.end(),
								[&c](const CellStruct& taken) { return taken.DistanceFrom(c) < NavalFiringSpacingCells; }))
							{
								cells.push_back(c);
							}
						}
					}
				};

				// The targets (user: a bombardment spread over several targets is chaos for the victim, who
				// stops only some of the attacks): one for each NavalSquadShips shelling ships,
				// NavalMaxTargets at most, NavalTargetSpacingCells apart, the most worth first (coastal
				// defenses first), each with a firing cell.
				const size_t shelling = std::min<size_t>(siegeShips.size(), NavalStrikeMaxShips);
				std::vector<BuildingClass*> targets;
				std::vector<CellStruct> targetCells;

				if (siegeShips.size() >= 2 || (siegeShips.size() == 1 && escorts.size() >= 2))
				{
					std::vector<std::pair<int, BuildingClass*>> worthy;

					for (auto const pBld : knownEnemyBuildingList)
					{
						if (!isKnownEnemyBuilding(pBld))
							continue;

						const bool isDefense = pBld->Type->IsBaseDefense && GroundRangeOf(pBld->Type) > 0;
						const int worth = isDefense ? 300 : valuableBuildingWorth(pBld->Type);

						if (worth > 0)
							worthy.emplace_back(worth, pBld);
					}

					std::stable_sort(worthy.begin(), worthy.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
					// A mass strike has one target: short guns split over several targets reach none of them in numbers.
					const size_t squads = massStrike ? 1 : std::clamp<size_t>((shelling + NavalSquadShips - 1) / NavalSquadShips, 1, NavalMaxTargets);
					std::vector<CellStruct> probe;

					for (auto const& [worth, pBld] : worthy)
					{
						if (targets.size() >= squads)
							break;

						const CellStruct bc = pBld->GetMapCoords();

						if (std::any_of(targets.begin(), targets.end(),
							[&bc](BuildingClass* pTaken) { return pTaken->GetMapCoords().DistanceFrom(bc) < NavalTargetSpacingCells; }))
						{
							continue;
						}

						probe.clear();
						firingCellsFor(pBld, 1, probe);

						if (probe.empty())
							continue;

						if (targets.empty())
						{
							bestScore = worth;
							pTarget = pBld;
							firing = probe.front();
						}

						targets.push_back(pBld);
						targetCells.push_back(probe.front());
					}
				}

				const int utility = pTarget ? (320 + bestScore / 4) : -1000;

				if (utility > 0 && shouldTryStart(NeuralPlanId::NavalStrikeGroup, utility))
				{
					std::vector<FootClass*> group(siegeShips.begin(), siegeShips.begin() + shelling);
					group.insert(group.end(), escorts.begin(), escorts.begin() + std::min<size_t>(escorts.size(), NavalStrikeMaxEscorts));
					startPlan(NeuralPlanId::NavalStrikeGroup, group, pTarget->GetMapCoords(), firing, pTarget->UniqueID, 5400);
					auto& started = planOf(NeuralPlanId::NavalStrikeGroup);

					// Ship i of the shelling ones goes to squad i % squads, each ship to a firing cell of its own
					// by its squad's target, NavalFiringSpacingCells from the others: a whole fleet sent to
					// one cell jams in front of it.
					if (started.Active && started.TargetID == pTarget->UniqueID)
					{
						const size_t squads = targets.size();
						started.AuxBaseline = static_cast<int>(shelling);
						started.MassStrike = massStrike;

						if (massStrike)
							CountDecision(NeuralDecision::NavalMassStrike, pHouse->ArrayIndex);

						std::vector<std::vector<CellStruct>> cellsOf(squads);
						std::vector<CellStruct> taken;

						for (size_t s = 0; s < squads; ++s)
						{
							started.Targets.push_back(targets[s]->UniqueID);
							const size_t members = (shelling + squads - 1 - s) / squads;
							const size_t before = taken.size();
							firingCellsFor(targets[s], std::max<size_t>(1, members), taken);
							cellsOf[s].assign(taken.begin() + static_cast<std::ptrdiff_t>(before), taken.end());

							if (cellsOf[s].empty())
								cellsOf[s].push_back(targetCells[s]);
						}

						for (size_t i = 0; i < shelling; ++i)
						{
							auto const& own = cellsOf[i % squads];
							started.Cells.push_back(own[(i / squads) % own.size()]);
						}

						Debug::Log("[NeuralAI] House %d navalStrike: %d shelling ships in %d squads and %d escorts%s, first target %s at (%d,%d), "
							"%d firing cells, %d ships left in reserve by the yard, frame %d.\n", houseIdx, static_cast<int>(shelling),
							static_cast<int>(squads), static_cast<int>(group.size() - shelling), massStrike ? " (mass strike of short guns)" : "",
							pTarget->Type->ID, pTarget->GetMapCoords().X, pTarget->GetMapCoords().Y, static_cast<int>(taken.size()), reserveShips,
							Unsorted::CurrentFrame);
					}
				}
			}
		}

		// ---------------------------------------------------------------- Plan 24: Engineer Assault
		// An armored ground transport carries engineers to a capturable valuable enemy building
		// (one-engineer capture, or already damaged down to the capture level) and unloads them
		// next to it. Members[0] = transport. AuxBaseline = phase (0 boarding, 1 driving, 2 out).
		{
			auto& plan = planOf(NeuralPlanId::EngineerAssault);
			const double captureLevel = RulesClass::Instance ? RulesClass::Instance->EngineerCaptureLevel : 1.0;

			if (plan.Active)
			{
				FootClass* const pTransport = plan.Members.empty() ? nullptr : findOwnFoot(plan.Members[0]);
				auto const pTarget = FindBuildingByID(plan.TargetID);
				const std::vector<DWORD> engineers(plan.Members.empty() ? plan.Members.end() : plan.Members.begin() + 1, plan.Members.end());

				if (!pTarget)
				{
					finishPlan(NeuralPlanId::EngineerAssault, false, true);
				}
				else if (pTarget->Owner == pHouse)
				{
					finishPlan(NeuralPlanId::EngineerAssault, true, true);
				}
				else if (Unsorted::CurrentFrame >= plan.Deadline || (plan.AuxBaseline < 2 && !pTransport))
				{
					finishPlan(NeuralPlanId::EngineerAssault, false, true);
				}
				else if (plan.AuxBaseline == 0)
				{
					int waiting = 0;

					for (const DWORD uid : engineers)
					{
						if (auto const pEng = findOwnFoot(uid))
						{
							waiting++;
							OrderFootToEnter(pEng, pTransport);
						}
					}

					if (pTransport->Passengers.NumPassengers > 0 && (waiting == 0 || Unsorted::CurrentFrame - plan.StartFrame >= 900))
						plan.AuxBaseline = 1;
					else if (pTransport->Passengers.NumPassengers == 0 && Unsorted::CurrentFrame - plan.StartFrame >= 1500)
						finishPlan(NeuralPlanId::EngineerAssault, false, true);
				}
				else if (plan.AuxBaseline == 1)
				{
					if (pTransport->Passengers.NumPassengers == 0)
					{
						plan.AuxBaseline = 2;
					}
					else if (pTransport->GetMapCoords().DistanceFrom(pTarget->GetMapCoords()) <= 3.0
						|| pTransport->GetHealthPercentage() < 0.3)
					{
						if (pTransport->CurrentMission != Mission::Unload)
							pTransport->QueueMission(Mission::Unload, true);
					}
					else
					{
						IssueMoveOrder(pTransport, pTarget->GetCell(), 1.5);
					}
				}
				else
				{
					int alive = 0;

					for (const DWORD uid : engineers)
					{
						if (auto const pEng = findOwnFoot(uid))
						{
							alive++;
							OrderFootToEnter(pEng, pTarget);
						}
					}

					if (alive == 0 && pTarget->Owner != pHouse)
						finishPlan(NeuralPlanId::EngineerAssault, false, true);
				}
			}
			else if (planReady(NeuralPlanId::EngineerAssault))
			{
				FootClass* pTransport = nullptr;

				for (auto const& [uid, pFoot] : ownFoots)
				{
					auto const pUnit = abstract_cast<UnitClass*>(pFoot);

					if (pUnit && pUnit->Type && !pUnit->Type->Naval && pUnit->Type->Passengers >= 2 && !pUnit->Type->OpenTopped
						&& !pUnit->Type->Gunner && !IsAmphibiousOrAirTransport(pUnit->Type) && pUnit->Passengers.NumPassengers == 0
						&& !HasNoManualEnter(pUnit->Type) && !HasNoManualUnload(pUnit->Type) && isFreeForPlan(pUnit)
						&& ObjectZone(pUnit, MovementZone::Normal) == homeZone && (!pTransport || uid < pTransport->UniqueID))
					{
						pTransport = pUnit;
					}
				}

				std::vector<FootClass*> engineers;

				if (pTransport)
				{
					for (auto const& [uid, pFoot] : ownFoots)
					{
						auto const pInf = abstract_cast<InfantryClass*>(pFoot);

						if (pInf && pInf->Type && pInf->Type->Engineer && isFreeForPlan(pInf) && ObjectZone(pInf, MovementZone::Normal) == homeZone)
							engineers.push_back(pInf);
					}

					std::sort(engineers.begin(), engineers.end(), [](FootClass* a, FootClass* b) { return a->UniqueID < b->UniqueID; });
					engineers.resize(std::min<size_t>(engineers.size(), std::min(3, pTransport->GetTechnoType()->Passengers)));
				}

				BuildingClass* pTarget = nullptr;
				int bestScore = INT_MIN;

				if (!engineers.empty())
				{
					for (auto const pBld : knownEnemyBuildingList)
					{
						if (!isKnownEnemyBuilding(pBld) || !pBld->Type->Capturable || ObjectZone(pBld, MovementZone::Normal) != homeZone)
							continue;

						const bool capturableNow = captureLevel >= 0.99 || pBld->GetHealthPercentage() <= captureLevel + 0.05;
						const int worth = valuableBuildingWorth(pBld->Type) + (pBld->Type->ConstructionYard ? 300 : 0);
						const int defenses = CountKnownEnemyDefensesNear(pHouse, pBld->GetMapCoords(), 8.0, false);

						if (!capturableNow || worth <= 0 || defenses > 2)
							continue;

						const int score = worth - defenses * 80 - static_cast<int>(pBld->GetMapCoords().DistanceFrom(homeCell));

						if (score > bestScore)
						{
							bestScore = score;
							pTarget = pBld;
						}
					}
				}

				const int utility = pTarget ? (300 + bestScore / 4) : -1000;

				if (utility > 0 && shouldTryStart(NeuralPlanId::EngineerAssault, utility))
				{
					std::vector<FootClass*> group { pTransport };
					group.insert(group.end(), engineers.begin(), engineers.end());
					startPlan(NeuralPlanId::EngineerAssault, group, pTarget->GetMapCoords(), CellStruct::Empty, pTarget->UniqueID, 4500);
					planOf(NeuralPlanId::EngineerAssault).AuxBaseline = 0;
				}
			}
		}

		// ---------------------------------------------------------------- Plan 25: Sabotage
		// Right as a wave starts (points 23/55), knock out the enemy radar or the power feeding
		// the defenses near the wave target: a spy infiltrates it when we have one, else aircraft
		// or a fast group strike it, so the wave arrives with the enemy blind or unpowered.
		{
			auto& plan = planOf(NeuralPlanId::Sabotage);

			if (plan.Active)
			{
				auto const pTarget = FindBuildingByID(plan.TargetID);
				bool spyLeft = false;

				for (const DWORD uid : plan.Members)
				{
					if (auto const pFoot = findOwnFoot(uid))
					{
						if (auto const pInf = abstract_cast<InfantryClass*>(pFoot); pInf && IsSpyInfantryType(pInf->Type))
							spyLeft = true;
					}
				}

				if (!pTarget)
				{
					finishPlan(NeuralPlanId::Sabotage, true, true);
				}
				else if (plan.AuxBaseline == 1 && !spyLeft)
				{
					// The spy went in (or died at the door): infiltration counts as done.
					finishPlan(NeuralPlanId::Sabotage, true, true);
				}
				else if (Unsorted::CurrentFrame >= plan.Deadline || memberValue(plan) <= 0)
				{
					finishPlan(NeuralPlanId::Sabotage, false, true);
				}
				else if (plan.AuxBaseline == 1)
				{
					for (const DWORD uid : plan.Members)
					{
						if (auto const pSpy = findOwnFoot(uid))
							OrderFootToEnter(pSpy, pTarget);
					}
				}
				else
				{
					strikeWithMembers(plan.Members, pTarget);
				}
			}
			else if (planReady(NeuralPlanId::Sabotage) && macro.LastWaveTargetCell != CellStruct::Empty
				&& Unsorted::CurrentFrame - macro.WaveStartFrame <= 600)
			{
				BuildingClass* pTarget = nullptr;
				int bestScore = INT_MIN;

				for (auto const pBld : knownEnemyBuildingList)
				{
					if (!isKnownEnemyBuilding(pBld) || (!pBld->Type->Radar && pBld->Type->PowerBonus <= 0))
						continue;

					const CellStruct bc = pBld->GetMapCoords();
					const double d = bc.DistanceFrom(macro.LastWaveTargetCell);

					if (d > 25.0)
						continue;

					const int score = (pBld->Type->Radar ? 200 : pBld->Type->PowerBonus / 2) - static_cast<int>(d) * 4;

					if (score > bestScore)
					{
						bestScore = score;
						pTarget = pBld;
					}
				}

				std::vector<FootClass*> group;
				bool bySpy = false;

				if (pTarget && pTarget->Type->Spyable)
				{
					for (auto const& [uid, pFoot] : ownFoots)
					{
						auto const pInf = abstract_cast<InfantryClass*>(pFoot);

						if (pInf && IsSpyInfantryType(pInf->Type) && isFreeForPlan(pInf) && ObjectZone(pInf, MovementZone::Normal) == ObjectZone(pTarget, MovementZone::Normal)
							&& (group.empty() || uid < group.front()->UniqueID))
						{
							group.assign(1, pInf);
							bySpy = true;
						}
					}
				}

				if (pTarget && group.empty())
					group = pickStrikeGroup(pTarget, 3, 4);

				const int utility = (pTarget && !group.empty()) ? (330 + (bySpy ? 80 : 0)) : -1000;

				if (utility > 0 && shouldTryStart(NeuralPlanId::Sabotage, utility))
				{
					startPlan(NeuralPlanId::Sabotage, group, pTarget->GetMapCoords(), CellStruct::Empty, pTarget->UniqueID, 2400);
					planOf(NeuralPlanId::Sabotage).AuxBaseline = bySpy ? 1 : 0;

					if (bySpy)
						OrderFootToEnter(group.front(), pTarget);
					else
						strikeWithMembers(planOf(NeuralPlanId::Sabotage).Members, pTarget);
				}
			}
		}

		// ---------------------------------------------------------------- Plan 26: Hunt Key Units
		// A visible high-value enemy unit (mind controller, heavy artillery, commando-class or
		// very expensive unit) with a light escort is hunted by a fast group or aircraft while
		// visible; out of sight they go to where it was last seen. Success when it dies.
		{
			auto& plan = planOf(NeuralPlanId::HuntKeyUnits);

			auto keyUnitValue = [](TechnoClass* pTech) -> int
			{
				auto const pType = pTech->GetTechnoType();

				if (!pType || pTech->WhatAmI() == AbstractType::Building)
					return 0;

				int value = 0;

				for (int i = 0; i < 2; ++i)
				{
					auto const pWS = pType->GetWeapon(i);
					auto const pW = pWS ? pWS->WeaponType : nullptr;

					if (pW && pW->Warhead && pW->Warhead->MindControl)
						value += 800;

					if (pW && pW->Damage > 0 && pW->Range >= 10 * Unsorted::LeptonsPerCell && (!pW->Projectile || pW->Projectile->AG))
						value += 300;
				}

				if (pType->Cost >= 1500)
					value += pType->Cost / 3;
				else if (pTech->WhatAmI() == AbstractType::Infantry && pType->Cost >= 1000)
					value += 400;

				return value;
			};

			if (plan.Active)
			{
				auto const pTarget = findTechnoByID(plan.TargetID);
				const int strength = groupStrength(plan.Members);
				// Tracked while its cell is known (IsCellKnownToAI), as the match's vision rule shows it.
				const bool targetSeen = pTarget && pTarget->CloakState == CloakState::Uncloaked
					&& IsCellKnownToAI(pHouse, pTarget->GetMapCoords());

				if (targetSeen)
				{
					plan.TargetCell = pTarget->GetMapCoords();
					plan.TargetSeenFrame = Unsorted::CurrentFrame;
				}

				const CellStruct center = groupCentroid(plan.Members);

				if (!pTarget)
				{
					finishPlan(NeuralPlanId::HuntKeyUnits, true, true);
				}
				else if (memberValue(plan) * 100 < plan.InitialValue * 45 || strength * 100 < plan.InitialStrength * 30)
				{
					abortPlan(NeuralPlanId::HuntKeyUnits, NeuralDecision::PlanAbortValueLost, strength);
				}
				else if (seenEnemyValueNear(plan.TargetCell, 7.0, 150) * 10 > strength * 15
					|| CountKnownEnemyDefensesNear(pHouse, plan.TargetCell, 7.0, false) > 0)
				{
					// Its escort grew past the group, or it ran under its defenses.
					abortPlan(NeuralPlanId::HuntKeyUnits, NeuralDecision::PlanAbortEnemyGrew, strength);
				}
				else if (!targetSeen && (Unsorted::CurrentFrame - plan.TargetSeenFrame >= 1350
					|| (Unsorted::CurrentFrame - plan.TargetSeenFrame >= 300 && center != CellStruct::Empty
						&& center.DistanceFrom(plan.TargetCell) <= 5.0)))
				{
					// Lost from sight and not where it was last seen.
					abortPlan(NeuralPlanId::HuntKeyUnits, NeuralDecision::PlanAbortTargetGone, strength);
				}
				else if (Unsorted::CurrentFrame >= plan.Deadline)
				{
					finishPlan(NeuralPlanId::HuntKeyUnits, false, true);
				}
				else if (targetSeen)
				{
					attackTargetGroup(plan.Members, pTarget);
				}
				else
				{
					attackMoveGroup(plan.Members, MapClass::Instance.TryGetCellAt(plan.TargetCell));
				}
			}
			else if (planReady(NeuralPlanId::HuntKeyUnits))
			{
				TechnoClass* pBest = nullptr;
				int bestScore = 0;

				for (auto const pTech : TechnoClass::Array)
				{
					if (!pTech || !pTech->IsAlive || pTech->InLimbo || !pTech->Owner || pTech->Owner == pHouse
						|| pHouse->IsAlliedWith(pTech->Owner) || pTech->Owner->IsNeutral() || pTech->CloakState != CloakState::Uncloaked
						|| !IsCellKnownToAI(pHouse, pTech->GetMapCoords()))
					{
						continue;
					}

					const int value = keyUnitValue(pTech);
					const CellStruct tc = pTech->GetMapCoords();

					if (value <= 0 || tc.DistanceFrom(homeCell) > 45.0 || seenEnemyValueNear(tc, 7.0, 150) > 400
						|| CountKnownEnemyDefensesNear(pHouse, tc, 8.0, false) > 0)
					{
						continue;
					}

					const int score = value - static_cast<int>(tc.DistanceFrom(homeCell)) * 5;

					if (score > bestScore)
					{
						bestScore = score;
						pBest = pTech;
					}
				}

				std::vector<FootClass*> group;

				if (pBest)
				{
					const bool airborneTarget = IsAirborneThreatTechno(pBest);

					if (!airborneTarget && knownAARiskAt(pBest->GetMapCoords()) <= 1)
						group = collectAirStrikers();

					if (group.size() < 2)
					{
						group = collectIdleGround(airborneTarget);

						// A ground target off our landmass cannot be reached by the ground group.
						if (!airborneTarget && ObjectZone(pBest, MovementZone::Normal) != homeZone)
							group.clear();

						sortFastest(group);
						group.resize(std::min<size_t>(group.size(), 6));
						int huntStrength = 0;

						for (auto const pFoot : group)
							huntStrength += static_cast<int>(GetCombatValueOfType(pFoot->GetTechnoType(), false) * pFoot->GetHealthPercentage());

						// 1.3x what the enemy has around the target (10 cells: the escort we will meet on the
						// way in, not only what stands next to it), health weighted; the run aborts when the
						// escort grows past 1.5x the group.
						if (group.size() < 3 || huntStrength * 10 < seenEnemyValueNear(pBest->GetMapCoords(), 10.0, 300) * 13)
							group.clear();
					}
					else
					{
						group.resize(std::min<size_t>(group.size(), 4));
					}
				}

				const int utility = !group.empty() ? (300 + std::min(300, bestScore / 4)) : -1000;

				if (utility > 0 && shouldTryStart(NeuralPlanId::HuntKeyUnits, utility))
				{
					startPlan(NeuralPlanId::HuntKeyUnits, group, pBest->GetMapCoords(), CellStruct::Empty, pBest->UniqueID, 2700);
					attackTargetGroup(planOf(NeuralPlanId::HuntKeyUnits).Members, pBest);
				}
			}
		}

		// ---------------------------------------------------------------- Plan 27: Rotating Raids
		// Two small fast groups raid two lightly defended known enemy buildings far apart and
		// switch to new targets every 900 frames (or once theirs falls), so the defender is
		// always pulled two ways. AuxBaseline = size of group A, AuxPeak = next switch frame.
		{
			auto& plan = planOf(NeuralPlanId::RotatingRaids);

			// A building our wave or another active plan of ours goes for (RaidClaimedCells).
			auto claimedByOurAttack = [&](BuildingClass* pBld, const CellStruct& bc) -> bool
			{
				if (macro.LastWaveTargetCell != CellStruct::Empty && Unsorted::CurrentFrame - macro.LastWaveDispatchFrame <= 3600
					&& bc.DistanceFrom(macro.LastWaveTargetCell) <= RaidClaimedCells)
				{
					return true;
				}

				for (size_t i = 0; i < macro.Plans.size(); ++i)
				{
					auto const& other = macro.Plans[i];

					if (!other.Active || i == static_cast<size_t>(NeuralPlanId::RotatingRaids))
						continue;

					if (other.TargetID == pBld->UniqueID || std::find(other.Targets.begin(), other.Targets.end(), pBld->UniqueID) != other.Targets.end()
						|| (other.TargetCell != CellStruct::Empty && bc.DistanceFrom(other.TargetCell) <= RaidClaimedCells))
					{
						return true;
					}
				}

				return false;
			};

			auto pickRaidTarget = [&](const CellStruct& avoidA, const CellStruct& avoidB) -> BuildingClass*
			{
				BuildingClass* pBest = nullptr;
				int bestScore = INT_MIN;

				for (auto const pBld : knownEnemyBuildingList)
				{
					if (!isKnownEnemyBuilding(pBld) || pBld->Type->IsBaseDefense)
						continue;

					const CellStruct bc = pBld->GetMapCoords();

					if (zoneOf(bc) != homeZone || (avoidA != CellStruct::Empty && bc.DistanceFrom(avoidA) < 15.0)
						|| (avoidB != CellStruct::Empty && bc.DistanceFrom(avoidB) < 15.0) || claimedByOurAttack(pBld, bc))
					{
						continue;
					}

					const int defenses = CountKnownEnemyDefensesNear(pHouse, bc, 9.0, false);

					if (defenses > 1)
						continue;

					const int score = 100 + valuableBuildingWorth(pBld->Type) / 2 - defenses * 120 - static_cast<int>(bc.DistanceFrom(homeCell));

					if (score > bestScore)
					{
						bestScore = score;
						pBest = pBld;
					}
				}

				return pBest;
			};

			if (plan.Active)
			{
				const size_t split = std::min<size_t>(plan.Members.size(), static_cast<size_t>(std::max(0, plan.AuxBaseline)));
				const std::vector<DWORD> groupA(plan.Members.begin(), plan.Members.begin() + split);
				const std::vector<DWORD> groupB(plan.Members.begin() + split, plan.Members.end());
				const int value = memberValue(plan);

				if (value * 100 < plan.InitialValue * 40)
				{
					finishPlan(NeuralPlanId::RotatingRaids, false, true);
				}
				else if (Unsorted::CurrentFrame >= plan.Deadline)
				{
					finishPlan(NeuralPlanId::RotatingRaids, value * 100 >= plan.InitialValue * 50, true);
				}
				else
				{
					auto const pA = MapClass::Instance.TryGetCellAt(plan.TargetCell);
					auto const pB = MapClass::Instance.TryGetCellAt(plan.StagingCell);
					const bool aFallen = !pA || !pA->GetBuilding();
					const bool bFallen = !pB || !pB->GetBuilding();

					// The switch clock starts once a group reached its target (AuxPeak -1 until then): the
					// old one ran from the start, and the groups were sent elsewhere before they got
					// anywhere (no raid of 2026-11-17 arrived).
					if (plan.AuxPeak < 0)
					{
						const CellStruct centreA = groupCentroid(groupA);
						const CellStruct centreB = groupCentroid(groupB);

						if ((centreA != CellStruct::Empty && centreA.DistanceFrom(plan.TargetCell) <= RaidArrivedCells)
							|| (centreB != CellStruct::Empty && centreB.DistanceFrom(plan.StagingCell) <= RaidArrivedCells))
						{
							plan.AuxPeak = Unsorted::CurrentFrame + RaidSwitchFrames;

							if (!plan.Arrived)
							{
								plan.Arrived = true;
								plan.ArrivedFrame = Unsorted::CurrentFrame;
							}
						}
					}

					if ((plan.AuxPeak >= 0 && Unsorted::CurrentFrame >= plan.AuxPeak) || aFallen || bFallen)
					{
						auto const pNewA = pickRaidTarget(plan.TargetCell, plan.StagingCell);
						auto const pNewB = pNewA ? pickRaidTarget(pNewA->GetMapCoords(), plan.TargetCell) : nullptr;

						if (!pNewA)
						{
							// No target left: it went well only if it destroyed more than it lost (66 of
							// these ends counted as well, none had arrived, most destroyed nothing). One
							// that destroyed and lost nothing saw its targets fall to others: no defeat.
							const int lostValue = std::max(0, plan.InitialValue - value);
							plan.Neutral = plan.KillValue == 0 && lostValue == 0;
							finishPlan(NeuralPlanId::RotatingRaids, plan.KillValue > lostValue, true);
						}
						else
						{
							// Groups that joined (or are joining) after one was outgunned keep one target.
							const bool oneGroup = plan.RegroupFrame >= 0 || plan.AuxBaseline >= static_cast<int>(plan.Members.size());
							plan.TargetCell = pNewA->GetMapCoords();
							plan.StagingCell = (pNewB && !oneGroup) ? pNewB->GetMapCoords() : pNewA->GetMapCoords();
							plan.AuxPeak = -1;

							// The attack-move to the old target is dropped too: attackMoveGroup leaves a unit
							// already on an attack-move alone, so the members went on to the old cell and took
							// the new one only once they got there.
							for (const DWORD uid : plan.Members)
							{
								if (auto const pFoot = findOwnFoot(uid))
								{
									pFoot->SetTarget(nullptr);

									if (pFoot->MegaMission == Mission::AttackMove)
										pFoot->ClearMegaMissionData();
								}
							}
						}
					}

					// The outgunned group falls back to the other one (RaidRegroupCells): the holders (group A)
					// stop where they are and still fight what comes at them, the other group moves to
					// them, not attack-moving, so it leaves the fight it was losing. Once together, or after
					// RaidRegroupFrames, they are one group.
					if (plan.Active && plan.RegroupFrame >= 0)
					{
						const CellStruct centreA = groupCentroid(groupA);
						const CellStruct centreB = groupCentroid(groupB);
						const int apart = (centreA != CellStruct::Empty && centreB != CellStruct::Empty) ? static_cast<int>(centreA.DistanceFrom(centreB)) : -1;

						if (apart < 0 || apart <= RaidRegroupCells || Unsorted::CurrentFrame - plan.RegroupFrame >= RaidRegroupFrames)
						{
							Debug::Log("[NeuralAI] House %d plan rotatingRaids groups joined after %d frames, %d cells apart: %d members go on to (%d,%d), frame %d.\n",
								houseIdx, Unsorted::CurrentFrame - plan.RegroupFrame, apart, static_cast<int>(plan.Members.size()), plan.TargetCell.X,
								plan.TargetCell.Y, Unsorted::CurrentFrame);
							plan.RegroupFrame = -1;
							plan.AuxBaseline = static_cast<int>(plan.Members.size());
							plan.StagingCell = plan.TargetCell;
						}
						else
						{
							for (const DWORD uid : groupA)
							{
								auto const pFoot = findOrderableFoot(uid);

								if (pFoot && pFoot->Target == nullptr && (pFoot->Destination || pFoot->MegaDestination))
								{
									pFoot->SetDestination(nullptr, true);
									pFoot->ClearMegaMissionData();
									pFoot->QueueMission(Mission::Guard, false);
								}
							}

							moveGroup(groupB, MapClass::Instance.TryGetCellAt(centreA), 4.0);
						}
					}

					// Each group marches as one column to its own target (columnMovingOn): the fastest
					// members ran ahead alone, in 4 of 7 ends of 2026-11-18 the dead were last seen 35 to 99
					// cells nearer the target than their group, which saw no enemy in 9 of 11 turn backs.
					if (plan.Active && plan.RegroupFrame < 0)
					{
						attackMoveGroup(columnMovingOn(groupA, plan.TargetCell), MapClass::Instance.TryGetCellAt(plan.TargetCell));
						attackMoveGroup(columnMovingOn(groupB, plan.StagingCell), MapClass::Instance.TryGetCellAt(plan.StagingCell));
					}
				}
			}
			else if (Unsorted::CurrentFrame >= 6000 && (macro.RaidRecruitFrame >= 0 || planReady(NeuralPlanId::RotatingRaids)))
			{
				// The recruits still with us (RaidRecruits) and the idle ground units, recruits left out
				// (they count as plan members).
				std::vector<FootClass*> recruits;

				for (const DWORD uid : macro.RaidRecruits)
				{
					if (auto const pFoot = findOwnFoot(uid))
						recruits.push_back(pFoot);
				}

				std::vector<FootClass*> group = collectIdleGround(false);
				sortFastest(group);
				auto const pTargetA = pickRaidTarget(CellStruct::Empty, CellStruct::Empty);
				auto const pTargetB = pTargetA ? pickRaidTarget(pTargetA->GetMapCoords(), CellStruct::Empty) : nullptr;
				const int targetOwner = (pTargetA && pTargetA->Owner) ? pTargetA->Owner->ArrayIndex : -1;
				const size_t raidSize = rotatingRaidsSize(targetOwner);
				const bool haveTargets = pTargetA && pTargetB;
				const bool lesson = raidSize > static_cast<size_t>(RaidBaseGroup);
				bool setOut = false;

				auto const stopRecruiting = [&](const char* why)
				{
					if (macro.RaidRecruitFrame >= 0)
					{
						Debug::Log("[NeuralAI] House %d plan rotatingRaids stops recruiting (%s) after %d frames with %d of %d, frame %d.\n", houseIdx, why,
							Unsorted::CurrentFrame - macro.RaidRecruitFrame, static_cast<int>(recruits.size()), static_cast<int>(raidSize), Unsorted::CurrentFrame);
					}

					macro.RaidRecruits.clear();
					macro.RaidRecruitFrame = -1;
					macro.RaidRecruitWanted = 0;
				};

				auto const recruitFrom = [&](const std::vector<FootClass*>& idle)
				{
					for (auto const pFoot : idle)
					{
						if (recruits.size() >= raidSize)
							break;

						recruits.push_back(pFoot);
					}
				};

				if (macro.RaidRecruitFrame >= 0)
				{
					// Gathering: the new idle units join and the raid sets out once full, or late (or
					// once the lesson faded) with RaidBaseGroup at least; no targets left ends it.
					if (!haveTargets)
					{
						stopRecruiting("no targets");
						recruits.clear();
					}
					else
					{
						recruitFrom(group);
						setOut = recruits.size() >= raidSize || ((!lesson || Unsorted::CurrentFrame - macro.RaidRecruitFrame >= RaidRecruitFrames)
							&& recruits.size() >= static_cast<size_t>(RaidBaseGroup));

						if (!setOut && !lesson)
						{
							stopRecruiting("the lesson faded");
							recruits.clear();
						}
					}
				}
				else
				{
					// After a lesson the bigger group is recruited (RaidRecruitFrames), not waited for.
					const int utility = (haveTargets && (lesson || group.size() >= raidSize)) ? 280 : -1000;

					if (utility > 0 && shouldTryStart(NeuralPlanId::RotatingRaids, utility))
					{
						recruits.clear();
						recruitFrom(group);
						setOut = recruits.size() >= raidSize;

						if (!setOut)
						{
							macro.RaidRecruitFrame = Unsorted::CurrentFrame;
							Debug::Log("[NeuralAI] House %d plan rotatingRaids recruits %d units against house %d (lesson), %d idle now, frame %d.\n", houseIdx,
								static_cast<int>(raidSize), targetOwner, static_cast<int>(recruits.size()), Unsorted::CurrentFrame);
						}
					}
				}

				if (setOut)
				{
					const int recruitFrames = macro.RaidRecruitFrame >= 0 ? Unsorted::CurrentFrame - macro.RaidRecruitFrame : -1;
					std::vector<FootClass*> members = recruits;
					sortFastest(members);
					members.resize(std::min(members.size(), raidSize));
					stopRecruiting("set out");
					startPlan(NeuralPlanId::RotatingRaids, members, pTargetA->GetMapCoords(), pTargetB->GetMapCoords(), 0, 5400);

					if (auto& raid = planOf(NeuralPlanId::RotatingRaids); raid.Active)
					{
						raid.AuxBaseline = static_cast<int>(members.size() / 2);
						raid.AuxPeak = -1;
						raid.RegroupFrame = -1;
						raid.TargetOwner = targetOwner;

						if (recruitFrames >= 0)
						{
							Debug::Log("[NeuralAI] House %d plan rotatingRaids sets out with %d of %d recruited in %d frames, frame %d.\n", houseIdx,
								static_cast<int>(members.size()), static_cast<int>(raidSize), recruitFrames, Unsorted::CurrentFrame);
						}
					}
				}
				else if (macro.RaidRecruitFrame >= 0)
				{
					// The recruits are kept out of the waves and the other plans and wait at the rally point.
					macro.RaidRecruits.clear();

					for (auto const pFoot : recruits)
					{
						macro.RaidRecruits.push_back(pFoot->UniqueID);
						macro.PlanMembers.insert(pFoot->UniqueID);
					}

					macro.RaidRecruitWanted = static_cast<int>(raidSize - std::min(raidSize, recruits.size()));
					moveGroup(macro.RaidRecruits, MapClass::Instance.TryGetCellAt(homeCell), 6.0);
				}
			}
			else if (macro.RaidRecruitFrame < 0)
			{
				macro.RaidRecruitWanted = 0;
			}
		}

		// ---------------------------------------------------------------- Plan 28: Air Corridor
		// Our flyers keep dying in the same places (air danger memory): a ground group, or
		// aircraft when the AA there is light, destroys the known enemy anti-air defense sitting
		// next to those spots. Once it falls, the danger spots around it are cleared, so air
		// strikes, carryalls and air transports get their lane back.
		{
			auto& plan = planOf(NeuralPlanId::AirCorridor);

			auto spotsNear = [&](const CellStruct& cell, double radius) -> int
			{
				int count = 0;

				for (auto const& [spot, frame] : macro.AirDangerSpots)
				{
					if (spot.DistanceFrom(cell) <= radius)
						count++;
				}

				return count;
			};

			if (plan.Active)
			{
				auto const pTarget = FindBuildingByID(plan.TargetID);

				if (!pTarget)
				{
					std::erase_if(macro.AirDangerSpots, [&](const auto& spot) { return spot.first.DistanceFrom(plan.TargetCell) <= 10.0; });
					finishPlan(NeuralPlanId::AirCorridor, true, true);
				}
				else if (memberValue(plan) * 100 < plan.InitialValue * 40 || Unsorted::CurrentFrame >= plan.Deadline)
				{
					finishPlan(NeuralPlanId::AirCorridor, false, true);
				}
				else
				{
					strikeWithMembers(plan.Members, pTarget);
				}
			}
			else if (planReady(NeuralPlanId::AirCorridor) && !macro.AirDangerSpots.empty())
			{
				BuildingClass* pTarget = nullptr;
				int bestScore = 0;

				for (auto const pBld : knownEnemyBuildingList)
				{
					if (!isKnownEnemyBuilding(pBld) || !pBld->Type->IsBaseDefense || GetCombatValueOfType(pBld->Type, true) <= 0)
						continue;

					const CellStruct bc = pBld->GetMapCoords();
					const int spots = spotsNear(bc, 10.0);

					if (spots <= 0)
						continue;

					const int score = spots * 100 - static_cast<int>(bc.DistanceFrom(homeCell));

					if (score > bestScore)
					{
						bestScore = score;
						pTarget = pBld;
					}
				}

				std::vector<FootClass*> group = pickStrikeGroup(pTarget, 4, 8);
				const int utility = (pTarget && !group.empty()) ? (300 + std::min(200, spotsNear(pTarget->GetMapCoords(), 10.0) * 60)) : -1000;

				if (utility > 0 && shouldTryStart(NeuralPlanId::AirCorridor, utility))
				{
					startPlan(NeuralPlanId::AirCorridor, group, pTarget->GetMapCoords(), CellStruct::Empty, pTarget->UniqueID, 3600);
					strikeWithMembers(planOf(NeuralPlanId::AirCorridor).Members, pTarget);
				}
			}
		}

		// ---------------------------------------------------------------- Plan 29: Silent Infiltration
		// Cloakable or tunneling (subterranean) armed units slip into the enemy rear and destroy
		// a power, tech or refinery building with no known sensor building near it.
		{
			auto& plan = planOf(NeuralPlanId::SilentInfiltration);

			if (plan.Active)
			{
				auto const pTarget = FindBuildingByID(plan.TargetID);

				if (!pTarget)
					finishPlan(NeuralPlanId::SilentInfiltration, true, true);
				else if (memberValue(plan) * 100 < plan.InitialValue * 40 || Unsorted::CurrentFrame >= plan.Deadline)
					finishPlan(NeuralPlanId::SilentInfiltration, false, true);
				else
					strikeWithMembers(plan.Members, pTarget);
			}
			else if (planReady(NeuralPlanId::SilentInfiltration))
			{
				std::vector<FootClass*> stealthy;

				for (auto const pFoot : collectIdleGround(false))
				{
					auto const pType = pFoot->GetTechnoType();

					if (pType->Cloakable || pType->MovementZone == MovementZone::Subterrannean)
						stealthy.push_back(pFoot);
				}

				BuildingClass* pTarget = nullptr;
				int bestScore = INT_MIN;

				if (stealthy.size() >= 2)
				{
					for (auto const pBld : knownEnemyBuildingList)
					{
						if (!isKnownEnemyBuilding(pBld) || pBld->Type->IsBaseDefense
							|| (pBld->Type->PowerBonus <= 0 && !pBld->Type->Radar && !pBld->Type->Refinery && pBld->Type->SuperWeapon == -1))
						{
							continue;
						}

						const CellStruct bc = pBld->GetMapCoords();
						bool watched = false;

						for (auto const pSensor : knownEnemyBuildingList)
						{
							if (isKnownEnemyBuilding(pSensor) && pSensor->Type->SensorArray && pSensor->GetMapCoords().DistanceFrom(bc) <= 10.0)
							{
								watched = true;
								break;
							}
						}

						if (watched || zoneOf(bc) != homeZone || CountKnownEnemyDefensesNear(pHouse, bc, 8.0, false) > 1)
							continue;

						// Deep targets: the rear, away from where we usually arrive.
						const int score = valuableBuildingWorth(pBld->Type) + static_cast<int>(bc.DistanceFrom(homeCell));

						if (score > bestScore)
						{
							bestScore = score;
							pTarget = pBld;
						}
					}
				}

				const int utility = pTarget ? (300 + bestScore / 8) : -1000;

				if (utility > 0 && shouldTryStart(NeuralPlanId::SilentInfiltration, utility))
				{
					stealthy.resize(std::min<size_t>(stealthy.size(), 5));
					startPlan(NeuralPlanId::SilentInfiltration, stealthy, pTarget->GetMapCoords(), CellStruct::Empty, pTarget->UniqueID, 4500);
				}
			}
		}

		// ---------------------------------------------------------------- Plan 30: Artillery vs Economy
		// Artillery that outranges the defenses around a known enemy refinery sets up out of
		// their reach and shells the refinery (and the harvesters docking at it).
		{
			auto& plan = planOf(NeuralPlanId::ArtilleryEcoSiege);

			if (plan.Active)
			{
				auto const pTarget = FindBuildingByID(plan.TargetID);
				auto const pStage = MapClass::Instance.TryGetCellAt(plan.StagingCell);

				if (!pTarget)
				{
					finishPlan(NeuralPlanId::ArtilleryEcoSiege, true, true);
				}
				else if (!pStage || memberValue(plan) * 100 < plan.InitialValue * 40 || Unsorted::CurrentFrame >= plan.Deadline)
				{
					finishPlan(NeuralPlanId::ArtilleryEcoSiege, false, true);
				}
				else
				{
					for (const DWORD uid : plan.Members)
					{
						if (handleSiegeDeployMember(uid, pTarget, pStage))
							continue;

						auto const pFoot = findOwnFoot(uid);

						if (!pFoot)
							continue;

						const bool inPlace = pFoot->GetMapCoords().DistanceFrom(plan.StagingCell) <= 3.0;
						const bool isArtillery = SiegeRangeOf(pFoot->GetTechnoType()) >= 8 * Unsorted::LeptonsPerCell;

						// The stage is measured from the refinery's corner, so a gun at the stage can be a
						// cell or two short: in place, artillery gets the attack order and closes in by
						// itself.
						if (inPlace && isArtillery)
						{
							if (pFoot->Target != pTarget)
							{
								pFoot->SetTarget(pTarget);
								pFoot->QueueMission(Mission::Attack, true);
							}
						}
						else if (!inPlace && pFoot->Target == nullptr)
						{
							IssueMoveOrder(pFoot, pStage, 2.0);
						}
					}
				}
			}
			else if (planReady(NeuralPlanId::ArtilleryEcoSiege))
			{
				std::vector<FootClass*> artillery;
				std::vector<FootClass*> escorts;
				int artilleryRange = INT_MAX;

				for (auto const pFoot : collectIdleGround(false, true))
				{
					const int range = SiegeRangeOf(pFoot->GetTechnoType());

					if (range >= 8 * Unsorted::LeptonsPerCell)
					{
						artillery.push_back(pFoot);
						artilleryRange = std::min(artilleryRange, range);
					}
					else if (escorts.size() < 3)
					{
						escorts.push_back(pFoot);
					}
				}

				BuildingClass* pTarget = nullptr;
				CellClass* pStage = nullptr;
				double bestDist = 1e9;

				if (artillery.size() >= 2)
				{
					const double standOff = artilleryRange / static_cast<double>(Unsorted::LeptonsPerCell) - 1.0;

					for (auto const pBld : knownEnemyBuildingList)
					{
						if (!isKnownEnemyBuilding(pBld) || (!pBld->Type->Refinery && !pBld->Type->ResourceDestination))
							continue;

						const CellStruct bc = pBld->GetMapCoords();
						const double d = bc.DistanceFrom(homeCell);

						if (zoneOf(bc) != homeZone || d >= bestDist)
							continue;

						auto const pCell = landCellInZone(pointAlong(bc, homeCell, standOff, 0.0), homeZone, 2);

						if (pCell && !inKnownDefenseReach(pCell->MapCoords, nullptr))
						{
							bestDist = d;
							pTarget = pBld;
							pStage = pCell;
						}
					}
				}

				const int utility = pTarget ? 290 : -1000;

				if (utility > 0 && shouldTryStart(NeuralPlanId::ArtilleryEcoSiege, utility))
				{
					artillery.resize(std::min<size_t>(artillery.size(), 5));
					artillery.insert(artillery.end(), escorts.begin(), escorts.end());
					startPlan(NeuralPlanId::ArtilleryEcoSiege, artillery, pTarget->GetMapCoords(), pStage->MapCoords, pTarget->UniqueID, 5400);
				}
			}
		}

		// The next defense to shell (Rolling Siege, Siege Battery): a known enemy ground defense on
		// our landmass that the battery outranges by 2 cells, nearest to `from` within maxCells, with
		// a firing spot at its reach + 2 cells on our side that no other known defense reaches (and
		// the defense itself not within a cell of it), off any building. The first plan took the defense nearest a point 12
		// cells inside the enemy base and shelled it from wherever its own reach ended, often
		// under the guns of the outer ring: 30 of 40 plans lost a third of their value 10 to 25
		// cells from the defense and turned back, 2 ended well (archive of 2026-11-14 night).
		// A defense a group of this value failed against lately is left out (targetLessonAllows,
		// as for the other raids): the same one was taken three times in a row with the same
		// group and the same losses (match #9493).
		auto pickNextDefense = [&](const CellStruct& from, double maxCells, int artilleryRange, int value) -> std::pair<BuildingClass*, CellClass*>
		{
			std::vector<std::pair<double, BuildingClass*>> candidates;

			for (auto const pBld : knownEnemyBuildingList)
			{
				if (!isKnownEnemyBuilding(pBld) || !pBld->Type->IsBaseDefense)
					continue;

				const int defenseRange = GroundRangeOf(pBld->Type);
				const CellStruct bc = pBld->GetMapCoords();
				const double d = bc.DistanceFrom(from);

				if (defenseRange > 0 && artilleryRange >= defenseRange + 2 * Unsorted::LeptonsPerCell && d <= maxCells && zoneOf(bc) == homeZone
					&& targetLessonAllows(pBld->UniqueID, value))
				{
					candidates.emplace_back(d, pBld);
				}
			}

			// Nearest first (ties by id), so the reach of the other defenses is weighed only until a
			// spot is found.
			std::sort(candidates.begin(), candidates.end(), [](auto const& a, auto const& b)
				{
					return a.first < b.first || (a.first == b.first && a.second->UniqueID < b.second->UniqueID);
				});

			for (auto const& [d, pBld] : candidates)
			{
				const int defenseRange = GroundRangeOf(pBld->Type);
				const CellStruct bc = pBld->GetMapCoords();
				const double standOff = (defenseRange + 2 * Unsorted::LeptonsPerCell) / static_cast<double>(Unsorted::LeptonsPerCell) + 0.5;

				for (auto const& [towardHome, aside] : RollingSiegeStageDirections)
				{
					auto const pStage = landCellInZone(pointAlong(bc, homeCell, standOff * towardHome, standOff * aside), homeZone, 2);

					if (pStage && !pStage->GetBuilding() && pStage->MapCoords.DistanceFrom(bc) * Unsorted::LeptonsPerCell > defenseRange + Unsorted::LeptonsPerCell
						&& !inKnownDefenseReach(pStage->MapCoords, pBld))
					{
						return { pBld, pStage };
					}
				}
			}

			return { nullptr, nullptr };
		};

		auto groupArtilleryRange = [&](const std::vector<DWORD>& uids) -> int
		{
			int range = 0;

			for (const DWORD uid : uids)
			{
				TechnoTypeClass* pMemberType = nullptr;

				if (auto const pFoot = findOwnFoot(uid))
					pMemberType = pFoot->GetTechnoType();
				else if (auto const itBld = ownSiegeBuildings.find(uid); itBld != ownSiegeBuildings.end())
					pMemberType = itBld->second->Type;

				const int r = SiegeRangeOf(pMemberType);

				if (r >= 8 * Unsorted::LeptonsPerCell)
					range = range == 0 ? r : std::min(range, r);
			}

			return range;
		};

		// ---------------------------------------------------------------- Plan 31: Rolling Siege
		// Against a fortified base: artillery escorted by mobile anti-air and a few tanks takes
		// the known defenses out one by one, always from outside the reach of every known one,
		// starting with the one nearest home and moving on to the next defense nearest to the last
		// one (within RollingSiegeChainCells) until none is left.
		{
			auto& plan = planOf(NeuralPlanId::RollingSiege);

			if (plan.Active)
			{
				BuildingClass* pDefense = FindBuildingByID(plan.TargetID);
				const int artilleryRange = groupArtilleryRange(plan.Members);

				if (!pDefense)
					plan.Progress++; // The current defense fell.

				if (!pDefense && artilleryRange > 0)
				{
					auto const [pNext, pNextStage] = pickNextDefense(plan.TargetCell, RollingSiegeChainCells, artilleryRange, memberValue(plan));

					if (pNext)
					{
						plan.TargetID = pNext->UniqueID;
						plan.TargetCell = pNext->GetMapCoords();
						plan.StagingCell = pNextStage->MapCoords;
						pDefense = pNext;
					}
				}

				auto const pStage = MapClass::Instance.TryGetCellAt(plan.StagingCell);

				// Success = the chain ran out of defenses in reach, or at least two defenses fell before
				// the deadline / the battery was worn down (a fortified base never runs out of them).
				if (!pDefense || artilleryRange <= 0)
				{
					finishPlan(NeuralPlanId::RollingSiege, !pDefense || plan.Progress >= 2, true);
				}
				else if (!pStage || memberValue(plan) * 100 < plan.InitialValue * 35 || Unsorted::CurrentFrame >= plan.Deadline)
				{
					finishPlan(NeuralPlanId::RollingSiege, plan.Progress >= 2, true);
				}
				else
				{
					// The escorts wait behind the guns, out of the shelled defense's reach + 1.5 cells
					// and out of every other known one's, and an escort that comes into one (chasing
					// what came out of the base, or parked at the edge) drops its target and goes back
					// there. They stood around the spot itself, which is only 2.5 cells out of the
					// defense's reach, and were left to their own targets: in the focus batch of
					// 2026-11-15 141 of 342 escorts lost died within the reach of the defense being
					// shelled, 76 of them at its edge (within a cell) and 65 deeper in, 121 with enemy
					// units seen near.
					const int defenseReach = GroundRangeOf(pDefense->Type);
					auto inEscortDanger = [&](const CellStruct& cell)
					{
						return pDefense->GetMapCoords().DistanceFrom(cell) * Unsorted::LeptonsPerCell
							<= defenseReach + RollingSiegeEscortMarginCells * Unsorted::LeptonsPerCell || inKnownDefenseReach(cell, pDefense);
					};

					auto pEscortPost = landCellInZone(pointAlong(plan.StagingCell, homeCell, RollingSiegeEscortBackCells, 0.0), homeZone, 2);

					if (!pEscortPost || inEscortDanger(pEscortPost->MapCoords))
						pEscortPost = pStage;

					// The escorts and the guns march as one column to the spot (columnMovingOn).
					for (const DWORD uid : columnMovingOn(plan.Members, plan.StagingCell))
					{
						if (handleSiegeDeployMember(uid, pDefense, pStage))
							continue;

						auto const pFoot = findOwnFoot(uid);

						if (!pFoot)
							continue;

						// A gun stopped near its spot but inside the defense's reach (the spot is
						// 2.5 cells out of it, and a crowded spot leaves units up to 3 cells off) is
						// sent back to the spot before it fires.
						const bool isArtillery = SiegeRangeOf(pFoot->GetTechnoType()) >= 8 * Unsorted::LeptonsPerCell;
						const bool outOfReach = pFoot->DistanceFrom(pDefense) > GroundRangeOf(pDefense->Type) + Unsorted::LeptonsPerCell / 2;
						const bool inPlace = outOfReach && pFoot->GetMapCoords().DistanceFrom(plan.StagingCell) <= 3.0;

						if (isArtillery && inPlace)
						{
							if (pFoot->Target != pDefense)
							{
								pFoot->SetTarget(pDefense);
								pFoot->QueueMission(Mission::Attack, true);
							}
						}
						else if (isArtillery)
						{
							if (!outOfReach && pFoot->Target)
								pFoot->SetTarget(nullptr);

							IssueMoveOrder(pFoot, pStage, outOfReach ? 2.0 : 0.5);
						}
						else if (inEscortDanger(pFoot->GetMapCoords()))
						{
							if (pFoot->Target)
								pFoot->SetTarget(nullptr);

							IssueMoveOrder(pFoot, pEscortPost);
						}
						else if (pFoot->Target == nullptr)
						{
							// Escorts and AA stay with the battery, which moves on as one.
							IssueMoveOrder(pFoot, pEscortPost, RollingSiegeEscortSpreadCells);
						}
					}
				}
			}
			else if (planReady(NeuralPlanId::RollingSiege) && foe.KnownDefenses >= 4)
			{
				std::vector<FootClass*> artillery;
				std::vector<FootClass*> escorts;
				int artilleryRange = INT_MAX;

				for (auto const pFoot : collectIdleGround(false, true))
				{
					const int range = SiegeRangeOf(pFoot->GetTechnoType());

					if (range >= 8 * Unsorted::LeptonsPerCell)
					{
						artillery.push_back(pFoot);
						artilleryRange = std::min(artilleryRange, range);
					}
					else
					{
						escorts.push_back(pFoot);
					}
				}

				// Numbers decide the fights the battery meets on its way and at its spot.
				// Main battle tanks first (PlanTankEscorts).
				std::stable_partition(escorts.begin(), escorts.end(), [](FootClass* pFoot)
					{ auto const pUnit = abstract_cast<UnitClass*>(pFoot); return pUnit && IsMainBattleTankType(pUnit->Type); });
				escorts.resize(std::min(escorts.size(), std::clamp<size_t>(escorts.size() / PlanStrikeIdleShare, 3, RollingSiegeMaxEscorts)));
				const size_t groundEscorts = escorts.size();

				for (auto const pAA : collectIdleGround(true))
				{
					if (escorts.size() < groundEscorts + RollingSiegeAntiAirEscorts && std::find(escorts.begin(), escorts.end(), pAA) == escorts.end()
						&& std::find(artillery.begin(), artillery.end(), pAA) == artillery.end())
					{
						escorts.push_back(pAA);
					}
				}

				// The first defense is the one nearest home with a safe firing spot: the outer ring of
				// the enemy base, on our side.
				std::pair<BuildingClass*, CellClass*> first { nullptr, nullptr };
				artillery.resize(std::min<size_t>(artillery.size(), 6));

				if (artillery.size() >= 3 && groundEscorts >= RollingSiegeMinEscorts)
				{
					const int batteryValue = groupValueOf(artillery) + groupValueOf(escorts);
					first = pickNextDefense(homeCell, 1e9, artilleryRange, batteryValue);

					// The battery outranges the defenses, not the units that guard the way in: it goes
					// only when it is worth PlanStrikeAdvantagePercent of the enemy units seen near its
					// spot and on the last stretch of its way, the defenses left out (as the other
					// strikes weigh their way in). In the focus batch of 2026-11-15 222 of the 530
					// members lost died on the way, with a median 78% of it done and 182 of them with
					// enemy units seen near, and the watchdog turned 198 of 347 plans back.
					if (first.first && batteryValue * 100
						< knownEnemyStrengthOnWay(homeCell, first.second->MapCoords, PlanStrikeGuardFrames, false) * PlanStrikeAdvantagePercent)
					{
						first = { nullptr, nullptr };
					}

					// No defense with a safe spot and a way in: the search waits like a held-back raid.
					if (!first.first)
						macro.PlanNextAttemptFrame[static_cast<size_t>(NeuralPlanId::RollingSiege)] = Unsorted::CurrentFrame + PlanGateRetryFrames;
				}

				const int utility = first.first ? (300 + std::min(150, foe.KnownDefenses * 20)) : -1000;

				if (utility > 0 && shouldTryStart(NeuralPlanId::RollingSiege, utility))
				{
					artillery.insert(artillery.end(), escorts.begin(), escorts.end());
					startPlan(NeuralPlanId::RollingSiege, artillery, first.first->GetMapCoords(), first.second->MapCoords,
						first.first->UniqueID, 9000);
				}
			}
		}

		// ---------------------------------------------------------------- Plan 2: Siege Battery
		// Against a turtle: artillery that outranges a known defense shells it, escorted, from a
		// spot no other known defense reaches; guns and escorts march as one column (the one-defense
		// version of Rolling Siege, which runs first and has the first pick of the guns). The battery
		// used to stand on the line defense -> home at the defense's reach + 2.5 cells, whatever the
		// defenses next to it reached, with escorts that ran ahead, and took the same defense again
		// after each failure: 117 of 199 plans of 39 matches ended badly (build bb545626b,
		// 2026-11-15), 80 of them worn down with the lost members last seen 9 to 16 cells from the
		// defense in half, three in a row against one defense with the same three guns stuck 6 cells
		// off their spot (match #9572), and 22 turned back outgunned by units that were around the
		// group when it set out, 10 of them within 450 frames.
		{
			auto& plan = planOf(NeuralPlanId::SiegeBattery);

			// One of this member's guns, or the gun of the structure it unfolds into, hurts the defense.
			auto gunHurts = [](FootClass* pFoot, BuildingClass* pDefense)
			{
				auto const pType = pFoot->GetTechnoType();
				auto const pUnitType = abstract_cast<UnitTypeClass*>(pType);

				return HurtsBuilding(pType, pDefense)
					|| (pUnitType && IsDeployableSiegeUnitType(pUnitType) && HurtsBuilding(pUnitType->DeploysInto, pDefense));
			};

			if (plan.Active)
			{
				auto const pDefense = FindBuildingByID(plan.TargetID);
				auto const pStage = MapClass::Instance.TryGetCellAt(plan.StagingCell);

				if (!pDefense)
				{
					finishPlan(NeuralPlanId::SiegeBattery, true, false);
				}
				else if (!pStage || memberValue(plan) * 100 < plan.InitialValue * 40 || Unsorted::CurrentFrame >= plan.Deadline)
				{
					finishPlan(NeuralPlanId::SiegeBattery, false, true);
				}
				else
				{
					const int defenseRange = GroundRangeOf(pDefense->Type);

					// As Rolling Siege's escorts: they wait behind the guns, out of the defense's reach
					// + RollingSiegeEscortMarginCells and of every other known one's, and one that gets
					// into one drops its target and goes back.
					auto inEscortDanger = [&](const CellStruct& cell)
					{
						return pDefense->GetMapCoords().DistanceFrom(cell) * Unsorted::LeptonsPerCell
							<= defenseRange + RollingSiegeEscortMarginCells * Unsorted::LeptonsPerCell || inKnownDefenseReach(cell, pDefense);
					};

					auto pEscortPost = landCellInZone(pointAlong(plan.StagingCell, homeCell, RollingSiegeEscortBackCells, 0.0), homeZone, 2);

					if (!pEscortPost || inEscortDanger(pEscortPost->MapCoords))
						pEscortPost = pStage;

					for (const DWORD uid : columnMovingOn(plan.Members, plan.StagingCell))
					{
						if (handleSiegeDeployMember(uid, pDefense, pStage))
							continue;

						auto const pFoot = findOrderableFoot(uid);

						if (!pFoot)
							continue;

						// As Rolling Siege's guns: one that stopped near its spot but inside the
						// defense's reach goes back to the spot before it fires.
						const bool isGun = SiegeRangeOf(pFoot->GetTechnoType()) >= defenseRange + 2 * Unsorted::LeptonsPerCell;
						const bool outOfReach = pFoot->DistanceFrom(pDefense) > defenseRange + Unsorted::LeptonsPerCell / 2;
						const bool inPlace = outOfReach && pFoot->GetMapCoords().DistanceFrom(plan.StagingCell) <= 3.0;

						if (isGun && inPlace)
						{
							if (pFoot->Target != pDefense)
								OrderFootAttack(pFoot, pDefense);
						}
						else if (isGun)
						{
							if (!outOfReach && pFoot->Target)
								pFoot->SetTarget(nullptr);

							IssueMoveOrder(pFoot, pStage, outOfReach ? 2.0 : 0.5);
						}
						else if (inEscortDanger(pFoot->GetMapCoords()))
						{
							if (pFoot->Target)
								pFoot->SetTarget(nullptr);

							IssueMoveOrder(pFoot, pEscortPost);
						}
						else if (pFoot->Target == nullptr)
						{
							IssueMoveOrder(pFoot, pEscortPost, RollingSiegeEscortSpreadCells);
						}
					}
				}
			}
			else if (planReady(NeuralPlanId::SiegeBattery) && foe.KnownDefenses >= 3)
			{
				std::vector<FootClass*> artillery;
				std::vector<FootClass*> escorts;
				std::vector<int> ranges;

				for (auto const pFoot : collectIdleGround(false, true))
				{
					const int range = SiegeRangeOf(pFoot->GetTechnoType());

					if (range >= 8 * Unsorted::LeptonsPerCell)
					{
						artillery.push_back(pFoot);
						ranges.push_back(range);
					}
					else
					{
						escorts.push_back(pFoot);
					}
				}

				// Main battle tanks first (PlanTankEscorts).
				std::stable_partition(escorts.begin(), escorts.end(), [](FootClass* pFoot)
					{ auto const pUnit = abstract_cast<UnitClass*>(pFoot); return pUnit && IsMainBattleTankType(pUnit->Type); });
				escorts.resize(std::min(escorts.size(), std::clamp<size_t>(escorts.size() / PlanStrikeIdleShare, 3, SiegeBatteryMaxEscorts)));

				// The reach two guns at least have: the defense must be one they outrange.
				std::sort(ranges.begin(), ranges.end(), [](int a, int b) { return a > b; });
				std::pair<BuildingClass*, CellClass*> pick { nullptr, nullptr };

				if (ranges.size() >= 2)
					pick = pickNextDefense(homeCell, 1e9, ranges[1], groupValueOf(artillery) + groupValueOf(escorts));

				auto const [pDefense, pStage] = pick;
				const int utility = pDefense ? (200 + std::clamp(foe.KnownDefenses * 30, 0, 300)) : -1000;

				if (pDefense && shouldTryStart(NeuralPlanId::SiegeBattery, utility))
				{
					const int defenseRange = GroundRangeOf(pDefense->Type);
					std::vector<FootClass*> group;

					for (auto const pArt : artillery)
					{
						if (group.size() < SiegeBatteryMaxGuns && SiegeRangeOf(pArt->GetTechnoType()) >= defenseRange + 2 * Unsorted::LeptonsPerCell
							&& gunHurts(pArt, pDefense))
						{
							group.push_back(pArt);
						}
					}

					const size_t guns = group.size();
					group.insert(group.end(), escorts.begin(), escorts.end());

					// Not into a fight the raid watchdog would turn it back from at once (the units seen
					// around the group as it sets out outgun it), nor past more than it can take on the
					// last stretch of its way in (the enemy units seen there, the defenses left out, as
					// Rolling Siege weighs its way).
					int strength = 0;
					int sumX = 0;
					int sumY = 0;

					for (auto const pFoot : group)
					{
						strength += static_cast<int>(GetCombatValueOfType(pFoot->GetTechnoType(), false) * pFoot->GetHealthPercentage());
						sumX += pFoot->GetMapCoords().X;
						sumY += pFoot->GetMapCoords().Y;
					}

					const CellStruct from { static_cast<short>(sumX / std::max<int>(1, static_cast<int>(group.size()))),
						static_cast<short>(sumY / std::max<int>(1, static_cast<int>(group.size()))) };
					const int aroundGroup = seenEnemyValueNear(from, PlanOutgunnedSightCells, PlanOutgunnedSightFrames);
					const int onWay = knownEnemyStrengthOnWay(from, pStage->MapCoords, PlanStrikeGuardFrames, false);

					if (guns >= 2 && (aroundGroup * 100 > strength * PlanOutgunnedRatioPercent
						|| groupValueOf(group) * 100 < onWay * PlanStrikeAdvantagePercent))
					{
						macro.PlanNextAttemptFrame[static_cast<size_t>(NeuralPlanId::SiegeBattery)] = Unsorted::CurrentFrame + PlanGateRetryFrames;
					}
					else if (guns >= 2)
					{
						startPlan(NeuralPlanId::SiegeBattery, group, pDefense->GetMapCoords(), pStage->MapCoords, pDefense->UniqueID, 5400);
					}
				}
			}
		}

		// ---------------------------------------------------------------- Plan 32: Delaying Action
		// A big enemy force is on its way and our base defenses are thin: a few cheap units meet
		// it halfway to slow it down while the base finishes its defenses (point 34). Elite
		// units are never sacrificed.
		{
			auto& plan = planOf(NeuralPlanId::DelayingAction);

			if (plan.Active)
			{
				const int atBase = seenEnemyValueNear(homeCell, 14.0, 150);
				const CellStruct enemyCenter = seenEnemyCentroidNear(homeCell, 40.0, 150);

				if (atBase > 0)
				{
					finishPlan(NeuralPlanId::DelayingAction, Unsorted::CurrentFrame - plan.StartFrame >= 900, false);
				}
				else if (Unsorted::CurrentFrame >= plan.Deadline || groupValue(plan.Members) <= 0 || enemyCenter == CellStruct::Empty)
				{
					finishPlan(NeuralPlanId::DelayingAction, true, false);
				}
				else
				{
					attackMoveGroup(plan.Members, MapClass::Instance.TryGetCellAt(enemyCenter));
				}
			}
			else if (planReady(NeuralPlanId::DelayingAction) && ownHomeDefenses < 3)
			{
				const int approaching = seenEnemyValueNear(homeCell, 35.0, 150);
				const int arrived = seenEnemyValueNear(homeCell, 15.0, 150);
				std::vector<FootClass*> cheap;

				if (approaching >= 200 && arrived == 0)
				{
					for (auto const pFoot : collectIdleGround(false))
					{
						if (pFoot->GetTechnoType()->Cost <= 500 && !isElite(pFoot))
							cheap.push_back(pFoot);
					}

					cheap.resize(std::min<size_t>(cheap.size(), 4));
				}

				const int utility = cheap.size() >= 2 ? 300 : -1000;

				if (utility > 0 && shouldTryStart(NeuralPlanId::DelayingAction, utility))
					startPlan(NeuralPlanId::DelayingAction, cheap, homeCell, CellStruct::Empty, 0, 1500);
			}
		}

		// ---------------------------------------------------------------- Plan 33: Three-Front Assault
		// Three groups stage around the same known enemy production building (straight on and
		// +-60 degrees) and attack together once all are in place or after 1800 frames (point 48).
		// Targets[0..2] hold the size of each group; AuxPeak = phase.
		{
			auto& plan = planOf(NeuralPlanId::ThreeFrontAssault);
			constexpr double FrontSide[] = { 0.0, 1.732, -1.732 }; // tan(60 deg)

			auto frontStage = [&](const CellStruct& target, int front) -> CellClass*
			{
				return landCellInZone(pointAlong(target, homeCell, 8.0, 8.0 * FrontSide[front]), homeZone, 4);
			};

			if (plan.Active)
			{
				auto const pTarget = FindBuildingByID(plan.TargetID);

				if (!pTarget)
				{
					finishPlan(NeuralPlanId::ThreeFrontAssault, true, true);
				}
				else if (memberValue(plan) * 100 < plan.InitialValue * 40 || Unsorted::CurrentFrame >= plan.Deadline)
				{
					finishPlan(NeuralPlanId::ThreeFrontAssault, false, true);
				}
				else if (plan.AuxPeak == 0)
				{
					bool allReady = true;
					size_t first = 0;

					for (int front = 0; front < 3 && front < static_cast<int>(plan.Targets.size()); ++front)
					{
						const size_t count = std::min<size_t>(plan.Targets[front], plan.Members.size() - first);
						const std::vector<DWORD> group(plan.Members.begin() + first, plan.Members.begin() + first + count);
						first += count;
						auto const pStage = frontStage(plan.TargetCell, front);
						moveGroup(group, pStage, 3.0);
						const CellStruct gc = groupCentroid(group);

						if (gc != CellStruct::Empty && (!pStage || gc.DistanceFrom(pStage->MapCoords) > 5.0))
							allReady = false;
					}

					if (allReady || Unsorted::CurrentFrame - plan.StartFrame >= 1800)
						plan.AuxPeak = 1;
				}
				else
				{
					attackMoveGroup(plan.Members, pTarget->GetCell());
				}
			}
			else if (planReady(NeuralPlanId::ThreeFrontAssault))
			{
				std::vector<FootClass*> army = collectIdleGround(false);
				BuildingClass* pTarget = nullptr;
				double bestDist = 1e9;

				if (army.size() >= 15)
				{
					for (auto const pBld : knownEnemyBuildingList)
					{
						if (!isKnownEnemyBuilding(pBld) || (pBld->Type->Factory == AbstractType::None && !pBld->Type->ConstructionYard))
							continue;

						const CellStruct bc = pBld->GetMapCoords();
						const double d = bc.DistanceFrom(homeCell);

						if (zoneOf(bc) == homeZone && d >= 28.0 && d < bestDist && frontStage(bc, 1) && frontStage(bc, 2))
						{
							bestDist = d;
							pTarget = pBld;
						}
					}
				}

				const int utility = pTarget ? (320 + std::min(200, static_cast<int>(army.size()) * 8)) : -1000;

				if (utility > 0 && shouldTryStart(NeuralPlanId::ThreeFrontAssault, utility))
				{
					army.resize(std::min<size_t>(army.size(), 24));
					const DWORD third = static_cast<DWORD>(army.size() / 3);
					startPlan(NeuralPlanId::ThreeFrontAssault, army, pTarget->GetMapCoords(), CellStruct::Empty, pTarget->UniqueID, 6300);
					auto& started = planOf(NeuralPlanId::ThreeFrontAssault);
					started.Targets = { third, third, static_cast<DWORD>(army.size()) - 2 * third };
					started.AuxPeak = 0;
				}
			}
		}

		// ---------------------------------------------------------------- Plan 34: Factory Decapitation
		// Every known enemy factory (up to 3) is hit at the same moment by its own small group, so
		// even a defended base cannot replace its army (point 49). Targets = factory UniqueIDs;
		// member i attacks Targets[i * T / M], or any factory still standing.
		{
			auto& plan = planOf(NeuralPlanId::FactoryDecapitation);

			if (plan.Active)
			{
				std::vector<BuildingClass*> standing;

				for (const DWORD uid : plan.Targets)
				{
					if (auto const pBld = FindBuildingByID(uid))
						standing.push_back(pBld);
				}

				if (standing.empty())
				{
					finishPlan(NeuralPlanId::FactoryDecapitation, true, true);
				}
				else if (memberValue(plan) * 100 < plan.InitialValue * 35 || Unsorted::CurrentFrame >= plan.Deadline)
				{
					// At least half of the targeted factories destroyed counts as a success: losing
					// most of its production is what the plan is for.
					const size_t destroyed = plan.Targets.size() - standing.size();
					finishPlan(NeuralPlanId::FactoryDecapitation, destroyed * 2 >= plan.Targets.size(), true);
				}
				else
				{
					for (size_t i = 0; i < plan.Members.size(); ++i)
					{
						auto const pAssigned = FindBuildingByID(plan.Targets[i * plan.Targets.size() / plan.Members.size()]);
						strikeWithMembers({ plan.Members[i] }, pAssigned ? pAssigned : standing.front());
					}
				}
			}
			else if (planReady(NeuralPlanId::FactoryDecapitation))
			{
				std::vector<BuildingClass*> factories;

				for (auto const pBld : knownEnemyBuildingList)
				{
					if (isKnownEnemyBuilding(pBld) && pBld->Type->Factory != AbstractType::None && !pBld->Type->Naval)
						factories.push_back(pBld);
				}

				std::sort(factories.begin(), factories.end(), [&](BuildingClass* a, BuildingClass* b)
					{
						const double da = a->GetMapCoords().DistanceFrom(homeCell);
						const double db = b->GetMapCoords().DistanceFrom(homeCell);
						return da != db ? da < db : a->UniqueID < b->UniqueID;
					});

				// The ground units (taken from the home landmass) go only for factories on that ground.
				// With fewer than two of those the aircraft fly alone at the nearest ones.
				std::vector<BuildingClass*> onOurGround;

				for (auto const pFactory : factories)
				{
					if (zoneOf(pFactory->GetMapCoords()) == homeZone || IsBuildingEnterableFromZone(pFactory, homeZone))
						onOurGround.push_back(pFactory);
				}

				const bool byLand = onOurGround.size() >= 2;

				if (byLand)
					factories = onOurGround;

				factories.resize(std::min<size_t>(factories.size(), 3));

				std::vector<FootClass*> group = collectAirStrikers();

				if (byLand)
				{
					std::vector<FootClass*> ground = collectIdleGround(false);
					sortFastest(ground);
					group.insert(group.end(), ground.begin(), ground.end());
				}
				group.resize(std::min(group.size(), std::max(factories.size() * 3, group.size() / PlanStrikeIdleShare)));

				const int utility = (factories.size() >= 2 && group.size() >= factories.size() * 2) ? 310 : -1000;

				if (utility > 0 && shouldTryStart(NeuralPlanId::FactoryDecapitation, utility))
				{
					startPlan(NeuralPlanId::FactoryDecapitation, group, factories.front()->GetMapCoords(), CellStruct::Empty,
						factories.front()->UniqueID, 3600);
					auto& started = planOf(NeuralPlanId::FactoryDecapitation);

					for (auto const pFactory : factories)
						started.Targets.push_back(pFactory->UniqueID);
				}
			}
		}

		// ---------------------------------------------------------------- Plan 35: Cut Reinforcements
		// A group holds a spot just outside the defenses of the enemy war factory closest to the
		// front, shooting its new units one by one before they can join its army (point 52).
		{
			auto& plan = planOf(NeuralPlanId::CutReinforcements);

			if (plan.Active)
			{
				auto const pFactory = FindBuildingByID(plan.TargetID);
				auto const pPost = MapClass::Instance.TryGetCellAt(plan.StagingCell);
				const int value = memberValue(plan);

				if (pPost)
					plan.AuxPeak = std::max(plan.AuxPeak, seenEnemyValueNear(plan.StagingCell, 9.0, 150));

				if (!pFactory || !pPost || value * 100 < plan.InitialValue * 40
					|| seenEnemyValueNear(plan.StagingCell, 9.0, 150) * 10 > value * 15)
				{
					finishPlan(NeuralPlanId::CutReinforcements, !pFactory, true);
				}
				else if (Unsorted::CurrentFrame >= plan.Deadline)
				{
					finishPlan(NeuralPlanId::CutReinforcements, plan.AuxPeak > 0 && value * 100 >= plan.InitialValue * 50, true);
				}
				else
				{
					moveGroup(plan.Members, pPost, 3.0);
				}
			}
			else if (planReady(NeuralPlanId::CutReinforcements))
			{
				const CellStruct front = macro.LastWaveTargetCell != CellStruct::Empty ? macro.LastWaveTargetCell : knownEnemyCentroid;
				BuildingClass* pFactory = nullptr;
				CellClass* pPost = nullptr;
				double bestDist = 1e9;

				if (front != CellStruct::Empty)
				{
					for (auto const pBld : knownEnemyBuildingList)
					{
						if (!isKnownEnemyBuilding(pBld) || pBld->Type->Factory != AbstractType::UnitType || pBld->Type->Naval)
							continue;

						const CellStruct bc = pBld->GetMapCoords();
						const double d = bc.DistanceFrom(front);

						if (zoneOf(bc) != homeZone || d >= bestDist)
							continue;

						const int reach = maxKnownDefenseRangeNear(bc, 14.0) / Unsorted::LeptonsPerCell;
						auto const pCell = landCellInZone(pointAlong(bc, homeCell, std::max(8, reach + 3), 0.0), homeZone, 3);

						if (pCell && !inKnownDefenseReach(pCell->MapCoords, nullptr))
						{
							bestDist = d;
							pFactory = pBld;
							pPost = pCell;
						}
					}
				}

				std::vector<FootClass*> group;

				if (pFactory)
				{
					group = collectIdleGround(false);
					group.resize(std::min(group.size(), std::max<size_t>(6, group.size() / PlanStrikeIdleShare)));

					// The post is given up when the enemy seen there outweighs the group by half: a group
					// that would give it up on arrival does not set out.
					if (group.size() < 4 || seenEnemyValueNear(pPost->MapCoords, 9.0, 150) * 10 > groupValueOf(group) * 15)
						group.clear();
				}

				const int utility = !group.empty() ? 280 : -1000;

				if (utility > 0 && shouldTryStart(NeuralPlanId::CutReinforcements, utility))
				{
					startPlan(NeuralPlanId::CutReinforcements, group, pFactory->GetMapCoords(), pPost->MapCoords, pFactory->UniqueID, 3600);
					planOf(NeuralPlanId::CutReinforcements).AuxPeak = 0;
				}
			}
		}

		// ---------------------------------------------------------------- Plan: Edge Blitz
		// A lightning strike round the enemy base (user, 2026-11-20: «recorrer los bordes del mapa y atacar
		// por la espalda, que no suele protegerse ... un buen ataque relámpago con unidades poderosas»).
		// The strongest fast idle ground units go out to one side of the line between the two bases, as
		// far as the map and our ground let them, then past the target behind the enemy base, and strike a
		// decisive building of its rear: one behind the centre of its owner's known buildings as seen from
		// us, with few known defenses near it. The side with less known enemy strength on the way is taken.
		// Cells = the waypoints; AuxPeak = the leg (one per waypoint, then the strike); AuxBaseline = when
		// the leg began.
		{
			auto& plan = planOf(NeuralPlanId::EdgeBlitz);

			if (plan.Active)
			{
				auto const pTarget = FindBuildingByID(plan.TargetID);

				if (!pTarget)
				{
					finishPlan(NeuralPlanId::EdgeBlitz, true, true);
				}
				else if (memberValue(plan) * 100 < plan.InitialValue * EdgeBlitzKeepPercent || Unsorted::CurrentFrame >= plan.Deadline)
				{
					finishPlan(NeuralPlanId::EdgeBlitz, false, true);
				}
				else if (plan.AuxPeak >= 0 && plan.AuxPeak < static_cast<int>(plan.Cells.size()))
				{
					auto const pLeg = MapClass::Instance.TryGetCellAt(plan.Cells[plan.AuxPeak]);
					const CellStruct at = groupCentroid(plan.Members);

					if (!pLeg || (at != CellStruct::Empty && at.DistanceFrom(pLeg->MapCoords) <= EdgeBlitzWaypointCells)
						|| Unsorted::CurrentFrame - plan.AuxBaseline >= EdgeBlitzLegFrames || Unsorted::CurrentFrame < plan.AuxBaseline)
					{
						plan.AuxPeak++;
						plan.AuxBaseline = Unsorted::CurrentFrame;
					}
					else
					{
						moveGroup(plan.Members, pLeg, 3.0);
					}
				}
				else
				{
					strikeWithMembers(plan.Members, pTarget);
				}
			}
			else if (planReady(NeuralPlanId::EdgeBlitz) && homeCell != CellStruct::Empty)
			{
				// The strongest fast units first: combat value times speed.
				std::vector<FootClass*> group = collectIdleGround(false);
				std::sort(group.begin(), group.end(), [](FootClass* a, FootClass* b)
					{
						auto const pA = a->GetTechnoType();
						auto const pB = b->GetTechnoType();
						const long long va = static_cast<long long>(GetCombatValueOfType(pA, false)) * pA->Speed;
						const long long vb = static_cast<long long>(GetCombatValueOfType(pB, false)) * pB->Speed;
						return va != vb ? va > vb : a->UniqueID < b->UniqueID;
					});

				if (group.size() > EdgeBlitzGroupMost)
					group.resize(EdgeBlitzGroupMost);

				// The centre of each enemy house's known buildings on our ground.
				std::vector<long long> sumX(HouseClass::Array.Count, 0);
				std::vector<long long> sumY(HouseClass::Array.Count, 0);
				std::vector<int> counted(HouseClass::Array.Count, 0);

				for (auto const pBld : knownEnemyBuildingList)
				{
					if (!isKnownEnemyBuilding(pBld) || zoneOf(pBld->GetMapCoords()) != homeZone)
						continue;

					const int owner = pBld->Owner->ArrayIndex;

					if (owner < 0 || owner >= HouseClass::Array.Count)
						continue;

					sumX[owner] += pBld->GetMapCoords().X;
					sumY[owner] += pBld->GetMapCoords().Y;
					counted[owner]++;
				}

				// Behind the centre as seen from home, worth a strike, few defenses near: the deepest and
				// most valuable, nearest last.
				BuildingClass* pTarget = nullptr;
				int bestScore = INT_MIN;
				double centreX = 0.0;
				double centreY = 0.0;
				double targetDepth = 0.0;

				if (group.size() >= EdgeBlitzGroupLeast)
				{
					for (auto const pBld : knownEnemyBuildingList)
					{
						if (!isKnownEnemyBuilding(pBld) || zoneOf(pBld->GetMapCoords()) != homeZone || pBld->Type->Immune)
							continue;

						const int worth = valuableBuildingWorth(pBld->Type);
						const int owner = pBld->Owner->ArrayIndex;

						if (worth <= 0 || owner < 0 || owner >= HouseClass::Array.Count || counted[owner] <= 0)
							continue;

						const double cx = static_cast<double>(sumX[owner]) / counted[owner];
						const double cy = static_cast<double>(sumY[owner]) / counted[owner];
						const double fx = homeCell.X - cx;
						const double fy = homeCell.Y - cy;
						const double axis = std::sqrt(fx * fx + fy * fy);

						if (axis < 1.0)
							continue;

						const CellStruct bc = pBld->GetMapCoords();
						const double depth = -((bc.X - cx) * fx + (bc.Y - cy) * fy) / axis;
						const int defenses = CountKnownEnemyDefensesNear(pHouse, bc, EdgeBlitzDefenseCells, false);

						if (depth < EdgeBlitzRearDepthCells || defenses > EdgeBlitzTargetDefenses)
							continue;

						const int score = worth + static_cast<int>(depth * 10.0) - defenses * 80 - static_cast<int>(bc.DistanceFrom(homeCell)) / 2;

						if (score > bestScore)
						{
							bestScore = score;
							pTarget = pBld;
							centreX = cx;
							centreY = cy;
							targetDepth = depth;
						}
					}
				}

				// The way round: from an ideal point toward a fallback, the first legal cell of our ground out
				// of known defense reach (2-cell steps).
				auto const groundToward = [&](double fromX, double fromY, double toX, double toY) -> CellStruct
				{
					const double dx = toX - fromX;
					const double dy = toY - fromY;
					const double length = std::sqrt(dx * dx + dy * dy);

					for (double done = 0.0; done <= length; done += 2.0)
					{
						const double t = length > 0.0 ? done / length : 0.0;
						const CellStruct c { static_cast<short>(std::lround(fromX + dx * t)), static_cast<short>(std::lround(fromY + dy * t)) };

						if (MapClass::Instance.CoordinatesLegal(c) && MapClass::Instance.IsWithinUsableArea(c, true) && zoneOf(c) == homeZone
							&& !IsCellInKnownEnemyDefenseReach(pHouse, c, 2))
						{
							return c;
						}
					}

					return CellStruct::Empty;
				};

				std::vector<CellStruct> route;
				int routeStrength = INT_MAX;

				if (pTarget)
				{
					const CellStruct tc = pTarget->GetMapCoords();
					const double ux = (centreX - homeCell.X);
					const double uy = (centreY - homeCell.Y);
					const double axis = std::sqrt(ux * ux + uy * uy);
					const double nx = ux / axis;
					const double ny = uy / axis;
					const double midX = (homeCell.X + centreX) / 2.0;
					const double midY = (homeCell.Y + centreY) / 2.0;

					for (const int side : { 1, -1 })
					{
						const double px = -ny * side;
						const double py = nx * side;
						const CellStruct out = groundToward(midX + px * EdgeBlitzSideMostCells, midY + py * EdgeBlitzSideMostCells, midX, midY);

						if (out == CellStruct::Empty || std::abs((out.X - midX) * px + (out.Y - midY) * py) < EdgeBlitzSideLeastCells)
							continue;

						const CellStruct behind = groundToward(tc.X + nx * EdgeBlitzBehindCells * 0.5 + px * EdgeBlitzBehindCells,
							tc.Y + ny * EdgeBlitzBehindCells * 0.5 + py * EdgeBlitzBehindCells, tc.X, tc.Y);
						const int strength = knownEnemyStrengthOnWay(homeCell, out, 900)
							+ knownEnemyStrengthOnWay(out, behind != CellStruct::Empty ? behind : tc, 900);

						if (strength < routeStrength)
						{
							routeStrength = strength;
							route = { out };

							if (behind != CellStruct::Empty)
								route.push_back(behind);
						}
					}
				}

				const int utility = (pTarget && !route.empty()) ? EdgeBlitzUtility : -1000;

				if (utility > 0 && shouldTryStart(NeuralPlanId::EdgeBlitz, utility))
				{
					startPlan(NeuralPlanId::EdgeBlitz, group, pTarget->GetMapCoords(), route.front(), pTarget->UniqueID, EdgeBlitzFrames);
					auto& started = planOf(NeuralPlanId::EdgeBlitz);

					if (started.Active)
					{
						started.Cells = route;
						started.AuxPeak = 0;
						started.AuxBaseline = Unsorted::CurrentFrame;

						// TEMP diagnostic: the starts, with the defenses of the target's owner in front of its
						// centre and behind it (does its rear stand less defended?).
						if (macro.EdgeBlitzLogCount < EdgeBlitzLogLines)
						{
							macro.EdgeBlitzLogCount++;
							int front = 0;
							int rear = 0;
							const double fx = homeCell.X - centreX;
							const double fy = homeCell.Y - centreY;

							for (auto const pBld : knownEnemyBuildingList)
							{
								if (!isKnownEnemyBuilding(pBld) || pBld->Owner != pTarget->Owner || !pBld->Type->IsBaseDefense
									|| GroundRangeOf(pBld->Type) <= 0)
								{
									continue;
								}

								const double along = (pBld->GetMapCoords().X - centreX) * fx + (pBld->GetMapCoords().Y - centreY) * fy;
								(along >= 0.0 ? front : rear)++;
							}

							const CellStruct tc = pTarget->GetMapCoords();
							const CellStruct second = route.size() > 1 ? route[1] : CellStruct { -1, -1 };
							Debug::Log("[NeuralAI] House %d edge blitz (TEMP): %d units -> %s of house %d at (%d,%d), %d cells behind its centre, "
								"its defenses %d in front and %d behind, by (%d,%d) and (%d,%d), strength on the way %d, frame %d.\n",
								pHouse->ArrayIndex, static_cast<int>(started.Members.size()), pTarget->Type->ID, pTarget->Owner->ArrayIndex, tc.X, tc.Y,
								static_cast<int>(targetDepth), front, rear, route[0].X, route[0].Y, second.X, second.Y, routeStrength,
								Unsorted::CurrentFrame);
						}
					}
				}
			}
		}

		// ---------------------------------------------------------------- Plan 36: Tech Tree Cut
		// Among the enemy unit types we have seen, the strongest one's tech prerequisite (a
		// building listed in its Prerequisite= that is not a ConYard, factory, power plant or
		// refinery: rules data is public) is destroyed when we know where it is (point 54).
		{
			auto& plan = planOf(NeuralPlanId::TechTreeCut);

			if (plan.Active)
			{
				auto const pTarget = FindBuildingByID(plan.TargetID);

				if (!pTarget)
					finishPlan(NeuralPlanId::TechTreeCut, true, true);
				else if (memberValue(plan) * 100 < plan.InitialValue * 40 || Unsorted::CurrentFrame >= plan.Deadline)
					finishPlan(NeuralPlanId::TechTreeCut, false, true);
				else
					strikeWithMembers(plan.Members, pTarget);
			}
			else if (planReady(NeuralPlanId::TechTreeCut))
			{
				// Seen value per enemy unit type (key: type array index, ordered map).
				std::map<int, int> typeValue;
				std::map<int, const TechnoTypeClass*> typeByIndex;

				for (auto const& [uid, sighting] : macro.EnemySightings)
				{
					if (sighting.Type && Unsorted::CurrentFrame - sighting.LastSeenFrame <= 5400)
					{
						const int key = static_cast<int>(sighting.Type->WhatAmI()) * 10000 + sighting.Type->GetArrayIndex();
						typeValue[key] += sighting.Value;
						typeByIndex[key] = sighting.Type;
					}
				}

				BuildingClass* pTarget = nullptr;
				int bestValue = 59;

				for (auto const& [key, value] : typeValue)
				{
					if (value <= bestValue)
						continue;

					auto const pType = const_cast<TechnoTypeClass*>(typeByIndex[key]);

					for (int i = 0; i < pType->Prerequisite.Count; ++i)
					{
						const int bIdx = pType->Prerequisite.GetItem(i);

						if (bIdx < 0 || bIdx >= BuildingTypeClass::Array.Count)
							continue;

						auto const pPrereq = BuildingTypeClass::Array.GetItem(bIdx);

						if (!pPrereq || pPrereq->ConstructionYard || pPrereq->Factory != AbstractType::None || pPrereq->PowerBonus > 0
							|| pPrereq->Refinery || pPrereq->ResourceDestination)
						{
							continue;
						}

						for (auto const pBld : knownEnemyBuildingList)
						{
							if (isKnownEnemyBuilding(pBld) && pBld->Type == pPrereq)
							{
								pTarget = pBld;
								bestValue = value;
								break;
							}
						}
					}
				}

				std::vector<FootClass*> group = pickStrikeGroup(pTarget, 5, 10);
				const int utility = (pTarget && !group.empty()) ? (300 + std::min(250, bestValue)) : -1000;

				if (utility > 0 && shouldTryStart(NeuralPlanId::TechTreeCut, utility))
				{
					startPlan(NeuralPlanId::TechTreeCut, group, pTarget->GetMapCoords(), CellStruct::Empty, pTarget->UniqueID, 4500);
					strikeWithMembers(planOf(NeuralPlanId::TechTreeCut).Members, pTarget);
				}
			}
		}

		for (size_t i = 0; i < gateOpenedUnstarted.size(); ++i)
		{
			if (gateOpenedUnstarted[i])
				CountPlanOutcome(houseIdx, static_cast<NeuralPlanId>(i), NeuralPlanOutcome::NoGroup);
		}

		// Follow-up attacks (PlanFollowUp*, NeuralAI.State.inl): the group of a plan that took its building
		// target goes on with the nearest known building of that kind of the same house, then of any kind,
		// until none is left within PlanFollowUpCells; one order for each target, again only to a member left
		// idle, and a member that fights is left to it. Over, the group goes home.
		{
			static int followUpLogSerial = -1;
			static int followUpLogCount = 0;

			if (followUpLogSerial != NeuralMatchSerial)
			{
				followUpLogSerial = NeuralMatchSerial;
				followUpLogCount = 0;
			}

			const int now = Unsorted::CurrentFrame;

			// TEMP diagnostic: what the last pick found (known buildings of that house, within reach, reached).
			int pickKnown = 0;
			int pickNear = 0;
			int pickReached = 0;

			auto const pickFollowUpTarget = [&](const NeuralFollowUp& followUp, const CellStruct& center, FootClass* pLead) -> BuildingClass*
			{
				pickKnown = 0;
				pickNear = 0;
				pickReached = 0;
				BuildingClass* pSame = nullptr;
				BuildingClass* pAny = nullptr;
				int sameDistance = INT_MAX;
				int anyDistance = INT_MAX;

				for (auto const pBld : BuildingClass::Array)
				{
					if (!pBld || pBld->InLimbo || !pBld->IsAlive || !pBld->Owner || !pBld->Type || pBld->Owner->ArrayIndex != followUp.Owner
						|| pBld->Type->Insignificant || pBld->Type->InvisibleInGame || pBld->Type->Wall || pHouse->IsAlliedWith(pBld->Owner)
						|| !IsEnemyBuildingKnown(pHouse, pBld))
					{
						continue;
					}

					const CellStruct bc = pBld->GetMapCoords();
					const int distance = std::max(std::abs(bc.X - center.X), std::abs(bc.Y - center.Y));
					const bool same = PlanBuildingKindOf(pBld->Type) == followUp.Kind;
					pickKnown++;

					if (distance > PlanFollowUpCells)
						continue;

					pickNear++;

					if (pLead && !FootReachesAlone(pLead, bc, pBld))
						continue;

					pickReached++;

					if (distance >= (same ? sameDistance : anyDistance))
						continue;

					(same ? pSame : pAny) = pBld;
					(same ? sameDistance : anyDistance) = distance;
				}

				return pSame ? pSame : pAny;
			};

			auto const logFollowUp = [&](const NeuralFollowUp& followUp, const char* what, BuildingClass* pTarget, int value)
			{
				if (followUpLogCount++ >= PlanFollowUpLogLines)
					return;

				const CellStruct tc = pTarget ? pTarget->GetMapCoords() : CellStruct::Empty;
				Debug::Log("[NeuralAI] House %d follow-up of plan %s against house %d (kind %d): %s %s at (%d,%d), %d members, value %d of %d, "
					"%d targets, %d frames, frame %d.\n",
					houseIdx, followUp.Plan >= 0 ? NeuralPlanNames[static_cast<size_t>(followUp.Plan)] : "?", followUp.Owner, followUp.Kind,
					what, pTarget ? pTarget->Type->ID : "none", tc.X, tc.Y, static_cast<int>(followUp.Members.size()), value,
					followUp.InitialValue, followUp.Targets, now - followUp.StartFrame, now);
			};

			for (auto& followUp : macro.FollowUps)
			{
				std::erase_if(followUp.Members, [&](DWORD uid) { return !ownFoots.contains(uid) || IsOnRepairTrip(pHouse, uid); });

				int value = 0;
				int sumX = 0;
				int sumY = 0;
				FootClass* pLead = nullptr;

				for (const DWORD uid : followUp.Members)
				{
					auto const pFoot = ownFoots.at(uid);
					value += GetCombatValueOfType(pFoot->GetTechnoType(), false);
					const CellStruct fc = pFoot->GetMapCoords();
					sumX += fc.X;
					sumY += fc.Y;

					if (!pLead)
						pLead = pFoot;
				}

				bool over = followUp.Members.empty() || value * 100 < followUp.InitialValue * PlanFollowUpLowPercent;

				if (!over && now >= followUp.Until)
				{
					if (followUp.LastKillFrame >= 0 && now - followUp.LastKillFrame <= PlanCombatWindowFrames
						&& followUp.Until - followUp.StartFrame + PlanCombatExtendFrames <= PlanFollowUpMostFrames)
					{
						followUp.Until += PlanCombatExtendFrames;
					}
					else
					{
						over = true;
					}
				}

				BuildingClass* pTarget = nullptr;

				if (!over && followUp.TargetID != 0)
				{
					for (auto const pBld : BuildingClass::Array)
					{
						if (pBld && pBld->UniqueID == followUp.TargetID)
						{
							if (pBld->IsAlive && !pBld->InLimbo && pBld->Owner && !pHouse->IsAlliedWith(pBld->Owner))
								pTarget = pBld;

							break;
						}
					}
				}

				if (!over && !pTarget)
				{
					const int alive = static_cast<int>(followUp.Members.size());
					const CellStruct center = followUp.Center != CellStruct::Empty
						? followUp.Center
						: CellStruct { static_cast<short>(sumX / alive), static_cast<short>(sumY / alive) };
					pTarget = pickFollowUpTarget(followUp, center, pLead);

					if (pTarget)
					{
						followUp.Center = pTarget->GetMapCoords();
						followUp.TargetID = pTarget->UniqueID;
						followUp.Ordered.clear();
						followUp.Targets++;
						logFollowUp(followUp, "goes for", pTarget, value);
					}
					else
					{
						over = true;
					}
				}

				if (over)
				{
					char endText[96];
					std::snprintf(endText, sizeof(endText), "ends (%d known, %d near, %d reached), home with", pickKnown, pickNear, pickReached);
					logFollowUp(followUp, endText, nullptr, value);

					if (auto const pHome = MapClass::Instance.TryGetCellAt(homeCell))
					{
						for (const DWORD uid : followUp.Members)
						{
							if (auto const pFoot = findOrderableFoot(uid))
								IssueMoveOrder(pFoot, pHome, 4.0);
						}
					}

					followUp.Members.clear();
					continue;
				}

				for (const DWORD uid : followUp.Members)
				{
					auto const pFoot = findOrderableFoot(uid);

					if (!pFoot || memberFights(pFoot))
						continue;

					const bool sent = std::find(followUp.Ordered.begin(), followUp.Ordered.end(), uid) != followUp.Ordered.end();
					const bool idle = pFoot->Target == nullptr && pFoot->MegaMission != Mission::AttackMove
						&& (pFoot->CurrentMission == Mission::Guard || pFoot->CurrentMission == Mission::Area_Guard
							|| pFoot->CurrentMission == Mission::Sleep || pFoot->CurrentMission == Mission::None);

					if (sent && !idle)
						continue;

					if (pFoot->GetMapCoords().DistanceFrom(pTarget->GetMapCoords()) <= 9.0)
					{
						if (pFoot->Target != pTarget)
						{
							pFoot->SetTarget(pTarget);
							pFoot->QueueMission(Mission::Attack, true);
							NoteOrderCaller(pFoot, "attack");
						}
					}
					else
					{
						OrderFootToAttackMove(pFoot, pTarget->GetCell());
					}

					if (!sent)
						followUp.Ordered.push_back(uid);
				}
			}

			std::erase_if(macro.FollowUps, [](const NeuralFollowUp& followUp) { return followUp.Members.empty(); });
		}

		// Rebuild the member set used to keep plan units out of the generic recruitment.
		macro.PlanMembers.clear();

		for (auto const& plan : macro.Plans)
		{
			if (plan.Active)
				macro.PlanMembers.insert(plan.Members.begin(), plan.Members.end());
		}

		// The units a Rotating Raids recruits for its next group are kept out the same way.
		macro.PlanMembers.insert(macro.RaidRecruits.begin(), macro.RaidRecruits.end());

		// So are the groups of the follow-up attacks.
		for (auto const& followUp : macro.FollowUps)
			macro.PlanMembers.insert(followUp.Members.begin(), followUp.Members.end());
	}
} // namespace

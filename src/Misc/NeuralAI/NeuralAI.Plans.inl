// NeuralAI.Plans.inl: Strategic plan library and its helpers.
// Part of NeuralAI.cpp (included in order; one translation unit).

namespace
{
	int AirSortieDamageVs(AircraftClass* pAir, BuildingClass* pTarget);

	bool IsPlanMember(HouseClass* pHouse, DWORD uid)
	{
		if (!pHouse)
			return false;

		auto const& members = GetMacroState(pHouse).PlanMembers;

		return members.find(uid) != members.end();
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

			if (pWS && WeaponHitsGround(pWS->WeaponType))
				range = std::max(range, pWS->WeaponType->Range);
		}

		return range;
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

	// Last frame the Neural AI itself ordered a vehicle to unfold, by unit (LogSiegeDeploys).
	std::unordered_map<DWORD, int> NeuralDeployOrderFrame {};

	// TEMP diagnostic: every siege unit of this house that starts to unfold, with who ordered it
	// (the Neural AI in the last 90 frames, or the engine / another system) and what it was aiming
	// at: type, in the air or not, distance and the reach of the deployed gun.
	void LogSiegeDeploys(HouseClass* pHouse)
	{
		static int logSerial = -1;
		static int logCount = 0;
		static std::unordered_map<DWORD, int> loggedFrame;

		if (logSerial != NeuralMatchSerial)
		{
			logSerial = NeuralMatchSerial;
			logCount = 0;
			loggedFrame.clear();
			NeuralDeployOrderFrame.clear();
		}

		if (logCount >= 80)
			return;

		const int now = Unsorted::CurrentFrame;

		for (auto const pUnit : UnitClass::Array)
		{
			if (!pUnit || pUnit->Owner != pHouse || !pUnit->IsAlive || pUnit->InLimbo || !pUnit->Type
				|| !IsDeployableSiegeUnitType(pUnit->Type))
			{
				continue;
			}

			if (pUnit->CurrentMission != Mission::Unload && pUnit->QueuedMission != Mission::Unload && !pUnit->IsDeploying)
				continue;

			auto const itLogged = loggedFrame.find(pUnit->UniqueID);

			if (itLogged != loggedFrame.end() && now - itLogged->second < 600)
				continue;

			loggedFrame[pUnit->UniqueID] = now;
			logCount++;

			auto const itOrder = NeuralDeployOrderFrame.find(pUnit->UniqueID);
			const bool byNeural = itOrder != NeuralDeployOrderFrame.end() && now - itOrder->second <= 90;
			auto const pTarget = abstract_cast<TechnoClass*>(pUnit->Target);
			// The nearest enemy within 12 cells, whatever it is.
			TechnoClass* pNearest = nullptr;
			int nearestDist = 12 * Unsorted::LeptonsPerCell;

			for (auto const pTechno : TechnoClass::Array)
			{
				if (!pTechno || !pTechno->IsAlive || pTechno->InLimbo || !pTechno->Owner || pTechno->Owner == pHouse
					|| pHouse->IsAlliedWith(pTechno->Owner) || pTechno->Owner->IsNeutral())
				{
					continue;
				}

				const int dist = pUnit->DistanceFrom(pTechno);

				if (dist < nearestDist)
				{
					nearestDist = dist;
					pNearest = pTechno;
				}
			}

			Debug::Log("[NeuralAI] House %d SiegeDeploy DIAG: %s %u at (%d,%d) unfolds, ordered by %s, mission %d queued %d, "
				"target %s (in air %d, height %d, %d cells), nearest enemy %s (in air %d, %d cells), deployed reach %d cells, frame %d.\n",
				pHouse->ArrayIndex, pUnit->Type->ID, pUnit->UniqueID, pUnit->GetMapCoords().X, pUnit->GetMapCoords().Y,
				byNeural ? "neural" : "engine", static_cast<int>(pUnit->CurrentMission), static_cast<int>(pUnit->QueuedMission),
				pTarget ? pTarget->GetTechnoType()->ID : (pUnit->Target ? "cell" : "none"), pTarget && pTarget->IsInAir() ? 1 : 0,
				pTarget ? pTarget->GetHeight() : 0, pTarget ? pUnit->DistanceFrom(pTarget) / Unsorted::LeptonsPerCell : -1,
				pNearest ? pNearest->GetTechnoType()->ID : "none", pNearest && pNearest->IsInAir() ? 1 : 0,
				pNearest ? nearestDist / Unsorted::LeptonsPerCell : -1,
				GroundRangeOf(pUnit->Type->DeploysInto) / Unsorted::LeptonsPerCell, now);
		}
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

		auto& watch = GetMacroState(pHouse).UnloadWatch;
		const int now = Unsorted::CurrentFrame;
		std::unordered_set<DWORD> seen;

		auto const handle = [&](FootClass* pTrans)
		{
			auto const pType = pTrans ? pTrans->GetTechnoType() : nullptr;

			if (!pType || pType->Passengers <= 0 || pTrans->Owner != pHouse || !pTrans->IsAlive || pTrans->InLimbo
				|| pTrans->CurrentMission != Mission::Unload)
			{
				return;
			}

			const CellStruct here = pTrans->GetMapCoords();
			seen.insert(pTrans->UniqueID);
			auto [it, inserted] = watch.try_emplace(pTrans->UniqueID, here, now);

			if (!inserted && it->second.first != here)
				it->second = { here, now };

			if (now - it->second.second < 450)
				return;

			it->second = { here, now };

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

		std::erase_if(watch, [&](const auto& entry) { return !seen.contains(entry.first); });
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
				|| pUnit->GetHealthPercentage() < 0.25)
			{
				continue;
			}

			// Armed transports and carryalls (a transport helicopter, a Nighthawk) ferry first: they only
			// strike while empty (no passengers, nothing slung), with nobody waiting for a ferry (fresh
			// demand, no passenger reserved) and only targets within 30 cells. A far strike left them
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

				if (!empty || ferryWanted || IsFerryLoading(pHouse, pUnit->UniqueID))
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
			if (pUnit && pUnit->Owner == pHouse && pUnit->IsAlive && !pUnit->InLimbo && IsCapitalUnitType(pUnit->Type)
				&& !macro.PlanMembers.contains(pUnit->UniqueID) && !IsOnRepairTrip(pHouse, pUnit->UniqueID))
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
				&& pUnit->Type->SensorsSight > 0 && IsCellInSightOfAI(pHouse, pUnit->GetMapCoords()))
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

		for (auto const pSub : UnitClass::Array)
		{
			if (!pSub || pSub->Owner != pHouse || !pSub->IsAlive || pSub->InLimbo || !pSub->Type || !pSub->Type->Naval
				|| !pSub->Type->Cloakable || pSub->Target != nullptr || pSub->CurrentMission == Mission::Attack)
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
					CountDecision(NeuralDecision::SubmarineSonarAvoided, pHouse->ArrayIndex);
				}
			}
			else if (!pDestCell && inSonar(here))
			{
				if (auto const pSafe = nearestHidden(pSub, here, 12))
				{
					pSub->SetDestination(pSafe, true);
					pSub->QueueMission(Mission::Move, true);
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
		const NeuralSlowStep slowStep { "UpdateMapObstacles" };

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

		// Resolve our own units by UniqueID once per tick.
		std::unordered_map<DWORD, FootClass*> ownFoots;

		for (auto const pTech : TechnoClass::Array)
		{
			if (pTech && pTech->Owner == pHouse && pTech->IsAlive && !pTech->InLimbo)
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
				NeuralDeployOrderFrame[pUnit->UniqueID] = Unsorted::CurrentFrame;
				pUnit->SetDestination(nullptr, true);
				pUnit->ClearNavigationList();
				pUnit->SetTarget(nullptr);
				pUnit->QueueMission(Mission::Unload, true);
			}

			return true;
		};

		auto isFreeForPlan = [&](FootClass* pFoot) -> bool
		{
			if (!pFoot || macro.PlanMembers.contains(pFoot->UniqueID) || IsOnRepairTrip(pHouse, pFoot->UniqueID))
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
			const bool marchingInWave = pFoot->MegaMission == Mission::AttackMove && pFoot->Target == nullptr
				&& (pFoot->CurrentMission == Mission::Move || pFoot->CurrentMission == Mission::Guard);

			return idle || marchingInWave;
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
					houseIdx, NeuralPlanNames[static_cast<size_t>(id)], success ? "well" : "badly", rule,
					Unsorted::CurrentFrame - plan.StartFrame, plan.Deadline - plan.StartFrame, alive,
					static_cast<int>(plan.Members.size()), memberValue(plan), plan.InitialValue, plan.Arrived ? 1 : 0, nearest,
					plan.TargetCell.X, plan.TargetCell.Y, lostDead, lostOffMap, lostTraced > 0 ? lostDistance / lostTraced : -1, lostTraced);

				// TEMP diagnostic: a group that ends badly almost whole never got there. Where it stands
				// and what its members are doing (63 Factory Decapitation, 62 Superweapon Race and 37
				// Tech Tree Cut plans ran out of time with nine tenths of their value, 58 to 95 cells
				// from the target, in the focus batch of 2026-11-13).
				if (!success && alive > 0 && memberValue(plan) * 100 >= plan.InitialValue * 80)
				{
					int sumX = 0;
					int sumY = 0;
					int onMap = 0;
					int held = 0;
					std::map<int, int> missions;
					std::string aircraftText;

					for (const DWORD uid : plan.Members)
					{
						auto const it = ownFoots.find(uid);

						if (it == ownFoots.end())
							continue;

						const CellStruct cell = it->second->GetMapCoords();
						sumX += cell.X;
						sumY += cell.Y;
						onMap++;
						missions[static_cast<int>(it->second->CurrentMission)]++;

						if (auto const pAirMember = abstract_cast<AircraftClass*>(it->second))
						{
							char air[96];
							std::snprintf(air, sizeof(air), " %s ammo %d/%d target %d air %d docked %d hp %d", pAirMember->Type->ID, pAirMember->Ammo,
								pAirMember->Type->Ammo, pAirMember->Target ? 1 : 0, pAirMember->IsInAir() ? 1 : 0,
								pAirMember->GetNthLink(0) ? 1 : 0, static_cast<int>(pAirMember->GetHealthPercentage() * 100));
							aircraftText += air;
						}

						if (IsFootHeldStill(pHouse, it->second, 300))
							held++;
					}

					if (onMap > 0)
					{
						const CellStruct centre { static_cast<short>(sumX / onMap), static_cast<short>(sumY / onMap) };
						std::string missionText;

						for (auto const& [mission, count] : missions)
							missionText += "m" + std::to_string(mission) + "x" + std::to_string(count) + " ";

						Debug::Log("[NeuralAI] House %d plan %s intact DIAG at rule %d: %d members around (%d,%d), %d cells from home and %d from "
							"the target, %d held still 300 frames, staging (%d,%d), zones home %d group %d target %d, missions %s| aircraft:%s\n",
							houseIdx, NeuralPlanNames[static_cast<size_t>(id)], rule, onMap, centre.X, centre.Y,
							static_cast<int>(centre.DistanceFrom(homeCell)), static_cast<int>(centre.DistanceFrom(plan.TargetCell)), held,
							plan.StagingCell.X, plan.StagingCell.Y, homeZone,
							MapClass::Instance.GetMovementZoneType(centre, MovementZone::Normal, false),
							MapClass::Instance.GetMovementZoneType(plan.TargetCell, MovementZone::Normal, false), missionText.c_str(),
							aircraftText.empty() ? " none" : aircraftText.c_str());
					}
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
							|| it->second->GetTechnoType()->Naval)
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

			CountPlanOutcome(houseIdx, id, success ? NeuralPlanOutcome::Succeeded : NeuralPlanOutcome::Failed);

			if (!success && Unsorted::CurrentFrame >= plan.Deadline)
				CountPlanOutcome(houseIdx, id, NeuralPlanOutcome::TimedOut);

			plan = NeuralPlanInstance {};
			macro.PlanNextAttemptFrame[static_cast<size_t>(id)] = Unsorted::CurrentFrame + (success ? 1800 : 2700);
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

		auto shouldTryStart = [&](NeuralPlanId id, int contextUtility) -> bool
		{
			const size_t i = static_cast<size_t>(id);

			if (macro.Plans[i].Active || Unsorted::CurrentFrame < macro.PlanNextAttemptFrame[i])
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

			bool open = SlotGate(static_cast<int>(scores[slot]) + contextUtility + (contextUtility > 0 ? boost : 0) + advantage, 300, slot);

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
		// group of aircraft alone is not weighed.
		auto raidMayStart = [&](NeuralPlanId id, std::vector<FootClass*> const& group, const CellStruct& target, int targetOwner) -> bool
		{
			constexpr NeuralPlanId gated[] = { NeuralPlanId::EcoRaid, NeuralPlanId::Blackout, NeuralPlanId::Feint,
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

			// TEMP diagnostic: the raids the gate holds back (20 per match).
			static int gateLogSerial = -1;
			static int gateLogCount = 0;

			if (gateLogSerial != NeuralMatchSerial)
			{
				gateLogSerial = NeuralMatchSerial;
				gateLogCount = 0;
			}

			if (gateLogCount++ < 20)
			{
				Debug::Log("[NeuralAI] House %d plan %s not started: ground group worth %d against %d known on its way to (%d,%d), lesson of house %d %s, frame %d.\n",
					houseIdx, NeuralPlanNames[static_cast<size_t>(id)], groundValue, strength, target.X, target.Y, targetOwner,
					taught ? "asks for more" : "allows it", Unsorted::CurrentFrame);
			}

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

		// ---------------------------------------------------------------- Plan 2: Siege Battery
		// Against a turtle: artillery that outranges a known defense sets up just outside its
		// reach on our side, escorted, and shells it (the kiting micro keeps the distance).
		{
			auto& plan = macro.Plans[static_cast<size_t>(NeuralPlanId::SiegeBattery)];

			if (plan.Active)
			{
				auto const pDefense = FindBuildingByID(plan.TargetID);
				const int value = memberValue(plan);

				if (!pDefense)
				{
					finishPlan(NeuralPlanId::SiegeBattery, true, false);
				}
				else if (value * 100 < plan.InitialValue * 40 || Unsorted::CurrentFrame >= plan.Deadline)
				{
					finishPlan(NeuralPlanId::SiegeBattery, false, true);
				}
				else if (auto const pStaging = MapClass::Instance.TryGetCellAt(plan.StagingCell))
				{
					const int defenseRange = GroundRangeOf(pDefense->Type);

					for (const DWORD uid : plan.Members)
					{
						if (handleSiegeDeployMember(uid, pDefense, pStaging))
							continue;

						auto const it = ownFoots.find(uid);

						if (it == ownFoots.end())
							continue;

						auto const pFoot = it->second;
						const bool outranges = SiegeRangeOf(pFoot->GetTechnoType()) >= defenseRange + 2 * Unsorted::LeptonsPerCell;
						const bool atStaging = pFoot->GetMapCoords().DistanceFrom(plan.StagingCell) <= 3.0;

						if (outranges && atStaging)
						{
							if (pFoot->Target != pDefense)
							{
								pFoot->SetTarget(pDefense);
								pFoot->QueueMission(Mission::Attack, true);
							}
						}
						else if (!atStaging && pFoot->Target == nullptr)
						{
							IssueMoveOrder(pFoot, pStaging, 2.0);
						}
					}
				}
			}
			else if (foe.KnownDefenses >= 3)
			{
				// Pick the known enemy defense closest to our base that some idle artillery outranges.
				std::vector<FootClass*> artillery;
				std::vector<FootClass*> escorts;
				int bestArtilleryRange = 0;

				for (auto const& [uid, pFoot] : ownFoots)
				{
					auto const pUnit = abstract_cast<UnitClass*>(pFoot);

					if (!pUnit || !pUnit->Type || pUnit->Type->Naval || pUnit->Type->Harvester
						|| (pUnit->Type->DeploysInto && !IsDeployableSiegeUnitType(pUnit->Type))
						|| IsAmphibiousOrAirTransport(pUnit->Type) || !isFreeForPlan(pUnit)
						|| ObjectZone(pUnit, MovementZone::Normal) != homeZone)
					{
						continue;
					}

					const int range = SiegeRangeOf(pUnit->Type);

					if (range >= 8 * Unsorted::LeptonsPerCell)
					{
						artillery.push_back(pUnit);
						bestArtilleryRange = std::max(bestArtilleryRange, range);
					}
					else if (range > 0)
					{
						escorts.push_back(pUnit);
					}
				}

				BuildingClass* pBestDefense = nullptr;
				double bestDist = 1e9;

				for (auto const pBld : BuildingClass::Array)
				{
					if (!pBld || pBld->InLimbo || !pBld->IsAlive || !pBld->Type || !pBld->Type->IsBaseDefense || !pBld->Owner
						|| pBld->Owner == pHouse || pHouse->IsAlliedWith(pBld->Owner) || pBld->Owner->IsNeutral()
						|| !IsEnemyBuildingKnown(pHouse, pBld))
					{
						continue;
					}

					const int defenseRange = GroundRangeOf(pBld->Type);

					if (defenseRange <= 0 || bestArtilleryRange < defenseRange + 2 * Unsorted::LeptonsPerCell)
						continue;

					const double d = pBld->GetMapCoords().DistanceFrom(homeCell);

					if (d < bestDist)
					{
						bestDist = d;
						pBestDefense = pBld;
					}
				}

				// Deterministic member choice on every client.
				std::sort(artillery.begin(), artillery.end(), [](FootClass* a, FootClass* b) { return a->UniqueID < b->UniqueID; });

				const int utility = pBestDefense ? (200 + std::clamp(foe.KnownDefenses * 30, 0, 300)) : -1000;

				if (pBestDefense && artillery.size() >= 2 && shouldTryStart(NeuralPlanId::SiegeBattery, utility))
				{
					// Staging cell on the line defense -> home, just outside the defense's reach.
					const CellStruct dc = pBestDefense->GetMapCoords();
					const double dx = homeCell.X - dc.X;
					const double dy = homeCell.Y - dc.Y;
					const double len = std::max(1.0, std::sqrt(dx * dx + dy * dy));
					const double standOff = GroundRangeOf(pBestDefense->Type) / static_cast<double>(Unsorted::LeptonsPerCell) + 2.5;
					CellStruct staging { static_cast<short>(dc.X + dx / len * standOff), static_cast<short>(dc.Y + dy / len * standOff) };
					bool stagingOk = false;

					for (int r = 0; r <= 3 && !stagingOk; ++r)
					{
						for (int oy = -r; oy <= r && !stagingOk; ++oy)
						{
							for (int ox = -r; ox <= r && !stagingOk; ++ox)
							{
								const CellStruct cand { static_cast<short>(staging.X + ox), static_cast<short>(staging.Y + oy) };
								auto const pCand = MapClass::Instance.TryGetCellAt(cand);

								if (pCand && pCand->LandType != LandType::Water && !pCand->GetBuilding()
									&& MapClass::Instance.GetMovementZoneType(cand, MovementZone::Normal, false) == homeZone
									&& pCand->IsClearToMove(SpeedType::Track, true, true, -1, MovementZone::Normal, -1, false))
								{
									staging = cand;
									stagingOk = true;
								}
							}
						}
					}

					if (stagingOk)
					{
						std::vector<FootClass*> group;

						for (auto const pArt : artillery)
						{
							if (GroundRangeOf(pArt->GetTechnoType()) >= GroundRangeOf(pBestDefense->Type) + 2 * Unsorted::LeptonsPerCell)
								group.push_back(pArt);

							if (group.size() >= 6)
								break;
						}

						std::sort(escorts.begin(), escorts.end(), [](FootClass* a, FootClass* b) { return a->UniqueID < b->UniqueID; });

						for (size_t e = 0; e < escorts.size() && e < 3; ++e)
							group.push_back(escorts[e]);

						if (group.size() >= 2)
							startPlan(NeuralPlanId::SiegeBattery, group, dc, staging, pBestDefense->UniqueID, 5400);
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
						|| IsAmphibiousOrAirTransport(pUnit->Type)))
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

		auto commandGroupAttack = [&](NeuralPlanInstance& plan, AbstractClass* pTarget)
		{
			for (const DWORD uid : plan.Members)
			{
				auto const it = ownFoots.find(uid);

				if (it == ownFoots.end() || IsOnRepairTrip(pHouse, uid))
					continue;

				// A resting aircraft is sent only when it is armed (ReadyPlanAircraft); anything else
				// when its target is another one.
				auto const pAir = abstract_cast<AircraftClass*>(it->second);
				const bool resting = pAir && (pAir->CurrentMission == Mission::Guard || pAir->CurrentMission == Mission::Area_Guard
					|| pAir->CurrentMission == Mission::Sleep);

				if (resting ? ReadyPlanAircraft(pAir) : it->second->Target != pTarget)
				{
					it->second->SetTarget(pTarget);
					it->second->QueueMission(Mission::Attack, true);
				}
			}
		};

		// ---------------------------------------------------------------- Plan 4: Blackout
		// The enemy relies on powered defenses: take out a known power plant (by air when
		// possible, else with a fast ground group if it is undefended) and open a window in
		// which our main wave launches earlier, while those defenses are offline.
		{
			auto& plan = macro.Plans[static_cast<size_t>(NeuralPlanId::Blackout)];

			if (plan.Active)
			{
				auto const pPlant = FindBuildingByID(plan.TargetID);
				const int value = memberValue(plan);

				if (!pPlant)
				{
					macro.BlackoutUntilFrame = Unsorted::CurrentFrame + 1200;
					finishPlan(NeuralPlanId::Blackout, true, true);
				}
				else if (value * 100 < plan.InitialValue * 40 || Unsorted::CurrentFrame >= plan.Deadline)
				{
					finishPlan(NeuralPlanId::Blackout, false, true);
				}
				else
				{
					commandGroupAttack(plan, pPlant);
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
					std::vector<FootClass*> group;

					for (auto const& [uid, pFoot] : ownFoots)
					{
						auto const pAir = abstract_cast<AircraftClass*>(pFoot);

						if (isFreeAirStriker(pAir))
						{
							group.push_back(pAir);
						}
					}

					std::sort(group.begin(), group.end(), [](FootClass* a, FootClass* b) { return a->UniqueID < b->UniqueID; });

					const CellStruct plantCell = pBestPlant->GetMapCoords();
					const bool groundReachable = MapClass::Instance.GetMovementZoneType(plantCell, MovementZone::Normal, pBestPlant->OnBridge) == homeZone
						&& CountKnownEnemyDefensesNear(pHouse, plantCell, 9.0, false) == 0;

					if (group.size() < 2 && groundReachable)
					{
						group = collectIdleGround(false);
						std::stable_sort(group.begin(), group.end(), [](FootClass* a, FootClass* b)
							{ return a->GetTechnoType()->Speed > b->GetTechnoType()->Speed; });

						if (group.size() > 5)
							group.resize(5);

						if (group.size() < 3)
							group.clear();
					}

					if (group.size() >= 2)
					{
						if (group.size() > 6)
							group.resize(6);

						startPlan(NeuralPlanId::Blackout, group, plantCell, CellStruct::Empty, pBestPlant->UniqueID, 3600);
						commandGroupAttack(macro.Plans[static_cast<size_t>(NeuralPlanId::Blackout)], pBestPlant);
					}
				}
			}
		}

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
		// defenses and the units seen not longer ago than maxAge within PlanStrikeGuardCells of the
		// target or of the last PlanStrikeApproachCells of the way, each counted once.
		auto knownEnemyStrengthOnWay = [&](const CellStruct& from, const CellStruct& to, int maxAge) -> int
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
				if (isKnownEnemyBuilding(pBld) && pBld->Type->IsBaseDefense && onWay(pBld->GetMapCoords()))
					strength += GetCombatValueOfType(pBld->Type, false);
			}

			return strength;
		};

		// ---------------------------------------------------------------- Plan 5: Feint
		// While our army gathers, a cheap decoy group attacks a known enemy building far from
		// the main approach to pull defenders away. Success = enemy forces seen gathering at
		// the decoy target after it arrives.
		{
			auto& plan = macro.Plans[static_cast<size_t>(NeuralPlanId::Feint)];

			auto enemyValueNear = [&](const CellStruct& cell) -> int
			{
				int value = 0;

				for (auto const& [uid, sighting] : macro.EnemySightings)
				{
					if (Unsorted::CurrentFrame - sighting.LastSeenFrame <= 150 && sighting.LastCoords.DistanceFrom(cell) <= 10.0)
						value += sighting.Value;
				}

				return value;
			};

			if (plan.Active)
			{
				const int value = memberValue(plan);
				auto const pDecoyCell = MapClass::Instance.TryGetCellAt(plan.TargetCell);

				if (plan.Arrived)
					plan.AuxPeak = std::max(plan.AuxPeak, enemyValueNear(plan.TargetCell));

				const bool drewDefenders = plan.Arrived && plan.AuxPeak >= plan.AuxBaseline + 60;

				if (drewDefenders)
				{
					finishPlan(NeuralPlanId::Feint, true, true);
				}
				else if (!pDecoyCell || value * 100 < plan.InitialValue * 25 || Unsorted::CurrentFrame >= plan.Deadline
					|| (plan.Arrived && Unsorted::CurrentFrame - plan.ArrivedFrame >= 900))
				{
					finishPlan(NeuralPlanId::Feint, false, true);
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

						const CellStruct fc = it->second->GetMapCoords();
						sumX += fc.X;
						sumY += fc.Y;
						alive++;

						if (it->second->Target == nullptr && it->second->MegaMission != Mission::AttackMove)
							OrderFootToAttackMove(it->second, pDecoyCell);
					}

					if (alive > 0 && !plan.Arrived
						&& CellStruct { static_cast<short>(sumX / alive), static_cast<short>(sumY / alive) }.DistanceFrom(plan.TargetCell) <= 8.0)
					{
						plan.Arrived = true;
						plan.ArrivedFrame = Unsorted::CurrentFrame;
						plan.AuxBaseline = enemyValueNear(plan.TargetCell);
						plan.AuxPeak = plan.AuxBaseline;
					}
				}
			}
			else
			{
				std::vector<FootClass*> idleArmy = collectIdleGround(false);
				BuildingClass* pMain = nullptr;
				double mainDist = 1e9;
				int sumX = 0;
				int sumY = 0;
				int known = 0;

				for (auto const pBld : knownEnemyBuildingList)
				{
					if (!isKnownEnemyBuilding(pBld)
						|| ObjectZone(pBld, MovementZone::Normal) != homeZone)
					{
						continue;
					}

					const CellStruct bc = pBld->GetMapCoords();
					sumX += bc.X;
					sumY += bc.Y;
					known++;

					if (bc.DistanceFrom(homeCell) < mainDist)
					{
						mainDist = bc.DistanceFrom(homeCell);
						pMain = pBld;
					}
				}

				// The decoy group is the four cheapest idle units: a building where such a group already
				// failed is no decoy target while that lesson stands.
				std::vector<FootClass*> decoyGroup = idleArmy;
				std::stable_sort(decoyGroup.begin(), decoyGroup.end(), [](FootClass* a, FootClass* b)
					{ return a->GetTechnoType()->Cost < b->GetTechnoType()->Cost; });
				decoyGroup.resize(std::min<size_t>(decoyGroup.size(), 4));
				const int decoyValue = groupValueOf(decoyGroup);

				// Decoy target: a known building on the far side of the enemy base, away from
				// the main approach (angle > ~70 degrees around the enemy centroid).
				BuildingClass* pDecoy = nullptr;

				if (pMain && known >= 4 && idleArmy.size() >= 8)
				{
					const double cx = sumX / static_cast<double>(known);
					const double cy = sumY / static_cast<double>(known);
					const CellStruct mc = pMain->GetMapCoords();
					const double mx = mc.X - cx;
					const double my = mc.Y - cy;
					const double mLen = std::max(1.0, std::sqrt(mx * mx + my * my));
					double bestDecoyScore = -1e9;

					for (auto const pBld : knownEnemyBuildingList)
					{
						if (!isKnownEnemyBuilding(pBld) || pBld == pMain || !targetLessonAllows(pBld->UniqueID, decoyValue))
							continue;

						const CellStruct bc = pBld->GetMapCoords();

						// A decoy has to get there: one whose way in is held by more than the decoy group is
						// worth is no decoy (it arrived once in a hundred starts and drew nobody).
						if (knownEnemyStrengthOnWay(homeCell, bc, PlanStrikeGuardFrames) > decoyValue)
							continue;

						if (MapClass::Instance.GetMovementZoneType(bc, MovementZone::Normal, pBld->OnBridge) != homeZone
							|| bc.DistanceFrom(mc) < 15.0)
						{
							continue;
						}

						const double bx = bc.X - cx;
						const double by = bc.Y - cy;
						const double bLen = std::max(1.0, std::sqrt(bx * bx + by * by));
						const double cosAngle = (mx * bx + my * by) / (mLen * bLen);

						if (cosAngle > 0.35)
							continue;

						const double decoyScore = -cosAngle * 100.0 - CountKnownEnemyDefensesNear(pHouse, bc, 8.0, false) * 40.0;

						if (decoyScore > bestDecoyScore)
						{
							bestDecoyScore = decoyScore;
							pDecoy = pBld;
						}
					}
				}

				const int utility = pDecoy ? (200 + std::clamp(static_cast<int>(idleArmy.size()) * 15, 0, 200)) : -1000;

				if (pDecoy && shouldTryStart(NeuralPlanId::Feint, utility))
				{
					// The cheapest units make the decoy; the main army stays for the real wave.
					std::stable_sort(idleArmy.begin(), idleArmy.end(), [](FootClass* a, FootClass* b)
						{ return a->GetTechnoType()->Cost < b->GetTechnoType()->Cost; });
					idleArmy.resize(std::min<size_t>(idleArmy.size(), 4));
					startPlan(NeuralPlanId::Feint, idleArmy, pDecoy->GetMapCoords(), CellStruct::Empty, pDecoy->UniqueID, 3600);
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
						{
							pAir->SetTarget(pTarget);
							pAir->QueueMission(Mission::Attack, true);
						}
					}
				}
			}
			else
			{
				std::vector<FootClass*> strikers;

				for (auto const& [uid, pFoot] : ownFoots)
				{
					auto const pAir = abstract_cast<AircraftClass*>(pFoot);

					if (isFreeAirStriker(pAir))
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
					{
						pStriker->SetTarget(pBestTarget);
						pStriker->QueueMission(Mission::Attack, true);
					}
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

				if (pFoot && pFoot->Target == nullptr && pFoot->MegaMission != Mission::AttackMove)
					OrderFootToAttackMove(pFoot, pCell);
			}
		};

		auto moveGroup = [&](const std::vector<DWORD>& uids, CellClass* pCell, double keepWithin)
		{
			if (!pCell)
				return;

			for (const DWORD uid : uids)
			{
				if (auto const pFoot = findOrderableFoot(uid))
					IssueMoveOrder(pFoot, pCell, keepWithin);
			}
		};

		auto attackTargetGroup = [&](const std::vector<DWORD>& uids, TechnoClass* pTarget)
		{
			for (const DWORD uid : uids)
			{
				auto const pFoot = findOrderableFoot(uid);

				if (pFoot && pTarget && pFoot->Target != pTarget)
				{
					pFoot->SetTarget(pTarget);
					pFoot->QueueMission(Mission::Attack, true);
				}
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
			for (int r = 0; r <= searchRadius; ++r)
			{
				for (int dy = -r; dy <= r; ++dy)
				{
					for (int dx = -r; dx <= r; ++dx)
					{
						if (std::max(std::abs(dx), std::abs(dy)) != r)
							continue;

						const CellStruct c { static_cast<short>(center.X + dx), static_cast<short>(center.Y + dy) };

						if (!MapClass::Instance.CoordinatesLegal(c))
							continue;

						auto const pCell = MapClass::Instance.TryGetCellAt(c);

						if (pCell && pCell->LandType != LandType::Water
							&& (zone < 0 || MapClass::Instance.GetMovementZoneType(c, MovementZone::Normal, false) == zone))
						{
							return pCell;
						}
					}
				}
			}

			return nullptr;
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
		// 2026-11-14).
		{
			constexpr NeuralPlanId raidPlans[] = { NeuralPlanId::EcoRaid, NeuralPlanId::Blackout, NeuralPlanId::Feint,
				NeuralPlanId::TimingRush, NeuralPlanId::ExpansionDenial, NeuralPlanId::SuperweaponRace, NeuralPlanId::HarvesterAmbush,
				NeuralPlanId::PreemptiveStrike, NeuralPlanId::RotatingRaids, NeuralPlanId::ArtilleryEcoSiege,
				NeuralPlanId::FactoryDecapitation, NeuralPlanId::CutReinforcements, NeuralPlanId::TechTreeCut,
				NeuralPlanId::RollingSiege, NeuralPlanId::SiegeBattery };

			for (const NeuralPlanId id : raidPlans)
			{
				auto const& plan = planOf(id);

				if (!plan.Active || plan.TargetCell == CellStruct::Empty)
					continue;

				int sumX = 0;
				int sumY = 0;
				int alive = 0;
				int strength = 0;

				for (const DWORD uid : plan.Members)
				{
					auto const pFoot = findOwnFoot(uid);

					if (!pFoot || pFoot->WhatAmI() == AbstractType::Aircraft || pFoot->GetTechnoType()->Naval || pFoot->IsInAir())
						continue;

					const CellStruct cell = pFoot->GetMapCoords();
					sumX += cell.X;
					sumY += cell.Y;
					alive++;
					strength += static_cast<int>(GetCombatValueOfType(pFoot->GetTechnoType(), false) * pFoot->GetHealthPercentage());
				}

				if (alive == 0)
					continue;

				const CellStruct centre { static_cast<short>(sumX / alive), static_cast<short>(sumY / alive) };
				double toGoal = centre.DistanceFrom(plan.TargetCell);

				if (plan.StagingCell != CellStruct::Empty)
					toGoal = std::min(toGoal, centre.DistanceFrom(plan.StagingCell));

				if (toGoal <= PlanOutgunnedCommittedCells)
					continue;

				int enemy = seenEnemyValueNear(centre, PlanOutgunnedSightCells, PlanOutgunnedSightFrames);

				for (auto const pBld : knownEnemyBuildingList)
				{
					if (!isKnownEnemyBuilding(pBld) || !pBld->Type->IsBaseDefense)
						continue;

					const int range = GroundRangeOf(pBld->Type);

					if (range > 0 && pBld->GetMapCoords().DistanceFrom(centre) * Unsorted::LeptonsPerCell <= range + 2 * Unsorted::LeptonsPerCell)
						enemy += GetCombatValueOfType(pBld->Type, false);
				}

				// Losses before arriving tell the same when the shooters are out of the group's sight
				// (long guns, aircraft, defenses it does not know yet).
				const bool outgunned = enemy * 100 > strength * PlanOutgunnedRatioPercent;
				const bool bleeding = memberValue(plan) * 100 < plan.InitialValue * PlanEnRouteLossPercent;

				if (!outgunned && !bleeding)
					continue;

				Debug::Log("[NeuralAI] House %d plan %s %s %d cells from its goal: enemy %d against %d around (%d,%d).\n", houseIdx,
					NeuralPlanNames[static_cast<size_t>(id)], outgunned ? "outgunned" : "losing members", static_cast<int>(toGoal), enemy,
					strength, centre.X, centre.Y);
				abortPlan(id, outgunned ? NeuralDecision::PlanAbortEnemyGrew : NeuralDecision::PlanAbortValueLost, strength);
				macro.PlanNextAttemptFrame[static_cast<size_t>(id)] = Unsorted::CurrentFrame + PlanOutgunnedRetryFrames;
			}
		}

		// ---------------------------------------------------------------- Plan 41: Base Threat Intercept
		// Visible enemy ground units with orders against our or an ally's base are met on the way by
		// an idle group sized to beat them, before they get there. A unit is a base threat when its
		// target is one of those buildings or harvesters, when its move or attack-move destination
		// lies next to one of those buildings, or when its course over the last sightings leads into
		// them. Only units in our current sight count. The group comes from free units near the
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
				const bool leadSeen = pLead && IsCellInSightOfAI(pHouse, pLead->GetMapCoords());
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
				};

				std::vector<BaseThreat> threats;

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

					if (!IsCellInSightOfAI(pHouse, cell) || ObjectZone(pFoot, MovementZone::Normal) != homeZone)
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

					// Far from the building it is after, it is not a base threat yet (it may never come).
					if (goal != CellStruct::Empty && cell.DistanceFrom(goal) <= 35.0)
						threats.push_back({ pFoot, cell, goal, value, cell.DistanceFrom(goal), ownGoal, reason });
				}

				if (!threats.empty())
				{
					CountDecision(NeuralDecision::BaseThreatTick, houseIdx);

					// The most urgent first: closest to the building it is after, ties by UniqueID.
					std::sort(threats.begin(), threats.end(), [](const BaseThreat& a, const BaseThreat& b)
						{
							return a.GoalDistance != b.GoalDistance ? a.GoalDistance < b.GoalDistance : a.Techno->UniqueID < b.Techno->UniqueID;
						});

					const std::vector<FootClass*> idle = collectIdleGround(false);
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
					{
						pAir->SetTarget(pTarget);
						pAir->QueueMission(Mission::Attack, true);
					}
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
		// minGround..maxGround idle ground units when it is on our landmass.
		auto pickStrikeGroup = [&](BuildingClass* pTarget, size_t minGround, size_t maxGround) -> std::vector<FootClass*>
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

		// ---------------------------------------------------------------- Plan 38: Combo Watch Strike
		// Chain (point 56): our sentries spot an enemy force -> preemptive strike, or fall back
		// under the towers if it is too strong; the counterattack window follows on its own.
		{
			auto& plan = planOf(NeuralPlanId::ComboWatchStrike);
			auto const& pickets = planOf(NeuralPlanId::Pickets);

			if (plan.Active)
			{
				if (planOf(NeuralPlanId::PreemptiveStrike).Active || planOf(NeuralPlanId::HoldUnderTowers).Active)
					finishPlan(NeuralPlanId::ComboWatchStrike, true, false);
				else if (Unsorted::CurrentFrame >= plan.Deadline)
					finishPlan(NeuralPlanId::ComboWatchStrike, false, false);
				else
				{
					boostPlan(NeuralPlanId::PreemptiveStrike, 350);
					boostPlan(NeuralPlanId::HoldUnderTowers, 250);
				}
			}
			else if (planReady(NeuralPlanId::ComboWatchStrike) && pickets.Active && pickets.AuxPeak > pickets.AuxBaseline)
			{
				planOf(NeuralPlanId::Pickets).AuxBaseline = pickets.AuxPeak;

				if (shouldTryStart(NeuralPlanId::ComboWatchStrike, 320))
				{
					startPlan(NeuralPlanId::ComboWatchStrike, std::vector<FootClass*> {}, homeCell, CellStruct::Empty, 0, 900);
					boostPlan(NeuralPlanId::PreemptiveStrike, 350);
					boostPlan(NeuralPlanId::HoldUnderTowers, 250);
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
					// TEMP diagnostic: what the island yard had when its time ran out.
					Debug::Log("[NeuralAI] House %d IslandHold DIAG: out of time at (%d,%d) with %d ground defenses and %d anti-air, enemy air seen %d, credits %d.\n",
						houseIdx, pYard->GetMapCoords().X, pYard->GetMapCoords().Y, ground, antiAir, enemyAirSeen ? 1 : 0, pHouse->Available_Money());
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
		// While our ground army is attacking, an air transport (passenger aircraft or flying
		// transport vehicle) drops infantry on a known valuable building away from the fighting,
		// flying the safe air lane. Members[0] = transport, the rest = infantry.
		// AuxBaseline = phase: 0 boarding, 1 flying, 2 dropped.
		{
			auto& plan = planOf(NeuralPlanId::AirborneFront);

			if (plan.Active)
			{
				FootClass* const pTransport = plan.Members.empty() ? nullptr : findOwnFoot(plan.Members[0]);
				auto const pTarget = FindBuildingByID(plan.TargetID);
				auto const pDrop = MapClass::Instance.TryGetCellAt(plan.StagingCell);
				const std::vector<DWORD> infantry(plan.Members.empty() ? plan.Members.end() : plan.Members.begin() + 1, plan.Members.end());
				auto const pTransportUnit = abstract_cast<UnitClass*>(pTransport);
				const bool underground = pTransportUnit && IsUndergroundTransportType(pTransportUnit->Type);
				constexpr const char* PhaseNames[] = { "boarding", "on the way", "dropped" };
				auto logEnd = [&](const char* reason)
				{
					Debug::Log("[NeuralAI] House %d AirborneFront: ends while %s (%s), frame %d.\n", pHouse->ArrayIndex,
						PhaseNames[std::clamp(plan.AuxBaseline, 0, 2)], reason, Unsorted::CurrentFrame);
				};

				if (!pTarget)
				{
					logEnd("target gone");
					finishPlan(NeuralPlanId::AirborneFront, plan.AuxBaseline >= 2, true);
				}
				else if (Unsorted::CurrentFrame >= plan.Deadline || !pDrop)
				{
					logEnd("out of time");
					finishPlan(NeuralPlanId::AirborneFront, false, true);
				}
				else if (plan.AuxBaseline < 2 && !pTransport)
				{
					// Shot down (or lost) before the drop.
					logEnd("transport lost");
					finishPlan(NeuralPlanId::AirborneFront, false, false);
				}
				else if (plan.AuxBaseline == 0)
				{
					// Boarding: the infantry walk in (they are in limbo, hence not found, once aboard).
					// The transport comes to meet the nearest one (UniqueID on ties): a hovercraft left
					// at sea or a flyer parked across the base is out of the soldiers' reach otherwise.
					// One boards at a time (a transport has a single radio link for loading: all of them
					// ordered to Enter at once stole it from each other and jammed at the door, Heisenberg:
					// 10-15 "nobody boarded" per match with soldiers 0-2 cells away in Enter); the others
					// walk up and wait FerryWaitCells away for their turn.
					int waiting = 0;
					FootClass* pNearest = nullptr;
					int nearestDist = INT_MAX;
					FootClass* pBoarding = nullptr;

					for (const DWORD uid : infantry)
					{
						if (auto const pInf = findOwnFoot(uid))
						{
							waiting++;
							const int dist = pTransport->DistanceFrom(pInf);

							if (dist < nearestDist || (dist == nearestDist && pInf->UniqueID < pNearest->UniqueID))
							{
								nearestDist = dist;
								pNearest = pInf;
							}

							const bool enteringThis = (pInf->CurrentMission == Mission::Enter || pInf->QueuedMission == Mission::Enter)
								&& (pInf->ContainsLink(pTransport) || pInf->Destination == pTransport || pInf->ArchiveTarget == pTransport);

							if (enteringThis && (!pBoarding || pInf->ContainsLink(pTransport)))
								pBoarding = pInf;
						}
					}

					// The soldier nearest to the transport that holds the Enter order within
					// BoardingRestartCells of it, with no radio link to it, and has not moved for
					// BoardingRestartFrames starts over: its order is dropped here and given again below.
					// It stood there until the plan gave up after 1500 frames (a third of the Airborne
					// Front plans of the focus batch of 2026-11-13). AuxPeak keeps the last restart.
					if (pNearest && pNearest->CurrentMission == Mission::Enter && !pNearest->ContainsLink(pTransport)
						&& nearestDist <= BoardingRestartCells * Unsorted::LeptonsPerCell && IsFootHeldStill(pHouse, pNearest, BoardingRestartFrames)
						&& Unsorted::CurrentFrame - std::max(plan.AuxPeak, plan.StartFrame) >= BoardingRestartFrames)
					{
						plan.AuxPeak = Unsorted::CurrentFrame;
						pNearest->SetArchiveTarget(nullptr);
						pNearest->SetDestination(nullptr, true);
						pNearest->ForceMission(Mission::Guard);
						Debug::Log("[NeuralAI] House %d AirborneFront: boarding of %s %u started over, %d leptons from %s, frame %d.\n",
							pHouse->ArrayIndex, pNearest->GetTechnoType()->ID, pNearest->UniqueID, nearestDist, pTransport->GetTechnoType()->ID,
							Unsorted::CurrentFrame);

						if (pBoarding == pNearest)
							pBoarding = nullptr;
					}

					// The boarder that stands linked to the transport, next to it, and does not get in: it
					// stopped short of the centre of its cell, so the engine never ran the step that takes
					// it aboard (InfantryClass::Per_Cell_Process at the end of a move). Run it once it has
					// stood there BoardingSettleFrames (7 of 33 plans with nobody aboard, 2026-11-14).
					if (pBoarding)
						WatchBoarder(pBoarding, pTransport);

					if (pBoarding && pBoarding->ContainsLink(pTransport) && pBoarding->LastMapCoords != pBoarding->GetMapCoords()
						&& !pBoarding->Locomotor->Is_Moving() && pTransport->DistanceFrom(pBoarding) <= 2 * Unsorted::LeptonsPerCell
						&& IsFootHeldStill(pHouse, pBoarding, BoardingSettleFrames))
					{
						pBoarding->UpdatePosition(PCPType::End);
					}

					for (const DWORD uid : infantry)
					{
						auto const pInf = findOwnFoot(uid);

						if (!pInf || pInf == pBoarding)
							continue;

						// The nearest soldier asks to load once it is within BoardingRestartCells and the
						// door is open (OpenBoardingDoor: the transport at rest, the cells on record for
						// both in one zone). Asked earlier, the transport set out for the soldier instead of
						// taking it in and neither got anywhere (127 plans ended with nobody aboard in 46
						// matches, 145 boarding restarts did not help). Until then it walks up and waits
						// like the others.
						if (!pBoarding && pInf == pNearest && nearestDist <= BoardingRestartCells * Unsorted::LeptonsPerCell
							&& OpenBoardingDoor(pTransport, pInf))
						{
							OrderFootToEnter(pInf, pTransport);
							WatchBoarder(pInf, pTransport);
							pBoarding = pInf;
							continue;
						}

						if (pInf->CurrentMission == Mission::Enter || pInf->QueuedMission == Mission::Enter)
						{
							if (pInf->ContainsLink(pTransport))
								pInf->SendCommand(RadioCommand::NotifyUnlink, pTransport);

							pInf->SetArchiveTarget(nullptr);
							pInf->SetDestination(nullptr, true);
							pInf->ForceMission(Mission::Guard);
						}

						if (!HoldFerryWaiter(pInf, pTransport))
							IssueMoveOrder(pInf, pTransport, 3.0);
					}

					if (pNearest && pTransport->CurrentMission != Mission::Unload)
					{
						// A hover-attack flyer with a target stays in the air to shoot at it and never lands
						// for its soldiers: while it loads it has none.
						if (pTransport->Target && pTransport->IsInAir())
							pTransport->SetTarget(nullptr);

						auto const pCarrier = abstract_cast<UnitClass*>(pTransport);
						const int soldierZone = MapClass::Instance.GetMovementZoneType(pNearest->GetMapCoords(), MovementZone::Normal, pNearest->OnBridge);

						// Once it has parked by its soldiers it stays: it sets out again only when the nearest
						// one is BoardingParkedReachCells away. Leaving at 4 cells, it followed every soldier
						// the door routine scattered off the cells next to it, and a transport that moves
						// takes nobody in (half of the plans ended with the soldiers waiting within 4 cells
						// of a transport on Guard that was still moving).
						const int reachCells = plan.BoardingParked ? BoardingParkedReachCells : BoardingRestartCells;

						if (nearestDist > reachCells * Unsorted::LeptonsPerCell)
						{
							plan.BoardingParked = false;
							IssueMoveOrder(pTransport, pNearest->GetCell(), 2.0);
						}
						else if (pCarrier && MoveTransportToLoadingCell(pHouse, pCarrier, pCarrier->GetMapCoords(), soldierZone, true))
						{
							// On its way to a cell the soldiers can step onto: a flyer that hung over them
							// (the cells below taken) lands next to them instead, the ferry loader's way.
						}
						else
						{
							// Close enough: a moving transport takes nobody in, so it stops and waits.
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
								// locomotor still says it moves: the door stays shut for a moving transport (23 of
								// 81 plans with nobody aboard, 2026-11-14), so the locomotor is told to stop.
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

					if (aboard > 0 && (waiting == 0 || parkedFrames >= BoardingPartialLoadFrames))
						plan.AuxBaseline = 1;
					else if (aboard == 0 && nobodyCame)
					{
						// Not this transport again for a while: the same failure would repeat.
						auto const pTransportCell = pTransport->GetCell();
						Debug::Log("[NeuralAI] House %d AirborneFront: %s %u not boarded at (%d,%d)%s, nearest soldier %d cells away in mission %d.\n",
							pHouse->ArrayIndex, pTransport->GetTechnoType()->ID, pTransport->UniqueID, pTransport->GetMapCoords().X,
							pTransport->GetMapCoords().Y, (pTransportCell && pTransportCell->LandType == LandType::Water) ? " on water" : "",
							pNearest ? nearestDist / Unsorted::LeptonsPerCell : -1, pNearest ? static_cast<int>(pNearest->CurrentMission) : -1);

						// TEMP diagnostic: the state of both when a soldier stood next to the transport with
						// its Enter order and never got in.
						if (pNearest)
						{
							Debug::Log("[NeuralAI] House %d AirborneFront boarding DIAG: transport mission %d, in air %d, height %d, moving %d, passengers %d of %d, "
								"size limit %d, links soldier %d; soldier %s size %d, %d leptons away, height %d, links transport %d, destination is transport %d, "
								"queued mission %d.\n", pHouse->ArrayIndex, static_cast<int>(pTransport->CurrentMission), pTransport->IsInAir() ? 1 : 0,
								pTransport->GetHeight(), (pTransport->Locomotor && pTransport->Locomotor->Is_Moving()) ? 1 : 0,
								pTransport->Passengers.NumPassengers, pTransport->GetTechnoType()->Passengers,
								static_cast<int>(pTransport->GetTechnoType()->SizeLimit), pTransport->ContainsLink(pNearest) ? 1 : 0,
								pNearest->GetTechnoType()->ID, static_cast<int>(pNearest->GetTechnoType()->Size), nearestDist, pNearest->GetHeight(),
								pNearest->ContainsLink(pTransport) ? 1 : 0, pNearest->Destination == pTransport ? 1 : 0,
								static_cast<int>(pNearest->QueuedMission));

							const FerryDoorZones door = GetFerryDoorZones(pTransport, pNearest);
							Debug::Log("[NeuralAI] House %d AirborneFront door DIAG: zones %d and %d for movement zone %d, transport at (%d,%d) on record "
								"(%d,%d) with destination %d, soldier at (%d,%d) on record (%d,%d), plain zones %d and %d, land types %d and %d.\n",
								pHouse->ArrayIndex, door.Transport, door.Boarder, static_cast<int>(pNearest->GetTechnoType()->MovementZone),
								pTransport->GetMapCoords().X, pTransport->GetMapCoords().Y, pTransport->LastMapCoords.X, pTransport->LastMapCoords.Y,
								pTransport->Destination ? 1 : 0, pNearest->GetMapCoords().X, pNearest->GetMapCoords().Y, pNearest->LastMapCoords.X,
								pNearest->LastMapCoords.Y,
								MapClass::Instance.GetMovementZoneType(pTransport->GetMapCoords(), MovementZone::Normal, false),
								MapClass::Instance.GetMovementZoneType(pNearest->GetMapCoords(), MovementZone::Normal, false),
								pTransportCell ? static_cast<int>(pTransportCell->LandType) : -1,
								pNearest->GetCell() ? static_cast<int>(pNearest->GetCell()->LandType) : -1);
						}

						macro.RaidTransportBanUntil[pTransport->UniqueID] = Unsorted::CurrentFrame + 9000;
						logEnd("nobody boarded");
						finishPlan(NeuralPlanId::AirborneFront, false, true);
					}
				}
				else if (plan.AuxBaseline == 1)
				{
					if (pTransport->Passengers.NumPassengers == 0)
					{
						plan.AuxBaseline = 2;
						plan.Arrived = true;
						plan.ArrivedFrame = Unsorted::CurrentFrame;
						Debug::Log("[NeuralAI] House %d AirborneFront: team dropped at (%d,%d) by %s, frame %d.\n", pHouse->ArrayIndex,
							plan.StagingCell.X, plan.StagingCell.Y, pTransport->GetTechnoType()->ID, Unsorted::CurrentFrame);

						if (!underground)
							GetSafeReturnState(pHouse).Flights.push_back(pTransport->UniqueID);
					}
					else if (pTransport->GetMapCoords().DistanceFrom(pDrop->MapCoords) <= 3.0)
					{
						if (pTransport->CurrentMission != Mission::Unload)
						{
							pTransport->SetDestination(pDrop, true);
							pTransport->QueueMission(Mission::Unload, true);
						}
					}
					else if (pTransport->GetHealthPercentage() < 0.35)
					{
						logEnd("transport badly hit");
						finishPlan(NeuralPlanId::AirborneFront, false, true);
					}
					else
					{
						// Flyers take the safe air lane; a burrowing transport goes straight under.
						IssueMoveOrder(pTransport, underground ? pDrop : GetSafeAirHop(pHouse, pTransport, pDrop), 2.0);
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
							IssueMoveOrder(pTransport, underground ? pHome : GetSafeAirHop(pHouse, pTransport, pHome), 4.0);
					}

					// Captured (engineers) or the whole team gone.
					if (pTarget->Owner && pHouse->IsAlliedWith(pTarget->Owner))
					{
						Debug::Log("[NeuralAI] House %d AirborneFront: %s at (%d,%d) captured, frame %d.\n", pHouse->ArrayIndex,
							pTarget->Type->ID, plan.TargetCell.X, plan.TargetCell.Y, Unsorted::CurrentFrame);
						finishPlan(NeuralPlanId::AirborneFront, true, false);
					}
					else if (alive == 0)
					{
						logEnd("team gone");
						finishPlan(NeuralPlanId::AirborneFront, false, false);
					}
				}
			}
			else if (planReady(NeuralPlanId::AirborneFront))
			{
				// The second front only makes sense while a main attack keeps the enemy busy.
				bool mainAttack = planOf(NeuralPlanId::TimingRush).Active || planOf(NeuralPlanId::Pincer).Active
					|| planOf(NeuralPlanId::BaseTrade).Active || planOf(NeuralPlanId::AllyJointAttack).Active
					|| planOf(NeuralPlanId::ThreeFrontAssault).Active || planOf(NeuralPlanId::FactoryDecapitation).Active;

				if (!mainAttack && knownEnemyHomeBuildings > 0)
				{
					int engaged = 0;

					for (auto const& [uid, pFoot] : ownFoots)
					{
						if (isArmedGroundFoot(pFoot) && (pFoot->Target != nullptr || pFoot->MegaMission == Mission::AttackMove)
							&& pFoot->GetMapCoords().DistanceFrom(knownEnemyCentroid) <= 18.0)
						{
							engaged++;
						}
					}

					mainAttack = engaged >= 6;
				}

				FootClass* pTransport = nullptr;

				for (auto const& [uid, pFoot] : ownFoots)
				{
					if (macro.PlanMembers.contains(uid) || pFoot->Passengers.NumPassengers != 0 || pFoot->GetHealthPercentage() < 0.6
						|| (pTransport && pTransport->UniqueID < uid))
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
						// Flying transport vehicles (HIND, transport disk), amphibious APCs and hovercraft
						// (around by sea, on the same safe lane), or burrowing ones (they travel unseen under the
						// defenses, so a commando team needs no lane).
						const bool flying = pUnit->Type && IsAmphibiousOrAirTransport(pUnit->Type)
							&& (pUnit->Type->JumpJet || pUnit->Type->ConsideredAircraft || pUnit->Type->MovementZone == MovementZone::Fly);
						const bool underground = IsUndergroundTransportType(pUnit->Type);

						// An empty transport waiting on a pickup shore keeps a ferry record that has not left
						// (Departed=no) for as long as some ferry demand exists, even on a land map; only a
						// trip under way holds it. The ferry loop skips plan members.
						// One that holds a radio link is already taking somebody in (an MCV on its way to an
						// island, a unit the ferry loader sent): it has that single link for nobody else.
						auto const pFerryTrip = FindFerryTrip(uid);
						isCandidate = pUnit->Type && (flying || underground || IsAmphibiousGroundTransportType(pUnit->Type))
							&& pUnit->Type->Passengers >= (underground ? 1 : 3) && isFreeForPlan(pUnit)
							&& !(pFerryTrip && pFerryTrip->Departed) && !pUnit->GetNthLink(0);
					}

					if (isCandidate)
						pTransport = pFoot;
				}

				// Raiders (C4 commandos and other elite soldiers) or specialists (engineers, spies)
				// dropped behind the lines need no main attack to hide behind, but then only
				// go for a building with no known defense around it. A specialist rides along only to
				// a building it can work (capture, infiltrate); vehicle thieves only fill seats
				// and hunt vehicles once down.
				const int transportZone = pTransport ? ObjectZone(pTransport, MovementZone::Normal) : -1;
				bool freeRaider = false;
				std::vector<FootClass*> specialists;

				// Volley damage of one soldier (primary weapon, burst included, against the building's
				// armor when one is given): a squad of 4 or more worth 200 a volley (shock troopers,
				// flamethrowers) wrecks a building before anyone answers.
				auto volleyOf = [](FootClass* pFoot, BuildingClass* pBuilding)
				{
					auto const pType = pFoot->GetTechnoType();
					auto const pSlot = pType ? pType->GetWeapon(0) : nullptr;
					auto const pWeapon = pSlot ? pSlot->WeaponType : nullptr;

					if (!pWeapon || !pWeapon->Warhead)
						return 0;

					const double verses = pBuilding ? GeneralUtils::GetWarheadVersusArmor(pWeapon->Warhead, pBuilding, pBuilding->Type) : 1.0;

					return static_cast<int>(std::max(0, pWeapon->Damage) * std::max(1, pWeapon->Burst) * verses);
				};

				int freeSquad = 0;
				int freeSquadVolley = 0;

				for (auto const& [uid, pFoot] : ownFoots)
				{
					auto const pInf = abstract_cast<InfantryClass*>(pFoot);

					if (!pTransport || ObjectZone(pFoot, MovementZone::Normal) != transportZone)
						continue;

					// A demolition vehicle that fits the transport is a raider too.
					if (auto const pUnit = abstract_cast<UnitClass*>(pFoot); pUnit && pFoot != pTransport && IsVehicleRaiderType(pUnit->Type)
						&& pUnit->GetTechnoType()->Size <= pTransport->GetTechnoType()->SizeLimit && isFreeForPlan(pFoot))
					{
						freeRaider = true;
					}

					if (!pInf)
						continue;

					if (isArmedGroundFoot(pFoot) && isFreeForPlan(pFoot) && !IsSpecialistInfantryType(pInf->Type))
					{
						freeRaider |= IsRaiderInfantryType(pInf->Type);
						freeSquad++;
						freeSquadVolley += volleyOf(pFoot, nullptr);
					}
					else if (IsSpecialistInfantryType(pInf->Type) && !macro.PlanMembers.contains(uid)
						&& !(pInf->Type->Engineer && IsSpecOpsEngineerBusy(pHouse, uid)) && pFoot->GetHealthPercentage() >= 0.6
						&& (pFoot->CurrentMission == Mission::Guard || pFoot->CurrentMission == Mission::Area_Guard))
					{
						specialists.push_back(pFoot);
					}
				}

				std::sort(specialists.begin(), specialists.end(), [](FootClass* a, FootClass* b) { return a->UniqueID < b->UniqueID; });

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

				const size_t seats = pTransport ? static_cast<size_t>(std::max(0, pTransport->GetTechnoType()->Passengers)) : 0;
				const bool freeStrikeSquad = seats >= 4 && freeSquad >= 4 && freeSquadVolley >= 200;

				BuildingClass* pTarget = nullptr;
				CellClass* pDrop = nullptr;
				int bestScore = INT_MIN;

				if ((mainAttack || freeRaider || freeStrikeSquad || !specialists.empty()) && pTransport)
				{
					const int defensesAllowed = mainAttack ? 1 : 0;

					for (auto const pBld : knownEnemyBuildingList)
					{
						if (!isKnownEnemyBuilding(pBld))
							continue;

						const int workers = specialistsFor(pBld->Type);

						// Without a main attack only raiders, a strike squad or a specialist job justify the trip.
						if (!mainAttack && !freeRaider && !freeStrikeSquad && workers == 0)
							continue;

						const int worth = valuableBuildingWorth(pBld->Type) + workers * 150;
						const CellStruct bc = pBld->GetMapCoords();

						if (worth <= 0 || CountKnownEnemyDefensesNear(pHouse, bc, 9.0, true) > defensesAllowed
							|| CountVisibleEnemyMobileAANear(pHouse, bc, 9.0) > defensesAllowed)
						{
							continue;
						}

						// Deep targets away from where the main army fights.
						const int score = worth + static_cast<int>(bc.DistanceFrom(knownEnemyCentroid == CellStruct::Empty ? homeCell : knownEnemyCentroid)) * 3
							- CountKnownEnemyDefensesNear(pHouse, bc, 9.0, false) * 60;

						if (score > bestScore)
						{
							if (auto const pCell = landCellInZone(pointAlong(bc, homeCell, 4.0, 0.0), zoneOf(bc), 5))
							{
								bestScore = score;
								pTarget = pBld;
								pDrop = pCell;
							}
						}
					}
				}

				// Boarding order: raiders (soldiers, then demolition vehicles that fit), the specialists
				// with a job there, the other soldiers by UniqueID, then vehicle thieves; seats are
				// counted by passenger Size, each passenger within the transport's SizeLimit.
				std::vector<FootClass*> infantry;
				int raiders = 0;
				int workersAboard = 0;

				if (pTarget)
				{
					auto isRaider = [](FootClass* pFoot)
					{
						if (auto const pInf = abstract_cast<InfantryClass*>(pFoot))
							return IsRaiderInfantryType(pInf->Type);

						auto const pUnit = abstract_cast<UnitClass*>(pFoot);
						return pUnit && IsVehicleRaiderType(pUnit->Type);
					};

					auto const pTransportType = pTransport->GetTechnoType();
					std::vector<FootClass*> soldiers;
					std::vector<FootClass*> vehicles;

					for (auto const& [uid, pFoot] : ownFoots)
					{
						if (!isFreeForPlan(pFoot) || ObjectZone(pFoot, MovementZone::Normal) != transportZone)
							continue;

						if (abstract_cast<InfantryClass*>(pFoot) && isArmedGroundFoot(pFoot))
							soldiers.push_back(pFoot);
						else if (auto const pUnit = abstract_cast<UnitClass*>(pFoot); pUnit && pFoot != pTransport
							&& IsVehicleRaiderType(pUnit->Type) && pUnit->GetTechnoType()->Size <= pTransportType->SizeLimit)
							vehicles.push_back(pFoot);
					}

					std::sort(soldiers.begin(), soldiers.end(), [&](FootClass* a, FootClass* b)
					{
						const bool ra = isRaider(a);
						const bool rb = isRaider(b);
						return ra != rb ? ra : a->UniqueID < b->UniqueID;
					});

					std::sort(vehicles.begin(), vehicles.end(), [](FootClass* a, FootClass* b) { return a->UniqueID < b->UniqueID; });

					std::vector<FootClass*> order;
					auto const firstOther = std::find_if(soldiers.begin(), soldiers.end(), [&](FootClass* pFoot) { return !isRaider(pFoot); });
					order.assign(soldiers.begin(), firstOther);
					order.insert(order.end(), vehicles.begin(), vehicles.end());

					for (auto const pFoot : specialists)
					{
						if (SpecialistWorksBuilding(static_cast<InfantryClass*>(pFoot)->Type, pTarget->Type))
							order.push_back(pFoot);
					}

					order.insert(order.end(), firstOther, soldiers.end());

					for (auto const pFoot : specialists)
					{
						if (static_cast<InfantryClass*>(pFoot)->Type->VehicleThief)
							order.push_back(pFoot);
					}

					const int capacity = std::max(0, pTransportType->Passengers);
					int usedSeats = 0;

					for (auto const pFoot : order)
					{
						const int size = std::max(1, static_cast<int>(pFoot->GetTechnoType()->Size));

						if (usedSeats + size > capacity)
							continue;

						usedSeats += size;
						infantry.push_back(pFoot);

						auto const pInf = abstract_cast<InfantryClass*>(pFoot);

						if (isRaider(pFoot))
							raiders++;
						else if (pInf && SpecialistWorksBuilding(pInf->Type, pTarget->Type))
							workersAboard++;
					}
				}

				// A raid without a main attack must carry a raider, a specialist with a job there (one is
				// a team by itself) or a strike squad of 4+ soldiers worth 200 a volley.
				int teamVolley = 0;

				for (auto const pFoot : infantry)
					teamVolley += volleyOf(pFoot, pTarget);

				const bool strikeSquad = infantry.size() >= 4 && teamVolley >= 200;

				// A transport that goes by land or sea drives its team through whatever stands on its
				// way, so the team and it must be worth what is known there, as for any raid (48 of 182
				// plans lost their transport or had it badly hit on the way, nearly all of them
				// hovercraft and carriers on the surface). A flyer takes the safe air lane and a
				// burrower goes under.
				bool wayOpen = true;

				if (pTarget && pTransport)
				{
					auto const pCarrierType = pTransport->GetTechnoType();
					auto const pCarrierUnit = abstract_cast<UnitClass*>(pTransport);
					const bool surfaceTrip = !pCarrierType->JumpJet && pCarrierType->MovementZone != MovementZone::Fly
						&& pTransport->WhatAmI() != AbstractType::Aircraft && !(pCarrierUnit && IsUndergroundTransportType(pCarrierUnit->Type));

					if (surfaceTrip)
					{
						int teamValue = GetCombatValueOfType(pCarrierType, false);

						for (auto const pFoot : infantry)
							teamValue += GetCombatValueOfType(pFoot->GetTechnoType(), false);

						wayOpen = teamValue * 100 >= knownEnemyStrengthOnWay(pTransport->GetMapCoords(), pTarget->GetMapCoords(),
							PlanStrikeGuardFrames) * PlanStrikeAdvantagePercent;
					}
				}

				const bool teamReady = wayOpen
					&& (raiders > 0 || workersAboard > 0 || strikeSquad || (mainAttack && infantry.size() >= 3));
				const int utility = (pTarget && teamReady) ? (320 + bestScore / 6 + raiders * 150 + workersAboard * 120) : -1000;

				if (utility > 0 && shouldTryStart(NeuralPlanId::AirborneFront, utility))
				{
					std::vector<FootClass*> group { pTransport };
					group.insert(group.end(), infantry.begin(), infantry.end());
					startPlan(NeuralPlanId::AirborneFront, group, pTarget->GetMapCoords(), pDrop->MapCoords, pTarget->UniqueID, 4500);
					ClearFerryTrip(pTransport->UniqueID);
					planOf(NeuralPlanId::AirborneFront).AuxBaseline = 0;
					Debug::Log("[NeuralAI] House %d AirborneFront: %s %u carries %d soldiers (%d raiders, %d specialists) to %s of house %d at (%d,%d)%s, frame %d.\n",
						pHouse->ArrayIndex, pTransport->GetTechnoType()->ID, pTransport->UniqueID, static_cast<int>(infantry.size()),
						raiders, workersAboard, pTarget->Type->ID, pTarget->Owner ? pTarget->Owner->ArrayIndex : -1, pDrop->MapCoords.X,
						pDrop->MapCoords.Y, mainAttack ? "" : " (stealth raid)", Unsorted::CurrentFrame);
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
					plan.TargetCell = pTarget->GetMapCoords();
					attackTargetGroup(plan.Members, pTarget);
				}
				else
				{
					// Out of sight: head for where it was last seen.
					attackMoveGroup(plan.Members, MapClass::Instance.TryGetCellAt(plan.TargetCell));
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
		// A clearly stronger enemy force approaches: our ground army falls back under our base
		// defenses (AuxBaseline 0), lets the enemy bleed there, then counterattacks the survivors
		// (AuxBaseline 1) once the seen enemy value dropped to 60% of its peak or below ours.
		{
			auto& plan = planOf(NeuralPlanId::HoldUnderTowers);

			if (plan.Active)
			{
				auto const pRally = MapClass::Instance.TryGetCellAt(plan.StagingCell);
				// Same radius as the start check, so a force 27..30 cells out that started the plan is
				// not read as one that never came.
				const int enemyNear = seenEnemyValueNear(homeCell, 30.0, 150);
				const int ourValue = memberValue(plan);
				const int strength = groupStrength(plan.Members);
				plan.AuxPeak = std::max(plan.AuxPeak, enemyNear);

				// The enemy is still at our towers when the time is up and the group holds: hold on
				// (once, 1800 frames) instead of releasing the army in the middle of the defense.
				if (pRally && Unsorted::CurrentFrame >= plan.Deadline && plan.Progress == 0 && enemyNear > 0
					&& strength * 100 >= plan.InitialStrength * 40)
				{
					plan.Progress = 1;
					plan.Deadline += 1800;
				}

				if (!pRally || Unsorted::CurrentFrame >= plan.Deadline)
				{
					finishPlan(NeuralPlanId::HoldUnderTowers, ourValue * 100 >= plan.InitialValue * 30 && enemyNear == 0, false);
				}
				else if (plan.AuxBaseline == 0)
				{
					moveGroup(plan.Members, pRally, 5.0);

					if (enemyNear == 0 && Unsorted::CurrentFrame - plan.StartFrame >= 900)
						finishPlan(NeuralPlanId::HoldUnderTowers, false, false); // They never came.
					else if (plan.AuxPeak > 0 && (enemyNear * 10 <= plan.AuxPeak * 6 || strength * 10 >= enemyNear * 12))
						plan.AuxBaseline = 1;
				}
				else
				{
					const CellStruct enemyCenter = seenEnemyCentroidNear(homeCell, 30.0, 150);

					// The counterattack meets more than it can take: back under the towers.
					if (enemyCenter != CellStruct::Empty && seenEnemyValueNear(enemyCenter, 10.0, 150) * 10 > strength * 12)
					{
						plan.AuxBaseline = 0;
						plan.AuxPeak = enemyNear;
						moveGroup(plan.Members, pRally, 5.0);
					}
					else if (enemyCenter == CellStruct::Empty)
					{
						finishPlan(NeuralPlanId::HoldUnderTowers, true, false);
					}
					else
						attackMoveGroup(plan.Members, MapClass::Instance.TryGetCellAt(enemyCenter));
				}
			}
			else if (planReady(NeuralPlanId::HoldUnderTowers) && ownHomeDefenses >= 2)
			{
				const int enemyNear = seenEnemyValueNear(homeCell, 30.0, 150);
				std::vector<FootClass*> army;

				for (auto const& [uid, pFoot] : ownFoots)
				{
					if (isArmedGroundFoot(pFoot) && !macro.PlanMembers.contains(uid) && ObjectZone(pFoot, MovementZone::Normal) == homeZone
						&& pFoot->GetMapCoords().DistanceFrom(homeCell) <= 40.0)
					{
						army.push_back(pFoot);
					}
				}

				std::sort(army.begin(), army.end(), [](FootClass* a, FootClass* b) { return a->UniqueID < b->UniqueID; });
				const int ourValue = feetValue(army);

				// Hold just behind the hottest approach our towers cover (route heat), else at the
				// middle of our home defenses.
				CellClass* pRally = nullptr;

				if (auto const pHoldSpot = BestRouteHoldSpot(pHouse, homeCell, 26))
					pRally = landCellInZone(pointAlong(pHoldSpot->Cell, homeCell, 4.0, 0.0), homeZone, 4);

				const bool rallyOnRoute = pRally != nullptr;

				if (!pRally)
					pRally = landCellInZone(ownDefenseCentroid, homeZone, 4);

				const int utility = (pRally && !army.empty() && enemyNear >= 120 && enemyNear * 10 > ourValue * 13) ? 380 : -1000;

				if (utility > 0 && shouldTryStart(NeuralPlanId::HoldUnderTowers, utility))
				{
					startPlan(NeuralPlanId::HoldUnderTowers, army, homeCell, pRally->MapCoords, 0, 3600);
					planOf(NeuralPlanId::HoldUnderTowers).AuxBaseline = 0;

					if (rallyOnRoute)
						CountDecision(NeuralDecision::RouteAmbushSpot, pHouse->ArrayIndex);
				}
			}
		}

		// ---------------------------------------------------------------- Plan 13: Superweapon Race
		// An enemy superweapon with a public countdown (ShowTimer=yes, shown to every player)
		// is about to fire and we know its building: strike it before the timer runs out, by
		// air when the target is lightly covered, else with a ground group.
		{
			auto& plan = planOf(NeuralPlanId::SuperweaponRace);

			if (plan.Active)
			{
				auto const pTarget = FindBuildingByID(plan.TargetID);

				if (!pTarget)
				{
					finishPlan(NeuralPlanId::SuperweaponRace, true, true);
				}
				else if (memberValue(plan) * 100 < plan.InitialValue * 40 || Unsorted::CurrentFrame >= plan.Deadline)
				{
					finishPlan(NeuralPlanId::SuperweaponRace, false, true);
				}
				else
				{
					for (const DWORD uid : plan.Members)
					{
						auto const pFoot = findOwnFoot(uid);

						if (!pFoot)
							continue;

						if (auto const pAir = abstract_cast<AircraftClass*>(pFoot))
						{
							// Re-arm and strike again while the target stands.
							if (ReadyPlanAircraft(pAir))
							{
								pAir->SetTarget(pTarget);
								pAir->QueueMission(Mission::Attack, true);
							}
						}
						else if (pFoot->GetMapCoords().DistanceFrom(pTarget->GetMapCoords()) <= 9.0)
						{
							if (pFoot->Target != pTarget && pFoot->Target == nullptr)
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
				}
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

				std::vector<FootClass*> group;

				if (pTarget)
				{
					const CellStruct tc = pTarget->GetMapCoords();

					if (CountKnownEnemyDefensesNear(pHouse, tc, 10.0, true) + CountVisibleEnemyMobileAANear(pHouse, tc, 10.0) <= 2)
					{
						for (auto const& [uid, pFoot] : ownFoots)
						{
							auto const pAir = abstract_cast<AircraftClass*>(pFoot);

							if (isFreeAirStriker(pAir))
							{
								group.push_back(pAir);
							}
						}

						std::sort(group.begin(), group.end(), [](FootClass* a, FootClass* b) { return a->UniqueID < b->UniqueID; });

						if (group.size() < 2 || !targetLessonAllows(pTarget->UniqueID, groupValueOf(group)))
							group.clear();
					}

					if (group.empty() && zoneOf(tc) == homeZone)
					{
						group = collectIdleGround(false);
						group.resize(std::min(group.size(), std::max<size_t>(12, group.size() / PlanStrikeIdleShare)));

						// The superweapon stands deep in the base: the ground group goes only with the
						// strength to beat what is known to guard it.
						if (group.size() < 4 || !targetLessonAllows(pTarget->UniqueID, groupValueOf(group)) || groupValueOf(group) * 100
							< knownEnemyStrengthOnWay(homeCell, tc, PlanStrikeGuardFrames) * PlanStrikeAdvantagePercent)
						{
							group.clear();
						}
					}
				}

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

					for (auto const pFoot : group)
					{
						if (pFoot->WhatAmI() == AbstractType::Aircraft)
						{
							pFoot->SetTarget(pTarget);
							pFoot->QueueMission(Mission::Attack, true);
						}
					}
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
		// An enemy force is seen gathering outside its defenses (and not yet at our base): hit it
		// with a stronger idle group before it is complete. AuxBaseline = its value at the start.
		{
			auto& plan = planOf(NeuralPlanId::PreemptiveStrike);

			if (plan.Active)
			{
				const CellStruct current = seenEnemyCentroidNear(plan.TargetCell, 10.0, 150);

				// The plan follows the force PreemptiveLeashCells from where it gathered and no
				// farther: a force that has set out is no longer a gathering, and the group that
				// followed it ended across the map (37 of 102 plans ran out of time 27 to 107 cells
				// from a target that kept moving, 50 more were beaten on the way, 2026-11-14).
				const bool forceLeft = current != CellStruct::Empty && plan.AnchorCell != CellStruct::Empty
					&& current.DistanceFrom(plan.AnchorCell) > PreemptiveLeashCells;

				if (current != CellStruct::Empty && !forceLeft)
					plan.TargetCell = current;

				// Arrived with half of the group at the force: the centre of a group strung out
				// along its way stays behind while its head is already fighting.
				int membersAlive = 0;
				int membersThere = 0;

				for (const DWORD uid : plan.Members)
				{
					if (auto const pMember = findOwnFoot(uid))
					{
						membersAlive++;

						if (pMember->GetMapCoords().DistanceFrom(plan.TargetCell) <= 8.0)
							membersThere++;
					}
				}

				if (!plan.Arrived && membersThere > 0 && membersThere * 2 >= membersAlive)
				{
					plan.Arrived = true;
					plan.ArrivedFrame = Unsorted::CurrentFrame;
				}

				if (memberValue(plan) * 100 < plan.InitialValue * 40 || Unsorted::CurrentFrame >= plan.Deadline)
					finishPlan(NeuralPlanId::PreemptiveStrike, false, true);
				else if (forceLeft && !plan.Arrived)
					finishPlan(NeuralPlanId::PreemptiveStrike, false, true); // It left before the group got there.
				else if (plan.Arrived && seenEnemyValueNear(plan.TargetCell, 10.0, 150) * 10 <= plan.AuxBaseline * 4)
					finishPlan(NeuralPlanId::PreemptiveStrike, true, true);
				else
					attackMoveGroup(plan.Members, MapClass::Instance.TryGetCellAt(plan.TargetCell));
			}
			else if (planReady(NeuralPlanId::PreemptiveStrike))
			{
				CellStruct bestCenter = CellStruct::Empty;
				int bestValue = 0;

				for (auto const& [uid, seed] : macro.EnemySightings)
				{
					if (Unsorted::CurrentFrame - seed.LastSeenFrame > 150 || seed.NearOurBase || seed.Value < 20)
						continue;

					const int value = seenEnemyValueNear(seed.LastCoords, 8.0, 150);

					// Ties broken by position so the pick does not depend on the map's iteration order.
					if (value > bestValue
						|| (value == bestValue && value > 0
							&& (seed.LastCoords.X < bestCenter.X || (seed.LastCoords.X == bestCenter.X && seed.LastCoords.Y < bestCenter.Y))))
					{
						bestValue = value;
						bestCenter = seed.LastCoords;
					}
				}

				CellStruct clusterCenter = CellStruct::Empty;
				std::vector<FootClass*> group;

				if (bestValue >= 140)
				{
					clusterCenter = seenEnemyCentroidNear(bestCenter, 8.0, 150);

					if (clusterCenter != CellStruct::Empty && zoneOf(clusterCenter) == homeZone
						&& CountKnownEnemyDefensesNear(pHouse, clusterCenter, 10.0, false) == 0
						&& clusterCenter.DistanceFrom(homeCell) > 22.0)
					{
						group = collectIdleGround(false);
						group.resize(std::min<size_t>(group.size(), 12));

						// Only a force the group reaches while it is still gathering.
						int sumX = 0;
						int sumY = 0;

						for (auto const pFoot : group)
						{
							const CellStruct cell = pFoot->GetMapCoords();
							sumX += cell.X;
							sumY += cell.Y;
						}

						const int count = static_cast<int>(group.size());
						const bool inReach = count > 0 && CellStruct { static_cast<short>(sumX / count), static_cast<short>(sumY / count) }
							.DistanceFrom(clusterCenter) <= PreemptiveReachCells;

						if (group.size() < 5 || !inReach || feetValue(group) * 100 < bestValue * 125)
							group.clear();
					}
				}

				const int utility = !group.empty() ? 320 : -1000;

				if (utility > 0 && shouldTryStart(NeuralPlanId::PreemptiveStrike, utility))
				{
					startPlan(NeuralPlanId::PreemptiveStrike, group, clusterCenter, CellStruct::Empty, 0, 2400);
					planOf(NeuralPlanId::PreemptiveStrike).AuxBaseline = bestValue;
					planOf(NeuralPlanId::PreemptiveStrike).AnchorCell = clusterCenter;
				}
			}
		}

		// ---------------------------------------------------------------- Plan 20: Bait Ambush
		// The fastest unit (Members[0]) provokes the enemy near its base and runs back through an
		// ambush point where the rest of the group waits on guard. AuxBaseline = phase (0 lure,
		// 1 run back, 2 sprung), AuxPeak = peak enemy value seen at the ambush point.
		{
			auto& plan = planOf(NeuralPlanId::BaitAmbush);

			if (plan.Active)
			{
				FootClass* const pBait = plan.Members.empty() ? nullptr : findOwnFoot(plan.Members[0]);
				const std::vector<DWORD> ambushers(plan.Members.empty() ? plan.Members.end() : plan.Members.begin() + 1, plan.Members.end());
				auto const pAmbush = MapClass::Instance.TryGetCellAt(plan.StagingCell);
				const int enemyAtAmbush = seenEnemyValueNear(plan.StagingCell, 9.0, 150);
				const int ambushValue = groupValue(ambushers);
				plan.AuxPeak = std::max(plan.AuxPeak, enemyAtAmbush);

				if (!pAmbush || ambushValue * 100 < plan.InitialValue * 35)
				{
					finishPlan(NeuralPlanId::BaitAmbush, false, true);
				}
				else if (Unsorted::CurrentFrame >= plan.Deadline)
				{
					finishPlan(NeuralPlanId::BaitAmbush, plan.AuxPeak >= 60 && ambushValue * 100 >= plan.InitialValue * 50, true);
				}
				else if (plan.AuxBaseline == 0)
				{
					moveGroup(ambushers, pAmbush, 3.0);

					if (!pBait)
					{
						plan.AuxBaseline = 1;
						plan.ArrivedFrame = Unsorted::CurrentFrame;
					}
					else if (seenEnemyValueNear(pBait->GetMapCoords(), 8.0, 60) >= 40 || pBait->GetHealthPercentage() < 0.7)
					{
						plan.AuxBaseline = 1;
						plan.ArrivedFrame = Unsorted::CurrentFrame;
					}
					else if (auto const pLure = landCellInZone(pointAlong(plan.TargetCell, homeCell, 8.0, 0.0), homeZone, 4))
					{
						IssueMoveOrder(pBait, pLure, 2.0);

						// Shown at the enemy's door for BaitLureWaitFrames and nobody came out: the
						// ambushers go back to work. The bait stood there until the plan's deadline
						// while eight units waited for it (32 of 125 plans, 2026-11-14).
						if (!plan.Arrived && pBait->GetMapCoords().DistanceFrom(pLure->MapCoords) <= 4.0)
						{
							plan.Arrived = true;
							plan.ArrivedFrame = Unsorted::CurrentFrame;
						}
						else if (plan.Arrived && Unsorted::CurrentFrame - plan.ArrivedFrame >= BaitLureWaitFrames)
						{
							finishPlan(NeuralPlanId::BaitAmbush, false, true);
							macro.PlanNextAttemptFrame[static_cast<size_t>(NeuralPlanId::BaitAmbush)] = Unsorted::CurrentFrame + BaitRetryFrames;
						}
					}
				}
				else if (plan.AuxBaseline == 1)
				{
					moveGroup(ambushers, pAmbush, 3.0);
					bool baitBack = true;

					if (pBait)
					{
						if (auto const pBehind = landCellInZone(pointAlong(plan.StagingCell, homeCell, 4.0, 0.0), homeZone, 3))
						{
							IssueMoveOrder(pBait, pBehind, 2.0);
							baitBack = pBait->GetMapCoords().DistanceFrom(pBehind->MapCoords) <= 5.0;
						}
					}

					// Whoever chases the bait is on its heels: the wait for them starts when the bait
					// is back behind the ambush, and lasts BaitChaseWaitFrames (it was 1500 frames
					// from the moment it turned, and 70 of 125 plans spent them on nobody).
					if (!baitBack)
						plan.ArrivedFrame = Unsorted::CurrentFrame;

					if (enemyAtAmbush >= 40)
						plan.AuxBaseline = 2;
					else if (Unsorted::CurrentFrame - plan.ArrivedFrame >= BaitChaseWaitFrames)
					{
						finishPlan(NeuralPlanId::BaitAmbush, false, true); // Nobody took the bait.
						macro.PlanNextAttemptFrame[static_cast<size_t>(NeuralPlanId::BaitAmbush)] = Unsorted::CurrentFrame + BaitRetryFrames;
					}
				}
				else
				{
					const CellStruct enemyCenter = seenEnemyCentroidNear(plan.StagingCell, 12.0, 150);

					if (enemyCenter == CellStruct::Empty)
						finishPlan(NeuralPlanId::BaitAmbush, plan.AuxPeak >= 60, true);
					else
						attackMoveGroup(plan.Members, MapClass::Instance.TryGetCellAt(enemyCenter));
				}
			}
			else if (planReady(NeuralPlanId::BaitAmbush) && knownEnemyHomeBuildings > 0)
			{
				std::vector<FootClass*> idle = collectIdleGround(false);
				BuildingClass* pLureTarget = nullptr;
				double bestDist = 1e9;

				for (auto const pBld : knownEnemyBuildingList)
				{
					if (!isKnownEnemyBuilding(pBld) || ObjectZone(pBld, MovementZone::Normal) != homeZone)
						continue;

					const double d = pBld->GetMapCoords().DistanceFrom(homeCell);

					if (d < bestDist)
					{
						bestDist = d;
						pLureTarget = pBld;
					}
				}

				CellClass* pAmbush = nullptr;

				if (pLureTarget && idle.size() >= 7 && bestDist >= 35.0)
				{
					pAmbush = landCellInZone(pointAlong(homeCell, pLureTarget->GetMapCoords(), bestDist * 0.35, 0.0), homeZone, 4);

					if (pAmbush)
					{
						for (auto const pBld : knownEnemyBuildingList)
						{
							if (isKnownEnemyBuilding(pBld) && pBld->GetMapCoords().DistanceFrom(pAmbush->MapCoords) < 15.0)
							{
								pAmbush = nullptr;
								break;
							}
						}
					}
				}

				const int utility = pAmbush ? 270 : -1000;

				if (utility > 0 && shouldTryStart(NeuralPlanId::BaitAmbush, utility))
				{
					sortFastest(idle);
					// Never risk an elite unit as the bait (point 38).
					auto const baitIt = std::find_if(idle.begin(), idle.end(), [](FootClass* f) { return !f->Veterancy.IsElite(); });
					std::rotate(idle.begin(), baitIt != idle.end() ? baitIt : idle.begin(), (baitIt != idle.end() ? baitIt : idle.begin()) + 1);
					std::vector<FootClass*> group { idle.front() };
					std::vector<FootClass*> rest(idle.begin() + 1, idle.end());
					std::sort(rest.begin(), rest.end(), [](FootClass* a, FootClass* b) { return a->UniqueID < b->UniqueID; });
					rest.resize(std::min<size_t>(rest.size(), 8));
					group.insert(group.end(), rest.begin(), rest.end());
					startPlan(NeuralPlanId::BaitAmbush, group, pLureTarget->GetMapCoords(), pAmbush->MapCoords, pLureTarget->UniqueID, 3600);
					planOf(NeuralPlanId::BaitAmbush).AuxBaseline = 0;
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
		// forward structures (ForwardRepairCell, read by the building selector and placement), so
		// damaged units repair close to the fight instead of driving home.
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

			if (plan.Active)
			{
				if (repairStations >= 2)
				{
					clearForward();
					finishPlan(NeuralPlanId::ForwardRepair, true, false);
				}
				else if (repairStations == 0 || Unsorted::CurrentFrame >= plan.Deadline)
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
				&& pFirstRepair && knownEnemyCentroid != CellStruct::Empty && pHouse->Available_Money() >= 2000)
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
		// a known building reachable from the sea, from a water cell outside the reach of the
		// other known defenses, with the remaining warships screening them. Coastal defenses are
		// taken out first. Damaged ships (< 40%) head back to our naval yard.
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
					else if (WeaponHitsGround(pWeapon))
						gun = std::max(gun, pWeapon->Range);
				}

				return gun > 0 ? gun : (launches ? 18 * Unsorted::LeptonsPerCell : 0);
			};

			auto isLongRangeShip = [&landReachOf](FootClass* pFoot) -> bool
			{
				auto const pUnit = abstract_cast<UnitClass*>(pFoot);

				return pUnit && pUnit->Type && pUnit->Type->Naval && landReachOf(pUnit->Type) >= 10 * Unsorted::LeptonsPerCell;
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
				auto const pTarget = FindBuildingByID(plan.TargetID);
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
					// TEMP diagnostic: 48 of 93 plans ran out of time with four fifths of the fleet, the
					// ships on Guard 50 to 200 cells from the target (2026-11-14). What each ship is
					// doing every 1500 frames of its plan. AuxPeak = reports written.
					static int navalDiagLines = 0;
					const bool navalDiag = Unsorted::CurrentFrame - plan.StartFrame >= 1500 * (plan.AuxPeak + 1) && navalDiagLines < 60;

					if (navalDiag)
						plan.AuxPeak++;

					for (const DWORD uid : plan.Members)
					{
						auto const pShip = findOwnFoot(uid);

						if (!pShip)
							continue;

						if (navalDiag)
						{
							auto const pShipType = pShip->GetTechnoType();
							const CellStruct at = pShip->GetMapCoords();
							auto const pGoing = abstract_cast<CellClass*>(pShip->Destination);
							auto const pReach = NearestReachableNavalCell(pShip, plan.StagingCell);
							navalDiagLines++;
							Debug::Log("[NeuralAI] House %d navalStrike ship DIAG frame %d (plan frame %d): %s zone type %d at (%d,%d) zone %d, firing "
								"cell (%d,%d) zone %d at %d cells, mission %d, target %d (the plan's %d), destination (%d,%d), reachable cell (%d,%d), "
								"hp %d, range %d.\n", houseIdx, Unsorted::CurrentFrame, Unsorted::CurrentFrame - plan.StartFrame, pShipType->ID,
								static_cast<int>(pShipType->MovementZone), at.X, at.Y, ObjectZone(pShip, pShipType->MovementZone),
								plan.StagingCell.X, plan.StagingCell.Y,
								MapClass::Instance.GetMovementZoneType(plan.StagingCell, pShipType->MovementZone, false),
								static_cast<int>(at.DistanceFrom(plan.StagingCell)), static_cast<int>(pShip->CurrentMission),
								pShip->Target ? 1 : 0, pShip->Target == pTarget ? 1 : 0, pGoing ? pGoing->MapCoords.X : -1,
								pGoing ? pGoing->MapCoords.Y : -1, pReach ? pReach->MapCoords.X : -1, pReach ? pReach->MapCoords.Y : -1,
								static_cast<int>(pShip->GetHealthPercentage() * 100), GroundRangeOf(pShipType) / Unsorted::LeptonsPerCell);
						}

						if (pShip->GetHealthPercentage() < 0.4)
						{
							if (pNavalYard)
								IssueMoveOrder(pShip, pNavalYard, 5.0);

							continue;
						}

						const bool siege = isLongRangeShip(pShip);
						const double distToFiring = pShip->GetMapCoords().DistanceFrom(plan.StagingCell);

						if (siege && distToFiring <= 4.0)
						{
							if (pShip->Target != pTarget)
							{
								pShip->SetTarget(pTarget);
								pShip->QueueMission(Mission::Attack, true);
							}
						}
						else if (pShip->Target == nullptr)
						{
							// TEMP trace: a ship far from the firing cell, as this tick finds it and as the move
							// order leaves it (they are found on Guard with no destination at every report).
							static int navalOrderLines = 0;
							const bool traceOrder = distToFiring >= 25.0 && navalOrderLines < 120 && pShip->UniqueID % 3 == 0;
							const int missionBefore = static_cast<int>(pShip->CurrentMission);
							const int ageBefore = Unsorted::CurrentFrame - pShip->CurrentMissionStartTime;
							auto const pGoingBefore = abstract_cast<CellClass*>(pShip->Destination);
							const CellStruct goingBefore = pGoingBefore ? pGoingBefore->MapCoords : CellStruct::Empty;

							// Escorts screen around the firing position (they engage on their own).
							const bool issued = IssueMoveOrder(pShip, pFiring, siege ? 2.0 : 4.0);

							if (distToFiring >= 25.0)
								WatchMove(pShip);

							if (traceOrder)
							{
								navalOrderLines++;
								auto const pGoingAfter = abstract_cast<CellClass*>(pShip->Destination);
								Debug::Log("[NeuralAI] House %d navalStrike order DIAG frame %d: %s %u at (%d,%d) %d cells from the firing cell, found on mission "
									"%d %d frames old with destination (%d,%d), moving %d; order issued %d, now mission %d queued %d destination (%d,%d), "
									"submerged %d, held still %d.\n", houseIdx, Unsorted::CurrentFrame, pShip->GetTechnoType()->ID, pShip->UniqueID,
									pShip->GetMapCoords().X, pShip->GetMapCoords().Y, static_cast<int>(distToFiring), missionBefore, ageBefore, goingBefore.X,
									goingBefore.Y, (pShip->Locomotor && pShip->Locomotor->Is_Moving()) ? 1 : 0, issued ? 1 : 0,
									static_cast<int>(pShip->CurrentMission), static_cast<int>(pShip->QueuedMission),
									pGoingAfter ? pGoingAfter->MapCoords.X : -1, pGoingAfter ? pGoingAfter->MapCoords.Y : -1,
									pShip->CloakState != CloakState::Uncloaked ? 1 : 0, IsFootHeldStill(pHouse, pShip, 300) ? 1 : 0);
							}
						}
					}
				}
			}
			else if (planReady(NeuralPlanId::NavalStrikeGroup))
			{
				std::vector<FootClass*> siegeShips;
				std::vector<FootClass*> escorts;
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
						// The firing cell is within the shortest reach of the shelling ships, so all of them fire.
						const int reach = landReachOf(pUnit->Type);
						fleetRange = fleetRange == 0 ? reach : std::min(fleetRange, reach);

						// Minimum range of its ground weapons (e.g. a missile sub's launcher): the firing
						// spot keeps outside the largest one of the fleet.
						for (int i = 0; i < 2; ++i)
						{
							auto const pWS = pUnit->Type->GetWeapon(i);

							if (pWS && pWS->WeaponType && WeaponHitsGround(pWS->WeaponType))
								fleetMinRange = std::max(fleetMinRange, pWS->WeaponType->MinimumRange);
						}
					}
					else if (GetCombatValueOfType(pUnit->Type, false) > 0)
					{
						escorts.push_back(pUnit);
					}
				}

				auto byID = [](FootClass* a, FootClass* b) { return a->UniqueID < b->UniqueID; };
				std::sort(siegeShips.begin(), siegeShips.end(), byID);
				std::sort(escorts.begin(), escorts.end(), byID);

				if (!siegeShips.empty())
					fleetZone = ObjectZone(siegeShips.front(), MovementZone::Water);

				BuildingClass* pTarget = nullptr;
				CellStruct firing = CellStruct::Empty;
				int bestScore = INT_MIN;
				const int rangeCells = std::clamp(fleetRange / Unsorted::LeptonsPerCell, 6, 26);

				// Enemy ships in our sight now: the firing spot stays out of their reach too.
				std::vector<std::pair<CellStruct, int>> enemyShips;

				for (auto const pEnemy : UnitClass::Array)
				{
					if (pEnemy && pEnemy->IsAlive && !pEnemy->InLimbo && pEnemy->Type && pEnemy->Type->Naval && pEnemy->Owner
						&& pEnemy->Owner != pHouse && !pHouse->IsAlliedWith(pEnemy->Owner) && !pEnemy->Owner->IsNeutral()
						&& IsCellInSightOfAI(pHouse, pEnemy->GetMapCoords()))
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

				if (siegeShips.size() >= 2 || (siegeShips.size() == 1 && escorts.size() >= 2))
				{
					for (auto const pBld : knownEnemyBuildingList)
					{
						if (!isKnownEnemyBuilding(pBld))
							continue;

						const bool isDefense = pBld->Type->IsBaseDefense && GroundRangeOf(pBld->Type) > 0;
						const int worth = isDefense ? 300 : valuableBuildingWorth(pBld->Type);

						if (worth <= 0 || worth <= bestScore)
							continue;

						// A water cell of our fleet's sea within range, outside the other defenses' reach.
						const CellStruct bc = pBld->GetMapCoords();
						CellStruct spot = CellStruct::Empty;

						const int minRangeCells = std::max(4, fleetMinRange / Unsorted::LeptonsPerCell + 1);

						for (int r = rangeCells - 1; r >= minRangeCells && spot == CellStruct::Empty; r -= 2)
						{
							for (int k = 0; k < 16 && spot == CellStruct::Empty; ++k)
							{
								constexpr int DirX[16] = { 10, 9, 7, 4, 0, -4, -7, -9, -10, -9, -7, -4, 0, 4, 7, 9 };
								constexpr int DirY[16] = { 0, 4, 7, 9, 10, 9, 7, 4, 0, -4, -7, -9, -10, -9, -7, -4 };
								const CellStruct c { static_cast<short>(bc.X + DirX[k] * r / 10), static_cast<short>(bc.Y + DirY[k] * r / 10) };
								auto const pCell = MapClass::Instance.CoordinatesLegal(c) ? MapClass::Instance.TryGetCellAt(c) : nullptr;

								if (pCell && pCell->LandType == LandType::Water
									&& MapClass::Instance.GetMovementZoneType(c, MovementZone::Water, false) == fleetZone
									&& !inKnownDefenseReach(c, pBld) && !inEnemyShipReach(c))
								{
									spot = c;
								}
							}
						}

						if (spot != CellStruct::Empty)
						{
							bestScore = worth;
							pTarget = pBld;
							firing = spot;
						}
					}
				}

				const int utility = pTarget ? (320 + bestScore / 4) : -1000;

				if (utility > 0 && shouldTryStart(NeuralPlanId::NavalStrikeGroup, utility))
				{
					std::vector<FootClass*> group(siegeShips.begin(), siegeShips.begin() + std::min<size_t>(siegeShips.size(), 6));
					group.insert(group.end(), escorts.begin(), escorts.begin() + std::min<size_t>(escorts.size(), 4));
					startPlan(NeuralPlanId::NavalStrikeGroup, group, pTarget->GetMapCoords(), firing, pTarget->UniqueID, 5400);
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
				// Tracked in current sight only: explored cells would show it through the fog.
				const bool targetSeen = pTarget && pTarget->CloakState == CloakState::Uncloaked
					&& IsCellInSightOfAI(pHouse, pTarget->GetMapCoords());

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
						|| !IsCellInSightOfAI(pHouse, pTech->GetMapCoords()))
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
						|| (avoidB != CellStruct::Empty && bc.DistanceFrom(avoidB) < 15.0))
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

					if (Unsorted::CurrentFrame >= plan.AuxPeak || aFallen || bFallen)
					{
						auto const pNewA = pickRaidTarget(plan.TargetCell, plan.StagingCell);
						auto const pNewB = pNewA ? pickRaidTarget(pNewA->GetMapCoords(), plan.TargetCell) : nullptr;

						if (!pNewA)
						{
							finishPlan(NeuralPlanId::RotatingRaids, true, true);
						}
						else
						{
							plan.TargetCell = pNewA->GetMapCoords();
							plan.StagingCell = pNewB ? pNewB->GetMapCoords() : pNewA->GetMapCoords();
							plan.AuxPeak = Unsorted::CurrentFrame + 900;

							for (const DWORD uid : plan.Members)
							{
								if (auto const pFoot = findOwnFoot(uid))
									pFoot->SetTarget(nullptr);
							}
						}
					}

					if (plan.Active)
					{
						attackMoveGroup(groupA, MapClass::Instance.TryGetCellAt(plan.TargetCell));
						attackMoveGroup(groupB, MapClass::Instance.TryGetCellAt(plan.StagingCell));
					}
				}
			}
			else if (planReady(NeuralPlanId::RotatingRaids) && Unsorted::CurrentFrame >= 6000)
			{
				std::vector<FootClass*> group = collectIdleGround(false);
				auto const pTargetA = pickRaidTarget(CellStruct::Empty, CellStruct::Empty);
				auto const pTargetB = pTargetA ? pickRaidTarget(pTargetA->GetMapCoords(), CellStruct::Empty) : nullptr;
				const int utility = (pTargetA && pTargetB && group.size() >= 8) ? 280 : -1000;

				if (utility > 0 && shouldTryStart(NeuralPlanId::RotatingRaids, utility))
				{
					sortFastest(group);
					group.resize(6);
					startPlan(NeuralPlanId::RotatingRaids, group, pTargetA->GetMapCoords(), pTargetB->GetMapCoords(), 0, 5400);
					planOf(NeuralPlanId::RotatingRaids).AuxBaseline = 3;
					planOf(NeuralPlanId::RotatingRaids).AuxPeak = Unsorted::CurrentFrame + 900;
				}
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

		// ---------------------------------------------------------------- Plan 31: Rolling Siege
		// Against a fortified base: artillery escorted by mobile anti-air and a few tanks takes
		// the known defenses out one by one, always from outside their reach, moving on to the
		// next defense nearest to the last one (within 20 cells) until none is left.
		{
			auto& plan = planOf(NeuralPlanId::RollingSiege);

			auto pickNextDefense = [&](const CellStruct& from, int artilleryRange) -> std::pair<BuildingClass*, CellClass*>
			{
				BuildingClass* pBest = nullptr;
				CellClass* pBestStage = nullptr;
				double bestDist = 1e9;

				for (auto const pBld : knownEnemyBuildingList)
				{
					if (!isKnownEnemyBuilding(pBld) || !pBld->Type->IsBaseDefense)
						continue;

					const int defenseRange = GroundRangeOf(pBld->Type);
					const CellStruct bc = pBld->GetMapCoords();
					const double d = bc.DistanceFrom(from);

					if (defenseRange <= 0 || artilleryRange < defenseRange + 2 * Unsorted::LeptonsPerCell || zoneOf(bc) != homeZone
						|| d >= bestDist || d > 20.0)
					{
						continue;
					}

					const double standOff = (defenseRange + 2 * Unsorted::LeptonsPerCell) / static_cast<double>(Unsorted::LeptonsPerCell) + 0.5;
					auto const pStage = landCellInZone(pointAlong(bc, homeCell, standOff, 0.0), homeZone, 2);

					if (pStage)
					{
						bestDist = d;
						pBest = pBld;
						pBestStage = pStage;
					}
				}

				return { pBest, pBestStage };
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

			if (plan.Active)
			{
				BuildingClass* pDefense = FindBuildingByID(plan.TargetID);
				const int artilleryRange = groupArtilleryRange(plan.Members);

				if (!pDefense)
					plan.Progress++; // The current defense fell.

				if (!pDefense && artilleryRange > 0)
				{
					auto const [pNext, pNextStage] = pickNextDefense(plan.TargetCell, artilleryRange);

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
					for (const DWORD uid : plan.Members)
					{
						if (handleSiegeDeployMember(uid, pDefense, pStage))
							continue;

						auto const pFoot = findOwnFoot(uid);

						if (!pFoot)
							continue;

						const bool isArtillery = SiegeRangeOf(pFoot->GetTechnoType()) >= 8 * Unsorted::LeptonsPerCell;
						const bool inPlace = pFoot->GetMapCoords().DistanceFrom(plan.StagingCell) <= 3.0;

						if (isArtillery && inPlace)
						{
							if (pFoot->Target != pDefense)
							{
								pFoot->SetTarget(pDefense);
								pFoot->QueueMission(Mission::Attack, true);
							}
						}
						else if (pFoot->Target == nullptr || (isArtillery && !inPlace))
						{
							// Escorts and AA stay with the battery, which moves on as one.
							IssueMoveOrder(pFoot, pStage, isArtillery ? 2.0 : 4.0);
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
					else if (escorts.size() < 3)
					{
						escorts.push_back(pFoot);
					}
				}

				for (auto const pAA : collectIdleGround(true))
				{
					if (escorts.size() < 5 && std::find(escorts.begin(), escorts.end(), pAA) == escorts.end()
						&& std::find(artillery.begin(), artillery.end(), pAA) == artillery.end())
					{
						escorts.push_back(pAA);
					}
				}

				std::pair<BuildingClass*, CellClass*> first { nullptr, nullptr };

				if (artillery.size() >= 3)
					first = pickNextDefense(homeCell, artilleryRange);

				// The first defense may be far from home: search from our side of the enemy base.
				if (!first.first && artillery.size() >= 3 && knownEnemyCentroid != CellStruct::Empty)
					first = pickNextDefense(pointAlong(knownEnemyCentroid, homeCell, 12.0, 0.0), artilleryRange);

				const int utility = first.first ? (300 + std::min(150, foe.KnownDefenses * 20)) : -1000;

				if (utility > 0 && shouldTryStart(NeuralPlanId::RollingSiege, utility))
				{
					artillery.resize(std::min<size_t>(artillery.size(), 6));
					artillery.insert(artillery.end(), escorts.begin(), escorts.end());
					startPlan(NeuralPlanId::RollingSiege, artillery, first.first->GetMapCoords(), first.second->MapCoords,
						first.first->UniqueID, 9000);
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

				factories.resize(std::min<size_t>(factories.size(), 3));

				std::vector<FootClass*> group = collectAirStrikers();
				std::vector<FootClass*> ground = collectIdleGround(false);
				sortFastest(ground);
				group.insert(group.end(), ground.begin(), ground.end());
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

		// Rebuild the member set used to keep plan units out of the generic recruitment.
		macro.PlanMembers.clear();

		for (auto const& plan : macro.Plans)
		{
			if (plan.Active)
				macro.PlanMembers.insert(plan.Members.begin(), plan.Members.end());
		}
	}
} // namespace

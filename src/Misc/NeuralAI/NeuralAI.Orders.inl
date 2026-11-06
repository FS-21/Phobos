// NeuralAI.Orders.inl: Repair, enter and attack-move orders, boarding, island landing and ferry waypoints.
// Part of NeuralAI.cpp (included in order; one translation unit).

namespace
{
	// Picks the repair station (UnitRepair=yes) this unit should use: only stations it can
	// actually reach (naval units at naval stations, ground units at land stations,
	// amphibious/hover units at either), balancing load by adding the current queue (our
	// units already entering or docked there) to the distance. No more single-depot queues.
	BuildingClass* FindBestRepairStation(HouseClass* pHouse, FootClass* pFoot, bool airlifted = false)
	{
		auto const pType = pFoot ? pFoot->GetTechnoType() : nullptr;

		// Repairs cost credits: none while broke (UpdateEconomyCrisis).
		if (!pHouse || !pType || GetMacroState(pHouse).BrokeHoldActive)
			return nullptr;

		const CellStruct fc = pFoot->GetMapCoords();
		const bool footNaval = pType->Naval;
		const bool footAmphibious = pType->SpeedType == SpeedType::Hover || pType->SpeedType == SpeedType::Amphibious
			|| pType->MovementZone == MovementZone::Amphibious || pType->MovementZone == MovementZone::AmphibiousDestroyer
			|| pType->MovementZone == MovementZone::AmphibiousCrusher;
		BuildingClass* pBest = nullptr;
		double bestCost = 1e9;

		for (auto const pBld : pHouse->Buildings)
		{
			if (!pBld || pBld->InLimbo || !pBld->IsAlive || !pBld->Type || !pBld->Type->UnitRepair
				|| pBld->CurrentMission == Mission::Selling)
			{
				continue;
			}

			const bool stationNaval = pBld->Type->Naval;

			if ((footNaval && !stationNaval && !footAmphibious) || (!footNaval && stationNaval && !footAmphibious))
				continue;

			// Reachable when some cell around the foundation is in the unit's movement zone.
			const CellStruct bc = pBld->GetMapCoords();
			const int w = pBld->Type->GetFoundationWidth();
			const int h = pBld->Type->GetFoundationHeight(false);
			bool reachable = false;

			for (int dy = -1; dy <= h && !reachable; ++dy)
			{
				for (int dx = -1; dx <= w && !reachable; ++dx)
				{
					if (dx >= 0 && dx < w && dy >= 0 && dy < h)
						continue;

					const CellStruct edge { static_cast<short>(bc.X + dx), static_cast<short>(bc.Y + dy) };

					if (AreZonesConnected(fc, edge, pType->MovementZone))
						reachable = true;
				}
			}

			// An airlifted unit (carried by a carryall) does not need a ground path.
			if (!reachable && !airlifted)
				continue;

			int queue = 0;

			for (auto const pTech : TechnoClass::Array)
			{
				auto const pOther = abstract_cast<FootClass*>(pTech);

				if (pOther && pOther != pFoot && pOther->Owner == pHouse && pOther->IsAlive && !pOther->InLimbo
					&& (pOther->Destination == pBld || pOther->ArchiveTarget == pBld || pOther->ContainsLink(pBld))
					&& (pOther->CurrentMission == Mission::Enter || pOther->ContainsLink(pBld)))
				{
					queue++;
				}
			}

			const double cost = fc.DistanceFrom(bc) + queue * 8.0;

			if (cost < bestCost)
			{
				bestCost = cost;
				pBest = pBld;
			}
		}

		return pBest;
	}

	// True while a transport is heading to, waiting at or docked in a repair station.
	bool IsTransportUnderRepair(FootClass* pTrans)
	{
		if (!pTrans)
			return false;

		for (AbstractClass* const pRef : { pTrans->Destination, pTrans->ArchiveTarget, static_cast<AbstractClass*>(pTrans->GetNthLink(0)) })
		{
			if (auto const pBld = abstract_cast<BuildingClass*>(pRef))
			{
				if (pBld->Type && pBld->Type->UnitRepair && (pTrans->CurrentMission == Mission::Enter || pTrans->ContainsLink(pBld)))
					return true;
			}
		}

		return false;
	}

	void OrderFootToEnter(FootClass* pFoot, TechnoClass* pEnterTarget)
	{
		if (!pFoot || !pEnterTarget || pFoot->InLimbo || !pFoot->IsAlive || pEnterTarget->InLimbo
			|| !pEnterTarget->IsAlive)
		{
			return;
		}

		if (pFoot->CurrentMission == Mission::Enter || pFoot->QueuedMission == Mission::Enter)
		{
			if (pFoot->ContainsLink(pEnterTarget) || pFoot->ArchiveTarget == pEnterTarget
				|| pFoot->Destination == pEnterTarget)
			{
				return;
			}

			// When FootClass::Mission_Enter is driving a large vehicle (e.g. Slave
			// Miner / MCV) onto the boarding cell next to pEnterTarget, the YR engine
			// sets pFoot->Destination to that adjacent CellClass*. Do NOT reset its
			// navigation list or spam SetDestination!
			if (pFoot->Destination != nullptr)
			{
				const CellStruct dc = CellClass::Coord2Cell(pFoot->Destination->GetCoords());
				if (dc.DistanceFrom(pEnterTarget->GetMapCoords()) <= 3.5)
					return;
			}
		}

		// Match EventClass::Execute (MEGA_MISSION, 0x004C73B9-0x004C747C) exact call
		// order: UnitClass::Assign_Destination (0x00741C54) and
		// InfantryClass::Assign_Destination (0x0051AC2A) ONLY establish radio contact
		// (RequestLink + RequestLoading) or ArchiveTarget when CurrentMission ==
		// Mission::Enter or QueuedMission == Mission::Enter BEFORE SetDestination!
		pFoot->ClearMegaMissionData();

		if (pFoot->CurrentMission == Mission::Guard || pFoot->CurrentMission == Mission::Area_Guard
			|| pFoot->CurrentMission == Mission::None || pFoot->CurrentMission == Mission::Sleep)
		{
			pFoot->ForceMission(Mission::Enter);
		}
		else
		{
			pFoot->QueueMission(Mission::Enter, false);
		}

		pFoot->SetArchiveTarget(nullptr);
		pFoot->ClearNavigationList();
		pFoot->SetTarget(nullptr);
		pFoot->SetDestination(pEnterTarget, true);

		if (pFoot->ReadyToNextMission())
			pFoot->NextMission();
	}

	bool CanFootDamageBuildings(const FootClass* pFoot)
	{
		if (!pFoot)
			return false;

		const auto pType = pFoot->GetTechnoType();
		if (!pType)
			return false;

		if (pType->Spawns != nullptr)
			return true;

		for (int wIdx = 0; wIdx < 2; ++wIdx)
		{
			const auto pWS = pFoot->GetWeapon(wIdx);
			const auto pWpn = pWS ? pWS->WeaponType : nullptr;
			if (!pWpn)
				continue;

			if (pWpn->Spawner)
				return true;

			if (pWpn->Damage <= 0)
				continue;

			if (pWpn->Projectile && !pWpn->Projectile->AG)
				continue;

			if (pWpn->Damage <= 5)
				continue;

			return true;
		}

		return false;
	}

	void OrderFootToAttackMove(FootClass* pFoot, AbstractClass* pDest)
	{
		if (!pFoot || !pDest || pFoot->InLimbo || !pFoot->IsAlive)
			return;

		AbstractClass* pResolvedDest = pDest;
		CellClass* pDestCell = (pDest->WhatAmI() == AbstractType::Cell)
			? static_cast<CellClass*>(pDest)
			: MapClass::Instance.TryGetCellAt(CellClass::Coord2Cell(pDest->GetCoords()));

		// Long-range units (ground weapon or drone launcher reaching >= 8 cells) sent against an
		// enemy building attack it directly, like a player's attack order: the engine stops them
		// at their maximum range. An attack-move to the building cell would drive them right
		// into the enemy defenses (e.g. a drone carrier hovering on top of its target).
		{
			BuildingClass* pEnemyBld = abstract_cast<BuildingClass*>(pDest);

			if (!pEnemyBld && pDestCell)
				pEnemyBld = pDestCell->GetBuilding();

			auto const pFootType = pFoot->GetTechnoType();
			int longestGroundRange = 0;

			for (int i = 0; pFootType && i < 2; ++i)
			{
				auto const pWS = pFootType->GetWeapon(i);
				auto const pW = pWS ? pWS->WeaponType : nullptr;

				if (pW && (pW->Spawner || (pW->Damage > 0 && (!pW->Projectile || pW->Projectile->AG))))
					longestGroundRange = std::max(longestGroundRange, pW->Range);
			}

			if (pEnemyBld && pEnemyBld->IsAlive && !pEnemyBld->InLimbo && pEnemyBld->Owner && pFoot->Owner
				&& pEnemyBld->Owner != pFoot->Owner && !pFoot->Owner->IsAlliedWith(pEnemyBld->Owner)
				&& longestGroundRange >= 8 * Unsorted::LeptonsPerCell)
			{
				if (pFoot->Target != pEnemyBld)
				{
					pFoot->ClearMegaMissionData();
					pFoot->SetTarget(pEnemyBld);
					pFoot->QueueMission(Mission::Attack, true);
					CountDecision(NeuralDecision::MoveOrderIssued, pFoot->Owner->ArrayIndex);
				}
				else
				{
					CountDecision(NeuralDecision::MoveOrderSkipped, pFoot->Owner->ArrayIndex);
				}

				return;
			}
		}

		if (pDestCell && pDestCell->GetBuilding() != nullptr)
		{
			const CellStruct fc = pFoot->GetMapCoords();
			const CellStruct dc = pDestCell->MapCoords;
			CellClass* pBestAdj = nullptr;
			int bestDist = 999999;
			for (int r = 1; r <= 4 && !pBestAdj; ++r)
			{
				for (int dy = -r; dy <= r; ++dy)
				{
					for (int dx = -r; dx <= r; ++dx)
					{
						if (std::abs(dx) != r && std::abs(dy) != r)
							continue;

						const CellStruct cc { static_cast<short>(dc.X + dx), static_cast<short>(dc.Y + dy) };
						if (auto const pC = MapClass::Instance.TryGetCellAt(cc))
						{
							if (pC->GetBuilding() != nullptr)
								continue;

							if (pC->LandType == LandType::Rock || pC->LandType == LandType::Wall)
								continue;

							const int d = std::abs(cc.X - fc.X) + std::abs(cc.Y - fc.Y);
							if (d < bestDist)
							{
								bestDist = d;
								pBestAdj = pC;
							}
						}
					}
				}
			}

			if (pBestAdj)
			{
				pDestCell = pBestAdj;
				if (pDest->WhatAmI() == AbstractType::Cell)
					pResolvedDest = pBestAdj;
			}
		}

		// Ships cannot attack-move onto land: the nearest water cell of their own body instead.
		if (pDestCell)
		{
			if (auto const pReach = NearestReachableNavalCell(pFoot, pDestCell->MapCoords))
			{
				pDestCell = pReach;
				pResolvedDest = pReach;
			}
		}

		// Hysteresis: a unit already attack-moving to (almost) the same place keeps its order.
		// Re-issuing it resets the target, re-paths and restarts the locomotor (ships slow down
		// and their wake animation restarts), so recomputed cells within 3 cells are ignored.
		{
			AbstractClass* pNewMegaDest = (pResolvedDest->WhatAmI() == AbstractType::Cell) ? pResolvedDest : pDestCell;
			auto const pOldMegaCell = abstract_cast<CellClass*>(pFoot->MegaDestination);
			auto const pNewMegaCell = abstract_cast<CellClass*>(pNewMegaDest);
			const bool stillAttackMoving = pFoot->MegaMission == Mission::AttackMove
				&& (pFoot->CurrentMission == Mission::Move || pFoot->CurrentMission == Mission::Attack
					|| pFoot->HaveAttackMoveTarget);
			const AbstractClass* pNewMegaTarget = (pResolvedDest->WhatAmI() == AbstractType::Cell) ? nullptr : pResolvedDest;

			if (stillAttackMoving && pOldMegaCell && pNewMegaCell && pFoot->MegaTarget == pNewMegaTarget
				&& pOldMegaCell->MapCoords.DistanceFrom(pNewMegaCell->MapCoords) <= 3.0)
			{
				CountDecision(NeuralDecision::MoveOrderSkipped, pFoot->Owner ? pFoot->Owner->ArrayIndex : -1);
				return;
			}
		}

		CountDecision(NeuralDecision::MoveOrderIssued, pFoot->Owner ? pFoot->Owner->ArrayIndex : -1);
		pFoot->SetTarget(nullptr);
		pFoot->SetArchiveTarget(nullptr);
		pFoot->MegaMission = Mission::AttackMove;

		if (pResolvedDest->WhatAmI() == AbstractType::Cell)
		{
			pFoot->MegaDestination = pResolvedDest;
			pFoot->MegaTarget = nullptr;
		}
		else
		{
			pFoot->MegaDestination = pDestCell;
			pFoot->MegaTarget = pResolvedDest;
		}

		pFoot->HaveAttackMoveTarget = false;
		pFoot->SetDestination(pFoot->MegaDestination ? pFoot->MegaDestination : pResolvedDest, true);
		if (pFoot->CurrentMission == Mission::Guard || pFoot->CurrentMission == Mission::Area_Guard
			|| pFoot->CurrentMission == Mission::None || pFoot->CurrentMission == Mission::Sleep)
		{
			pFoot->ForceMission(Mission::Move);
		}
		else
		{
			pFoot->QueueMission(Mission::Move, true);
		}
	}

	bool HasActiveBoardingPassenger(TechnoClass* pTrans, int transZone)
	{
		if (!pTrans)
			return false;

		if (pTrans->HasAnyLink())
		{
			if (auto const pLinked = pTrans->GetNthLink(0))
			{
				if (pLinked->IsAlive && !pLinked->InLimbo && pLinked->Owner == pTrans->Owner
					&& (pLinked->CurrentMission == Mission::Enter || pLinked->QueuedMission == Mission::Enter))
				{
					return true;
				}
			}
		}

		const CellStruct tc = pTrans->GetMapCoords();

		for (auto const pU : UnitClass::Array)
		{
			if (!pU || pU == pTrans || pU->Owner != pTrans->Owner || pU->InLimbo || !pU->IsAlive)
				continue;

			if (pU->CurrentMission != Mission::Enter && pU->QueuedMission != Mission::Enter)
				continue;

			if (pU->ContainsLink(pTrans) || pU->ArchiveTarget == pTrans || pU->Destination == pTrans)
				return true;

			if (pU->Destination != nullptr && pU->GetMapCoords().DistanceFrom(tc) <= 6.0
				&& CellClass::Coord2Cell(pU->Destination->GetCoords()).DistanceFrom(tc) <= 3.0)
			{
				return true;
			}
		}

		for (auto const pI : InfantryClass::Array)
		{
			if (!pI || pI->Owner != pTrans->Owner || pI->InLimbo || !pI->IsAlive)
				continue;

			if (pI->CurrentMission != Mission::Enter && pI->QueuedMission != Mission::Enter)
				continue;

			if (pI->ContainsLink(pTrans) || pI->ArchiveTarget == pTrans || pI->Destination == pTrans)
				return true;

			if (pI->Destination != nullptr && pI->GetMapCoords().DistanceFrom(tc) <= 6.0
				&& CellClass::Coord2Cell(pI->Destination->GetCoords()).DistanceFrom(tc) <= 3.0)
			{
				return true;
			}
		}

		return false;
	}

	bool ClearStaleEnterOrUnreachableCrossIslandMission(FootClass* pFoot, bool canCrossWaterDirectly)
	{
		if (!pFoot || pFoot->InLimbo || !pFoot->IsAlive)
			return false;

		const auto pFootType = pFoot->GetTechnoType();

		if (!pFootType)
			return false;

		const CellStruct footCoords = pFoot->GetMapCoords();
		const int fz = MapClass::Instance.GetMovementZoneType(footCoords, MovementZone::Normal, pFoot->OnBridge);

		if (pFoot->CurrentMission == Mission::Enter || pFoot->QueuedMission == Mission::Enter)
		{
			TechnoClass* pEnterT = pFoot->HasAnyLink()
				? pFoot->GetNthLink(0)
				: abstract_cast<TechnoClass*>(pFoot->ArchiveTarget ? pFoot->ArchiveTarget : pFoot->Destination);
			bool validEnter = false;

			if (pEnterT && pEnterT->IsAlive && !pEnterT->InLimbo)
			{
				const auto pEnterType = pEnterT->GetTechnoType();
				const int ez
					= ObjectZone(pEnterT, MovementZone::Normal);

				if (pEnterT->WhatAmI() == AbstractType::Building)
				{
					if (auto const pEnterBld = static_cast<BuildingClass*>(pEnterT))
					{
						const bool sameLandmass = (canCrossWaterDirectly || fz < 0 || ez < 0 || ez == fz);

						if (pEnterBld->Type && pEnterBld->Type->Bunker)
						{
							validEnter = sameLandmass
								&& (pEnterBld->BunkerLinkedItem == nullptr || pEnterBld->BunkerLinkedItem == pFoot);
						}
						else if (pEnterBld->Type && pEnterBld->Type->CanBeOccupied)
						{
							const int maxOcc = std::clamp(pEnterBld->Type->MaxNumberOccupants, 1, 10);
							validEnter = sameLandmass && pEnterBld->Occupants.Count < maxOcc;
						}
						else
						{
							validEnter = sameLandmass;
						}
					}
				}
				else if (pEnterType && pEnterType->Passengers > 0)
				{
					const bool hasRoom = CanPassengerFitInTransport(pEnterT, pFootType);
					const auto pECell = pEnterT->GetCell();
					const bool reachableShore = canCrossWaterDirectly || (fz >= 0 && ez == fz)
						|| pFoot->ContainsLink(pEnterT)
						|| (pECell && pECell->LandType == LandType::Water && pFoot->DistanceFrom(pEnterT) <= 1280);
					validEnter = hasRoom && reachableShore;
				}
			}

			if (validEnter)
				return false;

			if (pFoot->HasAnyLink())
				pFoot->SendToFirstLink(RadioCommand::NotifyUnlink);

			pFoot->ClearMegaMissionData();
			pFoot->SetArchiveTarget(nullptr);
			pFoot->SetDestination(nullptr, true);
			pFoot->ClearNavigationList();
			pFoot->ForceMission(Mission::Guard);

			return true;
		}

		if (!canCrossWaterDirectly && fz >= 0
			&& (pFoot->CurrentMission == Mission::Move || pFoot->CurrentMission == Mission::AttackMove
				|| pFoot->MegaMission == Mission::AttackMove || pFoot->CurrentMission == Mission::Hunt
				|| pFoot->CurrentMission == Mission::Attack))
		{
			if (pFoot->Target == nullptr
				&& (pFoot->CurrentMission == Mission::Hunt || pFoot->CurrentMission == Mission::AttackMove
					|| pFoot->MegaMission == Mission::AttackMove))
			{
				bool reachedDest = (pFoot->Destination == nullptr || pFoot->DistanceFrom(pFoot->Destination) <= 384);

				if (!reachedDest && pFoot->Owner)
				{
					bool anyEnemyInZone = false;

					for (auto const pEB : BuildingClass::Array)
					{
						if (!pEB || pEB->InLimbo || !pEB->IsAlive || !pEB->Owner || !pEB->Type
							|| pEB->Type->Insignificant || pEB->Type->InvisibleInGame)
						{
							continue;
						}

						if (pEB->Owner == pFoot->Owner || pFoot->Owner->IsAlliedWith(pEB->Owner) || !IsEnemyBuildingKnown(pFoot->Owner, pEB)
							|| pEB->Owner->IsNeutral() || pEB->Owner->IsObserver() || pEB->Owner->Defeated)
						{
							continue;
						}

						if (ObjectZone(pEB, MovementZone::Normal)
							== fz)
						{
							anyEnemyInZone = true;
							break;
						}
					}

					if (!anyEnemyInZone)
					{
						for (auto const pEU : UnitClass::Array)
						{
							if (!pEU || pEU->InLimbo || !pEU->IsAlive || !pEU->Owner || !pEU->Type || pEU->Type->Naval)
								continue;

							if (pEU->Owner == pFoot->Owner || pFoot->Owner->IsAlliedWith(pEU->Owner) || !IsCellVisibleToAI(pFoot->Owner, pEU->GetCell())
								|| pEU->Owner->IsNeutral() || pEU->Owner->IsObserver() || pEU->Owner->Defeated)
							{
								continue;
							}

							if (ObjectZone(pEU, MovementZone::Normal)
								== fz)
							{
								anyEnemyInZone = true;
								break;
							}
						}
					}

					if (!anyEnemyInZone)
						reachedDest = true;
				}

				if (reachedDest)
				{
					pFoot->ClearMegaMissionData();
					pFoot->SetDestination(nullptr, true);
					pFoot->ClearNavigationList();
					pFoot->ForceMission(Mission::Guard);

					return true;
				}
			}

			if (pFoot->Destination != nullptr && pFoot->Target == nullptr)
			{
				const CellStruct dc = CellClass::Coord2Cell(pFoot->Destination->GetCoords());
				const int dz = MapClass::Instance.GetMovementZoneType(dc, MovementZone::Normal, false);
				auto const pDCell = MapClass::Instance.TryGetCellAt(dc);

				if ((dz >= 0 && dz != fz) || (pDCell && pDCell->LandType == LandType::Water))
				{
					pFoot->ClearMegaMissionData();
					pFoot->SetDestination(nullptr, true);
					pFoot->ClearNavigationList();
					pFoot->ForceMission(Mission::Guard);

					return true;
				}
			}

			if (pFoot->Target != nullptr)
			{
				const CellStruct tc = CellClass::Coord2Cell(pFoot->Target->GetCoords());
				const int tz = MapClass::Instance.GetMovementZoneType(tc, MovementZone::Normal, false);
				int wpnRange = 1280;

				if (pFootType->GetWeapon(0) && pFootType->GetWeapon(0)->WeaponType)
					wpnRange = std::max(wpnRange, pFootType->GetWeapon(0)->WeaponType->Range);

				if (tz >= 0 && tz != fz && pFoot->DistanceFrom(pFoot->Target) > (wpnRange + 512))
				{
					pFoot->ClearMegaMissionData();
					pFoot->SetTarget(nullptr);
					pFoot->SetDestination(nullptr, true);
					pFoot->ClearNavigationList();
					pFoot->ForceMission(Mission::Guard);

					return true;
				}
			}
		}

		return false;
	}

	bool IsTrueBaseStructureTarget(const BuildingTypeClass* pTargetBld)
	{
		if (!pTargetBld)
			return false;

		return (pTargetBld->ConstructionYard || pTargetBld->Refinery || pTargetBld->ResourceGatherer
			|| pTargetBld->ResourceDestination || pTargetBld->Factory != AbstractType::None
			|| pTargetBld->PowerBonus > pTargetBld->PowerDrain || pTargetBld->BaseNormal || pTargetBld->IsBaseDefense
			|| pTargetBld->Radar || pTargetBld->SpySat || pTargetBld->UnitRepair || pTargetBld->SuperWeapon != -1
			|| pTargetBld->Storage > 0 || pTargetBld->Grinding || pTargetBld->Cloning || pTargetBld->OrePurifier
			|| pTargetBld->FactoryPlant || pTargetBld->SecretLab || pTargetBld->Hospital || pTargetBld->Armory
			|| (pTargetBld->TechLevel >= 0 && pTargetBld->TechLevel <= 10));
	}

	CellClass* FindSafeIslandLandingCell(
		HouseClass* pHouse, const CellStruct& islandRefCell, int targetIslandZone, bool preferOre)
	{
		bool islandHasEnemyPresence = false;
		for (auto const pEB : BuildingClass::Array)
		{
			if (!pEB || pEB->InLimbo || !pEB->IsAlive || !pEB->Owner || !pEB->Type || pEB->Type->Insignificant
				|| pEB->Type->InvisibleInGame || pEB->Type->Naval)
			{
				continue;
			}

			if (pEB->Owner == pHouse || pHouse->IsAlliedWith(pEB->Owner) || pEB->Owner->IsNeutral()
				|| pEB->Owner->IsObserver() || pEB->Owner->Defeated || !IsEnemyBuildingKnown(pHouse, pEB))
			{
				continue;
			}

			const CellStruct ebc = pEB->GetMapCoords();
			if (targetIslandZone >= 0)
			{
				if (MapClass::Instance.GetMovementZoneType(ebc, MovementZone::Normal, pEB->OnBridge) == targetIslandZone)
				{
					islandHasEnemyPresence = true;
					break;
				}
			}
			else if (std::max(std::abs(ebc.X - islandRefCell.X), std::abs(ebc.Y - islandRefCell.Y)) <= 45)
			{
				islandHasEnemyPresence = true;
				break;
			}
		}

		CellClass* pBestLanding = nullptr;
		int bestScore = -999999;
		const int maxR = 64;
		for (int dy = -maxR; dy <= maxR; dy += (std::abs(dy) <= 32 ? 2 : 3))
		{
			for (int dx = -maxR; dx <= maxR; dx += (std::abs(dx) <= 32 ? 2 : 3))
			{
				const CellStruct cand { static_cast<short>(islandRefCell.X + dx),
					static_cast<short>(islandRefCell.Y + dy) };
				auto const pCell = MapClass::Instance.TryGetCellAt(cand);
				if (!pCell || pCell->LandType == LandType::Water || pCell->LandType == LandType::Rock
					|| pCell->GetBuilding())
				{
					continue;
				}

				if (targetIslandZone >= 0)
				{
					const int cz = MapClass::Instance.GetMovementZoneType(cand, MovementZone::Normal, false);
					if (cz != targetIslandZone)
						continue;
				}

				if (!pCell->IsClearToMove(SpeedType::Track, true, true, -1, MovementZone::Normal, -1, false)
					|| !pCell->IsClearToMove(
						SpeedType::Hover, true, true, -1, MovementZone::AmphibiousDestroyer, -1, false))
				{
					continue;
				}

				bool nearCoast = (pCell->LandType == LandType::Beach);
				if (!nearCoast)
				{
					const CellStruct neighbors[8] = { { static_cast<short>(cand.X + 1), cand.Y },
						{ static_cast<short>(cand.X - 1), cand.Y }, { cand.X, static_cast<short>(cand.Y + 1) },
						{ cand.X, static_cast<short>(cand.Y - 1) }, { static_cast<short>(cand.X + 2), cand.Y },
						{ static_cast<short>(cand.X - 2), cand.Y }, { cand.X, static_cast<short>(cand.Y + 2) },
						{ cand.X, static_cast<short>(cand.Y - 2) } };
					for (const auto& nc : neighbors)
					{
						if (auto const pNC = MapClass::Instance.TryGetCellAt(nc))
						{
							if ((pNC->LandType == LandType::Water || pNC->LandType == LandType::Beach)
								&& std::abs(static_cast<int>(pCell->Level) - static_cast<int>(pNC->Level)) <= 1)
							{
								nearCoast = true;
								break;
							}
						}
					}
				}

				if (!nearCoast)
				{
					const CellStruct ring3[4] = { { static_cast<short>(cand.X + 3), cand.Y },
						{ static_cast<short>(cand.X - 3), cand.Y }, { cand.X, static_cast<short>(cand.Y + 3) },
						{ cand.X, static_cast<short>(cand.Y - 3) } };
					for (const auto& nc3 : ring3)
					{
						if (auto const pNC3 = MapClass::Instance.TryGetCellAt(nc3))
						{
							if ((pNC3->LandType == LandType::Water || pNC3->LandType == LandType::Beach)
								&& std::abs(static_cast<int>(pCell->Level) - static_cast<int>(pNC3->Level)) <= 1)
							{
								nearCoast = true;
								break;
							}
						}
					}
				}

				if (!nearCoast)
					continue;

				const int distFromRef = std::abs(dx) + std::abs(dy);
				// On an enemy-occupied island, islandRefCell is typically an enemy building:
				// prefer landing 20..36 cells away on an uncontested part of the island rather than
				// right on top of islandRefCell! On a friendly/neutral island, prefer close to islandRefCell.
				int score = islandHasEnemyPresence
					? (900 - std::abs(distFromRef - 26) * 4)
					: (900 - distFromRef * 3);
				if (pCell->FirstObject != nullptr)
				{
					score -= 550; // Prefer an unoccupied beach cell so multiple
								// transports/retries spread out!
				}

				bool allImmediateNeighborsDryLand = (pCell->LandType != LandType::Beach);
				int emptyDryNeighbors = 0;
				for (int ndy = -1; ndy <= 1; ++ndy)
				{
					for (int ndx = -1; ndx <= 1; ++ndx)
					{
						if (ndx == 0 && ndy == 0)
							continue;

						const CellStruct adj { static_cast<short>(cand.X + ndx), static_cast<short>(cand.Y + ndy) };
						auto const pAdj = MapClass::Instance.TryGetCellAt(adj);
						if (!pAdj || pAdj->LandType == LandType::Water || pAdj->GetBuilding() != nullptr
							|| !pAdj->IsClearToMove(
								SpeedType::Track, true, true, -1, MovementZone::Normal, -1, false))
						{
							allImmediateNeighborsDryLand = false;
						}
						else if (pAdj->FirstObject == nullptr)
						{
							emptyDryNeighbors++;
						}
					}
				}

				if (allImmediateNeighborsDryLand)
					score += 350;

				score += emptyDryNeighbors * 55;
				if (emptyDryNeighbors < 2)
				{
					score -= 700; // Heavily penalize congested/blocked exit cells so
								// transports shift to a clear beach zone!
				}

				if (preferOre && pCell->GetContainedTiberiumValue() > 0)
					score += 180;

				// Avoid beaches where our loaded transports were recently destroyed.
				for (auto const& [bloodyCell, bloodyFrame] : GetMacroState(pHouse).BloodyBeaches)
				{
					const double dBloody = cand.DistanceFrom(bloodyCell);

					if (dBloody <= 10.0)
						score -= 2500 - static_cast<int>(dBloody * 150.0);
				}

				int minEnemyDefDist = 9999;
				int minEnemyBldDist = 9999;
				for (auto const pBld : BuildingClass::Array)
				{
					if (!pBld || pBld->InLimbo || !pBld->IsAlive || !pBld->Owner || !pBld->Type
						|| pBld->Type->Insignificant || pBld->Type->InvisibleInGame)
					{
						continue;
					}

					if (pBld->Owner == pHouse || pHouse->IsAlliedWith(pBld->Owner) || pBld->Owner->IsNeutral()
						|| !IsEnemyBuildingKnown(pHouse, pBld))
					{
						continue;
					}

					const CellStruct bc = pBld->GetMapCoords();
					const int dist = std::max(std::abs(cand.X - bc.X), std::abs(cand.Y - bc.Y));
					if (pBld->Type->IsBaseDefense)
					{
						if (dist < minEnemyDefDist)
							minEnemyDefDist = dist;
					}

					if (dist < minEnemyBldDist)
						minEnemyBldDist = dist;
				}

				int minEnemyUnitDist = 9999;
				for (auto const pEnemyU : UnitClass::Array)
				{
					if (!pEnemyU || pEnemyU->InLimbo || !pEnemyU->IsAlive || !pEnemyU->Owner)
						continue;

					// Our own transports are always known; enemy units only when in sight.
					if (pEnemyU->Owner != pHouse && !IsCellVisibleToAI(pHouse, pEnemyU->GetCell()))
						continue;

					if (pEnemyU->Owner == pHouse)
					{
						if (pEnemyU->Type && IsAmphibiousOrAirTransport(pEnemyU->Type))
						{
							const CellStruct tc = pEnemyU->GetMapCoords();
							if (tc != islandRefCell)
							{
								const int dTrans = std::max(std::abs(cand.X - tc.X), std::abs(cand.Y - tc.Y));
								if (dTrans <= 3)
									score -= (4 - dTrans) * 450;
							}

							if (pEnemyU->Destination != nullptr)
							{
								const CellStruct dc = CellClass::Coord2Cell(pEnemyU->Destination->GetCoords());
								if (dc != islandRefCell)
								{
									const int dDest = std::max(std::abs(cand.X - dc.X), std::abs(cand.Y - dc.Y));
									if (dDest <= 2)
										score -= (3 - dDest) * 350;
								}
							}
						}

						continue;
					}

					if (pHouse->IsAlliedWith(pEnemyU->Owner) || pEnemyU->Owner->IsNeutral())
						continue;

					const CellStruct uc = pEnemyU->GetMapCoords();
					const int dist = std::max(std::abs(cand.X - uc.X), std::abs(cand.Y - uc.Y));
					if (dist < minEnemyUnitDist)
						minEnemyUnitDist = dist;
				}

				// Strongly avoid landing inside an enemy base or within range of enemy defenses/units!
				if (minEnemyDefDist <= 16)
					score -= 5000 + (17 - minEnemyDefDist) * 350;
				if (minEnemyBldDist <= 14)
					score -= 3800 + (15 - minEnemyBldDist) * 260;
				if (minEnemyUnitDist <= 11)
					score -= 2400 + (12 - minEnemyUnitDist) * 200;

				// Reward completely uncontested beachheads on the island outside enemy range!
				if (islandHasEnemyPresence && minEnemyDefDist >= 17 && minEnemyBldDist >= 15 && minEnemyUnitDist >= 12)
				{
					score += 1400;
				}

				if (score > bestScore)
				{
					bestScore = score;
					pBestLanding = pCell;
				}
			}
		}

		return pBestLanding ? pBestLanding : MapClass::Instance.TryGetCellAt(islandRefCell);
	}

	// Safe ferry routing: when the straight sea lane from a transport to its landing
	// cell crosses an enemy base or the range of its defenses, pick a detour
	// waypoint that clearly lowers the exposure. Returns CellStruct::Empty when the
	// direct lane is safe or when no detour is meaningfully safer (unavoidable).
	CellStruct FindSafeFerryWaypoint(HouseClass* pHouse, const CellStruct& fromCell, const CellStruct& toCell,
		const TechnoTypeClass* pTransType)
	{
		if (!pHouse || !pTransType || fromCell == CellStruct::Empty || toCell == CellStruct::Empty)
			return CellStruct::Empty;

		const bool isAirborneTransport = pTransType->WhatAmI() == AbstractType::AircraftType
			|| pTransType->MovementZone == MovementZone::Fly || pTransType->JumpJet || pTransType->ConsideredAircraft;

		struct FerryThreat
		{
			int X;
			int Y;
			int Radius;
			int Weight;
		};

		std::vector<FerryThreat> threats;

		auto weaponThreatRange = [isAirborneTransport](const WeaponTypeClass* pWeapon) -> int
		{
			if (!pWeapon || pWeapon->Damage <= 0)
				return 0;

			if (auto const pProj = pWeapon->Projectile)
			{
				if (isAirborneTransport ? !pProj->AA : !pProj->AG)
					return 0;
			}
			else if (isAirborneTransport)
			{
				return 0;
			}

			return pWeapon->Range / Unsorted::LeptonsPerCell;
		};

		for (auto const pBld : BuildingClass::Array)
		{
			if (!pBld || pBld->InLimbo || !pBld->IsAlive || !pBld->Owner || !pBld->Type || pBld->Type->Insignificant
				|| pBld->Type->InvisibleInGame)
			{
				continue;
			}

			if (pBld->Owner == pHouse || pHouse->IsAlliedWith(pBld->Owner) || pBld->Owner->IsNeutral()
				|| pBld->Owner->IsObserver() || pBld->Owner->Defeated || !IsEnemyBuildingKnown(pHouse, pBld))
			{
				continue;
			}

			const CellStruct bc = pBld->GetMapCoords();
			const auto pWS0 = pBld->Type->GetWeapon(0);
			const auto pWS1 = pBld->Type->GetWeapon(1);
			const int weaponRange = std::max(weaponThreatRange(pWS0 ? pWS0->WeaponType : nullptr),
				weaponThreatRange(pWS1 ? pWS1->WeaponType : nullptr));

			if (weaponRange > 0)
				threats.push_back({ bc.X, bc.Y, weaponRange + 2, 4 });
			else
				threats.push_back({ bc.X, bc.Y, 6, 1 }); // The enemy base footprint itself.
		}

		// Mobile threats in sight: warships for surface transports; for flying carriers every
		// anti-air vehicle, infantry and aircraft (flak tracks, rocket troopers, interceptors).
		for (auto const pEnemy : TechnoClass::Array)
		{
			if (!pEnemy || pEnemy->InLimbo || !pEnemy->IsAlive || !pEnemy->Owner
				|| pEnemy->WhatAmI() == AbstractType::Building)
			{
				continue;
			}

			auto const pEnemyType = pEnemy->GetTechnoType();

			if (!pEnemyType || (!isAirborneTransport && !pEnemyType->Naval))
				continue;

			if (pEnemy->Owner == pHouse || pHouse->IsAlliedWith(pEnemy->Owner) || pEnemy->Owner->IsNeutral()
				|| !IsCellVisibleToAI(pHouse, pEnemy->GetCell()))
			{
				continue;
			}

			const auto pWS0 = pEnemyType->GetWeapon(0);
			const auto pWS1 = isAirborneTransport ? pEnemyType->GetWeapon(1) : nullptr;
			const int weaponRange = std::max(weaponThreatRange(pWS0 ? pWS0->WeaponType : nullptr),
				weaponThreatRange(pWS1 ? pWS1->WeaponType : nullptr));

			if (weaponRange > 0)
			{
				const CellStruct ec = pEnemy->GetMapCoords();
				threats.push_back({ ec.X, ec.Y, weaponRange + 1, 2 });
			}
		}

		// Where our own flyers were shot down recently (UpdateAirLossMemory): the anti-air that
		// did it may be out of sight now. Weight fades 3 -> 1 over 8100 frames.
		if (isAirborneTransport)
		{
			for (auto const& [spot, frame] : GetMacroState(pHouse).AirDangerSpots)
			{
				const int age = Unsorted::CurrentFrame - frame;

				if (age >= 0 && age < 8100)
					threats.push_back({ spot.X, spot.Y, 7, 3 - age / 2700 });
			}
		}

		if (threats.empty())
			return CellStruct::Empty;

		auto segmentThreatCost = [&threats](const CellStruct& a, const CellStruct& b) -> int
		{
			const int dx = b.X - a.X;
			const int dy = b.Y - a.Y;
			const int steps = std::max(1, std::max(std::abs(dx), std::abs(dy)) / 2);
			int cost = 0;

			for (int i = 0; i <= steps; ++i)
			{
				const int px = a.X + dx * i / steps;
				const int py = a.Y + dy * i / steps;

				for (auto const& threat : threats)
				{
					const int dist = std::max(std::abs(px - threat.X), std::abs(py - threat.Y));

					if (dist < threat.Radius)
						cost += (threat.Radius - dist) * threat.Weight;
				}
			}

			return cost;
		};

		const int directCost = segmentThreatCost(fromCell, toCell);

		if (directCost <= 0)
			return CellStruct::Empty;

		const double laneDX = static_cast<double>(toCell.X - fromCell.X);
		const double laneDY = static_cast<double>(toCell.Y - fromCell.Y);
		const double laneLength = std::sqrt(laneDX * laneDX + laneDY * laneDY);

		if (laneLength < 8.0)
			return CellStruct::Empty;

		const double normalX = -laneDY / laneLength;
		const double normalY = laneDX / laneLength;
		constexpr double LaneFractions[] = { 0.35, 0.5, 0.65 };
		constexpr int LaneOffsets[] = { 10, -10, 18, -18, 26, -26, 36, -36 };

		CellStruct bestWaypoint = CellStruct::Empty;
		int bestCost = directCost;

		for (const double fraction : LaneFractions)
		{
			for (const int offset : LaneOffsets)
			{
				const CellStruct candidate {
					static_cast<short>(fromCell.X + laneDX * fraction + normalX * offset),
					static_cast<short>(fromCell.Y + laneDY * fraction + normalY * offset)
				};

				auto const pCandCell = MapClass::Instance.TryGetCellAt(candidate);

				if (!pCandCell || !MapClass::Instance.CoordinatesLegal(candidate))
					continue;

				if (!isAirborneTransport)
				{
					// Surface transports detour through open water reachable from here.
					if (pCandCell->LandType != LandType::Water
						|| !pCandCell->IsClearToMove(
							pTransType->SpeedType, true, true, -1, pTransType->MovementZone, -1, false)
						|| !AreZonesConnected(fromCell, candidate, pTransType->MovementZone))
					{
						continue;
					}
				}

				// Exposure dominates; the extra sailing distance only breaks ties.
				const int detourLength = static_cast<int>(fromCell.DistanceFrom(candidate) + candidate.DistanceFrom(toCell));
				const int cost = segmentThreatCost(fromCell, candidate) + segmentThreatCost(candidate, toCell)
					+ std::max(0, detourLength - static_cast<int>(laneLength)) / 4;

				if (cost < bestCost)
				{
					bestCost = cost;
					bestWaypoint = candidate;
				}
			}
		}

		// Only detour when it is clearly safer; otherwise exposure is unavoidable.
		return (bestWaypoint != CellStruct::Empty && bestCost * 10 < directCost * 7) ? bestWaypoint : CellStruct::Empty;
	}

} // namespace

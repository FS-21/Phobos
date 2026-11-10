// NeuralAI.Micro.inl: Tactical and special operations micro (one function).
// Part of NeuralAI.cpp (included in order; one translation unit).

namespace
{
	// Defined in NeuralAI.Scouting.inl.
	bool IsActiveScout(HouseClass* pHouse, DWORD uid);
}

// InfantryClass::Scatter runs the locomotor right away (0x51D478), so a scattered unit can leave its
// cell and be relinked into another cell's object list inside the call. Walking FirstObject/NextObject
// while scattering then follows the moved unit into the other list and can revisit units forever (the
// game frame froze on "(4) The Honeymoon"). Copy the cell's foot units first and act on the copy.
static void CollectCellFoots(CellClass* pCell, std::vector<FootClass*>& foots)
{
	if (!pCell)
		return;

	for (ObjectClass* pObj = pCell->FirstObject; pObj; pObj = pObj->NextObject)
	{
		if (auto const pFoot = abstract_cast<FootClass*>(pObj))
		{
			if (std::find(foots.begin(), foots.end(), pFoot) == foots.end())
				foots.push_back(pFoot);
		}
	}
}

void NeuralAI::ExecuteTacticalAndSpecOpsMicro(HouseClass* pHouse, const std::array<int16_t, OutputVectorSize>& scores)
{
	// Phase stopwatch (telemetry only): which part of the micro is heavy.
	NeuralPhaseClock microClock { pHouse ? pHouse->ArrayIndex : -1, NeuralSection::MicroSetup };

	auto const FindDeployableFoundationNearCellUncached
		= [](HouseClass* pH, BuildingTypeClass* pBldType, const CellStruct& originCell, int requiredZone,
			UnitClass* pDeployerUnit) -> CellStruct
	{
		if (!pH || !pBldType || originCell == CellStruct::Empty)
			return CellStruct::Empty;

		const int w = pBldType->GetFoundationWidth();
		const int h = pBldType->GetFoundationHeight(false);
		const CellStruct deployOffset = (w > 2 || h > 2) ? CellStruct { -1, -1 } : CellStruct { 0, 0 };
		const CellStruct curUnitCell = pDeployerUnit ? pDeployerUnit->GetMapCoords() : CellStruct::Empty;
		for (int r = 0; r <= 28; ++r)
		{
			for (int dy = -r; dy <= r; ++dy)
			{
				for (int dx = -r; dx <= r; ++dx)
				{
					if (std::max(std::abs(dx), std::abs(dy)) != r)
						continue;

					const CellStruct cand { static_cast<short>(originCell.X + dx),
						static_cast<short>(originCell.Y + dy) };
					const CellStruct topLeftCell = cand + deployOffset;
					if (!MapClass::Instance.CoordinatesLegal(cand) || !MapClass::Instance.CoordinatesLegal(topLeftCell))
						continue;

					if (requiredZone >= 0)
					{
						const int cz = MapClass::Instance.GetMovementZoneType(cand, MovementZone::Normal, false);
						if (cz != requiredZone)
							continue;
					}

					if (pDeployerUnit && pDeployerUnit->Type && cand != curUnitCell)
					{
						auto const pStandCell = MapClass::Instance.TryGetCellAt(cand);
						if (!pStandCell
							|| !pStandCell->IsClearToMove(pDeployerUnit->Type->SpeedType, false, false, -1,
								pDeployerUnit->Type->MovementZone, -1, false))
						{
							continue;
						}
					}

					if (!NeuralAI::IsCellClearedOfExitCorridors(pH, pBldType, topLeftCell))
						continue;

					UnitExt::Deployer = pDeployerUnit;
					const bool canCreate = pBldType->CanCreateHere(topLeftCell, pH);
					bool canExist = canCreate;
					if (canExist)
					{
						for (auto pFoundCell = pBldType->GetFoundationData(true);
							*pFoundCell != CellStruct { 0x7FFF, 0x7FFF }; ++pFoundCell)
						{
							const CellStruct fc = topLeftCell + *pFoundCell;
							auto const pFC = MapClass::Instance.TryGetCellAt(fc);
							if (!pFC || !pFC->CanThisExistHere(pBldType->SpeedType, pBldType, pH))
							{
								canExist = false;
								break;
							}

							for (ObjectClass* pObj = pFC->FirstObject; pObj; pObj = pObj->NextObject)
							{
								if (pObj == pDeployerUnit)
									continue;

								if (auto const pOtherU = abstract_cast<UnitClass*>(pObj))
								{
									if (pOtherU->IsAlive && !pOtherU->InLimbo && pOtherU->Type
										&& (pOtherU->Type->DeploysInto != nullptr
											|| IsAmphibiousOrAirTransport(pOtherU->Type)))
									{
										canExist = false;
										break;
									}
								}
							}

							if (!canExist)
								break;
						}
					}

					UnitExt::Deployer = nullptr;
					if (canExist)
						return cand;
				}
			}
		}

		return CellStruct::Empty;
	};

	// Memoized deploy-site search: an MCV / deployer that has not moved reuses its last answer
	// for 150 frames (a "no site" answer too), instead of re-scanning up to 28 rings of cells
	// every AI tick. A cached site is re-validated cheaply before reuse. Deterministic: the
	// memo is filled by the same synced calls on every client.
	auto const FindDeployableFoundationNearCell
		= [&FindDeployableFoundationNearCellUncached](HouseClass* pH, BuildingTypeClass* pBldType, const CellStruct& originCell,
			int requiredZone, UnitClass* pDeployerUnit = nullptr) -> CellStruct
	{
		if (!pH || !pBldType || originCell == CellStruct::Empty)
			return CellStruct::Empty;

		struct DeploySiteMemo
		{
			int Frame;
			CellStruct Result;
		};

		static std::unordered_map<uint64_t, DeploySiteMemo> memo;
		static int memoMatchSerial = -1;

		if (memoMatchSerial != NeuralMatchSerial)
		{
			memoMatchSerial = NeuralMatchSerial;
			memo.clear();
		}

		uint64_t key = 1469598103934665603ULL;

		for (const uint64_t part : { static_cast<uint64_t>(pH->ArrayIndex), static_cast<uint64_t>(pBldType->ArrayIndex),
				 static_cast<uint64_t>(static_cast<uint16_t>(originCell.X)), static_cast<uint64_t>(static_cast<uint16_t>(originCell.Y)),
				 static_cast<uint64_t>(static_cast<uint32_t>(requiredZone)),
				 static_cast<uint64_t>(pDeployerUnit ? pDeployerUnit->UniqueID : 0u) })
		{
			key = (key ^ part) * 1099511628211ULL;
		}

		auto const it = memo.find(key);

		if (it != memo.end() && it->second.Frame <= Unsorted::CurrentFrame && Unsorted::CurrentFrame - it->second.Frame <= 150)
		{
			const CellStruct cached = it->second.Result;

			if (cached == CellStruct::Empty)
				return cached;

			const int w = pBldType->GetFoundationWidth();
			const int h = pBldType->GetFoundationHeight(false);
			const CellStruct deployOffset = (w > 2 || h > 2) ? CellStruct { -1, -1 } : CellStruct { 0, 0 };

			if (NeuralAI::IsCellClearedOfExitCorridors(pH, pBldType, cached + deployOffset))
				return cached;
		}

		const CellStruct result = FindDeployableFoundationNearCellUncached(pH, pBldType, originCell, requiredZone, pDeployerUnit);

		if (memo.size() > 1024)
		{
			std::erase_if(memo, [](const auto& entry)
				{ return entry.second.Frame > Unsorted::CurrentFrame || Unsorted::CurrentFrame - entry.second.Frame > 150; });
		}

		memo[key] = { Unsorted::CurrentFrame, result };

		return result;
	};

	auto const CanDeployerUnpackHere = [](HouseClass* pH, BuildingTypeClass* pBldType, UnitClass* pDeployerUnit) -> bool
	{
		if (!pH || !pBldType || !pDeployerUnit)
			return false;

		const int w = pBldType->GetFoundationWidth();
		const int h = pBldType->GetFoundationHeight(false);
		const CellStruct deployOffset = (w > 2 || h > 2) ? CellStruct { -1, -1 } : CellStruct { 0, 0 };
		const CellStruct topLeftCell = pDeployerUnit->GetMapCoords() + deployOffset;
		if (!NeuralAI::IsCellClearedOfExitCorridors(pH, pBldType, topLeftCell))
			return false;

		bool occupiedByOtherDeployerOrTrans = false;
		for (auto pFoundCell = pBldType->GetFoundationData(true); *pFoundCell != CellStruct { 0x7FFF, 0x7FFF };
			++pFoundCell)
		{
			const CellStruct fc = topLeftCell + *pFoundCell;
			auto const pFC = MapClass::Instance.TryGetCellAt(fc);
			if (!pFC)
				return false;

			std::vector<FootClass*> cellFoots;
			CollectCellFoots(pFC, cellFoots);

			for (auto const pFoot : cellFoots)
			{
				if (pFoot == pDeployerUnit)
					continue;

				if (pFoot->Owner != pH || !pFoot->IsAlive || pFoot->InLimbo)
					continue;

				auto const pOtherU = abstract_cast<UnitClass*>(pFoot);
				if (pOtherU && pOtherU->Type
					&& (pOtherU->Type->DeploysInto != nullptr || IsAmphibiousOrAirTransport(pOtherU->Type)))
				{
					occupiedByOtherDeployerOrTrans = true;
					if (pOtherU->Type->DeploysInto != nullptr)
					{
						if (pOtherU->UniqueID > pDeployerUnit->UniqueID
							&& pOtherU->CurrentMission != Mission::Move)
						{
							pOtherU->Scatter(CoordStruct::Empty, true, false);
						}
					}
					else if (!IsAmphibiousOrAirTransport(pOtherU->Type) && pOtherU->CurrentMission != Mission::Move)
					{
						pOtherU->Scatter(CoordStruct::Empty, true, false);
					}
				}
				else if (pFoot->CurrentMission != Mission::Move)
				{
					if (!pOtherU || !pOtherU->Type
						|| (!pOtherU->Type->Harvester && !pOtherU->Type->ResourceGatherer))
					{
						pFoot->Scatter(CoordStruct::Empty, true, false);
					}
				}
			}
		}

		if (occupiedByOtherDeployerOrTrans)
			return false;

		return UnitExt::CanDeployIntoBuilding(pDeployerUnit);
	};

	auto const OrderDeployerUnpack = [](UnitClass* pDeployerUnit) -> void
	{
		if (!pDeployerUnit)
			return;

		// UnitClass::TryToDeploy (0x7393DA) strictly aborts if this->Destination !=
		// nullptr!
		const bool hadDest = (pDeployerUnit->Destination != nullptr);
		pDeployerUnit->SetDestination(nullptr, true);
		pDeployerUnit->ClearNavigationList();
		pDeployerUnit->SetTarget(nullptr);
		if (pDeployerUnit->CurrentMission != Mission::Unload)
		{
			pDeployerUnit->QueueMission(Mission::Unload, true);
		}
		else if (hadDest && !pDeployerUnit->IsDeploying)
		{
			pDeployerUnit->MissionStatus = 0;
		}
	};

	auto const FindBestDeployStandCell
		= [&](HouseClass* pH, BuildingTypeClass* pBldType, UnitClass* pDeployerUnit, int requiredZone) -> CellStruct
	{
		if (!pH || !pBldType || !pDeployerUnit)
			return CellStruct::Empty;

		// A full placement search (FindSpatialTetrisBuildCell) costs 25-55 ms in a late game and
		// several deploy branches ask every tick: reuse a unit's answer for 120 frames (same match only:
		// NeuralMatchSerial, so a previous match's entry is never used).
		struct DeployStandCache
		{
			int MatchSerial;
			int Frame;
			int TypeIndex;
			int Zone;
			CellStruct Cell;
		};

		static std::unordered_map<DWORD, DeployStandCache> standCache;
		const int now = Unsorted::CurrentFrame;

		if (standCache.size() > 512)
			standCache.clear();

		if (auto const it = standCache.find(pDeployerUnit->UniqueID); it != standCache.end())
		{
			auto const& cached = it->second;

			if (cached.MatchSerial == NeuralMatchSerial && now >= cached.Frame && now - cached.Frame < 120 && cached.TypeIndex == pBldType->ArrayIndex
				&& cached.Zone == requiredZone)
			{
				return cached.Cell;
			}
		}

		auto remember = [&](const CellStruct& cell)
		{
			standCache[pDeployerUnit->UniqueID] = DeployStandCache { NeuralMatchSerial, now, pBldType->ArrayIndex, requiredZone, cell };
			return cell;
		};

		const CellStruct tetrisTopLeft = CachedTetrisBuildCell(pH, pBldType);
		if (tetrisTopLeft != CellStruct::Empty)
		{
			const CellStruct standOffset
				= (pBldType->GetFoundationWidth() > 2 || pBldType->GetFoundationHeight(false) > 2)
				? CellStruct { 1, 1 }
				: CellStruct { 0, 0 };
			const CellStruct nearTetris = FindDeployableFoundationNearCell(
				pH, pBldType, tetrisTopLeft + standOffset, requiredZone, pDeployerUnit);
			if (nearTetris != CellStruct::Empty)
				return remember(nearTetris);
		}

		return remember(FindDeployableFoundationNearCell(
			pH, pBldType, pDeployerUnit->GetMapCoords(), requiredZone, pDeployerUnit));
	};

	auto const pExt = HouseExt::Fetch(pHouse);
	CellStruct homeCoords = pHouse->BaseCenter != CellStruct::Empty ? pHouse->BaseCenter : pHouse->BaseSpawnCell;

	if (pHouse->ConYards.Count > 0 && pHouse->ConYards.GetItem(0))
		homeCoords = pHouse->ConYards.GetItem(0)->GetMapCoords();

	const int homeGroundZone = (homeCoords != CellStruct::Empty)
		? MapClass::Instance.GetMovementZoneType(homeCoords, MovementZone::Normal, false)
		: -1;

	// Universal Cross-Island Reachability & Transport Dispatcher:
	// Any ground-only unit or infantry whose action target is on a disconnected
	// landmass (footZone != targetZone) is automatically routed via an
	// Amphibious, Hover, Air, or Carryall Transport to the target island's
	// landing beach, and once unloaded on the target island (footZone ==
	// targetZone), executes its native mission.
	static struct PassengerIslandGoalEntry
	{
		DWORD UniqueID;
		CellStruct TargetCoords;
		int Frame;
	} s_PassengerIslandGoals[64] {};

	auto RecordPassengerIslandGoal = [](DWORD uid, const CellStruct& targetCoords)
	{
		int oldestIdx = 0;
		int oldestFrame = INT_MAX;
		for (int i = 0; i < 64; ++i)
		{
			if (s_PassengerIslandGoals[i].UniqueID == uid || s_PassengerIslandGoals[i].UniqueID == 0)
			{
				s_PassengerIslandGoals[i] = { uid, targetCoords, Unsorted::CurrentFrame };
				return;
			}

			if (s_PassengerIslandGoals[i].Frame < oldestFrame)
			{
				oldestFrame = s_PassengerIslandGoals[i].Frame;
				oldestIdx = i;
			}
		}

		s_PassengerIslandGoals[oldestIdx] = { uid, targetCoords, Unsorted::CurrentFrame };
	};

	auto LookupPassengerIslandGoal = [](DWORD uid) -> CellStruct
	{
		for (int i = 0; i < 64; ++i)
		{
			if (s_PassengerIslandGoals[i].UniqueID == uid
				&& (Unsorted::CurrentFrame - s_PassengerIslandGoals[i].Frame) <= 9000)
			{
				return s_PassengerIslandGoals[i].TargetCoords;
			}
		}

		return CellStruct::Empty;
	};

	auto ClearPassengerIslandGoal = [](DWORD uid)
	{
		for (int i = 0; i < 64; ++i)
		{
			if (s_PassengerIslandGoals[i].UniqueID == uid)
			{
				s_PassengerIslandGoals[i] = { 0, CellStruct::Empty, 0 };
				return;
			}
		}
	};

	auto countPendingBoardersOn = [&](TechnoClass* pT, FootClass* pExcludeFoot, int& outCount, int& outSize)
	{
		outCount = 0;
		outSize = 0;
		for (auto const pCheckU : UnitClass::Array)
		{
			if (!pCheckU || pCheckU->Owner != pHouse || pCheckU->InLimbo || !pCheckU->IsAlive || !pCheckU->Type
				|| pCheckU == pT || pCheckU == pExcludeFoot)
			{
				continue;
			}

			if ((pCheckU->CurrentMission == Mission::Enter || pCheckU->QueuedMission == Mission::Enter)
				&& (pCheckU->ContainsLink(pT) || pCheckU->ArchiveTarget == pT || pCheckU->Destination == pT))
			{
				outCount++;
				outSize += std::max(1, static_cast<int>(pCheckU->Type->TechnoTypeClass::Size));
			}
		}

		for (auto const pCheckI : InfantryClass::Array)
		{
			if (!pCheckI || pCheckI->Owner != pHouse || pCheckI->InLimbo || !pCheckI->IsAlive || !pCheckI->Type
				|| pCheckI == pExcludeFoot)
			{
				continue;
			}

			if ((pCheckI->CurrentMission == Mission::Enter || pCheckI->QueuedMission == Mission::Enter)
				&& (pCheckI->ContainsLink(pT) || pCheckI->ArchiveTarget == pT || pCheckI->Destination == pT))
			{
				outCount++;
				outSize += std::max(1, static_cast<int>(pCheckI->Type->TechnoTypeClass::Size));
			}
		}
	};

	auto CanFootCrossWaterDirectly = [](const FootClass* pF) -> bool
	{
		if (!pF)
			return false;

		auto const pT = pF->GetTechnoType();
		if (!pT)
			return false;

		return (pT->MovementZone == MovementZone::Amphibious || pT->MovementZone == MovementZone::AmphibiousDestroyer
			|| pT->MovementZone == MovementZone::AmphibiousCrusher || pT->MovementZone == MovementZone::Fly
			|| pT->MovementZone == MovementZone::Subterrannean || pT->MovementZone == MovementZone::Water
			|| pT->SpeedType == SpeedType::Hover || pT->SpeedType == SpeedType::Winged || pT->JumpJet || pT->Teleporter
			|| pT->Naval);
	};

	struct PendingCrossIslandOrder
	{
		FootClass* Passenger;
		int PickupZone;
		CellClass* TargetLandingCell;
		int TargetZone;
		int PassengerSize;
	};
	std::vector<PendingCrossIslandOrder> pendingCrossIslandOrders;

	auto UnstickCrossIslandFoot = [&](FootClass* pFoot) -> bool
	{
		if (!pFoot || CanFootCrossWaterDirectly(pFoot))
			return false;

		if (pFoot->CurrentMission == Mission::Guard || pFoot->CurrentMission == Mission::Area_Guard
			|| pFoot->CurrentMission == Mission::Harvest || pFoot->CurrentMission == Mission::Unload)
		{
			return false;
		}

		AbstractClass* pGoal = pFoot->Destination ? pFoot->Destination : pFoot->Target;
		if (!pGoal)
			return false;

		if (auto const pDestFoot = abstract_cast<FootClass*>(pFoot->Destination))
		{
			if (pDestFoot->Owner == pHouse && pDestFoot->GetTechnoType() && pDestFoot->GetTechnoType()->Passengers > 0)
			{
				const int fz
					= ObjectZone(pFoot, MovementZone::Normal);
				const int tz
					= ObjectZone(pDestFoot, MovementZone::Normal);
				if (fz == tz)
					return false;
			}
		}

		const int fz = ObjectZone(pFoot, MovementZone::Normal);
		const CellStruct gc = CellClass::Coord2Cell(pGoal->GetCoords());
		const int gz = MapClass::Instance.GetMovementZoneType(gc, MovementZone::Normal, false);
		if (fz >= 0 && gz >= 0 && fz != gz && !IsBuildingEnterableFromZone(abstract_cast<BuildingClass*>(pGoal), fz))
		{
			if (pFoot->Target && pFoot->CurrentMission == Mission::Attack && pFoot->GetTechnoType())
			{
				const auto pW
					= pFoot->GetTechnoType()->GetWeapon(0) ? pFoot->GetTechnoType()->GetWeapon(0)->WeaponType : nullptr;
				if (pW && pFoot->DistanceFrom(pFoot->Target) <= pW->Range)
					return false;
			}

			pFoot->SetDestination(nullptr, true);
			pFoot->SetTarget(nullptr);
			pFoot->QueueMission(Mission::Guard, true);
			return true;
		}

		return false;
	};

	auto DispatchFootToTargetOrTransport
		= [&](FootClass* pFoot, AbstractClass* pTarget, Mission desiredMission, bool useSetTarget = false) -> bool
	{
		if (!pFoot || !pTarget)
			return false;

		auto const pFootType = pFoot->GetTechnoType();
		if (!pFootType)
			return false;

		const CellStruct footCoords = pFoot->GetMapCoords();
		const int footZone = MapClass::Instance.GetMovementZoneType(footCoords, MovementZone::Normal, pFoot->OnBridge);
		const CellStruct targetCoords = CellClass::Coord2Cell(pTarget->GetCoords());
		auto const pTargetObject = abstract_cast<ObjectClass*>(pTarget);
		int targetZone = MapClass::Instance.GetMovementZoneType(
			targetCoords, MovementZone::Normal, pTargetObject && pTargetObject->OnBridge);

		// A building answers for the ground around it (its own cells may carry another zone).
		if (footZone >= 0 && targetZone != footZone && IsBuildingEnterableFromZone(abstract_cast<BuildingClass*>(pTarget), footZone))
			targetZone = footZone;

		bool canShootFromHere = false;
		if (useSetTarget || desiredMission == Mission::Attack)
		{
			const auto pWpn = pFootType->GetWeapon(0) ? pFootType->GetWeapon(0)->WeaponType : nullptr;
			if (pWpn && pFoot->DistanceFrom(pTarget) <= pWpn->Range)
				canShootFromHere = true;
		}

		if (footZone < 0 || targetZone < 0 || footZone == targetZone || CanFootCrossWaterDirectly(pFoot)
			|| canShootFromHere)
		{
			GetMacroState(pHouse).FerryWaitSince.erase(pFoot->UniqueID);

			if (useSetTarget || desiredMission == Mission::Attack)
			{
				pFoot->ClearMegaMissionData();
				pFoot->SetTarget(pTarget);
				pFoot->QueueMission(desiredMission, true);
			}
			else if (desiredMission == Mission::Enter && abstract_cast<TechnoClass*>(pTarget))
			{
				OrderFootToEnter(pFoot, abstract_cast<TechnoClass*>(pTarget));
			}
			else if (desiredMission == Mission::AttackMove)
			{
				OrderFootToAttackMove(pFoot, pTarget);
			}
			else
			{
				pFoot->ClearMegaMissionData();
				pFoot->SetDestination(pTarget, true);
				pFoot->QueueMission(desiredMission, true);
			}

			return true;
		}

		if (!CanHouseFerryTypeAcrossWater(pHouse, pFootType))
			return false;

		// Safety net: a unit that has asked for a crossing for 3600 frames without ever boarding
		// (no transport came, or it could not reach the pickup) is released to other jobs and may
		// not ask again for 9000 frames, instead of waiting on the beach for the rest of the match.
		{
			auto& ferryMacro = GetMacroState(pHouse);
			const int now = Unsorted::CurrentFrame;

			if (auto const itBlocked = ferryMacro.FerryBlockedUntil.find(pFoot->UniqueID);
				itBlocked != ferryMacro.FerryBlockedUntil.end() && now < itBlocked->second)
			{
				return false;
			}

			if (pFoot->CurrentMission == Mission::Enter || pFoot->QueuedMission == Mission::Enter)
			{
				ferryMacro.FerryWaitSince.erase(pFoot->UniqueID);
			}
			else
			{
				auto const [itWait, inserted] = ferryMacro.FerryWaitSince.try_emplace(pFoot->UniqueID, now);

				if (!inserted && now - itWait->second >= 3600)
				{
					ferryMacro.FerryWaitSince.erase(itWait);
					ferryMacro.FerryBlockedUntil[pFoot->UniqueID] = now + 9000;
					ClearPassengerIslandGoal(pFoot->UniqueID);
					pFoot->SetDestination(nullptr, true);
					pFoot->QueueMission(Mission::Guard, true);
					CountDecision(NeuralDecision::FerryWaitTimeout, pHouse->ArrayIndex);
					return false;
				}
			}
		}

		RecordPassengerIslandGoal(pFoot->UniqueID, targetCoords);
		const int passSize = std::max(1, static_cast<int>(pFootType->Size));
		if (CellClass* pTargetLanding = FindSafeIslandLandingCell(pHouse, targetCoords, targetZone, false))
			pendingCrossIslandOrders.push_back({ pFoot, footZone, pTargetLanding, targetZone, passSize });

		// Do NOT order multiple units into Mission::Enter here! The centralized
		// transport controller boards units strictly ONE BY ONE so transports never jam.
		if (pFoot->CurrentMission == Mission::Enter || pFoot->QueuedMission == Mission::Enter)
			return true;

		if (auto const pPickupBeach = FindSafeIslandLandingCell(pHouse, footCoords, footZone, false))
		{
			pFoot->SetTarget(nullptr);
			if (footCoords.DistanceFrom(pPickupBeach->MapCoords) > 9.0)
			{
				IssueMoveOrder(pFoot, pPickupBeach, 4.0);
			}
			else if (pFoot->CurrentMission != Mission::Guard)
			{
				pFoot->SetDestination(nullptr, true);
				pFoot->ClearNavigationList();
				pFoot->QueueMission(Mission::Guard, true);
			}

			return true;
		}

		return false;
	};

	// Find any idle Amphibious/Hover Transport owned by this house that still has
	// passenger room
	FootClass* pLoadingTransport = nullptr;
	int loadingTransMaxPass = 0;
	int loadingTransSizeLimit = 0;
	for (auto const pAirT : AircraftClass::Array)
	{
		if (!pAirT || pAirT->Owner != pHouse || pAirT->InLimbo || !pAirT->IsAlive || !pAirT->Type)
			continue;

		if (!pAirT->Type->Carryall && pAirT->Type->Passengers >= 3 && !pAirT->IsInAir() && pAirT->GetHeight() == 0
			&& pAirT->Passengers.NumPassengers < pAirT->Type->Passengers)
		{
			pLoadingTransport = pAirT;
			loadingTransMaxPass = pAirT->Type->Passengers;
			loadingTransSizeLimit = static_cast<int>(pAirT->Type->SizeLimit);
			break;
		}
	}

	for (auto const pU : UnitClass::Array)
	{
		if (!pU || pU->Owner != pHouse || pU->InLimbo || !pU->IsAlive || !pU->Type)
			continue;

		if (IsAmphibiousOrAirTransport(pU->Type) && pU->Passengers.NumPassengers < pU->Type->Passengers
			&& !IsTransportUnderRepair(pU)
			&& !(pU->Passengers.NumPassengers == 0 && pU->GetHealthPercentage() < 0.5))
		{
			if (!pLoadingTransport || static_cast<int>(pU->Type->SizeLimit) > loadingTransSizeLimit)
			{
				pLoadingTransport = pU;
				loadingTransMaxPass = pU->Type->Passengers;
				loadingTransSizeLimit = static_cast<int>(pU->Type->SizeLimit);
			}

			if (loadingTransSizeLimit >= 6)

				break;
		}
	}

	// Multi-Player Opportunism (Expert 3 Routing Slot 43): Balance focusing
	// high-threat aggressor vs finishing off the most vulnerable enemy house
	const int focusBias = static_cast<int>(scores[43]);

	// Discover uncolonized island Resource (Ore/Gems), buildable Island Land
	// (when home island is full), and Strategic Neutral Tech Building targets
	CellClass* pIslandResourceGoalCell = nullptr;
	int resourceIslandZone = -1;
	CellClass* pIslandTechGoalCell = nullptr;
	int techIslandZone = -1;
	int conYardsOnHomeIsland = 0;
	int refineriesOnHomeIsland = 0;
	bool hasUncolonizedOreOnHomeIsland = false;
	UnitClass* pExtraHomeSlaveMiner = nullptr;
	if (homeGroundZone >= 0 && homeCoords != CellStruct::Empty)
	{
		for (auto const pOwnB : pHouse->Buildings)
		{
			if (!pOwnB || pOwnB->InLimbo || !pOwnB->IsAlive || !pOwnB->Type || pOwnB->Type->Insignificant
				|| pOwnB->Type->InvisibleInGame)
			{
				continue;
			}

			const int bz = ObjectZone(pOwnB, MovementZone::Normal);
			if (bz == homeGroundZone)
			{
				if (pOwnB->Type->ConstructionYard)
					conYardsOnHomeIsland++;

				if (pOwnB->Type->Refinery || pOwnB->Type->ResourceGatherer || pOwnB->Type->ResourceDestination)
					refineriesOnHomeIsland++;
			}
		}

		for (auto const pOwnU : UnitClass::Array)
		{
			if (!pOwnU || pOwnU->Owner != pHouse || pOwnU->InLimbo || !pOwnU->IsAlive || !pOwnU->Type)
				continue;

			if (pOwnU->Type->ResourceGatherer && pOwnU->Type->DeploysInto != nullptr)
			{
				const int uz
					= ObjectZone(pOwnU, MovementZone::Normal);
				if (uz == homeGroundZone)
				{
					refineriesOnHomeIsland++;
					if (refineriesOnHomeIsland >= 3)
					{
						if (!pExtraHomeSlaveMiner || pOwnU->CurrentMission == Mission::Enter
							|| pOwnU->QueuedMission == Mission::Enter)
						{
							pExtraHomeSlaveMiner = pOwnU;
						}
					}
				}
			}
		}

		const bool homeIslandCongested = (pHouse->Buildings.Count >= 14 || pHouse->Available_Money() >= 3000
			|| GetMacroState(pHouse).ConsecutivePlacementFailures > 0);
		std::vector<int> enemyBaseZones;
		for (auto const pEB : BuildingClass::Array)
		{
			if (!pEB || pEB->InLimbo || !pEB->IsAlive || !pEB->Owner || !pEB->Type || pEB->Type->Insignificant
				|| pEB->Type->InvisibleInGame)
			{
				continue;
			}

			if (pEB->Owner == pHouse || pHouse->IsAlliedWith(pEB->Owner) || !IsEnemyBuildingKnown(pHouse, pEB) || pEB->Owner->IsNeutral())
				continue;

			if (pEB->Type->ConstructionYard || pEB->Type->Factory != AbstractType::None || pEB->Type->Refinery
				|| pEB->Type->IsBaseDefense)
			{
				const int ebz
					= ObjectZone(pEB, MovementZone::Normal);
				if (ebz >= 0 && std::find(enemyBaseZones.begin(), enemyBaseZones.end(), ebz) == enemyBaseZones.end())
					enemyBaseZones.push_back(ebz);
			}
		}

		int bestResScore = -999999;
		CellClass* pFallbackIslandLandCell = nullptr;
		int fallbackIslandLandZone = -1;
		int bestLandScore = -999999;
		for (int dy = -216; dy <= 216; dy += 4)
		{
			for (int dx = -216; dx <= 216; dx += 4)
			{
				const CellStruct cand { static_cast<short>(homeCoords.X + dx), static_cast<short>(homeCoords.Y + dy) };
				auto const pCell = MapClass::Instance.TryGetCellAt(cand);
				if (!pCell || pCell->LandType == LandType::Water || pCell->LandType == LandType::Rock)
					continue;

				const int cz = MapClass::Instance.GetMovementZoneType(cand, MovementZone::Normal, false);
				if (cz < 0)
					continue;

				const int tibVal = pCell->GetContainedTiberiumValue();
				if (cz == homeGroundZone)
				{
					if (tibVal > 0 && !hasUncolonizedOreOnHomeIsland)
					{
						bool coveredByHomeHub = false;
						for (auto const pBld : pHouse->Buildings)
						{
							if (!pBld || pBld->InLimbo || !pBld->IsAlive || !pBld->Type || pBld->Type->Insignificant
								|| pBld->Type->InvisibleInGame)
							{
								continue;
							}

							if (pBld->Type->Refinery || pBld->Type->ConstructionYard || pBld->Type->ResourceDestination)
							{
								const CellStruct bc = pBld->GetMapCoords();
								if (std::max(std::abs(cand.X - bc.X), std::abs(cand.Y - bc.Y)) < 14)
								{
									coveredByHomeHub = true;
									break;
								}
							}
						}

						if (!coveredByHomeHub)
							hasUncolonizedOreOnHomeIsland = true;
					}

					continue;
				}

				// Do not mistake an enemy-occupied base island for a peaceful neutral
				// resource island during archipelago colonization!
				if (std::find(enemyBaseZones.begin(), enemyBaseZones.end(), cz) != enemyBaseZones.end())
					continue;

				// An expansion deployer found no room for its building there lately.
				if (auto const& noRoom = GetMacroState(pHouse).NoRoomIslandUntil;
					noRoom.contains(cz) && noRoom.at(cz) > Unsorted::CurrentFrame)
				{
					continue;
				}

				bool alreadyColonized = false;
				bool hasOrphanedHubWithoutConYardOrWarFac = false;
				int conYardsInCz = 0;
				int warFacsInCz = 0;
				int hubsInCz = 0;
				int minStepStoneDist = std::abs(dx) + std::abs(dy);
				for (auto const pBld : pHouse->Buildings)
				{
					if (!pBld || pBld->InLimbo || !pBld->IsAlive || !pBld->Type || pBld->Type->Insignificant
						|| pBld->Type->InvisibleInGame)
					{
						continue;
					}

					if (pBld->Type->Refinery || pBld->Type->ConstructionYard || pBld->Type->ResourceDestination
						|| pBld->Type->BaseNormal || pBld->Type->Factory != AbstractType::None)
					{
						const CellStruct bc = pBld->GetMapCoords();
						const int bz = MapClass::Instance.GetMovementZoneType(bc, MovementZone::Normal, pBld->OnBridge);
						if (bz == cz)
						{
							hubsInCz++;
							if (pBld->Type->ConstructionYard)
								conYardsInCz++;

							if (pBld->Type->Factory == AbstractType::UnitType && !pBld->Type->Naval)
								warFacsInCz++;

							continue;
						}

						const int dHub = std::abs(cand.X - bc.X) + std::abs(cand.Y - bc.Y);
						if (dHub < minStepStoneDist)
							minStepStoneDist = dHub;
					}
				}

				if (conYardsInCz >= 1 || warFacsInCz >= 1)
					alreadyColonized = true;
				else if (hubsInCz >= 1)
					hasOrphanedHubWithoutConYardOrWarFac = true;

				if (!alreadyColonized)
				{
					for (auto const pOwnMcv : UnitClass::Array)
					{
						if (!pOwnMcv || pOwnMcv->Owner != pHouse || pOwnMcv->InLimbo || !pOwnMcv->IsAlive
							|| !pOwnMcv->Type || !pOwnMcv->Type->DeploysInto)
						{
							continue;
						}

						if (pOwnMcv->Type->DeploysInto->ConstructionYard || pOwnMcv->Type->DeploysInto->Refinery
							|| pOwnMcv->Type->DeploysInto->BaseNormal)
						{
							if (ObjectZone(pOwnMcv, MovementZone::Normal)
								== cz)
							{
								alreadyColonized = true;
								break;
							}
						}
					}
				}

				if (alreadyColonized)
					continue;

				if (tibVal > 0)
				{
					const int sc = std::min(tibVal, 600) - minStepStoneDist * 2
						- (hasOrphanedHubWithoutConYardOrWarFac ? 250 : 0);
					if (sc > bestResScore)
					{
						bestResScore = sc;
						resourceIslandZone = cz;
						pIslandResourceGoalCell = pCell;
					}
				}
				else if ((homeIslandCongested || !hasUncolonizedOreOnHomeIsland) && pCell->GetBuilding() == nullptr
					&& pCell->IsClearToMove(SpeedType::Track, true, true, -1, MovementZone::Normal, -1, false))
				{
					const int lsc = 300 - minStepStoneDist * 2;
					if (lsc > bestLandScore)
					{
						bestLandScore = lsc;
						fallbackIslandLandZone = cz;
						pFallbackIslandLandCell = pCell;
					}
				}
			}
		}

		if (!pIslandResourceGoalCell && pFallbackIslandLandCell)
		{
			pIslandResourceGoalCell = pFallbackIslandLandCell;
			resourceIslandZone = fallbackIslandLandZone;
		}

		if (pIslandResourceGoalCell)
		{
			if (auto const pSafe
				= FindSafeIslandLandingCell(pHouse, pIslandResourceGoalCell->MapCoords, resourceIslandZone, true))
			{
				pIslandResourceGoalCell = pSafe;
			}
		}

		// Orphaned Island Refinery Recovery: If any island has an owned Refinery
		// with 0 Harvesters in its movement zone, either switch that island's War
		// Factory to PrimaryFactory (if one exists on that island) OR ferry a spare
		// Harvester from another island!
		if (IsAITickWindow(90, 20))
		{
			for (auto const pRefB : pHouse->Buildings)
			{
				if (!pRefB || pRefB->InLimbo || !pRefB->IsAlive || !pRefB->Type)
					continue;

				if (!pRefB->Type->Refinery && !pRefB->Type->ResourceDestination)
					continue;

				const int rz
					= ObjectZone(pRefB, MovementZone::Normal);
				if (rz < 0)
					continue;

				int harvsInRz = 0;
				for (auto const pHU : UnitClass::Array)
				{
					if (!pHU || pHU->Owner != pHouse || pHU->InLimbo || !pHU->IsAlive || !pHU->Type)
						continue;

					if ((pHU->Type->Harvester || pHU->Type->ResourceGatherer)
						&& ObjectZone(pHU, MovementZone::Normal)
							== rz)
					{
						harvsInRz++;
					}
				}

				if (harvsInRz == 0)
				{
					BuildingClass* pWarFacInRz = nullptr;
					for (auto const pWB : pHouse->Buildings)
					{
						if (pWB && !pWB->InLimbo && pWB->IsAlive && pWB->Type
							&& pWB->Type->Factory == AbstractType::UnitType && !pWB->Type->Naval
							&& ObjectZone(pWB, MovementZone::Normal)
								== rz)
						{
							pWarFacInRz = pWB;
							break;
						}
					}

					if (pWarFacInRz)
					{
						if (!pWarFacInRz->IsPrimaryFactory)
							pWarFacInRz->TogglePrimaryFactory();
					}
					else if (auto const pRefCell = pRefB->GetCell())
					{
						for (auto const pSpareH : UnitClass::Array)
						{
							if (!pSpareH || pSpareH->Owner != pHouse || pSpareH->InLimbo || !pSpareH->IsAlive
								|| !pSpareH->Type || !pSpareH->Type->Harvester)
							{
								continue;
							}

							const int uz = ObjectZone(pSpareH, MovementZone::Normal);
							if (uz < 0 || uz == rz)
								continue;

							int harvsInUz = 0;
							for (auto const pOtherH : UnitClass::Array)
							{
								if (pOtherH && pOtherH->Owner == pHouse && !pOtherH->InLimbo && pOtherH->IsAlive
									&& pOtherH->Type && pOtherH->Type->Harvester
									&& ObjectZone(pOtherH, MovementZone::Normal)
										== uz)
								{
									harvsInUz++;
								}
							}

							if (harvsInUz >= 2 && CanHouseFerryTypeAcrossWater(pHouse, pSpareH->Type))
							{
								DispatchFootToTargetOrTransport(pSpareH, pRefCell, Mission::Move, false);
								break;
							}
						}
					}

					break;
				}
			}
		}

		// Enemy Island Offensive Beachhead & Reconquest Discovery: Once all neutral
		// islands in the archipelago are colonized (!pIslandResourceGoalCell),
		// allow MCVs & Forward Deployers (Outposts, Mobile War Factories/Barracks,
		// Nanocores) to reconquer the weakest/least-defended enemy island! With an
		// offensive bank (HasOffensiveBank) the enemy island comes before the free ones.
		if (!pIslandResourceGoalCell || HasOffensiveBank(pHouse))
		{
			int bestEnemyReconqScore = -999999;
			for (auto const pEB : BuildingClass::Array)
			{
				if (!pEB || pEB->InLimbo || !pEB->IsAlive || !pEB->Owner || !pEB->Type || pEB->Type->Insignificant
					|| pEB->Type->InvisibleInGame || pEB->Type->Naval)
				{
					continue;
				}

				if (pEB->Owner == pHouse || pHouse->IsAlliedWith(pEB->Owner) || !IsEnemyBuildingKnown(pHouse, pEB) || pEB->Owner->IsNeutral())
					continue;

				const CellStruct ebc = pEB->GetMapCoords();
				const int ebz = MapClass::Instance.GetMovementZoneType(ebc, MovementZone::Normal, pEB->OnBridge);
				if (ebz < 0 || ebz == homeGroundZone)
					continue;

				bool weHaveBeachheadThere = false;
				for (auto const pOwnB : pHouse->Buildings)
				{
					if (!pOwnB || pOwnB->InLimbo || !pOwnB->IsAlive || !pOwnB->Type)
						continue;

					if (pOwnB->Type->ConstructionYard || pOwnB->Type->BaseNormal
						|| (pOwnB->Type->Factory != AbstractType::None && !pOwnB->Type->Naval))
					{
						if (ObjectZone(pOwnB, MovementZone::Normal)
							== ebz)
						{
							weHaveBeachheadThere = true;
							break;
						}
					}
				}

				if (!weHaveBeachheadThere)
				{
					int enemyTurretsInZone = 0;
					int enemyBldsInZone = 0;
					for (auto const pCheckEB : BuildingClass::Array)
					{
						if (!pCheckEB || pCheckEB->InLimbo || !pCheckEB->IsAlive || !pCheckEB->Owner || !pCheckEB->Type
							|| pCheckEB->Type->Insignificant)
						{
							continue;
						}

						if (pCheckEB->Owner == pHouse || pHouse->IsAlliedWith(pCheckEB->Owner) || !IsEnemyBuildingKnown(pHouse, pCheckEB)
							|| pCheckEB->Owner->IsNeutral())
						{
							continue;
						}

						if (ObjectZone(pCheckEB, MovementZone::Normal)
							== ebz)
						{
							enemyBldsInZone++;
							if (pCheckEB->Type->IsBaseDefense)
								enemyTurretsInZone++;
						}
					}

					const int reconqScore = 1000 - (enemyTurretsInZone * 180) - (enemyBldsInZone * 30);
					if (reconqScore > bestEnemyReconqScore)
					{
						if (auto const pEnemyFlankBeach = FindSafeIslandLandingCell(pHouse, ebc, ebz, false))
						{
							bestEnemyReconqScore = reconqScore;
							pIslandResourceGoalCell = pEnemyFlankBeach;
							resourceIslandZone = ebz;
						}
					}
				}
			}
		}

		int bestTechDist = 999999;
		for (auto const pCivBld : BuildingClass::Array)
		{
			if (!pCivBld || pCivBld->InLimbo || !pCivBld->IsAlive || !pCivBld->Type || !pCivBld->Type->Capturable)
				continue;

			if (!pCivBld->Owner || (!pCivBld->Owner->IsNeutral() && pHouse->IsAlliedWith(pCivBld->Owner)))
				continue;

			// Fog-fair: a tech we saw as neutral, or an enemy building we have seen.
			if (pCivBld->Owner->IsNeutral() ? !IsKnownNeutralTech(pHouse, pCivBld) : !IsEnemyBuildingKnown(pHouse, pCivBld))
				continue;

			const CellStruct bc = pCivBld->GetMapCoords();
			const int bz = MapClass::Instance.GetMovementZoneType(bc, MovementZone::Normal, pCivBld->OnBridge);
			if (bz < 0 || bz == homeGroundZone)
				continue;

			bool engAlreadyOnIsland = false;
			for (auto const pOwnInf : InfantryClass::Array)
			{
				if (pOwnInf && pOwnInf->Owner == pHouse && !pOwnInf->InLimbo && pOwnInf->IsAlive && pOwnInf->Type
					&& pOwnInf->Type->Engineer)
				{
					if (ObjectZone(pOwnInf, MovementZone::Normal)
						== bz)
					{
						engAlreadyOnIsland = true;
						break;
					}
				}
			}

			if (engAlreadyOnIsland)
				continue;

			const int rawDist = std::abs(bc.X - homeCoords.X) + std::abs(bc.Y - homeCoords.Y);
			const bool isCashDerrick = (pCivBld->Type->ProduceCashAmount > 0 || pCivBld->Type->ProduceCashStartup > 0);
			const int d = isCashDerrick ? (rawDist / 2) : rawDist;
			if (d < bestTechDist)
			{
				bestTechDist = d;
				techIslandZone = bz;
				pIslandTechGoalCell = FindSafeIslandLandingCell(pHouse, bc, bz, false);
			}
		}
	}

	int bestEnemyIdx = -1;
	int bestEnemyPriority = -999999;

	for (auto const pOther : HouseClass::Array)
	{
		if (!pOther || pOther == pHouse || pHouse->IsAlliedWith(pOther) || pOther->IsNeutral() || pOther->IsObserver()
			|| pOther->Defeated)
		{
			continue;
		}

		const int idx = std::clamp(pOther->ArrayIndex, 0, 7);
		int priority = pExt->NeuralAI_EstimatedEnemyThreat[idx] * GetMacroState(pHouse).FfaThreatScale[idx] / 100;

		if (pHouse->WhoLastHurtMe == pOther->ArrayIndex)
			priority += 250;

		// Finish a weak enemy: judged by the structures of it we know of (its real building count
		// would see through the fog).
		if (focusBias > 40)
		{
			int knownStructures = 0;

			for (auto const pEnemyBld : pOther->Buildings)
			{
				if (pEnemyBld && pEnemyBld->IsAlive && !pEnemyBld->InLimbo && IsEnemyBuildingKnown(pHouse, pEnemyBld)
					&& ++knownStructures > 4)
				{
					break;
				}
			}

			if (knownStructures > 0 && knownStructures <= 4)
				priority += std::clamp(focusBias / 2, 40, 260);
		}

		// Expert 3 (UpdateFfaTargets): busy, at our door, weakest, nearest.
		priority += GetMacroState(pHouse).FfaEnemyBonus[idx];

		// Expert 3 diplomacy (NeuralAI.Diplomacy.inl): the coalition target, never a truce partner, an
		// ex-partner not for a while, two houses at war left to it, the dying house first.
		priority += DiplomacyTargetBias(pHouse, pOther->ArrayIndex, false);

		// Team blackboard: the enemy this house covers for its team (its front).
		priority += TeamFrontBonus(pHouse, pOther->ArrayIndex);

		if (priority > bestEnemyPriority)
		{
			bestEnemyPriority = priority;

			bestEnemyIdx = pOther->ArrayIndex;
		}
	}

	if (bestEnemyIdx >= 0)
		pHouse->EnemyHouseIndex = bestEnemyIdx;

	microClock.Mark(NeuralSection::MicroSensors);

	// Tactical Squad Micro: Retreat damaged expensive/Elite/Parasited vehicles to
	// Service Depot, Chrono-Miner Warp escape, Beacons & Allied Base Rescue!
	BuildingClass* pRepairDepot
		= (pHouse->UnitRepairStations.Count > 0 && !GetMacroState(pHouse).BrokeHoldActive) ? pHouse->UnitRepairStations.GetItem(0) : nullptr;

	BuildingClass* pGrinder = pHouse->Grinders.Count > 0 ? pHouse->Grinders.GetItem(0) : nullptr;

	CellClass* pBeaconCell = (pExt->NeuralAI_AttackTargetCell != CellStruct::Empty)
		? MapClass::Instance.TryGetCellAt(pExt->NeuralAI_AttackTargetCell)
		: nullptr;

	// If no manual Beacon is active, check if an ALLIED player's base is under
	// attack (< 85% HP) and rush to defend our Ally!
	if (!pBeaconCell)
	{
		for (auto const pBld : BuildingClass::Array)
		{
			if (!pBld || pBld->InLimbo || !pBld->IsAlive || !pBld->Owner || !pBld->Type)
				continue;

			if (pBld->Owner != pHouse && pHouse->IsAlliedWith(pBld->Owner) && !pBld->Owner->IsNeutral())
			{
				if (pBld->HasBeenAttacked && pBld->GetHealthPercentage() < 0.85)
				{
					pBeaconCell = pBld->GetCell();

					break;
				}
			}
		}
	}

	// Real Tactical Sensors for Slots 56..60:
	// - Slot 56: Own Harvester under attack & Visible Enemy Harvester to raid
	// - Slot 57: Visible Enemy Garrisoned Building / Base Defense & Urban
	// Strongholds to clear/repair
	// - Slot 58: Enemy MindController holding a unit & Imminent Enemy Superweapon
	// - Slot 59: Visible Map Crate to collect
	// - Slot 60: Leading Heavy/Siege Tank for AA & Sensor Escort
	UnitClass* pAttackedOwnHarvester = nullptr;
	UnitClass* attackedOwnHarvesters[6] {};
	int attackedOwnHarvCount = 0;
	UnitClass* pVisibleEnemyHarvester = nullptr;
	UnitClass* visibleEnemyHarvesters[6] {};
	int visibleEnemyHarvCount = 0;
	UnitClass* pEnemySlaveMiner = nullptr;
	UnitClass* pLeadHeavyTank = nullptr;
	UnitClass* pLeadWarship = nullptr;
	UnitClass* pParasitedOwnUnit = nullptr;
	UnitClass* pBestParasiteVictim = nullptr;
	UnitClass* pEnemyChaosTarget = nullptr;
	InfantryClass* pSampleEnemyInfantry = nullptr;
	InfantryClass* pEnemySpecialistInf = nullptr;
	TechnoClass* pDetectedEnemyInfiltrator = nullptr;
	TechnoClass* detectedEnemyInfiltrators[8] {};
	int detectedInfiltratorCount = 0;
	TechnoClass* pHiddenOrStealthEnemy = nullptr;
	UnitClass* pEnemySubOrShip = nullptr;

	const bool combatStalledForHunt
		= (Unsorted::CurrentFrame > 5400 && (Unsorted::CurrentFrame - NeuralAI::LastCombatFrame) >= 3600);

	TechnoClass* pEnemyMindController = nullptr;
	TechnoClass* pEnemyEMPedTarget = nullptr;
	TechnoClass* pEnemyNonEMPedHeavy = nullptr;
	TechnoClass* pEnemyTemporalAttacker = nullptr;
	TechnoClass* pEnemyDrainer = nullptr;
	TechnoClass* pEnemyArtilleryUnit = nullptr;
	UnitClass* pWoundedEnemyVehicle = nullptr;
	BuildingClass* pSurgicalEnemyBld = nullptr;
	BuildingClass* surgicalEnemyBlds[8] {};
	int surgicalEnemyBldCount = 0;
	BuildingClass* pPrioritySiegeBld = nullptr;
	BuildingClass* pEnemyGarrisonToClear = nullptr;
	BuildingClass* enemyGarrisonsToClear[6] {};
	int enemyGarrisonCount = 0;
	BuildingClass* pDamagedUrbanToRepair = nullptr;
	BuildingClass* pGrinderBld = nullptr;
	BuildingClass* pEmptyTankBunker = nullptr;
	BuildingClass* pUnitReloadBld = nullptr;
	BuildingClass* pCapturableEnemyConYard = nullptr;
	CellClass* pVisibleCrateCell = nullptr;
	bool enemySuperImminent = false;
	bool ownEngineerNearEnemyConYard = false;

	for (auto const pOtherH : HouseClass::Array)
	{
		if (!pOtherH || pOtherH == pHouse || pHouse->IsAlliedWith(pOtherH) || pOtherH->IsNeutral()
			|| pOtherH->IsObserver() || pOtherH->Defeated)
		{
			continue;
		}

		for (auto const pSup : pOtherH->Supers)
		{
			if (!pSup || !pSup->Type || !pSup->Type->DisableableFromShell)
				continue;

			if (pSup->IsReady || (pSup->RechargeTimer.GetTimeLeft() > 0 && pSup->RechargeTimer.GetTimeLeft() <= 600))
			{
				enemySuperImminent = true;

				break;
			}
		}

		if (enemySuperImminent)
			break;
	}

	for (auto const pCheckU : UnitClass::Array)
	{
		if (!pCheckU || pCheckU->InLimbo || !pCheckU->IsAlive || !pCheckU->Owner || !pCheckU->Type)
			continue;

		if (pCheckU->MindControlledBy && pCheckU->MindControlledBy->Owner && pCheckU->MindControlledBy->Owner != pHouse
			&& !pHouse->IsAlliedWith(pCheckU->MindControlledBy->Owner) && pCheckU->MindControlledBy->GetCell()
			&& IsCellVisibleToAI(pHouse, pCheckU->MindControlledBy->GetCell()))
		{
			pEnemyMindController = pCheckU->MindControlledBy;
		}

		if (pCheckU->Owner == pHouse)
		{
			if (pCheckU->ParasiteEatingMe != nullptr && !pParasitedOwnUnit)
				pParasitedOwnUnit = pCheckU;

			if (pCheckU->TemporalTargetingMe && pCheckU->TemporalTargetingMe->Owner && !pEnemyTemporalAttacker)
				pEnemyTemporalAttacker = pCheckU->TemporalTargetingMe->Owner;

			if (pCheckU->Type->Naval && pCheckU->Type->Cost >= 1000 && !pLeadWarship)
				pLeadWarship = pCheckU;

			if ((pCheckU->Type->Harvester || pCheckU->Type->ResourceGatherer)
				&& (pCheckU->HasBeenAttacked || pCheckU->GetHealthPercentage() < 0.90))
			{
				if (!pAttackedOwnHarvester)
					pAttackedOwnHarvester = pCheckU;

				if (attackedOwnHarvCount < 6)
					attackedOwnHarvesters[attackedOwnHarvCount++] = pCheckU;
			}
			else if (!pLeadHeavyTank && !pCheckU->Type->Naval && !pCheckU->Type->Harvester
				&& !pCheckU->Type->ResourceGatherer)
			{
				const auto pCWS = pCheckU->Type->GetWeapon(0);
				const auto pCW = pCWS ? pCWS->WeaponType : nullptr;

				if (pCheckU->Type->Cost >= 1000 || (pCW && (pCW->Range >= (8 * 256) || pCW->Spawner)))
					pLeadHeavyTank = pCheckU;
			}
		}
		else if (!pHouse->IsAlliedWith(pCheckU->Owner) && !pCheckU->Owner->IsNeutral())
		{
			if (pCheckU->GetCell() && IsCellVisibleToAI(pHouse, pCheckU->GetCell()))
			{
				if (pCheckU->Type->Harvester || pCheckU->Type->ResourceGatherer)
				{
					if (!pVisibleEnemyHarvester)
						pVisibleEnemyHarvester = pCheckU;

					if (visibleEnemyHarvCount < 6)
						visibleEnemyHarvesters[visibleEnemyHarvCount++] = pCheckU;
				}

				if (!pEnemySlaveMiner && pCheckU->Type->Enslaves != nullptr)
					pEnemySlaveMiner = pCheckU;

				if (!pBestParasiteVictim && pCheckU->Type->Parasiteable && !pCheckU->Type->Organic
					&& pCheckU->ParasiteEatingMe == nullptr && pCheckU->Type->Cost >= 700)
				{
					pBestParasiteVictim = pCheckU;
				}

				if (pCheckU->IsUnderEMP() && !pEnemyEMPedTarget)
				{
					pEnemyEMPedTarget = pCheckU;
				}
				else if (!pCheckU->IsUnderEMP() && pCheckU->Type->Cost >= 800 && !pCheckU->Type->Organic
					&& !pEnemyNonEMPedHeavy)
				{
					pEnemyNonEMPedHeavy = pCheckU;
				}

				if (!pEnemyChaosTarget && !pCheckU->Berzerk && !pCheckU->Type->ImmuneToPsionics
					&& pCheckU->Type->Cost >= 600)
				{
					pEnemyChaosTarget = pCheckU;
				}

				if (!pWoundedEnemyVehicle && pCheckU->GetHealthPercentage() < 0.55 && pCheckU->Type->Cost >= 500)
					pWoundedEnemyVehicle = pCheckU;

				const auto pEWS = pCheckU->Type->GetWeapon(0);
				const auto pEW = pEWS ? pEWS->WeaponType : nullptr;

				if (!pEnemyArtilleryUnit && pEW && (pEW->Range >= (8 * 256) || pEW->Spawner) && !pCheckU->Type->Naval)
					pEnemyArtilleryUnit = pCheckU;

				if (pCheckU->Disguised && homeCoords != CellStruct::Empty
					&& pCheckU->GetMapCoords().DistanceFrom(homeCoords) <= 22.0)
				{
					if (!pDetectedEnemyInfiltrator)
						pDetectedEnemyInfiltrator = pCheckU;

					if (detectedInfiltratorCount < 8)
						detectedEnemyInfiltrators[detectedInfiltratorCount++] = pCheckU;
				}

				if (pCheckU->Type->Naval
					&& (!pEnemySubOrShip || pCheckU->Type->Cloakable || pCheckU->CloakState != CloakState::Uncloaked))
				{
					pEnemySubOrShip = pCheckU;
				}

				if (pCheckU->Type->Naval && pCheckU->Type->Cloakable && homeCoords != CellStruct::Empty
					&& pCheckU->GetMapCoords().DistanceFrom(homeCoords) <= 30.0)
				{
					GetMacroState(pHouse).SubThreatUntil = Unsorted::CurrentFrame + 1800;
				}

				if (!pCheckU->Type->Naval && !pHiddenOrStealthEnemy
					&& (pCheckU->CloakState != CloakState::Uncloaked || pCheckU->Disguised || pCheckU->Type->Cloakable
						|| pCheckU->Type->CanDisguise || pCheckU->Type->PermaDisguise))
				{
					pHiddenOrStealthEnemy = pCheckU;
				}
			}
			else if (combatStalledForHunt || pCheckU->Owner->OwnedBuildings == 0)
			{
				if (pCheckU->Type->Naval
					&& (!pEnemySubOrShip || pCheckU->Type->Cloakable || pCheckU->CloakState != CloakState::Uncloaked))
				{
					pEnemySubOrShip = pCheckU;
				}
				else if (!pCheckU->Type->Naval && !pHiddenOrStealthEnemy
					&& (pCheckU->CloakState != CloakState::Uncloaked || pCheckU->Disguised || pCheckU->Type->Cloakable
						|| pCheckU->Type->CanDisguise || pCheckU->Type->PermaDisguise
						|| pCheckU->Owner->OwnedBuildings == 0))
				{
					pHiddenOrStealthEnemy = pCheckU;
				}
			}
		}
	}

	for (auto const pCheckInf : InfantryClass::Array)
	{
		if (!pCheckInf || pCheckInf->InLimbo || !pCheckInf->IsAlive || !pCheckInf->Owner || !pCheckInf->Type)
			continue;

		if (pCheckInf->Owner == pHouse || pHouse->IsAlliedWith(pCheckInf->Owner) || pCheckInf->Owner->IsNeutral())
			continue;

		if (!pCheckInf->GetCell())
			continue;

		if (!IsCellVisibleToAI(pHouse, pCheckInf->GetCell()))
		{
			if ((combatStalledForHunt || pCheckInf->Owner->OwnedBuildings == 0) && !pHiddenOrStealthEnemy
				&& (pCheckInf->CloakState != CloakState::Uncloaked || pCheckInf->Disguised || pCheckInf->Type->Cloakable
					|| pCheckInf->Type->CanDisguise || pCheckInf->Type->PermaDisguise
					|| pCheckInf->Owner->OwnedBuildings == 0))
			{
				pHiddenOrStealthEnemy = pCheckInf;
			}

			continue;
		}

		if (!pSampleEnemyInfantry && !pCheckInf->Type->Doggie && !pCheckInf->Type->Slaved)
			pSampleEnemyInfantry = pCheckInf;

		if (!pEnemySpecialistInf
			&& (pCheckInf->Type->Engineer || IsSpyInfantryType(pCheckInf->Type) || pCheckInf->Type->C4 || pCheckInf->Type->Ivan
				|| pCheckInf->Type->Deployer || pCheckInf->Type->VehicleThief || pCheckInf->Type->Thief))
		{
			pEnemySpecialistInf = pCheckInf;
		}

		if (homeCoords != CellStruct::Empty
			&& (pCheckInf->Disguised || IsSpyInfantryType(pCheckInf->Type) || pCheckInf->Type->Engineer
				|| pCheckInf->Type->Thief || pCheckInf->Type->VehicleThief || pCheckInf->Type->C4
				|| pCheckInf->Type->Ivan)
			&& pCheckInf->GetMapCoords().DistanceFrom(homeCoords) <= 22.0)
		{
			if (!pDetectedEnemyInfiltrator)
				pDetectedEnemyInfiltrator = pCheckInf;

			if (detectedInfiltratorCount < 8)
				detectedEnemyInfiltrators[detectedInfiltratorCount++] = pCheckInf;
		}
	}

	struct IncomingAirFlank
	{
		FootClass* Target { nullptr };
		int AssignedInterceptors { 0 };
		int NeededInterceptors { 1 };
		int ThreatValue { 0 };
		CellStruct InterceptCell { CellStruct::Empty };
	};

	IncomingAirFlank baseAirFlanks[8] {};
	int baseAirFlankCount = 0;

	if (homeCoords != CellStruct::Empty)
	{
		for (auto const pTech : TechnoClass::Array)
		{
			if (!pTech || pTech->InLimbo || !pTech->IsAlive || !pTech->Owner)
				continue;

			if (pTech->Owner == pHouse || pHouse->IsAlliedWith(pTech->Owner) || pTech->Owner->IsNeutral())
				continue;

			auto const pAirFoot = abstract_cast<FootClass*>(pTech);

			if (!pAirFoot)
				continue;

			const bool isAirborneThreat = (pAirFoot->WhatAmI() == AbstractType::Aircraft || pAirFoot->IsInAir()
				|| pAirFoot->GetHeight() > 20);

			if (!isAirborneThreat || !pAirFoot->GetCell() || !IsCellVisibleToAI(pHouse, pAirFoot->GetCell()))
				continue;

			const CellStruct ac = pAirFoot->GetMapCoords();
			bool threatensBase = (ac.DistanceFrom(homeCoords) <= 22.0);

			// Early warning: a remembered track whose heading reaches our base soon
			// (slow Kirovs are detected well before they arrive).
			const NeuralAirThreatTrack* pTrack = nullptr;

			for (auto const& track : GetMacroState(pHouse).AirThreatTracks)
			{
				if (track.UniqueID == pAirFoot->UniqueID)
				{
					pTrack = &track;
					break;
				}
			}

			const int arrivalFrames = pTrack ? EstimateAirThreatArrival(pHouse, *pTrack, 15.0, 900) : -1;

			if (arrivalFrames >= 0)
				threatensBase = true;

			if (!threatensBase)
			{
				for (auto const pOwnB : pHouse->Buildings)
				{
					if (pOwnB && !pOwnB->InLimbo && pOwnB->IsAlive
						&& ac.DistanceFrom(pOwnB->GetMapCoords()) <= 15.0)
					{
						threatensBase = true;
						break;
					}
				}
			}

			if (threatensBase && baseAirFlankCount < 8)
			{
				int interceptors = 0;

				for (auto const pOwnU : UnitClass::Array)
				{
					if (pOwnU && pOwnU->Owner == pHouse && !pOwnU->InLimbo && pOwnU->IsAlive
						&& pOwnU->Target == pAirFoot)
					{
						interceptors++;
					}
				}

				for (auto const pOwnI : InfantryClass::Array)
				{
					if (pOwnI && pOwnI->Owner == pHouse && !pOwnI->InLimbo && pOwnI->IsAlive
						&& pOwnI->Target == pAirFoot)
					{
						interceptors++;
					}
				}

				const int threatValue = pTrack ? pTrack->ThreatValue : GetCombatValueOfType(pAirFoot->GetTechnoType(), false);
				const CellStruct interceptCell = (pTrack && arrivalFrames > 0)
					? PredictAirThreatPosition(*pTrack, std::min(arrivalFrames, 300))
					: ac;

				// Heavy threats (Kirov ~200) draw several interceptors at once.
				baseAirFlanks[baseAirFlankCount++]
					= { pAirFoot, interceptors, std::clamp(threatValue / 45, 1, 6), threatValue, interceptCell };
			}
		}
	}

	// Picks the incoming air threat a given anti-air defender should engage. Idle
	// defenders take any threat in reach; busy ones only drop a ground target for
	// a heavy threat (Kirov-class) that is still short of interceptors.
	auto pickAirFlankFor = [&](FootClass* pDefender, bool defenderIdle, int maxDistLeptons) -> int
	{
		if (!pDefender)
			return -1;

		auto const pCurTarget = abstract_cast<TechnoClass*>(pDefender->Target);

		if (!defenderIdle && pCurTarget && IsAirborneThreatTechno(pCurTarget))
			return -1;

		const CellStruct defenderCoords = pDefender->GetMapCoords();
		int bestIdx = -1;
		int bestCost = INT_MAX;

		for (int ai = 0; ai < baseAirFlankCount; ++ai)
		{
			auto const& flank = baseAirFlanks[ai];

			if (!flank.Target || flank.Target->InLimbo || !flank.Target->IsAlive)
				continue;

			if (!defenderIdle && (flank.ThreatValue < 120 || flank.AssignedInterceptors >= flank.NeededInterceptors))
				continue;

			const int distNow = pDefender->DistanceFrom(flank.Target);
			const int distIntercept = (flank.InterceptCell != CellStruct::Empty)
				? static_cast<int>(defenderCoords.DistanceFrom(flank.InterceptCell) * Unsorted::LeptonsPerCell)
				: distNow;
			const int dist = std::min(distNow, distIntercept);

			if (dist > maxDistLeptons)
				continue;

			const int saturation = flank.AssignedInterceptors * 3072 / std::max(1, flank.NeededInterceptors);
			const int cost = dist + saturation - flank.ThreatValue * 6;

			if (cost < bestCost)
			{
				bestCost = cost;
				bestIdx = ai;
			}
		}

		return bestIdx;
	};

	// Opportunistic local engagements: every visible enemy (fog-fair) with its value,
	// its own firepower and whether it flies, gathered once per AI tick.
	struct LocalCombatContact
	{
		TechnoClass* Techno;
		CellStruct Coords;
		int Zone;
		int Value;
		int Firepower;
		bool Airborne;
		int LocalEnemyFirepower;
		int GroundRange;
		int Speed;
	};

	std::vector<LocalCombatContact> enemyContacts;
	std::vector<LocalCombatContact> ownCombatContacts;

	for (auto const pTech : TechnoClass::Array)
	{
		if (!pTech || pTech->InLimbo || !pTech->IsAlive || !pTech->Owner || !pTech->GetTechnoType())
			continue;

		auto const pTechType = pTech->GetTechnoType();
		const bool isOwn = pTech->Owner == pHouse;

		if (!isOwn && (pHouse->IsAlliedWith(pTech->Owner) || pTech->Owner->IsNeutral()))
			continue;

		if (auto const pTechBld = abstract_cast<BuildingClass*>(pTech))
		{
			if (pTechBld->Type->Insignificant || pTechBld->Type->InvisibleInGame || pTechBld->Type->Immune)
				continue;
		}

		const CellStruct coords = pTech->GetMapCoords();
		auto const pTechWS0 = pTechType->GetWeapon(0);
		auto const pTechWS1 = pTechType->GetWeapon(1);
		const bool isArmed = GetWeaponThreatRate(pTechWS0 ? pTechWS0->WeaponType : nullptr) > 0
			|| GetWeaponThreatRate(pTechWS1 ? pTechWS1->WeaponType : nullptr) > 0;
		const int firepower = isArmed ? GetCombatValueOfType(pTechType, false) : 0;
		int groundRange = 0;

		for (auto const pTechWS : { pTechWS0, pTechWS1 })
		{
			auto const pTechW = pTechWS ? pTechWS->WeaponType : nullptr;

			if (pTechW && pTechW->Damage > 0 && (!pTechW->Projectile || pTechW->Projectile->AG))
				groundRange = std::max(groundRange, pTechW->Range);
		}

		if (isOwn)
		{
			if (firepower > 0 && pTech->WhatAmI() != AbstractType::Building)
				ownCombatContacts.push_back({ pTech, coords, -1, 0, firepower, false, 0, groundRange, pTechType->Speed });

			continue;
		}

		if (pTech->CloakState != CloakState::Uncloaked || !IsCellVisibleToAI(pHouse, pTech->GetCell()))
			continue;

		int value = GetCombatValueOfType(pTechType, false);

		if (auto const pTechUnit = abstract_cast<UnitClass*>(pTech))
		{
			if (pTechUnit->Type->Harvester || pTechUnit->Type->ResourceGatherer)
				value += 60 + std::clamp(static_cast<int>(scores[56]) / 4, 0, 120); // Economy raid value (slot 56).
		}
		else if (auto const pTechBld = abstract_cast<BuildingClass*>(pTech))
		{
			if (pTechBld->Type->Refinery || pTechBld->Type->PowerBonus > 0 || pTechBld->Type->Factory != AbstractType::None)
				value += 40;
		}

		if (pTech->GetHealthPercentage() < 0.5)
			value += value / 4; // Finish off damaged targets.

		enemyContacts.push_back({ pTech, coords,
			MapClass::Instance.GetMovementZoneType(coords, MovementZone::Normal, pTech->OnBridge), value, firepower,
			IsAirborneThreatTechno(pTech), 0, groundRange, pTechType->Speed });
	}

	// Enemy firepower around each contact (its escort, nearby defenses), computed once.
	for (auto& contact : enemyContacts)
	{
		for (auto const& other : enemyContacts)
		{
			if (other.Firepower > 0 && other.Coords.DistanceFrom(contact.Coords) <= 7.0)
				contact.LocalEnemyFirepower += other.Firepower;
		}
	}

	// A real fight, for the Expert 5 selector: our combat units and the visible armed enemies
	// within reach of each other (the longer ground range of the pair plus 3 cells).
	{
		int engagedOwn = 0;
		int engagedEnemy = 0;
		std::vector<bool> enemyEngaged(enemyContacts.size(), false);

		for (auto const& own : ownCombatContacts)
		{
			bool engaged = false;

			for (size_t i = 0; i < enemyContacts.size(); ++i)
			{
				auto const& enemy = enemyContacts[i];

				if (enemy.Firepower <= 0)
					continue;

				const double reach = std::max(own.GroundRange, enemy.GroundRange) / static_cast<double>(Unsorted::LeptonsPerCell) + 3.0;

				if (own.Coords.DistanceFrom(enemy.Coords) <= reach)
				{
					engaged = true;
					enemyEngaged[i] = true;
				}
			}

			if (engaged)
				engagedOwn += own.Firepower;
		}

		for (size_t i = 0; i < enemyContacts.size(); ++i)
		{
			if (enemyEngaged[i])
				engagedEnemy += enemyContacts[i].Firepower;
		}

		auto& contactMacro = GetMacroState(pHouse);
		contactMacro.ArmyContactOwn = engagedOwn;
		contactMacro.ArmyContactEnemy = engagedEnemy;

		if (engagedOwn >= 150 && engagedEnemy >= 100)
			contactMacro.LastArmyContactFrame = Unsorted::CurrentFrame;
	}

	// Damage of one volley of the weapon this shooter would use against that target (Verses and
	// shields included).
	auto volleyDamageAgainst = [](TechnoClass* pShooter, TechnoClass* pTarget, const WeaponTypeClass** ppWeapon) -> int
	{
		auto const pWS = pShooter->GetWeapon(pShooter->SelectWeapon(pTarget));
		auto const pW = pWS ? pWS->WeaponType : nullptr;
		*ppWeapon = pW;

		if (!pW || pW->Damage <= 0 || !pW->Warhead)
			return 0;

		const double verses = GeneralUtils::GetWarheadVersusArmor(pW->Warhead, pTarget, pTarget->GetTechnoType());

		return static_cast<int>(pW->Damage * std::max(1, pW->Burst) * verses);
	};

	// Damage per volley our units already put on each enemy they are shooting (lookup only).
	std::unordered_map<const TechnoClass*, int> focusDamage;

	for (auto const& own : ownCombatContacts)
	{
		auto const pOwnTarget = abstract_cast<TechnoClass*>(own.Techno->Target);

		if (!pOwnTarget || !pOwnTarget->Owner || pOwnTarget->Owner == pHouse || pHouse->IsAlliedWith(pOwnTarget->Owner))
			continue;

		const WeaponTypeClass* pOwnWeapon = nullptr;
		const int damage = volleyDamageAgainst(own.Techno, pOwnTarget, &pOwnWeapon);

		if (damage > 0)
			focusDamage[pOwnTarget] += damage;
	}

	// Focus fire (slot 76): the visible enemy inside our reach that is worth the most per volley
	// needed to kill it, the volleys of our units already shooting it included (Lanchester: kill
	// the dangerous ones first, and together). Never joins a target the others already kill twice
	// over, and keeps the current target unless the new one is clearly better (Expert 5 switches
	// sooner). Returns the new target, or nullptr to keep the current one.
	const bool focusFireOpen = SlotGate(scores[76], 80, 76);
	const int focusSwitchPercent = (pExt->NeuralAI_ActiveExpert == 5) ? 115 : 140;

	auto pickFocusTarget = [&](FootClass* pFoot, int maxRange) -> TechnoClass*
	{
		if (!focusFireOpen || enemyContacts.empty() || maxRange <= 0)
			return nullptr;

		auto const pCurrent = abstract_cast<TechnoClass*>(pFoot->Target);
		TechnoClass* pBest = nullptr;
		int bestScore = 0;
		int bestDamage = 0;
		int currentScore = 0;

		for (auto const& contact : enemyContacts)
		{
			auto const pTarget = contact.Techno;
			const int dist = pFoot->DistanceFrom(pTarget);

			if (dist > maxRange + Unsorted::LeptonsPerCell / 2 || pTarget->Health <= 0)
				continue;

			const WeaponTypeClass* pW = nullptr;
			const int damage = volleyDamageAgainst(pFoot, pTarget, &pW);

			if (damage <= 0 || dist > pW->Range + Unsorted::LeptonsPerCell / 2 || dist < pW->MinimumRange)
				continue;

			if (pW->Projectile && (contact.Airborne ? !pW->Projectile->AA : !pW->Projectile->AG))
				continue;

			int others = 0;
			auto const it = focusDamage.find(pTarget);

			if (it != focusDamage.end())
				others = std::max(0, it->second - (pTarget == pCurrent ? damage : 0));

			if (pTarget != pCurrent && others >= pTarget->Health * 2)
				continue;

			const int volleys = 1 + (pTarget->Health - 1) / std::max(1, damage + others);
			const int score = (contact.Value + contact.Firepower) * 100 / volleys;

			if (pTarget == pCurrent)
				currentScore = score;

			if (score > bestScore)
			{
				bestScore = score;
				bestDamage = damage;
				pBest = pTarget;
			}
		}

		if (!pBest || pBest == pCurrent || (currentScore > 0 && bestScore * 100 < currentScore * focusSwitchPercent))
			return nullptr;

		focusDamage[pBest] += bestDamage;

		return pBest;
	};

	// True while a unit is heading to, waiting at or standing on one of our repair stations. Such
	// a unit is never ordered there again: a new Enter order restarts its approach.
	auto isAtOrBoundForRepair = [](FootClass* pFoot) -> bool
	{
		if (IsTransportUnderRepair(pFoot))
			return true;

		if (pFoot->QueuedMission == Mission::Enter)
		{
			auto const pDestBld = abstract_cast<BuildingClass*>(pFoot->Destination);

			if (pDestBld && pDestBld->Type->UnitRepair)
				return true;
		}

		auto const pFootCell = pFoot->GetCell();
		auto const pCellBld = pFootCell ? pFootCell->GetBuilding() : nullptr;

		return pCellBld && pCellBld->Type->UnitRepair && pCellBld->Owner == pFoot->Owner;
	};

	// Repair trips under way (at or bound for a station, or ordered within the last 450 frames)
	// and how many may be at once: a third of the combat units, so a fight is never left by the
	// whole army. Trip memories older than 3000 frames are dropped.
	auto& repairTripFrames = GetMacroState(pHouse).RepairTripFrames;
	const int repairNow = Unsorted::CurrentFrame;

	std::erase_if(repairTripFrames, [repairNow](auto const& entry) { return repairNow - entry.second > 3000 || repairNow < entry.second; });

	auto repairTripRecent = [&](FootClass* pFoot) -> bool
	{
		auto const it = repairTripFrames.find(pFoot->UniqueID);

		return it != repairTripFrames.end() && repairNow - it->second < 450;
	};

	int repairTripsActive = 0;

	for (auto const& own : ownCombatContacts)
	{
		auto const pOwnFoot = abstract_cast<FootClass*>(own.Techno);

		if (pOwnFoot && (isAtOrBoundForRepair(pOwnFoot) || repairTripRecent(pOwnFoot) || IsOnRepairTrip(pHouse, pOwnFoot->UniqueID)))
			repairTripsActive++;
	}

	const int repairTripsCap = std::max(1, static_cast<int>(ownCombatContacts.size()) / 3);

	// A repair trip pays when the place it heals at (station, hospital, repairer, self-heal spot) is
	// close enough for what the unit is worth, the repair money is there (paid repairs: depots), no
	// visible armed enemy covers that place or a stretch of the way that gets closer to it than the
	// unit is now, no known enemy defense sits on the way, and the trip cap above leaves the fight to
	// the rest (a unit about to die may always go).
	auto repairTripPays = [&](FootClass* pFoot, const CellStruct& to, bool paid) -> bool
	{
		auto const pFootType = pFoot->GetTechnoType();
		const CellStruct from = pFoot->GetMapCoords();
		const double tripCells = from.DistanceFrom(to);

		if (tripCells > std::clamp(20.0 + pFootType->Cost / 20.0, 30.0, 100.0))
			return false;

		const double health = pFoot->GetHealthPercentage();
		const int repairCost = paid ? static_cast<int>(pFootType->GetActualCost(pHouse) * (1.0 - health) * RulesClass::Instance->RepairPercent) : 0;

		if (pHouse->Available_Money() < repairCost)
			return false;

		if (health >= 0.2 && repairTripsActive >= repairTripsCap)
			return false;

		const int probes = static_cast<int>(tripCells / 2.0);

		for (auto const& contact : enemyContacts)
		{
			if (contact.Firepower <= 0 || contact.GroundRange <= 0)
				continue;

			const double reachCells = contact.GroundRange / static_cast<double>(Unsorted::LeptonsPerCell) + 1.0;

			if (contact.Coords.DistanceFrom(to) <= std::max(8.0, reachCells + 2.0))
				return false;

			const double threatToFoot = contact.Coords.DistanceFrom(from);

			for (int p = 1; p <= probes; ++p)
			{
				const double t = p * 2.0 / tripCells;
				const CellStruct probe { static_cast<short>(from.X + (to.X - from.X) * t), static_cast<short>(from.Y + (to.Y - from.Y) * t) };
				const double threatToProbe = contact.Coords.DistanceFrom(probe);

				if (threatToProbe <= reachCells && threatToProbe < threatToFoot)
					return false;
			}
		}

		for (int p = 3; p * 2 < static_cast<int>(tripCells); p += 2)
		{
			const double t = p * 2.0 / tripCells;
			const CellStruct probe { static_cast<short>(from.X + (to.X - from.X) * t), static_cast<short>(from.Y + (to.Y - from.Y) * t) };

			if (CountKnownEnemyDefensesNear(pHouse, probe, 7.0, false) > 0)
				return false;
		}

		return true;
	};

	// A fight is being won around a unit when our fighting units within 8 cells (not on a repair
	// trip) bring twice the firepower of the visible armed enemies there, both weighted by health.
	// A damaged unit stays in such a fight: leaving would cost the fire that wins it.
	auto fightWonAround = [&](FootClass* pFoot) -> bool
	{
		const CellStruct fc = pFoot->GetMapCoords();
		int enemyFire = 0;
		int ownFire = 0;

		for (auto const& contact : enemyContacts)
		{
			if (contact.Firepower > 0 && contact.Coords.DistanceFrom(fc) <= 8.0)
				enemyFire += static_cast<int>(contact.Firepower * contact.Techno->GetHealthPercentage());
		}

		if (enemyFire <= 0)
			return false;

		for (auto const& own : ownCombatContacts)
		{
			if (own.Coords.DistanceFrom(fc) <= 8.0 && !IsOnRepairTrip(pHouse, own.Techno->UniqueID))
				ownFire += static_cast<int>(own.Firepower * own.Techno->GetHealthPercentage());
		}

		return ownFire >= enemyFire * 2;
	};

	const int armyAverageCost = GetArmyAverageCost(pHouse);

	// Hospitals (Hospital=yes) of ours and our allies: infantry walks in to be healed.
	std::vector<BuildingClass*> repairHospitals;

	for (auto const pBld : BuildingClass::Array)
	{
		if (pBld && pBld->IsAlive && !pBld->InLimbo && pBld->Type && pBld->Type->Hospital && pBld->Owner
			&& (pBld->Owner == pHouse || pHouse->IsAlliedWith(pBld->Owner)) && pBld->CurrentMission != Mission::Selling)
		{
			repairHospitals.push_back(pBld);
		}
	}

	// Repair retreat of a damaged valuable unit: a depot (vehicles that can be repaired there), a
	// hospital (infantry), one of our units with a healing weapon for it, or a wait next to our
	// buildings while the house self-heal works, the first of them whose trip pays. Never while it
	// helps win a fight (unless about to die), never twice in a row (UpdateRepairTrips follows the
	// trip and sends the unit back to its group or plan once healed). True when a trip started.
	auto tryRepairRetreat = [&](FootClass* pFoot, double threshold) -> bool
	{
		const double health = pFoot->GetHealthPercentage();

		if (health >= threshold || !IsRepairTripAllowed(pHouse, pFoot->UniqueID))
			return false;

		if (health >= 0.2 && fightWonAround(pFoot))
		{
			CountDecision(NeuralDecision::RepairHeldWinning, pHouse->ArrayIndex);
			return false;
		}

		auto const pType = pFoot->GetTechnoType();
		const bool isInfantry = pFoot->WhatAmI() == AbstractType::Infantry;
		const int footZone = ObjectZone(pFoot, MovementZone::Normal);

		// A deployed unit cannot drive anywhere.
		if (auto const pFootUnit = abstract_cast<UnitClass*>(pFoot))
		{
			if (pFootUnit->Deployed)
				return false;
		}

		// Infantry heals between fights: a soldier walking off under fire is shot in the back and
		// stops shooting, so it stays while armed enemies are in sight within 6 cells.
		if (isInfantry)
		{
			const CellStruct fc = pFoot->GetMapCoords();

			for (auto const& contact : enemyContacts)
			{
				if (contact.Firepower > 0 && contact.Coords.DistanceFrom(fc) <= 6.0)
					return false;
			}
		}

		if (!isInfantry && pRepairDepot && pType->Repairable && !pType->Organic && !IsAmphibiousOrAirTransport(pType)
			&& !IsRepairStationFailed(pHouse, pType, NeuralRepairKind::Depot))
		{
			if (auto const pStation = FindBestRepairStation(pHouse, pFoot))
			{
				if (repairTripPays(pFoot, pStation->GetMapCoords(), true))
				{
					repairTripsActive++;
					repairTripFrames[pFoot->UniqueID] = repairNow;
					DispatchFootToTargetOrTransport(pFoot, pStation, Mission::Enter, false);
					BeginRepairTrip(pHouse, pFoot, NeuralRepairKind::Depot, pStation->UniqueID, CellStruct::Empty);

					return true;
				}
			}
		}

		if (isInfantry && !pType->Naval && !IsRepairStationFailed(pHouse, pType, NeuralRepairKind::Hospital))
		{
			BuildingClass* pBestHospital = nullptr;
			double bestDist = 1e9;

			for (auto const pHospitalBld : repairHospitals)
			{
				const double d = pHospitalBld->GetMapCoords().DistanceFrom(pFoot->GetMapCoords());

				if (d < bestDist && IsBuildingEnterableFromZone(pHospitalBld, footZone))
				{
					bestDist = d;
					pBestHospital = pHospitalBld;
				}
			}

			if (pBestHospital && repairTripPays(pFoot, pBestHospital->GetMapCoords(), false))
			{
				repairTripsActive++;
				OrderFootToEnter(pFoot, pBestHospital);
				BeginRepairTrip(pHouse, pFoot, NeuralRepairKind::Hospital, pBestHospital->UniqueID, CellStruct::Empty);

				return true;
			}
		}

		if (auto const pRepairer = FindFieldRepairer(pHouse, pFoot, 20.0))
		{
			// A repairer in the same fight heals the unit where it stands: walking over to it would
			// only stop the unit's fire.
			if (pRepairer->GetMapCoords().DistanceFrom(pFoot->GetMapCoords()) <= 6.0)
			{
				if (pRepairer->Target != pFoot)
				{
					pRepairer->SetTarget(pFoot);
					pRepairer->QueueMission(Mission::Attack, true);
				}

				return false;
			}

			if (repairTripPays(pFoot, pRepairer->GetMapCoords(), false))
			{
				repairTripsActive++;
				IssueMoveOrder(pFoot, pRepairer->GetCell(), 2.0);
				pRepairer->SetTarget(pFoot);
				pRepairer->QueueMission(Mission::Attack, true);
				BeginRepairTrip(pHouse, pFoot, NeuralRepairKind::Repairer, pRepairer->UniqueID, CellStruct::Empty);

				return true;
			}
		}

		// Self-heal only when it brings the unit back within 3600 frames.
		const double selfHealRate = SelfHealRateOf(pHouse, pFoot);

		if (selfHealRate > 0.0 && (pType->Strength * 0.9 - pFoot->Health) / selfHealRate <= 3600.0)
		{
			const CellStruct waitCell = FindSelfHealWaitCell(pHouse, pFoot, std::clamp(20.0 + pType->Cost / 20.0, 30.0, 100.0));

			if (waitCell != CellStruct::Empty && repairTripPays(pFoot, waitCell, false))
			{
				repairTripsActive++;
				IssueMoveOrder(pFoot, MapClass::Instance.TryGetCellAt(waitCell), 2.0);
				BeginRepairTrip(pHouse, pFoot, NeuralRepairKind::SelfHeal, 0, waitCell);

				return true;
			}
		}

		return false;
	};

	// Best nearby enemy an idle unit can reach and hit while we hold local
	// superiority, so units punish exposed targets instead of idling, but never
	// walk alone into a defended area.
	auto findOpportunityTarget = [&](FootClass* pFoot, const WeaponTypeClass* pW0, const WeaponTypeClass* pW1)
		-> TechnoClass*
	{
		if (!pFoot || enemyContacts.empty())
			return nullptr;

		auto canHit = [](const WeaponTypeClass* pW, bool airborne) -> bool
		{
			if (!pW || pW->Damage <= 0)
				return false;

			if (!pW->Projectile)
				return !airborne;

			return airborne ? pW->Projectile->AA : pW->Projectile->AG;
		};

		const int maxRange = std::max(pW0 ? pW0->Range : 0, pW1 ? pW1->Range : 0);
		const double searchCells = std::max(10.0, maxRange / static_cast<double>(Unsorted::LeptonsPerCell) + 4.0);
		const CellStruct footCoords = pFoot->GetMapCoords();
		const int footZone = MapClass::Instance.GetMovementZoneType(footCoords, MovementZone::Normal, pFoot->OnBridge);
		const bool footCrossesWater = CanFootCrossWaterDirectly(pFoot);
		TechnoClass* pBest = nullptr;
		int bestScore = 0;
		int ownLocal = 0;

		for (auto const& own : ownCombatContacts)
		{
			if (own.Coords.DistanceFrom(footCoords) <= 7.0)
				ownLocal += own.Firepower;
		}

		for (auto const& contact : enemyContacts)
		{
			const double dist = footCoords.DistanceFrom(contact.Coords);

			if (dist > searchCells || (!canHit(pW0, contact.Airborne) && !canHit(pW1, contact.Airborne)))
				continue;

			const bool inRangeNow = dist * Unsorted::LeptonsPerCell <= maxRange;

			if (!inRangeNow && !contact.Airborne && !footCrossesWater && contact.Zone != footZone)
				continue; // Not reachable on foot from here.

			if (ownLocal * 10 < contact.LocalEnemyFirepower * 9)
				continue;

			const int score = contact.Value * 100 / (10 + static_cast<int>(dist * 3));

			if (score > bestScore)
			{
				bestScore = score;
				pBest = contact.Techno;
			}
		}

		return pBest;
	};

	int bestSiegeScore = -1;

	for (auto const pEBld : BuildingClass::Array)
	{
		if (!pEBld || pEBld->InLimbo || !pEBld->IsAlive || !pEBld->Type || pEBld->Type->Immune
			|| pEBld->Type->Insignificant || pEBld->Type->InvisibleInGame)
		{
			continue;
		}

		if (pEBld->Owner == pHouse)
		{
			if (pEBld->TemporalTargetingMe && pEBld->TemporalTargetingMe->Owner && !pEnemyTemporalAttacker)
				pEnemyTemporalAttacker = pEBld->TemporalTargetingMe->Owner;

			if (pEBld->DrainingMe && !pEnemyDrainer)
				pEnemyDrainer = pEBld->DrainingMe;

			if (!pGrinderBld && pEBld->Type->Grinding)
				pGrinderBld = pEBld;

			if (!pEmptyTankBunker && pEBld->Type->Bunker && pEBld->BunkerLinkedItem == nullptr)
				pEmptyTankBunker = pEBld;

			if (!pUnitReloadBld && (pEBld->Type->UnitReload || pEBld->Type->UnitRepair || pEBld->Type->Helipad))
				pUnitReloadBld = pEBld;

		}

		// Damaged occupiable buildings an engineer could repair: their health is only known while
		// they are ours or in sight (fog-fair). Evacuations are UpdateUrban's job (Urban.inl).
		if (pEBld->Type->CanBeOccupied && pEBld->Type->Repairable && pEBld->GetHealthPercentage() < 0.72
			&& (pEBld->Owner == pHouse || IsCellInSightOfAI(pHouse, pEBld->GetMapCoords())))
		{
			const bool isFriendlyOrEmpty = (pEBld->Occupants.Count == 0)
				|| (pEBld->Occupants.GetItem(0) && pHouse->IsAlliedWith(pEBld->Occupants.GetItem(0)->Owner));

			if (isFriendlyOrEmpty && !pDamagedUrbanToRepair)
				pDamagedUrbanToRepair = pEBld;
		}

		if (!pEBld->Owner || pEBld->Owner == pHouse || pHouse->IsAlliedWith(pEBld->Owner) || pEBld->Owner->IsNeutral())
			continue;

		if (!pEBld->GetCell() || !IsCellVisibleToAI(pHouse, pEBld->GetCell()))
			continue;

		// Occupants are only known while the building is in sight now.
		const bool enemyGarrisonInSight = pEBld->Type->CanBeOccupied && pEBld->Occupants.Count > 0
			&& IsCellInSightOfAI(pHouse, pEBld->GetMapCoords());

		if (enemyGarrisonInSight)
		{
			if (!pEnemyGarrisonToClear)
				pEnemyGarrisonToClear = pEBld;

			if (enemyGarrisonCount < 6)
				enemyGarrisonsToClear[enemyGarrisonCount++] = pEBld;
		}

		if (pEBld->Type->Capturable && pEBld->Type->ConstructionYard && !pCapturableEnemyConYard)
			pCapturableEnemyConYard = pEBld;

		if (SlotGate(scores[79], 80, 79)
			&& (pEBld->Type->SuperWeapon != -1 || pEBld->Type->PowerBonus > 0
				|| pEBld->Type->Factory != AbstractType::None || pEBld->Type->ConstructionYard))
		{
			if (!pSurgicalEnemyBld || pEBld->Type->SuperWeapon != -1
				|| (pEBld->Type->PowerBonus > 0 && pSurgicalEnemyBld->Type->SuperWeapon == -1))
			{
				pSurgicalEnemyBld = pEBld;
			}

			if (surgicalEnemyBldCount < 8)
				surgicalEnemyBlds[surgicalEnemyBldCount++] = pEBld;
		}

		if (SlotGate(scores[57], 150, 57))
		{
			int sScore = 0;

			if (enemyGarrisonInSight)
				sScore = 100 + pEBld->Occupants.Count * 15;
			else if (pEBld->Type->IsBaseDefense)
				sScore = 85;

			if (sScore > bestSiegeScore)
			{
				bestSiegeScore = sScore;

				pPrioritySiegeBld = pEBld;
			}
		}
	}

	if (pCapturableEnemyConYard && SlotGate(scores[82], 60, 82))
	{
		for (auto const pEng : InfantryClass::Array)
		{
			if (pEng && pEng->Owner == pHouse && !pEng->InLimbo && pEng->IsAlive && pEng->Type && pEng->Type->Engineer
				&& pEng->DistanceFrom(pCapturableEnemyConYard) <= 2560)
			{
				ownEngineerNearEnemyConYard = true;

				break;
			}
		}
	}

	// Slot 82 (Stolen ConYard / MCV Human Shield Escort, Anti-Recapture & Island
	// Extraction): Detect if we own a newly captured ConYard or stolen/escaping
	// MCV outside our safe home base (> 16 cells from homeCoords), and identify
	// the most dangerous enemy threat near it (prioritizing enemy Engineers
	// trying to recapture it, then enemy tanks/infantry firing at it) so nearby
	// friendly units & infantry act as Human Shields / Bodyguards!
	TechnoClass* pStolenConYardOrMCV = nullptr;
	TechnoClass* pThreatToStolenAsset = nullptr;
	UnitClass* pStrandedStolenMCVOnIsland = nullptr;

	if (SlotGate(scores[82], 60, 82) && homeCoords != CellStruct::Empty)
	{
		for (auto const pOwnB : pHouse->Buildings)
		{
			if (!pOwnB || pOwnB->InLimbo || !pOwnB->IsAlive || !pOwnB->Type || !pOwnB->Type->ConstructionYard)
				continue;

			if (pHouse->NumConYards >= 2 && pOwnB != pHouse->ConYards.GetItem(0)
				&& pOwnB->GetMapCoords().DistanceFrom(homeCoords) > 16.0)
			{
				pStolenConYardOrMCV = pOwnB;

				break;
			}
		}

		for (auto const pOwnU : UnitClass::Array)
		{
			if (!pOwnU || pOwnU->Owner != pHouse || pOwnU->InLimbo || !pOwnU->IsAlive || !pOwnU->Type
				|| !pOwnU->Type->DeploysInto || !pOwnU->Type->DeploysInto->ConstructionYard)
			{
				continue;
			}

			const CellStruct mc = pOwnU->GetMapCoords();

			const bool isForeignMCV
				= (pHouse->CountOwnedNow(pOwnU->Type->DeploysInto) == 0 && pHouse->NumConYards >= 1);

			if (mc.DistanceFrom(homeCoords) > 14.0 && (isForeignMCV || pOwnU->HasBeenAttacked))
			{
				pStolenConYardOrMCV = pOwnU;

				const int mz = MapClass::Instance.GetMovementZoneType(mc, MovementZone::Normal, pOwnU->OnBridge);

				if (homeGroundZone >= 0 && mz >= 0 && mz != homeGroundZone && isForeignMCV)
					pStrandedStolenMCVOnIsland = pOwnU;

				break;
			}
		}

		if (pStolenConYardOrMCV)
		{
			int bestThreatScore = -1;

			for (auto const pET : TechnoClass::Array)
			{
				if (!pET || pET->InLimbo || !pET->IsAlive || !pET->Owner || pET->WhatAmI() == AbstractType::Building)
					continue;

				if (pET->Owner == pHouse || pHouse->IsAlliedWith(pET->Owner) || !IsCellVisibleToAI(pHouse, pET->GetCell()) || pET->Owner->IsNeutral())
					continue;

				const int dist = pStolenConYardOrMCV->DistanceFrom(pET);

				if (dist > 3584)
					continue;

				int tScore = 4000 - dist;

				if (auto const pEI = abstract_cast<InfantryClass*>(pET))
				{
					if (pEI->Type && pEI->Type->Engineer)
						tScore += 15000;
					else if (pEI->Type && (pEI->Type->C4 || pEI->Type->Ivan))
						tScore += 9000;
				}

				if (pET->Target == pStolenConYardOrMCV)
					tScore += 5000;

				if (tScore > bestThreatScore)
				{
					bestThreatScore = tScore;

					pThreatToStolenAsset = pET;
				}
			}
		}
	}

	std::vector<CellStruct> claimedCrateCellsThisTick;
	auto findNearestReachableCrateForFoot = [&](FootClass* pFoot, int maxRadiusCells) -> CellClass*
	{
		if (!pFoot || !pFoot->IsAlive || pFoot->InLimbo)
			return nullptr;

		auto const pFType = pFoot->GetTechnoType();
		if (!pFType)
			return nullptr;

		const CellStruct fc = pFoot->GetMapCoords();
		const bool isNavalFoot = pFType->Naval || (pFType->SpeedType == SpeedType::Float);
		const bool canCrossWater = isNavalFoot || pFType->JumpJet
			|| pFType->MovementZone == MovementZone::Fly
			|| pFType->MovementZone == MovementZone::Amphibious
			|| pFType->MovementZone == MovementZone::AmphibiousDestroyer
			|| pFType->MovementZone == MovementZone::AmphibiousCrusher
			|| pFType->SpeedType == SpeedType::Hover
			|| pFType->SpeedType == SpeedType::Amphibious;
		const int footZone = MapClass::Instance.GetMovementZoneType(fc, isNavalFoot ? MovementZone::Water : MovementZone::Normal, pFoot->OnBridge);

		CellClass* pBestCrateCell = nullptr;
		int bestDistSq = (maxRadiusCells * maxRadiusCells) + 1;

		for (int dy = -maxRadiusCells; dy <= maxRadiusCells; ++dy)
		{
			for (int dx = -maxRadiusCells; dx <= maxRadiusCells; ++dx)
			{
				const int dSq = dx * dx + dy * dy;
				if (dSq >= bestDistSq)
					continue;

				const CellStruct tc { static_cast<short>(fc.X + dx), static_cast<short>(fc.Y + dy) };
				auto const pC = MapClass::Instance.TryGetCellAt(tc);
				if (!pC || pC->OverlayTypeIndex < 0 || pC->OverlayTypeIndex >= OverlayTypeClass::Array.Count
					|| !IsCellVisibleToAI(pHouse, pC))
				{
					continue;
				}

				auto const pOvl = OverlayTypeClass::Array.GetItem(pC->OverlayTypeIndex);
				if (!pOvl || !pOvl->Crate)
					continue;

				if (std::find(claimedCrateCellsThisTick.begin(), claimedCrateCellsThisTick.end(), tc)
					!= claimedCrateCellsThisTick.end())
				{
					continue;
				}

				const bool crateOnWater = (pC->LandType == LandType::Water);
				if (isNavalFoot && !crateOnWater)
					continue;
				if (!canCrossWater && crateOnWater)
					continue;

				if (!pC->IsClearToMove(pFType->SpeedType, false, false, -1, pFType->MovementZone, -1, false))
					continue;

				if (!canCrossWater && footZone >= 0)
				{
					const int crateZone = MapClass::Instance.GetMovementZoneType(tc, MovementZone::Normal, false);
					if (crateZone != footZone)
						continue;
				}
				else if (isNavalFoot && footZone >= 0)
				{
					const int crateWaterZone = MapClass::Instance.GetMovementZoneType(tc, MovementZone::Water, false);
					if (crateWaterZone != footZone)
						continue;
				}

				bestDistSq = dSq;
				pBestCrateCell = pC;
			}
		}

		if (pBestCrateCell)
			claimedCrateCellsThisTick.push_back(pBestCrateCell->MapCoords);

		return pBestCrateCell;
	};

	// Strict Objective & Combat Non-Interruption Guards:
	// Never abort the target or attack mission of a unit/infantry that is already
	// attacking an enemy objective or actively marching toward an enemy target to
	// pull it into a critical-mass assembly, staging rally, or transport!
	auto IsFootActivelyAttackingOrMovingToEnemy = [pHouse](const FootClass* pFoot) -> bool
	{
		if (!pFoot || !pFoot->IsAlive || pFoot->InLimbo)
			return false;

		if (pFoot->Target != nullptr)
		{
			if (auto const pTargetTech = abstract_cast<TechnoClass*>(pFoot->Target))
			{
				if (pTargetTech->IsAlive && !pTargetTech->InLimbo)
					return true;
			}
			else if (pFoot->Target->WhatAmI() == AbstractType::Cell)
			{
				return true;
			}
		}

		if (pFoot->CurrentMission == Mission::Attack || pFoot->QueuedMission == Mission::Attack
			|| pFoot->SuspendedMission == Mission::Attack || pFoot->CurrentMission == Mission::Hunt
			|| pFoot->QueuedMission == Mission::Hunt)
		{
			return true;
		}

		if (pFoot->HaveAttackMoveTarget || pFoot->IsFiring)
			return true;

		AbstractClass* pDest = pFoot->Destination
			? pFoot->Destination
			: (pFoot->MegaDestination ? pFoot->MegaDestination : pFoot->MegaTarget);
		if (pDest != nullptr
			&& (pFoot->CurrentMission == Mission::Move || pFoot->CurrentMission == Mission::Area_Guard
				|| pFoot->CurrentMission == Mission::AttackMove || pFoot->MegaMission == Mission::AttackMove))
		{
			if (auto const pDestTech = abstract_cast<TechnoClass*>(pDest))
			{
				if (pDestTech->IsAlive && !pDestTech->InLimbo && pDestTech->Owner && pDestTech->Owner != pHouse
					&& !pHouse->IsAlliedWith(pDestTech->Owner) && !pDestTech->Owner->IsNeutral())
				{
					return true;
				}
			}

			const CellStruct fc = pFoot->GetMapCoords();
			const CellStruct dc = CellClass::Coord2Cell(pDest->GetCoords());
			if (auto const pDestCell = MapClass::Instance.TryGetCellAt(dc))
			{
				if (pDestCell->OverlayTypeIndex >= 0 && pDestCell->OverlayTypeIndex < OverlayTypeClass::Array.Count)
				{
					if (auto const pDestOvl = OverlayTypeClass::Array.GetItem(pDestCell->OverlayTypeIndex))
					{
						if (pDestOvl->Crate && fc.DistanceFrom(dc) <= 20.0)
							return true; // Never interrupt a unit/infantry actively walking to pick up a Crate!
					}
				}
			}

			if (fc.DistanceFrom(dc) > 2.0)
			{
				for (auto const pEB : BuildingClass::Array)
				{
					if (!pEB || pEB->InLimbo || !pEB->IsAlive || !pEB->Owner || !pEB->Type || pEB->Type->Insignificant
						|| pEB->Type->InvisibleInGame)
					{
						continue;
					}

					if (pEB->Owner == pHouse || pHouse->IsAlliedWith(pEB->Owner) || !IsEnemyBuildingKnown(pHouse, pEB) || pEB->Owner->IsNeutral()
						|| pEB->Owner->IsObserver() || pEB->Owner->Defeated)
					{
						continue;
					}

					if (dc.DistanceFrom(pEB->GetMapCoords()) <= 15.0)
						return true;
				}

				for (auto const pEU : UnitClass::Array)
				{
					if (!pEU || pEU->InLimbo || !pEU->IsAlive || !pEU->Owner || !pEU->Type)
						continue;

					if (pEU->Owner == pHouse || pHouse->IsAlliedWith(pEU->Owner) || !IsCellVisibleToAI(pHouse, pEU->GetCell()) || pEU->Owner->IsNeutral()
						|| pEU->Owner->IsObserver() || pEU->Owner->Defeated)
					{
						continue;
					}

					if (dc.DistanceFrom(pEU->GetMapCoords()) <= 12.0)
						return true;
				}
			}
		}

		return false;
	};

	// Same test as before (known enemy structure within 18.5 cells, visible enemy vehicle within
	// 13, same landmass unless naval) through the spatial index instead of scanning every
	// building and vehicle for every unit. A yes / no answer, so the scan order does not matter.
	auto IsFootNearEnemyFrontline = [pHouse](const FootClass* pFoot, int footZone) -> bool
	{
		if (!pFoot || !pFoot->IsAlive || pFoot->InLimbo)
			return false;

		const CellStruct fc = pFoot->GetMapCoords();
		bool nearEnemy = false;

		ForEachTechnoNear(fc, 19, [&](TechnoClass* pTechno) -> bool
		{
			auto const pOwner = pTechno->Owner;

			if (!pOwner || pOwner == pHouse || pHouse->IsAlliedWith(pOwner) || pOwner->IsNeutral() || pOwner->IsObserver()
				|| pOwner->Defeated)
			{
				return true;
			}

			if (auto const pEB = abstract_cast<BuildingClass*>(pTechno))
			{
				if (!pEB->Type || pEB->Type->Insignificant || pEB->Type->InvisibleInGame || !IsEnemyBuildingKnown(pHouse, pEB))
					return true;

				const CellStruct ebc = pEB->GetMapCoords();

				if (footZone >= 0 && !pEB->Type->Naval
					&& MapClass::Instance.GetMovementZoneType(ebc, MovementZone::Normal, pEB->OnBridge) != footZone)
				{
					return true;
				}

				if (fc.DistanceFrom(ebc) <= 18.5)
					nearEnemy = true;
			}
			else if (auto const pEU = abstract_cast<UnitClass*>(pTechno))
			{
				if (!pEU->Type || !IsCellVisibleToAI(pHouse, pEU->GetCell()))
					return true;

				const CellStruct euc = pEU->GetMapCoords();

				if (footZone >= 0 && !pEU->Type->Naval
					&& MapClass::Instance.GetMovementZoneType(euc, MovementZone::Normal, pEU->OnBridge) != footZone)
				{
					return true;
				}

				if (fc.DistanceFrom(euc) <= 13.0)
					nearEnemy = true;
			}

			return !nearEnemy;
		});

		return nearEnemy;
	};

	microClock.Mark(NeuralSection::MicroUnits);

	for (auto const pUnit : UnitClass::Array)
	{
		if (!pUnit || pUnit->Owner != pHouse || pUnit->InLimbo || !pUnit->IsAlive)
			continue;

		auto const pType = pUnit->Type;

		if (!pType)
			continue;

		// Scouts on a trip take their orders from it.
		if (IsActiveScout(pHouse, pUnit->UniqueID))
			continue;

		// A demolition truck sneaking in takes its orders from UpdateSneakStrikes.
		if (IsSneakStrikeActive(pHouse, pUnit->UniqueID))
			continue;

		if (pUnit->HasAnyLink())
		{
			if (auto const pLinkedBld = abstract_cast<BuildingClass*>(pUnit->GetNthLink(0)))
			{
				if (pLinkedBld->Type
					&& (pLinkedBld->Type->WeaponsFactory || pLinkedBld->Type->Factory == AbstractType::UnitType))
				{
					if (pUnit->CurrentMission == Mission::Move && pUnit->Destination != nullptr)
					{
						continue; // Never interrupt a vehicle's short exit move while still
								// linked to its War Factory / Shipyard bib!
					}

					pUnit->Scatter(CoordStruct::Empty, true, false);
					continue;
				}
			}
		}

		// Evade radiation clouds (Desolator / Nuke fallout) immediately so units
		// don't melt in place
		if (auto const pUnitCell = pUnit->GetCell())
		{
			if (pUnitCell->GetRadLevel() > 25 && pUnit->CurrentMission != Mission::Move)
				pUnit->Scatter(CoordStruct::Empty, true, false);
		}

		// Slot 58 (Anti-SW Scatter): When an enemy Superweapon is ready/imminent,
		// scatter tightly clumped combat vehicles so a single Nuke/Lightning Storm
		// doesn't wipe the whole army!
		if (enemySuperImminent && SlotGate(scores[58], SurvivalMicroVetoScore, 58) && !pType->Harvester && !pType->ResourceGatherer
			&& pUnit->CurrentMission != Mission::Move && (IsAITickWindow(90, 15)))
		{
			const CellStruct uc = pUnit->GetMapCoords();
			int clumpedNeighbors = 0;

			for (auto const pOtherU : UnitClass::Array)
			{
				if (!pOtherU || pOtherU == pUnit || pOtherU->Owner != pHouse || pOtherU->InLimbo || !pOtherU->IsAlive)
					continue;

				const CellStruct oc = pOtherU->GetMapCoords();

				if (std::abs(uc.X - oc.X) <= 1 && std::abs(uc.Y - oc.Y) <= 1)
					clumpedNeighbors++;
			}

			if (clumpedNeighbors >= 3)
			{
				pUnit->Scatter(CoordStruct::Empty, true, false);

				continue;
			}
		}

		// Slot 70 (Dying Transport / OpenTopped Emergency Passenger Unload):
		// If a transport or OpenTopped vehicle carrying passengers on land is about
		// to explode (< 24% HP), immediately Unload so the squad inside survives!
		// Strictly respect NoManualUnload / NoManualEnter (dummy sub-weapon
		// passenger hacks in any mod).
		if (pUnit->Passengers.NumPassengers > 0 && !pType->Naval && !HasNoManualUnload(pType)
			&& !HasNoManualEnter(pType) && pUnit->GetHealthPercentage() < 0.24 && SlotGate(scores[70], SurvivalMicroVetoScore, 70))
		{
			if (auto const pUC = pUnit->GetCell())
			{
				if (pUC->LandType != LandType::Water && pUnit->CurrentMission != Mission::Unload)
				{
					pUnit->QueueMission(Mission::Unload, true);

					continue;
				}
			}
		}

		// Slot 72 (Crazy Ivan AttachedBomb Kamikaze Charge): If our vehicle has an
		// attached Ivan bomb and no Repair Depot is nearby, charge straight into
		// the nearest enemy so the bomb explodes in the enemy's ranks instead of
		// ours!
		if (pUnit->AttachedBomb != nullptr && !pRepairDepot && SlotGate(scores[72], 60, 72))
		{
			TechnoClass* pKamikazeTarget = pSurgicalEnemyBld ? static_cast<TechnoClass*>(pSurgicalEnemyBld)
															: static_cast<TechnoClass*>(pWoundedEnemyVehicle);

			if (pKamikazeTarget && pUnit->Target != pKamikazeTarget)
			{
				pUnit->SetTarget(pKamikazeTarget);

				pUnit->QueueMission(Mission::Attack, true);

				continue;
			}
		}

		// If infected by a Terror Drone / Giant Squid (ParasiteEatingMe), rush
		// immediately to Service Depot to rip it out!
		if (pRepairDepot && pUnit->ParasiteEatingMe != nullptr)
		{
			if (auto const pStation = FindBestRepairStation(pHouse, pUnit))
			{
				if (pUnit->CurrentMission != Mission::Enter)
					OrderFootToEnter(pUnit, pStation);

				continue;
			}
		}

		// A unit on a repair trip takes its orders from UpdateRepairTrips until it is back.
		if (IsOnRepairTrip(pHouse, pUnit->UniqueID))
			continue;

		const int unitZoneForEco
			= ObjectZone(pUnit, MovementZone::Normal);
		// Only treat pExtraHomeSlaveMiner as a transport passenger while it is
		// still on homeGroundZone waiting to sail to resourceIslandZone! Once a
		// Slave Miner lands on another island (unitZoneForEco != homeGroundZone),
		// it must work normally via Mission::Harvest and NEVER be forced to deploy
		// on a barren beach (which causes YAREFN to loop in Mission::Selling)!
		const bool isIslandColonizerSlaveMiner = (pType->ResourceGatherer && pType->DeploysInto != nullptr
			&& pUnit == pExtraHomeSlaveMiner && homeGroundZone >= 0 && unitZoneForEco == homeGroundZone
			&& CanHouseFerryTypeAcrossWater(pHouse, pType) && pIslandResourceGoalCell != nullptr
			&& resourceIslandZone != homeGroundZone);
		if ((pType->Harvester || pType->ResourceGatherer) && !isIslandColonizerSlaveMiner)
		{
			const bool canHarvCrossWater = (pType->MovementZone == MovementZone::Amphibious
				|| pType->MovementZone == MovementZone::AmphibiousDestroyer
				|| pType->MovementZone == MovementZone::AmphibiousCrusher || pType->MovementZone == MovementZone::Fly
				|| pType->JumpJet || pType->Teleporter);
			ClearStaleEnterOrUnreachableCrossIslandMission(pUnit, canHarvCrossWater);
			if (pUnit->CurrentMission == Mission::Enter || pUnit->QueuedMission == Mission::Enter)
				continue;

			// An idle Slave Miner (ResourceGatherer, DeploysInto) gets the nearest ore of its own land
			// as Destination before Mission::Harvest: SlaveManagerClass::Harvest looks for a deploy
			// spot only around the Destination and sends the miner back to Guard without one, so a
			// miner told to harvest next to the ore just stood there. Once every 150 frames per miner.
			if (pType->ResourceGatherer && pType->DeploysInto != nullptr && !pType->Teleporter)
			{
				const bool minerIdle = pUnit->CurrentMission == Mission::Guard || pUnit->CurrentMission == Mission::Area_Guard
					|| pUnit->CurrentMission == Mission::Sleep || pUnit->CurrentMission == Mission::None;
				const int minerPhase = static_cast<int>((static_cast<unsigned>(Unsorted::CurrentFrame) + pUnit->UniqueID) % 150);

				if (!minerIdle || minerPhase >= std::max(15, CurrentAITickFrames))
					continue;

				const CellStruct uc = pUnit->GetMapCoords();
				CellClass* pNearestOre = nullptr;
				int nearestOreDist = INT_MAX;

				for (int dy = -48; dy <= 48; dy += 2)
				{
					for (int dx = -48; dx <= 48; dx += 2)
					{
						const int d = std::max(std::abs(dx), std::abs(dy));

						if (d >= nearestOreDist)
							continue;

						const CellStruct tc { static_cast<short>(uc.X + dx), static_cast<short>(uc.Y + dy) };
						auto const pC = MapClass::Instance.TryGetCellAt(tc);

						if (!pC || pC->GetContainedTiberiumValue() <= 0)
							continue;

						if (unitZoneForEco >= 0 && MapClass::Instance.GetMovementZoneType(tc, MovementZone::Normal, false) != unitZoneForEco)
							continue;

						nearestOreDist = d;
						pNearestOre = pC;
					}
				}

				if (pNearestOre)
				{
					pUnit->SetDestination(pNearestOre, true);
					pUnit->QueueMission(Mission::Harvest, true);
				}

				continue;
			}

			// IMPORTANT: Never call SetDestination() on Harvesters or Chrono Miners
			// (Teleporter=yes)! In Yuri's Revenge, calling SetDestination() on a
			// ChronoLocomotionClass unit triggers an immediate Chrono-shift and
			// resets UnitClass::Mission_Harvest's internal state machine, causing
			// Allied Chrono Miners to loop-teleport endlessly to the Refinery
			// entrance cell. Vanilla Mission_Harvest natively handles ore/gem search
			// and Chrono return.
			if (pUnit->CurrentMission == Mission::Sleep || pUnit->CurrentMission == Mission::None
				|| ((pUnit->CurrentMission == Mission::Guard || pUnit->CurrentMission == Mission::Area_Guard)
					&& !pUnit->HasAnyLink()))
			{
				pUnit->QueueMission(Mission::Harvest, false);
			}

			continue;
		}

		// Yuri Grinder Economy: Recycle heavily damaged Mind-Controlled enemy
		// vehicles into the Grinder for instant cash!
		if (pGrinder && pUnit->MindControlledBy != nullptr && pUnit->GetHealthPercentage() < 0.45
			&& pUnit->CurrentMission != Mission::Enter)
		{
			OrderFootToEnter(pUnit, pGrinder);

			continue;
		}

		// Auto-deploy MCV if we have no ConYards (ensuring a safe, corridor-clear
		// 4x4 foundation when rebuilt from a War Factory or retreating after
		// emergency undeploy), or march any additional Expansion MCV to an Ore
		// field
		if (pType->DeploysInto && pType->DeploysInto->ConstructionYard)
		{
			// When a ConYard packs up via UndeployBuildingToVehicle, the newly spawned MCV
			// starts in Mission::Guard with its migration goal stored in ArchiveTarget.
			// Promote ArchiveTarget to an active Move destination on frame 1 so it never
			// immediately unpacks on the exact same cell!
			if (pUnit->ArchiveTarget != nullptr && pUnit->CurrentMission != Mission::Move
				&& pUnit->CurrentMission != Mission::Enter && pUnit->QueuedMission != Mission::Move
				&& pUnit->QueuedMission != Mission::Enter && pUnit->DistanceFrom(pUnit->ArchiveTarget) > 2048)
			{
				AbstractClass* const pArchDest = pUnit->ArchiveTarget;
				pUnit->SetArchiveTarget(nullptr);

				const int packedZone = ObjectZone(pUnit, MovementZone::Normal);
				const int goalZone = MapClass::Instance.GetMovementZoneType(
					CellClass::Coord2Cell(pArchDest->GetCoords()), MovementZone::Normal, false);

				// Goal across water (a yard packed up on another island to go home, or a nomad
				// heading to an island): ask for a transport like any other unit, instead of a
				// ground move it can never finish.
				if (packedZone >= 0 && goalZone >= 0 && packedZone != goalZone && CanHouseFerryTypeAcrossWater(pHouse, pType))
				{
					DispatchFootToTargetOrTransport(pUnit, pArchDest, Mission::Move, false);
					auto& ferryingList = GetMacroState(pHouse).FerryingMcvs;

					if (ferryingList.size() >= 16)
						ferryingList.erase(ferryingList.begin()); // Lost MCVs never leave by themselves.

					ferryingList.push_back(pUnit->UniqueID);
				}
				else
				{
					pUnit->SetDestination(pArchDest, true);
					pUnit->ForceMission(Mission::Move);
				}
			}

			// A packed-up yard MCV sent across water (FerryingMcvs) keeps asking the transport
			// controller each tick and skips the "new island without a ConYard" deploy rule, which
			// would unpack it on the spot. Other island MCVs keep their own boarding logic below.
			{
				auto& ferrying = GetMacroState(pHouse).FerryingMcvs;
				auto const itFerry = std::find(ferrying.begin(), ferrying.end(), pUnit->UniqueID);

				if (itFerry != ferrying.end())
				{
					const CellStruct ferryGoal = LookupPassengerIslandGoal(pUnit->UniqueID);
					const int standZone = ObjectZone(pUnit, MovementZone::Normal);

					if (ferryGoal != CellStruct::Empty && standZone >= 0
						&& MapClass::Instance.GetMovementZoneType(ferryGoal, MovementZone::Normal, false) != standZone
						&& CanHouseFerryTypeAcrossWater(pHouse, pType))
					{
						if (pUnit->CurrentMission != Mission::Enter && pUnit->QueuedMission != Mission::Enter)
						{
							if (auto const pFerryGoalCell = MapClass::Instance.TryGetCellAt(ferryGoal))
								DispatchFootToTargetOrTransport(pUnit, pFerryGoalCell, Mission::Move, false);
						}

						continue;
					}

					// Landed on the goal's landmass, or the goal expired: normal MCV logic again.
					ferrying.erase(itFerry);
				}
			}

			const CellStruct mcvCell = pUnit->GetMapCoords();
			const bool isForeignTechMCV = (pHouse->CountOwnedNow(pType->DeploysInto) == 0 && pHouse->NumConYards >= 1);
			bool enemyNearMCV = false;

			for (auto const pEB : BuildingClass::Array)
			{
				if (!pEB || pEB->InLimbo || !pEB->IsAlive || !pEB->Owner || !pEB->Type || pEB->Type->Immune
					|| pEB->Type->Insignificant || pEB->Type->InvisibleInGame)
				{
					continue;
				}

				if (pEB->Owner == pHouse || pHouse->IsAlliedWith(pEB->Owner) || !IsEnemyBuildingKnown(pHouse, pEB) || pEB->Owner->IsNeutral())
					continue;

				const CellStruct ec = pEB->GetMapCoords();
				const int maxBldThreatDist = pEB->Type->IsBaseDefense ? 13 : 11;

				if (std::max(std::abs(mcvCell.X - ec.X), std::abs(mcvCell.Y - ec.Y)) <= maxBldThreatDist)
				{
					enemyNearMCV = true;
					break;
				}
			}

			if (!enemyNearMCV)
			{
				for (auto const pET : TechnoClass::Array)
				{
					if (!pET || pET->InLimbo || !pET->IsAlive || !pET->Owner
						|| pET->WhatAmI() == AbstractType::Building)
					{
						continue;
					}

					if (pET->Owner == pHouse || pHouse->IsAlliedWith(pET->Owner) || !IsCellVisibleToAI(pHouse, pET->GetCell()) || pET->Owner->IsNeutral())
						continue;

					auto const pETType = pET->GetTechnoType();
					auto const pETUnitType = abstract_cast<UnitTypeClass*>(pETType);

					if (!pETType || (pETUnitType && pETUnitType->Harvester) || pETType->ResourceGatherer
						|| IsAmphibiousOrAirTransport(pETType))
					{
						continue;
					}

					auto const pEWpn = pET->GetWeapon(0) ? pET->GetWeapon(0)->WeaponType : nullptr;

					if (!pEWpn
						|| (pETType->Naval && (pEWpn->Range <= 2048 || (pEWpn->Projectile && !pEWpn->Projectile->AG))))
					{
						continue;
					}

					const CellStruct etc = pET->GetMapCoords();

					if (std::max(std::abs(mcvCell.X - etc.X), std::abs(mcvCell.Y - etc.Y)) <= 12)
					{
						enemyNearMCV = true;
						break;
					}
				}
			}

			const int mcvZone = MapClass::Instance.GetMovementZoneType(mcvCell, MovementZone::Normal, pUnit->OnBridge);
			int minAnchorDist = 9999;
			int conYardsInMcvZone = 0;
			int hubsAndFactoriesInMcvZone = 0;
			CellStruct newestYard = pHouse->BaseCenter != CellStruct::Empty ? pHouse->BaseCenter : mcvCell;

			for (auto const pOwnBld : pHouse->Buildings)
			{
				if (!pOwnBld || pOwnBld->InLimbo || !pOwnBld->IsAlive || !pOwnBld->Type || pOwnBld->Type->Insignificant
					|| pOwnBld->Type->InvisibleInGame)
				{
					continue;
				}

				if (!pOwnBld->Type->ConstructionYard && !pOwnBld->Type->Refinery && !pOwnBld->Type->BaseNormal
					&& pOwnBld->Type->Factory == AbstractType::None)
				{
					continue;
				}

				const CellStruct bc = pOwnBld->GetMapCoords();
				const int bz = MapClass::Instance.GetMovementZoneType(bc, MovementZone::Normal, pOwnBld->OnBridge);

				if (pOwnBld->Type->ConstructionYard)
				{
					newestYard = bc;

					if (bz == mcvZone)
						conYardsInMcvZone++;
				}

				if (mcvZone >= 0 && bz != mcvZone)
					continue;

				if (pOwnBld->Type->Refinery || pOwnBld->Type->ResourceDestination
					|| pOwnBld->Type->Factory != AbstractType::None || pOwnBld->Type->BaseNormal)
				{
					hubsAndFactoriesInMcvZone++;
				}

				const int d = std::max(std::abs(mcvCell.X - bc.X), std::abs(mcvCell.Y - bc.Y));

				if (d < minAnchorDist)
					minAnchorDist = d;
			}

			const bool canFerryThisMCV = CanHouseFerryTypeAcrossWater(pHouse, pType);
			bool ownsLiveMcvTransportNow = false;

			for (auto const pT : UnitClass::Array)
			{
				if (pT && pT->Owner == pHouse && !pT->InLimbo && pT->IsAlive && pT->Type
					&& IsAmphibiousOrAirTransport(pT->Type)
					&& static_cast<int>(pT->Type->SizeLimit) >= static_cast<int>(pType->TechnoTypeClass::Size)
					&& pT->Type->Passengers >= static_cast<int>(pType->TechnoTypeClass::Size))
				{
					ownsLiveMcvTransportNow = true;
					break;
				}
			}

			const bool isMovingToDestEarly
				= ((pUnit->CurrentMission == Mission::Move || pUnit->QueuedMission == Mission::Move)
					&& pUnit->Destination != nullptr && pUnit->DistanceFrom(pUnit->Destination) > 1280)
				|| pUnit->CurrentMission == Mission::Enter || pUnit->QueuedMission == Mission::Enter;

			// Keep a nomadic single MCV undeployed while it is actively migrating to a distant
			// resource expansion or boarding/sailing on a live transport to a resource island
			const bool isNomadicSingleMCVHop = (pHouse->NumConYards == 0 && hubsAndFactoriesInMcvZone >= 3
				&& pHouse->NumWarFactories >= 1 && !enemyNearMCV
				&& ((ownsLiveMcvTransportNow && pIslandResourceGoalCell != nullptr && mcvZone != resourceIslandZone)
					|| isMovingToDestEarly));

			if (pHouse->NumConYards == 0 && !isNomadicSingleMCVHop)
			{
				auto const pYardType = pType->DeploysInto;
				if (!enemyNearMCV && CanDeployerUnpackHere(pHouse, pYardType, pUnit))
				{
					OrderDeployerUnpack(pUnit);
					continue;
				}

				const bool reachedOrNoDest = (pUnit->CurrentMission != Mission::Move || pUnit->Destination == nullptr
					|| CellClass::Coord2Cell(pUnit->Destination->GetCoords()) == mcvCell);
				if (reachedOrNoDest || (IsAITickWindow(45, 15)))
				{
					const CellStruct safeYardStandCell = FindBestDeployStandCell(pHouse, pYardType, pUnit, mcvZone);
					if (safeYardStandCell == mcvCell && !enemyNearMCV
						&& CanDeployerUnpackHere(pHouse, pYardType, pUnit))
					{
						OrderDeployerUnpack(pUnit);
					}
					else if (safeYardStandCell != CellStruct::Empty)
					{
						if (auto const pSafeDest = MapClass::Instance.TryGetCellAt(safeYardStandCell))
						{
							IssueMoveOrder(pUnit, pSafeDest, 3.0);
						}
					}
					else if (!enemyNearMCV)
					{
						pUnit->Scatter(CoordStruct::Empty, true, false);
					}
				}

				continue;
			}

			// Slot 82 (Stolen / Foreign MCV Safe Extraction, Island Transport
			// Evacuation & Home Base Deployment): If the stolen MCV is on a
			// disconnected island (mcvZone != homeGroundZone), board a nearby
			// Amphibious Transport (or move to the safest pickup beach while a
			// transport sails over to rescue it) to ferry it back to our safe Home
			// Island! If on the same landmass and inside a hostile enemy zone
			// (enemyNearMCV) OR outside our home base (> 16 cells from homeCoords),
			// evacuate it by land back to our safe home base! Once the stolen
			// foreign-faction MCV reaches our safe home base (<= 16 cells from
			// homeCoords and !enemyNearMCV), deploy it safely inside our base
			// perimeter so we can build its foreign tech tree!
			const bool landedOnNewIsland = (homeGroundZone >= 0 && mcvZone >= 0 && mcvZone != homeGroundZone
				&& conYardsInMcvZone == 0 && hubsAndFactoriesInMcvZone < 2);
			if (SlotGate(scores[82], 60, 82) && homeCoords != CellStruct::Empty && !landedOnNewIsland
				&& (enemyNearMCV || isForeignTechMCV))
			{
				const bool onDisconnectedEnemyIsland
					= (homeGroundZone >= 0 && mcvZone >= 0 && mcvZone != homeGroundZone);

				if (onDisconnectedEnemyIsland && isForeignTechMCV && CanHouseFerryTypeAcrossWater(pHouse, pType))
				{
					UnitClass* pRescueTrans = nullptr;
					int bestRescueDist = 999999;

					for (auto const pT : UnitClass::Array)
					{
						if (!pT || pT->Owner != pHouse || pT->InLimbo || !pT->IsAlive || !pT->Type
							|| !IsAmphibiousOrAirTransport(pT->Type))
						{
							continue;
						}

						if (pType->TechnoTypeClass::Size > pT->Type->SizeLimit
							|| (pT->Passengers.GetTotalSize() + static_cast<int>(pType->TechnoTypeClass::Size))
								> pT->Type->Passengers)
						{
							continue;
						}

						const int d = pUnit->DistanceFrom(pT);

						if (d < bestRescueDist)
						{
							bestRescueDist = d;

							pRescueTrans = pT;
						}
					}

					if (pRescueTrans && bestRescueDist <= 3584)
					{
						if (pRescueTrans->CurrentMission == Mission::Move
							|| (pRescueTrans->Destination != nullptr
								&& !HasActiveBoardingPassenger(pRescueTrans, mcvZone)))
						{
							pRescueTrans->SetDestination(nullptr, true);
							pRescueTrans->ClearNavigationList();
							pRescueTrans->ForceMission(Mission::Guard);
						}

						OrderFootToEnter(pUnit, pRescueTrans);

						continue;
					}

					if (pUnit->CurrentMission != Mission::Move || (IsAITickWindow(60, 15)))
					{
						if (auto const pPickupBeach = FindSafeIslandLandingCell(pHouse, mcvCell, mcvZone, false))
						{
							IssueMoveOrder(pUnit, pPickupBeach, 4.0);
						}
					}

					continue;
				}

				const double distToHome = mcvCell.DistanceFrom(homeCoords);

				if (enemyNearMCV || (isForeignTechMCV && distToHome > 16.0))
				{
					if (pUnit->CurrentMission != Mission::Move || (IsAITickWindow(60, 15)))
					{
						const CellStruct safeHomeCell
							= CachedTetrisBuildCell(pHouse, pType->DeploysInto);

						const CellStruct goalCoords = (safeHomeCell != CellStruct::Empty) ? safeHomeCell : homeCoords;

						if (auto const pHomeDest = MapClass::Instance.TryGetCellAt(goalCoords))
						{
							IssueMoveOrder(pUnit, pHomeDest, 3.0);
						}
					}

					continue;
				}

				if (isForeignTechMCV && !enemyNearMCV
					&& (distToHome <= 16.0
						|| (onDisconnectedEnemyIsland && !CanHouseFerryTypeAcrossWater(pHouse, pType))))
				{
					if (CanDeployerUnpackHere(pHouse, pType->DeploysInto, pUnit))
					{
						OrderDeployerUnpack(pUnit);
					}
					else
					{
						const CellStruct clearCell
							= FindBestDeployStandCell(pHouse, pType->DeploysInto, pUnit, mcvZone);
						if (auto const pClear = MapClass::Instance.TryGetCellAt(clearCell))
						{
							pUnit->SetDestination(pClear, true);
							pUnit->QueueMission(Mission::Move, true);
						}
					}

					continue;
				}
			}

			const bool isMovingToDest = (pUnit->CurrentMission == Mission::Move && pUnit->Destination != nullptr
				&& pUnit->DistanceFrom(pUnit->Destination) > 768);
			const bool isSecondaryIsland = (homeGroundZone >= 0 && mcvZone >= 0 && mcvZone != homeGroundZone);
			const bool isArchipelagoOrNavalMap = (pIslandResourceGoalCell != nullptr || pIslandTechGoalCell != nullptr
				|| pHouse->NumShipyards > 0 || pLoadingTransport != nullptr || isSecondaryIsland);
			// A free ore field on this landmass is the next expansion: the MCV goes there instead of
			// waiting at a beach for a ferry to another island.
			const bool hasFreeLandFieldInMcvZone = !isForeignTechMCV && mcvZone >= 0
				&& FindFreeOreFieldCell(pHouse, mcvCell, mcvZone, 14) != nullptr;
			const bool alreadyHasConYardOnThisIsland = (!isForeignTechMCV && !hasFreeLandFieldInMcvZone
				&& ((isSecondaryIsland && conYardsInMcvZone >= 1) || (isArchipelagoOrNavalMap && conYardsInMcvZone >= 1)
					|| conYardsInMcvZone >= 2));
			const bool currentIslandAlreadyDeveloped = (conYardsInMcvZone >= 1
				|| (hubsAndFactoriesInMcvZone >= 2
					&& (mcvZone != homeGroundZone || pHouse->NumConYards >= 1 || !hasUncolonizedOreOnHomeIsland
						|| pHouse->OwnedBuildings >= 6)));
			// A landmass with a ConYard sends its expansion MCV to the free island (the uncolonized
			// one in pIslandResourceGoalCell) even while fields remain on it: a big island always has
			// one, and the island was never colonized. Once that island holds a ConYard it is no
			// longer the goal and the next MCVs take the fields at home again.
			const bool islandExpansionDue = conYardsInMcvZone >= 1 && pIslandResourceGoalCell != nullptr
				&& mcvZone != resourceIslandZone;
			const bool mustColonizeOtherIsland = (canFerryThisMCV && mcvZone >= 0
				&& (!hasFreeLandFieldInMcvZone || islandExpansionDue)
				&& (currentIslandAlreadyDeveloped || alreadyHasConYardOnThisIsland) && !isForeignTechMCV
				&& ((pIslandResourceGoalCell != nullptr && mcvZone != resourceIslandZone)
					|| alreadyHasConYardOnThisIsland));
			const bool wantsMultiConYardSpeed = (pHouse->NumConYards >= 1 && !mustColonizeOtherIsland
				&& !alreadyHasConYardOnThisIsland && !isArchipelagoOrNavalMap
				&& ((scores[30] > 650 && scores[35] <= 60 && scores[81] <= 90)
					|| (!canFerryThisMCV && !hasUncolonizedOreOnHomeIsland)
					|| GetMacroState(pHouse).BankWantedYards > pHouse->NumConYards));
			// Below the 14 cells the expansion destinations keep from our hubs: an MCV that reached a
			// field 14..15 cells from a power plant never deployed and wandered from field to field.
			const int requiredAnchorDist = landedOnNewIsland ? 0 : (wantsMultiConYardSpeed ? 8 : 12);
			// Two expansion MCVs that reach the same field together both saw no ConYard there and
			// deployed side by side. The one with the lower UniqueID deploys first; this one waits
			// until that ConYard stands and then picks another field.
			bool twinMcvNearby = false;

			for (auto const pOtherMcv : UnitClass::Array)
			{
				if (pOtherMcv && pOtherMcv != pUnit && pOtherMcv->Owner == pHouse && pOtherMcv->IsAlive && !pOtherMcv->InLimbo
					&& pOtherMcv->Type && pOtherMcv->Type->DeploysInto && pOtherMcv->Type->DeploysInto->ConstructionYard
					&& pOtherMcv->UniqueID < pUnit->UniqueID)
				{
					const CellStruct oc = pOtherMcv->GetMapCoords();

					if (std::max(std::abs(oc.X - mcvCell.X), std::abs(oc.Y - mcvCell.Y)) <= 12)
					{
						twinMcvNearby = true;
						break;
					}
				}
			}

			if (mustColonizeOtherIsland)
			{
				const bool isSelfAmphibOrAir = (pType->MovementZone == MovementZone::Amphibious
					|| pType->MovementZone == MovementZone::AmphibiousDestroyer
					|| pType->MovementZone == MovementZone::AmphibiousCrusher
					|| pType->MovementZone == MovementZone::Fly || pType->JumpJet);
				if (isSelfAmphibOrAir && pIslandResourceGoalCell)
				{
					if (!isMovingToDest || (IsAITickWindow(90, 25)))
					{
						IssueMoveOrder(pUnit, pIslandResourceGoalCell, 3.0);
					}
				}
				else
				{
					ClearStaleEnterOrUnreachableCrossIslandMission(pUnit, false);
					if (pUnit->CurrentMission == Mission::Enter || pUnit->QueuedMission == Mission::Enter)
					{
						// Already actively boarding a valid transport: do NOT overwrite or
						// spam orders!
					}
					else
					{
						UnitClass* pBeachedMcvTrans = nullptr;
						int bestTransDist = 999999;
						for (auto const pT : UnitClass::Array)
						{
							if (!pT || pT->Owner != pHouse || pT->InLimbo || !pT->IsAlive || !pT->Type
								|| !IsAmphibiousOrAirTransport(pT->Type))
							{
								continue;
							}

							if (static_cast<int>(pType->TechnoTypeClass::Size) > static_cast<int>(pT->Type->SizeLimit)
								|| (pT->Passengers.GetTotalSize() + static_cast<int>(pType->TechnoTypeClass::Size))
									> pT->Type->Passengers)
							{
								continue;
							}

							auto const pTCell = pT->GetCell();
							const int tz = ObjectZone(pT, MovementZone::Normal);
							if (tz == mcvZone && pTCell && pTCell->LandType != LandType::Water)
							{
								const int d = pUnit->DistanceFrom(pT);
								if (d < bestTransDist)
								{
									bestTransDist = d;
									pBeachedMcvTrans = pT;
								}
							}
						}

						if (pBeachedMcvTrans && !HasActiveBoardingPassenger(pBeachedMcvTrans, mcvZone))
						{
							if (pBeachedMcvTrans->CurrentMission == Mission::Move
								|| pBeachedMcvTrans->Destination != nullptr)
							{
								pBeachedMcvTrans->SetDestination(nullptr, true);
								pBeachedMcvTrans->ClearNavigationList();
								pBeachedMcvTrans->ForceMission(Mission::Guard);
							}

							OrderFootToEnter(pUnit, pBeachedMcvTrans);
						}
						else if (!isMovingToDest)
						{
							if (!NeuralAI::IsCellClearedOfExitCorridors(pHouse, pType->DeploysInto, mcvCell))
							{
								pUnit->Scatter(CoordStruct::Empty, true, false);
							}
							else if (auto const pIslandPickupBeach
								= FindSafeIslandLandingCell(pHouse, mcvCell, mcvZone, false))
							{
								if (mcvCell.DistanceFrom(pIslandPickupBeach->MapCoords) > 8.0)
								{
									IssueMoveOrder(pUnit, pIslandPickupBeach, 4.0);
								}
							}
						}
					}
				}
			}
			else if (!twinMcvNearby && (landedOnNewIsland || (!isMovingToDest && minAnchorDist >= requiredAnchorDist && !enemyNearMCV)))
			{
				if (CanDeployerUnpackHere(pHouse, pType->DeploysInto, pUnit))
				{
					OrderDeployerUnpack(pUnit);
				}
				else
				{
					const CellStruct deployCell
						= FindDeployableFoundationNearCell(pHouse, pType->DeploysInto, mcvCell, mcvZone, pUnit);
					if (deployCell != CellStruct::Empty)
					{
						if (!isMovingToDest || (IsAITickWindow(45, 15)))
						{
							if (auto const pClear = MapClass::Instance.TryGetCellAt(deployCell))
							{
								pUnit->SetDestination(pClear, true);
								pUnit->QueueMission(Mission::Move, true);
							}
						}
					}
					else
					{
						pUnit->Scatter(CoordStruct::Empty, true, false);
					}
				}
			}
			else if (!isMovingToDest
				&& (pUnit->CurrentMission == Mission::Guard || pUnit->CurrentMission == Mission::Area_Guard
					|| pUnit->CurrentMission == Mission::Move))
			{
				CellClass* pExpansionDest = nullptr;
				// Slot 81 (Forward Siege Outpost / Prudent Standoff Distance from
				// Enemy Base): Find the nearest enemy structure on the same landmass
				// (mcvZone) and compute a safe standoff position
				// (~16 cells outside the enemy perimeter, >= 14 cells from our existing
				// hubs, and >= 12 cells from enemy turrets) so the MCV can deploy a
				// forward Outpost (War Factory, Barracks, Refinery, or
				// Prism/Obelisk/Grand Cannon turret push)!
				if (SlotGate(scores[81], 75, 81) && (pHouse->CountResourceDestinations >= 1 || pHouse->OwnedBuildings >= 5))
				{
					BuildingClass* pSameZoneEnemyBld = nullptr;
					int bestEnemyDist = 99999;
					for (auto const pEB : BuildingClass::Array)
					{
						if (!pEB || pEB->InLimbo || !pEB->IsAlive || !pEB->Owner || !pEB->Type
							|| pEB->Type->Insignificant || pEB->Type->InvisibleInGame)
						{
							continue;
						}

						if (pEB->Owner == pHouse || pHouse->IsAlliedWith(pEB->Owner) || !IsEnemyBuildingKnown(pHouse, pEB) || pEB->Owner->IsNeutral())
							continue;

						const CellStruct ec = pEB->GetMapCoords();
						if (mcvZone >= 0
							&& MapClass::Instance.GetMovementZoneType(ec, MovementZone::Normal, pEB->OnBridge) != mcvZone)
						{
							continue;
						}

						const int dE = std::max(std::abs(mcvCell.X - ec.X), std::abs(mcvCell.Y - ec.Y));
						if (dE < bestEnemyDist)
						{
							bestEnemyDist = dE;
							pSameZoneEnemyBld = pEB;
						}
					}

					if (pSameZoneEnemyBld && bestEnemyDist >= 20)
					{
						const CellStruct ec = pSameZoneEnemyBld->GetMapCoords();
						const int dxE = mcvCell.X - ec.X;
						const int dyE = mcvCell.Y - ec.Y;
						const int distE = std::max(1, std::max(std::abs(dxE), std::abs(dyE)));
						for (int standoffDist = 16; standoffDist <= 22 && !pExpansionDest; standoffDist += 2)
						{
							const CellStruct siegeCenter { static_cast<short>(ec.X + (dxE * standoffDist) / distE),
								static_cast<short>(ec.Y + (dyE * standoffDist) / distE) };
							const CellStruct deployStand = FindDeployableFoundationNearCell(
								pHouse, pType->DeploysInto, siegeCenter, mcvZone, pUnit);
							const CellStruct candStage = (deployStand != CellStruct::Empty) ? deployStand : siegeCenter;
							auto const pSC = MapClass::Instance.TryGetCellAt(candStage);
							if (!pSC
								|| !pSC->IsClearToMove(
									pType->SpeedType, true, true, -1, pType->MovementZone, -1, false))
							{
								continue;
							}

							if (mcvZone >= 0
								&& MapClass::Instance.GetMovementZoneType(candStage, MovementZone::Normal, false)
									!= mcvZone)
							{
								continue;
							}

							bool nearOwnHubOrEnemyTurret = false;
							for (auto const pOwnBld : pHouse->Buildings)
							{
								if (!pOwnBld || pOwnBld->InLimbo || !pOwnBld->IsAlive || !pOwnBld->Type
									|| pOwnBld->Type->Insignificant || pOwnBld->Type->InvisibleInGame)
								{
									continue;
								}

								if (!pOwnBld->Type->ConstructionYard && !pOwnBld->Type->Refinery
									&& !pOwnBld->Type->ResourceDestination && !pOwnBld->Type->BaseNormal
									&& pOwnBld->Type->Factory == AbstractType::None)
								{
									continue;
								}

								const CellStruct bc = pOwnBld->GetMapCoords();
								if (std::max(std::abs(candStage.X - bc.X), std::abs(candStage.Y - bc.Y)) < 14)
								{
									nearOwnHubOrEnemyTurret = true;
									break;
								}
							}

							for (auto const pEB : BuildingClass::Array)
							{
								if (nearOwnHubOrEnemyTurret)
									break;

								if (!pEB || pEB->InLimbo || !pEB->IsAlive || !pEB->Owner || !pEB->Type
									|| pEB->Owner == pHouse || pHouse->IsAlliedWith(pEB->Owner) || !IsEnemyBuildingKnown(pHouse, pEB)
									|| pEB->Owner->IsNeutral())
								{
									continue;
								}

								const CellStruct ebc = pEB->GetMapCoords();
								if (std::max(std::abs(candStage.X - ebc.X), std::abs(candStage.Y - ebc.Y)) < 12)
								{
									nearOwnHubOrEnemyTurret = true;
									break;
								}
							}

							if (!nearOwnHubOrEnemyTurret)
								pExpansionDest = pSC;
						}
					}
				}

				// Resource claim (UpdateResourceClaim): the field the house picked by value and risk,
				// when it is on this landmass, 16+ cells from every ConYard of ours and 12+ from any
				// building of ours (the MCV can deploy there).
				if (!pExpansionDest)
				{
					auto const& claimMacro = GetMacroState(pHouse);
					const CellStruct claimCell = claimMacro.ResourceClaimCell;

					if (claimCell != CellStruct::Empty && mcvZone >= 0 && claimMacro.ResourceClaimZone == mcvZone)
					{
						bool deployableThere = true;

						for (int yi = 0; yi < pHouse->ConYards.Count && deployableThere; ++yi)
						{
							if (auto const pYard = pHouse->ConYards.GetItem(yi))
							{
								const CellStruct yc = pYard->GetMapCoords();
								deployableThere = std::max(std::abs(yc.X - claimCell.X), std::abs(yc.Y - claimCell.Y)) >= 16;
							}
						}

						for (auto const pOwnBld : pHouse->Buildings)
						{
							if (!deployableThere)
								break;

							if (pOwnBld && pOwnBld->IsAlive && !pOwnBld->InLimbo)
							{
								const CellStruct bc = pOwnBld->GetMapCoords();
								deployableThere = std::max(std::abs(bc.X - claimCell.X), std::abs(bc.Y - claimCell.Y)) >= 12;
							}
						}

						if (deployableThere)
							pExpansionDest = MapClass::Instance.TryGetCellAt(claimCell);
					}
				}

				// Distant Uncolonized Ore / Difficult Map Zone Expansion (scan up to
				// [-128, 128] across the landmass):
				// Nearest worthy field (>= 4 ore cells in 5x5) over the whole scan, not the first row
				// that has any ore.
				int bestExpDist = 99999;
				bool teamYieldedSite = false;
				const bool siegeDestFound = (pExpansionDest != nullptr);
				for (int dy = -128; dy <= 128 && !siegeDestFound; dy += (std::abs(dy) <= 56 ? 3 : 4))
				{
					for (int dx = -128; dx <= 128; dx += (std::abs(dx) <= 56 ? 3 : 4))
					{
						const CellStruct tc { static_cast<short>(mcvCell.X + dx), static_cast<short>(mcvCell.Y + dy) };
						auto const pCell = MapClass::Instance.TryGetCellAt(tc);
						if (!pCell || pCell->GetContainedTiberiumValue() <= 0)
							continue;

						const int candZone = MapClass::Instance.GetMovementZoneType(tc, MovementZone::Normal, false);
						if (mcvZone >= 0 && candZone != mcvZone)
							continue;

						bool tooCloseToExistingHub = false;
						for (auto const pOwnBld : pHouse->Buildings)
						{
							if (!pOwnBld || pOwnBld->InLimbo || !pOwnBld->IsAlive || !pOwnBld->Type
								|| pOwnBld->Type->Insignificant || pOwnBld->Type->InvisibleInGame)
							{
								continue;
							}

							if (!pOwnBld->Type->ConstructionYard && !pOwnBld->Type->Refinery
								&& !pOwnBld->Type->ResourceDestination && !pOwnBld->Type->BaseNormal
								&& pOwnBld->Type->Factory == AbstractType::None)
							{
								continue;
							}

							const CellStruct bc = pOwnBld->GetMapCoords();
							if (MapClass::Instance.GetMovementZoneType(bc, MovementZone::Normal, pOwnBld->OnBridge) != candZone)
								continue;

							if (std::max(std::abs(tc.X - bc.X), std::abs(tc.Y - bc.Y)) < 14)
							{
								tooCloseToExistingHub = true;
								break;
							}
						}

						if (tooCloseToExistingHub)
							continue;

						const int distFromMCV = std::abs(tc.X - mcvCell.X) + std::abs(tc.Y - mcvCell.Y);

						// Team blackboard: not a field an ally mines or a teammate's MCV is heading to.
						if (distFromMCV < bestExpDist && IsExpansionCellTakenByTeam(pHouse, tc))
						{
							teamYieldedSite = true;
							continue;
						}

						if (distFromMCV < bestExpDist)
						{
							int fieldCells = 0;

							for (int fy = -2; fy <= 2; ++fy)
							{
								for (int fx = -2; fx <= 2; ++fx)
								{
									auto const pFC = MapClass::Instance.TryGetCellAt(
										CellStruct { static_cast<short>(tc.X + fx), static_cast<short>(tc.Y + fy) });

									if (pFC && pFC->GetContainedTiberiumValue() > 0)
										fieldCells++;
								}
							}

							if (fieldCells < 4)
								continue;

							bestExpDist = distFromMCV;
							pExpansionDest = pCell;
						}
					}
				}

				// Nothing in the local scan: the nearest free field anywhere on our landmass.
				if (!pExpansionDest)
					pExpansionDest = FindFreeOreFieldCell(pHouse, mcvCell, mcvZone, 14);

				if (teamYieldedSite)
					CountDecision(NeuralDecision::TeamExpansionYield, pHouse->ArrayIndex);

				if (pExpansionDest)
				{
					IssueMoveOrder(pUnit, pExpansionDest, 3.0);
					NoteTeamExpansionSite(pHouse, pExpansionDest->MapCoords);
				}
				else if (!enemyNearMCV && !alreadyHasConYardOnThisIsland && !twinMcvNearby)
				{
					if (CanDeployerUnpackHere(pHouse, pType->DeploysInto, pUnit))
					{
						OrderDeployerUnpack(pUnit);
					}
					else
					{
						const CellStruct localYardCell
							= FindBestDeployStandCell(pHouse, pType->DeploysInto, pUnit, mcvZone);
						if (localYardCell != CellStruct::Empty)
						{
							if (auto const pLocalDest = MapClass::Instance.TryGetCellAt(localYardCell))
							{
								IssueMoveOrder(pUnit, pLocalDest, 3.0);
							}
						}
						else
						{
							pUnit->Scatter(CoordStruct::Empty, true, false);
						}
					}
				}
			}

			continue;
		}

		// Factory deployer (NeuralAI.MobileFactory.inl): drive to its spot and unfold there. A mobile
		// factory (it can pack up again) re-checks the spot on the way; a one-way deployer picks it
		// once. With no spot it unfolds where it stands, like any base deployer, unless it just fled:
		// then it heads for the place it fled to first.
		if (IsFactoryDeployerUnit(pType))
		{
			if (pUnit->IsDeploying || pUnit->CurrentMission == Mission::Unload || pUnit->CurrentMission == Mission::Enter
				|| pUnit->QueuedMission == Mission::Enter)
			{
				continue;
			}

			auto& mobileMacro = GetMacroState(pHouse);
			const int now = Unsorted::CurrentFrame;
			const CellStruct unitCell = pUnit->GetMapCoords();
			const int unitZone = ObjectZone(pUnit, MovementZone::Normal);
			auto const pFactoryType = pType->DeploysInto;
			const bool isMobile = IsMobileFactoryUnit(pType);

			if (mobileMacro.MobileFactoryGoals.size() > 16)
				std::erase_if(mobileMacro.MobileFactoryGoals, [now](const auto& entry) { return now - entry.second.ProgressFrame > 9000; });

			auto& goal = mobileMacro.MobileFactoryGoals[pUnit->UniqueID];
			const bool firstVisit = goal.StartFrame < 0;

			if (firstVisit)
			{
				goal.StartFrame = now;
				goal.ProgressFrame = now;

				// The vehicle a mobile factory just packed into keeps the goal it was packed up for: picked
				// afresh, without its building, the spot it left could look empty and win again.
				if (isMobile && now - mobileMacro.MobileFactoryLastPackFrame <= 300
					&& mobileMacro.MobileFactoryPackCell != CellStruct::Empty
					&& unitCell.DistanceFrom(mobileMacro.MobileFactoryPackCell) <= 5.0)
				{
					goal.Cell = mobileMacro.MobileFactoryPackTarget;
					goal.Reason = mobileMacro.MobileFactoryPackReason;
					goal.PickFrame = now;
					goal.HoldUntil = now + 1800;
				}
			}

			if (isMobile)
				mobileMacro.MobileFactoryVehicleSeenFrame = now;

			// A packed mobile factory caught by more than our forces around it turns back for a quiet
			// hub, as a deployed one would flee.
			if (isMobile && now >= goal.HoldUntil && IsAITickWindow(90, 15))
			{
				const int threat = VisibleEnemyThreatNear(pHouse, unitCell, 8);

				if (threat >= 80 && threat * 4 > OwnDefenseNear(pHouse, unitCell, 8) * 5)
				{
					const CellStruct retreat
						= FindMobileFactoryRetreat(pHouse, unitCell, unitZone, CanHouseFerryTypeAcrossWater(pHouse, pType));

					if (retreat != CellStruct::Empty && retreat.DistanceFrom(unitCell) >= 8.0)
					{
						mobileMacro.MobileFactoryAvoidCell = unitCell;
						mobileMacro.MobileFactoryAvoidFrame = now;
						goal.Cell = retreat;
						goal.Reason = MobileFactoryReasonNone;
						goal.PickFrame = now;
						goal.HoldUntil = now + 1200;
						goal.BestDist = INT_MAX;
						goal.ProgressFrame = now;
						goal.DeployTries = 0;
					}
				}
			}

			const bool holding = now < goal.HoldUntil;

			if ((firstVisit && !holding) || (isMobile && !holding && (now - goal.PickFrame >= 150 || now < goal.PickFrame)))
			{
				const MobileFactorySpot spot
					= FindMobileFactorySpot(pHouse, pFactoryType, nullptr, unitCell, unitZone, pUnit->UniqueID, goal.Cell);
				CellStruct newCell = spot.Cell;

				const bool fledHere = mobileMacro.MobileFactoryAvoidCell != CellStruct::Empty
					&& now - mobileMacro.MobileFactoryAvoidFrame < MobileFactoryAvoidFrames
					&& unitCell.DistanceFrom(mobileMacro.MobileFactoryAvoidCell) <= MobileFactoryAvoidRadius;

				if (newCell == CellStruct::Empty && fledHere)
					newCell = homeCoords;

				if (newCell != goal.Cell)
				{
					goal.BestDist = INT_MAX;
					goal.ProgressFrame = now;
					goal.DeployTries = 0;
				}

				goal.Cell = newCell;
				goal.Reason = spot.Reason;
				goal.PickFrame = now;
			}
			else if (!isMobile || holding)
			{
				// Keep the spot reserved while a one-way deployer, or a vehicle holding the goal it was
				// packed up for, drives there.
				goal.PickFrame = now;
			}

			const int goalDist = goal.Cell != CellStruct::Empty ? static_cast<int>(unitCell.DistanceFrom(goal.Cell)) : 0;

			if (goalDist < goal.BestDist)
			{
				goal.BestDist = goalDist;
				goal.ProgressFrame = now;
			}

			// No cell closer to the spot for 1800 frames (a blocked path, or a ferry that never comes):
			// unfold where it is instead of wandering. Progress, not trip time: these vehicles are slow.
			const bool stalled = now - goal.ProgressFrame > 1800;
			const bool atGoal = goal.Cell == CellStruct::Empty || stalled || goalDist <= 6;
			const bool reachedOrNoDest = pUnit->Destination == nullptr || pUnit->CurrentMission != Mission::Move
				|| CellClass::Coord2Cell(pUnit->Destination->GetCoords()) == unitCell;

			if (atGoal)
			{
				if (CanDeployerUnpackHere(pHouse, pFactoryType, pUnit))
				{
					// Another routine can hold the unit in its own mission so the queued unpack never
					// starts: force it after a few tries. When the engine keeps refusing the spot, step
					// aside and try again from a nearby cell.
					if (goal.DeployTries == 0 || IsAITickWindow(45, 15))
					{
						if (goal.DeployTries >= 12)
						{
							goal.DeployTries = 0;
							pUnit->Scatter(CoordStruct::Empty, true, false);
						}
						else
						{
							OrderDeployerUnpack(pUnit);

							if (++goal.DeployTries >= 3)
								pUnit->ForceMission(Mission::Unload);
						}
					}
				}
				else if (reachedOrNoDest && IsAITickWindow(45, 15))
				{
					// The first deployable foundation near the spot (or here, once stalled) is good.
					const CellStruct origin = (goal.Cell != CellStruct::Empty && !stalled) ? goal.Cell : unitCell;
					CellStruct stand = FindDeployableFoundationNearCell(pHouse, pFactoryType, origin, unitZone, pUnit);

					if (stand == CellStruct::Empty)
						stand = FindBestDeployStandCell(pHouse, pFactoryType, pUnit, unitZone);

					if (stand != unitCell)
					{
						if (auto const pStand = MapClass::Instance.TryGetCellAt(stand))
							IssueMoveOrder(pUnit, pStand, 0.0);
					}
				}
			}
			else if (IsAITickWindow(90, 15))
			{
				if (auto const pGoalCell = MapClass::Instance.TryGetCellAt(goal.Cell))
				{
					const int goalZone = MapClass::Instance.GetMovementZoneType(goal.Cell, MovementZone::Normal, false);

					if (goalZone < 0 || unitZone < 0 || goalZone == unitZone)
						IssueMoveOrder(pUnit, pGoalCell, 4.0);
					else
						DispatchFootToTargetOrTransport(pUnit, pGoalCell, Mission::Move, false);
				}
			}

			continue;
		}

		// Tactical Combat Deployment (Tick Tank, Nod Artillery, Juggernaut, Siege
		// Chopper) & Universal Deployable Key Structures across all mods (Mobile
		// Outposts/Explorers, Mobile Refineries/Factories, Power/Tech/Turret Cores,
		// CABAL Nanocores)!
		if ((pType->DeploysInto && !pType->DeploysInto->ConstructionYard) || pType->DeployFire)
		{
			const bool hasPrimaryWeapon
				= (pType->GetWeapon(0) != nullptr && pType->GetWeapon(0)->WeaponType != nullptr);

			auto const pTargetBld = pType->DeploysInto;

			const bool isKeyStructureDeployer = (pTargetBld != nullptr && IsTrueBaseStructureTarget(pTargetBld)
				&& (!pType->Harvester && !pType->ResourceGatherer || isIslandColonizerSlaveMiner)
				&& (!pTargetBld->IsBaseDefense || !hasPrimaryWeapon));

			if (isKeyStructureDeployer)
			{
				const CellStruct nanoCell = pUnit->GetMapCoords();
				const int nanoZone = MapClass::Instance.GetMovementZoneType(nanoCell, MovementZone::Normal, pUnit->OnBridge);
				const bool isNanoMoving = (pUnit->CurrentMission == Mission::Move && pUnit->Destination != nullptr
					&& pUnit->DistanceFrom(pUnit->Destination) > 768);
				const bool nanoReachedOrNoDest
					= (pUnit->CurrentMission != Mission::Move || pUnit->Destination == nullptr
						|| CellClass::Coord2Cell(pUnit->Destination->GetCoords()) == nanoCell);

				int ownedRefineries = 0;
				int ownedWarFactories = 0;
				int ownedBarracks = 0;
				int minDistFromBaseBld = 9999;
				int minDistFromRefOrYard = 9999;
				int hubsInNanoZone = 0;

				for (auto const pOwnBld : pHouse->Buildings)
				{
					if (!pOwnBld || pOwnBld->InLimbo || !pOwnBld->IsAlive || !pOwnBld->Type
						|| pOwnBld->Type->Insignificant || pOwnBld->Type->InvisibleInGame)
					{
						continue;
					}

					const CellStruct bc = pOwnBld->GetMapCoords();
					const int bz = MapClass::Instance.GetMovementZoneType(bc, MovementZone::Normal, pOwnBld->OnBridge);
					const int d = std::max(std::abs(nanoCell.X - bc.X), std::abs(nanoCell.Y - bc.Y));

					if (nanoZone < 0 || bz == nanoZone)
					{
						if (d < minDistFromBaseBld)
							minDistFromBaseBld = d;

						if (pOwnBld->Type->Refinery || pOwnBld->Type->ConstructionYard
							|| pOwnBld->Type->ResourceDestination || pOwnBld->Type->Factory != AbstractType::None)
						{
							hubsInNanoZone++;
							if (d < minDistFromRefOrYard)
								minDistFromRefOrYard = d;
						}
					}

					if (pOwnBld->Type->Refinery || pOwnBld->Type->ResourceDestination)
						ownedRefineries++;
					else if (pOwnBld->Type->Factory == AbstractType::UnitType && !pOwnBld->Type->Naval)
						ownedWarFactories++;
					else if (pOwnBld->Type->Factory == AbstractType::InfantryType)
						ownedBarracks++;
				}

				const bool isDedicatedMobileOutpost
					= (pTargetBld->BaseNormal && !pTargetBld->Refinery && pTargetBld->Factory == AbstractType::None
						&& pTargetBld->PowerBonus <= pTargetBld->PowerDrain && pHouse->Buildings.Count >= 1);
				const bool canFerryDeployer = CanHouseFerryTypeAcrossWater(pHouse, pType);
				const bool isCrossIslandFactoryDeployer = (canFerryDeployer && pIslandResourceGoalCell != nullptr
					&& nanoZone != resourceIslandZone
					&& ((pTargetBld->Factory == AbstractType::UnitType && !pTargetBld->Naval && ownedWarFactories >= 1)
						|| (pTargetBld->Factory == AbstractType::InfantryType && ownedBarracks >= 1)));

				const bool isExpansionDeployer = isIslandColonizerSlaveMiner || isDedicatedMobileOutpost
					|| isCrossIslandFactoryDeployer
					|| ((pTargetBld->Refinery || pTargetBld->ResourceDestination) && ownedRefineries >= 1
						&& scores[30] > 450);

				if (isExpansionDeployer)
				{
					auto& noRoomIslands = GetMacroState(pHouse).NoRoomIslandUntil;
					const bool nanoIslandHasNoRoom = nanoZone >= 0 && noRoomIslands.contains(nanoZone)
						&& noRoomIslands.at(nanoZone) > Unsorted::CurrentFrame;
					const bool mustColonizeIslandWithDeployer = (canFerryDeployer && nanoZone >= 0
						&& (hubsInNanoZone >= 1 || nanoIslandHasNoRoom) && pIslandResourceGoalCell != nullptr
						&& nanoZone != resourceIslandZone);
					const bool landedOnNewIsland = (homeGroundZone >= 0 && nanoZone >= 0 && nanoZone != homeGroundZone
						&& hubsInNanoZone == 0 && !nanoIslandHasNoRoom);

					// Stranded on an island without room and no other island to go to: back home.
					if (nanoIslandHasNoRoom && !mustColonizeIslandWithDeployer && hubsInNanoZone == 0)
					{
						if ((!isNanoMoving || IsAITickWindow(300, 15)) && homeCoords != CellStruct::Empty)
						{
							if (auto const pHomeCell = MapClass::Instance.TryGetCellAt(homeCoords))
								DispatchFootToTargetOrTransport(pUnit, pHomeCell, Mission::Move, false);
						}

						continue;
					}

					if (mustColonizeIslandWithDeployer)
					{
						const bool isSelfAmphibOrAir = (pType->MovementZone == MovementZone::Amphibious
							|| pType->MovementZone == MovementZone::AmphibiousDestroyer
							|| pType->MovementZone == MovementZone::AmphibiousCrusher
							|| pType->MovementZone == MovementZone::Fly || pType->JumpJet);
						if (isSelfAmphibOrAir && pIslandResourceGoalCell)
						{
							if (!isNanoMoving || (IsAITickWindow(90, 25)))
							{
								IssueMoveOrder(pUnit, pIslandResourceGoalCell, 3.0);
							}
						}
						else
						{
							ClearStaleEnterOrUnreachableCrossIslandMission(pUnit, false);
							if (pUnit->CurrentMission == Mission::Enter || pUnit->QueuedMission == Mission::Enter)
							{
								// Already actively boarding a valid transport: do NOT overwrite
								// or spam orders!
							}
							else
							{
								UnitClass* pBeachedTrans = nullptr;
								int bestTransDist = 999999;
								for (auto const pT : UnitClass::Array)
								{
									if (!pT || pT->Owner != pHouse || pT->InLimbo || !pT->IsAlive || !pT->Type
										|| !IsAmphibiousOrAirTransport(pT->Type))
									{
										continue;
									}

									if (static_cast<int>(pType->TechnoTypeClass::Size)
											> static_cast<int>(pT->Type->SizeLimit)
										|| (pT->Passengers.GetTotalSize()
											+ static_cast<int>(pType->TechnoTypeClass::Size))
											> pT->Type->Passengers)
									{
										continue;
									}

									auto const pTCell = pT->GetCell();
									const int tz = ObjectZone(pT, MovementZone::Normal);
									if (tz == nanoZone && pTCell && pTCell->LandType != LandType::Water)
									{
										const int d = pUnit->DistanceFrom(pT);
										if (d < bestTransDist)
										{
											bestTransDist = d;
											pBeachedTrans = pT;
										}
									}
								}

								if (pBeachedTrans && !HasActiveBoardingPassenger(pBeachedTrans, nanoZone))
								{
									if (pBeachedTrans->CurrentMission == Mission::Move
										|| pBeachedTrans->Destination != nullptr)
									{
										pBeachedTrans->SetDestination(nullptr, true);
										pBeachedTrans->ClearNavigationList();
										pBeachedTrans->ForceMission(Mission::Guard);
									}

									OrderFootToEnter(pUnit, pBeachedTrans);
								}
								else if (!isNanoMoving)
								{
									if (auto const pIslandPickupBeach
										= FindSafeIslandLandingCell(pHouse, nanoCell, nanoZone, false))
									{
										if (nanoCell.DistanceFrom(pIslandPickupBeach->MapCoords) > 8.0)
										{
											IssueMoveOrder(pUnit, pIslandPickupBeach, 4.0);
										}
									}
								}
							}
						}
					}
					else if (isIslandColonizerSlaveMiner)
					{
						// Never force OrderDeployerUnpack on a Slave Miner; let
						// Mission::Harvest manage deployment on ore!
						if (pUnit->CurrentMission != Mission::Harvest && pUnit->CurrentMission != Mission::Enter
							&& pUnit->CurrentMission != Mission::Move)
						{
							pUnit->QueueMission(Mission::Harvest, false);
						}
					}
					else if (landedOnNewIsland || (!isNanoMoving && minDistFromRefOrYard >= 10))
					{
						if (CanDeployerUnpackHere(pHouse, pTargetBld, pUnit))
						{
							OrderDeployerUnpack(pUnit);
						}
						else
						{
							const CellStruct deployCell
								= FindDeployableFoundationNearCell(pHouse, pTargetBld, nanoCell, nanoZone, pUnit);
							if (deployCell != CellStruct::Empty)
							{
								if (nanoReachedOrNoDest || (IsAITickWindow(45, 15)))
								{
									if (auto const pClear = MapClass::Instance.TryGetCellAt(deployCell))
									{
										pUnit->SetDestination(pClear, true);
										pUnit->QueueMission(Mission::Move, true);
									}
								}
							}
							else if (isIslandColonizerSlaveMiner)
							{
								pUnit->QueueMission(Mission::Harvest, false);
							}
							else if (landedOnNewIsland)
							{
								// No foundation for this building anywhere near on this island: the
								// island search skips it for 9000 frames and the deployer moves on.
								noRoomIslands[nanoZone] = Unsorted::CurrentFrame + 9000;
								CountDecision(NeuralDecision::DeployerIslandNoRoom, pHouse->ArrayIndex);
							}
						}
					}
					else if (nanoReachedOrNoDest || (IsAITickWindow(60, 15)))
					{
						CellClass* pExpOreCell = nullptr;
						int bestOreDist = 99999;
						for (int dy = -56; dy <= 56; dy += 3)
						{
							for (int dx = -56; dx <= 56; dx += 3)
							{
								const CellStruct tc { static_cast<short>(nanoCell.X + dx),
									static_cast<short>(nanoCell.Y + dy) };
								auto const pC = MapClass::Instance.TryGetCellAt(tc);
								if (!pC || pC->GetContainedTiberiumValue() <= 0)
									continue;

								const int candZone
									= MapClass::Instance.GetMovementZoneType(tc, MovementZone::Normal, false);
								if (nanoZone >= 0 && candZone != nanoZone)
									continue;

								bool nearExistingRef = false;
								for (auto const pOwnBld : pHouse->Buildings)
								{
									if (!pOwnBld || pOwnBld->InLimbo || !pOwnBld->IsAlive || !pOwnBld->Type
										|| pOwnBld->Type->Insignificant || pOwnBld->Type->InvisibleInGame)
									{
										continue;
									}

									if (pOwnBld->Type->Refinery || pOwnBld->Type->ConstructionYard
										|| pOwnBld->Type->ResourceDestination)
									{
										const CellStruct bc = pOwnBld->GetMapCoords();
										if (MapClass::Instance.GetMovementZoneType(bc, MovementZone::Normal, pOwnBld->OnBridge)
											!= candZone)
										{
											continue;
										}

										if (std::max(std::abs(tc.X - bc.X), std::abs(tc.Y - bc.Y)) < 12)
										{
											nearExistingRef = true;
											break;
										}
									}
								}

								if (nearExistingRef)
									continue;

								const int distFromNano = std::abs(tc.X - nanoCell.X) + std::abs(tc.Y - nanoCell.Y);
								if (distFromNano < bestOreDist)
								{
									bestOreDist = distFromNano;
									pExpOreCell = pC;
								}
							}
						}

						if (pExpOreCell && bestOreDist > 6)
						{
							IssueMoveOrder(pUnit, pExpOreCell, 3.0);
						}
						else if (CanDeployerUnpackHere(pHouse, pTargetBld, pUnit))
						{
							OrderDeployerUnpack(pUnit);
						}
						else
						{
							const CellStruct deployCell = FindBestDeployStandCell(pHouse, pTargetBld, pUnit, nanoZone);
							if (auto const pClear = MapClass::Instance.TryGetCellAt(deployCell))
							{
								pUnit->SetDestination(pClear, true);
								pUnit->QueueMission(Mission::Move, true);
							}
						}
					}

					continue;
				}

				// Defense Turret Deployer (Mobile Turret Cores / ROBOTEGG_OBELISK2,
				// ROBOTEGG_CERBERUS2, etc.): Unpack on a valid foundation clear of
				// factory exit corridors, or move to a free perimeter foundation!
				if (pTargetBld->IsBaseDefense)
				{
					if (minDistFromBaseBld >= 2 && CanDeployerUnpackHere(pHouse, pTargetBld, pUnit))
					{
						OrderDeployerUnpack(pUnit);
						continue;
					}

					if (nanoReachedOrNoDest || (IsAITickWindow(45, 15)))
					{
						const CellStruct perimStandCell = FindBestDeployStandCell(pHouse, pTargetBld, pUnit, nanoZone);
						if (perimStandCell == nanoCell && CanDeployerUnpackHere(pHouse, pTargetBld, pUnit))
						{
							OrderDeployerUnpack(pUnit);
							continue;
						}

						if (auto const pPerim = MapClass::Instance.TryGetCellAt(perimStandCell))
						{
							pUnit->SetDestination(pPerim, true);
							pUnit->QueueMission(Mission::Move, true);
							continue;
						}
					}

					continue;
				}

				// Base Infrastructure Deployer (Power, Barracks, Radar, Tech, Repair
				// Bay, Shield/Gap Generator, or Refinery/Factory): Unpack immediately
				// if current cell has a valid foundation & is clear of factory exit
				// corridors; otherwise search for a free foundation and move there!
				if (CanDeployerUnpackHere(pHouse, pTargetBld, pUnit))
				{
					OrderDeployerUnpack(pUnit);
				}
				else if (nanoReachedOrNoDest || (IsAITickWindow(45, 15)))
				{
					const CellStruct clearStandCell = FindBestDeployStandCell(pHouse, pTargetBld, pUnit, nanoZone);
					if (auto const pClear = MapClass::Instance.TryGetCellAt(clearStandCell))
					{
						pUnit->SetDestination(pClear, true);
						pUnit->QueueMission(Mission::Move, true);
					}
				}

				continue;
			}
			else if (pUnit->Target != nullptr && pUnit->CurrentMission == Mission::Attack)
			{
				OrderDeployerUnpack(pUnit);

				continue;
			}
			else if (pTargetBld != nullptr && !IsTrueBaseStructureTarget(pTargetBld))
			{
				// Tactical Support Deployers (Mobile Stealth Generator SGEN/GGEN,
				// Mobile Sensor Array LPST/NLPST, Limpet Drone): Only deploy when near
				// our frontline lead tank or when an enemy unit is within 10 cells,
				// never right outside the War Factory!
				const bool nearLeadTank = (pLeadHeavyTank != nullptr && pUnit->DistanceFrom(pLeadHeavyTank) <= 1024);
				bool nearEnemyThreat = false;
				for (auto const pEU : UnitClass::Array)
				{
					if (pEU && !pEU->InLimbo && pEU->IsAlive && pEU->Owner && pEU->Owner != pHouse
						&& !pHouse->IsAlliedWith(pEU->Owner) && !pEU->Owner->IsNeutral()
						&& pUnit->DistanceFrom(pEU) <= 2560 && IsCellVisibleToAI(pHouse, pEU->GetCell()))
					{
						nearEnemyThreat = true;
						break;
					}
				}

				if ((nearLeadTank || nearEnemyThreat) && CanDeployerUnpackHere(pHouse, pTargetBld, pUnit))
				{
					OrderDeployerUnpack(pUnit);
					continue;
				}
			}
		}

		// Preserve Elite vehicles (< 60% HP) and expensive vehicles by retreating
		// to Service Depot (Slot 54 adjusts retreat HP & survival reflex)
		const double baseRetreat = std::clamp(0.35 + static_cast<double>(scores[54]) * 0.00025, 0.22, 0.55);
		const double repairThreshold = pUnit->Veterancy.IsElite() ? std::max(0.60, baseRetreat + 0.15) : baseRetreat;

		// Slot 64 (Immediate Service Depot De-Parasiting) & Slot 54 (Repair
		// Retreat): If a Terror Drone or Squid is eating our vehicle
		// (ParasiteEatingMe != nullptr), rush to Service Depot immediately even at
		// 99% HP so the repair arm kills the parasite!
		// Transports are repaired by the ferry controller (only when empty). A damaged valuable
		// unit retreats when the trip pays (tryRepairRetreat: depot, repairer or self-heal); a trip
		// under way is not judged again.
		const bool parasiteToRemove = pUnit->ParasiteEatingMe != nullptr && SlotGate(scores[64], 60, 64);

		// Worth a repair trip: expensive, veteran, or clearly above what our army fields on average
		// (an army of cheap units has no 800 credit tanks to save).
		const bool valuableUnit = pType->Cost >= 800 || pUnit->Veterancy.IsElite() || pUnit->Veterancy.IsVeteran()
			|| (pType->Cost >= 400 && pType->Cost * 4 >= armyAverageCost * 5);

		if (pRepairDepot && !IsAmphibiousOrAirTransport(pType)
			&& (parasiteToRemove || (valuableUnit && pUnit->GetHealthPercentage() < repairThreshold)))
		{
			if (pUnit->CurrentMission == Mission::Enter || isAtOrBoundForRepair(pUnit) || repairTripRecent(pUnit))
				continue;

			if (parasiteToRemove)
			{
				if (auto const pStation = FindBestRepairStation(pHouse, pUnit))
				{
					repairTripsActive++;
					repairTripFrames[pUnit->UniqueID] = repairNow;
					DispatchFootToTargetOrTransport(pUnit, pStation, Mission::Enter, false);

					continue;
				}
			}
		}

		if (valuableUnit && !IsAmphibiousOrAirTransport(pType) && tryRepairRetreat(pUnit, repairThreshold))
			continue;

		// Slot 65 (Grinder Mind-Control Recycling & Salvage): Send mind-controlled
		// vehicles we captured (MindControlledBy != nullptr) into our Grinder for
		// instant cash AND to immediately free our Yuri/Mastermind's control slot!
		if (pGrinderBld && !pType->Naval && SlotGate(scores[65], 80, 65)
			&& (pUnit->MindControlledBy != nullptr
				|| (!pRepairDepot && pUnit->GetHealthPercentage() < 0.18 && pHouse->Available_Money() < 600)))
		{
			if (pUnit->CurrentMission != Mission::Enter)
			{
				DispatchFootToTargetOrTransport(pUnit, pGrinderBld, Mission::Enter, false);
			}

			continue;
		}

		// Slot 67 (ManualReload Ammo Replenishment): Return to reload dock when
		// Ammo == 0
		if (pUnitReloadBld && pType->ManualReload && pType->Ammo > 0 && pUnit->Ammo == 0 && SlotGate(scores[67], 60, 67))
		{
			if (pUnit->CurrentMission != Mission::Enter)
			{
				DispatchFootToTargetOrTransport(pUnit, pUnitReloadBld, Mission::Enter, false);
			}

			continue;
		}

		// Slot 68 (TiberiumHeal Regeneration): Move wounded
		// CyborgReaper/Fiend/Visceroid units onto nearby Tiberium/Ore to heal for
		// free!
		if (pType->TiberiumHeal && pUnit->GetHealthPercentage() < 0.55 && SlotGate(scores[68], 60, 68)
			&& pUnit->CurrentMission != Mission::Move)
		{
			if (pUnit->MoveToTiberium(10, false))
				continue;
		}

		UnstickCrossIslandFoot(pUnit);
		const bool isUnitIdle
			= (pUnit->CurrentMission == Mission::Guard || pUnit->CurrentMission == Mission::Area_Guard)
			&& !IsFootActivelyAttackingOrMovingToEnemy(pUnit);

		const auto pPrimaryWpn = pType->GetWeapon(0) ? pType->GetWeapon(0)->WeaponType : nullptr;

		// Slot 82 (Engineer ConYard Heist Cease-Fire): If our tank is firing at an
		// enemy ConYard that is already in red health (< 35% HP) while our own
		// Engineer is within 10 cells moving in to capture it, hold fire on the
		// ConYard so our Engineer can steal it alive!
		if (ownEngineerNearEnemyConYard && pCapturableEnemyConYard && pUnit->Target == pCapturableEnemyConYard
			&& pCapturableEnemyConYard->GetHealthPercentage() < 0.35 && SlotGate(scores[82], 60, 82))
		{
			pUnit->SetTarget(nullptr);

			pUnit->QueueMission(Mission::Guard, true);

			continue;
		}

		// Slot 71 (Chrono Legionnaire Temporal Breaker): Immediately kill any enemy
		// Chrono Legionnaire erasing our building or tank so it is restored at
		// 100%!
		if (pEnemyTemporalAttacker && pPrimaryWpn && pPrimaryWpn->Damage > 0 && SlotGate(scores[71], 60, 71)
			&& pUnit->DistanceFrom(pEnemyTemporalAttacker) <= 4096)
		{
			DispatchFootToTargetOrTransport(pUnit, pEnemyTemporalAttacker, Mission::Attack, true);

			continue;
		}

		const auto pSecondaryWpn = pType->GetWeapon(1) ? pType->GetWeapon(1)->WeaponType : nullptr;
		const bool hasMobileAAWeapon = (pPrimaryWpn && pPrimaryWpn->Projectile && pPrimaryWpn->Projectile->AA)
			|| (pSecondaryWpn && pSecondaryWpn->Projectile && pSecondaryWpn->Projectile->AA);

		// Slot 74 (Anti-Drain Floating Disc Interceptor & Multi-Flank Mobile AA Defense):
		// Mobile AA units immediately shoot down any enemy Floating Disc draining our buildings,
		// and distribute across multiple simultaneous incoming airborne flanks near our base!
		if (pEnemyDrainer && hasMobileAAWeapon && SlotGate(scores[74], SurvivalMicroVetoScore, 74) && pUnit->DistanceFrom(pEnemyDrainer) <= 4608)
		{
			pUnit->SetTarget(pEnemyDrainer);

			pUnit->QueueMission(Mission::Attack, true);

			continue;
		}

		if (baseAirFlankCount > 0 && hasMobileAAWeapon && pUnit->GetHealthPercentage() >= 0.35)
		{
			const int bestAirIdx = pickAirFlankFor(pUnit, isUnitIdle, 5632);

			if (bestAirIdx >= 0)
			{
				auto const pBestAir = baseAirFlanks[bestAirIdx].Target;
				baseAirFlanks[bestAirIdx].AssignedInterceptors++;
				CountDecision(NeuralDecision::AirIntercept);
				DispatchFootToTargetOrTransport(pUnit, pBestAir, Mission::Attack, true);
				continue;
			}
		}

		// Opportunistic local strike for idle combat vehicles.
		if (isUnitIdle && pPrimaryWpn && !pType->Harvester && !pType->ResourceGatherer
			&& !IsAmphibiousOrAirTransport(pType) && pUnit->GetHealthPercentage() >= 0.45
			&& pUnit->Passengers.NumPassengers == 0)
		{
			if (auto const pLocalTarget = findOpportunityTarget(pUnit, pPrimaryWpn, pSecondaryWpn))
			{
				pUnit->ClearMegaMissionData();
				pUnit->SetTarget(pLocalTarget);
				pUnit->QueueMission(Mission::Attack, true);
				CountDecision(NeuralDecision::LocalStrike);
				continue;
			}
		}

		// Slot 77 (Wounded Frontline Pull-Back): a unit in combat below 38% HP steps 3 cells away
		// from the nearest visible armed ground enemy, but only behind a healthy ally (> 70% HP,
		// within 5 cells) that is at least as close to that enemy, so the enemy has to switch
		// targets. Without such an ally the step would only cost fire.
		if (pUnit->Target != nullptr && !pType->Naval && pUnit->GetHealthPercentage() < 0.38 && SlotGate(scores[77], SurvivalMicroVetoScore, 77)
			&& (IsAITickWindow(45, 15)))
		{
			const CellStruct uc = pUnit->GetMapCoords();
			const LocalCombatContact* pNearestThreat = nullptr;
			double nearestThreatDist = 12.0;

			for (auto const& contact : enemyContacts)
			{
				if (contact.Airborne || contact.Firepower <= 0 || contact.GroundRange <= 0)
					continue;

				const double d = contact.Coords.DistanceFrom(uc);

				if (d < nearestThreatDist)
				{
					nearestThreatDist = d;
					pNearestThreat = &contact;
				}
			}

			bool coveredByAlly = false;

			if (pNearestThreat)
			{
				for (auto const& own : ownCombatContacts)
				{
					if (own.Techno != pUnit && own.Techno->GetHealthPercentage() > 0.70 && own.Coords.DistanceFrom(uc) <= 5.0
						&& own.Coords.DistanceFrom(pNearestThreat->Coords) <= nearestThreatDist + 1.0)
					{
						coveredByAlly = true;
						break;
					}
				}
			}

			if (coveredByAlly)
			{
				const double dx = uc.X - pNearestThreat->Coords.X;
				const double dy = uc.Y - pNearestThreat->Coords.Y;
				const double len = std::max(1.0, std::sqrt(dx * dx + dy * dy));
				CellClass* pBackCell = nullptr;

				for (const double sideBias : { 0.0, 0.6, -0.6 })
				{
					const CellStruct backCoords { static_cast<short>(uc.X + (dx / len - (dy / len) * sideBias) * 3.0),
						static_cast<short>(uc.Y + (dy / len + (dx / len) * sideBias) * 3.0) };
					auto const pCandCell = MapClass::Instance.TryGetCellAt(backCoords);

					if (pCandCell && pCandCell->IsClearToMove(pType->SpeedType, true, true, -1, pType->MovementZone, -1, false))
					{
						pBackCell = pCandCell;
						break;
					}
				}

				if (pBackCell)
				{
					pUnit->SetDestination(pBackCell, true);
					pUnit->QueueMission(Mission::Move, true);

					continue;
				}
			}
		}

		// Slot 79 (Surgical Enemy Power, Superweapon & Factory Snipe): When inside
		// range of a critical enemy structure, prioritize the nearest one over walls/silos!
		if (surgicalEnemyBldCount > 0 && isUnitIdle && pPrimaryWpn && pPrimaryWpn->Damage > 0 && SlotGate(scores[79], 80, 79))
		{
			const int maxSnipeDist = std::max(1280, static_cast<int>(pPrimaryWpn->Range) + 384);
			BuildingClass* pNearestSurgical = nullptr;
			int bestSurgDist = maxSnipeDist + 1;

			for (int si = 0; si < surgicalEnemyBldCount; ++si)
			{
				auto const pCandBld = surgicalEnemyBlds[si];

				if (!pCandBld || pCandBld->InLimbo || !pCandBld->IsAlive)
					continue;

				const int d = pUnit->DistanceFrom(pCandBld);

				if (d <= maxSnipeDist && d < bestSurgDist)
				{
					bestSurgDist = d;
					pNearestSurgical = pCandBld;
				}
			}

			if (pNearestSurgical)
			{
				DispatchFootToTargetOrTransport(pUnit, pNearestSurgical, Mission::Attack, true);
				continue;
			}
		}

		// Slot 80 (Fast Flanker & Jumpjet Counter-Battery vs Enemy Artillery):
		if (pEnemyArtilleryUnit && isUnitIdle && pPrimaryWpn && pPrimaryWpn->Damage > 0
			&& (pType->Speed >= 8 || pType->JumpJet) && SlotGate(scores[80], 80, 80)
			&& pUnit->DistanceFrom(pEnemyArtilleryUnit) <= 4608)
		{
			DispatchFootToTargetOrTransport(pUnit, pEnemyArtilleryUnit, Mission::Attack, true);

			continue;
		}

		// Slot 78 (High-Ground Cliff Advantage Positioning): Siege/long-range units
		// step onto nearby higher-elevation cells (CellClass::Level) when idle!
		if (isUnitIdle && pPrimaryWpn && pPrimaryWpn->Range >= (7 * 256) && !pType->Naval && SlotGate(scores[78], 90, 78)
			&& (IsAITickWindow(120, 20)))
		{
			if (auto const pCurC = pUnit->GetCell())
			{
				const CellStruct uc = pUnit->GetMapCoords();

				for (int dy = -4; dy <= 4; dy += 2)
				{
					bool movedHigh = false;

					for (int dx = -4; dx <= 4; dx += 2)
					{
						const CellStruct hc { static_cast<short>(uc.X + dx), static_cast<short>(uc.Y + dy) };

						if (auto const pHC = MapClass::Instance.TryGetCellAt(hc))
						{
							if (pHC->Level > pCurC->Level
								&& pHC->IsClearToMove(pType->SpeedType, true, true, -1, pType->MovementZone, -1, false))
							{
								pUnit->SetDestination(pHC, true);

								pUnit->QueueMission(Mission::Move, true);

								movedHigh = true;

								break;
							}
						}
					}

					if (movedHigh)
						break;
				}
			}
		}

		// Slot 58 (Mind-Control Breaker): Focus-fire enemy Yuri/Mastermind/Psychic
		// Tower with ImmuneToPsionics vehicles or long-range artillery to
		// immediately free our mind-controlled tank!
		if (pEnemyMindController && isUnitIdle
			&& (pType->ImmuneToPsionics || (pPrimaryWpn && pPrimaryWpn->Range >= (6 * 256)))
			&& pUnit->DistanceFrom(pEnemyMindController) <= 4608)
		{
			DispatchFootToTargetOrTransport(pUnit, pEnemyMindController, Mission::Attack, true);

			continue;
		}

		// Slot 58 (Tank Infantry Crushing): Heavy Crusher=yes tanks run over nearby
		// Crushable=yes enemy infantry (<= 3 cells) instead of wasting slow cannon
		// shells!
		if (pType->Crusher && !pType->Naval && SlotGate(scores[58], 100, 58)
			&& (isUnitIdle || pUnit->CurrentMission == Mission::Attack) && (IsAITickWindow(45, 15)))
		{
			CellClass* pCrushCell = nullptr;

			for (auto const pEInf : InfantryClass::Array)
			{
				if (!pEInf || pEInf->InLimbo || !pEInf->IsAlive || !pEInf->Owner || !pEInf->Type
					|| !pEInf->Type->Crushable || pEInf->Type->C4 || pEInf->Type->Ivan)
				{
					continue;
				}

				if (pEInf->Owner == pHouse || pHouse->IsAlliedWith(pEInf->Owner) || pEInf->Owner->IsNeutral())
					continue;

				if (pEInf->GetCell() && pEInf->GetCell()->GetBuilding() == nullptr && pUnit->DistanceFrom(pEInf) <= 768
					&& IsCellVisibleToAI(pHouse, pEInf->GetCell()))
				{
					pCrushCell = pEInf->GetCell();

					break;
				}
			}

			if (pCrushCell)
			{
				DispatchFootToTargetOrTransport(pUnit, pCrushCell, Mission::Move, false);

				continue;
			}
		}

		// Slot 58 (Range-Advantage Kiting / Hit-and-Run): a unit that outranges the
		// enemy closing in (vehicle or infantry) steps back AWAY from it until it sits
		// outside the enemy's reach but still inside its own, then fires again. Only
		// when the threat is not faster than us, otherwise retreating just loses DPS.
		// The trained slot only scales the safety margin; it never switches it off.
		if (pPrimaryWpn && pPrimaryWpn->Damage > 0 && !pType->Harvester && !pType->ResourceGatherer
			&& !IsAmphibiousOrAirTransport(pType) && !IsImmobileUnitTypeOnLand(pType))
		{
			const int ownRange = std::max(pPrimaryWpn->Range, pSecondaryWpn && pSecondaryWpn->Damage > 0 ? pSecondaryWpn->Range : 0);
			const int marginLeptons = Unsorted::LeptonsPerCell / 2
				+ std::clamp(static_cast<int>(scores[58]), -300, 600) * Unsorted::LeptonsPerCell / 600;
			const CellStruct uc = pUnit->GetMapCoords();
			const LocalCombatContact* pKiteThreat = nullptr;
			int closestThreatDist = INT_MAX;

			for (auto const& contact : enemyContacts)
			{
				if (contact.Airborne || contact.GroundRange <= 0 || contact.GroundRange + Unsorted::LeptonsPerCell > ownRange
					|| contact.Speed > pType->Speed)
				{
					continue;
				}

				const int dist = pUnit->DistanceFrom(contact.Techno);

				if (dist <= contact.GroundRange + marginLeptons + Unsorted::LeptonsPerCell && dist < closestThreatDist)
				{
					closestThreatDist = dist;
					pKiteThreat = &contact;
				}
			}

			// Hit and run: the step back happens while the weapon reloads. With the weapon ready the
			// unit stays and fires (focus fire below picks the target), so kiting never costs shots.
			if (pKiteThreat && pUnit->CurrentMission != Mission::Move && pUnit->RearmTimer.GetTimeLeft() >= 10)
			{
				// Step straight away from the threat to sit just outside its range.
				const int dx = uc.X - pKiteThreat->Coords.X;
				const int dy = uc.Y - pKiteThreat->Coords.Y;
				const double len = std::max(1.0, std::sqrt(static_cast<double>(dx * dx + dy * dy)));
				const double desiredCells = (pKiteThreat->GroundRange + marginLeptons + Unsorted::LeptonsPerCell)
					/ static_cast<double>(Unsorted::LeptonsPerCell);
				const double stepCells = std::clamp(desiredCells - closestThreatDist / static_cast<double>(Unsorted::LeptonsPerCell), 1.5, 4.0);
				CellClass* pKiteCell = nullptr;

				// Straight back first, then to each side; the first pass skips cells inside the
				// reach of another visible armed enemy, the second takes any clear cell.
				for (int pass = 0; pass < 2 && !pKiteCell; ++pass)
				{
					for (const double sideBias : { 0.0, 0.6, -0.6, 1.2, -1.2 })
					{
						const double dirX = dx / len - (dy / len) * sideBias;
						const double dirY = dy / len + (dx / len) * sideBias;
						const CellStruct kiteCoords { static_cast<short>(uc.X + dirX * stepCells),
							static_cast<short>(uc.Y + dirY * stepCells) };
						auto const pCandCell = MapClass::Instance.TryGetCellAt(kiteCoords);

						if (!pCandCell || !pCandCell->IsClearToMove(pType->SpeedType, true, true, -1, pType->MovementZone, -1, false))
							continue;

						bool covered = false;

						for (auto const& other : enemyContacts)
						{
							if (pass == 0 && &other != pKiteThreat && !other.Airborne && other.Firepower > 0 && other.GroundRange > 0
								&& other.Coords.DistanceFrom(kiteCoords) * Unsorted::LeptonsPerCell <= other.GroundRange + Unsorted::LeptonsPerCell / 2)
							{
								covered = true;
								break;
							}
						}

						if (!covered)
						{
							pKiteCell = pCandCell;
							break;
						}
					}
				}

				if (pKiteCell)
				{
					pUnit->SetDestination(pKiteCell, true);
					CountDecision(NeuralDecision::KiteStep);
					pUnit->QueueMission(Mission::Move, true);
					continue;
				}
			}
			else if (!pKiteThreat && isUnitIdle && pUnit->Target == nullptr)
			{
				// Back at a safe distance: resume fire on anything inside our reach.
				for (auto const& contact : enemyContacts)
				{
					if (!contact.Airborne && contact.GroundRange > 0 && contact.GroundRange + Unsorted::LeptonsPerCell <= ownRange
						&& pUnit->DistanceFrom(contact.Techno) <= ownRange)
					{
						pUnit->SetTarget(contact.Techno);
						pUnit->QueueMission(Mission::Attack, true);
						break;
					}
				}

				if (pUnit->Target != nullptr)
					continue;
			}
		}

		// Slot 76 (Squad Focus Fire, pickFocusTarget) for a unit already fighting.
		if (pUnit->Target != nullptr && pPrimaryWpn && !pType->Harvester && !pType->ResourceGatherer
			&& (pUnit->CurrentMission == Mission::Attack || pUnit->CurrentMission == Mission::Guard
				|| pUnit->CurrentMission == Mission::Area_Guard || pUnit->CurrentMission == Mission::Hunt))
		{
			const int focusRange = std::max(pPrimaryWpn->Range, pSecondaryWpn && pSecondaryWpn->Damage > 0 ? pSecondaryWpn->Range : 0);

			if (auto const pFocusTarget = pickFocusTarget(pUnit, focusRange))
			{
				pUnit->SetTarget(pFocusTarget);

				continue;
			}
		}

		// Slot 59 (Crate Pickup): Send any idle vehicle (or non-engaged vehicle near a crate)
		// to collect the nearest visible reachable Crate within 18 cells!
		if ((isUnitIdle || (!IsFootActivelyAttackingOrMovingToEnemy(pUnit) && (IsAITickWindow(45, 15))))
			&& !pType->Harvester && !pType->ResourceGatherer
			&& !(pType->DeploysInto && pType->DeploysInto->ConstructionYard))
		{
			if (CellClass* pNearCrate = findNearestReachableCrateForFoot(pUnit, 18))
			{
				pUnit->SetDestination(pNearCrate, true);
				pUnit->QueueMission(Mission::Move, true);
				continue;
			}
		}

		// Slot 56 (Own Harvester Defense & Enemy Harvester Eco-Raid - Multi-Instance Nearest Match):
		if (attackedOwnHarvCount > 0 && isUnitIdle)
		{
			UnitClass* pNearestAttackedHarv = nullptr;
			int bestOwnHarvDist = 4609;

			for (int hi = 0; hi < attackedOwnHarvCount; ++hi)
			{
				auto const pCandH = attackedOwnHarvesters[hi];

				if (!pCandH || pCandH->InLimbo || !pCandH->IsAlive)
					continue;

				const int d = pUnit->DistanceFrom(pCandH);

				if (d <= 4608 && d < bestOwnHarvDist)
				{
					bestOwnHarvDist = d;
					pNearestAttackedHarv = pCandH;
				}
			}

			if (pNearestAttackedHarv)
			{
				if (auto const pHarvCell = pNearestAttackedHarv->GetCell())
				{
					DispatchFootToTargetOrTransport(pUnit, pHarvCell, Mission::AttackMove, false);

					continue;
				}
			}
		}

		// Enemy harvester hunting is handled by the opportunistic local strike (it checks
		// local superiority and reachability, and slot 56 scales the harvester priority), while
		// planned raids on refineries are the Eco Raid plan: no duplicate chase logic here.

		// Slot 57 (Garrison Penetrator & Siege Focus): units whose weapon kills the occupants
		// through the walls (Phobos PenetratesGarrison) empty enemy garrisons in sight without
		// demolishing the building, so our infantry can take it; up to 8 cells beyond their range
		// while Expert 9 is in charge. Long-range siege vehicles go for garrisons and defenses.
		if (enemyGarrisonCount > 0 && isUnitIdle && pPrimaryWpn && HasGarrisonPenetratingWeapon(pType))
		{
			const int maxGarrDist = std::max(1536, static_cast<int>(pPrimaryWpn->Range)
				+ (pExt->NeuralAI_ActiveExpert == 9 ? 8 : 2) * Unsorted::LeptonsPerCell);
			BuildingClass* pNearestGarr = nullptr;
			int bestGarrDist = maxGarrDist + 1;

			for (int gi = 0; gi < enemyGarrisonCount; ++gi)
			{
				auto const pCandG = enemyGarrisonsToClear[gi];

				if (!pCandG || pCandG->InLimbo || !pCandG->IsAlive || pCandG->Occupants.Count <= 0)
					continue;

				const int d = pUnit->DistanceFrom(pCandG);

				if (d <= maxGarrDist && d < bestGarrDist)
				{
					bestGarrDist = d;
					pNearestGarr = pCandG;
				}
			}

			if (pNearestGarr && DispatchFootToTargetOrTransport(pUnit, pNearestGarr, Mission::Attack, true))
			{
				NoteUrbanClearOrder(pHouse, pUnit, pNearestGarr);
				continue;
			}
		}

		if (pPrioritySiegeBld && isUnitIdle && pPrimaryWpn && pPrimaryWpn->Range >= (7 * 256)
			&& pUnit->DistanceFrom(pPrioritySiegeBld) <= static_cast<int>(pPrimaryWpn->Range) + 512)
		{
			DispatchFootToTargetOrTransport(pUnit, pPrioritySiegeBld, Mission::Attack, true);
			continue;
		}

		// Slot 82 (Human Shield & Anti-Recapture Bodyguard Screen for Stolen
		// ConYard / Escaping MCV): If we just captured an enemy ConYard or are
		// extracting its stolen MCV, any nearby friendly combat vehicle (<= 16
		// cells) immediately intercepts enemy Engineers/tanks threatening it, or
		// moves directly around the stolen ConYard/MCV as a protective human shield
		// to draw enemy fire away while it escapes!
		if (pStolenConYardOrMCV && pUnit != pStolenConYardOrMCV && !pType->Naval && SlotGate(scores[82], 60, 82)
			&& pUnit->DistanceFrom(pStolenConYardOrMCV) <= 4096)
		{
			if (pThreatToStolenAsset && pPrimaryWpn && pPrimaryWpn->Damage > 0 && pUnit->Target != pThreatToStolenAsset)
			{
				DispatchFootToTargetOrTransport(pUnit, pThreatToStolenAsset, Mission::Attack, true);

				continue;
			}
			else if (isUnitIdle && pUnit->DistanceFrom(pStolenConYardOrMCV) > 640)
			{
				if (auto const pShieldCell = pStolenConYardOrMCV->GetCell())
				{
					DispatchFootToTargetOrTransport(pUnit, pShieldCell, Mission::Area_Guard, false);

					continue;
				}
			}
		}

		// Slot 60 (Spawner Standoff & AA/Sensor Combined-Arms Escort):
		// Keep Spawner=yes units (Aircraft Carrier, Dreadnought, V3) at standoff
		// distance (> 5 cells) from approaching enemies!
		const bool shellingBuildingFromRange = abstract_cast<BuildingClass*>(pUnit->Target)
			&& pUnit->DistanceFrom(pUnit->Target) >= 6 * Unsorted::LeptonsPerCell;

		if (pPrimaryWpn && pPrimaryWpn->Spawner && !shellingBuildingFromRange && SlotGate(scores[60], 100, 60)
			&& homeCoords != CellStruct::Empty && (IsAITickWindow(45, 15)))
		{
			bool enemyClosingOnSpawner = false;

			for (auto const pET : TechnoClass::Array)
			{
				if (!pET || pET->InLimbo || !pET->IsAlive || !pET->Owner)
					continue;

				if (pET->Owner == pHouse || pHouse->IsAlliedWith(pET->Owner) || !IsCellVisibleToAI(pHouse, pET->GetCell()) || pET->Owner->IsNeutral())
					continue;

				// Only something that can actually hurt us counts (armed units or defenses).
				auto const pETType = pET->GetTechnoType();

				if (!pETType || GetCombatValueOfType(pETType, false) <= 0
					|| (pET->WhatAmI() == AbstractType::Building && !static_cast<BuildingClass*>(pET)->Type->IsBaseDefense))
				{
					continue;
				}

				{
					auto const pETW = pETType->GetWeapon(0);

					if (!pETW || !pETW->WeaponType || pETW->WeaponType->Damage <= 0)
						continue;
				}

				if (pUnit->DistanceFrom(pET) <= 1280)
				{
					enemyClosingOnSpawner = true;

					break;
				}
			}

			if (enemyClosingOnSpawner)
			{
				const CellStruct uc = pUnit->GetMapCoords();
				const int sx = (homeCoords.X > uc.X) ? 4 : ((homeCoords.X < uc.X) ? -4 : 0);
				const int sy = (homeCoords.Y > uc.Y) ? 4 : ((homeCoords.Y < uc.Y) ? -4 : 0);
				const CellStruct retreatCell { static_cast<short>(uc.X + sx), static_cast<short>(uc.Y + sy) };

				if (auto const pRC = MapClass::Instance.TryGetCellAt(retreatCell))
				{
					if (pRC->IsClearToMove(pType->SpeedType, true, true, -1, pType->MovementZone, -1, false))
					{
						pUnit->SetDestination(pRC, true);

						pUnit->QueueMission(Mission::Move, true);

						continue;
					}
				}
			}
		}

		// Escort our lead Heavy/Siege Tank with mobile AA, Sensor, Mobile Radar
		// Jammer, GapGenerator & Mobile Stealth Generator vehicles (Slots 60 & 63)!
		const bool isUnarmedSupportEscort
			= (pPrimaryWpn == nullptr && !pType->Harvester && !pType->ResourceGatherer && pType->Passengers == 0
				&& (pType->DeploysInto == nullptr || !IsTrueBaseStructureTarget(pType->DeploysInto)));
		if (pLeadHeavyTank && pUnit != pLeadHeavyTank && isUnitIdle && !pType->Naval
			&& (isUnarmedSupportEscort
				|| (((pPrimaryWpn && pPrimaryWpn->Projectile && pPrimaryWpn->Projectile->AA) || pType->Sensors)
					&& SlotGate(scores[60], 120, 60))
				|| (pType->GapGenerator && SlotGate(scores[63], 80, 63)))
			&& (IsAITickWindow(90, 20)))
		{
			const int distToLead = pUnit->DistanceFrom(pLeadHeavyTank);

			if (distToLead > 768 && distToLead <= 4608)
			{
				if (auto const pLeadCell = pLeadHeavyTank->GetCell())
				{
					DispatchFootToTargetOrTransport(pUnit, pLeadCell, Mission::Area_Guard, false);

					continue;
				}
			}
			else if (pType->GapGenerator && pType->IsSimpleDeployer && !pUnit->GapSuperCharged && distToLead <= 768)
			{
				pUnit->QueueMission(Mission::Unload, true);

				continue;
			}
		}

		// Slot 61 (Mirage Tank Dynamic Disguise & Counter-Disguise Interception):
		if (pType->CanDisguise && !pType->PermaDisguise && !pUnit->Disguised && pSampleEnemyInfantry
			&& pUnit->CanDisguiseAs(pSampleEnemyInfantry) && SlotGate(scores[61], 80, 61))
		{
			pUnit->DisguiseAs(pSampleEnemyInfantry);
		}

		// Slot 83 (Mirage Tank & Camouflage Tree-Grove Ambush, Spotted Retreat &
		// Ambush Zone Rotation): Position Mirage Tanks (CanDisguise /
		// PermaDisguise) and ground Stealth Ambush tanks (Cloakable && !Naval)
		// inside tree groves (GetTerrain) along enemy Harvester/reinforcement
		// corridors and mimic the exact tree species of the grove! Ambush
		// unsuspecting critical enemy units (Harvesters, Engineers, Artillery,
		// wounded vehicles) that pass within weapon range without leaving the tree
		// grove. If spotted/attacked by the enemy (HasBeenAttacked, HP < 78%,
		// targeted by enemy tanks, or inside enemy DisguiseSensors), retreat
		// immediately back to our home base / Service Depot! After regrouping or
		// after an ambush epoch (~900 frames), rotate to a different tree grove
		// sector to keep harassing the enemy from new angles!
		const bool isCamoAmbushTank =

			!pType->Naval && !pType->Harvester && !pType->ResourceGatherer
			&& (pType->CanDisguise || pType->PermaDisguise || pType->Cloakable) && pPrimaryWpn
			&& pPrimaryWpn->Damage > 0;

		if (isCamoAmbushTank && SlotGate(scores[83], 60, 83) && homeCoords != CellStruct::Empty)
		{
			const CellStruct uc = pUnit->GetMapCoords();
			auto const pUC = pUnit->GetCell();
			int adjacentTrees = 0;

			TerrainClass* pNearTree = nullptr;

			for (int dy = -1; dy <= 1; ++dy)
			{
				for (int dx = -1; dx <= 1; ++dx)
				{
					const CellStruct nc { static_cast<short>(uc.X + dx), static_cast<short>(uc.Y + dy) };

					if (auto const pNC = MapClass::Instance.TryGetCellAt(nc))
					{
						if (auto const pTer = pNC->GetTerrain(false))
						{
							adjacentTrees++;

							if (!pNearTree)
								pNearTree = pTer;
						}
					}
				}
			}

			// Mimic the exact tree species of the surrounding grove when stationary!
			if ((pType->CanDisguise || pType->PermaDisguise) && pNearTree && isUnitIdle
				&& (!pUnit->Disguised || (IsAITickPeriod(300)))
				&& pUnit->CanDisguiseAs(reinterpret_cast<AbstractClass*>(pNearTree)))
			{
				pUnit->DisguiseAs(reinterpret_cast<AbstractClass*>(pNearTree));
			}

			// Check if the enemy has spotted / compromised our ambush ("si les
			// pillan")
			bool ambushSpotted = (pUnit->GetHealthPercentage() < 0.78)
				|| (pUnit->HasBeenAttacked && pUnit->GetHealthPercentage() < 0.92);

			// Inside the reach of a disguise detector we know of (the hidden sensor map is not read).
			if (!ambushSpotted && pUC && IsCellWatchedByDisguiseDetector(pHouse, pUC->MapCoords, 0))
				ambushSpotted = true;

			if (!ambushSpotted)
			{
				int closeHostileTanks = 0;

				for (auto const pEU : UnitClass::Array)
				{
					if (!pEU || pEU->InLimbo || !pEU->IsAlive || !pEU->Owner || !pEU->Type || pEU->Type->Harvester
						|| pEU->Type->ResourceGatherer)
					{
						continue;
					}

					if (pEU->Owner == pHouse || pHouse->IsAlliedWith(pEU->Owner) || !IsCellVisibleToAI(pHouse, pEU->GetCell()) || pEU->Owner->IsNeutral())
						continue;

					if (pEU->Target == pUnit)
					{
						ambushSpotted = true;

						break;
					}

					if (pUnit->DistanceFrom(pEU) <= 1152 && ++closeHostileTanks >= 2)
					{
						ambushSpotted = true;

						break;
					}
				}
			}

			if (ambushSpotted && uc.DistanceFrom(homeCoords) > 10.0)
			{
				pUnit->SetTarget(nullptr);

				BuildingClass* const pAmbushStation = (pRepairDepot && pUnit->GetHealthPercentage() < 0.85
					&& !IsAmphibiousOrAirTransport(pType)) ? FindBestRepairStation(pHouse, pUnit) : nullptr;

				if (pAmbushStation)
				{
					DispatchFootToTargetOrTransport(pUnit, pAmbushStation, Mission::Enter, false);
				}
				else if (auto const pHomeCell = MapClass::Instance.TryGetCellAt(homeCoords))
				{
					DispatchFootToTargetOrTransport(pUnit, pHomeCell, Mission::Move, false);
				}

				continue;
			}

			// If concealed in a tree grove away from base, snipe unsuspecting
			// critical enemies that walk directly into our weapon range!
			if (!ambushSpotted && adjacentTrees >= 1 && uc.DistanceFrom(homeCoords) > 10.0)
			{
				TechnoClass* pAmbushVictim = nullptr;

				if (pVisibleEnemyHarvester && pUnit->DistanceFrom(pVisibleEnemyHarvester) <= pPrimaryWpn->Range)
				{
					pAmbushVictim = pVisibleEnemyHarvester;
				}
				else if (pEnemySpecialistInf && pUnit->DistanceFrom(pEnemySpecialistInf) <= pPrimaryWpn->Range)
				{
					pAmbushVictim = pEnemySpecialistInf;
				}
				else if (pEnemyArtilleryUnit && pUnit->DistanceFrom(pEnemyArtilleryUnit) <= pPrimaryWpn->Range)
				{
					pAmbushVictim = pEnemyArtilleryUnit;
				}
				else if (pWoundedEnemyVehicle && pUnit->DistanceFrom(pWoundedEnemyVehicle) <= pPrimaryWpn->Range)
				{
					pAmbushVictim = pWoundedEnemyVehicle;
				}
				else if (pSampleEnemyInfantry && pUnit->DistanceFrom(pSampleEnemyInfantry) <= pPrimaryWpn->Range)
				{
					pAmbushVictim = pSampleEnemyInfantry;
				}

				if (pAmbushVictim)
				{
					pUnit->SetTarget(pAmbushVictim);

					pUnit->QueueMission(Mission::Attack, true);

					continue;
				}
			}

			// Move to a tree grove along enemy Harvester/transit routes (or rotate to
			// a new grove sector every ~900 frames so the enemy cannot predict us!)
			const bool shouldRotateGrove = (IsAITickWindow(900, 20));

			if (isUnitIdle && pUnit->GetHealthPercentage() >= 0.85
				&& (adjacentTrees == 0 || uc.DistanceFrom(homeCoords) <= 11.0 || shouldRotateGrove)
				&& (IsAITickWindow(60, 20)))
			{
				CellStruct enemyFocus = homeCoords;

				if (pVisibleEnemyHarvester)
					enemyFocus = pVisibleEnemyHarvester->GetMapCoords();
				else if (pSurgicalEnemyBld)
					enemyFocus = pSurgicalEnemyBld->GetMapCoords();
				else if (pPrioritySiegeBld)
					enemyFocus = pPrioritySiegeBld->GetMapCoords();

				const CellStruct searchCenter { static_cast<short>((homeCoords.X + enemyFocus.X) / 2),
					static_cast<short>((homeCoords.Y + enemyFocus.Y) / 2) };

				const int desiredSector = ((Unsorted::CurrentFrame / 900) + pUnit->UniqueID) % 4;
				CellClass* pBestGroveCell = nullptr;
				int bestGroveScore = -99999;

				for (int dy = -28; dy <= 28; dy += 2)
				{
					for (int dx = -28; dx <= 28; dx += 2)
					{
						const CellStruct gc { static_cast<short>(searchCenter.X + dx),
							static_cast<short>(searchCenter.Y + dy) };

						auto const pGC = MapClass::Instance.TryGetCellAt(gc);

						if (!pGC || pGC->GetTerrain(false) != nullptr
							|| !pGC->IsClearToMove(pType->SpeedType, true, true, -1, pType->MovementZone, -1, false))
						{
							continue;
						}

						if (std::max(std::abs(gc.X - homeCoords.X), std::abs(gc.Y - homeCoords.Y)) <= 11
							|| std::max(std::abs(gc.X - uc.X), std::abs(gc.Y - uc.Y)) <= 6)
						{
							continue;
						}

						// Count adjacent trees & nearby Ore (Harvester transit route!)
						int groveTrees = 0;
						bool nearOreRoute = (pGC->GetContainedTiberiumValue() > 0);

						for (int ty = -1; ty <= 1; ++ty)
						{
							for (int tx = -1; tx <= 1; ++tx)
							{
								const CellStruct tc2 { static_cast<short>(gc.X + tx), static_cast<short>(gc.Y + ty) };

								if (auto const pTC2 = MapClass::Instance.TryGetCellAt(tc2))
								{
									if (pTC2->GetTerrain(false) != nullptr)
										groveTrees++;

									if (pTC2->GetContainedTiberiumValue() > 0)
										nearOreRoute = true;
								}
							}
						}

						if (groveTrees == 0)
							continue;

						// Ensure outside enemy base defense range (> 11 cells)
						bool nearEnemyTurret = false;

						for (auto const pEB : BuildingClass::Array)
						{
							if (!pEB || pEB->InLimbo || !pEB->IsAlive || !pEB->Owner || !pEB->Type
								|| !pEB->Type->IsBaseDefense)
							{
								continue;
							}

							if (pEB->Owner == pHouse || pHouse->IsAlliedWith(pEB->Owner) || !IsEnemyBuildingKnown(pHouse, pEB) || pEB->Owner->IsNeutral())
								continue;

							const CellStruct ec = pEB->GetMapCoords();

							if (std::max(std::abs(gc.X - ec.X), std::abs(gc.Y - ec.Y)) <= 11)
							{
								nearEnemyTurret = true;

								break;
							}
						}

						if (nearEnemyTurret)
							continue;

						const int cellSector = ((dx >= 0 ? 1 : 0) + (dy >= 0 ? 2 : 0));

						int gScore = groveTrees * 35 + (nearOreRoute ? 90 : 0) + (cellSector == desiredSector ? 120 : 0)
							- (std::abs(gc.X - enemyFocus.X) + std::abs(gc.Y - enemyFocus.Y));

						if (gScore > bestGroveScore)
						{
							bestGroveScore = gScore;

							pBestGroveCell = pGC;
						}
					}
				}

				if (pBestGroveCell)
				{
					DispatchFootToTargetOrTransport(pUnit, pBestGroveCell, Mission::Move, false);

					continue;
				}
			}
		}

		if (isUnitIdle && !pType->Naval && (pType->DetectDisguise || pType->Sensors))
		{
			if (pDetectedEnemyInfiltrator && pUnit->DistanceFrom(pDetectedEnemyInfiltrator) <= 3840)
			{
				DispatchFootToTargetOrTransport(pUnit, pDetectedEnemyInfiltrator, Mission::Attack, true);

				continue;
			}

			TechnoClass* pSweepTarget = pHiddenOrStealthEnemy ? pHiddenOrStealthEnemy : pDetectedEnemyInfiltrator;

			if (pSweepTarget)
			{
				if (auto const pSweepCell = pSweepTarget->GetCell())
				{
					DispatchFootToTargetOrTransport(pUnit, pSweepCell, Mission::AttackMove, false);

					continue;
				}
			}
		}

		// Slot 62 (Slave Miner Liberation): If our unit is shooting an enslaved
		// worker (Slaved=yes) whose SlaveOwner (Slave Miner) is alive nearby,
		// redirect fire to the SlaveOwner so all 5 slaves flip to our House!
		if (auto const pCurTargetInf = abstract_cast<InfantryClass*>(pUnit->Target))
		{
			if (pCurTargetInf->Type && pCurTargetInf->Type->Slaved && pCurTargetInf->SlaveOwner
				&& pCurTargetInf->SlaveOwner->IsAlive && !pCurTargetInf->SlaveOwner->InLimbo && SlotGate(scores[62], 60, 62))
			{
				DispatchFootToTargetOrTransport(pUnit, pCurTargetInf->SlaveOwner, Mission::Attack, true);

				continue;
			}
		}

		if (pEnemySlaveMiner && isUnitIdle && !pType->Naval && SlotGate(scores[62], 140, 62)
			&& pUnit->DistanceFrom(pEnemySlaveMiner) <= 4096)
		{
			DispatchFootToTargetOrTransport(pUnit, pEnemySlaveMiner, Mission::Attack, true);

			continue;
		}

		// Slot 64 (Active Ally De-Parasiting via Sonic, RemoveParasite, or Damage <
		// 0): Dolphins (Sonic=yes), Repair IFVs (Damage < 0), and units with Phobos
		// RemoveParasite=yes immediately free allied vehicles/ships being eaten by
		// a Giant Squid or Terror Drone!
		const auto pWH = pPrimaryWpn ? pPrimaryWpn->Warhead : nullptr;
		const auto pWHExt = pWH ? WarheadTypeExt::TryFetch(pWH) : nullptr;

		const bool canCureParasite = pPrimaryWpn
			&& (pPrimaryWpn->Damage < 0 || (pWH && pWH->Sonic) || (pWHExt && pWHExt->RemoveParasite.Get(false)));

		if (pParasitedOwnUnit && pParasitedOwnUnit != pUnit && isUnitIdle && canCureParasite && SlotGate(scores[64], 60, 64)
			&& pUnit->DistanceFrom(pParasitedOwnUnit) <= 4096)
		{
			DispatchFootToTargetOrTransport(pUnit, pParasitedOwnUnit, Mission::Attack, true);

			continue;
		}

		// Slot 64 (Offensive Parasite Targeting: Terror Drone, Giant Squid,
		// Cyberworm): Target expensive enemy tanks/harvesters/warships that do NOT
		// yet have a parasite!
		if (pWH && pWH->Parasite && isUnitIdle && pBestParasiteVictim && pUnit->ParasiteImUsing == nullptr
			&& SlotGate(scores[64], 60, 64) && pType->Naval == pBestParasiteVictim->Type->Naval
			&& pUnit->DistanceFrom(pBestParasiteVictim) <= 4608)
		{
			DispatchFootToTargetOrTransport(pUnit, pBestParasiteVictim, Mission::Attack, true);

			continue;
		}

		// Slot 66 (EMP Pulse Targeting & EMP-Paralyzed Target Finisher):
		if (pWH && pWH->EMEffect && isUnitIdle && pEnemyNonEMPedHeavy && SlotGate(scores[66], 80, 66)
			&& pUnit->DistanceFrom(pEnemyNonEMPedHeavy) <= 4096)
		{
			DispatchFootToTargetOrTransport(pUnit, pEnemyNonEMPedHeavy, Mission::Attack, true);

			continue;
		}
		else if (pEnemyEMPedTarget && isUnitIdle && pPrimaryWpn && pPrimaryWpn->Damage > 0 && (!pWH || !pWH->EMEffect)
			&& SlotGate(scores[66], 100, 66) && pUnit->DistanceFrom(pEnemyEMPedTarget) <= 3584)
		{
			DispatchFootToTargetOrTransport(pUnit, pEnemyEMPedTarget, Mission::Attack, true);

			continue;
		}

		// Prioritize filling any owned empty Tank Bunker with compatible idle Bunkerable tanks
		if (isUnitIdle && pType->Bunkerable && !pType->Naval && !pType->Harvester && !pType->ResourceGatherer
			&& pUnit->BunkerLinkedItem == nullptr && pPrimaryWpn && pPrimaryWpn->Damage > 0)
		{
			const int unitZoneForBunker
				= ObjectZone(pUnit, MovementZone::Normal);
			BuildingClass* pBestTankBunker = nullptr;
			int bestTankBunkerDist = 4096;

			for (auto const pBld : pHouse->Buildings)
			{
				if (!pBld || pBld->InLimbo || !pBld->IsAlive || !pBld->Type || !pBld->Type->Bunker
					|| pBld->BunkerLinkedItem != nullptr)
				{
					continue;
				}

				const int bz
					= ObjectZone(pBld, MovementZone::Normal);

				if (unitZoneForBunker >= 0 && bz >= 0 && bz != unitZoneForBunker)
					continue;

				bool anotherTankAlreadyEntering = false;

				for (auto const pOtherU : UnitClass::Array)
				{
					if (pOtherU && pOtherU != pUnit && pOtherU->Owner == pHouse && !pOtherU->InLimbo && pOtherU->IsAlive
						&& (pOtherU->CurrentMission == Mission::Enter || pOtherU->QueuedMission == Mission::Enter)
						&& (pOtherU->Destination == pBld || pOtherU->ArchiveTarget == pBld
							|| pOtherU->ContainsLink(pBld)))
					{
						anotherTankAlreadyEntering = true;
						break;
					}
				}

				if (anotherTankAlreadyEntering)
					continue;

				const int d = pUnit->DistanceFrom(pBld);

				if (d <= bestTankBunkerDist)
				{
					bestTankBunkerDist = d;
					pBestTankBunker = pBld;
				}
			}

			if (pBestTankBunker)
			{
				OrderFootToEnter(pUnit, pBestTankBunker);
				continue;
			}
		}

		// Slot 68 (Psychedelic Chaos Gas): Target non-berserk heavy enemy tanks!
		if (pWH && pWH->Psychedelic && isUnitIdle && pEnemyChaosTarget && SlotGate(scores[68], 80, 68)
			&& pUnit->DistanceFrom(pEnemyChaosTarget) <= 3584)
		{
			DispatchFootToTargetOrTransport(pUnit, pEnemyChaosTarget, Mission::Attack, true);

			continue;
		}

		// Slot 69 (Submarine Cloak Ambush & Anti-Submarine Destroyer Escort). While only enemy ships or
		// submarines remain (NavalRemnantUntil) the hunt runs whatever the learned gate says.
		if (pType->Naval && (SlotGate(scores[69], 80, 69) || GetMacroState(pHouse).NavalRemnantUntil > Unsorted::CurrentFrame
			|| GetMacroState(pHouse).SubThreatUntil > Unsorted::CurrentFrame))
		{
			if (pType->Cloakable && pUnit->GetHealthPercentage() < 0.45 && homeCoords != CellStruct::Empty
				&& pUnit->CurrentMission != Mission::Move)
			{
				if (auto const pHC = MapClass::Instance.TryGetCellAt(homeCoords))
				{
					pUnit->SetDestination(pHC, true);

					pUnit->QueueMission(Mission::Move, true);

					continue;
				}
			}
			// Against a submarine only ships that hit under water (NavalTargeting) or carry sonar go.
			else if (isUnitIdle && pEnemySubOrShip
				&& (pType->Sensors || pType->NavalTargeting == NavalTargetingType::Underwater_Secondary
					|| pType->NavalTargeting == NavalTargetingType::Underwater_Only
					|| pType->NavalTargeting == NavalTargetingType::Organic_Secondary
					|| (!pEnemySubOrShip->Type->Cloakable && pPrimaryWpn && pPrimaryWpn->Damage > 0)))
			{
				if (auto const pSubCell = pEnemySubOrShip->GetCell())
				{
					if (pType->Sensors || IsCellVisibleToAI(pHouse, pSubCell))
					{
						OrderFootToAttackMove(pUnit, pSubCell);

						continue;
					}
				}
			}
			else if (pType->Sensors && pLeadWarship && pUnit != pLeadWarship && isUnitIdle
				&& (IsAITickWindow(90, 20)))
			{
				const int distW = pUnit->DistanceFrom(pLeadWarship);

				if (distW > 768 && distW <= 4608)
				{
					if (auto const pWC = pLeadWarship->GetCell())
					{
						pUnit->SetDestination(pWC, true);

						pUnit->QueueMission(Mission::Area_Guard, true);

						continue;
					}
				}
			}
		}

		// Field Repair Vehicles (Repair IFV / Mobile Mechanic / Support Drone where
		// Damage < 0): Heal damaged own OR allied vehicles in the field!
		if (pPrimaryWpn && pPrimaryWpn->Damage < 0 && isUnitIdle)
		{
			for (auto const pAllyU : UnitClass::Array)
			{
				if (!pAllyU || pAllyU == pUnit || !pAllyU->Owner || !pHouse->IsAlliedWith(pAllyU->Owner)
					|| pAllyU->InLimbo || !pAllyU->IsAlive || !pAllyU->Type || pAllyU->Type->Organic)
				{
					continue;
				}

				if ((pAllyU->ParasiteEatingMe != nullptr || pAllyU->GetHealthPercentage() < 0.85)
					&& pUnit->DistanceFrom(pAllyU) <= 3584)
				{
					DispatchFootToTargetOrTransport(pUnit, pAllyU, Mission::Attack, true);

					break;
				}
			}

			continue;
		}

		// Yuri Floating Disc (DrainWeapon=yes): Prioritize draining Enemy
		// Superweapons, Refineries (steal credits!), and Power Plants (blackout!)
		if (pPrimaryWpn && pPrimaryWpn->DrainWeapon && isUnitIdle && pUnit->DrainTarget == nullptr)
		{
			BuildingClass* pBestDrainBld = nullptr;
			int bestDrainScore = -1;

			for (auto const pEBld : BuildingClass::Array)
			{
				if (!pEBld || pEBld->InLimbo || !pEBld->IsAlive || !pEBld->Owner || !pEBld->Type
					|| pEBld->DrainingMe != nullptr)
				{
					continue;
				}

				if (pEBld->Owner == pHouse || pHouse->IsAlliedWith(pEBld->Owner) || pEBld->Owner->IsNeutral())
					continue;

				if (!pEBld->GetCell() || !IsCellVisibleToAI(pHouse, pEBld->GetCell()))
					continue;

				int dScore = 0;

				if (pEBld->Type->SuperWeapon != -1)
					dScore = 100;
				else if (pEBld->Type->Refinery)
					dScore = 90;
				else if (pEBld->Type->PowerBonus > 0)
					dScore = 85;
				else if (pEBld->Type->IsBaseDefense && pEBld->Type->Powered)
					dScore = 75;

				if (dScore > bestDrainScore)
				{
					bestDrainScore = dScore;

					pBestDrainBld = pEBld;
				}
			}

			if (pBestDrainBld)
			{
				DispatchFootToTargetOrTransport(pUnit, pBestDrainBld, Mission::Attack, true);

				continue;
			}
		}

		// Generalized Deployable OpenTopped Bunker Vehicles across any mod
		// (IsSimpleDeployer with Convert.Deploy / Convert.UniversalDeploy ->
		// OpenTopped transport/bunker): Before deploying into an immobile
		// OpenTopped bunker, ensure we are >= 4 cells away from any
		// Factory/Refinery/ConYard exit so we never block production doors!
		if (isUnitIdle && !pType->Naval && pType->IsSimpleDeployer && !pType->OpenTopped
			&& (IsAITickWindow(90, 25)))
		{
			TechnoTypeClass* pConvTarget = GetConvertDeployTargetType(pType);
			if (pConvTarget && pConvTarget->OpenTopped && pConvTarget->Passengers > 0 && !HasNoManualEnter(pConvTarget)
				&& !HasNoManualUnload(pConvTarget))
			{
				const CellStruct uc = pUnit->GetMapCoords();
				bool tooCloseToBaseDoor = false;
				for (auto const pOwnB : pHouse->Buildings)
				{
					if (!pOwnB || pOwnB->InLimbo || !pOwnB->IsAlive || !pOwnB->Type)
						continue;

					if (pOwnB->Type->Factory != AbstractType::None || pOwnB->Type->Refinery
						|| pOwnB->Type->ConstructionYard)
					{
						const CellStruct bc = pOwnB->GetMapCoords();
						const int bw = pOwnB->Type->GetFoundationWidth();
						const int bh = pOwnB->Type->GetFoundationHeight(false);
						if (uc.X >= bc.X - 3 && uc.X <= bc.X + bw + 2 && uc.Y >= bc.Y - 3 && uc.Y <= bc.Y + bh + 3)
						{
							tooCloseToBaseDoor = true;
							break;
						}
					}
				}

				if (tooCloseToBaseDoor)
				{
					pUnit->Scatter(CoordStruct::Empty, true, false);
					continue;
				}

				int nearbyFriendlyInf = 0;
				for (auto const pOwnI : InfantryClass::Array)
				{
					if (pOwnI && pOwnI->Owner == pHouse && !pOwnI->InLimbo && pOwnI->IsAlive && pOwnI->Type
						&& !pOwnI->Type->Engineer && !IsSpyInfantryType(pOwnI->Type) && pOwnI->DistanceFrom(pUnit) <= 2816)
					{
						nearbyFriendlyInf++;
					}
				}

				if (nearbyFriendlyInf >= 2 || pUnit->Passengers.NumPassengers > 0)
				{
					pUnit->QueueMission(Mission::Unload, true);
					continue;
				}
			}
		}

		// Armed Vehicles (any unit with Size <= transport SizeLimit) boarding
		// OpenTopped transports (Slot 53):
		const int unitPassSize = std::max(1, static_cast<int>(pType->TechnoTypeClass::Size));
		if (isUnitIdle && !pType->Naval && !pType->Harvester && !pType->ResourceGatherer && !pType->OpenTopped
			&& !pType->Gunner && pType->Passengers == 0 && pType->DeploysInto == nullptr
			&& GetConvertDeployTargetType(pType) == nullptr && pPrimaryWpn && pPrimaryWpn->Damage > 0
			&& (IsAITickWindow(90, 25)))
		{
			bool unitBoardedOpenTopped = false;
			for (auto const pOTVeh : UnitClass::Array)
			{
				if (!pOTVeh || pOTVeh == pUnit || pOTVeh->Owner != pHouse || pOTVeh->InLimbo || !pOTVeh->IsAlive
					|| !pOTVeh->Type)
				{
					continue;
				}

				if (!pOTVeh->Type->OpenTopped || HasNoManualEnter(pOTVeh->Type) || HasNoManualUnload(pOTVeh->Type))
					continue;

				if (unitPassSize > static_cast<int>(pOTVeh->Type->SizeLimit))
					continue;

				int pendingCount = 0;
				int pendingSize = 0;
				for (auto const pOtherI : InfantryClass::Array)
				{
					if (pOtherI && pOtherI->Owner == pHouse && !pOtherI->InLimbo && pOtherI->IsAlive
						&& pOtherI->CurrentMission == Mission::Enter && pOtherI->Destination == pOTVeh)
					{
						pendingCount++;
						pendingSize += std::max(1, static_cast<int>(pOtherI->Type->TechnoTypeClass::Size));
					}
				}

				for (auto const pOtherU : UnitClass::Array)
				{
					if (pOtherU && pOtherU != pUnit && pOtherU->Owner == pHouse && !pOtherU->InLimbo && pOtherU->IsAlive
						&& pOtherU->CurrentMission == Mission::Enter && pOtherU->Destination == pOTVeh)
					{
						pendingCount++;
						pendingSize += std::max(1, static_cast<int>(pOtherU->Type->TechnoTypeClass::Size));
					}
				}

				if (CanPassengerFitInTransport(pOTVeh, pType, pendingCount, pendingSize)
					&& pUnit->DistanceFrom(pOTVeh) <= 2560)
				{
					DispatchFootToTargetOrTransport(pUnit, pOTVeh, Mission::Enter, false);
					unitBoardedOpenTopped = true;
					break;
				}
			}

			if (unitBoardedOpenTopped)
				continue;
		}

		// Tank Bunker Garrisoning (Bunkerable=yes into empty owned Tank Bunker):
		// Fortify base perimeter!
		if (pType->Bunkerable && isUnitIdle && pUnit->BunkerLinkedItem == nullptr && (IsAITickWindow(90, 20)))
		{
			for (auto const pBld : pHouse->Buildings)
			{
				if (!pBld || pBld->InLimbo || !pBld->IsAlive || !pBld->Type || !pBld->Type->Bunker
					|| pBld->BunkerLinkedItem != nullptr)
				{
					continue;
				}

				if (pUnit->DistanceFrom(pBld) <= 3072)
				{
					DispatchFootToTargetOrTransport(pUnit, pBld, Mission::Enter, false);

					break;
				}
			}
		}

		// Respond to allied Beacon with idle combat units
		if (pBeaconCell && isUnitIdle)
		{
			DispatchFootToTargetOrTransport(pUnit, pBeaconCell, Mission::AttackMove, false);
		}
	}

	microClock.Mark(NeuralSection::MicroInfantry);

	// Special Operations: Spies, Bridge Repair, Urban Garrisoning, Bio-Reactors,
	// Tesla Overpowering, Hospitals, Medics, Crazy Ivan & Bridge Demolition
	// The learned gate (slot 21) is asked only when a hut is worth working: a smooth gate keeps a
	// gradient and reaches the trainer's gate report, where a fixed 400 threshold above the
	// learned score kept every repair from happening.
	int targetBridgeSpan = -1;
	BuildingClass* pTargetBridgeHut = PickBridgeToRepair(pHouse, targetBridgeSpan);

	if (pTargetBridgeHut && !SlotGate(scores[21], 400, 21))
		pTargetBridgeHut = nullptr;

	// A unit whose warhead repairs a bridge from its hut does it in one hit, no engineer needed.
	if (pTargetBridgeHut)
	{
		for (auto const pFoot : FootClass::Array)
		{
			if (!pFoot || pFoot->Owner != pHouse || !pFoot->IsAlive || pFoot->InLimbo
				|| IsFootActivelyAttackingOrMovingToEnemy(pFoot)
				|| pFoot->DistanceFrom(pTargetBridgeHut) > 40 * Unsorted::LeptonsPerCell
				|| !CanWorkBridgeFromHut(pFoot, pTargetBridgeHut))
			{
				continue;
			}

			NoteBridgeWorker(pHouse, targetBridgeSpan, pFoot,
				DispatchFootToTargetOrTransport(pFoot, pTargetBridgeHut, Mission::Attack, true));
			pTargetBridgeHut = nullptr;

			break;
		}
	}

	BuildingClass* pBioReactor = nullptr;
	BuildingClass* pOverpowerCoil = nullptr;

	for (auto const pBld : BuildingClass::Array)
	{
		if (!pBld || pBld->InLimbo || !pBld->IsAlive || !pBld->Type)
			continue;

		if (pBld->Owner && pHouse->IsAlliedWith(pBld->Owner))
		{
			if (pBld->Owner == pHouse && pBld->Type->InfantryAbsorb
				&& (pBld->Type->PowerBonus > 0 || pBld->Type->ExtraPowerBonus > 0)
				&& !IsTunnelNetworkBuilding(pBld->Type) && pBld->Passengers.NumPassengers < pBld->Type->Passengers)
			{
				if (!pBioReactor || pBld->Passengers.NumPassengers < pBioReactor->Passengers.NumPassengers)
					pBioReactor = pBld;
			}

			if (!pOverpowerCoil && pBld->Type->Overpowerable && (!pBld->IsOverpowered || pBld->Owner->HasLowPower()))
				pOverpowerCoil = pBld; // Overpower own OR allied Tesla Coils!
		}
	}

	int infAlreadyHeadingToBioReactor = 0;
	if (pBioReactor)
	{
		for (auto const pCheckInf : InfantryClass::Array)
		{
			if (pCheckInf && pCheckInf->Owner == pHouse && !pCheckInf->InLimbo && pCheckInf->IsAlive
				&& pCheckInf->Destination == pBioReactor)
			{
				infAlreadyHeadingToBioReactor++;
			}
		}

		if (pBioReactor->Passengers.NumPassengers + infAlreadyHeadingToBioReactor >= pBioReactor->Type->Passengers)
			pBioReactor = nullptr;
	}

	const bool bioReactorNeedsPower
		= pBioReactor && (pHouse->HasLowPower() || (pHouse->PowerOutput - pHouse->PowerDrain) < 350);
	if (false)
	{
		{
		}
	}

	for (auto const pInf : InfantryClass::Array)
	{
		// Slaves work for their slave master, which overrides any order given to them.
		if (!pInf || pInf->Owner != pHouse || pInf->InLimbo || !pInf->IsAlive || !pInf->Type || pInf->SlaveOwner)
			continue;

		// Scouts on a trip take their orders from it (the dog leash and the patrols below would call
		// them home every tick).
		if (IsActiveScout(pHouse, pInf->UniqueID))
			continue;

		// A terrorist sneaking in takes its orders from UpdateSneakStrikes.
		if (IsSneakStrikeActive(pHouse, pInf->UniqueID))
			continue;

		// Evade radiation puddles (Desolator / Nuke fallout) immediately if not
		// radiation-immune; if we ARE a radiation-immune Deployer (Desolator,
		// Slot 63), tactically deploy to irradiate nearby enemy squads/slaves and
		// undeploy once the zone is cleared!
		if (auto const pInfCell = pInf->GetCell())
		{
			const int cellRad = pInfCell->GetRadLevel();

			if (cellRad > 10 && !pInf->Type->ImmuneToRadiation && pInf->CurrentMission != Mission::Move)
			{
				pInf->Scatter(CoordStruct::Empty, true, false);

				continue;
			}
			else if (pInf->Type->Deployer && pInf->Type->ImmuneToRadiation && SlotGate(scores[63], 80, 63)
				&& (IsAITickWindow(45, 15)))
			{
				int enemiesInRadRange = 0;
				ForEachTechnoNear(pInf->GetMapCoords(), 7, [&](TechnoClass* pET) -> bool
				{
					if (!pET->Owner || pET->WhatAmI() == AbstractType::Building || pET->WhatAmI() == AbstractType::Aircraft)
						return true;

					if (pET->Owner == pHouse || pHouse->IsAlliedWith(pET->Owner) || !IsCellVisibleToAI(pHouse, pET->GetCell()) || pET->Owner->IsNeutral())
						return true;

					if (pInf->DistanceFrom(pET) <= 1536)
						enemiesInRadRange++;

					return true;
				});

				if (!pInf->IsDeployed() && enemiesInRadRange >= 2 && cellRad < 180)
				{
					pInf->QueueMission(Mission::Unload, true);

					continue;
				}
				else if (pInf->IsDeployed() && enemiesInRadRange == 0 && cellRad >= 100)
				{
					pInf->QueueMission(Mission::Unload, true);

					continue;
				}
			}
		}

		// A soldier on a repair trip takes its orders from UpdateRepairTrips until it is back.
		if (IsOnRepairTrip(pHouse, pInf->UniqueID))
			continue;

		UnstickCrossIslandFoot(pInf);
		const bool isIdle = (pInf->CurrentMission == Mission::Guard || pInf->CurrentMission == Mission::Area_Guard)
			&& !IsFootActivelyAttackingOrMovingToEnemy(pInf);

		// Specialists flying a plan (airborne raid) take their orders from it.
		const bool inPlan = IsSpecialistInfantryType(pInf->Type) && IsPlanMember(pHouse, pInf->UniqueID);
		const auto pInfWpn = pInf->Type->GetWeapon(0) ? pInf->Type->GetWeapon(0)->WeaponType : nullptr;

		// Slot 82 (Infantry Human Shield & Anti-Engineer Recapture Screen for
		// Stolen ConYard / Escaping MCV): Nearby friendly combat infantry (<= 14
		// cells) immediately gun down enemy Engineers trying to recapture our
		// stolen ConYard, or form a human shield perimeter around the stolen
		// ConYard/MCV to absorb enemy fire during extraction!
		if (pStolenConYardOrMCV && !pInf->Type->Engineer && !IsSpyInfantryType(pInf->Type) && SlotGate(scores[82], 60, 82)
			&& pInf->DistanceFrom(pStolenConYardOrMCV) <= 3584)
		{
			if (pThreatToStolenAsset && pInfWpn && pInfWpn->Damage > 0 && pInf->Target != pThreatToStolenAsset)
			{
				DispatchFootToTargetOrTransport(pInf, pThreatToStolenAsset, Mission::Attack, true);

				continue;
			}
			else if (isIdle && pInf->DistanceFrom(pStolenConYardOrMCV) > 512)
			{
				if (auto const pShieldCell = pStolenConYardOrMCV->GetCell())
				{
					DispatchFootToTargetOrTransport(pInf, pShieldCell, Mission::Area_Guard, false);

					continue;
				}
			}
		}

		// Slot 57 (Combat Infantry Deploy): Deploy GI / Guardian GI / Desolator
		// (Deployer=yes) when actively engaging an enemy in combat!
		if (pInf->Type->Deployer && pInf->Target != nullptr && pInf->CurrentMission == Mission::Attack
			&& SlotGate(scores[57], 100, 57) && (IsAITickWindow(60, 20)))
		{
			pInf->QueueMission(Mission::Unload, true);

			continue;
		}

		// Slot 58 (Mind-Control Breaker): Snipers / Virus / ImmuneToPsionics
		// infantry eliminate enemy Yuri/Mastermind controllers!
		if (pEnemyMindController && isIdle && (pInf->Type->ImmuneToPsionics || (pInfWpn && pInfWpn->Range >= (6 * 256)))
			&& pInf->DistanceFrom(pEnemyMindController) <= 4096)
		{
			DispatchFootToTargetOrTransport(pInf, pEnemyMindController, Mission::Attack, true);

			continue;
		}

		// Slot 59 (Crate Pickup): Send any idle infantry (or non-engaged infantry near a crate)
		// to pick up the nearest visible reachable Crate within 14 cells!
		if ((isIdle || (!IsFootActivelyAttackingOrMovingToEnemy(pInf) && (IsAITickWindow(45, 15))))
			&& !pInf->Type->Engineer)
		{
			if (CellClass* pNearCrate = findNearestReachableCrateForFoot(pInf, 14))
			{
				pInf->SetDestination(pNearCrate, true);
				pInf->QueueMission(Mission::Move, true);
				continue;
			}
		}

		// Slot 65 (Grinder Mind-Control Recycling for Infantry): Send
		// mind-controlled infantry into our Grinder for instant credits and to free
		// Yuri's control slot!
		if (pInf->MindControlledBy != nullptr)
		{
			if (pBioReactor && pInf->Destination != pBioReactor
				&& (bioReactorNeedsPower || !pGrinderBld
					|| pInf->DistanceFrom(pBioReactor) <= pInf->DistanceFrom(pGrinderBld) + 1536))
			{
				DispatchFootToTargetOrTransport(pInf, pBioReactor, Mission::Enter, false);
				infAlreadyHeadingToBioReactor++;
				if (pBioReactor->Passengers.NumPassengers + infAlreadyHeadingToBioReactor
					>= pBioReactor->Type->Passengers)
				{
					pBioReactor = nullptr;
				}

				continue;
			}
		}

		if (pGrinderBld && pInf->MindControlledBy != nullptr && SlotGate(scores[65], 80, 65))
		{
			DispatchFootToTargetOrTransport(pInf, pGrinderBld, Mission::Enter, false);

			continue;
		}

		// Slot 68 (TiberiumHeal Infantry Regeneration & Sniper/Poison Specialist
		// Hunter):
		if (pInf->Type->TiberiumHeal && pInf->GetHealthPercentage() < 0.55 && SlotGate(scores[68], 60, 68)
			&& pInf->CurrentMission != Mission::Move)
		{
			if (pInf->MoveToTiberium(8, false))
				continue;
		}

		if (pEnemySpecialistInf && isIdle && pInfWpn && pInfWpn->Damage > 0 && !pInf->Type->Engineer
			&& ((pInfWpn->Warhead && pInfWpn->Warhead->Poison) || pInfWpn->Range >= (6 * 256)) && SlotGate(scores[68], 80, 68)
			&& pInf->DistanceFrom(pEnemySpecialistInf) <= 4096)
		{
			DispatchFootToTargetOrTransport(pInf, pEnemySpecialistInf, Mission::Attack, true);

			continue;
		}

		// Wounded valuable or veteran infantry (< 55% HP) retreats to a hospital, a medic or the
		// house self-heal (tryRepairRetreat).
		if (!pInf->Type->Engineer && (pInf->Type->Cost >= 300 || pInf->Veterancy.IsElite() || pInf->Veterancy.IsVeteran()
			|| (pInf->Type->Cost >= 200 && pInf->Type->Cost * 4 >= armyAverageCost * 5))
			&& pInf->CurrentMission != Mission::Enter && tryRepairRetreat(pInf, 0.55))
		{
			continue;
		}

		// Combat Medics / Field Mechanics (Damage < 0): Heal wounded own OR allied
		// infantry nearby!
		if (pInfWpn && pInfWpn->Damage < 0 && isIdle)
		{
			for (auto const pAllyInf : InfantryClass::Array)
			{
				if (!pAllyInf || pAllyInf == pInf || !pAllyInf->Owner || !pHouse->IsAlliedWith(pAllyInf->Owner)
					|| pAllyInf->InLimbo || !pAllyInf->IsAlive)
				{
					continue;
				}

				if (pAllyInf->GetHealthPercentage() < 0.85 && pInf->DistanceFrom(pAllyInf) <= 2560)
				{
					DispatchFootToTargetOrTransport(pInf, pAllyInf, Mission::Attack, true);

					break;
				}
			}

			continue;
		}

		// Chrono Legionnaire (Temporal=yes) & Crazy Ivan (Ivan=yes): Prioritize
		// high-cost enemy units/buildings!
		if (isIdle && ((pInfWpn && pInfWpn->Warhead && pInfWpn->Warhead->Temporal) || pInf->Type->Ivan))
		{
			TechnoClass* pBestSpecTarget = nullptr;
			int bestSpecCost = 800;

			for (auto const pEnemyU : UnitClass::Array)
			{
				if (!pEnemyU || pEnemyU->InLimbo || !pEnemyU->IsAlive || !pEnemyU->Owner || !pEnemyU->Type)
					continue;

				if (pEnemyU->Owner == pHouse || pHouse->IsAlliedWith(pEnemyU->Owner) || pEnemyU->Owner->IsNeutral())
					continue;

				if (pEnemyU->GetCell() && IsCellVisibleToAI(pHouse, pEnemyU->GetCell())
					&& pEnemyU->Type->Cost > bestSpecCost && pInf->DistanceFrom(pEnemyU) <= 4608)
				{
					bestSpecCost = pEnemyU->Type->Cost;

					pBestSpecTarget = pEnemyU;
				}
			}

			if (pBestSpecTarget)
			{
				DispatchFootToTargetOrTransport(pInf, pBestSpecTarget, Mission::Attack, true);

				continue;
			}
		}

		// Send idle Engineer to disarm Ivan Bombs on our own OR allied
		// buildings/tanks, repair destroyed bridges, or capture Neutral Tech
		// Buildings (Oil Derricks, Tech Outposts, Airports) and damaged enemy
		// buildings (respecting Multi-Engineer)
		if (pInf->Type->Engineer && isIdle && !inPlan)
		{
			// Disarm any attached Crazy Ivan Bomb on our own OR allied high-value
			// buildings!
			bool disarmedBomb = false;

			for (auto const pAllyBld : BuildingClass::Array)
			{
				if (pAllyBld && !pAllyBld->InLimbo && pAllyBld->IsAlive && pAllyBld->Owner
					&& pHouse->IsAlliedWith(pAllyBld->Owner) && pAllyBld->AttachedBomb != nullptr
					&& pInf->DistanceFrom(pAllyBld) <= 3584)
				{
					DispatchFootToTargetOrTransport(pInf, pAllyBld, Mission::Attack, true);

					disarmedBomb = true;

					break;
				}
			}

			if (disarmedBomb)
				continue;

			if (pTargetBridgeHut)
			{
				NoteBridgeWorker(pHouse, targetBridgeSpan, pInf,
					DispatchFootToTargetOrTransport(pInf, pTargetBridgeHut, Mission::Enter, false));

				pTargetBridgeHut = nullptr; // One engineer per hut is enough

				continue;
			}

			// Repair heavily damaged friendly-occupied or empty urban garrisonable
			// buildings (CanBeOccupied && Repairable) to 100% full health so our
			// infantry hold an unbreakable urban stronghold!
			if (pDamagedUrbanToRepair && pInf->DistanceFrom(pDamagedUrbanToRepair) <= 4096)
			{
				DispatchFootToTargetOrTransport(pInf, pDamagedUrbanToRepair, Mission::Enter, false);

				pDamagedUrbanToRepair = nullptr;

				continue;
			}

			auto const pHoarderExt = HouseExt::Fetch(pHouse);

			// Expert 4 (Special Operations): the best known safe neutral tech, with an escort first
			// when enemies were seen near it. Captures already under way finish under any expert.
			if (RunSpecOpsEngineer(pHouse, pInf, pHoarderExt && pHoarderExt->NeuralAI_ActiveExpert == 4,
				DispatchFootToTargetOrTransport))
			{
				continue;
			}

			const double capLevel = RulesClass::Instance ? RulesClass::Instance->EngineerCaptureLevel : 1.0;
			const int engCaptureBias = NeuralAI::PolicyFor(pHouse).OutputBias[36];
			// The resource hoarder (Expert 6) takes visible enemy oil derricks too, as eagerly as
			// neutral ones.
			const bool resourceHoarder = pHoarderExt && pHoarderExt->NeuralAI_ActiveExpert == 6;
			BuildingClass* pBestCapBld = nullptr;
			int bestCapDist = 999999;

			for (auto const pBld : BuildingClass::Array)
			{
				if (!pBld || pBld->InLimbo || !pBld->IsAlive || !pBld->Type || !pBld->Owner)
					continue;

				if (!pBld->Type->Capturable || pBld->Owner == pHouse || pHouse->IsAlliedWith(pBld->Owner))
					continue;

				auto const pCell = pBld->GetCell();

				if (!pCell)
					continue;

				const bool isNeutralTech = pBld->Owner->IsNeutral();

				// Fog-fair: neutral techs only once seen as neutral, out of known enemy defense reach
				// and not taken by an Expert 4 capture; enemy buildings require line of sight.
				if (isNeutralTech ? !IsSpecOpsTechAvailable(pHouse, pBld, pInf->UniqueID) : !IsCellVisibleToAI(pHouse, pCell))
					continue;

				const bool isCashTechBld = (pBld->Type->ProduceCashAmount > 0 || pBld->Type->ProduceCashStartup > 0);

				const bool isHighValueEnemyBld = !isNeutralTech
					&& (isCashTechBld || pBld->Type->ConstructionYard || pBld->Type->Refinery
						|| pBld->Type->Factory != AbstractType::None || pBld->Type->SuperWeapon != -1
						|| pBld->Type->PowerBonus > 0);

				const bool canCaptureNow = isNeutralTech || (pBld->GetHealthPercentage() <= capLevel + 0.05)
					|| (engCaptureBias > 60 && isHighValueEnemyBld) || (resourceHoarder && isCashTechBld)
					|| (SlotGate(scores[82], 60, 82) && pBld->Type->ConstructionYard);

				if (!canCaptureNow)
					continue;

				// Coordinate multiple Engineers: do not send a 2nd Engineer to a
				// neutral tech building already targeted by another friendly Engineer!
				if (isNeutralTech)
				{
					bool alreadyTargetedByOtherEng = false;
					for (auto const pOtherInf : InfantryClass::Array)
					{
						if (pOtherInf && pOtherInf != pInf && pOtherInf->Owner == pHouse && !pOtherInf->InLimbo
							&& pOtherInf->IsAlive && pOtherInf->Type && pOtherInf->Type->Engineer
							&& pOtherInf->Destination == pBld)
						{
							alreadyTargetedByOtherEng = true;
							break;
						}
					}

					if (alreadyTargetedByOtherEng)
						continue;
				}

				const int dist = pInf->DistanceFrom(pBld);

				// Strongly prioritize Cash-Producing Oil Derricks (ProduceCashAmount >
				// and BaseNormal expansion hubs!
				const bool isStrategicTechHub = isCashTechBld || BuildingTypeExt::IsAIBaseNormal(pBld->Type);
				const int effectiveDist
					= (!isNeutralTech && (pBld->Type->ConstructionYard || isCashTechBld)
						  && (SlotGate(scores[82], 60, 82) || (resourceHoarder && isCashTechBld)))
					? (dist / 3)
					: (isNeutralTech ? (isCashTechBld ? (dist / 3) : (isStrategicTechHub ? (dist * 2 / 5) : (dist / 2)))
									: dist);

				if (effectiveDist < bestCapDist)
				{
					bestCapDist = effectiveDist;

					pBestCapBld = pBld;
				}
			}

			if (pBestCapBld)
			{
				DispatchFootToTargetOrTransport(pInf, pBestCapBld, Mission::Enter, false);
			}

			continue;
		}

		// Spy disguise: it only fools the copied house and its allies, so a spy on its way wears
		// the colors of its target's owner, copied from one of that house's soldiers in sight.
		// Without a target yet (Slot 61) it copies any enemy soldier, which is right in a duel.
		if (pInf->Type->CanDisguise)
		{
			auto const pVictim = IsSpyInfantryType(pInf->Type) ? GetSpyVictim(pHouse, pInf) : nullptr;

			// Until a soldier of the victim is in sight, any enemy colors beat its own (they fool at
			// least that house on the way).
			const bool victimColors = pVictim && DisguiseSpyFor(pHouse, pInf, pVictim);

			if (!victimColors && pSampleEnemyInfantry && (pVictim || SlotGate(scores[61], 60, 61))
				&& !SpyDisguiseHouse(pInf) && IsCellInSightOfAI(pHouse, pSampleEnemyInfantry->GetMapCoords()))
			{
				CopySoldierDisguise(pHouse, pInf, pSampleEnemyInfantry);
			}
		}

		// Slot 62 (Slave Miner Liberation for Infantry): Redirect fire from Slaved
		// workers onto their SlaveOwner so killing the miner frees the slaves!
		if (auto const pCurTargetInf = abstract_cast<InfantryClass*>(pInf->Target))
		{
			if (pCurTargetInf->Type && pCurTargetInf->Type->Slaved && pCurTargetInf->SlaveOwner
				&& pCurTargetInf->SlaveOwner->IsAlive && !pCurTargetInf->SlaveOwner->InLimbo && SlotGate(scores[62], 60, 62))
			{
				DispatchFootToTargetOrTransport(pInf, pCurTargetInf->SlaveOwner, Mission::Attack, true);

				continue;
			}
		}

		// Spy Full-Spectrum Sabotage, Multi-Spy Role Coordination & Pre-Attack Power Plant Staging:
		// 1) Multiple Spies coordinate so each targets a distinct building/role:
		//    - Un-infiltrated War Factory / Barracks so newly built units spawn Veteran!
		//    - Enemy Radar when our AI lacks SpySat / radar vision on that enemy!
		//    - Enemy Refinery / Silos when we need credits and the enemy has funds!
		//    - Lightly guarded Enemy Power Plants to black out powered defenses!
		//    - Enemy Superweapons & Battle Labs (Secret Tech)!
		// 2) Pre-Attack Power Plant Blackout Staging: When targeting an enemy Power Plant
		//    while our army is assembling (not yet attacking that enemy house), wait disguised
		//    5..10 cells away if safe from detectors, and immediately infiltrate as soon as
		//    our attack wave launches/approaches (or if an enemy detector gets close)!
		if (IsSpyInfantryType(pInf->Type) && !inPlan)
		{
			BuildingClass* pStagedPowerBld = abstract_cast<BuildingClass*>(
				pInf->Destination ? pInf->Destination : pInf->ArchiveTarget);

			if (!pStagedPowerBld || !pStagedPowerBld->IsAlive || pStagedPowerBld->InLimbo || !pStagedPowerBld->Owner
				|| pHouse->IsAlliedWith(pStagedPowerBld->Owner) || pStagedPowerBld->Owner->IsNeutral()
				|| !pStagedPowerBld->Type || pStagedPowerBld->Type->PowerBonus <= pStagedPowerBld->Type->PowerDrain)
			{
				pStagedPowerBld = nullptr;

				if (isIdle)
				{
					for (auto const pCandPwr : BuildingClass::Array)
					{
						if (!pCandPwr || pCandPwr->InLimbo || !pCandPwr->IsAlive || !pCandPwr->Owner || !pCandPwr->Type)
							continue;

						if (pHouse->IsAlliedWith(pCandPwr->Owner) || !IsEnemyBuildingKnown(pHouse, pCandPwr) || pCandPwr->Owner->IsNeutral())
							continue;

						if (pCandPwr->Type->Spyable && pCandPwr->Type->PowerBonus > pCandPwr->Type->PowerDrain
							&& pInf->DistanceFrom(pCandPwr) <= 2816)
						{
							pStagedPowerBld = pCandPwr;
							break;
						}
					}
				}
			}

			// Waiting next to the base is only safe in the colors of the plant's owner.
			if (pStagedPowerBld && IsSpyDisguisedFor(pInf, pStagedPowerBld->Owner))
			{
				const int distToPwr = pInf->DistanceFrom(pStagedPowerBld);

				if (distToPwr >= 1024 && distToPwr <= 2816)
				{
					HouseClass* const pPwrOwner = pStagedPowerBld->Owner;
					// Revealed by a known detector building, or a dog / detector in sight close by: go in now.
					const bool detectorDangerNearSpy = pInf->HasBeenAttacked
						|| IsCellWatchedByDisguiseDetector(pHouse, pInf->GetMapCoords(), 0);

					// Powered defenses of that owner we have seen (fog-fair).
					bool enemyHasPoweredDefenses = false;
					for (auto const& info : GetKnownEnemyBuildingCache(pHouse).Entries)
					{
						if (info.IsDefense && info.Building->Owner == pPwrOwner
							&& (info.Building->Type->Powered || info.Building->Type->PoweredSpecial))
						{
							enemyHasPoweredDefenses = true;
							break;
						}
					}

					int friendlyWaveApproachingEnemy = 0;
					const CellStruct pwrCoords = pStagedPowerBld->GetMapCoords();

					for (auto const pOwnU : UnitClass::Array)
					{
						if (!pOwnU || pOwnU->Owner != pHouse || pOwnU->InLimbo || !pOwnU->IsAlive || !pOwnU->Type
							|| pOwnU->Type->Harvester || pOwnU->Type->ResourceGatherer)
						{
							continue;
						}

						if (pOwnU->GetMapCoords().DistanceFrom(pwrCoords) <= 26.0
							|| (IsFootActivelyAttackingOrMovingToEnemy(pOwnU)
								&& pOwnU->GetMapCoords().DistanceFrom(pwrCoords) <= 38.0))
						{
							friendlyWaveApproachingEnemy++;
							if (friendlyWaveApproachingEnemy >= 2)
								break;
						}
					}

					const bool blackoutAlreadyActive = (pPwrOwner->PowerBlackoutTimer.GetTimeLeft() > 150);
					const bool shouldWaitForWave = !detectorDangerNearSpy && enemyHasPoweredDefenses
						&& !blackoutAlreadyActive && friendlyWaveApproachingEnemy < 2
						&& (pHouse->OwnedUnits + pHouse->OwnedInfantry >= 4) && distToPwr >= 1280 && distToPwr <= 2560;

					if (shouldWaitForWave)
					{
						if (pInf->CurrentMission != Mission::Guard)
						{
							pInf->SetDestination(nullptr, true);
							pInf->QueueMission(Mission::Guard, true);
						}

						continue;
					}
					else if (isIdle && !blackoutAlreadyActive)
					{
						if (DispatchFootToTargetOrTransport(pInf, pStagedPowerBld, Mission::Enter, false))
							NoteSpecOpsSpy(pHouse, pInf, pStagedPowerBld);
						pExt->NeuralAI_InfiltratedEnemyMask |= (1 << std::clamp(pPwrOwner->ArrayIndex, 0, 7));
						continue;
					}
				}
			}
		}

		if (IsSpyInfantryType(pInf->Type) && isIdle && !inPlan)
		{
			BuildingClass* pBestSpyTarget = nullptr;
			int bestSpyScore = -9999;
			const int ourMoney = pHouse->Available_Money();
			// Per enemy house: 2 when the spy already wears its colors, 1 when one of its soldiers is
			// in sight to copy, 0 otherwise (-1: not computed yet). The disguise is what gets the spy
			// past that house's defenses, so those houses' buildings come first.
			std::array<int8_t, 32> disguiseFit {};
			disguiseFit.fill(-1);

			for (auto const pEnemyBld : BuildingClass::Array)
			{
				if (!pEnemyBld || pEnemyBld->InLimbo || !pEnemyBld->IsAlive || !pEnemyBld->Owner || !pEnemyBld->Type)
					continue;

				if (!pEnemyBld->Type->Spyable || pHouse->IsAlliedWith(pEnemyBld->Owner)
					|| pEnemyBld->Owner->IsNeutral() || !IsEnemyBuildingKnown(pHouse, pEnemyBld))
				{
					continue;
				}

				HouseClass* const pEnemyOwner = pEnemyBld->Owner;
				int spyScore = 0;

				if (pEnemyBld->Type->SuperWeapon != -1)
				{
					spyScore = enemySuperImminent ? 160 : 125;
				}
				else if (pEnemyBld->Type->Factory == AbstractType::UnitType && !pEnemyBld->Type->Naval)
				{
					// Unlock Veteran Vehicle production if our House hasn't infiltrated a War Factory yet!
					spyScore = !pHouse->WarFactoryInfiltrated ? 145 : 15;
				}
				else if (pEnemyBld->Type->Factory == AbstractType::InfantryType)
				{
					// Unlock Veteran Infantry production if our House hasn't infiltrated a Barracks yet!
					spyScore = !pHouse->BarracksInfiltrated ? 135 : 15;
				}
				else if (pEnemyBld->Type->Factory == AbstractType::AircraftType || pEnemyBld->Type->Helipad)
				{
					spyScore = 95;
				}
				else if (pEnemyBld->Type->Radar || pEnemyBld->Type->SpySat)
				{
					// Gain enemy Radar / map vision when our AI lacks SpySat and hasn't spied this enemy's radar!
					const bool alreadyHaveRadarVision
						= pHouse->SpySatActive || pEnemyOwner->RadarVisibleTo.Contains(pHouse->ArrayIndex);
					spyScore = !alreadyHaveRadarVision ? 130 : 35;
				}
				else if (pEnemyBld->Type->Refinery || pEnemyBld->Type->Storage > 0)
				{
					// The enemy's money is hidden: a refinery pays most when we are short ourselves.
					spyScore = (ourMoney < 2500) ? 150 : 105;
				}
				else if (pEnemyBld->Type->PowerBonus > pEnemyBld->Type->PowerDrain)
				{
					if (pEnemyOwner->PowerBlackoutTimer.GetTimeLeft() > 150)
					{
						spyScore = 15;
					}
					else
					{
						// Powered defenses of that owner we have seen (fog-fair).
						bool ownerHasPoweredDef = false;
						for (auto const& info : GetKnownEnemyBuildingCache(pHouse).Entries)
						{
							if (info.IsDefense && info.Building->Owner == pEnemyOwner
								&& (info.Building->Type->Powered || info.Building->Type->PoweredSpecial))
							{
								ownerHasPoweredDef = true;
								break;
							}
						}

						spyScore = ownerHasPoweredDef ? 142 : 115;
					}
				}
				else if (pEnemyBld->Type->TechLevel >= 6)
				{
					const int eSide = pEnemyOwner->SideIndex;
					const bool needStolenTech = (eSide == 0 && !pHouse->Side0TechInfiltrated)
						|| (eSide == 1 && !pHouse->Side1TechInfiltrated)
						|| (eSide == 2 && !pHouse->Side2TechInfiltrated);
					spyScore = needStolenTech ? 128 : 55;
				}

				// A custom infiltration (Ares SpyEffect.* tags) is worth what its effects give.
				if (auto const pSpyBldExt = BuildingTypeExt::Fetch(pEnemyBld->Type); pSpyBldExt && pSpyBldExt->SpyEffect_AIWorth >= 0)
					spyScore = pSpyBldExt->SpyEffect_AIWorth;

				if (spyScore <= 0)
					continue;

				const size_t ownerSlot = static_cast<size_t>(std::clamp(pEnemyOwner->ArrayIndex, 0, 31));

				if (disguiseFit[ownerSlot] < 0)
				{
					disguiseFit[ownerSlot] = IsSpyDisguisedFor(pInf, pEnemyOwner) ? 2
						: (pInf->Type->CanDisguise && FindSpyDisguiseSource(pHouse, pInf, pEnemyOwner)) ? 1 : 0;
				}

				spyScore += disguiseFit[ownerSlot] == 2 ? 40 : (disguiseFit[ownerSlot] == 1 ? 20 : 0);

				// De-duplicate across multiple friendly Spies so each Spy picks a distinct target/role!
				for (auto const pOtherSpy : InfantryClass::Array)
				{
					if (!pOtherSpy || pOtherSpy == pInf || pOtherSpy->Owner != pHouse || pOtherSpy->InLimbo
						|| !pOtherSpy->IsAlive || !pOtherSpy->Type || !IsSpyInfantryType(pOtherSpy->Type))
					{
						continue;
					}

					if (pOtherSpy->Destination == pEnemyBld || pOtherSpy->ArchiveTarget == pEnemyBld
						|| LookupPassengerIslandGoal(pOtherSpy->UniqueID) == pEnemyBld->GetMapCoords()
						|| (pOtherSpy->CurrentMission == Mission::Guard && pOtherSpy->DistanceFrom(pEnemyBld) <= 2816))
					{
						spyScore -= 500;
						break;
					}
				}

				// Prefer lightly guarded buildings: detectors covering it (known DetectDisguise= buildings,
				// dogs and detector units in sight; mind controllers count twice, they take the spy over)
				// strip the disguise at the door, detectors on the straight walk in do it on the way, and
				// defense turrets shoot a revealed spy.
				const CellStruct spyTargetCell = pEnemyBld->GetMapCoords();
				const int detectorPenalty = DisguiseDetectorPenaltyAt(pHouse, spyTargetCell)
					+ DisguiseDetectorPenaltyOnWay(pHouse, pInf->GetMapCoords(), spyTargetCell);

				int turretPenalty = 0;
				for (auto const pDefB : pEnemyOwner->Buildings)
				{
					if (pDefB && !pDefB->InLimbo && pDefB->IsAlive && pDefB->Type && pDefB->Type->IsBaseDefense
						&& pEnemyBld->DistanceFrom(pDefB) <= 1792 && IsEnemyBuildingKnown(pHouse, pDefB))
					{
						turretPenalty += 10;
					}
				}

				spyScore -= std::min(300, detectorPenalty + turretPenalty);
				spyScore -= (pInf->DistanceFrom(pEnemyBld) / 1024);

				if (spyScore > bestSpyScore)
				{
					bestSpyScore = spyScore;

					pBestSpyTarget = pEnemyBld;
				}
			}

			// Every target is watched (a Psychic Sensor covering the whole base, mind controllers at
			// the doors): the spy waits for the detectors to fall or another house to open up.
			if (pBestSpyTarget && bestSpyScore > 0)
			{
				// While the spy fires its disguise kit at a soldier of the owner it is not sent yet (the
				// walk order would cancel the shot); it is idle again right after and goes then.
				const bool copying = DisguiseSpyFor(pHouse, pInf, pBestSpyTarget->Owner)
					&& !IsSpyDisguisedFor(pInf, pBestSpyTarget->Owner);

				if (!copying && DispatchFootToTargetOrTransport(pInf, pBestSpyTarget, Mission::Enter, false))
					NoteSpecOpsSpy(pHouse, pInf, pBestSpyTarget);

				pExt->NeuralAI_InfiltratedEnemyMask |= (1 << std::clamp(pBestSpyTarget->Owner->ArrayIndex, 0, 7));
			}

			continue;
		}

		// Credit Thief (Thief=yes): Coordinate multiple Thieves to infiltrate distinct
		// enemy Refineries & Ore Silos (preferring solvent enemy houses)!
		if (pInf->Type->Thief && !IsSpyInfantryType(pInf->Type) && isIdle && !inPlan)
		{
			BuildingClass* pBestThiefTarget = nullptr;
			int bestThiefScore = -999999;

			for (auto const pEnemyBld : BuildingClass::Array)
			{
				if (!pEnemyBld || pEnemyBld->InLimbo || !pEnemyBld->IsAlive || !pEnemyBld->Owner || !pEnemyBld->Type)
					continue;

				if (pHouse->IsAlliedWith(pEnemyBld->Owner) || !IsEnemyBuildingKnown(pHouse, pEnemyBld) || pEnemyBld->Owner->IsNeutral())
					continue;

				if (pEnemyBld->Type->Refinery || pEnemyBld->Type->Storage > 0)
				{
					int thiefScore = 10000 - (pInf->DistanceFrom(pEnemyBld) / 16);

					if (pEnemyBld->Owner->Available_Money() >= 600)
						thiefScore += 3000;

					for (auto const pOtherThief : InfantryClass::Array)
					{
						if (pOtherThief && pOtherThief != pInf && pOtherThief->Owner == pHouse && !pOtherThief->InLimbo
							&& pOtherThief->IsAlive && pOtherThief->Type && pOtherThief->Type->Thief
							&& (pOtherThief->Destination == pEnemyBld || pOtherThief->ArchiveTarget == pEnemyBld))
						{
							thiefScore -= 8000;
							break;
						}
					}

					if (thiefScore > bestThiefScore)
					{
						bestThiefScore = thiefScore;

						pBestThiefTarget = pEnemyBld;
					}
				}
			}

			if (pBestThiefTarget)
			{
				DispatchFootToTargetOrTransport(pInf, pBestThiefTarget, Mission::Enter, false);

				continue;
			}
		}

		// Hijacker (VehicleThief=yes): Coordinate multiple Hijackers so each steals a
		// distinct expensive visible enemy tank (Apocalypse, Mammoth Mk.II, Avatar, Prism Tank)!
		if (pInf->Type->VehicleThief && isIdle)
		{
			UnitClass* pBestStealTank = nullptr;
			int bestTankScore = 700;

			for (auto const pEnemyU : UnitClass::Array)
			{
				if (!pEnemyU || pEnemyU->InLimbo || !pEnemyU->IsAlive || !pEnemyU->Owner || !pEnemyU->Type)
					continue;

				if (pEnemyU->Owner == pHouse || pHouse->IsAlliedWith(pEnemyU->Owner) || pEnemyU->Owner->IsNeutral())
					continue;

				if (pEnemyU->Type->Naval || pEnemyU->Type->Organic || pEnemyU->ParasiteEatingMe != nullptr)
					continue;

				if (pEnemyU->GetCell() && IsCellVisibleToAI(pHouse, pEnemyU->GetCell())
					&& pInf->DistanceFrom(pEnemyU) <= 5120)
				{
					bool alreadyTargetedByOtherHijacker = false;

					for (auto const pOtherHij : InfantryClass::Array)
					{
						if (pOtherHij && pOtherHij != pInf && pOtherHij->Owner == pHouse && !pOtherHij->InLimbo
							&& pOtherHij->IsAlive && pOtherHij->Type && pOtherHij->Type->VehicleThief
							&& (pOtherHij->Destination == pEnemyU || pOtherHij->ArchiveTarget == pEnemyU))
						{
							alreadyTargetedByOtherHijacker = true;
							break;
						}
					}

					const int stealScore = pEnemyU->Type->Cost - (alreadyTargetedByOtherHijacker ? 5000 : 0);

					if (stealScore > bestTankScore)
					{
						bestTankScore = stealScore;

						pBestStealTank = pEnemyU;
					}
				}
			}

			if (pBestStealTank)
			{
				DispatchFootToTargetOrTransport(pInf, pBestStealTank, Mission::Enter, false);

				continue;
			}
		}

		// Slot 61 (Guard Dogs & Counter-Spy Detectors - Multi-Infiltrator Interception):
		// Distribute Dogs/Detectors across all detected enemy Spies, Engineers, Thieves,
		// Hijackers, or Commandos approaching our base!
		if ((pInf->Type->Doggie || pInf->Type->DetectDisguise || pInf->Type->Sensors) && isIdle)
		{
			if (detectedInfiltratorCount > 0)
			{
				TechnoClass* pBestInfiltrator = nullptr;
				int bestInfDistScore = 999999;

				for (int di = 0; di < detectedInfiltratorCount; ++di)
				{
					auto const pCandInf = detectedEnemyInfiltrators[di];

					if (!pCandInf || pCandInf->InLimbo || !pCandInf->IsAlive)
						continue;

					const int d = pInf->DistanceFrom(pCandInf);

					if (d > 4608)
						continue;

					int dogsAlreadyChasing = 0;

					for (auto const pOtherDog : InfantryClass::Array)
					{
						if (pOtherDog && pOtherDog != pInf && pOtherDog->Owner == pHouse && !pOtherDog->InLimbo
							&& pOtherDog->IsAlive && (pOtherDog->Target == pCandInf || pOtherDog->Destination == pCandInf))
						{
							dogsAlreadyChasing++;
						}
					}

					const int distScore = d + (dogsAlreadyChasing * 1536);

					if (distScore < bestInfDistScore)
					{
						bestInfDistScore = distScore;
						pBestInfiltrator = pCandInf;
					}
				}

				if (pBestInfiltrator)
				{
					DispatchFootToTargetOrTransport(pInf, pBestInfiltrator, Mission::Attack, true);

					continue;
				}
			}

			if (pHiddenOrStealthEnemy && (IsAITickWindow(90, 25)))
			{
				if (auto const pSweepCell = pHiddenOrStealthEnemy->GetCell())
				{
					DispatchFootToTargetOrTransport(pInf, pSweepCell, Mission::AttackMove, false);

					continue;
				}
			}

			// Expert 2: Patrol idle defensive infantry & dogs around fragile key buildings
			// (Superweapons, Power Plants, Radar/SpySat, Factories, ConYard)!
			if ((Unsorted::CurrentFrame + pInf->UniqueID * 13) % 180 < 20)
			{
				BuildingClass* pPatrolBld = nullptr;
				const int patrolSlot = ((Unsorted::CurrentFrame / 180) + pInf->UniqueID) % 7;
				int seenKeyBlds = 0;

				for (auto const pOwnB : pHouse->Buildings)
				{
					if (!pOwnB || pOwnB->InLimbo || !pOwnB->IsAlive || !pOwnB->Type)
						continue;

					if (pOwnB->Type->SuperWeapon != -1 || pOwnB->Type->PowerBonus > pOwnB->Type->PowerDrain
						|| pOwnB->Type->SpySat || pOwnB->Type->Factory != AbstractType::None
						|| pOwnB->Type->ConstructionYard)
					{
						pPatrolBld = pOwnB;
						if (seenKeyBlds == patrolSlot)
							break;

						seenKeyBlds++;
					}
				}

				if (pPatrolBld && pInf->DistanceFrom(pPatrolBld) > 768 && pInf->DistanceFrom(pPatrolBld) <= 6144)
				{
					if (auto const pPatrolCell = pPatrolBld->GetCell())
					{
						OrderFootToAttackMove(pInf, pPatrolCell);

						continue;
					}
				}
			}

			if (pInf->Type->Doggie && homeCoords != CellStruct::Empty && (IsAITickWindow(120, 20)))
			{
				if (auto const pHomeCell = MapClass::Instance.TryGetCellAt(homeCoords))
				{
					if (pInf->DistanceFrom(pHomeCell) > 6144)
					{
						DispatchFootToTargetOrTransport(pInf, pHomeCell, Mission::Area_Guard, false);
					}
				}

				continue;
			}
		}

		// C4 Commandos (Tanya, Navy SEAL, Boris, Cyborg Commando): Coordinate multiple
		// Commandos to demolish distinct visible enemy Warships (if amphibious) or buildings!
		if (pInf->Type->C4 && isIdle)
		{
			TechnoClass* pBestC4Target = nullptr;
			int bestC4Dist = 5632;

			const bool canSwim = (pInf->Type->MovementZone == MovementZone::Amphibious
				|| pInf->Type->MovementZone == MovementZone::AmphibiousDestroyer
				|| pInf->Type->SpeedType == SpeedType::Hover);

			auto IsTargetedByOtherC4 = [pInf, pHouse](const TechnoClass* pCand) -> bool
			{
				for (auto const pOtherC4 : InfantryClass::Array)
				{
					if (pOtherC4 && pOtherC4 != pInf && pOtherC4->Owner == pHouse && !pOtherC4->InLimbo
						&& pOtherC4->IsAlive && pOtherC4->Type && pOtherC4->Type->C4
						&& (pOtherC4->Target == pCand || pOtherC4->Destination == pCand))
					{
						return true;
					}
				}

				return false;
			};

			if (canSwim)
			{
				for (auto const pEnemyShip : UnitClass::Array)
				{
					if (!pEnemyShip || pEnemyShip->InLimbo || !pEnemyShip->IsAlive || !pEnemyShip->Owner
						|| !pEnemyShip->Type || !pEnemyShip->Type->Naval)
					{
						continue;
					}

					if (pEnemyShip->Owner == pHouse || pHouse->IsAlliedWith(pEnemyShip->Owner)
						|| pEnemyShip->Owner->IsNeutral())
					{
						continue;
					}

					if (pEnemyShip->GetCell() && IsCellVisibleToAI(pHouse, pEnemyShip->GetCell()))
					{
						const int d = pInf->DistanceFrom(pEnemyShip) + (IsTargetedByOtherC4(pEnemyShip) ? 2560 : 0);

						if (d < bestC4Dist)
						{
							bestC4Dist = d;

							pBestC4Target = pEnemyShip;
						}
					}
				}
			}

			if (!pBestC4Target && surgicalEnemyBldCount > 0)
			{
				for (int si = 0; si < surgicalEnemyBldCount; ++si)
				{
					auto const pCandBld = surgicalEnemyBlds[si];

					if (!pCandBld || pCandBld->InLimbo || !pCandBld->IsAlive || !pCandBld->Type->CanC4)
						continue;

					const int d = pInf->DistanceFrom(pCandBld) + (IsTargetedByOtherC4(pCandBld) ? 2560 : 0);

					if (d < bestC4Dist)
					{
						bestC4Dist = d;
						pBestC4Target = pCandBld;
					}
				}
			}

			if (!pBestC4Target && (pSurgicalEnemyBld || pPrioritySiegeBld))
				pBestC4Target = pSurgicalEnemyBld ? pSurgicalEnemyBld : pPrioritySiegeBld;

			if (pBestC4Target)
			{
				DispatchFootToTargetOrTransport(pInf, pBestC4Target, Mission::Attack, true);

				continue;
			}
		}

		// IFV (Gunner=yes), OpenTopped Transports/Bunkers (OpenTopped=yes) &
		// Subterranean/Ground APC Strike Team Boarding (Slot 53)! Generalized &
		// cross-faction agnostic: any armed infantry of any faction (native or
		// unlocked from a foreign tech tree) can board any owned OpenTopped or
		// Gunner vehicle, while strictly respecting NoManualEnter & NoManualUnload!
		const int apcBoardBias = static_cast<int>(scores[53]);
		const int apcBoardWindow = std::clamp(25 + (apcBoardBias / 18), 15, 55);
		const int apcBoardRange = std::clamp(2560 + apcBoardBias * 2, 1792, 4096);
		const auto pInfWpn1 = pInf->Type->GetWeapon(1) ? pInf->Type->GetWeapon(1)->WeaponType : nullptr;
		const auto pInfEliteWpn0 = pInf->Type->GetEliteWeapon(0) ? pInf->Type->GetEliteWeapon(0)->WeaponType : nullptr;
		const auto pInfEliteWpn1 = pInf->Type->GetEliteWeapon(1) ? pInf->Type->GetEliteWeapon(1)->WeaponType : nullptr;
		const WeaponTypeClass* pBestInfGun = nullptr;
		for (auto const pCandW : { pInfWpn, pInfWpn1, pInfEliteWpn0, pInfEliteWpn1 })
		{
			if (pCandW && pCandW->Damage > 0 && (!pBestInfGun || pCandW->Range > pBestInfGun->Range))
				pBestInfGun = pCandW;
		}

		// Immediately man any owned under-filled Battle Bunker (IsBaseDefense && CanBeOccupied)
		// with nearby compatible Occupier infantry before considering APC boarding or offensive waves
		if (isIdle && pInf->Type->Occupier && !pInf->Type->Engineer && !IsSpyInfantryType(pInf->Type))
		{
			const int infZoneForBunker
				= ObjectZone(pInf, MovementZone::Normal);
			BuildingClass* pBestOwnedBunker = nullptr;
			int bestOwnedBunkerDist = 4608;

			for (auto const pBld : pHouse->Buildings)
			{
				if (!pBld || pBld->InLimbo || !pBld->IsAlive || !pBld->Type || !pBld->Type->IsBaseDefense
					|| !pBld->Type->CanBeOccupied)
				{
					continue;
				}

				const int maxOcc = std::clamp(pBld->Type->MaxNumberOccupants, 1, 10);

				if (!pBld->CanBeOccupiedBy(pInf) || pBld->Occupants.Count >= maxOcc || pBld->GetHealthPercentage() < 0.30)
					continue;

				const int bz
					= ObjectZone(pBld, MovementZone::Normal);

				if (infZoneForBunker >= 0 && bz >= 0 && bz != infZoneForBunker)
					continue;

				int pendingInfEntering = 0;

				for (auto const pOtherI : InfantryClass::Array)
				{
					if (pOtherI && pOtherI != pInf && pOtherI->Owner == pHouse && !pOtherI->InLimbo && pOtherI->IsAlive
						&& (pOtherI->CurrentMission == Mission::Enter || pOtherI->QueuedMission == Mission::Enter)
						&& (pOtherI->Destination == pBld || pOtherI->ArchiveTarget == pBld))
					{
						pendingInfEntering++;
					}
				}

				if (pBld->Occupants.Count + pendingInfEntering >= maxOcc)
					continue;

				const int d = pInf->DistanceFrom(pBld);

				if (d <= bestOwnedBunkerDist)
				{
					bestOwnedBunkerDist = d;
					pBestOwnedBunker = pBld;
				}
			}

			if (pBestOwnedBunker)
			{
				OrderFootToEnter(pInf, pBestOwnedBunker);
				continue;
			}
		}

		if (isIdle && !IsSpyInfantryType(pInf->Type) && (IsAITickWindow(90, apcBoardWindow)))
		{
			bool boardedCombatVehicle = false;
			for (auto const pCombVeh : UnitClass::Array)
			{
				if (!pCombVeh || pCombVeh->Owner != pHouse || pCombVeh->InLimbo || !pCombVeh->IsAlive
					|| !pCombVeh->Type)
				{
					continue;
				}

				if (HasNoManualEnter(pCombVeh->Type) || HasNoManualUnload(pCombVeh->Type))
					continue;

				const int infPassengerSize = std::max(1, static_cast<int>(pInf->Type->TechnoTypeClass::Size));
				if (infPassengerSize > static_cast<int>(pCombVeh->Type->SizeLimit))
					continue;

				int pendingBoardersCount = 0;
				int pendingBoardersSize = 0;
				for (auto const pOtherInf : InfantryClass::Array)
				{
					if (pOtherInf && pOtherInf != pInf && pOtherInf->Owner == pHouse && !pOtherInf->InLimbo
						&& pOtherInf->IsAlive && pOtherInf->CurrentMission == Mission::Enter
						&& pOtherInf->Destination == pCombVeh)
					{
						pendingBoardersCount++;
						pendingBoardersSize += std::max(1, static_cast<int>(pOtherInf->Type->TechnoTypeClass::Size));
					}
				}

				for (auto const pOtherU : UnitClass::Array)
				{
					if (pOtherU && pOtherU != pCombVeh && pOtherU->Owner == pHouse && !pOtherU->InLimbo
						&& pOtherU->IsAlive && pOtherU->CurrentMission == Mission::Enter
						&& pOtherU->Destination == pCombVeh)
					{
						pendingBoardersCount++;
						pendingBoardersSize += std::max(1, static_cast<int>(pOtherU->Type->TechnoTypeClass::Size));
					}
				}

				if (!CanPassengerFitInTransport(pCombVeh, pInf->Type, pendingBoardersCount, pendingBoardersSize))
					continue;

				const bool isUsefulOpenToppedGunner = !pInf->Type->Engineer && !pInf->Type->Doggie && !pInf->Type->Thief
					&& !pInf->Type->VehicleThief && !pInf->Type->Ivan && pBestInfGun != nullptr
					&& pBestInfGun->Damage > 0 && pBestInfGun->Range >= 384;

				const bool isUsefulIFVPassenger = !pInf->Type->Doggie && !pInf->Type->Thief && !pInf->Type->VehicleThief
					&& (pBestInfGun != nullptr || pInf->Type->IFVMode > 0)
					&& (!pInf->Type->Engineer || pParasitedOwnUnit != nullptr);

				const bool isGunnerIFV = (isUsefulIFVPassenger && pCombVeh->Type->Gunner
					&& (pCombVeh->Passengers.NumPassengers + pendingBoardersCount) == 0);
				const bool isOpenToppedFortress = (isUsefulOpenToppedGunner && pCombVeh->Type->OpenTopped
					&& (pCombVeh->Passengers.NumPassengers + pendingBoardersCount) < pCombVeh->Type->Passengers);
				const bool isSubterraneanOrGroundAPC
					= (!pCombVeh->Type->Naval && !pCombVeh->Type->Gunner && !pCombVeh->Type->OpenTopped
						&& pCombVeh->Type->Passengers >= 3 && IsTransportFreeForLandStrike(pHouse, pCombVeh)
						&& (pCombVeh->Passengers.NumPassengers + pendingBoardersCount) < pCombVeh->Type->Passengers
						&& (pCombVeh->Type->MovementZone == MovementZone::Subterrannean || pInf->Type->Engineer
							|| pInf->Type->C4 || apcBoardBias > 80));

				if ((isGunnerIFV || isOpenToppedFortress || isSubterraneanOrGroundAPC)
					&& pInf->DistanceFrom(pCombVeh) <= apcBoardRange)
				{
					DispatchFootToTargetOrTransport(pInf, pCombVeh, Mission::Enter, false);
					boardedCombatVehicle = true;
					break;
				}
			}

			if (boardedCombatVehicle)
				continue;
		}

		// Slot 62 (Soviet Tesla Trooper Overpowering): Charge owned/allied
		// Overpowerable Tesla Coils via Assaulter or IsElectricBolt weapon so they
		// stay powered during LowPower and gain bonus range/damage!
		const auto pSecWpn = pInf->Type->GetWeapon(1) ? pInf->Type->GetWeapon(1)->WeaponType : nullptr;
		const bool hasInfAAWeapon = (pInfWpn && pInfWpn->Projectile && pInfWpn->Projectile->AA)
			|| (pSecWpn && pSecWpn->Projectile && pSecWpn->Projectile->AA);

		// Multi-Flank Mobile AA Infantry Interception (Flak Troopers, Guardian GIs):
		if (baseAirFlankCount > 0 && hasInfAAWeapon && pInf->GetHealthPercentage() >= 0.35)
		{
			const int bestInfAirIdx = pickAirFlankFor(pInf, isIdle, 4608);

			if (bestInfAirIdx >= 0)
			{
				auto const pBestAir = baseAirFlanks[bestInfAirIdx].Target;
				baseAirFlanks[bestInfAirIdx].AssignedInterceptors++;
				CountDecision(NeuralDecision::AirIntercept);
				DispatchFootToTargetOrTransport(pInf, pBestAir, Mission::Attack, true);
				continue;
			}
		}

		// Opportunistic local strike for idle combat infantry.
		if (isIdle && pInfWpn && !pInf->Type->Engineer && !IsSpyInfantryType(pInf->Type)
			&& pInf->GetHealthPercentage() >= 0.45)
		{
			if (auto const pLocalTarget = findOpportunityTarget(pInf, pInfWpn, pSecWpn))
			{
				pInf->ClearMegaMissionData();
				pInf->SetTarget(pLocalTarget);
				pInf->QueueMission(Mission::Attack, true);
				CountDecision(NeuralDecision::LocalStrike);
				continue;
			}
		}

		// Slot 76 (Squad Focus Fire, pickFocusTarget) for infantry already fighting.
		if (pInf->Target != nullptr && pInfWpn && !pInf->Type->Engineer && !pInf->Type->Infiltrate
			&& (pInf->CurrentMission == Mission::Attack || pInf->CurrentMission == Mission::Guard
				|| pInf->CurrentMission == Mission::Area_Guard || pInf->CurrentMission == Mission::Hunt))
		{
			const int focusRange = std::max(pInfWpn->Range, pSecWpn && pSecWpn->Damage > 0 ? pSecWpn->Range : 0);

			if (auto const pFocusTarget = pickFocusTarget(pInf, focusRange))
			{
				pInf->SetTarget(pFocusTarget);

				continue;
			}
		}

		if (pOverpowerCoil && isIdle
			&& (pInf->Type->Assaulter || (pInfWpn && pInfWpn->IsElectricBolt) || (pSecWpn && pSecWpn->IsElectricBolt))
			&& pInf->DistanceFrom(pOverpowerCoil) <= 3584)
		{
			DispatchFootToTargetOrTransport(pInf, pOverpowerCoil, Mission::Attack, true);

			pOverpowerCoil = nullptr;

			continue;
		}

		// Surgical Urban Garrison Clearing: an Assaulter walks in and kills the occupants, a
		// PenetratesGarrison weapon shoots them through the walls; the building stays for our
		// engineers and occupiers. Enemy garrisons in sight within 15 cells (30 while Expert 9 is
		// in charge).
		if (enemyGarrisonCount > 0 && isIdle && IsGarrisonClearerType(pInf->Type) && pInf->GetHealthPercentage() >= 0.45)
		{
			const int clearRange = (pExt->NeuralAI_ActiveExpert == 9 ? 30 : 15) * Unsorted::LeptonsPerCell;
			BuildingClass* pNearestGarr = nullptr;
			int bestInfGarrDist = clearRange + 1;

			for (int gi = 0; gi < enemyGarrisonCount; ++gi)
			{
				auto const pCandG = enemyGarrisonsToClear[gi];

				if (!pCandG || pCandG->InLimbo || !pCandG->IsAlive || pCandG->Occupants.Count <= 0)
					continue;

				const int d = pInf->DistanceFrom(pCandG);

				if (d <= clearRange && d < bestInfGarrDist)
				{
					bestInfGarrDist = d;
					pNearestGarr = pCandG;
				}
			}

			if (pNearestGarr
				&& DispatchFootToTargetOrTransport(pInf, pNearestGarr, pInf->Type->Assaulter ? Mission::Enter : Mission::Attack, !pInf->Type->Assaulter))
			{
				NoteUrbanClearOrder(pHouse, pInf, pNearestGarr);
				continue;
			}
		}

		// Yuri Bio-Reactor Power Surge: Enter Bio-Reactor with basic infantry
		// during LowPower!
		if (pBioReactor && bioReactorNeedsPower && isIdle && pInf->Destination != pBioReactor && !pInf->Type->Engineer
			&& !IsSpyInfantryType(pInf->Type) && !pInf->Type->Doggie && pInf->Type->Cost <= 250
			&& pInf->DistanceFrom(pBioReactor) <= 4096)
		{
			DispatchFootToTargetOrTransport(pInf, pBioReactor, Mission::Enter, false);
			infAlreadyHeadingToBioReactor++;
			if (pBioReactor->Passengers.NumPassengers + infAlreadyHeadingToBioReactor >= pBioReactor->Type->Passengers)
				pBioReactor = nullptr;

			continue;
		}

		if (false && pBioReactor && isIdle && pInf->Type->Cost <= 250 && pInf->DistanceFrom(pBioReactor) <= 3072)
		{
			pInf->SetDestination(pBioReactor, true);

			pInf->QueueMission(Mission::Enter, true);

			pBioReactor = nullptr;

			continue;
		}

		// Urban Garrisoning (Slot 23): idle occupiers go into the best known building worth
		// holding (RunUrbanGarrison: fog-fair memory, out of enemy fire, room left after the
		// soldiers already walking in, a capped share of our occupiers). Slot 23 sets how often
		// and how far (6..16 cells); under Expert 9 every tick and up to 64 cells.
		const int garrisonBias = static_cast<int>(scores[23]);
		const int garrisonWindow = std::clamp(15 + (garrisonBias / 20), 6, 40);
		const int garrisonRangeCells = std::clamp(10 + garrisonBias / 128, 6, 16);
		const bool urbanExpert = pExt->NeuralAI_ActiveExpert == 9;

		if (isIdle && (urbanExpert || IsAITickWindow(90, garrisonWindow))
			&& RunUrbanGarrison(pHouse, pInf, urbanExpert, garrisonRangeCells, DispatchFootToTargetOrTransport))
		{
			continue;
		}
	}

	// Bridge demolition: an ambush on a crowded deck, a sea lane for our fleet, or a moat against
	// a stronger force (see PickBridgeToDestroy). The learned gate (slot 22) is asked only when
	// there is a bridge to bring down, like the repair gate above.
	if (AreBridgesDestroyable())
	{
		const NeuralBridgeDemolition demolition = PickBridgeToDestroy(pHouse);
		auto const pDeckCell = (demolition.Span >= 0 && SlotGate(scores[22], 600, 22)) ? MapClass::Instance.TryGetCellAt(demolition.Cell) : nullptr;

		if (pDeckCell)
		{
			auto const& span = BridgeSpans[demolition.Span];
			auto const pHut = GetBridgeHut(span);
			bool ordered = false;

			// A warhead that brings the bridge down from its hut drops the whole span at once.
			if (pHut)
			{
				for (auto const pFoot : FootClass::Array)
				{
					if (!pFoot || pFoot->Owner != pHouse || !pFoot->IsAlive || pFoot->InLimbo
						|| IsFootActivelyAttackingOrMovingToEnemy(pFoot)
						|| pFoot->DistanceFrom(pHut) > 40 * Unsorted::LeptonsPerCell || !CanWorkBridgeFromHut(pFoot, pHut))
					{
						continue;
					}

					ordered = DispatchFootToTargetOrTransport(pFoot, pHut, Mission::Attack, true);

					if (ordered)
						break;
				}
			}

			if (!ordered)
			{
				int assigned = 0;

				for (auto const pUnit : UnitClass::Array)
				{
					if (pUnit && pUnit->Owner == pHouse && pUnit->IsAlive && !pUnit->InLimbo && pUnit->Target == pDeckCell)
						assigned++;
				}

				const int wanted = demolition.FleetZone > 1 ? 6 : 4;

				for (auto const pUnit : UnitClass::Array)
				{
					if (assigned >= wanted)
						break;

					if (!pUnit || pUnit->Owner != pHouse || pUnit->InLimbo || !pUnit->IsAlive || !pUnit->Type
						|| pUnit->Target == pDeckCell)
					{
						continue;
					}

					if (pUnit->Type->Harvester || pUnit->Type->ResourceGatherer || pUnit->OnBridge
						|| BridgeSpanAt(pUnit->GetMapCoords()) >= 0 || IsFootActivelyAttackingOrMovingToEnemy(pUnit))
					{
						continue;
					}

					if (BridgeDemolitionDamage(pUnit, span.Low) <= 0)
						continue;

					const bool naval = pUnit->Type->Naval || pUnit->Type->SpeedType == SpeedType::Float;
					const int rangeCells = naval ? 30 : 12;

					if (naval && ObjectZone(pUnit, MovementZone::Water) != demolition.FleetZone)
						continue;

					if (pUnit->DistanceFrom(pDeckCell) > rangeCells * Unsorted::LeptonsPerCell)
						continue;

					pUnit->SetTarget(pDeckCell);
					pUnit->QueueMission(Mission::Attack, true);
					assigned++;
					ordered = true;
				}
			}

			if (ordered)
				NoteBridgeAction(pHouse, demolition.Span, NeuralBridgeAction::Destroy);
		}
	}

	microClock.Mark(NeuralSection::MicroWaves);

	// Autonomous Fog-of-War Scouting & Coordinated Multi-Wave Offensive Dispatch
	// (Replaces legacy aimd.ini TeamScripts)
	BuildingClass* pVisibleEnemyBld = nullptr;
	int bestVisEnemyBldScore = -999999;
	CellClass* pFallbackEnemyCell = nullptr;
	int bestFallbackEnemyScore = -999999;
	bool anyEnemyBuildingExists = false;
	const int coalitionTarget = CoalitionTargetOf(pHouse);

	// First pass: find the closest enemy structure distance on our landmass
	// (outer perimeter reference) and count enemy base defenses so we peel the
	// enemy base from the outside in instead of driving suicidally through half
	// the enemy base to reach a deep interior ConYard/Superweapon!
	int minSameZoneEnemyBldDist = 999999;
	int minAnyEnemyBldDist = 999999;
	int totalEnemyBaseDefenses = 0;
	for (auto const pEnemyBld : BuildingClass::Array)
	{
		if (!pEnemyBld || pEnemyBld->InLimbo || !pEnemyBld->IsAlive || !pEnemyBld->Owner || !pEnemyBld->Type
			|| pEnemyBld->Type->Immune || pEnemyBld->Type->Insignificant || pEnemyBld->Type->InvisibleInGame)
		{
			continue;
		}

		const CellStruct ebc = pEnemyBld->GetMapCoords();
		if (ebc.X <= 1 && ebc.Y <= 1)
			continue;

		auto const pOwner = pEnemyBld->Owner;
		if (pOwner == pHouse || pHouse->IsAlliedWith(pOwner) || !IsEnemyBuildingKnown(pHouse, pEnemyBld) || pOwner->IsNeutral() || pOwner->IsObserver()
			|| pOwner->Defeated)
		{
			continue;
		}

		if (coalitionTarget >= 0 && IsTacitTrucePartner(pHouse, pOwner->ArrayIndex))
			continue;

		if (pEnemyBld->Type->IsBaseDefense)
			totalEnemyBaseDefenses++;

		const int distHome
			= (homeCoords != CellStruct::Empty) ? (std::abs(ebc.X - homeCoords.X) + std::abs(ebc.Y - homeCoords.Y)) : 0;
		if (distHome < minAnyEnemyBldDist)
			minAnyEnemyBldDist = distHome;

		const int ebz = MapClass::Instance.GetMovementZoneType(ebc, MovementZone::Normal, pEnemyBld->OnBridge);
		if (homeGroundZone >= 0 && ebz == homeGroundZone && distHome < minSameZoneEnemyBldDist)
			minSameZoneEnemyBldDist = distHome;
	}

	for (auto const pEnemyBld : BuildingClass::Array)
	{
		if (!pEnemyBld || pEnemyBld->InLimbo || !pEnemyBld->IsAlive || !pEnemyBld->Owner || !pEnemyBld->Type
			|| pEnemyBld->Type->Immune || pEnemyBld->Type->Insignificant || pEnemyBld->Type->InvisibleInGame)
		{
			continue;
		}

		const CellStruct ebc = pEnemyBld->GetMapCoords();
		if (ebc.X <= 1 && ebc.Y <= 1)
			continue;

		auto const pOwner = pEnemyBld->Owner;
		if (pOwner == pHouse || pHouse->IsAlliedWith(pOwner) || pOwner->IsNeutral() || pOwner->IsObserver()
			|| pOwner->Defeated)
		{
			continue;
		}

		// Never a wave at a truce partner (Expert 3 diplomacy).
		if (coalitionTarget >= 0 && IsTacitTrucePartner(pHouse, pOwner->ArrayIndex))
			continue;

		auto const pCell = pEnemyBld->GetCell();
		if (!pCell)
			continue;

		const int distHome
			= (homeCoords != CellStruct::Empty) ? (std::abs(ebc.X - homeCoords.X) + std::abs(ebc.Y - homeCoords.Y)) : 0;
		const int ebz = MapClass::Instance.GetMovementZoneType(ebc, MovementZone::Normal, pEnemyBld->OnBridge);
		const int refMinDist = (homeGroundZone >= 0 && ebz == homeGroundZone && minSameZoneEnemyBldDist < 999999)
			? minSameZoneEnemyBldDist
			: (minAnyEnemyBldDist < 999999 ? minAnyEnemyBldDist : distHome);
		const int depthIntoEnemyBase = std::max(0, distHome - refMinDist);

		// Perimeter-First Scoring: heavily penalize targets that sit deep behind
		// the enemy's outer perimeter (> 6 cells deeper than the closest enemy
		// structure) so our battalions chew through the closest enemy
		// structures/defenses on the outer ring first instead of driving through
		// half the enemy base!
		int bldCandScore = 1000 - (distHome * 8) - (depthIntoEnemyBase > 6 ? (depthIntoEnemyBase - 6) * 18 : 0);
		if (depthIntoEnemyBase <= 6)
			bldCandScore += 220; // Outer-perimeter target facing our approach!
		if (pOwner->ArrayIndex == pHouse->EnemyHouseIndex)
			bldCandScore += 220;

		// Focus the clearly weakest enemy (FFA, or the weakest member of the enemy team).
		if (pOwner->ArrayIndex == GetMacroState(pHouse).FocusEnemyIndex)
			bldCandScore += 300;

		// Expert 3 diplomacy: the coalition target's buildings first, but not where another house's
		// army is already hitting it (spread the fronts); see DiplomacyTargetBias for the rest.
		if (pOwner->ArrayIndex == coalitionTarget)
			bldCandScore += IsCoalitionFrontNear(pHouse, ebc, 10) ? 350 : 700;

		bldCandScore += DiplomacyTargetBias(pHouse, pOwner->ArrayIndex, true);

		if (homeGroundZone >= 0 && ebz == homeGroundZone)
			bldCandScore += 350;

		if (!pEnemyBld->Type->Naval && pCell->LandType != LandType::Water)
			bldCandScore += 180;

		if (pEnemyBld->Type->IsBaseDefense && depthIntoEnemyBase <= 8)
		{
			bldCandScore += 135; // Clear outer base defenses blocking our approach!
		}
		else if (pEnemyBld->Type->ConstructionYard || pEnemyBld->Type->Factory != AbstractType::None
			|| pEnemyBld->Type->SuperWeapon != -1)
		{
			bldCandScore += 110;
		}
		else if (pEnemyBld->Type->Refinery || pEnemyBld->Type->PowerBonus > 0)
		{
			bldCandScore += 90;
		}

		anyEnemyBuildingExists = true;

		// Fallback wave targets only among buildings this house knows about.
		const bool isKnownBuilding = pHouse->SpySatActive || IsCellVisibleToAI(pHouse, pCell)
			|| IsEnemyBuildingKnown(pHouse, pEnemyBld);

		if (isKnownBuilding && bldCandScore > bestFallbackEnemyScore)
		{
			bestFallbackEnemyScore = bldCandScore;
			pFallbackEnemyCell = pCell;
		}

		const int ownerIdx = std::clamp(pOwner->ArrayIndex, 0, 7);
		const bool visible = pHouse->SpySatActive || IsCellVisibleToAI(pHouse, pCell)
			|| IsEnemyBuildingKnown(pHouse, pEnemyBld)
			|| ((pExt->NeuralAI_InfiltratedEnemyMask & (1 << ownerIdx)) != 0);

		if (visible && bldCandScore > bestVisEnemyBldScore)
		{
			bestVisEnemyBldScore = bldCandScore;
			pVisibleEnemyBld = pEnemyBld;
		}
	}

	// Nothing known yet: scout the nearest unchecked start waypoint (public map knowledge).
	if (!pFallbackEnemyCell && anyEnemyBuildingExists && homeCoords != CellStruct::Empty)
	{
		const CellStruct scoutCell = FindNextStartLocationToScout(pHouse, homeCoords);

		if (scoutCell != CellStruct::Empty)
			pFallbackEnemyCell = MapClass::Instance.TryGetCellAt(scoutCell);
	}

	const bool allEnemyBldsGone = !anyEnemyBuildingExists;

	if (!pVisibleEnemyBld && !pFallbackEnemyCell)
	{
		if (pHiddenOrStealthEnemy && pHiddenOrStealthEnemy->GetCell())
		{
			pFallbackEnemyCell = pHiddenOrStealthEnemy->GetCell();
		}
		else if (pEnemySubOrShip && pEnemySubOrShip->GetCell())
		{
			pFallbackEnemyCell = pEnemySubOrShip->GetCell();
		}
		else
		{
			for (auto const pRemT : TechnoClass::Array)
			{
				if (!pRemT || pRemT->InLimbo || !pRemT->IsAlive || !pRemT->Owner
					|| pRemT->WhatAmI() == AbstractType::Building)
				{
					continue;
				}

				auto const pRO = pRemT->Owner;

				if (pRO == pHouse || pHouse->IsAlliedWith(pRO) || pRO->IsNeutral() || pRO->IsObserver()
					|| pRO->Defeated)
				{
					continue;
				}

				if (coalitionTarget >= 0 && IsTacitTrucePartner(pHouse, pRO->ArrayIndex))
					continue;

				if (auto const pRC = pRemT->GetCell())
				{
					pFallbackEnemyCell = pRC;

					if (pRO->ArrayIndex == pHouse->EnemyHouseIndex)
						break;
				}
			}
		}
	}

	const bool endgameRemnantHunt
		= (!pVisibleEnemyBld && pFallbackEnemyCell != nullptr && (allEnemyBldsGone || combatStalledForHunt));

	if (allEnemyBldsGone && pEnemySubOrShip)
		GetMacroState(pHouse).NavalRemnantUntil = Unsorted::CurrentFrame + 900;

	// Remnants in the air (aircraft, jumpjets) once the enemy has no building left: a ground wave
	// sent to their cell cannot hit them, so the 12 nearest units with an anti-air weapon attack
	// each one directly. Every 150 frames.
	if (endgameRemnantHunt && allEnemyBldsGone && IsAITickWindow(150, 15))
	{
		auto const hitsAir = [](TechnoTypeClass* pType)
		{
			for (int i = 0; i < 2; ++i)
			{
				auto const pWS = pType->GetWeapon(i);

				if (pWS && pWS->WeaponType && pWS->WeaponType->Damage > 0 && pWS->WeaponType->Projectile
					&& pWS->WeaponType->Projectile->AA)
				{
					return true;
				}
			}

			return false;
		};

		for (auto const pRemnant : TechnoClass::Array)
		{
			if (!pRemnant || pRemnant->InLimbo || !pRemnant->IsAlive || !pRemnant->IsInAir() || !pRemnant->Owner
				|| pRemnant->WhatAmI() == AbstractType::Building)
			{
				continue;
			}

			auto const pRO = pRemnant->Owner;

			if (pRO == pHouse || pHouse->IsAlliedWith(pRO) || pRO->IsNeutral() || pRO->IsObserver() || pRO->Defeated
				|| (coalitionTarget >= 0 && IsTacitTrucePartner(pHouse, pRO->ArrayIndex)))
			{
				continue;
			}

			std::vector<std::pair<int, FootClass*>> hunters;

			for (auto const pTechno : TechnoClass::Array)
			{
				auto const pFoot = abstract_cast<FootClass*>(pTechno);

				if (!pFoot || pFoot->Owner != pHouse || pFoot->InLimbo || !pFoot->IsAlive || pFoot->Target == pRemnant)
					continue;

				auto const pType = pFoot->GetTechnoType();

				if (!pType || pType->Spawned || !hitsAir(pType))
					continue;

				hunters.emplace_back(pFoot->DistanceFrom(pRemnant), pFoot);
			}

			std::sort(hunters.begin(), hunters.end(), [](auto const& a, auto const& b)
				{
					return a.first != b.first ? a.first < b.first : a.second->UniqueID < b.second->UniqueID;
				});

			for (size_t i = 0; i < hunters.size() && i < 12; ++i)
			{
				hunters[i].second->SetTarget(pRemnant);
				hunters[i].second->QueueMission(Mission::Attack, false);
			}
		}
	}

	// Count idle combat units and infantry ready at base
	std::vector<UnitClass*> idleCombatUnits;

	std::vector<InfantryClass*> idleCombatInf;

	for (auto const pUnit : UnitClass::Array)
	{
		if (!pUnit || pUnit->Owner != pHouse || pUnit->InLimbo || !pUnit->IsAlive || !pUnit->Type)
			continue;

		if (pUnit->Type->Harvester || pUnit->Type->ResourceGatherer
			|| (pUnit->Type->DeploysInto && pUnit->Type->DeploysInto->ConstructionYard))
		{
			continue;
		}

		// Capital units go out in their own groups (UpdateCapitalGroups).
		if (GetMacroState(pHouse).CapitalHeld.contains(pUnit->UniqueID))
			continue;

		if (pUnit->GetHealthPercentage() < 0.40)
			continue;

		if (!endgameRemnantHunt && (pUnit->Type->CanDisguise || pUnit->Type->PermaDisguise) && SlotGate(scores[83], 60, 83)
			&& homeCoords != CellStruct::Empty && pUnit->GetMapCoords().DistanceFrom(homeCoords) > 11.0)
		{
			continue;
		}

		const bool canCrossWaterDirectly = (pUnit->Type->MovementZone == MovementZone::Amphibious
			|| pUnit->Type->MovementZone == MovementZone::AmphibiousDestroyer
			|| pUnit->Type->MovementZone == MovementZone::AmphibiousCrusher
			|| pUnit->Type->MovementZone == MovementZone::Fly
			|| pUnit->Type->MovementZone == MovementZone::Subterrannean || pUnit->Type->JumpJet
			|| pUnit->Type->Teleporter);
		ClearStaleEnterOrUnreachableCrossIslandMission(pUnit, canCrossWaterDirectly);

		if (IsImmobileUnitTypeOnLand(pUnit->Type))
		{
			continue; // Static deployed vehicle bunker (NoManualMove=yes / Speed=0):
					// stay in Area_Guard and let OpenTopped passengers fire!
		}

		if ((pUnit->Type->Gunner || pUnit->Type->OpenTopped) && !HasNoManualEnter(pUnit->Type)
			&& !HasNoManualUnload(pUnit->Type) && pUnit->Passengers.NumPassengers < pUnit->Type->Passengers
			&& !pUnit->HasBeenAttacked)
		{
			bool passengerWalkingToBoard = false;
			for (auto const pCheckI : InfantryClass::Array)
			{
				if (pCheckI && pCheckI->Owner == pHouse && !pCheckI->InLimbo && pCheckI->IsAlive
					&& pCheckI->CurrentMission == Mission::Enter && pCheckI->Destination == pUnit
					&& pCheckI->DistanceFrom(pUnit) <= 3840)
				{
					passengerWalkingToBoard = true;
					break;
				}
			}

			if (passengerWalkingToBoard)
			{
				continue; // Wait a few seconds for the nearby gunner/passenger to climb
						// inside before driving away!
			}
		}

		if (pUnit->CurrentMission == Mission::Enter || pUnit->QueuedMission == Mission::Enter
			|| pUnit->BunkerLinkedItem != nullptr || pUnit->HasAnyLink())
		{
			continue;
		}

		if (IsFootActivelyAttackingOrMovingToEnemy(pUnit))
			continue;

		if (pUnit->CurrentMission == Mission::Guard || pUnit->CurrentMission == Mission::Area_Guard
			|| pUnit->CurrentMission == Mission::AttackMove
			|| (pUnit->CurrentMission == Mission::Move && pUnit->MegaMission == Mission::AttackMove))
		{
			if (!IsPlanMember(pHouse, pUnit->UniqueID))
				idleCombatUnits.push_back(pUnit);
		}
	}

	for (auto const pInf : InfantryClass::Array)
	{
		if (!pInf || pInf->Owner != pHouse || pInf->InLimbo || !pInf->IsAlive || !pInf->Type)
			continue;

		if (pInf->Type->Engineer || IsSpyInfantryType(pInf->Type))
			continue;

		const bool canInfCrossWater = (pInf->Type->MovementZone == MovementZone::Amphibious
			|| pInf->Type->MovementZone == MovementZone::AmphibiousDestroyer
			|| pInf->Type->MovementZone == MovementZone::Fly || pInf->Type->JumpJet || pInf->Type->Teleporter);
		ClearStaleEnterOrUnreachableCrossIslandMission(pInf, canInfCrossWater);

		if (pInf->CurrentMission == Mission::Enter || pInf->QueuedMission == Mission::Enter || pInf->HasAnyLink())
		{
			continue;
		}

		if (IsFootActivelyAttackingOrMovingToEnemy(pInf))
			continue;

		if (pInf->CurrentMission == Mission::Guard || pInf->CurrentMission == Mission::Area_Guard
			|| pInf->CurrentMission == Mission::AttackMove
			|| (pInf->CurrentMission == Mission::Move && pInf->MegaMission == Mission::AttackMove))
		{
			if (!IsPlanMember(pHouse, pInf->UniqueID))
				idleCombatInf.push_back(pInf);
		}
	}

	// Determine the primary island zone where our idle ground army is
	// concentrated (could be homeGroundZone or an expansion island!)
	int primaryArmyZone = homeGroundZone;
	{
		struct ZoneArmyCount
		{
			int zone;
			int weight;
		};
		std::vector<ZoneArmyCount> zoneCounts;
		auto addZoneWeight = [&zoneCounts](int z, int w)
		{
			if (z < 0)
				return;

			for (auto& zc : zoneCounts)
			{
				if (zc.zone == z)
				{
					zc.weight += w;
					return;
				}
			}

			zoneCounts.push_back({ z, w });
		};
		for (auto const pU : idleCombatUnits)
		{
			if (!IsAmphibiousOrAirTransport(pU->Type))
			{
				addZoneWeight(
					ObjectZone(pU, MovementZone::Normal), 3);
			}
		}

		for (auto const pI : idleCombatInf)
		{
			addZoneWeight(ObjectZone(pI, MovementZone::Normal), 1);
		}

		int bestW = 0;
		for (const auto& zc : zoneCounts)
		{
			if (zc.weight > bestW)
			{
				bestW = zc.weight;
				primaryArmyZone = zc.zone;
			}
		}
	}

	const int arch = pExt->NeuralAI_Archetype;
	const int rushWaveBias = NeuralAI::PolicyFor(pHouse).OutputBias[37];
	const int reserveBias = -rushWaveBias;
	// Critical-Mass Team Scaling (Lanchester's Law):
	// In RA2/YR, larger battalions are exponentially more devastating against
	// fortified AI/human bases. Scale minimum battalion size up when the enemy
	// has base defenses or when our industrial capacity is mature!
	const int baseWave = (arch == 3 || arch == 5) ? 4 : ((arch == 2 || arch == 4 || arch == 7) ? 8 : 6);
	const int defenseMassBonus = std::clamp(totalEnemyBaseDefenses / 2, 0, 4) + ((pHouse->OwnedUnits >= 18) ? 2 : 0);
	const bool highMoneyMultiFactoryStream = (pHouse->Available_Money() >= 4000
		&& (pHouse->NumWarFactories >= 2 || pHouse->NumBarracks >= 2 || pHouse->NumAirpads >= 2));
	const int streamWaveDiscount = highMoneyMultiFactoryStream ? 2 : 0;
	auto& waveMacro = GetMacroState(pHouse);
	// Counterattack right after their wave broke (point 45), steady pressure while our economy
	// leads (point 53), and a second echelon 300..1500 frames after a wave started so it arrives
	// while the first one is still fighting (point 33).
	const bool counterattackWindow = waveMacro.CounterattackUntilFrame > Unsorted::CurrentFrame;
	const int sinceWaveStart = Unsorted::CurrentFrame - waveMacro.WaveStartFrame;
	const bool secondEchelonWindow = sinceWaveStart >= 300 && sinceWaveStart <= 1500;
	const int tacticalWaveDiscount = (counterattackWindow ? 3 : 0) + (waveMacro.PressureMode ? 2 : 0)
		+ (secondEchelonWindow ? (baseWave + defenseMassBonus) / 2 : 0);
	const size_t minWaveUnits = endgameRemnantHunt
		? 1u
		: static_cast<size_t>(std::clamp(
			baseWave + defenseMassBonus - streamWaveDiscount - (rushWaveBias / 160)
				- (waveMacro.BlackoutUntilFrame > Unsorted::CurrentFrame ? 3 : 0) - tacticalWaveDiscount
				+ StyleJitter(pHouse->ArrayIndex, 1000, 2700, 3) - 1, // +-1 unit: waves are not clockwork
			(highMoneyMultiFactoryStream || tacticalWaveDiscount > 0) ? 2 : 3, 14));
	const int blindPushFrame = (rushWaveBias > 100 || highMoneyMultiFactoryStream) ? 1350 : 3200;

	// Multi-Purpose Map Unit Recall ("utilizarlas para diferentes propósitos por
	// el mapa y a la hora de la verdad reúne las que necesite en un lugar y
	// mandarlas contra objetivos enemigos"): While accumulating forces, units can
	// ambush in tree groves, patrol outposts, hunt crates, or guard harvesters.
	// When recalling those non-engaged map units brings our strike force to
	// critical mass (>= minWaveUnits), recruit them into idleCombatUnits so the
	// entire army rallies for the decisive offensive push!
	if (!endgameRemnantHunt && idleCombatUnits.size() < minWaveUnits)
	{
		std::vector<UnitClass*> recallableMapUnits;
		for (auto const pUnit : UnitClass::Array)
		{
			if (!pUnit || pUnit->Owner != pHouse || pUnit->InLimbo || !pUnit->IsAlive || !pUnit->Type)
				continue;

			if (pUnit->Type->Naval || pUnit->Type->Harvester || pUnit->Type->ResourceGatherer
				|| IsAmphibiousOrAirTransport(pUnit->Type))
			{
				continue;
			}

			if ((pUnit->Type->DeploysInto && pUnit->Type->DeploysInto->ConstructionYard)
				|| IsImmobileUnitTypeOnLand(pUnit->Type))
			{
				continue;
			}

			if (pUnit->GetHealthPercentage() < 0.65 || pUnit->HasBeenAttacked || pUnit->Target != nullptr)
				continue;

			if (pUnit->CurrentMission == Mission::Enter || pUnit->QueuedMission == Mission::Enter
				|| (pUnit->CaptureManager && pUnit->CaptureManager->NumControlNodes() > 0))
			{
				continue;
			}

			const int recZone
				= ObjectZone(pUnit, MovementZone::Normal);
			if (IsFootActivelyAttackingOrMovingToEnemy(pUnit) || IsFootNearEnemyFrontline(pUnit, recZone))
			{
				continue; // Never abort a unit that is already attacking an enemy
						// objective or at an active frontline!
			}

			const auto pW0 = pUnit->Type->GetWeapon(0) ? pUnit->Type->GetWeapon(0)->WeaponType : nullptr;
			if (!pW0 || pW0->Damage <= 0)
				continue;

			if (std::find(idleCombatUnits.begin(), idleCombatUnits.end(), pUnit) != idleCombatUnits.end())
				continue;

			if (pUnit->CurrentMission == Mission::Guard || pUnit->CurrentMission == Mission::Area_Guard
				|| pUnit->CurrentMission == Mission::Move)
			{
				recallableMapUnits.push_back(pUnit);
			}
		}

		if (idleCombatUnits.size() + recallableMapUnits.size() >= minWaveUnits)
		{
			for (auto const pRecU : recallableMapUnits)
			{
				if (!IsPlanMember(pHouse, pRecU->UniqueID))
					idleCombatUnits.push_back(pRecU);
			}
		}
	}

	// Helper: returns true ONLY if a sister transport is actively
	// beaching/unloading right next to pFoot (within 10 cells) and is alive and
	// NOT intercepted/attacked. If a transport was destroyed or intercepted at
	// sea, immediately ignore it!
	auto isWaitingForSisterTransportUnload = [pHouse](FootClass* pFoot, bool canCrossWaterDirectly) -> bool
	{
		if (!pFoot || canCrossWaterDirectly || pFoot->HasBeenAttacked || (Unsorted::CurrentFrame % 180 >= 110))
			return false;

		const CellStruct fc = pFoot->GetMapCoords();
		for (auto const pCheckT : UnitClass::Array)
		{
			if (!pCheckT || pCheckT->Owner != pHouse || pCheckT->InLimbo || !pCheckT->IsAlive || !pCheckT->Type
				|| !IsAmphibiousOrAirTransport(pCheckT->Type))
			{
				continue;
			}

			if (pCheckT->Passengers.NumPassengers == 0)
				continue;

			// If the sister transport was intercepted/attacked or is critically
			// damaged, ignore it so landed units march immediately!
			if (pCheckT->HasBeenAttacked || pCheckT->GetHealthPercentage() < 0.65)
				continue;

			if (fc.DistanceFrom(pCheckT->GetMapCoords()) <= 10.0)
				return true;
		}

		return false;
	};

	CellClass* pWaveTargetCell = pVisibleEnemyBld
		? pVisibleEnemyBld->GetCell()
		: ((endgameRemnantHunt || Unsorted::CurrentFrame > blindPushFrame) ? pFallbackEnemyCell : nullptr);

	// Expert 9 & Arch 7 (Allied Coalition Rescue & Joint Attack Wave):
	// If an allied house's critical building (ConYard, Factory, Superweapon) is
	// under enemy attack, coordinate our wave to defend our ally!
	if (pExt->NeuralAI_ActiveExpert == 9 || pExt->NeuralAI_Archetype == 7 || SlotGate(scores[85], 120, 85))
	{
		for (auto const pAllyB : BuildingClass::Array)
		{
			if (!pAllyB || pAllyB->InLimbo || !pAllyB->IsAlive || !pAllyB->Owner || pAllyB->Owner == pHouse
				|| !pHouse->IsAlliedWith(pAllyB->Owner) || pAllyB->Owner->IsNeutral() || !pAllyB->Type)
			{
				continue;
			}

			if ((pAllyB->Type->ConstructionYard || pAllyB->Type->Factory != AbstractType::None
					|| pAllyB->Type->SuperWeapon != -1)
				&& pAllyB->HasBeenAttacked && pAllyB->GetHealthPercentage() < 0.85)
			{
				if (auto const pRescueCell = pAllyB->GetCell())
				{
					pWaveTargetCell = pRescueCell;

					break;
				}
			}
		}
	}

	// Team blackboard (NeuralAI.Team.inl): allied Neural AI houses push one target at one time (the
	// far armies leave first), split fronts when each is strong enough, and the house with the idle
	// army answers a teammate's call for help. Alone, the order is empty.
	const NeuralTeamWaveOrder teamOrder = UpdateTeamWave(pHouse, idleCombatUnits, idleCombatInf, static_cast<int>(minWaveUnits),
		pVisibleEnemyBld, homeCoords, endgameRemnantHunt);

	if (teamOrder.TargetCell != CellStruct::Empty)
	{
		if (auto const pTeamCell = MapClass::Instance.TryGetCellAt(teamOrder.TargetCell))
		{
			pWaveTargetCell = pTeamCell;

			// A team target building is what the wave (and the superweapons) attack; a call for help
			// is a spot to attack-move to.
			if (teamOrder.pTarget)
				pVisibleEnemyBld = teamOrder.pTarget;
		}
	}

	const size_t waveUnitsNeeded = teamOrder.MinWaveUnits > 0
		? std::min(minWaveUnits, static_cast<size_t>(teamOrder.MinWaveUnits))
		: minWaveUnits;

	// Expert 3 tacit coalition: this wave goes for the coalition target (approach away from the
	// spots other houses' armies are hitting).
	const bool waveAtCoalitionTarget = coalitionTarget >= 0 && pVisibleEnemyBld && pVisibleEnemyBld->Owner
		&& pVisibleEnemyBld->Owner->ArrayIndex == coalitionTarget && pWaveTargetCell == pVisibleEnemyBld->GetCell();

	CellClass* pIslandGoalCell = pWaveTargetCell ? pWaveTargetCell : pFallbackEnemyCell;
	int targetIslandZone = -1;
	if (pIslandGoalCell)
	{
		targetIslandZone
			= MapClass::Instance.GetMovementZoneType(pIslandGoalCell->MapCoords, MovementZone::Normal, false);
	}

	CellClass* pSecondaryWaveTargetCell = nullptr;
	CellClass* pSecondaryIslandGoalCell = nullptr;
	int secondaryIslandZone = -1;

	// Smart Enemy Island Reconquest & Multi-Squadron Parallel Target Selection:
	// Pick BOTH a primary enemy island objective (pIslandGoalCell) AND a
	// secondary enemy island/flank objective (pSecondaryIslandGoalCell /
	// pSecondaryWaveTargetCell) so ground armies, transports, and naval squadrons
	// operate in parallel teams across multiple fronts!
	{
		CellClass* pBestEnemyIslandCell = nullptr;
		int bestEnemyIslandZone = -1;
		int bestEnemyIslandScore = -999999;
		CellClass* pSecondEnemyIslandCell = nullptr;
		int secondEnemyIslandZone = -1;
		int secondEnemyIslandScore = -999999;

		for (auto const pEB : BuildingClass::Array)
		{
			if (!pEB || pEB->InLimbo || !pEB->IsAlive || !pEB->Owner || !pEB->Type || pEB->Type->Immune
				|| pEB->Type->Insignificant || pEB->Type->InvisibleInGame || pEB->Type->Naval)
			{
				continue;
			}

			auto const pEO = pEB->Owner;
			if (pEO == pHouse || pHouse->IsAlliedWith(pEO) || pEO->IsNeutral() || pEO->IsObserver() || pEO->Defeated)
				continue;

			auto const pECell = pEB->GetCell();
			if (!pECell || pECell->LandType == LandType::Water)
				continue;

			const int ebz = MapClass::Instance.GetMovementZoneType(pECell->MapCoords, MovementZone::Normal, false);
			if (ebz < 0)
				continue;

			if (!pSecondaryWaveTargetCell && pWaveTargetCell
				&& pECell->MapCoords.DistanceFrom(pWaveTargetCell->MapCoords) >= 10.0)
			{
				pSecondaryWaveTargetCell = pECell;
			}

			if (primaryArmyZone >= 0 && ebz == primaryArmyZone)
				continue;

			int enemyTurretsInZone = 0;
			int enemyBldsInZone = 0;
			for (auto const pCheckEB : BuildingClass::Array)
			{
				if (!pCheckEB || pCheckEB->InLimbo || !pCheckEB->IsAlive || !pCheckEB->Owner || !pCheckEB->Type
					|| pCheckEB->Type->Insignificant)
				{
					continue;
				}

				if (pCheckEB->Owner == pHouse || pHouse->IsAlliedWith(pCheckEB->Owner) || !IsEnemyBuildingKnown(pHouse, pCheckEB) || pCheckEB->Owner->IsNeutral())
					continue;

				if (ObjectZone(pCheckEB, MovementZone::Normal)
					== ebz)
				{
					enemyBldsInZone++;
					if (pCheckEB->Type->IsBaseDefense)
						enemyTurretsInZone++;
				}
			}

			int ourUnitsInZone = 0;
			for (auto const pU : idleCombatUnits)
			{
				if (ObjectZone(pU, MovementZone::Normal) == ebz)
					ourUnitsInZone++;
			}

			int score = 2000 - (enemyTurretsInZone * 180) - (enemyBldsInZone * 25);
			if (ourUnitsInZone >= 1 && ourUnitsInZone <= 12)
				score += 350; // Reinforce active landing beachhead!
			if (homeGroundZone >= 0 && ebz != homeGroundZone)
				score += 200;

			if (pEO->ArrayIndex == pHouse->EnemyHouseIndex)
				score += 150;

			if (score > bestEnemyIslandScore)
			{
				if (pBestEnemyIslandCell
					&& (bestEnemyIslandZone != ebz
						|| pECell->MapCoords.DistanceFrom(pBestEnemyIslandCell->MapCoords) >= 14.0))
				{
					secondEnemyIslandScore = bestEnemyIslandScore;
					pSecondEnemyIslandCell = pBestEnemyIslandCell;
					secondEnemyIslandZone = bestEnemyIslandZone;
				}

				bestEnemyIslandScore = score;
				pBestEnemyIslandCell = pECell;
				bestEnemyIslandZone = ebz;
			}
			else if (score > secondEnemyIslandScore && pBestEnemyIslandCell
				&& (bestEnemyIslandZone != ebz
					|| pECell->MapCoords.DistanceFrom(pBestEnemyIslandCell->MapCoords) >= 14.0))
			{
				secondEnemyIslandScore = score;
				pSecondEnemyIslandCell = pECell;
				secondEnemyIslandZone = ebz;
			}
		}

		if (pBestEnemyIslandCell && bestEnemyIslandZone >= 0)
		{
			pIslandGoalCell = pBestEnemyIslandCell;
			targetIslandZone = bestEnemyIslandZone;
		}

		if (pSecondEnemyIslandCell && secondEnemyIslandZone >= 0)
		{
			pSecondaryIslandGoalCell = pSecondEnemyIslandCell;
			secondaryIslandZone = secondEnemyIslandZone;
		}
	}

	bool hasIdleGroundArmyOnOtherIsland = false;
	if (targetIslandZone >= 0)
	{
		for (auto const pU : idleCombatUnits)
		{
			if (IsAmphibiousOrAirTransport(pU->Type))
				continue;

			const int uz = ObjectZone(pU, MovementZone::Normal);
			if (uz >= 0 && uz != targetIslandZone)
			{
				hasIdleGroundArmyOnOtherIsland = true;
				break;
			}
		}

		if (!hasIdleGroundArmyOnOtherIsland)
		{
			for (auto const pI : idleCombatInf)
			{
				const int iz = ObjectZone(pI, MovementZone::Normal);
				if (iz >= 0 && iz != targetIslandZone)
				{
					hasIdleGroundArmyOnOtherIsland = true;
					break;
				}
			}
		}
	}

	const bool enemyOnDisconnectedIsland = (targetIslandZone >= 0
		&& ((homeGroundZone >= 0 && targetIslandZone != homeGroundZone)
			|| (primaryArmyZone >= 0 && targetIslandZone != primaryArmyZone) || hasIdleGroundArmyOnOtherIsland));

	// If no enemy building is visible yet, send a fast scout unit/infantry only
	// if on the same landmass
	const int scoutPeriod = std::clamp(225 - (static_cast<int>(scores[37]) / 4), 105, 360);
	if (!pVisibleEnemyBld && pFallbackEnemyCell && !enemyOnDisconnectedIsland
		&& (IsAITickWindow(scoutPeriod, 30)))
	{
		const int fbZone
			= MapClass::Instance.GetMovementZoneType(pFallbackEnemyCell->MapCoords, MovementZone::Normal, false);
		if (!idleCombatUnits.empty())
		{
			auto const pScout = idleCombatUnits.front();
			const int sz = ObjectZone(pScout, MovementZone::Normal);
			if (fbZone >= 0 && sz == fbZone && pFallbackEnemyCell->LandType != LandType::Water)
			{
				OrderFootToAttackMove(pScout, pFallbackEnemyCell);
			}
		}
		else if (!idleCombatInf.empty())
		{
			auto const pScoutInf = idleCombatInf.front();
			const int sz
				= ObjectZone(pScoutInf, MovementZone::Normal);
			if (fbZone >= 0 && sz == fbZone && pFallbackEnemyCell->LandType != LandType::Water)
			{
				OrderFootToAttackMove(pScoutInf, pFallbackEnemyCell);
			}
		}
	}

	// Strength gate on top of the unit count: the main wave (and the same-landmass push below)
	// waits until our idle army clearly outweighs what this house knows of the enemy at the
	// target, or a window opens (see EvaluateWaveLaunch). The endgame remnant hunt never waits.
	// The unit count a team wave asks for (waveUnitsNeeded) may be below our own minimum.
	const bool waveCountReady = idleCombatUnits.size() >= waveUnitsNeeded
		|| idleCombatUnits.size() + idleCombatInf.size() >= waveUnitsNeeded + 4 || idleCombatInf.size() >= waveUnitsNeeded + 3;
	NeuralWaveCheck waveCheck {};
	waveCheck.Launch = true;

	if (pWaveTargetCell && !endgameRemnantHunt && waveCountReady)
	{
		const CellStruct gateTarget = pWaveTargetCell->MapCoords;
		const int gateZone = MapClass::Instance.GetMovementZoneType(gateTarget, MovementZone::Normal, false);
		int readyValue = 0;

		auto addReadyValue = [&](FootClass* pFoot, bool canCrossWater)
		{
			const int footZone = ObjectZone(pFoot, MovementZone::Normal);

			if ((canCrossWater || (gateZone >= 0 && footZone == gateZone)) && pFoot->GetMapCoords().DistanceFrom(gateTarget) > 14.0)
				readyValue += OwnGroundFighterValue(pFoot);
		};

		for (auto const pU : idleCombatUnits)
		{
			const auto mz = pU->Type->MovementZone;
			addReadyValue(pU, mz == MovementZone::Amphibious || mz == MovementZone::AmphibiousDestroyer
				|| mz == MovementZone::AmphibiousCrusher || mz == MovementZone::Fly || mz == MovementZone::Subterrannean
				|| pU->Type->JumpJet || pU->Type->Teleporter);
		}

		for (auto const pI : idleCombatInf)
		{
			const auto mz = pI->Type->MovementZone;
			addReadyValue(pI, mz == MovementZone::Amphibious || mz == MovementZone::AmphibiousDestroyer || mz == MovementZone::Fly
				|| pI->Type->JumpJet || pI->Type->Teleporter);
		}

		waveCheck = EvaluateWaveLaunch(pHouse, gateTarget, readyValue,
			counterattackWindow || waveMacro.BlackoutUntilFrame > Unsorted::CurrentFrame);

		NoteWaveDecision(pHouse, waveCheck, false, 0);
	}

	const bool waveStrengthReady = waveCheck.Launch;

	// Coordinated Multi-Squadron Offensive Strike Wave (Slot 37: Rush vs Mass
	// Timing) Splits forces into parallel Squadron Alpha (pWaveTargetCell) and
	// Squadron Bravo (pSecondaryWaveTargetCell)
	if (pWaveTargetCell && !teamOrder.Hold
		&& (endgameRemnantHunt || ((idleCombatUnits.size() >= waveUnitsNeeded
			|| (idleCombatUnits.size() + idleCombatInf.size() >= waveUnitsNeeded + 4)) && waveStrengthReady)))
	{
		const int waveTargetZone
			= MapClass::Instance.GetMovementZoneType(pWaveTargetCell->MapCoords, MovementZone::Normal, false);
		const bool waveTargetInWater = (pWaveTargetCell->LandType == LandType::Water);
		const int secWaveZone = pSecondaryWaveTargetCell
			? MapClass::Instance.GetMovementZoneType(pSecondaryWaveTargetCell->MapCoords, MovementZone::Normal, false)
			: -1;
		// Strategic reserve (point 44): one or two more units stay home when we know the side we
		// are attacked from or the enemy has been pressing early.
		const int reserveThreatBonus = (GetMainAttackSector(pHouse) >= 0 ? 1 : 0)
			+ (waveMacro.OpponentProfile.RushPressure >= 150 ? 1 : 0);
		const size_t desiredUnitReserve = endgameRemnantHunt
			? 0u
			: static_cast<size_t>(std::clamp(1 + (reserveBias / 180) + reserveThreatBonus, 0, 4));
		const size_t reserveCount = idleCombatUnits.size() > (desiredUnitReserve + 1) ? desiredUnitReserve : 0;

		// Forward Staging / Rally Assembly ("reúne las que necesite en un lugar y
		// mandarlas contra objetivos enemigos"): If our battalion units on the
		// target landmass are scattered across the map (e.g., > 35% of them are >
		// 15 cells away from the forward staging point outside the enemy
		// perimeter), rally them at a safe Staging Cell (~15 cells from
		// pWaveTargetCell toward our army centroid) first so they hit the enemy
		// perimeter together as one devastating wall of armor instead of trickling!
		CellClass* pForwardStagingCell = nullptr;
		bool holdAtForwardStaging = false;
		if (!endgameRemnantHunt && idleCombatUnits.size() < 20 && waveTargetZone >= 0 && !waveTargetInWater)
		{
			int sameZoneCount = 0;
			int sumX = 0;
			int sumY = 0;
			for (size_t i = reserveCount; i < idleCombatUnits.size(); ++i)
			{
				auto const pCheckU = idleCombatUnits[i];
				if (!pCheckU || pCheckU->Type->Naval || IsAmphibiousOrAirTransport(pCheckU->Type))
					continue;

				const CellStruct cuc = pCheckU->GetMapCoords();
				if (MapClass::Instance.GetMovementZoneType(cuc, MovementZone::Normal, pCheckU->OnBridge) == waveTargetZone)
				{
					if (IsFootActivelyAttackingOrMovingToEnemy(pCheckU)
						|| IsFootNearEnemyFrontline(pCheckU, waveTargetZone))
					{
						continue; // Do not include units already engaged at a frontline in
								// staging centroid/count!
					}

					sameZoneCount++;
					sumX += cuc.X;
					sumY += cuc.Y;
				}
			}

			if (sameZoneCount >= 4)
			{
				const CellStruct armyCenter { static_cast<short>(sumX / sameZoneCount),
					static_cast<short>(sumY / sameZoneCount) };
				const CellStruct tc = pWaveTargetCell->MapCoords;
				const int distToTarget = std::max(1, static_cast<int>(tc.DistanceFrom(armyCenter)));
				int dx = armyCenter.X - tc.X;
				int dy = armyCenter.Y - tc.Y;

				// Approach from the weakest side (point 31) and not again from the side where our
				// last waves died (point 42): the direct line and +-45/90 degrees around the
				// target are probed 12 cells out for known defenses and recorded losses.
				{
					const double baseLen = std::max(1.0, std::sqrt(static_cast<double>(dx * dx + dy * dy)));
					const double ux = dx / baseLen;
					const double uy = dy / baseLen;
					constexpr double RotCos[] = { 1.0, 0.70710678, 0.70710678, 0.0, 0.0 };
					constexpr double RotSin[] = { 0.0, 0.70710678, -0.70710678, 1.0, -1.0 };
					constexpr int RotPenalty[] = { 0, 40, 40, 120, 120 };
					int bestApproachCost = INT_MAX;
					double bestUX = ux;
					double bestUY = uy;

					for (int k = 0; k < 5; ++k)
					{
						const double rx = ux * RotCos[k] - uy * RotSin[k];
						const double ry = ux * RotSin[k] + uy * RotCos[k];
						const CellStruct probe { static_cast<short>(tc.X + rx * 12.0), static_cast<short>(tc.Y + ry * 12.0) };

						if (!MapClass::Instance.CoordinatesLegal(probe)
							|| MapClass::Instance.GetMovementZoneType(probe, MovementZone::Normal, false) != waveTargetZone)
						{
							continue;
						}

						const int sector = AttackSectorOf(tc, probe);
						const int cost = CountKnownEnemyDefensesNear(pHouse, probe, 10.0, false) * 100
							+ (sector >= 0 ? waveMacro.FailedApproachWeight[sector] / 4 : 0) + RotPenalty[k]
							+ (waveAtCoalitionTarget && IsCoalitionFrontNear(pHouse, probe, 8) ? 150 : 0);

						if (cost < bestApproachCost)
						{
							bestApproachCost = cost;
							bestUX = rx;
							bestUY = ry;
						}
					}

					dx = static_cast<int>(bestUX * distToTarget);
					dy = static_cast<int>(bestUY * distToTarget);
				}

				if (distToTarget >= 21)
				{
					// Prefer staging 20 to 30 cells outside the enemy perimeter (outside
					// long-range defenses), adapting dynamically if map/island geometry
					// is tighter.
					const int candidateStageDists[] = { 25, 23, 27, 21, 29, 19, 16 };
					for (int pass = 0; pass < 2 && !pForwardStagingCell; ++pass)
					{
						for (int desiredDist : candidateStageDists)
						{
							if (pForwardStagingCell)
								break;

							if (pass == 0 && desiredDist < 20)
								continue;

							const int stageDist = std::min(desiredDist, distToTarget - 5);
							if (stageDist < (pass == 0 ? 20 : 15))
								continue;

							const CellStruct rawStage { static_cast<short>(tc.X + (dx * stageDist) / distToTarget),
								static_cast<short>(tc.Y + (dy * stageDist) / distToTarget) };
							for (int r = 0; r <= 6 && !pForwardStagingCell; r += 2)
							{
								for (int sy = -r; sy <= r && !pForwardStagingCell; sy += 2)
								{
									for (int sx = -r; sx <= r; sx += 2)
									{
										const CellStruct sc { static_cast<short>(rawStage.X + sx),
											static_cast<short>(rawStage.Y + sy) };
										if (auto const pSC = MapClass::Instance.TryGetCellAt(sc))
										{
											if (pSC->LandType != LandType::Water
												&& MapClass::Instance.GetMovementZoneType(
													sc, MovementZone::Normal, false)
													== waveTargetZone
												&& pSC->IsClearToMove(
													SpeedType::Track, true, true, -1, MovementZone::Normal, -1, false))
											{
												bool safeFromLongRangeDef = true;
												const double minSafeDefDist = (pass == 0) ? 20.0 : 15.0;
												if (sc.DistanceFrom(tc) < minSafeDefDist)
													safeFromLongRangeDef = false;

												for (int ebIdx = 0;
													safeFromLongRangeDef && ebIdx < BuildingClass::Array.Count; ++ebIdx)
												{
													auto const pEB = BuildingClass::Array.GetItem(ebIdx);
													if (!pEB || !pEB->IsAlive || pEB->InLimbo || !pEB->Owner
														|| pHouse->IsAlliedWith(pEB->Owner))
													{
														continue;
													}

													if (!pEB->Type
														|| (!pEB->Type->IsBaseDefense
															&& !pEB->GetWeapon(0)->WeaponType))
													{
														continue;
													}

													const CellStruct ebc = pEB->GetMapCoords();
													if (MapClass::Instance.GetMovementZoneType(ebc, MovementZone::Normal, pEB->OnBridge)
														!= waveTargetZone)
													{
														continue;
													}

													double reqClearance = minSafeDefDist;
													if (pass == 0 && pEB->Type->IsBaseDefense)
													{
														if (auto const pW = pEB->GetWeapon(0)->WeaponType)
														{
															reqClearance = std::max(
																reqClearance, static_cast<double>(pW->Range / 256 + 3));
														}
													}

													if (sc.DistanceFrom(ebc) < reqClearance)
														safeFromLongRangeDef = false;
												}

												if (safeFromLongRangeDef)
												{
													pForwardStagingCell = pSC;
													break;
												}
											}
										}
									}
								}
							}
						}
					}

					if (pForwardStagingCell)
					{
						int gatheredNearStage = 0;
						bool anyVanguardUnderFire = false;
						for (size_t i = reserveCount; i < idleCombatUnits.size(); ++i)
						{
							auto const pCheckU = idleCombatUnits[i];
							if (!pCheckU || pCheckU->Type->Naval || IsAmphibiousOrAirTransport(pCheckU->Type))
								continue;

							const CellStruct cuc = pCheckU->GetMapCoords();
							if (MapClass::Instance.GetMovementZoneType(cuc, MovementZone::Normal, pCheckU->OnBridge)
								!= waveTargetZone)
							{
								continue;
							}

							if (IsFootActivelyAttackingOrMovingToEnemy(pCheckU)
								|| IsFootNearEnemyFrontline(pCheckU, waveTargetZone))
							{
								continue;
							}

							if (pCheckU->HasBeenAttacked || cuc.DistanceFrom(tc) <= 18.0)
								anyVanguardUnderFire = true;

							if (cuc.DistanceFrom(pForwardStagingCell->MapCoords) <= 14.0)
								gatheredNearStage++;
						}

						if (!anyVanguardUnderFire && gatheredNearStage * 100 < sameZoneCount * 65)
						{
							holdAtForwardStaging = true;
						}
					}
				}
			}
		}

		size_t dispatchedVeh = 0;
		for (size_t i = reserveCount; i < idleCombatUnits.size(); ++i)
		{
			auto const pU = idleCombatUnits[i];
			const int uz = ObjectZone(pU, MovementZone::Normal);
			// Never abort a unit's current attack target/objective or pull a
			// frontline unit away from an active enemy front!
			if (IsFootActivelyAttackingOrMovingToEnemy(pU) || IsFootNearEnemyFrontline(pU, uz))
				continue;

			const bool canCrossWaterDirectly = (pU->Type->MovementZone == MovementZone::Amphibious
				|| pU->Type->MovementZone == MovementZone::AmphibiousDestroyer
				|| pU->Type->MovementZone == MovementZone::AmphibiousCrusher
				|| pU->Type->MovementZone == MovementZone::Fly || pU->Type->MovementZone == MovementZone::Subterrannean
				|| pU->Type->JumpJet || pU->Type->Teleporter);
			// If this unit was ferried across islands by transport, check if an
			// unattacked sister transport of the battalion is still unloading right
			// next to us (<= 10 cells)
			if (isWaitingForSisterTransportUnload(pU, canCrossWaterDirectly))
			{
				continue; // Regroup battalion on beachhead while unattacked sister
						// transport finishes unloading!
			}

			CellClass* pUnitWaveGoal = pWaveTargetCell;
			int goalZone = waveTargetZone;
			const CellStruct preTransportGoal = LookupPassengerIslandGoal(pU->UniqueID);
			if (preTransportGoal != CellStruct::Empty)
			{
				const int ptz = MapClass::Instance.GetMovementZoneType(preTransportGoal, MovementZone::Normal, false);
				if (ptz >= 0 && uz == ptz)
				{
					if (pU->GetMapCoords().DistanceFrom(preTransportGoal) <= 5.0)
					{
						ClearPassengerIslandGoal(pU->UniqueID);
					}
					else if (CellClass* pPreCell = MapClass::Instance.TryGetCellAt(preTransportGoal))
					{
						pUnitWaveGoal = pPreCell;
						goalZone = ptz;
					}
				}
			}

			if (pUnitWaveGoal == pWaveTargetCell && pSecondaryWaveTargetCell
				&& idleCombatUnits.size() >= (minWaveUnits + 2) && (pU->UniqueID % 3 == 0)
				&& (canCrossWaterDirectly || (secWaveZone >= 0 && uz == secWaveZone)))
			{
				pUnitWaveGoal = pSecondaryWaveTargetCell;
				goalZone = secWaveZone;
			}

			const bool goalInWater = (pUnitWaveGoal->LandType == LandType::Water);
			if (!canCrossWaterDirectly && (goalInWater || goalZone < 0 || uz < 0 || uz != goalZone))
			{
				continue; // Keep ground-only vehicles in Guard state on their island so
						// they board Amphibious/Air Transports!
			}

			if (pU->Type->JumpJet && pU->Type->Strength >= 1000 && pU->DistanceFrom(pUnitWaveGoal) > 7680)
			{
				const CellStruct uCoords = pU->GetMapCoords();
				const CellStruct flankCoords { uCoords.X, pUnitWaveGoal->MapCoords.Y };
				if (auto const pFlankCell = MapClass::Instance.TryGetCellAt(flankCoords))
				{
					IssueMoveOrder(pU, pFlankCell, 6.0);
					NoteExpertCommand(pHouse, pU, CreditSourceWave);
					dispatchedVeh++;
					continue;
				}
			}

			const CellStruct uCellCoords = pU->GetMapCoords();
			// Cohesion for stragglers: once the wave is released, a unit still > 30 cells behind the
			// staging point (late production, slow units) goes through the staging point instead of
			// crossing the map alone straight into the enemy.
			const bool isWaveStraggler = !holdAtForwardStaging && pForwardStagingCell && uz == waveTargetZone
				&& pUnitWaveGoal == pWaveTargetCell && uCellCoords.DistanceFrom(pForwardStagingCell->MapCoords) > 30.0;

			if (isWaveStraggler && pU->CurrentMission == Mission::Guard)
				CountDecision(NeuralDecision::WaveStragglerRegroup, pHouse->ArrayIndex);

			const bool canUseForwardStaging = (holdAtForwardStaging || isWaveStraggler) && pForwardStagingCell && uz == waveTargetZone
				&& pUnitWaveGoal == pWaveTargetCell
				&& (uCellCoords.DistanceFrom(pUnitWaveGoal->MapCoords)
					> pForwardStagingCell->MapCoords.DistanceFrom(pUnitWaveGoal->MapCoords) + 2.0);
			if (canUseForwardStaging)
			{
				AbstractClass* pCurDest = pU->Destination ? pU->Destination : pU->MegaDestination;
				const bool alreadyHeadingToStage = (pCurDest != nullptr && pU->CurrentMission != Mission::Guard
					&& CellClass::Coord2Cell(pCurDest->GetCoords()).DistanceFrom(pForwardStagingCell->MapCoords)
						<= 6.0);
				if (!alreadyHeadingToStage)
				{
					OrderFootToAttackMove(pU, pForwardStagingCell);
				}
			}
			else if (pUnitWaveGoal)
			{
				const int wpnRange = (pU->Type->GetWeapon(0) && pU->Type->GetWeapon(0)->WeaponType)
					? pU->Type->GetWeapon(0)->WeaponType->Range
					: 1280;
				if (pVisibleEnemyBld && pUnitWaveGoal == pWaveTargetCell && CanFootDamageBuildings(pU)
					&& pU->DistanceFrom(pVisibleEnemyBld) <= std::max(1280, wpnRange + 384))
				{
					pU->ClearMegaMissionData();
					pU->SetTarget(pVisibleEnemyBld);
					pU->QueueMission(Mission::Attack, true);
				}
				else
				{
					OrderFootToAttackMove(pU, pUnitWaveGoal);
				}
			}
			else
			{
				pU->ClearMegaMissionData();
				pU->QueueMission(Mission::Hunt, true);
			}

			NoteExpertCommand(pHouse, pU, CreditSourceWave);
			dispatchedVeh++;
		}

		const size_t desiredInfReserve = static_cast<size_t>(std::clamp(1 + (reserveBias / 160), 0, 3));
		const size_t infReserve = idleCombatInf.size() > (desiredInfReserve + 2) ? desiredInfReserve : 0;
		size_t dispatchedInf = 0;
		for (size_t i = infReserve; i < idleCombatInf.size(); ++i)
		{
			auto const pI = idleCombatInf[i];
			const int iz = ObjectZone(pI, MovementZone::Normal);
			if (IsFootActivelyAttackingOrMovingToEnemy(pI) || IsFootNearEnemyFrontline(pI, iz))
				continue;

			const bool canInfCrossWater = (pI->Type->MovementZone == MovementZone::Amphibious
				|| pI->Type->MovementZone == MovementZone::AmphibiousDestroyer
				|| pI->Type->MovementZone == MovementZone::Fly || pI->Type->JumpJet || pI->Type->Teleporter);
			CellClass* pInfWaveGoal = pWaveTargetCell;
			const CellStruct preTransportInfGoal = LookupPassengerIslandGoal(pI->UniqueID);
			if (preTransportInfGoal != CellStruct::Empty)
			{
				const int ptz
					= MapClass::Instance.GetMovementZoneType(preTransportInfGoal, MovementZone::Normal, false);
				if (ptz >= 0 && iz == ptz)
				{
					if (pI->GetMapCoords().DistanceFrom(preTransportInfGoal) <= 5.0)
					{
						ClearPassengerIslandGoal(pI->UniqueID);
					}
					else if (CellClass* pPreCell = MapClass::Instance.TryGetCellAt(preTransportInfGoal))
					{
						pInfWaveGoal = pPreCell;
					}
				}
			}

			if (pInfWaveGoal == pWaveTargetCell && pSecondaryWaveTargetCell && idleCombatInf.size() >= 5
				&& (pI->UniqueID % 3 == 0) && (canInfCrossWater || (secWaveZone >= 0 && iz == secWaveZone)))
			{
				pInfWaveGoal = pSecondaryWaveTargetCell;
			}

			// Reachability on foot uses the soldier's own movement zone, since infantry walks where
			// vehicles cannot. Ground infantry that cannot walk there stays to board transports.
			const bool infGoalInWater = (pInfWaveGoal->LandType == LandType::Water);
			const int footZone = ObjectZone(pI, pI->Type->MovementZone);
			const int goalFootZone = MapClass::Instance.GetMovementZoneType(pInfWaveGoal->MapCoords, pI->Type->MovementZone, false);

			if (!canInfCrossWater && (infGoalInWater || footZone < 0 || footZone != goalFootZone))
				continue;

			const int infWpnRange = (pI->Type->GetWeapon(0) && pI->Type->GetWeapon(0)->WeaponType)
				? pI->Type->GetWeapon(0)->WeaponType->Range
				: 1024;
			if (pVisibleEnemyBld && pInfWaveGoal == pWaveTargetCell
				&& pI->DistanceFrom(pVisibleEnemyBld) <= std::max(1024, infWpnRange + 384))
			{
				pI->ClearMegaMissionData();
				pI->SetTarget(pVisibleEnemyBld);
				pI->QueueMission(Mission::Attack, true);
			}
			else
			{
				OrderFootToAttackMove(pI, pInfWaveGoal);
			}

			NoteExpertCommand(pHouse, pI, CreditSourceWave);
			dispatchedInf++;
		}

		if (dispatchedVeh > 0 || dispatchedInf > 0)
		{
			// A new wave starts after 900 quiet frames or on a different target (wave memory).
			if (Unsorted::CurrentFrame - waveMacro.LastWaveDispatchFrame > 900 || waveMacro.LastWaveTargetCell == CellStruct::Empty
				|| waveMacro.LastWaveTargetCell.DistanceFrom(pWaveTargetCell->MapCoords) > 12.0)
			{
				waveMacro.WaveStartFrame = Unsorted::CurrentFrame;

				if (!endgameRemnantHunt)
					NoteWaveDecision(pHouse, waveCheck, true, static_cast<int>(dispatchedVeh + dispatchedInf));
				// Coalition telemetry: whose building this new wave goes for.
				auto const pWaveBld = pWaveTargetCell->GetBuilding();
				const int waveOwner = (pWaveBld && pWaveBld->Owner) ? pWaveBld->Owner->ArrayIndex : -1;
				auto& waveCoal = waveMacro.Coalition;

				if (waveOwner >= 0 && waveOwner == waveCoal.DyingTarget && Unsorted::CurrentFrame < waveCoal.DyingUntil)
					waveCoal.DyingWaves++;

				if (coalitionTarget < 0)
				{
					waveCoal.FreeWaves++;
				}
				else if (waveOwner == coalitionTarget)
				{
					waveCoal.TargetWaves++;
					CountDecision(NeuralDecision::CoalitionTargetWave, pHouse->ArrayIndex);
				}
				else if (IsTacitTrucePartner(pHouse, waveOwner))
				{
					waveCoal.PartnerWaves++;
				}
				else
				{
					waveCoal.OtherWaves++;
				}
			}

			waveMacro.LastWaveDispatchFrame = Unsorted::CurrentFrame;
			waveMacro.LastWaveTargetCell = pWaveTargetCell->MapCoords;
			NoteTeamWaveDispatched(pHouse, teamOrder, dispatchedVeh, dispatchedInf);

			Debug::Log("[NeuralAI] Frame %d | House #%d (%s) launched Offensive Wave "
					"(%zu Vehicles + %zu Infantry) -> Target Cell (%d,%d)\n",
				Unsorted::CurrentFrame, pHouse->ArrayIndex,
				(pHouse->Type && pHouse->Type->ID[0]) ? pHouse->Type->ID : "AI", dispatchedVeh, dispatchedInf,
				pWaveTargetCell->MapCoords.X, pWaveTargetCell->MapCoords.Y);
		}
	}

	// Slot 75 (Parallel Multi-Group Ground Push & Full-Map Naval Coastal
	// Bombardment Saturation): Combat vehicles/infantry on ANY landmass that
	// shares a MovementZone with enemy structures split into two
	//    parallel assault groups (primary vs secondary target on that landmass)
	//    and push without hesitation!
	// Naval Warships (including long-range capital ships & missile subs whose
	// land-attack weapon is Primary, Secondary,
	//    or Spawner, e.g., Dreadnought, Aircraft Carrier, Boomer, Nod Missile
	//    Sub, Destroyer) scan the ENTIRE map for enemy Naval Yards and
	//    coastal/island structures within bombardment reach of water, distribute
	//    across up to 4 coastal targets in parallel, sail to offshore water
	//    firing cells, and continuously saturate the enemy coast!
	{
		auto findBestSameLandmassEnemyBld
			= [pHouse](const CellStruct& unitCoords, int unitZone, bool pickSecondaryGroup,
				bool allowCrossWater = false) -> BuildingClass*
		{
			// First find the closest enemy structure distance on this landmass so our
			// units peel the outer perimeter first instead of suicidally driving
			// through half the enemy base to reach an interior ConYard/Superweapon!
			int minDistOnLandmass = 999999;
			for (auto const pEB : BuildingClass::Array)
			{
				if (!pEB || pEB->InLimbo || !pEB->IsAlive || !pEB->Owner || !pEB->Type || pEB->Type->Immune
					|| pEB->Type->Insignificant || pEB->Type->InvisibleInGame)
				{
					continue;
				}

				if (pEB->Owner == pHouse || pHouse->IsAlliedWith(pEB->Owner) || !IsEnemyBuildingKnown(pHouse, pEB) || pEB->Owner->IsNeutral()
					|| pEB->Owner->IsObserver() || pEB->Owner->Defeated)
				{
					continue;
				}

				const CellStruct ebc = pEB->GetMapCoords();
				if (!allowCrossWater
					&& (pEB->Type->Naval || unitZone < 0
						|| MapClass::Instance.GetMovementZoneType(ebc, MovementZone::Normal, pEB->OnBridge) != unitZone))
				{
					continue;
				}

				const int d = std::abs(ebc.X - unitCoords.X) + std::abs(ebc.Y - unitCoords.Y);
				if (d < minDistOnLandmass)
					minDistOnLandmass = d;
			}

			BuildingClass* pBestBld = nullptr;
			int bestScore = -999999;
			BuildingClass* pSecondBld = nullptr;
			int secondScore = -999999;
			for (auto const pEB : BuildingClass::Array)
			{
				if (!pEB || pEB->InLimbo || !pEB->IsAlive || !pEB->Owner || !pEB->Type || pEB->Type->Immune
					|| pEB->Type->Insignificant || pEB->Type->InvisibleInGame)
				{
					continue;
				}

				if (pEB->Owner == pHouse || pHouse->IsAlliedWith(pEB->Owner) || !IsEnemyBuildingKnown(pHouse, pEB) || pEB->Owner->IsNeutral()
					|| pEB->Owner->IsObserver() || pEB->Owner->Defeated)
				{
					continue;
				}

				const CellStruct ebc = pEB->GetMapCoords();
				if (!allowCrossWater
					&& (pEB->Type->Naval || unitZone < 0
						|| MapClass::Instance.GetMovementZoneType(ebc, MovementZone::Normal, pEB->OnBridge) != unitZone))
				{
					continue;
				}

				const int distCells = std::abs(ebc.X - unitCoords.X) + std::abs(ebc.Y - unitCoords.Y);
				const int depthBeyondPerimeter = std::max(0, distCells - minDistOnLandmass);
				int prio = 60;
				if (pEB->Type->IsBaseDefense && distCells <= 14)
				{
					prio = 165;
				}
				else if (depthBeyondPerimeter <= 6)
				{
					// Among outer-perimeter structures within 6 cells of the front edge,
					// prioritize high-value targets!
					if (pEB->Type->ConstructionYard || pEB->Type->SuperWeapon != -1)
						prio = 125;
					else if (pEB->Type->Factory != AbstractType::None)
						prio = 115;
					else if (pEB->Type->Refinery || pEB->Type->PowerBonus > 0)
						prio = 95;
					else
						prio = 80;
				}

				const int sc
					= prio - (distCells * 12) - (depthBeyondPerimeter > 6 ? (depthBeyondPerimeter - 6) * 20 : 0);
				if (sc > bestScore)
				{
					if (pBestBld && pBestBld->GetMapCoords().DistanceFrom(ebc) >= 6.0)
					{
						secondScore = bestScore;
						pSecondBld = pBestBld;
					}

					bestScore = sc;
					pBestBld = pEB;
				}
				else if (sc > secondScore && pBestBld && pBestBld->GetMapCoords().DistanceFrom(ebc) >= 6.0)
				{
					secondScore = sc;
					pSecondBld = pEB;
				}
			}

			return (pickSecondaryGroup && pSecondBld) ? pSecondBld : pBestBld;
		};

		auto findCoastalNavalBombardmentTarget
			= [pHouse](const CellStruct& shipCoords, int maxStandOffCells, unsigned int shipUniqueID) -> BuildingClass*
		{
			struct NavalCand
			{
				BuildingClass* Bld;
				int Score;
			};
			NavalCand topCands[4]
				= { { nullptr, -999999 }, { nullptr, -999999 }, { nullptr, -999999 }, { nullptr, -999999 } };
			const int waterCheckRadius = std::clamp(maxStandOffCells, 6, 26);

			for (auto const pEB : BuildingClass::Array)
			{
				if (!pEB || pEB->InLimbo || !pEB->IsAlive || !pEB->Owner || !pEB->Type || pEB->Type->Immune
					|| pEB->Type->Insignificant || pEB->Type->InvisibleInGame)
				{
					continue;
				}

				if (pEB->Owner == pHouse || pHouse->IsAlliedWith(pEB->Owner) || pEB->Owner->IsNeutral()
					|| pEB->Owner->IsObserver() || pEB->Owner->Defeated || !IsEnemyBuildingKnown(pHouse, pEB))
				{
					continue;
				}

				const CellStruct ebc = pEB->GetMapCoords();
				if (ebc.X <= 1 && ebc.Y <= 1)
					continue;

				bool reachableFromWater = pEB->Type->Naval;
				if (!reachableFromWater)
				{
					for (int r = 3; r <= waterCheckRadius && !reachableFromWater; r += 3)
					{
						const CellStruct probes[8] = { { static_cast<short>(ebc.X + r), ebc.Y },
							{ static_cast<short>(ebc.X - r), ebc.Y }, { ebc.X, static_cast<short>(ebc.Y + r) },
							{ ebc.X, static_cast<short>(ebc.Y - r) },
							{ static_cast<short>(ebc.X + (r * 3) / 4), static_cast<short>(ebc.Y + (r * 3) / 4) },
							{ static_cast<short>(ebc.X - (r * 3) / 4), static_cast<short>(ebc.Y + (r * 3) / 4) },
							{ static_cast<short>(ebc.X + (r * 3) / 4), static_cast<short>(ebc.Y - (r * 3) / 4) },
							{ static_cast<short>(ebc.X - (r * 3) / 4), static_cast<short>(ebc.Y - (r * 3) / 4) } };
						for (const auto& pr : probes)
						{
							if (auto const pWC = MapClass::Instance.TryGetCellAt(pr))
							{
								if (pWC->LandType == LandType::Water)
								{
									reachableFromWater = true;
									break;
								}
							}
						}
					}
				}

				if (!reachableFromWater)
					continue;

				const int distCells = std::abs(ebc.X - shipCoords.X) + std::abs(ebc.Y - shipCoords.Y);
				int prio = 200;
				if (pEB->Type->Naval || pEB->Type->IsBaseDefense)
					prio = 360;
				else if (pEB->Type->ConstructionYard || pEB->Type->SuperWeapon != -1)
					prio = 330;
				else if (pEB->Type->Factory != AbstractType::None)
					prio = 300;
				else if (pEB->Type->Refinery || pEB->Type->PowerBonus > 0)
					prio = 260;

				if (pEB->Owner->ArrayIndex == pHouse->EnemyHouseIndex)
					prio += 60;

				const int sc = prio - (distCells * 2);

				for (int k = 0; k < 4; ++k)
				{
					if (sc > topCands[k].Score)
					{
						for (int m = 3; m > k; --m)
							topCands[m] = topCands[m - 1];
						topCands[k] = { pEB, sc };
						break;
					}
				}
			}

			int validCount = 0;
			for (int k = 0; k < 4; ++k)
			{
				if (topCands[k].Bld != nullptr)
					validCount++;
			}

			if (validCount == 0)
				return nullptr;

			return topCands[shipUniqueID % static_cast<unsigned int>(validCount)].Bld;
		};

		auto findOffshoreFiringWaterCell
			= [](UnitClass* pShip, const CellStruct& targetCoords, int desiredRangeCells) -> CellClass*
		{
			const CellStruct sc = pShip->GetMapCoords();
			const int dx = sc.X - targetCoords.X;
			const int dy = sc.Y - targetCoords.Y;
			const int dist = std::max(1, std::max(std::abs(dx), std::abs(dy)));
			const int lat = (static_cast<int>(pShip->UniqueID % 5) - 2) * 2;
			const int perpX = (-dy * lat) / dist;
			const int perpY = (dx * lat) / dist;
			const int maxR = std::clamp(desiredRangeCells, 5, 22);
			for (int r = std::max(4, maxR - 3); r <= maxR; ++r)
			{
				const CellStruct probe { static_cast<short>(targetCoords.X + (dx * r) / dist + perpX),
					static_cast<short>(targetCoords.Y + (dy * r) / dist + perpY) };
				if (auto const pWC = MapClass::Instance.TryGetCellAt(probe))
				{
					if (pWC->LandType == LandType::Water
						&& pWC->IsClearToMove(
							pShip->Type->SpeedType, true, true, -1, pShip->Type->MovementZone, -1, false))
					{
						return pWC;
					}
				}
			}

			CellClass* pBestWater = nullptr;
			int bestWaterDist = 999999;
			for (int wy = -maxR; wy <= maxR; wy += 2)
			{
				for (int wx = -maxR; wx <= maxR; wx += 2)
				{
					const int dTarget = std::max(std::abs(wx), std::abs(wy));
					if (dTarget < 3 || dTarget > maxR)
						continue;

					const CellStruct wc { static_cast<short>(targetCoords.X + wx),
						static_cast<short>(targetCoords.Y + wy) };
					if (auto const pWC = MapClass::Instance.TryGetCellAt(wc))
					{
						if (pWC->LandType == LandType::Water
							&& pWC->IsClearToMove(
								pShip->Type->SpeedType, true, true, -1, pShip->Type->MovementZone, -1, false))
						{
							const int dShip = std::abs(wc.X - sc.X) + std::abs(wc.Y - sc.Y);
							if (dShip < bestWaterDist)
							{
								bestWaterDist = dShip;
								pBestWater = pWC;
							}
						}
					}
				}
			}

			return pBestWater;
		};

		auto findNearestSameZoneEnemyUnit
			= [pHouse](const CellStruct& unitCoords, int unitZone, int maxDistCells) -> UnitClass*
		{
			UnitClass* pBestEU = nullptr;
			int bestDist = maxDistCells + 1;
			for (auto const pEU : UnitClass::Array)
			{
				if (!pEU || pEU->InLimbo || !pEU->IsAlive || !pEU->Owner || !pEU->Type || pEU->Type->Naval)
					continue;

				if (pEU->Owner == pHouse || pHouse->IsAlliedWith(pEU->Owner) || pEU->Owner->IsNeutral()
					|| pEU->Owner->IsObserver() || pEU->Owner->Defeated)
				{
					continue;
				}

				const CellStruct euc = pEU->GetMapCoords();
				const int d = std::abs(euc.X - unitCoords.X) + std::abs(euc.Y - unitCoords.Y);
				if (d >= bestDist)
					continue;

				if (unitZone >= 0
					&& MapClass::Instance.GetMovementZoneType(euc, MovementZone::Normal, pEU->OnBridge) != unitZone)
				{
					continue;
				}

				if (pEU->GetCell() && IsCellVisibleToAI(pHouse, pEU->GetCell()))
				{
					bestDist = d;
					pBestEU = pEU;
				}
			}

			return pBestEU;
		};

		for (auto const pBeachU : idleCombatUnits)
		{
			if (!pBeachU || !pBeachU->Type || IsAmphibiousOrAirTransport(pBeachU->Type))
				continue;

			if (IsFootActivelyAttackingOrMovingToEnemy(pBeachU))
			{
				continue; // Already attacking or moving toward an enemy objective:
						// never abort its current target!
			}

			const auto pWpn0 = pBeachU->Type->GetWeapon(0) ? pBeachU->Type->GetWeapon(0)->WeaponType : nullptr;
			const auto pWpn1 = pBeachU->Type->GetWeapon(1) ? pBeachU->Type->GetWeapon(1)->WeaponType : nullptr;
			const bool isCarrierSpawner
				= (pBeachU->Type->Spawns != nullptr) || (pWpn0 && pWpn0->Spawner) || (pWpn1 && pWpn1->Spawner);
			if ((!pWpn0 || pWpn0->Damage <= 0) && (!pWpn1 || pWpn1->Damage <= 0) && !isCarrierSpawner)
				continue;

			const CellStruct uc = pBeachU->GetMapCoords();
			const int uz = MapClass::Instance.GetMovementZoneType(uc, MovementZone::Normal, pBeachU->OnBridge);
			const bool canBeachUCrossWater = (pBeachU->Type->MovementZone == MovementZone::Amphibious
				|| pBeachU->Type->MovementZone == MovementZone::AmphibiousDestroyer
				|| pBeachU->Type->MovementZone == MovementZone::AmphibiousCrusher
				|| pBeachU->Type->MovementZone == MovementZone::Fly
				|| pBeachU->Type->MovementZone == MovementZone::Subterrannean || pBeachU->Type->JumpJet
				|| pBeachU->Type->Teleporter);

			if (pBeachU->Type->Naval && !canBeachUCrossWater)
			{
				const bool wpn0HitsLand = pWpn0 && (pWpn0->Damage > 0 || pWpn0->Spawner)
					&& (!pWpn0->Projectile || pWpn0->Projectile->AG || pWpn0->Spawner || isCarrierSpawner);
				const bool wpn1HitsLand = pWpn1 && (pWpn1->Damage > 0 || pWpn1->Spawner)
					&& (!pWpn1->Projectile || pWpn1->Projectile->AG || pWpn1->Spawner);
				const WeaponTypeClass* pLandSiegeWpn = nullptr;
				if (wpn0HitsLand && wpn1HitsLand)
					pLandSiegeWpn = (pWpn1->Range > pWpn0->Range) ? pWpn1 : pWpn0;
				else if (wpn1HitsLand)
					pLandSiegeWpn = pWpn1;
				else if (wpn0HitsLand)
					pLandSiegeWpn = pWpn0;

				if (!pLandSiegeWpn)
				{
					// Anti-naval / anti-sub warship (Torpedo Sub, Dolphin, Squid): hunt
					// enemy ships or Naval Yards!
					if (pEnemySubOrShip)
					{
						pBeachU->SetTarget(pEnemySubOrShip);
						pBeachU->QueueMission(Mission::Attack, true);
					}

					continue;
				}

				const int siegeRangeLeptons = std::max(pLandSiegeWpn->Range, isCarrierSpawner ? (18 * 256) : 1536);
				const int siegeRangeCells = std::clamp(siegeRangeLeptons / 256, 6, 26);
				if (BuildingClass* pNavalTargetBld
					= findCoastalNavalBombardmentTarget(uc, siegeRangeCells, pBeachU->UniqueID))
				{
					const int distLeptons = pBeachU->DistanceFrom(pNavalTargetBld);
					if (distLeptons <= std::max(2560, siegeRangeLeptons))
					{
						pBeachU->ClearMegaMissionData();
						pBeachU->SetTarget(pNavalTargetBld);
						pBeachU->QueueMission(Mission::Attack, true);
					}
					else if (CellClass* pOffshoreFireCell = findOffshoreFiringWaterCell(
								pBeachU, pNavalTargetBld->GetMapCoords(), std::max(5, siegeRangeCells - 2)))
					{
						OrderFootToAttackMove(pBeachU, pOffshoreFireCell);
					}
					else if (auto const pNCell = pNavalTargetBld->GetCell())
					{
						OrderFootToAttackMove(pBeachU, pNCell);
					}
				}

				continue;
			}

			if (isWaitingForSisterTransportUnload(pBeachU, canBeachUCrossWater))
			{
				continue; // Wait on beachhead while unattacked adjacent sister
						// transport finishes unloading!
			}

			const int maxWpnRangeLeptons = std::max(
				pWpn0 ? static_cast<int>(pWpn0->Range) : 1280, pWpn1 ? static_cast<int>(pWpn1->Range) : 1280);
			UnitClass* pCloseEnemyVeh = findNearestSameZoneEnemyUnit(uc, uz, 12);
			if (pCloseEnemyVeh)
			{
				if (pBeachU->DistanceFrom(pCloseEnemyVeh) <= maxWpnRangeLeptons + 384)
				{
					pBeachU->ClearMegaMissionData();
					pBeachU->SetTarget(pCloseEnemyVeh);
					pBeachU->QueueMission(Mission::Attack, true);
				}
				else if (auto const pCloseECell = pCloseEnemyVeh->GetCell())
				{
					OrderFootToAttackMove(pBeachU, pCloseECell);
				}

				continue;
			}

			// Resume pre-transport initial objective if this unit just landed on its
			// target island!
			const CellStruct preGoal = LookupPassengerIslandGoal(pBeachU->UniqueID);
			if (preGoal != CellStruct::Empty && uz >= 0
				&& MapClass::Instance.GetMovementZoneType(preGoal, MovementZone::Normal, false) == uz)
			{
				if (uc.DistanceFrom(preGoal) <= 5.0)
				{
					ClearPassengerIslandGoal(pBeachU->UniqueID);
				}
				else if (CellClass* pPreGoalCell = MapClass::Instance.TryGetCellAt(preGoal))
				{
					OrderFootToAttackMove(pBeachU, pPreGoalCell);
					continue;
				}
			}

			if (BuildingClass* pSameIslBld
				= findBestSameLandmassEnemyBld(uc, uz, (pBeachU->UniqueID & 1) != 0, canBeachUCrossWater))
			{
				const double distToBld = uc.DistanceFrom(pSameIslBld->GetMapCoords());
				const bool onEnemyIsland = (homeGroundZone >= 0 && uz >= 0 && uz != homeGroundZone);
				// Do NOT trickle units 1-by-1 from our home base when
				// idleCombatUnits.size() < minWaveUnits! Only engage on home island
				// once critical mass (>= minWaveUnits) is assembled, or if the unit is
				// already at the enemy perimeter (<= 20 cells)!
				if (onEnemyIsland || endgameRemnantHunt || distToBld <= 20.0 || (idleCombatUnits.size() >= minWaveUnits && waveStrengthReady))
				{
					if (CanFootDamageBuildings(pBeachU) && pBeachU->DistanceFrom(pSameIslBld) <= maxWpnRangeLeptons + 384
						&& pSameIslBld->GetCell()
						&& (pHouse->SpySatActive || IsCellVisibleToAI(pHouse, pSameIslBld->GetCell())))
					{
						pBeachU->ClearMegaMissionData();
						pBeachU->SetTarget(pSameIslBld);
						pBeachU->QueueMission(Mission::Attack, true);
					}
					else if (auto const pSameIslCell = pSameIslBld->GetCell())
					{
						OrderFootToAttackMove(pBeachU, pSameIslCell);
					}
				}
			}
		}

		for (auto const pBeachI : idleCombatInf)
		{
			if (!pBeachI || !pBeachI->Type)
				continue;

			if (IsFootActivelyAttackingOrMovingToEnemy(pBeachI))
				continue;

			const CellStruct ic = pBeachI->GetMapCoords();
			const int iz = MapClass::Instance.GetMovementZoneType(ic, MovementZone::Normal, pBeachI->OnBridge);
			const bool canBeachICrossWater = (pBeachI->Type->MovementZone == MovementZone::Amphibious
				|| pBeachI->Type->MovementZone == MovementZone::AmphibiousDestroyer
				|| pBeachI->Type->MovementZone == MovementZone::Fly || pBeachI->Type->JumpJet
				|| pBeachI->Type->Teleporter);
			if (isWaitingForSisterTransportUnload(pBeachI, canBeachICrossWater))
				continue;

			const CellStruct preInfGoal = LookupPassengerIslandGoal(pBeachI->UniqueID);
			if (preInfGoal != CellStruct::Empty && iz >= 0
				&& MapClass::Instance.GetMovementZoneType(preInfGoal, MovementZone::Normal, false) == iz)
			{
				if (ic.DistanceFrom(preInfGoal) <= 5.0)
				{
					ClearPassengerIslandGoal(pBeachI->UniqueID);
				}
				else if (CellClass* pPreInfCell = MapClass::Instance.TryGetCellAt(preInfGoal))
				{
					OrderFootToAttackMove(pBeachI, pPreInfCell);
					continue;
				}
			}

			if (BuildingClass* pSameIslBld
				= findBestSameLandmassEnemyBld(ic, iz, (pBeachI->UniqueID & 1) != 0, canBeachICrossWater))
			{
				const double distToBld = ic.DistanceFrom(pSameIslBld->GetMapCoords());
				const bool onEnemyIsland = (homeGroundZone >= 0 && iz >= 0 && iz != homeGroundZone);
				if (onEnemyIsland || endgameRemnantHunt || distToBld <= 20.0
					|| ((idleCombatUnits.size() >= minWaveUnits || idleCombatInf.size() >= (minWaveUnits + 3)) && waveStrengthReady))
				{
					const auto pInfW0 = pBeachI->Type->GetWeapon(0) ? pBeachI->Type->GetWeapon(0)->WeaponType : nullptr;
					const int infWpnRange = pInfW0 ? static_cast<int>(pInfW0->Range) : 1024;
					if (pBeachI->DistanceFrom(pSameIslBld) <= infWpnRange + 384 && pSameIslBld->GetCell()
						&& (pHouse->SpySatActive || IsCellVisibleToAI(pHouse, pSameIslBld->GetCell())))
					{
						pBeachI->ClearMegaMissionData();
						pBeachI->SetTarget(pSameIslBld);
						pBeachI->QueueMission(Mission::Attack, true);
					}
					else if (auto const pSameIslCell = pSameIslBld->GetCell())
					{
						OrderFootToAttackMove(pBeachI, pSameIslCell);
					}
				}
			}
		}
	}

	microClock.Mark(NeuralSection::MicroTransports);

	// Count how many amphibious transports are currently loading on our home
	// island so loaded transports can synchronize and sail together as a D-Day
	// Convoy!
	UnitClass* pHomeExpansionDeployer = nullptr;
	if (homeGroundZone >= 0
		&& (pIslandResourceGoalCell != nullptr || !hasUncolonizedOreOnHomeIsland || pHouse->Buildings.Count >= 16))
	{
		for (auto const pOwnU : UnitClass::Array)
		{
			if (!pOwnU || pOwnU->Owner != pHouse || pOwnU->InLimbo || !pOwnU->IsAlive || !pOwnU->Type
				|| !pOwnU->Type->DeploysInto)
			{
				continue;
			}

			const auto pUT = pOwnU->Type;
			const bool isSelfAmphibOrAir = (pUT->MovementZone == MovementZone::Amphibious
				|| pUT->MovementZone == MovementZone::AmphibiousDestroyer
				|| pUT->MovementZone == MovementZone::AmphibiousCrusher || pUT->MovementZone == MovementZone::Fly
				|| pUT->JumpJet);
			if (isSelfAmphibOrAir)
				continue;

			const int uz = ObjectZone(pOwnU, MovementZone::Normal);
			if (uz < 0 || uz == resourceIslandZone)
				continue;

			int conYardsInUz = 0;
			int hubsInUz = 0;
			for (auto const pOwnB : pHouse->Buildings)
			{
				if (!pOwnB || pOwnB->InLimbo || !pOwnB->IsAlive || !pOwnB->Type || pOwnB->Type->Insignificant
					|| pOwnB->Type->InvisibleInGame)
				{
					continue;
				}

				if (ObjectZone(pOwnB, MovementZone::Normal) != uz)
					continue;

				if (pOwnB->Type->ConstructionYard)
					conYardsInUz++;

				if (pOwnB->Type->Refinery || pOwnB->Type->ResourceDestination || pOwnB->Type->BaseNormal
					|| pOwnB->Type->Factory != AbstractType::None)
				{
					hubsInUz++;
				}
			}

			const bool isExpMCV = (pUT->DeploysInto->ConstructionYard && (conYardsInUz >= 1 || hubsInUz >= 2));
			const bool isExpOutpostOrRef = (!pUT->DeploysInto->ConstructionYard && hubsInUz >= 1
				&& (pOwnU == pExtraHomeSlaveMiner
					|| (!pUT->Harvester && !pUT->ResourceGatherer
						&& (pUT->DeploysInto->Refinery || pUT->DeploysInto->ResourceDestination
							|| pUT->DeploysInto->BaseNormal || pUT->DeploysInto->Factory != AbstractType::None))));
			if (isExpMCV || isExpOutpostOrRef)
			{
				pHomeExpansionDeployer = pOwnU;
				if (isExpMCV)
					break;
			}
		}
	}

	InfantryClass* pHomeIslandEngineer = nullptr;
	if (homeGroundZone >= 0 && pIslandTechGoalCell != nullptr)
	{
		for (auto const pOwnInf : InfantryClass::Array)
		{
			if (!pOwnInf || pOwnInf->Owner != pHouse || pOwnInf->InLimbo || !pOwnInf->IsAlive || !pOwnInf->Type
				|| !pOwnInf->Type->Engineer)
			{
				continue;
			}

			const int iz = ObjectZone(pOwnInf, MovementZone::Normal);
			if (iz == homeGroundZone)
			{
				pHomeIslandEngineer = pOwnInf;
				break;
			}
		}
	}

	// Publish the real ferry demand for the production selector: only the units this
	// controller would actually board (pending cross-island orders, idle ground units
	// stranded away from the enemy island, home deployer/engineer). Units already
	// fighting, already on the target island or able to cross water are not demand.
	{
		auto& macro = GetMacroState(pHouse);
		macro.FerryDemandSizeBySize.fill(0);
		macro.FerryDemandFrame = Unsorted::CurrentFrame;

		std::vector<const FootClass*> countedFerryPassengers;

		auto addFerryDemand = [&](const FootClass* pFoot)
		{
			if (!pFoot || pFoot->InLimbo || !pFoot->IsAlive || CanFootCrossWaterDirectly(pFoot))
				return;

			auto const pFootType = pFoot->GetTechnoType();
			if (!pFootType || IsAmphibiousOrAirTransport(pFootType))
				return;

			if (std::find(countedFerryPassengers.begin(), countedFerryPassengers.end(), pFoot)
				!= countedFerryPassengers.end())
			{
				return;
			}

			countedFerryPassengers.push_back(pFoot);
			const int size = std::clamp(static_cast<int>(pFootType->Size), 1, 15);
			macro.FerryDemandSizeBySize[size] += size;
		};

		for (auto const& ord : pendingCrossIslandOrders)
			addFerryDemand(ord.Passenger);

		if (enemyOnDisconnectedIsland && targetIslandZone >= 0)
		{
			auto isStrandedAwayFromEnemyIsland = [&](const FootClass* pFoot) -> bool
			{
				const int zone = ObjectZone(pFoot, MovementZone::Normal);

				return zone >= 0 && zone != targetIslandZone && zone != secondaryIslandZone
					&& !IsFootActivelyAttackingOrMovingToEnemy(pFoot) && !IsFootNearEnemyFrontline(pFoot, zone);
			};

			for (auto const pU : idleCombatUnits)
			{
				if (pU && isStrandedAwayFromEnemyIsland(pU))
					addFerryDemand(pU);
			}

			for (auto const pI : idleCombatInf)
			{
				if (pI && isStrandedAwayFromEnemyIsland(pI))
					addFerryDemand(pI);
			}
		}

		addFerryDemand(pHomeExpansionDeployer);
		addFerryDemand(pHomeIslandEngineer);
	}

	// Any real reason to ferry this tick: waiting cargo, or an island resource/tech goal.
	bool hasAnyFerryDemand = (pIslandResourceGoalCell && resourceIslandZone >= 0 && resourceIslandZone != homeGroundZone)
		|| (pIslandTechGoalCell && techIslandZone >= 0 && techIslandZone != homeGroundZone);

	for (const int demandSize : GetMacroState(pHouse).FerryDemandSizeBySize)
		hasAnyFerryDemand = hasAnyFerryDemand || demandSize > 0;

	int readyCombatConvoyTransports = 0;

	for (auto const pCheckT : UnitClass::Array)
	{
		if (!pCheckT || pCheckT->Owner != pHouse || pCheckT->InLimbo || !pCheckT->IsAlive || !pCheckT->Type
			|| !IsAmphibiousOrAirTransport(pCheckT->Type))
		{
			continue;
		}

		if (pCheckT->Type->MovementZone == MovementZone::Fly || pCheckT->Type->JumpJet)
			continue;

		const CellStruct checkCoords = pCheckT->GetMapCoords();
		const int tz = MapClass::Instance.GetMovementZoneType(checkCoords, MovementZone::Normal, pCheckT->OnBridge);
		const int checkFreeCap = pCheckT->Type->Passengers - pCheckT->Passengers.GetTotalSize();
		const bool isCheckFull = (pCheckT->Passengers.NumPassengers >= pCheckT->Type->Passengers || checkFreeCap <= 0);
		if (isCheckFull && (targetIslandZone < 0 || tz != targetIslandZone))
			readyCombatConvoyTransports++;
	}

	// Helper: Enforce strict 1-by-1 boarding on a transport (UnitClass or AircraftClass).
	// Because TechnoClass/RadioClass only has 1 radio link slot for RequestLoading (0x0073781E),
	// if multiple units are in Mission::Enter targeting the same transport simultaneously, they
	// steal each other's radio link and jam the 8 adjacent cells around the transport!
	// This helper keeps at most ONE active boarder (preferring the unit with ContainsLink(pTransport),
	// or otherwise the closest unit), unlinks/stops all extra Mission::Enter units targeting pTransport,
	// and scatters any other friendly units standing on the 8 cells adjacent to pTransport.
	auto enforceSingleActiveBoarderOn = [&](FootClass* pTransport, int pickupZone) -> FootClass*
	{
		if (!pTransport || !pTransport->IsAlive || pTransport->InLimbo)
			return nullptr;

		const CellStruct trCoords = pTransport->GetMapCoords();
		FootClass* pChosenBoarder = nullptr;
		double chosenScore = 999999.0;

		auto evaluateBoarder = [&](FootClass* pCand)
		{
			if (!pCand || pCand->Owner != pHouse || pCand->InLimbo || !pCand->IsAlive || pCand == pTransport)
				return;

			const bool isEnteringThis = (pCand->CurrentMission == Mission::Enter || pCand->QueuedMission == Mission::Enter)
				&& (pCand->ContainsLink(pTransport) || pCand->ArchiveTarget == pTransport
					|| pCand->Destination == pTransport);
			if (!isEnteringThis)
				return;

			const int cz = ObjectZone(pCand, MovementZone::Normal);
			if (pickupZone >= 0 && cz != pickupZone)
				return;

			const double dist = pCand->GetMapCoords().DistanceFrom(trCoords);
			// Strongly prefer the unit that already holds the active radio link with pTransport
			const double score = pCand->ContainsLink(pTransport) ? (dist - 1000.0) : dist;
			if (score < chosenScore)
			{
				chosenScore = score;
				pChosenBoarder = pCand;
			}
		};

		for (auto const pCheckU : UnitClass::Array)
			evaluateBoarder(pCheckU);
		for (auto const pCheckI : InfantryClass::Array)
			evaluateBoarder(pCheckI);

		auto stopExtraBoarder = [&](FootClass* pCand)
		{
			if (!pCand || pCand->Owner != pHouse || pCand->InLimbo || !pCand->IsAlive || pCand == pTransport
				|| pCand == pChosenBoarder)
			{
				return;
			}

			const bool isTargetingThis = (pCand->CurrentMission == Mission::Enter || pCand->QueuedMission == Mission::Enter)
				&& (pCand->ContainsLink(pTransport) || pCand->ArchiveTarget == pTransport
					|| pCand->Destination == pTransport);
			if (!isTargetingThis)
				return;

			if (pCand->ContainsLink(pTransport))
				pCand->SendCommand(RadioCommand::NotifyUnlink, pTransport);

			pCand->SetArchiveTarget(nullptr);
			pCand->SetDestination(nullptr, true);
			pCand->ClearNavigationList();
			pCand->ForceMission(Mission::Guard);
		};

		for (auto const pCheckU : UnitClass::Array)
			stopExtraBoarder(pCheckU);
		for (auto const pCheckI : InfantryClass::Array)
			stopExtraBoarder(pCheckI);

		// Keep the 8 adjacent cells around pTransport clear of any non-boarding friendly units
		// so pChosenBoarder has a clear path into pTransport without getting jammed!
		// Gather all of them first: a scattered unit may walk into the next cell and must not be revisited.
		std::vector<FootClass*> adjacentFoots;

		for (int dy = -1; dy <= 1; ++dy)
		{
			for (int dx = -1; dx <= 1; ++dx)
			{
				if (dx == 0 && dy == 0)
					continue;

				const CellStruct adjC {
					static_cast<short>(trCoords.X + dx),
					static_cast<short>(trCoords.Y + dy)
				};
				CollectCellFoots(MapClass::Instance.TryGetCellAt(adjC), adjacentFoots);
			}
		}

		for (auto const pFootAdj : adjacentFoots)
		{
			if (pFootAdj->Owner != pHouse || !pFootAdj->IsAlive || pFootAdj->InLimbo
				|| pFootAdj == pTransport || pFootAdj == pChosenBoarder)
			{
				continue;
			}

			if (auto const pUnitAdj = abstract_cast<UnitClass*>(pFootAdj))
			{
				if (IsAmphibiousOrAirTransport(pUnitAdj->Type))
					continue; // Never move parked transports!
			}

			if (pFootAdj->CurrentMission != Mission::Move && pFootAdj->CurrentMission != Mission::Attack)
				pFootAdj->Scatter(CoordStruct::Empty, true, false);
		}

		return pChosenBoarder;
	};

	bool deployerTransportAssigned = false;

	// A departed, loaded transport of ours that no longer exists was destroyed on its way:
	// remember where, so the next landings pick another beach.
	{
		std::unordered_set<DWORD> aliveTransports;

		for (auto const pT : UnitClass::Array)
		{
			if (pT && pT->Owner == pHouse && pT->IsAlive && !pT->InLimbo)
				aliveTransports.insert(pT->UniqueID);
		}

		auto& bloody = GetMacroState(pHouse).BloodyBeaches;

		for (auto& trip : FerryTrips)
		{
			if (trip.TransportID == 0 || trip.HouseIndex != pHouse->ArrayIndex || aliveTransports.contains(trip.TransportID))
				continue;

			if (trip.Departed && trip.LastCoords != CellStruct::Empty)
				bloody.emplace_back(trip.LastCoords, Unsorted::CurrentFrame);

			trip = NeuralFerryTripRecord {};
		}

		std::erase_if(bloody, [](const auto& entry) { return Unsorted::CurrentFrame - entry.second > 5400; });

		if (bloody.size() > 16)
			bloody.erase(bloody.begin(), bloody.begin() + static_cast<std::ptrdiff_t>(bloody.size() - 16));
	}

	for (auto const pTrans : UnitClass::Array)
	{
		// Transports flying a strategic plan (e.g. the airborne second front) belong to that plan.
		if (!pTrans || pTrans->Owner != pHouse || pTrans->InLimbo || !pTrans->IsAlive || !pTrans->Type
			|| IsPlanMember(pHouse, pTrans->UniqueID))
		{
			continue;
		}

		// Subterranean APC / Ground APC Strike Team: If carrying Engineers or >= 3
		// Infantry, burrow/drive to enemy base and Unload! (Never unload
		// NoManualUnload/NoManualEnter units!)
		// Amphibious/air/subterranean transports join only while no water crossing needs them
		// (IsTransportFreeForLandStrike); when empty they still fall through to the ferry
		// logic below, which handles their repairs.
		if (IsTransportFreeForLandStrike(pHouse, pTrans) && pTrans->Type->Passengers >= 3 && !pTrans->Type->OpenTopped
			&& !pTrans->Type->Gunner && !HasNoManualUnload(pTrans->Type) && !HasNoManualEnter(pTrans->Type)
			&& (!IsAmphibiousOrAirTransport(pTrans->Type) || pTrans->Passengers.NumPassengers > 0))
		{
			if (pTrans->Passengers.NumPassengers > 0 && pVisibleEnemyBld)
			{
				bool hasEngineerOrFullSquad = (pTrans->Passengers.NumPassengers >= 3);

				for (FootClass* pPass = pTrans->Passengers.GetFirstPassenger(); pPass != nullptr;
					pPass = abstract_cast<FootClass*>(pPass->NextObject))
				{
					if (auto const pPassInf = abstract_cast<InfantryClass*>(pPass))
					{
						if (pPassInf->Type && (pPassInf->Type->Engineer || pPassInf->Type->C4))
						{
							hasEngineerOrFullSquad = true;

							break;
						}
					}
				}

				if (hasEngineerOrFullSquad)
				{
					if (pTrans->DistanceFrom(pVisibleEnemyBld) <= 640)
					{
						if (pTrans->CurrentMission != Mission::Unload)
							pTrans->QueueMission(Mission::Unload, true);
					}
					else if (pTrans->CurrentMission == Mission::Guard || pTrans->CurrentMission == Mission::Area_Guard)
					{
						if (auto const pEnemyCell = pVisibleEnemyBld->GetCell())
						{
							pTrans->SetDestination(pEnemyCell, true);

							pTrans->QueueMission(Mission::Move, true);
						}
					}
				}
			}

			continue;
		}

		if (auto const pTransTypeExt = TechnoTypeExt::Fetch(pTrans->Type))
		{
			if (pTransTypeExt->Carryall && pTrans->Passengers.NumPassengers == 0)
			{
				if (auto const pCarryExt = UnitExt::Fetch(pTrans))
				{
					if (pCarryExt->CarryallCargo && pCarryExt->CarryallCargo->IsAlive)
					{
						auto const pCarriedUnit = pCarryExt->CarryallCargo;
						CellClass* pDropCell = nullptr;
						const CellStruct customCoords = LookupPassengerIslandGoal(pCarriedUnit->UniqueID);
						if (customCoords != CellStruct::Empty)
						{
							const int cz
								= MapClass::Instance.GetMovementZoneType(customCoords, MovementZone::Normal, false);
							pDropCell = FindSafeIslandLandingCell(pHouse, customCoords, cz,
								pCarriedUnit->Type && pCarriedUnit->Type->DeploysInto != nullptr);
						}

						if (!pDropCell && pCarriedUnit->Type)
						{
							BuildingClass* const pCarriedRepair = (pRepairDepot && (pCarriedUnit->GetHealthPercentage() < 0.45
									|| (pCarriedUnit->Type->Cost >= 1000 && pCarriedUnit->GetHealthPercentage() < 0.75)))
								? FindBestRepairStation(pHouse, pCarriedUnit, true)
								: nullptr;

							if (pCarriedRepair)
							{
								pDropCell = pCarriedRepair->GetCell();
							}
							else if ((pCarriedUnit->Type->DeploysInto != nullptr || pCarriedUnit->Type->Harvester
										|| pCarriedUnit->Type->Weeder)
								&& pIslandResourceGoalCell)
							{
								pDropCell = pIslandResourceGoalCell;
							}
						}

						if (!pDropCell)
						{
							pDropCell = pIslandGoalCell
								? pIslandGoalCell
								: (pIslandTechGoalCell
										? pIslandTechGoalCell
										: (pIslandResourceGoalCell ? pIslandResourceGoalCell : pWaveTargetCell));
						}

						if (pDropCell && pCarryExt->CarryallState == JumpjetCarryallState::Carrying)
						{
							// Badly damaged, or the drop zone got covered by anti-air: bring the cargo home.
							pDropCell = ResolveAirDropCell(pHouse, pTrans, pDropCell);

							if (pTrans->GetMapCoords().DistanceFrom(pDropCell->MapCoords) <= 2.5
								&& pCarryExt->CarryallCanDropAt(pTrans->GetCell()))
							{
								if (pTrans->CurrentMission != Mission::Unload)
									CountDecision(NeuralDecision::AirliftDrop, pHouse->ArrayIndex);

								pTrans->QueueMission(Mission::Unload, true);
							}
							else if (pTrans->CurrentMission == Mission::Guard
								|| pTrans->CurrentMission == Mission::Area_Guard || (IsAITickWindow(90, 25)))
							{
								// Fly around known enemy anti-air when a safer lane exists.
								IssueMoveOrder(pTrans, GetSafeAirHop(pHouse, pTrans, pDropCell), 3.0);
							}
						}

						continue;
					}
					else if (UnitClass* const pPendingCargo = (pCarryExt->CarryallState == JumpjetCarryallState::Idle)
								 ? FindPendingPickupCargo(pHouse, pTrans)
								 : nullptr)
					{
						// Still flying the safe detour toward its pickup.
						if (pCarryExt->CarryallCanLift(pPendingCargo))
						{
							if (pTrans->CurrentMission == Mission::Guard || pTrans->CurrentMission == Mission::Area_Guard
								|| IsAITickWindow(90, 25))
							{
								AdvanceRoutedPickup(pHouse, pTrans, pPendingCargo, true);
							}

							continue;
						}

						AirPickups.erase(pTrans->UniqueID);
					}
					else if (pCarryExt->CarryallState == JumpjetCarryallState::Idle
						&& (pTrans->CurrentMission == Mission::Guard || pTrans->CurrentMission == Mission::Area_Guard)
						&& (IsAITickWindow(90, 35)))
					{
						bool pickedCarryallTarget = false;
						for (auto const& ord : pendingCrossIslandOrders)
						{
							if (auto const pOrdU = abstract_cast<UnitClass*>(ord.Passenger))
							{
								if (pOrdU->IsAlive && !pOrdU->InLimbo && pCarryExt->CarryallCanLift(pOrdU))
								{
									AdvanceRoutedPickup(pHouse, pTrans, pOrdU, true);
									CountDecision(NeuralDecision::AirliftJob, pHouse->ArrayIndex);
									pickedCarryallTarget = true;
									break;
								}
							}
						}

						const bool islandLiftsPaused
							= Unsorted::CurrentFrame - GetMacroState(pHouse).LastAirDropAbortFrame < 1800;
						const bool evacuationAllowed = pRepairDepot && SlotGate(scores[54], -200, 54);

						for (auto const pU : UnitClass::Array)
						{
							if (pickedCarryallTarget)
								break;

							if (!pU || pU->Owner != pHouse || pU->InLimbo || !pU->IsAlive || !pU->Type
								|| !pCarryExt->CarryallCanLift(pU))
							{
								continue;
							}

							const int uZone = ObjectZone(pU, MovementZone::Normal);
							const bool isCriticallyDamagedTank
								= (pRepairDepot && pU->Type->Cost >= 1000 && pU->GetHealthPercentage() < 0.40);
							const bool isLoadedStrikeAPC
								= (pU->Type->Passengers >= 3 && pU->Passengers.NumPassengers >= 1
									&& (pWaveTargetCell || pIslandGoalCell || pIslandTechGoalCell));
							const bool isIslandExpansionMCV = (pU->Type->DeploysInto != nullptr
								&& pIslandResourceGoalCell && uZone >= 0 && uZone != resourceIslandZone
								&& (pU == pHomeExpansionDeployer || pHouse->NumConYards >= 1
									|| pHouse->CountOwnedNow(pU->Type) >= 2 || pHouse->OwnedBuildings >= 6));
							const bool isIslandHarvester
								= ((pU->Type->Harvester || pU->Type->Weeder) && pIslandResourceGoalCell
									&& uZone == homeGroundZone && pHouse->CountOwnedNow(pU->Type) >= 2);
							const bool isIslandCombatVehicle = (pIslandGoalCell != nullptr && uZone == homeGroundZone
								&& !IsAmphibiousOrAirTransport(pU->Type) && !pU->Type->Harvester && !pU->Type->Weeder
								&& pU->Type->DeploysInto == nullptr
								&& (pU->CurrentMission == Mission::Guard || pU->CurrentMission == Mission::Area_Guard)
								&& !IsFootActivelyAttackingOrMovingToEnemy(pU) && !IsFootNearEnemyFrontline(pU, uZone));
							// Combat evacuation: an expensive unit losing its fight is pulled out early.
							const bool isLosingExpensiveUnit = evacuationAllowed && !isCriticallyDamagedTank
								&& pU->Type->Cost >= 1000 && pU->GetHealthPercentage() < 0.70 && IsFootOutmatched(pHouse, pU, 7.0);
							const bool isIslandJob = isIslandExpansionMCV || isIslandHarvester || isIslandCombatVehicle;
							if (isCriticallyDamagedTank || isLosingExpensiveUnit || isLoadedStrikeAPC
								|| (isIslandJob && !islandLiftsPaused))
							{
								AdvanceRoutedPickup(pHouse, pTrans, pU, true);
								CountDecision(NeuralDecision::AirliftJob, pHouse->ArrayIndex);

								if (isCriticallyDamagedTank)
									CountDecision(NeuralDecision::AirliftRescue, pHouse->ArrayIndex);
								else if (isLosingExpensiveUnit)
									CountDecision(NeuralDecision::CombatEvac, pHouse->ArrayIndex);

								pickedCarryallTarget = true;
								break;
							}
						}

						// A carryall that is also a passenger ferry is parked by the ferry logic below.
						if (!pickedCarryallTarget && !IsAmphibiousOrAirTransport(pTrans->Type))
							ReturnIdleCarryallToBase(pHouse, pTrans);

						if (pickedCarryallTarget || !IsAmphibiousOrAirTransport(pTrans->Type))
							continue;
					}
					else if (pCarryExt->CarryallState != JumpjetCarryallState::Idle)
					{
						continue;
					}
				}
			}
		}

		if (!IsAmphibiousOrAirTransport(pTrans->Type))
			continue;

		const int numPass = pTrans->Passengers.NumPassengers;
		const int totalPassSize = pTrans->Passengers.GetTotalSize();

		// Damaged transports repair only when EMPTY (never with cargo: that delayed landings
		// forever) and are not recruited for ferry work until they are fixed.
		if (numPass == 0)
		{
			const double transHP = pTrans->GetHealthPercentage();
			const bool underRepair = IsTransportUnderRepair(pTrans);

			if (underRepair && transHP < 0.98)
				continue;

			if (!underRepair && transHP < 0.5)
			{
				if (auto const pStation = FindBestRepairStation(pHouse, pTrans))
				{
					ClearFerryTrip(pTrans->UniqueID);
					OrderFootToEnter(pTrans, pStation);
					continue;
				}
			}
		}

		bool carryingMCV = false;
		bool carryingStolenForeignMCV = false;

		for (FootClass* pPass = pTrans->Passengers.GetFirstPassenger(); pPass != nullptr;
			pPass = abstract_cast<FootClass*>(pPass->NextObject))
		{
			if (auto const pPassUnit = abstract_cast<UnitClass*>(pPass))
			{
				if (pPassUnit->Type && pPassUnit->Type->DeploysInto != nullptr)
				{
					carryingMCV = true; // MCV or Red Alert 3-style CABAL Expansion Nanocore!
					const unsigned int ownHouseBit
						= (pHouse->Type && pHouse->Type->ArrayIndex >= 0 && pHouse->Type->ArrayIndex < 32)
						? (1u << pHouse->Type->ArrayIndex)
						: 0u;
					if (pPassUnit->Type->DeploysInto->ConstructionYard
						&& pHouse->CountOwnedNow(pPassUnit->Type->DeploysInto) == 0 && pHouse->NumConYards >= 1
						&& (ownHouseBit == 0u || !pPassUnit->Type->DeploysInto->InOwners(ownHouseBit))
						&& !HouseExt::PrerequisitesMet(pHouse, pPassUnit->Type))
					{
						carryingStolenForeignMCV = true;
					}

					break;
				}
			}
		}

		const CellStruct transCoords = pTrans->GetMapCoords();
		const int transZone = MapClass::Instance.GetMovementZoneType(transCoords, MovementZone::Normal, pTrans->OnBridge);

		// Slot 82 (Island Extraction of Stolen Foreign MCV to Safe Home Island):
		// If this transport is carrying a stolen foreign-faction MCV from an
		// enemy island, sail straight back to our safe Home Island (homeCoords /
		// homeGroundZone) and unload it there so it can deploy safely!
		if (carryingStolenForeignMCV && homeCoords != CellStruct::Empty && SlotGate(scores[82], 60, 82))
		{
			if (CellClass* pHomeSafeLanding = FindSafeIslandLandingCell(pHouse, homeCoords, homeGroundZone, true))
			{
				auto const pTransCell = pTrans->GetCell();

				const bool onHomeIslandLand = (homeGroundZone >= 0 && transZone == homeGroundZone && pTransCell
					&& pTransCell->LandType != LandType::Water);

				if (onHomeIslandLand && pTrans->DistanceFrom(pHomeSafeLanding) <= 1024)
				{
					if (pTrans->CurrentMission != Mission::Unload)
						pTrans->QueueMission(Mission::Unload, true);
				}
				else if (pTrans->CurrentMission == Mission::Guard || pTrans->CurrentMission == Mission::Area_Guard
					|| (IsAITickWindow(90, 25)))
				{
					IssueMoveOrder(pTrans, pHomeSafeLanding, 4.0);
				}

				continue;
			}
		}

		// If we have a stolen foreign MCV stranded on an enemy island
		// (pStrandedStolenMCVOnIsland) and this transport is empty, dispatch it to
		// the island beach near the stolen MCV to pick it up!
		if (numPass == 0 && pStrandedStolenMCVOnIsland && SlotGate(scores[82], 60, 82))
		{
			const CellStruct mcvIslandCell = pStrandedStolenMCVOnIsland->GetMapCoords();

			const int mcvIslandZone
				= MapClass::Instance.GetMovementZoneType(mcvIslandCell, MovementZone::Normal, pStrandedStolenMCVOnIsland->OnBridge);

			if (CellClass* pRescueBeach = FindSafeIslandLandingCell(pHouse, mcvIslandCell, mcvIslandZone, false))
			{
				if (pTrans->DistanceFrom(pRescueBeach) > 1024
					&& (pTrans->CurrentMission == Mission::Guard || pTrans->CurrentMission == Mission::Area_Guard
						|| (IsAITickWindow(90, 25))))
				{
					IssueMoveOrder(pTrans, pRescueBeach, 4.0);
				}

				continue;
			}
		}

		// Ferry trip bookkeeping: the loading island is recorded while boarding on a
		// pickup shore, and the trip counts as departed once the loaded transport
		// reaches water or another landmass.
		auto const pTripCell = pTrans->GetCell();
		const bool tripOnWater = pTripCell && pTripCell->LandType == LandType::Water;
		NeuralFerryTripRecord* pTrip = nullptr;

		if (numPass == 0)
		{
			ClearFerryTrip(pTrans->UniqueID);
		}
		else
		{
			pTrip = FindFerryTrip(pTrans->UniqueID);

			if (!pTrip && tripOnWater)
			{
				// Loaded transport without a record (e.g. after loading a savegame):
				// it is already at sea, so treat it as in transit.
				pTrip = &AcquireFerryTrip(pTrans->UniqueID);
				pTrip->Departed = true;
				CountDecision(NeuralDecision::FerryDeparted);
			}

			if (pTrip)
			{
				pTrip->Frame = Unsorted::CurrentFrame;
				pTrip->HouseIndex = pHouse->ArrayIndex;
				pTrip->LastCoords = transCoords;

				if (!pTrip->Departed && pTrip->LoadZone >= 0
					&& (tripOnWater || (transZone >= 0 && transZone != pTrip->LoadZone)))
				{
					pTrip->Departed = true;
					CountDecision(NeuralDecision::FerryDeparted);
				}
			}
		}

		const bool tripDeparted = pTrip && pTrip->Departed;

		// The island the cargo comes from is never a valid destination. An empty
		// transport parked on land treats its current island as the origin as well.
		const int ferryOriginZone = (pTrip && pTrip->LoadZone >= 0)
			? pTrip->LoadZone
			: ((numPass == 0 && !tripOnWater) ? transZone : -1);

		auto isValidFerryGoalZone = [ferryOriginZone](int zone) -> bool
		{
			return zone >= 0 && zone != ferryOriginZone;
		};

		bool carryingEngineer = false;
		bool carryingHarvester = false;
		CellClass* pCustomPassengerGoal = nullptr;
		int customPassengerZone = -1;
		for (FootClass* pPass = pTrans->Passengers.GetFirstPassenger(); pPass != nullptr;
			pPass = abstract_cast<FootClass*>(pPass->NextObject))
		{
			if (!pCustomPassengerGoal)
			{
				const CellStruct passGoalCoords = LookupPassengerIslandGoal(pPass->UniqueID);
				if (passGoalCoords != CellStruct::Empty)
				{
					const int pgz = MapClass::Instance.GetMovementZoneType(passGoalCoords, MovementZone::Normal, false);
					if (pgz >= 0 && !isValidFerryGoalZone(pgz))
					{
						// Stale goal on the loading island: it must never pull the
						// transport back or make it unload where the cargo boarded.
						ClearPassengerIslandGoal(pPass->UniqueID);
					}
					else if (pgz >= 0)
					{
						if (CellClass* pSafePassLanding = FindSafeIslandLandingCell(pHouse, passGoalCoords, pgz, false))
						{
							pCustomPassengerGoal = pSafePassLanding;
							customPassengerZone = pgz;
						}
					}
				}
			}

			if (auto const pPassInf = abstract_cast<InfantryClass*>(pPass))
			{
				if (pPassInf->Type && pPassInf->Type->Engineer)
					carryingEngineer = true;
			}
			else if (auto const pPassUnit = abstract_cast<UnitClass*>(pPass))
			{
				if (pPassUnit->Type && (pPassUnit->Type->Harvester || pPassUnit->Type->Weeder))
					carryingHarvester = true;
			}
		}

		CellClass* pActiveIslandGoal = pCustomPassengerGoal ? pCustomPassengerGoal : pIslandGoalCell;
		int activeIslandZone = pCustomPassengerGoal ? customPassengerZone : targetIslandZone;
		// Parallel Amphibious Task Groups: when multiple combat transports are
		// ready and a secondary enemy island/flank exists, assign odd-numbered
		// transports to strike the secondary objective in parallel!
		if (!pCustomPassengerGoal && !carryingMCV && !carryingHarvester && !carryingEngineer && pSecondaryIslandGoalCell
			&& secondaryIslandZone >= 0 && readyCombatConvoyTransports >= 2 && (pTrans->UniqueID % 2 == 1))
		{
			pActiveIslandGoal = pSecondaryIslandGoalCell;
			activeIslandZone = secondaryIslandZone;
		}

		if (!pCustomPassengerGoal && (carryingMCV || carryingHarvester) && pIslandResourceGoalCell)
		{
			pActiveIslandGoal = pIslandResourceGoalCell;
			activeIslandZone = resourceIslandZone;
		}
		else if (carryingEngineer && pIslandTechGoalCell)
		{
			pActiveIslandGoal = pIslandTechGoalCell;
			activeIslandZone = techIslandZone;
		}
		else if (!pActiveIslandGoal)
		{
			if (pIslandResourceGoalCell)
			{
				pActiveIslandGoal = pIslandResourceGoalCell;
				activeIslandZone = resourceIslandZone;
			}
			else if (pIslandTechGoalCell)
			{
				pActiveIslandGoal = pIslandTechGoalCell;
				activeIslandZone = techIslandZone;
			}
		}

		// A goal on the origin island is no destination at all: fall back to the
		// next objective located on another landmass, or to none.
		if (pActiveIslandGoal && !isValidFerryGoalZone(activeIslandZone))
		{
			pActiveIslandGoal = nullptr;
			activeIslandZone = -1;

			const std::pair<CellClass*, int> fallbackGoals[] = {
				{ pIslandGoalCell, targetIslandZone },
				{ pSecondaryIslandGoalCell, secondaryIslandZone },
				{ pIslandResourceGoalCell, resourceIslandZone },
				{ pIslandTechGoalCell, techIslandZone }
			};

			for (auto const& [pGoalCell, goalZone] : fallbackGoals)
			{
				if (pGoalCell && isValidFerryGoalZone(goalZone))
				{
					pActiveIslandGoal = pGoalCell;
					activeIslandZone = goalZone;
					break;
				}
			}
		}

		// If this transport already has passengers (numPass >= 1) and is
		// intercepted/under attack during boarding, depart immediately with what we
		// have!
		const bool emergencyDepartUnderFire
			= (numPass >= 1 && (pTrans->HasBeenAttacked || pTrans->GetHealthPercentage() < 0.70));

		auto hasEnemyPresenceInIslandZone = [&](int checkZone) -> bool
		{
			if (checkZone < 0)
				return false;

			for (auto const pEB : BuildingClass::Array)
			{
				if (!pEB || pEB->InLimbo || !pEB->IsAlive || !pEB->Owner || !pEB->Type || pEB->Type->Insignificant
					|| pEB->Type->InvisibleInGame || pEB->Type->Naval)
				{
					continue;
				}

				if (pEB->Owner == pHouse || pHouse->IsAlliedWith(pEB->Owner) || !IsEnemyBuildingKnown(pHouse, pEB) || pEB->Owner->IsNeutral()
					|| pEB->Owner->IsObserver() || pEB->Owner->Defeated)
				{
					continue;
				}

				if (ObjectZone(pEB, MovementZone::Normal)
					== checkZone)
				{
					return true;
				}
			}

			for (auto const pEU : UnitClass::Array)
			{
				if (!pEU || pEU->InLimbo || !pEU->IsAlive || !pEU->Owner || !pEU->Type || pEU->Type->Naval)
					continue;

				if (pEU->Owner == pHouse || pHouse->IsAlliedWith(pEU->Owner) || !IsCellVisibleToAI(pHouse, pEU->GetCell()) || pEU->Owner->IsNeutral()
					|| pEU->Owner->IsObserver() || pEU->Owner->Defeated)
				{
					continue;
				}

				if (ObjectZone(pEU, MovementZone::Normal)
					== checkZone)
				{
					return true;
				}
			}

			return false;
		};

		auto isSafeFriendlyPickupIsland = [&](int checkZone) -> bool
		{
			if (checkZone < 0)
				return false;

			if ((activeIslandZone >= 0 && checkZone == activeIslandZone)
				|| (targetIslandZone >= 0 && checkZone == targetIslandZone && checkZone != homeGroundZone)
				|| (secondaryIslandZone >= 0 && checkZone == secondaryIslandZone && checkZone != homeGroundZone))
			{
				return false;
			}

			// Any non-home island that still has enemy buildings or units is an active
			// invasion/combat zone: transports arriving there must UNLOAD, never wait for pickup!
			if (checkZone != homeGroundZone && hasEnemyPresenceInIslandZone(checkZone))
				return false;

			if (checkZone == homeGroundZone)
				return true;

			for (auto const pOwnB : pHouse->Buildings)
			{
				if (!pOwnB || pOwnB->InLimbo || !pOwnB->IsAlive || !pOwnB->Type)
					continue;

				if (pOwnB->Type->ConstructionYard
					|| (pOwnB->Type->Factory != AbstractType::None && !pOwnB->Type->Naval))
				{
					if (ObjectZone(pOwnB, MovementZone::Normal)
						== checkZone)
					{
						return true;
					}
				}
			}

			// Also allow picking up stranded friendly units from cleared/neutral islands
			// that have no enemy presence so they can rejoin the next offensive wave
			return true;
		};

		auto hasMoreBoardableUnitsOnIsland = [&](int checkZone, int reservedDeployerCap = 0) -> bool
		{
			if (checkZone < 0 || !isSafeFriendlyPickupIsland(checkZone))
				return false;

			const int maxAllowedCap = std::max(0, pTrans->Type->Passengers - reservedDeployerCap);
			const int freeCap = maxAllowedCap - totalPassSize;
			if (freeCap <= 0 || numPass >= pTrans->Type->Passengers)
				return false;

			auto isAlreadyBoardingAnotherTransport = [&](FootClass* pCand) -> bool
			{
				if (!pCand)
					return false;

				return (pCand->CurrentMission == Mission::Enter || pCand->QueuedMission == Mission::Enter)
					&& !pCand->ContainsLink(pTrans) && pCand->ArchiveTarget != pTrans && pCand->Destination != pTrans;
			};

			for (auto const& ord : pendingCrossIslandOrders)
			{
				if (!ord.Passenger || ord.Passenger->InLimbo || !ord.Passenger->IsAlive || ord.PickupZone != checkZone)
					continue;

				if (isAlreadyBoardingAnotherTransport(ord.Passenger))
					continue;

				const int osz = std::max(1, ord.PassengerSize);
				if (osz <= static_cast<int>(pTrans->Type->SizeLimit) && osz <= freeCap)
					return true;
			}

			for (auto const pV : idleCombatUnits)
			{
				if (!pV || pV->InLimbo || !pV->IsAlive || IsAmphibiousOrAirTransport(pV->Type))
					continue;

				if (isAlreadyBoardingAnotherTransport(pV))
					continue;

				if (pV->CaptureManager && pV->CaptureManager->NumControlNodes() > 0)
					continue;

				const int vz = ObjectZone(pV, MovementZone::Normal);
				if (vz != checkZone || IsFootActivelyAttackingOrMovingToEnemy(pV) || IsFootNearEnemyFrontline(pV, vz))
					continue;

				const int vsz = std::max(1, static_cast<int>(pV->Type->TechnoTypeClass::Size));
				if (vsz <= static_cast<int>(pTrans->Type->SizeLimit) && vsz <= freeCap)
					return true;
			}

			for (auto const pI : idleCombatInf)
			{
				if (!pI || pI->InLimbo || !pI->IsAlive)
					continue;

				if (isAlreadyBoardingAnotherTransport(pI))
					continue;

				const int iz = ObjectZone(pI, MovementZone::Normal);
				if (iz != checkZone || IsFootActivelyAttackingOrMovingToEnemy(pI) || IsFootNearEnemyFrontline(pI, iz))
					continue;

				const int isz = std::max(1, static_cast<int>(pI->Type->TechnoTypeClass::Size));
				if (isz <= static_cast<int>(pTrans->Type->SizeLimit) && isz <= freeCap)
					return true;
			}

			return false;
		};

		auto const pCurTransCell = pTrans->GetCell();
		// A loaded transport that already left its loading island is in transit:
		// brushing a friendly coast on the way never turns it back into a pickup.
		const bool onSafePickupShore = !tripDeparted && isSafeFriendlyPickupIsland(transZone) && pCurTransCell
			&& pCurTransCell->LandType != LandType::Water;

		// Enforce strict 1-by-1 boarding while parked on a pickup island so units never jam!
		FootClass* pSingleActiveBoarder = onSafePickupShore ? enforceSingleActiveBoarderOn(pTrans, transZone) : nullptr;
		const bool canFitMoreOnPickupIsland = onSafePickupShore
			&& (pSingleActiveBoarder != nullptr || hasMoreBoardableUnitsOnIsland(transZone, 0));

		if (numPass == 0 && onSafePickupShore && pSingleActiveBoarder == nullptr
			&& (pTrans->CurrentMission == Mission::Guard || pTrans->CurrentMission == Mission::Area_Guard)
			&& std::any_of(pendingCrossIslandOrders.begin(), pendingCrossIslandOrders.end(),
				[transZone](const PendingCrossIslandOrder& ord) { return ord.PickupZone == transZone; }))
		{
			CountDecision(NeuralDecision::FerryIdleWithDemand, pHouse->ArrayIndex);
		}

		const bool hasCargoInTransitOrUnloading = (numPass >= 1) && !onSafePickupShore
			&& (tripDeparted || (activeIslandZone >= 0 && transZone == activeIslandZone)
				|| pTrans->CurrentMission == Mission::Unload
				|| (transZone >= 0 && !isSafeFriendlyPickupIsland(transZone))
				|| (pCurTransCell && pCurTransCell->LandType == LandType::Water));

		// Depart ONLY when:
		// 1) Carrying an MCV / Harvester / Engineer for a specific island objective, OR
		// 2) Under enemy attack during boarding (emergencyDepartUnderFire), OR
		// 3) Already in transit / unloading at the target island, OR
		// 4) Parked on a pickup island with >= 1 passenger and NO MORE available units on the island can fit!
		// Convoy: a combat transport ready to sail waits (up to 600 frames) while sister
		// transports on the same pickup island are still loading, so the invasion lands as
		// one wave instead of 3-4 units at a time.
		bool convoyHold = false;

		if (numPass >= 1 && onSafePickupShore && !canFitMoreOnPickupIsland && enemyOnDisconnectedIsland && pTrip
			&& !carryingMCV && !carryingStolenForeignMCV && !carryingHarvester && !carryingEngineer
			&& !emergencyDepartUnderFire)
		{
			if (pTrip->ReadyFrame < 0)
				pTrip->ReadyFrame = Unsorted::CurrentFrame;

			if (Unsorted::CurrentFrame - pTrip->ReadyFrame < 600)
			{
				for (auto const pSister : UnitClass::Array)
				{
					if (!pSister || pSister == pTrans || pSister->Owner != pHouse || !pSister->IsAlive || pSister->InLimbo
						|| !pSister->Type || !IsAmphibiousOrAirTransport(pSister->Type) || pSister->Passengers.NumPassengers <= 0
						|| pSister->DistanceFrom(pTrans) > 20 * Unsorted::LeptonsPerCell
						|| ObjectZone(pSister, MovementZone::Normal) != transZone)
					{
						continue;
					}

					auto const pSisterTrip = FindFerryTrip(pSister->UniqueID);

					// Still loading: not departed and not ready yet.
					if (pSisterTrip && !pSisterTrip->Departed && pSisterTrip->ReadyFrame < 0)
					{
						convoyHold = true;
						break;
					}
				}
			}
		}

		const bool isReadyToSailEffective = carryingMCV || carryingStolenForeignMCV || carryingHarvester
			|| (carryingEngineer && pIslandTechGoalCell != nullptr)
			|| emergencyDepartUnderFire || hasCargoInTransitOrUnloading
			|| (numPass >= 1 && onSafePickupShore && !canFitMoreOnPickupIsland && !convoyHold);

		// Prevent AttackMove.Follow=true (e.g. on GDI APC/APCW) from hijacking transports into escorting warships!
		pTrans->ClearMegaMissionData();

		// The cargo's objective vanished while at sea and no other landmass is a
		// valid destination: bring the cargo back to its loading island and drop
		// it there instead of drifting loaded forever.
		if (isReadyToSailEffective && !pActiveIslandGoal && numPass >= 1 && tripDeparted)
		{
			const int returnZone = (ferryOriginZone >= 0) ? ferryOriginZone : homeGroundZone;
			const bool onReturnIslandLand = pCurTransCell && pCurTransCell->LandType != LandType::Water
				&& !pCurTransCell->GetBuilding() && returnZone >= 0 && transZone == returnZone;

			if (onReturnIslandLand)
			{
				if (pTrans->CurrentMission != Mission::Unload)
					CountDecision(NeuralDecision::FerryCargoReturned);

				pTrans->SetDestination(nullptr, true);
				pTrans->ClearNavigationList();

				if (pTrans->CurrentMission != Mission::Unload)
					pTrans->QueueMission(Mission::Unload, true);
			}
			else if (pTrans->CurrentMission == Mission::Guard || pTrans->CurrentMission == Mission::Area_Guard
				|| (IsAITickWindow(60, 20)))
			{
				const CellStruct returnAnchor = (returnZone == homeGroundZone && homeCoords != CellStruct::Empty)
					? homeCoords
					: transCoords;
				CellClass* pReturnBeach = FindSafeIslandLandingCell(pHouse, returnAnchor, returnZone, false);

				if (!pReturnBeach && homeCoords != CellStruct::Empty)
					pReturnBeach = FindSafeIslandLandingCell(pHouse, homeCoords, homeGroundZone, false);

				if (pReturnBeach)
				{
					IssueMoveOrder(pTrans, pReturnBeach, 4.0);
				}
			}

			continue;
		}

		if (isReadyToSailEffective && pActiveIslandGoal)
		{
			CellClass* pSafeLanding = FindSafeIslandLandingCell(
				pHouse, pActiveIslandGoal->MapCoords, activeIslandZone, carryingMCV || carryingHarvester);
			if (pSafeLanding)
			{
				auto const pTransCell = pTrans->GetCell();
				const double distToLanding = transCoords.DistanceFrom(pSafeLanding->MapCoords);
				// Unload strictly on the destination island: never on the first coast
				// touched on the way, an intermediate island or the loading island.
				bool onTargetIslandLand = pTransCell && pTransCell->LandType != LandType::Water
					&& !pTransCell->GetBuilding() && isValidFerryGoalZone(activeIslandZone)
					&& transZone == activeIslandZone;

				// Progress watchdog near the landing zone (handles blocked beaches, unload
				// failures and any order the transport silently dropped).
				if (pTrip && tripDeparted && distToLanding <= 14.0)
				{
					// Progress = moving, or passengers actually leaving during an unload.
					if (pTrip->ProgressCoords == CellStruct::Empty || transCoords.DistanceFrom(pTrip->ProgressCoords) > 2.0
						|| numPass != pTrip->ProgressPassengers)
					{
						pTrip->ProgressCoords = transCoords;
						pTrip->ProgressFrame = Unsorted::CurrentFrame;
						pTrip->ProgressPassengers = numPass;
					}
					else if (Unsorted::CurrentFrame - pTrip->ProgressFrame >= 450)
					{
						pTrip->ProgressFrame = Unsorted::CurrentFrame;
						pTrip->StuckStage++;

						// A failing unload (blocked exits) goes back to relocating.
						if (pTrans->CurrentMission == Mission::Unload && pTrip->StuckStage >= 2)
							pTrip->StuckStage = 1;

						if (pTrip->StuckStage == 1)
						{
							// Stage 1: try another free beach cell of the destination island.
							CellClass* pAltBeach = nullptr;
							int bestAlt = INT_MAX;

							for (int dy = -8; dy <= 8; ++dy)
							{
								for (int dx = -8; dx <= 8; ++dx)
								{
									const CellStruct bc { static_cast<short>(transCoords.X + dx), static_cast<short>(transCoords.Y + dy) };
									auto const pBC = MapClass::Instance.TryGetCellAt(bc);

									if (!pBC || pBC->LandType == LandType::Water || pBC->LandType == LandType::Rock
										|| pBC->GetBuilding() || pBC->FirstObject
										|| MapClass::Instance.GetMovementZoneType(bc, MovementZone::Normal, false) != activeIslandZone
										|| !pBC->IsClearToMove(pTrans->Type->SpeedType, true, true, -1, pTrans->Type->MovementZone, -1, false))
									{
										continue;
									}

									const int d = std::abs(dx) + std::abs(dy);

									if (d >= 2 && d < bestAlt)
									{
										bestAlt = d;
										pAltBeach = pBC;
									}
								}
							}

							if (pAltBeach)
							{
								pTrans->SetDestination(pAltBeach, true);
								pTrans->QueueMission(Mission::Move, true);
								continue;
							}
						}
						else if (pTransCell && pTransCell->LandType != LandType::Water && isValidFerryGoalZone(transZone))
						{
							// Stage 2+: unload right here; a stuck full transport helps nobody.
							onTargetIslandLand = true;
						}
					}
				}

				if (onTargetIslandLand)
				{
					// Scatter any idle friendly units standing on the 8 adjacent cells
					// around pTrans so they never block the unload ramp, and count how
					// many adjacent dry land cells are actually empty right now!
					int emptyAdjUnloadCells = 0;
					for (int ndy = -1; ndy <= 1; ++ndy)
					{
						for (int ndx = -1; ndx <= 1; ++ndx)
						{
							if (ndx == 0 && ndy == 0)
								continue;

							const CellStruct adj { static_cast<short>(transCoords.X + ndx),
								static_cast<short>(transCoords.Y + ndy) };
							auto const pAdj = MapClass::Instance.TryGetCellAt(adj);
							if (!pAdj || pAdj->LandType == LandType::Water || pAdj->GetBuilding() != nullptr
								|| !pAdj->IsClearToMove(
									SpeedType::Track, true, true, -1, MovementZone::Normal, -1, false))
							{
								continue;
							}

							if (pAdj->FirstObject == nullptr)
							{
								emptyAdjUnloadCells++;
							}
							else
							{
								std::vector<FootClass*> adjFoots;
								CollectCellFoots(pAdj, adjFoots);

								for (auto const pFootAdj : adjFoots)
								{
									if (pFootAdj->Owner == pHouse && pFootAdj->IsAlive && !pFootAdj->InLimbo
										&& pFootAdj->CurrentMission != Mission::Move
										&& pFootAdj->CurrentMission != Mission::Attack)
									{
										if (auto const pUAdj = abstract_cast<UnitClass*>(pFootAdj))
										{
											if (IsAmphibiousOrAirTransport(pUAdj->Type))
												continue;
										}

										pFootAdj->Scatter(CoordStruct::Empty, true, false);
									}
								}
							}
						}
					}

					// If our current beach spot has < 2 empty exit cells (or if
					// Mission::Unload failed and reverted to Guard while congested),
					// relocate to a nearby unobstructed beach/land cell with >= 3 empty
					// dry neighbor cells so remaining passengers unload cleanly!
					if (emptyAdjUnloadCells < 2)
					{
						CellClass* pBetterUnloadCell = nullptr;
						int bestAltScore = -999999;
						for (int dy = -12; dy <= 12; ++dy)
						{
							for (int dx = -12; dx <= 12; ++dx)
							{
								if (std::abs(dx) <= 1 && std::abs(dy) <= 1)
									continue;

								const CellStruct altC { static_cast<short>(transCoords.X + dx),
									static_cast<short>(transCoords.Y + dy) };
								auto const pAlt = MapClass::Instance.TryGetCellAt(altC);
								if (!pAlt || pAlt->LandType == LandType::Water || pAlt->LandType == LandType::Rock
									|| pAlt->GetBuilding() || pAlt->FirstObject != nullptr)
								{
									continue;
								}

								if (activeIslandZone >= 0
									&& MapClass::Instance.GetMovementZoneType(altC, MovementZone::Normal, false)
										!= activeIslandZone)
								{
									continue;
								}

								if (!pAlt->IsClearToMove(
										SpeedType::Track, false, false, -1, MovementZone::Normal, -1, false))
								{
									continue;
								}

								int altEmptyNeighbors = 0;
								for (int ay = -1; ay <= 1; ++ay)
								{
									for (int ax = -1; ax <= 1; ++ax)
									{
										if (ax == 0 && ay == 0)
											continue;

										const CellStruct nC { static_cast<short>(altC.X + ax),
											static_cast<short>(altC.Y + ay) };
										auto const pN = MapClass::Instance.TryGetCellAt(nC);
										if (pN && pN->LandType != LandType::Water && !pN->GetBuilding()
											&& pN->FirstObject == nullptr
											&& pN->IsClearToMove(
												SpeedType::Track, false, false, -1, MovementZone::Normal, -1, false))
										{
											altEmptyNeighbors++;
										}
									}
								}

								if (altEmptyNeighbors >= 3)
								{
									const int sc = altEmptyNeighbors * 80 - (std::abs(dx) + std::abs(dy)) * 10;
									if (sc > bestAltScore)
									{
										bestAltScore = sc;
										pBetterUnloadCell = pAlt;
									}
								}
							}
						}

						if (pBetterUnloadCell)
						{
							IssueMoveOrder(pTrans, pBetterUnloadCell, 1.5);
							continue;
						}
					}

					pTrans->SetDestination(nullptr, true);
					pTrans->ClearNavigationList();
					if (pTrans->CurrentMission != Mission::Unload)
					{
						pTrans->QueueMission(Mission::Unload, true);
						CountDecision(NeuralDecision::FerryUnloadAtGoal);
					}
				}
				else if (pTransCell && pTransCell->LandType == LandType::Water && distToLanding <= 10.0
					&& (pTrans->CurrentMission == Mission::Guard || pTrans->CurrentMission == Mission::Area_Guard))
				{
					// If the transport stopped in water near the shore because pSafeLanding became occupied,
					// find the closest unoccupied dry beach cell on activeIslandZone within 10 cells and beach immediately!
					CellClass* pNearBeach = nullptr;
					int bestNearScore = -999999;
					for (int dy = -10; dy <= 10; ++dy)
					{
						for (int dx = -10; dx <= 10; ++dx)
						{
							const CellStruct bc { static_cast<short>(transCoords.X + dx),
								static_cast<short>(transCoords.Y + dy) };
							auto const pBC = MapClass::Instance.TryGetCellAt(bc);
							if (!pBC || pBC->LandType == LandType::Water || pBC->LandType == LandType::Rock
								|| pBC->GetBuilding() || pBC->FirstObject != nullptr)
							{
								continue;
							}

							if (activeIslandZone >= 0
								&& MapClass::Instance.GetMovementZoneType(bc, MovementZone::Normal, false)
									!= activeIslandZone)
							{
								continue;
							}

							if (!pBC->IsClearToMove(SpeedType::Track, false, false, -1, MovementZone::Normal, -1, false)
								|| !pBC->IsClearToMove(
									pTrans->Type->SpeedType, true, true, -1, pTrans->Type->MovementZone, -1, false))
							{
								continue;
							}

							const int sc = 500 - (std::abs(dx) + std::abs(dy)) * 15;
							if (sc > bestNearScore)
							{
								bestNearScore = sc;
								pNearBeach = pBC;
							}
						}
					}

					IssueMoveOrder(pTrans, pNearBeach ? pNearBeach : pSafeLanding, 1.5);
				}
				else if (pTrans->CurrentMission == Mission::Guard || pTrans->CurrentMission == Mission::Area_Guard
					|| (pTrans->CurrentMission != Mission::Unload && (IsAITickWindow(60, 20))))
				{
					// Sail around enemy bases and defenses through a safe waypoint
					// unless crossing their range is unavoidable.
					CellClass* pNextHop = pSafeLanding;

					if (pTrip && distToLanding > 12.0)
					{
						const bool landingMoved = pTrip->RouteTarget == CellStruct::Empty
							|| pTrip->RouteTarget.DistanceFrom(pSafeLanding->MapCoords) > 6.0;

						if (landingMoved || (Unsorted::CurrentFrame - pTrip->RouteFrame) > 450)
						{
							pTrip->RouteTarget = pSafeLanding->MapCoords;
							pTrip->RouteFrame = Unsorted::CurrentFrame;
							pTrip->RouteWaypoint
								= FindSafeFerryWaypoint(pHouse, transCoords, pSafeLanding->MapCoords, pTrans->Type);

							if (pTrip->RouteWaypoint != CellStruct::Empty)
								CountDecision(NeuralDecision::SafeLaneDetour);
						}

						if (pTrip->RouteWaypoint != CellStruct::Empty)
						{
							if (transCoords.DistanceFrom(pTrip->RouteWaypoint) <= 4.0)
								pTrip->RouteWaypoint = CellStruct::Empty;
							else if (auto const pWaypointCell = MapClass::Instance.TryGetCellAt(pTrip->RouteWaypoint))
								pNextHop = pWaypointCell;
						}
					}

					IssueMoveOrder(pTrans, pNextHop, 3.0);
				}
			}
		}
		else if (!isReadyToSailEffective)
		{
			auto const pTransCell = pTrans->GetCell();

			// No ferry demand at all (e.g. a land map): an empty transport parks instead of
			// chasing the nearest idle unit's coast back and forth between water and land.
			if (numPass == 0 && !hasAnyFerryDemand)
			{
				if (pTransCell && pTransCell->LandType != LandType::Water)
				{
					if ((pTrans->CurrentMission != Mission::Guard && pTrans->CurrentMission != Mission::Area_Guard)
						|| pTrans->Destination != nullptr)
					{
						pTrans->SetDestination(nullptr, true);
						pTrans->ClearNavigationList();
						pTrans->ForceMission(Mission::Guard);
					}
				}
				else if (homeCoords != CellStruct::Empty)
				{
					if (auto const pHomeBeach = FindSafeIslandLandingCell(pHouse, homeCoords, homeGroundZone, false))
						IssueMoveOrder(pTrans, pHomeBeach, 6.0);
				}

				continue;
			}

			// Loading on a pickup shore: this island is the origin of the next trip.
			if (onSafePickupShore && transZone >= 0)
			{
				auto& trip = AcquireFerryTrip(pTrans->UniqueID);
				trip.LoadZone = transZone;
				trip.Departed = false;
				trip.Frame = Unsorted::CurrentFrame;
			}

			// Reserve at most ONE transport for pHomeExpansionDeployer, and if we
			// already own >= 2 ConYards while >= 5 combat vehicles are waiting to
			// invade an enemy island, let combat invasion waves take priority when no
			// second transport is free!
			// A transport already holding cargo is only reserved when the deployer
			// still fits and waits on this same island: boarded passengers are never
			// unloaded again to make room for it.
			const bool prioritizeCombatInvasionNow = (pHouse->NumConYards >= 2 && enemyOnDisconnectedIsland
				&& idleCombatUnits.size() >= 5 && (IsAITickWindow(240, 150)));
			const int deployerZone = pHomeExpansionDeployer
				? ObjectZone(pHomeExpansionDeployer, MovementZone::Normal)
				: -1;
			const bool canCarryHomeDeployer = (!deployerTransportAssigned && !prioritizeCombatInvasionNow
				&& pHomeExpansionDeployer != nullptr && !carryingMCV
				&& static_cast<int>(pHomeExpansionDeployer->Type->TechnoTypeClass::Size)
					<= static_cast<int>(pTrans->Type->SizeLimit)
				&& (totalPassSize + static_cast<int>(pHomeExpansionDeployer->Type->TechnoTypeClass::Size))
					<= pTrans->Type->Passengers
				&& (numPass == 0 || transZone == deployerZone));
			const int reservedSizeForDeployer
				= canCarryHomeDeployer ? static_cast<int>(pHomeExpansionDeployer->Type->TechnoTypeClass::Size) : 0;

			if (canCarryHomeDeployer)
			{
				deployerTransportAssigned = true;
				CellClass* pDeployerPickupBeach
					= FindSafeIslandLandingCell(pHouse, pHomeExpansionDeployer->GetMapCoords(), deployerZone, false);
				const bool onDeployerLand
					= (transZone == deployerZone && pTransCell && pTransCell->LandType != LandType::Water);
				if (!onDeployerLand && pDeployerPickupBeach)
				{
					if (pSingleActiveBoarder == nullptr
						&& (pTrans->CurrentMission == Mission::Guard || pTrans->CurrentMission == Mission::Area_Guard
							|| (IsAITickWindow(60, 20))))
					{
						IssueMoveOrder(pTrans, pDeployerPickupBeach, 4.0);
					}

					continue;
				}

				if (onDeployerLand)
				{
					// Stay 100% still on the land spot where the transport is parked!
					if (pTrans->CurrentMission != Mission::Guard || pTrans->Destination != nullptr)
					{
						pTrans->SetDestination(nullptr, true);
						pTrans->ClearNavigationList();
						pTrans->ForceMission(Mission::Guard);
					}

					if (pSingleActiveBoarder == nullptr || pSingleActiveBoarder == pHomeExpansionDeployer)
						OrderFootToEnter(pHomeExpansionDeployer, pTrans);

					continue;
				}
			}
			else if (pHomeIslandEngineer != nullptr && !carryingEngineer
				&& CanPassengerFitInTransport(pTrans, pHomeIslandEngineer->Type))
			{
				const int engZone = ObjectZone(pHomeIslandEngineer, MovementZone::Normal);
				CellClass* pEngPickupBeach
					= FindSafeIslandLandingCell(pHouse, pHomeIslandEngineer->GetMapCoords(), engZone, false);
				const bool onEngLand = (transZone == engZone && pTransCell && pTransCell->LandType != LandType::Water);
				if (!onEngLand && pEngPickupBeach)
				{
					if (pSingleActiveBoarder == nullptr
						&& (pTrans->CurrentMission == Mission::Guard || pTrans->CurrentMission == Mission::Area_Guard
							|| (IsAITickWindow(60, 20))))
					{
						IssueMoveOrder(pTrans, pEngPickupBeach, 4.0);
					}

					continue;
				}

				if (onEngLand)
				{
					if (pTrans->CurrentMission != Mission::Guard || pTrans->Destination != nullptr)
					{
						pTrans->SetDestination(nullptr, true);
						pTrans->ClearNavigationList();
						pTrans->ForceMission(Mission::Guard);
					}

					if (pSingleActiveBoarder == nullptr || pSingleActiveBoarder == pHomeIslandEngineer)
					{
						OrderFootToEnter(pHomeIslandEngineer, pTrans);
						pSingleActiveBoarder = pHomeIslandEngineer;
					}
				}
			}

			// Safe Friendly Base & Stranded Cleared-Island Pickup + Empty Transport Retreat:
			int desiredPickupZone = -1;
			CellStruct desiredPickupAnchor = homeCoords;
			{
				struct IslandArmyCluster
				{
					int zone;
					int score;
					int minDistToTrans;
					CellStruct closestUnitCoords;
				};
				std::vector<IslandArmyCluster> clusters;
				auto addCandidateToCluster = [&](FootClass* pCand, int candSize, int weight)
				{
					if (!pCand || pCand->InLimbo || !pCand->IsAlive)
						return;

					if (candSize > static_cast<int>(pTrans->Type->SizeLimit))
						return;

					if ((pCand->CurrentMission == Mission::Enter || pCand->QueuedMission == Mission::Enter)
						&& !pCand->ContainsLink(pTrans) && pCand->ArchiveTarget != pTrans
						&& pCand->Destination != pTrans)
					{
						return; // Already boarding a different transport
					}

					const CellStruct cc = pCand->GetMapCoords();
					const int cz = MapClass::Instance.GetMovementZoneType(cc, MovementZone::Normal, pCand->OnBridge);
					if (!isSafeFriendlyPickupIsland(cz))
						return;

					const int d = std::abs(cc.X - transCoords.X) + std::abs(cc.Y - transCoords.Y);
					for (auto& cl : clusters)
					{
						if (cl.zone == cz)
						{
							cl.score += weight;
							if (d < cl.minDistToTrans)
							{
								cl.minDistToTrans = d;
								cl.closestUnitCoords = cc;
							}

							return;
						}
					}

					clusters.push_back({ cz, weight, d, cc });
				};

				for (auto const& ord : pendingCrossIslandOrders)
				{
					if (ord.Passenger && ord.Passenger->IsAlive && !ord.Passenger->InLimbo)
						addCandidateToCluster(ord.Passenger, std::max(1, ord.PassengerSize), 45);
				}

				for (auto const pV : idleCombatUnits)
				{
					if (!IsAmphibiousOrAirTransport(pV->Type))
						addCandidateToCluster(pV, static_cast<int>(pV->Type->TechnoTypeClass::Size), 35);
				}

				for (auto const pI : idleCombatInf)
				{
					addCandidateToCluster(pI, std::max(1, static_cast<int>(pI->Type->TechnoTypeClass::Size)), 12);
				}

				std::vector<IslandArmyCluster> readyIslands;
				for (const auto& cl : clusters)
				{
					if (cl.zone == transZone)
					{
						desiredPickupZone = cl.zone;
						desiredPickupAnchor = cl.closestUnitCoords;
						break;
					}

					if (cl.score >= 35 || (cl.zone != homeGroundZone && cl.score >= 24))
						readyIslands.push_back(cl);
				}

				if (desiredPickupZone < 0 && !readyIslands.empty())
				{
					const auto& chosenCl = readyIslands[pTrans->UniqueID % readyIslands.size()];
					desiredPickupZone = chosenCl.zone;
					desiredPickupAnchor = chosenCl.closestUnitCoords;
				}

				if (desiredPickupZone < 0)
				{
					int bestClusterVal = -999999;
					for (const auto& cl : clusters)
					{
						int val = cl.score - (cl.minDistToTrans * 2);
						if (val > bestClusterVal)
						{
							bestClusterVal = val;
							desiredPickupZone = cl.zone;
							desiredPickupAnchor = cl.closestUnitCoords;
						}
					}
				}

				if (desiredPickupZone < 0)
				{
					desiredPickupZone = homeGroundZone;
					desiredPickupAnchor = homeCoords;
					// Only anchor Naval transports (e.g. Hover Transport / Landing Craft) near our Naval Yard;
					// non-naval amphibious APCs built at the War Factory stay anchored near the War Factory / homeCoords!
					if (pTrans->Type->Naval)
					{
						for (auto const pOwnB : pHouse->Buildings)
						{
							if (!pOwnB || pOwnB->InLimbo || !pOwnB->IsAlive || !pOwnB->Type)
								continue;

							if (pOwnB->Type->Naval && pOwnB->Type->Factory == AbstractType::UnitType)
							{
								desiredPickupAnchor = pOwnB->GetMapCoords();
								break;
							}
						}
					}
				}
			}

			const bool notAtPickupShore = (pTransCell && pTransCell->LandType == LandType::Water)
				|| (desiredPickupZone >= 0 && transZone != desiredPickupZone)
				|| (!isSafeFriendlyPickupIsland(transZone));
			if (notAtPickupShore && desiredPickupAnchor != CellStruct::Empty)
			{
				CellClass* pPickupBeach
					= FindSafeIslandLandingCell(pHouse, desiredPickupAnchor, desiredPickupZone, false);
				if (!pPickupBeach && homeCoords != CellStruct::Empty)
					pPickupBeach = FindSafeIslandLandingCell(pHouse, homeCoords, homeGroundZone, false);

				// The waiting passengers stand on and around the pickup beach cell, so a transport
				// that cannot enter it lands on the nearest free shore cell instead (land, same zone,
				// next to water, enterable by this transport, within 8 cells).
				if (pPickupBeach
					&& !pPickupBeach->IsClearToMove(pTrans->Type->SpeedType, true, true, -1, pTrans->Type->MovementZone, -1, false))
				{
					const CellStruct beach = pPickupBeach->MapCoords;
					const int beachZone = MapClass::Instance.GetMovementZoneType(beach, MovementZone::Normal, false);
					CellClass* pFreeShore = nullptr;

					for (int r = 1; r <= 8 && !pFreeShore; ++r)
					{
						int bestD = INT_MAX;

						for (int dy = -r; dy <= r; ++dy)
						{
							for (int dx = -r; dx <= r; ++dx)
							{
								if (std::abs(dx) != r && std::abs(dy) != r)
									continue;

								const CellStruct c { static_cast<short>(beach.X + dx), static_cast<short>(beach.Y + dy) };
								auto const pC = MapClass::Instance.TryGetCellAt(c);

								if (!pC || pC->LandType == LandType::Water || pC->GetBuilding()
									|| MapClass::Instance.GetMovementZoneType(c, MovementZone::Normal, false) != beachZone
									|| !pC->IsClearToMove(pTrans->Type->SpeedType, true, true, -1, pTrans->Type->MovementZone, -1, false))
								{
									continue;
								}

								bool nextToWater = false;

								for (int ny = -1; ny <= 1 && !nextToWater; ++ny)
								{
									for (int nx = -1; nx <= 1 && !nextToWater; ++nx)
									{
										auto const pN = MapClass::Instance.TryGetCellAt(
											CellStruct { static_cast<short>(c.X + nx), static_cast<short>(c.Y + ny) });
										nextToWater = pN && pN->LandType == LandType::Water;
									}
								}

								const int d = dx * dx + dy * dy;

								if (nextToWater && d < bestD)
								{
									bestD = d;
									pFreeShore = pC;
								}
							}
						}
					}

					if (pFreeShore)
					{
						pPickupBeach = pFreeShore;
						CountDecision(NeuralDecision::FerryLandingRerouted, pHouse->ArrayIndex);
					}
				}

				if (pPickupBeach)
				{
					if (pTrans->CurrentMission == Mission::Guard || pTrans->CurrentMission == Mission::Area_Guard
						|| pTrans->CurrentMission == Mission::Unload || (IsAITickWindow(60, 20)))
					{
						IssueMoveOrder(pTrans, pPickupBeach, 4.0);
					}
				}

				continue;
			}

			// =========================================================================
			// PARKED ON DRY LAND ON A SAFE PICKUP ISLAND:
			// 1) Keep the transport 100% stationary on its current land cell (Mission::Guard,
			//    Destination == nullptr). It must NEVER move from the land spot where it waits!
			// 2) Send units strictly ONE BY ONE (only when pSingleActiveBoarder == nullptr)
			//    so the single radio link (0x0073781E) is never stolen and no jamming occurs!
			// =========================================================================
			if ((pTrans->Type->MovementZone == MovementZone::Fly || pTrans->Type->JumpJet) && pTrans->GetHeight() > 0
				&& pTrans->Destination == nullptr && pTransCell && pTransCell->LandType != LandType::Water
				&& !pTransCell->GetBuilding())
			{
				if (pTrans->CurrentMission != Mission::Unload)
					pTrans->QueueMission(Mission::Unload, true);
			}
			else if (pTrans->CurrentMission != Mission::Guard || pTrans->Destination != nullptr)
			{
				pTrans->SetDestination(nullptr, true);
				pTrans->ClearNavigationList();
				pTrans->ForceMission(Mission::Guard);
			}

			if (!pActiveIslandGoal && !enemyOnDisconnectedIsland && pIslandResourceGoalCell == nullptr
				&& pIslandTechGoalCell == nullptr && pendingCrossIslandOrders.empty())
			{
				continue;
			}

			// If ONE unit is already actively boarding this transport right now, wait for that
			// single unit to finish entering before dispatching the next unit!
			if (pSingleActiveBoarder != nullptr)
			{
				// Re-issue Enter only if the active boarder somehow lost its radio link
				if (!pSingleActiveBoarder->ContainsLink(pTrans))
					OrderFootToEnter(pSingleActiveBoarder, pTrans);

				continue;
			}

			const int maxAllowedPassSize = std::max(0, pTrans->Type->Passengers - reservedSizeForDeployer);
			const int freeCapNow = maxAllowedPassSize - totalPassSize;
			if (numPass >= pTrans->Type->Passengers || freeCapNow <= 0)
				continue;

			// Pick the SINGLE best/closest available passenger on this island that fits into freeCapNow:
			// Priority 1: pendingCrossIslandOrders on transZone
			FootClass* pNextBoarder = nullptr;
			bool nextBoarderHasOwnGoal = false;
			double bestBoarderDist = 999999.0;

			for (auto const& ord : pendingCrossIslandOrders)
			{
				if (!ord.Passenger || !ord.Passenger->IsAlive || ord.Passenger->InLimbo || ord.PickupZone != transZone)
					continue;

				if (ord.Passenger->CurrentMission == Mission::Enter || ord.Passenger->QueuedMission == Mission::Enter)
					continue;

				// Never mix destinations: once cargo is bound for an island, only
				// pending orders heading to that same island may join it.
				if (!isValidFerryGoalZone(ord.TargetZone)
					|| (pCustomPassengerGoal && ord.TargetZone != customPassengerZone))
				{
					continue;
				}

				const int osz = std::max(1, ord.PassengerSize);
				if (osz > static_cast<int>(pTrans->Type->SizeLimit) || osz > freeCapNow)
					continue;

				const double d = ord.Passenger->GetMapCoords().DistanceFrom(transCoords);
				if (d < bestBoarderDist)
				{
					bestBoarderDist = d;
					pNextBoarder = ord.Passenger;
					nextBoarderHasOwnGoal = true;
				}
			}

			// Priority 2: Closest available combat vehicle on transZone that fits
			if (!pNextBoarder)
			{
				for (auto const pV : idleCombatUnits)
				{
					if (!pV || pV->InLimbo || !pV->IsAlive || !pV->Type || IsAmphibiousOrAirTransport(pV->Type))
						continue;

					if (pV->CurrentMission == Mission::Enter || pV->QueuedMission == Mission::Enter)
						continue;

					if (pV->CaptureManager && pV->CaptureManager->NumControlNodes() > 0)
						continue;

					const int vehSize = std::max(1, static_cast<int>(pV->Type->TechnoTypeClass::Size));
					if (vehSize > static_cast<int>(pTrans->Type->SizeLimit) || vehSize > freeCapNow)
						continue;

					const int uz
						= ObjectZone(pV, MovementZone::Normal);
					if (transZone >= 0 && uz != transZone)
						continue;

					if (IsFootActivelyAttackingOrMovingToEnemy(pV) || IsFootNearEnemyFrontline(pV, uz))
						continue;

					const double d = pV->GetMapCoords().DistanceFrom(transCoords);
					if (d < bestBoarderDist)
					{
						bestBoarderDist = d;
						pNextBoarder = pV;
					}
				}
			}

			// Priority 3: Closest available combat infantry on transZone that fits
			if (!pNextBoarder && static_cast<int>(pTrans->Type->SizeLimit) >= 1)
			{
				for (auto const pI : idleCombatInf)
				{
					if (!pI || pI->InLimbo || !pI->IsAlive || !pI->Type)
						continue;

					if (pI->CurrentMission == Mission::Enter || pI->QueuedMission == Mission::Enter)
						continue;

					const int infSize = std::max(1, static_cast<int>(pI->Type->TechnoTypeClass::Size));
					if (infSize > static_cast<int>(pTrans->Type->SizeLimit) || infSize > freeCapNow)
						continue;

					const int iz
						= ObjectZone(pI, MovementZone::Normal);
					if (transZone >= 0 && iz != transZone)
						continue;

					if (IsFootActivelyAttackingOrMovingToEnemy(pI) || IsFootNearEnemyFrontline(pI, iz))
						continue;

					const double d = pI->GetMapCoords().DistanceFrom(transCoords);
					if (d < bestBoarderDist)
					{
						bestBoarderDist = d;
						pNextBoarder = pI;
					}
				}
			}

			if (pNextBoarder)
			{
				// Pending cross-island orders keep their own exact objective; other
				// boarders inherit the transport's destination, never the pickup island.
				if (!nextBoarderHasOwnGoal && pActiveIslandGoal && isValidFerryGoalZone(activeIslandZone)
					&& activeIslandZone != transZone)
				{
					RecordPassengerIslandGoal(pNextBoarder->UniqueID, pActiveIslandGoal->MapCoords);
				}

				OrderFootToEnter(pNextBoarder, pTrans);
			}
		}
	}

	microClock.Mark(NeuralSection::MicroAircraft);

	// Dispatch loaded/idle combat Aircraft, Carryall «Matryoshka» Airlift &
	// Emergency Sell-Dodge / ConYard Undeploy
	// Air harassment (slot 32): armed aircraft that re-arm on pads also hunt exposed enemy units
	// (not only buildings), and aircraft striking at once spread over weak targets instead of
	// all overkilling the same one. Sortie damage ~ Damage x Burst x Ammo of the ground weapon.
	auto airSortieDamage = [](AircraftClass* pAircraft) -> int
	{
		int best = 0;

		for (int i = 0; i < 2; ++i)
		{
			auto const pWS = pAircraft->Type->GetWeapon(i);
			auto const pW = pWS ? pWS->WeaponType : nullptr;

			if (pW && pW->Damage > 0 && (!pW->Projectile || pW->Projectile->AG))
				best = std::max(best, pW->Damage * std::max(1, pW->Burst));
		}

		return best * (pAircraft->Type->Ammo > 0 ? pAircraft->Type->Ammo : 4);
	};

	// Enemy UniqueID -> sortie damage already sent at it this tick. Key access only.
	std::unordered_map<DWORD, int> airDamageCommitted;

	for (auto const pOwnAir : AircraftClass::Array)
	{
		if (pOwnAir && pOwnAir->Owner == pHouse && pOwnAir->IsAlive && !pOwnAir->InLimbo && pOwnAir->Type && pOwnAir->Ammo != 0)
		{
			if (auto const pAirTarget = abstract_cast<TechnoClass*>(pOwnAir->Target))
				airDamageCommitted[pAirTarget->UniqueID] += airSortieDamage(pOwnAir);
		}
	}

	std::vector<int> contactAirRisk(enemyContacts.size(), -1);
	const bool airHarassAllowed = SlotGate(scores[32], -100, 32);

	// Expert 8 in charge: strike wings form first (NeuralAI.AirForce.inl).
	const bool airForceInCharge = pExt->NeuralAI_ActiveExpert == 8;

	auto pickAirHarassTarget = [&](AircraftClass* pAircraft) -> TechnoClass*
	{
		const int sortie = airSortieDamage(pAircraft);

		if (sortie <= 0)
			return nullptr;

		const CellStruct ac = pAircraft->GetMapCoords();
		TechnoClass* pBest = nullptr;
		int bestScore = 0;

		for (size_t i = 0; i < enemyContacts.size(); ++i)
		{
			auto const& contact = enemyContacts[i];
			auto const pPrey = contact.Techno;

			if (contact.Airborne || pPrey->WhatAmI() == AbstractType::Building)
				continue;

			const double dist = ac.DistanceFrom(contact.Coords);

			if (dist > 45.0)
				continue;

			// A lone harassing aircraft takes no anti-air risk at all (AirStrikeRiskAt: known sites,
			// anti-air in current sight, places our aircraft were shot down; computed once per
			// contact), so bombers do not dive again and again into the same covered army.
			if (contactAirRisk[i] < 0)
				contactAirRisk[i] = AirStrikeRiskAt(pHouse, contact.Coords);

			if (contactAirRisk[i] > 0)
				continue;

			// Already covered by other strikes (with a 20% margin): leave it, take the next one.
			auto const committedIt = airDamageCommitted.find(pPrey->UniqueID);
			const int committed = committedIt != airDamageCommitted.end() ? committedIt->second : 0;
			const int health = std::max(1, pPrey->Health);

			if (committed >= health + health / 5)
				continue;

			// Prefer what this sortie finishes (no wasted ammo), then value; distance lowers it.
			int score = contact.Value * 100 / (10 + static_cast<int>(dist * 2.0));

			if (committed + sortie >= health)
				score += score / 2;

			if (score <= bestScore)
				continue;

			// The way there must be clear too: halfway and three quarters of the flight.
			const CellStruct halfway { static_cast<short>((ac.X + contact.Coords.X) / 2), static_cast<short>((ac.Y + contact.Coords.Y) / 2) };
			const CellStruct lastLeg { static_cast<short>((halfway.X + contact.Coords.X) / 2), static_cast<short>((halfway.Y + contact.Coords.Y) / 2) };

			if (dist > 12.0 && (AirStrikeRiskAt(pHouse, halfway) > 0 || AirStrikeRiskAt(pHouse, lastLeg) > 0))
				continue;

			bestScore = score;
			pBest = pPrey;
		}

		if (pBest)
			airDamageCommitted[pBest->UniqueID] += sortie;

		return pBest;
	};

	if (airForceInCharge)
		UpdateAirWings(pHouse);

	// Interceptors: enemy air in current sight that threatens the base (the early warning flanks), and
	// with Expert 8 in charge any enemy flyer in sight within 30 cells away from known anti-air
	// defenses. Strikes already sent at a flyer count against it, so interceptors spread.
	auto pickAirInterceptTarget = [&](AircraftClass* pAircraft) -> TechnoClass*
	{
		const int flankIdx = baseAirFlankCount > 0
			? pickAirFlankFor(pAircraft, true, (airForceInCharge ? 40 : 28) * Unsorted::LeptonsPerCell)
			: -1;

		if (flankIdx >= 0 && IsCellInSightOfAI(pHouse, baseAirFlanks[flankIdx].Target->GetMapCoords()))
		{
			baseAirFlanks[flankIdx].AssignedInterceptors++;

			return baseAirFlanks[flankIdx].Target;
		}

		if (!airForceInCharge)
			return nullptr;

		int sortie = 0;

		for (int i = 0; i < 2; ++i)
		{
			auto const pWS = pAircraft->Type->GetWeapon(i);
			auto const pW = pWS ? pWS->WeaponType : nullptr;

			if (pW && pW->Damage > 0 && pW->Projectile && pW->Projectile->AA)
				sortie = std::max(sortie, pW->Damage * std::max(1, pW->Burst));
		}

		sortie *= pAircraft->Type->Ammo > 0 ? pAircraft->Type->Ammo : 4;

		const CellStruct ac = pAircraft->GetMapCoords();
		TechnoClass* pBest = nullptr;
		int bestScore = 0;

		for (auto const& contact : enemyContacts)
		{
			if (!contact.Airborne || ac.DistanceFrom(contact.Coords) > 30.0 || !IsCellInSightOfAI(pHouse, contact.Coords)
				|| CountKnownEnemyDefensesNear(pHouse, contact.Coords, 9.0, true) > 0)
			{
				continue;
			}

			auto const committedIt = airDamageCommitted.find(contact.Techno->UniqueID);
			const int committed = committedIt != airDamageCommitted.end() ? committedIt->second : 0;
			const int health = std::max(1, contact.Techno->Health);

			if (committed >= health + health / 5)
				continue;

			const int score = (contact.Value + contact.Firepower) * 100 / (10 + static_cast<int>(ac.DistanceFrom(contact.Coords) * 2.0));

			if (score > bestScore)
			{
				bestScore = score;
				pBest = contact.Techno;
			}
		}

		if (pBest)
			airDamageCommitted[pBest->UniqueID] += sortie;

		return pBest;
	};

	// A lone aircraft only strikes a building with no anti-air risk at all (known anti-air sites,
	// anti-air units in sight, places our aircraft were shot down), picked once per tick.
	// The enemy mind controller and a lone harvester in sight are worth a dive only where no anti-air
	// covers them.
	const bool airDiveAtMindController = pEnemyMindController && AirStrikeRiskAt(pHouse, pEnemyMindController->GetMapCoords()) == 0;
	const bool airDiveAtHarvester = pVisibleEnemyHarvester && AirStrikeRiskAt(pHouse, pVisibleEnemyHarvester->GetMapCoords()) == 0;

	bool soloStrikePicked = false;
	BuildingClass* pSoloStrikeTarget = nullptr;

	auto getSoloStrikeTarget = [&]() -> BuildingClass*
	{
		if (!soloStrikePicked)
		{
			soloStrikePicked = true;

			if (homeCoords != CellStruct::Empty)
				pSoloStrikeTarget = PickAirStrikeTarget(pHouse, homeCoords, 0, CellStruct::Empty);
		}

		return pSoloStrikeTarget;
	};

	for (auto const pAir : AircraftClass::Array)
	{
		// Aircraft flying a strategic plan (air strike, airborne second front...) belong to that plan.
		if (!pAir || pAir->Owner != pHouse || pAir->InLimbo || !pAir->IsAlive || !pAir->Type
			|| IsPlanMember(pHouse, pAir->UniqueID))
		{
			continue;
		}

		// A hovering aircraft that reached the end of a move stays in Mission::Move with its
		// destination under it: treat it as idle so it can be given new work.
		const bool isAirIdle = (pAir->CurrentMission == Mission::Guard || pAir->CurrentMission == Mission::Area_Guard
			|| pAir->CurrentMission == Mission::Sleep
			|| (pAir->CurrentMission == Mission::Move && pAir->Target == nullptr
				&& (!pAir->Destination || pAir->DistanceFrom(pAir->Destination) <= 384)));

		// Carryall «Matryoshka / Muñeca Rusa» Airlift (Slot 53): Pick up loaded
		// APCs, Expansion Nanocores/MCVs, or rescue critically damaged tanks!
		const int carryallWindow = std::clamp(20 + (static_cast<int>(scores[53]) / 18), 8, 45);

		if (!pAir->Type->Carryall && pAir->Type->Passengers > 0)
		{
			const int numAirPass = pAir->Passengers.NumPassengers;
			const int totalAirPassSize = pAir->Passengers.GetTotalSize();
			const CellStruct airCoords = pAir->GetMapCoords();
			const int airZone = MapClass::Instance.GetMovementZoneType(airCoords, MovementZone::Normal, pAir->OnBridge);
			bool airCarryingMCV = false;
			bool airCarryingHarvester = false;
			bool airCarryingEngineer = false;
			CellClass* pAirCustomPassGoal = nullptr;
			int airCustomPassZone = -1;
			for (FootClass* pPass = pAir->Passengers.GetFirstPassenger(); pPass != nullptr;
				pPass = abstract_cast<FootClass*>(pPass->NextObject))
			{
				if (!pAirCustomPassGoal)
				{
					const CellStruct passGoalCoords = LookupPassengerIslandGoal(pPass->UniqueID);
					if (passGoalCoords != CellStruct::Empty)
					{
						const int pgz
							= MapClass::Instance.GetMovementZoneType(passGoalCoords, MovementZone::Normal, false);
						if (pgz >= 0)
						{
							if (CellClass* pSafePassLanding
								= FindSafeIslandLandingCell(pHouse, passGoalCoords, pgz, false))
							{
								pAirCustomPassGoal = pSafePassLanding;
								airCustomPassZone = pgz;
							}
						}
					}
				}

				if (auto const pPassUnit = abstract_cast<UnitClass*>(pPass))
				{
					if (pPassUnit->Type && pPassUnit->Type->DeploysInto != nullptr)
						airCarryingMCV = true;
					else if (pPassUnit->Type && (pPassUnit->Type->Harvester || pPassUnit->Type->Weeder))
						airCarryingHarvester = true;
				}
				else if (auto const pPassInf = abstract_cast<InfantryClass*>(pPass))
				{
					if (pPassInf->Type && pPassInf->Type->Engineer)
						airCarryingEngineer = true;
				}
			}

			FootClass* pSingleAirBoarder = (!pAir->IsInAir() && pAir->GetHeight() == 0)
				? enforceSingleActiveBoarderOn(pAir, airZone)
				: nullptr;
			const int freeAirCap = pAir->Type->Passengers - totalAirPassSize;
			bool moreUnitsFitAir = (pSingleAirBoarder != nullptr);
			if (!moreUnitsFitAir && !pAir->IsInAir() && pAir->GetHeight() == 0 && freeAirCap > 0
				&& numAirPass < pAir->Type->Passengers)
			{
				for (auto const pV : idleCombatUnits)
				{
					if (!pV || pV->InLimbo || !pV->IsAlive || IsAmphibiousOrAirTransport(pV->Type))
						continue;
					const int vsz = std::max(1, static_cast<int>(pV->Type->TechnoTypeClass::Size));
					const int uz = ObjectZone(pV, MovementZone::Normal);
					if (uz == airZone && vsz <= static_cast<int>(pAir->Type->SizeLimit) && vsz <= freeAirCap)
					{
						moreUnitsFitAir = true;
						break;
					}
				}

				if (!moreUnitsFitAir && static_cast<int>(pAir->Type->SizeLimit) >= 1)
				{
					for (auto const pI : idleCombatInf)
					{
						if (!pI || pI->InLimbo || !pI->IsAlive)
							continue;
						const int isz = std::max(1, static_cast<int>(pI->Type->TechnoTypeClass::Size));
						const int iz = ObjectZone(pI, MovementZone::Normal);
						if (iz == airZone && isz <= static_cast<int>(pAir->Type->SizeLimit) && isz <= freeAirCap)
						{
							moreUnitsFitAir = true;
							break;
						}
					}
				}
			}

			const bool airReadyToFly = airCarryingMCV || airCarryingHarvester
				|| (airCarryingEngineer && pIslandTechGoalCell != nullptr)
				|| (numAirPass >= 1 && (pAir->IsInAir() || !moreUnitsFitAir));
			if (airReadyToFly)
			{
				CellClass* pAirGoal = pAirCustomPassGoal ? pAirCustomPassGoal : pIslandGoalCell;
				int airGoalZone = pAirCustomPassGoal ? airCustomPassZone : targetIslandZone;
				if ((airCarryingMCV || airCarryingHarvester) && pIslandResourceGoalCell)
				{
					pAirGoal = pIslandResourceGoalCell;
					airGoalZone = resourceIslandZone;
				}
				else if (airCarryingEngineer && pIslandTechGoalCell)
				{
					pAirGoal = pIslandTechGoalCell;
					airGoalZone = techIslandZone;
				}
				else if (!pAirGoal)
				{
					pAirGoal = pIslandResourceGoalCell ? pIslandResourceGoalCell
													: (pIslandTechGoalCell ? pIslandTechGoalCell : pWaveTargetCell);
					airGoalZone
						= pIslandResourceGoalCell ? resourceIslandZone : (pIslandTechGoalCell ? techIslandZone : -1);
				}

				if (pAirGoal)
				{
					CellClass* pSafeAirDrop = FindSafeIslandLandingCell(
						pHouse, pAirGoal->MapCoords, airGoalZone, airCarryingMCV || airCarryingHarvester);
					if (pSafeAirDrop)
					{
						if (airCoords.DistanceFrom(pSafeAirDrop->MapCoords) <= 4.0
							&& pSafeAirDrop->LandType != LandType::Water)
						{
							if (pAir->CurrentMission != Mission::Unload)
							{
								pAir->SetDestination(pSafeAirDrop, true);
								pAir->QueueMission(Mission::Unload, true);
							}
						}
						else if (isAirIdle || (IsAITickWindow(90, 25)))
						{
							IssueMoveOrder(pAir, GetSafeAirHop(pHouse, pAir, pSafeAirDrop), 3.0);
						}
					}
				}
			}
			else if (numAirPass == 0 && primaryArmyZone >= 0 && airZone != primaryArmyZone
				&& homeCoords != CellStruct::Empty)
			{
				if (CellClass* pHomeAirBeach = FindSafeIslandLandingCell(pHouse, homeCoords, primaryArmyZone, false))
				{
					if (isAirIdle)
						IssueMoveOrder(pAir, GetSafeAirHop(pHouse, pAir, pHomeAirBeach), 3.0);
				}
			}
			else if (!pAir->IsInAir() && pAir->GetHeight() == 0 && pSingleAirBoarder == nullptr && freeAirCap > 0
				&& (!pendingCrossIslandOrders.empty() || enemyOnDisconnectedIsland || pIslandResourceGoalCell != nullptr
					|| pIslandTechGoalCell != nullptr || scores[28] > 600 || SlotGate(scores[75], 100, 75)))
			{
				FootClass* pNextAirPass = nullptr;
				double bestAirPassDist = 999999.0;
				for (auto const& ord : pendingCrossIslandOrders)
				{
					if (!ord.Passenger || !ord.Passenger->IsAlive || ord.Passenger->InLimbo || ord.PickupZone != airZone)
						continue;
					if (ord.Passenger->CurrentMission == Mission::Enter || ord.Passenger->QueuedMission == Mission::Enter)
						continue;
					const int osz = std::max(1, ord.PassengerSize);
					if (osz <= static_cast<int>(pAir->Type->SizeLimit) && osz <= freeAirCap)
					{
						const double d = ord.Passenger->GetMapCoords().DistanceFrom(airCoords);
						if (d < bestAirPassDist)
						{
							bestAirPassDist = d;
							pNextAirPass = ord.Passenger;
						}
					}
				}

				if (!pNextAirPass)
				{
					for (auto const pV : idleCombatUnits)
					{
						if (!pV || pV->InLimbo || !pV->IsAlive || IsAmphibiousOrAirTransport(pV->Type))
							continue;
						if (pV->CurrentMission == Mission::Enter || pV->QueuedMission == Mission::Enter)
							continue;
						const int vehSize = std::max(1, static_cast<int>(pV->Type->TechnoTypeClass::Size));
						const int uz = ObjectZone(pV, MovementZone::Normal);
						if (IsFootActivelyAttackingOrMovingToEnemy(pV) || IsFootNearEnemyFrontline(pV, uz))
							continue;
						if (uz == airZone && vehSize <= static_cast<int>(pAir->Type->SizeLimit) && vehSize <= freeAirCap)
						{
							const double d = pV->GetMapCoords().DistanceFrom(airCoords);
							if (d < bestAirPassDist)
							{
								bestAirPassDist = d;
								pNextAirPass = pV;
							}
						}
					}
				}

				if (!pNextAirPass && static_cast<int>(pAir->Type->SizeLimit) >= 1)
				{
					for (auto const pI : idleCombatInf)
					{
						if (!pI || pI->InLimbo || !pI->IsAlive)
							continue;
						if (pI->CurrentMission == Mission::Enter || pI->QueuedMission == Mission::Enter)
							continue;
						const int infSize = std::max(1, static_cast<int>(pI->Type->TechnoTypeClass::Size));
						const int iz = ObjectZone(pI, MovementZone::Normal);
						if (IsFootActivelyAttackingOrMovingToEnemy(pI) || IsFootNearEnemyFrontline(pI, iz))
							continue;
						if (iz == airZone && infSize <= static_cast<int>(pAir->Type->SizeLimit) && infSize <= freeAirCap)
						{
							const double d = pI->GetMapCoords().DistanceFrom(airCoords);
							if (d < bestAirPassDist)
							{
								bestAirPassDist = d;
								pNextAirPass = pI;
							}
						}
					}
				}

				if (pNextAirPass)
					OrderFootToEnter(pNextAirPass, pAir);
			}

			continue;
		}


		if (pAir->Type->Carryall && pAir->Passengers.NumPassengers > 0)
		{
			FootClass* pCarriedFoot = pAir->Passengers.GetFirstPassenger();
			UnitClass* pCarriedUnit = abstract_cast<UnitClass*>(pCarriedFoot);
			CellClass* pDropCell = nullptr;
			if (pCarriedUnit && pCarriedUnit->Type)
			{
				BuildingClass* const pCarriedRepair = (pRepairDepot && (pCarriedUnit->GetHealthPercentage() < 0.45
									|| (pCarriedUnit->Type->Cost >= 1000 && pCarriedUnit->GetHealthPercentage() < 0.75)))
					? FindBestRepairStation(pHouse, pCarriedUnit, true)
					: nullptr;

				if (pCarriedRepair)
				{
					pDropCell = pCarriedRepair->GetCell();
				}
				else if ((pCarriedUnit->Type->DeploysInto != nullptr || pCarriedUnit->Type->Harvester
							|| pCarriedUnit->Type->Weeder)
					&& pIslandResourceGoalCell)
				{
					pDropCell = pIslandResourceGoalCell;
				}
			}

			if (!pDropCell)
			{
				pDropCell = pIslandGoalCell
					? pIslandGoalCell
					: (pIslandTechGoalCell ? pIslandTechGoalCell
										: (pIslandResourceGoalCell ? pIslandResourceGoalCell : pWaveTargetCell));
			}

			if (pDropCell)
			{
				// Badly damaged, or the drop zone got covered by anti-air: bring the cargo home.
				pDropCell = ResolveAirDropCell(pHouse, pAir, pDropCell);

				if (pAir->DistanceFrom(pDropCell) <= 768 && pDropCell->LandType != LandType::Water)
				{
					if (pAir->CurrentMission != Mission::Unload)
					{
						CountDecision(NeuralDecision::AirliftDrop, pHouse->ArrayIndex);
						pAir->SetDestination(pDropCell, true);
						pAir->QueueMission(Mission::Unload, true);
					}
				}
				else if (isAirIdle || (IsAITickWindow(90, 25)))
				{
					// Fly around known enemy anti-air when a safer lane exists.
					IssueMoveOrder(pAir, GetSafeAirHop(pHouse, pAir, pDropCell), 3.0);
				}
			}

			continue;
		}

		// Empty carryall still flying the safe detour toward its pickup.
		if (pAir->Type->Carryall && pAir->Passengers.NumPassengers == 0)
		{
			if (auto const pPendingCargo = FindPendingPickupCargo(pHouse, pAir))
			{
				if (CanCarryallLiftUnit(pAir, pPendingCargo))
				{
					if (isAirIdle || IsAITickWindow(90, 25))
						AdvanceRoutedPickup(pHouse, pAir, pPendingCargo, false);

					continue;
				}

				AirPickups.erase(pAir->UniqueID);
			}
		}

		if (pAir->Type->Carryall && isAirIdle && pAir->Passengers.NumPassengers == 0
			&& (IsAITickWindow(90, carryallWindow)))
		{
			bool pickedAirliftJob = false;
			const bool islandLiftsPaused = Unsorted::CurrentFrame - GetMacroState(pHouse).LastAirDropAbortFrame < 1800;
			const bool evacuationAllowed = pRepairDepot && SlotGate(scores[54], -200, 54);

			for (auto const pU : UnitClass::Array)
			{
				if (!pU || pU->Owner != pHouse || pU->InLimbo || !pU->IsAlive || !pU->Type
					|| !CanCarryallLiftUnit(pAir, pU))
				{
					continue;
				}

				const int uZone
					= ObjectZone(pU, MovementZone::Normal);
				const bool isCriticallyDamagedTank
					= (pRepairDepot && pU->Type->Cost >= 1000 && pU->GetHealthPercentage() < 0.40);
				const bool isLoadedStrikeAPC = (pU->Type->Passengers >= 3 && pU->Passengers.NumPassengers >= 1
					&& (pWaveTargetCell || pIslandGoalCell || pIslandTechGoalCell));
				const bool isIslandExpansionMCV = (pU->Type->DeploysInto != nullptr && pIslandResourceGoalCell
					&& uZone >= 0 && uZone != resourceIslandZone
					&& (pHouse->NumConYards >= 1 || pHouse->CountOwnedNow(pU->Type) >= 2));
				const bool isIslandHarvester = ((pU->Type->Harvester || pU->Type->Weeder) && pIslandResourceGoalCell
					&& uZone == homeGroundZone && pHouse->CountOwnedNow(pU->Type) >= 2);
				// Combat evacuation: an expensive unit losing its fight is pulled out early.
				const bool isLosingExpensiveUnit = evacuationAllowed && !isCriticallyDamagedTank && pU->Type->Cost >= 1000
					&& pU->GetHealthPercentage() < 0.70 && IsFootOutmatched(pHouse, pU, 7.0);
				const bool isIslandJob = isIslandExpansionMCV || isIslandHarvester;
				if (isCriticallyDamagedTank || isLosingExpensiveUnit || isLoadedStrikeAPC || (isIslandJob && !islandLiftsPaused))
				{
					AdvanceRoutedPickup(pHouse, pAir, pU, false);
					CountDecision(NeuralDecision::AirliftJob, pHouse->ArrayIndex);

					if (isCriticallyDamagedTank)
						CountDecision(NeuralDecision::AirliftRescue, pHouse->ArrayIndex);
					else if (isLosingExpensiveUnit)
						CountDecision(NeuralDecision::CombatEvac, pHouse->ArrayIndex);

					pickedAirliftJob = true;
					break;
				}
			}

			if (!pickedAirliftJob)
				ReturnIdleCarryallToBase(pHouse, pAir);

			continue;
		}

		if (false && pAir->Type->Carryall && isAirIdle && pAir->Passengers.NumPassengers == 0
			&& (IsAITickWindow(90, carryallWindow)))
		{
			for (auto const pU : UnitClass::Array)
			{
				if (!pU || pU->Owner != pHouse || pU->InLimbo || !pU->IsAlive || !pU->Type || pU->Type->Naval)
					continue;

				const bool isCriticallyDamagedTank
					= (pRepairDepot && pU->Type->Cost >= 1000 && pU->GetHealthPercentage() < 0.40);

				const bool isLoadedStrikeAPC
					= (pU->Type->Passengers >= 3 && pU->Passengers.NumPassengers >= 2 && pWaveTargetCell);

				if (isCriticallyDamagedTank || isLoadedStrikeAPC)
				{
					pAir->SetTarget(pU);

					pAir->QueueMission(Mission::Capture, true);

					break;
				}
			}

			continue;
		}

		// Aircraft with unlimited ammo (Ammo=-1) have no load to spend and are always ready.
		const bool hasAirLoad = pAir->Ammo > 0 || (pAir->Type->Ammo < 0 && !pAir->Type->Spawned);
		const bool isAirForceCraft = IsAirForceCraft(pAir->Type);

		if (hasAirLoad && isAirIdle)
		{
			TechnoClass* pInterceptTarget = nullptr;

			if (airDiveAtMindController)
			{
				pAir->SetTarget(pEnemyMindController);

				pAir->QueueMission(Mission::Attack, true);
			}
			else if (isAirForceCraft && pAir->GetHealthPercentage() >= 0.4 && AircraftHitsAir(pAir->Type)
				&& (pInterceptTarget = pickAirInterceptTarget(pAir)) != nullptr)
			{
				pAir->SetTarget(pInterceptTarget);
				pAir->QueueMission(Mission::Attack, true);
				CountDecision(NeuralDecision::AirInterceptSortie, pHouse->ArrayIndex);
			}
			else if (airForceInCharge && isAirForceCraft && ContinueAirStrike(pHouse, pAir))
			{
				// Flying (or rearmed for) its wing strike.
			}
			else if (TechnoClass* const pPrey = (airHarassAllowed && !pAir->Type->Spawned) ? pickAirHarassTarget(pAir) : nullptr)
			{
				pAir->SetTarget(pPrey);
				pAir->QueueMission(Mission::Attack, true);
				CountDecision(NeuralDecision::AirHarass, pHouse->ArrayIndex);
			}
			else if (airDiveAtHarvester && SlotGate(scores[56], 200, 56))
			{
				pAir->SetTarget(pVisibleEnemyHarvester);

				pAir->QueueMission(Mission::Attack, true);
			}
			else if (!airForceInCharge && isAirForceCraft && AircraftHitsGround(pAir->Type) && getSoloStrikeTarget()
				&& AirSortieDamageVs(pAir, pSoloStrikeTarget) > 0)
			{
				pAir->SetTarget(pSoloStrikeTarget);
				pAir->QueueMission(Mission::Attack, true);
				CountDecision(NeuralDecision::AirSoloStrike, pHouse->ArrayIndex);
			}
			else
			{
				// Nothing to strike (or, with Expert 8 in charge, waiting for its wing): patrol the
				// route of key points (NeuralAI.AirPatrol.inl).
				AssignAirPatrol(pHouse, pAir);
			}
		}
	}

	microClock.Mark(NeuralSection::MicroBuildings);

	// Dying Garrison Evacuation, Re-Mobilizing Deployed Siege Units (Slot 57),
	// Smart Wrench Building Repair (Slot 59), Emergency Sell-Dodge & ConYard
	// Emergency Undeploy into MCV (Slot 54)!
	if (IsAITickWindow(30, 10))
	{
		// Evacuate our garrisoned infantry from any civilian/bunker structure that
		// is about to collapse (< 18% HP) so the squad survives!
		for (auto const pBld : BuildingClass::Array)
		{
			if (!pBld || pBld->InLimbo || !pBld->IsAlive || pBld->Occupants.Count == 0)
				continue;

			if (pBld->Occupants.GetItem(0) && pBld->Occupants.GetItem(0)->Owner == pHouse
				&& pBld->GetHealthPercentage() < 0.18)
			{
				pBld->QueueMission(Mission::Unload, true);
			}
		}

		const double sellDodgeThresh = std::clamp(0.09 + static_cast<double>(scores[54]) * 0.0001, 0.05, 0.16);
		const double undeployThresh = std::clamp(0.14 + static_cast<double>(scores[54]) * 0.00012, 0.08, 0.22);
		// Wrench repairs cost credits: none while broke (UpdateEconomyCrisis), they go to a harvester.
		const int currentCredits = GetMacroState(pHouse).BrokeHoldActive ? 0 : pHouse->Available_Money();
		const bool isLowPower = pHouse->HasLowPower();
		bool hasPoweredDefense = false;

		if (isLowPower)
		{
			for (auto const pCheckBld : pHouse->Buildings)
			{
				if (pCheckBld && !pCheckBld->InLimbo && pCheckBld->IsAlive && pCheckBld->Type
					&& pCheckBld->Type->IsBaseDefense && pCheckBld->Type->Powered && pCheckBld->StuffEnabled)
				{
					hasPoweredDefense = true;

					break;
				}
			}
		}

		for (auto const pOwnBld : pHouse->Buildings)
		{
			if (!pOwnBld || pOwnBld->InLimbo || !pOwnBld->IsAlive || !pOwnBld->Type || pOwnBld->IsIronCurtained())
				continue;

			// Slot 60 (Emergency Low-Power Defense Prioritization): If our base is in
			// LowPower and we own powered base defenses (Tesla Coils, Prism Towers,
			// Obelisks, SAMs), temporarily toggle offline non-defense high-drain
			// structures (Superweapons, Tech/Radar buildings) via GoOffline() so base
			// defenses stay active! Once power surplus is restored (>= 150), bring
			// offline structures back online via GoOnline().
			if (pOwnBld->Type->TogglePower && (IsAITickWindow(90, 15)))
			{
				if (isLowPower && hasPoweredDefense && SlotGate(scores[60], 80, 60) && pOwnBld->StuffEnabled
					&& !pOwnBld->Type->IsBaseDefense && pOwnBld->Type->PowerDrain >= 100
					&& (pOwnBld->Type->SuperWeapon != -1 || pOwnBld->Type->Factory == AbstractType::None))
				{
					pOwnBld->GoOffline();
				}
				else if (!isLowPower && !pOwnBld->StuffEnabled && (pHouse->PowerOutput - pHouse->PowerDrain >= 150))
				{
					pOwnBld->GoOnline();
				}
			}

			// Slot 75 (Island Beachhead Primary Factory Switch): Once we construct a
			// War Factory or Barracks on a disconnected island (e.g. from a landed
			// Beachhead MCV), set it as PrimaryFactory via TogglePrimaryFactory() so
			// new tanks and infantry spawn directly on the enemy's island!
			if (!pOwnBld->IsPrimaryFactory
				&& (pOwnBld->Type->Factory == AbstractType::UnitType
					|| pOwnBld->Type->Factory == AbstractType::InfantryType)
				&& !pOwnBld->Type->Naval && (IsAITickWindow(90, 20)))
			{
				const int bldZone
					= ObjectZone(pOwnBld, MovementZone::Normal);
				bool factoryOnEnemyIsland = (enemyOnDisconnectedIsland && bldZone >= 0 && bldZone == targetIslandZone);
				if (!factoryOnEnemyIsland && bldZone >= 0 && homeGroundZone >= 0 && bldZone != homeGroundZone)
				{
					for (auto const pEB : BuildingClass::Array)
					{
						if (!pEB || pEB->InLimbo || !pEB->IsAlive || !pEB->Owner || !pEB->Type || pEB->Owner == pHouse
							|| pHouse->IsAlliedWith(pEB->Owner) || !IsEnemyBuildingKnown(pHouse, pEB) || pEB->Owner->IsNeutral())
						{
							continue;
						}

						if (ObjectZone(pEB, MovementZone::Normal)
							== bldZone)
						{
							factoryOnEnemyIsland = true;
							break;
						}
					}
				}

				if (factoryOnEnemyIsland)
				{
					bool alreadyHasPrimaryInZone = false;
					for (auto const pOtherB : pHouse->Buildings)
					{
						if (!pOtherB || pOtherB == pOwnBld || pOtherB->InLimbo || !pOtherB->IsAlive || !pOtherB->Type
							|| !pOtherB->IsPrimaryFactory)
						{
							continue;
						}

						if (pOtherB->Type->Factory == pOwnBld->Type->Factory
							&& pOtherB->Type->Naval == pOwnBld->Type->Naval
							&& ObjectZone(pOtherB, MovementZone::Normal)
								== bldZone)
						{
							alreadyHasPrimaryInZone = true;
							break;
						}
					}

					if (!alreadyHasPrimaryInZone)
						pOwnBld->TogglePrimaryFactory();
				}
			}

			const double hpPct = pOwnBld->GetHealthPercentage();

			// Slot 73 (Dynamic Tank Bunker Unload): Eject the bunkered tank if the
			// Tank Bunker is about to collapse (< 22% HP) or if no enemies are near!
			if (pOwnBld->Type->Bunker && pOwnBld->BunkerLinkedItem != nullptr && SlotGate(scores[73], 60, 73))
			{
				if (hpPct < 0.22)
					pOwnBld->UnloadBunker();
			}

			// Helper to properly undeploy a BuildingClass into its UndeploysInto
			// vehicle (MCV, Tick Tank, Nod Artillery, Siege Chopper): In the Yuri's
			// Revenge engine, BuildingClass undeployment requires setting a non-null
			// ArchiveTarget cell and calling Sell(1) (whereas selling for cash
			// requires ArchiveTarget == nullptr).
			auto UndeployBuildingToVehicleRaw = [](BuildingClass* pBld, const CellStruct& goalCoords)
			{
				if (!pBld || !pBld->Type || !pBld->Type->UndeploysInto)
					return;

				// Never manually force-undeploy a Slave Miner / ResourceGatherer
				// building via Sell(1); its native BuildingClass::Mission_Harvest state
				// machine handles undeployment when moving between ore patches!
				if (pBld->Type->ResourceGatherer || pBld->Type->Enslaves != nullptr)
					return;

				if (pBld->CurrentMission == Mission::Construction || pBld->CurrentMission == Mission::Selling
					|| pBld->QueuedMission == Mission::Construction || pBld->QueuedMission == Mission::Selling)
				{
					return;
				}

				const CellStruct bc = pBld->GetMapCoords();
				const int bZone = MapClass::Instance.GetMovementZoneType(bc, MovementZone::Normal, pBld->OnBridge);
				CellClass* pDestCell = nullptr;
				if (goalCoords != CellStruct::Empty && goalCoords != bc)
				{
					if (auto const pGC = MapClass::Instance.TryGetCellAt(goalCoords))
					{
						if (pGC->LandType != LandType::Water
							&& (bZone < 0
								|| MapClass::Instance.GetMovementZoneType(goalCoords, MovementZone::Normal, false)
									== bZone))
						{
							pDestCell = pGC;
						}
					}
				}

				if (!pDestCell)
				{
					for (int dy = -3; dy <= 3 && !pDestCell; ++dy)
					{
						for (int dx = -3; dx <= 3 && !pDestCell; ++dx)
						{
							if (dx == 0 && dy == 0)
								continue;

							const CellStruct nc { static_cast<short>(bc.X + dx), static_cast<short>(bc.Y + dy) };
							if (auto const pNC = MapClass::Instance.TryGetCellAt(nc))
							{
								if (pNC->LandType != LandType::Water && pNC->GetBuilding() == nullptr
									&& (bZone < 0
										|| MapClass::Instance.GetMovementZoneType(nc, MovementZone::Normal, false)
											== bZone))
								{
									pDestCell = pNC;
								}
							}
						}
					}
				}

				if (!pDestCell)
					pDestCell = pBld->GetCell();

				if (pDestCell)
				{
					pBld->SetArchiveTarget(pDestCell);
					pBld->Sell(1);
				}
			};

			auto UndeployBuildingToVehicle = [&](BuildingClass* pBld, const CellStruct& goalCoords)
			{
				if (!pBld || !pBld->Type)
					return;

				if (pBld->Type->ConstructionYard)
				{
					auto& yardMacro = GetMacroState(pHouse);
					const CellStruct here = pBld->GetMapCoords();

					if (Unsorted::CurrentFrame - yardMacro.LastYardUndeployFrame < 3600
						&& yardMacro.LastYardUndeployCell != CellStruct::Empty
						&& here.DistanceFrom(yardMacro.LastYardUndeployCell) <= 8.0)
					{
						CountDecision(NeuralDecision::YardUndeployBlocked, pHouse->ArrayIndex);
						return;
					}

					yardMacro.LastYardUndeployCell = here;
					yardMacro.LastYardUndeployFrame = Unsorted::CurrentFrame;
				}

				UndeployBuildingToVehicleRaw(pBld, goalCoords);
			};

			// Always activate wrench repair immediately on any damaged Construction Yard
			// so it never stays below undeployThresh or stalls tech/defense production
			if (pOwnBld->Type->ConstructionYard && pOwnBld->Type->ClickRepairable && hpPct < 0.92
				&& currentCredits >= 100 && !pOwnBld->IsBeingRepaired)
			{
				pOwnBld->SetRepairState(1);
			}

			bool handledConYardAction = false;

			if (hpPct < 0.16 && pOwnBld->HasBeenAttacked && pOwnBld->Type->ConstructionYard && pHouse->NumConYards == 1
				&& pOwnBld->Type->UndeploysInto != nullptr && GameModeOptionsClass::Instance.MCVRedeploy)
			{
				const CellStruct yc = pOwnBld->GetMapCoords();
				const int yz = MapClass::Instance.GetMovementZoneType(yc, MovementZone::Normal, pOwnBld->OnBridge);
				bool closeAttackerNearCY = false;
				ForEachTechnoNear(yc, 12, [&](TechnoClass* pET) -> bool
				{
					if (!pET->Owner || pET->WhatAmI() == AbstractType::Building)
						return true;

					if (pET->Owner == pHouse || pHouse->IsAlliedWith(pET->Owner) || !IsCellVisibleToAI(pHouse, pET->GetCell()) || pET->Owner->IsNeutral())
						return true;

					if (pOwnBld->DistanceFrom(pET) <= 2048)
						closeAttackerNearCY = true;

					return !closeAttackerNearCY;
				});

				CellStruct safeRetreatHub = CellStruct::Empty;

				if (closeAttackerNearCY)
				{
					for (auto const pFriendlyB : pHouse->Buildings)
					{
						if (!pFriendlyB || pFriendlyB == pOwnBld || pFriendlyB->InLimbo || !pFriendlyB->IsAlive
							|| !pFriendlyB->Type)
						{
							continue;
						}

						const CellStruct fc = pFriendlyB->GetMapCoords();

						if (yz >= 0 && MapClass::Instance.GetMovementZoneType(fc, MovementZone::Normal, pFriendlyB->OnBridge) != yz)
							continue;

						if (fc.DistanceFrom(yc) >= 16.0)
						{
							safeRetreatHub = fc;
							break;
						}
					}
				}

				// Only pack up our last ConYard if an enemy is right on top of it AND we own a
				// distant friendly hub (>= 16 cells away) to retreat to; otherwise stay deployed
				// with wrench repair active and keep building defenses and units!
				if (closeAttackerNearCY && safeRetreatHub != CellStruct::Empty)
				{
					UndeployBuildingToVehicle(pOwnBld, safeRetreatHub);
					handledConYardAction = true;
				}
			}
			else if (pOwnBld->Type->ConstructionYard && pHouse->NumConYards >= 2
				&& pOwnBld != pHouse->ConYards.GetItem(0) && SlotGate(scores[82], 60, 82) && homeCoords != CellStruct::Empty
				&& pOwnBld->GetMapCoords().DistanceFrom(homeCoords) > 18.0)
			{
				const CellStruct yc = pOwnBld->GetMapCoords();
				int closeHostileAttackers = 0;

				for (auto const pEB : BuildingClass::Array)
				{
					if (!pEB || pEB->InLimbo || !pEB->IsAlive || !pEB->Owner || !pEB->Type || pEB->Type->Immune)
						continue;

					if (pEB->Owner == pHouse || pHouse->IsAlliedWith(pEB->Owner) || !IsEnemyBuildingKnown(pHouse, pEB) || pEB->Owner->IsNeutral())
						continue;

					const CellStruct ec = pEB->GetMapCoords();
					const int dB = std::max(std::abs(yc.X - ec.X), std::abs(yc.Y - ec.Y));

					if (pEB->Type->IsBaseDefense && dB <= 11)
						closeHostileAttackers++;
				}

				ForEachTechnoNear(yc, 14, [&](TechnoClass* pET) -> bool
				{
					if (!pET->Owner || pET->WhatAmI() == AbstractType::Building)
						return true;

					if (pET->Owner == pHouse || pHouse->IsAlliedWith(pET->Owner) || !IsCellVisibleToAI(pHouse, pET->GetCell()) || pET->Owner->IsNeutral())
						return true;

					if (pOwnBld->DistanceFrom(pET) <= 2560)
						closeHostileAttackers++;

					return true;
				});

				const bool hostileZone = (closeHostileAttackers >= 1)
					&& (pOwnBld->HasBeenAttacked || hpPct < 0.75 || closeHostileAttackers >= 2);

				if (hostileZone)
				{
					const bool canUndeployToMCV
						= (GameModeOptionsClass::Instance.MCVRedeploy && pOwnBld->Type->UndeploysInto != nullptr);
					const int yardZone = MapClass::Instance.GetMovementZoneType(yc, MovementZone::Normal, pOwnBld->OnBridge);
					const bool onEnemyIsland = (homeGroundZone >= 0 && yardZone >= 0 && yardZone != homeGroundZone);
					bool hasAmphibTransportOrNavalYard = (pHouse->NumShipyards > 0);

					if (onEnemyIsland && !hasAmphibTransportOrNavalYard)
					{
						for (auto const pT : UnitClass::Array)
						{
							if (pT && pT->Owner == pHouse && !pT->InLimbo && pT->IsAlive && pT->Type
								&& IsAmphibiousOrAirTransport(pT->Type))
							{
								hasAmphibTransportOrNavalYard = true;
								break;
							}
						}
					}

					const bool impossibleToExtractAlive
						= (!canUndeployToMCV && (hpPct < 0.65 || closeHostileAttackers >= 2))
						|| (hpPct < 0.32 && closeHostileAttackers >= 2)
						|| (onEnemyIsland && !hasAmphibTransportOrNavalYard
							&& (hpPct < 0.60 || closeHostileAttackers >= 1));

					if (impossibleToExtractAlive)
					{
						pOwnBld->SetArchiveTarget(nullptr);
						pOwnBld->Sell(1);
					}
					else if (canUndeployToMCV
						&& (!onEnemyIsland || CanHouseFerryTypeAcrossWater(pHouse, pOwnBld->Type->UndeploysInto)))
					{
						// Across water the MCV asks for a transport home (MCV logic above).
						UndeployBuildingToVehicle(pOwnBld, homeCoords);
					}
					else if (onEnemyIsland)
					{
						// Home is across water and nothing can carry the MCV: packing up only made an MCV
						// that redeployed on the spot and packed up again, in a loop. Stay and repair.
						if (pOwnBld->Type->ClickRepairable && hpPct < 0.90 && currentCredits > 600 && !pOwnBld->IsBeingRepaired)
							pOwnBld->SetRepairState(1);
					}
					else if (pOwnBld->Type->ClickRepairable && hpPct < 0.90 && currentCredits > 600
						&& !pOwnBld->IsBeingRepaired)
					{
						pOwnBld->SetRepairState(1);
					}

					handledConYardAction = true;
				}
			}

			const bool conYardUnderRecentAttack = pOwnBld->HasBeenAttacked || hpPct < 0.90
				|| (pHouse->LATime > 0 && (Unsorted::CurrentFrame - pHouse->LATime) < 1200);

			if (!handledConYardAction && !conYardUnderRecentAttack && GameModeOptionsClass::Instance.MCVRedeploy
				&& pOwnBld->Type->ConstructionYard && pOwnBld->Type->UndeploysInto != nullptr
				&& (IsAITickWindow(240, 20)))
			{
				FactoryClass* pBldFac
					= pHouse->GetPrimaryFactory(AbstractType::BuildingType, false, BuildCat::DontCare);
				FactoryClass* pDefFac = pHouse->GetPrimaryFactory(AbstractType::BuildingType, false, BuildCat::Combat);
				const bool busyConstructingBld
					= ((pBldFac && pBldFac->Object != nullptr) || (pDefFac && pDefFac->Object != nullptr));
				const CellStruct yc = pOwnBld->GetMapCoords();
				const int yz = MapClass::Instance.GetMovementZoneType(yc, MovementZone::Normal, pOwnBld->OnBridge);
				auto const pMcvType = pOwnBld->Type->UndeploysInto;
				bool ownsLiveMcvTransport = false;

				for (auto const pT : UnitClass::Array)
				{
					if (pT && pT->Owner == pHouse && !pT->InLimbo && pT->IsAlive && pT->Type
						&& IsAmphibiousOrAirTransport(pT->Type)
						&& static_cast<int>(pT->Type->SizeLimit) >= static_cast<int>(pMcvType->TechnoTypeClass::Size)
						&& pT->Type->Passengers >= static_cast<int>(pMcvType->TechnoTypeClass::Size))
					{
						ownsLiveMcvTransport = true;
						break;
					}
				}

				const unsigned int yardHouseBit
					= (pHouse->Type && pHouse->Type->ArrayIndex >= 0 && pHouse->Type->ArrayIndex < 32)
					? (1u << pHouse->Type->ArrayIndex)
					: 0u;
				const bool canBuildAdditionalMCV = (pHouse->NumWarFactories >= 1 && pMcvType->TechLevel >= 0
					&& pMcvType->TechLevel <= pHouse->TechLevel && yardHouseBit != 0u
					&& pMcvType->InOwners(yardHouseBit) && HouseExt::PrerequisitesMet(pHouse, pMcvType));
				const bool canFerryNomadNow
					= (pIslandResourceGoalCell != nullptr && yz != resourceIslandZone && ownsLiveMcvTransport);

				bool hasUnharvestedOreNearThisYard = false;
				CellClass* pDistantUncolonizedLandOreCell = nullptr;
				int bestDistantOreDist = 999999;

				for (int dy = -64; dy <= 64; dy += 4)
				{
					for (int dx = -64; dx <= 64; dx += 4)
					{
						const CellStruct tc { static_cast<short>(yc.X + dx), static_cast<short>(yc.Y + dy) };
						auto const pC = MapClass::Instance.TryGetCellAt(tc);

						if (!pC)
							continue;

						if (MapClass::Instance.GetMovementZoneType(tc, MovementZone::Normal, false) != yz)
							continue;

						const auto pTerr = pC->GetTerrain(false);
						const bool hasOreOrTree = (pC->GetContainedTiberiumValue() > 0)
							|| (pTerr && pTerr->Type && pTerr->Type->SpawnsTiberium);

						if (!hasOreOrTree)
							continue;

						const int chebDist = std::max(std::abs(dx), std::abs(dy));

						if (chebDist <= 22)
						{
							hasUnharvestedOreNearThisYard = true;
							break;
						}
						else if (chebDist >= 26 && chebDist < bestDistantOreDist)
						{
							bool coveredByOwnedHub = false;

							for (auto const pCheckB : pHouse->Buildings)
							{
								if (!pCheckB || pCheckB->InLimbo || !pCheckB->IsAlive || !pCheckB->Type)
									continue;

								if (pCheckB->Type->Refinery || pCheckB->Type->ConstructionYard
									|| pCheckB->Type->ResourceDestination)
								{
									const CellStruct cbc = pCheckB->GetMapCoords();

									if (std::max(std::abs(tc.X - cbc.X), std::abs(tc.Y - cbc.Y)) <= 16)
									{
										coveredByOwnedHub = true;
										break;
									}
								}
							}

							if (!coveredByOwnedHub)
							{
								bestDistantOreDist = chebDist;
								pDistantUncolonizedLandOreCell = pC;
							}
						}
					}

					if (hasUnharvestedOreNearThisYard)
						break;
				}

				if (!pDistantUncolonizedLandOreCell)
					pDistantUncolonizedLandOreCell = FindFreeOreFieldCell(pHouse, yc, yz, 26);

				int structuresDroppedHere = 0;
				int defsDroppedHere = 0;
				bool hasRefineryHere = false;
				bool hasWarFacHere = false;

				for (auto const pNearB : pHouse->Buildings)
				{
					if (!pNearB || pNearB == pOwnBld || pNearB->InLimbo || !pNearB->IsAlive || !pNearB->Type)
						continue;

					const CellStruct nc = pNearB->GetMapCoords();

					if (std::max(std::abs(yc.X - nc.X), std::abs(yc.Y - nc.Y)) <= 16
						&& MapClass::Instance.GetMovementZoneType(nc, MovementZone::Normal, pNearB->OnBridge) == yz)
					{
						if (pNearB->Type->Refinery || pNearB->Type->ResourceDestination || pNearB->Type->BaseNormal
							|| pNearB->Type->Factory != AbstractType::None || pNearB->Type->IsBaseDefense)
						{
							structuresDroppedHere++;
						}

						if (pNearB->Type->Refinery || pNearB->Type->ResourceDestination)
							hasRefineryHere = true;

						if (pNearB->Type->Factory == AbstractType::UnitType && !pNearB->Type->Naval)
							hasWarFacHere = true;

						if (pNearB->Type->IsBaseDefense && !IsTunnelNetworkBuilding(pNearB->Type))
							defsDroppedHere++;
					}
				}

				const bool hasRefineryOrFactoryHere = hasRefineryHere && hasWarFacHere;
				CellClass* pNomadicResourceGoalCell
					= canFerryNomadNow ? pIslandResourceGoalCell : pDistantUncolonizedLandOreCell;

				// Siege creep goal for a secondary yard with no free field left (slot 81): 18 cells short
				// of the nearest known enemy structure on this landmass, when it is 36+ cells away.
				CellClass* pSiegeCreepGoalCell = nullptr;
				const bool isSecondaryYard = (pHouse->NumConYards >= 2 && pOwnBld != pHouse->ConYards.GetItem(0));

				if (isSecondaryYard && pNomadicResourceGoalCell == nullptr && SlotGate(scores[81], 75, 81))
				{
					CellStruct nearestEnemy = CellStruct::Empty;
					int nearestEnemyDist = INT_MAX;

					for (auto const pEB : BuildingClass::Array)
					{
						if (!pEB || pEB->InLimbo || !pEB->IsAlive || !pEB->Owner || !pEB->Type || pEB->Type->Insignificant
							|| pEB->Type->InvisibleInGame || pEB->Owner == pHouse || pHouse->IsAlliedWith(pEB->Owner)
							|| pEB->Owner->IsNeutral() || !IsEnemyBuildingKnown(pHouse, pEB))
						{
							continue;
						}

						const CellStruct ec = pEB->GetMapCoords();

						if (MapClass::Instance.GetMovementZoneType(ec, MovementZone::Normal, pEB->OnBridge) != yz)
							continue;

						const int d = std::max(std::abs(ec.X - yc.X), std::abs(ec.Y - yc.Y));

						if (d < nearestEnemyDist)
						{
							nearestEnemyDist = d;
							nearestEnemy = ec;
						}
					}

					if (nearestEnemy != CellStruct::Empty && nearestEnemyDist >= 36)
					{
						const int step = nearestEnemyDist - 18;
						const CellStruct goal { static_cast<short>(yc.X + ((nearestEnemy.X - yc.X) * step) / nearestEnemyDist),
							static_cast<short>(yc.Y + ((nearestEnemy.Y - yc.Y) * step) / nearestEnemyDist) };
						auto const pGoalCell = MapClass::Instance.TryGetCellAt(goal);

						if (pGoalCell && MapClass::Instance.GetMovementZoneType(goal, MovementZone::Normal, false) == yz)
							pSiegeCreepGoalCell = pGoalCell;
					}
				}

				// A secondary ConYard leapfrogs once its outpost stands on its own (a refinery, or a
				// siege outpost, with two defenses and five structures): the outpost stays as a
				// building anchor and the yard moves on to the next free field, or creeps toward the
				// enemy when no field is left. Regrowing ore and MCV availability do not hold it back.
				const bool isSecondaryConYardReadyToHop = isSecondaryYard
					&& (hasRefineryHere || pSiegeCreepGoalCell != nullptr) && defsDroppedHere >= 2 && structuresDroppedHere >= 5
					&& (pNomadicResourceGoalCell != nullptr || pSiegeCreepGoalCell != nullptr);

				if (pNomadicResourceGoalCell == nullptr && isSecondaryConYardReadyToHop)
					pNomadicResourceGoalCell = pSiegeCreepGoalCell;

				const bool isSingleConYardDepletedHop = (pHouse->NumConYards == 1 && !canBuildAdditionalMCV
					&& !hasUnharvestedOreNearThisYard && pHouse->OwnedBuildings >= 10 && pHouse->NumWarFactories >= 1
					&& hasRefineryOrFactoryHere && structuresDroppedHere >= 4 && pNomadicResourceGoalCell != nullptr);

				if (!busyConstructingBld && pNomadicResourceGoalCell != nullptr
					&& (isSecondaryConYardReadyToHop || isSingleConYardDepletedHop))
				{
					UndeployBuildingToVehicle(pOwnBld, pNomadicResourceGoalCell->MapCoords);
					handledConYardAction = true;
				}
			}

			// A damaged mobile factory is always repaired (it is never sell-dodged, see below).
			if (IsMobileFactoryBuilding(pOwnBld->Type) && pOwnBld->Type->ClickRepairable && hpPct < 0.82 && currentCredits > 600
				&& !pOwnBld->IsBeingRepaired)
			{
				pOwnBld->SetRepairState(1);
			}

			// Deployed mobile factory (NeuralAI.MobileFactory.inl): production uses it like any other
			// factory (UpdatePrimaryFactories picks the one closest to the front). It flees (packs up for
			// the main base or the nearest quiet hub) when the enemy it sees around outweighs our local
			// defense or its health fell sharply. Otherwise, after standing MobileFactoryDwellFrames and
			// MobileFactoryPackCooldownFrames after the house's last pack-up, it moves when its spot is
			// elsewhere: the front moved 30+ cells past it (leapfrog, only with an idle queue), the main
			// base needs hidden production, or a known enemy defense now reaches it. Only buildings
			// that pack back into their vehicle (UndeploysInto) ever move; the MCVRedeploy option only
			// concerns ConYards.
			if (!handledConYardAction && IsMobileFactoryBuilding(pOwnBld->Type) && hpPct >= 0.12
				&& pOwnBld->CurrentMission != Mission::Construction && pOwnBld->CurrentMission != Mission::Selling
				&& IsAITickWindow(90, 20))
			{
				auto& mobileMacro = GetMacroState(pHouse);
				const int now = Unsorted::CurrentFrame;
				const CellStruct fc = pOwnBld->GetMapCoords();
				const int fz = ObjectZone(pOwnBld, MovementZone::Normal);

				if (mobileMacro.MobileFactoryStands.size() > 16)
				{
					std::erase_if(mobileMacro.MobileFactoryStands,
						[now](const auto& entry) { return now - entry.second.HealthFrame > 9000; });
				}

				auto const [standIt, justDeployed] = mobileMacro.MobileFactoryStands.try_emplace(pOwnBld->UniqueID,
					NeuralMobileFactoryStand { now, hpPct, now });
				auto& stand = standIt->second;

				if (justDeployed)
					mobileMacro.MobileFactoryDeployFrame = now;

				const int stoodFor = now - stand.Since;
				const bool healthDropped = stand.Health - hpPct >= 0.2;

				if (now - stand.HealthFrame >= 600 || hpPct > stand.Health)
				{
					stand.Health = hpPct;
					stand.HealthFrame = now;
				}

				const int threat = VisibleEnemyThreatNear(pHouse, fc, 10);
				const int localDefense = OwnDefenseNear(pHouse, fc, 10);
				const bool flee = (threat >= 80 && threat * 4 > localDefense * 5 && (stoodFor >= 300 || hpPct < 0.5))
					|| (healthDropped && stoodFor >= 150);
				const bool producing = pOwnBld->Factory != nullptr && pOwnBld->Factory->Object != nullptr;
				auto const pVehicle = pOwnBld->Type->UndeploysInto;

				if (flee)
				{
					const CellStruct retreat
						= FindMobileFactoryRetreat(pHouse, fc, fz, CanHouseFerryTypeAcrossWater(pHouse, pVehicle));

					if (retreat != CellStruct::Empty && retreat.DistanceFrom(fc) >= 8.0)
					{
						mobileMacro.MobileFactoryAvoidCell = fc;
						mobileMacro.MobileFactoryAvoidFrame = now;
						mobileMacro.MobileFactoryPackCell = fc;
						mobileMacro.MobileFactoryPackTarget = retreat;
						mobileMacro.MobileFactoryPackReason = MobileFactoryReasonNone;
						mobileMacro.MobileFactoryLastPackFrame = now;
						UndeployBuildingToVehicle(pOwnBld, retreat);
						handledConYardAction = true;
					}
				}
				else if (threat == 0 && stoodFor >= MobileFactoryDwellFrames
					&& now - mobileMacro.MobileFactoryLastPackFrame >= MobileFactoryPackCooldownFrames)
				{
					const bool inDefenseReach = IsCellInKnownEnemyDefenseReach(pHouse, fc, 1);
					const MobileFactorySpot spot
						= FindMobileFactorySpot(pHouse, pOwnBld->Type, pOwnBld, fc, fz, pOwnBld->UniqueID);
					// Evasive moves (hidden production, a defense in reach) may interrupt production, as
					// fleeing does; a planned move (leapfrog, a better spot) waits until the queue is idle.
					const bool evasive = inDefenseReach || spot.Reason == MobileFactoryReasonHidden;
					const double minMove = evasive ? 10.0 : MobileFactoryLeapfrogCells;

					if (spot.Reason != MobileFactoryReasonNone && (evasive || !producing) && spot.Cell.DistanceFrom(fc) >= minMove)
					{
						if (inDefenseReach)
						{
							mobileMacro.MobileFactoryAvoidCell = fc;
							mobileMacro.MobileFactoryAvoidFrame = now;
						}

						mobileMacro.MobileFactoryLastPackFrame = now;
						mobileMacro.MobileFactoryPackCell = fc;
						mobileMacro.MobileFactoryPackTarget = spot.Cell;
						mobileMacro.MobileFactoryPackReason = spot.Reason;
						UndeployBuildingToVehicle(pOwnBld, spot.Cell);
						handledConYardAction = true;
					}
				}
			}

			// A mobile factory is not sell-dodged: selling it only packs it back into its vehicle, which
			// unfolds again at once. It is repaired instead (wrench repair below).
			if (!handledConYardAction && hpPct < sellDodgeThresh && IsDeployedSiegeBuildingType(pOwnBld->Type)
				&& pOwnBld->CurrentMission != Mission::Selling && pOwnBld->QueuedMission != Mission::Selling)
			{
				// A deployed siege unit about to die packs up and drives home instead (selling one
				// packs it up in place: Phobos BuildingExt::CanUndeployOnSell).
				UndeployBuildingToVehicle(pOwnBld, homeCoords);
			}
			else if (!handledConYardAction && hpPct < sellDodgeThresh && !pOwnBld->Type->ConstructionYard
				&& !IsMobileFactoryBuilding(pOwnBld->Type) && !pOwnBld->Type->Unsellable && !IsDeployedSiegeBuildingType(pOwnBld->Type)
				&& !pOwnBld->Type->ResourceGatherer && pOwnBld->Type->Enslaves == nullptr && pOwnBld->Type->Cost >= 500
				&& pOwnBld->CurrentMission != Mission::Selling)
			{
				// Classic C&C Sell-Dodge right before destruction!
				pOwnBld->SetArchiveTarget(nullptr);
				pOwnBld->Sell(1);
			}
			else if (!handledConYardAction && pOwnBld->Type->UndeploysInto != nullptr
				&& !pOwnBld->Type->ConstructionYard && !pOwnBld->Type->Refinery && !pOwnBld->Type->ResourceGatherer
				&& !pOwnBld->Type->ResourceDestination && pOwnBld->Type->Enslaves == nullptr
				&& !pOwnBld->Type->BaseNormal && !IsTrueBaseStructureTarget(pOwnBld->Type)
				&& (pOwnBld->Type->IsBaseDefense
					|| (pOwnBld->Type->GetWeapon(0) != nullptr && pOwnBld->Type->GetWeapon(0)->WeaponType != nullptr))
				&& pOwnBld->Target == nullptr && !GetMacroState(pHouse).PlanMembers.contains(pOwnBld->UniqueID)
				&& (IsAITickWindow(150, 30)))
			{
				// Slot 57 (Re-Mobilize Deployed Siege Unit), not while a plan holds it: When a deployed Siege
				// Chopper, Tick Tank, or Nod Artillery has finished combat and no
				// enemies are within 12 cells, pack back into a mobile vehicle so it
				// advances with the army!
				bool enemyInRange = false;
				ForEachTechnoNear(pOwnBld->GetMapCoords(), 16, [&](TechnoClass* pETech) -> bool
				{
					if (!pETech->Owner)
						return true;

					if (pETech->Owner == pHouse || pHouse->IsAlliedWith(pETech->Owner) || !IsCellVisibleToAI(pHouse, pETech->GetCell()) || pETech->Owner->IsNeutral())
						return true;

					if (pOwnBld->DistanceFrom(pETech) <= 3072)
						enemyInRange = true;

					return !enemyInRange;
				});

				if (!enemyInRange)
				{
					const CellStruct rallyGoal = pWaveTargetCell ? pWaveTargetCell->MapCoords : homeCoords;
					UndeployBuildingToVehicle(pOwnBld, rallyGoal);
				}
			}
			else if (pOwnBld->Type->ClickRepairable)
			{
				// Slot 59 (Smart Wrench Building Repair): Activate engine wrench repair
				// on damaged key structures when credits are healthy, and pause repair
				// if credits drop critically low!
				if (hpPct < 0.82 && hpPct >= sellDodgeThresh && SlotGate(scores[59], 80, 59) && currentCredits > 1200
					&& !pOwnBld->IsBeingRepaired)
				{
					pOwnBld->SetRepairState(1);
				}
				else if (pOwnBld->IsBeingRepaired && currentCredits < 450)
				{
					pOwnBld->SetRepairState(0);
				}
			}
		}
	}

	// CABAL / Faction Research & Tech-Upgrade Unlocks (PreUpgr_SW* /
	// LimboUpgrade_SW* where DisableableFromShell=no, excluding Tunnel Delivery
	// SWs)
	// Generic research selection (no type names): among the ready research SWs (limbo
	// delivery / limbo kill / unit conversion), pick the one whose upgrade improves the most
	// of OUR current army, and only if we can afford it. A "start" SW (delivers a limbo
	// marker ID) is linked to its "finish" SW (kills that marker ID and converts units), so
	// both stages of a two-step research are valued by the units the finish will upgrade.
	// Auto-fired SWs are left to the engine and AI-forbidden SWs are never touched.
	if (homeCoords != CellStruct::Empty && (IsAITickWindow(120, 25)))
	{
		auto countOwnedConvertible = [pHouse](const SWTypeExt* pExt) -> int
		{
			int count = 0;

			for (auto const& group : pExt->Convert_Pairs)
			{
				for (auto const pTech : TechnoClass::Array)
				{
					if (!pTech || pTech->Owner != pHouse || !pTech->IsAlive || pTech->InLimbo)
						continue;

					auto const pType = pTech->GetTechnoType();

					if (std::find(group.FromTypes.begin(), group.FromTypes.end(), pType) != group.FromTypes.end())
						count++;
				}
			}

			return count;
		};

		// Unlock value: limbo buildings delivered by a SW can be prerequisites. Count the types
		// this house could own that list one of them as prerequisite and are not buildable yet
		// (combat units and factories weigh more), so research that opens new tech is chosen.
		auto unlockValueOf = [pHouse](const std::vector<BuildingTypeClass*>& delivered) -> int
		{
			if (delivered.empty())
				return 0;

			int value = 0;

			auto scanType = [&](TechnoTypeClass* pType)
			{
				if (!pType || pType->TechLevel < 0 || !HouseExt::IsAvailableToHouse(pHouse, pType))
					return;

				bool needsDelivered = false;

				for (const int prereqIdx : pType->Prerequisite)
				{
					if (prereqIdx < 0 || prereqIdx >= BuildingTypeClass::Array.Count)
						continue;

					auto const pPrereq = BuildingTypeClass::Array.GetItem(prereqIdx);

					if (std::find(delivered.begin(), delivered.end(), pPrereq) != delivered.end())
					{
						needsDelivered = true;
						break;
					}
				}

				if (!needsDelivered || HouseExt::PrerequisitesMet(pHouse, pType))
					return;

				auto const pW = pType->GetWeapon(0);
				const bool isCombat = pW && pW->WeaponType && pW->WeaponType->Damage > 0;
				auto const pBld = abstract_cast<BuildingTypeClass*>(pType);
				const bool isProduction = pBld && (pBld->Factory != AbstractType::None || pBld->SuperWeapon != -1);
				value += isCombat || isProduction ? 60 : 25;
			};

			for (auto const pT : UnitTypeClass::Array)
				scanType(pT);
			for (auto const pT : InfantryTypeClass::Array)
				scanType(pT);
			for (auto const pT : AircraftTypeClass::Array)
				scanType(pT);
			for (auto const pT : BuildingTypeClass::Array)
				scanType(pT);

			return std::min(value, 600);
		};

		SuperClass* pBestResearch = nullptr;
		int bestResearchValue = INT_MIN;
		const int creditsNow = pHouse->Available_Money();

		for (auto const pSuper : pHouse->Supers)
		{
			if (!pSuper || !pSuper->IsReady || !pSuper->Type)
				continue;

			auto const pSWExt = SWTypeExt::Fetch(pSuper->Type);
			const bool isLimboResearchUpgrade = pSWExt
				&& (!pSWExt->LimboDelivery_Types.empty() || !pSWExt->LimboKill_IDs.empty()
					|| !pSWExt->Convert_Pairs.empty());

			if (!isLimboResearchUpgrade || pSuper->Type->DisableableFromShell
				|| pSuper->Type->Type == SuperWeaponType::ChronoWarp || IsTunnelDeliverySuperWeapon(pSuper->Type, pHouse)
				|| !pSWExt->SW_AllowAI || pSWExt->SW_AutoFire)
			{
				continue;
			}

			// Affordability: keep a reserve so research never starves production.
			const int cost = pSWExt->Money_Amount < 0 ? -pSWExt->Money_Amount : 0;

			if (cost > 0 && creditsNow < cost + 800)
				continue;

			int value = 100 + countOwnedConvertible(pSWExt) * 40;
			std::vector<BuildingTypeClass*> deliveredTypes(pSWExt->LimboDelivery_Types.begin(), pSWExt->LimboDelivery_Types.end());

			// Two-step research: add the value of the finishing stage(s) linked by limbo IDs.
			for (auto const pOtherType : SuperWeaponTypeClass::Array)
			{
				auto const pOtherExt = pOtherType ? SWTypeExt::Fetch(pOtherType) : nullptr;

				if (!pOtherExt || pOtherExt == pSWExt || pOtherExt->LimboKill_IDs.empty())
					continue;

				bool linked = false;

				for (const int id : pSWExt->LimboDelivery_IDs)
				{
					if (std::find(pOtherExt->LimboKill_IDs.begin(), pOtherExt->LimboKill_IDs.end(), id)
						!= pOtherExt->LimboKill_IDs.end())
					{
						linked = true;
						break;
					}
				}

				if (linked)
				{
					value += countOwnedConvertible(pOtherExt) * 40;
					deliveredTypes.insert(deliveredTypes.end(), pOtherExt->LimboDelivery_Types.begin(),
						pOtherExt->LimboDelivery_Types.end());
				}
			}

			value += unlockValueOf(deliveredTypes);
			value -= cost / 50;

			if (value > bestResearchValue)
			{
				bestResearchValue = value;
				pBestResearch = pSuper;
			}
		}

		if (pBestResearch)
		{
			pBestResearch->Launch(homeCoords, false);
			pBestResearch->Reset();
		}
	}

	// CABAL / Ares / Antares Subterranean Tunnel Network (ROBOTTUNNEL +
	// CABALTunnelSpecial -> ROBOTCTUNNEL -> ROBOTTUNNEL): Creative Tunnel Exit
	// Placement via Tunnel Delivery SW on unprotected enemy flanks, behind
	// defenses, on islands, or near neutral Techs Two-Way Subterranean Portal
	// Micro: load Infantry, Cyborgs, Engineers & Size<=2 Vehicles at Home Tunnel
	// -> eject at Forward Tunnel!
	if (homeCoords != CellStruct::Empty && (IsAITickWindow(45, 20)))
	{
		BuildingClass* pHomeTunnel = nullptr;
		BuildingClass* pForwardTunnel = nullptr;
		int minHomeTunnelDist = 999999;
		int bestForwardTunnelScore = -999999;
		int ownTunnelCount = 0;

		if (auto const pHomeCell = MapClass::Instance.TryGetCellAt(homeCoords))
		{
			for (auto const pOwnBld : pHouse->Buildings)
			{
				if (!pOwnBld || pOwnBld->InLimbo || !pOwnBld->IsAlive || !pOwnBld->Type
					|| !IsTunnelNetworkBuilding(pOwnBld->Type))
				{
					continue;
				}

				ownTunnelCount++;

				const int dHome = pOwnBld->DistanceFrom(pHomeCell);

				if (dHome < minHomeTunnelDist)
				{
					minHomeTunnelDist = dHome;

					pHomeTunnel = pOwnBld;
				}
			}

			for (auto const pOwnBld : pHouse->Buildings)
			{
				if (!pOwnBld || pOwnBld == pHomeTunnel || pOwnBld->InLimbo || !pOwnBld->IsAlive || !pOwnBld->Type
					|| !IsTunnelNetworkBuilding(pOwnBld->Type))
				{
					continue;
				}

				const int dHome = pOwnBld->DistanceFrom(pHomeCell);
				const int dTarget = pWaveTargetCell ? pOwnBld->DistanceFrom(pWaveTargetCell) : 0;
				const int fwdScore = pWaveTargetCell ? (dHome - dTarget * 2) : dHome;

				if (fwdScore > bestForwardTunnelScore)
				{
					bestForwardTunnelScore = fwdScore;

					pForwardTunnel = pOwnBld;
				}
			}
		}

		// Step 1: If we own 1 Tunnel (or need a 2nd Tunnel endpoint) and our Tunnel
		// Delivery SW (e.g. CABALTunnelSpecial) is ready, dig the remote tunnel
		// endpoint at an unprotected enemy flank, island beachhead, or neutral Tech
		// Building!
		if (ownTunnelCount == 1 && pHomeTunnel)
		{
			for (auto const pSuper : pHouse->Supers)
			{
				if (!pSuper || !pSuper->IsReady || !pSuper->Type || !IsTunnelDeliverySuperWeapon(pSuper->Type, pHouse))
					continue;

				CellStruct bestTunnelCell = CellStruct::Empty;
				int bestTunnelCellScore = -999999;

				// If our single surviving tunnel is far from home (> 18 cells), our
				// Home Tunnel was destroyed -> restore Home Tunnel first!
				if (minHomeTunnelDist > 4608)
				{
					if (CellClass* pHomeRestore = FindSafeIslandLandingCell(pHouse, homeCoords, homeGroundZone, false))
						bestTunnelCell = pHomeRestore->MapCoords;
				}
				else
				{
					// Evaluate visible enemy buildings and neutral capturable Tech
					// Buildings to find an unprotected flank/infiltration spot
					for (auto const pBld : BuildingClass::Array)
					{
						if (!pBld || pBld->InLimbo || !pBld->IsAlive || !pBld->Owner || !pBld->Type
							|| pBld->Type->Immune)
						{
							continue;
						}

						const bool isNeutralTech = (pBld->Owner->IsNeutral() && pBld->Type->Capturable);

						const bool isEnemyBld = (pBld->Owner != pHouse && !pHouse->IsAlliedWith(pBld->Owner)
							&& !pBld->Owner->IsNeutral() && !pBld->Owner->IsObserver() && !pBld->Owner->Defeated);

						if (!isNeutralTech && !isEnemyBld)
							continue;

						auto const pBldCell = pBld->GetCell();

						if (!pBldCell || !IsCellVisibleToAI(pHouse, pBldCell))
							continue;

						const CellStruct bc = pBld->GetMapCoords();
						const int distFromHomeCells = std::abs(bc.X - homeCoords.X) + std::abs(bc.Y - homeCoords.Y);

						if (distFromHomeCells < 20)
							continue;

						const int ringMin = isNeutralTech ? 3 : 6;
						const int ringMax = isNeutralTech ? 6 : 12;

						for (int dy = -ringMax; dy <= ringMax; dy += 3)
						{
							for (int dx = -ringMax; dx <= ringMax; dx += 3)
							{
								const int cheb = std::max(std::abs(dx), std::abs(dy));

								if (cheb < ringMin || cheb > ringMax)
									continue;

								const CellStruct tc { static_cast<short>(bc.X + dx), static_cast<short>(bc.Y + dy) };
								auto const pCand = MapClass::Instance.TryGetCellAt(tc);

								if (!pCand || !IsCellVisibleToAI(pHouse, pCand))
									continue;

								if ((pCand->LandType != LandType::Clear && pCand->LandType != LandType::Road)
									|| pCand->ContainsBridge() || pCand->GetBuilding() != nullptr
									|| pCand->FirstObject != nullptr)
								{
									continue;
								}

								// Count nearby enemy defensive turrets within 9 cells of this
								// flank candidate
								int nearbyEnemyTurrets = 0;

								for (auto const pOtherBld : BuildingClass::Array)
								{
									if (!pOtherBld || pOtherBld->InLimbo || !pOtherBld->IsAlive || !pOtherBld->Owner
										|| !pOtherBld->Type)
									{
										continue;
									}

									if (pOtherBld->Owner == pHouse || pHouse->IsAlliedWith(pOtherBld->Owner) || !IsEnemyBuildingKnown(pHouse, pOtherBld)
										|| pOtherBld->Owner->IsNeutral())
									{
										continue;
									}

									if (pOtherBld->Type->IsBaseDefense && pOtherBld->Type->GetWeapon(0) != nullptr)
									{
										const CellStruct oc = pOtherBld->GetMapCoords();

										if (std::abs(tc.X - oc.X) <= 9 && std::abs(tc.Y - oc.Y) <= 9)
											nearbyEnemyTurrets++;
									}
								}

								int candScore = 400 - nearbyEnemyTurrets * 260;

								if (nearbyEnemyTurrets == 0)
								{
									candScore +=

										450; // Completely unprotected flank / rear blindspot!
								}

								if (isNeutralTech)
								{
									candScore += 380 + (scores[36] > 60 ? 200 : 0);
								}
								else
								{
									if (pBld->Type->ConstructionYard || pBld->Type->SuperWeapon != -1)
									{
										candScore += 420;
									}
									else if (pBld->Type->Factory != AbstractType::None || pBld->Type->PowerBonus > 0
										|| pBld->Type->Refinery)
									{
										candScore += 280;
									}
								}

								const int candZone
									= MapClass::Instance.GetMovementZoneType(tc, MovementZone::Normal, false);

								if (homeGroundZone >= 0 && candZone >= 0 && candZone != homeGroundZone)
									candScore += 480; // Disconnected island portal!

								if (candScore > bestTunnelCellScore)
								{
									bestTunnelCellScore = candScore;

									bestTunnelCell = tc;
								}
							}
						}
					}

					// Fallback: if no visible enemy building yet but pWaveTargetCell is
					// far and visible, place near frontline
					if (bestTunnelCell == CellStruct::Empty && pWaveTargetCell)
					{
						const CellStruct wc = pWaveTargetCell->MapCoords;

						if (std::abs(wc.X - homeCoords.X) + std::abs(wc.Y - homeCoords.Y) >= 22)
						{
							if (CellClass* pFrontCell = FindSafeIslandLandingCell(pHouse, wc, targetIslandZone, false))
							{
								if (IsCellVisibleToAI(pHouse, pFrontCell))
									bestTunnelCell = pFrontCell->MapCoords;
							}
						}
					}
				}

				if (bestTunnelCell != CellStruct::Empty)
				{
					pSuper->Launch(bestTunnelCell, false);
					pSuper->Reset();

					break;
				}
			}
		}

		// Step 2: Active Two-Way Subterranean Tunnel Portal (pHomeTunnel <->
		// pForwardTunnel)
		if (ownTunnelCount >= 2 && pHomeTunnel && pForwardTunnel && pHomeTunnel != pForwardTunnel
			&& pHomeTunnel->DistanceFrom(pForwardTunnel) >= 2560)
		{
			// Keep all non-forward Tunnel mouths in Guard so passengers only unload
			// at pForwardTunnel!
			for (auto const pOwnBld : pHouse->Buildings)
			{
				if (!pOwnBld || pOwnBld == pForwardTunnel || pOwnBld->InLimbo || !pOwnBld->IsAlive || !pOwnBld->Type
					|| !IsTunnelNetworkBuilding(pOwnBld->Type))
				{
					continue;
				}

				if (pOwnBld->CurrentMission == Mission::Unload)
					pOwnBld->QueueMission(Mission::Guard, false);
			}

			if (pForwardTunnel->CurrentMission != Mission::Unload)
				pForwardTunnel->QueueMission(Mission::Unload, true);

			// Check if there is a high-value capturable building near pForwardTunnel
			// (within 22 cells = 5632 leptons)
			BuildingClass* pFlankCaptureTarget = nullptr;
			BuildingClass* pFlankAttackTarget = nullptr;
			int bestCapDist = 5632;
			int bestAtkDist = 6144;

			for (auto const pBld : BuildingClass::Array)
			{
				if (!pBld || pBld->InLimbo || !pBld->IsAlive || !pBld->Owner || !pBld->Type || pBld->Type->Immune)
					continue;

				if (pBld->Owner == pHouse || pHouse->IsAlliedWith(pBld->Owner) || !IsEnemyBuildingKnown(pHouse, pBld) || pBld->Owner->IsObserver()
					|| pBld->Owner->Defeated)
				{
					continue;
				}

				const int dFwd = pBld->DistanceFrom(pForwardTunnel);

				if (dFwd <= bestCapDist && pBld->Type->Capturable
					&& (pBld->Owner->IsNeutral() || pBld->Type->ConstructionYard
						|| pBld->Type->Factory != AbstractType::None || pBld->Type->Refinery
						|| pBld->Type->SuperWeapon != -1 || pBld->Type->TechLevel >= 5))
				{
					bestCapDist = dFwd;

					pFlankCaptureTarget = pBld;
				}

				if (!pBld->Owner->IsNeutral() && dFwd <= bestAtkDist)
				{
					bestAtkDist = dFwd;

					pFlankAttackTarget = pBld;
				}
			}

			// Feed Engineers, Cyborgs, Infantry & Size<=SizeLimit Vehicles near
			// pHomeTunnel into the subterranean portal!
			int sentIntoTunnel = 0;
			const int maxBatchEnter = std::clamp(static_cast<int>(pHomeTunnel->Type->Passengers), 4, 10);

			if (pFlankCaptureTarget)
			{
				for (auto const pInf : InfantryClass::Array)
				{
					if (!pInf || pInf->Owner != pHouse || pInf->InLimbo || !pInf->IsAlive || !pInf->Type
						|| !pInf->Type->Engineer)
					{
						continue;
					}

					// If Engineer emerged from pForwardTunnel, immediately capture the
					// flank target!
					if (pInf->DistanceFrom(pForwardTunnel) <= 2560)
					{
						OrderFootToEnter(pInf, pFlankCaptureTarget);
					}
					else if (pInf->DistanceFrom(pHomeTunnel) <= 7168 && pInf->CurrentMission != Mission::Enter)
					{
						OrderFootToEnter(pInf, pHomeTunnel);
						sentIntoTunnel++;
					}
				}
			}

			for (auto const pI : idleCombatInf)
			{
				if (sentIntoTunnel >= maxBatchEnter)
					break;

				if (pI->DistanceFrom(pForwardTunnel) <= 2304 && (pFlankAttackTarget || pWaveTargetCell))
				{
					if (pFlankAttackTarget)
					{
						pI->ClearMegaMissionData();
						pI->SetTarget(pFlankAttackTarget);

						pI->QueueMission(Mission::Attack, true);
					}
					else
					{
						OrderFootToAttackMove(pI, pWaveTargetCell);
					}
				}
				else if (pI->DistanceFrom(pHomeTunnel) <= 6144
					&& pI->DistanceFrom(pForwardTunnel) > pHomeTunnel->DistanceFrom(pForwardTunnel) - 1024)
				{
					OrderFootToEnter(pI, pHomeTunnel);
					sentIntoTunnel++;
				}
			}

			if (pHomeTunnel->Type->UnitAbsorb)
			{
				const int maxAllowedSize = std::max(1, static_cast<int>(pHomeTunnel->Type->SizeLimit));

				for (auto const pV : idleCombatUnits)
				{
					if (sentIntoTunnel >= maxBatchEnter)
						break;

					if (IsAmphibiousOrAirTransport(pV->Type)
						|| static_cast<int>(pV->Type->TechnoTypeClass::Size) > maxAllowedSize)
					{
						continue;
					}

					if (pV->DistanceFrom(pForwardTunnel) <= 2304 && (pFlankAttackTarget || pWaveTargetCell))
					{
						if (pFlankAttackTarget)
						{
							pV->ClearMegaMissionData();
							pV->SetTarget(pFlankAttackTarget);

							pV->QueueMission(Mission::Attack, true);
						}
						else
						{
							OrderFootToAttackMove(pV, pWaveTargetCell);
						}
					}
					else if (pV->DistanceFrom(pHomeTunnel) <= 6144
						&& pV->DistanceFrom(pForwardTunnel) > pHomeTunnel->DistanceFrom(pForwardTunnel) - 1024)
					{
						OrderFootToEnter(pV, pHomeTunnel);
						sentIntoTunnel++;
					}
				}
			}
		}
	}

	// Frontline Primary Factory Switching & Charged Superweapon Execution
	if (pWaveTargetCell && (IsAITickWindow(150, 30)))
	{
		BuildingClass* pBestWarFac = nullptr;
		BuildingClass* pBestBarracks = nullptr;
		int bestWarDist = 999999;
		int bestBarDist = 999999;

		for (auto const pBld : pHouse->Buildings)
		{
			if (!pBld || pBld->InLimbo || !pBld->IsAlive || !pBld->Type)
				continue;

			if (pBld->Type->Factory == AbstractType::UnitType && !pBld->Type->Naval)
			{
				const int d = pBld->DistanceFrom(pWaveTargetCell);

				if (d < bestWarDist)
				{
					bestWarDist = d;

					pBestWarFac = pBld;
				}
			}
			else if (pBld->Type->Factory == AbstractType::InfantryType)
			{
				const int d = pBld->DistanceFrom(pWaveTargetCell);

				if (d < bestBarDist)
				{
					bestBarDist = d;

					pBestBarracks = pBld;
				}
			}
		}

		if (false && pBestWarFac && !pBestWarFac->IsPrimaryFactory)
		{
			for (auto const pBld : pHouse->Buildings)
			{
				if (pBld && pBld->Type && pBld->Type->Factory == AbstractType::UnitType && !pBld->Type->Naval)
					pBld->IsPrimaryFactory = (pBld == pBestWarFac);
			}
		}

		if (false && pBestBarracks && !pBestBarracks->IsPrimaryFactory)
		{
			for (auto const pBld : pHouse->Buildings)
			{
				if (pBld && pBld->Type && pBld->Type->Factory == AbstractType::InfantryType)
					pBld->IsPrimaryFactory = (pBld == pBestBarracks);
			}
		}

		{
			SuperClass* pChronoWarp = pHouse->FindSuperWeapon(SuperWeaponType::ChronoWarp);

			// Honor [AI] RandomizeSuperWeaponPriority (same tag as the vanilla AI path in
			// HouseExt::AI_TryFireSW_CooldownGroupAware): shuffle the evaluation order with the
			// synchronized scenario PRNG, so multiplayer clients stay in sync.
			std::vector<SuperClass*> supersInOrder;

			for (auto const pSuper : pHouse->Supers)
				supersInOrder.push_back(pSuper);

			if (RulesExt::Global()->RandomizeSuperWeaponPriority.Get() && supersInOrder.size() > 1)
			{
				for (int i = static_cast<int>(supersInOrder.size()) - 1; i > 0; --i)
					std::swap(supersInOrder[i], supersInOrder[ScenarioClass::Instance->Random.RandomRanged(0, i)]);
			}

			for (auto const pSuper : supersInOrder)
			{
				if (pSuper && pSuper->Type && !pSuper->IsReady)
					GetMacroState(pHouse).SuperReadySinceFrame.erase(pSuper->Type->GetArrayIndex());

				if (!pSuper || !pSuper->IsReady || !pSuper->Type)
					continue;

				if (!GameModeOptionsClass::Instance.SWAllowed && pSuper->Type->DisableableFromShell)
					continue;

				// Generic (no type names): auto-fired SWs belong to the engine and AI-forbidden ones
				// are not for us (hidden SWs may be AI-only on purpose, so they stay allowed).
				if (auto const pGateExt = SWTypeExt::Fetch(pSuper->Type))
				{
					if (pGateExt->SW_AutoFire || !pGateExt->SW_AllowAI)
						continue;
				}

				if (IsTunnelDeliverySuperWeapon(pSuper->Type, pHouse))
					continue;

				if (auto const pSWExt = SWTypeExt::Fetch(pSuper->Type))
				{
					if (!pSWExt->LimboDelivery_Types.empty() || !pSWExt->LimboKill_IDs.empty()
						|| !pSWExt->Convert_Pairs.empty())
					{
						continue;
					}
				}

				const SuperWeaponType swType = pSuper->Type->Type;

				if (swType == SuperWeaponType::ChronoWarp)
					continue; // Fired as the second step of ChronoSphere

				// A) IRON CURTAIN & FORCE SHIELD: Defensive Emergency Save OR Offensive
				// Invulnerable Tank Assault
				if (swType == SuperWeaponType::IronCurtain || swType == SuperWeaponType::ForceShield)
				{
					CellStruct icTargetCell = CellStruct::Empty;

					// Defensive save: check if our own OR an ALLIED ConYard, Superweapon,
					// or War Factory is under heavy attack (< 75% HP)
					for (auto const pFriendlyBld : BuildingClass::Array)
					{
						if (!pFriendlyBld || pFriendlyBld->InLimbo || !pFriendlyBld->IsAlive || !pFriendlyBld->Owner
							|| !pFriendlyBld->Type || pFriendlyBld->IsIronCurtained())
						{
							continue;
						}

						if (!pHouse->IsAlliedWith(pFriendlyBld->Owner) || pFriendlyBld->Owner->IsNeutral())
							continue;

						const bool isCriticalBld = pFriendlyBld->Type->ConstructionYard
							|| pFriendlyBld->Type->Factory == AbstractType::UnitType
							|| pFriendlyBld->Type->SuperWeapon != -1;

						if (isCriticalBld && pFriendlyBld->GetHealthPercentage() < 0.75)
						{
							icTargetCell = pFriendlyBld->GetMapCoords();

							break;
						}
					}

					// Offensive Invulnerability (Slot 55): cast Iron Curtain on our own
					// OR an ALLIED attacking tank cluster near enemy
					if (icTargetCell == CellStruct::Empty && swType == SuperWeaponType::IronCurtain)
					{
						const int minCluster = (SlotGate(scores[55], 120, 55)) ? 1 : 2;
						int bestClusterCount = 0;

						for (auto const pU : UnitClass::Array)
						{
							if (!pU || !pU->Owner || !pHouse->IsAlliedWith(pU->Owner) || pU->Owner->IsNeutral()
								|| pU->InLimbo || !pU->IsAlive || !pU->Type || pU->IsIronCurtained())
							{
								continue;
							}

							if (pU->Type->Harvester || pU->Type->ResourceGatherer || pU->Type->Organic)
								continue;

							const bool isEngagingEnemy = (pU->Target != nullptr)
								|| (pVisibleEnemyBld && pU->DistanceFrom(pVisibleEnemyBld) <= 4096);

							if (!isEngagingEnemy)
								continue;

							const CellStruct uCell = pU->GetMapCoords();
							int neighbors = 0;

							for (auto const pOtherU : UnitClass::Array)
							{
								if (!pOtherU || !pOtherU->Owner || !pHouse->IsAlliedWith(pOtherU->Owner)
									|| pOtherU->InLimbo || !pOtherU->IsAlive || !pOtherU->Type)
								{
									continue;
								}

								if (pOtherU->Type->Harvester || pOtherU->Type->Organic)
									continue;

								const CellStruct oCell = pOtherU->GetMapCoords();

								if (std::abs(uCell.X - oCell.X) <= 1 && std::abs(uCell.Y - oCell.Y) <= 1)
									neighbors++;
							}

							if (neighbors >= minCluster && neighbors > bestClusterCount)
							{
								bestClusterCount = neighbors;

								icTargetCell = uCell;
							}
						}
					}

					if (icTargetCell != CellStruct::Empty)
					{
						pSuper->Launch(icTargetCell, false);
						pSuper->Reset();

						break;
					}

					continue;
				}

				// B) CHRONOSPHERE + CHRONOWARP:
				//    Teleport Iron-Curtained tank squad or Expansion MCV to enemy
				// base / island Teleport Enemy Player A's tanks into Enemy Player
				// B's base to cause mutual attrition in FFA Drown Enemy Ground
				// Tanks/MCVs by teleporting them into Water Destroy Enemy Naval
				// Warships by teleporting them onto dry Land
				if (swType == SuperWeaponType::ChronoSphere && pChronoWarp)
				{
					CellStruct chronoSrc = CellStruct::Empty;
					CellStruct chronoDst = CellStruct::Empty;

					// Find a known water cell (for drowning enemy ground tanks) and a
					// known clear land cell (for beaching enemy ships)
					CellStruct knownWaterCell = CellStruct::Empty;
					CellStruct knownDryLandCell = CellStruct::Empty;

					if (homeCoords != CellStruct::Empty)
					{
						for (int dy = -40; dy <= 40; dy += 4)
						{
							for (int dx = -40; dx <= 40; dx += 4)
							{
								const CellStruct tc { static_cast<short>(homeCoords.X + dx),
									static_cast<short>(homeCoords.Y + dy) };

								auto const pC = MapClass::Instance.TryGetCellAt(tc);

								if (!pC || !IsCellVisibleToAI(pHouse, pC) || pC->GetBuilding() != nullptr)
									continue;

								if (knownWaterCell == CellStruct::Empty && pC->LandType == LandType::Water
									&& !pC->ContainsBridge())
								{
									knownWaterCell = tc;
								}

								if (knownDryLandCell == CellStruct::Empty && pC->LandType == LandType::Clear
									&& pC->FirstObject == nullptr && std::abs(dx) >= 12)
								{
									knownDryLandCell = tc;
								}
							}
						}
					}

					// Mode 1: Teleport our own Iron-Curtained tank squad or distant
					// Expansion MCV directly to the target beachhead!
					for (auto const pU : UnitClass::Array)
					{
						if (!pU || pU->Owner != pHouse || pU->InLimbo || !pU->IsAlive || !pU->Type)
							continue;

						const bool isExpansionMCV = (pU->Type->DeploysInto && pU->Type->DeploysInto->ConstructionYard
							&& pHouse->NumConYards >= 1);

						const bool isIronCurtainedTank
							= (pU->IsIronCurtained() && !pU->Type->Harvester && !pU->Type->Organic);

						if ((isIronCurtainedTank || isExpansionMCV) && pWaveTargetCell)
						{
							CellClass* pDropCell = FindSafeIslandLandingCell(
								pHouse, pWaveTargetCell->MapCoords, targetIslandZone, isExpansionMCV);

							if (pDropCell && pU->DistanceFrom(pDropCell) >= 5120)
							{
								chronoSrc = pU->GetMapCoords();

								chronoDst = pDropCell->MapCoords;

								break;
							}
						}
					}

					// Mode 2, 3 & 4: Offensive Chronoshift against visible enemy
					// vehicles/ships
					if (chronoSrc == CellStruct::Empty)
					{
						// Check if there is a SECOND rival enemy house with a visible
						// building (for Enemy-vs-Enemy FFA attrition!)
						CellStruct rivalEnemyBaseCell = CellStruct::Empty;
						int rivalEnemyHouseIdx = -1;

						for (auto const pBld : BuildingClass::Array)
						{
							if (!pBld || pBld->InLimbo || !pBld->IsAlive || !pBld->Owner || !pBld->Type
								|| pBld->Type->Immune)
							{
								continue;
							}

							auto const pOwn = pBld->Owner;

							if (pOwn == pHouse || pHouse->IsAlliedWith(pOwn) || pOwn->IsNeutral() || pOwn->IsObserver()
								|| pOwn->Defeated)
							{
								continue;
							}

							if (pOwn->ArrayIndex != pHouse->EnemyHouseIndex && pBld->GetCell()
								&& IsCellVisibleToAI(pHouse, pBld->GetCell()))
							{
								CellClass* pAdj = FindSafeIslandLandingCell(pHouse, pBld->GetMapCoords(), -1, false);

								if (pAdj)
								{
									rivalEnemyBaseCell = pAdj->MapCoords;

									rivalEnemyHouseIdx = pOwn->ArrayIndex;

									break;
								}
							}
						}

						for (auto const pEnemyU : UnitClass::Array)
						{
							if (!pEnemyU || pEnemyU->InLimbo || !pEnemyU->IsAlive || !pEnemyU->Owner || !pEnemyU->Type)
								continue;

							if (pEnemyU->Owner == pHouse || pHouse->IsAlliedWith(pEnemyU->Owner)
								|| pEnemyU->Owner->IsNeutral())
							{
								continue;
							}

							if (pEnemyU->IsIronCurtained() || pEnemyU->Type->Teleporter || pEnemyU->Type->Organic)
								continue;

							auto const pECell = pEnemyU->GetCell();

							if (!pECell || !IsCellVisibleToAI(pHouse, pECell))
								continue;

							const bool isPureGroundUnit = !pEnemyU->Type->Naval
								&& pEnemyU->Type->SpeedType != SpeedType::Hover
								&& pEnemyU->Type->MovementZone != MovementZone::Amphibious;

							const bool isPureNavalShip = pEnemyU->Type->Naval
								&& pEnemyU->Type->SpeedType != SpeedType::Hover
								&& pEnemyU->Type->MovementZone != MovementZone::Amphibious;

							// Mode 2: Pit Enemy A's ground tanks directly inside Enemy B's
							// base to trigger mutual attrition!
							if (isPureGroundUnit && rivalEnemyBaseCell != CellStruct::Empty
								&& pEnemyU->Owner->ArrayIndex != rivalEnemyHouseIdx)
							{
								chronoSrc = pEnemyU->GetMapCoords();

								chronoDst = rivalEnemyBaseCell;

								break;
							}

							// Mode 3 (Slot 55): Drown Enemy ground tanks/MCVs in water!
							const int drownMinCost = std::clamp(700 - static_cast<int>(scores[55]), 450, 950);

							if (isPureGroundUnit && knownWaterCell != CellStruct::Empty
								&& pEnemyU->Type->Cost >= drownMinCost)
							{
								chronoSrc = pEnemyU->GetMapCoords();

								chronoDst = knownWaterCell;

								break;
							}

							// Mode 4: Beach Enemy naval warships onto dry land!
							if (isPureNavalShip && knownDryLandCell != CellStruct::Empty)
							{
								chronoSrc = pEnemyU->GetMapCoords();

								chronoDst = knownDryLandCell;

								break;
							}
						}
					}

					if (chronoSrc != CellStruct::Empty && chronoDst != CellStruct::Empty)
					{
						pSuper->Launch(chronoSrc, false);
						pSuper->Reset();
						pSuper->ChronoMapCoords = chronoSrc;
						pChronoWarp->ChronoMapCoords = chronoSrc;
						pChronoWarp->Launch(chronoDst, false);
						pChronoWarp->Reset();

						break;
					}

					continue;
				}

				// C) YURI GENETIC MUTATOR: Mutate enemy infantry clusters into Brutes!
				if (swType == SuperWeaponType::GeneticMutator)
				{
					CellStruct bestMutateCell = CellStruct::Empty;
					int bestInfCluster = 1;

					for (auto const pEnemyInf : InfantryClass::Array)
					{
						if (!pEnemyInf || pEnemyInf->InLimbo || !pEnemyInf->IsAlive || !pEnemyInf->Owner
							|| !pEnemyInf->Type)
						{
							continue;
						}

						if (pEnemyInf->Owner == pHouse || pHouse->IsAlliedWith(pEnemyInf->Owner)
							|| pEnemyInf->Owner->IsNeutral())
						{
							continue;
						}

						if (pEnemyInf->GetCell() && IsCellVisibleToAI(pHouse, pEnemyInf->GetCell()))
						{
							const CellStruct ic = pEnemyInf->GetMapCoords();
							int cnt = 0;

							for (auto const pOtherInf : InfantryClass::Array)
							{
								if (!pOtherInf || pOtherInf->InLimbo || !pOtherInf->IsAlive || !pOtherInf->Owner)
									continue;

								if (pOtherInf->Owner == pHouse || pHouse->IsAlliedWith(pOtherInf->Owner) || !IsCellVisibleToAI(pHouse, pOtherInf->GetCell()))
									continue;

								const CellStruct oc = pOtherInf->GetMapCoords();

								if (std::abs(ic.X - oc.X) <= 1 && std::abs(ic.Y - oc.Y) <= 1)
									cnt++;
							}

							if (cnt > bestInfCluster)
							{
								bestInfCluster = cnt;

								bestMutateCell = ic;
							}
						}
					}

					if (bestMutateCell != CellStruct::Empty && MaySuperweaponHit(pHouse, bestMutateCell))
					{
						pSuper->Launch(bestMutateCell, false);
						pSuper->Reset();

						break;
					}
				}

				// D) YURI PSYCHIC DOMINATOR: Permanently capture clusters of expensive
				// enemy tanks/MCVs while crushing nearby structures!
				if (swType == SuperWeaponType::PsychicDominator)
				{
					CellStruct bestDomCell = CellStruct::Empty;
					int bestDomValue = 1200;

					for (auto const pEnemyU : UnitClass::Array)
					{
						if (!pEnemyU || pEnemyU->InLimbo || !pEnemyU->IsAlive || !pEnemyU->Owner || !pEnemyU->Type)
							continue;

						if (pEnemyU->Owner == pHouse || pHouse->IsAlliedWith(pEnemyU->Owner)
							|| pEnemyU->Owner->IsNeutral())
						{
							continue;
						}

						if (pEnemyU->GetCell() && IsCellVisibleToAI(pHouse, pEnemyU->GetCell())
							&& !pEnemyU->Type->ImmuneToPsionics)
						{
							const CellStruct uc = pEnemyU->GetMapCoords();
							int val = 0;

							for (auto const pOtherU : UnitClass::Array)
							{
								if (!pOtherU || pOtherU->InLimbo || !pOtherU->IsAlive || !pOtherU->Owner
									|| !pOtherU->Type)
								{
									continue;
								}

								if (pOtherU->Owner == pHouse || pHouse->IsAlliedWith(pOtherU->Owner) || !IsCellVisibleToAI(pHouse, pOtherU->GetCell()))
									continue;

								const CellStruct oc = pOtherU->GetMapCoords();

								if (std::abs(uc.X - oc.X) <= 1 && std::abs(uc.Y - oc.Y) <= 1)
									val += pOtherU->Type->Cost;
							}

							if (val > bestDomValue)
							{
								bestDomValue = val;

								bestDomCell = uc;
							}
						}
					}

					if (bestDomCell != CellStruct::Empty && MaySuperweaponHit(pHouse, bestDomCell))
					{
						pSuper->Launch(bestDomCell, false);
						pSuper->Reset();

						break;
					}
				}

				// E) Reconnaissance Superweapons (Psychic Reveal / Spy Plane): Uncover
				// shrouded enemy base candidates!
				if (swType == SuperWeaponType::PsychicReveal || swType == SuperWeaponType::SpyPlane)
				{
					const CellStruct reconCell = pFallbackEnemyCell
						? pFallbackEnemyCell->MapCoords
						: (pVisibleEnemyBld ? pVisibleEnemyBld->GetMapCoords() : CellStruct::Empty);
					if (reconCell != CellStruct::Empty)
					{
						pSuper->Launch(reconCell, false);
						pSuper->Reset();
						break;
					}

					continue;
				}

				// F) Standard Offensive/Support Superweapons (Nuke, Lightning Storm,
				// Ion Cannon, ParaDrop, etc.)
				if (pVisibleEnemyBld)
				{
					// Superweapon + army (point 37): while our wave is on its way to this target, hold
					// the ready weapon (at most 1800 frames) until >= 3 of our units are within 15
					// cells of it, so the blast lands as the army arrives.
					auto& swMacro = GetMacroState(pHouse);
					const int swIndex = pSuper->Type->GetArrayIndex();
					auto const readyIt = swMacro.SuperReadySinceFrame.try_emplace(swIndex, Unsorted::CurrentFrame).first;
					const CellStruct swTarget = pVisibleEnemyBld->GetMapCoords();
					const bool waveHeadingThere = swMacro.LastWaveTargetCell != CellStruct::Empty
						&& Unsorted::CurrentFrame - swMacro.LastWaveDispatchFrame <= 900
						&& swMacro.LastWaveTargetCell.DistanceFrom(swTarget) <= 12.0;

					if (waveHeadingThere && Unsorted::CurrentFrame - readyIt->second < 1800 && SlotGate(scores[55], -100, 55))
					{
						int arrived = 0;

						for (auto const pOwnU : UnitClass::Array)
						{
							if (pOwnU && pOwnU->Owner == pHouse && pOwnU->IsAlive && !pOwnU->InLimbo && pOwnU->Type
								&& !pOwnU->Type->Naval && pOwnU->GetMapCoords().DistanceFrom(swTarget) <= 15.0)
							{
								arrived++;
							}
						}

						if (arrived < 3)
							continue;
					}

					// Never on a truce partner (Expert 3 diplomacy): keep the charge.
					if (!MaySuperweaponHit(pHouse, pVisibleEnemyBld->GetMapCoords()))
						break;

					swMacro.SuperReadySinceFrame.erase(swIndex);
					pSuper->Launch(pVisibleEnemyBld->GetMapCoords(), false);
					pSuper->Reset();

					break;
				}
			}
		}
	}
}


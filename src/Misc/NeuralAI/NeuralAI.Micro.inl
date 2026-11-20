// NeuralAI.Micro.inl: Tactical and special operations micro (one function).
// Part of NeuralAI.cpp (included in order; one translation unit).

namespace
{
	// Defined in NeuralAI.Scouting.inl.
	bool IsActiveScout(HouseClass* pHouse, DWORD uid);
	CellStruct PickRevealTarget(HouseClass* pHouse, int radius);
	CellStruct PickNavalSweepCell(HouseClass* pHouse, FootClass* pShip, const std::vector<CellStruct>& taken);
}

// InfantryClass::Scatter runs the locomotor right away (0x51D478), so a scattered unit can leave its
// cell and be relinked into another cell's object list inside the call. Walking FirstObject/NextObject
// while scattering then follows the moved unit into the other list and can revisit units forever (the
// game frame froze on "(4) The Honeymoon"). Copy the cell's foot units first and act on the copy.
namespace
{
	std::vector<CellStruct> FindRemnantSearchCells(HouseClass* pHouse, size_t most);
	void NoteRemnantLeads(HouseClass* pHouse);
	size_t KeepRemnantSearchers(HouseClass* pHouse, std::vector<UnitClass*>& idleUnits, std::vector<InfantryClass*>& idleInf);
	void NoteRemnantSearcher(HouseClass* pHouse, const FootClass* pFoot, const CellStruct& place);
	bool MayLogRemnantNoPlace(HouseClass* pHouse);

	// Whether a cell stands on high ground over the ground of a zone: a cell of that zone
	// CellClass::BridgeLevels or more below it within CliffScanCells (sampled every 2 cells). The
	// ground does not change, so the answer is kept for the match, by zone and cell.
	bool IsHighGroundOverZone(const CellStruct& cell, int level, int zone)
	{
		static int cacheSerial = -1;
		static std::unordered_map<long long, bool> cache;

		if (cacheSerial != NeuralMatchSerial)
		{
			cacheSerial = NeuralMatchSerial;
			cache.clear();
		}

		if (level < CellClass::BridgeLevels || zone < 0)
			return false;

		const long long key = (static_cast<long long>(zone) << 32) | (static_cast<long long>(static_cast<unsigned short>(cell.X)) << 16)
			| static_cast<unsigned short>(cell.Y);

		if (auto const itKnown = cache.find(key); itKnown != cache.end())
			return itKnown->second;

		bool high = false;

		for (int dy = -CliffScanCells; dy <= CliffScanCells && !high; dy += 2)
		{
			for (int dx = -CliffScanCells; dx <= CliffScanCells && !high; dx += 2)
			{
				const CellStruct belowCell { static_cast<short>(cell.X + dx), static_cast<short>(cell.Y + dy) };
				auto const pBelow = dx * dx + dy * dy <= CliffScanCells * CliffScanCells ? MapClass::Instance.TryGetCellAt(belowCell) : nullptr;

				high = pBelow && pBelow->Level <= level - CellClass::BridgeLevels
					&& MapClass::Instance.GetMovementZoneType(belowCell, MovementZone::Normal, false) == zone;
			}
		}

		cache.emplace(key, high);
		return high;
	}
}

// Land before islands (IslandMcvWaitFrames, NeuralAI.State.inl).
namespace
{
	// An island expansion MCV whose ride is under way: it boards a transport, a transport operation
	// carries it, or a carryall flies to it. A ferry seat reserved for it alone is no ride: the loader
	// kept seats for MCVs whose transport never came.
	bool IsIslandMcvRideUnderWay(HouseClass* pHouse, UnitClass* pUnit)
	{
		if (pUnit->CurrentMission == Mission::Enter || pUnit->QueuedMission == Mission::Enter
			|| IsInTransportOp(pHouse, pUnit->UniqueID))
		{
			return true;
		}

		for (auto const& entry : AirPickups)
		{
			const int age = Unsorted::CurrentFrame - entry.second.Frame;

			if (entry.second.CargoID == pUnit->UniqueID && age >= 0 && age <= 2700)
				return true;
		}

		for (auto const pAir : AircraftClass::Array)
		{
			if (pAir && pAir->Owner == pHouse && pAir->IsAlive && !pAir->InLimbo && pAir->Type && pAir->Type->Carryall
				&& pAir->Destination == pUnit)
			{
				return true;
			}
		}

		return false;
	}

	// The yards island expansion MCVs unfolded while no ride came that still stand (a yard of ours
	// within 3 cells of the cell, or an entry too new for the yard to be up), IslandWaitYardFrames at
	// most; the others are dropped.
	int PruneIslandWaitYards(HouseClass* pHouse)
	{
		auto& yards = GetMacroState(pHouse).IslandWaitYards;
		const int now = Unsorted::CurrentFrame;

		std::erase_if(yards, [pHouse, now](const std::pair<CellStruct, int>& entry)
		{
			if (now < entry.second || now - entry.second > IslandWaitYardFrames)
				return true;

			if (now - entry.second <= 300)
				return false;

			for (int i = 0; i < pHouse->ConYards.Count; ++i)
			{
				auto const pYard = pHouse->ConYards.GetItem(i);

				if (pYard && pYard->IsAlive && !pYard->InLimbo
					&& std::max(std::abs(pYard->GetMapCoords().X - entry.first.X), std::abs(pYard->GetMapCoords().Y - entry.first.Y)) <= 3)
				{
					return false;
				}
			}

			return true;
		});

		return static_cast<int>(yards.size());
	}

	// The crossing a packed-up yard's MCV would ask the transport operations for (economy client,
	// DispatchFootToTargetOrTransport) is shut after an operation ended with no transport.
	bool IsIslandCrossingBlocked(HouseClass* pHouse, int fromZone, int toZone)
	{
		if (!TransportOpsEnabled)
			return false;

		auto const& blocked = GetMacroState(pHouse).CrossingBlockedUntil;
		auto const it = blocked.find(CrossingBlockKey(NeuralOpClient::Economy, fromZone, toZone));
		const int now = Unsorted::CurrentFrame;

		return it != blocked.end() && now < it->second && it->second - now <= OpNoTransportRetryFrames;
	}

	// An empty transport of ours, within IslandWaitFerryCells of the yard and free of other work, that
	// can carry the yard's MCV from the yard to the island goal (the old ferry loader's filter, and
	// empty).
	UnitClass* FindIdleIslandMcvFerry(HouseClass* pHouse, const UnitTypeClass* pMcvType, const CellStruct& yardCell,
		const CellStruct& goalCell)
	{
		for (auto const pT : UnitClass::Array)
		{
			if (!pT || pT->Owner != pHouse || pT->InLimbo || !pT->IsAlive || !pT->Type || !IsAmphibiousOrAirTransport(pT->Type)
				|| static_cast<int>(pT->Type->SizeLimit) < static_cast<int>(pMcvType->TechnoTypeClass::Size)
				|| pT->Type->Passengers < static_cast<int>(pMcvType->TechnoTypeClass::Size) || pT->Passengers.NumPassengers > 0
				|| IsPlanMember(pHouse, pT->UniqueID) || IsInTransportOp(pHouse, pT->UniqueID))
			{
				continue;
			}

			const CellStruct tc = pT->GetMapCoords();

			if (std::max(std::abs(tc.X - yardCell.X), std::abs(tc.Y - yardCell.Y)) <= IslandWaitFerryCells
				&& CanTransportTypeLink(pT->Type, yardCell, goalCell))
			{
				return pT;
			}
		}

		return nullptr;
	}
}

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

namespace
{
	// Where a transport stands to load. Passengers cannot step onto every cell: where nothing on
	// foot, tracks or wheels crosses the beach, the engine refuses to load there and sends the
	// transport towards the passenger, so the transport moves on to the nearest free cell of the
	// island that its passengers can reach. A flying one that hangs over a cell for
	// FerryLandingFrames without touching down while passengers wait tries to land on another cell
	// near, one ring farther each time. True while it is on its way to such a cell.
	bool MoveTransportToLoadingCell(HouseClass* pHouse, UnitClass* pTransport, const CellStruct& from, int landZone, bool passengersWaiting)
	{
		// A jumpjet does not touch down on a beach: over one it flies on to a random cell next to
		// it and tries again (JumpjetLocomotionClass, descending state at 0x54C550, water and beach
		// alike), so a flying transport called to soldiers on the shore hung over them for good.
		const bool flyer = pTransport->Type->JumpJet || pTransport->Type->MovementZone == MovementZone::Fly;

		// The free cell of the land nearest `from`, minRing to maxRing out, the whole square row by row
		// (FindWaitCellNear, SquareNearest): land of the zone on foot with no building, off the beach for
		// a flyer, clear by the transport's own movement.
		auto const nearestLoadingCell = [&](int minRing, int maxRing) -> CellClass*
		{
			NeuralWaitAsker asker {};
			asker.Speed = pTransport->Type->SpeedType;
			asker.Movement = pTransport->Type->MovementZone;
			asker.ZoneMovement = static_cast<int>(MovementZone::Normal);
			asker.Zone = landZone;
			NeuralWaitSearch search {};
			search.MinRing = minRing;
			search.MaxRing = maxRing;
			search.Pick = NeuralWaitPick::SquareNearest;
			search.MetricTo = from;

			return FindWaitCellNear(from, search, asker,
				WaitInMap | WaitLand | WaitNoBuilding | WaitZone | WaitClearOfUnits | (flyer ? WaitNotBeach : 0u),
				[](const CellStruct&, const CellClass*) { return false; });
		};

		auto const sendTo = [pTransport](CellClass* pCell)
		{
			if (pTransport->Destination != pCell || pTransport->CurrentMission != Mission::Move)
			{
				pTransport->SetDestination(pCell, true);
				pTransport->QueueMission(Mission::Move, true);
				NoteOrderCaller(pTransport, "move");
			}
		};

		constexpr int doorSearchCells = 4;
		auto const pFromCell = MapClass::Instance.TryGetCellAt(from);
		const bool overBeach = flyer && pFromCell && pFromCell->LandType == LandType::Beach;

		if (overBeach || MapClass::Instance.GetMovementZoneType(from, MovementZone::Normal, false) != landZone)
		{
			auto const pDoorCell = nearestLoadingCell(0, overBeach ? FerryBeachSearchCells : doorSearchCells);

			if (!pDoorCell)
				return false;

			if (pTransport->Destination != pDoorCell)
				CountDecision(NeuralDecision::FerryDoorCellMove, pHouse->ArrayIndex);

			sendTo(pDoorCell);

			return true;
		}

		auto& landings = GetMacroState(pHouse).FerryLandings;

		if (!IsFerryAloft(pTransport))
		{
			landings.erase(pTransport->UniqueID);
			return false;
		}

		auto& landing = landings[pTransport->UniqueID];
		const int now = Unsorted::CurrentFrame;

		if (landing.Target != CellStruct::Empty)
		{
			auto const pGoing = abstract_cast<CellClass*>(pTransport->Destination);

			if (pGoing && pGoing->MapCoords == landing.Target)
				return true;

			landing.Target = CellStruct::Empty;
		}

		// Hovering drifts it over the cells around: only a real move starts the count again.
		if (landing.Cell == CellStruct::Empty || landing.Cell.DistanceFrom(from) > FerryLandingDriftCells || !passengersWaiting)
		{
			landing.Cell = from;
			landing.Since = now;
			return false;
		}

		if (now - landing.Since < FerryLandingFrames)
			return false;

		const int ring = std::min(landing.Tries + 1, FerryLandingMaxRing);
		auto const pLandingCell = nearestLoadingCell(ring, FerryLandingMaxRing);
		landing.Since = now;

		if (!pLandingCell)
			return false;

		landing.Tries++;
		landing.Target = pLandingCell->MapCoords;
		sendTo(pLandingCell);
		CountDecision(NeuralDecision::FerryLandingRetried, pHouse->ArrayIndex);

		return true;
	}
}

void NeuralAI::ExecuteTacticalAndSpecOpsMicro(HouseClass* pHouse, const std::array<int16_t, OutputVectorSize>& scores)
{
	// Phase stopwatch (telemetry only): which part of the micro is heavy.
	NeuralPhaseClock microClock { pHouse ? pHouse->ArrayIndex : -1, NeuralSection::MicroSetup };

	// pAvoid: sites tried before (the idle MCV watchdog); a candidate within 6 cells of one is passed over.
	auto const FindDeployableFoundationNearCellUncached
		= [](HouseClass* pH, BuildingTypeClass* pBldType, const CellStruct& originCell, int requiredZone,
			UnitClass* pDeployerUnit, const std::vector<CellStruct>* pAvoid = nullptr) -> CellStruct
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

					if (pAvoid && std::any_of(pAvoid->begin(), pAvoid->end(), [&cand](const CellStruct& avoid)
						{ return std::max(std::abs(avoid.X - cand.X), std::abs(avoid.Y - cand.Y)) <= 6; }))
					{
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

	// pWhy: what refused the deploy (TEMP diagnostic of the idle watchdog).
	auto const CanDeployerUnpackHere = [](HouseClass* pH, BuildingTypeClass* pBldType, UnitClass* pDeployerUnit,
		const char** pWhy = nullptr) -> bool
	{
		auto const refuse = [pWhy](const char* why)
		{
			if (pWhy)
				*pWhy = why;

			return false;
		};

		if (!pH || !pBldType || !pDeployerUnit)
			return refuse("no deployer");

		const int w = pBldType->GetFoundationWidth();
		const int h = pBldType->GetFoundationHeight(false);
		const CellStruct deployOffset = (w > 2 || h > 2) ? CellStruct { -1, -1 } : CellStruct { 0, 0 };
		const CellStruct topLeftCell = pDeployerUnit->GetMapCoords() + deployOffset;
		if (!NeuralAI::IsCellClearedOfExitCorridors(pH, pBldType, topLeftCell))
			return refuse("an exit corridor of ours");

		bool occupiedByOtherDeployerOrTrans = false;
		for (auto pFoundCell = pBldType->GetFoundationData(true); *pFoundCell != CellStruct { 0x7FFF, 0x7FFF };
			++pFoundCell)
		{
			const CellStruct fc = topLeftCell + *pFoundCell;
			auto const pFC = MapClass::Instance.TryGetCellAt(fc);
			if (!pFC)
				return refuse("off the map");

			std::vector<FootClass*> cellFoots;
			CollectCellFoots(pFC, cellFoots);

			for (auto const pFoot : cellFoots)
			{
				if (pFoot == pDeployerUnit || !pFoot->IsAlive || pFoot->InLimbo || !pFoot->Owner)
					continue;

				// An ally's unit standing on the foundation blocks the deploy as much as ours (match
				// #6198: a Soviet MCV among its ally's units never deployed): it is asked to step aside.
				if (pFoot->Owner != pH)
				{
					if (pH->IsAlliedWith(pFoot->Owner) && pFoot->CurrentMission != Mission::Move)
						pFoot->Scatter(CoordStruct::Empty, true, false);

					continue;
				}

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
			return refuse("another deployer or transport on the foundation");

		if (!UnitExt::CanDeployIntoBuilding(pDeployerUnit))
			return refuse("the engine refuses the foundation");

		return true;
	};

	// Whether a gun of `range` leptons at `from` can hit pTarget: a living, seen object on the ground
	// (not in the air, not a submerged or cloaked ship) within range.
	auto const DeployedGunCanHit = [pHouse](const CoordStruct& from, AbstractClass* pTarget, int range) -> bool
	{
		auto const pTechno = abstract_cast<TechnoClass*>(pTarget);

		if (!pTechno || !pTechno->IsAlive || pTechno->InLimbo || range <= 0 || pTechno->IsInAir()
			|| !IsCellVisibleToAI(pHouse, pTechno->GetCell()))
		{
			return false;
		}

		if (auto const pType = pTechno->GetTechnoType();
			pType && pType->Naval && (pType->Underwater || pType->Cloakable || pTechno->CloakState != CloakState::Uncloaked))
		{
			return false;
		}

		return pTechno->GetCoords().DistanceFrom(from) <= range;
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

	// The MCV danger rule (McvDanger*, NeuralAI.State.inl): the visible armed enemy units that turn a packed
	// MCV back, each with its combat value (GetCombatValueOfType by health), gathered once a tick.
	std::vector<std::pair<CellStruct, int>> mcvDangerUnitsCache;
	bool mcvDangerUnitsReady = false;

	auto const GetMcvDangerUnits = [&]() -> const std::vector<std::pair<CellStruct, int>>&
	{
		if (mcvDangerUnitsReady)
			return mcvDangerUnitsCache;

		mcvDangerUnitsReady = true;

		for (auto const pET : TechnoClass::Array)
		{
			if (!pET || pET->InLimbo || !pET->IsAlive || !pET->Owner || pET->WhatAmI() == AbstractType::Building)
				continue;

			if (pET->Owner == pHouse || pHouse->IsAlliedWith(pET->Owner) || !IsCellVisibleToAI(pHouse, pET->GetCell()) || pET->Owner->IsNeutral())
				continue;

			auto const pETType = pET->GetTechnoType();
			auto const pETUnitType = abstract_cast<UnitTypeClass*>(pETType);

			if (!pETType || (pETUnitType && pETUnitType->Harvester) || pETType->ResourceGatherer || IsAmphibiousOrAirTransport(pETType))
				continue;

			auto const pEWpn = pET->GetWeapon(0) ? pET->GetWeapon(0)->WeaponType : nullptr;

			if (!pEWpn || (pETType->Naval && (pEWpn->Range <= 2048 || (pEWpn->Projectile && !pEWpn->Projectile->AG))))
				continue;

			mcvDangerUnitsCache.emplace_back(pET->GetMapCoords(),
				std::max(1, static_cast<int>(GetCombatValueOfType(pETType, false) * pET->GetHealthPercentage())));
		}

		return mcvDangerUnitsCache;
	};

	// The combat value of those units within `cells` of `cell` (0: none near).
	auto const McvDangerUnitValueNear = [&](const CellStruct& cell, int cells) -> int
	{
		int value = 0;

		for (auto const& [unitCell, unitValue] : GetMcvDangerUnits())
		{
			if (std::max(std::abs(cell.X - unitCell.X), std::abs(cell.Y - unitCell.Y)) <= cells)
				value += unitValue;
		}

		return value;
	};

	// Hold the ground (McvFallback*, NeuralAI.State.inl): a cell out of every danger.
	auto const IsMcvFallbackSafe = [&](const CellStruct& cell, const std::vector<std::pair<CellStruct, int>>& dangers) -> bool
	{
		return !IsInMcvDanger(dangers, cell, McvSiteDangerMarginCells)
			&& McvDangerUnitValueNear(cell, McvDangerUnitCells + McvFallbackMarginCells) == 0;
	};

	// Our defenses whose gun reaches `cell` and our armed units within McvCoverUnitCells of it, by combat
	// value and health.
	auto const McvOwnCoverValue = [pHouse](const CellStruct& cell) -> int
	{
		int value = 0;

		for (auto const pBld : pHouse->Buildings)
		{
			if (!pBld || pBld->InLimbo || !pBld->IsAlive || !pBld->Type || !pBld->Type->IsBaseDefense)
				continue;

			auto const pWS = pBld->GetWeapon(0);
			auto const pWpn = pWS ? pWS->WeaponType : nullptr;

			if (!pWpn || pWpn->Range <= 0)
				continue;

			const CellStruct bc = pBld->GetMapCoords();

			if (std::max(std::abs(cell.X - bc.X), std::abs(cell.Y - bc.Y)) <= pWpn->Range / 256 + 1)
				value += static_cast<int>(GetCombatValueOfType(pBld->Type, false) * pBld->GetHealthPercentage());
		}

		for (auto const pT : TechnoClass::Array)
		{
			if (!pT || pT->Owner != pHouse || pT->InLimbo || !pT->IsAlive)
				continue;

			const AbstractType kind = pT->WhatAmI();

			if (kind != AbstractType::Unit && kind != AbstractType::Infantry)
				continue;

			auto const pTType = pT->GetTechnoType();
			auto const pTUnitType = abstract_cast<UnitTypeClass*>(pTType);

			if (!pTType || pTType->ResourceGatherer || (pTUnitType && (pTUnitType->Harvester || pTUnitType->DeploysInto))
				|| !pT->GetWeapon(0) || !pT->GetWeapon(0)->WeaponType)
			{
				continue;
			}

			const CellStruct tc = pT->GetMapCoords();

			if (std::max(std::abs(cell.X - tc.X), std::abs(cell.Y - tc.Y)) <= McvCoverUnitCells)
				value += static_cast<int>(GetCombatValueOfType(pTType, false) * pT->GetHealthPercentage());
		}

		return value;
	};

	// The nearest cell out of every danger where the yard of pMcv fits (McvFallback*, NeuralAI.State.inl),
	// near a building of ours before the rest; Empty when none of the McvFallbackTries best has room.
	auto const FindMcvFallbackSpot = [&](UnitClass* pMcv, BuildingTypeClass* pYardType, int zone,
		const std::vector<std::pair<CellStruct, int>>& dangers) -> CellStruct
	{
		if (!pMcv || !pYardType || zone < 0)
			return CellStruct::Empty;

		const CellStruct from = pMcv->GetMapCoords();
		std::vector<CellStruct> ownCells;

		for (auto const pBld : pHouse->Buildings)
		{
			if (pBld && !pBld->InLimbo && pBld->IsAlive)
				ownCells.push_back(pBld->GetMapCoords());
		}

		struct Candidate
		{
			int Score;
			CellStruct Cell;
		};

		std::vector<Candidate> candidates;

		for (int dy = -McvFallbackSearchCells; dy <= McvFallbackSearchCells; dy += 2)
		{
			for (int dx = -McvFallbackSearchCells; dx <= McvFallbackSearchCells; dx += 2)
			{
				const CellStruct cell { static_cast<short>(from.X + dx), static_cast<short>(from.Y + dy) };

				if (!MapClass::Instance.TryGetCellAt(cell)
					|| MapClass::Instance.GetMovementZoneType(cell, MovementZone::Normal, false) != zone
					|| !IsMcvFallbackSafe(cell, dangers))
				{
					continue;
				}

				int nearOwn = INT_MAX;

				for (auto const& ownCell : ownCells)
					nearOwn = std::min(nearOwn, std::max(std::abs(cell.X - ownCell.X), std::abs(cell.Y - ownCell.Y)));

				candidates.push_back({ std::max(std::abs(dx), std::abs(dy)) * 10 + (nearOwn <= McvFallbackOwnCells ? 0 : McvFallbackOwnPenalty), cell });
			}
		}

		std::sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b)
		{
			if (a.Score != b.Score)
				return a.Score < b.Score;

			return a.Cell.Y != b.Cell.Y ? a.Cell.Y < b.Cell.Y : a.Cell.X < b.Cell.X;
		});

		int tries = 0;

		for (auto const& candidate : candidates)
		{
			if (tries++ >= McvFallbackTries)
				break;

			const CellStruct spot = FindDeployableFoundationNearCell(pHouse, pYardType, candidate.Cell, zone, pMcv);

			if (spot != CellStruct::Empty && IsMcvFallbackSafe(spot, dangers))
				return spot;
		}

		return CellStruct::Empty;
	};

	// TEMP diagnostic: each step of the hold-the-ground rule (McvHoldLogLines a match).
	auto const logMcvHold = [pHouse](UnitClass* pMcv, const char* what, const CellStruct& at, int enemyValue, int ownValue)
	{
		static int holdLogSerial = -1;
		static int holdLogCount = 0;

		if (holdLogSerial != NeuralMatchSerial)
		{
			holdLogSerial = NeuralMatchSerial;
			holdLogCount = 0;
		}

		if (holdLogCount++ >= McvHoldLogLines)
			return;

		const CellStruct here = pMcv->GetMapCoords();
		Debug::Log("[NeuralAI] House %d MCV holds: %s %u %s (%d,%d), from (%d,%d), enemy value %d near, ours %d, %d yards, frame %d.\n",
			pHouse->ArrayIndex, pMcv->Type->ID, pMcv->UniqueID, what, at.X, at.Y, here.X, here.Y, enemyValue, ownValue,
			pHouse->NumConYards, Unsorted::CurrentFrame);
	};

	// A free start location (StartSpot*, NeuralAI.State.inl): start i is defined, no house of ours or of an
	// ally started there, it was seen empty or its house was defeated, no known building of anyone stands
	// within StartSpotTakenCells and an ore field lies within StartSpotOreCells. Out: the start's cell.
	auto const IsFreeStartSpot = [pHouse](int i, CellStruct& outCell) -> bool
	{
		if (!ScenarioClass::Instance->IsDefinedWaypoint(i))
			return false;

		const CellStruct startCell = ScenarioClass::Instance->GetWaypointCoords(i);
		bool leftByDefeated = false;

		for (auto const pOther : HouseClass::Array)
		{
			if (!pOther || pOther->IsNeutral() || pOther->BaseSpawnCell == CellStruct::Empty || pOther->BaseSpawnCell.DistanceFrom(startCell) > 6.0)
				continue;

			if (pOther == pHouse || pHouse->IsAlliedWith(pOther))
				return false;

			leftByDefeated = leftByDefeated || pOther->Defeated;
		}

		auto const pStartCell = MapClass::Instance.TryGetCellAt(startCell);

		if (!pStartCell)
			return false;

		const bool seenEmpty = (GetMacroState(pHouse).ScoutedStartMask & (1u << i)) != 0 || IsCellVisibleToAI(pHouse, pStartCell);

		if (!seenEmpty && !leftByDefeated)
			return false;

		for (auto const pBld : BuildingClass::Array)
		{
			if (!pBld || pBld->InLimbo || !pBld->IsAlive || !pBld->Owner || pBld->Owner->IsNeutral() || !pBld->Type
				|| pBld->Type->Insignificant || pBld->Type->InvisibleInGame || pBld->Type->Wall)
			{
				continue;
			}

			const bool known = pBld->Owner == pHouse || pHouse->IsAlliedWith(pBld->Owner) || IsEnemyBuildingKnown(pHouse, pBld);
			const CellStruct bc = pBld->GetMapCoords();

			if (known && std::max(std::abs(bc.X - startCell.X), std::abs(bc.Y - startCell.Y)) <= StartSpotTakenCells)
				return false;
		}

		const bool oreNear = std::any_of(GetOreFields().begin(), GetOreFields().end(), [&](const auto& field)
		{
			return std::max(std::abs(field.Cell.X - startCell.X), std::abs(field.Cell.Y - startCell.Y)) <= StartSpotOreCells;
		});

		if (!oreNear)
			return false;

		outCell = startCell;

		return true;
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
	// landmass (footZone != targetZone) is handed to a transport operation, which
	// lands it on the target's ground; there its client gives it its mission again.
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
			// Ground the unit itself can walk is no island crossing.
			if (IsSameGroundForFoot(pFoot, gc))
				return false;

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
		= [&](FootClass* pFoot, AbstractClass* pTarget, Mission desiredMission, bool useSetTarget = false,
			const char* callerFile = __builtin_FILE(), int callerLine = __builtin_LINE()) -> bool
	{
		if (!pFoot || !pTarget)
			return false;

		// A unit of a transport operation is on its way already: the operation carries it and gives it
		// back to its client on the target's ground.
		if (IsHeldByTransportOp(pFoot))
			return true;

		// Another code holds the unit for its own goal (order leases).
		if (!MayOrderUnit(pFoot, CellClass::Coord2Cell(pTarget->GetCoords()), callerFile, callerLine))
			return false;

		auto const pFootType = pFoot->GetTechnoType();
		if (!pFootType)
			return false;

		const CellStruct footCoords = pFoot->GetMapCoords();
		int footZone = MapClass::Instance.GetMovementZoneType(footCoords, MovementZone::Normal, pFoot->OnBridge);

		// A unit on a cell of no ground zone (one the zone map calls impassable for a vehicle) stands
		// for the ground next to it. It was taken for a unit on another island and asked for a
		// transport that never came: 257 of the 453 transport waits that ran out in 40 matches, on
		// maps without water too.
		for (int r = 1; r <= 2 && !IsGroundZone(footZone); ++r)
		{
			for (int dy = -r; dy <= r && !IsGroundZone(footZone); ++dy)
			{
				for (int dx = -r; dx <= r && !IsGroundZone(footZone); ++dx)
				{
					if (std::abs(dx) != r && std::abs(dy) != r)
						continue;

					const CellStruct beside { static_cast<short>(footCoords.X + dx), static_cast<short>(footCoords.Y + dy) };
					const int nearZone = MapClass::Instance.GetMovementZoneType(beside, MovementZone::Normal, false);

					if (IsGroundZone(nearZone))
						footZone = nearZone;
				}
			}
		}

		if (footZone == NoZoneId)
			footZone = -1;

		const CellStruct targetCoords = CellClass::Coord2Cell(pTarget->GetCoords());
		auto const pTargetObject = abstract_cast<ObjectClass*>(pTarget);
		int targetZone = MapClass::Instance.GetMovementZoneType(
			targetCoords, MovementZone::Normal, pTargetObject && pTargetObject->OnBridge);

		// Its own way: the unit walks when its own movement zone joins both cells (user: a transport
		// only for what the unit cannot reach itself).
		const int ownFootZone = MapClass::Instance.GetMovementZoneType(footCoords, pFootType->MovementZone, pFoot->OnBridge);
		const bool walksThere = IsGroundZone(ownFootZone) && ownFootZone == MapClass::Instance.GetMovementZoneType(
			targetCoords, pFootType->MovementZone, pTargetObject && pTargetObject->OnBridge);

		// A cell of a standing bridge is on the deck's ground, not on the water or the chasm under
		// it: units asked for a transport to cells of the bridge next to them and waited at its
		// mouth (match #8920).
		if (targetZone <= 1 && !(pTargetObject && pTargetObject->OnBridge))
		{
			auto const pTargetCell = MapClass::Instance.TryGetCellAt(targetCoords);

			if (pTargetCell && (pTargetCell->ContainsBridge() || pTargetCell->ContainsBridgeEx()))
				targetZone = MapClass::Instance.GetMovementZoneType(targetCoords, MovementZone::Normal, true);
		}

		// A building answers for the ground around it (its own cells may carry another zone).
		if (footZone >= 0 && targetZone != footZone && IsBuildingEnterableFromZone(abstract_cast<BuildingClass*>(pTarget), footZone))
			targetZone = footZone;

		// A target on no ground (a ship off the shore, a unit at a cliff foot, a cell with no zone) is
		// reached from the ground next to it: ours when it touches it (the unit walks up and fires from
		// the shore), else the nearest other one. With no ground around it no transport can land the
		// unit by it. Units asked for a transport to such targets and waited on the beach until they
		// gave up: 43 of 219 ferry wait timeouts in 41 matches were for zone 1, 11 for no zone.
		CellStruct targetGround = targetCoords;

		if (!IsGroundZone(targetZone))
		{
			int nearestZone = -1;
			bool touchesFootGround = false;

			for (int r = 1; r <= FerryTargetGroundCells && !touchesFootGround; ++r)
			{
				for (int dy = -r; dy <= r && !touchesFootGround; ++dy)
				{
					for (int dx = -r; dx <= r; ++dx)
					{
						if (std::abs(dx) != r && std::abs(dy) != r)
							continue;

						const CellStruct beside { static_cast<short>(targetCoords.X + dx), static_cast<short>(targetCoords.Y + dy) };
						const int nearZone = MapClass::Instance.GetMovementZoneType(beside, MovementZone::Normal, false);

						if (!IsGroundZone(nearZone))
							continue;

						if (nearZone == footZone)
						{
							touchesFootGround = true;
							break;
						}

						if (nearestZone < 0)
						{
							nearestZone = nearZone;
							targetGround = beside;
						}
					}
				}
			}

			if (touchesFootGround)
			{
				targetZone = footZone;
				targetGround = targetCoords;
			}
			else
			{
				targetZone = nearestZone;
			}
		}

		bool canShootFromHere = false;
		if (useSetTarget || desiredMission == Mission::Attack)
		{
			const auto pWpn = pFootType->GetWeapon(0) ? pFootType->GetWeapon(0)->WeaponType : nullptr;
			if (pWpn && pFoot->DistanceFrom(pTarget) <= pWpn->Range)
				canShootFromHere = true;
		}

		if (footZone < 0 || targetZone < 0 || footZone == targetZone || walksThere || CanFootCrossWaterDirectly(pFoot)
			|| canShootFromHere)
		{
			if (useSetTarget || desiredMission == Mission::Attack)
			{
				pFoot->ClearMegaMissionData();
				pFoot->SetTarget(pTarget);
				pFoot->QueueMission(desiredMission, true);
				NoteOrderCaller(pFoot, "dispatchAttack", callerFile, callerLine);
			}
			else if (desiredMission == Mission::Enter && abstract_cast<TechnoClass*>(pTarget))
			{
				// It notes (and leases the unit for) the code that dispatched it, not this line.
				OrderFootToEnter(pFoot, abstract_cast<TechnoClass*>(pTarget), callerFile, callerLine);
			}
			else if (desiredMission == Mission::AttackMove)
			{
				OrderFootToAttackMove(pFoot, pTarget, callerFile, callerLine);
			}
			else
			{
				pFoot->ClearMegaMissionData();
				pFoot->SetDestination(pTarget, true);
				pFoot->QueueMission(desiredMission, true);
				NoteOrderCaller(pFoot, "dispatchMove", callerFile, callerLine);
			}

			return true;
		}

		// A repairer stays with the vehicles on its own ground (Repairer*): no crossing for it.
		if (auto const pRepairer = abstract_cast<UnitClass*>(pFoot); pRepairer && IsRepairerUnit(pRepairer))
			return false;

		// The transport operations manager takes it across (user: a removal company apart from the
		// client); the unit's client gives it its order again once it stands on the target's ground.
		return RequestTransportOperation(pHouse, NeuralOpClient::Economy, pFoot, targetGround);
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
			&& !IsTransportUnderRepair(pU) && !IsInTransportOp(pHouse, pU->UniqueID)
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

	// Discover uncolonized island Resource (Ore/Gems), buildable Island Land
	// (when home island is full), and Strategic Neutral Tech Building targets
	CellClass* pIslandResourceGoalCell = nullptr;
	int resourceIslandZone = -1;
	// Where pIslandResourceGoalCell comes from: an island with ore, the bare land fallback (no ore,
	// while the home island counts as congested) or the beachhead on an enemy island (offensive).
	enum class IslandGoalKind
	{
		None,
		Ore,
		BareLand,
		EnemyBeachhead
	};
	IslandGoalKind islandGoalKind = IslandGoalKind::None;
	// Harvesters are carried only to where they can unload (user, match #10266: the transport
	// helicopters of a house set seven of its harvesters down on an island with no ore and no
	// refinery, the nearest free land for an expansion, where they stood until they were
	// destroyed). The island goal takes harvesters only with a refinery of ours on that land. A
	// harvester on a land with no refinery and no construction yard of ours is stranded: a
	// carryall brings it back to the nearest refinery.
	std::vector<int> ownRefineryZones;
	std::vector<int> ownYardZones;

	for (auto const pOwnBld : pHouse->Buildings)
	{
		if (!pOwnBld || pOwnBld->InLimbo || !pOwnBld->IsAlive || !pOwnBld->Type)
			continue;

		if (pOwnBld->Type->Refinery || pOwnBld->Type->ResourceDestination)
			ownRefineryZones.push_back(ObjectZone(pOwnBld, MovementZone::Normal));
		else if (pOwnBld->Type->ConstructionYard)
			ownYardZones.push_back(ObjectZone(pOwnBld, MovementZone::Normal));
	}

	auto const zoneHasOwnRefinery = [&ownRefineryZones](int zone)
	{
		return std::find(ownRefineryZones.begin(), ownRefineryZones.end(), zone) != ownRefineryZones.end();
	};
	auto const islandGoalTakesHarvesters = [&]()
	{
		return pIslandResourceGoalCell && resourceIslandZone >= 0 && zoneHasOwnRefinery(resourceIslandZone);
	};
	// An economic crossing only when needed (user approved, 2026-11-19, match #11475: an expansion
	// MCV next to a yard boarded a landing craft for bare land with no ore while its house had 80 free
	// fields at home). A yard MCV crosses to an ore island only when the land it stands on has no free
	// ore field (unitFreeLandField, as the MCV logic asks it) or no room for its yard (noRoomHere), to
	// the bare land fallback never, and to the beachhead on an enemy island (offensive) whatever the
	// fields at home. Never to an island given up for lack of room (NoRoomIslandUntil): a transport
	// takes a yard MCV back from there (FerryMcvNowhere).
	auto const isNoRoomIsland = [pHouse](int zone) -> bool
	{
		auto const& noRoom = GetMacroState(pHouse).NoRoomIslandUntil;
		auto const itNoRoom = noRoom.find(zone);

		return itNoRoom != noRoom.end() && itNoRoom->second > Unsorted::CurrentFrame;
	};
	auto const yardMcvCrossingAllowed = [&](bool noRoomHere, bool freeFieldHere) -> bool
	{
		if (!pIslandResourceGoalCell || isNoRoomIsland(resourceIslandZone))
			return false;

		if (islandGoalKind == IslandGoalKind::EnemyBeachhead)
			return true;

		if (islandGoalKind != IslandGoalKind::Ore)
			return false;

		return noRoomHere || !freeFieldHere;
	};
	// The ground a unit stands on: its cell's zone, or on a cell of no ground (a shore, a cliff foot)
	// the land around it (TransportLandZone), else -1 (as the MCV logic reads it).
	auto const unitLandZone = [](const UnitClass* pU) -> int
	{
		const CellStruct cell = pU->GetMapCoords();
		const int cellZone = MapClass::Instance.GetMovementZoneType(cell, MovementZone::Normal, pU->OnBridge);
		const int landZone = IsGroundZone(cellZone) ? cellZone : TransportLandZone(cell, pU->OnBridge);

		return IsGroundZone(landZone) ? landZone : -1;
	};
	// The free ore field of a unit's land (FindFreeOreFieldCell from its cell and land zone, 14 cells),
	// searched once per unit and frame: the MCV logic, its idle watchdog, the deployer pick and the
	// airlifts all ask it. Lookups by UniqueID in a list filled in the game's unit order.
	auto const unitFreeLandField = [&](UnitClass* pU) -> CellClass*
	{
		static int fieldCacheFrame = -1;
		static int fieldCacheSerial = -1;
		static std::vector<std::pair<DWORD, CellClass*>> fieldCache;

		if (fieldCacheFrame != Unsorted::CurrentFrame || fieldCacheSerial != NeuralMatchSerial)
		{
			fieldCacheFrame = Unsorted::CurrentFrame;
			fieldCacheSerial = NeuralMatchSerial;
			fieldCache.clear();
		}

		for (auto const& [uid, pField] : fieldCache)
		{
			if (uid == pU->UniqueID)
				return pField;
		}

		const int zone = unitLandZone(pU);
		CellClass* const pField = zone >= 0 ? FindFreeOreFieldCell(pHouse, pU->GetMapCoords(), zone, 14) : nullptr;
		fieldCache.emplace_back(pU->UniqueID, pField);

		return pField;
	};
	// A yard MCV with no ground under it nor around it may not cross. The free field is looked for
	// only when the answer depends on it.
	auto const yardMcvMayCrossToIslandGoal = [&](UnitClass* pMcv) -> bool
	{
		const int zone = unitLandZone(pMcv);

		if (zone < 0)
			return false;

		const bool noRoomHere = isNoRoomIsland(zone);

		if (!yardMcvCrossingAllowed(noRoomHere, false))
			return false;

		return noRoomHere || islandGoalKind == IslandGoalKind::EnemyBeachhead || unitFreeLandField(pMcv) == nullptr;
	};
	// A building that counts as a colony on an island (user approved, 2026-11-19: "Sí, los cinco";
	// match #11506): a yard, a refinery or resource destination, or a factory on land, as the island
	// scan reads a colony. A support building, a defense, a power plant, a building on water
	// (IsWaterBuildingType) or one the AI keeps inside its base (AIInnerBase) is no expansion: its
	// deployer deploys at the base.
	auto const isColonyBuilding = [](BuildingTypeClass* pBld) -> bool
	{
		return pBld && !IsWaterBuildingType(pBld) && !BuildingTypeExt::IsAIInnerBase(pBld)
			&& (pBld->ConstructionYard || pBld->Refinery || pBld->ResourceGatherer || pBld->ResourceDestination
				|| pBld->Factory != AbstractType::None);
	};
	// No room for this building on that island: the island given up (NoRoomIslandUntil) or the
	// building type given up there (NoRoomTypeIslandUntil).
	auto const isNoRoomIslandFor = [&](const BuildingTypeClass* pBld, int zone) -> bool
	{
		if (isNoRoomIsland(zone))
			return true;

		if (!pBld)
			return false;

		auto const& typeMarks = GetMacroState(pHouse).NoRoomTypeIslandUntil;
		auto const itMark = typeMarks.find({ pBld->ArrayIndex, zone });

		return itMark != typeMarks.end() && itMark->second > Unsorted::CurrentFrame;
	};
	// A deployer is sent to the island goal only when its building fits there: a legal foundation in
	// the island's zone near the goal (FindDeployableFoundationNearCell). An island it does not fit on
	// is given up for that building type for NoRoomIslandFrames, not for the others.
	auto const deployerFitsOnIslandGoal = [&](UnitClass* pDeployer, BuildingTypeClass* pBld) -> bool
	{
		if (!pDeployer || !pBld || !pIslandResourceGoalCell || !IsGroundZone(resourceIslandZone)
			|| isNoRoomIslandFor(pBld, resourceIslandZone))
		{
			return false;
		}

		if (FindDeployableFoundationNearCell(pHouse, pBld, pIslandResourceGoalCell->MapCoords, resourceIslandZone, pDeployer)
			!= CellStruct::Empty)
		{
			return true;
		}

		GetMacroState(pHouse).NoRoomTypeIslandUntil[{ pBld->ArrayIndex, resourceIslandZone }]
			= Unsorted::CurrentFrame + NoRoomIslandFrames;
		CountDecision(NeuralDecision::DeployerGoalNoFit, pHouse->ArrayIndex);

		return false;
	};
	// Whether a deployer (not a yard MCV) expands with its building: one predicate for the pick of the
	// home expansion deployer, the carryall airlifts and the deployer logic, so a deployer is never
	// ferried to an island the deployer logic then sends it back from. A colony building, a land
	// factory of a kind the house owns already, or a refinery while the house owns one and wants more
	// (scores[30]).
	int houseRefineries = 0;
	int houseWarFactories = 0;
	int houseBarracks = 0;

	for (auto const pOwnBld : pHouse->Buildings)
	{
		if (!pOwnBld || pOwnBld->InLimbo || !pOwnBld->IsAlive || !pOwnBld->Type || pOwnBld->Type->Insignificant
			|| pOwnBld->Type->InvisibleInGame)
		{
			continue;
		}

		if (pOwnBld->Type->Refinery || pOwnBld->Type->ResourceDestination)
			houseRefineries++;
		else if (pOwnBld->Type->Factory == AbstractType::UnitType && !pOwnBld->Type->Naval)
			houseWarFactories++;
		else if (pOwnBld->Type->Factory == AbstractType::InfantryType)
			houseBarracks++;
	}

	auto const deployerExpandsWith = [&](BuildingTypeClass* pBld) -> bool
	{
		if (!isColonyBuilding(pBld) || pBld->ConstructionYard)
			return false;

		if (pBld->Factory == AbstractType::UnitType)
			return houseWarFactories >= 1;

		if (pBld->Factory == AbstractType::InfantryType)
			return houseBarracks >= 1;

		return (pBld->Refinery || pBld->ResourceDestination) && houseRefineries >= 1 && scores[30] > 450;
	};
	// A deployer the idle deployer watchdog sent to the main base is no one's to ferry.
	auto const deployerSentToMainBase = [pHouse](DWORD uid) -> bool
	{
		auto const& sites = GetMacroState(pHouse).DeployerSites;
		auto const itSite = sites.find(uid);

		return itSite != sites.end() && itSite->second.ToMainBase;
	};
	auto const islandGoalKindName = [&]() -> const char*
	{
		switch (islandGoalKind)
		{
		case IslandGoalKind::Ore:
			return "ore";
		case IslandGoalKind::BareLand:
			return "bare land";
		case IslandGoalKind::EnemyBeachhead:
			return "enemy beachhead";
		default:
			return "none";
		}
	};
	auto const isStrandedHarvester = [&](UnitClass* pU, int zone)
	{
		return (pU->Type->Harvester || pU->Type->Weeder) && zone >= 0 && !ownRefineryZones.empty() && !zoneHasOwnRefinery(zone)
			&& std::find(ownYardZones.begin(), ownYardZones.end(), zone) == ownYardZones.end();
	};
	auto const nearestOwnRefineryCell = [pHouse](const CellStruct& from) -> CellClass*
	{
		CellClass* pBest = nullptr;
		double bestDist = 999999.0;

		for (auto const pOwnBld : pHouse->Buildings)
		{
			if (!pOwnBld || pOwnBld->InLimbo || !pOwnBld->IsAlive || !pOwnBld->Type
				|| (!pOwnBld->Type->Refinery && !pOwnBld->Type->ResourceDestination))
			{
				continue;
			}

			const double dist = pOwnBld->GetMapCoords().DistanceFrom(from);

			if (dist < bestDist)
			{
				bestDist = dist;
				pBest = pOwnBld->GetCell();
			}
		}

		return pBest;
	};
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

		// The answer of the scan below, kept MapScanKeepFrames per house.
		struct IslandScan
		{
			int Serial { -1 };
			int Frame { -100000 };
			int Buildings { -1 };
			CellStruct Home { CellStruct::Empty };
			int HomeZone { -1 };
			bool Congested { false };
			bool HomeOre { false };
			CellStruct Goal { CellStruct::Empty };
			int GoalZone { -1 };
			bool GoalBareLand { false };
		};

		static std::array<IslandScan, 32> islandScans;
		IslandScan* const pIslandScan = (pHouse->ArrayIndex >= 0 && pHouse->ArrayIndex < 32) ? &islandScans[pHouse->ArrayIndex] : nullptr;
		const bool islandScanKept = pIslandScan && pIslandScan->Serial == NeuralMatchSerial
			&& Unsorted::CurrentFrame >= pIslandScan->Frame && Unsorted::CurrentFrame - pIslandScan->Frame < MapScanKeepFrames
			&& pIslandScan->Buildings == pHouse->Buildings.Count && pIslandScan->Home == homeCoords
			&& pIslandScan->HomeZone == homeGroundZone && pIslandScan->Congested == homeIslandCongested;
		// A kept answer skips the scan (no cell is within a negative reach).
		const int islandScanReach = islandScanKept ? -1 : 216;

		// One reach answer per zone (CanHouseReachGroundByTransport).
		std::vector<std::pair<int, bool>> islandZoneReach;

		auto const isIslandZoneReached = [&](int zone, const CellStruct& cell) -> bool
		{
			for (auto const& [knownZone, reached] : islandZoneReach)
			{
				if (knownZone == zone)
					return reached;
			}

			const bool reached = CanHouseReachGroundByTransport(pHouse, homeCoords, cell);
			islandZoneReach.emplace_back(zone, reached);

			return reached;
		};

		int bestResScore = -999999;
		CellClass* pFallbackIslandLandCell = nullptr;
		int fallbackIslandLandZone = -1;
		int bestLandScore = -999999;
		for (int dy = -islandScanReach; dy <= islandScanReach; dy += 4)
		{
			for (int dx = -islandScanReach; dx <= islandScanReach; dx += 4)
			{
				const CellStruct cand { static_cast<short>(homeCoords.X + dx), static_cast<short>(homeCoords.Y + dy) };
				auto const pCell = MapClass::Instance.TryGetCellAt(cand);
				if (!pCell || pCell->LandType == LandType::Water || pCell->LandType == LandType::Rock)
					continue;

				const int cz = MapClass::Instance.GetMovementZoneType(cand, MovementZone::Normal, false);

				// A goal on no ground is no island: the transports loaded for one found no beach there
				// and kept their cargo for the rest of the match.
				if (!IsGroundZone(cz))
					continue;

				// Ground no transport of ours lands on from home is no island to colonize: on a land map
				// a plateau's ore sent every expansion MCV into a hovercraft that never got there.
				if (cz != homeGroundZone && !isIslandZoneReached(cz, cand))
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

		if (pIslandResourceGoalCell)
		{
			islandGoalKind = IslandGoalKind::Ore;
		}
		else if (pFallbackIslandLandCell)
		{
			pIslandResourceGoalCell = pFallbackIslandLandCell;
			resourceIslandZone = fallbackIslandLandZone;
			islandGoalKind = IslandGoalKind::BareLand;
		}

		if (islandScanKept)
		{
			hasUncolonizedOreOnHomeIsland = pIslandScan->HomeOre;
			resourceIslandZone = pIslandScan->GoalZone;
			pIslandResourceGoalCell = pIslandScan->Goal != CellStruct::Empty ? MapClass::Instance.TryGetCellAt(pIslandScan->Goal) : nullptr;
			islandGoalKind = !pIslandResourceGoalCell ? IslandGoalKind::None
				: (pIslandScan->GoalBareLand ? IslandGoalKind::BareLand : IslandGoalKind::Ore);
		}
		else if (pIslandScan)
		{
			*pIslandScan = IslandScan { NeuralMatchSerial, Unsorted::CurrentFrame, pHouse->Buildings.Count, homeCoords, homeGroundZone,
				homeIslandCongested, hasUncolonizedOreOnHomeIsland,
				pIslandResourceGoalCell ? pIslandResourceGoalCell->MapCoords : CellStruct::Empty, resourceIslandZone,
				islandGoalKind == IslandGoalKind::BareLand };
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
							islandGoalKind = IslandGoalKind::EnemyBeachhead;
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
			// Only a tech on ground some transport of ours lands on from home: engineers boarded
			// hovercraft for one they could not reach and were set down every 1000 frames.
			if (d < bestTechDist)
			{
				CellClass* pTechLanding = FindSafeIslandLandingCell(pHouse, bc, bz, false);

				if (pTechLanding && CanHouseReachGroundByTransport(pHouse, homeCoords, pTechLanding->MapCoords))
				{
					bestTechDist = d;
					techIslandZone = bz;
					pIslandTechGoalCell = pTechLanding;
				}
			}
		}
	}

	int bestEnemyIdx = -1;
	int bestEnemyPriority = -999999;
	int runnerUpIdx = -1;
	int runnerUpPriority = -999999;
	// What made up the priority of the chosen house (main enemy log): threat seen, hurt us last,
	// weak enough to finish, Expert 3 opportunism, diplomacy, team front.
	std::array<int, 6> bestParts {};
	// Per house this tick: whether it was weighed, its priority, and what makes it urgent enough to
	// take over a main enemy held for less than MainEnemyHoldFrames.
	std::array<bool, 8> weighed {};
	std::array<int, 8> priorityOf {};
	std::array<const char*, 8> urgentWhy {};
	auto& mainCoal = GetMacroState(pHouse).Coalition;
	std::array<bool, 8> atOurBase {};

	if (homeCoords != CellStruct::Empty)
	{
		for (auto const& [uid, sighting] : GetMacroState(pHouse).EnemySightings)
		{
			if (sighting.OwnerIndex >= 0 && sighting.OwnerIndex < 8 && Unsorted::CurrentFrame - sighting.LastSeenFrame <= 150
				&& sighting.LastCoords.DistanceFrom(homeCoords) <= MainEnemyBaseCells)
			{
				atOurBase[sighting.OwnerIndex] = true;
			}
		}
	}

	for (auto const pOther : HouseClass::Array)
	{
		if (!pOther || pOther == pHouse || pHouse->IsAlliedWith(pOther) || pOther->IsNeutral() || pOther->IsObserver()
			|| pOther->Defeated)
		{
			continue;
		}

		const int idx = std::clamp(pOther->ArrayIndex, 0, 7);
		std::array<int, 6> parts {};
		parts[0] = pExt->NeuralAI_EstimatedEnemyThreat[idx] * GetMacroState(pHouse).FfaThreatScale[idx] / 100;

		if (pHouse->WhoLastHurtMe == pOther->ArrayIndex)
			parts[1] = 250;

		// Expert 3 (UpdateFfaTargets): busy, at our door, weakest, nearest.
		parts[3] = GetMacroState(pHouse).FfaEnemyBonus[idx];

		// Expert 3 diplomacy (NeuralAI.Diplomacy.inl): the coalition target, never a truce partner, an
		// ex-partner not for a while, two houses at war left to it, the dying house first, the poorest.
		parts[4] = DiplomacyTargetBias(pHouse, pOther->ArrayIndex, false);

		// Team blackboard: the enemy this house covers for its team (its front).
		parts[5] = TeamFrontBonus(pHouse, pOther->ArrayIndex);

		const int priority = parts[0] + parts[1] + parts[2] + parts[3] + parts[4] + parts[5];
		weighed[idx] = true;
		priorityOf[idx] = priority;

		// The engine never clears WhoLastHurtMe, so only a hit of the last MainEnemyUrgentFrames counts.
		if (Unsorted::CurrentFrame - mainCoal.HurtByFrame[idx] <= MainEnemyUrgentFrames)
			urgentWhy[idx] = "it hurts us";
		else if (atOurBase[idx])
			urgentWhy[idx] = "at our base";
		else if (parts[2] > 0 || (idx == mainCoal.DyingTarget && Unsorted::CurrentFrame < mainCoal.DyingUntil))
			urgentWhy[idx] = "dying";

		if (priority > bestEnemyPriority)
		{
			runnerUpPriority = bestEnemyPriority;
			runnerUpIdx = bestEnemyIdx;
			bestEnemyPriority = priority;
			bestEnemyIdx = pOther->ArrayIndex;
			bestParts = parts;
		}
		else if (priority > runnerUpPriority)
		{
			runnerUpPriority = priority;
			runnerUpIdx = pOther->ArrayIndex;
		}
	}

	// The main enemy is held for MainEnemyHoldFrames: before that only a house worth
	// MainEnemySwitchMargin more, or one that is urgent (it hurts us, it is at our base, it is
	// dying) while the held one is not, takes over: two houses hitting us at once took turns.
	const char* switchWhy = "free";

	// Our own last choice: the engine's house AI writes EnemyHouseIndex too, between our ticks.
	if (const int current = mainCoal.MainEnemy; bestEnemyIdx >= 0 && bestEnemyIdx != current && current >= 0 && current < 8
		&& weighed[current] && Unsorted::CurrentFrame >= mainCoal.MainEnemySinceFrame
		&& Unsorted::CurrentFrame - mainCoal.MainEnemySinceFrame < MainEnemyHoldFrames)
	{
		const int best = std::clamp(bestEnemyIdx, 0, 7);

		if (urgentWhy[best] && !urgentWhy[current])
		{
			switchWhy = urgentWhy[best];
		}
		else if (bestEnemyPriority >= priorityOf[current] + MainEnemySwitchMargin)
		{
			switchWhy = "clear margin";
		}
		else
		{
			bestEnemyIdx = current;
			mainCoal.MainEnemyHeld++;
		}
	}

	// Which motive weighs most in the choice whenever the main enemy changes, so the motives can be
	// measured (the first MainEnemyLogLimit changes of each house in a match).
	if (bestEnemyIdx >= 0 && bestEnemyIdx != mainCoal.MainEnemy)
	{
		const int previous = mainCoal.MainEnemy;
		mainCoal.MainEnemy = bestEnemyIdx;
		mainCoal.MainEnemySinceFrame = Unsorted::CurrentFrame;
		mainCoal.MainEnemyChanges++;

		static int mainEnemyLogSerial = -1;
		static std::array<int, 32> mainEnemyLogCount {};

		if (mainEnemyLogSerial != NeuralMatchSerial)
		{
			mainEnemyLogSerial = NeuralMatchSerial;
			mainEnemyLogCount.fill(0);
		}

		if (mainEnemyLogCount[std::clamp(pHouse->ArrayIndex, 0, 31)]++ < MainEnemyLogLimit)
		{
			constexpr const char* partNames[] = { "threat", "hurt us", "weak", "ffa", "diplomacy", "team" };
			size_t top = 0;

			for (size_t i = 1; i < bestParts.size(); ++i)
			{
				if (bestParts[i] > bestParts[top])
					top = i;
			}

			const char* motive = top == 4 ? DiplomacyTargetMotive(pHouse, bestEnemyIdx) : partNames[top];
			Debug::Log("[NeuralAI] House %d main enemy: house %d by %s (threat %d, hurt us %d, weak %d, ffa %d, diplomacy %d, team %d), "
				"was house %d (switch: %s), runner-up house %d at %d against %d, frame %d.\n", pHouse->ArrayIndex, bestEnemyIdx, motive,
				bestParts[0], bestParts[1], bestParts[2], bestParts[3], bestParts[4], bestParts[5], previous, switchWhy,
				runnerUpIdx, runnerUpPriority, bestEnemyPriority, Unsorted::CurrentFrame);
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

	// Enemy ships on cells we see (any of them once the fight has stalled or their owner has no
	// building left): what each ship of ours that fights at sea chooses from.
	std::vector<UnitClass*> seenEnemyShips;

	// The enemy ship one of ours goes for: the nearest of seenEnemyShips in its own sea (the lower
	// UniqueID among equals). With mustFire, only one it can fire at now: in sight (not cloaked, or
	// on a cell our sensors cover) and with a weapon the engine lets it use on it, range apart.
	// One target for the whole fleet, an enemy submarine by preference, sent the submarines of
	// every house at each other's, where they stood on Attack with no target (match #9872,
	// Mayflower).
	auto const nearestEnemyShipFor = [&](UnitClass* pShip, bool mustFire) -> UnitClass*
	{
		const MovementZone movementZone = pShip->Type->MovementZone;
		const int ownSea = ObjectZone(pShip, movementZone);
		UnitClass* pBest = nullptr;
		int bestDist = INT_MAX;

		for (auto const pEnemy : seenEnemyShips)
		{
			auto const pCell = pEnemy->GetCell();

			if (!pCell || MapClass::Instance.GetMovementZoneType(pEnemy->GetMapCoords(), movementZone, false) != ownSea)
				continue;

			const int dist = pShip->DistanceFrom(pEnemy);

			if (dist > bestDist || (dist == bestDist && pBest && pEnemy->UniqueID > pBest->UniqueID))
				continue;

			if (mustFire)
			{
				if (pEnemy->CloakState != CloakState::Uncloaked && !pCell->Sensors_InclHouse(static_cast<unsigned int>(pHouse->ArrayIndex)))
					continue;

				const FireError error = pShip->GetFireError(pEnemy, pShip->SelectWeapon(pEnemy), true);

				if (error == FireError::ILLEGAL || error == FireError::CANT || error == FireError::NONE)
					continue;
			}

			bestDist = dist;
			pBest = pEnemy;
		}

		return pBest;
	};

	// Where the idle warships of one sea gather while no enemy ship is seen (slot 69; user approved, 2026-11-19): the
	// water of that sea nearest to the known enemy building nearest to our base that has water of it within
	// NavalShoreSearchCells, pulled back toward our base (NavalStagingBackCells at most) until NavalStagingMarginCells out
	// of known defense reach. They gathered by the first ship of ours costing 1000 or more, wherever it was: 31 destroyers
	// in a clump within 18 cells of it (match #11499). One answer per sea and tick; nullptr when there is none.
	std::vector<std::tuple<MovementZone, int, CellClass*>> fleetStagingCells;

	auto const fleetStagingCellFor = [&](UnitClass* pShip) -> CellClass*
	{
		const MovementZone movementZone = pShip->Type->MovementZone;
		const int sea = ObjectZone(pShip, movementZone);

		if (sea < 0 || homeCoords == CellStruct::Empty)
			return nullptr;

		for (auto const& [zoneType, zone, pCell] : fleetStagingCells)
		{
			if (zoneType == movementZone && zone == sea)
				return pCell;
		}

		BuildingClass* pNearest = nullptr;
		double nearestDist = 0.0;

		for (auto const pBld : BuildingClass::Array)
		{
			if (!pBld || pBld->InLimbo || !pBld->IsAlive || !pBld->Owner || !pBld->Type || pBld->Type->Insignificant
				|| pBld->Type->InvisibleInGame || pBld->Owner == pHouse || pHouse->IsAlliedWith(pBld->Owner) || pBld->Owner->IsNeutral()
				|| pBld->Owner->IsObserver() || pBld->Owner->Defeated || !IsEnemyBuildingKnown(pHouse, pBld))
			{
				continue;
			}

			const CellStruct bc = pBld->GetMapCoords();
			const double dist = bc.DistanceFrom(homeCoords);

			if ((pNearest && dist >= nearestDist) || NearestNavalShore(bc, movementZone, sea).Cells > NavalShoreSearchCells)
				continue;

			pNearest = pBld;
			nearestDist = dist;
		}

		CellClass* pStaging = nullptr;
		CellClass* pShore = pNearest ? NearestReachableNavalCell(pShip, pNearest->GetMapCoords()) : nullptr;

		// A building whose cell reads as the sea itself (one that stands on water): the nearest cell of the sea around it
		// that no building stands on, ring by ring.
		if (pNearest && !pShore)
		{
			const CellStruct bc = pNearest->GetMapCoords();

			for (int r = 1; r <= NavalShoreSearchCells && !pShore; ++r)
			{
				int bestDist = INT_MAX;

				for (int dy = -r; dy <= r; ++dy)
				{
					for (int dx = -r; dx <= r; ++dx)
					{
						if (std::abs(dx) != r && std::abs(dy) != r)
							continue;

						const CellStruct c { static_cast<short>(bc.X + dx), static_cast<short>(bc.Y + dy) };
						auto const pCell = MapClass::Instance.TryGetCellAt(c);

						if (pCell && !pCell->GetBuilding() && dx * dx + dy * dy < bestDist
							&& MapClass::Instance.GetMovementZoneType(c, movementZone, false) == sea)
						{
							bestDist = dx * dx + dy * dy;
							pShore = pCell;
						}
					}
				}
			}
		}

		if (pShore)
		{
			const CellStruct from = pShore->MapCoords;
			const int dx = homeCoords.X - from.X;
			const int dy = homeCoords.Y - from.Y;
			const int len = std::max({ 1, std::abs(dx), std::abs(dy) });

			for (int back = 0; back <= std::min(NavalStagingBackCells, len) && !pStaging; ++back)
			{
				const CellStruct c { static_cast<short>(from.X + dx * back / len), static_cast<short>(from.Y + dy * back / len) };
				auto const pCell = MapClass::Instance.TryGetCellAt(c);

				if (pCell && MapClass::Instance.GetMovementZoneType(c, movementZone, false) == sea
					&& !IsCellInKnownEnemyDefenseReach(pHouse, c, NavalStagingMarginCells))
				{
					pStaging = pCell;
				}
			}
		}

		fleetStagingCells.emplace_back(movementZone, sea, pStaging);

		return pStaging;
	};

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

				if (pCheckU->Type->Naval)
					seenEnemyShips.push_back(pCheckU);

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

				if (pCheckU->Type->Cloakable || pCheckU->Type->Underwater || pCheckU->CloakState != CloakState::Uncloaked)
				{
					auto& sightings = GetMacroState(pHouse).StealthSightings;

					if (sightings.size() > 256)
					{
						std::erase_if(sightings, [](const auto& e) { return Unsorted::CurrentFrame - e.second.second > 3000; });
					}

					sightings[pCheckU->UniqueID] = { pCheckU->GetMapCoords(), Unsorted::CurrentFrame };
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
				if (pCheckU->Type->Naval)
					seenEnemyShips.push_back(pCheckU);

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
	// a heavy threat (Kirov-class) that is still short of interceptors. With inAirOnly, only
	// threats off the ground (an aircraft interceptor never dives at one parked on its pad).
	auto pickAirFlankFor = [&](FootClass* pDefender, bool defenderIdle, int maxDistLeptons, bool inAirOnly) -> int
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

			if (!flank.Target || flank.Target->InLimbo || !flank.Target->IsAlive || (inAirOnly && !flank.Target->IsInAir()))
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
	const bool focusFireOpen = SlotGateHeld(scores[76], 80, 76);
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
		// Occupiable buildings pass even when Insignificant: C&C Reloaded marks 167 of its 169
		// garrisonable civilian buildings Insignificant=yes, so no damaged garrison was ever found for
		// an engineer to repair (0 repair orders in every match).
		if (!pEBld || pEBld->InLimbo || !pEBld->IsAlive || !pEBld->Type || pEBld->Type->Immune
			|| (pEBld->Type->Insignificant && !pEBld->Type->CanBeOccupied) || pEBld->Type->InvisibleInGame)
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
		// A repair uses the engineer up: one with soldiers of ours or of an ally inside is worth it once
		// it is half gone, an empty one only when our idle occupiers asked for it (RunUrbanGarrison:
		// nothing else to take, only its damage keeps them out; 178 of 251 repair orders went to
		// empty buildings nobody was going to man, up to 79% health).
		const bool urbanHeldByFriend = pEBld->Occupants.Count > 0 && pEBld->Occupants.GetItem(0)
			&& pHouse->IsAlliedWith(pEBld->Occupants.GetItem(0)->Owner);
		auto const wantedAsGarrison = [pHouse, pEBld]()
		{
			auto const& urban = GetUrbanState(pHouse);

			return urban.RepairWantedID == pEBld->UniqueID
				&& Unsorted::CurrentFrame - urban.RepairWantedFrame <= UrbanRepairWantedFrames;
		};
		const bool urbanRepairCandidate = pEBld->Type->CanBeOccupied && pEBld->Type->Repairable
			&& (pEBld->Owner == pHouse || IsCellKnownToAI(pHouse, pEBld->GetMapCoords()));
		const bool urbanWorthAnEngineer = urbanRepairCandidate
			&& (urbanHeldByFriend
				? pEBld->GetHealthPercentage() < UrbanRepairHeldBelow
				: (pEBld->Occupants.Count == 0 && pEBld->GetHealthPercentage() * 100.0 < UrbanGarrisonMinHealthPercent
					&& wantedAsGarrison()));

		if (urbanWorthAnEngineer)
		{
			const bool isFriendlyOrEmpty = true;

			// One engineer per building: none while one of ours already walks into it.
			bool engineerOnTheWay = false;

			if (isFriendlyOrEmpty && !pDamagedUrbanToRepair)
			{
				for (auto const pOwnEng : InfantryClass::Array)
				{
					if (pOwnEng && pOwnEng->Owner == pHouse && pOwnEng->IsAlive && !pOwnEng->InLimbo && pOwnEng->Type
						&& pOwnEng->Type->Engineer && pOwnEng->Destination == pEBld)
					{
						engineerOnTheWay = true;
						break;
					}
				}
			}

			if (isFriendlyOrEmpty && !pDamagedUrbanToRepair && !engineerOnTheWay)
				pDamagedUrbanToRepair = pEBld;
		}

		if (!pEBld->Owner || pEBld->Owner == pHouse || pHouse->IsAlliedWith(pEBld->Owner) || pEBld->Owner->IsNeutral())
			continue;

		if (!pEBld->GetCell() || !IsCellVisibleToAI(pHouse, pEBld->GetCell()))
			continue;

		// Occupants are only read while this house knows the building's cell (IsCellKnownToAI).
		const bool enemyGarrisonInSight = pEBld->Type->CanBeOccupied && pEBld->Occupants.Count > 0
			&& IsCellKnownToAI(pHouse, pEBld->GetMapCoords());

		if (enemyGarrisonInSight)
		{
			if (!pEnemyGarrisonToClear)
				pEnemyGarrisonToClear = pEBld;

			if (enemyGarrisonCount < 6)
				enemyGarrisonsToClear[enemyGarrisonCount++] = pEBld;
		}

		if (pEBld->Type->Capturable && pEBld->Type->ConstructionYard && !pCapturableEnemyConYard)
			pCapturableEnemyConYard = pEBld;

		if (SlotGateHeld(scores[79], 80, 79)
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

		if (SlotGateHeld(scores[57], 150, 57))
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

	if (pCapturableEnemyConYard && SlotGateHeld(scores[82], 60, 82, pCapturableEnemyConYard->UniqueID))
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

	// Not slot-gated any more (match #7132: a captured NODMCV held for an hour, never deployed):
	// keeping and using what we captured is not optional. Slot 82 still decides the heists.
	if (homeCoords != CellStruct::Empty)
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

		// Held 600+ frames on one cell, not firing, target (if any) out of reach: doing nothing,
		// whatever its order says (a block of our own units, a target it cannot get to).
		if (IsFootHeldStill(pHouse, pFoot, 600))
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

		// Near the enemy but held still 600+ frames without firing: not fighting there.
		if (IsFootHeldStill(pHouse, pFoot, 600))
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

	// TEMP diagnostic (match #11476: a yard MCV stood packed 60000+ frames with no watchdog line):
	// every 4500 frames, each packed yard MCV of the house and what keeps the MCV rules from it
	// (McvPackedLogLines a match).
	if (IsAITickWindow(4500, 0))
	{
		static int mcvPackedLogSerial = -1;
		static int mcvPackedLogCount = 0;

		if (mcvPackedLogSerial != NeuralMatchSerial)
		{
			mcvPackedLogSerial = NeuralMatchSerial;
			mcvPackedLogCount = 0;
		}

		auto& packedMacro = GetMacroState(pHouse);

		for (auto const pMcv : UnitClass::Array)
		{
			if (mcvPackedLogCount >= McvPackedLogLines)
				break;

			if (!pMcv || pMcv->Owner != pHouse || !pMcv->IsAlive || !pMcv->Type || !pMcv->Type->DeploysInto
				|| !pMcv->Type->DeploysInto->ConstructionYard)
			{
				continue;
			}

			++mcvPackedLogCount;
			const CellStruct mc = pMcv->GetMapCoords();
			char watchText[96] = "none";

			if (auto const itWatch = packedMacro.McvIdle.find(pMcv->UniqueID); itWatch != packedMacro.McvIdle.end())
			{
				std::snprintf(watchText, sizeof(watchText), "at (%d,%d) since %d, %d sites failed, own order until %d",
					itWatch->second.Cell.X, itWatch->second.Cell.Y, itWatch->second.SinceFrame,
					static_cast<int>(itWatch->second.FailedSites.size()), itWatch->second.OwnOrderUntil);
			}

			auto const itUnfold = packedMacro.McvUnfolds.find(pMcv->UniqueID);
			auto const itWait = packedMacro.IslandMcvWaits.find(pMcv->UniqueID);
			Debug::Log("[NeuralAI] House %d packed MCV: %s %u at (%d,%d) zone %d, mission %d queued %d, deploying %d, in limbo %d "
				"transporter %u, link %d, transport op %d, scout %d, plan %d, unfolding since %d, island wait since %d released %d, "
				"island goal %s zone %d, idle watch %s, %d yards, frame %d.\n",
				pHouse->ArrayIndex, pMcv->Type->ID, pMcv->UniqueID, mc.X, mc.Y, ObjectZone(pMcv, MovementZone::Normal),
				static_cast<int>(pMcv->CurrentMission), static_cast<int>(pMcv->QueuedMission), pMcv->IsDeploying ? 1 : 0,
				pMcv->InLimbo ? 1 : 0, pMcv->Transporter ? pMcv->Transporter->UniqueID : 0u, pMcv->HasAnyLink() ? 1 : 0,
				IsInTransportOp(pHouse, pMcv->UniqueID) ? 1 : 0, IsActiveScout(pHouse, pMcv->UniqueID) ? 1 : 0,
				IsPlanMember(pHouse, pMcv->UniqueID) ? 1 : 0, itUnfold != packedMacro.McvUnfolds.end() ? itUnfold->second.Since : -1,
				itWait != packedMacro.IslandMcvWaits.end() ? itWait->second.SinceFrame : -1,
				(itWait != packedMacro.IslandMcvWaits.end() && itWait->second.Released) ? 1 : 0, islandGoalKindName(), resourceIslandZone,
				watchText, pHouse->NumConYards, Unsorted::CurrentFrame);
		}
	}

	// TEMP diagnostic: an MCV the idle watchdog followed that began to unfold by another rule's order
	// leaves the watch with no line of the watchdog; a line tells when its yard stands (the scenario
	// probes close an MCV's wait on an "unfolds" line). Looked at until McvIdleUnfoldLookFrames; an MCV
	// that stands packed again, not unfolding, is the watchdog's again.
	auto const noteWatchedMcvUnfolding = [pHouse](UnitClass* pMcv)
	{
		auto& macro = GetMacroState(pHouse);

		if (auto const itWatch = macro.McvIdle.find(pMcv->UniqueID); itWatch != macro.McvIdle.end())
		{
			macro.McvIdleUnfolds[pMcv->UniqueID]
				= { pMcv->GetMapCoords(), Unsorted::CurrentFrame, itWatch->second.SinceFrame, pMcv->Type->ID };
		}
	};

	if (auto& idleUnfolds = GetMacroState(pHouse).McvIdleUnfolds; !idleUnfolds.empty())
	{
		static int idleUnfoldLogSerial = -1;
		static int idleUnfoldLogCount = 0;

		if (idleUnfoldLogSerial != NeuralMatchSerial)
		{
			idleUnfoldLogSerial = NeuralMatchSerial;
			idleUnfoldLogCount = 0;
		}

		const int now = Unsorted::CurrentFrame;

		for (auto it = idleUnfolds.begin(); it != idleUnfolds.end();)
		{
			auto const& unfold = it->second;
			bool over = now < unfold.Frame || now - unfold.Frame > McvIdleUnfoldLookFrames;
			UnitClass* pMcv = nullptr;

			for (auto const pU : UnitClass::Array)
			{
				if (pU && pU->UniqueID == it->first)
				{
					pMcv = pU;
					break;
				}
			}

			if (!over && pMcv && pMcv->IsAlive && !pMcv->InLimbo)
			{
				// Still packed: unfolding yet, or the engine refused the deploy.
				over = !pMcv->IsDeploying && pMcv->CurrentMission != Mission::Unload && pMcv->QueuedMission != Mission::Unload;
			}
			else if (!over)
			{
				for (auto const pBld : pHouse->Buildings)
				{
					if (!pBld || !pBld->IsAlive || pBld->InLimbo || !pBld->Type || !pBld->Type->ConstructionYard
						|| pBld->GetMapCoords().DistanceFrom(unfold.Cell) > 2.0)
					{
						continue;
					}

					if (idleUnfoldLogCount++ < McvIdleUnfoldLogLines)
					{
						const CellStruct yardCell = pBld->GetMapCoords();
						Debug::Log("[NeuralAI] House %d MCV watchdog: %s %u unfolds by its own order at (%d,%d), into %s at (%d,%d), "
							"watched since frame %d, unfolding since frame %d, %d yards, frame %d.\n",
							pHouse->ArrayIndex, unfold.TypeId, it->first, unfold.Cell.X, unfold.Cell.Y, pBld->Type->ID, yardCell.X,
							yardCell.Y, unfold.WatchedSince, unfold.Frame, pHouse->NumConYards, now);
					}

					over = true;
					break;
				}
			}

			it = over ? idleUnfolds.erase(it) : std::next(it);
		}
	}

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

		// A transport parked to load takes its orders from the transport loader.
		if (IsFerryParkedToLoad(pHouse, pUnit->UniqueID))
			continue;

		// A unit or transport of a transport operation takes its orders from the operation alone.
		if (IsInTransportOp(pHouse, pUnit->UniqueID))
			continue;

		if (pUnit->HasAnyLink())
		{
			if (auto const pLinkedBld = abstract_cast<BuildingClass*>(pUnit->GetNthLink(0)))
			{
				if (pLinkedBld->Type
					&& (pLinkedBld->Type->WeaponsFactory || pLinkedBld->Type->Factory == AbstractType::UnitType))
				{
					// A damaged unit (or one sent in) that docks at a repair station that is also a factory (a
					// CABAL yard) is not leaving it: dispersed each tick it never docked (match #12347, Tsunami: a
					// hovercraft at 41% for 30000 frames by a ROBOTYARD).
					if (pLinkedBld->Type->UnitRepair && (pUnit->GetHealthPercentage() < 1.0
						|| pUnit->CurrentMission == Mission::Enter || pUnit->QueuedMission == Mission::Enter))
					{
						continue;
					}

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
		if (enemySuperImminent && SlotGateHeld(scores[58], SurvivalMicroVetoScore, 58, pUnit->UniqueID) && !pType->Harvester && !pType->ResourceGatherer
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
			&& !HasNoManualEnter(pType) && pUnit->GetHealthPercentage() < 0.24 && SlotGateHeld(scores[70], SurvivalMicroVetoScore, 70, pUnit->UniqueID))
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
		if (pUnit->AttachedBomb != nullptr && !pRepairDepot && SlotGateHeld(scores[72], 60, 72, pUnit->UniqueID))
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
				// A survival order: it takes the unit from a plan or an operation (order leases).
				const NeuralOrderPriorityScope orderPriority { NeuralOrderPriority::Survival };

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
				// The nearest field with ore to spare that nobody mines yet, else the nearest ore cell: a
				// miner packed up from a spent field went back to its last ore cells and deployed there again.
				CellClass* pNearestOre = FindMinerOreField(pHouse, uc, IsGroundZone(unitZoneForEco) ? unitZoneForEco : -1, 0, MinerFieldMaxCells);
				const bool fieldFound = pNearestOre != nullptr;
				int nearestOreDist = INT_MAX;

				for (int dy = -48; dy <= 48 && !fieldFound; dy += 2)
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
			// A yard unfolding is the engine's: no rule below gives that MCV an order (matches
			// #11445-#11450: MCVs ordered to unfold were sent elsewhere by them, three times one MCV).
			// When the engine refuses the deploy it clears the foundation for the next try, so a yard
			// that was unfolding unfolds again on that spot, McvUnfoldMostTries times within
			// McvUnfoldRetryFrames, before the rules below may move it. The skip lasts
			// McvUnfoldSkipMostFrames at most from the first frame the MCV unfolded on that spot
			// (match #11476): past it the rules below and the idle watchdog act on it again.
			bool unfoldingTooLong = false;
			{
				auto& unfoldMacro = GetMacroState(pHouse);
				auto& unfolds = unfoldMacro.McvUnfolds;
				const int now = Unsorted::CurrentFrame;
				const CellStruct here = pUnit->GetMapCoords();

				if (unfolds.size() > 64)
					std::erase_if(unfolds, [now](const auto& entry) { return now - entry.second.Frame > McvUnfoldRetryFrames; });

				if (pUnit->IsDeploying || pUnit->CurrentMission == Mission::Unload || pUnit->QueuedMission == Mission::Unload)
				{
					auto& unfold = unfolds[pUnit->UniqueID];

					if (unfold.Cell != here)
					{
						unfold.Cell = here;
						unfold.Tries = 0;
						unfold.Since = now;
					}
					else if (unfold.Since < 0 || now < unfold.Since || now < unfold.Frame || now - unfold.Frame > McvUnfoldRetryFrames)
					{
						unfold.Since = now;
					}

					unfold.Frame = now;
					unfoldingTooLong = now - unfold.Since > McvUnfoldSkipMostFrames;

					if (!unfoldingTooLong)
					{
						noteWatchedMcvUnfolding(pUnit);
						unfoldMacro.McvIdle.erase(pUnit->UniqueID);
						continue;
					}
				}
				else if (auto const itUnfold = unfolds.find(pUnit->UniqueID); itUnfold != unfolds.end())
				{
					if (here.DistanceFrom(itUnfold->second.Cell) <= 1.0 && now - itUnfold->second.Frame <= McvUnfoldRetryFrames
						&& itUnfold->second.Tries < McvUnfoldMostTries && CanDeployerUnpackHere(pHouse, pType->DeploysInto, pUnit))
					{
						++itUnfold->second.Tries;
						CountDecision(NeuralDecision::McvUnfoldRetry, pHouse->ArrayIndex);
						OrderDeployerUnpack(pUnit);
						continue;
					}

					unfolds.erase(itUnfold);
				}
			}

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
				}
				else
				{
					pUnit->SetDestination(pArchDest, true);
					pUnit->ForceMission(Mission::Move);
					NoteOrderCaller(pUnit, "move");
				}
			}

			const CellStruct mcvCell = pUnit->GetMapCoords();
			const bool isForeignTechMCV = (pHouse->CountOwnedNow(pType->DeploysInto) == 0 && pHouse->NumConYards >= 1);
			// The MCV danger rule (CollectMcvDangerBuildings), the same one its expansion goals keep off. An
			// MCV in danger at its last look leaves it only McvDangerExitCells farther: at the edge of a reach
			// it turned home and back each AI tick.
			auto& mcvInDanger = GetMacroState(pHouse).McvInDanger;
			const int dangerExitCells = mcvInDanger.contains(pUnit->UniqueID) ? McvDangerExitCells : 0;
			std::vector<std::pair<CellStruct, int>> mcvDangers;
			CollectMcvDangerBuildings(pHouse, mcvDangers);
			const bool dangerFromBuildings = IsInMcvDanger(mcvDangers, mcvCell, dangerExitCells);
			// The visible armed enemy units within McvDangerUnitCells, by combat value (hold the ground below).
			const int enemyUnitValueNearMcv = McvDangerUnitValueNear(mcvCell, McvDangerUnitCells + dangerExitCells);
			bool enemyNearMCV = dangerFromBuildings || enemyUnitValueNearMcv > 0;
			// The goal it drives to is in danger as well, or it met the danger near that goal: a danger met on
			// the way, far from the goal, keeps no goal from the expansion goals (match #12278: one 34 cells off).
			auto const mcvDangerNearItsGoal = [&]() -> bool
			{
				if (pUnit->CurrentMission != Mission::Move || !pUnit->Destination)
					return false;

				const CellStruct goal = CellClass::Coord2Cell(pUnit->Destination->GetCoords());

				return mcvCell.DistanceFrom(goal) <= McvFledNearGoalCells || IsInMcvDanger(mcvDangers, goal, McvSiteDangerMarginCells)
					|| McvDangerUnitValueNear(goal, McvDangerUnitCells + McvSiteDangerMarginCells) > 0;
			};

			if (mcvInDanger.size() > 64)
				mcvInDanger.clear();

			if (enemyNearMCV)
				mcvInDanger.insert(pUnit->UniqueID);
			else
				mcvInDanger.erase(pUnit->UniqueID);

			// The ground the MCV stands on. A cell of no ground under it (zone 1: a shore, a cliff foot)
			// is not another island: read the land around it, or no zone. Taken as an island, it made
			// MCVs in their own base give that "island" up for lack of room (match #10365).
			const int mcvCellZone = MapClass::Instance.GetMovementZoneType(mcvCell, MovementZone::Normal, pUnit->OnBridge);
			const int mcvLandZone = IsGroundZone(mcvCellZone) ? mcvCellZone : TransportLandZone(mcvCell, pUnit->OnBridge);
			const int mcvZone = IsGroundZone(mcvLandZone) ? mcvLandZone : -1;

			// Idle MCV watchdog (McvIdleFrames, NeuralAI.State.inl): no other rule notices a packed MCV
			// of a house with a yard that keeps to one spot. Its site search below checks the zone, not
			// a path, and keeps its answer, so an MCV the engine finds no path for is sent to the same
			// site again and again, and an MCV within 3 cells of its goal gets no order at all. The
			// watchdog unfolds it where it stands if it can; else it leads it along legs of our own route
			// search to the nearest site not tried yet, and home to a yard of ours after McvIdleMostTries
			// such sites. While it follows that order the MCV logic below leaves it alone. An MCV that
			// may be waiting for a ferry to the island goal (one that may cross to it,
			// yardMcvMayCrossToIslandGoal) is left to the island logic and its timers, until its wait ran
			// out and the land rule gave it its order (IslandMcvWaitFrames, land before islands below: a
			// free field of its landmass, or a site to unfold at meanwhile).
			{
				auto& idleWatch = GetMacroState(pHouse).McvIdle;
				const int now = Unsorted::CurrentFrame;
				auto const& islandWaits = GetMacroState(pHouse).IslandMcvWaits;
				auto const itIslandWait = islandWaits.find(pUnit->UniqueID);
				const bool islandWaitOver = itIslandWait != islandWaits.end() && itIslandWait->second.Released
					&& now - itIslandWait->second.SeenFrame <= IslandMcvWaitGapFrames;
				const bool mayWaitForFerry = pIslandResourceGoalCell != nullptr && mcvZone >= 0
					&& MapClass::Instance.GetMovementZoneType(pIslandResourceGoalCell->MapCoords, MovementZone::Normal, false) != mcvZone
					&& CanHouseFerryTypeAcrossWater(pHouse, pType) && !islandWaitOver
					&& (isForeignTechMCV || yardMcvMayCrossToIslandGoal(pUnit));
				const bool unfolding = (pUnit->IsDeploying || pUnit->CurrentMission == Mission::Unload) && !unfoldingTooLong;

				if (idleWatch.size() > 64)
					std::erase_if(idleWatch, [now](const auto& entry) { return now - entry.second.SinceFrame > 20000; });

				if (mayWaitForFerry || unfolding)
				{
					if (unfolding)
						noteWatchedMcvUnfolding(pUnit);

					idleWatch.erase(pUnit->UniqueID);
				}
				else
				{
					auto& watch = idleWatch[pUnit->UniqueID];

					if (watch.Cell == CellStruct::Empty || mcvCell.DistanceFrom(watch.Cell) > McvIdleDriftCells)
					{
						watch.Cell = mcvCell;
						watch.SinceFrame = now;
					}

					const int stood = now - watch.SinceFrame;
					// TEMP diagnostic: each step of the watchdog (60 lines per match).
					static int mcvWatchLogSerial = -1;
					static int mcvWatchLogCount = 0;

					if (mcvWatchLogSerial != NeuralMatchSerial)
					{
						mcvWatchLogSerial = NeuralMatchSerial;
						mcvWatchLogCount = 0;
					}

					auto const logStep = [&](const char* what, const CellStruct& at)
					{
						if (mcvWatchLogCount++ >= 60)
							return;

						char destText[24] = "none";

						if (pUnit->Destination)
						{
							const CellStruct destCell = CellClass::Coord2Cell(pUnit->Destination->GetCoords());
							std::snprintf(destText, sizeof(destText), "(%d,%d)", destCell.X, destCell.Y);
						}

						Debug::Log("[NeuralAI] House %d MCV watchdog: %s %u %s (%d,%d), from (%d,%d) after %d frames there, mission %d dest %s, "
							"enemy near %d, %d sites failed, %d yards, frame %d.\n",
							pHouse->ArrayIndex, pType->ID, pUnit->UniqueID, what, at.X, at.Y, mcvCell.X, mcvCell.Y, stood,
							static_cast<int>(pUnit->CurrentMission), destText, enemyNearMCV ? 1 : 0, static_cast<int>(watch.FailedSites.size()),
							pHouse->NumConYards, now);
					};

					// The next leg toward Goal: false when there is no way there (a direct move for the
					// last stretch, where the route search gives no leg). A move the MCV already makes is
					// kept only when it heads for Goal or for the leg given last: any other is the order
					// that left it standing (matches #11445-#11450: MCVs on another rule's move were never
					// sent to their site, and the site was judged from where they stood).
					auto const followGoal = [&]() -> bool
					{
						if (pUnit->CurrentMission == Mission::Move && pUnit->Destination && pUnit->DistanceFrom(pUnit->Destination) > 512)
						{
							const CellStruct dest = CellClass::Coord2Cell(pUnit->Destination->GetCoords());

							if (dest.DistanceFrom(watch.Goal) <= 2.0 || (watch.Leg != CellStruct::Empty && dest.DistanceFrom(watch.Leg) <= 2.0))
								return true;
						}

						CellClass* pLeg = GroundRouteHop(pUnit, watch.Goal, McvIdleLegSteps);

						if (!pLeg)
						{
							if (LastGroundRouteAnswer == NeuralRouteAnswer::Budget)
								return true;

							if (LastGroundRouteAnswer != NeuralRouteAnswer::Near && LastGroundRouteAnswer != NeuralRouteAnswer::ShortLeg)
								return false;

							pLeg = MapClass::Instance.TryGetCellAt(watch.Goal);
						}

						if (pLeg)
						{
							watch.Leg = pLeg->MapCoords;
							IssueMoveOrder(pUnit, pLeg, 1.0);
						}

						return pLeg != nullptr;
					};

					auto const failGoal = [&](const char* what)
					{
						logStep(what, watch.Goal);
						watch.FailedSites.push_back(watch.Goal);
						watch.OwnOrderUntil = -1;
						watch.Leg = CellStruct::Empty;
						watch.NearSince = -1;
					};

					bool handled = false;
					bool retryNow = false;

					if (watch.OwnOrderUntil >= now)
					{
						// The deploy check reads the cell the MCV stands on, so only the site cell itself
						// tells whether the site is good; stopped on the way, the MCV unfolds where the
						// yard fits. Next to the site and stopped, it is moved straight onto the site cell
						// (not by the order helper, which keeps a unit within a cell or two of its
						// destination), and the site fails after McvIdleArriveFrames there.
						const bool onSite = mcvCell == watch.Goal;
						const double goalDist = mcvCell.DistanceFrom(watch.Goal);
						const bool driving = pUnit->CurrentMission == Mission::Move && pUnit->Destination
							&& pUnit->DistanceFrom(pUnit->Destination) > 256;

						const char* unfoldRefusal = "not tried";

						if ((onSite || !driving) && CanDeployerUnpackHere(pHouse, pType->DeploysInto, pUnit, &unfoldRefusal))
						{
							logStep(onSite ? "unfolds at the site" : "unfolds on the way at", mcvCell);
							OrderDeployerUnpack(pUnit);
							idleWatch.erase(pUnit->UniqueID);
							continue;
						}

						if (onSite)
						{
							failGoal("cannot unfold at");
							retryNow = true;

							// TEMP diagnostic: what refused the deploy on the site cell.
							if (mcvWatchLogCount < 60)
							{
								Debug::Log("[NeuralAI] House %d MCV watchdog: %s %u deploy refused at (%d,%d): %s, frame %d.\n",
									pHouse->ArrayIndex, pType->ID, pUnit->UniqueID, mcvCell.X, mcvCell.Y, unfoldRefusal, now);
							}
						}
						else if (goalDist <= McvIdleNearCells && !driving)
						{
							if (watch.NearSince < 0)
								watch.NearSince = now;

							if (now - watch.NearSince >= McvIdleArriveFrames)
							{
								failGoal("cannot step onto");
								retryNow = true;
							}
							else
							{
								auto const pGoalCell = MapClass::Instance.TryGetCellAt(watch.Goal);

								if (pGoalCell && pUnit->Destination != pGoalCell)
								{
									pUnit->SetDestination(pGoalCell, true);
									pUnit->QueueMission(Mission::Move, true);
									NoteOrderCaller(pUnit, "move");
									CountDecision(NeuralDecision::McvIdleStepOnSite, pHouse->ArrayIndex);
								}

								handled = true;
							}
						}
						else if (followGoal())
						{
							handled = true;
						}
						else
						{
							failGoal("finds no way to");
							retryNow = true;
						}
					}
					else if (watch.OwnOrderUntil >= 0)
					{
						failGoal("did not reach");
						retryNow = true;
					}

					if (!handled && (retryNow || stood >= McvIdleFrames))
					{
						watch.SinceFrame = now;
						watch.Cell = mcvCell;

						if (CanDeployerUnpackHere(pHouse, pType->DeploysInto, pUnit))
						{
							logStep("unfolds where it stood", mcvCell);
							CountDecision(NeuralDecision::McvIdleUnfold, pHouse->ArrayIndex);
							OrderDeployerUnpack(pUnit);
							idleWatch.erase(pUnit->UniqueID);
							continue;
						}

						CellStruct goal = CellStruct::Empty;
						bool home = false;

						if (static_cast<int>(watch.FailedSites.size()) < McvIdleMostTries)
						{
							goal = FindDeployableFoundationNearCellUncached(pHouse, pType->DeploysInto, mcvCell, mcvZone, pUnit,
								&watch.FailedSites);
						}

						if (goal == CellStruct::Empty)
						{
							goal = FindBestDeployStandCell(pHouse, pType->DeploysInto, pUnit, mcvZone);
							home = true;
						}

						if (goal != CellStruct::Empty)
						{
							watch.Goal = goal;
							watch.Leg = CellStruct::Empty;
							watch.NearSince = -1;
							watch.OwnOrderUntil = now + McvIdleOrderFrames;
							CountDecision(home ? NeuralDecision::McvIdleHome : NeuralDecision::McvIdleSite, pHouse->ArrayIndex);
							logStep(home ? "goes home to" : "goes to the site", goal);
							followGoal();
							handled = true;
						}
						else
						{
							logStep("finds no site at all near", mcvCell);
						}
					}

					if (handled)
						continue;
				}
			}

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

			// Without any ConYard for 2700 frames the MCV stops waiting for a ferry or a far field and
			// deploys where it can, enemies near or not: no base at all is worse.
			auto& yardMacro = GetMacroState(pHouse);

			if (pHouse->NumConYards > 0)
				yardMacro.NoYardSinceFrame = -1;
			else if (yardMacro.NoYardSinceFrame < 0 || yardMacro.NoYardSinceFrame > Unsorted::CurrentFrame)
				yardMacro.NoYardSinceFrame = Unsorted::CurrentFrame;

			const bool noYardTooLong = pHouse->NumConYards == 0 && Unsorted::CurrentFrame - yardMacro.NoYardSinceFrame >= 2700;

			// Keep a nomadic single MCV undeployed while it is actively migrating to a distant
			// resource expansion or boarding/sailing on a live transport to a resource island (one it
			// may cross to, yardMcvMayCrossToIslandGoal)
			const bool isNomadicSingleMCVHop = (pHouse->NumConYards == 0 && !noYardTooLong && hubsAndFactoriesInMcvZone >= 3
				&& pHouse->NumWarFactories >= 1 && !enemyNearMCV
				&& ((ownsLiveMcvTransportNow && pIslandResourceGoalCell != nullptr && mcvZone != resourceIslandZone
						&& yardMcvMayCrossToIslandGoal(pUnit))
					|| isMovingToDestEarly));

			if (pHouse->NumConYards == 0 && !isNomadicSingleMCVHop)
			{
				auto const pYardType = pType->DeploysInto;
				if ((!enemyNearMCV || noYardTooLong) && CanDeployerUnpackHere(pHouse, pYardType, pUnit))
				{
					OrderDeployerUnpack(pUnit);
					continue;
				}

				const bool reachedOrNoDest = (pUnit->CurrentMission != Mission::Move || pUnit->Destination == nullptr
					|| CellClass::Coord2Cell(pUnit->Destination->GetCoords()) == mcvCell);
				if (reachedOrNoDest || (IsAITickWindow(45, 15)))
				{
					const CellStruct safeYardStandCell = FindBestDeployStandCell(pHouse, pYardType, pUnit, mcvZone);
					if (safeYardStandCell == mcvCell && (!enemyNearMCV || noYardTooLong)
						&& CanDeployerUnpackHere(pHouse, pYardType, pUnit))
					{
						OrderDeployerUnpack(pUnit);
					}
					else if (safeYardStandCell == mcvCell && noYardTooLong)
					{
						// Its own cell is the best stand but the deploy is refused there (units on the
						// foundation, the cell itself): step aside and try the cells around.
						pUnit->Scatter(CoordStruct::Empty, true, false);
					}
					else if (safeYardStandCell != CellStruct::Empty)
					{
						if (auto const pSafeDest = MapClass::Instance.TryGetCellAt(safeYardStandCell))
						{
							IssueMoveOrder(pUnit, pSafeDest, 3.0);
						}
					}
					else if (!enemyNearMCV || noYardTooLong)
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
			// An island where this house found no room for a yard lately is not tried again: the MCV
			// leaves it.
			auto& mcvNoRoomIslands = GetMacroState(pHouse).NoRoomIslandUntil;
			const bool mcvIslandHasNoRoom = mcvZone >= 0 && mcvNoRoomIslands.contains(mcvZone)
				&& mcvNoRoomIslands.at(mcvZone) > Unsorted::CurrentFrame;
			const bool landedOnNewIsland = (homeGroundZone >= 0 && mcvZone >= 0 && mcvZone != homeGroundZone
				&& conYardsInMcvZone == 0 && hubsAndFactoriesInMcvZone < 2 && !mcvIslandHasNoRoom);
			// Stolen MCVs (StolenMcv*, NeuralAI.State.inl): the engine's hunt goes; one out of danger near our
			// buildings unfolds there, its tech tree on safe ground; one that never unfolded goes into our Grinder.
			if (isForeignTechMCV && !landedOnNewIsland)
			{
				auto& stolenSince = GetMacroState(pHouse).StolenMcvSince;
				const int now = Unsorted::CurrentFrame;

				if (stolenSince.size() > 32)
					std::erase_if(stolenSince, [now](const auto& entry) { return now - entry.second > 4 * StolenMcvGrinderFrames; });

				const int stolenFrom = stolenSince.try_emplace(pUnit->UniqueID, now).first->second;

				if (pUnit->CurrentMission == Mission::Hunt || pUnit->QueuedMission == Mission::Hunt)
				{
					pUnit->ForceMission(Mission::Guard);
					logMcvHold(pUnit, "stops the engine's hunt at", mcvCell, enemyUnitValueNearMcv, -1);
				}

				int nearOwnCells = INT_MAX;

				for (auto const pOwnBld : pHouse->Buildings)
				{
					if (!pOwnBld || pOwnBld->InLimbo || !pOwnBld->IsAlive || !pOwnBld->Type || pOwnBld->Type->Wall)
						continue;

					const CellStruct bc = pOwnBld->GetMapCoords();
					nearOwnCells = std::min(nearOwnCells, std::max(std::abs(mcvCell.X - bc.X), std::abs(mcvCell.Y - bc.Y)));
				}

				if (!enemyNearMCV && nearOwnCells <= StolenMcvOwnCells && CanDeployerUnpackHere(pHouse, pType->DeploysInto, pUnit))
				{
					logMcvHold(pUnit, "stolen, unfolds by our buildings at", mcvCell, enemyUnitValueNearMcv, -1);
					OrderDeployerUnpack(pUnit);
					stolenSince.erase(pUnit->UniqueID);
					continue;
				}

				if (now - stolenFrom >= StolenMcvGrinderFrames && pGrinder && pGrinder->IsAlive && !pGrinder->InLimbo
					&& FootReachesAlone(pUnit, pGrinder->GetMapCoords(), pGrinder))
				{
					if (pUnit->CurrentMission != Mission::Enter && pUnit->QueuedMission != Mission::Enter)
					{
						logMcvHold(pUnit, "stolen, never unfolded, goes into the Grinder at", pGrinder->GetMapCoords(), enemyUnitValueNearMcv, -1);
						OrderFootToEnter(pUnit, pGrinder);
					}

					continue;
				}
			}

			// Hold the ground (McvFallback*, NeuralAI.State.inl; user approved, 2026-11-20, match #12278): an
			// expansion MCV of our tech that meets an enemy unfolds where it stands when only enemy units are near
			// and our defenses that reach it and our armed units around are at least as strong; else it heads
			// for the nearest cell out of every danger where its yard fits, near our buildings when it can, and
			// unfolds there (or on the way once out of danger and stopped), instead of driving home. With no such
			// cell it turns home as before. An MCV the idle watchdog leads (its own order since its last
			// failed site) is left to the watchdog.
			auto const& holdIdleWatch = GetMacroState(pHouse).McvIdle;
			auto const itHoldIdle = holdIdleWatch.find(pUnit->UniqueID);
			const bool watchdogLeadsMcv = itHoldIdle != holdIdleWatch.end() && itHoldIdle->second.OwnOrderUntil >= 0;

			if (watchdogLeadsMcv)
				GetMacroState(pHouse).McvFallbacks.erase(pUnit->UniqueID);

			if (!isForeignTechMCV && homeCoords != CellStruct::Empty && !landedOnNewIsland && !watchdogLeadsMcv)
			{
				auto& fallbacks = GetMacroState(pHouse).McvFallbacks;
				const int now = Unsorted::CurrentFrame;
				auto const pYardType = pType->DeploysInto;

				if (fallbacks.size() > 64)
					std::erase_if(fallbacks, [now](const auto& entry) { return now >= entry.second.Until; });

				if (enemyNearMCV && !dangerFromBuildings)
				{
					const int ownCover = McvOwnCoverValue(mcvCell);

					if (ownCover >= enemyUnitValueNearMcv && CanDeployerUnpackHere(pHouse, pYardType, pUnit))
					{
						logMcvHold(pUnit, "unfolds covered at", mcvCell, enemyUnitValueNearMcv, ownCover);
						OrderDeployerUnpack(pUnit);
						fallbacks.erase(pUnit->UniqueID);
						continue;
					}
				}

				auto itFallback = fallbacks.find(pUnit->UniqueID);

				// Over: its time ran out, or its cell is in danger now.
				if (itFallback != fallbacks.end()
					&& (now >= itFallback->second.Until
						|| (itFallback->second.Cell != CellStruct::Empty && !IsMcvFallbackSafe(itFallback->second.Cell, mcvDangers))))
				{
					fallbacks.erase(itFallback);
					itFallback = fallbacks.end();
				}

				if (itFallback == fallbacks.end() && enemyNearMCV)
				{
					if (mcvDangerNearItsGoal())
						NoteMcvFledSite(pHouse, pUnit, homeCoords);

					const CellStruct spot = FindMcvFallbackSpot(pUnit, pYardType, mcvZone, mcvDangers);
					itFallback = fallbacks.insert_or_assign(pUnit->UniqueID,
						NeuralMcvFallback { spot, now + (spot != CellStruct::Empty ? McvFallbackFrames : McvFallbackNoneFrames) }).first;
					logMcvHold(pUnit, spot != CellStruct::Empty ? "heads for the safe cell" : "finds no safe cell near",
						spot != CellStruct::Empty ? spot : mcvCell, enemyUnitValueNearMcv, -1);
				}

				if (itFallback != fallbacks.end() && itFallback->second.Cell != CellStruct::Empty)
				{
					const CellStruct spot = itFallback->second.Cell;
					const bool onSpot = mcvCell == spot;
					const bool driving = pUnit->CurrentMission == Mission::Move && pUnit->Destination
						&& pUnit->DistanceFrom(pUnit->Destination) > 256;

					if ((onSpot || (!driving && !enemyNearMCV)) && CanDeployerUnpackHere(pHouse, pYardType, pUnit))
					{
						logMcvHold(pUnit, onSpot ? "unfolds at the safe cell" : "unfolds on the way at", mcvCell, enemyUnitValueNearMcv, -1);
						OrderDeployerUnpack(pUnit);
						fallbacks.erase(pUnit->UniqueID);
						continue;
					}

					if (onSpot)
					{
						// The yard does not fit there after all: the turn home below, for a while.
						logMcvHold(pUnit, "cannot unfold at the safe cell", mcvCell, enemyUnitValueNearMcv, -1);
						itFallback->second = NeuralMcvFallback { CellStruct::Empty, now + McvFallbackNoneFrames };
					}
					else if (auto const pSpot = MapClass::Instance.TryGetCellAt(spot))
					{
						const NeuralOrderPriorityScope orderPriority { enemyNearMCV ? NeuralOrderPriority::Survival : NeuralOrderPriority::Routine };

						// Next to it and stopped: straight onto the cell (the order helper keeps a unit within a cell
						// or two of its goal). The order is given once.
						if (!driving && mcvCell.DistanceFrom(spot) <= McvIdleNearCells)
						{
							if (pUnit->Destination != pSpot)
							{
								pUnit->SetDestination(pSpot, true);
								pUnit->QueueMission(Mission::Move, true);
								NoteOrderCaller(pUnit, "move");
							}
						}
						else
						{
							IssueMoveOrder(pUnit, pSpot, 1.0);
						}

						continue;
					}
				}
			}

			// Not slot-gated (see the stolen asset scan above).
			if (homeCoords != CellStruct::Empty && !landedOnNewIsland && (enemyNearMCV || isForeignTechMCV))
			{
				const bool onDisconnectedEnemyIsland
					= (homeGroundZone >= 0 && mcvZone >= 0 && mcvZone != homeGroundZone);

				if (onDisconnectedEnemyIsland && isForeignTechMCV && CanHouseFerryTypeAcrossWater(pHouse, pType))
				{
					UnitClass* pRescueTrans = nullptr;
					int bestRescueDist = 999999;

					for (auto const pT : UnitClass::Array)
					{
						// Never a transport a strategic plan is using (Airborne Front boards its own): a unit
						// asking it for a ride took its single radio link and kept the plan's soldiers out.
						if (!pT || pT->Owner != pHouse || pT->InLimbo || !pT->IsAlive || !pT->Type
							|| !IsAmphibiousOrAirTransport(pT->Type) || IsPlanMember(pHouse, pT->UniqueID) || IsInTransportOp(pHouse, pT->UniqueID))
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
				// Home (ForeignMcvHomeCells, NeuralAI.State.inl): near the yard or the placement cell of its own
				// yard, the cell its way home leads to; one that was home stays so until ForeignMcvHomeExitCells
				// farther. With no ferry it unfolds on the island it stands on, however far from home: the way
				// home by land does not exist.
				bool foreignMcvHome = false;
				const bool foreignMcvUnfoldsOnIsland = onDisconnectedEnemyIsland && !CanHouseFerryTypeAcrossWater(pHouse, pType);

				if (isForeignTechMCV)
				{
					auto& foreignHome = GetMacroState(pHouse).ForeignMcvHome;
					const double homeCells = ForeignMcvHomeCells
						+ (foreignHome.contains(pUnit->UniqueID) ? ForeignMcvHomeExitCells : 0);
					const CellStruct homeGoal = CachedTetrisBuildCell(pHouse, pType->DeploysInto);
					foreignMcvHome = distToHome <= homeCells
						|| (homeGoal != CellStruct::Empty && mcvCell.DistanceFrom(homeGoal) <= homeCells);

					if (foreignHome.size() > 64)
						foreignHome.clear();

					if (foreignMcvHome)
						foreignHome.insert(pUnit->UniqueID);
					else
						foreignHome.erase(pUnit->UniqueID);
				}

				if (enemyNearMCV || (isForeignTechMCV && !foreignMcvHome && !foreignMcvUnfoldsOnIsland))
				{
					// The goal it drove to lies past the danger: the expansion goals keep off it for a while
					// (it was picked again once the MCV was home, match #11862).
					if (enemyNearMCV && !isForeignTechMCV && mcvDangerNearItsGoal())
						NoteMcvFledSite(pHouse, pUnit, homeCoords);

					if (pUnit->CurrentMission != Mission::Move || (IsAITickWindow(60, 15)))
					{
						const CellStruct safeHomeCell
							= CachedTetrisBuildCell(pHouse, pType->DeploysInto);

						const CellStruct goalCoords = (safeHomeCell != CellStruct::Empty) ? safeHomeCell : homeCoords;

						if (auto const pHomeDest = MapClass::Instance.TryGetCellAt(goalCoords))
						{
							const NeuralOrderPriorityScope orderPriority { enemyNearMCV ? NeuralOrderPriority::Survival : NeuralOrderPriority::Routine };
							IssueMoveOrder(pUnit, pHomeDest, 3.0);
						}
					}

					continue;
				}

				if (isForeignTechMCV && !enemyNearMCV && (foreignMcvHome || foreignMcvUnfoldsOnIsland))
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
							NoteOrderCaller(pUnit, "move");
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
			// waiting at a beach for a ferry to another island (unitFreeLandField: mcvZone is the unit's
			// land zone, searched once per frame).
			CellClass* const pFreeLandFieldInMcvZone = (!isForeignTechMCV && mcvZone >= 0)
				? unitFreeLandField(pUnit)
				: nullptr;
			const bool hasFreeLandFieldInMcvZone = pFreeLandFieldInMcvZone != nullptr;
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
			// longer the goal and the next MCVs take the fields at home again. Only for the beachhead
			// on an enemy island now: an economic crossing waits for no free field at home
			// (islandCrossingNeeded below).
			const bool islandExpansionDue = conYardsInMcvZone >= 1 && pIslandResourceGoalCell != nullptr
				&& mcvZone != resourceIslandZone;
			// It crosses only for a field across the water: with no such goal an MCV that boarded a
			// transport stayed inside it for good (two hovercraft held four MCVs for 25000 frames on a
			// land map, match #8875, while their house had 100000 credits and one yard). And only with a
			// transport that makes that very crossing from where the MCV stands: hovercraft took four
			// MCVs for a plateau they could not climb (match #10267, one landmass).
			const bool canFerryThisMcvToIsland = pIslandResourceGoalCell != nullptr
				&& CanHouseFerryTypeAcrossWater(pHouse, pType, &mcvCell, &pIslandResourceGoalCell->MapCoords);
			// An economic crossing only when needed (yardMcvMayCrossToIslandGoal, the same free field
			// test as above): to an ore island with no free field on this landmass or no room here, never
			// to bare land, nor to an island given up for lack of room; the beachhead on an enemy island
			// keeps the island expansion due.
			const bool islandCrossingNeeded = !isNoRoomIsland(resourceIslandZone)
				&& (islandGoalKind == IslandGoalKind::EnemyBeachhead
					|| (islandGoalKind == IslandGoalKind::Ore && (!hasFreeLandFieldInMcvZone || mcvIslandHasNoRoom)));
			const bool wantsOtherIsland = (canFerryThisMcvToIsland && mcvZone >= 0 && islandCrossingNeeded
				&& (!hasFreeLandFieldInMcvZone || islandExpansionDue || mcvIslandHasNoRoom)
				&& (currentIslandAlreadyDeveloped || alreadyHasConYardOnThisIsland || mcvIslandHasNoRoom) && !isForeignTechMCV
				&& pIslandResourceGoalCell != nullptr && mcvZone != resourceIslandZone);
			const bool isSelfAmphibOrAirMcv = (pType->MovementZone == MovementZone::Amphibious
				|| pType->MovementZone == MovementZone::AmphibiousDestroyer
				|| pType->MovementZone == MovementZone::AmphibiousCrusher
				|| pType->MovementZone == MovementZone::Fly || pType->JumpJet);
			// Land before islands (IslandMcvWaitFrames, NeuralAI.State.inl): the wait for a ride to the
			// island counts from the first tick the MCV wants the island, whatever seats the ferry loader
			// reserves for it (those MCVs had a transport able to carry them in most of their lines). Once
			// it ran out, and the MCV is not boarding, it takes the free field of its own landmass, or with
			// none unfolds where it waits; its yard packs up for the island when a transport is free.
			bool islandWaitTakesLand = false;
			{
				auto& waitMacro = GetMacroState(pHouse);
				auto& waits = waitMacro.IslandMcvWaits;
				const int now = Unsorted::CurrentFrame;

				if (waits.size() > 32)
					std::erase_if(waits, [now](const auto& entry) { return now < entry.second.SeenFrame || now - entry.second.SeenFrame > 9000; });

				if (wantsOtherIsland && !isSelfAmphibOrAirMcv && !mcvIslandHasNoRoom && pHouse->NumConYards >= 1)
				{
					auto& wait = waits[pUnit->UniqueID];

					if (wait.SinceFrame < 0 || now < wait.SeenFrame || now - wait.SeenFrame > IslandMcvWaitGapFrames)
					{
						wait.SinceFrame = now;
						wait.LandField = CellStruct::Empty;
						wait.LandFrame = -1;
						wait.Released = false;
					}

					wait.SeenFrame = now;
					const int waited = now - wait.SinceFrame;
					const bool rideUnderWay = IsIslandMcvRideUnderWay(pHouse, pUnit);

					if (waited >= IslandMcvWaitFrames && (!rideUnderWay || waited >= 3 * IslandMcvWaitFrames))
					{
						CellClass* pLandField = nullptr;

						if (wait.LandField != CellStruct::Empty)
						{
							// The land was taken once: no more waiting for the ferry. Past
							// IslandMcvLandFieldFrames the expansion logic below picks its own field.
							islandWaitTakesLand = true;

							if (now - wait.LandFrame <= IslandMcvLandFieldFrames)
								pLandField = MapClass::Instance.TryGetCellAt(wait.LandField);
						}
						else if (pFreeLandFieldInMcvZone)
						{
							islandWaitTakesLand = true;
							pLandField = pFreeLandFieldInMcvZone;
							// TEMP diagnostic: an island expansion MCV that takes a field of its own landmass.
							Debug::Log("[NeuralAI] House %d IslandExpansion MCV: %s %u waited %d frames at (%d,%d) for a ride (ride under way %d), "
								"takes the free land field (%d,%d) %d cells away, frame %d.\n",
								pHouse->ArrayIndex, pType->ID, pUnit->UniqueID, waited, mcvCell.X, mcvCell.Y, rideUnderWay ? 1 : 0,
								pLandField->MapCoords.X, pLandField->MapCoords.Y,
								static_cast<int>(mcvCell.DistanceFrom(pLandField->MapCoords)), now);
							wait.LandField = pLandField->MapCoords;
							wait.LandFrame = now;
						}

						wait.Released = wait.Released || islandWaitTakesLand;

						if (pLandField)
						{
							if (mcvCell.DistanceFrom(pLandField->MapCoords) > IslandMcvLandFieldNearCells)
							{
								const bool headingThere = isMovingToDest && pUnit->Destination
									&& CellClass::Coord2Cell(pUnit->Destination->GetCoords()).DistanceFrom(pLandField->MapCoords) <= 3.0;

								if (!headingThere || IsAITickWindow(90, 25))
								{
									IssueMoveOrder(pUnit, pLandField, 3.0);
									NoteTeamExpansionSite(pHouse, pLandField->MapCoords);
								}

								continue;
							}

							// Within reach of the field: the expansion logic below unfolds it there.
						}
						else if (!islandWaitTakesLand && !enemyNearMCV && (waitMacro.IslandWaitPackFrame < 0 || now < waitMacro.IslandWaitPackFrame
							|| now - waitMacro.IslandWaitPackFrame >= IslandWaitRepackFrames))
						{
							// No free field on its landmass: it unfolds where it waits, a yard more for the
							// building queue (user, Fluddle), until a transport is free for the crossing.
							if (CanDeployerUnpackHere(pHouse, pType->DeploysInto, pUnit))
							{
								// TEMP diagnostic: an island expansion MCV that unfolds while it waits.
								Debug::Log("[NeuralAI] House %d IslandExpansion MCV: %s %u waited %d frames at (%d,%d) for a ride (ride under way %d), "
									"no free land field, unfolds there meanwhile, %d yards, frame %d.\n",
									pHouse->ArrayIndex, pType->ID, pUnit->UniqueID, waited, mcvCell.X, mcvCell.Y, rideUnderWay ? 1 : 0,
									pHouse->NumConYards, now);

								if (waitMacro.IslandWaitYards.size() >= 4)
									waitMacro.IslandWaitYards.erase(waitMacro.IslandWaitYards.begin());

								waitMacro.IslandWaitYards.emplace_back(mcvCell, now);
								waits.erase(pUnit->UniqueID);
								OrderDeployerUnpack(pUnit);
								continue;
							}

							const CellStruct unfoldCell = FindDeployableFoundationNearCell(pHouse, pType->DeploysInto, mcvCell, mcvZone, pUnit);

							if (unfoldCell != CellStruct::Empty)
							{
								wait.Released = true;
								const bool headingThere = isMovingToDest && pUnit->Destination
									&& CellClass::Coord2Cell(pUnit->Destination->GetCoords()) == unfoldCell;

								if (!headingThere || IsAITickWindow(45, 15))
								{
									if (auto const pUnfoldCell = MapClass::Instance.TryGetCellAt(unfoldCell))
										IssueMoveOrder(pUnit, pUnfoldCell, 1.0);
								}

								continue;
							}

							// No site at all: it keeps waiting for its ride.
						}
					}
				}
				else
				{
					waits.erase(pUnit->UniqueID);
				}
			}

			const bool mustColonizeOtherIsland = wantsOtherIsland && !islandWaitTakesLand;
			// The yard the bank governor wants for the building queue unfolds at home on any map and
			// next to the yards already there: it is wanted for its speed, not for new ground.
			const bool bankWantsYardHere = GetMacroState(pHouse).BankWantedYards > pHouse->NumConYards && !isForeignTechMCV
				&& !isSecondaryIsland;
			const bool wantsMultiConYardSpeed = (pHouse->NumConYards >= 1 && !mustColonizeOtherIsland
				&& (bankWantsYardHere
					|| (!alreadyHasConYardOnThisIsland && !isArchipelagoOrNavalMap
						&& ((scores[30] > 650 && scores[35] <= 60 && scores[81] <= 90)
							|| (!canFerryThisMCV && !hasUncolonizedOreOnHomeIsland)))));
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

			// Island expansion due (UpdateIslandOreOutlook): where its MCV is and what it was told,
			// every 2700 frames, so a crossing that never happens shows up in the log.
			if (auto& islandMacro = GetMacroState(pHouse); islandMacro.IslandExpansionUrgent
				&& Unsorted::CurrentFrame - islandMacro.IslandMcvLogFrame >= 2700)
			{
				islandMacro.IslandMcvLogFrame = Unsorted::CurrentFrame;
				Debug::Log("[NeuralAI] House %d IslandExpansion MCV: %s %u at (%d,%d) zone %d mission %d, colonize %d (free land field %d, "
					"goal %s zone %d), enemy near %d, transport able %d, frame %d.\n",
					pHouse->ArrayIndex, pType->ID, pUnit->UniqueID, mcvCell.X, mcvCell.Y, mcvZone, static_cast<int>(pUnit->CurrentMission),
					mustColonizeOtherIsland ? 1 : 0, hasFreeLandFieldInMcvZone ? 1 : 0, islandGoalKindName(),
					resourceIslandZone, enemyNearMCV ? 1 : 0, ownsLiveMcvTransportNow ? 1 : 0, Unsorted::CurrentFrame);
			}

			// Stranded on an island without room and no other island to go to: back home.
			if (mcvIslandHasNoRoom && !mustColonizeOtherIsland && conYardsInMcvZone == 0 && mcvZone != homeGroundZone
				&& homeCoords != CellStruct::Empty)
			{
				if (!isMovingToDest || IsAITickWindow(300, 15))
				{
					if (auto const pHomeCell = MapClass::Instance.TryGetCellAt(homeCoords))
						DispatchFootToTargetOrTransport(pUnit, pHomeCell, Mission::Move, false);
				}

				continue;
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
							// Never a transport a strategic plan is using (Airborne Front boards its own): a unit
							// asking it for a ride took its single radio link and kept the plan's soldiers out.
							if (!pT || pT->Owner != pHouse || pT->InLimbo || !pT->IsAlive || !pT->Type
								|| !IsAmphibiousOrAirTransport(pT->Type) || IsPlanMember(pHouse, pT->UniqueID) || IsInTransportOp(pHouse, pT->UniqueID))
							{
								continue;
							}

							if (static_cast<int>(pType->TechnoTypeClass::Size) > static_cast<int>(pT->Type->SizeLimit)
								|| (pT->Passengers.GetTotalSize() + static_cast<int>(pType->TechnoTypeClass::Size))
									> pT->Type->Passengers
								|| !CanTransportTypeLink(pT->Type, mcvCell, pIslandResourceGoalCell->MapCoords))
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
				auto& noSiteSince = GetMacroState(pHouse).DeployerNoSiteSince;

				if (CanDeployerUnpackHere(pHouse, pType->DeploysInto, pUnit))
				{
					noSiteSince.erase(pUnit->UniqueID);
					OrderDeployerUnpack(pUnit);
				}
				else
				{
					const CellStruct deployCell
						= FindDeployableFoundationNearCell(pHouse, pType->DeploysInto, mcvCell, mcvZone, pUnit);
					if (deployCell != CellStruct::Empty)
					{
						noSiteSince.erase(pUnit->UniqueID);

						if (!isMovingToDest || (IsAITickWindow(45, 15)))
						{
							if (auto const pClear = MapClass::Instance.TryGetCellAt(deployCell))
							{
								pUnit->SetDestination(pClear, true);
								pUnit->QueueMission(Mission::Move, true);
								NoteOrderCaller(pUnit, "move");
							}
						}
					}
					else if (landedOnNewIsland)
					{
						// No site for the yard anywhere on this island. Something passing may only be
						// in the way, so the MCV gives it DeployerNoSiteFrames; after that the island
						// search skips the island and the MCV asks for a transport to its next goal,
						// or back home when there is none.
						auto const [itSince, firstTime] = noSiteSince.try_emplace(pUnit->UniqueID, Unsorted::CurrentFrame);

						if (!firstTime && Unsorted::CurrentFrame - itSince->second >= DeployerNoSiteFrames)
						{
							mcvNoRoomIslands[mcvZone] = Unsorted::CurrentFrame + NoRoomIslandFrames;
							noSiteSince.erase(itSince);
							CountDecision(NeuralDecision::DeployerIslandNoRoom, pHouse->ArrayIndex);
						}
						else
						{
							pUnit->Scatter(CoordStruct::Empty, true, false);
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
				if (SlotGateHeld(scores[81], 75, 81, pUnit->UniqueID) && (pHouse->CountResourceDestinations >= 1 || pHouse->OwnedBuildings >= 5))
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

							bool nearOwnHubOrEnemyTurret = !IsMcvSiteSafe(pHouse, mcvDangers, candStage);
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

						if (deployableThere && IsMcvSiteSafe(pHouse, mcvDangers, claimCell))
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

							if (fieldCells < 4 || !IsMcvSiteSafe(pHouse, mcvDangers, tc))
								continue;

							bestExpDist = distFromMCV;
							pExpansionDest = pCell;
						}
					}
				}

				// A free start location is one more candidate next to the fields (StartSpot*, NeuralAI.State.inl).
				if (!siegeDestFound && mcvZone >= 0)
				{
					int bestStart = -1;
					int bestStartMetric = bestExpDist;
					CellStruct bestStartCell = CellStruct::Empty;

					for (int i = 0; i < MaxStartWaypoints; ++i)
					{
						CellStruct startCell = CellStruct::Empty;

						if (!IsFreeStartSpot(i, startCell)
							|| MapClass::Instance.GetMovementZoneType(startCell, MovementZone::Normal, false) != mcvZone)
						{
							continue;
						}

						const int metric = std::abs(startCell.X - mcvCell.X) + std::abs(startCell.Y - mcvCell.Y) - StartSpotBonusCells;

						if (metric >= bestStartMetric || !IsMcvSiteSafe(pHouse, mcvDangers, startCell)
							|| IsExpansionCellTakenByTeam(pHouse, startCell))
						{
							continue;
						}

						bestStart = i;
						bestStartMetric = metric;
						bestStartCell = startCell;
					}

					if (bestStart >= 0)
					{
						if (auto const pStartDest = MapClass::Instance.TryGetCellAt(bestStartCell))
						{
							// TEMP diagnostic: an expansion MCV sent to a free start location (StartSpotLogLines).
							static int startSpotLogSerial = -1;
							static int startSpotLogCount = 0;

							if (startSpotLogSerial != NeuralMatchSerial)
							{
								startSpotLogSerial = NeuralMatchSerial;
								startSpotLogCount = 0;
							}

							if (pUnit->Destination != pStartDest && startSpotLogCount++ < StartSpotLogLines)
							{
								Debug::Log("[NeuralAI] House %d expansion: %s %u heads for the free start location %d at (%d,%d), %d cells away "
									"(nearest field %d), frame %d.\n", pHouse->ArrayIndex, pType->ID, pUnit->UniqueID, bestStart, bestStartCell.X,
									bestStartCell.Y, bestStartMetric + StartSpotBonusCells, pExpansionDest ? bestExpDist : -1, Unsorted::CurrentFrame);
							}

							pExpansionDest = pStartDest;
						}
					}
				}

				// Nothing in the local scan: the nearest free field anywhere on our landmass.
				if (!pExpansionDest)
				{
					pExpansionDest = FindFreeOreFieldCell(pHouse, mcvCell, mcvZone, 14);

					if (pExpansionDest && !IsMcvSiteSafe(pHouse, mcvDangers, pExpansionDest->MapCoords))
						pExpansionDest = nullptr;
				}

				if (teamYieldedSite)
					CountDecision(NeuralDecision::TeamExpansionYield, pHouse->ArrayIndex);

				if (pExpansionDest)
				{
					IssueMoveOrder(pUnit, pExpansionDest, 3.0);
					NoteTeamExpansionSite(pHouse, pExpansionDest->MapCoords);
				}
				else if (!enemyNearMCV && !twinMcvNearby)
				{
					// No field to take and no crossing to make (mustColonizeOtherIsland is false): the MCV
					// unfolds here, a yard more for the building queue, even with a yard on this landmass
					// already. Kept packed it stood idle in its base for the rest of the match (#10267).
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
				// The ground the deployer stands on. On water or on a cell of no ground its own zone holds
				// no site for a land building, so it reads the zone of the main base (user approved,
				// 2026-11-19, match #11506: from the water its site searches saw water cells only). A
				// building on water (IsWaterBuildingType) takes water sites only (siteZone -1: the base
				// placement search puts it on the water by our base).
				const bool deploysOnWater = IsWaterBuildingType(pTargetBld);
				const int nanoCellZone = MapClass::Instance.GetMovementZoneType(nanoCell, MovementZone::Normal, pUnit->OnBridge);
				auto const pNanoCell = pUnit->GetCell();
				const bool nanoOnGround = IsGroundZone(nanoCellZone) && pNanoCell && pNanoCell->LandType != LandType::Water;
				const int nanoZone = (nanoOnGround || !IsGroundZone(homeGroundZone)) ? nanoCellZone : homeGroundZone;
				const int siteZone = deploysOnWater ? -1 : nanoZone;
				const bool isNanoMoving = (pUnit->CurrentMission == Mission::Move && pUnit->Destination != nullptr
					&& pUnit->DistanceFrom(pUnit->Destination) > 768);
				const bool nanoReachedOrNoDest
					= (pUnit->CurrentMission != Mission::Move || pUnit->Destination == nullptr
						|| CellClass::Coord2Cell(pUnit->Destination->GetCoords()) == nanoCell);

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
				}

				// Only a deployer that expands with its building goes colonizing (deployerExpandsWith, the
				// predicate of the deployer pick and the airlifts too, so it is never ferried to an island
				// this logic sends it back from): BaseNormal, which most support buildings have, made a
				// deployer of a shield generator an outpost that never reached the base logic below (match
				// #11506). A factory deployer crosses only to an island goal its building fits on
				// (deployerFitsOnIslandGoal).
				const bool deploysColony = isColonyBuilding(pTargetBld);
				const bool expandsWithBuilding = deployerExpandsWith(pTargetBld);
				const bool canFerryDeployer = CanHouseFerryTypeAcrossWater(pHouse, pType);
				const bool isCrossIslandFactoryDeployer = (expandsWithBuilding && pTargetBld->Factory != AbstractType::None
					&& canFerryDeployer && pIslandResourceGoalCell != nullptr && nanoZone != resourceIslandZone
					&& deployerFitsOnIslandGoal(pUnit, pTargetBld));

				const bool isExpansionDeployer = isIslandColonizerSlaveMiner || isCrossIslandFactoryDeployer
					|| (expandsWithBuilding && (pTargetBld->Refinery || pTargetBld->ResourceDestination));

				auto& siteMacro = GetMacroState(pHouse);
				const int siteNow = Unsorted::CurrentFrame;

				if (siteMacro.DeployerSites.size() > 64)
				{
					std::erase_if(siteMacro.DeployerSites, [siteNow](const auto& entry)
						{ return siteNow < entry.second.SeenFrame || siteNow - entry.second.SeenFrame > 900; });
				}

				auto& deploySite = siteMacro.DeployerSites[pUnit->UniqueID];

				// The site of a deployer of the base (DeployerSites, user approved, 2026-11-19, match #11506:
				// the site search near the deployer ignores Adjacent): the cell of the base placement search
				// (CachedTetrisBuildCell, as FindBestDeployStandCell reads it: Adjacent and the placement
				// rules, water by our shore for a building on water), else the main base, avoiding the sites
				// it failed at. Sent there by the idle watchdog (toMainBase), a land building takes the legal
				// foundation nearest the main base in its ground zone. A deployer of a land building standing
				// on the ground of another island than the main base's takes its site on that island (the
				// placement cell when it lies there, else the nearest foundation): sent home for a site of
				// the main base, a deployer ferried there was picked for the island again. A water site is
				// one the deployer reaches by its own moves: the search goes on past the ones it does not
				// (DeployerWaterSiteTries searches at most).
				auto const pickBaseDeploySite = [&](bool toMainBase) -> CellStruct
				{
					const CellStruct standOffset = (pTargetBld->GetFoundationWidth() > 2 || pTargetBld->GetFoundationHeight(false) > 2)
						? CellStruct { 1, 1 }
						: CellStruct { 0, 0 };
					const bool onOtherIsland = !toMainBase && !deploysOnWater && nanoOnGround && IsGroundZone(homeGroundZone)
						&& nanoCellZone != homeGroundZone;
					const CellStruct tetris = (!toMainBase || deploysOnWater) ? CachedTetrisBuildCell(pHouse, pTargetBld) : CellStruct::Empty;
					CellStruct tetrisStand = tetris != CellStruct::Empty ? tetris + standOffset : CellStruct::Empty;

					if (onOtherIsland && tetrisStand != CellStruct::Empty
						&& MapClass::Instance.GetMovementZoneType(tetrisStand, MovementZone::Normal, false) != nanoCellZone)
					{
						tetrisStand = CellStruct::Empty;
					}

					const CellStruct origins[2] = { tetrisStand, onOtherIsland ? nanoCell : homeCoords };
					const int ownMoveZone = ObjectZone(pUnit, pType->MovementZone);
					std::vector<CellStruct> avoid = deploySite.FailedSites;

					for (auto const& origin : origins)
					{
						if (origin == CellStruct::Empty)
							continue;

						const int originZone = MapClass::Instance.GetMovementZoneType(origin, MovementZone::Normal, false);
						const int zone = deploysOnWater ? -1
							: (onOtherIsland ? nanoCellZone : ((toMainBase || !IsGroundZone(originZone)) ? homeGroundZone : originZone));

						for (int search = 0; search < DeployerWaterSiteTries; ++search)
						{
							const CellStruct cell = FindDeployableFoundationNearCellUncached(pHouse, pTargetBld, origin, zone, pUnit, &avoid);

							if (cell == CellStruct::Empty)
								break;

							if (!deploysOnWater || MapClass::Instance.GetMovementZoneType(cell, pType->MovementZone, false) == ownMoveZone)
								return cell;

							avoid.push_back(cell);
						}
					}

					return CellStruct::Empty;
				};

				// It goes to its site and deploys there; the site changes only when the deploy fails on it
				// (DeployerSiteMostTries unpack orders there, or McvIdleArriveFrames on or next to it without
				// deploying, as the idle MCV watchdog steps an MCV onto its site): the site goes to the avoid
				// list for DeployerFailedSiteFrames and the next one is taken. One boarding a transport is
				// left to it.
				auto const deployAtBaseSite = [&](bool toMainBase)
				{
					if (pUnit->IsDeploying || pUnit->CurrentMission == Mission::Enter || pUnit->QueuedMission == Mission::Enter
						|| (pUnit->CurrentMission == Mission::Unload && deploySite.TryFrame >= 0
							&& siteNow >= deploySite.TryFrame && siteNow - deploySite.TryFrame < McvUnfoldRetryFrames))
					{
						return;
					}

					for (size_t failed = deploySite.FailedSites.size(); failed-- > 0;)
					{
						if (failed >= deploySite.FailedFrames.size() || siteNow < deploySite.FailedFrames[failed]
							|| siteNow - deploySite.FailedFrames[failed] > DeployerFailedSiteFrames)
						{
							deploySite.FailedSites.erase(deploySite.FailedSites.begin() + static_cast<std::ptrdiff_t>(failed));

							if (failed < deploySite.FailedFrames.size())
								deploySite.FailedFrames.erase(deploySite.FailedFrames.begin() + static_cast<std::ptrdiff_t>(failed));
						}
					}

					if (deploySite.Site == CellStruct::Empty && (deploySite.PickFrame < 0 || siteNow < deploySite.PickFrame
						|| siteNow - deploySite.PickFrame >= DeployerSiteRetryFrames))
					{
						deploySite.PickFrame = siteNow;
						deploySite.Site = pickBaseDeploySite(toMainBase);
						deploySite.Tries = 0;
						deploySite.NearSince = -1;
					}

					if (deploySite.Site == CellStruct::Empty)
					{
						// No site at all for now: it deploys where it stands when it can.
						if (CanDeployerUnpackHere(pHouse, pTargetBld, pUnit))
							OrderDeployerUnpack(pUnit);

						return;
					}

					const CellStruct site = deploySite.Site;
					const double siteDist = nanoCell.DistanceFrom(site);
					const bool driving = pUnit->CurrentMission == Mission::Move && pUnit->Destination
						&& pUnit->DistanceFrom(pUnit->Destination) > 256;

					auto const failSite = [&]()
					{
						deploySite.FailedSites.push_back(site);
						deploySite.FailedFrames.push_back(siteNow);
						deploySite.Site = CellStruct::Empty;
						deploySite.PickFrame = -1;
						deploySite.Tries = 0;
						deploySite.NearSince = -1;
						CountDecision(NeuralDecision::DeployerSiteFailed, pHouse->ArrayIndex);
					};

					if ((nanoCell == site || (siteDist <= 1.5 && !driving)) && CanDeployerUnpackHere(pHouse, pTargetBld, pUnit))
					{
						if (deploySite.Tries >= DeployerSiteMostTries)
						{
							failSite();
							return;
						}

						++deploySite.Tries;
						deploySite.TryFrame = siteNow;
						OrderDeployerUnpack(pUnit);
						return;
					}

					// On the site and refused (units on the foundation, which the check asks to step aside), or
					// next to it: it fails only when that lasts McvIdleArriveFrames.
					if (siteDist <= McvIdleNearCells && !driving)
					{
						if (deploySite.NearSince < 0)
							deploySite.NearSince = siteNow;

						if (siteNow - deploySite.NearSince >= McvIdleArriveFrames)
						{
							failSite();
							return;
						}

						if (nanoCell == site)
							return;

						if (auto const pSiteCell = MapClass::Instance.TryGetCellAt(site); pSiteCell && pUnit->Destination != pSiteCell)
						{
							pUnit->SetDestination(pSiteCell, true);
							pUnit->QueueMission(Mission::Move, true);
							NoteOrderCaller(pUnit, "move");
						}

						return;
					}

					deploySite.NearSince = -1;
					const bool headingThere = driving && CellClass::Coord2Cell(pUnit->Destination->GetCoords()).DistanceFrom(site) <= 1.5;

					if (!headingThere || IsAITickWindow(90, 15))
					{
						if (auto const pSiteCell = MapClass::Instance.TryGetCellAt(site))
						{
							const int siteGround = MapClass::Instance.GetMovementZoneType(site, MovementZone::Normal, false);
							const bool reachesOnItsOwn = (IsGroundZone(siteGround) && siteGround == nanoCellZone)
								|| MapClass::Instance.GetMovementZoneType(site, pType->MovementZone, false) == ObjectZone(pUnit, pType->MovementZone);

							if (reachesOnItsOwn)
								IssueMoveOrder(pUnit, pSiteCell, 0.0);
							else
								DispatchFootToTargetOrTransport(pUnit, pSiteCell, Mission::Move, false);
						}
					}
				};

				// Idle deployer watchdog (DeployerIdleUndeployedFrames, NeuralAI.State.inl; user approved,
				// 2026-11-19, match #11506): a deployer packed for that long, or keeping to one spot for
				// DeployerIdleStillFrames, goes to the main base and deploys at the legal foundation nearest
				// it (for a building on water, on the water by the base). A slave miner deploys through its
				// harvest, a defense deployer has its own logic below, one boarding a transport is left to it,
				// and one on its way to an island goal counts its packed time only. A ride restarts the packed
				// time: it counts from the boarding and again from the landing on other ground (a deployer
				// that waited on the shore landed with its time all but run out and was sent back at once);
				// one that crosses water by itself keeps its time from land to land.
				if (!pTargetBld->IsBaseDefense)
				{
					if (deploySite.SeenFrame < 0 || siteNow < deploySite.SeenFrame || siteNow - deploySite.SeenFrame > 900)
					{
						deploySite = NeuralDeployerSite {};
						deploySite.FirstFrame = siteNow;
					}

					deploySite.SeenFrame = siteNow;
					const bool deployerBoarding = pUnit->CurrentMission == Mission::Enter || pUnit->QueuedMission == Mission::Enter;
					const bool headsForIsland = isExpansionDeployer && canFerryDeployer && pIslandResourceGoalCell != nullptr
						&& nanoZone != resourceIslandZone;
					const int groundZoneNow = nanoOnGround ? nanoCellZone : -1;

					if (deployerBoarding || (groundZoneNow >= 0 && deploySite.Zone >= 0 && groundZoneNow != deploySite.Zone
						&& !CanFootCrossWaterDirectly(pUnit)))
					{
						deploySite.FirstFrame = siteNow;
					}

					if (groundZoneNow >= 0)
						deploySite.Zone = groundZoneNow;

					if (deploySite.StillCell == CellStruct::Empty || nanoCell.DistanceFrom(deploySite.StillCell) > DeployerIdleDriftCells
						|| deployerBoarding || headsForIsland || pUnit->IsDeploying)
					{
						deploySite.StillCell = nanoCell;
						deploySite.StillSince = siteNow;
					}

					const int packedFrames = siteNow - deploySite.FirstFrame;
					const int stillFrames = siteNow - deploySite.StillSince;

					if (!deploySite.ToMainBase && !isIslandColonizerSlaveMiner && !deployerBoarding
						&& (packedFrames >= DeployerIdleUndeployedFrames || stillFrames >= DeployerIdleStillFrames))
					{
						deploySite.ToMainBase = true;
						deploySite.Site = CellStruct::Empty;
						deploySite.PickFrame = -1;
						deploySite.Tries = 0;
						deploySite.NearSince = -1;
						CountDecision(NeuralDecision::DeployerIdleWatchdog, pHouse->ArrayIndex);

						// TEMP diagnostic: each deployer the watchdog sends (DeployerIdleLogLines a match).
						static int deployerWatchLogSerial = -1;
						static int deployerWatchLogCount = 0;

						if (deployerWatchLogSerial != NeuralMatchSerial)
						{
							deployerWatchLogSerial = NeuralMatchSerial;
							deployerWatchLogCount = 0;
						}

						if (deployerWatchLogCount < DeployerIdleLogLines)
						{
							++deployerWatchLogCount;
							Debug::Log("[NeuralAI] House %d deployer watchdog: %s %u (%s, on water %d, colony %d, expansion %d) at (%d,%d) "
								"zone %d cell zone %d, packed %d frames, still %d frames, mission %d, %d sites failed, goes to the main base "
								"(%d,%d) zone %d, frame %d.\n",
								pHouse->ArrayIndex, pType->ID, pUnit->UniqueID, pTargetBld->ID, deploysOnWater ? 1 : 0, deploysColony ? 1 : 0,
								isExpansionDeployer ? 1 : 0, nanoCell.X, nanoCell.Y, nanoZone, nanoCellZone, packedFrames, stillFrames,
								static_cast<int>(pUnit->CurrentMission), static_cast<int>(deploySite.FailedSites.size()), homeCoords.X,
								homeCoords.Y, homeGroundZone, siteNow);
						}
					}

					if (deploySite.ToMainBase && !isIslandColonizerSlaveMiner)
					{
						deployAtBaseSite(true);
						continue;
					}
				}

				if (isExpansionDeployer)
				{
					auto& noRoomIslands = GetMacroState(pHouse).NoRoomIslandUntil;
					const bool nanoIslandHasNoRoom = nanoZone >= 0 && isNoRoomIslandFor(pTargetBld, nanoZone);
					const bool mustColonizeIslandWithDeployer = (canFerryDeployer && nanoZone >= 0
						&& (hubsInNanoZone >= 1 || nanoIslandHasNoRoom) && pIslandResourceGoalCell != nullptr
						&& nanoZone != resourceIslandZone && deployerFitsOnIslandGoal(pUnit, pTargetBld));
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
									// Never a transport a strategic plan is using (Airborne Front boards its own): a unit
									// asking it for a ride took its single radio link and kept the plan's soldiers out.
									if (!pT || pT->Owner != pHouse || pT->InLimbo || !pT->IsAlive || !pT->Type
										|| !IsAmphibiousOrAirTransport(pT->Type) || IsPlanMember(pHouse, pT->UniqueID) || IsInTransportOp(pHouse, pT->UniqueID))
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
								= FindDeployableFoundationNearCell(pHouse, pTargetBld, nanoCell, siteZone, pUnit);
							if (deployCell != CellStruct::Empty)
							{
								if (nanoReachedOrNoDest || (IsAITickWindow(45, 15)))
								{
									if (auto const pClear = MapClass::Instance.TryGetCellAt(deployCell))
									{
										pUnit->SetDestination(pClear, true);
										pUnit->QueueMission(Mission::Move, true);
										NoteOrderCaller(pUnit, "move");
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
								// island search skips it for 9000 frames and the deployer moves on. The
								// mark is kept for this building type as well (NoRoomTypeIslandUntil).
								noRoomIslands[nanoZone] = Unsorted::CurrentFrame + NoRoomIslandFrames;
								GetMacroState(pHouse).NoRoomTypeIslandUntil[{ pTargetBld->ArrayIndex, nanoZone }]
									= Unsorted::CurrentFrame + NoRoomIslandFrames;
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
							const CellStruct deployCell = FindBestDeployStandCell(pHouse, pTargetBld, pUnit, siteZone);
							if (auto const pClear = MapClass::Instance.TryGetCellAt(deployCell))
							{
								pUnit->SetDestination(pClear, true);
								pUnit->QueueMission(Mission::Move, true);
								NoteOrderCaller(pUnit, "move");
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
						const CellStruct perimStandCell = FindBestDeployStandCell(pHouse, pTargetBld, pUnit, siteZone);
						if (perimStandCell == nanoCell && CanDeployerUnpackHere(pHouse, pTargetBld, pUnit))
						{
							OrderDeployerUnpack(pUnit);
							continue;
						}

						if (auto const pPerim = MapClass::Instance.TryGetCellAt(perimStandCell))
						{
							pUnit->SetDestination(pPerim, true);
							pUnit->QueueMission(Mission::Move, true);
							NoteOrderCaller(pUnit, "move");
							continue;
						}
					}

					continue;
				}

				// Base Infrastructure Deployer (Power, Barracks, Radar, Tech, Repair Bay, Shield/Gap
				// Generator, a naval yard on the water, or Refinery/Factory): it deploys at the site of the
				// base placement search it keeps (deployAtBaseSite above).
				deployAtBaseSite(false);
				continue;
			}
			// A siege unit stuck on an unfold that never happens goes back on guard, where the rules
			// below pick it up again.
			else if (IsSiegeUnfoldStuck(pUnit))
			{
				pUnit->ForceMission(Mission::Guard);

				continue;
			}
			// A unit that unfolds to fire does it only at a target the deployed gun reaches and can
			// hit, while it fights or stands guard: unfolding at anything else (out of reach, submerged
			// or flying) only starts a loop of unfolding and packing up.
			else if (pUnit->Target != nullptr
				&& (pUnit->CurrentMission == Mission::Attack || pUnit->CurrentMission == Mission::Guard
					|| pUnit->CurrentMission == Mission::Area_Guard || pUnit->CurrentMission == Mission::Hunt)
				&& DeployedGunCanHit(pUnit->GetCoords(), pUnit->Target, GroundRangeOf(pType->DeploysInto)))
			{
				OrderDeployerUnpack(pUnit);

				continue;
			}
			// Siege units (Tick Tank, artillery) are left to the rule above: an enemy vehicle near them
			// may be one their gun cannot hit, such as a flying one.
			else if (pTargetBld != nullptr && !IsTrueBaseStructureTarget(pTargetBld) && !IsDeployableSiegeUnitType(pType))
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
		const bool parasiteToRemove = pUnit->ParasiteEatingMe != nullptr && SlotGateHeld(scores[64], 60, 64, pUnit->UniqueID);

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
		if (pGrinderBld && !pType->Naval && SlotGateHeld(scores[65], 80, 65, pUnit->UniqueID)
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
		if (pUnitReloadBld && pType->ManualReload && pType->Ammo > 0 && pUnit->Ammo == 0 && SlotGateHeld(scores[67], 60, 67, pUnit->UniqueID))
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
		if (pType->TiberiumHeal && pUnit->GetHealthPercentage() < 0.55 && SlotGateHeld(scores[68], 60, 68, pUnit->UniqueID)
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

		// Repairers (Repairer*: a repair vehicle, the Repair Disk, an IFV with an engineer aboard) go with
		// our vehicles as the medics go with the soldiers. Repairing first: a damaged own or allied vehicle
		// (or one a parasite eats) on its ground within RepairerHealCells, whatever it was doing. Else it
		// follows the nearest own armed vehicle out in the field (not at IsAtOwnBase) on the move or in a
		// fight on its ground (RepairerEscortMost repairers per vehicle at most; the one followed now is
		// kept while it still is), keeping within RepairerEscortKeepCells of it. With nobody out it repairs
		// the nearest damaged vehicle or building at our bases and expansions (FindBaseRepairTarget; user,
		// 2026-11-19), else it stays where it is. A flying repairer (MDISK) reaches any ground. Every
		// order is direct: no transport operation for a repair across water (one was opened for a TSREPAIR).
		if (IsRepairerUnit(pUnit))
		{
			auto const pRepairTarget = abstract_cast<TechnoClass*>(pUnit->Target);

			if (pRepairTarget && pRepairTarget->IsAlive && !pRepairTarget->InLimbo && pRepairTarget->Owner
				&& pHouse->IsAlliedWith(pRepairTarget->Owner) && pRepairTarget->GetHealthPercentage() < 1.0)
			{
				continue;
			}

			const bool repairerFlies = pType->MovementZone == MovementZone::Fly || pType->JumpJet || pType->BalloonHover;
			const int repairerZone = repairerFlies ? -1 : ObjectZone(pUnit, MovementZone::Normal);
			auto const onRepairerGround = [repairerZone](const ObjectClass* pOther)
			{
				return repairerZone < 0 || ObjectZone(pOther, MovementZone::Normal) == repairerZone;
			};
			auto& repairerMacro = GetMacroState(pHouse);
			UnitClass* pDamaged = nullptr;
			int damagedDist = INT_MAX;

			for (auto const pAllyU : UnitClass::Array)
			{
				if (!pAllyU || pAllyU == pUnit || !pAllyU->Owner || !pHouse->IsAlliedWith(pAllyU->Owner) || pAllyU->InLimbo
					|| !pAllyU->IsAlive || !pAllyU->Type || pAllyU->Type->Organic || pAllyU->IsInAir()
					|| (pAllyU->ParasiteEatingMe == nullptr && pAllyU->GetHealthPercentage() >= RepairerDamagedBelow))
				{
					continue;
				}

				// One a parasite eats from as far as the de-parasiting rule (Slot 64) went: 16 cells.
				const int dist = pUnit->DistanceFrom(pAllyU);
				const int reach = pAllyU->ParasiteEatingMe ? 16 * Unsorted::LeptonsPerCell : RepairerHealCells * Unsorted::LeptonsPerCell;

				if (dist <= reach && dist < damagedDist && onRepairerGround(pAllyU) && HealingWeaponFor(pUnit, pAllyU))
				{
					damagedDist = dist;
					pDamaged = pAllyU;
				}
			}

			if (pDamaged)
			{
				pUnit->ClearMegaMissionData();
				pUnit->SetTarget(pDamaged);
				pUnit->QueueMission(Mission::Attack, true);
				NoteOrderCaller(pUnit, "repair");

				// TEMP diagnostic: the repairers' orders (RepairerLogLines per house and match).
				if (repairerMacro.RepairerLogCount < RepairerLogLines)
				{
					repairerMacro.RepairerLogCount++;
					Debug::Log("[NeuralAI] Frame %d | House #%d repairer %s %u repairs %s %u at %d%% %d cells away\n", Unsorted::CurrentFrame,
						pHouse->ArrayIndex, pType->ID, pUnit->UniqueID, pDamaged->Type->ID, pDamaged->UniqueID,
						static_cast<int>(pDamaged->GetHealthPercentage() * 100.0), damagedDist / Unsorted::LeptonsPerCell);
				}

				continue;
			}

			auto const isArmedGroundVehicle = [pHouse](UnitClass* pVehicle) -> bool
			{
				return pVehicle && pVehicle->Owner == pHouse && !pVehicle->InLimbo && pVehicle->IsAlive && pVehicle->Type
					&& !pVehicle->Type->Naval && !pVehicle->Type->Harvester && !pVehicle->Type->ResourceGatherer
					&& pVehicle->Type->GetWeapon(0) && pVehicle->Type->GetWeapon(0)->WeaponType && !IsRepairerUnit(pVehicle);
			};

			// A vehicle in a group (user, 2026-11-19: approved «Sí, hazlo»): RepairerEscortGroupMin armed
			// ground vehicles of ours within RepairerEscortGroupCells of it, itself included, counted once
			// per vehicle, tick and house. A lone raider is no army: 20 of 35 follows of 2026-11-19 went after
			// TSBGGY buggies, 13 of them after one buggy on Attack.
			static int groupSizesFrame = -1;
			static int groupSizesHouse = -1;
			static std::unordered_map<DWORD, int> groupSizes;

			if (groupSizesFrame != Unsorted::CurrentFrame || groupSizesHouse != pHouse->ArrayIndex)
			{
				groupSizesFrame = Unsorted::CurrentFrame;
				groupSizesHouse = pHouse->ArrayIndex;
				groupSizes.clear();
			}

			auto const inGroup = [&](UnitClass* pVehicle) -> bool
			{
				auto it = groupSizes.find(pVehicle->UniqueID);

				if (it == groupSizes.end())
				{
					int size = 0;

					for (auto const pOther : UnitClass::Array)
					{
						if (isArmedGroundVehicle(pOther) && pOther->DistanceFrom(pVehicle) <= RepairerEscortGroupCells * Unsorted::LeptonsPerCell)
							size++;
					}

					it = groupSizes.emplace(pVehicle->UniqueID, size).first;
				}

				return it->second >= RepairerEscortGroupMin;
			};

			auto const isEscortable = [&](UnitClass* pOther) -> bool
			{
				if (pOther == pUnit || !isArmedGroundVehicle(pOther) || IsInTransportOp(pHouse, pOther->UniqueID)
					|| !onRepairerGround(pOther) || IsAtOwnBase(pHouse, pOther->GetMapCoords()))
				{
					return false;
				}

				const Mission mission = pOther->GetCurrentMission();

				return (mission == Mission::Move || mission == Mission::AttackMove || mission == Mission::Attack || mission == Mission::Hunt)
					&& inGroup(pOther);
			};

			if (isEscortable(abstract_cast<UnitClass*>(pUnit->Destination)))
				continue;

			// Repairers already following each vehicle, counted once per tick and house.
			static int repairersFollowingFrame = -1;
			static int repairersFollowingHouse = -1;
			static std::unordered_map<DWORD, int> repairersFollowing;

			if (repairersFollowingFrame != Unsorted::CurrentFrame || repairersFollowingHouse != pHouse->ArrayIndex)
			{
				repairersFollowingFrame = Unsorted::CurrentFrame;
				repairersFollowingHouse = pHouse->ArrayIndex;
				repairersFollowing.clear();

				for (auto const pRepairer : UnitClass::Array)
				{
					if (pRepairer && pRepairer->Owner == pHouse && !pRepairer->InLimbo && pRepairer->IsAlive && IsRepairerUnit(pRepairer))
					{
						if (auto const pFollowedVehicle = abstract_cast<UnitClass*>(pRepairer->Destination))
							repairersFollowing[pFollowedVehicle->UniqueID]++;
					}
				}
			}

			UnitClass* pEscorted = nullptr;
			int escortDist = INT_MAX;

			for (auto const pOwnU : UnitClass::Array)
			{
				if (!isEscortable(pOwnU))
					continue;

				const int dist = pUnit->DistanceFrom(pOwnU);

				if (dist >= escortDist)
					continue;

				auto const itFollowing = repairersFollowing.find(pOwnU->UniqueID);
				const int repairersWithIt = (itFollowing != repairersFollowing.end() ? itFollowing->second : 0)
					- (pUnit->Destination == pOwnU ? 1 : 0);

				if (repairersWithIt >= RepairerEscortMost)
					continue;

				escortDist = dist;
				pEscorted = pOwnU;
			}

			if (pEscorted && escortDist > RepairerEscortKeepCells * Unsorted::LeptonsPerCell && pUnit->Destination != pEscorted)
			{
				pUnit->ClearMegaMissionData();
				pUnit->SetTarget(nullptr);
				pUnit->SetDestination(pEscorted, true);
				pUnit->QueueMission(Mission::Move, false);
				NoteOrderCaller(pUnit, "move");
				repairersFollowing[pEscorted->UniqueID]++;

				if (repairerMacro.RepairerLogCount < RepairerLogLines)
				{
					repairerMacro.RepairerLogCount++;
					Debug::Log("[NeuralAI] Frame %d | House #%d repairer %s %u follows %s %u (mission %d, %d vehicles in its group) %d cells away\n",
						Unsorted::CurrentFrame, pHouse->ArrayIndex, pType->ID, pUnit->UniqueID, pEscorted->Type->ID, pEscorted->UniqueID,
						static_cast<int>(pEscorted->GetCurrentMission()), groupSizes[pEscorted->UniqueID], escortDist / Unsorted::LeptonsPerCell);
				}
			}

			if (pEscorted)
				continue;

			// Nobody out in the field: the damage at our bases and expansions.
			if (auto const pAtBase = FindBaseRepairTarget(pHouse, pUnit, repairerZone, RepairerBaseSearchCells))
			{
				pUnit->ClearMegaMissionData();
				pUnit->SetTarget(pAtBase);
				pUnit->QueueMission(Mission::Attack, true);
				NoteOrderCaller(pUnit, "repair");

				if (repairerMacro.RepairerLogCount < RepairerLogLines)
				{
					repairerMacro.RepairerLogCount++;
					Debug::Log("[NeuralAI] Frame %d | House #%d repairer %s %u repairs at base %s %u at %d%% %d cells away\n", Unsorted::CurrentFrame,
						pHouse->ArrayIndex, pType->ID, pUnit->UniqueID, pAtBase->GetTechnoType()->ID, pAtBase->UniqueID,
						static_cast<int>(pAtBase->GetHealthPercentage() * 100.0),
						static_cast<int>(pUnit->GetMapCoords().DistanceFrom(pAtBase->GetMapCoords())));
				}
			}

			continue;
		}

		// Slot 82 (Engineer ConYard Heist Cease-Fire): If our tank is firing at an
		// enemy ConYard that is already in red health (< 35% HP) while our own
		// Engineer is within 10 cells moving in to capture it, hold fire on the
		// ConYard so our Engineer can steal it alive!
		if (ownEngineerNearEnemyConYard && pCapturableEnemyConYard && pUnit->Target == pCapturableEnemyConYard
			&& pCapturableEnemyConYard->GetHealthPercentage() < 0.35 && SlotGateHeld(scores[82], 60, 82, pUnit->UniqueID))
		{
			pUnit->SetTarget(nullptr);

			pUnit->QueueMission(Mission::Guard, true);

			continue;
		}

		// Slot 71 (Chrono Legionnaire Temporal Breaker): Immediately kill any enemy
		// Chrono Legionnaire erasing our building or tank so it is restored at
		// 100%!
		if (pEnemyTemporalAttacker && pPrimaryWpn && pPrimaryWpn->Damage > 0 && SlotGateHeld(scores[71], 60, 71, pUnit->UniqueID)
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
		if (pEnemyDrainer && hasMobileAAWeapon && SlotGateHeld(scores[74], SurvivalMicroVetoScore, 74, pUnit->UniqueID) && pUnit->DistanceFrom(pEnemyDrainer) <= 4608)
		{
			pUnit->SetTarget(pEnemyDrainer);

			pUnit->QueueMission(Mission::Attack, true);

			continue;
		}

		if (baseAirFlankCount > 0 && hasMobileAAWeapon && pUnit->GetHealthPercentage() >= 0.35)
		{
			const int bestAirIdx = pickAirFlankFor(pUnit, isUnitIdle, 5632, false);

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
		if (pUnit->Target != nullptr && !pType->Naval && pUnit->GetHealthPercentage() < 0.38 && SlotGateHeld(scores[77], SurvivalMicroVetoScore, 77, pUnit->UniqueID)
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
					const NeuralOrderPriorityScope orderPriority { NeuralOrderPriority::Survival };
					pUnit->SetDestination(pBackCell, true);
					pUnit->QueueMission(Mission::Move, true);
					NoteOrderCaller(pUnit, "move");

					continue;
				}
			}
		}

		// Slot 79 (Surgical Enemy Power, Superweapon & Factory Snipe): When inside
		// range of a critical enemy structure, prioritize the nearest one over walls/silos!
		if (surgicalEnemyBldCount > 0 && isUnitIdle && pPrimaryWpn && pPrimaryWpn->Damage > 0 && SlotGateHeld(scores[79], 80, 79, pUnit->UniqueID))
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
			&& (pType->Speed >= 8 || pType->JumpJet) && SlotGateHeld(scores[80], 80, 80, pUnit->UniqueID)
			&& pUnit->DistanceFrom(pEnemyArtilleryUnit) <= 4608)
		{
			DispatchFootToTargetOrTransport(pUnit, pEnemyArtilleryUnit, Mission::Attack, true);

			continue;
		}

		// Slot 78 (High-Ground Cliff Advantage Positioning): Siege/long-range units
		// step onto nearby higher-elevation cells (CellClass::Level) when idle!
		if (isUnitIdle && pPrimaryWpn && pPrimaryWpn->Range >= (7 * 256) && !pType->Naval && SlotGateHeld(scores[78], 90, 78, pUnit->UniqueID)
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
								NoteOrderCaller(pUnit, "move");

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
		if (pType->Crusher && !pType->Naval && SlotGateHeld(scores[58], 100, 58, pUnit->UniqueID)
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

		// Minimum range: when the weapon the engine picks for our target (SelectWeapon) has a minimum
		// range and the target is inside it (e.g. a missile sub next to the shore it should shell),
		// the unit steps straight away from it to a cell between the minimum + 1 and the range - 2,
		// clear for it (to the sides when straight back is blocked). Every 30 frames.
		if (auto const pMinTarget = abstract_cast<TechnoClass*>(pUnit->Target);
			pMinTarget && pMinTarget->IsAlive && IsAITickWindow(30, 15) && pUnit->CurrentMission != Mission::Move)
		{
			auto const pChosen = pUnit->GetWeapon(pUnit->SelectWeapon(pMinTarget));
			auto const pMinWpn = pChosen ? pChosen->WeaponType : nullptr;
			const int dist = pUnit->DistanceFrom(pMinTarget);

			if (pMinWpn && pMinWpn->MinimumRange > 0 && dist < pMinWpn->MinimumRange)
			{
				const double desired = (pMinWpn->MinimumRange + Unsorted::LeptonsPerCell
					+ std::max(pMinWpn->MinimumRange + Unsorted::LeptonsPerCell, pMinWpn->Range - 2 * Unsorted::LeptonsPerCell))
					/ 2.0 / Unsorted::LeptonsPerCell;
				const CellStruct uc = pUnit->GetMapCoords();
				const CellStruct tc = pMinTarget->GetMapCoords();
				const double dx = uc.X - tc.X;
				const double dy = uc.Y - tc.Y;
				const double len = std::max(1.0, std::sqrt(dx * dx + dy * dy));
				CellClass* pBackCell = nullptr;

				for (const double sideBias : { 0.0, 0.6, -0.6, 1.2, -1.2 })
				{
					const double dirX = dx / len - (dy / len) * sideBias;
					const double dirY = dy / len + (dx / len) * sideBias;
					const double dirLen = std::max(0.01, std::sqrt(dirX * dirX + dirY * dirY));
					const CellStruct cand { static_cast<short>(tc.X + dirX / dirLen * desired), static_cast<short>(tc.Y + dirY / dirLen * desired) };
					auto const pCand = MapClass::Instance.CoordinatesLegal(cand) ? MapClass::Instance.TryGetCellAt(cand) : nullptr;

					if (pCand && pCand->IsClearToMove(pType->SpeedType, false, false, -1, pType->MovementZone, -1, false))
					{
						pBackCell = pCand;
						break;
					}
				}

				if (pBackCell)
				{
					pUnit->SetDestination(pBackCell, true);
					pUnit->QueueMission(Mission::Move, true);
					NoteOrderCaller(pUnit, "move");
					continue;
				}
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
					NoteOrderCaller(pUnit, "move");
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
				NoteOrderCaller(pUnit, "move");
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
		if (pStolenConYardOrMCV && pUnit != pStolenConYardOrMCV && !pType->Naval
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

		if (pPrimaryWpn && pPrimaryWpn->Spawner && !shellingBuildingFromRange && SlotGateHeld(scores[60], 100, 60, pUnit->UniqueID)
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
						NoteOrderCaller(pUnit, "move");

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
					&& SlotGateHeld(scores[60], 120, 60, pUnit->UniqueID))
				|| (pType->GapGenerator && SlotGateHeld(scores[63], 80, 63, pUnit->UniqueID)))
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
			&& pUnit->CanDisguiseAs(pSampleEnemyInfantry) && SlotGateHeld(scores[61], 80, 61, pUnit->UniqueID))
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

		if (isCamoAmbushTank && SlotGateHeld(scores[83], 60, 83, pUnit->UniqueID) && homeCoords != CellStruct::Empty)
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
				&& pCurTargetInf->SlaveOwner->IsAlive && !pCurTargetInf->SlaveOwner->InLimbo && SlotGateHeld(scores[62], 60, 62, pUnit->UniqueID))
			{
				DispatchFootToTargetOrTransport(pUnit, pCurTargetInf->SlaveOwner, Mission::Attack, true);

				continue;
			}
		}

		if (pEnemySlaveMiner && isUnitIdle && !pType->Naval && SlotGateHeld(scores[62], 140, 62, pUnit->UniqueID)
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

		if (pParasitedOwnUnit && pParasitedOwnUnit != pUnit && isUnitIdle && canCureParasite && SlotGateHeld(scores[64], 60, 64, pUnit->UniqueID)
			&& pUnit->DistanceFrom(pParasitedOwnUnit) <= 4096)
		{
			DispatchFootToTargetOrTransport(pUnit, pParasitedOwnUnit, Mission::Attack, true);

			continue;
		}

		// Slot 64 (Offensive Parasite Targeting: Terror Drone, Giant Squid,
		// Cyberworm): Target expensive enemy tanks/harvesters/warships that do NOT
		// yet have a parasite!
		if (pWH && pWH->Parasite && isUnitIdle && pBestParasiteVictim && pUnit->ParasiteImUsing == nullptr
			&& SlotGateHeld(scores[64], 60, 64, pUnit->UniqueID) && pType->Naval == pBestParasiteVictim->Type->Naval
			&& pUnit->DistanceFrom(pBestParasiteVictim) <= 4608)
		{
			DispatchFootToTargetOrTransport(pUnit, pBestParasiteVictim, Mission::Attack, true);

			continue;
		}

		// Slot 66 (EMP Pulse Targeting & EMP-Paralyzed Target Finisher):
		if (pWH && pWH->EMEffect && isUnitIdle && pEnemyNonEMPedHeavy && SlotGateHeld(scores[66], 80, 66, pUnit->UniqueID)
			&& pUnit->DistanceFrom(pEnemyNonEMPedHeavy) <= 4096)
		{
			DispatchFootToTargetOrTransport(pUnit, pEnemyNonEMPedHeavy, Mission::Attack, true);

			continue;
		}
		else if (pEnemyEMPedTarget && isUnitIdle && pPrimaryWpn && pPrimaryWpn->Damage > 0 && (!pWH || !pWH->EMEffect)
			&& SlotGateHeld(scores[66], 100, 66, pUnit->UniqueID) && pUnit->DistanceFrom(pEnemyEMPedTarget) <= 3584)
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
		if (pWH && pWH->Psychedelic && isUnitIdle && pEnemyChaosTarget && SlotGateHeld(scores[68], 80, 68, pUnit->UniqueID)
			&& pUnit->DistanceFrom(pEnemyChaosTarget) <= 3584)
		{
			DispatchFootToTargetOrTransport(pUnit, pEnemyChaosTarget, Mission::Attack, true);

			continue;
		}

		// Slot 69 (Submarine Cloak Ambush & Anti-Submarine Destroyer Escort). While only enemy ships or
		// submarines remain (NavalRemnantUntil) the hunt runs whatever the learned gate says.
		if (pType->Naval && (SlotGateHeld(scores[69], 80, 69, pUnit->UniqueID) || GetMacroState(pHouse).NavalRemnantUntil > Unsorted::CurrentFrame
			|| GetMacroState(pHouse).SubThreatUntil > Unsorted::CurrentFrame))
		{
			if (pType->Cloakable && pUnit->GetHealthPercentage() < 0.45 && homeCoords != CellStruct::Empty
				&& pUnit->CurrentMission != Mission::Move)
			{
				if (auto const pHC = MapClass::Instance.TryGetCellAt(homeCoords))
				{
					pUnit->SetDestination(pHC, true);

					pUnit->QueueMission(Mission::Move, true);
					NoteOrderCaller(pUnit, "move");

					continue;
				}
			}
			// Against a submarine only ships that hit under water (NavalTargeting) or carry sonar go.
			// Each goes for the enemy ship nearest to it.
			else if (auto const pNearShip = isUnitIdle ? nearestEnemyShipFor(pUnit, false) : nullptr;
				pNearShip && (pType->Sensors || pType->NavalTargeting == NavalTargetingType::Underwater_Secondary
					|| pType->NavalTargeting == NavalTargetingType::Underwater_Only
					|| pType->NavalTargeting == NavalTargetingType::Organic_Secondary
					|| (!pNearShip->Type->Cloakable && pPrimaryWpn && pPrimaryWpn->Damage > 0)))
			{
				if (auto const pSubCell = pNearShip->GetCell())
				{
					if (pType->Sensors || IsCellVisibleToAI(pHouse, pSubCell))
					{
						OrderFootToAttackMove(pUnit, pSubCell);

						continue;
					}
				}
			}
			// With no enemy ship to go for, an idle warship (armed, or with sonar) outside a plan, a capital group or a
			// naval sweep attack-moves to the fleet staging cell of its sea (fleetStagingCellFor) unless it is within
			// NavalStagingKeepCells of it already.
			else if (isUnitIdle
				&& (pType->Sensors || pType->Spawns || (pPrimaryWpn && pPrimaryWpn->Damage > 0)
					|| (pType->GetWeapon(1) && pType->GetWeapon(1)->WeaponType && pType->GetWeapon(1)->WeaponType->Damage > 0))
				&& !IsAmphibiousOrAirTransport(pType) && !IsWaitingAtNavalPassage(pUnit) && !IsPlanMember(pHouse, pUnit->UniqueID)
				&& !GetMacroState(pHouse).CapitalHeld.contains(pUnit->UniqueID) && !GetMacroState(pHouse).NavalSweeps.contains(pUnit->UniqueID)
				&& (IsAITickWindow(90, 20)))
			{
				if (auto const pStaging = fleetStagingCellFor(pUnit);
					pStaging && pUnit->GetMapCoords().DistanceFrom(pStaging->MapCoords) > NavalStagingKeepCells)
				{
					OrderFootToAttackMove(pUnit, pStaging);

					continue;
				}
			}
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

				// A transport working for a transport operation is filled by the operation alone.
				if (!pOTVeh->Type->OpenTopped || HasNoManualEnter(pOTVeh->Type) || HasNoManualUnload(pOTVeh->Type)
					|| IsInTransportOp(pHouse, pOTVeh->UniqueID))
				{
					continue;
				}

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

	if (pTargetBridgeHut && !SlotGateHeld(scores[21], 400, 21, pTargetBridgeHut->UniqueID))
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

	// Engineer work by warhead (FakeEngineerAbilities) for any unit, which survives it: a unit whose
	// warhead disarms bombs shoots the bomb off a building or unit of ours or of an ally within
	// FakeEngineerBombCells (engineers with Engineer=yes walk in, below), and an idle vehicle or aircraft
	// whose warhead captures takes the nearest tech it can as an idle engineer does (soldiers that capture
	// by their weapon are engineers, below).
	{
		auto const isFreeWorker = [&](FootClass* pFoot)
		{
			return pFoot && pFoot->Owner == pHouse && pFoot->IsAlive && !pFoot->InLimbo
				&& (pFoot->CurrentMission == Mission::Guard || pFoot->CurrentMission == Mission::Area_Guard)
				&& !IsFootActivelyAttackingOrMovingToEnemy(pFoot) && !IsPlanMember(pHouse, pFoot->UniqueID);
		};

		for (auto const pBombed : TechnoClass::Array)
		{
			if (!pBombed || !pBombed->IsAlive || pBombed->InLimbo || !pBombed->AttachedBomb || !pBombed->Owner
				|| !pHouse->IsAlliedWith(pBombed->Owner))
			{
				continue;
			}

			FootClass* pDisarmer = nullptr;
			int nearest = FakeEngineerBombCells * Unsorted::LeptonsPerCell;
			bool taken = false;

			for (auto const pFoot : FootClass::Array)
			{
				if (pFoot && pFoot->Owner == pHouse && pFoot->Target == pBombed)
				{
					taken = true;
					break;
				}

				auto const pInfType = pFoot ? abstract_cast<InfantryTypeClass*>(pFoot->GetTechnoType()) : nullptr;

				if (pFoot == pBombed || !isFreeWorker(pFoot) || (pInfType && pInfType->Engineer)
					|| !(FakeEngineerAbilities(pFoot->GetTechnoType()) & FakeEngineerDisarmsBombs)
					|| !TechnoExt::CanBeAffectedByFakeEngineer(pFoot, pBombed, false, false, true))
				{
					continue;
				}

				const int distance = pFoot->DistanceFrom(pBombed);

				if (distance < nearest)
				{
					nearest = distance;
					pDisarmer = pFoot;
				}
			}

			if (!taken && pDisarmer)
				DispatchFootToTargetOrTransport(pDisarmer, pBombed, Mission::Attack, true);
		}

		auto const pWorkExt = HouseExt::Fetch(pHouse);
		const int workExpert = pWorkExt ? pWorkExt->NeuralAI_ActiveExpert : -1;
		std::vector<const AbstractClass*> capturesUnderWay;
		bool capturersFound = false;

		for (auto const pFoot : FootClass::Array)
		{
			if (!pFoot || pFoot->Owner != pHouse || !pFoot->IsAlive || pFoot->WhatAmI() == AbstractType::Infantry
				|| !(FakeEngineerAbilities(pFoot->GetTechnoType()) & FakeEngineerCaptures))
			{
				continue;
			}

			capturersFound = true;
			break;
		}

		if (capturersFound)
		{
			for (auto const pFoot : FootClass::Array)
			{
				if (pFoot && pFoot->Owner == pHouse && pFoot->IsAlive)
				{
					if (pFoot->Target)
						capturesUnderWay.push_back(pFoot->Target);

					if (pFoot->Destination)
						capturesUnderWay.push_back(pFoot->Destination);
				}
			}
		}

		for (auto const pFoot : FootClass::Array)
		{
			if (!capturersFound)
				break;

			if (!isFreeWorker(pFoot) || pFoot->WhatAmI() == AbstractType::Infantry
				|| !(FakeEngineerAbilities(pFoot->GetTechnoType()) & FakeEngineerCaptures))
			{
				continue;
			}

			BuildingClass* pBestTech = nullptr;
			int bestDistance = INT_MAX;

			for (auto const pBld : BuildingClass::Array)
			{
				if (!pBld || !pBld->IsAlive || pBld->InLimbo || !pBld->Owner || !CanCaptureByWeapon(pFoot, pBld)
					|| std::find(capturesUnderWay.begin(), capturesUnderWay.end(), static_cast<const AbstractClass*>(pBld)) != capturesUnderWay.end())
				{
					continue;
				}

				// A neutral tech as for any engineer; one an enemy took for Expert 4, and for the resource
				// hoarder when it pays (as the routing of engineers in training, UpdateCaptureBarracks).
				const bool available = pBld->Owner->IsNeutral()
					? IsSpecOpsTechAvailable(pHouse, pBld, pFoot->UniqueID)
					: (IsKnownCapturableTech(pHouse, pBld) && !IsSpecOpsCaptureTarget(pHouse, pBld->UniqueID, 0)
						&& !IsCellInKnownEnemyDefenseReach(pHouse, pBld->GetMapCoords(), 2)
						&& (workExpert == 4 || (workExpert == 6 && (pBld->Type->ProduceCashAmount > 0 || pBld->Type->ProduceCashStartup > 0))));

				if (!available)
					continue;

				const int distance = pFoot->DistanceFrom(pBld);

				if (distance < bestDistance)
				{
					bestDistance = distance;
					pBestTech = pBld;
				}
			}

			if (pBestTech && DispatchFootToTargetOrTransport(pFoot, pBestTech, Mission::Attack, true))
				capturesUnderWay.push_back(pBestTech);
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

		// A soldier of a transport operation takes its orders from the operation alone.
		if (IsInTransportOp(pHouse, pInf->UniqueID))
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
			else if (pInf->Type->Deployer && pInf->Type->ImmuneToRadiation && SlotGateHeld(scores[63], 80, 63, pInf->UniqueID)
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
		// A finished Move counts as idle too (a jumpjet soldier hovers in Move over its last cell).
		const bool moveOver = pInf->CurrentMission == Mission::Move && pInf->MegaMission != Mission::AttackMove
			&& (!pInf->Destination || CellClass::Coord2Cell(pInf->Destination->GetCoords()).DistanceFrom(pInf->GetMapCoords()) <= 1.5);
		const bool isIdle = (pInf->CurrentMission == Mission::Guard || pInf->CurrentMission == Mission::Area_Guard || moveOver)
			&& !IsFootActivelyAttackingOrMovingToEnemy(pInf);

		// Specialists flying a plan (airborne raid) take their orders from it.
		const bool inPlan = IsSpecialistInfantryType(pInf->Type) && IsPlanMember(pHouse, pInf->UniqueID);
		const auto pInfWpn = pInf->Type->GetWeapon(0) ? pInf->Type->GetWeapon(0)->WeaponType : nullptr;

		// Slot 82 (Infantry Human Shield & Anti-Engineer Recapture Screen for
		// Stolen ConYard / Escaping MCV): Nearby friendly combat infantry (<= 14
		// cells) immediately gun down enemy Engineers trying to recapture our
		// stolen ConYard, or form a human shield perimeter around the stolen
		// ConYard/MCV to absorb enemy fire during extraction!
		if (pStolenConYardOrMCV && !pInf->Type->Engineer && !IsSpyInfantryType(pInf->Type)
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
			&& SlotGateHeld(scores[57], 100, 57, pInf->UniqueID) && (IsAITickWindow(60, 20)))
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
				NoteOrderCaller(pInf, "move");
				continue;
			}
		}

		// Slot 65 (Grinder Mind-Control Recycling for Infantry): Send
		// mind-controlled infantry into our Grinder for instant credits and to free
		// Yuri's control slot!
		if (pInf->MindControlledBy != nullptr)
		{
			// Already on its way into the Bio Reactor: it keeps that way. It fell through to the Grinder
			// below, and the next tick back to the Bio Reactor (about 1000 turns a match, order churn).
			if (pBioReactor && pInf->Destination == pBioReactor)
			{
				infAlreadyHeadingToBioReactor++;

				if (pBioReactor->Passengers.NumPassengers + infAlreadyHeadingToBioReactor >= pBioReactor->Type->Passengers)
					pBioReactor = nullptr;

				continue;
			}

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

		if (pGrinderBld && pInf->MindControlledBy != nullptr && SlotGateHeld(scores[65], 80, 65, pInf->UniqueID))
		{
			DispatchFootToTargetOrTransport(pInf, pGrinderBld, Mission::Enter, false);

			continue;
		}

		// Slot 68 (TiberiumHeal Infantry Regeneration & Sniper/Poison Specialist
		// Hunter):
		if (pInf->Type->TiberiumHeal && pInf->GetHealthPercentage() < 0.55 && SlotGateHeld(scores[68], 60, 68, pInf->UniqueID)
			&& pInf->CurrentMission != Mission::Move)
		{
			if (pInf->MoveToTiberium(8, false))
				continue;
		}

		if (pEnemySpecialistInf && isIdle && pInfWpn && pInfWpn->Damage > 0 && !pInf->Type->Engineer
			&& ((pInfWpn->Warhead && pInfWpn->Warhead->Poison) || pInfWpn->Range >= (6 * 256)) && SlotGateHeld(scores[68], 80, 68, pInf->UniqueID)
			&& pInf->DistanceFrom(pEnemySpecialistInf) <= 4096)
		{
			DispatchFootToTargetOrTransport(pInf, pEnemySpecialistInf, Mission::Attack, true);

			continue;
		}

		// A wounded soldier left idle far from home walks back to it (WoundedIdleHomeCells).
		if (pInf->GetHealthPercentage() < WoundedIdleHealth && pInf->Target == nullptr && homeCoords != CellStruct::Empty
			&& (pInf->CurrentMission == Mission::Guard || pInf->CurrentMission == Mission::Area_Guard)
			&& pInf->GetMapCoords().DistanceFrom(homeCoords) > WoundedIdleHomeCells
			&& ObjectZone(pInf, MovementZone::Normal) == homeGroundZone && !IsPlanMember(pHouse, pInf->UniqueID)
			&& IsAITickWindow(450, 0))
		{
			IssueMoveOrder(pInf, MapClass::Instance.TryGetCellAt(homeCoords), 6.0);
		}

		// Wounded valuable or veteran infantry (< 55% HP) retreats to a hospital, a medic or the
		// house self-heal (tryRepairRetreat).
		if (!pInf->Type->Engineer && (pInf->Type->Cost >= 300 || pInf->Veterancy.IsElite() || pInf->Veterancy.IsVeteran()
			|| (pInf->Type->Cost >= 200 && pInf->Type->Cost * 4 >= armyAverageCost * 5))
			&& pInf->CurrentMission != Mission::Enter && tryRepairRetreat(pInf, 0.55))
		{
			continue;
		}

		// Combat Medics / Field Mechanics (Damage < 0): they go with the soldiers of their team and
		// keep an eye on them (user: medics escort their squad, watching for the wounded). Healing
		// first: a wounded own or allied soldier within 10 cells, whatever the medic was doing. Else
		// the medic follows the nearest own soldier on the move or in a fight within 24 cells (two
		// medics per soldier group at most: one already followed or with two medics within 6 cells
		// is skipped), keeping within 4 cells of it; with nobody out it stays where it is.
		if (pInfWpn && pInfWpn->Damage < 0)
		{
			auto const pHealTarget = abstract_cast<TechnoClass*>(pInf->Target);

			if (pHealTarget && pHealTarget->IsAlive && !pHealTarget->InLimbo && pHealTarget->Owner
				&& pHouse->IsAlliedWith(pHealTarget->Owner) && pHealTarget->GetHealthPercentage() < 1.0)
			{
				continue;
			}

			InfantryClass* pWounded = nullptr;
			int woundedDist = 2561;

			for (auto const pAllyInf : InfantryClass::Array)
			{
				if (!pAllyInf || pAllyInf == pInf || !pAllyInf->Owner || !pHouse->IsAlliedWith(pAllyInf->Owner)
					|| pAllyInf->InLimbo || !pAllyInf->IsAlive)
				{
					continue;
				}

				const int dist = pInf->DistanceFrom(pAllyInf);

				if (pAllyInf->GetHealthPercentage() < 0.85 && dist < woundedDist)
				{
					woundedDist = dist;
					pWounded = pAllyInf;
				}
			}

			if (pWounded)
			{
				DispatchFootToTargetOrTransport(pInf, pWounded, Mission::Attack, true);
				continue;
			}

			// The soldier to escort: own fighting infantry on the move or in a fight, anywhere on
			// ground the medic can walk to (user: idle medics at home are taken back into missions,
			// no distance limit, only reachable ground). The one followed now is kept while it still
			// is (no switching between soldiers of one group).
			InfantryClass* pEscorted = nullptr;
			int escortDist = INT_MAX;
			const int medicZone = ObjectZone(pInf, MovementZone::Normal);

			if (auto const pFollowed = abstract_cast<InfantryClass*>(pInf->Destination))
			{
				const Mission followedMission = pFollowed->IsAlive ? pFollowed->GetCurrentMission() : Mission::None;

				if (pFollowed->Owner == pHouse && !pFollowed->InLimbo && pFollowed->Type && !IsHealerInfantryType(pFollowed->Type)
					&& (followedMission == Mission::Move || followedMission == Mission::AttackMove || followedMission == Mission::Attack
						|| followedMission == Mission::Hunt)
					&& (medicZone < 0 || ObjectZone(pFollowed, MovementZone::Normal) == medicZone))
				{
					continue;
				}
			}

			for (auto const pOwnI : InfantryClass::Array)
			{
				if (!pOwnI || pOwnI == pInf || pOwnI->Owner != pHouse || pOwnI->InLimbo || !pOwnI->IsAlive || !pOwnI->Type
					|| IsHealerInfantryType(pOwnI->Type) || IsSpecialistInfantryType(pOwnI->Type) || !pOwnI->Type->GetWeapon(0)
					|| !pOwnI->Type->GetWeapon(0)->WeaponType)
				{
					continue;
				}

				const Mission mission = pOwnI->GetCurrentMission();

				if (mission != Mission::Move && mission != Mission::AttackMove && mission != Mission::Attack && mission != Mission::Hunt)
					continue;

				const int dist = pInf->DistanceFrom(pOwnI);

				if (dist >= escortDist || (medicZone >= 0 && ObjectZone(pOwnI, MovementZone::Normal) != medicZone))
					continue;

				// Healers already following this soldier, counted once per tick and house (was a
				// scan of every soldier per candidate: with no distance limit the AI tick went over
				// 4 ms on average, aiTickCost alarms in every match).
				static int medicsFollowingFrame = -1;
				static int medicsFollowingHouse = -1;
				static std::unordered_map<DWORD, int> medicsFollowing;

				if (medicsFollowingFrame != Unsorted::CurrentFrame || medicsFollowingHouse != pHouse->ArrayIndex)
				{
					medicsFollowingFrame = Unsorted::CurrentFrame;
					medicsFollowingHouse = pHouse->ArrayIndex;
					medicsFollowing.clear();

					for (auto const pHealer : InfantryClass::Array)
					{
						if (pHealer && pHealer->Owner == pHouse && !pHealer->InLimbo && pHealer->IsAlive && pHealer->Type
							&& IsHealerInfantryType(pHealer->Type))
						{
							if (auto const pFollowedSoldier = abstract_cast<InfantryClass*>(pHealer->Destination))
								medicsFollowing[pFollowedSoldier->UniqueID]++;
						}
					}
				}

				auto const itFollowing = medicsFollowing.find(pOwnI->UniqueID);
				const int medicsWithIt = (itFollowing != medicsFollowing.end() ? itFollowing->second : 0)
					- (pInf->Destination == pOwnI ? 1 : 0);

				if (medicsWithIt >= 2)
					continue;

				escortDist = dist;
				pEscorted = pOwnI;
			}

			if (pEscorted && escortDist > 4 * Unsorted::LeptonsPerCell && pInf->Destination != pEscorted)
			{
				pInf->ClearMegaMissionData();
				pInf->SetTarget(nullptr);
				pInf->SetDestination(pEscorted, true);
				pInf->QueueMission(Mission::Move, false);
				NoteOrderCaller(pInf, "move");
				CountDecision(NeuralDecision::MedicEscort, pHouse->ArrayIndex);
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
		// An engineer by its weapon (FakeEngineer.CanCaptureBuildings, IsCaptureInfantryType) captures
		// here, with an attack order instead of walking in; its bombs and bridges are the general work by
		// warhead above.
		if (IsCaptureInfantryType(pInf->Type) && isIdle && !inPlan)
		{
			const bool capturesByWeapon = CapturesByWeapon(pInf->Type);
			auto const dispatchCapture = [&](FootClass* pFoot, AbstractClass* pTarget, Mission mission, bool useSetTarget) -> bool
			{
				return (capturesByWeapon && mission == Mission::Enter)
					? DispatchFootToTargetOrTransport(pFoot, pTarget, Mission::Attack, true)
					: DispatchFootToTargetOrTransport(pFoot, pTarget, mission, useSetTarget);
			};

			// Disarm any attached Crazy Ivan Bomb on our own OR allied high-value
			// buildings!
			bool disarmedBomb = false;

			for (auto const pAllyBld : BuildingClass::Array)
			{
				if (!capturesByWeapon && pAllyBld && !pAllyBld->InLimbo && pAllyBld->IsAlive && pAllyBld->Owner
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

			if (pTargetBridgeHut && !capturesByWeapon)
			{
				NoteBridgeWorker(pHouse, targetBridgeSpan, pInf,
					DispatchFootToTargetOrTransport(pInf, pTargetBridgeHut, Mission::Enter, false));

				pTargetBridgeHut = nullptr; // One engineer per hut is enough

				continue;
			}

			// Repair heavily damaged friendly-occupied or empty urban garrisonable
			// buildings (CanBeOccupied && Repairable) to 100% full health so our
			// infantry hold an unbreakable urban stronghold!
			// No distance limit (was 16 cells): reachable ground is what counts. Once repaired the
			// building is worth holding again (RateUrbanBuilding needs 45% health) and is re-garrisoned.
			if (pDamagedUrbanToRepair && !capturesByWeapon && pInf->Destination == pDamagedUrbanToRepair)
			{
				pDamagedUrbanToRepair = nullptr;
				continue;
			}

			// The repair order: a garrison we (or an ally) hold refuses Enter like it does for soldiers
			// (repro, Fort Urban Square: 30 engineers sent into a 71% bunker of ours, none got in), so
			// it goes through the capture mission (FootClass::Mission_Capture handles the engineer's
			// repair as it does the occupier); beyond 12 cells the engineer walks next to it first.
			// An empty civilian building keeps the Enter order.
			auto const repairOrder = [&](InfantryClass* pEngineer, BuildingClass* pBld) -> bool
			{
				const bool heldByFriend = pBld->Owner && (pBld->Owner == pHouse || pHouse->IsAlliedWith(pBld->Owner))
					&& !pBld->Owner->IsNeutral();

				if (!heldByFriend)
					return DispatchFootToTargetOrTransport(pEngineer, pBld, Mission::Enter, false);

				if (pEngineer->DistanceFrom(pBld) > 12 * Unsorted::LeptonsPerCell)
					return IssueMoveOrder(pEngineer, pBld->GetCell(), 6.0) || pEngineer->CurrentMission == Mission::Move;

				// The player's repair click (user: the engineer gets the repair cursor on a garrison with
				// soldiers inside) runs EventClass MEGAMISSION -> TechnoClass::ClickedMission, which sets
				// the capture mission with the building as its TARGET (Mission_Capture works on the
				// target); a Destination alone was dropped. Called directly: no network event, the same
				// on every client.
				pEngineer->ClearMegaMissionData();
				return pEngineer->ClickedMission(Mission::Capture, pBld, pBld->GetCell(), nullptr);
			};

			if (pDamagedUrbanToRepair && !capturesByWeapon && IsBuildingEnterableFromZone(pDamagedUrbanToRepair, ObjectZone(pInf, MovementZone::Normal))
				&& repairOrder(pInf, pDamagedUrbanToRepair))
			{
				pDamagedUrbanToRepair = nullptr;

				continue;
			}

			auto const pHoarderExt = HouseExt::Fetch(pHouse);

			// Expert 4 (Special Operations): the best known safe neutral tech, with an escort first
			// when enemies were seen near it. Captures already under way finish under any expert.
			if (RunSpecOpsEngineer(pHouse, pInf, pHoarderExt && pHoarderExt->NeuralAI_ActiveExpert == 4, dispatchCapture))
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
					|| (SlotGateHeld(scores[82], 60, 82, pInf->UniqueID) && pBld->Type->ConstructionYard);

				if (!canCaptureNow)
					continue;

				// Coordinate multiple Engineers: do not send a 2nd Engineer to a building, neutral or enemy,
				// another engineer of ours already goes for: walking to it, attacking it, or claimed for it
				// (EngineerCaptureTargets: waiting for a transport, aboard, sent lately).
				{
					bool alreadyTargetedByOtherEng = IsCaptureClaimedByOther(pHouse, pBld, pInf->UniqueID, false);

					for (auto const pOtherInf : InfantryClass::Array)
					{
						if (alreadyTargetedByOtherEng)
							break;

						if (pOtherInf && pOtherInf != pInf && pOtherInf->Owner == pHouse && !pOtherInf->InLimbo
							&& pOtherInf->IsAlive && pOtherInf->Type && IsCaptureInfantryType(pOtherInf->Type)
							&& (pOtherInf->Destination == pBld || pOtherInf->Target == pBld))
						{
							alreadyTargetedByOtherEng = true;
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
						  && (SlotGateHeld(scores[82], 60, 82, pInf->UniqueID) || (resourceHoarder && isCashTechBld)))
					? (dist / 3)
					: (isNeutralTech ? (isCashTechBld ? (dist / 3) : (isStrategicTechHub ? (dist * 2 / 5) : (dist / 2)))
									: dist);

				if (effectiveDist < bestCapDist)
				{
					bestCapDist = effectiveDist;

					pBestCapBld = pBld;
				}
			}

			if (pBestCapBld && dispatchCapture(pInf, pBestCapBld, Mission::Enter, false))
			{
				// Engineer claims (EngineerClaim*): it goes for this building now, a transport or not.
				auto& claims = GetMacroState(pHouse).EngineerCaptureTargets;

				if (claims.size() > 64)
				{
					std::erase_if(claims, [pHouse](const auto& entry)
						{ return Unsorted::CurrentFrame - entry.second.second > 4 * EngineerClaimFrames && !IsInTransportOp(pHouse, entry.first); });
				}

				claims[pInf->UniqueID] = { pBestCapBld->UniqueID, Unsorted::CurrentFrame };
			}
			// Nothing to capture: a capturer that also repairs (CABAL's Mechanic: SuperRepairGun) repairs the
			// nearest damaged vehicle or building at our bases and expansions (Repairer*).
			else if ((RepairAbilitiesOf(pInf->Type) & (RepairsVehicles | RepairsBuildings)) != 0)
			{
				if (auto const pAtBase = FindBaseRepairTarget(pHouse, pInf, ObjectZone(pInf, MovementZone::Normal), RepairerMechanicCells))
				{
					pInf->ClearMegaMissionData();
					pInf->SetTarget(pAtBase);
					pInf->QueueMission(Mission::Attack, true);
					NoteOrderCaller(pInf, "repair");

					// TEMP diagnostic: the repairers' orders (RepairerLogLines per house and match).
					auto& mechanicMacro = GetMacroState(pHouse);

					if (mechanicMacro.RepairerLogCount < RepairerLogLines)
					{
						mechanicMacro.RepairerLogCount++;
						Debug::Log("[NeuralAI] Frame %d | House #%d repairer %s %u repairs at base %s %u at %d%% %d cells away\n",
							Unsorted::CurrentFrame, pHouse->ArrayIndex, pInf->Type->ID, pInf->UniqueID, pAtBase->GetTechnoType()->ID,
							pAtBase->UniqueID, static_cast<int>(pAtBase->GetHealthPercentage() * 100.0),
							static_cast<int>(pInf->GetMapCoords().DistanceFrom(pAtBase->GetMapCoords())));
					}
				}
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

			if (!victimColors && pSampleEnemyInfantry && (pVictim || SlotGateHeld(scores[61], 60, 61, pInf->UniqueID))
				&& !SpyDisguiseHouse(pInf) && IsCellKnownToAI(pHouse, pSampleEnemyInfantry->GetMapCoords()))
			{
				CopySoldierDisguise(pHouse, pInf, pSampleEnemyInfantry);
			}
		}

		// Slot 62 (Slave Miner Liberation for Infantry): Redirect fire from Slaved
		// workers onto their SlaveOwner so killing the miner frees the slaves!
		if (auto const pCurTargetInf = abstract_cast<InfantryClass*>(pInf->Target))
		{
			if (pCurTargetInf->Type && pCurTargetInf->Type->Slaved && pCurTargetInf->SlaveOwner
				&& pCurTargetInf->SlaveOwner->IsAlive && !pCurTargetInf->SlaveOwner->InLimbo && SlotGateHeld(scores[62], 60, 62, pInf->UniqueID))
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

		// Transports working on a crossing belong to the ferry until it ends: one the ferry holds is
		// filled by the ferry alone. An IFV or an OpenTopped vehicle that is no ferry right now still
		// takes idle gunners.
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

				if (IsFerryParkedToLoad(pHouse, pCombVeh->UniqueID)
					|| IsInTransportOp(pHouse, pCombVeh->UniqueID)
					|| (pCombVeh->Passengers.NumPassengers > 0 && FindFerryTrip(pCombVeh->UniqueID)))
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
			const int bestInfAirIdx = pickAirFlankFor(pInf, isIdle, 4608, false);

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

		// Yuri Bio-Reactor Power Surge: Enter Bio-Reactor with basic infantry while the power is low
		// or its surplus under 350. No distance limit (was 16 cells): any idle cheap soldier on ground
		// that reaches the reactor (same zone, or a ferry through the dispatcher).
		if (pBioReactor && bioReactorNeedsPower && isIdle && pInf->Destination != pBioReactor && !pInf->Type->Engineer
			&& !IsSpyInfantryType(pInf->Type) && !pInf->Type->Doggie && !IsHealerInfantryType(pInf->Type) && pInf->Type->Cost <= 250
			&& IsBuildingEnterableFromZone(pBioReactor, ObjectZone(pInf, MovementZone::Normal)))
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

		// Buildings worth holding (urban opportunity >= 150) are taken every tick from the start,
		// whatever the expert (user: strategic positions decide matches); slot 23 sets the pace
		// otherwise.
		if (isIdle && (urbanExpert || UrbanOpportunity(pHouse) >= 150 || IsAITickWindow(90, garrisonWindow))
			&& RunUrbanGarrison(pHouse, pInf, urbanExpert, garrisonRangeCells, DispatchFootToTargetOrTransport,
				IsFootActivelyAttackingOrMovingToEnemy))
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
	// What the idle army of the last tick does to each building type in 900 frames, as a percent of
	// its strength (WaveGroupDamage*); -1 with no army counted lately. Spawners, mind control and
	// temporal weapons are left out: their damage figure says nothing.
	std::unordered_map<const BuildingTypeClass*, int> groupDamagePercents;
	// The landing a transport operation takes the wave to by a target the path finder does not
	// reach (PathDeadFerry*), by square of that target: Empty when the house owns no transport for
	// it, no safe landing lies by it, or the try for that target is over.
	std::vector<std::pair<int, CellStruct>> pathDeadLandings;
	int ownsOpTransport = -1;

	auto const pathDeadFerryLanding = [&](const CellStruct& goal) -> CellStruct
	{
		const int square = ((goal.X / 8) << 10) | (goal.Y / 8);

		for (auto const& [key, landing] : pathDeadLandings)
		{
			if (key == square)
				return landing;
		}

		auto const& ferryMacro = GetMacroState(pHouse);
		const int tried = Unsorted::CurrentFrame - ferryMacro.PathDeadFerrySince;
		// A goal near the target being ferried is that target: its landing while the try lasts,
		// none once it is over.
		const bool sameTarget = ferryMacro.PathDeadFerryGoal != CellStruct::Empty && tried >= 0
			&& tried <= PathDeadFerryTryFrames + PathDeadFerryRestFrames && ferryMacro.PathDeadFerryGoal.DistanceFrom(goal) <= PathDeadFerryGoalCells;
		CellStruct landing = CellStruct::Empty;

		if (sameTarget && tried <= PathDeadFerryTryFrames && ferryMacro.PathDeadFerryLanding != CellStruct::Empty)
		{
			landing = ferryMacro.PathDeadFerryLanding;
		}
		else if (!sameTarget)
		{
			if (ownsOpTransport < 0)
				ownsOpTransport = HouseOwnsOpTransport(pHouse) ? 1 : 0;

			const int zone = MapClass::Instance.GetMovementZoneType(goal, MovementZone::Normal, false);

			if (ownsOpTransport > 0 && IsGroundZone(zone))
			{
				int danger = 0;
				landing = FindShoreLandingBy(pHouse, goal, zone, danger);

				if (danger > OpSailDangerLimit)
					landing = CellStruct::Empty;
			}
		}

		pathDeadLandings.push_back({ square, landing });

		return landing;
	};

	// Where the enemy aircraft rearm counts by the value of the aircraft of his we have seen.
	const std::array<int, 8> enemyAirValues = EnemyAirValueByOwner(pHouse);

	auto const groupDamagePercent = [&](const BuildingTypeClass* pBldType) -> int
	{
		auto const& groupMacro = GetMacroState(pHouse);
		const int age = Unsorted::CurrentFrame - groupMacro.WaveGroupFrame;

		if (groupMacro.WaveGroupTypes.empty() || age < 0 || age > WaveGroupFreshFrames)
			return -1;

		auto const [it, inserted] = groupDamagePercents.try_emplace(pBldType, 0);

		if (inserted)
		{
			long long damage = 0;

			for (auto const& [pGroupType, count] : groupMacro.WaveGroupTypes)
			{
				int best = 0;

				for (int i = 0; i < 2; ++i)
				{
					// TechnoTypeClass::GetWeapon is not const-qualified in YRpp.
					auto const pWS = const_cast<TechnoTypeClass*>(pGroupType)->GetWeapon(i);
					auto const pWeapon = pWS ? pWS->WeaponType : nullptr;

					if (!WeaponHitsGround(pWeapon) || !pWeapon->Warhead || pWeapon->Spawner || pWeapon->Warhead->MindControl
						|| pWeapon->Warhead->Temporal)
					{
						continue;
					}

					const int versesPercent = static_cast<int>(GeneralUtils::GetWarheadVersusArmor(pWeapon->Warhead, pBldType->Armor) * 100.0);
					best = std::max(best,
						pWeapon->Damage * std::max(1, pWeapon->Burst) * std::max(0, versesPercent) / 100 * 900 / std::max(1, pWeapon->ROF));
				}

				damage += static_cast<long long>(best) * count;
			}

			it->second = static_cast<int>(std::min<long long>(damage * 100 / std::max(1, pBldType->Strength), 100000));
		}

		return it->second;
	};

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

	// Missions of the tank mass, each weighed by the trained preference of its slot
	// (TankMassMissionBonus). Clear Our Ground: what the enemy built next to our ground goes before
	// the far targets, a defense, a siege gun or a factory before the rest. Reprisal: the buildings
	// of the house that hurt us most lately go before those of the others. Our ground is what lies
	// around our construction yards, refineries and factories, each with the land it stands on.
	struct TankMassSite
	{
		CellStruct Cell;
		int Zone;
	};

	const bool tankMassTargets = pExt->NeuralAI_Archetype == TankMassArchetype;
	std::vector<TankMassSite> tankMassSites;
	int reprisalHouse = -1;
	const int reprisalBonus = TankMassMissionBonus(TankMassReprisalBonus,
		scores[NeuralPlanSlots[static_cast<size_t>(NeuralPlanId::Reprisal)]]);
	const int ourGroundBonus = TankMassMissionBonus(TankMassNearBuildingBonus,
		scores[NeuralPlanSlots[static_cast<size_t>(NeuralPlanId::ClearOurGround)]]);
	// What the tank mass knows of the power of each enemy: output and drain of his known buildings,
	// and his known defenses that need power.
	std::array<int, 8> knownPowerOutput {};
	std::array<int, 8> knownPowerDrain {};
	std::array<int, 8> knownPoweredDefenses {};
	auto const pWayDangerMap = (tankMassTargets && homeCoords != CellStruct::Empty) ? &GetOpDangerMap(pHouse, false) : nullptr;
	// The ring of Clear Our Ground: TankMassDangerCells, and with no known enemy building inside
	// it reaches out to the nearest one (TankMassDangerStepCells, TankMassDangerMaxCells). While an
	// enemy building stands on the land of one of our sites the missions count only on such land.
	double ourGroundCells = TankMassDangerCells;
	bool enemyOnOurLand = false;
	auto const onOurLand = [&tankMassSites](int zone)
	{
		return std::any_of(tankMassSites.begin(), tankMassSites.end(), [zone](const TankMassSite& site) { return site.Zone == zone; });
	};
	auto const missionLand = [&enemyOnOurLand, &onOurLand](int zone) { return !enemyOnOurLand || onOurLand(zone); };
	auto const nearOurGround = [&tankMassSites, &ourGroundCells, &enemyOnOurLand](const CellStruct& cell, int zone)
	{
		return std::any_of(tankMassSites.begin(), tankMassSites.end(), [&](const TankMassSite& site)
			{
				return (!enemyOnOurLand || site.Zone == zone) && site.Cell.DistanceFrom(cell) <= ourGroundCells;
			});
	};

	if (tankMassTargets)
	{
		for (auto const pOwnBld : pHouse->Buildings)
		{
			if (pOwnBld && pOwnBld->IsAlive && !pOwnBld->InLimbo && pOwnBld->Type
				&& (pOwnBld->Type->ConstructionYard || pOwnBld->Type->Refinery || pOwnBld->Type->Factory != AbstractType::None))
			{
				const CellStruct siteCell = pOwnBld->GetMapCoords();
				tankMassSites.push_back({ siteCell, MapClass::Instance.GetMovementZoneType(siteCell, MovementZone::Normal, pOwnBld->OnBridge) });
			}
		}

		auto const& grudges = GetMacroState(pHouse).Coalition;
		int worstGrudge = 0;

		for (int i = 0; i < 8; ++i)
		{
			if (Unsorted::CurrentFrame - grudges.HurtByFrame[i] <= TankMassReprisalFrames && grudges.Grudge[i] > worstGrudge)
			{
				worstGrudge = grudges.Grudge[i];
				reprisalHouse = i;
			}
		}

		// The nearest known enemy building sets the ring. Truce partners are no targets.
		double nearestOnOurLand = TankMassDangerMaxCells + 1.0;
		double nearestAnywhere = TankMassDangerMaxCells + 1.0;

		for (auto const& info : GetKnownEnemyBuildingCache(pHouse).Entries)
		{
			if (const int owner = info.Building->Owner->ArrayIndex; owner >= 0 && owner < 8)
			{
				auto const pKnownType = info.Building->Type;
				knownPowerOutput[owner] += pKnownType->PowerBonus;
				knownPowerDrain[owner] += pKnownType->PowerDrain;

				if (pKnownType->IsBaseDefense && (pKnownType->PowerDrain > 0 || pKnownType->Powered))
					knownPoweredDefenses[owner]++;
			}

			if (info.Building->Type->Insignificant
				|| (coalitionTarget >= 0 && IsTacitTrucePartner(pHouse, info.Building->Owner->ArrayIndex)))
			{
				continue;
			}

			const int zone = MapClass::Instance.GetMovementZoneType(info.Coords, MovementZone::Normal, info.Building->OnBridge);

			for (auto const& site : tankMassSites)
			{
				const double distance = site.Cell.DistanceFrom(info.Coords);
				nearestAnywhere = std::min(nearestAnywhere, distance);

				if (site.Zone == zone)
				{
					enemyOnOurLand = true;
					nearestOnOurLand = std::min(nearestOnOurLand, distance);
				}
			}
		}

		const double nearestToOurGround = enemyOnOurLand ? nearestOnOurLand : nearestAnywhere;

		if (nearestToOurGround > TankMassDangerCells && nearestToOurGround <= TankMassDangerMaxCells)
		{
			ourGroundCells = TankMassDangerCells
				+ std::ceil((nearestToOurGround - TankMassDangerCells) / TankMassDangerStepCells) * TankMassDangerStepCells;
		}
	}

	// High ground (Cliff*): the share of our armed ground army whose projectile a cliff stops going
	// up, and, when it is CliffBlockedSharePercent or more, the cells of the known enemy defenses.
	int cliffBlockedPercent = 0;
	std::vector<CellStruct> cliffDefenseCells;

	if (tankMassTargets)
	{
		int armedGround = 0;
		int blockedGoingUp = 0;
		auto const countCliffShooter = [&armedGround, &blockedGoingUp](FootClass* pFoot)
		{
			auto const pType = pFoot->GetTechnoType();
			auto const pWeapon = pType->GetWeapon(0) ? pType->GetWeapon(0)->WeaponType : nullptr;

			if (!pWeapon || pWeapon->Damage <= 0 || pFoot->IsInAir())
				return;

			armedGround++;

			if (pWeapon->Projectile && pWeapon->Projectile->SubjectToCliffs)
				blockedGoingUp++;
		};

		for (auto const pUnit : UnitClass::Array)
		{
			if (pUnit && pUnit->Owner == pHouse && pUnit->IsAlive && !pUnit->InLimbo && pUnit->Type && !pUnit->Type->Naval
				&& !pUnit->Type->Harvester && !pUnit->Type->ResourceGatherer)
			{
				countCliffShooter(pUnit);
			}
		}

		for (auto const pInf : InfantryClass::Array)
		{
			if (pInf && pInf->Owner == pHouse && pInf->IsAlive && !pInf->InLimbo && pInf->Type)
				countCliffShooter(pInf);
		}

		cliffBlockedPercent = armedGround > 0 ? blockedGoingUp * 100 / armedGround : 0;

		if (cliffBlockedPercent >= CliffBlockedSharePercent)
		{
			for (auto const& info : GetKnownEnemyBuildingCache(pHouse).Entries)
			{
				if (info.IsDefense)
					cliffDefenseCells.push_back(info.Coords);
			}
		}
	}

	// Siege mode (Siege*): the enemies the tank mass besieges while it dominates (a place of theirs
	// resisted our waves), those places, and the known defenses of those enemies.
	unsigned int siegeMask = 0u;
	std::vector<CellStruct> siegePlaces;
	std::vector<std::pair<CellStruct, int>> siegeDefenses;

	if (tankMassTargets)
	{
		auto& siegeMacro = GetMacroState(pHouse);
		const int siegeNow = Unsorted::CurrentFrame;
		const int siegeAdvantage = GetWaveTimingState(pHouse).AdvantageRatio;
		const bool siegeBank = HasOffensiveBank(pHouse);

		if (siegeAdvantage >= SiegeAdvantageRatio || siegeBank)
		{
			const NeuralResistedPlace* pLastPlace = nullptr;

			for (auto const& place : siegeMacro.ResistedPlaces)
			{
				if (place.Owner < 0 || place.Owner >= 32 || siegeNow - place.Frame > SiegeMemoryFrames || siegeNow < place.Frame
					|| (coalitionTarget >= 0 && IsTacitTrucePartner(pHouse, place.Owner)))
				{
					continue;
				}

				if (!pLastPlace || place.Frame > pLastPlace->Frame)
					pLastPlace = &place;

				if (place.Failures >= SiegeFailuresAtPlace)
				{
					siegeMask |= 1u << place.Owner;
					siegePlaces.push_back(place.Cell);
				}
			}

			if (pLastPlace && siegeMacro.WaveFailuresInARow >= SiegeFailuresInARow && (siegeMask & (1u << pLastPlace->Owner)) == 0u)
			{
				siegeMask |= 1u << pLastPlace->Owner;
				siegePlaces.push_back(pLastPlace->Cell);
			}
		}

		if (siegeMask != 0u)
		{
			for (auto const& info : GetKnownEnemyBuildingCache(pHouse).Entries)
			{
				if (const int owner = info.Building->Owner->ArrayIndex; info.IsDefense && owner >= 0 && owner < 32 && (siegeMask & (1u << owner)) != 0u)
					siegeDefenses.emplace_back(info.Coords, GetCombatValueOfType(info.Building->Type, false));
			}
		}

		if (siegeMask != siegeMacro.SiegeOwnersMask || (siegeMask != 0u && siegeNow - siegeMacro.SiegeLogFrame >= SiegeLogFrames))
		{
			Debug::Log("[NeuralAI] Frame %d | House #%d siege mode: besieged houses mask %u (was %u), %d resisting places, first (%d,%d), "
				"%d known defenses of theirs, advantage %d, offensive bank %d, waves failed in a row %d\n", siegeNow, pHouse->ArrayIndex,
				siegeMask, siegeMacro.SiegeOwnersMask, static_cast<int>(siegePlaces.size()), siegePlaces.empty() ? -1 : siegePlaces.front().X,
				siegePlaces.empty() ? -1 : siegePlaces.front().Y, static_cast<int>(siegeDefenses.size()), siegeAdvantage, siegeBank ? 1 : 0,
				siegeMacro.WaveFailuresInARow);
			siegeMacro.SiegeLogFrame = siegeNow;
		}

		siegeMacro.SiegeOwnersMask = siegeMask;
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

		// A besieged enemy (siege mode) is not taken ring by ring: what keeps him going counts, wherever it stands.
		const int siegeOwnerIdx = pOwner->ArrayIndex;
		const bool siegeTarget = siegeMask != 0u && siegeOwnerIdx >= 0 && siegeOwnerIdx < 32 && (siegeMask & (1u << siegeOwnerIdx)) != 0u;

		// Perimeter-First Scoring: heavily penalize targets that sit deep behind
		// the enemy's outer perimeter (> 6 cells deeper than the closest enemy
		// structure) so our battalions chew through the closest enemy
		// structures/defenses on the outer ring first instead of driving through
		// half the enemy base!
		int bldCandScore = 1000 - (distHome * 8) - (!siegeTarget && depthIntoEnemyBase > 6 ? (depthIntoEnemyBase - 6) * 18 : 0);
		if (depthIntoEnemyBase <= 6 && !siegeTarget)
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
		int candWayDanger = 0;

		if (tankMassTargets)
		{
			// The least defended way in, and the power plant that leaves the defenses without power.
			if (pWayDangerMap)
			{
				OpLineCost(*pWayDangerMap, homeCoords, ebc, candWayDanger);
				bldCandScore -= std::min(siegeTarget ? SiegeWayDangerMost : TankMassWayDangerMost, candWayDanger / TankMassWayDangerPerPoint);
			}

			// High ground with a known defense near it, while most of our army cannot fire up the cliff
			// (Cliff*): the units would drive round by the ramps into the defenses.
			if (!cliffDefenseCells.empty()
				&& std::any_of(cliffDefenseCells.begin(), cliffDefenseCells.end(),
					[&ebc](const CellStruct& defense) { return defense.DistanceFrom(ebc) <= CliffDefenseCells; })
				&& IsHighGroundOverZone(ebc, pCell->Level, homeGroundZone))
			{
				bldCandScore -= CliffTargetPenalty * cliffBlockedPercent / 100;
			}

			if (const int owner = pOwner->ArrayIndex; pEnemyBld->Type->PowerBonus > 0 && owner >= 0 && owner < 8
				&& knownPoweredDefenses[owner] >= TankMassPowerCutDefenses && IsEnemyBuildingKnown(pHouse, pEnemyBld)
				&& knownPowerOutput[owner] - pEnemyBld->Type->PowerBonus < knownPowerDrain[owner])
			{
				bldCandScore += TankMassPowerCutBonus;
			}

			if (pOwner->ArrayIndex == reprisalHouse && missionLand(ebz))
				bldCandScore += reprisalBonus;

			if (ourGroundBonus > 0 && nearOurGround(ebc, ebz))
			{
				const bool dangerous = pEnemyBld->Type->IsBaseDefense || pEnemyBld->Type->Factory != AbstractType::None
					|| IsDeployedSiegeBuildingType(pEnemyBld->Type);
				bldCandScore += ourGroundBonus + (dangerous ? TankMassDangerBuildingBonus : 0);
			}
		}

		if (homeGroundZone >= 0 && ebz == homeGroundZone)
			bldCandScore += 350;

		if (!pEnemyBld->Type->Naval && pCell->LandType != LandType::Water)
			bldCandScore += 180;

		// A place our units find no path to comes after every other target (IsPathDeadGoal), unless
		// a transport operation can take the wave to a landing by it (user: the transports first).
		if (IsPathDeadGoal(pHouse, ebc) && pathDeadFerryLanding(ebc) == CellStruct::Empty)
			bldCandScore -= WavePathDeadGoalPenalty;

		// The wave under way keeps its target: what stands where the last dispatch went, while that
		// dispatch is fresh, comes before a target of about the same worth elsewhere.
		{
			auto const& lastWave = GetMacroState(pHouse);

			if (lastWave.LastWaveTargetCell != CellStruct::Empty
				&& Unsorted::CurrentFrame - lastWave.LastWaveDispatchFrame <= WaveTargetKeepFrames
				&& ebc.DistanceFrom(lastWave.LastWaveTargetCell) <= WaveGoalKeepCells)
			{
				bldCandScore += WaveTargetKeepBonus;
			}
		}

		if (pEnemyBld->Type->IsBaseDefense && depthIntoEnemyBase <= 8)
		{
			// Clear outer base defenses blocking our approach! Not those of a besieged enemy: they
			// are what holds our waves off.
			if (!siegeTarget)
				bldCandScore += 135;
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

		// Siege mode: the defenses that cover a building of a besieged enemy, and the places that
		// resisted, cost; his refineries, power plants and factories gain.
		if (siegeTarget)
		{
			int siegeDefenseNear = 0;

			for (auto const& [defenseCell, defenseValue] : siegeDefenses)
			{
				if (defenseCell.DistanceFrom(ebc) <= SiegeDefenseCells)
					siegeDefenseNear += defenseValue;
			}

			bldCandScore -= std::min(SiegeDefenseMost, siegeDefenseNear * 3 / 2 / SiegeDefensePerPoint);

			if (std::any_of(siegePlaces.begin(), siegePlaces.end(), [&ebc](const CellStruct& place) { return place.DistanceFrom(ebc) <= SiegePlaceCells; }))
				bldCandScore -= SiegePlacePenalty;

			auto const pSiegeType = pEnemyBld->Type;

			if (pSiegeType->Refinery || pSiegeType->ResourceGatherer)
				bldCandScore += SiegeEconomyBonus;
			else if (pSiegeType->PowerBonus > pSiegeType->PowerDrain)
				bldCandScore += SiegePowerBonus;
			else if (pSiegeType->ConstructionYard || pSiegeType->Factory != AbstractType::None)
				bldCandScore += SiegeFactoryBonus;
		}

		const int candAirBaseBonus = AirBaseTargetBonus(enemyAirValues, pEnemyBld, AirBaseWavePercent, AirBaseWaveMost);
		bldCandScore += candAirBaseBonus;

		// What the group brings down fast comes first, what it barely scratches last.
		const int candGroupPercent = groupDamagePercent(pEnemyBld->Type);

		if (candGroupPercent >= WaveGroupDamageEvenPercent)
		{
			bldCandScore += std::min(WaveGroupDamageMost, (candGroupPercent - WaveGroupDamageEvenPercent) * WaveGroupDamageMost
				/ (WaveGroupDamageFastPercent - WaveGroupDamageEvenPercent));
		}
		else if (candGroupPercent >= 0)
		{
			bldCandScore -= std::min(WaveGroupDamageMost, (WaveGroupDamageEvenPercent - candGroupPercent) * WaveGroupDamageMost
				/ (WaveGroupDamageEvenPercent - WaveGroupDamageSlowPercent));
		}

		anyEnemyBuildingExists = true;

		// Fallback wave targets only among buildings this house knows about.
		const bool isKnownBuilding = IsCellVisibleToAI(pHouse, pCell) || IsEnemyBuildingKnown(pHouse, pEnemyBld);

		if (isKnownBuilding && bldCandScore > bestFallbackEnemyScore)
		{
			bestFallbackEnemyScore = bldCandScore;
			pFallbackEnemyCell = pCell;
		}

		const int ownerIdx = std::clamp(pOwner->ArrayIndex, 0, 7);
		const bool visible = IsCellVisibleToAI(pHouse, pCell)
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

	// Remnant phase: no enemy building known and no start left to check. The remnant search and the
	// hunt of the remnants in the air run in it, whatever enemy unit the waves go after meanwhile
	// (match #11110: 180 vehicle waves chased a flying unit for 4500 frames while one turret no unit
	// of ours had seen kept the match going).
	const bool remnantPhase = !pVisibleEnemyBld && !pFallbackEnemyCell;

	// A ground wave sent to the cell of an enemy in the air cannot hit it: those are left to the hunt
	// of the remnants in the air below.
	if (!pVisibleEnemyBld && !pFallbackEnemyCell)
	{
		if (pHiddenOrStealthEnemy && pHiddenOrStealthEnemy->GetCell() && !pHiddenOrStealthEnemy->IsInAir())
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
					|| pRemT->WhatAmI() == AbstractType::Building || pRemT->IsInAir())
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

	// Remnants in the air (aircraft, jumpjets) in the remnant phase, also while an enemy building no
	// unit of ours has seen is left: a ground wave sent to their cell cannot hit them, so the 12
	// nearest units with an anti-air weapon attack each one directly. Every 150 frames.
	if (remnantPhase && IsAITickWindow(150, 15))
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
	// Ships put back on guard this tick from an attack out of their reach (idle at once, whatever their mission
	// still reads), and ships still on such an attack (counted for the shipyard brake, NoteShipsWithoutTarget).
	std::vector<UnitClass*> releasedShips;
	int shipsOnUnreachableAttack = 0;
	// Why combat vehicles were not idle (wave gate log): busy attacking / heading to the enemy, in a
	// plan, other (health, capital group, Enter, links, mission).
	int notIdleBusy = 0;
	int notIdlePlan = 0;
	int notIdleMission = 0;
	// The missions those other units are on, ships apart from vehicles (wave gate log): about 47 of
	// them in match #9846, most of the house's ships, with nothing said of what they were doing.
	std::map<int, int> notIdleMissionShips;
	std::map<int, int> notIdleMissionVehicles;
	// More reasons (wave gate log): low health, immobile deployed vehicle, entering / linked, and for
	// infantry busy, mission and plan.
	int notIdleHealth = 0;
	int notIdleImmobile = 0;
	int notIdleLinked = 0;
	int notIdleInfBusy = 0;
	int notIdleInfMission = 0;
	int notIdleInfPlan = 0;
	int notIdleInfLinked = 0;

	std::vector<InfantryClass*> idleCombatInf;

	// A Move that is over (destination reached or gone) is idle too: jumpjets (Jumpjet Infantry)
	// hover in Move over the cell they were sent to instead of falling back to Guard (match #6879,
	// Prince of Darkness: 30 of a house's soldiers out of every wave, hovering in its base).
	auto const isFinishedMove = [](const FootClass* pFoot)
	{
		if (pFoot->CurrentMission != Mission::Move || pFoot->MegaMission == Mission::AttackMove)
			return false;

		return !pFoot->Destination
			|| CellClass::Coord2Cell(pFoot->Destination->GetCoords()).DistanceFrom(pFoot->GetMapCoords()) <= 1.5;
	};

	for (auto const pUnit : UnitClass::Array)
	{
		if (!pUnit || pUnit->Owner != pHouse || pUnit->InLimbo || !pUnit->IsAlive || !pUnit->Type)
			continue;

		// Repairers follow the vehicles on their own (Repairer*): no wave or push takes them. Nor a unit
		// that unfolds into a building other than a siege gun: its deployer logic leads it (user approved,
		// 2026-11-20, «Sí, todos»; match #12347, Tsunami: a Nanocore of a refinery, ROBOTEGG_REFN, went with
		// a unit wave toward an island it could not reach and drove round its own).
		if (pUnit->Type->Harvester || pUnit->Type->ResourceGatherer
			|| (pUnit->Type->DeploysInto && (pUnit->Type->DeploysInto->ConstructionYard || !IsDeployableSiegeUnitType(pUnit->Type)))
			|| IsRepairerUnit(pUnit))
		{
			continue;
		}

		// Capital units go out in their own groups (UpdateCapitalGroups), the tank guard stays
		// (UpdateTankGuard).
		if (GetMacroState(pHouse).CapitalHeld.contains(pUnit->UniqueID) || GetMacroState(pHouse).TankGuard.contains(pUnit->UniqueID))
			continue;

		// Units and transports of a transport operation are on their way already.
		if (IsInTransportOp(pHouse, pUnit->UniqueID))
		{
			++notIdlePlan;
			continue;
		}

		// A damaged unit sits out the waves while it repairs (on a trip, bound for or standing on a
		// station, a trip ordered lately) or near home, where it guards the base. Out in the field
		// with no repair it can take (the station too far for the trip to pay, none at all) it fights
		// with the rest instead of idling next to the enemy (match #6487, Cranberry Island: a Yuri
		// Boomer at 32% 150 cells from its naval yard sat in Area Guard beside the enemy island).
		if (pUnit->GetHealthPercentage() < 0.40
			&& (IsOnRepairTrip(pHouse, pUnit->UniqueID) || isAtOrBoundForRepair(pUnit) || repairTripRecent(pUnit)
				|| homeCoords == CellStruct::Empty || pUnit->GetMapCoords().DistanceFrom(homeCoords) <= 30.0))
		{
			++notIdleHealth;
			continue;
		}

		if (!endgameRemnantHunt && (pUnit->Type->CanDisguise || pUnit->Type->PermaDisguise) && SlotGateHeld(scores[83], 60, 83, pUnit->UniqueID)
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

		// The units of a strategic plan take their orders from it.
		if (!IsPlanMember(pHouse, pUnit->UniqueID))
			ClearStaleEnterOrUnreachableCrossIslandMission(pUnit, canCrossWaterDirectly);

		if (IsImmobileUnitTypeOnLand(pUnit->Type))
		{
			++notIdleImmobile;
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
			++notIdleLinked;
			continue;
		}

		// Left on Attack with nothing to attack (the engine dropped a target the unit could not fire
		// at, a submerged submarine for one) it does nothing, and it counts as busy below, so no
		// rule gave it another order: the submarines sent at an enemy one stayed where they lost
		// it for the rest of the match. It goes back on guard and is idle from the next tick.
		if (pUnit->CurrentMission == Mission::Attack && !pUnit->Target && !pUnit->IsFiring
			&& Unsorted::CurrentFrame - pUnit->CurrentMissionStartTime >= AttackWithNoTargetFrames)
		{
			pUnit->QueueMission(Mission::Guard, true);
		}

		// A ship on Attack at a target that no cell of its sea brings within the reach of the weapon it fires at it
		// (IsNavalTargetInReach) does nothing either: the engine answers fire from any range, and 24 of the 31
		// destroyers of a house stood so against guns on the shore (match #11499), busy for every rule
		// and never counted by the shipyard brake. Held still NavalStuckAttackFrames (IsFootHeldStill) it drops the
		// target and its record of standing still (so the next shot at it starts the count again), and goes back on
		// guard, idle from this tick on; until then it counts for the brake. A target that can fire at it would keep
		// hitting it there, and every hit put it on the same attack again: it moves out of that target's reach first,
		// NavalStuckRetreatMarginCells more, away from the target along its sea (else to the fleet staging cell), and is
		// idle once there. Plans give their ships their own targets.
		if (IsWaterBound(pUnit->Type) && pUnit->CurrentMission == Mission::Attack && pUnit->Target
			&& !IsPlanMember(pHouse, pUnit->UniqueID) && !IsNavalTargetInReach(pUnit, pUnit->Target))
		{
			if (!IsFootHeldStill(pHouse, pUnit, NavalStuckAttackFrames))
			{
				++shipsOnUnreachableAttack;
			}
			else if (!IsAmphibiousOrAirTransport(pUnit->Type))
			{
				auto const pStuckTarget = abstract_cast<TechnoClass*>(pUnit->Target);
				const CellStruct here = pUnit->GetMapCoords();
				const CellStruct targetCell = CellClass::Coord2Cell(pUnit->Target->GetCoords());
				CellClass* pRetreat = nullptr;

				if (pStuckTarget && pStuckTarget->IsAlive && !pStuckTarget->InLimbo)
				{
					const int weaponIndex = pStuckTarget->SelectWeapon(pUnit);
					const FireError error = pStuckTarget->GetFireError(pUnit, weaponIndex, true);
					auto const pSlot = pStuckTarget->GetWeapon(weaponIndex);
					auto const pWeapon = pSlot ? pSlot->WeaponType : nullptr;

					if (pWeapon && error != FireError::ILLEGAL && error != FireError::CANT && error != FireError::NONE)
					{
						const double safeCells = pWeapon->Range / static_cast<double>(Unsorted::LeptonsPerCell) + NavalStuckRetreatMarginCells;
						const int sea = ObjectZone(pUnit, pUnit->Type->MovementZone);
						const double awayLen = std::max(1.0, here.DistanceFrom(targetCell));
						const double awayX = (here.X - targetCell.X) / awayLen;
						const double awayY = (here.Y - targetCell.Y) / awayLen;
						const int steps = static_cast<int>(safeCells) + 2;

						for (int step = 1; step <= steps && !pRetreat && here.DistanceFrom(targetCell) <= safeCells; ++step)
						{
							const CellStruct c { static_cast<short>(here.X + static_cast<int>(awayX * step)),
								static_cast<short>(here.Y + static_cast<int>(awayY * step)) };
							auto const pCell = MapClass::Instance.TryGetCellAt(c);

							if (pCell && c.DistanceFrom(targetCell) > safeCells
								&& MapClass::Instance.GetMovementZoneType(c, pUnit->Type->MovementZone, false) == sea
								&& pCell->IsClearToMove(pUnit->Type->SpeedType, true, true, -1, pUnit->Type->MovementZone, -1, false))
							{
								pRetreat = pCell;
							}
						}

						if (!pRetreat && here.DistanceFrom(targetCell) <= safeCells)
						{
							if (auto const pStaging = fleetStagingCellFor(pUnit); pStaging && pStaging->MapCoords.DistanceFrom(targetCell) > safeCells)
								pRetreat = pStaging;
						}
					}
				}

				// TEMP diagnostic: a ship let go from an attack out of its reach, NavalStuckLogLines lines a match.
				static int releaseLogSerial = -1;
				static int releaseLogLines = 0;

				if (releaseLogSerial != NeuralMatchSerial)
				{
					releaseLogSerial = NeuralMatchSerial;
					releaseLogLines = 0;
				}

				if (releaseLogLines++ < NavalStuckLogLines)
				{
					Debug::Log("[NeuralAI] Naval stuck DIAG house %d frame %d: %s %u at (%d,%d) let go from Attack on %s at (%d,%d), %d cells, "
						"out of reach from its sea; %s.\n", pHouse->ArrayIndex, Unsorted::CurrentFrame, pUnit->Type->ID, pUnit->UniqueID, here.X,
						here.Y, (pStuckTarget && pStuckTarget->GetTechnoType()) ? pStuckTarget->GetTechnoType()->ID : "a cell", targetCell.X,
						targetCell.Y, static_cast<int>(here.DistanceFrom(targetCell)), pRetreat ? "moves out of its reach" : "stays");
				}

				pUnit->SetTarget(nullptr);
				pUnit->ClearMegaMissionData();
				DropClock(GetMacroState(pHouse), pUnit->UniqueID, NeuralClock::Still);
				CountDecision(NeuralDecision::NavalStuckReleased, pHouse->ArrayIndex);

				if (pRetreat && IssueMoveOrder(pUnit, pRetreat, 0.0))
				{
					++shipsOnUnreachableAttack;

					continue;
				}

				pUnit->QueueMission(Mission::Guard, true);
				idleCombatUnits.push_back(pUnit);
				releasedShips.push_back(pUnit);

				continue;
			}
		}

		if (IsFootActivelyAttackingOrMovingToEnemy(pUnit))
		{
			++notIdleBusy;
			continue;
		}

		if (pUnit->CurrentMission == Mission::Guard || pUnit->CurrentMission == Mission::Area_Guard
			|| pUnit->CurrentMission == Mission::AttackMove
			|| (pUnit->CurrentMission == Mission::Move && pUnit->MegaMission == Mission::AttackMove) || isFinishedMove(pUnit))
		{
			if (!IsPlanMember(pHouse, pUnit->UniqueID))
				idleCombatUnits.push_back(pUnit);
			else
				++notIdlePlan;
		}
		else
		{
			++notIdleMission;
			++(IsWaterBound(pUnit->Type) ? notIdleMissionShips : notIdleMissionVehicles)[static_cast<int>(pUnit->CurrentMission)];
		}
	}

	// Two healers (medics, field mechanics) at most go with a wave: soldiers first, the rest heal
	// at home (user: a GDI squad wants two medics, not a Red Cross column).
	int healersForWave = 0;

	for (auto const pInf : InfantryClass::Array)
	{
		if (!pInf || pInf->Owner != pHouse || pInf->InLimbo || !pInf->IsAlive || !pInf->Type)
			continue;

		// Slaves belong to their miner (match #7106: a Yuri slave on an attack-move to the enemy base
		// 110 cells away, pulled back by its master, standing still between the two orders).
		if (pInf->Type->Engineer || IsSpyInfantryType(pInf->Type) || pInf->SlaveOwner)
			continue;

		// Soldiers of a transport operation are on their way already.
		if (IsInTransportOp(pHouse, pInf->UniqueID))
		{
			++notIdleInfPlan;
			continue;
		}

		if (IsHealerInfantryType(pInf->Type) && healersForWave >= 2)
			continue;

		const bool canInfCrossWater = (pInf->Type->MovementZone == MovementZone::Amphibious
			|| pInf->Type->MovementZone == MovementZone::AmphibiousDestroyer
			|| pInf->Type->MovementZone == MovementZone::Fly || pInf->Type->JumpJet || pInf->Type->Teleporter);

		// The soldiers of a strategic plan take their orders from it: this test drops the Enter
		// order of a soldier whose transport stands on a cell of another zone and the walk of one
		// bound for a transport afloat, and the plan (Airborne Front boarding) gives them again at
		// every AI tick.
		if (!IsPlanMember(pHouse, pInf->UniqueID))
			ClearStaleEnterOrUnreachableCrossIslandMission(pInf, canInfCrossWater);

		// On its way aboard a transport, or linked to one.
		if (pInf->CurrentMission == Mission::Enter || pInf->QueuedMission == Mission::Enter || pInf->HasAnyLink())
		{
			++notIdleInfLinked;
			continue;
		}

		if (IsFootActivelyAttackingOrMovingToEnemy(pInf))
		{
			++notIdleInfBusy;
			continue;
		}

		if (pInf->CurrentMission == Mission::Guard || pInf->CurrentMission == Mission::Area_Guard
			|| pInf->CurrentMission == Mission::AttackMove
			|| (pInf->CurrentMission == Mission::Move && pInf->MegaMission == Mission::AttackMove) || isFinishedMove(pInf))
		{
			if (!IsPlanMember(pHouse, pInf->UniqueID))
			{
				idleCombatInf.push_back(pInf);

				if (IsHealerInfantryType(pInf->Type))
					healersForWave++;
			}
			else
			{
				++notIdleInfPlan;
			}
		}
		else
		{
			++notIdleInfMission;
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

	// Remnant search: the remnant phase while the enemy still has buildings, whatever enemy unit the
	// waves go after meanwhile. Idle units go and look, the fastest first, each to a different place
	// (RemnantSearchScouts, one in RemnantSearchArmyShare of a big idle army), and leave the idle
	// pools until they get there (KeepRemnantSearchers). Outside it the known enemy buildings are
	// noted as the first places of a later search (NoteRemnantLeads).
	if (!remnantPhase)
	{
		NoteRemnantLeads(pHouse);
	}
	else if (anyEnemyBuildingExists)
	{
		const size_t onTheirWay = KeepRemnantSearchers(pHouse, idleCombatUnits, idleCombatInf);

		if (Unsorted::CurrentFrame - GetMacroState(pHouse).RemnantSearchFrame >= RemnantSearchEveryFrames)
		{
			GetMacroState(pHouse).RemnantSearchFrame = Unsorted::CurrentFrame;
			std::vector<FootClass*> searchers(idleCombatUnits.begin(), idleCombatUnits.end());
			searchers.insert(searchers.end(), idleCombatInf.begin(), idleCombatInf.end());
			std::stable_sort(searchers.begin(), searchers.end(), [](FootClass* a, FootClass* b)
				{
					return a->GetTechnoType()->Speed > b->GetTechnoType()->Speed;
				});

			const size_t wantedSearchers = std::clamp((searchers.size() + onTheirWay) / RemnantSearchArmyShare,
				RemnantSearchScouts, RemnantSearchMostScouts);
			const size_t searcherCount = std::min(searchers.size(), wantedSearchers > onTheirWay ? wantedSearchers - onTheirWay : 0);
			const std::vector<CellStruct> places = searcherCount > 0 ? FindRemnantSearchCells(pHouse, searcherCount) : std::vector<CellStruct> {};

			for (size_t i = 0; i < places.size(); ++i)
			{
				auto const pSearcher = searchers[i];

				if (auto const pPlace = MapClass::Instance.TryGetCellAt(places[i]))
				{
					OrderFootToAttackMove(pSearcher, pPlace);
					NoteRemnantSearcher(pHouse, pSearcher, places[i]);
				}

				std::erase(idleCombatUnits, pSearcher);
				std::erase(idleCombatInf, pSearcher);
			}

			if (!places.empty())
			{
				Debug::Log("[NeuralAI] Frame %d | House #%d remnant search: %d scouts sent, %d on their way, first to (%d,%d)\n",
					Unsorted::CurrentFrame, pHouse->ArrayIndex, static_cast<int>(places.size()), static_cast<int>(onTheirWay),
					places.front().X, places.front().Y);
			}
			else if (searcherCount > 0 && MayLogRemnantNoPlace(pHouse))
			{
				Debug::Log("[NeuralAI] Frame %d | House #%d remnant search: no place left to look, %d idle, %d on their way\n",
					Unsorted::CurrentFrame, pHouse->ArrayIndex, static_cast<int>(searchers.size()), static_cast<int>(onTheirWay));
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
	// Only the guerrilla raider (5) goes out with small groups: a rush wins by arriving early in
	// numbers, and Blitzkrieg (3) houses sent out waves of 4 and lost 18 of 26 decided matches.
	// The tank mass goes out in large waves that grow after each one that took no building
	// (TankMassWaveUnits).
	const bool tankMassWaves = arch == TankMassArchetype;
	UpdateWaveVerdict(pHouse, false);
	const int baseWave = tankMassWaves ? TankMassWaveUnits(pHouse)
		: ((arch == 5) ? 4 : ((arch == 2 || arch == 4 || arch == 7) ? 8 : 6));
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
	// The second echelon goes only while the waves before fight at their goal: by time alone it sent
	// packets of a few units after waves still on their way (match #11465).
	bool secondEchelonWindow = sinceWaveStart >= 300 && sinceWaveStart <= 1500;

	if (secondEchelonWindow)
	{
		int engaged = 0;

		auto const countEngaged = [&](FootClass* pFoot)
		{
			if (engaged >= WaveEchelonEngagedUnits || !pFoot || pFoot->Owner != pHouse || !pFoot->IsAlive || pFoot->InLimbo)
				return;

			auto const itGoal = waveMacro.WaveGoals.find(pFoot->UniqueID);

			if (itGoal != waveMacro.WaveGoals.end() && pFoot->GetMapCoords().DistanceFrom(itGoal->second.Cell) <= WaveEchelonEngagedCells)
				++engaged;
		};

		for (auto const pU : UnitClass::Array)
			countEngaged(pU);

		for (auto const pI : InfantryClass::Array)
			countEngaged(pI);

		secondEchelonWindow = engaged >= WaveEchelonEngagedUnits;
	}

	const int tacticalWaveDiscount = (counterattackWindow ? 3 : 0) + (waveMacro.PressureMode ? 2 : 0)
		+ (secondEchelonWindow ? (baseWave + defenseMassBonus) / 2 : 0);
	// The learned wave mass scales the count and its cap alike.
	const int waveMass = WaveMassPermille(rushWaveBias);
	const int waveUnitCap = (tankMassWaves ? TankMassWaveCap : 14) * waveMass / 1000;
	const size_t minWaveUnits = endgameRemnantHunt
		? 1u
		: static_cast<size_t>(std::clamp(
			(baseWave + defenseMassBonus - streamWaveDiscount
				- (waveMacro.BlackoutUntilFrame > Unsorted::CurrentFrame ? 3 : 0) - tacticalWaveDiscount
				+ StyleJitter(pHouse->ArrayIndex, 1000, 2700, 3) - 1) * waveMass / 1000, // +-1 unit: waves are not clockwork
			(highMoneyMultiFactoryStream || tacticalWaveDiscount > 0) ? 2 : 3, waveUnitCap));
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
				|| (pUnit->CaptureManager && pUnit->CaptureManager->NumControlNodes() > 0) || IsInTransportOp(pHouse, pUnit->UniqueID))
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
	if (pExt->NeuralAI_ActiveExpert == 9 || pExt->NeuralAI_Archetype == 7 || SlotGateHeld(scores[85], 120, 85))
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

	// Mass, not a stream (match #11465 Dead Six: a wave of 2 or 3 and a dispatch every tick sent the
	// armies of two houses of 80+ units out in packets of 7 to 17): once the army is big, a main wave
	// waits for a share of it idle. The army is the idle fighters and those busy at the enemy, not the
	// guards, plans or garrisons. The guerrilla raider (5) keeps its small groups. The share does not
	// hold while the second echelon window is open, and gives way after WaveFloorWaitMostFrames (below).
	const int waveArmy = static_cast<int>(idleCombatUnits.size() + idleCombatInf.size()) + notIdleBusy + notIdleInfBusy;
	const size_t armyShareUnits = (endgameRemnantHunt || arch == 5 || waveArmy < WaveArmyMassFrom || secondEchelonWindow)
		? 0u
		: static_cast<size_t>(std::min(WaveArmyShareMost, waveArmy / WaveArmyShareDivisor));
	const size_t waveUnitsWithoutShare = teamOrder.MinWaveUnits > 0
		? std::min(minWaveUnits, static_cast<size_t>(teamOrder.MinWaveUnits))
		: minWaveUnits;
	size_t waveUnitsNeeded = std::max(armyShareUnits, waveUnitsWithoutShare);

	// Expert 3 tacit coalition: this wave goes for the coalition target (approach away from the
	// spots other houses' armies are hitting).
	const bool waveAtCoalitionTarget = coalitionTarget >= 0 && pVisibleEnemyBld && pVisibleEnemyBld->Owner
		&& pVisibleEnemyBld->Owner->ArrayIndex == coalitionTarget && pWaveTargetCell == pVisibleEnemyBld->GetCell();

	CellClass* pIslandGoalCell = pWaveTargetCell ? pWaveTargetCell : pFallbackEnemyCell;
	int targetIslandZone = -1;
	if (pIslandGoalCell)
	{
		auto const pGoalBld = pIslandGoalCell->GetBuilding();
		targetIslandZone = pGoalBld ? BuildingGroundZone(pGoalBld)
			: MapClass::Instance.GetMovementZoneType(pIslandGoalCell->MapCoords, MovementZone::Normal, false);
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

			// Only what this house has seen: the island to land on and the second wave target were
			// picked among every enemy building on the map.
			if (!IsEnemyBuildingKnown(pHouse, pEB))
				continue;

			auto const pECell = pEB->GetCell();
			if (!pECell || pECell->LandType == LandType::Water)
				continue;

			// The ground the building stands on: its own cell may read as no ground, and a base taken for
			// an island of zone 1 is a goal no transport finds a beach for.
			const int ebz = BuildingGroundZone(pEB);

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

				if (BuildingGroundZone(pCheckEB) == ebz)
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

	// What stands idle at this tick, for the production brake (NoteIdleArmy): the pools as the
	// waves, the plans and the transports find them, and the units jammed at home with somewhere to
	// go (HeldAtHomeFrames): whatever wave or plan holds them, they are of no use there, and every
	// new one only grows the jam.
	{
		int idleVehicles = 0;
		int idleShips = 0;

		for (auto const pU : idleCombatUnits)
		{
			auto const pWeapon = pU->Type->GetWeapon(0);

			if (IsAmphibiousOrAirTransport(pU->Type) || !pWeapon || !pWeapon->WeaponType || pU->Type->Naval)
				continue;

			++idleVehicles;
		}

		// Warships nobody uses: in no plan or transport operation, with no target and on no attack,
		// whatever their mission. The jam rules and the wave dispatch move idle ships about, so the
		// floor of the strictly idle ones never reached the brake: 108 Boomers of a house stood idle
		// while its shipyard went on building them (2026-11-17).
		for (auto const pU : UnitClass::Array)
		{
			if (!pU || pU->Owner != pHouse || !pU->IsAlive || pU->InLimbo || !pU->Type || !pU->Type->Naval
				|| IsAmphibiousOrAirTransport(pU->Type))
			{
				continue;
			}

			auto const pWeapon = pU->Type->GetWeapon(0);

			if (!pWeapon || !pWeapon->WeaponType || pU->Target || IsFootActivelyAttackingOrMovingToEnemy(pU)
				|| IsPlanMember(pHouse, pU->UniqueID) || IsInTransportOp(pHouse, pU->UniqueID))
			{
				continue;
			}

			++idleShips;
		}

		const std::unordered_set<const FootClass*> inIdlePools(idleCombatUnits.begin(), idleCombatUnits.end());
		const std::unordered_set<const FootClass*> idleSoldiers(idleCombatInf.begin(), idleCombatInf.end());
		int heldVehicles = 0;
		int heldSoldiers = 0;
		// Tank mass: those of them with no other job join the idle army below, and the next wave
		// takes them along.
		std::vector<UnitClass*> heldJoinVehicles;
		std::vector<InfantryClass*> heldJoinSoldiers;

		auto const heldJoinsWave = [&](FootClass* pFoot)
		{
			if (!tankMassWaves)
				return false;

			if (IsInTransportOp(pHouse, pFoot->UniqueID))
				return false;

			if (IsPlanMember(pHouse, pFoot->UniqueID))
				return false;

			if (pFoot->CurrentMission == Mission::Enter || pFoot->QueuedMission == Mission::Enter
				|| pFoot->CurrentMission == Mission::Unload || pFoot->HasAnyLink() || pFoot->BunkerLinkedItem
				|| pFoot->Deactivated || pFoot->IsUnderEMP() || pFoot->GetHealthPercentage() < HeldJoinsWaveHealth)
			{
				return false;
			}

			return true;
		};

		auto const isHeldAtHome = [&](const FootClass* pFoot)
		{
			if (homeCoords == CellStruct::Empty || pFoot->IsInAir())
				return false;

			AbstractClass* const pDest = pFoot->Destination ? pFoot->Destination : pFoot->MegaDestination;
			const CellStruct here = pFoot->GetMapCoords();

			return pDest && CellClass::Coord2Cell(pDest->GetCoords()).DistanceFrom(here) > 1.5
				&& here.DistanceFrom(homeCoords) <= HeldAtHomeCells && IsFootHeldStill(pHouse, pFoot, HeldAtHomeFrames);
		};

		for (auto const pU : UnitClass::Array)
		{
			if (!pU || pU->Owner != pHouse || pU->InLimbo || !pU->IsAlive || !pU->Type || pU->Type->Naval
				|| pU->Type->Harvester || pU->Type->ResourceGatherer || IsAmphibiousOrAirTransport(pU->Type)
				|| (pU->Type->DeploysInto && pU->Type->DeploysInto->ConstructionYard) || inIdlePools.contains(pU))
			{
				continue;
			}

			auto const pWeapon = pU->Type->GetWeapon(0);

			if (pWeapon && pWeapon->WeaponType && isHeldAtHome(pU))
			{
				++heldVehicles;

				if (!IsImmobileUnitTypeOnLand(pU->Type) && heldJoinsWave(pU))
					heldJoinVehicles.push_back(pU);
			}
		}

		for (auto const pInf : InfantryClass::Array)
		{
			if (!pInf || pInf->Owner != pHouse || pInf->InLimbo || !pInf->IsAlive || !pInf->Type || pInf->Type->Engineer
				|| IsSpyInfantryType(pInf->Type) || pInf->SlaveOwner || idleSoldiers.contains(pInf))
			{
				continue;
			}

			auto const pWeapon = pInf->Type->GetWeapon(0);

			if (pWeapon && pWeapon->WeaponType && isHeldAtHome(pInf))
			{
				++heldSoldiers;

				if (heldJoinsWave(pInf))
					heldJoinSoldiers.push_back(pInf);
			}
		}

		// How many may join grows with the waves in a row that took no building, like the wave
		// (user): none while the waves take buildings, HeldJoinsWaveUnits after one that took none,
		// twice that after each more. Vehicles first.
		const int heldJoinSteps = std::min(GetMacroState(pHouse).WaveFailuresInARow, HeldJoinsWaveMaxSteps);
		const size_t heldJoinMost = heldJoinSteps > 0 ? static_cast<size_t>(HeldJoinsWaveUnits) << (heldJoinSteps - 1) : 0u;

		if (heldJoinVehicles.size() > heldJoinMost)
			heldJoinVehicles.resize(heldJoinMost);

		if (heldJoinSoldiers.size() > heldJoinMost - heldJoinVehicles.size())
			heldJoinSoldiers.resize(heldJoinMost - heldJoinVehicles.size());

		// Since when enough vehicles stand jammed at home for the cure of closed-in war factories
		// (SellFactoryRoadBlock); the stretch ends when fewer than half of that many do.
		{
			auto& heldMacro = GetMacroState(pHouse);

			if (heldVehicles < FactoryRoadSellHeldVehicles / 2)
			{
				heldMacro.HeldAtHomeSince = -1;
			}
			else if (heldVehicles >= FactoryRoadSellHeldVehicles
				&& (heldMacro.HeldAtHomeSince < 0 || heldMacro.HeldAtHomeSince > Unsorted::CurrentFrame))
			{
				heldMacro.HeldAtHomeSince = Unsorted::CurrentFrame;
			}
		}

		NoteIdleArmy(pHouse, idleVehicles + heldVehicles, static_cast<int>(idleCombatInf.size()) + heldSoldiers, idleShips);

		// The idle army by type, for the target scoring of the next tick (groupDamagePercent).
		{
			auto& groupMacro = GetMacroState(pHouse);
			auto& group = groupMacro.WaveGroupTypes;
			group.clear();

			auto const count = [&group](const TechnoTypeClass* pType)
			{
				auto const it = std::find_if(group.begin(), group.end(), [pType](auto const& entry) { return entry.first == pType; });

				if (it != group.end())
					it->second++;
				else
					group.emplace_back(pType, 1);
			};

			for (auto const pU : idleCombatUnits)
				count(pU->Type);

			for (auto const pInf : idleCombatInf)
				count(pInf->Type);

			groupMacro.WaveGroupFrame = Unsorted::CurrentFrame;
		}

		// From here on they are idle army like the rest: they make the count of the next wave and
		// leave with it.
		if (!heldJoinVehicles.empty() || !heldJoinSoldiers.empty())
		{
			idleCombatUnits.insert(idleCombatUnits.end(), heldJoinVehicles.begin(), heldJoinVehicles.end());
			idleCombatInf.insert(idleCombatInf.end(), heldJoinSoldiers.begin(), heldJoinSoldiers.end());

			if (IsAITickWindow(900, 0))
			{
				Debug::Log("[NeuralAI] Frame %d | House #%d held at home join the idle army: %d vehicles and %d soldiers\n",
					Unsorted::CurrentFrame, pHouse->ArrayIndex, static_cast<int>(heldJoinVehicles.size()),
					static_cast<int>(heldJoinSoldiers.size()));
			}
		}
	}

	// Strength gate on top of the unit count: the main wave (and the same-landmass push below)
	// waits until our idle army clearly outweighs what this house knows of the enemy at the
	// target, or a window opens (see EvaluateWaveLaunch). The endgame remnant hunt never waits.
	// The unit count a team wave asks for (waveUnitsNeeded) may be below our own minimum.
	// Only what gets to the target by itself makes the count (FootReachesAlone): with the army on
	// another island the wave was three or five flying units at a time, launched because the
	// vehicles that stayed behind made the number (394 waves of about 5 in 20000 frames, match #9885).
	size_t waveAbleUnits = idleCombatUnits.size();
	size_t waveAbleInf = idleCombatInf.size();

	// A unit that does not get there alone counts when a transport operation can take it: a
	// transport of ours, or one we can build, carries its type (the request checks the very
	// crossing). Without this the wave never left an island and nobody asked for a transport.
	// The strength gate below weighs the same units.
	std::unordered_map<const TechnoTypeClass*, bool> ferriedTypes;

	auto const reachesWaveGoal = [&](FootClass* pFoot)
	{
		if (FootReachesAlone(pFoot, pWaveTargetCell->MapCoords, pWaveTargetCell->GetBuilding()))
			return true;

		auto const pType = pFoot->GetTechnoType();

		if (!TransportOpsEnabled || IsWaterBound(pType))
			return false;

		auto const [itType, inserted] = ferriedTypes.try_emplace(pType, false);

		if (inserted)
			itType->second = CanHouseFerryTypeAcrossWater(pHouse, pType);

		return itType->second;
	};

	if (pWaveTargetCell && !endgameRemnantHunt)
	{
		waveAbleUnits = static_cast<size_t>(std::count_if(idleCombatUnits.begin(), idleCombatUnits.end(),
			[&](UnitClass* pU) { return reachesWaveGoal(pU); }));
		waveAbleInf = static_cast<size_t>(std::count_if(idleCombatInf.begin(), idleCombatInf.end(),
			[&](InfantryClass* pI) { return reachesWaveGoal(pI); }));
	}

	// The army share floor waits at most WaveFloorWaitMostFrames once the count without it is met:
	// then the wave goes with what it has (user: waiting to gather must have a limit). A dispatch
	// starts the wait again, so the waves after it do not stream out at the smaller count.
	{
		auto const countMet = [&waveAbleUnits, &waveAbleInf](size_t needed)
		{
			return waveAbleUnits >= needed || waveAbleUnits + waveAbleInf >= needed + 4 || waveAbleInf >= needed + 3;
		};
		const int floorNow = Unsorted::CurrentFrame;

		if (pWaveTargetCell && !endgameRemnantHunt && waveUnitsNeeded > waveUnitsWithoutShare && countMet(waveUnitsWithoutShare)
			&& !countMet(waveUnitsNeeded))
		{
			if (waveMacro.WaveFloorWaitSince < 0 || waveMacro.WaveFloorWaitSince > floorNow
				|| waveMacro.LastWaveDispatchFrame >= waveMacro.WaveFloorWaitSince)
			{
				waveMacro.WaveFloorWaitSince = floorNow;
				waveMacro.WaveFloorWaitLogged = false;
			}
			else if (floorNow - waveMacro.WaveFloorWaitSince >= WaveFloorWaitMostFrames)
			{
				if (!waveMacro.WaveFloorWaitLogged)
				{
					Debug::Log("[NeuralAI] Frame %d | House #%d wave floor waited %d frames: the wave asks for %d units instead of %d "
						"(%d idle reach the target, %d soldiers)\n", floorNow, pHouse->ArrayIndex, floorNow - waveMacro.WaveFloorWaitSince,
						static_cast<int>(waveUnitsWithoutShare), static_cast<int>(waveUnitsNeeded), static_cast<int>(waveAbleUnits),
						static_cast<int>(waveAbleInf));
					waveMacro.WaveFloorWaitLogged = true;
				}

				waveUnitsNeeded = waveUnitsWithoutShare;
			}
		}
		else
		{
			waveMacro.WaveFloorWaitSince = -1;
		}
	}

	const bool waveCountReady = waveAbleUnits >= waveUnitsNeeded
		|| waveAbleUnits + waveAbleInf >= waveUnitsNeeded + 4 || waveAbleInf >= waveUnitsNeeded + 3;

	// Wave gate inputs, every 900 frames per house: why a house with an army never reaches the wave.
	if (IsAITickWindow(900, 0))
	{
		auto const missions = [](const std::map<int, int>& byMission)
		{
			std::string text;

			for (auto const& [mission, count] : byMission)
				text += " m" + std::to_string(mission) + "x" + std::to_string(count);

			return text.empty() ? std::string(" none") : text;
		};

		Debug::Log("[NeuralAI] Frame %d | House #%d wave gate: idle units %d (%d reach the target) inf %d (%d) needed %d, not idle busy %d "
			"plan %d mission %d health %d immobile %d linked %d, inf busy %d plan %d mission %d linked %d, target %s (%d,%d) hold %d "
			"remnant %d army %d share %d | mission ships%s, vehicles%s\n",
			Unsorted::CurrentFrame, pHouse->ArrayIndex, static_cast<int>(idleCombatUnits.size()), static_cast<int>(waveAbleUnits),
			static_cast<int>(idleCombatInf.size()), static_cast<int>(waveAbleInf), static_cast<int>(waveUnitsNeeded), notIdleBusy,
			notIdlePlan, notIdleMission, notIdleHealth, notIdleImmobile, notIdleLinked, notIdleInfBusy, notIdleInfPlan, notIdleInfMission,
			notIdleInfLinked, pWaveTargetCell ? "yes" : "none", pWaveTargetCell ? pWaveTargetCell->MapCoords.X : -1,
			pWaveTargetCell ? pWaveTargetCell->MapCoords.Y : -1, teamOrder.Hold ? 1 : 0, endgameRemnantHunt ? 1 : 0,
			waveArmy, static_cast<int>(armyShareUnits), missions(notIdleMissionShips).c_str(), missions(notIdleMissionVehicles).c_str());
	}
	NeuralWaveCheck waveCheck {};
	waveCheck.Launch = true;

	if (pWaveTargetCell && !endgameRemnantHunt && waveCountReady)
	{
		const CellStruct gateTarget = pWaveTargetCell->MapCoords;
		int readyValue = 0;

		// The units the count took (reachesWaveGoal). With the zone of the target cell alone, a wave
		// whose army needs a ferry, or whose target cell lies in another zone than the ground around
		// it, weighed 0 with its count met and was held ("value 0" in 5 of 44 matches, most of them
		// on water maps: #11875, #11904, #11906, #11917, #11925).
		auto const addReadyValue = [&](FootClass* pFoot)
		{
			if (pFoot->GetMapCoords().DistanceFrom(gateTarget) > 14.0 && reachesWaveGoal(pFoot))
				readyValue += OwnGroundFighterValue(pFoot);
		};

		for (auto const pU : idleCombatUnits)
			addReadyValue(pU);

		for (auto const pI : idleCombatInf)
			addReadyValue(pI);

		// The waves before on their way to this target count too: each packet was judged alone
		// against the whole defense (match #11465: "value 159 vs 1035 expected" while 47 units of
		// the house marched there).
		auto const addEnRouteValue = [&](FootClass* pFoot)
		{
			if (!pFoot || pFoot->Owner != pHouse || !pFoot->IsAlive || pFoot->InLimbo || !IsFootActivelyAttackingOrMovingToEnemy(pFoot))
				return;

			auto const itGoal = waveMacro.WaveGoals.find(pFoot->UniqueID);

			if (itGoal != waveMacro.WaveGoals.end() && itGoal->second.Cell.DistanceFrom(gateTarget) <= 12.0
				&& pFoot->GetMapCoords().DistanceFrom(gateTarget) > 14.0)
			{
				readyValue += OwnGroundFighterValue(pFoot);
			}
		};

		for (auto const pU : UnitClass::Array)
			addEnRouteValue(pU);

		for (auto const pI : InfantryClass::Array)
			addEnRouteValue(pI);

		waveCheck = EvaluateWaveLaunch(pHouse, gateTarget, readyValue,
			counterattackWindow || waveMacro.BlackoutUntilFrame > Unsorted::CurrentFrame, waveMass);

		NoteWaveDecision(pHouse, waveCheck, false, 0);
	}

	// The tank mass takes anti-air along as a strong preference, not a rule: with enemy air
	// presence known, a wave with nothing that shoots at aircraft waits for it while the house
	// can build one, at most TankMassEscortWaitFrames; otherwise it goes as it is.
	bool waveAwaitsEscort = false;

	if (tankMassWaves && pWaveTargetCell && !endgameRemnantHunt && waveCountReady && waveCheck.Launch)
	{
		auto const pEscortGoal = pWaveTargetCell->GetBuilding();
		const bool escorted = std::any_of(idleCombatUnits.begin(), idleCombatUnits.end(),
				[](UnitClass* pU) { return ShootsAtAircraft(pU->Type); })
			|| std::any_of(idleCombatInf.begin(), idleCombatInf.end(), [](InfantryClass* pI) { return ShootsAtAircraft(pI->Type); });
		const bool escortWanted = !escorted
			&& EnemyAirPresenceKnown(pHouse, (pEscortGoal && pEscortGoal->Owner) ? pEscortGoal->Owner->ArrayIndex : -1);
		const int frame = Unsorted::CurrentFrame;

		if (!escortWanted)
		{
			waveMacro.WaveEscortWaitSince = -1;
		}
		else if (waveMacro.WaveEscortWaitSince >= 0)
		{
			// Waited long enough: it goes without, and does not wait again for a while.
			if (frame - waveMacro.WaveEscortWaitSince >= TankMassEscortWaitFrames)
			{
				waveMacro.WaveEscortWaitSince = -1;
				waveMacro.WaveEscortRetryFrame = frame + TankMassEscortRetryFrames;
				Debug::Log("[NeuralAI] Frame %d | House #%d wave goes without anti-air escort: none came in time\n", frame,
					pHouse->ArrayIndex);
			}
		}
		else if (frame >= waveMacro.WaveEscortRetryFrame)
		{
			if (CanBuildAntiAirEscort(pHouse))
			{
				waveMacro.WaveEscortWaitSince = frame;
				Debug::Log("[NeuralAI] Frame %d | House #%d wave waits for its anti-air escort\n", frame, pHouse->ArrayIndex);
			}
			else
			{
				waveMacro.WaveEscortRetryFrame = frame + TankMassEscortRetryFrames;
				Debug::Log("[NeuralAI] Frame %d | House #%d wave goes without anti-air escort: none can be built\n", frame,
					pHouse->ArrayIndex);
			}
		}

		waveAwaitsEscort = escortWanted && waveMacro.WaveEscortWaitSince >= 0;
	}

	// A base full of idle fighting vehicles of the tank mass, whatever their types, goes out as a
	// wave without waiting for the strength gate or the escort (user: use them, and the base is
	// free of them).
	const bool massOverflow = tankMassWaves && waveAbleUnits >= static_cast<size_t>(TankMassOverflowVehicles);
	const bool waveStrengthReady = (waveCheck.Launch && !waveAwaitsEscort) || massOverflow;

	// Coordinated Multi-Squadron Offensive Strike Wave (Slot 37: Rush vs Mass
	// Timing) Splits forces into parallel Squadron Alpha (pWaveTargetCell) and
	// Squadron Bravo (pSecondaryWaveTargetCell)
	if (pWaveTargetCell && !teamOrder.Hold
		&& (endgameRemnantHunt || ((waveAbleUnits >= waveUnitsNeeded
			|| (waveAbleUnits + waveAbleInf >= waveUnitsNeeded + 4)) && waveStrengthReady)))
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

		// A staging point needs room for the wave: most of the 7x7 cells around it free ground of the
		// target zone (no building, water or cliff) and no building of ours or an ally within 3 cells.
		// A point in a gap between our buildings, a corridor or a bridgehead (bases next to each other)
		// packed the wave into a block that stopped our own traffic, harvesters included, for good.
		auto const isOpenStagingArea = [&](const CellStruct& center, int minFree) -> bool
		{
			int freeCells = 0;

			for (int dy = -3; dy <= 3; ++dy)
			{
				for (int dx = -3; dx <= 3; ++dx)
				{
					const CellStruct c { static_cast<short>(center.X + dx), static_cast<short>(center.Y + dy) };
					auto const pC = MapClass::Instance.TryGetCellAt(c);

					if (!pC)
						continue;

					auto const pB = pC->GetBuilding();

					if (pB && pB->Owner && pHouse->IsAlliedWith(pB->Owner))
						return false;

					if (!pB && pC->LandType != LandType::Water
						&& MapClass::Instance.GetMovementZoneType(c, MovementZone::Normal, false) == waveTargetZone)
					{
						++freeCells;
					}
				}
			}

			return freeCells >= minFree;
		};

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
							NeuralWaitAsker stagingAsker {};
							stagingAsker.Zone = waveTargetZone;

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
											// Land of the target's zone clear of everything but units (IsWaitCellAllowed), in open ground.
											if (IsWaitCellAllowed(sc, stagingAsker, WaitLand | WaitZone | WaitClearIgnoringUnits)
												&& isOpenStagingArea(sc, pass == 0 ? 38 : 30))
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

		// A hold that has not gathered the wave in 2700 frames (blocked route, units that never
		// arrive) releases it and rests 2700 frames: the wave goes as it is instead of waiting forever.
		{
			const int nowFrame = Unsorted::CurrentFrame;

			if (!holdAtForwardStaging)
			{
				waveMacro.StagingHoldSince = -1;
			}
			else if (nowFrame < waveMacro.StagingHoldPauseUntil)
			{
				holdAtForwardStaging = false;
			}
			else if (waveMacro.StagingHoldSince < 0 || waveMacro.StagingHoldSince > nowFrame)
			{
				waveMacro.StagingHoldSince = nowFrame;
			}
			else if (nowFrame - waveMacro.StagingHoldSince > 2700)
			{
				waveMacro.StagingHoldPauseUntil = nowFrame + 2700;
				waveMacro.StagingHoldSince = -1;
				holdAtForwardStaging = false;
				CountDecision(NeuralDecision::StagingHoldTimedOut, pHouse->ArrayIndex);
			}
		}

		// A unit an earlier wave sent somewhere else keeps going there until its mission ends
		// (IsOnWaveMarch: it arrived, the enemy there is gone or at truce, no path, blocked): this wave
		// takes the units that are free, that arrived, or whose goal is gone. The target is picked anew at
		// every tick, and units held in a jam were turned round by every wave that went another way.
		auto const keptWaveGoal = [&](const FootClass* pFoot, const CellClass* pNewGoal, int& zone) -> CellClass*
		{
			CellStruct kept = CellStruct::Empty;
			int keptZone = -1;

			if (endgameRemnantHunt || !IsOnWaveMarch(pHouse, pFoot, &kept, &keptZone) || kept.DistanceFrom(pNewGoal->MapCoords) <= WaveGoalKeepCells)
				return nullptr;

			// TEMP diagnostic (wave march, 2026-11-19): a goal kept past the old WaveGoalKeepFrames.
			auto const itGoal = waveMacro.WaveGoals.find(pFoot->UniqueID);
			const int marchFrames = itGoal != waveMacro.WaveGoals.end() ? Unsorted::CurrentFrame - itGoal->second.Frame : 0;

			if (marchFrames > WaveGoalKeepFrames && waveMacro.WaveMarchLogCount < WaveMarchLogLines)
			{
				waveMacro.WaveMarchLogCount++;
				Debug::Log("[NeuralAI] Frame %d | House #%d wave march: %s %u keeps (%d,%d) after %d frames, %d cells away, new goal (%d,%d)\n",
					Unsorted::CurrentFrame, pHouse->ArrayIndex, pFoot->GetTechnoType()->ID, pFoot->UniqueID, kept.X, kept.Y, marchFrames,
					static_cast<int>(pFoot->GetMapCoords().DistanceFrom(kept)), pNewGoal->MapCoords.X, pNewGoal->MapCoords.Y);
			}

			zone = keptZone;

			return MapClass::Instance.TryGetCellAt(kept);
		};

		size_t dispatchedVeh = 0;
		// Units of this wave handed to the transport operations manager.
		size_t handedVeh = 0;
		size_t handedInf = 0;
		// A unit far from a goal on its own ground that the path finder does not reach rides a
		// transport operation to the landing by it (PathDeadFerry*). True when the wave dispatch is
		// done with the unit: handed over, or left where it is with no transport for it.
		auto const ridesToPathDeadGoal = [&](FootClass* pFoot, const CellStruct& goal, bool& handed)
		{
			handed = false;

			if (!IsPathDeadGoal(pHouse, goal) || pFoot->GetMapCoords().DistanceFrom(goal) <= PathDeadFerryNearCells)
				return false;

			const CellStruct landing = pathDeadFerryLanding(goal);

			if (landing != CellStruct::Empty && RequestStrikeTransport(pHouse, pFoot, landing, goal))
			{
				handed = true;
				const int tried = Unsorted::CurrentFrame - waveMacro.PathDeadFerrySince;

				if (waveMacro.PathDeadFerryGoal == CellStruct::Empty || waveMacro.PathDeadFerryGoal.DistanceFrom(goal) > PathDeadFerryGoalCells
					|| tried > PathDeadFerryTryFrames + PathDeadFerryRestFrames || tried < 0)
				{
					waveMacro.PathDeadFerryGoal = goal;
					waveMacro.PathDeadFerryLanding = landing;
					waveMacro.PathDeadFerrySince = Unsorted::CurrentFrame;
					Debug::Log("[NeuralAI] Frame %d | House #%d wave target at (%d,%d) has no path: transports take the wave to the landing "
						"(%d,%d)\n", Unsorted::CurrentFrame, pHouse->ArrayIndex, goal.X, goal.Y, landing.X, landing.Y);
				}
			}

			return true;
		};

		// Siege screen (SiegeScreen*): the centre of the wave's guns on its target's ground (those with a
		// wave goal or idle for this wave), for the main battle tanks of an archetype 7 house.
		CellStruct waveGunsCenter = CellStruct::Empty;

		if (HouseExt::Fetch(pHouse)->NeuralAI_Archetype == SiegeScreenArchetype)
		{
			int sumX = 0;
			int sumY = 0;
			int guns = 0;

			for (auto const pGun : UnitClass::Array)
			{
				if (!pGun || pGun->Owner != pHouse || !IsSiegeScreenGun(pGun)
					|| (waveTargetZone >= 0 && ObjectZone(pGun, MovementZone::Normal) != waveTargetZone)
					|| (!waveMacro.WaveGoals.contains(pGun->UniqueID)
						&& std::find(idleCombatUnits.begin(), idleCombatUnits.end(), pGun) == idleCombatUnits.end()))
				{
					continue;
				}

				const CellStruct at = pGun->GetMapCoords();
				sumX += at.X;
				sumY += at.Y;
				guns++;
			}

			if (guns > 0)
				waveGunsCenter = CellStruct { static_cast<short>(sumX / guns), static_cast<short>(sumY / guns) };
		}

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

			if (pUnitWaveGoal == pWaveTargetCell && pSecondaryWaveTargetCell
				&& idleCombatUnits.size() >= (minWaveUnits + 2) && (pU->UniqueID % 3 == 0)
				&& (canCrossWaterDirectly || (secWaveZone >= 0 && uz == secWaveZone)))
			{
				pUnitWaveGoal = pSecondaryWaveTargetCell;
				goalZone = secWaveZone;
			}

			// Still on its way to where an earlier wave sent it: it keeps that goal.
			bool keepsEarlierGoal = false;

			if (pUnitWaveGoal == pWaveTargetCell || pUnitWaveGoal == pSecondaryWaveTargetCell)
			{
				if (CellClass* const pKept = keptWaveGoal(pU, pUnitWaveGoal, goalZone))
				{
					pUnitWaveGoal = pKept;
					keepsEarlierGoal = true;
				}
			}

			// A unit that cannot drive there asks the transport operations manager to take it across
			// (user: its mission waits meanwhile); with no operation it waits on its island.
			const bool goalInWater = (pUnitWaveGoal->LandType == LandType::Water);
			if (!canCrossWaterDirectly && (goalInWater || goalZone < 0 || uz < 0 || uz != goalZone))
			{
				const bool handed = RequestTransportOperation(pHouse, NeuralOpClient::Wave, pU, pUnitWaveGoal->MapCoords);

				if (handed)
					++handedVeh;

				continue;
			}

			if (bool rides = false; !canCrossWaterDirectly && ridesToPathDeadGoal(pU, pUnitWaveGoal->MapCoords, rides))
			{
				handedVeh += rides ? 1 : 0;
				continue;
			}

			if (!keepsEarlierGoal && (pUnitWaveGoal == pWaveTargetCell || pUnitWaveGoal == pSecondaryWaveTargetCell))
				waveMacro.WaveGoals[pU->UniqueID] = { pUnitWaveGoal->MapCoords, goalZone, Unsorted::CurrentFrame };

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

				if (waveGunsCenter != CellStruct::Empty && HoldSiegeScreen(pHouse, pU, pUnitWaveGoal->MapCoords, waveGunsCenter))
				{
					// The siege screen holds it out of the known defenses while the wave's guns work.
				}
				else if (pVisibleEnemyBld && pUnitWaveGoal == pWaveTargetCell && CanFootDamageBuildings(pU)
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
				// A unit that unfolds into a building is not sent hunting: Tick Tanks on that mission
				// were seen unfolding at targets beyond their reach.
				pU->ClearMegaMissionData();
				pU->QueueMission(pU->Type->DeploysInto ? Mission::Area_Guard : Mission::Hunt, true);
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

			if (pInfWaveGoal == pWaveTargetCell && pSecondaryWaveTargetCell && idleCombatInf.size() >= 5
				&& (pI->UniqueID % 3 == 0) && (canInfCrossWater || (secWaveZone >= 0 && iz == secWaveZone)))
			{
				pInfWaveGoal = pSecondaryWaveTargetCell;
			}

			// Still on its way to where an earlier wave sent it: it keeps that goal.
			bool infKeepsEarlierGoal = false;
			int infGoalZone = (pInfWaveGoal == pSecondaryWaveTargetCell) ? secWaveZone : waveTargetZone;

			if (pInfWaveGoal == pWaveTargetCell || pInfWaveGoal == pSecondaryWaveTargetCell)
			{
				if (CellClass* const pKept = keptWaveGoal(pI, pInfWaveGoal, infGoalZone))
				{
					pInfWaveGoal = pKept;
					infKeepsEarlierGoal = true;
				}
			}

			// Reachability on foot uses the soldier's own movement zone, since infantry walks where
			// vehicles cannot. Ground infantry that cannot walk there asks the transport operations
			// manager to take it across; with no operation it waits on its island.
			const bool infGoalInWater = (pInfWaveGoal->LandType == LandType::Water);
			const int footZone = ObjectZone(pI, pI->Type->MovementZone);
			const int goalFootZone = MapClass::Instance.GetMovementZoneType(pInfWaveGoal->MapCoords, pI->Type->MovementZone, false);

			if (!canInfCrossWater && (infGoalInWater || footZone < 0 || footZone != goalFootZone))
			{
				const bool handed = RequestTransportOperation(pHouse, NeuralOpClient::Wave, pI, pInfWaveGoal->MapCoords);

				if (handed)
					++handedInf;

				continue;
			}

			if (bool rides = false; !canInfCrossWater && ridesToPathDeadGoal(pI, pInfWaveGoal->MapCoords, rides))
			{
				handedInf += rides ? 1 : 0;
				continue;
			}

			if (!infKeepsEarlierGoal && (pInfWaveGoal == pWaveTargetCell || pInfWaveGoal == pSecondaryWaveTargetCell))
				waveMacro.WaveGoals[pI->UniqueID] = { pInfWaveGoal->MapCoords, infGoalZone, Unsorted::CurrentFrame };

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

		// The units handed to the transport operations go with the wave: they reach it later.
		if (dispatchedVeh > 0 || dispatchedInf > 0 || handedVeh > 0 || handedInf > 0)
		{
			// A new wave starts after 900 quiet frames or on a different target (wave memory).
			if (Unsorted::CurrentFrame - waveMacro.LastWaveDispatchFrame > 900 || waveMacro.LastWaveTargetCell == CellStruct::Empty
				|| waveMacro.LastWaveTargetCell.DistanceFrom(pWaveTargetCell->MapCoords) > 12.0)
			{
				auto const pWaveBld = pWaveTargetCell->GetBuilding();
				const int waveOwner = (pWaveBld && pWaveBld->Owner) ? pWaveBld->Owner->ArrayIndex : -1;

				// The verdict of the wave before reads its start frame: it comes first. The mission of
				// this wave is the one its target stands for, our ground before the reprisal.
				if (tankMassWaves && !endgameRemnantHunt)
				{
					const int goalZone = MapClass::Instance.GetMovementZoneType(pWaveTargetCell->MapCoords, MovementZone::Normal,
						pWaveBld && pWaveBld->OnBridge);
					const int mission = (pWaveBld && ourGroundBonus > 0 && nearOurGround(pWaveTargetCell->MapCoords, goalZone))
						? static_cast<int>(NeuralPlanId::ClearOurGround)
						: ((waveOwner >= 0 && waveOwner == reprisalHouse && reprisalBonus > 0 && missionLand(goalZone))
							? static_cast<int>(NeuralPlanId::Reprisal)
							: -1);
					NoteWaveForVerdict(pHouse, pWaveTargetCell->MapCoords, mission, waveOwner);

					// TEMP diagnostic (P8c, 2026-11-20): whether the target stands on high ground with
					// ground of our zone CellClass::BridgeLevels or more below it within 8 cells, and how
					// many of the idle units and soldiers the wave draws from fire a projectile the cliff
					// blocks going up (SubjectToCliffs: the unit then drives round by the ramps to get a
					// shot, user). At most 200 lines a match.
					static int cliffDiagSerial = -1;
					static int cliffDiagLines = 0;

					if (cliffDiagSerial != NeuralMatchSerial)
					{
						cliffDiagSerial = NeuralMatchSerial;
						cliffDiagLines = 0;
					}

					if (cliffDiagLines < 200)
					{
						++cliffDiagLines;
						const CellStruct cliffTarget = pWaveTargetCell->MapCoords;
						const int targetLevel = pWaveTargetCell->Level;
						int groundBelow = 0;

						for (short dy = -8; dy <= 8; ++dy)
						{
							for (short dx = -8; dx <= 8; ++dx)
							{
								const CellStruct belowCell { static_cast<short>(cliffTarget.X + dx), static_cast<short>(cliffTarget.Y + dy) };
								auto const pBelow = MapClass::Instance.TryGetCellAt(belowCell);

								if (pBelow && dx * dx + dy * dy <= 64 && pBelow->Level <= targetLevel - CellClass::BridgeLevels
									&& MapClass::Instance.GetMovementZoneType(belowCell, MovementZone::Normal, false) == homeGroundZone)
								{
									groundBelow++;
								}
							}
						}

						auto const blockedUp = [](TechnoTypeClass* pType)
						{
							auto const pWeapon = pType->GetWeapon(0) ? pType->GetWeapon(0)->WeaponType : nullptr;
							return pWeapon && pWeapon->Projectile && pWeapon->Projectile->SubjectToCliffs;
						};
						const int unitsBlocked = static_cast<int>(std::count_if(idleCombatUnits.begin(), idleCombatUnits.end(),
							[&blockedUp](UnitClass* pU) { return blockedUp(pU->Type); }));
						const int soldiersBlocked = static_cast<int>(std::count_if(idleCombatInf.begin(), idleCombatInf.end(),
							[&blockedUp](InfantryClass* pI) { return blockedUp(pI->Type); }));

						Debug::Log("[NeuralAI] Frame %d | House #%d wave cliff DIAG: target (%d,%d) level %d, %d cells of our zone %d+ levels "
							"below within 8, idle units %d (%d blocked going up), idle soldiers %d (%d blocked)\n", Unsorted::CurrentFrame,
							pHouse->ArrayIndex, cliffTarget.X, cliffTarget.Y, targetLevel, groundBelow, CellClass::BridgeLevels,
							static_cast<int>(idleCombatUnits.size()), unitsBlocked, static_cast<int>(idleCombatInf.size()), soldiersBlocked);
					}
				}

				waveMacro.WaveStartFrame = Unsorted::CurrentFrame;

				if (!endgameRemnantHunt)
					NoteWaveDecision(pHouse, waveCheck, true, static_cast<int>(dispatchedVeh + dispatchedInf + handedVeh + handedInf));

				// Coalition telemetry: whose building this new wave goes for.
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
			NoteTeamWaveDispatched(pHouse, teamOrder, dispatchedVeh + handedVeh, dispatchedInf + handedInf);

			Debug::Log("[NeuralAI] Frame %d | House #%d (%s) launched Offensive Wave "
					"(%zu Vehicles + %zu Infantry) -> Target Cell (%d,%d)\n",
				Unsorted::CurrentFrame, pHouse->ArrayIndex,
				(pHouse->Type && pHouse->Type->ID[0]) ? pHouse->Type->ID : "AI", dispatchedVeh, dispatchedInf,
				pWaveTargetCell->MapCoords.X, pWaveTargetCell->MapCoords.Y);

			if (handedVeh > 0 || handedInf > 0)
			{
				Debug::Log("[NeuralAI] Frame %d | House #%d wave to (%d,%d): %zu vehicles and %zu infantry handed to transport operations\n",
					Unsorted::CurrentFrame, pHouse->ArrayIndex, pWaveTargetCell->MapCoords.X, pWaveTargetCell->MapCoords.Y, handedVeh, handedInf);
			}
		}

		// Goals past their time go (units that died or went on to other work).
		if (waveMacro.WaveGoals.size() > 256)
		{
			std::erase_if(waveMacro.WaveGoals, [](auto const& entry)
				{ return Unsorted::CurrentFrame - entry.second.Frame > WaveMarchMostFrames; });
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

		// A building counts only when water of the ship's own sea lies within the reach of its land gun
		// against it (otherReach for a ship without one): coarse probes for any water within 6 cells sent
		// guns of 5 cells at buildings they never hit, the ships parked at the nearest water for good
		// (match #9846).
		auto findCoastalNavalBombardmentTarget
			= [pHouse](UnitClass* pShip, const WeaponTypeClass* pLandGun, int otherReach) -> BuildingClass*
		{
			struct NavalCand
			{
				BuildingClass* Bld;
				int Score;
			};
			NavalCand topCands[4]
				= { { nullptr, -999999 }, { nullptr, -999999 }, { nullptr, -999999 }, { nullptr, -999999 } };
			const CellStruct shipCoords = pShip->GetMapCoords();
			const MovementZone shipMZ = pShip->Type->MovementZone;
			const int shipZone = ObjectZone(pShip, shipMZ);

			if (shipZone < 0)
				return nullptr;

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

				const int reach = pLandGun ? WeaponReachAgainst(pLandGun, pEB->Type) : otherReach;

				if (!IsNavalShoreWithin(ebc, shipMZ, shipZone, reach))
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

			return topCands[pShip->UniqueID % static_cast<unsigned int>(validCount)].Bld;
		};

		// A cell of the ship's own sea from which its shot of reachLeptons gets to the building (height
		// counts, as for IsNavalShoreWithin): first on the line from the building toward the ship, shifted
		// sideways by the ship's id so a group spreads, as far out as the reach allows; else the one
		// nearest the ship; nullptr when there is none.
		// minLeptons: the minimum range of the weapon that fires; the cell keeps one cell more off the
		// target (match #11450: a missile ship with a minimum range of 8 cells was sent closer, where it
		// cannot fire).
		auto findOffshoreFiringWaterCell
			= [](UnitClass* pShip, BuildingClass* pTarget, int reachLeptons, int minLeptons) -> CellClass*
		{
			const MovementZone shipMZ = pShip->Type->MovementZone;
			const int shipZone = ObjectZone(pShip, shipMZ);

			if (shipZone < 0)
				return nullptr;

			const CellStruct targetCoords = pTarget->GetMapCoords();
			auto const pTargetCell = pTarget->GetCell();
			const int targetLevel = pTargetCell ? pTargetCell->Level : 0;
			const int maxR = std::clamp((reachLeptons - NavalReachMarginLeptons) / Unsorted::LeptonsPerCell, 1, NavalShoreSearchCells);

			auto const firesFrom = [&](const CellStruct& c, CellClass* pWC)
			{
				if (!pWC || MapClass::Instance.GetMovementZoneType(c, shipMZ, false) != shipZone
					|| !pWC->IsClearToMove(pShip->Type->SpeedType, true, true, -1, shipMZ, -1, false))
				{
					return false;
				}

				const double flat = c.DistanceFrom(targetCoords) * Unsorted::LeptonsPerCell;
				const double rise = std::max(0, targetLevel - pWC->Level) * Unsorted::LevelHeight;
				const double reach = std::sqrt(flat * flat + rise * rise);

				return reach + NavalReachMarginLeptons <= reachLeptons
					&& (minLeptons <= 0 || reach >= minLeptons + Unsorted::LeptonsPerCell);
			};

			const CellStruct sc = pShip->GetMapCoords();
			const int dx = sc.X - targetCoords.X;
			const int dy = sc.Y - targetCoords.Y;
			const int dist = std::max(1, std::max(std::abs(dx), std::abs(dy)));
			const int lat = (static_cast<int>(pShip->UniqueID % 5) - 2) * 2;
			const int perpX = (-dy * lat) / dist;
			const int perpY = (dx * lat) / dist;

			for (int r = maxR; r >= std::max(1, maxR - 3); --r)
			{
				const CellStruct probe { static_cast<short>(targetCoords.X + (dx * r) / dist + perpX),
					static_cast<short>(targetCoords.Y + (dy * r) / dist + perpY) };

				if (auto const pWC = MapClass::Instance.TryGetCellAt(probe); pWC && firesFrom(probe, pWC))
					return pWC;
			}

			CellClass* pBestWater = nullptr;
			int bestWaterDist = INT_MAX;

			for (int wy = -maxR; wy <= maxR; ++wy)
			{
				for (int wx = -maxR; wx <= maxR; ++wx)
				{
					const CellStruct wc { static_cast<short>(targetCoords.X + wx), static_cast<short>(targetCoords.Y + wy) };
					const int dShip = std::abs(wc.X - sc.X) + std::abs(wc.Y - sc.Y);

					if (dShip >= bestWaterDist)
						continue;

					if (auto const pWC = MapClass::Instance.TryGetCellAt(wc); pWC && firesFrom(wc, pWC))
					{
						bestWaterDist = dShip;
						pBestWater = pWC;
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

		// TEMP diagnostic: what the idle ships that fight only at sea got this tick.
		int seaHunters = 0;
		int seaHuntersSent = 0;
		// Idle warships with nothing in reach this tick: no target for their pick and no enemy ship seen in
		// their sea (NoteShipsWithoutTarget, the shipyard's brake), and those of them a naval sweep may take.
		int shipsWithoutTarget = 0;
		std::vector<UnitClass*> sweepCandidates;
		// TEMP diagnostic: the idle warships of this tick and what their pick sent them at.
		int navalPicking = 0;
		int navalAtAfloat = 0;
		int navalAtLoaded = 0;
		int navalAtAshore = 0;
		int navalAtBuilding = 0;

		// Enemy units an idle warship may go for (navalPreyFor): the enemy ships seen (seenEnemyShips) and the
		// other enemy vehicles and soldiers we know of by the vision rule (IsCellKnownToAI), none in the air. Built
		// at the first warship of the tick.
		std::vector<TechnoClass*> navalPrey;
		bool navalPreyBuilt = false;

		auto const buildNavalPrey = [&]()
		{
			if (navalPreyBuilt)
				return;

			navalPreyBuilt = true;
			navalPrey.assign(seenEnemyShips.begin(), seenEnemyShips.end());

			auto const consider = [&](TechnoClass* pTechno)
			{
				if (!pTechno || pTechno->InLimbo || !pTechno->IsAlive || !pTechno->Owner || pTechno->IsInAir() || pTechno->Owner == pHouse
					|| pHouse->IsAlliedWith(pTechno->Owner) || pTechno->Owner->IsNeutral() || pTechno->Owner->IsObserver()
					|| pTechno->Owner->Defeated || !IsCellKnownToAI(pHouse, pTechno->GetMapCoords()))
				{
					return;
				}

				navalPrey.push_back(pTechno);
			};

			for (auto const pEnemyUnit : UnitClass::Array)
			{
				if (pEnemyUnit && pEnemyUnit->Type && !pEnemyUnit->Type->Naval)
					consider(pEnemyUnit);
			}

			for (auto const pEnemyInf : InfantryClass::Array)
				consider(pEnemyInf);
		};

		// Can pFrom fire at pAt with the weapon the engine picks for it, range apart?
		auto const firesAt = [](TechnoClass* pFrom, TechnoClass* pAt)
		{
			const FireError error = pFrom->GetFireError(pAt, pFrom->SelectWeapon(pAt), true);

			return error != FireError::ILLEGAL && error != FireError::CANT && error != FireError::NONE;
		};

		// What a target is worth to an idle warship (user approved, 2026-11-19): its cost, NavalPreyLoadedBonus
		// for a unit with passengers aboard and NavalPreyThreatBonus for one that can fire at the ship, less
		// NavalPreyCellWeight per cell between them.
		auto const navalPreyBase = [](UnitClass* pShip, TechnoClass* pPrey)
		{
			auto const pPreyType = pPrey->GetTechnoType();
			auto const pPreyFoot = abstract_cast<FootClass*>(pPrey);

			return (pPreyType ? pPreyType->Cost : 0) + ((pPreyFoot && pPreyFoot->Passengers.NumPassengers > 0) ? NavalPreyLoadedBonus : 0)
				- NavalPreyCellWeight * static_cast<int>(pShip->GetMapCoords().DistanceFrom(pPrey->GetMapCoords()));
		};

		// The enemy unit an idle warship goes for: one on the water of its own sea (a ship, a hovercraft, an
		// amphibious transport afloat), or one on other ground within NavalShorePreyCells whose cell the weapon
		// it would fire at it reaches from that sea (IsNavalShoreWithin); in sight (a cloaked one only on a cell
		// our sensors cover) and one the engine lets it fire at, range apart. The best worth (navalPreyBase plus
		// the threat), the lower UniqueID among equals. Ships whose gun hits land looked only for buildings on
		// the coast, never at the enemy ships or at the loaded transports crossing to them (match #11499).
		auto const navalPreyFor = [&](UnitClass* pShip, int& bestScore, bool& afloat) -> TechnoClass*
		{
			buildNavalPrey();
			const MovementZone shipMZ = pShip->Type->MovementZone;
			const int sea = ObjectZone(pShip, shipMZ);
			const CellStruct shipCell = pShip->GetMapCoords();
			TechnoClass* pBest = nullptr;

			if (sea < 0)
				return nullptr;

			for (auto const pPrey : navalPrey)
			{
				auto const pPreyCell = pPrey->GetCell();

				if (!pPreyCell || !pPrey->IsAlive || pPrey->InLimbo)
					continue;

				const CellStruct preyCell = pPrey->GetMapCoords();
				const bool onSea = MapClass::Instance.GetMovementZoneType(preyCell, shipMZ, false) == sea;

				if (!onSea && std::max(std::abs(preyCell.X - shipCell.X), std::abs(preyCell.Y - shipCell.Y)) > NavalShorePreyCells)
					continue;

				const int base = navalPreyBase(pShip, pPrey);

				if (pBest && (base + NavalPreyThreatBonus < bestScore
					|| (base + NavalPreyThreatBonus == bestScore && pPrey->UniqueID > pBest->UniqueID)))
				{
					continue;
				}

				if (pPrey->CloakState != CloakState::Uncloaked && !pPreyCell->Sensors_InclHouse(static_cast<unsigned int>(pHouse->ArrayIndex)))
					continue;

				const int weaponIndex = pShip->SelectWeapon(pPrey);
				const FireError error = pShip->GetFireError(pPrey, weaponIndex, true);

				if (error == FireError::ILLEGAL || error == FireError::CANT || error == FireError::NONE)
					continue;

				if (!onSea)
				{
					auto const pSlot = pShip->GetWeapon(weaponIndex);
					auto const pWeapon = pSlot ? pSlot->WeaponType : nullptr;
					const int reach = !pWeapon ? 0
						: (pWeapon->Spawner ? NavalSpawnerReachCells * Unsorted::LeptonsPerCell : pWeapon->Range - Unsorted::LeptonsPerCell / 3);

					if (reach <= 0 || !IsNavalShoreWithin(preyCell, shipMZ, sea, reach))
						continue;
				}

				const int score = base + (firesAt(pPrey, pShip) ? NavalPreyThreatBonus : 0);

				if (!pBest || score > bestScore || (score == bestScore && pPrey->UniqueID < pBest->UniqueID))
				{
					pBest = pPrey;
					bestScore = score;
					afloat = onSea;
				}
			}

			return pBest;
		};

		// Push gate (PushGate*): the pushes at a building of this tick, vehicles and soldiers, ordered
		// after both loops once the company each target gets is known.
		struct PendingPush
		{
			FootClass* Foot;
			BuildingClass* Target;
			bool AttackNow;
		};

		std::vector<PendingPush> pendingPushes;

		for (auto const pBeachU : idleCombatUnits)
		{
			if (!pBeachU || !pBeachU->Type || IsAmphibiousOrAirTransport(pBeachU->Type))
				continue;

			// A ship let go from an attack out of its reach this tick is idle whatever its mission still reads.
			if (std::find(releasedShips.begin(), releasedShips.end(), pBeachU) == releasedShips.end()
				&& IsFootActivelyAttackingOrMovingToEnemy(pBeachU))
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
				// A ship that waits outside a crowded narrow passage stays where it is (UpdateNavalPassageGate).
				if (IsWaitingAtNavalPassage(pBeachU))
					continue;

				const bool wpn0HitsLand = pWpn0 && (pWpn0->Damage > 0 || pWpn0->Spawner) && LandTargetingAllows(pBeachU->Type, 0)
					&& (!pWpn0->Projectile || pWpn0->Projectile->AG || pWpn0->Spawner || isCarrierSpawner);
				const bool wpn1HitsLand = pWpn1 && (pWpn1->Damage > 0 || pWpn1->Spawner) && LandTargetingAllows(pBeachU->Type, 1)
					&& (!pWpn1->Projectile || pWpn1->Projectile->AG || pWpn1->Spawner);
				const WeaponTypeClass* pLandSiegeWpn = nullptr;
				if (wpn0HitsLand && wpn1HitsLand)
					pLandSiegeWpn = (pWpn1->Range > pWpn0->Range) ? pWpn1 : pWpn0;
				else if (wpn1HitsLand)
					pLandSiegeWpn = pWpn1;
				else if (wpn0HitsLand)
					pLandSiegeWpn = pWpn0;

				// A ship with a gun that hits land shells the coast with it, at the gun's range, whatever
				// else it launches (a destroyer's anti-submarine plane made it a carrier with 18 cells
				// of reach and sent it at buildings far inland). Only a ship without such a gun counts
				// on its aircraft.
				const WeaponTypeClass* pLandGun = nullptr;

				for (auto const pGun : { pWpn0, pWpn1 })
				{
					if (pGun && !pGun->Spawner && pGun->Damage > 0 && (!pGun->Projectile || pGun->Projectile->AG)
						&& LandTargetingAllows(pBeachU->Type, pGun == pWpn0 ? 0 : 1)
						&& (!pLandGun || pGun->Range > pLandGun->Range))
					{
						pLandGun = pGun;
					}
				}

				// One pick for every idle warship (user approved, 2026-11-19): the enemy unit navalPreyFor gives
				// it and, for a ship whose weapon hits land, the coastal building findCoastalNavalBombardmentTarget
				// gives it, scored alike (navalPreyBase, NavalPreyThreatBonus for a building that can fire at the
				// ship), the higher first, the lower UniqueID on a tie. A target in reach (the engine's own test) is
				// attacked from where the ship is; a unit out of reach is attack-moved to (the water nearest to it
				// when it stands on land), a building from an offshore firing cell. A ship that fights only at sea
				// (torpedo submarine, dolphin, squid) has the units alone. With nothing it stays on guard, where
				// the engine gives it what comes near.
				++navalPicking;

				if (!pLandSiegeWpn)
					seaHunters++;

				// The pick weighs every enemy unit in sight: it runs once per NavalPreyPickFrames for a ship, which keeps
				// the last answer in between (one that found nothing still counts for the brake and for a sweep).
				auto& navalMacro = GetMacroState(pHouse);
				auto& lastPick = navalMacro.NavalPicks[pBeachU->UniqueID];
				const int pickFrame = Unsorted::CurrentFrame;

				if (lastPick.Frame > 0 && pickFrame >= lastPick.Frame && pickFrame - lastPick.Frame < NavalPreyPickFrames)
				{
					if (!lastPick.HadTarget && !nearestEnemyShipFor(pBeachU, false))
					{
						++shipsWithoutTarget;
						sweepCandidates.push_back(pBeachU);
					}

					continue;
				}

				const DWORD lastPreyID = lastPick.PreyID;
				int preyScore = INT_MIN;
				bool preyAfloat = false;
				TechnoClass* const pPrey = navalPreyFor(pBeachU, preyScore, preyAfloat);
				BuildingClass* pNavalTargetBld = nullptr;
				int buildingScore = INT_MIN;
				int otherReach = 0;

				if (pLandSiegeWpn)
				{
					otherReach = std::max(static_cast<int>(pLandSiegeWpn->Range),
						isCarrierSpawner ? (NavalSpawnerReachCells * Unsorted::LeptonsPerCell) : 1536);
					pNavalTargetBld = findCoastalNavalBombardmentTarget(pBeachU, pLandGun, otherReach);

					if (pNavalTargetBld)
					{
						auto const pDefenseSlot = pNavalTargetBld->Type->GetWeapon(0);
						const bool armedBuilding = pDefenseSlot && pDefenseSlot->WeaponType;
						buildingScore = navalPreyBase(pBeachU, pNavalTargetBld)
							+ ((armedBuilding && firesAt(pNavalTargetBld, pBeachU)) ? NavalPreyThreatBonus : 0);
					}
				}

				auto const orderBuilding = [&]() -> bool
				{
					// The engine's own reach test: the attack order only when it can fire from here.
					if (pBeachU->IsCloseEnoughToAttack(pNavalTargetBld))
					{
						pBeachU->ClearMegaMissionData();
						pBeachU->SetTarget(pNavalTargetBld);
						pBeachU->QueueMission(Mission::Attack, true);
						navalAtBuilding++;

						return true;
					}

					if (CellClass* pOffshoreFireCell = findOffshoreFiringWaterCell(pBeachU, pNavalTargetBld,
						pLandGun ? WeaponReachAgainst(pLandGun, pNavalTargetBld->Type) : otherReach,
						pLandGun ? pLandGun->MinimumRange : pLandSiegeWpn->MinimumRange))
					{
						OrderFootToAttackMove(pBeachU, pOffshoreFireCell);
						navalAtBuilding++;

						return true;
					}

					return false;
				};

				// A unit on the ship's sea is attacked at once and the engine chases it (an attack-move to the cell it
				// stood on was given again at every tick while it moved); one ashore is attacked once in reach, else
				// the ship attack-moves to the water nearest to it.
				DWORD pickedPreyID = 0;

				auto const orderPrey = [&]() -> bool
				{
					auto const pPreyCell = pPrey->GetCell();

					if (preyAfloat || pBeachU->IsCloseEnoughToAttack(pPrey))
					{
						if (pBeachU->Target != pPrey)
						{
							pBeachU->ClearMegaMissionData();
							pBeachU->SetTarget(pPrey);
							pBeachU->QueueMission(Mission::Attack, true);
						}
					}
					else if (pPreyCell)
					{
						OrderFootToAttackMove(pBeachU, pPreyCell);
					}
					else
					{
						return false;
					}

					if (pPrey->UniqueID != lastPreyID)
						CountDecision(NeuralDecision::NavalPreyAttack, pHouse->ArrayIndex);

					(preyAfloat ? navalAtAfloat : navalAtAshore)++;

					if (pPrey->Passengers.NumPassengers > 0 && abstract_cast<FootClass*>(pPrey))
						navalAtLoaded++;

					pickedPreyID = pPrey->UniqueID;

					return true;
				};

				const bool buildingFirst = pNavalTargetBld && (!pPrey || buildingScore > preyScore
					|| (buildingScore == preyScore && pNavalTargetBld->UniqueID < pPrey->UniqueID));
				bool hasTarget = false;

				if (buildingFirst)
					hasTarget = orderBuilding();

				if (!hasTarget && pPrey)
					hasTarget = orderPrey();

				if (!hasTarget && pNavalTargetBld && !buildingFirst)
					hasTarget = orderBuilding();

				lastPick = { pickFrame, hasTarget, pickedPreyID };

				if (hasTarget)
				{
					navalMacro.NavalTargetFrame = Unsorted::CurrentFrame;
					navalMacro.NavalSweeps.erase(pBeachU->UniqueID);

					if (!pLandSiegeWpn)
						seaHuntersSent++;
				}
				else if (!nearestEnemyShipFor(pBeachU, false))
				{
					++shipsWithoutTarget;
					sweepCandidates.push_back(pBeachU);
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
					const bool attackNow = CanFootDamageBuildings(pBeachU) && pBeachU->DistanceFrom(pSameIslBld) <= maxWpnRangeLeptons + 384
						&& pSameIslBld->GetCell() && IsCellVisibleToAI(pHouse, pSameIslBld->GetCell());
					pendingPushes.push_back({ pBeachU, pSameIslBld, attackNow });
				}
			}
		}

		// TEMP diagnostic: where the ships that fight only at sea stand and what they are on, every
		// 900 frames: how many, on Attack with and without a target, on guard, on the move, and the
		// most of them within 4 cells of one of them (the submarines of match #9872 stood packed).
		if (IsAITickWindow(900, 15))
		{
			std::vector<UnitClass*> seaOnly;

			for (auto const pShip : UnitClass::Array)
			{
				if (!pShip || pShip->Owner != pHouse || !pShip->IsAlive || pShip->InLimbo || !pShip->Type || !pShip->Type->Naval
					|| IsAmphibiousOrAirTransport(pShip->Type) || pShip->Type->Spawns)
				{
					continue;
				}

				bool armed = false;
				bool hitsLand = false;

				for (int i = 0; i < 2; ++i)
				{
					auto const pSlot = pShip->Type->GetWeapon(i);
					auto const pWeapon = pSlot ? pSlot->WeaponType : nullptr;

					if (!pWeapon || (pWeapon->Damage <= 0 && !pWeapon->Spawner))
						continue;

					armed = true;
					hitsLand = hitsLand || pWeapon->Spawner
						|| (LandTargetingAllows(pShip->Type, i) && (!pWeapon->Projectile || pWeapon->Projectile->AG));
				}

				if (armed && !hitsLand)
					seaOnly.push_back(pShip);
			}

			if (!seaOnly.empty())
			{
				int attacking = 0;
				int attackNoTarget = 0;
				int onGuard = 0;
				int onTheMove = 0;
				int packed = 0;

				for (auto const pShip : seaOnly)
				{
					if (pShip->CurrentMission == Mission::Attack)
						(pShip->Target ? attacking : attackNoTarget)++;
					else if (pShip->CurrentMission == Mission::Guard || pShip->CurrentMission == Mission::Area_Guard)
						onGuard++;
					else
						onTheMove++;

					int beside = 0;

					for (auto const pOther : seaOnly)
					{
						if (pOther->GetMapCoords().DistanceFrom(pShip->GetMapCoords()) <= 4.0)
							beside++;
					}

					packed = std::max(packed, beside);
				}

				Debug::Log("[NeuralAI] House %d sea hunters DIAG frame %d: %d ships that fight only at sea: %d on Attack with a target, %d on "
					"Attack with none, %d on guard, %d on the move; most within 4 cells of one %d; idle this tick %d, sent at a "
					"target %d, enemy ships seen %d.\n", pHouse->ArrayIndex, Unsorted::CurrentFrame, static_cast<int>(seaOnly.size()),
					attacking, attackNoTarget, onGuard, onTheMove, packed, seaHunters, seaHuntersSent,
					static_cast<int>(seenEnemyShips.size()));
			}
		}

		// Naval sweep (user approved, 2026-11-19): no ship went to look for the enemy at sea, so a fleet with nothing
		// in reach sat where it was (match #11499: 77 destroyers). With no target found by the pick and no shot of a
		// warship of ours for NavalSweepIdleFrames, and at least NavalSweepMinShips idle warships with nothing in
		// reach, up to NavalSweepShips of those (the fastest, the cheapest among equals, the lower UniqueID; half
		// health at least, none that waits at a passage) attack-move each to its own place of their sea
		// (PickNavalSweepCell). A trip ends within 6 cells of its place, when the ship takes a target or finds one by
		// its pick, when the ship is gone, or after NavalSweepTripFrames. A far order goes in route legs: a sweeper
		// idle short of its place (at the end of a leg) gets the order again. A sea with no place worth a look is
		// searched again only after NavalSweepRetryFrames (with the map revealed every place teaches nothing, and the
		// search ran for every idle warship at every tick).
		{
			auto& sweepMacro = GetMacroState(pHouse);
			auto& sweeps = sweepMacro.NavalSweeps;
			const int now = Unsorted::CurrentFrame;

			std::erase_if(sweeps, [&](auto const& entry)
				{
					auto const pSweeper = FindOwnFootByID(pHouse, entry.first);

					return !pSweeper || now < entry.second.second || now - entry.second.second > NavalSweepTripFrames
						|| pSweeper->Target || pSweeper->GetMapCoords().DistanceFrom(entry.second.first) <= 6.0;
				});

			for (auto const& [uid, trip] : sweeps)
			{
				auto const pSweeper = FindOwnFootByID(pHouse, uid);
				auto const pPlaceCell = MapClass::Instance.TryGetCellAt(trip.first);

				if (pSweeper && pPlaceCell && (pSweeper->CurrentMission == Mission::Guard || pSweeper->CurrentMission == Mission::Area_Guard)
					&& !pSweeper->Destination && !IsWaitingAtNavalPassage(pSweeper) && !IsInTransportOp(pHouse, uid) && !IsPlanMember(pHouse, uid))
				{
					OrderFootToAttackMove(pSweeper, pPlaceCell);
				}
			}

			std::erase_if(sweepMacro.NavalSweepRetryUntil, [now](const auto& entry) { return now >= entry.second || entry.second - now > NavalSweepRetryFrames; });

			if (sweepMacro.NavalPicks.size() > 512)
			{
				std::erase_if(sweepMacro.NavalPicks, [now](const auto& entry)
					{ return now < entry.second.Frame || now - entry.second.Frame >= NavalPreyPickFrames; });
			}

			std::erase_if(sweepCandidates, [&](UnitClass* pShip)
				{
					return sweeps.contains(pShip->UniqueID) || pShip->GetHealthPercentage() < 0.5 || IsWaitingAtNavalPassage(pShip);
				});

			if (sweeps.size() < NavalSweepShips && sweepCandidates.size() >= NavalSweepMinShips
				&& (now < sweepMacro.NavalTargetFrame || now - sweepMacro.NavalTargetFrame >= NavalSweepIdleFrames))
			{
				bool shotLately = false;

				for (auto const pShip : UnitClass::Array)
				{
					if (pShip && pShip->Owner == pHouse && pShip->IsAlive && !pShip->InLimbo && pShip->Type && IsWaterBound(pShip->Type)
						&& pShip->LastFireBulletFrame > 0 && now >= pShip->LastFireBulletFrame && now - pShip->LastFireBulletFrame < NavalSweepIdleFrames)
					{
						shotLately = true;
						break;
					}
				}

				if (!shotLately)
				{
					std::sort(sweepCandidates.begin(), sweepCandidates.end(), [](UnitClass* a, UnitClass* b)
						{
							if (a->Type->Speed != b->Type->Speed)
								return a->Type->Speed > b->Type->Speed;

							if (a->Type->Cost != b->Type->Cost)
								return a->Type->Cost < b->Type->Cost;

							return a->UniqueID < b->UniqueID;
						});

					std::vector<CellStruct> taken;

					for (auto const& [uid, trip] : sweeps)
						taken.push_back(trip.first);

					// TEMP diagnostic: each sweep that sets out, NavalSweepLogLines lines a match.
					static int sweepLogSerial = -1;
					static int sweepLogLines = 0;

					if (sweepLogSerial != NeuralMatchSerial)
					{
						sweepLogSerial = NeuralMatchSerial;
						sweepLogLines = 0;
					}

					for (auto const pShip : sweepCandidates)
					{
						if (sweeps.size() >= NavalSweepShips)
							break;

						const std::pair<int, int> seaKey { static_cast<int>(pShip->Type->MovementZone),
							ObjectZone(pShip, pShip->Type->MovementZone) };

						if (sweepMacro.NavalSweepRetryUntil.contains(seaKey))
							continue;

						const CellStruct place = PickNavalSweepCell(pHouse, pShip, taken);
						auto const pPlaceCell = place != CellStruct::Empty ? MapClass::Instance.TryGetCellAt(place) : nullptr;

						if (!pPlaceCell)
						{
							sweepMacro.NavalSweepRetryUntil[seaKey] = now + NavalSweepRetryFrames;
							continue;
						}

						OrderFootToAttackMove(pShip, pPlaceCell);
						sweeps[pShip->UniqueID] = { place, now };
						taken.push_back(place);
						CountDecision(NeuralDecision::NavalSweep, pHouse->ArrayIndex);

						if (sweepLogLines++ < NavalSweepLogLines)
						{
							Debug::Log("[NeuralAI] House %d naval sweep DIAG frame %d: %s %u at (%d,%d) sent to look at (%d,%d), %d cells; %d idle "
								"warships with nothing in reach, no target for %d frames.\n", pHouse->ArrayIndex, now, pShip->Type->ID,
								pShip->UniqueID, pShip->GetMapCoords().X, pShip->GetMapCoords().Y, place.X, place.Y,
								static_cast<int>(pShip->GetMapCoords().DistanceFrom(place)), static_cast<int>(sweepCandidates.size()),
								now - sweepMacro.NavalTargetFrame);
						}
					}
				}
			}

			// TEMP diagnostic: what the pick of the idle warships gave this tick, every 900 frames, NavalPickLogLines lines
			// a match.
			static int pickLogSerial = -1;
			static int pickLogLines = 0;

			if (pickLogSerial != NeuralMatchSerial)
			{
				pickLogSerial = NeuralMatchSerial;
				pickLogLines = 0;
			}

			if (navalPicking > 0 && IsAITickWindow(900, 15) && pickLogLines++ < NavalPickLogLines)
			{
				Debug::Log("[NeuralAI] House %d naval pick DIAG frame %d: %d idle warships, %d sent at enemy units afloat (%d loaded), %d at "
					"units ashore, %d at buildings, %d with nothing in reach; %d let go from an attack out of reach, %d still on one, %d on "
					"a sweep, %d waiting at a passage.\n", pHouse->ArrayIndex, now, navalPicking, navalAtAfloat, navalAtLoaded, navalAtAshore,
					navalAtBuilding, shipsWithoutTarget, static_cast<int>(releasedShips.size()), shipsOnUnreachableAttack,
					static_cast<int>(sweeps.size()), static_cast<int>(sweepMacro.NavalPassageWaits.size()));
			}
		}

		NoteShipsWithoutTarget(pHouse, shipsWithoutTarget + shipsOnUnreachableAttack);

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
					const bool attackNow = pBeachI->DistanceFrom(pSameIslBld) <= infWpnRange + 384 && pSameIslBld->GetCell()
						&& IsCellVisibleToAI(pHouse, pSameIslBld->GetCell());
					pendingPushes.push_back({ pBeachI, pSameIslBld, attackNow });
				}
			}
		}

		// Push gate (PushGate*): a push at a building inside the reach of known enemy defenses goes
		// only with company enough; without it the unit does not go, and one standing in such a reach
		// steps out of it towards home to wait for the wave.
		if (!pendingPushes.empty())
		{
			std::unordered_map<BuildingClass*, int> pushesAt;
			std::unordered_set<DWORD> pushing;

			for (auto const& push : pendingPushes)
			{
				pushesAt[push.Target]++;
				pushing.insert(push.Foot->UniqueID);
			}

			// The known enemy defenses covering a target, and the company it gets.
			std::unordered_map<BuildingClass*, std::pair<int, int>> gates;

			auto const gateFor = [&](BuildingClass* pTarget) -> std::pair<int, int>
			{
				if (auto const it = gates.find(pTarget); it != gates.end())
					return it->second;

				const CellStruct at = pTarget->GetMapCoords();
				int defenses = 0;

				for (auto const& info : GetKnownEnemyBuildingCache(pHouse).Entries)
				{
					const int range = info.IsDefense ? GroundRangeOf(info.Building->Type) : 0;

					if (range > 0 && info.Coords.DistanceFrom(at) * Unsorted::LeptonsPerCell
						<= range + SiegeScreenMarginCells * Unsorted::LeptonsPerCell)
					{
						defenses++;
					}
				}

				int company = pushesAt[pTarget];

				auto const joins = [&](FootClass* pFoot)
				{
					auto const pType = pFoot ? pFoot->GetTechnoType() : nullptr;

					if (!pType || pFoot->Owner != pHouse || !pFoot->IsAlive || pFoot->InLimbo || pType->Naval || pushing.contains(pFoot->UniqueID))
						return false;

					auto const pWS = pType->GetWeapon(0);

					if (!pWS || !pWS->WeaponType || pWS->WeaponType->Damage <= 0)
						return false;

					if (pFoot->Target == pTarget || pFoot->GetMapCoords().DistanceFrom(at) <= PushGateGroupCells)
						return true;

					auto const pGoing = pFoot->MegaDestination ? pFoot->MegaDestination : pFoot->Destination;

					return pGoing && CellClass::Coord2Cell(pGoing->GetCoords()).DistanceFrom(at) <= PushGateHeadingCells;
				};

				if (defenses > 0)
				{
					for (auto const pOwnUnit : UnitClass::Array)
						company += joins(pOwnUnit) ? 1 : 0;

					for (auto const pOwnInf : InfantryClass::Array)
						company += joins(pOwnInf) ? 1 : 0;
				}

				return gates[pTarget] = { defenses, company };
			};

			auto& gateMacro = GetMacroState(pHouse);

			for (auto const& push : pendingPushes)
			{
				auto const [defenses, company] = gateFor(push.Target);
				const int needed = std::max(PushGateMinGroup, PushGatePerDefense * defenses);

				if (endgameRemnantHunt || defenses == 0 || company >= needed)
				{
					if (push.AttackNow)
					{
						push.Foot->ClearMegaMissionData();
						push.Foot->SetTarget(push.Target);
						push.Foot->QueueMission(Mission::Attack, true);
					}
					else if (auto const pTargetCell = push.Target->GetCell())
					{
						OrderFootToAttackMove(push.Foot, pTargetCell);
					}

					continue;
				}

				CellClass* pWait = nullptr;

				if (IsCellInKnownEnemyDefenseReach(pHouse, push.Foot->GetMapCoords(), SiegeScreenMarginCells))
				{
					pWait = DefenseReachWaitCell(pHouse, push.Foot, push.Target->GetMapCoords(), homeCoords);

					if (pWait)
						IssueMoveOrder(push.Foot, pWait, 1.5);
				}

				// TEMP diagnostic: the pushes the gate holds (PushGateLogLines per house and match).
				if (gateMacro.PushGateLogCount < PushGateLogLines)
				{
					gateMacro.PushGateLogCount++;
					Debug::Log("[NeuralAI] Frame %d | House #%d push gate: %s holds from %s at (%d,%d), %d known defenses cover it, %d of %d with it, %d cells away, %s (%d,%d)\n",
						Unsorted::CurrentFrame, pHouse->ArrayIndex, push.Foot->GetTechnoType()->ID, push.Target->Type->ID,
						push.Target->GetMapCoords().X, push.Target->GetMapCoords().Y, defenses, company, needed,
						static_cast<int>(push.Foot->GetMapCoords().DistanceFrom(push.Target->GetMapCoords())),
						pWait ? "steps out to" : "waits at", pWait ? pWait->MapCoords.X : push.Foot->GetMapCoords().X,
						pWait ? pWait->MapCoords.Y : push.Foot->GetMapCoords().Y);
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
			// One a transport operation carries is the operation's.
			if (!pOwnU || pOwnU->Owner != pHouse || pOwnU->InLimbo || !pOwnU->IsAlive || !pOwnU->Type
				|| !pOwnU->Type->DeploysInto || IsInTransportOp(pHouse, pOwnU->UniqueID))
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

			// A deployer on an island given up for lack of room waits there for a transport as well.
			auto const& noRoomForPickup = GetMacroState(pHouse).NoRoomIslandUntil;
			const bool strandedWithoutRoom = noRoomForPickup.contains(uz) && noRoomForPickup.at(uz) > Unsorted::CurrentFrame;
			// A yard MCV of our own tech is the one to ferry only when it may cross to the island goal
			// (yardMcvMayCrossToIslandGoal: an economic crossing only when needed); the transport loader
			// below carries no other yard MCV for it. A stolen MCV (one of a tech we have no yard of) is
			// none: slot 82 takes it home and unfolds it there, and the MCV's own island branch never
			// sends it across (wantsOtherIsland). Taken for the island here, the ferry loader had it board
			// while slot 82 sent it home, every AI tick (match #11992: 3 SMCVs, 120 to 250 turns each 9000
			// frames).
			const bool isForeignTechYardMcv = pUT->DeploysInto->ConstructionYard && pHouse->CountOwnedNow(pUT->DeploysInto) == 0
				&& pHouse->NumConYards >= 1;
			const bool isExpMCV = (pUT->DeploysInto->ConstructionYard && (conYardsInUz >= 1 || hubsInUz >= 2 || strandedWithoutRoom)
				&& !isForeignTechYardMcv && yardMcvMayCrossToIslandGoal(pOwnU));
			// Another deployer only when it expands with its building (deployerExpandsWith, as the deployer
			// logic reads it) and the building fits on the island goal (deployerFitsOnIslandGoal), never
			// one the idle deployer watchdog sent to the main base.
			const bool isExpOutpostOrRef = (!pUT->DeploysInto->ConstructionYard && (hubsInUz >= 1 || strandedWithoutRoom)
				&& (pOwnU == pExtraHomeSlaveMiner
					|| (!pUT->Harvester && !pUT->ResourceGatherer && deployerExpandsWith(pUT->DeploysInto)))
				&& !deployerSentToMainBase(pOwnU->UniqueID) && deployerFitsOnIslandGoal(pOwnU, pUT->DeploysInto));
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
			// One a transport operation carries is the operation's (the old loader sent engineers of
			// operations to its own transports: 33 Enter orders found on them in one match).
			if (!pOwnInf || pOwnInf->Owner != pHouse || pOwnInf->InLimbo || !pOwnInf->IsAlive || !pOwnInf->Type
				|| !pOwnInf->Type->Engineer || IsInTransportOp(pHouse, pOwnInf->UniqueID))
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
	// controller would actually board (idle ground units stranded away from the enemy
	// island, home deployer/engineer). Units already
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

		// The members of the transport operations still waiting on their land (published by
		// UpdateTransportOperations at the end of the last tick).
		if (macro.OpWaitingFrame >= 0 && Unsorted::CurrentFrame - macro.OpWaitingFrame <= 600 && Unsorted::CurrentFrame >= macro.OpWaitingFrame)
		{
			for (int size = 1; size <= 15; ++size)
				macro.FerryDemandSizeBySize[size] += macro.OpWaitingSizeBySize[size];
		}
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

	// A transport takes a passenger in only when it is at rest and the cells the engine has on
	// record for both lie in one zone. A record left behind (transport halted at the shore before
	// the centre of its cell, jumpjet landed) keeps the door shut for every passenger, so it is
	// brought up to date. A passenger sent while the transport still glides to the centre of its
	// cell, or is in the air, walks onto that very cell and the transport never arrives. True when
	// the passenger may ask to load.
	auto const openFerryDoor = [&](FootClass* pTransport, FootClass* pBoarder) -> bool
	{
		const FerryDoorZones doorBefore = GetFerryDoorZones(pTransport, pBoarder);
		const bool atRest = !pTransport->Locomotor->Is_Moving();

		if (doorBefore.Match() && atRest)
			return true;

		const bool transportBehind = pTransport->LastMapCoords != pTransport->GetMapCoords();
		const bool boarderBehind = pBoarder->LastMapCoords != pBoarder->GetMapCoords();
		const bool transportRegistered = RegisterStoppedFootCell(pTransport);
		const bool boarderRegistered = !GetFerryDoorZones(pTransport, pBoarder).Match() && RegisterStoppedFootCell(pBoarder);
		const FerryDoorZones doorAfter = GetFerryDoorZones(pTransport, pBoarder);

		if (transportRegistered || boarderRegistered)
			CountDecision(NeuralDecision::FerryDoorRegistered, pHouse->ArrayIndex);

		// TEMP diagnostic: what kept the door shut and whether registering the cell opened it.
		static int doorLogSerial = -1;
		static int doorLogCount = 0;

		if (doorLogSerial != NeuralMatchSerial)
		{
			doorLogSerial = NeuralMatchSerial;
			doorLogCount = 0;
		}

		int& doorLogFrame = GetMacroState(pHouse).FerryBoardTurns[pTransport->UniqueID].DoorLogFrame;
		const bool doorLogDue = doorLogFrame < 0 || Unsorted::CurrentFrame - doorLogFrame >= FerryBoardTurnFrames;

		if ((transportRegistered || boarderRegistered || doorLogDue) && doorLogCount++ < 60)
		{
			doorLogFrame = Unsorted::CurrentFrame;
			Debug::Log("[NeuralAI] Ferry DIAG house %d frame %d: door of %s %u shut to %s %u, zones %d and %d, last cells behind %d and %d, "
				"transport height %d mission %d moving %d dest %d on %s; registered %d and %d, zones now %d and %d, land type %d, plain zones %d and %d.\n",
				pHouse->ArrayIndex, Unsorted::CurrentFrame, pTransport->GetTechnoType()->ID, pTransport->UniqueID,
				pBoarder->GetTechnoType()->ID, pBoarder->UniqueID, doorBefore.Transport, doorBefore.Boarder,
				transportBehind ? 1 : 0, boarderBehind ? 1 : 0, pTransport->GetHeight(), static_cast<int>(pTransport->CurrentMission),
				pTransport->Locomotor->Is_Moving() ? 1 : 0, pTransport->Destination ? 1 : 0,
				pTransport->GetCell()->LandType == LandType::Water ? "water" : "land",
				transportRegistered ? 1 : 0, boarderRegistered ? 1 : 0, doorAfter.Transport, doorAfter.Boarder,
				static_cast<int>(pTransport->GetCell()->LandType), ObjectZone(pTransport, MovementZone::Normal),
				ObjectZone(pBoarder, MovementZone::Normal));
		}

		return doorAfter.Match() && atRest;
	};

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

		// Its own cell counts too: a unit standing where the transport still has to settle or land
		// keeps it from arriving.
		for (int dy = -1; dy <= 1; ++dy)
		{
			for (int dx = -1; dx <= 1; ++dx)
			{
				const CellStruct adjC {
					static_cast<short>(trCoords.X + dx),
					static_cast<short>(trCoords.Y + dy)
				};
				CollectCellFoots(MapClass::Instance.TryGetCellAt(adjC), adjacentFoots);
			}
		}

		for (auto const pFootAdj : adjacentFoots)
		{
			// The units of a transport operation wait by their own transports and move on its orders alone.
			if (pFootAdj->Owner != pHouse || !pFootAdj->IsAlive || pFootAdj->InLimbo
				|| pFootAdj == pTransport || pFootAdj == pChosenBoarder || IsInTransportOp(pHouse, pFootAdj->UniqueID))
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

		// The turn passes on: a boarder that stands by the transport for FerryBoardTurnFrames
		// without getting aboard (it cannot fit, cannot reach the door, or never lands) would keep
		// every other passenger waiting behind it.
		auto& macroTurns = GetMacroState(pHouse);
		auto& turn = macroTurns.FerryBoardTurns[pTransport->UniqueID];
		auto const endTurn = [&turn]()
		{
			turn.Boarder = 0;
			turn.NearSince = -1;
		};

		// The transport has a single radio link. Left with a unit that is no longer boarding (called
		// away to fight, sent elsewhere) it shuts every other passenger out.
		if (auto const pLinked = abstract_cast<FootClass*>(pTransport->GetNthLink(0));
			pLinked && pLinked != pChosenBoarder && pLinked->Owner == pHouse && !pLinked->InLimbo
			&& pLinked->CurrentMission != Mission::Enter && pLinked->QueuedMission != Mission::Enter
			&& !pLinked->IsTether && !pTransport->IsTether)
		{
			auto const pTransportUnit = abstract_cast<UnitClass*>(pTransport);
			auto const pCarryState = pTransportUnit ? UnitExt::Fetch(pTransportUnit) : nullptr;

			if (!pCarryState || (!pCarryState->CarryallCargo && pCarryState->CarryallState == JumpjetCarryallState::Idle))
			{
				pTransport->SendCommand(RadioCommand::NotifyUnlink, pLinked);
				CountDecision(NeuralDecision::FerryStrayLinkCut, pHouse->ArrayIndex);

				// Whatever took it away keeps it: asked to board again it would link, leave and
				// shut the door once more, so no transport takes it for a while.
				auto& strayMacro = GetMacroState(pHouse);
				strayMacro.FerryBlockedUntil[pLinked->UniqueID] = Unsorted::CurrentFrame + FerryBoardFailedFrames;
			}
		}

		// With the door shut every request to load only sends the transport towards the boarder, so
		// the boarder lets go until the door opens. Aircraft answer a request to load in their own way.
		if (pChosenBoarder && pTransport->WhatAmI() == AbstractType::Unit && !openFerryDoor(pTransport, pChosenBoarder))
		{
			if (pChosenBoarder->ContainsLink(pTransport))
				pChosenBoarder->SendCommand(RadioCommand::NotifyUnlink, pTransport);

			pChosenBoarder->SetArchiveTarget(nullptr);
			pChosenBoarder->SetDestination(nullptr, true);
			pChosenBoarder->ClearNavigationList();
			pChosenBoarder->ForceMission(Mission::Guard);
			endTurn();

			return nullptr;
		}

		if (!pChosenBoarder || pChosenBoarder->GetMapCoords().DistanceFrom(trCoords) > FerryBoardTurnCells)
		{
			endTurn();
			return pChosenBoarder;
		}

		if (turn.Boarder != pChosenBoarder->UniqueID || turn.NearSince < 0)
		{
			turn.Boarder = pChosenBoarder->UniqueID;
			turn.NearSince = Unsorted::CurrentFrame;
			return pChosenBoarder;
		}

		if (Unsorted::CurrentFrame - turn.NearSince < FerryBoardTurnFrames)
			return pChosenBoarder;

		if (pChosenBoarder->ContainsLink(pTransport))
			pChosenBoarder->SendCommand(RadioCommand::NotifyUnlink, pTransport);

		pChosenBoarder->SetArchiveTarget(nullptr);
		pChosenBoarder->SetDestination(nullptr, true);
		pChosenBoarder->ClearNavigationList();
		pChosenBoarder->ForceMission(Mission::Guard);
		macroTurns.FerryBlockedUntil[pChosenBoarder->UniqueID] = Unsorted::CurrentFrame + FerryBoardFailedFrames;
		endTurn();

		return nullptr;
	};

	// Where a transport stands to load: see MoveTransportToLoadingCell.
	// Airborne Front boards through the same routine. Its transport is a plan member, which the
	// ferry loader leaves alone, so nothing kept its single radio link free, the cells at its door
	// clear or its boarder to one turn: about half of its plans ended with nobody aboard (focus
	// batches of 2026-11-13 and 14), whatever the plan did about the order itself.
	{
		auto const& airborne = GetMacroState(pHouse).Plans[static_cast<size_t>(NeuralPlanId::AirborneFront)];

		if (airborne.Active && airborne.AuxBaseline == 0 && !airborne.Members.empty())
		{
			FootClass* pCarrier = nullptr;

			for (auto const pUnit : UnitClass::Array)
			{
				if (pUnit && pUnit->UniqueID == airborne.Members[0] && pUnit->Owner == pHouse && pUnit->IsAlive && !pUnit->InLimbo)
				{
					pCarrier = pUnit;
					break;
				}
			}

			if (pCarrier)
				enforceSingleActiveBoarderOn(pCarrier, -1);
		}
	}

	auto const moveToLoadingCell = [&](UnitClass* pTransport, const CellStruct& from, int landZone, bool passengersWaiting) -> bool
	{
		return MoveTransportToLoadingCell(pHouse, pTransport, from, landZone, passengersWaiting);
	};

	// A unit that may board a ferry: it needs one to cross water, no transport gave up on it lately
	// and no plan has it.
	auto const canRideFerry = [&](const FootClass* pFoot) -> bool
	{
		return !CanFootCrossWaterDirectly(pFoot) && !IsFerryBlocked(pHouse, pFoot->UniqueID) && !IsPlanMember(pHouse, pFoot->UniqueID);
	};

	bool deployerTransportAssigned = false;
	bool engineerTransportAssigned = false;

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

	// Cargo with its own crossing rules: an engineer, a harvester, a base deployer. Its transport sails
	// as soon as it is aboard, so it never shares a transport with the combat load.
	auto const isOwnRulesCargo = [](FootClass* pFoot) -> bool
	{
		auto const pCargoUnit = abstract_cast<UnitClass*>(pFoot);
		auto const pCargoInf = abstract_cast<InfantryClass*>(pFoot);

		return (pCargoUnit && pCargoUnit->Type && (pCargoUnit->Type->Harvester || pCargoUnit->Type->Weeder
				|| (pCargoUnit->Type->DeploysInto && !IsDeployableSiegeUnitType(pCargoUnit->Type))))
			|| (pCargoInf && pCargoInf->Type && pCargoInf->Type->Engineer);
	};

	// Fleet split (after the distribution mode of the Phobos click actions, pull request #1993: the
	// selected units are dealt out over every transport in reach, largest first, one round at a
	// time). On each pickup shore the whole load (the passengers aboard and every unit there that
	// could still board) is weighed against as few parked transports as can hold it, those already
	// carrying first, then the nearest to the units: what is still on land and the room on its way
	// hold the loaded transports for one joint departure. A transport with a base deployer, a
	// harvester or an engineer aboard keeps its own rules.
	// By pickup zone: the size of the battalion still on land (boardable, nobody aboard yet), and
	// the room of our empty transports on their way to that zone.
	std::map<int, int> ferryBattalionLeft;
	std::map<int, int> ferryComingRoom;
	{
		struct FleetTransport
		{
			UnitClass* Transport;
			int Held;
			int Room;
		};

		std::map<int, std::vector<FleetTransport>> fleets;

		for (auto const pFleet : UnitClass::Array)
		{
			if (!pFleet || pFleet->Owner != pHouse || pFleet->InLimbo || !pFleet->IsAlive || !pFleet->Type
				|| !IsAmphibiousOrAirTransport(pFleet->Type) || pFleet->Type->Passengers <= 0 || IsPlanMember(pHouse, pFleet->UniqueID)
				|| IsInTransportOp(pHouse, pFleet->UniqueID))
			{
				continue;
			}

			auto const pFleetCell = pFleet->GetCell();
			auto const pFleetTrip = FindFerryTrip(pFleet->UniqueID);

			if (!pFleetCell || pFleetCell->LandType == LandType::Water || (pFleetTrip && pFleetTrip->Departed))
				continue;

			bool ownRules = false;

			for (FootClass* pPass = pFleet->Passengers.GetFirstPassenger(); pPass != nullptr && !ownRules;
				pPass = abstract_cast<FootClass*>(pPass->NextObject))
			{
				ownRules = isOwnRulesCargo(pPass);
			}

			const int fleetZone = ObjectZone(pFleet, MovementZone::Normal);

			if (ownRules || fleetZone < 0)
				continue;

			const int held = pFleet->Passengers.GetTotalSize();
			fleets[fleetZone].push_back({ pFleet, held, pFleet->Type->Passengers });

			// The whole battalion is ferry demand for the production selector, its passengers aboard too:
			// with only the units still free on land counted, two full transports and 23 units waiting
			// read as "two transports are enough" and no third one was built.
			auto& demandMacro = GetMacroState(pHouse);

			for (FootClass* pPass = pFleet->Passengers.GetFirstPassenger(); pPass != nullptr; pPass = abstract_cast<FootClass*>(pPass->NextObject))
			{
				const int passSize = std::clamp(static_cast<int>(pPass->GetTechnoType()->Size), 1, 15);
				demandMacro.FerryDemandSizeBySize[passSize] += passSize;
			}
		}

		// The room of a transport our factories are building: a battalion bigger than the transports
		// parked on its shore waits for it as well (user: call the transports the group needs, or have
		// them built when none is free).
		int productionRoom = 0;

		for (const bool naval : { false, true })
		{
			if (auto const pFactory = pHouse->GetPrimaryFactory(AbstractType::UnitType, naval, BuildCat::DontCare))
			{
				auto const pBuilt = pFactory->Object ? pFactory->Object->GetTechnoType() : nullptr;

				if (pBuilt && IsAmphibiousOrAirTransport(pBuilt) && pBuilt->Passengers > 0)
					productionRoom += pBuilt->Passengers;
			}
		}

		// Empty transports on their way to a pickup zone (on the water or elsewhere, heading for a cell
		// of that zone): the battalion waits for them.
		for (auto const pComing : UnitClass::Array)
		{
			if (!pComing || pComing->Owner != pHouse || pComing->InLimbo || !pComing->IsAlive || !pComing->Type
				|| !IsAmphibiousOrAirTransport(pComing->Type) || pComing->Type->Passengers <= 0 || pComing->Passengers.NumPassengers > 0
				|| IsPlanMember(pHouse, pComing->UniqueID))
			{
				continue;
			}

			auto const pComingDest = abstract_cast<CellClass*>(pComing->Destination);
			auto const pComingCell = pComing->GetCell();

			if (!pComingDest || !pComingCell)
				continue;

			const int destZone = MapClass::Instance.GetMovementZoneType(pComingDest->MapCoords, MovementZone::Normal, false);

			if (destZone >= 0 && (pComingCell->LandType == LandType::Water || ObjectZone(pComing, MovementZone::Normal) != destZone))
				ferryComingRoom[destZone] += pComing->Type->Passengers;
		}

		for (auto& [fleetZone, fleet] : fleets)
		{
			int sizeLimit = 0;
			int load = 0;

			for (auto const& member : fleet)
			{
				sizeLimit = std::max(sizeLimit, static_cast<int>(member.Transport->Type->SizeLimit));
				load += member.Held;
			}

			const int heldTotal = load;

			std::unordered_set<DWORD> counted;
			int sumX = 0;
			int sumY = 0;
			int boardable = 0;

			auto const addBoardable = [&](FootClass* pFoot, int size)
			{
				if (!pFoot || !pFoot->IsAlive || pFoot->InLimbo || size > sizeLimit || counted.contains(pFoot->UniqueID)
					|| pFoot->CurrentMission == Mission::Enter || pFoot->QueuedMission == Mission::Enter
					|| !canRideFerry(pFoot) || isOwnRulesCargo(pFoot))
				{
					return;
				}

				const int footZone = ObjectZone(pFoot, MovementZone::Normal);

				if (footZone != fleetZone || IsFootActivelyAttackingOrMovingToEnemy(pFoot) || IsFootNearEnemyFrontline(pFoot, footZone))
					return;

				counted.insert(pFoot->UniqueID);
				load += size;
				sumX += pFoot->GetMapCoords().X;
				sumY += pFoot->GetMapCoords().Y;
				++boardable;
			};

			for (auto const pV : idleCombatUnits)
			{
				if (pV && pV->Type && !IsAmphibiousOrAirTransport(pV->Type) && !(pV->CaptureManager && pV->CaptureManager->NumControlNodes() > 0))
					addBoardable(pV, std::max(1, static_cast<int>(pV->Type->TechnoTypeClass::Size)));
			}

			for (auto const pI : idleCombatInf)
			{
				if (pI && pI->Type)
					addBoardable(pI, std::max(1, static_cast<int>(pI->Type->TechnoTypeClass::Size)));
			}

			if (load <= 0)
				continue;

			ferryBattalionLeft[fleetZone] = load - heldTotal;
			int fleetRoom = 0;

			for (auto const& member : fleet)
				fleetRoom += member.Room;

			if (productionRoom > 0 && load > fleetRoom)
				ferryComingRoom[fleetZone] += productionRoom;

			const CellStruct centre = boardable > 0
				? CellStruct { static_cast<short>(sumX / boardable), static_cast<short>(sumY / boardable) }
				: fleet.front().Transport->GetMapCoords();

			std::sort(fleet.begin(), fleet.end(), [&centre](const FleetTransport& a, const FleetTransport& b)
				{
					if ((a.Held > 0) != (b.Held > 0))
						return a.Held > 0;

					const double distA = a.Transport->GetMapCoords().DistanceFrom(centre);
					const double distB = b.Transport->GetMapCoords().DistanceFrom(centre);

					if (distA != distB)
						return distA < distB;

					return a.Transport->UniqueID < b.Transport->UniqueID;
				});

			size_t chosen = 0;
			int room = 0;

			while (chosen < fleet.size() && room < load)
			{
				room += fleet[chosen].Room;
				++chosen;
			}

			const int share = (load + static_cast<int>(chosen) - 1) / static_cast<int>(chosen);

			auto& fleetMacro = GetMacroState(pHouse);
			const int now = Unsorted::CurrentFrame;

			if (chosen >= 2 && (fleetMacro.FerryFleetLogFrame < 0 || now < fleetMacro.FerryFleetLogFrame
				|| now - fleetMacro.FerryFleetLogFrame >= FerryIslandKeptLogFrames))
			{
				fleetMacro.FerryFleetLogFrame = now;
				auto const itComing = ferryComingRoom.find(fleetZone);
				Debug::Log("[NeuralAI] Frame %d | House #%d ferry fleet split on zone %d: load %d over %d of %d transports, share %d each, "
					"%d still on land, room %d on its way\n", now, pHouse->ArrayIndex, fleetZone, load, static_cast<int>(chosen),
					static_cast<int>(fleet.size()), share, load - heldTotal, itComing != ferryComingRoom.end() ? itComing->second : 0);
			}
		}
	}

	for (auto const pTrans : UnitClass::Array)
	{
		// Transports flying a strategic plan (e.g. the airborne second front) belong to that plan, those of
		// a transport operation to the operation.
		if (!pTrans || pTrans->Owner != pHouse || pTrans->InLimbo || !pTrans->IsAlive || !pTrans->Type
			|| IsPlanMember(pHouse, pTrans->UniqueID) || IsInTransportOp(pHouse, pTrans->UniqueID))
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
							NoteOrderCaller(pTrans, "move");
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
					if (!pCarryExt->CarryallCargo)
						GetMacroState(pHouse).CarryallDropSince.erase(pTrans->UniqueID);

					if (pCarryExt->CarryallCargo && pCarryExt->CarryallCargo->IsAlive)
					{
						auto const pCarriedUnit = pCarryExt->CarryallCargo;
						CellClass* pDropCell = nullptr;

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
							else if (pCarriedUnit->Type->Harvester || pCarriedUnit->Type->Weeder)
							{
								// Where it can unload: the island goal with a refinery of ours, else the nearest refinery.
								pDropCell = islandGoalTakesHarvesters() ? pIslandResourceGoalCell : nearestOwnRefineryCell(pTrans->GetMapCoords());
							}
							else if (pCarriedUnit->Type->DeploysInto != nullptr && pIslandResourceGoalCell)
							{
								pDropCell = pIslandResourceGoalCell;
							}
						}

						if (!pDropCell && !(pCarriedUnit->Type && (pCarriedUnit->Type->Harvester || pCarriedUnit->Type->Weeder)))
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

							// The cell the cargo is set down on: the one below when it takes it, else the
							// nearest one around the drop cell that does (a building, another unit or a
							// cliff below kept a carryall hanging over its drop zone with a harvester for
							// 65000 frames, match #8867). The choice is kept while it stays good. While it
							// flies to that cell any cell below that takes the cargo will do (a carryall
							// circles its cell without stopping over it), and the search reaches one ring
							// farther every CarryallDropStuckFrames it has gone without setting it down.
							auto& dropCells = GetMacroState(pHouse).CarryallDropCells;
							auto& dropSince = GetMacroState(pHouse).CarryallDropSince;
							auto const itChosen = dropCells.find(pTrans->UniqueID);
							auto const itSince = dropSince.find(pTrans->UniqueID);
							CellClass* pChosen = itChosen != dropCells.end() ? MapClass::Instance.TryGetCellAt(itChosen->second) : nullptr;
							const int stuckFrames = itSince != dropSince.end() ? Unsorted::CurrentFrame - itSince->second : 0;
							const int dropSearchCells = CarryallDropSearchCells
								+ CarryallDropRingCells * std::min(CarryallDropMaxRings, stuckFrames / CarryallDropStuckFrames);

							if (pChosen && (!pCarryExt->CarryallCanDropAt(pChosen)
								|| pChosen->MapCoords.DistanceFrom(pDropCell->MapCoords) > dropSearchCells + CarryallDropZoneCells))
							{
								dropCells.erase(pTrans->UniqueID);
								pChosen = nullptr;
							}

							const double cellsFromDrop = pTrans->GetMapCoords().DistanceFrom(pDropCell->MapCoords);
							const bool inDropZone = cellsFromDrop <= CarryallDropZoneCells;
							const bool nearChosen = pChosen && cellsFromDrop <= dropSearchCells + CarryallDropZoneCells;

							if ((inDropZone || nearChosen) && pCarryExt->CarryallCanDropAt(pTrans->GetCell()))
							{
								if (pTrans->CurrentMission != Mission::Unload)
									CountDecision(NeuralDecision::AirliftDrop, pHouse->ArrayIndex);

								dropCells.erase(pTrans->UniqueID);
								dropSince.erase(pTrans->UniqueID);
								pTrans->QueueMission(Mission::Unload, true);
							}
							else if (pChosen)
							{
								if (pTrans->Destination != pChosen || pTrans->CurrentMission != Mission::Move)
								{
									pTrans->SetDestination(pChosen, true);
									pTrans->QueueMission(Mission::Move, true);
									NoteOrderCaller(pTrans, "move");
								}
							}
							else if (inDropZone)
							{
								CellClass* pBest = nullptr;
								double bestDist = 999999.0;
								const CellStruct centre = pDropCell->MapCoords;

								dropSince.try_emplace(pTrans->UniqueID, Unsorted::CurrentFrame);

								for (int dy = -dropSearchCells; dy <= dropSearchCells; ++dy)
								{
									for (int dx = -dropSearchCells; dx <= dropSearchCells; ++dx)
									{
										const CellStruct cell { static_cast<short>(centre.X + dx), static_cast<short>(centre.Y + dy) };
										auto const pCell = MapClass::Instance.TryGetCellAt(cell);
										const double dist = cell.DistanceFrom(centre);

										if (pCell && dist < bestDist && pCarryExt->CarryallCanDropAt(pCell))
										{
											bestDist = dist;
											pBest = pCell;
										}
									}
								}

								if (pBest)
								{
									dropCells[pTrans->UniqueID] = pBest->MapCoords;
									pTrans->SetDestination(pBest, true);
									pTrans->QueueMission(Mission::Move, true);
									NoteOrderCaller(pTrans, "move");
									Debug::Log("[NeuralAI] House %d airlift: %s %u cannot set %s down at (%d,%d), goes to (%d,%d), frame %d.\n",
										pHouse->ArrayIndex, pTrans->Type->ID, pTrans->UniqueID, pCarriedUnit->Type->ID, pTrans->GetMapCoords().X,
										pTrans->GetMapCoords().Y, pBest->MapCoords.X, pBest->MapCoords.Y, Unsorted::CurrentFrame);
								}
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
								AdvanceRoutedPickup(pHouse, pTrans, pPendingCargo);
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

						const bool islandLiftsPaused
							= Unsorted::CurrentFrame - GetMacroState(pHouse).LastAirDropAbortFrame < 1800;
						const bool evacuationAllowed = pRepairDepot && SlotGateHeld(scores[54], -200, 54, pTrans->UniqueID);

						for (auto const pU : UnitClass::Array)
						{
							if (pickedCarryallTarget)
								break;

							// A unit of a transport operation waits for the operation's own transports.
							if (!pU || pU->Owner != pHouse || pU->InLimbo || !pU->IsAlive || !pU->Type
								|| !pCarryExt->CarryallCanLift(pU) || IsInTransportOp(pHouse, pU->UniqueID))
							{
								continue;
							}

							const int uZone = ObjectZone(pU, MovementZone::Normal);
							const bool isCriticallyDamagedTank
								= (pRepairDepot && pU->Type->Cost >= 1000 && pU->GetHealthPercentage() < 0.40);
							const bool isLoadedStrikeAPC
								= (pU->Type->Passengers >= 3 && pU->Passengers.NumPassengers >= 1
									&& (pWaveTargetCell || pIslandGoalCell || pIslandTechGoalCell));
							// A yard MCV of our own tech only when it may cross (yardMcvMayCrossToIslandGoal; a stolen one never: slot 82),
							// another deployer only when it expands with its building (deployerExpandsWith) and fits there.
							const bool isIslandExpansionMCV = (pU->Type->DeploysInto != nullptr
								&& pIslandResourceGoalCell && uZone >= 0 && uZone != resourceIslandZone
								&& (pU == pHomeExpansionDeployer || pHouse->NumConYards >= 1
									|| pHouse->CountOwnedNow(pU->Type) >= 2 || pHouse->OwnedBuildings >= 6)
								&& (pU->Type->DeploysInto->ConstructionYard
									? (!(pHouse->CountOwnedNow(pU->Type->DeploysInto) == 0 && pHouse->NumConYards >= 1)
										&& (pU == pHomeExpansionDeployer || yardMcvMayCrossToIslandGoal(pU)))
									: ((pU == pExtraHomeSlaveMiner || deployerExpandsWith(pU->Type->DeploysInto)) && !deployerSentToMainBase(pU->UniqueID)
										&& deployerFitsOnIslandGoal(pU, pU->Type->DeploysInto))));
							const bool isIslandHarvester
								= ((pU->Type->Harvester || pU->Type->Weeder) && islandGoalTakesHarvesters()
									&& uZone == homeGroundZone && uZone != resourceIslandZone && pHouse->CountOwnedNow(pU->Type) >= 2);
							const bool strandedHarvester = isStrandedHarvester(pU, uZone);
							const bool isIslandCombatVehicle = (pIslandGoalCell != nullptr && uZone == homeGroundZone
								&& !IsAmphibiousOrAirTransport(pU->Type) && !pU->Type->Harvester && !pU->Type->Weeder
								&& pU->Type->DeploysInto == nullptr
								&& (pU->CurrentMission == Mission::Guard || pU->CurrentMission == Mission::Area_Guard)
								&& !IsFootActivelyAttackingOrMovingToEnemy(pU) && !IsFootNearEnemyFrontline(pU, uZone));
							// Combat evacuation: an expensive unit losing its fight is pulled out early.
							const bool isLosingExpensiveUnit = evacuationAllowed && !isCriticallyDamagedTank
								&& pU->Type->Cost >= 1000 && pU->GetHealthPercentage() < 0.70 && IsFootOutmatched(pHouse, pU, 7.0);
							const bool isIslandJob = isIslandExpansionMCV || isIslandHarvester || isIslandCombatVehicle;
							if (isCriticallyDamagedTank || isLosingExpensiveUnit || isLoadedStrikeAPC || strandedHarvester
								|| (isIslandJob && !islandLiftsPaused))
							{
								AdvanceRoutedPickup(pHouse, pTrans, pU);
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

			// On a repair trip the trip leads it (its time cap, its give-up, the stations that failed it).
			if (IsOnRepairTrip(pHouse, pTrans->UniqueID))
				continue;

			// It goes for repair as a repair trip, with the credits to pay for it (TransportRepairMinCredits).
			if (!underRepair && transHP < 0.5 && pHouse->Available_Money() >= TransportRepairMinCredits
				&& !IsRepairStationFailed(pHouse, pTrans->GetTechnoType(), NeuralRepairKind::Depot))
			{
				if (auto const pStation = FindBestRepairStation(pHouse, pTrans))
				{
					ClearFerryTrip(pTrans->UniqueID);
					OrderFootToEnter(pTrans, pStation);
					BeginRepairTrip(pHouse, pTrans, NeuralRepairKind::Depot, pStation->UniqueID, CellStruct::Empty);
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
				// A siege unit (Tick Tank, artillery) also unfolds into a building, but it is combat cargo:
				// it waits for a full load like any tank instead of crossing alone as an MCV does.
				if (pPassUnit->Type && pPassUnit->Type->DeploysInto != nullptr && !IsDeployableSiegeUnitType(pPassUnit->Type))
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

		// A yard MCV among all the passengers, and the yard it unfolds into (an MCV aboard with nowhere
		// to unfold, below).
		BuildingTypeClass* pCarriedYardType = nullptr;

		for (FootClass* pPass = pTrans->Passengers.GetFirstPassenger(); pPass != nullptr && !pCarriedYardType;
			pPass = abstract_cast<FootClass*>(pPass->NextObject))
		{
			if (auto const pPassUnit = abstract_cast<UnitClass*>(pPass);
				pPassUnit && pPassUnit->Type && pPassUnit->Type->DeploysInto && pPassUnit->Type->DeploysInto->ConstructionYard)
			{
				pCarriedYardType = pPassUnit->Type->DeploysInto;
			}
		}

		const bool carryingYardMcv = pCarriedYardType != nullptr;

		// Whatever the loader orders from here on is its own doing.
		GetMacroState(pHouse).ParkedFerries.erase(pTrans->UniqueID);

		const CellStruct transCoords = pTrans->GetMapCoords();
		// The land around a parked transport, not the shore cell's own zone (see TransportLandZone).
		const int transZone = TransportLandZone(transCoords, pTrans->OnBridge);

		// Slot 82 (Island Extraction of Stolen Foreign MCV to Safe Home Island):
		// If this transport is carrying a stolen foreign-faction MCV from an
		// enemy island, sail straight back to our safe Home Island (homeCoords /
		// homeGroundZone) and unload it there so it can deploy safely!
		if (carryingStolenForeignMCV && homeCoords != CellStruct::Empty)
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
		if (numPass == 0 && pStrandedStolenMCVOnIsland)
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

		// TEMP diagnostic: what a transport leaves its shore with and the FerrySail flags of the last
		// decision there (0 when it left without one, -1 for a loaded transport found at sea with no
		// trip).
		auto const logFerryDeparture = [&](int reason)
		{
			static int departLogSerial = -1;
			static int departLogCount = 0;

			if (departLogSerial != NeuralMatchSerial)
			{
				departLogSerial = NeuralMatchSerial;
				departLogCount = 0;
			}

			if (departLogCount++ >= 80)
				return;

			char cargo[96] = "";
			size_t used = 0;

			for (FootClass* pPass = pTrans->Passengers.GetFirstPassenger(); pPass != nullptr && used + 26 < sizeof(cargo);
				pPass = abstract_cast<FootClass*>(pPass->NextObject))
			{
				used += static_cast<size_t>(sprintf_s(cargo + used, sizeof(cargo) - used, "%s%s", used ? "," : "", pPass->GetTechnoType()->ID));
			}

			Debug::Log("[NeuralAI] Ferry departure house %d frame %d: %s %u leaves zone %d with %d passengers, size %d/%d (%s), reason %d, "
				"on water %d, zone now %d.\n",
				pHouse->ArrayIndex, Unsorted::CurrentFrame, pTrans->Type->ID, pTrans->UniqueID, pTrip ? pTrip->LoadZone : -1, numPass,
				totalPassSize, pTrans->Type->Passengers, cargo, reason, tripOnWater ? 1 : 0, transZone);
		};

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
				logFerryDeparture(-1);
			}

			if (pTrip)
			{
				pTrip->Frame = Unsorted::CurrentFrame;
				pTrip->HouseIndex = pHouse->ArrayIndex;
				pTrip->LastCoords = transCoords;

				// Under fire now: the health dropped since the last tick.
				if (pTrip->LastHealth >= 0 && pTrans->Health < pTrip->LastHealth)
					pTrip->HitFrame = Unsorted::CurrentFrame;

				pTrip->LastHealth = pTrans->Health;

				// Off its shore means on water or on other ground: a hovercraft that drove over a cell of
				// no ground (zone 1) inside its own base counted as departed and was never set down again.
				if (!pTrip->Departed && pTrip->LoadZone >= 0
					&& (tripOnWater || (IsGroundZone(transZone) && transZone != pTrip->LoadZone)))
				{
					pTrip->Departed = true;
					CountDecision(NeuralDecision::FerryDeparted);
					logFerryDeparture(pTrip->SailReason);
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
			return IsGroundZone(zone) && zone != ferryOriginZone;
		};

		// A goal is one only on ground this very transport lands on from where it is
		// (CanTransportTypeLink): hovercraft "ready" for a plateau they could not climb kept their
		// MCVs in the hold for the rest of the match (#10267). With no goal it reaches, the parked
		// loader takes nobody and the no-goal set-down frees the cargo.
		auto isReachableFerryGoal = [&](const CellClass* pGoal, int zone) -> bool
		{
			return pGoal && isValidFerryGoalZone(zone) && CanTransportTypeLink(pTrans->Type, transCoords, pGoal->MapCoords);
		};

		bool carryingEngineer = false;
		bool carryingHarvester = false;
		for (FootClass* pPass = pTrans->Passengers.GetFirstPassenger(); pPass != nullptr;
			pPass = abstract_cast<FootClass*>(pPass->NextObject))
		{
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

		CellClass* pActiveIslandGoal = pIslandGoalCell;
		int activeIslandZone = targetIslandZone;
		// Parallel amphibious task groups: with FerrySplitFrontTransports combat transports ready and a
		// secondary enemy island, the odd-numbered ones strike it. Fewer stay one convoy: two
		// transports split over two islands landed a handful of units on each (the larger group wins).
		if (!carryingMCV && !carryingHarvester && !carryingEngineer && pSecondaryIslandGoalCell
			&& secondaryIslandZone >= 0 && readyCombatConvoyTransports >= FerrySplitFrontTransports && (pTrans->UniqueID % 2 == 1))
		{
			pActiveIslandGoal = pSecondaryIslandGoalCell;
			activeIslandZone = secondaryIslandZone;
		}

		if ((carryingMCV || (carryingHarvester && islandGoalTakesHarvesters())) && pIslandResourceGoalCell)
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

		// A goal on the origin island, or on ground this transport does not reach, is no destination
		// at all: fall back to the next objective it lands on, or to none.
		if (pActiveIslandGoal && !isReachableFerryGoal(pActiveIslandGoal, activeIslandZone))
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
				if (isReachableFerryGoal(pGoalCell, goalZone))
				{
					pActiveIslandGoal = pGoalCell;
					activeIslandZone = goalZone;
					break;
				}
			}
		}

		// A transport with passengers aboard that is under fire now leaves with what it has. The
		// engine's HasBeenAttacked flag is never cleared and a damaged transport is only repaired
		// empty, so neither of them means now: only a recent hit does.
		const bool emergencyDepartUnderFire = numPass >= 1 && pTrip
			&& Unsorted::CurrentFrame - pTrip->HitFrame < FerryUnderFireFrames;

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

		// Island commitment: a loaded transport that has left its shore keeps the island it sailed for
		// while that island is still one of our goals or, for a combat or engineer cargo, still holds
		// the enemy. The goals are scored again every tick (defenses coming into sight move the best
		// enemy island, the odd transports take the secondary island only while two or more are
		// ready) and the landing commitment went with the island: transports turned away at the
		// enemy's coast, came back and sailed to and fro before going elsewhere (user, island match
		// on Fluddle, hover transports at his shore).
		if (pTrip && numPass == 0)
		{
			pTrip->CommittedZone = -1;
			pTrip->CommittedGoal = CellStruct::Empty;
			pTrip->LastLanding = CellStruct::Empty;
		}
		else if (pTrip && tripDeparted)
		{
			const int keptZone = pTrip->CommittedZone;
			CellClass* pKeptGoal = nullptr;

			// The island's goal cell when it is still one of our goals, else the cell it sailed for.
			const std::pair<CellClass*, int> goalsNow[] = {
				{ pIslandGoalCell, targetIslandZone },
				{ pSecondaryIslandGoalCell, secondaryIslandZone },
				{ pIslandResourceGoalCell, resourceIslandZone },
				{ pIslandTechGoalCell, techIslandZone }
			};

			for (auto const& [pGoalCell, goalZone] : goalsNow)
			{
				if (pGoalCell && keptZone >= 0 && goalZone == keptZone)
				{
					pKeptGoal = pGoalCell;
					break;
				}
			}

			// At its landing (committed, or within FerryArrivalKeepCells of the last one picked) the
			// transport finishes the trip whatever the goals say: an island often stopped being a goal
			// the moment our transport reached it and became one again once it sailed off (20 of 34
			// islands given up with a known distance were within 5 cells of the landing, 8 matches).
			const bool arriving = pTrip->CommittedLanding != CellStruct::Empty
				|| (pTrip->LastLanding != CellStruct::Empty && transCoords.DistanceFrom(pTrip->LastLanding) <= FerryArrivalKeepCells);

			if (!pKeptGoal && keptZone >= 0 && pTrip->CommittedGoal != CellStruct::Empty
				&& (arriving || (!carryingMCV && !carryingHarvester && hasEnemyPresenceInIslandZone(keptZone))))
			{
				pKeptGoal = MapClass::Instance.TryGetCellAt(pTrip->CommittedGoal);
			}

			if (isReachableFerryGoal(pKeptGoal, keptZone))
			{
				if (activeIslandZone != keptZone)
				{
					auto& macro = GetMacroState(pHouse);
					const int now = Unsorted::CurrentFrame;
					++macro.FerryIslandKept;

					if (macro.FerryIslandKeptLogFrame < 0 || now < macro.FerryIslandKeptLogFrame
						|| now - macro.FerryIslandKeptLogFrame >= FerryIslandKeptLogFrames)
					{
						Debug::Log("[NeuralAI] Frame %d | House #%d ferry island commitment: %d ticks a loaded transport kept the island "
							"it sailed for while the goals moved (since frame %d), last %s %u on zone %d over zone %d at (%d,%d)\n",
							now, pHouse->ArrayIndex, macro.FerryIslandKept, std::max(0, macro.FerryIslandKeptLogFrame), pTrans->Type->ID,
							pTrans->UniqueID, keptZone, activeIslandZone, transCoords.X, transCoords.Y);
						macro.FerryIslandKeptLogFrame = now;
						macro.FerryIslandKept = 0;
					}
				}

				pActiveIslandGoal = pKeptGoal;
				activeIslandZone = keptZone;
				pTrip->CommittedGoal = pKeptGoal->MapCoords;
			}
			else if (isReachableFerryGoal(pActiveIslandGoal, activeIslandZone))
			{
				if (keptZone >= 0 && keptZone != activeIslandZone)
				{
					Debug::Log("[NeuralAI] Frame %d | House #%d ferry: %s %u with %d passengers leaves zone %d (no goal or enemy of "
						"ours there now) for zone %d at (%d,%d)\n", Unsorted::CurrentFrame, pHouse->ArrayIndex, pTrans->Type->ID,
						pTrans->UniqueID, numPass, keptZone, activeIslandZone, transCoords.X, transCoords.Y);
				}

				pTrip->CommittedZone = activeIslandZone;
				pTrip->CommittedGoal = pActiveIslandGoal->MapCoords;
			}
		}

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

		auto const pCurTransCell = pTrans->GetCell();
		// A loaded transport that already left its loading island is in transit:
		// brushing a friendly coast on the way never turns it back into a pickup.
		const bool onSafePickupShore = !tripDeparted && isSafeFriendlyPickupIsland(transZone) && pCurTransCell
			&& pCurTransCell->LandType != LandType::Water;

		// Enforce strict 1-by-1 boarding while parked on a pickup island so units never jam!
		FootClass* pSingleActiveBoarder = onSafePickupShore ? enforceSingleActiveBoarderOn(pTrans, transZone) : nullptr;

		const bool canFitMoreOnPickupIsland = onSafePickupShore && pSingleActiveBoarder != nullptr;

		// Ferry diagnostics, every 900 frames: each transport's state against the demand around it.
		if (IsAITickWindow(900, 0))
		{
			Debug::Log("[NeuralAI] Ferry diag house %d frame %d: %s %u at (%d,%d) zone %d cell %s mission %d pass %d/%d departed %d "
				"pickupShore %d boarder %u canFitMore %d, idle units %d inf %d\n",
				pHouse->ArrayIndex, Unsorted::CurrentFrame, pTrans->Type->ID, pTrans->UniqueID, pTrans->GetMapCoords().X,
				pTrans->GetMapCoords().Y, transZone, (pCurTransCell && pCurTransCell->LandType == LandType::Water) ? "water" : "land",
				static_cast<int>(pTrans->CurrentMission), numPass, pTrans->Type->Passengers, tripDeparted ? 1 : 0,
				onSafePickupShore ? 1 : 0, pSingleActiveBoarder ? pSingleActiveBoarder->UniqueID : 0u, canFitMoreOnPickupIsland ? 1 : 0,
				static_cast<int>(idleCombatUnits.size()), static_cast<int>(idleCombatInf.size()));
		}

		// Arrival by touch (TransportTouchesLandZone): the goal island counts as reached when the land
		// cell under the transport or one next to it is of its zone.
		const bool hasCargoInTransitOrUnloading = (numPass >= 1) && !onSafePickupShore
			&& (tripDeparted
				|| (activeIslandZone >= 0 && (transZone == activeIslandZone || TransportTouchesLandZone(transCoords, activeIslandZone)))
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

			if (Unsorted::CurrentFrame - pTrip->ReadyFrame < FerryConvoyWaitFrames)
			{
				for (auto const pSister : UnitClass::Array)
				{
					if (!pSister || pSister == pTrans || pSister->Owner != pHouse || !pSister->IsAlive || pSister->InLimbo
						|| !pSister->Type || !IsAmphibiousOrAirTransport(pSister->Type)
						|| pSister->DistanceFrom(pTrans) > FerryConvoyCells * Unsorted::LeptonsPerCell
						|| ObjectZone(pSister, MovementZone::Normal) != transZone)
					{
						continue;
					}

					auto const pSisterTrip = FindFerryTrip(pSister->UniqueID);

					if (pSisterTrip && pSisterTrip->Departed)
						continue;

					// Still loading: cargo aboard and not ready yet.
					if (pSister->Passengers.NumPassengers > 0)
					{
						if (pSisterTrip && pSisterTrip->ReadyFrame < 0)
						{
							convoyHold = true;
							break;
						}

						continue;
					}
				}
			}

			// The whole battalion (user: call the transports the group needs, or build them, load it all,
			// then gather and sail together): units of the split still on land while empty transports of
			// ours are on their way to this zone keep it waiting, up to FerryBattalionWaitFrames. When the
			// last one is loaded, the joint departure takes them all at once.
			if (!convoyHold && Unsorted::CurrentFrame - pTrip->ReadyFrame < FerryBattalionWaitFrames)
			{
				auto const itLeft = ferryBattalionLeft.find(transZone);
				auto const itComing = ferryComingRoom.find(transZone);

				convoyHold = itLeft != ferryBattalionLeft.end() && itLeft->second > 0 && itComing != ferryComingRoom.end()
					&& itComing->second > 0;
			}
		}

		// A combat cargo waits on its shore for a real load, since a lone vehicle that lands is
		// finished off by any defender: longer under half the room than under three quarters of it,
		// counted from the first frame it sat there short of that. MCVs, harvesters, engineers and a
		// transport under fire still leave at once.
		bool underfilledHold = false;
		const int transRoom = pTrans->Type->Passengers;
		// The convoy's cargo counts as one load: with the fleet split each transport carries a part
		// of what its sister transports on this shore carry with it.
		int convoyCargo = totalPassSize;

		if (numPass >= 1 && onSafePickupShore && convoyCargo * 4 < transRoom * 3)
		{
			for (auto const pSister : UnitClass::Array)
			{
				if (!pSister || pSister == pTrans || pSister->Owner != pHouse || !pSister->IsAlive || pSister->InLimbo
					|| !pSister->Type || !IsAmphibiousOrAirTransport(pSister->Type)
					|| pSister->DistanceFrom(pTrans) > FerryConvoyCells * Unsorted::LeptonsPerCell
					|| ObjectZone(pSister, MovementZone::Normal) != transZone)
				{
					continue;
				}

				if (auto const pSisterTrip = FindFerryTrip(pSister->UniqueID); pSisterTrip && pSisterTrip->Departed)
					continue;

				convoyCargo += pSister->Passengers.GetTotalSize();
			}
		}

		if (pTrip && numPass >= 1 && onSafePickupShore && !carryingMCV && !carryingStolenForeignMCV && !carryingHarvester
			&& !carryingEngineer && !emergencyDepartUnderFire && convoyCargo * 4 < transRoom * 3)
		{
			if (pTrip->UnderfilledSince < 0 || Unsorted::CurrentFrame < pTrip->UnderfilledSince)
				pTrip->UnderfilledSince = Unsorted::CurrentFrame;

			const int waitFrames = totalPassSize * 2 < transRoom ? FerryHalfLoadWaitFrames : FerryMostLoadWaitFrames;
			underfilledHold = Unsorted::CurrentFrame - pTrip->UnderfilledSince < waitFrames;
		}
		else if (pTrip)
		{
			pTrip->UnderfilledSince = -1;
		}

		// Loading that stalls (units on the island that fit and never get aboard) does not hold a
		// real load for good: at half the room or more, with no new passenger in FerryLoadStallFrames
		// the load counts as complete. Under half it keeps waiting, a lone vehicle is not worth the trip.
		bool loadStalled = false;

		if (pTrip && numPass >= 1 && onSafePickupShore)
		{
			if (numPass != pTrip->LastPassCount || pTrip->LastBoardFrame < 0)
			{
				pTrip->LastPassCount = numPass;
				pTrip->LastBoardFrame = Unsorted::CurrentFrame;
			}

			loadStalled = totalPassSize * 2 >= transRoom && Unsorted::CurrentFrame - pTrip->LastBoardFrame >= FerryLoadStallFrames;
		}

		const bool nothingMoreToLoad = numPass >= 1 && onSafePickupShore && (!canFitMoreOnPickupIsland || loadStalled) && !convoyHold
			&& !underfilledHold;
		// Joint departure: a combat transport that sails from its pickup shore gives the signal, and the
		// transports with combat cargo aboard within FerryConvoyCells of it on that shore sail with it
		// for FerryConvoyGoFrames, so the battalion crosses and lands as one. The fleet split loaded
		// them side by side, yet they sailed minutes apart (22% of combat departures within 600 frames
		// of another, 60% within 3600, 15 matches). The units still walking up wait for the next one.
		const bool combatCargoAboard = numPass >= 1 && !carryingMCV && !carryingStolenForeignMCV && !carryingHarvester && !carryingEngineer;
		auto& convoyGoSignals = GetMacroState(pHouse).FerryConvoyGo;
		bool convoyGo = false;

		if (combatCargoAboard && onSafePickupShore && !tripDeparted && transZone >= 0)
		{
			if (auto const itGo = convoyGoSignals.find(transZone); itGo != convoyGoSignals.end()
				&& Unsorted::CurrentFrame >= itGo->second.Frame && Unsorted::CurrentFrame - itGo->second.Frame <= FerryConvoyGoFrames
				&& itGo->second.Leader != pTrans->UniqueID && transCoords.DistanceFrom(itGo->second.Cell) <= FerryConvoyCells)
			{
				convoyGo = true;
			}
		}

		const bool isReadyToSailEffective = carryingMCV || carryingStolenForeignMCV || carryingHarvester
			|| (carryingEngineer && pIslandTechGoalCell != nullptr)
			|| emergencyDepartUnderFire || hasCargoInTransitOrUnloading || nothingMoreToLoad || convoyGo;

		if (combatCargoAboard && onSafePickupShore && !tripDeparted && transZone >= 0 && isReadyToSailEffective && !convoyGo)
			convoyGoSignals[transZone] = { Unsorted::CurrentFrame, transCoords, pTrans->UniqueID };

		if (pTrip && !tripDeparted)
		{
			const bool engineerForTech = carryingEngineer && pIslandTechGoalCell != nullptr;

			pTrip->SailReason = (carryingMCV ? FerrySailBaseDeployer : 0)
				| (carryingStolenForeignMCV ? FerrySailStolenDeployer : 0)
				| (carryingHarvester ? FerrySailHarvester : 0)
				| (engineerForTech ? FerrySailEngineer : 0)
				| (emergencyDepartUnderFire ? FerrySailUnderFire : 0)
				| (hasCargoInTransitOrUnloading ? FerrySailInTransit : 0)
				| (nothingMoreToLoad ? FerrySailNothingMoreToLoad : 0)
				| (nothingMoreToLoad && loadStalled ? FerrySailLoadStalled : 0)
				| (convoyGo ? FerrySailConvoy : 0);
		}

		// Prevent AttackMove.Follow=true (e.g. on GDI APC/APCW) from hijacking transports into escorting warships!
		pTrans->ClearMegaMissionData();

		// Loaded and ready on its pickup shore with no landmass to go to: after FerryNoGoalUnloadFrames
		// the cargo is set down where the transport stands, instead of riding inside it for good.
		{
			auto& noGoalSince = GetMacroState(pHouse).FerryNoGoalSince;

			if (isReadyToSailEffective && !pActiveIslandGoal && numPass >= 1 && !tripDeparted && onSafePickupShore)
			{
				auto const itSince = noGoalSince.find(pTrans->UniqueID);

				if (itSince == noGoalSince.end() || itSince->second > Unsorted::CurrentFrame)
				{
					noGoalSince[pTrans->UniqueID] = Unsorted::CurrentFrame;
				}
				else if (Unsorted::CurrentFrame - itSince->second >= FerryNoGoalUnloadFrames && pCurTransCell
					&& pCurTransCell->LandType != LandType::Water && !pCurTransCell->GetBuilding())
				{
					if (pTrans->CurrentMission != Mission::Unload)
					{
						CountDecision(NeuralDecision::FerryCargoReturned);
						Debug::Log("[NeuralAI] House %d ferry: %s %u sets its %d passengers down at (%d,%d), nowhere to go for %d frames, frame %d.\n",
							pHouse->ArrayIndex, pTrans->Type->ID, pTrans->UniqueID, numPass, transCoords.X, transCoords.Y,
							Unsorted::CurrentFrame - itSince->second, Unsorted::CurrentFrame);
						pTrans->SetDestination(nullptr, true);
						pTrans->ClearNavigationList();
						pTrans->QueueMission(Mission::Unload, true);
					}

					continue;
				}
			}
			else
			{
				noGoalSince.erase(pTrans->UniqueID);
			}
		}

		// Whatever else keeps a cargo on its shore: a transport loaded with the same passengers on its
		// pickup shore for FerryStuckCargoFrames, past every wait of the loader, sets them down so they
		// fight again and the transport is free. 91 transports sat loaded 9000 frames or more (up to
		// 114000), no one boarding and nothing pending, in 55 of 111 houses with a ferry fleet (41
		// matches); the transport production counted them as fleet and built no other.
		if (pTrip && numPass >= 1 && !tripDeparted)
		{
			const int now = Unsorted::CurrentFrame;
			const bool settingDown = pTrip->SetDownFrame >= 0 && now >= pTrip->SetDownFrame
				&& now - pTrip->SetDownFrame < FerrySetDownFrames;
			const bool stuck = !settingDown && onSafePickupShore && pTrip->LastBoardFrame >= 0 && now >= pTrip->LastBoardFrame
				&& now - pTrip->LastBoardFrame >= FerryStuckCargoFrames;

			if ((settingDown || stuck) && pCurTransCell && pCurTransCell->LandType != LandType::Water && !pCurTransCell->GetBuilding())
			{
				if (stuck)
				{
					pTrip->SetDownFrame = now;
					CountDecision(NeuralDecision::FerryCargoReturned);
					Debug::Log("[NeuralAI] House %d ferry: %s %u sets its %d passengers down at (%d,%d), loaded on its shore for %d frames "
						"(ready %d, goal %d at (%d,%d) zone %d, underfilled hold %d, convoy hold %d, can fit more %d), frame %d.\n",
						pHouse->ArrayIndex, pTrans->Type->ID, pTrans->UniqueID, numPass, transCoords.X, transCoords.Y,
						now - pTrip->LastBoardFrame, isReadyToSailEffective ? 1 : 0, pActiveIslandGoal ? 1 : 0,
						pActiveIslandGoal ? pActiveIslandGoal->MapCoords.X : -1, pActiveIslandGoal ? pActiveIslandGoal->MapCoords.Y : -1,
						activeIslandZone, underfilledHold ? 1 : 0, convoyHold ? 1 : 0, canFitMoreOnPickupIsland ? 1 : 0, now);
				}

				if (pTrans->CurrentMission != Mission::Unload)
				{
					pTrans->SetDestination(nullptr, true);
					pTrans->ClearNavigationList();
					pTrans->QueueMission(Mission::Unload, true);
				}

				continue;
			}
		}
		else if (pTrip && numPass == 0)
		{
			pTrip->SetDownFrame = -1;
		}

		// Departed and stuck, or a yard MCV aboard with nowhere to unfold (FerryDepartedStuckFrames,
		// NeuralAI.State.inl; user approved, 2026-11-19, match #11475: a landing craft with an MCV
		// stood 50000 frames next to a narrow strip of its goal island, its arriving commitment kept
		// the goal, so the return of a cargo whose objective vanished never came, and the watchdog near
		// the landing did nothing after its first step). The trip drops its commitment; the first time
		// it is stuck it unloads where it stands when it touches ground of another zone than its origin,
		// else it takes the cargo back to its origin landmass (Homeward, the return below).
		if (tripDeparted && numPass >= 1)
		{
			const int now = Unsorted::CurrentFrame;
			const bool onLand = pCurTransCell && pCurTransCell->LandType != LandType::Water && !pCurTransCell->GetBuilding();

			if (pTrip->StillCoords == CellStruct::Empty || numPass != pTrip->StillPassengers || now < pTrip->StillFrame
				|| std::max(std::abs(transCoords.X - pTrip->StillCoords.X), std::abs(transCoords.Y - pTrip->StillCoords.Y)) > 1)
			{
				pTrip->StillCoords = transCoords;
				pTrip->StillFrame = now;
				pTrip->StillPassengers = numPass;
			}

			const int stoodFrames = now - pTrip->StillFrame;
			const bool departedStuck = stoodFrames >= FerryDepartedStuckFrames;
			const bool goalHasNoRoom = activeIslandZone >= 0 && isNoRoomIslandFor(pCarriedYardType, activeIslandZone);
			const bool mcvGoalLost = !departedStuck && !pTrip->Homeward && carryingYardMcv && !carryingStolenForeignMCV
				&& (!pIslandResourceGoalCell || goalHasNoRoom);
			// Not while it is about to land: arriving (its landing committed, or within
			// FerryArrivalKeepCells of the last one) and touching the goal zone with room there, or where
			// the yard fits near the landing. The goal often turns null at the landing itself (the island
			// counts as colonized once a deployer of ours stands there).
			bool mcvAboutToLand = false;

			if (mcvGoalLost && IsGroundZone(activeIslandZone))
			{
				const bool arrivingNow = pTrip->CommittedLanding != CellStruct::Empty
					|| (pTrip->LastLanding != CellStruct::Empty && transCoords.DistanceFrom(pTrip->LastLanding) <= FerryArrivalKeepCells);

				if (arrivingNow)
				{
					const CellStruct landingRef = pTrip->CommittedLanding != CellStruct::Empty ? pTrip->CommittedLanding
						: (pTrip->LastLanding != CellStruct::Empty ? pTrip->LastLanding : transCoords);
					mcvAboutToLand = (!goalHasNoRoom && TransportTouchesLandZone(transCoords, activeIslandZone))
						|| FindDeployableFoundationNearCell(pHouse, pCarriedYardType, landingRef, activeIslandZone, nullptr) != CellStruct::Empty;
				}
			}

			const bool mcvNowhere = mcvGoalLost && !mcvAboutToLand;

			if (departedStuck || mcvNowhere)
			{
				// With no origin known, ground of another zone than the one it stands on.
				const int touchedZone = (departedStuck && onLand && pTrip->UnloadHereFrame < 0)
					? TransportTouchedGroundZone(transCoords, ferryOriginZone >= 0 ? ferryOriginZone : transZone)
					: -1;
				// The way back: its origin landmass, or the main base's when the origin holds no yard of ours
				// or was given up for lack of room (a yard MCV loaded on such an island went back there).
				const int originZone = (ferryOriginZone >= 0) ? ferryOriginZone : homeGroundZone;
				const bool originDeadEnd = originZone != homeGroundZone && IsGroundZone(homeGroundZone)
					&& (std::find(ownYardZones.begin(), ownYardZones.end(), originZone) == ownYardZones.end()
						|| isNoRoomIslandFor(pCarriedYardType, originZone));
				const int backZone = originDeadEnd ? homeGroundZone : originZone;

				pTrip->CommittedLanding = CellStruct::Empty;
				pTrip->CommittedZone = -1;
				pTrip->CommittedGoal = CellStruct::Empty;
				pTrip->LastLanding = CellStruct::Empty;
				pTrip->StillCoords = transCoords;
				pTrip->StillFrame = now;

				if (touchedZone >= 0)
				{
					pTrip->UnloadHereFrame = now;
				}
				else
				{
					pTrip->Homeward = true;
					pTrip->HomewardZone = backZone;
				}

				CountDecision(departedStuck ? NeuralDecision::FerryDepartedStuck : NeuralDecision::FerryMcvNowhere, pHouse->ArrayIndex);

				// TEMP diagnostic: each give-up (FerryGiveUpLogLines a match).
				static int giveUpLogSerial = -1;
				static int giveUpLogCount = 0;

				if (giveUpLogSerial != NeuralMatchSerial)
				{
					giveUpLogSerial = NeuralMatchSerial;
					giveUpLogCount = 0;
				}

				if (giveUpLogCount < FerryGiveUpLogLines)
				{
					++giveUpLogCount;
					Debug::Log("[NeuralAI] House %d ferry gives up: %s %u at (%d,%d) zone %d cell %s with %d passengers (yard MCV %d), %s "
						"(still %d frames), goal zone %d (island goal %s zone %d, no room %d), origin zone %d, %s zone %d, frame %d.\n",
						pHouse->ArrayIndex, pTrans->Type->ID, pTrans->UniqueID, transCoords.X, transCoords.Y, transZone,
						onLand ? "land" : "water", numPass, carryingYardMcv ? 1 : 0, departedStuck ? "stuck" : "MCV with nowhere to unfold",
						stoodFrames, activeIslandZone, islandGoalKindName(), resourceIslandZone, goalHasNoRoom ? 1 : 0, ferryOriginZone,
						touchedZone >= 0 ? "unloads here next to" : "goes back to", touchedZone >= 0 ? touchedZone : backZone, now);
				}
			}

			// Told to unload where it stands: it keeps unloading for FerrySetDownFrames.
			if (onLand && pTrip->UnloadHereFrame >= 0 && now >= pTrip->UnloadHereFrame && now - pTrip->UnloadHereFrame < FerrySetDownFrames)
			{
				if (pTrans->CurrentMission != Mission::Unload)
				{
					pTrans->SetDestination(nullptr, true);
					pTrans->ClearNavigationList();
					pTrans->QueueMission(Mission::Unload, true);
				}

				continue;
			}
		}

		// The cargo's objective vanished while at sea and no other landmass is a
		// valid destination, or the trip gave its goal up (Homeward, above): bring
		// the cargo back to its loading island and drop it there instead of
		// drifting loaded forever. Its land counts as reached by touch as well
		// (TransportTouchesLandZone).
		if (numPass >= 1 && tripDeparted && ((isReadyToSailEffective && !pActiveIslandGoal) || pTrip->Homeward))
		{
			const int returnZone = (pTrip->Homeward && IsGroundZone(pTrip->HomewardZone)) ? pTrip->HomewardZone
				: ((ferryOriginZone >= 0) ? ferryOriginZone : homeGroundZone);
			const bool onReturnIslandLand = pCurTransCell && pCurTransCell->LandType != LandType::Water
				&& !pCurTransCell->GetBuilding() && returnZone >= 0
				&& (transZone == returnZone || TransportTouchesLandZone(transCoords, returnZone));

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

			// Landing commitment: within 14 cells of a landing the transport keeps it and unloads,
			// enemy units coming near or not (they made the pick jump away every tick and the
			// transport turned back with its cargo). A cell no longer usable (a building on it,
			// water) is dropped.
			if (pTrip && tripDeparted)
			{
				auto const pCommitted = pTrip->CommittedLanding != CellStruct::Empty
					? MapClass::Instance.TryGetCellAt(pTrip->CommittedLanding)
					: nullptr;

				if (pCommitted && pCommitted->LandType != LandType::Water && !pCommitted->GetBuilding()
					&& MapClass::Instance.GetMovementZoneType(pTrip->CommittedLanding, MovementZone::Normal, false) == activeIslandZone)
				{
					pSafeLanding = pCommitted;
				}
				else
				{
					pTrip->CommittedLanding = CellStruct::Empty;

					// Diagnostics: a landing that jumps before the commitment (the transport turns).
					if (pSafeLanding && numPass >= 1)
					{
						auto& macro = GetMacroState(pHouse);
						const int now = Unsorted::CurrentFrame;

						if (pTrip->LastLanding != CellStruct::Empty && pTrip->LastLanding.DistanceFrom(pSafeLanding->MapCoords) > FerryLandingRepickCells)
						{
							++macro.FerryLandingRepicks;

							if (macro.FerryLandingRepickLogFrame < 0 || now < macro.FerryLandingRepickLogFrame
								|| now - macro.FerryLandingRepickLogFrame >= FerryIslandKeptLogFrames)
							{
								Debug::Log("[NeuralAI] Frame %d | House #%d ferry landing re-picks: %d by loaded transports before their "
									"landing commitment (since frame %d), last %s %u at (%d,%d) from (%d,%d) to (%d,%d)\n", now,
									pHouse->ArrayIndex, macro.FerryLandingRepicks, std::max(0, macro.FerryLandingRepickLogFrame),
									pTrans->Type->ID, pTrans->UniqueID, transCoords.X, transCoords.Y, pTrip->LastLanding.X,
									pTrip->LastLanding.Y, pSafeLanding->MapCoords.X, pSafeLanding->MapCoords.Y);
								macro.FerryLandingRepickLogFrame = now;
								macro.FerryLandingRepicks = 0;
							}
						}

						pTrip->LastLanding = pSafeLanding->MapCoords;
					}

					if (pSafeLanding && transCoords.DistanceFrom(pSafeLanding->MapCoords) <= 14.0)
						pTrip->CommittedLanding = pSafeLanding->MapCoords;
				}
			}

			if (pSafeLanding)
			{
				auto const pTransCell = pTrans->GetCell();
				const double distToLanding = transCoords.DistanceFrom(pSafeLanding->MapCoords);
				// Unload strictly on the destination island: never on the first coast
				// touched on the way, an intermediate island or the loading island. The
				// island counts as reached by touch as well (TransportTouchesLandZone: a
				// narrow strip reads as no ground in TransportLandZone).
				bool onTargetIslandLand = pTransCell && pTransCell->LandType != LandType::Water
					&& !pTransCell->GetBuilding() && isValidFerryGoalZone(activeIslandZone)
					&& (transZone == activeIslandZone || TransportTouchesLandZone(transCoords, activeIslandZone));

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
								NoteOrderCaller(pTrans, "move");
								continue;
							}
						}
						else if (pTransCell && pTransCell->LandType != LandType::Water
							&& (isValidFerryGoalZone(transZone)
							|| TransportTouchedGroundZone(transCoords, ferryOriginZone >= 0 ? ferryOriginZone : transZone) >= 0))
						{
							// Stage 2+: unload right here, also next to ground of another zone than the
							// origin (a narrow strip); a stuck full transport helps nobody.
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
										&& pFootAdj->CurrentMission != Mission::Attack
										&& !IsInTransportOp(pHouse, pFootAdj->UniqueID))
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
				&& (numPass == 0 || transZone == deployerZone)
				// Only with an island to take it to that this transport lands on: with none the
				// deployer was boarded, set down for having nowhere to go, and boarded again. A yard MCV
				// of our own tech is the deployer only when it may cross (yardMcvMayCrossToIslandGoal, at
				// its pick above).
				&& pIslandResourceGoalCell != nullptr
				&& CanTransportTypeLink(pTrans->Type, pHomeExpansionDeployer->GetMapCoords(), pIslandResourceGoalCell->MapCoords));
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
					if (moveToLoadingCell(pTrans, transCoords, transZone, true))
						continue;

					// Stay 100% still on the land spot where the transport is parked!
					if (pTrans->CurrentMission != Mission::Guard || pTrans->Destination != nullptr)
					{
						pTrans->SetDestination(nullptr, true);
						pTrans->ClearNavigationList();
						pTrans->ForceMission(Mission::Guard);
					}

					if ((pSingleActiveBoarder == nullptr || pSingleActiveBoarder == pHomeExpansionDeployer)
						&& openFerryDoor(pTrans, pHomeExpansionDeployer))
					{
						OrderFootToEnter(pHomeExpansionDeployer, pTrans);
					}

					continue;
				}
			}
			// One transport for the engineer, the one it is already boarding if any: asked by every
			// transport on its island, it was sent from one to the next and held them all.
			else if (pHomeIslandEngineer != nullptr && !carryingEngineer && !engineerTransportAssigned
				&& CanPassengerFitInTransport(pTrans, pHomeIslandEngineer->Type)
				&& CanTransportTypeLink(pTrans->Type, pHomeIslandEngineer->GetMapCoords(), pIslandTechGoalCell->MapCoords)
				&& !((pHomeIslandEngineer->CurrentMission == Mission::Enter || pHomeIslandEngineer->QueuedMission == Mission::Enter)
					&& !pHomeIslandEngineer->ContainsLink(pTrans) && pHomeIslandEngineer->ArchiveTarget != pTrans
					&& pHomeIslandEngineer->Destination != pTrans))
			{
				engineerTransportAssigned = true;
				const int engZone = ObjectZone(pHomeIslandEngineer, MovementZone::Normal);
				// The transport comes to the engineer, on the outskirts of the base or expansion it is at,
				// rather than the engineer walking to a far coast.
				const CellStruct engCoords = pHomeIslandEngineer->GetMapCoords();
				CellClass* pEngPickupBeach = FindBoardingCell(pHouse, engCoords, engZone, pIslandTechGoalCell->MapCoords, pTrans->Type);

				if (!pEngPickupBeach)
					pEngPickupBeach = FindSafeIslandLandingCell(pHouse, engCoords, engZone, false);

				const bool onEngLand = transZone == engZone && pTransCell && pTransCell->LandType != LandType::Water
					&& (!pEngPickupBeach || transCoords.DistanceFrom(pEngPickupBeach->MapCoords) <= OpGatherCells + 3.0
						|| transCoords.DistanceFrom(engCoords) <= OpGatherCells + 3.0);
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
					if (moveToLoadingCell(pTrans, transCoords, transZone, true))
						continue;

					if (pTrans->CurrentMission != Mission::Guard || pTrans->Destination != nullptr)
					{
						pTrans->SetDestination(nullptr, true);
						pTrans->ClearNavigationList();
						pTrans->ForceMission(Mission::Guard);
					}

					if ((pSingleActiveBoarder == nullptr || pSingleActiveBoarder == pHomeIslandEngineer)
						&& openFerryDoor(pTrans, pHomeIslandEngineer))
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

			// A transport that holds cargo keeps loading on the shore it stands on: fetching more from
			// another shore takes it across water, where a loaded transport counts as departed and
			// lands with whatever it has.
			if (numPass >= 1 && onSafePickupShore)
			{
				desiredPickupZone = transZone;
				desiredPickupAnchor = transCoords;
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
			if (moveToLoadingCell(pTrans, transCoords, transZone, pSingleActiveBoarder != nullptr))
				continue;

			// An armed transport with a target stays in the air to shoot (HoverAttack) and nobody can
			// board it: with passengers to take it drops the target.
			if (pTrans->Target && pSingleActiveBoarder != nullptr)
			{
				pTrans->SetTarget(nullptr);
				CountDecision(NeuralDecision::FerryTargetDropped, pHouse->ArrayIndex);
			}

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

			// Parked with something to load or loaded: nobody else orders it about, and whoever
			// moves it all the same is logged.
			if (pSingleActiveBoarder != nullptr || numPass >= 1)
				GetMacroState(pHouse).ParkedFerries[pTrans->UniqueID] = { pTrans, Unsorted::CurrentFrame };

			// Nobody boards a transport with nowhere to go: an enemy somewhere across the water with no
			// goal this transport can sail for loaded idle units that were set down again after
			// FerryNoGoalUnloadFrames, over and over (one soldier 15 times in one match).
			if (!pActiveIslandGoal)
				continue;

			// With room left, the one boarding is sent in again when it lost its radio link.
			const int maxAllowedPassSize = std::max(0, pTrans->Type->Passengers - reservedSizeForDeployer);
			const int freeCapNow = maxAllowedPassSize - totalPassSize;

			if (numPass >= pTrans->Type->Passengers || freeCapNow <= 0)
				continue;

			if (pSingleActiveBoarder != nullptr)
			{
				// Re-issue Enter only if the active boarder somehow lost its radio link
				if (!pSingleActiveBoarder->ContainsLink(pTrans))
					OrderFootToEnter(pSingleActiveBoarder, pTrans);
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

		return best * AirSortieShots(pAircraft->Type);
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
	const bool airHarassAllowed = SlotGateHeld(scores[32], -100, 32);
	const bool airForceInCharge = pExt->NeuralAI_ActiveExpert == 8;
	// Island war (AirWing*): the strike aircraft go in wings only, and the strikes of ours they fly.
	const bool airIslandWar = IsAirIslandWar(pHouse);
	auto const& wingAirState = GetAirForceState(pHouse);

	// The harassment of the aircraft that are not of the wings (gas bombers, gunships that do
	// nothing to buildings; the strike aircraft wait on their pads for their wing): a ground contact
	// in sight, never an aircraft (IsAirborneThreatTechno: one parked on its pad too), with no
	// anti-air risk there or on the way (known anti-air defenses within their real reach).
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
			// anti-air units we know of, places our aircraft were shot down; computed once per
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

	// Strike wings form first, whichever expert is in charge (NeuralAI.AirForce.inl).
	UpdateAirWings(pHouse);

	// Interceptors: enemy air we know of that threatens the base (the early warning flanks), and
	// with Expert 8 in charge any enemy flyer we know of within 30 cells that no known anti-air
	// defense covers. Only aircraft in the air: one parked on its pad counts as a flyer
	// (IsAirborneThreatTechno) and drew our Black Eagles one by one into the gattling cannons of the
	// enemy base in match #11501. Strikes already sent at a flyer count against it, so interceptors
	// spread.
	auto pickAirInterceptTarget = [&](AircraftClass* pAircraft) -> TechnoClass*
	{
		const int flankIdx = baseAirFlankCount > 0
			? pickAirFlankFor(pAircraft, true, (airForceInCharge ? 40 : 28) * Unsorted::LeptonsPerCell, true)
			: -1;

		if (flankIdx >= 0 && IsCellKnownToAI(pHouse, baseAirFlanks[flankIdx].Target->GetMapCoords()))
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

		sortie *= AirSortieShots(pAircraft->Type);

		const CellStruct ac = pAircraft->GetMapCoords();
		TechnoClass* pBest = nullptr;
		int bestScore = 0;

		for (auto const& contact : enemyContacts)
		{
			if (!contact.Airborne || !contact.Techno->IsInAir() || ac.DistanceFrom(contact.Coords) > 30.0
				|| !IsCellKnownToAI(pHouse, contact.Coords) || CountKnownAntiAirCovering(pHouse, contact.Coords) > 0)
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

	// A lone aircraft (one not of the wings) only strikes a building with no anti-air risk at all
	// (known anti-air sites, anti-air units in sight, places our aircraft were shot down), picked
	// once per tick. The enemy mind controller and a lone harvester in sight are worth a dive only
	// where no anti-air covers them.
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
		// Aircraft flying a strategic plan (air strike, airborne second front...) belong to that plan, the
		// carryalls of a transport operation to the operation.
		if (!pAir || pAir->Owner != pHouse || pAir->InLimbo || !pAir->IsAlive || !pAir->Type
			|| IsPlanMember(pHouse, pAir->UniqueID) || IsInTransportOp(pHouse, pAir->UniqueID))
		{
			continue;
		}

		// An aircraft with no weapon keeps no target: on Guard the engine puts any aircraft that
		// has one on Attack (AircraftClass::Mission_Guard, 0x41A822), where a carryall found no
		// firing cell and sat between "pick a cell" and "return" for good.
		if (pAir->Target && abstract_cast<TechnoClass*>(pAir->Target) && !AircraftHitsGround(pAir->Type) && !AircraftHitsAir(pAir->Type)
			&& (pAir->CurrentMission == Mission::Attack || pAir->CurrentMission == Mission::Guard
				|| pAir->CurrentMission == Mission::Area_Guard))
		{
			const bool wasAttacking = pAir->CurrentMission == Mission::Attack;
			pAir->SetTarget(nullptr);

			if (wasAttacking)
				pAir->EnterIdleMode(false, true);
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
			const int airZone = TransportLandZone(airCoords, pAir->OnBridge);
			bool airCarryingMCV = false;
			bool airCarryingHarvester = false;
			bool airCarryingEngineer = false;
			for (FootClass* pPass = pAir->Passengers.GetFirstPassenger(); pPass != nullptr;
				pPass = abstract_cast<FootClass*>(pPass->NextObject))
			{
				if (auto const pPassUnit = abstract_cast<UnitClass*>(pPass))
				{
					if (pPassUnit->Type && pPassUnit->Type->DeploysInto != nullptr && !IsDeployableSiegeUnitType(pPassUnit->Type))
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
			const bool moreUnitsFitAir = pSingleAirBoarder != nullptr;

			const bool airReadyToFly = airCarryingMCV || airCarryingHarvester
				|| (airCarryingEngineer && pIslandTechGoalCell != nullptr)
				|| (numAirPass >= 1 && (pAir->IsInAir() || !moreUnitsFitAir));

			if (airReadyToFly)
			{
				CellClass* pAirGoal = pIslandGoalCell;
				int airGoalZone = targetIslandZone;
				if ((airCarryingMCV || (airCarryingHarvester && islandGoalTakesHarvesters())) && pIslandResourceGoalCell)
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
			else if (!pAir->IsInAir() && pAir->GetHeight() == 0 && freeAirCap > 0
				&& (enemyOnDisconnectedIsland || pIslandResourceGoalCell != nullptr
					|| pIslandTechGoalCell != nullptr || scores[28] > 600 || SlotGateHeld(scores[75], 100, 75, pAir->UniqueID)))
			{
				// The one boarding is sent in again when it lost its radio link.
				if (pSingleAirBoarder)
				{
					if (!pSingleAirBoarder->ContainsLink(pAir))
						OrderFootToEnter(pSingleAirBoarder, pAir);
				}
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
				else if (pCarriedUnit->Type->Harvester || pCarriedUnit->Type->Weeder)
				{
					// Where it can unload: the island goal with a refinery of ours, else the nearest refinery.
					pDropCell = islandGoalTakesHarvesters() ? pIslandResourceGoalCell : nearestOwnRefineryCell(pAir->GetMapCoords());
				}
				else if (pCarriedUnit->Type->DeploysInto != nullptr && pIslandResourceGoalCell)
				{
					pDropCell = pIslandResourceGoalCell;
				}
			}

			if (!pDropCell && !(pCarriedUnit->Type && (pCarriedUnit->Type->Harvester || pCarriedUnit->Type->Weeder)))
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
						AdvanceRoutedPickup(pHouse, pAir, pPendingCargo);

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
			const bool evacuationAllowed = pRepairDepot && SlotGateHeld(scores[54], -200, 54, pAir->UniqueID);

			for (auto const pU : UnitClass::Array)
			{
				// A unit of a transport operation waits for the operation's own transports.
				if (!pU || pU->Owner != pHouse || pU->InLimbo || !pU->IsAlive || !pU->Type
					|| !CanCarryallLiftUnit(pAir, pU) || IsInTransportOp(pHouse, pU->UniqueID))
				{
					continue;
				}

				const int uZone
					= ObjectZone(pU, MovementZone::Normal);
				const bool isCriticallyDamagedTank
					= (pRepairDepot && pU->Type->Cost >= 1000 && pU->GetHealthPercentage() < 0.40);
				const bool isLoadedStrikeAPC = (pU->Type->Passengers >= 3 && pU->Passengers.NumPassengers >= 1
					&& (pWaveTargetCell || pIslandGoalCell || pIslandTechGoalCell));
				// A yard MCV of our own tech only when it may cross (yardMcvMayCrossToIslandGoal; a stolen one never: slot 82), another
				// deployer only when it expands with its building (deployerExpandsWith) and fits there.
				const bool isIslandExpansionMCV = (pU->Type->DeploysInto != nullptr && pIslandResourceGoalCell
					&& uZone >= 0 && uZone != resourceIslandZone
					&& (pHouse->NumConYards >= 1 || pHouse->CountOwnedNow(pU->Type) >= 2)
					&& (pU->Type->DeploysInto->ConstructionYard
						? (!(pHouse->CountOwnedNow(pU->Type->DeploysInto) == 0 && pHouse->NumConYards >= 1)
							&& (pU == pHomeExpansionDeployer || yardMcvMayCrossToIslandGoal(pU)))
						: ((pU == pExtraHomeSlaveMiner || deployerExpandsWith(pU->Type->DeploysInto)) && !deployerSentToMainBase(pU->UniqueID)
							&& deployerFitsOnIslandGoal(pU, pU->Type->DeploysInto))));
				const bool isIslandHarvester = ((pU->Type->Harvester || pU->Type->Weeder) && islandGoalTakesHarvesters()
					&& uZone == homeGroundZone && uZone != resourceIslandZone && pHouse->CountOwnedNow(pU->Type) >= 2);
				const bool strandedHarvester = isStrandedHarvester(pU, uZone);
				// Combat evacuation: an expensive unit losing its fight is pulled out early.
				const bool isLosingExpensiveUnit = evacuationAllowed && !isCriticallyDamagedTank && pU->Type->Cost >= 1000
					&& pU->GetHealthPercentage() < 0.70 && IsFootOutmatched(pHouse, pU, 7.0);
				const bool isIslandJob = isIslandExpansionMCV || isIslandHarvester;
				if (isCriticallyDamagedTank || isLosingExpensiveUnit || isLoadedStrikeAPC || strandedHarvester
					|| (isIslandJob && !islandLiftsPaused))
				{
					AdvanceRoutedPickup(pHouse, pAir, pU);
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

		// Aircraft with unlimited ammo (Ammo=-1) have no load to spend and are always ready. One
		// with no weapon has no load at all: a carryall was sent to attack harvesters and sat on
		// Attack until the stall watchdog released it, again and again.
		const bool hitsGround = AircraftHitsGround(pAir->Type);
		// On its dock it waits for the full load: sent out at once it flew every sortie with what
		// it had left (one missile of four), and with a target the dock does not reload it, so the
		// target of its last strike is dropped while it reloads.
		const bool reloadingOnDock = IsAircraftReloadingOnDock(pAir);

		if (reloadingOnDock && isAirIdle && pAir->Target)
			pAir->SetTarget(nullptr);

		const bool hasAirLoad = (hitsGround || AircraftHitsAir(pAir->Type)) && !reloadingOnDock
			&& (pAir->Ammo > 0 || (pAir->Type->Ammo < 0 && !pAir->Type->Spawned));
		const bool isAirForceCraft = IsAirForceCraft(pAir->Type);
		const bool isWingCraft = IsWingStrikeType(pAir->Type);

		// Flying home to its pad: along the safe lane, not over the base it has just struck.
		if (isAirForceCraft && FlyAircraftHomeSafely(pHouse, pAir))
			continue;

		// In an island war a strike aircraft put on Attack outside a strike of ours (the engine on
		// Guard, or on its way home or scouting) at a ground target farther than AirPadGuardCells from
		// our nearest helipad goes back to its pad: alone it was lost (AirWing*). A chase of an enemy in
		// the air is an interception, and stays.
		if (isWingCraft && airIslandWar && pAir->CurrentMission == Mission::Attack && pAir->Target
			&& !wingAirState.Orders.contains(pAir->UniqueID))
		{
			auto const pTargetAir = abstract_cast<AircraftClass*>(pAir->Target);

			if (!pTargetAir || !pTargetAir->IsInAir())
			{
				const int padCells = CellsFromNearestHelipad(pHouse, CellClass::Coord2Cell(pAir->Target->GetCoords()));

				if (padCells > AirPadGuardCells)
				{
					WaitOnAirPad(pHouse, pAir);
					continue;
				}
			}
		}

		if (hasAirLoad && isAirIdle)
		{
			TechnoClass* pInterceptTarget = nullptr;

			if (airDiveAtMindController && hitsGround && !isWingCraft)
			{
				OrderAircraftAttack(pAir, pEnemyMindController);
				NoteAirTask(pHouse, pAir, NeuralAirTask::MindDive);
			}
			else if (isAirForceCraft && ContinueAirStrike(pHouse, pAir))
			{
				// Flying its wing strike: a member of a wing on its way out stays with it.
			}
			else if (isAirForceCraft && pAir->GetHealthPercentage() >= 0.4 && AircraftHitsAir(pAir->Type)
				&& (pInterceptTarget = pickAirInterceptTarget(pAir)) != nullptr)
			{
				OrderAircraftAttack(pAir, pInterceptTarget);
				CountDecision(NeuralDecision::AirInterceptSortie, pHouse->ArrayIndex);
				NoteAirTask(pHouse, pAir, NeuralAirTask::Intercept);
			}
			else if (isWingCraft)
			{
				// A strike aircraft waits on its pad for its wing (UpdateAirWings): it no longer goes
				// alone at units, harvesters or buildings, nor on patrol.
				WaitOnAirPad(pHouse, pAir);
			}
			else if (TechnoClass* const pPrey = (airHarassAllowed && !pAir->Type->Spawned) ? pickAirHarassTarget(pAir) : nullptr)
			{
				OrderAircraftAttack(pAir, pPrey);
				CountDecision(NeuralDecision::AirHarass, pHouse->ArrayIndex);
				NoteAirTask(pHouse, pAir, NeuralAirTask::Harass);
			}
			else if (airDiveAtHarvester && hitsGround && SlotGateHeld(scores[56], 200, 56, pAir->UniqueID))
			{
				OrderAircraftAttack(pAir, pVisibleEnemyHarvester);
				NoteAirTask(pHouse, pAir, NeuralAirTask::HarvesterDive);
			}
			else if (!airForceInCharge && isAirForceCraft && AircraftHitsGround(pAir->Type)
				&& pAir->GetHealthPercentage() >= AirSoloStrikeMinHealth && getSoloStrikeTarget()
				&& AirSortieDamageVs(pAir, pSoloStrikeTarget) > 0)
			{
				FlySoloAirStrike(pHouse, pAir, pSoloStrikeTarget);
				CountDecision(NeuralDecision::AirSoloStrike, pHouse->ArrayIndex);
				NoteAirTask(pHouse, pAir, NeuralAirTask::Solo);
			}
			else if (AssignAirPatrol(pHouse, pAir))
			{
				// Nothing to strike: patrol the route of key points (NeuralAI.AirPatrol.inl).
				NoteAirTask(pHouse, pAir, NeuralAirTask::Patrol);
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
				if (isLowPower && hasPoweredDefense && SlotGateHeld(scores[60], 80, 60, pOwnBld->UniqueID) && pOwnBld->StuffEnabled
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
			if (pOwnBld->Type->Bunker && pOwnBld->BunkerLinkedItem != nullptr && SlotGateHeld(scores[73], 60, 73, pOwnBld->UniqueID))
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
				&& pOwnBld != pHouse->ConYards.GetItem(0) && SlotGateHeld(scores[82], 60, 82, pOwnBld->UniqueID) && homeCoords != CellStruct::Empty
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
				// The yard packs up for the island only when its MCV may cross (yardMcvCrossingAllowed, the
				// free field read from the yard's cell).
				const bool yardNoRoomHere = isNoRoomIsland(yz);
				const bool canFerryNomadNow = (pIslandResourceGoalCell != nullptr && yz != resourceIslandZone && ownsLiveMcvTransport
					&& yardMcvCrossingAllowed(yardNoRoomHere, false)
					&& (yardNoRoomHere || islandGoalKind == IslandGoalKind::EnemyBeachhead
						|| (IsGroundZone(yz) && FindFreeOreFieldCell(pHouse, yc, yz, 14) == nullptr)));

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

				if (isSecondaryYard && pNomadicResourceGoalCell == nullptr && SlotGateHeld(scores[81], 75, 81, pOwnBld->UniqueID))
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
				else if (!busyConstructingBld && canFerryNomadNow && pHouse->NumConYards >= 2 && PruneIslandWaitYards(pHouse) > 0
					&& !IsIslandCrossingBlocked(pHouse, yz, resourceIslandZone)
					&& (GetMacroState(pHouse).IslandWaitPackFrame < 0 || Unsorted::CurrentFrame < GetMacroState(pHouse).IslandWaitPackFrame
						|| Unsorted::CurrentFrame - GetMacroState(pHouse).IslandWaitPackFrame >= IslandWaitRepackFrames))
				{
					// A yard an island expansion MCV unfolded while no ride came (land before islands,
					// IslandMcvWaitFrames): it packs up for the island once an idle transport able to make
					// the crossing is near, the crossing is open to the transport operations and the last
					// such pack-up of the house is IslandWaitRepackFrames old (its MCV gets a new UniqueID,
					// so its own wait would start again from zero).
					auto& waitYards = GetMacroState(pHouse).IslandWaitYards;
					auto const itWaitYard = std::find_if(waitYards.begin(), waitYards.end(), [yc](const std::pair<CellStruct, int>& entry)
						{ return std::max(std::abs(entry.first.X - yc.X), std::abs(entry.first.Y - yc.Y)) <= 3; });

					if (itWaitYard != waitYards.end())
					{
						if (auto const pFerry = FindIdleIslandMcvFerry(pHouse, pMcvType, yc, pIslandResourceGoalCell->MapCoords))
						{
							// TEMP diagnostic: a yard that unfolded while it waited packs up for its ride.
							Debug::Log("[NeuralAI] House %d IslandExpansion yard at (%d,%d) packs up for the island (%d,%d): %s %u free %d cells away, "
								"stood %d frames, %d yards, frame %d.\n",
								pHouse->ArrayIndex, yc.X, yc.Y, pIslandResourceGoalCell->MapCoords.X, pIslandResourceGoalCell->MapCoords.Y,
								pFerry->Type->ID, pFerry->UniqueID, static_cast<int>(yc.DistanceFrom(pFerry->GetMapCoords())),
								Unsorted::CurrentFrame - itWaitYard->second, pHouse->NumConYards, Unsorted::CurrentFrame);
							waitYards.erase(itWaitYard);
							GetMacroState(pHouse).IslandWaitPackFrame = Unsorted::CurrentFrame;
							UndeployBuildingToVehicle(pOwnBld, pIslandResourceGoalCell->MapCoords);
							handledConYardAction = true;
						}
					}
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
				&& !pOwnBld->Type->CanBeOccupied && pOwnBld->CurrentMission != Mission::Selling)
			{
				// Classic C&C Sell-Dodge right before destruction! Never a garrison (a civilian
				// building we hold: at red health the engine evicts the soldiers and gives it back).
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
				// Chopper, Tick Tank, or Nod Artillery has finished combat (no enemy its gun can hit within
				// its reach) and has stood deployed for 450 frames, pack back into a mobile vehicle so it
				// advances with the army.
				auto& deployedSince = GetMacroState(pHouse).DeployedSiegeSince;

				if (deployedSince.size() > 512)
					std::erase_if(deployedSince, [](const auto& e) { return Unsorted::CurrentFrame - e.second > 9000; });

				const int sinceFrame = deployedSince.try_emplace(pOwnBld->UniqueID, Unsorted::CurrentFrame).first->second;
				const int gunRange = GroundRangeOf(pOwnBld->Type);
				bool enemyInRange = Unsorted::CurrentFrame - sinceFrame < 450;

				if (!enemyInRange)
				{
					ForEachTechnoNear(pOwnBld->GetMapCoords(), std::max(8, gunRange / Unsorted::LeptonsPerCell + 1), [&](TechnoClass* pETech) -> bool
					{
						if (!pETech->Owner || pETech->Owner == pHouse || pHouse->IsAlliedWith(pETech->Owner) || pETech->Owner->IsNeutral())
							return true;

						if (DeployedGunCanHit(pOwnBld->GetCoords(), pETech, gunRange))
							enemyInRange = true;

						return !enemyInRange;
					});
				}

				if (!enemyInRange)
				{
					const CellStruct rallyGoal = pWaveTargetCell ? pWaveTargetCell->MapCoords : homeCoords;
					UndeployBuildingToVehicle(pOwnBld, rallyGoal);
				}
			}
			else if (pOwnBld->Type->ClickRepairable)
			{
				// A garrison we hold is a free bunker (user): repaired below 75% whenever 600 credits are
				// there, not left to the slot 59 gate (at red health the engine evicts the soldiers).
				const bool heldGarrison = pOwnBld->Type->CanBeOccupied && pOwnBld->Occupants.Count > 0;

				if (heldGarrison && hpPct < 0.75 && currentCredits > 600 && !pOwnBld->IsBeingRepaired)
				{
					pOwnBld->SetRepairState(1);
				}
				// Slot 59 (Smart Wrench Building Repair): Activate engine wrench repair
				// on damaged key structures when credits are healthy, and pause repair
				// if credits drop critically low!
				else if (hpPct < 0.82 && hpPct >= sellDodgeThresh && SlotGateHeld(scores[59], 80, 59, pOwnBld->UniqueID) && currentCredits > 1200
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
		auto unlockValueOf = [pHouse](const std::vector<BuildingTypeClass*>& delivered, bool& opensResearchProducer) -> int
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

				// A producer of what our research costs pays for the rest of it: the best unlock.
				if (ResearchPaidByProducer(pHouse, pType) > 0)
				{
					opensResearchProducer = true;
					value += ResearchGeneratorUnlockValue;

					return;
				}

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
			if (!pSuper || !pSuper->Type || !pSuper->IsReady || !IsResearchSuperWeapon(pSuper->Type, pHouse))
				continue;

			auto const pSWExt = SWTypeExt::Fetch(pSuper->Type);

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

			bool opensResearchProducer = false;
			value += unlockValueOf(deliveredTypes, opensResearchProducer);
			value -= cost / 50;

			// CanFire also asks for the resources the research costs (battle points), as for a human. The
			// research that opens a producer of those resources is the one saved for while they are short.
			if (!pSuper->CanFire() && !opensResearchProducer)
				continue;

			if (value > bestResearchValue)
			{
				bestResearchValue = value;
				pBestResearch = pSuper;
			}
		}

		if (pBestResearch && pBestResearch->CanFire())
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
					// TEMP diagnostic: the second tunnel mouth is dug here.
					Debug::Log("[NeuralAI] House %d Tunnel: %s fired at (%d,%d), score %d, home mouth %d leptons from home, frame %d.\n",
						pHouse->ArrayIndex, pSuper->Type->ID, bestTunnelCell.X, bestTunnelCell.Y, bestTunnelCellScore, minHomeTunnelDist,
						Unsorted::CurrentFrame);
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

					// The effect this weapon has on a unit type (Phobos IronCurtain.Effect / ForceShield.Effect,
					// organics default to [CombatDamage] IronCurtain.EffectOnOrganics / ForceShield.EffectOnOrganics).
					auto const effectOn = [swType](TechnoTypeClass* pType)
					{
						auto const pTypeExt = TechnoTypeExt::ExtMap.Find(pType);
						auto const pRules = RulesExt::Global();

						if (swType == SuperWeaponType::ForceShield)
						{
							return pTypeExt && pTypeExt->ForceShield_Effect.isset() ? pTypeExt->ForceShield_Effect.Get()
								: (pType->Organic ? pRules->ForceShield_EffectOnOrganics.Get() : IronCurtainEffect::Invulnerable);
						}

						return pTypeExt && pTypeExt->IronCurtain_Effect.isset() ? pTypeExt->IronCurtain_Effect.Get()
							: (pType->Organic ? pRules->IronCurtain_EffectOnOrganics.Get() : IronCurtainEffect::Invulnerable);
					};

					// Never where it would kill infantry of ours or an ally (within 2 cells).
					auto const killsFriends = [&](const CellStruct& cell)
					{
						for (auto const pInf : InfantryClass::Array)
						{
							if (pInf && pInf->IsAlive && !pInf->InLimbo && pInf->Owner && pHouse->IsAlliedWith(pInf->Owner)
								&& pInf->GetMapCoords().DistanceFrom(cell) <= 2.0 && effectOn(pInf->Type) == IronCurtainEffect::Kill)
							{
								return true;
							}
						}

						return false;
					};

					if (icTargetCell != CellStruct::Empty && killsFriends(icTargetCell))
						icTargetCell = CellStruct::Empty;

					// Desperate use: enemy infantry it kills that is about to capture, sabotage or
					// infiltrate (engineers, C4 commandos, spies) within 12 cells of our buildings: on the
					// cell with the most of them (the cost of the others it kills there counts too).
					if (icTargetCell == CellStruct::Empty)
					{
						int bestValue = 0;

						for (auto const pInf : InfantryClass::Array)
						{
							if (!pInf || !pInf->IsAlive || pInf->InLimbo || !pInf->Type || !pInf->Owner || pHouse->IsAlliedWith(pInf->Owner)
								|| pInf->Owner->IsNeutral() || !IsCellVisibleToAI(pHouse, pInf->GetCell())
								|| effectOn(pInf->Type) != IronCurtainEffect::Kill
								|| !(pInf->Type->Engineer || pInf->Type->C4 || pInf->Type->Infiltrate))
							{
								continue;
							}

							const CellStruct ic = pInf->GetMapCoords();
							bool nearOurs = false;

							for (auto const pOwnBld : pHouse->Buildings)
							{
								if (pOwnBld && pOwnBld->IsAlive && !pOwnBld->InLimbo && pOwnBld->GetMapCoords().DistanceFrom(ic) <= 12.0)
								{
									nearOurs = true;
									break;
								}
							}

							if (!nearOurs || killsFriends(ic))
								continue;

							int value = 0;

							for (auto const pOther : InfantryClass::Array)
							{
								if (pOther && pOther->IsAlive && !pOther->InLimbo && pOther->Owner && !pHouse->IsAlliedWith(pOther->Owner)
									&& pOther->GetMapCoords().DistanceFrom(ic) <= 1.5 && effectOn(pOther->Type) == IronCurtainEffect::Kill)
								{
									value += std::max(200, pOther->Type->Cost) * ((pOther->Type->Engineer || pOther->Type->C4 || pOther->Type->Infiltrate) ? 3 : 1);
								}
							}

							if (value > bestValue)
							{
								bestValue = value;
								icTargetCell = ic;
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
					CellStruct knownRockCell = CellStruct::Empty;

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

								// Rock (cliffs): no ground unit can stand there either.
								if (knownRockCell == CellStruct::Empty && pC->LandType == LandType::Rock && pC->FirstObject == nullptr)
									knownRockCell = tc;
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

						// Every candidate is scored and the best one is shifted (cost of what it destroys, an
						// enemy MCV first: a base it never builds). Units Chronoshift.Allow=no cannot be
						// shifted; a source cell with units of ours or an ally within 1 cell is skipped.
						auto const pINI = CCINIClass::INI_Rules;
						auto const shiftable = [pINI](TechnoTypeClass* pType)
						{
							return !pINI || pINI->ReadBool(pType->ID, "Chronoshift.Allow", true);
						};

						auto const hitsFriends = [&](const CellStruct& cell)
						{
							for (auto const pTech : TechnoClass::Array)
							{
								if (pTech && pTech->IsAlive && !pTech->InLimbo && pTech->Owner && pHouse->IsAlliedWith(pTech->Owner)
									&& pTech->WhatAmI() != AbstractType::Building && pTech->GetMapCoords().DistanceFrom(cell) <= 1.5)
								{
									return true;
								}
							}

							return false;
						};

						const CellStruct groundDeathCell = knownWaterCell != CellStruct::Empty ? knownWaterCell : knownRockCell;
						const bool killsOrganics = !pINI || pINI->ReadBool(pSuper->Type->ID, "Chronosphere.KillOrganic", true);
						int bestScore = 0;

						for (auto const pEnemyU : UnitClass::Array)
						{
							if (!pEnemyU || pEnemyU->InLimbo || !pEnemyU->IsAlive || !pEnemyU->Owner || !pEnemyU->Type)
								continue;

							if (pEnemyU->Owner == pHouse || pHouse->IsAlliedWith(pEnemyU->Owner) || pEnemyU->Owner->IsNeutral())
								continue;

							if (pEnemyU->IsIronCurtained() || pEnemyU->Type->Teleporter || pEnemyU->Type->Organic || !shiftable(pEnemyU->Type)
								|| pEnemyU->IsInAir())
							{
								continue;
							}

							auto const pECell = pEnemyU->GetCell();

							if (!pECell || !IsCellVisibleToAI(pHouse, pECell) || hitsFriends(pEnemyU->GetMapCoords()))
								continue;

							const bool isPureGroundUnit = !pEnemyU->Type->Naval && pEnemyU->Type->SpeedType != SpeedType::Hover
								&& pEnemyU->Type->MovementZone != MovementZone::Amphibious && !pEnemyU->Type->JumpJet;
							const bool isPureNavalShip = pEnemyU->Type->Naval && pEnemyU->Type->SpeedType != SpeedType::Hover
								&& pEnemyU->Type->MovementZone != MovementZone::Amphibious;
							const bool isMcv = pEnemyU->Type->DeploysInto && pEnemyU->Type->DeploysInto->ConstructionYard;
							const int cost = std::max(100, pEnemyU->Type->Cost);
							const int drownMinCost = std::clamp(700 - static_cast<int>(scores[55]), 450, 950);

							// Mode 3 (Slot 55): ground units (an MCV above all) into water or onto rock.
							if (isPureGroundUnit && groundDeathCell != CellStruct::Empty && (isMcv || cost >= drownMinCost))
							{
								const int score = isMcv ? cost * 3 + 2000 : cost;

								if (score > bestScore)
								{
									bestScore = score;
									chronoSrc = pEnemyU->GetMapCoords();
									chronoDst = groundDeathCell;
								}
							}

							// Mode 4: ships onto dry land.
							if (isPureNavalShip && knownDryLandCell != CellStruct::Empty && cost > bestScore)
							{
								bestScore = cost;
								chronoSrc = pEnemyU->GetMapCoords();
								chronoDst = knownDryLandCell;
							}

							// Mode 2: ground units of one enemy into the base of another (they fight there).
							if (isPureGroundUnit && rivalEnemyBaseCell != CellStruct::Empty
								&& pEnemyU->Owner->ArrayIndex != rivalEnemyHouseIdx && cost * 6 / 10 > bestScore)
							{
								bestScore = cost * 6 / 10;
								chronoSrc = pEnemyU->GetMapCoords();
								chronoDst = rivalEnemyBaseCell;
							}
						}

						// Mode 5: enemy infantry it kills (Chronosphere.KillOrganic, not Chronoshift.IsVehicle):
						// the densest group (3 or more within 1 cell), sent anywhere.
						if (killsOrganics && knownDryLandCell != CellStruct::Empty)
						{
							for (auto const pInf : InfantryClass::Array)
							{
								if (!pInf || !pInf->IsAlive || pInf->InLimbo || !pInf->Type || !pInf->Owner || pHouse->IsAlliedWith(pInf->Owner)
									|| pInf->Owner->IsNeutral() || !IsCellVisibleToAI(pHouse, pInf->GetCell()))
								{
									continue;
								}

								const CellStruct ic = pInf->GetMapCoords();
								int count = 0;
								int value = 0;

								for (auto const pOther : InfantryClass::Array)
								{
									if (pOther && pOther->IsAlive && !pOther->InLimbo && pOther->Type && pOther->Owner && !pHouse->IsAlliedWith(pOther->Owner)
										&& pOther->GetMapCoords().DistanceFrom(ic) <= 1.5 && shiftable(pOther->Type)
										&& !(pINI && pINI->ReadBool(pOther->Type->ID, "Chronoshift.IsVehicle", false)))
									{
										count++;
										value += std::max(100, pOther->Type->Cost);
									}
								}

								if (count >= 3 && value > bestScore && !hitsFriends(ic))
								{
									bestScore = value;
									chronoSrc = ic;
									chronoDst = knownDryLandCell;
								}
							}
						}
					}

					// Mode 6: strategic insertion. With nothing better, a group of 5+ of our armed vehicles
					// standing together goes next to the known enemy refinery, factory or ConYard with the
					// fewest known defenses around it (at most 1 within 8 cells).
					if (chronoSrc == CellStruct::Empty)
					{
						CellStruct groupCell = CellStruct::Empty;
						int groupSize = 0;

						for (auto const pU : UnitClass::Array)
						{
							if (!pU || pU->Owner != pHouse || !pU->IsAlive || pU->InLimbo || !pU->Type || pU->Type->Naval
								|| pU->Type->Harvester || pU->Type->Organic || GroundRangeOf(pU->Type) <= 0 || IsAirborneThreatTechno(pU))
							{
								continue;
							}

							int around = 0;

							for (auto const pOther : UnitClass::Array)
							{
								if (pOther && pOther->Owner == pHouse && pOther->IsAlive && !pOther->InLimbo && pOther->Type && !pOther->Type->Naval
									&& !pOther->Type->Organic && GroundRangeOf(pOther->Type) > 0 && pOther->GetMapCoords().DistanceFrom(pU->GetMapCoords()) <= 1.5)
								{
									around++;
								}
							}

							if (around > groupSize)
							{
								groupSize = around;
								groupCell = pU->GetMapCoords();
							}
						}

						if (groupSize >= 5)
						{
							BuildingClass* pWeakest = nullptr;
							int weakestDefenses = INT_MAX;

							for (auto const pEnemyBld : GetKnownEnemyBuildingCache(pHouse).Buildings)
							{
								if (!pEnemyBld || !pEnemyBld->IsAlive || !pEnemyBld->Type
									|| !(pEnemyBld->Type->Refinery || pEnemyBld->Type->ConstructionYard || pEnemyBld->Type->Factory != AbstractType::None))
								{
									continue;
								}

								const int defenses = CountKnownEnemyDefensesNear(pHouse, pEnemyBld->GetMapCoords(), 8.0, false);

								if (defenses < weakestDefenses && MaySuperweaponHit(pHouse, pEnemyBld->GetMapCoords()))
								{
									weakestDefenses = defenses;
									pWeakest = pEnemyBld;
								}
							}

							if (pWeakest && weakestDefenses <= 1)
							{
								if (auto const pDrop = FindSafeIslandLandingCell(pHouse, pWeakest->GetMapCoords(), -1, false))
								{
									chronoSrc = groupCell;
									chronoDst = pDrop->MapCoords;
								}
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

				// E) Reconnaissance Superweapons (Psychic Reveal / Spy Plane): where the revealed area
				// teaches the most (PickRevealTarget), else they wait. A rectangle counts as the disc
				// of its mean half side, a spy plane's look as 10 cells; a reveal of the whole map
				// goes anywhere. A SpyPlane type that sends an air strike (SpyPlane.Mission=Attack)
				// is aimed as an offensive one below.
				if (swType == SuperWeaponType::PsychicReveal
					|| (swType == SuperWeaponType::SpyPlane && GetSWTargeting(pSuper->Type).SpyPlaneLooks))
				{
					int height = -1;
					const double range = swType == SuperWeaponType::PsychicReveal ? GetPsychicRevealRange(pSuper->Type, height) : 10.0;
					const CellStruct anyCell = pFallbackEnemyCell
						? pFallbackEnemyCell->MapCoords
						: (pVisibleEnemyBld ? pVisibleEnemyBld->GetMapCoords() : homeCoords);
					const CellStruct reconCell = range < 0.0
						? anyCell
						: PickRevealTarget(pHouse, static_cast<int>(height > 0 ? (range + height) / 4.0 : range));

					if (reconCell != CellStruct::Empty)
					{
						pSuper->Launch(reconCell, false);
						pSuper->Reset();
						break;
					}

					continue;
				}

				// G) Stealth reveal (SW.AITargeting=Stealth, e.g. a sonar pulse): on the cell with the most
				// stealthy enemy units seen in the last 3000 frames within its range (one seen in the
				// last 1800 frames at least), when our armed units are within its range + 6 to use the
				// reveal or the spot is within 30 cells of home. SW.RequiresTarget=water and
				// SW.FireIntoShroud=no are respected.
				const auto& swAim = GetSWTargeting(pSuper->Type);

				if (swAim.Mode == "stealth")
				{
					auto& sightings = GetMacroState(pHouse).StealthSightings;
					std::erase_if(sightings, [](const auto& e) { return Unsorted::CurrentFrame - e.second.second > 3000; });
					const double range = std::max(3.0, swAim.Range);
					CellStruct bestCell = CellStruct::Empty;
					int bestCount = 0;

					for (auto const& [uid, seen] : sightings)
					{
						if (Unsorted::CurrentFrame - seen.second > 1800)
							continue;

						auto const pCell = MapClass::Instance.TryGetCellAt(seen.first);

						if (!pCell || (swAim.RequiresWater && pCell->LandType != LandType::Water)
							|| (!swAim.FireIntoShroud && !IsCellVisibleToAI(pHouse, pCell)))
						{
							continue;
						}

						int count = 0;

						for (auto const& [otherID, other] : sightings)
						{
							if (other.first.DistanceFrom(seen.first) <= range)
								count++;
						}

						if (count > bestCount || (count == bestCount && bestCell != CellStruct::Empty && seen.first.X + seen.first.Y < bestCell.X + bestCell.Y))
						{
							bestCount = count;
							bestCell = seen.first;
						}
					}

					bool useful = bestCell != CellStruct::Empty
						&& homeCoords != CellStruct::Empty && bestCell.DistanceFrom(homeCoords) <= 30.0;

					if (bestCell != CellStruct::Empty && !useful)
					{
						for (auto const pOwnU : UnitClass::Array)
						{
							if (pOwnU && pOwnU->Owner == pHouse && pOwnU->IsAlive && !pOwnU->InLimbo && pOwnU->Type
								&& GroundRangeOf(pOwnU->Type) > 0 && pOwnU->GetMapCoords().DistanceFrom(bestCell) <= range + 6.0)
							{
								useful = true;
								break;
							}
						}
					}

					if (useful && MaySuperweaponHit(pHouse, bestCell))
					{
						pSuper->Launch(bestCell, false);
						pSuper->Reset();
						CountDecision(NeuralDecision::StealthRevealFired, pHouse->ArrayIndex);
						break;
					}

					continue;
				}

				// Modes that do not aim at the enemy, fired as Ares fires them (Antares
				// Ext/SWType/Hooks.Targeting.cpp): NoTarget at once, Attack while our base is under
				// attack, LowPower under low power, LowPowerAttack with both (the cell does not matter:
				// our base); Base on our base center; Self on the building that grants it. None,
				// IronCurtain and ForceShield (own branches for the vanilla ones) are kept.
				if (!IsOffensiveSWTargetingMode(swAim.Mode))
				{
					const bool attacked = pHouse->LATime > 0 && Unsorted::CurrentFrame - pHouse->LATime < 1350;
					const bool lowPower = pHouse->HasLowPower();
					CellStruct ownCell = CellStruct::Empty;

					if (swAim.Mode == "notarget" || (swAim.Mode == "attack" && attacked) || (swAim.Mode == "lowpower" && lowPower)
						|| (swAim.Mode == "lowpowerattack" && attacked && lowPower) || swAim.Mode == "base")
					{
						ownCell = homeCoords;
					}
					else if (swAim.Mode == "self")
					{
						for (auto const pOwnBld : pHouse->Buildings)
						{
							if (!pOwnBld || !pOwnBld->IsAlive || pOwnBld->InLimbo || !pOwnBld->Type || !pOwnBld->IsPowerOnline())
								continue;

							auto const pBldExt = BuildingTypeExt::ExtMap.Find(pOwnBld->Type);

							for (int i = 0; pBldExt && i < pBldExt->GetSuperWeaponCount(); ++i)
							{
								if (pBldExt->GetSuperWeaponIndex(i) == pSuper->Type->ArrayIndex)
								{
									ownCell = pOwnBld->GetMapCoords();
									break;
								}
							}

							if (ownCell != CellStruct::Empty)
								break;
						}
					}

					if (ownCell != CellStruct::Empty)
					{
						pSuper->Launch(ownCell, false);
						pSuper->Reset();
						break;
					}

					continue;
				}

				// Ring-limited superweapons (SW.RangeMinimum / SW.RangeMaximum, e.g. an EMP cannon): the
				// target has to be inside the ring of one of our launch sites (powered). Among the known
				// enemy buildings and enemy units in sight inside a ring, the cell whose SW.Range holds the
				// most enemy value (buildings by cost, units by cost) is hit; with nothing in reach the
				// weapon is kept.
				if (swAim.RangeMax > 0.0)
				{
					std::vector<CellStruct> sites;

					for (auto const pOwnBld : pHouse->Buildings)
					{
						if (!pOwnBld || !pOwnBld->IsAlive || pOwnBld->InLimbo || !pOwnBld->Type || !pOwnBld->IsPowerOnline())
							continue;

						bool isSite = false;

						if (!swAim.Cannons.empty())
						{
							isSite = std::any_of(swAim.Cannons.begin(), swAim.Cannons.end(),
								[pOwnBld](const std::string& id) { return _stricmp(id.c_str(), pOwnBld->Type->ID) == 0; });
						}
						else if (swAim.IsEMPulse)
						{
							isSite = pOwnBld->Type->EMPulseCannon;
						}
						else if (auto const pBldExt = BuildingTypeExt::ExtMap.Find(pOwnBld->Type))
						{
							for (int i = 0; i < pBldExt->GetSuperWeaponCount() && !isSite; ++i)
								isSite = pBldExt->GetSuperWeaponIndex(i) == pSuper->Type->ArrayIndex;
						}

						if (isSite)
							sites.push_back(pOwnBld->GetMapCoords());
					}

					auto const inRing = [&](const CellStruct& c)
					{
						return std::any_of(sites.begin(), sites.end(), [&](const CellStruct& s)
							{
								const double d = s.DistanceFrom(c);
								return d <= swAim.RangeMax && d >= std::max(0.0, swAim.RangeMin);
							});
					};

					struct EnemyMark
					{
						CellStruct Cell;
						int Value;
					};

					std::vector<EnemyMark> marks;

					for (auto const pEnemyBld : GetKnownEnemyBuildingCache(pHouse).Buildings)
					{
						if (pEnemyBld && pEnemyBld->IsAlive && !pEnemyBld->InLimbo && pEnemyBld->Type && !pEnemyBld->Type->Insignificant)
							marks.push_back({ pEnemyBld->GetMapCoords(), std::max(100, pEnemyBld->Type->Cost) });
					}

					for (auto const pEnemyU : UnitClass::Array)
					{
						if (pEnemyU && pEnemyU->IsAlive && !pEnemyU->InLimbo && pEnemyU->Type && pEnemyU->Owner && pEnemyU->Owner != pHouse
							&& !pHouse->IsAlliedWith(pEnemyU->Owner) && !pEnemyU->Owner->IsNeutral()
							&& IsCellKnownToAI(pHouse, pEnemyU->GetMapCoords()))
						{
							// An EMP weapon is a defense first: enemy units closing on our base count double.
							const bool threat = swAim.IsEMP && homeCoords != CellStruct::Empty
								&& pEnemyU->GetMapCoords().DistanceFrom(homeCoords) <= 35.0;
							marks.push_back({ pEnemyU->GetMapCoords(), std::max(100, pEnemyU->Type->Cost) * (threat ? 2 : 1) });
						}
					}

					const double blast = std::max(1.0, swAim.Range);
					CellStruct bestCell = CellStruct::Empty;
					int bestValue = 0;

					for (auto const& mark : marks)
					{
						auto const pMarkCell = MapClass::Instance.TryGetCellAt(mark.Cell);

						if (!pMarkCell || !inRing(mark.Cell) || (!swAim.FireIntoShroud && !IsCellVisibleToAI(pHouse, pMarkCell))
							|| !MaySuperweaponHit(pHouse, mark.Cell))
						{
							continue;
						}

						int value = 0;

						for (auto const& other : marks)
						{
							if (other.Cell.DistanceFrom(mark.Cell) <= blast)
								value += other.Value;
						}

						if (value > bestValue || (value == bestValue && bestCell != CellStruct::Empty
							&& (mark.Cell.X < bestCell.X || (mark.Cell.X == bestCell.X && mark.Cell.Y < bestCell.Y))))
						{
							bestValue = value;
							bestCell = mark.Cell;
						}
					}

					if (bestCell != CellStruct::Empty)
					{
						pSuper->Launch(bestCell, false);
						pSuper->Reset();

						// After an EMP, idle armed ground units within 25 cells (close enough to arrive
						// while the paralysis lasts; not in plans or capital groups; nearest first)
						// attack-move into the paralysed enemies until their cost matches the enemy value
						// hit (20 units at most).
						if (swAim.IsEMP)
						{
							auto& empMacro = GetMacroState(pHouse);
							std::vector<std::pair<int, UnitClass*>> followers;

							for (auto const pOwnU : UnitClass::Array)
							{
								if (!pOwnU || pOwnU->Owner != pHouse || !pOwnU->IsAlive || pOwnU->InLimbo || !pOwnU->Type || pOwnU->Type->Naval
									|| pOwnU->Type->Harvester || pOwnU->Type->ResourceGatherer || pOwnU->Type->DeploysInto
									|| IsAirborneThreatTechno(pOwnU) || GroundRangeOf(pOwnU->Type) <= 0 || pOwnU->Target != nullptr
									|| empMacro.PlanMembers.contains(pOwnU->UniqueID) || empMacro.CapitalHeld.contains(pOwnU->UniqueID)
									|| (pOwnU->CurrentMission != Mission::Guard && pOwnU->CurrentMission != Mission::Area_Guard))
								{
									continue;
								}

								const int d = static_cast<int>(pOwnU->GetMapCoords().DistanceFrom(bestCell));

								if (d <= 25)
									followers.emplace_back(d, pOwnU);
							}

							std::sort(followers.begin(), followers.end(), [](const auto& a, const auto& b)
								{
									return a.first != b.first ? a.first < b.first : a.second->UniqueID < b.second->UniqueID;
								});

							auto const pStrikeCell = MapClass::Instance.TryGetCellAt(bestCell);

							int sentCost = 0;

							for (size_t i = 0; pStrikeCell && i < followers.size() && i < 20 && sentCost < bestValue; ++i)
							{
								OrderFootToAttackMove(followers[i].second, pStrikeCell);
								sentCost += followers[i].second->Type->Cost;
							}
						}

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


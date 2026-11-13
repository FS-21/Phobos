// NeuralAI.Tactics.inl: Watchdogs (air losses, stuck units, idle harvesters, offensive stones) and match tactics.
// Part of NeuralAI.cpp (included in order; one translation unit).

namespace
{
	// TEMP diagnostic: technos wrongly linked from a cell. Seven of the eleven crashes of
	// 2026-11-11..13 are the engine walking the objects of a cell (TechnoClass::Evaluate_Cell picking
	// a target, Can_Enter_Cell finding a path) and calling into one that was already destroyed. The
	// object lists of every cell are checked every CellGhostCheckFrames for two things, logged with
	// what can be read of the object so the match log tells which kind of unit it is and when:
	// - an object that is not a live techno and has no vtable of a map object: a destroyed techno
	//   (its destructors leave it the vtable of a base class) or freed memory;
	// - a live mobile unit linked from a cell it does not stand on, or linked while in limbo: the
	//   state a unit has to be in for its death to leave its cell pointing at it.
	// One check walks every cell of the map (about 10 ms): every 30 frames it was 4% of the AI tick
	// cost and found nothing in two days, so it runs rarely while the crash is set aside.
	constexpr int CellGhostCheckFrames = 900;
	constexpr int CellGhostLogsPerMatch = 12;
	constexpr int CellGhostChainSteps = 64;
	constexpr int CellGhostTypeSlots = 4;

	// Vtables of the base classes of a techno (abstract, object, mission, radio, techno, foot).
	constexpr uintptr_t TechnoBaseVTables[] = { 0x7E1F50, 0x7EF060, 0x7EDCC0, 0x7F0508, 0x7F4960, 0x7E8C94 };

	// Read-only tables of the game image: the vtable of any real object lies there.
	constexpr uintptr_t ImageDataStart = 0x7E0000;
	constexpr uintptr_t ImageDataEnd = 0x840000;

	struct NeuralCellGhostInfo
	{
		uintptr_t VTable;
		ObjectClass* Next;
		void* Types[CellGhostTypeSlots];
		void* Owner;
		DWORD UniqueID;
		int Health;
		int Mission;
		int CellX;
		int CellY;
		bool HeadRead;
		bool IsAlive;
		bool InLimbo;
		bool OnBridge;
	};

	// Reads an object that may be freed memory. Plain data only in here: structured exception
	// handling does not mix with objects that have destructors. The type is read at the place each
	// kind of techno keeps it, the caller tells which one is a real type.
	bool ReadCellGhost(ObjectClass* pObject, NeuralCellGhostInfo* pInfo)
	{
		__try
		{
			pInfo->VTable = *reinterpret_cast<uintptr_t*>(pObject);
			pInfo->Next = pObject->NextObject;
			pInfo->HeadRead = true;

			auto const pTechno = static_cast<TechnoClass*>(pObject);
			pInfo->UniqueID = pObject->UniqueID;
			pInfo->Health = pObject->Health;
			pInfo->IsAlive = pObject->IsAlive;
			pInfo->InLimbo = pObject->InLimbo;
			pInfo->OnBridge = pObject->OnBridge;
			pInfo->CellX = pObject->Location.X / Unsorted::LeptonsPerCell;
			pInfo->CellY = pObject->Location.Y / Unsorted::LeptonsPerCell;
			pInfo->Owner = pTechno->Owner;
			pInfo->Mission = static_cast<int>(pTechno->CurrentMission);
			pInfo->Types[0] = static_cast<UnitClass*>(pObject)->Type;
			pInfo->Types[1] = static_cast<InfantryClass*>(pObject)->Type;
			pInfo->Types[2] = static_cast<AircraftClass*>(pObject)->Type;
			pInfo->Types[3] = static_cast<BuildingClass*>(pObject)->Type;

			return true;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return pInfo->HeadRead;
		}
	}

	void CheckCellGhosts()
	{
		const NeuralSlowStep slowStep { "CheckCellGhosts" };

		static int serial = -1;
		static int lastFrame = -1;
		static int logged = 0;
		static std::unordered_set<DWORD> reported;
		const int now = Unsorted::CurrentFrame;

		if (serial != NeuralMatchSerial)
		{
			serial = NeuralMatchSerial;
			lastFrame = -1;
			logged = 0;
			reported.clear();
		}

		if (logged >= CellGhostLogsPerMatch || (lastFrame >= 0 && now >= lastFrame && now - lastFrame < CellGhostCheckFrames))
			return;

		lastFrame = now;
		std::unordered_set<const void*> live;
		live.reserve(static_cast<size_t>(TechnoClass::Array.Count) * 2);

		for (auto const pTechno : TechnoClass::Array)
			live.insert(pTechno);

		for (int i = 0; i < MapClass::Instance.Cells.Capacity && logged < CellGhostLogsPerMatch; ++i)
		{
			auto const pCell = MapClass::Instance.Cells.Items[i];

			if (!pCell)
				continue;

			for (int list = 0; list < 2; ++list)
			{
				ObjectClass* pObject = list == 0 ? pCell->FirstObject : pCell->AltObject;
				const char* listName = list == 0 ? "ground" : "bridge";

				for (int step = 0; pObject && step < CellGhostChainSteps; ++step)
				{
					if (live.contains(pObject))
					{
						auto const pTechno = static_cast<TechnoClass*>(pObject);

						if (pTechno->WhatAmI() != AbstractType::Building && !reported.contains(pTechno->UniqueID))
						{
							const CellStruct own = CellClass::Coord2Cell(pTechno->Location);
							const bool elsewhere = own != pCell->MapCoords;

							if (elsewhere || pTechno->InLimbo || !pTechno->IsAlive)
							{
								reported.insert(pTechno->UniqueID);
								++logged;
								auto const pCarrier = pTechno->Transporter;
								auto const pExt = TechnoExt::Fetch(pTechno);

								Debug::Log("[NeuralAI] Cell link DIAG frame %d: cell (%d,%d) %s list, link %d, holds the live %s of house %d, id %u, %s: "
									"its own cell (%d,%d), height %d, alive %d, limbo %d, on map %d, on bridge %d, mission %d, transporter %s, on carryall %d, attached %d.\n",
									now, pCell->MapCoords.X, pCell->MapCoords.Y, listName, step, pTechno->GetTechnoType()->ID,
									pTechno->Owner ? pTechno->Owner->ArrayIndex : -1, pTechno->UniqueID,
									pTechno->InLimbo ? "that is in limbo" : (elsewhere ? "that stands on another cell" : "that is not alive"),
									own.X, own.Y, pTechno->GetHeight(), pTechno->IsAlive ? 1 : 0, pTechno->InLimbo ? 1 : 0, pTechno->IsOnMap ? 1 : 0,
									pTechno->OnBridge ? 1 : 0, static_cast<int>(pTechno->CurrentMission),
									pCarrier && live.contains(pCarrier) ? pCarrier->GetTechnoType()->ID : (pCarrier ? "?" : "-"),
									pTechno->IsOnCarryall ? 1 : 0, pExt && pExt->ParentAttachment ? 1 : 0);
							}
						}

						pObject = pObject->NextObject;
						continue;
					}

					NeuralCellGhostInfo info {};
					const bool readable = ReadCellGhost(pObject, &info);
					const bool techno = readable && (info.VTable == UnitClass::AbsVTable || info.VTable == InfantryClass::AbsVTable
						|| info.VTable == AircraftClass::AbsVTable || info.VTable == BuildingClass::AbsVTable);
					bool destructed = false;

					for (const uintptr_t vtable : TechnoBaseVTables)
						destructed = destructed || (readable && info.VTable == vtable);

					// Terrain objects and the like: not technos, their chain goes on.
					if (readable && !techno && !destructed && info.VTable >= ImageDataStart && info.VTable < ImageDataEnd)
					{
						pObject = info.Next;
						continue;
					}

					++logged;
					const char* typeID = "?";
					int owner = -1;
					constexpr AbstractType slotKinds[CellGhostTypeSlots] = { AbstractType::UnitType, AbstractType::InfantryType,
						AbstractType::AircraftType, AbstractType::BuildingType };

					for (auto const pType : TechnoTypeClass::Array)
					{
						for (int slot = 0; slot < CellGhostTypeSlots; ++slot)
						{
							if (pType == info.Types[slot] && pType->WhatAmI() == slotKinds[slot])
								typeID = pType->ID;
						}
					}

					for (auto const pOwner : HouseClass::Array)
					{
						if (pOwner == info.Owner)
						{
							owner = pOwner->ArrayIndex;
							break;
						}
					}

					Debug::Log("[NeuralAI] Cell ghost DIAG frame %d: cell (%d,%d) %s list, link %d, holds %p (%s, vtable %08X) that is not in the techno array: "
						"type %s, house %d, id %u, alive %d, limbo %d, on bridge %d, health %d, mission %d, its own cell (%d,%d).\n",
						now, pCell->MapCoords.X, pCell->MapCoords.Y, listName, step, pObject,
						!readable ? "unreadable memory" : (techno ? "a techno" : (destructed ? "a destroyed techno" : "reused memory")),
						static_cast<unsigned>(info.VTable), typeID, owner, info.UniqueID, info.IsAlive ? 1 : 0, info.InLimbo ? 1 : 0,
						info.OnBridge ? 1 : 0, info.Health, info.Mission, info.CellX, info.CellY);

					break;
				}
			}
		}
	}

	// Air danger memory: every AI tick, our flying units in the air are remembered; one that
	// vanished since the last tick (not alive any more, not just loaded or landed) away from our
	// base was shot down, so its last cell is an anti-air danger spot for 8100 frames. Air lanes
	// (FindSafeFerryWaypoint) avoid those spots even when the shooter is no longer visible.
	void UpdateAirLossMemory(HouseClass* pHouse)
	{
		if (!pHouse)
			return;

		auto& macro = GetMacroState(pHouse);
		std::unordered_set<DWORD> alive;
		std::vector<NeuralAirLossTrack> next;

		for (auto const pTech : TechnoClass::Array)
		{
			if (!pTech || pTech->Owner != pHouse || !pTech->IsAlive)
				continue;

			auto const pFoot = abstract_cast<FootClass*>(pTech);

			if (!pFoot)
				continue;

			alive.insert(pFoot->UniqueID);

			if (pFoot->InLimbo || !pFoot->IsInAir())
				continue;

			bool isCarrier = false;

			if (auto const pAir = abstract_cast<AircraftClass*>(pFoot))
			{
				if (!pAir->Type || pAir->Type->Spawned)
					continue;

				isCarrier = pAir->Type->Carryall || pAir->Type->Passengers > 0;
			}
			else if (auto const pUnit = abstract_cast<UnitClass*>(pFoot))
			{
				auto const pTypeExt = pUnit->Type ? TechnoTypeExt::Fetch(pUnit->Type) : nullptr;
				isCarrier = (pTypeExt && pTypeExt->Carryall) || (pUnit->Type && IsAmphibiousOrAirTransport(pUnit->Type));
			}

			next.push_back({ pFoot->UniqueID, pFoot->GetMapCoords(), isCarrier });
		}

		for (auto const& track : macro.AirLossTracks)
		{
			if (alive.contains(track.ID) || track.LastCoords == CellStruct::Empty)
				continue;

			bool nearHome = false;

			for (auto const pBld : pHouse->Buildings)
			{
				if (pBld && pBld->IsAlive && !pBld->InLimbo && pBld->GetMapCoords().DistanceFrom(track.LastCoords) <= 10.0)
				{
					nearHome = true;
					break;
				}
			}

			if (nearHome)
				continue;

			macro.AirDangerSpots.emplace_back(track.LastCoords, Unsorted::CurrentFrame);

			if (track.IsCarrier)
				CountDecision(NeuralDecision::AirCarrierLost, pHouse->ArrayIndex);
		}

		std::erase_if(macro.AirDangerSpots, [](const auto& spot)
			{ return spot.second > Unsorted::CurrentFrame || Unsorted::CurrentFrame - spot.second >= 8100; });

		if (macro.AirDangerSpots.size() > 24)
		{
			macro.AirDangerSpots.erase(macro.AirDangerSpots.begin(),
				macro.AirDangerSpots.begin() + static_cast<std::ptrdiff_t>(macro.AirDangerSpots.size() - 24));
		}

		macro.AirLossTracks = std::move(next);
	}

	// A unit of ours held on one cell with an order pending, where it stands and where it wants to go.
	struct NeuralHeldOrder
	{
		FootClass* Foot { nullptr };
		CellStruct Cell { CellStruct::Empty };
		CellStruct Dest { CellStruct::Empty };
	};

	// A unit of ours that has stood on one cell for `frames` (UpdateStuckUnits keeps the record for
	// every ground unit) without firing in the last 300 frames and without a target within reach:
	// whatever its order says it is doing nothing, so the "busy" filters (attacking, at the front) let
	// other jobs take it. Before, a unit with a live target out of reach, or anywhere within 18.5 cells
	// of an enemy building (bases next to each other), was busy for good.
	bool IsFootHeldStill(const HouseClass* pHouse, const FootClass* pFoot, int frames)
	{
		if (!pHouse || !pFoot || pFoot->IsFiring)
			return false;

		const int now = Unsorted::CurrentFrame;

		if (pFoot->LastFireBulletFrame > 0 && now >= pFoot->LastFireBulletFrame && now - pFoot->LastFireBulletFrame < 300)
			return false;

		auto const& progress = GetMacroState(pHouse).FootProgress;
		auto const it = progress.find(pFoot->UniqueID);

		if (it == progress.end() || now < it->second.SinceFrame || now - it->second.SinceFrame < frames)
			return false;

		return !(pFoot->Target && pFoot->IsCloseEnoughToAttack(pFoot->Target));
	}

	bool IsBridgeOrTubeCell(const CellClass* pCell)
	{
		return pCell && (pCell->ContainsBridge() || pCell->ContainsBridgeEx() || pCell->TubeIndex >= 0);
	}

	// A narrow passage: a bridge deck, bridge head or tunnel tube within 1 cell, or fewer than 25 of
	// the 7x7 cells around free ground (no building, water or cliff).
	bool IsNarrowPassage(const CellStruct& center)
	{
		int freeCells = 0;

		for (int dy = -3; dy <= 3; ++dy)
		{
			for (int dx = -3; dx <= 3; ++dx)
			{
				auto const pCell = MapClass::Instance.TryGetCellAt(
					CellStruct { static_cast<short>(center.X + dx), static_cast<short>(center.Y + dy) });

				if (!pCell)
					continue;

				if (std::abs(dx) <= 1 && std::abs(dy) <= 1 && IsBridgeOrTubeCell(pCell))
					return true;

				if (!pCell->GetBuilding() && pCell->LandType != LandType::Water
					&& pCell->IsClearToMove(SpeedType::Track, true, true, -1, MovementZone::Normal, -1, false))
				{
					++freeCells;
				}
			}
		}

		return freeCells < 25;
	}

	// Idle for the jam breaker and the mouth clearing: a ground unit of ours on Guard / Area Guard /
	// Sleep with no target, not firing, not a gatherer, not deployed, not in a plan.
	bool IsIdleBlocker(HouseClass* pHouse, FootClass* pFoot)
	{
		if (!pFoot || pFoot->Owner != pHouse || !pFoot->IsAlive || pFoot->InLimbo || pFoot->IsInAir() || pFoot->Transporter
			|| pFoot->Target || pFoot->IsFiring)
		{
			return false;
		}

		auto const pType = pFoot->GetTechnoType();
		auto const pUnit = abstract_cast<UnitClass*>(pFoot);

		if (!pType || pType->JumpJet || pType->MovementZone == MovementZone::Fly || pType->ResourceGatherer
			|| (pUnit && (pUnit->Deployed || (pUnit->Type && pUnit->Type->Harvester))))
		{
			return false;
		}

		if (pFoot->CurrentMission != Mission::Guard && pFoot->CurrentMission != Mission::Area_Guard
			&& pFoot->CurrentMission != Mission::Sleep)
		{
			return false;
		}

		return !GetMacroState(pHouse).PlanMembers.contains(pFoot->UniqueID);
	}

	// Free ground for a unit moved out of the way: a cell of its zone on the rings minRing..maxRing
	// (step 2) around `center`, not a bridge or tube cell, scored by the units already near it, cells
	// already handed out and, when (prefX, prefY) is set, going that way (cost up to 200). With
	// offMouths, never a passage mouth cell either (a unit moved out of the way does not park in the
	// next mouth).
	CellClass* FindClearingCell(FootClass* pFoot, const CellStruct& center, int minRing, int maxRing, double prefX, double prefY,
		const std::vector<CellStruct>& occupied, const std::vector<CellStruct>& taken, bool offMouths = false)
	{
		auto const pType = pFoot ? pFoot->GetTechnoType() : nullptr;

		if (!pType)
			return nullptr;

		const int zone = ObjectZone(pFoot, pType->MovementZone);
		// Ships make room on water (they never found a cell: water was always skipped, so a two-way
		// jam of submarines in a strait was never broken, match repro on Fludded Recources).
		const bool naval = pType->Naval || pType->MovementZone == MovementZone::Water
			|| pType->MovementZone == MovementZone::WaterBeach;
		CellClass* pBest = nullptr;
		int bestScore = INT_MAX;

		for (int r = minRing; r <= maxRing; r += 2)
		{
			for (int dy = -r; dy <= r; dy += 2)
			{
				for (int dx = -r; dx <= r; dx += 2)
				{
					if (std::abs(dx) != r && std::abs(dy) != r)
						continue;

					const CellStruct c { static_cast<short>(center.X + dx), static_cast<short>(center.Y + dy) };
					auto const pCell = MapClass::Instance.TryGetCellAt(c);

					if (!pCell || pCell->GetBuilding() || (pCell->LandType == LandType::Water) != naval || IsBridgeOrTubeCell(pCell)
						|| (offMouths && IsPassageMouthCell(c))
						|| MapClass::Instance.GetMovementZoneType(c, pType->MovementZone, false) != zone
						|| !pCell->IsClearToMove(pType->SpeedType, true, true, -1, pType->MovementZone, -1, false))
					{
						continue;
					}

					int crowd = 0;

					for (auto const& o : occupied)
					{
						if (std::abs(o.X - c.X) <= 2 && std::abs(o.Y - c.Y) <= 2)
							++crowd;
					}

					for (auto const& tk : taken)
					{
						if (std::abs(tk.X - c.X) <= 1 && std::abs(tk.Y - c.Y) <= 1)
							crowd += 4;
					}

					const double dot = (dx * prefX + dy * prefY) / std::max(1.0, std::sqrt(static_cast<double>(dx * dx + dy * dy)));
					const int score = crowd * 30 + static_cast<int>(std::max(0.0, dot) * 200.0) + r * 5;

					if (score < bestScore)
					{
						bestScore = score;
						pBest = pCell;
					}
				}
			}
		}

		return pBest;
	}

	// A held unit whose order ends within this many cells of its block's centre wants to stand in it.
	constexpr double JamInsideCells = 6.0;

	CellClass* JamRerouteHop(FootClass* pFoot, const CellStruct& goal);

	// Jam breaker: our units held 450+ frames with an order pending, 8+ within 4 cells of each other in
	// open ground or 5+ in a narrow passage (bridge, tunnel tube, a corridor with fewer than 25 free
	// cells in 7x7), are a block (a rally point in a gap, two-way traffic at a choke). Looked for every
	// 60 frames, cleared at most once per 300:
	// - our idle ground units within 6 cells of it are moved out to free ground 8..14 cells away,
	//   preferably not toward where the held units want to go (up to 16);
	// - held units outside a plan whose move ends inside the block leave it the same way;
	// - in a narrow passage with traffic both ways, the smaller side backs off (orders dropped, 8..14
	//   cells away on its own side; the AI gives it a new job) so the other side gets through;
	// - a narrow passage where blocks are still found JamRerouteFrames after the first one is gone
	//   around: half of the units whose way leads past it, then all of them (JamRerouteHop).
	void UpdateJamBreaker(HouseClass* pHouse, const std::vector<NeuralHeldOrder>& held)
	{
		const NeuralSlowStep slowStep { "UpdateJamBreaker" };

		auto& macro = GetMacroState(pHouse);
		const int now = Unsorted::CurrentFrame;

		if (held.size() < 5 || (macro.LastJamBreakFrame >= 0 && now >= macro.LastJamBreakFrame && now - macro.LastJamBreakFrame < 300)
			|| (macro.LastJamCheckFrame >= 0 && now >= macro.LastJamCheckFrame && now - macro.LastJamCheckFrame < 60))
		{
			return;
		}

		macro.LastJamCheckFrame = now;

		auto const inBlock = [](const CellStruct& a, const CellStruct& b)
		{ return std::abs(a.X - b.X) <= 4 && std::abs(a.Y - b.Y) <= 4; };

		// The block with the most held units over what it takes there.
		size_t bestIdx = held.size();
		double bestRatio = 0.0;
		bool narrow = false;

		for (size_t i = 0; i < held.size(); ++i)
		{
			int count = 0;

			for (auto const& other : held)
			{
				if (inBlock(other.Cell, held[i].Cell))
					++count;
			}

			if (count < 5)
				continue;

			const bool isNarrow = IsNarrowPassage(held[i].Cell);
			const int need = isNarrow ? 5 : 8;
			const double ratio = static_cast<double>(count) / need;

			if (count >= need && ratio > bestRatio)
			{
				bestRatio = ratio;
				bestIdx = i;
				narrow = isNarrow;
			}
		}

		if (bestIdx >= held.size())
			return;

		const CellStruct seed = held[bestIdx].Cell;
		std::vector<std::pair<const NeuralHeldOrder*, std::pair<double, double>>> block;
		int sumX = 0, sumY = 0;
		double wantX = 0.0, wantY = 0.0;

		for (auto const& h : held)
		{
			if (!inBlock(h.Cell, seed))
				continue;

			const double len = std::max(1.0, h.Cell.DistanceFrom(h.Dest));
			const double wx = (h.Dest.X - h.Cell.X) / len;
			const double wy = (h.Dest.Y - h.Cell.Y) / len;
			block.push_back({ &h, { wx, wy } });
			sumX += h.Cell.X;
			sumY += h.Cell.Y;
			wantX += wx;
			wantY += wy;
		}

		const int n = static_cast<int>(block.size());
		const CellStruct center { static_cast<short>(sumX / n), static_cast<short>(sumY / n) };
		const double wantLen = std::sqrt(wantX * wantX + wantY * wantY);
		const double avgX = wantLen > 0.0 ? wantX / wantLen : 0.0;
		const double avgY = wantLen > 0.0 ? wantY / wantLen : 0.0;

		// The same narrow passage blocked again: since when, and from JamRerouteFrames on the way
		// around it opens (half of the units, then all of them) and stays open JamRerouteHoldFrames.
		if (narrow)
		{
			if (macro.JamSpot == CellStruct::Empty || macro.JamSpot.DistanceFrom(center) > JamSpotCells || now < macro.JamSpotSeenFrame
				|| now - macro.JamSpotSeenFrame > JamSpotGapFrames)
			{
				macro.JamSpot = center;
				macro.JamSpotSince = now;
			}

			macro.JamSpotSeenFrame = now;
			const int blockedFor = now - macro.JamSpotSince;

			if (blockedFor >= JamRerouteFrames)
			{
				const bool all = blockedFor >= JamRerouteAllFrames;

				if (now >= macro.JamRerouteUntil || macro.JamRerouteSpot != macro.JamSpot || all != macro.JamRerouteAll)
				{
					Debug::Log("[NeuralAI] Frame %d | House #%d passage at (%d,%d) blocked for %d frames: %s of the units take another way\n",
						now, pHouse->ArrayIndex, macro.JamSpot.X, macro.JamSpot.Y, blockedFor, all ? "all" : "half");
				}

				macro.JamRerouteSpot = macro.JamSpot;
				macro.JamRerouteUntil = now + JamRerouteHoldFrames;
				macro.JamRerouteAll = all;
			}
		}

		std::vector<FootClass*> idle;
		std::vector<CellStruct> occupied;
		// Our units within 6 cells that are not idle, by what keeps them there (diagnostics).
		std::map<std::string, int> around;

		auto const consider = [&](FootClass* pFoot)
		{
			if (!pFoot || !pFoot->IsAlive || pFoot->InLimbo || pFoot->IsInAir())
				return;

			const CellStruct here = pFoot->GetMapCoords();

			if (here.DistanceFrom(center) > 14.0)
				return;

			occupied.push_back(here);

			if (here.DistanceFrom(center) > 6.0 || pFoot->Owner != pHouse)
				return;

			// Never a slave of a slave miner: it works in a crowd around its miner by design.
			if (pFoot->SlaveOwner)
			{
				around["slave"]++;
			}
			else if (IsIdleBlocker(pHouse, pFoot))
			{
				idle.push_back(pFoot);
			}
			else
			{
				auto const pAroundType = pFoot->GetTechnoType();
				auto const pAroundUnit = abstract_cast<UnitClass*>(pFoot);

				if (macro.PlanMembers.contains(pFoot->UniqueID))
					around["plan"]++;
				else if ((pAroundType && pAroundType->ResourceGatherer) || (pAroundUnit && pAroundUnit->Type && pAroundUnit->Type->Harvester))
					around["gatherer"]++;
				else if (pFoot->Target)
					around["target"]++;
				else
					around["m" + std::to_string(static_cast<int>(pFoot->CurrentMission))]++;
			}
		};

		for (auto const pUnit : UnitClass::Array)
			consider(pUnit);

		for (auto const pInf : InfantryClass::Array)
			consider(pInf);

		std::vector<CellStruct> taken;
		int moved = 0;

		// Free ground 8..14 cells away, off the passage mouths when there is any.
		auto const findWayOut = [&](FootClass* pFoot)
		{
			auto pCell = FindClearingCell(pFoot, center, 8, 14, avgX, avgY, occupied, taken, true);

			return pCell ? pCell : FindClearingCell(pFoot, center, 8, 14, avgX, avgY, occupied, taken);
		};

		for (auto const pFoot : idle)
		{
			if (moved >= 16)
				break;

			if (auto const pCell = findWayOut(pFoot))
			{
				taken.push_back(pCell->MapCoords);

				if (IssueMoveOrder(pFoot, pCell, 0.0))
					++moved;
			}
		}

		// A held unit whose move ends inside the block itself is not passing through: it wants to
		// stand where the crowd is (a post, a rally spot, a cell another job handed out there). Outside
		// a plan it gives the spot up and leaves like the idle ones; its owner logic gives it new work.
		int gaveUp = 0;
		std::unordered_set<FootClass*> left;

		for (auto const& [pHeld, want] : block)
		{
			FootClass* const pFoot = pHeld ? pHeld->Foot : nullptr;

			if (moved + gaveUp >= 16)
				break;

			if (!pFoot || !pFoot->IsAlive || pFoot->InLimbo || pFoot->SlaveOwner || pHeld->Dest.DistanceFrom(center) > JamInsideCells
				|| (pFoot->CurrentMission != Mission::Move && pFoot->CurrentMission != Mission::AttackMove)
				|| macro.PlanMembers.contains(pFoot->UniqueID))
			{
				continue;
			}

			if (auto const pCell = findWayOut(pFoot))
			{
				taken.push_back(pCell->MapCoords);
				pFoot->ClearMegaMissionData();
				pFoot->SetTarget(nullptr);
				pFoot->SetDestination(nullptr, true);

				if (IssueMoveOrder(pFoot, pCell, 0.0))
				{
					++gaveUp;
					left.insert(pFoot);
				}
			}
		}

		// The passage stayed blocked: held units of the block take the way around it at once
		// (JamRerouteHop says which ones); the others get it with their next order.
		int rerouted = 0;

		if (now < macro.JamRerouteUntil)
		{
			for (auto const& [pHeld, want] : block)
			{
				FootClass* const pFoot = pHeld ? pHeld->Foot : nullptr;

				if (rerouted >= 12 || !pFoot || !pFoot->IsAlive || pFoot->InLimbo || left.contains(pFoot))
					continue;

				if (auto const pHop = JamRerouteHop(pFoot, pHeld->Dest))
				{
					pFoot->ClearMegaMissionData();
					pFoot->SetTarget(nullptr);

					if (IssueMoveOrder(pFoot, pHop, 1.5))
					{
						++rerouted;
						left.insert(pFoot);
					}
				}
			}
		}

		// Two-way traffic in a narrow passage: the smaller side backs off.
		int yielded = 0;

		if (narrow)
		{
			const bool clearAxis = wantLen >= 0.3 * n;
			const double axisX = clearAxis ? avgX : block.front().second.first;
			const double axisY = clearAxis ? avgY : block.front().second.second;
			std::vector<std::pair<FootClass*, std::pair<double, double>>> forward, backward;

			for (auto const& [pHeld, want] : block)
			{
				const double dot = want.first * axisX + want.second * axisY;

				if (dot > 0.3)
					forward.push_back({ pHeld->Foot, want });
				else if (dot < -0.3)
					backward.push_back({ pHeld->Foot, want });
			}

			if (!forward.empty() && !backward.empty())
			{
				auto& yielders = backward.size() <= forward.size() ? backward : forward;

				for (auto const& [pFoot, want] : yielders)
				{
					if (yielded >= 12 || !pFoot || !pFoot->IsAlive || pFoot->InLimbo || left.contains(pFoot))
						continue;

					// On a long bridge the 8..14 ring is all deck or water: the side that backs off then
					// goes past the bridge end (16..26 cells; match #7080, 20-25 units held for minutes on
					// a bridge in two-way traffic, 0-3 backed off per break).
					auto pCell = FindClearingCell(pFoot, center, 8, 14, want.first, want.second, occupied, taken);

					if (!pCell)
						pCell = FindClearingCell(pFoot, center, 16, 26, want.first, want.second, occupied, taken);

					if (pCell)
					{
						taken.push_back(pCell->MapCoords);
						pFoot->ClearMegaMissionData();
						pFoot->SetTarget(nullptr);
						pFoot->SetDestination(nullptr, true);

						if (IssueMoveOrder(pFoot, pCell, 0.0))
							++yielded;
					}
				}
			}
		}

		// Ships in the block take a leg of a real water route (GroundRouteHop: cell by cell, buildings
		// block) to their destination: match #7061, Fludded Recources, 15 ships held for minutes in a
		// channel our own naval yard closed, with open sea all around the island.
		int detoured = 0;

		for (auto const& [pHeld, want] : block)
		{
			FootClass* const pShip = pHeld ? pHeld->Foot : nullptr;

			if (detoured >= 12 || !pShip || !pShip->IsAlive || pShip->InLimbo)
				continue;

			auto const pShipType = pShip->GetTechnoType();

			if (!pShipType || !(pShipType->Naval || pShipType->MovementZone == MovementZone::Water
				|| pShipType->MovementZone == MovementZone::WaterBeach))
			{
				continue;
			}

			if (auto const pHop = GroundRouteHop(pShip, pHeld->Dest, 14))
			{
				pShip->ClearMegaMissionData();
				pShip->SetTarget(nullptr);

				if (IssueMoveOrder(pShip, pHop, 1.5))
					++detoured;
			}
		}

		if (detoured > 0)
		{
			CountDecision(NeuralDecision::GroundRouteHop, pHouse->ArrayIndex);
			Debug::Log("[NeuralAI] Frame %d | House #%d naval jam at (%d,%d): %d ships sent along a water route around it\n",
				now, pHouse->ArrayIndex, center.X, center.Y, detoured);
		}

		if (moved > 0 || gaveUp > 0 || yielded > 0 || detoured > 0 || rerouted > 0)
		{
			macro.LastJamBreakFrame = now;

			if (moved > 0 || gaveUp > 0)
				CountDecision(NeuralDecision::JamCleared, pHouse->ArrayIndex);

			if (yielded > 0)
				CountDecision(NeuralDecision::JamYielded, pHouse->ArrayIndex);

			if (rerouted > 0)
				CountDecision(NeuralDecision::GroundRouteHop, pHouse->ArrayIndex);

			Debug::Log("[NeuralAI] Frame %d | House #%d jam at (%d,%d)%s: %d held units, %d idle moved out (of %d idle), %d backed off, "
				"%d gave up a spot inside it, %d sent around\n", now, pHouse->ArrayIndex, center.X, center.Y,
				narrow ? " in a narrow passage" : "", n, moved, static_cast<int>(idle.size()), yielded, gaveUp, rerouted);
		}

		// What the block is made of (diagnostics, at most once per 900 frames): held units by mission,
		// the destinations most of them share, their types.
		if (now - macro.LastJamDiagFrame >= 900 || now < macro.LastJamDiagFrame)
		{
			macro.LastJamDiagFrame = now;
			std::map<int, int> byMission;
			std::map<std::pair<short, short>, int> byDest;
			std::map<std::string, int> byType;

			for (auto const& [pHeld, want] : block)
			{
				if (!pHeld || !pHeld->Foot)
					continue;

				byMission[static_cast<int>(pHeld->Foot->CurrentMission)]++;
				byDest[{ pHeld->Dest.X, pHeld->Dest.Y }]++;

				if (auto const pHeldType = pHeld->Foot->GetTechnoType())
					byType[pHeldType->ID]++;
			}

			std::string text;

			for (auto const& [mission, count] : byMission)
				text += "m" + std::to_string(mission) + "x" + std::to_string(count) + " ";

			std::vector<std::pair<int, std::pair<short, short>>> dests;

			for (auto const& [cell, count] : byDest)
				dests.push_back({ count, cell });

			std::sort(dests.begin(), dests.end(), [](auto const& a, auto const& b) { return a.first > b.first; });
			text += "| dest ";

			for (size_t i = 0; i < dests.size() && i < 4; ++i)
				text += "(" + std::to_string(dests[i].second.first) + "," + std::to_string(dests[i].second.second) + ")x" + std::to_string(dests[i].first) + " ";

			text += "| types ";

			for (auto const& [typeId, count] : byType)
				text += typeId + "x" + std::to_string(count) + " ";

			text += "| around ";

			for (auto const& [reason, count] : around)
				text += reason + "x" + std::to_string(count) + " ";

			Debug::Log("[NeuralAI] Frame %d | House #%d jam detail at (%d,%d): %s\n", now, pHouse->ArrayIndex, center.X, center.Y, text.c_str());

			// The ground around the first block of each house (diagnostics): '#' building, '~' water,
			// '^' impassable for tracked vehicles, 'u' our unit, 'e' another house's unit, 'H' a held
			// unit of the block, '.' free; 31 x 21 cells, row by row.
			if (!macro.JamMapLogged)
			{
				macro.JamMapLogged = true;
				Debug::Log("[NeuralAI] House #%d jam map around (%d,%d), x %d..%d:\n", pHouse->ArrayIndex, center.X, center.Y,
					center.X - 15, center.X + 15);

				for (int dy = -10; dy <= 10; ++dy)
				{
					char row[40] = {};

					for (int dx = -15; dx <= 15; ++dx)
					{
						const CellStruct c { static_cast<short>(center.X + dx), static_cast<short>(center.Y + dy) };
						auto const pCell = MapClass::Instance.TryGetCellAt(c);
						char ch = ' ';

						if (pCell)
						{
							ch = '.';

							if (pCell->GetBuilding())
								ch = '#';
							else if (pCell->LandType == LandType::Water)
								ch = '~';
							else if (!pCell->IsClearToMove(SpeedType::Track, true, true, -1, MovementZone::Normal, -1, false))
								ch = '^';

							if (ch == '.')
							{
								for (auto const& [pHeld, want] : block)
								{
									if (pHeld && pHeld->Cell == c)
									{
										ch = 'H';
										break;
									}
								}
							}

							if (ch == '.')
							{
								if (auto const pTechno = abstract_cast<TechnoClass*>(pCell->FirstObject); pTechno && pTechno->Owner)
									ch = pTechno->Owner == pHouse ? 'u' : 'e';
							}
						}

						row[dx + 15] = ch;
					}

					Debug::Log("[NeuralAI]   y %3d %s\n", center.Y + dy, row);
				}
			}
		}
	}

	// Bridge and tunnel mouths kept clear: every 300 frames, our idle ground units (IsIdleBlocker)
	// standing on a passage mouth (IsPassageMouthCell), on a bridge deck or within 2 cells of one,
	// within 2 cells of a tunnel tube's entry or exit cell, or within 3 cells of a tunnel building of
	// ours or an ally (Ares Tunnel=), are moved 4..12 cells away (not onto a bridge, a tube or another
	// mouth) when one of our units within 6 cells waits there with an order (move, attack-move, enter,
	// harvest; 90+ frames on its cell). Up to 12 per pass.
	void UpdateJamMouths(HouseClass* pHouse)
	{
		const NeuralSlowStep slowStep { "UpdateJamMouths" };

		auto& macro = GetMacroState(pHouse);
		const int now = Unsorted::CurrentFrame;

		if (macro.LastMouthClearFrame >= 0 && now >= macro.LastMouthClearFrame && now - macro.LastMouthClearFrame < 300)
			return;

		macro.LastMouthClearFrame = now;

		std::vector<std::pair<CellStruct, double>> mouths;

		for (auto const pTube : TubeClass::Array)
		{
			if (!pTube)
				continue;

			mouths.emplace_back(pTube->EnterCell, 2.0);
			mouths.emplace_back(pTube->ExitCell, 2.0);
		}

		for (auto const pBld : BuildingClass::Array)
		{
			if (!pBld || !pBld->IsAlive || pBld->InLimbo || !pBld->Type || !pBld->Owner || !pHouse->IsAlliedWith(pBld->Owner))
				continue;

			auto const pBldExt = BuildingTypeExt::ExtMap.Find(pBld->Type);

			if (pBldExt && pBldExt->Tunnel)
			{
				const double reach = 3.0 + std::max(pBld->Type->GetFoundationWidth(), pBld->Type->GetFoundationHeight(false)) / 2.0;
				mouths.emplace_back(pBld->GetMapCoords(), reach);
			}
		}

		// Our units waiting with an order: the traffic a mouth is kept clear for.
		std::vector<CellStruct> waiting;

		auto const collectWaiting = [&](FootClass* pFoot)
		{
			if (!pFoot || pFoot->Owner != pHouse || !pFoot->IsAlive || pFoot->InLimbo || pFoot->IsInAir())
				return;

			const bool ordered = pFoot->CurrentMission == Mission::Move || pFoot->CurrentMission == Mission::AttackMove
				|| pFoot->MegaMission == Mission::AttackMove || pFoot->CurrentMission == Mission::Enter
				|| ((pFoot->CurrentMission == Mission::Harvest || pFoot->CurrentMission == Mission::Return) && pFoot->Destination);

			if (!ordered)
				return;

			auto const it = macro.FootProgress.find(pFoot->UniqueID);

			if (it != macro.FootProgress.end() && now >= it->second.SinceFrame && now - it->second.SinceFrame >= 90)
				waiting.push_back(pFoot->GetMapCoords());
		};

		for (auto const pUnit : UnitClass::Array)
			collectWaiting(pUnit);

		for (auto const pInf : InfantryClass::Array)
			collectWaiting(pInf);

		if (waiting.empty())
			return;

		auto const atMouth = [&](const CellStruct& here) -> bool
		{
			if (IsPassageMouthCell(here))
				return true;

			for (auto const& [cell, reach] : mouths)
			{
				if (here.DistanceFrom(cell) <= reach)
					return true;
			}

			for (int dy = -2; dy <= 2; ++dy)
			{
				for (int dx = -2; dx <= 2; ++dx)
				{
					auto const pCell = MapClass::Instance.TryGetCellAt(
						CellStruct { static_cast<short>(here.X + dx), static_cast<short>(here.Y + dy) });

					if (pCell && (pCell->ContainsBridge() || pCell->ContainsBridgeEx()))
						return true;
				}
			}

			return false;
		};

		std::vector<CellStruct> occupied;
		std::vector<CellStruct> taken;
		int moved = 0;

		auto const clearFoot = [&](FootClass* pFoot)
		{
			if (moved >= 12 || !IsIdleBlocker(pHouse, pFoot))
				return;

			const CellStruct here = pFoot->GetMapCoords();
			bool trafficNear = false;

			for (auto const& w : waiting)
			{
				if (w != here && w.DistanceFrom(here) <= 6.0)
				{
					trafficNear = true;
					break;
				}
			}

			if (!trafficNear || !atMouth(here))
				return;

			if (occupied.empty())
			{
				for (auto const pU : UnitClass::Array)
				{
					if (pU && pU->IsAlive && !pU->InLimbo)
						occupied.push_back(pU->GetMapCoords());
				}
			}

			if (auto const pCell = FindClearingCell(pFoot, here, 4, 12, 0.0, 0.0, occupied, taken, true))
			{
				if (atMouth(pCell->MapCoords))
					return;

				taken.push_back(pCell->MapCoords);

				if (IssueMoveOrder(pFoot, pCell, 0.0))
					++moved;
			}
		};

		for (auto const pUnit : UnitClass::Array)
			clearFoot(pUnit);

		for (auto const pInf : InfantryClass::Array)
			clearFoot(pInf);

		if (moved > 0)
		{
			CountDecision(NeuralDecision::MouthCleared, pHouse->ArrayIndex);
			Debug::Log("[NeuralAI] Frame %d | House #%d cleared %d idle units from bridge/tunnel mouths\n", now, pHouse->ArrayIndex, moved);
		}
	}

	// Hovering flyer watchdog: a flying unit of ours (jumpjet, or a Fly unit that is no aircraft) held
	// in the air over one cell (within 1.5) for 600 frames, 900 when it is in a plan, without firing in
	// the last 300 frames, over water or 12+ cells from any building of ours, is doing nothing where
	// anti-air finds it (match #6164: a HIND hovered over the ocean on a finished move until shot
	// down; a jumpjet cannot settle on water, so its move never ends and nothing gave it new work).
	// Its orders are dropped and it flies home (FindCarrierHomeCell); the carryall / transport logic
	// takes it from there. The state is logged so the order that left it there can be traced.
	// Landed or near our base it is also released (orders dropped, free for the AI's next job, no trip
	// home) when what it holds is a dead order: a move it already finished, or a target out of reach
	// it does not go after (match #6176: a HIND landed on an island coast, Move to its own cell and a
	// target 67 cells away, for minutes; the ground watchdog skips jumpjets).
	void UpdateHoveringFlyers(HouseClass* pHouse)
	{
		auto& macro = GetMacroState(pHouse);
		const int now = Unsorted::CurrentFrame;
		std::unordered_map<DWORD, NeuralStuckTrack> next;

		for (auto const pUnit : UnitClass::Array)
		{
			if (!pUnit || pUnit->Owner != pHouse || !pUnit->IsAlive || pUnit->InLimbo || !pUnit->Type
				|| !(pUnit->Type->JumpJet || pUnit->Type->MovementZone == MovementZone::Fly))
			{
				continue;
			}

			const CellStruct here = pUnit->GetMapCoords();
			NeuralStuckTrack track { here, now };

			if (auto const it = macro.HoverTracks.find(pUnit->UniqueID);
				it != macro.HoverTracks.end() && it->second.Cell.DistanceFrom(here) <= 1.5 && now >= it->second.SinceFrame)
			{
				track = it->second;
			}

			if (pUnit->IsFiring || (pUnit->LastFireBulletFrame > 0 && now >= pUnit->LastFireBulletFrame && now - pUnit->LastFireBulletFrame < 300))
				track.SinceFrame = now;

			const bool inPlan = macro.PlanMembers.contains(pUnit->UniqueID);

			if (now - track.SinceFrame < (inPlan ? 900 : 600))
			{
				next.emplace(pUnit->UniqueID, track);
				continue;
			}

			auto const pCell = pUnit->GetCell();
			const bool inAir = pUnit->IsInAir();
			bool exposed = inAir && pCell && pCell->LandType == LandType::Water;

			if (inAir && !exposed)
			{
				exposed = true;

				for (auto const pOwnBld : pHouse->Buildings)
				{
					if (pOwnBld && pOwnBld->IsAlive && !pOwnBld->InLimbo && pOwnBld->GetMapCoords().DistanceFrom(here) <= 12.0)
					{
						exposed = false;
						break;
					}
				}
			}

			auto const pDestCell = abstract_cast<CellClass*>(pUnit->Destination);
			const bool finishedMove = pUnit->CurrentMission == Mission::Move
				&& (!pUnit->Destination || CellClass::Coord2Cell(pUnit->Destination->GetCoords()).DistanceFrom(here) <= 1.5);
			const bool targetOutOfReach = pUnit->Target && !pUnit->IsCloseEnoughToAttack(pUnit->Target);
			const bool deadOrder = finishedMove || targetOutOfReach;

			if (!exposed && !deadOrder)
			{
				next.emplace(pUnit->UniqueID, track);
				continue;
			}

			auto const pTargetObj = abstract_cast<ObjectClass*>(pUnit->Target);
			auto const pCarryExt = UnitExt::Fetch(pUnit);
			Debug::Log("[NeuralAI] Frame %d | House #%d hovering %s #%u at (%d,%d) for %d frames: mission %d mega %d dest (%d,%d) target %s (%d,%d) passengers %d carryall state %d cargo %s plan %d -> %s\n",
				now, pHouse->ArrayIndex, pUnit->Type->ID, pUnit->UniqueID, here.X, here.Y, now - track.SinceFrame,
				static_cast<int>(pUnit->CurrentMission), static_cast<int>(pUnit->MegaMission),
				pDestCell ? pDestCell->MapCoords.X : -1, pDestCell ? pDestCell->MapCoords.Y : -1,
				pTargetObj && pTargetObj->GetType() ? pTargetObj->GetType()->ID : "-",
				pTargetObj ? pTargetObj->GetMapCoords().X : -1, pTargetObj ? pTargetObj->GetMapCoords().Y : -1,
				pUnit->Passengers.NumPassengers, pCarryExt ? static_cast<int>(pCarryExt->CarryallState) : -1,
				(pCarryExt && pCarryExt->CarryallCargo && pCarryExt->CarryallCargo->GetTechnoType()) ? pCarryExt->CarryallCargo->GetTechnoType()->ID : "-",
				inPlan ? 1 : 0, exposed ? "home" : "released");

			pUnit->SetTarget(nullptr);
			pUnit->ClearMegaMissionData();
			pUnit->SetDestination(nullptr, true);

			if (!exposed)
				pUnit->QueueMission(Mission::Guard, true);
			else if (auto const pHome = FindCarrierHomeCell(pHouse, pUnit))
				IssueMoveOrder(pUnit, pHome, 0.0);
			else
				pUnit->QueueMission(Mission::Guard, true);

			CountDecision(NeuralDecision::HoverReleased, pHouse->ArrayIndex);
			// Its next stop starts a new track.
		}

		macro.HoverTracks = std::move(next);
	}

	// Stuck order watchdog: a ground unit with a move or attack-move destination that has not
	// left its cell is released (orders cleared, back to Guard) so the AI gives it a new job:
	// after 450 frames when the destination is in another movement zone (unreachable: e.g. the
	// only bridge was destroyed and the pathfinder parks it on the near bank forever), after 1800
	// frames otherwise (a traffic jam that does not clear). Units fighting are never touched.
	// Aircraft stuck on an attack they never fly are released as well (see below).
	void UpdateStuckUnits(HouseClass* pHouse)
	{
		const NeuralSlowStep slowStep { "UpdateStuckUnits" };

		if (!pHouse)
			return;

		auto& macro = GetMacroState(pHouse);
		std::unordered_map<DWORD, NeuralStuckTrack> next;
		const int now = Unsorted::CurrentFrame;
		// Units of ours held 450+ frames on one cell with an order pending, and where they want to go.
		std::vector<NeuralHeldOrder> stuckOrders;

		auto check = [&](FootClass* pFoot, bool skip)
		{
			if (skip || !pFoot || pFoot->Owner != pHouse || !pFoot->IsAlive || pFoot->InLimbo || pFoot->IsInAir())
				return;

			// The slaves of a slave miner walk between it and the ore in a crowd, and its slave manager
			// drives them: they are neither stuck nor in a jam, and an order of ours takes them off
			// their work (slaves were the unit most often "freed" from jams; a Yuri house sat at 5
			// credits for half an hour with its miners deployed).
			if (pFoot->SlaveOwner)
				return;

			auto const pType = pFoot->GetTechnoType();

			// Ships included: an unreachable naval order (land cell) is dropped like any other.
			if (!pType || pType->JumpJet || pType->Teleporter || pType->MovementZone == MovementZone::Fly)
				return;

			// An attack on a target out of reach counts as a move toward it: in a block of our own
			// units it waits for a path forever.
			const bool attackingOutOfReach = pFoot->CurrentMission == Mission::Attack && pFoot->Target
				&& !pFoot->IsCloseEnoughToAttack(pFoot->Target);
			// Entering a building (a tunnel, a depot, a bio reactor) waits in the queue like a move; a
			// transport's boarding has its own one-by-one control.
			BuildingClass* const pEnterBuilding = pFoot->CurrentMission == Mission::Enter
				? abstract_cast<BuildingClass*>(pFoot->Destination ? pFoot->Destination : pFoot->Target)
				: nullptr;
			const bool moving = pFoot->CurrentMission == Mission::Move || pFoot->CurrentMission == Mission::AttackMove
				|| pFoot->MegaMission == Mission::AttackMove || attackingOutOfReach || pEnterBuilding;
			AbstractClass* const pDest = pEnterBuilding ? pEnterBuilding
				: (pFoot->Destination ? pFoot->Destination
					: (pFoot->MegaDestination ? pFoot->MegaDestination : (attackingOutOfReach ? pFoot->Target : nullptr)));

			if (!moving || !pDest)
				return;

			const CellStruct here = pFoot->GetMapCoords();
			const CellStruct destCell = CellClass::Coord2Cell(pDest->GetCoords());

			if (here.DistanceFrom(destCell) <= 2.5)
				return;

			NeuralStuckTrack track { here, now };

			if (auto const it = macro.StuckTracks.find(pFoot->UniqueID);
				it != macro.StuckTracks.end() && it->second.Cell.DistanceFrom(here) <= 1.5)
			{
				track = it->second;
			}

			auto const pTargetTechno = abstract_cast<TechnoClass*>(pFoot->Target);

			// Fighting: firing, or a live target within reach. A target out of reach does not keep a
			// unit that never moves from being released.
			if (pFoot->IsFiring
				|| (pTargetTechno && pTargetTechno->IsAlive && !pTargetTechno->InLimbo && pFoot->IsCloseEnoughToAttack(pTargetTechno)))
			{
				track.SinceFrame = now;
			}

			const int stuckFrames = now - track.SinceFrame;
			auto const pDestObject = abstract_cast<ObjectClass*>(pDest);
			const int footZone = ObjectZone(pFoot, pType->MovementZone);
			const int destZone = MapClass::Instance.GetMovementZoneType(
				destCell, pType->MovementZone, pDestObject && pDestObject->OnBridge);
			bool unreachable = footZone > 1 && destZone != footZone;

			// A destination on an impassable cell (a building, an obstacle; zone 1) is reachable
			// when a cell next to it (up to 2 away) is in our zone, as for attack-moves onto enemy
			// buildings.
			if (unreachable && destZone <= 1)
			{
				for (int r = 1; r <= 2 && unreachable; ++r)
				{
					for (int dy = -r; dy <= r && unreachable; ++dy)
					{
						for (int dx = -r; dx <= r && unreachable; ++dx)
						{
							if (std::abs(dx) != r && std::abs(dy) != r)
								continue;

							const CellStruct nc { static_cast<short>(destCell.X + dx), static_cast<short>(destCell.Y + dy) };

							if (MapClass::Instance.GetMovementZoneType(nc, pType->MovementZone, false) == footZone)
								unreachable = false;
						}
					}
				}
			}

			// Stalled attack-move: the unit fell back to Guard (typically after a fight) with its
			// attack-move still pending and does not resume it, while the order paths that check
			// "MegaMission != AttackMove" treat it as already on its way. Drop the stale order after
			// 150 frames so its owner logic sends it again.
			const bool stalledAttackMove = (pFoot->CurrentMission == Mission::Guard || pFoot->CurrentMission == Mission::Area_Guard)
				&& pFoot->MegaMission == Mission::AttackMove;

			if ((unreachable && stuckFrames >= 450) || stuckFrames >= 1800 || (stalledAttackMove && stuckFrames >= 150))
			{
				// TEMP diagnostic: a ship that never left its cell, where it was sent and why it is released.
				if (pType->Naval)
				{
					static int shipLogSerial = -1;
					static int shipLogCount = 0;

					if (shipLogSerial != NeuralMatchSerial)
					{
						shipLogSerial = NeuralMatchSerial;
						shipLogCount = 0;
					}

					if (shipLogCount++ < 30)
					{
						Debug::Log("[NeuralAI] Naval DIAG house %d frame %d: %s %u held at (%d,%d) for %d frames, sent to (%d,%d) %d cells away, "
							"zones %d and %d, unreachable %d, stalled attack-move %d, mission %d mega %d, destination land type %d.\n",
							pHouse->ArrayIndex, now, pType->ID, pFoot->UniqueID, here.X, here.Y, stuckFrames, destCell.X, destCell.Y,
							static_cast<int>(here.DistanceFrom(destCell)), footZone, destZone, unreachable ? 1 : 0, stalledAttackMove ? 1 : 0,
							static_cast<int>(pFoot->CurrentMission), static_cast<int>(pFoot->MegaMission),
							MapClass::Instance.TryGetCellAt(destCell) ? static_cast<int>(MapClass::Instance.TryGetCellAt(destCell)->LandType) : -1);
					}
				}

				pFoot->ClearMegaMissionData();
				pFoot->SetDestination(nullptr, true);
				pFoot->QueueMission(Mission::Guard, true);
				CountDecision(unreachable ? NeuralDecision::UnreachableOrderDropped
					: (stalledAttackMove ? NeuralDecision::AttackMoveStalled : NeuralDecision::StuckUnitReleased),
					pHouse->ArrayIndex);
				return; // Its next order starts a new track.
			}

			if (stuckFrames >= 450)
				stuckOrders.push_back({ pFoot, here, destCell });

			next.emplace(pFoot->UniqueID, track);
		};

		for (auto const pUnit : UnitClass::Array)
			check(pUnit, !pUnit || !pUnit->Type || pUnit->Type->Harvester || pUnit->Type->ResourceGatherer);

		for (auto const pInf : InfantryClass::Array)
			check(pInf, false);

		macro.StuckTracks = std::move(next);

		// Where every ground unit of ours stands and since when (firing restarts it): IsFootHeldStill.
		{
			std::unordered_map<DWORD, NeuralStuckTrack> progress;

			auto const trackProgress = [&](FootClass* pFoot)
			{
				if (!pFoot || pFoot->Owner != pHouse || !pFoot->IsAlive || pFoot->InLimbo || pFoot->IsInAir())
					return;

				const CellStruct here = pFoot->GetMapCoords();
				NeuralStuckTrack track { here, now };

				if (auto const it = macro.FootProgress.find(pFoot->UniqueID);
					it != macro.FootProgress.end() && it->second.Cell.DistanceFrom(here) <= 1.5 && !pFoot->IsFiring)
				{
					track = it->second;
				}

				progress.emplace(pFoot->UniqueID, track);
			};

			for (auto const pUnit : UnitClass::Array)
				trackProgress(pUnit);

			for (auto const pInf : InfantryClass::Array)
				trackProgress(pInf);

			macro.FootProgress = std::move(progress);
		}

		UpdateJamBreaker(pHouse, stuckOrders);
		UpdateJamMouths(pHouse);
		UpdateHoveringFlyers(pHouse);
		CheckCellGhosts();

		// Aircraft on Mission::Attack that neither move nor fire: when the engine finds no fire
		// location for the target it goes back to "return to base" and, still armed and targeting,
		// straight back to picking a location, so the aircraft sits on its pad for the rest of the
		// match, never reloading and never free for other work. Released (target cleared, idle
		// mode: back to the pad and reload) after 450 frames on the ground without moving or a
		// shot, or 1800 frames in the air without a shot. Plan members are left to their plan.
		std::unordered_map<DWORD, NeuralAirStallTrack> nextAir;

		for (auto const pAir : AircraftClass::Array)
		{
			if (!pAir || pAir->Owner != pHouse || !pAir->IsAlive || pAir->InLimbo || !pAir->Type || pAir->Type->Spawned
				|| pAir->CurrentMission != Mission::Attack || !pAir->Target || macro.PlanMembers.contains(pAir->UniqueID))
			{
				continue;
			}

			const CellStruct here = pAir->GetMapCoords();
			NeuralAirStallTrack track { here, now, pAir->Ammo, now };

			if (auto const it = macro.AirStallTracks.find(pAir->UniqueID); it != macro.AirStallTracks.end())
			{
				track = it->second;

				if (track.Cell.DistanceFrom(here) > 1.5)
				{
					track.Cell = here;
					track.SinceFrame = now;
				}

				if (track.Ammo != pAir->Ammo)
				{
					track.Ammo = pAir->Ammo;
					track.AmmoSinceFrame = now;
				}
			}

			if (pAir->IsFiring)
			{
				track.SinceFrame = now;
				track.AmmoSinceFrame = now;
			}

			const bool parked = !pAir->IsInAir() && now - track.SinceFrame >= 450 && now - track.AmmoSinceFrame >= 450;
			const bool idleFlight = pAir->IsInAir() && now - track.AmmoSinceFrame >= 1800;

			if (parked || idleFlight)
			{
				pAir->SetTarget(nullptr);
				pAir->SetDestination(nullptr, true);
				pAir->EnterIdleMode(false, true);
				CountDecision(NeuralDecision::AirAttackStalled, pHouse->ArrayIndex);
				continue; // Its next attack starts a new track.
			}

			nextAir.emplace(pAir->UniqueID, track);
		}

		macro.AirStallTracks = std::move(nextAir);
	}

	// Idle harvester dispatcher: an empty harvester on Harvest that has not left its cell for 900
	// frames found no ore within its scan, and vanilla keeps it waiting in place (e.g. on a
	// depleted field). Send it to the nearest worthy ore field (>= 4 ore cells in 5x5) on its own
	// landmass within 96 cells, 8+ cells away and 10+ cells from known enemy structures; on
	// arrival it falls back to Guard and the harvester logic puts it back on Harvest.
	// Chrono miners (Teleporter) and deployable gatherers are left alone: a destination makes a
	// chrono miner loop-teleport, and slave miners have their own routing.
	void UpdateIdleHarvesters(HouseClass* pHouse)
	{
		const NeuralSlowStep slowStep { "UpdateIdleHarvesters" };

		if (!pHouse)
			return;

		auto& macro = GetMacroState(pHouse);
		std::unordered_map<DWORD, NeuralStuckTrack> next;
		const int now = Unsorted::CurrentFrame;
		// Expert 0 in charge moves a harvester stuck on a drained field three times sooner.
		auto const pIdleExt = HouseExt::Fetch(pHouse);
		const int idleWait = (pIdleExt && pIdleExt->NeuralAI_ActiveExpert == 0) ? 300 : 900;
		std::vector<CellStruct> enemyStructures;
		bool enemyStructuresKnown = false;

		for (auto const pUnit : UnitClass::Array)
		{
			if (!pUnit || pUnit->Owner != pHouse || !pUnit->IsAlive || pUnit->InLimbo || !pUnit->Type
				|| !pUnit->Type->Harvester || pUnit->Type->Teleporter || pUnit->Type->DeploysInto || pUnit->IsInAir())
			{
				continue;
			}

			if (pUnit->CurrentMission != Mission::Harvest || pUnit->HasAnyLink()
				|| pUnit->Tiberium.GetTotalAmount() > 0.0f)
			{
				continue;
			}

			const CellStruct here = pUnit->GetMapCoords();
			NeuralStuckTrack track { here, now };

			if (auto const it = macro.IdleHarvesterTracks.find(pUnit->UniqueID);
				it != macro.IdleHarvesterTracks.end() && it->second.Cell.DistanceFrom(here) <= 1.5)
			{
				track = it->second;
			}

			if (now - track.SinceFrame < idleWait)
			{
				next.emplace(pUnit->UniqueID, track);
				continue;
			}

			if (!enemyStructuresKnown)
			{
				enemyStructuresKnown = true;

				for (auto const pEB : BuildingClass::Array)
				{
					if (pEB && pEB->IsAlive && !pEB->InLimbo && pEB->Owner && pEB->Type && pEB->Owner != pHouse
						&& !pHouse->IsAlliedWith(pEB->Owner) && !pEB->Owner->IsNeutral() && IsEnemyBuildingKnown(pHouse, pEB))
					{
						enemyStructures.push_back(pEB->GetMapCoords());
					}
				}
			}

			const MovementZone mz = pUnit->Type->MovementZone;
			const int unitZone = ObjectZone(pUnit, mz);
			CellClass* pBestOre = nullptr;
			// Distance plus the enemy route heat the straight way there crosses (40 heat = 1 cell),
			// so a harvester does not cross the hottest enemy routes for a slightly closer field.
			int bestCost = INT_MAX;
			CellClass* pNearestOre = nullptr;
			int nearestDist = INT_MAX;

			for (int dy = -96; dy <= 96; dy += 3)
			{
				for (int dx = -96; dx <= 96; dx += 3)
				{
					const int d = std::max(std::abs(dx), std::abs(dy));

					if (d < 8 || (d >= bestCost && d >= nearestDist))
						continue;

					const CellStruct tc { static_cast<short>(here.X + dx), static_cast<short>(here.Y + dy) };
					auto const pCell = MapClass::Instance.TryGetCellAt(tc);

					if (!pCell || pCell->GetContainedTiberiumValue() <= 0)
						continue;

					if (unitZone >= 0 && MapClass::Instance.GetMovementZoneType(tc, mz, false) != unitZone)
						continue;

					bool nearEnemy = false;

					for (auto const& ec : enemyStructures)
					{
						if (std::max(std::abs(ec.X - tc.X), std::abs(ec.Y - tc.Y)) < 10)
						{
							nearEnemy = true;
							break;
						}
					}

					if (nearEnemy)
						continue;

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

					if (d < nearestDist)
					{
						nearestDist = d;
						pNearestOre = pCell;
					}

					const int cost = d + RouteHeatAlong(pHouse, here, tc) / 40;

					if (cost < bestCost)
					{
						bestCost = cost;
						pBestOre = pCell;
					}
				}
			}

			if (pBestOre && pNearestOre && pBestOre != pNearestOre && pBestOre->MapCoords.DistanceFrom(pNearestOre->MapCoords) > 6.0)
				CountDecision(NeuralDecision::RouteHarvesterDetour, pHouse->ArrayIndex);

			if (!pBestOre)
				pBestOre = FindFreeOreFieldCell(pHouse, here, ObjectZone(pUnit, MovementZone::Normal), 8, true);

			if (pBestOre)
			{
				pUnit->SetDestination(pBestOre, true);
				pUnit->QueueMission(Mission::Move, true);
				CountDecision(NeuralDecision::IdleHarvesterSent, pHouse->ArrayIndex);
				continue; // A new track starts when it is back on Harvest.
			}

			// No field in reach: look again in 900 frames rather than scanning every tick.
			track.SinceFrame = now;
			next.emplace(pUnit->UniqueID, track);
		}

		macro.IdleHarvesterTracks = std::move(next);
	}

	// Offensive crawl bookkeeping: confirm the cells chosen for stones toward the enemy once a
	// structure of ours stands there, then count the confirmed stones that are destroyed. The
	// trainer credits slots 38 / 81 with how well forward stones survive.
	void UpdateOffensiveStones(HouseClass* pHouse)
	{
		const NeuralSlowStep slowStep { "UpdateOffensiveStones" };

		if (!pHouse)
			return;

		auto& macro = GetMacroState(pHouse);
		const int now = Unsorted::CurrentFrame;

		if (now - macro.LastStoneCheckFrame < 150)
			return;

		macro.LastStoneCheckFrame = now;

		auto findOwnAt = [pHouse](const CellStruct& cell) -> BuildingClass*
		{
			for (auto const pB : pHouse->Buildings)
			{
				if (pB && pB->IsAlive && !pB->InLimbo && pB->Type && pB->GetMapCoords() == cell)
					return pB;
			}

			return nullptr;
		};

		std::vector<std::pair<CellStruct, int>> stillPending;

		for (auto const& [cell, frame] : macro.OffensiveStoneCells)
		{
			if (auto const pPlaced = findOwnAt(cell))
			{
				macro.OffensiveStones.push_back(pPlaced->UniqueID);
				CountDecision(NeuralDecision::CrawlStoneForward, pHouse->ArrayIndex);
			}
			else if (now - frame <= 1800)
			{
				stillPending.emplace_back(cell, frame);
			}
		}

		macro.OffensiveStoneCells = std::move(stillPending);

		std::vector<DWORD> alive;

		for (const DWORD id : macro.OffensiveStones)
		{
			bool found = false;

			for (auto const pB : pHouse->Buildings)
			{
				if (pB && pB->IsAlive && !pB->InLimbo && pB->UniqueID == id)
				{
					found = true;
					break;
				}
			}

			if (found)
				alive.push_back(id);
			else
				CountDecision(NeuralDecision::CrawlStoneLost, pHouse->ArrayIndex);
		}

		macro.OffensiveStones = std::move(alive);
	}

	// Cell of the newest forward stone of the offensive crawl still standing (the front), or Empty.
	CellStruct GetNewestOffensiveStoneCell(HouseClass* pHouse)
	{
		auto const& stones = GetMacroState(pHouse).OffensiveStones;

		if (stones.empty())
			return CellStruct::Empty;

		for (auto const pOwnB : pHouse->Buildings)
		{
			if (pOwnB && pOwnB->IsAlive && !pOwnB->InLimbo && pOwnB->UniqueID == stones.back())
				return pOwnB->GetMapCoords();
		}

		return CellStruct::Empty;
	}

	// Resource hoarder (Expert 6 at full strength, every house a little): the best ore field we
	// do not mine yet. Value = ore cells + 40 for a spawner tree (a renewable field); risk tier 0 =
	// no known enemy structure within 20 cells, 1 = one is near (only Expert 6 claims those, after
	// every tier 0 field); fields held by a known enemy refinery or ConYard (14 cells) are left to
	// the raid plans. Fog-fair: enemy structures count only when known. Only fields on the
	// landmass of one of our ConYards (the MCV / crawl can reach them).
	// The same pass grades the work there is for Expert 6 (ExpansionOpportunity, the selector
	// prior): explored free fields out of known enemy defense reach, by value over distance, the
	// best three; those beyond the crawl's reach (60 cells from any structure of ours) only count
	// while an expansion MCV can be built. Fields matter less the more we already mine.
	bool IsCellInKnownEnemyDefenseReach(HouseClass* pHouse, const CellStruct& cell, int marginCells);

	void UpdateResourceClaim(HouseClass* pHouse)
	{
		if (!pHouse)
			return;

		auto& macro = GetMacroState(pHouse);
		const int now = Unsorted::CurrentFrame;

		if (now - macro.LastResourceClaimFrame < 300)
			return;

		macro.LastResourceClaimFrame = now;
		macro.ResourceClaimCell = CellStruct::Empty;
		macro.ResourceClaimTreeCell = CellStruct::Empty;
		macro.ResourceClaimTier = -1;
		macro.ResourceClaimZone = -1;
		macro.ExpansionOpportunity = 0;
		macro.ClaimNeedsMCV = false;

		auto const pExt = HouseExt::Fetch(pHouse);
		const bool hoarder = pExt && pExt->NeuralAI_ActiveExpert == 6;

		std::vector<CellStruct> ownHubs;
		std::vector<std::pair<CellStruct, int>> ownAnchors; // cell, zone
		std::vector<CellStruct> ownStructures;

		for (auto const pBld : pHouse->Buildings)
		{
			if (!pBld || !pBld->IsAlive || pBld->InLimbo || !pBld->Type)
				continue;

			const CellStruct bc = pBld->GetMapCoords();

			if (pBld->Type->Refinery || pBld->Type->ResourceDestination)
				ownHubs.push_back(bc);

			if (pBld->Type->ConstructionYard)
				ownAnchors.emplace_back(bc, MapClass::Instance.GetMovementZoneType(bc, MovementZone::Normal, pBld->OnBridge));

			if (!pBld->Type->Wall && !pBld->Type->Insignificant && !pBld->Type->InvisibleInGame)
				ownStructures.push_back(bc);
		}

		if (ownAnchors.empty())
		{
			NoteTeamResourceClaim(pHouse, CellStruct::Empty);
			return;
		}

		// An expansion MCV is possible: a ground war factory and money for it, below the yard cap the
		// production uses (2 + free fields up to 3, + 1 with a big bank, at most 6).
		const int money = pHouse->Available_Money();
		const bool canSendMCV = pHouse->NumWarFactories > 0 && money >= 2500
			&& pHouse->NumConYards < std::clamp(2 + std::min(3, macro.UncolonizedOreFields) + (money >= 15000 ? 1 : 0), 2, 6);
		std::array<int, 3> bestWorth {};

		std::vector<CellStruct> enemyStructures;
		std::vector<CellStruct> enemyHubs;

		for (auto const pBld : BuildingClass::Array)
		{
			if (!pBld || !pBld->IsAlive || pBld->InLimbo || !pBld->Type || !pBld->Owner || pBld->Owner == pHouse
				|| pHouse->IsAlliedWith(pBld->Owner) || pBld->Owner->IsNeutral() || pBld->Type->Insignificant
				|| pBld->Type->InvisibleInGame || !IsEnemyBuildingKnown(pHouse, pBld))
			{
				continue;
			}

			const CellStruct bc = pBld->GetMapCoords();
			enemyStructures.push_back(bc);

			if (pBld->Type->Refinery || pBld->Type->ResourceDestination || pBld->Type->ConstructionYard)
				enemyHubs.push_back(bc);
		}

		auto chebyshev = [](const CellStruct& a, const CellStruct& b)
		{
			return std::max(std::abs(a.X - b.X), std::abs(a.Y - b.Y));
		};

		int bestMetric = INT_MAX;
		// Best field left to a teammate (team blackboard: an ally mines it or claimed it first).
		int bestYieldedMetric = INT_MAX;

		for (auto const& field : GetOreFields())
		{
			if (std::any_of(ownHubs.begin(), ownHubs.end(), [&](const CellStruct& h) { return chebyshev(h, field.Cell) <= 14; }))
				continue;

			if (std::any_of(enemyHubs.begin(), enemyHubs.end(), [&](const CellStruct& h) { return chebyshev(h, field.Cell) <= 14; }))
				continue;

			const int tier = std::any_of(enemyStructures.begin(), enemyStructures.end(),
				[&](const CellStruct& e) { return chebyshev(e, field.Cell) <= 20; }) ? 1 : 0;

			if (tier == 1 && !hoarder)
				continue;

			const int fieldZone = MapClass::Instance.GetMovementZoneType(field.Cell, MovementZone::Normal, false);
			int dist = INT_MAX;

			for (auto const& [anchorCell, anchorZone] : ownAnchors)
			{
				if (anchorZone == fieldZone)
					dist = std::min(dist, chebyshev(anchorCell, field.Cell));
			}

			if (dist == INT_MAX)
				continue;

			const int value = field.OreCells + (field.SpawnerTree ? 40 : 0);
			const int metric = tier * 100000 + dist * 3 - value;
			int crawlDist = INT_MAX;

			for (auto const& sc : ownStructures)
				crawlDist = std::min(crawlDist, chebyshev(sc, field.Cell));

			const bool beyondCrawl = crawlDist > 60;

			if (IsOreFieldTakenByTeam(pHouse, field))
			{
				bestYieldedMetric = std::min(bestYieldedMetric, metric);
				continue;
			}

			if (metric < bestMetric)
			{
				bestMetric = metric;
				macro.ResourceClaimCell = field.Cell;
				macro.ResourceClaimTier = tier;
				macro.ResourceClaimZone = fieldZone;
				macro.ClaimNeedsMCV = beyondCrawl;
			}

			// Work for Expert 6: only what it knows (explored ground) and can take now.
			if (beyondCrawl && !canSendMCV)
				continue;

			auto const pFieldCell = MapClass::Instance.TryGetCellAt(field.Cell);

			if (!pFieldCell || !IsCellVisibleToAI(pHouse, pFieldCell) || IsCellInKnownEnemyDefenseReach(pHouse, field.Cell, 3))
				continue;

			int worth = std::min(120, value) * 2 * 30 / std::max(30, dist);

			if (tier == 1)
				worth /= 2;

			for (auto& slot : bestWorth)
			{
				if (worth > slot)
					std::swap(worth, slot);
			}
		}

		// Each field more is worth less the more we already mine (5% per refinery of ours, 8% with a
		// bank of 30000+ where income stopped being the limit; denied ground still counts, so never
		// below a third): on maps with ore everywhere the three best fields always look perfect.
		int opportunity = bestWorth[0] + bestWorth[1] + bestWorth[2];
		const int perRefinery = money >= 30000 ? 8 : 5;
		opportunity = opportunity * std::max(34, 100 - perRefinery * static_cast<int>(ownHubs.size())) / 100;

		// Nothing to gather with: the money for the first step of a crawl or a refinery is missing.
		if (money < 1500 && pHouse->CountResourceGatherers == 0)
			opportunity /= 2;

		macro.ExpansionOpportunity = std::min(450, opportunity);

		if (bestYieldedMetric < bestMetric)
		{
			CountDecision(NeuralDecision::TeamClaimYield, pHouse->ArrayIndex);

			if (macro.ResourceClaimCell != CellStruct::Empty && OreBlockKey(macro.ResourceClaimCell) != TeamReports[pHouse->ArrayIndex].ClaimBlock)
			{
				Debug::Log("[NeuralAI] Frame %d | Team | House #%d leaves a better ore field to its team, claims (%d,%d)\n", now,
					pHouse->ArrayIndex, macro.ResourceClaimCell.X, macro.ResourceClaimCell.Y);
			}
		}

		NoteTeamResourceClaim(pHouse, macro.ResourceClaimCell);

		// The field's spawner tree (within 6 cells of its cell): its refinery goes next to it.
		if (macro.ResourceClaimCell != CellStruct::Empty)
		{
			int bestTree = INT_MAX;

			for (int dy = -6; dy <= 6; ++dy)
			{
				for (int dx = -6; dx <= 6; ++dx)
				{
					const CellStruct tc { static_cast<short>(macro.ResourceClaimCell.X + dx), static_cast<short>(macro.ResourceClaimCell.Y + dy) };

					if (auto const pCell = MapClass::Instance.TryGetCellAt(tc))
					{
						if (auto const pTerrain = pCell->GetTerrain(false);
							pTerrain && pTerrain->IsAlive && pTerrain->Type && pTerrain->Type->SpawnsTiberium
							&& std::abs(dx) + std::abs(dy) < bestTree)
						{
							bestTree = std::abs(dx) + std::abs(dy);
							macro.ResourceClaimTreeCell = tc;
						}
					}
				}
			}
		}
	}

	bool CanBuildUnderHumanRules(HouseClass* pHouse, TechnoTypeClass* pType);

	// A real structure a player can place: not a hidden switch, limbo upgrade or wall.
	bool IsPlayerFacingBuilding(BuildingTypeClass* pType)
	{
		return pType && pType->Selectable && !pType->InvisibleInGame && !pType->Insignificant && !pType->Wall
			&& !pType->Gate && pType->TechLevel >= 0 && pType->TechLevel <= 10;
	}

	void CollectPrerequisiteBuildings(int prereq, std::vector<int>& out, int depth);

	// Branch choices (map-wide, computed once per match): building types that exclude another
	// building through Prerequisite.Negative, or are excluded by one. Building one closes the other
	// branch (two tech labs, rival defense lines), so a player picks by taste, not by a fixed rule.
	std::vector<char> BranchChoiceBuildings;
	int BranchChoiceMatchSerial { -1 };
	bool BranchChoiceReady { false };

	bool IsBranchChoiceBuilding(int buildingIdx)
	{
		if (!BranchChoiceReady || BranchChoiceMatchSerial != NeuralMatchSerial
			|| static_cast<int>(BranchChoiceBuildings.size()) != BuildingTypeClass::Array.Count)
		{
			BranchChoiceReady = true;
			BranchChoiceMatchSerial = NeuralMatchSerial;
			BranchChoiceBuildings.assign(static_cast<size_t>(BuildingTypeClass::Array.Count), 0);

			std::vector<int> excluded;

			for (auto const pType : BuildingTypeClass::Array)
			{
				auto const pTypeExt = pType ? TechnoTypeExt::Fetch(pType) : nullptr;

				if (!pTypeExt || pTypeExt->Prerequisite_Negative.empty())
					continue;

				for (const int negative : pTypeExt->Prerequisite_Negative)
				{
					excluded.clear();
					CollectPrerequisiteBuildings(negative, excluded, 0);

					for (const int b : excluded)
					{
						if (b >= 0 && b < BuildingTypeClass::Array.Count)
						{
							BranchChoiceBuildings[static_cast<size_t>(b)] = 1;
							BranchChoiceBuildings[static_cast<size_t>(pType->ArrayIndex)] = 1;
						}
					}
				}
			}
		}

		return buildingIdx >= 0 && buildingIdx < static_cast<int>(BranchChoiceBuildings.size())
			&& BranchChoiceBuildings[static_cast<size_t>(buildingIdx)] != 0;
	}

	// Deterministic per-match taste in [-range, range] for one house and one type (game seed,
	// house, type): the same all match, different between matches, identical on every client.
	int MatchTaste(HouseClass* pHouse, int typeIdx, int range)
	{
		const uint32_t taste = (static_cast<uint32_t>(Game::Seed) * 2654435761u)
			^ (static_cast<uint32_t>(pHouse->ArrayIndex + 1) * 40503u)
			^ (static_cast<uint32_t>(typeIdx + 1) * 2246822519u);
		return static_cast<int>(taste % static_cast<uint32_t>(2 * range + 1)) - range;
	}

	// Building types that satisfy one prerequisite entry: the building itself (index >= 0), or the
	// members of a generic prerequisite group (index < 0: Phobos [GenericPrerequisites] when
	// defined, else the vanilla POWER / FACTORY / BARRACKS / RADAR / TECH / PROC lists).
	void CollectPrerequisiteBuildings(int prereq, std::vector<int>& out, int depth)
	{
		if (prereq >= 0)
		{
			out.push_back(prereq);
			return;
		}

		if (depth > 4)
			return;

		auto const pRulesExt = RulesExt::Global();
		const int group = -prereq;

		if (pRulesExt && group < pRulesExt->GenericPrerequisites.Count && pRulesExt->GenericPrerequisites.GetItem(group).Count > 0)
		{
			for (const int member : pRulesExt->GenericPrerequisites.GetItem(group))
				CollectPrerequisiteBuildings(member, out, depth + 1);

			return;
		}

		auto const pRules = RulesClass::Instance;

		if (!pRules)
			return;

		TypeList<int>* pList = nullptr;

		switch (prereq)
		{
		case -1: pList = &pRules->PrerequisitePower; break;
		case -2: pList = &pRules->PrerequisiteFactory; break;
		case -3: pList = &pRules->PrerequisiteBarracks; break;
		case -4: pList = &pRules->PrerequisiteRadar; break;
		case -5: pList = &pRules->PrerequisiteTech; break;
		case -6: pList = &pRules->PrerequisiteProc; break;
		default: break;
		}

		if (pList)
		{
			for (const int member : *pList)
				out.push_back(member);
		}
	}

	// Tech tree progress like a human player (every 900 frames): for each building type we do not
	// own, how many unit or building types of our tree (not buildable yet) list it, or a generic
	// group it belongs to, among their missing prerequisites. Read from the prerequisite lists, nothing named, so captured ConYards that
	// open foreign trees count the same way. HasBuildableTechUnlock: one of those unlockers can be
	// built right now (its own prerequisites are met), so the tech tree is not done yet.
	void UpdateTechUnlocks(HouseClass* pHouse)
	{
		if (!pHouse)
			return;

		auto& macro = GetMacroState(pHouse);
		const int now = Unsorted::CurrentFrame;

		if (now - macro.TechUnlockFrame < 900 && now >= macro.TechUnlockFrame)
			return;

		macro.TechUnlockFrame = now;
		macro.TechUnlocks.clear();
		macro.TechUnlockTypes.clear();
		macro.HasBuildableTechUnlock = false;

		std::vector<int> satisfiers;

		// Our tree: types available to our country (HouseExt::IsAvailableToHouse: owners, required
		// and forbidden houses, parent country), or owned by the country of a foreign ConYard we hold
		// (a captured one opens its owners' tree), within our tech level. "Not buildable yet" is
		// judged under human rules (CanBuildUnderHumanRules): for an AI house the game's CanBuild
		// skips prerequisites and even ownership, so it calls nearly everything buildable.
		DWORD foreignYardOwners = 0u;

		for (auto const pOwnBld : pHouse->Buildings)
		{
			if (pOwnBld && pOwnBld->IsAlive && pOwnBld->Type && pOwnBld->Type->ConstructionYard
				&& !HouseExt::IsAvailableToHouse(pHouse, pOwnBld->Type))
			{
				foreignYardOwners |= pOwnBld->Type->OwnerFlags;
			}
		}

		// Research: a superweapon that delivers hidden (limbo) buildings opens what lists them as
		// prerequisites. Those buildings are never on the sidebar, so the unlock counts for the
		// visible building that grants the superweapon. A two-step research (a hidden marker that
		// grants the finishing superweapon) is followed back to the visible building too.
		std::unordered_map<int, std::vector<int>> limboHosts;

		for (auto const pHostType : BuildingTypeClass::Array)
		{
			if (!pHostType || pHostType->TechLevel > pHouse->TechLevel)
				continue;

			auto const pHostExt = BuildingTypeExt::Fetch(pHostType);
			const int swSlots = 2 + static_cast<int>(pHostExt->SuperWeapons.size());

			for (int i = 0; i < swSlots; ++i)
			{
				auto const pSWType = SuperWeaponTypeClass::Array.GetItemOrDefault(pHostExt->GetSuperWeaponIndex(i));

				if (!pSWType)
					continue;

				for (auto const pDelivered : SWTypeExt::Fetch(pSWType)->LimboDelivery_Types)
				{
					if (pDelivered && pDelivered != pHostType && !IsPlayerFacingBuilding(pDelivered))
						limboHosts[pDelivered->ArrayIndex].push_back(pHostType->ArrayIndex);
				}
			}
		}

		// Visible buildings of our tree that grant (directly or through a hidden marker) the
		// superweapon delivering this hidden building, in building array order.
		auto researchHostsOf = [&](int limboIdx, std::vector<int>& out)
		{
			std::vector<int> pending { limboIdx };
			std::vector<int> seen;

			for (size_t step = 0; step < pending.size() && step < 16; ++step)
			{
				auto const it = limboHosts.find(pending[step]);

				if (it == limboHosts.end())
					continue;

				for (const int host : it->second)
				{
					if (std::find(seen.begin(), seen.end(), host) != seen.end())
						continue;

					seen.push_back(host);
					auto const pHost = BuildingTypeClass::Array.GetItem(host);

					if (!IsPlayerFacingBuilding(pHost))
						pending.push_back(host);
					else if (pHost->TechLevel >= 0
						&& (HouseExt::IsAvailableToHouse(pHouse, pHost) || (pHost->OwnerFlags & foreignYardOwners)))
						out.push_back(host);
				}
			}
		};

		auto scanType = [&](TechnoTypeClass* pType)
		{
			if (!pType || pType->TechLevel < 0 || pType->TechLevel > pHouse->TechLevel
				|| !(HouseExt::IsAvailableToHouse(pHouse, pType) || (pType->OwnerFlags & foreignYardOwners))
				|| CanBuildUnderHumanRules(pHouse, pType))
			{
				return;
			}

			std::vector<int> unlockers;

			for (const int prereq : pType->Prerequisite)
			{
				const bool met = prereq >= 0 ? HouseExt::HasBuildingPrerequisite(pHouse, prereq)
					: (RulesExt::Global() && -prereq < RulesExt::Global()->GenericPrerequisites.Count
						&& RulesExt::Global()->GenericPrerequisites.GetItem(-prereq).Count > 0
						? HouseExt::HasGenericPrerequisite(prereq, pHouse)
						: false);

				if (met)
					continue;

				satisfiers.clear();
				CollectPrerequisiteBuildings(prereq, satisfiers, 0);

				// A vanilla group we already own a member of is met as well.
				if (prereq < 0 && std::any_of(satisfiers.begin(), satisfiers.end(),
						[pHouse](int b) { return HouseExt::HasBuildingPrerequisite(pHouse, b); }))
				{
					continue;
				}

				for (const int b : satisfiers)
				{
					if (b < 0 || b >= BuildingTypeClass::Array.Count)
						continue;

					if (IsPlayerFacingBuilding(BuildingTypeClass::Array.GetItem(b)))
						unlockers.push_back(b);
					else
						researchHostsOf(b, unlockers);
				}
			}

			std::sort(unlockers.begin(), unlockers.end());
			unlockers.erase(std::unique(unlockers.begin(), unlockers.end()), unlockers.end());

			for (const int b : unlockers)
			{
				macro.TechUnlocks[b]++;
				macro.TechUnlockTypes[b].push_back(pType);
			}
		};

		for (auto const pType : InfantryTypeClass::Array)
			scanType(pType);

		for (auto const pType : UnitTypeClass::Array)
			scanType(pType);

		for (auto const pType : AircraftTypeClass::Array)
			scanType(pType);

		for (auto const pType : BuildingTypeClass::Array)
		{
			if (IsPlayerFacingBuilding(pType))
				scanType(pType);
		}

		for (auto const& [buildingIdx, count] : macro.TechUnlocks)
		{
			auto const pBld = BuildingTypeClass::Array.GetItem(buildingIdx);

			if (count > 0 && IsPlayerFacingBuilding(pBld) && pHouse->CountOwnedNow(pBld) == 0
				&& CanBuildUnderHumanRules(pHouse, pBld))
			{
				macro.HasBuildableTechUnlock = true;
				break;
			}
		}
	}

	// Progressive unit tier (every 150 frames): super units come in step by step, as the tech a
	// player can afford grows, even when the tech tree is complete from minute 0. The cap starts at
	// TechLevel 2 and rises one level every 3 / 2 / 1 game minutes (Easy / Normal / Hard), up to the
	// house's tech level (the map's limit), where it ends. It rises sooner when the house has seen an
	// enemy unit of a higher tier (seen tier + 1, fog-fair) or has lost half its peak buildings.
	void UpdateUnitTierCap(HouseClass* pHouse)
	{
		if (!pHouse)
			return;

		auto& macro = GetMacroState(pHouse);
		const int now = Unsorted::CurrentFrame;

		if (now - macro.LastUnitTierFrame < 150 && now >= macro.LastUnitTierFrame)
			return;

		macro.LastUnitTierFrame = now;

		for (auto const pTechno : TechnoClass::Array)
		{
			if (!pTechno || !pTechno->IsAlive || pTechno->InLimbo || !pTechno->Owner || pTechno->Owner == pHouse
				|| pTechno->Owner->IsNeutral() || pHouse->IsAlliedWith(pTechno->Owner)
				|| pTechno->WhatAmI() == AbstractType::Building || !IsCellVisibleToAI(pHouse, pTechno->GetCell()))
			{
				continue;
			}

			if (auto const pType = pTechno->GetTechnoType())
				macro.SeenEnemyUnitTier = std::max(macro.SeenEnemyUnitTier, std::clamp(pType->TechLevel, 0, 10));
		}

		macro.PeakOwnedBuildings = std::max(macro.PeakOwnedBuildings, pHouse->OwnedBuildings);

		const int diffIdx = static_cast<int>(pHouse->GetAIDifficultyIndex());
		const int framesPerTier = 900 * (diffIdx >= 2 ? 3 : (diffIdx == 1 ? 2 : 1));
		int cap = 2 + std::max(0, now) / framesPerTier;

		if (macro.SeenEnemyUnitTier > 0)
			cap = std::max(cap, macro.SeenEnemyUnitTier + 1);

		if (macro.PeakOwnedBuildings >= 8 && pHouse->OwnedBuildings * 2 < macro.PeakOwnedBuildings)
			cap++;

		macro.UnitTierCap = std::min(cap, std::max(0, pHouse->TechLevel));
	}

	// Whether the progressive tier lets this house build this unit now. Economy and utility units are
	// never held back (harvesters, MCVs and other deployers, engineers, unarmed transports, carryalls),
	// and a very expensive unit counts as at least tier 7 whatever its TechLevel says.
	bool IsUnitTierAllowed(HouseClass* pHouse, TechnoTypeClass* pType)
	{
		if (!pHouse || !pType)
			return false;

		auto const& macro = GetMacroState(pHouse);

		if (macro.UnitTierCap >= std::max(0, pHouse->TechLevel))
			return true;

		if (auto const pUnitType = abstract_cast<UnitTypeClass*>(pType))
		{
			if (pUnitType->Harvester || pUnitType->ResourceGatherer || pUnitType->DeploysInto)
				return true;
		}

		if (auto const pInfType = abstract_cast<InfantryTypeClass*>(pType); pInfType && pInfType->Engineer)
			return true;

		auto const pWeapon = pType->GetWeapon(0);
		const bool armed = pWeapon && pWeapon->WeaponType && pWeapon->WeaponType->Damage > 0;

		auto const pAirType = abstract_cast<AircraftTypeClass*>(pType);

		if (!armed && (pType->Passengers > 0 || (pAirType && pAirType->Carryall)))
			return true;

		const int tier = std::max(std::clamp(pType->TechLevel, 0, 10), pType->Cost >= 2000 ? 7 : 0);

		return tier <= macro.UnitTierCap;
	}

	// Production queues the bank governor may grow, in tie-break order.
	enum class BankQueue : int
	{
		Vehicle,
		Infantry,
		Aircraft,
		Naval,

		Count
	};

	// Bank governor (every 450 frames). A production queue builds one object at a time, so its
	// spending rate is fixed by the rules (BuildSpeed: minutes per 1000 credits) and the factory
	// count (each extra factory of the kind divides the build time by MultipleFactory), whatever
	// the object costs. A bank beyond the reserve is wasted strength when the queues cannot spend
	// it: the governor compares what they can spend with the measured income plus the surplus
	// spread over 9000 frames, and while they fall short it wants one more factory at a time in
	// the slowest queue until they would not. Only a queue that kept producing (85% of the AI
	// ticks since the last sample) gets one: a queue that waits for something to build (army caps,
	// no target) does not spend faster with more factories. Airfields are never added: they are
	// hangars that follow the docks the aircraft need. Only game state is read (money spent
	// and held, factory counts, factories producing, rules), so every machine computes the same
	// wants.
	void UpdateBankGovernor(HouseClass* pHouse)
	{
		if (!pHouse)
			return;

		auto& macro = GetMacroState(pHouse);
		const int now = Unsorted::CurrentFrame;
		const int money = pHouse->Available_Money();
		const int earned = pHouse->CreditsSpent + money;

		if (macro.BankIncomeFrame < 0 || now < macro.BankIncomeFrame)
		{
			macro.BankIncomeFrame = now;
			macro.BankIncomeEarned = earned;

			return;
		}

		const int elapsed = now - macro.BankIncomeFrame;

		if (elapsed < 450)
			return;

		const int sample = static_cast<int>(std::max<int64_t>(0, static_cast<int64_t>(earned - macro.BankIncomeEarned)) * 1000 / elapsed);
		macro.BankIncomeRate = macro.BankIncomeRate < 0 ? sample : (macro.BankIncomeRate + sample) / 2;
		macro.BankIncomeFrame = now;
		macro.BankIncomeEarned = earned;
		macro.BankGovernorActive = false;
		macro.BankWantedFactories.fill(0);

		std::array<bool, static_cast<size_t>(BankQueue::Count)> queueKeptBusy {};

		for (size_t q = 0; q < queueKeptBusy.size(); ++q)
			queueKeptBusy[q] = macro.BankQueueTicks[q] > 0 && macro.BankQueueBusyTicks[q] * 100 >= macro.BankQueueTicks[q] * 85;

		macro.BankQueueTicks.fill(0);
		macro.BankQueueBusyTicks.fill(0);
		macro.BankWantedYards = 0;
		macro.BankSurplus = 0;

		RulesClass* const pRules = RulesClass::Instance;

		if (!pRules || pRules->BuildSpeed <= 0.0 || pHouse->NumConYards <= 0 || macro.BankIncomeRate <= 0)
			return;

		// Credits one factory spends per 1000 frames (900 frames per game minute), and the gain
		// of each extra factory in permille.
		const int64_t baseRate = static_cast<int64_t>(1000000.0 / (pRules->BuildSpeed * 900.0));
		const int multiplePermille = static_cast<int>(pRules->MultipleFactory * 1000.0f);

		if (baseRate <= 0 || multiplePermille <= 0 || multiplePermille >= 1000)
			return;

		auto const queueRate = [baseRate, multiplePermille](int factories) -> int64_t
		{
			int64_t rate = baseRate;

			for (int i = 1; i < factories && rate < 100000000; ++i)
				rate = rate * 1000 / multiplePermille;

			return factories > 0 ? rate : 0;
		};

		std::array<int, static_cast<size_t>(BankQueue::Count)> counts {};
		counts[static_cast<size_t>(BankQueue::Vehicle)] = pHouse->NumWarFactories;
		counts[static_cast<size_t>(BankQueue::Infantry)] = pHouse->NumBarracks;
		counts[static_cast<size_t>(BankQueue::Aircraft)] = pHouse->NumAirpads;
		counts[static_cast<size_t>(BankQueue::Naval)] = HouseNeedsNaval(pHouse) ? pHouse->NumShipyards : 0;

		int64_t capacity = queueRate(pHouse->NumConYards);

		for (auto const count : counts)
			capacity += queueRate(count);

		// The reserve covers an expansion MCV, ferries and a raid on the harvesters: plans and
		// economy rebuilds never wait for money the governor spent.
		const int reserve = 15000 + macro.BankIncomeRate;
		const int64_t surplus = static_cast<int64_t>(money) - reserve;
		const int64_t needed = macro.BankIncomeRate + std::max<int64_t>(0, surplus) * 1000 / 9000;
		macro.BankSpendCapacity = static_cast<int>(std::min<int64_t>(capacity, INT_MAX));
		macro.BankSpendNeeded = static_cast<int>(std::min<int64_t>(needed, INT_MAX));
		macro.BankSurplus = static_cast<int>(std::clamp<int64_t>(surplus, 0, INT_MAX));

		if (surplus <= 0 || needed <= capacity)
			return;

		auto wanted = counts;

		for (int added = 0; added < 8 && capacity < needed; ++added)
		{
			int best = -1;

			for (int q = 0; q < static_cast<int>(BankQueue::Count); ++q)
			{
				if (q != static_cast<int>(BankQueue::Aircraft) && counts[q] > 0 && queueKeptBusy[q]
					&& (best < 0 || wanted[q] < wanted[best]))
				{
					best = q;
				}
			}

			if (best < 0)
				break;

			capacity += queueRate(wanted[best] + 1) - queueRate(wanted[best]);
			wanted[best]++;
		}

		macro.BankGovernorActive = true;
		macro.BankWantedFactories = wanted;

		// The building queue builds those factories one at a time: while three or more are wanted,
		// one more ConYard (its own MultipleFactory gain), up to four, six from 100000 credits
		// beyond the reserve.
		int factoryDeficit = 0;

		for (int q = 0; q < static_cast<int>(BankQueue::Count); ++q)
			factoryDeficit += wanted[q] - counts[q];

		if (factoryDeficit >= 3 && pHouse->NumConYards < (surplus >= 100000 ? 6 : 4))
			macro.BankWantedYards = pHouse->NumConYards + 1;
	}

	// Credits beyond the governor's reserve that make a house play offensively: losses are bought
	// back at once, so waves leave with less advantage, forward stones go toward the enemy and an
	// enemy across the water is harassed from range and invaded with a beachhead.
	constexpr int OffensiveBankSurplus = 25000;

	bool HasOffensiveBank(HouseClass* pHouse)
	{
		return pHouse && GetMacroState(pHouse).BankSurplus >= OffensiveBankSurplus;
	}

	// Factories the bank governor still wants in this queue (0 when it is idle).
	int BankFactoryDeficit(HouseClass* pHouse, BankQueue queue, int owned)
	{
		auto const& macro = GetMacroState(pHouse);

		if (!macro.BankGovernorActive)
			return 0;

		return std::max(0, macro.BankWantedFactories[static_cast<size_t>(queue)] - owned);
	}

	int VisibleEnemyThreatNear(HouseClass* pHouse, const CellStruct& cell, int radius);
	bool IsBuildingEnterableFromZone(const BuildingClass* pBld, int zone, MovementZone movementZone = MovementZone::Normal);
	void OrderFootToEnter(FootClass* pFoot, TechnoClass* pEnterTarget);

	// Expert 0 (Economy & Crisis) bookkeeping and actions, every 150 frames. The peaks of static
	// refineries and harvesters are tracked for every house; EconomyRecoveryActive is set when a raid
	// cost a third of either (production then rebuilds the economy first while Expert 0 runs).
	// Any house, whatever the expert (without income it is dead anyway):
	// - Broke: no income and less than a harvester's price: sell the least useful structure, one per
	//   600 frames: a factory copy, a defense copy far from any known enemy structure, a power plant
	//   copy the power surplus can spare, any defense copy, then a lone support structure that is no
	//   factory. Never a ConYard or a refinery.
	// - Harvesters under fire: a damaged one goes home to a refinery with no enemy in sight.
	// - EconomyTrouble: the selector prior of Expert 0 (what it could fix now; 0 vetoes it).
	// While Expert 0 is in charge it also acts:
	// - Harvesters under fire go home at the first enemy seen near them, damaged or not.
	// - Blackout: with low power, high-drain structures that are neither factories nor defenses go
	//   offline (the existing micro brings them back online once the surplus is >= 150).
	// - Harvester balance: a refinery with 3+ harvesters waiting next to it sends one to the ore of a
	//   refinery of ours that has none (one per 450 frames).
	int GroundRangeOf(TechnoTypeClass* pType);

	// Emergency sale on a repair station, as a player can do (Phobos: a unit standing on a building
	// with UnitSell, default UnitRepair, is sold; Unsellable / UnitsUnsellable respected). While the
	// income has to be restored and the credits do not cover it, the least needed sellable vehicle
	// (armed, no harvester, deployer, transport or naval unit, in no plan, 300+ credits; most
	// expensive first, nearer first) drives onto our nearest such station and is sold there, one
	// at a time, until the credits reach targetMoney.
	void UpdateDepotEmergencySale(HouseClass* pHouse, bool needed, int targetMoney)
	{
		auto& macro = GetMacroState(pHouse);

		if (!needed || targetMoney == INT_MAX || pHouse->Available_Money() >= targetMoney)
		{
			macro.DepotSaleUnit = 0;
			return;
		}

		std::vector<BuildingClass*> depots;

		for (auto const pBld : pHouse->Buildings)
		{
			if (pBld && pBld->IsAlive && !pBld->InLimbo && pBld->Type && !pBld->Type->Naval
				&& BuildingTypeExt::ExtMap.Find(pBld->Type)->UnitSell.Get(pBld->Type->UnitRepair))
			{
				depots.push_back(pBld);
			}
		}

		if (depots.empty())
		{
			macro.DepotSaleUnit = 0;
			return;
		}

		auto const sellable = [pHouse, &macro](UnitClass* pUnit)
		{
			auto const pType = pUnit ? pUnit->Type : nullptr;

			if (!pType || pUnit->Owner != pHouse || !pUnit->IsAlive || pUnit->InLimbo || pType->Naval || pType->Harvester
				|| pType->ResourceGatherer || pType->DeploysInto || IsAmphibiousOrAirTransport(pType) || pType->Cost < 300
				|| pUnit->Passengers.NumPassengers > 0 || GroundRangeOf(pType) <= 0 || macro.PlanMembers.contains(pUnit->UniqueID)
				|| IsAirborneThreatTechno(pUnit))
			{
				return false;
			}

			auto const pTypeExt = TechnoTypeExt::ExtMap.Find(pType);

			return !(pTypeExt && pTypeExt->Unsellable.Get(RulesExt::Global()->UnitsUnsellable));
		};

		UnitClass* pSale = nullptr;

		for (auto const pUnit : UnitClass::Array)
		{
			if (pUnit && pUnit->UniqueID == macro.DepotSaleUnit)
			{
				pSale = sellable(pUnit) ? pUnit : nullptr;
				break;
			}
		}

		auto const nearestDepot = [&depots](UnitClass* pUnit)
		{
			BuildingClass* pBest = nullptr;
			int bestDist = INT_MAX;

			for (auto const pDepot : depots)
			{
				const int d = pUnit->DistanceFrom(pDepot);

				if (d < bestDist)
				{
					bestDist = d;
					pBest = pDepot;
				}
			}

			return pBest;
		};

		if (!pSale)
		{
			int bestScore = INT_MIN;

			for (auto const pUnit : UnitClass::Array)
			{
				if (!sellable(pUnit))
					continue;

				auto const pDepot = nearestDepot(pUnit);
				const int score = pUnit->Type->Cost - pUnit->DistanceFrom(pDepot) / Unsorted::LeptonsPerCell * 20;

				if (score > bestScore || (score == bestScore && pSale && pUnit->UniqueID < pSale->UniqueID))
				{
					bestScore = score;
					pSale = pUnit;
				}
			}

			if (!pSale)
				return;

			macro.DepotSaleUnit = pSale->UniqueID;
		}

		auto const pDepot = nearestDepot(pSale);
		auto const pCell = pSale->GetCell();

		if (pCell && std::find(depots.begin(), depots.end(), pCell->GetBuilding()) != depots.end())
		{
			pSale->Sell(-1);
			macro.DepotSaleUnit = 0;
			CountDecision(NeuralDecision::DepotEmergencySale, pHouse->ArrayIndex);
		}
		else if (pSale->CurrentMission != Mission::Enter)
		{
			OrderFootToEnter(pSale, pDepot);
		}
	}

	void UpdateEconomyCrisis(HouseClass* pHouse)
	{
		if (!pHouse || pHouse->Defeated)
			return;

		auto& macro = GetMacroState(pHouse);
		const int now = Unsorted::CurrentFrame;

		if (now - macro.LastEconomyCrisisFrame < 150 && now >= macro.LastEconomyCrisisFrame)
			return;

		macro.LastEconomyCrisisFrame = now;

		std::vector<BuildingClass*> refineries;
		std::vector<UnitClass*> harvesters;
		int deployedGatherers = 0;
		int cashBuildings = 0;

		for (auto const pBld : pHouse->Buildings)
		{
			if (!pBld || !pBld->IsAlive || pBld->InLimbo || !pBld->Type || pBld->CurrentMission == Mission::Selling)
				continue;

			if (pBld->Type->ProduceCashAmount > 0)
				cashBuildings++;

			// A deployed slave miner (ResourceGatherer, Yuri) gathers by itself: counted with the
			// gatherers, not the refineries, so deploying one never looks like a lost harvester.
			if (pBld->Type->ResourceGatherer)
				deployedGatherers++;
			else if (pBld->Type->Refinery || pBld->Type->ResourceDestination)
				refineries.push_back(pBld);
		}

		for (auto const pUnit : UnitClass::Array)
		{
			// Mobile slave miners (they deploy into their refinery) gather too. Not one in limbo: the
			// harvester a factory is building exists already, waiting for the credits it needs.
			if (pUnit && pUnit->Owner == pHouse && pUnit->IsAlive && !pUnit->InLimbo && pUnit->Type
				&& (pUnit->Type->Harvester || pUnit->Type->ResourceGatherer
					|| (pUnit->Type->DeploysInto && (pUnit->Type->DeploysInto->Refinery || pUnit->Type->DeploysInto->ResourceGatherer))))
			{
				harvesters.push_back(pUnit);
			}
		}

		const int refCount = static_cast<int>(refineries.size());
		const int harvCount = static_cast<int>(harvesters.size()) + deployedGatherers;
		macro.PeakRefineries = std::max(macro.PeakRefineries, refCount);
		macro.PeakHarvesters = std::max(macro.PeakHarvesters, harvCount);
		macro.EconomyRecoveryActive = (macro.PeakRefineries >= 2 && refCount * 3 < macro.PeakRefineries * 2)
			|| (macro.PeakHarvesters >= 3 && harvCount * 3 < macro.PeakHarvesters * 2);

		auto const pExt = HouseExt::Fetch(pHouse);

		if (!pExt)
			return;

		const bool expertInCharge = pExt->NeuralAI_ActiveExpert == 0;

		// Harvesters under fire: an armed enemy in our sight within 7 cells. A damaged one (any one
		// while Expert 0 is in charge) goes home to the nearest refinery it can reach with no enemy in
		// sight around it, so the load is not lost and the guards there take over. One order per
		// harvester per 600 frames. Chrono miners are counted but never ordered (an order makes
		// them loop-teleport); slave miners deploy where they stop.
		int underFire = 0;
		{
			std::unordered_map<DWORD, int> keptRetreats;

			for (auto const pHarv : harvesters)
			{
				if (!pHarv->Type->Harvester || pHarv->Type->DeploysInto || pHarv->IsInAir())
					continue;

				const CellStruct hc = pHarv->GetMapCoords();

				if (VisibleEnemyThreatNear(pHouse, hc, 7) <= 0)
					continue;

				underFire++;

				if (pHarv->Type->Teleporter)
					continue;

				auto const lastIt = macro.HarvesterRetreatFrames.find(pHarv->UniqueID);
				const bool recentOrder = lastIt != macro.HarvesterRetreatFrames.end() && now - lastIt->second < 600;

				if (recentOrder)
					keptRetreats.emplace(pHarv->UniqueID, lastIt->second);

				if (recentOrder || (!expertInCharge && pHarv->GetHealthPercentage() >= 0.9)
					|| pHarv->CurrentMission == Mission::Enter || pHarv->CurrentMission == Mission::Unload
					|| pHarv->QueuedMission == Mission::Enter || pHarv->HasAnyLink())
				{
					continue;
				}

				const int harvZone = ObjectZone(pHarv, pHarv->Type->MovementZone);
				BuildingClass* pHome = nullptr;
				BuildingClass* pNearestHome = nullptr;
				// Distance plus the enemy route heat the straight way home crosses (40 heat = 1 cell).
				int bestCost = INT_MAX;
				int nearestDist = INT_MAX;

				for (auto const pRef : refineries)
				{
					if (!pRef->Type->Refinery || !IsBuildingEnterableFromZone(pRef, harvZone))
						continue;

					const int dist = static_cast<int>(pRef->GetMapCoords().DistanceFrom(hc));

					if (dist < 6 || dist > 60 || (dist >= bestCost && dist >= nearestDist)
						|| VisibleEnemyThreatNear(pHouse, pRef->GetMapCoords(), 8) > 0)
					{
						continue;
					}

					if (dist < nearestDist)
					{
						nearestDist = dist;
						pNearestHome = pRef;
					}

					const int cost = dist + RouteHeatAlong(pHouse, hc, pRef->GetMapCoords()) / 40;

					if (cost < bestCost)
					{
						bestCost = cost;
						pHome = pRef;
					}
				}

				if (!pHome)
					continue;

				if (pHome != pNearestHome)
					CountDecision(NeuralDecision::RouteHarvesterDetour, pHouse->ArrayIndex);

				OrderFootToEnter(pHarv, pHome);
				keptRetreats[pHarv->UniqueID] = now;
				CountDecision(NeuralDecision::HarvesterRetreat, pHouse->ArrayIndex);
			}

			macro.HarvesterRetreatFrames = std::move(keptRetreats);
		}

		macro.HarvestersUnderFire = underFire;

		// Refineries no harvester serves while another has 3+ waiting next to it (the balance below
		// has work), and storage nearly full while it limits our income.
		BuildingClass* pCrowded = nullptr;
		BuildingClass* pIdle = nullptr;
		int starvedRefineries = 0;

		if (refCount >= 2)
		{
			int crowdedQueue = 2;

			for (auto const pRef : refineries)
			{
				int queuedNear = 0;
				bool anyAssigned = false;

				for (auto const pHarv : harvesters)
				{
					const int dist = pHarv->DistanceFrom(pRef);

					if (dist <= 6 * 256)
						queuedNear++;

					if (dist <= 14 * 256)
						anyAssigned = true;
				}

				if (queuedNear > crowdedQueue)
				{
					crowdedQueue = queuedNear;
					pCrowded = pRef;
				}

				if (!anyAssigned)
				{
					starvedRefineries++;

					if (!pIdle)
						pIdle = pRef;
				}
			}
		}

		macro.StarvedRefineries = pCrowded ? starvedRefineries : 0;

		// Storage only limits income where a refinery keeps the harvest (Refinery.UseStorage, the
		// same rule a human player has): 85% full means the next unloads will be lost.
		bool storageLimited = false;

		for (auto const pBld : pHouse->Buildings)
		{
			if (pBld->IsOnMap && !pBld->InLimbo && BuildingTypeExt::Fetch(pBld->Type)->Refinery_UseStorage)
			{
				storageLimited = true;
				break;
			}
		}

		bool nearlyFull = storageLimited && pHouse->TotalStorage > 0
			&& pHouse->OwnedTiberium.GetTotalAmount() >= static_cast<float>(pHouse->TotalStorage) * 0.85f;

		// Only trouble Expert 0 can fix: a silo we can build now (a silo may need a refinery type
		// the storage option disables, so a full house can have none).
		if (nearlyFull)
		{
			nearlyFull = std::any_of(BuildingTypeClass::Array.begin(), BuildingTypeClass::Array.end(), [pHouse](BuildingTypeClass* pType)
				{
					return pType && pType->Storage > 0 && !pType->Refinery && !pType->ResourceGatherer && !pType->ResourceDestination
						&& CanBuildUnderHumanRules(pHouse, pType);
				});
		}

		macro.StorageNearlyFull = nearlyFull;

		if (pHouse->HasLowPower())
		{
			if (macro.LowPowerSinceFrame < 0 || macro.LowPowerSinceFrame > now)
				macro.LowPowerSinceFrame = now;
		}
		else
		{
			macro.LowPowerSinceFrame = -1;
		}

		// Blackout.
		if (expertInCharge && pHouse->HasLowPower())
		{
			for (auto const pBld : pHouse->Buildings)
			{
				if (pBld && pBld->IsAlive && !pBld->InLimbo && pBld->Type && pBld->Type->TogglePower && pBld->StuffEnabled
					&& !pBld->IsIronCurtained() && !pBld->Type->IsBaseDefense && pBld->Type->Factory == AbstractType::None
					&& !pBld->Type->ConstructionYard && !pBld->Type->Refinery && pBld->Type->PowerDrain >= 100)
				{
					pBld->GoOffline();
				}
			}
		}

		// Broke.
		int cheapestHarvester = INT_MAX;

		for (auto const pType : UnitTypeClass::Array)
		{
			if (pType && (pType->Harvester || pType->ResourceGatherer)
				&& (!pType->DeploysInto || pType->DeploysInto->Refinery || pType->DeploysInto->ResourceGatherer)
				&& CanBuildUnderHumanRules(pHouse, pType))
				cheapestHarvester = std::min(cheapestHarvester, pType->GetActualCost(pHouse));
		}

		const bool harvesterBuildable = cheapestHarvester != INT_MAX;

		// Without a refinery no harvester can be built: the cheapest refinery (with its free
		// harvester) is then what the credits are kept for.
		if (!harvesterBuildable && refCount == 0)
		{
			for (auto const pType : BuildingTypeClass::Array)
			{
				if (pType && (pType->Refinery || pType->ResourceGatherer) && CanBuildUnderHumanRules(pHouse, pType))
					cheapestHarvester = std::min(cheapestHarvester, pType->GetActualCost(pHouse));
			}
		}

		// No income: nothing gathers (harvesters, slave miners deployed or not).
		const bool noIncome = harvCount == 0;

		// Broke hold: while nothing gathers, no cash building pays and a harvester (or, with no
		// refinery left, a refinery) can be built, every production that does not restore income stops
		// spending, so the last credits and what a broke sale returns go to that one item. A refinery
		// waits while a harvester can be built (it costs more for the same income), a power plant
		// goes on under low power. The vehicle in the war factory is cancelled (refunded; the harvester
		// needs that queue), the others are put on hold and resumed once something gathers.
		const bool holdForHarvester = noIncome && cashBuildings == 0 && cheapestHarvester != INT_MAX;
		macro.BrokeHoldActive = holdForHarvester;

		// Dry economy: we own gatherers and refineries, yet no unload has paid for 3600 frames (never
		// one after frame 9000) while we hold less than 1500 credits: the fields within reach are
		// gone (match #6526, Siberian Wastes, minute 129: three houses at $2-$24 with 5-8
		// harvesters each and no ore left on the map). No more harvesters or refineries then, and
		// every 300 frames one building that earns nothing more is sold for an army (SellForDryEconomy).
		const int lastHarvest = macro.LastHarvestIncomeFrame;
		const int money = pHouse->Available_Money();
		// The fields themselves must be gone too: half or more of our refineries have fewer than 6
		// ore cells around them (Production.inl depletion check), so harvesters chased off by an
		// attack for a while never trigger it.
		const bool fieldsDepleted = macro.RefineriesChecked > 0 && macro.RefineriesDepleted * 2 >= macro.RefineriesChecked;
		macro.DryEconomyActive = harvCount > 0 && refCount > 0 && cashBuildings == 0 && money < 1500 && now > 9000
			&& fieldsDepleted && (lastHarvest < 0 || now - lastHarvest > 3600);

		if (macro.DryEconomyActive && now - macro.LastDrySaleFrame >= 300)
		{
			if (SellForDryEconomy(pHouse))
				macro.LastDrySaleFrame = now;
		}

		// Emergency money from units sold on a repair station: for the item that restores income,
		// or for an MCV when no ConYard and no MCV is left.
		{
			bool hasMcv = false;
			int mcvCost = INT_MAX;

			for (auto const pUnit : UnitClass::Array)
			{
				if (pUnit && pUnit->Owner == pHouse && pUnit->IsAlive && pUnit->Type && pUnit->Type->DeploysInto
					&& pUnit->Type->DeploysInto->ConstructionYard)
				{
					hasMcv = true;
					break;
				}
			}

			if (pHouse->ConYards.Count == 0 && !hasMcv)
			{
				for (auto const pType : UnitTypeClass::Array)
				{
					if (pType && pType->DeploysInto && pType->DeploysInto->ConstructionYard && CanBuildUnderHumanRules(pHouse, pType))
						mcvCost = std::min(mcvCost, pType->GetActualCost(pHouse));
				}
			}

			const bool needMcv = mcvCost != INT_MAX;
			const bool savingForExpansion = !holdForHarvester && !needMcv && macro.ExpansionSaveTarget > 0;
			UpdateDepotEmergencySale(pHouse, holdForHarvester || needMcv || savingForExpansion,
				needMcv ? mcvCost : (savingForExpansion ? macro.ExpansionSaveTarget : cheapestHarvester));
		}

		// Economy hold: an economic investment the production wants and cannot pay yet, in this
		// order: a harvester (fewer than refineries), a refinery for a newly reached field, the expansion
		// MCV when our fields ran dry. Until the credits reach it the other production waits
		// (suspended, never cancelled), except harvesters, refineries, MCVs, transports, income,
		// power under low power, and nothing waits while our base is under attack. A hold that
		// bought nothing for 3600 frames rests 1800 (the investment may be blocked elsewhere).
		// Without it the trickle of income went to the army and the investment never came.
		const bool baseUnderAttack = pHouse->LATime > 0 && now - pHouse->LATime < 1350;
		const int economyTarget = macro.HarvesterSaveTarget > 0 ? macro.HarvesterSaveTarget
			: (macro.RefinerySaveTarget > 0 ? macro.RefinerySaveTarget : macro.ExpansionSaveTarget);
		bool holdForEconomy = !holdForHarvester && !baseUnderAttack && economyTarget > 0
			&& pHouse->Available_Money() < economyTarget && now >= macro.EconomyHoldPauseUntil;
		// Saving for the island MCV on a slow trickle of income takes longer than 3600 frames, and the
		// rest after it gave the credits back to the army before the MCV was ever paid.
		const bool islandHold = macro.IslandExpansionUrgent && economyTarget == macro.ExpansionSaveTarget;

		// Island expansion (UpdateIslandOreOutlook) and the fields we hold nearly gone: every 450
		// frames one building that earns nothing more is sold toward the island MCV.
		// SellForDryEconomy keeps ConYards, naval buildings, defenses and the last of each type.
		if (islandHold && macro.ExpansionSaveTarget > 0 && pHouse->Available_Money() < macro.ExpansionSaveTarget
			&& !macro.DryEconomyActive && now - macro.LastIslandSaleFrame >= 450
			&& (macro.IslandHomeOre < 3000 || (macro.IslandOreRunway >= 0 && macro.IslandOreRunway < 2700)))
		{
			const int moneyBefore = pHouse->Available_Money();
			macro.LastIslandSaleFrame = now;

			if (SellForDryEconomy(pHouse))
			{
				Debug::Log("[NeuralAI] House %d IslandExpansion: home ore %d, %d of %d credits for the MCV, that sale goes to it, frame %d.\n",
					pHouse->ArrayIndex, macro.IslandHomeOre, moneyBefore, macro.ExpansionSaveTarget, now);
			}
		}

		if (!holdForEconomy)
		{
			macro.EconomyHoldSince = -1;
		}
		else if (macro.EconomyHoldSince < 0 || macro.EconomyHoldSince > now)
		{
			macro.EconomyHoldSince = now;
		}
		else if (now - macro.EconomyHoldSince > (islandHold ? 9000 : 3600))
		{
			macro.EconomyHoldPauseUntil = now + 1800;
			macro.EconomyHoldSince = -1;
			holdForEconomy = false;
		}

		const bool holdForExpansion = holdForEconomy;
		const bool holdProduction = holdForHarvester || holdForEconomy;

		// No repairs while broke: wrench repairs stop, units on a repair station (service depot,
		// shipyard) step off it.
		if (holdForHarvester)
		{
			for (auto const pBld : pHouse->Buildings)
			{
				if (!pBld || !pBld->IsAlive || pBld->InLimbo || !pBld->Type)
					continue;

				if (pBld->IsBeingRepaired)
					pBld->SetRepairState(0);

				if (!pBld->Type->UnitRepair)
					continue;

				const CellStruct exitCell = pBld->GetMapCoords() + CellStruct { static_cast<short>(pBld->Type->GetFoundationWidth() + 2), 0 };

				for (int i = 0; pBld->RadioLinks.IsAllocated && i < pBld->RadioLinks.Capacity; ++i)
				{
					auto const pDocked = abstract_cast<FootClass*>(pBld->GetNthLink(i));

					if (!pDocked || !pDocked->IsAlive || pDocked->Owner != pHouse || pDocked->GetHealthPercentage() >= 1.0
						|| pDocked->UniqueID == macro.DepotSaleUnit)
					{
						continue;
					}

					if (auto const pExitCell = MapClass::Instance.TryGetCellAt(exitCell))
					{
						pDocked->SetDestination(pExitCell, true);
						pDocked->QueueMission(Mission::Move, false);
					}
				}
			}
		}

		for (auto const pFactory : FactoryClass::Array)
		{
			if (!pFactory || pFactory->Owner != pHouse || !pFactory->Object)
				continue;

			auto const pObject = pFactory->Object;
			auto const pType = pObject->GetTechnoType();
			const DWORD objectId = pObject->UniqueID;
			auto const held = std::find(macro.BrokeHeldObjects.begin(), macro.BrokeHeldObjects.end(), objectId);

			if (!holdProduction)
			{
				if (held != macro.BrokeHeldObjects.end() && pFactory->IsSuspended)
					pFactory->Unsuspend(true);

				continue;
			}

			if (!pType || pFactory->IsSuspended || pFactory->IsDone() || held != macro.BrokeHeldObjects.end())
				continue;

			auto const pUnitType = abstract_cast<UnitTypeClass*>(pType);
			auto const pBldType = abstract_cast<BuildingTypeClass*>(pType);
			auto const pInfType = abstract_cast<InfantryTypeClass*>(pType);
			const bool restoresIncome = (pUnitType
					&& (pUnitType->Harvester || pUnitType->ResourceGatherer
						|| (pUnitType->DeploysInto
							&& (pUnitType->DeploysInto->Refinery || pUnitType->DeploysInto->ResourceGatherer
								|| (pUnitType->DeploysInto->ConstructionYard && pHouse->ConYards.Count == 0)))))
				|| (pBldType && (((pBldType->Refinery || pBldType->ResourceGatherer) && !harvesterBuildable)
					|| pBldType->ProduceCashAmount > 0
					|| (pBldType->PowerBonus > pBldType->PowerDrain && pHouse->HasLowPower())))
				|| (pInfType && pInfType->Engineer)
				|| (holdForExpansion && pUnitType
					&& (IsAmphibiousOrAirTransport(pUnitType) || (pUnitType->DeploysInto && pUnitType->DeploysInto->ConstructionYard)))
				// The island MCV and its ride come out of the first war factory and shipyard, and the MCV
				// needs the buildings of its prerequisite list we lack.
				|| (holdForExpansion && macro.IslandExpansionUrgent && pBldType
					&& ((pBldType->Factory == AbstractType::UnitType && (pBldType->Naval ? pHouse->NumShipyards == 0 : pHouse->NumWarFactories == 0))
						|| std::find(macro.IslandMcvPrerequisites.begin(), macro.IslandMcvPrerequisites.end(), pBldType->ArrayIndex)
							!= macro.IslandMcvPrerequisites.end()))
				|| (holdForEconomy && pBldType && (pBldType->Refinery || pBldType->ResourceGatherer));

			if (restoresIncome)
				continue;

			if (pUnitType && !pUnitType->Naval && holdForHarvester)
			{
				Debug::Log("[NeuralAI] House %d broke: cancelling %s for a harvester.\n", pHouse->ArrayIndex, pType->ID);
				pFactory->AbandonProduction();
			}
			else
			{
				pFactory->Suspend(true);
				macro.BrokeHeldObjects.push_back(objectId);
			}
		}

		if (!holdProduction)
			macro.BrokeHeldObjects.clear();

		if (noIncome && cheapestHarvester != INT_MAX && pHouse->Available_Money() < cheapestHarvester
			&& now - macro.LastBrokeSaleFrame >= 600)
		{
			std::map<int, int> typeCounts;

			for (auto const pBld : pHouse->Buildings)
			{
				if (pBld && pBld->IsAlive && !pBld->InLimbo && pBld->Type && pBld->CurrentMission != Mission::Selling)
					typeCounts[pBld->Type->ArrayIndex]++;
			}

			std::vector<CellStruct> enemyCells;

			for (auto const pEB : BuildingClass::Array)
			{
				if (pEB && pEB->IsAlive && !pEB->InLimbo && pEB->Owner && pEB->Owner != pHouse && !pEB->Owner->IsNeutral()
					&& !pHouse->IsAlliedWith(pEB->Owner) && IsEnemyBuildingKnown(pHouse, pEB))
				{
					enemyCells.push_back(pEB->GetMapCoords());
				}
			}

			BuildingClass* pSell = nullptr;
			int bestTier = INT_MAX;

			for (auto const pBld : pHouse->Buildings)
			{
				if (!pBld || !pBld->IsAlive || pBld->InLimbo || !pBld->Type || pBld->CurrentMission == Mission::Selling
					|| pBld->Type->ConstructionYard || pBld->Type->Refinery || pBld->Type->ResourceDestination
					|| pBld->Type->Unsellable || pBld->Type->InvisibleInGame || pBld->Type->Naval)
				{
					// Never a naval building: on an island map the yard is the only way off the
					// island for an MCV or an army (user: selling it is a death sentence).
					continue;
				}

				const CellStruct bc = pBld->GetMapCoords();
				const bool hasCopy = typeCounts[pBld->Type->ArrayIndex] >= 2;
				const int netPower = pBld->Type->PowerBonus - pBld->Type->PowerDrain;
				int tier = INT_MAX;

				if (netPower > 0)
				{
					if (hasCopy && pHouse->PowerOutput - pHouse->PowerDrain >= netPower)
						tier = 2;
				}
				else if (hasCopy && pBld->Type->Factory != AbstractType::None)
				{
					tier = 0;
				}
				else if (hasCopy && pBld->Type->IsBaseDefense)
				{
					tier = std::none_of(enemyCells.begin(), enemyCells.end(), [&bc](const CellStruct& e) { return bc.DistanceFrom(e) <= 30.0; })
						? 1 : 3;
				}
				else if (pBld->Type->Factory == AbstractType::None && !pBld->Type->IsBaseDefense)
				{
					tier = 4;
				}

				if (tier < bestTier)
				{
					bestTier = tier;
					pSell = pBld;
				}
			}

			if (pSell)
			{
				macro.LastBrokeSaleFrame = now;
				Debug::Log("[NeuralAI] House %d broke (no income, %d credits): selling %s at (%d,%d) for a harvester.\n",
					pHouse->ArrayIndex, pHouse->Available_Money(), pSell->Type->ID, pSell->GetMapCoords().X, pSell->GetMapCoords().Y);
				pSell->Sell(1);
			}
		}

		// Selector prior of Expert 0: the economy trouble it can act on now. Nothing gathering (with a
		// harvester or a refinery buildable), harvesters with no refinery, a raid that cost a third of
		// the economy, low power that lasted 300 frames (worse the bigger the deficit), harvesters
		// under fire, a refinery no harvester serves while another is crowded, storage nearly full.
		// 0 = nothing to do: Expert 0 is vetoed.
		{
			int trouble = 0;
			// Only harvesters that unload at a refinery need one (a slave miner is its own refinery).
			const bool dockingHarvesterOwned = std::any_of(harvesters.begin(), harvesters.end(),
				[](UnitClass* pHarv) { return pHarv->Type->Harvester && !pHarv->Type->DeploysInto; });

			if (noIncome && cheapestHarvester != INT_MAX)
				trouble += 900;
			else if (refCount == 0 && dockingHarvesterOwned)
				trouble += 600;

			if (macro.EconomyRecoveryActive)
				trouble += 450;

			if (macro.LowPowerSinceFrame >= 0 && now - macro.LowPowerSinceFrame >= 300)
				trouble += 250 + std::clamp((pHouse->PowerDrain - pHouse->PowerOutput) * 2, 0, 250);

			trouble += 150 * std::min(2, underFire);

			if (macro.StarvedRefineries > 0)
				trouble += 200;

			if (macro.StorageNearlyFull)
				trouble += 350;

			macro.EconomyTrouble = std::min(950, trouble);
		}

		if (!expertInCharge)
			return;

		// Harvester balance.
		if (refCount >= 2 && now - macro.LastHarvesterBalanceFrame >= 450)
		{
			if (pCrowded && pIdle && pCrowded != pIdle)
			{
				CellClass* pOre = FindFreeOreFieldCell(pHouse, pIdle->GetMapCoords(),
					MapClass::Instance.GetMovementZoneType(pIdle->GetMapCoords(), MovementZone::Normal, false), 0, true);

				if (pOre && pOre->MapCoords.DistanceFrom(pIdle->GetMapCoords()) <= 16.0)
				{
					for (auto const pHarv : harvesters)
					{
						// Chrono miners loop-teleport on a destination, slave miners deploy where they stop.
						if (pHarv->DistanceFrom(pCrowded) <= 6 * 256 && !pHarv->InLimbo && pHarv->CurrentMission != Mission::Enter
							&& pHarv->CurrentMission != Mission::Unload && pHarv->Type->Harvester && !pHarv->Type->Teleporter
							&& !pHarv->Type->DeploysInto)
						{
							macro.LastHarvesterBalanceFrame = now;
							pHarv->SetDestination(pOre, true);
							pHarv->QueueMission(Mission::Harvest, false);
							CountDecision(NeuralDecision::HarvesterRebalance, pHouse->ArrayIndex);
							break;
						}
					}
				}
			}
		}
	}

	// FindSpatialTetrisBuildCell costs 20-55 ms in a late game. Questions that only need a rough
	// answer (is there room for it, where would it go: deploy stands, a fleeing MCV, the shipyard
	// check) share one answer per house and building type for 240 frames; real placement always
	// searches. Lookups only, keyed by house and type index, tied to NeuralMatchSerial.
	CellStruct CachedTetrisBuildCell(HouseClass* pHouse, BuildingTypeClass* pType)
	{
		if (!pHouse || !pType)
			return CellStruct::Empty;

		struct Entry
		{
			int MatchSerial;
			int Frame;
			CellStruct Cell;
		};

		static std::unordered_map<int, Entry> cache;
		const int key = pHouse->ArrayIndex * 8192 + pType->ArrayIndex;
		const int now = Unsorted::CurrentFrame;

		if (auto const it = cache.find(key); it != cache.end() && it->second.MatchSerial == NeuralMatchSerial
			&& now >= it->second.Frame && now - it->second.Frame < 240)
		{
			return it->second.Cell;
		}

		const CellStruct cell = NeuralAI::FindSpatialTetrisBuildCell(pHouse, pType);
		cache[key] = Entry { NeuralMatchSerial, now, cell };
		return cell;
	}

	// Expert 3 (FFA opportunism), every 450 frames, only with two or more living enemies and Expert 3
	// in charge: a priority bonus per enemy house for the main enemy choice (Micro), from what this
	// house has seen only (fog-fair):
	// - up to +600 (40 per unit) to an enemy caught fighting someone else (its visible units next to a
	//   third house's known structures, or a third house's visible units next to its known structures):
	//   hit it while busy.
	// - +500 to an enemy whose visible units are inside our base (12 cells of our buildings): never
	//   two fronts, answer the one at the door first.
	// - up to +400 to the weakest by known structures, +150 to the nearest known base.
	// - the visible threat of an enemy not at our door counts 40%: let the others wear the big one down.
	void UpdateFfaTargets(HouseClass* pHouse)
	{
		if (!pHouse)
			return;

		auto& macro = GetMacroState(pHouse);
		const int now = Unsorted::CurrentFrame;

		if (now - macro.LastFfaTargetFrame < 450 && now >= macro.LastFfaTargetFrame)
			return;

		macro.LastFfaTargetFrame = now;
		macro.FfaEnemyBonus.fill(0);
		macro.FfaThreatScale.fill(100);

		auto const pExt = HouseExt::Fetch(pHouse);

		if (!pExt || pExt->NeuralAI_ActiveExpert != 3)
			return;

		std::vector<HouseClass*> enemies;

		for (auto const pOther : HouseClass::Array)
		{
			if (pOther && pOther != pHouse && !pHouse->IsAlliedWith(pOther) && !pOther->IsNeutral() && !pOther->IsObserver()
				&& !pOther->Defeated && pOther->ArrayIndex >= 0 && pOther->ArrayIndex < 8)
			{
				enemies.push_back(pOther);
			}
		}

		if (enemies.size() < 2)
			return;

		// Known structures per house (enemies and third parties alike) and our own building cells.
		std::map<int, std::vector<CellStruct>> knownStructures;
		std::vector<CellStruct> ownCells;

		for (auto const pBld : BuildingClass::Array)
		{
			if (!pBld || !pBld->IsAlive || pBld->InLimbo || !pBld->Owner || !pBld->Type || pBld->Type->InvisibleInGame)
				continue;

			if (pBld->Owner == pHouse)
				ownCells.push_back(pBld->GetMapCoords());
			else if (!pBld->Owner->IsNeutral() && IsEnemyBuildingKnown(pHouse, pBld))
				knownStructures[pBld->Owner->ArrayIndex].push_back(pBld->GetMapCoords());
		}

		auto isNear = [](const std::vector<CellStruct>& cells, const CellStruct& c, int range)
		{
			return std::any_of(cells.begin(), cells.end(),
				[&](const CellStruct& o) { return std::max(std::abs(o.X - c.X), std::abs(o.Y - c.Y)) <= range; });
		};

		std::array<int, 8> busy {};
		std::array<bool, 8> atOurDoor {};

		for (auto const pTechno : TechnoClass::Array)
		{
			if (!pTechno || !pTechno->IsAlive || pTechno->InLimbo || !pTechno->Owner || pTechno->Owner == pHouse
				|| pTechno->Owner->IsNeutral() || pTechno->WhatAmI() == AbstractType::Building
				|| pTechno->Owner->ArrayIndex < 0 || pTechno->Owner->ArrayIndex >= 8
				|| !IsCellVisibleToAI(pHouse, pTechno->GetCell()))
			{
				continue;
			}

			const int owner = pTechno->Owner->ArrayIndex;
			const CellStruct c = pTechno->GetMapCoords();

			if (!pHouse->IsAlliedWith(pTechno->Owner) && isNear(ownCells, c, 12))
				atOurDoor[owner] = true;

			for (auto const& [third, cells] : knownStructures)
			{
				if (third == owner || third < 0 || third >= 8)
					continue;

				if (isNear(cells, c, 12))
				{
					busy[owner]++; // its units at a third house's base
					busy[third]++; // that third house is under attack
				}
			}
		}

		int maxKnown = 1;

		for (auto const pEnemy : enemies)
			maxKnown = std::max(maxKnown, static_cast<int>(knownStructures[pEnemy->ArrayIndex].size()));

		const CellStruct home = pHouse->ConYards.Count > 0 ? pHouse->ConYards.GetItem(0)->GetMapCoords() : pHouse->BaseSpawnCell;
		int nearestIdx = -1;
		int nearestDist = INT_MAX;

		for (auto const pEnemy : enemies)
		{
			for (auto const& c : knownStructures[pEnemy->ArrayIndex])
			{
				const int d = std::max(std::abs(c.X - home.X), std::abs(c.Y - home.Y));

				if (d < nearestDist)
				{
					nearestDist = d;
					nearestIdx = pEnemy->ArrayIndex;
				}
			}
		}

		for (auto const pEnemy : enemies)
		{
			const int idx = pEnemy->ArrayIndex;
			const int known = static_cast<int>(knownStructures[idx].size());
			int bonus = 0;

			// Graded: a few units trading at a border do not make a house busy, a siege does.
			bonus += std::min(600, 40 * busy[idx]);

			if (atOurDoor[idx])
				bonus += 500;

			if (known > 0)
				bonus += 400 * (maxKnown - known) / maxKnown;

			if (idx == nearestIdx)
				bonus += 150;

			macro.FfaEnemyBonus[idx] = bonus;
			macro.FfaThreatScale[idx] = atOurDoor[idx] ? 100 : 40;

			// Tacit coalition: its target is the one to wear down now, at full threat; a truce
			// partner busy with it is no opportunity.
			if (idx == macro.Coalition.Target)
			{
				macro.FfaEnemyBonus[idx] += 300;
				macro.FfaThreatScale[idx] = 100;
			}
			else if (IsTacitTrucePartner(pHouse, idx))
			{
				macro.FfaEnemyBonus[idx] = 0;
				macro.FfaThreatScale[idx] = 20;
			}
		}
	}

	// Per-tick match tactics feeding the wave logic:
	// 1. our armed ground units that died within 22 cells of the current wave target, by the
	//    side they came from (points 31/42: the next waves approach from another side);
	// 2. a counterattack window when the enemy pressure near home peaked (>= 200) and collapsed
	//    to a quarter (point 45);
	// 3. every 900 frames: continuous pressure when our economy leads (point 53) and the enemy
	//    house to focus, the clearly weakest one (points 39/51). All fog-fair.
	void UpdateMatchTactics(HouseClass* pHouse, const std::array<int16_t, NeuralAI::OutputVectorSize>& scores)
	{
		if (!pHouse)
			return;

		auto& macro = GetMacroState(pHouse);
		const int now = Unsorted::CurrentFrame;
		const CellStruct home = GetHouseHomeCell(pHouse);

		{
			std::unordered_set<DWORD> alive;
			std::vector<NeuralGroundLossTrack> next;

			for (auto const pTech : TechnoClass::Array)
			{
				if (!pTech || pTech->Owner != pHouse || !pTech->IsAlive)
					continue;

				const auto what = pTech->WhatAmI();

				if (what != AbstractType::Unit && what != AbstractType::Infantry)
					continue;

				alive.insert(pTech->UniqueID);
				auto const pType = pTech->GetTechnoType();
				auto const pWS = pType ? pType->GetWeapon(0) : nullptr;

				if (pTech->InLimbo || !pType || pType->Naval || !pWS || GetWeaponThreatRate(pWS->WeaponType) <= 0)
					continue;

				next.push_back({ pTech->UniqueID, pTech->GetMapCoords(), GetCombatValueOfType(pType, false) });
			}

			const bool waveRecent = macro.LastWaveTargetCell != CellStruct::Empty && now - macro.LastWaveDispatchFrame <= 3600;

			for (auto const& track : macro.GroundLossTracks)
			{
				if (!waveRecent || alive.contains(track.ID) || track.Coords.DistanceFrom(macro.LastWaveTargetCell) > 22.0)
					continue;

				const int sector = AttackSectorOf(macro.LastWaveTargetCell, track.Coords);

				if (sector >= 0)
					macro.FailedApproachWeight[sector] += track.Value;
			}

			macro.GroundLossTracks = std::move(next);

			if (now - macro.FailedApproachDecayFrame >= 2700 || macro.FailedApproachDecayFrame > now)
			{
				macro.FailedApproachDecayFrame = now;

				for (auto& weight : macro.FailedApproachWeight)
					weight = weight * 8 / 10;
			}
		}

		if (home != CellStruct::Empty)
		{
			int threat = 0;

			for (auto const& [uid, sighting] : macro.EnemySightings)
			{
				if (now - sighting.LastSeenFrame <= 150 && sighting.LastCoords.DistanceFrom(home) <= 22.0)
					threat += sighting.Value;
			}

			if (threat > macro.HomeThreatPeak || now - macro.HomeThreatPeakFrame > 2700 || macro.HomeThreatPeakFrame > now)
			{
				macro.HomeThreatPeak = threat;
				macro.HomeThreatPeakFrame = now;
			}
			else if (macro.HomeThreatPeak >= 200 && threat * 4 <= macro.HomeThreatPeak)
			{
				// Their attack broke on our base: hit back while they are depleted.
				if (SlotGate(scores[37], -200, 37))
				{
					macro.CounterattackUntilFrame = now + 1200;
					CountDecision(NeuralDecision::Counterattack, pHouse->ArrayIndex);
				}

				macro.HomeThreatPeak = threat;
				macro.HomeThreatPeakFrame = now;
			}
		}

		if (now - macro.TacticsRefreshFrame < 900 && macro.TacticsRefreshFrame <= now)
			return;

		macro.TacticsRefreshFrame = now;

		int ownRefineries = 0;

		for (auto const pBld : pHouse->Buildings)
		{
			if (pBld && pBld->IsAlive && !pBld->InLimbo && pBld->Type && (pBld->Type->Refinery || pBld->Type->ResourceDestination))
				ownRefineries++;
		}

		std::array<int, 32> enemyStrength {};
		std::array<int, 32> enemyKnownBuildings {};
		std::array<int, 32> enemyKnownRefineries {};

		for (auto const pBld : BuildingClass::Array)
		{
			if (!pBld || pBld->InLimbo || !pBld->IsAlive || !pBld->Type || !pBld->Owner || pBld->Owner == pHouse
				|| pHouse->IsAlliedWith(pBld->Owner) || pBld->Owner->IsNeutral() || pBld->Owner->Defeated
				|| pBld->Owner->IsObserver() || pBld->Type->Insignificant || !IsEnemyBuildingKnown(pHouse, pBld))
			{
				continue;
			}

			const int idx = std::clamp(pBld->Owner->ArrayIndex, 0, 31);
			enemyKnownBuildings[idx]++;
			enemyStrength[idx] += 10 + (pBld->Type->IsBaseDefense ? 25 : 0);

			if (pBld->Type->Refinery || pBld->Type->ResourceDestination)
				enemyKnownRefineries[idx]++;
		}

		for (auto const& [uid, sighting] : macro.EnemySightings)
		{
			if (sighting.OwnerIndex >= 0 && sighting.OwnerIndex < 32 && now - sighting.LastSeenFrame <= 2700)
				enemyStrength[sighting.OwnerIndex] += sighting.Value;
		}

		int weakest = -1;
		int weakestStrength = INT_MAX;
		int strongestStrength = 0;
		int enemies = 0;
		int richestEnemyRefineries = 0;

		for (int i = 0; i < 32; ++i)
		{
			if (enemyKnownBuildings[i] <= 0)
				continue;

			enemies++;
			richestEnemyRefineries = std::max(richestEnemyRefineries, enemyKnownRefineries[i]);
			strongestStrength = std::max(strongestStrength, enemyStrength[i]);

			if (enemyStrength[i] < weakestStrength)
			{
				weakestStrength = enemyStrength[i];
				weakest = i;
			}
		}

		macro.PressureMode = pHouse->Available_Money() >= 2500 && pHouse->NumWarFactories >= 1 && ownRefineries >= 2
			&& ownRefineries >= richestEnemyRefineries + 1 && SlotGate(scores[37], 0, 37);

		macro.FocusEnemyIndex = (enemies >= 2 && weakestStrength * 10 <= strongestStrength * 6 && SlotGate(scores[43], -150, 43))
			? weakest
			: -1;
	}

} // namespace

namespace
{
	// Path search backoff. A failed search of the engine's path finder costs about 6 ms (five tries
	// of 1.2 ms) and a unit that cannot get through asks again every few frames: a match had a
	// million failures, half of its wall time. A unit of a Neural AI house whose searches to one
	// place fail PathBackoffFails times in a row (a gap of PathBackoffForgetFrames without a failure
	// starts the count again) skips its searches to that place for PathBackoffFirstFrames, doubled
	// with each further failure up to PathBackoffMaxFrames. A new goal PathBackoffGoalCells or more
	// away is searched at once.
	constexpr int PathBackoffFails = 3;
	constexpr int PathBackoffFirstFrames = 15;
	constexpr int PathBackoffMaxFrames = 240;
	constexpr int PathBackoffForgetFrames = 60;
	constexpr double PathBackoffGoalCells = 3.0;

	struct NeuralPathBackoff
	{
		int Fails { 0 };
		int LastFailFrame { -100000 };
		int SkipUntil { -100000 };
		CellStruct Goal { CellStruct::Empty };
	};

	std::unordered_map<DWORD, NeuralPathBackoff> PathBackoffs;
	int PathBackoffSerial = -1;
	int PathSearchesSkipped = 0;

	void ResetPathBackoffsForMatch()
	{
		if (PathBackoffSerial == NeuralMatchSerial)
			return;

		PathBackoffSerial = NeuralMatchSerial;
		PathBackoffs.clear();
		PathSearchesSkipped = 0;
	}
}

bool NeuralAI::ShouldSkipPathSearch(const FootClass* pFoot, const CellStruct* pGoal)
{
	if (!pFoot || !pGoal || PathBackoffSerial != NeuralMatchSerial)
		return false;

	auto const it = PathBackoffs.find(pFoot->UniqueID);

	if (it == PathBackoffs.end() || Unsorted::CurrentFrame >= it->second.SkipUntil || it->second.Goal.DistanceFrom(*pGoal) >= PathBackoffGoalCells)
		return false;

	++PathSearchesSkipped;

	return true;
}

// TEMP diagnostic: who the engine's path finder fails for. One failure costs about 1.2 ms of wall
// time (frame cost sample of 2026-11-13) and a match can have a million. Counted per match by unit
// type, role (slave, gatherer, plan member, free), mission, mega mission and how far the goal is
// (near up to 12 cells, mid up to 40, far), and logged every PathFailureLogFrames.
void NeuralAI::OnPathFailure(FootClass* pFoot, const CellStruct& from, const CellStruct& to)
{
	constexpr int PathFailureLogFrames = 18000;
	static int serial = -1;
	static int total = 0;
	static int logBucket = 0;
	static std::map<std::string, int> kinds;

	if (serial != NeuralMatchSerial)
	{
		serial = NeuralMatchSerial;
		total = 0;
		logBucket = 0;
		kinds.clear();
	}

	if (!pFoot)
		return;

	// The engine reports up to five failures per search: one count per unit and frame.
	ResetPathBackoffsForMatch();

	if (pFoot->Owner && NeuralAI::IsEnabledForHouse(pFoot->Owner))
	{
		const int now = Unsorted::CurrentFrame;

		if (PathBackoffs.size() > 4096)
		{
			std::erase_if(PathBackoffs, [now](const auto& entry)
				{ return now - std::max(entry.second.LastFailFrame, entry.second.SkipUntil) > PathBackoffForgetFrames * 10; });
		}

		auto& backoff = PathBackoffs[pFoot->UniqueID];

		if (backoff.LastFailFrame != now)
		{
			if (now - std::max(backoff.LastFailFrame, backoff.SkipUntil) > PathBackoffForgetFrames
				|| backoff.Goal.DistanceFrom(to) >= PathBackoffGoalCells)
			{
				backoff.Fails = 0;
			}

			backoff.Fails++;
			backoff.LastFailFrame = now;
			backoff.Goal = to;

			if (backoff.Fails >= PathBackoffFails)
			{
				backoff.SkipUntil = now + std::min(PathBackoffMaxFrames,
					PathBackoffFirstFrames << std::min(backoff.Fails - PathBackoffFails, 5));
			}
		}
	}

	auto const pType = pFoot->GetTechnoType();
	auto const pUnit = abstract_cast<UnitClass*>(pFoot);
	const int owner = pFoot->Owner ? pFoot->Owner->ArrayIndex : -1;
	const int distance = static_cast<int>(from.DistanceFrom(to));
	const char* role = "free";

	if (pFoot->SlaveOwner)
		role = "slave";
	else if ((pType && pType->ResourceGatherer) || (pUnit && pUnit->Type && pUnit->Type->Harvester))
		role = "gatherer";
	else if (owner >= 0 && owner < 32 && MacroStates[owner].PlanMembers.contains(pFoot->UniqueID))
		role = "plan";

	char key[112];
	std::snprintf(key, sizeof(key), "%s %s m%d mega%d %s", pType ? pType->ID : "?", role, static_cast<int>(pFoot->CurrentMission),
		static_cast<int>(pFoot->MegaMission), distance <= 12 ? "near" : (distance <= 40 ? "mid" : "far"));
	kinds[key]++;
	total++;

	const int bucket = Unsorted::CurrentFrame / PathFailureLogFrames;

	if (bucket <= logBucket)
		return;

	logBucket = bucket;
	std::vector<std::pair<int, std::string>> top;

	for (auto const& [name, count] : kinds)
		top.emplace_back(count, name);

	std::sort(top.begin(), top.end(), [](auto const& a, auto const& b) { return a.first != b.first ? a.first > b.first : a.second < b.second; });
	std::string text;

	for (size_t i = 0; i < top.size() && i < 16; ++i)
		text += (i ? "; " : "") + top[i].second + " x" + std::to_string(top[i].first);

	Debug::Log("[NeuralAI] Path failures DIAG frame %d: %d so far in %d kinds, %d searches skipped; %s.\n", Unsorted::CurrentFrame,
		total, static_cast<int>(kinds.size()), PathSearchesSkipped, text.c_str());
}

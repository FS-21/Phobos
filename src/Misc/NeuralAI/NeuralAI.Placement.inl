// NeuralAI.Placement.inl: Building placement, base crawl and placement completion.
// Part of NeuralAI.cpp (included in order; one translation unit).

namespace
{
	// Compact per-frame snapshot of every live building footprint. Deploy-site and placement
	// searches test thousands of candidate cells per call; reading the snapshot avoids the
	// virtual GetMapCoords/foundation calls on every building for every candidate (the
	// mid/late-game stutters). Rebuilt on the first query of each frame, from synced game
	// state only, so every client sees the same data.
	struct BuildingFootprint
	{
		HouseClass* Owner;
		BuildingTypeClass* Type;
		short X;
		short Y;
		short W;
		short H;
		bool IsDefense;
	};

	std::vector<BuildingFootprint> FootprintCache;
	int FootprintCacheFrame = -1;
	int FootprintCacheCount = -1;

	const std::vector<BuildingFootprint>& GetBuildingFootprints()
	{
		if (FootprintCacheFrame != Unsorted::CurrentFrame || FootprintCacheCount != BuildingClass::Array.Count)
		{
			FootprintCache.clear();

			for (auto const pBld : BuildingClass::Array)
			{
				if (!pBld || pBld->InLimbo || !pBld->IsAlive || !pBld->Type || pBld->Type->InvisibleInGame)
					continue;

				const CellStruct bc = pBld->GetMapCoords();
				FootprintCache.push_back({ pBld->Owner, pBld->Type, bc.X, bc.Y,
					static_cast<short>(pBld->Type->GetFoundationWidth()),
					static_cast<short>(pBld->Type->GetFoundationHeight(false)), pBld->Type->IsBaseDefense });
			}

			FootprintCacheFrame = Unsorted::CurrentFrame;
			FootprintCacheCount = BuildingClass::Array.Count;
		}

		return FootprintCache;
	}

	// ---- Expert 1 (Spatial Base Architect) helpers ----

	// Inclusive cell rectangle.
	struct ArchitectRect
	{
		short X0;
		short Y0;
		short X1;
		short Y1;

		bool Overlaps(int x, int y, int w, int h) const
		{
			return x <= this->X1 && x + w - 1 >= this->X0 && y <= this->Y1 && y + h - 1 >= this->Y0;
		}
	};

	// Exit lane of a ground factory: the cells its units walk out through, 3 wide and 3 long from
	// the foundation edge. Weapons factories push their units east out of the door (see
	// BuildingExt::KickOutStuckUnits); other factories use the side nearest to their ExitCoord
	// (leptons from the top-left corner). Aircraft pads, naval yards and ConYards need no lane.
	bool GetFactoryExitLane(const BuildingTypeClass* pType, const CellStruct& topLeft, ArchitectRect& lane)
	{
		if (!pType || pType->Naval || pType->ConstructionYard)
			return false;

		if (!pType->WeaponsFactory && pType->Factory != AbstractType::UnitType && pType->Factory != AbstractType::InfantryType)
			return false;

		const int x = topLeft.X;
		const int y = topLeft.Y;
		const int w = pType->GetFoundationWidth();
		const int h = pType->GetFoundationHeight(false);
		const bool hasExit = pType->ExitCoord.X != 0 || pType->ExitCoord.Y != 0;
		const int exitX = hasExit ? x + pType->ExitCoord.X / Unsorted::LeptonsPerCell : x + w / 2;
		const int exitY = hasExit ? y + pType->ExitCoord.Y / Unsorted::LeptonsPerCell : y + h / 2;
		const int rowY = std::clamp(exitY, y, y + h - 1);
		const int colX = std::clamp(exitX, x, x + w - 1);

		auto make = [](int x0, int y0, int x1, int y1)
		{
			return ArchitectRect { static_cast<short>(x0), static_cast<short>(y0), static_cast<short>(x1), static_cast<short>(y1) };
		};

		if (pType->WeaponsFactory || exitX >= x + w)
			lane = make(x + w, rowY - 1, x + w + 2, rowY + 1);
		else if (exitX < x)
			lane = make(x - 3, rowY - 1, x - 1, rowY + 1);
		else if (exitY < y)
			lane = make(colX - 1, y - 3, colX + 1, y - 1);
		else
			lane = make(colX - 1, y + h, colX + 1, y + h + 2);

		return true;
	}

	// The launch ring of the ring-limited superweapons (SW.RangeMaximum > 0, e.g. an EMP cannon) a
	// building type fires: listed in their EMPulse.Cannons, else EMPulseCannon=yes for an EMPulse type,
	// else granting the superweapon itself. The largest maximum (cells) and its minimum; 0 when none.
	double GetLaunchRingOf(const BuildingTypeClass* pType, double& outMin)
	{
		outMin = 0.0;
		double best = 0.0;

		if (!pType)
			return best;

		auto const pBldExt = BuildingTypeExt::ExtMap.Find(const_cast<BuildingTypeClass*>(pType));

		for (auto const pSWType : SuperWeaponTypeClass::Array)
		{
			if (!pSWType)
				continue;

			auto const& aim = GetSWTargeting(pSWType);

			if (aim.RangeMax <= 0.0 || aim.RangeMax <= best)
				continue;

			bool fires = false;

			if (!aim.Cannons.empty())
			{
				fires = std::any_of(aim.Cannons.begin(), aim.Cannons.end(),
					[pType](const std::string& id) { return _stricmp(id.c_str(), pType->ID) == 0; });
			}
			else if (aim.IsEMPulse)
			{
				fires = pType->EMPulseCannon;
			}
			else if (pBldExt)
			{
				for (int i = 0; i < pBldExt->GetSuperWeaponCount() && !fires; ++i)
					fires = pBldExt->GetSuperWeaponIndex(i) == pSWType->ArrayIndex;
			}

			if (fires)
			{
				best = aim.RangeMax;
				outMin = std::max(0.0, aim.RangeMin);
			}
		}

		return best;
	}

	// Radius in cells of the area a superweapon type destroys, read from its own data (Phobos
	// Detonate weapon / warhead, its WeaponType, the rules' nuke, lightning storm and dominator
	// settings). 0 for superweapons that hit no area (Iron Curtain, Chronosphere, paradrops, spy
	// plane, force shield, reveal), so they never spread a base for nothing.
	int GetSuperWeaponStrikeRadius(SuperWeaponTypeClass* pSW)
	{
		if (!pSW)
			return 0;

		double spread = 0.0;

		auto addWarhead = [&spread](const WarheadTypeClass* pWH)
		{
			if (pWH)
				spread = std::max(spread, static_cast<double>(pWH->CellSpread));
		};

		auto addWeapon = [&addWarhead](const WeaponTypeClass* pW)
		{
			if (pW && pW->Damage > 0)
				addWarhead(pW->Warhead);
		};

		if (auto const pSWExt = SWTypeExt::TryFetch(pSW))
		{
			addWeapon(pSWExt->Detonate_Weapon.Get());
			addWarhead(pSWExt->Detonate_Warhead.Get());
		}

		addWeapon(pSW->WeaponType);

		if (auto const pRules = RulesClass::Instance)
		{
			switch (pSW->Type)
			{
			case SuperWeaponType::Nuke:
				addWarhead(pRules->NukeWarhead);
				break;
			case SuperWeaponType::LightningStorm:
				spread = std::max(spread, static_cast<double>(pRules->LightningCellSpread));
				break;
			case SuperWeaponType::PsychicDominator:
				spread = std::max(spread, static_cast<double>(pRules->DominatorCaptureRange));
				break;
			default:
				break;
			}
		}

		return static_cast<int>(std::ceil(spread));
	}

	// Key buildings a house had at its last placement search and the ones it lost since, so
	// Expert 1 rebuilds a destroyed factory, refinery or power plant where it stood. Plain
	// bookkeeping for every house (synced state only); tied to NeuralMatchSerial.
	struct ArchitectSite
	{
		DWORD ID;
		int TypeIndex;
		CellStruct Cell;
		bool Selling;
	};

	struct ArchitectLostSite
	{
		int TypeIndex;
		CellStruct Cell;
		int LostFrame;
	};

	struct ArchitectHouseMemory
	{
		int MatchSerial { -1 };
		std::vector<ArchitectSite> Alive {};
		std::vector<ArchitectLostSite> Lost {};
	};

	std::vector<ArchitectHouseMemory> ArchitectMemory;

	// A lost site is worth remembering for 10 game minutes.
	constexpr int ArchitectLostSiteFrames = 9000;

	bool IsArchitectKeyType(const BuildingTypeClass* pType)
	{
		if (!pType || pType->ConstructionYard || pType->IsBaseDefense || pType->Insignificant || pType->InvisibleInGame
			|| pType->UndeploysInto || pType->PowersUpBuilding[0])
		{
			return false;
		}

		return pType->Factory != AbstractType::None || pType->WeaponsFactory || pType->Refinery || pType->ResourceDestination
			|| pType->PowerBonus > pType->PowerDrain || pType->Radar || pType->HasSuperWeapon() || pType->UnitRepair
			|| pType->Cost >= 1000;
	}

	ArchitectHouseMemory& UpdateArchitectMemory(HouseClass* pHouse)
	{
		const size_t idx = static_cast<size_t>(std::max(0, pHouse->ArrayIndex));

		if (ArchitectMemory.size() <= idx)
			ArchitectMemory.resize(idx + 1);

		auto& mem = ArchitectMemory[idx];

		if (mem.MatchSerial != NeuralMatchSerial)
		{
			mem = ArchitectHouseMemory {};
			mem.MatchSerial = NeuralMatchSerial;
		}

		std::vector<ArchitectSite> now;
		now.reserve(mem.Alive.size() + 8);

		for (auto const pBld : pHouse->Buildings)
		{
			if (pBld && pBld->IsAlive && !pBld->InLimbo && IsArchitectKeyType(pBld->Type))
			{
				now.push_back({ pBld->UniqueID, pBld->Type->ArrayIndex, pBld->GetMapCoords(),
					pBld->CurrentMission == Mission::Selling || pBld->QueuedMission == Mission::Selling });
			}
		}

		std::sort(now.begin(), now.end(), [](const ArchitectSite& a, const ArchitectSite& b) { return a.ID < b.ID; });

		// Gone since the last search and not being sold then: destroyed or captured.
		for (auto const& old : mem.Alive)
		{
			const bool stillThere = std::binary_search(now.begin(), now.end(), old,
				[](const ArchitectSite& a, const ArchitectSite& b) { return a.ID < b.ID; });

			if (!stillThere && !old.Selling)
			{
				if (mem.Lost.size() >= 12)
					mem.Lost.erase(mem.Lost.begin());

				mem.Lost.push_back({ old.TypeIndex, old.Cell, Unsorted::CurrentFrame });
			}
		}

		// Forget sites that are too old or rebuilt (a key building of ours stands there again).
		mem.Lost.erase(std::remove_if(mem.Lost.begin(), mem.Lost.end(), [&now](const ArchitectLostSite& site)
			{
				return Unsorted::CurrentFrame - site.LostFrame > ArchitectLostSiteFrames
					|| std::any_of(now.begin(), now.end(), [&site](const ArchitectSite& s) { return s.Cell == site.Cell; });
			}), mem.Lost.end());

		mem.Alive = std::move(now);

		return mem;
	}

	// A land passage needs LandPassageLaneCells clear cells beside a building on one of every two
	// opposite sides.
	constexpr int LandPassageLaneCells = 2;

	// A land building that closes a passage: one whose footprint stands on a passage mouth (bridge
	// end, tunnel tube), or one bigger than a cell with closed ground (water, cliff, map edge; other
	// buildings do not count, a base keeps its one-cell streets) within LandPassageLaneCells on both
	// opposite sides of its footprint, so what passes there is left a lane of one cell or none.
	// Placement skips such cells while it has any other and takes them last when it has not.
	bool ClosesLandPassage(BuildingTypeClass* pType, const CellStruct& topLeft)
	{
		if (!pType || pType->Naval)
			return false;

		const int w = pType->GetFoundationWidth();
		const int h = pType->GetFoundationHeight(false);

		if (IsFootprintOnPassageMouth(topLeft, w, h))
			return true;

		if (w * h <= 1)
			return false;

		// A side is closed when less than three quarters of the strip beside it is open ground.
		auto const sideClosed = [](int x0, int y0, int x1, int y1)
		{
			int open = 0;
			int total = 0;

			for (int y = y0; y <= y1; ++y)
			{
				for (int x = x0; x <= x1; ++x)
				{
					const CellStruct c { static_cast<short>(x), static_cast<short>(y) };
					++total;

					if (MapClass::Instance.CoordinatesLegal(c) && IsOpenGroundCell(MapClass::Instance.TryGetCellAt(c)))
						++open;
				}
			}

			return open * 4 < total * 3;
		};

		const int x0 = topLeft.X;
		const int y0 = topLeft.Y;
		const int x1 = topLeft.X + w - 1;
		const int y1 = topLeft.Y + h - 1;
		const int lane = LandPassageLaneCells;

		return (sideClosed(x0 - lane, y0, x0 - 1, y1) && sideClosed(x1 + 1, y0, x1 + lane, y1))
			|| (sideClosed(x0, y0 - lane, x1, y0 - 1) && sideClosed(x0, y1 + 1, x1, y1 + lane));
	}
}

bool NeuralAI::IsCellClearedOfExitCorridors(HouseClass* pHouse, BuildingTypeClass* pType, CellStruct candidateCell)
{
	const int candW = pType->GetFoundationWidth();
	const int candH = pType->GetFoundationHeight(false);
	const bool isCandDefense = pType->IsBaseDefense;

	if (ClosesLandPassage(pType, candidateCell))
		return false;

	for (auto const& fp : GetBuildingFootprints())
	{
		// Quick reject: nothing farther than the footprints plus the 4-cell turret spacing matters.
		if (fp.X > candidateCell.X + candW + 4 || fp.X + fp.W + 4 < candidateCell.X || fp.Y > candidateCell.Y + candH + 4
			|| fp.Y + fp.H + 4 < candidateCell.Y)
		{
			continue;
		}

		auto const pExType = fp.Type;
		const CellStruct exCell { fp.X, fp.Y };
		const int exW = fp.W;
		const int exH = fp.H;
		auto const pExOwner = fp.Owner;

		// Strictly prevent overlapping any existing building footprint on the map
		if ((candidateCell.X < exCell.X + exW) && (candidateCell.X + candW > exCell.X)
			&& (candidateCell.Y < exCell.Y + exH) && (candidateCell.Y + candH > exCell.Y))
		{
			return false;
		}

		if (pExOwner != pHouse && (!pExOwner || !pHouse->IsAlliedWith(pExOwner)))
			continue;

		// Enforce minimum 4-cell spacing between Base Defense turrets so the AI
		// spreads defenses along the perimeter instead of clumping
		if (isCandDefense && pExType->IsBaseDefense)
		{
			const int turretDist = std::abs(candidateCell.X - exCell.X) + std::abs(candidateCell.Y - exCell.Y);

			if (turretDist < 4)
				return false;
		}

		// Enforce a 1-cell street margin between non-defense structures when
		// Adjacent >= 2 (from AdvAI). Note: A 1-cell gap equals Chebyshev
		// foundation distance 2, which fits inside standard Adjacent=2 while
		// keeping a passable 1-cell road around all 4 sides of every Factory,
		// Refinery, and ConYard!
		const int basePad = (!isCandDefense && !pExType->IsBaseDefense && pType->Adjacent >= 2) ? 1 : 0;
		if (basePad > 0)
		{
			const int exMinX = exCell.X - basePad;
			const int exMaxX = exCell.X + exW + basePad;
			const int exMinY = exCell.Y - basePad;
			const int exMaxY = exCell.Y + exH + basePad;
			const bool overlapX = (candidateCell.X < exMaxX) && (candidateCell.X + candW > exMinX);
			const bool overlapY = (candidateCell.Y < exMaxY) && (candidateCell.Y + candH > exMinY);
			if (overlapX && overlapY)
				return false;
		}
	}

	return true;
}

CellStruct NeuralAI::FindSpatialTetrisBuildCell(HouseClass* pHouse, BuildingTypeClass* pBuildingType)
{
	if (!pHouse || !pBuildingType || pBuildingType->PowersUpBuilding[0])
		return CellStruct::Empty;

	const NeuralSectionTimer placementTimer { pHouse->ArrayIndex, NeuralSection::Placement };

	// Lost key buildings are tracked for every house (bookkeeping only); Expert 1 uses them.
	auto const& architectMemory = UpdateArchitectMemory(pHouse);

	CellStruct center = pHouse->BaseCenter != CellStruct::Empty ? pHouse->BaseCenter : pHouse->BaseSpawnCell;

	const bool isRefineryOrSlaveMiner
		= pBuildingType->Refinery || pBuildingType->ResourceGatherer || pBuildingType->ResourceDestination;

	if (pHouse->ConYards.Count > 0)
	{
		// If we own an expansion or nomadic ConYard, prioritize planting structures
		// around the newest ConYard (especially when it has < 2 nearby structures
		// so a Nomadic MCV can drop its outpost and hop on!)
		const bool canBuildAtOutpostYard = isRefineryOrSlaveMiner || pBuildingType->IsBaseDefense
			|| pBuildingType->Factory == AbstractType::UnitType || pBuildingType->Factory == AbstractType::InfantryType
			|| pBuildingType->PowerBonus > 0 || pBuildingType->BaseNormal;
		int yardIdx = (pHouse->ConYards.Count > 1 && canBuildAtOutpostYard) ? (pHouse->ConYards.Count - 1) : 0;
		if (pHouse->ConYards.Count > 1)
		{
			for (int yi = pHouse->ConYards.Count - 1; yi >= 1; --yi)
			{
				auto const pCY = pHouse->ConYards.GetItem(yi);
				if (!pCY || pCY->InLimbo || !pCY->IsAlive)
					continue;

				const CellStruct cyc = pCY->GetMapCoords();
				int nearCount = 0;
				for (auto const pB : pHouse->Buildings)
				{
					if (!pB || pB == pCY || pB->InLimbo || !pB->IsAlive || !pB->Type)
						continue;

					if (pB->Type->ConstructionYard)
						continue;

					const CellStruct bc = pB->GetMapCoords();
					if (std::max(std::abs(cyc.X - bc.X), std::abs(cyc.Y - bc.Y)) <= 12)
						nearCount++;
				}

				if (nearCount < 4)
				{
					yardIdx = yi;
					break;
				}
			}
		}

		center = pHouse->ConYards.GetItem(yardIdx)->GetMapCoords();
	}
	else if (pHouse->Buildings.Count > 0)
	{
		// When all ConYards are lost (ConYards.Count == 0), anchor around our
		// safest surviving War Factory, Refinery, or Power Plant that is not under
		// enemy fire!
		for (auto const pOwnBld : pHouse->Buildings)
		{
			if (!pOwnBld || pOwnBld->InLimbo || !pOwnBld->IsAlive || !pOwnBld->Type)
				continue;

			if (pOwnBld->Type->Factory == AbstractType::UnitType || pOwnBld->Type->Refinery || pOwnBld->Type->BaseNormal
				|| pOwnBld->Type->PowerBonus > 0)
			{
				center = pOwnBld->GetMapCoords();

				if (!pOwnBld->HasBeenAttacked)
					break;
			}
		}
	}

	// Support universal Mobile Outpost / Expansion Hub structures, newly
	// colonized archipelago islands, and forward siege outposts: Always
	// prioritize the least-developed expansion island or forward outpost (< 8
	// local structures) so newly colonized islands and forward ConYards
	// immediately receive their Refinery, War Factory, Barracks, and Base
	// Defenses!
	const CellStruct homeSpawnRef = (pHouse->BaseSpawnCell != CellStruct::Empty)
		? pHouse->BaseSpawnCell
		: ((pHouse->ConYards.Count > 0 && pHouse->ConYards.GetItem(0)) ? pHouse->ConYards.GetItem(0)->GetMapCoords()
																	: center);
	const int homeSpawnZone = (homeSpawnRef != CellStruct::Empty)
		? MapClass::Instance.GetMovementZoneType(homeSpawnRef, MovementZone::Normal, false)
		: -1;
	int targetExpansionZone = -1;
	int bestHubPriorityScore = -9999;
	// The chosen hub lacks this very factory or barracks: it is built there, never moved on by
	// the crawl (a remote ConYard kept asking for a barracks the crawl always placed elsewhere).
	bool anchoredAtMissingFactoryHub = false;
	const bool placingWarFactory = (pBuildingType->Factory == AbstractType::UnitType && !pBuildingType->Naval);
	const bool placingBarracks = (pBuildingType->Factory == AbstractType::InfantryType);
	const bool placingShipyard = (pBuildingType->Factory == AbstractType::UnitType && pBuildingType->Naval);

	// Factory dispersal: same-kind factories at the main base (within 16 cells of the spawn).
	// Once it has two, further ones prefer the outpost with the fewest, so a superweapon or a
	// blitz on the main base cannot take all the production at once.
	int homeSameKindFactories = 0;

	if ((placingWarFactory || placingBarracks) && homeSpawnRef != CellStruct::Empty)
	{
		for (auto const pOwnB : pHouse->Buildings)
		{
			if (pOwnB && pOwnB->IsAlive && !pOwnB->InLimbo && pOwnB->Type && pOwnB->Type->Factory == pBuildingType->Factory
				&& pOwnB->Type->Naval == pBuildingType->Naval && pOwnB->GetMapCoords().DistanceFrom(homeSpawnRef) <= 16.0)
			{
				homeSameKindFactories++;
			}
		}
	}

	// Superweapons and the other expensive buildings that produce nothing (tech labs, radars, any
	// structure of 1500+ credits that is no factory, refinery, power plant or defense) stay where the
	// base is strong: an outpost hub draws them only when it has a ConYard and 3+ defenses and no known
	// enemy on its land. Before, every building went to the least developed hub, and a captured oil
	// derrick alone on an islet got the Puppet Master and the Genetic Mutator with no protection.
	// A ring-limited launcher (EMP cannon: SW.RangeMinimum..SW.RangeMaximum around it) goes to the
	// strong base, contested or not, whose ring holds the most known enemy value when that beats the
	// main base's by half again: a forward base the crawl made strong becomes its harassment platform.
	double launchRingMin = 0.0;
	const double launchRing = GetLaunchRingOf(pBuildingType, launchRingMin);
	const bool keepAtStrongBase = launchRing > 0.0
		|| (!pBuildingType->IsBaseDefense && !pBuildingType->ConstructionYard && pBuildingType->Factory == AbstractType::None
			&& !pBuildingType->Refinery && !pBuildingType->ResourceGatherer && pBuildingType->PowerBonus <= pBuildingType->PowerDrain
			&& (pBuildingType->HasSuperWeapon() || pBuildingType->Radar || pBuildingType->GetActualCost(pHouse) >= 1500));

	auto const enemyValueInRing = [&](const CellStruct& site) -> int
	{
		int value = 0;

		for (auto const pEB : BuildingClass::Array)
		{
			if (!pEB || pEB->InLimbo || !pEB->IsAlive || !pEB->Owner || !pEB->Type || pEB->Owner == pHouse
				|| pHouse->IsAlliedWith(pEB->Owner) || pEB->Owner->IsNeutral() || !IsEnemyBuildingKnown(pHouse, pEB))
			{
				continue;
			}

			const double d = site.DistanceFrom(pEB->GetMapCoords());

			if (d >= launchRingMin + 2.0 && d <= launchRing - 2.0)
				value += pEB->Type->Cost;
		}

		return value;
	};

	const int homeRingValue = (launchRing > 0.0 && homeSpawnRef != CellStruct::Empty) ? enemyValueInRing(homeSpawnRef) : 0;

	for (auto const pOwnBld : pHouse->Buildings)
	{
		if (!pOwnBld || pOwnBld->InLimbo || !pOwnBld->IsAlive || !pOwnBld->Type || pOwnBld->Type->Insignificant
			|| pOwnBld->Type->InvisibleInGame)
		{
			continue;
		}

		const bool isHubAnchor = pOwnBld->Type->ConstructionYard
			|| (pOwnBld->Type->BaseNormal && !pOwnBld->Type->Refinery
				&& pOwnBld->Type->PowerBonus <= pOwnBld->Type->PowerDrain);
		if (!isHubAnchor && !BuildingTypeExt::IsAIBaseNormal(pOwnBld->Type))
			continue;

		const CellStruct bc = pOwnBld->GetMapCoords();
		const int bz = MapClass::Instance.GetMovementZoneType(bc, MovementZone::Normal, pOwnBld->OnBridge);
		const bool isOtherIsland = (bz >= 0 && homeSpawnZone >= 0 && bz != homeSpawnZone);
		const bool isRemoteHubOnSameLand = (homeSpawnRef != CellStruct::Empty
			&& std::max(std::abs(bc.X - homeSpawnRef.X), std::abs(bc.Y - homeSpawnRef.Y)) > 16);
		if (isOtherIsland || (isRemoteHubOnSameLand && isHubAnchor))
		{
			int localHubBlds = 0;
			int localRefs = 0;
			int localDefs = 0;
			int localYards = 0;
			int localSameKindFactories = 0;
			bool hasLocalWarFac = false;
			bool hasLocalBarracks = false;
			for (auto const pCheckB : pHouse->Buildings)
			{
				if (!pCheckB || pCheckB == pOwnBld || pCheckB->InLimbo || !pCheckB->IsAlive || !pCheckB->Type
					|| pCheckB->Type->Insignificant || pCheckB->Type->InvisibleInGame)
				{
					continue;
				}

				const CellStruct cbc = pCheckB->GetMapCoords();
				const bool isLocal = MapClass::Instance.GetMovementZoneType(cbc, MovementZone::Normal, pCheckB->OnBridge) == bz
					&& std::max(std::abs(bc.X - cbc.X), std::abs(bc.Y - cbc.Y)) <= 18;

				if (pCheckB->Type->ConstructionYard)
				{
					if (isLocal)
						localYards++;

					continue;
				}

				if (isLocal)
				{
					localHubBlds++;

					if ((placingWarFactory || placingBarracks) && pCheckB->Type->Factory == pBuildingType->Factory
						&& pCheckB->Type->Naval == pBuildingType->Naval)
					{
						localSameKindFactories++;
					}

					if (pCheckB->Type->Factory == AbstractType::UnitType && !pCheckB->Type->Naval)
					{
						hasLocalWarFac = true;
					}
					else if (pCheckB->Type->Factory == AbstractType::InfantryType)
					{
						hasLocalBarracks = true;
					}
					else if (pCheckB->Type->Refinery || pCheckB->Type->ResourceGatherer
						|| pCheckB->Type->ResourceDestination)
					{
						localRefs++;
					}
					else if (pCheckB->Type->IsBaseDefense && !IsTunnelNetworkBuilding(pCheckB->Type))
					{
						localDefs++;
					}
				}
			}

			const bool hasLocalRef = (localRefs >= 1);
			bool enemyInHubZone = false;
			for (auto const pEB : BuildingClass::Array)
			{
				if (!pEB || pEB->InLimbo || !pEB->IsAlive || !pEB->Owner || !pEB->Type || pEB->Owner == pHouse
					|| pHouse->IsAlliedWith(pEB->Owner) || !IsEnemyBuildingKnown(pHouse, pEB) || pEB->Owner->IsNeutral())
				{
					continue;
				}

				const CellStruct ec = pEB->GetMapCoords();
				if ((bz >= 0 && MapClass::Instance.GetMovementZoneType(ec, MovementZone::Normal, pEB->OnBridge) == bz)
					|| std::max(std::abs(bc.X - ec.X), std::abs(bc.Y - ec.Y)) <= 36)
				{
					enemyInHubZone = true;
					break;
				}
			}

			// A conquered island without a shipyard of ours within 22 cells (shipyards stand on water,
			// so the land zone filter above never counts them).
			const bool islandLacksShipyard = placingShipyard && isOtherIsland
				&& std::none_of(pHouse->Buildings.begin(), pHouse->Buildings.end(), [&bc](BuildingClass* pB)
					{
						return pB && pB->IsAlive && !pB->InLimbo && pB->Type && pB->Type->Factory == AbstractType::UnitType
							&& pB->Type->Naval && pB->GetMapCoords().DistanceFrom(bc) <= 22.0;
					});
			const bool missingThisCoreBld = (placingWarFactory && !hasLocalWarFac)
				|| (placingBarracks && !hasLocalBarracks) || islandLacksShipyard
				|| (isRefineryOrSlaveMiner && (!hasLocalRef || (localRefs == 1 && hasLocalWarFac)))
				|| (pBuildingType->IsBaseDefense && localDefs < 2);
			const bool disperseFactoryHere = homeSameKindFactories >= 2 && localSameKindFactories < homeSameKindFactories;
			const bool strongHub = (pOwnBld->Type->ConstructionYard || localYards > 0) && localDefs >= 3;

			if (keepAtStrongBase && (!strongHub || (enemyInHubZone && launchRing <= 0.0)))
				continue;

			int ringBonus = 0;

			if (launchRing > 0.0)
			{
				const int ringValue = enemyValueInRing(bc);

				if (ringValue <= 0 || ringValue * 2 <= homeRingValue * 3)
					continue; // The main base reaches as much: the launcher stays home.

				ringBonus = 1000 + std::min(2000, ringValue / 10);
			}

			if (localHubBlds < 18 || missingThisCoreBld || disperseFactoryHere || ringBonus > 0)
			{
				int hubScore = (18 - std::min(18, localHubBlds)) * 10 + (pOwnBld->Type->ConstructionYard ? 50 : 0)
					+ (enemyInHubZone ? 250 : 0) + (missingThisCoreBld ? 500 : 0) + ringBonus
					+ (disperseFactoryHere ? 300 + (homeSameKindFactories - localSameKindFactories) * 100 : 0);
				if (hubScore > bestHubPriorityScore)
				{
					bestHubPriorityScore = hubScore;
					anchoredAtMissingFactoryHub = missingThisCoreBld && (placingWarFactory || placingBarracks);
					center = bc;
					targetExpansionZone = bz;
				}
			}
		}
	}

	// Allied Base-Sharing (BuildOffAlly=yes): with no ConYard of our own we build around an
	// ally's base. Defenses for an ally under attack now are anchored later (AllyDefenseCell).
	if (GameModeOptionsClass::Instance.BuildOffAlly && pHouse->ConYards.Count == 0)
	{
		for (auto const pAllyBld : BuildingClass::Array)
		{
			if (!pAllyBld || pAllyBld->InLimbo || !pAllyBld->IsAlive || !pAllyBld->Owner || !pAllyBld->Type)
				continue;

			if (pAllyBld->Owner != pHouse && pHouse->IsAlliedWith(pAllyBld->Owner) && !pAllyBld->Owner->IsNeutral()
				&& pAllyBld->Type->EligibileForAllyBuilding)
			{
				center = pAllyBld->GetMapCoords();

				break;
			}
		}
	}

	if (center == CellStruct::Empty)
		return CellStruct::Empty;

	const int w = pBuildingType->GetFoundationWidth();
	const int h = pBuildingType->GetFoundationHeight(false);
	auto const pExt = HouseExt::Fetch(pHouse);

	const AIBackpackFile& policy = NeuralAI::PolicyFor(pHouse);

	const int crawlBias
		= static_cast<int>(policy.OutputBias[34]) + ((pExt && pExt->NeuralAI_ActiveExpert == 6) ? 220 : 0);

	// Expert 1 (Spatial Base Architect) leads: the base is laid out like a careful player's (see the
	// architect block before the candidate list).
	const bool architectActive = pExt && pExt->NeuralAI_ActiveExpert == 1;

	// Expert 7 puts towers where the enemy really came (FindDefenseNeed), never creeping them toward
	// the enemy base on its own.
	const bool expert7Defense = pExt && pExt->NeuralAI_ActiveExpert == 7;
	const int turretCreepBias = static_cast<int>(policy.OutputBias[38]);

	// Discover nearest uncolonized Ore/Tiberium patch across an 84-cell radius
	// (for Expert 6, Slot 34 & AdvAI Stepping-Stone Base-Crawling). Measure
	// distance from our closest BaseNormal/ConYard anchor and prefer uncolonized
	// Ore patches >= 14 cells from any existing Refinery, outside the ore blocks we
	// already hold (GetHeldOreBlocks).
	const int centerZone = MapClass::Instance.GetMovementZoneType(center, MovementZone::Normal, false);
	CellStruct nearestOreCell = CellStruct::Empty;
	CellStruct fallbackOreCell = CellStruct::Empty;
	int bestOreDist = 9999;
	int bestOreMetric = INT_MAX;
	int bestFallbackOreMetric = INT_MAX;

	// Ore fields are judged as fields, not single tiles: a lone or depleted ore
	// cell must never pull a Refinery into the middle of nowhere, and a field hugging
	// an enemy base is only a last resort.
	std::vector<CellStruct> enemyBaseCells;

	for (auto const pEnemyBld : BuildingClass::Array)
	{
		if (pEnemyBld && !pEnemyBld->InLimbo && pEnemyBld->IsAlive && pEnemyBld->Owner && pEnemyBld->Type
			&& !pEnemyBld->Type->Insignificant && !pEnemyBld->Type->InvisibleInGame && pEnemyBld->Owner != pHouse
			&& !pHouse->IsAlliedWith(pEnemyBld->Owner) && !pEnemyBld->Owner->IsNeutral()
			&& IsEnemyBuildingKnown(pHouse, pEnemyBld))
		{
			enemyBaseCells.push_back(pEnemyBld->GetMapCoords());
		}
	}

	// Ore cells in the 7x7 around a cell, and whether a resource-spawning tree (renewable field)
	// stands within 4 cells of it.
	auto measureOreField = [](const CellStruct& oreCell, bool& hasSpawnerTree) -> int
	{
		int fieldCells = 0;
		hasSpawnerTree = false;

		for (int fy = -4; fy <= 4; ++fy)
		{
			for (int fx = -4; fx <= 4; ++fx)
			{
				const CellStruct fc { static_cast<short>(oreCell.X + fx), static_cast<short>(oreCell.Y + fy) };

				if (auto const pFieldCell = MapClass::Instance.TryGetCellAt(fc))
				{
					if (std::abs(fx) <= 3 && std::abs(fy) <= 3 && pFieldCell->GetContainedTiberiumValue() > 0)
						fieldCells++;

					if (!hasSpawnerTree)
					{
						if (auto const pTerrain = pFieldCell->GetTerrain(false))
						{
							if (pTerrain->IsAlive && pTerrain->Type && pTerrain->Type->SpawnsTiberium)
								hasSpawnerTree = true;
						}
					}
				}
			}
		}

		return fieldCells;
	};

	// Our refineries and building anchors on this landmass, gathered once (the ore scan used to
	// walk every building for every ore cell, which also kept its radius short).
	std::vector<CellStruct> ownRefineryCells;
	std::vector<CellStruct> ownAnchorCells;

	for (auto const pOwnBld : pHouse->Buildings)
	{
		if (!pOwnBld || pOwnBld->InLimbo || !pOwnBld->IsAlive || !pOwnBld->Type || pOwnBld->Type->Insignificant
			|| pOwnBld->Type->InvisibleInGame)
		{
			continue;
		}

		const CellStruct bc = pOwnBld->GetMapCoords();

		if (centerZone >= 0
			&& MapClass::Instance.GetMovementZoneType(bc, MovementZone::Normal, pOwnBld->OnBridge) != centerZone)
		{
			continue;
		}

		if (pOwnBld->Type->Refinery || pOwnBld->Type->ResourceGatherer || pOwnBld->Type->ResourceDestination)
			ownRefineryCells.push_back(bc);

		if (pOwnBld->Type->BaseNormal || pOwnBld->Type->ConstructionYard || BuildingTypeExt::IsAIBaseNormal(pOwnBld->Type))
			ownAnchorCells.push_back(bc);
	}

	// Scattered ore (maps without spawner trees, small patches spread on the ground): a free
	// patch of 2..4 cells is a second tier, still taken before any already colonized ore.
	CellStruct smallPatchCell = CellStruct::Empty;
	int bestSmallPatchMetric = INT_MAX;
	int smallPatchDist = 9999;
	const std::vector<int> heldOreBlocks = GetHeldOreBlocks(pHouse);
	std::vector<CellStruct> nearbyOreCells;

	// Resource claim (UpdateResourceClaim): its field pulls the crawl and the refineries, hard for
	// the resource hoarder (Expert 6), lightly for everyone else.
	const CellStruct claimCell = GetMacroState(pHouse).ResourceClaimCell;
	const int refineryReachFailures = GetMacroState(pHouse).RefineryReachFailures;
	const int refineryReach = refineryReachFailures >= 3 ? INT_MAX : 10 + 4 * refineryReachFailures;
	const int claimPull = (pExt && pExt->NeuralAI_ActiveExpert == 6) ? 25 : 10;

	for (int dy = -120; dy <= 120; dy += (std::abs(dy) <= 44 ? 2 : (std::abs(dy) <= 84 ? 3 : 4)))
	{
		for (int dx = -120; dx <= 120; dx += (std::abs(dx) <= 44 ? 2 : (std::abs(dx) <= 84 ? 3 : 4)))
		{
			const int dCenter = std::abs(dx) + std::abs(dy);
			if (dCenter < 3)
				continue;

			const CellStruct tc { static_cast<short>(center.X + dx), static_cast<short>(center.Y + dy) };
			auto const pCell = MapClass::Instance.TryGetCellAt(tc);
			if (!pCell || pCell->GetContainedTiberiumValue() <= 0)
				continue;

			const int oreZone = MapClass::Instance.GetMovementZoneType(tc, MovementZone::Normal, false);

			// Ore near the base (sampled by this scan): a refinery may stand next to any of it.
			if (std::abs(dx) <= 44 && std::abs(dy) <= 44 && (centerZone < 0 || oreZone == centerZone))
				nearbyOreCells.push_back(tc);

			if (centerZone >= 0 && oreZone != centerZone)
			{
				continue; // Never try to Adjacent=2 base-crawl across water to a
						// different island!
			}

			bool nearExistingRef = IsOreCellInHeldField(heldOreBlocks, tc);
			int minAnchorManhattan = dCenter;

			for (size_t r = 0; !nearExistingRef && r < ownRefineryCells.size(); ++r)
			{
				auto const& rc = ownRefineryCells[r];

				// 14 cells between refineries, as AdvAI spaced them on scattered ore.
				if (std::max(std::abs(tc.X - rc.X), std::abs(tc.Y - rc.Y)) < 14)
					nearExistingRef = true;
			}

			for (auto const& ac : ownAnchorCells)
			{
				const int dAnc = std::abs(tc.X - ac.X) + std::abs(tc.Y - ac.Y);

				if (dAnc < minAnchorManhattan)
					minAnchorManhattan = dAnc;
			}

			// Field size (ore cells in 7x7, max 49) shortens the effective distance, a spawner
			// tree (renewable field) by 10 more cells; tiny patches (< 5 cells) are a second tier
			// and fields within 12 cells of an enemy base are only used when nothing better exists.
			bool hasSpawnerTree = false;
			const int fieldCells = measureOreField(tc, hasSpawnerTree);
			bool nearEnemyBase = false;

			for (auto const& ebc : enemyBaseCells)
			{
				if (std::max(std::abs(tc.X - ebc.X), std::abs(tc.Y - ebc.Y)) <= 12)
				{
					nearEnemyBase = true;
					break;
				}
			}

			const bool isWorthyField = fieldCells >= 5 && !nearEnemyBase;
			const bool inClaimedField = claimCell != CellStruct::Empty
				&& std::max(std::abs(tc.X - claimCell.X), std::abs(tc.Y - claimCell.Y)) <= 8;
			const int oreMetric = minAnchorManhattan - fieldCells / 2 - (hasSpawnerTree ? 10 : 0) + (isWorthyField ? 0 : 1000)
				- ((inClaimedField && isWorthyField) ? claimPull : 0);

			if (oreMetric < bestFallbackOreMetric)
			{
				bestFallbackOreMetric = oreMetric;
				fallbackOreCell = tc;
			}

			if (!isWorthyField)
			{
				const int smallMetric = minAnchorManhattan - fieldCells - (hasSpawnerTree ? 10 : 0);

				if (fieldCells >= 2 && !nearEnemyBase && !nearExistingRef && smallMetric < bestSmallPatchMetric)
				{
					bestSmallPatchMetric = smallMetric;
					smallPatchCell = tc;
					smallPatchDist = minAnchorManhattan;
				}

				continue;
			}

			if ((pHouse->CountResourceDestinations == 0 || !nearExistingRef) && oreMetric < bestOreMetric)
			{
				bestOreMetric = oreMetric;
				bestOreDist = minAnchorManhattan;
				nearestOreCell = tc;
			}
		}
	}

	// A free scattered patch in reach beats a worthy field far away (12 cells of handicap): on
	// maps with ore spread thin every refinery used to march toward a field 60+ cells away,
	// each one placed 5 cells past the last like a stepping stone.
	if (smallPatchCell != CellStruct::Empty
		&& (nearestOreCell == CellStruct::Empty || bestSmallPatchMetric + 12 < bestOreMetric))
	{
		nearestOreCell = smallPatchCell;
		bestOreDist = smallPatchDist;
	}

	const bool hasUncolonizedOre = (nearestOreCell != CellStruct::Empty);

	if (hasUncolonizedOre)
	{
		auto& fieldMacro = GetMacroState(pHouse);
		fieldMacro.FreeFieldAnchorDist = bestOreDist;
		fieldMacro.FreeFieldFrame = Unsorted::CurrentFrame;
	}

	if (nearestOreCell == CellStruct::Empty)
		nearestOreCell = fallbackOreCell;

	// A refinery for the ore we can reach: when the nearest free field is far from every building of
	// ours (the crawl or an MCV goes there), the refinery serves the nearest ore instead, our own
	// field included. Pulled toward a field 50 cells away, every candidate around the base failed the
	// reach check and the ConYard kept building refineries it could not place.
	if (pBuildingType->Refinery && hasUncolonizedOre && bestOreDist > 20 && fallbackOreCell != CellStruct::Empty)
		nearestOreCell = fallbackOreCell;

	// Spawner tree of the target field (within 6 cells of its ore cell): a refinery goes next to the
	// tree, the renewable heart of the field, not at whatever edge the crawl reached first.
	CellStruct oreTreeCell = CellStruct::Empty;

	if (nearestOreCell != CellStruct::Empty && pBuildingType->Refinery)
	{
		int bestTreeDist = INT_MAX;

		for (int ty = -6; ty <= 6; ++ty)
		{
			for (int tx = -6; tx <= 6; ++tx)
			{
				const CellStruct treeCell { static_cast<short>(nearestOreCell.X + tx), static_cast<short>(nearestOreCell.Y + ty) };

				if (auto const pTreeCell = MapClass::Instance.TryGetCellAt(treeCell))
				{
					if (auto const pTerrain = pTreeCell->GetTerrain(false); pTerrain && pTerrain->IsAlive && pTerrain->Type
						&& pTerrain->Type->SpawnsTiberium && std::abs(tx) + std::abs(ty) < bestTreeDist)
					{
						bestTreeDist = std::abs(tx) + std::abs(ty);
						oreTreeCell = treeCell;
					}
				}
			}
		}
	}

	// Discover primary Enemy Threat Vector (for Expert 7 & Slot 38: Forward
	// Defense Crawling & Rear Tech Shielding)
	CellStruct enemyTargetCell = CellStruct::Empty;

	// Fog-fair enemy direction: centroid of the enemy buildings this house has seen
	// (preferring its main enemy); BaseCenter / ConYards would x-ray the fog.
	{
		int sumX = 0;
		int sumY = 0;
		int knownCount = 0;
		int sumAnyX = 0;
		int sumAnyY = 0;
		int knownAnyCount = 0;

		for (auto const pEnemyBld : BuildingClass::Array)
		{
			if (!pEnemyBld || pEnemyBld->InLimbo || !pEnemyBld->IsAlive || !pEnemyBld->Owner
				|| pEnemyBld->Owner == pHouse || pHouse->IsAlliedWith(pEnemyBld->Owner) || pEnemyBld->Owner->IsNeutral()
				|| !IsEnemyBuildingKnown(pHouse, pEnemyBld))
			{
				continue;
			}

			const CellStruct ebc = pEnemyBld->GetMapCoords();
			sumAnyX += ebc.X;
			sumAnyY += ebc.Y;
			knownAnyCount++;

			if (pEnemyBld->Owner->ArrayIndex == pHouse->EnemyHouseIndex)
			{
				sumX += ebc.X;
				sumY += ebc.Y;
				knownCount++;
			}
		}

		if (knownCount > 0)
			enemyTargetCell = CellStruct { static_cast<short>(sumX / knownCount), static_cast<short>(sumY / knownCount) };
		else if (knownAnyCount > 0)
			enemyTargetCell = CellStruct { static_cast<short>(sumAnyX / knownAnyCount), static_cast<short>(sumAnyY / knownAnyCount) };
	}

	// Nothing seen yet: lean toward the nearest unchecked start waypoint (public map
	// knowledge), never the enemy's live BaseCenter or its actual spawn.
	if (enemyTargetCell == CellStruct::Empty)
		enemyTargetCell = FindNextStartLocationToScout(pHouse, center);

	const bool isResourceBld = isRefineryOrSlaveMiner || pBuildingType->Storage > 0 || pBuildingType->OrePurifier;
	const bool isCombatDefense = pBuildingType->IsBaseDefense;
	const auto pWpnStruct = pBuildingType->GetWeapon(0);
	const auto pWpn = pWpnStruct ? pWpnStruct->WeaponType : nullptr;
	const bool isPureAA = isCombatDefense && pWpn && pWpn->Projectile && pWpn->Projectile->AA && !pWpn->Projectile->AG;
	const bool isGroundDefense = isCombatDefense && !isPureAA;
	const bool isTunnelBld = IsTunnelNetworkBuilding(pBuildingType);

	std::vector<CellStruct> existingTunnelCells;

	CellStruct homeFactoryCell = CellStruct::Empty;

	if (isTunnelBld)
	{
		for (auto const pOwnBld : pHouse->Buildings)
		{
			if (!pOwnBld || pOwnBld->InLimbo || !pOwnBld->IsAlive || !pOwnBld->Type)
				continue;

			if (IsTunnelNetworkBuilding(pOwnBld->Type))
			{
				existingTunnelCells.push_back(pOwnBld->GetMapCoords());
			}
			else if (homeFactoryCell == CellStruct::Empty
				&& (pOwnBld->Type->Factory == AbstractType::InfantryType
					|| pOwnBld->Type->Factory == AbstractType::UnitType))
			{
				homeFactoryCell = pOwnBld->GetMapCoords();
			}
		}
	}

	const bool isForwardTunnelMouth = (isTunnelBld && !existingTunnelCells.empty());
	const bool isNavalBld = pBuildingType->Naval || (pBuildingType->SpeedType == SpeedType::Float);

	const bool isFactoryOrBarracks = !isNavalBld
		&& ((pBuildingType->Factory != AbstractType::None) || pBuildingType->WeaponsFactory
			|| pBuildingType->UnitRepair);

	const int supportRadius = GetSupportRadius(pBuildingType);

	const bool isSupportRadiusBld
		= (supportRadius > 0) && !isCombatDefense && !isResourceBld && !isFactoryOrBarracks && !isNavalBld;

	const bool isPowerPlantBld = (pBuildingType->PowerBonus > pBuildingType->PowerDrain) && !isCombatDefense;
	// Stepping-Stone BaseNormal Expander (from AdvAI): After our 1st Power Plant,
	// BaseNormal Power Plants (and Outposts) step outward toward uncolonized Ore
	// fields or the frontline instead of crowding into the rear corner behind the
	// ConYard!
	// Precious or explosive structures (expensive, or blowing up on death like a nuclear reactor)
	// never act as crawl stones and keep away from known enemy structures: a stone is the
	// cheapest power plant we can build (within 1.5x of it), never a reactor at the enemy's door.
	const bool isExplosiveBld = pBuildingType->Explodes || pBuildingType->DeathWeapon != nullptr;
	// A power plant that will give >= 25% of the house's output is a critical power source: losing
	// it at the front could black the base out, so it stays at the rear like a precious structure.
	const bool isCriticalPowerSource = isPowerPlantBld && pBuildingType->PowerBonus > 0
		&& pBuildingType->PowerBonus * 4 >= pHouse->PowerOutput + pBuildingType->PowerBonus;
	const bool isPreciousBld = !isCombatDefense && !isFactoryOrBarracks && !isResourceBld && !isNavalBld
		&& (isExplosiveBld || pBuildingType->GetActualCost(pHouse) >= 1500 || isCriticalPowerSource);
	int cheapestPowerPlantCost = INT_MAX;
	int cheapestPowerPlantArea = INT_MAX;

	if (isPowerPlantBld && pBuildingType->BaseNormal)
	{
		for (auto const pBT : BuildingTypeClass::Array)
		{
			if (pBT && pBT->BaseNormal && pBT->PowerBonus > pBT->PowerDrain && !pBT->IsBaseDefense && pBT->Cost > 0
				&& CanBuildUnderHumanRules(pHouse, pBT))
			{
				const int btCost = pBT->GetActualCost(pHouse);

				if (btCost < cheapestPowerPlantCost)
				{
					cheapestPowerPlantCost = btCost;
					cheapestPowerPlantArea = pBT->GetFoundationWidth() * pBT->GetFoundationHeight(false);
				}
			}
		}
	}

	// A stone is expendable: cheap (<= 1.5x the cheapest plant), no bigger than the cheapest plant's
	// footprint, not explosive, and the house already has a power surplus without it (it is never
	// the plant that fixes a shortage, so losing it never blacks the base out).
	const bool isCheapStone = !isExplosiveBld && !isCriticalPowerSource
		&& pHouse->PowerOutput >= pHouse->PowerDrain + 50
		&& (cheapestPowerPlantCost == INT_MAX
			|| (pBuildingType->GetActualCost(pHouse) * 2 <= cheapestPowerPlantCost * 3
				&& pBuildingType->GetFoundationWidth() * pBuildingType->GetFoundationHeight(false) <= cheapestPowerPlantArea));
	// Under Expert 1 a stone only carries the crawl to free ore: with every field in reach taken it is
	// a plain power plant grouped behind the core, never a plant marching to the enemy's door.
	const bool offensiveBank = HasOffensiveBank(pHouse);
	const bool isSteppingStoneExpander = pBuildingType->BaseNormal && isPowerPlantBld && isCheapStone && (pHouse->PowerOutput >= 150)
		&& (nearestOreCell != CellStruct::Empty || crawlBias > 0 || offensiveBank) && (!architectActive || hasUncolonizedOre);
	const bool isRearProtected = !isNavalBld && !isResourceBld && !isCombatDefense && !isFactoryOrBarracks
		&& !isTunnelBld && !isSupportRadiusBld && !isSteppingStoneExpander;

	// Offensive base crawl (asphyxiation, AdvAI's combat crawling target): once every worthy ore
	// field in reach is ours, stepping stones march toward the enemy base instead of clumping
	// around colonized ore; while free fields remain, one stone in four still goes toward the
	// enemy (two in four with a strong learned creep preference: slot 38 Forward Turret Creep +
	// half of slot 81 Nomadic Siege Creep) so the front moves too; with an offensive bank
	// (HasOffensiveBank) every stone does. Defenses then anchor at the most forward structure
	// (turret creep). Stones stay out of reach of known enemy structures.
	const AIBackpackFile& crawlPolicy = NeuralAI::PolicyFor(pHouse);
	const int offensiveCrawlBias
		= static_cast<int>(crawlPolicy.OutputBias[38]) + static_cast<int>(crawlPolicy.OutputBias[81]) / 2;
	const int stoneIndex = isSteppingStoneExpander ? pHouse->CountOwnedNow(pBuildingType) : 0;
	const bool stoneTowardEnemy = isSteppingStoneExpander && enemyTargetCell != CellStruct::Empty && !architectActive
		&& offensiveCrawlBias > -400
		&& (!hasUncolonizedOre || offensiveBank || (stoneIndex % 4) == 3 || (offensiveCrawlBias > 300 && (stoneIndex % 4) == 1));
	// Several enemies (free-for-all): the offensive crawl heads for the nearest known enemy base,
	// rotating stone by stone between every enemy within 1.5x of that distance, so it can choke
	// several neighbours instead of only the main enemy.
	CellStruct crawlEnemyGoal = enemyTargetCell;

	if (stoneTowardEnemy)
	{
		std::array<int, 32> sumX {};
		std::array<int, 32> sumY {};
		std::array<int, 32> known {};

		for (auto const pEnemyBld : BuildingClass::Array)
		{
			if (!pEnemyBld || pEnemyBld->InLimbo || !pEnemyBld->IsAlive || !pEnemyBld->Owner || pEnemyBld->Owner == pHouse
				|| pHouse->IsAlliedWith(pEnemyBld->Owner) || pEnemyBld->Owner->IsNeutral() || !IsEnemyBuildingKnown(pHouse, pEnemyBld))
			{
				continue;
			}

			const int idx = pEnemyBld->Owner->ArrayIndex;

			if (idx < 0 || idx >= 32)
				continue;

			const CellStruct ebc = pEnemyBld->GetMapCoords();

			if (centerZone >= 0 && MapClass::Instance.GetMovementZoneType(ebc, MovementZone::Normal, pEnemyBld->OnBridge) != centerZone)
				continue;

			sumX[idx] += ebc.X;
			sumY[idx] += ebc.Y;
			known[idx]++;
		}

		std::vector<std::pair<double, CellStruct>> enemyBases;

		for (int idx = 0; idx < 32; ++idx)
		{
			if (known[idx] == 0)
				continue;

			const CellStruct c { static_cast<short>(sumX[idx] / known[idx]), static_cast<short>(sumY[idx] / known[idx]) };
			enemyBases.emplace_back(c.DistanceFrom(center), c);
		}

		if (!enemyBases.empty())
		{
			std::sort(enemyBases.begin(), enemyBases.end(), [](auto const& a, auto const& b)
				{ return a.first != b.first ? a.first < b.first : (a.second.X != b.second.X ? a.second.X < b.second.X : a.second.Y < b.second.Y); });

			size_t eligible = 1;

			while (eligible < enemyBases.size() && enemyBases[eligible].first <= enemyBases.front().first * 1.5)
				eligible++;

			crawlEnemyGoal = enemyBases[static_cast<size_t>(stoneIndex) % eligible].second;
		}
	}

	const CellStruct stoneGoal = stoneTowardEnemy ? crawlEnemyGoal : nearestOreCell;
	std::vector<std::pair<CellStruct, int>> enemyStructureReach;

	if (stoneTowardEnemy)
	{
		for (auto const pEB : BuildingClass::Array)
		{
			if (!pEB || pEB->InLimbo || !pEB->IsAlive || !pEB->Owner || !pEB->Type || pEB->Type->Insignificant
				|| pEB->Type->InvisibleInGame || pEB->Owner == pHouse || pHouse->IsAlliedWith(pEB->Owner)
				|| pEB->Owner->IsNeutral() || !IsEnemyBuildingKnown(pHouse, pEB))
			{
				continue;
			}

			int reach = 10;

			if (auto const pWS = pEB->GetWeapon(0); pWS && pWS->WeaponType)
				reach = std::max(reach, pWS->WeaponType->Range / 256 + 3);

			enemyStructureReach.emplace_back(pEB->GetMapCoords(), reach);
		}
	}

	// Forward factories: once the offensive crawl holds a front (forward stones alive), extra War
	// Factories / Barracks (never the first one) and repair depots go next to it, behind our
	// towers and off the crawl lane, so reinforcements reach the fight sooner.
	const bool frontFactory = isFactoryOrBarracks && enemyTargetCell != CellStruct::Empty
		&& !GetMacroState(pHouse).OffensiveStones.empty()
		&& ((pBuildingType->Factory == AbstractType::UnitType && pHouse->NumWarFactories >= 1)
			|| (pBuildingType->Factory == AbstractType::InfantryType && pHouse->NumBarracks >= 1)
			|| pBuildingType->UnitRepair);

	if (frontFactory && enemyStructureReach.empty())
	{
		for (auto const pEB : BuildingClass::Array)
		{
			if (!pEB || pEB->InLimbo || !pEB->IsAlive || !pEB->Owner || !pEB->Type || pEB->Type->Insignificant
				|| pEB->Type->InvisibleInGame || pEB->Owner == pHouse || pHouse->IsAlliedWith(pEB->Owner)
				|| pEB->Owner->IsNeutral() || !IsEnemyBuildingKnown(pHouse, pEB))
			{
				continue;
			}

			int reach = 10;

			if (auto const pWS = pEB->GetWeapon(0); pWS && pWS->WeaponType)
				reach = std::max(reach, pWS->WeaponType->Range / 256 + 3);

			enemyStructureReach.emplace_back(pEB->GetMapCoords(), reach);
		}
	}

	// Our ground defenses: a stone covered by two or more of them may step inside enemy reach
	// (the crawl cycle: stone, towers, next stone under their cover, toward the enemy's heart).
	std::vector<CellStruct> ownCoverDefenses;

	if (stoneTowardEnemy || frontFactory)
	{
		for (auto const pOwnB : pHouse->Buildings)
		{
			if (!pOwnB || pOwnB->InLimbo || !pOwnB->IsAlive || !pOwnB->Type || !pOwnB->Type->IsBaseDefense
				|| IsTunnelNetworkBuilding(pOwnB->Type))
			{
				continue;
			}

			auto const pWS = pOwnB->GetWeapon(0);

			if (pWS && pWS->WeaponType && pWS->WeaponType->Projectile && pWS->WeaponType->Projectile->AG)
				ownCoverDefenses.push_back(pOwnB->GetMapCoords());
		}
	}

	// Turret creep front (set below when defenses anchor at the most forward structure toward
	// the enemy): towers consolidate beside the crawl lane, never on it.
	CellStruct creepFrontGoal = CellStruct::Empty;

	// Gather Support Radius coverage & separation targets
	// (Gap/Cloak/Sensor/Jammer)
	std::vector<CellStruct> existingSameSupportCells;

	std::vector<CellStruct> uncoveredKeyBldCells;

	const int targetSupportSeparation = supportRadius > 1 ? std::max(7, (supportRadius * 3) / 2) : 8;

	if (isSupportRadiusBld)
	{
		std::vector<CellStruct> keyBaseCells;

		for (auto const pOwnBld : pHouse->Buildings)
		{
			if (!pOwnBld || pOwnBld->InLimbo || !pOwnBld->IsAlive || !pOwnBld->Type)
				continue;

			auto const pOT = pOwnBld->Type;
			const CellStruct bc = pOwnBld->GetMapCoords();

			if (pOT == pBuildingType || (pOT->GapGenerator && pBuildingType->GapGenerator)
				|| (pOT->CloakGenerator && pBuildingType->CloakGenerator)
				|| (pOT->SensorArray && pBuildingType->SensorArray))
			{
				existingSameSupportCells.push_back(bc);
			}

			if (pOT->ConstructionYard || pOT->Factory != AbstractType::None || pOT->SuperWeapon != -1 || pOT->Refinery
				|| pOT->Radar)
			{
				keyBaseCells.push_back(bc);
			}
		}

		for (const CellStruct& kc : keyBaseCells)
		{
			bool covered = false;

			for (const CellStruct& sc : existingSameSupportCells)
			{
				if (kc.DistanceFrom(sc) <= std::max(3, supportRadius - 1))
				{
					covered = true;

					break;
				}
			}

			if (!covered)
				uncoveredKeyBldCells.push_back(kc);
		}
	}

	// Gather Directional Attack Vector (from FrontlineThreat & AttackedBuildingsLIFO) and
	// Undefended Key Assets / Vulnerable Power Plant Clusters (3+ plants grouped)
	CellStruct directionalAttackedBld = CellStruct::Empty;
	CellStruct directionalAttacker = CellStruct::Empty;
	CellStruct undefendedKeyAssetCell = CellStruct::Empty;
	int totalOwnedDefensesCount = 0;
	std::vector<CellStruct> ownMatchingDefCells;

	auto& macroState = GetMacroState(pHouse);
	macroState.UnsafePlacementZones.erase(
		std::remove_if(macroState.UnsafePlacementZones.begin(), macroState.UnsafePlacementZones.end(),
			[](const NeuralUnsafeZone& z) { return Unsorted::CurrentFrame > z.ExpirationFrame; }),
		macroState.UnsafePlacementZones.end());

	if (isCombatDefense)
	{
		bool defAntiInf = false;
		bool defAntiArmor = false;
		bool defAntiAir = false;
		bool defPureAA = false;
		bool defUnarmedBunker = false;
		GetDefenseCapabilities(pBuildingType, defAntiInf, defAntiArmor, defAntiAir, defPureAA, defUnarmedBunker);

		// Priority 1 (AdvAI FrontlineThreat): If a recently placed building detected an
		// enemy threat within 8 cells, place this ground defense directly between our
		// building and that enemy threat!
		if (!isPureAA && macroState.FrontlineThreatNeedsDefenses > 0
			&& Unsorted::CurrentFrame <= macroState.FrontlineThreatActiveFrames
			&& macroState.FrontlineThreatBuildingCoords != CellStruct::Empty
			&& macroState.FrontlineThreatCoords != CellStruct::Empty)
		{
			directionalAttackedBld = macroState.FrontlineThreatBuildingCoords;
			directionalAttacker = macroState.FrontlineThreatCoords;
		}
		else if (!macroState.AttackedBuildingsLIFO.empty())
		{
			PruneAttackedBuildingsLIFO(pHouse);

			int chosenIdx = -1;

			for (int i = static_cast<int>(macroState.AttackedBuildingsLIFO.size()) - 1; i >= 0; --i)
			{
				const auto& rec = macroState.AttackedBuildingsLIFO[i];

				if ((rec.Threat == NeuralThreatCategory::Air && defAntiAir)
					|| (rec.Threat == NeuralThreatCategory::Vehicle && defAntiArmor)
					|| (rec.Threat == NeuralThreatCategory::Infantry && defAntiInf))
				{
					chosenIdx = i;

					break;
				}
			}

			if (chosenIdx < 0 && !macroState.AttackedBuildingsLIFO.empty())
			{
				for (int i = static_cast<int>(macroState.AttackedBuildingsLIFO.size()) - 1; i >= 0; --i)
				{
					const auto& rec = macroState.AttackedBuildingsLIFO[i];
					if ((rec.Threat == NeuralThreatCategory::Air && isPureAA)
						|| (rec.Threat != NeuralThreatCategory::Air && isGroundDefense))
					{
						chosenIdx = i;
						break;
					}
				}
			}

			if (chosenIdx >= 0)
			{
				directionalAttackedBld = macroState.AttackedBuildingsLIFO[chosenIdx].BuildingCoords;

				directionalAttacker = macroState.AttackedBuildingsLIFO[chosenIdx].AttackerCoords;
			}
		}

		// Expert 7: our structure most in need from what really came at us (sightings and hits),
		// ahead of a forward building that merely stands near the enemy.
		if (expert7Defense)
		{
			CellStruct needAsset = CellStruct::Empty;
			CellStruct needThreat = CellStruct::Empty;

			if (FindDefenseNeed(pHouse, isPureAA, GroundRangeOf(pBuildingType) / Unsorted::LeptonsPerCell, needAsset, needThreat))
			{
				directionalAttackedBld = needAsset;
				directionalAttacker = needThreat;
				Debug::Log("[NeuralAI] House %d Expert 7: %s guards (%d,%d) against (%d,%d)\n", pHouse->ArrayIndex,
					pBuildingType->ID, needAsset.X, needAsset.Y, needThreat.X, needThreat.Y);
			}
		}

		std::vector<CellStruct> powerPlantCells;

		std::vector<CellStruct> priorityAssetCells;

		for (auto const pOwnBld : pHouse->Buildings)
		{
			if (!pOwnBld || pOwnBld->InLimbo || !pOwnBld->IsAlive || !pOwnBld->Type)
				continue;

			auto const pOT = pOwnBld->Type;
			const CellStruct bc = pOwnBld->GetMapCoords();

			if (pOT->IsBaseDefense && !IsTunnelNetworkBuilding(pOT))
			{
				totalOwnedDefensesCount++;

				const auto pWS = pOT->GetWeapon(0);
				const auto pW = pWS ? pWS->WeaponType : nullptr;
				const bool otherPureAA = pW && pW->Projectile && pW->Projectile->AA && !pW->Projectile->AG;

				if (otherPureAA == isPureAA)
					ownMatchingDefCells.push_back(bc);
			}
			else if (pOT->PowerBonus > pOT->PowerDrain)
			{
				powerPlantCells.push_back(bc);
			}

			const bool isCapturedTechOrCash = (pOT->ProduceCashAmount > 0 || pOT->ProduceCashStartup > 0
				|| pOT->ToProtect || (pOT->Capturable && pOT->NeedsEngineer));
			const bool isRemoteAsset = (std::max(std::abs(bc.X - center.X), std::abs(bc.Y - center.Y)) > 15);
			if (isCapturedTechOrCash
				|| (isRemoteAsset && (pOT->Refinery || pOT->ConstructionYard || pOT->Factory != AbstractType::None)))
			{
				priorityAssetCells.insert(priorityAssetCells.begin(),
					bc); // Highest priority: defend remote Oil Derricks, Tech Outposts
						// & Expansion Refineries!
			}
			else if (pOT->ConstructionYard || pOT->Factory == AbstractType::UnitType || pOT->SuperWeapon != -1
				|| pOT->Refinery)
			{
				priorityAssetCells.push_back(bc);
			}
		}

		// Detect Vulnerable Power Plant Clusters (3+ Power Plants within 8 cells)
		for (const CellStruct& ppc : powerPlantCells)
		{
			int nearbyPlants = 0;

			for (const CellStruct& otherPpc : powerPlantCells)
			{
				if (std::max(std::abs(ppc.X - otherPpc.X), std::abs(ppc.Y - otherPpc.Y)) <= 8)
					nearbyPlants++;
			}

			if (nearbyPlants >= 3)
				priorityAssetCells.push_back(ppc);
		}

		// Find the first priority asset or power cluster that lacks a nearby
		// defense (or has < 2 ground defenses when we already have >= 3 total defenses)
		const int minDefsPerKeyAsset = (!isPureAA && ownMatchingDefCells.size() >= 3) ? 2 : 1;
		for (const CellStruct& ac : priorityAssetCells)
		{
			int localDefsNearAsset = 0;

			for (const CellStruct& dc : ownMatchingDefCells)
			{
				if (std::max(std::abs(ac.X - dc.X), std::abs(ac.Y - dc.Y)) <= 10)
					localDefsNearAsset++;
			}

			if (localDefsNearAsset < minDefsPerKeyAsset)
			{
				undefendedKeyAssetCell = ac;

				break;
			}
		}

		// If no directionalAttackedBld is set yet for a ground defense, check if any of our
		// buildings is within 26 cells of an enemy building/unit OR contesting an Ore patch
		// with fewer than 4 local ground defenses (Base-Crawling Siege & Resource Choke Harassment!).
		// Not under Expert 7: a building near enemy structures is no sign of where they attack.
		if (!isPureAA && directionalAttackedBld == CellStruct::Empty && !expert7Defense)
		{
			int bestChokeScore = -999999;
			for (auto const pOwnBld : pHouse->Buildings)
			{
				if (!pOwnBld || pOwnBld->InLimbo || !pOwnBld->IsAlive || !pOwnBld->Type || pOwnBld->Type->Insignificant
					|| pOwnBld->Type->InvisibleInGame)
				{
					continue;
				}

				const CellStruct bc = pOwnBld->GetMapCoords();
				int localDefsHere = 0;
				for (const CellStruct& dc : ownMatchingDefCells)
				{
					if (std::max(std::abs(bc.X - dc.X), std::abs(bc.Y - dc.Y)) <= 10)
						localDefsHere++;
				}

				if (localDefsHere >= 4)
					continue;

				CellStruct closestThreat = CellStruct::Empty;
				int minThreatDist = 9999;
				for (auto const pEB : BuildingClass::Array)
				{
					if (!pEB || pEB->InLimbo || !pEB->IsAlive || !pEB->Owner || !pEB->Type || pEB->Type->Insignificant
						|| pEB->Type->InvisibleInGame || pEB->Owner == pHouse || pHouse->IsAlliedWith(pEB->Owner) || !IsEnemyBuildingKnown(pHouse, pEB)
						|| pEB->Owner->IsNeutral() || pEB->Owner->IsObserver() || pEB->Owner->Defeated)
					{
						continue;
					}

					const CellStruct ec = pEB->GetMapCoords();
					const int dCheb = std::max(std::abs(bc.X - ec.X), std::abs(bc.Y - ec.Y));
					if (dCheb <= 28 && dCheb < minThreatDist)
					{
						minThreatDist = dCheb;
						closestThreat = ec;
					}
				}

				for (auto const pEU : UnitClass::Array)
				{
					if (!pEU || pEU->InLimbo || !pEU->IsAlive || !pEU->Owner || !pEU->Type || pEU->Owner == pHouse
						|| pHouse->IsAlliedWith(pEU->Owner) || !IsCellVisibleToAI(pHouse, pEU->GetCell()) || pEU->Owner->IsNeutral() || pEU->Owner->IsObserver()
						|| pEU->Owner->Defeated)
					{
						continue;
					}

					const CellStruct euc = pEU->GetMapCoords();
					const int dCheb = std::max(std::abs(bc.X - euc.X), std::abs(bc.Y - euc.Y));
					if (dCheb <= 20 && dCheb < minThreatDist)
					{
						minThreatDist = dCheb;
						closestThreat = euc;
					}
				}

				if (closestThreat != CellStruct::Empty)
				{
					const int sc = (32 - minThreatDist) * 40 - localDefsHere * 90;
					if (sc > bestChokeScore)
					{
						bestChokeScore = sc;
						directionalAttackedBld = bc;
						directionalAttacker = closestThreat;
					}
				}
			}
		}
	}

	// True C&C Base-Crawling Chain & Forward Turret Creep: Instead of always
	// anchoring at ConYards[0] (which forms a static circle around spawn), anchor
	// at our most forward owned structure toward the Ore field or Enemy base so
	// buildings chain across the map! Forward Tunnel Mouths (2nd+ mouth built
	// from Construction Yard) ALWAYS anchor at our most forward
	// outpost/base-crawl structure!
	const bool enableBaseCrawl = isForwardTunnelMouth || isSteppingStoneExpander
		|| ((isResourceBld || (isFactoryOrBarracks && !anchoredAtMissingFactoryHub))
			&& (crawlBias > -20 || bestOreDist > 10 || pHouse->CountResourceDestinations >= 1))
		|| (isCombatDefense && !isPureAA && (turretCreepBias > -20 || crawlBias > 0 || totalOwnedDefensesCount >= 1)
			&& directionalAttackedBld == CellStruct::Empty && !expert7Defense);

	// Team games: the crawl starts from an ally building closer to the ore (below).
	bool crawlFromAllyAnchor = false;

	if (isCombatDefense && directionalAttackedBld != CellStruct::Empty)
	{
		center = directionalAttackedBld;
	}
	else if (enableBaseCrawl && isCombatDefense && (totalOwnedDefensesCount >= 2 || turretCreepBias > 40))
	{
		// Forward Turret Creep & Resource Choke Denial: anchor at our most forward
		// BaseNormal / ConYard structure toward the enemy base or contested Ore field!
		const CellStruct crawlGoal = (enemyTargetCell != CellStruct::Empty) ? enemyTargetCell : nearestOreCell;
		if (crawlGoal != CellStruct::Empty)
		{
			if (crawlGoal == enemyTargetCell)
				creepFrontGoal = crawlGoal;

			int bestAnchorDist = std::abs(center.X - crawlGoal.X) + std::abs(center.Y - crawlGoal.Y);
			for (auto const pOwnBld : pHouse->Buildings)
			{
				if (!pOwnBld || pOwnBld->InLimbo || !pOwnBld->IsAlive || !pOwnBld->Type || pOwnBld->Type->Insignificant
					|| pOwnBld->Type->InvisibleInGame)
				{
					continue;
				}

				if (!pOwnBld->Type->BaseNormal && !pOwnBld->Type->ConstructionYard
					&& !BuildingTypeExt::IsAIBaseNormal(pOwnBld->Type))
				{
					continue;
				}

				const CellStruct bc = pOwnBld->GetMapCoords();
				if (centerZone >= 0
					&& MapClass::Instance.GetMovementZoneType(bc, MovementZone::Normal, pOwnBld->OnBridge) != centerZone)
				{
					continue;
				}

				const int dGoal = std::abs(bc.X - crawlGoal.X) + std::abs(bc.Y - crawlGoal.Y);
				if (dGoal < bestAnchorDist)
				{
					bestAnchorDist = dGoal;
					center = bc;
				}
			}
		}
	}
	else if (isCombatDefense && undefendedKeyAssetCell != CellStruct::Empty)
	{
		center = undefendedKeyAssetCell;
	}
	else if (enableBaseCrawl)
	{
		const bool crawlTowardOre = isResourceBld || isSteppingStoneExpander
			|| (isFactoryOrBarracks && !frontFactory && nearestOreCell != CellStruct::Empty && bestOreDist > 10
				&& pHouse->CountResourceDestinations < 3);
		const CellStruct crawlGoal = (isSteppingStoneExpander && stoneGoal != CellStruct::Empty)
			? stoneGoal
			: frontFactory ? enemyTargetCell
			: (crawlTowardOre ? (nearestOreCell != CellStruct::Empty ? nearestOreCell : enemyTargetCell)
							  : (enemyTargetCell != CellStruct::Empty ? enemyTargetCell : nearestOreCell));

		if (crawlGoal != CellStruct::Empty)
		{
			int bestAnchorDist = std::abs(center.X - crawlGoal.X) + std::abs(center.Y - crawlGoal.Y);

			for (auto const pOwnBld : pHouse->Buildings)
			{
				if (!pOwnBld || pOwnBld->InLimbo || !pOwnBld->IsAlive || !pOwnBld->Type || pOwnBld->Type->Insignificant
					|| pOwnBld->Type->InvisibleInGame)
				{
					continue;
				}

				if (!pOwnBld->Type->BaseNormal && !pOwnBld->Type->ConstructionYard
					&& !BuildingTypeExt::IsAIBaseNormal(pOwnBld->Type))
				{
					continue;
				}

				// A refinery never anchors the next one, nor does a building within 8 cells of one
				// once two stand (AdvAI): chained refineries stood 5 cells apart on the way to the ore.
				// Stepping stones carry the crawl, the refinery comes once one of them reaches the field.
				if (pBuildingType->Refinery
					&& (pOwnBld->Type->Refinery || pOwnBld->Type->ResourceDestination
						|| (ownRefineryCells.size() >= 2
							&& std::any_of(ownRefineryCells.begin(), ownRefineryCells.end(), [pOwnBld](const CellStruct& rc)
								{ return std::max(std::abs(pOwnBld->GetMapCoords().X - rc.X), std::abs(pOwnBld->GetMapCoords().Y - rc.Y)) <= 8; }))))
				{
					continue;
				}

				const CellStruct bc = pOwnBld->GetMapCoords();
				if (centerZone >= 0
					&& MapClass::Instance.GetMovementZoneType(bc, MovementZone::Normal, pOwnBld->OnBridge) != centerZone)
				{
					continue;
				}

				const int dGoal = std::abs(bc.X - crawlGoal.X) + std::abs(bc.Y - crawlGoal.Y);

				if (dGoal < bestAnchorDist)
				{
					bestAnchorDist = dGoal;

					center = bc;
				}
			}

			// Team games (BuildOffAlly): an ally building 6+ cells closer to the ore carries the
			// crawl while the ally keeps room there; candidates then keep a free cell from its
			// buildings and 4 from its ConYard (allySiteKeepClear). Its refineries never anchor, nor
			// does a building within 20 cells of a ConYard of ours: when the starts are next to each
			// other only the ally's expansions are worth crawling from.
			if (crawlTowardOre && GameModeOptionsClass::Instance.BuildOffAlly)
			{
				BuildingClass* pAllyAnchor = nullptr;
				int bestAllyDist = bestAnchorDist - 6;

				for (auto const pAllyBld : BuildingClass::Array)
				{
					if (!pAllyBld || !pAllyBld->IsAlive || pAllyBld->InLimbo || !pAllyBld->Type || !pAllyBld->Owner
						|| pAllyBld->Owner == pHouse || !pHouse->IsAlliedWith(pAllyBld->Owner) || pAllyBld->Owner->IsNeutral()
						|| !pAllyBld->Type->EligibileForAllyBuilding || !pAllyBld->Type->BaseNormal || pAllyBld->Type->Refinery
						|| pAllyBld->Type->ResourceDestination)
					{
						continue;
					}

					const CellStruct bc = pAllyBld->GetMapCoords();

					if (std::any_of(pHouse->ConYards.begin(), pHouse->ConYards.end(),
							[&bc](BuildingClass* pYard) { return pYard && pYard->GetMapCoords().DistanceFrom(bc) < 20.0; }))
					{
						continue;
					}

					if (centerZone >= 0
						&& MapClass::Instance.GetMovementZoneType(bc, MovementZone::Normal, pAllyBld->OnBridge) != centerZone)
					{
						continue;
					}

					const int dGoal = std::abs(bc.X - crawlGoal.X) + std::abs(bc.Y - crawlGoal.Y);

					if (dGoal < bestAllyDist || (dGoal == bestAllyDist && pAllyAnchor && pAllyBld->UniqueID < pAllyAnchor->UniqueID))
					{
						bestAllyDist = dGoal;
						pAllyAnchor = pAllyBld;
					}
				}

				if (pAllyAnchor && AllyKeepsRoomAround(pAllyAnchor->GetMapCoords(), pAllyAnchor->Owner, pBuildingType))
				{
					center = pAllyAnchor->GetMapCoords();
					crawlFromAllyAnchor = true;
				}
			}
		}
	}

	// Route heat spot this ground tower is anchored to cover (Empty: none).
	CellStruct routeDefenseSpot = CellStruct::Empty;

	// Island Hold (UpdateStrategicPlans): a defense the island yard still lacks goes next to it, on
	// a cell the plan counts (its radius, its landmass), whatever else asks for a tower. An attack
	// remembered somewhere else does not take it: a house is hardly ever without one, and the towers
	// built during the plan end at the main base (match #8875: five towers in 5400 frames, none at
	// the yard; match #8864: towers 32 and 36 cells from it). A building an ally site plan ordered
	// keeps its site.
	const bool holdingIsland = isCombatDefense && IslandHoldWants(pHouse, pBuildingType)
		&& (macroState.AllySiteUntilFrame <= Unsorted::CurrentFrame || macroState.AllySiteBuildType != pBuildingType->ArrayIndex);
	const CellStruct islandHoldYardCell = holdingIsland ? macroState.IslandHoldCell : CellStruct::Empty;
	const int islandHoldZone = holdingIsland
		? MapClass::Instance.GetMovementZoneType(islandHoldYardCell, MovementZone::Normal, false)
		: -1;

	// Strategic plan anchors (UpdateStrategicPlans): defenses of a new island ConYard go next to
	// it (Island Hold), a second repair station next to our structure closest to the front
	// (Forward Repair). Elsewhere a defense needed where we are being attacked keeps priority.
	{
		auto const& planMacro = GetMacroState(pHouse);

		const int mainAttackSector = isCombatDefense ? GetMainAttackSector(pHouse) : -1;

		// Route heat: the hottest approach our towers do not cover yet (chokepoints first) is a
		// better target than the side we were attacked from. Only spots this tower can reach from
		// a cell next to our structures (its range plus its Adjacent distance).
		const int routeReachCells = GroundRangeOf(pBuildingType) / Unsorted::LeptonsPerCell
			+ std::max(1, static_cast<int>(pBuildingType->Adjacent)) + 1;
		auto const pRouteSpot = (isCombatDefense && !isPureAA && directionalAttackedBld == CellStruct::Empty)
			? BestRouteDefenseSpot(pHouse, routeReachCells)
			: nullptr;

		if (holdingIsland)
		{
			center = islandHoldYardCell;
			targetExpansionZone = -1;
			creepFrontGoal = CellStruct::Empty;

			// Only an attack on the island itself or an asset there steers where the tower faces.
			if (directionalAttackedBld != CellStruct::Empty && directionalAttackedBld.DistanceFrom(center) > IslandHoldDefenseRadius)
			{
				directionalAttackedBld = CellStruct::Empty;
				directionalAttacker = CellStruct::Empty;
			}

			if (undefendedKeyAssetCell != CellStruct::Empty && undefendedKeyAssetCell.DistanceFrom(center) > IslandHoldDefenseRadius)
				undefendedKeyAssetCell = CellStruct::Empty;
		}
		else if ((pRouteSpot || mainAttackSector >= 0) && directionalAttackedBld == CellStruct::Empty
			&& center.DistanceFrom(GetHouseHomeCell(pHouse)) <= 24.0) // Forward turret creep keeps its own anchor.
		{
			// Attack route memory: fortify the route the enemy really takes (else the side we have
			// really been attacked from), anchored at our structure closest to it.
			const CellStruct home = GetHouseHomeCell(pHouse);
			CellStruct routePoint = CellStruct::Empty;

			if (pRouteSpot)
			{
				routePoint = pRouteSpot->Cell;
				routeDefenseSpot = routePoint;
			}
			else
			{
				const int reach = (AttackSectorDX[mainAttackSector] != 0 && AttackSectorDY[mainAttackSector] != 0) ? 10 : 14;
				routePoint = CellStruct { static_cast<short>(home.X + AttackSectorDX[mainAttackSector] * reach),
					static_cast<short>(home.Y + AttackSectorDY[mainAttackSector] * reach) };
			}

			const int homeZoneForRoute = MapClass::Instance.GetMovementZoneType(home, MovementZone::Normal, false);
			double bestAnchorDist = center.DistanceFrom(routePoint);
			bool moved = false;

			for (auto const pOwnBld : pHouse->Buildings)
			{
				if (!pOwnBld || pOwnBld->InLimbo || !pOwnBld->IsAlive || !pOwnBld->Type || pOwnBld->Type->Insignificant
					|| pOwnBld->Type->InvisibleInGame
					|| (!pOwnBld->Type->BaseNormal && !pOwnBld->Type->ConstructionYard && !BuildingTypeExt::IsAIBaseNormal(pOwnBld->Type)))
				{
					continue;
				}

				const CellStruct bc = pOwnBld->GetMapCoords();

				if ((bc.DistanceFrom(home) > 24.0 && (!pRouteSpot || bc.DistanceFrom(routePoint) > 16.0))
					|| MapClass::Instance.GetMovementZoneType(bc, MovementZone::Normal, pOwnBld->OnBridge) != homeZoneForRoute)
				{
					continue;
				}

				const double d = bc.DistanceFrom(routePoint);

				if (d < bestAnchorDist)
				{
					bestAnchorDist = d;
					center = bc;
					moved = true;
				}
			}

			if (moved)
				CountDecision(NeuralDecision::AttackRouteDefense, pHouse->ArrayIndex);
		}
		else if (pBuildingType->UnitRepair && !pBuildingType->Naval && planMacro.ForwardRepairUntilFrame > Unsorted::CurrentFrame
			&& planMacro.ForwardRepairCell != CellStruct::Empty)
		{
			const int homeZoneForRepair = MapClass::Instance.GetMovementZoneType(center, MovementZone::Normal, false);
			double bestAnchorDist = center.DistanceFrom(planMacro.ForwardRepairCell);

			for (auto const pOwnBld : pHouse->Buildings)
			{
				if (!pOwnBld || pOwnBld->InLimbo || !pOwnBld->IsAlive || !pOwnBld->Type
					|| (!pOwnBld->Type->BaseNormal && !pOwnBld->Type->ConstructionYard && !BuildingTypeExt::IsAIBaseNormal(pOwnBld->Type)))
				{
					continue;
				}

				const CellStruct bc = pOwnBld->GetMapCoords();

				if (MapClass::Instance.GetMovementZoneType(bc, MovementZone::Normal, pOwnBld->OnBridge) != homeZoneForRepair)
					continue;

				const double d = bc.DistanceFrom(planMacro.ForwardRepairCell);

				if (d < bestAnchorDist)
				{
					bestAnchorDist = d;
					center = bc;
				}
			}
		}
	}

	// Ally site plans: this building goes next to the ally's base, from the cell the plan chose
	// (its side of that base); expansion zones and old sites of lost buildings do not apply there.
	auto const& allySiteMacro = GetMacroState(pHouse);
	const bool placingAtAllySite = GameModeOptionsClass::Instance.BuildOffAlly
		&& allySiteMacro.AllySiteUntilFrame > Unsorted::CurrentFrame && allySiteMacro.AllySiteBuildType == pBuildingType->ArrayIndex
		&& allySiteMacro.AllySiteCell != CellStruct::Empty && allySiteMacro.AllySiteYardCell != CellStruct::Empty;

	// Ally buildings we keep clear of. Near the site, the ally crawl anchor or an ally defense spot:
	// a free cell from each of its buildings (its lanes and the room it builds into) and 4 from its
	// ConYard.
	std::vector<ArchitectRect> allySiteKeepClear;

	if (placingAtAllySite)
	{
		center = allySiteMacro.AllySiteCell;
		targetExpansionZone = -1;
	}

	// An ally building attacked lately (production, AllyDefenseCell): our armed defense goes there,
	// after every other anchor so none of them moves it home. Once the enemy is no longer in sight
	// there, a hot spot of ours (a building of ours attacked, an active front, a key asset without
	// cover, chosen above) wins; with none the ally spot is still reinforced.
	const bool ownHotSpot = directionalAttackedBld != CellStruct::Empty || undefendedKeyAssetCell != CellStruct::Empty
		|| (allySiteMacro.FrontlineThreatNeedsDefenses > 0 && Unsorted::CurrentFrame <= allySiteMacro.FrontlineThreatActiveFrames);
	const bool defendingAlly = !placingAtAllySite && !holdingIsland && pBuildingType->IsBaseDefense && GroundRangeOf(pBuildingType) > 0
		&& GameModeOptionsClass::Instance.BuildOffAlly && allySiteMacro.AllyDefenseUntilFrame > Unsorted::CurrentFrame
		&& allySiteMacro.AllyDefenseCell != CellStruct::Empty
		&& (!ownHotSpot || VisibleEnemyThreatNear(pHouse, allySiteMacro.AllyDefenseCell, 10) > 0);

	if (defendingAlly)
	{
		center = allySiteMacro.AllyDefenseCell;
		targetExpansionZone = -1;
	}

	// Whatever we place never touches an ally's factory, refinery, repair station or ConYard (their
	// exits and docks stay open): a free cell from each, in both passes.
	for (auto const& fp : GetBuildingFootprints())
	{
		if (!fp.Owner || fp.Owner == pHouse || !pHouse->IsAlliedWith(fp.Owner) || fp.Owner->IsNeutral()
			|| std::max(std::abs(fp.X - center.X), std::abs(fp.Y - center.Y)) > 40)
		{
			continue;
		}

		if (fp.Type->Factory != AbstractType::None || fp.Type->Refinery || fp.Type->ResourceDestination
			|| fp.Type->ConstructionYard || fp.Type->UnitRepair)
		{
			allySiteKeepClear.push_back({ static_cast<short>(fp.X - 1), static_cast<short>(fp.Y - 1),
				static_cast<short>(fp.X + fp.W), static_cast<short>(fp.Y + fp.H) });
		}
	}

	if (placingAtAllySite || crawlFromAllyAnchor || defendingAlly)
	{
		for (auto const pAllyBld : BuildingClass::Array)
		{
			if (!pAllyBld || !pAllyBld->IsAlive || pAllyBld->InLimbo || !pAllyBld->Type || !pAllyBld->Owner
				|| pAllyBld->Owner == pHouse || !pHouse->IsAlliedWith(pAllyBld->Owner)
				|| pAllyBld->GetMapCoords().DistanceFrom(center) > 40.0)
			{
				continue;
			}

			const CellStruct bc = pAllyBld->GetMapCoords();
			const short margin = pAllyBld->Type->ConstructionYard ? 4 : 1;
			allySiteKeepClear.push_back({ static_cast<short>(bc.X - margin), static_cast<short>(bc.Y - margin),
				static_cast<short>(bc.X + pAllyBld->Type->GetFoundationWidth() - 1 + margin),
				static_cast<short>(bc.Y + pAllyBld->Type->GetFoundationHeight(false) - 1 + margin) });
		}
	}

	const bool hasPendingClearanceForOther =

		(GetMacroState(pHouse).PendingClearanceType != nullptr
			&& GetMacroState(pHouse).PendingClearanceType != pBuildingType
			&& GetMacroState(pHouse).PendingClearanceCell != CellStruct::Empty);

	// Collect all valid BaseNormal / ConstructionYard adjacency anchor structures
	// (from AdvAI)
	std::vector<BuildingClass*> adjacencyAnchors;

	if (!pBuildingType->ConstructionYard)
	{
		for (auto const pOwnBld : pHouse->Buildings)
		{
			if (!pOwnBld || pOwnBld->InLimbo || !pOwnBld->IsAlive || !pOwnBld->Type || pOwnBld->Type->InvisibleInGame)
				continue;

			if (pOwnBld->CurrentMission == Mission::Selling || pOwnBld->QueuedMission == Mission::Selling)
				continue;

			if (pOwnBld->Type->BaseNormal || pOwnBld->Type->ConstructionYard
				|| BuildingTypeExt::IsAIBaseNormal(pOwnBld->Type) || (isNavalBld && pOwnBld->Type->Naval))
			{
				adjacencyAnchors.push_back(pOwnBld);
			}
		}

		// A refinery leans on ally buildings only when the crawl to its ore starts from one.
		if (GameModeOptionsClass::Instance.BuildOffAlly && (!isRefineryOrSlaveMiner || crawlFromAllyAnchor))
		{
			for (auto const pAllyBld : BuildingClass::Array)
			{
				if (!pAllyBld || pAllyBld->InLimbo || !pAllyBld->IsAlive || !pAllyBld->Owner || !pAllyBld->Type
					|| pAllyBld->Type->InvisibleInGame)
				{
					continue;
				}

				if (pAllyBld->Owner != pHouse && pHouse->IsAlliedWith(pAllyBld->Owner) && !pAllyBld->Owner->IsNeutral()
					&& pAllyBld->Type->EligibileForAllyBuilding && pAllyBld->Type->BaseNormal)
				{
					adjacencyAnchors.push_back(pAllyBld);
				}
			}
		}

		if (adjacencyAnchors.empty())
			return CellStruct::Empty;
	}

	// Helper that enforces real C&C / YR building adjacency
	// (pBuildingType->Adjacent / AINavalYardAdjacency)
	// against our BaseNormal anchor structures using both fast bounding box and
	// exact GetFoundationData(true) cells
	auto passesBaseAdjacency = [&](const CellStruct& cell) -> bool
	{
		if (pBuildingType->ConstructionYard)
			return true;

		for (auto const pAnchor : adjacencyAnchors)
		{
			int adjacency = std::max(1, pBuildingType->Adjacent);

			if (isNavalBld && pAnchor->Type->ConstructionYard && RulesClass::Instance)
				adjacency = std::max(adjacency, RulesClass::Instance->AINavalYardAdjacency);

			const int otherW = pAnchor->Type->GetFoundationWidth();
			const int otherH = pAnchor->Type->GetFoundationHeight(false);
			const CellStruct origin = pAnchor->GetMapCoords();

			const int gapX = (cell.X > origin.X + otherW - 1)
				? (cell.X - (origin.X + otherW - 1))
				: ((cell.X + w - 1 < origin.X) ? (origin.X - (cell.X + w - 1)) : 0);

			if (gapX > adjacency)
				continue;

			const int gapY = (cell.Y > origin.Y + otherH - 1)
				? (cell.Y - (origin.Y + otherH - 1))
				: ((cell.Y + h - 1 < origin.Y) ? (origin.Y - (cell.Y + h - 1)) : 0);

			if (gapY > adjacency)
				continue;

			CellStruct const* occupy = pAnchor->GetFoundationData(true);

			while (occupy && (occupy->X != 0x7FFF || occupy->Y != 0x7FFF))
			{
				const CellStruct sum = origin + *occupy;
				CellStruct const* newOccupy = pBuildingType->GetFoundationData(true);

				while (newOccupy && (newOccupy->X != 0x7FFF || newOccupy->Y != 0x7FFF))
				{
					const CellStruct newSum = cell + *newOccupy;

					if (std::abs(newSum.X - sum.X) <= adjacency && std::abs(newSum.Y - sum.Y) <= adjacency)
						return true;

					++newOccupy;
				}

				++occupy;
			}
		}

		return false;
	};

	CellStruct bestCandidate = CellStruct::Empty;
	int bestSpatialScore = -999999;

	// Defense spots that do not pay (DefenseSpotShift): inside the reach of a known enemy tower
	// that outranges this one, or where one of our towers died without a kill. The tower's ground
	// reach also scores how much of a hot enemy route it covers (route heat).
	std::vector<std::pair<CellStruct, int>> enemyDefenseReach;
	const int ownTowerRangeCells = isCombatDefense ? GroundRangeOf(pBuildingType) / Unsorted::LeptonsPerCell : 0;

	if (isCombatDefense)
		CollectEnemyDefenseReach(pHouse, enemyDefenseReach);

	// Base spread against superweapons (point 43): while an enemy owns a superweapon with a
	// public countdown (ShowTimer=yes, visible to every player), our expensive non-defense
	// structures keep at least 6 cells apart so one strike cannot take several of them.
	std::vector<CellStruct> swSpreadAnchors;

	if (!pBuildingType->IsBaseDefense && pBuildingType->GetActualCost(pHouse) >= 1000)
	{
		bool enemyHasTimedSuper = false;

		for (auto const pEnemy : HouseClass::Array)
		{
			if (!pEnemy || pEnemy == pHouse || pHouse->IsAlliedWith(pEnemy) || pEnemy->IsNeutral() || pEnemy->Defeated)
				continue;

			for (auto const pSuper : pEnemy->Supers)
			{
				if (pSuper && pSuper->Type && pSuper->IsPresent && pSuper->Type->ShowTimer)
				{
					enemyHasTimedSuper = true;
					break;
				}
			}

			if (enemyHasTimedSuper)
				break;
		}

		if (enemyHasTimedSuper)
		{
			for (auto const pOwnB : pHouse->Buildings)
			{
				if (pOwnB && pOwnB->IsAlive && !pOwnB->InLimbo && pOwnB->Type && !pOwnB->Type->IsBaseDefense && pOwnB->Type->Cost >= 1000)
					swSpreadAnchors.push_back(pOwnB->GetMapCoords());
			}
		}
	}

	// Expert 1 (Spatial Base Architect): while it leads, the base is laid out like a careful
	// player's. Everything below is gated on it, so other experts place exactly as before.
	// - factory exit lanes stay free and new buildings line up on 1-cell streets;
	// - power plants group behind the core, away from the threat side (stones only crawl to free ore);
	// - defenses sit on the threat side of the refineries / ConYard / factories they cover,
	//   spaced wider than the splash of the enemy weapons this house has seen;
	// - with a known enemy superweapon, high-value buildings keep its strike radius apart;
	// - a lost key building is rebuilt where it stood while that spot is safe.
	// Refineries and silos stand where the ore is: never pushed apart for a superweapon.
	const bool isHighValueBld = !isCombatDefense && !isNavalBld && !isTunnelBld && !isResourceBld
		&& (pBuildingType->ConstructionYard || isFactoryOrBarracks || pBuildingType->Radar || pBuildingType->HasSuperWeapon()
			|| pBuildingType->GetActualCost(pHouse) >= 1000);
	std::vector<ArchitectRect> architectExitLanes;
	std::vector<CellStruct> architectPowerCells;
	std::vector<CellStruct> architectCoverAssets;
	std::vector<CellStruct> architectHighValueCells;
	CellStruct architectThreatCell = CellStruct::Empty;
	CellStruct architectRebuildCell = CellStruct::Empty;
	int architectSplashSpacing = 3;
	int architectSwRadius = 0;

	auto footprintCenter = [](const CellStruct& topLeft, const BuildingTypeClass* pType)
	{
		return CellStruct { static_cast<short>(topLeft.X + pType->GetFoundationWidth() / 2),
			static_cast<short>(topLeft.Y + pType->GetFoundationHeight(false) / 2) };
	};

	if (architectActive)
	{
		// Threat side: the side we have really been attacked from (attack route memory), else
		// the known enemy buildings (or the next unchecked start); an active attack on a defended
		// building wins for defenses. All fog-fair.
		architectThreatCell = enemyTargetCell;
		const int architectSector = GetMainAttackSector(pHouse);

		if (architectSector >= 0 && center.DistanceFrom(GetHouseHomeCell(pHouse)) <= 24.0)
		{
			architectThreatCell = CellStruct { static_cast<short>(center.X + AttackSectorDX[architectSector] * 14),
				static_cast<short>(center.Y + AttackSectorDY[architectSector] * 14) };
		}

		if (isCombatDefense && directionalAttacker != CellStruct::Empty)
			architectThreatCell = directionalAttacker;

		for (auto const pOwnB : pHouse->Buildings)
		{
			if (!pOwnB || pOwnB->InLimbo || !pOwnB->IsAlive || !pOwnB->Type || pOwnB->Type->InvisibleInGame)
				continue;

			auto const pOT = pOwnB->Type;
			const CellStruct bc = pOwnB->GetMapCoords();
			ArchitectRect lane {};

			if (GetFactoryExitLane(pOT, bc, lane))
				architectExitLanes.push_back(lane);

			if (pOT->IsBaseDefense)
				continue;

			const CellStruct mid = footprintCenter(bc, pOT);

			if (pOT->PowerBonus > pOT->PowerDrain
				&& (centerZone < 0 || MapClass::Instance.GetMovementZoneType(bc, MovementZone::Normal, pOwnB->OnBridge) == centerZone))
			{
				architectPowerCells.push_back(mid);
			}

			if (pOT->Refinery || pOT->ResourceDestination || pOT->ConstructionYard
				|| ((pOT->Factory == AbstractType::UnitType || pOT->Factory == AbstractType::InfantryType) && !pOT->Naval))
			{
				architectCoverAssets.push_back(mid);
			}

			if (isHighValueBld && !pOT->Naval && !pOT->Refinery && !pOT->ResourceDestination && !pOT->ResourceGatherer
				&& pOT->Storage <= 0 && !pOT->OrePurifier
				&& (pOT->ConstructionYard || pOT->Factory != AbstractType::None || pOT->WeaponsFactory || pOT->Radar
					|| pOT->HasSuperWeapon() || pOT->GetActualCost(pHouse) >= 1000))
			{
				architectHighValueCells.push_back(mid);
			}
		}

		// Splash of the enemy weapons this house has seen (remembered sightings and known
		// defenses): two of our defenses closer than twice that radius die to one shot.
		if (isCombatDefense)
		{
			double maxSpread = 0.0;

			auto addWeapon = [&maxSpread](const WeaponTypeClass* pW)
			{
				if (pW && pW->Warhead && pW->Damage > 0 && (!pW->Projectile || pW->Projectile->AG))
					maxSpread = std::max(maxSpread, static_cast<double>(pW->Warhead->CellSpread));
			};

			for (auto const& [id, sighting] : macroState.EnemySightings)
			{
				if (sighting.Type)
				{
					addWeapon(sighting.Type->GetWeapon(0, false).WeaponType);
					addWeapon(sighting.Type->GetWeapon(1, false).WeaponType);
				}
			}

			for (auto const pEB : BuildingClass::Array)
			{
				if (pEB && pEB->IsAlive && !pEB->InLimbo && pEB->Owner && pEB->Type && pEB->Type->IsBaseDefense
					&& pEB->Owner != pHouse && !pHouse->IsAlliedWith(pEB->Owner) && IsEnemyBuildingKnown(pHouse, pEB))
				{
					if (auto const pWS = pEB->GetWeapon(0))
						addWeapon(pWS->WeaponType);
				}
			}

			architectSplashSpacing = std::clamp(2 * static_cast<int>(std::ceil(maxSpread)) + 1, 3, 7);
		}

		// Strike radius of the enemy superweapons this house knows of: a superweapon building it
		// has seen, or one with a public countdown (ShowTimer, shown to every player).
		if (isHighValueBld)
		{
			auto addSuper = [&architectSwRadius](int swIndex)
			{
				if (swIndex >= 0 && swIndex < SuperWeaponTypeClass::Array.Count)
					architectSwRadius = std::max(architectSwRadius, GetSuperWeaponStrikeRadius(SuperWeaponTypeClass::Array.GetItem(swIndex)));
			};

			for (auto const pEB : BuildingClass::Array)
			{
				if (pEB && pEB->IsAlive && !pEB->InLimbo && pEB->Owner && pEB->Type && pEB->Type->HasSuperWeapon()
					&& pEB->Owner != pHouse && !pHouse->IsAlliedWith(pEB->Owner) && !pEB->Owner->IsNeutral()
					&& IsEnemyBuildingKnown(pHouse, pEB))
				{
					addSuper(pEB->Type->SuperWeapon);
					addSuper(pEB->Type->SuperWeapon2);
				}
			}

			for (auto const pEnemy : HouseClass::Array)
			{
				if (!pEnemy || pEnemy == pHouse || pHouse->IsAlliedWith(pEnemy) || pEnemy->IsNeutral() || pEnemy->Defeated)
					continue;

				for (auto const pSuper : pEnemy->Supers)
				{
					if (pSuper && pSuper->Type && pSuper->IsPresent && pSuper->Type->ShowTimer)
						architectSwRadius = std::max(architectSwRadius, GetSuperWeaponStrikeRadius(pSuper->Type));
				}
			}

			// Wider than 12 cells would push buildings out of any reachable spot around the base.
			architectSwRadius = architectSwRadius > 0 ? std::clamp(architectSwRadius, 3, 12) : 0;
		}

		// Rebuild where it stood: the most recent lost site of this type (or the same role and
		// foundation), when no known enemy structure reaches it and no visible armed enemy stands
		// within 10 cells.
		if (!pBuildingType->ConstructionYard)
		{
			auto sameRole = [pBuildingType](const BuildingTypeClass* pT)
			{
				return pT == pBuildingType
					|| (pT->GetFoundationWidth() == pBuildingType->GetFoundationWidth()
						&& pT->GetFoundationHeight(false) == pBuildingType->GetFoundationHeight(false)
						&& pT->Factory == pBuildingType->Factory && pT->WeaponsFactory == pBuildingType->WeaponsFactory
						&& pT->Naval == pBuildingType->Naval && pT->Refinery == pBuildingType->Refinery
						&& (pT->PowerBonus > pT->PowerDrain) == (pBuildingType->PowerBonus > pBuildingType->PowerDrain)
						&& pT->Radar == pBuildingType->Radar && pT->HasSuperWeapon() == pBuildingType->HasSuperWeapon());
			};

			auto isSiteSafe = [pHouse](const CellStruct& site)
			{
				for (auto const pEB : BuildingClass::Array)
				{
					if (!pEB || !pEB->IsAlive || pEB->InLimbo || !pEB->Owner || !pEB->Type || pEB->Owner == pHouse
						|| pHouse->IsAlliedWith(pEB->Owner) || pEB->Owner->IsNeutral() || !IsEnemyBuildingKnown(pHouse, pEB))
					{
						continue;
					}

					int reach = 10;

					if (auto const pWS = pEB->GetWeapon(0); pWS && pWS->WeaponType)
						reach = std::max(reach, pWS->WeaponType->Range / Unsorted::LeptonsPerCell + 2);

					const CellStruct ec = pEB->GetMapCoords();

					if (std::max(std::abs(site.X - ec.X), std::abs(site.Y - ec.Y)) <= reach)
						return false;
				}

				for (auto const pTechno : TechnoClass::Array)
				{
					if (!pTechno || !pTechno->IsAlive || pTechno->InLimbo || !pTechno->Owner || pTechno->Owner == pHouse
						|| pTechno->WhatAmI() == AbstractType::Building || pHouse->IsAlliedWith(pTechno->Owner)
						|| pTechno->Owner->IsNeutral())
					{
						continue;
					}

					const CellStruct tc = pTechno->GetMapCoords();

					if (std::max(std::abs(site.X - tc.X), std::abs(site.Y - tc.Y)) > 10 || !IsCellVisibleToAI(pHouse, pTechno->GetCell()))
						continue;

					if (auto const pWS = pTechno->GetWeapon(0); pWS && pWS->WeaponType && pWS->WeaponType->Damage > 0)
						return false;
				}

				return true;
			};

			for (auto it = architectMemory.Lost.rbegin(); it != architectMemory.Lost.rend(); ++it)
			{
				if (it->TypeIndex < 0 || it->TypeIndex >= BuildingTypeClass::Array.Count)
					continue;

				auto const pLostType = BuildingTypeClass::Array.GetItem(it->TypeIndex);

				if (pLostType && sameRole(pLostType) && isSiteSafe(it->Cell))
				{
					architectRebuildCell = it->Cell;
					break;
				}
			}
		}
	}

	int validCandidatesChecked = 0;

	std::vector<CellStruct> candidateList;
	candidateList.reserve(1024);

	// Gather exact Adjacent= ring around EVERY BaseNormal / ConstructionYard
	// anchor (from AdvAI) so we never miss a valid placement slot even when
	// ConYard[0] is surrounded or an expansion is far away!
	if (!pBuildingType->ConstructionYard && !isNavalBld && !adjacencyAnchors.empty())
	{
		const int adjRange = std::max(1, pBuildingType->Adjacent);
		for (auto const pAnchor : adjacencyAnchors)
		{
			const CellStruct ac = pAnchor->GetMapCoords();
			const int aW = pAnchor->Type->GetFoundationWidth();
			const int aH = pAnchor->Type->GetFoundationHeight(false);
			const int xMin = ac.X - adjRange - w + 1;
			const int xMax = ac.X + aW + adjRange - 1;
			const int yMin = ac.Y - adjRange - h + 1;
			const int yMax = ac.Y + aH + adjRange - 1;
			for (int cy = yMin; cy <= yMax; ++cy)
			{
				for (int cx = xMin; cx <= xMax; ++cx)
				{
					// Skip cells directly inside the anchor's own footprint
					if (cx + w > ac.X && cx < ac.X + aW && cy + h > ac.Y && cy < ac.Y + aH)
						continue;

					candidateList.push_back(CellStruct { static_cast<short>(cx), static_cast<short>(cy) });
				}
			}
		}
	}
	else if (isNavalBld)
	{
		// AdvAI Naval Yard candidate scan: scan INNER-TO-OUTER (r = 1 .. maxRadius)
		// around coastal BaseNormal/Naval anchors first and around every ConYard,
		// so shoreline water cells next to the allied base are always evaluated
		// first!
		const int baseAdjRange = std::max(1, pBuildingType->Adjacent);
		for (auto const pAnchor : adjacencyAnchors)
		{
			if (pAnchor->Type->ConstructionYard)
				continue;

			const CellStruct ac = pAnchor->GetMapCoords();
			const int anchorScanRange = baseAdjRange + w + 3;
			for (int r = 1; r <= anchorScanRange; ++r)
			{
				for (int dy = -r; dy <= r; ++dy)
				{
					for (int dx = -r; dx <= r; ++dx)
					{
						if (std::max(std::abs(dx), std::abs(dy)) != r)
							continue;

						const CellStruct cand { static_cast<short>(ac.X + dx), static_cast<short>(ac.Y + dy) };
						if (auto const pC = MapClass::Instance.TryGetCellAt(cand))
						{
							if (pC->LandType == LandType::Water)
								candidateList.push_back(cand);
						}
					}
				}
			}
		}

		const int navalMaxAdj = std::max(std::max(1, pBuildingType->Adjacent),
			RulesClass::Instance ? RulesClass::Instance->AINavalYardAdjacency : 36);
		const int maxRadius = std::clamp(navalMaxAdj + w + 2, 24, 48);
		for (int r = 1; r <= maxRadius; ++r)
		{
			for (int dy = -r; dy <= r; ++dy)
			{
				for (int dx = -r; dx <= r; ++dx)
				{
					if (std::max(std::abs(dx), std::abs(dy)) != r)
						continue;

					const CellStruct cand { static_cast<short>(center.X + dx), static_cast<short>(center.Y + dy) };
					if (auto const pC = MapClass::Instance.TryGetCellAt(cand))
					{
						if (pC->LandType == LandType::Water)
							candidateList.push_back(cand);
					}
				}
			}
		}

		for (auto const pAnchor : adjacencyAnchors)
		{
			if (!pAnchor->Type->ConstructionYard)
				continue;

			const CellStruct ac = pAnchor->GetMapCoords();
			if (std::max(std::abs(ac.X - center.X), std::abs(ac.Y - center.Y)) <= 4)
				continue;

			for (int r = 1; r <= maxRadius; ++r)
			{
				for (int dy = -r; dy <= r; ++dy)
				{
					for (int dx = -r; dx <= r; ++dx)
					{
						if (std::max(std::abs(dx), std::abs(dy)) != r)
							continue;

						const CellStruct cand { static_cast<short>(ac.X + dx), static_cast<short>(ac.Y + dy) };
						if (auto const pC = MapClass::Instance.TryGetCellAt(cand))
						{
							if (pC->LandType == LandType::Water)
								candidateList.push_back(cand);
						}
					}
				}
			}
		}
	}
	else
	{
		const int startRadius = 1;
		const int maxRadius = 28;
		for (int r = startRadius; r <= maxRadius; ++r)
		{
			for (int dy = -r; dy <= r; ++dy)
			{
				for (int dx = -r; dx <= r; ++dx)
				{
					if (std::max(std::abs(dx), std::abs(dy)) != r)
						continue;

					candidateList.push_back(
						CellStruct { static_cast<short>(center.X + dx), static_cast<short>(center.Y + dy) });
				}
			}
		}
	}

	// Evaluate the anchor rings nearest the chosen anchor first, each cell once, so the
	// 900-candidate budget is spent around the anchor (the crawl front) rather than on the
	// oldest buildings of a big base.
	if (!pBuildingType->ConstructionYard && !isNavalBld && candidateList.size() > 1)
	{
		auto ringDistance = [&center](const CellStruct& c)
		{
			return std::max(std::abs(c.X - center.X), std::abs(c.Y - center.Y));
		};

		std::sort(candidateList.begin(), candidateList.end(), [&ringDistance](const CellStruct& a, const CellStruct& b)
		{
			const int da = ringDistance(a);
			const int db = ringDistance(b);

			if (da != db)
				return da < db;

			return a.Y != b.Y ? a.Y < b.Y : a.X < b.X;
		});

		candidateList.erase(std::unique(candidateList.begin(), candidateList.end()), candidateList.end());
	}

	// Expert 1: the old site of a lost key building is always evaluated, even past the budget.
	if (architectRebuildCell != CellStruct::Empty && !placingAtAllySite && !defendingAlly)
		candidateList.insert(candidateList.begin(), architectRebuildCell);

	// Ally site: the cell found before production is tried first and kept while still valid.
	const bool hasAllySiteReservation = placingAtAllySite && allySiteMacro.AllySiteReservedType == pBuildingType->ArrayIndex
		&& allySiteMacro.AllySiteReservedCell != CellStruct::Empty;

	if (hasAllySiteReservation)
		candidateList.insert(candidateList.begin(), allySiteMacro.AllySiteReservedCell);

	const int maxCandidatesToEvaluate = isNavalBld ? 2400 : 900;

	// Crawl lane from the anchor (center) toward a goal, integer geometry (MP safe): 0 = ahead of
	// the anchor within ~2.5 cells of the axis (kept free for the next stone), 1 = flank 3..7
	// cells off the axis, 2 = elsewhere.
	auto laneSide = [&center](const CellStruct& cell, const CellStruct& goal) -> int
	{
		const int ax = goal.X - center.X;
		const int ay = goal.Y - center.Y;
		const long long len2 = static_cast<long long>(ax) * ax + static_cast<long long>(ay) * ay;

		if (len2 < 16)
			return 2;

		const int cx = cell.X - center.X;
		const int cy = cell.Y - center.Y;
		const long long dot = static_cast<long long>(cx) * ax + static_cast<long long>(cy) * ay;
		const long long cross = static_cast<long long>(cx) * ay - static_cast<long long>(cy) * ax;
		const long long cross2 = cross * cross;

		if (dot > 0 && 4 * cross2 < 25 * len2)
			return 0;

		return (cross2 >= 9 * len2 && cross2 <= 49 * len2) ? 1 : 2;
	};

	// A stone toward the enemy: remember its cell (UpdateOffensiveStones confirms the placement).
	auto rememberOffensiveStone = [&](const CellStruct& chosen) -> CellStruct
	{
		if (stoneTowardEnemy && chosen != CellStruct::Empty)
		{
			auto& pending = GetMacroState(pHouse).OffensiveStoneCells;

			if (std::none_of(pending.begin(), pending.end(), [&chosen](auto const& entry) { return entry.first == chosen; }))
			{
				if (pending.size() >= 8)
					pending.erase(pending.begin());

				pending.emplace_back(chosen, Unsorted::CurrentFrame);
			}
		}

		if (architectRebuildCell != CellStruct::Empty && chosen == architectRebuildCell)
		{
			Debug::Log("[NeuralAI] House %d Architect: %s goes back to its lost site (%d,%d).\n",
				pHouse->ArrayIndex, pBuildingType->ID, chosen.X, chosen.Y);
		}

		return chosen;
	};
	std::vector<int> strategicWaterZones;
	if (isNavalBld)
	{
		for (auto const pEB : BuildingClass::Array)
		{
			if (!pEB || pEB->InLimbo || !pEB->IsAlive || !pEB->Owner || !pEB->Type || !pEB->Type->Naval
				|| pEB->Owner == pHouse || pHouse->IsAlliedWith(pEB->Owner) || !IsEnemyBuildingKnown(pHouse, pEB) || pEB->Owner->IsNeutral())
			{
				continue;
			}

			const int ewz = ObjectZone(pEB, MovementZone::Water);
			if (ewz >= 0
				&& std::find(strategicWaterZones.begin(), strategicWaterZones.end(), ewz) == strategicWaterZones.end())
			{
				strategicWaterZones.push_back(ewz);
			}
		}

		for (auto const pEU : UnitClass::Array)
		{
			if (!pEU || pEU->InLimbo || !pEU->IsAlive || !pEU->Owner || !pEU->Type || !pEU->Type->Naval
				|| pEU->Owner == pHouse || pHouse->IsAlliedWith(pEU->Owner) || !IsCellVisibleToAI(pHouse, pEU->GetCell()) || pEU->Owner->IsNeutral())
			{
				continue;
			}

			const int uwz = ObjectZone(pEU, MovementZone::Water);
			if (uwz >= 0
				&& std::find(strategicWaterZones.begin(), strategicWaterZones.end(), uwz) == strategicWaterZones.end())
			{
				strategicWaterZones.push_back(uwz);
			}
		}
	}
	// Two-pass evaluation: Pass 0 strictly requires the 1-cell street margin
	// (IsCellClearedOfExitCorridors); Pass 1 only runs if Pass 0 found 0 valid
	// cells (e.g. tight cliff/island corner), allowing 0-cell touching with a
	// heavy score penalty (like AdvAI).
	for (int relaxMarginPass = 0; relaxMarginPass < 2 && bestCandidate == CellStruct::Empty; ++relaxMarginPass)
	{
		{
			for (const CellStruct& candidate : candidateList)
			{
				const int radius
					= std::max(1, std::max(std::abs(candidate.X - center.X), std::abs(candidate.Y - center.Y)));

				if (!MapClass::Instance.CoordinatesLegal(candidate))
					continue;

				// Island Hold: only cells the plan counts for its yard.
				if (holdingIsland && (candidate.DistanceFrom(islandHoldYardCell) > IslandHoldDefenseRadius
					|| MapClass::Instance.GetMovementZoneType(candidate, MovementZone::Normal, false) != islandHoldZone))
				{
					continue;
				}

				// Naval structures only on useful water (no enclosed ponds).
				if (isNavalBld && !IsUsefulWaterCell(candidate))
					continue;

				// Never across a channel: both opposite sides of the footprint closed (less than half
				// open water in the 2-cell strip beside it) means it would cut the waterway (match
				// #7061: a naval yard built across a 4-cell channel held 15 ships for minutes). A yard
				// on a coast has land on one side only.
				if (isNavalBld)
				{
					auto const sideOpen = [](int x0, int y0, int x1, int y1)
					{
						int water = 0;
						int total = 0;

						for (int y = y0; y <= y1; ++y)
						{
							for (int x = x0; x <= x1; ++x)
							{
								const CellStruct c { static_cast<short>(x), static_cast<short>(y) };
								auto const pCell = MapClass::Instance.CoordinatesLegal(c) ? MapClass::Instance.TryGetCellAt(c) : nullptr;
								++total;

								if (pCell && pCell->LandType == LandType::Water && !pCell->GetBuilding())
									++water;
							}
						}

						return water * 2 >= total;
					};

					const int x0 = candidate.X;
					const int y0 = candidate.Y;
					const int x1 = candidate.X + w - 1;
					const int y1 = candidate.Y + h - 1;
					const bool westOpen = sideOpen(x0 - 2, y0, x0 - 1, y1);
					const bool eastOpen = sideOpen(x1 + 1, y0, x1 + 2, y1);
					const bool northOpen = sideOpen(x0, y0 - 2, x1, y0 - 1);
					const bool southOpen = sideOpen(x0, y1 + 1, x1, y1 + 2);

					if ((!westOpen && !eastOpen) || (!northOpen && !southOpen))
						continue;
				}

				// Strictly enforce BaseNormal Adjacency (Adjacent= /
				// AINavalYardAdjacency)
				if (!passesBaseAdjacency(candidate))
					continue;

				// Do not place on a cell reserved by PendingClearanceCell for another
				// building
				if (hasPendingClearanceForOther)
				{
					const int rX = GetMacroState(pHouse).PendingClearanceCell.X;
					const int rY = GetMacroState(pHouse).PendingClearanceCell.Y;
					const int rW = GetMacroState(pHouse).PendingClearanceFoundationWidth;
					const int rH = GetMacroState(pHouse).PendingClearanceFoundationHeight;

					if ((candidate.X < rX + rW) && (candidate.X + w > rX) && (candidate.Y < rY + rH)
						&& (candidate.Y + h > rY))
					{
						continue;
					}
				}

				// Never crush or block Ore/Tiberium Spawner Trees or place on Bridges
				if (OverlapsTiberiumTreeZone(candidate, pBuildingType) || OverlapsBridge(candidate, pBuildingType))
					continue;

				bool foundationValid = true;

				for (int fy = 0; fy < h && foundationValid; ++fy)
				{
					for (int fx = 0; fx < w; ++fx)
					{
						const CellStruct fc { static_cast<short>(candidate.X + fx),
							static_cast<short>(candidate.Y + fy) };

						auto const pCell = MapClass::Instance.TryGetCellAt(fc);

						if (!pCell || !pCell->CanThisExistHere(pBuildingType->SpeedType, pBuildingType, pHouse))
						{
							foundationValid = false;

							break;
						}
					}
				}

				if (!foundationValid)
					continue;

				// Ally site: within 20 cells of the ally's ConYard. Never inside an ally keep-clear
				// rectangle (allySiteKeepClear), in both passes: a cramped ally base is not squeezed
				// further.
				if (placingAtAllySite && candidate.DistanceFrom(allySiteMacro.AllySiteYardCell) > 20.0)
					continue;

				if (!allySiteKeepClear.empty())
				{
					bool touchesAlly = false;

					for (auto const& rect : allySiteKeepClear)
					{
						if (rect.Overlaps(candidate.X, candidate.Y, w, h))
						{
							touchesAlly = true;
							break;
						}
					}

					if (touchesAlly)
						continue;
				}

				// Expert 1: never stand in a factory's exit lane, and a new factory needs its own lane
				// open (its door cell free, most of the lane walkable). The strict pass skips such
				// cells, the relaxed pass only penalizes them.
				int architectLanePenalty = 0;

				if (architectActive && !isNavalBld)
				{
					for (auto const& lane : architectExitLanes)
					{
						if (lane.Overlaps(candidate.X, candidate.Y, w, h))
							architectLanePenalty += 2500;
					}

					ArchitectRect ownLane {};

					if (GetFactoryExitLane(pBuildingType, candidate, ownLane))
					{
						const bool laneEast = ownLane.X0 == candidate.X + w;
						const bool laneWest = ownLane.X1 == candidate.X - 1;
						const bool laneNorth = !laneEast && !laneWest && ownLane.Y1 == candidate.Y - 1;
						const short midX = static_cast<short>((ownLane.X0 + ownLane.X1) / 2);
						const short midY = static_cast<short>((ownLane.Y0 + ownLane.Y1) / 2);
						const CellStruct door = laneEast ? CellStruct { ownLane.X0, midY }
							: laneWest ? CellStruct { ownLane.X1, midY }
							: laneNorth ? CellStruct { midX, ownLane.Y1 }
							: CellStruct { midX, ownLane.Y0 };
						int blocked = 0;
						bool doorBlocked = false;

						for (short ly = ownLane.Y0; ly <= ownLane.Y1; ++ly)
						{
							for (short lx = ownLane.X0; lx <= ownLane.X1; ++lx)
							{
								const CellStruct lc { lx, ly };
								auto const pLaneCell = MapClass::Instance.CoordinatesLegal(lc) ? MapClass::Instance.TryGetCellAt(lc) : nullptr;
								const bool isBlocked = !pLaneCell || pLaneCell->GetBuilding() != nullptr
									|| pLaneCell->LandType == LandType::Water || pLaneCell->LandType == LandType::Rock;

								if (isBlocked)
								{
									blocked++;

									if (lc == door)
										doorBlocked = true;
								}
							}
						}

						architectLanePenalty += blocked * 400 + ((doorBlocked || blocked >= 4) ? 2500 : 0);
					}

					if (architectLanePenalty >= 2500 && relaxMarginPass == 0)
						continue;
				}

				const bool hasOneCellMargin = NeuralAI::IsCellClearedOfExitCorridors(pHouse, pBuildingType, candidate);
				if (!hasOneCellMargin)
				{
					if (relaxMarginPass == 0)
						continue;

					// Even in relaxed pass, never overlap an existing building footprint
					// or clump turrets < 4 cells
					bool overlapsFootprint = false;
					for (auto const pEx : BuildingClass::Array)
					{
						if (!pEx || pEx->InLimbo || !pEx->IsAlive || !pEx->Type || pEx->Type->InvisibleInGame)
							continue;

						const CellStruct exC = pEx->GetMapCoords();
						const int exW = pEx->Type->GetFoundationWidth();
						const int exH = pEx->Type->GetFoundationHeight(false);
						if ((candidate.X < exC.X + exW) && (candidate.X + w > exC.X) && (candidate.Y < exC.Y + exH)
							&& (candidate.Y + h > exC.Y))
						{
							overlapsFootprint = true;
							break;
						}

						if (isCombatDefense && pEx->Type->IsBaseDefense
							&& (std::abs(candidate.X - exC.X) + std::abs(candidate.Y - exC.Y)) < 2)
						{
							overlapsFootprint = true;
							break;
						}
					}

					if (overlapsFootprint)
						continue;
				}

				// When placing/deploying a Construction Yard, never pick a cell within
				// 14 cells of an enemy building or combat unit!
				if (pBuildingType->ConstructionYard)
				{
					bool enemyThreatNearCandidate = false;

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

						if (std::max(std::abs(candidate.X - ec.X), std::abs(candidate.Y - ec.Y)) <= 14)
						{
							enemyThreatNearCandidate = true;

							break;
						}
					}

					if (!enemyThreatNearCandidate)
					{
						for (auto const pEU : UnitClass::Array)
						{
							if (!pEU || pEU->InLimbo || !pEU->IsAlive || !pEU->Owner || !pEU->Type
								|| pEU->Type->Harvester || pEU->Type->ResourceGatherer
								|| IsAmphibiousOrAirTransport(pEU->Type))
							{
								continue;
							}

							if (pEU->Owner == pHouse || pHouse->IsAlliedWith(pEU->Owner) || !IsCellVisibleToAI(pHouse, pEU->GetCell()) || pEU->Owner->IsNeutral())
								continue;

							auto const pEWpn = pEU->GetWeapon(0) ? pEU->GetWeapon(0)->WeaponType : nullptr;
							if (!pEWpn
								|| (pEU->Type->Naval
									&& (pEWpn->Range <= 2048 || (pEWpn->Projectile && !pEWpn->Projectile->AG))))
							{
								continue;
							}

							const CellStruct euc = pEU->GetMapCoords();
							if (std::max(std::abs(candidate.X - euc.X), std::abs(candidate.Y - euc.Y)) <= 12)
							{
								enemyThreatNearCandidate = true;
								break;
							}
						}
					}

					if (enemyThreatNearCandidate && relaxMarginPass == 0)
						continue;
				}

				const int candZone = MapClass::Instance.GetMovementZoneType(candidate, MovementZone::Normal, false);
				const int expansionZoneBonus
					= (!isNavalBld && targetExpansionZone >= 0 && candZone == targetExpansionZone) ? 650 : 0;
				int spatialScore = (enableBaseCrawl ? (-radius * 5) : (-radius * 10)) - (hasOneCellMargin ? 0 : 4000)
					+ expansionZoneBonus;

				// With no cell left but tight ones, one that closes a passage is the last to be taken.
				if (!hasOneCellMargin && ClosesLandPassage(pBuildingType, candidate))
					spatialScore -= 6000;

				if (hasAllySiteReservation && candidate == allySiteMacro.AllySiteReservedCell)
					spatialScore += 100000;

				if (!isCombatDefense)
				{
					int oreUnderFoundation = 0;

					for (int fy = 0; fy < h; ++fy)
					{
						for (int fx = 0; fx < w; ++fx)
						{
							auto const pFC = MapClass::Instance.TryGetCellAt(
								CellStruct { static_cast<short>(candidate.X + fx), static_cast<short>(candidate.Y + fy) });

							if (pFC && pFC->GetContainedTiberiumValue() > 0)
								oreUnderFoundation++;
						}
					}

					spatialScore -= oreUnderFoundation * 250;
				}

				// AdvAI UnsafePlacementZones Graduated Soft-Avoidance (Never a hard block!):
				// Apply a distance-graduated penalty within 6 cells of a recently destroyed/critically
				// damaged building so the AI prefers building on the safer opposite side of its base,
				// while NEVER self-blocking if the whole buildable area is small or under attack!
				if (!isCombatDefense && !isNavalBld && relaxMarginPass == 0 && !macroState.UnsafePlacementZones.empty())
				{
					for (const auto& uz : macroState.UnsafePlacementZones)
					{
						const int dChebU
							= std::max(std::abs(candidate.X - uz.Coords.X), std::abs(candidate.Y - uz.Coords.Y));
						if (dChebU <= 6)
						{
							spatialScore -= (7 - dChebU) * 280;
							break;
						}
					}
				}

				if (isCombatDefense)
					spatialScore += DefenseSpotShift(pHouse, candidate, ownTowerRangeCells, enemyDefenseReach);

				if (isNavalBld)
				{
					// Exact AdvAI NavalYard_Placement_Cell_Value +
					// Modify_Rating_By_Terrain_Passability + minNavalDist spacing: AdvAI
					// minimizes: (conyardDist * + (closestDist * 100), keeping Naval
					// Yards hugged right next to our closest shoreline structures and
					// ConYard while maintaining >= 5.0 cells between multiple Naval
					// Yards.
					const CellStruct candCenter { static_cast<short>(candidate.X + w / 2),
						static_cast<short>(candidate.Y + h / 2) };
					double closestDistSq = 1e12;
					double conyardDistSq = 1e12;
					int navalProximityPenalty = 0;

					for (auto const pOwnB : pHouse->Buildings)
					{
						if (!pOwnB || pOwnB->InLimbo || !pOwnB->IsAlive || !pOwnB->Type || pOwnB->Type->InvisibleInGame)
							continue;

						const CellStruct nbc = pOwnB->GetMapCoords();
						const int oW = pOwnB->Type->GetFoundationWidth();
						const int oH = pOwnB->Type->GetFoundationHeight(false);
						const CellStruct otherCenter { static_cast<short>(nbc.X + oW / 2),
							static_cast<short>(nbc.Y + oH / 2) };
						const double dxB = static_cast<double>(candCenter.X - otherCenter.X);
						const double dyB = static_cast<double>(candCenter.Y - otherCenter.Y);
						const double dSq = dxB * dxB + dyB * dyB;

						if (dSq < closestDistSq)
							closestDistSq = dSq;

						if (pOwnB->Type->ConstructionYard)
						{
							const double dxCY = static_cast<double>(candidate.X - otherCenter.X);
							const double dyCY = static_cast<double>(candidate.Y - otherCenter.Y);
							const double cySq = dxCY * dxCY + dyCY * dyCY;
							if (cySq < conyardDistSq)
								conyardDistSq = cySq;
						}

						if (pOwnB->Type->Naval || (pOwnB->Type->SpeedType == SpeedType::Float))
						{
							// AdvAI NavalMode minNavalDist = 5.0 cells between Naval Yards
							// (dSq < 25.0)
							if (dSq < 25.0)
								navalProximityPenalty = 50000;
						}
					}

					if (conyardDistSq >= 1e11)
					{
						const double dxC = static_cast<double>(candidate.X - center.X);
						const double dyC = static_cast<double>(candidate.Y - center.Y);
						conyardDistSq = dxC * dxC + dyC * dyC;
					}

					if (closestDistSq >= 1e11)
						closestDistSq = conyardDistSq;

					const double closestDist = std::sqrt(closestDistSq);
					const double conyardDist = std::sqrt(conyardDistSq);
					int advAiCost
						= std::max(1, static_cast<int>(conyardDist * 10.0) + static_cast<int>(closestDist * 100.0));

					// AdvAI Modify_Rating_By_Terrain_Passability (4 diagonal water
					// corners around foundation):
					auto isCornerFloatPassable = [&](short cx, short cy) -> bool
					{
						const CellStruct cc { cx, cy };
						if (!MapClass::Instance.CoordinatesLegal(cc))
							return true;

						if (auto const pCorner = MapClass::Instance.TryGetCellAt(cc))
						{
							return pCorner->IsClearToMove(
								SpeedType::Float, true, true, 0, MovementZone::Normal, 0, false);
						}

						return true;
					};

					const bool passableAbove = isCornerFloatPassable(
						static_cast<short>(candidate.X - 1), static_cast<short>(candidate.Y - 1));
					const bool passableBelow = isCornerFloatPassable(
						static_cast<short>(candidate.X + w), static_cast<short>(candidate.Y + h));
					const bool passableEast = isCornerFloatPassable(
						static_cast<short>(candidate.X + w), static_cast<short>(candidate.Y - 1));
					const bool passableWest = isCornerFloatPassable(
						static_cast<short>(candidate.X - 1), static_cast<short>(candidate.Y + h));

					if (!passableAbove && !passableBelow)
						advAiCost *= 3;

					if (!passableEast && !passableWest)
						advAiCost *= 3;

					if (!passableAbove)
						advAiCost = (advAiCost * 4) / 3;

					if (!passableBelow)
						advAiCost = (advAiCost * 4) / 3;

					if (!passableEast)
						advAiCost = (advAiCost * 4) / 3;

					if (!passableWest)
						advAiCost = (advAiCost * 4) / 3;

					spatialScore = 50000 - advAiCost - navalProximityPenalty - (hasOneCellMargin ? 0 : 4000);

					// Expert 4 (Naval) -> Expert 1 (BaseBuild) Strategic Water-Body Signaling:
					// Prefer placing Naval Yards in water bodies (MovementZone::Water) that contain
					// enemy Naval Yards / warships or border enemy/resource islands, and penalize isolated inland ponds!
					if (!strategicWaterZones.empty())
					{
						const int candWaterZone
							= MapClass::Instance.GetMovementZoneType(candidate, MovementZone::Water, false);
						if (candWaterZone >= 0)
						{
							const bool isStrategicSea
								= std::find(strategicWaterZones.begin(), strategicWaterZones.end(), candWaterZone)
								!= strategicWaterZones.end();
							if (isStrategicSea)
								spatialScore += 3500;
							else
								spatialScore -= 6000;
						}
					}
				}

				// Bonus when South/East exit of a Factory or Refinery has 2+ clear
				// cells in front of it
				if (isFactoryOrBarracks || isRefineryOrSlaveMiner)
				{
					const CellStruct southExit { static_cast<short>(candidate.X + w / 2),
						static_cast<short>(candidate.Y + h + 1) };
					const CellStruct eastExit { static_cast<short>(candidate.X + w + 1),
						static_cast<short>(candidate.Y + h / 2) };
					if (auto const pSE = MapClass::Instance.TryGetCellAt(southExit))
					{
						if (pSE->GetBuilding() == nullptr && pSE->LandType != LandType::Water
							&& pSE->LandType != LandType::Rock)
						{
							spatialScore += 220;
						}
					}

					if (auto const pEE = MapClass::Instance.TryGetCellAt(eastExit))
					{
						if (pEE->GetBuilding() == nullptr && pEE->LandType != LandType::Water
							&& pEE->LandType != LandType::Rock)
						{
							spatialScore += 220;
						}
					}
				}

				const CellStruct pullGoal = (isSteppingStoneExpander && !isResourceBld && stoneGoal != CellStruct::Empty)
					? stoneGoal
					: (oreTreeCell != CellStruct::Empty ? oreTreeCell : nearestOreCell);

				// A refinery is never a stepping stone: one placed halfway to a far field stands on
				// bare ground. Past the first one it goes within 10 cells of its field or waits
				// (cooldown) while power plants and barracks crawl there; each failed placement in a
				// row widens that by 4 cells and after 3 any cell will do (rugged maps).
				if (pBuildingType->Refinery && !ownRefineryCells.empty() && pullGoal != CellStruct::Empty
					&& refineryReach != INT_MAX)
				{
					const CellStruct refCenter { static_cast<short>(candidate.X + w / 2), static_cast<short>(candidate.Y + h / 2) };
					const bool nearGoal = std::max(std::abs(refCenter.X - pullGoal.X), std::abs(refCenter.Y - pullGoal.Y)) <= refineryReach;

					// Any ore of this landmass within 8 cells will do too: a field whose heart is walled
					// by cliffs is still mined from its open edge.
					const bool nearAnyOre = nearGoal || std::any_of(nearbyOreCells.begin(), nearbyOreCells.end(),
						[&refCenter](const CellStruct& ore) { return std::max(std::abs(refCenter.X - ore.X), std::abs(refCenter.Y - ore.Y)) <= 8; });

					if (!nearAnyOre)
						continue;
				}

				if ((isResourceBld || isSteppingStoneExpander) && pullGoal != CellStruct::Empty)
				{
					// Expert 6, Slot 34 & AdvAI Stepping-Stone Crawler:
					// Chain Refineries, Silos, Ore Purifiers AND BaseNormal
					// stepping-stone Power Plants directly toward the nearest uncolonized
					// Ore/Gem patch (or toward the enemy base: offensive crawl).
					const int distToOre
						= std::abs(candidate.X - pullGoal.X) + std::abs(candidate.Y - pullGoal.Y);
					const int orePull = std::clamp(52 + (crawlBias / 8), 32, 95);
					spatialScore -= distToOre * orePull;

					// Next to its tree, never hugging it: the cells around a tree are where it grows
					// new ore.
					if (pullGoal == oreTreeCell && oreTreeCell != CellStruct::Empty
						&& std::max(std::abs(candidate.X - oreTreeCell.X), std::abs(candidate.Y - oreTreeCell.Y)) < 3)
					{
						spatialScore -= 600;
					}

					// Once two refineries stand, the next keeps 10 cells from the others (AdvAI
					// spacing on scattered ore): next to another one it only shares its ore.
					if (pBuildingType->Refinery && ownRefineryCells.size() >= 2)
					{
						int nearestRefinery = INT_MAX;

						for (auto const& rc : ownRefineryCells)
							nearestRefinery = std::min(nearestRefinery, static_cast<int>(std::max(std::abs(candidate.X - rc.X), std::abs(candidate.Y - rc.Y))));

						if (nearestRefinery < 10)
							spatialScore -= (10 - nearestRefinery) * 300;
					}

					int cover = 0;

					for (auto const& dc : ownCoverDefenses)
					{
						if (std::max(std::abs(candidate.X - dc.X), std::abs(candidate.Y - dc.Y)) <= 6)
							cover++;
					}

					for (auto const& [enemyCell, reach] : enemyStructureReach)
					{
						const int dE = std::max(std::abs(candidate.X - enemyCell.X), std::abs(candidate.Y - enemyCell.Y));

						if (cover >= 2)
						{
							// Under our towers: advance, only never hug an enemy structure.
							if (dE <= 5)
								spatialScore -= 2500;
							else if (dE <= reach)
								spatialScore -= 300;
						}
						else if (dE <= reach)
						{
							spatialScore -= 2500 + (reach - dE) * 150;
						}
					}

					if (enableBaseCrawl && radius >= 2 && radius <= 12)
						spatialScore += 220 + std::clamp(crawlBias / 3, 0, 180);
				}
				else if (isCombatDefense)
				{
					// Tactical Battery Spacing: keep >= 3 cells between turrets to avoid single-shot
					// splash overlap while allowing tight 3..5 cell interlocking defense batteries at
					// frontlines, base entrances, and resource chokes!
					for (const CellStruct& dc : ownMatchingDefCells)
					{
						const int dCheb = std::max(std::abs(candidate.X - dc.X), std::abs(candidate.Y - dc.Y));
						if (dCheb < 3)
							spatialScore -= (3 - dCheb) * 320;
						else if (dCheb >= 3 && dCheb <= 6 && directionalAttackedBld != CellStruct::Empty)
							spatialScore += 140; // Interlocking crossfire battery bonus!
					}

					// Priority 1: Directional Defense Placement along active attack
					// vector (B -> A)
					if (directionalAttackedBld != CellStruct::Empty && directionalAttacker != CellStruct::Empty)
					{
						const double dxA = static_cast<double>(directionalAttacker.X - directionalAttackedBld.X);
						const double dyA = static_cast<double>(directionalAttacker.Y - directionalAttackedBld.Y);
						const double L2 = dxA * dxA + dyA * dyA;

						const int distToAttacked = std::abs(candidate.X - directionalAttackedBld.X)
							+ std::abs(candidate.Y - directionalAttackedBld.Y);

						if (L2 >= 9.0)
						{
							const double cx = static_cast<double>(candidate.X - directionalAttackedBld.X);
							const double cy = static_cast<double>(candidate.Y - directionalAttackedBld.Y);
							const double distPerp = std::abs(cx * dyA - cy * dxA) / std::sqrt(L2);
							const double dot = cx * dxA + cy * dyA;

							spatialScore -= static_cast<int>(distPerp * 45.0) + distToAttacked * 35;

							if (dot < 0.0)
								spatialScore -= 1200; // Behind the attacked building instead of

							// facing attacker
							else
								spatialScore += 350;
						}
						else
						{
							spatialScore -= distToAttacked * 45;
						}
					}
					else if (undefendedKeyAssetCell != CellStruct::Empty)
					{
						// Priority 2: Shield undefended ConYard, War Factory, Superweapon,
						// exposed Refinery, or Power Plant Cluster (3+ plants grouped)!
						const int distToAsset = std::abs(candidate.X - undefendedKeyAssetCell.X)
							+ std::abs(candidate.Y - undefendedKeyAssetCell.Y);

						spatialScore -= distToAsset * 42;

						if (distToAsset >= 4 && distToAsset <= 9)
							spatialScore += 280;

						if (enemyTargetCell != CellStruct::Empty)
						{
							const int distToEnemy
								= std::abs(candidate.X - enemyTargetCell.X) + std::abs(candidate.Y - enemyTargetCell.Y);

							spatialScore -= distToEnemy * 16;
						}
					}
					else if (isGroundDefense && enemyTargetCell != CellStruct::Empty && routeDefenseSpot == CellStruct::Empty)
					{
						// Priority 3: Frontline perimeter facing the enemy
						const int distToEnemy
							= std::abs(candidate.X - enemyTargetCell.X) + std::abs(candidate.Y - enemyTargetCell.Y);

						const int enemyPull = std::clamp(38 + (turretCreepBias / 10), 20, 80);

						spatialScore -= distToEnemy * enemyPull;

						if (radius >= 6 && radius <= 15)
							spatialScore += 220 + std::clamp(turretCreepBias / 2, 0, 220);
					}
					else if (isPureAA)
					{
						// Spread AA defenses around mid-ring (radius 5..11) to cover both
						// ConYard/Tech/Power clusters and frontline
						if (radius >= 5 && radius <= 11)
							spatialScore += 180;
					}

					// Route heat: cover the approaches the enemy really takes, above all the chosen spot.
					if (isGroundDefense && ownTowerRangeCells > 0)
						spatialScore += RouteDefenseCellShift(pHouse, candidate, ownTowerRangeCells, routeDefenseSpot);
				}
				else if (isSupportRadiusBld)
				{
					// Area-Support Dispersion (Gap Generator, Cloak Generator, Sensor
					// Array): Cover uncovered base buildings while keeping optimal
					// distance from existing support structures of the same network!
					for (const CellStruct& sc : existingSameSupportCells)
					{
						const int dSame = std::abs(candidate.X - sc.X) + std::abs(candidate.Y - sc.Y);

						if (dSame < targetSupportSeparation)
							spatialScore -= 4500;
					}

					if (!uncoveredKeyBldCells.empty())
					{
						int minDistUncovered = 9999;

						for (const CellStruct& uc : uncoveredKeyBldCells)
						{
							const int dU = std::abs(candidate.X - uc.X) + std::abs(candidate.Y - uc.Y);

							if (dU < minDistUncovered)
								minDistUncovered = dU;
						}

						spatialScore -= minDistUncovered * 40;
					}
				}
				else if (isRearProtected && enemyTargetCell != CellStruct::Empty)
				{
					// Expert 1: Tuck Power Plants, Tech Centers & Superweapons safely
					// into the rear perimeter away from enemy attacks
					const int distToEnemy
						= std::abs(candidate.X - enemyTargetCell.X) + std::abs(candidate.Y - enemyTargetCell.Y);

					spatialScore += distToEnemy * 18;
				}
				else if (isTunnelBld)
				{
					if (!isForwardTunnelMouth)
					{
						// 1st Tunnel Mouth (Home Entrance): Place near our home
						// Barracks/War Factory so newly produced infantry & vehicles
						// immediately enter the portal!
						if (homeFactoryCell != CellStruct::Empty)
						{
							const int distToFac
								= std::abs(candidate.X - homeFactoryCell.X) + std::abs(candidate.Y - homeFactoryCell.Y);

							spatialScore -= distToFac * 35;

							if (distToFac >= 4 && distToFac <= 9)
								spatialScore += 260;
						}
					}
					else
					{
						// 2nd+ Tunnel Mouth built directly from Construction Yard (Forward
						// Exit): Maximize separation from existing Tunnel Mouths and push
						// toward the enemy flank/outpost!
						int minDistToExistingTunnel = 9999;

						for (const CellStruct& tc : existingTunnelCells)
						{
							const int dT = std::abs(candidate.X - tc.X) + std::abs(candidate.Y - tc.Y);

							if (dT < minDistToExistingTunnel)
								minDistToExistingTunnel = dT;
						}

						if (minDistToExistingTunnel < 10)
							spatialScore -= 5000;
						else
							spatialScore += std::min(minDistToExistingTunnel, 45) * 45;

						if (enemyTargetCell != CellStruct::Empty)
						{
							const int distToEnemy
								= std::abs(candidate.X - enemyTargetCell.X) + std::abs(candidate.Y - enemyTargetCell.Y);

							spatialScore -= distToEnemy * 40;
						}
					}
				}
				else if (isFactoryOrBarracks
					&& (enemyTargetCell != CellStruct::Empty || nearestOreCell != CellStruct::Empty))
				{
					// Place War Factories & Barracks forward along the crawl chain
					// (stepping toward uncolonized Ore when expanding economy, or toward
					// Enemy frontline!)
					const bool stepFactoryToOre = !frontFactory && (nearestOreCell != CellStruct::Empty && bestOreDist > 10
						&& (pHouse->CountResourceDestinations < 3 || Unsorted::CurrentFrame > 2400));
					const CellStruct facGoal = stepFactoryToOre
						? nearestOreCell
						: (enemyTargetCell != CellStruct::Empty ? enemyTargetCell : nearestOreCell);
					const int distToGoal = std::abs(candidate.X - facGoal.X) + std::abs(candidate.Y - facGoal.Y);
					const int fwdPull = stepFactoryToOre ? std::clamp(38 + (crawlBias / 10), 24, 75)
														: std::clamp(22 + (crawlBias / 12), 12, 55);
					spatialScore -= distToGoal * fwdPull;

					if (enableBaseCrawl && radius >= 2 && radius <= 12)
						spatialScore += 180;
				}

				if (creepFrontGoal != CellStruct::Empty && isGroundDefense)
				{
					// Lane from the forward anchor to the enemy's heart (integer geometry, MP safe):
					// cells ahead of the anchor within ~2.5 cells of that axis stay free for the next
					// stone; flanks 3..7 cells off the axis are preferred, forward is better, and a
					// tower that already reaches enemy structures is worth more.
					const int side = laneSide(candidate, creepFrontGoal);

					if (side == 0)
						spatialScore -= 1800;
					else if (side == 1)
						spatialScore += 250;

					spatialScore -= (std::abs(candidate.X - creepFrontGoal.X) + std::abs(candidate.Y - creepFrontGoal.Y)) * 15;

					const int towerRange = (pWpn && pWpn->Range > 0) ? (pWpn->Range / 256) : 0;

					if (towerRange > 0)
					{
						int inRange = 0;

						for (auto const& ec : enemyBaseCells)
						{
							if (std::max(std::abs(candidate.X - ec.X), std::abs(candidate.Y - ec.Y)) <= towerRange)
								inRange++;
						}

						spatialScore += std::min(3, inRange) * 350;
					}
				}

				if (frontFactory)
				{
					int cover = 0;

					for (auto const& dc : ownCoverDefenses)
					{
						if (std::max(std::abs(candidate.X - dc.X), std::abs(candidate.Y - dc.Y)) <= 7)
							cover++;
					}

					spatialScore += std::min(3, cover) * 200;

					for (auto const& [enemyCell, reach] : enemyStructureReach)
					{
						const int dE = std::max(std::abs(candidate.X - enemyCell.X), std::abs(candidate.Y - enemyCell.Y));

						if (dE <= reach)
							spatialScore -= (cover >= 3 ? 600 : 2500) + (reach - dE) * 100;
					}

					if (laneSide(candidate, enemyTargetCell) == 0)
						spatialScore -= 1800;
				}

				if (isPreciousBld && !enemyBaseCells.empty())
				{
					int nearestEnemy = INT_MAX;

					for (auto const& ec : enemyBaseCells)
						nearestEnemy = std::min(nearestEnemy, std::max(std::abs(candidate.X - ec.X), std::abs(candidate.Y - ec.Y)));

					if (nearestEnemy < 22)
						spatialScore -= (22 - nearestEnemy) * 120;
				}

				for (auto const& anchor : swSpreadAnchors)
				{
					const int dCheb = std::max(std::abs(candidate.X - anchor.X), std::abs(candidate.Y - anchor.Y));

					if (dCheb < 6)
						spatialScore -= (6 - dCheb) * 350;
				}

				if (architectActive)
				{
					const CellStruct candMid { static_cast<short>(candidate.X + w / 2), static_cast<short>(candidate.Y + h / 2) };
					spatialScore -= architectLanePenalty;

					// Street grid: a block that lines up with its neighbours across a 1-cell street
					// (same edge line) keeps the roads straight instead of a maze of dead ends.
					if (!isCombatDefense && !isNavalBld)
					{
						int aligned = 0;

						for (auto const& fp : GetBuildingFootprints())
						{
							if (aligned >= 4 || fp.IsDefense || (fp.Owner != pHouse && (!fp.Owner || !pHouse->IsAlliedWith(fp.Owner))))
								continue;

							const int gapX = std::max(fp.X - (candidate.X + w), candidate.X - (fp.X + fp.W));
							const int gapY = std::max(fp.Y - (candidate.Y + h), candidate.Y - (fp.Y + fp.H));

							if ((gapY == 1 && gapX < 0 && (fp.X == candidate.X || fp.X + fp.W == candidate.X + w))
								|| (gapX == 1 && gapY < 0 && (fp.Y == candidate.Y || fp.Y + fp.H == candidate.Y + h)))
							{
								aligned++;
							}
						}

						spatialScore += aligned * 70;
					}

					// Power plants grouped behind the core, in its shadow from the threat side, and
					// never trailing far out of the base (a raid there blacks the base out).
					if (isPowerPlantBld && !isSteppingStoneExpander && !isCombatDefense)
					{
						if (architectThreatCell != CellStruct::Empty)
						{
							const int tx = architectThreatCell.X - center.X;
							const int ty = architectThreatCell.Y - center.Y;
							const int len2 = tx * tx + ty * ty;

							if (len2 >= 16)
							{
								const double len = std::sqrt(static_cast<double>(len2));
								const int vx = candMid.X - center.X;
								const int vy = candMid.Y - center.Y;
								const int behind = static_cast<int>(-(vx * tx + vy * ty) / len);
								const int lateral = static_cast<int>(std::abs(vx * ty - vy * tx) / len);

								spatialScore += std::clamp(behind, -8, 10) * 60 - std::max(0, lateral - 6) * 35;
							}
						}

						if (radius > 10)
							spatialScore -= (radius - 10) * 60;

						int nearestPlant = INT_MAX;

						for (auto const& pc : architectPowerCells)
							nearestPlant = std::min(nearestPlant, static_cast<int>(std::max(std::abs(candMid.X - pc.X), std::abs(candMid.Y - pc.Y))));

						if (nearestPlant != INT_MAX)
							spatialScore += nearestPlant <= 6 ? 250 : -std::min(nearestPlant - 6, 14) * 20;
					}

					// Defense ring: spaced beyond the enemy splash, covering refineries / ConYard /
					// factories, on the side of them that faces the threat.
					if (isCombatDefense)
					{
						for (auto const& dc : ownMatchingDefCells)
						{
							const int dCheb = std::max(std::abs(candidate.X - dc.X), std::abs(candidate.Y - dc.Y));

							if (dCheb < architectSplashSpacing)
								spatialScore -= (architectSplashSpacing - dCheb) * 380;
						}

						const int towerRange = (pWpn && pWpn->Range > 0) ? std::max(4, pWpn->Range / Unsorted::LeptonsPerCell) : 5;
						int covered = 0;
						int nearestAssetDist = INT_MAX;
						CellStruct nearestAsset = CellStruct::Empty;

						for (auto const& ac : architectCoverAssets)
						{
							const int dA = std::max(std::abs(candMid.X - ac.X), std::abs(candMid.Y - ac.Y));

							if (dA <= towerRange)
								covered++;

							if (dA < nearestAssetDist)
							{
								nearestAssetDist = dA;
								nearestAsset = ac;
							}
						}

						spatialScore += std::min(3, covered) * (isPureAA ? 150 : 220);

						if (!isPureAA && directionalAttackedBld == CellStruct::Empty && architectThreatCell != CellStruct::Empty
							&& nearestAsset != CellStruct::Empty)
						{
							const int dot = (candMid.X - nearestAsset.X) * (architectThreatCell.X - nearestAsset.X)
								+ (candMid.Y - nearestAsset.Y) * (architectThreatCell.Y - nearestAsset.Y);

							spatialScore += dot > 0 ? 300 : -350;
						}
					}

					// Superweapon dispersal: no two high-value buildings inside one strike.
					if (architectSwRadius > 0 && isHighValueBld)
					{
						const int keepApart = architectSwRadius + 1;

						for (auto const& hc : architectHighValueCells)
						{
							const int dx = candMid.X - hc.X;
							const int dy = candMid.Y - hc.Y;
							const int d2 = dx * dx + dy * dy;

							if (d2 < keepApart * keepApart)
								spatialScore -= (keepApart - static_cast<int>(std::sqrt(static_cast<double>(d2)))) * 450;
						}
					}

					if (architectRebuildCell != CellStruct::Empty && candidate == architectRebuildCell)
						spatialScore += 4000;
				}

				if (spatialScore > bestSpatialScore)
				{
					bestSpatialScore = spatialScore;

					bestCandidate = candidate;
				}

				if (++validCandidatesChecked >= maxCandidatesToEvaluate)
					return rememberOffensiveStone(bestCandidate);
			}
		}
	}

	return rememberOffensiveStone(bestCandidate);
}

int NeuralAI::PlaceCompletedBuilding(BuildingClass* pFactory, BuildingClass* pBuilding, CellStruct& outCell)
{
	outCell = CellStruct::Empty;

	auto const pHouse = pFactory ? pFactory->Owner : (pBuilding ? pBuilding->Owner : nullptr);

	if (!pHouse || !pBuilding || !pBuilding->Type)
		return 0;

	auto const pBuildingType = pBuilding->Type;
	auto const pExt = HouseExt::Fetch(pHouse);
	const int w = pBuildingType->GetFoundationWidth();
	const int h = pBuildingType->GetFoundationHeight(false);

	// Step 1: Check active PendingClearanceCell reservation (waiting for sold
	// structure or scattered units to clear the footprint)
	if (GetMacroState(pHouse).PendingClearanceType != nullptr)
	{
		const bool coordsValid = GetMacroState(pHouse).PendingClearanceCell != CellStruct::Empty
			&& MapClass::Instance.CoordinatesLegal(GetMacroState(pHouse).PendingClearanceCell);

		const bool foundationMatches = (GetMacroState(pHouse).PendingClearanceType == pBuildingType)
			|| (GetMacroState(pHouse).PendingClearanceFoundationWidth == w
				&& GetMacroState(pHouse).PendingClearanceFoundationHeight == h
				&& GetMacroState(pHouse).PendingClearanceFoundation == pBuildingType->Foundation);

		if (!coordsValid || !foundationMatches)
		{
			GetMacroState(pHouse).ClearClearanceReservation();
		}
		else
		{
			GetMacroState(pHouse).PendingClearanceType = pBuildingType;

			if (Unsorted::CurrentFrame < GetMacroState(pHouse).NextClearanceCheckFrame)
				return 1; // Keep building waiting in Construction Yard!

			bool stillObstructed = false;

			for (int fy = 0; fy < h; ++fy)
			{
				for (int fx = 0; fx < w; ++fx)
				{
					const CellStruct fc { static_cast<short>(GetMacroState(pHouse).PendingClearanceCell.X + fx),
						static_cast<short>(GetMacroState(pHouse).PendingClearanceCell.Y + fy) };

					if (auto const pCell = MapClass::Instance.TryGetCellAt(fc))
					{
						if (pCell->GetBuilding() != nullptr)
							stillObstructed = true;

						if (pCell->GetUnit(false) || pCell->GetInfantry(false))
						{
							if (pCell->OverlayTypeIndex >= 0 && pCell->WallOwnerIndex == pHouse->ArrayIndex)
							{
								if (auto const pOv = OverlayTypeClass::Array.GetItem(pCell->OverlayTypeIndex))
								{
									if (pOv->Wall)
									{
										CellStruct wallCell = fc;
										pHouse->SellWall(wallCell, true);
									}
								}
							}

							pCell->ScatterContent(CellClass::Cell2Coord(fc), true, true, false);

							stillObstructed = true;
						}
					}
				}
			}

			if (!stillObstructed)
			{
				bool canExistNow = true;

				for (int fy = 0; fy < h && canExistNow; ++fy)
				{
					for (int fx = 0; fx < w; ++fx)
					{
						const CellStruct fc { static_cast<short>(GetMacroState(pHouse).PendingClearanceCell.X + fx),
							static_cast<short>(GetMacroState(pHouse).PendingClearanceCell.Y + fy) };

						auto const pCell = MapClass::Instance.TryGetCellAt(fc);

						if (!pCell || !pCell->CanThisExistHere(pBuildingType->SpeedType, pBuildingType, pHouse))
						{
							canExistNow = false;

							break;
						}
					}
				}

				if (canExistNow)
				{
					outCell = GetMacroState(pHouse).PendingClearanceCell;

					Debug::Log("[NeuralAI] House %d placed %s at cleared reservation "
							"cell (%d,%d).\n",
						pHouse->ArrayIndex, pBuildingType->ID, outCell.X, outCell.Y);

					GetMacroState(pHouse).ClearClearanceReservation();

					return 2;
				}
			}

			GetMacroState(pHouse).ClearanceAttempts++;

			if (GetMacroState(pHouse).ClearanceAttempts < 6)
			{
				GetMacroState(pHouse).NextClearanceCheckFrame = Unsorted::CurrentFrame + 45;

				return 1; // Keep waiting in Construction Yard!
			}

			GetMacroState(pHouse).ClearClearanceReservation();

			GetMacroState(pHouse).LastClearanceFailedFrame = Unsorted::CurrentFrame;
		}
	}

	// AdvAI Harvester Recovery via Refinery Sell-and-Replace (Variant A) or
	// Temporary Placement (Variant B): Selling an existing static Refinery
	// (especially one on an island that lost its Harvester, or a damaged/idle
	// Refinery) and placing the newly built Refinery in its exact footprint
	// spawns a FreeUnit Harvester immediately while recovering 50% cash and
	// keeping the War Factory vehicle queue 100% dedicated to combat units!
	const bool isStaticRefineryWithFreeHarv = (pBuildingType->Refinery || pBuildingType->ResourceDestination)
		&& !pBuildingType->ResourceGatherer && pBuildingType->Enslaves == nullptr
		&& pBuildingType->UndeploysInto == nullptr && pBuildingType->FreeUnit != nullptr
		&& (pBuildingType->FreeUnit->Harvester || pBuildingType->FreeUnit->ResourceGatherer);

	auto tryReplaceExistingRefineryForHarvester = [&]() -> bool
	{
		if (!isStaticRefineryWithFreeHarv)
			return false;

		const int adj = std::max(1, pBuildingType->Adjacent);
		const int targetZone = GetMacroState(pHouse).TargetRecoveryRefineryZone;
		std::vector<BuildingClass*> zeroHarvZoneCandidates;
		std::vector<BuildingClass*> damagedIdleCandidates;
		std::vector<BuildingClass*> idleCandidates;
		std::vector<BuildingClass*> fallbackCandidates;

		for (auto const pOldRef : pHouse->Buildings)
		{
			if (!pOldRef || !pOldRef->IsAlive || pOldRef->InLimbo || pOldRef == pBuilding || !pOldRef->Type)
				continue;

			if ((!pOldRef->Type->Refinery && !pOldRef->Type->ResourceDestination) || pOldRef->Type->ResourceGatherer
				|| pOldRef->Type->Enslaves != nullptr || pOldRef->Type->UndeploysInto != nullptr)
			{
				continue;
			}

			if (pOldRef->CurrentMission == Mission::Construction || pOldRef->QueuedMission == Mission::Construction
				|| pOldRef->CurrentMission == Mission::Selling || pOldRef->QueuedMission == Mission::Selling)
			{
				continue;
			}

			if (pOldRef->Type->GetFoundationWidth() != w || pOldRef->Type->GetFoundationHeight(false) != h
				|| pOldRef->Type->Foundation != pBuildingType->Foundation)
			{
				continue;
			}

			const CellStruct oldCell = pOldRef->GetMapCoords();
			bool hasAdjacentAnchor = false;
			for (auto const pOther : pHouse->Buildings)
			{
				if (!pOther || !pOther->IsAlive || pOther->InLimbo || pOther == pOldRef || pOther == pBuilding
					|| !pOther->Type)
				{
					continue;
				}

				if ((!pOther->Type->BaseNormal && !pOther->Type->ConstructionYard
						&& !BuildingTypeExt::IsAIBaseNormal(pOther->Type))
					|| pOther->CurrentMission == Mission::Selling || pOther->QueuedMission == Mission::Selling)
				{
					continue;
				}

				const CellStruct otherCell = pOther->GetMapCoords();
				const int otherW = pOther->Type->GetFoundationWidth();
				const int otherH = pOther->Type->GetFoundationHeight(false);
				const int gapX = std::max(0,
					std::max(static_cast<int>(oldCell.X) - (static_cast<int>(otherCell.X) + otherW - 1),
						static_cast<int>(otherCell.X) - (static_cast<int>(oldCell.X) + w - 1)));
				const int gapY = std::max(0,
					std::max(static_cast<int>(oldCell.Y) - (static_cast<int>(otherCell.Y) + otherH - 1),
						static_cast<int>(otherCell.Y) - (static_cast<int>(oldCell.Y) + h - 1)));
				if (std::max(gapX, gapY) <= adj)
				{
					hasAdjacentAnchor = true;
					break;
				}
			}

			if (!hasAdjacentAnchor)
				continue;

			const int oldZone = MapClass::Instance.GetMovementZoneType(oldCell, MovementZone::Normal, pOldRef->OnBridge);
			int harvsInOldZone = 0;
			if (oldZone >= 0)
			{
				for (auto const pHU : UnitClass::Array)
				{
					if (pHU && pHU->Owner == pHouse && !pHU->InLimbo && pHU->IsAlive && pHU->Type
						&& (pHU->Type->Harvester || pHU->Type->ResourceGatherer)
						&& ObjectZone(pHU, MovementZone::Normal)
							== oldZone)
					{
						harvsInOldZone++;
					}
				}
			}

			if ((targetZone >= 0 && oldZone == targetZone) || (oldZone >= 0 && harvsInOldZone == 0))
				zeroHarvZoneCandidates.push_back(pOldRef);

			fallbackCandidates.push_back(pOldRef);
			if (!pOldRef->HasAnyLink())
			{
				idleCandidates.push_back(pOldRef);
				if (pOldRef->Health < pOldRef->Type->Strength)
					damagedIdleCandidates.push_back(pOldRef);
			}
		}

		const auto& pool = !zeroHarvZoneCandidates.empty()
			? zeroHarvZoneCandidates
			: (!damagedIdleCandidates.empty() ? damagedIdleCandidates
											: (!idleCandidates.empty() ? idleCandidates : fallbackCandidates));
		if (pool.empty())
			return false;

		const int randIdx = ScenarioClass::Instance->Random.RandomRanged(0, static_cast<int>(pool.size()) - 1);
		BuildingClass* pChosenOldRef = pool[randIdx];
		const CellStruct replaceCell = pChosenOldRef->GetMapCoords();

		Debug::Log("[NeuralAI] HarvesterRecovery: House %d selling existing refinery %s "
				"at (%d,%d) to place new refinery %s in its exact spot.\n",
			pHouse->ArrayIndex, pChosenOldRef->Type->ID, replaceCell.X, replaceCell.Y, pBuildingType->ID);

		pChosenOldRef->Sell(1);
		for (int fy = 0; fy < h; ++fy)
		{
			for (int fx = 0; fx < w; ++fx)
			{
				const CellStruct fc { static_cast<short>(replaceCell.X + fx), static_cast<short>(replaceCell.Y + fy) };
				if (auto const pCell = MapClass::Instance.TryGetCellAt(fc))
				{
					if (pCell->OverlayTypeIndex >= 0 && pCell->WallOwnerIndex == pHouse->ArrayIndex)
					{
						if (auto const pOv = OverlayTypeClass::Array.GetItem(pCell->OverlayTypeIndex))
						{
							if (pOv->Wall)
							{
								CellStruct wallCell = fc;
								pHouse->SellWall(wallCell, true);
							}
						}
					}

					pCell->ScatterContent(CellClass::Cell2Coord(fc), true, true, false);
				}
			}
		}

		auto& st = GetMacroState(pHouse);
		st.PendingClearanceCell = replaceCell;
		st.PendingClearanceType = pBuildingType;
		st.PendingClearanceFoundationWidth = w;
		st.PendingClearanceFoundationHeight = h;
		st.PendingClearanceFoundation = pBuildingType->Foundation;
		st.ClearanceAttempts = 0;
		st.NextClearanceCheckFrame = Unsorted::CurrentFrame + 60;
		st.IsBuildingRecoveryRefinery = false;
		st.TargetRecoveryRefineryZone = -1;
		st.NextHarvesterRecoveryFrame = Unsorted::CurrentFrame + 1800;
		return true;
	};

	if (GetMacroState(pHouse).IsBuildingRecoveryRefinery && isStaticRefineryWithFreeHarv
		&& (GetMacroState(pHouse).TargetRecoveryRefineryZone >= 0
			|| ScenarioClass::Instance->Random.RandomRanged(0, 99) < 65))
	{
		if (tryReplaceExistingRefineryForHarvester())
		{
			return 1; // Keep building waiting in ConYard while old refinery finishes
					// selling!
		}
	}

	// Step 2: Normal Spatial Tetris placement search
	const bool wantedByIslandHold = IslandHoldWants(pHouse, pBuildingType);
	CellStruct bestCell = NeuralAI::FindSpatialTetrisBuildCell(pHouse, pBuildingType);

	// A building an ally site plan wanted found no room there: it goes home (the failure code below
	// would count it against the home crawl and may sell buildings to clear space) and the plan
	// ends (AllySiteFailedFrame).
	{
		auto& siteSt = GetMacroState(pHouse);
		const bool wantedAtSite = siteSt.AllySiteUntilFrame > Unsorted::CurrentFrame
			&& siteSt.AllySiteBuildType == pBuildingType->ArrayIndex;

		if (wantedAtSite && bestCell == CellStruct::Empty)
		{
			siteSt.AllySiteFailedFrame = Unsorted::CurrentFrame;
			siteSt.AllySiteUntilFrame = -1;
			bestCell = NeuralAI::FindSpatialTetrisBuildCell(pHouse, pBuildingType);
		}
		else if (wantedAtSite)
		{
			CountDecision(NeuralDecision::AllySiteBuilding, pHouse->ArrayIndex);
		}
	}

	// A defense the Island Hold yard lacks found no cell the plan counts: it goes where any other
	// tower would (the failure code below may sell buildings to clear space) and the plan asks for
	// nothing until IslandHoldNoRoomUntilFrame, so the house does not keep making towers for a yard
	// that cannot take them.
	if (wantedByIslandHold)
	{
		auto& holdSt = GetMacroState(pHouse);

		if (bestCell == CellStruct::Empty)
		{
			holdSt.IslandHoldNoRoomUntilFrame = Unsorted::CurrentFrame + IslandHoldNoRoomFrames;
			// TEMP diagnostic: the island yard has no cell for this defense.
			Debug::Log("[NeuralAI] House %d IslandHold DIAG: no room for %s next to the yard at (%d,%d), frame %d.\n",
				pHouse->ArrayIndex, pBuildingType->ID, holdSt.IslandHoldCell.X, holdSt.IslandHoldCell.Y, Unsorted::CurrentFrame);
			bestCell = NeuralAI::FindSpatialTetrisBuildCell(pHouse, pBuildingType);
		}
		else
		{
			// TEMP diagnostic: where a defense chosen for the island yard ends.
			Debug::Log("[NeuralAI] House %d IslandHold DIAG: %s placed at (%d,%d), %d cells from the yard at (%d,%d), frame %d.\n",
				pHouse->ArrayIndex, pBuildingType->ID, bestCell.X, bestCell.Y,
				static_cast<int>(bestCell.DistanceFrom(holdSt.IslandHoldCell)), holdSt.IslandHoldCell.X, holdSt.IslandHoldCell.Y,
				Unsorted::CurrentFrame);
		}
	}

	// Placed next to an ally building: a defense for an ally under attack, or our crawl using ally
	// space for our expansion.
	if (bestCell != CellStruct::Empty && pBuildingType->IsBaseDefense
		&& GetMacroState(pHouse).AllyDefenseUntilFrame > Unsorted::CurrentFrame
		&& bestCell.DistanceFrom(GetMacroState(pHouse).AllyDefenseCell) <= 14.0)
	{
		CountDecision(NeuralDecision::AllyDefense, pHouse->ArrayIndex);
	}
	else if (bestCell != CellStruct::Empty && GameModeOptionsClass::Instance.BuildOffAlly)
	{
		const int bw = pBuildingType->GetFoundationWidth();
		const int bh = pBuildingType->GetFoundationHeight(false);

		for (auto const pAllyBld : BuildingClass::Array)
		{
			if (pAllyBld && pAllyBld->IsAlive && !pAllyBld->InLimbo && pAllyBld->Type && pAllyBld->Owner
				&& pAllyBld->Owner != pHouse && pHouse->IsAlliedWith(pAllyBld->Owner)
				&& pAllyBld->GetMapCoords().DistanceFrom(bestCell) <= static_cast<double>(std::max(bw, bh) + 4)
				&& GetMacroState(pHouse).AllySiteBuildType != pBuildingType->ArrayIndex)
			{
				CountDecision(NeuralDecision::AllyCrawlAnchor, pHouse->ArrayIndex);
				break;
			}
		}
	}

	if (bestCell != CellStruct::Empty)
	{
		auto& st = GetMacroState(pHouse);
		st.ConsecutivePlacementFailures = 0;
		st.BuildingPlacementCooldownUntilFrame.erase(pBuildingType->ArrayIndex);

		// Island Hold: the pick stored while this building was under construction is not the
		// yard's (its defense never displaces a building in progress), and the yard would start it
		// before the next evaluation. It waits for that evaluation, which counts what the island
		// yard lacks with this building in place.
		if (st.IslandHoldUntilFrame > Unsorted::CurrentFrame)
			pExt->NeuralAI_NextBuildingTarget = -1;

		if (pBuildingType->Refinery)
		{
			if (st.RefineryReachFailures > 0)
				st.RelaxedReachRefineries.push_back(pBuilding->UniqueID);

			st.RefineryReachFailures = std::max(0, st.RefineryReachFailures - 1);
		}

		if (pBuildingType->IsBaseDefense)
		{
			RecordDefensePlaced(pHouse, pBuildingType);

			bool defAntiInf = false;
			bool defAntiArmor = false;
			bool defAntiAir = false;
			bool defPureAA = false;
			bool defUnarmedBunker = false;
			GetDefenseCapabilities(pBuildingType, defAntiInf, defAntiArmor, defAntiAir, defPureAA, defUnarmedBunker);

			// A ground tower in range of a hot enemy approach (route heat telemetry).
			if (const int routeRange = GroundRangeOf(pBuildingType) / Unsorted::LeptonsPerCell; routeRange > 0)
			{
				CellStruct coveredSpot = CellStruct::Empty;

				if (FindRouteSpotCovered(pHouse, bestCell, routeRange, coveredSpot))
				{
					CountDecision(NeuralDecision::RouteTowerPlaced, pHouse->ArrayIndex);
					RecordRouteTowerPlaced(pHouse, bestCell, coveredSpot);
					Debug::Log("[NeuralAI] House %d route tower: %s at (%d,%d) covers (%d,%d)\n", pHouse->ArrayIndex, pBuildingType->ID,
						bestCell.X, bestCell.Y, coveredSpot.X, coveredSpot.Y);
				}
			}

			if (!defPureAA && st.FrontlineThreatNeedsDefenses > 0
				&& Unsorted::CurrentFrame <= st.FrontlineThreatActiveFrames)
			{
				st.FrontlineThreatNeedsDefenses--;
				if (st.FrontlineThreatNeedsDefenses <= 0)
				{
					st.FrontlineThreatNeedsDefenses = 0;
					st.FrontlineThreatCoords = CellStruct::Empty;
					st.FrontlineThreatBuildingCoords = CellStruct::Empty;
				}
			}
			else if (!st.AttackedBuildingsLIFO.empty())
			{
				bool poppedMatched = false;
				for (int i = static_cast<int>(st.AttackedBuildingsLIFO.size()) - 1; i >= 0; --i)
				{
					const auto& rec = st.AttackedBuildingsLIFO[i];
					if ((rec.Threat == NeuralThreatCategory::Air && defAntiAir)
						|| (rec.Threat == NeuralThreatCategory::Vehicle && defAntiArmor)
						|| (rec.Threat == NeuralThreatCategory::Infantry && defAntiInf))
					{
						st.AttackedBuildingsLIFO.erase(st.AttackedBuildingsLIFO.begin() + i);
						poppedMatched = true;
						break;
					}
				}

				if (!poppedMatched)
					st.AttackedBuildingsLIFO.pop_back();
			}
		}
		else
		{
			// AdvAI Local Frontline Threat Detection on Building Placement (Building_Body_AdvAI.cpp:525-605):
			// If we just placed a non-defense building within 8.0 cells of an armed enemy building or unit,
			// immediately flag FrontlineThreatNeedsDefenses = 2 so the ConYard builds defenses facing it!
			double nearestThreatDistSq = 324.0; // Within 18.0 cells of ANY enemy building or unit!
			CellStruct nearestThreatCoords = CellStruct::Empty;

			for (auto const pEB : BuildingClass::Array)
			{
				if (!pEB || pEB->InLimbo || !pEB->IsAlive || !pEB->Owner || !pEB->Type || pEB->Type->Insignificant
					|| pEB->Type->InvisibleInGame || pEB->Owner == pHouse || pHouse->IsAlliedWith(pEB->Owner) || !IsEnemyBuildingKnown(pHouse, pEB)
					|| pEB->Owner->IsNeutral())
				{
					continue;
				}

				const CellStruct ec = pEB->GetMapCoords();
				const double dx = static_cast<double>(bestCell.X - ec.X);
				const double dy = static_cast<double>(bestCell.Y - ec.Y);
				const double dSq = dx * dx + dy * dy;
				if (dSq <= nearestThreatDistSq)
				{
					nearestThreatDistSq = dSq;
					nearestThreatCoords = ec;
				}
			}

			for (auto const pTechno : TechnoClass::Array)
			{
				if (!pTechno || pTechno->InLimbo || !pTechno->IsAlive || !pTechno->Owner || pTechno->Owner == pHouse
					|| pHouse->IsAlliedWith(pTechno->Owner) || !IsCellVisibleToAI(pHouse, pTechno->GetCell()) || pTechno->Owner->IsNeutral()
					|| pTechno->WhatAmI() == AbstractType::Building)
				{
					continue;
				}

				// Flying units ask for anti-air, not for the ground towers this flag builds.
				if (pTechno->WhatAmI() == AbstractType::Aircraft || pTechno->GetTechnoType()->JumpJet || pTechno->IsInAir())
					continue;

				const CellStruct tc = pTechno->GetMapCoords();
				const double dx = static_cast<double>(bestCell.X - tc.X);
				const double dy = static_cast<double>(bestCell.Y - tc.Y);
				const double dSq = dx * dx + dy * dy;
				if (dSq <= nearestThreatDistSq)
				{
					nearestThreatDistSq = dSq;
					nearestThreatCoords = tc;
				}
			}

			if (nearestThreatCoords != CellStruct::Empty)
			{
				st.FrontlineThreatCoords = nearestThreatCoords;
				st.FrontlineThreatBuildingCoords = bestCell;
				st.FrontlineThreatActiveFrames = Unsorted::CurrentFrame + 1800;
				st.FrontlineThreatNeedsDefenses = 3;
			}

			// AdvAI Cluster Congestion Avoidance: if >= 3 owned buildings touch this newly placed
			// footprint within 1 cell, record a short-term UnsafePlacementZone so subsequent buildings
			// spread out along streets instead of creating a deadlocked cluster!
			int touchingOwnedCount = 0;
			for (auto const pOwnB : pHouse->Buildings)
			{
				if (!pOwnB || pOwnB->InLimbo || !pOwnB->IsAlive || pOwnB == pBuilding || !pOwnB->Type)
					continue;

				const CellStruct oc = pOwnB->GetMapCoords();
				const int oW = pOwnB->Type->GetFoundationWidth();
				const int oH = pOwnB->Type->GetFoundationHeight(false);
				const int gapX = std::max(0,
					std::max(static_cast<int>(bestCell.X) - (static_cast<int>(oc.X) + oW - 1),
						static_cast<int>(oc.X) - (static_cast<int>(bestCell.X) + w - 1)));
				const int gapY = std::max(0,
					std::max(static_cast<int>(bestCell.Y) - (static_cast<int>(oc.Y) + oH - 1),
						static_cast<int>(oc.Y) - (static_cast<int>(bestCell.Y) + h - 1)));
				if (std::max(gapX, gapY) <= 1)
					touchingOwnedCount++;
			}

			if (touchingOwnedCount >= 3)
				RecordUnsafePlacementZone(pHouse, bestCell, 900);
		}

		if (st.IsBuildingRecoveryRefinery && isStaticRefineryWithFreeHarv)
		{
			st.TemporaryRecoveryRefineryPlacedFrame[pBuilding->UniqueID] = Unsorted::CurrentFrame;
			st.IsBuildingRecoveryRefinery = false;
			st.TargetRecoveryRefineryZone = -1;
			st.NextHarvesterRecoveryFrame = Unsorted::CurrentFrame + 1800;
			Debug::Log("[NeuralAI] HarvesterRecovery: House %d placed temporary recovery "
					"refinery %s at (%d,%d). Will sell once FreeUnit harvester spawns.\n",
				pHouse->ArrayIndex, pBuildingType->ID, bestCell.X, bestCell.Y);
		}

		outCell = bestCell;
		return 2;
	}

	// No cell for a refinery: count it for the widening reach (FindSpatialTetrisBuildCell) before
	// anything else, or a map too rugged for the 10-cell reach never widens it.
	if (pBuildingType->Refinery)
		GetMacroState(pHouse).RefineryReachFailures++;

	// Harvester recovery with no free cell: sell an existing refinery and place the new one in its
	// exact spot. Only for a real recovery: as a fallback for any refinery that found no cell it
	// sold and rebuilt the same refinery forever (one free harvester per round, the ConYard never
	// free for anything else: Polar Oblong).
	if (isStaticRefineryWithFreeHarv && GetMacroState(pHouse).IsBuildingRecoveryRefinery
		&& tryReplaceExistingRefineryForHarvester())
	{
		return 1;
	}

	// Step 3: Surgical Space Clearance & Obsolete Structure Replacement when the
	// base is congested and no free cell exists!
	auto recordPlacementFailure = [&]() -> int
	{
		auto& st = GetMacroState(pHouse);

		// A refinery with no cell near its ore waits longer (1800 frames per failure, up to 5400) and
		// does not count as a general placement failure: those switch off the stepping-stone crawl,
		// the very thing that brings buildable cells next to the ore. The ConYard is free meanwhile.
		if (pBuildingType->Refinery)
		{
			st.BuildingPlacementCooldownUntilFrame[pBuildingType->ArrayIndex]
				= Unsorted::CurrentFrame + std::min(5400, 1800 * std::max(1, st.RefineryReachFailures));
		}
		else
		{
			st.ConsecutivePlacementFailures++;
		}

		const int cdUntil = Unsorted::CurrentFrame + std::min(3600, 300 * std::max(1, st.ConsecutivePlacementFailures));

		if (!pBuildingType->Refinery)
			st.BuildingPlacementCooldownUntilFrame[pBuildingType->ArrayIndex] = cdUntil;
		if (pBuildingType->PowerBonus > pBuildingType->PowerDrain && !pBuildingType->IsBaseDefense
			&& !pHouse->HasLowPower())
		{
			for (auto const pOtherBld : BuildingTypeClass::Array)
			{
				if (pOtherBld && pOtherBld->PowerBonus > pOtherBld->PowerDrain && !pOtherBld->IsBaseDefense)
					st.BuildingPlacementCooldownUntilFrame[pOtherBld->ArrayIndex] = cdUntil;
			}
		}

		if (FactoryClass* pBldFac
			= pHouse->GetPrimaryFactory(AbstractType::BuildingType, pBuildingType->Naval, pBuildingType->BuildCat))
		{
			pBldFac->AbandonProduction();
		}

		return 0;
	};

	if (Unsorted::CurrentFrame < GetMacroState(pHouse).LastClearanceFailedFrame + 150)
		return recordPlacementFailure();

	const int money = pHouse->Available_Money();
	const bool isCombatDef = pBuildingType->IsBaseDefense;

	const bool isLateGameHighTierDefense
		= isCombatDef && pBuildingType->TechLevel >= 6 && pBuildingType->Cost >= 1000 && money > 4000;

	if (isCombatDef && !isLateGameHighTierDefense)
	{
		return recordPlacementFailure(); // Only sacrifice structures for key
										// non-defense buildings or
	}

	// late-game high-tier defense upgrades
	bool hasUnpowered = false;
	int poweredDefAndSWCount = 0;
	int maxSinglePlantBonus = 0;
	bool hasAdvancedPowerPlants = false;

	const int realDrain = GetRealPowerDrainAndGridMetrics(
		pHouse, hasUnpowered, poweredDefAndSWCount, maxSinglePlantBonus, hasAdvancedPowerPlants);

	const int realSurplus = pHouse->PowerOutput - realDrain;

	const int requiredCushion
		= (poweredDefAndSWCount > 0) ? std::max(300, std::min(550, 220 + poweredDefAndSWCount * 45)) : 150;

	// Strict Silo Rule: NEVER sell Ore Silos if the house has any stored
	// Tiberium/Ore (because selling a Silo with ore destroys the stored credits!)
	const bool canSacrificeEmptySilos = (pHouse->OwnedTiberium.GetTotalAmount() <= 0);
	int ownedRefineries = 0;

	for (auto const pOwnBld : pHouse->Buildings)
	{
		if (pOwnBld && pOwnBld->IsAlive && !pOwnBld->InLimbo && pOwnBld->Type
			&& (pOwnBld->Type->Refinery || pOwnBld->Type->ResourceDestination))
		{
			ownedRefineries++;
		}
	}

	const CellStruct baseCenter = (pHouse->ConYards.Count > 0 && pHouse->ConYards.GetItem(0))
		? pHouse->ConYards.GetItem(0)->GetMapCoords()
		: (pHouse->BaseCenter != CellStruct::Empty ? pHouse->BaseCenter : pHouse->BaseSpawnCell);

	if (baseCenter == CellStruct::Empty)
		return 0;

	const int adjRange = std::max(1, pBuildingType->Adjacent + 1);
	CellStruct bestClearanceCell = CellStruct::Empty;

	std::vector<BuildingClass*> bestToSell;

	int bestSacrificeCost = 999999;

	for (int dy = -20; dy <= 20; ++dy)
	{
		for (int dx = -20; dx <= 20; ++dx)
		{
			const CellStruct cand { static_cast<short>(baseCenter.X + dx), static_cast<short>(baseCenter.Y + dy) };

			if (!MapClass::Instance.CoordinatesLegal(cand))
				continue;

			if (OverlapsTiberiumTreeZone(cand, pBuildingType) || OverlapsBridge(cand, pBuildingType))
				continue;

			bool terrainAndUnitsValid = true;

			std::vector<BuildingClass*> obstructingBlds;

			int sacrificeScore = 0;
			int lostPowerBonus = 0;

			for (int fy = 0; fy < h && terrainAndUnitsValid; ++fy)
			{
				for (int fx = 0; fx < w; ++fx)
				{
					const CellStruct fc { static_cast<short>(cand.X + fx), static_cast<short>(cand.Y + fy) };
					auto const pCell = MapClass::Instance.TryGetCellAt(fc);

					if (!pCell || pCell->GetUnit(false) || pCell->GetInfantry(false))
					{
						terrainAndUnitsValid = false;

						break;
					}

					if (pBuildingType->Naval)
					{
						if (pCell->LandType != LandType::Water)
						{
							terrainAndUnitsValid = false;

							break;
						}
					}
					else if (pCell->LandType == LandType::Water || pCell->LandType == LandType::Beach
						|| pCell->LandType == LandType::Rock || pCell->ContainsBridge())
					{
						terrainAndUnitsValid = false;

						break;
					}

					if (pCell->OverlayTypeIndex >= 0)
					{
						if (auto const pOv = OverlayTypeClass::Array.GetItem(pCell->OverlayTypeIndex))
						{
							if (pOv->Wall && pCell->WallOwnerIndex != pHouse->ArrayIndex)
							{
								terrainAndUnitsValid = false;

								break;
							}
						}
					}

					if (auto const pOcc = pCell->GetBuilding())
					{
						if (pOcc->Owner != pHouse || !pOcc->IsAlive || pOcc->InLimbo || !pOcc->Type
							|| pOcc->Type->ToProtect || pOcc->Type->ConstructionYard
							|| pOcc->CurrentMission == Mission::Construction
							|| pOcc->CurrentMission == Mission::Selling)
						{
							terrainAndUnitsValid = false;

							break;
						}

						if (std::find(obstructingBlds.begin(), obstructingBlds.end(), pOcc) == obstructingBlds.end())
						{
							auto const pOT = pOcc->Type;
							int itemCost = -1;

							// Priority 0: Obstructing AI-owned Wall / LaserFence / FirestormWall
							if (pOT->Wall || pOT->FirestormWall || pOT->LaserFence || pOT->LaserFencePost)
							{
								itemCost = 10;
							}

							// Priority 1: Exhausted or redundant idle Refinery
							else if ((pOT->Refinery || pOT->ResourceDestination) && !pOT->ResourceGatherer
								&& !pOT->UndeploysInto && !pOcc->HasAnyLink()
								&& (IsRefineryExhausted(pOcc, baseCenter)
									|| (ownedRefineries > 2 && pBuildingType->Refinery)))
							{
								itemCost = 100;
							}

							// Priority 2: Empty Ore Silo (STRICTLY ONLY when stored Tiberium
							// == 0!)
							else if (pOT->Storage > 0 && !pOT->Refinery && !pOT->ResourceGatherer
								&& canSacrificeEmptySilos)
							{
								itemCost = 250;
							}

							// Priority 3: Redundant Basic Power Plant when Advanced Reactors
							// exist and power cushion stays safe
							else if (pOT->PowerBonus > pOT->PowerDrain && pOT->PowerBonus <= 200 && !pOT->IsBaseDefense
								&& hasAdvancedPowerPlants
								&& (realSurplus - (lostPowerBonus + pOT->PowerBonus) - pBuildingType->PowerDrain)
									>= requiredCushion)
							{
								itemCost = 400;

								lostPowerBonus += pOT->PowerBonus;
							}

							// Priority 4: Obstructing Tier-1 Defense (for non-defense
							// infrastructure or late-game Tier-2/3 defense upgrade)
							else if (pOT->IsBaseDefense && !IsTunnelNetworkBuilding(pOT)
								&& (!isCombatDef
									|| (isLateGameHighTierDefense && pBuildingType->Cost >= pOT->Cost + 450)))
							{
								itemCost = 600 + (pOT->Cost / 4);
							}

							if (itemCost < 0)
							{
								terrainAndUnitsValid = false;

								break;
							}

							obstructingBlds.push_back(pOcc);

							sacrificeScore += itemCost;
						}
					}
					else if (!pCell->CanThisExistHere(pBuildingType->SpeedType, pBuildingType, pHouse))
					{
						terrainAndUnitsValid = false;

						break;
					}
				}
			}

			if (!terrainAndUnitsValid || obstructingBlds.empty() || obstructingBlds.size() > 2)
				continue;

			// Verify that after selling obstructingBlds, cand still has at least one
			// surviving BaseNormal / ConstructionYard anchor within adjRange!
			bool hasSurvivingAnchor = false;

			for (auto const pOwnBld : pHouse->Buildings)
			{
				if (!pOwnBld || !pOwnBld->IsAlive || pOwnBld->InLimbo || !pOwnBld->Type)
					continue;

				if (std::find(obstructingBlds.begin(), obstructingBlds.end(), pOwnBld) != obstructingBlds.end())
					continue;

				if (!pOwnBld->Type->BaseNormal && !pOwnBld->Type->ConstructionYard
					&& !BuildingTypeExt::IsAIBaseNormal(pOwnBld->Type))
				{
					continue;
				}

				const CellStruct ac = pOwnBld->GetMapCoords();
				const int aW = pOwnBld->Type->GetFoundationWidth();
				const int aH = pOwnBld->Type->GetFoundationHeight(false);

				const int gapX = std::max(0,
					std::max(static_cast<int>(cand.X) - (static_cast<int>(ac.X) + aW - 1),
						static_cast<int>(ac.X) - (static_cast<int>(cand.X) + w - 1)));

				const int gapY = std::max(0,
					std::max(static_cast<int>(cand.Y) - (static_cast<int>(ac.Y) + aH - 1),
						static_cast<int>(ac.Y) - (static_cast<int>(cand.Y) + h - 1)));

				if (std::max(gapX, gapY) <= adjRange)
				{
					hasSurvivingAnchor = true;

					break;
				}
			}

			if (!hasSurvivingAnchor)
				continue;

			const int totalScore = sacrificeScore + static_cast<int>(obstructingBlds.size()) * 500
				+ (std::abs(cand.X - baseCenter.X) + std::abs(cand.Y - baseCenter.Y)) * 4;

			if (totalScore < bestSacrificeCost)
			{
				bestSacrificeCost = totalScore;

				bestClearanceCell = cand;

				bestToSell = std::move(obstructingBlds);
			}
		}
	}

	if (bestClearanceCell != CellStruct::Empty && !bestToSell.empty())
	{
		for (auto const pObs : bestToSell)
		{
			if (pObs && pObs->IsAlive && !pObs->InLimbo)
			{
				Debug::Log("[NeuralAI] House %d selling obstructing %s at (%d,%d) to "
						"clear space for %s at (%d,%d).\n",
					pHouse->ArrayIndex, pObs->Type->ID, pObs->GetMapCoords().X, pObs->GetMapCoords().Y,
					pBuildingType->ID, bestClearanceCell.X, bestClearanceCell.Y);

				pObs->Sell(1);
			}
		}

		for (int fy = 0; fy < h; ++fy)
		{
			for (int fx = 0; fx < w; ++fx)
			{
				const CellStruct fc { static_cast<short>(bestClearanceCell.X + fx),
					static_cast<short>(bestClearanceCell.Y + fy) };

				if (auto const pCell = MapClass::Instance.TryGetCellAt(fc))
				{
					if (pCell->OverlayTypeIndex >= 0 && pCell->WallOwnerIndex == pHouse->ArrayIndex)
					{
						if (auto const pOv = OverlayTypeClass::Array.GetItem(pCell->OverlayTypeIndex))
						{
							if (pOv->Wall)
							{
								CellStruct wallCell = fc;
								pHouse->SellWall(wallCell, true);
							}
						}
					}

					pCell->ScatterContent(CellClass::Cell2Coord(fc), true, true, false);
				}
			}
		}

		GetMacroState(pHouse).PendingClearanceCell = bestClearanceCell;

		GetMacroState(pHouse).PendingClearanceType = pBuildingType;

		GetMacroState(pHouse).PendingClearanceFoundationWidth = w;

		GetMacroState(pHouse).PendingClearanceFoundationHeight = h;

		GetMacroState(pHouse).PendingClearanceFoundation = pBuildingType->Foundation;

		GetMacroState(pHouse).ClearanceAttempts = 0;

		GetMacroState(pHouse).NextClearanceCheckFrame = Unsorted::CurrentFrame + 65;

		return 1; // Keep building waiting in Construction Yard while obstructing

		// structure sells!
	}

	GetMacroState(pHouse).LastClearanceFailedFrame = Unsorted::CurrentFrame;

	return recordPlacementFailure();
}


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
}

bool NeuralAI::IsCellClearedOfExitCorridors(HouseClass* pHouse, BuildingTypeClass* pType, CellStruct candidateCell)
{
	const int candW = pType->GetFoundationWidth();
	const int candH = pType->GetFoundationHeight(false);
	const bool isCandDefense = pType->IsBaseDefense;

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
	const bool placingWarFactory = (pBuildingType->Factory == AbstractType::UnitType && !pBuildingType->Naval);
	const bool placingBarracks = (pBuildingType->Factory == AbstractType::InfantryType);

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

				if (pCheckB->Type->ConstructionYard)
					continue;

				const CellStruct cbc = pCheckB->GetMapCoords();
				if (MapClass::Instance.GetMovementZoneType(cbc, MovementZone::Normal, pCheckB->OnBridge) == bz
					&& std::max(std::abs(bc.X - cbc.X), std::abs(bc.Y - cbc.Y)) <= 18)
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

			const bool missingThisCoreBld = (placingWarFactory && !hasLocalWarFac)
				|| (placingBarracks && !hasLocalBarracks)
				|| (isRefineryOrSlaveMiner && (!hasLocalRef || (localRefs == 1 && hasLocalWarFac)))
				|| (pBuildingType->IsBaseDefense && localDefs < 2);
			const bool disperseFactoryHere = homeSameKindFactories >= 2 && localSameKindFactories < homeSameKindFactories;

			if (localHubBlds < 18 || missingThisCoreBld || disperseFactoryHere)
			{
				int hubScore = (18 - std::min(18, localHubBlds)) * 10 + (pOwnBld->Type->ConstructionYard ? 50 : 0)
					+ (enemyInHubZone ? 250 : 0) + (missingThisCoreBld ? 500 : 0)
					+ (disperseFactoryHere ? 300 + (homeSameKindFactories - localSameKindFactories) * 100 : 0);
				if (hubScore > bestHubPriorityScore)
				{
					bestHubPriorityScore = hubScore;
					center = bc;
					targetExpansionZone = bz;
				}
			}
		}
	}

	// Allied Base-Sharing (BuildOffAlly=yes): If we lost our ConYard OR an Ally's
	// ConYard is under attack (< 80% HP) while placing a Base Defense, build
	// around our Ally's ConYard!
	if (GameModeOptionsClass::Instance.BuildOffAlly && (pHouse->ConYards.Count == 0 || pBuildingType->IsBaseDefense))
	{
		for (auto const pAllyBld : BuildingClass::Array)
		{
			if (!pAllyBld || pAllyBld->InLimbo || !pAllyBld->IsAlive || !pAllyBld->Owner || !pAllyBld->Type)
				continue;

			if (pAllyBld->Owner != pHouse && pHouse->IsAlliedWith(pAllyBld->Owner) && !pAllyBld->Owner->IsNeutral()
				&& pAllyBld->Type->EligibileForAllyBuilding)
			{
				if (pHouse->ConYards.Count == 0
					|| (pBuildingType->IsBaseDefense && pAllyBld->HasBeenAttacked
						&& pAllyBld->GetHealthPercentage() < 0.80))
				{
					center = pAllyBld->GetMapCoords();

					break;
				}
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

	const int turretCreepBias
		= static_cast<int>(policy.OutputBias[38]) + ((pExt && pExt->NeuralAI_ActiveExpert == 7) ? 220 : 0);

	// Discover nearest uncolonized Ore/Tiberium patch across an 84-cell radius
	// (for Expert 6, Slot 34 & AdvAI Stepping-Stone Base-Crawling). Measure
	// distance from our closest BaseNormal/ConYard anchor and prefer uncolonized
	// Ore patches >= 10 cells from any existing Refinery, outside the ore blocks we
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

				if (std::max(std::abs(tc.X - rc.X), std::abs(tc.Y - rc.Y)) < 10)
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
			const int oreMetric = minAnchorManhattan - fieldCells / 2 - (hasSpawnerTree ? 10 : 0) + (isWorthyField ? 0 : 1000);

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

	if (nearestOreCell == CellStruct::Empty && smallPatchCell != CellStruct::Empty)
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
	const bool isSteppingStoneExpander = pBuildingType->BaseNormal && isPowerPlantBld && isCheapStone && (pHouse->PowerOutput >= 150)
		&& (nearestOreCell != CellStruct::Empty || crawlBias > 0);
	const bool isRearProtected = !isNavalBld && !isResourceBld && !isCombatDefense && !isFactoryOrBarracks
		&& !isTunnelBld && !isSupportRadiusBld && !isSteppingStoneExpander;

	// Offensive base crawl (asphyxiation, AdvAI's combat crawling target): once every worthy ore
	// field in reach is ours, stepping stones march toward the enemy base instead of clumping
	// around colonized ore; while free fields remain, one stone in four still goes toward the
	// enemy (two in four with a strong learned creep preference: slot 38 Forward Turret Creep +
	// half of slot 81 Nomadic Siege Creep) so the front moves too. Defenses then anchor at the
	// most forward structure (turret creep). Stones stay out of reach of known enemy structures.
	const AIBackpackFile& crawlPolicy = NeuralAI::PolicyFor(pHouse);
	const int offensiveCrawlBias
		= static_cast<int>(crawlPolicy.OutputBias[38]) + static_cast<int>(crawlPolicy.OutputBias[81]) / 2;
	const int stoneIndex = isSteppingStoneExpander ? pHouse->CountOwnedNow(pBuildingType) : 0;
	const bool stoneTowardEnemy = isSteppingStoneExpander && enemyTargetCell != CellStruct::Empty
		&& offensiveCrawlBias > -400
		&& (!hasUncolonizedOre || (stoneIndex % 4) == 3 || (offensiveCrawlBias > 300 && (stoneIndex % 4) == 1));
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
		// with fewer than 4 local ground defenses (Base-Crawling Siege & Resource Choke Harassment!):
		if (!isPureAA && directionalAttackedBld == CellStruct::Empty)
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
		|| ((isResourceBld || isFactoryOrBarracks)
			&& (crawlBias > -20 || bestOreDist > 10 || pHouse->CountResourceDestinations >= 1))
		|| (isCombatDefense && !isPureAA && (turretCreepBias > -20 || crawlBias > 0 || totalOwnedDefensesCount >= 1)
			&& directionalAttackedBld == CellStruct::Empty);

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

	// Strategic plan anchors (UpdateStrategicPlans): defenses of a new island ConYard go next to
	// it (Island Hold), a second repair station next to our structure closest to the front
	// (Forward Repair). A defense needed where we are being attacked keeps priority.
	{
		auto const& planMacro = GetMacroState(pHouse);

		const int mainAttackSector = isCombatDefense ? GetMainAttackSector(pHouse) : -1;

		if (isCombatDefense && directionalAttackedBld == CellStruct::Empty
			&& planMacro.IslandHoldUntilFrame > Unsorted::CurrentFrame && planMacro.IslandHoldCell != CellStruct::Empty)
		{
			center = planMacro.IslandHoldCell;
		}
		else if (mainAttackSector >= 0 && directionalAttackedBld == CellStruct::Empty
			&& center.DistanceFrom(GetHouseHomeCell(pHouse)) <= 24.0) // Forward turret creep keeps its own anchor.
		{
			// Attack route memory: fortify the side we have really been attacked from, anchored
			// at our structure closest to that side of the base.
			const CellStruct home = GetHouseHomeCell(pHouse);
			const int reach = (AttackSectorDX[mainAttackSector] != 0 && AttackSectorDY[mainAttackSector] != 0) ? 10 : 14;
			const CellStruct routePoint { static_cast<short>(home.X + AttackSectorDX[mainAttackSector] * reach),
				static_cast<short>(home.Y + AttackSectorDY[mainAttackSector] * reach) };
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

				if (bc.DistanceFrom(home) > 24.0
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

		if (GameModeOptionsClass::Instance.BuildOffAlly && !isRefineryOrSlaveMiner)
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

				// Naval structures only on useful water (no enclosed ponds).
				if (isNavalBld && !IsUsefulWaterCell(candidate))
					continue;

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

				const CellStruct pullGoal
					= (isSteppingStoneExpander && !isResourceBld && stoneGoal != CellStruct::Empty) ? stoneGoal : nearestOreCell;

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
					else if (isGroundDefense && enemyTargetCell != CellStruct::Empty)
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
		st.NextHarvesterRecoveryFrame = Unsorted::CurrentFrame + 750;
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
	const CellStruct bestCell = NeuralAI::FindSpatialTetrisBuildCell(pHouse, pBuildingType);

	if (bestCell != CellStruct::Empty)
	{
		auto& st = GetMacroState(pHouse);
		st.ConsecutivePlacementFailures = 0;
		st.BuildingPlacementCooldownUntilFrame.erase(pBuildingType->ArrayIndex);
		if (pBuildingType->IsBaseDefense)
		{
			bool defAntiInf = false;
			bool defAntiArmor = false;
			bool defAntiAir = false;
			bool defPureAA = false;
			bool defUnarmedBunker = false;
			GetDefenseCapabilities(pBuildingType, defAntiInf, defAntiArmor, defAntiAir, defPureAA, defUnarmedBunker);

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
			st.NextHarvesterRecoveryFrame = Unsorted::CurrentFrame + 750;
			Debug::Log("[NeuralAI] HarvesterRecovery: House %d placed temporary recovery "
					"refinery %s at (%d,%d). Will sell once FreeUnit harvester spawns.\n",
				pHouse->ArrayIndex, pBuildingType->ID, bestCell.X, bestCell.Y);
		}

		outCell = bestCell;
		return 2;
	}

	// Fallback for Refinery / Harvester Recovery when no free cell exists: sell
	// an existing Refinery and place in its exact spot!
	if (isStaticRefineryWithFreeHarv && tryReplaceExistingRefineryForHarvester())
		return 1;

	// Step 3: Surgical Space Clearance & Obsolete Structure Replacement when the
	// base is congested and no free cell exists!
	auto recordPlacementFailure = [&]() -> int
	{
		auto& st = GetMacroState(pHouse);
		st.ConsecutivePlacementFailures++;
		const int cdUntil = Unsorted::CurrentFrame + std::min(3600, 300 * st.ConsecutivePlacementFailures);
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


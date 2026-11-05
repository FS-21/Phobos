// NeuralAI.Expansion.inl: Map-wide ore fields, useful water bodies and naval need.
// Part of NeuralAI.cpp (included in order; one translation unit).

namespace
{
	// Map-wide ore fields (shared by every house, map state only, so deterministic): 12x12
	// blocks with >= 4 ore cells, stored as the ore cell nearest the block's ore centroid and
	// refreshed every 900 frames, so expansion can target fields beyond the local scan radius.
	struct NeuralOreField
	{
		CellStruct Cell { CellStruct::Empty };
		int OreCells { 0 };
		bool SpawnerTree { false };
		int BlockKey { -1 };
	};

	std::vector<NeuralOreField> OreFieldCache;
	int OreFieldCacheFrame { -100000 };

	// Key of the 12x12 ore block a cell falls in (the grid GetOreFields scans), -1 off the grid.
	int OreBlockKey(const CellStruct& cell)
	{
		LTRBStruct bounds = MapClass::Instance.MapCoordBounds;

		if (bounds.Right <= bounds.Left || bounds.Bottom <= bounds.Top)
			bounds = LTRBStruct { 0, 0, 511, 511 };

		const int left = std::max(0, bounds.Left);
		const int top = std::max(0, bounds.Top);

		if (cell.X < left || cell.Y < top)
			return -1;

		return ((cell.X - left) / 12) * 4096 + (cell.Y - top) / 12;
	}

	const std::vector<NeuralOreField>& GetOreFields()
	{
		if (Unsorted::CurrentFrame - OreFieldCacheFrame < 900 && OreFieldCacheFrame <= Unsorted::CurrentFrame)
			return OreFieldCache;

		OreFieldCacheFrame = Unsorted::CurrentFrame;
		OreFieldCache.clear();

		LTRBStruct bounds = MapClass::Instance.MapCoordBounds;

		if (bounds.Right <= bounds.Left || bounds.Bottom <= bounds.Top)
			bounds = LTRBStruct { 0, 0, 511, 511 };

		for (int by = std::max(0, bounds.Top); by <= bounds.Bottom; by += 12)
		{
			for (int bx = std::max(0, bounds.Left); bx <= bounds.Right; bx += 12)
			{
				int cells = 0;
				int sumX = 0;
				int sumY = 0;
				bool tree = false;

				for (int y = by; y < by + 12; ++y)
				{
					for (int x = bx; x < bx + 12; ++x)
					{
						auto const pCell = MapClass::Instance.TryGetCellAt(CellStruct { static_cast<short>(x), static_cast<short>(y) });

						if (!pCell)
							continue;

						if (pCell->GetContainedTiberiumValue() > 0)
						{
							cells++;
							sumX += x;
							sumY += y;
						}

						if (!tree)
						{
							if (auto const pTerrain = pCell->GetTerrain(false))
								tree = pTerrain->IsAlive && pTerrain->Type && pTerrain->Type->SpawnsTiberium;
						}
					}
				}

				if (cells < 4)
					continue;

				const int cx = sumX / cells;
				const int cy = sumY / cells;
				CellStruct best = CellStruct::Empty;
				int bestDist = INT_MAX;

				for (int y = by; y < by + 12; ++y)
				{
					for (int x = bx; x < bx + 12; ++x)
					{
						auto const pCell = MapClass::Instance.TryGetCellAt(CellStruct { static_cast<short>(x), static_cast<short>(y) });

						if (pCell && pCell->GetContainedTiberiumValue() > 0)
						{
							const int d = std::abs(x - cx) + std::abs(y - cy);

							if (d < bestDist)
							{
								bestDist = d;
								best = CellStruct { static_cast<short>(x), static_cast<short>(y) };
							}
						}
					}
				}

				OreFieldCache.push_back({ best, cells, tree, OreBlockKey(CellStruct { static_cast<short>(bx), static_cast<short>(by) }) });
			}
		}

		return OreFieldCache;
	}

	// Nearest free ore field anywhere on the map for this house: same movement zone as `from`
	// (when zone >= 0), at least minDist cells away, no own refinery / resource destination /
	// ConYard within 14 cells (unless ownHubsOk: harvesters may use our own fields) and no
	// known enemy structure within 12. Spawner trees count the field as 10 cells closer, bigger
	// fields up to 4 cells closer.
	CellClass* FindFreeOreFieldCell(HouseClass* pHouse, const CellStruct& from, int zone, int minDist, bool ownHubsOk = false)
	{
		if (!pHouse || from == CellStruct::Empty)
			return nullptr;

		std::vector<CellStruct> ownHubs;
		std::vector<CellStruct> enemyStructures;

		for (auto const pBld : BuildingClass::Array)
		{
			if (!pBld || !pBld->IsAlive || pBld->InLimbo || !pBld->Type || !pBld->Owner)
				continue;

			if (pBld->Owner == pHouse)
			{
				if (!ownHubsOk && (pBld->Type->Refinery || pBld->Type->ResourceDestination || pBld->Type->ConstructionYard))
					ownHubs.push_back(pBld->GetMapCoords());
			}
			else if (!pHouse->IsAlliedWith(pBld->Owner) && !pBld->Owner->IsNeutral() && IsEnemyBuildingKnown(pHouse, pBld))
			{
				enemyStructures.push_back(pBld->GetMapCoords());
			}
		}

		CellClass* pBest = nullptr;
		int bestMetric = INT_MAX;

		for (auto const& field : GetOreFields())
		{
			const int dist = std::max(std::abs(field.Cell.X - from.X), std::abs(field.Cell.Y - from.Y));

			if (dist < minDist)
				continue;

			const int metric = dist - (field.SpawnerTree ? 10 : 0) - std::min(4, field.OreCells / 10);

			if (metric >= bestMetric)
				continue;

			if (zone >= 0 && MapClass::Instance.GetMovementZoneType(field.Cell, MovementZone::Normal, false) != zone)
				continue;

			bool taken = false;

			for (auto const& hub : ownHubs)
			{
				if (std::max(std::abs(hub.X - field.Cell.X), std::abs(hub.Y - field.Cell.Y)) <= 14)
				{
					taken = true;
					break;
				}
			}

			for (size_t e = 0; !taken && e < enemyStructures.size(); ++e)
			{
				if (std::max(std::abs(enemyStructures[e].X - field.Cell.X), std::abs(enemyStructures[e].Y - field.Cell.Y)) <= 12)
					taken = true;
			}

			if (taken)
				continue;

			if (auto const pCell = MapClass::Instance.TryGetCellAt(field.Cell))
			{
				bestMetric = metric;
				pBest = pCell;
			}
		}

		return pBest;
	}

	// Ore blocks this house already mines: fields (GetOreFields) with one of its refineries or
	// resource destinations within 14 cells. Sorted keys for IsOreCellInHeldField. Without it the
	// edge of a field we already mine (10+ cells from its refinery) looked like a new field, so
	// the base never went for real new ones. A ConYard alone does not hold its field: an
	// expansion yard still needs its refinery there.
	std::vector<int> GetHeldOreBlocks(HouseClass* pHouse)
	{
		std::vector<int> held;

		if (!pHouse)
			return held;

		std::vector<CellStruct> hubs;

		for (auto const pBld : pHouse->Buildings)
		{
			if (pBld && pBld->IsAlive && !pBld->InLimbo && pBld->Type
				&& (pBld->Type->Refinery || pBld->Type->ResourceDestination))
			{
				hubs.push_back(pBld->GetMapCoords());
			}
		}

		for (auto const& field : GetOreFields())
		{
			const bool isHeld = std::any_of(hubs.begin(), hubs.end(), [&field](const CellStruct& hub)
				{ return std::max(std::abs(hub.X - field.Cell.X), std::abs(hub.Y - field.Cell.Y)) <= 14; });

			if (isHeld)
				held.push_back(field.BlockKey);
		}

		// The block a refinery stands in is held too, even when it was placed at the edge of the
		// block far from the field's centre (more refineries kept piling up there).
		for (auto const& hub : hubs)
			held.push_back(OreBlockKey(hub));

		std::sort(held.begin(), held.end());
		held.erase(std::unique(held.begin(), held.end()), held.end());

		return held;
	}

	bool IsOreCellInHeldField(const std::vector<int>& heldBlocks, const CellStruct& cell)
	{
		return std::binary_search(heldBlocks.begin(), heldBlocks.end(), OreBlockKey(cell));
	}

	// Water bodies (map state, computed once per match): cells per water movement zone. Useful
	// water = >= 150 cells and >= 1/5 of the largest body: the open sea or a big lake, never an
	// enclosed pond where a shipyard only traps its ships.
	std::map<int, int> WaterZoneCells;
	int LargestWaterZoneCells { -1 };

	bool IsUsefulWaterZone(int waterZone)
	{
		if (LargestWaterZoneCells < 0)
		{
			LargestWaterZoneCells = 0;
			LTRBStruct bounds = MapClass::Instance.MapCoordBounds;

			if (bounds.Right <= bounds.Left || bounds.Bottom <= bounds.Top)
				bounds = LTRBStruct { 0, 0, 511, 511 };

			for (int y = std::max(0, bounds.Top); y <= bounds.Bottom; ++y)
			{
				for (int x = std::max(0, bounds.Left); x <= bounds.Right; ++x)
				{
					const CellStruct c { static_cast<short>(x), static_cast<short>(y) };
					auto const pCell = MapClass::Instance.TryGetCellAt(c);

					if (!pCell || pCell->LandType != LandType::Water)
						continue;

					const int zone = MapClass::Instance.GetMovementZoneType(c, MovementZone::Water, false);

					if (zone > 1)
						WaterZoneCells[zone]++;
				}
			}

			for (auto const& [zone, cells] : WaterZoneCells)
				LargestWaterZoneCells = std::max(LargestWaterZoneCells, cells);
		}

		auto const it = WaterZoneCells.find(waterZone);

		if (it == WaterZoneCells.end())
			return false;

		return it->second >= 150 && it->second * 5 >= LargestWaterZoneCells;
	}

	bool IsUsefulWaterCell(const CellStruct& cell)
	{
		return IsUsefulWaterZone(MapClass::Instance.GetMovementZoneType(cell, MovementZone::Water, false));
	}

	int CountFreeOreFields(HouseClass* pHouse, const CellStruct& from, int zone)
	{
		if (!pHouse || from == CellStruct::Empty)
			return 0;

		std::vector<CellStruct> ownHubs;

		for (auto const pBld : pHouse->Buildings)
		{
			if (pBld && pBld->IsAlive && !pBld->InLimbo && pBld->Type
				&& (pBld->Type->Refinery || pBld->Type->ResourceDestination || pBld->Type->ConstructionYard))
			{
				ownHubs.push_back(pBld->GetMapCoords());
			}
		}

		int count = 0;

		for (auto const& field : GetOreFields())
		{
			if (zone >= 0 && MapClass::Instance.GetMovementZoneType(field.Cell, MovementZone::Normal, false) != zone)
				continue;

			const bool taken = std::any_of(ownHubs.begin(), ownHubs.end(), [&field](const CellStruct& hub)
				{ return std::max(std::abs(hub.X - field.Cell.X), std::abs(hub.Y - field.Cell.Y)) <= 14; });

			if (!taken)
				count++;
		}

		return count;
	}

	// A navy is a need, not a habit: a known enemy across water (or, with no enemy known yet, starting
	// bases not linked by land), an enemy warship seen on useful water, or free ore fields only
	// reachable across water. Cached every 900 frames.
	bool HouseNeedsNaval(HouseClass* pHouse)
	{
		if (!pHouse)
			return false;

		auto& macro = GetMacroState(pHouse);

		if (Unsorted::CurrentFrame - macro.NavalNeedFrame < 900 && macro.NavalNeedFrame <= Unsorted::CurrentFrame)
			return macro.NavalNeed;

		macro.NavalNeedFrame = Unsorted::CurrentFrame;
		CellStruct home = pHouse->BaseSpawnCell;

		if (pHouse->ConYards.Count > 0 && pHouse->ConYards.GetItem(0))
			home = pHouse->ConYards.GetItem(0)->GetMapCoords();

		const int homeZone = home != CellStruct::Empty ? MapClass::Instance.GetMovementZoneType(home, MovementZone::Normal, false) : -1;
		bool need = false;
		bool knowsEnemy = false;

		for (auto const pBld : BuildingClass::Array)
		{
			if (!pBld || !pBld->IsAlive || pBld->InLimbo || !pBld->Owner || pBld->Owner == pHouse || pHouse->IsAlliedWith(pBld->Owner)
				|| pBld->Owner->IsNeutral() || !IsEnemyBuildingKnown(pHouse, pBld))
			{
				continue;
			}

			knowsEnemy = true;

			if (homeZone > 1 && MapClass::Instance.GetMovementZoneType(pBld->GetMapCoords(), MovementZone::Normal, pBld->OnBridge) != homeZone)
			{
				need = true;
				break;
			}
		}

		if (!need && !knowsEnemy && MapInfo.Computed)
			need = !MapInfo.LandLinked;

		for (size_t u = 0; !need && u < static_cast<size_t>(UnitClass::Array.Count); ++u)
		{
			auto const pUnit = UnitClass::Array.GetItem(static_cast<int>(u));

			if (pUnit && pUnit->IsAlive && !pUnit->InLimbo && pUnit->Type && pUnit->Type->Naval && pUnit->Owner
				&& pUnit->Owner != pHouse && !pHouse->IsAlliedWith(pUnit->Owner) && !pUnit->Owner->IsNeutral()
				&& IsCellVisibleToAI(pHouse, pUnit->GetCell()) && IsUsefulWaterCell(pUnit->GetMapCoords()))
			{
				need = true;
			}
		}

		if (!need && homeZone > 1 && CountFreeOreFields(pHouse, home, homeZone) == 0 && CountFreeOreFields(pHouse, home, -1) > 0)
			need = true;

		macro.NavalNeed = need;
		return need;
	}

	// Multiplayer start locations are waypoints 0..7 (public map knowledge). While no
	// enemy building is known yet, the AI scouts them nearest-first, exactly like a human
	// checking spawn points: it never learns which one an enemy really took until it
	// sees it. A start counts as checked when it belongs to us or an ally, or once it is
	// in sight with no known enemy building around it.
	constexpr int MaxStartWaypoints = 8;

} // namespace

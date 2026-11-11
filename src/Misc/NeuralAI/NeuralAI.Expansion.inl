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

	// Team blackboard (NeuralAI.Team.inl): a teammate already holds this field.
	bool IsOreFieldTakenByTeam(HouseClass* pHouse, const NeuralOreField& field);

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

	// The scan of one row of 12x12 blocks, appended to `out`.
	void ScanOreFieldBlockRow(int by, const LTRBStruct& bounds, std::vector<NeuralOreField>& out)
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

			out.push_back({ best, cells, tree, OreBlockKey(CellStruct { static_cast<short>(bx), static_cast<short>(by) }) });
		}
	}

	// Refresh in progress: the next block row to scan, the fields found so far, the last frame a
	// slice ran and the match the cache belongs to.
	std::vector<NeuralOreField> OreFieldBuild;
	int OreFieldBuildRow { -1 };
	int OreFieldBuildSliceFrame { -100000 };
	int OreFieldCacheSerial { -1 };

	const std::vector<NeuralOreField>& GetOreFields()
	{
		LTRBStruct bounds = MapClass::Instance.MapCoordBounds;

		if (bounds.Right <= bounds.Left || bounds.Bottom <= bounds.Top)
			bounds = LTRBStruct { 0, 0, 511, 511 };

		const int now = Unsorted::CurrentFrame;
		const int top = std::max(0, bounds.Top);

		// New match: one full scan, so nothing reads the previous map's fields.
		if (OreFieldCacheSerial != NeuralMatchSerial || now < OreFieldCacheFrame)
		{
			OreFieldCacheSerial = NeuralMatchSerial;
			OreFieldCacheFrame = now;
			OreFieldBuildRow = -1;
			OreFieldBuild.clear();
			OreFieldCache.clear();

			for (int by = top; by <= bounds.Bottom; by += 12)
				ScanOreFieldBlockRow(by, bounds, OreFieldCache);

			return OreFieldCache;
		}

		// Every 900 frames the map is scanned again, 4 block rows per frame so no single AI tick pays
		// for the whole map; the previous fields serve until the new scan is complete.
		if (OreFieldBuildRow < 0)
		{
			if (now - OreFieldCacheFrame < 900)
				return OreFieldCache;

			OreFieldBuildRow = top;
			OreFieldBuild.clear();
		}

		if (OreFieldBuildSliceFrame == now)
			return OreFieldCache;

		OreFieldBuildSliceFrame = now;

		for (int rows = 0; rows < 4 && OreFieldBuildRow <= bounds.Bottom; ++rows, OreFieldBuildRow += 12)
			ScanOreFieldBlockRow(OreFieldBuildRow, bounds, OreFieldBuild);

		if (OreFieldBuildRow > bounds.Bottom)
		{
			OreFieldCache.swap(OreFieldBuild);
			OreFieldBuild.clear();
			OreFieldCacheFrame = now;
			OreFieldBuildRow = -1;
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

			// Expansion use: not a field a teammate already holds (harvesters may share it).
			if (taken || (!ownHubsOk && IsOreFieldTakenByTeam(pHouse, field)))
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

		if (macro.NavalRemnantUntil > Unsorted::CurrentFrame || macro.SubThreatUntil > Unsorted::CurrentFrame)
			return true;

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

	// Island ore outlook, every 900 frames: how long the ore of the landmasses we hold (a ConYard or
	// a refinery there) lasts, and the nearest free ore field on another landmass. On a map of small
	// islands the income is spent as it comes in, and once the island's ore is gone there is no money
	// left for the MCV and the transport that would reach the next one (matches #7051 and #7053,
	// Heisenberg: most houses at $1-$15 on a bare island, the other islands' fields untouched; a
	// repro had 2500-4400 credits per 1000 frames for 20000 frames, then a flat 400). The expansion
	// is urgent while that ore lasts less than 20 minutes at the pace it is drained, or holds less
	// than 25000 credits (a small island: planned from the start). Map state, our own objects and the
	// enemy buildings we know, walked in a fixed order: every machine computes the same.
	void UpdateIslandOreOutlook(HouseClass* pHouse)
	{
		if (!pHouse)
			return;

		auto& macro = GetMacroState(pHouse);
		const int now = Unsorted::CurrentFrame;

		if (now - macro.IslandOutlookFrame < 900 && macro.IslandOutlookFrame <= now)
			return;

		const bool wasUrgent = macro.IslandExpansionUrgent;
		macro.IslandOutlookFrame = now;
		macro.IslandExpansionUrgent = false;
		macro.IslandExpansionField = CellStruct::Empty;

		std::vector<int> hubZones;
		CellStruct home = CellStruct::Empty;

		for (auto const pBld : pHouse->Buildings)
		{
			if (!pBld || !pBld->IsAlive || pBld->InLimbo || !pBld->Type
				|| !(pBld->Type->ConstructionYard || pBld->Type->Refinery || pBld->Type->ResourceDestination))
			{
				continue;
			}

			const int zone = MapClass::Instance.GetMovementZoneType(pBld->GetMapCoords(), MovementZone::Normal, pBld->OnBridge);

			if (zone >= 0)
				hubZones.push_back(zone);

			if (home == CellStruct::Empty && pBld->Type->ConstructionYard)
				home = pBld->GetMapCoords();
		}

		std::sort(hubZones.begin(), hubZones.end());
		hubZones.erase(std::unique(hubZones.begin(), hubZones.end()), hubZones.end());

		if (home == CellStruct::Empty || hubZones.empty())
		{
			macro.IslandHomeOre = 0;
			macro.IslandOreDrain = -1;
			macro.IslandOreRunway = -1;
			macro.IslandHubZoneKey = 0;
			return;
		}

		auto const isHubZone = [&hubZones](int zone)
		{
			return std::binary_search(hubZones.begin(), hubZones.end(), zone);
		};

		// Known enemy structures: a field within 12 cells of one is theirs, neither ours to mine nor free.
		std::vector<CellStruct> enemyStructures;

		for (auto const pBld : BuildingClass::Array)
		{
			if (pBld && pBld->IsAlive && !pBld->InLimbo && pBld->Type && pBld->Owner && pBld->Owner != pHouse
				&& !pHouse->IsAlliedWith(pBld->Owner) && !pBld->Owner->IsNeutral() && IsEnemyBuildingKnown(pHouse, pBld))
			{
				enemyStructures.push_back(pBld->GetMapCoords());
			}
		}

		auto const nearEnemy = [&enemyStructures](const CellStruct& cell)
		{
			return std::any_of(enemyStructures.begin(), enemyStructures.end(), [&cell](const CellStruct& e)
				{ return std::max(std::abs(e.X - cell.X), std::abs(e.Y - cell.Y)) <= 12; });
		};

		// Ore left on those landmasses: every field block there, the ones we mine and the free ones a
		// land expansion would take (a field counts by its 12x12 block, so one patch is often two or
		// three blocks and a free block next to our refinery is no new field), but not the enemy's.
		// Meanwhile the nearest free field on any other landmass, the island expansion target (same
		// rules as FindFreeOreFieldCell: no own hub within 14 cells, not a teammate's).
		LTRBStruct bounds = MapClass::Instance.MapCoordBounds;

		if (bounds.Right <= bounds.Left || bounds.Bottom <= bounds.Top)
			bounds = LTRBStruct { 0, 0, 511, 511 };

		const int left = std::max(0, bounds.Left);
		const int top = std::max(0, bounds.Top);
		int homeOre = 0;
		int homeFields = 0;
		unsigned int countedKey = 2166136261u;
		const NeuralOreField* pIslandField = nullptr;
		int islandFieldZone = -1;
		int islandFieldMetric = INT_MAX;

		for (auto const& field : GetOreFields())
		{
			if (field.BlockKey < 0 || nearEnemy(field.Cell))
				continue;

			const int fieldZone = MapClass::Instance.GetMovementZoneType(field.Cell, MovementZone::Normal, false);

			if (fieldZone < 0)
				continue;

			if (!isHubZone(fieldZone))
			{
				const int dist = std::max(std::abs(field.Cell.X - home.X), std::abs(field.Cell.Y - home.Y));
				const int metric = dist - (field.SpawnerTree ? 10 : 0) - std::min(4, field.OreCells / 10);

				if (metric < islandFieldMetric && !IsOreFieldTakenByTeam(pHouse, field))
				{
					islandFieldMetric = metric;
					pIslandField = &field;
					islandFieldZone = fieldZone;
				}

				continue;
			}

			homeFields++;
			countedKey = (countedKey ^ static_cast<unsigned int>(field.BlockKey)) * 16777619u;
			const int bx = left + (field.BlockKey / 4096) * 12;
			const int by = top + (field.BlockKey % 4096) * 12;

			for (int y = by; y < by + 12; ++y)
			{
				for (int x = bx; x < bx + 12; ++x)
				{
					const CellStruct c { static_cast<short>(x), static_cast<short>(y) };
					auto const pCell = MapClass::Instance.TryGetCellAt(c);

					if (!pCell)
						continue;

					const int value = pCell->GetContainedTiberiumValue();

					if (value > 0 && isHubZone(MapClass::Instance.GetMovementZoneType(c, MovementZone::Normal, false)))
						homeOre += value;
				}
			}
		}

		// Drain per 900 frames from the previous sample of the same field blocks (a new refinery or
		// yard elsewhere, or an enemy base found next to a field, changes what is counted, so that
		// sample is skipped). It is net of what spawner trees grow back and counts what a neighbour
		// on the same island mines too.
		unsigned int zoneKey = countedKey;

		for (const int zone : hubZones)
			zoneKey = (zoneKey ^ static_cast<unsigned int>(zone)) * 16777619u;

		if (macro.IslandHubZoneKey == zoneKey && now >= 1800)
		{
			const int sample = std::max(0, macro.IslandHomeOre - homeOre);
			macro.IslandOreDrain = macro.IslandOreDrain < 0 ? sample : (macro.IslandOreDrain + sample) / 2;
		}

		macro.IslandHubZoneKey = zoneKey;
		macro.IslandHomeOre = homeOre;
		macro.IslandOreRunway = macro.IslandOreDrain > 0
			? static_cast<int>(std::min<int64_t>(INT_MAX, static_cast<int64_t>(homeOre) * 900 / macro.IslandOreDrain))
			: -1;

		// The island field must be free of our own hubs (an outpost refinery there makes it held land).
		if (pIslandField)
		{
			for (auto const pBld : pHouse->Buildings)
			{
				if (pBld && pBld->IsAlive && !pBld->InLimbo && pBld->Type
					&& (pBld->Type->Refinery || pBld->Type->ResourceDestination || pBld->Type->ConstructionYard)
					&& std::max(std::abs(pBld->GetMapCoords().X - pIslandField->Cell.X), std::abs(pBld->GetMapCoords().Y - pIslandField->Cell.Y)) <= 14)
				{
					pIslandField = nullptr;
					break;
				}
			}
		}

		// Urgent while the ore of our land lasts less than 20 minutes at the pace it is drained, or
		// holds less than 25000 credits: such a small island is spent long before its ore runs out (the
		// houses of the Heisenberg repro held 14000-18000 and had no credits left by frame 9000), so
		// its expansion is planned from the start, while the starting credits pay the MCV's
		// prerequisites, the MCV and its ride.
		const bool runningOut = pIslandField
			&& (homeOre < 25000 || (macro.IslandOreRunway >= 0 && macro.IslandOreRunway < 18000));
		macro.IslandExpansionUrgent = runningOut;
		macro.IslandExpansionField = pIslandField ? pIslandField->Cell : CellStruct::Empty;

		if (runningOut != wasUrgent || now - macro.IslandExpansionLogFrame >= (runningOut ? 4500 : 9000))
		{
			macro.IslandExpansionLogFrame = now;
			Debug::Log("[NeuralAI] House %d IslandExpansion: %s, home ore %d in %d fields (drain %d per 900 frames, runway %d frames), "
				"%d landmasses held, free field across water at (%d,%d) zone %d, %d credits, frame %d.\n",
				pHouse->ArrayIndex, runningOut ? "urgent" : "not urgent", homeOre, homeFields, macro.IslandOreDrain, macro.IslandOreRunway,
				static_cast<int>(hubZones.size()), macro.IslandExpansionField.X, macro.IslandExpansionField.Y, islandFieldZone,
				pHouse->Available_Money(), now);
		}
	}

	// Multiplayer start locations are waypoints 0..7 (public map knowledge). While no
	// enemy building is known yet, the AI scouts them nearest-first, exactly like a human
	// checking spawn points: it never learns which one an enemy really took until it
	// sees it. A start counts as checked when it belongs to us or an ally, or once it is
	// in sight with no known enemy building around it.
	constexpr int MaxStartWaypoints = 8;

} // namespace

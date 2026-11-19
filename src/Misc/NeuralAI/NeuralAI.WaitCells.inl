// NeuralAI.WaitCells.inl: one rule set and one ring search for the cells where units wait, park or
// gather (rule consolidation, case 3). Each finder keeps its own scoring and its own extra rules
// (claimed cells, berth spacing, bands, lanes) and asks these for the checks they share.
// Part of NeuralAI.cpp (included in order; one translation unit).

namespace
{
	// Defined in NeuralAI.Tactics.inl.
	bool IsBridgeOrTubeCell(const CellClass* pCell);
	bool IsNarrowPassage(const CellStruct& center);

	// The rules a cell must pass for a unit to wait, park or gather on it; each one is a check one of
	// the finders makes. WaitInMap: the map has the cell; WaitLegal: CoordinatesLegal too;
	// WaitNoBuilding; WaitSurfaceOfAsker: water for a naval asker, else no water; WaitLand: no water;
	// WaitNotBeach; WaitNotRock; WaitZone: the asker's movement zone; WaitLevelNear: within LevelSpan
	// levels of RefLevel; WaitClearIgnoringUnits, WaitClearIgnoringInfantry, WaitClearOfUnits: the
	// three ways the finders ask IsClearToMove; WaitNoObject: nothing stands on it; WaitOffDeck: no
	// bridge or tube cell; WaitOffMouth: no passage mouth (IsPassageMouthCell); WaitOffNarrow: no
	// narrow passage (IsNarrowPassage, the dearest check, made last).
	enum NeuralWaitRule : uint32_t
	{
		WaitInMap = 1u << 0,
		WaitLegal = 1u << 1,
		WaitNoBuilding = 1u << 2,
		WaitSurfaceOfAsker = 1u << 3,
		WaitLand = 1u << 4,
		WaitNotBeach = 1u << 5,
		WaitNotRock = 1u << 6,
		WaitZone = 1u << 7,
		WaitLevelNear = 1u << 8,
		WaitClearIgnoringUnits = 1u << 9,
		WaitClearIgnoringInfantry = 1u << 10,
		WaitClearOfUnits = 1u << 11,
		WaitNoObject = 1u << 12,
		WaitOffDeck = 1u << 13,
		WaitOffMouth = 1u << 14,
		WaitOffNarrow = 1u << 15
	};

	// Who asks: the speed and movement of the unit, its zone, and the level a search keeps to.
	// ZoneMovement: the movement WaitZone reads the zone with when it is not the unit's own (-1: the
	// unit's; a transport sets a load down on the land of its members' feet).
	struct NeuralWaitAsker
	{
		SpeedType Speed { SpeedType::Track };
		MovementZone Movement { MovementZone::Normal };
		int ZoneMovement { -1 };
		int Zone { -1 };
		bool Naval { false };
		int RefLevel { 0 };
		int LevelSpan { 0 };
	};

	// Whether a cell passes the rules. It only reads the map.
	bool IsWaitCellAllowed(const CellStruct& cell, const NeuralWaitAsker& asker, uint32_t rules)
	{
		if ((rules & WaitLegal) && !MapClass::Instance.CoordinatesLegal(cell))
			return false;

		auto const pCell = MapClass::Instance.TryGetCellAt(cell);

		if (!pCell)
			return false;

		if ((rules & WaitNoBuilding) && pCell->GetBuilding())
			return false;

		if ((rules & WaitSurfaceOfAsker) && (pCell->LandType == LandType::Water) != asker.Naval)
			return false;

		if ((rules & WaitLand) && pCell->LandType == LandType::Water)
			return false;

		if ((rules & WaitNotBeach) && pCell->LandType == LandType::Beach)
			return false;

		if ((rules & WaitNotRock) && pCell->LandType == LandType::Rock)
			return false;

		if ((rules & WaitNoObject) && pCell->FirstObject)
			return false;

		if ((rules & WaitOffDeck) && IsBridgeOrTubeCell(pCell))
			return false;

		if ((rules & WaitOffMouth) && IsPassageMouthCell(cell))
			return false;

		if ((rules & WaitZone) && MapClass::Instance.GetMovementZoneType(cell,
			asker.ZoneMovement >= 0 ? static_cast<MovementZone>(asker.ZoneMovement) : asker.Movement, false) != asker.Zone)
		{
			return false;
		}

		if ((rules & WaitLevelNear) && std::abs(pCell->Level - asker.RefLevel) > asker.LevelSpan)
			return false;

		if ((rules & WaitClearIgnoringUnits) && !pCell->IsClearToMove(asker.Speed, true, true, -1, asker.Movement, -1, false))
			return false;

		if ((rules & WaitClearIgnoringInfantry) && !pCell->IsClearToMove(asker.Speed, true, false, -1, asker.Movement, -1, false))
			return false;

		if ((rules & WaitClearOfUnits) && !pCell->IsClearToMove(asker.Speed, false, false, -1, asker.Movement, -1, false))
			return false;

		return !((rules & WaitOffNarrow) && IsNarrowPassage(cell));
	}

	// How a search picks its cell. First: the first allowed cell in the order every finder uses;
	// LowestScore: every ring, the lowest score, the first one on a tie; RingNearest: the ring of the
	// first allowed cell, the one nearest MetricTo in it, the first one on a tie.
	enum class NeuralWaitPick : uint8_t
	{
		First,
		LowestScore,
		RingNearest
	};

	// Step: 2 walks rings and cells on a grid of 2 (FindClearingCell).
	struct NeuralWaitSearch
	{
		int MinRing { 0 };
		int MaxRing { 0 };
		int Step { 1 };
		NeuralWaitPick Pick { NeuralWaitPick::First };
		CellStruct MetricTo { CellStruct::Empty };
	};

	// Rings MinRing..MaxRing (Chebyshev, Step apart) around center in the order the finders use: dy
	// from -r to r, then dx from -r to r, Step apart, the perimeter of each ring only (ring 0 is the
	// center). reject(c, pCell) holds the caller's own rules; score(c, pCell, r, dx, dy) is read for
	// LowestScore only. Null when no cell passes. It only reads.
	template <typename Reject, typename Score>
	CellClass* FindWaitCellNear(const CellStruct& center, const NeuralWaitSearch& search, const NeuralWaitAsker& asker, uint32_t rules,
		Reject&& reject, Score&& score)
	{
		const int step = std::max(1, search.Step);
		CellClass* pBest = nullptr;
		int bestScore = INT_MAX;

		for (int r = std::max(0, search.MinRing); r <= search.MaxRing; r += step)
		{
			double nearest = 1.0e9;

			for (int dy = -r; dy <= r; dy += step)
			{
				for (int dx = -r; dx <= r; dx += step)
				{
					if (std::abs(dx) != r && std::abs(dy) != r)
						continue;

					const CellStruct c { static_cast<short>(center.X + dx), static_cast<short>(center.Y + dy) };

					if (!IsWaitCellAllowed(c, asker, rules))
						continue;

					auto const pCell = MapClass::Instance.TryGetCellAt(c);

					if (reject(c, pCell))
						continue;

					if (search.Pick == NeuralWaitPick::First)
						return pCell;

					if (search.Pick == NeuralWaitPick::RingNearest)
					{
						const double distance = c.DistanceFrom(search.MetricTo);

						if (distance < nearest)
						{
							nearest = distance;
							pBest = pCell;
						}

						continue;
					}

					const int value = score(c, pCell, r, dx, dy);

					if (value < bestScore)
					{
						bestScore = value;
						pBest = pCell;
					}
				}
			}

			if (search.Pick == NeuralWaitPick::RingNearest && pBest)
				return pBest;
		}

		return pBest;
	}

	template <typename Reject>
	CellClass* FindWaitCellNear(const CellStruct& center, const NeuralWaitSearch& search, const NeuralWaitAsker& asker, uint32_t rules,
		Reject&& reject)
	{
		return FindWaitCellNear(center, search, asker, rules, std::forward<Reject>(reject),
			[](const CellStruct&, const CellClass*, int, int, int) { return 0; });
	}

	// TEMP diagnostic (rule consolidation, case 3): a cell a finder chose that FindWaitCellNear does
	// not choose, WaitShadowLogLines a match for each finder. With none over a desync test and some
	// matches, the finder's own search goes.
	void NoteWaitCellShadow(const char* name, int& serial, int& count, const CellStruct& center, const CellClass* pOld,
		const CellClass* pNew)
	{
		if (pOld == pNew)
			return;

		if (serial != NeuralMatchSerial)
		{
			serial = NeuralMatchSerial;
			count = 0;
		}

		if (count++ < WaitShadowLogLines)
		{
			Debug::Log("[NeuralAI] wait cell shadow mismatch %s: old (%d,%d) new (%d,%d) center (%d,%d) frame %d.\n", name,
				pOld ? pOld->MapCoords.X : -1, pOld ? pOld->MapCoords.Y : -1, pNew ? pNew->MapCoords.X : -1,
				pNew ? pNew->MapCoords.Y : -1, center.X, center.Y, Unsorted::CurrentFrame);
		}
	}
} // namespace

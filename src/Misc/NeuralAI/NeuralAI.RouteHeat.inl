// NeuralAI.RouteHeat.inl: Fog-fair memory of the routes enemy ground forces take toward our bases.
// Part of NeuralAI.cpp (included in order; one translation unit).

namespace
{
	// Route heat: a coarse grid per house (RouteHeatBucketCells x RouteHeatBucketCells cells per
	// bucket), fed only by enemy ground units this house sees right now moving toward its
	// structures or attacking them, weighted by unit cost and decayed by 1/16 every
	// RouteHeatDecayFrames (half-life about 4800 frames). Only touched buckets are stored, in an
	// ordered map: updates stay incremental and every client walks them in the same order.
	constexpr int RouteHeatBucketCells = 4;
	constexpr int RouteHeatBucketsPerRow = 512 / RouteHeatBucketCells;
	constexpr int RouteHeatDecayFrames = 450;
	constexpr int RouteHeatRefreshFrames = 300;
	constexpr int RouteHeatPruneFrames = 900;
	constexpr int RouteHeatMaxApproachCells = 48;
	constexpr int RouteHeatMinSpotHeat = 60;
	constexpr int RouteHeatAttackDepositFrames = 150;
	constexpr int RouteHeatMaxDefenseSpots = 8;
	constexpr int RouteHeatMaxAmbushSpots = 6;

	struct NeuralRouteHeatBucket
	{
		int Heat { 0 };
		// Decay is applied up to this frame (lazily, on write and on prune).
		int Frame { 0 };
		// Summed approach direction of the units that crossed it, weighted like the heat.
		int FlowX { 0 };
		int FlowY { 0 };
		// Last cell an enemy unit was really seen in (always a cell a ground unit stood on).
		CellStruct RepCell { CellStruct::Empty };
	};

	constexpr int RouteSpotBridge = 1;
	constexpr int RouteSpotRamp = 2;

	// A hot route segment worth acting on, rebuilt every RouteHeatRefreshFrames.
	struct NeuralRouteSpot
	{
		CellStruct Cell { CellStruct::Empty };
		int Heat { 0 };
		// Narrowest passable width through Cell, in cells (15 = open ground).
		int Width { 15 };
		int Flags { 0 };
		// Chebyshev distance to our nearest structure.
		int BaseDist { 0 };
		int Score { 0 };
		int FlowX { 0 };
		int FlowY { 0 };
		// Own ground towers whose range covers Cell.
		int Cover { 0 };
		int BucketIndex { -1 };
	};

	struct NeuralRouteHeatState
	{
		std::map<int, NeuralRouteHeatBucket> Buckets {};
		// Per enemy unit: frame of its last attack deposit (keyed lookups; pruned by a predicate).
		std::unordered_map<DWORD, int> AttackDepositFrame {};
		// Chokepoints and hot segments close to our structures, best tower spot first.
		std::vector<NeuralRouteSpot> DefenseSpots {};
		// Hot segments further out (picket and ambush posts), hottest first.
		std::vector<NeuralRouteSpot> AmbushSpots {};
		// Towers placed covering a defense spot (tower cell, spot cell), last 16, for telemetry.
		std::vector<std::pair<CellStruct, CellStruct>> RouteTowers {};
		int RefreshFrame { -100000 };
		int PruneFrame { 0 };

		void Reset()
		{
			this->Buckets.clear();
			this->AttackDepositFrame.clear();
			this->DefenseSpots.clear();
			this->AmbushSpots.clear();
			this->RouteTowers.clear();
			this->RefreshFrame = -100000;
			this->PruneFrame = 0;
		}
	};

	std::array<NeuralRouteHeatState, 32> RouteHeatStates {};

	NeuralRouteHeatState& GetRouteHeat(const HouseClass* pHouse)
	{
		const int idx = pHouse ? std::clamp(pHouse->ArrayIndex, 0, 31) : 0;

		return RouteHeatStates[idx];
	}

	int RouteBucketIndex(const CellStruct& cell)
	{
		if (cell.X < 0 || cell.Y < 0 || cell.X >= 512 || cell.Y >= 512)
			return -1;

		return (cell.Y / RouteHeatBucketCells) * RouteHeatBucketsPerRow + cell.X / RouteHeatBucketCells;
	}

	// Heat of a bucket now, without touching it.
	int DecayedRouteHeat(const NeuralRouteHeatBucket& bucket)
	{
		const int steps = (Unsorted::CurrentFrame - bucket.Frame) / RouteHeatDecayFrames;

		if (steps <= 0)
			return bucket.Heat;

		if (steps >= 48)
			return 0;

		int heat = bucket.Heat;

		for (int i = 0; i < steps && heat > 0; ++i)
			heat = heat * 15 / 16;

		return heat;
	}

	void DecayRouteBucket(NeuralRouteHeatBucket& bucket)
	{
		const int now = Unsorted::CurrentFrame;

		if (bucket.Frame > now)
		{
			bucket.Frame = now;

			return;
		}

		const int steps = (now - bucket.Frame) / RouteHeatDecayFrames;

		if (steps <= 0)
			return;

		if (steps >= 48)
		{
			bucket.Heat = 0;
			bucket.FlowX = 0;
			bucket.FlowY = 0;
		}
		else
		{
			for (int i = 0; i < steps; ++i)
			{
				bucket.Heat = bucket.Heat * 15 / 16;
				bucket.FlowX = bucket.FlowX * 15 / 16;
				bucket.FlowY = bucket.FlowY * 15 / 16;
			}
		}

		bucket.Frame += steps * RouteHeatDecayFrames;
	}

	void DepositRouteHeat(NeuralRouteHeatState& state, int bucketIndex, int heat, int flowX, int flowY, const CellStruct& seenCell)
	{
		if (bucketIndex < 0 || heat <= 0)
			return;

		auto [it, inserted] = state.Buckets.try_emplace(bucketIndex);
		auto& bucket = it->second;

		if (inserted)
			bucket.Frame = Unsorted::CurrentFrame;
		else
			DecayRouteBucket(bucket);

		bucket.Heat = std::min(bucket.Heat + heat, 1 << 20);
		bucket.FlowX = std::clamp(bucket.FlowX + flowX, -(1 << 16), 1 << 16);
		bucket.FlowY = std::clamp(bucket.FlowY + flowY, -(1 << 16), 1 << 16);

		if (seenCell != CellStruct::Empty)
			bucket.RepCell = seenCell;
	}

	// One sighting of an enemy ground unit (RecordEnemySighting): coords now and at its previous
	// sighting, with the distance of both to our nearest structure. Heat goes to every bucket it
	// entered while closing in on us (the ones in between too, when it moved up to 3 buckets since
	// it was last seen), and to its bucket while it attacks something of ours (once per
	// RouteHeatAttackDepositFrames per unit).
	void RecordRouteHeatSighting(HouseClass* pHouse, TechnoClass* pTechno, const CellStruct& coords, const CellStruct& prevCoords,
		int prevFrame, double distNow, double distPrev)
	{
		if (!pHouse || !pTechno || pTechno->IsInAir() || distNow > RouteHeatMaxApproachCells)
			return;

		auto const pType = pTechno->GetTechnoType();

		if (!pType || pType->Naval)
			return;

		// What a player sees right now, never the explored but unwatched map.
		if (!pHouse->SpySatActive && !IsCellInSightOfAI(pHouse, coords))
			return;

		const int now = Unsorted::CurrentFrame;
		const int weight = std::clamp(pType->Cost / 10, 5, 250);
		auto& state = GetRouteHeat(pHouse);
		const int bucketNow = RouteBucketIndex(coords);

		auto const pTarget = abstract_cast<TechnoClass*>(pTechno->Target);

		if (pTarget && pTarget->Owner == pHouse)
		{
			auto const it = state.AttackDepositFrame.find(pTechno->UniqueID);

			if (it == state.AttackDepositFrame.end() || now - it->second >= RouteHeatAttackDepositFrames)
			{
				state.AttackDepositFrame[pTechno->UniqueID] = now;
				DepositRouteHeat(state, bucketNow, weight / 3, 0, 0, coords);
			}
		}

		if (prevCoords == CellStruct::Empty || prevFrame < 0 || now - prevFrame > 450 || distNow >= distPrev)
			return;

		const int bucketPrev = RouteBucketIndex(prevCoords);

		if (bucketPrev < 0 || bucketNow < 0 || bucketPrev == bucketNow)
			return;

		const int dx = coords.X - prevCoords.X;
		const int dy = coords.Y - prevCoords.Y;
		const int len = std::max({ std::abs(dx), std::abs(dy), 1 });
		const int flowX = dx * weight / len;
		const int flowY = dy * weight / len;

		const int pbx = bucketPrev % RouteHeatBucketsPerRow;
		const int pby = bucketPrev / RouteHeatBucketsPerRow;
		const int nbx = bucketNow % RouteHeatBucketsPerRow;
		const int nby = bucketNow / RouteHeatBucketsPerRow;
		const int steps = std::max(std::abs(nbx - pbx), std::abs(nby - pby));

		if (steps > 3)
		{
			DepositRouteHeat(state, bucketNow, weight, flowX, flowY, coords);

			return;
		}

		for (int k = 1; k <= steps; ++k)
		{
			const int bx = pbx + ((nbx - pbx) * k * 2 + (nbx >= pbx ? steps : -steps)) / (steps * 2);
			const int by = pby + ((nby - pby) * k * 2 + (nby >= pby ? steps : -steps)) / (steps * 2);
			const int index = by * RouteHeatBucketsPerRow + bx;
			DepositRouteHeat(state, index, weight, flowX, flowY, index == bucketNow ? coords : CellStruct::Empty);
		}
	}

	bool IsRouteCellPassable(const CellStruct& cell)
	{
		auto const pCell = MapClass::Instance.TryGetCellAt(cell);

		if (!pCell)
			return false;

		if (pCell->ContainsBridge())
			return true;

		return pCell->LandType != LandType::Water && pCell->LandType != LandType::Rock && pCell->LandType != LandType::Wall;
	}

	// Narrowest passable width through a cell (cells along E-W, N-S and both diagonals, up to 7 each
	// way), and whether a bridge or a ramp is right there.
	int MeasureRouteWidth(const CellStruct& cell, int& outFlags)
	{
		constexpr int Dirs[4][2] = { { 1, 0 }, { 0, 1 }, { 1, 1 }, { 1, -1 } };
		int width = 15;
		outFlags = 0;

		for (auto const& dir : Dirs)
		{
			int span = 1;

			for (int sign = -1; sign <= 1; sign += 2)
			{
				for (int k = 1; k <= 7; ++k)
				{
					const CellStruct c { static_cast<short>(cell.X + dir[0] * k * sign), static_cast<short>(cell.Y + dir[1] * k * sign) };

					if (!IsRouteCellPassable(c))
						break;

					span++;
				}
			}

			width = std::min(width, span);
		}

		for (int oy = -1; oy <= 1; ++oy)
		{
			for (int ox = -1; ox <= 1; ++ox)
			{
				auto const pCell = MapClass::Instance.TryGetCellAt(CellStruct { static_cast<short>(cell.X + ox), static_cast<short>(cell.Y + oy) });

				if (!pCell)
					continue;

				if (pCell->ContainsBridge())
					outFlags |= RouteSpotBridge;

				if (pCell->SlopeIndex != 0)
					outFlags |= RouteSpotRamp;
			}
		}

		return width;
	}

	// Percent: chokepoints (narrow passages, bridges, ramps in a passage) hold an attack best.
	int RouteChokeFactor(int width, int flags)
	{
		int factor = 100;

		if (width <= 3)
			factor = 220;
		else if (width <= 5)
			factor = 170;
		else if (width <= 8)
			factor = 130;

		if (flags & RouteSpotBridge)
			factor = std::max(factor, 200);

		if ((flags & RouteSpotRamp) && width <= 8)
			factor += 25;

		return factor;
	}

	// Percent: a tower spot is best a few cells outside our structures, where a tower placed next
	// to them still reaches it.
	int RouteDefenseDistanceFactor(int baseDist)
	{
		if (baseDist <= 2)
			return 40;

		if (baseDist <= 10)
			return 100;

		if (baseDist <= 24)
			return 100 - (baseDist - 10) * 5;

		return 0;
	}

	// Prunes cold buckets and rebuilds the defense and ambush spots. Called once per AI tick; does
	// real work every RouteHeatRefreshFrames (the bucket map only holds touched buckets).
	void UpdateRouteHeat(HouseClass* pHouse)
	{
		const NeuralSlowStep slowStep { "UpdateRouteHeat" };

		if (!pHouse)
			return;

		auto& state = GetRouteHeat(pHouse);
		const int now = Unsorted::CurrentFrame;

		if (now < state.RefreshFrame)
			state.RefreshFrame = -100000;

		if (now - state.RefreshFrame < RouteHeatRefreshFrames)
			return;

		state.RefreshFrame = now;

		if (now - state.PruneFrame >= RouteHeatPruneFrames || now < state.PruneFrame)
		{
			state.PruneFrame = now;

			for (auto it = state.Buckets.begin(); it != state.Buckets.end();)
			{
				DecayRouteBucket(it->second);

				if (it->second.Heat < 4)
					it = state.Buckets.erase(it);
				else
					++it;
			}

			std::erase_if(state.AttackDepositFrame, [now](const auto& entry) { return now - entry.second > 900; });
		}

		state.DefenseSpots.clear();
		state.AmbushSpots.clear();

		if (state.Buckets.empty())
			return;

		std::vector<CellStruct> ownStructures;
		std::vector<std::pair<CellStruct, int>> ownTowers;

		for (auto const pBld : pHouse->Buildings)
		{
			if (!pBld || pBld->InLimbo || !pBld->IsAlive || !pBld->Type || pBld->Type->Insignificant || pBld->Type->InvisibleInGame
				|| pBld->Type->Wall)
			{
				continue;
			}

			const CellStruct bc = pBld->GetMapCoords();
			ownStructures.push_back(bc);

			if (pBld->Type->IsBaseDefense)
			{
				int range = 0;

				for (int i = 0; i < 2; ++i)
				{
					auto const pWS = pBld->Type->GetWeapon(i);
					auto const pWeapon = pWS ? pWS->WeaponType : nullptr;

					if (pWeapon && pWeapon->Damage > 0 && (!pWeapon->Projectile || pWeapon->Projectile->AG))
						range = std::max(range, pWeapon->Range / Unsorted::LeptonsPerCell);
				}

				if (range > 0)
					ownTowers.emplace_back(bc, range);
			}
		}

		if (ownStructures.empty())
			return;

		// Rank every warm bucket with a real sighting by heat and distance to us; only the best
		// get the (cell-reading) width measure.
		std::vector<NeuralRouteSpot> defense;
		std::vector<NeuralRouteSpot> ambush;

		for (auto const& [index, bucket] : state.Buckets)
		{
			const int heat = DecayedRouteHeat(bucket);

			if (heat < RouteHeatMinSpotHeat || bucket.RepCell == CellStruct::Empty)
				continue;

			int baseDist = INT_MAX;

			for (auto const& sc : ownStructures)
				baseDist = std::min(baseDist, static_cast<int>(std::max(std::abs(sc.X - bucket.RepCell.X), std::abs(sc.Y - bucket.RepCell.Y))));

			NeuralRouteSpot spot;
			spot.Cell = bucket.RepCell;
			spot.Heat = heat;
			spot.BaseDist = baseDist;
			spot.FlowX = bucket.FlowX;
			spot.FlowY = bucket.FlowY;
			spot.BucketIndex = index;

			const int distFactor = RouteDefenseDistanceFactor(baseDist);

			if (distFactor > 0)
			{
				spot.Score = heat * distFactor / 100;
				defense.push_back(spot);
			}

			if (baseDist >= 10 && baseDist <= 36)
			{
				spot.Score = heat;
				ambush.push_back(spot);
			}
		}

		auto byScore = [](const NeuralRouteSpot& a, const NeuralRouteSpot& b)
		{
			return a.Score != b.Score ? a.Score > b.Score : a.BucketIndex < b.BucketIndex;
		};

		// Best spots, at least 6 cells apart, scored with their chokepoint factor.
		auto finish = [&byScore](std::vector<NeuralRouteSpot>& spots, size_t maxSpots, std::vector<NeuralRouteSpot>& out)
		{
			std::sort(spots.begin(), spots.end(), byScore);

			if (spots.size() > 16)
				spots.resize(16);

			for (auto& spot : spots)
			{
				spot.Width = MeasureRouteWidth(spot.Cell, spot.Flags);
				spot.Score = spot.Score * RouteChokeFactor(spot.Width, spot.Flags) / 100;
			}

			std::sort(spots.begin(), spots.end(), byScore);

			for (auto const& spot : spots)
			{
				bool close = false;

				for (auto const& kept : out)
				{
					if (std::max(std::abs(kept.Cell.X - spot.Cell.X), std::abs(kept.Cell.Y - spot.Cell.Y)) < 6)
					{
						close = true;

						break;
					}
				}

				if (!close)
					out.push_back(spot);

				if (out.size() >= maxSpots)
					break;
			}
		};

		finish(defense, RouteHeatMaxDefenseSpots, state.DefenseSpots);
		finish(ambush, RouteHeatMaxAmbushSpots, state.AmbushSpots);

		for (auto& spot : state.DefenseSpots)
		{
			for (auto const& [towerCell, range] : ownTowers)
			{
				if (std::max(std::abs(towerCell.X - spot.Cell.X), std::abs(towerCell.Y - spot.Cell.Y)) <= range)
					spot.Cover++;
			}
		}
	}

	// The defense spot a new ground tower should cover: best score for its cover (uncovered
	// chokepoints first) among the spots it can reach from a cell next to our structures
	// (reachCells: its range plus how far from them it may be placed). nullptr when no route in
	// reach is hot enough.
	const NeuralRouteSpot* BestRouteDefenseSpot(HouseClass* pHouse, int reachCells)
	{
		auto const& state = GetRouteHeat(pHouse);
		const NeuralRouteSpot* pBest = nullptr;
		int bestValue = 0;

		for (auto const& spot : state.DefenseSpots)
		{
			if (spot.BaseDist > reachCells)
				continue;

			const int value = spot.Score / (1 + spot.Cover * 2);

			if (value >= RouteHeatMinSpotHeat && value > bestValue)
			{
				bestValue = value;
				pBest = &spot;
			}
		}

		return pBest;
	}

	// Placement score of a ground tower candidate against the route memory: covering hot spots
	// pays (less where our towers already cover them), the chosen spot pulls the tower into range,
	// and a tower never stands in a narrow passage itself (it would wall our own units in).
	int RouteDefenseCellShift(HouseClass* pHouse, const CellStruct& candidate, int rangeCells, const CellStruct& chosenSpot)
	{
		auto const& state = GetRouteHeat(pHouse);
		int shift = 0;

		for (auto const& spot : state.DefenseSpots)
		{
			const int d = std::max(std::abs(candidate.X - spot.Cell.X), std::abs(candidate.Y - spot.Cell.Y));
			const bool chosen = spot.Cell == chosenSpot;

			if (d <= 1 && spot.Width <= 5)
				shift -= 800;

			if (d <= rangeCells)
				shift += std::clamp(spot.Score / 4, 0, 400) / (1 + spot.Cover) + (chosen ? 500 + (rangeCells - d) * 10 : 0);
			else if (chosen)
				shift -= (d - rangeCells) * 60;
		}

		return shift;
	}

	// The defense spot within a placed tower's range, if any (telemetry and the route tower list).
	bool FindRouteSpotCovered(HouseClass* pHouse, const CellStruct& towerCell, int rangeCells, CellStruct& outSpot)
	{
		for (auto const& spot : GetRouteHeat(pHouse).DefenseSpots)
		{
			if (std::max(std::abs(towerCell.X - spot.Cell.X), std::abs(towerCell.Y - spot.Cell.Y)) <= rangeCells)
			{
				outSpot = spot.Cell;

				return true;
			}
		}

		return false;
	}

	void RecordRouteTowerPlaced(HouseClass* pHouse, const CellStruct& towerCell, const CellStruct& spotCell)
	{
		auto& towers = GetRouteHeat(pHouse).RouteTowers;

		if (towers.size() >= 16)
			towers.erase(towers.begin());

		towers.emplace_back(towerCell, spotCell);
	}

	// Picket / ambush post number `index` on the hot routes further out, `sideCells` off the route
	// (left for even posts, right for odd ones, so a passing army does not run over it), or Empty.
	CellStruct RouteAmbushCell(HouseClass* pHouse, size_t index, int sideCells)
	{
		auto const& spots = GetRouteHeat(pHouse).AmbushSpots;

		if (index >= spots.size())
			return CellStruct::Empty;

		auto const& spot = spots[index];
		const int ax = std::abs(spot.FlowX);
		const int ay = std::abs(spot.FlowY);

		if (ax == 0 && ay == 0)
			return spot.Cell;

		// Approach direction in 8 steps (tan 22.5 deg ~ 5/12, integer only), then its normal.
		const int fx = (ax * 12 > ay * 5) ? (spot.FlowX > 0 ? 1 : -1) : 0;
		const int fy = (ay * 12 > ax * 5) ? (spot.FlowY > 0 ? 1 : -1) : 0;
		const int side = (index % 2 == 0) ? sideCells : -sideCells;

		return CellStruct { static_cast<short>(spot.Cell.X - fy * side), static_cast<short>(spot.Cell.Y + fx * side) };
	}

	// The hottest approach within maxDist cells of `nearCell` that our towers already cover: a ground
	// army holding just behind it fights the enemy where it really comes in, under tower fire.
	const NeuralRouteSpot* BestRouteHoldSpot(HouseClass* pHouse, const CellStruct& nearCell, int maxDist)
	{
		const NeuralRouteSpot* pBest = nullptr;

		for (auto const& spot : GetRouteHeat(pHouse).DefenseSpots)
		{
			if (spot.Cover <= 0 || std::max(std::abs(nearCell.X - spot.Cell.X), std::abs(nearCell.Y - spot.Cell.Y)) > maxDist)
				continue;

			if (!pBest || spot.Score > pBest->Score)
				pBest = &spot;
		}

		return pBest;
	}

	// Route heat summed over the buckets a straight line from `from` to `to` crosses.
	int RouteHeatAlong(HouseClass* pHouse, const CellStruct& from, const CellStruct& to)
	{
		auto const& buckets = GetRouteHeat(pHouse).Buckets;

		if (buckets.empty())
			return 0;

		const int dx = to.X - from.X;
		const int dy = to.Y - from.Y;
		const int steps = std::max(1, std::max(std::abs(dx), std::abs(dy)) / 2);
		int lastIndex = -1;
		int total = 0;

		for (int k = 0; k <= steps; ++k)
		{
			const CellStruct c { static_cast<short>(from.X + dx * k / steps), static_cast<short>(from.Y + dy * k / steps) };
			const int index = RouteBucketIndex(c);

			if (index < 0 || index == lastIndex)
				continue;

			lastIndex = index;

			if (auto const it = buckets.find(index); it != buckets.end())
				total += DecayedRouteHeat(it->second);
		}

		return total;
	}

	// Route heat of the buckets within `radiusBuckets` of a cell.
	int RouteHeatNear(HouseClass* pHouse, const CellStruct& cell, int radiusBuckets)
	{
		auto const& buckets = GetRouteHeat(pHouse).Buckets;
		const int center = RouteBucketIndex(cell);

		if (buckets.empty() || center < 0)
			return 0;

		const int cbx = center % RouteHeatBucketsPerRow;
		const int cby = center / RouteHeatBucketsPerRow;
		int total = 0;

		for (int by = cby - radiusBuckets; by <= cby + radiusBuckets; ++by)
		{
			for (int bx = cbx - radiusBuckets; bx <= cbx + radiusBuckets; ++bx)
			{
				if (bx < 0 || by < 0 || bx >= RouteHeatBucketsPerRow || by >= RouteHeatBucketsPerRow)
					continue;

				if (auto const it = buckets.find(by * RouteHeatBucketsPerRow + bx); it != buckets.end())
					total += DecayedRouteHeat(it->second);
			}
		}

		return total;
	}
} // namespace

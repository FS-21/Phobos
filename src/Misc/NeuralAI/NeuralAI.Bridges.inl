// NeuralAI.Bridges.inl: Bridge planner: map catalog of bridges linked to repair huts, what each
// house has seen of them, and which bridge to destroy or repair (sea lanes, fleet blocks, ground
// routes, moats and ambushes).
// Part of NeuralAI.cpp (included in order; one translation unit).

namespace
{
	// Low bridge overlays: the same ranges the game uses to link a repair hut to its bridge.
	bool IsLowBridgeOverlay(int overlayIndex)
	{
		return (overlayIndex >= 74 && overlayIndex <= 101) || (overlayIndex >= 205 && overlayIndex <= 232);
	}

	enum class NeuralBridgeDeck : int
	{
		None = 0,
		Low = 1, // ships cannot pass while it stands
		High = 2
	};

	NeuralBridgeDeck GetBridgeDeck(const CellClass* pCell)
	{
		if (!pCell)
			return NeuralBridgeDeck::None;

		if (IsLowBridgeOverlay(pCell->OverlayTypeIndex))
			return NeuralBridgeDeck::Low;

		return pCell->ContainsBridgeEx() ? NeuralBridgeDeck::High : NeuralBridgeDeck::None;
	}

	// A bridge linked to a repair hut, charted from the map while it stands. Cells are kept, not
	// zone numbers: the game renumbers the movement zones whenever a bridge falls or is repaired.
	struct NeuralBridgeSpan
	{
		CellStruct HutCell { CellStruct::Empty };
		std::vector<CellStruct> OtherHutCells {}; // huts of the same bridge on the other bank
		CellStruct Center { CellStruct::Empty };
		std::vector<CellStruct> Deck {};
		std::vector<CellStruct> WaterSideA {};
		std::vector<CellStruct> WaterSideB {};
		std::vector<CellStruct> BankA {};
		std::vector<CellStruct> BankB {};
		CellStruct WaterCenterA { CellStruct::Empty };
		CellStruct WaterCenterB { CellStruct::Empty };
		CellStruct BankCenterA { CellStruct::Empty };
		CellStruct BankCenterB { CellStruct::Empty };
		bool Low { false };
		bool Charted { false }; // false while the bridge has never been seen standing
		int Broken { -1 }; // map state at the last check, to log when it changes
		int ChangeFrame { -1 }; // when it last changed, to log the zones once they settle
		bool Duplicate { false }; // a second hut of an already charted bridge
	};

	enum class NeuralBridgeAction : int
	{
		None = 0,
		Destroy = 1,
		Repair = 2
	};

	// What one house knows about one bridge: only what its own and allied units have seen.
	struct NeuralBridgeKnowledge
	{
		int Known { -1 }; // -1 never seen, 0 standing, 1 broken
		int SeenFrame { -1 };
		NeuralBridgeAction LastAction { NeuralBridgeAction::None };
		int LastActionFrame { -100000 };
		DWORD WorkerID { 0 }; // engineer sent to the hut
		int WorkerFrame { -100000 };
		int BlockedUntilFrame { -1 }; // the hut could not be reached
	};

	struct NeuralBridgeHouseState
	{
		std::vector<NeuralBridgeKnowledge> Spans {};
		int SeaLaneSpan { -1 }; // standing low bridge between our fleet and its targets
		int SeaLaneFleetZone { -1 };
		int FleetBlockSpan { -1 }; // broken low bridge an enemy fleet is about to cross
		int RouteSpan { -1 }; // broken bridge whose repair links our base to known enemies
		int MoatSpan { -1 }; // standing bridge a stronger enemy force is about to cross
		int AmbushSpan { -1 }; // standing bridge with enemies on its deck
		CellStruct AmbushCell { CellStruct::Empty };
		int AmbushCount { 0 };
		int KnownBroken { 0 };
	};

	// Opposite actions on the same bridge wait this long, so the AI never flips a bridge back
	// and forth.
	constexpr int BridgeActionCooldownFrames = 2700;

	std::vector<NeuralBridgeSpan> BridgeSpans;
	std::unordered_map<int, int> BridgeDeckOwner; // deck cell index -> span index
	int BridgeCatalogFrame = -1;
	int BridgeChartRetryFrame = 0;
	std::array<NeuralBridgeHouseState, 32> BridgeStates {};

	int BridgeCellIndex(const CellStruct& cell)
	{
		return (static_cast<int>(cell.Y) << 9) + cell.X;
	}

	// Passage mouths: open ground within PassageMouthSteps steps (8 directions, over open ground
	// only) of the land at either end of a charted bridge, or of a tunnel tube's entry or exit cell.
	// Buildings stay off it and idle units are moved out of it, so the traffic of a base never has
	// to squeeze past them to reach the passage (match #8920: a house walled in the mouth of its own
	// bridge and held 15 to 27 units there for the rest of the match). Map state only, the same for
	// every house.
	constexpr int PassageMouthSteps = 5;
	constexpr int PassageGridSide = 512;
	std::vector<uint8_t> PassageMouthGrid;
	int PassageMouthSpans = -1; // charted bridges the grid was drawn for

	// Ground a vehicle can stand on once whatever is on it moves away: not water, and either under
	// a building (buildable ground) or clear for a tracked vehicle.
	bool IsOpenGroundCell(CellClass* pCell)
	{
		return pCell && pCell->LandType != LandType::Water
			&& (pCell->GetBuilding() || pCell->IsClearToMove(SpeedType::Track, true, true, -1, MovementZone::Normal, -1, false));
	}

	// The same per cell, remembered OpenGroundCacheFrames (0 not asked yet, 1 open, 2 closed): the
	// ground does not change, and building placement asks it for every candidate cell.
	constexpr int OpenGroundCacheFrames = 4500;
	std::vector<uint8_t> OpenGroundGrid;
	int OpenGroundFrame = -1;

	bool IsOpenGroundAt(const CellStruct& cell)
	{
		if (cell.X < 0 || cell.Y < 0 || cell.X >= PassageGridSide || cell.Y >= PassageGridSide)
			return false;

		const int now = Unsorted::CurrentFrame;

		if (OpenGroundGrid.empty() || now < OpenGroundFrame || now - OpenGroundFrame >= OpenGroundCacheFrames)
		{
			OpenGroundGrid.assign(PassageGridSide * PassageGridSide, 0);
			OpenGroundFrame = now;
		}

		uint8_t& known = OpenGroundGrid[(static_cast<int>(cell.Y) << 9) + cell.X];

		if (known == 0)
			known = (MapClass::Instance.CoordinatesLegal(cell) && IsOpenGroundCell(MapClass::Instance.TryGetCellAt(cell))) ? 1 : 2;

		return known == 1;
	}

	bool IsPassageMouthCell(const CellStruct& cell)
	{
		return !PassageMouthGrid.empty() && cell.X >= 0 && cell.Y >= 0 && cell.X < PassageGridSide && cell.Y < PassageGridSide
			&& PassageMouthGrid[BridgeCellIndex(cell)] != 0;
	}

	bool IsFootprintOnPassageMouth(const CellStruct& topLeft, int width, int height)
	{
		if (PassageMouthGrid.empty())
			return false;

		for (int fy = 0; fy < height; ++fy)
		{
			for (int fx = 0; fx < width; ++fx)
			{
				if (IsPassageMouthCell(CellStruct { static_cast<short>(topLeft.X + fx), static_cast<short>(topLeft.Y + fy) }))
					return true;
			}
		}

		return false;
	}

	int CellDistanceSq(const CellStruct& a, const CellStruct& b)
	{
		const int dx = a.X - b.X;
		const int dy = a.Y - b.Y;

		return dx * dx + dy * dy;
	}

	CellStruct AverageCell(const std::vector<CellStruct>& cells)
	{
		if (cells.empty())
			return CellStruct::Empty;

		int sumX = 0;
		int sumY = 0;

		for (auto const& cell : cells)
		{
			sumX += cell.X;
			sumY += cell.Y;
		}

		const int count = static_cast<int>(cells.size());

		return CellStruct { static_cast<short>(sumX / count), static_cast<short>(sumY / count) };
	}

	// First valid zone among a list of cells (zones 0 and 1 are not places a unit can stand on).
	int FirstZoneOf(const std::vector<CellStruct>& cells, MovementZone movementZone)
	{
		for (auto const& cell : cells)
		{
			const int zone = MapClass::Instance.GetMovementZoneType(cell, movementZone, false);

			if (zone > 1)
				return zone;
		}

		return -1;
	}

	bool AreBridgesDestroyable()
	{
		if (RulesClass::Instance && RulesClass::Instance->BridgeStrength >= 1000000)
			return false;

		if (SessionClass::Instance.GameMode == GameMode::Campaign)
			return ScenarioClass::Instance && ScenarioClass::Instance->SpecialFlags.DestroyableBridges;

		return GameModeOptionsClass::Instance.BridgeDestruction;
	}

	BuildingClass* GetBridgeHutAt(const CellStruct& cell)
	{
		auto const pCell = MapClass::Instance.TryGetCellAt(cell);
		auto const pBld = pCell ? pCell->GetBuilding() : nullptr;

		if (!pBld || !pBld->IsAlive || pBld->InLimbo || !pBld->Type || !pBld->Type->BridgeRepairHut)
			return nullptr;

		return pBld;
	}

	// The bridge's hut on the given ground zone if it has one there, else its first hut.
	BuildingClass* GetBridgeHut(const NeuralBridgeSpan& span, int preferredZone = -1)
	{
		if (preferredZone > 1)
		{
			for (auto const& cell : span.OtherHutCells)
			{
				if (MapClass::Instance.GetMovementZoneType(cell, MovementZone::Normal, false) == preferredZone)
				{
					if (auto const pHut = GetBridgeHutAt(cell))
						return pHut;
				}
			}
		}

		if (auto const pHut = GetBridgeHutAt(span.HutCell))
			return pHut;

		for (auto const& cell : span.OtherHutCells)
		{
			if (auto const pHut = GetBridgeHutAt(cell))
				return pHut;
		}

		return nullptr;
	}

	// Index of the span whose deck covers this cell, or -1.
	int BridgeSpanAt(const CellStruct& cell)
	{
		auto const it = BridgeDeckOwner.find(BridgeCellIndex(cell));

		return it != BridgeDeckOwner.end() ? it->second : -1;
	}

	// Follows the deck from the cell nearest to the hut and records the water on both sides (low
	// bridges) and the land at both ends. Bridges run along one map axis, so the sides are the
	// cells on either side of the deck line and the banks the cells beyond its two ends.
	void ChartBridgeSpan(NeuralBridgeSpan& span, int spanIndex)
	{
		CellStruct anchor = CellStruct::Empty;
		NeuralBridgeDeck deck = NeuralBridgeDeck::None;
		int bestDist = INT_MAX;

		for (int dy = -3; dy <= 3; ++dy)
		{
			for (int dx = -3; dx <= 3; ++dx)
			{
				const CellStruct cell { static_cast<short>(span.HutCell.X + dx), static_cast<short>(span.HutCell.Y + dy) };
				const NeuralBridgeDeck kind = GetBridgeDeck(MapClass::Instance.TryGetCellAt(cell));
				const int dist = dx * dx + dy * dy;

				if (kind != NeuralBridgeDeck::None && dist < bestDist)
				{
					bestDist = dist;
					anchor = cell;
					deck = kind;
				}
			}
		}

		if (deck == NeuralBridgeDeck::None)
			return;

		const int owner = BridgeSpanAt(anchor);

		if (owner >= 0 && owner != spanIndex)
		{
			span.Duplicate = true;
			span.Charted = true;
			BridgeSpans[owner].OtherHutCells.push_back(span.HutCell);

			return;
		}

		span.Deck.clear();
		std::unordered_set<int> visited { BridgeCellIndex(anchor) };
		span.Deck.push_back(anchor);

		constexpr int StepX[4] = { 1, -1, 0, 0 };
		constexpr int StepY[4] = { 0, 0, 1, -1 };

		for (size_t i = 0; i < span.Deck.size() && span.Deck.size() < 160; ++i)
		{
			const CellStruct current = span.Deck[i];

			for (int dir = 0; dir < 4; ++dir)
			{
				const CellStruct next { static_cast<short>(current.X + StepX[dir]), static_cast<short>(current.Y + StepY[dir]) };

				if (!visited.insert(BridgeCellIndex(next)).second)
					continue;

				if (GetBridgeDeck(MapClass::Instance.TryGetCellAt(next)) == deck)
					span.Deck.push_back(next);
			}
		}

		short minX = SHRT_MAX;
		short maxX = SHRT_MIN;
		short minY = SHRT_MAX;
		short maxY = SHRT_MIN;
		int sumX = 0;
		int sumY = 0;

		for (auto const& cell : span.Deck)
		{
			minX = std::min(minX, cell.X);
			maxX = std::max(maxX, cell.X);
			minY = std::min(minY, cell.Y);
			maxY = std::max(maxY, cell.Y);
			sumX += cell.X;
			sumY += cell.Y;
		}

		const int count = static_cast<int>(span.Deck.size());
		const bool alongX = (maxX - minX) >= (maxY - minY);

		std::unordered_set<int> deckCells;

		for (auto const& cell : span.Deck)
			deckCells.insert(BridgeCellIndex(cell));

		std::unordered_set<int> sorted;

		span.Low = deck == NeuralBridgeDeck::Low;

		for (int pass = 0; pass < 3; ++pass)
		{
			// Pass 0 looks next to the deck; the next passes look further out along the axis
			// when the ramps leave no land right at the ends.
			if (pass > 0 && !span.BankA.empty() && !span.BankB.empty())
				break;

			for (auto const& cell : span.Deck)
			{
				for (int dy = -1 - pass; dy <= 1 + pass; ++dy)
				{
					for (int dx = -1 - pass; dx <= 1 + pass; ++dx)
					{
						const CellStruct nearCell { static_cast<short>(cell.X + dx), static_cast<short>(cell.Y + dy) };
						const int idx = BridgeCellIndex(nearCell);

						if (deckCells.count(idx) || !sorted.insert(idx).second)
							continue;

						auto const pNear = MapClass::Instance.TryGetCellAt(nearCell);

						if (!pNear)
							continue;

						const int along = alongX ? nearCell.X : nearCell.Y;
						const int alongMin = alongX ? minX : minY;
						const int alongMax = alongX ? maxX : maxY;

						if (pNear->LandType == LandType::Water)
						{
							if (pass > 0 || !span.Low || along < alongMin || along > alongMax)
								continue;

							// Which side of the deck line: compared against the mean in integers.
							const int perpTimesCount = (alongX ? nearCell.Y : nearCell.X) * count;
							const int perpSum = alongX ? sumY : sumX;

							if (perpTimesCount < perpSum)
								span.WaterSideA.push_back(nearCell);
							else if (perpTimesCount > perpSum)
								span.WaterSideB.push_back(nearCell);
						}
						else if (along < alongMin)
						{
							span.BankA.push_back(nearCell);
						}
						else if (along > alongMax)
						{
							span.BankB.push_back(nearCell);
						}
					}
				}
			}
		}

		span.Center = CellStruct { static_cast<short>(sumX / count), static_cast<short>(sumY / count) };
		span.WaterCenterA = AverageCell(span.WaterSideA);
		span.WaterCenterB = AverageCell(span.WaterSideB);
		span.BankCenterA = AverageCell(span.BankA);
		span.BankCenterB = AverageCell(span.BankB);
		span.Charted = true;

		for (auto const& cell : span.Deck)
			BridgeDeckOwner.try_emplace(BridgeCellIndex(cell), spanIndex);

		Debug::Log("[NeuralAI] Bridge %d charted: hut (%d,%d) %s, %d deck cells, water sides %d/%d, banks %d/%d.\n",
			spanIndex, span.HutCell.X, span.HutCell.Y, span.Low ? "low" : "high", count,
			static_cast<int>(span.WaterSideA.size()), static_cast<int>(span.WaterSideB.size()),
			static_cast<int>(span.BankA.size()), static_cast<int>(span.BankB.size()));
	}

	// Draws the passage mouths again when the number of charted bridges changed (a bridge broken at
	// the start of the match is charted the first time it stands).
	void UpdatePassageMouths()
	{
		const NeuralSlowStep slowStep { "UpdatePassageMouths" };

		int charted = 0;

		for (auto const& span : BridgeSpans)
		{
			if (span.Charted && !span.Duplicate)
				++charted;
		}

		if (charted == PassageMouthSpans)
			return;

		PassageMouthSpans = charted;
		PassageMouthGrid.assign(PassageGridSide * PassageGridSide, 0);
		std::vector<CellStruct> frontier;

		auto const reach = [&frontier](const CellStruct& cell)
		{
			if (cell.X < 0 || cell.Y < 0 || cell.X >= PassageGridSide || cell.Y >= PassageGridSide)
				return;

			uint8_t& mark = PassageMouthGrid[BridgeCellIndex(cell)];

			if (mark != 0)
				return;

			auto const pCell = MapClass::Instance.TryGetCellAt(cell);

			if (!IsOpenGroundCell(pCell) || GetBridgeDeck(pCell) != NeuralBridgeDeck::None)
				return;

			mark = 1;
			frontier.push_back(cell);
		};

		for (auto const& span : BridgeSpans)
		{
			if (!span.Charted || span.Duplicate)
				continue;

			for (auto const& cell : span.BankA)
				reach(cell);

			for (auto const& cell : span.BankB)
				reach(cell);
		}

		for (auto const pTube : TubeClass::Array)
		{
			if (pTube)
			{
				reach(pTube->EnterCell);
				reach(pTube->ExitCell);
			}
		}

		int cells = static_cast<int>(frontier.size());

		for (int step = 0; step < PassageMouthSteps && !frontier.empty(); ++step)
		{
			std::vector<CellStruct> reached;
			reached.swap(frontier);

			for (auto const& cell : reached)
			{
				for (int dy = -1; dy <= 1; ++dy)
				{
					for (int dx = -1; dx <= 1; ++dx)
						reach(CellStruct { static_cast<short>(cell.X + dx), static_cast<short>(cell.Y + dy) });
				}
			}

			cells += static_cast<int>(frontier.size());
		}

		Debug::Log("[NeuralAI] Passage mouths drawn for %d bridges and %d tunnel tubes: %d cells kept clear.\n", charted,
			TubeClass::Array.Count, cells);
	}

	// Map catalog shared by every house (map state only, so deterministic). A bridge broken at
	// the start of the match is charted the first time it stands.
	void UpdateBridgeCatalog()
	{
		const int now = Unsorted::CurrentFrame;

		if (BridgeCatalogFrame < 0)
		{
			BridgeCatalogFrame = now;

			for (auto const pBld : BuildingClass::Array)
			{
				if (pBld && pBld->IsAlive && !pBld->InLimbo && pBld->Type && pBld->Type->BridgeRepairHut)
				{
					NeuralBridgeSpan span;
					span.HutCell = pBld->GetMapCoords();
					BridgeSpans.push_back(std::move(span));
				}
			}
		}

		const bool chart = now >= BridgeChartRetryFrame;

		if (chart)
			BridgeChartRetryFrame = now + 900;

		for (size_t i = 0; i < BridgeSpans.size(); ++i)
		{
			auto& span = BridgeSpans[i];
			const int broken = MapClass::Instance.IsLinkedBridgeDestroyed(span.HutCell) ? 1 : 0;

			if (chart && !span.Charted && !broken)
				ChartBridgeSpan(span, static_cast<int>(i));

			if (span.Charted && !span.Duplicate && broken != span.Broken)
			{
				if (span.Broken >= 0)
				{
					Debug::Log("[NeuralAI] Bridge %d %s at frame %d: water zones %d/%d, bank zones %d/%d.\n",
						static_cast<int>(i), broken ? "fell" : "was repaired", now,
						FirstZoneOf(span.WaterSideA, MovementZone::Water), FirstZoneOf(span.WaterSideB, MovementZone::Water),
						FirstZoneOf(span.BankA, MovementZone::Normal), FirstZoneOf(span.BankB, MovementZone::Normal));
				}

				span.ChangeFrame = span.Broken >= 0 ? now : -1;
				span.Broken = broken;
			}
			else if (span.ChangeFrame >= 0 && now - span.ChangeFrame >= 450)
			{
				Debug::Log("[NeuralAI] Bridge %d settled at frame %d: water zones %d/%d, bank zones %d/%d.\n",
					static_cast<int>(i), now,
					FirstZoneOf(span.WaterSideA, MovementZone::Water), FirstZoneOf(span.WaterSideB, MovementZone::Water),
					FirstZoneOf(span.BankA, MovementZone::Normal), FirstZoneOf(span.BankB, MovementZone::Normal));

				span.ChangeFrame = -1;
			}
		}

		UpdatePassageMouths();
	}

	void ResetBridgePlanner()
	{
		OpenGroundGrid.clear();
		OpenGroundFrame = -1;
		PassageMouthGrid.clear();
		PassageMouthSpans = -1;
		BridgeSpans.clear();
		BridgeDeckOwner.clear();
		BridgeCatalogFrame = -1;
		BridgeChartRetryFrame = 0;

		for (auto& state : BridgeStates)
			state = NeuralBridgeHouseState {};
	}

	NeuralBridgeHouseState& GetBridgeState(const HouseClass* pHouse)
	{
		return BridgeStates[std::clamp(pHouse ? pHouse->ArrayIndex : 0, 0, 31)];
	}

	bool IsBridgeActionAllowed(const NeuralBridgeKnowledge& knowledge, NeuralBridgeAction action)
	{
		return knowledge.LastAction == NeuralBridgeAction::None || knowledge.LastAction == action
			|| Unsorted::CurrentFrame - knowledge.LastActionFrame >= BridgeActionCooldownFrames;
	}

	void NoteBridgeAction(HouseClass* pHouse, int spanIndex, NeuralBridgeAction action)
	{
		auto& state = GetBridgeState(pHouse);

		if (spanIndex < 0 || spanIndex >= static_cast<int>(state.Spans.size()))
			return;

		auto& knowledge = state.Spans[spanIndex];

		if (knowledge.LastAction != action || Unsorted::CurrentFrame - knowledge.LastActionFrame >= 1800)
		{
			const char* reason = "route";
			NeuralDecision decision = NeuralDecision::BridgeRoute;

			if (action == NeuralBridgeAction::Destroy)
			{
				if (spanIndex == state.AmbushSpan)
				{
					reason = "ambush";
					decision = NeuralDecision::BridgeAmbush;
				}
				else if (spanIndex == state.SeaLaneSpan)
				{
					reason = "sea lane";
					decision = NeuralDecision::BridgeSeaLane;
				}
				else
				{
					reason = "moat";
					decision = NeuralDecision::BridgeMoat;
				}
			}
			else if (spanIndex == state.FleetBlockSpan)
			{
				reason = "fleet block";
				decision = NeuralDecision::BridgeFleetBlock;
			}

			CountDecision(decision, pHouse->ArrayIndex);

			Debug::Log("[NeuralAI] House %d %s bridge %d (%s) at frame %d.\n", pHouse->ArrayIndex,
				action == NeuralBridgeAction::Destroy ? "destroys" : "repairs", spanIndex, reason, Unsorted::CurrentFrame);
		}

		knowledge.LastAction = action;
		knowledge.LastActionFrame = Unsorted::CurrentFrame;
	}

	// Best damage a unit can deal to a bridge deck: high bridges only take damage from Wall=
	// warheads, low bridges from any ground-hitting weapon.
	int BridgeDemolitionDamage(TechnoClass* pTechno, bool lowBridge)
	{
		int best = 0;

		for (int i = 0; i < 2; ++i)
		{
			auto const pSlot = pTechno->GetWeapon(i);
			auto const pWeapon = pSlot ? pSlot->WeaponType : nullptr;

			if (!pWeapon || !pWeapon->Warhead || !pWeapon->Projectile || !pWeapon->Projectile->AG || pWeapon->Damage <= 0)
				continue;

			if (!lowBridge && !pWeapon->Warhead->Wall)
				continue;

			best = std::max(best, pWeapon->Damage);
		}

		return best;
	}

	// Units and infantry with a warhead that repairs or destroys a bridge from its hut in one hit
	// (FakeEngineer.CanRepairBridges / FakeEngineer.CanDestroyBridges).
	bool CanWorkBridgeFromHut(TechnoClass* pTechno, BuildingClass* pHut)
	{
		return TechnoExt::CanBeAffectedByFakeEngineer(pTechno, pHut, true);
	}

	struct NeuralBridgeUnitDot
	{
		CellStruct Cell;
		int Zone;
		int Value;
	};

	// Decides, from what this house has seen, which bridge it would destroy or repair and why.
	// Runs before the features so the brain sees the opportunities it is choosing between.
	void UpdateBridgePlanner(HouseClass* pHouse)
	{
		const NeuralSlowStep slowStep { "UpdateBridgePlanner" };

		auto& state = GetBridgeState(pHouse);
		state.SeaLaneSpan = -1;
		state.SeaLaneFleetZone = -1;
		state.FleetBlockSpan = -1;
		state.RouteSpan = -1;
		state.MoatSpan = -1;
		state.AmbushSpan = -1;
		state.AmbushCell = CellStruct::Empty;
		state.AmbushCount = 0;
		state.KnownBroken = 0;

		if (!pHouse)
			return;

		UpdateBridgeCatalog();

		if (BridgeSpans.empty())
			return;

		if (state.Spans.size() != BridgeSpans.size())
			state.Spans.resize(BridgeSpans.size());

		const int now = Unsorted::CurrentFrame;
		const int spanCount = static_cast<int>(BridgeSpans.size());

		for (int i = 0; i < spanCount; ++i)
		{
			auto const& span = BridgeSpans[i];
			auto& knowledge = state.Spans[i];

			if (span.Duplicate)
				continue;

			bool seen = IsCellInSightOfAI(pHouse, span.HutCell);

			if (!seen && span.Charted)
			{
				seen = IsCellInSightOfAI(pHouse, span.Center) || IsCellInSightOfAI(pHouse, span.Deck.front())
					|| IsCellInSightOfAI(pHouse, span.Deck.back());
			}

			if (seen)
			{
				knowledge.Known = MapClass::Instance.IsLinkedBridgeDestroyed(span.HutCell) ? 1 : 0;
				knowledge.SeenFrame = now;
			}

			if (knowledge.Known == 1)
				state.KnownBroken++;
		}

		std::vector<NeuralBridgeUnitDot> ownShips;
		std::vector<NeuralBridgeUnitDot> enemyShips;
		std::vector<NeuralBridgeUnitDot> ownGround;
		std::vector<NeuralBridgeUnitDot> enemyGround;
		std::vector<uint8_t> ownOnDeck(BridgeSpans.size(), 0); // never bring a bridge down on our or allied units
		std::vector<int> deckEnemyCount(BridgeSpans.size(), 0);
		std::vector<int> deckEnemyValue(BridgeSpans.size(), 0);
		std::vector<CellStruct> deckEnemyCell(BridgeSpans.size(), CellStruct::Empty);

		auto addFoot = [&](FootClass* pFoot, TechnoTypeClass* pType)
		{
			if (!pFoot || !pType || !pFoot->IsAlive || pFoot->InLimbo || !pFoot->Owner || pFoot->IsInAir())
				return;

			const bool own = pFoot->Owner == pHouse;
			const CellStruct cell = pFoot->GetMapCoords();

			if (own || pHouse->IsAlliedWith(pFoot->Owner))
			{
				const int onSpan = BridgeSpanAt(cell);

				if (onSpan >= 0)
					ownOnDeck[onSpan] = 1;
			}

			auto const pSlot = pFoot->GetWeapon(0);

			if (!pSlot || !pSlot->WeaponType)
				return;

			if (!own && (pHouse->IsAlliedWith(pFoot->Owner) || pFoot->Owner->IsNeutral() || !IsCellInSightOfAI(pHouse, cell)))
				return;

			const bool naval = pType->Naval || pType->SpeedType == SpeedType::Float;
			const int value = std::max(100, pType->Cost);

			if (naval)
				(own ? ownShips : enemyShips).push_back({ cell, ObjectZone(pFoot, MovementZone::Water), value });
			else
				(own ? ownGround : enemyGround).push_back({ cell, -1, value });

			if (!own && !naval)
			{
				const int onSpan = BridgeSpanAt(cell);

				if (onSpan >= 0)
				{
					deckEnemyCount[onSpan]++;
					deckEnemyValue[onSpan] += value;

					if (deckEnemyCell[onSpan] == CellStruct::Empty)
						deckEnemyCell[onSpan] = cell;
				}
			}
		};

		for (auto const pUnit : UnitClass::Array)
			addFoot(pUnit, pUnit ? pUnit->Type : nullptr);

		for (auto const pInf : InfantryClass::Array)
			addFoot(pInf, pInf ? pInf->Type : nullptr);

		// Ambush: worth a bridge only for a column (two or more armed enemies, or 1500 credits of
		// them) on the deck of a standing bridge with none of ours on it.
		int bestAmbushValue = 0;

		for (int i = 0; i < spanCount; ++i)
		{
			if (state.Spans[i].Known != 0 || ownOnDeck[i] || (deckEnemyCount[i] < 2 && deckEnemyValue[i] < 1500))
				continue;

			if (deckEnemyValue[i] > bestAmbushValue)
			{
				bestAmbushValue = deckEnemyValue[i];
				state.AmbushSpan = i;
				state.AmbushCell = deckEnemyCell[i];
				state.AmbushCount = deckEnemyCount[i];
			}
		}

		const CellStruct home = GetHouseHomeCell(pHouse);
		const int homeZone = (home != CellStruct::Empty)
			? MapClass::Instance.GetMovementZoneType(home, MovementZone::Normal, false)
			: -1;

		// Zones (water next to them, and ground under them) of the enemy buildings this house knows.
		std::vector<int> enemyWaterZones;
		std::vector<int> enemyGroundZones;

		for (auto const pBld : BuildingClass::Array)
		{
			if (!pBld || !pBld->IsAlive || pBld->InLimbo || !pBld->Type || !pBld->Owner || pBld->Owner == pHouse
				|| pHouse->IsAlliedWith(pBld->Owner) || pBld->Owner->IsNeutral() || pBld->Type->InvisibleInGame
				|| !IsEnemyBuildingKnown(pHouse, pBld))
			{
				continue;
			}

			const CellStruct cell = pBld->GetMapCoords();
			const int groundZone = MapClass::Instance.GetMovementZoneType(cell, MovementZone::Normal, false);

			if (groundZone > 1 && std::find(enemyGroundZones.begin(), enemyGroundZones.end(), groundZone) == enemyGroundZones.end())
				enemyGroundZones.push_back(groundZone);

			for (int dy = -4; dy <= 4; dy += 2)
			{
				for (int dx = -4; dx <= 4; dx += 2)
				{
					const CellStruct nearCell { static_cast<short>(cell.X + dx), static_cast<short>(cell.Y + dy) };
					auto const pNear = MapClass::Instance.TryGetCellAt(nearCell);

					if (!pNear || pNear->LandType != LandType::Water)
						continue;

					const int waterZone = MapClass::Instance.GetMovementZoneType(nearCell, MovementZone::Water, false);

					if (waterZone > 1 && std::find(enemyWaterZones.begin(), enemyWaterZones.end(), waterZone) == enemyWaterZones.end())
						enemyWaterZones.push_back(waterZone);
				}
			}
		}

		auto hasZone = [](const std::vector<int>& zones, int zone)
		{
			return zone > 1 && std::find(zones.begin(), zones.end(), zone) != zones.end();
		};

		auto valueNear = [](const std::vector<NeuralBridgeUnitDot>& dots, const CellStruct& center, int radius)
		{
			int value = 0;

			for (auto const& dot : dots)
			{
				if (CellDistanceSq(dot.Cell, center) <= radius * radius)
					value += dot.Value;
			}

			return value;
		};

		const bool destroyable = AreBridgesDestroyable();
		int bestSeaLaneFleet = 0;
		int bestFleetThreat = 0;
		int bestRouteDist = INT_MAX;
		int bestMoatThreat = 0;

		for (int i = 0; i < spanCount; ++i)
		{
			auto const& span = BridgeSpans[i];
			auto const& knowledge = state.Spans[i];

			if (span.Duplicate || knowledge.Known < 0)
				continue;

			if (knowledge.Known == 0)
			{
				if (!destroyable || !span.Charted || ownOnDeck[i] || !IsBridgeActionAllowed(knowledge, NeuralBridgeAction::Destroy))
					continue;

				// Sea lane: our fleet waits on one side of a low bridge, its targets lie on the other.
				if (span.Low)
				{
					const int zoneA = FirstZoneOf(span.WaterSideA, MovementZone::Water);
					const int zoneB = FirstZoneOf(span.WaterSideB, MovementZone::Water);

					if (zoneA > 1 && zoneB > 1 && zoneA != zoneB)
					{
						int shipsA = 0;
						int shipsB = 0;

						for (auto const& ship : ownShips)
						{
							if (ship.Zone == zoneA)
								shipsA++;
							else if (ship.Zone == zoneB)
								shipsB++;
						}

						const bool fleetOnA = shipsA >= 2 && shipsA >= shipsB;
						const int fleetZone = fleetOnA ? zoneA : zoneB;
						const int targetZone = fleetOnA ? zoneB : zoneA;
						const int fleet = fleetOnA ? shipsA : shipsB;
						bool targetValue = hasZone(enemyWaterZones, targetZone);

						for (auto const& ship : enemyShips)
						{
							if (ship.Zone == targetZone)
								targetValue = true;
						}

						if (fleet >= 2 && targetValue && fleet > bestSeaLaneFleet)
						{
							bestSeaLaneFleet = fleet;
							state.SeaLaneSpan = i;
							state.SeaLaneFleetZone = fleetZone;
						}
					}
				}

				// Moat: a stronger enemy force gathers on the far bank of a bridge nearCell our base.
				if (home != CellStruct::Empty && span.BankCenterA != CellStruct::Empty && span.BankCenterB != CellStruct::Empty)
				{
					const bool nearIsA = CellDistanceSq(home, span.BankCenterA) <= CellDistanceSq(home, span.BankCenterB);
					const CellStruct nearBank = nearIsA ? span.BankCenterA : span.BankCenterB;
					const CellStruct farBank = nearIsA ? span.BankCenterB : span.BankCenterA;

					if (CellDistanceSq(home, nearBank) <= 45 * 45)
					{
						const int threat = valueNear(enemyGround, farBank, 12);
						const int defence = valueNear(ownGround, nearBank, 12);

						if (threat >= 2000 && threat * 2 > defence * 3 && threat > bestMoatThreat)
						{
							bestMoatThreat = threat;
							state.MoatSpan = i;
						}
					}
				}

				continue;
			}

			if (!IsBridgeActionAllowed(knowledge, NeuralBridgeAction::Repair) || now < knowledge.BlockedUntilFrame)
				continue;

			// Fleet block: an enemy fleet is about to sail through the gap of a broken low bridge
			// toward our side, and our own ships there could not stop it.
			if (span.Low && span.Charted && span.WaterCenterA != CellStruct::Empty && span.WaterCenterB != CellStruct::Empty)
			{
				int threatA = 0;
				int threatB = 0;

				for (auto const& ship : enemyShips)
				{
					const int distA = CellDistanceSq(ship.Cell, span.WaterCenterA);
					const int distB = CellDistanceSq(ship.Cell, span.WaterCenterB);

					if (std::min(distA, distB) > 22 * 22)
						continue;

					(distA < distB ? threatA : threatB) += ship.Value;
				}

				const bool enemyOnA = threatA >= threatB;
				const int threat = enemyOnA ? threatA : threatB;
				const CellStruct ownSide = enemyOnA ? span.WaterCenterB : span.WaterCenterA;
				bool ownAssets = false;

				if (threat > bestFleetThreat && threat * 4 > valueNear(ownShips, ownSide, 22) * 5)
				{
					for (auto const pBld : pHouse->Buildings)
					{
						if (pBld && pBld->IsAlive && !pBld->InLimbo && CellDistanceSq(pBld->GetMapCoords(), ownSide) <= 25 * 25)
						{
							ownAssets = true;

							break;
						}
					}
				}

				if (ownAssets)
				{
					bestFleetThreat = threat;
					state.FleetBlockSpan = i;
				}
			}

			// Ground route: the repair joins our home zone to a zone with known enemy buildings.
			// Not while our ships are using the gap of a low bridge.
			if (span.Low && valueNear(ownShips, span.Center, 15) > 0)
				continue;

			if (span.Charted)
			{
				const int zoneA = FirstZoneOf(span.BankA, MovementZone::Normal);
				const int zoneB = FirstZoneOf(span.BankB, MovementZone::Normal);

				if (zoneA <= 1 || zoneB <= 1 || zoneA == zoneB || (homeZone != zoneA && homeZone != zoneB))
					continue;

				if (!hasZone(enemyGroundZones, homeZone == zoneA ? zoneB : zoneA))
					continue;
			}
			else if (MapClass::Instance.GetMovementZoneType(span.HutCell, MovementZone::Normal, false) != homeZone
				|| enemyGroundZones.empty() || hasZone(enemyGroundZones, homeZone))
			{
				// Never seen standing: only a hut on our side, with every known enemy elsewhere.
				continue;
			}

			const int dist = (home != CellStruct::Empty) ? CellDistanceSq(home, span.HutCell) : 0;

			if (dist < bestRouteDist)
			{
				bestRouteDist = dist;
				state.RouteSpan = i;
			}
		}
	}

	// Engineer (or hut-working unit) still on its way to this bridge's hut?
	bool IsBridgeWorkerBusy(const NeuralBridgeKnowledge& knowledge, BuildingClass* pHut)
	{
		if (!knowledge.WorkerID || Unsorted::CurrentFrame - knowledge.WorkerFrame >= 3600)
			return false;

		for (auto const pFoot : FootClass::Array)
		{
			if (!pFoot || pFoot->UniqueID != knowledge.WorkerID)
				continue;

			if (!pFoot->IsAlive)
				return false;

			if (pFoot->InLimbo)
				return true; // inside a transport on its way

			return pFoot->Destination == pHut || pFoot->Target == pHut
				|| pFoot->CurrentMission == Mission::Enter || pFoot->QueuedMission == Mission::Enter;
		}

		return false;
	}

	// The hut to send an engineer to this tick, if any: blocking an enemy fleet comes first,
	// then reopening a ground route.
	BuildingClass* PickBridgeToRepair(HouseClass* pHouse, int& outSpan)
	{
		outSpan = -1;
		auto& state = GetBridgeState(pHouse);
		const int span = state.FleetBlockSpan >= 0 ? state.FleetBlockSpan : state.RouteSpan;

		if (span < 0)
			return nullptr;

		const CellStruct home = GetHouseHomeCell(pHouse);
		const int homeZone = (home != CellStruct::Empty)
			? MapClass::Instance.GetMovementZoneType(home, MovementZone::Normal, false)
			: -1;
		auto const pHut = GetBridgeHut(BridgeSpans[span], homeZone);

		if (!pHut || IsBridgeWorkerBusy(state.Spans[span], pHut))
			return nullptr;

		outSpan = span;

		return pHut;
	}

	void NoteBridgeWorker(HouseClass* pHouse, int spanIndex, TechnoClass* pWorker, bool dispatched)
	{
		auto& state = GetBridgeState(pHouse);

		if (spanIndex < 0 || spanIndex >= static_cast<int>(state.Spans.size()))
			return;

		auto& knowledge = state.Spans[spanIndex];

		if (!dispatched)
		{
			knowledge.BlockedUntilFrame = Unsorted::CurrentFrame + 1800;

			return;
		}

		knowledge.WorkerID = pWorker ? pWorker->UniqueID : 0;
		knowledge.WorkerFrame = Unsorted::CurrentFrame;
		NoteBridgeAction(pHouse, spanIndex, NeuralBridgeAction::Repair);
	}

	struct NeuralBridgeDemolition
	{
		int Span { -1 };
		CellStruct Cell { CellStruct::Empty }; // deck cell to fire at
		int FleetZone { -1 }; // ships of this water zone may join (sea lane)
	};

	// The bridge to bring down this tick: an ambush on a crowded deck first, then a sea lane for
	// our fleet, then a moat against a stronger force.
	NeuralBridgeDemolition PickBridgeToDestroy(HouseClass* pHouse)
	{
		NeuralBridgeDemolition order;
		auto const& state = GetBridgeState(pHouse);

		if (state.AmbushSpan >= 0)
		{
			order.Span = state.AmbushSpan;
			order.Cell = state.AmbushCell;

			return order;
		}

		order.Span = state.SeaLaneSpan >= 0 ? state.SeaLaneSpan : state.MoatSpan;

		if (order.Span < 0)
			return order;

		auto const& span = BridgeSpans[order.Span];
		order.FleetZone = state.SeaLaneSpan >= 0 ? state.SeaLaneFleetZone : -1;

		// The deck cell nearest to the middle that this house can see (it cannot aim into the fog).
		int bestDist = INT_MAX;

		for (auto const& cell : span.Deck)
		{
			const int dist = CellDistanceSq(cell, span.Center);

			if (dist < bestDist && IsCellInSightOfAI(pHouse, cell))
			{
				bestDist = dist;
				order.Cell = cell;
			}
		}

		if (order.Cell == CellStruct::Empty)
			order.Span = -1;

		return order;
	}
} // namespace

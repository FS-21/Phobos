// NeuralAI.Scouting.inl: Purposeful scouting. Scout targets are scored by what a look there would
// teach us (ground never explored, ground not in sight for a long time) and by what they are: free
// ore fields an enemy may have expanded to, the approaches to our base, the known enemy bases and
// the places enemy armies were last seen. Scouts are cheap, fast, far-sighted units picked from
// their rules tags, kept out of known defense reach.
// Part of NeuralAI.cpp (included in order; one translation unit).

namespace
{
	enum class NeuralScoutKind : uint8_t
	{
		Start,
		Field,
		Route,
		EnemyBase,
		EnemyArmy
	};

	constexpr const char* NeuralScoutKindNames[] = { "start", "field", "route", "enemyBase", "enemyArmy" };

	struct NeuralScoutTrip
	{
		DWORD ScoutID { 0 };
		CellStruct Target { CellStruct::Empty };
		CellStruct LastCoords { CellStruct::Empty };
		NeuralScoutKind Kind { NeuralScoutKind::Field };
		int LegFrame { 0 };
		int LegDistance { 0 };
		int Legs { 0 };
		int Reissues { 0 };
		int Sight { 0 };
		int Speed { 1 };
		double StartHealth { 1.0 };
		bool Air { false };
	};

	struct NeuralScoutCandidate
	{
		CellStruct Cell { CellStruct::Empty };
		NeuralScoutKind Kind { NeuralScoutKind::Field };
		int Gain { 0 };
		int Value { 0 };
	};

	struct NeuralScoutUnit
	{
		FootClass* Foot { nullptr };
		int Fitness { 0 };
		int Speed { 1 };
		int Zone { -1 };
		bool Air { false };
		bool Flies { false };
		int Cost { 0 };
	};

	// Last frame each 8x8 block of cells was in our current sight (-1: never).
	constexpr int ScoutBlockSize = 8;
	constexpr int ScoutBlockSide = VisionMapSide / ScoutBlockSize;
	constexpr int ScoutUpdateFrames = 45;
	// Ground out of sight this long counts as fully stale.
	constexpr int ScoutStaleFrames = 5400;
	constexpr int ScoutLaunchCooldownFrames = 240;
	constexpr int ScoutMaxLegs = 3;
	// Radius of the look a scout takes at its target (cells).
	constexpr int ScoutLookRadius = 7;

	struct NeuralScoutState
	{
		int MatchSerial { -1 };
		int LastUpdateFrame { -100000 };
		int LastLaunchFrame { -100000 };
		std::vector<NeuralScoutTrip> Trips {};
		std::vector<int> BlockSeenFrame {};
		// Targets looked at (cell, frame) and where scouts died (cell, frame).
		std::vector<std::pair<CellStruct, int>> Visited {};
		std::vector<std::pair<CellStruct, int>> LostAt {};
		// Targets a scout gave up on (late or stopped short), kept for 27000 frames.
		std::vector<std::pair<CellStruct, int>> Failed {};
		// Units whose move order ended at once where they stood (UniqueID, frame): not scouts again
		// for 9000 frames. Some idle soldiers drop a long move right away while vehicles reach the
		// same targets, so the unit is to blame, not the target.
		std::vector<std::pair<DWORD, int>> Benched {};
		// Known enemy buildings already counted for the found-by-scouting telemetry. Lookup only.
		std::unordered_set<DWORD> CountedBuildings {};
		// Telemetry only, never read by a decision: enemy expansion buildings by UniqueID, the frame
		// each was first noticed to exist, and the ones already reported as known. Lookup only.
		std::unordered_map<DWORD, int> ExpansionBornFrame {};
		std::unordered_set<DWORD> ExpansionKnown {};
		int ExpansionCheckFrame { -1 };
	};

	std::array<NeuralScoutState, 32> ScoutStates {};

	NeuralScoutState& GetScoutState(const HouseClass* pHouse)
	{
		auto& state = ScoutStates[std::clamp(pHouse ? pHouse->ArrayIndex : 0, 0, 31)];

		if (state.MatchSerial != NeuralMatchSerial)
		{
			state = NeuralScoutState { NeuralMatchSerial };
			state.BlockSeenFrame.assign(static_cast<size_t>(ScoutBlockSide) * ScoutBlockSide, -1);
		}

		return state;
	}

	bool IsFogOfWarMatch()
	{
		return GameModeOptionsClass::Instance.FogOfWar || (ScenarioClass::Instance && ScenarioClass::Instance->SpecialFlags.FogOfWar);
	}

	size_t ScoutBlockIndex(const CellStruct& cell)
	{
		const int bx = std::clamp(static_cast<int>(cell.X) / ScoutBlockSize, 0, ScoutBlockSide - 1);
		const int by = std::clamp(static_cast<int>(cell.Y) / ScoutBlockSize, 0, ScoutBlockSide - 1);

		return static_cast<size_t>(by) * ScoutBlockSide + bx;
	}

	// Marks the blocks our and our allies' units and buildings see right now.
	void UpdateScoutSightBlocks(HouseClass* pHouse, NeuralScoutState& state)
	{
		const int now = Unsorted::CurrentFrame;

		for (auto const pTechno : TechnoClass::Array)
		{
			if (!pTechno || pTechno->InLimbo || !pTechno->IsAlive || !pTechno->Owner
				|| (pTechno->Owner != pHouse && !pHouse->IsAlliedWith(pTechno->Owner)))
			{
				continue;
			}

			auto const pType = pTechno->GetTechnoType();

			if (!pType)
				continue;

			const CellStruct coords = pTechno->GetMapCoords();
			const int radius = std::clamp(static_cast<int>(pType->Sight), 1, 24) + 2;
			const int bx0 = std::clamp((coords.X - radius) / ScoutBlockSize, 0, ScoutBlockSide - 1);
			const int bx1 = std::clamp((coords.X + radius) / ScoutBlockSize, 0, ScoutBlockSide - 1);
			const int by0 = std::clamp((coords.Y - radius) / ScoutBlockSize, 0, ScoutBlockSide - 1);
			const int by1 = std::clamp((coords.Y + radius) / ScoutBlockSize, 0, ScoutBlockSide - 1);

			for (int by = by0; by <= by1; ++by)
			{
				for (int bx = bx0; bx <= bx1; ++bx)
				{
					const int dx = bx * ScoutBlockSize + ScoutBlockSize / 2 - coords.X;
					const int dy = by * ScoutBlockSize + ScoutBlockSize / 2 - coords.Y;

					if (dx * dx + dy * dy <= radius * radius)
						state.BlockSeenFrame[static_cast<size_t>(by) * ScoutBlockSide + bx] = now;
				}
			}
		}
	}

	// What a look at center would teach us, 0..100: ground never explored counts fully, explored
	// ground by the time since it was last in sight. Without fog of war the engine shows explored
	// ground live (and the AI's known buildings and sightings use it), so there it counts a fifth.
	int ScoutInfoGain(HouseClass* pHouse, const NeuralScoutState& state, const CellStruct& center, int radius, bool fog)
	{
		const int now = Unsorted::CurrentFrame;
		int total = 0;
		int samples = 0;

		for (int dy = -radius; dy <= radius; dy += 2)
		{
			for (int dx = -radius; dx <= radius; dx += 2)
			{
				if (dx * dx + dy * dy > radius * radius)
					continue;

				const CellStruct cell { static_cast<short>(center.X + dx), static_cast<short>(center.Y + dy) };
				auto const pCell = MapClass::Instance.CoordinatesLegal(cell) ? MapClass::Instance.TryGetCellAt(cell) : nullptr;

				if (!pCell)
					continue;

				samples++;

				if (!IsCellVisibleToAI(pHouse, pCell))
				{
					total += 100;
					continue;
				}

				const int seen = state.BlockSeenFrame[ScoutBlockIndex(cell)];
				const int stale = std::min(100, (seen < 0 ? now : now - seen) * 100 / ScoutStaleFrames);
				total += fog ? stale : stale / 5;
			}
		}

		return samples > 0 ? total / samples : 0;
	}

	// The first cell from target back toward home (2 cell steps, at most 20 cells) out of every
	// known enemy defense's reach plus 2 cells; Empty when there is none.
	CellStruct ScoutStandoffCell(HouseClass* pHouse, const CellStruct& target, const CellStruct& home)
	{
		const int dx = home.X - target.X;
		const int dy = home.Y - target.Y;
		const int len = std::max({ 1, std::abs(dx), std::abs(dy) });

		for (int back = 0; back <= std::min(20, len); back += 2)
		{
			const CellStruct cell { static_cast<short>(target.X + dx * back / len), static_cast<short>(target.Y + dy * back / len) };

			if (!IsCellInKnownEnemyDefenseReach(pHouse, cell, 2))
				return cell;
		}

		return CellStruct::Empty;
	}

	// Points of the straight lane from -> to (every 4 cells) inside known enemy defense reach or
	// within 6 cells of where a scout of ours died or got badly hurt lately.
	int ScoutGroundLaneDanger(HouseClass* pHouse, const NeuralScoutState& state, const CellStruct& from, const CellStruct& to)
	{
		const int dx = to.X - from.X;
		const int dy = to.Y - from.Y;
		const int len = std::max({ 1, std::abs(dx), std::abs(dy) });
		int danger = 0;

		for (int step = 4; step < len; step += 4)
		{
			const CellStruct cell { static_cast<short>(from.X + dx * step / len), static_cast<short>(from.Y + dy * step / len) };

			if (IsCellInKnownEnemyDefenseReach(pHouse, cell, 1))
				danger++;

			for (auto const& [spot, frame] : state.LostAt)
			{
				if (spot.DistanceFrom(cell) <= 6.0)
				{
					danger++;
					break;
				}
			}
		}

		return danger;
	}

	int AntiAirRangeCells(TechnoTypeClass* pType)
	{
		int range = 0;

		for (int i = 0; pType && i < 2; ++i)
		{
			auto const pWS = pType->GetWeapon(i);
			auto const pWeapon = pWS ? pWS->WeaponType : nullptr;

			if (pWeapon && pWeapon->Damage > 0 && pWeapon->Projectile && pWeapon->Projectile->AA)
				range = std::max(range, pWeapon->Range / Unsorted::LeptonsPerCell);
		}

		return range;
	}

	// An aircraft may fly the straight lane from -> to: no known anti-air building covers it (its
	// range plus 3 cells), no anti-air unit was seen near it lately and none of our flyers was shot
	// down near it lately.
	bool IsScoutAirLaneSafe(HouseClass* pHouse, const CellStruct& from, const CellStruct& to,
		const std::vector<CellStruct>& recentAntiAir)
	{
		auto const& macro = GetMacroState(pHouse);
		auto const& known = GetKnownEnemyBuildingCache(pHouse).Entries;
		const int dx = to.X - from.X;
		const int dy = to.Y - from.Y;
		const int len = std::max({ 1, std::abs(dx), std::abs(dy) });
		const int now = Unsorted::CurrentFrame;

		for (int step = 0; step <= len; step += 3)
		{
			const CellStruct cell { static_cast<short>(from.X + dx * step / len), static_cast<short>(from.Y + dy * step / len) };

			for (auto const& info : known)
			{
				if (info.IsAA && info.Coords.DistanceFrom(cell) <= AntiAirRangeCells(info.Building->Type) + 3)
					return false;
			}

			for (auto const& aa : recentAntiAir)
			{
				if (aa.DistanceFrom(cell) <= 8.0)
					return false;
			}

			for (auto const& [spot, frame] : macro.AirDangerSpots)
			{
				if (now - frame >= 0 && now - frame < 8100 && spot.DistanceFrom(cell) <= 7.0)
					return false;
			}
		}

		return true;
	}

	CellClass* ScoutLandCellInZone(const CellStruct& center, int zone, int searchRadius)
	{
		for (int r = 0; r <= searchRadius; ++r)
		{
			for (int dy = -r; dy <= r; ++dy)
			{
				for (int dx = -r; dx <= r; ++dx)
				{
					if (std::max(std::abs(dx), std::abs(dy)) != r)
						continue;

					const CellStruct cell { static_cast<short>(center.X + dx), static_cast<short>(center.Y + dy) };

					if (!MapClass::Instance.CoordinatesLegal(cell))
						continue;

					auto const pCell = MapClass::Instance.TryGetCellAt(cell);

					if (pCell && pCell->LandType != LandType::Water
						&& (zone < 0 || MapClass::Instance.GetMovementZoneType(cell, MovementZone::Normal, false) == zone))
					{
						return pCell;
					}
				}
			}
		}

		return nullptr;
	}

	// Scout quality from the rules tags only: fast, far-sighted and cheap.
	int ScoutFitness(const TechnoTypeClass* pType)
	{
		return pType->Speed * 10 + pType->Sight * 15 - pType->Cost / 10;
	}

	void AddDecisionAmount(NeuralDecision decision, int houseIndex, uint32_t amount)
	{
		if (houseIndex >= 0 && houseIndex < 32)
			DecisionCounts[houseIndex][static_cast<size_t>(decision)] += amount;
	}

	void CountScoutKind(NeuralScoutKind kind, int houseIndex)
	{
		static constexpr NeuralDecision KindDecisions[] = { NeuralDecision::ScoutStart, NeuralDecision::FieldScoutSent,
			NeuralDecision::ScoutRoute, NeuralDecision::ScoutBase, NeuralDecision::ScoutArmy };

		CountDecision(KindDecisions[static_cast<size_t>(kind)], houseIndex);
	}

	// Enemy units seen lately but not in sight now, sorted by UniqueID (the sightings map is only
	// copied out here, never iterated by a decision in its own order).
	struct NeuralScoutSighting
	{
		DWORD ID;
		CellStruct Cell;
		int Value;
		int Age;
		bool AntiAir;
		bool Air;
	};

	void CollectScoutSightings(HouseClass* pHouse, std::vector<NeuralScoutSighting>& out)
	{
		const int now = Unsorted::CurrentFrame;

		for (auto const& [uid, sighting] : GetMacroState(pHouse).EnemySightings)
		{
			if (sighting.LastCoords != CellStruct::Empty)
			{
				out.push_back({ uid, sighting.LastCoords, sighting.Value, now - sighting.LastSeenFrame, sighting.AntiAir,
					sighting.Category == EnemyForceCategory::Air });
			}
		}

		std::sort(out.begin(), out.end(), [](const NeuralScoutSighting& a, const NeuralScoutSighting& b) { return a.ID < b.ID; });
	}

	// Every place worth a look, valued as kind weight x information gain there; targets inside known
	// defense reach are pulled back out of it, targets other scouts head to or looked at lately are
	// left out (also those given up on lately or twice), places where a scout died lately count a quarter. Also counts the free ore fields and
	// how many of them were never explored.
	void CollectScoutCandidates(HouseClass* pHouse, const NeuralScoutState& state, const CellStruct& home, bool fog,
		const std::vector<NeuralScoutSighting>& sightings, std::vector<NeuralScoutCandidate>& out, int& freeFields,
		int& unexploredFields)
	{
		auto const& macro = GetMacroState(pHouse);
		auto const& known = GetKnownEnemyBuildingCache(pHouse).Entries;
		const int now = Unsorted::CurrentFrame;

		auto add = [&](NeuralScoutKind kind, const CellStruct& cell, int weight)
		{
			if (cell == CellStruct::Empty || !MapClass::Instance.CoordinatesLegal(cell))
				return;

			const CellStruct look = ScoutStandoffCell(pHouse, cell, home);

			if (look == CellStruct::Empty)
				return;

			for (auto const& trip : state.Trips)
			{
				if (trip.Target.DistanceFrom(look) <= 12.0)
					return;
			}

			for (auto const& [spot, frame] : state.Visited)
			{
				if (now - frame < 2700 && spot.DistanceFrom(look) <= 10.0)
					return;
			}

			// Given up on lately, or twice: some cliff, wall or enemy keeps scouts from it.
			int failures = 0;

			for (auto const& [spot, frame] : state.Failed)
			{
				if (spot.DistanceFrom(look) <= 8.0 && (now - frame < 9000 || ++failures >= 2))
					return;
			}

			const int gain = ScoutInfoGain(pHouse, state, look, ScoutLookRadius, fog);

			if (gain < 20)
				return;

			int value = weight * gain;

			for (auto const& [spot, frame] : state.LostAt)
			{
				if (now - frame < 5400 && spot.DistanceFrom(look) <= 10.0)
				{
					value /= 4;
					break;
				}
			}

			out.push_back({ look, kind, gain, value });
		};

		// Directions enemies may come from: unchecked starts and known enemy bases.
		std::vector<CellStruct> enemyDirections;

		// Start positions not checked yet (refreshes the checked mask first). A start next to a known
		// enemy building is that enemy's base, valued below as one.
		FindNextStartLocationToScout(pHouse, home);

		for (int i = 0; i < MaxStartWaypoints; ++i)
		{
			if ((macro.ScoutedStartMask & (1u << i)) != 0 || !ScenarioClass::Instance->IsDefinedWaypoint(i))
				continue;

			const CellStruct start = ScenarioClass::Instance->GetWaypointCoords(i);

			if (start == CellStruct::Empty)
				continue;

			enemyDirections.push_back(start);

			const bool knownBase = std::any_of(known.begin(), known.end(),
				[&start](const KnownEnemyBuildingInfo& info) { return info.Coords.DistanceFrom(start) <= 14.0; });

			if (!knownBase)
				add(NeuralScoutKind::Start, start, known.empty() ? 100 : 70);
		}

		// Free ore fields: where an enemy may have expanded (or we may expand next).
		for (auto const& field : GetOreFields())
		{
			bool ours = false;

			for (auto const pOwnB : pHouse->Buildings)
			{
				if (pOwnB && pOwnB->IsAlive && pOwnB->GetMapCoords().DistanceFrom(field.Cell) <= 14.0)
				{
					ours = true;
					break;
				}
			}

			if (ours)
				continue;

			freeFields++;

			if (auto const pCell = MapClass::Instance.TryGetCellAt(field.Cell))
			{
				if (!IsCellVisibleToAI(pHouse, pCell))
					unexploredFields++;
			}

			add(NeuralScoutKind::Field, field.Cell, 90);
		}

		// Known enemy bases, by owner: the centroid of its known buildings and four points 14 cells
		// around it, where a growing base adds buildings we have not seen.
		std::array<int, 32> sumX {};
		std::array<int, 32> sumY {};
		std::array<int, 32> count {};

		for (auto const& info : known)
		{
			const size_t owner = static_cast<size_t>(std::clamp(info.Building->Owner->ArrayIndex, 0, 31));
			sumX[owner] += info.Coords.X;
			sumY[owner] += info.Coords.Y;
			count[owner]++;
		}

		for (size_t owner = 0; owner < count.size(); ++owner)
		{
			if (count[owner] == 0)
				continue;

			const CellStruct center { static_cast<short>(sumX[owner] / count[owner]), static_cast<short>(sumY[owner] / count[owner]) };
			enemyDirections.push_back(center);
			add(NeuralScoutKind::EnemyBase, center, 80);

			for (int sector = 0; sector < 8; sector += 2)
			{
				add(NeuralScoutKind::EnemyBase,
					CellStruct { static_cast<short>(center.X + AttackSectorDX[sector] * 14), static_cast<short>(center.Y + AttackSectorDY[sector] * 14) },
					80);
			}
		}

		// Approaches to our base: 28 cells out toward each enemy direction and the side most attacks
		// came from.
		auto addRoute = [&](int dx, int dy)
		{
			const int len = std::max(std::abs(dx), std::abs(dy));

			if (len < 20 || home == CellStruct::Empty)
				return;

			add(NeuralScoutKind::Route, CellStruct { static_cast<short>(home.X + dx * 28 / len), static_cast<short>(home.Y + dy * 28 / len) }, 55);
		};

		for (auto const& direction : enemyDirections)
			addRoute(direction.X - home.X, direction.Y - home.Y);

		const int mainSector = GetMainAttackSector(pHouse);

		if (mainSector >= 0)
			addRoute(AttackSectorDX[mainSector] * 28, AttackSectorDY[mainSector] * 28);

		// Where enemy armies were last seen (out of sight for 300+ frames, away from our base): the
		// biggest three groups (value within 10 cells of the strongest unit, at least 100).
		std::vector<const NeuralScoutSighting*> armies;

		for (auto const& sighting : sightings)
		{
			if (!sighting.Air && sighting.Age >= 300 && sighting.Age <= EnemySightingMemoryFrames
				&& (home == CellStruct::Empty || sighting.Cell.DistanceFrom(home) > 24.0) && !IsCellInSightOfAI(pHouse, sighting.Cell))
			{
				armies.push_back(&sighting);
			}
		}

		std::stable_sort(armies.begin(), armies.end(), [](const NeuralScoutSighting* a, const NeuralScoutSighting* b) { return a->Value > b->Value; });
		std::vector<bool> grouped(armies.size(), false);
		int groups = 0;

		for (size_t i = 0; i < armies.size() && groups < 3; ++i)
		{
			if (grouped[i])
				continue;

			int value = 0;

			for (size_t j = i; j < armies.size(); ++j)
			{
				if (!grouped[j] && armies[j]->Cell.DistanceFrom(armies[i]->Cell) <= 10.0)
				{
					grouped[j] = true;
					value += armies[j]->Value;
				}
			}

			if (value < 100)
				continue;

			groups++;
			add(NeuralScoutKind::EnemyArmy, armies[i]->Cell, std::clamp(30 + value / 5, 30, 85));
		}
	}

	bool IsScoutTripUnit(const NeuralScoutState& state, DWORD uid)
	{
		return std::any_of(state.Trips.begin(), state.Trips.end(), [uid](const NeuralScoutTrip& trip) { return trip.ScoutID == uid; });
	}

	// A unit on a scouting trip of this house (the micro leaves it alone).
	bool IsActiveScout(HouseClass* pHouse, DWORD uid)
	{
		return pHouse && IsScoutTripUnit(GetScoutState(pHouse), uid);
	}

	NeuralScoutUnit MakeScoutUnit(FootClass* pFoot, bool air)
	{
		auto const pType = pFoot->GetTechnoType();
		const bool flies = air || IsAirborneThreatTechno(pFoot);

		return { pFoot, ScoutFitness(pType), std::max(1, pType->Speed), flies ? -1 : ObjectZone(pFoot, MovementZone::Normal), air, flies,
			std::max(0, pType->Cost) };
	}

	// Scout candidates from the rules tags, best first: idle, healthy, not veteran ground units of
	// at most 1000 credits with Speed and Sight 4+ (no harvesters, deployers, transports, engineers,
	// slaves or spies), and landed aircraft of at most 1200 credits that are not spawned, carry nobody and
	// fly no air patrol.
	void CollectScoutUnits(HouseClass* pHouse, const NeuralScoutState& state, std::vector<NeuralScoutUnit>& ground,
		std::vector<NeuralScoutUnit>& air)
	{
		auto const& macro = GetMacroState(pHouse);
		auto const& patrol = GetAirPatrolState(pHouse);
		const int now = Unsorted::CurrentFrame;

		auto isFree = [&](FootClass* pFoot) -> bool
		{
			return pFoot && pFoot->Owner == pHouse && pFoot->IsAlive && !pFoot->InLimbo && !macro.PlanMembers.contains(pFoot->UniqueID)
				&& !IsScoutTripUnit(state, pFoot->UniqueID)
				&& std::none_of(state.Benched.begin(), state.Benched.end(),
					[pFoot](const std::pair<DWORD, int>& bench) { return bench.first == pFoot->UniqueID; });
		};

		auto isIdleGround = [&](FootClass* pFoot, TechnoTypeClass* pType) -> bool
		{
			return isFree(pFoot) && pType && !pType->Naval && pType->Cost <= 1000 && pType->Speed >= 4 && pType->Sight >= 4
				&& pFoot->Target == nullptr && pFoot->GetHealthPercentage() >= 0.6 && !pFoot->Veterancy.IsElite()
				&& (pFoot->CurrentMission == Mission::Guard || pFoot->CurrentMission == Mission::Area_Guard
					|| pFoot->CurrentMission == Mission::Sleep);
		};

		for (auto const pUnit : UnitClass::Array)
		{
			if (pUnit && isIdleGround(pUnit, pUnit->Type) && !pUnit->Type->Harvester && !pUnit->Type->ResourceGatherer
				&& !pUnit->Type->DeploysInto && !IsAmphibiousOrAirTransport(pUnit->Type) && pUnit->Passengers.NumPassengers == 0)
			{
				ground.push_back(MakeScoutUnit(pUnit, false));
			}
		}

		for (auto const pInf : InfantryClass::Array)
		{
			if (pInf && isIdleGround(pInf, pInf->Type) && !pInf->Type->Engineer && !pInf->Type->Slaved && !IsSpyInfantryType(pInf->Type))
				ground.push_back(MakeScoutUnit(pInf, false));
		}

		for (auto const pAir : AircraftClass::Array)
		{
			auto const pType = pAir ? pAir->Type : nullptr;

			if (!pType || !isFree(pAir) || pAir->IsInAir() || pType->Spawned || pType->Carryall || pType->Passengers > 0
				|| pType->Cost > 1200 || pType->Speed <= 0 || pType->Sight < 4 || pAir->GetHealthPercentage() < 0.8)
			{
				continue;
			}

			// On its pad: idle, or holding an attack order it has not flown yet (the air harass).
			if (pAir->CurrentMission != Mission::Guard && pAir->CurrentMission != Mission::Sleep
				&& pAir->CurrentMission != Mission::Area_Guard && pAir->CurrentMission != Mission::Attack)
			{
				continue;
			}

			auto const it = patrol.Tracks.find(pAir->UniqueID);

			if (it != patrol.Tracks.end() && now - it->second.Frame < 4 * AirPatrolStopFrames)
				continue;

			air.push_back(MakeScoutUnit(pAir, true));
		}

		auto byFitness = [](const NeuralScoutUnit& a, const NeuralScoutUnit& b)
		{
			return a.Fitness != b.Fitness ? a.Fitness > b.Fitness : a.Foot->UniqueID < b.Foot->UniqueID;
		};

		std::sort(ground.begin(), ground.end(), byFitness);
		std::sort(air.begin(), air.end(), byFitness);
	}

	struct NeuralScoutChoice
	{
		const NeuralScoutUnit* Unit { nullptr };
		const NeuralScoutCandidate* Candidate { nullptr };
		CellClass* Cell { nullptr };
		int Score { 0 };
	};

	// Best (scout, target) pair: the target value over the trip time and the scout's cost (a 1000
	// credit scout counts half), divided by the known defenses
	// on a straight ground lane; aircraft only on safe lanes and at three quarters, so cheap ground
	// scouts go first where they can.
	NeuralScoutChoice PickScoutTarget(HouseClass* pHouse, const NeuralScoutState& state, const std::vector<NeuralScoutCandidate>& candidates,
		const std::vector<NeuralScoutUnit>& units, const std::vector<CellStruct>& recentAntiAir)
	{
		NeuralScoutChoice best;

		for (auto const& candidate : candidates)
		{
			const int targetZone = MapClass::Instance.GetMovementZoneType(candidate.Cell, MovementZone::Normal, false);

			for (auto const& unit : units)
			{
				CellClass* pCell = MapClass::Instance.TryGetCellAt(candidate.Cell);

				if (!unit.Flies)
				{
					if (unit.Zone < 0)
						continue;

					if (targetZone != unit.Zone)
						pCell = ScoutLandCellInZone(candidate.Cell, unit.Zone, 4);
				}

				if (!pCell)
					continue;

				const CellStruct from = unit.Foot->GetMapCoords();
				const int eta = static_cast<int>(from.DistanceFrom(pCell->MapCoords)) * 100 / unit.Speed;
				int score = candidate.Value * 1000 / (1000 + eta) * 1000 / (1000 + unit.Cost);

				if (unit.Air)
					score = score * 3 / 4;

				if (score <= best.Score)
					continue;

				if (unit.Air ? !IsScoutAirLaneSafe(pHouse, from, pCell->MapCoords, recentAntiAir) : false)
					continue;

				if (!unit.Flies)
					score /= 1 + ScoutGroundLaneDanger(pHouse, state, from, pCell->MapCoords);

				if (score > best.Score)
					best = { &unit, &candidate, pCell, score };
			}
		}

		return best;
	}

	FootClass* FindOwnScoutFoot(HouseClass* pHouse, DWORD uid)
	{
		for (auto const pUnit : UnitClass::Array)
		{
			if (pUnit && pUnit->UniqueID == uid)
				return (pUnit->Owner == pHouse && pUnit->IsAlive) ? pUnit : nullptr;
		}

		for (auto const pInf : InfantryClass::Array)
		{
			if (pInf && pInf->UniqueID == uid)
				return (pInf->Owner == pHouse && pInf->IsAlive) ? pInf : nullptr;
		}

		for (auto const pAir : AircraftClass::Array)
		{
			if (pAir && pAir->UniqueID == uid)
				return (pAir->Owner == pHouse && pAir->IsAlive) ? pAir : nullptr;
		}

		return nullptr;
	}

	void SendScoutHome(HouseClass* pHouse, FootClass* pFoot)
	{
		if (auto const pAir = abstract_cast<AircraftClass*>(pFoot))
		{
			pAir->SetTarget(nullptr);
			pAir->SetDestination(nullptr, true);
			pAir->EnterIdleMode(false, true);
			return;
		}

		const CellStruct home = GetHouseHomeCell(pHouse);

		if (home == CellStruct::Empty)
			return;

		if (auto const pHome = ScoutLandCellInZone(home, IsAirborneThreatTechno(pFoot) ? -1 : ObjectZone(pFoot, MovementZone::Normal), 5))
			IssueMoveOrder(pFoot, pHome, 4.0);
	}

	// Ground scouts switch to the move at once: a queued move left an idle soldier on area guard
	// (the AI's idle mission) where it stood.
	void OrderScoutMove(FootClass* pFoot, CellClass* pCell, bool air)
	{
		if (air)
		{
			pFoot->SetTarget(nullptr);
			IssueMoveOrder(pFoot, pCell, 0.0);
			return;
		}

		pFoot->SetTarget(nullptr);
		pFoot->SetDestination(pCell, true);
		pFoot->ForceMission(Mission::Move);
		CountDecision(NeuralDecision::MoveOrderIssued, pFoot->Owner ? pFoot->Owner->ArrayIndex : -1);
	}

	void SendScoutLeg(HouseClass* pHouse, NeuralScoutTrip& trip, FootClass* pFoot, const NeuralScoutChoice& choice)
	{
		const CellStruct from = pFoot->GetMapCoords();
		trip.Target = choice.Cell->MapCoords;
		trip.Kind = choice.Candidate->Kind;
		trip.LegFrame = Unsorted::CurrentFrame;
		trip.LegDistance = static_cast<int>(from.DistanceFrom(trip.Target));
		trip.Reissues = 0;
		trip.LastCoords = from;

		OrderScoutMove(pFoot, choice.Cell, trip.Air);
		CountScoutKind(trip.Kind, pHouse->ArrayIndex);

		Debug::Log("[NeuralAI] House %d Scout: %s %s leg %d to %s (%d,%d), value %d, gain %d, %d cells, frame %d.\n",
			pHouse->ArrayIndex, pFoot->GetTechnoType()->ID, trip.Air ? "(air)" : "(ground)", trip.Legs + 1,
			NeuralScoutKindNames[static_cast<size_t>(trip.Kind)], trip.Target.X, trip.Target.Y, choice.Candidate->Value,
			choice.Candidate->Gain, trip.LegDistance, Unsorted::CurrentFrame);
	}

	bool IsNearScoutSpot(const std::vector<std::pair<CellStruct, int>>& spots, const CellStruct& cell)
	{
		return std::any_of(spots.begin(), spots.end(),
			[&cell](const std::pair<CellStruct, int>& spot) { return spot.first.DistanceFrom(cell) <= spot.second + 4; });
	}

	// Telemetry: enemy buildings that became known near a scout of ours (its sight plus 4 cells).
	void CountScoutFoundBuildings(HouseClass* pHouse, NeuralScoutState& state, const std::vector<std::pair<CellStruct, int>>& scoutSpots)
	{
		for (auto const& info : GetKnownEnemyBuildingCache(pHouse).Entries)
		{
			if (state.CountedBuildings.insert(info.Building->UniqueID).second && IsNearScoutSpot(scoutSpots, info.Coords))
				CountDecision(NeuralDecision::ScoutBuildingFound, pHouse->ArrayIndex);
		}
	}

	// Telemetry only, never read by a decision (it looks at every enemy building, seen or not): how
	// long enemy expansions (refineries and construction yards 24+ cells from their owner's start)
	// stay unknown to this house. enemyExpDelayFrames / enemyExpKnown is the mean delay until one is
	// known; enemyExpBlindFrames adds up the time each existing expansion was unknown.
	void UpdateEnemyExpansionTelemetry(HouseClass* pHouse, NeuralScoutState& state, const std::vector<std::pair<CellStruct, int>>& scoutSpots)
	{
		const int now = Unsorted::CurrentFrame;
		const int elapsed = state.ExpansionCheckFrame < 0 ? 0 : std::max(0, now - state.ExpansionCheckFrame);
		state.ExpansionCheckFrame = now;
		uint32_t unknown = 0;

		for (auto const pBld : BuildingClass::Array)
		{
			if (!pBld || pBld->InLimbo || !pBld->IsAlive || !pBld->Type || !pBld->Owner || pBld->Owner == pHouse
				|| pHouse->IsAlliedWith(pBld->Owner) || pBld->Owner->IsNeutral() || pBld->Owner->BaseSpawnCell == CellStruct::Empty
				|| !(pBld->Type->Refinery || pBld->Type->ResourceDestination || pBld->Type->Factory == AbstractType::BuildingType))
			{
				continue;
			}

			const CellStruct coords = pBld->GetMapCoords();

			if (coords.DistanceFrom(pBld->Owner->BaseSpawnCell) < 24.0)
				continue;

			const int born = state.ExpansionBornFrame.try_emplace(pBld->UniqueID, now).first->second;

			if (!IsEnemyBuildingKnown(pHouse, pBld))
			{
				unknown++;
				continue;
			}

			if (!state.ExpansionKnown.insert(pBld->UniqueID).second)
				continue;

			const bool byScout = IsNearScoutSpot(scoutSpots, coords);
			CountDecision(NeuralDecision::EnemyExpansionKnown, pHouse->ArrayIndex);
			AddDecisionAmount(NeuralDecision::EnemyExpansionDelay, pHouse->ArrayIndex, static_cast<uint32_t>(now - born));

			if (byScout)
				CountDecision(NeuralDecision::EnemyExpansionByScout, pHouse->ArrayIndex);

			Debug::Log("[NeuralAI] House %d EnemyExpansion: %s of house %d at (%d,%d) known after %d frames%s, frame %d.\n",
				pHouse->ArrayIndex, pBld->Type->ID, pBld->Owner->ArrayIndex, coords.X, coords.Y, now - born,
				byScout ? " (scout)" : "", now);
		}

		AddDecisionAmount(NeuralDecision::EnemyExpansionBlindFrames, pHouse->ArrayIndex, unknown * static_cast<uint32_t>(elapsed));
	}

	// Purposeful scouting, every 45 frames: follows the scouts on their way (lost, hurt, late,
	// arrived: next leg up to 3, then home), then sends a new scout to the best target when a slot
	// is free. One scout per house, two in FFA or with a big idle army, three while the
	// information is very stale (no enemy building known after 4500 frames, or 60% of the free ore
	// fields never explored after 9000 frames). At most one new scout every 240 frames.
	void UpdatePurposefulScouting(HouseClass* pHouse)
	{
		if (!pHouse || !ScenarioClass::Instance)
			return;

		auto& state = GetScoutState(pHouse);
		auto& macro = GetMacroState(pHouse);
		const int now = Unsorted::CurrentFrame;

		// Plans, waves and the micro leave scouts alone. The plans rebuild this set every AI tick
		// (they run before this), so it is refilled on every call, also between updates.
		for (auto const& trip : state.Trips)
			macro.PlanMembers.insert(trip.ScoutID);

		if (now - state.LastUpdateFrame < ScoutUpdateFrames && now >= state.LastUpdateFrame)
			return;

		state.LastUpdateFrame = now;

		const int houseIdx = pHouse->ArrayIndex;
		const bool fog = IsFogOfWarMatch();
		const CellStruct home = GetHouseHomeCell(pHouse);

		UpdateScoutSightBlocks(pHouse, state);

		std::erase_if(state.Visited, [now](const std::pair<CellStruct, int>& spot) { return now - spot.second > 5400; });
		std::erase_if(state.LostAt, [now](const std::pair<CellStruct, int>& spot) { return now - spot.second > 8100; });
		std::erase_if(state.Failed, [now](const std::pair<CellStruct, int>& spot) { return now - spot.second > 27000; });
		std::erase_if(state.Benched, [now](const std::pair<DWORD, int>& bench) { return now - bench.second > 9000; });

		std::vector<std::pair<CellStruct, int>> scoutSpots;
		std::vector<std::pair<NeuralScoutTrip, FootClass*>> nextLegs;
		std::vector<NeuralScoutTrip> keep;

		for (auto trip : state.Trips)
		{
			FootClass* const pFoot = FindOwnScoutFoot(pHouse, trip.ScoutID);

			if (!pFoot)
			{
				CountDecision(NeuralDecision::ScoutLost, houseIdx);
				state.LostAt.emplace_back(trip.LastCoords, now);
				Debug::Log("[NeuralAI] House %d Scout: lost at (%d,%d) on leg %d to %s (%d,%d), frame %d.\n", houseIdx,
					trip.LastCoords.X, trip.LastCoords.Y, trip.Legs + 1, NeuralScoutKindNames[static_cast<size_t>(trip.Kind)],
					trip.Target.X, trip.Target.Y, now);
				continue;
			}

			// Loaded into a transport or a building: released.
			if (pFoot->InLimbo)
				continue;

			trip.LastCoords = pFoot->GetMapCoords();
			scoutSpots.emplace_back(trip.LastCoords, trip.Sight);

			// Arrived when there or when its sight has already taught us what the look was for.
			const double dist = trip.LastCoords.DistanceFrom(trip.Target);
			const bool arrived = dist <= 3.0 || (dist <= std::max(3, trip.Sight / 2) && IsCellInSightOfAI(pHouse, trip.Target))
				|| ScoutInfoGain(pHouse, state, trip.Target, ScoutLookRadius, fog) < 20;
			const bool hurt = pFoot->GetHealthPercentage() < std::min(0.5, trip.StartHealth * 0.6);
			// Speed 4 crosses a cell in about 64 frames: allow 1.5 times that, plus 900 frames.
			const bool late = now - trip.LegFrame > 900 + trip.LegDistance * 400 / std::max(1, trip.Speed);

			if (arrived || hurt || late)
			{
				state.Visited.emplace_back(trip.Target, now);

				// Where a scout got badly hurt is avoided like where one died.
				if (hurt)
					state.LostAt.emplace_back(trip.LastCoords, now);

				if (!arrived)
				{
					if (late)
						state.Failed.emplace_back(trip.Target, now);

					Debug::Log("[NeuralAI] House %d Scout: %s gave up on %s (%d,%d), %s, %d cells short, frame %d.\n", houseIdx,
						pFoot->GetTechnoType()->ID, NeuralScoutKindNames[static_cast<size_t>(trip.Kind)], trip.Target.X, trip.Target.Y,
						hurt ? "hurt" : "late", static_cast<int>(dist), now);
				}

				if (arrived && !hurt && trip.Legs + 1 < ScoutMaxLegs)
					nextLegs.emplace_back(trip, pFoot);
				else
					SendScoutHome(pHouse, pFoot);

				continue;
			}

			// Some other order or the engine stopped it short: send it on again, twice at most.
			const bool moving = pFoot->CurrentMission == Mission::Move || pFoot->QueuedMission == Mission::Move;

			if (!moving)
			{
				if (trip.Reissues >= 2)
				{
					const bool neverMoved = dist >= trip.LegDistance - 1;
					state.Visited.emplace_back(trip.Target, now);

					if (neverMoved)
						state.Benched.emplace_back(trip.ScoutID, now);
					else
						state.Failed.emplace_back(trip.Target, now);

					Debug::Log("[NeuralAI] House %d Scout: %s gave up on %s (%d,%d), %s (mission %d), %d cells short, frame %d.\n",
						houseIdx, pFoot->GetTechnoType()->ID, NeuralScoutKindNames[static_cast<size_t>(trip.Kind)], trip.Target.X,
						trip.Target.Y, neverMoved ? "never moved" : "stopped", static_cast<int>(pFoot->CurrentMission), static_cast<int>(dist), now);
					continue;
				}

				if (auto const pCell = MapClass::Instance.TryGetCellAt(trip.Target))
				{
					trip.Reissues++;
					OrderScoutMove(pFoot, pCell, trip.Air);
				}
			}

			keep.push_back(trip);
		}

		state.Trips = std::move(keep);

		CountScoutFoundBuildings(pHouse, state, scoutSpots);
		UpdateEnemyExpansionTelemetry(pHouse, state, scoutSpots);

		const bool launchWindow = now >= 450 && now - state.LastLaunchFrame >= ScoutLaunchCooldownFrames;

		if (nextLegs.empty() && (!launchWindow || state.Trips.size() >= 3))
			return;

		std::vector<NeuralScoutSighting> sightings;
		CollectScoutSightings(pHouse, sightings);

		// Anti-air units seen in the last 1800 frames (air lane safety).
		std::vector<CellStruct> recentAntiAir;

		for (auto const& sighting : sightings)
		{
			if (sighting.AntiAir && sighting.Age < 1800)
				recentAntiAir.push_back(sighting.Cell);
		}

		std::vector<NeuralScoutCandidate> candidates;
		int freeFields = 0;
		int unexploredFields = 0;
		CollectScoutCandidates(pHouse, state, home, fog, sightings, candidates, freeFields, unexploredFields);

		// Targets worth a trip: kind weight x gain of at least 1500 (a route with 28% to learn).
		std::erase_if(candidates, [](const NeuralScoutCandidate& candidate) { return candidate.Value < 1500; });

		// A target taken by a scout is not offered to the next one.
		auto takeTarget = [&candidates](const CellStruct& cell)
		{
			std::erase_if(candidates, [&cell](const NeuralScoutCandidate& candidate) { return candidate.Cell.DistanceFrom(cell) <= 12.0; });
		};

		for (auto& [trip, pFoot] : nextLegs)
		{
			const std::vector<NeuralScoutUnit> self { MakeScoutUnit(pFoot, trip.Air) };
			const NeuralScoutChoice choice = PickScoutTarget(pHouse, state, candidates, self, recentAntiAir);

			if (!choice.Unit)
			{
				SendScoutHome(pHouse, pFoot);
				continue;
			}

			trip.Legs++;
			trip.Speed = choice.Unit->Speed;
			SendScoutLeg(pHouse, trip, pFoot, choice);
			state.Trips.push_back(trip);
			takeTarget(trip.Target);
		}

		int enemies = 0;

		for (auto const pOther : HouseClass::Array)
		{
			if (pOther && pOther != pHouse && !pOther->Defeated && !pOther->IsNeutral() && !pOther->IsObserver()
				&& !(pOther->Type && pOther->Type->MultiplayPassive) && !pHouse->IsAlliedWith(pOther))
			{
				enemies++;
			}
		}

		std::vector<NeuralScoutUnit> ground;
		std::vector<NeuralScoutUnit> air;
		CollectScoutUnits(pHouse, state, ground, air);

		const bool veryStale = (GetKnownEnemyBuildingCache(pHouse).Entries.empty() && now > 4500)
			|| (freeFields >= 4 && unexploredFields * 10 >= freeFields * 6 && now > 9000);
		const size_t limit = veryStale ? 3 : ((enemies >= 2 || ground.size() >= 8) ? 2 : 1);

		if (launchWindow && state.Trips.size() < limit && !candidates.empty())
		{
			// One idle ground unit stays home, except early on when the first looks matter most.
			if (ground.size() < 2 && now > 9000)
				ground.clear();

			std::vector<NeuralScoutUnit> units(ground.begin(), ground.begin() + std::min<size_t>(ground.size(), 4));
			units.insert(units.end(), air.begin(), air.begin() + std::min<size_t>(air.size(), 2));

			const NeuralScoutChoice choice = PickScoutTarget(pHouse, state, candidates, units, recentAntiAir);

			if (choice.Unit)
			{
				FootClass* const pFoot = choice.Unit->Foot;
				NeuralScoutTrip trip;
				trip.ScoutID = pFoot->UniqueID;
				trip.Air = choice.Unit->Air;
				trip.Sight = std::clamp(static_cast<int>(pFoot->GetTechnoType()->Sight), 1, 24);
				trip.StartHealth = pFoot->GetHealthPercentage();
				trip.Speed = choice.Unit->Speed;
				SendScoutLeg(pHouse, trip, pFoot, choice);
				state.Trips.push_back(trip);
				state.LastLaunchFrame = now;
				CountDecision(NeuralDecision::ScoutTrip, houseIdx);

				if (trip.Air)
					CountDecision(NeuralDecision::ScoutAirTrip, houseIdx);
			}
		}

		// New scouts join the plan members right away.
		for (auto const& trip : state.Trips)
			macro.PlanMembers.insert(trip.ScoutID);
	}
} // namespace

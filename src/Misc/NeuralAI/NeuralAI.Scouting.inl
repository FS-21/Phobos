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
		EnemyArmy,
		Sweep
	};

	constexpr const char* NeuralScoutKindNames[] = { "start", "field", "route", "enemyBase", "enemyArmy", "sweep" };

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

	// A mobile sensor array on its way to a lookout (SensorLookoutMinSight).
	struct NeuralSensorTrip
	{
		DWORD ID { 0 };
		CellStruct Target { CellStruct::Empty };
		int Frame { 0 };
	};

	// A known enemy building area for a later remnant search: its cell, owner and the frame it was last known.
	struct NeuralRemnantLead
	{
		CellStruct Cell { CellStruct::Empty };
		int Owner { -1 };
		int Frame { 0 };
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
		// Area reveal superweapons (PickRevealTarget): the frame before which a waiting one is not
		// weighed again, and whether the wait was logged since the last reveal.
		int RevealCheckFrame { -1 };
		bool RevealHeld { false };
		// Places remnant searchers were sent to (cell, frame), kept RemnantSearchMemoryFrames.
		std::vector<std::pair<CellStruct, int>> RemnantSent {};
		// Remnant searchers on their way: UniqueID -> (place, frame sent), RemnantSearchKeepFrames at most.
		std::unordered_map<DWORD, std::pair<CellStruct, int>> RemnantScouts {};
		// The last known enemy buildings (one an area, the newest first, RemnantLeadsMost at most), the
		// last frame they were noted, and the last frame of the "no place" line.
		std::vector<NeuralRemnantLead> RemnantLeads {};
		int RemnantLeadsFrame { -100000 };
		int RemnantNoPlaceLogFrame { -100000 };
		// Last frame a sweep scout could leave (ScoutSweepEveryFrames).
		int LastSweepFrame { -100000 };
		// Whether an enemy object was in our sight at the last update.
		bool EnemyInSight { true };
		// Mobile sensor arrays on their way to a lookout, the last frame lookouts were weighed, and
		// our unfolded sensor arrays with the frame each was first seen standing (UniqueID order).
		std::vector<NeuralSensorTrip> SensorTrips {};
		int SensorCheckFrame { -100000 };
		std::vector<std::pair<DWORD, int>> SensorStands {};
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

	// Whether a cell lies in the enemy gap runs of GetEnemyGapRuns (first and last cell index of each
	// run, ascending).
	bool IsInGapRuns(const std::vector<size_t>& runs, const CellStruct& cell)
	{
		const size_t idx = static_cast<size_t>(cell.Y) * VisionMapSide + cell.X;
		const size_t pos = static_cast<size_t>(std::upper_bound(runs.begin(), runs.end(), idx) - runs.begin());

		return (pos & 1) != 0 || (pos > 0 && runs[pos - 1] == idx);
	}

	// Ground a look can teach about: on the map and inside its playable area, the cell's height
	// counted. The engine never shows the margins, so they stay unexplored for good (match #11110:
	// the searchers went to a corner of the top margin for 22000 frames).
	bool IsScoutableCell(const CellStruct& cell)
	{
		return MapClass::Instance.CoordinatesLegal(cell) && MapClass::Instance.IsWithinUsableArea(cell, true);
	}

	// A mobile sensor array: a vehicle that unfolds into a building whose sensors reach far.
	bool IsSensorLookoutType(const UnitTypeClass* pType)
	{
		return pType && pType->DeploysInto && pType->DeploysInto->SensorArray && pType->DeploysInto->SensorsSight >= SensorLookoutMinSight;
	}

	// How far a type finds what this need hides, in cells (Detection*; INI flags only). Cloaked,
	// disguised and submerged enemies (stealth, sub): SensorsSight with Sensors or SensorArray (its
	// Sight when SensorsSight is 0), DetectDisguiseRange with DetectDisguise, PsychicDetectionRadius.
	// The dark (blind): the engine's sensors reveal hidden units, not the shroud, so only the Sight from
	// SensorLookoutMinSight counts. A vehicle counts the building it unfolds into too.
	int DetectionReachCells(TechnoTypeClass* pType, int need)
	{
		if (!pType || !need)
			return 0;

		auto const pBldType = abstract_cast<BuildingTypeClass*>(pType);
		int reach = 0;

		if (need & (DetectionNeedStealth | DetectionNeedSub))
		{
			if (pType->Sensors || (pBldType && pBldType->SensorArray))
				reach = std::max(reach, pType->SensorsSight > 0 ? pType->SensorsSight : pType->Sight);

			if (pType->DetectDisguise && (need & DetectionNeedStealth))
				reach = std::max(reach, std::clamp(pType->DetectDisguiseRange, 1, 64));

			if (pBldType && (need & DetectionNeedStealth))
				reach = std::max(reach, pBldType->PsychicDetectionRadius);
		}

		if ((need & DetectionNeedBlind) && pType->Sight >= SensorLookoutMinSight)
			reach = std::max(reach, pType->Sight);

		if (auto const pUnitType = abstract_cast<UnitTypeClass*>(pType); pUnitType && pUnitType->DeploysInto)
			reach = std::max(reach, DetectionReachCells(pUnitType->DeploysInto, need));

		return std::clamp(reach, 0, 64);
	}

	// What this house needs detection for now (DetectionNeed*: blind, stealth, sub; Detection*).
	int DetectionNeedOf(HouseClass* pHouse)
	{
		if (!pHouse)
			return 0;

		auto const& macro = GetMacroState(pHouse);
		const int now = Unsorted::CurrentFrame;
		int need = 0;
		bool enemiesAlive = false;

		for (auto const pOther : HouseClass::Array)
		{
			if (pOther && pOther != pHouse && !pOther->Defeated && !pOther->IsObserver() && !pOther->IsNeutral() && !pHouse->IsAlliedWith(pOther)
				&& (pOther->OwnedBuildings > 0 || pOther->OwnedUnits > 0 || pOther->OwnedInfantry > 0))
			{
				enemiesAlive = true;
				break;
			}
		}

		if (enemiesAlive && ((now >= DetectionBlindFromFrame && now - macro.LastEnemySeenFrame >= DetectionBlindFrames)
			|| now - macro.RemnantSearchFrame < 2 * RemnantSearchEveryFrames))
		{
			need |= DetectionNeedBlind;
		}

		if (std::any_of(macro.StealthSightings.begin(), macro.StealthSightings.end(),
			[now](const auto& seen) { return now - seen.second.second <= SensorLookoutSightingFrames; }))
		{
			need |= DetectionNeedStealth;
		}

		for (auto const& loss : macro.HiddenLosses)
		{
			if (now - loss.Frame <= SensorLookoutSightingFrames)
				need |= loss.Kind == 2 ? DetectionNeedSub : (loss.Kind == 1 ? DetectionNeedStealth : DetectionNeedBlind);
		}

		if (macro.SubThreatUntil > now || macro.NavalRemnantUntil > now)
			need |= DetectionNeedSub;

		return need;
	}

	// Is a recent stealth sighting or hidden loss of this house out of the reach of every detector of
	// ours (vehicles and buildings)? A sensor building is wanted for it.
	bool DetectionSpotUncovered(HouseClass* pHouse)
	{
		auto const& macro = GetMacroState(pHouse);
		const int now = Unsorted::CurrentFrame;
		std::vector<CellStruct> spots;

		for (auto const& [uid, seen] : macro.StealthSightings)
		{
			if (now - seen.second <= SensorLookoutSightingFrames)
				spots.push_back(seen.first);
		}

		for (auto const& loss : macro.HiddenLosses)
		{
			if (now - loss.Frame <= SensorLookoutSightingFrames && loss.Kind == 1)
				spots.push_back(loss.Cell);
		}

		if (spots.empty())
			return false;

		std::vector<std::pair<CellStruct, int>> detectors;

		for (auto const pBld : pHouse->Buildings)
		{
			if (pBld && pBld->IsAlive && !pBld->InLimbo && pBld->Type)
			{
				if (const int reach = DetectionReachCells(pBld->Type, DetectionNeedStealth); reach > 0)
					detectors.push_back({ pBld->GetMapCoords(), reach });
			}
		}

		for (auto const pUnit : UnitClass::Array)
		{
			if (pUnit && pUnit->Owner == pHouse && pUnit->IsAlive && !pUnit->InLimbo && pUnit->Type)
			{
				if (const int reach = DetectionReachCells(pUnit->Type, DetectionNeedStealth); reach > 0)
					detectors.push_back({ pUnit->GetMapCoords(), reach });
			}
		}

		return std::any_of(spots.begin(), spots.end(), [&detectors](const CellStruct& spot)
			{
				return std::none_of(detectors.begin(), detectors.end(),
					[&spot](const auto& detector) { return detector.first.DistanceFrom(spot) <= detector.second; });
			});
	}

	// What a look at center would teach us, 0..100: ground never explored (or shrouded again by an
	// enemy gap) counts fully, explored ground with fog of war by the time since it was last in sight.
	// Without fog of war the engine shows explored ground live (and the AI's known buildings and
	// sightings use it), so a look there teaches nothing: with the map revealed ("Reveal map", a Spy
	// Satellite) only enemy gaps are left to scout. With pGapRuns (an area reveal, which the engine
	// shrouds again at once under an enemy gap) the cells under an enemy gap teach nothing. Cells
	// outside the playable area (IsScoutableCell) are left out.
	int ScoutInfoGain(HouseClass* pHouse, const NeuralScoutState& state, const CellStruct& center, int radius, bool fog,
		const std::vector<size_t>* pGapRuns = nullptr)
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
				auto const pCell = IsScoutableCell(cell) ? MapClass::Instance.TryGetCellAt(cell) : nullptr;

				if (!pCell)
					continue;

				samples++;

				if (pGapRuns && IsInGapRuns(*pGapRuns, cell))
					continue;

				if (!IsCellVisibleToAI(pHouse, pCell))
				{
					total += 100;
					continue;
				}

				if (!fog)
					continue;

				const int seen = state.BlockSeenFrame[ScoutBlockIndex(cell)];
				total += std::min(100, (seen < 0 ? now : now - seen) * 100 / ScoutStaleFrames);
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
			NeuralDecision::ScoutRoute, NeuralDecision::ScoutBase, NeuralDecision::ScoutArmy, NeuralDecision::ScoutSweep };

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
	// how many of them were never explored. With revealRadius above 0 the places are valued for an area
	// reveal of that radius instead: the place itself, by what the disc would teach (the cells under an
	// enemy gap left out), whatever scouts do or met there. With sweep the places of a grid over the
	// playable land are weighed too (ScoutSweepWeight). Only playable ground is a target.
	void CollectScoutCandidates(HouseClass* pHouse, const NeuralScoutState& state, const CellStruct& home, bool fog,
		const std::vector<NeuralScoutSighting>& sightings, std::vector<NeuralScoutCandidate>& out, int& freeFields,
		int& unexploredFields, int revealRadius = 0, bool sweep = false)
	{
		auto const& macro = GetMacroState(pHouse);
		auto const& known = GetKnownEnemyBuildingCache(pHouse).Entries;
		const int now = Unsorted::CurrentFrame;
		const bool reveal = revealRadius > 0;
		const std::vector<size_t> gapRuns = reveal ? GetEnemyGapRuns(pHouse) : std::vector<size_t> {};

		auto add = [&](NeuralScoutKind kind, const CellStruct& cell, int weight)
		{
			if (cell == CellStruct::Empty || !IsScoutableCell(cell))
				return;

			if (reveal)
			{
				const int gain = ScoutInfoGain(pHouse, state, cell, revealRadius, fog, &gapRuns);

				if (gain >= 20)
					out.push_back({ cell, kind, gain, weight * gain });

				return;
			}

			const CellStruct look = ScoutStandoffCell(pHouse, cell, home);

			if (look == CellStruct::Empty || !IsScoutableCell(look))
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

		// Where enemy armies were last seen (unseen for 300+ frames, on ground we no longer know, away
		// from our base): the biggest three groups (value within 10 cells of the strongest unit, at
		// least 100).
		std::vector<const NeuralScoutSighting*> armies;

		for (auto const& sighting : sightings)
		{
			if (!sighting.Air && sighting.Age >= 300 && sighting.Age <= EnemySightingMemoryFrames
				&& (home == CellStruct::Empty || sighting.Cell.DistanceFrom(home) > 24.0) && !IsCellKnownToAI(pHouse, sighting.Cell))
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

		// Sweep: the land of a grid over the playable area, what is never looked at otherwise.
		if (sweep)
		{
			for (int y = ScoutSweepStepCells / 2; y < VisionMapSide; y += ScoutSweepStepCells)
			{
				for (int x = ScoutSweepStepCells / 2; x < VisionMapSide; x += ScoutSweepStepCells)
				{
					const CellStruct cell { static_cast<short>(x), static_cast<short>(y) };

					if (!IsScoutableCell(cell))
						continue;

					auto const pCell = MapClass::Instance.TryGetCellAt(cell);

					if (pCell && pCell->LandType != LandType::Water)
						add(NeuralScoutKind::Sweep, cell, ScoutSweepWeight);
				}
			}
		}
	}

	// Where an area reveal of this radius teaches the most: the places scouts weigh (unchecked starts,
	// free ore fields, the known enemy bases and the ground around them, the approaches to our base,
	// the armies last seen), valued as kind weight x what the disc would teach, from 1500 as for a
	// scout trip. Empty when no place is worth it: the superweapon waits and is weighed again 150
	// frames later.
	CellStruct PickRevealTarget(HouseClass* pHouse, int radius)
	{
		auto& state = GetScoutState(pHouse);
		const int now = Unsorted::CurrentFrame;

		if (now < state.RevealCheckFrame)
			return CellStruct::Empty;

		std::vector<NeuralScoutSighting> sightings;
		CollectScoutSightings(pHouse, sightings);
		std::vector<NeuralScoutCandidate> candidates;
		int freeFields = 0;
		int unexploredFields = 0;
		CollectScoutCandidates(pHouse, state, GetHouseHomeCell(pHouse), IsFogOfWarMatch(), sightings, candidates, freeFields,
			unexploredFields, std::max(1, radius));

		const NeuralScoutCandidate* pBest = nullptr;

		for (auto const& candidate : candidates)
		{
			if (!pBest || candidate.Value > pBest->Value)
				pBest = &candidate;
		}

		if (!pBest || pBest->Value < 1500)
		{
			state.RevealCheckFrame = now + 150;

			if (!state.RevealHeld)
			{
				state.RevealHeld = true;
				Debug::Log("[NeuralAI] House %d Reveal: nothing worth a reveal of radius %d (best value %d), the superweapon waits, frame %d.\n",
					pHouse->ArrayIndex, radius, pBest ? pBest->Value : 0, now);
			}

			return CellStruct::Empty;
		}

		state.RevealHeld = false;
		Debug::Log("[NeuralAI] House %d Reveal: %s (%d,%d), gain %d, value %d, frame %d.\n", pHouse->ArrayIndex,
			NeuralScoutKindNames[static_cast<size_t>(pBest->Kind)], pBest->Cell.X, pBest->Cell.Y, pBest->Gain, pBest->Value, now);

		return pBest->Cell;
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

	// Where a warship on a naval sweep looks (user approved, 2026-11-19; the micro's naval sweep): a place of a
	// NavalSweepStepCells grid over the playable water of its own sea, out of known enemy defense reach (2 cells more),
	// at least half a step from the ship and a whole step from the places other sweeps look at (taken). A place never
	// explored by the house first, then the one a look teaches most about (ScoutInfoGain: ground unseen longest), the
	// nearest to the ship among equals, the first of the grid after that. Empty when no place teaches anything.
	CellStruct PickNavalSweepCell(HouseClass* pHouse, FootClass* pShip, const std::vector<CellStruct>& taken)
	{
		auto const pType = pShip ? pShip->GetTechnoType() : nullptr;

		if (!pHouse || !pType)
			return CellStruct::Empty;

		const MovementZone movementZone = pType->MovementZone;
		const int sea = ObjectZone(pShip, movementZone);

		if (sea < 0)
			return CellStruct::Empty;

		auto const& state = GetScoutState(pHouse);
		const bool fog = IsFogOfWarMatch();
		const CellStruct from = pShip->GetMapCoords();
		CellStruct best = CellStruct::Empty;
		int bestScore = INT_MIN;

		for (int y = NavalSweepStepCells / 2; y < VisionMapSide; y += NavalSweepStepCells)
		{
			for (int x = NavalSweepStepCells / 2; x < VisionMapSide; x += NavalSweepStepCells)
			{
				const CellStruct cell { static_cast<short>(x), static_cast<short>(y) };

				if (!IsScoutableCell(cell) || MapClass::Instance.GetMovementZoneType(cell, movementZone, false) != sea
					|| cell.DistanceFrom(from) < NavalSweepStepCells / 2)
				{
					continue;
				}

				auto const pCell = MapClass::Instance.TryGetCellAt(cell);

				if (!pCell || std::any_of(taken.begin(), taken.end(), [&cell](const CellStruct& other) { return other.DistanceFrom(cell) < NavalSweepStepCells; }))
					continue;

				const int gain = ScoutInfoGain(pHouse, state, cell, ScoutLookRadius, fog);

				if (gain < 20)
					continue;

				const int score = (IsCellVisibleToAI(pHouse, pCell) ? 0 : 1000) + gain * 10 - static_cast<int>(cell.DistanceFrom(from));

				// The defense reach test last, for a place that would be the best one only.
				if (score > bestScore && !IsCellInKnownEnemyDefenseReach(pHouse, cell, 2))
				{
					bestScore = score;
					best = cell;
				}
			}
		}

		return best;
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
			// A unit of a transport operation is no scout until the operation hands it back.
			return pFoot && pFoot->Owner == pHouse && pFoot->IsAlive && !pFoot->InLimbo && !macro.PlanMembers.contains(pFoot->UniqueID)
				&& !IsInTransportOp(pHouse, pFoot->UniqueID) && !IsScoutTripUnit(state, pFoot->UniqueID)
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

		// The tank guard (UpdateTankGuard) stays at home unless the house has no other scout.
		for (auto const pUnit : UnitClass::Array)
		{
			if (pUnit && isIdleGround(pUnit, pUnit->Type) && !pUnit->Type->Harvester && !pUnit->Type->ResourceGatherer
				&& !pUnit->Type->DeploysInto && !IsAmphibiousOrAirTransport(pUnit->Type) && pUnit->Passengers.NumPassengers == 0
				&& !macro.TankGuard.contains(pUnit->UniqueID) && !IsRepairerUnit(pUnit))
			{
				ground.push_back(MakeScoutUnit(pUnit, false));
			}
		}

		// A slave with a miner belongs to it; a freed one is a cheap soldier like any other.
		for (auto const pInf : InfantryClass::Array)
		{
			if (pInf && isIdleGround(pInf, pInf->Type) && !pInf->Type->Engineer && !pInf->SlaveOwner && !IsSpyInfantryType(pInf->Type)
				&& !IsHealerInfantryType(pInf->Type))
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

		// No other scout: one tank of the guard goes, the fittest (user: a floor tank can scout when the
		// house built nothing faster), whatever it cost.
		if (ground.empty() && air.empty())
		{
			for (auto const pUnit : UnitClass::Array)
			{
				if (pUnit && macro.TankGuard.contains(pUnit->UniqueID) && isFree(pUnit) && pUnit->Type->Speed >= 4 && pUnit->Type->Sight >= 4
					&& pUnit->Target == nullptr && pUnit->GetHealthPercentage() >= 0.6
					&& (pUnit->CurrentMission == Mission::Guard || pUnit->CurrentMission == Mission::Area_Guard
						|| pUnit->CurrentMission == Mission::Sleep || pUnit->CurrentMission == Mission::Move))
				{
					ground.push_back(MakeScoutUnit(pUnit, false));
				}
			}

			if (ground.size() > 1)
			{
				std::sort(ground.begin(), ground.end(), byFitness);
				ground.resize(1);
			}
		}

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
	// (the AI's idle mission) where it stood. Never one its war factory still unloads (IsLeavingItsFactory).
	void OrderScoutMove(FootClass* pFoot, CellClass* pCell, bool air)
	{
		if (air)
		{
			pFoot->SetTarget(nullptr);
			IssueMoveOrder(pFoot, pCell, 0.0);
			return;
		}

		if (IsLeavingItsFactory(pFoot))
			return;

		pFoot->SetTarget(nullptr);
		pFoot->SetDestination(pCell, true);
		pFoot->ForceMission(Mission::Move);
		NoteOrderCaller(pFoot, "move");
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
	// fields never explored after 9000 frames). At most one new scout every 240 frames. While no
	// enemy object is in sight, one more every ScoutSweepEveryFrames up to ScoutSweepMostTrips, the
	// grid of sweep places weighed too, and the sensor lookouts move (UpdateSensorLookouts).
	// Places to look for what is left of an enemy, at most this many, the best first (nearer to home
	// among equals): the last known enemy buildings of houses still in the match and the free ore
	// fields first (RemnantLead*), then playable land cells of a grid, by what their surroundings
	// teach. Only places that teach RemnantSearchMinGain or more while there are any, otherwise the
	// stalest ones. A place searchers went to in the last RemnantSearchSpreadFrames, or one a searcher
	// is still on its way to, is left to the next ones; one they went to RemnantSearchMostSends times
	// within RemnantSearchMemoryFrames is out of their reach. One searcher a place: places closer than
	// half a grid step count once. The places returned are noted as sent to.
	std::vector<CellStruct> FindRemnantSearchCells(HouseClass* pHouse, size_t most)
	{
		auto& state = GetScoutState(pHouse);
		const int now = Unsorted::CurrentFrame;
		const bool fog = IsFogOfWarMatch();
		const CellStruct home = GetHouseHomeCell(pHouse);

		struct RemnantPlace
		{
			int Score;
			CellStruct Cell;
			int Gain;
		};

		std::vector<RemnantPlace> found;

		std::erase_if(state.RemnantSent, [now](const std::pair<CellStruct, int>& sent) { return now - sent.second > RemnantSearchMemoryFrames; });

		auto leftAlone = [&state, now](const CellStruct& cell)
		{
			for (auto const& [uid, kept] : state.RemnantScouts)
			{
				if (kept.first.DistanceFrom(cell) <= RemnantSearchStepCells / 2)
					return true;
			}

			int sends = 0;

			for (auto const& [spot, frame] : state.RemnantSent)
			{
				if (spot.DistanceFrom(cell) > RemnantSearchStepCells / 2)
					continue;

				if (now - frame < RemnantSearchSpreadFrames || ++sends >= RemnantSearchMostSends)
					return true;
			}

			return false;
		};

		auto const consider = [&](const CellStruct& cell, int bonus)
		{
			if (!IsScoutableCell(cell) || leftAlone(cell))
				return;

			auto const pCell = MapClass::Instance.TryGetCellAt(cell);

			if (!pCell || pCell->LandType == LandType::Water)
				return;

			const int gain = ScoutInfoGain(pHouse, state, cell, RemnantSearchRadius, fog);

			if (gain > 0)
				found.push_back({ gain * 4 + bonus - static_cast<int>(cell.DistanceFrom(home)), cell, gain });
		};

		std::vector<CellStruct> leads;

		for (auto const& lead : state.RemnantLeads)
		{
			auto const pOwner = lead.Owner >= 0 && lead.Owner < HouseClass::Array.Count ? HouseClass::Array.GetItem(lead.Owner) : nullptr;

			if (pOwner && !pOwner->Defeated && pOwner->Buildings.Count > 0)
			{
				leads.push_back(lead.Cell);
				consider(lead.Cell, RemnantLeadBuildingBonus);
			}
		}

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

			const bool nearLead = leads.empty() || std::any_of(leads.begin(), leads.end(),
				[&field](const CellStruct& lead) { return lead.DistanceFrom(field.Cell) <= RemnantLeadFieldCells; });
			consider(field.Cell, nearLead ? RemnantLeadFieldBonus : RemnantLeadFieldBonus / 2);
		}

		for (int y = RemnantSearchStepCells / 2; y < VisionMapSide; y += RemnantSearchStepCells)
		{
			for (int x = RemnantSearchStepCells / 2; x < VisionMapSide; x += RemnantSearchStepCells)
				consider(CellStruct { static_cast<short>(x), static_cast<short>(y) }, 0);
		}

		if (std::any_of(found.begin(), found.end(), [](const RemnantPlace& place) { return place.Gain >= RemnantSearchMinGain; }))
			std::erase_if(found, [](const RemnantPlace& place) { return place.Gain < RemnantSearchMinGain; });

		std::sort(found.begin(), found.end(), [](const RemnantPlace& a, const RemnantPlace& b)
			{
				if (a.Score != b.Score)
					return a.Score > b.Score;

				return a.Cell.X != b.Cell.X ? a.Cell.X < b.Cell.X : a.Cell.Y < b.Cell.Y;
			});

		std::vector<CellStruct> cells;

		for (auto const& place : found)
		{
			if (cells.size() >= most)
				break;

			if (std::any_of(cells.begin(), cells.end(),
				[&place](const CellStruct& taken) { return taken.DistanceFrom(place.Cell) <= RemnantSearchStepCells / 2; }))
			{
				continue;
			}

			cells.push_back(place.Cell);
			state.RemnantSent.emplace_back(place.Cell, now);
		}

		return cells;
	}

	// Outside the remnant phase: the searchers are free again, and the known enemy buildings are noted
	// as leads for a later search (RemnantLeads), every RemnantLeadsEveryFrames.
	void NoteRemnantLeads(HouseClass* pHouse)
	{
		auto& state = GetScoutState(pHouse);
		const int now = Unsorted::CurrentFrame;
		state.RemnantScouts.clear();

		if (now - state.RemnantLeadsFrame < RemnantLeadsEveryFrames && now >= state.RemnantLeadsFrame)
			return;

		state.RemnantLeadsFrame = now;

		for (auto const& info : GetKnownEnemyBuildingCache(pHouse).Entries)
		{
			if (!info.Building || !info.Building->Owner)
				continue;

			auto const itLead = std::find_if(state.RemnantLeads.begin(), state.RemnantLeads.end(),
				[&info](const NeuralRemnantLead& lead) { return lead.Cell.DistanceFrom(info.Coords) <= RemnantSearchStepCells / 2; });

			if (itLead != state.RemnantLeads.end())
			{
				itLead->Owner = info.Building->Owner->ArrayIndex;
				itLead->Frame = now;
			}
			else
			{
				state.RemnantLeads.push_back({ info.Coords, info.Building->Owner->ArrayIndex, now });
			}
		}

		std::stable_sort(state.RemnantLeads.begin(), state.RemnantLeads.end(),
			[](const NeuralRemnantLead& a, const NeuralRemnantLead& b) { return a.Frame > b.Frame; });

		if (state.RemnantLeads.size() > RemnantLeadsMost)
			state.RemnantLeads.resize(RemnantLeadsMost);
	}

	// In the remnant phase: the searchers still on their way to their place leave the idle pools, so
	// nothing sends them elsewhere; one that came within RemnantSearchArriveCells of it, heads more
	// than that from it (another order took it, or it stopped) or ran out of RemnantSearchKeepFrames
	// is free again. Returns how many are on their way.
	size_t KeepRemnantSearchers(HouseClass* pHouse, std::vector<UnitClass*>& idleUnits, std::vector<InfantryClass*>& idleInf)
	{
		auto& state = GetScoutState(pHouse);
		const int now = Unsorted::CurrentFrame;

		std::erase_if(state.RemnantScouts, [now](const std::pair<const DWORD, std::pair<CellStruct, int>>& kept)
			{ return now - kept.second.second > RemnantSearchKeepFrames || now < kept.second.second; });

		if (state.RemnantScouts.empty())
			return 0;

		size_t onTheirWay = 0;

		auto const keeps = [&state, &onTheirWay](FootClass* pFoot)
		{
			auto const itKept = state.RemnantScouts.find(pFoot->UniqueID);

			if (itKept == state.RemnantScouts.end())
				return false;

			const CellStruct place = itKept->second.first;
			const CellStruct goal = OrderLeaseGoalOf(pFoot);

			if (pFoot->GetMapCoords().DistanceFrom(place) <= RemnantSearchArriveCells || goal == CellStruct::Empty
				|| goal.DistanceFrom(place) > RemnantSearchArriveCells)
			{
				state.RemnantScouts.erase(itKept);
				return false;
			}

			onTheirWay++;
			return true;
		};

		std::erase_if(idleUnits, keeps);
		std::erase_if(idleInf, keeps);

		return onTheirWay;
	}

	// A remnant searcher sent to its place (KeepRemnantSearchers).
	void NoteRemnantSearcher(HouseClass* pHouse, const FootClass* pFoot, const CellStruct& place)
	{
		GetScoutState(pHouse).RemnantScouts[pFoot->UniqueID] = { place, Unsorted::CurrentFrame };
	}

	// Whether the "no place left" line of the remnant search may be written now (one every 2700 frames).
	bool MayLogRemnantNoPlace(HouseClass* pHouse)
	{
		auto& state = GetScoutState(pHouse);
		const int now = Unsorted::CurrentFrame;

		if (now - state.RemnantNoPlaceLogFrame < 2700 && now >= state.RemnantNoPlaceLogFrame)
			return false;

		state.RemnantNoPlaceLogFrame = now;
		return true;
	}

	// Where a sensor array of this sensor radius, unfolded in the given movement zone, covers the most
	// playable ground no sensor of ours covers: places of a ScoutSweepStepCells grid in that zone, out
	// of known enemy defense reach and away from the lookouts in taken, by the uncovered samples
	// (every 5 cells) less a cell per 2 of the way from `from`. Empty when none covers 8 samples.
	CellStruct FindSensorLookout(HouseClass* pHouse, const CellStruct& from, int zone, int radius, const std::vector<CellStruct>& taken)
	{
		const unsigned int houseIdx = static_cast<unsigned int>(pHouse->ArrayIndex);
		CellStruct best = CellStruct::Empty;
		int bestScore = 0;

		for (int y = ScoutSweepStepCells / 2; y < VisionMapSide; y += ScoutSweepStepCells)
		{
			for (int x = ScoutSweepStepCells / 2; x < VisionMapSide; x += ScoutSweepStepCells)
			{
				const CellStruct cell { static_cast<short>(x), static_cast<short>(y) };

				if (!IsScoutableCell(cell))
					continue;

				auto const pCell = MapClass::Instance.TryGetCellAt(cell);

				if (!pCell || pCell->LandType == LandType::Water
					|| MapClass::Instance.GetMovementZoneType(cell, MovementZone::Normal, false) != zone
					|| IsCellInKnownEnemyDefenseReach(pHouse, cell, 2)
					|| std::any_of(taken.begin(), taken.end(), [&cell, radius](const CellStruct& spot) { return spot.DistanceFrom(cell) < radius; }))
				{
					continue;
				}

				int uncovered = 0;

				for (int dy = -radius; dy <= radius; dy += 5)
				{
					for (int dx = -radius; dx <= radius; dx += 5)
					{
						if (dx * dx + dy * dy > radius * radius)
							continue;

						const CellStruct sample { static_cast<short>(x + dx), static_cast<short>(y + dy) };
						auto const pSample = IsScoutableCell(sample) ? MapClass::Instance.TryGetCellAt(sample) : nullptr;

						if (pSample && !pSample->Sensors_InclHouse(houseIdx))
							uncovered++;
					}
				}

				const int score = uncovered * 20 - static_cast<int>(from.DistanceFrom(cell)) / 2;

				if (uncovered >= 8 && score > bestScore)
				{
					bestScore = score;
					best = cell;
				}
			}
		}

		return best;
	}

	// Sensor lookouts (SensorLookoutMinSight), every scouting update: a mobile sensor array on its way
	// unfolds once there (or stopped within 8 cells), is sent on again when stopped short and given up
	// after SensorLookoutLateFrames. Every ScoutSweepEveryFrames an idle one goes where enemy stealth
	// units were seen lately and no sensor of ours covers, else (no enemy in sight) to the best
	// lookout; an unfolded one packs up for such a place SensorLookoutMinMoveCells or more away after
	// SensorLookoutDwellFrames with no enemy in sight, or after ScoutSweepEveryFrames x 2 for a
	// stealth sighting no sensor of ours covers.
	void UpdateSensorLookouts(HouseClass* pHouse, NeuralScoutState& state, bool enemyInSight)
	{
		auto& macro = GetMacroState(pHouse);
		const int now = Unsorted::CurrentFrame;
		const int houseIdx = pHouse->ArrayIndex;
		std::vector<NeuralSensorTrip> keep;

		for (auto const& trip : state.SensorTrips)
		{
			auto const pUnit = abstract_cast<UnitClass*>(FindOwnFootByID(pHouse, trip.ID));

			if (!pUnit || pUnit->InLimbo || !IsSensorLookoutType(pUnit->Type))
				continue;

			const double dist = pUnit->GetMapCoords().DistanceFrom(trip.Target);
			const bool moving = pUnit->CurrentMission == Mission::Move || pUnit->QueuedMission == Mission::Move;

			if (pUnit->IsDeploying || pUnit->CurrentMission == Mission::Unload)
			{
				macro.PlanMembers.insert(trip.ID);
				keep.push_back(trip);
				continue;
			}

			const bool there = dist <= 3.0 || (!moving && dist <= 8.0);

			if (there && UnitExt::CanDeployIntoBuilding(pUnit))
			{
				// UnitClass::TryToDeploy gives up while the unit has a destination.
				pUnit->SetDestination(nullptr, true);
				pUnit->ClearNavigationList();
				pUnit->SetTarget(nullptr);
				pUnit->QueueMission(Mission::Unload, true);
				Debug::Log("[NeuralAI] House %d Sensor: %s unfolds at (%d,%d), frame %d.\n", houseIdx, pUnit->Type->ID,
					pUnit->GetMapCoords().X, pUnit->GetMapCoords().Y, now);
			}
			else if (now - trip.Frame > SensorLookoutLateFrames)
			{
				Debug::Log("[NeuralAI] House %d Sensor: %s gave up on (%d,%d), %d cells short, frame %d.\n", houseIdx, pUnit->Type->ID,
					trip.Target.X, trip.Target.Y, static_cast<int>(dist), now);
				continue;
			}
			else if (there)
			{
				// No room to unfold here: it steps aside and tries again.
				if (!moving)
					pUnit->Scatter(CoordStruct::Empty, true, false);
			}
			else if (!moving)
			{
				if (auto const pCell = MapClass::Instance.TryGetCellAt(trip.Target))
					OrderScoutMove(pUnit, pCell, false);
			}

			macro.PlanMembers.insert(trip.ID);
			keep.push_back(trip);
		}

		state.SensorTrips = std::move(keep);

		if (now - state.SensorCheckFrame < ScoutSweepEveryFrames)
			return;

		state.SensorCheckFrame = now;

		// The newest stealth sighting of the last SensorLookoutSightingFrames no sensor of ours covers
		// (the frame, then the lowest UniqueID: the map's own order is never read).
		CellStruct stealthCell = CellStruct::Empty;
		int stealthFrame = -1;
		DWORD stealthID = 0;

		for (auto const& [uid, seen] : macro.StealthSightings)
		{
			if (now - seen.second > SensorLookoutSightingFrames || seen.first == CellStruct::Empty)
				continue;

			auto const pCell = MapClass::Instance.TryGetCellAt(seen.first);

			if (!pCell || pCell->Sensors_InclHouse(static_cast<unsigned int>(houseIdx)))
				continue;

			if (seen.second > stealthFrame || (seen.second == stealthFrame && uid < stealthID))
			{
				stealthCell = seen.first;
				stealthFrame = seen.second;
				stealthID = uid;
			}
		}

		if (enemyInSight && stealthCell == CellStruct::Empty)
			return;

		const CellStruct home = GetHouseHomeCell(pHouse);
		std::vector<CellStruct> taken;

		for (auto const& trip : state.SensorTrips)
			taken.push_back(trip.Target);

		// Where a sensor array at `from` in `zone` goes next: the stealth sighting (out of known defense
		// reach), else with no enemy in sight the best lookout.
		auto nextLookout = [&](const CellStruct& from, int zone, int radius, bool& forStealth) -> CellStruct
		{
			forStealth = false;

			if (stealthCell != CellStruct::Empty
				&& std::none_of(taken.begin(), taken.end(), [&stealthCell, radius](const CellStruct& spot) { return spot.DistanceFrom(stealthCell) < radius; }))
			{
				const CellStruct look = ScoutStandoffCell(pHouse, stealthCell, home == CellStruct::Empty ? from : home);

				if (look != CellStruct::Empty)
				{
					if (auto const pLand = ScoutLandCellInZone(look, zone, 6))
					{
						forStealth = true;
						return pLand->MapCoords;
					}
				}
			}

			return enemyInSight ? CellStruct::Empty : FindSensorLookout(pHouse, from, zone, radius, taken);
		};

		// Unfolded ones: the frame each was first seen standing.
		std::vector<std::pair<DWORD, int>> stands;

		for (auto const pBld : pHouse->Buildings)
		{
			if (!pBld || !pBld->IsAlive || pBld->InLimbo || !pBld->Type || !pBld->Type->UndeploysInto
				|| !IsSensorLookoutType(pBld->Type->UndeploysInto))
			{
				continue;
			}

			auto const it = std::find_if(state.SensorStands.begin(), state.SensorStands.end(),
				[pBld](const std::pair<DWORD, int>& stand) { return stand.first == pBld->UniqueID; });
			const int since = it != state.SensorStands.end() ? it->second : now;
			stands.emplace_back(pBld->UniqueID, since);

			if (pBld->CurrentMission == Mission::Construction || pBld->CurrentMission == Mission::Selling
				|| pBld->QueuedMission == Mission::Selling || macro.PlanMembers.contains(pBld->UniqueID))
			{
				continue;
			}

			const CellStruct here = pBld->GetMapCoords();
			const int zone = MapClass::Instance.GetMovementZoneType(here, MovementZone::Normal, pBld->OnBridge);
			const int radius = std::max(1, static_cast<int>(pBld->Type->SensorsSight));
			bool forStealth = false;
			const CellStruct next = nextLookout(here, zone, radius, forStealth);

			if (next == CellStruct::Empty || next.DistanceFrom(here) < SensorLookoutMinMoveCells
				|| now - since < (forStealth ? 2 * ScoutSweepEveryFrames : SensorLookoutDwellFrames))
			{
				continue;
			}

			if (auto const pNext = MapClass::Instance.TryGetCellAt(next))
			{
				pBld->SetArchiveTarget(pNext);
				pBld->Sell(1);
				taken.push_back(next);
				Debug::Log("[NeuralAI] House %d Sensor: %s packs up at (%d,%d) for %s (%d,%d) after %d frames, frame %d.\n", houseIdx,
					pBld->Type->ID, here.X, here.Y, forStealth ? "a stealth sighting" : "a lookout", next.X, next.Y, now - since, now);
			}
		}

		std::sort(stands.begin(), stands.end());
		state.SensorStands = std::move(stands);

		// Idle mobile ones go to the next lookout.
		for (auto const pUnit : UnitClass::Array)
		{
			if (!pUnit || pUnit->Owner != pHouse || !pUnit->IsAlive || pUnit->InLimbo || !IsSensorLookoutType(pUnit->Type)
				|| pUnit->IsDeploying || macro.PlanMembers.contains(pUnit->UniqueID) || IsInTransportOp(pHouse, pUnit->UniqueID)
				|| IsScoutTripUnit(state, pUnit->UniqueID) || pUnit->GetHealthPercentage() < 0.5
				|| (pUnit->CurrentMission != Mission::Guard && pUnit->CurrentMission != Mission::Area_Guard
					&& pUnit->CurrentMission != Mission::Sleep))
			{
				continue;
			}

			const int zone = ObjectZone(pUnit, MovementZone::Normal);

			if (zone < 0)
				continue;

			const CellStruct from = pUnit->GetMapCoords();
			const int radius = std::max(1, static_cast<int>(pUnit->Type->DeploysInto->SensorsSight));
			bool forStealth = false;
			const CellStruct next = nextLookout(from, zone, radius, forStealth);
			auto const pNext = next != CellStruct::Empty ? MapClass::Instance.TryGetCellAt(next) : nullptr;

			if (!pNext)
				continue;

			OrderScoutMove(pUnit, pNext, false);
			state.SensorTrips.push_back({ pUnit->UniqueID, next, now });
			macro.PlanMembers.insert(pUnit->UniqueID);
			taken.push_back(next);
			CountDecision(NeuralDecision::SensorLookout, houseIdx);
			Debug::Log("[NeuralAI] House %d Sensor: %s to %s (%d,%d), %d cells, frame %d.\n", houseIdx, pUnit->Type->ID,
				forStealth ? "a stealth sighting" : "a lookout", next.X, next.Y, static_cast<int>(from.DistanceFrom(next)), now);
		}
	}

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

		for (auto const& trip : state.SensorTrips)
			macro.PlanMembers.insert(trip.ID);

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
			FootClass* const pFoot = FindOwnFootByID(pHouse, trip.ScoutID);

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

		// Whether any enemy object stands in our sight now (a cloaked one does not count).
		bool enemyInSight = false;

		for (auto const pTechno : TechnoClass::Array)
		{
			if (!pTechno || pTechno->InLimbo || !pTechno->IsAlive || !pTechno->Owner || pTechno->Owner == pHouse
				|| pHouse->IsAlliedWith(pTechno->Owner) || pTechno->Owner->IsNeutral() || pTechno->Owner->IsObserver()
				|| pTechno->CloakState == CloakState::Cloaked)
			{
				continue;
			}

			if (IsCellInSightOfAI(pHouse, pTechno->GetMapCoords()))
			{
				enemyInSight = true;
				break;
			}
		}

		state.EnemyInSight = enemyInSight;
		UpdateSensorLookouts(pHouse, state, enemyInSight);

		const bool launchWindow = now >= 450 && now - state.LastLaunchFrame >= ScoutLaunchCooldownFrames;
		// Sweep: with no enemy in sight one more scout every ScoutSweepEveryFrames, the grid of places
		// weighed too.
		const bool sweep = launchWindow && !enemyInSight && now - state.LastSweepFrame >= ScoutSweepEveryFrames;

		if (nextLegs.empty() && (!launchWindow || state.Trips.size() >= (sweep ? ScoutSweepMostTrips : 3)))
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
		CollectScoutCandidates(pHouse, state, home, fog, sightings, candidates, freeFields, unexploredFields, 0, sweep);

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
		const size_t usualLimit = veryStale ? 3 : ((enemies >= 2 || ground.size() >= 8) ? 2 : 1);
		const size_t limit = sweep ? std::max(usualLimit, std::min(ScoutSweepMostTrips, state.Trips.size() + 1)) : usualLimit;

		if (sweep)
			state.LastSweepFrame = now;

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

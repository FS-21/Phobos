// NeuralAI.Internal.inl: Shared utilities, decision counters, territory / map info, plan ids, learned gates, vision and type helpers.
// Part of NeuralAI.cpp (included in order; one translation unit).

namespace
{
	constexpr uint64_t FnvOffsetBasis = 14695981039346656037ULL;

	constexpr uint64_t FnvPrime = 1099511628211ULL;

	inline uint64_t HashBytesFNV1a(const void* data, size_t len, uint64_t hash = FnvOffsetBasis)
	{
		const uint8_t* pBytes = static_cast<const uint8_t*>(data);

		for (size_t i = 0; i < len; ++i)
		{
			hash ^= static_cast<uint64_t>(pBytes[i]);

			hash *= FnvPrime;
		}

		return hash;
	}

	template <typename T> inline uint64_t HashValueFNV1a(const T& val, uint64_t hash)
	{
		return HashBytesFNV1a(&val, sizeof(T), hash);
	}

	inline int8_t ClampInt8(int val)
	{
		if (val < -127)
			return -127;

		if (val > 127)
			return 127;

		return static_cast<int8_t>(val);
	}

	inline int16_t ClampInt16(int val)
	{
		if (val < -32000)
			return -32000;

		if (val > 32000)
			return 32000;

		return static_cast<int16_t>(val);
	}

	// UpdateExpertAI runs once every 15/45/105 frames (Hard/Medium/Easy), not every
	// frame. A raw "CurrentFrame % period < window" window would then land on the
	// same phase forever (e.g. % 45 at a 45-frame cadence) and keep a behavior
	// permanently on or off for that house; "% period == 0" would almost never fire.
	// These helpers widen the window to at least one AI tick so every period gets
	// exactly the intended chance.
	int CurrentAITickFrames = 1;
	int CurrentAIHouseIndex = -1;

	// Decision telemetry: per-house counters of what the AI actually did this match.
	// RAM only; published to the AI Trainer through the telemetry shared memory JSON so
	// broken logic (like the old transport behavior) shows up without any disk logging.
	enum class NeuralDecision : uint8_t
	{
		KiteStep,
		AirIntercept,
		LocalStrike,
		FerryDeparted,
		FerryUnloadAtGoal,
		FerryCargoReturned,
		SafeLaneDetour,
		MoveOrderIssued,
		MoveOrderSkipped,
		SlotGateOpen,
		SlotGateClosed,
		AirThreatTick,
		AirliftJob,
		AirliftRescue,
		AirliftDrop,
		AirCarrierLost,
		PickupDetour,
		DropAbort,
		CombatEvac,
		AirHarass,
		AttackRouteDefense,
		Counterattack,
		StuckUnitReleased,
		UnreachableOrderDropped,
		IdleHarvesterSent,
		CrawlStoneForward,
		CrawlStoneLost,
		IdleFlyerStrike,
		YardUndeployBlocked,
		FerryWaitTimeout,
		FerryIdleWithDemand,
		AttackMoveStalled,
		FieldScoutSent,
		WaveStragglerRegroup,
		FerryLandingRerouted,
		BridgeAmbush,
		BridgeSeaLane,
		BridgeMoat,
		BridgeFleetBlock,
		BridgeRoute,
		HarvesterRetreat,
		HarvesterRebalance,
		AllySiteBuilding,
		AllyCrawlAnchor,
		AllyDefense,
		BaseThreatTick,
		BaseThreatIntercept,
		BaseThreatRepelled,
		AirAttackStalled,
		PlanDeadlineExtended,
		RouteTowerPlaced,
		RouteAmbushSpot,
		RouteHarvesterDetour,
		WaveHeld,
		WaveHeldTick,
		WaveLaunched,
		WaveWindowLaunch,
		WaveFloorLaunch,
		WaveLaunchRatioSum,
		WaveLaunchUnitsSum,
		ScoutTrip,
		ScoutAirTrip,
		ScoutStart,
		ScoutRoute,
		ScoutBase,
		ScoutArmy,
		ScoutBuildingFound,
		ScoutLost,
		EnemyExpansionKnown,
		EnemyExpansionByScout,
		EnemyExpansionDelay,
		EnemyExpansionBlindFrames,
		RepairRetreat,
		RepairByDepot,
		RepairByHospital,
		RepairByRepairer,
		RepairBySelfHeal,
		RepairSaved,
		RepairLostOnTrip,
		RepairAbandoned,
		RepairHeldWinning,
		PlanAbortEnemyGrew,
		PlanAbortValueLost,
		PlanAbortTargetGone,
		CoalitionJoined,
		CoalitionDeclined,
		TruceRevoked,
		CoalitionTargetWave,
		TeamTargetSet,
		TeamWaveHold,
		TeamWaveLaunch,
		TeamSplitFronts,
		TeamWaveJoined,
		TeamHelpRequest,
		TeamHelpAnswered,
		TeamClaimYield,
		TeamExpansionYield,
		BankFactoryPick,
		BankYardPick,
		AirWing,
		AirWingSortie,
		AirInterceptSortie,
		AirSoloStrike,
		AirFrontPatrol,
		AircraftFielded,
		AircraftLost,
		UrbanGarrison,
		UrbanEvacuate,
		UrbanClear,
		AllyTechYield,
		DeployerIslandNoRoom,
		StuckUnloadMoved,
		CapitalGroupLaunched,
		CapitalGroupRetreat,
		SubmarineSonarAvoided,
		DepotEmergencySale,
		VeinholeAttack,
		CliffOpened,
		StealthRevealFired,
		StagingHoldTimedOut,
		JamCleared,
		JamYielded,
		MouthCleared,
		HoverReleased,
		GroundRouteHop,
		MedicEscort,
		GarrisonSquad,
		DryEconomySale,
		FerryReserved,
		ProductionPickBanned,
		FerryDoorRegistered,
		FerryStrayLinkCut,
		FerryTargetDropped,
		FerryDoorCellMove,
		Count
	};

	constexpr const char* NeuralDecisionNames[] = { "kiteStep", "airIntercept", "localStrike", "ferryDeparted",
		"ferryUnloadAtGoal", "ferryCargoReturned", "safeLaneDetour", "moveIssued", "moveSkipped", "slotGateOpen",
		"slotGateClosed", "airThreatTicks", "airliftJobs", "airliftRescues", "airliftDrops", "airCarrierLost",
		"pickupDetours", "dropAborts", "combatEvacs", "airHarass", "attackRouteDefense", "counterattacks",
		"stuckReleased", "unreachableDropped", "idleHarvesterSent", "crawlStoneForward", "crawlStoneLost", "idleFlyerStrike", "yardUndeployBlocked", "ferryWaitTimeout", "ferryIdleWithDemand", "attackMoveStalled", "fieldScoutSent", "waveStragglerRegroup", "ferryLandingRerouted",
		"bridgeAmbush", "bridgeSeaLane", "bridgeMoat", "bridgeFleetBlock", "bridgeRoute", "harvesterRetreat", "harvesterRebalance",
		"allySiteBuilding", "allyCrawlAnchor", "allyDefense", "baseThreatTicks", "baseThreatIntercepts", "baseThreatRepelled",
		"airAttackStalled", "planDeadlineExtended", "routeTowerPlaced", "routeAmbushSpot", "routeHarvesterDetour",
		"wavesHeld", "waveHeldTicks", "wavesLaunched", "waveWindowLaunches", "waveFloorLaunches",
		"waveLaunchRatioSum", "waveLaunchUnitsSum",
		"scoutTrips", "scoutAirTrips", "scoutStart", "scoutRoute", "scoutBase", "scoutArmy",
		"scoutBldFound", "scoutLost", "enemyExpKnown", "enemyExpByScout", "enemyExpDelayFrames", "enemyExpBlindFrames",
		"repairRetreats", "repairByDepot", "repairByHospital", "repairByRepairer", "repairBySelfHeal", "repairSaved",
		"repairLostOnTrip", "repairAbandoned", "repairHeldWinning", "planAbortEnemyGrew", "planAbortValueLost", "planAbortTargetGone",
		"coalitionJoined", "coalitionDeclined", "truceRevoked", "coalitionTargetWaves",
		"teamTargetSet", "teamWaveHold", "teamWaveLaunch", "teamSplitFronts", "teamWaveJoined", "teamHelpRequest",
		"teamHelpAnswered", "teamClaimYield", "teamExpansionYield",
		"bankFactoryPick", "bankYardPick",
		"airWings", "airWingSorties", "airInterceptSorties", "airSoloStrikes", "airFrontPatrols", "aircraftFielded",
		"aircraftLost",
		"urbanGarrison", "urbanEvacuate", "urbanClear", "allyTechYield", "deployerIslandNoRoom", "stuckUnloadMoved", "capitalGroupLaunched", "capitalGroupRetreat", "submarineSonarAvoided", "depotEmergencySale", "veinholeAttack", "cliffOpened", "stealthRevealFired",
		"stagingHoldTimedOut", "jamCleared", "jamYielded", "mouthCleared",
		"hoverReleased", "groundRouteHop", "medicEscort", "garrisonSquad", "dryEconomySale", "ferryReserved", "productionPickBanned",
		"ferryDoorRegistered", "ferryStrayLinkCut", "ferryTargetDropped", "ferryDoorCellMove" };

	static_assert(std::size(NeuralDecisionNames) == static_cast<size_t>(NeuralDecision::Count));

	std::array<std::array<uint32_t, static_cast<size_t>(NeuralDecision::Count)>, 32> DecisionCounts {};

	// Learned gate verdicts per house and slot this match ([open, closed]): the trainer finds the
	// slots pushed so far that the gate always gives the same answer (no gradient reaches them).
	std::array<std::array<std::array<uint32_t, 2>, 128>, 32> GateCounts {};

	// CPU cost of each house's AI ticks this match (milliseconds). Telemetry only: it never feeds
	// a decision (wall-clock time differs between machines), so it cannot desync.
	struct NeuralTickCost
	{
		double TotalMs { 0.0 };
		double MaxMs { 0.0 };
		uint32_t Ticks { 0 };
	};

	std::array<NeuralTickCost, 32> TickCosts {};

	// Territory telemetry (reward only: the match score and the trainer read it, no decision does,
	// so it may look through the fog). Every 900 frames, per house: the 8x8 map blocks holding one
	// of its structures (walls and insignificant structures excluded), the ore fields (12x12 blocks
	// with >= 5 ore cells) whose nearest resource hub (refinery / resource destination / ConYard)
	// within 14 cells is its own, and the total cost of its structures.
	struct NeuralTerritoryStat
	{
		int Territory { 0 };
		int OreFields { 0 };
		int BuildingValue { 0 };
		int64_t TerritorySum { 0 };
		int64_t OreFieldSum { 0 };
		int Samples { 0 };

		double TerritoryAvg() const
		{
			return this->Samples > 0 ? static_cast<double>(this->TerritorySum) / this->Samples : this->Territory;
		}

		double OreFieldsAvg() const
		{
			return this->Samples > 0 ? static_cast<double>(this->OreFieldSum) / this->Samples : this->OreFields;
		}
	};

	std::array<NeuralTerritoryStat, 32> TerritoryStats {};
	int TerritoryLastFrame { -1 };

	// Bank telemetry (reward and diagnosis only, no decision reads it), sampled every AI tick:
	// peak and summed credits, ticks with a rich bank (>= 5000), and per production queue
	// (building, vehicle, naval, infantry, aircraft) the rich ticks with no target chosen and the
	// rich ticks whose primary factory of that kind sat empty.
	enum class NeuralBankQueue : int
	{
		Building,
		Vehicle,
		Naval,
		Infantry,
		Aircraft,

		Count
	};

	struct NeuralBankStat
	{
		int PeakCredits { 0 };
		int64_t CreditsSum { 0 };
		uint32_t Ticks { 0 };
		uint32_t RichTicks { 0 };
		uint32_t GovernorTicks { 0 };
		std::array<uint32_t, static_cast<size_t>(NeuralBankQueue::Count)> NoTarget {};
		std::array<uint32_t, static_cast<size_t>(NeuralBankQueue::Count)> FactoryIdle {};
	};

	std::array<NeuralBankStat, 32> BankStats {};

	// Map class for the trainer (telemetry only): size, share of water cells, and whether the
	// starting bases are linked by land. Computed once per match, once two bases exist.
	struct NeuralMapInfo
	{
		bool Computed { false };
		int Cells { 0 };
		int WaterPct { 0 };
		bool LandLinked { true };
	};

	NeuralMapInfo MapInfo {};

	// Per map class policy corrections (trainer: shared theta + delta per class). Outside training,
	// the delta of this map's class is added once to the loaded backpack biases when the class is
	// known. File: Resources\AI\ai_mapclass.dat = "PHOBOSMC", uint32 version 1, uint32 classes,
	// int16 delta[classes][128]. Classes: 0 small land, 1 large land, 2 naval (bases not linked by land).
	constexpr int NumMapClasses = 3;
	std::array<std::array<int16_t, 128>, NumMapClasses> MapClassDeltas {};
	bool MapClassLoaded { false };
	bool MapClassApplied { false };
	std::array<int16_t, 128> MapClassOriginalBias {};

	int CurrentMapClass()
	{
		if (!MapInfo.Computed)
			return -1;

		if (!MapInfo.LandLinked)
			return 2;

		return MapInfo.Cells >= 15000 ? 1 : 0;
	}

	void ComputeMapInfo()
	{
		if (MapInfo.Computed || Unsorted::CurrentFrame < 30)
			return;

		std::vector<int> baseZones;

		for (auto const pHouse : HouseClass::Array)
		{
			if (!pHouse || pHouse->IsNeutral() || pHouse->IsObserver() || pHouse->Defeated)
				continue;

			CellStruct base = pHouse->BaseSpawnCell;

			if (pHouse->ConYards.Count > 0 && pHouse->ConYards.GetItem(0))
				base = pHouse->ConYards.GetItem(0)->GetMapCoords();

			if (base == CellStruct::Empty)
				continue;

			const int zone = MapClass::Instance.GetMovementZoneType(base, MovementZone::Normal, false);

			if (zone > 1)
				baseZones.push_back(zone);
		}

		if (baseZones.size() < 2)
			return;

		MapInfo.Computed = true;
		MapInfo.LandLinked = std::all_of(baseZones.begin(), baseZones.end(), [&](int z) { return z == baseZones.front(); });
		MapInfo.Cells = MapClass::Instance.MapRect.Width * MapClass::Instance.MapRect.Height;

		LTRBStruct bounds = MapClass::Instance.MapCoordBounds;

		if (bounds.Right <= bounds.Left || bounds.Bottom <= bounds.Top)
			bounds = LTRBStruct { 0, 0, 511, 511 };

		int total = 0;
		int water = 0;

		for (int y = std::max(0, bounds.Top); y <= bounds.Bottom; y += 2)
		{
			for (int x = std::max(0, bounds.Left); x <= bounds.Right; x += 2)
			{
				if (auto const pCell = MapClass::Instance.TryGetCellAt(CellStruct { static_cast<short>(x), static_cast<short>(y) }))
				{
					total++;

					if (pCell->LandType == LandType::Water)
						water++;
				}
			}
		}

		MapInfo.WaterPct = total > 0 ? (water * 100) / total : 0;
	}

	void SampleTerritory(bool force)
	{
		const int now = Unsorted::CurrentFrame;

		if (TerritoryLastFrame == now || (!force && TerritoryLastFrame >= 0 && now - TerritoryLastFrame < 900))
			return;

		TerritoryLastFrame = now;

		std::array<std::vector<int>, 32> blocks {};
		std::array<int, 32> value {};
		std::array<int, 32> oreFields {};
		std::vector<std::pair<CellStruct, int>> hubs;

		for (auto const pBld : BuildingClass::Array)
		{
			if (!pBld || !pBld->IsAlive || pBld->InLimbo || !pBld->Type || !pBld->Owner || pBld->Owner->IsNeutral())
				continue;

			const int idx = pBld->Owner->ArrayIndex;
			auto const pType = pBld->Type;

			if (idx < 0 || idx >= 32 || pType->Insignificant || pType->InvisibleInGame || pType->Wall)
				continue;

			const CellStruct c = pBld->GetMapCoords();
			blocks[idx].push_back((c.X / 8) * 4096 + (c.Y / 8));
			value[idx] += std::max(0, pType->Cost);

			if (pType->ConstructionYard || pType->Refinery || pType->ResourceDestination)
				hubs.emplace_back(c, idx);
		}

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

				for (int y = by; y < by + 12; ++y)
				{
					for (int x = bx; x < bx + 12; ++x)
					{
						auto const pCell = MapClass::Instance.TryGetCellAt(CellStruct { static_cast<short>(x), static_cast<short>(y) });

						if (pCell && pCell->GetContainedTiberiumValue() > 0)
						{
							++cells;
							sumX += x;
							sumY += y;
						}
					}
				}

				if (cells < 5)
					continue;

				const CellStruct mid { static_cast<short>(sumX / cells), static_cast<short>(sumY / cells) };
				int bestIdx = -1;
				int bestDist = 15;

				for (auto const& [hubCell, hubIdx] : hubs)
				{
					const int d = std::max(std::abs(hubCell.X - mid.X), std::abs(hubCell.Y - mid.Y));

					if (d < bestDist)
					{
						bestDist = d;
						bestIdx = hubIdx;
					}
				}

				if (bestIdx >= 0)
					oreFields[bestIdx]++;
			}
		}

		for (int idx = 0; idx < 32; ++idx)
		{
			auto& list = blocks[idx];
			std::sort(list.begin(), list.end());
			list.erase(std::unique(list.begin(), list.end()), list.end());

			auto& stat = TerritoryStats[idx];
			stat.Territory = static_cast<int>(list.size());
			stat.OreFields = oreFields[idx];
			stat.BuildingValue = value[idx] / 100;
			stat.TerritorySum += stat.Territory;
			stat.OreFieldSum += stat.OreFields;
			stat.Samples++;
		}
	}

	// Match score used to decide stalemates and to credit the backpack: kills, surviving units,
	// the value of the structures (by cost, so cheap posts and towers weigh little) and the
	// ground held (map blocks and ore fields, half final, half average over the match), so
	// holding the map pays more than fortifying a single base.
	int HouseMatchScore(const HouseClass* pHouse)
	{
		if (!pHouse)
			return 0;

		double territory = 0.0;
		double ore = 0.0;
		int value = pHouse->OwnedBuildings * 300;
		const int idx = pHouse->ArrayIndex;

		if (idx >= 0 && idx < 32 && TerritoryStats[idx].Samples > 0)
		{
			auto const& stat = TerritoryStats[idx];
			territory = (stat.Territory + stat.TerritoryAvg()) / 2.0;
			ore = (stat.OreFields + stat.OreFieldsAvg()) / 2.0;
			value = stat.BuildingValue * 25;
		}

		return (pHouse->TotalKilledUnits * 200) + (pHouse->TotalKilledBuildings * 450) + (pHouse->OwnedUnits * 200) + value
			+ static_cast<int>(territory * 300.0) + static_cast<int>(ore * 1200.0);
	}

	// AI tick cost per section (telemetry only, wall clock: never read by a decision, so it cannot
	// desync): which parts of the tick are worth optimizing. Placement is also called from
	// production, so it overlaps that section.
	enum class NeuralSection : uint8_t
	{
		Features,
		MoE,
		Tactics,
		Micro,
		Plans,
		Watchdogs,
		Production,
		Placement,
		// Phases inside the micro and the production (finer view of the two big sections).
		MicroSetup,
		MicroSensors,
		MicroUnits,
		MicroInfantry,
		MicroWaves,
		MicroTransports,
		MicroAircraft,
		MicroBuildings,
		ProdBuildings,
		ProdUnits,
		ProdInfantry,
		ProdAircraft,
		Count
	};

	constexpr const char* NeuralSectionNames[] = { "features", "moe", "tactics", "micro", "plans", "watchdogs",
		"production", "placement", "micro.setup", "micro.sensors", "micro.units", "micro.infantry", "micro.waves",
		"micro.transports", "micro.aircraft", "micro.buildings", "prod.buildings", "prod.units", "prod.infantry",
		"prod.aircraft" };

	static_assert(std::size(NeuralSectionNames) == static_cast<size_t>(NeuralSection::Count));

	std::array<std::array<double, static_cast<size_t>(NeuralSection::Count)>, 32> SectionTotalMs {};
	std::array<std::array<double, static_cast<size_t>(NeuralSection::Count)>, 32> SectionMaxMs {};
	// This tick's time per section (reset when the tick starts), for the peak breakdown.
	std::array<std::array<double, static_cast<size_t>(NeuralSection::Count)>, 32> TickSectionMs {};

	// The 10 slowest AI ticks of the match per house (>= 15 ms): frame, ms, live objects and the
	// three heaviest phases, so the heavy process shows itself with its circumstances.
	struct NeuralTickPeak
	{
		int Frame { 0 };
		double Ms { 0.0 };
		int Objects { 0 };
		std::string Breakdown {};
	};

	std::array<std::vector<NeuralTickPeak>, 32> TickPeaks {};

	// Expert selections per house in the current match (one count per expert choice), so the
	// trainer can measure how long each expert runs and how the house does while it runs.
	std::array<std::array<int, NeuralAI::NumSpecialists>, 32> ExpertTicks {};

	// Bumped when a match starts (OnScenarioStart): function-level caches compare it so nothing of a
	// previous match (same frames, same unique IDs, even the same seed in training) is ever reused.
	// Two clients that played different matches before must decide identically (multiplayer sync).
	int NeuralMatchSerial { 0 };

	// Fair credit per expert (trainer only, never read by AI decisions): every 60 frames the change
	// of each house's advantage (value of its units, buildings and half its money, minus the
	// average of its living enemies', in tens of credits) goes to the expert running at that
	// moment, so an expert is judged by what happened while it ran, not by who was in charge
	// when the match ended.
	std::array<std::array<int, NeuralAI::NumSpecialists>, 32> ExpertGain {};
	std::array<int, 32> LastHouseAdvantage {};
	std::array<bool, 32> HasLastHouseAdvantage {};

	// Context routing: a learned bias per (game phase, map class, expert) added to the expert
	// selection on top of the global routing bias, so an expert that pays off on naval maps or
	// late in the match is preferred there and not everywhere. Phases by game frame (15 logic
	// frames per game second): early until 5400 (first 6 minutes: base, factories, first
	// expansion), mid until 18000 (20 minutes: armies, expansions, first sieges), late after.
	// Map classes as CurrentMapClass (0 small land, 1 large land, 2 naval).
	// File: "PHOBOSCR", uint32 version 1, uint32 phases, uint32 classes, uint32 experts,
	// int16 bias[phases][classes][experts]. Without it the term is 0 and the choice is unchanged.
	constexpr int NumRoutingPhases = 3;
	constexpr int RoutingMidPhaseFrame = 5400;
	constexpr int RoutingLatePhaseFrame = 18000;
	constexpr int ContextRoutingCap = 150;

	struct NeuralContextRouting
	{
		bool Loaded { false };
		std::array<std::array<std::array<int16_t, NeuralAI::NumSpecialists>, NumMapClasses>, NumRoutingPhases> Bias {};
	};

	NeuralContextRouting ContextRouting {};
	NeuralContextRouting OpponentContextRouting {};

	int RoutingPhase(int frame)
	{
		if (frame < RoutingMidPhaseFrame)
			return 0;

		return frame < RoutingLatePhaseFrame ? 1 : 2;
	}

	struct NeuralContextRoutingHeader
	{
		char Magic[8];
		uint32_t Version;
		uint32_t Phases;
		uint32_t Classes;
		uint32_t Experts;
	};

	constexpr size_t ContextRoutingFileSize = sizeof(NeuralContextRoutingHeader) + sizeof(NeuralContextRouting::Bias);

	bool ParseContextRouting(const uint8_t* pData, size_t size, NeuralContextRouting& out)
	{
		NeuralContextRoutingHeader header {};

		if (!pData || size < ContextRoutingFileSize)
			return false;

		std::memcpy(&header, pData, sizeof(header));

		if (std::memcmp(header.Magic, "PHOBOSCR", 8) != 0 || header.Version != 1 || header.Phases != NumRoutingPhases
			|| header.Classes != NumMapClasses || header.Experts != NeuralAI::NumSpecialists)
		{
			return false;
		}

		std::memcpy(out.Bias.data(), pData + sizeof(header), sizeof(out.Bias));
		out.Loaded = true;

		return true;
	}

	bool LoadContextRoutingFile(const char* pPath, NeuralContextRouting& out)
	{
		HANDLE hFile = CreateFileA(pPath, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);

		if (hFile == INVALID_HANDLE_VALUE)
			return false;

		std::array<uint8_t, 256> buffer {};
		DWORD read = 0;
		const bool ok = ReadFile(hFile, buffer.data(), static_cast<DWORD>(buffer.size()), &read, NULL) != FALSE;

		CloseHandle(hFile);

		return ok && ParseContextRouting(buffer.data(), read, out);
	}

	// The trainer hands each match's tables over through named page-file mappings (RAM only).
	bool LoadContextRoutingFromSharedMemory(const char* pName, NeuralContextRouting& out)
	{
		HANDLE hMapping = OpenFileMappingA(FILE_MAP_READ, FALSE, pName);

		if (!hMapping)
			return false;

		bool ok = false;

		if (auto const pView = static_cast<const uint8_t*>(MapViewOfFile(hMapping, FILE_MAP_READ, 0, 0, ContextRoutingFileSize)))
		{
			ok = ParseContextRouting(pView, ContextRoutingFileSize, out);
			UnmapViewOfFile(pView);
		}

		CloseHandle(hMapping);

		return ok;
	}

	// The table of the policy this house plays (the trainer gives challenger and opponent their own).
	const NeuralContextRouting& ContextRoutingFor(const HouseClass* pHouse)
	{
		if (NeuralAI::HeadlessMode && OpponentContextRouting.Loaded)
		{
			auto const pExt = HouseExt::TryFetch(pHouse);

			if (pExt && pExt->NeuralAI_PolicySlot == 1)
				return OpponentContextRouting;
		}

		return ContextRouting;
	}

	// Expert choices and fair gain per game phase (trainer only, never read by AI decisions), so
	// the trainer can credit the context routing per phase; the match gives the map class.
	std::array<std::array<std::array<int, NeuralAI::NumSpecialists>, NumRoutingPhases>, 32> ExpertTicksByPhase {};
	std::array<std::array<std::array<int, NeuralAI::NumSpecialists>, NumRoutingPhases>, 32> ExpertGainByPhase {};

	void SampleExpertGains()
	{
		std::array<int64_t, 32> assets {};

		for (auto const pTechno : TechnoClass::Array)
		{
			if (!pTechno || !pTechno->IsAlive || !pTechno->Owner || pTechno->Owner->ArrayIndex < 0 || pTechno->Owner->ArrayIndex >= 32)
				continue;

			if (auto const pType = pTechno->GetTechnoType())
				assets[pTechno->Owner->ArrayIndex] += std::max(0, static_cast<int>(pType->Cost));
		}

		for (auto const pHouse : HouseClass::Array)
		{
			if (pHouse && pHouse->ArrayIndex >= 0 && pHouse->ArrayIndex < 32)
				assets[pHouse->ArrayIndex] += std::max(0, static_cast<int>(pHouse->Available_Money())) / 2;
		}

		for (auto const pHouse : HouseClass::Array)
		{
			if (!pHouse || pHouse->ArrayIndex < 0 || pHouse->ArrayIndex >= 32 || pHouse->IsNeutral() || pHouse->Defeated)
				continue;

			const int idx = pHouse->ArrayIndex;
			int64_t enemySum = 0;
			int enemies = 0;

			for (auto const pOther : HouseClass::Array)
			{
				if (pOther && pOther != pHouse && !pOther->IsNeutral() && !pOther->Defeated && !pHouse->IsAlliedWith(pOther)
					&& pOther->ArrayIndex >= 0 && pOther->ArrayIndex < 32)
				{
					enemySum += assets[pOther->ArrayIndex];
					enemies++;
				}
			}

			const int advantage = static_cast<int>(std::clamp<int64_t>(
				(assets[idx] - (enemies > 0 ? enemySum / enemies : 0)) / 10, -2000000, 2000000));
			int ticksSum = 0;

			for (const int t : ExpertTicks[idx])
				ticksSum += t;

			if (HasLastHouseAdvantage[idx] && ticksSum > 0)
			{
				auto const pExt = HouseExt::Fetch(pHouse);
				const int expert = pExt ? std::clamp(pExt->NeuralAI_ActiveExpert, 0, NeuralAI::NumSpecialists - 1) : 0;
				ExpertGain[idx][expert] = static_cast<int>(std::clamp<int64_t>(
					static_cast<int64_t>(ExpertGain[idx][expert]) + advantage - LastHouseAdvantage[idx], INT_MIN / 2, INT_MAX / 2));

				int& phaseGain = ExpertGainByPhase[idx][RoutingPhase(Unsorted::CurrentFrame)][expert];
				phaseGain = static_cast<int>(std::clamp<int64_t>(
					static_cast<int64_t>(phaseGain) + advantage - LastHouseAdvantage[idx], INT_MIN / 2, INT_MAX / 2));
			}

			LastHouseAdvantage[idx] = advantage;
			HasLastHouseAdvantage[idx] = true;
		}
	}

	void AddSectionTime(int houseIndex, NeuralSection section, double ms)
	{
		if (houseIndex < 0 || houseIndex >= 32)
			return;

		const size_t s = static_cast<size_t>(section);
		SectionTotalMs[houseIndex][s] += ms;
		SectionMaxMs[houseIndex][s] = std::max(SectionMaxMs[houseIndex][s], ms);
		TickSectionMs[houseIndex][s] += ms;
	}

	double ElapsedMs(const LARGE_INTEGER& start)
	{
		LARGE_INTEGER end {};
		LARGE_INTEGER frequency {};
		QueryPerformanceCounter(&end);
		QueryPerformanceFrequency(&frequency);

		return frequency.QuadPart > 0 ? static_cast<double>(end.QuadPart - start.QuadPart) * 1000.0 / static_cast<double>(frequency.QuadPart) : 0.0;
	}

	// Times consecutive phases of one long function: Mark(next) closes the current phase.
	struct NeuralPhaseClock
	{
		int HouseIndex;
		NeuralSection Current;
		LARGE_INTEGER Start {};

		NeuralPhaseClock(int houseIndex, NeuralSection first) : HouseIndex { houseIndex }, Current { first }
		{
			QueryPerformanceCounter(&this->Start);
		}

		void Mark(NeuralSection next)
		{
			AddSectionTime(this->HouseIndex, this->Current, ElapsedMs(this->Start));
			this->Current = next;
			QueryPerformanceCounter(&this->Start);
		}

		~NeuralPhaseClock()
		{
			AddSectionTime(this->HouseIndex, this->Current, ElapsedMs(this->Start));
		}
	};

	struct NeuralSectionTimer
	{
		int HouseIndex;
		NeuralSection Section;
		LARGE_INTEGER Start {};

		NeuralSectionTimer(int houseIndex, NeuralSection section) : HouseIndex { houseIndex }, Section { section }
		{
			QueryPerformanceCounter(&this->Start);
		}

		~NeuralSectionTimer()
		{
			if (this->HouseIndex < 0 || this->HouseIndex >= 32)
				return;

			LARGE_INTEGER end {};
			LARGE_INTEGER frequency {};
			QueryPerformanceCounter(&end);
			QueryPerformanceFrequency(&frequency);

			if (frequency.QuadPart <= 0)
				return;

			const double ms = static_cast<double>(end.QuadPart - this->Start.QuadPart) * 1000.0 / static_cast<double>(frequency.QuadPart);
			AddSectionTime(this->HouseIndex, this->Section, ms);
		}
	};

	// Spatial index of live, placed technos (map state, rebuilt once per AI house tick on first
	// use): 8x8-cell buckets holding pointers in TechnoClass::Array order, scanned row by row, so
	// the result order is the same on every client (MP safe). "Who is within N cells of here"
	// then reads only the buckets in range instead of every object on the map.
	struct NeuralSpatialIndex
	{
		static constexpr int BucketShift = 3; // 8 cells
		int Frame { -1 };
		int HouseIndex { -2 };
		int Columns { 0 };
		int Rows { 0 };
		std::vector<std::vector<TechnoClass*>> Buckets {};
	};

	NeuralSpatialIndex TechnoSpatialIndex {};

	void RebuildTechnoSpatialIndexIfNeeded()
	{
		auto& index = TechnoSpatialIndex;

		// Rebuilt per house tick (not per frame): an object removed between two houses' ticks of
		// the same frame must never be handed out.
		if (index.Frame == Unsorted::CurrentFrame && index.HouseIndex == CurrentAIHouseIndex && !index.Buckets.empty())
			return;

		index.Frame = Unsorted::CurrentFrame;
		index.HouseIndex = CurrentAIHouseIndex;
		index.Columns = (512 >> NeuralSpatialIndex::BucketShift) + 1;
		index.Rows = index.Columns;

		if (index.Buckets.size() != static_cast<size_t>(index.Columns * index.Rows))
			index.Buckets.assign(static_cast<size_t>(index.Columns * index.Rows), {});
		else
			for (auto& bucket : index.Buckets)
				bucket.clear();

		for (auto const pTechno : TechnoClass::Array)
		{
			if (!pTechno || !pTechno->IsAlive || pTechno->InLimbo)
				continue;

			const CellStruct c = pTechno->GetMapCoords();
			const int bx = std::clamp(static_cast<int>(c.X) >> NeuralSpatialIndex::BucketShift, 0, index.Columns - 1);
			const int by = std::clamp(static_cast<int>(c.Y) >> NeuralSpatialIndex::BucketShift, 0, index.Rows - 1);
			index.Buckets[static_cast<size_t>(by * index.Columns + bx)].push_back(pTechno);
		}
	}

	// Calls fn(TechnoClass*) for every live placed techno whose cell is within `radiusCells`
	// (Chebyshev) of `center`; fn returns false to stop early.
	template <typename Fn>
	void ForEachTechnoNear(const CellStruct& center, int radiusCells, Fn&& fn)
	{
		RebuildTechnoSpatialIndexIfNeeded();
		auto const& index = TechnoSpatialIndex;
		const int shift = NeuralSpatialIndex::BucketShift;
		const int bx0 = std::clamp((static_cast<int>(center.X) - radiusCells) >> shift, 0, index.Columns - 1);
		const int bx1 = std::clamp((static_cast<int>(center.X) + radiusCells) >> shift, 0, index.Columns - 1);
		const int by0 = std::clamp((static_cast<int>(center.Y) - radiusCells) >> shift, 0, index.Rows - 1);
		const int by1 = std::clamp((static_cast<int>(center.Y) + radiusCells) >> shift, 0, index.Rows - 1);

		for (int by = by0; by <= by1; ++by)
		{
			for (int bx = bx0; bx <= bx1; ++bx)
			{
				for (auto const pTechno : index.Buckets[static_cast<size_t>(by * index.Columns + bx)])
				{
					const CellStruct c = pTechno->GetMapCoords();

					if (std::max(std::abs(c.X - center.X), std::abs(c.Y - center.Y)) > radiusCells)
						continue;

					if (!fn(pTechno))
						return;
				}
			}
		}
	}

	struct NeuralTickTimer
	{
		int HouseIndex;
		LARGE_INTEGER Start {};

		explicit NeuralTickTimer(int houseIndex) : HouseIndex { houseIndex }
		{
			if (houseIndex >= 0 && houseIndex < 32)
				TickSectionMs[houseIndex].fill(0.0);

			QueryPerformanceCounter(&this->Start);
		}

		~NeuralTickTimer()
		{
			if (this->HouseIndex < 0 || this->HouseIndex >= 32)
				return;

			LARGE_INTEGER end {};
			LARGE_INTEGER frequency {};
			QueryPerformanceCounter(&end);
			QueryPerformanceFrequency(&frequency);

			if (frequency.QuadPart <= 0)
				return;

			const double ms = static_cast<double>(end.QuadPart - this->Start.QuadPart) * 1000.0 / static_cast<double>(frequency.QuadPart);
			auto& cost = TickCosts[this->HouseIndex];
			cost.TotalMs += ms;
			cost.MaxMs = std::max(cost.MaxMs, ms);
			cost.Ticks++;

			if (ms >= 15.0)
			{
				auto& peaks = TickPeaks[this->HouseIndex];

				if (peaks.size() < 10 || ms > peaks.back().Ms)
				{
					// Three heaviest leaf phases (the aggregate micro / production entries excluded).
					std::vector<std::pair<double, size_t>> parts;
					auto const& tick = TickSectionMs[this->HouseIndex];

					for (size_t sec = 0; sec < tick.size(); ++sec)
					{
						if (sec != static_cast<size_t>(NeuralSection::Micro) && sec != static_cast<size_t>(NeuralSection::Production)
							&& tick[sec] >= 0.5)
						{
							parts.emplace_back(tick[sec], sec);
						}
					}

					std::sort(parts.begin(), parts.end(), [](auto const& a, auto const& b) { return a.first > b.first; });
					std::string text;

					for (size_t k = 0; k < parts.size() && k < 3; ++k)
					{
						char part[64];
						std::snprintf(part, sizeof(part), "%s%s %.1f", k ? ", " : "", NeuralSectionNames[parts[k].second], parts[k].first);
						text += part;
					}

					peaks.push_back({ Unsorted::CurrentFrame, ms, TechnoClass::Array.Count, text });
					std::sort(peaks.begin(), peaks.end(), [](auto const& a, auto const& b) { return a.Ms > b.Ms; });

					if (peaks.size() > 10)
						peaks.resize(10);
				}
			}
		}
	};

	// Strategic plan library telemetry: per house and plan, how many were started,
	// succeeded and failed this match (RAM only, published in the telemetry JSON).
	enum class NeuralPlanId : uint8_t
	{
		EcoRaid,
		SiegeBattery,
		AirStrike,
		Blackout,
		Feint,
		TimingRush,
		HeavyAirPush,
		IslandHold,
		AirborneFront,
		ExpansionDenial,
		BaseTrade,
		HoldUnderTowers,
		SuperweaponRace,
		Pincer,
		HarvesterAmbush,
		Pickets,
		KeyPointHold,
		Survival,
		PreemptiveStrike,
		BaitAmbush,
		AllyJointAttack,
		ForwardRepair,
		NavalStrikeGroup,
		EngineerAssault,
		Sabotage,
		HuntKeyUnits,
		RotatingRaids,
		AirCorridor,
		SilentInfiltration,
		ArtilleryEcoSiege,
		RollingSiege,
		DelayingAction,
		ThreeFrontAssault,
		FactoryDecapitation,
		CutReinforcements,
		TechTreeCut,
		ComboBlackoutPush,
		ComboWatchStrike,
		AllyBackup,
		AllyForwardOutpost,
		BaseThreatIntercept,
		Count
	};

	constexpr const char* NeuralPlanNames[] = { "ecoRaid", "siegeBattery", "airStrike", "blackout", "feint", "timingRush",
		"heavyAirPush", "islandHold", "airborneFront", "expansionDenial", "baseTrade", "holdUnderTowers", "swRace", "pincer",
		"harvesterAmbush", "pickets", "keyPointHold", "survival", "preemptiveStrike", "baitAmbush", "allyJointAttack",
		"forwardRepair", "navalStrike", "engineerAssault", "sabotage", "huntKeyUnits", "rotatingRaids", "airCorridor",
		"silentInfiltration", "artilleryEcoSiege", "rollingSiege", "delayingAction", "threeFront", "factoryDecapitation",
		"cutReinforcements", "techTreeCut", "comboBlackoutPush", "comboWatchStrike", "allyBackup", "allyForwardOutpost", "baseThreatIntercept" };

	// Trained preference of each plan: free reserve output slots 89.. (see the trainer's
	// OUTPUT_SLOT_NAMES), learned by ES like any other slot. The ally site plans share the slot of
	// the plan closest to them: Ally Backup with Survival (106), Ally Forward Outpost with Ally Joint
	// Attack (109). Base Threat Intercept shares Preemptive Strike's slot (107): both hit an enemy
	// force with a stronger idle group before it reaches us.
	constexpr int NeuralPlanSlots[] = { 89, 90, 91, 92, 93, 94, 95, 96, 97, 98, 99, 100, 101, 102, 103, 104, 105, 106, 107,
		108, 109, 110, 111, 112, 113, 114, 115, 116, 117, 118, 119, 120, 121, 122, 123, 124, 125, 126, 106, 109, 107 };

	static_assert(std::size(NeuralPlanNames) == static_cast<size_t>(NeuralPlanId::Count));
	static_assert(std::size(NeuralPlanSlots) == static_cast<size_t>(NeuralPlanId::Count));

	enum class NeuralPlanOutcome : uint8_t
	{
		Started,
		Succeeded,
		Failed,
		// The plan found a target and a group (context utility > 0) and asked the learned gate:
		// eligible but never started = held back by its learned preference; never eligible =
		// its preconditions never happen on these maps.
		Eligible,
		// The learned gate opened but the plan did not start: no group of free units fit it, so the
		// plan is held back by unit availability, not by its learned preference.
		NoGroup,
		// Part of Failed: the plan ran out of time (its deadline), not out of units or of target.
		// Most such groups are still far from their target: the route took longer than the plan.
		TimedOut,
		// Ended because we entered a tacit truce with the house it was against (Expert 3): neither a
		// success nor a failure, no credit either way.
		Pact,
		Count
	};

	std::array<std::array<std::array<uint32_t, static_cast<size_t>(NeuralPlanOutcome::Count)>,
		static_cast<size_t>(NeuralPlanId::Count)>, 32> PlanStats {};

	void CountPlanOutcome(int houseIndex, NeuralPlanId plan, NeuralPlanOutcome outcome)
	{
		if (houseIndex >= 0 && houseIndex < 32)
			PlanStats[houseIndex][static_cast<size_t>(plan)][static_cast<size_t>(outcome)]++;
	}

	void CountDecision(NeuralDecision decision, int houseIndex = -1)
	{
		const int idx = (houseIndex >= 0) ? houseIndex : CurrentAIHouseIndex;

		if (idx >= 0 && idx < 32)
			DecisionCounts[idx][static_cast<size_t>(decision)]++;
	}

	// Decision credit per expert (trainer only, never read by AI decisions). expertGain gives each
	// 60-frame change of a house's advantage to the expert in charge at that moment, but most
	// results come from earlier decisions of other experts: a refinery started under Expert 0 pays
	// for minutes, a wave sent under Expert 5 lands while Expert 7 runs. Here every own techno is
	// tagged with the expert in charge when it first exists (production start, placement, capture;
	// a deployer passes its tag to what it deploys into) and with the expert whose plan or wave
	// last sent it. A techno that appears outside production takes the tags of what made it: the
	// launcher of a spawn, the mind controller, the slave master, the building that gives it as a
	// free unit, or an own techno gone nearby (crew leaving a wreck, an engineer that captured).
	// The same advantage change is split into the parts that have an author: Built (cost of a new
	// techno, to the expert that started it), Lost (cost of a techno gone, to the expert that sent
	// it; one nobody sent goes to the expert that started it, but when an enemy killed it half goes
	// to the recently active experts, who let the attack through), Spent (money paid into
	// productions, to the experts that started them, by cost), Income (ore unloaded goes half to
	// the expert that started the gatherer and half to the one that started the refinery; other
	// income goes to the experts that started the economic technos, by cost), Kills (enemy
	// assets destroyed, to the expert that sent or started the killer) and Shared (the rest:
	// enemy growth, third-party fights, defeats), which goes to the recently active experts by an
	// eligibility trace (expert choices, decaying by 7/8 per sample, about 20 game seconds).
	// The parts add up to the advantage change, so the totals match expertGain; only the
	// author changes. Each part is booked in the game phase of the decision that earned it.
	enum class NeuralCreditPart : uint8_t
	{
		Built,
		Lost,
		Spent,
		Income,
		Kills,
		Shared,
		Count
	};

	struct NeuralCreditTag
	{
		TechnoTypeClass* Type { nullptr };
		// Where it was at the last sample, to find what left crew or captured nearby.
		CoordStruct Location {};
		int Cost { 0 };
		int Stamp { 0 };
		int CommandFrame { 0 };
		int8_t Origin { 0 };
		int8_t OriginPhase { 0 };
		int8_t Command { -1 };
		int8_t CommandPhase { 0 };
		// Plan id of the last command, CreditSourceWave for an offensive wave, -1 none.
		int16_t CommandSource { -1 };
		bool Credited { false };
		bool Economic { false };
		bool KilledByEnemy { false };
	};

	struct NeuralCreditGone
	{
		DWORD ID;
		NeuralCreditTag Tag;
		bool Transformed;
	};

	constexpr int16_t CreditSourceWave = 1000;
	// How far from its source a techno that appears outside production may be (6 cells).
	constexpr int CreditParentRange = 6 * 256;

	std::array<std::unordered_map<DWORD, NeuralCreditTag>, 32> CreditTags {};
	// In credits (telemetry divides by 10 like expertGain).
	std::array<std::array<int64_t, NeuralAI::NumSpecialists>, 32> ExpertCredit {};
	std::array<std::array<std::array<int64_t, NeuralAI::NumSpecialists>, NumRoutingPhases>, 32> ExpertCreditByPhase {};
	std::array<std::array<std::array<int64_t, static_cast<size_t>(NeuralCreditPart::Count)>, NeuralAI::NumSpecialists>, 32> ExpertCreditParts {};
	// Enemy asset value killed since the last sample, by the killer's author [phase][expert].
	std::array<std::array<std::array<int64_t, NeuralAI::NumSpecialists>, NumRoutingPhases>, 32> PendingKillCredit {};
	// Money from ore unloaded since the last sample, by the authors of the gatherer and the dock.
	std::array<std::array<std::array<int64_t, NeuralAI::NumSpecialists>, NumRoutingPhases>, 32> PendingHarvestCredit {};
	// Own technos gone at the last sample, sorted by UniqueID: crew may appear one sample later.
	std::array<std::vector<NeuralCreditGone>, 32> CreditRecentGone {};
	std::array<std::array<int, NeuralAI::NumSpecialists>, 32> CreditTrace {};
	std::array<std::array<int, NeuralAI::NumSpecialists>, 32> CreditTraceTicks {};
	std::array<int64_t, 32> CreditLastAdvantage {};
	std::array<int, 32> CreditLastHalfMoney {};
	std::array<int, 32> CreditLastSpent {};
	std::array<bool, 32> CreditHasLast {};
	std::array<std::array<int8_t, static_cast<size_t>(NeuralPlanId::Count)>, 32> PlanStarterExpert {};
	std::array<std::array<int8_t, static_cast<size_t>(NeuralPlanId::Count)>, 32> PlanStarterPhase {};
	int CreditSerial { 0 };
	int CreditLastFrame { -1 };

	void ResetExpertCredit()
	{
		for (auto& tags : CreditTags)
			tags.clear();

		ExpertCredit = {};
		ExpertCreditByPhase = {};
		ExpertCreditParts = {};
		PendingKillCredit = {};
		PendingHarvestCredit = {};

		for (auto& recent : CreditRecentGone)
			recent.clear();

		CreditTrace = {};
		CreditTraceTicks = {};
		CreditLastAdvantage.fill(0);
		CreditLastHalfMoney.fill(0);
		CreditLastSpent.fill(0);
		CreditHasLast.fill(false);

		for (auto& starters : PlanStarterExpert)
			starters.fill(-1);

		PlanStarterPhase = {};
		CreditSerial = 0;
		CreditLastFrame = -1;
	}

	int CreditActiveExpert(const HouseClass* pHouse)
	{
		auto const pExt = HouseExt::TryFetch(pHouse);
		return pExt ? std::clamp(pExt->NeuralAI_ActiveExpert, 0, NeuralAI::NumSpecialists - 1) : 0;
	}

	// What earns the house money: refineries, gatherers, cash and purifier buildings.
	bool IsCreditEconomicType(TechnoTypeClass* pType)
	{
		if (pType->ResourceGatherer || pType->ResourceDestination)
			return true;

		if (auto const pBldType = abstract_cast<BuildingTypeClass*>(pType))
			return pBldType->Refinery || pBldType->ProduceCashAmount > 0 || pBldType->OrePurifier;

		if (auto const pUnitType = abstract_cast<UnitTypeClass*>(pType))
			return pUnitType->Harvester;

		return false;
	}

	// The tag of an own techno, created with the expert in charge now when it has none.
	NeuralCreditTag* CreditTagFor(int houseIdx, TechnoClass* pTechno, int expert, int phase)
	{
		auto const pType = pTechno->GetTechnoType();

		if (!pType)
			return nullptr;

		auto& tag = CreditTags[houseIdx][pTechno->UniqueID];

		if (!tag.Type)
		{
			tag.Type = pType;
			tag.Cost = std::max(0, static_cast<int>(pType->Cost));
			tag.Origin = static_cast<int8_t>(expert);
			tag.OriginPhase = static_cast<int8_t>(phase);
			tag.Economic = IsCreditEconomicType(pType);
		}

		return &tag;
	}

	// A plan or a wave sends this techno: its losses and kills from now on belong to the expert in
	// charge now (headless only, where the credit is measured).
	void NoteExpertCommand(HouseClass* pHouse, TechnoClass* pTechno, int16_t source)
	{
		if (!NeuralAI::HeadlessMode || !pHouse || !pTechno || pHouse->ArrayIndex < 0 || pHouse->ArrayIndex >= 32)
			return;

		const int expert = CreditActiveExpert(pHouse);
		const int phase = RoutingPhase(Unsorted::CurrentFrame);

		if (auto const pTag = CreditTagFor(pHouse->ArrayIndex, pTechno, expert, phase))
		{
			pTag->Command = static_cast<int8_t>(expert);
			pTag->CommandPhase = static_cast<int8_t>(phase);
			pTag->CommandSource = source;
			pTag->CommandFrame = Unsorted::CurrentFrame;
		}
	}

	// The expert that started a plan also commands the units the plan recruits later.
	void NoteExpertPlanStart(HouseClass* pHouse, NeuralPlanId plan)
	{
		if (!NeuralAI::HeadlessMode || !pHouse || pHouse->ArrayIndex < 0 || pHouse->ArrayIndex >= 32)
			return;

		PlanStarterExpert[pHouse->ArrayIndex][static_cast<size_t>(plan)] = static_cast<int8_t>(CreditActiveExpert(pHouse));
		PlanStarterPhase[pHouse->ArrayIndex][static_cast<size_t>(plan)] = static_cast<int8_t>(RoutingPhase(Unsorted::CurrentFrame));
	}

	// exp(x) for |x| <= 8 using only IEEE + - * /, so every client computes bit-identical
	// results regardless of which CRT math implementation its CPU selects.
	double DeterministicExp(double x)
	{
		// exp(x) = exp(x / 16)^16 keeps the Taylor series argument small.
		const double y = x / 16.0;
		double term = 1.0;
		double sum = 1.0;

		for (int n = 1; n <= 20; ++n)
		{
			term = term * y / n;
			sum += term;
		}

		for (int i = 0; i < 4; ++i)
			sum *= sum;

		return sum;
	}

	// Graded slot gate replacing hard "scores[N] > T" switches for tactical behaviors.
	// The behavior fires with probability logistic((score - T) / 120): far above the
	// threshold it is (almost) always on, far below (almost) always off, and in between
	// the trained slot keeps a smooth effect, so Evolution Strategies always get a
	// gradient instead of a flat dead zone. The draw is a hash of (AI tick, house,
	// slot), identical on every client: no desync. During headless training only, a
	// 3% exploration floor lets a slot that drifted far below recover.
	// Per-match salt of each house (drawn from the synced scenario RNG in OnScenarioStart): the
	// SlotGate draws differ between matches instead of repeating frame by frame.
	std::array<uint32_t, 32> HouseStyleSalt {};

	bool SlotGate(int slotScore, int threshold, int slot)
	{
		constexpr double Softness = 120.0;
		const double margin = static_cast<double>(slotScore - threshold);

		double probability = 1.0 / (1.0 + DeterministicExp(-std::clamp(margin / Softness, -8.0, 8.0)));

		if (NeuralAI::HeadlessMode)
			probability = std::clamp(probability, 0.03, 0.97);

		uint32_t h = 2166136261u;

		const uint32_t salt = (CurrentAIHouseIndex >= 0 && CurrentAIHouseIndex < 32) ? HouseStyleSalt[CurrentAIHouseIndex] : 0u;

		for (const uint32_t v : { static_cast<uint32_t>(Unsorted::CurrentFrame / std::max(1, CurrentAITickFrames)),
				 static_cast<uint32_t>(CurrentAIHouseIndex + 1), static_cast<uint32_t>(slot), salt })
		{
			h = (h ^ v) * 16777619u;
			h ^= h >> 13;
		}

		const bool open = static_cast<double>(h % 10000u) < probability * 10000.0;
		CountDecision(open ? NeuralDecision::SlotGateOpen : NeuralDecision::SlotGateClosed);

		if (CurrentAIHouseIndex >= 0 && CurrentAIHouseIndex < 32 && slot >= 0 && slot < 128)
			GateCounts[CurrentAIHouseIndex][slot][open ? 0 : 1]++;

		return open;
	}

	bool IsAITickWindow(int period, int window)
	{
		if (period <= 0)
			return true;

		return (Unsorted::CurrentFrame % period) < std::max(window, CurrentAITickFrames);
	}

	bool IsAITickPeriod(int period)
	{
		return IsAITickWindow(period, 1);
	}

	// Scopes the AI tick cadence (and the house being evaluated) to one UpdateExpertAI
	// call; hooks running outside of it (building placement, damage tracking) keep
	// frame-exact windows.
	struct AITickCadenceScope
	{
		explicit AITickCadenceScope(int tickFrames, int houseIndex)
		{
			CurrentAITickFrames = std::max(1, tickFrames);
			CurrentAIHouseIndex = houseIndex;
		}

		~AITickCadenceScope()
		{
			CurrentAITickFrames = 1;
			CurrentAIHouseIndex = -1;
		}

		AITickCadenceScope(const AITickCadenceScope&) = delete;
		AITickCadenceScope& operator=(const AITickCadenceScope&) = delete;
	};

	// Per-house vision map built from the sight of the AI and its allies. Replaces
	// CellClass::IsShrouded, which reflects the local player's shroud and would
	// desync multiplayer and leak the human's exploration to the AI.
	constexpr int VisionMapSide = 512;

	std::vector<uint8_t> VisionMap;
	// Cells in sight of the AI and its allies right now (the explored map only ever grows).
	std::vector<uint8_t> CurrentSightMap;
	std::array<std::vector<uint8_t>, 8> HouseExploredMaps;
	int LastExploredResetFrame = -1;

	int VisionMapFrame = -1;
	int VisionMapOwner = -1;

	void RebuildVisionMap(HouseClass* pHouse)
	{
		const size_t totalCells = static_cast<size_t>(VisionMapSide) * VisionMapSide;
		if (LastExploredResetFrame < 0 || Unsorted::CurrentFrame < LastExploredResetFrame || Unsorted::CurrentFrame <= 15)
		{
			for (auto& m : HouseExploredMaps)
				m.assign(totalCells, 0);

			LastExploredResetFrame = Unsorted::CurrentFrame;
		}
		else
		{
			LastExploredResetFrame = Unsorted::CurrentFrame;
		}

		const int hIdx = std::clamp(pHouse->ArrayIndex, 0, 7);
		auto& explored = HouseExploredMaps[hIdx];
		if (explored.size() != totalCells)
			explored.assign(totalCells, 0);

		VisionMapFrame = Unsorted::CurrentFrame;

		VisionMapOwner = pHouse->ArrayIndex;

		CurrentSightMap.assign(totalCells, 0);

		for (auto const pTechno : TechnoClass::Array)
		{
			if (!pTechno || pTechno->InLimbo || !pTechno->IsAlive || !pTechno->Owner)
				continue;

			if (pTechno->Owner != pHouse && !pHouse->IsAlliedWith(pTechno->Owner))
				continue;

			auto const pType = pTechno->GetTechnoType();

			if (!pType)
				continue;

			const CellStruct coords = pTechno->GetMapCoords();
			const int radius = std::clamp(static_cast<int>(pType->Sight), 1, 24);

			for (int dy = -radius; dy <= radius; ++dy)
			{
				const int y = coords.Y + dy;

				if (y < 0 || y >= VisionMapSide)
					continue;

				for (int dx = -radius; dx <= radius; ++dx)
				{
					const int x = coords.X + dx;

					if (x < 0 || x >= VisionMapSide || dx * dx + dy * dy > radius * radius)
						continue;

					const size_t idx = static_cast<size_t>(y) * VisionMapSide + x;
					explored[idx] = 1;
					CurrentSightMap[idx] = 1;
				}
			}
		}

		VisionMap = explored;
	}

	bool IsCellInSightOfAI(HouseClass* pHouse, const CellStruct& coords)
	{
		if (!pHouse)
			return false;

		if (VisionMapFrame != Unsorted::CurrentFrame || VisionMapOwner != pHouse->ArrayIndex)
			RebuildVisionMap(pHouse);

		if (coords.X < 0 || coords.X >= VisionMapSide || coords.Y < 0 || coords.Y >= VisionMapSide)
			return false;

		return CurrentSightMap[static_cast<size_t>(coords.Y) * VisionMapSide + coords.X] != 0;
	}

	bool IsCellVisibleToAI(HouseClass* pHouse, CellClass* pCell)
	{
		if (!pHouse || !pCell)
			return false;

		if (VisionMapFrame != Unsorted::CurrentFrame || VisionMapOwner != pHouse->ArrayIndex)
			RebuildVisionMap(pHouse);

		const CellStruct coords = pCell->MapCoords;

		if (coords.X < 0 || coords.X >= VisionMapSide || coords.Y < 0 || coords.Y >= VisionMapSide)
			return false;

		return VisionMap[static_cast<size_t>(coords.Y) * VisionMapSide + coords.X] != 0;
	}

	// A spy: it enters enemy buildings for the spy effects (Agent=). The engine also sets Infiltrate
	// on engineers and C4 commandos, which fight or capture instead.
	bool IsSpyInfantryType(const InfantryTypeClass* pType)
	{
		return pType && pType->Infiltrate && pType->Agent;
	}

	// A ground transport that burrows (subterranean movement zone).
	bool IsUndergroundTransportType(const UnitTypeClass* pType)
	{
		return pType && !pType->Naval && pType->Passengers > 0 && pType->MovementZone == MovementZone::Subterrannean;
	}

	// A transport that moves on land and water (amphibious APC, hovercraft built at a shipyard):
	// it can go around by sea and still drive up to the target.
	bool IsAmphibiousGroundTransportType(const UnitTypeClass* pType)
	{
		return pType && pType->Passengers >= 3
			&& (pType->SpeedType == SpeedType::Amphibious || pType->MovementZone == MovementZone::Amphibious
				|| pType->MovementZone == MovementZone::AmphibiousCrusher || pType->MovementZone == MovementZone::AmphibiousDestroyer);
	}

	// A commando: an armed soldier that blows up buildings with C4 (Tanya, Navy SEAL, ...).
	bool IsCommandoInfantryType(const InfantryTypeClass* pType)
	{
		return pType && pType->C4 && !pType->Engineer && !IsSpyInfantryType(pType);
	}

	// A specialist that works a building or a vehicle instead of fighting: engineer, spy (which
	// also robs refineries and silos), vehicle thief.
	bool IsSpecialistInfantryType(const InfantryTypeClass* pType)
	{
		return pType && (pType->Engineer || IsSpyInfantryType(pType) || pType->VehicleThief);
	}

	// A soldier that only heals or repairs (every weapon it has does negative damage): a medic or a
	// field mechanic, support for the army and no fighter of its own. NeverUse weapons (target
	// scanners such as C&C Reloaded's VirtualScanner, Damage=1) never fire and do not count: with
	// them counted the medic was a fighter, so no healer cap, no wave limit and no escort (match
	// #7002: 37 medics).
	bool IsHealerInfantryType(const InfantryTypeClass* pType)
	{
		if (!pType)
			return false;

		bool heals = false;

		for (int i = 0; i < 2; ++i)
		{
			auto const pWS = const_cast<InfantryTypeClass*>(pType)->GetWeapon(i);
			auto const pWeapon = pWS ? pWS->WeaponType : nullptr;

			if (!pWeapon || pWeapon->NeverUse)
				continue;

			if (pWeapon->Damage > 0)
				return false;

			if (pWeapon->Damage < 0)
				heals = true;
		}

		return heals;
	}

	// A specialist of this type has a job inside this building (it enters it): engineers capture,
	// spies infiltrate.
	bool SpecialistWorksBuilding(const InfantryTypeClass* pType, const BuildingTypeClass* pBldType)
	{
		if (!pType || !pBldType)
			return false;

		if (pType->Engineer)
			return pBldType->Capturable;

		if (IsSpyInfantryType(pType))
			return pBldType->Spyable;

		return false;
	}

	// A warhead that hits an area hard enough to matter (CellSpread 1 cell or more, 50 damage).
	bool IsAreaWeapon(const WeaponTypeClass* pWeapon)
	{
		return pWeapon && pWeapon->Warhead && pWeapon->Warhead->CellSpread >= 1.0f && pWeapon->Damage >= 50;
	}

	// Infantry that deploys into an area weapon (Desolator: Deployer= with DeployFire=), a suicide
	// bomber (weapon Suicide=, or a Phobos FeedbackWeapon= with an area warhead that blows up the
	// firer: cheap but deadly in a crowd), or one whose primary weapon hits an area (flamethrowers).
	bool IsAreaDamageInfantryType(const InfantryTypeClass* pType)
	{
		if (!pType)
			return false;

		if (pType->Deployer && pType->DeployFire)
			return true;

		auto const pSlot = const_cast<InfantryTypeClass*>(pType)->GetWeapon(0);
		auto const pWeapon = pSlot ? pSlot->WeaponType : nullptr;

		if (!pWeapon || !pWeapon->Warhead)
			return false;

		if (pWeapon->Suicide)
			return true;

		if (auto const pWeaponExt = WeaponTypeExt::TryFetch(pWeapon); pWeaponExt && IsAreaWeapon(pWeaponExt->FeedbackWeapon.Get()))
			return true;

		return IsAreaWeapon(pWeapon);
	}

	// A vehicle that blows up among the enemy (demolition truck): weapon Suicide= or an area
	// FeedbackWeapon=, or Explodes= with an area DeathWeapon=.
	bool IsVehicleRaiderType(const UnitTypeClass* pType)
	{
		if (!pType || pType->Naval)
			return false;

		auto const pSlot = const_cast<UnitTypeClass*>(pType)->GetWeapon(0);

		if (auto const pWeapon = pSlot ? pSlot->WeaponType : nullptr)
		{
			if (pWeapon->Suicide)
				return true;

			if (auto const pWeaponExt = WeaponTypeExt::TryFetch(pWeapon); pWeaponExt && IsAreaWeapon(pWeaponExt->FeedbackWeapon.Get()))
				return true;
		}

		return pType->Explodes && IsAreaWeapon(pType->DeathWeapon);
	}

	// A soldier that blows itself up among the enemy (terrorist): weapon Suicide=, or a Phobos
	// FeedbackWeapon= with an area warhead.
	bool IsSuicideInfantryType(const InfantryTypeClass* pType)
	{
		auto const pSlot = pType ? const_cast<InfantryTypeClass*>(pType)->GetWeapon(0) : nullptr;
		auto const pWeapon = pSlot ? pSlot->WeaponType : nullptr;

		if (!pWeapon || !pWeapon->Warhead)
			return false;

		if (pWeapon->Suicide)
			return true;

		auto const pWeaponExt = WeaponTypeExt::TryFetch(pWeapon);

		return pWeaponExt && IsAreaWeapon(pWeaponExt->FeedbackWeapon.Get());
	}

	// A raider for a drop behind the lines: a commando, a bomber (Ivan=), an area damage soldier,
	// or another elite soldier (cost 1000 or more); never a specialist.
	bool IsRaiderInfantryType(const InfantryTypeClass* pType)
	{
		return pType && !IsSpecialistInfantryType(pType)
			&& (IsCommandoInfantryType(pType) || pType->Ivan || IsAreaDamageInfantryType(pType) || pType->Cost >= 1000);
	}

	bool IsTunnelNetworkBuilding(const BuildingTypeClass* pBldType)
	{
		if (!pBldType)
			return false;

		static std::vector<int8_t> TunnelTypeCache;

		static int LastCachedCount = -1;

		if (BuildingTypeClass::Array.Count != LastCachedCount)
		{
			TunnelTypeCache.assign(static_cast<size_t>(std::max(0, BuildingTypeClass::Array.Count)) + 16, -1);

			LastCachedCount = BuildingTypeClass::Array.Count;
		}

		const int idx = pBldType->ArrayIndex;

		if (idx >= 0)
		{
			if (static_cast<size_t>(idx) >= TunnelTypeCache.size())
				TunnelTypeCache.resize(static_cast<size_t>(idx) + 32, -1);

			if (TunnelTypeCache[idx] != -1)
				return TunnelTypeCache[idx] != 0;
		}

		bool isTunnel = false;

		if (CCINIClass::INI_Rules && pBldType->ID && pBldType->ID[0] != '\0')
		{
			char tunnelBuf[64] = {};

			if (CCINIClass::INI_Rules->ReadString(pBldType->ID, "Tunnel", "", tunnelBuf, sizeof(tunnelBuf)) > 0
				&& !INIClass::IsBlank(tunnelBuf))
			{
				isTunnel = true;
			}
		}

		if (!isTunnel)
		{
			if (pBldType->Passengers > 0 && (pBldType->InfantryAbsorb || pBldType->UnitAbsorb)
				&& pBldType->PowerBonus == 0 && !pBldType->Bunker && !pBldType->CanBeOccupied && !pBldType->Grinding
				&& !pBldType->Hospital && !pBldType->Armory)
			{
				isTunnel = true;
			}
		}

		if (idx >= 0)
			TunnelTypeCache[idx] = isTunnel ? 1 : 0;

		return isTunnel;
	}

	bool HasNoManualUnload(const TechnoTypeClass* pType)
	{
		if (!pType)
			return false;

		if (auto const pTypeExt = TechnoTypeExt::TryFetch(const_cast<TechnoTypeClass*>(pType)))
		{
			if (pTypeExt->NoManualUnload.Get(false))
				return true;
		}

		static std::map<const TechnoTypeClass*, int8_t> NoManualUnloadCache;
		auto const it = NoManualUnloadCache.find(pType);
		if (it != NoManualUnloadCache.end())
			return it->second != 0;

		bool val = false;
		if (CCINIClass::INI_Rules && pType->ID && pType->ID[0] != '\0')
			val = CCINIClass::INI_Rules->ReadBool(pType->ID, "NoManualUnload", false);

		NoManualUnloadCache[pType] = val ? 1 : 0;
		return val;
	}

	bool HasNoManualEnter(const TechnoTypeClass* pType)
	{
		if (!pType)
			return false;

		static std::map<const TechnoTypeClass*, int8_t> NoManualEnterCache;
		auto const it = NoManualEnterCache.find(pType);
		if (it != NoManualEnterCache.end())
			return it->second != 0;

		bool val = false;
		if (CCINIClass::INI_Rules && pType->ID && pType->ID[0] != '\0')
			val = CCINIClass::INI_Rules->ReadBool(pType->ID, "NoManualEnter", false);

		if (!val && HasNoManualUnload(pType) && static_cast<int>(pType->SizeLimit) >= 50)
			val = true;

		NoManualEnterCache[pType] = val ? 1 : 0;
		return val;
	}

	bool IsImmobileUnitTypeOnLand(const TechnoTypeClass* pType)
	{
		if (!pType)
			return false;

		if (auto const pTypeExt = TechnoTypeExt::TryFetch(const_cast<TechnoTypeClass*>(pType)))
		{
			if (pTypeExt->NoManualMove.Get())
				return true;
		}

		return (pType->Speed <= 0) || (!pType->Naval && pType->SpeedType == SpeedType::Float);
	}

	TechnoTypeClass* GetConvertDeployTargetType(const TechnoTypeClass* pType)
	{
		if (!pType)
			return nullptr;

		if (auto const pTypeExt = TechnoTypeExt::TryFetch(const_cast<TechnoTypeClass*>(pType)))
		{
			if (auto const pCD = pTypeExt->Convert_Deploy.Get())
				return pCD;

			if (auto const pCUD = pTypeExt->Convert_UniversalDeploy.Get())
				return pCUD;
		}

		return nullptr;
	}

	// Matches Phobos CanEnterNow (Hooks.Transport.cpp:255-280):
	// Enforces individual unit Size <= Transport->SizeLimit AND total capacity
	// (respecting Passengers.BySize).
	bool CanPassengerFitInTransport(const TechnoClass* pTransport, const TechnoTypeClass* pPassengerType,
		int pendingCount = 0, int pendingSize = 0, int reservedSize = 0)
	{
		if (!pTransport || !pPassengerType)
			return false;

		const auto pTransportType = pTransport->GetTechnoType();
		if (!pTransportType || pTransportType->Passengers <= 0)
			return false;

		const int passengerSize = std::max(1, static_cast<int>(pPassengerType->Size));
		if (passengerSize > static_cast<int>(pTransportType->SizeLimit))
			return false;

		const auto pTransportTypeExt = TechnoTypeExt::Fetch(pTransportType);
		const bool bySize = pTransportTypeExt ? pTransportTypeExt->Passengers_BySize.Get() : true;
		const int maxAllowed = std::max(0, pTransportType->Passengers - reservedSize);
		if ((pTransport->Passengers.NumPassengers + pendingCount + 1) > pTransportType->Passengers)
			return false;

		if (bySize)
			return (pTransport->Passengers.GetTotalSize() + pendingSize + passengerSize) <= maxAllowed;

		return (pTransport->Passengers.NumPassengers + pendingCount + 1) <= maxAllowed;
	}

	// What the rules say about how a superweapon is aimed (Ares tags Phobos does not keep): the
	// SW.AITargeting mode in lower case ("" when not set), SW.RequiresTarget mentions water,
	// SW.FireIntoShroud, and SW.Range (cells, Range when not set). Read once per type.
	struct NeuralSWTargeting
	{
		std::string Mode {};
		bool RequiresWater { false };
		bool FireIntoShroud { true };
		double Range { 0.0 };
		// Launch-site ring (SW.RangeMinimum / SW.RangeMaximum, cells; < 0 when not set) and the
		// buildings that fire it: EMPulse.Cannons, else any building with EMPulseCannon=yes for an
		// EMPulse, else the buildings that grant it.
		double RangeMin { -1.0 };
		double RangeMax { -1.0 };
		bool IsEMPulse { false };
		// Its warhead (SW.Warhead) paralyses (EMEffect): aimed at threats near our base, followed up.
		bool IsEMP { false };
		std::vector<std::string> Cannons {};
	};

	const NeuralSWTargeting& GetSWTargeting(const SuperWeaponTypeClass* pSWType)
	{
		static std::unordered_map<const SuperWeaponTypeClass*, NeuralSWTargeting> cache;
		static int cacheSerial = -1;

		if (cacheSerial != NeuralMatchSerial)
		{
			cacheSerial = NeuralMatchSerial;
			cache.clear();
		}

		auto [it, inserted] = cache.try_emplace(pSWType);

		if (inserted && pSWType && CCINIClass::INI_Rules)
		{
			auto const pINI = CCINIClass::INI_Rules;
			char buffer[128] {};

			pINI->ReadString(pSWType->ID, "SW.AITargeting", "", buffer, sizeof(buffer));
			std::string mode = buffer;
			std::transform(mode.begin(), mode.end(), mode.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
			mode.erase(std::remove_if(mode.begin(), mode.end(), [](unsigned char c) { return std::isspace(c); }), mode.end());

			// Not set: the default Ares gives each of its own types (Antares Misc/SWTypes, Initialize).
			// Vanilla types keep "" (their own branches aim them).
			if (mode.empty())
			{
				char typeName[64] {};
				pINI->ReadString(pSWType->ID, "Type", "", typeName, sizeof(typeName));
				std::string type = typeName;
				std::transform(type.begin(), type.end(), type.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
				type.erase(std::remove_if(type.begin(), type.end(), [](unsigned char c) { return std::isspace(c); }), type.end());

				mode = type == "sonarpulse" ? "stealth"
					: type == "battery" ? "lowpower"
					: (type == "droppod" || type == "unitdelivery") ? "paradrop"
					: type == "genericwarhead" ? "offensive"
					: type == "hunterseeker" ? "hunterseeker"
					: type == "protect" ? "forceshield"
					: (type == "empulse" || type == "firestorm") ? "none"
					: "";
			}

			it->second.Mode = mode;

			char required[128] {};
			pINI->ReadString(pSWType->ID, "SW.RequiresTarget", "", required, sizeof(required));
			std::string req = required;
			std::transform(req.begin(), req.end(), req.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
			it->second.RequiresWater = req.find("water") != std::string::npos && req.find("land") == std::string::npos;

			char shroud[32] {};
			pINI->ReadString(pSWType->ID, "SW.FireIntoShroud", "yes", shroud, sizeof(shroud));
			it->second.FireIntoShroud = !(shroud[0] == 'n' || shroud[0] == 'N' || shroud[0] == 'f' || shroud[0] == 'F' || shroud[0] == '0');

			it->second.Range = pINI->ReadDouble(pSWType->ID, "SW.Range", pINI->ReadDouble(pSWType->ID, "Range", 0.0));
			it->second.RangeMin = pINI->ReadDouble(pSWType->ID, "SW.RangeMinimum", -1.0);
			it->second.RangeMax = pINI->ReadDouble(pSWType->ID, "SW.RangeMaximum", -1.0);

			char typeName[64] {};
			pINI->ReadString(pSWType->ID, "Type", "", typeName, sizeof(typeName));
			it->second.IsEMPulse = _stricmp(typeName, "EMPulse") == 0;

			char warhead[64] {};
			pINI->ReadString(pSWType->ID, "SW.Warhead", "", warhead, sizeof(warhead));
			auto const pWarhead = warhead[0] ? WarheadTypeClass::Find(warhead) : nullptr;
			it->second.IsEMP = pWarhead && pWarhead->EMEffect;

			char cannons[512] {};
			pINI->ReadString(pSWType->ID, "EMPulse.Cannons", "", cannons, sizeof(cannons));
			std::string list = cannons;
			size_t start = 0;

			while (start < list.size())
			{
				size_t end = list.find(',', start);

				if (end == std::string::npos)
					end = list.size();

				std::string id = list.substr(start, end - start);
				id.erase(std::remove_if(id.begin(), id.end(), [](unsigned char c) { return std::isspace(c); }), id.end());

				if (!id.empty())
					it->second.Cannons.push_back(id);

				start = end + 1;
			}
		}

		return it->second;
	}

	// Modes that aim at the enemy: the generic "fire at a visible enemy building" branch fits them.
	bool IsOffensiveSWTargetingMode(const std::string& mode)
	{
		return mode.empty() || mode == "nuke" || mode == "lightningstorm" || mode == "offensive" || mode == "enemybase"
			|| mode == "multimissile" || mode == "attack" || mode == "paradrop" || mode == "droppod" || mode == "lightningrandom"
			|| mode == "lighningrandom" || mode == "geneticmutator" || mode == "psychicdominator" || mode == "hunterseeker";
	}

	bool IsTunnelDeliverySuperWeapon(const SuperWeaponTypeClass* pSWType, HouseClass* pOwner)
	{
		if (!pSWType)
			return false;

		// Generic: a tunnel delivery SW is the one granted by a tunnel network building
		// (no type-name matching, so any mod's tunnel naming works).

		if (pOwner)
		{
			for (auto const pBld : pOwner->Buildings)
			{
				if (pBld && !pBld->InLimbo && pBld->IsAlive && pBld->Type && IsTunnelNetworkBuilding(pBld->Type))
				{
					// Indices 0/1 are SuperWeapon/SuperWeapon2, then the SuperWeapons= list (iterated
					// directly: GetSuperWeaponCount undercounts when only SuperWeapon2 is set).
					if (auto const pBldTypeExt = BuildingTypeExt::Fetch(pBld->Type))
					{
						const int swSlots = 2 + static_cast<int>(pBldTypeExt->SuperWeapons.size());

						for (int i = 0; i < swSlots; ++i)
						{
							if (pBldTypeExt->GetSuperWeaponIndex(i) == pSWType->ArrayIndex)
								return true;
						}
					}
				}
			}
		}

		return false;
	}

	// Protects Ore/Tiberium Spawner Trees (SpawnsTiberium) from being crushed or
	// blocked by AI building placement
	bool OverlapsTiberiumTreeZone(CellStruct cell, BuildingTypeClass* pType)
	{
		if (!pType || pType->ResourceDestination)
			return false;

		struct CachedTiberiumTree
		{
			short X;
			short Y;
			short Range;
		};

		static int s_CachedTreesFrame = -1;

		static std::vector<CachedTiberiumTree> s_CachedTrees;

		if (s_CachedTreesFrame != Unsorted::CurrentFrame)
		{
			s_CachedTreesFrame = Unsorted::CurrentFrame;

			s_CachedTrees.clear();

			for (auto const pTerrain : TerrainClass::Array)
			{
				if (pTerrain && pTerrain->IsAlive && !pTerrain->InLimbo && pTerrain->Type
					&& pTerrain->Type->SpawnsTiberium)
				{
					auto const pTerrainTypeExt = TerrainTypeExt::Fetch(pTerrain->Type);

					const short range = static_cast<short>(
						pTerrainTypeExt ? std::max(1, pTerrainTypeExt->SpawnsTiberium_Range.Get()) : 1);

					const CellStruct treeCell = pTerrain->GetMapCoords();

					s_CachedTrees.push_back({ treeCell.X, treeCell.Y, range });
				}
			}
		}

		if (s_CachedTrees.empty())
			return false;

		const int foundationW = pType->GetFoundationWidth();
		const int foundationH = pType->GetFoundationHeight(false);
		const int minX = cell.X;
		const int maxX = cell.X + foundationW - 1;
		const int minY = cell.Y;
		const int maxY = cell.Y + foundationH - 1;

		for (const auto& tree : s_CachedTrees)
		{
			if (minX <= tree.X + tree.Range && maxX >= tree.X - tree.Range && minY <= tree.Y + tree.Range
				&& maxY >= tree.Y - tree.Range)
			{
				return true;
			}
		}

		return false;
	}

	bool OverlapsBridge(CellStruct cell, BuildingTypeClass* pType)
	{
		if (!pType)
			return false;

		const int foundationW = pType->GetFoundationWidth();
		const int foundationH = pType->GetFoundationHeight(false);

		for (int dy = 0; dy < foundationH; ++dy)
		{
			for (int dx = 0; dx < foundationW; ++dx)
			{
				const CellStruct checkCell { static_cast<short>(cell.X + dx), static_cast<short>(cell.Y + dy) };

				if (!MapClass::Instance.CoordinatesLegal(checkCell))
					continue;

				if (auto const pCell = MapClass::Instance.TryGetCellAt(checkCell))
				{
					if (pCell->ContainsBridge())
						return true;
				}
			}
		}

		return false;
	}

	int GetSupportRadius(const BuildingTypeClass* pType)
	{
		if (!pType)
			return 0;

		int minRadius = 999;

		if (pType->CloakGenerator && pType->CloakRadiusInCells > 0 && pType->CloakRadiusInCells < minRadius)
			minRadius = pType->CloakRadiusInCells;

		auto const pBldExt = BuildingTypeExt::Fetch(pType);
		const bool isGapGen = (pBldExt && pBldExt->GapGenerator) || pType->GapGenerator;

		const int gapRadius
			= (pBldExt && pBldExt->GapRadiusInCells != 0) ? pBldExt->GapRadiusInCells.Get() : pType->GapRadiusInCells;

		const int superGapRadius = (pBldExt && pBldExt->SuperGapRadiusInCells != 0)
			? pBldExt->SuperGapRadiusInCells.Get()
			: pType->SuperGapRadiusInCells;

		if (isGapGen)
		{
			if (gapRadius > 0 && gapRadius < minRadius)
				minRadius = gapRadius;

			if (superGapRadius > 0 && superGapRadius < minRadius)
				minRadius = superGapRadius;
		}

		if (pType->SensorArray && pType->SensorsSight > 0 && pType->SensorsSight < minRadius)
			minRadius = pType->SensorsSight;

		if (auto const pTechExt = TechnoTypeExt::Fetch(pType))
		{
			if (pTechExt->InhibitorRange.isset() && pTechExt->InhibitorRange.Get() > 0
				&& pTechExt->InhibitorRange.Get() < minRadius)
			{
				minRadius = pTechExt->InhibitorRange.Get();
			}

			if (pTechExt->RadarJamRadius.Get() > 0 && pTechExt->RadarJamRadius.Get() < minRadius)
				minRadius = pTechExt->RadarJamRadius.Get();
		}

		return minRadius == 999 ? 0 : minRadius;
	}

	// Calculates real power drain (including toggled-off structures) and power grid
	// vulnerability metrics
	int GetRealPowerDrainAndGridMetrics(HouseClass* pHouse, bool& outHasUnpoweredBuildings,
		int& outPoweredDefAndSWCount, int& outMaxSinglePlantBonus, bool& outHasAdvancedPowerPlants)
	{
		outHasUnpoweredBuildings = false;

		outPoweredDefAndSWCount = 0;

		outMaxSinglePlantBonus = 0;

		outHasAdvancedPowerPlants = false;

		if (!pHouse)
			return 0;

		int drain = pHouse->PowerDrain;

		for (auto const pBld : pHouse->Buildings)
		{
			if (!pBld || !pBld->IsAlive || pBld->InLimbo || !pBld->Type)
				continue;

			auto const pOT = pBld->Type;

			if (pOT->TogglePower && !pBld->StuffEnabled)
			{
				drain += pOT->PowerDrain;

				outHasUnpoweredBuildings = true;
			}

			if (pOT->PowerBonus > pOT->PowerDrain && !pOT->IsBaseDefense)
			{
				if (pOT->PowerBonus > outMaxSinglePlantBonus)
					outMaxSinglePlantBonus = pOT->PowerBonus;

				if (pOT->PowerBonus >= 300 || pOT->Cost >= 1000)
					outHasAdvancedPowerPlants = true;
			}

			if ((pOT->IsBaseDefense || pOT->SuperWeapon != -1) && (pOT->Powered || pOT->PowerDrain > 0))
				outPoweredDefAndSWCount++;
		}

		return drain;
	}

} // namespace

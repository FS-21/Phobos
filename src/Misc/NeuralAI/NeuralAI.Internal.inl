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
		FerryLandingRetried,
		GarrisonTraining,
		ResourceDetour,
		ResourceHealStop,
		FerryTakenAboard,
		ScoutSweep,
		SensorLookout,
		McvIdleUnfold,
		McvIdleSite,
		McvIdleHome,
		OpHoldMoved,
		JamBarrierBreach,
		OpNoTransportReach,
		BackdoorNoCarrier,
		McvIdleStepOnSite,
		McvUnfoldRetry,
		NavalStrikeInRange,
		NavalPlanShipsFreed,
		UrbanQuietRelease,
		FerryDepartedStuck,
		FerryMcvNowhere,
		DeployerIdleWatchdog,
		DeployerSiteFailed,
		DeployerGoalNoFit,
		NavalStuckReleased,
		NavalPreyAttack,
		NavalSweep,
		NavalMassStrike,
		NavalPassageWait,
		OrderLeaseRefused,
		EngineerCaptured,
		EnemyBldCaptured,
		ConYardCaptured,
		EngineerLostOnWay,
		SpyInfiltrated,
		SpyDisguisedEntry,
		SpyLostOnWay,
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
		"ferryDoorRegistered", "ferryStrayLinkCut", "ferryTargetDropped", "ferryDoorCellMove", "ferryLandingRetried", "garrisonTraining",
		"resourceDetour", "resourceHealStop", "ferryTakenAboard", "scoutSweep", "sensorLookout",
		"mcvIdleUnfold", "mcvIdleSite", "mcvIdleHome", "opHoldMoved", "jamBarrierBreach", "opNoTransportReach", "backdoorNoCarrier",
		"mcvIdleStepOnSite", "mcvUnfoldRetry", "navalStrikeInRange", "navalPlanShipsFreed", "urbanQuietRelease",
		"ferryDepartedStuck", "ferryMcvNowhere", "deployerIdleWatchdog", "deployerSiteFailed", "deployerGoalNoFit",
		"navalStuckReleased", "navalPreyAttacks", "navalSweeps", "navalMassStrikes", "navalPassageWaits", "orderLeaseRefused",
		"engineerCaptured", "enemyBldCaptured", "conYardCaptured", "engineerLostOnWay", "spyInfiltrated", "spyDisguisedEntry", "spyLostOnWay" };

	static_assert(std::size(NeuralDecisionNames) == static_cast<size_t>(NeuralDecision::Count));

	std::array<std::array<uint32_t, static_cast<size_t>(NeuralDecision::Count)>, 32> DecisionCounts {};

	// Learned gate verdicts per house and slot this match ([open, closed]): the trainer finds the
	// slots pushed so far that the gate always gives the same answer (no gradient reaches them).
	std::array<std::array<std::array<uint32_t, 2>, 128>, 32> GateCounts {};

	// Statistics of the SlotGate draws of the current trajectory window (headless training only;
	// nothing in the simulation reads them).
	struct GateWindowStats
	{
		std::array<uint16_t, 128> Draws {};
		std::array<uint16_t, 128> Opens {};
		std::array<uint16_t, 128> FreeDraws {};
		std::array<int32_t, 128> ScoreFnSum {};
	};

	std::array<GateWindowStats, 8> GateWindow {};

	// One bit per draw hash, so a held draw counts once, at its first read. 1 << 16 words per house,
	// allocated at match start in headless mode only (where GateCounts is cleared).
	std::array<std::vector<uint64_t>, 8> GateSeenDraws {};

	struct TrajRoutingRecord
	{
		uint16_t PoolMask { 1u };
		uint8_t BestExpert { 0 };
		uint8_t RoutingFlags { 0 };
	};

	std::array<TrajRoutingRecord, 8> LastRoutingRecords {};

	struct PickEval
	{
		bool Valid { false };
		uint8_t Expert { 0 };
		std::array<int, 40> BestTotal {};
		std::array<int, 40> BestType {};
		std::array<bool, 40> BestFree {};
	};

	// One evaluation per factory of the engine: 0 structures, 1 infantry, 2 vehicles, 3 naval,
	// 4 aircraft, 5 base defenses. Structures and base defenses (BuildCat::Combat) are two factories
	// that build at the same time: with one evaluation for both, a defense led 73% of the building
	// starts that another class made (25 training matches, 2026-11-25), and each of them was
	// recorded as overridden. The trajectory step files both under queue 0.
	constexpr int PickEvalStructures = 0;
	constexpr int PickEvalDefenses = 5;
	constexpr int PickEvalQueues = 6;

	std::array<std::array<PickEval, PickEvalQueues>, 8> PickEvals {};

	void ResetPickEval(const HouseClass* pHouse, int queue, int activeExpert)
	{
		if (!NeuralAI::HeadlessMode || !pHouse || pHouse->ArrayIndex < 0 || pHouse->ArrayIndex >= 8 || queue < 0 || queue >= PickEvalQueues)
			return;

		auto& eval = PickEvals[pHouse->ArrayIndex][queue];
		eval.Valid = true;
		eval.Expert = static_cast<uint8_t>(std::clamp(activeExpert, 0, 9));
		eval.BestTotal.fill(INT_MIN);
		eval.BestType.fill(-1);
		eval.BestFree.fill(false);
	}

	void NotePickCandidate(const HouseClass* pHouse, int queue, int candSlot, bool candFree, int typeIndex, int score)
	{
		if (!NeuralAI::HeadlessMode || !pHouse || pHouse->ArrayIndex < 0 || pHouse->ArrayIndex >= 8
			|| queue < 0 || queue >= PickEvalQueues || candSlot < 0 || candSlot >= 40)
		{
			return;
		}

		auto& eval = PickEvals[pHouse->ArrayIndex][queue];

		if (score > eval.BestTotal[candSlot])
		{
			eval.BestTotal[candSlot] = score;
			eval.BestType[candSlot] = typeIndex;
			eval.BestFree[candSlot] = candFree;
		}
	}

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

	// Archetype intent route, checkpoint milestones and segment values handed over by the trainer.
	constexpr int IntentCheckpoints = 7;
	constexpr int IntentSegments = 8;
	constexpr int IntentQuantities = 12;
	constexpr int IntentCheckpointFrames[IntentCheckpoints] = { 6300, 13500, 22500, 36000, 49500, 63000, 76500 };
	constexpr int IntentLateReviewFrames = 13500;
	constexpr int IntentFitWeight = 4;
	constexpr int IntentStayBonus = 100;
	constexpr int IntentSwitchMargin = 120;
	constexpr int IntentMaxSwitches = 3;
	constexpr int IntentExploreOneIn = 12;
	constexpr uint8_t IntentNoTarget = 255;

	#pragma pack(push, 1)

	struct NeuralIntentPlanHeader
	{
		char Magic[8];
		uint32_t Version;
		uint32_t Enabled;
		uint32_t Checkpoints;
		uint32_t Archetypes;
		uint32_t Quantities;
		uint32_t MapClasses;
		uint32_t MapClass;
	};

	struct NeuralIntentPlanBlock
	{
		NeuralIntentPlanHeader Header;
		uint8_t Milestones[IntentCheckpoints][NeuralAI::NumArchetypes][IntentQuantities];
		int16_t SegmentValue[IntentSegments][NeuralAI::NumArchetypes];
		int16_t MapValue[NumMapClasses][NeuralAI::NumArchetypes];
	};

	#pragma pack(pop)

	constexpr size_t IntentPlanFileSize = sizeof(NeuralIntentPlanBlock);

	static_assert(sizeof(NeuralIntentPlanHeader) == 36);
	static_assert(sizeof(NeuralIntentPlanBlock) == 884);
	static_assert(offsetof(NeuralIntentPlanBlock, Milestones) == 36);
	static_assert(offsetof(NeuralIntentPlanBlock, SegmentValue) == 708);
	static_assert(offsetof(NeuralIntentPlanBlock, MapValue) == 836);

	struct NeuralIntentPlan
	{
		bool Loaded { false };
		bool Enabled { false };
		int MapClass { -1 };
		uint8_t Milestones[IntentCheckpoints][NeuralAI::NumArchetypes][IntentQuantities] {};
		int16_t SegmentValue[IntentSegments][NeuralAI::NumArchetypes] {};
		int16_t MapValue[NumMapClasses][NeuralAI::NumArchetypes] {};
	};

	NeuralIntentPlan IntentPlan {};
	NeuralIntentPlan OpponentIntentPlan {};

	// The map class the plan uses: the trainer's when it gave one, else the one the DLL computes
	// (-1 before frame 30).
	int IntentMapClass(const NeuralIntentPlan& plan)
	{
		return plan.MapClass >= 0 ? plan.MapClass : CurrentMapClass();
	}

	bool ParseIntentPlan(const uint8_t* pData, size_t size, NeuralIntentPlan& out)
	{
		if (!pData || size != IntentPlanFileSize)
			return false;

		NeuralIntentPlanHeader header {};
		std::memcpy(&header, pData, sizeof(header));

		if (std::memcmp(header.Magic, "PHOBOSIP", 8) != 0
			|| header.Version != 1u
			|| (header.Enabled != 0u && header.Enabled != 1u)
			|| header.Checkpoints != static_cast<uint32_t>(IntentCheckpoints)
			|| header.Archetypes != static_cast<uint32_t>(NeuralAI::NumArchetypes)
			|| header.Quantities != static_cast<uint32_t>(IntentQuantities)
			|| header.MapClasses != static_cast<uint32_t>(NumMapClasses)
			|| (header.MapClass != 0xFFFFFFFFu && header.MapClass >= static_cast<uint32_t>(NumMapClasses)))
		{
			return false;
		}

		out.Loaded = true;
		out.Enabled = header.Enabled == 1u;
		out.MapClass = header.MapClass == 0xFFFFFFFFu ? -1 : static_cast<int>(header.MapClass);
		std::memcpy(out.Milestones, pData + offsetof(NeuralIntentPlanBlock, Milestones), sizeof(out.Milestones));
		std::memcpy(out.SegmentValue, pData + offsetof(NeuralIntentPlanBlock, SegmentValue), sizeof(out.SegmentValue));
		std::memcpy(out.MapValue, pData + offsetof(NeuralIntentPlanBlock, MapValue), sizeof(out.MapValue));

		return true;
	}

	bool LoadIntentPlanFile(const char* pPath, NeuralIntentPlan& out)
	{
		HANDLE hFile = CreateFileA(pPath, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);

		if (hFile == INVALID_HANDLE_VALUE)
			return false;

		LARGE_INTEGER fileSize {};

		if (!GetFileSizeEx(hFile, &fileSize) || static_cast<size_t>(fileSize.QuadPart) != IntentPlanFileSize)
		{
			CloseHandle(hFile);

			return false;
		}

		std::array<uint8_t, IntentPlanFileSize> buffer {};
		DWORD read = 0;
		const bool ok = ReadFile(hFile, buffer.data(), static_cast<DWORD>(buffer.size()), &read, NULL) != FALSE;

		CloseHandle(hFile);

		return ok && ParseIntentPlan(buffer.data(), read, out);
	}

	bool LoadIntentPlanFromSharedMemory(const char* pName, NeuralIntentPlan& out)
	{
		HANDLE hMapping = OpenFileMappingA(FILE_MAP_READ, FALSE, pName);

		if (!hMapping)
			return false;

		bool ok = false;

		if (auto const pView = static_cast<const uint8_t*>(MapViewOfFile(hMapping, FILE_MAP_READ, 0, 0, IntentPlanFileSize)))
		{
			ok = ParseIntentPlan(pView, IntentPlanFileSize, out);
			UnmapViewOfFile(pView);
		}

		CloseHandle(hMapping);

		return ok;
	}

	// An opponent in headless training never falls back to the challenger's intent plan when it has none.
	const NeuralIntentPlan& IntentPlanFor(const HouseClass* pHouse)
	{
		if (NeuralAI::HeadlessMode)
		{
			auto const pExt = HouseExt::TryFetch(pHouse);

			if (pExt && pExt->NeuralAI_PolicySlot == 1)
				return OpponentIntentPlan;
		}

		return IntentPlan;
	}

	// How much of the milestone of `archetype` at `checkpoint` the snapshot has, 0..100. 50 when the
	// milestone has no target yet.
	int IntentFit(const NeuralIntentPlan& plan, int checkpoint, int archetype, const uint8_t snapshot[IntentQuantities])
	{
		int sum = 0;
		int known = 0;

		for (int k = 0; k < IntentQuantities; ++k)
		{
			const int target = plan.Milestones[checkpoint][archetype][k];

			if (target == IntentNoTarget)
				continue;

			known++;
			sum += target == 0 ? 100 : std::min(100, snapshot[k] * 100 / target);
		}

		return known > 0 ? sum / known : 50;
	}

	void IntentWorths(HouseClass* pHouse, int& knownFoe, int& kills, int& losses);
	void TakeIntentSnapshot(HouseClass* pHouse, uint8_t out[IntentQuantities]);
	void ReviewIntent(HouseClass* pHouse);

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
	// TEMP diagnostic (NeuralAI.Orders.inl): names the part of the AI tick that moved a transport
	// the loader left parked to load.
	void TraceParkedFerries(int houseIndex, const char* part);

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
			TraceParkedFerries(this->HouseIndex, NeuralSectionNames[static_cast<size_t>(this->Current)]);
			AddSectionTime(this->HouseIndex, this->Current, ElapsedMs(this->Start));
			this->Current = next;
			QueryPerformanceCounter(&this->Start);
		}

		~NeuralPhaseClock()
		{
			TraceParkedFerries(this->HouseIndex, NeuralSectionNames[static_cast<size_t>(this->Current)]);
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

			// The heaviest leaf phases of a set of per-phase times (the aggregate micro / production
			// entries excluded), `most` of them at `least` ms or more, as text.
			auto const heaviest = [](const std::array<double, static_cast<size_t>(NeuralSection::Count)>& times, double scale,
				size_t most, double least)
			{
				std::vector<std::pair<double, size_t>> parts;

				for (size_t sec = 0; sec < times.size(); ++sec)
				{
					if (sec != static_cast<size_t>(NeuralSection::Micro) && sec != static_cast<size_t>(NeuralSection::Production)
						&& times[sec] * scale >= least)
					{
						parts.emplace_back(times[sec] * scale, sec);
					}
				}

				std::sort(parts.begin(), parts.end(), [](auto const& a, auto const& b) { return a.first > b.first; });
				std::string text;

				for (size_t k = 0; k < parts.size() && k < most; ++k)
				{
					char part[64];
					std::snprintf(part, sizeof(part), "%s%s %.*f", k ? ", " : "", NeuralSectionNames[parts[k].second], least < 0.5 ? 2 : 1,
						parts[k].first);
					text += part;
				}

				return text;
			};

			if (ms >= 15.0)
			{
				auto& peaks = TickPeaks[this->HouseIndex];

				if (peaks.size() < 10 || ms > peaks.back().Ms)
				{
					peaks.push_back({ Unsorted::CurrentFrame, ms, TechnoClass::Array.Count, heaviest(TickSectionMs[this->HouseIndex], 1.0, 3, 0.5) });
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
		Reprisal,
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
		ClearOurGround,
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
		Feint,
		BaitAmbush,
		CounterSquad,
		EdgeBlitz,
		Count
	};

	constexpr const char* NeuralPlanNames[] = { "ecoRaid", "siegeBattery", "airStrike", "blackout", "reprisal", "timingRush",
		"heavyAirPush", "islandHold", "airborneFront", "expansionDenial", "baseTrade", "holdUnderTowers", "swRace", "pincer",
		"harvesterAmbush", "pickets", "keyPointHold", "survival", "preemptiveStrike", "clearGround", "allyJointAttack",
		"forwardRepair", "navalStrike", "engineerAssault", "sabotage", "huntKeyUnits", "rotatingRaids", "airCorridor",
		"silentInfiltration", "artilleryEcoSiege", "rollingSiege", "delayingAction", "threeFront", "factoryDecapitation",
		"cutReinforcements", "techTreeCut", "comboBlackoutPush", "comboWatchStrike", "allyBackup", "allyForwardOutpost", "baseThreatIntercept",
		"feint", "baitAmbush", "counterSquad", "edgeBlitz" };

	// Trained preference of each plan: free reserve output slots 89.. (see the trainer's
	// OUTPUT_SLOT_NAMES), learned by ES like any other slot. The ally site plans share the slot of
	// the plan closest to them: Ally Backup with Survival (106), Ally Forward Outpost with Ally Joint
	// Attack (109). Base Threat Intercept shares Preemptive Strike's slot (107): both hit an enemy
	// force with a stronger idle group before it reaches us. Feint and Bait Ambush, which only
	// start against a human, share those of Pincer (102) and Hold Under Towers (100). Their own
	// slots went to the missions of the tank mass, which have no plan instance: Reprisal (93) and
	// Clear Our Ground (108) weigh the target of its main wave (TankMassMissionBonus) and count
	// the waves sent for them as their starts, with the wave verdict as their result.
	// Counter Squad has no slot (-1) and never goes through the learned gate (shouldTryStart): it is
	// a fixed rule, and the trainer credits no slot for a plan name it does not know. Edge Blitz shares
	// Pincer's (102): both come at the enemy from another side than its front.
	constexpr int NeuralPlanSlots[] = { 89, 90, 91, 92, 93, 94, 95, 96, 97, 98, 99, 100, 101, 102, 103, 104, 105, 106, 107,
		108, 109, 110, 111, 112, 113, 114, 115, 116, 117, 118, 119, 120, 121, 122, 123, 124, 125, 126, 106, 109, 107, 102, 100, -1, 102 };

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

	// Training focus (trainer matches only): the trainer may force an expert in charge, an
	// archetype and plans that start whenever they have a target and a group, so a weak strategy
	// is played many times in a row and its failures can be read. It comes as environment
	// variables (PHOBOS_FOCUS_EXPERT, PHOBOS_FOCUS_ARCHETYPE, PHOBOS_FOCUS_PLANS: plan names
	// separated by commas), the same for every client of a match. Real games never read them.
	struct NeuralTrainingFocus
	{
		bool Loaded { false };
		int Expert { -1 };
		int Archetype { -1 };
		// Half focus (PHOBOS_FOCUS_ARCHETYPE_HALF): only the houses of one policy slot take the
		// focused archetype and the others choose theirs, so it is measured against the rest.
		bool ArchetypeHalf { false };
		int ArchetypeSlot { 0 };
		bool AnyPlan { false };
		std::array<bool, static_cast<size_t>(NeuralPlanId::Count)> Plans {};
	};

	NeuralTrainingFocus TrainingFocusState;

	// The focused expert leads all but one of every FocusExpertWindows windows of 900 frames.
	constexpr int FocusExpertWindows = 4;

	// Archetype choice at match start (Lifecycle): the faction memory counts by its mean result per
	// match (win +12..+16, loss -6, stalemate +-3) times ArchetypeMemoryWeight, at most
	// ArchetypeMemoryCap either way; an archetype with few matches is pulled toward the side's mean
	// as if it had played ArchetypeMemoryPrior matches there. In training matches an archetype gets
	// ArchetypeTryBonus / sqrt(1 + its matches for this side), so every one keeps being tried.
	constexpr int ArchetypeMemoryPrior = 4;
	constexpr int ArchetypeMemoryWeight = 40;
	constexpr int ArchetypeMemoryCap = 600;
	constexpr double ArchetypeTryBonus = 600.0;

	const NeuralTrainingFocus& TrainingFocus()
	{
		auto& focus = TrainingFocusState;

		if (focus.Loaded)
			return focus;

		focus.Loaded = true;

		if (!NeuralAI::TrainingMode)
			return focus;

		char text[512] {};

		if (GetEnvironmentVariableA("PHOBOS_FOCUS_EXPERT", text, sizeof(text)) > 0)
		{
			const int expert = std::atoi(text);

			if (expert >= 0 && expert < NeuralAI::NumSpecialists)
				focus.Expert = expert;
		}

		if (GetEnvironmentVariableA("PHOBOS_FOCUS_ARCHETYPE", text, sizeof(text)) > 0)
		{
			const int archetype = std::atoi(text);

			if (archetype >= 0 && archetype < NeuralAI::NumArchetypes)
				focus.Archetype = archetype;
		}

		if (GetEnvironmentVariableA("PHOBOS_FOCUS_ARCHETYPE_HALF", text, sizeof(text)) > 0)
			focus.ArchetypeHalf = text[0] == '1' || text[0] == 't';

		std::string plans;

		if (GetEnvironmentVariableA("PHOBOS_FOCUS_PLANS", text, sizeof(text)) > 0)
		{
			plans = text;
			size_t start = 0;

			while (start <= plans.size())
			{
				const size_t comma = plans.find(',', start);
				const std::string name = plans.substr(start, comma == std::string::npos ? std::string::npos : comma - start);

				for (size_t i = 0; i < std::size(NeuralPlanNames); ++i)
				{
					if (_stricmp(name.c_str(), NeuralPlanNames[i]) == 0)
					{
						focus.Plans[i] = true;
						focus.AnyPlan = true;
					}
				}

				if (comma == std::string::npos)
					break;

				start = comma + 1;
			}
		}

		if (focus.Expert >= 0 || focus.Archetype >= 0 || focus.AnyPlan)
		{
			Debug::Log("[NeuralAI] Training focus: expert %d, archetype %d, plans [%s]%s.\n", focus.Expert, focus.Archetype,
				focus.AnyPlan ? plans.c_str() : "", focus.ArchetypeHalf ? ", archetype for one policy slot only" : "");
		}

		return focus;
	}

	// Half focus: the policy slot whose houses take the focused archetype this match, drawn from the
	// scenario's random numbers (the same draw on every client of the match).
	void DrawFocusArchetypeSlot()
	{
		TrainingFocus();
		auto& focus = TrainingFocusState;

		if (focus.Archetype < 0 || !focus.ArchetypeHalf)
			return;

		focus.ArchetypeSlot = ScenarioClass::Instance->Random.RandomRanged(0, 1);
		Debug::Log("[NeuralAI] Training focus: archetype %d goes to the houses of policy slot %d.\n", focus.Archetype, focus.ArchetypeSlot);
	}

	// The archetype a house takes: the focused one in a focus match (in a half focus match only
	// for the houses of the drawn policy slot), else the chosen one.
	int FocusedArchetype(int chosen, int policySlot)
	{
		auto const& focus = TrainingFocus();

		if (focus.Archetype < 0 || (focus.ArchetypeHalf && policySlot != focus.ArchetypeSlot))
			return chosen;

		return focus.Archetype;
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
		bool ConsumedOnMission { false };
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

	// TEMP diagnostic (tank census, 2026-11-18; NoteMainTankDeath): the code that gave each unit its
	// last move, attack-move or dispatch order (user: what sends the tanks into the enemy bases?).
	struct NeuralOrderCaller
	{
		const char* Kind { nullptr };
		const char* File { nullptr };
		int Line { 0 };
		int Frame { 0 };
	};

	std::array<std::unordered_map<DWORD, NeuralOrderCaller>, 32> LastOrderCaller {};

	void NoteOrderLease(TechnoClass* pTechno, const char* file, int line);

	// Every order noted here also leases the unit to its code (NoteOrderLease, NeuralAI.State.inl), in
	// every mode; the record of the last caller is kept in training only.
	void NoteOrderCaller(TechnoClass* pTechno, const char* kind, const char* file = __builtin_FILE(), int line = __builtin_LINE())
	{
		NoteOrderLease(pTechno, file, line);

		if (!NeuralAI::HeadlessMode || !pTechno || !pTechno->Owner || pTechno->Owner->ArrayIndex < 0 || pTechno->Owner->ArrayIndex >= 32)
			return;

		const char* const slash = file ? std::max(std::strrchr(file, '\\'), std::strrchr(file, '/')) : nullptr;
		LastOrderCaller[pTechno->Owner->ArrayIndex][pTechno->UniqueID] = { kind, slash ? slash + 1 : file, line, Unsorted::CurrentFrame };
	}

	void ResetExpertCredit()
	{
		for (auto& tags : CreditTags)
			tags.clear();

		for (auto& callers : LastOrderCaller)
			callers.clear();

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

	// A held gate (SlotGateHeld) keeps its draw this long: a unit that a gate sends one way keeps that
	// way instead of turning each AI tick (user, 2026-11-19: units drove halfway, turned back and took
	// their goal up again). The open share is the same as with a draw per tick.
	constexpr int SlotGateHoldFrames = 900;

	bool SlotGateDraw(int slotScore, int threshold, int slot, uint32_t period, const uint32_t* pHolder)
	{
		constexpr double Softness = 120.0;
		const double margin = static_cast<double>(slotScore - threshold);

		double probability = 1.0 / (1.0 + DeterministicExp(-std::clamp(margin / Softness, -8.0, 8.0)));

		if (NeuralAI::HeadlessMode)
			probability = std::clamp(probability, 0.03, 0.97);

		uint32_t h = 2166136261u;

		const uint32_t salt = (CurrentAIHouseIndex >= 0 && CurrentAIHouseIndex < 32) ? HouseStyleSalt[CurrentAIHouseIndex] : 0u;

		for (const uint32_t v : { period, static_cast<uint32_t>(CurrentAIHouseIndex + 1), static_cast<uint32_t>(slot), salt })
		{
			h = (h ^ v) * 16777619u;
			h ^= h >> 13;
		}

		if (pHolder)
		{
			h = (h ^ *pHolder) * 16777619u;
			h ^= h >> 13;
		}

		const bool open = static_cast<double>(h % 10000u) < probability * 10000.0;
		CountDecision(open ? NeuralDecision::SlotGateOpen : NeuralDecision::SlotGateClosed);

		if (CurrentAIHouseIndex >= 0 && CurrentAIHouseIndex < 32 && slot >= 0 && slot < 128)
			GateCounts[CurrentAIHouseIndex][slot][open ? 0 : 1]++;

		if (NeuralAI::HeadlessMode && CurrentAIHouseIndex >= 0 && CurrentAIHouseIndex < 8 && slot >= 0 && slot < 128)
		{
			auto& seen = GateSeenDraws[CurrentAIHouseIndex];
			const uint32_t bit = h & ((1u << 22) - 1u);

			if (!seen.empty() && !(seen[bit >> 6] & (1ULL << (bit & 63u))))
			{
				seen[bit >> 6] |= 1ULL << (bit & 63u);
				auto& window = GateWindow[CurrentAIHouseIndex];
				const double rawProbability = 1.0 / (1.0 + DeterministicExp(-std::clamp(margin / Softness, -8.0, 8.0)));

				if (window.Draws[slot] < 65535u)
				{
					window.Draws[slot]++;

					if (open)
						window.Opens[slot]++;

					if (rawProbability >= 0.03 && rawProbability <= 0.97)
					{
						window.FreeDraws[slot]++;
						window.ScoreFnSum[slot] += static_cast<int32_t>(std::lround(((open ? 1.0 : 0.0) - probability) * 16384.0));
					}
				}
			}
		}

		return open;
	}

	// A draw per AI tick: for gates that start something (a plan, a build, a strike), where a draw
	// per tick only decides how soon.
	bool SlotGate(int slotScore, int threshold, int slot)
	{
		return SlotGateDraw(slotScore, threshold, slot, static_cast<uint32_t>(Unsorted::CurrentFrame / std::max(1, CurrentAITickFrames)),
			nullptr);
	}

	// A draw held for SlotGateHoldFrames: for gates that steer what a unit, a building or the house
	// keeps doing. Each holder (a UniqueID; 0 for the house) draws on its own and its windows start at
	// its own phase, so the holders of a house do not all switch together.
	bool SlotGateHeld(int slotScore, int threshold, int slot, DWORD holder = 0)
	{
		const uint32_t key = static_cast<uint32_t>(holder);
		const uint32_t phase = (key * 2654435761u) % static_cast<uint32_t>(SlotGateHoldFrames);
		const uint32_t window = (static_cast<uint32_t>(std::max(0, Unsorted::CurrentFrame)) + phase) / static_cast<uint32_t>(SlotGateHoldFrames);

		return SlotGateDraw(slotScore, threshold, slot, window, &key);
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
	//
	// The match's vision rule. Without fog of war (training matches) the map only has the shroud:
	// a cell once explored stays revealed with whatever stands on it (units moving there, new
	// buildings) until a working gap generator of an enemy covers it again; under the gap only the
	// cells in sight now show, and once the gap goes they stay shrouded until looked at again. With
	// fog of war only what is in sight now is seen.
	constexpr int VisionMapSide = 512;

	// Explored map of the house of the last rebuild (cells under an enemy gap only while in sight).
	std::vector<uint8_t> VisionMap;
	// Cells in sight of the AI and its allies right now.
	std::vector<uint8_t> CurrentSightMap;
	std::array<std::vector<uint8_t>, 8> HouseExploredMaps;
	int LastExploredResetFrame = -1;

	int VisionMapFrame = -1;
	int VisionMapOwner = -1;

	// Map-wide reveals and reshrouds of one house, by ArrayIndex. The engine reveals the whole map
	// for a house with MapClass::Clear_Shroud (0x577D90: a working Spy Satellite, a reveal crate, a
	// warhead with Reveal=-1 such as the one the "Reveal map" game option fires for every player),
	// which sets HouseClass::MapIsClear (+0x241) of that house on every client, and shrouds it again
	// with the MapClass::Reset_Shroud functions (losing the Spy Satellite, a darkness crate, a spy in
	// its radar, a reshroud trigger action or team mission), which clear the flag of the house they
	// are given; without a house they shroud the map of every house and clear no flag. The reshroud
	// trigger "Eternal darkness" fires every minute belongs to no house and gives the action the
	// house HouseClass::As_Pointer finds for it (house 0 in the training logs): only that house is
	// shrouded again, on every client. HouseClass::SpySatActive (+0x577A) only
	// says a Spy Satellite works. A reveal is MapIsClear turning on; the reshrouds come from hooks at
	// the start of the three functions (NeuralAI::OnMapReshroud), all called by the match logic on
	// every client.
	struct NeuralMapShroudState
	{
		bool Revealed { false };
		bool ReshroudPending { false };
		// The enemy houses whose radar a spy of this house entered, as last logged.
		uint32_t SpiedRadars { 0 };
	};

	std::array<NeuralMapShroudState, 32> MapShroudStates {};

	void MarkMapReshroud(HouseClass* pHouse)
	{
		const int only = pHouse ? std::clamp(pHouse->ArrayIndex, 0, 31) : -1;

		for (int i = 0; i < static_cast<int>(MapShroudStates.size()); ++i)
		{
			if (only >= 0 && i != only)
				continue;

			MapShroudStates[i].ReshroudPending = true;

			// The engine cleared MapIsClear: turning on again is a new reveal.
			if (only >= 0)
				MapShroudStates[i].Revealed = false;
		}

		VisionMapFrame = -1;
		Debug::Log("[NeuralAI] Map reshroud for house %d at frame %d.\n", only, Unsorted::CurrentFrame);
	}

	bool IsFogOfWarMatch()
	{
		return GameModeOptionsClass::Instance.FogOfWar || (ScenarioClass::Instance && ScenarioClass::Instance->SpecialFlags.FogOfWar);
	}

	// Sight radius in cells of one object in the vision rule (it lends its sight to its owner
	// and the owner's allies), 0 for an object off the map that sees nothing. A spy plane on its
	// run (AircraftClass::Mission_Spyplane_Approach 0x4155F0 and _Overfly 0x4157C0) looks while its
	// target is within its first weapon's Range, with that weapon's Damage as the radius
	// (TechnoClass::Look 0x70AF50 takes it over Sight): SPYP has Sight=0 and a camera weapon.
	int GetAISightRadius(const TechnoClass* pTechno)
	{
		if (!pTechno || pTechno->InLimbo || !pTechno->IsAlive || !pTechno->Owner)
			return 0;

		auto const pType = pTechno->GetTechnoType();

		if (!pType)
			return 0;

		if ((pTechno->CurrentMission == Mission::SpyplaneApproach || pTechno->CurrentMission == Mission::SpyplaneOverfly)
			&& pTechno->Target && pTechno->WhatAmI() == AbstractType::Aircraft)
		{
			// TechnoClass::GetWeapon is not const-qualified in YRpp.
			auto const pWS = const_cast<TechnoClass*>(pTechno)->GetWeapon(0);
			auto const pWeapon = pWS ? pWS->WeaponType : nullptr;

			if (pWeapon && pWeapon->Damage > 0 && pTechno->DistanceFrom(pTechno->Target) <= pWeapon->Range)
				return std::clamp(pWeapon->Damage, 1, 24);
		}

		return std::clamp(static_cast<int>(pType->Sight), 1, 24);
	}

	// Whether an object's sight reaches pHouse: its own and its allies' objects, and the units of a
	// house whose radar a spy of pHouse entered. Ares SpyEffect.RevealRadar puts the spy's house in
	// the victim's RadarVisibleTo, and every reveal of the victim's objects then reaches it too. Of
	// the victim only units, infantry and aircraft count: they reveal again as they move, while a
	// building revealed once, when it was placed, mostly before the spy went in.
	bool LendsSightTo(const TechnoClass* pTechno, HouseClass* pHouse)
	{
		auto const pOwner = pTechno->Owner;

		if (pOwner == pHouse || pHouse->IsAlliedWith(pOwner))
			return true;

		return (pTechno->AbstractFlags & AbstractFlags::Foot) != AbstractFlags::None && pOwner->RadarVisibleTo.Contains(pHouse->ArrayIndex);
	}

	// The cell a reveal or a gap is centred on: the engine moves the centre one cell north for every
	// two height levels of the coords.
	CellStruct GetRevealCenterCell(const CoordStruct& coords)
	{
		const int lift = coords.Z / Unsorted::CellHeight;

		return { static_cast<short>(coords.X / Unsorted::LeptonsPerCell - lift), static_cast<short>(coords.Y / Unsorted::LeptonsPerCell - lift) };
	}

	// Calls func(first, last) with the first and last vision map index of each row run of the cells
	// up to reach cells from coords on each axis with dx * dx + dy * dy at most radiusSq (at least
	// reach * reach), row by row.
	template <typename Func>
	void ForEachRowSpanWithin(const CellStruct& coords, int reach, int radiusSq, Func&& func)
	{
		for (int dy = -reach; dy <= reach; ++dy)
		{
			const int y = coords.Y + dy;

			if (y < 0 || y >= VisionMapSide)
				continue;

			// Exact integer square root of radiusSq - dy * dy (gaps reach 127 cells).
			const int restSq = radiusSq - dy * dy;
			int span = std::min(reach, static_cast<int>(std::sqrt(static_cast<double>(restSq))));

			while (span * span > restSq)
				--span;

			while (span < reach && (span + 1) * (span + 1) <= restSq)
				++span;

			const int firstX = std::max(0, coords.X - span);
			const int lastX = std::min(VisionMapSide - 1, coords.X + span);

			if (firstX > lastX)
				continue;

			const size_t row = static_cast<size_t>(y) * VisionMapSide;

			func(row + firstX, row + lastX);
		}
	}

	// Calls func(index) for every vision map cell within radius cells of coords (dx * dx + dy * dy
	// at most radius * radius), row by row.
	template <typename Func>
	void ForEachCellInAISight(const CellStruct& coords, int radius, Func&& func)
	{
		ForEachRowSpanWithin(coords, radius, radius * radius, [&func](size_t first, size_t last)
		{
			for (size_t idx = first; idx <= last; ++idx)
				func(idx);
		});
	}

	// Shroud radius in cells of a gap generator working now, 0 otherwise. The engine keeps the gap
	// only while the building is powered, and a deployed (super charged) one uses the larger radius.
	int GetActiveGapRadius(const BuildingClass* pBld)
	{
		if (!pBld || !pBld->Type || !pBld->Type->GapGenerator || pBld->InLimbo || !pBld->IsAlive || !pBld->Owner
			|| !pBld->IsPowerOnline())
		{
			return 0;
		}

		auto const pType = pBld->Type;
		auto const pExt = BuildingTypeExt::Fetch(pType);

		if (pBld->GapSuperCharged)
		{
			const int superRadius = (pExt && pExt->SuperGapRadiusInCells != 0)
				? pExt->SuperGapRadiusInCells.Get()
				: pType->SuperGapRadiusInCells;

			if (superRadius > 0)
				return superRadius;
		}

		return (pExt && pExt->GapRadiusInCells != 0) ? pExt->GapRadiusInCells.Get() : pType->GapRadiusInCells;
	}

	// The cells the working gap generators of the enemies of one house (houses that are not it and
	// not allied with it) shroud again for it, as vision map index runs merged per row. The
	// "MapVisibility - ShroudRegrows" game option makes most base buildings gaps of 127 cells, so
	// the runs are only worked out again when the list of those gaps changes.
	struct NeuralEnemyGapCover
	{
		// Centre X, centre Y and radius of each gap, in BuildingClass::Array order.
		std::vector<int> Gaps {};
		// First and last vision map index of each run.
		std::vector<size_t> Runs {};
	};

	std::array<NeuralEnemyGapCover, 32> EnemyGapCovers {};
	std::vector<int> EnemyGapScratchList;
	std::vector<uint8_t> EnemyGapScratchMap;

	const std::vector<size_t>& GetEnemyGapRuns(HouseClass* pHouse)
	{
		auto& cover = EnemyGapCovers[std::clamp(pHouse->ArrayIndex, 0, 31)];
		auto& gaps = EnemyGapScratchList;
		gaps.clear();

		for (auto const pBld : BuildingClass::Array)
		{
			const int radius = GetActiveGapRadius(pBld);

			if (radius <= 0 || pBld->Owner == pHouse || pBld->Owner->IsAlliedWith(pHouse))
				continue;

			// The engine covers the cells with dx * dx + dy * dy below (radius + 1) squared around the
			// building's location.
			const CellStruct center = GetRevealCenterCell(pBld->Location);
			gaps.push_back(center.X);
			gaps.push_back(center.Y);
			gaps.push_back(radius);
		}

		if (gaps == cover.Gaps)
			return cover.Runs;

		cover.Gaps = gaps;
		cover.Runs.clear();

		if (gaps.empty())
			return cover.Runs;

		auto& covered = EnemyGapScratchMap;
		covered.assign(static_cast<size_t>(VisionMapSide) * VisionMapSide, 0);
		int firstRow = VisionMapSide;
		int lastRow = -1;

		for (size_t i = 0; i + 2 < gaps.size(); i += 3)
		{
			const int radius = gaps[i + 2];
			const CellStruct center { static_cast<short>(gaps[i]), static_cast<short>(gaps[i + 1]) };

			firstRow = std::min(firstRow, std::max(0, gaps[i + 1] - radius));
			lastRow = std::max(lastRow, std::min(VisionMapSide - 1, gaps[i + 1] + radius));

			ForEachRowSpanWithin(center, radius, radius * radius + 2 * radius, [&covered](size_t first, size_t last)
			{
				std::memset(covered.data() + first, 1, last - first + 1);
			});
		}

		for (int y = firstRow; y <= lastRow; ++y)
		{
			const size_t row = static_cast<size_t>(y) * VisionMapSide;

			for (int x = 0; x < VisionMapSide; ++x)
			{
				if (!covered[row + x])
					continue;

				const int first = x;

				while (x + 1 < VisionMapSide && covered[row + x + 1])
					++x;

				cover.Runs.push_back(row + first);
				cover.Runs.push_back(row + x);
			}
		}

		return cover.Runs;
	}

	// The explored map of a house, emptied for a new match (the frame count starts again).
	std::vector<uint8_t>& GetHouseExploredMap(HouseClass* pHouse)
	{
		const size_t totalCells = static_cast<size_t>(VisionMapSide) * VisionMapSide;

		if (LastExploredResetFrame < 0 || Unsorted::CurrentFrame < LastExploredResetFrame || Unsorted::CurrentFrame <= 15)
		{
			for (auto& m : HouseExploredMaps)
				m.assign(totalCells, 0);

			MapShroudStates.fill(NeuralMapShroudState {});
		}

		LastExploredResetFrame = Unsorted::CurrentFrame;

		auto& explored = HouseExploredMaps[std::clamp(pHouse->ArrayIndex, 0, 7)];

		if (explored.size() != totalCells)
			explored.assign(totalCells, 0);

		return explored;
	}

	// A map-wide reshroud since the last rebuild leaves only what is in sight now (marked by the
	// rebuild).
	void ApplyPendingReshroud(HouseClass* pHouse, std::vector<uint8_t>& explored)
	{
		auto& shroudState = MapShroudStates[std::clamp(pHouse->ArrayIndex, 0, 31)];

		if (!shroudState.ReshroudPending)
			return;

		std::memset(explored.data(), 0, explored.size());
		shroudState.ReshroudPending = false;
	}

	// An area reveal of pRevealer: the cells closer than range to the centre of coords, or, with a
	// height, the rectangle of range by height cells around it (both ends included, as Ares walks
	// it), or the whole map when range is below 0. It reaches pRevealer and the houses it is allied
	// with and, for a superweapon reveal, the houses that spied its radar (the engine's reveal
	// checks), and is marked in their explored maps now, on every client. Enemy gaps cover it again
	// at the next rebuild, as the engine makes them again right after the reveal.
	void MarkAreaReveal(HouseClass* pRevealer, const CoordStruct& coords, double range, int height, bool toRadarSpies)
	{
		const CellStruct center = GetRevealCenterCell(coords);
		const int width = static_cast<int>(range);
		const bool wholeMap = range < 0.0;
		const bool rectangle = !wholeMap && height > 0;

		if (!pRevealer || range == 0.0 || (rectangle && width <= 0))
			return;

		const int firstX = center.X - width / 2;
		const int firstY = center.Y - height / 2;
		// A cell closer than range: dx * dx + dy * dy below range * range.
		const int reach = static_cast<int>(std::ceil(range)) - 1;
		const int radiusSq = static_cast<int>(std::ceil(range * range)) - 1;
		uint32_t reached = 0;

		for (auto const pHouse : HouseClass::Array)
		{
			if (pHouse != pRevealer && !pRevealer->IsAlliedWith(pHouse)
				&& !(toRadarSpies && pRevealer->RadarVisibleTo.Contains(pHouse->ArrayIndex)))
			{
				continue;
			}

			auto& explored = GetHouseExploredMap(pHouse);
			ApplyPendingReshroud(pHouse, explored);

			if (wholeMap)
			{
				std::memset(explored.data(), 1, explored.size());
			}
			else if (rectangle)
			{
				for (int y = std::max(0, firstY); y <= std::min(VisionMapSide - 1, firstY + height); ++y)
				{
					const int fromX = std::max(0, firstX);
					const int toX = std::min(VisionMapSide - 1, firstX + width);

					if (fromX <= toX)
						std::memset(explored.data() + static_cast<size_t>(y) * VisionMapSide + fromX, 1, toX - fromX + 1);
				}
			}
			else
			{
				ForEachRowSpanWithin(center, reach, radiusSq, [&explored](size_t first, size_t last)
				{
					std::memset(explored.data() + first, 1, last - first + 1);
				});
			}

			if (pHouse->ArrayIndex >= 0 && pHouse->ArrayIndex < 32)
				reached |= 1u << pHouse->ArrayIndex;
		}

		VisionMapFrame = -1;
		Debug::Log("[NeuralAI] Area reveal of house %d at (%d,%d), range %.1f, height %d, for houses 0x%X at frame %d.\n",
			pRevealer->ArrayIndex, center.X, center.Y, range, height, reached, Unsorted::CurrentFrame);
	}

	void RebuildVisionMap(HouseClass* pHouse)
	{
		const size_t totalCells = static_cast<size_t>(VisionMapSide) * VisionMapSide;
		auto& explored = GetHouseExploredMap(pHouse);

		VisionMapFrame = Unsorted::CurrentFrame;

		VisionMapOwner = pHouse->ArrayIndex;

		CurrentSightMap.assign(totalCells, 0);

		// The map-wide reshrouds and reveals since the last rebuild: a reshroud leaves only what is in
		// sight now (marked below), a reveal the whole map.
		auto& shroudState = MapShroudStates[std::clamp(pHouse->ArrayIndex, 0, 31)];
		ApplyPendingReshroud(pHouse, explored);

		if (pHouse->MapIsClear && !shroudState.Revealed)
		{
			std::memset(explored.data(), 1, explored.size());
			Debug::Log("[NeuralAI] Map revealed for house %d at frame %d.\n", pHouse->ArrayIndex, Unsorted::CurrentFrame);
		}

		shroudState.Revealed = pHouse->MapIsClear;

		uint32_t spiedRadars = 0;

		for (auto const pOther : HouseClass::Array)
		{
			if (pOther != pHouse && pOther->ArrayIndex >= 0 && pOther->ArrayIndex < 32 && !pHouse->IsAlliedWith(pOther)
				&& pOther->RadarVisibleTo.Contains(pHouse->ArrayIndex))
			{
				spiedRadars |= 1u << pOther->ArrayIndex;
			}
		}

		if (spiedRadars != shroudState.SpiedRadars)
		{
			shroudState.SpiedRadars = spiedRadars;
			Debug::Log("[NeuralAI] House %d gets the sight of the units of houses 0x%X (spied radars) at frame %d.\n",
				pHouse->ArrayIndex, spiedRadars, Unsorted::CurrentFrame);
		}

		// Under an enemy gap the explored cells are shrouded again (the engine makes every gap again
		// right after a reveal); the sight below reveals the ones in sight now.
		auto const& gapRuns = GetEnemyGapRuns(pHouse);

		for (size_t i = 0; i + 1 < gapRuns.size(); i += 2)
			std::memset(explored.data() + gapRuns[i], 0, gapRuns[i + 1] - gapRuns[i] + 1);

		for (auto const pTechno : TechnoClass::Array)
		{
			if (!pTechno || !pTechno->Owner || !LendsSightTo(pTechno, pHouse))
				continue;

			const int radius = GetAISightRadius(pTechno);

			if (radius <= 0)
				continue;

			ForEachCellInAISight(pTechno->GetMapCoords(), radius, [&](size_t idx)
			{
				explored[idx] = 1;
				CurrentSightMap[idx] = 1;
			});
		}

		VisionMap = explored;
	}

	// Whether the sight of pHouse or its allies covers a cell right now: for the questions that need
	// our own eyes there (a scout that arrived), not for what the house knows (IsCellKnownToAI).
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

	// Whether pHouse has a cell explored: revealed by its or its allies' sight at some time and not
	// shrouded again by an enemy gap generator since.
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

	// The vision rule for one cell: what stands on it is known while it is in sight now and, without
	// fog of war, while it stays explored and no working enemy gap generator covers it.
	bool IsKnownByVisionRule(bool inSightNow, bool explored, bool underEnemyGap)
	{
		return inSightNow || (explored && !underEnemyGap && !IsFogOfWarMatch());
	}

	// Whether pHouse knows what stands on a cell now (enemy units, new buildings, garrisons, a
	// destroyed bridge), by the match's vision rule. RebuildVisionMap already keeps the cells under
	// an enemy gap explored only while in sight.
	bool IsCellKnownToAI(HouseClass* pHouse, const CellStruct& coords)
	{
		if (!pHouse)
			return false;

		if (VisionMapFrame != Unsorted::CurrentFrame || VisionMapOwner != pHouse->ArrayIndex)
			RebuildVisionMap(pHouse);

		if (coords.X < 0 || coords.X >= VisionMapSide || coords.Y < 0 || coords.Y >= VisionMapSide)
			return false;

		const size_t idx = static_cast<size_t>(coords.Y) * VisionMapSide + coords.X;

		return IsKnownByVisionRule(CurrentSightMap[idx] != 0, VisionMap[idx] != 0, false);
	}

	// The unit is boarding pTarget: an Enter mission (current or queued) bound to it by radio
	// contact, archive target or destination.
	bool IsFootBoarding(const FootClass* pFoot, const TechnoClass* pTarget)
	{
		return (pFoot->CurrentMission == Mission::Enter || pFoot->QueuedMission == Mission::Enter)
			&& (pFoot->ContainsLink(pTarget) || pFoot->ArchiveTarget == pTarget || pFoot->Destination == pTarget);
	}

	// The living unit, soldier or aircraft of this house with this UniqueID (in limbo too), or null.
	FootClass* FindOwnFootByID(HouseClass* pHouse, DWORD id)
	{
		if (!id)
			return nullptr;

		for (auto const pFoot : FootClass::Array)
		{
			if (pFoot && pFoot->UniqueID == id)
				return (pFoot->Owner == pHouse && pFoot->IsAlive) ? pFoot : nullptr;
		}

		return nullptr;
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

	// What the weapons of a type do like an engineer, from their warheads: capture a building
	// (FakeEngineer.CanCaptureBuildings), repair or destroy a bridge from its hut (CanRepairBridges,
	// CanDestroyBridges) and disarm an attached bomb (BombDisarm), without losing the firer. Any type:
	// infantry, vehicle, aircraft. Every weapon slot it has, normal and elite (the one ForceWeapon.Capture
	// picks included). Read from the rules once per type and match.
	constexpr int FakeEngineerCaptures = 1;
	constexpr int FakeEngineerRepairsBridges = 2;
	constexpr int FakeEngineerDestroysBridges = 4;
	constexpr int FakeEngineerDisarmsBombs = 8;

	int FakeEngineerAbilities(const TechnoTypeClass* pType)
	{
		if (!pType)
			return 0;

		static int cacheSerial = -1;
		// Key access only.
		static std::unordered_map<const TechnoTypeClass*, int> cache;

		if (cacheSerial != NeuralMatchSerial)
		{
			cacheSerial = NeuralMatchSerial;
			cache.clear();
		}

		if (auto const it = cache.find(pType); it != cache.end())
			return it->second;

		int abilities = 0;
		const int weapons = std::clamp(std::max(2, pType->WeaponCount), 0, TechnoTypeClass::MaxWeapons);

		for (int i = 0; i < weapons; ++i)
		{
			for (const bool elite : { false, true })
			{
				auto const pWeapon = pType->GetWeapon(static_cast<size_t>(i), elite).WeaponType;
				auto const pWHExt = (pWeapon && pWeapon->Warhead) ? WarheadTypeExt::TryFetch(pWeapon->Warhead) : nullptr;

				if (!pWHExt)
					continue;

				abilities |= (pWHExt->FakeEngineer_CanCaptureBuildings ? FakeEngineerCaptures : 0)
					| (pWHExt->FakeEngineer_CanRepairBridges ? FakeEngineerRepairsBridges : 0)
					| (pWHExt->FakeEngineer_CanDestroyBridges ? FakeEngineerDestroysBridges : 0)
					| (pWHExt->FakeEngineer_BombDisarm ? FakeEngineerDisarmsBombs : 0);
			}
		}

		cache.emplace(pType, abilities);

		return abilities;
	}

	// May this unit capture that building by its weapon? What the warhead captures (Capturable or
	// NeedsEngineer, owned by an enemy or neutral), as feature/EngineerLogicsOnWarheads applies it.
	bool CanCaptureByWeapon(const TechnoClass* pTechno, const BuildingClass* pBld)
	{
		if (!pTechno || !pTechno->Owner || !pBld || !pBld->IsAlive || !pBld->Type || !pBld->Owner || pBld->Owner == pTechno->Owner
			|| !(FakeEngineerAbilities(pTechno->GetTechnoType()) & FakeEngineerCaptures))
		{
			return false;
		}

		return (pBld->Type->Capturable || pBld->Type->NeedsEngineer)
			&& (pBld->Owner->IsNeutral() || !pTechno->Owner->IsAlliedWith(pBld->Owner));
	}

	// A soldier that captures buildings: an engineer, or one whose weapon captures like an engineer
	// (FakeEngineerAbilities); that one captures with an attack order from its weapon's range instead of
	// walking in.
	bool IsCaptureInfantryType(const InfantryTypeClass* pType)
	{
		return pType && (pType->Engineer || (FakeEngineerAbilities(pType) & FakeEngineerCaptures) != 0);
	}

	// An engineer by its weapon only (IsCaptureInfantryType without Engineer=yes).
	bool CapturesByWeapon(const InfantryTypeClass* pType)
	{
		return pType && !pType->Engineer && IsCaptureInfantryType(pType);
	}

	// A specialist that works a building or a vehicle instead of fighting: engineer (by its weapon
	// too), spy (which also robs refineries and silos), vehicle thief.
	bool IsSpecialistInfantryType(const InfantryTypeClass* pType)
	{
		return pType && (IsCaptureInfantryType(pType) || IsSpyInfantryType(pType) || pType->VehicleThief);
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

	// Dynamic custom-armor ([ArmorTypes] in Ares / Antares) and shield ([ShieldTypes] in Phobos)
	// catalog built once per match. Resolves total armor count, transitive base-armor inheritance
	// (e.g. mm2mec_armor -> super_heavy_armor -> heavy) and which unit/building/shield classes field
	// each armor index, with zero hot-path allocations.
	constexpr uint8_t ArmorDomainInfantry = 1;
	constexpr uint8_t ArmorDomainVehicle = 2;
	constexpr uint8_t ArmorDomainBuilding = 4;
	constexpr uint8_t ArmorDomainShield = 8;

	struct NeuralAresVersesEntry
	{
		double Verses;
		WarheadFlags Flags;
	};

	struct NeuralAresWarheadExtLayout
	{
		char Padding[0x24];
		std::vector<NeuralAresVersesEntry> Verses;
	};

	struct NeuralArmorCatalog
	{
		int MatchSerial { -1 };
		int ArmorCount { 11 };
		std::vector<int> RootBaseArmor;
		std::vector<uint8_t> DomainMask;
	};

	const NeuralArmorCatalog& GetNeuralArmorCatalog()
	{
		static NeuralArmorCatalog catalog {};

		if (catalog.MatchSerial == NeuralMatchSerial && catalog.ArmorCount >= 11)
			return catalog;

		catalog.MatchSerial = NeuralMatchSerial;
		int armorCount = 11;

		if (AresHelper::CanUseAres && WarheadTypeClass::Array.Count > 0)
		{
			if (auto const pWH = WarheadTypeClass::Array.GetItem(0))
			{
				if (auto const pExt = reinterpret_cast<const NeuralAresWarheadExtLayout*>(*reinterpret_cast<const uintptr_t*>(reinterpret_cast<const char*>(pWH) + 0x1CC)))
					armorCount = std::max(armorCount, static_cast<int>(pExt->Verses.size()));
			}
		}

		// Parse [ArmorTypes] names and parent links in exact registration order (works identically
		// under Ares and Antares, even if AresHelper::CanUseAres is false on a custom Antares build).
		static constexpr const char* DefaultArmorNames[11] =
		{
			"none", "flak", "plate", "light", "medium", "heavy",
			"wood", "steel", "concrete", "special_1", "special_2"
		};

		std::vector<std::string> names;
		names.reserve(64);

		for (const char* pDef : DefaultArmorNames)
			names.emplace_back(pDef);

		auto findArmorNameIndex = [&](const char* pName) -> int
		{
			if (!pName || pName[0] == '\0')
				return -1;

			for (size_t i = 0; i < names.size(); ++i)
			{
				if (_stricmp(names[i].c_str(), pName) == 0)
					return static_cast<int>(i);
			}

			return -1;
		};

		if (auto const pINI = CCINIClass::INI_Rules)
		{
			const int keyCount = pINI->GetKeyCount("ArmorTypes");

			for (int i = 0; i < keyCount; ++i)
			{
				const char* pKey = pINI->GetKeyName("ArmorTypes", i);

				if (pKey && pKey[0] != '\0' && findArmorNameIndex(pKey) < 0)
					names.emplace_back(pKey);
			}
		}

		armorCount = std::max(armorCount, static_cast<int>(names.size()));

		auto bumpCount = [&](Armor armor)
		{
			const int idx = static_cast<int>(armor);

			if (idx >= armorCount && idx < 4096)
				armorCount = idx + 1;
		};

		for (auto const pInf : InfantryTypeClass::Array)
		{
			if (pInf)
				bumpCount(pInf->Armor);
		}

		for (auto const pUnit : UnitTypeClass::Array)
		{
			if (pUnit)
				bumpCount(pUnit->Armor);
		}

		for (auto const pAir : AircraftTypeClass::Array)
		{
			if (pAir)
				bumpCount(pAir->Armor);
		}

		for (auto const pBld : BuildingTypeClass::Array)
		{
			if (pBld)
				bumpCount(pBld->Armor);
		}

		for (auto const& pShield : ShieldTypeClass::Array)
		{
			if (pShield)
				bumpCount(pShield->Armor.Get());
		}

		for (auto const pOv : OverlayTypeClass::Array)
		{
			if (pOv)
				bumpCount(pOv->Armor);
		}

		std::vector<int> parentIndex(armorCount, -1);

		for (int a = 0; a < 11 && a < armorCount; ++a)
			parentIndex[a] = a;

		if (auto const pINI = CCINIClass::INI_Rules)
		{
			char valueBuf[0x40] {};

			for (size_t i = 11; i < names.size() && static_cast<int>(i) < armorCount; ++i)
			{
				if (pINI->ReadString("ArmorTypes", names[i].c_str(), "", valueBuf, sizeof(valueBuf)) > 0)
					parentIndex[i] = findArmorNameIndex(valueBuf);
			}
		}

		catalog.ArmorCount = armorCount;
		catalog.RootBaseArmor.assign(armorCount, -1);
		catalog.DomainMask.assign(armorCount, 0);

		for (int a = 0; a < armorCount; ++a)
		{
			int cur = a;

			for (int hop = 0; hop < 16 && cur >= 11 && cur < armorCount; ++hop)
				cur = parentIndex[cur];

			catalog.RootBaseArmor[a] = (cur >= 0 && cur < 11) ? cur : -1;

			const int root = catalog.RootBaseArmor[a];

			if (root >= 0 && root <= 2)
				catalog.DomainMask[a] |= ArmorDomainInfantry;
			else if (root >= 3 && root <= 5)
				catalog.DomainMask[a] |= ArmorDomainVehicle;
			else if (root >= 6 && root <= 8)
				catalog.DomainMask[a] |= ArmorDomainBuilding;
		}

		for (auto const pInf : InfantryTypeClass::Array)
		{
			if (pInf && static_cast<int>(pInf->Armor) >= 0 && static_cast<int>(pInf->Armor) < armorCount)
				catalog.DomainMask[static_cast<int>(pInf->Armor)] |= ArmorDomainInfantry;
		}

		for (auto const pUnit : UnitTypeClass::Array)
		{
			if (pUnit && static_cast<int>(pUnit->Armor) >= 0 && static_cast<int>(pUnit->Armor) < armorCount)
				catalog.DomainMask[static_cast<int>(pUnit->Armor)] |= ArmorDomainVehicle;
		}

		for (auto const pAir : AircraftTypeClass::Array)
		{
			if (pAir && static_cast<int>(pAir->Armor) >= 0 && static_cast<int>(pAir->Armor) < armorCount)
				catalog.DomainMask[static_cast<int>(pAir->Armor)] |= ArmorDomainVehicle;
		}

		for (auto const pBld : BuildingTypeClass::Array)
		{
			if (pBld && static_cast<int>(pBld->Armor) >= 0 && static_cast<int>(pBld->Armor) < armorCount)
				catalog.DomainMask[static_cast<int>(pBld->Armor)] |= ArmorDomainBuilding;
		}

		for (auto const& pShield : ShieldTypeClass::Array)
		{
			if (pShield && pShield->Strength.Get() > 0)
			{
				const int sIdx = static_cast<int>(static_cast<Armor>(pShield->Armor.Get()));

				if (sIdx >= 0 && sIdx < armorCount)
					catalog.DomainMask[sIdx] |= ArmorDomainShield;
			}
		}

		return catalog;
	}

	inline int GetRegisteredArmorCount()
	{
		return GetNeuralArmorCatalog().ArmorCount;
	}

	inline int GetRootBaseArmor(Armor armor)
	{
		auto const& cat = GetNeuralArmorCatalog();
		const int idx = static_cast<int>(armor);

		return (idx >= 0 && idx < cat.ArmorCount) ? cat.RootBaseArmor[idx] : -1;
	}

	inline bool IsHeavyLikeArmor(Armor armor)
	{
		return armor == Armor::Heavy || GetRootBaseArmor(armor) == static_cast<int>(Armor::Heavy);
	}

	inline uint8_t GetArmorDomainMask(int armorIndex)
	{
		auto const& cat = GetNeuralArmorCatalog();

		return (armorIndex >= 0 && armorIndex < cat.ArmorCount) ? cat.DomainMask[armorIndex] : 0;
	}

	inline WarheadFlags GetWarheadVersusFlags(WarheadTypeClass* pWH, Armor armor)
	{
		if (!pWH)
			return WarheadFlags { false, false, false };

		const int idx = static_cast<int>(armor);

		if (idx < 0)
			return WarheadFlags { false, false, false };

		if (AresHelper::CanUseAres)
		{
			if (auto const pExt = reinterpret_cast<const NeuralAresWarheadExtLayout*>(*reinterpret_cast<const uintptr_t*>(reinterpret_cast<const char*>(pWH) + 0x1CC)))
			{
				if (static_cast<size_t>(idx) < pExt->Verses.size())
					return pExt->Verses[idx].Flags;
			}
		}

		const double v = GeneralUtils::GetWarheadVersusArmor(pWH, armor);
		const bool negHeal = v < -0.0001;
		const bool forceFire = negHeal || v > 0.0001;
		const bool retaliate = negHeal || v > 0.0101;
		const bool passiveAcquire = negHeal || v > 0.0201;

		return WarheadFlags { forceFire, retaliate, passiveAcquire };
	}

	inline bool CanWarheadDamageEnemy(WarheadTypeClass* pWH)
	{
		if (!pWH)
			return false;

		if (auto const pWHExt = WarheadTypeExt::TryFetch(pWH))
		{
			if (!pWHExt->AffectsEnemies.Get())
				return false;

			if (pWHExt->FakeEngineer_CanCaptureBuildings)
				return false;
		}

		return true;
	}

	inline ShieldTypeClass* GetActiveTypeShield(const TechnoTypeClass* pType, Armor& outShieldArmor, int& outShieldStrength)
	{
		outShieldArmor = pType ? pType->Armor : Armor::None;
		outShieldStrength = 0;

		if (!pType)
			return nullptr;

		auto const pMutType = const_cast<TechnoTypeClass*>(pType);
		auto const pTypeExt = TechnoTypeExt::TryFetch(pMutType);
		auto const pShieldType = pTypeExt ? pTypeExt->ShieldType.Get() : nullptr;

		if (!pShieldType || pShieldType->Strength.Get() <= 0)
			return nullptr;

		outShieldStrength = pShieldType->Strength.Get();

		if (pShieldType->InheritArmorFromTechno
			&& (pShieldType->InheritArmor_Allowed.empty() || pShieldType->InheritArmor_Allowed.Contains(pMutType))
			&& !pShieldType->InheritArmor_Disallowed.Contains(pMutType))
		{
			outShieldArmor = pType->Armor;
		}
		else
		{
			outShieldArmor = pShieldType->Armor.Get();
		}

		return pShieldType;
	}

	inline bool WarheadPenetratesShieldType(WarheadTypeClass* pWH, ShieldTypeClass* pShieldType)
	{
		if (!pWH || !pShieldType)
			return true;

		if (pWH->Temporal)
			return true;

		auto const pWHExt = WarheadTypeExt::TryFetch(pWH);

		if (!pWHExt || !pWHExt->Shield_Penetrate.Get())
			return false;

		auto const affected = pWHExt->Shield_Penetrate_Types.GetElements(pWHExt->Shield_AffectTypes);

		return affected.empty() || affected.contains(pShieldType);
	}

	inline bool WarheadBreaksShieldType(WarheadTypeClass* pWH, ShieldTypeClass* pShieldType)
	{
		if (!pWH || !pShieldType)
			return false;

		auto const pWHExt = WarheadTypeExt::TryFetch(pWH);

		if (!pWHExt || !pWHExt->Shield_Break.Get())
			return false;

		auto const affected = pWHExt->Shield_Break_Types.GetElements(pWHExt->Shield_AffectTypes);

		return affected.empty() || affected.contains(pShieldType);
	}

	// Deterministic integer permille effectiveness of pWH against pTargetType, accounting for custom
	// [ArmorTypes], Ares ForceFire/PassiveAcquire flags, and Phobos [ShieldTypes] absorption/penetration/break.
	int GetEffectiveVersusAgainstTypePermille(WarheadTypeClass* pWH, const TechnoTypeClass* pTargetType, bool* outBreaksShield = nullptr)
	{
		if (outBreaksShield)
			*outBreaksShield = false;

		if (!pWH || !pTargetType || !CanWarheadDamageEnemy(pWH))
			return 0;

		const Armor hullArmor = pTargetType->Armor;
		const double rawHull = GeneralUtils::GetWarheadVersusArmor(pWH, hullArmor);
		const WarheadFlags hullFlags = GetWarheadVersusFlags(pWH, hullArmor);
		int hullPermille = (rawHull > 0.0 && (hullFlags.ForceFire || rawHull > 0.0001))
			? std::clamp(static_cast<int>(rawHull * 1000.0 + 0.5), 0, 3000)
			: 0;

		if (hullPermille > 0 && !hullFlags.PassiveAcquire)
			hullPermille = hullPermille * 55 / 100;

		Armor shieldArmor = hullArmor;
		int shieldStrength = 0;
		ShieldTypeClass* pShieldType = GetActiveTypeShield(pTargetType, shieldArmor, shieldStrength);

		if (!pShieldType || shieldStrength <= 0)
			return hullPermille;

		const bool breaksShield = WarheadBreaksShieldType(pWH, pShieldType);

		if (outBreaksShield)
			*outBreaksShield = breaksShield;

		if (WarheadPenetratesShieldType(pWH, pShieldType))
			return breaksShield ? std::min(3000, hullPermille * 135 / 100 + 250) : hullPermille;

		const double rawShield = GeneralUtils::GetWarheadVersusArmor(pWH, shieldArmor);
		const WarheadFlags shieldFlags = GetWarheadVersusFlags(pWH, shieldArmor);
		int shieldPermille = (rawShield > 0.0 && (shieldFlags.ForceFire || rawShield > 0.0001))
			? std::clamp(static_cast<int>(rawShield * 1000.0 + 0.5), 0, 3000)
			: 0;

		if (shieldPermille > 0 && !shieldFlags.PassiveAcquire)
			shieldPermille = shieldPermille * 55 / 100;

		auto const pWHExt = WarheadTypeExt::TryFetch(pWH);
		const bool affectsShield = !pWHExt || pWHExt->Shield_AffectTypes.empty() || pWHExt->Shield_AffectTypes.Contains(pShieldType);
		const double absorbD = affectsShield && pWHExt
			? pWHExt->Shield_AbsorbPercent.Get(pShieldType->AbsorbPercent)
			: pShieldType->AbsorbPercent.Get();
		const double passD = affectsShield && pWHExt
			? pWHExt->Shield_PassPercent.Get(pShieldType->PassPercent)
			: pShieldType->PassPercent.Get();
		const int absorbPct = std::clamp(static_cast<int>(absorbD * 100.0 + 0.5), 0, 200);
		const int passPct = std::clamp(static_cast<int>(passD * 100.0 + 0.5), 0, 200);

		if (breaksShield && shieldPermille > 0)
			return std::min(3000, std::max(hullPermille, shieldPermille) * 135 / 100 + 250);

		// A non-penetrating warhead with 0% vs shield_armor and 0% pass-through cannot even target or
		// scratch an intact shield (ShieldClass::CanBeTargeted == false).
		if (shieldPermille <= 0 && passPct <= 0)
			return hullPermille / 5;

		const int hullHP = std::max(1, pTargetType->Strength);
		const int totalHP = hullHP + shieldStrength;
		const int shieldEff = shieldPermille * absorbPct / 100 + hullPermille * passPct / 100;
		const long long blended = (static_cast<long long>(hullPermille) * hullHP
			+ static_cast<long long>(shieldEff) * shieldStrength) / totalHP;

		return std::clamp(static_cast<int>(blended), 0, 3000);
	}

	// What a weapon does to registered armors (Repairer*), including custom [ArmorTypes] for vehicles
	// and buildings, by its effective damage, Damage x Verses: it repairs vehicles or buildings when
	// that is negative and its warhead affects allies, and it hurts enemies when it is positive for
	// one armor and the warhead affects enemies. NeverUse weapons never fire and do nothing.
	constexpr int RepairsVehicles = 1;
	constexpr int RepairsBuildings = 2;
	constexpr int HurtsEnemies = 4;

	int WeaponRepairAbilities(const WeaponTypeClass* pWeapon)
	{
		if (!pWeapon || pWeapon->NeverUse || !pWeapon->Warhead || pWeapon->Damage == 0)
			return 0;

		auto const pWH = pWeapon->Warhead;
		auto const pWHExt = WarheadTypeExt::TryFetch(pWH);
		const bool affectsEnemies = !pWHExt || pWHExt->AffectsEnemies.Get();
		const int armorCount = GetRegisteredArmorCount();
		int abilities = 0;

		for (int armor = 0; armor < armorCount; ++armor)
		{
			const double effect = pWeapon->Damage * GeneralUtils::GetWarheadVersusArmor(pWH, static_cast<Armor>(armor));

			if (effect < 0.0 && pWH->AffectsAllies)
			{
				const uint8_t domain = GetArmorDomainMask(armor);

				if (domain & ArmorDomainBuilding)
					abilities |= RepairsBuildings;

				if (domain & ArmorDomainVehicle)
					abilities |= RepairsVehicles;
			}
			else if (effect > 0.0 && affectsEnemies)
			{
				abilities |= HurtsEnemies;
			}
		}

		return abilities;
	}

	// The same for every weapon slot of a type, normal and elite (WeaponCount: MultiWeapon, IFV modes),
	// once per type and match.
	int RepairAbilitiesOf(const TechnoTypeClass* pType)
	{
		if (!pType)
			return 0;

		static int cacheSerial = -1;
		// Key access only.
		static std::unordered_map<const TechnoTypeClass*, int> cache;

		if (cacheSerial != NeuralMatchSerial)
		{
			cacheSerial = NeuralMatchSerial;
			cache.clear();
		}

		if (auto const it = cache.find(pType); it != cache.end())
			return it->second;

		int abilities = 0;
		const int weapons = std::clamp(std::max(2, pType->WeaponCount), 0, TechnoTypeClass::MaxWeapons);

		for (int i = 0; i < weapons; ++i)
		{
			for (const bool elite : { false, true })
				abilities |= WeaponRepairAbilities(pType->GetWeapon(static_cast<size_t>(i), elite).WeaponType);
		}

		cache.emplace(pType, abilities);

		return abilities;
	}

	// A vehicle type built as a repairer (Repairer*): it repairs and hurts no enemy (TSREPAIR, MDISK);
	// no IFV, whose weapon comes from its passenger.
	bool IsRepairerUnitType(UnitTypeClass* pType)
	{
		if (!pType || pType->Gunner)
			return false;

		const int abilities = RepairAbilitiesOf(pType);

		return (abilities & (RepairsVehicles | RepairsBuildings)) != 0 && (abilities & HurtsEnemies) == 0;
	}

	// A vehicle that only repairs now (Repairer*): a repairer type, or an IFV whose current weapon (its
	// passenger's, TechnoExt::GetCurrentWeapon) repairs and hurts no enemy (an FV with an engineer).
	bool IsRepairerUnit(UnitClass* pUnit)
	{
		if (!pUnit || !pUnit->Type)
			return false;

		if (!pUnit->Type->Gunner)
			return IsRepairerUnitType(pUnit->Type);

		const int abilities = WeaponRepairAbilities(TechnoExt::GetCurrentWeapon(pUnit, pUnit->Type));

		return (abilities & (RepairsVehicles | RepairsBuildings)) != 0 && (abilities & HurtsEnemies) == 0;
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

	// May the type fire the weapon of this slot at the ground, by its LandTargeting? Land_Not_OK
	// bars both slots, Land_Secondary bars the primary. A projectile that hits the ground does not
	// settle it: the torpedo of the rules has AG=yes, so torpedo submarines counted as ships that
	// shell the coast and were sent at buildings they cannot fire at, where they stood packed on
	// an Attack mission with no target (match #9872, Mayflower).
	bool LandTargetingAllows(const TechnoTypeClass* pType, int slot)
	{
		return pType && pType->LandTargeting != LandTargetingType::Land_Not_OK
			&& !(pType->LandTargeting == LandTargetingType::Land_Secondary && slot == 0);
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

	// Damage and healing of resource cells (Ares tags the engine types do not carry): whether each is
	// on, and what a unit takes for entering a cell of each resource type that hurts.
	struct NeuralResourceCellRules
	{
		int MatchSerial { -1 };
		bool HealEnabled { false };
		std::vector<std::pair<int, WarheadTypeClass*>> Damages;
		std::map<const TechnoTypeClass*, int> DamageByType;
	};

	NeuralResourceCellRules& GetResourceCellRules()
	{
		static NeuralResourceCellRules rules;

		if (rules.MatchSerial == NeuralMatchSerial)
			return rules;

		rules = NeuralResourceCellRules {};
		rules.MatchSerial = NeuralMatchSerial;
		auto const pINI = CCINIClass::INI_Rules;

		if (!pINI)
			return rules;

		rules.HealEnabled = pINI->ReadBool("General", "TiberiumHealEnabled", false);

		if (!pINI->ReadBool("General", "TiberiumDamageEnabled", false))
			return rules;

		for (auto const pTiberium : TiberiumClass::Array)
		{
			if (!pTiberium || !pTiberium->ID || pTiberium->ID[0] == '\0')
				continue;

			const int damage = pINI->ReadInteger(pTiberium->ID, "Damage", std::max(1, pTiberium->Power / 10));
			char warheadID[0x40] = "";
			pINI->ReadString(pTiberium->ID, "Warhead", "", warheadID, sizeof(warheadID));
			auto pWarhead = warheadID[0] != '\0' ? WarheadTypeClass::Find(warheadID) : nullptr;

			if (!pWarhead)
				pWarhead = RulesClass::Instance->C4Warhead;

			if (damage > 0 && pWarhead)
				rules.Damages.push_back({ damage, pWarhead });
		}

		return rules;
	}

	// What a unit of this type loses for each resource cell it enters: 0 when they do not hurt it
	// (soldiers are hurt unless marked proof, everything else is proof unless marked otherwise).
	int ResourceCellDamage(const TechnoTypeClass* pType)
	{
		if (!pType || !pType->ID || pType->ID[0] == '\0')
			return 0;

		auto& rules = GetResourceCellRules();

		if (rules.Damages.empty())
			return 0;

		if (auto const it = rules.DamageByType.find(pType); it != rules.DamageByType.end())
			return it->second;

		int worst = 0;
		const bool proofByDefault = pType->WhatAmI() != AbstractType::InfantryType;

		if (!CCINIClass::INI_Rules->ReadBool(pType->ID, "TiberiumProof", proofByDefault))
		{
			Armor shieldArmor = pType->Armor;
			int shieldStrength = 0;
			ShieldTypeClass* pShieldType = GetActiveTypeShield(pType, shieldArmor, shieldStrength);

			for (auto const& [damage, pWarhead] : rules.Damages)
			{
				const Armor effArmor = (pShieldType && shieldStrength > 0 && !WarheadPenetratesShieldType(pWarhead, pShieldType))
					? shieldArmor
					: pType->Armor;
				worst = std::max(worst, MapClass::GetTotalDamage(damage, pWarhead, effArmor, 0));
			}
		}

		rules.DamageByType[pType] = worst;
		return worst;
	}

	bool HealsOnResourceCells(const TechnoTypeClass* pType)
	{
		return pType && pType->TiberiumHeal && GetResourceCellRules().HealEnabled;
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

		// A carryall that does not mix troops and cargo lets nobody in while it slings a vehicle or
		// is on a job for one (the engine refuses the entry): it is no transport until it is done.
		if (pTransport->WhatAmI() == AbstractType::Unit)
		{
			auto const pCarryState = UnitExt::Fetch(const_cast<UnitClass*>(static_cast<const UnitClass*>(pTransport)));

			if (pCarryState && pCarryState->CarryallBlocksPassengers())
				return false;
		}

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
		// SW.Range as Ares reads it for the area of a superweapon: a radius (RangeHeight 0) or the
		// width of a rectangle RangeHeight cells high; both -1 when not set.
		float RangeWidthOrRadius { -1.0f };
		int RangeHeight { -1 };
		// For an Ares SpyPlane type: its planes fly a spy plane run (SpyPlane.Mission not set, Ares
		// default SpyplaneApproach, or a Spyplane mission), not an air strike (e.g. Attack).
		bool SpyPlaneLooks { true };
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

			char rangeText[64] {};

			if (pINI->ReadString(pSWType->ID, "SW.Range", "", rangeText, sizeof(rangeText)) > 0)
			{
				char* context = nullptr;
				char* pPart = strtok_s(rangeText, ",", &context);

				if (pPart && *pPart)
				{
					it->second.RangeWidthOrRadius = static_cast<float>(std::atof(pPart));
					it->second.RangeHeight = 0;
					pPart = strtok_s(nullptr, ",", &context);

					if (pPart && *pPart)
						it->second.RangeHeight = std::atoi(pPart);
				}
			}

			char typeName[64] {};
			pINI->ReadString(pSWType->ID, "Type", "", typeName, sizeof(typeName));
			it->second.IsEMPulse = _stricmp(typeName, "EMPulse") == 0;

			char spyMission[64] {};
			pINI->ReadString(pSWType->ID, "SpyPlane.Mission", "", spyMission, sizeof(spyMission));
			it->second.SpyPlaneLooks = !spyMission[0] || _strnicmp(spyMission, "Spyplane", 8) == 0;

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

	// The area an Ares PsychicReveal superweapon reveals: SW.Range (a radius, height 0, or the width of a
	// rectangle height cells high; below 0 the whole map), else the rules' PsychicRevealRadius up to 10
	// cells (height -1).
	double GetPsychicRevealRange(const SuperWeaponTypeClass* pSWType, int& height)
	{
		auto const& aim = GetSWTargeting(pSWType);
		const bool rangeSet = aim.RangeWidthOrRadius >= 0.0f || aim.RangeHeight >= 0;
		height = rangeSet ? aim.RangeHeight : -1;

		return rangeSet ? aim.RangeWidthOrRadius : std::min(RulesClass::Instance->PsychicRevealRadius, 10);
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

	// A research super weapon the Neural AI fires itself: it delivers or kills limbo buildings or
	// converts units, and is no AutoFire (the engine fires those), AI-forbidden or tunnel delivery one.
	bool IsResearchSuperWeapon(SuperWeaponTypeClass* pSWType, HouseClass* pOwner)
	{
		auto const pSWExt = pSWType ? SWTypeExt::Fetch(pSWType) : nullptr;

		return pSWExt && (!pSWExt->LimboDelivery_Types.empty() || !pSWExt->LimboKill_IDs.empty() || !pSWExt->Convert_Pairs.empty())
			&& !pSWType->DisableableFromShell && pSWType->Type != SuperWeaponType::ChronoWarp && pSWExt->SW_AllowAI
			&& !pSWExt->SW_AutoFire && !IsTunnelDeliverySuperWeapon(pSWType, pOwner);
	}

	// Research still to buy that a type pays for: for each resource it produces, the research super
	// weapons of the house that cost that resource.
	int ResearchPaidByProducer(HouseClass* pHouse, TechnoTypeClass* pType)
	{
		auto const pTypeExt = pType ? TechnoTypeExt::Fetch(pType) : nullptr;

		if (!pHouse || !pTypeExt || pTypeExt->ResourceProductions.empty())
			return 0;

		int count = 0;

		for (auto const& production : pTypeExt->ResourceProductions)
		{
			if (production.Amount <= 0 || production.ResourceIndex < 0)
				continue;

			const size_t resource = static_cast<size_t>(production.ResourceIndex);

			for (auto const pSuper : pHouse->Supers)
			{
				if (!pSuper || !pSuper->IsPresent || !IsResearchSuperWeapon(pSuper->Type, pHouse))
					continue;

				auto const& amounts = SWTypeExt::Fetch(pSuper->Type)->SW_ResourceAmounts;

				if (resource < amounts.size() && amounts[resource] < 0)
					count++;
			}
		}

		return count;
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
		// As in the AdvAI fork: a blackout or an EMP switches buildings off whatever the surplus is,
		// so a building off for one of those is no sign of a shortage.
		const bool blackout = pHouse->PowerBlackoutTimer.HasTimeLeft();

		for (auto const pBld : pHouse->Buildings)
		{
			if (!pBld || !pBld->IsAlive || pBld->InLimbo || !pBld->Type)
				continue;

			auto const pOT = pBld->Type;

			if (pOT->TogglePower && !pBld->StuffEnabled)
			{
				drain += pOT->PowerDrain;

				if (!blackout && !pBld->IsUnderEMP())
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

// NeuralAI.State.inl: Per-house macro state, ferry / air records, match style, attack sectors and known enemy buildings.
// Part of NeuralAI.cpp (included in order; one translation unit).

namespace
{
	enum class NeuralThreatCategory : int
	{
		None = 0,
		Infantry = 1,
		Vehicle = 2,
		Air = 3
	};

	struct NeuralAttackedBuildingRecord
	{
		CellStruct BuildingCoords { CellStruct::Empty };
		CellStruct AttackerCoords { CellStruct::Empty };

		TechnoTypeClass* AttackerType { nullptr };

		NeuralThreatCategory Threat { NeuralThreatCategory::None };

		int AttackFrame { 0 };
	};

	struct NeuralUnsafeZone
	{
		CellStruct Coords { CellStruct::Empty };
		int ExpirationFrame { 0 };
	};

	// Last sightings of an enemy airborne unit, used to estimate its heading and
	// time of arrival over our base after it vanishes into the fog.
	struct NeuralAirThreatTrack
	{
		DWORD UniqueID { 0 };
		CellStruct LastCoords { CellStruct::Empty };
		CellStruct PrevCoords { CellStruct::Empty };
		int LastSeenFrame { -1 };
		int PrevSeenFrame { -1 };
		int ThreatValue { 0 };
		bool IsHeavy { false };
	};

	// Opponent model: what this house has actually seen of its enemies (fog-fair),
	// remembered for a few minutes so the AI adapts its army to the enemy's style.
	enum class EnemyForceCategory : uint8_t
	{
		Infantry,
		Vehicle,
		Artillery,
		Naval,
		Air,
		Count
	};

	struct EnemyUnitSighting
	{
		EnemyForceCategory Category { EnemyForceCategory::Vehicle };
		int Value { 0 };
		int LastSeenFrame { 0 };
		bool NearOurBase { false };
		CellStruct LastCoords { CellStruct::Empty };
		bool AntiAir { false };
		int OwnerIndex { -1 };
		const TechnoTypeClass* Type { nullptr };
	};

	struct NeuralOpponentProfile
	{
		std::array<int, static_cast<size_t>(EnemyForceCategory::Count)> Value {};
		int TotalValue { 0 };
		int KnownDefenses { 0 };
		int RushPressure { 0 };
		// Value of seen enemy units able to shoot at aircraft, and known enemy AA defenses.
		int AntiAirValue { 0 };
		int KnownAADefenses { 0 };

		int Share(EnemyForceCategory category) const
		{
			// Share in 0..100 of the remembered enemy force value.
			return this->TotalValue > 0 ? this->Value[static_cast<size_t>(category)] * 100 / this->TotalValue : 0;
		}

		int Confidence() const
		{
			// 0..100: sparse early sightings must not swing the production much.
			return std::clamp(this->TotalValue / 4, 0, 100);
		}
	};

	// One running strategic plan (members and targets are kept by UniqueID, never by
	// pointer, and resolved every AI tick).
	struct NeuralPlanInstance
	{
		bool Active { false };
		int StartFrame { 0 };
		int Deadline { 0 };
		int InitialValue { 0 };
		bool Arrived { false };
		int ArrivedFrame { 0 };
		CellStruct TargetCell { CellStruct::Empty };
		CellStruct StagingCell { CellStruct::Empty };
		DWORD TargetID { 0 };
		std::vector<DWORD> Members {};
		int AuxBaseline { 0 };
		int AuxPeak { 0 };
		// Several targets at once (Factory Decapitation) or per-group sizes (Three-Front Assault).
		std::vector<DWORD> Targets {};
		// Sub-targets destroyed so far (chain plans: Rolling Siege), so a plan that did real
		// damage before its deadline is not scored as a plain failure.
		int Progress { 0 };
	};

	struct NeuralStuckTrack
	{
		CellStruct Cell { CellStruct::Empty };
		int SinceFrame { 0 };
	};

	struct NeuralAirLossTrack
	{
		DWORD ID { 0 };
		CellStruct LastCoords { CellStruct::Empty };
		bool IsCarrier { false };
	};

	struct NeuralDelayedRetreat
	{
		DWORD ID { 0 };
		int Frame { 0 };
	};

	// Where a packed factory deployer is heading (NeuralAI.MobileFactory.inl): the spot, why, when it
	// was last picked, when the trip started, the closest it got and when (a trip that stops getting
	// closer ends in a deploy on the spot), the unpack orders given where it stands, and until when
	// it keeps the goal it was packed up for without picking again.
	struct NeuralMobileFactoryGoal
	{
		CellStruct Cell { CellStruct::Empty };
		int Reason { 0 };
		int PickFrame { -100000 };
		int StartFrame { -1 };
		int BestDist { INT_MAX };
		int ProgressFrame { 0 };
		int DeployTries { 0 };
		int HoldUntil { -1 };
	};

	// A deployed mobile factory: since when it stands, and its health at the last sample (a sharp
	// drop since then makes it flee even from an attacker it cannot see).
	struct NeuralMobileFactoryStand
	{
		int Since { 0 };
		double Health { 1.0 };
		int HealthFrame { 0 };
	};

	struct NeuralGroundLossTrack
	{
		DWORD ID { 0 };
		CellStruct Coords { CellStruct::Empty };
		int Value { 0 };
	};

	struct NeuralHouseMacroState
	{
		CellStruct PendingClearanceCell { CellStruct::Empty };

		BuildingTypeClass* PendingClearanceType { nullptr };

		int PendingClearanceFoundationWidth { 0 };
		int PendingClearanceFoundationHeight { 0 };

		Foundation PendingClearanceFoundation { Foundation::_1x1 };

		int ClearanceAttempts { 0 };
		int NextClearanceCheckFrame { 0 };
		int LastClearanceFailedFrame { -1000 };
		int ConsecutivePlacementFailures { 0 };
		bool IsBuildingRecoveryRefinery { false };
		int NextHarvesterRecoveryFrame { 0 };
		int TargetRecoveryRefineryZone { -1 };
		CellStruct FrontlineThreatCoords { CellStruct::Empty };
		CellStruct FrontlineThreatBuildingCoords { CellStruct::Empty };
		int FrontlineThreatActiveFrames { 0 };
		int FrontlineThreatNeedsDefenses { 0 };
		std::map<unsigned int, int> TemporaryRecoveryRefineryPlacedFrame {};
		std::map<int, int> BuildingPlacementCooldownUntilFrame {};

		std::vector<NeuralAttackedBuildingRecord> AttackedBuildingsLIFO {};
		std::vector<NeuralUnsafeZone> UnsafePlacementZones {};

		// Real cross-island ferry demand published by the transport controller
		// (summed passenger Size, bucketed by passenger Size) and read by the
		// production selector in the same AI tick.
		std::array<int, 16> FerryDemandSizeBySize {};
		int FerryDemandFrame { -1 };

		// Fog-fair air threat memory: only what this house has actually seen.
		std::vector<NeuralAirThreatTrack> AirThreatTracks {};
		int AirThreatValue { 0 };
		int OwnAACapacity { 0 };
		int HeavyAirThreats { 0 };

		// Enemy buildings this house has actually seen (fog-fair base knowledge).
		std::unordered_set<DWORD> SeenEnemyBuildings {};

		// Start waypoints (0..7) already checked: ours/allied, or seen empty.
		uint32_t ScoutedStartMask { 0 };

		// Opponent model (see RefreshOpponentModel).
		std::unordered_map<DWORD, EnemyUnitSighting> EnemySightings {};
		NeuralOpponentProfile OpponentProfile {};

		// Strategic plan library (see UpdateStrategicPlans).
		std::array<NeuralPlanInstance, static_cast<size_t>(NeuralPlanId::Count)> Plans {};
		std::array<int, static_cast<size_t>(NeuralPlanId::Count)> PlanNextAttemptFrame {};
		std::unordered_set<DWORD> PlanMembers {};
		// Blackout plan success opens a window where the main wave launches earlier.
		int BlackoutUntilFrame { -1 };

		// Landing spots where a loaded transport of ours was destroyed (cell, frame):
		// FindSafeIslandLandingCell avoids them for a while (no "Normandy" repeats).
		std::vector<std::pair<CellStruct, int>> BloodyBeaches {};

		// Air danger memory (see UpdateAirLossMemory): our flying units seen in the air last
		// tick, and the cells where one of them vanished away from our base (shot down), so
		// air lanes avoid anti-air this house cannot see any more. Own knowledge only.
		std::vector<NeuralAirLossTrack> AirLossTracks {};
		std::vector<std::pair<CellStruct, int>> AirDangerSpots {};
		// Stuck order watchdog (UpdateStuckUnits): by unit UniqueID, the cell where it has been
		// standing with a destination and since when. Keyed lookups only, never iterated.
		std::unordered_map<DWORD, NeuralStuckTrack> StuckTracks {};
		// Expansion drive: ore fields (12x12 blocks) on our landmass more than 14 cells from any
		// of our hubs (feature scan), and how close the base crawl has brought a building anchor
		// to the nearest free worthy field (placement scan) so a refinery is wanted there.
		int UncolonizedOreFields { 0 };
		// Idle harvester dispatcher (UpdateIdleHarvesters), same keyed shape as StuckTracks.
		std::unordered_map<DWORD, NeuralStuckTrack> IdleHarvesterTracks {};
		// Offensive base crawl bookkeeping (UpdateOffensiveStones): cells chosen for a stone toward
		// the enemy and not yet confirmed placed, then the placed stones by UniqueID, so how many
		// forward stones survive is counted (crawlStoneForward / crawlStoneLost) for the trainer.
		std::vector<std::pair<CellStruct, int>> OffensiveStoneCells {};
		std::vector<DWORD> OffensiveStones {};
		// Last sale of a factory over the difficulty cap (SellFactoriesOverDifficultyCap): no factory
		// is built past the cap for an outpost within 4500 frames of it, so a factory that lands
		// at another hub (the outpost still asking) cannot loop build-and-sell.
		int LastFactoryCapSaleFrame { -100000 };
		// Failed refinery placements: the 10-cell reach to its field (FindSpatialTetrisBuildCell) grows
		// 4 cells per failure and is dropped after 3, so a map too rugged to build next to its ore still
		// gets its refinery. A success lowers it by one only, so a cramped base does not fail twice
		// before every refinery (the ConYard lost its time on them).
		int RefineryReachFailures { 0 };
		// Refineries placed with that widened reach (UniqueIDs): the best spot a rugged map allowed,
		// so the strict refinery audit of the resource hoarder never sells them (it would only
		// rebuild them as far away again).
		std::vector<DWORD> RelaxedReachRefineries {};
		// Resource claim (UpdateResourceClaim, the resource hoarder of Expert 6): the best ore field
		// not mined by us yet, chosen by value (spawner tree, size), distance and risk (tier 0 away
		// from known enemy structures, 1 near them). MCVs, the crawl and refineries head there.
		// TreeCell is the field's spawner tree, where its refinery is pulled.
		CellStruct ResourceClaimCell { CellStruct::Empty };
		CellStruct ResourceClaimTreeCell { CellStruct::Empty };
		int ResourceClaimTier { -1 };
		int ResourceClaimZone { -1 };
		int LastResourceClaimFrame { -100000 };
		// Tech tree progress (UpdateTechUnlocks): per building type index, how many types it would
		// unlock as a missing prerequisite; whether one of those is buildable right now.
		std::unordered_map<int, int> TechUnlocks {};
		// The not yet buildable types behind each of those buildings (same scan), so a building can
		// be valued by what it would open, not only by how much.
		std::unordered_map<int, std::vector<TechnoTypeClass*>> TechUnlockTypes {};
		bool HasBuildableTechUnlock { false };
		int TechUnlockFrame { -100000 };
		// Counter composition (UpdateCounterComposition, Expert 2): production score shift per
		// buildable combat type against the enemy army seen so far, and the extra priority of the
		// first of a building that would unlock a better counter than any we can build now.
		std::unordered_map<const TechnoTypeClass*, int> CounterBonus {};
		std::unordered_map<int, int> CounterUnlockBonus {};
		int CounterFrame { -100000 };
		// Progressive unit tier (UpdateUnitTierCap): highest TechLevel of combat units it may build
		// now, the highest tier of enemy units it has seen, and its peak building count (a house
		// that lost half its base unlocks one tier more).
		int UnitTierCap { 2 };
		int SeenEnemyUnitTier { 0 };
		int PeakOwnedBuildings { 0 };
		int LastUnitTierFrame { -100000 };
		// Expert 0 (UpdateEconomyCrisis): economy peaks, raid recovery flag and action timers.
		int PeakRefineries { 0 };
		int PeakHarvesters { 0 };
		bool EconomyRecoveryActive { false };
		int LastEconomyCrisisFrame { -100000 };
		// Productions put on hold while broke (UpdateEconomyCrisis), by the UniqueID of their object.
		std::vector<DWORD> BrokeHeldObjects {};
		// Broke hold on: no repairs (wrench, service depot, shipyard) either.
		bool BrokeHoldActive { false };
		int LastBrokeSaleFrame { -100000 };
		int LastHarvesterBalanceFrame { -100000 };
		// Expert 3 (UpdateFfaTargets): main enemy choice bonus per enemy house index (0..7).
		std::array<int, 8> FfaEnemyBonus {};
		// Percent of an enemy's visible threat its main enemy priority keeps (Expert 3: 40 unless it is
		// at our door, so the biggest visible base is not the default target of a free-for-all).
		std::array<int, 8> FfaThreatScale { 100, 100, 100, 100, 100, 100, 100, 100 };
		int LastFfaTargetFrame { -100000 };
		// Expert 5 selector: combat value of our units and of the visible armed enemies within reach
		// of each other (a real fight), measured by the micro each AI tick, and when it last happened.
		int ArmyContactOwn { 0 };
		int ArmyContactEnemy { 0 };
		int LastArmyContactFrame { -100000 };
		// Frame of the last repair trip ordered per unit (UniqueID): a trip is not ordered again for
		// 450 frames, so another routine moving the unit cannot turn it into one order per AI tick.
		std::unordered_map<DWORD, int> RepairTripFrames {};
		int LastStoneCheckFrame { -100000 };
		int LastIdleFlyerCheckFrame { -100000 };
		// ConYard pack-up anti-loop: where and when our last ConYard was packed into an MCV.
		CellStruct LastYardUndeployCell { CellStruct::Empty };
		int LastYardUndeployFrame { -100000 };
		// Bank-driven factory scaling checks (every 900 frames): share of the non-defense
		// structures we can build now that we already own, and whether a war factory still fits.
		// Naval need (HouseNeedsNaval, every 900 frames).
		int NavalNeedFrame { -100000 };
		bool NavalNeed { false };
		int BankCheckFrame { -100000 };
		int TechCoveragePct { 0 };
		bool BankFactoryRoom { false };
		// Bank window bookkeeping: factories owned when the current 900-frame window opened and
		// the placement failure count then (one boosted factory per window, none after a failure).
		int BankWindow { -1 };
		int BankWindowFactories { 0 };
		int BankWindowFailures { 0 };
		// Packed-up yard MCVs sent across water by transport (by UniqueID, in order).
		std::vector<DWORD> FerryingMcvs {};
		// Field scouting (UpdateFieldScouting): the current scout and the free fields visited.
		DWORD FieldScoutUid { 0 };
		int LastFieldScoutFrame { -100000 };
		std::vector<std::pair<CellStruct, int>> ScoutedFields {};
		// Ferry wait safety net (DispatchFootToTargetOrTransport): since when a unit has been asking
		// for a crossing, and until when it may not ask again after giving up. Keyed lookups only.
		std::unordered_map<DWORD, int> FerryWaitSince {};
		std::unordered_map<DWORD, int> FerryBlockedUntil {};
		int FreeFieldAnchorDist { 9999 };
		int FreeFieldFrame { -100000 };
		// Last airlift that turned back (damaged carrier or a defended drop zone).
		int LastAirDropAbortFrame { -100000 };

		// Plan side effects read by building production and placement (UpdateStrategicPlans):
		// a new island ConYard to fortify, and a forward repair station toward the front.
		CellStruct IslandHoldCell { CellStruct::Empty };
		int IslandHoldUntilFrame { -1 };
		CellStruct ForwardRepairCell { CellStruct::Empty };
		int ForwardRepairUntilFrame { -1 };
		// Survival plan: next frame a building may be sold to fund a replacement MCV.
		int SurvivalNextSellFrame { 0 };

		// Attack route memory (this match only, fog-fair): enemy combat value seen arriving near
		// our base, by direction from home (8 sectors, see AttackSectorOf), decayed x0.9 every
		// 2700 frames. Defense placement and pickets favor the sides we are really hit from.
		std::array<int, 8> AttackSectorWeight {};
		int AttackSectorDecayFrame { 0 };

		// Match style (DrawMatchStyle / ApplyMatchStyle): per-match offsets of the plan and macro
		// slots, the opening drawn for this match, and the current surprise tech switch.
		std::array<int16_t, 128> StyleOffset {};
		int Opening { 0 };
		int TechSwitchMode { 0 };
		int TechSwitchUntilFrame { 0 };

		// Wave memory (UpdateMatchTactics): the current wave target and timing, our losses near it
		// by approach side (x0.8 every 2700 frames), the counterattack / continuous pressure
		// windows and the enemy house to focus (FFA weakest or weakest enemy team member).
		CellStruct LastWaveTargetCell { CellStruct::Empty };
		int LastWaveDispatchFrame { -100000 };
		int WaveStartFrame { -100000 };
		std::array<int, 8> FailedApproachWeight {};
		int FailedApproachDecayFrame { 0 };
		std::vector<NeuralGroundLossTrack> GroundLossTracks {};
		int HomeThreatPeak { 0 };
		int HomeThreatPeakFrame { 0 };
		int CounterattackUntilFrame { -1 };
		bool PressureMode { false };
		int FocusEnemyIndex { -1 };
		int TacticsRefreshFrame { -100000 };

		// Plans 23..38: staggered retreats waiting their turn, the next scouting run, combo boosts
		// per plan and the frame each ready offensive superweapon became ready (SW + army, point 37).
		std::vector<NeuralDelayedRetreat> DelayedRetreats {};
		int NextScoutRunFrame { 3600 };
		std::array<int, static_cast<size_t>(NeuralPlanId::Count)> PlanBoost {};
		std::array<int, static_cast<size_t>(NeuralPlanId::Count)> PlanBoostUntilFrame {};
		std::map<int, int> SuperReadySinceFrame {};

		// Mobile factories (NeuralAI.MobileFactory.inl): the trip of each packed factory deployer (by
		// unit UniqueID), each deployed mobile factory (by building UniqueID), the last spot one fled
		// from, the last pack-up (where, toward what and why, handed to the vehicle it becomes),
		// whether the main base ever had a fixed factory of each kind (unit, infantry, aircraft), the
		// main base ConYard, when a packed one was last seen and when one last unfolded (a vehicle that
		// vanished in between was lost), and the cached build answers (by unit type index: frame,
		// wanted). Keyed lookups only.
		std::unordered_map<DWORD, NeuralMobileFactoryGoal> MobileFactoryGoals {};
		std::unordered_map<DWORD, NeuralMobileFactoryStand> MobileFactoryStands {};
		CellStruct MobileFactoryAvoidCell { CellStruct::Empty };
		int MobileFactoryAvoidFrame { -100000 };
		int MobileFactoryLastPackFrame { -100000 };
		CellStruct MobileFactoryPackCell { CellStruct::Empty };
		CellStruct MobileFactoryPackTarget { CellStruct::Empty };
		int MobileFactoryPackReason { 0 };
		std::array<bool, 3> MobileFactoryMainHad {};
		DWORD MobileFactoryHomeID { 0 };
		int MobileFactoryVehicleSeenFrame { -100000 };
		int MobileFactoryDeployFrame { -100000 };
		std::unordered_map<int, std::pair<int, bool>> MobileFactoryWanted {};

		void ClearClearanceReservation()
		{
			this->PendingClearanceCell = CellStruct::Empty;

			this->PendingClearanceType = nullptr;

			this->PendingClearanceFoundationWidth = 0;

			this->PendingClearanceFoundationHeight = 0;

			this->PendingClearanceFoundation = Foundation::_1x1;

			this->ClearanceAttempts = 0;

			this->NextClearanceCheckFrame = 0;
		}

		void Reset()
		{
			this->ClearClearanceReservation();

			this->LastClearanceFailedFrame = -1000;
			this->ConsecutivePlacementFailures = 0;
			this->IsBuildingRecoveryRefinery = false;
			this->NextHarvesterRecoveryFrame = 0;
			this->TargetRecoveryRefineryZone = -1;
			this->FrontlineThreatCoords = CellStruct::Empty;
			this->FrontlineThreatBuildingCoords = CellStruct::Empty;
			this->FrontlineThreatActiveFrames = 0;
			this->FrontlineThreatNeedsDefenses = 0;
			this->TemporaryRecoveryRefineryPlacedFrame.clear();
			this->BuildingPlacementCooldownUntilFrame.clear();
			this->AttackedBuildingsLIFO.clear();
			this->UnsafePlacementZones.clear();
			this->FerryDemandSizeBySize.fill(0);
			this->FerryDemandFrame = -1;
			this->AirThreatTracks.clear();
			this->AirThreatValue = 0;
			this->OwnAACapacity = 0;
			this->HeavyAirThreats = 0;
			this->SeenEnemyBuildings.clear();
			this->ScoutedStartMask = 0;
			this->EnemySightings.clear();
			this->OpponentProfile = NeuralOpponentProfile {};
			this->Plans.fill(NeuralPlanInstance {});
			this->PlanNextAttemptFrame.fill(0);
			this->PlanMembers.clear();
			this->BlackoutUntilFrame = -1;
			this->BloodyBeaches.clear();
			this->AirLossTracks.clear();
			this->AirDangerSpots.clear();
			this->StuckTracks.clear();
			this->UncolonizedOreFields = 0;
			this->IdleHarvesterTracks.clear();
			this->OffensiveStoneCells.clear();
			this->OffensiveStones.clear();
			this->LastFactoryCapSaleFrame = -100000;
			this->RefineryReachFailures = 0;
			this->RelaxedReachRefineries.clear();
			this->ResourceClaimCell = CellStruct::Empty;
			this->ResourceClaimTreeCell = CellStruct::Empty;
			this->ResourceClaimTier = -1;
			this->ResourceClaimZone = -1;
			this->LastResourceClaimFrame = -100000;
			this->TechUnlocks.clear();
			this->TechUnlockTypes.clear();
			this->HasBuildableTechUnlock = false;
			this->TechUnlockFrame = -100000;
			this->CounterBonus.clear();
			this->CounterUnlockBonus.clear();
			this->CounterFrame = -100000;
			this->UnitTierCap = 2;
			this->SeenEnemyUnitTier = 0;
			this->PeakOwnedBuildings = 0;
			this->LastUnitTierFrame = -100000;
			this->PeakRefineries = 0;
			this->PeakHarvesters = 0;
			this->EconomyRecoveryActive = false;
			this->LastEconomyCrisisFrame = -100000;
			this->BrokeHeldObjects.clear();
			this->BrokeHoldActive = false;
			this->LastBrokeSaleFrame = -100000;
			this->LastHarvesterBalanceFrame = -100000;
			this->FfaEnemyBonus.fill(0);
			this->FfaThreatScale.fill(100);
			this->LastFfaTargetFrame = -100000;
			this->ArmyContactOwn = 0;
			this->ArmyContactEnemy = 0;
			this->LastArmyContactFrame = -100000;
			this->RepairTripFrames.clear();
			this->LastStoneCheckFrame = -100000;
			this->LastIdleFlyerCheckFrame = -100000;
			this->LastYardUndeployCell = CellStruct::Empty;
			this->LastYardUndeployFrame = -100000;
			this->NavalNeedFrame = -100000;
			this->NavalNeed = false;
			this->BankCheckFrame = -100000;
			this->TechCoveragePct = 0;
			this->BankFactoryRoom = false;
			this->BankWindow = -1;
			this->BankWindowFactories = 0;
			this->BankWindowFailures = 0;
			this->FerryingMcvs.clear();
			this->FieldScoutUid = 0;
			this->LastFieldScoutFrame = -100000;
			this->ScoutedFields.clear();
			this->FerryWaitSince.clear();
			this->FerryBlockedUntil.clear();
			this->FreeFieldAnchorDist = 9999;
			this->FreeFieldFrame = -100000;
			this->LastAirDropAbortFrame = -100000;
			this->IslandHoldCell = CellStruct::Empty;
			this->IslandHoldUntilFrame = -1;
			this->ForwardRepairCell = CellStruct::Empty;
			this->ForwardRepairUntilFrame = -1;
			this->SurvivalNextSellFrame = 0;
			this->AttackSectorWeight.fill(0);
			this->AttackSectorDecayFrame = 0;
			this->StyleOffset.fill(0);
			this->Opening = 0;
			this->TechSwitchMode = 0;
			this->TechSwitchUntilFrame = 0;
			this->LastWaveTargetCell = CellStruct::Empty;
			this->LastWaveDispatchFrame = -100000;
			this->WaveStartFrame = -100000;
			this->FailedApproachWeight.fill(0);
			this->FailedApproachDecayFrame = 0;
			this->GroundLossTracks.clear();
			this->HomeThreatPeak = 0;
			this->HomeThreatPeakFrame = 0;
			this->CounterattackUntilFrame = -1;
			this->PressureMode = false;
			this->FocusEnemyIndex = -1;
			this->TacticsRefreshFrame = -100000;
			this->DelayedRetreats.clear();
			this->NextScoutRunFrame = 3600;
			this->PlanBoost.fill(0);
			this->PlanBoostUntilFrame.fill(0);
			this->SuperReadySinceFrame.clear();
			this->MobileFactoryGoals.clear();
			this->MobileFactoryStands.clear();
			this->MobileFactoryAvoidCell = CellStruct::Empty;
			this->MobileFactoryAvoidFrame = -100000;
			this->MobileFactoryLastPackFrame = -100000;
			this->MobileFactoryPackCell = CellStruct::Empty;
			this->MobileFactoryPackTarget = CellStruct::Empty;
			this->MobileFactoryPackReason = 0;
			this->MobileFactoryMainHad.fill(false);
			this->MobileFactoryHomeID = 0;
			this->MobileFactoryVehicleSeenFrame = -100000;
			this->MobileFactoryDeployFrame = -100000;
			this->MobileFactoryWanted.clear();
		}
	};

	std::array<NeuralHouseMacroState, 32> MacroStates {};

	// Per-transport ferry trip: the island where the cargo was loaded and whether
	// the transport already left it. A loaded transport must never unload on its
	// loading island, nor stop again to pick up when it brushes a friendly coast.
	struct NeuralFerryTripRecord
	{
		DWORD TransportID { 0 };
		int LoadZone { -1 };
		bool Departed { false };
		int Frame { 0 };

		// Safe sea lane toward the landing cell (see FindSafeFerryWaypoint).
		CellStruct RouteTarget { CellStruct::Empty };
		CellStruct RouteWaypoint { CellStruct::Empty };
		int RouteFrame { -1 };

		// Progress watchdog: a loaded transport that stops moving near its landing zone
		// first relocates, then unloads where it is (never on its loading island).
		CellStruct ProgressCoords { CellStruct::Empty };
		int ProgressFrame { 0 };
		int StuckStage { 0 };
		int ProgressPassengers { -1 };

		// Convoy: frame at which the cargo was ready to sail (waiting for sister transports).
		int ReadyFrame { -1 };
		CellStruct LastCoords { CellStruct::Empty };
		int HouseIndex { -1 };
	};

	constexpr int FerryTripCapacity = 128;
	std::array<NeuralFerryTripRecord, FerryTripCapacity> FerryTrips {};

	// Safe air lane of a flying carrier (carryall aircraft, jumpjet carryall, passenger
	// aircraft) toward its current goal (see GetSafeAirHop). Keyed by UniqueID and only
	// accessed by key, so it is deterministic.
	struct NeuralAirRouteRecord
	{
		CellStruct Target { CellStruct::Empty };
		CellStruct Waypoint { CellStruct::Empty };
		int Frame { -1 };
	};

	std::unordered_map<DWORD, NeuralAirRouteRecord> AirRoutes;

	// Empty carryall flying to its pickup along the safe air lane (see AdvanceRoutedPickup):
	// carrier UniqueID -> cargo UniqueID. Accessed by key only.
	struct NeuralAirPickupRecord
	{
		DWORD CargoID { 0 };
		int Frame { -1 };
	};

	std::unordered_map<DWORD, NeuralAirPickupRecord> AirPickups;

	// Loaded carrier UniqueID -> frame its drop was aborted (see ResolveAirDropCell). Key access only.
	std::unordered_map<DWORD, int> AirDropAborts;

	NeuralFerryTripRecord* FindFerryTrip(DWORD transportID)
	{
		for (auto& trip : FerryTrips)
		{
			if (trip.TransportID == transportID)
				return &trip;
		}

		return nullptr;
	}

	NeuralFerryTripRecord& AcquireFerryTrip(DWORD transportID)
	{
		if (auto const pTrip = FindFerryTrip(transportID))
			return *pTrip;

		NeuralFerryTripRecord* pOldest = &FerryTrips[0];

		for (auto& trip : FerryTrips)
		{
			if (trip.TransportID == 0)
			{
				pOldest = &trip;
				break;
			}

			if (trip.Frame < pOldest->Frame)
				pOldest = &trip;
		}

		*pOldest = NeuralFerryTripRecord {};
		pOldest->TransportID = transportID;
		pOldest->Frame = Unsorted::CurrentFrame;

		return *pOldest;
	}

	void ClearFerryTrip(DWORD transportID)
	{
		if (auto const pTrip = FindFerryTrip(transportID))
			*pTrip = NeuralFerryTripRecord {};
	}

	NeuralHouseMacroState& GetMacroState(const HouseClass* pHouse)
	{
		const int idx = pHouse ? std::clamp(pHouse->ArrayIndex, 0, 31) : 0;

		return MacroStates[idx];
	}

	// Match style (point 28/29): at scenario start every Neural AI house draws, from the synced
	// scenario RNG (same draws on every client: no desync), a small offset for each plan slot
	// (89..127) and for the main macro slots, plus an opening for the first minutes. The size is
	// [AI] NeuralAI.StyleVariance= (default 150, 0 = off). Same brain, map and side, yet a
	// different style each match. The draw count never depends on settings or policy files.
	constexpr int MacroStyleSlots[] = { 5, 6, 13, 14, 15, 24, 25, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 37, 38, 39 };

	// Openings: 0 balanced, 1 fast expansion, 2 early tech, 3 early pressure, 4 economy turtle.
	// Key slot of each (its trained preference), and the offsets applied while it lasts.
	constexpr int OpeningKeySlots[] = { -1, 35, 13, 37, 39 };

	struct StyleSlotOffset
	{
		int Slot;
		int Offset;
	};

	constexpr StyleSlotOffset OpeningOffsets[][4] = {
		{ { -1, 0 }, { -1, 0 }, { -1, 0 }, { -1, 0 } },
		{ { 35, 300 }, { 31, 250 }, { 30, 200 }, { 37, -150 } },
		{ { 13, 300 }, { 4, 200 }, { 24, -100 }, { -1, 0 } },
		{ { 37, 300 }, { 24, 200 }, { 25, 200 }, { 14, -150 } },
		{ { 14, 250 }, { 5, 200 }, { 39, 250 }, { 37, -200 } },
	};

	// Surprise tech switch modes: 0 none, 1 air, 2 anti-infantry, 3 anti-armor.
	constexpr StyleSlotOffset TechSwitchOffsets[][4] = {
		{ { -1, 0 }, { -1, 0 }, { -1, 0 }, { -1, 0 } },
		{ { 32, 300 }, { 33, 250 }, { 8, 200 }, { 84, 200 } },
		{ { 25, 250 }, { 23, 120 }, { 16, -100 }, { -1, 0 } },
		{ { 17, 200 }, { 24, 250 }, { 27, 150 }, { -1, 0 } },
	};

	// openingTrained[i]: the trainer's preference for opening i (its key slot's bias / 3, or 0).
	void DrawMatchStyle(HouseClass* pHouse, const std::array<int, 5>& openingTrained)
	{
		if (!pHouse)
			return;

		auto& macro = GetMacroState(pHouse);
		auto& random = ScenarioClass::Instance->Random;
		const int idx = std::clamp(pHouse->ArrayIndex, 0, 31);
		const int variance = std::clamp(RulesExt::Global()->NeuralAI_StyleVariance.Get(), 0, 600);

		HouseStyleSalt[idx] = static_cast<uint32_t>(random.RandomRanged(1, 0x7FFFFFFF));
		macro.StyleOffset.fill(0);

		for (const int slot : MacroStyleSlots)
			macro.StyleOffset[slot] = static_cast<int16_t>(random.RandomRanged(-variance, variance));

		for (int slot = 89; slot < 128; ++slot)
			macro.StyleOffset[slot] = static_cast<int16_t>(random.RandomRanged(-variance, variance));

		// The trainer's preference for each opening's key slot plus a draw of the same size.
		int bestOpening = 0;
		int bestScore = INT_MIN;

		for (int opening = 0; opening < static_cast<int>(std::size(OpeningKeySlots)); ++opening)
		{
			const int score = random.RandomRanged(-variance, variance) + openingTrained[static_cast<size_t>(opening)];

			if (score > bestScore)
			{
				bestScore = score;
				bestOpening = opening;
			}
		}

		macro.Opening = variance > 0 ? bestOpening : 0;
	}

	// Adds the match style, the opening (full until frame 6000, fading out by 12000) and the
	// surprise tech switch (point 32) to this tick's inferred scores.
	void ApplyMatchStyle(HouseClass* pHouse, std::array<int16_t, NeuralAI::OutputVectorSize>& scores)
	{
		if (!pHouse)
			return;

		auto& macro = GetMacroState(pHouse);

		for (size_t slot = 0; slot < scores.size() && slot < macro.StyleOffset.size(); ++slot)
		{
			if (macro.StyleOffset[slot] != 0)
				scores[slot] = ClampInt16(scores[slot] + macro.StyleOffset[slot]);
		}

		const int frame = Unsorted::CurrentFrame;
		const int openingWeight = frame <= 6000 ? 100 : (frame >= 12000 ? 0 : (12000 - frame) / 60);

		if (macro.Opening > 0 && openingWeight > 0)
		{
			for (auto const& entry : OpeningOffsets[macro.Opening])
			{
				if (entry.Slot >= 0)
					scores[entry.Slot] = ClampInt16(scores[entry.Slot] + entry.Offset * openingWeight / 100);
			}
		}

		// Surprise tech switch: every 5400 frames, against what this house has really seen, commit
		// hard to what the enemy lacks: aircraft when it shows almost no anti-air, anti-infantry
		// against infantry masses, anti-armor against vehicle/artillery armies.
		if (frame >= macro.TechSwitchUntilFrame || macro.TechSwitchUntilFrame - frame > 5400)
		{
			auto const& foe = macro.OpponentProfile;
			int mode = 0;

			if (foe.Confidence() >= 50 && SlotGate(scores[42], -100, 42))
			{
				if (foe.AntiAirValue * 100 < foe.TotalValue * 8 && foe.KnownAADefenses <= 1)
					mode = 1;
				else if (foe.Share(EnemyForceCategory::Infantry) >= 55)
					mode = 2;
				else if (foe.Share(EnemyForceCategory::Vehicle) + foe.Share(EnemyForceCategory::Artillery) >= 60)
					mode = 3;
			}

			macro.TechSwitchMode = mode;
			macro.TechSwitchUntilFrame = frame + 5400;
		}

		for (auto const& entry : TechSwitchOffsets[std::clamp(macro.TechSwitchMode, 0, 3)])
		{
			if (entry.Slot >= 0)
				scores[entry.Slot] = ClampInt16(scores[entry.Slot] + entry.Offset);
		}
	}

	// Deterministic per-house jitter in [0, range) for timings (point 30): changes every
	// `period` frames and between matches (style salt), identical on every client.
	int StyleJitter(int houseIndex, int key, int period, int range)
	{
		if (range <= 1)
			return 0;

		const uint32_t salt = (houseIndex >= 0 && houseIndex < 32) ? HouseStyleSalt[houseIndex] : 0u;
		uint32_t h = salt ^ (static_cast<uint32_t>(Unsorted::CurrentFrame / std::max(1, period)) * 2654435761u)
			^ (static_cast<uint32_t>(key) * 40503u);
		h ^= h >> 15;
		h *= 2246822519u;
		h ^= h >> 13;

		return static_cast<int>(h % static_cast<uint32_t>(range));
	}

	// Home reference of a house: its first ConYard, else its start cell.
	CellStruct GetHouseHomeCell(HouseClass* pHouse)
	{
		if (!pHouse)
			return CellStruct::Empty;

		if (pHouse->ConYards.Count > 0 && pHouse->ConYards.GetItem(0))
			return pHouse->ConYards.GetItem(0)->GetMapCoords();

		return pHouse->BaseSpawnCell;
	}

	// 8 compass sectors (E, SE, S, SW, W, NW, N, NE in map cells) with integer math only
	// (tan 22.5 deg ~ 5/12), so every client gets the same sector: no atan2.
	constexpr int AttackSectorDX[8] = { 1, 1, 0, -1, -1, -1, 0, 1 };
	constexpr int AttackSectorDY[8] = { 0, 1, 1, 1, 0, -1, -1, -1 };

	int AttackSectorOf(const CellStruct& from, const CellStruct& to)
	{
		const int dx = to.X - from.X;
		const int dy = to.Y - from.Y;
		const int ax = std::abs(dx);
		const int ay = std::abs(dy);
		const int sx = (ax * 12 > ay * 5) ? (dx > 0 ? 1 : -1) : 0;
		const int sy = (ay * 12 > ax * 5) ? (dy > 0 ? 1 : -1) : 0;

		for (int i = 0; i < 8; ++i)
		{
			if (AttackSectorDX[i] == sx && AttackSectorDY[i] == sy)
				return i;
		}

		return -1;
	}

	// Strongest remembered attack sector (ties: lowest index), or -1 when no side stands out:
	// at least 300 value and 40% of everything that came at us.
	int GetMainAttackSector(HouseClass* pHouse)
	{
		auto const& weights = GetMacroState(pHouse).AttackSectorWeight;
		int best = -1;
		int total = 0;

		for (int i = 0; i < 8; ++i)
		{
			total += weights[i];

			if (best < 0 || weights[i] > weights[best])
				best = i;
		}

		return (best >= 0 && weights[best] >= 300 && weights[best] * 10 >= total * 4) ? best : -1;
	}

	// Survival micro (anti-superweapon scatter, dying transport unload, drainer
	// interception, wounded pull-back) is core competence: a trained slot can only
	// veto it when strongly negative, so ES noise never switches it off by accident.
	constexpr int SurvivalMicroVetoScore = -300;

	// An enemy building counts as known once this house has seen it; structures do
	// not move, so remembering them is fair while x-raying the fog is not.
	bool IsEnemyBuildingKnown(HouseClass* pHouse, const BuildingClass* pBld)
	{
		if (!pHouse || !pBld)
			return false;

		auto const& seen = GetMacroState(pHouse).SeenEnemyBuildings;

		return seen.find(pBld->UniqueID) != seen.end();
	}

} // namespace

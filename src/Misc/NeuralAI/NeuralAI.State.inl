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

	// A defense of ours destroyed before it killed anything, and the ground reach (cells) of what
	// destroyed it: the spot did not pay, so the next tower keeps away from it for a while.
	struct NeuralWastedDefenseSpot
	{
		CellStruct Coords { CellStruct::Empty };
		int Frame { 0 };
		int KillerRangeCells { 0 };
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
	// Last type and cell seen of a plan member, so a member that deploys into a structure or packs
	// up again (a new object with a new UniqueID) is followed into what it became.
	struct NeuralPlanMemberTrace
	{
		const TechnoTypeClass* Type { nullptr };
		CellStruct Cell { CellStruct::Empty };
		int Frame { 0 };
	};

	// A group of capital units of one domain (ground, air, sea): UpdateCapitalGroups.
	struct NeuralCapitalGroup
	{
		std::vector<DWORD> Members {};
		bool Attacking { false };
		DWORD TargetID { 0 };
		int StartValue { 0 };
	};

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
		// Deadline extension for groups still closing in on their target (UpdateStrategicPlans):
		// the length the plan was started with, the last progress check (frame and the members'
		// mean distance to the target, -1 before the first check) and whether that check found
		// the group nearer than the one before.
		int BaseDuration { 0 };
		int ProgressCheckFrame { 0 };
		int ProgressCheckDistance { -1 };
		bool ClosingIn { false };
		// Combat value of the group at the start weighted by health (early abort checks), and the
		// last frame the target was in current sight (Hunt Key Units).
		int InitialStrength { 0 };
		int TargetSeenFrame { 0 };
		// Per member: a frame (Pickets: until when a sentry holds back, when it last saw something).
		std::vector<int> MemberFrames {};
	};

	struct NeuralStuckTrack
	{
		CellStruct Cell { CellStruct::Empty };
		int SinceFrame { 0 };
	};

	// Aircraft attack watchdog (UpdateStuckUnits): where an aircraft on Mission::Attack stands and
	// since when, and its ammo and since when it last changed (a shot fired or a reload).
	struct NeuralAirStallTrack
	{
		CellStruct Cell { CellStruct::Empty };
		int SinceFrame { 0 };
		int Ammo { 0 };
		int AmmoSinceFrame { 0 };
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

	// Expert 3 diplomacy (NeuralAI.Diplomacy.inl), frames unless noted.
	// Grace window at the start of a truce: attacks launched before it (missiles, artillery shells,
	// a superweapon or a storm already running) cannot be called back, so damage between the new
	// partners in the first 10 seconds of game time counts for nothing.
	constexpr int PactGraceFrames = 150;
	// A coalition stays small (2 or 3 houses from our point of view): at most two truce partners.
	constexpr int PactMaxPartners = 2;
	// Betrayal: aimed damage from a partner in force (an assault, not a stray shot) within a window.
	constexpr int BetrayalDamage = 1500;
	constexpr int BetrayalWindowFrames = 450;
	// Collateral: any other damage from a partner (splash, units that hit us while firing at the
	// shared target, small aimed hits) over a window; above the limit the truce with it ends.
	constexpr int CollateralDamageLimit = 2500;
	constexpr int CollateralWindowFrames = 1800;
	// After a truce ends without betrayal the ex-partner is not our main target for a while.
	constexpr int ExPartnerCooldownFrames = 1800;
	// Stalemate: not before this frame (about 30 minutes of game time), no defeat for this long.
	constexpr int StalemateMinFrame = 27000;
	constexpr int StalemateNoDefeatFrames = 18000;

	// Why a coalition ended (telemetry and log), and why a truce with one partner ended.
	enum class NeuralPactEnd : int
	{
		Betrayal = 0,
		Weakened = 1,
		TargetGone = 2,
		Timeout = 3,
		Team = 4,
		Count = 5
	};

	enum class NeuralTruceEnd : int
	{
		Betrayal = 0,
		Collateral = 1,
		Count = 2
	};

	// A spot where this house saw another house's army hitting its coalition target.
	struct NeuralCoalitionFront
	{
		CellStruct Cell { CellStruct::Empty };
		int Frame { 0 };
	};

	// Expert 3 diplomacy (NeuralAI.Diplomacy.inl): this house's own reading of the other houses
	// (seen strength, who fights whom, grudges, traitors) and its tacit coalition. Never a real
	// alliance, never shared with another house; houses of a preset team never get one. Indices are
	// house ArrayIndex 0..7.
	struct NeuralCoalitionState
	{
		// The house we lean against (-1 none), the trigger (1 elimination, 2 runaway leader,
		// 3 stalemate, 4 traitor), how the target was chosen (0 by the trigger, 1 grudge, 2 most
		// fragile, 3 poorest, 4 traitor), since and until when.
		int Target { -1 };
		int Reason { 0 };
		int TargetChoice { 0 };
		int SinceFrame { -1 };
		int UntilFrame { -1 };
		// Peak seen strength and known structures of the target while the coalition lasts.
		int TargetPeakStrength { 0 };
		int TargetPeakBuildings { 0 };
		// Last decision: the target and trigger it was taken for, and whether we joined.
		int DecidedTarget { -1 };
		int DecidedSerial { -1 };
		bool DecidedJoin { false };
		// No new coalition against a house before this frame (one against it ended).
		std::array<int, 8> CooldownUntil {};
		// Last frame each house was seen fighting our target, when our truce with it began (-1 none),
		// and the truces that ended (betrayal, collateral) until the coalition ends.
		std::array<int, 8> PartnerSeenFrame { -100000, -100000, -100000, -100000, -100000, -100000, -100000, -100000 };
		std::array<int, 8> TruceSinceFrame { -1, -1, -1, -1, -1, -1, -1, -1 };
		uint32_t RevokedMask { 0 };
		// Would-be partners refused this coalition because we distrust them (telemetry once each).
		uint32_t DistrustMask { 0 };
		// Last frame each house hit one of ours on purpose (damage hook: the victim always knows).
		std::array<int, 8> HurtByFrame { -100000, -100000, -100000, -100000, -100000, -100000, -100000, -100000 };
		// Damage taken from each partner in the current windows: aimed (betrayal test) and any other
		// (collateral test), with the frame each window opened.
		std::array<int, 8> AimedDamage {};
		std::array<int, 8> AimedWindowFrame {};
		std::array<int, 8> CollateralDamage {};
		std::array<int, 8> CollateralWindowFrame {};
		// Aimed damage we did to each partner in the same kind of window: when both sides trade
		// shots (units clashing at the shared target) it is a skirmish, not a betrayal.
		std::array<int, 8> AimedDealt {};
		std::array<int, 8> AimedDealtFrame {};
		// A partner betrayed us: the coalition ends at the next update (the truce at once).
		bool BetrayedPending { false };
		// An ex-partner is not our main target before this frame (no betrayal).
		std::array<int, 8> ExPartnerUntil {};
		// Grudge per house: damage it did to us (aimed in full, other a quarter), x0.98 per update,
		// +3000 per betrayal. Feeds the "worst suits us" target choice.
		std::array<int, 8> Grudge {};
		// Betrayals by each house that we suffered or saw ourselves (never decays): distrust.
		std::array<int, 8> TraitorScore {};
		// Last frame we saw houses a and b both hitting the same third house (inferred partners),
		// and the last betrayal of b by a that we witnessed.
		std::array<std::array<int, 8>, 8> SideBySideFrame {};
		std::array<std::array<int, 8>, 8> WitnessedFrame {};
		// Most harvesters of each house seen in sight at once, slowly forgotten (poorest choice).
		std::array<int, 8> HarvestersSeen {};
		// Where we saw other houses' armies hitting the target (merged within 8 cells).
		std::vector<NeuralCoalitionFront> Fronts {};
		// Per house: last known base (centroid of its known structures), how many we knew, and the
		// most we ever knew at once.
		std::array<CellStruct, 8> LastKnownBase {};
		std::array<int, 8> LastKnownBuildings {};
		std::array<int, 8> PeakKnownBuildings {};
		// Fighting seen, [victim][attacker]: value of attacker units seen in sight next to the
		// victim's known structures, decayed every update.
		std::array<std::array<int, 8>, 8> FightSeen {};
		// Houses whose defeat we already looked at, and the frame of the last one.
		uint32_t DefeatSeenMask { 0 };
		int LastDefeatFrame { 0 };
		// Stalemate samples every 2700 frames (oldest first): known structures of the other houses,
		// our money and the waves we started so far.
		std::array<int, 5> FrontSamples {};
		std::array<int, 5> MoneySamples {};
		std::array<int, 5> WaveSamples {};
		int SampleCount { 0 };
		int LastSampleFrame { -100000 };
		bool Stalemate { false };
		// Two other houses at war: hold our attacks on them while it lasts, then strike the one it
		// left weakest (PostDuelTarget until PostDuelUntil).
		int DuelA { -1 };
		int DuelB { -1 };
		int DuelQuietUpdates { 0 };
		std::array<int, 2> DuelStartStrength {};
		std::array<int, 2> DuelStartBuildings {};
		int PostDuelTarget { -1 };
		int PostDuelUntil { -1 };
		// A house about to fall: reach its base before its aggressor.
		int DyingTarget { -1 };
		int DyingUntil { -1 };
		// We are clearly the strongest: do not provoke a coalition; the nearest known base stays fair game.
		bool LowProfile { false };
		int LowProfileNearest { -1 };
		int LastUpdateFrame { -100000 };
		int LastFullFrame { -100000 };
		// Telemetry: frames in coalition, joins, declines (all, as second strongest, as strongest),
		// truces started and ended, waves at the target / partners / others while on / with none on
		// / at a dying house, the last target and the last decline reason (NeuralCoalitionDecline),
		// joins by trigger and by target choice, coalition and truce ends by reason, betrayals
		// suffered and witnessed, partners refused for distrust, pact start work (plans cancelled,
		// units recalled, redirected, partner targets cleared later), grace damage ignored,
		// superweapon launches cancelled, low profile / duel hold frames, post-duel strikes, dying
		// house rushes and updates in stalemate.
		int ActiveFrames { 0 };
		int Joined { 0 };
		int Declined { 0 };
		int DeclinedSecond { 0 };
		int DeclinedStrongest { 0 };
		int Truces { 0 };
		int Revoked { 0 };
		int TargetWaves { 0 };
		int PartnerWaves { 0 };
		int OtherWaves { 0 };
		int FreeWaves { 0 };
		int DyingWaves { 0 };
		int LastTarget { -1 };
		int LastDeclineReason { 0 };
		std::array<int, 5> JoinsByReason {};
		std::array<int, 5> JoinsByChoice {};
		std::array<int, static_cast<size_t>(NeuralPactEnd::Count)> PactEnds {};
		std::array<int, static_cast<size_t>(NeuralTruceEnd::Count)> TruceEnds {};
		int BetrayalsSuffered { 0 };
		int BetrayalsWitnessed { 0 };
		int DistrustRefusals { 0 };
		int PlansCancelled { 0 };
		int UnitsRecalled { 0 };
		int UnitsRedirected { 0 };
		int PartnerTargetsCleared { 0 };
		int GraceDamageIgnored { 0 };
		int SuperweaponsCancelled { 0 };
		int LowProfileFrames { 0 };
		int DuelHoldFrames { 0 };
		int PostDuelStrikes { 0 };
		int DyingRushes { 0 };
		int StalemateUpdates { 0 };
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

		// Spaced sightings of enemy ground units near our or an ally's base (same layout and
		// extrapolation as the air tracks), read by the Base Threat Intercept plan.
		std::vector<NeuralAirThreatTrack> GroundThreatTracks {};

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
		// Airborne raid transports nobody managed to board: UniqueID -> frame they may be used
		// again (key access only).
		std::unordered_map<DWORD, int> RaidTransportBanUntil {};
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
		// Every ground unit of ours: the cell it stands on and since when, without firing (IsFootHeldStill).
		std::unordered_map<DWORD, NeuralStuckTrack> FootProgress {};
		// Our flying ground units (jumpjets): the cell they hover over and since when (UpdateHoveringFlyers).
		std::unordered_map<DWORD, NeuralStuckTrack> HoverTracks {};
		// Aircraft attack watchdog (UpdateStuckUnits), keyed the same way.
		std::unordered_map<DWORD, NeuralAirStallTrack> AirStallTracks {};
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
		// Dry economy (UpdateEconomyCrisis): the last frame a gatherer of ours brought money in (-1:
		// never), whether our gatherers have brought nothing for a long time while we are broke (the
		// fields are gone), and the last sale it made.
		int LastHarvestIncomeFrame { -1 };
		bool DryEconomyActive { false };
		int LastDrySaleFrame { -100000 };
		int LastHarvesterBalanceFrame { -100000 };
		// Expert 0 work (UpdateEconomyCrisis, every 150 frames): the selector prior from the economy
		// trouble there is now, and what it is made of. Own state and current sight only.
		int EconomyTrouble { 0 };
		int LowPowerSinceFrame { -1 };
		int HarvestersUnderFire { 0 };
		int StarvedRefineries { 0 };
		bool StorageNearlyFull { false };
		// Harvesters sent home from a seen threat (UniqueID -> frame of the order), so one is not
		// ordered again every AI tick.
		std::unordered_map<DWORD, int> HarvesterRetreatFrames {};
		// Expert 6 work (UpdateResourceClaim, every 300 frames): value of the free ore fields this
		// house knows and can take now, and whether the best one is beyond the crawl's reach.
		int ExpansionOpportunity { 0 };
		bool ClaimNeedsMCV { false };
		// Defense ledger (NeuralAI.Defense.inl): what our defenses cost and what they paid back, per
		// match. Kills are counted per defense UniqueID (keyed lookups only, never iterated).
		int DefenseSpent { 0 };
		int DefensesBuilt { 0 };
		int DefensesLost { 0 };
		int DefensesLostNoKill { 0 };
		int DefenseLostValue { 0 };
		int DefenseKills { 0 };
		int DefenseKillValue { 0 };
		// Value of our towers lost lately (x3/4 every 900 frames): rebuilding what keeps dying is spending too.
		int RecentDefenseLossValue { 0 };
		int RecentDefenseLossDecayFrame { 0 };
		std::unordered_map<DWORD, int> DefenseKillsById {};
		std::vector<NeuralWastedDefenseSpot> WastedDefenseSpots {};
		// Defense budget and Expert 7 pressure (RefreshDefenseBudget, every 60 frames): value of our
		// defenses and armed army, the defense value we allow, the payback percent of our defenses,
		// the seen enemy artillery (value share, longest ground range in cells) and the pressure that
		// grades Expert 7 in the selector.
		int DefenseBudgetFrame { -100000 };
		int OwnDefenseValue { 0 };
		int OwnArmyValue { 0 };
		int DefenseValueCap { 0 };
		int DefenseEfficiencyPct { 100 };
		int SeenArtilleryShare { 0 };
		int SeenArtilleryRangeCells { 0 };
		int DefensePressure { 0 };
		// Longest ground reach (cells) of the towers we can build now, refreshed every 900 frames.
		int BestTowerRangeCells { 0 };
		int BestTowerRangeFrame { -100000 };
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
		// Expert 3 diplomacy: tacit coalition, truces, grudges, traitors (NeuralAI.Diplomacy.inl).
		NeuralCoalitionState Coalition {};
		int LastStoneCheckFrame { -100000 };
		int LastIdleFlyerCheckFrame { -100000 };
		// Transports in Mission::Unload: UniqueID -> (cell, first frame seen there) (UpdateStuckUnloads).
		std::unordered_map<DWORD, std::pair<CellStruct, int>> UnloadWatch;
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
		// Bank governor (UpdateBankGovernor, every 450 frames): credits earned per 1000 frames
		// (smoothed), the earned total and frame of the last sample, and while the bank beyond
		// the reserve outgrows what the production queues can spend, the factory count wanted
		// per queue (BankQueue order: vehicle, infantry, aircraft, naval).
		int BankIncomeFrame { -100000 };
		int BankIncomeEarned { 0 };
		int BankIncomeRate { -1 };
		bool BankGovernorActive { false };
		int BankSpendCapacity { 0 };
		int BankSpendNeeded { 0 };
		// Credits held beyond the reserve at the last sample (0 when none): army caps grow with it.
		int BankSurplus { 0 };
		std::array<int, 4> BankWantedFactories {};
		// AI ticks since the last governor sample at which each queue (BankQueue order) had a
		// factory of its kind, and those at which one of them was producing.
		std::array<int, 4> BankQueueTicks {};
		std::array<int, 4> BankQueueBusyTicks {};
		// A known enemy base stands on land we cannot walk to from home (features[21] at the last
		// production tick): harassment from range is how an offensive bank presses it.
		bool EnemyAcrossWater { false };
		// Islands (movement zone -> frame until) where an expansion deployer found no room for its
		// building: the island search skips them until then.
		std::map<int, int> NoRoomIslandUntil;
		// The enemy has no building left that counts and still has ships or submarines: until this
		// frame the house builds and sends sonar ships and ships that hit under water.
		int NavalRemnantUntil { -1 };
		// An enemy submarine (Naval, Cloakable) was seen within 30 cells of our base: until this frame
		// the house builds and sends ships that hit under water or carry sonar.
		int SubThreatUntil { -1 };
		// The unit on its way to a repair station to be sold for emergency money (0 when none).
		DWORD DepotSaleUnit { 0 };
		// Our refineries and those with fewer than 6 ore cells left within 12 cells (sampled every 2
		// cells), refreshed every 900 frames (UpdateRefineryDepletion).
		int RefineriesChecked { 0 };
		int RefineriesDepleted { 0 };
		int RefineryDepletionFrame { -100000 };
		// Deployed siege structures of ours: UniqueID -> first frame seen deployed (they stay at least
		// 450 frames before packing up again).
		std::unordered_map<DWORD, int> DeployedSiegeSince;
		// First frame without any ConYard of ours (-1 while we have one).
		int NoYardSinceFrame { -1 };
		// Our fields ran dry and an expansion MCV is wanted: what it costs (0 when not saving for one).
		// UpdateEconomyCrisis holds the other production until the credits reach it.
		int ExpansionSaveTarget { 0 };
		// Island ore outlook (UpdateIslandOreOutlook, every 900 frames): credits of ore left in the
		// fields of the landmasses we hold (a ConYard or refinery there), what they lost per 900
		// frames (smoothed; IslandHubZoneKey names the landmasses and field blocks of that sample),
		// how many frames the rest lasts at that pace, the nearest free field on another landmass,
		// and whether an MCV must go there now, while the credits for it and its ride are still
		// there. LastIslandSaleFrame paces the sales that pay for that MCV once the home ore is gone.
		int IslandOutlookFrame { -100000 };
		int IslandHomeOre { 0 };
		int IslandOreRunway { -1 };
		int IslandOreDrain { -1 };
		unsigned int IslandHubZoneKey { 0 };
		CellStruct IslandExpansionField { CellStruct::Empty };
		bool IslandExpansionUrgent { false };
		int IslandExpansionLogFrame { -100000 };
		int LastIslandSaleFrame { -100000 };
		int IslandMcvLogFrame { -100000 };
		// The buildings of our MCV's Prerequisite list we still lack while that expansion is urgent.
		std::vector<int> IslandMcvPrerequisites {};
		// The other economic investments the production wants and cannot pay yet (0 when none): a
		// harvester while we have fewer than refineries, a refinery for a newly reached field.
		int HarvesterSaveTarget { 0 };
		int RefinerySaveTarget { 0 };
		// Economy hold bookkeeping: first frame of the current hold, and no hold before this frame (a
		// hold that bought nothing for 3600 frames rests 1800).
		int EconomyHoldSince { -1 };
		int EconomyHoldPauseUntil { -1 };
		// Enemy units seen cloaked, cloakable or under water: UniqueID -> (cell, frame), for
		// superweapons that reveal stealth (SW.AITargeting=Stealth).
		std::unordered_map<DWORD, std::pair<CellStruct, int>> StealthSightings;
		// Plan members by UniqueID (UpdateStrategicPlans); dropped 600 frames after the last sight.
		std::unordered_map<DWORD, NeuralPlanMemberTrace> PlanMemberTraces;
		// Capital groups by domain (0 ground, 1 air, 2 sea) and the capital units they hold back from
		// waves and lone strikes (UpdateCapitalGroups).
		std::array<NeuralCapitalGroup, 3> CapitalGroups {};
		std::unordered_set<DWORD> CapitalHeld {};
		// ConYard count wanted by the governor for the building queue (0 when none).
		int BankWantedYards { 0 };
		// Packed-up yard MCVs sent across water by transport (by UniqueID, in order).
		std::vector<DWORD> FerryingMcvs {};
		// Ferry wait safety net (DispatchFootToTargetOrTransport): since when a unit has been asking
		// for a crossing, and until when it may not ask again after giving up. Keyed lookups only.
		std::unordered_map<DWORD, int> FerryWaitSince {};
		std::unordered_map<DWORD, int> FerryBlockedUntil {};
		// Ferry loading (user: divide the work by what fits): passengers reserved by a transport
		// parked on a pickup shore, up to its free room by size. Passenger UniqueID -> transport
		// UniqueID and the frame it was reserved. Ordered map: walked in a deterministic order.
		struct FerryReservation
		{
			DWORD Transport { 0 };
			int Frame { 0 };
		};
		std::map<DWORD, FerryReservation> FerryReserved {};
		// Picks the engine never starts (match #7132 / training: 66000-290000 credits, five war
		// factories idle for thousands of frames, the AI picking the same type every tick): the pick
		// of a queue that stays empty 900 frames with money and factories is banned 5400 frames.
		// Key: queue (1 vehicles, 2 infantry) * 100000 + type index -> frame the ban ends.
		std::map<int, int> ProductionBanUntil {};
		std::array<int, 3> ProductionPickIdx { -1, -1, -1 };
		std::array<int, 3> ProductionPickSince { 0, 0, 0 };
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
		// Ally site plans (Ally Backup, Ally Forward Outpost): the next building we want next to an
		// ally's base, the ally ConYard it belongs to and the cell its side of that base starts at.
		int AllySiteBuildType { -1 };
		CellStruct AllySiteCell { CellStruct::Empty };
		CellStruct AllySiteYardCell { CellStruct::Empty };
		int AllySiteUntilFrame { -1 };
		// Last frame the wanted building found no room at the ally site (it was placed at home).
		int AllySiteFailedFrame { -1 };
		// Cell found for the wanted building before paying for it (placement tries it first), the
		// type it was found for and when it was last checked.
		CellStruct AllySiteReservedCell { CellStruct::Empty };
		int AllySiteReservedType { -1 };
		int AllySiteReservedFrame { -1 };
		// An ally building under attack right now that our next armed defense goes to (production).
		CellStruct AllyDefenseCell { CellStruct::Empty };
		int AllyDefenseUntilFrame { -1 };
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
		// Forward staging hold: first frame of the current hold, and no hold before this frame (a hold
		// that never gathered the wave in 2700 frames rests 2700).
		int StagingHoldSince { -1 };
		int StagingHoldPauseUntil { -1 };
		// Last frame the jam breaker cleared a block of our units, last frame it looked for one, last
		// frame bridge and tunnel mouths were cleared (UpdateStuckUnits).
		int LastJamBreakFrame { -1 };
		int LastJamCheckFrame { -1 };
		int LastMouthClearFrame { -1 };
		int LastJamDiagFrame { -1 };
		bool JamMapLogged { false };
		int FailedApproachDecayFrame { 0 };
		std::vector<NeuralGroundLossTrack> GroundLossTracks {};
		int HomeThreatPeak { 0 };
		int HomeThreatPeakFrame { 0 };
		int CounterattackUntilFrame { -1 };
		bool PressureMode { false };
		int FocusEnemyIndex { -1 };
		int TacticsRefreshFrame { -100000 };

		// Plans 23..38: staggered retreats waiting their turn, combo boosts
		// per plan and the frame each ready offensive superweapon became ready (SW + army, point 37).
		std::vector<NeuralDelayedRetreat> DelayedRetreats {};
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
			this->GroundThreatTracks.clear();
			this->SeenEnemyBuildings.clear();
			this->ScoutedStartMask = 0;
			this->EnemySightings.clear();
			this->OpponentProfile = NeuralOpponentProfile {};
			this->Plans.fill(NeuralPlanInstance {});
			this->PlanNextAttemptFrame.fill(0);
			this->PlanMembers.clear();
			this->RaidTransportBanUntil.clear();
			this->BlackoutUntilFrame = -1;
			this->BloodyBeaches.clear();
			this->AirLossTracks.clear();
			this->AirDangerSpots.clear();
			this->StuckTracks.clear();
			this->FootProgress.clear();
			this->HoverTracks.clear();
			this->AirStallTracks.clear();
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
			this->LastHarvestIncomeFrame = -1;
			this->DryEconomyActive = false;
			this->LastDrySaleFrame = -100000;
			this->LastHarvesterBalanceFrame = -100000;
			this->EconomyTrouble = 0;
			this->LowPowerSinceFrame = -1;
			this->HarvestersUnderFire = 0;
			this->StarvedRefineries = 0;
			this->StorageNearlyFull = false;
			this->HarvesterRetreatFrames.clear();
			this->ExpansionOpportunity = 0;
			this->ClaimNeedsMCV = false;
			this->DefenseSpent = 0;
			this->DefensesBuilt = 0;
			this->DefensesLost = 0;
			this->DefensesLostNoKill = 0;
			this->DefenseLostValue = 0;
			this->DefenseKills = 0;
			this->DefenseKillValue = 0;
			this->RecentDefenseLossValue = 0;
			this->RecentDefenseLossDecayFrame = 0;
			this->DefenseKillsById.clear();
			this->WastedDefenseSpots.clear();
			this->DefenseBudgetFrame = -100000;
			this->OwnDefenseValue = 0;
			this->OwnArmyValue = 0;
			this->DefenseValueCap = 0;
			this->DefenseEfficiencyPct = 100;
			this->SeenArtilleryShare = 0;
			this->SeenArtilleryRangeCells = 0;
			this->DefensePressure = 0;
			this->BestTowerRangeCells = 0;
			this->BestTowerRangeFrame = -100000;
			this->FfaEnemyBonus.fill(0);
			this->FfaThreatScale.fill(100);
			this->LastFfaTargetFrame = -100000;
			this->ArmyContactOwn = 0;
			this->ArmyContactEnemy = 0;
			this->LastArmyContactFrame = -100000;
			this->RepairTripFrames.clear();
			this->Coalition = NeuralCoalitionState {};
			this->LastStoneCheckFrame = -100000;
			this->LastIdleFlyerCheckFrame = -100000;
			this->UnloadWatch.clear();
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
			this->BankIncomeFrame = -100000;
			this->BankIncomeEarned = 0;
			this->BankIncomeRate = -1;
			this->BankGovernorActive = false;
			this->BankSpendCapacity = 0;
			this->BankSpendNeeded = 0;
			this->BankSurplus = 0;
			this->BankWantedFactories.fill(0);
			this->BankQueueTicks.fill(0);
			this->BankQueueBusyTicks.fill(0);
			this->EnemyAcrossWater = false;
			this->NoRoomIslandUntil.clear();
			this->NavalRemnantUntil = -1;
			this->SubThreatUntil = -1;
			this->DepotSaleUnit = 0;
			this->RefineriesChecked = 0;
			this->RefineriesDepleted = 0;
			this->RefineryDepletionFrame = -100000;
			this->DeployedSiegeSince.clear();
			this->NoYardSinceFrame = -1;
			this->ExpansionSaveTarget = 0;
			this->IslandOutlookFrame = -100000;
			this->IslandHomeOre = 0;
			this->IslandOreRunway = -1;
			this->IslandOreDrain = -1;
			this->IslandHubZoneKey = 0;
			this->IslandExpansionField = CellStruct::Empty;
			this->IslandExpansionUrgent = false;
			this->IslandExpansionLogFrame = -100000;
			this->LastIslandSaleFrame = -100000;
			this->IslandMcvLogFrame = -100000;
			this->IslandMcvPrerequisites.clear();
			this->HarvesterSaveTarget = 0;
			this->RefinerySaveTarget = 0;
			this->EconomyHoldSince = -1;
			this->EconomyHoldPauseUntil = -1;
			this->StealthSightings.clear();
			this->PlanMemberTraces.clear();
			this->CapitalGroups = {};
			this->CapitalHeld.clear();
			this->BankWantedYards = 0;
			this->FerryingMcvs.clear();
			this->FerryWaitSince.clear();
			this->FerryBlockedUntil.clear();
			this->FerryReserved.clear();
			this->ProductionBanUntil.clear();
			this->ProductionPickIdx = { -1, -1, -1 };
			this->ProductionPickSince = { 0, 0, 0 };
			this->FreeFieldAnchorDist = 9999;
			this->FreeFieldFrame = -100000;
			this->LastAirDropAbortFrame = -100000;
			this->IslandHoldCell = CellStruct::Empty;
			this->IslandHoldUntilFrame = -1;
			this->ForwardRepairCell = CellStruct::Empty;
			this->ForwardRepairUntilFrame = -1;
			this->AllySiteBuildType = -1;
			this->AllySiteCell = CellStruct::Empty;
			this->AllySiteYardCell = CellStruct::Empty;
			this->AllySiteUntilFrame = -1;
			this->AllySiteFailedFrame = -1;
			this->AllySiteReservedCell = CellStruct::Empty;
			this->AllySiteReservedType = -1;
			this->AllySiteReservedFrame = -1;
			this->AllyDefenseCell = CellStruct::Empty;
			this->AllyDefenseUntilFrame = -1;
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
			this->StagingHoldSince = -1;
			this->StagingHoldPauseUntil = -1;
			this->LastJamBreakFrame = -1;
			this->LastJamCheckFrame = -1;
			this->LastMouthClearFrame = -1;
			this->LastJamDiagFrame = -1;
			this->JamMapLogged = false;
			this->FailedApproachDecayFrame = 0;
			this->GroundLossTracks.clear();
			this->HomeThreatPeak = 0;
			this->HomeThreatPeakFrame = 0;
			this->CounterattackUntilFrame = -1;
			this->PressureMode = false;
			this->FocusEnemyIndex = -1;
			this->TacticsRefreshFrame = -100000;
			this->DelayedRetreats.clear();
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

		// The landing a loaded transport committed to once within 14 cells of it: kept to the end
		// (user: transports turned away 10-15 cells from the shore when his troops came near, the
		// landing being re-picked every tick with enemy units in sight weighing -2400).
		CellStruct CommittedLanding { CellStruct::Empty };
		// First frame a combat cargo sat loaded but short of three quarters of the room on its
		// pickup shore.
		int UnderfilledSince { -1 };
		// Health at the last tick and the last frame it dropped (under fire now).
		int LastHealth { -1 };
		int HitFrame { -100000 };
		// Why the cargo was let go the last time it was decided on the pickup shore, as bit flags
		// (departure log).
		int SailReason { 0 };
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

	// A passenger reserved by one of our transports loading on a pickup shore: it walks to that
	// transport and boards it, no other job takes it meanwhile.
	constexpr int FerryReservationFrames = 1800;

	bool IsFerryReserved(const HouseClass* pHouse, DWORD uid)
	{
		auto const& reserved = GetMacroState(pHouse).FerryReserved;
		auto const it = reserved.find(uid);

		return it != reserved.end() && Unsorted::CurrentFrame - it->second.Frame < FerryReservationFrames
			&& Unsorted::CurrentFrame >= it->second.Frame;
	}

	// A vehicle (queue 1) or infantry (queue 2) type the engine did not start for 900 frames.
	bool IsProductionBanned(const HouseClass* pHouse, int queue, int typeIndex)
	{
		auto const& bans = GetMacroState(pHouse).ProductionBanUntil;
		auto const it = bans.find(queue * 100000 + typeIndex);

		return it != bans.end() && Unsorted::CurrentFrame < it->second;
	}

	// Tacit coalitions (Expert 3): how many different houses are coalition targets right now, the
	// peak this match, and the betrayals each house committed as its victims saw them. Telemetry
	// only, it never feeds a decision.
	int CoalitionConcurrentNow { 0 };
	int CoalitionConcurrentPeak { 0 };
	int CoalitionConcurrentSerial { -1 };
	std::array<int, 32> BetrayalsCommitted {};

	int CoalitionTargetOf(HouseClass* pHouse)
	{
		return pHouse ? GetMacroState(pHouse).Coalition.Target : -1;
	}

	// A house in a truce with us now: seen fighting our coalition target recently, and the truce
	// has not ended (betrayal, collateral).
	bool IsTacitTrucePartner(HouseClass* pHouse, int idx)
	{
		if (!pHouse || idx < 0 || idx >= 8)
			return false;

		auto const& coal = GetMacroState(pHouse).Coalition;

		if (coal.Target < 0 || idx == coal.Target || (coal.RevokedMask & (1u << idx)) != 0 || coal.TruceSinceFrame[idx] < 0)
			return false;

		return Unsorted::CurrentFrame - coal.PartnerSeenFrame[idx] <= 5400;
	}

	// Inside the grace window at the start of our truce with that house.
	bool IsInPactGrace(HouseClass* pHouse, int idx)
	{
		if (!pHouse || idx < 0 || idx >= 8)
			return false;

		auto const& coal = GetMacroState(pHouse).Coalition;
		const int since = coal.TruceSinceFrame[idx];

		return coal.Target >= 0 && since >= 0 && (coal.RevokedMask & (1u << idx)) == 0
			&& Unsorted::CurrentFrame - since < PactGraceFrames && Unsorted::CurrentFrame >= since;
	}

	// Whether another house's army was seen hitting our coalition target within range of a cell in
	// the last 2700 frames: that front is theirs, ours goes elsewhere.
	bool IsCoalitionFrontNear(HouseClass* pHouse, const CellStruct& cell, int range)
	{
		if (!pHouse)
			return false;

		auto const& coal = GetMacroState(pHouse).Coalition;

		if (coal.Target < 0)
			return false;

		const int now = Unsorted::CurrentFrame;

		return std::any_of(coal.Fronts.begin(), coal.Fronts.end(), [&](const NeuralCoalitionFront& front)
			{
				return now - front.Frame <= 2700 && std::max(std::abs(front.Cell.X - cell.X), std::abs(front.Cell.Y - cell.Y)) <= range;
			});
	}

	// Expert 3 diplomacy bias for picking a house to fight (main enemy, or a wave's building with
	// wave set): coalition target first (the wave bonus with its fronts is added by the caller),
	// truce partners never, an ex-partner not for a while, two houses at war left alone until it is
	// over and then the one it left weakest, a dying house before its aggressor gets there; while we
	// lead by far, no far adventures against houses that leave us alone.
	int DiplomacyTargetBias(HouseClass* pHouse, int idx, bool wave)
	{
		if (!pHouse || idx < 0 || idx >= 8)
			return 0;

		auto const& coal = GetMacroState(pHouse).Coalition;
		const int now = Unsorted::CurrentFrame;
		const bool hurtUsRecently = now - coal.HurtByFrame[idx] <= 1800;
		int bias = 0;

		if (idx == coal.Target)
			bias += wave ? 0 : 700;
		else if (IsTacitTrucePartner(pHouse, idx))
			return -2000;

		if (coal.ExPartnerUntil[idx] > now && coal.TraitorScore[idx] == 0)
			bias -= 600;

		if ((idx == coal.DuelA || idx == coal.DuelB) && idx != coal.Target && !hurtUsRecently)
			bias -= wave ? 400 : 300;

		if (idx == coal.PostDuelTarget && now < coal.PostDuelUntil)
			bias += 500;

		if (idx == coal.DyingTarget && now < coal.DyingUntil)
			bias += wave ? 800 : 600;

		if (wave && coal.LowProfile && idx != coal.LowProfileNearest && idx != coal.Target
			&& now - coal.HurtByFrame[idx] > 5400)
		{
			bias -= 300;
		}

		return bias;
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

	// Fog-fair strength of one enemy house as another house knows it (UpdateEnemyStrength): the
	// remembered army (sightings weighted by age), the part of it seen lately far from that
	// house's known base, the value of its units we watched die lately, its known defenses and
	// the centre of its known buildings.
	struct NeuralEnemyStrength
	{
		int Army { 0 };
		int ArmyAway { 0 };
		int RecentLosses { 0 };
		int DefenseValue { 0 };
		int KnownBuildings { 0 };
		CellStruct BaseCenter { CellStruct::Empty };
	};

	struct NeuralEnemyLoss
	{
		int Frame { 0 };
		int OwnerIndex { -1 };
		int Value { 0 };
	};

	// Wave timing (NeuralAI.WaveTiming.inl): the strength estimate per enemy house index, the
	// sighted enemy units we watched die (keyed lookups only) and their losses, our army value,
	// the advantage against the strongest known enemy (percent, read by feature 99 and the main
	// assault plans) and whether the main wave is being held for strength.
	struct NeuralWaveTimingState
	{
		std::array<NeuralEnemyStrength, 32> Enemies {};
		std::unordered_set<DWORD> SeenDead {};
		std::vector<NeuralEnemyLoss> Losses {};
		int LastUpdateFrame { -100000 };
		int OwnArmyValue { 0 };
		int AdvantageRatio { 100 };
		bool Holding { false };
		int HoldSinceFrame { -1 };
	};

	std::array<NeuralWaveTimingState, 32> WaveTimingStates {};

	NeuralWaveTimingState& GetWaveTimingState(const HouseClass* pHouse)
	{
		return WaveTimingStates[pHouse ? std::clamp(pHouse->ArrayIndex, 0, 31) : 0];
	}

} // namespace

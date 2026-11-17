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
		// Where it has stood since when (it moved no more than EnemyStandCells from there): a force
		// gathering stands, one on its way does not (Preemptive Strike).
		CellStruct StandCoords { CellStruct::Empty };
		int StandSinceFrame { 0 };
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

	// What a failed plan teaches about its target: a group of this value was not enough there.
	struct NeuralPlanTargetLesson
	{
		int Strength { 0 };
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
		// Cells of the members (Naval Strike Group: a firing cell for each shelling ship, apart).
		std::vector<CellStruct> Cells {};
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
		// The house that owns the target building (-1 none), for the lesson a beaten raid leaves.
		int TargetOwner { -1 };
		// Airborne Front: its transport has parked by its soldiers to take them in.
		bool BoardingParked { false };
		// Airborne Front: the frame its transport first parked by its soldiers (-1 not yet).
		int BoardingParkedFrame { -1 };
		// Airborne Front: how many were aboard at the last tick of its unloading.
		int UnloadAboard { 0 };
		// Preemptive Strike: where the enemy force was gathering when the plan started. Airborne
		// Front: the cell its flying transport was last sent to land on.
		CellStruct AnchorCell { 0, 0 };
		int TargetSeenFrame { 0 };
		// Per member: a frame (Pickets: until when a sentry holds back, when it last saw something).
		std::vector<int> MemberFrames {};
		// Combat value of the enemy units its members destroyed (NeuralAI::OnTechnoDestroyed).
		int KillValue { 0 };
		// Bait Ambush: the bait's health in percent when it set out to fire at the building.
		int LureHealth { 0 };
		// Counter Squad: the enemy house and force category (EnemyForceCategory) it goes after.
		int ForceOwner { -1 };
		int ForceCategory { -1 };
	};

	struct NeuralStuckTrack
	{
		CellStruct Cell { CellStruct::Empty };
		int SinceFrame { 0 };
	};

	// Where a wave sent a unit, the ground zone of that cell and when (KeptWaveGoal).
	struct NeuralWaveGoal
	{
		CellStruct Cell { CellStruct::Empty };
		int Zone { -1 };
		int Frame { 0 };
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
	// At most two truce partners that were only seen fighting the target; the houses that stand
	// against the same target by a call are partners beyond that limit.
	constexpr int PactMaxPartners = 2;
	// A call to arms (another house already stands against the target) adds this to the situation
	// of the house that weighs joining: a pact that exists is worth more than one to be started.
	constexpr int PactCallBonus = 25;
	// A pact has no fixed length (user: a pact lives while it pays, not for a turn of a few
	// minutes). While it lasts the house weighs it again and leaves it for one of two reasons:
	// - another house became the danger: its army at our base, or hitting us on purpose within
	//   PactThreatFrames, while the target does neither, for PactRethinkUpdates updates in a row
	//   (never in the first PactRethinkMinFrames of the pact);
	// - nothing is done about the target: for PactIdleFrames we sent no wave at it, saw nobody
	//   fight it and it lost none of the structures we know.
	constexpr int PactRethinkMinFrames = 1800;
	constexpr int PactThreatFrames = 450;
	constexpr int PactRethinkUpdates = 6;
	constexpr int PactIdleFrames = 9000;
	// No new coalition against a house for this long after one against it ended.
	constexpr int PactTargetCooldownFrames = 4500;
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
	// Free-for-all houses by what we know of them, with no preset team in the match and two or more
	// living enemies. Income: 3 per known refinery and 1 per harvester seen at once. Production:
	// LeaderFactoryWeight per known factory of units, LeaderIncomeWeight per point of income and the
	// value of the army seen (ours at two thirds, as the others see only part of it). A runaway leader
	// is an enemy that produces LeaderLeadPercent of the next house (measured only: it weighs nothing
	// yet, to see first how often the coalition against a leader answers it). A house is judged once
	// we know PoorMinKnownBuildings of its structures. The poor target is the poorest enemy when its
	// income is at most PoorShareOfMeanPercent of the mean of the other enemies, or an enemy whose
	// income fell to PoorLostPercent of the most we knew of it, at least PoorLostMinPeak: it cannot
	// replace what it loses and falls sooner. It gets PoorTargetBias in the main enemy choice and in
	// the waves, on top of the preference for finishing the weakest, which stays the usual choice.
	constexpr int LeaderFactoryWeight = 100;
	constexpr int LeaderIncomeWeight = 30;
	constexpr int LeaderLeadPercent = 150;
	constexpr int PoorMinKnownBuildings = 3;
	constexpr int PoorShareOfMeanPercent = 60;
	constexpr int PoorLostPercent = 40;
	constexpr int PoorLostMinPeak = 6;
	constexpr int PoorTargetBias = 250;
	// Main enemy log: the first changes of each house in a match.
	constexpr int MainEnemyLogLimit = 60;
	// The main enemy is held for MainEnemyHoldFrames once chosen. Before that another house takes
	// over only with a priority MainEnemySwitchMargin above it, or when it is urgent and the held one
	// is not: it hit us within MainEnemyUrgentFrames, its units were seen within MainEnemyBaseCells of
	// home in the last 150 frames, or it is dying (weak enough to finish, or the dying house of the
	// diplomacy).
	constexpr int MainEnemyHoldFrames = 2250;
	constexpr int MainEnemySwitchMargin = 300;
	constexpr int MainEnemyUrgentFrames = 450;
	constexpr double MainEnemyBaseCells = 18.0;

	// Why a coalition ended (telemetry and log), and why a truce with one partner ended.
	enum class NeuralPactEnd : int
	{
		Betrayal = 0,
		Weakened = 1,
		TargetGone = 2,
		Rethought = 3,
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
		// fragile, 3 poorest, 4 traitor) and since when.
		int Target { -1 };
		int Reason { 0 };
		int TargetChoice { 0 };
		int SinceFrame { -1 };
		// Weighing the pact again: updates in a row with another house as the danger, and the last
		// frame something was done about the target with our waves at it and its known structures
		// as they were then.
		int RethinkUpdates { 0 };
		int ProgressFrame { -1 };
		int ProgressWaves { 0 };
		int ProgressBuildings { 0 };
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
		// Free-for-all leader and poor target (UpdateTacitCoalition), by what we know of the houses.
		// The leader is an enemy that produces LeaderLeadPercent of the next house (-1 none) and its
		// lead in percent: measured only, it weighs nothing in the choice yet. The poor target is the
		// clearly poorest enemy or one that lost its economy (DiplomacyTargetBias), and whether it lost
		// it; IncomePeak the most income we ever knew of each house. Telemetry: frames with a runaway
		// enemy leader, of those with a coalition of any house against it, and with a poor target.
		int ProductionLeader { -1 };
		int ProductionLeadPercent { 0 };
		int PoorTarget { -1 };
		bool PoorTargetLost { false };
		std::array<int, 8> IncomePeak {};
		int LeaderFrames { 0 };
		int LeaderCoalitionFrames { 0 };
		int PoorTargetFrames { 0 };
		// Main enemy hold (Micro): our last main enemy choice (the engine also writes EnemyHouseIndex)
		// and since when, and telemetry of its changes and of the ticks a held choice kept it.
		int MainEnemy { -1 };
		int MainEnemySinceFrame { -100000 };
		int MainEnemyChanges { 0 };
		int MainEnemyHeld { 0 };
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
		std::array<int, 6> JoinsByReason {};
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

	// Transport operations (user: a removal company apart from the client that booked the move,
	// NeuralAI.Operations.inl). A client asks to take units to an address on another landmass; the
	// operation gathers them on a beach of their land, brings the transports it needs (idle ones
	// first, the shortfall from the factories), loads them, crosses, sets them down on the land of
	// the address, hands them back to their mission and fetches the ones left behind.
	enum class NeuralOpClient : uint8_t
	{
		Wave,
		Plan,
		Team,
		Economy,
		Strike,
		Shuttle
	};

	// What a harvester of a shuttle operation does between its rides (UpdateHarvesterShuttles): it
	// waits for one or rides (None), mines the field across water (Harvesting), or unloads at a
	// refinery at home (Unloading).
	enum class NeuralShuttleWork : uint8_t
	{
		None,
		Harvesting,
		Unloading
	};

	// What a transport of an operation is doing: on its way to the staging beach (or, a carryall, to
	// the vehicle it lifts), parked there while its passengers board, crossing loaded, or taking back
	// to the staging beach a load it could not set down.
	enum class NeuralOpLeg : uint8_t
	{
		ToStaging,
		Parked,
		Crossing,
		Returning
	};

	// A unit an operation carries: its passenger size, the address its client gave it, the transport
	// it is to board (0: none yet), whether it is aboard, and the cell the operation last sent it to
	// (another order found on it counts as an interference). A harvester of a shuttle: its work, when
	// it began, and the largest load seen on it with the frame it grew last. Progress*: where it stood
	// since when (a shuttle's harvester while a transport waits for it, any other member while it is
	// farther from the cell it was sent to than OpMemberStuckFarCells). Cleared while it is aboard.
	struct NeuralOpMember
	{
		DWORD UniqueID { 0 };
		int Size { 1 };
		CellStruct Target { CellStruct::Empty };
		DWORD Transport { 0 };
		bool Aboard { false };
		CellStruct OrderCell { CellStruct::Empty };
		NeuralShuttleWork Work { NeuralShuttleWork::None };
		int WorkFrame { -1 };
		float LoadSeen { 0.0f };
		int LoadFrame { -1 };
		CellStruct ProgressCell { CellStruct::Empty };
		int ProgressFrame { -1 };
	};

	// A transport an operation holds: its type, its leg and the frame the leg began, the cell it sets
	// its load down on, the cell a flying one comes down on to load (with when it was sent there and
	// how many cells it has tried), the progress watchdog, and the cell the operation last sent it to.
	// Route: the waypoints of its safe route still ahead (PlanOpRoute), planned at RouteFrame with
	// RouteHealth left, at a danger of RouteDanger against StraightDanger for the straight lane. Last*:
	// where it stood, its health and its load at the last tick (the loss log, the set-downs the engine
	// makes). Carried: it sailed with a load of the operation (a shuttle keeps it as its driver).
	struct NeuralOpTransport
	{
		DWORD UniqueID { 0 };
		const TechnoTypeClass* Type { nullptr };
		NeuralOpLeg Leg { NeuralOpLeg::ToStaging };
		int LegFrame { 0 };
		CellStruct Landing { CellStruct::Empty };
		CellStruct ParkCell { CellStruct::Empty };
		int ParkFrame { -1 };
		int Tries { 0 };
		CellStruct ProgressCell { CellStruct::Empty };
		int ProgressFrame { -1 };
		CellStruct OrderCell { CellStruct::Empty };
		std::vector<CellStruct> Route;
		int RouteFrame { -1 };
		int RouteHealth { 0 };
		int RouteDanger { 0 };
		int StraightDanger { 0 };
		CellStruct LastCell { CellStruct::Empty };
		int LastHealth { 0 };
		int LastLoaded { 0 };
		// Its berth around the staging cell, where it parks to load, and where its passengers wait for it
		// (PickOpBerth); the danger at its landing when it sailed (a landing that grows much more
		// dangerous during the crossing is changed).
		CellStruct Berth { CellStruct::Empty };
		CellStruct BerthWait { CellStruct::Empty };
		int LandingDanger { 0 };
		bool Carried { false };
		// A carryall that leaves its vehicle on the ground because the straight way is too dangerous
		// (OpSailDangerLimit): since when (-1: not waiting), when that way was last measured, its
		// danger, and whether the die of the end of the wait was thrown (OpAbortDangerLimit).
		int HoldSince { -1 };
		int HoldCheckFrame { -1 };
		int HoldDanger { 0 };
		bool HoldRolled { false };
	};

	// Where a transport of an operation was lost (NeuralHouseMacroState::OpLossSpots).
	struct NeuralOpLossSpot
	{
		CellStruct Cell { CellStruct::Empty };
		int Frame { 0 };
		bool Airborne { false };
	};

	struct NeuralTransportOperation
	{
		int Id { 0 };
		NeuralOpClient Client { NeuralOpClient::Wave };
		int OriginZone { -1 };
		int TargetZone { -1 };
		CellStruct Target { CellStruct::Empty };
		CellStruct Staging { CellStruct::Empty };
		CellStruct Landing { CellStruct::Empty };
		// Where the members wait for their seat: behind the staging beach, off the cells the
		// transports come to.
		CellStruct Gather { CellStruct::Empty };
		// The landing the load sailing now was given, chosen with its route (ChooseOpTripLanding), and
		// the frame: the transports that sail in that tick land around it together.
		CellStruct TripLanding { CellStruct::Empty };
		int TripLandingFrame { -1 };
		int CreatedFrame { 0 };
		// First frame a transport stood parked on the staging beach for the current load (-1: none).
		int BoardingSince { -1 };
		// Last frame a member went aboard.
		int LastBoardFrame { -1 };
		// Last frame the operation had at least one transport (no transport for long: given back).
		int LastTransportFrame { 0 };
		int Trips { 0 };
		int FailedTrips { 0 };
		int Landed { 0 };
		int Lost { 0 };
		int Released { 0 };
		int Joined { 0 };
		int TransportsTaken { 0 };
		int TransportsLost { 0 };
		int Interferences { 0 };
		int InterferenceLogFrame { -1 };
		// Routes planned again during crossings, how many of them changed the route, and loads that
		// landed somewhere else than the operation's first landing (ChooseOpTripLanding).
		int Replans { 0 };
		int RouteChanges { 0 };
		int LandingMoves { 0 };
		// A load held on its beach because even its safest route was too dangerous (OpSailDangerLimit):
		// since when (-1: not held), when its route was last planned for the check, that route's danger,
		// and how many times a load was held.
		int DangerWaitSince { -1 };
		int DangerCheckFrame { -1 };
		int TripDanger { 0 };
		int DangerHolds { 0 };
		// The last time the landing of loads under way was changed for enemies gathered at it, and how
		// many times.
		int LandingSwitchFrame { -1 };
		int LandingSwitches { 0 };
		// A harvester shuttle (UpdateHarvesterShuttles): the field across water and its land, the
		// refinery at home and its land, whether the loads go home now, the frame the last turn about
		// began, the rounds brought home and the credits of ore they held.
		CellStruct Field { CellStruct::Empty };
		int FieldZone { -1 };
		CellStruct Home { CellStruct::Empty };
		int HomeZone { -1 };
		bool Homeward { false };
		int PhaseFrame { -1 };
		int Rounds { 0 };
		int ShuttleCredits { 0 };
		bool Over { false };
		const char* EndReason { "" };
		std::vector<NeuralOpMember> Members;
		std::vector<NeuralOpTransport> Transports;
	};

	// TEMP trace: a unit of a transport operation as the operation left it at the end of the last
	// tick; TraceTransportOpUnits names the part of the AI tick that changes its order.
	struct NeuralOpUnitWatch
	{
		DWORD UniqueID { 0 };
		FootClass* Foot { nullptr };
		Mission Current { Mission::None };
		Mission Mega { Mission::None };
		AbstractClass* Destination { nullptr };
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
		// Deployed slave miners (UpdateStrandedSlaveMiners): ore samples in their slaves' reach (read
		// by the economy unit cap), the frame each one ran out of ore in reach, and the moves ordered
		// since the last log line.
		std::unordered_map<DWORD, int> MinerOreSamples {};
		std::unordered_map<DWORD, int> StrandedMinerSince {};
		int MinerRelocations { 0 };
		int MinerLogFrame { -1 };
		// Harvest income since the last income log line: unloads at a refinery, and what slaves bring
		// to their deployed miner (compares the two economies).
		int HarvesterIncome { 0 };
		int MinerIncome { 0 };
		int IncomeLogFrame { -1 };
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
		// Counter composition (UpdateCounterComposition): production score shift per buildable
		// combat type against the enemy army seen so far and what is attacking the base now, the
		// extra priority of the first of a building that would unlock a better counter than any we
		// can build now, and whether the base was under attack at the last update.
		std::unordered_map<const TechnoTypeClass*, int> CounterBonus {};
		std::unordered_map<int, int> CounterUnlockBonus {};
		int CounterFrame { -100000 };
		bool CounterUnderAttack { false };
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
		// Last recovery sale of a surplus factory (EconomyRecoverySaleFrames).
		int LastRecoverySaleFrame { -100000 };
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
		// Army nobody uses (NoteIdleArmy), by ArmyKind: the fewest of each kind that stood in the idle
		// pool at any AI tick of the last ArmyIdleWindowFrames, the running minimum of the window in
		// progress and its first frame. A kind is built without a ceiling while its units are put to
		// work, and pauses while that many of them go unused.
		std::array<int, 3> ArmyUnused {};
		std::array<int, 3> ArmyIdleFloor { INT_MAX, INT_MAX, INT_MAX };
		int ArmyIdleWindowStart { -1 };
		// Idle warships with nothing in reach (NoteShipsWithoutTarget): the fewest at any AI tick of the
		// last ArmyIdleWindowFrames, the running minimum of the window in progress and its first frame.
		int ShipsWithoutTarget { 0 };
		int ShipsWithoutTargetFloor { INT_MAX };
		int ShipsWithoutTargetWindowStart { -1 };
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
		// Deployers (unit -> frame since) standing on a new island with no site for their building.
		std::unordered_map<DWORD, int> DeployerNoSiteSince;
		// Wounded units (unit -> frame since) that stopped on resource cells that heal them.
		std::unordered_map<DWORD, int> ResourceHealSince;
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
		// The transport types a crossing our units wait for needs (NoteCrossingNeed): of our side, able
		// to make it, none of them owned. The production builds the buildings they lack, then one of them.
		std::vector<const TechnoTypeClass*> CrossingNeedTypes {};
		int CrossingNeedFrame { -1 };
		int CrossingNeedLogFrame { -100000 };
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
		// Targets by UniqueID where a plan's group failed (finishPlan); key access only.
		std::unordered_map<DWORD, NeuralPlanTargetLesson> PlanTargetLessons;
		// The same per enemy house: the value of the last raid group that was beaten on its way to
		// one of its buildings (raid gate of startPlan).
		std::array<NeuralPlanTargetLesson, 32> PlanHouseLessons {};
		// Capital groups by domain (0 ground, 1 air, 2 sea) and the capital units they hold back from
		// waves and lone strikes (UpdateCapitalGroups).
		std::array<NeuralCapitalGroup, 3> CapitalGroups {};
		std::unordered_set<DWORD> CapitalHeld {};
		// The tank guard (UpdateTankGuard): its tanks, and for each the site it guards and since when.
		std::unordered_set<DWORD> TankGuard {};
		std::unordered_map<DWORD, std::pair<int, int>> TankGuardPosts {};
		int TankGuardLogFrame { -100000 };
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
			// TEMP diagnostic: orders it got from its transport.
			int EnterOrders { 0 };
			int WalkOrders { 0 };
			// Last frame the transport handled it in its queue (-1 until then): a transport that stops
			// loading lets its passengers go after FerryQueueStaleFrames.
			int QueueFrame { -1 };
		};
		std::map<DWORD, FerryReservation> FerryReserved {};
		// Boarding turn of each loading transport: who holds it and since when it stands next to the
		// transport without getting aboard. Transport UniqueID -> turn. Keyed lookups only.
		struct FerryBoardTurn
		{
			DWORD Boarder { 0 };
			int NearSince { -1 };
			// TEMP diagnostic: last frame its shut door was logged.
			int DoorLogFrame { -1 };
		};
		std::unordered_map<DWORD, FerryBoardTurn> FerryBoardTurns {};
		// Flying transports that hang over a cell without touching down while passengers wait:
		// the cell, since when, the cell they were sent to instead and how many times they moved on.
		struct FerryLanding
		{
			CellStruct Cell { CellStruct::Empty };
			int Since { -1 };
			CellStruct Target { CellStruct::Empty };
			int Tries { 0 };
		};
		std::unordered_map<DWORD, FerryLanding> FerryLandings {};
		// The cell a carryall chose to set its cargo down on when the one below it at the drop zone
		// did not take it, by carryall UniqueID (key access only).
		std::unordered_map<DWORD, CellStruct> CarryallDropCells {};
		// Since when a carryall has looked for such a cell without setting its cargo down, by
		// carryall UniqueID (key access only).
		std::unordered_map<DWORD, int> CarryallDropSince {};
		// Since when a loaded transport on its pickup shore has had no landmass to go to, by
		// transport UniqueID (key access only).
		std::unordered_map<DWORD, int> FerryNoGoalSince {};
		// Transports the loader left parked with something to load or cargo aboard, and when:
		// nobody else gives them orders meanwhile (IsFerryParkedToLoad). TraceParkedFerries (TEMP
		// diagnostic) names whoever moved one all the same.
		struct ParkedFerry
		{
			UnitClass* Unit { nullptr };
			int Frame { -1 };
		};
		std::unordered_map<DWORD, ParkedFerry> ParkedFerries {};
		// Picks that never come out of their factory (WatchUndeliveredPick): the type is banned for a
		// while and the queue picks the next best one. Key: queue (1 vehicles, 2 infantry, 3 ships,
		// 4 aircraft) * 100000 + type index -> frame the ban ends.
		std::map<int, int> ProductionBanUntil {};
		std::array<int, 3> ProductionPickIdx { -1, -1, -1 };
		std::array<int, 3> ProductionPickSince { 0, 0, 0 };
		// Since when each type has been the pick of its queue (a pick left for less than
		// PickStreakGapFrames keeps its streak) and for how many of those frames the house could pay
		// for it; same key as ProductionBanUntil.
		struct PickStreak
		{
			int Since { -1 };
			int LastSeen { -1 };
			int PaidFrames { 0 };
		};
		std::map<int, PickStreak> PickStreaks {};
		// The last frame an object of each type came out of one of our factories, and how many times
		// each type was banned this match; same key.
		std::map<int, int> LastDeliveredFrame {};
		std::map<int, int> ProductionBanCount {};
		int FreeFieldAnchorDist { 9999 };
		int FreeFieldFrame { -100000 };
		// Last airlift that turned back (damaged carrier or a defended drop zone).
		int LastAirDropAbortFrame { -100000 };

		// Plan side effects read by building production and placement (UpdateStrategicPlans):
		// a new island ConYard to fortify, and a forward repair station toward the front.
		CellStruct IslandHoldCell { CellStruct::Empty };
		int IslandHoldUntilFrame { -1 };
		// What that yard still lacks, as the plan counts it: a defense that hits the ground, one
		// that shoots up.
		bool IslandHoldNeedsGround { false };
		bool IslandHoldNeedsAntiAir { false };
		// Placement found no cell that counts for that yard: until this frame its defenses are
		// chosen and placed like any other.
		int IslandHoldNoRoomUntilFrame { -1 };
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
		// Wave lesson (UpdateWaveVerdict): the wave under way waits for its verdict, with its target,
		// the known enemy buildings that stood near it when the wave left, whether a unit of ours
		// got there and the plan id of the mission it left for (-1 for none); and the waves in a
		// row that took no building, with the frame of the last one.
		bool WaveVerdictPending { false };
		bool WaveVerdictArrived { false };
		CellStruct WaveVerdictCell { CellStruct::Empty };
		int WaveVerdictBuildings { 0 };
		int WaveVerdictMission { -1 };
		int WaveLessonSteps { 0 };
		int WaveLessonFrame { -100000 };
		// Waves in a row that took no building, whatever the time between them.
		int WaveFailuresInARow { 0 };
		// TEMP diagnostic: units held at home that a wave ordered out, by unique id, with the frame.
		std::unordered_map<DWORD, int> HeldJoinersOut;
		// First frame a wave of the tank mass waited for its anti-air escort (-1 when none waits),
		// and no new wait before this frame.
		int WaveEscortWaitSince { -1 };
		int WaveEscortRetryFrame { -1 };
		// Last frame scouts were sent to look for the remnants of an enemy (RemnantSearchScouts).
		int RemnantSearchFrame { -100000 };
		// The goal the waves gave each unit (KeptWaveGoal): one still on its way keeps it.
		std::unordered_map<DWORD, NeuralWaveGoal> WaveGoals;
		// The idle army by type as the last tick counted it, and when: the group the next wave is
		// made of, for the target scoring (WaveGroupDamage*).
		std::vector<std::pair<const TechnoTypeClass*, int>> WaveGroupTypes;
		int WaveGroupFrame { -100000 };
		// Transport operations of the house (NeuralAI.Operations.inl): the operations, the next id,
		// the operation that holds each member or transport by UniqueID, units an operation gave back
		// and that may not ask again before a frame, the size of the members still waiting on their
		// land by passenger size (the transport production reads it as demand) with its frame, and
		// the frame of the last status line.
		std::vector<NeuralTransportOperation> TransportOps;
		int NextTransportOpId { 1 };
		std::unordered_map<DWORD, int> TransportOpOf;
		std::unordered_map<DWORD, int> TransportOpRetryUntil;
		std::array<int, 16> OpWaitingSizeBySize {};
		int OpWaitingFrame { -1 };
		int TransportOpLogFrame { -1 };
		std::vector<NeuralOpUnitWatch> OpWatch;
		// Where transports of the operations were lost, with the frame and whether they flew: the
		// routes of the next crossings keep away from there (PlanOpRoute).
		std::vector<NeuralOpLossSpot> OpLossSpots;
		// The danger of the crossings waiting for transports, on the ground (0) and in the air (1), and
		// when it was measured (UpdateTransportOperations): which transports the operations take and
		// the factories build follows it.
		std::array<int, 2> OpKindDanger {};
		int OpKindDangerFrame { -1 };
		// Harvester shuttle (UpdateHarvesterShuttles): the last check, no new shuttle before this
		// frame, the last frame a check found no ride, and the totals of the match (credits of ore
		// brought home, transports lost).
		int ShuttleCheckFrame { -1 };
		int ShuttleRetryUntil { -1 };
		// Back door (UpdateBackdoor): the last check, and no new one before this frame.
		int BackdoorCheckFrame { -1 };
		int BackdoorRetryUntil { -1 };
		int ShuttleNoRideLogFrame { -1 };
		int ShuttleCreditsTotal { 0 };
		int ShuttleTransportsLost { 0 };
		// Harvesters that could not get to their ride (UniqueID -> no shuttle takes them before that frame).
		std::unordered_map<DWORD, int> ShuttleSkipUntil;
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
		// The narrow passage where the last block of ours stood, since when blocks keep being found
		// there and when the last one was; and the way around it (JamRerouteHop): the spot avoided,
		// until when, whether every unit takes it or half of them, and the last frame no way was found.
		CellStruct JamSpot { CellStruct::Empty };
		int JamSpotSince { -1 };
		int JamSpotSeenFrame { -1 };
		CellStruct JamRerouteSpot { CellStruct::Empty };
		int JamRerouteUntil { -1 };
		bool JamRerouteAll { false };
		int JamRerouteNoWayFrame { -100000 };
		// Last frame one of our buildings was sold because it held a blocked passage shut (JamSealSellFrames).
		int LastJamSealSellFrame { -100000 };
		// Closed-in war factories (SellFactoryRoadBlock): first frame of the current stretch with
		// FactoryRoadSellHeldVehicles vehicles jammed at home (-1 none), last check and last sale.
		int HeldAtHomeSince { -1 };
		// Power a long-range gun tower held back by power alone asks for (its drain plus the
		// margin), and the frame it asked.
		int DefensePowerWanted { 0 };
		int DefensePowerWantedFrame { -100000 };
		int LastFactoryRoadCheckFrame { -100000 };
		int LastFactoryRoadSellFrame { -100000 };
		// Units shut in a pocket (SellPocketBlock): last check, last sale, last log line, and the
		// ground of the building sold, kept free until PocketClearUntil.
		// The wave target with no path that transport operations carry the wave to, and since when
		// (PathDeadFerryTryFrames).
		CellStruct PathDeadFerryGoal { CellStruct::Empty };
		CellStruct PathDeadFerryLanding { CellStruct::Empty };
		int PathDeadFerrySince { -100000 };
		int LastPocketCheckFrame { -100000 };
		int LastPocketSellFrame { -100000 };
		int LastPocketLogFrame { -100000 };
		CellStruct PocketClearFrom { CellStruct::Empty };
		CellStruct PocketClearTo { CellStruct::Empty };
		int PocketClearUntil { -1 };
		// Emergency lanes (UpdateEmergencyLanes): the last check, the last log line, and since that
		// line the idle units that stepped aside and the units with an order they made way for.
		int LastEmergencyLaneFrame { -1 };
		int EmergencyLaneLogFrame { -1 };
		int EmergencyLaneMoved { 0 };
		int EmergencyLaneServed { 0 };
		// Shared destinations (SpreadDestination): orders moved to a free cell next to a cell another
		// unit of ours holds or heads to since the last log line, and the frame of that line.
		int DestinationSpreadCount { 0 };
		int DestinationSpreadLogFrame { -1 };
		// Ferry island commitment: ticks a loaded transport kept the island it sailed for while the
		// goals pointed elsewhere, since the last log line, and the frame of that line.
		int FerryIslandKept { 0 };
		int FerryIslandKeptLogFrame { -1 };
		// Landings of loaded transports that moved more than FerryLandingRepickCells before the
		// landing commitment since the last log line, and the frame of that line (diagnostics).
		int FerryLandingRepicks { 0 };
		int FerryLandingRepickLogFrame { -1 };
		// Last log line of the ferry fleet split (diagnostics, at most every FerryIslandKeptLogFrames).
		int FerryFleetLogFrame { -1 };
		// Joint departure signal by pickup zone: the frame, the cell and the transport that sailed.
		struct FerryConvoySignal
		{
			int Frame { -1 };
			CellStruct Cell { CellStruct::Empty };
			DWORD Leader { 0 };
		};
		std::map<int, FerryConvoySignal> FerryConvoyGo {};
		// Last frame the aircraft pick wanted a type that needs a dock and every dock was taken: the
		// building pass answers with another airfield (AirDockWantedFrames).
		int AirDockWantedFrame { -100000 };
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
		// The frame each plan last ended and whether it ended well: a combo chain is judged by the
		// plan it led into.
		std::array<int, static_cast<size_t>(NeuralPlanId::Count)> PlanEndFrame {};
		std::array<bool, static_cast<size_t>(NeuralPlanId::Count)> PlanEndedWell {};
		// What each plan did this match against each enemy house, as a weight in percent of its
		// utility (100 at the start, see PlanEnemyWeightMin), and the house each plan last went for.
		std::array<std::array<int, 32>, static_cast<size_t>(NeuralPlanId::Count)> PlanEnemyWeight {};
		std::array<int, static_cast<size_t>(NeuralPlanId::Count)> PlanLastEnemy {};
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
			this->MinerOreSamples.clear();
			this->StrandedMinerSince.clear();
			this->MinerRelocations = 0;
			this->MinerLogFrame = -1;
			this->HarvesterIncome = 0;
			this->MinerIncome = 0;
			this->IncomeLogFrame = -1;
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
			this->CounterUnderAttack = false;
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
			this->LastRecoverySaleFrame = -100000;
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
			this->ArmyUnused.fill(0);
			this->ArmyIdleFloor.fill(INT_MAX);
			this->ArmyIdleWindowStart = -1;
			this->ShipsWithoutTarget = 0;
			this->ShipsWithoutTargetFloor = INT_MAX;
			this->ShipsWithoutTargetWindowStart = -1;
			this->NoRoomIslandUntil.clear();
			this->DeployerNoSiteSince.clear();
			this->ResourceHealSince.clear();
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
			this->CrossingNeedTypes.clear();
			this->CrossingNeedFrame = -1;
			this->CrossingNeedLogFrame = -100000;
			this->HarvesterSaveTarget = 0;
			this->RefinerySaveTarget = 0;
			this->EconomyHoldSince = -1;
			this->EconomyHoldPauseUntil = -1;
			this->StealthSightings.clear();
			this->PlanMemberTraces.clear();
			this->PlanTargetLessons.clear();
			this->PlanHouseLessons.fill(NeuralPlanTargetLesson {});
			this->CapitalGroups = {};
			this->CapitalHeld.clear();
			this->TankGuard.clear();
			this->TankGuardPosts.clear();
			this->TankGuardLogFrame = -100000;
			this->BankWantedYards = 0;
			this->FerryingMcvs.clear();
			this->FerryWaitSince.clear();
			this->FerryBlockedUntil.clear();
			this->FerryReserved.clear();
			this->FerryBoardTurns.clear();
			this->FerryLandings.clear();
			this->CarryallDropCells.clear();
			this->CarryallDropSince.clear();
			this->FerryNoGoalSince.clear();
			this->ParkedFerries.clear();
			this->ProductionBanUntil.clear();
			this->ProductionPickIdx = { -1, -1, -1 };
			this->ProductionPickSince = { 0, 0, 0 };
			this->PickStreaks.clear();
			this->LastDeliveredFrame.clear();
			this->ProductionBanCount.clear();
			this->FreeFieldAnchorDist = 9999;
			this->FreeFieldFrame = -100000;
			this->LastAirDropAbortFrame = -100000;
			this->IslandHoldCell = CellStruct::Empty;
			this->IslandHoldUntilFrame = -1;
			this->IslandHoldNeedsGround = false;
			this->IslandHoldNeedsAntiAir = false;
			this->IslandHoldNoRoomUntilFrame = -1;
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
			this->WaveVerdictPending = false;
			this->WaveVerdictArrived = false;
			this->WaveVerdictCell = CellStruct::Empty;
			this->WaveVerdictBuildings = 0;
			this->WaveVerdictMission = -1;
			this->WaveLessonSteps = 0;
			this->WaveLessonFrame = -100000;
			this->WaveFailuresInARow = 0;
			this->HeldJoinersOut.clear();
			this->WaveEscortWaitSince = -1;
			this->WaveEscortRetryFrame = -1;
			this->RemnantSearchFrame = -100000;
			this->WaveGoals.clear();
			this->WaveGroupTypes.clear();
			this->WaveGroupFrame = -100000;
			this->TransportOps.clear();
			this->NextTransportOpId = 1;
			this->TransportOpOf.clear();
			this->TransportOpRetryUntil.clear();
			this->OpWaitingSizeBySize.fill(0);
			this->OpWaitingFrame = -1;
			this->TransportOpLogFrame = -1;
			this->OpWatch.clear();
			this->OpLossSpots.clear();
			this->OpKindDanger.fill(0);
			this->OpKindDangerFrame = -1;
			this->ShuttleCheckFrame = -1;
			this->ShuttleRetryUntil = -1;
			this->BackdoorCheckFrame = -1;
			this->BackdoorRetryUntil = -1;
			this->ShuttleNoRideLogFrame = -1;
			this->ShuttleCreditsTotal = 0;
			this->ShuttleTransportsLost = 0;
			this->ShuttleSkipUntil.clear();
			this->FailedApproachWeight.fill(0);
			this->StagingHoldSince = -1;
			this->StagingHoldPauseUntil = -1;
			this->LastJamBreakFrame = -1;
			this->LastJamCheckFrame = -1;
			this->LastMouthClearFrame = -1;
			this->LastJamDiagFrame = -1;
			this->JamMapLogged = false;
			this->JamSpot = CellStruct::Empty;
			this->JamSpotSince = -1;
			this->JamSpotSeenFrame = -1;
			this->JamRerouteSpot = CellStruct::Empty;
			this->JamRerouteUntil = -1;
			this->JamRerouteAll = false;
			this->JamRerouteNoWayFrame = -100000;
			this->LastJamSealSellFrame = -100000;
			this->HeldAtHomeSince = -1;
			this->DefensePowerWanted = 0;
			this->DefensePowerWantedFrame = -100000;
			this->LastFactoryRoadCheckFrame = -100000;
			this->LastFactoryRoadSellFrame = -100000;
			this->PathDeadFerryGoal = CellStruct::Empty;
			this->PathDeadFerryLanding = CellStruct::Empty;
			this->PathDeadFerrySince = -100000;
			this->LastPocketCheckFrame = -100000;
			this->LastPocketSellFrame = -100000;
			this->LastPocketLogFrame = -100000;
			this->PocketClearFrom = CellStruct::Empty;
			this->PocketClearTo = CellStruct::Empty;
			this->PocketClearUntil = -1;
			this->LastEmergencyLaneFrame = -1;
			this->EmergencyLaneLogFrame = -1;
			this->EmergencyLaneMoved = 0;
			this->EmergencyLaneServed = 0;
			this->DestinationSpreadCount = 0;
			this->DestinationSpreadLogFrame = -1;
			this->FerryIslandKept = 0;
			this->FerryIslandKeptLogFrame = -1;
			this->FerryLandingRepicks = 0;
			this->FerryLandingRepickLogFrame = -1;
			this->FerryFleetLogFrame = -1;
			this->FerryConvoyGo.clear();
			this->AirDockWantedFrame = -100000;
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
			this->PlanEndFrame.fill(0);
			this->PlanEndedWell.fill(false);

			for (auto& weights : this->PlanEnemyWeight)
				weights.fill(100);

			this->PlanLastEnemy.fill(-1);
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
		// The island a loaded transport sailed for (its zone and goal cell): kept while it is still a
		// goal of ours or still holds the enemy, cleared once the cargo is off (see the ferry loop).
		int CommittedZone { -1 };
		CellStruct CommittedGoal { CellStruct::Empty };
		// The landing picked at the last tick before the commitment (diagnostics: landing re-picks).
		CellStruct LastLanding { CellStruct::Empty };
		// First frame a combat cargo sat loaded but short of three quarters of the room on its
		// pickup shore.
		int UnderfilledSince { -1 };
		// Health at the last tick and the last frame it dropped (under fire now).
		int LastHealth { -1 };
		int HitFrame { -100000 };
		// Why the cargo was let go at the last decision on the pickup shore (FerrySail flags).
		int SailReason { 0 };
		// Passengers aboard at the last count and the frame that count last changed.
		int LastPassCount { 0 };
		int LastBoardFrame { -1 };
		// Frame the stuck cargo watchdog had the cargo set down on its pickup shore (FerryStuckCargoFrames).
		int SetDownFrame { -1 };
	};

	// The zone map's value for a cell with no zone at all (outside the zoned map), read as 65535.
	constexpr int NoZoneId = 0xFFFF;

	// A zone units stand on and set out from. Zones 0 and 1 are no ground for a vehicle (water, a
	// shore, a cliff foot) and NoZoneId is no zone: an ore field, a goal or a target there is no
	// island to ferry to, and transports loaded for one never found a beach and kept their cargo.
	bool IsGroundZone(int zone)
	{
		return zone > 1 && zone != NoZoneId;
	}

	// Reasons a transport is let go from its pickup shore (NeuralFerryTripRecord::SailReason).
	constexpr int FerrySailBaseDeployer = 1;
	constexpr int FerrySailStolenDeployer = 2;
	constexpr int FerrySailHarvester = 4;
	constexpr int FerrySailEngineer = 8;
	constexpr int FerrySailUnderFire = 16;
	constexpr int FerrySailInTransit = 32;
	constexpr int FerrySailNothingMoreToLoad = 64;
	constexpr int FerrySailLoadStalled = 128;
	constexpr int FerrySailConvoy = 256;
	constexpr int FerryLoadStallFrames = 5400;

	// How long a combat cargo waits on its shore for more: under half the room, and under three
	// quarters of it. A transport counts as under fire this long after a hit.
	constexpr int FerryHalfLoadWaitFrames = 5400;
	// A combat transport ready to cross waits this long for the other transports within
	// FerryConvoyCells on its shore that are loading or have cargo waiting for them, so the
	// landing comes as one wave.
	constexpr int FerryConvoyWaitFrames = 1800;
	constexpr int FerryConvoyCells = 30;
	// Ready combat transports needed before the odd-numbered ones strike the secondary island.
	constexpr int FerrySplitFrontTransports = 4;
	// How long the departure signal of a combat transport makes its sister transports on the same
	// shore sail with it (joint departure).
	constexpr int FerryConvoyGoFrames = 120;
	// Longest wait of a loaded combat transport for the empty transports on their way to load the rest
	// of its battalion (it counts from the frame the transport had nothing more to load).
	constexpr int FerryBattalionWaitFrames = 3600;
	// Enemies seen near a landing this recently count against it even when out of sight now.
	constexpr int FerryLandingSightingFrames = 900;
	// How often the ferry island commitment logs how many times it kept a loaded transport on course.
	constexpr int FerryIslandKeptLogFrames = 1800;
	constexpr double FerryLandingRepickCells = 8.0;
	// A loaded transport this close to its landing keeps its island whatever the goals say.
	constexpr double FerryArrivalKeepCells = 20.0;
	constexpr int FerryMostLoadWaitFrames = 2700;
	constexpr int FerryUnderFireFrames = 300;

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
	// A reservation its transport has not handled for this long is dropped (the transport left, got
	// another job or the passenger is on other ground).
	constexpr int FerryQueueStaleFrames = 120;
	// Reserved passengers wait this far from their transport, so the cells next to it stay free for
	// the one boarding.
	constexpr double FerryWaitCells = 4.0;
	// The one boarding loses its turn after this long within FerryBoardTurnCells of the transport
	// without getting aboard, and is not reserved again for FerryBoardFailedFrames.
	constexpr int FerryBoardTurnFrames = 300;
	constexpr double FerryBoardTurnCells = 6.0;
	constexpr int FerryBoardFailedFrames = 2700;
	// A flying transport that has not touched down after this long over one cell tries another, one
	// ring of cells farther each time up to FerryLandingMaxRing.
	constexpr int FerryLandingFrames = 300;
	constexpr int FerryLandingMaxRing = 5;
	// How far a flying transport over a beach looks for a cell it can touch down on.
	constexpr int FerryBeachSearchCells = 8;
	constexpr double FerryLandingDriftCells = 1.5;
	// A carryall within CarryallDropZoneCells of its drop cell sets the cargo down; when the cell
	// below does not take it, on the nearest one that does within CarryallDropSearchCells.
	constexpr double CarryallDropZoneCells = 2.5;
	// A loaded transport that has had nowhere to go for this long, still on its pickup shore, sets
	// its cargo down again.
	constexpr int FerryNoGoalUnloadFrames = 900;
	// A loaded transport still on its pickup shore whose passengers have not changed for this long
	// sets them down too: it is past every wait of the loader (convoy, battalion, short load).
	constexpr int FerryStuckCargoFrames = 9000;
	// How long that set-down keeps the transport unloading before the loader has it again.
	constexpr int FerrySetDownFrames = 900;
	// Transport operations (NeuralAI.Operations.inl). TransportOpsEnabled switches them on: the
	// waves hand the units that cannot walk to their goal to an operation, and the old ferry no
	// longer loads idle units of its own accord.
	// - OpBoardingCapFrames: how long a load waits for the whole battalion once a transport stands
	//   on the staging beach before the loaded transports sail (user: all loaded, with a time cap).
	// - OpNoTransportFrames: an operation that has had no transport for this long gives its units
	//   back; those may not ask again for OpRetryFrames.
	// - OpStuckFrames: a transport that has not moved OpStuckCells in this long tries another cell.
	// - OpCrossCapFrames: a crossing that lasts longer turns back with its cargo.
	// - OpUnloadCells: how close to its landing a transport sets its cargo down; OpParkCells: how
	//   close to the staging cell it parks; OpGatherCells: where the members wait around it.
	// - OpTransportMinHealth: a transport below it is not taken; OpDyingTransportHealth: one below it
	//   on land sets its load down at once (the old emergency unload, Slot 70).
	// - OpUnloadRetryFrames: a transport still loaded this long at its landing tries the engine's
	//   unload, then another cell (OpMaxLandingTries cells, then the nearest land it stands by).
	// - OpLandingSpreadCells: the landings of one load lie this far apart at least.
	// - OpMaxFailedTrips: loads taken back to the staging beach before the operation gives up.
	constexpr bool TransportOpsEnabled = true;
	constexpr int OpBoardingCapFrames = 3600;
	constexpr int OpNoTransportFrames = 9000;
	constexpr int OpRetryFrames = 3600;
	constexpr int OpStuckFrames = 900;
	constexpr double OpStuckCells = 2.0;
	constexpr int OpCrossCapFrames = 9000;
	constexpr double OpUnloadCells = 2.5;
	constexpr double OpParkCells = 4.0;
	constexpr double OpGatherCells = 5.0;
	// Where units board a transport (FindBoardingCell): the nearest cell to them on the outskirts
	// of the base or expansion they stand at, BoardingBaseClearCells or more from every building of
	// ours (not inside the base), with BoardingRoomCells of the 24 cells around it free for them to
	// wait on, within BoardingSearchCells; with none, a second pass asks for the relaxed values.
	// An enemy in sight (or seen a moment ago) within BoardingEnemyCells counts as
	// BoardingEnemyPenaltyCells more cells of walk, a beach where our loaded transports died within
	// BoardingBloodyCells as BoardingBloodyPenaltyCells; each cell of distance to the landing counts
	// BoardingLandingWeight, so among cells about as near the side facing the crossing wins.
	constexpr int BoardingBaseClearCells = 3;
	constexpr int BoardingBaseClearRelaxedCells = 2;
	constexpr int BoardingRoomCells = 16;
	constexpr int BoardingRoomRelaxedCells = 10;
	constexpr int BoardingSearchCells = 48;
	constexpr int BoardingEnemyCells = 8;
	constexpr double BoardingEnemyPenaltyCells = 12.0;
	constexpr double BoardingBloodyCells = 10.0;
	constexpr double BoardingBloodyPenaltyCells = 20.0;
	constexpr double BoardingLandingWeight = 0.05;
	constexpr int BoardingCacheFrames = 150;
	constexpr int BoardingCacheGridCells = 4;
	// A crossing no transport of the house makes (NoteCrossingNeed) stays wanted this long after the
	// last unit waited for it, as long as a failed operation waits to ask again (OpRetryFrames). The
	// buildings the transport that would make it lacks, and then that transport, get these bonuses in
	// the production.
	constexpr int CrossingNeedFrames = 3600;
	constexpr int CrossingNeedLogFrames = 1800;
	constexpr int CrossingPrerequisiteBonus = 1200;
	constexpr int CrossingTransportBonus = 1100;
	constexpr double OpTransportMinHealth = 0.5;
	constexpr double OpDyingTransportHealth = 0.24;
	constexpr int OpUnloadRetryFrames = 450;
	constexpr int OpMaxLandingTries = 4;
	constexpr int OpLandingSpreadCells = 3;
	constexpr int OpMaxFailedTrips = 2;
	constexpr int OpLogFrames = 1800;
	constexpr int OpInterferenceLogFrames = 900;
	// Safe routes of the transport operations (PlanOpRoute) over a grid of OpRouteBlockCells-cell
	// blocks: a block costs OpRouteBaseCost per cell crossed plus its danger, the sum over every
	// threat that can hit the transport of the cells the block lies inside the threat's reach times
	// its weight. Threats (user: the defenses, and the enemy units too wherever possible): a known
	// enemy defense (weapon range + OpRouteDefenseMarginCells, OpRouteDefenseWeight), any other known
	// enemy building (OpRouteBaseCells, weight 1), an enemy unit in sight (range +
	// OpRouteUnitMarginCells, OpRouteUnitWeight), one out of sight seen within OpRouteSightingFrames
	// (range + OpRouteSightingMarginCells, OpRouteSightingWeight fading with age), and a place where
	// we lost such a transport within OpRouteLossFrames (OpRouteLossCells, OpRouteLossWeight). Reach
	// is at most OpRouteMaxReachCells. A crossing is planned again every OpRouteReplanFrames and when
	// its transport loses OpRouteHitHealth of its strength (OpRouteHitReplanFrames apart at least),
	// and takes the new route when it costs OpRouteKeepPercent or less of what is left of the old one.
	// A waypoint counts as reached within OpRouteReachCells; OpRouteMaxWaypoints at most, each the
	// farthest block of the searched path, up to OpRouteSmoothBlocks ahead, a straight line reaches
	// at no more danger.
	// The landing of a load (ChooseOpTripLanding) is the best of OpLandingCandidates cells of the
	// target's land, OpLandingSpacingCells apart and up to OpLandingCandidateCells from the landing
	// first chosen for the operation: route cost, plus the danger of its block times
	// OpLandingDangerFactor (a flying transport comes down there: times OpLandingFlyerDangerFactor
	// against anti-air), plus each cell of walk it adds to the target at the cost of a safe cell.
	constexpr int OpRouteBlockCells = 3;
	constexpr int OpRouteBaseCost = 8;
	constexpr int OpRouteDefenseWeight = 6;
	constexpr int OpRouteDefenseMarginCells = 2;
	constexpr int OpRouteBaseCells = 5;
	constexpr int OpRouteUnitWeight = 3;
	constexpr int OpRouteUnitMarginCells = 3;
	constexpr int OpRouteSightingWeight = 2;
	constexpr int OpRouteSightingMarginCells = 4;
	constexpr int OpRouteSightingFrames = 1800;
	constexpr int OpRouteLossWeight = 4;
	constexpr int OpRouteLossCells = 6;
	constexpr int OpRouteLossFrames = 9000;
	constexpr int OpRouteMaxReachCells = 20;
	constexpr int OpRouteReplanFrames = 450;
	constexpr double OpRouteHitHealth = 0.1;
	constexpr int OpRouteHitReplanFrames = 60;
	constexpr int OpRouteKeepPercent = 85;
	constexpr double OpRouteReachCells = 3.5;
	constexpr int OpRouteMaxWaypoints = 12;
	constexpr size_t OpRouteSmoothBlocks = 40;
	constexpr int OpLandingCandidates = 12;
	constexpr int OpLandingCandidateCells = 30;
	constexpr int OpLandingSpacingCells = 6;
	constexpr int OpLandingDangerFactor = 3;
	constexpr int OpLandingFlyerDangerFactor = 8;
	constexpr int OpLossSpotsKept = 24;
	// The kind of transport by danger (user): every OpKindDangerFrames the operations measure the
	// danger of the crossings waiting for transports, straight from the staging cell to the landing,
	// on the ground and in the air (a burrowing transport meets none). An idle transport counts that
	// danger as one cell farther per OpKindDangerPerCell when the operations take transports, and the
	// factories score a transport type OpKindDangerPenalty lower per unit of it, OpKindDangerMaxPenalty
	// at most.
	constexpr int OpKindDangerFrames = 150;
	constexpr int OpKindDangerPerCell = 2;
	constexpr int OpKindDangerPenalty = 3;
	constexpr int OpKindDangerMaxPenalty = 1000;
	// Waiting on the beach (user: when even the safest route is too dangerous, with a cap): a load whose
	// route has a danger above OpSailDangerLimit waits for it to pass (moving enemies go, sightings
	// fade), planned again every OpDangerCheckFrames, OpDangerWaitFrames at most; then it sails. The
	// first measures of the routes: 3 of the 9 loads that sailed along a route of danger 90 or more
	// were lost, 4 of the 130 along one of none (2026-11-16).
	// A carryall waits too, before it lifts its vehicle: it flies straight (the engine sets the vehicle
	// down at the end of every move), and it never parks with a load. While the carryalls skipped the
	// wait, they lost 9 of 21 crossing straight into dangers of 369 to 2559 (2026-11-17).
	constexpr int OpSailDangerLimit = 90;
	constexpr int OpDangerCheckFrames = 150;
	constexpr int OpDangerWaitFrames = 1800;
	// At the end of the wait, a load or a carryall whose safest way to the cheapest landing of the
	// target's land is still above OpAbortDangerLimit throws a die: with OpAbortPercent it aborts, and
	// its troops and transports stay idle for others to take (user). A load aboard first goes with its
	// transports to another operation of the same client from the same land that waits for seats,
	// rather than unload to load again. The carryalls that waited and then lifted at danger 384 to 618
	// lost 5 of 6 (2026-11-17).
	constexpr int OpAbortDangerLimit = 3 * OpSailDangerLimit;
	constexpr int OpAbortPercent = 50;
	// A threat whose weapon takes the transport (mind control) weighs OpRouteTakeFactor times as
	// much: a transport taken goes over to the enemy with its whole load. 26 of the 28 transports
	// taken in 47 matches were SAPC landing in the reach of Yuri's Psychic Towers, Yuri and
	// Masterminds (2026-11-17).
	constexpr int OpRouteTakeFactor = 3;
	// A member that gets no OpMemberStuckCells nearer to the cell the operation sent it to in
	// OpMemberStuckFrames, while farther from it than OpMemberStuckFarCells, is let go. It may ask
	// again after OpRetryFrames. One engineer stood 17 cells from its berth wait cell for most of an
	// operation that stayed open 65000 frames, with 5098 interferences (match #10708, Moonlit
	// Waters of Esvia). Closer than OpMemberStuckFarCells it may only be queued behind a large wave.
	constexpr int OpMemberStuckFrames = 1800;
	constexpr double OpMemberStuckCells = 2.0;
	constexpr double OpMemberStuckFarCells = 12.0;
	// Against the threats that move (user): a transport inside the reach of a threat plans its route
	// again every OpRouteDangerReplanFrames instead of OpRouteReplanFrames. A crossing whose landing
	// grew OpLandingSwitchDanger more dangerous since it sailed (enemies gathered at it), still
	// OpLandingSwitchMinCells or more from it, takes another landing for its load when that one costs
	// OpLandingSwitchPercent or less, once per OpLandingSwitchFrames for an operation.
	constexpr int OpRouteDangerReplanFrames = 90;
	constexpr int OpLandingSwitchDanger = 40;
	constexpr int OpLandingSwitchPercent = 75;
	constexpr int OpLandingSwitchFrames = 450;
	constexpr double OpLandingSwitchMinCells = 6.0;
	// Berths (user: forty transports and 185 units bound for one staging cell jammed into one mass the
	// transports could not get through, match #10477): each transport parks to load at its own cell
	// around the staging cell, within OpBerthRingCells of it and OpBerthSpacingCells from the others,
	// and its passengers wait for it OpBerthWaitCells farther out, in the small group of its own load.
	constexpr int OpBerthRingCells = 14;
	constexpr double OpBerthSpacingCells = 4.0;
	constexpr int OpBerthWaitCells = 3;
	// Harvester shuttle (user: a harvester with no ore on its land rides an idle transport to a field
	// across water with no refinery of ours, the transport waits nearby like a driver, brings it back
	// full and the loop starts again). Checked every ShuttleCheckFrames once the ore of the lands we
	// hold is under ShuttleHomeOre credits (UpdateIslandOreOutlook); up to ShuttleMaxHarvesters per
	// shuttle, one shuttle per house. A harvester on the field comes back full, when its load has not
	// grown for ShuttleIdleFrames, or ShuttleHarvestCapFrames after the landing; at home it boards
	// again once empty, or ShuttleUnloadCapFrames after the landing. A shuttle that lost
	// ShuttleMaxLostTransports transports ends, and no other starts for ShuttleRetryFrames. A harvester
	// that gets no 2 cells nearer to its ride in ShuttleStuckFrames while a transport waits for it is let
	// go and not taken again for ShuttleStuckRetryFrames: the engine turns an AI harvester whose move
	// ends or fails back to harvesting, and one shut in near its refinery kept the only transport of its
	// house waiting 8000 frames twice in a row (match #10559, The Honeymoon).
	// The archives before it (2026-11-16, 79 logs): 9 houses in 5 of the 23 matches with a free field
	// across water spent 14000 to 46000 frames with their ore gone, harvesters and transports at hand,
	// often with single-digit credits.
	constexpr int ShuttleCheckFrames = 450;
	constexpr int ShuttleHomeOre = 10000;
	constexpr int ShuttleMaxHarvesters = 4;
	constexpr int ShuttleIdleFrames = 900;
	constexpr int ShuttleHarvestCapFrames = 4500;
	constexpr int ShuttleUnloadCapFrames = 4500;
	constexpr int ShuttleMaxLostTransports = 2;
	constexpr int ShuttleRetryFrames = 9000;
	constexpr int ShuttleStuckFrames = 1800;
	constexpr int ShuttleStuckRetryFrames = 27000;
	// Back door (user, an idea of the old aimd.ini, whose transported attack on the farthest factory had
	// the largest reward of the file). Checked every BackdoorCheckFrames:
	// - The house's latest wave (BackdoorFrontFrames old at most) hits an enemy base on the house's own
	//   ground, with BackdoorMinDefenses or more known defenses within BackdoorFrontCells of its target.
	//   With 900 frames and 3 defenses no back door started in about 60 matches: no front in most
	//   checks (a wave takes far longer to reach its target), too few defenses in the rest (2026-11-17).
	// - BackdoorTeam idle armed ground units of the house (BackdoorMinTeam at least, within BackdoorHomeCells
	//   of its base) ride a transport operation (client Strike) around that base.
	// - They land within BackdoorLandingCells of a known valuable building of the same house,
	//   BackdoorMinGapCells or more from the wave's target, with a landing danger below
	//   OpSailDangerLimit, and attack it. The building farthest from the wave's target with the fewest
	//   defenses around it is taken.
	// - A member is delivered once a transport sets it down within BackdoorDeliverCells of the landing.
	//   The landings of the load stay within BackdoorLandingCandidateCells of the first.
	// - One back door at a time per house, BackdoorRetryFrames after the last started.
	constexpr int BackdoorCheckFrames = 900;
	constexpr int BackdoorFrontFrames = 3600;
	constexpr int BackdoorMinDefenses = 2;
	constexpr double BackdoorFrontCells = 12.0;
	constexpr int BackdoorTeam = 8;
	constexpr int BackdoorMinTeam = 4;
	constexpr double BackdoorHomeCells = 30.0;
	constexpr int BackdoorLandingCells = 8;
	constexpr double BackdoorMinGapCells = 20.0;
	constexpr double BackdoorGuardCells = 9.0;
	constexpr double BackdoorDeliverCells = 12.0;
	constexpr int BackdoorLandingCandidateCells = 8;
	constexpr int BackdoorRetryFrames = 5400;
	// Capture barracks (user): a house with buildings of its own on a land where a tech it would take
	// stands (neutral, or one an enemy took: oil derricks, tech buildings) and no barracks of its own
	// there builds a barracks next to that tech, so its engineer walks a few cells instead of crossing
	// the water from the main base. Checked with the tech memory every 150 frames
	// (UpdateCaptureBarracks):
	// - The site is the best such tech out of known enemy defense reach: an urgent one first (an
	//   engineer of ours failed on the way to it or next to it, or the straight crossing from our
	//   nearest barracks has a danger above CaptureCrossingDangerLimit on the ground danger map of the
	//   crossings), then the most valuable for the walk from our nearest building on its land.
	// - The barracks of an urgent site comes before the other building picks under the terms of Island
	//   Hold; any other site adds CaptureBarracksBonus to the barracks score. The difficulty cap lets it
	//   through as it does an outpost's factory.
	// - It is placed on the tech's land, CaptureBarracksPullPerCell lower per cell away from the tech.
	//   With no such cell it is placed like any other barracks and no site on that land asks again for
	//   CaptureBarracksNoRoomFrames, longer each time, up to CaptureBarracksNoRoomSteps times as long. A
	//   barracks placed for a site that does not stand within CaptureBarracksNoRoomFrames is forgotten.
	// - An engineer in training comes out of the barracks of ours on the land of the best tech it can
	//   take that no engineer of ours stands on or goes for (UpdatePrimaryFactories).
	// - An engineer is any soldier that captures (IsCaptureInfantryType): Engineer=yes, or a weapon whose
	//   warhead captures like one (FakeEngineer.CanCaptureBuildings), which is sent with an attack order.
	// Match #10769 (Crystal Whiplash): house 1 held a yard on an island where house 2 had taken two oil
	// derricks; its engineers for them crossed from the main base by transport, and three of the four
	// transport operations ended with the engineer dead 60 to 390 frames after they opened.
	constexpr int CaptureCrossingDangerLimit = OpSailDangerLimit;
	constexpr int CaptureBarracksBonus = 1200;
	constexpr int CaptureBarracksPullPerCell = 120;
	constexpr int CaptureBarracksNoRoomFrames = 5400;
	constexpr int CaptureBarracksNoRoomSteps = 4;
	constexpr int CaptureBarracksLogLines = 60;
	// Engineer work by warhead (FakeEngineerAbilities): a unit whose warhead disarms bombs answers a bomb
	// on a building or unit of ours or of an ally within this many cells, as far as an engineer goes for
	// one.
	constexpr int FakeEngineerBombCells = 14;
	// How far around a target on no ground the ground it stands next to is looked for.
	constexpr int FerryTargetGroundCells = 3;
	// Deployed slave miners: a miner with this many ore cells (sampled every 2 cells) within its
	// slaves' reach is productive; one with fewer for MinerStrandedFrames packs up for the nearest
	// field holding MinerFieldMinOreCells or more, with no mining site within MinerFieldClearCells,
	// up to MinerFieldMaxCells away. MinerLogFrames between the log lines of a house.
	constexpr int MinerProductiveOreSamples = 4;
	constexpr int MinerStrandedFrames = 900;
	constexpr int MinerFieldMinOreCells = 10;
	constexpr int MinerFieldClearCells = 10;
	constexpr int MinerFieldMaxCells = 72;
	constexpr int MinerLogFrames = 1800;
	// Recovery sale (user: in extreme poverty a house with too many factories may sell one to pay
	// for the recovery of its economy): while a house saves for the harvester or the refinery that
	// restores its income, is short of its price and nothing comes in (nothing gathers, or it lost
	// a third of its refineries or harvesters and no unload paid for
	// EconomyRecoverySaleQuietFrames), one factory of the category with the most copies is sold
	// every EconomyRecoverySaleFrames, down to EconomyRecoveryKeepFactories a category. Never a
	// shipyard, the newest factory, the last of a type or the only one by a construction yard.
	constexpr int EconomyRecoverySaleFrames = 300;
	constexpr int EconomyRecoverySaleQuietFrames = 1800;
	constexpr int EconomyRecoveryKeepFactories = 1;
	// Ore cells of a held 12x12 ore block for each refinery past the first it takes (3 at most).
	constexpr int RefineryOreCellsPerExtra = 24;
	// Most mobile refineries (slave miners) a house mining with them wants, as the Advanced AI, and
	// the least it wants with free fields (a classic base holds 4 refineries at home).
	constexpr int MaxMobileMiners = 15;
	constexpr int MinMobileMiners = 4;
	// Docks an AI house keeps free for its aircraft, by difficulty (user): another airfield while
	// fewer stand free.
	constexpr int MinFreeAirDocksEasy = 4;
	constexpr int MinFreeAirDocksNormal = 6;
	constexpr int MinFreeAirDocksHard = 8;
	// Vehicles and ships of ours per mind-control vehicle (2 at least).
	constexpr int MindControlVehiclesPer = 4;
	// Frames between the harvest income log lines of a house (harvesters against deployed miners).
	constexpr int IncomeLogFrames = 4500;
	// Defenses measured in points as the Advanced AI does, by AI difficulty (Hard, Normal, Easy):
	// the ground level a base wants (base, per refinery, per functional structure, with an
	// expansion pending), the anti-air base, and the most either reaches. A defense that declares
	// no value counts as a typical one. Coverage of the key assets (CoverageDefenseCells) comes on
	// top, two towers per asset at most past the level.
	constexpr int DefenseBaseValue[3] = { 70, 45, 25 };
	constexpr int AntiAirBaseValue[3] = { 40, 25, 15 };
	constexpr int DefenseMaxValue[3] = { 220, 150, 90 };
	constexpr int DefenseValuePerRefinery = 10;
	constexpr int DefenseValuePerStructure = 5;
	constexpr int DefenseValueExpansionPending = 25;
	constexpr int DefenseValueRecentAttack = 30;
	constexpr int GroundDefenseTypicalValue = 20;
	constexpr int CoverageDefenseCells = 10;
	constexpr int CoverageDefenseBonus = 500;
	constexpr int CarryallDropSearchCells = 6;
	// The search reaches CarryallDropRingCells farther for every CarryallDropStuckFrames the carryall
	// has gone without setting its cargo down, up to CarryallDropMaxRings times.
	constexpr int CarryallDropStuckFrames = 450;
	constexpr int CarryallDropRingCells = 4;
	constexpr int CarryallDropMaxRings = 3;
	// A transport counts as parked to load for this long after the loader last left it so (a
	// transport the loader stops handling is free again).
	constexpr int FerryParkedFrames = 300;
	// A deployer on a new island with no site for its building gives the island up after this long
	// (something passing may only be in the way), and the island stays given up for NoRoomIslandFrames.
	constexpr int DeployerNoSiteFrames = 450;
	constexpr int NoRoomIslandFrames = 9000;
	// The scans for a free island (micro setup) and for the nearest free ore field (building pass)
	// walk thousands of cells and, for each, every building of the house: 19% and 10% of the AI tick
	// when run every tick (tick cost diagnostic, 2026-11-13). Their answers hold this long per house,
	// less when the house gains or loses a building or its home changes.
	constexpr int MapScanKeepFrames = 90;
	// An aircraft the house wants and cannot start for lack of a free dock asks for another airfield
	// (user: the expert in charge should build more structures with docks instead of starting an
	// aircraft that cannot leave; airfields have no fixed cap, they follow the demand). The wish
	// holds AirDockWantedFrames after the pick last made it and allows one airfield more than the
	// house has, so the next one is asked for only when the new docks are taken too.
	constexpr int AirDockWantedFrames = 450;
	// A unit that resource cells hurt goes around a field when the straight way to its goal (the
	// first ResourceLineCells of it) would cost it this share of its health, in legs of
	// ResourceDetourHopSteps along a route that keeps off those cells.
	constexpr int ResourceDetourHealthPercent = 20;
	constexpr int ResourceLineCells = 40;
	constexpr int ResourceDetourHopSteps = 8;
	constexpr int ResourceDetourMaxNodes = 12000;
	// Cell by cell route searches (GroundRouteHop) a house may start in one AI tick.
	constexpr int GroundRouteSearchesPerTick = 2;
	// A narrow passage that stays blocked (user: first try to clear the funnel, and after a minute
	// with the problem spread the units over other crossings). The jam breaker works on a block
	// first. When blocks of ours are still found within JamSpotCells of the same spot
	// JamRerouteFrames later (a gap of more than JamSpotGapFrames starts the count again), half of
	// the units whose way leads past the spot (by id) take a land route that keeps off the narrow
	// cells within JamAvoidCells of it, in legs of JamRerouteHopSteps, and all of them from
	// JamRerouteAllFrames on. The way around is kept JamRerouteHoldFrames after each such block, so
	// units already on it do not turn back, and is not looked for again for JamRerouteRetryFrames
	// when there is none. Only units within JamRerouteReachCells of the spot are sent around.
	constexpr int JamRerouteFrames = 900;
	constexpr int JamRerouteAllFrames = 1800;
	constexpr int JamRerouteHoldFrames = 2700;
	constexpr int JamRerouteRetryFrames = 900;
	constexpr int JamSpotGapFrames = 900;
	constexpr int JamRerouteHopSteps = 20;
	constexpr double JamSpotCells = 8.0;
	// Enough to cut the passage where the block stands; a wider ring also closed the other ways out
	// of a base next to it (no way around in 16 of 16 searches with 10).
	constexpr double JamAvoidCells = 6.0;
	constexpr double JamRerouteReachCells = 60.0;
	// A building cuts the ground where it stands when two ways out of the box LandWayCheckCells
	// around its footprint meet inside the box without it and no longer meet with it. Placement
	// skips such cells like those that close a passage, and counts a way only where two units pass
	// side by side, so a road keeps traffic both ways (match #9724, Frosty Zigzag: towers and a war
	// factory on the edge of a base closed the only way between two plateaus cell by cell, and an
	// army of 265 units stood in front of it for 30000 frames).
	constexpr int LandWayCheckCells = 3;
	// A box wider or deeper than this is not checked (no foundation is that big).
	constexpr int LandWayBoxMaxCells = 24;
	// A narrow passage blocked for JamRerouteAllFrames with no way around it: the cheapest of our
	// buildings within JamSealSearchCells of the block, on the side the units want to go, whose
	// footprint cuts the ground there is sold, at most once per JamSealSellFrames.
	constexpr double JamSealSearchCells = 10.0;
	constexpr int JamSealSellFrames = 1800;
	// Emergency lane (user: when an ambulance comes up a jammed road the cars ease aside as far as
	// they can and open a corridor for it). Every EmergencyLaneCheckFrames, each ground unit of ours
	// with an order that has stood EmergencyLaneWaitFrames on its cell looks EmergencyLaneCells ahead
	// on its path (the straight way to its destination when it holds none); our idle units on those
	// cells step 1..EmergencyLaneSideCells cells aside, across its way rather than along it and off
	// every lane, up to EmergencyLaneMovesPerCheck per check. Logged every EmergencyLaneLogFrames.
	constexpr int EmergencyLaneCheckFrames = 60;
	constexpr int EmergencyLaneWaitFrames = 45;
	constexpr int EmergencyLaneCells = 5;
	constexpr int EmergencyLaneSideCells = 3;
	constexpr int EmergencyLaneMovesPerCheck = 16;
	constexpr int EmergencyLaneLogFrames = 1800;
	// Allied units this close to a block in a narrow passage, heading against it, are the other side
	// of two-way traffic (UpdateJamBreaker).
	constexpr double JamAlliedFlowCells = 10.0;
	// Soldiers shut in a building (FreeSoldiersShutInBuildings): checked every ShutInCheckFrames; one
	// whose path searches from its building cell failed on ShutInFailFrames frames in a row (none more
	// than ShutInFailGapFrames apart) is freed. At most ShutInLogsPerMatch of them logged one by one.
	constexpr int ShutInCheckFrames = 30;
	constexpr int ShutInFailFrames = 3;
	constexpr int ShutInFailGapFrames = 150;
	constexpr int ShutInLogsPerMatch = 40;
	// Shared destination (user: units sent to one cell that is taken should each look for the nearest
	// free cell). Ships of one house sent to the water cell nearest an inland target queued for it in
	// a narrow channel (matches #9771 and #9832, 100000 frames), 29 vehicles to one cell (#9820), and
	// about 50 ships in two packs off the shore of an inland target (#9846). A vehicle, ship or flyer
	// whose destination another unit of ours holds or heads to takes the nearest free cell within
	// DestinationSpreadCells of it: in its zone and, off the air, at most DestinationSpreadLevels
	// height levels from it (never under a cliff). Our units count when their destination, or their
	// cell while they have none, lies within DestinationClaimCells. Logged every DestinationSpreadLogFrames.
	constexpr int DestinationSpreadCells = 5;
	constexpr int DestinationSpreadLevels = 1;
	constexpr double DestinationClaimCells = 8.0;
	constexpr int DestinationSpreadLogFrames = 1800;
	// Naval reach (user: ships that sit idle because nothing they shoot at is in reach). A water-bound
	// unit counts a land target as in reach only when water of its own sea lies within its weapon's
	// reach of it, measured as the engine does against a building (TechnoTypeClass::In_Range). Match
	// #9846: Sea Scorpions (ground gun 5.25 cells) sent at buildings with water 6 cells away and counted
	// for an inland wave target; about 50 sat idle off the enemy island while the house built more.
	// NavalShoreSearchCells: how far the nearest water of a sea is searched. NavalReachMarginLeptons:
	// what a ship that stops off its cell's center may lose. NavalSpawnerReachCells: the reach of a ship
	// whose only land weapon is its aircraft.
	constexpr int NavalShoreSearchCells = 26;
	constexpr int NavalReachMarginLeptons = 64;
	constexpr int NavalSpawnerReachCells = 18;
	// A route leg that would end on a bridge deck runs this many route cells past its far end.
	constexpr size_t RouteLegPastBridgeCells = 4;
	// A ground target off the unit's zone stands for the nearest cell of the zone this near to it.
	constexpr int RouteGoalSnapCells = 4;
	// A search that finds its target walled in by buildings keeps up to RouteWallsKept of those it ran
	// into and logs the RouteWallsLogged nearest to the target, RouteWallLogsPerMatch times per match.
	constexpr size_t RouteWallsKept = 256;
	constexpr size_t RouteWallsLogged = 6;
	constexpr int RouteWallLogsPerMatch = 12;
	// A unit the engine left waiting with its attack-move pending (no path, a blocked way) gets the
	// same order again only after this long.
	constexpr int AttackMoveRetryFrames = 90;
	// A long-range unit gets a direct attack order on an enemy building only within its weapon range
	// plus this many cells, and not while it has stood held for LongRangeHeldFrames.
	constexpr int LongRangeDirectAttackCells = 12;
	constexpr int LongRangeHeldFrames = 300;
	// A raid's ground group turns back when, further than PlanOutgunnedCommittedCells from its goal,
	// the enemy strength around it (units seen within PlanOutgunnedSightCells in the last
	// PlanOutgunnedSightFrames, known defenses that reach it) is over PlanOutgunnedRatioPercent of
	// its own; the plan then waits PlanOutgunnedRetryFrames before it tries again.
	constexpr double PlanOutgunnedCommittedCells = 10.0;
	constexpr double PlanOutgunnedSightCells = 9.0;
	constexpr int PlanOutgunnedSightFrames = 150;
	constexpr int PlanOutgunnedRatioPercent = 150;
	constexpr int PlanOutgunnedRetryFrames = 5400;
	// A raid's ground group also turns back when, before it is committed, the members it has left
	// are worth less than this share of the group that set out: the way there is defended.
	constexpr int PlanEnRouteLossPercent = 70;
	// For PlanLessonFrames after a group failed against a target, only a group worth
	// PlanLessonAdvantagePercent of that one is sent at it again.
	constexpr int PlanLessonFrames = 9000;
	// A raid the gate of startPlan held back is weighed again after this long.
	constexpr int PlanGateRetryFrames = 900;
	constexpr int PlanLessonAdvantagePercent = 150;
	// In-match weight of a plan against an enemy house (PlanEnemyWeight, in percent of the plan's
	// utility, from 100): a plan that destroyed more than it lost against that house gains
	// PlanEnemyWeightUpStep, PlanEnemyWeightFallStep when its target fell too; one that failed or
	// lost more than it destroyed loses PlanEnemyWeightDownStep. A plan that destroyed and lost
	// nothing (sentries that saw nobody) stays as it was.
	constexpr int PlanEnemyWeightMin = 50;
	constexpr int PlanEnemyWeightMax = 200;
	constexpr int PlanEnemyWeightUpStep = 15;
	constexpr int PlanEnemyWeightFallStep = 25;
	constexpr int PlanEnemyWeightDownStep = 20;
	// A ground strike on a building needs this share of the strength known to guard it (defenses
	// within PlanStrikeGuardCells, units seen there in the last PlanStrikeGuardFrames).
	constexpr int PlanStrikeAdvantagePercent = 120;
	// A ground strike takes one in PlanStrikeIdleShare of the idle ground army when that is more
	// than the plan's own group size: the more numerous attack wins more often than not (user), and
	// groups of 4 to 7 bled out before their targets in the focus batches of 2026-11-13.
	constexpr size_t PlanStrikeIdleShare = 2;
	// The army has no ceiling by number (user: the bigger group wins, and a rich house that stopped
	// at 100 soldiers and 70 vehicles put 1.5 million credits into 227 buildings, match #9885).
	// What pauses a queue is its units going unused: that many vehicles, soldiers or ships idle at
	// every AI tick of ArmyIdleWindowFrames.
	enum class ArmyKind : size_t { Vehicles = 0, Infantry = 1, Ships = 2 };
	// With this much in hand a house takes a free island field whatever the ore of its own land.
	constexpr int IslandExpansionRichCredits = 30000;
	// Base ceilings: ground towers an outpost or a front may add over the base cap, per ConYard,
	// and the power surplus over the cushion a crawl stone may still be a plant.
	constexpr int OutpostDefenseRoomPerYard = 6;
	constexpr int CrawlPowerSurplus = 700;
	constexpr int ArmyIdleWindowFrames = 2700;
	constexpr std::array<int, 3> ArmyIdleBrake { 30, 40, 12 };
	// Naval Strike Group (user: ships are of more use wearing the enemy down, and taking the place of
	// the damaged ones, than idle by the dozen; a few may stay by the base to defend it). The whole free
	// long-range fleet goes, NavalStrikeMaxShips at most, and NavalStrikeMaxEscorts escorts.
	// - A reserve stays by our naval yard: NavalHomeReservePercent of the fleet, NavalHomeReserveMin at
	//   least, the ships nearest to the yard. A fleet of NavalHomeReserveMin + 2 ships or fewer keeps
	//   none.
	// - Each shelling ship gets its own firing cell, NavalFiringSpacingCells from the others.
	// - A ship that leaves for repairs is replaced from the free fleet, one per AI tick.
	// It took 6 ships and 4 escorts to one cell: 108 Boomers of a house stood idle while 6 of them
	// jammed at their firing cell (2026-11-17).
	constexpr int NavalStrikeMaxShips = 40;
	constexpr int NavalStrikeMaxEscorts = 8;
	constexpr int NavalHomeReservePercent = 20;
	constexpr int NavalHomeReserveMin = 2;
	constexpr double NavalFiringSpacingCells = 3.0;
	// The shelling ships split into squads of NavalSquadShips, each against its own target,
	// NavalMaxTargets at most, NavalTargetSpacingCells apart (user: a bombardment spread over several
	// targets is chaos for the victim, who stops only some of the attacks).
	constexpr size_t NavalSquadShips = 6;
	constexpr size_t NavalMaxTargets = 6;
	constexpr double NavalTargetSpacingCells = 8.0;
	// Warships pause sooner when the idle ones have nothing to shoot: that many with no coastal
	// building in reach and no enemy ship in their sea at every AI tick of ArmyIdleWindowFrames
	// (match #9846: a house kept building ships while about 50 sat with no target in reach).
	constexpr int ShipsWithoutTargetBrake = 6;
	// Units jammed in their own base count as unused too: a ground unit with somewhere to go that has
	// stood HeldAtHomeFrames on one cell, not firing, within HeldAtHomeCells of home. A wave or a
	// plan takes them again every few hundred frames, so the idle pools alone never showed them
	// (match #10472, Hammer and Sickle: 114 to 181 vehicles while 26 to 58 stood jammed at the
	// base exit and the unused floor read 15, 8 and 4).
	constexpr int HeldAtHomeFrames = 600;
	constexpr double HeldAtHomeCells = 20.0;
	// A repair station serves one unit at a time; the others wait on cells next to it. It takes at
	// most RepairStationQueueUnits repair trips at once (docked, on their way or waiting): the house
	// cap alone (a third of the army) let 30 and more units queue for one depot, in one line that
	// shut the way past it (user, match #10286, Blistering Heatstroke: 40+ Flak Tracks held in a
	// passage by the house's forward base, the followed one at 43% health on Move 3 cells from its
	// goal).
	constexpr int RepairStationQueueUnits = 4;
	// Artillery (main gun reaching ArtilleryRangeCells against the ground) needs a front line: the
	// first ArtilleryFreeVehicles are free, then one more for every ArtilleryFrontLinePerGun armed
	// ground vehicles of other kinds; each one over that costs ArtilleryOverFrontPenalty. The type
	// penalty alone stops at 1200 points, and the artillery slot can stand 2000 and more over the
	// main battle tanks (archetypes 4 and 7, known defenses): 178 V3 and no Rhino in match #10472,
	// one artillery type in 75 to 96% of the vehicle picks of 9 houses in 60 matches.
	constexpr int ArtilleryRangeCells = 8;
	// A main battle tank (IsMainBattleTankType; user: what the mod calls a tank, as a rule of its own
	// rules and never a list of names): its first weapon reaches farther than MainTankMinRangeCells
	// (a suicide truck does not) and, with more Damage than MainTankSpecialDamage, does at least
	// MainTankHeavyVersus of it to heavy armor (a Gattling Tank does 5%, a flame tank 25%).
	constexpr double MainTankMinRangeCells = 1.0;
	// The floor of main battle tanks (user: no house may be left without line tanks, if only to guard
	// and patrol; this is Red Alert and the tanks are the base, whatever the archetype). A house keeps
	// TankFloorMin of them alive, one more for each refinery past TankFloorRefineries (TankFloorEconomyMost
	// more at most), and at least one in TankFloorShare of its armed ground vehicles, TankFloorMost at
	// most. Below it the best scored main battle tank it can build goes ahead of the war factory's
	// pick, except when that pick restores the income (a harvester, a miner, an MCV), with no income
	// at all, or while the base is under attack (the answer to the threat first); and the tanks it
	// asks for are built even while the vehicles we have go unused (IsArmyKindUnused): they guard
	// and patrol. Measured on 22 matches of stamp 03383eb89: below the floor the houses held 1.5 to 4
	// tanks of 4 to 7; most checks with no tank to build were houses with no war factory yet (860
	// before frame 9000, 268 later), the idle army brake stopped the vehicles 4 times.
	constexpr int TankFloorMin = 4;
	constexpr int TankFloorRefineries = 2;
	constexpr int TankFloorEconomyMost = 4;
	constexpr int TankFloorShare = 3;
	constexpr int TankFloorMost = 16;
	// Tank guard (user: the floor's tanks guard the base or patrol for siege units near our bases and
	// expansions; they do not stand still and the waves do not always take them). Half of the main
	// battle tanks (TankGuardShare), TankGuardMin at least while there are that many and
	// TankGuardMost at most, are held back from the waves and the plans, the ones nearest to our
	// sites first (construction yards, refineries, factories). Every TankGuardCheckFrames they go at a
	// known enemy siege unit (a ground gun of ArtilleryRangeCells or more that is no main battle tank,
	// or a deployed siege gun) within TankGuardSiegeCells of a site; with none, each one patrols the
	// sites in turn, TankGuardPostFrames at each. While the base is under attack they are free for its
	// defense.
	// Tank escorts (user: the spare main battle tanks escort the other groups when the house is no
	// tank mass): a marching plan whose walking group holds no main battle tank takes up to
	// PlanTankEscorts free ones (never of the guard) within PlanTankEscortCells of the group, the
	// nearest first; and the plans that pick escorts take main battle tanks before other units.
	constexpr size_t PlanTankEscorts = 2;
	constexpr double PlanTankEscortCells = 30.0;
	constexpr int TankGuardShare = 2;
	constexpr int TankGuardMin = 2;
	constexpr int TankGuardMost = 6;
	constexpr int TankGuardCheckFrames = 90;
	constexpr double TankGuardSiegeCells = 24.0;
	constexpr int TankGuardPostFrames = 1800;
	constexpr double TankGuardSiteCells = 40.0;
	constexpr double TankGuardPostCells = 6.0;
	constexpr int TankGuardBaseAttackFrames = 1350;
	constexpr int MainTankSpecialDamage = 5;
	constexpr double MainTankHeavyVersus = 0.3;
	constexpr int ArtilleryFreeVehicles = 4;
	constexpr int ArtilleryFrontLinePerGun = 2;
	constexpr int ArtilleryOverFrontPenalty = 300;
	// Archetype 0 is the plain basic strategy (user, 2026-11-16): an early economy, several war
	// factories, a mass of main battle tanks with some support and large joint attacks. Every other
	// archetype and every expert is a specialist, and archetype 0 had no prior of its own.
	constexpr int TankMassArchetype = 0;
	// Economy first: it asks for TankMassRefineries refineries without waiting for a bank.
	constexpr int TankMassRefineries = 3;
	constexpr int TankMassRefineryPrior = 500;
	// War factories ahead of the bank, up to TankMassWarFactories: the second with one refinery
	// less than TankMassRefineries, each one after it with a refinery more. The prior comes on top
	// of what the factories owned take off the next one.
	constexpr int TankMassWarFactories = 3;
	constexpr int TankMassFactoryPrior = 700;
	// The tanks: the prior of the main battle tank slot, the share of the type penalty they keep
	// (a mass is many of one type), and the share the other front line vehicles keep (user: very
	// little for the tanks; for the others a group of 6 or 7 of one type can happen without being
	// the rule, the mass is mostly tanks and some mixed in). Artillery has its place in the other
	// archetypes: here a gun needs TankMassFrontLinePerGun front line vehicles at any size, with
	// none free, and each gun over that costs TankMassArtilleryPenaltyPercent of the usual penalty.
	constexpr int TankMassTankPrior = 900;
	constexpr int TankMassTypePenaltyPercent = 20;
	constexpr int TankMassOtherTypePenaltyPercent = 50;
	constexpr int TankMassFrontLinePerGun = 4;
	constexpr int TankMassArtilleryPenaltyPercent = 200;
	// Main battle tanks run artillery down: their slot gains up to this much with the share of
	// artillery in the enemy army seen, for every archetype.
	constexpr int TankCounterArtilleryBonus = 500;
	// Waves: the unit count they start from and the most they wait for. A wave that took no known
	// enemy building within WaveVerdictCells of its target asks the next one for
	// WaveLessonPercent of its count, up to the most and to WaveLessonMaxSteps steps; one that took
	// a building ends the lesson (user: grow the group after each wave until it works, without
	// filling the map). The verdict comes once no unit of ours is left there WaveVerdictQuietFrames
	// after the last dispatch, or after WaveVerdictFrames.
	constexpr int TankMassBaseWave = 10;
	// Building time, not money, is what the tank mass lacks (40 matches from #10431: one war factory
	// until frame 27000 with 14000 credits unspent at frame 18000, a tank every 1000 frames, 24 new
	// buildings between frames 9000 and 27000, mostly defenses and power). Of every four building
	// windows the factories the bank governor wants take TankMassBankWindows, TankMassBankWindowsThreat
	// under threat, and the war factory counts TankMassBankFactoryLead more wanted than it is.
	constexpr int TankMassBankWindows = 3;
	constexpr int TankMassBankWindowsThreat = 2;
	constexpr int TankMassBankFactoryLead = 2;
	// This many idle fighting vehicles able to join a wave go out whatever the strength gate says.
	constexpr int TankMassOverflowVehicles = 24;
	// A fighting unit of the tank mass held at home (HeldAtHomeFrames) joins the idle army its next
	// wave is made of from this health (user: the wave takes the idle offensive units along, which
	// also clears the base).
	constexpr double HeldJoinsWaveHealth = 0.5;
	// How many of them may join after the first wave in a row that took no building; twice as many
	// after each one more, up to HeldJoinsWaveMaxSteps of them (none before). These failures do not
	// fade with time like the wave lesson: joiners never make a wave wait, and the lesson stood at
	// 0 in 92% of the readings of the tank mass (40 matches to #10732), so nearly none joined.
	constexpr int HeldJoinsWaveUnits = 8;
	constexpr int HeldJoinsWaveMaxSteps = 3;
	// TEMP diagnostic: this long after a wave ordered them out, the log says how many of the units
	// held at home that joined it left the base.
	constexpr int HeldJoinerCheckFrames = 900;
	// Each step doubles the count (user): 10, 20, then the most.
	constexpr int TankMassWaveCap = 32;
	constexpr int WaveLessonPercent = 200;
	constexpr int WaveLessonMaxSteps = 2;
	// The lesson fades a step every WaveLessonStepFrames without a new failure: a house that lost
	// its army to three waves was asked for 24 units for 9000 frames with 4 vehicles left, and
	// sent nothing more (match #10262).
	constexpr int WaveLessonStepFrames = 3000;
	// The army of the tank mass comes once its economy stands: with TankMassRefineries refineries
	// and TankMassGatherersPerRefinery gatherers for each, more gatherers and mobile miners wait
	// while its front line is under TankMassBaseWave vehicles (match #10262: a house built 14
	// slave miners and no tank in its first 18000 frames, with two war factories).
	constexpr int TankMassGatherersPerRefinery = 2;
	// Remnant search (user): a house that knows no enemy building while the enemy still has some
	// sends up to RemnantSearchScouts idle units, the fastest first, each to a different place of
	// a grid of RemnantSearchStepCells whose surroundings (RemnantSearchRadius) it knows least
	// (information gain of RemnantSearchMinGain or more), every RemnantSearchEveryFrames (match
	// #10274: 160 vehicles and 95 soldiers stood idle for 12000 frames while six buildings they
	// had never seen kept the match going).
	constexpr size_t RemnantSearchScouts = 12;
	constexpr int RemnantSearchStepCells = 14;
	constexpr int RemnantSearchRadius = 7;
	constexpr int RemnantSearchMinGain = 60;
	constexpr int RemnantSearchEveryFrames = 450;
	// A wounded soldier (under WoundedIdleHealth) left idle more than WoundedIdleHomeCells from
	// home walks back to it (user).
	constexpr double WoundedIdleHealth = 0.55;
	constexpr double WoundedIdleHomeCells = 25.0;
	constexpr double WaveVerdictCells = 14.0;
	constexpr int WaveVerdictQuietFrames = 900;
	constexpr int WaveVerdictFrames = 5400;
	// Missions of the mass (user): its area of influence is the ground within
	// TankMassInfluenceCells of our buildings and our allies'. An armed enemy unit standing there
	// is met whatever its orders (Base Threat Intercept), artillery first. An enemy building
	// within TankMassDangerCells of ours is a wave target before the far ones, more so a defense
	// or a factory; and the buildings of the house that hurt us most in the last
	// TankMassReprisalFrames come before those of the others. That ring grows as it is cleared:
	// with no known enemy building inside it reaches out to the nearest one in steps of
	// TankMassDangerStepCells, up to TankMassDangerMaxCells. While an enemy building stands on the
	// land of one of our sites, both missions count only for the buildings on such land (the mass
	// is not drawn to another island); with none there, for all of them.
	constexpr double TankMassInfluenceCells = 16.0;
	constexpr double TankMassDangerCells = 26.0;
	constexpr double TankMassDangerStepCells = 10.0;
	constexpr double TankMassDangerMaxCells = 76.0;
	// The mass takes anti-air along, as a strong preference and not a rule (user): from
	// TankMassEscortFromVehicles front line vehicles on it wants one vehicle that shoots at
	// aircraft per TankMassVehiclesPerEscort, and the pick of an anti-air vehicle gains
	// TankMassEscortBonus while they are missing. With enemy air presence known (aircraft seen
	// lately, or an airfield of the house the wave goes for) a wave with nothing that shoots at
	// aircraft waits for it while the house can build one, at most TankMassEscortWaitFrames, and
	// then not again for TankMassEscortRetryFrames. With no air presence known, or nothing to
	// build, it goes as it is.
	constexpr int TankMassEscortFromVehicles = 4;
	constexpr int TankMassVehiclesPerEscort = 5;
	constexpr int TankMassEscortBonus = 1200;
	constexpr int TankMassEscortWaitFrames = 1800;
	constexpr int TankMassEscortRetryFrames = 2700;
	constexpr int TankMassNearBuildingBonus = 350;
	constexpr int TankMassDangerBuildingBonus = 300;
	constexpr int TankMassReprisalFrames = 2700;
	constexpr int TankMassReprisalBonus = 450;
	// The mass goes in where the way is least defended (user): the danger of the straight way from
	// home to a target, on the ground danger map of the crossings (GetOpDangerMap: the weapons of
	// the known enemy defenses and units that hit the ground), comes off its score, a point per
	// TankMassWayDangerPerPoint of danger and TankMassWayDangerMost at most, so it settles targets
	// of about the same worth and overrules no mission. And a known power plant whose loss leaves
	// its owner short of power, by the buildings of his we know, gains TankMassPowerCutBonus when
	// he has TankMassPowerCutDefenses known defenses that need power (user: without power the
	// base opens).
	constexpr int TankMassWayDangerPerPoint = 2;
	constexpr int TankMassWayDangerMost = 450;
	constexpr int TankMassPowerCutBonus = 300;
	constexpr int TankMassPowerCutDefenses = 2;
	// Plan priorities of the mass: the plans that send the army together gain this much on their
	// trained preference, the ones that split it into small groups or need specialists lose it.
	constexpr int TankMassPlanBonus = 400;
	// A unit a wave sent out keeps that goal for WaveGoalKeepFrames while it is still more than
	// WaveGoalKeepCells from it and a known enemy building stands within that distance of it. The
	// wave target is picked anew at every tick; a wave to the other side of the map turned units
	// held in a jam round, and the next one turned them back (match #10472: 43 vehicles east at
	// frame 101565, the same 43 west at frame 101655, in one-cell streets).
	constexpr int WaveGoalKeepFrames = 2700;
	constexpr double WaveGoalKeepCells = 12.0;
	// The house itself leans to the target of its last dispatch for WaveTargetKeepFrames: enemy
	// buildings within WaveGoalKeepCells of it score WaveTargetKeepBonus more, above the focus
	// enemy (300) and below the coalition target (700). 189 of 20504 launches of 48 houses went
	// more than 12 cells from the launch before within 900 frames, up to 26 per house.
	// A wave goes first for what its group brings down fast and last for what it barely scratches
	// (user): the damage the idle army does to a building type in 900 frames (damage, burst, verses
	// its armor, rate of fire of the better weapon of each type) as a percent of its strength. At
	// WaveGroupDamageEvenPercent it changes nothing; from WaveGroupDamageFastPercent on the target
	// gains WaveGroupDamageMost, at WaveGroupDamageSlowPercent or under it loses as much, in
	// proportion between. Less than the bonus of the target a wave keeps, so the target does not
	// change with every unit that joins. The army counted is that of the last tick, no older than
	// WaveGroupFreshFrames.
	// Where the enemy aircraft rearm (user): a known airfield or helipad gains, as a wave target and
	// as an air strike target, a share of the value of that enemy's aircraft seen lately
	// (EnemyAirValueByOwner): AirBaseWavePercent of it up to AirBaseWaveMost on the score of a
	// wave target, AirBaseStrikePercent up to AirBaseStrikeMost on the worth of an air strike.
	constexpr int AirBaseWavePercent = 50;
	constexpr int AirBaseWaveMost = 250;
	constexpr int AirBaseStrikePercent = 30;
	constexpr int AirBaseStrikeMost = 150;
	constexpr int WaveGroupDamageEvenPercent = 100;
	constexpr int WaveGroupDamageFastPercent = 400;
	constexpr int WaveGroupDamageSlowPercent = 25;
	constexpr int WaveGroupDamageMost = 200;
	constexpr int WaveGroupFreshFrames = 60;
	constexpr int WaveTargetKeepFrames = 900;
	constexpr int WaveTargetKeepBonus = 400;
	// A war factory keeps a road out: two-way ground (cells of walkable 2x2 squares) from its exit
	// lane to a cell more than FactoryRoadClearCells from every building of ours, found within
	// FactoryRoadWalkCells cells. Buildings stand at most one cell apart (Adjacent), so a base is
	// a grid of one-cell streets, and a factory closed in by it puts every vehicle it builds into
	// them (match #10472: 11 war factories, 26 to 58 units held at the base exit for 43000
	// frames). At most FactoryRoadWalksPerFrame walks per frame.
	constexpr int FactoryRoadClearCells = 4;
	constexpr int FactoryRoadWalkCells = 400;
	constexpr int FactoryRoadWalksPerFrame = 48;
	// What a cell that closes such a road loses in the pass that takes tight cells (one that
	// touches another building loses 4000 there).
	constexpr int FactoryRoadClosedPenalty = 6000;
	// A war factory with no road out is this much farther (squared cells) from the front when the
	// primary one is picked: every vehicle leaves by the primary factory, so one with a road comes
	// first however far it stands.
	constexpr double ClosedInFactoryDistanceSq = 1.0e12;
	// The cure when every war factory is closed in (SellFactoryRoadBlock, user: sell what closes
	// the road unless it is a critical structure): with FactoryRoadSellHeldVehicles vehicles
	// jammed at home for FactoryRoadSellHeldFrames (the stretch ends when fewer than half are),
	// looked at every FactoryRoadSellCheckFrames, one sale per FactoryRoadSellFrames, trying the
	// FactoryRoadSellTests cheapest spare buildings on the factories' pockets.
	constexpr int FactoryRoadSellHeldVehicles = 10;
	constexpr int FactoryRoadSellHeldFrames = 1800;
	constexpr int FactoryRoadSellCheckFrames = 300;
	constexpr int FactoryRoadSellFrames = 1800;
	constexpr size_t FactoryRoadSellTests = 24;
	// Units shut in a pocket (user: sell the building of ours that closes it; the house builds it
	// again elsewhere): ground units whose path searches from where they stand have failed for
	// PocketShutFrames, PocketShutFails times or more, none more than PocketFailGapFrames apart, to
	// more than one place, without leaving the PocketStepCells around their first cell. With
	// PocketShutUnits of them in one pocket (the ground they reach without crossing a building, a
	// cliff or water, PocketMaxCells at most: more is open ground), the cheapest building of ours
	// on its edge that the house can spare, or a factory of a type it owns twice, whose sale alone
	// opens it is sold: looked at every PocketCheckFrames, one sale per PocketSellFrames, trying
	// the PocketSellTests cheapest. Nothing is built on its ground for PocketClearFrames.
	// A wave target on our own ground that the path finder does not reach (a fallen bridge; user:
	// the transports do the hard work until the ground can be used again, and only with no way to
	// send the units is that target left for other missions). While the house owns a transport its
	// operations use and a landing lies by the target, the wave keeps it and hands its units more
	// than PathDeadFerryNearCells from it to a transport operation to that landing
	// (RequestStrikeTransport), for PathDeadFerryTryFrames from the first one; then the target is
	// passed over like any path dead goal for PathDeadFerryRestFrames. The goals within
	// PathDeadFerryGoalCells of the ferried target (the target, the secondary one, a goal a unit
	// keeps from an earlier wave) are the same target: they share its landing and its try, so the
	// wave rides one operation (match #10793: 7 operations, one per landing, in 900 frames). A
	// fallen bridge within BridgeDeadGoalCells of such a place is repaired whatever the zone map
	// says of its banks.
	constexpr int PathDeadFerryTryFrames = 5400;
	constexpr int PathDeadFerryRestFrames = 9000;
	constexpr double PathDeadFerryGoalCells = 24.0;
	constexpr double PathDeadFerryNearCells = 24.0;
	constexpr double BridgeDeadGoalCells = 30.0;
	constexpr int PocketShutFrames = 1800;
	constexpr int PocketShutFails = 30;
	constexpr int PocketFailGapFrames = 450;
	constexpr double PocketStepCells = 4.0;
	constexpr size_t PocketShutUnits = 3;
	constexpr size_t PocketMaxCells = 200;
	constexpr int PocketCheckFrames = 300;
	constexpr int PocketSellFrames = 900;
	constexpr size_t PocketSellTests = 8;
	constexpr int PocketClearFrames = 9000;
	constexpr int PocketLogFrames = 1800;
	// Long-range gun towers with a dead zone (IsDeadZoneTower; user: a very advanced defense with
	// a long reach, to be taken into account). The tier bonus of a tower stops at 950, so nothing
	// told a gun of 15 cells from one of 7:
	// - its reach scores LongRangeTowerBonusPerCell per cell over ArtilleryRangeCells, up to
	//   LongRangeTowerBonusMax, and CounterBatteryBonusPerShare per point of seen artillery share
	//   (up to CounterBatteryBonusMax) when that artillery does not out-reach it;
	// - it is no first line: short of the core ground towers it loses DeadZoneTowerEarlyPenalty;
	// - held back by power alone, it asks for a plant for DefensePowerWantedFrames (its drain
	//   plus the margin a powered defense needs counts as the power cushion);
	// - placement gives DeadZoneCoveredBonus to a cell with a point-blank ground tower of ours
	//   within its dead zone plus DeadZoneCoverMarginCells, and takes DeadZoneAlonePenalty off
	//   one with none.
	constexpr int LongRangeTowerBonusPerCell = 40;
	constexpr int LongRangeTowerBonusMax = 300;
	constexpr int CounterBatteryBonusPerShare = 8;
	constexpr int CounterBatteryBonusMax = 400;
	constexpr int DeadZoneTowerEarlyPenalty = 1500;
	constexpr int DefensePowerWantedFrames = 300;
	constexpr int PoweredDefenseSurplusMargin = 75;
	constexpr int DeadZoneCoverMarginCells = 3;
	constexpr int DeadZoneCoveredBonus = 350;
	constexpr int DeadZoneAlonePenalty = 450;
	constexpr double PlanStrikeGuardCells = 12.0;
	// A ground group is weighed against what it meets on its way in, not only at the target: what
	// is known within PlanStrikeGuardCells of the last PlanStrikeApproachCells of the straight way
	// from home (a point every PlanStrikeApproachStepCells). A raid dies at the edge of the enemy
	// base: with the target's own guard alone, groups of 7 lost a third of their value 30 to 60
	// cells short of it (251 of 387 Factory Decapitation plans in the focus batch of 2026-11-13).
	// Airborne Front: its transport comes this near the nearest of its soldiers before it parks,
	// and a soldier this near a parked transport on each axis goes aboard (a cell and a half: the
	// cells next to it).
	constexpr int BoardingMeetCells = 4;
	constexpr int BoardingReachLeptons = 384;
	// Airborne Front, a flying transport: the cell it lands on is looked for up to this many
	// cells from the nearest soldier; within BoardingLandingNearCells of that cell it gets
	// BoardingLandingFrames to touch down before it is sent to another.
	constexpr int BoardingLandingRingCells = 7;
	constexpr double BoardingLandingNearCells = 3.0;
	constexpr int BoardingLandingFrames = 150;
	// Airborne Front: the team is this many strong, or fills a smaller transport, and is drawn
	// from within AirborneMusterCells of the soldier nearest to the transport, which is within
	// AirborneSurfaceMeetCells of that soldier unless it flies. Teams of one or two did nothing
	// once dropped, and soldiers spread over the base kept the transport waiting.
	constexpr int AirborneTeamMin = 4;
	// Airborne Front: a plan that ends with soldiers aboard gets this long to take them home and
	// let them out.
	constexpr int AirborneStandDownFrames = 1800;
	constexpr double AirborneMusterCells = 20.0;
	constexpr double AirborneSurfaceMeetCells = 40.0;
	// A unit this long on an Attack mission with no target is put back on guard.
	constexpr int AttackWithNoTargetFrames = 45;
	// Airborne Front: its team is taken from soldiers this near a building of ours.
	constexpr double AirborneHomeCells = 15.0;
	// A parked Airborne Front transport sets out for its soldiers again only when the nearest one
	// is this far.
	constexpr int BoardingParkedReachCells = 12;
	// Airborne Front, counted from the moment its transport parked by its soldiers: it leaves with
	// a part of them after BoardingPartialLoadFrames, and gives up with nobody aboard after
	// BoardingWaitFrames. A transport that has not parked BoardingApproachFrames into the plan is
	// given up too.
	constexpr int BoardingPartialLoadFrames = 900;
	constexpr int BoardingWaitFrames = 1500;
	constexpr int BoardingApproachFrames = 2700;
	// What a wave target costs while the path finder does not reach it: more than any bonus of the
	// target score adds up to, so it is taken only when there is no other.
	constexpr int WavePathDeadGoalPenalty = 5000;
	// Airborne Front, when its target falls before the drop: how many times the team takes another
	// and the time it gets for each.
	constexpr int AirborneRetargets = 2;
	constexpr int AirborneRetargetFrames = 1800;
	// Airborne Front: the time the trip gets from the moment the transport leaves and the time the
	// team gets on the ground, of which it is reckoned to fire 1 part in AirborneAssaultShare; a transport badly hit goes on when it is this near its drop cell,
	// and one that has let nobody out for AirborneDropRetryFrames tries another cell;
	// the target stands between AirborneFrontGapCells and AirborneFrontReachCells from where the
	// main attack is aimed; the drop cell is looked for AirborneDropRingCells from the target,
	// and what is known within AirborneDropGuardCells of each candidate decides between them.
	constexpr int AirborneTripFrames = 2700;
	constexpr int AirborneAssaultFrames = 2700;
	constexpr int AirborneAssaultShare = 2;
	constexpr double AirbornePressOnCells = 15.0;
	constexpr int AirborneDropRetryFrames = 450;
	constexpr double AirborneFrontGapCells = 10.0;
	constexpr double AirborneFrontReachCells = 40.0;
	constexpr double AirborneDropRingCells = 5.0;
	constexpr double AirborneDropGuardCells = 7.0;
	// A parked transport on the same cell this long whose locomotor still reports movement is
	// told to stop.
	constexpr int BoardingSettleFrames = 90;
	// Preemptive Strike goes for a gathering enemy force this near the group that would go (about
	// 750 frames of march), and follows it this far from where it was gathering.
	constexpr double PreemptiveReachCells = 45.0;
	constexpr double PreemptiveLeashCells = 15.0;
	// A gathering force: ground units seen within PreemptiveClusterCells of each other, at least
	// PreemptiveMinValue of them standing (no more than EnemyStandCells from where they were) for
	// PreemptiveGatherFrames. Any force seen in the last 150 frames was taken before, and 10 of 18
	// plans went for one already on its way: it was 15 cells from its start point before the
	// group got there, 600 frames into the plan at the median (2026-11-15).
	constexpr double EnemyStandCells = 4.0;
	constexpr int PreemptiveGatherFrames = 300;
	constexpr double PreemptiveClusterCells = 8.0;
	constexpr int PreemptiveMinValue = 140;
	// The group: the free units nearest to the force, each within PreemptiveReachCells of it. They
	// meet PreemptiveStageCells short of the force (out of reach of a force spread over
	// PreemptiveClusterCells) and strike together once two thirds of them are within
	// PreemptiveStageNearCells of that spot, PreemptiveStageWaitFrames into the plan, or once the
	// fight has begun. The first twelve free units of the whole base, with only their centre within
	// reach, came one by one: 6 of 18 plans lost a third of their value with the centre of the group
	// 27 to 52 cells short and the head 4 to 19 cells from the force.
	constexpr size_t PreemptiveMinMembers = 5;
	constexpr size_t PreemptiveMaxMembers = 12;
	constexpr double PreemptiveStageCells = 16.0;
	constexpr double PreemptiveStageNearCells = 5.0;
	constexpr int PreemptiveStageWaitFrames = 600;
	// A force this near our nearest member is in the fight: the whole group strikes, and it is
	// fought wherever it goes from there (it came at us).
	constexpr double PreemptiveEngageCells = 8.0;
	// Counter Squad: an enemy house has CounterSquadForceUnits or more units of one force category
	// (infantry, vehicles, artillery, ships, aircraft) seen within CounterSquadClusterCells of each
	// other in the last CounterSquadSeenFrames. Our free units that counter that category go at the
	// largest such force together: their fit against it (CounterFitOf) is at least CounterSquadFitMin
	// and CounterSquadFitEdge points above the value-weighted mean fit of our whole army. The squad
	// counts each member's value times its health and fit; best counters first, it takes members
	// until it is worth CounterSquadEnoughPercent of what stands there (every unit seen within
	// CounterSquadClusterCells of the force and the known defenses there) or has
	// CounterSquadMaxMembers, and starts with CounterSquadMinMembers or more worth
	// CounterSquadAdvantagePercent of it (by plain value no less than the outgunned turn-back of the
	// raids asks for). A force no squad can take on yields to the next largest one.
	constexpr int CounterSquadForceUnits = 9;
	constexpr double CounterSquadClusterCells = 10.0;
	constexpr int CounterSquadSeenFrames = 450;
	constexpr int CounterSquadFitMin = 80;
	constexpr int CounterSquadFitEdge = 25;
	constexpr size_t CounterSquadMinMembers = 4;
	constexpr size_t CounterSquadMaxMembers = 20;
	constexpr int CounterSquadAdvantagePercent = 110;
	constexpr int CounterSquadEnoughPercent = 200;
	// Unless the force is within CounterSquadHomeCells of home or CounterSquadOwnBuildingCells of one
	// of our buildings, CounterSquadHomeKeepPercent of the value of the free units within
	// CounterSquadHomeCells of home stays there (the base threat intercept keeps the same reserve).
	constexpr double CounterSquadHomeCells = 18.0;
	constexpr double CounterSquadOwnBuildingCells = 12.0;
	constexpr int CounterSquadHomeKeepPercent = 33;
	// A land squad meets CounterSquadStageCells short of the force and strikes together once two
	// thirds of it are within CounterSquadStageNearCells of that spot, CounterSquadStageWaitFrames
	// into the plan, or once the force is within CounterSquadEngageCells of a member; ships strike at
	// once. The squad follows the force (its category seen in the last CounterSquadTrackFrames within
	// CounterSquadClusterCells of where it was) and is sent again when it moved CounterSquadReaimCells
	// from where the members were sent. It went well when the force fell to CounterSquadBrokenPercent
	// of its value at the start with half of the squad at it, or when the members destroyed enemy
	// units worth CounterSquadKillPercent of it; badly when the force was not seen for
	// CounterSquadLostFrames, the squad fell to CounterSquadKeptPercent of its value or its time ran
	// out. With no force or no squad it looks again after CounterSquadRetryFrames.
	constexpr double CounterSquadStageCells = 14.0;
	constexpr double CounterSquadStageNearCells = 5.0;
	constexpr int CounterSquadStageWaitFrames = 600;
	constexpr double CounterSquadEngageCells = 8.0;
	constexpr int CounterSquadTrackFrames = 300;
	constexpr double CounterSquadReaimCells = 6.0;
	constexpr int CounterSquadBrokenPercent = 35;
	constexpr int CounterSquadKillPercent = 60;
	constexpr int CounterSquadLostFrames = 900;
	constexpr int CounterSquadKeptPercent = 40;
	constexpr int CounterSquadDurationFrames = 3000;
	constexpr int CounterSquadRetryFrames = 300;
	// Hold Under Towers: a ground enemy force of at least HoldTowersMinEnemyValue within
	// HoldTowersEnemyCells of home, over HoldTowersOddsPercent of our free army within
	// HoldTowersArmyCells, and an army of at least HoldTowersMinMembers: 19 of the 27 holds whose
	// whole group died were of one or two units (3 of 24 such holds went well, 8 of 29 from three
	// on, 2026-11-15). The attack is over when the ground enemy near home fell to
	// HoldTowersRepelledPercent of its peak (killed or gone: no enemy at all within 30 cells never
	// happens in a match of six or eight houses, so intact groups ran out of time); at the
	// deadline the hold went well if the group kept HoldTowersKeptPercent of its value and the
	// enemy near home is no longer that much stronger than it.
	constexpr double HoldTowersEnemyCells = 30.0;
	constexpr double HoldTowersArmyCells = 40.0;
	constexpr int HoldTowersMinEnemyValue = 120;
	constexpr int HoldTowersOddsPercent = 130;
	constexpr size_t HoldTowersMinMembers = 3;
	constexpr int HoldTowersRepelledPercent = 30;
	constexpr int HoldTowersKeptPercent = 50;
	// A hold starts only when the army and our ground defenses within HoldTowersDefenseCells of the
	// rally are worth HoldTowersStandPercent of the force: below that the towers do not save the
	// army, it died about 525 frames in (focus batch of 12 matches, 2026-11-15: 4 of 22 such holds
	// went well, 51 of 74 from there on).
	constexpr double HoldTowersDefenseCells = 10.0;
	constexpr int HoldTowersStandPercent = 100;
	// Combo Watch Strike: a sentry spotted a force when a ground enemy of it within
	// ComboWatchSightCells of the sentry was seen in the last ComboWatchSightFrames; the follow-up
	// it leads into has ComboWatchStartFrames to start. It started on any sighting before, whether
	// or not a follow-up had a force to act on, and 60 of 63 ran out of time.
	constexpr double ComboWatchSightCells = 9.0;
	constexpr int ComboWatchSightFrames = 30;
	constexpr int ComboWatchStartFrames = 900;
	// Bait Ambush: how long the bait fires at the building with no answer before the plan gives up
	// (the intercept set out 300 to 400 frames into the fire, after a wait of 450 had given up on
	// 9 of 65), and how long the ambush waits for a chaser once the bait is back behind it.
	constexpr int BaitLureWaitFrames = 900;
	constexpr int BaitChaseWaitFrames = 450;
	// A bait nobody answered is not tried again for this long: giving up sooner made the plan start
	// twice as often (272 plans in 18 matches against 127 in 40).
	constexpr int BaitRetryFrames = 5400;
	// A Neural AI house sends its intercept group at whoever fires at one of its buildings and follows
	// it until it is 8 cells farther from the building than when the intercept began. The bait begins
	// to fire from its weapon range (4 to 6 cells), so the answer comes out 12 to 14 cells and the
	// ambush waits BaitAmbushCells from the building on the way home; the ambush of the old design
	// stood 23 cells or more away and no chaser ever reached it. Neither the building nor the ambush
	// has a known defense within BaitTowerClearCells, and the building is BaitLureMinCells from home.
	constexpr double BaitAmbushCells = 12.0;
	constexpr double BaitTowerClearCells = 9.0;
	constexpr double BaitLureMinCells = 35.0;
	constexpr size_t BaitIdleMin = 7;
	constexpr size_t BaitAmbushSize = 8;
	constexpr int BaitPlanFrames = 4500;
	// The bait sets out once half the ambush stands within BaitInPlaceCells of its cell, or after
	// BaitSetFrames (baits sent then went well as often as the others: 4 of 21 against 2 of 13), and
	// has BaitApproachFrames to come within range of the building.
	constexpr double BaitInPlaceCells = 5.0;
	constexpr int BaitSetFrames = 1800;
	constexpr int BaitApproachFrames = 900;
	// The bait runs back once it lost BaitHurtPercent of its health while luring or enemy value of
	// BaitThreatValue is seen within BaitThreatCells of it; the ambush springs on that much seen within
	// BaitAmbushSightCells of its cell and fights what is seen within BaitFightCells.
	constexpr int BaitHurtPercent = 15;
	constexpr int BaitThreatValue = 40;
	constexpr double BaitThreatCells = 8.0;
	constexpr double BaitAmbushSightCells = 9.0;
	constexpr double BaitFightCells = 12.0;
	// A bait ambush went well when its members destroyed enemy units worth this much and no less than
	// the value it lost: the old test (60 enemy value seen near the ambush) counted any wave passing by.
	constexpr int BaitKillValue = 60;
	// Feint: a decoy of FeintDecoySize fast units fires at a building of the house our main attack is
	// going for, FeintGapCells to FeintReachCells from where that attack is aimed and with no known
	// defense within FeintTowerClearCells. A wave counts as our main attack for FeintWaveFrames after
	// it was sent out.
	constexpr double FeintGapCells = 25.0;
	constexpr double FeintReachCells = 60.0;
	constexpr double FeintTowerClearCells = 8.0;
	constexpr int FeintWaveFrames = 600;
	constexpr size_t FeintDecoySize = 3;
	constexpr size_t FeintIdleMin = 6;
	constexpr int FeintPlanFrames = 3600;
	// It went well once enemy value of FeintDrawPercent of the decoy's own is seen within FeintDrawCells
	// of the decoy while it is within FeintNearCells of its target. It ends badly with less than
	// FeintLossPercent of its value left, or FeintAnswerWaitFrames after it arrived with no answer.
	constexpr double FeintNearCells = 20.0;
	constexpr double FeintDrawCells = 10.0;
	constexpr int FeintDrawPercent = 100;
	constexpr int FeintLossPercent = 50;
	constexpr int FeintAnswerWaitFrames = 900;
	// An air group goes for a building only when this many sorties of its full load add up to the
	// building's health: Air Strike sent two aircraft at buildings they could not bring down in
	// its 2700 frames, and 128 of 163 plans ran out of time (focus batch of 2026-11-14).
	constexpr int PlanAirStrikeSorties = 2;
	constexpr double PlanStrikeApproachCells = 40.0;
	constexpr double PlanStrikeApproachStepCells = 8.0;
	constexpr int PlanStrikeGuardFrames = 900;
	// Rolling Siege: the battery goes with RollingSiegeMinEscorts..RollingSiegeMaxEscorts armed
	// ground escorts (one in PlanStrikeIdleShare of the idle ones when that is more than three),
	// plus RollingSiegeAntiAirEscorts mobile anti-air, and moves on to a defense at most
	// RollingSiegeChainCells from the last one. The escort was at most three and could be none:
	// 13 of 40 plans went with five members or fewer, three of them guns (archive of 2026-11-14
	// night).
	constexpr size_t RollingSiegeMinEscorts = 2;
	constexpr size_t RollingSiegeMaxEscorts = 8;
	constexpr size_t RollingSiegeAntiAirEscorts = 2;
	constexpr double RollingSiegeChainCells = 20.0;
	// Rolling Siege firing spots around a defense, as fractions of the stand-off distance toward
	// home and to its side: straight toward home first, then 30 and 60 degrees to either side.
	constexpr std::pair<double, double> RollingSiegeStageDirections[] = { { 1.0, 0.0 }, { 0.866, 0.5 }, { 0.866, -0.5 },
		{ 0.5, 0.866 }, { 0.5, -0.866 } };
	// Rolling Siege escorts wait RollingSiegeEscortBackCells behind the firing spot (toward home),
	// within RollingSiegeEscortSpreadCells of that post, and one that comes within the reach of the
	// shelled defense + RollingSiegeEscortMarginCells goes back to it.
	constexpr double RollingSiegeEscortBackCells = 3.0;
	constexpr double RollingSiegeEscortSpreadCells = 3.0;
	constexpr double RollingSiegeEscortMarginCells = 1.5;
	// Siege Battery: at most this many guns, and ground escorts as Rolling Siege's (one in
	// PlanStrikeIdleShare of the idle ones, at least three when there are) up to this many. It went
	// with three escorts at most: 80 of 117 failed plans lost their strength (2026-11-15).
	constexpr size_t SiegeBatteryMaxGuns = 6;
	constexpr size_t SiegeBatteryMaxEscorts = 6;
	// Superweapon Race: the ground group has this many units at least and takes at most this many,
	// or one in PlanStrikeIdleShare of the idle ground army when that is more.
	constexpr size_t SuperweaponRaceMinGround = 4;
	constexpr size_t SuperweaponRaceMaxGround = 12;
	// Blackout: the ground group has this many units that hurt the plant at least and takes at most
	// this many, or one in PlanStrikeIdleShare of the idle ground army when that is more. It took
	// the five fastest idle units: 1 of 14 plans ended well (2026-11-15).
	constexpr size_t BlackoutMinGround = 4;
	constexpr size_t BlackoutMaxGround = 8;
	// A plan's ground column keeps together on its way (Superweapon Race, Rolling Siege): a member
	// more than PlanMarchBandCells nearer the goal than the column's rear waits for it, until the
	// rear is within PlanMarchCommittedCells of the goal.
	constexpr double PlanMarchBandCells = 6.0;
	constexpr double PlanMarchCommittedCells = 14.0;
	// A unit that resource cells heal, wounded below ResourceHealBelow, stops on one within
	// ResourceHealSearchCells until it is back at ResourceHealUntil, for ResourceHealMaxFrames at
	// most, and then not again for ResourceHealCooldownFrames.
	constexpr double ResourceHealBelow = 0.7;
	constexpr double ResourceHealUntil = 0.95;
	constexpr int ResourceHealSearchCells = 8;
	constexpr int ResourceHealMaxFrames = 900;
	constexpr int ResourceHealCooldownFrames = 900;

	bool IsFerryReserved(const HouseClass* pHouse, DWORD uid)
	{
		auto const& reserved = GetMacroState(pHouse).FerryReserved;
		auto const it = reserved.find(uid);

		return it != reserved.end() && Unsorted::CurrentFrame - it->second.Frame < FerryReservationFrames
			&& Unsorted::CurrentFrame >= it->second.Frame;
	}

	// A transport with passengers reserved is loading and takes no other job meanwhile.
	bool IsFerryLoading(const HouseClass* pHouse, DWORD transportUid)
	{
		for (auto const& entry : GetMacroState(pHouse).FerryReserved)
		{
			if (entry.second.Transport == transportUid)
				return true;
		}

		return false;
	}

	// A transport the loader left parked to load lately. The combat micro and the waves took it for
	// a combat unit (armed or not) and sent it off on attack-moves and strikes while its passengers
	// walked up: it takes its orders from the loader alone.
	bool IsFerryParkedToLoad(const HouseClass* pHouse, DWORD uid)
	{
		auto const& parked = GetMacroState(pHouse).ParkedFerries;
		auto const it = parked.find(uid);

		return it != parked.end() && Unsorted::CurrentFrame - it->second.Frame <= FerryParkedFrames
			&& Unsorted::CurrentFrame >= it->second.Frame;
	}

	// A unit or transport working for a transport operation (NeuralAI.Operations.inl): it takes its
	// orders from the operation alone until the operation lets it go.
	bool IsInTransportOp(const HouseClass* pHouse, DWORD uid)
	{
		return GetMacroState(pHouse).TransportOpOf.contains(uid);
	}

	// True while UpdateTransportOperations gives its orders. The order helpers (IssueMoveOrder,
	// OrderFootToAttackMove, OrderFootToEnter, DispatchFootToTargetOrTransport) turn the units of the
	// operations down for every other part of the AI tick: an order the operation then finds on one
	// of them is logged and overridden, and the trace of 46 matches (2026-11-16) found 357 such
	// orders from nine parts that only had to leave them out.
	bool TransportOpOrdersOpen = false;

	bool IsHeldByTransportOp(const FootClass* pFoot)
	{
		return !TransportOpOrdersOpen && pFoot && pFoot->Owner && IsInTransportOp(pFoot->Owner, pFoot->UniqueID);
	}

	// A unit that gave up waiting for a crossing or lost its boarding turn: no transport takes it for a while.
	bool IsFerryBlocked(const HouseClass* pHouse, DWORD uid)
	{
		auto const& blocked = GetMacroState(pHouse).FerryBlockedUntil;
		auto const it = blocked.find(uid);

		return it != blocked.end() && Unsorted::CurrentFrame < it->second;
	}

	// Meaning version of the output slots whose meaning changed, sent to the trainer in every
	// telemetry (slotMeanings): a slot whose version grows starts again from zero there, with the
	// memory of an archetype whose slot it is. Whoever changes what a slot drives bumps its version
	// here in the same change. 48, 93, 108: archetype 0 became the tank mass and the slots of Feint
	// and Bait Ambush went to its Reprisal and Clear Our Ground missions (2026-11-16).
	constexpr std::pair<int, int> NeuralSlotMeanings[] = { { 48, 1 }, { 93, 1 }, { 108, 1 } };

	// Production queues of a type, as ProductionBanUntil keys them: 1 vehicles, 2 infantry, 3 ships,
	// 4 aircraft, 0 for anything else.
	int ProductionQueueOf(const TechnoTypeClass* pType)
	{
		if (!pType)
			return 0;

		switch (pType->WhatAmI())
		{
		case AbstractType::UnitType:
			return static_cast<const UnitTypeClass*>(pType)->Naval ? 3 : 1;
		case AbstractType::InfantryType:
			return 2;
		case AbstractType::AircraftType:
			return 4;
		default:
			return 0;
		}
	}

	// A type banned by WatchUndeliveredPick (queue as in ProductionQueueOf).
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
	// over and then the one it left weakest, a dying house before its aggressor gets there, the
	// poorest enemy or one that lost its economy (it falls sooner); while we lead by far, no far
	// adventures against houses that leave us alone.
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

		if (idx == coal.PoorTarget)
			bias += PoorTargetBias;

		if (wave && coal.LowProfile && idx != coal.LowProfileNearest && idx != coal.Target
			&& now - coal.HurtByFrame[idx] > 5400)
		{
			bias -= 300;
		}

		return bias;
	}

	// The diplomacy motive that weighs most for a house in DiplomacyTargetBias (main enemy log).
	const char* DiplomacyTargetMotive(HouseClass* pHouse, int idx)
	{
		if (!pHouse || idx < 0 || idx >= 8)
			return "diplomacy";

		auto const& coal = GetMacroState(pHouse).Coalition;
		const int now = Unsorted::CurrentFrame;

		if (idx == coal.Target)
			return "coalition";

		if (idx == coal.DyingTarget && now < coal.DyingUntil)
			return "dying";

		if (idx == coal.PostDuelTarget && now < coal.PostDuelUntil)
			return "after a duel";

		if (idx == coal.PoorTarget)
			return coal.PoorTargetLost ? "lost economy" : "poorest";

		return "diplomacy";
	}

	// Weight of a mission of the tank mass on the target of its main wave: its base, moved by half
	// of the trained preference of its slot, from nothing to twice the base.
	int TankMassMissionBonus(int base, int slotScore)
	{
		return std::clamp(base + slotScore / 2, 0, base * 2);
	}

	// Plan priorities of an archetype: points added to the trained preference of a plan at its
	// gate (shouldTryStart). The trained preference is one per plan for the whole policy, so
	// without this every archetype runs the same plans with the same priorities. Only the tank
	// mass has its own so far: the plans that send the army together and the ones that defend its
	// ground come first, the ones that split it into small groups or need specialists last. They
	// are shifts, not vetoes: a plan the situation asks for loudly still starts.
	int ArchetypePlanOffset(int archetype, NeuralPlanId id)
	{
		if (archetype != TankMassArchetype)
			return 0;

		switch (id)
		{
		case NeuralPlanId::TimingRush:
		case NeuralPlanId::Pincer:
		case NeuralPlanId::ThreeFrontAssault:
		case NeuralPlanId::AllyJointAttack:
		case NeuralPlanId::ForwardRepair:
		case NeuralPlanId::PreemptiveStrike:
		case NeuralPlanId::BaseThreatIntercept:
		case NeuralPlanId::HuntKeyUnits:
		case NeuralPlanId::ComboBlackoutPush:
			return TankMassPlanBonus;
		case NeuralPlanId::EcoRaid:
		case NeuralPlanId::HarvesterAmbush:
		case NeuralPlanId::RotatingRaids:
		case NeuralPlanId::SiegeBattery:
		case NeuralPlanId::ArtilleryEcoSiege:
		case NeuralPlanId::RollingSiege:
		case NeuralPlanId::SilentInfiltration:
		case NeuralPlanId::EngineerAssault:
		case NeuralPlanId::Sabotage:
		case NeuralPlanId::AirCorridor:
		case NeuralPlanId::HeavyAirPush:
			return -TankMassPlanBonus;
		default:
			return 0;
		}
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

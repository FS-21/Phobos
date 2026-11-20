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

	// A unit of ours lost to a killer we could not see (DetectionNeedOf): where, when, and of what kind
	// (0 the killer stood in the dark, 1 it was cloaked or disguised, 2 a naval one under water or a ship
	// of ours lost so).
	struct NeuralHiddenLoss
	{
		CellStruct Cell {};
		int Frame { 0 };
		int Kind { 0 };
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
		// Rotating Raids: the frame its outgunned group began to fall back to the other one (-1 none).
		int RegroupFrame { -1 };
		// Rotating Raids: it ends with no target left after others destroyed its targets while it
		// destroyed and lost nothing: no defeat (its weight stays, no outcome is counted).
		bool Neutral { false };
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
		// The last frame a member destroyed anything, buildings too (-1 never: PlanCombat*), and the kind
		// of building its target was when it fell (PlanBuildingKindOf, -1 not fallen: PlanFollowUp*).
		int LastKillFrame { -1 };
		int FallenTargetKind { -1 };
		// Bait Ambush: the bait's health in percent when it set out to fire at the building.
		int LureHealth { 0 };
		// Counter Squad: the enemy house and force category (EnemyForceCategory) it goes after.
		int ForceOwner { -1 };
		int ForceCategory { -1 };
		// Naval Strike Group: a mass strike of short-range gun ships (NavalMassStrikeShips), all of which shell.
		bool MassStrike { false };
	};

	struct NeuralStuckTrack
	{
		CellStruct Cell { CellStruct::Empty };
		int SinceFrame { 0 };
	};

	// Progress clocks (NeuralAI.Progress.inl): how long a unit has kept to one spot, by the rule of each
	// stuck watchdog. Still: every ground unit (IsFootHeldStill). OrderStill: a ground unit with an order
	// pending (UpdateStuckUnits). Hover: a flying ground unit (UpdateHoveringFlyers). Air: an aircraft on
	// Attack, with its ammo (UpdateStuckUnits). HarvestIdle: an empty harvester on Harvest
	// (UpdateIdleHarvesters). Unload: a transport on Unload, by its exact cell (UpdateStuckUnloads).
	enum class NeuralClock : uint8_t
	{
		Still,
		OrderStill,
		Hover,
		Air,
		HarvestIdle,
		Unload,
		Count
	};

	constexpr size_t NeuralClockCount = static_cast<size_t>(NeuralClock::Count);

	// One clock of a unit: the cell it keeps to and since when; Aux and AuxSince: what else the clock
	// follows and since when (an aircraft's ammo); Seen: the serial of the sample that last kept it.
	struct NeuralClockState
	{
		CellStruct Anchor { CellStruct::Empty };
		int Since { 0 };
		int Aux { 0 };
		int AuxSince { 0 };
		int Seen { -1 };
	};

	// A ship of ours that waits outside a crowded narrow water passage (UpdateNavalPassageGate): the passage's
	// centre, where it was bound, whether it was on an attack-move, and since when it waits.
	struct NeuralPassageWait
	{
		CellStruct Passage { CellStruct::Empty };
		CellStruct Goal { CellStruct::Empty };
		bool AttackMove { false };
		int Frame { 0 };
	};

	// The last target pick of an idle warship of ours (NavalPreyPickFrames): when, whether it found a target, and the
	// UniqueID of the enemy unit it went for (0 none).
	struct NeuralNavalPick
	{
		int Frame { 0 };
		bool HadTarget { false };
		DWORD PreyID { 0 };
	};

	// Where a wave sent a unit, the ground zone of that cell and when (KeptWaveGoal).
	struct NeuralWaveGoal
	{
		CellStruct Cell { CellStruct::Empty };
		int Zone { -1 };
		int Frame { 0 };
		// The nearest the unit came to the goal so far (cells, -1 not yet read) and when (IsOnWaveMarch).
		double BestDistance { -1.0 };
		int BestFrame { 0 };
	};

	// A place a wave of the tank mass did not take (siege mode, Siege*): its target, the house its
	// buildings belong to, the waves in a row that took nothing there and the frame of the last.
	struct NeuralResistedPlace
	{
		CellStruct Cell { CellStruct::Empty };
		int Owner { -1 };
		int Failures { 0 };
		int Frame { 0 };
	};

	// Aircraft attack watchdog (UpdateStuckUnits): where an aircraft on Mission::Attack stands and
	// since when, and its ammo and since when it last changed (a shot fired or a reload).
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
	// Client: the one that asked for it, which it goes back to; a wave and the economy share the
	// operation of a crossing (RequestTransportOperation), so it may differ from the operation's.
	// HoldCell: the wait cell it was given while it has no seat, kept while it is still one.
	struct NeuralOpMember
	{
		DWORD UniqueID { 0 };
		NeuralOpClient Client { NeuralOpClient::Wave };
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
		CellStruct HoldCell { CellStruct::Empty };
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
		// Wait cells moved out of the way (FindOpHoldOutOfTheWay): the cell the plain rule gives, then
		// the one used instead (Empty: none found).
		std::vector<std::pair<CellStruct, CellStruct>> HoldMoves;
		// Since when no transport of the house can come to the staging cell (-1: one can; OpNoReachFrames).
		int NoReachSince { -1 };
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

	// Idle MCV watchdog (Micro): where and since when a packed MCV stands, the sites it was sent to
	// without unfolding there, and until when it follows the watchdog's own order to Goal.
	struct NeuralMcvIdle
	{
		int SinceFrame { 0 };
		CellStruct Cell { CellStruct::Empty };
		std::vector<CellStruct> FailedSites {};
		int OwnOrderUntil { -1 };
		CellStruct Goal { CellStruct::Empty };
		// The last leg the watchdog gave toward Goal, and since when the MCV stands next to Goal.
		CellStruct Leg { CellStruct::Empty };
		int NearSince { -1 };
	};

	// TEMP diagnostic (Micro): an MCV the idle watchdog followed that began to unfold by an order of
	// another rule, where and when, and since when the watchdog followed it; a line tells when its yard
	// stands (McvIdleUnfoldLookFrames), as the watchdog's own unfold lines do.
	struct NeuralMcvIdleUnfold
	{
		CellStruct Cell { CellStruct::Empty };
		int Frame { -1 };
		int WatchedSince { -1 };
		const char* TypeId { "" };
	};

	// Hold the ground (Micro, McvFallback*): the cell out of danger an expansion MCV that met an enemy
	// unfolds at, and until when it keeps to it; an empty cell when none was found, until it looks again.
	struct NeuralMcvFallback
	{
		CellStruct Cell { CellStruct::Empty };
		int Until { -1 };
	};

	// A follow-up attack (PlanFollowUp*, UpdateStrategicPlans): the armed ground units of a plan that took
	// its building target, the house and kind of building they go on against, the building they go for
	// now and the members already sent to it, until when it lasts, its value at the start and its last kill.
	struct NeuralFollowUp
	{
		std::vector<DWORD> Members {};
		std::vector<DWORD> Ordered {};
		int Owner { -1 };
		int Kind { -1 };
		int Plan { -1 };
		DWORD TargetID { 0 };
		int StartFrame { 0 };
		int Until { 0 };
		int InitialValue { 0 };
		int LastKillFrame { -1 };
		int Targets { 0 };
		// Where the next target is looked for: the plan's fallen target, then each target taken.
		CellStruct Center { CellStruct::Empty };
	};

	// An MCV whose yard was ordered to unfold (Micro): where and when it last unfolded, how often it
	// tried again on that spot after the engine refused the deploy, and since when it unfolds there
	// (McvUnfoldSkipMostFrames).
	struct NeuralMcvUnfold
	{
		CellStruct Cell { CellStruct::Empty };
		int Frame { -1 };
		int Tries { 0 };
		int Since { -1 };
	};

	// Expansion MCV waiting for a ride to the island goal (Micro): since when it waits with no ride on
	// the way, the free field of its own landmass it took instead once the wait ran out, and whether
	// the land rule gave it its order (the idle MCV watchdog follows it from then on).
	struct NeuralIslandMcvWait
	{
		int SinceFrame { -1 };
		int SeenFrame { -1 };
		CellStruct LandField { CellStruct::Empty };
		int LandFrame { -1 };
		bool Released { false };
	};

	// A deployer of a building other than a yard (Micro): the site it keeps (DeployerSiteMostTries) and
	// when it was picked, the sites it could not deploy at and when (DeployerFailedSiteFrames), the
	// unpack orders given there and the frame of the last one, since when it stands on or next to the
	// site; for the idle deployer watchdog the frame it was first and last seen packed, the ground zone
	// it was last on, the spot it keeps to and since when, and whether the watchdog sent it to the
	// main base (ToMainBase).
	struct NeuralDeployerSite
	{
		CellStruct Site { CellStruct::Empty };
		int PickFrame { -1 };
		std::vector<CellStruct> FailedSites {};
		std::vector<int> FailedFrames {};
		int Tries { 0 };
		int TryFrame { -1 };
		int NearSince { -1 };
		int FirstFrame { -1 };
		int SeenFrame { -1 };
		int Zone { -1 };
		CellStruct StillCell { CellStruct::Empty };
		int StillSince { -1 };
		bool ToMainBase { false };
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
		// The gatherers we had when each temporary recovery refinery was placed: it is sold once we have
		// more (its FreeUnit came out). The frame of the last harvester recovery placed (sell-and-replace
		// or temporary): the second harvester per refinery waits SecondHarvesterRecoveryFrames after it.
		std::map<unsigned int, int> TemporaryRecoveryRefineryGatherers {};
		int LastHarvesterRecoveryFrame { -100000 };
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
		// Follow-up attacks of plans that took their building target (PlanFollowUp*); their members count
		// as plan members.
		std::vector<NeuralFollowUp> FollowUps {};
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
		// Expansion drive: ore fields (12x12 blocks) on our landmass more than 14 cells from any
		// of our hubs (feature scan), and how close the base crawl has brought a building anchor
		// to the nearest free worthy field (placement scan) so a refinery is wanted there.
		int UncolonizedOreFields { 0 };
		// Progress clocks by unit UniqueID (NeuralAI.Progress.inl), the serial of the last sample of each
		// clock (a clock counts only while that sample kept it), the serial counter and the last purge.
		// Keyed lookups only, never iterated to decide.
		std::unordered_map<DWORD, std::array<NeuralClockState, NeuralClockCount>> ProgressClocks {};
		std::array<int, NeuralClockCount> ClockSampleSerial {};
		int ClockSerialCounter { 0 };
		int ClockPurgeFrame { 0 };
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
		// Income trend (the income window of UpdateStrandedSlaveMiners): income per gatherer of the last
		// IncomeTrendWindows windows, how many in a row fell under IncomeFallingPermille of their best,
		// and the falling economy flag two of them set (the expansion is planned, no extra harvester).
		std::vector<int> IncomePerGathererWindows {};
		int IncomeLowWindows { 0 };
		bool IncomeFalling { false };
		// The ore of the fields we hold (GetHeldOreBlocks, UpdateIslandOreOutlook): its value, its drain
		// per 900 frames, the frames it lasts at that pace (-1 unknown) and the key of the held blocks
		// it was counted on.
		int HeldOre { 0 };
		int HeldOreDrain { -1 };
		int HeldOreRunway { -1 };
		unsigned int HeldOreKey { 0 };
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
		// The frame our last refinery was placed (PlaceCompletedBuilding): a newly reached field gets
		// one refinery past what we own at most every RefineryNewFieldFrames.
		int LastRefineryPlacedFrame { -100000 };
		// TEMP diagnostic (expansion refineries, 2026-11-18): the yard hub the last refinery search was
		// anchored at (FindSpatialTetrisBuildCell, a remote yard without a refinery), Empty for none.
		CellStruct RefineryHubCell { CellStruct::Empty };
		// The field the last new field refinery slot was opened for (production) and how close the next
		// refinery placed must stand to it; one placed farther while the slot is still open (seen within
		// RefinerySlotAimFrames) adds it to MissedNewFields (cell, frame), which open no slot for
		// RefineryMissedFieldFrames.
		CellStruct NewFieldSlotCell { CellStruct::Empty };
		int NewFieldSlotReach { 0 };
		// The frame production last saw that slot open: placement aims a refinery at its field only
		// within RefinerySlotAimFrames of it. TEMP diagnostic: whether the last refinery search was
		// aimed at it, and why not (NeuralSlotAimWhy).
		int NewFieldSlotFrame { -100000 };
		bool RefinerySlotAimed { false };
		int RefinerySlotAimWhy { 0 };
		// TEMP diagnostic (crawl DIAG, user approved 2026-11-19): the last building search that crawled
		// toward free ore (type, frame, goal, anchor, ally anchor, kind 0 other, 1 stone, 2 refinery,
		// 3 factory) and the last such building placed (frame, type, Chebyshev cells to the goal from
		// its anchor and from where it stands).
		int CrawlSearchType { -1 };
		int CrawlSearchFrame { -1 };
		CellStruct CrawlGoalCell { CellStruct::Empty };
		CellStruct CrawlAnchorCell { CellStruct::Empty };
		bool CrawlFromAlly { false };
		int CrawlKind { 0 };
		int LastCrawlFrame { -1 };
		int LastCrawlType { -1 };
		int LastCrawlBefore { 0 };
		int LastCrawlAfter { 0 };
		std::vector<std::pair<CellStruct, int>> MissedNewFields {};
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
		bool CounterUnderGroundAttack { false };
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
		// Naval hunt: the last frame the target pick of an idle warship of ours found something, the last pick of each
		// ship (lookup only), the naval sweeps under way (ship -> the place it looks at and the frame it left), the seas
		// with no place for a sweep ((movement zone, zone) -> no search before that frame), the ships that wait outside
		// a jammed narrow water passage (NeuralPassageWait), the ships the gate let go on (ship -> frame) and the last
		// frame the passages were checked.
		int NavalTargetFrame { 0 };
		std::unordered_map<DWORD, NeuralNavalPick> NavalPicks {};
		std::map<DWORD, std::pair<CellStruct, int>> NavalSweeps {};
		std::map<std::pair<int, int>, int> NavalSweepRetryUntil {};
		std::map<DWORD, NeuralPassageWait> NavalPassageWaits {};
		std::map<DWORD, int> NavalPassageReleased {};
		int NavalPassageCheckFrame { -1 };
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
		// The same for one building type ((type index, movement zone) -> frame until): a deployer found
		// no room for it there, or no legal foundation near the island goal before it set out.
		std::map<std::pair<int, int>, int> NoRoomTypeIslandUntil;
		// Deployers (unit -> frame since) standing on a new island with no site for their building.
		std::unordered_map<DWORD, int> DeployerNoSiteSince;
		// Deployers of buildings other than yards (unit -> NeuralDeployerSite).
		std::unordered_map<DWORD, NeuralDeployerSite> DeployerSites;
		// Packed MCVs the idle watchdog follows (unit -> NeuralMcvIdle).
		std::unordered_map<DWORD, NeuralMcvIdle> McvIdle;
		// Watched MCVs unfolding by another rule's order (unit -> NeuralMcvIdleUnfold, TEMP diagnostic).
		std::unordered_map<DWORD, NeuralMcvIdleUnfold> McvIdleUnfolds;
		// Expansion MCVs holding the ground (unit -> NeuralMcvFallback, Micro).
		std::unordered_map<DWORD, NeuralMcvFallback> McvFallbacks;
		// Stolen MCVs (Micro, StolenMcv*): unit -> the frame it was first seen stolen and packed.
		std::unordered_map<DWORD, int> StolenMcvSince;
		// Frame a ConYard of ours was last destroyed (OnTechnoDestroyed; ExpansionMcvLossFrames).
		int ConYardLostFrame { -100000 };
		// Packed MCVs that had an enemy near them at their last look (Micro, the MCV danger rule).
		std::unordered_set<DWORD> McvInDanger;
		// Expansion goals packed MCVs turned back from for an enemy near them (goal, frame until).
		std::vector<std::pair<CellStruct, int>> McvFledSites;
		// Stolen MCVs that were home at their last look (Micro, slot 82: ForeignMcvHomeCells).
		std::unordered_set<DWORD> ForeignMcvHome;
		// Packed MCVs that were unfolding (unit -> NeuralMcvUnfold).
		std::unordered_map<DWORD, NeuralMcvUnfold> McvUnfolds;
		// Expansion MCVs waiting for a ride to the island goal (unit -> NeuralIslandMcvWait).
		std::unordered_map<DWORD, NeuralIslandMcvWait> IslandMcvWaits;
		// Yards an island expansion MCV unfolded while no ride came (cell, frame): they pack up for the
		// island once an idle transport able to make the crossing is ours.
		std::vector<std::pair<CellStruct, int>> IslandWaitYards;
		// Frame one of those yards last packed up for the island.
		int IslandWaitPackFrame { -1 };
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
		// The other economic investments the production wants and cannot pay yet (0 when none): a
		// harvester while we have fewer than refineries, a refinery for a newly reached field.
		int HarvesterSaveTarget { 0 };
		int RefinerySaveTarget { 0 };
		// Economy hold bookkeeping: first frame of the current hold, and no hold before this frame (a
		// hold that bought nothing for 3600 frames rests 1800).
		int EconomyHoldSince { -1 };
		int EconomyHoldPauseUntil { -1 };
		// TEMP diagnostic (economy hold): what the hold saves for (1 harvester, 2 refinery, 3 MCV, 4 broke),
		// when it began, the productions it cancelled since then and since the last status line, what the house
		// owned of that kind when it began, and the status lines written this match.
		int EconomyHoldKind { 0 };
		int EconomyHoldCancels { 0 };
		int EconomyHoldLineCancels { 0 };
		int EconomyHoldStartAssets { 0 };
		int EconomyHoldLogLines { 0 };
		int EconomyHoldStartFrame { -1 };
		// A hold that ended with the credits at the price: its kind, frame and what the house owned of
		// that kind then, looked at again 1800 frames later (was the item bought).
		int EconomyHoldPaidKind { 0 };
		int EconomyHoldPaidFrame { -1 };
		int EconomyHoldPaidAssets { 0 };
		// Enemy units seen cloaked, cloakable or under water: UniqueID -> (cell, frame), for
		// superweapons that reveal stealth (SW.AITargeting=Stealth).
		std::unordered_map<DWORD, std::pair<CellStruct, int>> StealthSightings;
		// Detection need (Detection*): the frame an enemy was last in sight, our losses to killers we
		// could not see (DetectionHiddenLosses at most), and the TEMP diagnostic's last line.
		int LastEnemySeenFrame { 0 };
		std::vector<NeuralHiddenLoss> HiddenLosses {};
		int DetectionLogFrame { -100000 };
		// Plan members by UniqueID (UpdateStrategicPlans); dropped 600 frames after the last sight.
		std::unordered_map<DWORD, NeuralPlanMemberTrace> PlanMemberTraces;
		// Targets by UniqueID where a plan's group failed (finishPlan); key access only.
		std::unordered_map<DWORD, NeuralPlanTargetLesson> PlanTargetLessons;
		// The same per enemy house: the value of the last raid group that was beaten on its way to
		// one of its buildings (raid gate of startPlan).
		std::array<NeuralPlanTargetLesson, 32> PlanHouseLessons {};
		// Rotating Raids lesson per enemy house (RotatingRaidsSize): the doubling steps the group asks for
		// after the raids it beat, and the frame of the last beaten raid (a step fades every
		// RaidLessonStepFrames).
		std::array<int, 32> RaidLessonSteps {};
		std::array<int, 32> RaidLessonFrame {};
		// Units set aside for the next Rotating Raids while it gathers the bigger group a lesson asks
		// for (kept out of the waves and the other plans like plan members), the frame it began
		// (-1 not gathering), and how many it still wants (production favours fast vehicles meanwhile).
		std::vector<DWORD> RaidRecruits {};
		int RaidRecruitFrame { -1 };
		int RaidRecruitWanted { 0 };
		// Capital groups by domain (0 ground, 1 air, 2 sea) and the capital units they hold back from
		// waves and lone strikes (UpdateCapitalGroups).
		std::array<NeuralCapitalGroup, 3> CapitalGroups {};
		std::unordered_set<DWORD> CapitalHeld {};
		// The tank guard (UpdateTankGuard): its tanks, and for each the site it guards and since when.
		std::unordered_set<DWORD> TankGuard {};
		std::unordered_map<DWORD, std::pair<int, int>> TankGuardPosts {};
		int TankGuardLogFrame { -100000 };
		// The parts of the tank floor the production check saw last: the base defense (TankFloorMin and
		// the economy part) and the escorts (one per artillery, anti-air and support vehicle), for the
		// wide guard (TankGuardWideArchetype); and the guard tanks past the base part, which leave the
		// guard only as escorts of a plan group with such vehicles.
		int TankFloorBasePart { 0 };
		int TankFloorEscortPart { 0 };
		std::unordered_set<DWORD> TankGuardEscorts {};
		// TEMP diagnostic (siege screen): the wait orders logged this match.
		int SiegeScreenLogCount { 0 };
		// TEMP diagnostic (push gate): the held pushes logged this match.
		int PushGateLogCount { 0 };
		// TEMP diagnostic (repairers): the repair and escort orders logged this match.
		int RepairerLogCount { 0 };
		// TEMP diagnostic (tank census, 2026-11-18; NoteMainTankDeath): main battle tanks out of a
		// factory, the ones lost by what they did when they died (guard, Base Threat Intercept, another
		// plan, a wave, scouting, a repair trip or transport op, at home, away), the kind of the killer
		// (vehicle, infantry, defense, aircraft, other or none), those lost near our sites and the
		// frames lived by the ones built in this match.
		int TankCensusBuilt { 0 };
		std::array<int, 8> TankCensusLost {};
		std::array<int, 5> TankCensusKillers {};
		int TankCensusLostAtHome { 0 };
		int TankCensusLifeFrames { 0 };
		int TankCensusLifeCount { 0 };
		std::unordered_map<DWORD, int> TankCensusBirth {};
		// ConYard count wanted by the governor for the building queue (0 when none).
		int BankWantedYards { 0 };
		// Until when a unit that lost its boarding turn, or held a transport's radio link without
		// boarding, counts as blocked from ferries (IsFerryBlocked). Keyed lookups only.
		std::unordered_map<DWORD, int> FerryBlockedUntil {};
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
		// Siege mode (Siege*): the owner of the buildings near the target of the wave waiting for its
		// verdict, the places our waves did not take, the enemies besieged now (a bit per house) and
		// the last frame of the siege line.
		int WaveVerdictOwner { -1 };
		std::vector<NeuralResistedPlace> ResistedPlaces {};
		unsigned int SiegeOwnersMask { 0u };
		int SiegeLogFrame { -100000 };
		// First frame a wave of the tank mass waited for its anti-air escort (-1 when none waits),
		// and no new wait before this frame.
		int WaveEscortWaitSince { -1 };
		int WaveEscortRetryFrame { -1 };
		// Last frame scouts were sent to look for the remnants of an enemy (RemnantSearchScouts).
		int RemnantSearchFrame { -100000 };
		// The goal the waves gave each unit (KeptWaveGoal): one still on its way keeps it (IsOnWaveMarch).
		std::unordered_map<DWORD, NeuralWaveGoal> WaveGoals;
		// TEMP diagnostic (wave march): the goals kept past the old WaveGoalKeepFrames logged this match.
		int WaveMarchLogCount { 0 };
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
		// Engineer claims (EngineerClaim*): the building each engineer of ours was sent to capture by the
		// generic capture choice, and when.
		std::unordered_map<DWORD, std::pair<DWORD, int>> EngineerCaptureTargets;
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
		// Crossings shut after an operation ended with no transport (OpNoTransportRetryFrames): the
		// frame they open again, by CrossingBlockKey (client and the two lands).
		std::unordered_map<long long, int> CrossingBlockedUntil;
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
			this->TemporaryRecoveryRefineryGatherers.clear();
			this->LastHarvesterRecoveryFrame = -100000;
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
			this->FollowUps.clear();
			this->RaidTransportBanUntil.clear();
			this->BlackoutUntilFrame = -1;
			this->BloodyBeaches.clear();
			this->AirLossTracks.clear();
			this->AirDangerSpots.clear();
			this->UncolonizedOreFields = 0;
			this->ProgressClocks.clear();
			this->ClockSampleSerial.fill(0);
			this->ClockSerialCounter = 0;
			this->ClockPurgeFrame = 0;
			this->MinerOreSamples.clear();
			this->StrandedMinerSince.clear();
			this->MinerRelocations = 0;
			this->MinerLogFrame = -1;
			this->HarvesterIncome = 0;
			this->MinerIncome = 0;
			this->IncomeLogFrame = -1;
			this->IncomePerGathererWindows.clear();
			this->IncomeLowWindows = 0;
			this->IncomeFalling = false;
			this->HeldOre = 0;
			this->HeldOreDrain = -1;
			this->HeldOreRunway = -1;
			this->HeldOreKey = 0;
			this->OffensiveStoneCells.clear();
			this->OffensiveStones.clear();
			this->LastFactoryCapSaleFrame = -100000;
			this->RefineryReachFailures = 0;
			this->LastRefineryPlacedFrame = -100000;
			this->RefineryHubCell = CellStruct::Empty;
			this->NewFieldSlotCell = CellStruct::Empty;
			this->NewFieldSlotReach = 0;
			this->NewFieldSlotFrame = -100000;
			this->RefinerySlotAimed = false;
			this->RefinerySlotAimWhy = 0;
			this->CrawlSearchType = -1;
			this->CrawlSearchFrame = -1;
			this->CrawlGoalCell = CellStruct::Empty;
			this->CrawlAnchorCell = CellStruct::Empty;
			this->CrawlFromAlly = false;
			this->CrawlKind = 0;
			this->LastCrawlFrame = -1;
			this->LastCrawlType = -1;
			this->LastCrawlBefore = 0;
			this->LastCrawlAfter = 0;
			this->MissedNewFields.clear();
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
			this->CounterUnderGroundAttack = false;
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
			this->NavalTargetFrame = 0;
			this->NavalPicks.clear();
			this->NavalSweeps.clear();
			this->NavalSweepRetryUntil.clear();
			this->NavalPassageWaits.clear();
			this->NavalPassageReleased.clear();
			this->NavalPassageCheckFrame = -1;
			this->NoRoomIslandUntil.clear();
			this->NoRoomTypeIslandUntil.clear();
			this->DeployerNoSiteSince.clear();
			this->DeployerSites.clear();
			this->McvIdle.clear();
			this->McvIdleUnfolds.clear();
			this->McvFallbacks.clear();
			this->StolenMcvSince.clear();
			this->ConYardLostFrame = -100000;
			this->McvInDanger.clear();
			this->McvFledSites.clear();
			this->ForeignMcvHome.clear();
			this->McvUnfolds.clear();
			this->IslandMcvWaits.clear();
			this->IslandWaitYards.clear();
			this->IslandWaitPackFrame = -1;
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
			this->HarvesterSaveTarget = 0;
			this->RefinerySaveTarget = 0;
			this->EconomyHoldSince = -1;
			this->EconomyHoldPauseUntil = -1;
			this->EconomyHoldKind = 0;
			this->EconomyHoldCancels = 0;
			this->EconomyHoldLineCancels = 0;
			this->EconomyHoldStartAssets = 0;
			this->EconomyHoldLogLines = 0;
			this->EconomyHoldStartFrame = -1;
			this->EconomyHoldPaidKind = 0;
			this->EconomyHoldPaidFrame = -1;
			this->EconomyHoldPaidAssets = 0;
			this->StealthSightings.clear();
			this->LastEnemySeenFrame = 0;
			this->HiddenLosses.clear();
			this->DetectionLogFrame = -100000;
			this->PlanMemberTraces.clear();
			this->PlanTargetLessons.clear();
			this->PlanHouseLessons.fill(NeuralPlanTargetLesson {});
			this->RaidLessonSteps.fill(0);
			this->RaidLessonFrame.fill(-100000);
			this->RaidRecruits.clear();
			this->RaidRecruitFrame = -1;
			this->RaidRecruitWanted = 0;
			this->CapitalGroups = {};
			this->CapitalHeld.clear();
			this->TankGuard.clear();
			this->TankGuardPosts.clear();
			this->TankGuardLogFrame = -100000;
			this->TankFloorBasePart = 0;
			this->TankFloorEscortPart = 0;
			this->TankGuardEscorts.clear();
			this->SiegeScreenLogCount = 0;
			this->PushGateLogCount = 0;
			this->RepairerLogCount = 0;
			this->TankCensusBuilt = 0;
			this->TankCensusLost.fill(0);
			this->TankCensusKillers.fill(0);
			this->TankCensusLostAtHome = 0;
			this->TankCensusLifeFrames = 0;
			this->TankCensusLifeCount = 0;
			this->TankCensusBirth.clear();
			this->BankWantedYards = 0;
			this->FerryBlockedUntil.clear();
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
			this->WaveVerdictOwner = -1;
			this->ResistedPlaces.clear();
			this->SiegeOwnersMask = 0u;
			this->SiegeLogFrame = -100000;
			this->WaveEscortWaitSince = -1;
			this->WaveEscortRetryFrame = -1;
			this->RemnantSearchFrame = -100000;
			this->WaveGoals.clear();
			this->WaveMarchLogCount = 0;
			this->WaveGroupTypes.clear();
			this->WaveGroupFrame = -100000;
			this->TransportOps.clear();
			this->EngineerCaptureTargets.clear();
			this->NextTransportOpId = 1;
			this->TransportOpOf.clear();
			this->TransportOpRetryUntil.clear();
			this->OpWaitingSizeBySize.fill(0);
			this->OpWaitingFrame = -1;
			this->TransportOpLogFrame = -1;
			this->OpLossSpots.clear();
			this->OpKindDanger.fill(0);
			this->OpKindDangerFrame = -1;
			this->ShuttleCheckFrame = -1;
			this->ShuttleRetryUntil = -1;
			this->BackdoorCheckFrame = -1;
			this->BackdoorRetryUntil = -1;
			this->CrossingBlockedUntil.clear();
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
		// Departed and stuck (FerryDepartedStuckFrames): the cell and passengers at the last progress
		// of a departed loaded transport and its frame, the frame it was told to unload where it
		// stands, and whether it gave its goal up and takes the cargo back (Homeward) to HomewardZone
		// (LoadZone, or the main base's when LoadZone holds no yard of ours or has no room).
		CellStruct StillCoords { CellStruct::Empty };
		int StillFrame { -1 };
		int StillPassengers { -1 };
		int UnloadHereFrame { -1 };
		bool Homeward { false };
		int HomewardZone { -1 };
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

	// Our gatherers out on the map (harvesters and slave miners, not one a factory still holds): the
	// temporary recovery refinery is sold once there is one more than when it was placed.
	int CountOwnGatherers(const HouseClass* pHouse)
	{
		int count = 0;

		for (auto const pUnit : UnitClass::Array)
		{
			if (pUnit && pUnit->Owner == pHouse && pUnit->IsAlive && !pUnit->InLimbo && pUnit->Type
				&& (pUnit->Type->Harvester || pUnit->Type->ResourceGatherer))
			{
				count++;
			}
		}

		return count;
	}

	// The one boarding loses its turn after this long within FerryBoardTurnCells of the transport
	// without getting aboard, and counts as blocked from ferries for FerryBoardFailedFrames.
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
	// A transport that departed with cargo and has not moved more than one cell, nor let a passenger
	// off, for this long gives its goal up (user approved, 2026-11-19, match #11475: a landing craft
	// with an MCV stood 50000 frames next to a narrow strip of its goal island): it unloads where it
	// stands when it touches ground of another zone than its origin, else (and after one such unload
	// that left it stuck again) it takes the cargo back to its origin landmass. A transport carrying
	// a yard MCV whose island goal is gone, or given up for lack of room, goes back the same way.
	// FerryGiveUpLogLines: lines a match of the TEMP diagnostic of both.
	constexpr int FerryDepartedStuckFrames = FerryStuckCargoFrames;
	constexpr int FerryGiveUpLogLines = 40;
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
	// An operation that no transport takes, for whose staging cell the house has none that seats a
	// member and comes there, owned or buildable now (CanHouseCarryNow), for OpNoReachFrames ends;
	// after an operation ends with no transport, its back door or its crossing (client and the two
	// lands) opens again only OpNoTransportRetryFrames later. User, 2026-11-18: on Fort Urban Square
	// one back door opened 12 times in a row and held its team 9000 frames each time for a hovercraft
	// that could not leave the water.
	constexpr int OpNoReachFrames = 1800;
	constexpr int OpNoTransportRetryFrames = 18000;
	// A free transport goes first to the operation of the house that has waited longest with none,
	// once it has waited OpStarvedFrames (user, 2026-11-19: the oldest operation took each transport
	// that came free or was built, and those on other crossings waited OpNoTransportFrames).
	constexpr int OpStarvedFrames = 1800;

	// Progress clocks (PurgeClocks): how often the clocks no sample can carry on any more go.
	constexpr int ClockPurgeFrames = 900;
	// TEMP diagnostic (LogOpTransportRefusals): how often an operation that no transport takes names
	// why each transport of ours is turned down, the lines a match and the transports a line.
	constexpr int OpRefusalLogFrames = 1800;
	constexpr int OpRefusalLogLines = 150;
	constexpr int OpRefusalLogTransports = 12;
	// TEMP diagnostic: the members that join the operation of another client on their crossing, logged
	// per match.
	constexpr int OpSharedCrossingLogLines = 60;
	// TEMP diagnostic: the transports an operation leaves to one that waits longer, logged per match.
	constexpr int OpStarvedLogLines = 80;
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
	// A member without a seat never waits on a bridge, a passage mouth or in a narrow passage
	// (IsOpWaitInTheWay): its wait cell moves to the nearest cell within OpHoldMoveRing that is out of
	// the way and still in the waiting band. User, 2026-11-18 (Fort Urban Square): the wait cell 17 cells
	// from the staging beach fell on a junction islet's one-cell gate, the band took in the bridge and
	// the islet, and the members of one operation held the way of the house's own waves for 27000 frames.
	constexpr int OpHoldMoveRing = 8;
	constexpr size_t OpHoldMovesMost = 64;
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
	// TEMP diagnostic (economy hold): status lines a house writes a match, one every 900 frames of a hold.
	constexpr int EconomyHoldLogLinesMost = 120;
	// An economy hold whose item is in no factory after EconomyHoldStartFrames rests (user approved,
	// 2026-11-20, P1: the item is the pick of its queue as soon as it is wanted, so one that did not
	// start is blocked elsewhere).
	constexpr int EconomyHoldStartFrames = 2700;
	// Ore cells of a held 12x12 ore block for each refinery past the first it takes (3 at most).
	constexpr int RefineryOreCellsPerExtra = 24;
	// A newly reached ore field gets a refinery past what we own (and past the held fields' room) at
	// most once in this many frames (user, 2026-11-18: capping refineries is a synonym of future defeat).
	constexpr int RefineryNewFieldFrames = 3000;
	// A refinery for a remote yard without one stands within this many cells of it, on its land
	// (production counts a yard's refinery within 18 cells, the footprint corner is not its centre),
	// its search starts at the yard's own field, within RefineryHubOreCells of it.
	constexpr int RefineryHubCells = 16;
	constexpr int RefineryHubOreCells = 24;
	// ... and within this many cells of that field (its centre against the field's best ore cell).
	constexpr int RefineryHubFieldCells = 8;
	// The new field refinery slot (past what we own) needs this many ore cells in the 7x7 of the free
	// ore cell; its refinery counts as landing there within RefineryMissedFieldCells of it
	// (RefineryMissedYardCells of a yard without one), else that field opens no slot for
	// RefineryMissedFieldFrames (a missed field is the same within RefineryMissedSameFieldCells).
	constexpr int RefineryNewFieldOreCells = 5;
	constexpr int RefineryMissedFieldCells = 14;
	constexpr int RefineryMissedYardCells = 18;
	constexpr int RefineryMissedSameFieldCells = 6;
	constexpr int RefineryMissedFieldFrames = 18000;
	// A refinery is aimed at the field of a new field slot production saw open this recently, and only
	// then judged against it (user approved, 2026-11-19): in 20 matches of a31220939, 7 of 33 missed
	// slot fields (12 of 26 on 724f571ef) were judged against a slot already closed, the cell left over
	// from its last opening (Snowballs Chance house 1: slot closed at frame 67500, the refinery of 71568
	// judged against it), and each put a field it was never placed for on MissedNewFields.
	constexpr int RefinerySlotAimFrames = 1800;
	// TEMP diagnostic: why the last refinery search was or was not aimed at the slot field.
	enum class NeuralSlotAimWhy : int
	{
		NoSlot,
		Aimed,
		YardSlot,
		Stale,
		YardHub,
		AllySite,
		ReachFailures,
		NoAnchor,
	};
	constexpr const char* NeuralSlotAimWhyNames[] = { "no slot", "aimed", "yard slot", "stale", "yard hub", "ally site", "reach failures",
		"no anchor within reach" };
	// Most mobile refineries (slave miners) a house mining with them wants, as the Advanced AI, and
	// the least it wants with free fields (a classic base holds 4 refineries at home).
	constexpr int MaxMobileMiners = 15;
	constexpr int MinMobileMiners = 4;
	// Docks an AI house keeps free for its aircraft, by difficulty (user): another airfield while
	// fewer stand free.
	constexpr int MinFreeAirDocksEasy = 4;
	constexpr int MinFreeAirDocksNormal = 6;
	constexpr int MinFreeAirDocksHard = 8;
	// Docks every AI house keeps, whatever its archetype and its learned airfield slot, as the Advanced
	// AI (user approved, 2026-11-20, «Sí, hazlo»: aircraft are among the strongest units of the game
	// and went unused; in 40 archived matches 74 of 119 houses never built an airfield, the learned
	// slot stood at -158). Below them, once the core economy and production stand, the next airfield
	// comes before the routine picks; above them one more comes the same way while an aircraft waits
	// for a dock (AirDockWantedFrames). The number of airfields follows the docks of each type.
	constexpr int AirDocksFloorEasy = 2;
	constexpr int AirDocksFloorNormal = 6;
	constexpr int AirDocksFloorHard = 8;
	// Vehicles and ships of ours per mind-control vehicle (2 at least).
	constexpr int MindControlVehiclesPer = 4;
	// Frames between the harvest income log lines of a house (harvesters against deployed miners).
	constexpr int IncomeLogFrames = 4500;
	// Income trend (user, 2026-11-18: foresee the falling resource flow): an income window whose
	// income per gatherer is under IncomeFallingPermille of the best of the last IncomeTrendWindows
	// is low, two low ones in a row make the economy falling; not before IncomeTrendMinFrame (the
	// first harvesters share a fresh field) nor with fewer than 2 gatherers.
	constexpr int IncomeTrendWindows = 8;
	constexpr int IncomeFallingPermille = 700;
	constexpr int IncomeTrendMinFrame = 9000;
	// The ore of our held fields lasting less than this at the pace it is drained makes the crawl and
	// expansion slots urgent (localOreDepleting), from IncomeTrendMinFrame.
	constexpr int HeldOreRunwayFrames = 18000;
	// Bonus of the refinery for a real new field (or a yard without one) while the income falls.
	constexpr int IncomeFallingRefineryBonus = 3000;
	// The refinery trick for a second harvester per refinery (its FreeUnit, no war factory time): at
	// most once in this many frames; its temporary refinery is sold once a harvester of ours stands
	// within 8 cells of it, or after TemporaryRefineryMaxFrames anyway.
	constexpr int SecondHarvesterRecoveryFrames = 4500;
	constexpr int TemporaryRefineryMaxFrames = 900;
	// Research resources (battle points in CnCReloaded): a type that produces what our research still
	// to buy costs is worth the best unlock in the research choice (the cap of the unlock value), and
	// as a building it scores like a refinery, up to ResearchGeneratorMost of them.
	constexpr int ResearchGeneratorUnlockValue = 600;
	constexpr int ResearchGeneratorMost = 3;
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
	// Deployers of buildings other than yards (user approved, 2026-11-19: "Sí, los cinco"; match #11506:
	// a deployer of a support building of 4x3 cells wandered between islets, home and ore fields and
	// never deployed). One of the base deploys at the site of the base placement search it keeps
	// (NeuralDeployerSite): DeployerSiteMostTries unpack orders on it at most, then the site goes to
	// its avoid list and the next one is taken; with no site it looks again every
	// DeployerSiteRetryFrames. Idle deployer watchdog: one packed for DeployerIdleUndeployedFrames, or
	// within DeployerIdleDriftCells of one spot for DeployerIdleStillFrames, is sent to the nearest
	// legal foundation of the main base and deploys there (DeployerIdleLogLines TEMP lines a match).
	constexpr int DeployerSiteMostTries = 3;
	constexpr int DeployerSiteRetryFrames = 150;
	// A failed site stays on the avoid list this long (a block there may clear); a building on water
	// takes up to DeployerWaterSiteTries searches for a water site its deployer reaches.
	constexpr int DeployerFailedSiteFrames = 5400;
	constexpr int DeployerWaterSiteTries = 4;
	constexpr int DeployerIdleUndeployedFrames = 2700;
	constexpr int DeployerIdleStillFrames = 900;
	constexpr double DeployerIdleDriftCells = 4.0;
	constexpr int DeployerIdleLogLines = 40;
	// Idle MCV watchdog (user, match #11562 Fort Urban Square: two expansion MCVs of houses with a
	// yard stood packed for thousands of frames, 227 path failures of their type, no fallback ever
	// reached): an MCV that stays within McvIdleDriftCells of one spot for McvIdleFrames without
	// unfolding unfolds there if it can, else walks the legs of its own route search to the nearest
	// site (McvIdleOrderFrames to get there); after McvIdleMostTries sites it did not unfold at it
	// goes home and unfolds next to a yard of ours.
	constexpr int McvIdleFrames = 900;
	constexpr int McvIdleDriftCells = 4;
	constexpr int McvIdleOrderFrames = 1800;
	constexpr int McvIdleMostTries = 3;
	constexpr int McvIdleLegSteps = 24;
	// TEMP diagnostic: how long the yard of a watched MCV that unfolds by another rule's order is looked
	// for, and lines a match.
	constexpr int McvIdleUnfoldLookFrames = 1800;
	constexpr int McvIdleUnfoldLogLines = 60;
	// Matches #11445-#11450 (2026-11-19): every site the watchdog gave up was judged from a cell 1 or 2
	// short of it, and MCVs ordered to unfold were sent elsewhere by the MCV logic below. An MCV
	// within McvIdleNearCells of its site is stepped onto the site cell itself, and the site fails
	// only after McvIdleArriveFrames there. A yard whose unfold the engine refused (it clears the
	// foundation for the next try) unfolds again on that spot, McvUnfoldMostTries times within
	// McvUnfoldRetryFrames, before the MCV logic may move it.
	constexpr double McvIdleNearCells = 3.0;
	constexpr int McvIdleArriveFrames = 450;
	constexpr int McvUnfoldRetryFrames = 450;
	constexpr int McvUnfoldMostTries = 3;
	// The MCV rules leave an unfolding MCV to the engine for this long at most, counted from the
	// first frame it unfolded on that spot (user approved, 2026-11-19, match #11476: a yard MCV stood
	// packed 60000+ frames and no rule nor the idle watchdog ever spoke of it); after that they act
	// on it again. McvPackedLogLines: lines a match of the packed yard MCV diagnostic.
	constexpr int McvUnfoldSkipMostFrames = 900;
	constexpr int McvPackedLogLines = 40;
	// The MCV danger rule (user approved, 2026-11-19, match #11862: an expansion MCV drove toward a field 8
	// cells from an enemy Tesla Coil, turned home 13 cells from it, and picked the same field again once
	// home, for 13500+ frames). A known enemy building within McvDangerBuildingCells (a base defense
	// within McvDangerDefenseCells), or a visible armed unit within McvDangerUnitCells, sends a packed MCV
	// home; an MCV in danger leaves it only McvDangerExitCells farther. An expansion goal keeps
	// McvSiteDangerMarginCells beyond the reach of those buildings, so the MCV can get there, and keeps
	// McvFledSiteCells from a goal an MCV turned back from in the last McvFledSiteFrames.
	// McvFledLogLines: lines a match of the fled site diagnostic.
	constexpr int McvDangerBuildingCells = 11;
	constexpr int McvDangerDefenseCells = 13;
	constexpr int McvDangerUnitCells = 12;
	constexpr int McvDangerExitCells = 3;
	constexpr int McvSiteDangerMarginCells = 3;
	constexpr int McvFledSiteCells = 8;
	constexpr int McvFledSiteFrames = 9000;
	constexpr int McvFledLogLines = 30;
	// A stolen MCV is home (slot 82, Micro) within ForeignMcvHomeCells of the yard or of the cell its way
	// home leads to (the placement cell of its yard, which may lie farther: there it was sent to that cell
	// again and again and never unfolded), and stays home until it is ForeignMcvHomeExitCells farther: its
	// deploy stand, a cell past that edge, and the way home took it in turns.
	constexpr int ForeignMcvHomeCells = 16;
	constexpr int ForeignMcvHomeExitCells = 8;
	// Stolen MCVs (user, 2026-11-20: «si no se pudiese desplegar el MCV controlado la única salida que veo es
	// meterlo en el grinder para obtener dinero extra.. y si se puede desplegar lo ideal sería expandir ese
	// arbol tecnológico en zona segura»; match #12285, In The Pipe: three Robot MCVs Yuri took 140 cells from
	// its only yard drove home through enemy ground, and the hunt the engine gives a unit a computer house
	// captures took them back toward the enemy). No MCV hunts. A stolen MCV out of danger within
	// StolenMcvOwnCells of a building of ours unfolds there; one still packed StolenMcvGrinderFrames after it
	// was first seen goes into our Grinder when it reaches it.
	constexpr int StolenMcvOwnCells = 10;
	constexpr int StolenMcvGrinderFrames = 5400;
	// No expansion MCV while losing (user approved, 2026-11-20, «de tus 5 propuestas hazlas todas»; match
	// #12322, The Green Mile: Nod built 11 MCVs, its ConYards went 6 -> 3 -> 1, while the Soviet army
	// outgrew it 107 to 31; the MCV scored 4000 to 9500 in the vehicle pick, every tank 2000 to 4100).
	// With a ConYard standing, an expansion MCV waits while our army is under ExpansionMcvAdvantageMin
	// percent of the army of the strongest enemy seen (NeuralEnemyStrength::Army) or for
	// ExpansionMcvLossFrames after a ConYard of ours was destroyed. A house without a ConYard is not held.
	constexpr int ExpansionMcvAdvantageMin = 67;
	constexpr int ExpansionMcvLossFrames = 5400;
	// Hold the ground (user approved, 2026-11-20, «haz todas las propuestas que me acabas de hacer», match
	// #12278: in a contested middle with no enemy building known, expansion MCVs turned home from every
	// enemy unit passing by, and one stood 900 frames in danger at its home cell next to the front). An
	// expansion MCV that meets an enemy looks for a cell out of every danger (off the reach of the known
	// enemy buildings by McvSiteDangerMarginCells, of the visible enemy units by McvDangerUnitCells +
	// McvFallbackMarginCells) within McvFallbackSearchCells, every other cell, nearest first and those
	// within McvFallbackOwnCells of a building of ours first (a farther one counts McvFallbackOwnPenalty
	// tenths of a cell more), tries McvFallbackTries of them for a cell its yard fits on and unfolds there.
	// It keeps to that cell McvFallbackFrames, or turns home as before for McvFallbackNoneFrames without
	// one. With only enemy units near, and our defenses that reach it and our armed units within
	// McvCoverUnitCells at least as strong, it unfolds where it stands. The goal it turns from is kept from
	// the expansion goals only when the danger is near that goal or the MCV was within McvFledNearGoalCells
	// of it. McvHoldLogLines: lines a match of the TEMP diagnostic.
	constexpr int McvFallbackSearchCells = 20;
	constexpr int McvFallbackMarginCells = 2;
	constexpr int McvFallbackOwnCells = 8;
	constexpr int McvFallbackOwnPenalty = 60;
	constexpr int McvFallbackTries = 4;
	constexpr int McvFallbackFrames = 1800;
	constexpr int McvFallbackNoneFrames = 450;
	constexpr int McvCoverUnitCells = 8;
	constexpr int McvFledNearGoalCells = 16;
	constexpr int McvHoldLogLines = 40;
	// Armies go out in mass (user approved, 2026-11-19: "sí haz las 4"; match #11465 Dead Six: two
	// allied houses of 80+ units and 100000+ credits streamed packets of 7 to 17 units for 18000
	// frames while a third of each army stayed home). Once the army (idle and busy fighters, not
	// guards or plans) has WaveArmyMassFrom units, a main wave waits for WaveArmyShareDivisor-th of it
	// idle, at most WaveArmyShareMost; the second echelon discount holds only while
	// WaveEchelonEngagedUnits of the waves before stand within WaveEchelonEngagedCells of their goal.
	constexpr int WaveArmyMassFrom = 18;
	constexpr int WaveArmyShareDivisor = 3;
	constexpr int WaveArmyShareMost = 40;
	constexpr int WaveEchelonEngagedUnits = 3;
	constexpr double WaveEchelonEngagedCells = 14.0;
	// A team push lasts at least the walk of its farthest member plus TeamPushMarginFrames; the team
	// keeps its target for TeamTargetKeepFrames while it stands; a house with TeamWeakHouseBuildings
	// known buildings or fewer is the team target only when no member picks a stronger one.
	constexpr int TeamPushMarginFrames = 900;
	constexpr int TeamTargetKeepFrames = 9000;
	constexpr int TeamWeakHouseBuildings = 2;
	// Out of an attack on the base the tank guard keeps at most a TankGuardPeaceShare-th of the main
	// battle tanks home (TankGuardMin at least). Off urban maps a garrison is taken only with a seen
	// enemy within UrbanQuietGarrisonCells of the building, and a quiet one empties into the army,
	// the building closed to us for UrbanQuietReleaseFrames.
	constexpr int TankGuardPeaceShare = 4;
	constexpr int UrbanQuietGarrisonCells = 16;
	constexpr int UrbanQuietReleaseFrames = 2700;
	// Land before islands (user approved, 2026-11-19): in 4 matches of 724f571ef 44 of 87 expansion
	// lines of houses whose income fell found the expansion MCV built and packed, waiting for a ride to
	// an island; 31 of 38 such MCVs of 5 stamps had a free field on their own landmass (match #11376,
	// house 1: an SMCV from frame 22500 to 135000 with 29 to 43 free fields). An expansion MCV that has
	// waited IslandMcvWaitFrames with no ride on the way takes the free field of its landmass
	// (IslandMcvLandFieldFrames to unfold there); with none it unfolds where it waits, a yard more for
	// the building queue, and that yard packs up for the island once an idle transport able to make the
	// crossing is ours, standing IslandWaitYardFrames at most as one; after a pack-up no MCV unfolds that
	// way for IslandWaitRepackFrames.
	constexpr int IslandMcvWaitFrames = 2700;
	// A wait not seen for this long starts again.
	constexpr int IslandMcvWaitGapFrames = 600;
	constexpr int IslandMcvLandFieldFrames = 4500;
	constexpr int IslandMcvLandFieldNearCells = 6;
	constexpr int IslandWaitYardFrames = 45000;
	constexpr int IslandWaitRepackFrames = 9000;
	constexpr int IslandWaitFerryCells = 24;
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
	// A narrow passage blocked for JamBreachFrames: a neutral building of no worth (insignificant, not
	// capturable, not occupiable, not a bridge repair hut, a legal target) within JamBreachReachCells of
	// a held unit of the block, whose footprint leaves a lane of one cell there (FootprintLeavesOneCellLane),
	// is shot down by up to JamBreachShootersMost armed held units in range. User,
	// 2026-11-18 (Fort Urban Square): pairs of neutral barricades at the bridge ends left one-cell gates,
	// every jam of the match was at one, a column of 21 to 83 units held up to 13000 frames.
	constexpr int JamBreachFrames = 900;
	constexpr int JamBreachReachCells = 2;
	constexpr int JamBreachShootersMost = 6;
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
	// Rotating Raids: two groups of half the members each. The group starts at RaidBaseGroup units and
	// doubles after each raid an enemy house beat (it lost value and destroyed no more than it lost), up
	// to RaidLessonMaxSteps steps and RaidMostGroup units; a step fades every RaidLessonStepFrames
	// without a new beaten raid, a raid that destroyed more than it lost ends the lesson (user: grow the
	// group after each failure as the tank mass does; 2026-11-17 the groups of 3 died on the way).
	// The step lasts as long as the other plan lessons (PlanLessonFrames): with 3000 frames it faded
	// 300 frames after the plan could try again (2700 after a failure), and of 13 lessons of
	// 2026-11-18 none sent a raid of 16.
	// The targets change RaidSwitchFrames after a group reached its own (or once one falls).
	constexpr int RaidBaseGroup = 8;
	constexpr int RaidMostGroup = 24;
	constexpr int RaidLessonMaxSteps = 2;
	constexpr int RaidLessonStepFrames = PlanLessonFrames;
	constexpr int RaidSwitchFrames = 900;
	constexpr double RaidArrivedCells = 6.0;
	// A group of Rotating Raids outgunned on its way, while the other one is not, falls back to the
	// other one instead of ending the raid (user, 2026-11-18: in 3 of 6 aborts for an outgunned group
	// the raid had lost nothing, one had destroyed 283 at its target): the other group holds, the
	// outgunned one moves to it, and they go on as one group to the other one's target once within
	// RaidRegroupCells of each other or after RaidRegroupFrames. Only an outgunned group that is
	// alone (the other is gone, or they are one already) ends the raid.
	constexpr double RaidRegroupCells = 8.0;
	constexpr int RaidRegroupFrames = 1800;
	// The bigger group of a lesson is recruited, not waited for (user, 2026-11-18): the raid needed
	// all of it idle at once and never got it (the houses had a median of 3.5 to 12.5 idle ground
	// units after a lesson, the waves take them once their own need is met, and of 8 lessons none sent
	// a raid of 16). Each idle ground unit that shows up is set aside (RaidRecruits) and walks to the
	// rally point (our first ConYard) until the lesson's size is reached; after RaidRecruitFrames the
	// raid sets out with what it has, RaidBaseGroup at least. Meanwhile the war factories favour fast
	// armed vehicles, RaidRecruitSpeedBonus per point of Speed, RaidRecruitMostBonus at most.
	constexpr int RaidRecruitFrames = 2700;
	constexpr int RaidRecruitSpeedBonus = 35;
	constexpr int RaidRecruitMostBonus = 450;
	// A raid target is no building within RaidClaimedCells of the target of another active plan of the
	// house or of its wave: those fell to our own attack right after the raid set out, and the raid
	// ended with no target left (3 of 3 quick ends of 2026-11-18).
	constexpr double RaidClaimedCells = 6.0;
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
	// Naval hunt (user approved, 2026-11-19). Match #11499: 24 of the 31 destroyers of a house stood on
	// Attack, still and silent, at shore guns out of their reach; loaded enemy transports crossed the sea unopposed; the
	// house built 77 destroyers and none went to look for the enemy at sea.
	// A ship held still NavalStuckAttackFrames on Attack at a target no cell of its sea brings within the reach of its
	// weapons goes back on guard, out of the target's reach (NavalStuckRetreatMarginCells more) when the target can fire
	// at it; NavalStuckLogLines lines a match.
	constexpr int NavalStuckAttackFrames = 150;
	constexpr int NavalStuckRetreatMarginCells = 2;
	constexpr int NavalStuckLogLines = 30;
	// The target pick of an idle warship: the target's cost, NavalPreyLoadedBonus for a unit with passengers aboard and
	// NavalPreyThreatBonus for one that can fire at the ship, less NavalPreyCellWeight per cell between them. Enemy units
	// on land count within NavalShorePreyCells of the ship. A ship picks once per NavalPreyPickFrames at most and keeps
	// the answer in between; NavalPickLogLines lines of the pick diagnostic a match.
	constexpr int NavalPreyLoadedBonus = 1000;
	constexpr int NavalPreyThreatBonus = 500;
	constexpr int NavalPreyCellWeight = 2;
	constexpr int NavalShorePreyCells = 30;
	constexpr int NavalPreyPickFrames = 90;
	constexpr int NavalPickLogLines = 40;
	// Naval sweep: with no target found by the pick and no shot of a warship of ours for NavalSweepIdleFrames, and at least
	// NavalSweepMinShips idle warships with nothing in reach, up to NavalSweepShips of them at a time look at the places of
	// a NavalSweepStepCells grid over their sea, NavalSweepTripFrames at most for each trip, NavalSweepLogLines lines a match.
	// A sea with no place worth a look is searched again after NavalSweepRetryFrames.
	constexpr int NavalSweepIdleFrames = 1800;
	constexpr size_t NavalSweepMinShips = 4;
	constexpr size_t NavalSweepShips = 2;
	constexpr int NavalSweepStepCells = 14;
	constexpr int NavalSweepTripFrames = 3600;
	constexpr int NavalSweepLogLines = 20;
	constexpr int NavalSweepRetryFrames = 900;
	// Naval mass strike: with fewer than two long-range ships, NavalMassStrikeShips healthy warships of one sea whose guns
	// hit land at under 10 cells start the Naval Strike Group, all of them shelling one target.
	constexpr size_t NavalMassStrikeShips = 8;
	// Fleet staging (slot 69 with no enemy ship seen): the water of the sea nearest to the known enemy building nearest to
	// our base, pulled back toward the base (NavalStagingBackCells at most) until NavalStagingMarginCells out of known
	// defense reach. A ship within NavalStagingKeepCells of it stays where it is.
	constexpr int NavalStagingMarginCells = 3;
	constexpr int NavalStagingBackCells = 20;
	constexpr double NavalStagingKeepCells = 8.0;
	// Narrow water passage: a line across the water of a sea, in one of four directions, NavalPassageWidthCells wide at
	// most. Once NavalNarrowPassageShips ships of ours on their way stand held in one (still NavalPassageHeldFrames), the
	// others bound through it within NavalPassageApproachCells wait where they are, NavalPassageWaitMostFrames at most,
	// and a ship that went on is not held again for NavalPassageCooldownFrames. Checked every NavalPassageCheckFrames,
	// NavalPassageLogLines lines a match.
	constexpr int NavalPassageWidthCells = 5;
	constexpr int NavalNarrowPassageShips = 4;
	constexpr int NavalPassageHeldFrames = 150;
	constexpr double NavalPassageApproachCells = 8.0;
	constexpr int NavalPassageWaitMostFrames = 1800;
	constexpr int NavalPassageCooldownFrames = 1800;
	constexpr int NavalPassageCheckFrames = 60;
	constexpr int NavalPassageLogLines = 20;
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
	// more at most), and at least one in TankFloorShare of its armed ground vehicles: that is the base
	// defense. On top of it one more escorts each artillery, anti-air and support vehicle we own, in
	// every archetype (user, match #11587: a lighter tank class does not mean no tanks, they are good
	// escorts and even as basic defense they are very good; a house with 0 to 3 tanks under a floor of 4
	// built artillery). TankFloorMost at most. Below it the best scored main battle tank it can build goes
	// ahead of the war factory's pick, except when that pick restores the income (a harvester, a miner,
	// an MCV), with no income at all, or while the base is under attack from the air alone (the answer
	// to the aircraft first): a main battle tank is the basic answer to a ground attack (13 of 42 floor
	// checks of that house yielded to the attack, artillery picked with 0 tanks). The tanks it asks for
	// are built even while the vehicles we have go unused (IsArmyKindUnused): they guard and patrol. Measured on 22 matches of stamp 03383eb89: below the floor the houses held 1.5 to 4
	// tanks of 4 to 7; most checks with no tank to build were houses with no war factory yet (860
	// before frame 9000, 268 later), the idle army brake stopped the vehicles 4 times.
	constexpr int TankFloorMin = 4;
	constexpr int TankFloorRefineries = 2;
	constexpr int TankFloorEconomyMost = 4;
	constexpr int TankFloorShare = 3;
	constexpr int TankFloorMost = 16;
	// Light complement (user, 2026-11-18: the light vehicles, the gattling tank, the Wolverine, are strong
	// complements, some good against infantry or aircraft, and the AI almost never builds them). In 25
	// matches of 2026-11-18 the light vehicle was the first choice of 0 to 6% of the war factory picks: its
	// class weight (slot 25) was the lowest of the five (arch 0: 1629 against 1981 tank, 2069 anti-air, 2791
	// artillery, 3328 transport), a gap the counters (up to 900) do not close. With LightComplementMinTanks
	// main battle tanks or more, a house keeps one armed light vehicle (no main battle tank, artillery,
	// anti-air, transport or repairer) for each LightComplementPerTanks of them: below that the best scored
	// light vehicle goes ahead of the pick (the counters choose which: one good against what the enemy
	// fields), with the tank floor's exceptions (no income, a pick that restores it, an attack from the air
	// alone). Below the tank floor as well the two alternate (user, 2026-11-19): the light vehicle goes
	// first when the newest of our main battle tanks and light vehicles is a tank. In 8 matches of 97af3a23a
	// the floor kept the light vehicle out in 136 of 230 checks below it (59%), the light went first in 32.
	// A repairer (negative damage: TSREPAIR) passed for armed and took the light vehicle's place.
	constexpr int LightComplementMinTanks = 3;
	constexpr int LightComplementPerTanks = 3;
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
	// PlanTankEscorts free ones (never of the guard) anywhere on the land of the group, the nearest
	// first, a tank marching in a wave too (user: with 30 cells at most only 9 of 245 plan starts of
	// stamp a8694a191 found one, the free tanks were away with the waves); and the plans that pick
	// escorts take main battle tanks before other units.
	constexpr size_t PlanTankEscorts = 2;
	constexpr int TankGuardShare = 2;
	constexpr int TankGuardMin = 2;
	constexpr int TankGuardMost = 6;
	constexpr int TankGuardCheckFrames = 90;
	constexpr double TankGuardSiegeCells = 24.0;
	constexpr int TankGuardPostFrames = 1800;
	constexpr double TankGuardSiteCells = 40.0;
	constexpr double TankGuardPostCells = 6.0;
	// Wide guard (user, 2026-11-18: in the archetypes that are no tank archetype the tanks are the basic
	// defense and the escorts). In archetypes 1 (SpecOps), 2 (Turtle), 5 (Harasser), 6 (Air and Naval)
	// and 7 (Siege) the guard holds the tank floor's base part and its escort part (TankFloorBasePart,
	// TankFloorEscortPart), up to TankFloorMost, instead of half of the tanks; the tanks past the base
	// part leave the guard only as escorts of a plan group with artillery, anti-air or support vehicles
	// (IsTankEscortedVehicle), up to one for each such vehicle and PlanTankEscorts at least. Only the
	// tanks past the floor go with the waves and the plans. In 40 matches of 2026-11-18 (frames up to
	// 36000) those archetypes lost 25 to 36% of their tanks away with no role and 13 to 28% in waves;
	// archetype 6 lost 61% to defenses and 15% near our sites.
	constexpr bool TankGuardWideArchetype(int archetype)
	{
		return archetype == 1 || archetype == 2 || archetype == 5 || archetype == 6 || archetype == 7;
	}

	// Siege screen (user, 2026-11-18: in archetype 7 the tanks with the artillery stay between the guns
	// and the enemy at the guns' range, as long as they do not hinder the transports of the units they
	// guard). A main battle tank of an archetype 7 house marching in a wave or a plan group that holds
	// a gun (SiegeRangeOf of ArtilleryRangeCells or more, no main battle tank) does not go into the reach
	// of a known enemy defense (+ SiegeScreenMarginCells): it waits on the last cell of its straight way
	// out of every such reach, or, from inside one, on the first cell out of it on the way back to the
	// guns (SiegeScreenBackCells at most), and goes on once no known defense covers its goal or its
	// group has no gun left. The wait cell is within SiegeScreenRingCells of that point, on its own
	// ground, with no building, bridge, tube, passage mouth or narrow passage, and farther than
	// SiegeScreenTransportCells from every transport of ours on the ground; a unit of a transport
	// operation takes no screen order. It still fights the units that come at it. Rolling Siege keeps
	// its own escort posts. In 40 matches of 2026-11-18 archetype 7 lost 32% of its tanks away with no
	// role and 33% in plans (pincer, threeFront), 33% to defenses; its houses owned 3.6 guns on average.
	constexpr int SiegeScreenArchetype = 7;
	constexpr int SiegeScreenMarginCells = 2;
	constexpr int SiegeScreenBackCells = 20;
	constexpr int SiegeScreenRingCells = 4;
	constexpr double SiegeScreenTransportCells = 4.0;
	constexpr double SiegeScreenArrivedCells = 2.0;
	constexpr int SiegeScreenLogLines = 40;

	// Push gate (user, 2026-11-18). The Slot 75 push of Micro sends every idle ground unit, vehicle or
	// soldier, at the best enemy building of its land when it is on an enemy island, within 20 cells of
	// that building, in the endgame or with the wave ready. Now a push at a building inside the reach
	// of known enemy defenses (+ SiegeScreenMarginCells) goes only with company: the units of ours
	// pushing at it this tick, the armed ones within PushGateGroupCells of it, attacking it or heading
	// to within PushGateHeadingCells of it, PushGateMinGroup at least and PushGatePerDefense for each
	// such defense. Without it the unit does not go, and one inside such a reach steps out of it
	// towards home (DefenseReachWaitCell) to wait for the wave. Buildings no known defense covers and
	// the endgame remnant hunt go as before. In 6 matches of 2026-11-18 79 main battle tanks died with
	// this push as their last order; 34 of the 59 sent at a building were killed by defenses, half of
	// them 4 cells or less from an enemy building and 45 cells or more from our sites.
	constexpr int PushGateMinGroup = 6;
	constexpr int PushGatePerDefense = 2;
	constexpr double PushGateGroupCells = 10.0;
	constexpr double PushGateHeadingCells = 6.0;
	constexpr int PushGateLogLines = 30;

	// Repairers (user, 2026-11-19: approved «Sí, prepáralo»; an IFV with an engineer aboard does the
	// same job): a vehicle every weapon of which does negative damage (IsRepairerUnit, read from the unit:
	// TSREPAIR, an FV with an engineer). Before: no production role (built when it won the score, no cap),
	// no escort (it repaired a damaged vehicle within 14 cells only when idle), and it went out as a scout,
	// with the waves and, for a heal across water, on a transport operation. Now like the healer infantry:
	// one built for each RepairerPerVehicles armed ground vehicles of ours (repairers already out
	// included), a damaged own or allied vehicle on its ground within RepairerHealCells repaired first
	// whatever it was doing, else it follows the nearest own armed vehicle on the move or in a fight on its
	// ground (RepairerEscortMost repairers per vehicle at most), keeping within RepairerEscortKeepCells of
	// it; no scout, wave, plan or transport operation. Corrections (user, 2026-11-19: «deberia buscar
	// objetos dañados a reparar en la base y expansiones. además tambien existe el MDISK que es un jumpjet
	// volador que tambien reparar y la version upgradeada del ingeniero de CABAL»): a weapon repairs by its
	// effective damage, Damage x Verses (WeaponRepairAbilities: MDISK's DiskHeal and ROBOTENGINEER2's
	// SuperRepairGun do positive damage through negative Verses), over every weapon slot (MultiWeapon);
	// a flying repairer reaches any ground; the escort follows only vehicles out in the field (farther than
	// RepairerBaseCells from every building of ours), and with none out the repairer repairs the nearest
	// vehicle or building below RepairerDamagedBelow at our bases and expansions (RepairerBaseSearchCells);
	// an infantry capturer that also repairs (CABAL's Mechanic) repairs them too when it has nothing to
	// capture, within RepairerMechanicCells.
	constexpr int RepairerPerVehicles = 6;
	constexpr int RepairerHealCells = 10;
	constexpr int RepairerEscortMost = 2;
	constexpr int RepairerEscortKeepCells = 4;
	// Below one repairer for each RepairerPerVehicles armed ground vehicles (and with that many) the best
	// repairer goes ahead of the vehicle pick (Production.inl, the repairer complement, user 2026-11-19),
	// alternating with the tank floor as the light complement does.
	// The escort follows only a vehicle in a group (user, 2026-11-19): RepairerEscortGroupMin armed ground
	// vehicles of ours within RepairerEscortGroupCells of it, itself included.
	constexpr int RepairerEscortGroupMin = 3;
	constexpr int RepairerEscortGroupCells = 8;
	constexpr int RepairerLogLines = 40;
	constexpr double RepairerBaseCells = 15.0;
	constexpr double RepairerDamagedBelow = 0.85;
	constexpr double RepairerBaseSearchCells = 200.0;
	constexpr double RepairerMechanicCells = 30.0;

	// Detection need (user, 2026-11-18: when the AI stops seeing the enemy, because it has not explored
	// enough or he is cloaked or under water, it builds the units whose detection reaches far or that
	// unfold into something that does; mods differ, so only the INI flags count). DetectionNeedOf: blind
	// while enemies are alive and none was in sight for DetectionBlindFrames from frame
	// DetectionBlindFromFrame, after a remnant search lately, or after a loss to a killer standing in the
	// dark; stealth after an enemy stealth unit in sight or a loss to a cloaked or disguised killer within
	// SensorLookoutSightingFrames; sub with a submarine near home, a naval remnant hunt, or a naval loss
	// to a killer we could not see. A land loss to a hidden killer also counts as a stealth sighting at
	// the victim's cell (the sensor lookouts go to cover it). DetectionReachCells: for a stealth or sub
	// need SensorsSight with Sensors or SensorArray (its Sight when SensorsSight is 0), and for stealth
	// DetectDisguiseRange with DetectDisguise and PsychicDetectionRadius; for a blind need only the Sight
	// from SensorLookoutMinSight (the engine's sensors reveal hidden units, not the shroud); a vehicle
	// counts the building it unfolds into. While a need is on, a detector scores DetectionScorePerCell per
	// cell of reach, DetectionScoreMost at most (it was a flat 480 for a vehicle with Sensors or
	// DetectDisguise while a stealth unit was in sight), and vehicle detectors have a cap of their own,
	// DetectionCap and one more with two construction yards or more, counting only the vehicle detectors
	// of ours that reach as far or farther: a radar jammer of 8 cells no longer keeps a sensor array of
	// 25 out, and a detector building at home (it never goes with the army) keeps no vehicle out (counted
	// with the buildings, in 6 matches a Psychic Sensor of 15 cells kept the jammers out: 0.88% of the
	// NextUnit lines before, 0.09% after). A building with SensorArray that is no base defense gets the same score and
	// one more past the support cap while a stealth or blind need has a sighting or loss no detector of
	// ours covers.
	constexpr int DetectionBlindFrames = 2700;
	constexpr int DetectionBlindFromFrame = 9000;
	constexpr int DetectionScorePerCell = 40;
	constexpr int DetectionScoreMost = 800;
	constexpr int DetectionCap = 1;
	constexpr size_t DetectionHiddenLosses = 32;
	constexpr int DetectionLogFrames = 1800;
	// Engineer claims (user, 2026-11-18: a transport set down more engineers on an island than it had
	// targets, and some stood at the landing with nothing to do). The generic capture choice kept a
	// second engineer off a neutral building only when another one had it as destination or target: one
	// waiting for a transport (its destination the staging beach) or aboard (in limbo) did not count,
	// and enemy buildings had no such check; in the observer match of stamp d6d85b855 single crossings
	// carried 7 to 9 engineers (one of house 2, zone 26 to 10, set down 29 units in 10 trips). Now each
	// engineer sent at a building claims it (EngineerCaptureTargets): the claim holds while the engineer
	// is in a transport operation, walks to or attacks that building, or for EngineerClaimFrames after
	// the order, and no other engineer of ours is sent at a claimed building, neutral or enemy. A transport
	// operation lets an engineer go before it boards when its claimed building is gone, ours or allied,
	// no longer capturable, or claimed by an engineer ahead of it (walking, aboard, or waiting with a
	// lower UniqueID). One set down with its claimed building no longer to take walks EngineerLandingStepCells
	// inland from where it stands, towards its old goal, off the landing.
	constexpr int EngineerClaimFrames = 900;
	constexpr int EngineerLandingStepCells = 6;

	constexpr int DetectionNeedBlind = 1;
	constexpr int DetectionNeedStealth = 2;
	constexpr int DetectionNeedSub = 4;
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
	// A big idle army searches with more units: one in RemnantSearchArmyShare of the idle ones, at
	// most RemnantSearchMostScouts (user, match #11110: 180 idle vehicles searched 3 at a time).
	constexpr size_t RemnantSearchArmyShare = 6;
	constexpr size_t RemnantSearchMostScouts = 32;
	// A place the searchers went to lately is left to the next ones for RemnantSearchSpreadFrames;
	// one they went to RemnantSearchMostSends times within RemnantSearchMemoryFrames is out of their
	// reach (match #11110: the same corner 25 times in 22000 frames, never reached).
	constexpr int RemnantSearchSpreadFrames = 1800;
	constexpr int RemnantSearchMostSends = 3;
	constexpr int RemnantSearchMemoryFrames = 27000;
	// A searcher keeps its place until it comes within RemnantSearchArriveCells of it, takes another
	// goal, or RemnantSearchKeepFrames pass: it is out of the idle pools meanwhile (match #11884:
	// the searchers were sent somewhere else every 450 frames and never reached the far places, then
	// the search found no place left, and a base of the enemy was never seen).
	constexpr int RemnantSearchArriveCells = 5;
	constexpr int RemnantSearchKeepFrames = 2700;
	// The places start where the enemy was: the last known enemy buildings of houses still in the
	// match (RemnantLeadsMost areas, noted every RemnantLeadsEveryFrames while some are known) with
	// RemnantLeadBuildingBonus, then the free ore fields, where a beaten house builds again, with
	// RemnantLeadFieldBonus (half beyond RemnantLeadFieldCells of every lead). With no place left
	// that teaches RemnantSearchMinGain, the stalest ones go instead of none.
	constexpr size_t RemnantLeadsMost = 24;
	constexpr int RemnantLeadsEveryFrames = 450;
	constexpr int RemnantLeadBuildingBonus = 400;
	constexpr int RemnantLeadFieldBonus = 200;
	constexpr int RemnantLeadFieldCells = 40;
	// Sweep (user): while no enemy object is in our sight, one more scout every ScoutSweepEveryFrames
	// (half a minute) may leave, up to ScoutSweepMostTrips scouts out, and the places of a grid of
	// ScoutSweepStepCells over the playable ground are scout targets too (weight ScoutSweepWeight):
	// the ground never looked at gets visited, not only the starts, ore fields and known bases.
	constexpr int ScoutSweepEveryFrames = 450;
	constexpr int ScoutSweepStepCells = 14;
	constexpr int ScoutSweepWeight = 50;
	constexpr size_t ScoutSweepMostTrips = 4;
	// Sensor lookouts (user): a mobile sensor array (a vehicle that unfolds into a building with
	// SensorArray and SensorsSight of SensorLookoutMinSight or more) goes where enemy stealth units
	// were seen in the last SensorLookoutSightingFrames or, with no enemy in sight, to the place its
	// sensors would cover the most ground no sensor of ours covers, and unfolds there. Unfolded, it
	// packs up for a better place after SensorLookoutDwellFrames with no enemy in sight. Production
	// allows one of them apart from the other unarmed support vehicles while stealth units were seen
	// lately or the house searches for remnants.
	constexpr int SensorLookoutMinSight = 10;
	constexpr int SensorLookoutSightingFrames = 5400;
	constexpr int SensorLookoutDwellFrames = 5400;
	constexpr int SensorLookoutMinMoveCells = 20;
	constexpr int SensorLookoutLateFrames = 4500;
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
	// Since 2026-11-19 (user: a mission in progress is not disturbed until it ends; the main enemy
	// changing is no reason, a pact is) the goal is kept until the mission ends (IsOnWaveMarch): the unit
	// arrived within WaveGoalKeepCells, no known building of a house at war with us stands there, no path,
	// or the unit stood still WaveMarchHeldFrames (a blocked way); WaveMarchMostFrames is only a safety
	// net. Plans recruit no unit on such a march (isFreeForPlan). WaveGoalKeepFrames stays for the house's
	// lean to its last target and the TEMP wave march line.
	constexpr int WaveMarchMostFrames = 18000;
	constexpr int WaveMarchHeldFrames = 900;
	// A march also ends when the unit came no WaveMarchProgressCells nearer to its goal in
	// WaveMarchStallFrames (user approved, 2026-11-19: «Sí, hazlo»): in 9 matches of 534a42c97 a TICKT
	// went from 57 to 62 cells of its goal in 5400 frames and an FTHROW was 124 cells away after 15930,
	// moving without getting nearer, which IsFootHeldStill does not catch.
	constexpr int WaveMarchStallFrames = 4500;
	constexpr double WaveMarchProgressCells = 5.0;
	constexpr int WaveMarchLogLines = 40;
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
	// Siege mode of the tank mass (user approved, 2026-11-20, P8: «cuando se resiste un objetivo
	// porque está atrincherado se le asfixia de distintas maneras»). A place resists when
	// SiegeFailuresAtPlace waves in a row took nothing within WaveVerdictCells of it, or the waves
	// failed SiegeFailuresInARow times in a row and it was the last one; it is forgotten
	// SiegeMemoryFrames after its last failure, or when a wave takes a building there. While our
	// advantage is SiegeAdvantageRatio or more (or we have an offensive bank), the owner of such a place
	// is besieged: its buildings lose neither score for lying deep behind its perimeter nor gain for
	// standing on the outer ring, a defense of the outer ring gains nothing, the known defenses within
	// SiegeDefenseCells of a building cost a point per SiegeDefensePerPoint of their value x1.5 (at
	// most SiegeDefenseMost), a building within SiegePlaceCells of a resisting place loses
	// SiegePlacePenalty, the way danger counts up to SiegeWayDangerMost, and what keeps the owner
	// going gains: SiegeEconomyBonus for a refinery, SiegePowerBonus for a power plant,
	// SiegeFactoryBonus for a factory or construction yard. The siege plans of the tank mass lose
	// their penalty meanwhile (ArchetypePlanOffset). Match #12285: house 2, ahead with 111 units and
	// 178000 credits, sent wave after wave at the plateau of house 1 nearest to it.
	constexpr int SiegeFailuresAtPlace = 2;
	constexpr int SiegeFailuresInARow = 3;
	constexpr int SiegeMemoryFrames = 9000;
	constexpr int SiegeAdvantageRatio = 150;
	constexpr double SiegeDefenseCells = 14.0;
	constexpr int SiegeDefensePerPoint = 2;
	constexpr int SiegeDefenseMost = 1500;
	constexpr double SiegePlaceCells = 12.0;
	constexpr int SiegePlacePenalty = 800;
	constexpr int SiegeWayDangerMost = 1500;
	constexpr int SiegeEconomyBonus = 400;
	constexpr int SiegePowerBonus = 300;
	constexpr int SiegeFactoryBonus = 300;
	constexpr int SiegeLogFrames = 2700;
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
	// Missions last while they work (user approved, 2026-11-20: «haz todas las propuestas que me acabas de
	// hacer para la persistencia de misiones», «que se les extienda generosamente la duración de la misión
	// ofensiva en curso mientras funcione y mate/destruya objetivos»). A plan one of whose members destroyed
	// anything in the last PlanCombatWindowFrames gets PlanCombatExtendFrames more when its deadline comes, up
	// to PlanCombatExtendMostTimes the length it started with. A member that fights (its target within its
	// range and PlanFightLeashCells more) gets no move of its group. A plan that took its building target
	// hands its armed ground units (PlanFollowUpMinMembers or more) to a follow-up attack instead of sending
	// them home: the nearest known building of the same kind of that house within PlanFollowUpCells of the
	// fallen target, then of the last target taken (around the group, most follow-ups of 3e042b9f7 found
	// none: the plan won while its group was still far), else of any kind, one after the other until none is
	// left, the group keeps less than
	// PlanFollowUpLowPercent of its value, or PlanFollowUpFrames pass (extended the same way while it
	// destroys, up to PlanFollowUpMostFrames); then it goes home. PlanFollowUpLogLines: lines a match of its
	// TEMP diagnostic.
	constexpr int PlanCombatWindowFrames = 1200;
	constexpr int PlanCombatExtendFrames = 1200;
	constexpr int PlanCombatExtendMostTimes = 4;
	constexpr int PlanFightLeashCells = 3;
	constexpr int PlanFollowUpCells = 28;
	constexpr size_t PlanFollowUpMinMembers = 2;
	constexpr int PlanFollowUpLowPercent = 35;
	constexpr int PlanFollowUpFrames = 2400;
	constexpr int PlanFollowUpMostFrames = 10800;
	constexpr int PlanFollowUpLogLines = 40;
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

	// Order leases (user approved, 2026-11-19, point D: the AI picks the unit, the mission and the goal,
	// starts it, and does not step in again until it ends). The order churn diagnostic found about 2200
	// turns a match, units sent back by another rule before they reached their goal (mind-controlled
	// infantry between the Bio Reactor and the Grinder, about 1000 times a match, plans and the micro
	// rules taking the same unit in turns). An order noted with NoteOrderCaller leases the unit to the
	// code that gave it, at the priority of the scope it runs in (NeuralOrderPriorityScope): survival
	// over plans and operations over routine rules. Until the lease ends the order helpers
	// (IssueMoveOrder, OrderFootToAttackMove, DispatchFootToTargetOrTransport) turn down an order of
	// other code with no higher priority. The holder may change its own order (a routine rule by its
	// line, plans and operations by their file), and an order for (near) the same goal passes. The lease
	// ends when the unit comes within OrderLeaseArriveCells of its goal, has no goal left, heads
	// somewhere else (the engine or an order outside the helpers took it), or after OrderLeaseFrames of
	// its priority.
	enum class NeuralOrderPriority : int8_t
	{
		Routine = 0,
		Operation = 1,
		Survival = 2
	};

	constexpr std::array<int, 3> OrderLeaseFrames { 900, 1800, 450 };
	constexpr double OrderLeaseArriveCells = 3.0;
	constexpr double OrderLeaseSameGoalCells = 6.0;

	struct NeuralOrderLease
	{
		const char* File { nullptr };
		int Line { 0 };
		NeuralOrderPriority Priority { NeuralOrderPriority::Routine };
		int Until { -1 };
		CellStruct Goal { CellStruct::Empty };
	};

	struct NeuralOrderLeases
	{
		std::unordered_map<DWORD, NeuralOrderLease> Units;
		int Serial { -1 };
	};

	std::array<NeuralOrderLeases, 32> OrderLeases {};
	NeuralOrderPriority CurrentOrderPriority = NeuralOrderPriority::Routine;

	// The priority of the orders given in this scope (the previous one again at its end).
	struct NeuralOrderPriorityScope
	{
		explicit NeuralOrderPriorityScope(NeuralOrderPriority priority)
			: Previous { CurrentOrderPriority }
		{
			CurrentOrderPriority = priority;
		}

		~NeuralOrderPriorityScope()
		{
			CurrentOrderPriority = Previous;
		}

		NeuralOrderPriorityScope(const NeuralOrderPriorityScope&) = delete;
		NeuralOrderPriorityScope& operator=(const NeuralOrderPriorityScope&) = delete;

		NeuralOrderPriority Previous;
	};

	std::unordered_map<DWORD, NeuralOrderLease>& OrderLeasesOf(int houseIndex)
	{
		auto& leases = OrderLeases[std::clamp(houseIndex, 0, 31)];

		if (leases.Serial != NeuralMatchSerial)
		{
			leases.Units.clear();
			leases.Serial = NeuralMatchSerial;
		}

		return leases.Units;
	}

	const char* OrderCallerBaseName(const char* file)
	{
		const char* const slash = file ? std::max(std::strrchr(file, '\\'), std::strrchr(file, '/')) : nullptr;

		return slash ? slash + 1 : file;
	}

	// Where a unit heads now: its attack-move goal, else its destination, else its target.
	CellStruct OrderLeaseGoalOf(const FootClass* pFoot)
	{
		AbstractClass* const pGoal = (pFoot->MegaMission == Mission::AttackMove && pFoot->MegaDestination)
			? pFoot->MegaDestination
			: (pFoot->Destination ? pFoot->Destination : pFoot->Target);

		return pGoal ? CellClass::Coord2Cell(pGoal->GetCoords()) : CellStruct::Empty;
	}

	bool IsOrderLeaseOver(const FootClass* pFoot, const NeuralOrderLease& lease)
	{
		const int now = Unsorted::CurrentFrame;

		if (now >= lease.Until || lease.Until - now > OrderLeaseFrames[1])
			return true;

		const CellStruct goal = OrderLeaseGoalOf(pFoot);

		return goal == CellStruct::Empty || goal.DistanceFrom(lease.Goal) > OrderLeaseSameGoalCells
			|| pFoot->GetMapCoords().DistanceFrom(lease.Goal) <= OrderLeaseArriveCells;
	}

	// TEMP diagnostic (order leases): orders turned down, by the pair of codes (holder > refused), logged
	// with the order churn lines (UpdateOrderChurn).
	struct NeuralLeaseRefusals
	{
		std::map<std::string, int> Pairs;
		int Total { 0 };
	};

	std::array<NeuralLeaseRefusals, 32> LeaseRefusals {};

	bool IsPlanMember(HouseClass* pHouse, DWORD uid);

	// Whether the code at file:line may give pFoot an order toward goal now. An order to board (boarding:
	// OrderFootToEnter) passes a lease of its own priority too: it mostly ends the way its own code sent
	// the unit along (a pickup beach, then the transport there), and held to the line it waited out the
	// lease of that move next to the transport.
	bool MayOrderUnit(const FootClass* pFoot, const CellStruct& goal, const char* file, int line, bool boarding = false)
	{
		if (!pFoot || !pFoot->Owner || pFoot->Owner->ArrayIndex < 0 || pFoot->Owner->ArrayIndex >= 32)
			return true;

		// A unit of a strategic plan or of its follow-up attack takes its orders from the plan alone until the
		// plan lets it go (missions last until they end, PlanCombat*); an operation or survival order still
		// takes it.
		if (CurrentOrderPriority < NeuralOrderPriority::Operation && IsPlanMember(pFoot->Owner, pFoot->UniqueID))
		{
			const char* const callerBase = OrderCallerBaseName(file);

			if (!callerBase || std::strcmp(callerBase, "NeuralAI.Plans.inl") != 0)
			{
				CountDecision(NeuralDecision::OrderLeaseRefused, pFoot->Owner->ArrayIndex);

				if (NeuralAI::HeadlessMode)
				{
					auto& refusals = LeaseRefusals[pFoot->Owner->ArrayIndex];
					char key[128];
					std::snprintf(key, sizeof(key), "plan member > %s:%d", callerBase ? callerBase : "?", line);
					refusals.Pairs[key]++;
					refusals.Total++;
				}

				return false;
			}
		}

		auto& leases = OrderLeasesOf(pFoot->Owner->ArrayIndex);
		auto const itLease = leases.find(pFoot->UniqueID);

		if (itLease == leases.end())
			return true;

		auto const& lease = itLease->second;

		if (IsOrderLeaseOver(pFoot, lease))
		{
			leases.erase(itLease);
			return true;
		}

		const char* const base = OrderCallerBaseName(file);
		const bool sameFile = lease.File && base && std::strcmp(lease.File, base) == 0;

		if (CurrentOrderPriority > lease.Priority || (boarding && CurrentOrderPriority == lease.Priority)
			|| (sameFile && (lease.Priority != NeuralOrderPriority::Routine || lease.Line == line))
			|| (goal != CellStruct::Empty && goal.DistanceFrom(lease.Goal) <= OrderLeaseSameGoalCells))
		{
			return true;
		}

		CountDecision(NeuralDecision::OrderLeaseRefused, pFoot->Owner->ArrayIndex);

		if (NeuralAI::HeadlessMode)
		{
			auto& refusals = LeaseRefusals[pFoot->Owner->ArrayIndex];
			char key[128];
			std::snprintf(key, sizeof(key), "%s:%d > %s:%d", lease.File ? lease.File : "?", lease.Line, base ? base : "?", line);
			refusals.Pairs[key]++;
			refusals.Total++;
		}

		return false;
	}

	// An order was given (NoteOrderCaller): the unit is leased to its code, unless another code holds it
	// for (near) the same goal with no lower priority.
	void NoteOrderLease(TechnoClass* pTechno, const char* file, int line)
	{
		if (!pTechno || !pTechno->Owner || pTechno->Owner->ArrayIndex < 0 || pTechno->Owner->ArrayIndex >= 32)
			return;

		const AbstractType kind = pTechno->WhatAmI();

		if (kind != AbstractType::Unit && kind != AbstractType::Infantry && kind != AbstractType::Aircraft)
			return;

		auto const pFoot = static_cast<FootClass*>(pTechno);
		const CellStruct goal = OrderLeaseGoalOf(pFoot);

		if (goal == CellStruct::Empty)
			return;

		auto& leases = OrderLeasesOf(pTechno->Owner->ArrayIndex);
		const int now = Unsorted::CurrentFrame;

		if (leases.size() > 2048)
			std::erase_if(leases, [now](const auto& entry) { return now >= entry.second.Until; });

		const char* const base = OrderCallerBaseName(file);

		if (auto const itLease = leases.find(pTechno->UniqueID); itLease != leases.end())
		{
			auto const& lease = itLease->second;
			const bool sameHolder = lease.File && base && std::strcmp(lease.File, base) == 0 && lease.Line == line;

			if (!sameHolder && now < lease.Until && CurrentOrderPriority <= lease.Priority
				&& goal.DistanceFrom(lease.Goal) <= OrderLeaseSameGoalCells)
			{
				return;
			}
		}

		leases[pTechno->UniqueID] = { base, line, CurrentOrderPriority, now + OrderLeaseFrames[static_cast<int>(CurrentOrderPriority)], goal };
	}

	// A ship of ours that waits outside a jammed narrow water passage (UpdateNavalPassageGate): the order helpers
	// (IssueMoveOrder, OrderFootToAttackMove) leave it where it is until the gate lets it go on. A transport operation
	// that takes it gives its orders whatever the gate says.
	bool IsWaitingAtNavalPassage(const FootClass* pFoot)
	{
		if (!pFoot || !pFoot->Owner || IsInTransportOp(pFoot->Owner, pFoot->UniqueID))
			return false;

		auto const& waits = GetMacroState(pFoot->Owner).NavalPassageWaits;
		auto const it = waits.find(pFoot->UniqueID);
		const int now = Unsorted::CurrentFrame;

		return it != waits.end() && now >= it->second.Frame && now - it->second.Frame < NavalPassageWaitMostFrames;
	}

	// The gate lets a waiting ship go (the jam breaker moves it, or its wait is over): no longer waiting, and not held
	// again for NavalPassageCooldownFrames.
	void DropNavalPassageWait(const FootClass* pFoot)
	{
		if (!pFoot || !pFoot->Owner)
			return;

		auto& macro = GetMacroState(pFoot->Owner);

		if (macro.NavalPassageWaits.erase(pFoot->UniqueID) > 0)
			macro.NavalPassageReleased[pFoot->UniqueID] = Unsorted::CurrentFrame;
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
	// and Bait Ambush went to its Reprisal and Clear Our Ground missions (2026-11-16). 37: the
	// Rush/Wave Timing slot sizes the main wave by WaveMassPermille (2026-11-18).
	constexpr std::pair<int, int> NeuralSlotMeanings[] = { { 37, 1 }, { 48, 1 }, { 93, 1 }, { 108, 1 } };

	// Size of the main wave, per mille of the default, that the learned bias of the Rush/Wave
	// Timing slot (37, with the correction of the map class) asks for: it scales the unit count
	// the wave waits for, its cap, the smallest value worth sending and the cost floor. 1000 with
	// an untrained slot, down to 750 for a rush (+500), up to 1500 for a patient house (-1000).
	int WaveMassPermille(int rushWaveBias)
	{
		return std::clamp(1000 - rushWaveBias / 2, 750, 1500);
	}

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
	// are shifts, not vetoes: a plan the situation asks for loudly still starts. While the house
	// besieges an enemy (siege, SiegeOwnersMask) the siege plans that end well for a house ahead lose
	// their penalty (2026-11-20, 44 matches, ended well / badly: siege battery 49/9, rolling siege
	// 21/5, artillery eco siege 6/2); eco raid 3/7, harvester ambush 0/3 and rotating raids 10/12 keep it.
	int ArchetypePlanOffset(int archetype, NeuralPlanId id, bool siege)
	{
		if (archetype != TankMassArchetype)
			return 0;

		if (siege && (id == NeuralPlanId::SiegeBattery || id == NeuralPlanId::RollingSiege || id == NeuralPlanId::ArtilleryEcoSiege))
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

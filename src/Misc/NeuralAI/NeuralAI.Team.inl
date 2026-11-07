// NeuralAI.Team.inl: Team blackboard shared by allied Neural AI houses (one target and wave time,
// ore field and expansion claims, fronts and calls for help).
// Part of NeuralAI.cpp (included in order; one translation unit).

namespace
{
	int VisibleEnemyThreatNear(HouseClass* pHouse, const CellStruct& cell, int radius);
	int OwnDefenseNear(HouseClass* pHouse, const CellStruct& cell, int radius);
	BuildingClass* FindBuildingByID(DWORD uid);

	// What each Neural AI house tells its team at its own AI tick (index = house ArrayIndex). Human
	// allies never write here: their calls for help are written by a Neural teammate from what it
	// sees of their base, and their claims are only their refineries on the map.
	struct NeuralTeamReport
	{
		int Frame { -100000 };
		// Idle ground army (vehicles, infantry), the wave size this house waits for, its value and
		// centroid.
		int IdleUnits { 0 };
		int IdleInfantry { 0 };
		int MinWave { 0 };
		int IdleValue { 0 };
		CellStruct ArmyCell { CellStruct::Empty };
		// Its own pick of a wave target (the perimeter-first known building of its main enemy).
		DWORD CandidateID { 0 };
		CellStruct CandidateCell { CellStruct::Empty };
		// Ore field claim (UpdateResourceClaim): the field cell, its 12x12 block, since when it is
		// held and when it was last confirmed.
		CellStruct ClaimCell { CellStruct::Empty };
		int ClaimBlock { -1 };
		int ClaimSince { -1 };
		int ClaimFrame { -100000 };
		// Where an expansion MCV of this house is heading.
		CellStruct ExpansionSite { CellStruct::Empty };
		int ExpansionFrame { -100000 };
		// Call for help: the threatened spot, since when and when the threat was last seen there.
		CellStruct HelpCell { CellStruct::Empty };
		int HelpThreat { 0 };
		int HelpSince { -1 };
		int HelpFrame { -100000 };
		int HelpCheckFrame { -100000 };
	};

	std::array<NeuralTeamReport, 32> TeamReports {};

	enum class NeuralTeamPhase : uint8_t
	{
		Idle,
		Muster,
		Push
	};

	// One board per alliance, keyed by the bit mask of its members (house ArrayIndex bits, so the key
	// is the sorted member list). Every Neural AI member advances it at its own tick from the reports
	// only, in house array order, so every client computes the same board.
	struct NeuralTeamBoard
	{
		uint32_t MemberMask { 0 };
		int LastUpdateFrame { -100000 };
		NeuralTeamPhase Phase { NeuralTeamPhase::Idle };
		int Leader { -1 };
		DWORD TargetID { 0 };
		CellStruct TargetCell { CellStruct::Empty };
		int MusterStartFrame { -1 };
		int FirstReadyFrame { -1 };
		int LaunchFrame { -1 };
		int PushUntilFrame { -1 };
		bool Split { false };
		// Per member: when it may leave (the far armies leave first, so all arrive together), its own
		// target in a split push, the launch it already joined.
		std::array<int, 32> DepartFrame {};
		std::array<DWORD, 32> MemberTargetID {};
		std::array<int, 32> JoinedLaunch {};
		// Fronts: the enemy house each Neural member covers, refreshed every 900 frames.
		std::array<int, 32> FrontEnemy {};
		int FrontFrame { -100000 };
		// Calls for help: the requester each member answers (-1 none) and the call it already counted.
		std::array<int, 32> HelpAnswer {};
		std::array<int, 32> HelpCounted {};
		int HumanHelpFrame { -100000 };

		void Reset(uint32_t mask)
		{
			*this = NeuralTeamBoard {};
			this->MemberMask = mask;
			this->FrontEnemy.fill(-1);
			this->HelpAnswer.fill(-1);
			this->HelpCounted.fill(-1);
			this->JoinedLaunch.fill(-1);
		}
	};

	std::array<NeuralTeamBoard, 8> TeamBoards {};
	int TeamBoardSerial { -1 };

	// Ground speed of a typical army in frames per cell, for the departure stagger only.
	constexpr int TeamFramesPerCell = 20;
	constexpr int TeamMaxStagger = 900;
	// A muster waits at most this long once the first member is ready, the push lasts this long.
	constexpr int TeamMusterWait = 900;
	constexpr int TeamMusterMax = 2700;
	constexpr int TeamPushFrames = 1800;
	constexpr int TeamReportFresh = 600;

	void ResetTeamBoards()
	{
		TeamReports.fill(NeuralTeamReport {});

		for (auto& board : TeamBoards)
			board.Reset(0);

		TeamBoardSerial = NeuralMatchSerial;
	}

	// Mutual alliance of two different live player houses (spawn alliances are mutual; a one-sided
	// alliance never makes a team).
	bool IsTeamMember(const HouseClass* pHouse, HouseClass* pOther)
	{
		return pHouse && pOther && pOther != pHouse && pOther->ArrayIndex >= 0 && pOther->ArrayIndex < 32
			&& !pOther->IsNeutral() && !pOther->IsObserver() && !pOther->Defeated
			&& pHouse->IsAlliedWith(pOther) && pOther->IsAlliedWith(pHouse);
	}

	uint32_t TeamMaskOf(HouseClass* pHouse)
	{
		if (!pHouse || pHouse->ArrayIndex < 0 || pHouse->ArrayIndex >= 32)
			return 0;

		uint32_t mask = 1u << pHouse->ArrayIndex;

		for (auto const pOther : HouseClass::Array)
		{
			if (IsTeamMember(pHouse, pOther))
				mask |= 1u << pOther->ArrayIndex;
		}

		return mask;
	}

	HouseClass* TeamHouseAt(int idx)
	{
		if (idx < 0 || idx >= HouseClass::Array.Count)
			return nullptr;

		auto const pHouse = HouseClass::Array.GetItem(idx);

		return (pHouse && pHouse->ArrayIndex == idx) ? pHouse : nullptr;
	}

	bool IsNeuralTeamMember(int idx)
	{
		auto const pHouse = TeamHouseAt(idx);

		return pHouse && NeuralAI::IsEnabledForHouse(pHouse);
	}

	int CountBits(uint32_t mask)
	{
		int count = 0;

		for (; mask; mask &= mask - 1)
			++count;

		return count;
	}

	// The board of this house's alliance, or null without an ally. A new member mask (an ally
	// defeated) takes a free slot, else the one updated longest ago.
	NeuralTeamBoard* GetTeamBoard(HouseClass* pHouse)
	{
		if (TeamBoardSerial != NeuralMatchSerial)
			ResetTeamBoards();

		const uint32_t mask = TeamMaskOf(pHouse);

		if (CountBits(mask) < 2)
			return nullptr;

		NeuralTeamBoard* pFree = nullptr;

		for (auto& board : TeamBoards)
		{
			if (board.MemberMask == mask)
				return &board;

			if (!pFree || (pFree->MemberMask != 0 && (board.MemberMask == 0 || board.LastUpdateFrame < pFree->LastUpdateFrame)))
				pFree = &board;
		}

		pFree->Reset(mask);

		return pFree;
	}

	int NeuralMembersOf(const NeuralTeamBoard& board)
	{
		int count = 0;

		for (int i = 0; i < 32; ++i)
		{
			if ((board.MemberMask & (1u << i)) && IsNeuralTeamMember(i))
				++count;
		}

		return count;
	}

	bool IsTeamReportFresh(int idx, int now)
	{
		return idx >= 0 && idx < 32 && now - TeamReports[idx].Frame <= TeamReportFresh;
	}

	// The wave dispatch's own size test: enough vehicles, or vehicles and infantry four more.
	bool IsWaveSizeReached(int vehicles, int infantry, int minWave)
	{
		return vehicles >= minWave || vehicles + infantry >= minWave + 4;
	}

	bool IsHelpActive(int idx, int now)
	{
		auto const& report = TeamReports[idx];

		return report.HelpCell != CellStruct::Empty && now - report.HelpFrame <= 450;
	}

	// A known, live enemy building for this house (team target checks).
	BuildingClass* ResolveTeamTarget(HouseClass* pHouse, DWORD uid)
	{
		auto const pBld = uid ? FindBuildingByID(uid) : nullptr;

		if (!pBld || !pBld->Owner || pBld->Owner == pHouse || pHouse->IsAlliedWith(pBld->Owner) || pBld->Owner->IsNeutral())
			return nullptr;

		return pBld;
	}

	int Chebyshev(const CellStruct& a, const CellStruct& b)
	{
		return std::max(std::abs(a.X - b.X), std::abs(a.Y - b.Y));
	}

	// ---------------------------------------------------------------- ore fields and expansion sites

	// Refineries and resource destinations of this house's allies (any ally, human or not: allied
	// structures are always visible), cached per house and frame because the claim scans ask for
	// every field.
	struct NeuralTeamHubCache
	{
		int Frame { -1 };
		int House { -1 };
		int Serial { -1 };
		std::vector<std::pair<CellStruct, int>> Hubs {};
	};

	NeuralTeamHubCache TeamHubCache {};

	const std::vector<std::pair<CellStruct, int>>& GetAllyResourceHubs(HouseClass* pHouse)
	{
		auto& cache = TeamHubCache;

		if (cache.Frame == Unsorted::CurrentFrame && cache.House == pHouse->ArrayIndex && cache.Serial == NeuralMatchSerial)
			return cache.Hubs;

		cache.Frame = Unsorted::CurrentFrame;
		cache.House = pHouse->ArrayIndex;
		cache.Serial = NeuralMatchSerial;
		cache.Hubs.clear();

		for (auto const pBld : BuildingClass::Array)
		{
			if (pBld && pBld->IsAlive && !pBld->InLimbo && pBld->Type && (pBld->Type->Refinery || pBld->Type->ResourceDestination)
				&& IsTeamMember(pHouse, pBld->Owner))
			{
				cache.Hubs.emplace_back(pBld->GetMapCoords(), pBld->Owner->ArrayIndex);
			}
		}

		return cache.Hubs;
	}

	// A field has room for two houses when it is big (a third of its 12x12 block is ore) or grows
	// back (spawner tree) and is not small.
	bool IsOreFieldRoomy(int oreCells, bool spawnerTree)
	{
		return oreCells >= 48 || (spawnerTree && oreCells >= 30);
	}

	// Whether teammates already hold this field: an ally refinery mines it, a Neural teammate claimed
	// it before us, or a teammate's expansion MCV is heading there. Shared fields are allowed while
	// there is room (two holders at most on a roomy field). Ties go to the lower house index.
	bool IsOreFieldSpotTakenByTeam(HouseClass* pHouse, const CellStruct& cell, int blockKey, int oreCells, bool spawnerTree)
	{
		if (!pHouse || pHouse->ArrayIndex < 0 || pHouse->ArrayIndex >= 32 || CountBits(TeamMaskOf(pHouse)) < 2)
			return false;

		const int self = pHouse->ArrayIndex;
		const int now = Unsorted::CurrentFrame;
		auto const& own = TeamReports[self];
		const int ownSince = (blockKey >= 0 && own.ClaimBlock == blockKey && now - own.ClaimFrame <= 900) ? own.ClaimSince : INT_MAX;
		uint32_t holders = 0;

		for (auto const& [hubCell, owner] : GetAllyResourceHubs(pHouse))
		{
			if (Chebyshev(hubCell, cell) <= 14)
				holders |= 1u << owner;
		}

		for (int i = 0; i < 32; ++i)
		{
			if (i == self || (holders & (1u << i)) || !IsTeamMember(pHouse, TeamHouseAt(i)))
				continue;

			auto const& report = TeamReports[i];
			const bool claimed = blockKey >= 0 && report.ClaimBlock == blockKey && now - report.ClaimFrame <= 900
				&& (report.ClaimSince < ownSince || (report.ClaimSince == ownSince && i < self));
			const bool heading = report.ExpansionSite != CellStruct::Empty && now - report.ExpansionFrame <= 1800
				&& Chebyshev(report.ExpansionSite, cell) <= 12;

			if (claimed || heading)
				holders |= 1u << i;
		}

		return CountBits(holders) >= (IsOreFieldRoomy(oreCells, spawnerTree) ? 2 : 1);
	}

	bool IsOreFieldTakenByTeam(HouseClass* pHouse, const NeuralOreField& field)
	{
		return IsOreFieldSpotTakenByTeam(pHouse, field.Cell, field.BlockKey, field.OreCells, field.SpawnerTree);
	}

	// Same check for any ore cell (MCV expansion scan): the map field of its block when there is one.
	bool IsExpansionCellTakenByTeam(HouseClass* pHouse, const CellStruct& cell)
	{
		if (!pHouse || CountBits(TeamMaskOf(pHouse)) < 2)
			return false;

		const int key = OreBlockKey(cell);

		for (auto const& field : GetOreFields())
		{
			if (field.BlockKey == key)
				return IsOreFieldSpotTakenByTeam(pHouse, cell, key, field.OreCells, field.SpawnerTree);
		}

		return IsOreFieldSpotTakenByTeam(pHouse, cell, key, 0, false);
	}

	// Published by UpdateResourceClaim after every refresh.
	void NoteTeamResourceClaim(HouseClass* pHouse, const CellStruct& cell)
	{
		if (!pHouse || pHouse->ArrayIndex < 0 || pHouse->ArrayIndex >= 32)
			return;

		auto& report = TeamReports[pHouse->ArrayIndex];
		const int key = cell != CellStruct::Empty ? OreBlockKey(cell) : -1;

		if (key != report.ClaimBlock || report.ClaimSince < 0)
			report.ClaimSince = Unsorted::CurrentFrame;

		report.ClaimCell = cell;
		report.ClaimBlock = key;
		report.ClaimFrame = Unsorted::CurrentFrame;
	}

	void NoteTeamExpansionSite(HouseClass* pHouse, const CellStruct& cell)
	{
		if (!pHouse || pHouse->ArrayIndex < 0 || pHouse->ArrayIndex >= 32)
			return;

		auto& report = TeamReports[pHouse->ArrayIndex];
		report.ExpansionSite = cell;
		report.ExpansionFrame = Unsorted::CurrentFrame;
	}

	// ---------------------------------------------------------------- calls for help

	// The most threatened spot of a house's base (home and ConYards) as `viewer` sees it: visible
	// enemy combat value within 12 cells against the house's own armed defenders there. A call is
	// made at 200+ value when the defenders have less than 1.4x that.
	void RefreshHelpRequest(HouseClass* pViewer, HouseClass* pHouse)
	{
		if (!pViewer || !pHouse || pHouse->ArrayIndex < 0 || pHouse->ArrayIndex >= 32)
			return;

		auto& report = TeamReports[pHouse->ArrayIndex];
		const int now = Unsorted::CurrentFrame;

		if (now - report.HelpCheckFrame < 90)
			return;

		report.HelpCheckFrame = now;

		CellStruct spots[5];
		int spotCount = 0;
		const CellStruct home = GetHouseHomeCell(pHouse);

		if (home != CellStruct::Empty)
			spots[spotCount++] = home;

		for (int i = 0; i < pHouse->ConYards.Count && spotCount < 5; ++i)
		{
			if (auto const pYard = pHouse->ConYards.GetItem(i))
			{
				const CellStruct yc = pYard->GetMapCoords();

				if (spotCount == 0 || Chebyshev(yc, spots[0]) > 6)
					spots[spotCount++] = yc;
			}
		}

		int bestNeed = 0;
		CellStruct bestCell = CellStruct::Empty;
		int bestThreat = 0;

		for (int i = 0; i < spotCount; ++i)
		{
			const int threat = VisibleEnemyThreatNear(pViewer, spots[i], 12);

			if (threat < 200)
				continue;

			const int defense = OwnDefenseNear(pHouse, spots[i], 12);
			const int need = threat * 14 - defense * 10;

			if (need > bestNeed)
			{
				bestNeed = need;
				bestCell = spots[i];
				bestThreat = threat;
			}
		}

		if (bestCell == CellStruct::Empty)
			return;

		if (!IsHelpActive(pHouse->ArrayIndex, now))
		{
			report.HelpSince = now;
			CountDecision(NeuralDecision::TeamHelpRequest, pHouse->ArrayIndex);
			Debug::Log("[NeuralAI] Frame %d | Team | House #%d asks for help at (%d,%d), threat %d\n", now, pHouse->ArrayIndex,
				bestCell.X, bestCell.Y, bestThreat);
		}

		report.HelpCell = bestCell;
		report.HelpThreat = bestThreat;
		report.HelpFrame = now;
	}

	// Each call gets one answer: the Neural member with the biggest idle army on the same landmass that
	// is not calling itself (the previous answer is kept while it still qualifies). A member answers
	// one call at a time.
	void AssignTeamHelp(NeuralTeamBoard& board, int now)
	{
		std::array<int, 32> previous = board.HelpAnswer;
		board.HelpAnswer.fill(-1);

		for (int requester = 0; requester < 32; ++requester)
		{
			if (!(board.MemberMask & (1u << requester)) || !IsHelpActive(requester, now))
				continue;

			const CellStruct helpCell = TeamReports[requester].HelpCell;
			const int helpZone = MapClass::Instance.GetMovementZoneType(helpCell, MovementZone::Normal, false);
			int best = -1;
			int bestScore = INT_MIN;

			for (int m = 0; m < 32; ++m)
			{
				if (m == requester || !(board.MemberMask & (1u << m)) || board.HelpAnswer[m] >= 0 || !IsNeuralTeamMember(m)
					|| !IsTeamReportFresh(m, now) || IsHelpActive(m, now))
				{
					continue;
				}

				auto const& report = TeamReports[m];

				if (report.IdleUnits + report.IdleInfantry / 2 < 3 || report.ArmyCell == CellStruct::Empty
					|| MapClass::Instance.GetMovementZoneType(report.ArmyCell, MovementZone::Normal, false) != helpZone)
				{
					continue;
				}

				const int score = report.IdleValue - Chebyshev(report.ArmyCell, helpCell) * 4 + (previous[m] == requester ? 400 : 0);

				if (score > bestScore)
				{
					bestScore = score;
					best = m;
				}
			}

			if (best >= 0)
				board.HelpAnswer[best] = requester;
		}
	}

	// ---------------------------------------------------------------- fronts

	// Every 900 frames each Neural member gets the enemy house whose known structures are nearest its
	// home, nearest pairs first, each enemy taken once while there are enemies left (then shared).
	void AssignTeamFronts(HouseClass* pViewer, NeuralTeamBoard& board, int now)
	{
		if (now - board.FrontFrame < 900)
			return;

		board.FrontFrame = now;
		board.FrontEnemy.fill(-1);

		std::array<int, 32> sumX {};
		std::array<int, 32> sumY {};
		std::array<int, 32> count {};

		for (auto const pBld : BuildingClass::Array)
		{
			if (!pBld || !pBld->IsAlive || pBld->InLimbo || !pBld->Type || !pBld->Owner || pBld->Type->Insignificant
				|| pBld->Type->InvisibleInGame || pBld->Owner->ArrayIndex < 0 || pBld->Owner->ArrayIndex >= 32
				|| pBld->Owner == pViewer || pViewer->IsAlliedWith(pBld->Owner) || pBld->Owner->IsNeutral()
				|| pBld->Owner->IsObserver() || pBld->Owner->Defeated || !IsEnemyBuildingKnown(pViewer, pBld))
			{
				continue;
			}

			const int owner = pBld->Owner->ArrayIndex;
			const CellStruct bc = pBld->GetMapCoords();
			sumX[owner] += bc.X;
			sumY[owner] += bc.Y;
			count[owner]++;
		}

		struct FrontPair
		{
			int Dist;
			int Member;
			int Enemy;
		};

		std::vector<FrontPair> pairs;
		int enemies = 0;

		for (int e = 0; e < 32; ++e)
		{
			if (count[e] == 0)
				continue;

			++enemies;
			const CellStruct centroid { static_cast<short>(sumX[e] / count[e]), static_cast<short>(sumY[e] / count[e]) };

			for (int m = 0; m < 32; ++m)
			{
				if (!(board.MemberMask & (1u << m)) || !IsNeuralTeamMember(m))
					continue;

				const CellStruct home = GetHouseHomeCell(TeamHouseAt(m));

				if (home != CellStruct::Empty)
					pairs.push_back({ Chebyshev(home, centroid), m, e });
			}
		}

		std::sort(pairs.begin(), pairs.end(), [](const FrontPair& a, const FrontPair& b)
		{
			if (a.Dist != b.Dist)
				return a.Dist < b.Dist;

			return a.Member != b.Member ? a.Member < b.Member : a.Enemy < b.Enemy;
		});

		uint32_t usedEnemies = 0;

		for (int pass = 0; pass < 2; ++pass)
		{
			for (auto const& pair : pairs)
			{
				if (board.FrontEnemy[pair.Member] >= 0)
					continue;

				if (pass == 0 && (usedEnemies & (1u << pair.Enemy)) && CountBits(usedEnemies) < enemies)
					continue;

				board.FrontEnemy[pair.Member] = pair.Enemy;
				usedEnemies |= 1u << pair.Enemy;
			}
		}
	}

	// Main enemy choice bonus for the enemy this house covers (only with a Neural teammate).
	int TeamFrontBonus(HouseClass* pHouse, int enemyIdx)
	{
		auto const pBoard = GetTeamBoard(pHouse);

		if (!pBoard || NeuralMembersOf(*pBoard) < 2 || enemyIdx < 0 || enemyIdx >= 32)
			return 0;

		return pBoard->FrontEnemy[pHouse->ArrayIndex] == enemyIdx ? 300 : 0;
	}

	// ---------------------------------------------------------------- the joint wave

	// Leader = the Neural member with the biggest idle army that has a target (ties: lower index); its
	// own pick is the team target.
	bool PickTeamTarget(HouseClass* pViewer, NeuralTeamBoard& board, int now)
	{
		int leader = -1;
		int bestValue = -1;

		for (int m = 0; m < 32; ++m)
		{
			if (!(board.MemberMask & (1u << m)) || !IsNeuralTeamMember(m) || !IsTeamReportFresh(m, now))
				continue;

			auto const& report = TeamReports[m];

			if (!ResolveTeamTarget(pViewer, report.CandidateID))
				continue;

			if (report.IdleValue > bestValue)
			{
				bestValue = report.IdleValue;
				leader = m;
			}
		}

		if (leader < 0)
			return false;

		auto const pTarget = ResolveTeamTarget(pViewer, TeamReports[leader].CandidateID);
		const bool changed = board.TargetID != pTarget->UniqueID;
		board.Leader = leader;
		board.TargetID = pTarget->UniqueID;
		board.TargetCell = pTarget->GetMapCoords();

		if (changed)
		{
			CountDecision(NeuralDecision::TeamTargetSet, leader);
			Debug::Log("[NeuralAI] Frame %d | Team %08X | House #%d leads, team target %s at (%d,%d) of house #%d\n", now,
				board.MemberMask, leader, pTarget->Type ? pTarget->Type->ID : "?", board.TargetCell.X, board.TargetCell.Y,
				pTarget->Owner->ArrayIndex);
		}

		return true;
	}

	void LaunchTeamWave(HouseClass* pViewer, NeuralTeamBoard& board, int now, bool allReady)
	{
		board.Phase = NeuralTeamPhase::Push;
		board.LaunchFrame = now;
		board.DepartFrame.fill(now);
		board.MemberTargetID.fill(0);

		// Split fronts: every member is ready with room to spare and they cover different enemies,
		// each with its own target there; else one joint push on the leader's target.
		int members = 0;
		uint32_t fronts = 0;
		bool strong = allReady;

		for (int m = 0; m < 32; ++m)
		{
			if (!(board.MemberMask & (1u << m)) || !IsNeuralTeamMember(m) || !IsTeamReportFresh(m, now) || IsHelpActive(m, now))
				continue;

			auto const& report = TeamReports[m];
			auto const pOwn = ResolveTeamTarget(pViewer, report.CandidateID);
			++members;

			if (!pOwn || !IsWaveSizeReached(report.IdleUnits, report.IdleInfantry, report.MinWave + 3) || board.FrontEnemy[m] < 0 || !pOwn->Owner
				|| pOwn->Owner->ArrayIndex != board.FrontEnemy[m])
			{
				strong = false;
			}
			else
			{
				fronts |= 1u << board.FrontEnemy[m];
			}
		}

		board.Split = strong && members >= 2 && CountBits(fronts) >= 2;

		int maxTravel = 0;
		std::array<int, 32> travel {};

		for (int m = 0; m < 32; ++m)
		{
			if (!(board.MemberMask & (1u << m)) || !IsNeuralTeamMember(m) || !IsTeamReportFresh(m, now))
				continue;

			auto const& report = TeamReports[m];
			board.MemberTargetID[m] = board.Split ? report.CandidateID : board.TargetID;
			auto const pTarget = ResolveTeamTarget(pViewer, board.MemberTargetID[m]);

			if (pTarget && report.ArmyCell != CellStruct::Empty)
				travel[m] = Chebyshev(report.ArmyCell, pTarget->GetMapCoords()) * TeamFramesPerCell;

			maxTravel = std::max(maxTravel, travel[m]);
		}

		for (int m = 0; m < 32; ++m)
		{
			if (board.MemberTargetID[m] != 0)
				board.DepartFrame[m] = now + std::min(TeamMaxStagger, maxTravel - travel[m]);
		}

		board.PushUntilFrame = now + TeamMaxStagger + TeamPushFrames;
		CountDecision(board.Split ? NeuralDecision::TeamSplitFronts : NeuralDecision::TeamWaveLaunch, board.Leader);
		Debug::Log("[NeuralAI] Frame %d | Team %08X | %s wave launched by house #%d (mustered %d frames%s) -> (%d,%d)\n", now,
			board.MemberMask, board.Split ? "Split-front" : "Joint", board.Leader, now - board.MusterStartFrame,
			allReady ? ", all ready" : (board.FirstReadyFrame >= 0 && now - board.FirstReadyFrame >= TeamMusterWait ? ", wait over" : ", team mass"),
			board.TargetCell.X, board.TargetCell.Y);
	}

	void AdvanceTeamBoard(HouseClass* pViewer, NeuralTeamBoard& board, int now)
	{
		board.LastUpdateFrame = now;

		// Human allies cannot call: a Neural teammate calls for them from what it sees of their base.
		if (now - board.HumanHelpFrame >= 90)
		{
			board.HumanHelpFrame = now;

			for (int i = 0; i < 32; ++i)
			{
				if ((board.MemberMask & (1u << i)) && !IsNeuralTeamMember(i))
				{
					if (auto const pHuman = TeamHouseAt(i))
						RefreshHelpRequest(pViewer, pHuman);
				}
			}
		}

		AssignTeamHelp(board, now);

		if (NeuralMembersOf(board) < 2)
		{
			board.Phase = NeuralTeamPhase::Idle;
			return;
		}

		AssignTeamFronts(pViewer, board, now);

		if (board.Phase == NeuralTeamPhase::Push)
		{
			if (now < board.PushUntilFrame)
			{
				// A destroyed joint target: the leader's (or a member's) next pick, the push goes on.
				if (!ResolveTeamTarget(pViewer, board.TargetID))
				{
					if (PickTeamTarget(pViewer, board, now))
					{
						for (int m = 0; m < 32; ++m)
						{
							if (!board.Split && board.MemberTargetID[m] != 0)
								board.MemberTargetID[m] = board.TargetID;
						}
					}
				}

				return;
			}

			board.Phase = NeuralTeamPhase::Idle;
		}

		if (board.Phase == NeuralTeamPhase::Idle || !ResolveTeamTarget(pViewer, board.TargetID))
		{
			if (!PickTeamTarget(pViewer, board, now))
			{
				board.Phase = NeuralTeamPhase::Idle;
				return;
			}

			if (board.Phase == NeuralTeamPhase::Idle)
			{
				board.Phase = NeuralTeamPhase::Muster;
				board.MusterStartFrame = now;
				board.FirstReadyFrame = -1;
			}
		}

		// Muster: members not answering or calling for help are expected; launch when all of them
		// are ready, once the first ready one has waited TeamMusterWait frames, or when their idle
		// armies together reach the biggest wave size among them (each then sends what it has). A
		// muster nobody completes picks its target again every TeamMusterMax frames.
		int expected = 0;
		int ready = 0;
		int teamVehicles = 0;
		int teamInfantry = 0;
		int teamMinWave = 1;

		for (int m = 0; m < 32; ++m)
		{
			if (!(board.MemberMask & (1u << m)) || !IsNeuralTeamMember(m) || !IsTeamReportFresh(m, now) || IsHelpActive(m, now)
				|| board.HelpAnswer[m] >= 0)
			{
				continue;
			}

			++expected;

			auto const& report = TeamReports[m];
			teamVehicles += report.IdleUnits;
			teamInfantry += report.IdleInfantry;
			teamMinWave = std::max(teamMinWave, report.MinWave);

			if (IsWaveSizeReached(report.IdleUnits, report.IdleInfantry, std::max(1, report.MinWave)))
				++ready;
		}

		if (ready > 0 && board.FirstReadyFrame < 0)
			board.FirstReadyFrame = now;

		const bool massReached = expected >= 2 && IsWaveSizeReached(teamVehicles, teamInfantry, teamMinWave);

		if ((ready > 0 && (ready >= expected || now - board.FirstReadyFrame >= TeamMusterWait)) || massReached)
		{
			LaunchTeamWave(pViewer, board, now, ready > 0 && ready >= expected);
			return;
		}

		if (ready == 0 && now - board.MusterStartFrame >= TeamMusterMax)
		{
			board.MusterStartFrame = now;
			PickTeamTarget(pViewer, board, now);
		}
	}

	// What the wave dispatch of one house does this tick.
	struct NeuralTeamWaveOrder
	{
		// Target override (Empty: keep the house's own pick) and its building when it is one.
		CellStruct TargetCell { CellStruct::Empty };
		BuildingClass* pTarget { nullptr };
		// Do not launch now (muster, or waiting for its departure slot).
		bool Hold { false };
		// Smaller wave allowed now (joining a launched push, answering a call), 0 = unchanged.
		int MinWaveUnits { 0 };
		bool AnswersHelp { false };
		bool Joining { false };
	};

	// Called by the wave dispatch with this tick's idle army and own target pick: publishes the report,
	// advances the board and returns the team's order. Without a Neural teammate or human ally the
	// order is empty and the house plays exactly as alone.
	NeuralTeamWaveOrder UpdateTeamWave(HouseClass* pHouse, const std::vector<UnitClass*>& idleUnits,
		const std::vector<InfantryClass*>& idleInf, int minWave, BuildingClass* pOwnTarget, const CellStruct& homeCell, bool endgame)
	{
		NeuralTeamWaveOrder order {};
		auto const pBoard = GetTeamBoard(pHouse);

		if (!pBoard)
			return order;

		auto& board = *pBoard;
		const int self = pHouse->ArrayIndex;
		const int now = Unsorted::CurrentFrame;
		auto& report = TeamReports[self];
		int value = 0;
		int sumX = 0;
		int sumY = 0;
		int ground = 0;

		for (auto const pUnit : idleUnits)
		{
			value += GetCombatValueOfType(pUnit->Type, false);
			const CellStruct uc = pUnit->GetMapCoords();
			sumX += uc.X;
			sumY += uc.Y;
			++ground;
		}

		for (auto const pInf : idleInf)
			value += GetCombatValueOfType(pInf->Type, false) / 2;

		report.Frame = now;
		report.IdleUnits = static_cast<int>(idleUnits.size());
		report.IdleInfantry = static_cast<int>(idleInf.size());
		report.MinWave = minWave;
		report.IdleValue = value;
		report.ArmyCell = ground > 0 ? CellStruct { static_cast<short>(sumX / ground), static_cast<short>(sumY / ground) } : homeCell;
		report.CandidateID = pOwnTarget ? pOwnTarget->UniqueID : 0;
		report.CandidateCell = pOwnTarget ? pOwnTarget->GetMapCoords() : CellStruct::Empty;
		RefreshHelpRequest(pHouse, pHouse);
		AdvanceTeamBoard(pHouse, board, now);

		// Answering a call: the idle army goes there now, whatever its size.
		const int requester = board.HelpAnswer[self];

		if (requester >= 0 && IsHelpActive(requester, now))
		{
			order.TargetCell = TeamReports[requester].HelpCell;
			order.MinWaveUnits = 2;
			order.AnswersHelp = true;
			return order;
		}

		if (endgame || NeuralMembersOf(board) < 2 || board.Phase == NeuralTeamPhase::Idle || IsHelpActive(self, now))
			return order;

		const bool wouldLaunch = IsWaveSizeReached(report.IdleUnits, report.IdleInfantry, minWave);

		if (board.Phase == NeuralTeamPhase::Muster)
		{
			if (auto const pTarget = ResolveTeamTarget(pHouse, board.TargetID))
			{
				order.TargetCell = pTarget->GetMapCoords();
				order.pTarget = pTarget;
			}

			order.Hold = true;

			if (wouldLaunch)
				CountDecision(NeuralDecision::TeamWaveHold, self);

			return order;
		}

		// Push: our own target in a split push, else the joint one; our own pick if it is gone.
		auto const pTarget = ResolveTeamTarget(pHouse, board.MemberTargetID[self] ? board.MemberTargetID[self] : board.TargetID);

		if (pTarget)
		{
			order.TargetCell = pTarget->GetMapCoords();
			order.pTarget = pTarget;
		}

		if (now < board.DepartFrame[self])
		{
			order.Hold = true;

			if (wouldLaunch)
				CountDecision(NeuralDecision::TeamWaveHold, self);
		}
		else if (now <= board.DepartFrame[self] + 600 && board.JoinedLaunch[self] != board.LaunchFrame)
		{
			order.MinWaveUnits = std::max(2, minWave / 2);
			order.Joining = true;
		}

		return order;
	}

	// The wave dispatch sent units under a team order: count the joined push or the answered call once.
	void NoteTeamWaveDispatched(HouseClass* pHouse, const NeuralTeamWaveOrder& order, size_t vehicles, size_t infantry)
	{
		auto const pBoard = GetTeamBoard(pHouse);

		if (!pBoard)
			return;

		auto& board = *pBoard;
		const int self = pHouse->ArrayIndex;
		const int now = Unsorted::CurrentFrame;

		if (order.AnswersHelp)
		{
			const int requester = board.HelpAnswer[self];

			if (requester >= 0 && board.HelpCounted[self] != TeamReports[requester].HelpSince)
			{
				board.HelpCounted[self] = TeamReports[requester].HelpSince;
				CountDecision(NeuralDecision::TeamHelpAnswered, self);
				Debug::Log("[NeuralAI] Frame %d | Team | House #%d answers the call of house #%d (asked at frame %d, threat %d): idle army %d vehicles + %d infantry, value %d, %zu + %zu ordered -> (%d,%d)\n",
					now, self, requester, TeamReports[requester].HelpSince, TeamReports[requester].HelpThreat, TeamReports[self].IdleUnits,
					TeamReports[self].IdleInfantry, TeamReports[self].IdleValue, vehicles, infantry, order.TargetCell.X, order.TargetCell.Y);
			}

			return;
		}

		if (board.Phase == NeuralTeamPhase::Push && board.JoinedLaunch[self] != board.LaunchFrame)
		{
			board.JoinedLaunch[self] = board.LaunchFrame;
			CountDecision(NeuralDecision::TeamWaveJoined, self);
			Debug::Log("[NeuralAI] Frame %d | Team %08X | House #%d joins the %s push launched at frame %d (slot %d): idle army %d vehicles + %d infantry, value %d, %zu + %zu ordered -> (%d,%d)\n",
				now, board.MemberMask, self, board.Split ? "split" : "joint", board.LaunchFrame, board.DepartFrame[self],
				TeamReports[self].IdleUnits, TeamReports[self].IdleInfantry, TeamReports[self].IdleValue, vehicles, infantry,
				order.TargetCell.X, order.TargetCell.Y);
		}
	}

	// Telemetry: [phase, leader, targetX, targetY, split, front, answers, calling] or null alone.
	void FormatTeamTelemetry(HouseClass* pHouse, char* pBuffer, size_t size)
	{
		std::snprintf(pBuffer, size, "null");

		if (!pHouse || pHouse->ArrayIndex < 0 || pHouse->ArrayIndex >= 32)
			return;

		const uint32_t mask = TeamMaskOf(pHouse);

		if (CountBits(mask) < 2)
			return;

		for (auto const& board : TeamBoards)
		{
			if (board.MemberMask != mask)
				continue;

			const int self = pHouse->ArrayIndex;
			std::snprintf(pBuffer, size, "[%d,%d,%d,%d,%d,%d,%d,%d]", static_cast<int>(board.Phase), board.Leader,
				board.TargetCell.X, board.TargetCell.Y, board.Split ? 1 : 0, board.FrontEnemy[self], board.HelpAnswer[self],
				IsHelpActive(self, Unsorted::CurrentFrame) ? 1 : 0);
			return;
		}
	}
} // namespace

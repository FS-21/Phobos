// NeuralAI.Diplomacy.inl: Expert 3 diplomacy in a free-for-all: tacit coalitions, truces, grudges,
// traitors, stalemate breaking, duels left to run, dying houses and the low profile of the leader.
// Part of NeuralAI.cpp (included in order; one translation unit).

namespace
{
	// Why a house stayed out of a coalition (telemetry and log).
	enum class NeuralCoalitionDecline : int
	{
		None = 0,
		Roll = 1,
		SecondStrongest = 2,
		Alone = 3,
		Stronger = 4,
		Strongest = 5
	};

	constexpr const char* NeuralCoalitionDeclineNames[] = { "none", "roll", "secondStrongest", "alone", "stronger", "strongest" };
	constexpr const char* NeuralCoalitionReasonNames[] = { "none", "elimination", "leader", "stalemate", "traitor", "call" };
	constexpr const char* NeuralCoalitionChoiceNames[] = { "trigger", "grudge", "fragile", "poorest", "traitor" };
	constexpr const char* NeuralPactEndNames[] = { "betrayal", "weakened", "target gone", "rethought", "team" };

	// A house of a preset team: allied with another player house (the spawner sets the setup's teams
	// in Allies; every house is also allied with the neutral house). The Neural AI never makes an
	// alliance itself, so this is the game setup (or a human's own diplomacy).
	bool IsInPresetTeam(const HouseClass* pHouse)
	{
		if (!pHouse)
			return false;

		for (auto const pOther : HouseClass::Array)
		{
			if (pOther && pOther != pHouse && pOther->ArrayIndex >= 0 && pOther->Type && !pOther->IsNeutral()
				&& !pOther->IsObserver() && !pOther->IsInitiallyObserver() && pHouse->Allies.Contains(pOther->ArrayIndex))
			{
				return true;
			}
		}

		return false;
	}

	int CellDistance(const CellStruct& a, const CellStruct& b)
	{
		return std::max(std::abs(a.X - b.X), std::abs(a.Y - b.Y));
	}

	// A cell on or next to a truce partner: one of its known structures within 6 cells, or one of its
	// units in our current sight within 3. Superweapons skip such cells while the truce lasts.
	bool IsCellNearTrucePartner(HouseClass* pHouse, const CellStruct& cell)
	{
		if (!pHouse || CoalitionTargetOf(pHouse) < 0)
			return false;

		for (auto const pBld : BuildingClass::Array)
		{
			if (pBld && pBld->IsAlive && !pBld->InLimbo && pBld->Owner && IsTacitTrucePartner(pHouse, pBld->Owner->ArrayIndex)
				&& IsEnemyBuildingKnown(pHouse, pBld) && CellDistance(pBld->GetMapCoords(), cell) <= 6)
			{
				return true;
			}
		}

		for (auto const pTechno : TechnoClass::Array)
		{
			if (pTechno && pTechno->IsAlive && !pTechno->InLimbo && pTechno->Owner && pTechno->WhatAmI() != AbstractType::Building
				&& IsTacitTrucePartner(pHouse, pTechno->Owner->ArrayIndex) && CellDistance(pTechno->GetMapCoords(), cell) <= 3
				&& IsCellInSightOfAI(pHouse, pTechno->GetMapCoords()))
			{
				return true;
			}
		}

		return false;
	}

	// Superweapon launch guard (Micro): false, and counted, when the cell is on a truce partner.
	bool MaySuperweaponHit(HouseClass* pHouse, const CellStruct& cell)
	{
		if (!IsCellNearTrucePartner(pHouse, cell))
			return true;

		GetMacroState(pHouse).Coalition.SuperweaponsCancelled++;
		return false;
	}

	// Our orders against a partner: units aiming at its units or buildings (unless that unit is
	// attacking us: self-defense stays) or heading into its known base. Their orders are cleared;
	// with redirect, armed ground units attack-move to the coalition target instead, else go home.
	// Returns how many were touched, adding the redirected ones to outRedirected.
	int ClearOrdersAgainstPartner(HouseClass* pHouse, int partner, const std::vector<CellStruct>& partnerBase,
		CellClass* pRedirectCell, bool redirect, int& outRedirected)
	{
		const CellStruct home = GetHouseHomeCell(pHouse);
		auto const pHomeCell = home != CellStruct::Empty ? MapClass::Instance.TryGetCellAt(home) : nullptr;
		int touched = 0;

		auto inPartnerBase = [&](AbstractClass* pDest)
		{
			if (!pDest)
				return false;

			if (auto const pObj = abstract_cast<ObjectClass*>(pDest))
			{
				auto const pOwner = pObj->GetOwningHouse();

				if (pOwner && pOwner->ArrayIndex == partner)
					return true;
			}

			if (auto const pCell = abstract_cast<CellClass*>(pDest))
			{
				return std::any_of(partnerBase.begin(), partnerBase.end(),
					[&](const CellStruct& c) { return CellDistance(c, pCell->MapCoords) <= 8; });
			}

			return false;
		};

		for (auto const pTechno : TechnoClass::Array)
		{
			if (!pTechno || pTechno->Owner != pHouse || !pTechno->IsAlive || pTechno->InLimbo)
				continue;

			auto const pFoot = abstract_cast<FootClass*>(pTechno);

			if (!pFoot)
				continue;

			auto const pAimed = abstract_cast<TechnoClass*>(pFoot->Target);
			bool aimsAtPartner = pAimed && pAimed->Owner && pAimed->Owner->ArrayIndex == partner;

			// Self-defense: the partner unit is attacking us.
			if (aimsAtPartner)
			{
				auto const pItsAim = abstract_cast<TechnoClass*>(pAimed->Target);

				if (pItsAim && pItsAim->Owner == pHouse)
					aimsAtPartner = false;
			}

			const bool headsThere = inPartnerBase(pFoot->Destination) || inPartnerBase(pFoot->MegaDestination)
				|| inPartnerBase(pFoot->MegaTarget) || (!aimsAtPartner && pAimed == nullptr && inPartnerBase(pFoot->Target));

			if (!aimsAtPartner && !headsThere)
				continue;

			pFoot->SetTarget(nullptr);
			pFoot->SetDestination(nullptr, true);
			pFoot->ClearMegaMissionData();
			touched++;

			if (!redirect)
			{
				pFoot->QueueMission(Mission::Guard, false);
				continue;
			}

			auto const pType = pFoot->GetTechnoType();
			const bool armedGround = pType && pFoot->WhatAmI() != AbstractType::Aircraft && GetCombatValueOfType(pType, false) > 0
				&& !(pType->Naval);

			if (armedGround && pRedirectCell)
			{
				OrderFootToAttackMove(pFoot, pRedirectCell);
				outRedirected++;
			}
			else if (pHomeCell && pFoot->WhatAmI() != AbstractType::Aircraft)
			{
				IssueMoveOrder(pFoot, pHomeCell, 4.0);
			}
			else
			{
				pFoot->QueueMission(Mission::Guard, false);
			}
		}

		return touched;
	}

	// A truce begins: in this same tick, end our plans against the new partner (outcome Pact, not a
	// failure), recall the units on their way to it and send the armed ones to the coalition target.
	void ApplyTruceStart(HouseClass* pHouse, int partner, const std::vector<CellStruct>& partnerBase, CellClass* pTargetCell)
	{
		auto& macro = GetMacroState(pHouse);
		auto& coal = macro.Coalition;
		std::unordered_map<DWORD, int> ownerOf;

		for (auto const pTechno : TechnoClass::Array)
		{
			if (pTechno && pTechno->IsAlive && pTechno->Owner)
				ownerOf[pTechno->UniqueID] = pTechno->Owner->ArrayIndex;
		}

		auto ownedByPartner = [&](DWORD uid)
		{
			auto const it = ownerOf.find(uid);
			return uid != 0 && it != ownerOf.end() && it->second == partner;
		};

		auto nearPartnerBase = [&](const CellStruct& cell)
		{
			return cell != CellStruct::Empty && std::any_of(partnerBase.begin(), partnerBase.end(),
				[&](const CellStruct& c) { return CellDistance(c, cell) <= 8; });
		};

		for (size_t i = 0; i < macro.Plans.size(); ++i)
		{
			auto& plan = macro.Plans[i];

			if (!plan.Active)
				continue;

			const bool againstPartner = ownedByPartner(plan.TargetID) || nearPartnerBase(plan.TargetCell)
				|| std::any_of(plan.Targets.begin(), plan.Targets.end(), ownedByPartner);

			if (!againstPartner)
				continue;

			for (const DWORD uid : plan.Members)
				macro.PlanMembers.erase(uid);

			CountPlanOutcome(pHouse->ArrayIndex, static_cast<NeuralPlanId>(i), NeuralPlanOutcome::Pact);
			plan = NeuralPlanInstance {};
			macro.PlanNextAttemptFrame[i] = Unsorted::CurrentFrame + 300;
			coal.PlansCancelled++;
		}

		int redirected = 0;
		const int touched = ClearOrdersAgainstPartner(pHouse, partner, partnerBase, pTargetCell, true, redirected);
		coal.UnitsRecalled += touched - redirected;
		coal.UnitsRedirected += redirected;
	}

	void EndTacitCoalition(HouseClass* pHouse, NeuralPactEnd why, int strength)
	{
		auto& coal = GetMacroState(pHouse).Coalition;
		const int now = Unsorted::CurrentFrame;

		Debug::Log("[NeuralAI] House %d Coalition: ends against house %d (%s) after %d frames, strength %d of peak %d, "
			"truces %d, revoked %d.\n", pHouse->ArrayIndex, coal.Target, NeuralPactEndNames[static_cast<int>(why)],
			now - coal.SinceFrame, strength, coal.TargetPeakStrength, coal.Truces, coal.Revoked);

		coal.PactEnds[static_cast<size_t>(why)]++;

		if (coal.Target >= 0 && coal.Target < 8)
			coal.CooldownUntil[coal.Target] = now + PactTargetCooldownFrames;

		// Partners that did not betray us are spared for a while.
		for (int p = 0; p < 8; ++p)
		{
			if (coal.TruceSinceFrame[p] >= 0 && (coal.RevokedMask & (1u << p)) == 0)
				coal.ExPartnerUntil[p] = now + ExPartnerCooldownFrames;
		}

		coal.Target = -1;
		coal.Reason = 0;
		coal.TargetChoice = 0;
		coal.SinceFrame = -1;
		coal.RethinkUpdates = 0;
		coal.ProgressFrame = -1;
		coal.TargetPeakStrength = 0;
		coal.TargetPeakBuildings = 0;
		coal.RevokedMask = 0;
		coal.DistrustMask = 0;
		coal.BetrayedPending = false;
		coal.TruceSinceFrame.fill(-1);
		coal.PartnerSeenFrame.fill(-100000);
		coal.AimedDamage.fill(0);
		coal.CollateralDamage.fill(0);
		coal.Fronts.clear();
	}

	// Expert 3 diplomacy, every 450 frames (150 while a coalition is on), for every Neural AI house
	// outside a preset team, whichever expert leads. Each house reads only its own memory (fog-fair):
	// seen army value in the last 2700 frames (EnemySightings) plus its known structures, which house
	// it saw fighting which (armed units in current sight next to a house's known structures), the
	// harvesters and refineries it saw, the damage it took (damage hook) and the public defeat flag.
	// - Triggers: a defeat and the house our memory blames for it (among the strong); a runaway
	//   leader (seen strength 1.7x every other house's, ours counted at two thirds, 1.3x all of ours,
	//   and seen attacking a third house or twice all of ours); a stalemate (after frame 27000, no
	//   defeat for 18000 frames, the known structures of the others stable over 10800 frames while
	//   our money grew or our waves got nowhere) against the house that worst suits us (grudge), or
	//   the most fragile or the poorest one; a traitor (a betrayal we suffered or saw).
	// - Joining: by our rank among the houses outside teams. The strongest house 3% (15% when the
	//   target threatens it or is its nearest known base), the second strongest 8% plus a third of the
	//   situation, the others 30% (strongest of the rest) to 70% (weakest) plus the situation (up to
	//   +65: the target at our base, hitting us, near, twice our size, an elimination, a traitor; a far
	//   target -15). The roll is a hash of the house, the target, the trigger and the per-match synced
	//   style salt: the same answer on every client. A house that declines keeps its FFA opportunism.
	// - Pact (one at a time): the target first, truce with up to two houses seen fighting it (never a
	//   distrusted one); on a new truce our plans, waves and orders against that house end at once. The
	//   first 150 frames of a truce ignore damage between the partners (attacks already launched).
	//   Then aimed damage in force from a partner is a betrayal (pact over, lasting grudge, traitor),
	//   smaller or splash damage is collateral (the truce ends above a limit). The coalition ends when
	//   the target is weakened (seen strength 45% of its peak, or known structures 55%), gone, on
	//   betrayal, or when the house thinks again (another house became the danger, or nothing was done
	//   about the target for PactIdleFrames); it has no fixed length. Ex-partners are spared 1800
	//   frames, the target PactTargetCooldownFrames.
	// - Without a coalition: two other houses at war are left alone until it ends, then the one it
	//   left weakest is hit; a dying house is reached before its aggressor; while we lead by far we
	//   keep a low profile (no far attacks on houses that leave us alone, expansion and defense first).
	// - Call to arms: a house that stands against a target calls the others. A house without a
	//   coalition and without a defeat or a traitor to answer weighs the call first (the target with
	//   most houses against it, then the strongest), with PactCallBonus added to the situation, before
	//   looking for a leader of its own: houses that each saw a different leader used to end up alone
	//   against different targets, pacts of one. The houses against one target are partners by the
	//   pact itself (truce between them, seen fighting it or not, beyond the limit of tacit partners).
	// Several coalitions can stand at once. The call is the only thing houses share: who stands
	// against whom; what each one knows of the map stays its own.
	void UpdateTacitCoalition(HouseClass* pHouse)
	{
		const NeuralSlowStep slowStep { "UpdateTacitCoalition" };

		if (!pHouse || pHouse->ArrayIndex < 0 || pHouse->ArrayIndex >= 8)
			return;

		auto& macro = GetMacroState(pHouse);
		auto& coal = macro.Coalition;
		const int now = Unsorted::CurrentFrame;
		const int self = pHouse->ArrayIndex;
		const int interval = coal.Target >= 0 ? 150 : 450;

		if (now - coal.LastUpdateFrame < interval && now >= coal.LastUpdateFrame)
			return;

		const int elapsed = std::clamp(now - coal.LastUpdateFrame, 0, 450);
		coal.LastUpdateFrame = now;

		if (coal.Target >= 0)
			coal.ActiveFrames += elapsed;

		// Telemetry: different coalition targets standing now, over every Neural AI house.
		if (CoalitionConcurrentSerial != NeuralMatchSerial)
		{
			CoalitionConcurrentSerial = NeuralMatchSerial;
			CoalitionConcurrentNow = 0;
			CoalitionConcurrentPeak = 0;
			BetrayalsCommitted.fill(0);
		}

		// A defeated house holds no coalition: nothing updates it any more, so its target would stand
		// for the rest of the match and count among the coalitions on.
		for (auto const pGone : HouseClass::Array)
		{
			if (!pGone || !pGone->Defeated || pGone->ArrayIndex < 0 || pGone->ArrayIndex >= 8)
				continue;

			auto& gone = MacroStates[pGone->ArrayIndex].Coalition;

			if (gone.Target < 0)
				continue;

			gone.Target = -1;
			gone.Reason = 0;
			gone.TargetChoice = 0;
			gone.SinceFrame = -1;
			gone.RethinkUpdates = 0;
			gone.ProgressFrame = -1;
			gone.TruceSinceFrame.fill(-1);
			gone.Fronts.clear();
		}

		{
			uint32_t targets = 0;

			for (int i = 0; i < 8; ++i)
			{
				const int t = MacroStates[i].Coalition.Target;

				if (t >= 0 && t < 8)
					targets |= 1u << t;
			}

			CoalitionConcurrentNow = 0;

			for (int t = 0; t < 8; ++t)
			{
				if ((targets & (1u << t)) != 0)
					CoalitionConcurrentNow++;
			}

			CoalitionConcurrentPeak = std::max(CoalitionConcurrentPeak, CoalitionConcurrentNow);
		}

		if (IsInPresetTeam(pHouse))
		{
			if (coal.Target >= 0)
				EndTacitCoalition(pHouse, NeuralPactEnd::Team, 0);

			coal.LowProfile = false;
			coal.DuelA = coal.DuelB = coal.PostDuelTarget = coal.DyingTarget = -1;
			return;
		}

		// Player houses 0..7: present, alive, in a preset team.
		std::array<bool, 8> present {};
		std::array<bool, 8> alive {};
		std::array<bool, 8> team {};

		for (auto const pOther : HouseClass::Array)
		{
			if (!pOther || pOther->ArrayIndex < 0 || pOther->ArrayIndex >= 8 || !pOther->Type || pOther->IsNeutral()
				|| pOther->IsObserver() || pOther->IsInitiallyObserver())
			{
				continue;
			}

			const int idx = pOther->ArrayIndex;
			present[idx] = true;
			alive[idx] = !pOther->Defeated;
			team[idx] = IsInPresetTeam(pOther);
		}

		// Known structures per house (ours all), their cells and centroid, and a coarse 8-cell block
		// grid of which houses have structures where (bit per house), for the fighting-seen test.
		constexpr int GridSide = 64;
		std::vector<uint8_t> grid(GridSide * GridSide, 0);
		std::array<std::vector<CellStruct>, 8> knownCells {};
		std::array<int, 8> knownBuildings {};
		std::array<int, 8> knownDefenses {};
		std::array<int, 8> knownRefineries {};
		std::array<int, 8> sumX {};
		std::array<int, 8> sumY {};

		auto blockOf = [](const CellStruct& c)
		{
			const int bx = std::clamp(static_cast<int>(c.X) / 8, 0, GridSide - 1);
			const int by = std::clamp(static_cast<int>(c.Y) / 8, 0, GridSide - 1);
			return by * GridSide + bx;
		};

		for (auto const pBld : BuildingClass::Array)
		{
			if (!pBld || !pBld->IsAlive || pBld->InLimbo || !pBld->Owner || !pBld->Type || pBld->Type->InvisibleInGame
				|| pBld->Type->Insignificant)
			{
				continue;
			}

			const int owner = pBld->Owner->ArrayIndex;

			if (owner < 0 || owner >= 8 || !present[owner] || (owner != self && !IsEnemyBuildingKnown(pHouse, pBld)))
				continue;

			const CellStruct c = pBld->GetMapCoords();
			grid[blockOf(c)] |= static_cast<uint8_t>(1u << owner);
			knownCells[owner].push_back(c);
			knownBuildings[owner]++;
			sumX[owner] += c.X;
			sumY[owner] += c.Y;

			if (pBld->Type->IsBaseDefense)
				knownDefenses[owner]++;

			if (pBld->Type->Refinery || pBld->Type->ResourceDestination)
				knownRefineries[owner]++;
		}

		for (int i = 0; i < 8; ++i)
		{
			if (i != self && knownBuildings[i] > 0)
			{
				coal.LastKnownBase[i] = CellStruct { static_cast<short>(sumX[i] / knownBuildings[i]),
					static_cast<short>(sumY[i] / knownBuildings[i]) };
				coal.LastKnownBuildings[i] = knownBuildings[i];
				coal.PeakKnownBuildings[i] = std::max(coal.PeakKnownBuildings[i], knownBuildings[i]);
			}
		}

		// Seen strength: armed units seen in the last 2700 frames plus known structures.
		std::array<int, 8> armySeen {};

		for (auto const& [uid, sighting] : macro.EnemySightings)
		{
			if (sighting.OwnerIndex >= 0 && sighting.OwnerIndex < 8 && sighting.OwnerIndex != self
				&& now - sighting.LastSeenFrame <= 2700)
			{
				armySeen[sighting.OwnerIndex] += sighting.Value;
			}
		}

		int ownArmy = 0;

		for (auto const pTechno : TechnoClass::Array)
		{
			if (!pTechno || pTechno->Owner != pHouse || !pTechno->IsAlive || pTechno->InLimbo
				|| pTechno->WhatAmI() == AbstractType::Building)
			{
				continue;
			}

			auto const pType = pTechno->GetTechnoType();
			auto const pUnitType = abstract_cast<UnitTypeClass*>(pType);

			if (!pType || (pUnitType && (pUnitType->Harvester || pUnitType->ResourceGatherer)))
				continue;

			auto const pW0 = pType->GetWeapon(0);

			if (pW0 && pW0->WeaponType && pW0->WeaponType->Damage > 0)
				ownArmy += GetCombatValueOfType(pType, false);
		}

		std::array<int, 8> strength {};

		for (int i = 0; i < 8; ++i)
			strength[i] = armySeen[i] + 30 * knownBuildings[i] + 25 * knownDefenses[i];

		// Ours as the others probably see it: two thirds (we know all of ours, them only in part). A
		// house we never found (no known structure, little army seen) cannot be judged: it counts as
		// our equal. The full figure is kept for the runaway test.
		const int ownFull = ownArmy + 30 * knownBuildings[self] + 25 * knownDefenses[self];
		strength[self] = ownFull * 2 / 3;

		for (int i = 0; i < 8; ++i)
		{
			if (i != self && present[i] && alive[i] && knownBuildings[i] == 0 && strength[i] < 200)
				strength[i] = std::max(strength[i], strength[self]);
		}

		// Fighting seen: armed units in current sight next to (the 3x3 blocks around them) another
		// house's known structures, away from their own. Also: which houses hit the same third house
		// (inferred partners), which aim at which in force (a betrayal we witness), the harvesters in
		// sight, and while a coalition is on the houses seen fighting its target (at its base, at their
		// own base against it, or aiming at it) and their spots at the target's base (fronts).
		const int decay = coal.Target >= 0 ? 95 : 85;

		for (auto& row : coal.FightSeen)
		{
			for (auto& value : row)
				value = value * decay / 100;
		}

		for (auto& grudge : coal.Grudge)
			grudge = grudge * 98 / 100;

		const int target = coal.Target;
		std::array<bool, 8> partnerSeen {};
		std::array<CellStruct, 8> partnerSpot {};
		partnerSpot.fill(CellStruct::Empty);
		std::array<uint32_t, 8> attackersAt {};
		std::array<std::array<int, 8>, 8> aimedSeen {};
		std::array<int, 8> harvestersNow {};

		for (auto const pTechno : TechnoClass::Array)
		{
			if (!pTechno || !pTechno->IsAlive || pTechno->InLimbo || !pTechno->Owner || pTechno->Owner == pHouse
				|| pTechno->WhatAmI() == AbstractType::Building || pTechno->CloakState == CloakState::Cloaked
				|| pTechno->Disguised)
			{
				continue;
			}

			const int owner = pTechno->Owner->ArrayIndex;

			if (owner < 0 || owner >= 8 || !present[owner] || !alive[owner])
				continue;

			const CellStruct c = pTechno->GetMapCoords();

			if (!IsCellInSightOfAI(pHouse, c))
				continue;

			auto const pType = pTechno->GetTechnoType();

			if (auto const pUnitType = abstract_cast<UnitTypeClass*>(pType))
			{
				if (pUnitType->Harvester || pUnitType->ResourceGatherer)
					harvestersNow[owner]++;
			}

			const int value = pType ? GetCombatValueOfType(pType, false) : 0;

			if (value <= 0)
				continue;

			const int bx = std::clamp(static_cast<int>(c.X) / 8, 0, GridSide - 1);
			const int by = std::clamp(static_cast<int>(c.Y) / 8, 0, GridSide - 1);
			uint32_t nearMask = 0;

			for (int y = std::max(0, by - 1); y <= std::min(GridSide - 1, by + 1); ++y)
			{
				for (int x = std::max(0, bx - 1); x <= std::min(GridSide - 1, bx + 1); ++x)
					nearMask |= grid[y * GridSide + x];
			}

			// Units at home guard their own base, they fight nobody there.
			if ((nearMask & (1u << owner)) == 0)
			{
				for (int victim = 0; victim < 8; ++victim)
				{
					if (victim != owner && (nearMask & (1u << victim)) != 0)
					{
						coal.FightSeen[victim][owner] += value;
						attackersAt[victim] |= 1u << owner;
					}
				}
			}

			auto const pAimed = abstract_cast<TechnoClass*>(pTechno->Target);
			const int aimedOwner = (pAimed && pAimed->Owner) ? pAimed->Owner->ArrayIndex : -1;

			if (aimedOwner >= 0 && aimedOwner < 8 && aimedOwner != owner)
				aimedSeen[owner][aimedOwner] += value;

			if (target < 0 || team[owner])
				continue;

			if (owner != target)
			{
				const bool atTargetBase = (nearMask & (1u << target)) != 0 && (nearMask & (1u << owner)) == 0;

				if (aimedOwner == target || atTargetBase)
				{
					partnerSeen[owner] = true;
					partnerSpot[owner] = c;
				}
			}
			else
			{
				// The target's army at another house's base: that house fights it too.
				for (int victim = 0; victim < 8; ++victim)
				{
					if (victim != self && victim != target && !team[victim] && alive[victim] && (nearMask & (1u << victim)) != 0)
						partnerSeen[victim] = true;
				}
			}
		}

		for (int i = 0; i < 8; ++i)
			coal.HarvestersSeen[i] = std::max(coal.HarvestersSeen[i] * 9 / 10, harvestersNow[i]);

		// Witnessed betrayals: a house we earlier saw hitting a third house side by side with another
		// now aims at that other house's units or buildings in force, in our sight.
		for (int a = 0; a < 8; ++a)
		{
			for (int b = 0; b < 8; ++b)
			{
				if (a == b || a == self || b == self || team[a] || !alive[a] || coal.SideBySideFrame[a][b] <= 0
					|| now - coal.SideBySideFrame[a][b] > 5400 || aimedSeen[a][b] < 500
					|| (coal.WitnessedFrame[a][b] > 0 && now - coal.WitnessedFrame[a][b] <= 5400))
				{
					continue;
				}

				coal.WitnessedFrame[a][b] = now;
				coal.TraitorScore[a]++;
				coal.BetrayalsWitnessed++;
				Debug::Log("[NeuralAI] House %d Coalition: saw house %d turn on house %d (seen side by side, aimed value %d).\n",
					self, a, b, aimedSeen[a][b]);
			}
		}

		for (int c = 0; c < 8; ++c)
		{
			for (int a = 0; a < 8; ++a)
			{
				for (int b = a + 1; b < 8; ++b)
				{
					if ((attackersAt[c] & (1u << a)) != 0 && (attackersAt[c] & (1u << b)) != 0)
						coal.SideBySideFrame[a][b] = coal.SideBySideFrame[b][a] = now;
				}
			}
		}

		const CellStruct home = GetHouseHomeCell(pHouse);

		// Strongest of the houses outside teams other than the given one (ours at two thirds).
		auto strongestBesides = [&](int excluded)
		{
			int best = 0;

			for (int i = 0; i < 8; ++i)
			{
				if (i != excluded && alive[i] && !team[i])
					best = std::max(best, strength[i]);
			}

			return best;
		};

		// Nearest known base to ours among the houses outside teams.
		int nearestKnown = -1;

		for (int i = 0; i < 8; ++i)
		{
			if (i == self || !alive[i] || team[i] || knownBuildings[i] <= 0 || home == CellStruct::Empty)
				continue;

			if (nearestKnown < 0 || CellDistance(home, coal.LastKnownBase[i]) < CellDistance(home, coal.LastKnownBase[nearestKnown]))
				nearestKnown = i;
		}

		// Low profile: we lead the houses outside teams by far (ours at two thirds).
		coal.LowProfile = now >= 9000 && strength[self] * 10 >= strongestBesides(self) * 13 && strongestBesides(self) > 0;
		coal.LowProfileNearest = nearestKnown;

		if (coal.LowProfile)
			coal.LowProfileFrames += elapsed;

		// Two other houses at war: the strongest war we see (fighting seen both ways).
		{
			int bestA = -1;
			int bestB = -1;
			int bestWar = 0;

			for (int a = 0; a < 8; ++a)
			{
				for (int b = a + 1; b < 8; ++b)
				{
					if (a == self || b == self || !alive[a] || !alive[b] || team[a] || team[b])
						continue;

					const int war = coal.FightSeen[a][b] + coal.FightSeen[b][a];

					if (war > bestWar)
					{
						bestWar = war;
						bestA = a;
						bestB = b;
					}
				}
			}

			if (coal.DuelA >= 0)
			{
				const int a = coal.DuelA;
				const int b = coal.DuelB;
				const int war = coal.FightSeen[a][b] + coal.FightSeen[b][a];
				coal.DuelQuietUpdates = (war < 250 || !alive[a] || !alive[b]) ? coal.DuelQuietUpdates + 1 : 0;

				if (coal.DuelQuietUpdates >= 2)
				{
					// Over: finish the loser when it lost 30% of its known structures, else hit the one
					// the war left weakest compared with its strength when it began.
					const std::array<int, 2> sides { a, b };
					int pick = -1;
					int pickScore = INT_MAX;

					for (int s = 0; s < 2; ++s)
					{
						const int h = sides[s];

						if (!alive[h])
							continue;

						const bool lostBase = coal.DuelStartBuildings[s] >= 3
							&& knownBuildings[h] * 10 <= coal.DuelStartBuildings[s] * 7;
						const int ratio = (lostBase ? -1000 : 0) + strength[h] * 100 / std::max(1, coal.DuelStartStrength[s]);

						if (ratio < pickScore)
						{
							pickScore = ratio;
							pick = h;
						}
					}

					Debug::Log("[NeuralAI] House %d Coalition: war between houses %d and %d is over, striking house %d.\n",
						self, a, b, pick);

					if (pick >= 0)
					{
						coal.PostDuelTarget = pick;
						coal.PostDuelUntil = now + 3600;
						coal.PostDuelStrikes++;
					}

					coal.DuelA = coal.DuelB = -1;
					coal.DuelQuietUpdates = 0;
				}
				else if (now - coal.HurtByFrame[a] > 1800 && now - coal.HurtByFrame[b] > 1800)
				{
					coal.DuelHoldFrames += elapsed;
				}
			}
			else if (bestWar >= 600)
			{
				coal.DuelA = bestA;
				coal.DuelB = bestB;
				coal.DuelQuietUpdates = 0;
				coal.DuelStartStrength = { strength[bestA], strength[bestB] };
				coal.DuelStartBuildings = { knownBuildings[bestA], knownBuildings[bestB] };
				Debug::Log("[NeuralAI] House %d Coalition: houses %d and %d at war (seen %d), holding our attacks on them.\n",
					self, bestA, bestB, bestWar);
			}
		}

		// A dying house: its known structures at 40% of the most we knew (6+), another house's army at
		// its base. Reach it before that aggressor.
		if (coal.DyingTarget >= 0 && (now >= coal.DyingUntil || !alive[coal.DyingTarget]))
			coal.DyingTarget = -1;

		if (coal.DyingTarget < 0)
		{
			for (int d = 0; d < 8; ++d)
			{
				if (d == self || !alive[d] || team[d] || knownBuildings[d] <= 0 || coal.PeakKnownBuildings[d] < 6
					|| knownBuildings[d] * 10 > coal.PeakKnownBuildings[d] * 4 || IsTacitTrucePartner(pHouse, d))
				{
					continue;
				}

				bool aggressorThere = false;

				for (int a = 0; a < 8; ++a)
				{
					if (a != self && a != d && coal.FightSeen[d][a] >= 200)
						aggressorThere = true;
				}

				if (aggressorThere)
				{
					coal.DyingTarget = d;
					coal.DyingUntil = now + 2700;
					coal.DyingRushes++;
					Debug::Log("[NeuralAI] House %d Coalition: house %d is falling (%d of %d known structures), going for it first.\n",
						self, d, knownBuildings[d], coal.PeakKnownBuildings[d]);
					break;
				}
			}
		}

		// Defeats: the aggressor is the house whose army or structures we saw at the defeated house's
		// last known base. Nothing known there, no aggressor.
		int triggerTarget = -1;
		int triggerReason = 0;
		int triggerChoice = 0;
		int triggerSerial = -1;
		int triggerVictim = -1;

		for (int d = 0; d < 8; ++d)
		{
			if (!present[d] || alive[d] || d == self || (coal.DefeatSeenMask & (1u << d)) != 0)
				continue;

			coal.DefeatSeenMask |= 1u << d;
			coal.LastDefeatFrame = now;

			if (coal.LastKnownBuildings[d] <= 0)
				continue;

			const CellStruct base = coal.LastKnownBase[d];
			std::array<int, 8> blame {};

			for (int a = 0; a < 8; ++a)
				blame[a] = coal.FightSeen[d][a];

			for (auto const& [uid, sighting] : macro.EnemySightings)
			{
				if (sighting.OwnerIndex >= 0 && sighting.OwnerIndex < 8 && now - sighting.LastSeenFrame <= 2700
					&& CellDistance(sighting.LastCoords, base) <= 20)
				{
					blame[sighting.OwnerIndex] += sighting.Value;
				}
			}

			for (int a = 0; a < 8; ++a)
			{
				for (auto const& cell : knownCells[a])
				{
					if (a != self && CellDistance(cell, base) <= 20)
						blame[a] += 60;
				}
			}

			int aggressor = -1;

			for (int a = 0; a < 8; ++a)
			{
				if (a == d || a == self || !alive[a] || team[a])
					continue;

				if (blame[a] >= 150 && (aggressor < 0 || blame[a] > blame[aggressor]))
					aggressor = a;
			}

			Debug::Log("[NeuralAI] House %d Coalition: house %d defeated, last known base (%d,%d), aggressor %d (blame %d).\n",
				self, d, base.X, base.Y, aggressor, aggressor >= 0 ? blame[aggressor] : 0);

			// Only a strong aggressor makes a coalition: one that finished a dying neighbour is no
			// danger to the rest.
			if (aggressor >= 0 && strength[aggressor] * 10 >= strongestBesides(aggressor) * 8)
			{
				triggerTarget = aggressor;
				triggerReason = 1;
				triggerSerial = 100 + d;
				triggerVictim = d;
			}
		}

		// The known base of a house, for orders against it.
		auto pickTargetCell = [&](int house) -> CellClass*
		{
			CellClass* pBest = nullptr;
			int bestDist = INT_MAX;

			for (auto const& cell : knownCells[house])
			{
				const int dist = home != CellStruct::Empty ? CellDistance(home, cell) : 0;

				if (dist < bestDist)
				{
					bestDist = dist;
					pBest = MapClass::Instance.TryGetCellAt(cell);
				}
			}

			return pBest;
		};

		// A coalition on: a betrayal ends it; track the target and the houses fighting it, keep our
		// orders off the partners, end it when the target is weak.
		if (coal.Target >= 0)
		{
			const int t = coal.Target;

			if (coal.BetrayedPending)
			{
				EndTacitCoalition(pHouse, NeuralPactEnd::Betrayal, strength[t]);
				return;
			}

			if (!alive[t] || team[t])
			{
				EndTacitCoalition(pHouse, NeuralPactEnd::TargetGone, strength[t]);
				return;
			}

			coal.TargetPeakStrength = std::max(coal.TargetPeakStrength, strength[t]);
			coal.TargetPeakBuildings = std::max(coal.TargetPeakBuildings, knownBuildings[t]);

			if (strength[t] * 100 <= coal.TargetPeakStrength * 45
				|| (coal.TargetPeakBuildings >= 4 && knownBuildings[t] * 100 <= coal.TargetPeakBuildings * 55))
			{
				EndTacitCoalition(pHouse, NeuralPactEnd::Weakened, strength[t]);
				return;
			}

			// The pact is weighed again. Another house is the danger now: the strongest one outside
			// the pact with its army at our base or hitting us on purpose, while the target does
			// neither.
			int danger = -1;

			if (now - coal.SinceFrame >= PactRethinkMinFrames && coal.FightSeen[self][t] < 150 && now - coal.HurtByFrame[t] > 1800)
			{
				for (int x = 0; x < 8; ++x)
				{
					if (x == self || x == t || !alive[x] || team[x] || IsTacitTrucePartner(pHouse, x)
						|| (coal.FightSeen[self][x] < 150 && now - coal.HurtByFrame[x] > PactThreatFrames))
					{
						continue;
					}

					if (danger < 0 || strength[x] > strength[danger])
						danger = x;
				}
			}

			coal.RethinkUpdates = danger >= 0 ? coal.RethinkUpdates + 1 : 0;

			if (coal.RethinkUpdates >= PactRethinkUpdates)
			{
				Debug::Log("[NeuralAI] House %d Coalition: thinks again, house %d is the danger now (strength %d, at our base %d, "
					"last hit us %d frames ago).\n", self, danger, strength[danger], coal.FightSeen[self][danger],
					now - coal.HurtByFrame[danger]);
				EndTacitCoalition(pHouse, NeuralPactEnd::Rethought, strength[t]);
				return;
			}

			// Something done about the target: a wave of ours at it, anybody seen fighting it, a
			// known structure of it gone.
			bool progress = coal.ProgressFrame < 0 || coal.TargetWaves != coal.ProgressWaves || knownBuildings[t] < coal.ProgressBuildings;

			for (int p = 0; p < 8 && !progress; ++p)
				progress = partnerSeen[p];

			if (progress)
			{
				coal.ProgressFrame = now;
				coal.ProgressWaves = coal.TargetWaves;
			}

			if (progress || knownBuildings[t] > coal.ProgressBuildings)
				coal.ProgressBuildings = knownBuildings[t];

			if (now - coal.ProgressFrame >= PactIdleFrames)
			{
				Debug::Log("[NeuralAI] House %d Coalition: thinks again, nothing done about house %d for %d frames.\n", self, t,
					now - coal.ProgressFrame);
				EndTacitCoalition(pHouse, NeuralPactEnd::Rethought, strength[t]);
				return;
			}

			// The houses that stand against the same target are partners by the pact itself.
			std::array<bool, 8> declared {};

			for (int p = 0; p < 8; ++p)
			{
				declared[p] = p != self && alive[p] && !team[p] && MacroStates[p].Coalition.Target == t;

				if (declared[p])
					partnerSeen[p] = true;
			}

			CellClass* const pTargetCell = pickTargetCell(t);
			int partners = 0;

			for (int p = 0; p < 8; ++p)
			{
				if (coal.TruceSinceFrame[p] >= 0 && (coal.RevokedMask & (1u << p)) == 0)
					partners++;
			}

			for (int p = 0; p < 8; ++p)
			{
				if (!partnerSeen[p])
					continue;

				coal.PartnerSeenFrame[p] = now;

				if (partnerSpot[p] != CellStruct::Empty)
				{
					auto const it = std::find_if(coal.Fronts.begin(), coal.Fronts.end(),
						[&](const NeuralCoalitionFront& front) { return CellDistance(front.Cell, partnerSpot[p]) <= 8; });

					if (it != coal.Fronts.end())
					{
						it->Cell = partnerSpot[p];
						it->Frame = now;
					}
					else
					{
						if (coal.Fronts.size() >= 12)
						{
							coal.Fronts.erase(std::min_element(coal.Fronts.begin(), coal.Fronts.end(),
								[](const NeuralCoalitionFront& a, const NeuralCoalitionFront& b) { return a.Frame < b.Frame; }));
						}

						coal.Fronts.push_back(NeuralCoalitionFront { partnerSpot[p], now });
					}
				}

				if (coal.TruceSinceFrame[p] >= 0 || (coal.RevokedMask & (1u << p)) != 0)
					continue;

				// Never a house we caught betraying, never more than two partners.
				if (coal.TraitorScore[p] > 0)
				{
					if ((coal.DistrustMask & (1u << p)) == 0)
					{
						coal.DistrustMask |= 1u << p;
						coal.DistrustRefusals++;
						Debug::Log("[NeuralAI] House %d Coalition: no truce with house %d, a traitor (%d).\n", self, p,
							coal.TraitorScore[p]);
					}

					continue;
				}

				if ((partners >= PactMaxPartners && !declared[p]) || now - coal.HurtByFrame[p] <= 450)
					continue;

				coal.TruceSinceFrame[p] = now;
				coal.AimedDamage[p] = 0;
				coal.CollateralDamage[p] = 0;
				coal.AimedWindowFrame[p] = now;
				coal.CollateralWindowFrame[p] = now;
				coal.Truces++;
				partners++;

				const int plansBefore = coal.PlansCancelled;
				const int recalledBefore = coal.UnitsRecalled;
				const int redirectedBefore = coal.UnitsRedirected;
				ApplyTruceStart(pHouse, p, knownCells[p], pTargetCell);

				if (declared[p])
				{
					Debug::Log("[NeuralAI] House %d Coalition: pact truce with house %d, both against house %d; "
						"%d plans cancelled, %d units recalled, %d redirected.\n", self, p, t, coal.PlansCancelled - plansBefore,
						coal.UnitsRecalled - recalledBefore, coal.UnitsRedirected - redirectedBefore);
				}
				else if (partnerSpot[p] != CellStruct::Empty)
				{
					Debug::Log("[NeuralAI] House %d Coalition: tacit truce with house %d, seen fighting house %d at (%d,%d); "
						"%d plans cancelled, %d units recalled, %d redirected.\n", self, p, t, partnerSpot[p].X, partnerSpot[p].Y,
						coal.PlansCancelled - plansBefore, coal.UnitsRecalled - recalledBefore, coal.UnitsRedirected - redirectedBefore);
				}
				else
				{
					Debug::Log("[NeuralAI] House %d Coalition: tacit truce with house %d, seen holding off house %d at its base; "
						"%d plans cancelled, %d units recalled, %d redirected.\n", self, p, t, coal.PlansCancelled - plansBefore,
						coal.UnitsRecalled - recalledBefore, coal.UnitsRedirected - redirectedBefore);
				}
			}

			// Units that picked a partner target since (micro, auto-acquire) let it go.
			for (int p = 0; p < 8; ++p)
			{
				if (coal.TruceSinceFrame[p] < 0 || !IsTacitTrucePartner(pHouse, p))
					continue;

				int redirected = 0;
				coal.PartnerTargetsCleared += ClearOrdersAgainstPartner(pHouse, p, knownCells[p], nullptr, false, redirected);
			}

			return;
		}

		// Stalemate samples every 2700 frames: known structures of the others, our money, our waves.
		if (now - coal.LastSampleFrame >= 2700 || coal.LastSampleFrame > now)
		{
			coal.LastSampleFrame = now;
			int othersKnown = 0;

			for (int i = 0; i < 8; ++i)
			{
				if (i != self && !team[i])
					othersKnown += knownBuildings[i];
			}

			for (size_t s = 0; s + 1 < coal.FrontSamples.size(); ++s)
			{
				coal.FrontSamples[s] = coal.FrontSamples[s + 1];
				coal.MoneySamples[s] = coal.MoneySamples[s + 1];
				coal.WaveSamples[s] = coal.WaveSamples[s + 1];
			}

			coal.FrontSamples.back() = othersKnown;
			coal.MoneySamples.back() = pHouse->Available_Money();
			coal.WaveSamples.back() = coal.FreeWaves + coal.TargetWaves + coal.PartnerWaves + coal.OtherWaves;
			coal.SampleCount++;

			const auto [minFront, maxFront] = std::minmax_element(coal.FrontSamples.begin(), coal.FrontSamples.end());
			const bool frontStable = *maxFront - *minFront <= std::max(2, *maxFront * 15 / 100);
			const bool moneyGrew = coal.MoneySamples.back() >= coal.MoneySamples.front() + 2000;
			const bool wavesGotNowhere = coal.WaveSamples.back() - coal.WaveSamples.front() >= 2;
			coal.Stalemate = now >= StalemateMinFrame && coal.SampleCount >= static_cast<int>(coal.FrontSamples.size())
				&& now - coal.LastDefeatFrame >= StalemateNoDefeatFrames && frontStable && (moneyGrew || wavesGotNowhere);

			if (coal.Stalemate)
				coal.StalemateUpdates++;
		}

		// May this house be a coalition target now (alive, outside teams, known, not spared).
		auto targetable = [&](int i)
		{
			return i != self && alive[i] && !team[i] && knownBuildings[i] > 0 && now >= coal.CooldownUntil[i]
				&& (coal.ExPartnerUntil[i] <= now || coal.TraitorScore[i] > 0);
		};

		// A traitor we suffered or saw.
		if (triggerTarget < 0)
		{
			for (int i = 0; i < 8; ++i)
			{
				if (coal.TraitorScore[i] > 0 && targetable(i)
					&& (triggerTarget < 0 || coal.TraitorScore[i] > coal.TraitorScore[triggerTarget]))
				{
					triggerTarget = i;
				}
			}

			if (triggerTarget >= 0)
			{
				triggerReason = 4;
				triggerChoice = 4;
				triggerSerial = 200 + triggerTarget;
			}
		}

		// A call to arms: another house already stands against one we know. Joining it makes the pact.
		if (triggerTarget < 0)
		{
			std::array<int, 8> callers {};

			for (int i = 0; i < 8; ++i)
			{
				const int called = MacroStates[i].Coalition.Target;

				if (i != self && alive[i] && !team[i] && called >= 0 && called < 8 && called != self)
					callers[called]++;
			}

			for (int i = 0; i < 8; ++i)
			{
				if (callers[i] == 0 || !targetable(i))
					continue;

				if (triggerTarget < 0 || callers[i] > callers[triggerTarget]
					|| (callers[i] == callers[triggerTarget] && strength[i] > strength[triggerTarget]))
				{
					triggerTarget = i;
				}
			}

			if (triggerTarget >= 0)
			{
				triggerReason = 5;
				triggerSerial = 400 + triggerTarget;
			}
		}

		// Runaway leader: one house's seen strength far above every other house's, the part of it we
		// know already 1.3x all of ours, and seen attacking a third house (or the part we know twice
		// all of ours): a danger to everyone, not only the rival next door whose army we see most.
		if (triggerTarget < 0 && now >= 9000)
		{
			int leader = -1;

			for (int i = 0; i < 8; ++i)
			{
				if (i != self && alive[i] && !team[i] && knownBuildings[i] > 0 && (leader < 0 || strength[i] > strength[leader]))
					leader = i;
			}

			int leaderOnOthers = 0;

			for (int v = 0; leader >= 0 && v < 8; ++v)
			{
				if (v != self && v != leader)
					leaderOnOthers += coal.FightSeen[v][leader];
			}

			if (leader >= 0 && strength[leader] >= 1500 && strength[leader] * 10 >= strongestBesides(leader) * 17
				&& strength[leader] * 10 >= ownFull * 13 && (leaderOnOthers >= 200 || strength[leader] >= 2 * ownFull))
			{
				triggerTarget = leader;
				triggerReason = 2;
				triggerSerial = 0;
			}
		}

		// Stalemate: the house that worst suits us (a big grudge), else, by this house's character
		// (style salt), the most fragile (least army and defense seen) or the poorest (fewest known
		// refineries and harvesters seen). Never its money.
		if (triggerTarget < 0 && coal.Stalemate)
		{
			int grudgeBest = -1;
			int fragileBest = -1;
			int poorBest = -1;

			for (int i = 0; i < 8; ++i)
			{
				if (!targetable(i))
					continue;

				const int grudge = coal.Grudge[i] + coal.FightSeen[self][i];
				const int fragile = armySeen[i] + 25 * knownDefenses[i];
				const int poor = 3 * knownRefineries[i] + coal.HarvestersSeen[i];

				if (grudgeBest < 0 || grudge > coal.Grudge[grudgeBest] + coal.FightSeen[self][grudgeBest])
					grudgeBest = i;

				if (fragileBest < 0 || fragile < armySeen[fragileBest] + 25 * knownDefenses[fragileBest])
					fragileBest = i;

				if (poorBest < 0 || poor < 3 * knownRefineries[poorBest] + coal.HarvestersSeen[poorBest])
					poorBest = i;
			}

			if (grudgeBest >= 0 && coal.Grudge[grudgeBest] + coal.FightSeen[self][grudgeBest] >= 2000)
			{
				triggerTarget = grudgeBest;
				triggerChoice = 1;
			}
			else if ((HouseStyleSalt[self] & 1u) != 0)
			{
				triggerTarget = fragileBest;
				triggerChoice = 2;
			}
			else
			{
				triggerTarget = poorBest;
				triggerChoice = 3;
			}

			if (triggerTarget >= 0)
			{
				triggerReason = 3;
				triggerSerial = 300 + triggerTarget;
			}
		}

		if (triggerTarget < 0 || now < coal.CooldownUntil[triggerTarget])
			return;

		// The same trigger is weighed again every 2700 frames with the same roll: only a change in
		// rank, distance or threat can turn a no into a yes.
		const bool sameTrigger = coal.DecidedTarget == triggerTarget && coal.DecidedSerial == triggerSerial;

		if (sameTrigger && now - coal.LastFullFrame < 2700 && now >= coal.LastFullFrame)
			return;

		coal.LastFullFrame = now;

		// Our rank among all houses outside teams (0 = strongest), and among them without the target.
		auto byStrength = [&](int a, int b) { return strength[a] != strength[b] ? strength[a] > strength[b] : a < b; };
		std::vector<int> everyone;
		std::vector<int> others;

		for (int i = 0; i < 8; ++i)
		{
			if (alive[i] && !team[i])
			{
				everyone.push_back(i);

				if (i != triggerTarget)
					others.push_back(i);
			}
		}

		std::stable_sort(everyone.begin(), everyone.end(), byStrength);
		std::stable_sort(others.begin(), others.end(), byStrength);

		const int rankAll = static_cast<int>(std::find(everyone.begin(), everyone.end(), self) - everyone.begin());
		const int n = static_cast<int>(others.size());
		const int rank = static_cast<int>(std::find(others.begin(), others.end(), self) - others.begin());
		const bool threatened = coal.FightSeen[self][triggerTarget] >= 150 || now - coal.HurtByFrame[triggerTarget] <= 1800;
		NeuralCoalitionDecline decline = NeuralCoalitionDecline::None;
		int score = 0;

		if (n < 2)
		{
			decline = NeuralCoalitionDecline::Alone;
		}
		else if ((triggerReason == 1 || triggerReason == 2) && strength[self] >= strength[triggerTarget])
		{
			decline = NeuralCoalitionDecline::Stronger;
		}
		else
		{
			int situation = 0;

			if (coal.FightSeen[self][triggerTarget] >= 150)
				situation += 15;

			if (now - coal.HurtByFrame[triggerTarget] <= 1800)
				situation += 10;

			if (home != CellStruct::Empty && coal.LastKnownBuildings[triggerTarget] > 0)
			{
				const int dist = CellDistance(home, coal.LastKnownBase[triggerTarget]);

				if (dist <= 60)
					situation += 5;
				else if (dist >= 120)
					situation -= 15;
			}

			if (strength[triggerTarget] >= 2 * strength[self])
				situation += 5;

			if (triggerReason == 1)
				situation += 10;

			if (triggerReason == 5)
				situation += PactCallBonus;

			if (coal.TraitorScore[triggerTarget] > 0)
				situation += 20;

			if (rankAll == 0)
				score = (threatened || triggerTarget == nearestKnown) ? 15 : 3;
			else if (rank == 0)
				score = 8 + situation / 3;
			else
				score = 30 + 40 * rank / (n - 1) + situation;

			score = std::clamp(score, 0, 95);
		}

		uint32_t hash = static_cast<uint32_t>(self + 1) * 2654435761u;
		hash ^= static_cast<uint32_t>(triggerTarget + 1) * 40503u;
		hash ^= static_cast<uint32_t>(triggerSerial + 7) * 69069u;
		hash ^= HouseStyleSalt[self];
		hash ^= hash >> 15;
		hash *= 2246822519u;
		hash ^= hash >> 13;
		const int roll = static_cast<int>(hash % 100);

		if (decline == NeuralCoalitionDecline::None && roll >= score)
		{
			decline = rankAll == 0 ? NeuralCoalitionDecline::Strongest
				: rank == 0 ? NeuralCoalitionDecline::SecondStrongest : NeuralCoalitionDecline::Roll;
		}

		const bool firstLook = !sameTrigger;
		coal.DecidedTarget = triggerTarget;
		coal.DecidedSerial = triggerSerial;
		coal.DecidedJoin = decline == NeuralCoalitionDecline::None;

		if (decline != NeuralCoalitionDecline::None)
		{
			if (firstLook)
			{
				coal.Declined++;
				coal.LastDeclineReason = static_cast<int>(decline);

				if (decline == NeuralCoalitionDecline::SecondStrongest)
					coal.DeclinedSecond++;
				else if (decline == NeuralCoalitionDecline::Strongest)
					coal.DeclinedStrongest++;

				CountDecision(NeuralDecision::CoalitionDeclined, self);
				Debug::Log("[NeuralAI] House %d Coalition: declines against house %d (%s, %s, %s), rank %d of %d, overall %d, "
					"score %d, roll %d, strength %d vs ours %d.\n", self, triggerTarget, NeuralCoalitionReasonNames[triggerReason],
					NeuralCoalitionChoiceNames[triggerChoice], NeuralCoalitionDeclineNames[static_cast<int>(decline)], rank, n,
					rankAll, score, roll, strength[triggerTarget], strength[self]);
			}

			return;
		}

		coal.Target = triggerTarget;
		coal.Reason = triggerReason;
		coal.TargetChoice = triggerChoice;
		coal.SinceFrame = now;
		coal.RethinkUpdates = 0;
		coal.ProgressFrame = now;
		coal.ProgressWaves = coal.TargetWaves;
		coal.ProgressBuildings = knownBuildings[triggerTarget];
		coal.TargetPeakStrength = strength[triggerTarget];
		coal.TargetPeakBuildings = knownBuildings[triggerTarget];
		coal.RevokedMask = 0;
		coal.DistrustMask = 0;
		coal.BetrayedPending = false;
		coal.TruceSinceFrame.fill(-1);
		coal.PartnerSeenFrame.fill(-100000);
		coal.Fronts.clear();
		coal.Joined++;
		coal.JoinsByReason[static_cast<size_t>(triggerReason)]++;
		coal.JoinsByChoice[static_cast<size_t>(triggerChoice)]++;
		coal.LastTarget = triggerTarget;
		CountDecision(NeuralDecision::CoalitionJoined, self);

		Debug::Log("[NeuralAI] House %d Coalition: joins against house %d (%s, %s, victim %d), rank %d of %d, overall %d, "
			"score %d, roll %d, strength %d vs ours %d.\n", self, triggerTarget, NeuralCoalitionReasonNames[triggerReason],
			NeuralCoalitionChoiceNames[triggerChoice], triggerVictim, rank, n, rankAll, score, roll, strength[triggerTarget],
			strength[self]);
	}

} // namespace

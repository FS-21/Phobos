// NeuralAI.AirForce.inl: Expert 8 (Airforce & Sky Dominance).
// The air work there is for a house (selector prior), strike wings on known targets away from
// known anti-air with safe lanes and rearm cycles, interceptors against enemy air in sight and
// patrol stops over the front. Everything is fog-fair: known enemy buildings, enemy units in
// current sight, and the places where our own aircraft were shot down.
// Part of NeuralAI.cpp (included in order; one translation unit).

namespace
{
	struct NeuralAirStrikeOrder
	{
		DWORD TargetID { 0 };
		int Frame { 0 };
	};

	struct NeuralAirForceState
	{
		int MatchSerial { -1 };
		int SummaryFrame { -1 };
		// Combat value of our armed aircraft that hit the ground / the air (not spawned, no transports).
		int StrikeFleetValue { 0 };
		int InterceptorValue { 0 };
		int StrikeAircraft { 0 };
		// Armed air force aircraft out of the factory.
		int FleetCount { 0 };
		// Aircraft lost lately, 100 per aircraft, x7/8 every 900 frames.
		int RecentLosses { 0 };
		int LossDecayFrame { 0 };
		// Sum of the three best strike targets the current fleet could hit at its risk tolerance.
		int StrikeWork { 0 };
		int InterceptWork { 0 };
		bool CanProduceStriker { false };
		bool CanProduceInterceptor { false };
		bool CanBuildAirfield { false };
		// Centroid of our ground units fighting enemies in sight (Empty when there is no fight).
		CellStruct FrontCell { CellStruct::Empty };
		// Aircraft UniqueID -> the strike it flies for a wing. Key access only.
		std::unordered_map<DWORD, NeuralAirStrikeOrder> Orders {};
		// Aircraft fielded this match, by UniqueID (sorted: the lost count never depends on order).
		std::map<DWORD, int> Fielded {};
		int WingsLaunched { 0 };
		// First frame some strike aircraft were ready while the wing was still incomplete.
		int WingWaitFrame { -1 };
	};

	std::array<NeuralAirForceState, 32> AirForceStates {};

	// How long a wing keeps flying (and rearming for) the same strike.
	constexpr int AirStrikeOrderFrames = 4500;

	NeuralAirForceState& GetAirForceState(const HouseClass* pHouse)
	{
		auto& state = AirForceStates[std::clamp(pHouse ? pHouse->ArrayIndex : 0, 0, 31)];

		if (state.MatchSerial != NeuralMatchSerial)
			state = NeuralAirForceState { NeuralMatchSerial };

		return state;
	}

	// An armed aircraft of the air force proper: no spawned missiles or fighters of a carrier, no
	// carryalls or passenger transports.
	bool IsAirForceCraft(const AircraftTypeClass* pType)
	{
		return pType && !pType->Spawned && !pType->Carryall && pType->Passengers <= 0;
	}

	bool AircraftHitsGround(AircraftTypeClass* pType)
	{
		return GroundRangeOf(pType) > 0;
	}

	bool AircraftHitsAir(AircraftTypeClass* pType)
	{
		return GetCombatValueOfType(pType, true) > 0;
	}

	// Free for a wing: docked or guarding, on a patrol stop with nothing to shoot, or hovering at the
	// end of a move.
	bool IsAircraftIdleForOrders(AircraftClass* pAir)
	{
		const Mission mission = pAir->CurrentMission;

		return mission == Mission::Guard || mission == Mission::Sleep || (mission == Mission::Area_Guard && pAir->Target == nullptr)
			|| (mission == Mission::Move && pAir->Target == nullptr
				&& (!pAir->Destination || pAir->DistanceFrom(pAir->Destination) <= 384));
	}

	bool HasFullAirLoad(AircraftClass* pAir)
	{
		return pAir->Type->Ammo < 0 || pAir->Ammo >= pAir->Type->Ammo;
	}

	// Worth of a known enemy building as an air strike target, from its rules only.
	int AirStrikeWorthOf(const BuildingTypeClass* pType)
	{
		if (!pType || pType->Wall || pType->Insignificant || pType->InvisibleInGame)
			return 0;

		if (pType->HasSuperWeapon())
			return 400;

		if (pType->ConstructionYard)
			return 360;

		if (pType->PowerBonus > 0)
			return 300;

		if (pType->Refinery || pType->ResourceDestination)
			return 280;

		if (pType->Factory != AbstractType::None)
			return 250;

		if (pType->Radar)
			return 180;

		return pType->Cost >= 1500 ? 160 : 0;
	}

	// Anti-air risk for aircraft over a cell: known enemy anti-air defenses within 9 cells count 2
	// each, enemy anti-air units in current sight within 8 cells 1 each, and the spots where our
	// aircraft were shot down in the last 5400 frames within 6 cells 1 each (2 at most).
	int AirStrikeRiskAt(HouseClass* pHouse, const CellStruct& cell)
	{
		int risk = CountKnownEnemyDefensesNear(pHouse, cell, 9.0, true) * 2;

		ForEachTechnoNear(cell, 8, [&](TechnoClass* pTech) -> bool
		{
			if (!pTech->IsAlive || pTech->InLimbo || !pTech->Owner || pTech->Owner == pHouse || pHouse->IsAlliedWith(pTech->Owner)
				|| pTech->Owner->IsNeutral() || pTech->WhatAmI() == AbstractType::Building
				|| !IsCellInSightOfAI(pHouse, pTech->GetMapCoords()))
			{
				return true;
			}

			if (GetCombatValueOfType(pTech->GetTechnoType(), true) > 0)
				risk++;

			return risk < 12;
		});

		int danger = 0;

		for (auto const& [spot, frame] : GetMacroState(pHouse).AirDangerSpots)
		{
			if (spot.DistanceFrom(cell) <= 6.0 && Unsorted::CurrentFrame - frame <= 5400)
				danger++;
		}

		return risk + std::min(2, danger);
	}

	// Best known enemy building for a strike of tolerance + 1 aircraft from a cell: worth (more near
	// our fighting army, where defenses become worth hitting too) lowered by risk and distance. Known
	// anti-air sites themselves are never picked. Ties keep the first in building array order.
	// Damage one sortie of this aircraft does to a building (ground weapon, burst, full load,
	// against the building's armor).
	int AirSortieDamageVs(AircraftClass* pAir, BuildingClass* pTarget)
	{
		int best = 0;

		for (int i = 0; i < 2; ++i)
		{
			auto const pWS = pAir->Type->GetWeapon(i);
			auto const pWeapon = pWS ? pWS->WeaponType : nullptr;

			if (!WeaponHitsGround(pWeapon) || !pWeapon->Warhead)
				continue;

			const double verses = GeneralUtils::GetWarheadVersusArmor(pWeapon->Warhead, pTarget, pTarget->Type);
			best = std::max(best, static_cast<int>(pWeapon->Damage * std::max(1, pWeapon->Burst) * verses));
		}

		return best * (pAir->Type->Ammo > 0 ? pAir->Type->Ammo : 4);
	}

	// Targets the sorties already sent at them cover (pCommitted: target UniqueID -> damage, with a
	// 20% margin) are left for the next wing; with pStriker, only buildings it can damage count
	// (some bombs do nothing against structures).
	BuildingClass* PickAirStrikeTarget(HouseClass* pHouse, const CellStruct& from, int tolerance, const CellStruct& front,
		int* pOutScore = nullptr, int* pOutRisk = nullptr, const std::unordered_map<DWORD, int>* pCommitted = nullptr,
		AircraftClass* pStriker = nullptr)
	{
		BuildingClass* pBest = nullptr;
		int bestScore = 0;
		int bestRisk = 0;

		for (auto const& info : GetKnownEnemyBuildingCache(pHouse).Entries)
		{
			if (info.IsAA)
				continue;

			if (pCommitted)
			{
				auto const it = pCommitted->find(info.Building->UniqueID);

				if (it != pCommitted->end() && it->second >= info.Building->Health + info.Building->Health / 5)
					continue;
			}

			auto const pType = info.Building->Type;
			int worth = AirStrikeWorthOf(pType);

			if (front != CellStruct::Empty && info.Coords.DistanceFrom(front) <= 14.0)
				worth = info.IsDefense ? std::max(worth, 220) : (worth > 0 ? worth + 120 : 0);

			if (worth <= 0 || (pStriker && AirSortieDamageVs(pStriker, info.Building) <= 0))
				continue;

			const int risk = AirStrikeRiskAt(pHouse, info.Coords);

			if (risk > tolerance)
				continue;

			int score = worth * 100 / (100 + risk * 60) - static_cast<int>(from.DistanceFrom(info.Coords)) / 2;

			if (info.Building->Health * 2 < pType->Strength)
				score += score / 4;

			if (info.Building->Owner->ArrayIndex == pHouse->EnemyHouseIndex)
				score += 30;

			if (score > bestScore)
			{
				bestScore = score;
				bestRisk = risk;
				pBest = info.Building;
			}
		}

		if (pOutScore)
			*pOutScore = bestScore;

		if (pOutRisk)
			*pOutRisk = bestRisk;

		return pBest;
	}

	// Centroid of our armed ground units that are fighting an enemy in current sight; needs 3 of them.
	CellStruct FindAirSupportFront(HouseClass* pHouse)
	{
		int sumX = 0;
		int sumY = 0;
		int count = 0;

		auto consider = [&](FootClass* pFoot)
		{
			if (!pFoot || pFoot->Owner != pHouse || !pFoot->IsAlive || pFoot->InLimbo || !pFoot->IsArmed())
				return;

			auto const pTarget = abstract_cast<TechnoClass*>(pFoot->Target);

			if (!pTarget || !pTarget->Owner || pTarget->Owner == pHouse || pHouse->IsAlliedWith(pTarget->Owner)
				|| pTarget->Owner->IsNeutral() || !IsCellInSightOfAI(pHouse, pTarget->GetMapCoords()))
			{
				return;
			}

			const CellStruct c = pFoot->GetMapCoords();
			sumX += c.X;
			sumY += c.Y;
			count++;
		};

		for (auto const pUnit : UnitClass::Array)
		{
			if (pUnit && pUnit->Type && !pUnit->Type->Harvester && !IsAirborneThreatTechno(pUnit))
				consider(pUnit);
		}

		for (auto const pInf : InfantryClass::Array)
			consider(pInf);

		if (count < 3)
			return CellStruct::Empty;

		return CellStruct { static_cast<short>(sumX / count), static_cast<short>(sumY / count) };
	}

	// Refreshes the air force summary every 150 frames: fleet, what it could build, strike work at
	// the fleet's risk tolerance, enemy air seen, the front, and the fielded / lost aircraft counters.
	NeuralAirForceState& RefreshAirForceSummary(HouseClass* pHouse)
	{
		auto& state = GetAirForceState(pHouse);
		const int now = Unsorted::CurrentFrame;

		if (state.SummaryFrame >= 0 && now >= state.SummaryFrame && now - state.SummaryFrame < 150)
			return state;

		state.SummaryFrame = now;
		state.StrikeFleetValue = 0;
		state.InterceptorValue = 0;
		state.StrikeAircraft = 0;
		state.StrikeWork = 0;
		state.InterceptWork = 0;
		state.CanProduceStriker = false;
		state.CanProduceInterceptor = false;
		state.CanBuildAirfield = false;
		state.FleetCount = 0;

		if (now - state.LossDecayFrame >= 900 || now < state.LossDecayFrame)
		{
			state.LossDecayFrame = now;
			state.RecentLosses = state.RecentLosses * 7 / 8;
		}

		std::unordered_set<DWORD> alive;
		CellStruct fleetCell = CellStruct::Empty;

		for (auto const pAir : AircraftClass::Array)
		{
			if (!pAir || pAir->Owner != pHouse || !pAir->IsAlive || !IsAirForceCraft(pAir->Type) || !pAir->IsArmed())
				continue;

			alive.insert(pAir->UniqueID);

			// Aircraft still in the factory are in limbo: counted once they are out.
			if (pAir->InLimbo)
				continue;

			state.FleetCount++;

			if (state.Fielded.emplace(pAir->UniqueID, pAir->Type->ArrayIndex).second)
			{
				CountDecision(NeuralDecision::AircraftFielded, pHouse->ArrayIndex);
				Debug::Log("[NeuralAI] House %d AirForce: fielded %s %u, frame %d.\n", pHouse->ArrayIndex, pAir->Type->ID,
					pAir->UniqueID, now);
			}

			if (AircraftHitsGround(pAir->Type))
			{
				state.StrikeFleetValue += GetCombatValueOfType(pAir->Type, false);
				state.StrikeAircraft++;

				if (fleetCell == CellStruct::Empty)
					fleetCell = pAir->GetMapCoords();
			}

			if (AircraftHitsAir(pAir->Type))
				state.InterceptorValue += GetCombatValueOfType(pAir->Type, true);
		}

		for (auto it = state.Fielded.begin(); it != state.Fielded.end();)
		{
			if (alive.contains(it->first))
			{
				++it;
				continue;
			}

			CountDecision(NeuralDecision::AircraftLost, pHouse->ArrayIndex);
			state.RecentLosses += 100;
			state.Orders.erase(it->first);
			it = state.Fielded.erase(it);
		}

		if (pHouse->NumAirpads > 0)
		{
			for (auto const pType : AircraftTypeClass::Array)
			{
				if (!IsAirForceCraft(pType) || !CanBuildUnderHumanRules(pHouse, pType))
					continue;

				state.CanProduceStriker = state.CanProduceStriker || AircraftHitsGround(pType);
				state.CanProduceInterceptor = state.CanProduceInterceptor || AircraftHitsAir(pType);
			}
		}
		else if (HouseCanEverFieldAircraft(pHouse))
		{
			for (auto const pType : BuildingTypeClass::Array)
			{
				if (pType && pType->Factory == AbstractType::AircraftType && CanBuildUnderHumanRules(pHouse, pType))
				{
					state.CanBuildAirfield = true;
					break;
				}
			}
		}

		state.FrontCell = FindAirSupportFront(pHouse);

		// Strike work: the three best targets for the fleet we have (one aircraft more of
		// tolerance per aircraft, three at most); a house still building its air force plans for one.
		if (state.StrikeAircraft > 0 || state.CanProduceStriker || state.CanBuildAirfield)
		{
			const CellStruct home = pHouse->ConYards.Count > 0 && pHouse->ConYards.GetItem(0)
				? pHouse->ConYards.GetItem(0)->GetMapCoords()
				: pHouse->BaseSpawnCell;
			const CellStruct from = fleetCell != CellStruct::Empty ? fleetCell : home;
			const int tolerance = std::clamp(state.StrikeAircraft - 1, 0, 3);
			std::array<int, 3> best {};

			for (auto const& info : GetKnownEnemyBuildingCache(pHouse).Entries)
			{
				if (info.IsAA)
					continue;

				int worth = AirStrikeWorthOf(info.Building->Type);

				if (state.FrontCell != CellStruct::Empty && info.Coords.DistanceFrom(state.FrontCell) <= 14.0)
					worth = info.IsDefense ? std::max(worth, 220) : (worth > 0 ? worth + 120 : 0);

				if (worth <= 0)
					continue;

				const int risk = AirStrikeRiskAt(pHouse, info.Coords);

				if (risk > tolerance)
					continue;

				const int score = std::max(0, worth * 100 / (100 + risk * 60) - static_cast<int>(from.DistanceFrom(info.Coords)) / 2);

				if (score > best[2])
				{
					best[2] = score;
					std::sort(best.begin(), best.end(), [](int a, int b) { return a > b; });
				}
			}

			state.StrikeWork = best[0] + best[1] + best[2];
		}

		// Enemy air seen (remembered tracks, decayed): full weight while our anti-air does not
		// cover it, half otherwise.
		auto const& macro = GetMacroState(pHouse);

		if (macro.AirThreatValue > 0)
		{
			state.InterceptWork = macro.AirThreatValue > macro.OwnAACapacity
				? macro.AirThreatValue
				: macro.AirThreatValue / 2;
		}

		return state;
	}

	// How much of its air force a house keeps (percent, 25..100): fleet against the aircraft lost
	// lately. An air force that is being shot down as fast as it is built is not worth more of it.
	int AirForceSurvivalPercent(const NeuralAirForceState& state)
	{
		const int fleet = (state.FleetCount + 1) * 100;

		return std::clamp(fleet * 100 / (fleet + state.RecentLosses), 25, 100);
	}

	// Selector prior of Expert 8: the air force we own and the work it has (strike targets the
	// fleet can reach past known anti-air, enemy air an interceptor can answer). Work counts in
	// full with a fleet, less while it still has to be built, and less while our aircraft are
	// being shot down. 0 = no air work at all (veto).
	int AirForceOpportunity(HouseClass* pHouse)
	{
		auto const& state = RefreshAirForceSummary(pHouse);
		const bool canStrike = state.StrikeFleetValue > 0 || state.CanProduceStriker || state.CanBuildAirfield;
		const bool canIntercept = state.InterceptorValue > 0 || state.CanProduceInterceptor;
		const int strikePart = canStrike ? std::min(320, state.StrikeWork / 3) : 0;
		const int interceptPart = canIntercept ? std::min(280, state.InterceptWork) : 0;
		const int fleet = state.StrikeFleetValue + state.InterceptorValue;

		if (fleet == 0 && strikePart == 0 && interceptPart == 0)
			return 0;

		const int fleetPart = std::min(240, fleet / 2);
		const int readiness = fleet > 0
			? std::clamp(50 + fleet / 4, 50, 100)
			: (state.CanBuildAirfield ? 30 : 40);

		const int prior = std::min(760, fleetPart + (strikePart + interceptPart) * readiness / 100);

		return std::max(1, prior * AirForceSurvivalPercent(state) / 100);
	}

	// Next leg of a wing strike: the safe lane hop, then the attack itself. False (and the order is
	// dropped) when the target is gone or no longer known, the order is too old, or the target is
	// now covered by more anti-air than the aircraft still flying for it can take.
	bool ContinueAirStrike(HouseClass* pHouse, AircraftClass* pAir)
	{
		auto& state = GetAirForceState(pHouse);
		auto const it = state.Orders.find(pAir->UniqueID);

		if (it == state.Orders.end())
			return false;

		const DWORD targetID = it->second.TargetID;
		const int age = Unsorted::CurrentFrame - it->second.Frame;
		auto const pTarget = FindBuildingByID(targetID);

		if (!pTarget || !pTarget->Owner || pTarget->Owner == pHouse || pHouse->IsAlliedWith(pTarget->Owner)
			|| !IsEnemyBuildingKnown(pHouse, pTarget) || age < 0 || age > AirStrikeOrderFrames)
		{
			state.Orders.erase(it);
			return false;
		}

		int wing = 0;

		for (auto const& [uid, order] : state.Orders)
		{
			if (order.TargetID == targetID)
				wing++;
		}

		if (AirStrikeRiskAt(pHouse, pTarget->GetMapCoords()) > wing - 1)
		{
			state.Orders.erase(it);
			return false;
		}

		auto const pTargetCell = pTarget->GetCell();
		auto const pHop = GetSafeAirHop(pHouse, pAir, pTargetCell);

		if (!pHop || pHop == pTargetCell)
		{
			pAir->SetTarget(pTarget);
			pAir->QueueMission(Mission::Attack, true);
		}
		else
		{
			IssueMoveOrder(pAir, pHop, 3.0);
		}

		return true;
	}

	// Strike wings (Expert 8 in charge): idle strike aircraft with a full load and at least half
	// their strength wait until up to three of them are ready, then fly together at the best known
	// target their number can take (one point of anti-air risk per aircraft beyond the first). A
	// wing only takes the aircraft its target needs (sortie damage >= 120% of its health, never
	// fewer than the risk asks for); the rest form the next wing. Aircraft keep their strike
	// through rearm trips until the target falls (ContinueAirStrike).
	void UpdateAirWings(HouseClass* pHouse)
	{
		auto& state = RefreshAirForceSummary(pHouse);
		std::vector<AircraftClass*> ready;
		int strikeFleet = 0;

		for (auto const pAir : AircraftClass::Array)
		{
			if (!pAir || pAir->Owner != pHouse || !pAir->IsAlive || pAir->InLimbo || !IsAirForceCraft(pAir->Type)
				|| !pAir->IsArmed() || !AircraftHitsGround(pAir->Type) || IsPlanMember(pHouse, pAir->UniqueID)
				|| pAir->GetHealthPercentage() < 0.5)
			{
				continue;
			}

			if (state.Orders.contains(pAir->UniqueID))
				continue;

			strikeFleet++;

			if (HasFullAirLoad(pAir) && IsAircraftIdleForOrders(pAir))
				ready.push_back(pAir);
		}

		// The wing waits for its third aircraft (or the whole fleet when smaller) at most 900 frames.
		const int now = Unsorted::CurrentFrame;

		if (ready.empty())
		{
			state.WingWaitFrame = -1;
			return;
		}

		if (static_cast<int>(ready.size()) < std::min(3, strikeFleet))
		{
			if (state.WingWaitFrame < 0 || state.WingWaitFrame > now)
				state.WingWaitFrame = now;

			if (now - state.WingWaitFrame < 900)
				return;
		}

		state.WingWaitFrame = -1;

		std::sort(ready.begin(), ready.end(), [](AircraftClass* a, AircraftClass* b) { return a->UniqueID < b->UniqueID; });

		// Sortie damage the wings already flying send at each target. Key access only.
		std::unordered_map<DWORD, int> committedByTarget;
		std::unordered_map<DWORD, BuildingClass*> targetsByID;

		for (auto const pAir : AircraftClass::Array)
		{
			if (!pAir || pAir->Owner != pHouse || !pAir->IsAlive || !IsAirForceCraft(pAir->Type))
				continue;

			auto const it = state.Orders.find(pAir->UniqueID);

			if (it == state.Orders.end())
				continue;

			auto const known = targetsByID.find(it->second.TargetID);
			auto const pTarget = known != targetsByID.end() ? known->second : FindBuildingByID(it->second.TargetID);
			targetsByID[it->second.TargetID] = pTarget;

			if (pTarget)
				committedByTarget[pTarget->UniqueID] += AirSortieDamageVs(pAir, pTarget);
		}

		while (!ready.empty())
		{
			const int tolerance = std::min(3, static_cast<int>(ready.size()) - 1);
			int score = 0;
			int risk = 0;
			auto const pTarget = PickAirStrikeTarget(pHouse, ready.front()->GetMapCoords(), tolerance, state.FrontCell, &score, &risk,
				&committedByTarget, ready.front());

			// Nothing this aircraft could hit: the next one picks.
			if (!pTarget)
			{
				ready.erase(ready.begin());
				continue;
			}

			// The wing: ready aircraft in UniqueID order that can damage the target, until their sorties
			// cover it and they are at least as many as the risk asks for.
			const int need = pTarget->Health + pTarget->Health / 5;
			int committed = 0;
			std::vector<AircraftClass*> wing;

			for (auto const pCandidate : ready)
			{
				if (committed >= need && static_cast<int>(wing.size()) >= risk + 1)
					break;

				const int damage = AirSortieDamageVs(pCandidate, pTarget);

				if (damage <= 0)
					continue;

				committed += damage;
				wing.push_back(pCandidate);
			}

			// Too few for the risk once the useless aircraft are left out: this target waits.
			if (static_cast<int>(wing.size()) < risk + 1)
			{
				ready.erase(ready.begin());
				continue;
			}

			const size_t taken = wing.size();

			const CellStruct targetCell = pTarget->GetMapCoords();
			state.WingsLaunched++;
			CountDecision(NeuralDecision::AirWing, pHouse->ArrayIndex);
			Debug::Log("[NeuralAI] House %d AirWing: %d %s -> %s of house %d at (%d,%d), risk %d, score %d, front %s, frame %d (wings %d).\n",
				pHouse->ArrayIndex, static_cast<int>(taken), wing.front()->Type->ID, pTarget->Type->ID, pTarget->Owner->ArrayIndex,
				targetCell.X, targetCell.Y, risk, score, state.FrontCell != CellStruct::Empty ? "yes" : "no", Unsorted::CurrentFrame,
				state.WingsLaunched);

			committedByTarget[pTarget->UniqueID] += committed;

			for (auto const pMember : wing)
			{
				state.Orders[pMember->UniqueID] = NeuralAirStrikeOrder { pTarget->UniqueID, Unsorted::CurrentFrame };
				CountDecision(NeuralDecision::AirWingSortie, pHouse->ArrayIndex);
				ContinueAirStrike(pHouse, pMember);
			}

			std::erase_if(ready, [&wing](AircraftClass* pAir) { return std::find(wing.begin(), wing.end(), pAir) != wing.end(); });
		}
	}

	// Production shift of an aircraft type while Expert 8 is in charge: strike craft while there
	// are targets they can reach, interceptors while enemy air is seen (more while our anti-air does
	// not cover it). 0 under any other expert.
	int AirForceProductionBonus(HouseClass* pHouse, AircraftTypeClass* pType)
	{
		auto const pExt = HouseExt::Fetch(pHouse);

		if (!pExt || pExt->NeuralAI_ActiveExpert != 8 || !IsAirForceCraft(pType))
			return 0;

		auto const& state = RefreshAirForceSummary(pHouse);
		int bonus = 0;

		if (state.StrikeWork > 0 && AircraftHitsGround(pType))
			bonus += 250 + std::min(250, state.StrikeWork / 4);

		if (state.InterceptWork > 0 && AircraftHitsAir(pType))
			bonus += std::min(600, state.InterceptWork * 3);

		// Aircraft that die fast: sturdier types first, and less push for more of them.
		const int survival = AirForceSurvivalPercent(state);

		if (survival < 60)
			bonus += pType->Strength / 2 - 200;

		return bonus * survival / 100;
	}

	// Patrol stops Expert 8 puts first: the front of our army (close air support) and the target
	// of an airborne raid under way (cover for the drop), each only when its anti-air risk is low.
	void CollectAirForcePriorityStops(HouseClass* pHouse, std::vector<CellStruct>& out)
	{
		out.clear();

		auto const pExt = HouseExt::Fetch(pHouse);

		if (!pExt || pExt->NeuralAI_ActiveExpert != 8)
			return;

		auto const& state = RefreshAirForceSummary(pHouse);

		if (state.FrontCell != CellStruct::Empty && AirStrikeRiskAt(pHouse, state.FrontCell) <= 1)
			out.push_back(state.FrontCell);

		auto const& raid = GetMacroState(pHouse).Plans[static_cast<size_t>(NeuralPlanId::AirborneFront)];

		if (raid.Active && raid.TargetCell != CellStruct::Empty && AirStrikeRiskAt(pHouse, raid.TargetCell) <= 1)
			out.push_back(raid.TargetCell);
	}
}

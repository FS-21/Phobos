// NeuralAI.Preservation.inl: Army ledger (losses) and repair trips of damaged units.
// Part of NeuralAI.cpp (included in order; one translation unit).

namespace
{
	// Our ground and naval fighting units (infantry and vehicles with a weapon, no harvesters) by
	// UniqueID with their cost, refreshed every AI tick, so a unit gone from the game counts as lost.
	// Sorted by UniqueID; the telemetry publishes the totals.
	struct NeuralArmyLedger
	{
		int MatchSerial { -1 };
		std::vector<std::pair<DWORD, int>> Units {};
		int UnitsLost { 0 };
		int ValueLost { 0 };
	};

	std::array<NeuralArmyLedger, 32> ArmyLedgers {};

	NeuralArmyLedger& GetArmyLedger(const HouseClass* pHouse)
	{
		auto& ledger = ArmyLedgers[std::clamp(pHouse ? pHouse->ArrayIndex : 0, 0, 31)];

		if (ledger.MatchSerial != NeuralMatchSerial)
			ledger = NeuralArmyLedger { NeuralMatchSerial };

		return ledger;
	}

	// Average cost of our fighting units (0 without any).
	int GetArmyAverageCost(const HouseClass* pHouse)
	{
		auto const& units = GetArmyLedger(pHouse).Units;

		if (units.empty())
			return 0;

		long long sum = 0;

		for (auto const& [uid, cost] : units)
			sum += cost;

		return static_cast<int>(sum / static_cast<long long>(units.size()));
	}

	bool IsArmyLedgerUnit(TechnoClass* pTech)
	{
		auto const pType = pTech ? pTech->GetTechnoType() : nullptr;

		if (!pType || (pTech->WhatAmI() != AbstractType::Unit && pTech->WhatAmI() != AbstractType::Infantry))
			return false;

		if (auto const pUnitType = abstract_cast<UnitTypeClass*>(pType))
		{
			if (pUnitType->Harvester || pUnitType->ResourceGatherer || pUnitType->DeploysInto)
				return false;
		}

		return GetCombatValueOfType(pType, false) > 0;
	}

	// Units of the last tick that no longer exist (or died) are added to the losses. A unit in limbo
	// (inside a transport, a hospital or a garrison) still exists and is kept.
	void UpdateArmyLedger(HouseClass* pHouse)
	{
		auto& ledger = GetArmyLedger(pHouse);
		std::vector<std::pair<DWORD, int>> current;

		for (auto const pTech : TechnoClass::Array)
		{
			if (pTech && pTech->Owner == pHouse && pTech->IsAlive && pTech->Health > 0 && IsArmyLedgerUnit(pTech))
				current.emplace_back(pTech->UniqueID, pTech->GetTechnoType()->GetActualCost(pHouse));
		}

		std::sort(current.begin(), current.end());
		size_t j = 0;

		for (auto const& [uid, cost] : ledger.Units)
		{
			while (j < current.size() && current[j].first < uid)
				++j;

			if (j < current.size() && current[j].first == uid)
				continue;

			ledger.UnitsLost++;
			ledger.ValueLost += cost;
		}

		ledger.Units = std::move(current);
	}

	// Where a damaged unit gets its health back: a repair depot (UnitRepair), a hospital (Hospital,
	// infantry), one of our units with a healing weapon that works on it, or the self-heal the house
	// grants (UnitsGainSelfHeal / InfantryGainSelfHeal buildings, Phobos SelfHealGainType) while it
	// waits next to our buildings, out of the fight.
	enum class NeuralRepairKind : uint8_t
	{
		Depot,
		Hospital,
		Repairer,
		SelfHeal
	};

	// One repair trip: the unit, where it goes, where it left the fight (to return there or to the
	// nearest group of ours afterwards), and its best health so far (a trip whose healing stalls ends).
	struct NeuralRepairTrip
	{
		DWORD UID { 0 };
		NeuralRepairKind Kind { NeuralRepairKind::Depot };
		DWORD SourceID { 0 };
		int StartFrame { 0 };
		int LastOrderFrame { 0 };
		int Orders { 1 };
		int BestHealth { 0 };
		int BestHealthFrame { 0 };
		int SettledFrame { -1 };
		CellStruct ReturnCell { CellStruct::Empty };
		CellStruct WaitCell { CellStruct::Empty };
	};

	// Trips sorted by UniqueID, units back from a trip with the frame a new one is allowed, and the
	// unit types whose station trip ended without a repair (a walker, deployer or hover unit the
	// depot never takes in: it stands on the pad or next to it on Area_Guard), with the kind of
	// station: those types are not sent to a station of that kind again this match.
	struct NeuralRepairState
	{
		int MatchSerial { -1 };
		std::vector<NeuralRepairTrip> Trips {};
		std::vector<std::pair<DWORD, int>> Rested {};
		std::vector<std::pair<const TechnoTypeClass*, NeuralRepairKind>> FailedStations {};
	};

	std::array<NeuralRepairState, 32> RepairStates {};

	NeuralRepairState& GetRepairState(const HouseClass* pHouse)
	{
		auto& state = RepairStates[std::clamp(pHouse ? pHouse->ArrayIndex : 0, 0, 31)];

		if (state.MatchSerial != NeuralMatchSerial)
			state = NeuralRepairState { NeuralMatchSerial };

		return state;
	}

	NeuralRepairTrip* FindRepairTrip(const HouseClass* pHouse, DWORD uid)
	{
		auto& trips = GetRepairState(pHouse).Trips;
		auto const it = std::lower_bound(trips.begin(), trips.end(), uid, [](const NeuralRepairTrip& trip, DWORD id) { return trip.UID < id; });

		return (it != trips.end() && it->UID == uid) ? &*it : nullptr;
	}

	bool IsRepairStationFailed(const HouseClass* pHouse, const TechnoTypeClass* pType, NeuralRepairKind kind)
	{
		auto const& failed = GetRepairState(pHouse).FailedStations;

		return std::find(failed.begin(), failed.end(), std::make_pair(pType, kind)) != failed.end();
	}

	bool IsOnRepairTrip(const HouseClass* pHouse, DWORD uid)
	{
		return FindRepairTrip(pHouse, uid) != nullptr;
	}

	int CountRepairTrips(const HouseClass* pHouse)
	{
		return static_cast<int>(GetRepairState(pHouse).Trips.size());
	}

	// A unit back from a trip rests 900 frames before the next one (no shuttling between the
	// station and the front at the health threshold).
	bool IsRepairTripAllowed(const HouseClass* pHouse, DWORD uid)
	{
		auto const& state = GetRepairState(pHouse);

		if (FindRepairTrip(pHouse, uid))
			return false;

		auto const it = std::lower_bound(state.Rested.begin(), state.Rested.end(), std::make_pair(uid, INT_MIN));

		return it == state.Rested.end() || it->first != uid || Unsorted::CurrentFrame >= it->second;
	}

	void BeginRepairTrip(HouseClass* pHouse, FootClass* pFoot, NeuralRepairKind kind, DWORD sourceID, const CellStruct& waitCell)
	{
		auto& trips = GetRepairState(pHouse).Trips;
		NeuralRepairTrip trip {};
		trip.UID = pFoot->UniqueID;
		trip.Kind = kind;
		trip.SourceID = sourceID;
		trip.StartFrame = Unsorted::CurrentFrame;
		trip.LastOrderFrame = Unsorted::CurrentFrame;
		trip.BestHealth = pFoot->Health;
		trip.BestHealthFrame = Unsorted::CurrentFrame;
		trip.ReturnCell = pFoot->GetMapCoords();
		trip.WaitCell = waitCell;

		auto const it = std::lower_bound(trips.begin(), trips.end(), trip.UID, [](const NeuralRepairTrip& t, DWORD id) { return t.UID < id; });
		trips.insert(it, trip);

		const int houseIdx = pHouse->ArrayIndex;
		CountDecision(NeuralDecision::RepairRetreat, houseIdx);

		switch (kind)
		{
		case NeuralRepairKind::Depot:
			CountDecision(NeuralDecision::RepairByDepot, houseIdx);
			break;
		case NeuralRepairKind::Hospital:
			CountDecision(NeuralDecision::RepairByHospital, houseIdx);
			break;
		case NeuralRepairKind::Repairer:
			CountDecision(NeuralDecision::RepairByRepairer, houseIdx);
			break;
		default:
			CountDecision(NeuralDecision::RepairBySelfHeal, houseIdx);
			break;
		}
	}

	// The weapon of pHealer that heals pPatient (negative damage, a warhead that works on its
	// armor), or nullptr. Read from the unit, not its type: an IFV's weapon comes from its passenger.
	WeaponTypeClass* HealingWeaponFor(TechnoClass* pHealer, TechnoClass* pPatient)
	{
		for (int i = 0; i < 2; ++i)
		{
			auto const pWS = pHealer->GetWeapon(i);
			auto const pW = pWS ? pWS->WeaponType : nullptr;

			if (!pW || pW->Damage >= 0 || !pW->Warhead || (pW->Projectile && !pW->Projectile->AG))
				continue;

			if (GeneralUtils::GetWarheadVersusArmor(pW->Warhead, pPatient, pPatient->GetTechnoType()) > 0.01)
				return pW;
		}

		return nullptr;
	}

	// Our nearest unit that can heal pPatient within maxCells on its landmass, preferring one that
	// heals nobody else right now. Never a unit on a repair trip itself or below half health.
	FootClass* FindFieldRepairer(HouseClass* pHouse, FootClass* pPatient, double maxCells)
	{
		auto const pType = pPatient->GetTechnoType();
		const CellStruct pc = pPatient->GetMapCoords();
		FootClass* pBest = nullptr;
		double bestCost = 1e9;

		for (auto const pTech : TechnoClass::Array)
		{
			auto const pHealer = abstract_cast<FootClass*>(pTech);

			if (!pHealer || pHealer == pPatient || pHealer->Owner != pHouse || !pHealer->IsAlive || pHealer->InLimbo
				|| pHealer->WhatAmI() == AbstractType::Aircraft || pHealer->GetHealthPercentage() < 0.5
				|| IsOnRepairTrip(pHouse, pHealer->UniqueID) || !HealingWeaponFor(pHealer, pPatient))
			{
				continue;
			}

			const CellStruct hc = pHealer->GetMapCoords();
			double cost = hc.DistanceFrom(pc);

			if (cost > maxCells || !AreZonesConnected(pc, hc, pType->MovementZone))
				continue;

			// Busy healing another of ours.
			if (auto const pHealTarget = abstract_cast<TechnoClass*>(pHealer->Target))
			{
				if (pHealTarget != pPatient && pHealTarget->Owner == pHouse)
					cost += 12.0;
			}

			if (cost < bestCost || (cost == bestCost && pBest && pHealer->UniqueID < pBest->UniqueID))
			{
				bestCost = cost;
				pBest = pHealer;
			}
		}

		return pBest;
	}

	// Health points per frame the house self-heal gives this unit while it waits (0 = none), as
	// TechnoExt::ApplyGainedSelfHeal applies it: SelfHealGainType (or the infantry / units default),
	// the house's healing building count, the Phobos cap.
	double SelfHealRateOf(HouseClass* pHouse, FootClass* pFoot)
	{
		auto const pType = pFoot->GetTechnoType();
		auto defaultType = SelfHealGainType::NoHeal;

		if (pFoot->WhatAmI() == AbstractType::Infantry)
			defaultType = SelfHealGainType::Infantry;
		else if (pFoot->WhatAmI() == AbstractType::Unit)
			defaultType = pType->Organic ? SelfHealGainType::Infantry : SelfHealGainType::Units;

		auto healType = defaultType;

		if (auto const pTypeExt = TechnoTypeExt::TryFetch(pType))
			healType = pTypeExt->SelfHealGainType.Get(defaultType);

		if (healType == SelfHealGainType::NoHeal)
			return 0.0;

		auto const pRules = RulesClass::Instance;
		auto const pRulesExt = RulesExt::Global();
		const bool infantryHeal = healType == SelfHealGainType::Infantry;
		int count = infantryHeal ? pHouse->InfantrySelfHeal : pHouse->UnitsSelfHeal;
		auto const& cap = infantryHeal ? pRulesExt->InfantryGainSelfHealCap : pRulesExt->UnitsGainSelfHealCap;

		if (cap.isset())
			count = std::min(count, std::max(cap.Get(), 1));

		const int frames = infantryHeal ? pRules->SelfHealInfantryFrames : pRules->SelfHealUnitFrames;
		const int amount = infantryHeal ? pRules->SelfHealInfantryAmount : pRules->SelfHealUnitAmount;

		if (count <= 0 || frames <= 0 || amount <= 0)
			return 0.0;

		return static_cast<double>(amount * count) / frames;
	}

	// Where a unit that heals on its own waits: a free cell 3 cells outside our nearest building on
	// its landmass, toward the unit, within maxCells (CellStruct::Empty when there is none).
	CellStruct FindSelfHealWaitCell(HouseClass* pHouse, FootClass* pFoot, double maxCells)
	{
		auto const pType = pFoot->GetTechnoType();
		const CellStruct fc = pFoot->GetMapCoords();
		BuildingClass* pBest = nullptr;
		double bestDist = maxCells;

		for (auto const pBld : pHouse->Buildings)
		{
			if (!pBld || !pBld->IsAlive || pBld->InLimbo || !pBld->Type || pBld->Type->InvisibleInGame || pBld->Type->Wall
				|| pBld->Type->Insignificant)
			{
				continue;
			}

			const double d = pBld->GetMapCoords().DistanceFrom(fc);

			if (d < bestDist || (d == bestDist && pBest && pBld->UniqueID < pBest->UniqueID))
			{
				bestDist = d;
				pBest = pBld;
			}
		}

		if (!pBest)
			return CellStruct::Empty;

		const CellStruct bc = pBest->GetMapCoords();
		const double dx = static_cast<double>(fc.X - bc.X);
		const double dy = static_cast<double>(fc.Y - bc.Y);
		const double len = std::max(1.0, std::sqrt(dx * dx + dy * dy));
		const double out = 3.0 + std::max(pBest->Type->GetFoundationWidth(), pBest->Type->GetFoundationHeight(false)) / 2.0;

		for (const double side : { 0.0, 1.5, -1.5, 3.0, -3.0 })
		{
			const CellStruct c { static_cast<short>(bc.X + dx / len * out - dy / len * side),
				static_cast<short>(bc.Y + dy / len * out + dx / len * side) };
			auto const pCell = MapClass::Instance.TryGetCellAt(c);

			if (pCell && pCell->IsClearToMove(pType->SpeedType, true, true, -1, pType->MovementZone, -1, false)
				&& AreZonesConnected(fc, c, pType->MovementZone))
			{
				return c;
			}
		}

		return CellStruct::Empty;
	}

	// A repaired unit goes back. Plan members are left to their plan (the plan helpers skip units on
	// a trip and take this one again on the next tick). Others go to the nearest group of ours
	// within 25 cells of where they left the fight, or to that place when no enemy was seen there in
	// the last 300 frames; else they stay.
	void ReturnFromRepair(HouseClass* pHouse, FootClass* pFoot, const NeuralRepairTrip& trip)
	{
		auto& macro = GetMacroState(pHouse);

		if (macro.PlanMembers.contains(pFoot->UniqueID) || trip.ReturnCell == CellStruct::Empty)
			return;

		auto const pType = pFoot->GetTechnoType();
		const CellStruct fc = pFoot->GetMapCoords();
		FootClass* pMate = nullptr;
		double mateDist = 25.0;

		for (auto const pTech : TechnoClass::Array)
		{
			auto const pOther = abstract_cast<FootClass*>(pTech);

			if (!pOther || pOther == pFoot || pOther->Owner != pHouse || !pOther->IsAlive || pOther->InLimbo
				|| pOther->WhatAmI() == AbstractType::Aircraft || !IsArmyLedgerUnit(pOther) || IsOnRepairTrip(pHouse, pOther->UniqueID))
			{
				continue;
			}

			const CellStruct oc = pOther->GetMapCoords();
			const double d = oc.DistanceFrom(trip.ReturnCell);

			if ((d < mateDist || (d == mateDist && pMate && pOther->UniqueID < pMate->UniqueID))
				&& oc.DistanceFrom(fc) > 4.0 && AreZonesConnected(fc, oc, pType->MovementZone))
			{
				mateDist = d;
				pMate = pOther;
			}
		}

		CellClass* pGoal = pMate ? pMate->GetCell() : nullptr;

		if (!pGoal)
		{
			bool enemySeen = false;

			for (auto const& [uid, sighting] : macro.EnemySightings)
			{
				if (Unsorted::CurrentFrame - sighting.LastSeenFrame <= 300 && sighting.LastCoords.DistanceFrom(trip.ReturnCell) <= 8.0)
				{
					enemySeen = true;
					break;
				}
			}

			if (!enemySeen && AreZonesConnected(fc, trip.ReturnCell, pType->MovementZone))
				pGoal = MapClass::Instance.TryGetCellAt(trip.ReturnCell);
		}

		if (pGoal)
			OrderFootToAttackMove(pFoot, pGoal);
	}

	// Follows every trip once per AI tick: units that died on the way are counted lost, healed units
	// go back (saved), and a trip ends when its station or repairer is gone, its healing stalls or it
	// takes too long. A unit pulled away by another routine is ordered again (at most 4 orders).
	void UpdateRepairTrips(HouseClass* pHouse)
	{
		auto& state = GetRepairState(pHouse);

		if (state.Trips.empty() && state.Rested.empty())
			return;

		const int now = Unsorted::CurrentFrame;
		const int houseIdx = pHouse->ArrayIndex;

		std::erase_if(state.Rested, [now](const auto& entry) { return now >= entry.second || now + 1800 < entry.second; });

		auto findTechno = [](DWORD uid) -> TechnoClass*
		{
			for (auto const pTech : TechnoClass::Array)
			{
				if (pTech && pTech->UniqueID == uid)
					return (pTech->IsAlive && pTech->Health > 0) ? pTech : nullptr;
			}

			return nullptr;
		};

		std::vector<std::pair<DWORD, int>> rested;

		std::erase_if(state.Trips, [&](NeuralRepairTrip& trip)
			{
				auto const pFoot = abstract_cast<FootClass*>(findTechno(trip.UID));

				if (!pFoot || pFoot->Owner != pHouse)
				{
					CountDecision(NeuralDecision::RepairLostOnTrip, houseIdx);
					return true;
				}

				// Inside a hospital, a transport or on the station pad: wait.
				if (pFoot->InLimbo)
					return now - trip.StartFrame > 7200;

				const double health = pFoot->GetHealthPercentage();
				const double healed = (trip.Kind == NeuralRepairKind::Depot || trip.Kind == NeuralRepairKind::Hospital) ? 0.95 : 0.9;

				if (pFoot->Health > trip.BestHealth)
				{
					trip.BestHealth = pFoot->Health;
					trip.BestHealthFrame = now;
				}

				if (health >= healed)
				{
					CountDecision(NeuralDecision::RepairSaved, houseIdx);
					ReturnFromRepair(pHouse, pFoot, trip);
					rested.emplace_back(trip.UID, now + 900);
					return true;
				}

				auto const pSource = trip.SourceID ? findTechno(trip.SourceID) : nullptr;

				auto abandon = [&]() -> bool
				{
					// A station trip that ended unrepaired at the station or kept losing its way: that type
					// never goes to such a station again this match.
					if (abstract_cast<BuildingClass*>(pSource) && (trip.Kind == NeuralRepairKind::Depot || trip.Kind == NeuralRepairKind::Hospital)
						&& (trip.SettledFrame >= 0 || trip.Orders >= 4) && !IsRepairStationFailed(pHouse, pFoot->GetTechnoType(), trip.Kind))
					{
						state.FailedStations.emplace_back(pFoot->GetTechnoType(), trip.Kind);
					}

					CountDecision(NeuralDecision::RepairAbandoned, houseIdx);
					rested.emplace_back(trip.UID, now + 900);

					if (health >= 0.5)
						ReturnFromRepair(pHouse, pFoot, trip);

					return true;
				};

				if (now - trip.StartFrame > 5400 || (trip.SettledFrame >= 0 && now - trip.BestHealthFrame > 900))
					return abandon();

				auto const pType = pFoot->GetTechnoType();
				const bool mayReorder = now - trip.LastOrderFrame >= 300 && trip.Orders < 4;

				switch (trip.Kind)
				{
				case NeuralRepairKind::Depot:
				case NeuralRepairKind::Hospital:
				{
					auto const pBld = abstract_cast<BuildingClass*>(pSource);

					if (!pBld || pBld->InLimbo || (pBld->Owner != pHouse && !pHouse->IsAlliedWith(pBld->Owner)))
						return abandon();

					// Waiting for a busy station the engine turns the Enter order into a move to a cell
					// next to it: that is still the trip.
					auto const pDestCell = abstract_cast<CellClass*>(pFoot->Destination);
					const bool bound = pFoot->CurrentMission == Mission::Enter || pFoot->QueuedMission == Mission::Enter
						|| pFoot->ContainsLink(pBld) || IsTransportUnderRepair(pFoot) || pFoot->Destination == pBld
						|| (pFoot->CurrentMission == Mission::Move && pDestCell && pDestCell->MapCoords.DistanceFrom(pBld->GetMapCoords()) <= 4.0);
					auto const pFootCell = pFoot->GetCell();
					auto const pCellBld = pFootCell ? pFootCell->GetBuilding() : nullptr;

					if (pCellBld == pBld && trip.SettledFrame < 0)
						trip.SettledFrame = now;

					// Standing on the pad without docking: ask again once.
					if (pCellBld == pBld && !pFoot->ContainsLink(pBld) && now - trip.SettledFrame >= 300 && mayReorder)
					{
						trip.Orders++;
						trip.LastOrderFrame = now;
						OrderFootToEnter(pFoot, pBld);
					}
					else if (!bound && pCellBld != pBld)
					{
						if (!mayReorder)
							return trip.Orders >= 4 ? abandon() : false;

						trip.Orders++;
						trip.LastOrderFrame = now;
						OrderFootToEnter(pFoot, pBld);
					}

					return false;
				}
				case NeuralRepairKind::Repairer:
				{
					auto const pHealer = abstract_cast<FootClass*>(pSource);

					if (!pHealer || pHealer->InLimbo || pHealer->Owner != pHouse || !HealingWeaponFor(pHealer, pFoot))
						return abandon();

					const double d = pFoot->GetMapCoords().DistanceFrom(pHealer->GetMapCoords());

					if (d <= 3.0 && trip.SettledFrame < 0)
						trip.SettledFrame = now;

					if (d > 3.0 && mayReorder)
					{
						trip.Orders++;
						trip.LastOrderFrame = now;
						IssueMoveOrder(pFoot, pHealer->GetCell(), 2.0);
					}

					// The healer works on the unit unless it heals a unit in worse shape.
					auto const pHealTarget = abstract_cast<TechnoClass*>(pHealer->Target);

					if (pHealTarget != pFoot && (!pHealTarget || pHealTarget->Owner != pHouse
						|| pHealTarget->GetHealthPercentage() > health || pHealTarget->GetHealthPercentage() >= 0.99))
					{
						pHealer->SetTarget(pFoot);
						pHealer->QueueMission(Mission::Attack, true);
					}

					return false;
				}
				default:
				{
					if (SelfHealRateOf(pHouse, pFoot) <= 0.0)
						return abandon();

					const double d = pFoot->GetMapCoords().DistanceFrom(trip.WaitCell);

					if (d <= 3.0 && trip.SettledFrame < 0)
						trip.SettledFrame = now;

					if (d > 3.0 && mayReorder && AreZonesConnected(pFoot->GetMapCoords(), trip.WaitCell, pType->MovementZone))
					{
						trip.Orders++;
						trip.LastOrderFrame = now;
						IssueMoveOrder(pFoot, MapClass::Instance.TryGetCellAt(trip.WaitCell), 2.0);
					}

					return false;
				}
				}
			});

		for (auto const& entry : rested)
		{
			auto const it = std::lower_bound(state.Rested.begin(), state.Rested.end(), std::make_pair(entry.first, INT_MIN));

			if (it != state.Rested.end() && it->first == entry.first)
				it->second = entry.second;
			else
				state.Rested.insert(it, entry);
		}
	}

	void FormatArmyTelemetry(const HouseClass* pHouse, char* pBuffer, size_t size)
	{
		auto const& ledger = GetArmyLedger(pHouse);
		std::snprintf(pBuffer, size, "%d, %d, %d", ledger.UnitsLost, ledger.ValueLost, CountRepairTrips(pHouse));
	}
}

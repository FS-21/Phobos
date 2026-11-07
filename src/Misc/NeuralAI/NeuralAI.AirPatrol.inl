// NeuralAI.AirPatrol.inl
// Cyclic air patrols over key points with Phobos' extended aircraft missions: an aircraft on
// area guard attacks what it finds there, flies home to rearm when its ammo is spent and comes
// back to the guarded point on its own (ArchiveTarget). The AI moves that point along a route
// (engineers on a capture first, then captured techs and refineries) every few minutes.

namespace
{
	struct NeuralAirPatrolTrack
	{
		CellStruct Point { CellStruct::Empty };
		int Frame { 0 };
	};

	struct NeuralAirPatrolState
	{
		int MatchSerial { -1 };
		// Next stop on the route (round robin over the current point list).
		int NextPoint { 0 };
		// Aircraft UniqueID -> its current patrol stop. Key access only.
		std::unordered_map<DWORD, NeuralAirPatrolTrack> Tracks {};
		int Assigned { 0 };
	};

	std::array<NeuralAirPatrolState, 32> AirPatrolStates {};

	// How long an aircraft guards one stop before it is sent to the next one.
	constexpr int AirPatrolStopFrames = 1500;

	NeuralAirPatrolState& GetAirPatrolState(const HouseClass* pHouse)
	{
		auto& state = AirPatrolStates[std::clamp(pHouse ? pHouse->ArrayIndex : 0, 0, 31)];

		if (state.MatchSerial != NeuralMatchSerial)
			state = NeuralAirPatrolState { NeuralMatchSerial };

		return state;
	}

	// The aircraft can fly area guard sorties and rearm by itself (Phobos extended missions on,
	// armed, docks at an airfield, not a spawned or transport craft).
	bool CanFlyAirPatrol(AircraftClass* pAir)
	{
		auto const pType = pAir ? pAir->Type : nullptr;

		if (!pType || pType->Spawned || pType->Carryall || pType->Passengers > 0 || !pAir->IsArmed()
			|| !(pType->AirportBound || pType->Dock.Count > 0))
		{
			return false;
		}

		auto const pTypeExt = AircraftTypeExt::Fetch(pType);

		return pTypeExt && pTypeExt->ExtendedAircraftMissions.Get(RulesExt::Global()->ExtendedAircraftMissions);
	}

	// Patrol stops in route order: techs an engineer of ours is walking to (air cover), then our
	// captured techs and refineries, each group by UniqueID. Stops inside known enemy defenses
	// are left out. With Expert 8 in charge, the front of our army and the target of an airborne
	// raid under way replace the route while there is one (NeuralAI.AirForce.inl).
	void CollectAirPatrolPoints(HouseClass* pHouse, std::vector<CellStruct>& out)
	{
		CollectAirForcePriorityStops(pHouse, out);

		if (!out.empty())
			return;

		for (auto const& capture : GetSpecOpsState(pHouse).Captures)
		{
			if (!IsCellInKnownEnemyDefenseReach(pHouse, capture.TargetCell, 2))
				out.push_back(capture.TargetCell);
		}

		std::vector<std::pair<DWORD, CellStruct>> owned;

		for (auto const pBld : pHouse->Buildings)
		{
			if (!pBld || !pBld->IsAlive || pBld->InLimbo || !pBld->Type)
				continue;

			if (pBld->Type->Capturable || pBld->Type->Refinery)
				owned.emplace_back(pBld->UniqueID, pBld->GetMapCoords());
		}

		std::sort(owned.begin(), owned.end(), [](const auto& a, const auto& b) { return a.first < b.first; });

		for (auto const& [id, cell] : owned)
		{
			if (!IsCellInKnownEnemyDefenseReach(pHouse, cell, 2))
				out.push_back(cell);
		}
	}

	// Idle armed aircraft with nothing to strike: send it to (or keep it on) its patrol stop.
	// True when the aircraft got a patrol order this tick or keeps guarding its stop.
	bool AssignAirPatrol(HouseClass* pHouse, AircraftClass* pAir)
	{
		if (!CanFlyAirPatrol(pAir))
			return false;

		auto& state = GetAirPatrolState(pHouse);
		const int now = Unsorted::CurrentFrame;
		auto const it = state.Tracks.find(pAir->UniqueID);

		// Still on its stop: the extended area guard does the work (and the rearm trips).
		if (it != state.Tracks.end() && now - it->second.Frame < AirPatrolStopFrames && pAir->ArchiveTarget
			&& pAir->GetCurrentMission() == Mission::Area_Guard)
		{
			return true;
		}

		// A new stop only with a full load, so a sortie is never cut short on its way back.
		if (pAir->Type->Ammo > 0 && pAir->Ammo < pAir->Type->Ammo)
			return false;

		std::vector<CellStruct> points;
		CollectAirPatrolPoints(pHouse, points);

		if (points.empty())
			return false;

		const int stop = state.NextPoint % static_cast<int>(points.size());
		const CellStruct point = points[static_cast<size_t>(stop)];
		state.NextPoint = (state.NextPoint + 1) % 1024;

		auto const pCell = MapClass::Instance.TryGetCellAt(point);

		if (!pCell)
			return false;

		if (GetAirForceState(pHouse).FrontCell == point)
			CountDecision(NeuralDecision::AirFrontPatrol, pHouse->ArrayIndex);

		pAir->SetTarget(nullptr);
		pAir->SetArchiveTarget(pCell);
		pAir->SetDestination(pCell, true);
		pAir->QueueMission(Mission::Area_Guard, false);

		// Forget aircraft that stopped patrolling long ago (dead, or flying other jobs).
		if (state.Tracks.size() > 64)
		{
			std::erase_if(state.Tracks, [now](const auto& entry) { return now - entry.second.Frame > 4 * AirPatrolStopFrames; });
		}

		state.Tracks[pAir->UniqueID] = NeuralAirPatrolTrack { point, now };
		state.Assigned++;
		Debug::Log("[NeuralAI] House %d AirPatrol: %s %u -> (%d,%d), stop %d of %d, frame %d (patrols %d).\n",
			pHouse->ArrayIndex, pAir->Type->ID, pAir->UniqueID, point.X, point.Y, stop + 1,
			static_cast<int>(points.size()), now, state.Assigned);

		return true;
	}
}

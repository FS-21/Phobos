// Progress clocks (rule consolidation, case 5): one keyed record of how long each unit of a house has
// kept to one spot, by the rule of each stuck watchdog (NeuralClock). Each watchdog samples its clock
// where it did before; a sample starts with BeginClockSample and keeps a unit's clock with KeepClock.
// A clock goes on only from the sample just before (CarriedClock): a unit that sample did not keep
// starts anew, as when each watchdog rebuilt its own map every tick.
namespace
{
	// Starts a sample of a clock for a house: the serial of the sample before it, which a clock must
	// have been kept by to go on.
	int BeginClockSample(NeuralHouseMacroState& macro, NeuralClock clock)
	{
		auto& serial = macro.ClockSampleSerial[static_cast<size_t>(clock)];
		const int previous = serial;
		serial = ++macro.ClockSerialCounter;

		return previous;
	}

	// The clock of a unit as the sample `previous` kept it, or nullptr when that sample did not keep it.
	const NeuralClockState* CarriedClock(const NeuralHouseMacroState& macro, DWORD uid, NeuralClock clock, int previous)
	{
		if (previous <= 0)
			return nullptr;

		auto const it = macro.ProgressClocks.find(uid);

		if (it == macro.ProgressClocks.end())
			return nullptr;

		auto const& state = it->second[static_cast<size_t>(clock)];

		return state.Seen == previous ? &state : nullptr;
	}

	// The clock of a unit as the last sample kept it (nullptr: that sample did not keep it).
	const NeuralClockState* GetClock(const NeuralHouseMacroState& macro, DWORD uid, NeuralClock clock)
	{
		return CarriedClock(macro, uid, clock, macro.ClockSampleSerial[static_cast<size_t>(clock)]);
	}

	// Keeps the clock of a unit in the sample under way.
	void KeepClock(NeuralHouseMacroState& macro, DWORD uid, NeuralClock clock, NeuralClockState state)
	{
		state.Seen = macro.ClockSampleSerial[static_cast<size_t>(clock)];
		macro.ProgressClocks[uid][static_cast<size_t>(clock)] = state;
	}

	// Drops the clock of a unit: the next sample starts it anew.
	void DropClock(NeuralHouseMacroState& macro, DWORD uid, NeuralClock clock)
	{
		if (auto const it = macro.ProgressClocks.find(uid); it != macro.ProgressClocks.end())
			it->second[static_cast<size_t>(clock)].Seen = -1;
	}

	// Every ClockPurgeFrames, the units none of whose clocks the last sample of that clock kept go:
	// no sample can carry them on (dead units, units no watchdog looks at any more).
	void PurgeClocks(NeuralHouseMacroState& macro)
	{
		const int now = Unsorted::CurrentFrame;

		if (now >= macro.ClockPurgeFrame && now - macro.ClockPurgeFrame < ClockPurgeFrames)
			return;

		macro.ClockPurgeFrame = now;
		std::erase_if(macro.ProgressClocks, [&macro](const auto& entry)
			{
				for (size_t c = 0; c < NeuralClockCount; ++c)
				{
					if (entry.second[c].Seen == macro.ClockSampleSerial[c] && entry.second[c].Seen > 0)
						return false;
				}

				return true;
			});
	}

	// TEMP diagnostic (rule consolidation, case 5): a clock the old map of a watchdog and the registry
	// carry on differently (kept or not, cell, since when, ammo), ClockShadowLogLines a match; every
	// ClockShadowSummaryFrames the samples and mismatches of each clock over all houses. With none over a
	// desync test and some matches, the old maps go.
	struct NeuralClockShadowStats
	{
		int Serial { -1 };
		int LogCount { 0 };
		int SummaryFrame { 0 };
		std::array<int, NeuralClockCount> Samples {};
		std::array<int, NeuralClockCount> Mismatches {};
	};

	NeuralClockShadowStats& ClockShadowStats()
	{
		static NeuralClockShadowStats stats;

		if (stats.Serial != NeuralMatchSerial)
		{
			stats = NeuralClockShadowStats {};
			stats.Serial = NeuralMatchSerial;
			stats.SummaryFrame = Unsorted::CurrentFrame;
		}

		return stats;
	}

	void NoteClockShadow(const HouseClass* pHouse, NeuralClock clock, DWORD uid, bool oldKept, const CellStruct& oldCell, int oldSince,
		int oldAux, int oldAuxSince, const NeuralClockState* pNew)
	{
		static constexpr const char* Names[NeuralClockCount] = { "Still", "OrderStill", "Hover", "Air", "HarvestIdle", "Unload" };
		auto& stats = ClockShadowStats();
		const size_t c = static_cast<size_t>(clock);
		stats.Samples[c]++;
		const bool same = oldKept == (pNew != nullptr)
			&& (!oldKept || (pNew->Anchor == oldCell && pNew->Since == oldSince && pNew->Aux == oldAux && pNew->AuxSince == oldAuxSince));

		if (same)
			return;

		stats.Mismatches[c]++;

		if (stats.LogCount++ < ClockShadowLogLines)
		{
			Debug::Log("[NeuralAI] progress shadow DIAG house %d frame %d: %s %u old %s (%d,%d) since %d aux %d/%d new %s (%d,%d) since %d aux %d/%d.\n",
				pHouse ? pHouse->ArrayIndex : -1, Unsorted::CurrentFrame, Names[c], uid, oldKept ? "kept" : "none", oldCell.X, oldCell.Y,
				oldSince, oldAux, oldAuxSince, pNew ? "kept" : "none", pNew ? pNew->Anchor.X : -1, pNew ? pNew->Anchor.Y : -1,
				pNew ? pNew->Since : -1, pNew ? pNew->Aux : -1, pNew ? pNew->AuxSince : -1);
		}
	}

	void NoteTrackShadow(const HouseClass* pHouse, NeuralClock clock, DWORD uid, const NeuralStuckTrack* pOld, const NeuralClockState* pNew)
	{
		NoteClockShadow(pHouse, clock, uid, pOld != nullptr, pOld ? pOld->Cell : CellStruct::Empty, pOld ? pOld->SinceFrame : 0, 0, 0, pNew);
	}

	const NeuralStuckTrack* FindTrack(const std::unordered_map<DWORD, NeuralStuckTrack>& tracks, DWORD uid)
	{
		auto const it = tracks.find(uid);

		return it != tracks.end() ? &it->second : nullptr;
	}

	void LogClockShadowSummary()
	{
		auto& stats = ClockShadowStats();
		const int now = Unsorted::CurrentFrame;

		if (now - stats.SummaryFrame < ClockShadowSummaryFrames)
			return;

		stats.SummaryFrame = now;
		Debug::Log("[NeuralAI] progress shadow (TEMP) frame %d: Still %d/%d, OrderStill %d/%d, Hover %d/%d, Air %d/%d, HarvestIdle %d/%d, "
			"Unload %d/%d (mismatches/samples).\n", now, stats.Mismatches[0], stats.Samples[0], stats.Mismatches[1], stats.Samples[1],
			stats.Mismatches[2], stats.Samples[2], stats.Mismatches[3], stats.Samples[3], stats.Mismatches[4], stats.Samples[4],
			stats.Mismatches[5], stats.Samples[5]);
	}
} // namespace

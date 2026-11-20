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
} // namespace

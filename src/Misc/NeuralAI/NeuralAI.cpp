#include "NeuralAI.h"

#include <AircraftClass.h>
#include <AircraftTypeClass.h>
#include <BeaconManagerClass.h>
#include <BuildingClass.h>
#include <BuildingTypeClass.h>
#include <CCINIClass.h>
#include <CellClass.h>
#include <ColorScheme.h>
#include <FactoryClass.h>
#include <FootClass.h>
#include <GameModeOptionsClass.h>
#include <HouseClass.h>
#include <InfantryClass.h>
#include <InfantryTypeClass.h>
#include <MapClass.h>
#include <MessageListClass.h>
#include <OverlayClass.h>
#include <OverlayTypeClass.h>
#include <RulesClass.h>
#include <ScenarioClass.h>
#include <SessionClass.h>
#include <SuperClass.h>
#include <TechnoClass.h>
#include <TemporalClass.h>
#include <TerrainClass.h>
#include <TubeClass.h>
#include <VeinholeMonsterClass.h>
#include <TerrainTypeClass.h>
#include <UnitClass.h>
#include <UnitTypeClass.h>
#include <Ext/AircraftType/Body.h>
#include <Ext/BuildingType/Body.h>
#include <Ext/Foot/Body.h>
#include <Ext/House/Body.h>
#include <Ext/Rules/Body.h>
#include <Ext/SWType/Body.h>
#include <Ext/Techno/Body.h>
#include <Ext/TechnoType/Body.h>
#include <Ext/Unit/Body.h>
#include <Ext/TerrainType/Body.h>
#include <Ext/WarheadType/Body.h>
#include <Ext/WeaponType/Body.h>
#include <Utilities/AresHelper.h>
#include <Utilities/Debug.h>
#include <Utilities/GeneralUtils.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <unordered_map>
#include <unordered_set>

bool NeuralAI::HeadlessMode = false;
int NeuralAI::TrainSlotIndex = 0;
bool NeuralAI::TrainingMode = false;
int NeuralAI::MatchEndWinnerHouse = -1;
bool NeuralAI::MatchEndStalemate = false;

HANDLE NeuralAI::BaseFileHandle = INVALID_HANDLE_VALUE;

HANDLE NeuralAI::BaseMappingHandle = NULL;

const uint8_t* NeuralAI::BaseMappedView = nullptr;

size_t NeuralAI::BaseMappedSize = 0;

HANDLE NeuralAI::TelemetryShmHandle = NULL;

AITrainerSharedTelemetry* NeuralAI::TelemetryView = nullptr;

namespace
{
	// 256 KB: radar points carry per-object details (mission, health, target, destination) for
	// the dashboard's map inspector. The trainer opens the same size.
	constexpr uint32_t TelemetryJsonShmCapacity = 262144;

	HANDLE TelemetryJsonShmHandle = NULL;

	uint8_t* TelemetryJsonView = nullptr;

	// The trainer created the JSON mapping before starting the game (it reads it). Otherwise
	// (developer repro runs) the snapshot also goes to AI\train_telemetry_<slot>.json.
	bool TelemetryJsonShmFromTrainer = false;

	// What the trainer's map view of this slot asks for, written by the trainer: 0 nothing, 1 the
	// maximized view is open, 2 + house index while it shows what that house knows.
	constexpr uint32_t TelemetryViewShmCapacity = 16;

	HANDLE TelemetryViewShmHandle = NULL;

	const volatile uint32_t* TelemetryViewRequest = nullptr;

	// Headless training trajectory ring buffer (zero bytes allocated in normal play).
	HANDLE TrajectoryShmHandle = NULL;
	AITrajectorySharedBuffer* TrajectoryView = nullptr;

	// Optional per-slot base models handed over by the Trainer during evaluations
	// (e.g. immutable Gen-0 untrained anchor or frozen previous generation), so each
	// policy slot evaluates its own ai_base weights instead of sharing BaseMappedView.
	struct SlotBaseMapping
	{
		HANDLE FileHandle = INVALID_HANDLE_VALUE;
		HANDLE MappingHandle = NULL;
		const uint8_t* View = nullptr;
		size_t Size = 0;

		void Close()
		{
			if (this->View)
			{
				UnmapViewOfFile(this->View);
				this->View = nullptr;
			}
			if (this->MappingHandle)
			{
				CloseHandle(this->MappingHandle);
				this->MappingHandle = NULL;
			}
			if (this->FileHandle != INVALID_HANDLE_VALUE)
			{
				CloseHandle(this->FileHandle);
				this->FileHandle = INVALID_HANDLE_VALUE;
			}
			this->Size = 0;
		}
	};

	SlotBaseMapping ChallengerSlotBase {};
	SlotBaseMapping OpponentSlotBase {};
	bool LoggedInvalidV3GateHeader = false;
	bool LoggedInvalidV3MlpHeader = false;
	bool LoggedExpertOffsetFallback = false;

	// Exact floor(numerator / sqrt(1 + count)) using pure 64-bit integer arithmetic
	// (k <= num / sqrt(1 + count) <=> k^2 <= floor(num^2 / (1 + count))), avoiding FPU/SSE ulp drift.
	int ExactInvSqrtScaled(int numerator, uint32_t count)
	{
		if (numerator <= 0)
			return 0;

		const uint64_t numSq = static_cast<uint64_t>(numerator) * static_cast<uint64_t>(numerator);
		const uint64_t denom = 1ULL + static_cast<uint64_t>(count);
		const uint64_t quot = numSq / denom;
		if (quot == 0)
			return 0;

		uint64_t x = quot;
		uint64_t y = (x + 1ULL) >> 1;
		while (y < x)
		{
			x = y;
			y = (x + quot / x) >> 1;
		}
		return static_cast<int>(x);
	}
} // namespace

AIBackpackFile NeuralAI::Backpack = {};

bool NeuralAI::BackpackLoaded = false;

AIBackpackFile NeuralAI::OpponentBackpack = {};

bool NeuralAI::OpponentLoaded = false;
bool NeuralAI::AllowBackpackInference = false;

uint64_t NeuralAI::CurrentRulesSignature = 0;

uint64_t NeuralAI::CurrentBaseSignature = 0;

int NeuralAI::LastCombatFrame = 0;
int NeuralAI::LastTotalKillsAndCaptures = 0;
DWORD NeuralAI::LastFPSCheckTick = 0;
int NeuralAI::FramesSinceLastFPSCheck = 0;
int NeuralAI::CurrentMeasuredFPS = 0;

// The Neural AI is split by area into .inl parts, included in this exact order (one
// translation unit, so internal linkage and declaration order are unchanged).
#include "NeuralAI.Internal.inl"
#include "NeuralAI.Economy.inl"
#include "NeuralAI.State.inl"
#include "NeuralAI.RouteHeat.inl"
#include "NeuralAI.Expansion.inl"
#include "NeuralAI.Intel.inl"
#include "NeuralAI.Team.inl"
#include "NeuralAI.Bridges.inl"
#include "NeuralAI.WaitCells.inl"
#include "NeuralAI.Progress.inl"
#include "NeuralAI.Lifecycle.inl"
#include "NeuralAI.Telemetry.inl"
#include "NeuralAI.Features.inl"
#include "NeuralAI.Experts.inl"
#include "NeuralAI.Transport.inl"
#include "NeuralAI.Tactics.inl"
#include "NeuralAI.WaveTiming.inl"
#include "NeuralAI.Counters.inl"
#include "NeuralAI.Orders.inl"
#include "NeuralAI.Air.inl"
#include "NeuralAI.Preservation.inl"
#include "NeuralAI.Plans.inl"
#include "NeuralAI.Diplomacy.inl"
#include "NeuralAI.MobileFactory.inl"
#include "NeuralAI.SpecOps.inl"
#include "NeuralAI.AirForce.inl"
#include "NeuralAI.AirPatrol.inl"
#include "NeuralAI.Urban.inl"
#include "NeuralAI.Defense.inl"
#include "NeuralAI.Operations.inl"
#include "NeuralAI.Micro.inl"
#include "NeuralAI.Scouting.inl"
#include "NeuralAI.Production.inl"
#include "NeuralAI.Placement.inl"

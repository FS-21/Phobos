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
#include <TerrainTypeClass.h>
#include <UnitClass.h>
#include <UnitTypeClass.h>
#include <Ext/BuildingType/Body.h>
#include <Ext/House/Body.h>
#include <Ext/Rules/Body.h>
#include <Ext/SWType/Body.h>
#include <Ext/Techno/Body.h>
#include <Ext/TechnoType/Body.h>
#include <Ext/Unit/Body.h>
#include <Ext/TerrainType/Body.h>
#include <Ext/WarheadType/Body.h>
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
#include "NeuralAI.Expansion.inl"
#include "NeuralAI.Intel.inl"
#include "NeuralAI.Bridges.inl"
#include "NeuralAI.Lifecycle.inl"
#include "NeuralAI.Telemetry.inl"
#include "NeuralAI.Features.inl"
#include "NeuralAI.Experts.inl"
#include "NeuralAI.Transport.inl"
#include "NeuralAI.Tactics.inl"
#include "NeuralAI.Counters.inl"
#include "NeuralAI.Orders.inl"
#include "NeuralAI.Air.inl"
#include "NeuralAI.Plans.inl"
#include "NeuralAI.MobileFactory.inl"
#include "NeuralAI.Micro.inl"
#include "NeuralAI.Production.inl"
#include "NeuralAI.Placement.inl"

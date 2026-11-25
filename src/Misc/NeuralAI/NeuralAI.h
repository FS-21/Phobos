#pragma once

#include <Windows.h>
#include <YRPP.h>
#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

class HouseClass;
class BuildingClass;
class BuildingTypeClass;
class TechnoClass;
class TechnoTypeClass;
class SuperClass;

#pragma pack(push, 1)

struct AIBaseHeader
{
	char Magic[8]; // "PHOBOSAI"
	uint32_t SchemaVersion;
	uint32_t TotalSizeBytes;
	uint64_t BaseModelHash;
	uint32_t NumExperts;
	uint32_t NumArchetypes;
	uint32_t InputDim;
	uint32_t ReservedInputDim;
	uint32_t HiddenDim1;
	uint32_t HiddenDim2;
	uint32_t OutputDim;
	uint32_t ReservedOutputDim;
	uint32_t ExpertWeightsOffset[8];
	int8_t SigmoidLUT[256];
	int8_t TanhLUT[256];
	uint32_t ExtraExpertWeightsOffset[2];
	uint32_t GateWeightsOffset;
	uint32_t MlpHiddenDim;
	uint32_t GateHiddenDim;
	uint32_t TanhLut16Offset;
	uint8_t ReservedHeader[360];
};

struct AIBackpackEntry
{
	uint32_t StateClusterHash;
	int16_t ArchetypeScoreDelta[8];
	int16_t ExpertScoreDelta[8];
	int16_t BuildCategoryDelta[16];
	uint32_t VisitCount;
};

struct AIBackpackFile
{
	static constexpr uint32_t MaxSupportedSides = 32;
	static constexpr uint32_t EntryCapacity = MaxSupportedSides * 8 * 10; // 2560
	static constexpr uint32_t LegacyEntryCapacity = 512;

	char Magic[8]; // "PHOBOSBP"
	uint32_t SchemaVersion;
	uint64_t BaseSignature;
	uint64_t BackpackHash;
	uint32_t GamesPlayed;
	uint32_t WinsCount;
	int32_t EstimatedELO;
	int8_t LoRA_A[256 * 16];
	int8_t LoRA_B[16 * 128];
	int16_t OutputBias[128];
	uint32_t NumEntries;
	AIBackpackEntry Entries[EntryCapacity];
};

struct AITrainerSharedTelemetry
{
	char Magic[8]; // "PHOBOSTM"
	uint32_t SchemaVersion;
	uint32_t SlotIndex;
	uint32_t ProcessId;
	uint32_t CurrentFrame;
	uint32_t FramesPerSecond;
	uint32_t MatchFinished;
	int32_t WinnerHouseIndex;
	uint32_t StalemateTriggered;
	uint64_t RulesSignature;
	uint32_t NumHouses;
	struct HouseSnapshot
	{
		int32_t ArrayIndex;
		int32_t SideIndex;
		int32_t CountryIndex;
		int32_t Archetype;
		int32_t ActiveExpert;
		int32_t Credits;
		int32_t KilledUnits;
		int32_t KilledBuildings;
		int32_t OwnedUnits;
		int32_t OwnedBuildings;
		int32_t OwnedInfantry;
		int32_t OwnedAircraft;
		int32_t NetWorthScore;
		uint8_t IsWinner;
		uint8_t IsDefeated;
		uint8_t ColorR;
		uint8_t ColorG;
		uint8_t ColorB;
		uint8_t PolicySlot;
		uint8_t Padding[2];
	} Houses[8];
	uint8_t RadarCells[128 * 128];
};

struct AITrajectoryPick
{
	uint8_t Queue;
	uint8_t Flags;
	int8_t WinnerSlot;
	int8_t RunnerSlot;
	int16_t Margin;
	uint8_t Expert;
	uint8_t Reserved;
};

enum TrajPickFlag : uint8_t
{
	NoClass = 1,
	Overridden = 2,
	SingleClass = 4,
	WinnerFree = 8,
	RunnerFree = 16
};

struct AITrajectoryStep
{
	uint32_t Frame;
	uint8_t HouseIndex;
	uint8_t PolicySlot;
	uint8_t ActiveExpert;
	uint8_t Archetype;
	int32_t OwnNetWorth;
	int32_t KnownEnemyWorth;
	int32_t KillsDelta;
	int32_t LossesDelta;
	int16_t DeploysGoodDelta;
	int16_t DeploysBadDelta;
	int16_t DropsGoodDelta;
	int16_t DropsBadDelta;
	int16_t FinisherDelta;
	int16_t CounterScore;
	uint64_t OpenGatesMask;
	int8_t Features[256];
	int8_t TopActionRanks[8];
	uint16_t WindowFrames;
	uint8_t WindowTicks;
	uint8_t ExpertOffTicks;
	uint16_t ExpertPoolMask;
	uint8_t BestExpert;
	uint8_t RoutingFlags;
	uint8_t PickCount;
	uint8_t PicksDropped;
	uint16_t Reserved;
	AITrajectoryPick Picks[8];
	uint8_t GateDraws[128];
	uint8_t GateOpens[128];
	uint8_t GateFreeDraws[128];
	int16_t GateScoreFn[128];
};

struct AITrajectorySharedBuffer
{
	char Magic[8]; // "PHOBOSTJ"
	uint32_t SchemaVersion;
	uint32_t MatchSerial;
	uint32_t WriteCount;
	uint32_t Capacity;

	// A whole match: the trainer reads the buffer once, when the match ends, so a ring shorter than
	// the match keeps its end only (with 200 steps 19 of 20 recorded matches lost their opening).
	// At most 90000 frames of 240 per step and eight houses: 3000 steps.
	static constexpr uint32_t StepCapacity = 4096u;

	AITrajectoryStep Steps[StepCapacity];
};

#pragma pack(pop)

static_assert(sizeof(AITrajectoryPick) == 8);
static_assert(sizeof(AITrajectoryStep) == 1024);
static_assert(offsetof(AITrajectoryStep, WindowFrames) == 308);
static_assert(offsetof(AITrajectoryStep, Picks) == 320);
static_assert(offsetof(AITrajectoryStep, GateDraws) == 384);
static_assert(offsetof(AITrajectoryStep, GateScoreFn) == 768);
static_assert(sizeof(AITrajectorySharedBuffer) == 24 + 4096 * 1024);
static_assert(sizeof(AIBaseHeader) == 984, "AIBaseHeader must remain exactly 984 bytes");
static_assert(sizeof(AIBackpackEntry) == 72, "AIBackpackEntry must remain 72 bytes");
static_assert(sizeof(AIBackpackFile) == 6444 + AIBackpackFile::EntryCapacity * sizeof(AIBackpackEntry),
	"AIBackpackFile size mismatch");

inline uint32_t NeuralBackpackEntryIndex(int sideIndex, int archetype, int expert) noexcept
{
	const uint32_t s = static_cast<uint32_t>(sideIndex < 0 ? 0 : (sideIndex >= static_cast<int>(AIBackpackFile::MaxSupportedSides)
		? static_cast<int>(AIBackpackFile::MaxSupportedSides) - 1
		: sideIndex));
	const uint32_t a = static_cast<uint32_t>(archetype < 0 ? 0 : (archetype >= 8 ? 7 : archetype));
	const uint32_t e = static_cast<uint32_t>(expert < 0 ? 0 : (expert >= 10 ? 9 : expert));
	return s * 80u + a * 10u + e;
}

// The factory a Neural AI house builds a queue at (src/Ext/Building/Hooks.Production.cpp).
namespace NeuralFactoryQueue
{
	BuildingClass* FindDesignated(HouseClass* pOwner, AbstractType kind, bool naval);
}

class NeuralAI
{
public:
	static constexpr int InputVectorSize = 256;
	static constexpr int OutputVectorSize = 128;
	static constexpr int NumSpecialists = 10;
	static constexpr int NumArchetypes = 8;

	static bool HeadlessMode;
	static int TrainSlotIndex;
	// Started by the trainer (-TrainSlot=, PHOBOS_TRAIN_SLOT): enables expert exploration.
	static bool TrainingMode;
	static int MatchEndWinnerHouse;
	static bool MatchEndStalemate;

	static void ParseCommandLineArg(const char* pArg);
	static void Initialize();
	static void Shutdown();

	static bool IsEnabledForHouse(const HouseClass* pHouse);
	static bool IsCampaignRestricted(const HouseClass* pHouse);

	static void OnScenarioStart();
	static void OnScenarioEnd(bool playerWon);
	static void OnHeadlessFrameTick();
	static void OnAutoBaseBuildingTriggered(HouseClass* pHouse, bool enabled);
	static void OnBuildingDamaged(BuildingClass* pBuilding, TechnoClass* pAttacker, HouseClass* pAttackerHouse);
	static void OnBuildingCaptured(BuildingClass* pBuilding, HouseClass* pOldOwner, HouseClass* pNewOwner);
	static void OnBuildingInfiltrated(TechnoClass* pInfiltrator, BuildingClass* pBuilding);
	static void OnTechnoKilled(TechnoClass* pVictim, TechnoClass* pKiller);
	static void OnHarvestIncome(HouseClass* pHouse, TechnoClass* pGatherer, TechnoClass* pDock, int money);
	static void OnHouseDamaged(HouseClass* pVictim, HouseClass* pAttacker, int damage, bool aimed);
	static void OnMapReshroud(HouseClass* pHouse);
	static void OnRevealWarhead(HouseClass* pHouse, const CoordStruct& coords, int radius);
	static void OnSuperWeaponLaunched(SuperClass* pSuper, const CellStruct& cell);
	static void OnPathFailure(FootClass* pFoot, const CellStruct& from, const CellStruct& to);
	static void OnHierarchicalPathFailure(FootClass* pFoot, const CellStruct& from, const CellStruct& to);
	static bool ShouldSkipPathSearch(const FootClass* pFoot, const CellStruct* pGoal);
	static void OnTechnoDestroyed(TechnoClass* pVictim, TechnoClass* pKiller);

	static int UpdateExpertAI(HouseClass* pHouse);
	static void UpdateBuildingProduction(HouseClass* pHouse);
	static void UpdateUnitProduction(HouseClass* pHouse);
	static void UpdateInfantryProduction(HouseClass* pHouse);
	static void UpdateAircraftProduction(HouseClass* pHouse);
	static void RefreshProductionIndices(HouseClass* pHouse);
	static void OnTechnoProduced(HouseClass* pHouse, TechnoClass* pTechno);

	static CellStruct FindSpatialTetrisBuildCell(HouseClass* pHouse, BuildingTypeClass* pBuildingType);
	static int PlaceCompletedBuilding(BuildingClass* pFactory, BuildingClass* pBuilding, CellStruct& outCell);

private:
	static HANDLE BaseFileHandle;
	static HANDLE BaseMappingHandle;
	static const uint8_t* BaseMappedView;
	static size_t BaseMappedSize;

	static HANDLE TelemetryShmHandle;
	static AITrainerSharedTelemetry* TelemetryView;

	static AIBackpackFile Backpack;
	static bool BackpackLoaded;
	static AIBackpackFile OpponentBackpack;
	static bool OpponentLoaded;
	static bool AllowBackpackInference;
	static uint64_t CurrentRulesSignature;
	static uint64_t CurrentBaseSignature;

	static int LastCombatFrame;
	static int LastTotalKillsAndCaptures;
	static DWORD LastFPSCheckTick;
	static int FramesSinceLastFPSCheck;
	static int CurrentMeasuredFPS;

	static uint64_t ComputeRulesSignature();
	static void EnsureBaseModelLoaded();
	static void LoadOrResetBackpack();
	static void SaveBackpack();
	static bool LoadBackpackFile(const char* pPath, AIBackpackFile& outBackpack, bool requireSignature);
	static const AIBackpackFile& PolicyFor(const HouseClass* pHouse);
	static void InitTelemetrySharedMemory();
	static void UpdateTelemetrySnapshot(bool matchEnded, int winnerHouseIdx, bool stalemate);

	static void ExtractFogFilteredFeatures(HouseClass* pHouse, std::array<int8_t, InputVectorSize>& features);
	static void RunDeterministicMoE(HouseClass* pHouse, const std::array<int8_t, InputVectorSize>& features,
		std::array<int16_t, OutputVectorSize>& outScores);

	static void ExecuteTacticalAndSpecOpsMicro(HouseClass* pHouse, const std::array<int16_t, OutputVectorSize>& scores);

	static bool IsCellClearedOfExitCorridors(HouseClass* pHouse, BuildingTypeClass* pType, CellStruct candidateCell);
};


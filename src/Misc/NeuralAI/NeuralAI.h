#pragma once

#include <Windows.h>
#include <YRPP.h>
#include <array>
#include <cstdint>
#include <vector>

class HouseClass;
class BuildingClass;
class BuildingTypeClass;
class TechnoClass;
class TechnoTypeClass;

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
	uint8_t ReservedHeader[376];
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
	AIBackpackEntry Entries[512];
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

#pragma pack(pop)

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
	static void OnTechnoKilled(TechnoClass* pVictim, TechnoClass* pKiller);
	static void OnHarvestIncome(HouseClass* pHouse, TechnoClass* pGatherer, TechnoClass* pDock, int money);
	static void OnHouseDamaged(HouseClass* pVictim, HouseClass* pAttacker, int damage, bool aimed);
	static void OnPathFailure(FootClass* pFoot, const CellStruct& from, const CellStruct& to);
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


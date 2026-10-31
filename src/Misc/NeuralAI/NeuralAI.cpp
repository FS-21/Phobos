#include "NeuralAI.h"

#include <BeaconManagerClass.h>
#include <BuildingClass.h>
#include <BuildingTypeClass.h>
#include <CellClass.h>
#include <FactoryClass.h>
#include <FootClass.h>
#include <GameModeOptionsClass.h>
#include <HouseClass.h>
#include <InfantryClass.h>
#include <InfantryTypeClass.h>
#include <MapClass.h>
#include <MessageListClass.h>
#include <ColorScheme.h>
#include <RulesClass.h>
#include <ScenarioClass.h>
#include <SessionClass.h>
#include <SuperClass.h>
#include <UnitClass.h>
#include <UnitTypeClass.h>
#include <AircraftClass.h>
#include <AircraftTypeClass.h>
#include <TechnoClass.h>

#include <Ext/BuildingType/Body.h>
#include <Ext/House/Body.h>
#include <Ext/Rules/Body.h>
#include <Ext/SWType/Body.h>
#include <Ext/Techno/Body.h>
#include <Ext/TechnoType/Body.h>
#include <Utilities/Debug.h>
#include <Utilities/GeneralUtils.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>

bool NeuralAI::HeadlessMode = false;
int NeuralAI::TrainSlotIndex = 0;
int NeuralAI::MatchEndWinnerHouse = -1;
bool NeuralAI::MatchEndStalemate = false;

HANDLE NeuralAI::BaseFileHandle = INVALID_HANDLE_VALUE;
HANDLE NeuralAI::BaseMappingHandle = NULL;
const uint8_t* NeuralAI::BaseMappedView = nullptr;
size_t NeuralAI::BaseMappedSize = 0;

HANDLE NeuralAI::TelemetryShmHandle = NULL;
AITrainerSharedTelemetry* NeuralAI::TelemetryView = nullptr;

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

namespace
{
	constexpr uint64_t FnvOffsetBasis = 14695981039346656037ULL;
	constexpr uint64_t FnvPrime = 1099511628211ULL;

	inline uint64_t HashBytesFNV1a(const void* data, size_t len, uint64_t hash = FnvOffsetBasis)
	{
		const uint8_t* pBytes = static_cast<const uint8_t*>(data);

		for (size_t i = 0; i < len; ++i)
		{
			hash ^= static_cast<uint64_t>(pBytes[i]);
			hash *= FnvPrime;
		}

		return hash;
	}

	template <typename T>
	inline uint64_t HashValueFNV1a(const T& val, uint64_t hash)
	{
		return HashBytesFNV1a(&val, sizeof(T), hash);
	}

	inline int8_t ClampInt8(int val)
	{
		if (val < -127)
			return -127;

		if (val > 127)
			return 127;

		return static_cast<int8_t>(val);
	}

	inline int16_t ClampInt16(int val)
	{
		if (val < -32000)
			return -32000;

		if (val > 32000)
			return 32000;

		return static_cast<int16_t>(val);
	}

	// Per-house vision map built from the sight of the AI and its allies. Replaces CellClass::IsShrouded, which
	// reflects the local player's shroud and would desync multiplayer and leak the human's exploration to the AI.
	constexpr int VisionMapSide = 512;
	std::vector<uint8_t> VisionMap;
	int VisionMapFrame = -1;
	int VisionMapOwner = -1;

	void RebuildVisionMap(HouseClass* pHouse)
	{
		VisionMap.assign(static_cast<size_t>(VisionMapSide) * VisionMapSide, 0);
		VisionMapFrame = Unsorted::CurrentFrame;
		VisionMapOwner = pHouse->ArrayIndex;

		for (auto const pTechno : TechnoClass::Array)
		{
			if (!pTechno || pTechno->InLimbo || !pTechno->IsAlive || !pTechno->Owner)
				continue;

			if (pTechno->Owner != pHouse && !pHouse->IsAlliedWith(pTechno->Owner))
				continue;

			auto const pType = pTechno->GetTechnoType();
			if (!pType)
				continue;

			const CellStruct coords = pTechno->GetMapCoords();
			const int radius = std::clamp(static_cast<int>(pType->Sight), 1, 24);

			for (int dy = -radius; dy <= radius; ++dy)
			{
				const int y = coords.Y + dy;
				if (y < 0 || y >= VisionMapSide)
					continue;

				for (int dx = -radius; dx <= radius; ++dx)
				{
					const int x = coords.X + dx;
					if (x < 0 || x >= VisionMapSide || dx * dx + dy * dy > radius * radius)
						continue;

					VisionMap[static_cast<size_t>(y) * VisionMapSide + x] = 1;
				}
			}
		}
	}

	bool IsCellVisibleToAI(HouseClass* pHouse, CellClass* pCell)
	{
		if (!pHouse || !pCell)
			return false;

		if (VisionMapFrame != Unsorted::CurrentFrame || VisionMapOwner != pHouse->ArrayIndex)
			RebuildVisionMap(pHouse);

		const CellStruct coords = pCell->MapCoords;
		if (coords.X < 0 || coords.X >= VisionMapSide || coords.Y < 0 || coords.Y >= VisionMapSide)
			return false;

		return VisionMap[static_cast<size_t>(coords.Y) * VisionMapSide + coords.X] != 0;
	}
}

void NeuralAI::ParseCommandLineArg(const char* pArg)
{
	if (!pArg)
		return;

	if (_stricmp(pArg, "-TRAIN_HEADLESS") == 0 || _stricmp(pArg, "-TrainHeadless") == 0)
	{
		NeuralAI::HeadlessMode = true;
		Debug::Log("[NeuralAI] Headless Turbo Training Mode enabled (-TRAIN_HEADLESS).\n");
	}
	else if (_strnicmp(pArg, "-TrainSlot=", 11) == 0)
	{
		NeuralAI::TrainSlotIndex = std::clamp(std::atoi(pArg + 11), 0, 63);
		Debug::Log("[NeuralAI] Training Slot Index set to %d.\n", NeuralAI::TrainSlotIndex);
	}
}

void NeuralAI::Initialize()
{
	CreateDirectoryA("AI", NULL);
	NeuralAI::EnsureBaseModelLoaded();

	if (NeuralAI::HeadlessMode)
		NeuralAI::InitTelemetrySharedMemory();
}

void NeuralAI::Shutdown()
{
	if (NeuralAI::TelemetryView)
	{
		UnmapViewOfFile(NeuralAI::TelemetryView);
		NeuralAI::TelemetryView = nullptr;
	}

	if (NeuralAI::TelemetryShmHandle)
	{
		CloseHandle(NeuralAI::TelemetryShmHandle);
		NeuralAI::TelemetryShmHandle = NULL;
	}

	if (NeuralAI::BaseMappedView)
	{
		UnmapViewOfFile(NeuralAI::BaseMappedView);
		NeuralAI::BaseMappedView = nullptr;
	}

	if (NeuralAI::BaseMappingHandle)
	{
		CloseHandle(NeuralAI::BaseMappingHandle);
		NeuralAI::BaseMappingHandle = NULL;
	}

	if (NeuralAI::BaseFileHandle != INVALID_HANDLE_VALUE)
	{
		CloseHandle(NeuralAI::BaseFileHandle);
		NeuralAI::BaseFileHandle = INVALID_HANDLE_VALUE;
	}
}

bool NeuralAI::IsEnabledForHouse(const HouseClass* pHouse)
{
	if (!pHouse || pHouse->IsControlledByHuman() || pHouse->IsNeutral() || pHouse->IsObserver() || pHouse->Defeated)
		return false;

	if (pHouse->Type && pHouse->Type->MultiplayPassive)
		return false;

	if (auto const pRulesExt = RulesExt::Global())
	{
		if (!pRulesExt->AIBrain)
			return false;
	}

	return true;
}

bool NeuralAI::IsCampaignRestricted(const HouseClass* pHouse)
{
	if (!SessionClass::IsCampaign())
		return false;

	if (!pHouse)
		return true;

	if (pHouse->AutoBaseBuilding)
		return false;

	if (auto const pHouseExt = HouseExt::TryFetch(pHouse))
	{
		if (pHouseExt->AIBrain_Campaign)
			return false;
	}

	return true;
}

uint64_t NeuralAI::ComputeRulesSignature()
{
	uint64_t hash = FnvOffsetBasis;
	auto const pRules = RulesClass::Instance;

	if (!pRules)
		return hash;

	hash = HashValueFNV1a(pRules->HarvestersPerRefinery[0], hash);
	hash = HashValueFNV1a(pRules->AISlaveMinerNumber[0], hash);
	hash = HashValueFNV1a(pRules->PowerSurplus, hash);

	for (auto const pBld : BuildingTypeClass::Array)
	{
		if (!pBld)
			continue;

		hash = HashValueFNV1a(pBld->Cost, hash);
		hash = HashValueFNV1a(pBld->Strength, hash);
		hash = HashValueFNV1a(pBld->TechLevel, hash);
		hash = HashValueFNV1a(pBld->PowerBonus, hash);
		hash = HashValueFNV1a(pBld->PowerDrain, hash);
		hash = HashValueFNV1a(pBld->GetOwners(), hash);
	}

	for (auto const pUnit : UnitTypeClass::Array)
	{
		if (!pUnit)
			continue;

		hash = HashValueFNV1a(pUnit->Cost, hash);
		hash = HashValueFNV1a(pUnit->Strength, hash);
		hash = HashValueFNV1a(pUnit->Speed, hash);
		hash = HashValueFNV1a(pUnit->TechLevel, hash);
		hash = HashValueFNV1a(pUnit->GetOwners(), hash);
	}

	for (auto const pInf : InfantryTypeClass::Array)
	{
		if (!pInf)
			continue;

		hash = HashValueFNV1a(pInf->Cost, hash);
		hash = HashValueFNV1a(pInf->Strength, hash);
		hash = HashValueFNV1a(pInf->TechLevel, hash);
		hash = HashValueFNV1a(pInf->GetOwners(), hash);
	}

	for (auto const pAir : AircraftTypeClass::Array)
	{
		if (!pAir)
			continue;

		hash = HashValueFNV1a(pAir->Cost, hash);
		hash = HashValueFNV1a(pAir->Strength, hash);
		hash = HashValueFNV1a(pAir->TechLevel, hash);
		hash = HashValueFNV1a(pAir->GetOwners(), hash);
	}

	return hash;
}

void NeuralAI::EnsureBaseModelLoaded()
{
	if (NeuralAI::BaseMappedView)
		return;

	CreateDirectoryA("AI", NULL);
	CreateDirectoryA("Resources\\AI", NULL);
	const char* basePath = (GetFileAttributesA("Resources\\AI\\ai_base.bin") != INVALID_FILE_ATTRIBUTES)
		? "Resources\\AI\\ai_base.bin"
		: "AI\\ai_base.bin";

	DWORD attrs = GetFileAttributesA(basePath);
	if (attrs == INVALID_FILE_ATTRIBUTES)
	{
		// Bootstrap a deterministic compact INT8 MoE base brain if none exists yet.
		// Phobos_AI_Trainer can expand or replace this with the full 250 MB league brain.
		constexpr uint32_t expertBlockSize = NeuralAI::InputVectorSize * NeuralAI::OutputVectorSize;
		constexpr uint32_t totalBootstrapSize = sizeof(AIBaseHeader) + (expertBlockSize * NeuralAI::NumSpecialists);

		std::vector<uint8_t> buffer(totalBootstrapSize, 0);
		auto const pHdr = reinterpret_cast<AIBaseHeader*>(buffer.data());
		std::memcpy(pHdr->Magic, "PHOBOSAI", 8);
		pHdr->SchemaVersion = 1;
		pHdr->TotalSizeBytes = totalBootstrapSize;
		pHdr->NumExperts = NeuralAI::NumSpecialists;
		pHdr->NumArchetypes = NeuralAI::NumArchetypes;
		pHdr->InputDim = 192;
		pHdr->ReservedInputDim = 64;
		pHdr->HiddenDim1 = 256;
		pHdr->HiddenDim2 = 128;
		pHdr->OutputDim = 48;
		pHdr->ReservedOutputDim = 16;

		for (int i = 0; i < 256; ++i)
		{
			const int x = i - 128;
			const double norm = static_cast<double>(x) / 32.0;
			const double sig = 1.0 / (1.0 + std::exp(-norm));
			const double tnh = std::tanh(norm);
			pHdr->SigmoidLUT[i] = ClampInt8(static_cast<int>(sig * 127.0));
			pHdr->TanhLUT[i] = ClampInt8(static_cast<int>(tnh * 127.0));
		}

		uint32_t offset = sizeof(AIBaseHeader);
		uint64_t seed = 0x50484F424F534149ULL;

		for (int e = 0; e < NeuralAI::NumSpecialists; ++e)
		{
			pHdr->ExpertWeightsOffset[e] = offset;
			int8_t* w = reinterpret_cast<int8_t*>(buffer.data() + offset);

			for (uint32_t idx = 0; idx < expertBlockSize; ++idx)
			{
				seed ^= seed >> 12;
				seed ^= seed << 25;
				seed ^= seed >> 27;
				const int32_t val = static_cast<int32_t>((seed * 0x2545F4914F6CDD1DULL) >> 58) - 32;
				w[idx] = ClampInt8(val);
			}

			offset += expertBlockSize;
		}

		pHdr->BaseModelHash = HashBytesFNV1a(
			buffer.data() + sizeof(AIBaseHeader),
			totalBootstrapSize - sizeof(AIBaseHeader));

		HANDLE hOut = CreateFileA(basePath, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
		if (hOut != INVALID_HANDLE_VALUE)
		{
			DWORD written = 0;
			WriteFile(hOut, buffer.data(), totalBootstrapSize, &written, NULL);
			CloseHandle(hOut);
		}
	}

	NeuralAI::BaseFileHandle = CreateFileA(
		basePath,
		GENERIC_READ,
		FILE_SHARE_READ | FILE_SHARE_WRITE,
		NULL,
		OPEN_EXISTING,
		FILE_ATTRIBUTE_NORMAL,
		NULL);

	if (NeuralAI::BaseFileHandle == INVALID_HANDLE_VALUE)
		return;

	LARGE_INTEGER fileSize = {};
	if (!GetFileSizeEx(NeuralAI::BaseFileHandle, &fileSize) || fileSize.QuadPart < static_cast<LONGLONG>(sizeof(AIBaseHeader)))
	{
		CloseHandle(NeuralAI::BaseFileHandle);
		NeuralAI::BaseFileHandle = INVALID_HANDLE_VALUE;
		return;
	}

	NeuralAI::BaseMappedSize = static_cast<size_t>(fileSize.QuadPart);
	NeuralAI::BaseMappingHandle = CreateFileMappingA(
		NeuralAI::BaseFileHandle,
		NULL,
		PAGE_READONLY,
		0,
		0,
		"Local\\PhobosAIBaseSharedMapping");

	if (!NeuralAI::BaseMappingHandle)
	{
		NeuralAI::BaseMappingHandle = CreateFileMappingA(
			NeuralAI::BaseFileHandle,
			NULL,
			PAGE_READONLY,
			0,
			0,
			NULL);
	}

	if (NeuralAI::BaseMappingHandle)
	{
		NeuralAI::BaseMappedView = static_cast<const uint8_t*>(
			MapViewOfFile(NeuralAI::BaseMappingHandle, FILE_MAP_READ, 0, 0, 0));
	}
}

bool NeuralAI::LoadBackpackFile(const char* pPath, AIBackpackFile& outBackpack, bool requireSignature)
{
	HANDLE hFile = CreateFileA(pPath, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	if (hFile == INVALID_HANDLE_VALUE)
		return false;

	AIBackpackFile loaded = {};
	DWORD bytesRead = 0;

	const bool valid = ReadFile(hFile, &loaded, sizeof(AIBackpackFile), &bytesRead, NULL)
		&& bytesRead == sizeof(AIBackpackFile)
		&& std::memcmp(loaded.Magic, "PHOBOSBP", 8) == 0
		&& loaded.SchemaVersion == 2
		&& (!requireSignature || loaded.BaseSignature == NeuralAI::CurrentBaseSignature);

	CloseHandle(hFile);

	if (valid)
		outBackpack = loaded;

	return valid;
}

const AIBackpackFile& NeuralAI::PolicyFor(const HouseClass* pHouse)
{
	if (NeuralAI::HeadlessMode && NeuralAI::OpponentLoaded)
	{
		auto const pExt = HouseExt::TryFetch(pHouse);

		if (pExt && pExt->AIBrain_PolicySlot == 1)
			return NeuralAI::OpponentBackpack;
	}

	return NeuralAI::Backpack;
}

void NeuralAI::LoadOrResetBackpack()
{
	NeuralAI::CurrentRulesSignature = NeuralAI::ComputeRulesSignature();
	uint64_t baseModelHash = 0;

	if (NeuralAI::BaseMappedView && NeuralAI::BaseMappedSize >= sizeof(AIBaseHeader))
	{
		auto const pHdr = reinterpret_cast<const AIBaseHeader*>(NeuralAI::BaseMappedView);
		baseModelHash = pHdr->BaseModelHash;
	}

	NeuralAI::CurrentBaseSignature = HashValueFNV1a(baseModelHash, NeuralAI::CurrentRulesSignature);

	// Campaign and standard Multiplayer use only the shared deterministic base brain.
	if (SessionClass::IsCampaign())
	{
		NeuralAI::AllowBackpackInference = false;
		return;
	}

	if (SessionClass::IsMultiplayer() && !NeuralAI::HeadlessMode)
	{
		NeuralAI::AllowBackpackInference = false;
		return;
	}

	// In headless training the trainer owns both policy files: they are loaded verbatim and never written back
	if (NeuralAI::HeadlessMode)
	{
		char policyPath[MAX_PATH];

		std::memset(&NeuralAI::Backpack, 0, sizeof(AIBackpackFile));
		std::memcpy(NeuralAI::Backpack.Magic, "PHOBOSBP", 8);
		NeuralAI::Backpack.SchemaVersion = 2;
		NeuralAI::Backpack.BaseSignature = NeuralAI::CurrentBaseSignature;
		NeuralAI::Backpack.EstimatedELO = 1200;
		std::snprintf(policyPath, sizeof(policyPath), "Resources\\AI\\ai_backpack_slot_%d.dat", NeuralAI::TrainSlotIndex);

		const bool challengerLoaded = NeuralAI::LoadBackpackFile(policyPath, NeuralAI::Backpack, false);

		std::memcpy(&NeuralAI::OpponentBackpack, &NeuralAI::Backpack, sizeof(AIBackpackFile));
		std::snprintf(policyPath, sizeof(policyPath), "Resources\\AI\\ai_backpack_opp_slot_%d.dat", NeuralAI::TrainSlotIndex);
		NeuralAI::OpponentLoaded = NeuralAI::LoadBackpackFile(policyPath, NeuralAI::OpponentBackpack, false);

		NeuralAI::AllowBackpackInference = true;
		NeuralAI::BackpackLoaded = true;
		Debug::Log("[NeuralAI] Headless policies: challenger=%s opponent=%s\n", challengerLoaded ? "loaded" : "default", NeuralAI::OpponentLoaded ? "loaded" : "shared");

		return;
	}

	NeuralAI::AllowBackpackInference = true;
	const char* bpPath = (GetFileAttributesA("Resources\\AI\\ai_backpack.dat") != INVALID_FILE_ATTRIBUTES)
		? "Resources\\AI\\ai_backpack.dat"
		: "AI\\ai_backpack.dat";

	const bool valid = NeuralAI::LoadBackpackFile(bpPath, NeuralAI::Backpack, true);

	if (!valid)
	{
		std::memset(&NeuralAI::Backpack, 0, sizeof(AIBackpackFile));
		std::memcpy(NeuralAI::Backpack.Magic, "PHOBOSBP", 8);
		NeuralAI::Backpack.SchemaVersion = 2;
		NeuralAI::Backpack.BaseSignature = NeuralAI::CurrentBaseSignature;
		NeuralAI::Backpack.EstimatedELO = 1200;
		NeuralAI::Backpack.BackpackHash = HashBytesFNV1a(&NeuralAI::Backpack.LoRA_A[0], sizeof(NeuralAI::Backpack.LoRA_A));
		NeuralAI::SaveBackpack();
		Debug::Log("[NeuralAI] Backpack initialized/reset for BaseSignature %llX.\n", NeuralAI::CurrentBaseSignature);
	}

	NeuralAI::BackpackLoaded = true;
}

void NeuralAI::SaveBackpack()
{
	if (SessionClass::IsCampaign() || !NeuralAI::AllowBackpackInference)
		return;

	CreateDirectoryA("AI", NULL);
	CreateDirectoryA("Resources\\AI", NULL);
	NeuralAI::Backpack.BackpackHash = HashBytesFNV1a(
		&NeuralAI::Backpack.LoRA_A[0],
		sizeof(NeuralAI::Backpack.LoRA_A) + sizeof(NeuralAI::Backpack.LoRA_B));

	char bpPath[MAX_PATH];
	if (NeuralAI::HeadlessMode && NeuralAI::TrainSlotIndex > 0)
		std::snprintf(bpPath, sizeof(bpPath), "Resources\\AI\\ai_backpack_slot_%d.dat", NeuralAI::TrainSlotIndex);
	else
		std::snprintf(bpPath, sizeof(bpPath), "Resources\\AI\\ai_backpack.dat");

	HANDLE hFile = CreateFileA(bpPath, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	if (hFile != INVALID_HANDLE_VALUE)
	{
		DWORD written = 0;
		WriteFile(hFile, &NeuralAI::Backpack, sizeof(AIBackpackFile), &written, NULL);
		CloseHandle(hFile);
	}

	if (!NeuralAI::HeadlessMode || NeuralAI::TrainSlotIndex == 0)
	{
		HANDLE hFileAlt = CreateFileA("AI\\ai_backpack.dat", GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
		if (hFileAlt != INVALID_HANDLE_VALUE)
		{
			DWORD written = 0;
			WriteFile(hFileAlt, &NeuralAI::Backpack, sizeof(AIBackpackFile), &written, NULL);
			CloseHandle(hFileAlt);
		}
	}
}

void NeuralAI::InitTelemetrySharedMemory()
{
	if (NeuralAI::TelemetryView)
		return;

	char shmName[128];
	std::snprintf(shmName, sizeof(shmName), "Local\\PhobosAITrainerShm_%d", NeuralAI::TrainSlotIndex);

	NeuralAI::TelemetryShmHandle = CreateFileMappingA(
		INVALID_HANDLE_VALUE,
		NULL,
		PAGE_READWRITE,
		0,
		sizeof(AITrainerSharedTelemetry),
		shmName);

	if (NeuralAI::TelemetryShmHandle)
	{
		NeuralAI::TelemetryView = static_cast<AITrainerSharedTelemetry*>(
			MapViewOfFile(NeuralAI::TelemetryShmHandle, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(AITrainerSharedTelemetry)));

		if (NeuralAI::TelemetryView)
		{
			std::memset(NeuralAI::TelemetryView, 0, sizeof(AITrainerSharedTelemetry));
			std::memcpy(NeuralAI::TelemetryView->Magic, "PHOBOSTM", 8);
			NeuralAI::TelemetryView->SchemaVersion = 1;
			NeuralAI::TelemetryView->SlotIndex = static_cast<uint32_t>(NeuralAI::TrainSlotIndex);
			NeuralAI::TelemetryView->ProcessId = GetCurrentProcessId();
		}
	}
}

void NeuralAI::OnScenarioStart()
{
	NeuralAI::Initialize();
	NeuralAI::LoadOrResetBackpack();

	NeuralAI::LastCombatFrame = 0;
	NeuralAI::LastTotalKillsAndCaptures = 0;
	NeuralAI::LastFPSCheckTick = GetTickCount();
	NeuralAI::FramesSinceLastFPSCheck = 0;

	static const wchar_t* const ArchetypeNamesW[5] = {
		L"Balanced Grandmaster",
		L"SpecOps & Infiltration",
		L"Tech & Fortress Turtle",
		L"Blitzkrieg Rush",
		L"Air & Artillery Siege"
	};

	int activeAIHouses = 0;

	for (auto const pHouse : HouseClass::Array)
	{
		if (!NeuralAI::IsEnabledForHouse(pHouse))
			continue;

		activeAIHouses++;
		auto const pExt = HouseExt::Fetch(pHouse);
		pExt->AIBrain_Archetype = ScenarioClass::Instance->Random.RandomRanged(0, NeuralAI::NumArchetypes - 1);
		pExt->AIBrain_PolicySlot = NeuralAI::HeadlessMode ? ((activeAIHouses - 1) % 2) : 0;
		pExt->AIBrain_ActiveExpert = 0;
		pExt->AIBrain_LastInferenceFrame = -1000;
		pExt->AIBrain_NextBuildingTarget = -1;
		pExt->AIBrain_NextUnitTarget = -1;
		pExt->AIBrain_NextNavalTarget = -1;
		pExt->AIBrain_NextInfantryTarget = -1;
		pExt->AIBrain_NextAircraftTarget = -1;
		pExt->AIBrain_PlannedBuildCell = CellStruct::Empty;
		pExt->AIBrain_ScoutTargetCell = CellStruct::Empty;
		pExt->AIBrain_AttackTargetCell = CellStruct::Empty;
		pExt->AIBrain_InfiltratedEnemyMask = 0;
		pExt->AIBrain_ScoutedEnemyMask = 0;
		pExt->AIBrain_EstimatedEnemyThreat.fill(0);

		const char* countryId = (pHouse->Type && pHouse->Type->ID[0]) ? pHouse->Type->ID : "AIHouse";
		const int archIdx = std::clamp(pExt->AIBrain_Archetype, 0, 4);
		Debug::Log(
			"[NeuralAI] Activated for House #%d (%s, Diff=%d) -> Archetype=%d (%ls)\n",
			pHouse->ArrayIndex, countryId, pHouse->GetAIDifficultyIndex(), archIdx, ArchetypeNamesW[archIdx]);

		if (!NeuralAI::HeadlessMode)
		{
			wchar_t msgBuf[256];
			std::swprintf(
				msgBuf, 256,
				L"[Neural AI] House #%d (%hs) -> Style: %ls",
				pHouse->ArrayIndex, countryId, ArchetypeNamesW[archIdx]);
			MessageListClass::Instance.PrintMessage(msgBuf, RulesClass::Instance->MessageDelay, HouseClass::CurrentPlayer ? HouseClass::CurrentPlayer->ColorSchemeIndex : 0, true);
		}
	}

	if (activeAIHouses > 0)
	{
		Debug::Log(
			"[NeuralAI] ScenarioStart: %d AI Houses controlled by NeuralAI (aimd.ini bypassed) | BaseSignature=%016llX | ELO=%d\n",
			activeAIHouses, NeuralAI::CurrentBaseSignature, NeuralAI::Backpack.EstimatedELO);

		if (!NeuralAI::HeadlessMode)
		{
			wchar_t bannerBuf[256];
			std::swprintf(
				bannerBuf, 256,
				L"[Phobos Neural AI] ACTIVE (%d AI Houses | Brain Sig:%08X | ELO:%d | aimd.ini bypassed)",
				activeAIHouses, static_cast<uint32_t>(NeuralAI::CurrentBaseSignature & 0xFFFFFFFFULL), NeuralAI::Backpack.EstimatedELO);
			MessageListClass::Instance.PrintMessage(bannerBuf, RulesClass::Instance->MessageDelay, HouseClass::CurrentPlayer ? HouseClass::CurrentPlayer->ColorSchemeIndex : 0, true);
		}
	}

	if (NeuralAI::HeadlessMode)
		NeuralAI::UpdateTelemetrySnapshot(false, -1, false);
}

void NeuralAI::OnScenarioEnd(bool playerWon)
{
	if (SessionClass::IsCampaign())
		return;

	int bestHouseIdx = -1;
	int bestScore = -1;

	for (auto const pHouse : HouseClass::Array)
	{
		if (!pHouse || pHouse->IsNeutral() || pHouse->IsObserver())
			continue;

		const int score = (pHouse->TotalKilledUnits * 200)
			+ (pHouse->TotalKilledBuildings * 450)
			+ (pHouse->OwnedBuildings * 300)
			+ (pHouse->OwnedUnits * 200);

		if (pHouse->IsWinner || (!pHouse->Defeated && score > bestScore))
		{
			bestScore = score;
			bestHouseIdx = pHouse->ArrayIndex;
		}
	}

	if (NeuralAI::AllowBackpackInference && NeuralAI::BackpackLoaded && !NeuralAI::HeadlessMode)
	{
		NeuralAI::Backpack.GamesPlayed++;

		for (auto const pHouse : HouseClass::Array)
		{
			if (!NeuralAI::IsEnabledForHouse(pHouse))
				continue;

			auto const pExt = HouseExt::Fetch(pHouse);
			const bool won = (pHouse->ArrayIndex == bestHouseIdx) || pHouse->IsWinner;

			if (won)
			{
				NeuralAI::Backpack.WinsCount++;
				NeuralAI::Backpack.EstimatedELO = std::min(3500, NeuralAI::Backpack.EstimatedELO + 8);
			}
			else
			{
				NeuralAI::Backpack.EstimatedELO = std::max(800, NeuralAI::Backpack.EstimatedELO - 4);
			}

			const int arch = std::clamp(pExt->AIBrain_Archetype, 0, 7);
			const int exp = std::clamp(pExt->AIBrain_ActiveExpert, 0, 7);
			const int delta = won ? 12 : -6;

			uint32_t entryIdx = static_cast<uint32_t>((pHouse->SideIndex * 40 + arch * 8 + exp) % 512);
			if (NeuralAI::Backpack.NumEntries < 512)
				NeuralAI::Backpack.NumEntries = std::max(NeuralAI::Backpack.NumEntries, entryIdx + 1);

			auto& entry = NeuralAI::Backpack.Entries[entryIdx];
			entry.StateClusterHash = (static_cast<uint32_t>(pHouse->SideIndex) << 16) | static_cast<uint32_t>(arch << 8) | static_cast<uint32_t>(exp);
			entry.ArchetypeScoreDelta[arch] = ClampInt16(entry.ArchetypeScoreDelta[arch] + delta);
			entry.ExpertScoreDelta[exp] = ClampInt16(entry.ExpertScoreDelta[exp] + delta);
			entry.VisitCount++;

			// Update LoRA rank-16 adapter weights deterministically
			const int loraIdx = (arch * 64 + exp * 8) % (256 * 16);
			NeuralAI::Backpack.LoRA_A[loraIdx] = ClampInt8(NeuralAI::Backpack.LoRA_A[loraIdx] + (won ? 2 : -1));
		}

		NeuralAI::SaveBackpack();
	}

	if (NeuralAI::HeadlessMode)
		NeuralAI::UpdateTelemetrySnapshot(true, NeuralAI::MatchEndWinnerHouse >= 0 ? NeuralAI::MatchEndWinnerHouse : bestHouseIdx, NeuralAI::MatchEndStalemate);
}

void NeuralAI::OnAutoBaseBuildingTriggered(HouseClass* pHouse, bool enabled)
{
	if (!pHouse)
		return;

	pHouse->AutoBaseBuilding = enabled;

	if (enabled)
	{
		if (auto const pExt = HouseExt::TryFetch(pHouse))
			pExt->AIBrain_Campaign = true;
	}
}

void NeuralAI::OnHeadlessFrameTick()
{
	if (!NeuralAI::HeadlessMode)
		return;

	NeuralAI::FramesSinceLastFPSCheck++;
	const DWORD now = GetTickCount();

	if (now - NeuralAI::LastFPSCheckTick >= 1000)
	{
		NeuralAI::CurrentMeasuredFPS = (NeuralAI::FramesSinceLastFPSCheck * 1000) / std::max<DWORD>(1, now - NeuralAI::LastFPSCheckTick);
		NeuralAI::FramesSinceLastFPSCheck = 0;
		NeuralAI::LastFPSCheckTick = now;
	}

	const int frame = Unsorted::CurrentFrame;
	if (frame % 60 != 0)
		return;

	int totalKillsAndCaptures = 0;
	int aliveCombatHouses = 0;
	int lastAliveHouseIdx = -1;

	for (auto const pHouse : HouseClass::Array)
	{
		if (!pHouse || pHouse->IsNeutral() || pHouse->IsObserver())
			continue;

		totalKillsAndCaptures += pHouse->TotalKilledUnits + pHouse->TotalKilledBuildings + pHouse->StolenBuildingsCredits;

		if (!pHouse->Defeated && (pHouse->OwnedBuildings > 0 || pHouse->OwnedUnits > 0 || pHouse->OwnedInfantry > 0))
		{
			aliveCombatHouses++;
			lastAliveHouseIdx = pHouse->ArrayIndex;
		}
	}

	if (totalKillsAndCaptures > NeuralAI::LastTotalKillsAndCaptures)
	{
		NeuralAI::LastTotalKillsAndCaptures = totalKillsAndCaptures;
		NeuralAI::LastCombatFrame = frame;
	}

	// Stalemate detector: 2,700 frames (3 game minutes) without any kills or captures, or 54,000 frames max match duration
	const bool stalemate = (frame > 9000 && (frame - NeuralAI::LastCombatFrame) >= 5400) || (frame >= 54000);
	const bool onlyOneLeft = (frame > 300 && aliveCombatHouses <= 1);

	if (stalemate || onlyOneLeft)
	{
		int bestHouseIdx = lastAliveHouseIdx;
		int bestScore = -1;

		for (auto const pHouse : HouseClass::Array)
		{
			if (!pHouse || pHouse->IsNeutral() || pHouse->IsObserver() || pHouse->Defeated)
				continue;

			const int score = (pHouse->TotalKilledUnits * 200)
				+ (pHouse->TotalKilledBuildings * 450)
				+ (pHouse->OwnedBuildings * 300)
				+ (pHouse->OwnedUnits * 200);

			if (score > bestScore)
			{
				bestScore = score;
				bestHouseIdx = pHouse->ArrayIndex;
			}
		}

		NeuralAI::MatchEndWinnerHouse = bestHouseIdx;
		NeuralAI::MatchEndStalemate = stalemate;
		NeuralAI::UpdateTelemetrySnapshot(true, bestHouseIdx, stalemate);
		NeuralAI::OnScenarioEnd(true);
		Game::IsActive = false;
		return;
	}

	if (frame % 120 == 0)
		NeuralAI::UpdateTelemetrySnapshot(false, -1, false);
}

void NeuralAI::UpdateTelemetrySnapshot(bool matchEnded, int winnerHouseIdx, bool stalemate)
{
	AITrainerSharedTelemetry localSnap = {};
	AITrainerSharedTelemetry* dst = NeuralAI::TelemetryView ? NeuralAI::TelemetryView : &localSnap;

	std::memcpy(dst->Magic, "PHOBOSTM", 8);
	dst->SchemaVersion = 1;
	dst->SlotIndex = static_cast<uint32_t>(NeuralAI::TrainSlotIndex);
	dst->ProcessId = GetCurrentProcessId();
	dst->CurrentFrame = static_cast<uint32_t>(Unsorted::CurrentFrame);
	dst->FramesPerSecond = static_cast<uint32_t>(NeuralAI::CurrentMeasuredFPS);
	dst->MatchFinished = matchEnded ? 1u : 0u;
	dst->WinnerHouseIndex = winnerHouseIdx;
	dst->StalemateTriggered = stalemate ? 1u : 0u;
	dst->RulesSignature = NeuralAI::CurrentRulesSignature;

	uint32_t hCount = 0;
	for (auto const pHouse : HouseClass::Array)
	{
		if (!pHouse || pHouse->IsNeutral() || pHouse->IsObserver() || hCount >= 8)
			continue;

		auto const pExt = HouseExt::Fetch(pHouse);
		auto& hs = dst->Houses[hCount++];
		hs.ArrayIndex = pHouse->ArrayIndex;
		hs.SideIndex = pHouse->SideIndex;
		hs.CountryIndex = pHouse->Type ? pHouse->Type->ArrayIndex : -1;
		hs.Archetype = pExt->AIBrain_Archetype;
		hs.ActiveExpert = pExt->AIBrain_ActiveExpert;
		hs.Credits = pHouse->Available_Money();
		hs.KilledUnits = pHouse->TotalKilledUnits;
		hs.KilledBuildings = pHouse->TotalKilledBuildings;
		hs.OwnedUnits = pHouse->OwnedUnits;
		hs.OwnedBuildings = pHouse->OwnedBuildings;
		hs.OwnedInfantry = pHouse->OwnedInfantry;
		hs.OwnedAircraft = pHouse->OwnedAircraft;
		hs.NetWorthScore = (hs.KilledUnits * 200) + (hs.KilledBuildings * 450) + (hs.OwnedBuildings * 300) + (hs.OwnedUnits * 200);
		hs.IsWinner = (pHouse->ArrayIndex == winnerHouseIdx || pHouse->IsWinner) ? 1 : 0;
		hs.IsDefeated = pHouse->Defeated ? 1 : 0;
		hs.PolicySlot = static_cast<uint8_t>(pExt->AIBrain_PolicySlot);
		hs.ColorR = pHouse->Color.R;
		hs.ColorG = pHouse->Color.G;
		hs.ColorB = pHouse->Color.B;
	}

	dst->NumHouses = hCount;
	std::memset(dst->RadarCells, 0, sizeof(dst->RadarCells));

	for (auto const pBld : BuildingClass::Array)
	{
		if (!pBld || pBld->InLimbo || !pBld->IsAlive || !pBld->Owner || pBld->Owner->IsNeutral())
			continue;

		const CellStruct cell = pBld->GetMapCoords();
		const int rx = std::clamp(static_cast<int>(cell.X) / 2, 0, 127);
		const int ry = std::clamp(static_cast<int>(cell.Y) / 2, 0, 127);
		dst->RadarCells[ry * 128 + rx] = static_cast<uint8_t>((pBld->Owner->ArrayIndex & 0x07) + 1 + 0x80);
	}

	for (auto const pUnit : UnitClass::Array)
	{
		if (!pUnit || pUnit->InLimbo || !pUnit->IsAlive || !pUnit->Owner || pUnit->Owner->IsNeutral())
			continue;

		const CellStruct cell = pUnit->GetMapCoords();
		const int rx = std::clamp(static_cast<int>(cell.X) / 2, 0, 127);
		const int ry = std::clamp(static_cast<int>(cell.Y) / 2, 0, 127);
		dst->RadarCells[ry * 128 + rx] = static_cast<uint8_t>((pUnit->Owner->ArrayIndex & 0x07) + 1);
	}

	if (matchEnded || (Unsorted::CurrentFrame % 240 == 0))
	{
		char jsonPath[MAX_PATH];
		std::snprintf(jsonPath, sizeof(jsonPath), "AI\\train_telemetry_%d.json", NeuralAI::TrainSlotIndex);

		FILE* fp = nullptr;
		if (fopen_s(&fp, jsonPath, "wb") == 0 && fp)
		{
			std::fprintf(fp,
				"{\n"
				"  \"slot\": %u,\n"
				"  \"pid\": %u,\n"
				"  \"frame\": %u,\n"
				"  \"fps\": %u,\n"
				"  \"finished\": %s,\n"
				"  \"winnerHouse\": %d,\n"
				"  \"stalemate\": %s,\n"
				"  \"elo\": %d,\n"
				"  \"gamesPlayed\": %u,\n"
				"  \"rulesSignature\": \"%016llX\",\n"
				"  \"baseSignature\": \"%016llX\",\n"
				"  \"houses\": [\n",
				dst->SlotIndex,
				dst->ProcessId,
				dst->CurrentFrame,
				dst->FramesPerSecond,
				matchEnded ? "true" : "false",
				dst->WinnerHouseIndex,
				stalemate ? "true" : "false",
				NeuralAI::Backpack.EstimatedELO,
				NeuralAI::Backpack.GamesPlayed,
				static_cast<unsigned long long>(NeuralAI::CurrentRulesSignature),
				static_cast<unsigned long long>(NeuralAI::CurrentBaseSignature));

			for (uint32_t i = 0; i < dst->NumHouses; ++i)
			{
				const auto& h = dst->Houses[i];
				auto const pOwner = (h.ArrayIndex >= 0 && h.ArrayIndex < HouseClass::Array.Count) ? HouseClass::Array.GetItem(h.ArrayIndex) : nullptr;
				int defenses = 0;

				if (pOwner)
				{
					for (auto const pOwned : pOwner->Buildings)
					{
						if (pOwned && pOwned->Type && !pOwned->InLimbo && pOwned->Type->BuildCat == BuildCat::Combat)
							defenses++;
					}
				}

				std::map<const TechnoTypeClass*, int> composition;

				if (pOwner)
				{
					for (auto const pOwnedUnit : UnitClass::Array)
					{
						if (pOwnedUnit && pOwnedUnit->Owner == pOwner && pOwnedUnit->Type && !pOwnedUnit->InLimbo && pOwnedUnit->IsAlive)
							composition[pOwnedUnit->Type]++;
					}

					for (auto const pOwnedInf : InfantryClass::Array)
					{
						if (pOwnedInf && pOwnedInf->Owner == pOwner && pOwnedInf->Type && !pOwnedInf->InLimbo && pOwnedInf->IsAlive)
							composition[pOwnedInf->Type]++;
					}

					for (auto const pOwnedAir : AircraftClass::Array)
					{
						if (pOwnedAir && pOwnedAir->Owner == pOwner && pOwnedAir->Type && !pOwnedAir->InLimbo && pOwnedAir->IsAlive)
							composition[pOwnedAir->Type]++;
					}
				}

				std::vector<std::pair<int, const TechnoTypeClass*>> ranked;

				for (auto const& entry : composition)
					ranked.emplace_back(entry.second, entry.first);

				std::sort(ranked.begin(), ranked.end(), [](auto const& a, auto const& b) { return a.first > b.first; });

				std::string compositionText;

				for (size_t r = 0; r < ranked.size() && r < 10; ++r)
				{
					if (r > 0)
						compositionText += ";";

					compositionText += ranked[r].second->ID;
					compositionText += ":" + std::to_string(ranked[r].first);
				}

				std::fprintf(fp,
					"    {\"idx\": %d, \"side\": %d, \"country\": %d, \"archetype\": %d, \"expert\": %d, \"credits\": %d, \"killedUnits\": %d, \"killedBlds\": %d, \"ownedUnits\": %d, \"ownedBlds\": %d, \"score\": %d, \"winner\": %u, \"defeated\": %u, \"policy\": %u, \"harvesters\": %d, \"refineries\": %d, \"warFactories\": %d, \"barracks\": %d, \"defenses\": %d, \"units\": \"%s\"}%s\n",
					h.ArrayIndex, h.SideIndex, h.CountryIndex, h.Archetype, h.ActiveExpert,
					h.Credits, h.KilledUnits, h.KilledBuildings, h.OwnedUnits, h.OwnedBuildings,
					h.NetWorthScore, h.IsWinner, h.IsDefeated, h.PolicySlot,
					pOwner ? pOwner->CountResourceGatherers : 0, pOwner ? pOwner->CountResourceDestinations : 0,
					pOwner ? pOwner->NumWarFactories : 0, pOwner ? pOwner->NumBarracks : 0, defenses, compositionText.c_str(),
					(i + 1 < dst->NumHouses) ? "," : "");
			}

			std::fprintf(fp, "  ]\n}\n");
			std::fclose(fp);
		}
	}
}

void NeuralAI::ExtractFogFilteredFeatures(HouseClass* pHouse, std::array<int8_t, InputVectorSize>& features)
{
	features.fill(0);
	auto const pExt = HouseExt::Fetch(pHouse);

	// 0..31: Own Economy, Power & Infrastructure
	const int credits = pHouse->Available_Money();
	features[0] = ClampInt8(credits / 250);
	features[1] = ClampInt8(pHouse->CountResourceGatherers * 16);
	features[2] = ClampInt8(pHouse->CountResourceDestinations * 20);
	features[3] = ClampInt8((pHouse->PowerOutput - pHouse->PowerDrain) / 10);
	features[4] = pHouse->HasLowPower() ? -100 : 60;
	features[5] = ClampInt8(pHouse->NumConYards * 32);
	features[6] = ClampInt8(pHouse->NumBarracks * 24);
	features[7] = ClampInt8(pHouse->NumWarFactories * 24);
	features[8] = ClampInt8(pHouse->NumShipyards * 24);
	features[9] = ClampInt8(pHouse->NumAirpads * 24);
	features[10] = ClampInt8(pHouse->NumOrePurifiers * 40);
	features[11] = ClampInt8(pHouse->UnitRepairStations.Count * 40);
	features[12] = ClampInt8(pHouse->CloningVats.Count * 40);
	features[13] = pHouse->SpySatActive ? 100 : 0;
	features[14] = pHouse->IsTiberiumShort ? -80 : 40;
	features[15] = ClampInt8(Unsorted::CurrentFrame / 450);

	// 32..63: Own Force Composition
	features[32] = ClampInt8(pHouse->OwnedInfantry * 4);
	features[33] = ClampInt8(pHouse->OwnedUnits * 6);
	features[34] = ClampInt8(pHouse->OwnedNavy * 8);
	features[35] = ClampInt8(pHouse->OwnedAircraft * 10);
	features[36] = ClampInt8(pHouse->OwnedBuildings * 4);
	features[37] = ClampInt8(pHouse->SideIndex * 32);
	features[38] = ClampInt8(pExt->AIBrain_Archetype * 24);

	// 64..95: Strict Fog-of-War Filtered Enemy Intelligence
	// We ONLY inspect enemy objects that are on unshrouded cells, or houses infiltrated by our Spies.
	int visibleEnemyInfantry = 0;
	int visibleEnemyTanks = 0;
	int visibleEnemyAir = 0;
	int visibleEnemyNavy = 0;
	int visibleEnemyDefenses = 0;
	int visibleEnemyBuildings = 0;
	int visibleEnemiesOnBridge = 0;
	int destroyedBridgesCount = 0;
	int ownParasitedOrControlled = 0;
	int ownEliteUnits = 0;
	int visibleEnemyAirShips = 0;
	int visibleEnemyStealthUnits = 0;
	int visibleEnemyGarrisons = 0;
	int visibleEnemyElites = 0;

	for (auto const pBld : BuildingClass::Array)
	{
		if (!pBld || pBld->InLimbo || !pBld->IsAlive || !pBld->Type)
			continue;

		if (pBld->Type->BridgeRepairHut && MapClass::Instance.IsLinkedBridgeDestroyed(pBld->GetMapCoords()))
			destroyedBridgesCount++;
	}

	pExt->AIBrain_EstimatedEnemyThreat.fill(0);

	CellStruct homeCoords = pHouse->BaseCenter != CellStruct::Empty ? pHouse->BaseCenter : pHouse->BaseSpawnCell;
	if (pHouse->ConYards.Count > 0 && pHouse->ConYards.GetItem(0))
		homeCoords = pHouse->ConYards.GetItem(0)->GetMapCoords();
	const int homeGroundZone = (homeCoords != CellStruct::Empty)
		? MapClass::Instance.GetMovementZoneType(homeCoords, MovementZone::Normal, false)
		: -1;
	int islandEnemyBuildings = 0;

	for (auto const pTechno : TechnoClass::Array)
	{
		if (!pTechno || pTechno->InLimbo || !pTechno->IsAlive || !pTechno->Owner)
			continue;

		auto const pOwner = pTechno->Owner;
		if (pOwner == pHouse)
		{
			if (pTechno->Veterancy.IsElite())
				ownEliteUnits++;
			if (pTechno->MindControlledBy != nullptr)
				ownParasitedOrControlled++;
			if (auto const pFoot = abstract_cast<FootClass*>(pTechno))
			{
				if (pFoot->ParasiteEatingMe != nullptr)
					ownParasitedOrControlled++;
			}
			continue;
		}

		if (pHouse->IsAlliedWith(pOwner) || pOwner->IsNeutral() || pOwner->IsObserver())
			continue;

		const CellStruct cellCoords = pTechno->GetMapCoords();
		auto const pCell = MapClass::Instance.TryGetCellAt(cellCoords);
		if (!pCell)
			continue;

		const int ownerIdx = std::clamp(pOwner->ArrayIndex, 0, 7);
		const bool spyInfiltrated = (pExt->AIBrain_InfiltratedEnemyMask & (1 << ownerIdx)) != 0;
		const bool cellVisibleToAI = pHouse->SpySatActive
			|| IsCellVisibleToAI(pHouse, pCell)
			|| spyInfiltrated
			|| (pOwner->WhoLastHurtMe == pHouse->ArrayIndex);

		if (!cellVisibleToAI)
			continue;

		pExt->AIBrain_ScoutedEnemyMask |= (1 << ownerIdx);

		if (pTechno->OnBridge && pCell->ContainsBridge())
			visibleEnemiesOnBridge++;
		if (pTechno->Veterancy.IsElite())
			visibleEnemyElites++;
		if (auto const pTType = pTechno->GetTechnoType())
		{
			if (pTType->Cloakable)
				visibleEnemyStealthUnits++;
		}

		switch (pTechno->WhatAmI())
		{
		case AbstractType::Infantry:
			visibleEnemyInfantry++;
			pExt->AIBrain_EstimatedEnemyThreat[ownerIdx] += 10;
			break;
		case AbstractType::Unit:
			if (pTechno->GetTechnoType()->Naval)
				visibleEnemyNavy++;
			else
				visibleEnemyTanks++;
			pExt->AIBrain_EstimatedEnemyThreat[ownerIdx] += 25;
			break;
		case AbstractType::Aircraft:
			visibleEnemyAir++;
			if (pTechno->GetTechnoType() && pTechno->GetTechnoType()->Strength >= 600)
				visibleEnemyAirShips++; // Kirov Airship / Floating Disc / Heavy Bomber alert!
			pExt->AIBrain_EstimatedEnemyThreat[ownerIdx] += 30;
			break;
		case AbstractType::Building:
		{
			auto const pBld = static_cast<BuildingClass*>(pTechno);
			auto const pBldType = pBld->Type;
			if (pBldType && pBldType->Immune)
				break; // Never waste firepower on Immune structures (e.g. Immune Civ Techs / Immune Derricks / CABHUT)

			visibleEnemyBuildings++;
			if (pBld->Occupants.Count > 0)
				visibleEnemyGarrisons++;
			if (pBldType && pBldType->BuildCat == BuildCat::Combat)
				visibleEnemyDefenses++;
			if (homeGroundZone >= 0)
			{
				const int enemyZone = MapClass::Instance.GetMovementZoneType(cellCoords, MovementZone::Normal, false);
				if (enemyZone >= 0 && enemyZone != homeGroundZone)
					islandEnemyBuildings++;
			}
			pExt->AIBrain_EstimatedEnemyThreat[ownerIdx] += 20;
			break;
		}
		default:
			break;
		}
	}

	features[64] = ClampInt8(visibleEnemyInfantry * 4);
	features[65] = ClampInt8(visibleEnemyTanks * 6);
	features[66] = ClampInt8(visibleEnemyAir * 12);
	features[67] = ClampInt8(visibleEnemyNavy * 10);
	features[68] = ClampInt8(visibleEnemyDefenses * 8);
	features[69] = ClampInt8(visibleEnemyBuildings * 4);
	features[70] = ClampInt8(visibleEnemiesOnBridge * 35);
	features[71] = ClampInt8(visibleEnemyAirShips * 45);
	features[72] = ClampInt8(visibleEnemyStealthUnits * 30);
	features[73] = ClampInt8(visibleEnemyGarrisons * 25);
	features[74] = ClampInt8(visibleEnemyElites * 25);
	features[18] = ClampInt8(destroyedBridgesCount * 30);
	features[22] = ClampInt8(ownParasitedOrControlled * 40);
	features[23] = ClampInt8(ownEliteUnits * 20);
	features[24] = ClampInt8((pHouse->OwnedAircraft - (pHouse->NumAirpads * 4)) * 20);

	// 96..127: Map Spatial Congestion & Allied Beacons
	int alliedBeaconActive = 0;
	for (auto const pAlly : HouseClass::Array)
	{
		if (!pAlly || pAlly == pHouse || !pHouse->IsAlliedWith(pAlly))
			continue;

		const int allyIdx = pAlly->ArrayIndex;
		if (allyIdx >= 0 && allyIdx < 8)
		{
			for (int b = 0; b < 3; ++b)
			{
				if (auto const pBeacon = BeaconManagerClass::Instance.Beacons[allyIdx][b])
				{
					alliedBeaconActive = 100;
					pExt->AIBrain_AttackTargetCell = CellClass::Coord2Cell(pBeacon->Coord);
					break;
				}
			}
		}
	}

	features[96] = ClampInt8(alliedBeaconActive);
	features[97] = ClampInt8(pHouse->Occupiables.Count * 16);

	// Scan local vs. uncolonized distant Ore/Tiberium patches across all owned ConYards for Expert 6 (Multi-Outpost Expansion)
	CellStruct baseCenter = pHouse->BaseCenter != CellStruct::Empty ? pHouse->BaseCenter : pHouse->BaseSpawnCell;
	if (pHouse->ConYards.Count > 0)
		baseCenter = pHouse->ConYards.GetItem(pHouse->ConYards.Count - 1)->GetMapCoords();

	int localOreCells = 0;
	int expansionOreCells = 0;
	int islandExpansionOreCells = 0;
	if (baseCenter != CellStruct::Empty)
	{
		for (int dy = -48; dy <= 48; dy += 3)
		{
			for (int dx = -48; dx <= 48; dx += 3)
			{
				const CellStruct tc { static_cast<short>(baseCenter.X + dx), static_cast<short>(baseCenter.Y + dy) };
				auto const pCell = MapClass::Instance.TryGetCellAt(tc);
				if (!pCell || pCell->GetContainedTiberiumValue() <= 0)
					continue;

				int minYardDist = std::max(std::abs(dx), std::abs(dy));
				for (auto const pYard : pHouse->ConYards)
				{
					if (!pYard || pYard->InLimbo || !pYard->IsAlive)
						continue;

					const CellStruct yc = pYard->GetMapCoords();
					const int d = std::max(std::abs(tc.X - yc.X), std::abs(tc.Y - yc.Y));
					if (d < minYardDist)
						minYardDist = d;
				}

				if (minYardDist <= 14)
				{
					localOreCells++;
				}
				else
				{
					expansionOreCells++;
					if (homeGroundZone >= 0)
					{
						const int oreZone = MapClass::Instance.GetMovementZoneType(tc, MovementZone::Normal, false);
						if (oreZone >= 0 && oreZone != homeGroundZone)
							islandExpansionOreCells++;
					}
				}
			}
		}
	}

	features[19] = ClampInt8(localOreCells * 4);
	features[20] = ClampInt8(expansionOreCells * 3);
	features[21] = ClampInt8((islandEnemyBuildings > 0 ? 75 : 0) + (islandExpansionOreCells > 3 ? 45 : 0));
	if (features[21] > 0 && features[67] < 40)
		features[67] = 40; // Ensure Shipyard is prioritized whenever an enemy or Ore expansion is on a disconnected island!

	// 128..191: Dynamic Rules & GameModeOptions Context
	features[128] = GameModeOptionsClass::Instance.SWAllowed ? 80 : -80;
	features[129] = GameModeOptionsClass::Instance.Crates ? 60 : -60;
	features[130] = GameModeOptionsClass::Instance.ShortGame ? 80 : 0;
	features[131] = GameModeOptionsClass::Instance.BuildOffAlly ? 60 : 0;
	features[132] = GameModeOptionsClass::Instance.MCVRedeploy ? 60 : -60;
	const bool bridgesDestroyable = RulesClass::Instance && RulesClass::Instance->BridgeStrength < 1000000;
	features[134] = bridgesDestroyable ? 80 : -80;
	const bool multiEngineerActive = RulesClass::Instance && RulesClass::Instance->EngineerCaptureLevel < 0.99;
	features[135] = multiEngineerActive ? 80 : -80;
}

void NeuralAI::RunDeterministicMoE(
	HouseClass* pHouse,
	const std::array<int8_t, InputVectorSize>& features,
	std::array<int16_t, OutputVectorSize>& outScores)
{
	outScores.fill(0);
	auto const pExt = HouseExt::Fetch(pHouse);

	// Select active specialist expert (0..7) based on current strategic & spatial priorities
	int activeExpert = 0;
	if (pHouse->HasLowPower() || pHouse->CountResourceGatherers == 0 || pHouse->CountResourceDestinations == 0 || pHouse->NumConYards == 0)
		activeExpert = 0; // Expert 0: Economy, Silos & Crisis Micro
	else if (pHouse->NumBarracks == 0 || pHouse->NumWarFactories == 0 || (pHouse->Available_Money() > 2500 && pHouse->OwnedBuildings < 8))
		activeExpert = 1; // Expert 1: Spatial Tetris Base Architect (Exit Corridors)
	else if ((pHouse->CountResourceDestinations < 3 || features[19] < 24) && features[20] > 12 && pHouse->Available_Money() >= 2000 && (Unsorted::CurrentFrame % 240 < 80))
		activeExpert = 6; // Expert 6: Base-Crawling & Ore Outpost Expansion
	else if ((features[68] > 16 || pExt->AIBrain_Archetype == 2 || pExt->AIBrain_Archetype == 4) && pHouse->OwnedBuildings >= 8 && (Unsorted::CurrentFrame % 240 >= 160))
		activeExpert = 7; // Expert 7: Forward Defense Crawling & Chokepoint Siege
	else if (features[66] > 24 || features[65] > 36)
		activeExpert = 2; // Expert 2: Dynamic Counter-Strategy
	else if (HouseClass::Array.Count > 4 && (Unsorted::CurrentFrame % 240 < 60))
		activeExpert = 3; // Expert 3: Multi-Player Opportunism & Diplomacy
	else if (pExt->AIBrain_Archetype == 1 || (Unsorted::CurrentFrame % 300 < 60 && pHouse->OwnedUnits >= 4))
		activeExpert = 4; // Expert 4: Special Operations & Trickster
	else
		activeExpert = 5; // Expert 5: Tactical Squad Micro, Kiting, Repair & Allied Beacons

	// Easy difficulty disables Spec-Ops Trickster and advanced tactical kiting
	const int diffIdx = pHouse->GetAIDifficultyIndex();
	if (diffIdx == 2 && (activeExpert == 4 || activeExpert == 5 || activeExpert == 7))
		activeExpert = 2;

	pExt->AIBrain_ActiveExpert = activeExpert;

	// Run INT8 Fixed-Point Matrix-Vector Inference on the mapped Base Model
	if (NeuralAI::BaseMappedView && NeuralAI::BaseMappedSize >= sizeof(AIBaseHeader))
	{
		auto const pHdr = reinterpret_cast<const AIBaseHeader*>(NeuralAI::BaseMappedView);
		uint32_t offset = pHdr->ExpertWeightsOffset[activeExpert % 8];
		if (offset == 0)
			offset = pHdr->ExpertWeightsOffset[activeExpert % 6];

		const size_t requiredBytes = static_cast<size_t>(offset) + (NeuralAI::InputVectorSize * NeuralAI::OutputVectorSize);

		if (offset >= sizeof(AIBaseHeader) && requiredBytes <= NeuralAI::BaseMappedSize)
		{
			const int8_t* weights = reinterpret_cast<const int8_t*>(NeuralAI::BaseMappedView + offset);

			for (int o = 0; o < NeuralAI::OutputVectorSize; ++o)
			{
				int32_t acc = 0;
				const int8_t* row = weights + (o * NeuralAI::InputVectorSize);

				for (int i = 0; i < NeuralAI::InputVectorSize; ++i)
					acc += static_cast<int32_t>(features[i]) * static_cast<int32_t>(row[i]);

				const int lutIdx = std::clamp((acc >> 10) + 128, 0, 255);
				outScores[o] = static_cast<int16_t>(pHdr->TanhLUT[lutIdx]) * 8;
			}
		}
	}

	// Apply Signed Backpack LoRA delta & Learned Faction Strategy Memory in Skirmish / Headless Training
	if (NeuralAI::AllowBackpackInference && NeuralAI::BackpackLoaded)
	{
		const AIBackpackFile& policy = NeuralAI::PolicyFor(pHouse);
		std::array<int32_t, 16> rankAct = {};
		for (int r = 0; r < 16; ++r)
		{
			int32_t acc = 0;
			for (int i = 0; i < NeuralAI::InputVectorSize; ++i)
				acc += static_cast<int32_t>(features[i]) * static_cast<int32_t>(policy.LoRA_A[r * NeuralAI::InputVectorSize + i]);

			rankAct[r] = acc >> 8;
		}

		for (int o = 0; o < NeuralAI::OutputVectorSize; ++o)
		{
			int32_t delta = 0;
			for (int r = 0; r < 16; ++r)
				delta += rankAct[r] * static_cast<int32_t>(policy.LoRA_B[r * NeuralAI::OutputVectorSize + o]);

			outScores[o] = ClampInt16(outScores[o] + (delta >> 7) + policy.OutputBias[o]);
		}

		// Recall learned strategy outcome deltas from Backpack Entries for this Faction + Archetype + Expert
		const int curArch = std::clamp(pExt->AIBrain_Archetype, 0, 7);
		const uint32_t entryIdx = static_cast<uint32_t>((pHouse->SideIndex * 40 + curArch * 8 + activeExpert) % 512);
		const auto& memEntry = policy.Entries[entryIdx];
		if (memEntry.VisitCount > 0)
		{
			const int16_t learnedBoost = ClampInt16(memEntry.ArchetypeScoreDelta[curArch] * 4 + memEntry.ExpertScoreDelta[activeExpert] * 4);
			for (int o = 16; o < 36; ++o)
				outScores[o] = ClampInt16(outScores[o] + learnedBoost);

			// If this Archetype has a negative win/loss record in the Backpack for this faction, pivot to a better or unexplored Archetype!
			if (memEntry.ArchetypeScoreDelta[curArch] < -18 && (Unsorted::CurrentFrame % 900 < 30))
				pExt->AIBrain_Archetype = (curArch + 1 + (Unsorted::CurrentFrame / 900)) % 5;
		}
	}

	// Mid-Match Adaptive Strategy Pivot: If our current strategy is stalled (e.g. high frame count, low army vs strong enemy),
	// pivot to a new Archetype (e.g. from Ground Rush -> Spec-Ops Trickster or Air/Naval Tech Siege) to experiment and surprise the opponent!
	if (Unsorted::CurrentFrame > 5400 && (Unsorted::CurrentFrame % 2700 == 0) && pHouse->OwnedUnits < 4 && features[65] > 20)
		pExt->AIBrain_Archetype = (pExt->AIBrain_Archetype + 1 + (pHouse->ArrayIndex % 3)) % 5;

	// Controlled Deterministic Exploration: Inject a small synchronized variation across unit/infantry/aircraft action slots
	// so the AI experiments with alternative unit compositions and tech branches rather than repeating a rigid script
	const int exploreSeed = ((Unsorted::CurrentFrame / 450) * 31 + pHouse->ArrayIndex * 17 + pHouse->SideIndex * 13);
	for (int o = 16; o <= 33; ++o)
	{
		const int exploreBonus = (((exploreSeed + o * 19) % 11) - 5) * 28; // -140 .. +140 exploration window
		outScores[o] = ClampInt16(outScores[o] + exploreBonus);
	}

	// Apply League Archetype & Expert structural priors so the AI executes crisp RTS build orders immediately
	const int arch = pExt->AIBrain_Archetype;
	// Output slots:
	// 0: Power, 1: Refinery, 2: Barracks, 3: WarFactory, 4: Radar/Tech, 5: BaseDefense, 6: AntiAirDefense, 7: Shipyard, 8: Airpad, 9: RepairDepot, 10: Superweapon
	// 16: BasicInf, 17: AntiTankInf, 18: Engineer, 19: Spy/Infiltrator, 20: EliteCommando/AAInf
	// 24: MainBattleTank, 25: AntiInfVehicle, 26: AntiAirVehicle, 27: SiegeArtillery, 28: TransportAPC, 29: NavalCombat, 30: ExpansionMCV, 31: WarFactoryHarvesterBoom
	// 32: Fighter/Helicopter, 33: HeavyBomber
	outScores[0] += pHouse->HasLowPower() ? 2200 : ((pHouse->PowerOutput - pHouse->PowerDrain < 60) ? 1050 : -350);
	outScores[1] += (pHouse->CountResourceDestinations == 0) ? 2100 : ((pHouse->CountResourceDestinations < 2) ? 1150 : ((pHouse->CountResourceDestinations < 3 && pHouse->Available_Money() > 2200) ? 650 : -150));
	outScores[2] += (pHouse->NumBarracks == 0) ? 1750 : -200;
	outScores[3] += (pHouse->NumWarFactories == 0) ? 1950 : ((pHouse->Available_Money() > 3200 && pHouse->NumWarFactories < 2) ? 850 : -100);
	outScores[4] += (pHouse->NumWarFactories > 0) ? 1150 : 100;
	outScores[5] += (features[65] > 18 || arch == 2) ? 420 : 120;
	outScores[6] += (features[66] > 12) ? 750 : 80;
	outScores[7] += ((features[67] > 10 || features[21] > 0) && pHouse->NumShipyards == 0) ? 1550 : -200;
	outScores[8] += (pHouse->NumWarFactories > 0 && pHouse->NumAirpads == 0) ? ((arch == 4) ? 1100 : 550) : -150;
	outScores[9] += (pHouse->OwnedUnits >= 5 && pHouse->UnitRepairStations.Count == 0) ? 750 : -250;
	outScores[10] += (GameModeOptionsClass::Instance.SWAllowed && pHouse->Available_Money() > 4500) ? 850 : -600;
	outScores[11] += (pHouse->IsTiberiumShort || features[14] < 0) ? 1450 : -300;

	outScores[16] += (arch == 3) ? 600 : 350;
	outScores[17] += (features[65] > 12) ? 750 : 400;
	outScores[18] += (arch == 1 || pHouse->ToCapture >= 0 || (features[18] > 0 && pHouse->OwnedUnits >= 5)) ? 650 : 150;
	outScores[19] += (arch == 1 && pExt->AIBrain_InfiltratedEnemyMask == 0) ? 500 : 100;
	outScores[20] += (features[66] > 12) ? 700 : 300;

	// Slot 21: Repair Destroyed Bridge via Engineer + BridgeRepairHut (only when ready to push offensively)
	if (features[18] > 0)
	{
		if (pHouse->OwnedUnits * 6 >= features[65] && pHouse->OwnedUnits >= 4)
			outScores[21] += 1200; // We have ground superiority: repair bridge to advance
		else
			outScores[21] -= 1500; // Outnumbered: keep broken bridge as a defensive moat!
	}
	else
	{
		outScores[21] = -2000;
	}

	// Slot 22: Demolish Intact Bridge (Ambush enemy column on bridge, or scorched-earth defense when outnumbered)
	if (features[134] > 0) // Bridges are destroyable (Immune Bridges GameOption is OFF)
	{
		if (features[70] > 0)
			outScores[22] += 1900; // Enemy units are currently crossing a bridge: blow it up to drown them!
		else if (features[65] > (pHouse->OwnedUnits * 6 + 18))
			outScores[22] += 1100; // Outnumbered by approaching enemy tanks: scorched-earth bridge denial
		else
			outScores[22] -= 1000;
	}
	else
	{
		outScores[22] = -3000; // Immune Bridges ON: never waste ammo shooting bridges
	}

	outScores[24] += 800 + (arch == 3 ? 300 : 0);
	outScores[25] += (features[64] > 20) ? 700 : 300;
	outScores[26] += (features[66] > 12) ? 950 : 250;
	outScores[27] += (features[68] > 16 || arch == 4 || activeExpert == 7) ? 950 : 350;
	outScores[28] += (features[21] > 0) ? 1450 : ((arch == 1 && pHouse->OwnedInfantry >= 3) ? 650 : 150);
	outScores[29] += 600;

	// Slot 30: Multi-Outpost Expansion MCV (Expert 6: Builds a 2nd, 3rd, 4th... MCV whenever uncolonized Ore fields exist)
	const int mcvReserveThreshold = 2800 + (pHouse->NumConYards * 1000);
	if (pHouse->NumConYards >= 1 && pHouse->NumWarFactories > 0 && pHouse->Available_Money() > mcvReserveThreshold
		&& features[20] > 9 && (features[19] < 28 || pHouse->Available_Money() > mcvReserveThreshold + 2500))
	{
		outScores[30] += 1150;
	}
	else
	{
		outScores[30] -= 1500;
	}

	// Slot 31: War Factory Harvester Boom (Dynamic Harvester-to-Refinery ratio scaled by Ore/Gem abundance on map)
	const int refCount = std::max(1, pHouse->CountResourceDestinations);
	const bool richResources = (features[19] >= 32 || (features[19] + features[20]) >= 56);
	const bool scarceResources = (features[19] < 12 && features[20] < 18);
	const int desiredHarvesters = scarceResources
		? refCount
		: std::min(10, refCount * (richResources ? 3 : 2));

	if (pHouse->CountResourceGatherers == 0)
		outScores[31] += 2600; // Emergency: 0 Harvesters alive -> build Harvester immediately in War Factory
	else if (pHouse->CountResourceGatherers < desiredHarvesters)
		outScores[31] += richResources ? 1650 : 1400; // Abundant Ore/Gems: pump extra Harvesters from War Factory!
	else
		outScores[31] -= 1200; // Target reached or Ore depleted: stop building Harvesters and build combat units

	// Expert 6 & Expert 7 spatial specialization boosts
	if (activeExpert == 6)
	{
		outScores[1] += 650;  // Crawl Refineries toward next Ore/Tiberium field
		outScores[3] += 350;  // Forward War Factory production
		outScores[31] += 300; // Boost War Factory Harvester production when colonizing rich Ore fields
	}
	else if (activeExpert == 7)
	{
		outScores[5] += 600;  // Forward Ground Defense Turrets
		outScores[6] += 450;  // Anti-Air Defense Perimeter
		outScores[27] += 400; // Chokepoint Siege Artillery
	}

	outScores[32] += (arch == 4) ? 850 : 400;
	outScores[33] += (arch == 4) ? 900 : 450;
}

namespace
{
	bool IsAmphibiousOrAirTransport(const TechnoTypeClass* pType)
	{
		if (!pType || pType->Passengers < 4)
			return false;

		return pType->SpeedType == SpeedType::Hover
			|| pType->SpeedType == SpeedType::Amphibious
			|| pType->SpeedType == SpeedType::FloatBeach
			|| pType->SpeedType == SpeedType::Winged
			|| pType->MovementZone == MovementZone::Amphibious
			|| pType->MovementZone == MovementZone::AmphibiousDestroyer
			|| pType->MovementZone == MovementZone::AmphibiousCrusher;
	}

	// Finds a safe coastal/flank cell on a target island (matching targetIslandZone) away from enemy turrets & units
	CellClass* FindSafeIslandLandingCell(HouseClass* pHouse, const CellStruct& islandRefCell, int targetIslandZone, bool preferOre)
	{
		CellClass* pBestLanding = nullptr;
		int bestScore = -999999;

		for (int dy = -22; dy <= 22; dy += 2)
		{
			for (int dx = -22; dx <= 22; dx += 2)
			{
				const CellStruct cand { static_cast<short>(islandRefCell.X + dx), static_cast<short>(islandRefCell.Y + dy) };
				auto const pCell = MapClass::Instance.TryGetCellAt(cand);
				if (!pCell || pCell->LandType == LandType::Water || pCell->LandType == LandType::Rock || pCell->GetBuilding())
					continue;

				if (targetIslandZone >= 0)
				{
					const int cz = MapClass::Instance.GetMovementZoneType(cand, MovementZone::Normal, false);
					if (cz != targetIslandZone)
						continue;
				}

				if (!pCell->IsClearToMove(SpeedType::Track, true, true, -1, MovementZone::Normal, -1, false))
					continue;

				int score = 200 - (std::abs(dx) + std::abs(dy)) * 3;

				// Bonus for coastal/beach cells so amphibious hovercraft can beach and unload without driving inland through defenses
				bool nearCoast = (pCell->LandType == LandType::Beach);
				if (!nearCoast)
				{
					const CellStruct neighbors[4] = {
						{ static_cast<short>(cand.X + 2), cand.Y },
						{ static_cast<short>(cand.X - 2), cand.Y },
						{ cand.X, static_cast<short>(cand.Y + 2) },
						{ cand.X, static_cast<short>(cand.Y - 2) }
					};
					for (const auto& nc : neighbors)
					{
						if (auto const pNC = MapClass::Instance.TryGetCellAt(nc))
						{
							if (pNC->LandType == LandType::Water || pNC->LandType == LandType::Beach)
							{
								nearCoast = true;
								break;
							}
						}
					}
				}
				if (nearCoast)
					score += 350;

				// Bonus for nearby Ore/Tiberium when landing an Expansion MCV on an island
				if (preferOre && pCell->GetContainedTiberiumValue() > 0)
					score += 350;

				// Avoid visible enemy defensive structures (Prism Towers, Tesla Coils, Obelisks, SAMs, Bunkers) and enemy armies
				for (auto const pBld : BuildingClass::Array)
				{
					if (!pBld || pBld->InLimbo || !pBld->IsAlive || !pBld->Owner || !pBld->Type)
						continue;

					if (pBld->Owner == pHouse || pHouse->IsAlliedWith(pBld->Owner) || pBld->Owner->IsNeutral())
						continue;

					const CellStruct bc = pBld->GetMapCoords();
					const int dist = std::max(std::abs(cand.X - bc.X), std::abs(cand.Y - bc.Y));
					if (dist <= 11)
					{
						if (pBld->Type->BuildCat == BuildCat::Combat)
							score -= (12 - dist) * 140; // Heavy penalty for enemy defensive turrets in range!
						else if (dist <= 5)
							score -= 120; // Don't try to unload directly on top of congested enemy buildings
					}
				}

				for (auto const pEnemyU : UnitClass::Array)
				{
					if (!pEnemyU || pEnemyU->InLimbo || !pEnemyU->IsAlive || !pEnemyU->Owner)
						continue;

					if (pEnemyU->Owner == pHouse || pHouse->IsAlliedWith(pEnemyU->Owner) || pEnemyU->Owner->IsNeutral())
						continue;

					const CellStruct uc = pEnemyU->GetMapCoords();
					const int dist = std::max(std::abs(cand.X - uc.X), std::abs(cand.Y - uc.Y));
					if (dist <= 8)
						score -= 220;
				}

				if (score > bestScore)
				{
					bestScore = score;
					pBestLanding = pCell;
				}
			}
		}

		return pBestLanding ? pBestLanding : MapClass::Instance.TryGetCellAt(islandRefCell);
	}
}

void NeuralAI::ExecuteTacticalAndSpecOpsMicro(
	HouseClass* pHouse,
	const std::array<int16_t, OutputVectorSize>& scores)
{
	auto const pExt = HouseExt::Fetch(pHouse);

	CellStruct homeCoords = pHouse->BaseCenter != CellStruct::Empty ? pHouse->BaseCenter : pHouse->BaseSpawnCell;
	if (pHouse->ConYards.Count > 0 && pHouse->ConYards.GetItem(0))
		homeCoords = pHouse->ConYards.GetItem(0)->GetMapCoords();
	const int homeGroundZone = (homeCoords != CellStruct::Empty)
		? MapClass::Instance.GetMovementZoneType(homeCoords, MovementZone::Normal, false)
		: -1;

	// Find any idle Amphibious/Hover Transport owned by this house that still has passenger room
	UnitClass* pLoadingTransport = nullptr;
	for (auto const pU : UnitClass::Array)
	{
		if (!pU || pU->Owner != pHouse || pU->InLimbo || !pU->IsAlive || !pU->Type)
			continue;

		if (IsAmphibiousOrAirTransport(pU->Type) && pU->Passengers.NumPassengers < pU->Type->Passengers)
		{
			pLoadingTransport = pU;
			break;
		}
	}

	// Multi-Player Opportunism: Focus the highest-threat or most vulnerable visible enemy house
	int bestEnemyIdx = -1;
	int bestEnemyPriority = -1;

	for (auto const pOther : HouseClass::Array)
	{
		if (!pOther || pOther == pHouse || pHouse->IsAlliedWith(pOther) || pOther->IsNeutral() || pOther->IsObserver() || pOther->Defeated)
			continue;

		const int idx = std::clamp(pOther->ArrayIndex, 0, 7);
		int priority = pExt->AIBrain_EstimatedEnemyThreat[idx];

		if (pHouse->WhoLastHurtMe == pOther->ArrayIndex)
			priority += 250;

		if (priority > bestEnemyPriority)
		{
			bestEnemyPriority = priority;
			bestEnemyIdx = pOther->ArrayIndex;
		}
	}

	if (bestEnemyIdx >= 0)
		pHouse->EnemyHouseIndex = bestEnemyIdx;

	// Tactical Squad Micro: Retreat damaged expensive/Elite/Parasited vehicles to Service Depot, Chrono-Miner Warp escape, Beacons & Allied Base Rescue!
	BuildingClass* pRepairDepot = pHouse->UnitRepairStations.Count > 0 ? pHouse->UnitRepairStations.GetItem(0) : nullptr;
	BuildingClass* pGrinder = pHouse->Grinders.Count > 0 ? pHouse->Grinders.GetItem(0) : nullptr;
	CellClass* pBeaconCell = (pExt->AIBrain_AttackTargetCell != CellStruct::Empty)
		? MapClass::Instance.TryGetCellAt(pExt->AIBrain_AttackTargetCell)
		: nullptr;

	// If no manual Beacon is active, check if an ALLIED player's base is under attack (< 85% HP) and rush to defend our Ally!
	if (!pBeaconCell)
	{
		for (auto const pBld : BuildingClass::Array)
		{
			if (!pBld || pBld->InLimbo || !pBld->IsAlive || !pBld->Owner || !pBld->Type)
				continue;
			if (pBld->Owner != pHouse && pHouse->IsAlliedWith(pBld->Owner) && !pBld->Owner->IsNeutral())
			{
				if (pBld->HasBeenAttacked && pBld->GetHealthPercentage() < 0.85)
				{
					pBeaconCell = pBld->GetCell();
					break;
				}
			}
		}
	}

	for (auto const pUnit : UnitClass::Array)
	{
		if (!pUnit || pUnit->Owner != pHouse || pUnit->InLimbo || !pUnit->IsAlive)
			continue;

		auto const pType = pUnit->Type;
		if (!pType)
			continue;

		// Evade radiation clouds (Desolator / Nuke fallout) immediately so units don't melt in place
		if (auto const pUnitCell = pUnit->GetCell())
		{
			if (pUnitCell->GetRadLevel() > 25 && pUnit->CurrentMission != Mission::Move)
				pUnit->Scatter(CoordStruct::Empty, true, false);
		}

		// If infected by a Terror Drone / Giant Squid (ParasiteEatingMe), rush immediately to Service Depot to rip it out!
		if (pRepairDepot && pUnit->ParasiteEatingMe != nullptr)
		{
			if (pUnit->CurrentMission != Mission::Enter)
			{
				pUnit->SetDestination(pRepairDepot, true);
				pUnit->QueueMission(Mission::Enter, true);
			}
			continue;
		}

		if (pType->Harvester || pType->ResourceGatherer)
		{
			// IMPORTANT: Never call SetDestination() on Harvesters or Chrono Miners (Teleporter=yes)!
			// In Yuri's Revenge, calling SetDestination() on a ChronoLocomotionClass unit triggers an immediate Chrono-shift
			// and resets UnitClass::Mission_Harvest's internal state machine, causing Allied Chrono Miners to loop-teleport
			// endlessly to the Refinery entrance cell. Vanilla Mission_Harvest natively handles ore/gem search and Chrono return.
			if (pUnit->CurrentMission == Mission::Sleep || pUnit->CurrentMission == Mission::None)
				pUnit->QueueMission(Mission::Harvest, false);
			continue;
		}

		// Yuri Grinder Economy: Recycle heavily damaged Mind-Controlled enemy vehicles into the Grinder for instant cash!
		if (pGrinder && pUnit->MindControlledBy != nullptr && pUnit->GetHealthPercentage() < 0.45 && pUnit->CurrentMission != Mission::Enter)
		{
			pUnit->SetDestination(pGrinder, true);
			pUnit->QueueMission(Mission::Enter, true);
			continue;
		}

		// Auto-deploy MCV if we have no ConYards, or march any additional Expansion MCV (2nd, 3rd, 4th...) to an uncolonized Ore field
		if (pType->DeploysInto && pType->DeploysInto->ConstructionYard)
		{
			if (pHouse->NumConYards == 0)
			{
				if (pUnit->CurrentMission != Mission::Unload)
					pUnit->QueueMission(Mission::Unload, true);

				continue;
			}

			const CellStruct mcvCell = pUnit->GetMapCoords();
			int minYardDist = 9999;
			CellStruct newestYard = pHouse->BaseCenter;
			for (auto const pYard : pHouse->ConYards)
			{
				if (!pYard || pYard->InLimbo || !pYard->IsAlive)
					continue;

				const CellStruct yc = pYard->GetMapCoords();
				newestYard = yc;
				const int d = std::max(std::abs(mcvCell.X - yc.X), std::abs(mcvCell.Y - yc.Y));
				if (d < minYardDist)
					minYardDist = d;
			}

			if (minYardDist >= 13)
			{
				if (pUnit->CurrentMission != Mission::Unload)
					pUnit->QueueMission(Mission::Unload, true);
			}
			else if (pUnit->CurrentMission == Mission::Guard || pUnit->CurrentMission == Mission::Area_Guard)
			{
				// Find the closest rich Ore/Tiberium patch that is >= 15 cells away from ALL existing ConYards
				CellClass* pExpansionDest = nullptr;
				int bestExpDist = 99999;

				for (int dy = -56; dy <= 56; dy += 3)
				{
					for (int dx = -56; dx <= 56; dx += 3)
					{
						const CellStruct tc { static_cast<short>(newestYard.X + dx), static_cast<short>(newestYard.Y + dy) };
						auto const pCell = MapClass::Instance.TryGetCellAt(tc);
						if (!pCell || pCell->GetContainedTiberiumValue() <= 0)
							continue;

						bool tooCloseToExistingYard = false;
						for (auto const pYard : pHouse->ConYards)
						{
							if (!pYard || pYard->InLimbo || !pYard->IsAlive)
								continue;

							const CellStruct yc = pYard->GetMapCoords();
							if (std::max(std::abs(tc.X - yc.X), std::abs(tc.Y - yc.Y)) < 15)
							{
								tooCloseToExistingYard = true;
								break;
							}
						}

						if (tooCloseToExistingYard)
							continue;

						const int distFromMCV = std::abs(tc.X - mcvCell.X) + std::abs(tc.Y - mcvCell.Y);
						if (distFromMCV < bestExpDist)
						{
							bestExpDist = distFromMCV;
							pExpansionDest = pCell;
						}
					}
				}

				if (pExpansionDest)
				{
					const int mcvZone = MapClass::Instance.GetMovementZoneType(mcvCell, MovementZone::Normal, false);
					const int expZone = MapClass::Instance.GetMovementZoneType(pExpansionDest->MapCoords, MovementZone::Normal, false);
					const bool onDifferentIsland = (mcvZone >= 0 && expZone >= 0 && mcvZone != expZone);

					if (onDifferentIsland && pLoadingTransport
						&& pType->TechnoTypeClass::Size <= pLoadingTransport->Type->SizeLimit
						&& (pLoadingTransport->Passengers.GetTotalSize() + static_cast<int>(pType->TechnoTypeClass::Size)) <= pLoadingTransport->Type->Passengers)
					{
						// Expansion Ore field is on an island! Board the Amphibious Hover Transport to colonize the island
						pUnit->SetDestination(pLoadingTransport, true);
						pUnit->QueueMission(Mission::Enter, true);
					}
					else if (!onDifferentIsland)
					{
						pUnit->SetDestination(pExpansionDest, true);
						pUnit->QueueMission(Mission::Move, true);
					}
				}
			}

			continue;
		}

		// Tactical Combat Deployment (Tick Tank, Nod Artillery, Juggernaut, Siege Chopper)
		// & Universal Deployable Key Structures across all mods (Mobile Outposts/Explorers, Mobile Refineries/Factories, Power/Tech/Turret Cores, CABAL Nanocores)!
		if ((pType->DeploysInto && !pType->DeploysInto->ConstructionYard) || pType->DeployFire)
		{
			const bool hasPrimaryWeapon = (pType->GetWeapon(0) != nullptr && pType->GetWeapon(0)->WeaponType != nullptr);
			auto const pTargetBld = pType->DeploysInto;
			const bool isKeyStructureDeployer = (pTargetBld != nullptr && !pType->Harvester && !pType->ResourceGatherer
				&& (pTargetBld->BuildCat != BuildCat::Combat || !hasPrimaryWeapon));

			if (isKeyStructureDeployer)
			{
				const CellStruct nanoCell = pUnit->GetMapCoords();
				const bool isNanoIdle = (pUnit->CurrentMission == Mission::Guard || pUnit->CurrentMission == Mission::Area_Guard);

				// Check if this unit deploys into an Outpost Expansion Hub (BaseNormal Outpost, Expansion Refinery, or Forward Factory)
				int ownedRefineries = 0;
				int ownedWarFactories = 0;
				int ownedBarracks = 0;
				int minDistFromBaseBld = 9999;
				int minDistFromRefOrYard = 9999;
				for (auto const pOwnBld : pHouse->Buildings)
				{
					if (!pOwnBld || pOwnBld->InLimbo || !pOwnBld->IsAlive || !pOwnBld->Type)
						continue;
					const CellStruct bc = pOwnBld->GetMapCoords();
					const int d = std::max(std::abs(nanoCell.X - bc.X), std::abs(nanoCell.Y - bc.Y));
					if (d < minDistFromBaseBld)
						minDistFromBaseBld = d;
					if (pOwnBld->Type->Refinery || pOwnBld->Type->ConstructionYard || pOwnBld->Type->BaseNormal)
					{
						if (d < minDistFromRefOrYard)
							minDistFromRefOrYard = d;
					}
					if (pOwnBld->Type->Refinery)
						ownedRefineries++;
					else if (pOwnBld->Type->Factory == AbstractType::UnitType && !pOwnBld->Type->Naval)
						ownedWarFactories++;
					else if (pOwnBld->Type->Factory == AbstractType::InfantryType)
						ownedBarracks++;
				}

				const bool isDedicatedMobileOutpost = (pTargetBld->BaseNormal && !pTargetBld->Refinery
					&& pTargetBld->Factory == AbstractType::None && pTargetBld->PowerBonus <= pTargetBld->PowerDrain
					&& pHouse->Buildings.Count >= 1);
				const bool isExpansionDeployer = isDedicatedMobileOutpost
					|| (pTargetBld->Refinery && ownedRefineries >= 1 && scores[30] > 650)
					|| (pTargetBld->Factory == AbstractType::UnitType && !pTargetBld->Naval && ownedWarFactories >= 1 && scores[30] > 750)
					|| (pTargetBld->Factory == AbstractType::InfantryType && ownedBarracks >= 1 && scores[30] > 800);

				if (isExpansionDeployer)
				{
					if (minDistFromRefOrYard >= 12)
					{
						if (pUnit->CurrentMission != Mission::Unload)
							pUnit->QueueMission(Mission::Unload, true);
					}
					else if (isNanoIdle && homeCoords != CellStruct::Empty)
					{
						// Search for closest uncolonized Ore/Gem field >= 14 cells from all existing Refineries, Outposts & ConYards
						CellClass* pExpOreCell = nullptr;
						int bestOreDist = 99999;
						for (int dy = -56; dy <= 56; dy += 3)
						{
							for (int dx = -56; dx <= 56; dx += 3)
							{
								const CellStruct tc { static_cast<short>(homeCoords.X + dx), static_cast<short>(homeCoords.Y + dy) };
								auto const pC = MapClass::Instance.TryGetCellAt(tc);
								if (!pC || pC->GetContainedTiberiumValue() <= 0)
									continue;

								bool nearExistingRef = false;
								for (auto const pOwnBld : pHouse->Buildings)
								{
									if (!pOwnBld || pOwnBld->InLimbo || !pOwnBld->IsAlive || !pOwnBld->Type)
										continue;
									if (pOwnBld->Type->Refinery || pOwnBld->Type->ConstructionYard || pOwnBld->Type->BaseNormal)
									{
										const CellStruct bc = pOwnBld->GetMapCoords();
										if (std::max(std::abs(tc.X - bc.X), std::abs(tc.Y - bc.Y)) < 14)
										{
											nearExistingRef = true;
											break;
										}
									}
								}
								if (nearExistingRef)
									continue;

								const int distFromNano = std::abs(tc.X - nanoCell.X) + std::abs(tc.Y - nanoCell.Y);
								if (distFromNano < bestOreDist)
								{
									bestOreDist = distFromNano;
									pExpOreCell = pC;
								}
							}
						}

						if (pExpOreCell)
						{
							const int nanoZone = MapClass::Instance.GetMovementZoneType(nanoCell, MovementZone::Normal, false);
							const int expZone = MapClass::Instance.GetMovementZoneType(pExpOreCell->MapCoords, MovementZone::Normal, false);
							const bool onDifferentIsland = (nanoZone >= 0 && expZone >= 0 && nanoZone != expZone);

							if (onDifferentIsland && pLoadingTransport
								&& pType->TechnoTypeClass::Size <= pLoadingTransport->Type->SizeLimit
								&& (pLoadingTransport->Passengers.GetTotalSize() + static_cast<int>(pType->TechnoTypeClass::Size)) <= pLoadingTransport->Type->Passengers)
							{
								pUnit->SetDestination(pLoadingTransport, true);
								pUnit->QueueMission(Mission::Enter, true);
							}
							else if (!onDifferentIsland)
							{
								pUnit->SetDestination(pExpOreCell, true);
								pUnit->QueueMission(Mission::Move, true);
							}
						}
						else
						{
							pUnit->QueueMission(Mission::Unload, true);
						}
					}
					continue;
				}

				// Defense Turret Deployer (Mobile Turret Cores / ROBOTEGG_OBELISK2, ROBOTEGG_CERBERUS2, etc.):
				// Move 6..10 cells away from factories/ConYard to the defensive perimeter before unpacking!
				if (pTargetBld->BuildCat == BuildCat::Combat)
				{
					if (minDistFromBaseBld >= 6 && NeuralAI::IsCellClearedOfExitCorridors(pHouse, pTargetBld, nanoCell))
					{
						if (pUnit->CurrentMission != Mission::Unload)
							pUnit->QueueMission(Mission::Unload, true);
						continue;
					}
					if (isNanoIdle && homeCoords != CellStruct::Empty)
					{
						const CellStruct perimCell = NeuralAI::FindSpatialTetrisBuildCell(pHouse, pTargetBld);
						if (auto const pPerim = MapClass::Instance.TryGetCellAt(perimCell))
						{
							pUnit->SetDestination(pPerim, true);
							pUnit->QueueMission(Mission::Move, true);
							continue;
						}
					}
				}

				// Base Infrastructure Deployer (Power, Barracks, Radar, Tech, Repair Bay, Shield/Gap Generator, or first Refinery/Factory):
				// Unpack as soon as clear of factory exit corridors; if blocking a factory door, step aside first!
				if (isNanoIdle)
				{
					if (NeuralAI::IsCellClearedOfExitCorridors(pHouse, pTargetBld, nanoCell))
					{
						pUnit->QueueMission(Mission::Unload, true);
					}
					else
					{
						const CellStruct clearCell = NeuralAI::FindSpatialTetrisBuildCell(pHouse, pTargetBld);
						if (auto const pClear = MapClass::Instance.TryGetCellAt(clearCell))
						{
							pUnit->SetDestination(pClear, true);
							pUnit->QueueMission(Mission::Move, true);
						}
					}
					continue;
				}
			}
			else if (pUnit->Target != nullptr && pUnit->CurrentMission == Mission::Attack)
			{
				pUnit->QueueMission(Mission::Unload, true);
				continue;
			}
		}

		// Preserve Elite vehicles (< 60% HP) and expensive vehicles (< 35% HP) by retreating to Service Depot
		const double repairThreshold = pUnit->Veterancy.IsElite() ? 0.60 : 0.35;
		if (pRepairDepot && (pType->Cost >= 800 || pUnit->Veterancy.IsElite()) && pUnit->GetHealthPercentage() < repairThreshold)
		{
			if (pUnit->CurrentMission != Mission::Enter)
			{
				pUnit->SetDestination(pRepairDepot, true);
				pUnit->QueueMission(Mission::Enter, true);
			}

			continue;
		}

		const bool isUnitIdle = (pUnit->CurrentMission == Mission::Guard || pUnit->CurrentMission == Mission::Area_Guard);
		const auto pPrimaryWpn = pType->GetWeapon(0) ? pType->GetWeapon(0)->WeaponType : nullptr;

		// Field Repair Vehicles (Repair IFV / Mobile Mechanic / Support Drone where Damage < 0): Heal damaged own OR allied vehicles in the field!
		if (pPrimaryWpn && pPrimaryWpn->Damage < 0 && isUnitIdle)
		{
			for (auto const pAllyU : UnitClass::Array)
			{
				if (!pAllyU || pAllyU == pUnit || !pAllyU->Owner || !pHouse->IsAlliedWith(pAllyU->Owner) || pAllyU->InLimbo || !pAllyU->IsAlive || !pAllyU->Type || pAllyU->Type->Organic)
					continue;
				if (pAllyU->GetHealthPercentage() < 0.85 && pUnit->DistanceFrom(pAllyU) <= 3584)
				{
					pUnit->SetTarget(pAllyU);
					pUnit->QueueMission(Mission::Attack, true);
					break;
				}
			}
			continue;
		}

		// Yuri Floating Disc (DrainWeapon=yes): Prioritize draining Enemy Superweapons, Refineries (steal credits!), and Power Plants (blackout!)
		if (pPrimaryWpn && pPrimaryWpn->DrainWeapon && isUnitIdle && pUnit->DrainTarget == nullptr)
		{
			BuildingClass* pBestDrainBld = nullptr;
			int bestDrainScore = -1;
			for (auto const pEBld : BuildingClass::Array)
			{
				if (!pEBld || pEBld->InLimbo || !pEBld->IsAlive || !pEBld->Owner || !pEBld->Type || pEBld->DrainingMe != nullptr)
					continue;
				if (pEBld->Owner == pHouse || pHouse->IsAlliedWith(pEBld->Owner) || pEBld->Owner->IsNeutral())
					continue;
				if (!pEBld->GetCell() || !IsCellVisibleToAI(pHouse, pEBld->GetCell()))
					continue;

				int dScore = 0;
				if (pEBld->Type->SuperWeapon != -1)
					dScore = 100;
				else if (pEBld->Type->Refinery)
					dScore = 90;
				else if (pEBld->Type->PowerBonus > 0)
					dScore = 85;
				else if (pEBld->Type->BuildCat == BuildCat::Combat && pEBld->Type->Powered)
					dScore = 75;

				if (dScore > bestDrainScore)
				{
					bestDrainScore = dScore;
					pBestDrainBld = pEBld;
				}
			}
			if (pBestDrainBld)
			{
				pUnit->SetTarget(pBestDrainBld);
				pUnit->QueueMission(Mission::Attack, true);
				continue;
			}
		}

		// Tank Bunker Garrisoning (Bunkerable=yes into empty owned Tank Bunker): Fortify base perimeter!
		if (pType->Bunkerable && isUnitIdle && pUnit->BunkerLinkedItem == nullptr && (Unsorted::CurrentFrame % 90 < 20))
		{
			for (auto const pBld : pHouse->Buildings)
			{
				if (!pBld || pBld->InLimbo || !pBld->IsAlive || !pBld->Type || !pBld->Type->Bunker || pBld->BunkerLinkedItem != nullptr)
					continue;
				if (pUnit->DistanceFrom(pBld) <= 3072)
				{
					pUnit->SetDestination(pBld, true);
					pUnit->QueueMission(Mission::Enter, true);
					break;
				}
			}
		}

		// Respond to allied Beacon with idle combat units
		if (pBeaconCell && isUnitIdle)
		{
			pUnit->SetDestination(pBeaconCell, true);
			pUnit->QueueMission(Mission::AttackMove, true);
		}
	}

	// Special Operations: Spies, Bridge Repair, Urban Garrisoning, Bio-Reactors, Tesla Overpowering, Hospitals, Medics, Crazy Ivan & Bridge Demolition
	BuildingClass* pTargetBridgeHut = nullptr;
	BuildingClass* pBioReactor = nullptr;
	BuildingClass* pOverpowerCoil = nullptr;
	BuildingClass* pHospital = nullptr;
	for (auto const pBld : BuildingClass::Array)
	{
		if (!pBld || pBld->InLimbo || !pBld->IsAlive || !pBld->Type)
			continue;

		if (scores[21] > 400 && !pTargetBridgeHut && pBld->Type->BridgeRepairHut && MapClass::Instance.IsLinkedBridgeDestroyed(pBld->GetMapCoords()))
			pTargetBridgeHut = pBld;

		if (pBld->Owner && pHouse->IsAlliedWith(pBld->Owner))
		{
			if (pBld->Owner == pHouse && !pBioReactor && pHouse->HasLowPower() && pBld->Type->InfantryAbsorb && pBld->Passengers.NumPassengers < pBld->Type->Passengers)
				pBioReactor = pBld;

			if (!pOverpowerCoil && pBld->Type->Overpowerable && (!pBld->IsOverpowered || pBld->Owner->HasLowPower()))
				pOverpowerCoil = pBld; // Overpower own OR allied Tesla Coils!

			if (!pHospital && pBld->Type->Hospital)
				pHospital = pBld; // Use own OR allied Hospital!
		}
	}

	for (auto const pInf : InfantryClass::Array)
	{
		if (!pInf || pInf->Owner != pHouse || pInf->InLimbo || !pInf->IsAlive || !pInf->Type)
			continue;

		// Evade radiation puddles (Desolator / Nuke fallout) immediately
		if (auto const pInfCell = pInf->GetCell())
		{
			if (pInfCell->GetRadLevel() > 10 && pInf->CurrentMission != Mission::Move)
			{
				pInf->Scatter(CoordStruct::Empty, true, false);
				continue;
			}
		}

		const bool isIdle = (pInf->CurrentMission == Mission::Guard || pInf->CurrentMission == Mission::Area_Guard);
		const auto pInfWpn = pInf->Type->GetWeapon(0) ? pInf->Type->GetWeapon(0)->WeaponType : nullptr;

		// Retreat wounded valuable/Elite infantry (< 55% HP) to Hospital if owned!
		if (pHospital && !pInf->Type->Engineer && (pInf->Type->Cost >= 300 || pInf->Veterancy.IsElite()) && pInf->GetHealthPercentage() < 0.55)
		{
			if (pInf->CurrentMission != Mission::Enter && pInf->DistanceFrom(pHospital) <= 4096)
			{
				pInf->SetDestination(pHospital, true);
				pInf->QueueMission(Mission::Enter, true);
				continue;
			}
		}

		// Combat Medics / Field Mechanics (Damage < 0): Heal wounded own OR allied infantry nearby!
		if (pInfWpn && pInfWpn->Damage < 0 && isIdle)
		{
			for (auto const pAllyInf : InfantryClass::Array)
			{
				if (!pAllyInf || pAllyInf == pInf || !pAllyInf->Owner || !pHouse->IsAlliedWith(pAllyInf->Owner) || pAllyInf->InLimbo || !pAllyInf->IsAlive)
					continue;
				if (pAllyInf->GetHealthPercentage() < 0.85 && pInf->DistanceFrom(pAllyInf) <= 2560)
				{
					pInf->SetTarget(pAllyInf);
					pInf->QueueMission(Mission::Attack, true);
					break;
				}
			}
			continue;
		}

		// Chrono Legionnaire (Temporal=yes) & Crazy Ivan (Ivan=yes): Prioritize high-cost enemy units/buildings!
		if (isIdle && ((pInfWpn && pInfWpn->Warhead && pInfWpn->Warhead->Temporal) || pInf->Type->Ivan))
		{
			TechnoClass* pBestSpecTarget = nullptr;
			int bestSpecCost = 800;
			for (auto const pEnemyU : UnitClass::Array)
			{
				if (!pEnemyU || pEnemyU->InLimbo || !pEnemyU->IsAlive || !pEnemyU->Owner || !pEnemyU->Type)
					continue;
				if (pEnemyU->Owner == pHouse || pHouse->IsAlliedWith(pEnemyU->Owner) || pEnemyU->Owner->IsNeutral())
					continue;
				if (pEnemyU->GetCell() && IsCellVisibleToAI(pHouse, pEnemyU->GetCell()) && pEnemyU->Type->Cost > bestSpecCost && pInf->DistanceFrom(pEnemyU) <= 4608)
				{
					bestSpecCost = pEnemyU->Type->Cost;
					pBestSpecTarget = pEnemyU;
				}
			}
			if (pBestSpecTarget)
			{
				pInf->SetTarget(pBestSpecTarget);
				pInf->QueueMission(Mission::Attack, true);
				continue;
			}
		}

		// Send idle Engineer to disarm Ivan Bombs on our own OR allied buildings/tanks, repair destroyed bridges,
		// or capture Neutral Tech Buildings (Oil Derricks, Tech Outposts, Airports) and damaged enemy buildings (respecting Multi-Engineer)
		if (pInf->Type->Engineer && isIdle)
		{
			// Disarm any attached Crazy Ivan Bomb on our own OR allied high-value buildings!
			bool disarmedBomb = false;
			for (auto const pAllyBld : BuildingClass::Array)
			{
				if (pAllyBld && !pAllyBld->InLimbo && pAllyBld->IsAlive && pAllyBld->Owner && pHouse->IsAlliedWith(pAllyBld->Owner)
					&& pAllyBld->AttachedBomb != nullptr && pInf->DistanceFrom(pAllyBld) <= 3584)
				{
					pInf->SetTarget(pAllyBld);
					pInf->QueueMission(Mission::Attack, true);
					disarmedBomb = true;
					break;
				}
			}
			if (disarmedBomb)
				continue;
			if (pTargetBridgeHut)
			{
				pInf->SetDestination(pTargetBridgeHut, true);
				pInf->QueueMission(Mission::Enter, true);
				pTargetBridgeHut = nullptr; // One engineer per hut is enough
				continue;
			}

			const double capLevel = RulesClass::Instance ? RulesClass::Instance->EngineerCaptureLevel : 1.0;
			for (auto const pBld : BuildingClass::Array)
			{
				if (!pBld || pBld->InLimbo || !pBld->IsAlive || !pBld->Type || !pBld->Owner)
					continue;

				if (!pBld->Type->Capturable || pBld->Owner == pHouse || pHouse->IsAlliedWith(pBld->Owner))
					continue;

				auto const pCell = pBld->GetCell();
				if (!pCell || !IsCellVisibleToAI(pHouse, pCell))
					continue;

				const bool isNeutralTech = pBld->Owner->IsNeutral();
				const bool canCaptureNow = isNeutralTech || (pBld->GetHealthPercentage() <= capLevel + 0.05);
				if (canCaptureNow && pInf->DistanceFrom(pBld) <= 6144)
				{
					pInf->SetDestination(pBld, true);
					pInf->QueueMission(Mission::Enter, true);
					break;
				}
			}
			continue;
		}

		// Spy Full-Spectrum Sabotage: Infiltrate Enemy Power Plants (Blackout!), Battle Labs (Secret Tech!), Superweapons, Refineries & Factories
		if (pInf->Type->Infiltrate && isIdle)
		{
			BuildingClass* pBestSpyTarget = nullptr;
			int bestSpyScore = -1;
			for (auto const pEnemyBld : BuildingClass::Array)
			{
				if (!pEnemyBld || pEnemyBld->InLimbo || !pEnemyBld->IsAlive || !pEnemyBld->Owner || !pEnemyBld->Type)
					continue;
				if (pHouse->IsAlliedWith(pEnemyBld->Owner) || pEnemyBld->Owner->IsNeutral())
					continue;

				int spyScore = 0;
				if (pEnemyBld->Type->SuperWeapon != -1)
					spyScore = 100; // Reset enemy Superweapon countdown!
				else if (pEnemyBld->Type->Refinery)
					spyScore = 90;  // Steal enemy credits!
				else if (pEnemyBld->Type->PowerBonus > 0)
					spyScore = 85;  // Black out enemy base defenses!
				else if (pEnemyBld->Type->Factory != AbstractType::None)
					spyScore = 80;  // Gain Veteran production!
				else if (pEnemyBld->Type->TechLevel >= 6)
					spyScore = 75;  // Battle Lab / Radar sabotage!

				if (spyScore > bestSpyScore)
				{
					bestSpyScore = spyScore;
					pBestSpyTarget = pEnemyBld;
				}
			}

			if (pBestSpyTarget)
			{
				pInf->SetDestination(pBestSpyTarget, true);
				pInf->QueueMission(Mission::Enter, true);
				pExt->AIBrain_InfiltratedEnemyMask |= (1 << std::clamp(pBestSpyTarget->Owner->ArrayIndex, 0, 7));
			}
			continue;
		}

		// Hijacker (VehicleThief=yes): Steal the most expensive visible enemy tank (Apocalypse, Mammoth Mk.II, Avatar, Prism Tank)!
		if (pInf->Type->VehicleThief && isIdle)
		{
			UnitClass* pBestStealTank = nullptr;
			int bestTankCost = 700;
			for (auto const pEnemyU : UnitClass::Array)
			{
				if (!pEnemyU || pEnemyU->InLimbo || !pEnemyU->IsAlive || !pEnemyU->Owner || !pEnemyU->Type)
					continue;
				if (pEnemyU->Owner == pHouse || pHouse->IsAlliedWith(pEnemyU->Owner) || pEnemyU->Owner->IsNeutral())
					continue;
				if (pEnemyU->Type->Naval || pEnemyU->Type->Organic || pEnemyU->ParasiteEatingMe != nullptr)
					continue;
				if (pEnemyU->GetCell() && IsCellVisibleToAI(pHouse, pEnemyU->GetCell()) && pEnemyU->Type->Cost > bestTankCost && pInf->DistanceFrom(pEnemyU) <= 5120)
				{
					bestTankCost = pEnemyU->Type->Cost;
					pBestStealTank = pEnemyU;
				}
			}

			if (pBestStealTank)
			{
				pInf->SetDestination(pBestStealTank, true);
				pInf->QueueMission(Mission::Enter, true);
				continue;
			}
		}

		// Guard Dogs (Doggie=yes): Patrol around ConYard / Superweapon / Radar to sniff out and maul enemy Spies & Engineers!
		if (pInf->Type->Doggie && isIdle && homeCoords != CellStruct::Empty && (Unsorted::CurrentFrame % 120 < 20))
		{
			if (auto const pHomeCell = MapClass::Instance.TryGetCellAt(homeCoords))
			{
				if (pInf->DistanceFrom(pHomeCell) > 2048)
				{
					pInf->SetDestination(pHomeCell, true);
					pInf->QueueMission(Mission::Area_Guard, true);
				}
			}
			continue;
		}

		// C4 Commandos (Tanya, Navy SEAL, Boris, Cyborg Commando): Demolish visible enemy Warships (if amphibious) or high-value enemy buildings!
		if (pInf->Type->C4 && isIdle)
		{
			TechnoClass* pBestC4Target = nullptr;
			int bestC4Dist = 5120;
			const bool canSwim = (pInf->Type->MovementZone == MovementZone::Amphibious || pInf->Type->MovementZone == MovementZone::AmphibiousDestroyer || pInf->Type->SpeedType == SpeedType::Hover);
			if (canSwim)
			{
				for (auto const pEnemyShip : UnitClass::Array)
				{
					if (!pEnemyShip || pEnemyShip->InLimbo || !pEnemyShip->IsAlive || !pEnemyShip->Owner || !pEnemyShip->Type || !pEnemyShip->Type->Naval)
						continue;
					if (pEnemyShip->Owner == pHouse || pHouse->IsAlliedWith(pEnemyShip->Owner) || pEnemyShip->Owner->IsNeutral())
						continue;
					if (pEnemyShip->GetCell() && IsCellVisibleToAI(pHouse, pEnemyShip->GetCell()))
					{
						const int d = pInf->DistanceFrom(pEnemyShip);
						if (d < bestC4Dist)
						{
							bestC4Dist = d;
							pBestC4Target = pEnemyShip;
						}
					}
				}
			}
			if (pBestC4Target)
			{
				pInf->SetTarget(pBestC4Target);
				pInf->QueueMission(Mission::Attack, true);
				continue;
			}
		}

		// IFV (Gunner=yes), Battle Fortress (OpenTopped=yes) & Subterranean/Ground APC Strike Team Boarding!
		if (isIdle && !pInf->Type->Infiltrate && (Unsorted::CurrentFrame % 90 < 20))
		{
			bool boardedCombatVehicle = false;
			for (auto const pCombVeh : UnitClass::Array)
			{
				if (!pCombVeh || pCombVeh->Owner != pHouse || pCombVeh->InLimbo || !pCombVeh->IsAlive || !pCombVeh->Type)
					continue;

				const bool isGunnerIFV = (!pInf->Type->Engineer && pCombVeh->Type->Gunner && pCombVeh->Passengers.NumPassengers == 0);
				const bool isOpenToppedFortress = (!pInf->Type->Engineer && pCombVeh->Type->OpenTopped && pCombVeh->Passengers.NumPassengers < pCombVeh->Type->Passengers);
				const bool isSubterraneanOrGroundAPC = (!pCombVeh->Type->Naval && !pCombVeh->Type->Gunner && !pCombVeh->Type->OpenTopped
					&& pCombVeh->Type->Passengers >= 3 && pCombVeh->Passengers.NumPassengers < pCombVeh->Type->Passengers
					&& (pCombVeh->Type->MovementZone == MovementZone::Subterrannean || pInf->Type->Engineer || pInf->Type->C4));

				if ((isGunnerIFV || isOpenToppedFortress || isSubterraneanOrGroundAPC) && pInf->DistanceFrom(pCombVeh) <= 2304)
				{
					pInf->SetDestination(pCombVeh, true);
					pInf->QueueMission(Mission::Enter, true);
					boardedCombatVehicle = true;
					break;
				}
			}
			if (boardedCombatVehicle)
				continue;
		}

		// Soviet Tesla Trooper Overpowering: Charge owned Tesla Coils so they stay powered during LowPower and gain bonus range/damage!
		if (pOverpowerCoil && isIdle && pInf->Type->Assaulter && pInf->DistanceFrom(pOverpowerCoil) <= 3072)
		{
			pInf->SetTarget(pOverpowerCoil);
			pInf->QueueMission(Mission::Attack, true);
			pOverpowerCoil = nullptr;
			continue;
		}

		// Yuri Bio-Reactor Power Surge: Enter Bio-Reactor with basic infantry during LowPower!
		if (pBioReactor && isIdle && pInf->Type->Cost <= 250 && pInf->DistanceFrom(pBioReactor) <= 3072)
		{
			pInf->SetDestination(pBioReactor, true);
			pInf->QueueMission(Mission::Enter, true);
			pBioReactor = nullptr;
			continue;
		}

		// Urban Garrisoning: Occupy nearby empty Civilian Buildings / Battle Bunkers with Occupier infantry (GI, Conscript, Initiate, Light Inf)
		if (pInf->Type->Occupier && isIdle && (Unsorted::CurrentFrame % 90 < 15))
		{
			for (auto const pBld : BuildingClass::Array)
			{
				if (!pBld || pBld->InLimbo || !pBld->IsAlive || !pBld->Type || !pBld->Type->CanBeOccupied)
					continue;

				if (pBld->CanBeOccupiedBy(pInf) && pBld->Occupants.Count < 4 && pInf->DistanceFrom(pBld) <= 2560)
				{
					pInf->SetDestination(pBld, true);
					pInf->QueueMission(Mission::Enter, true);
					break;
				}
			}
		}
	}

	// Tactical Bridge Demolition (Ambush enemy units crossing a bridge when Immune Bridges is OFF)
	if (scores[22] > 600 && RulesClass::Instance && RulesClass::Instance->BridgeStrength < 1000000)
	{
		CellClass* pTargetBridgeCell = nullptr;
		for (auto const pTechno : TechnoClass::Array)
		{
			if (!pTechno || pTechno->InLimbo || !pTechno->IsAlive || !pTechno->Owner)
				continue;

			if (pTechno->Owner == pHouse || pHouse->IsAlliedWith(pTechno->Owner) || pTechno->Owner->IsNeutral())
				continue;

			if (pTechno->OnBridge)
			{
				auto const pCell = pTechno->GetCell();
				if (pCell && pCell->ContainsBridge() && IsCellVisibleToAI(pHouse, pCell))
				{
					pTargetBridgeCell = pCell;
					break;
				}
			}
		}

		if (pTargetBridgeCell)
		{
			int assignedDemolishers = 0;
			for (auto const pUnit : UnitClass::Array)
			{
				if (!pUnit || pUnit->Owner != pHouse || pUnit->InLimbo || !pUnit->IsAlive || !pUnit->Type)
					continue;

				if (pUnit->Type->Harvester || pUnit->Type->ResourceGatherer || pUnit->OnBridge)
					continue;

				if (pUnit->DistanceFrom(pTargetBridgeCell) <= 3072) // Within 12 cells of the bridge
				{
					pUnit->SetTarget(pTargetBridgeCell);
					pUnit->QueueMission(Mission::Attack, true);
					if (++assignedDemolishers >= 4)
						break;
				}
			}
		}
	}

	// Autonomous Fog-of-War Scouting & Coordinated Multi-Wave Offensive Dispatch (Replaces legacy aimd.ini TeamScripts)
	BuildingClass* pVisibleEnemyBld = nullptr;
	CellClass* pFallbackEnemyCell = nullptr;

	for (auto const pEnemyBld : BuildingClass::Array)
	{
		if (!pEnemyBld || pEnemyBld->InLimbo || !pEnemyBld->IsAlive || !pEnemyBld->Owner || !pEnemyBld->Type || pEnemyBld->Type->Immune)
			continue;

		auto const pOwner = pEnemyBld->Owner;
		if (pOwner == pHouse || pHouse->IsAlliedWith(pOwner) || pOwner->IsNeutral() || pOwner->IsObserver() || pOwner->Defeated)
			continue;

		auto const pCell = pEnemyBld->GetCell();
		if (!pCell)
			continue;

		if (!pFallbackEnemyCell || pOwner->ArrayIndex == pHouse->EnemyHouseIndex)
			pFallbackEnemyCell = pCell;

		const int ownerIdx = std::clamp(pOwner->ArrayIndex, 0, 7);
		const bool visible = pHouse->SpySatActive
			|| IsCellVisibleToAI(pHouse, pCell)
			|| ((pExt->AIBrain_InfiltratedEnemyMask & (1 << ownerIdx)) != 0)
			|| (pOwner->WhoLastHurtMe == pHouse->ArrayIndex);

		if (visible)
		{
			pVisibleEnemyBld = pEnemyBld;
			if (pOwner->ArrayIndex == pHouse->EnemyHouseIndex)
				break;
		}
	}

	// Count idle combat units and infantry ready at base
	std::vector<UnitClass*> idleCombatUnits;
	std::vector<InfantryClass*> idleCombatInf;

	for (auto const pUnit : UnitClass::Array)
	{
		if (!pUnit || pUnit->Owner != pHouse || pUnit->InLimbo || !pUnit->IsAlive || !pUnit->Type)
			continue;

		if (pUnit->Type->Harvester || pUnit->Type->ResourceGatherer || (pUnit->Type->DeploysInto && pUnit->Type->DeploysInto->ConstructionYard))
			continue;

		if (pUnit->GetHealthPercentage() < 0.40)
			continue;

		if (pUnit->CurrentMission == Mission::Guard || pUnit->CurrentMission == Mission::Area_Guard)
			idleCombatUnits.push_back(pUnit);
	}

	for (auto const pInf : InfantryClass::Array)
	{
		if (!pInf || pInf->Owner != pHouse || pInf->InLimbo || !pInf->IsAlive || !pInf->Type)
			continue;

		if (pInf->Type->Engineer || pInf->Type->Infiltrate)
			continue;

		if (pInf->CurrentMission == Mission::Guard || pInf->CurrentMission == Mission::Area_Guard)
			idleCombatInf.push_back(pInf);
	}

	// If no enemy building is visible yet, send 1 fast scout unit/infantry every 15 seconds to uncover the fog of war
	if (!pVisibleEnemyBld && pFallbackEnemyCell && (Unsorted::CurrentFrame % 225 < 30))
	{
		if (!idleCombatUnits.empty())
		{
			auto const pScout = idleCombatUnits.front();
			pScout->SetDestination(pFallbackEnemyCell, true);
			pScout->QueueMission(Mission::AttackMove, true);
		}
		else if (!idleCombatInf.empty())
		{
			auto const pScoutInf = idleCombatInf.front();
			pScoutInf->SetDestination(pFallbackEnemyCell, true);
			pScoutInf->QueueMission(Mission::AttackMove, true);
		}
	}

	// Coordinated Offensive Strike Wave when strike force reaches archetype threshold
	const int arch = pExt->AIBrain_Archetype;
	const size_t minWaveUnits = (arch == 3) ? 3 : ((arch == 2 || arch == 4) ? 7 : 5);
	CellClass* pWaveTargetCell = pVisibleEnemyBld ? pVisibleEnemyBld->GetCell() : (Unsorted::CurrentFrame > 3600 ? pFallbackEnemyCell : nullptr);

	if (pWaveTargetCell && (idleCombatUnits.size() >= minWaveUnits || (idleCombatUnits.size() + idleCombatInf.size() >= minWaveUnits + 4)))
	{
		// Keep 1-2 guard units at base, send the rest in a coordinated AttackMove / Hunt wave
		const size_t reserveCount = idleCombatUnits.size() > 3 ? 1 : 0;
		for (size_t i = reserveCount; i < idleCombatUnits.size(); ++i)
		{
			auto const pU = idleCombatUnits[i];
			// Heavy Airships (Kirov / CABAL Leviathan: JumpJet && Strength >= 1000): Flank along the map edge first before diving onto the enemy base!
			if (pU->Type->JumpJet && pU->Type->Strength >= 1000 && pU->DistanceFrom(pWaveTargetCell) > 7680)
			{
				const CellStruct uCoords = pU->GetMapCoords();
				const CellStruct flankCoords { uCoords.X, pWaveTargetCell->MapCoords.Y };
				if (auto const pFlankCell = MapClass::Instance.TryGetCellAt(flankCoords))
				{
					pU->SetDestination(pFlankCell, true);
					pU->QueueMission(Mission::Move, true);
					continue;
				}
			}

			if (pVisibleEnemyBld && pU->Type->GetWeapon(0) && pU->Type->GetWeapon(0)->WeaponType)
			{
				pU->SetDestination(pWaveTargetCell, true);
				pU->QueueMission(Mission::AttackMove, true);
			}
			else
			{
				pU->QueueMission(Mission::Hunt, true);
			}
		}

		const size_t infReserve = idleCombatInf.size() > 4 ? 2 : 0;
		for (size_t i = infReserve; i < idleCombatInf.size(); ++i)
		{
			auto const pI = idleCombatInf[i];
			pI->SetDestination(pWaveTargetCell, true);
			pI->QueueMission(Mission::AttackMove, true);
		}

		Debug::Log(
			"[NeuralAI] Frame %d | House #%d (%s) launched Offensive Wave (%zu Vehicles + %zu Infantry) -> Target Cell (%d,%d)\n",
			Unsorted::CurrentFrame, pHouse->ArrayIndex,
			(pHouse->Type && pHouse->Type->ID[0]) ? pHouse->Type->ID : "AI",
			idleCombatUnits.size() - reserveCount, idleCombatInf.size() - infReserve,
			pWaveTargetCell->MapCoords.X, pWaveTargetCell->MapCoords.Y);
	}

	// Amphibious Island Invasion, Subterranean/Ground APC Surgical Strike & MCV/Nanocore Island Colonization
	CellClass* pIslandGoalCell = pWaveTargetCell;
	int targetIslandZone = -1;
	if (pIslandGoalCell)
		targetIslandZone = MapClass::Instance.GetMovementZoneType(pIslandGoalCell->MapCoords, MovementZone::Normal, false);

	const bool enemyOnDisconnectedIsland = (homeGroundZone >= 0 && targetIslandZone >= 0 && targetIslandZone != homeGroundZone);

	for (auto const pTrans : UnitClass::Array)
	{
		if (!pTrans || pTrans->Owner != pHouse || pTrans->InLimbo || !pTrans->IsAlive || !pTrans->Type)
			continue;

		// Subterranean APC / Ground APC Strike Team: If carrying Engineers or >= 3 Infantry, burrow/drive to enemy base and Unload!
		if (!IsAmphibiousOrAirTransport(pTrans->Type) && pTrans->Type->Passengers >= 3 && !pTrans->Type->OpenTopped && !pTrans->Type->Gunner)
		{
			if (pTrans->Passengers.NumPassengers > 0 && pVisibleEnemyBld)
			{
				bool hasEngineerOrFullSquad = (pTrans->Passengers.NumPassengers >= 3);
				for (FootClass* pPass = pTrans->Passengers.GetFirstPassenger(); pPass != nullptr; pPass = abstract_cast<FootClass*>(pPass->NextObject))
				{
					if (auto const pPassInf = abstract_cast<InfantryClass*>(pPass))
					{
						if (pPassInf->Type && (pPassInf->Type->Engineer || pPassInf->Type->C4))
						{
							hasEngineerOrFullSquad = true;
							break;
						}
					}
				}

				if (hasEngineerOrFullSquad)
				{
					if (pTrans->DistanceFrom(pVisibleEnemyBld) <= 640)
					{
						if (pTrans->CurrentMission != Mission::Unload)
							pTrans->QueueMission(Mission::Unload, true);
					}
					else if (pTrans->CurrentMission == Mission::Guard || pTrans->CurrentMission == Mission::Area_Guard)
					{
						if (auto const pEnemyCell = pVisibleEnemyBld->GetCell())
						{
							pTrans->SetDestination(pEnemyCell, true);
							pTrans->QueueMission(Mission::Move, true);
						}
					}
				}
			}
			continue;
		}

		if (!IsAmphibiousOrAirTransport(pTrans->Type))
			continue;

		const int numPass = pTrans->Passengers.NumPassengers;
		const int totalPassSize = pTrans->Passengers.GetTotalSize();

		bool carryingMCV = false;
		for (FootClass* pPass = pTrans->Passengers.GetFirstPassenger(); pPass != nullptr; pPass = abstract_cast<FootClass*>(pPass->NextObject))
		{
			if (auto const pPassUnit = abstract_cast<UnitClass*>(pPass))
			{
				if (pPassUnit->Type && pPassUnit->Type->DeploysInto != nullptr)
				{
					carryingMCV = true; // MCV or Red Alert 3-style CABAL Expansion Nanocore!
					break;
				}
			}
		}

		const bool isReadyToSail = carryingMCV || numPass >= 3 || totalPassSize >= 6;
		const CellStruct transCoords = pTrans->GetMapCoords();
		const int transZone = MapClass::Instance.GetMovementZoneType(transCoords, MovementZone::Normal, false);

		if (isReadyToSail && pIslandGoalCell)
		{
			CellClass* pSafeLanding = FindSafeIslandLandingCell(pHouse, pIslandGoalCell->MapCoords, targetIslandZone, carryingMCV);
			if (pSafeLanding)
			{
				auto const pTransCell = pTrans->GetCell();
				const bool onTargetIslandLand = (targetIslandZone >= 0 && transZone == targetIslandZone && pTransCell && pTransCell->LandType != LandType::Water);
				if (onTargetIslandLand && pTrans->DistanceFrom(pSafeLanding) <= 1024)
				{
					if (pTrans->CurrentMission != Mission::Unload)
						pTrans->QueueMission(Mission::Unload, true);
				}
				else if (pTrans->CurrentMission == Mission::Guard || pTrans->CurrentMission == Mission::Area_Guard || (Unsorted::CurrentFrame % 120 < 30))
				{
					pTrans->SetDestination(pSafeLanding, true);
					pTrans->QueueMission(Mission::Move, true);
				}
			}
		}
		else if (!isReadyToSail && (enemyOnDisconnectedIsland || scores[28] > 600))
		{
			// First ensure the empty Hover Transport comes ashore onto our homeGroundZone so ground vehicles/infantry/MCVs can board it!
			auto const pTransCell = pTrans->GetCell();
			if (pTransCell && pTransCell->LandType == LandType::Water && homeCoords != CellStruct::Empty)
			{
				CellClass* pHomeBeach = FindSafeIslandLandingCell(pHouse, homeCoords, homeGroundZone, false);
				if (pHomeBeach && (pTrans->CurrentMission == Mission::Guard || pTrans->CurrentMission == Mission::Area_Guard))
				{
					pTrans->SetDestination(pHomeBeach, true);
					pTrans->QueueMission(Mission::Move, true);
				}
				continue;
			}

			// Load nearby idle combat vehicles, Engineers, and infantry into the Amphibious Transport
			int boardedNow = numPass;
			int sizeNow = totalPassSize;
			for (auto const pV : idleCombatUnits)
			{
				const int vehSize = static_cast<int>(pV->Type->TechnoTypeClass::Size);
				if (boardedNow >= pTrans->Type->Passengers || (sizeNow + vehSize) > pTrans->Type->Passengers)
					break;

				if (!IsAmphibiousOrAirTransport(pV->Type) && pV->Type->TechnoTypeClass::Size <= pTrans->Type->SizeLimit && pV->DistanceFrom(pTrans) <= 4608)
				{
					pV->SetDestination(pTrans, true);
					pV->QueueMission(Mission::Enter, true);
					boardedNow++;
					sizeNow += vehSize;
				}
			}

			for (auto const pI : idleCombatInf)
			{
				if (boardedNow >= pTrans->Type->Passengers || (sizeNow + 1) > pTrans->Type->Passengers)
					break;

				if (pI->DistanceFrom(pTrans) <= 4608)
				{
					pI->SetDestination(pTrans, true);
					pI->QueueMission(Mission::Enter, true);
					boardedNow++;
					sizeNow += 1;
				}
			}
		}
	}

	// Dispatch loaded/idle combat Aircraft, Carryall «Matryoshka» Airlift & Emergency Sell-Dodge / ConYard Undeploy
	for (auto const pAir : AircraftClass::Array)
	{
		if (!pAir || pAir->Owner != pHouse || pAir->InLimbo || !pAir->IsAlive || !pAir->Type)
			continue;

		const bool isAirIdle = (pAir->CurrentMission == Mission::Guard || pAir->CurrentMission == Mission::Area_Guard || pAir->CurrentMission == Mission::Sleep);

		// Carryall «Matryoshka / Muñeca Rusa» Airlift: Pick up loaded APCs, Expansion Nanocores/MCVs, or rescue critically damaged tanks!
		if (pAir->Type->Carryall && isAirIdle && pAir->Passengers.NumPassengers == 0 && (Unsorted::CurrentFrame % 90 < 20))
		{
			for (auto const pU : UnitClass::Array)
			{
				if (!pU || pU->Owner != pHouse || pU->InLimbo || !pU->IsAlive || !pU->Type || pU->Type->Naval)
					continue;

				const bool isCriticallyDamagedTank = (pRepairDepot && pU->Type->Cost >= 1000 && pU->GetHealthPercentage() < 0.40);
				const bool isLoadedStrikeAPC = (pU->Type->Passengers >= 3 && pU->Passengers.NumPassengers >= 2 && pWaveTargetCell);
				if (isCriticallyDamagedTank || isLoadedStrikeAPC)
				{
					pAir->SetTarget(pU);
					pAir->QueueMission(Mission::Capture, true);
					break;
				}
			}
			continue;
		}

		if (pVisibleEnemyBld && pAir->Ammo > 0 && isAirIdle)
		{
			pAir->SetTarget(pVisibleEnemyBld);
			pAir->QueueMission(Mission::Attack, true);
		}
	}

	// Emergency Sell-Dodge (refund 50% cash + survivor infantry before final lethal blow) & ConYard Emergency Undeploy into MCV!
	if (Unsorted::CurrentFrame % 30 < 10)
	{
		for (auto const pOwnBld : pHouse->Buildings)
		{
			if (!pOwnBld || pOwnBld->InLimbo || !pOwnBld->IsAlive || !pOwnBld->Type || pOwnBld->IsIronCurtained())
				continue;

			const double hpPct = pOwnBld->GetHealthPercentage();
			if (hpPct < 0.14 && pOwnBld->Type->ConstructionYard && pHouse->NumConYards == 1 && pOwnBld->Type->UndeploysInto != nullptr)
			{
				// Last ConYard is about to be destroyed! Pack back into an MCV to survive!
				if (pOwnBld->CurrentMission != Mission::Unload)
					pOwnBld->QueueMission(Mission::Unload, true);
			}
			else if (hpPct < 0.09 && !pOwnBld->Type->ConstructionYard && pOwnBld->Type->Cost >= 500 && pOwnBld->CurrentMission != Mission::Selling)
			{
				// Classic C&C Sell-Dodge right before destruction!
				pOwnBld->QueueMission(Mission::Selling, true);
			}
		}
	}

	// CABAL / Faction Research & Tech-Upgrade Unlocks (PreUpgr_SW* / LimboUpgrade_SW* where DisableableFromShell=no)
	if (homeCoords != CellStruct::Empty && (Unsorted::CurrentFrame % 120 < 25))
	{
		for (auto const pSuper : pHouse->Supers)
		{
			if (!pSuper || !pSuper->IsReady || !pSuper->Type)
				continue;

			if (!pSuper->Type->DisableableFromShell && pSuper->Type->Type != SuperWeaponType::ChronoWarp)
			{
				pSuper->Launch(homeCoords, false);
				break;
			}
		}
	}

	// Frontline Primary Factory Switching & Charged Superweapon Execution
	if (pWaveTargetCell && (Unsorted::CurrentFrame % 150 < 30))
	{
		BuildingClass* pBestWarFac = nullptr;
		BuildingClass* pBestBarracks = nullptr;
		int bestWarDist = 999999;
		int bestBarDist = 999999;

		for (auto const pBld : pHouse->Buildings)
		{
			if (!pBld || pBld->InLimbo || !pBld->IsAlive || !pBld->Type)
				continue;

			if (pBld->Type->Factory == AbstractType::UnitType && !pBld->Type->Naval)
			{
				const int d = pBld->DistanceFrom(pWaveTargetCell);
				if (d < bestWarDist)
				{
					bestWarDist = d;
					pBestWarFac = pBld;
				}
			}
			else if (pBld->Type->Factory == AbstractType::InfantryType)
			{
				const int d = pBld->DistanceFrom(pWaveTargetCell);
				if (d < bestBarDist)
				{
					bestBarDist = d;
					pBestBarracks = pBld;
				}
			}
		}

		if (pBestWarFac && !pBestWarFac->IsPrimaryFactory)
		{
			for (auto const pBld : pHouse->Buildings)
			{
				if (pBld && pBld->Type && pBld->Type->Factory == AbstractType::UnitType && !pBld->Type->Naval)
					pBld->IsPrimaryFactory = (pBld == pBestWarFac);
			}
		}

		if (pBestBarracks && !pBestBarracks->IsPrimaryFactory)
		{
			for (auto const pBld : pHouse->Buildings)
			{
				if (pBld && pBld->Type && pBld->Type->Factory == AbstractType::InfantryType)
					pBld->IsPrimaryFactory = (pBld == pBestBarracks);
			}
		}

		if (GameModeOptionsClass::Instance.SWAllowed)
		{
			SuperClass* pChronoWarp = pHouse->FindSuperWeapon(SuperWeaponType::ChronoWarp);

			for (auto const pSuper : pHouse->Supers)
			{
				if (!pSuper || !pSuper->IsReady || !pSuper->Type)
					continue;

				const SuperWeaponType swType = pSuper->Type->Type;
				if (swType == SuperWeaponType::ChronoWarp)
					continue; // Fired as the second step of ChronoSphere

				// A) IRON CURTAIN & FORCE SHIELD: Defensive Emergency Save OR Offensive Invulnerable Tank Assault
				if (swType == SuperWeaponType::IronCurtain || swType == SuperWeaponType::ForceShield)
				{
					CellStruct icTargetCell = CellStruct::Empty;

					// Defensive save: check if our own OR an ALLIED ConYard, Superweapon, or War Factory is under heavy attack (< 75% HP)
					for (auto const pFriendlyBld : BuildingClass::Array)
					{
						if (!pFriendlyBld || pFriendlyBld->InLimbo || !pFriendlyBld->IsAlive || !pFriendlyBld->Owner || !pFriendlyBld->Type || pFriendlyBld->IsIronCurtained())
							continue;
						if (!pHouse->IsAlliedWith(pFriendlyBld->Owner) || pFriendlyBld->Owner->IsNeutral())
							continue;

						const bool isCriticalBld = pFriendlyBld->Type->ConstructionYard
							|| pFriendlyBld->Type->Factory == AbstractType::UnitType
							|| pFriendlyBld->Type->SuperWeapon != -1;
						if (isCriticalBld && pFriendlyBld->GetHealthPercentage() < 0.75)
						{
							icTargetCell = pFriendlyBld->GetMapCoords();
							break;
						}
					}

					// Offensive Invulnerability: cast Iron Curtain on our own OR an ALLIED attacking tank cluster (2+ vehicles near enemy)
					if (icTargetCell == CellStruct::Empty && swType == SuperWeaponType::IronCurtain)
					{
						int bestClusterCount = 0;
						for (auto const pU : UnitClass::Array)
						{
							if (!pU || !pU->Owner || !pHouse->IsAlliedWith(pU->Owner) || pU->Owner->IsNeutral() || pU->InLimbo || !pU->IsAlive || !pU->Type || pU->IsIronCurtained())
								continue;
							if (pU->Type->Harvester || pU->Type->ResourceGatherer || pU->Type->Organic)
								continue;

							const bool isEngagingEnemy = (pU->Target != nullptr)
								|| (pVisibleEnemyBld && pU->DistanceFrom(pVisibleEnemyBld) <= 4096);
							if (!isEngagingEnemy)
								continue;

							const CellStruct uCell = pU->GetMapCoords();
							int neighbors = 0;
							for (auto const pOtherU : UnitClass::Array)
							{
								if (!pOtherU || !pOtherU->Owner || !pHouse->IsAlliedWith(pOtherU->Owner) || pOtherU->InLimbo || !pOtherU->IsAlive || !pOtherU->Type)
									continue;
								if (pOtherU->Type->Harvester || pOtherU->Type->Organic)
									continue;
								const CellStruct oCell = pOtherU->GetMapCoords();
								if (std::abs(uCell.X - oCell.X) <= 1 && std::abs(uCell.Y - oCell.Y) <= 1)
									neighbors++;
							}

							if (neighbors >= 2 && neighbors > bestClusterCount)
							{
								bestClusterCount = neighbors;
								icTargetCell = uCell;
							}
						}
					}

					if (icTargetCell != CellStruct::Empty)
					{
						pSuper->Launch(icTargetCell, false);
						break;
					}
					continue;
				}

				// B) CHRONOSPHERE + CHRONOWARP:
				//    1) Teleport Iron-Curtained tank squad or Expansion MCV to enemy base / island
				//    2) Teleport Enemy Player A's tanks into Enemy Player B's base to cause mutual attrition in FFA
				//    3) Drown Enemy Ground Tanks/MCVs by teleporting them into Water
				//    4) Destroy Enemy Naval Warships by teleporting them onto dry Land
				if (swType == SuperWeaponType::ChronoSphere && pChronoWarp)
				{
					CellStruct chronoSrc = CellStruct::Empty;
					CellStruct chronoDst = CellStruct::Empty;

					// Find a known water cell (for drowning enemy ground tanks) and a known clear land cell (for beaching enemy ships)
					CellStruct knownWaterCell = CellStruct::Empty;
					CellStruct knownDryLandCell = CellStruct::Empty;
					if (homeCoords != CellStruct::Empty)
					{
						for (int dy = -40; dy <= 40; dy += 4)
						{
							for (int dx = -40; dx <= 40; dx += 4)
							{
								const CellStruct tc { static_cast<short>(homeCoords.X + dx), static_cast<short>(homeCoords.Y + dy) };
								auto const pC = MapClass::Instance.TryGetCellAt(tc);
								if (!pC || !IsCellVisibleToAI(pHouse, pC) || pC->GetBuilding() != nullptr)
									continue;

								if (knownWaterCell == CellStruct::Empty && pC->LandType == LandType::Water && !pC->ContainsBridge())
									knownWaterCell = tc;
								if (knownDryLandCell == CellStruct::Empty && pC->LandType == LandType::Clear && pC->FirstObject == nullptr && std::abs(dx) >= 12)
									knownDryLandCell = tc;
							}
						}
					}

					// Mode 1: Teleport our own Iron-Curtained tank squad or distant Expansion MCV directly to the target beachhead!
					for (auto const pU : UnitClass::Array)
					{
						if (!pU || pU->Owner != pHouse || pU->InLimbo || !pU->IsAlive || !pU->Type)
							continue;

						const bool isExpansionMCV = (pU->Type->DeploysInto && pU->Type->DeploysInto->ConstructionYard && pHouse->NumConYards >= 1);
						const bool isIronCurtainedTank = (pU->IsIronCurtained() && !pU->Type->Harvester && !pU->Type->Organic);

						if ((isIronCurtainedTank || isExpansionMCV) && pWaveTargetCell)
						{
							CellClass* pDropCell = FindSafeIslandLandingCell(pHouse, pWaveTargetCell->MapCoords, targetIslandZone, isExpansionMCV);
							if (pDropCell && pU->DistanceFrom(pDropCell) >= 5120)
							{
								chronoSrc = pU->GetMapCoords();
								chronoDst = pDropCell->MapCoords;
								break;
							}
						}
					}

					// Mode 2, 3 & 4: Offensive Chronoshift against visible enemy vehicles/ships
					if (chronoSrc == CellStruct::Empty)
					{
						// Check if there is a SECOND rival enemy house with a visible building (for Enemy-vs-Enemy FFA attrition!)
						CellStruct rivalEnemyBaseCell = CellStruct::Empty;
						int rivalEnemyHouseIdx = -1;
						for (auto const pBld : BuildingClass::Array)
						{
							if (!pBld || pBld->InLimbo || !pBld->IsAlive || !pBld->Owner || !pBld->Type || pBld->Type->Immune)
								continue;
							auto const pOwn = pBld->Owner;
							if (pOwn == pHouse || pHouse->IsAlliedWith(pOwn) || pOwn->IsNeutral() || pOwn->IsObserver() || pOwn->Defeated)
								continue;
							if (pOwn->ArrayIndex != pHouse->EnemyHouseIndex && pBld->GetCell() && IsCellVisibleToAI(pHouse, pBld->GetCell()))
							{
								CellClass* pAdj = FindSafeIslandLandingCell(pHouse, pBld->GetMapCoords(), -1, false);
								if (pAdj)
								{
									rivalEnemyBaseCell = pAdj->MapCoords;
									rivalEnemyHouseIdx = pOwn->ArrayIndex;
									break;
								}
							}
						}

						for (auto const pEnemyU : UnitClass::Array)
						{
							if (!pEnemyU || pEnemyU->InLimbo || !pEnemyU->IsAlive || !pEnemyU->Owner || !pEnemyU->Type)
								continue;
							if (pEnemyU->Owner == pHouse || pHouse->IsAlliedWith(pEnemyU->Owner) || pEnemyU->Owner->IsNeutral())
								continue;
							if (pEnemyU->IsIronCurtained() || pEnemyU->Type->Teleporter || pEnemyU->Type->Organic)
								continue;

							auto const pECell = pEnemyU->GetCell();
							if (!pECell || !IsCellVisibleToAI(pHouse, pECell))
								continue;

							const bool isPureGroundUnit = !pEnemyU->Type->Naval
								&& pEnemyU->Type->SpeedType != SpeedType::Hover
								&& pEnemyU->Type->MovementZone != MovementZone::Amphibious;
							const bool isPureNavalShip = pEnemyU->Type->Naval
								&& pEnemyU->Type->SpeedType != SpeedType::Hover
								&& pEnemyU->Type->MovementZone != MovementZone::Amphibious;

							// Mode 2: Pit Enemy A's ground tanks directly inside Enemy B's base to trigger mutual attrition!
							if (isPureGroundUnit && rivalEnemyBaseCell != CellStruct::Empty && pEnemyU->Owner->ArrayIndex != rivalEnemyHouseIdx)
							{
								chronoSrc = pEnemyU->GetMapCoords();
								chronoDst = rivalEnemyBaseCell;
								break;
							}
							// Mode 3: Drown Enemy ground tanks/MCVs in water!
							if (isPureGroundUnit && knownWaterCell != CellStruct::Empty && pEnemyU->Type->Cost >= 700)
							{
								chronoSrc = pEnemyU->GetMapCoords();
								chronoDst = knownWaterCell;
								break;
							}
							// Mode 4: Beach Enemy naval warships onto dry land!
							if (isPureNavalShip && knownDryLandCell != CellStruct::Empty)
							{
								chronoSrc = pEnemyU->GetMapCoords();
								chronoDst = knownDryLandCell;
								break;
							}
						}
					}

					if (chronoSrc != CellStruct::Empty && chronoDst != CellStruct::Empty)
					{
						pSuper->Launch(chronoSrc, false);
						pSuper->ChronoMapCoords = chronoSrc;
						pChronoWarp->ChronoMapCoords = chronoSrc;
						pChronoWarp->Launch(chronoDst, false);
						break;
					}
					continue;
				}

				// C) YURI GENETIC MUTATOR: Mutate enemy infantry clusters into Brutes!
				if (swType == SuperWeaponType::GeneticMutator)
				{
					CellStruct bestMutateCell = CellStruct::Empty;
					int bestInfCluster = 1;
					for (auto const pEnemyInf : InfantryClass::Array)
					{
						if (!pEnemyInf || pEnemyInf->InLimbo || !pEnemyInf->IsAlive || !pEnemyInf->Owner || !pEnemyInf->Type)
							continue;
						if (pEnemyInf->Owner == pHouse || pHouse->IsAlliedWith(pEnemyInf->Owner) || pEnemyInf->Owner->IsNeutral())
							continue;
						if (pEnemyInf->GetCell() && IsCellVisibleToAI(pHouse, pEnemyInf->GetCell()))
						{
							const CellStruct ic = pEnemyInf->GetMapCoords();
							int cnt = 0;
							for (auto const pOtherInf : InfantryClass::Array)
							{
								if (!pOtherInf || pOtherInf->InLimbo || !pOtherInf->IsAlive || !pOtherInf->Owner)
									continue;
								if (pOtherInf->Owner == pHouse || pHouse->IsAlliedWith(pOtherInf->Owner))
									continue;
								const CellStruct oc = pOtherInf->GetMapCoords();
								if (std::abs(ic.X - oc.X) <= 1 && std::abs(ic.Y - oc.Y) <= 1)
									cnt++;
							}
							if (cnt > bestInfCluster)
							{
								bestInfCluster = cnt;
								bestMutateCell = ic;
							}
						}
					}
					if (bestMutateCell != CellStruct::Empty)
					{
						pSuper->Launch(bestMutateCell, false);
						break;
					}
				}

				// D) YURI PSYCHIC DOMINATOR: Permanently capture clusters of expensive enemy tanks/MCVs while crushing nearby structures!
				if (swType == SuperWeaponType::PsychicDominator)
				{
					CellStruct bestDomCell = CellStruct::Empty;
					int bestDomValue = 1200;
					for (auto const pEnemyU : UnitClass::Array)
					{
						if (!pEnemyU || pEnemyU->InLimbo || !pEnemyU->IsAlive || !pEnemyU->Owner || !pEnemyU->Type)
							continue;
						if (pEnemyU->Owner == pHouse || pHouse->IsAlliedWith(pEnemyU->Owner) || pEnemyU->Owner->IsNeutral())
							continue;
						if (pEnemyU->GetCell() && IsCellVisibleToAI(pHouse, pEnemyU->GetCell()) && !pEnemyU->Type->ImmuneToPsionics)
						{
							const CellStruct uc = pEnemyU->GetMapCoords();
							int val = 0;
							for (auto const pOtherU : UnitClass::Array)
							{
								if (!pOtherU || pOtherU->InLimbo || !pOtherU->IsAlive || !pOtherU->Owner || !pOtherU->Type)
									continue;
								if (pOtherU->Owner == pHouse || pHouse->IsAlliedWith(pOtherU->Owner))
									continue;
								const CellStruct oc = pOtherU->GetMapCoords();
								if (std::abs(uc.X - oc.X) <= 1 && std::abs(uc.Y - oc.Y) <= 1)
									val += pOtherU->Type->Cost;
							}
							if (val > bestDomValue)
							{
								bestDomValue = val;
								bestDomCell = uc;
							}
						}
					}
					if (bestDomCell != CellStruct::Empty)
					{
						pSuper->Launch(bestDomCell, false);
						break;
					}
				}

				// E) Reconnaissance Superweapons (Psychic Reveal / Spy Plane): Uncover shrouded enemy base candidates!
				if ((swType == SuperWeaponType::PsychicReveal || swType == SuperWeaponType::SpyPlane) && pFallbackEnemyCell)
				{
					pSuper->Launch(pFallbackEnemyCell->MapCoords, false);
					break;
				}

				// F) Standard Offensive/Support Superweapons (Nuke, Lightning Storm, Ion Cannon, ParaDrop, etc.)
				if (pVisibleEnemyBld)
				{
					pSuper->Launch(pVisibleEnemyBld->GetMapCoords(), false);
					break;
				}
			}
		}
	}
}

namespace
{
	// Evaluates buildability strictly under Human Player rules:
	// - Requires valid human TechLevel (0..10), excluding TechLevel=-1 AI script hacks/limbo dummies and TechLevel=11 disabled types
	// - Excludes legacy *_AI hack clones and TEST_BUILDING
	// - Temporarily sets IsHumanPlayer=true and AIBuildThis=true during HouseClass::CanBuild so Ares/YRpp enforce real
	//   human Prerequisites, Prerequisite.Negative, Owner/ForbiddenHouses, BuildLimit, and PowersUpBuilding instead of AIBuildThis/PrerequisiteOverride
	bool CanBuildUnderHumanRules(HouseClass* pHouse, TechnoTypeClass* pType)
	{
		if (!pHouse || !pType)
			return false;

		if (pType->TechLevel < 0 || pType->TechLevel > 10 || pType->TechLevel > pHouse->TechLevel)
			return false;

		const char* id = pType->ID;
		if (!id || !id[0])
			return false;

		const size_t idLen = std::strlen(id);
		if (idLen > 3 && _stricmp(id + idLen - 3, "_AI") == 0)
			return false;

		if (_stricmp(id, "TEST_BUILDING") == 0 || std::strstr(id, "_FAKE") != nullptr)
			return false;

		auto const pBld = abstract_cast<BuildingTypeClass*>(pType);
		if (pBld && pBld->SuperWeapon >= 0 && pBld->SuperWeapon < SuperWeaponTypeClass::Array.Count)
		{
			if (RulesClass::Instance->BuildTech.FindItemIndex(pBld) == -1)
			{
				if (auto const pSW = SuperWeaponTypeClass::Array.GetItem(pBld->SuperWeapon))
				{
					if (auto const pSWExt = SWTypeExt::Fetch(pSW))
					{
						if (!pSWExt->SW_AllowAI)
							return false; // Respects SW01_Computers.ini .. SW14_Computers.ini (SW.AllowAI=no)
					}
				}
			}
		}

		// Evaluate full Phobos/Ares human prerequisites:
		// [GenericPrerequisites], Prerequisite<Name>Alternate, Prerequisite.Lists, Prerequisite.Negative,
		// Owner/RequiredHouses/ForbiddenHouses (with ParentCountry), ConsideredSecretLabTech, BuildLimit/BuildLimitGroup,
		// and PowersUpBuilding / PowersUp.Buildings parent upgrade slots.
		if (!HouseExt::PrerequisitesMet(pHouse, pType))
			return false;

		const bool prevHuman = pHouse->IsHumanPlayer;
		const bool prevAIBuild = pBld ? pBld->AIBuildThis : true;

		pHouse->IsHumanPlayer = true;
		if (pBld)
			pBld->AIBuildThis = true;

		const CanBuildResult res = pHouse->CanBuild(pType, false, false);

		pHouse->IsHumanPlayer = prevHuman;
		if (pBld)
			pBld->AIBuildThis = prevAIBuild;

		return res == CanBuildResult::Buildable;
	}
}

int NeuralAI::UpdateExpertAI(HouseClass* pHouse)
{
	if (!NeuralAI::IsEnabledForHouse(pHouse))
		return 30;

	auto const pExt = HouseExt::Fetch(pHouse);
	const int diffIdx = pHouse->GetAIDifficultyIndex();
	const int evalDelay = (diffIdx == 0) ? 15 : ((diffIdx == 1) ? 45 : 105);

	std::array<int8_t, InputVectorSize> features;
	std::array<int16_t, OutputVectorSize> scores;

	NeuralAI::ExtractFogFilteredFeatures(pHouse, features);
	NeuralAI::RunDeterministicMoE(pHouse, features, scores);
	pExt->AIBrain_LastInferenceFrame = Unsorted::CurrentFrame;

	if (diffIdx <= 1)
		NeuralAI::ExecuteTacticalAndSpecOpsMicro(pHouse, scores);

	// Select next targets for Building, Unit, Infantry, and Aircraft queues
	const int money = pHouse->Available_Money();

	// Building Target Selection
	if (NeuralAI::IsCampaignRestricted(pHouse))
	{
		// Campaign restriction: only build what is in Base.BaseNodes
		pExt->AIBrain_NextBuildingTarget = -1;
		for (int i = 0; i < pHouse->Base.BaseNodes.Count; ++i)
		{
			const auto& node = pHouse->Base.BaseNodes.Items[i];
			if (!node.Placed && node.BuildingTypeIndex >= 0 && node.BuildingTypeIndex < BuildingTypeClass::Array.Count)
			{
				auto const pBldType = BuildingTypeClass::Array.GetItem(node.BuildingTypeIndex);
				if (pBldType && pHouse->CanBuild(pBldType, false, false) == CanBuildResult::Buildable)
				{
					pExt->AIBrain_NextBuildingTarget = node.BuildingTypeIndex;
					break;
				}
			}
		}
	}
	else
	{
		int bestBldIdx = -1;
		int bestBldScore = -32000;

		// Count current owned macro & defense structures so the AI builds a balanced macro base
		// (Power -> Refinery -> Barracks -> War Factory -> 2nd Refinery -> Radar -> Battle Lab -> Superweapons/High-Tier Defenses)
		// and NEVER spams an endless sea of cheap Tier-1 Pillboxes/Turrets!
		int ownedRefineries = 0;
		int ownedBarracks = 0;
		int ownedWarFactories = 0;
		int ownedTechStructures = 0;
		int totalGroundDefenses = 0;
		int totalAADefenses = 0;

		for (auto const pOwnBld : pHouse->Buildings)
		{
			if (!pOwnBld || pOwnBld->InLimbo || !pOwnBld->IsAlive || !pOwnBld->Type)
				continue;

			auto const pOT = pOwnBld->Type;
			if (pOT->Refinery || pOT->ResourceGatherer || pOT->ResourceDestination)
				ownedRefineries++;
			else if (pOT->Factory == AbstractType::InfantryType)
				ownedBarracks++;
			else if (pOT->Factory == AbstractType::UnitType && !pOT->Naval)
				ownedWarFactories++;
			else if (pOT->BuildCat == BuildCat::Combat)
			{
				const auto pWS = pOT->GetWeapon(0);
				const auto pW = pWS ? pWS->WeaponType : nullptr;
				const bool isPureAA = pW && pW->Projectile && pW->Projectile->AA && !pW->Projectile->AG;
				if (isPureAA)
					totalAADefenses++;
				else
					totalGroundDefenses++;
			}
			else if (pOT->SpySat || pOT->OrePurifier || (pOT->PowerBonus <= pOT->PowerDrain && !pOT->ConstructionYard && pOT->Storage == 0 && !pOT->UnitRepair))
			{
				ownedTechStructures++;
			}
		}

		// Also count mobile Slave Miners (e.g. Yuri's SMIN when undeployed from YAREFN to harvest ore) as owned Refineries
		for (auto const pOwnUnit : UnitClass::Array)
		{
			if (!pOwnUnit || pOwnUnit->Owner != pHouse || pOwnUnit->InLimbo || !pOwnUnit->IsAlive || !pOwnUnit->Type)
				continue;

			if (auto const pDeploys = pOwnUnit->Type->DeploysInto)
			{
				if (pDeploys->Refinery || pDeploys->ResourceGatherer || pDeploys->ResourceDestination)
					ownedRefineries++;
			}
		}

		const bool hasCoreEconomyAndProduction = (ownedRefineries >= 1 && ownedBarracks >= 1 && ownedWarFactories >= 1);
		const int maxGroundDefenses = std::min(6, 1 + ownedRefineries + ownedWarFactories + ownedTechStructures);
		const int maxAADefenses = std::min(5, 1 + ownedWarFactories + ownedTechStructures);

		for (auto const pBld : BuildingTypeClass::Array)
		{
			if (!pBld || pBld->Wall || pBld->Gate || pBld->LaserFence || pBld->LaserFencePost || pBld->ToTile)
				continue;

			if (HouseExt::IsDisabledFromShell(pHouse, pBld))
				continue;

			if (!CanBuildUnderHumanRules(pHouse, pBld))
				continue;

			if (pBld->GetActualCost(pHouse) > money + 1200)
				continue;

			const int owned = BuildingTypeExt::CountOwnedNowWithDeployOrUpgrade(pBld, pHouse);
			const bool isRefineryOrSlaveMiner = pBld->Refinery || pBld->ResourceGatherer || pBld->ResourceDestination;

			int score = 0;
			if (pBld->PowerBonus > pBld->PowerDrain && pBld->BuildCat != BuildCat::Combat)
			{
				score = scores[0] + std::min(500, pBld->PowerBonus * 2);
				if (!pHouse->HasLowPower() && (pHouse->PowerOutput - pHouse->PowerDrain >= 80))
					score -= 1200;
			}
			else if (isRefineryOrSlaveMiner)
			{
				if (owned >= 4)
					continue;
				score = scores[1] - owned * 550;
				if (owned == 0)
					score += 900;
			}
			else if (pBld->Factory == AbstractType::InfantryType)
			{
				if (owned >= 2)
					continue;
				score = scores[2] - owned * 1100;
			}
			else if (pBld->Factory == AbstractType::UnitType && !pBld->Naval)
			{
				if (owned >= 3)
					continue;
				score = scores[3] - owned * 750;
			}
			else if (pBld->Factory == AbstractType::UnitType && pBld->Naval)
			{
				if (owned >= 2)
					continue;
				score = scores[7] - owned * 1100;
			}
			else if (pBld->Factory == AbstractType::AircraftType)
			{
				if (owned >= 2)
					continue;
				score = scores[8] - owned * 850;
			}
			else if (pBld->UnitRepair)
			{
				if (owned >= 1)
					continue;
				score = scores[9];
			}
			else if (pBld->SuperWeapon != -1)
			{
				if (owned >= 1)
					continue;
				score = scores[10] + 450;
			}
			else if (pBld->Storage > 0 && !isRefineryOrSlaveMiner)
			{
				if (owned >= 2)
					continue;
				score = scores[11] - owned * 900;
			}
			else if (pBld->BuildCat == BuildCat::Combat)
			{
				// Never build turrets before establishing core Economy + Barracks + War Factory
				if (!hasCoreEconomyAndProduction && pHouse->ConYards.Count > 0)
					continue;

				const auto pWpnStruct = pBld->GetWeapon(0);
				const auto pWpn = pWpnStruct ? pWpnStruct->WeaponType : nullptr;
				const bool isAA = pWpn && pWpn->Projectile && pWpn->Projectile->AA;
				const bool isPureAA = isAA && !pWpn->Projectile->AG;

				if (isPureAA && totalAADefenses >= maxAADefenses)
					continue;
				if (!isPureAA && totalGroundDefenses >= maxGroundDefenses)
					continue;

				// Scale defense score by Cost and TechLevel so once Radar/Battle Lab are online,
				// advanced Tier-2/3 defenses (Prism Tower, Tesla Coil, Psychic Tower, SAM, Patriot)
				// strongly outscore cheap $500 Tier-1 Pillboxes/Sentry Guns!
				const int tierQualityBonus = std::min(850, (pBld->Cost / 2) + std::max(0, pBld->TechLevel) * 140);
				score = (isPureAA ? scores[6] : scores[5]) + tierQualityBonus;

				if (isPureAA)
					score -= totalAADefenses * 420 + owned * 500;
				else
					score -= totalGroundDefenses * 480 + owned * 600;
			}
			else
			{
				// Tech buildings (Radar, Battle Lab, Ore Purifier, Industrial Plant, Cloning Vats, Grinder):
				// Always build 1 of each unlocked tech structure, never spam duplicates.
				if (owned >= 1)
					continue;
				score = scores[4] + 550 + std::min(600, pBld->Cost / 3);
			}

			if (score > bestBldScore)
			{
				bestBldScore = score;
				bestBldIdx = pBld->ArrayIndex;
			}
		}

		pExt->AIBrain_NextBuildingTarget = bestBldIdx;
	}

	// Unit & Naval Target Selection
	int bestUnitIdx = -1;
	int bestUnitScore = -32000;
	int bestNavalIdx = -1;
	int bestNavalScore = -32000;

	bool ownsUndeployedMCV = false;
	bool ownsUndeployedKeyStructure = false;
	int ownedAmphibTransports = 0;
	for (auto const pOwnedU : UnitClass::Array)
	{
		if (pOwnedU && pOwnedU->Owner == pHouse && !pOwnedU->InLimbo && pOwnedU->IsAlive && pOwnedU->Type)
		{
			auto const pOwnedType = pOwnedU->Type;
			if (pOwnedType->DeploysInto && pOwnedType->DeploysInto->ConstructionYard)
			{
				ownsUndeployedMCV = true;
			}
			else if (pOwnedType->DeploysInto && !pOwnedType->Harvester && !pOwnedType->ResourceGatherer
				&& (pOwnedType->DeploysInto->BuildCat != BuildCat::Combat || !pOwnedType->GetWeapon(0) || !pOwnedType->GetWeapon(0)->WeaponType))
			{
				ownsUndeployedKeyStructure = true;
			}
			else if (IsAmphibiousOrAirTransport(pOwnedType))
			{
				ownedAmphibTransports++;
			}
		}
	}

	// Economy capacity: true refineries plus deployed miners define how many harvesting units are useful
	int trueRefineries = 0;
	int ownedEconomyUnits = 0;

	for (auto const pOwnBldType : pHouse->Buildings)
	{
		if (!pOwnBldType || pOwnBldType->InLimbo || !pOwnBldType->IsAlive || !pOwnBldType->Type)
			continue;

		if (pOwnBldType->Type->ResourceGatherer)
			ownedEconomyUnits++;
		else if (pOwnBldType->Type->Refinery)
			trueRefineries++;
	}

	for (auto const pEcoUnit : UnitClass::Array)
	{
		if (!pEcoUnit || pEcoUnit->Owner != pHouse || pEcoUnit->InLimbo || !pEcoUnit->IsAlive || !pEcoUnit->Type)
			continue;

		auto const pEcoType = pEcoUnit->Type;
		if (pEcoType->Harvester || pEcoType->ResourceGatherer
			|| (pEcoType->DeploysInto && (pEcoType->DeploysInto->Refinery || pEcoType->DeploysInto->ResourceGatherer)))
		{
			ownedEconomyUnits++;
		}
	}

	const int economyUnitCap = (trueRefineries > 0) ? std::clamp(2 * trueRefineries + 1, 3, 7) : 4;
	for (auto const pUnit : UnitTypeClass::Array)
	{
		if (!pUnit || !CanBuildUnderHumanRules(pHouse, pUnit))
			continue;

		if (pUnit->GetActualCost(pHouse) > money + 800)
			continue;

		const bool isMCV = (pUnit->DeploysInto && pUnit->DeploysInto->ConstructionYard);
		if (isMCV)
		{
			if (ownsUndeployedMCV || (pHouse->NumConYards >= 1 && scores[30] < 800))
				continue;
		}

		const auto pWpnStruct = pUnit->GetWeapon(0);
		const auto pWpn = pWpnStruct ? pWpnStruct->WeaponType : nullptr;
		const bool isKeyStructureDeployer = (pUnit->DeploysInto != nullptr && !isMCV && !pUnit->Harvester && !pUnit->ResourceGatherer
			&& (pUnit->DeploysInto->BuildCat != BuildCat::Combat || pWpn == nullptr));
		if (isKeyStructureDeployer && ownsUndeployedKeyStructure)
			continue; // Deploy the current Mobile Outpost / Key Structure Core before queuing another one

		const bool isAmphibTransport = IsAmphibiousOrAirTransport(pUnit);

		// Economy units (harvesters and deployable miners) stop being built once the real refinery capacity is covered
		const bool isEconomyUnit = pUnit->Harvester || pUnit->ResourceGatherer
			|| (pUnit->DeploysInto && (pUnit->DeploysInto->Refinery || pUnit->DeploysInto->ResourceGatherer));

		if (isEconomyUnit && ownedEconomyUnits >= economyUnitCap)
			continue;
		int score = (pUnit->Cost / 4) + (pUnit->Strength / 3);

		// Penalize stacking one unit type so armies stay diverse
		score -= std::min(900, pHouse->CountOwnedNow(pUnit) * 45);
		if (pUnit->Naval)
		{
			if (isAmphibTransport)
			{
				if (ownedAmphibTransports >= 2)
					score -= 1200;
				else
					score += scores[28] + ((features[21] > 0 && ownedAmphibTransports == 0) ? 900 : 200);
			}
			else
			{
				score += scores[29];
			}

			if (score > bestNavalScore)
			{
				bestNavalScore = score;
				bestNavalIdx = pUnit->ArrayIndex;
			}
		}
		else
		{
			const bool isAA = pWpn && pWpn->Projectile && pWpn->Projectile->AA;
			const bool isArtillery = pWpn && pWpn->Range >= (8 * 256);

			if (isMCV)
				score += (pHouse->NumConYards == 0) ? 2500 : scores[30];
			else if (isKeyStructureDeployer)
			{
				// Universal Deployable Key Structure Scoring across all mods (Mobile Outposts/Explorers, Mobile Refineries/Factories, Power/Tech/Turret Cores, Nanocores):
				auto const pTargetBld = pUnit->DeploysInto;
				const int ownedTargetBld = BuildingTypeExt::CountOwnedNowWithDeployOrUpgrade(pTargetBld, pHouse);
				const bool isDedicatedOutpost = (pTargetBld->BaseNormal && !pTargetBld->Refinery
					&& pTargetBld->Factory == AbstractType::None && pTargetBld->PowerBonus <= pTargetBld->PowerDrain);

				if (isDedicatedOutpost)
				{
					// Mobile Outpost / Explorer / Expansion Hub (Tiberian Sun / Firestorm / Mental Omega / YR mods):
					if (scores[30] < 750 || ownedTargetBld >= 2)
						score -= 2000;
					else
						score += scores[30] - (ownedTargetBld * 600);
				}
				else if (pTargetBld->PowerBonus > pTargetBld->PowerDrain && pTargetBld->BuildCat != BuildCat::Combat)
					score += scores[0];
				else if (pTargetBld->Refinery)
					score += scores[1] + ((scores[30] > 700) ? (scores[30] / 2) : 0); // Use Mobile Refinery / Refinery Core for Outpost Expansion!
				else if (pTargetBld->Factory == AbstractType::InfantryType)
					score += scores[2];
				else if (pTargetBld->Factory == AbstractType::UnitType && !pTargetBld->Naval)
					score += scores[3] + ((scores[30] > 800) ? (scores[30] / 3) : 0); // Forward Mobile War Factory / Core!
				else if (pTargetBld->Factory == AbstractType::UnitType && pTargetBld->Naval)
					score += scores[7];
				else if (pTargetBld->Factory == AbstractType::AircraftType)
					score += scores[8];
				else if (pTargetBld->UnitRepair)
					score += scores[9] - (ownedTargetBld * 900);
				else if (pTargetBld->SuperWeapon != -1)
					score += scores[10] - (ownedTargetBld * 900);
				else if (pTargetBld->Storage > 0 && !pTargetBld->Refinery)
					score += scores[11];
				else if (pTargetBld->BuildCat == BuildCat::Combat)
				{
					const auto pBldWpn = pTargetBld->GetWeapon(0) ? pTargetBld->GetWeapon(0)->WeaponType : nullptr;
					const bool isBldAA = pBldWpn && pBldWpn->Projectile && pBldWpn->Projectile->AA;
					score += isBldAA ? scores[6] : scores[5];
				}
				else
				{
					score += scores[4] - (ownedTargetBld * 900);
				}
			}
			else if (pUnit->Harvester || pUnit->ResourceGatherer)
				score += scores[31] + ((ownedEconomyUnits < 2) ? 1200 : 0);
			else if (pUnit->Passengers > 0)
				score += scores[28] + (isAmphibTransport ? 350 : (features[21] > 0 ? -500 : 0));
			else if (isAA)
				score += scores[26];
			else if (isArtillery)
				score += scores[27];
			else if (pUnit->Strength < 300)
				score += scores[25];
			else
				score += scores[24];

			if (score > bestUnitScore)
			{
				bestUnitScore = score;
				bestUnitIdx = pUnit->ArrayIndex;
			}
		}
	}

	pExt->AIBrain_NextUnitTarget = bestUnitIdx;
	pExt->AIBrain_NextNavalTarget = bestNavalIdx;

	// Infantry Target Selection
	int ownedEngineers = 0;
	int ownedInfiltrators = 0;

	for (auto const pOwnInf : InfantryClass::Array)
	{
		if (!pOwnInf || pOwnInf->Owner != pHouse || pOwnInf->InLimbo || !pOwnInf->IsAlive || !pOwnInf->Type)
			continue;

		if (pOwnInf->Type->Engineer)
			ownedEngineers++;
		else if (pOwnInf->Type->Infiltrate)
			ownedInfiltrators++;
	}

	int bestInfIdx = -1;
	int bestInfScore = -32000;

	for (auto const pInf : InfantryTypeClass::Array)
	{
		if (!pInf || !CanBuildUnderHumanRules(pHouse, pInf))
			continue;

		if (pInf->GetActualCost(pHouse) > money + 400)
			continue;

		int score = (pInf->Cost / 2) + pInf->Strength;

		// Penalize stacking one infantry type so squads stay mixed
		score -= std::min(600, pHouse->CountOwnedNow(pInf) * 20);
		const auto pWpnStruct = pInf->GetWeapon(0);
		const auto pWpn = pWpnStruct ? pWpnStruct->WeaponType : nullptr;
		const bool isAA = pWpn && pWpn->Projectile && pWpn->Projectile->AA;

		if (pInf->Engineer)
		{
			if (ownedEngineers >= 2)
				continue;

			score += (pHouse->OwnedInfantry < 6) ? -400 : scores[18];
		}
		else if (pInf->Infiltrate)
		{
			if (ownedInfiltrators >= 1)
				continue;

			score += (pHouse->OwnedInfantry < 6) ? -400 : scores[19];
		}
		else if (isAA || pInf->C4)
			score += scores[20];
		else if (pInf->Cost >= 250)
			score += scores[17];
		else
			score += scores[16];

		if (score > bestInfScore)
		{
			bestInfScore = score;
			bestInfIdx = pInf->ArrayIndex;
		}
	}

	pExt->AIBrain_NextInfantryTarget = bestInfIdx;

	// Aircraft Target Selection
	int bestAirIdx = -1;
	int bestAirScore = -32000;

	if (pHouse->NumAirpads > 0)
	{
		for (auto const pAir : AircraftTypeClass::Array)
		{
			if (!pAir || !CanBuildUnderHumanRules(pHouse, pAir))
				continue;

			if (pAir->GetActualCost(pHouse) > money + 1000)
				continue;

			const int slotScore = (pAir->Strength >= 300 || pAir->Cost >= 1500) ? scores[33] : scores[32];
			const int score = (pAir->Cost / 3) + (pAir->Strength / 2) + slotScore;
			if (score > bestAirScore)
			{
				bestAirScore = score;
				bestAirIdx = pAir->ArrayIndex;
			}
		}
	}

	pExt->AIBrain_NextAircraftTarget = bestAirIdx;

	if (Unsorted::CurrentFrame % 450 < evalDelay)
	{
		const int bldIdx = pExt->AIBrain_NextBuildingTarget;
		const char* bldName = (bldIdx >= 0 && bldIdx < BuildingTypeClass::Array.Count) ? BuildingTypeClass::Array.GetItem(bldIdx)->ID : "None";
		const char* unitName = (bestUnitIdx >= 0 && bestUnitIdx < UnitTypeClass::Array.Count) ? UnitTypeClass::Array.GetItem(bestUnitIdx)->ID : "None";
		const char* infName = (bestInfIdx >= 0 && bestInfIdx < InfantryTypeClass::Array.Count) ? InfantryTypeClass::Array.GetItem(bestInfIdx)->ID : "None";
		const char* airName = (bestAirIdx >= 0 && bestAirIdx < AircraftTypeClass::Array.Count) ? AircraftTypeClass::Array.GetItem(bestAirIdx)->ID : "None";
		Debug::Log(
			"[NeuralAI] Frame %d | House #%d (%s) | Expert=%d Arch=%d | Credits=%d | NextBld=%s | NextUnit=%s | NextInf=%s | NextAir=%s\n",
			Unsorted::CurrentFrame, pHouse->ArrayIndex,
			(pHouse->Type && pHouse->Type->ID[0]) ? pHouse->Type->ID : "AI",
			pExt->AIBrain_ActiveExpert, pExt->AIBrain_Archetype, money,
			bldName, unitName, infName, airName);
	}

	return evalDelay;
}

void NeuralAI::UpdateBuildingProduction(HouseClass* pHouse)
{
	if (pHouse->ProducingBuildingTypeIndex != -1)
		return;

	auto const pExt = HouseExt::Fetch(pHouse);
	if (Unsorted::CurrentFrame - pExt->AIBrain_LastInferenceFrame > 60)
		NeuralAI::UpdateExpertAI(pHouse);

	if (pExt->AIBrain_NextBuildingTarget >= 0)
		pHouse->ProducingBuildingTypeIndex = pExt->AIBrain_NextBuildingTarget;
}

void NeuralAI::UpdateUnitProduction(HouseClass* pHouse)
{
	auto const pExt = HouseExt::Fetch(pHouse);
	if (Unsorted::CurrentFrame - pExt->AIBrain_LastInferenceFrame > 60)
		NeuralAI::UpdateExpertAI(pHouse);

	if (pHouse->ProducingUnitTypeIndex == -1 && pExt->AIBrain_NextUnitTarget >= 0)
		pHouse->ProducingUnitTypeIndex = pExt->AIBrain_NextUnitTarget;

	if (pExt->ProducingNavalUnitTypeIndex == -1 && pExt->AIBrain_NextNavalTarget >= 0)
		pExt->ProducingNavalUnitTypeIndex = pExt->AIBrain_NextNavalTarget;
}

void NeuralAI::UpdateInfantryProduction(HouseClass* pHouse)
{
	if (pHouse->ProducingInfantryTypeIndex != -1)
		return;

	auto const pExt = HouseExt::Fetch(pHouse);
	if (Unsorted::CurrentFrame - pExt->AIBrain_LastInferenceFrame > 60)
		NeuralAI::UpdateExpertAI(pHouse);

	if (pExt->AIBrain_NextInfantryTarget >= 0)
		pHouse->ProducingInfantryTypeIndex = pExt->AIBrain_NextInfantryTarget;
}

void NeuralAI::UpdateAircraftProduction(HouseClass* pHouse)
{
	if (pHouse->ProducingAircraftTypeIndex != -1)
		return;

	auto const pExt = HouseExt::Fetch(pHouse);
	if (Unsorted::CurrentFrame - pExt->AIBrain_LastInferenceFrame > 60)
		NeuralAI::UpdateExpertAI(pHouse);

	if (pExt->AIBrain_NextAircraftTarget >= 0)
		pHouse->ProducingAircraftTypeIndex = pExt->AIBrain_NextAircraftTarget;
}

bool NeuralAI::IsCellClearedOfExitCorridors(HouseClass* pHouse, BuildingTypeClass* pType, CellStruct candidateCell)
{
	const int candW = pType->GetFoundationWidth();
	const int candH = pType->GetFoundationHeight(false);

	for (auto const pExisting : pHouse->Buildings)
	{
		if (!pExisting || pExisting->InLimbo || !pExisting->IsAlive || !pExisting->Type)
			continue;

		auto const pExType = pExisting->Type;
		const CellStruct exCell = pExisting->GetMapCoords();
		const int exW = pExType->GetFoundationWidth();
		const int exH = pExType->GetFoundationHeight(false);

		// Reserve 3-cell exit corridor south and east of Factories, Barracks, Refineries, and Repair Depots
		const bool needsExit = (pExType->Factory != AbstractType::None) || pExType->Refinery || pExType->UnitRepair || pExType->WeaponsFactory;
		const int marginSouth = needsExit ? 3 : 1;
		const int marginEast = needsExit ? 3 : 1;

		// Enforce minimum 4-cell spacing between Base Defense turrets so the AI spreads defenses along the perimeter instead of clumping
		if (pType->BuildCat == BuildCat::Combat && pExType->BuildCat == BuildCat::Combat)
		{
			const int turretDist = std::abs(candidateCell.X - exCell.X) + std::abs(candidateCell.Y - exCell.Y);
			if (turretDist < 4)
				return false;
		}

		const int exMinX = exCell.X - 1;
		const int exMaxX = exCell.X + exW + marginEast;
		const int exMinY = exCell.Y - 1;
		const int exMaxY = exCell.Y + exH + marginSouth;

		const int candMargin = (pType->Factory != AbstractType::None || pType->Refinery) ? 3 : 0;
		const int candMinX = candidateCell.X;
		const int candMaxX = candidateCell.X + candW + candMargin;
		const int candMinY = candidateCell.Y;
		const int candMaxY = candidateCell.Y + candH + candMargin;

		const bool overlapX = (candMinX < exMaxX) && (candMaxX > exMinX);
		const bool overlapY = (candMinY < exMaxY) && (candMaxY > exMinY);

		if (overlapX && overlapY)
			return false;
	}

	return true;
}

CellStruct NeuralAI::FindSpatialTetrisBuildCell(HouseClass* pHouse, BuildingTypeClass* pBuildingType)
{
	if (!pHouse || !pBuildingType || pBuildingType->PowersUpBuilding[0])
		return CellStruct::Empty;

	CellStruct center = pHouse->BaseCenter != CellStruct::Empty ? pHouse->BaseCenter : pHouse->BaseSpawnCell;
	const bool isRefineryOrSlaveMiner = pBuildingType->Refinery || pBuildingType->ResourceGatherer || pBuildingType->ResourceDestination;
	if (pHouse->ConYards.Count > 0)
	{
		// If we own an expansion ConYard (Outpost), allow Refineries and Defenses to build around the newest Outpost ConYard
		const int yardIdx = (pHouse->ConYards.Count > 1 && (isRefineryOrSlaveMiner || pBuildingType->BuildCat == BuildCat::Combat))
			? (pHouse->ConYards.Count - 1)
			: 0;
		center = pHouse->ConYards.GetItem(yardIdx)->GetMapCoords();
	}

	// Support universal Mobile Outpost / Expansion Hub structures (BaseNormal=yes across all mods) for building Refineries & Defenses at outposts!
	if (isRefineryOrSlaveMiner || pBuildingType->BuildCat == BuildCat::Combat)
	{
		for (auto const pOwnBld : pHouse->Buildings)
		{
			if (pOwnBld && !pOwnBld->InLimbo && pOwnBld->IsAlive && pOwnBld->Type && pOwnBld->Type->BaseNormal && !pOwnBld->Type->ConstructionYard && !pOwnBld->Type->Refinery)
				center = pOwnBld->GetMapCoords();
		}
	}

	// Allied Base-Sharing (BuildOffAlly=yes): If we lost our ConYard OR an Ally's ConYard is under attack (< 80% HP) while placing a Base Defense, build around our Ally's ConYard!
	if (GameModeOptionsClass::Instance.BuildOffAlly && (pHouse->ConYards.Count == 0 || pBuildingType->BuildCat == BuildCat::Combat))
	{
		for (auto const pAllyBld : BuildingClass::Array)
		{
			if (!pAllyBld || pAllyBld->InLimbo || !pAllyBld->IsAlive || !pAllyBld->Owner || !pAllyBld->Type)
				continue;
			if (pAllyBld->Owner != pHouse && pHouse->IsAlliedWith(pAllyBld->Owner) && !pAllyBld->Owner->IsNeutral() && pAllyBld->Type->EligibileForAllyBuilding)
			{
				if (pHouse->ConYards.Count == 0 || (pBuildingType->BuildCat == BuildCat::Combat && pAllyBld->HasBeenAttacked && pAllyBld->GetHealthPercentage() < 0.80))
				{
					center = pAllyBld->GetMapCoords();
					break;
				}
			}
		}
	}

	if (center == CellStruct::Empty)
		return CellStruct::Empty;

	const int w = pBuildingType->GetFoundationWidth();
	const int h = pBuildingType->GetFoundationHeight(false);

	// Discover nearest Ore/Tiberium patch (for Expert 6: Resource Base-Crawling)
	CellStruct nearestOreCell = CellStruct::Empty;
	int bestOreDist = 9999;
	for (int dy = -30; dy <= 30; dy += 2)
	{
		for (int dx = -30; dx <= 30; dx += 2)
		{
			const int d = std::abs(dx) + std::abs(dy);
			if (d < 4 || d >= bestOreDist)
				continue;

			const CellStruct tc { static_cast<short>(center.X + dx), static_cast<short>(center.Y + dy) };
			auto const pCell = MapClass::Instance.TryGetCellAt(tc);
			if (pCell && pCell->GetContainedTiberiumValue() > 0)
			{
				bestOreDist = d;
				nearestOreCell = tc;
			}
		}
	}

	// Discover primary Enemy Threat Vector (for Expert 7: Forward Defense Crawling & Rear Tech Shielding)
	CellStruct enemyTargetCell = CellStruct::Empty;
	if (pHouse->EnemyHouseIndex >= 0 && pHouse->EnemyHouseIndex < HouseClass::Array.Count)
	{
		if (auto const pEnemy = HouseClass::Array.GetItem(pHouse->EnemyHouseIndex))
		{
			enemyTargetCell = pEnemy->BaseCenter != CellStruct::Empty ? pEnemy->BaseCenter : pEnemy->BaseSpawnCell;
			if (enemyTargetCell == CellStruct::Empty && pEnemy->ConYards.Count > 0)
				enemyTargetCell = pEnemy->ConYards.GetItem(0)->GetMapCoords();
		}
	}

	if (enemyTargetCell == CellStruct::Empty)
	{
		for (auto const pOther : HouseClass::Array)
		{
			if (!pOther || pOther == pHouse || pHouse->IsAlliedWith(pOther) || pOther->IsNeutral() || pOther->IsObserver() || pOther->Defeated)
				continue;

			enemyTargetCell = pOther->BaseCenter != CellStruct::Empty ? pOther->BaseCenter : pOther->BaseSpawnCell;
			if (enemyTargetCell != CellStruct::Empty)
				break;
		}
	}

	const bool isResourceBld = isRefineryOrSlaveMiner || pBuildingType->Storage > 0 || pBuildingType->OrePurifier;
	const bool isCombatDefense = (pBuildingType->BuildCat == BuildCat::Combat);
	const auto pWpnStruct = pBuildingType->GetWeapon(0);
	const auto pWpn = pWpnStruct ? pWpnStruct->WeaponType : nullptr;
	const bool isPureAA = isCombatDefense && pWpn && pWpn->Projectile && pWpn->Projectile->AA && !pWpn->Projectile->AG;
	const bool isGroundDefense = isCombatDefense && !isPureAA;
	const bool isFactoryOrBarracks = (pBuildingType->Factory != AbstractType::None) || pBuildingType->WeaponsFactory || pBuildingType->UnitRepair;
	const bool isRearProtected = !isResourceBld && !isCombatDefense && !isFactoryOrBarracks;

	CellStruct bestCandidate = CellStruct::Empty;
	int bestSpatialScore = -999999;
	int validCandidatesChecked = 0;

	// Search outward in spiral rings and score candidate cells directionally across the full base perimeter.
	// Combat defenses start at radius >= 5 so they are placed on the outer perimeter rather than inside the ConYard core.
	const int startRadius = isCombatDefense ? 5 : 3;
	const int maxCandidatesToEvaluate = isCombatDefense ? 180 : 140;

	for (int radius = startRadius; radius <= 26; ++radius)
	{
		for (int dy = -radius; dy <= radius; ++dy)
		{
			for (int dx = -radius; dx <= radius; ++dx)
			{
				if (std::abs(dx) != radius && std::abs(dy) != radius)
					continue;

				const CellStruct candidate { static_cast<short>(center.X + dx), static_cast<short>(center.Y + dy) };
				if (!MapClass::Instance.CoordinatesLegal(candidate))
					continue;

				bool foundationValid = true;
				for (int fy = 0; fy < h && foundationValid; ++fy)
				{
					for (int fx = 0; fx < w; ++fx)
					{
						const CellStruct fc { static_cast<short>(candidate.X + fx), static_cast<short>(candidate.Y + fy) };
						auto const pCell = MapClass::Instance.TryGetCellAt(fc);

						if (!pCell || !pCell->CanThisExistHere(pBuildingType->SpeedType, pBuildingType, pHouse))
						{
							foundationValid = false;
							break;
						}
					}
				}

				if (!foundationValid)
					continue;

				if (!NeuralAI::IsCellClearedOfExitCorridors(pHouse, pBuildingType, candidate))
					continue;

				int spatialScore = -radius * 10;

				if (isResourceBld && nearestOreCell != CellStruct::Empty)
				{
					// Expert 6: Crawl Refineries, Silos & Ore Purifiers directly toward the nearest Ore/Tiberium patch
					const int distToOre = std::abs(candidate.X - nearestOreCell.X) + std::abs(candidate.Y - nearestOreCell.Y);
					spatialScore -= distToOre * 45;
				}
				else if (isGroundDefense && enemyTargetCell != CellStruct::Empty)
				{
					// Expert 7: Place Ground Defenses (Prism, Tesla, Obelisk, Grand Cannon, Bunkers) on the frontline perimeter facing the enemy
					const int distToEnemy = std::abs(candidate.X - enemyTargetCell.X) + std::abs(candidate.Y - enemyTargetCell.Y);
					spatialScore -= distToEnemy * 38;
					if (radius >= 6 && radius <= 14)
						spatialScore += 220;
				}
				else if (isPureAA)
				{
					// Spread AA defenses around mid-ring (radius 5..11) to cover both ConYard/Tech and frontline
					if (radius >= 5 && radius <= 11)
						spatialScore += 160;
				}
				else if (isRearProtected && enemyTargetCell != CellStruct::Empty)
				{
					// Expert 1: Tuck Power Plants, Tech Centers & Superweapons safely into the rear perimeter away from enemy attacks
					const int distToEnemy = std::abs(candidate.X - enemyTargetCell.X) + std::abs(candidate.Y - enemyTargetCell.Y);
					spatialScore += distToEnemy * 18;
				}
				else if (isFactoryOrBarracks && enemyTargetCell != CellStruct::Empty)
				{
					// Place War Factories & Barracks slightly forward with guaranteed 3-cell South/East exit corridors
					const int distToEnemy = std::abs(candidate.X - enemyTargetCell.X) + std::abs(candidate.Y - enemyTargetCell.Y);
					spatialScore -= distToEnemy * 12;
				}

				if (spatialScore > bestSpatialScore)
				{
					bestSpatialScore = spatialScore;
					bestCandidate = candidate;
				}

				if (++validCandidatesChecked >= maxCandidatesToEvaluate)
					return bestCandidate;
			}
		}
	}

	return bestCandidate;
}

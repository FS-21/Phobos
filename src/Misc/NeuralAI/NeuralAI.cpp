#include "NeuralAI.h"

#include <AircraftClass.h>
#include <AircraftTypeClass.h>
#include <BeaconManagerClass.h>
#include <BuildingClass.h>
#include <BuildingTypeClass.h>
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
#include <OverlayTypeClass.h>
#include <RulesClass.h>
#include <ScenarioClass.h>
#include <SessionClass.h>
#include <SuperClass.h>
#include <TechnoClass.h>
#include <TemporalClass.h>
#include <UnitClass.h>
#include <UnitTypeClass.h>

#include <Ext/BuildingType/Body.h>
#include <Ext/House/Body.h>
#include <Ext/Rules/Body.h>
#include <Ext/SWType/Body.h>
#include <Ext/Techno/Body.h>
#include <Ext/TechnoType/Body.h>
#include <Ext/WarheadType/Body.h>
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

template <typename T> inline uint64_t HashValueFNV1a(const T& val, uint64_t hash)
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

// Per-house vision map built from the sight of the AI and its allies. Replaces
// CellClass::IsShrouded, which reflects the local player's shroud and would
// desync multiplayer and leak the human's exploration to the AI.
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

bool IsTunnelNetworkBuilding(const BuildingTypeClass* pBldType)
{
	if (!pBldType)
		return false;

	if (pBldType->Passengers > 0 && pBldType->InfantryAbsorb &&
	    (pBldType->UnitAbsorb || pBldType->SuperWeapon != -1 || pBldType->SuperWeapon2 != -1))
	{
		return true;
	}

	return pBldType->ID && std::strstr(pBldType->ID, "TUNNEL") != nullptr && pBldType->Passengers > 0;
}

bool IsTunnelDeliverySuperWeapon(const SuperWeaponTypeClass* pSWType, HouseClass* pOwner)
{
	if (!pSWType)
		return false;

	if (pSWType->ID && (std::strstr(pSWType->ID, "Tunnel") != nullptr || std::strstr(pSWType->ID, "TUNNEL") != nullptr))
		return true;

	if (pOwner)
	{
		for (auto const pBld : pOwner->Buildings)
		{
			if (pBld && !pBld->InLimbo && pBld->IsAlive && pBld->Type && IsTunnelNetworkBuilding(pBld->Type))
			{
				if (pBld->Type->SuperWeapon == pSWType->ArrayIndex || pBld->Type->SuperWeapon2 == pSWType->ArrayIndex)
					return true;
			}
		}
	}

	return false;
}
} // namespace

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
		// Phobos_AI_Trainer can expand or replace this with the full 250 MB league
		// brain.
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
		pHdr->OutputDim = 64;
		pHdr->ReservedOutputDim = 64;

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

		pHdr->BaseModelHash =
		    HashBytesFNV1a(buffer.data() + sizeof(AIBaseHeader), totalBootstrapSize - sizeof(AIBaseHeader));

		HANDLE hOut =
		    CreateFileA(basePath, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
		if (hOut != INVALID_HANDLE_VALUE)
		{
			DWORD written = 0;
			WriteFile(hOut, buffer.data(), totalBootstrapSize, &written, NULL);
			CloseHandle(hOut);
		}
	}

	NeuralAI::BaseFileHandle = CreateFileA(basePath, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
	                                       OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);

	if (NeuralAI::BaseFileHandle == INVALID_HANDLE_VALUE)
		return;

	LARGE_INTEGER fileSize = {};
	if (!GetFileSizeEx(NeuralAI::BaseFileHandle, &fileSize) ||
	    fileSize.QuadPart < static_cast<LONGLONG>(sizeof(AIBaseHeader)))
	{
		CloseHandle(NeuralAI::BaseFileHandle);
		NeuralAI::BaseFileHandle = INVALID_HANDLE_VALUE;
		return;
	}

	NeuralAI::BaseMappedSize = static_cast<size_t>(fileSize.QuadPart);
	NeuralAI::BaseMappingHandle =
	    CreateFileMappingA(NeuralAI::BaseFileHandle, NULL, PAGE_READONLY, 0, 0, "Local\\PhobosAIBaseSharedMapping");

	if (!NeuralAI::BaseMappingHandle)
	{
		NeuralAI::BaseMappingHandle = CreateFileMappingA(NeuralAI::BaseFileHandle, NULL, PAGE_READONLY, 0, 0, NULL);
	}

	if (NeuralAI::BaseMappingHandle)
	{
		NeuralAI::BaseMappedView =
		    static_cast<const uint8_t*>(MapViewOfFile(NeuralAI::BaseMappingHandle, FILE_MAP_READ, 0, 0, 0));
	}
}

bool NeuralAI::LoadBackpackFile(const char* pPath, AIBackpackFile& outBackpack, bool requireSignature)
{
	HANDLE hFile = CreateFileA(pPath, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
	                           FILE_ATTRIBUTE_NORMAL, NULL);
	if (hFile == INVALID_HANDLE_VALUE)
		return false;

	AIBackpackFile loaded = {};
	DWORD bytesRead = 0;

	const bool valid = ReadFile(hFile, &loaded, sizeof(AIBackpackFile), &bytesRead, NULL) &&
	                   bytesRead == sizeof(AIBackpackFile) && std::memcmp(loaded.Magic, "PHOBOSBP", 8) == 0 &&
	                   loaded.SchemaVersion == 3 &&
	                   (!requireSignature || loaded.BaseSignature == NeuralAI::CurrentBaseSignature);

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

	// Campaign and standard Multiplayer use only the shared deterministic base
	// brain.
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

	// In headless training the trainer owns both policy files: they are loaded
	// verbatim and never written back
	if (NeuralAI::HeadlessMode)
	{
		char policyPath[MAX_PATH];

		std::memset(&NeuralAI::Backpack, 0, sizeof(AIBackpackFile));
		std::memcpy(NeuralAI::Backpack.Magic, "PHOBOSBP", 8);
		NeuralAI::Backpack.SchemaVersion = 3;
		NeuralAI::Backpack.BaseSignature = NeuralAI::CurrentBaseSignature;
		NeuralAI::Backpack.EstimatedELO = 1200;
		std::snprintf(policyPath, sizeof(policyPath), "Resources\\AI\\ai_backpack_slot_%d.dat",
		              NeuralAI::TrainSlotIndex);

		const bool challengerLoaded = NeuralAI::LoadBackpackFile(policyPath, NeuralAI::Backpack, false);

		std::memcpy(&NeuralAI::OpponentBackpack, &NeuralAI::Backpack, sizeof(AIBackpackFile));
		std::snprintf(policyPath, sizeof(policyPath), "Resources\\AI\\ai_backpack_opp_slot_%d.dat",
		              NeuralAI::TrainSlotIndex);
		NeuralAI::OpponentLoaded = NeuralAI::LoadBackpackFile(policyPath, NeuralAI::OpponentBackpack, false);

		NeuralAI::AllowBackpackInference = true;
		NeuralAI::BackpackLoaded = true;
		Debug::Log("[NeuralAI] Headless policies: challenger=%s opponent=%s\n", challengerLoaded ? "loaded" : "default",
		           NeuralAI::OpponentLoaded ? "loaded" : "shared");

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
		NeuralAI::Backpack.SchemaVersion = 3;
		NeuralAI::Backpack.BaseSignature = NeuralAI::CurrentBaseSignature;
		NeuralAI::Backpack.EstimatedELO = 1200;
		NeuralAI::Backpack.BackpackHash =
		    HashBytesFNV1a(&NeuralAI::Backpack.LoRA_A[0], sizeof(NeuralAI::Backpack.LoRA_A));
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
	    &NeuralAI::Backpack.LoRA_A[0], sizeof(NeuralAI::Backpack.LoRA_A) + sizeof(NeuralAI::Backpack.LoRA_B));

	char bpPath[MAX_PATH];
	if (NeuralAI::HeadlessMode && NeuralAI::TrainSlotIndex > 0)
		std::snprintf(bpPath, sizeof(bpPath), "Resources\\AI\\ai_backpack_slot_%d.dat", NeuralAI::TrainSlotIndex);
	else
		std::snprintf(bpPath, sizeof(bpPath), "Resources\\AI\\ai_backpack.dat");

	HANDLE hFile =
	    CreateFileA(bpPath, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	if (hFile != INVALID_HANDLE_VALUE)
	{
		DWORD written = 0;
		WriteFile(hFile, &NeuralAI::Backpack, sizeof(AIBackpackFile), &written, NULL);
		CloseHandle(hFile);
	}

	if (!NeuralAI::HeadlessMode || NeuralAI::TrainSlotIndex == 0)
	{
		HANDLE hFileAlt = CreateFileA("AI\\ai_backpack.dat", GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS,
		                              FILE_ATTRIBUTE_NORMAL, NULL);
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

	NeuralAI::TelemetryShmHandle =
	    CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, sizeof(AITrainerSharedTelemetry), shmName);

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

	static const wchar_t* const ArchetypeNamesW[5] = {L"Balanced Grandmaster", L"SpecOps & Infiltration",
	                                                  L"Tech & Fortress Turtle", L"Blitzkrieg Rush",
	                                                  L"Air & Artillery Siege"};

	int activeAIHouses = 0;
	HouseClass* pFirstAI = nullptr;

	for (auto const pHouse : HouseClass::Array)
	{
		if (!NeuralAI::IsEnabledForHouse(pHouse))
			continue;

		activeAIHouses++;
		auto const pExt = HouseExt::Fetch(pHouse);

		if (!NeuralAI::IsCampaignRestricted(pHouse))
		{
			pHouse->Production = true;
			pHouse->AIState_1E4 = 0;
		}

		if (!NeuralAI::HeadlessMode)
		{
			pExt->AIBrain_PolicySlot = 0;
		}
		else if (!pFirstAI)
		{
			pFirstAI = pHouse;
			pExt->AIBrain_PolicySlot = 0;
		}
		else if (pHouse->IsAlliedWith(pFirstAI))
		{
			pExt->AIBrain_PolicySlot = 0;
		}
		else
		{
			bool hasAlliedSlot0 = false;
			bool hasAlliedSlot1 = false;
			for (auto const pPrev : HouseClass::Array)
			{
				if (!pPrev || pPrev == pHouse || !NeuralAI::IsEnabledForHouse(pPrev))
					continue;
				if (pPrev->ArrayIndex >= pHouse->ArrayIndex)
					break;
				if (pHouse->IsAlliedWith(pPrev))
				{
					const int prevSlot = HouseExt::Fetch(pPrev)->AIBrain_PolicySlot;
					if (prevSlot == 0)
						hasAlliedSlot0 = true;
					else
						hasAlliedSlot1 = true;
				}
			}
			if (hasAlliedSlot0)
				pExt->AIBrain_PolicySlot = 0;
			else if (hasAlliedSlot1)
				pExt->AIBrain_PolicySlot = 1;
			else
				pExt->AIBrain_PolicySlot = (activeAIHouses - 1) % 2;
		}

		// Select Strategic Archetype (0..4) weighted by evolved OutputBias[48..52]
		// + learned faction memory + stochastic exploration
		int bestArch = ScenarioClass::Instance->Random.RandomRanged(0, NeuralAI::NumArchetypes - 1);
		if (NeuralAI::AllowBackpackInference && NeuralAI::BackpackLoaded)
		{
			const AIBackpackFile& policy = NeuralAI::PolicyFor(pHouse);
			int bestArchScore = -999999;
			for (int a = 0; a < NeuralAI::NumArchetypes; ++a)
			{
				const uint32_t eIdx = static_cast<uint32_t>((pHouse->SideIndex * 40 + a * 8) % 512);
				const int memBonus = policy.Entries[eIdx].VisitCount > 0
				                         ? static_cast<int>(policy.Entries[eIdx].ArchetypeScoreDelta[a]) * 6
				                         : 0;
				const int exploreJitter = ScenarioClass::Instance->Random.RandomRanged(-90, 90);
				const int aScore = static_cast<int>(policy.OutputBias[48 + a]) + memBonus + exploreJitter;
				if (aScore > bestArchScore)
				{
					bestArchScore = aScore;
					bestArch = a;
				}
			}
		}
		pExt->AIBrain_Archetype = bestArch;
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
		Debug::Log("[NeuralAI] Activated for House #%d (%s, Diff=%d) -> "
		           "Archetype=%d (%ls)\n",
		           pHouse->ArrayIndex, countryId, pHouse->GetAIDifficultyIndex(), archIdx, ArchetypeNamesW[archIdx]);

		if (!NeuralAI::HeadlessMode)
		{
			wchar_t msgBuf[256];
			std::swprintf(msgBuf, 256, L"[Neural AI] House #%d (%hs) -> Style: %ls", pHouse->ArrayIndex, countryId,
			              ArchetypeNamesW[archIdx]);
			MessageListClass::Instance.PrintMessage(
			    msgBuf, RulesClass::Instance->MessageDelay,
			    HouseClass::CurrentPlayer ? HouseClass::CurrentPlayer->ColorSchemeIndex : 0, true);
		}
	}

	if (activeAIHouses > 0)
	{
		Debug::Log("[NeuralAI] ScenarioStart: %d AI Houses controlled by NeuralAI "
		           "(aimd.ini bypassed) | BaseSignature=%016llX | ELO=%d\n",
		           activeAIHouses, NeuralAI::CurrentBaseSignature, NeuralAI::Backpack.EstimatedELO);

		if (!NeuralAI::HeadlessMode)
		{
			wchar_t bannerBuf[256];
			std::swprintf(bannerBuf, 256,
			              L"[Phobos Neural AI] ACTIVE (%d AI Houses | Brain Sig:%08X | ELO:%d "
			              L"| aimd.ini bypassed)",
			              activeAIHouses, static_cast<uint32_t>(NeuralAI::CurrentBaseSignature & 0xFFFFFFFFULL),
			              NeuralAI::Backpack.EstimatedELO);
			MessageListClass::Instance.PrintMessage(
			    bannerBuf, RulesClass::Instance->MessageDelay,
			    HouseClass::CurrentPlayer ? HouseClass::CurrentPlayer->ColorSchemeIndex : 0, true);
		}
	}

	if (NeuralAI::HeadlessMode)
	{
		reinterpret_cast<void(__fastcall*)()>(0x407020)();

		if (Game::hWnd && !IsIconic(Game::hWnd))
		{
			ReleaseCapture();
			ShowWindow(Game::hWnd, SW_SHOWMINNOACTIVE);
		}

		NeuralAI::UpdateTelemetrySnapshot(false, -1, false);
	}
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

		const int score = (pHouse->TotalKilledUnits * 200) + (pHouse->TotalKilledBuildings * 450) +
		                  (pHouse->OwnedBuildings * 300) + (pHouse->OwnedUnits * 200);

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
			const int fastBonus =
			    (!NeuralAI::MatchEndStalemate && won && Unsorted::CurrentFrame > 0 && Unsorted::CurrentFrame <= 18000)
			        ? 4
			        : 0;
			const int delta = NeuralAI::MatchEndStalemate ? (won ? -4 : -8) : (won ? (12 + fastBonus) : -6);

			uint32_t entryIdx = static_cast<uint32_t>((pHouse->SideIndex * 40 + arch * 8 + exp) % 512);
			if (NeuralAI::Backpack.NumEntries < 512)
				NeuralAI::Backpack.NumEntries = std::max(NeuralAI::Backpack.NumEntries, entryIdx + 1);

			auto& entry = NeuralAI::Backpack.Entries[entryIdx];
			entry.StateClusterHash = (static_cast<uint32_t>(pHouse->SideIndex) << 16) |
			                         static_cast<uint32_t>(arch << 8) | static_cast<uint32_t>(exp);
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
		NeuralAI::UpdateTelemetrySnapshot(
		    true, NeuralAI::MatchEndWinnerHouse >= 0 ? NeuralAI::MatchEndWinnerHouse : bestHouseIdx,
		    NeuralAI::MatchEndStalemate);
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
		NeuralAI::CurrentMeasuredFPS =
		    (NeuralAI::FramesSinceLastFPSCheck * 1000) / std::max<DWORD>(1, now - NeuralAI::LastFPSCheckTick);
		NeuralAI::FramesSinceLastFPSCheck = 0;
		NeuralAI::LastFPSCheckTick = now;
	}

	const int frame = Unsorted::CurrentFrame;
	if (frame % 60 != 0)
		return;

	int totalKillsAndCaptures = 0;
	int aliveCombatHouses = 0;
	int lastAliveHouseIdx = -1;
	std::vector<HouseClass*> aliveCombatList;

	for (auto const pHouse : HouseClass::Array)
	{
		if (!pHouse || pHouse->IsNeutral() || pHouse->IsObserver())
			continue;

		totalKillsAndCaptures +=
		    pHouse->TotalKilledUnits + pHouse->TotalKilledBuildings + pHouse->StolenBuildingsCredits;

		if (!pHouse->Defeated && (pHouse->OwnedBuildings > 0 || pHouse->OwnedUnits > 0 || pHouse->OwnedInfantry > 0))
		{
			aliveCombatHouses++;
			lastAliveHouseIdx = pHouse->ArrayIndex;
			aliveCombatList.push_back(pHouse);
		}
	}

	bool hasHostilePairAlive = false;
	for (size_t i = 0; i < aliveCombatList.size() && !hasHostilePairAlive; ++i)
	{
		for (size_t j = i + 1; j < aliveCombatList.size(); ++j)
		{
			if (!aliveCombatList[i]->IsAlliedWith(aliveCombatList[j]) ||
			    !aliveCombatList[j]->IsAlliedWith(aliveCombatList[i]))
			{
				hasHostilePairAlive = true;
				break;
			}
		}
	}

	if (totalKillsAndCaptures > NeuralAI::LastTotalKillsAndCaptures)
	{
		NeuralAI::LastTotalKillsAndCaptures = totalKillsAndCaptures;
		NeuralAI::LastCombatFrame = frame;
	}

	const bool onlyOneSideLeft = (frame > 300 && (aliveCombatHouses <= 1 || !hasHostilePairAlive));
	const bool activeRecentCombat = ((frame - NeuralAI::LastCombatFrame) < 2700);
	const int baseMaxFrames = (aliveCombatHouses >= 3) ? 162000 : 135000;
	const int maxMatchFrames = activeRecentCombat ? (baseMaxFrames + 27000) : baseMaxFrames;
	const int noCombatLimit = (aliveCombatHouses >= 3) ? 13500 : 10800;
	const bool stalemate =
	    !onlyOneSideLeft &&
	    ((frame > 18000 && (frame - NeuralAI::LastCombatFrame) >= noCombatLimit) || (frame >= maxMatchFrames));

	if (stalemate || onlyOneSideLeft)
	{
		int bestHouseIdx = lastAliveHouseIdx;
		int bestScore = -1;

		for (auto const pHouse : HouseClass::Array)
		{
			if (!pHouse || pHouse->IsNeutral() || pHouse->IsObserver() || pHouse->Defeated)
				continue;

			const int score = (pHouse->TotalKilledUnits * 200) + (pHouse->TotalKilledBuildings * 450) +
			                  (pHouse->OwnedBuildings * 300) + (pHouse->OwnedUnits * 200);

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
		hs.NetWorthScore =
		    (hs.KilledUnits * 200) + (hs.KilledBuildings * 450) + (hs.OwnedBuildings * 300) + (hs.OwnedUnits * 200);
		hs.IsWinner = (pHouse->ArrayIndex == winnerHouseIdx || pHouse->IsWinner) ? 1 : 0;
		hs.IsDefeated = pHouse->Defeated ? 1 : 0;
		hs.PolicySlot = static_cast<uint8_t>(pExt->AIBrain_PolicySlot);
		hs.ColorR = pHouse->Color.R;
		hs.ColorG = pHouse->Color.G;
		hs.ColorB = pHouse->Color.B;
	}

	dst->NumHouses = hCount;
	std::memset(dst->RadarCells, 0, sizeof(dst->RadarCells));

	int minMapX = 99999;
	int maxMapX = -99999;
	int minMapY = 99999;
	int maxMapY = -99999;

	auto trackBounds = [&](const CellStruct& c)
	{
		if (c.X > 0 && c.Y > 0)
		{
			if (c.X < minMapX)
				minMapX = c.X;
			if (c.X > maxMapX)
				maxMapX = c.X;
			if (c.Y < minMapY)
				minMapY = c.Y;
			if (c.Y > maxMapY)
				maxMapY = c.Y;
		}
	};

	for (auto const pBld : BuildingClass::Array)
	{
		if (!pBld || pBld->InLimbo || !pBld->IsAlive || !pBld->Owner || pBld->Owner->IsNeutral())
			continue;

		const CellStruct cell = pBld->GetMapCoords();
		trackBounds(cell);
		const int rx = std::clamp(static_cast<int>(cell.X) / 2, 0, 127);
		const int ry = std::clamp(static_cast<int>(cell.Y) / 2, 0, 127);
		dst->RadarCells[ry * 128 + rx] = static_cast<uint8_t>((pBld->Owner->ArrayIndex & 0x07) + 1 + 0x80);
	}

	for (auto const pUnit : UnitClass::Array)
	{
		if (!pUnit || pUnit->InLimbo || !pUnit->IsAlive || !pUnit->Owner || pUnit->Owner->IsNeutral())
			continue;

		const CellStruct cell = pUnit->GetMapCoords();
		trackBounds(cell);
		const int rx = std::clamp(static_cast<int>(cell.X) / 2, 0, 127);
		const int ry = std::clamp(static_cast<int>(cell.Y) / 2, 0, 127);
		dst->RadarCells[ry * 128 + rx] = static_cast<uint8_t>((pUnit->Owner->ArrayIndex & 0x07) + 1);
	}

	for (auto const pInf : InfantryClass::Array)
	{
		if (!pInf || pInf->InLimbo || !pInf->IsAlive || !pInf->Owner || pInf->Owner->IsNeutral())
			continue;
		trackBounds(pInf->GetMapCoords());
	}

	if (minMapX > maxMapX)
	{
		minMapX = 0;
		maxMapX = 256;
		minMapY = 0;
		maxMapY = 256;
	}
	else
	{
		minMapX = std::max(0, minMapX - 10);
		minMapY = std::max(0, minMapY - 10);
		maxMapX += 10;
		maxMapY += 10;
	}
	const int spanX = std::max(32, maxMapX - minMapX);
	const int spanY = std::max(32, maxMapY - minMapY);

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
			             dst->SlotIndex, dst->ProcessId, dst->CurrentFrame, dst->FramesPerSecond,
			             matchEnded ? "true" : "false", dst->WinnerHouseIndex, stalemate ? "true" : "false",
			             NeuralAI::Backpack.EstimatedELO, NeuralAI::Backpack.GamesPlayed,
			             static_cast<unsigned long long>(NeuralAI::CurrentRulesSignature),
			             static_cast<unsigned long long>(NeuralAI::CurrentBaseSignature));

			for (uint32_t i = 0; i < dst->NumHouses; ++i)
			{
				const auto& h = dst->Houses[i];
				auto const pOwner = (h.ArrayIndex >= 0 && h.ArrayIndex < HouseClass::Array.Count)
				                        ? HouseClass::Array.GetItem(h.ArrayIndex)
				                        : nullptr;
				int defenses = 0;
				std::map<const BuildingTypeClass*, int> bldComp;

				if (pOwner)
				{
					for (auto const pOwned : pOwner->Buildings)
					{
						if (pOwned && pOwned->Type && !pOwned->InLimbo && pOwned->IsAlive)
						{
							if (pOwned->Type->BuildCat == BuildCat::Combat)
								defenses++;
							bldComp[pOwned->Type]++;
						}
					}
				}

				std::vector<std::pair<int, const BuildingTypeClass*>> rankedBlds;
				for (auto const& bEntry : bldComp)
					rankedBlds.emplace_back(bEntry.second, bEntry.first);
				std::sort(rankedBlds.begin(), rankedBlds.end(),
				          [](auto const& a, auto const& b) { return a.first > b.first; });

				std::string bldText;
				for (size_t rb = 0; rb < rankedBlds.size() && rb < 8; ++rb)
				{
					if (rb > 0)
						bldText += ";";
					bldText += rankedBlds[rb].second->ID;
					bldText += ":" + std::to_string(rankedBlds[rb].first);
				}

				std::map<const TechnoTypeClass*, int> composition;

				if (pOwner)
				{
					for (auto const pOwnedUnit : UnitClass::Array)
					{
						if (pOwnedUnit && pOwnedUnit->Owner == pOwner && pOwnedUnit->Type && !pOwnedUnit->InLimbo &&
						    pOwnedUnit->IsAlive)
							composition[pOwnedUnit->Type]++;
					}

					for (auto const pOwnedInf : InfantryClass::Array)
					{
						if (pOwnedInf && pOwnedInf->Owner == pOwner && pOwnedInf->Type && !pOwnedInf->InLimbo &&
						    pOwnedInf->IsAlive)
							composition[pOwnedInf->Type]++;
					}

					for (auto const pOwnedAir : AircraftClass::Array)
					{
						if (pOwnedAir && pOwnedAir->Owner == pOwner && pOwnedAir->Type && !pOwnedAir->InLimbo &&
						    pOwnedAir->IsAlive)
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
				             "    {\"idx\": %d, \"side\": %d, \"country\": %d, \"archetype\": "
				             "%d, \"expert\": %d, \"credits\": %d, \"killedUnits\": %d, "
				             "\"killedBlds\": %d, \"ownedUnits\": %d, \"ownedBlds\": %d, "
				             "\"score\": %d, \"winner\": %u, \"defeated\": %u, \"policy\": %u, "
				             "\"conYards\": %d, \"harvesters\": %d, \"refineries\": %d, "
				             "\"warFactories\": %d, \"barracks\": %d, \"defenses\": %d, "
				             "\"blds\": \"%s\", \"units\": \"%s\"}%s\n",
				             h.ArrayIndex, h.SideIndex, h.CountryIndex, h.Archetype, h.ActiveExpert, h.Credits,
				             h.KilledUnits, h.KilledBuildings, h.OwnedUnits, h.OwnedBuildings, h.NetWorthScore,
				             h.IsWinner, h.IsDefeated, h.PolicySlot, pOwner ? pOwner->NumConYards : 0,
				             pOwner ? pOwner->CountResourceGatherers : 0,
				             pOwner ? pOwner->CountResourceDestinations : 0, pOwner ? pOwner->NumWarFactories : 0,
				             pOwner ? pOwner->NumBarracks : 0, defenses, bldText.c_str(), compositionText.c_str(),
				             (i + 1 < dst->NumHouses) ? "," : "");
			}

			std::fprintf(fp, "  ],\n  \"radar\": [");
			bool firstPt = true;
			int ptCount = 0;

			for (auto const pBld : BuildingClass::Array)
			{
				if (!pBld || pBld->InLimbo || !pBld->IsAlive || !pBld->Owner || pBld->Owner->IsNeutral() ||
				    pBld->Owner->IsObserver())
					continue;
				if (ptCount >= 220)
					break;
				const CellStruct c = pBld->GetMapCoords();
				const int rx = std::clamp(6 + ((c.X - minMapX) * 115) / spanX, 4, 123);
				const int ry = std::clamp(6 + ((c.Y - minMapY) * 115) / spanY, 4, 123);
				std::fprintf(fp, "%s[%d,%d,%d,1]", firstPt ? "" : ",", rx, ry, pBld->Owner->ArrayIndex);
				firstPt = false;
				ptCount++;
			}

			for (auto const pUnit : UnitClass::Array)
			{
				if (!pUnit || pUnit->InLimbo || !pUnit->IsAlive || !pUnit->Owner || pUnit->Owner->IsNeutral() ||
				    pUnit->Owner->IsObserver())
					continue;
				if (ptCount >= 400)
					break;
				const CellStruct c = pUnit->GetMapCoords();
				const int rx = std::clamp(6 + ((c.X - minMapX) * 115) / spanX, 4, 123);
				const int ry = std::clamp(6 + ((c.Y - minMapY) * 115) / spanY, 4, 123);
				std::fprintf(fp, "%s[%d,%d,%d,0]", firstPt ? "" : ",", rx, ry, pUnit->Owner->ArrayIndex);
				firstPt = false;
				ptCount++;
			}

			int infStep = 0;
			for (auto const pInf : InfantryClass::Array)
			{
				if (!pInf || pInf->InLimbo || !pInf->IsAlive || !pInf->Owner || pInf->Owner->IsNeutral() ||
				    pInf->Owner->IsObserver())
					continue;
				if ((infStep++ % 2) != 0)
					continue;
				if (ptCount >= 480)
					break;
				const CellStruct c = pInf->GetMapCoords();
				const int rx = std::clamp(6 + ((c.X - minMapX) * 115) / spanX, 4, 123);
				const int ry = std::clamp(6 + ((c.Y - minMapY) * 115) / spanY, 4, 123);
				std::fprintf(fp, "%s[%d,%d,%d,0]", firstPt ? "" : ",", rx, ry, pInf->Owner->ArrayIndex);
				firstPt = false;
				ptCount++;
			}

			std::fprintf(fp, "]\n}\n");
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
	// We ONLY inspect enemy objects that are on unshrouded cells, or houses
	// infiltrated by our Spies.
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
	int visibleEnemyArtillery = 0;
	int visibleEnemyMindControllers = 0;
	int visibleEnemyHarvesters = 0;
	int neutralCapturableTechs = 0;
	int emptyUrbanOccupiables = 0;
	int damagedUrbanGarrisons = 0;
	int ownHeavyAndSiegeCount = 0;
	int ownDisguiseAndDetectorUnits = 0;
	int enemySlaveMinersAndOwnOverpowerCoils = 0;
	int ownRadDeployersAndGapStealthUnits = 0;
	int parasiteWarfareScore = 0;
	int grinderAndMindCapturedUnits = 0;
	int empUnitsAndEMPedEnemies = 0;
	int tankBunkersAndAmmoReloadUnits = 0;
	int sniperPoisonChaosAndTiberiumHealers = 0;
	int submarinesAndAntiSubDestroyers = 0;
	int dyingLoadedTransportsCount = 0;
	int ownTemporaledOrIvanBombedCount = 0;
	int occupiedTankBunkersAndDrainedBlds = 0;
	int amphibiousFleetAndBeachheadScore = 0;
	int woundedFrontlineAndFocusTargets = 0;
	int enemyExposedPowerAndFactories = 0;
	int foreignConYardAndCaptureScore = 0;
	int camouflageAmbushAndGroveScore = 0;
	int ownDamagedBuildings = 0;
	int ownDamagedVehicles = 0;
	int ownHarvestersUnderAttack = 0;
	const DWORD ownCountryBit = (pHouse->Type ? (1u << pHouse->Type->ArrayIndex2) : 0);
	const BuildingTypeClass* pPrimaryConYardType =
	    (pHouse->ConYards.Count > 0 && pHouse->ConYards.GetItem(0)) ? pHouse->ConYards.GetItem(0)->Type : nullptr;

	for (auto const pBld : BuildingClass::Array)
	{
		if (!pBld || pBld->InLimbo || !pBld->IsAlive || !pBld->Type)
			continue;

		if (pBld->Type->BridgeRepairHut && MapClass::Instance.IsLinkedBridgeDestroyed(pBld->GetMapCoords()))
			destroyedBridgesCount++;

		if (pBld->Owner && pBld->Owner->IsNeutral() && pBld->Type->Capturable)
			neutralCapturableTechs++;

		if (pBld->Type->CanBeOccupied)
		{
			if (pBld->Occupants.Count == 0)
			{
				emptyUrbanOccupiables++;
				if (pBld->Type->Repairable && pBld->GetHealthPercentage() < 0.72)
					damagedUrbanGarrisons++;
			}
			else if (pBld->Occupants.GetItem(0) && pHouse->IsAlliedWith(pBld->Occupants.GetItem(0)->Owner) &&
			         pBld->Type->Repairable && pBld->GetHealthPercentage() < 0.75)
			{
				damagedUrbanGarrisons++;
			}
		}

		if (pBld->Owner == pHouse)
		{
			if (pBld->GetHealthPercentage() < 0.75)
				ownDamagedBuildings++;
			if (pBld->Type->Overpowerable && (!pBld->IsOverpowered || pHouse->HasLowPower()))
				enemySlaveMinersAndOwnOverpowerCoils++;
			if (pBld->Type->Grinding)
				grinderAndMindCapturedUnits += 2;
			if (pBld->Type->Bunker)
			{
				if (pBld->BunkerLinkedItem == nullptr)
					tankBunkersAndAmmoReloadUnits += 2;
				else
					occupiedTankBunkersAndDrainedBlds++;
			}
			if (pBld->DrainingMe != nullptr)
				occupiedTankBunkersAndDrainedBlds += 3;
			if (pBld->TemporalTargetingMe != nullptr || pBld->AttachedBomb != nullptr)
				ownTemporaledOrIvanBombedCount += 3;
			if ((pBld->Type->ConstructionYard || pBld->Type->Factory != AbstractType::None) &&
			    ((pPrimaryConYardType && pBld->Type->ConstructionYard && pBld->Type != pPrimaryConYardType) ||
			     (ownCountryBit != 0 && !pBld->Type->InOwners(ownCountryBit))))
			{
				foreignConYardAndCaptureScore += 3;
			}
			if (IsTunnelNetworkBuilding(pBld->Type))
			{
				amphibiousFleetAndBeachheadScore += 3;
				foreignConYardAndCaptureScore += 2;
				camouflageAmbushAndGroveScore += 3;
			}
		}
	}

	int ownReadySupers = 0;
	int enemyImminentSuperUrgency = 0;
	for (auto const pOtherHouse : HouseClass::Array)
	{
		if (!pOtherHouse || pOtherHouse->IsNeutral() || pOtherHouse->IsObserver() || pOtherHouse->Defeated)
			continue;
		const bool isEnemyH = (pOtherHouse != pHouse && !pHouse->IsAlliedWith(pOtherHouse));
		for (auto const pSuper : pOtherHouse->Supers)
		{
			if (!pSuper || !pSuper->Type || !pSuper->Type->DisableableFromShell)
				continue;
			if (pOtherHouse == pHouse && pSuper->IsReady)
				ownReadySupers++;
			else if (isEnemyH && pSuper->IsPresent)
			{
				const int left = pSuper->RechargeTimer.GetTimeLeft();
				if (pSuper->IsReady || (left > 0 && left <= 900))
					enemyImminentSuperUrgency = std::max(enemyImminentSuperUrgency, 110);
				else if (left > 0 && left <= 2700)
					enemyImminentSuperUrgency = std::max(enemyImminentSuperUrgency, 65);
				else
					enemyImminentSuperUrgency = std::max(enemyImminentSuperUrgency, 30);
			}
		}
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
			if (auto const pOwnU = abstract_cast<UnitClass*>(pTechno))
			{
				if (pOwnU->Type)
				{
					if ((pOwnU->Type->Harvester || pOwnU->Type->ResourceGatherer) &&
					    pOwnU->GetHealthPercentage() < 0.85)
						ownHarvestersUnderAttack++;
					else if (pOwnU->Type->Cost >= 700 && pOwnU->GetHealthPercentage() < 0.75)
						ownDamagedVehicles++;

					if (!pOwnU->Type->Harvester && !pOwnU->Type->ResourceGatherer)
					{
						const auto pOWS = pOwnU->Type->GetWeapon(0);
						const auto pOW = pOWS ? pOWS->WeaponType : nullptr;
						if (pOwnU->Type->Cost >= 1000 || (pOW && (pOW->Range >= (8 * 256) || pOW->Spawner)))
							ownHeavyAndSiegeCount++;
					}
					if (pOwnU->Type->GapGenerator)
						ownRadDeployersAndGapStealthUnits++;
					if (pOwnU->Type->Passengers >= 4)
						amphibiousFleetAndBeachheadScore++;
					if (pOwnU->Type->DeploysInto && pOwnU->Type->DeploysInto->ConstructionYard && pPrimaryConYardType &&
					    pOwnU->Type->DeploysInto != pPrimaryConYardType)
					{
						foreignConYardAndCaptureScore += 4;
					}
					if (!pOwnU->Type->Naval && !pOwnU->Type->Harvester && !pOwnU->Type->ResourceGatherer &&
					    (pOwnU->Type->CanDisguise || pOwnU->Type->PermaDisguise || pOwnU->Type->Cloakable))
					{
						camouflageAmbushAndGroveScore += 3;
					}
				}
			}
			if (pTechno->MindControlledBy != nullptr)
				grinderAndMindCapturedUnits += 2;
			if (pTechno->TemporalTargetingMe != nullptr || pTechno->AttachedBomb != nullptr)
				ownTemporaledOrIvanBombedCount += 3;
			if (pTechno->Passengers.NumPassengers > 0 && pTechno->GetHealthPercentage() < 0.35)
				dyingLoadedTransportsCount += 3;
			if (pTechno->Target != nullptr && pTechno->GetHealthPercentage() < 0.45)
				woundedFrontlineAndFocusTargets++;
			if (auto const pOwnTType = pTechno->GetTechnoType())
			{
				if (pOwnTType->CanDisguise || pOwnTType->DetectDisguise)
					ownDisguiseAndDetectorUnits++;
				if (pOwnTType->ManualReload && pOwnTType->Ammo > 0 && pTechno->Ammo == 0)
					tankBunkersAndAmmoReloadUnits += 2;
				if (pOwnTType->TiberiumHeal && pTechno->GetHealthPercentage() < 0.65)
					sniperPoisonChaosAndTiberiumHealers += 2;
				if (pOwnTType->Naval && (pOwnTType->Cloakable || pOwnTType->Sensors))
					submarinesAndAntiSubDestroyers += 2;
				const auto pOwnWS = pOwnTType->GetWeapon(0);
				const auto pOwnW = pOwnWS ? pOwnWS->WeaponType : nullptr;
				if (pOwnW && pOwnW->Warhead)
				{
					if (pOwnW->Warhead->Parasite || pOwnW->Warhead->Sonic || pOwnW->Damage < 0)
						parasiteWarfareScore++;
					if (pOwnW->Warhead->EMEffect)
						empUnitsAndEMPedEnemies += 2;
					if (pOwnW->Warhead->Poison || pOwnW->Warhead->Psychedelic)
						sniperPoisonChaosAndTiberiumHealers += 2;
				}
				if (auto const pOwnIType = abstract_cast<InfantryTypeClass*>(pOwnTType))
				{
					if (pOwnIType->Deployer && pOwnIType->ImmuneToRadiation)
						ownRadDeployersAndGapStealthUnits++;
				}
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
		const bool cellVisibleToAI = pHouse->SpySatActive || IsCellVisibleToAI(pHouse, pCell) || spyInfiltrated ||
		                             (pOwner->WhoLastHurtMe == pHouse->ArrayIndex);

		if (!cellVisibleToAI)
		{
			if (auto const pHiddenTType = pTechno->GetTechnoType())
			{
				const bool isHiddenThreat =
				    (pTechno->CloakState != CloakState::Uncloaked || pTechno->Disguised || pHiddenTType->Cloakable ||
				     pHiddenTType->CanDisguise || pHiddenTType->PermaDisguise);
				if (isHiddenThreat &&
				    (pOwner->OwnedBuildings == 0 ||
				     (Unsorted::CurrentFrame > 5400 && (Unsorted::CurrentFrame - NeuralAI::LastCombatFrame) >= 3600)))
				{
					visibleEnemyStealthUnits += 2;
				}
			}
			continue;
		}

		pExt->AIBrain_ScoutedEnemyMask |= (1 << ownerIdx);

		if (pTechno->OnBridge && pCell->ContainsBridge())
			visibleEnemiesOnBridge++;
		if (pTechno->Veterancy.IsElite())
			visibleEnemyElites++;
		if (auto const pTType = pTechno->GetTechnoType())
		{
			if (pTType->Cloakable || pTechno->CloakState != CloakState::Uncloaked || pTechno->Disguised ||
			    pTType->CanDisguise || pTType->PermaDisguise)
				visibleEnemyStealthUnits++;
			const auto pWS = pTType->GetWeapon(0);
			const auto pW = pWS ? pWS->WeaponType : nullptr;
			if (pW && pW->Range >= (8 * 256) && pTechno->WhatAmI() != AbstractType::Building)
				visibleEnemyArtillery++;
			if (pW && pW->Warhead && pW->Warhead->MindControl)
				visibleEnemyMindControllers++;
			if (pTechno->Disguised)
				ownDisguiseAndDetectorUnits += 2;
			if (pTType->Enslaves != nullptr)
				enemySlaveMinersAndOwnOverpowerCoils += 2;
			if (pTechno->IsUnderEMP())
				empUnitsAndEMPedEnemies += 2;
			if (auto const pEIType = abstract_cast<InfantryTypeClass*>(pTType))
			{
				if (pEIType->Engineer || pEIType->Infiltrate || pEIType->C4 || pEIType->Ivan || pEIType->Deployer)
					sniperPoisonChaosAndTiberiumHealers++;
			}
			if (auto const pETUnit = abstract_cast<UnitTypeClass*>(pTType))
			{
				if (pETUnit->Harvester || pETUnit->ResourceGatherer)
					visibleEnemyHarvesters++;
			}
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
				visibleEnemyAirShips++; // Kirov Airship / Floating Disc / Heavy Bomber
				                        // alert!
			pExt->AIBrain_EstimatedEnemyThreat[ownerIdx] += 30;
			break;
		case AbstractType::Building:
		{
			auto const pBld = static_cast<BuildingClass*>(pTechno);
			auto const pBldType = pBld->Type;
			if (pBldType && pBldType->Immune)
				break; // Never waste firepower on Immune structures (e.g. Immune Civ
				       // Techs / Immune Derricks / CABHUT)

			visibleEnemyBuildings++;
			if (pBld->Occupants.Count > 0)
				visibleEnemyGarrisons++;
			if (pBldType && pBldType->BuildCat == BuildCat::Combat)
				visibleEnemyDefenses++;
			if (pBldType && (pBldType->PowerBonus > 0 || pBldType->SuperWeapon != -1 ||
			                 pBldType->Factory != AbstractType::None || pBldType->ConstructionYard))
				enemyExposedPowerAndFactories++;
			if (pBldType && pBldType->Capturable &&
			    (pBldType->ConstructionYard || pBldType->Factory != AbstractType::None || pBldType->TechLevel >= 6))
			{
				foreignConYardAndCaptureScore += (pBldType->ConstructionYard ? 3 : 1);
			}
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
	features[75] = ClampInt8(enemyImminentSuperUrgency);
	features[76] = ClampInt8(visibleEnemyArtillery * 25);
	features[77] = ClampInt8(visibleEnemyMindControllers * 35);
	features[78] = ClampInt8(visibleEnemyHarvesters * 25);
	features[79] = ClampInt8(damagedUrbanGarrisons * 30);
	features[80] = ClampInt8(ownHeavyAndSiegeCount * 15);
	features[81] = ClampInt8(ownDisguiseAndDetectorUnits * 20);
	features[82] = ClampInt8(enemySlaveMinersAndOwnOverpowerCoils * 25);
	features[83] = ClampInt8(ownRadDeployersAndGapStealthUnits * 25);
	features[84] = ClampInt8((parasiteWarfareScore + ownParasitedOrControlled * 2) * 20);
	features[85] = ClampInt8(grinderAndMindCapturedUnits * 25);
	features[86] = ClampInt8(empUnitsAndEMPedEnemies * 25);
	features[87] = ClampInt8(tankBunkersAndAmmoReloadUnits * 25);
	features[88] = ClampInt8(sniperPoisonChaosAndTiberiumHealers * 20);
	features[89] = ClampInt8((submarinesAndAntiSubDestroyers + visibleEnemyNavy) * 18);
	features[90] = ClampInt8(dyingLoadedTransportsCount * 35);
	features[91] = ClampInt8(ownTemporaledOrIvanBombedCount * 35);
	features[92] = ClampInt8(occupiedTankBunkersAndDrainedBlds * 30);
	features[93] = ClampInt8((amphibiousFleetAndBeachheadScore * 2 + islandEnemyBuildings * 3 + visibleEnemyNavy) * 15);
	features[94] = ClampInt8((woundedFrontlineAndFocusTargets + visibleEnemyTanks) * 12);
	features[95] = ClampInt8((enemyExposedPowerAndFactories + visibleEnemyArtillery * 2) * 18);
	features[18] = ClampInt8(destroyedBridgesCount * 30);
	features[22] = ClampInt8(ownParasitedOrControlled * 40);
	features[23] = ClampInt8(ownEliteUnits * 20);
	features[24] = ClampInt8((pHouse->OwnedAircraft - (pHouse->NumAirpads * 4)) * 20);
	features[25] = ClampInt8(neutralCapturableTechs * 25);
	features[26] = ClampInt8(emptyUrbanOccupiables * 8);
	features[28] = ClampInt8(ownReadySupers * 60);
	features[29] = ClampInt8(ownDamagedBuildings * 25);
	features[30] = ClampInt8(ownDamagedVehicles * 25);
	features[31] = ClampInt8(ownHarvestersUnderAttack * 60);
	features[16] = GameModeOptionsClass::Instance.MCVRedeploy
	                   ? ClampInt8((pHouse->NumConYards * 25) + (features[20] > 0 ? 40 : 15))
	                   : 0;
	features[17] = ClampInt8(foreignConYardAndCaptureScore * 20);

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

	// Scan local vs. uncolonized distant Ore/Tiberium patches across all owned
	// ConYards for Expert 6 (Multi-Outpost Expansion)
	CellStruct baseCenter = pHouse->BaseCenter != CellStruct::Empty ? pHouse->BaseCenter : pHouse->BaseSpawnCell;
	if (pHouse->ConYards.Count > 0)
		baseCenter = pHouse->ConYards.GetItem(pHouse->ConYards.Count - 1)->GetMapCoords();

	int localOreCells = 0;
	int expansionOreCells = 0;
	int islandExpansionOreCells = 0;
	int visibleCratesNearby = 0;
	int nearbyTreeGroveCells = 0;
	if (baseCenter != CellStruct::Empty)
	{
		for (int dy = -48; dy <= 48; dy += 3)
		{
			for (int dx = -48; dx <= 48; dx += 3)
			{
				const CellStruct tc{static_cast<short>(baseCenter.X + dx), static_cast<short>(baseCenter.Y + dy)};
				auto const pCell = MapClass::Instance.TryGetCellAt(tc);
				if (!pCell)
					continue;

				if (pCell->GetTerrain(false) != nullptr)
					nearbyTreeGroveCells++;

				if (pCell->OverlayTypeIndex >= 0 && pCell->OverlayTypeIndex < OverlayTypeClass::Array.Count &&
				    IsCellVisibleToAI(pHouse, pCell))
				{
					if (auto const pOvl = OverlayTypeClass::Array.GetItem(pCell->OverlayTypeIndex))
					{
						if (pOvl->Crate)
							visibleCratesNearby++;
					}
				}

				if (pCell->GetContainedTiberiumValue() <= 0)
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
	features[27] = ClampInt8(visibleCratesNearby * 45);
	features[98] = ClampInt8(camouflageAmbushAndGroveScore * 16 + std::min(40, nearbyTreeGroveCells * 2) +
	                         visibleEnemyHarvesters * 12);
	if (features[21] > 0 && features[67] < 40)
		features[67] = 40; // Ensure Shipyard is prioritized whenever an enemy or
		                   // Ore expansion is on a disconnected island!

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

void NeuralAI::RunDeterministicMoE(HouseClass* pHouse, const std::array<int8_t, InputVectorSize>& features,
                                   std::array<int16_t, OutputVectorSize>& outScores)
{
	outScores.fill(0);
	auto const pExt = HouseExt::Fetch(pHouse);
	const AIBackpackFile& policy = NeuralAI::PolicyFor(pHouse);

	// Select active specialist expert (0..7) combining situational priors +
	// evolved Expert Routing Bias (Slots 40..47) + learned Faction Memory
	std::array<int, 8> expScores = {100, 100, 100, 80, 100, 160, 110, 110};
	if (pHouse->HasLowPower() || pHouse->CountResourceGatherers == 0 || pHouse->CountResourceDestinations == 0 ||
	    pHouse->NumConYards == 0)
		expScores[0] += 950; // Expert 0: Economy, Silos & Crisis Micro
	if (pHouse->NumBarracks == 0 || pHouse->NumWarFactories == 0 ||
	    (pHouse->Available_Money() > 2500 && pHouse->OwnedBuildings < 8))
		expScores[1] += 700; // Expert 1: Spatial Tetris Base Architect
	if (features[66] > 24 || features[65] > 36)
		expScores[2] += 420; // Expert 2: Dynamic Counter-Strategy
	if (HouseClass::Array.Count > 4 && (Unsorted::CurrentFrame % 240 < 60))
		expScores[3] += 380; // Expert 3: Multi-Player Opportunism & Diplomacy
	if (pExt->AIBrain_Archetype == 1 || (Unsorted::CurrentFrame % 300 < 60 && pHouse->OwnedUnits >= 4))
		expScores[4] += 360; // Expert 4: Special Operations & Trickster
	if (pHouse->OwnedUnits >= 3 || pHouse->OwnedInfantry >= 5)
		expScores[5] += 300; // Expert 5: Tactical Squad Micro, Kiting & Beacons
	if ((pHouse->CountResourceDestinations < 3 || features[19] < 24) && features[20] > 10 &&
	    pHouse->Available_Money() >= 1800)
		expScores[6] += 480; // Expert 6: Base-Crawling & Ore Outpost Expansion
	if ((features[68] > 16 || pExt->AIBrain_Archetype == 2 || pExt->AIBrain_Archetype == 4) &&
	    pHouse->OwnedBuildings >= 7)
		expScores[7] += 440; // Expert 7: Forward Defense Crawling & Chokepoint Siege

	const int curArch = std::clamp(pExt->AIBrain_Archetype, 0, 4);
	int activeExpert = 0;
	int bestExpScore = -999999;
	for (int e = 0; e < 8; ++e)
	{
		int s = expScores[e];
		if (NeuralAI::AllowBackpackInference && NeuralAI::BackpackLoaded)
		{
			s += static_cast<int>(policy.OutputBias[40 + e]);
			const uint32_t eIdx = static_cast<uint32_t>((pHouse->SideIndex * 40 + curArch * 8 + e) % 512);
			if (policy.Entries[eIdx].VisitCount > 0)
				s += static_cast<int>(policy.Entries[eIdx].ExpertScoreDelta[e]) * 6;
		}
		if (s > bestExpScore)
		{
			bestExpScore = s;
			activeExpert = e;
		}
	}

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

		const size_t requiredBytes =
		    static_cast<size_t>(offset) + (NeuralAI::InputVectorSize * NeuralAI::OutputVectorSize);

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

	// Apply Signed Backpack LoRA delta & Learned Faction Strategy Memory in
	// Skirmish / Headless Training
	if (NeuralAI::AllowBackpackInference && NeuralAI::BackpackLoaded)
	{
		std::array<int32_t, 16> rankAct = {};
		for (int r = 0; r < 16; ++r)
		{
			int32_t acc = 0;
			for (int i = 0; i < NeuralAI::InputVectorSize; ++i)
				acc += static_cast<int32_t>(features[i]) *
				       static_cast<int32_t>(policy.LoRA_A[r * NeuralAI::InputVectorSize + i]);

			rankAct[r] = acc >> 8;
		}

		for (int o = 0; o < NeuralAI::OutputVectorSize; ++o)
		{
			int32_t delta = 0;
			for (int r = 0; r < 16; ++r)
				delta += rankAct[r] * static_cast<int32_t>(policy.LoRA_B[r * NeuralAI::OutputVectorSize + o]);

			outScores[o] = ClampInt16(outScores[o] + (delta >> 7) + policy.OutputBias[o]);
		}

		// Recall learned strategy outcome deltas from Backpack Entries for this
		// Faction + Archetype + Expert
		const uint32_t entryIdx = static_cast<uint32_t>((pHouse->SideIndex * 40 + curArch * 8 + activeExpert) % 512);
		const auto& memEntry = policy.Entries[entryIdx];
		if (memEntry.VisitCount > 0)
		{
			const int16_t learnedBoost =
			    ClampInt16(memEntry.ArchetypeScoreDelta[curArch] * 4 + memEntry.ExpertScoreDelta[activeExpert] * 4);
			for (int o = 16; o < 36; ++o)
				outScores[o] = ClampInt16(outScores[o] + learnedBoost);

			// If this Archetype has a negative win/loss record in the Backpack for
			// this faction, pivot to the highest-scoring alternative Archetype
			// (weighted by Slots 48..52)
			if (memEntry.ArchetypeScoreDelta[curArch] < -18 && (Unsorted::CurrentFrame % 900 < 30))
			{
				int bestAltArch = (curArch + 1) % 5;
				int bestAltScore = -999999;
				for (int a = 0; a < 5; ++a)
				{
					if (a == curArch)
						continue;
					const int altScore =
					    static_cast<int>(outScores[48 + a]) + static_cast<int>(memEntry.ArchetypeScoreDelta[a]) * 6;
					if (altScore > bestAltScore)
					{
						bestAltScore = altScore;
						bestAltArch = a;
					}
				}
				pExt->AIBrain_Archetype = bestAltArch;
			}
		}
	}

	// Mid-Match Adaptive Strategy Pivot: If our current strategy is stalled (e.g.
	// high frame count, low army vs strong enemy), pivot to the strongest
	// alternative Archetype (weighted by Slots 48..52) to surprise the opponent!
	if (Unsorted::CurrentFrame > 5400 && (Unsorted::CurrentFrame % 2700 == 0) && pHouse->OwnedUnits < 4 &&
	    features[65] > 20)
	{
		int bestPivotArch = (pExt->AIBrain_Archetype + 1) % 5;
		int bestPivotScore = -999999;
		for (int a = 0; a < 5; ++a)
		{
			if (a == pExt->AIBrain_Archetype)
				continue;
			const int pScore = static_cast<int>(outScores[48 + a]);
			if (pScore > bestPivotScore)
			{
				bestPivotScore = pScore;
				bestPivotArch = a;
			}
		}
		pExt->AIBrain_Archetype = bestPivotArch;
	}

	// Controlled Deterministic Exploration: Inject a small synchronized variation
	// across unit/infantry/aircraft action slots so the AI experiments with
	// alternative unit compositions and tech branches rather than repeating a
	// rigid script
	const int exploreSeed = ((Unsorted::CurrentFrame / 450) * 31 + pHouse->ArrayIndex * 17 + pHouse->SideIndex * 13);
	for (int o = 16; o <= 33; ++o)
	{
		const int exploreBonus = (((exploreSeed + o * 19) % 11) - 5) * 28; // -140 .. +140 exploration window
		outScores[o] = ClampInt16(outScores[o] + exploreBonus);
	}

	// Apply League Archetype & Expert structural priors so the AI executes crisp
	// RTS build orders immediately while integrating all 64 Output Slots:
	const int arch = pExt->AIBrain_Archetype;
	outScores[0] =
	    ClampInt16(outScores[0] +
	               (pHouse->HasLowPower() ? 2200 : ((pHouse->PowerOutput - pHouse->PowerDrain < 60) ? 1050 : -350)));
	outScores[1] = ClampInt16(
	    outScores[1] +
	    ((pHouse->CountResourceDestinations == 0)
	         ? 2100
	         : ((pHouse->CountResourceDestinations < 2)
	                ? 1150
	                : ((pHouse->CountResourceDestinations < 3 && pHouse->Available_Money() > 2200) ? 650 : -150))));
	outScores[2] = ClampInt16(outScores[2] + ((pHouse->NumBarracks == 0) ? 1750 : -200));
	outScores[3] = ClampInt16(outScores[3] +
	                          ((pHouse->NumWarFactories == 0)
	                               ? 1950
	                               : ((pHouse->Available_Money() > 3200 && pHouse->NumWarFactories < 2) ? 850 : -100)));
	outScores[4] = ClampInt16(outScores[4] + ((pHouse->NumWarFactories > 0) ? 1150 : 100));
	outScores[5] = ClampInt16(outScores[5] + ((features[65] > 18 || arch == 2) ? 420 : 120));
	outScores[6] = ClampInt16(outScores[6] + ((features[66] > 12) ? 750 : 80));
	outScores[7] = ClampInt16(outScores[7] +
	                          (((features[67] > 10 || features[21] > 0) && pHouse->NumShipyards == 0) ? 1550 : -200));
	outScores[8] = ClampInt16(
	    outScores[8] + ((pHouse->NumWarFactories > 0 && pHouse->NumAirpads == 0) ? ((arch == 4) ? 1100 : 550) : -150));
	outScores[9] = ClampInt16(outScores[9] + ((pHouse->NumWarFactories > 0 && pHouse->CountResourceDestinations >= 1 &&
	                                           pHouse->UnitRepairStations.Count == 0)
	                                              ? 1050
	                                              : -250));
	outScores[10] = ClampInt16(
	    outScores[10] + ((GameModeOptionsClass::Instance.SWAllowed && pHouse->Available_Money() > 4500) ? 850 : -600));
	outScores[11] = ClampInt16(outScores[11] + ((pHouse->IsTiberiumShort || features[14] < 0) ? 1450 : -300));

	// Macro Scaling & Tech/Defense/Naval Bias Slots (12..15): 12: Dual Factory
	// Production Scaling (2nd War Factory / 2nd Barracks) 13: Early Tech Center &
	// Battle Lab Rush 14: Base Defense Perimeter Density 15: Naval Supremacy &
	// Amphibious Commitment
	outScores[12] = ClampInt16(outScores[12] + ((pHouse->Available_Money() > 2600) ? 240 : -80));
	outScores[13] = ClampInt16(outScores[13] + ((arch == 2 || arch == 4) ? 280 : 60));
	outScores[14] = ClampInt16(outScores[14] + ((arch == 2 || activeExpert == 7) ? 260 : 40));
	outScores[15] = ClampInt16(outScores[15] + ((features[67] > 10 || features[21] > 0) ? 350 : -120));

	outScores[2] = ClampInt16(outScores[2] + std::clamp(static_cast<int>(outScores[12]) / 3, -150, 300));
	outScores[3] = ClampInt16(outScores[3] + std::clamp(static_cast<int>(outScores[12]) / 2, -200, 450));
	outScores[4] = ClampInt16(outScores[4] + std::clamp(static_cast<int>(outScores[13]) / 2, -250, 500));
	outScores[5] = ClampInt16(outScores[5] + std::clamp(static_cast<int>(outScores[14]) / 2, -250, 450));
	outScores[6] = ClampInt16(outScores[6] + std::clamp(static_cast<int>(outScores[14]) / 3, -200, 350));
	outScores[7] = ClampInt16(outScores[7] + std::clamp(static_cast<int>(outScores[15]) / 2, -250, 500));
	outScores[10] = ClampInt16(outScores[10] + std::clamp(static_cast<int>(outScores[55]) / 2, -300, 500));

	outScores[16] = ClampInt16(outScores[16] + ((arch == 3) ? 600 : 350));
	outScores[17] = ClampInt16(outScores[17] + ((features[65] > 12) ? 750 : 400));
	outScores[18] = ClampInt16(
	    outScores[18] +
	    ((arch == 1 || pHouse->ToCapture >= 0 || (features[18] > 0 && pHouse->OwnedUnits >= 5)) ? 650 : 250));
	outScores[19] = ClampInt16(outScores[19] + ((arch == 1 && pExt->AIBrain_InfiltratedEnemyMask == 0) ? 500 : 100));
	outScores[20] = ClampInt16(outScores[20] + ((features[66] > 12) ? 700 : 300));
	outScores[23] = ClampInt16(outScores[23] + ((arch == 2 || activeExpert == 7) ? 260 : 80));

	// Tactical & Spatial Strategy Output Slots (34..39) driven by Neural MoE +
	// LoRA + Evolved OutputBias:
	outScores[34] =
	    ClampInt16(outScores[34] + ((activeExpert == 6 || features[19] < 24 || features[20] > 15) ? 280 : 40));
	outScores[35] = ClampInt16(outScores[35] + ((activeExpert == 6 || features[20] > 12) ? 320 : 20));
	outScores[36] = ClampInt16(outScores[36] + ((arch == 1 || pHouse->ToCapture >= 0) ? 300 : 80));
	outScores[37] = ClampInt16(outScores[37] + ((arch == 3) ? 260 : ((arch == 2) ? -220 : 0)));
	outScores[38] = ClampInt16(outScores[38] + ((activeExpert == 7 || arch == 2) ? 280 : 40));
	outScores[39] =
	    ClampInt16(outScores[39] + ((activeExpert == 0 || activeExpert == 6) ? 220 : ((arch == 3) ? -220 : 0)));

	// Feed evolved tactical/spatial slots into production & expansion priorities
	outScores[1] =
	    ClampInt16(outScores[1] +
	               std::clamp(static_cast<int>(outScores[34]) / 3 + static_cast<int>(outScores[39]) / 2, -300, 550));
	outScores[3] = ClampInt16(outScores[3] + std::clamp(static_cast<int>(outScores[34]) / 3, -200, 400));
	outScores[5] = ClampInt16(outScores[5] + std::clamp(static_cast<int>(outScores[38]) / 2, -250, 500));
	outScores[6] = ClampInt16(outScores[6] + std::clamp(static_cast<int>(outScores[38]) / 3, -200, 350));
	outScores[18] = ClampInt16(outScores[18] + std::clamp(static_cast<int>(outScores[36]), -300, 750));

	// Slot 21: Repair Destroyed Bridge via Engineer + BridgeRepairHut (only when
	// ready to push offensively)
	if (features[18] > 0)
	{
		if (pHouse->OwnedUnits * 6 >= features[65] && pHouse->OwnedUnits >= 4)
			outScores[21] = ClampInt16(outScores[21] + 1200); // We have ground superiority: repair bridge
		else
			outScores[21] = ClampInt16(outScores[21] - 1500); // Outnumbered: keep broken bridge as a moat!
	}
	else
	{
		outScores[21] = -2000;
	}

	// Slot 22: Demolish Intact Bridge (Ambush enemy column on bridge, or
	// scorched-earth defense when outnumbered)
	if (features[134] > 0) // Bridges are destroyable (Immune Bridges GameOption is OFF)
	{
		if (features[70] > 0)
			outScores[22] = ClampInt16(outScores[22] + 1900);
		else if (features[65] > (pHouse->OwnedUnits * 6 + 18))
			outScores[22] = ClampInt16(outScores[22] + 1100);
		else
			outScores[22] = ClampInt16(outScores[22] - 1000);
	}
	else
	{
		outScores[22] = -3000; // Immune Bridges ON: never waste ammo shooting bridges
	}

	outScores[24] = ClampInt16(outScores[24] + 800 + (arch == 3 ? 300 : 0) -
	                           std::clamp(static_cast<int>(outScores[39]) / 2, -450, 350));
	outScores[25] = ClampInt16(outScores[25] + ((features[64] > 20) ? 700 : 300));
	outScores[26] = ClampInt16(outScores[26] + ((features[66] > 12) ? 950 : 250));
	outScores[27] = ClampInt16(outScores[27] + ((features[68] > 16 || arch == 4 || activeExpert == 7) ? 950 : 350));
	outScores[28] = ClampInt16(outScores[28] +
	                           ((features[21] > 0) ? 1450 : ((arch == 1 && pHouse->OwnedInfantry >= 3) ? 650 : 150)) +
	                           std::clamp(static_cast<int>(outScores[53]) / 2, -250, 450));
	outScores[29] = ClampInt16(outScores[29] + 600 + std::clamp(static_cast<int>(outScores[15]) / 2, -200, 450));

	// Slot 30: Multi-Outpost Expansion MCV (Expert 6 & Slot 35: Builds a 2nd,
	// 3rd, 4th... MCV to colonize distant Ore fields)
	const int mcvReserveThreshold =
	    std::clamp(2500 + (pHouse->NumConYards * 900) - (static_cast<int>(outScores[35]) * 2), 1100, 4400);
	if (pHouse->NumConYards >= 1 && pHouse->NumWarFactories > 0 && pHouse->Available_Money() > mcvReserveThreshold &&
	    (features[20] > 6 || outScores[35] > 140) &&
	    (features[19] < 32 || pHouse->Available_Money() > mcvReserveThreshold + 1800 || outScores[35] > 180))
	{
		outScores[30] = ClampInt16(outScores[30] + 1050 + std::clamp(static_cast<int>(outScores[35]), -350, 850));
	}
	else if (pHouse->NumConYards >= 1 && pHouse->NumWarFactories > 0 && pHouse->Available_Money() > 1600)
	{
		outScores[30] = ClampInt16(outScores[30] + std::clamp(static_cast<int>(outScores[35]) - 300, -900, 650));
	}
	else
	{
		outScores[30] = ClampInt16(outScores[30] - 1200);
	}

	// Slot 31: War Factory Harvester Boom (Dynamic Harvester-to-Refinery ratio
	// scaled by Ore/Gem abundance on map)
	const int refCount = std::max(1, pHouse->CountResourceDestinations);
	const bool richResources = (features[19] >= 32 || (features[19] + features[20]) >= 56);
	const bool scarceResources = (features[19] < 12 && features[20] < 18);
	const int desiredHarvesters = scarceResources ? refCount : std::min(10, refCount * (richResources ? 3 : 2));

	if (pHouse->CountResourceGatherers == 0)
		outScores[31] = ClampInt16(outScores[31] + 2600);
	else if (pHouse->CountResourceGatherers < desiredHarvesters)
		outScores[31] = ClampInt16(outScores[31] + (richResources ? 1650 : 1400));
	else
		outScores[31] = ClampInt16(outScores[31] - 1200);

	// Expert 6 & Expert 7 spatial specialization boosts
	if (activeExpert == 6)
	{
		outScores[1] = ClampInt16(outScores[1] + 650);
		outScores[3] = ClampInt16(outScores[3] + 350);
		outScores[31] = ClampInt16(outScores[31] + 300);
	}
	else if (activeExpert == 7)
	{
		outScores[5] = ClampInt16(outScores[5] + 600);
		outScores[6] = ClampInt16(outScores[6] + 450);
		outScores[27] = ClampInt16(outScores[27] + 400);
	}

	outScores[32] = ClampInt16(outScores[32] + ((arch == 4) ? 850 : 400));
	outScores[33] = ClampInt16(outScores[33] + ((arch == 4) ? 900 : 450));

	// New Real Tactical & Economic Hands (Slots 56..60):
	// Slot 56: Eco-Raid / Enemy Harvester Hunting & Own Harvester Defense
	outScores[56] = ClampInt16(outScores[56] + (features[78] * 8) + (features[31] * 10) +
	                           ((arch == 1 || arch == 3 || activeExpert == 4 || activeExpert == 5) ? 280 : 60));

	// Slot 57: Siege, Urban Garrison Repair/Assault & Combat Deploy/Undeploy
	outScores[57] = ClampInt16(outScores[57] + (features[73] * 10) + (features[79] * 9) + (features[68] * 6) +
	                           (features[76] * 6) + ((arch == 4 || activeExpert == 7) ? 320 : 80));
	outScores[27] = ClampInt16(outScores[27] + std::clamp(static_cast<int>(outScores[57]) / 3, -200, 450));
	if (features[79] > 0)
	{
		outScores[18] = ClampInt16(outScores[18] + 320); // Train Engineer to repair damaged urban strongholds!
		outScores[23] = ClampInt16(outScores[23] + 280); // Train Occupier infantry to hold repaired urban buildings!
	}

	// Slot 58: Tactical Kiting, Mind-Control Breaker, Tank Crush & Anti-SW
	// Scatter
	outScores[58] = ClampInt16(outScores[58] + (features[75] * 8) + (features[76] * 7) + (features[77] * 9) +
	                           (features[22] * 8) + ((activeExpert == 5) ? 340 : 100));

	// Slot 59: Smart Wrench Building Repair & Map Crate Pickup
	outScores[59] = ClampInt16(outScores[59] + (features[27] * 12) + (features[29] * 9) +
	                           ((pHouse->Available_Money() > 1400) ? 240 : -150));

	// Slot 60: AA & Sensor Escort, LowPower Defense Toggle & Spawner Standoff
	outScores[60] = ClampInt16(outScores[60] + (features[80] * 10) + (features[66] * 8) + (features[72] * 9) +
	                           (pHouse->HasLowPower() ? 360 : 80));
	outScores[26] = ClampInt16(outScores[26] + std::clamp(static_cast<int>(outScores[60]) / 3, -150, 380));

	// Slot 61: Dynamic Spy/Mirage Disguise (DisguiseAs) & Counter-Spy Detection
	// (DetectDisguise)
	outScores[61] = ClampInt16(outScores[61] + (features[81] * 10) + (features[72] * 8) +
	                           ((arch == 1 || activeExpert == 4) ? 320 : 90));

	// Slot 62: Slave Miner Liberation (Kill SlaveOwner -> Free Slaves) & Tesla
	// Coil Overpowering
	outScores[62] =
	    ClampInt16(outScores[62] + (features[82] * 11) + (features[78] * 6) + (pHouse->HasLowPower() ? 280 : 90));

	// Slot 63: Desolator Rad-Field Chokepoint Deploy & Mobile Gap/Stealth Shroud
	// Escort
	outScores[63] = ClampInt16(outScores[63] + (features[83] * 10) + (features[64] * 7) + (features[80] * 6) +
	                           ((activeExpert == 5 || activeExpert == 7) ? 260 : 80));

	// Slot 64: Parasite Warfare (Terror Drone/Squid), De-Parasiting
	// (Sonic/RemoveParasite/Repair) & Smart OpenTopped
	outScores[64] = ClampInt16(outScores[64] + (features[84] * 10) + (features[22] * 9) +
	                           ((activeExpert == 4 || activeExpert == 5) ? 280 : 90));

	// Slot 65: Grinder Mind-Control Recycling & Emergency Salvage
	outScores[65] = ClampInt16(outScores[65] + (features[85] * 11) + (features[77] * 6) +
	                           ((activeExpert == 0 || activeExpert == 4) ? 260 : 80));

	// Slot 66: EMP Pulse Targeting (Warhead->EMEffect) & EMP-Disabled Finisher
	// (IsUnderEMP)
	outScores[66] = ClampInt16(outScores[66] + (features[86] * 11) + (features[65] * 6) +
	                           ((activeExpert == 2 || activeExpert == 5) ? 280 : 90));

	// Slot 67: Tank Bunker Garrisoning (Bunker/Bunkerable) & Manual Ammo Reload
	// (ManualReload)
	outScores[67] = ClampInt16(outScores[67] + (features[87] * 10) + (features[68] * 6) +
	                           ((activeExpert == 1 || activeExpert == 7) ? 260 : 80));

	// Slot 68: Sniper/Poison Specialist Hunter, Chaos Gas (Psychedelic) &
	// TiberiumHeal Regeneration
	outScores[68] = ClampInt16(outScores[68] + (features[88] * 10) + (features[64] * 6) +
	                           ((activeExpert == 2 || activeExpert == 4) ? 280 : 90));

	// Slot 69: Submarine Cloak Ambush & Anti-Sub Destroyer Escort (Naval
	// Cloakable & Sensors)
	outScores[69] = ClampInt16(outScores[69] + (features[89] * 10) + (features[67] * 8) +
	                           ((arch == 4 || activeExpert == 2) ? 260 : 80));

	// Slot 70: Dying Transport / Battle Fortress Emergency Passenger Unload
	outScores[70] = ClampInt16(outScores[70] + (features[90] * 12) + (features[30] * 6) + 140);

	// Slot 71: Chrono Legionnaire Temporal Erasure Breaker (TemporalTargetingMe
	// -> Kill Owner)
	outScores[71] = ClampInt16(outScores[71] + (features[91] * 12) + 160);

	// Slot 72: Crazy Ivan AttachedBomb Disarm or Kamikaze Charge into Enemy Ranks
	outScores[72] = ClampInt16(outScores[72] + (features[91] * 11) + (features[64] * 5) + 140);

	// Slot 73: Dynamic Tank Bunker Unload (UnloadBunker on Low HP or Offensive
	// Wave)
	outScores[73] = ClampInt16(outScores[73] + (features[92] * 10) + (features[87] * 6) + 120);

	// Slot 74: Anti-Drain Floating Disc Interceptor (DrainingMe -> Priority #1 AA
	// Target)
	outScores[74] = ClampInt16(outScores[74] + (features[92] * 12) + (features[71] * 8) + 150);

	// Slot 75: Naval War Multi-Transport D-Day Convoy, Shuttle Ferry & Island MCV
	// Beachhead Factory
	outScores[75] = ClampInt16(outScores[75] + (features[93] * 11) + (features[21] * 10) +
	                           ((activeExpert == 4 || activeExpert == 6) ? 320 : 110));
	if (features[21] > 0 && pHouse->NumConYards == 1 && pHouse->Available_Money() > 2500)
	{
		outScores[30] = ClampInt16(outScores[30] + 650); // Queue Expansion MCV to colonize Enemy Island Beachhead!
		outScores[28] = ClampInt16(outScores[28] + 550); // Queue Amphibious Hover Transports for Convoy & MCV Ferry!
	}

	// Slot 76: Squad Focus-Fire on Wounded In-Range Enemy (Lanchester
	// Target-Fire)
	outScores[76] =
	    ClampInt16(outScores[76] + (features[94] * 10) + (features[65] * 6) + ((activeExpert == 5) ? 300 : 120));

	// Slot 77: Wounded Frontline Tank Pull-Back Micro (Step Behind Healthy Tanks)
	outScores[77] =
	    ClampInt16(outScores[77] + (features[94] * 9) + (features[30] * 8) + ((activeExpert == 5) ? 280 : 110));

	// Slot 78: High-Ground Cliff Advantage Positioning (CellClass::Level)
	outScores[78] =
	    ClampInt16(outScores[78] + (features[80] * 8) + (features[68] * 6) + ((activeExpert == 7) ? 280 : 100));

	// Slot 79: Surgical Enemy Power, Superweapon & Factory Snipe inside Enemy
	// Base
	outScores[79] = ClampInt16(outScores[79] + (features[95] * 11) + (features[75] * 8) +
	                           ((activeExpert == 4 || activeExpert == 5) ? 300 : 120));

	// Slot 80: Fast Flanker & Jumpjet Counter-Battery vs Enemy Artillery
	outScores[80] = ClampInt16(outScores[80] + (features[76] * 12) + (features[95] * 7) +
	                           ((arch == 3 || activeExpert == 2 || activeExpert == 5) ? 300 : 110));

	// Slot 81: Nomadic MCV Hop, Outpost Drop & Forward Siege Turret-Push
	// (MCVRedeploy)
	outScores[81] = ClampInt16(outScores[81] + (features[16] * 10) + (features[20] * 7) + (features[68] * 5) +
	                           ((activeExpert == 6 || activeExpert == 7) ? 320 : 100));

	// Slot 82: Enemy ConYard Heist, Stolen MCV Safe Extraction & Cross-Faction
	// Tech Tree
	outScores[82] = ClampInt16(outScores[82] + (features[17] * 11) + (features[95] * 6) +
	                           ((arch == 1 || activeExpert == 1 || activeExpert == 4) ? 320 : 120));
	if (features[17] >= 40)
	{
		outScores[18] = ClampInt16(outScores[18] + 450); // Train Engineers to capture Enemy ConYard/Tech!
		outScores[4] = ClampInt16(outScores[4] + 400);   // Climb newly captured Foreign Tech Tree!
	}
	// Slot 83: Mirage Tank & Camouflage Tree-Grove Ambush, Retreat When Spotted &
	// Ambush Zone Rotation
	outScores[83] = ClampInt16(outScores[83] + (features[98] * 9) + (features[78] * 6) +
	                           ((arch == 1 || activeExpert == 4 || activeExpert == 5) ? 300 : 110));
	// Slots 84..127 (44 slots) are FREE RESERVE SLOTS kept at 0 until assigned
}

namespace
{
bool IsAmphibiousOrAirTransport(const TechnoTypeClass* pType)
{
	if (!pType || pType->Passengers < 4)
		return false;

	return pType->SpeedType == SpeedType::Hover || pType->SpeedType == SpeedType::Amphibious ||
	       pType->SpeedType == SpeedType::FloatBeach || pType->SpeedType == SpeedType::Winged ||
	       pType->MovementZone == MovementZone::Amphibious ||
	       pType->MovementZone == MovementZone::AmphibiousDestroyer ||
	       pType->MovementZone == MovementZone::AmphibiousCrusher;
}

// Finds a safe coastal/flank cell on a target island (matching
// targetIslandZone) away from enemy turrets & units
CellClass* FindSafeIslandLandingCell(HouseClass* pHouse, const CellStruct& islandRefCell, int targetIslandZone,
                                     bool preferOre)
{
	CellClass* pBestLanding = nullptr;
	int bestScore = -999999;

	for (int dy = -32; dy <= 32; dy += 2)
	{
		for (int dx = -32; dx <= 32; dx += 2)
		{
			const CellStruct cand{static_cast<short>(islandRefCell.X + dx), static_cast<short>(islandRefCell.Y + dy)};
			auto const pCell = MapClass::Instance.TryGetCellAt(cand);
			if (!pCell || pCell->LandType == LandType::Water || pCell->LandType == LandType::Rock ||
			    pCell->GetBuilding())
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

			// Bonus for coastal/beach cells so amphibious hovercraft can beach and
			// unload without driving inland through defenses
			bool nearCoast = (pCell->LandType == LandType::Beach);
			if (!nearCoast)
			{
				const CellStruct neighbors[4] = {{static_cast<short>(cand.X + 2), cand.Y},
				                                 {static_cast<short>(cand.X - 2), cand.Y},
				                                 {cand.X, static_cast<short>(cand.Y + 2)},
				                                 {cand.X, static_cast<short>(cand.Y - 2)}};
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

			// Bonus for nearby Ore/Tiberium when landing an Expansion MCV on an
			// island
			if (preferOre && pCell->GetContainedTiberiumValue() > 0)
				score += 350;

			// Avoid visible enemy defensive structures (Prism Towers, Tesla Coils,
			// Obelisks, SAMs, Bunkers) and enemy armies
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
						score -= 120; // Don't try to unload directly on top of congested
						              // enemy buildings
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
} // namespace

void NeuralAI::ExecuteTacticalAndSpecOpsMicro(HouseClass* pHouse, const std::array<int16_t, OutputVectorSize>& scores)
{
	auto const pExt = HouseExt::Fetch(pHouse);

	CellStruct homeCoords = pHouse->BaseCenter != CellStruct::Empty ? pHouse->BaseCenter : pHouse->BaseSpawnCell;
	if (pHouse->ConYards.Count > 0 && pHouse->ConYards.GetItem(0))
		homeCoords = pHouse->ConYards.GetItem(0)->GetMapCoords();
	const int homeGroundZone = (homeCoords != CellStruct::Empty)
	                               ? MapClass::Instance.GetMovementZoneType(homeCoords, MovementZone::Normal, false)
	                               : -1;

	// Find any idle Amphibious/Hover Transport owned by this house that still has
	// passenger room
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

	// Multi-Player Opportunism (Expert 3 Routing Slot 43): Balance focusing
	// high-threat aggressor vs finishing off the most vulnerable enemy house
	const int focusBias = static_cast<int>(scores[43]);
	int bestEnemyIdx = -1;
	int bestEnemyPriority = -999999;

	for (auto const pOther : HouseClass::Array)
	{
		if (!pOther || pOther == pHouse || pHouse->IsAlliedWith(pOther) || pOther->IsNeutral() ||
		    pOther->IsObserver() || pOther->Defeated)
			continue;

		const int idx = std::clamp(pOther->ArrayIndex, 0, 7);
		int priority = pExt->AIBrain_EstimatedEnemyThreat[idx];

		if (pHouse->WhoLastHurtMe == pOther->ArrayIndex)
			priority += 250;

		if (focusBias > 40 && pOther->OwnedBuildings <= 4)
			priority += std::clamp(focusBias / 2, 40, 260);

		if (priority > bestEnemyPriority)
		{
			bestEnemyPriority = priority;
			bestEnemyIdx = pOther->ArrayIndex;
		}
	}

	if (bestEnemyIdx >= 0)
		pHouse->EnemyHouseIndex = bestEnemyIdx;

	// Tactical Squad Micro: Retreat damaged expensive/Elite/Parasited vehicles to
	// Service Depot, Chrono-Miner Warp escape, Beacons & Allied Base Rescue!
	BuildingClass* pRepairDepot =
	    pHouse->UnitRepairStations.Count > 0 ? pHouse->UnitRepairStations.GetItem(0) : nullptr;
	BuildingClass* pGrinder = pHouse->Grinders.Count > 0 ? pHouse->Grinders.GetItem(0) : nullptr;
	CellClass* pBeaconCell = (pExt->AIBrain_AttackTargetCell != CellStruct::Empty)
	                             ? MapClass::Instance.TryGetCellAt(pExt->AIBrain_AttackTargetCell)
	                             : nullptr;

	// If no manual Beacon is active, check if an ALLIED player's base is under
	// attack (< 85% HP) and rush to defend our Ally!
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

	// Real Tactical Sensors for Slots 56..60:
	// - Slot 56: Own Harvester under attack & Visible Enemy Harvester to raid
	// - Slot 57: Visible Enemy Garrisoned Building / Base Defense & Urban
	// Strongholds to clear/repair
	// - Slot 58: Enemy MindController holding a unit & Imminent Enemy Superweapon
	// - Slot 59: Visible Map Crate to collect
	// - Slot 60: Leading Heavy/Siege Tank for AA & Sensor Escort
	UnitClass* pAttackedOwnHarvester = nullptr;
	UnitClass* pVisibleEnemyHarvester = nullptr;
	UnitClass* pEnemySlaveMiner = nullptr;
	UnitClass* pLeadHeavyTank = nullptr;
	UnitClass* pLeadWarship = nullptr;
	UnitClass* pParasitedOwnUnit = nullptr;
	UnitClass* pBestParasiteVictim = nullptr;
	UnitClass* pEnemyChaosTarget = nullptr;
	InfantryClass* pSampleEnemyInfantry = nullptr;
	InfantryClass* pEnemySpecialistInf = nullptr;
	TechnoClass* pDetectedEnemyInfiltrator = nullptr;
	TechnoClass* pHiddenOrStealthEnemy = nullptr;
	UnitClass* pEnemySubOrShip = nullptr;
	const bool combatStalledForHunt =
	    (Unsorted::CurrentFrame > 5400 && (Unsorted::CurrentFrame - NeuralAI::LastCombatFrame) >= 3600);
	TechnoClass* pEnemyMindController = nullptr;
	TechnoClass* pEnemyEMPedTarget = nullptr;
	TechnoClass* pEnemyNonEMPedHeavy = nullptr;
	TechnoClass* pEnemyTemporalAttacker = nullptr;
	TechnoClass* pEnemyDrainer = nullptr;
	TechnoClass* pEnemyArtilleryUnit = nullptr;
	UnitClass* pWoundedEnemyVehicle = nullptr;
	BuildingClass* pSurgicalEnemyBld = nullptr;
	BuildingClass* pPrioritySiegeBld = nullptr;
	BuildingClass* pEnemyGarrisonToClear = nullptr;
	BuildingClass* pDamagedUrbanToRepair = nullptr;
	BuildingClass* pGrinderBld = nullptr;
	BuildingClass* pEmptyTankBunker = nullptr;
	BuildingClass* pUnitReloadBld = nullptr;
	BuildingClass* pCapturableEnemyConYard = nullptr;
	CellClass* pVisibleCrateCell = nullptr;
	bool enemySuperImminent = false;
	bool ownEngineerNearEnemyConYard = false;

	for (auto const pOtherH : HouseClass::Array)
	{
		if (!pOtherH || pOtherH == pHouse || pHouse->IsAlliedWith(pOtherH) || pOtherH->IsNeutral() ||
		    pOtherH->IsObserver() || pOtherH->Defeated)
			continue;
		for (auto const pSup : pOtherH->Supers)
		{
			if (!pSup || !pSup->Type || !pSup->Type->DisableableFromShell)
				continue;
			if (pSup->IsReady || (pSup->RechargeTimer.GetTimeLeft() > 0 && pSup->RechargeTimer.GetTimeLeft() <= 600))
			{
				enemySuperImminent = true;
				break;
			}
		}
		if (enemySuperImminent)
			break;
	}

	for (auto const pCheckU : UnitClass::Array)
	{
		if (!pCheckU || pCheckU->InLimbo || !pCheckU->IsAlive || !pCheckU->Owner || !pCheckU->Type)
			continue;

		if (pCheckU->MindControlledBy && pCheckU->MindControlledBy->Owner &&
		    pCheckU->MindControlledBy->Owner != pHouse && !pHouse->IsAlliedWith(pCheckU->MindControlledBy->Owner) &&
		    pCheckU->MindControlledBy->GetCell() && IsCellVisibleToAI(pHouse, pCheckU->MindControlledBy->GetCell()))
		{
			pEnemyMindController = pCheckU->MindControlledBy;
		}

		if (pCheckU->Owner == pHouse)
		{
			if (pCheckU->ParasiteEatingMe != nullptr && !pParasitedOwnUnit)
			{
				pParasitedOwnUnit = pCheckU;
			}
			if (pCheckU->TemporalTargetingMe && pCheckU->TemporalTargetingMe->Owner && !pEnemyTemporalAttacker)
			{
				pEnemyTemporalAttacker = pCheckU->TemporalTargetingMe->Owner;
			}
			if (pCheckU->Type->Naval && pCheckU->Type->Cost >= 1000 && !pLeadWarship)
			{
				pLeadWarship = pCheckU;
			}
			if ((pCheckU->Type->Harvester || pCheckU->Type->ResourceGatherer) &&
			    (pCheckU->HasBeenAttacked || pCheckU->GetHealthPercentage() < 0.90))
			{
				pAttackedOwnHarvester = pCheckU;
			}
			else if (!pLeadHeavyTank && !pCheckU->Type->Naval && !pCheckU->Type->Harvester &&
			         !pCheckU->Type->ResourceGatherer)
			{
				const auto pCWS = pCheckU->Type->GetWeapon(0);
				const auto pCW = pCWS ? pCWS->WeaponType : nullptr;
				if (pCheckU->Type->Cost >= 1000 || (pCW && (pCW->Range >= (8 * 256) || pCW->Spawner)))
				{
					pLeadHeavyTank = pCheckU;
				}
			}
		}
		else if (!pHouse->IsAlliedWith(pCheckU->Owner) && !pCheckU->Owner->IsNeutral())
		{
			if (pCheckU->GetCell() && IsCellVisibleToAI(pHouse, pCheckU->GetCell()))
			{
				if (!pVisibleEnemyHarvester && (pCheckU->Type->Harvester || pCheckU->Type->ResourceGatherer))
				{
					pVisibleEnemyHarvester = pCheckU;
				}
				if (!pEnemySlaveMiner && pCheckU->Type->Enslaves != nullptr)
				{
					pEnemySlaveMiner = pCheckU;
				}
				if (!pBestParasiteVictim && pCheckU->Type->Parasiteable && !pCheckU->Type->Organic &&
				    pCheckU->ParasiteEatingMe == nullptr && pCheckU->Type->Cost >= 700)
				{
					pBestParasiteVictim = pCheckU;
				}
				if (pCheckU->IsUnderEMP() && !pEnemyEMPedTarget)
				{
					pEnemyEMPedTarget = pCheckU;
				}
				else if (!pCheckU->IsUnderEMP() && pCheckU->Type->Cost >= 800 && !pCheckU->Type->Organic &&
				         !pEnemyNonEMPedHeavy)
				{
					pEnemyNonEMPedHeavy = pCheckU;
				}
				if (!pEnemyChaosTarget && !pCheckU->Berzerk && !pCheckU->Type->ImmuneToPsionics &&
				    pCheckU->Type->Cost >= 600)
				{
					pEnemyChaosTarget = pCheckU;
				}
				if (!pWoundedEnemyVehicle && pCheckU->GetHealthPercentage() < 0.55 && pCheckU->Type->Cost >= 500)
				{
					pWoundedEnemyVehicle = pCheckU;
				}
				const auto pEWS = pCheckU->Type->GetWeapon(0);
				const auto pEW = pEWS ? pEWS->WeaponType : nullptr;
				if (!pEnemyArtilleryUnit && pEW && (pEW->Range >= (8 * 256) || pEW->Spawner) && !pCheckU->Type->Naval)
				{
					pEnemyArtilleryUnit = pCheckU;
				}
				if (!pDetectedEnemyInfiltrator && pCheckU->Disguised && homeCoords != CellStruct::Empty &&
				    pCheckU->GetMapCoords().DistanceFrom(homeCoords) <= 22.0)
				{
					pDetectedEnemyInfiltrator = pCheckU;
				}
				if (pCheckU->Type->Naval &&
				    (!pEnemySubOrShip || pCheckU->Type->Cloakable || pCheckU->CloakState != CloakState::Uncloaked))
				{
					pEnemySubOrShip = pCheckU;
				}
				if (!pCheckU->Type->Naval && !pHiddenOrStealthEnemy &&
				    (pCheckU->CloakState != CloakState::Uncloaked || pCheckU->Disguised || pCheckU->Type->Cloakable ||
				     pCheckU->Type->CanDisguise || pCheckU->Type->PermaDisguise))
				{
					pHiddenOrStealthEnemy = pCheckU;
				}
			}
			else if (combatStalledForHunt || pCheckU->Owner->OwnedBuildings == 0)
			{
				if (pCheckU->Type->Naval &&
				    (!pEnemySubOrShip || pCheckU->Type->Cloakable || pCheckU->CloakState != CloakState::Uncloaked))
				{
					pEnemySubOrShip = pCheckU;
				}
				else if (!pCheckU->Type->Naval && !pHiddenOrStealthEnemy &&
				         (pCheckU->CloakState != CloakState::Uncloaked || pCheckU->Disguised ||
				          pCheckU->Type->Cloakable || pCheckU->Type->CanDisguise || pCheckU->Type->PermaDisguise ||
				          pCheckU->Owner->OwnedBuildings == 0))
				{
					pHiddenOrStealthEnemy = pCheckU;
				}
			}
		}
	}

	for (auto const pCheckInf : InfantryClass::Array)
	{
		if (!pCheckInf || pCheckInf->InLimbo || !pCheckInf->IsAlive || !pCheckInf->Owner || !pCheckInf->Type)
			continue;
		if (pCheckInf->Owner == pHouse || pHouse->IsAlliedWith(pCheckInf->Owner) || pCheckInf->Owner->IsNeutral())
			continue;
		if (!pCheckInf->GetCell())
			continue;
		if (!IsCellVisibleToAI(pHouse, pCheckInf->GetCell()))
		{
			if ((combatStalledForHunt || pCheckInf->Owner->OwnedBuildings == 0) && !pHiddenOrStealthEnemy &&
			    (pCheckInf->CloakState != CloakState::Uncloaked || pCheckInf->Disguised || pCheckInf->Type->Cloakable ||
			     pCheckInf->Type->CanDisguise || pCheckInf->Type->PermaDisguise ||
			     pCheckInf->Owner->OwnedBuildings == 0))
			{
				pHiddenOrStealthEnemy = pCheckInf;
			}
			continue;
		}

		if (!pSampleEnemyInfantry && !pCheckInf->Type->Doggie && !pCheckInf->Type->Slaved)
		{
			pSampleEnemyInfantry = pCheckInf;
		}
		if (!pEnemySpecialistInf &&
		    (pCheckInf->Type->Engineer || pCheckInf->Type->Infiltrate || pCheckInf->Type->C4 || pCheckInf->Type->Ivan ||
		     pCheckInf->Type->Deployer || pCheckInf->Type->VehicleThief || pCheckInf->Type->Thief))
		{
			pEnemySpecialistInf = pCheckInf;
		}
		if (!pDetectedEnemyInfiltrator && homeCoords != CellStruct::Empty &&
		    (pCheckInf->Disguised || pCheckInf->Type->Infiltrate || pCheckInf->Type->Engineer ||
		     pCheckInf->Type->Thief || pCheckInf->Type->VehicleThief || pCheckInf->Type->C4) &&
		    pCheckInf->GetMapCoords().DistanceFrom(homeCoords) <= 20.0)
		{
			pDetectedEnemyInfiltrator = pCheckInf;
		}
	}

	int bestSiegeScore = -1;
	for (auto const pEBld : BuildingClass::Array)
	{
		if (!pEBld || pEBld->InLimbo || !pEBld->IsAlive || !pEBld->Type || pEBld->Type->Immune)
			continue;

		if (pEBld->Owner == pHouse)
		{
			if (pEBld->TemporalTargetingMe && pEBld->TemporalTargetingMe->Owner && !pEnemyTemporalAttacker)
				pEnemyTemporalAttacker = pEBld->TemporalTargetingMe->Owner;
			if (pEBld->DrainingMe && !pEnemyDrainer)
				pEnemyDrainer = pEBld->DrainingMe;
			if (!pGrinderBld && pEBld->Type->Grinding)
				pGrinderBld = pEBld;
			if (!pEmptyTankBunker && pEBld->Type->Bunker && pEBld->BunkerLinkedItem == nullptr)
				pEmptyTankBunker = pEBld;
			if (!pUnitReloadBld && (pEBld->Type->UnitReload || pEBld->Type->UnitRepair || pEBld->Type->Helipad))
				pUnitReloadBld = pEBld;
		}
		if (pEBld->Type->CanBeOccupied && pEBld->Type->Repairable && pEBld->GetHealthPercentage() < 0.72)
		{
			const bool isFriendlyOrEmpty =
			    (pEBld->Occupants.Count == 0) ||
			    (pEBld->Occupants.GetItem(0) && pHouse->IsAlliedWith(pEBld->Occupants.GetItem(0)->Owner));
			if (isFriendlyOrEmpty && !pDamagedUrbanToRepair)
				pDamagedUrbanToRepair = pEBld;
		}

		if (!pEBld->Owner || pEBld->Owner == pHouse || pHouse->IsAlliedWith(pEBld->Owner) || pEBld->Owner->IsNeutral())
			continue;
		if (!pEBld->GetCell() || !IsCellVisibleToAI(pHouse, pEBld->GetCell()))
			continue;

		if (pEBld->Type->CanBeOccupied && pEBld->Occupants.Count > 0 && !pEnemyGarrisonToClear)
		{
			pEnemyGarrisonToClear = pEBld;
		}

		if (pEBld->Type->Capturable && pEBld->Type->ConstructionYard && !pCapturableEnemyConYard)
		{
			pCapturableEnemyConYard = pEBld;
		}

		if (scores[79] > 80 && (pEBld->Type->SuperWeapon != -1 || pEBld->Type->PowerBonus > 0 ||
		                        pEBld->Type->Factory != AbstractType::None || pEBld->Type->ConstructionYard))
		{
			if (!pSurgicalEnemyBld || pEBld->Type->SuperWeapon != -1 ||
			    (pEBld->Type->PowerBonus > 0 && pSurgicalEnemyBld->Type->SuperWeapon == -1))
			{
				pSurgicalEnemyBld = pEBld;
			}
		}
		if (scores[57] > 150)
		{
			int sScore = 0;
			if (pEBld->Occupants.Count > 0)
				sScore = 100 + pEBld->Occupants.Count * 15;
			else if (pEBld->Type->BuildCat == BuildCat::Combat)
				sScore = 85;
			if (sScore > bestSiegeScore)
			{
				bestSiegeScore = sScore;
				pPrioritySiegeBld = pEBld;
			}
		}
	}

	if (pCapturableEnemyConYard && scores[82] > 60)
	{
		for (auto const pEng : InfantryClass::Array)
		{
			if (pEng && pEng->Owner == pHouse && !pEng->InLimbo && pEng->IsAlive && pEng->Type &&
			    pEng->Type->Engineer && pEng->DistanceFrom(pCapturableEnemyConYard) <= 2560)
			{
				ownEngineerNearEnemyConYard = true;
				break;
			}
		}
	}

	// Slot 82 (Stolen ConYard / MCV Human Shield Escort, Anti-Recapture & Island
	// Extraction): Detect if we own a newly captured ConYard or stolen/escaping
	// MCV outside our safe home base (> 16 cells from homeCoords), and identify
	// the most dangerous enemy threat near it (prioritizing enemy Engineers
	// trying to recapture it, then enemy tanks/infantry firing at it) so nearby
	// friendly units & infantry act as Human Shields / Bodyguards!
	TechnoClass* pStolenConYardOrMCV = nullptr;
	TechnoClass* pThreatToStolenAsset = nullptr;
	UnitClass* pStrandedStolenMCVOnIsland = nullptr;
	if (scores[82] > 60 && homeCoords != CellStruct::Empty)
	{
		for (auto const pOwnB : pHouse->Buildings)
		{
			if (!pOwnB || pOwnB->InLimbo || !pOwnB->IsAlive || !pOwnB->Type || !pOwnB->Type->ConstructionYard)
				continue;
			if (pHouse->NumConYards >= 2 && pOwnB != pHouse->ConYards.GetItem(0) &&
			    pOwnB->GetMapCoords().DistanceFrom(homeCoords) > 16.0)
			{
				pStolenConYardOrMCV = pOwnB;
				break;
			}
		}
		for (auto const pOwnU : UnitClass::Array)
		{
			if (!pOwnU || pOwnU->Owner != pHouse || pOwnU->InLimbo || !pOwnU->IsAlive || !pOwnU->Type ||
			    !pOwnU->Type->DeploysInto || !pOwnU->Type->DeploysInto->ConstructionYard)
				continue;
			const CellStruct mc = pOwnU->GetMapCoords();
			const bool isForeignMCV =
			    (pHouse->CountOwnedNow(pOwnU->Type->DeploysInto) == 0 && pHouse->NumConYards >= 1);
			if (mc.DistanceFrom(homeCoords) > 14.0 && (isForeignMCV || pOwnU->HasBeenAttacked))
			{
				pStolenConYardOrMCV = pOwnU;
				const int mz = MapClass::Instance.GetMovementZoneType(mc, MovementZone::Normal, false);
				if (homeGroundZone >= 0 && mz >= 0 && mz != homeGroundZone && isForeignMCV)
					pStrandedStolenMCVOnIsland = pOwnU;
				break;
			}
		}
		if (pStolenConYardOrMCV)
		{
			int bestThreatScore = -1;
			for (auto const pET : TechnoClass::Array)
			{
				if (!pET || pET->InLimbo || !pET->IsAlive || !pET->Owner || pET->WhatAmI() == AbstractType::Building)
					continue;
				if (pET->Owner == pHouse || pHouse->IsAlliedWith(pET->Owner) || pET->Owner->IsNeutral())
					continue;
				const int dist = pStolenConYardOrMCV->DistanceFrom(pET);
				if (dist > 3584)
					continue;
				int tScore = 4000 - dist;
				if (auto const pEI = abstract_cast<InfantryClass*>(pET))
				{
					if (pEI->Type && pEI->Type->Engineer)
						tScore += 15000;
					else if (pEI->Type && (pEI->Type->C4 || pEI->Type->Ivan))
						tScore += 9000;
				}
				if (pET->Target == pStolenConYardOrMCV)
					tScore += 5000;
				if (tScore > bestThreatScore)
				{
					bestThreatScore = tScore;
					pThreatToStolenAsset = pET;
				}
			}
		}
	}

	if (scores[59] > 80 && homeCoords != CellStruct::Empty && (Unsorted::CurrentFrame % 90 < 25))
	{
		for (int dy = -36; dy <= 36 && !pVisibleCrateCell; dy += 2)
		{
			for (int dx = -36; dx <= 36; dx += 2)
			{
				const CellStruct tc{static_cast<short>(homeCoords.X + dx), static_cast<short>(homeCoords.Y + dy)};
				auto const pC = MapClass::Instance.TryGetCellAt(tc);
				if (!pC || pC->OverlayTypeIndex < 0 || pC->OverlayTypeIndex >= OverlayTypeClass::Array.Count ||
				    !IsCellVisibleToAI(pHouse, pC))
					continue;
				if (auto const pOvl = OverlayTypeClass::Array.GetItem(pC->OverlayTypeIndex))
				{
					if (pOvl->Crate)
					{
						pVisibleCrateCell = pC;
						break;
					}
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

		// Evade radiation clouds (Desolator / Nuke fallout) immediately so units
		// don't melt in place
		if (auto const pUnitCell = pUnit->GetCell())
		{
			if (pUnitCell->GetRadLevel() > 25 && pUnit->CurrentMission != Mission::Move)
				pUnit->Scatter(CoordStruct::Empty, true, false);
		}

		// Slot 58 (Anti-SW Scatter): When an enemy Superweapon is ready/imminent,
		// scatter tightly clumped combat vehicles so a single Nuke/Lightning Storm
		// doesn't wipe the whole army!
		if (enemySuperImminent && scores[58] > 140 && !pType->Harvester && !pType->ResourceGatherer &&
		    pUnit->CurrentMission != Mission::Move && (Unsorted::CurrentFrame % 90 < 15))
		{
			const CellStruct uc = pUnit->GetMapCoords();
			int clumpedNeighbors = 0;
			for (auto const pOtherU : UnitClass::Array)
			{
				if (!pOtherU || pOtherU == pUnit || pOtherU->Owner != pHouse || pOtherU->InLimbo || !pOtherU->IsAlive)
					continue;
				const CellStruct oc = pOtherU->GetMapCoords();
				if (std::abs(uc.X - oc.X) <= 1 && std::abs(uc.Y - oc.Y) <= 1)
					clumpedNeighbors++;
			}
			if (clumpedNeighbors >= 3)
			{
				pUnit->Scatter(CoordStruct::Empty, true, false);
				continue;
			}
		}

		// Slot 70 (Dying Transport / Battle Fortress Emergency Passenger Unload):
		// If a transport or OpenTopped vehicle carrying passengers on land is about
		// to explode (< 24% HP), immediately Unload so the squad inside survives!
		if (pUnit->Passengers.NumPassengers > 0 && !pType->Naval && pUnit->GetHealthPercentage() < 0.24 &&
		    scores[70] > 60)
		{
			if (auto const pUC = pUnit->GetCell())
			{
				if (pUC->LandType != LandType::Water && pUnit->CurrentMission != Mission::Unload)
				{
					pUnit->QueueMission(Mission::Unload, true);
					continue;
				}
			}
		}

		// Slot 72 (Crazy Ivan AttachedBomb Kamikaze Charge): If our vehicle has an
		// attached Ivan bomb and no Repair Depot is nearby, charge straight into
		// the nearest enemy so the bomb explodes in the enemy's ranks instead of
		// ours!
		if (pUnit->AttachedBomb != nullptr && !pRepairDepot && scores[72] > 60)
		{
			TechnoClass* pKamikazeTarget = pSurgicalEnemyBld ? static_cast<TechnoClass*>(pSurgicalEnemyBld)
			                                                 : static_cast<TechnoClass*>(pWoundedEnemyVehicle);
			if (pKamikazeTarget && pUnit->Target != pKamikazeTarget)
			{
				pUnit->SetTarget(pKamikazeTarget);
				pUnit->QueueMission(Mission::Attack, true);
				continue;
			}
		}

		// If infected by a Terror Drone / Giant Squid (ParasiteEatingMe), rush
		// immediately to Service Depot to rip it out!
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
			// IMPORTANT: Never call SetDestination() on Harvesters or Chrono Miners
			// (Teleporter=yes)! In Yuri's Revenge, calling SetDestination() on a
			// ChronoLocomotionClass unit triggers an immediate Chrono-shift and
			// resets UnitClass::Mission_Harvest's internal state machine, causing
			// Allied Chrono Miners to loop-teleport endlessly to the Refinery
			// entrance cell. Vanilla Mission_Harvest natively handles ore/gem search
			// and Chrono return.
			if (pUnit->CurrentMission == Mission::Sleep || pUnit->CurrentMission == Mission::None)
				pUnit->QueueMission(Mission::Harvest, false);
			continue;
		}

		// Yuri Grinder Economy: Recycle heavily damaged Mind-Controlled enemy
		// vehicles into the Grinder for instant cash!
		if (pGrinder && pUnit->MindControlledBy != nullptr && pUnit->GetHealthPercentage() < 0.45 &&
		    pUnit->CurrentMission != Mission::Enter)
		{
			pUnit->SetDestination(pGrinder, true);
			pUnit->QueueMission(Mission::Enter, true);
			continue;
		}

		// Auto-deploy MCV if we have no ConYards (ensuring a safe, corridor-clear
		// 4x4 foundation when rebuilt from a War Factory or retreating after
		// emergency undeploy), or march any additional Expansion MCV to an Ore
		// field
		if (pType->DeploysInto && pType->DeploysInto->ConstructionYard)
		{
			const CellStruct mcvCell = pUnit->GetMapCoords();
			const bool isForeignTechMCV = (pHouse->CountOwnedNow(pType->DeploysInto) == 0 && pHouse->NumConYards >= 1);
			bool enemyNearMCV = false;
			for (auto const pEB : BuildingClass::Array)
			{
				if (!pEB || pEB->InLimbo || !pEB->IsAlive || !pEB->Owner || !pEB->Type || pEB->Type->Immune)
					continue;
				if (pEB->Owner == pHouse || pHouse->IsAlliedWith(pEB->Owner) || pEB->Owner->IsNeutral())
					continue;
				const CellStruct ec = pEB->GetMapCoords();
				if (std::max(std::abs(mcvCell.X - ec.X), std::abs(mcvCell.Y - ec.Y)) <= 14)
				{
					enemyNearMCV = true;
					break;
				}
			}
			if (!enemyNearMCV)
			{
				for (auto const pEU : UnitClass::Array)
				{
					if (!pEU || pEU->InLimbo || !pEU->IsAlive || !pEU->Owner || !pEU->Type || pEU->Type->Harvester ||
					    pEU->Type->ResourceGatherer)
						continue;
					if (pEU->Owner == pHouse || pHouse->IsAlliedWith(pEU->Owner) || pEU->Owner->IsNeutral())
						continue;
					const CellStruct euc = pEU->GetMapCoords();
					if (std::max(std::abs(mcvCell.X - euc.X), std::abs(mcvCell.Y - euc.Y)) <= 12)
					{
						enemyNearMCV = true;
						break;
					}
				}
			}

			if (pHouse->NumConYards == 0)
			{
				auto const pYardType = pType->DeploysInto;
				// At initial match spawn (no buildings yet and no enemy nearby), unpack
				// immediately on the spot!
				if (pHouse->Buildings.Count == 0 && !enemyNearMCV)
				{
					if (pUnit->CurrentMission != Mission::Unload)
						pUnit->QueueMission(Mission::Unload, true);
					continue;
				}

				// Check if current MCV cell is safe (!enemyNearMCV), has a legal
				// ConYard foundation, and does not block our War Factory / Refinery
				// exit doors (critical when a newly rebuilt MCV just rolled out of the
				// War Factory!).
				bool canDeployHereSafely =
				    !enemyNearMCV && NeuralAI::IsCellClearedOfExitCorridors(pHouse, pYardType, mcvCell);
				if (canDeployHereSafely)
				{
					const int yw = pYardType->GetFoundationWidth();
					const int yh = pYardType->GetFoundationHeight(false);
					for (int fy = 0; fy < yh && canDeployHereSafely; ++fy)
					{
						for (int fx = 0; fx < yw; ++fx)
						{
							const CellStruct fc{static_cast<short>(mcvCell.X + fx), static_cast<short>(mcvCell.Y + fy)};
							auto const pFC = MapClass::Instance.TryGetCellAt(fc);
							if (!pFC || !pFC->CanThisExistHere(pYardType->SpeedType, pYardType, pHouse))
							{
								canDeployHereSafely = false;
								break;
							}
						}
					}
				}

				if (canDeployHereSafely)
				{
					if (pUnit->CurrentMission != Mission::Unload)
						pUnit->QueueMission(Mission::Unload, true);
				}
				else if (pUnit->CurrentMission != Mission::Move || (Unsorted::CurrentFrame % 45 < 15))
				{
					const CellStruct safeYardCell = NeuralAI::FindSpatialTetrisBuildCell(pHouse, pYardType);
					if (safeYardCell != CellStruct::Empty)
					{
						if (auto const pSafeDest = MapClass::Instance.TryGetCellAt(safeYardCell))
						{
							pUnit->SetDestination(pSafeDest, true);
							pUnit->QueueMission(Mission::Move, true);
						}
					}
					else if (!enemyNearMCV && pUnit->CurrentMission != Mission::Unload)
					{
						pUnit->QueueMission(Mission::Unload, true);
					}
				}
				continue;
			}

			// Slot 82 (Stolen / Foreign MCV Safe Extraction, Island Transport
			// Evacuation & Home Base Deployment): 1) If the stolen MCV is on a
			// disconnected island (mcvZone != homeGroundZone), board a nearby
			// Amphibious Transport (or move to the safest pickup beach while a
			// transport sails over to rescue it) to ferry it back to our safe Home
			// Island! 2) If on the same landmass and inside a hostile enemy zone
			// (enemyNearMCV) OR outside our home base (> 16 cells from homeCoords),
			// evacuate it by land back to our safe home base! 3) Once the stolen
			// foreign-faction MCV reaches our safe home base (<= 16 cells from
			// homeCoords and !enemyNearMCV), deploy it safely inside our base
			// perimeter so we can build its foreign tech tree!
			if (scores[82] > 60 && homeCoords != CellStruct::Empty && (enemyNearMCV || isForeignTechMCV))
			{
				const int mcvZone = MapClass::Instance.GetMovementZoneType(mcvCell, MovementZone::Normal, false);
				const bool onDisconnectedEnemyIsland =
				    (homeGroundZone >= 0 && mcvZone >= 0 && mcvZone != homeGroundZone);

				if (onDisconnectedEnemyIsland && isForeignTechMCV)
				{
					UnitClass* pRescueTrans = nullptr;
					int bestRescueDist = 999999;
					for (auto const pT : UnitClass::Array)
					{
						if (!pT || pT->Owner != pHouse || pT->InLimbo || !pT->IsAlive || !pT->Type ||
						    !IsAmphibiousOrAirTransport(pT->Type))
							continue;
						if (pType->TechnoTypeClass::Size > pT->Type->SizeLimit ||
						    (pT->Passengers.GetTotalSize() + static_cast<int>(pType->TechnoTypeClass::Size)) >
						        pT->Type->Passengers)
							continue;
						const int d = pUnit->DistanceFrom(pT);
						if (d < bestRescueDist)
						{
							bestRescueDist = d;
							pRescueTrans = pT;
						}
					}

					if (pRescueTrans && bestRescueDist <= 3584)
					{
						if (pUnit->CurrentMission != Mission::Enter)
						{
							pUnit->SetDestination(pRescueTrans, true);
							pUnit->QueueMission(Mission::Enter, true);
						}
						continue;
					}

					if (pUnit->CurrentMission != Mission::Move || (Unsorted::CurrentFrame % 60 < 15))
					{
						if (auto const pPickupBeach = FindSafeIslandLandingCell(pHouse, mcvCell, mcvZone, false))
						{
							pUnit->SetDestination(pPickupBeach, true);
							pUnit->QueueMission(Mission::Move, true);
						}
					}
					continue;
				}

				const double distToHome = mcvCell.DistanceFrom(homeCoords);
				if (enemyNearMCV || (isForeignTechMCV && distToHome > 16.0))
				{
					if (pUnit->CurrentMission != Mission::Move || (Unsorted::CurrentFrame % 60 < 15))
					{
						const CellStruct safeHomeCell =
						    NeuralAI::FindSpatialTetrisBuildCell(pHouse, pType->DeploysInto);
						const CellStruct goalCoords = (safeHomeCell != CellStruct::Empty) ? safeHomeCell : homeCoords;
						if (auto const pHomeDest = MapClass::Instance.TryGetCellAt(goalCoords))
						{
							pUnit->SetDestination(pHomeDest, true);
							pUnit->QueueMission(Mission::Move, true);
						}
					}
					continue;
				}
				if (isForeignTechMCV && !enemyNearMCV && distToHome <= 16.0)
				{
					if (NeuralAI::IsCellClearedOfExitCorridors(pHouse, pType->DeploysInto, mcvCell))
					{
						if (pUnit->CurrentMission != Mission::Unload)
							pUnit->QueueMission(Mission::Unload, true);
					}
					else if (pUnit->CurrentMission == Mission::Guard || pUnit->CurrentMission == Mission::Area_Guard)
					{
						const CellStruct clearCell = NeuralAI::FindSpatialTetrisBuildCell(pHouse, pType->DeploysInto);
						if (auto const pClear = MapClass::Instance.TryGetCellAt(clearCell))
						{
							pUnit->SetDestination(pClear, true);
							pUnit->QueueMission(Mission::Move, true);
						}
					}
					continue;
				}
			}

			int minAnchorDist = 9999;
			CellStruct newestYard = pHouse->BaseCenter != CellStruct::Empty ? pHouse->BaseCenter : mcvCell;
			for (auto const pOwnBld : pHouse->Buildings)
			{
				if (!pOwnBld || pOwnBld->InLimbo || !pOwnBld->IsAlive || !pOwnBld->Type)
					continue;
				if (!pOwnBld->Type->ConstructionYard && !pOwnBld->Type->Refinery && !pOwnBld->Type->BaseNormal &&
				    pOwnBld->Type->Factory == AbstractType::None)
					continue;

				const CellStruct bc = pOwnBld->GetMapCoords();
				if (pOwnBld->Type->ConstructionYard)
					newestYard = bc;
				const int d = std::max(std::abs(mcvCell.X - bc.X), std::abs(mcvCell.Y - bc.Y));
				if (d < minAnchorDist)
					minAnchorDist = d;
			}

			if (minAnchorDist >= 13 && !enemyNearMCV)
			{
				if (pUnit->CurrentMission != Mission::Unload)
					pUnit->QueueMission(Mission::Unload, true);
			}
			else if (pUnit->CurrentMission == Mission::Guard || pUnit->CurrentMission == Mission::Area_Guard)
			{
				CellClass* pExpansionDest = nullptr;

				// Slot 81 (Nomadic MCV Forward Siege Harassment): When scores[81] > 140
				// and we know an enemy base structure (pSurgicalEnemyBld or
				// pPrioritySiegeBld), march the MCV to a forward staging point ~15
				// cells outside the enemy base to drop Forward Turrets / Outposts /
				// Barracks right on their doorstep!
				BuildingClass* pHarassEnemyBld = pSurgicalEnemyBld ? pSurgicalEnemyBld : pPrioritySiegeBld;
				if (scores[81] > 140 && pHarassEnemyBld && pHouse->CountResourceDestinations >= 2)
				{
					const CellStruct ec = pHarassEnemyBld->GetMapCoords();
					const int dxE = newestYard.X - ec.X;
					const int dyE = newestYard.Y - ec.Y;
					const int distE = std::max(1, std::max(std::abs(dxE), std::abs(dyE)));
					if (distE >= 22)
					{
						const CellStruct siegeStage{static_cast<short>(ec.X + (dxE * 15) / distE),
						                            static_cast<short>(ec.Y + (dyE * 15) / distE)};
						if (auto const pSC = MapClass::Instance.TryGetCellAt(siegeStage))
						{
							if (pSC->IsClearToMove(pType->SpeedType, true, true, -1, pType->MovementZone, -1, false))
							{
								pExpansionDest = pSC;
							}
						}
					}
				}

				// Otherwise find the closest rich Ore/Tiberium patch that is >= 14
				// cells away from ALL existing ConYards AND Refineries/Outposts
				int bestExpDist = 99999;
				for (int dy = -56; dy <= 56 && !pExpansionDest; dy += 3)
				{
					for (int dx = -56; dx <= 56; dx += 3)
					{
						const CellStruct tc{static_cast<short>(newestYard.X + dx),
						                    static_cast<short>(newestYard.Y + dy)};
						auto const pCell = MapClass::Instance.TryGetCellAt(tc);
						if (!pCell || pCell->GetContainedTiberiumValue() <= 0)
							continue;

						bool tooCloseToExistingHub = false;
						for (auto const pOwnBld : pHouse->Buildings)
						{
							if (!pOwnBld || pOwnBld->InLimbo || !pOwnBld->IsAlive || !pOwnBld->Type)
								continue;
							if (!pOwnBld->Type->ConstructionYard && !pOwnBld->Type->Refinery &&
							    !pOwnBld->Type->BaseNormal)
								continue;

							const CellStruct bc = pOwnBld->GetMapCoords();
							if (std::max(std::abs(tc.X - bc.X), std::abs(tc.Y - bc.Y)) < 14)
							{
								tooCloseToExistingHub = true;
								break;
							}
						}

						if (tooCloseToExistingHub)
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
					const int expZone =
					    MapClass::Instance.GetMovementZoneType(pExpansionDest->MapCoords, MovementZone::Normal, false);
					const bool onDifferentIsland = (mcvZone >= 0 && expZone >= 0 && mcvZone != expZone);

					const bool wantIslandBeachheadMCV =
					    onDifferentIsland || (homeGroundZone >= 0 && mcvZone == homeGroundZone && scores[75] > 120);
					if (wantIslandBeachheadMCV && pLoadingTransport &&
					    pType->TechnoTypeClass::Size <= pLoadingTransport->Type->SizeLimit &&
					    (pLoadingTransport->Passengers.GetTotalSize() +
					     static_cast<int>(pType->TechnoTypeClass::Size)) <= pLoadingTransport->Type->Passengers)
					{
						// Expansion Ore field is on an island! Board the Amphibious Hover
						// Transport to colonize the island
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

		// Tactical Combat Deployment (Tick Tank, Nod Artillery, Juggernaut, Siege
		// Chopper) & Universal Deployable Key Structures across all mods (Mobile
		// Outposts/Explorers, Mobile Refineries/Factories, Power/Tech/Turret Cores,
		// CABAL Nanocores)!
		if ((pType->DeploysInto && !pType->DeploysInto->ConstructionYard) || pType->DeployFire)
		{
			const bool hasPrimaryWeapon =
			    (pType->GetWeapon(0) != nullptr && pType->GetWeapon(0)->WeaponType != nullptr);
			auto const pTargetBld = pType->DeploysInto;
			const bool isKeyStructureDeployer =
			    (pTargetBld != nullptr && !pType->Harvester && !pType->ResourceGatherer &&
			     (pTargetBld->BuildCat != BuildCat::Combat || !hasPrimaryWeapon));

			if (isKeyStructureDeployer)
			{
				const CellStruct nanoCell = pUnit->GetMapCoords();
				const bool isNanoIdle =
				    (pUnit->CurrentMission == Mission::Guard || pUnit->CurrentMission == Mission::Area_Guard);

				// Check if this unit deploys into an Outpost Expansion Hub (BaseNormal
				// Outpost, Expansion Refinery, or Forward Factory)
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

				const bool isDedicatedMobileOutpost =
				    (pTargetBld->BaseNormal && !pTargetBld->Refinery && pTargetBld->Factory == AbstractType::None &&
				     pTargetBld->PowerBonus <= pTargetBld->PowerDrain && pHouse->Buildings.Count >= 1);
				const bool isExpansionDeployer =
				    isDedicatedMobileOutpost || (pTargetBld->Refinery && ownedRefineries >= 1 && scores[30] > 650) ||
				    (pTargetBld->Factory == AbstractType::UnitType && !pTargetBld->Naval && ownedWarFactories >= 1 &&
				     scores[30] > 750) ||
				    (pTargetBld->Factory == AbstractType::InfantryType && ownedBarracks >= 1 && scores[30] > 800);

				if (isExpansionDeployer)
				{
					if (minDistFromRefOrYard >= 12)
					{
						if (pUnit->CurrentMission != Mission::Unload)
							pUnit->QueueMission(Mission::Unload, true);
					}
					else if (isNanoIdle && homeCoords != CellStruct::Empty)
					{
						// Search for closest uncolonized Ore/Gem field >= 14 cells from all
						// existing Refineries, Outposts & ConYards
						CellClass* pExpOreCell = nullptr;
						int bestOreDist = 99999;
						for (int dy = -56; dy <= 56; dy += 3)
						{
							for (int dx = -56; dx <= 56; dx += 3)
							{
								const CellStruct tc{static_cast<short>(homeCoords.X + dx),
								                    static_cast<short>(homeCoords.Y + dy)};
								auto const pC = MapClass::Instance.TryGetCellAt(tc);
								if (!pC || pC->GetContainedTiberiumValue() <= 0)
									continue;

								bool nearExistingRef = false;
								for (auto const pOwnBld : pHouse->Buildings)
								{
									if (!pOwnBld || pOwnBld->InLimbo || !pOwnBld->IsAlive || !pOwnBld->Type)
										continue;
									if (pOwnBld->Type->Refinery || pOwnBld->Type->ConstructionYard ||
									    pOwnBld->Type->BaseNormal)
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
							const int nanoZone =
							    MapClass::Instance.GetMovementZoneType(nanoCell, MovementZone::Normal, false);
							const int expZone = MapClass::Instance.GetMovementZoneType(pExpOreCell->MapCoords,
							                                                           MovementZone::Normal, false);
							const bool onDifferentIsland = (nanoZone >= 0 && expZone >= 0 && nanoZone != expZone);

							if (onDifferentIsland && pLoadingTransport &&
							    pType->TechnoTypeClass::Size <= pLoadingTransport->Type->SizeLimit &&
							    (pLoadingTransport->Passengers.GetTotalSize() +
							     static_cast<int>(pType->TechnoTypeClass::Size)) <= pLoadingTransport->Type->Passengers)
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

				// Defense Turret Deployer (Mobile Turret Cores / ROBOTEGG_OBELISK2,
				// ROBOTEGG_CERBERUS2, etc.): Move 6..10 cells away from
				// factories/ConYard to the defensive perimeter before unpacking!
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

				// Base Infrastructure Deployer (Power, Barracks, Radar, Tech, Repair
				// Bay, Shield/Gap Generator, or first Refinery/Factory): Unpack as soon
				// as clear of factory exit corridors; if blocking a factory door, step
				// aside first!
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

		// Preserve Elite vehicles (< 60% HP) and expensive vehicles by retreating
		// to Service Depot (Slot 54 adjusts retreat HP & survival reflex)
		const double baseRetreat = std::clamp(0.35 + static_cast<double>(scores[54]) * 0.00025, 0.22, 0.55);
		const double repairThreshold = pUnit->Veterancy.IsElite() ? std::max(0.60, baseRetreat + 0.15) : baseRetreat;
		// Slot 64 (Immediate Service Depot De-Parasiting) & Slot 54 (Repair
		// Retreat): If a Terror Drone or Squid is eating our vehicle
		// (ParasiteEatingMe != nullptr), rush to Service Depot immediately even at
		// 99% HP so the repair arm kills the parasite!
		if (pRepairDepot &&
		    ((pUnit->ParasiteEatingMe != nullptr && scores[64] > 60) ||
		     ((pType->Cost >= 800 || pUnit->Veterancy.IsElite()) && pUnit->GetHealthPercentage() < repairThreshold)))
		{
			if (pUnit->CurrentMission != Mission::Enter)
			{
				pUnit->SetDestination(pRepairDepot, true);
				pUnit->QueueMission(Mission::Enter, true);
			}

			continue;
		}

		// Slot 65 (Grinder Mind-Control Recycling & Salvage): Send mind-controlled
		// vehicles we captured (MindControlledBy != nullptr) into our Grinder for
		// instant cash AND to immediately free our Yuri/Mastermind's control slot!
		if (pGrinderBld && !pType->Naval && scores[65] > 80 &&
		    (pUnit->MindControlledBy != nullptr ||
		     (!pRepairDepot && pUnit->GetHealthPercentage() < 0.18 && pHouse->Available_Money() < 600)))
		{
			if (pUnit->CurrentMission != Mission::Enter)
			{
				pUnit->SetDestination(pGrinderBld, true);
				pUnit->QueueMission(Mission::Enter, true);
			}
			continue;
		}

		// Slot 67 (ManualReload Ammo Replenishment): Return to reload dock when
		// Ammo == 0
		if (pUnitReloadBld && pType->ManualReload && pType->Ammo > 0 && pUnit->Ammo == 0 && scores[67] > 60)
		{
			if (pUnit->CurrentMission != Mission::Enter)
			{
				pUnit->SetDestination(pUnitReloadBld, true);
				pUnit->QueueMission(Mission::Enter, true);
			}
			continue;
		}

		// Slot 68 (TiberiumHeal Regeneration): Move wounded
		// CyborgReaper/Fiend/Visceroid units onto nearby Tiberium/Ore to heal for
		// free!
		if (pType->TiberiumHeal && pUnit->GetHealthPercentage() < 0.55 && scores[68] > 60 &&
		    pUnit->CurrentMission != Mission::Move)
		{
			if (pUnit->MoveToTiberium(10, false))
				continue;
		}

		const bool isUnitIdle =
		    (pUnit->CurrentMission == Mission::Guard || pUnit->CurrentMission == Mission::Area_Guard);
		const auto pPrimaryWpn = pType->GetWeapon(0) ? pType->GetWeapon(0)->WeaponType : nullptr;

		// Slot 82 (Engineer ConYard Heist Cease-Fire): If our tank is firing at an
		// enemy ConYard that is already in red health (< 35% HP) while our own
		// Engineer is within 10 cells moving in to capture it, hold fire on the
		// ConYard so our Engineer can steal it alive!
		if (ownEngineerNearEnemyConYard && pCapturableEnemyConYard && pUnit->Target == pCapturableEnemyConYard &&
		    pCapturableEnemyConYard->GetHealthPercentage() < 0.35 && scores[82] > 60)
		{
			pUnit->SetTarget(nullptr);
			pUnit->QueueMission(Mission::Guard, true);
			continue;
		}

		// Slot 71 (Chrono Legionnaire Temporal Breaker): Immediately kill any enemy
		// Chrono Legionnaire erasing our building or tank so it is restored at
		// 100%!
		if (pEnemyTemporalAttacker && pPrimaryWpn && pPrimaryWpn->Damage > 0 && scores[71] > 60 &&
		    pUnit->DistanceFrom(pEnemyTemporalAttacker) <= 4096)
		{
			pUnit->SetTarget(pEnemyTemporalAttacker);
			pUnit->QueueMission(Mission::Attack, true);
			continue;
		}

		// Slot 74 (Anti-Drain Floating Disc Interceptor): Mobile AA units
		// immediately shoot down any enemy Floating Disc draining our buildings!
		if (pEnemyDrainer && pPrimaryWpn && pPrimaryWpn->Projectile && pPrimaryWpn->Projectile->AA && scores[74] > 60 &&
		    pUnit->DistanceFrom(pEnemyDrainer) <= 4608)
		{
			pUnit->SetTarget(pEnemyDrainer);
			pUnit->QueueMission(Mission::Attack, true);
			continue;
		}

		// Slot 77 (Wounded Frontline Tank Pull-Back Micro): When our tank is
		// focused below 38% HP in active combat and healthy allied tanks are right
		// beside it, pull back 4 cells behind our healthy frontline so the enemy
		// switches target!
		if (pUnit->Target != nullptr && !pType->Naval && pUnit->GetHealthPercentage() < 0.38 && scores[77] > 80 &&
		    homeCoords != CellStruct::Empty && (Unsorted::CurrentFrame % 45 < 15))
		{
			int healthyAlliesBeside = 0;
			for (auto const pAllyT : UnitClass::Array)
			{
				if (pAllyT && pAllyT != pUnit && pAllyT->Owner == pHouse && !pAllyT->InLimbo && pAllyT->IsAlive &&
				    pAllyT->GetHealthPercentage() > 0.70 && pUnit->DistanceFrom(pAllyT) <= 1280)
				{
					healthyAlliesBeside++;
				}
			}
			if (healthyAlliesBeside >= 1)
			{
				const CellStruct uc = pUnit->GetMapCoords();
				const int sx = (homeCoords.X > uc.X) ? 3 : ((homeCoords.X < uc.X) ? -3 : 0);
				const int sy = (homeCoords.Y > uc.Y) ? 3 : ((homeCoords.Y < uc.Y) ? -3 : 0);
				const CellStruct backCell{static_cast<short>(uc.X + sx), static_cast<short>(uc.Y + sy)};
				if (auto const pBC = MapClass::Instance.TryGetCellAt(backCell))
				{
					if (pBC->IsClearToMove(pType->SpeedType, true, true, -1, pType->MovementZone, -1, false))
					{
						pUnit->SetDestination(pBC, true);
						pUnit->QueueMission(Mission::Move, true);
						continue;
					}
				}
			}
		}

		// Slot 76 (Squad Focus-Fire on Wounded In-Range Enemy): Concentrate fire on
		// a wounded enemy vehicle already inside our exact weapon range!
		if (pWoundedEnemyVehicle && pUnit->CurrentMission == Mission::Attack && pUnit->Target != pWoundedEnemyVehicle &&
		    pPrimaryWpn && pPrimaryWpn->Damage > 0 && scores[76] > 80 &&
		    pUnit->DistanceFrom(pWoundedEnemyVehicle) <= pPrimaryWpn->Range)
		{
			pUnit->SetTarget(pWoundedEnemyVehicle);
			continue;
		}

		// Slot 79 (Surgical Enemy Power, Superweapon & Factory Snipe): When inside
		// range of a critical enemy structure, prioritize it over walls/silos!
		if (pSurgicalEnemyBld && isUnitIdle && pPrimaryWpn && pPrimaryWpn->Damage > 0 && scores[79] > 80 &&
		    pUnit->DistanceFrom(pSurgicalEnemyBld) <= 2816)
		{
			pUnit->SetTarget(pSurgicalEnemyBld);
			pUnit->QueueMission(Mission::Attack, true);
			continue;
		}

		// Slot 80 (Fast Flanker & Jumpjet Counter-Battery vs Enemy Artillery):
		if (pEnemyArtilleryUnit && isUnitIdle && pPrimaryWpn && pPrimaryWpn->Damage > 0 &&
		    (pType->Speed >= 8 || pType->JumpJet) && scores[80] > 80 &&
		    pUnit->DistanceFrom(pEnemyArtilleryUnit) <= 4608)
		{
			pUnit->SetTarget(pEnemyArtilleryUnit);
			pUnit->QueueMission(Mission::Attack, true);
			continue;
		}

		// Slot 78 (High-Ground Cliff Advantage Positioning): Siege/long-range units
		// step onto nearby higher-elevation cells (CellClass::Level) when idle!
		if (isUnitIdle && pPrimaryWpn && pPrimaryWpn->Range >= (7 * 256) && !pType->Naval && scores[78] > 90 &&
		    (Unsorted::CurrentFrame % 120 < 20))
		{
			if (auto const pCurC = pUnit->GetCell())
			{
				const CellStruct uc = pUnit->GetMapCoords();
				for (int dy = -4; dy <= 4; dy += 2)
				{
					bool movedHigh = false;
					for (int dx = -4; dx <= 4; dx += 2)
					{
						const CellStruct hc{static_cast<short>(uc.X + dx), static_cast<short>(uc.Y + dy)};
						if (auto const pHC = MapClass::Instance.TryGetCellAt(hc))
						{
							if (pHC->Level > pCurC->Level &&
							    pHC->IsClearToMove(pType->SpeedType, true, true, -1, pType->MovementZone, -1, false))
							{
								pUnit->SetDestination(pHC, true);
								pUnit->QueueMission(Mission::Move, true);
								movedHigh = true;
								break;
							}
						}
					}
					if (movedHigh)
						break;
				}
			}
		}

		// Slot 58 (Mind-Control Breaker): Focus-fire enemy Yuri/Mastermind/Psychic
		// Tower with ImmuneToPsionics vehicles or long-range artillery to
		// immediately free our mind-controlled tank!
		if (pEnemyMindController && isUnitIdle &&
		    (pType->ImmuneToPsionics || (pPrimaryWpn && pPrimaryWpn->Range >= (6 * 256))) &&
		    pUnit->DistanceFrom(pEnemyMindController) <= 4608)
		{
			pUnit->SetTarget(pEnemyMindController);
			pUnit->QueueMission(Mission::Attack, true);
			continue;
		}

		// Slot 58 (Tank Infantry Crushing): Heavy Crusher=yes tanks run over nearby
		// Crushable=yes enemy infantry (<= 3 cells) instead of wasting slow cannon
		// shells!
		if (pType->Crusher && !pType->Naval && scores[58] > 100 &&
		    (isUnitIdle || pUnit->CurrentMission == Mission::Attack) && (Unsorted::CurrentFrame % 45 < 15))
		{
			CellClass* pCrushCell = nullptr;
			for (auto const pEInf : InfantryClass::Array)
			{
				if (!pEInf || pEInf->InLimbo || !pEInf->IsAlive || !pEInf->Owner || !pEInf->Type ||
				    !pEInf->Type->Crushable || pEInf->Type->C4 || pEInf->Type->Ivan)
					continue;
				if (pEInf->Owner == pHouse || pHouse->IsAlliedWith(pEInf->Owner) || pEInf->Owner->IsNeutral())
					continue;
				if (pEInf->GetCell() && pEInf->GetCell()->GetBuilding() == nullptr &&
				    pUnit->DistanceFrom(pEInf) <= 768 && IsCellVisibleToAI(pHouse, pEInf->GetCell()))
				{
					pCrushCell = pEInf->GetCell();
					break;
				}
			}
			if (pCrushCell)
			{
				pUnit->SetDestination(pCrushCell, true);
				pUnit->QueueMission(Mission::Move, true);
				continue;
			}
		}

		// Slot 58 (Artillery Kiting / Hit-and-Run): Keep long-range siege units
		// (Prism Tank, V3, Magnetron, Juggernaut) at standoff distance when enemy
		// short-range tanks close within 3.5 cells!
		if (pPrimaryWpn && pPrimaryWpn->Range >= (8 * 256) && scores[58] > 140 && homeCoords != CellStruct::Empty &&
		    (Unsorted::CurrentFrame % 45 < 15))
		{
			bool enemyTooClose = false;
			for (auto const pCloseEnemy : UnitClass::Array)
			{
				if (!pCloseEnemy || pCloseEnemy->InLimbo || !pCloseEnemy->IsAlive || !pCloseEnemy->Owner)
					continue;
				if (pCloseEnemy->Owner == pHouse || pHouse->IsAlliedWith(pCloseEnemy->Owner) ||
				    pCloseEnemy->Owner->IsNeutral())
					continue;
				if (pUnit->DistanceFrom(pCloseEnemy) <= 896)
				{
					enemyTooClose = true;
					break;
				}
			}
			if (enemyTooClose)
			{
				const CellStruct uc = pUnit->GetMapCoords();
				const int stepX = (homeCoords.X > uc.X) ? 3 : ((homeCoords.X < uc.X) ? -3 : 0);
				const int stepY = (homeCoords.Y > uc.Y) ? 3 : ((homeCoords.Y < uc.Y) ? -3 : 0);
				const CellStruct kiteCoords{static_cast<short>(uc.X + stepX), static_cast<short>(uc.Y + stepY)};
				if (auto const pKiteCell = MapClass::Instance.TryGetCellAt(kiteCoords))
				{
					if (pKiteCell->IsClearToMove(pType->SpeedType, true, true, -1, pType->MovementZone, -1, false))
					{
						pUnit->SetDestination(pKiteCell, true);
						pUnit->QueueMission(Mission::Move, true);
						continue;
					}
				}
			}
		}

		// Slot 59 (Crate Pickup): Send nearest fast idle vehicle to collect any
		// visible map Crate!
		if (pVisibleCrateCell && isUnitIdle && pUnit->DistanceFrom(pVisibleCrateCell) <= 3584)
		{
			pUnit->SetDestination(pVisibleCrateCell, true);
			pUnit->QueueMission(Mission::Move, true);
			pVisibleCrateCell = nullptr;
			continue;
		}

		// Slot 56 (Own Harvester Defense & Enemy Harvester Eco-Raid):
		if (pAttackedOwnHarvester && isUnitIdle && pUnit->DistanceFrom(pAttackedOwnHarvester) <= 4608)
		{
			if (auto const pHarvCell = pAttackedOwnHarvester->GetCell())
			{
				pUnit->SetDestination(pHarvCell, true);
				pUnit->QueueMission(Mission::AttackMove, true);
				continue;
			}
		}
		else if (pVisibleEnemyHarvester && isUnitIdle && scores[56] > 220 && pType->Speed >= 6 &&
		         pUnit->DistanceFrom(pVisibleEnemyHarvester) <= 6144)
		{
			pUnit->SetTarget(pVisibleEnemyHarvester);
			pUnit->QueueMission(Mission::Attack, true);
			continue;
		}

		// Slot 57 (Surgical Bunker/Garrison Penetrator & Siege Focus):
		// 1) Units with PenetratesBunker=yes kill enemy occupants inside civilian
		// structures without demolishing the building so our infantry can take it!
		// 2) Long-range siege vehicles prioritize enemy garrisons & base defenses!
		if (pEnemyGarrisonToClear && isUnitIdle && pPrimaryWpn && pPrimaryWpn->Warhead &&
		    pPrimaryWpn->Warhead->PenetratesBunker && pUnit->DistanceFrom(pEnemyGarrisonToClear) <= 4608)
		{
			pUnit->SetTarget(pEnemyGarrisonToClear);
			pUnit->QueueMission(Mission::Attack, true);
			continue;
		}
		if (pPrioritySiegeBld && isUnitIdle && pPrimaryWpn && pPrimaryWpn->Range >= (7 * 256) &&
		    pUnit->DistanceFrom(pPrioritySiegeBld) <= 5632)
		{
			pUnit->SetTarget(pPrioritySiegeBld);
			pUnit->QueueMission(Mission::Attack, true);
			continue;
		}

		// Slot 82 (Human Shield & Anti-Recapture Bodyguard Screen for Stolen
		// ConYard / Escaping MCV): If we just captured an enemy ConYard or are
		// extracting its stolen MCV, any nearby friendly combat vehicle (<= 16
		// cells) immediately intercepts enemy Engineers/tanks threatening it, or
		// moves directly around the stolen ConYard/MCV as a protective human shield
		// to draw enemy fire away while it escapes!
		if (pStolenConYardOrMCV && pUnit != pStolenConYardOrMCV && !pType->Naval && scores[82] > 60 &&
		    pUnit->DistanceFrom(pStolenConYardOrMCV) <= 4096)
		{
			if (pThreatToStolenAsset && pPrimaryWpn && pPrimaryWpn->Damage > 0 && pUnit->Target != pThreatToStolenAsset)
			{
				pUnit->SetTarget(pThreatToStolenAsset);
				pUnit->QueueMission(Mission::Attack, true);
				continue;
			}
			else if (isUnitIdle && pUnit->DistanceFrom(pStolenConYardOrMCV) > 640)
			{
				if (auto const pShieldCell = pStolenConYardOrMCV->GetCell())
				{
					pUnit->SetDestination(pShieldCell, true);
					pUnit->QueueMission(Mission::Area_Guard, true);
					continue;
				}
			}
		}

		// Slot 60 (Spawner Standoff & AA/Sensor Combined-Arms Escort):
		// 1) Keep Spawner=yes units (Aircraft Carrier, Dreadnought, V3) at standoff
		// distance (> 5 cells) from approaching enemies!
		if (pPrimaryWpn && pPrimaryWpn->Spawner && scores[60] > 100 && homeCoords != CellStruct::Empty &&
		    (Unsorted::CurrentFrame % 45 < 15))
		{
			bool enemyClosingOnSpawner = false;
			for (auto const pET : TechnoClass::Array)
			{
				if (!pET || pET->InLimbo || !pET->IsAlive || !pET->Owner)
					continue;
				if (pET->Owner == pHouse || pHouse->IsAlliedWith(pET->Owner) || pET->Owner->IsNeutral())
					continue;
				if (pUnit->DistanceFrom(pET) <= 1280)
				{
					enemyClosingOnSpawner = true;
					break;
				}
			}
			if (enemyClosingOnSpawner)
			{
				const CellStruct uc = pUnit->GetMapCoords();
				const int sx = (homeCoords.X > uc.X) ? 4 : ((homeCoords.X < uc.X) ? -4 : 0);
				const int sy = (homeCoords.Y > uc.Y) ? 4 : ((homeCoords.Y < uc.Y) ? -4 : 0);
				const CellStruct retreatCell{static_cast<short>(uc.X + sx), static_cast<short>(uc.Y + sy)};
				if (auto const pRC = MapClass::Instance.TryGetCellAt(retreatCell))
				{
					if (pRC->IsClearToMove(pType->SpeedType, true, true, -1, pType->MovementZone, -1, false))
					{
						pUnit->SetDestination(pRC, true);
						pUnit->QueueMission(Mission::Move, true);
						continue;
					}
				}
			}
		}

		// 2) Escort our lead Heavy/Siege Tank with mobile AA, Sensor, GapGenerator
		// & Mobile Stealth Generator vehicles (Slots 60 & 63)!
		if (pLeadHeavyTank && pUnit != pLeadHeavyTank && isUnitIdle && !pType->Naval &&
		    (((pPrimaryWpn && pPrimaryWpn->Projectile && pPrimaryWpn->Projectile->AA) || pType->Sensors) &&
		         scores[60] > 120 ||
		     pType->GapGenerator && scores[63] > 80) &&
		    (Unsorted::CurrentFrame % 90 < 20))
		{
			const int distToLead = pUnit->DistanceFrom(pLeadHeavyTank);
			if (distToLead > 768 && distToLead <= 4608)
			{
				if (auto const pLeadCell = pLeadHeavyTank->GetCell())
				{
					pUnit->SetDestination(pLeadCell, true);
					pUnit->QueueMission(Mission::Area_Guard, true);
					continue;
				}
			}
			else if (pType->GapGenerator && pType->IsSimpleDeployer && !pUnit->GapSuperCharged && distToLead <= 768)
			{
				pUnit->QueueMission(Mission::Unload, true);
				continue;
			}
		}

		// Slot 61 (Mirage Tank Dynamic Disguise & Counter-Disguise Interception):
		if (pType->CanDisguise && !pType->PermaDisguise && !pUnit->Disguised && pSampleEnemyInfantry &&
		    pUnit->CanDisguiseAs(pSampleEnemyInfantry) && scores[61] > 80)
		{
			pUnit->DisguiseAs(pSampleEnemyInfantry);
		}

		// Slot 83 (Mirage Tank & Camouflage Tree-Grove Ambush, Spotted Retreat &
		// Ambush Zone Rotation): 1) Position Mirage Tanks (CanDisguise /
		// PermaDisguise) and ground Stealth Ambush tanks (Cloakable && !Naval)
		// inside tree groves (GetTerrain) along enemy Harvester/reinforcement
		// corridors and mimic the exact tree species of the grove! 2) Ambush
		// unsuspecting critical enemy units (Harvesters, Engineers, Artillery,
		// wounded vehicles) that pass within weapon range without leaving the tree
		// grove. 3) If spotted/attacked by the enemy (HasBeenAttacked, HP < 78%,
		// targeted by enemy tanks, or inside enemy DisguiseSensors), retreat
		// immediately back to our home base / Service Depot! 4) After regrouping or
		// after an ambush epoch (~900 frames), rotate to a different tree grove
		// sector to keep harassing the enemy from new angles!
		const bool isCamoAmbushTank = !pType->Naval && !pType->Harvester && !pType->ResourceGatherer &&
		                              (pType->CanDisguise || pType->PermaDisguise || pType->Cloakable) && pPrimaryWpn &&
		                              pPrimaryWpn->Damage > 0;
		if (isCamoAmbushTank && scores[83] > 60 && homeCoords != CellStruct::Empty)
		{
			const CellStruct uc = pUnit->GetMapCoords();
			auto const pUC = pUnit->GetCell();
			int adjacentTrees = 0;
			TerrainClass* pNearTree = nullptr;
			for (int dy = -1; dy <= 1; ++dy)
			{
				for (int dx = -1; dx <= 1; ++dx)
				{
					const CellStruct nc{static_cast<short>(uc.X + dx), static_cast<short>(uc.Y + dy)};
					if (auto const pNC = MapClass::Instance.TryGetCellAt(nc))
					{
						if (auto const pTer = pNC->GetTerrain(false))
						{
							adjacentTrees++;
							if (!pNearTree)
								pNearTree = pTer;
						}
					}
				}
			}

			// Mimic the exact tree species of the surrounding grove when stationary!
			if ((pType->CanDisguise || pType->PermaDisguise) && pNearTree && isUnitIdle &&
			    (!pUnit->Disguised || (Unsorted::CurrentFrame % 300 == 0)) &&
			    pUnit->CanDisguiseAs(reinterpret_cast<AbstractClass*>(pNearTree)))
			{
				pUnit->DisguiseAs(reinterpret_cast<AbstractClass*>(pNearTree));
			}

			// Check if the enemy has spotted / compromised our ambush ("si les
			// pillan")
			bool ambushSpotted = (pUnit->GetHealthPercentage() < 0.78) ||
			                     (pUnit->HasBeenAttacked && pUnit->GetHealthPercentage() < 0.92);
			if (!ambushSpotted && pUC && pHouse->EnemyHouseIndex >= 0 && pHouse->EnemyHouseIndex < 24 &&
			    pUC->DisguiseSensors_InclHouse(static_cast<unsigned int>(pHouse->EnemyHouseIndex)))
			{
				ambushSpotted = true;
			}
			if (!ambushSpotted)
			{
				int closeHostileTanks = 0;
				for (auto const pEU : UnitClass::Array)
				{
					if (!pEU || pEU->InLimbo || !pEU->IsAlive || !pEU->Owner || !pEU->Type || pEU->Type->Harvester ||
					    pEU->Type->ResourceGatherer)
						continue;
					if (pEU->Owner == pHouse || pHouse->IsAlliedWith(pEU->Owner) || pEU->Owner->IsNeutral())
						continue;
					if (pEU->Target == pUnit)
					{
						ambushSpotted = true;
						break;
					}
					if (pUnit->DistanceFrom(pEU) <= 1152 && ++closeHostileTanks >= 2)
					{
						ambushSpotted = true;
						break;
					}
				}
			}

			if (ambushSpotted && uc.DistanceFrom(homeCoords) > 10.0)
			{
				pUnit->SetTarget(nullptr);
				if (pRepairDepot && pUnit->GetHealthPercentage() < 0.85)
				{
					pUnit->SetDestination(pRepairDepot, true);
					pUnit->QueueMission(Mission::Enter, true);
				}
				else if (auto const pHomeCell = MapClass::Instance.TryGetCellAt(homeCoords))
				{
					pUnit->SetDestination(pHomeCell, true);
					pUnit->QueueMission(Mission::Move, true);
				}
				continue;
			}

			// If concealed in a tree grove away from base, snipe unsuspecting
			// critical enemies that walk directly into our weapon range!
			if (!ambushSpotted && adjacentTrees >= 1 && uc.DistanceFrom(homeCoords) > 10.0)
			{
				TechnoClass* pAmbushVictim = nullptr;
				if (pVisibleEnemyHarvester && pUnit->DistanceFrom(pVisibleEnemyHarvester) <= pPrimaryWpn->Range)
				{
					pAmbushVictim = pVisibleEnemyHarvester;
				}
				else if (pEnemySpecialistInf && pUnit->DistanceFrom(pEnemySpecialistInf) <= pPrimaryWpn->Range)
				{
					pAmbushVictim = pEnemySpecialistInf;
				}
				else if (pEnemyArtilleryUnit && pUnit->DistanceFrom(pEnemyArtilleryUnit) <= pPrimaryWpn->Range)
				{
					pAmbushVictim = pEnemyArtilleryUnit;
				}
				else if (pWoundedEnemyVehicle && pUnit->DistanceFrom(pWoundedEnemyVehicle) <= pPrimaryWpn->Range)
				{
					pAmbushVictim = pWoundedEnemyVehicle;
				}
				else if (pSampleEnemyInfantry && pUnit->DistanceFrom(pSampleEnemyInfantry) <= pPrimaryWpn->Range)
				{
					pAmbushVictim = pSampleEnemyInfantry;
				}

				if (pAmbushVictim)
				{
					pUnit->SetTarget(pAmbushVictim);
					pUnit->QueueMission(Mission::Attack, true);
					continue;
				}
			}

			// Move to a tree grove along enemy Harvester/transit routes (or rotate to
			// a new grove sector every ~900 frames so the enemy cannot predict us!)
			const bool shouldRotateGrove = (Unsorted::CurrentFrame % 900 < 20);
			if (isUnitIdle && pUnit->GetHealthPercentage() >= 0.85 &&
			    (adjacentTrees == 0 || uc.DistanceFrom(homeCoords) <= 11.0 || shouldRotateGrove) &&
			    (Unsorted::CurrentFrame % 60 < 20))
			{
				CellStruct enemyFocus = homeCoords;
				if (pVisibleEnemyHarvester)
					enemyFocus = pVisibleEnemyHarvester->GetMapCoords();
				else if (pSurgicalEnemyBld)
					enemyFocus = pSurgicalEnemyBld->GetMapCoords();
				else if (pPrioritySiegeBld)
					enemyFocus = pPrioritySiegeBld->GetMapCoords();

				const CellStruct searchCenter{static_cast<short>((homeCoords.X + enemyFocus.X) / 2),
				                              static_cast<short>((homeCoords.Y + enemyFocus.Y) / 2)};
				const int desiredSector = ((Unsorted::CurrentFrame / 900) + pUnit->UniqueID) % 4;
				CellClass* pBestGroveCell = nullptr;
				int bestGroveScore = -99999;

				for (int dy = -28; dy <= 28; dy += 2)
				{
					for (int dx = -28; dx <= 28; dx += 2)
					{
						const CellStruct gc{static_cast<short>(searchCenter.X + dx),
						                    static_cast<short>(searchCenter.Y + dy)};
						auto const pGC = MapClass::Instance.TryGetCellAt(gc);
						if (!pGC || pGC->GetTerrain(false) != nullptr ||
						    !pGC->IsClearToMove(pType->SpeedType, true, true, -1, pType->MovementZone, -1, false))
						{
							continue;
						}
						if (std::max(std::abs(gc.X - homeCoords.X), std::abs(gc.Y - homeCoords.Y)) <= 11 ||
						    std::max(std::abs(gc.X - uc.X), std::abs(gc.Y - uc.Y)) <= 6)
						{
							continue;
						}

						// Count adjacent trees & nearby Ore (Harvester transit route!)
						int groveTrees = 0;
						bool nearOreRoute = (pGC->GetContainedTiberiumValue() > 0);
						for (int ty = -1; ty <= 1; ++ty)
						{
							for (int tx = -1; tx <= 1; ++tx)
							{
								const CellStruct tc2{static_cast<short>(gc.X + tx), static_cast<short>(gc.Y + ty)};
								if (auto const pTC2 = MapClass::Instance.TryGetCellAt(tc2))
								{
									if (pTC2->GetTerrain(false) != nullptr)
										groveTrees++;
									if (pTC2->GetContainedTiberiumValue() > 0)
										nearOreRoute = true;
								}
							}
						}
						if (groveTrees == 0)
							continue;

						// Ensure outside enemy base defense range (> 11 cells)
						bool nearEnemyTurret = false;
						for (auto const pEB : BuildingClass::Array)
						{
							if (!pEB || pEB->InLimbo || !pEB->IsAlive || !pEB->Owner || !pEB->Type ||
							    pEB->Type->BuildCat != BuildCat::Combat)
								continue;
							if (pEB->Owner == pHouse || pHouse->IsAlliedWith(pEB->Owner) || pEB->Owner->IsNeutral())
								continue;
							const CellStruct ec = pEB->GetMapCoords();
							if (std::max(std::abs(gc.X - ec.X), std::abs(gc.Y - ec.Y)) <= 11)
							{
								nearEnemyTurret = true;
								break;
							}
						}
						if (nearEnemyTurret)
							continue;

						const int cellSector = ((dx >= 0 ? 1 : 0) + (dy >= 0 ? 2 : 0));
						int gScore = groveTrees * 35 + (nearOreRoute ? 90 : 0) +
						             (cellSector == desiredSector ? 120 : 0) -
						             (std::abs(gc.X - enemyFocus.X) + std::abs(gc.Y - enemyFocus.Y));
						if (gScore > bestGroveScore)
						{
							bestGroveScore = gScore;
							pBestGroveCell = pGC;
						}
					}
				}

				if (pBestGroveCell)
				{
					pUnit->SetDestination(pBestGroveCell, true);
					pUnit->QueueMission(Mission::Move, true);
					continue;
				}
			}
		}
		if (isUnitIdle && !pType->Naval && (pType->DetectDisguise || pType->Sensors))
		{
			if (pDetectedEnemyInfiltrator && pUnit->DistanceFrom(pDetectedEnemyInfiltrator) <= 3840)
			{
				pUnit->SetTarget(pDetectedEnemyInfiltrator);
				pUnit->QueueMission(Mission::Attack, true);
				continue;
			}
			TechnoClass* pSweepTarget = pHiddenOrStealthEnemy ? pHiddenOrStealthEnemy : pDetectedEnemyInfiltrator;
			if (pSweepTarget)
			{
				if (auto const pSweepCell = pSweepTarget->GetCell())
				{
					pUnit->SetDestination(pSweepCell, true);
					pUnit->QueueMission(Mission::AttackMove, true);
					continue;
				}
			}
		}

		// Slot 62 (Slave Miner Liberation): If our unit is shooting an enslaved
		// worker (Slaved=yes) whose SlaveOwner (Slave Miner) is alive nearby,
		// redirect fire to the SlaveOwner so all 5 slaves flip to our House!
		if (auto const pCurTargetInf = abstract_cast<InfantryClass*>(pUnit->Target))
		{
			if (pCurTargetInf->Type && pCurTargetInf->Type->Slaved && pCurTargetInf->SlaveOwner &&
			    pCurTargetInf->SlaveOwner->IsAlive && !pCurTargetInf->SlaveOwner->InLimbo && scores[62] > 60)
			{
				pUnit->SetTarget(pCurTargetInf->SlaveOwner);
				pUnit->QueueMission(Mission::Attack, true);
				continue;
			}
		}
		if (pEnemySlaveMiner && isUnitIdle && !pType->Naval && scores[62] > 140 &&
		    pUnit->DistanceFrom(pEnemySlaveMiner) <= 4096)
		{
			pUnit->SetTarget(pEnemySlaveMiner);
			pUnit->QueueMission(Mission::Attack, true);
			continue;
		}

		// Slot 64 (Active Ally De-Parasiting via Sonic, RemoveParasite, or Damage <
		// 0): Dolphins (Sonic=yes), Repair IFVs (Damage < 0), and units with Phobos
		// RemoveParasite=yes immediately free allied vehicles/ships being eaten by
		// a Giant Squid or Terror Drone!
		const auto pWH = pPrimaryWpn ? pPrimaryWpn->Warhead : nullptr;
		const auto pWHExt = pWH ? WarheadTypeExt::TryFetch(pWH) : nullptr;
		const bool canCureParasite = pPrimaryWpn && (pPrimaryWpn->Damage < 0 || (pWH && pWH->Sonic) ||
		                                             (pWHExt && pWHExt->RemoveParasite.Get(false)));
		if (pParasitedOwnUnit && pParasitedOwnUnit != pUnit && isUnitIdle && canCureParasite && scores[64] > 60 &&
		    pUnit->DistanceFrom(pParasitedOwnUnit) <= 4096)
		{
			pUnit->SetTarget(pParasitedOwnUnit);
			pUnit->QueueMission(Mission::Attack, true);
			continue;
		}

		// Slot 64 (Offensive Parasite Targeting: Terror Drone, Giant Squid,
		// Cyberworm): Target expensive enemy tanks/harvesters/warships that do NOT
		// yet have a parasite!
		if (pWH && pWH->Parasite && isUnitIdle && pBestParasiteVictim && pUnit->ParasiteImUsing == nullptr &&
		    scores[64] > 60 && pType->Naval == pBestParasiteVictim->Type->Naval &&
		    pUnit->DistanceFrom(pBestParasiteVictim) <= 4608)
		{
			pUnit->SetTarget(pBestParasiteVictim);
			pUnit->QueueMission(Mission::Attack, true);
			continue;
		}

		// Slot 66 (EMP Pulse Targeting & EMP-Paralyzed Target Finisher):
		if (pWH && pWH->EMEffect && isUnitIdle && pEnemyNonEMPedHeavy && scores[66] > 80 &&
		    pUnit->DistanceFrom(pEnemyNonEMPedHeavy) <= 4096)
		{
			pUnit->SetTarget(pEnemyNonEMPedHeavy);
			pUnit->QueueMission(Mission::Attack, true);
			continue;
		}
		else if (pEnemyEMPedTarget && isUnitIdle && pPrimaryWpn && pPrimaryWpn->Damage > 0 &&
		         (!pWH || !pWH->EMEffect) && scores[66] > 100 && pUnit->DistanceFrom(pEnemyEMPedTarget) <= 3584)
		{
			pUnit->SetTarget(pEnemyEMPedTarget);
			pUnit->QueueMission(Mission::Attack, true);
			continue;
		}

		// Slot 67 (Tank Bunker Garrisoning): Put idle Bunkerable=yes combat tanks
		// into an empty Tank Bunker for armored perimeter defense!
		if (pEmptyTankBunker && isUnitIdle && pType->Bunkerable && !pType->Naval && !pType->Harvester &&
		    pUnit->BunkerLinkedItem == nullptr && scores[67] > 80 && pUnit->DistanceFrom(pEmptyTankBunker) <= 3584)
		{
			pUnit->SetDestination(pEmptyTankBunker, true);
			pUnit->QueueMission(Mission::Enter, true);
			pEmptyTankBunker = nullptr;
			continue;
		}

		// Slot 68 (Psychedelic Chaos Gas): Target non-berserk heavy enemy tanks!
		if (pWH && pWH->Psychedelic && isUnitIdle && pEnemyChaosTarget && scores[68] > 80 &&
		    pUnit->DistanceFrom(pEnemyChaosTarget) <= 3584)
		{
			pUnit->SetTarget(pEnemyChaosTarget);
			pUnit->QueueMission(Mission::Attack, true);
			continue;
		}

		// Slot 69 (Submarine Cloak Ambush & Anti-Submarine Destroyer Escort):
		if (pType->Naval && scores[69] > 80)
		{
			if (pType->Cloakable && pUnit->GetHealthPercentage() < 0.45 && homeCoords != CellStruct::Empty &&
			    pUnit->CurrentMission != Mission::Move)
			{
				if (auto const pHC = MapClass::Instance.TryGetCellAt(homeCoords))
				{
					pUnit->SetDestination(pHC, true);
					pUnit->QueueMission(Mission::Move, true);
					continue;
				}
			}
			else if (isUnitIdle && pEnemySubOrShip && (pType->Sensors || (pPrimaryWpn && pPrimaryWpn->Damage > 0)))
			{
				if (auto const pSubCell = pEnemySubOrShip->GetCell())
				{
					if (pType->Sensors || IsCellVisibleToAI(pHouse, pSubCell))
					{
						pUnit->SetDestination(pSubCell, true);
						pUnit->QueueMission(Mission::AttackMove, true);
						continue;
					}
				}
			}
			else if (pType->Sensors && pLeadWarship && pUnit != pLeadWarship && isUnitIdle &&
			         (Unsorted::CurrentFrame % 90 < 20))
			{
				const int distW = pUnit->DistanceFrom(pLeadWarship);
				if (distW > 768 && distW <= 4608)
				{
					if (auto const pWC = pLeadWarship->GetCell())
					{
						pUnit->SetDestination(pWC, true);
						pUnit->QueueMission(Mission::Area_Guard, true);
						continue;
					}
				}
			}
		}

		// Field Repair Vehicles (Repair IFV / Mobile Mechanic / Support Drone where
		// Damage < 0): Heal damaged own OR allied vehicles in the field!
		if (pPrimaryWpn && pPrimaryWpn->Damage < 0 && isUnitIdle)
		{
			for (auto const pAllyU : UnitClass::Array)
			{
				if (!pAllyU || pAllyU == pUnit || !pAllyU->Owner || !pHouse->IsAlliedWith(pAllyU->Owner) ||
				    pAllyU->InLimbo || !pAllyU->IsAlive || !pAllyU->Type || pAllyU->Type->Organic)
					continue;
				if ((pAllyU->ParasiteEatingMe != nullptr || pAllyU->GetHealthPercentage() < 0.85) &&
				    pUnit->DistanceFrom(pAllyU) <= 3584)
				{
					pUnit->SetTarget(pAllyU);
					pUnit->QueueMission(Mission::Attack, true);
					break;
				}
			}
			continue;
		}

		// Yuri Floating Disc (DrainWeapon=yes): Prioritize draining Enemy
		// Superweapons, Refineries (steal credits!), and Power Plants (blackout!)
		if (pPrimaryWpn && pPrimaryWpn->DrainWeapon && isUnitIdle && pUnit->DrainTarget == nullptr)
		{
			BuildingClass* pBestDrainBld = nullptr;
			int bestDrainScore = -1;
			for (auto const pEBld : BuildingClass::Array)
			{
				if (!pEBld || pEBld->InLimbo || !pEBld->IsAlive || !pEBld->Owner || !pEBld->Type ||
				    pEBld->DrainingMe != nullptr)
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

		// Tank Bunker Garrisoning (Bunkerable=yes into empty owned Tank Bunker):
		// Fortify base perimeter!
		if (pType->Bunkerable && isUnitIdle && pUnit->BunkerLinkedItem == nullptr && (Unsorted::CurrentFrame % 90 < 20))
		{
			for (auto const pBld : pHouse->Buildings)
			{
				if (!pBld || pBld->InLimbo || !pBld->IsAlive || !pBld->Type || !pBld->Type->Bunker ||
				    pBld->BunkerLinkedItem != nullptr)
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

	// Special Operations: Spies, Bridge Repair, Urban Garrisoning, Bio-Reactors,
	// Tesla Overpowering, Hospitals, Medics, Crazy Ivan & Bridge Demolition
	BuildingClass* pTargetBridgeHut = nullptr;
	BuildingClass* pBioReactor = nullptr;
	BuildingClass* pOverpowerCoil = nullptr;
	BuildingClass* pHospital = nullptr;
	for (auto const pBld : BuildingClass::Array)
	{
		if (!pBld || pBld->InLimbo || !pBld->IsAlive || !pBld->Type)
			continue;

		if (scores[21] > 400 && !pTargetBridgeHut && pBld->Type->BridgeRepairHut &&
		    MapClass::Instance.IsLinkedBridgeDestroyed(pBld->GetMapCoords()))
			pTargetBridgeHut = pBld;

		if (pBld->Owner && pHouse->IsAlliedWith(pBld->Owner))
		{
			if (pBld->Owner == pHouse && !pBioReactor && pHouse->HasLowPower() && pBld->Type->InfantryAbsorb &&
			    pBld->Passengers.NumPassengers < pBld->Type->Passengers)
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

		// Evade radiation puddles (Desolator / Nuke fallout) immediately if not
		// radiation-immune; if we ARE a radiation-immune Deployer (Desolator,
		// Slot 63), tactically deploy to irradiate nearby enemy squads/slaves and
		// undeploy once the zone is cleared!
		if (auto const pInfCell = pInf->GetCell())
		{
			const int cellRad = pInfCell->GetRadLevel();
			if (cellRad > 10 && !pInf->Type->ImmuneToRadiation && pInf->CurrentMission != Mission::Move)
			{
				pInf->Scatter(CoordStruct::Empty, true, false);
				continue;
			}
			else if (pInf->Type->Deployer && pInf->Type->ImmuneToRadiation && scores[63] > 80 &&
			         (Unsorted::CurrentFrame % 45 < 15))
			{
				int enemiesInRadRange = 0;
				for (auto const pET : TechnoClass::Array)
				{
					if (!pET || pET->InLimbo || !pET->IsAlive || !pET->Owner ||
					    pET->WhatAmI() == AbstractType::Building || pET->WhatAmI() == AbstractType::Aircraft)
						continue;
					if (pET->Owner == pHouse || pHouse->IsAlliedWith(pET->Owner) || pET->Owner->IsNeutral())
						continue;
					if (pInf->DistanceFrom(pET) <= 1536)
						enemiesInRadRange++;
				}
				if (!pInf->IsDeployed() && enemiesInRadRange >= 2 && cellRad < 180)
				{
					pInf->QueueMission(Mission::Unload, true);
					continue;
				}
				else if (pInf->IsDeployed() && enemiesInRadRange == 0 && cellRad >= 100)
				{
					pInf->QueueMission(Mission::Unload, true);
					continue;
				}
			}
		}

		const bool isIdle = (pInf->CurrentMission == Mission::Guard || pInf->CurrentMission == Mission::Area_Guard);
		const auto pInfWpn = pInf->Type->GetWeapon(0) ? pInf->Type->GetWeapon(0)->WeaponType : nullptr;

		// Slot 82 (Infantry Human Shield & Anti-Engineer Recapture Screen for
		// Stolen ConYard / Escaping MCV): Nearby friendly combat infantry (<= 14
		// cells) immediately gun down enemy Engineers trying to recapture our
		// stolen ConYard, or form a human shield perimeter around the stolen
		// ConYard/MCV to absorb enemy fire during extraction!
		if (pStolenConYardOrMCV && !pInf->Type->Engineer && !pInf->Type->Infiltrate && scores[82] > 60 &&
		    pInf->DistanceFrom(pStolenConYardOrMCV) <= 3584)
		{
			if (pThreatToStolenAsset && pInfWpn && pInfWpn->Damage > 0 && pInf->Target != pThreatToStolenAsset)
			{
				pInf->SetTarget(pThreatToStolenAsset);
				pInf->QueueMission(Mission::Attack, true);
				continue;
			}
			else if (isIdle && pInf->DistanceFrom(pStolenConYardOrMCV) > 512)
			{
				if (auto const pShieldCell = pStolenConYardOrMCV->GetCell())
				{
					pInf->SetDestination(pShieldCell, true);
					pInf->QueueMission(Mission::Area_Guard, true);
					continue;
				}
			}
		}

		// Slot 57 (Combat Infantry Deploy): Deploy GI / Guardian GI / Desolator
		// (Deployer=yes) when actively engaging an enemy in combat!
		if (pInf->Type->Deployer && pInf->Target != nullptr && pInf->CurrentMission == Mission::Attack &&
		    scores[57] > 100 && (Unsorted::CurrentFrame % 60 < 20))
		{
			pInf->QueueMission(Mission::Unload, true);
			continue;
		}

		// Slot 58 (Mind-Control Breaker): Snipers / Virus / ImmuneToPsionics
		// infantry eliminate enemy Yuri/Mastermind controllers!
		if (pEnemyMindController && isIdle &&
		    (pInf->Type->ImmuneToPsionics || (pInfWpn && pInfWpn->Range >= (6 * 256))) &&
		    pInf->DistanceFrom(pEnemyMindController) <= 4096)
		{
			pInf->SetTarget(pEnemyMindController);
			pInf->QueueMission(Mission::Attack, true);
			continue;
		}

		// Slot 59 (Crate Pickup): Send nearest idle infantry to pick up a visible
		// map Crate!
		if (pVisibleCrateCell && isIdle && !pInf->Type->Engineer && pInf->DistanceFrom(pVisibleCrateCell) <= 3072)
		{
			pInf->SetDestination(pVisibleCrateCell, true);
			pInf->QueueMission(Mission::Move, true);
			pVisibleCrateCell = nullptr;
			continue;
		}

		// Slot 65 (Grinder Mind-Control Recycling for Infantry): Send
		// mind-controlled infantry into our Grinder for instant credits and to free
		// Yuri's control slot!
		if (pGrinderBld && pInf->MindControlledBy != nullptr && scores[65] > 80)
		{
			pInf->SetDestination(pGrinderBld, true);
			pInf->QueueMission(Mission::Enter, true);
			continue;
		}

		// Slot 68 (TiberiumHeal Infantry Regeneration & Sniper/Poison Specialist
		// Hunter):
		if (pInf->Type->TiberiumHeal && pInf->GetHealthPercentage() < 0.55 && scores[68] > 60 &&
		    pInf->CurrentMission != Mission::Move)
		{
			if (pInf->MoveToTiberium(8, false))
				continue;
		}
		if (pEnemySpecialistInf && isIdle && pInfWpn && pInfWpn->Damage > 0 && !pInf->Type->Engineer &&
		    ((pInfWpn->Warhead && pInfWpn->Warhead->Poison) || pInfWpn->Range >= (6 * 256)) && scores[68] > 80 &&
		    pInf->DistanceFrom(pEnemySpecialistInf) <= 4096)
		{
			pInf->SetTarget(pEnemySpecialistInf);
			pInf->QueueMission(Mission::Attack, true);
			continue;
		}

		// Retreat wounded valuable/Elite infantry (< 55% HP) to Hospital if owned!
		if (pHospital && !pInf->Type->Engineer && (pInf->Type->Cost >= 300 || pInf->Veterancy.IsElite()) &&
		    pInf->GetHealthPercentage() < 0.55)
		{
			if (pInf->CurrentMission != Mission::Enter && pInf->DistanceFrom(pHospital) <= 4096)
			{
				pInf->SetDestination(pHospital, true);
				pInf->QueueMission(Mission::Enter, true);
				continue;
			}
		}

		// Combat Medics / Field Mechanics (Damage < 0): Heal wounded own OR allied
		// infantry nearby!
		if (pInfWpn && pInfWpn->Damage < 0 && isIdle)
		{
			for (auto const pAllyInf : InfantryClass::Array)
			{
				if (!pAllyInf || pAllyInf == pInf || !pAllyInf->Owner || !pHouse->IsAlliedWith(pAllyInf->Owner) ||
				    pAllyInf->InLimbo || !pAllyInf->IsAlive)
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

		// Chrono Legionnaire (Temporal=yes) & Crazy Ivan (Ivan=yes): Prioritize
		// high-cost enemy units/buildings!
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
				if (pEnemyU->GetCell() && IsCellVisibleToAI(pHouse, pEnemyU->GetCell()) &&
				    pEnemyU->Type->Cost > bestSpecCost && pInf->DistanceFrom(pEnemyU) <= 4608)
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

		// Send idle Engineer to disarm Ivan Bombs on our own OR allied
		// buildings/tanks, repair destroyed bridges, or capture Neutral Tech
		// Buildings (Oil Derricks, Tech Outposts, Airports) and damaged enemy
		// buildings (respecting Multi-Engineer)
		if (pInf->Type->Engineer && isIdle)
		{
			// Disarm any attached Crazy Ivan Bomb on our own OR allied high-value
			// buildings!
			bool disarmedBomb = false;
			for (auto const pAllyBld : BuildingClass::Array)
			{
				if (pAllyBld && !pAllyBld->InLimbo && pAllyBld->IsAlive && pAllyBld->Owner &&
				    pHouse->IsAlliedWith(pAllyBld->Owner) && pAllyBld->AttachedBomb != nullptr &&
				    pInf->DistanceFrom(pAllyBld) <= 3584)
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

			// Repair heavily damaged friendly-occupied or empty urban garrisonable
			// buildings (CanBeOccupied && Repairable) to 100% full health so our
			// infantry hold an unbreakable urban stronghold!
			if (pDamagedUrbanToRepair && pInf->DistanceFrom(pDamagedUrbanToRepair) <= 4096)
			{
				pInf->SetDestination(pDamagedUrbanToRepair, true);
				pInf->QueueMission(Mission::Enter, true);
				pDamagedUrbanToRepair = nullptr;
				continue;
			}

			const double capLevel = RulesClass::Instance ? RulesClass::Instance->EngineerCaptureLevel : 1.0;
			const int engCaptureBias = NeuralAI::PolicyFor(pHouse).OutputBias[36];
			BuildingClass* pBestCapBld = nullptr;
			int bestCapDist = 999999;
			for (auto const pBld : BuildingClass::Array)
			{
				if (!pBld || pBld->InLimbo || !pBld->IsAlive || !pBld->Type || !pBld->Owner)
					continue;

				if (!pBld->Type->Capturable || pBld->Owner == pHouse || pHouse->IsAlliedWith(pBld->Owner))
					continue;

				auto const pCell = pBld->GetCell();
				if (!pCell)
					continue;

				const bool isNeutralTech = pBld->Owner->IsNeutral();
				// Neutral map tech structures (Oil Derricks, Hospitals, Outposts,
				// Secret Labs) are known map landmarks; Enemy buildings require
				// line-of-sight or active Engineer raid bias.
				if (!isNeutralTech && !IsCellVisibleToAI(pHouse, pCell))
					continue;

				const bool isHighValueEnemyBld =
				    !isNeutralTech && (pBld->Type->ConstructionYard || pBld->Type->Refinery ||
				                       pBld->Type->Factory != AbstractType::None || pBld->Type->SuperWeapon != -1 ||
				                       pBld->Type->PowerBonus > 0);
				const bool canCaptureNow = isNeutralTech || (pBld->GetHealthPercentage() <= capLevel + 0.05) ||
				                           (engCaptureBias > 60 && isHighValueEnemyBld) ||
				                           (scores[82] > 60 && pBld->Type->ConstructionYard);
				if (!canCaptureNow)
					continue;

				const int dist = pInf->DistanceFrom(pBld);
				const int effectiveDist = (!isNeutralTech && pBld->Type->ConstructionYard && scores[82] > 60)
				                              ? (dist / 3)
				                              : (isNeutralTech ? (dist / 2) : dist);
				if (effectiveDist < bestCapDist)
				{
					bestCapDist = effectiveDist;
					pBestCapBld = pBld;
				}
			}
			if (pBestCapBld)
			{
				pInf->SetDestination(pBestCapBld, true);
				pInf->QueueMission(Mission::Enter, true);
			}
			continue;
		}

		// Slot 61 (Dynamic Spy Disguise via DisguiseAs): Ensure our Spy disguises
		// specifically as an enemy infantryman rather than a friendly/default unit!
		if (pInf->Type->CanDisguise && pSampleEnemyInfantry && scores[61] > 60 &&
		    (!pInf->Disguised || !pInf->DisguisedAsHouse || pHouse->IsAlliedWith(pInf->DisguisedAsHouse)) &&
		    pInf->CanDisguiseAs(pSampleEnemyInfantry))
		{
			pInf->DisguiseAs(pSampleEnemyInfantry);
		}

		// Slot 62 (Slave Miner Liberation for Infantry): Redirect fire from Slaved
		// workers onto their SlaveOwner so killing the miner frees the slaves!
		if (auto const pCurTargetInf = abstract_cast<InfantryClass*>(pInf->Target))
		{
			if (pCurTargetInf->Type && pCurTargetInf->Type->Slaved && pCurTargetInf->SlaveOwner &&
			    pCurTargetInf->SlaveOwner->IsAlive && !pCurTargetInf->SlaveOwner->InLimbo && scores[62] > 60)
			{
				pInf->SetTarget(pCurTargetInf->SlaveOwner);
				pInf->QueueMission(Mission::Attack, true);
				continue;
			}
		}

		// Spy Full-Spectrum Sabotage: Infiltrate Enemy Power Plants (Blackout!),
		// Battle Labs (Secret Tech!), Superweapons, Refineries & Factories
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
					spyScore = 90; // Steal enemy credits!
				else if (pEnemyBld->Type->PowerBonus > 0)
					spyScore = 85; // Black out enemy base defenses!
				else if (pEnemyBld->Type->Factory != AbstractType::None)
					spyScore = 80; // Gain Veteran production!
				else if (pEnemyBld->Type->TechLevel >= 6)
					spyScore = 75; // Battle Lab / Radar sabotage!

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

		// Credit Thief (Thief=yes): Infiltrate enemy Refineries & Ore Silos to
		// steal 50% of the enemy house's credits!
		if (pInf->Type->Thief && !pInf->Type->Infiltrate && isIdle)
		{
			BuildingClass* pBestThiefTarget = nullptr;
			int bestThiefDist = 999999;
			for (auto const pEnemyBld : BuildingClass::Array)
			{
				if (!pEnemyBld || pEnemyBld->InLimbo || !pEnemyBld->IsAlive || !pEnemyBld->Owner || !pEnemyBld->Type)
					continue;
				if (pHouse->IsAlliedWith(pEnemyBld->Owner) || pEnemyBld->Owner->IsNeutral())
					continue;
				if (pEnemyBld->Type->Refinery || pEnemyBld->Type->Storage > 0)
				{
					const int d = pInf->DistanceFrom(pEnemyBld);
					if (d < bestThiefDist)
					{
						bestThiefDist = d;
						pBestThiefTarget = pEnemyBld;
					}
				}
			}
			if (pBestThiefTarget)
			{
				pInf->SetDestination(pBestThiefTarget, true);
				pInf->QueueMission(Mission::Enter, true);
				continue;
			}
		}

		// Hijacker (VehicleThief=yes): Steal the most expensive visible enemy tank
		// (Apocalypse, Mammoth Mk.II, Avatar, Prism Tank)!
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
				if (pEnemyU->GetCell() && IsCellVisibleToAI(pHouse, pEnemyU->GetCell()) &&
				    pEnemyU->Type->Cost > bestTankCost && pInf->DistanceFrom(pEnemyU) <= 5120)
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

		// Slot 61 (Guard Dogs & Counter-Spy Detectors): Immediately intercept any
		// enemy Spy, Engineer, Thief, Hijacker, or Commando approaching our base,
		// or patrol around our ConYard!
		if ((pInf->Type->Doggie || pInf->Type->DetectDisguise || pInf->Type->Sensors) && isIdle)
		{
			if (pDetectedEnemyInfiltrator && pInf->DistanceFrom(pDetectedEnemyInfiltrator) <= 3840)
			{
				pInf->SetTarget(pDetectedEnemyInfiltrator);
				pInf->QueueMission(Mission::Attack, true);
				continue;
			}
			if (pHiddenOrStealthEnemy && (Unsorted::CurrentFrame % 90 < 25))
			{
				if (auto const pSweepCell = pHiddenOrStealthEnemy->GetCell())
				{
					pInf->SetDestination(pSweepCell, true);
					pInf->QueueMission(Mission::AttackMove, true);
					continue;
				}
			}
			if (pInf->Type->Doggie && homeCoords != CellStruct::Empty && (Unsorted::CurrentFrame % 120 < 20))
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
		}

		// C4 Commandos (Tanya, Navy SEAL, Boris, Cyborg Commando): Demolish visible
		// enemy Warships (if amphibious) or high-value enemy buildings!
		if (pInf->Type->C4 && isIdle)
		{
			TechnoClass* pBestC4Target = nullptr;
			int bestC4Dist = 5120;
			const bool canSwim = (pInf->Type->MovementZone == MovementZone::Amphibious ||
			                      pInf->Type->MovementZone == MovementZone::AmphibiousDestroyer ||
			                      pInf->Type->SpeedType == SpeedType::Hover);
			if (canSwim)
			{
				for (auto const pEnemyShip : UnitClass::Array)
				{
					if (!pEnemyShip || pEnemyShip->InLimbo || !pEnemyShip->IsAlive || !pEnemyShip->Owner ||
					    !pEnemyShip->Type || !pEnemyShip->Type->Naval)
						continue;
					if (pEnemyShip->Owner == pHouse || pHouse->IsAlliedWith(pEnemyShip->Owner) ||
					    pEnemyShip->Owner->IsNeutral())
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

		// IFV (Gunner=yes), Battle Fortress (OpenTopped=yes) & Subterranean/Ground
		// APC Strike Team Boarding (Slot 53)!
		const int apcBoardBias = static_cast<int>(scores[53]);
		const int apcBoardWindow = std::clamp(20 + (apcBoardBias / 20), 10, 45);
		const int apcBoardRange = std::clamp(2304 + apcBoardBias * 2, 1536, 3840);
		if (isIdle && !pInf->Type->Infiltrate && (Unsorted::CurrentFrame % 90 < apcBoardWindow))
		{
			bool boardedCombatVehicle = false;
			for (auto const pCombVeh : UnitClass::Array)
			{
				if (!pCombVeh || pCombVeh->Owner != pHouse || pCombVeh->InLimbo || !pCombVeh->IsAlive ||
				    !pCombVeh->Type)
					continue;

				const int infPassengerSize = std::max(1, static_cast<int>(pInf->Type->TechnoTypeClass::Size));
				const bool fitsInVehicle =
				    infPassengerSize <= pCombVeh->Type->SizeLimit &&
				    (pCombVeh->Passengers.GetTotalSize() + infPassengerSize) <= pCombVeh->Type->Passengers;
				const bool isUsefulOpenToppedGunner =
				    !pInf->Type->Engineer && !pInf->Type->Doggie && !pInf->Type->Thief && !pInf->Type->VehicleThief &&
				    !pInf->Type->Ivan && pInfWpn && pInfWpn->Damage > 0 && pInfWpn->Range >= (4 * 256);
				const bool isUsefulIFVPassenger = !pInf->Type->Doggie && !pInf->Type->Thief &&
				                                  !pInf->Type->VehicleThief &&
				                                  (!pInf->Type->Engineer || pParasitedOwnUnit != nullptr);
				const bool isGunnerIFV = (fitsInVehicle && isUsefulIFVPassenger && pCombVeh->Type->Gunner &&
				                          pCombVeh->Passengers.NumPassengers == 0);
				const bool isOpenToppedFortress =
				    (fitsInVehicle && isUsefulOpenToppedGunner && pCombVeh->Type->OpenTopped &&
				     pCombVeh->Passengers.NumPassengers < pCombVeh->Type->Passengers);
				const bool isSubterraneanOrGroundAPC =
				    (!pCombVeh->Type->Naval && !pCombVeh->Type->Gunner && !pCombVeh->Type->OpenTopped &&
				     pCombVeh->Type->Passengers >= 3 &&
				     pCombVeh->Passengers.NumPassengers < pCombVeh->Type->Passengers &&
				     (pCombVeh->Type->MovementZone == MovementZone::Subterrannean || pInf->Type->Engineer ||
				      pInf->Type->C4 || apcBoardBias > 80));

				if ((isGunnerIFV || isOpenToppedFortress || isSubterraneanOrGroundAPC) &&
				    pInf->DistanceFrom(pCombVeh) <= apcBoardRange)
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

		// Slot 62 (Soviet Tesla Trooper Overpowering): Charge owned/allied
		// Overpowerable Tesla Coils via Assaulter or IsElectricBolt weapon so they
		// stay powered during LowPower and gain bonus range/damage!
		const auto pSecWpn = pInf->Type->GetWeapon(1) ? pInf->Type->GetWeapon(1)->WeaponType : nullptr;
		if (pOverpowerCoil && isIdle &&
		    (pInf->Type->Assaulter || (pInfWpn && pInfWpn->IsElectricBolt) || (pSecWpn && pSecWpn->IsElectricBolt)) &&
		    pInf->DistanceFrom(pOverpowerCoil) <= 3584)
		{
			pInf->SetTarget(pOverpowerCoil);
			pInf->QueueMission(Mission::Attack, true);
			pOverpowerCoil = nullptr;
			continue;
		}

		// Surgical Urban Garrison Clearing (Assaulter=yes or PenetratesBunker=yes):
		// Evict/kill enemy occupants inside civilian buildings without destroying
		// the structure so our Engineer can repair it and our infantry can occupy
		// it!
		if (pEnemyGarrisonToClear && isIdle && pInf->DistanceFrom(pEnemyGarrisonToClear) <= 3840)
		{
			if (pInf->Type->Assaulter)
			{
				pInf->SetDestination(pEnemyGarrisonToClear, true);
				pInf->QueueMission(Mission::Enter, true);
				continue;
			}
			else if (pInfWpn && pInfWpn->Warhead && pInfWpn->Warhead->PenetratesBunker)
			{
				pInf->SetTarget(pEnemyGarrisonToClear);
				pInf->QueueMission(Mission::Attack, true);
				continue;
			}
		}

		// Yuri Bio-Reactor Power Surge: Enter Bio-Reactor with basic infantry
		// during LowPower!
		if (pBioReactor && isIdle && pInf->Type->Cost <= 250 && pInf->DistanceFrom(pBioReactor) <= 3072)
		{
			pInf->SetDestination(pBioReactor, true);
			pInf->QueueMission(Mission::Enter, true);
			pBioReactor = nullptr;
			continue;
		}

		// Urban Garrisoning (Slot 23): Occupy nearby empty Civilian Buildings /
		// Battle Bunkers with Occupier infantry up to their real
		// MaxNumberOccupants!
		const int garrisonBias = static_cast<int>(scores[23]);
		const int garrisonWindow = std::clamp(15 + (garrisonBias / 20), 6, 40);
		const int garrisonRange = std::clamp(2560 + garrisonBias * 2, 1536, 4096);
		if (pInf->Type->Occupier && isIdle && (Unsorted::CurrentFrame % 90 < garrisonWindow))
		{
			for (auto const pBld : BuildingClass::Array)
			{
				if (!pBld || pBld->InLimbo || !pBld->IsAlive || !pBld->Type || !pBld->Type->CanBeOccupied)
					continue;

				const int maxOcc = std::clamp(pBld->Type->MaxNumberOccupants, 1, 10);
				if (pBld->CanBeOccupiedBy(pInf) && pBld->Occupants.Count < maxOcc &&
				    pInf->DistanceFrom(pBld) <= garrisonRange)
				{
					pInf->SetDestination(pBld, true);
					pInf->QueueMission(Mission::Enter, true);
					break;
				}
			}
		}
	}

	// Tactical Bridge Demolition (Ambush enemy units crossing a bridge when
	// Immune Bridges is OFF)
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

	// Autonomous Fog-of-War Scouting & Coordinated Multi-Wave Offensive Dispatch
	// (Replaces legacy aimd.ini TeamScripts)
	BuildingClass* pVisibleEnemyBld = nullptr;
	CellClass* pFallbackEnemyCell = nullptr;

	for (auto const pEnemyBld : BuildingClass::Array)
	{
		if (!pEnemyBld || pEnemyBld->InLimbo || !pEnemyBld->IsAlive || !pEnemyBld->Owner || !pEnemyBld->Type ||
		    pEnemyBld->Type->Immune)
			continue;

		auto const pOwner = pEnemyBld->Owner;
		if (pOwner == pHouse || pHouse->IsAlliedWith(pOwner) || pOwner->IsNeutral() || pOwner->IsObserver() ||
		    pOwner->Defeated)
			continue;

		auto const pCell = pEnemyBld->GetCell();
		if (!pCell)
			continue;

		if (!pFallbackEnemyCell || pOwner->ArrayIndex == pHouse->EnemyHouseIndex)
			pFallbackEnemyCell = pCell;

		const int ownerIdx = std::clamp(pOwner->ArrayIndex, 0, 7);
		const bool visible = pHouse->SpySatActive || IsCellVisibleToAI(pHouse, pCell) ||
		                     ((pExt->AIBrain_InfiltratedEnemyMask & (1 << ownerIdx)) != 0) ||
		                     (pOwner->WhoLastHurtMe == pHouse->ArrayIndex);

		if (visible)
		{
			pVisibleEnemyBld = pEnemyBld;
			if (pOwner->ArrayIndex == pHouse->EnemyHouseIndex)
				break;
		}
	}

	const bool allEnemyBldsGone = (pFallbackEnemyCell == nullptr);
	if (!pVisibleEnemyBld && !pFallbackEnemyCell)
	{
		if (pHiddenOrStealthEnemy && pHiddenOrStealthEnemy->GetCell())
		{
			pFallbackEnemyCell = pHiddenOrStealthEnemy->GetCell();
		}
		else if (pEnemySubOrShip && pEnemySubOrShip->GetCell())
		{
			pFallbackEnemyCell = pEnemySubOrShip->GetCell();
		}
		else
		{
			for (auto const pRemT : TechnoClass::Array)
			{
				if (!pRemT || pRemT->InLimbo || !pRemT->IsAlive || !pRemT->Owner ||
				    pRemT->WhatAmI() == AbstractType::Building)
					continue;
				auto const pRO = pRemT->Owner;
				if (pRO == pHouse || pHouse->IsAlliedWith(pRO) || pRO->IsNeutral() || pRO->IsObserver() ||
				    pRO->Defeated)
					continue;
				if (auto const pRC = pRemT->GetCell())
				{
					pFallbackEnemyCell = pRC;
					if (pRO->ArrayIndex == pHouse->EnemyHouseIndex)
						break;
				}
			}
		}
	}
	const bool endgameRemnantHunt =
	    (!pVisibleEnemyBld && pFallbackEnemyCell != nullptr && (allEnemyBldsGone || combatStalledForHunt));

	// Count idle combat units and infantry ready at base
	std::vector<UnitClass*> idleCombatUnits;
	std::vector<InfantryClass*> idleCombatInf;

	for (auto const pUnit : UnitClass::Array)
	{
		if (!pUnit || pUnit->Owner != pHouse || pUnit->InLimbo || !pUnit->IsAlive || !pUnit->Type)
			continue;

		if (pUnit->Type->Harvester || pUnit->Type->ResourceGatherer ||
		    (pUnit->Type->DeploysInto && pUnit->Type->DeploysInto->ConstructionYard))
			continue;

		if (pUnit->GetHealthPercentage() < 0.40)
			continue;

		// Slot 83: Keep Mirage Tanks disguised inside their ambush tree grove
		// instead of pulling them into frontal suicide waves (unless all enemy
		// buildings are gone and we are hunting down final remnants)!
		if (!endgameRemnantHunt && (pUnit->Type->CanDisguise || pUnit->Type->PermaDisguise) && scores[83] > 60 &&
		    homeCoords != CellStruct::Empty && pUnit->GetMapCoords().DistanceFrom(homeCoords) > 11.0)
		{
			continue;
		}

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

	// If no enemy building is visible yet, send a fast scout unit/infantry
	// (Slot 37 Rush/Scout timing adjusts Fog-of-War Scout Probe Frequency)
	const int scoutPeriod = std::clamp(225 - (static_cast<int>(scores[37]) / 4), 105, 360);
	if (!pVisibleEnemyBld && pFallbackEnemyCell && (Unsorted::CurrentFrame % scoutPeriod < 30))
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

	// Coordinated Offensive Strike Wave when strike force reaches evolved +
	// archetype threshold (Slot 37: Rush vs Mass Timing)
	const int arch = pExt->AIBrain_Archetype;
	const int rushWaveBias = NeuralAI::PolicyFor(pHouse).OutputBias[37];
	const int reserveBias = -rushWaveBias;
	const int baseWave = (arch == 3) ? 3 : ((arch == 2 || arch == 4) ? 7 : 5);
	const size_t minWaveUnits =
	    endgameRemnantHunt ? 1u : static_cast<size_t>(std::clamp(baseWave - (rushWaveBias / 140), 2, 10));
	const int blindPushFrame = (rushWaveBias > 100) ? 1350 : 3200;
	CellClass* pWaveTargetCell =
	    pVisibleEnemyBld
	        ? pVisibleEnemyBld->GetCell()
	        : ((endgameRemnantHunt || Unsorted::CurrentFrame > blindPushFrame) ? pFallbackEnemyCell : nullptr);

	if (pWaveTargetCell && (endgameRemnantHunt || idleCombatUnits.size() >= minWaveUnits ||
	                        (idleCombatUnits.size() + idleCombatInf.size() >= minWaveUnits + 4)))
	{
		// Keep 0..3 guard units at base depending on Slot 63 (0 during final remnant mop-up),
		// send the rest in a coordinated AttackMove / Hunt wave
		const size_t desiredUnitReserve =
		    endgameRemnantHunt ? 0u : static_cast<size_t>(std::clamp(1 + (reserveBias / 180), 0, 3));
		const size_t reserveCount = idleCombatUnits.size() > (desiredUnitReserve + 1) ? desiredUnitReserve : 0;
		for (size_t i = reserveCount; i < idleCombatUnits.size(); ++i)
		{
			auto const pU = idleCombatUnits[i];
			// Heavy Airships (Kirov / CABAL Leviathan: JumpJet && Strength >= 1000):
			// Flank along the map edge first before diving onto the enemy base!
			if (pU->Type->JumpJet && pU->Type->Strength >= 1000 && pU->DistanceFrom(pWaveTargetCell) > 7680)
			{
				const CellStruct uCoords = pU->GetMapCoords();
				const CellStruct flankCoords{uCoords.X, pWaveTargetCell->MapCoords.Y};
				if (auto const pFlankCell = MapClass::Instance.TryGetCellAt(flankCoords))
				{
					pU->SetDestination(pFlankCell, true);
					pU->QueueMission(Mission::Move, true);
					continue;
				}
			}

			if ((pVisibleEnemyBld || endgameRemnantHunt) && pU->Type->GetWeapon(0) &&
			    pU->Type->GetWeapon(0)->WeaponType)
			{
				pU->SetDestination(pWaveTargetCell, true);
				pU->QueueMission(Mission::AttackMove, true);
			}
			else
			{
				pU->QueueMission(Mission::Hunt, true);
			}
		}

		const size_t desiredInfReserve = static_cast<size_t>(std::clamp(2 + (reserveBias / 140), 0, 4));
		const size_t infReserve = idleCombatInf.size() > (desiredInfReserve + 2) ? desiredInfReserve : 0;
		for (size_t i = infReserve; i < idleCombatInf.size(); ++i)
		{
			auto const pI = idleCombatInf[i];
			pI->SetDestination(pWaveTargetCell, true);
			pI->QueueMission(Mission::AttackMove, true);
		}

		Debug::Log("[NeuralAI] Frame %d | House #%d (%s) launched Offensive Wave "
		           "(%zu Vehicles + %zu Infantry) -> Target Cell (%d,%d)\n",
		           Unsorted::CurrentFrame, pHouse->ArrayIndex,
		           (pHouse->Type && pHouse->Type->ID[0]) ? pHouse->Type->ID : "AI",
		           idleCombatUnits.size() - reserveCount, idleCombatInf.size() - infReserve,
		           pWaveTargetCell->MapCoords.X, pWaveTargetCell->MapCoords.Y);
	}

	// Amphibious Island Invasion, Subterranean/Ground APC Surgical Strike &
	// MCV/Nanocore Island Colonization
	CellClass* pIslandGoalCell = pWaveTargetCell;
	int targetIslandZone = -1;
	if (pIslandGoalCell)
		targetIslandZone =
		    MapClass::Instance.GetMovementZoneType(pIslandGoalCell->MapCoords, MovementZone::Normal, false);

	const bool enemyOnDisconnectedIsland =
	    (homeGroundZone >= 0 && targetIslandZone >= 0 && targetIslandZone != homeGroundZone);

	// Slot 75 (Immediate Beachhead Ground Push): Any combat vehicle or infantry
	// that has already landed on the enemy's island (unitZone ==
	// targetIslandZone) immediately pushes inland toward the enemy base instead
	// of waiting on the beach!
	if (enemyOnDisconnectedIsland && pIslandGoalCell && (Unsorted::CurrentFrame % 45 < 15))
	{
		for (auto const pBeachU : idleCombatUnits)
		{
			const int uz = MapClass::Instance.GetMovementZoneType(pBeachU->GetMapCoords(), MovementZone::Normal, false);
			if (uz == targetIslandZone)
			{
				pBeachU->SetDestination(pIslandGoalCell, true);
				pBeachU->QueueMission(Mission::AttackMove, true);
			}
		}
		for (auto const pBeachI : idleCombatInf)
		{
			const int iz = MapClass::Instance.GetMovementZoneType(pBeachI->GetMapCoords(), MovementZone::Normal, false);
			if (iz == targetIslandZone)
			{
				pBeachI->SetDestination(pIslandGoalCell, true);
				pBeachI->QueueMission(Mission::AttackMove, true);
			}
		}
	}

	// Count how many amphibious transports are currently loading on our home
	// island so loaded transports can synchronize and sail together as a D-Day
	// Convoy!
	int homeTransportsStillLoading = 0;
	for (auto const pCheckT : UnitClass::Array)
	{
		if (!pCheckT || pCheckT->Owner != pHouse || pCheckT->InLimbo || !pCheckT->IsAlive || !pCheckT->Type ||
		    !IsAmphibiousOrAirTransport(pCheckT->Type))
			continue;
		const int tz = MapClass::Instance.GetMovementZoneType(pCheckT->GetMapCoords(), MovementZone::Normal, false);
		if (tz == homeGroundZone && pCheckT->Passengers.NumPassengers > 0 && pCheckT->Passengers.NumPassengers < 3 &&
		    pCheckT->Passengers.GetTotalSize() < 6 && !idleCombatUnits.empty())
		{
			homeTransportsStillLoading++;
		}
	}

	for (auto const pTrans : UnitClass::Array)
	{
		if (!pTrans || pTrans->Owner != pHouse || pTrans->InLimbo || !pTrans->IsAlive || !pTrans->Type)
			continue;

		// Subterranean APC / Ground APC Strike Team: If carrying Engineers or >= 3
		// Infantry, burrow/drive to enemy base and Unload!
		if (!IsAmphibiousOrAirTransport(pTrans->Type) && pTrans->Type->Passengers >= 3 && !pTrans->Type->OpenTopped &&
		    !pTrans->Type->Gunner)
		{
			if (pTrans->Passengers.NumPassengers > 0 && pVisibleEnemyBld)
			{
				bool hasEngineerOrFullSquad = (pTrans->Passengers.NumPassengers >= 3);
				for (FootClass* pPass = pTrans->Passengers.GetFirstPassenger(); pPass != nullptr;
				     pPass = abstract_cast<FootClass*>(pPass->NextObject))
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
		bool carryingStolenForeignMCV = false;
		for (FootClass* pPass = pTrans->Passengers.GetFirstPassenger(); pPass != nullptr;
		     pPass = abstract_cast<FootClass*>(pPass->NextObject))
		{
			if (auto const pPassUnit = abstract_cast<UnitClass*>(pPass))
			{
				if (pPassUnit->Type && pPassUnit->Type->DeploysInto != nullptr)
				{
					carryingMCV = true; // MCV or Red Alert 3-style CABAL Expansion Nanocore!
					if (pPassUnit->Type->DeploysInto->ConstructionYard &&
					    pHouse->CountOwnedNow(pPassUnit->Type->DeploysInto) == 0 && pHouse->NumConYards >= 1)
					{
						carryingStolenForeignMCV = true;
					}
					break;
				}
			}
		}

		const bool isReadyToSail = carryingMCV || numPass >= 3 || totalPassSize >= 6;
		const CellStruct transCoords = pTrans->GetMapCoords();
		const int transZone = MapClass::Instance.GetMovementZoneType(transCoords, MovementZone::Normal, false);

		// Slot 82 (Island Extraction of Stolen Foreign MCV to Safe Home Island):
		// 1) If this transport is carrying a stolen foreign-faction MCV from an
		// enemy island, sail straight back to our safe Home Island (homeCoords /
		// homeGroundZone) and unload it there so it can deploy safely!
		if (carryingStolenForeignMCV && homeCoords != CellStruct::Empty && scores[82] > 60)
		{
			if (CellClass* pHomeSafeLanding = FindSafeIslandLandingCell(pHouse, homeCoords, homeGroundZone, true))
			{
				auto const pTransCell = pTrans->GetCell();
				const bool onHomeIslandLand = (homeGroundZone >= 0 && transZone == homeGroundZone && pTransCell &&
				                               pTransCell->LandType != LandType::Water);
				if (onHomeIslandLand && pTrans->DistanceFrom(pHomeSafeLanding) <= 1024)
				{
					if (pTrans->CurrentMission != Mission::Unload)
						pTrans->QueueMission(Mission::Unload, true);
				}
				else if (pTrans->CurrentMission == Mission::Guard || pTrans->CurrentMission == Mission::Area_Guard ||
				         (Unsorted::CurrentFrame % 90 < 25))
				{
					pTrans->SetDestination(pHomeSafeLanding, true);
					pTrans->QueueMission(Mission::Move, true);
				}
				continue;
			}
		}

		// 2) If we have a stolen foreign MCV stranded on an enemy island
		// (pStrandedStolenMCVOnIsland) and this transport is empty, dispatch it to
		// the island beach near the stolen MCV to pick it up!
		if (numPass == 0 && pStrandedStolenMCVOnIsland && scores[82] > 60)
		{
			const CellStruct mcvIslandCell = pStrandedStolenMCVOnIsland->GetMapCoords();
			const int mcvIslandZone =
			    MapClass::Instance.GetMovementZoneType(mcvIslandCell, MovementZone::Normal, false);
			if (CellClass* pRescueBeach = FindSafeIslandLandingCell(pHouse, mcvIslandCell, mcvIslandZone, false))
			{
				if (pTrans->DistanceFrom(pRescueBeach) > 1024 &&
				    (pTrans->CurrentMission == Mission::Guard || pTrans->CurrentMission == Mission::Area_Guard ||
				     (Unsorted::CurrentFrame % 90 < 25)))
				{
					pTrans->SetDestination(pRescueBeach, true);
					pTrans->QueueMission(Mission::Move, true);
				}
				continue;
			}
		}

		const bool waitForConvoySister = (homeTransportsStillLoading > 0 && transZone == homeGroundZone &&
		                                  !carryingMCV && (Unsorted::CurrentFrame % 240 < 150));
		if (isReadyToSail && pIslandGoalCell && !waitForConvoySister)
		{
			CellClass* pSafeLanding =
			    FindSafeIslandLandingCell(pHouse, pIslandGoalCell->MapCoords, targetIslandZone, carryingMCV);
			if (pSafeLanding)
			{
				auto const pTransCell = pTrans->GetCell();
				const bool onTargetIslandLand = (targetIslandZone >= 0 && transZone == targetIslandZone && pTransCell &&
				                                 pTransCell->LandType != LandType::Water);
				if (onTargetIslandLand && pTrans->DistanceFrom(pSafeLanding) <= 1024)
				{
					if (pTrans->CurrentMission != Mission::Unload)
						pTrans->QueueMission(Mission::Unload, true);
				}
				else if (pTrans->CurrentMission == Mission::Guard || pTrans->CurrentMission == Mission::Area_Guard ||
				         (Unsorted::CurrentFrame % 120 < 30))
				{
					pTrans->SetDestination(pSafeLanding, true);
					pTrans->QueueMission(Mission::Move, true);
				}
			}
		}
		else if (!isReadyToSail && (enemyOnDisconnectedIsland || scores[28] > 600 || scores[75] > 100))
		{
			// Slot 75 (Round-Trip Shuttle Ferry Loop): If the transport is empty
			// (numPass == 0) on the enemy island (transZone != homeGroundZone) OR out
			// on the water, immediately sail back to our home beachhead to pick up
			// the next wave of tanks, infantry, or an Expansion MCV!
			auto const pTransCell = pTrans->GetCell();
			const bool notAtHomeShore = (pTransCell && pTransCell->LandType == LandType::Water) ||
			                            (homeGroundZone >= 0 && transZone != homeGroundZone);
			if (notAtHomeShore && numPass == 0 && homeCoords != CellStruct::Empty)
			{
				CellClass* pHomeBeach = FindSafeIslandLandingCell(pHouse, homeCoords, homeGroundZone, false);
				if (pHomeBeach &&
				    (pTrans->CurrentMission == Mission::Guard || pTrans->CurrentMission == Mission::Area_Guard))
				{
					pTrans->SetDestination(pHomeBeach, true);
					pTrans->QueueMission(Mission::Move, true);
				}
				continue;
			}

			// Load nearby idle combat vehicles, Engineers, and infantry into the
			// Amphibious Transport
			int boardedNow = numPass;
			int sizeNow = totalPassSize;
			for (auto const pV : idleCombatUnits)
			{
				const int vehSize = static_cast<int>(pV->Type->TechnoTypeClass::Size);
				if (boardedNow >= pTrans->Type->Passengers || (sizeNow + vehSize) > pTrans->Type->Passengers)
					break;

				if (!IsAmphibiousOrAirTransport(pV->Type) &&
				    pV->Type->TechnoTypeClass::Size <= pTrans->Type->SizeLimit && pV->DistanceFrom(pTrans) <= 4608)
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

	// Dispatch loaded/idle combat Aircraft, Carryall «Matryoshka» Airlift &
	// Emergency Sell-Dodge / ConYard Undeploy
	for (auto const pAir : AircraftClass::Array)
	{
		if (!pAir || pAir->Owner != pHouse || pAir->InLimbo || !pAir->IsAlive || !pAir->Type)
			continue;

		const bool isAirIdle = (pAir->CurrentMission == Mission::Guard || pAir->CurrentMission == Mission::Area_Guard ||
		                        pAir->CurrentMission == Mission::Sleep);

		// Carryall «Matryoshka / Muñeca Rusa» Airlift (Slot 53): Pick up loaded
		// APCs, Expansion Nanocores/MCVs, or rescue critically damaged tanks!
		const int carryallWindow = std::clamp(20 + (static_cast<int>(scores[53]) / 18), 8, 45);
		if (pAir->Type->Carryall && isAirIdle && pAir->Passengers.NumPassengers == 0 &&
		    (Unsorted::CurrentFrame % 90 < carryallWindow))
		{
			for (auto const pU : UnitClass::Array)
			{
				if (!pU || pU->Owner != pHouse || pU->InLimbo || !pU->IsAlive || !pU->Type || pU->Type->Naval)
					continue;

				const bool isCriticallyDamagedTank =
				    (pRepairDepot && pU->Type->Cost >= 1000 && pU->GetHealthPercentage() < 0.40);
				const bool isLoadedStrikeAPC =
				    (pU->Type->Passengers >= 3 && pU->Passengers.NumPassengers >= 2 && pWaveTargetCell);
				if (isCriticallyDamagedTank || isLoadedStrikeAPC)
				{
					pAir->SetTarget(pU);
					pAir->QueueMission(Mission::Capture, true);
					break;
				}
			}
			continue;
		}

		if (pAir->Ammo > 0 && isAirIdle)
		{
			if (pEnemyMindController)
			{
				pAir->SetTarget(pEnemyMindController);
				pAir->QueueMission(Mission::Attack, true);
			}
			else if (pVisibleEnemyHarvester && scores[56] > 200)
			{
				pAir->SetTarget(pVisibleEnemyHarvester);
				pAir->QueueMission(Mission::Attack, true);
			}
			else if (pVisibleEnemyBld)
			{
				pAir->SetTarget(pVisibleEnemyBld);
				pAir->QueueMission(Mission::Attack, true);
			}
		}
	}

	// Dying Garrison Evacuation, Re-Mobilizing Deployed Siege Units (Slot 57),
	// Smart Wrench Building Repair (Slot 59), Emergency Sell-Dodge & ConYard
	// Emergency Undeploy into MCV (Slot 54)!
	if (Unsorted::CurrentFrame % 30 < 10)
	{
		// Evacuate our garrisoned infantry from any civilian/bunker structure that
		// is about to collapse (< 18% HP) so the squad survives!
		for (auto const pBld : BuildingClass::Array)
		{
			if (!pBld || pBld->InLimbo || !pBld->IsAlive || pBld->Occupants.Count == 0)
				continue;
			if (pBld->Occupants.GetItem(0) && pBld->Occupants.GetItem(0)->Owner == pHouse &&
			    pBld->GetHealthPercentage() < 0.18)
			{
				pBld->QueueMission(Mission::Unload, true);
			}
		}

		const double sellDodgeThresh = std::clamp(0.09 + static_cast<double>(scores[54]) * 0.0001, 0.05, 0.16);
		const double undeployThresh = std::clamp(0.14 + static_cast<double>(scores[54]) * 0.00012, 0.08, 0.22);
		const int currentCredits = pHouse->Available_Money();
		const bool isLowPower = pHouse->HasLowPower();
		bool hasPoweredDefense = false;
		if (isLowPower)
		{
			for (auto const pCheckBld : pHouse->Buildings)
			{
				if (pCheckBld && !pCheckBld->InLimbo && pCheckBld->IsAlive && pCheckBld->Type &&
				    pCheckBld->Type->BuildCat == BuildCat::Combat && pCheckBld->Type->Powered &&
				    pCheckBld->StuffEnabled)
				{
					hasPoweredDefense = true;
					break;
				}
			}
		}

		for (auto const pOwnBld : pHouse->Buildings)
		{
			if (!pOwnBld || pOwnBld->InLimbo || !pOwnBld->IsAlive || !pOwnBld->Type || pOwnBld->IsIronCurtained())
				continue;

			// Slot 60 (Emergency Low-Power Defense Prioritization): If our base is in
			// LowPower and we own powered base defenses (Tesla Coils, Prism Towers,
			// Obelisks, SAMs), temporarily toggle offline non-defense high-drain
			// structures (Superweapons, Tech/Radar buildings) via GoOffline() so base
			// defenses stay active! Once power surplus is restored (>= 150), bring
			// offline structures back online via GoOnline().
			if (pOwnBld->Type->TogglePower && (Unsorted::CurrentFrame % 90 < 15))
			{
				if (isLowPower && hasPoweredDefense && scores[60] > 80 && pOwnBld->StuffEnabled &&
				    pOwnBld->Type->BuildCat != BuildCat::Combat && pOwnBld->Type->PowerDrain >= 100 &&
				    (pOwnBld->Type->SuperWeapon != -1 || pOwnBld->Type->Factory == AbstractType::None))
				{
					pOwnBld->GoOffline();
				}
				else if (!isLowPower && !pOwnBld->StuffEnabled && (pHouse->PowerOutput - pHouse->PowerDrain >= 150))
				{
					pOwnBld->GoOnline();
				}
			}

			// Slot 75 (Island Beachhead Primary Factory Switch): Once we construct a
			// War Factory or Barracks on a disconnected island (e.g. from a landed
			// Beachhead MCV), set it as PrimaryFactory via TogglePrimaryFactory() so
			// new tanks and infantry spawn directly on the enemy's island!
			if (enemyOnDisconnectedIsland && !pOwnBld->IsPrimaryFactory &&
			    (pOwnBld->Type->Factory == AbstractType::UnitType ||
			     pOwnBld->Type->Factory == AbstractType::InfantryType) &&
			    !pOwnBld->Type->Naval)
			{
				const int bldZone =
				    MapClass::Instance.GetMovementZoneType(pOwnBld->GetMapCoords(), MovementZone::Normal, false);
				if (bldZone == targetIslandZone)
					pOwnBld->TogglePrimaryFactory();
			}

			const double hpPct = pOwnBld->GetHealthPercentage();

			// Slot 73 (Dynamic Tank Bunker Unload): Eject the bunkered tank if the
			// Tank Bunker is about to collapse (< 22% HP) or if no enemies are near!
			if (pOwnBld->Type->Bunker && pOwnBld->BunkerLinkedItem != nullptr && scores[73] > 60)
			{
				if (hpPct < 0.22)
				{
					pOwnBld->UnloadBunker();
				}
			}
			if (hpPct < undeployThresh && pOwnBld->Type->ConstructionYard && pHouse->NumConYards == 1 &&
			    pOwnBld->Type->UndeploysInto != nullptr)
			{
				// Last ConYard is about to be destroyed! Pack back into an MCV to
				// survive!
				if (pOwnBld->CurrentMission != Mission::Unload)
					pOwnBld->QueueMission(Mission::Unload, true);
			}
			else if (pOwnBld->Type->ConstructionYard && pHouse->NumConYards >= 2 &&
			         pOwnBld != pHouse->ConYards.GetItem(0) && scores[82] > 60 && homeCoords != CellStruct::Empty &&
			         pOwnBld->GetMapCoords().DistanceFrom(homeCoords) > 18.0)
			{
				// Slot 82 (Captured Enemy ConYard Immediate Pack, Extraction OR
				// Emergency Cash-Out Sale): 1) If we just captured an enemy ConYard
				// inside a hostile enemy base
				// (> 18 cells from our home base and enemy structures/units nearby):
				//    - If it CANNOT be extracted alive (e.g., MCVRedeploy=no while
				//    under
				// enemy fire, OR its HP is critically low < 32% with >= 2 enemies
				// attacking so it would die before escaping, OR it is trapped on an
				// enemy island with no Amphibious Transport / Shipyard while under
				// fire), immediately SELL IT (Mission::Selling) just like the classic
				// game to cash in ~$1500 extra credits + survivor infantry!
				//    - Otherwise, when MCVRedeploy=yes and extraction is viable, pack
				//    it
				// into an MCV (Mission::Unload) so our Human Shield escorts and/or
				// Amphibious Transports can bring it home!
				const CellStruct yc = pOwnBld->GetMapCoords();
				bool hostileZone = false;
				int closeHostileAttackers = 0;
				for (auto const pEB : BuildingClass::Array)
				{
					if (!pEB || pEB->InLimbo || !pEB->IsAlive || !pEB->Owner || !pEB->Type || pEB->Type->Immune)
						continue;
					if (pEB->Owner == pHouse || pHouse->IsAlliedWith(pEB->Owner) || pEB->Owner->IsNeutral())
						continue;
					const CellStruct ec = pEB->GetMapCoords();
					const int dB = std::max(std::abs(yc.X - ec.X), std::abs(yc.Y - ec.Y));
					if (dB <= 16)
					{
						hostileZone = true;
						if (pEB->Type->BuildCat == BuildCat::Combat && dB <= 10)
							closeHostileAttackers++;
					}
				}
				for (auto const pET : TechnoClass::Array)
				{
					if (!pET || pET->InLimbo || !pET->IsAlive || !pET->Owner ||
					    pET->WhatAmI() == AbstractType::Building)
						continue;
					if (pET->Owner == pHouse || pHouse->IsAlliedWith(pET->Owner) || pET->Owner->IsNeutral())
						continue;
					if (pOwnBld->DistanceFrom(pET) <= 2560)
					{
						hostileZone = true;
						closeHostileAttackers++;
					}
				}
				if (hostileZone)
				{
					const bool canUndeployToMCV =
					    (GameModeOptionsClass::Instance.MCVRedeploy && pOwnBld->Type->UndeploysInto != nullptr);
					const int yardZone = MapClass::Instance.GetMovementZoneType(yc, MovementZone::Normal, false);
					const bool onEnemyIsland = (homeGroundZone >= 0 && yardZone >= 0 && yardZone != homeGroundZone);
					bool hasAmphibTransportOrNavalYard = (pHouse->NumShipyards > 0);
					if (onEnemyIsland && !hasAmphibTransportOrNavalYard)
					{
						for (auto const pT : UnitClass::Array)
						{
							if (pT && pT->Owner == pHouse && !pT->InLimbo && pT->IsAlive && pT->Type &&
							    IsAmphibiousOrAirTransport(pT->Type))
							{
								hasAmphibTransportOrNavalYard = true;
								break;
							}
						}
					}

					const bool impossibleToExtractAlive =
					    (!canUndeployToMCV && (hpPct < 0.65 || closeHostileAttackers >= 2)) ||
					    (hpPct < 0.32 && closeHostileAttackers >= 2) ||
					    (onEnemyIsland && !hasAmphibTransportOrNavalYard &&
					     (hpPct < 0.60 || closeHostileAttackers >= 1));

					if (impossibleToExtractAlive)
					{
						if (pOwnBld->CurrentMission != Mission::Selling)
							pOwnBld->QueueMission(Mission::Selling, true);
					}
					else if (canUndeployToMCV)
					{
						if (pOwnBld->CurrentMission != Mission::Unload)
							pOwnBld->QueueMission(Mission::Unload, true);
					}
					else if (pOwnBld->Type->ClickRepairable && hpPct < 0.90 && currentCredits > 600 &&
					         !pOwnBld->IsBeingRepaired)
					{
						pOwnBld->SetRepairState(1);
					}
				}
			}
			else if (GameModeOptionsClass::Instance.MCVRedeploy && pOwnBld->Type->ConstructionYard &&
			         pOwnBld->Type->UndeploysInto != nullptr && scores[81] > 90 &&
			         (Unsorted::CurrentFrame % 360 < 15) &&
			         (pHouse->NumConYards >= 2 && pOwnBld != pHouse->ConYards.GetItem(0) ||
			          (pHouse->NumConYards == 1 && pHouse->OwnedBuildings >= 10 && scores[81] > 180)))
			{
				// Slot 81 (Nomadic MCV Hop & Outpost/Turret Drop): Once an expansion
				// ConYard (or a fully-developed main ConYard) has already dropped >= 2
				// structures around itself (Refineries, BaseNormal Outposts, Factories,
				// or Base Defenses), pack it back into a mobile MCV so it can travel to
				// the next Ore field or forward enemy siege position while leaving its
				// dropped structures behind!
				const CellStruct yc = pOwnBld->GetMapCoords();
				int structuresDroppedHere = 0;
				for (auto const pNearB : pHouse->Buildings)
				{
					if (!pNearB || pNearB == pOwnBld || pNearB->InLimbo || !pNearB->IsAlive || !pNearB->Type)
						continue;
					const CellStruct nc = pNearB->GetMapCoords();
					if (std::max(std::abs(yc.X - nc.X), std::abs(yc.Y - nc.Y)) <= 10 &&
					    (pNearB->Type->Refinery || pNearB->Type->BaseNormal ||
					     pNearB->Type->Factory != AbstractType::None || pNearB->Type->BuildCat == BuildCat::Combat))
					{
						structuresDroppedHere++;
					}
				}
				if (structuresDroppedHere >= 2 && pOwnBld->CurrentMission != Mission::Unload)
				{
					pOwnBld->QueueMission(Mission::Unload, true);
				}
			}
			else if (hpPct < sellDodgeThresh && !pOwnBld->Type->ConstructionYard && pOwnBld->Type->Cost >= 500 &&
			         pOwnBld->CurrentMission != Mission::Selling)
			{
				// Classic C&C Sell-Dodge right before destruction!
				pOwnBld->QueueMission(Mission::Selling, true);
			}
			else if (pOwnBld->Type->UndeploysInto != nullptr && !pOwnBld->Type->ConstructionYard &&
			         !pOwnBld->Type->Refinery && !pOwnBld->Type->BaseNormal &&
			         pOwnBld->Type->BuildCat == BuildCat::Combat && pOwnBld->Target == nullptr &&
			         (Unsorted::CurrentFrame % 150 < 30))
			{
				// Slot 57 (Re-Mobilize Deployed Siege Unit): When a deployed Siege
				// Chopper, Tick Tank, or Nod Artillery has finished combat and no
				// enemies are within 12 cells, pack back into a mobile vehicle so it
				// advances with the army!
				bool enemyInRange = false;
				for (auto const pETech : TechnoClass::Array)
				{
					if (!pETech || pETech->InLimbo || !pETech->IsAlive || !pETech->Owner)
						continue;
					if (pETech->Owner == pHouse || pHouse->IsAlliedWith(pETech->Owner) || pETech->Owner->IsNeutral())
						continue;
					if (pOwnBld->DistanceFrom(pETech) <= 3072)
					{
						enemyInRange = true;
						break;
					}
				}
				if (!enemyInRange && pOwnBld->CurrentMission != Mission::Unload)
					pOwnBld->QueueMission(Mission::Unload, true);
			}
			else if (pOwnBld->Type->ClickRepairable)
			{
				// Slot 59 (Smart Wrench Building Repair): Activate engine wrench repair
				// on damaged key structures when credits are healthy, and pause repair
				// if credits drop critically low!
				if (hpPct < 0.82 && hpPct >= sellDodgeThresh && scores[59] > 80 && currentCredits > 1200 &&
				    !pOwnBld->IsBeingRepaired)
				{
					pOwnBld->SetRepairState(1);
				}
				else if (pOwnBld->IsBeingRepaired && currentCredits < 450)
				{
					pOwnBld->SetRepairState(0);
				}
			}
		}
	}

	// CABAL / Faction Research & Tech-Upgrade Unlocks (PreUpgr_SW* /
	// LimboUpgrade_SW* where DisableableFromShell=no, excluding Tunnel Delivery SWs)
	if (homeCoords != CellStruct::Empty && (Unsorted::CurrentFrame % 120 < 25))
	{
		for (auto const pSuper : pHouse->Supers)
		{
			if (!pSuper || !pSuper->IsReady || !pSuper->Type)
				continue;

			if (!pSuper->Type->DisableableFromShell && pSuper->Type->Type != SuperWeaponType::ChronoWarp &&
			    !IsTunnelDeliverySuperWeapon(pSuper->Type, pHouse))
			{
				pSuper->Launch(homeCoords, false);
				break;
			}
		}
	}

	// CABAL / Ares / Antares Subterranean Tunnel Network (ROBOTTUNNEL + CABALTunnelSpecial -> ROBOTCTUNNEL ->
	// ROBOTTUNNEL): 1) Creative Tunnel Exit Placement via Tunnel Delivery SW on unprotected enemy flanks, behind
	// defenses, on islands, or near neutral Techs 2) Two-Way Subterranean Portal Micro: load Infantry, Cyborgs,
	// Engineers & Size<=2 Vehicles at Home Tunnel -> eject at Forward Tunnel!
	if (homeCoords != CellStruct::Empty && (Unsorted::CurrentFrame % 45 < 20))
	{
		BuildingClass* pHomeTunnel = nullptr;
		BuildingClass* pForwardTunnel = nullptr;
		int minHomeTunnelDist = 999999;
		int maxForwardTunnelDist = -1;
		int ownTunnelCount = 0;

		if (auto const pHomeCell = MapClass::Instance.TryGetCellAt(homeCoords))
		{
			for (auto const pOwnBld : pHouse->Buildings)
			{
				if (!pOwnBld || pOwnBld->InLimbo || !pOwnBld->IsAlive || !pOwnBld->Type ||
				    !IsTunnelNetworkBuilding(pOwnBld->Type))
					continue;

				ownTunnelCount++;
				const int dHome = pOwnBld->DistanceFrom(pHomeCell);
				if (dHome < minHomeTunnelDist)
				{
					minHomeTunnelDist = dHome;
					pHomeTunnel = pOwnBld;
				}
				if (dHome > maxForwardTunnelDist)
				{
					maxForwardTunnelDist = dHome;
					pForwardTunnel = pOwnBld;
				}
			}
		}

		// Step 1: If we own 1 Tunnel (or need a 2nd Tunnel endpoint) and our Tunnel Delivery SW (e.g.
		// CABALTunnelSpecial) is ready, dig the remote tunnel endpoint at an unprotected enemy flank, island beachhead,
		// or neutral Tech Building!
		if (ownTunnelCount == 1 && pHomeTunnel)
		{
			for (auto const pSuper : pHouse->Supers)
			{
				if (!pSuper || !pSuper->IsReady || !pSuper->Type || !IsTunnelDeliverySuperWeapon(pSuper->Type, pHouse))
					continue;

				CellStruct bestTunnelCell = CellStruct::Empty;
				int bestTunnelCellScore = -999999;

				// If our single surviving tunnel is far from home (> 18 cells), our Home Tunnel was destroyed ->
				// restore Home Tunnel first!
				if (minHomeTunnelDist > 4608)
				{
					if (CellClass* pHomeRestore = FindSafeIslandLandingCell(pHouse, homeCoords, homeGroundZone, false))
						bestTunnelCell = pHomeRestore->MapCoords;
				}
				else
				{
					// Evaluate visible enemy buildings and neutral capturable Tech Buildings to find an unprotected
					// flank/infiltration spot
					for (auto const pBld : BuildingClass::Array)
					{
						if (!pBld || pBld->InLimbo || !pBld->IsAlive || !pBld->Owner || !pBld->Type ||
						    pBld->Type->Immune)
							continue;

						const bool isNeutralTech = (pBld->Owner->IsNeutral() && pBld->Type->Capturable);
						const bool isEnemyBld =
						    (pBld->Owner != pHouse && !pHouse->IsAlliedWith(pBld->Owner) && !pBld->Owner->IsNeutral() &&
						     !pBld->Owner->IsObserver() && !pBld->Owner->Defeated);
						if (!isNeutralTech && !isEnemyBld)
							continue;

						auto const pBldCell = pBld->GetCell();
						if (!pBldCell || !IsCellVisibleToAI(pHouse, pBldCell))
							continue;

						const CellStruct bc = pBld->GetMapCoords();
						const int distFromHomeCells = std::abs(bc.X - homeCoords.X) + std::abs(bc.Y - homeCoords.Y);
						if (distFromHomeCells < 20)
							continue;

						const int ringMin = isNeutralTech ? 3 : 6;
						const int ringMax = isNeutralTech ? 6 : 12;
						for (int dy = -ringMax; dy <= ringMax; dy += 3)
						{
							for (int dx = -ringMax; dx <= ringMax; dx += 3)
							{
								const int cheb = std::max(std::abs(dx), std::abs(dy));
								if (cheb < ringMin || cheb > ringMax)
									continue;

								const CellStruct tc{static_cast<short>(bc.X + dx), static_cast<short>(bc.Y + dy)};
								auto const pCand = MapClass::Instance.TryGetCellAt(tc);
								if (!pCand || !IsCellVisibleToAI(pHouse, pCand))
									continue;
								if ((pCand->LandType != LandType::Clear && pCand->LandType != LandType::Road) ||
								    pCand->ContainsBridge() || pCand->GetBuilding() != nullptr ||
								    pCand->FirstObject != nullptr)
								{
									continue;
								}

								// Count nearby enemy defensive turrets within 9 cells of this flank candidate
								int nearbyEnemyTurrets = 0;
								for (auto const pOtherBld : BuildingClass::Array)
								{
									if (!pOtherBld || pOtherBld->InLimbo || !pOtherBld->IsAlive || !pOtherBld->Owner ||
									    !pOtherBld->Type)
										continue;
									if (pOtherBld->Owner == pHouse || pHouse->IsAlliedWith(pOtherBld->Owner) ||
									    pOtherBld->Owner->IsNeutral())
										continue;
									if (pOtherBld->Type->BuildCat == BuildCat::Combat &&
									    pOtherBld->Type->GetWeapon(0) != nullptr)
									{
										const CellStruct oc = pOtherBld->GetMapCoords();
										if (std::abs(tc.X - oc.X) <= 9 && std::abs(tc.Y - oc.Y) <= 9)
											nearbyEnemyTurrets++;
									}
								}

								int candScore = 400 - nearbyEnemyTurrets * 260;
								if (nearbyEnemyTurrets == 0)
									candScore += 450; // Completely unprotected flank / rear blindspot!

								if (isNeutralTech)
								{
									candScore += 380 + (scores[36] > 60 ? 200 : 0);
								}
								else
								{
									if (pBld->Type->ConstructionYard || pBld->Type->SuperWeapon != -1)
										candScore += 420;
									else if (pBld->Type->Factory != AbstractType::None || pBld->Type->PowerBonus > 0 ||
									         pBld->Type->Refinery)
										candScore += 280;
								}

								const int candZone =
								    MapClass::Instance.GetMovementZoneType(tc, MovementZone::Normal, false);
								if (homeGroundZone >= 0 && candZone >= 0 && candZone != homeGroundZone)
									candScore += 480; // Disconnected island portal!

								if (candScore > bestTunnelCellScore)
								{
									bestTunnelCellScore = candScore;
									bestTunnelCell = tc;
								}
							}
						}
					}

					// Fallback: if no visible enemy building yet but pWaveTargetCell is far and visible, place near
					// frontline
					if (bestTunnelCell == CellStruct::Empty && pWaveTargetCell)
					{
						const CellStruct wc = pWaveTargetCell->MapCoords;
						if (std::abs(wc.X - homeCoords.X) + std::abs(wc.Y - homeCoords.Y) >= 22)
						{
							if (CellClass* pFrontCell = FindSafeIslandLandingCell(pHouse, wc, targetIslandZone, false))
							{
								if (IsCellVisibleToAI(pHouse, pFrontCell))
									bestTunnelCell = pFrontCell->MapCoords;
							}
						}
					}
				}

				if (bestTunnelCell != CellStruct::Empty)
				{
					pSuper->Launch(bestTunnelCell, false);
					break;
				}
			}
		}

		// Step 2: Active Two-Way Subterranean Tunnel Portal (pHomeTunnel <-> pForwardTunnel)
		if (ownTunnelCount >= 2 && pHomeTunnel && pForwardTunnel && pHomeTunnel != pForwardTunnel &&
		    pHomeTunnel->DistanceFrom(pForwardTunnel) >= 3584)
		{
			// Keep Home Tunnel in Guard so passengers do not unload back at home; set Forward Tunnel to Unload so units
			// emerge on the flank!
			if (pHomeTunnel->CurrentMission == Mission::Unload)
				pHomeTunnel->QueueMission(Mission::Guard, false);
			if (pForwardTunnel->CurrentMission != Mission::Unload)
				pForwardTunnel->QueueMission(Mission::Unload, true);

			// Check if there is a high-value capturable building near pForwardTunnel (within 22 cells = 5632 leptons)
			BuildingClass* pFlankCaptureTarget = nullptr;
			BuildingClass* pFlankAttackTarget = nullptr;
			int bestCapDist = 5632;
			int bestAtkDist = 6144;
			for (auto const pBld : BuildingClass::Array)
			{
				if (!pBld || pBld->InLimbo || !pBld->IsAlive || !pBld->Owner || !pBld->Type || pBld->Type->Immune)
					continue;
				if (pBld->Owner == pHouse || pHouse->IsAlliedWith(pBld->Owner) || pBld->Owner->IsObserver() ||
				    pBld->Owner->Defeated)
					continue;

				const int dFwd = pBld->DistanceFrom(pForwardTunnel);
				if (dFwd <= bestCapDist && pBld->Type->Capturable &&
				    (pBld->Owner->IsNeutral() || pBld->Type->ConstructionYard ||
				     pBld->Type->Factory != AbstractType::None || pBld->Type->Refinery ||
				     pBld->Type->SuperWeapon != -1 || pBld->Type->TechLevel >= 5))
				{
					bestCapDist = dFwd;
					pFlankCaptureTarget = pBld;
				}
				if (!pBld->Owner->IsNeutral() && dFwd <= bestAtkDist)
				{
					bestAtkDist = dFwd;
					pFlankAttackTarget = pBld;
				}
			}

			// Feed Engineers, Cyborgs, Infantry & Size<=SizeLimit Vehicles near pHomeTunnel into the subterranean
			// portal!
			int sentIntoTunnel = 0;
			const int maxBatchEnter = std::clamp(static_cast<int>(pHomeTunnel->Type->Passengers), 4, 10);

			if (pFlankCaptureTarget)
			{
				for (auto const pInf : InfantryClass::Array)
				{
					if (!pInf || pInf->Owner != pHouse || pInf->InLimbo || !pInf->IsAlive || !pInf->Type ||
					    !pInf->Type->Engineer)
						continue;
					// If Engineer emerged from pForwardTunnel, immediately capture the flank target!
					if (pInf->DistanceFrom(pForwardTunnel) <= 2560)
					{
						if (pInf->CurrentMission != Mission::Capture)
						{
							pInf->SetTarget(pFlankCaptureTarget);
							pInf->SetDestination(pFlankCaptureTarget, true);
							pInf->QueueMission(Mission::Capture, true);
						}
					}
					else if (pInf->DistanceFrom(pHomeTunnel) <= 7168 && pInf->CurrentMission != Mission::Enter)
					{
						pInf->SetDestination(pHomeTunnel, true);
						pInf->QueueMission(Mission::Enter, true);
						sentIntoTunnel++;
					}
				}
			}

			for (auto const pI : idleCombatInf)
			{
				if (sentIntoTunnel >= maxBatchEnter)
					break;
				if (pI->DistanceFrom(pForwardTunnel) <= 2304 && pFlankAttackTarget)
				{
					pI->SetTarget(pFlankAttackTarget);
					pI->QueueMission(Mission::Attack, true);
				}
				else if (pI->DistanceFrom(pHomeTunnel) <= 6144 &&
				         pI->DistanceFrom(pForwardTunnel) > pHomeTunnel->DistanceFrom(pForwardTunnel) - 1024)
				{
					pI->SetDestination(pHomeTunnel, true);
					pI->QueueMission(Mission::Enter, true);
					sentIntoTunnel++;
				}
			}

			if (pHomeTunnel->Type->UnitAbsorb)
			{
				const int maxAllowedSize = std::max(1, static_cast<int>(pHomeTunnel->Type->SizeLimit));
				for (auto const pV : idleCombatUnits)
				{
					if (sentIntoTunnel >= maxBatchEnter)
						break;
					if (IsAmphibiousOrAirTransport(pV->Type) ||
					    static_cast<int>(pV->Type->TechnoTypeClass::Size) > maxAllowedSize)
						continue;

					if (pV->DistanceFrom(pForwardTunnel) <= 2304 && pFlankAttackTarget)
					{
						pV->SetTarget(pFlankAttackTarget);
						pV->QueueMission(Mission::Attack, true);
					}
					else if (pV->DistanceFrom(pHomeTunnel) <= 6144 &&
					         pV->DistanceFrom(pForwardTunnel) > pHomeTunnel->DistanceFrom(pForwardTunnel) - 1024)
					{
						pV->SetDestination(pHomeTunnel, true);
						pV->QueueMission(Mission::Enter, true);
						sentIntoTunnel++;
					}
				}
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

				// A) IRON CURTAIN & FORCE SHIELD: Defensive Emergency Save OR Offensive
				// Invulnerable Tank Assault
				if (swType == SuperWeaponType::IronCurtain || swType == SuperWeaponType::ForceShield)
				{
					CellStruct icTargetCell = CellStruct::Empty;

					// Defensive save: check if our own OR an ALLIED ConYard, Superweapon,
					// or War Factory is under heavy attack (< 75% HP)
					for (auto const pFriendlyBld : BuildingClass::Array)
					{
						if (!pFriendlyBld || pFriendlyBld->InLimbo || !pFriendlyBld->IsAlive || !pFriendlyBld->Owner ||
						    !pFriendlyBld->Type || pFriendlyBld->IsIronCurtained())
							continue;
						if (!pHouse->IsAlliedWith(pFriendlyBld->Owner) || pFriendlyBld->Owner->IsNeutral())
							continue;

						const bool isCriticalBld = pFriendlyBld->Type->ConstructionYard ||
						                           pFriendlyBld->Type->Factory == AbstractType::UnitType ||
						                           pFriendlyBld->Type->SuperWeapon != -1;
						if (isCriticalBld && pFriendlyBld->GetHealthPercentage() < 0.75)
						{
							icTargetCell = pFriendlyBld->GetMapCoords();
							break;
						}
					}

					// Offensive Invulnerability (Slot 55): cast Iron Curtain on our own
					// OR an ALLIED attacking tank cluster near enemy
					if (icTargetCell == CellStruct::Empty && swType == SuperWeaponType::IronCurtain)
					{
						const int minCluster = (scores[55] > 120) ? 1 : 2;
						int bestClusterCount = 0;
						for (auto const pU : UnitClass::Array)
						{
							if (!pU || !pU->Owner || !pHouse->IsAlliedWith(pU->Owner) || pU->Owner->IsNeutral() ||
							    pU->InLimbo || !pU->IsAlive || !pU->Type || pU->IsIronCurtained())
								continue;
							if (pU->Type->Harvester || pU->Type->ResourceGatherer || pU->Type->Organic)
								continue;

							const bool isEngagingEnemy =
							    (pU->Target != nullptr) ||
							    (pVisibleEnemyBld && pU->DistanceFrom(pVisibleEnemyBld) <= 4096);
							if (!isEngagingEnemy)
								continue;

							const CellStruct uCell = pU->GetMapCoords();
							int neighbors = 0;
							for (auto const pOtherU : UnitClass::Array)
							{
								if (!pOtherU || !pOtherU->Owner || !pHouse->IsAlliedWith(pOtherU->Owner) ||
								    pOtherU->InLimbo || !pOtherU->IsAlive || !pOtherU->Type)
									continue;
								if (pOtherU->Type->Harvester || pOtherU->Type->Organic)
									continue;
								const CellStruct oCell = pOtherU->GetMapCoords();
								if (std::abs(uCell.X - oCell.X) <= 1 && std::abs(uCell.Y - oCell.Y) <= 1)
									neighbors++;
							}

							if (neighbors >= minCluster && neighbors > bestClusterCount)
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
				//    1) Teleport Iron-Curtained tank squad or Expansion MCV to enemy
				// base / island 2) Teleport Enemy Player A's tanks into Enemy Player
				// B's base to cause mutual attrition in FFA 3) Drown Enemy Ground
				// Tanks/MCVs by teleporting them into Water 4) Destroy Enemy Naval
				// Warships by teleporting them onto dry Land
				if (swType == SuperWeaponType::ChronoSphere && pChronoWarp)
				{
					CellStruct chronoSrc = CellStruct::Empty;
					CellStruct chronoDst = CellStruct::Empty;

					// Find a known water cell (for drowning enemy ground tanks) and a
					// known clear land cell (for beaching enemy ships)
					CellStruct knownWaterCell = CellStruct::Empty;
					CellStruct knownDryLandCell = CellStruct::Empty;
					if (homeCoords != CellStruct::Empty)
					{
						for (int dy = -40; dy <= 40; dy += 4)
						{
							for (int dx = -40; dx <= 40; dx += 4)
							{
								const CellStruct tc{static_cast<short>(homeCoords.X + dx),
								                    static_cast<short>(homeCoords.Y + dy)};
								auto const pC = MapClass::Instance.TryGetCellAt(tc);
								if (!pC || !IsCellVisibleToAI(pHouse, pC) || pC->GetBuilding() != nullptr)
									continue;

								if (knownWaterCell == CellStruct::Empty && pC->LandType == LandType::Water &&
								    !pC->ContainsBridge())
									knownWaterCell = tc;
								if (knownDryLandCell == CellStruct::Empty && pC->LandType == LandType::Clear &&
								    pC->FirstObject == nullptr && std::abs(dx) >= 12)
									knownDryLandCell = tc;
							}
						}
					}

					// Mode 1: Teleport our own Iron-Curtained tank squad or distant
					// Expansion MCV directly to the target beachhead!
					for (auto const pU : UnitClass::Array)
					{
						if (!pU || pU->Owner != pHouse || pU->InLimbo || !pU->IsAlive || !pU->Type)
							continue;

						const bool isExpansionMCV = (pU->Type->DeploysInto && pU->Type->DeploysInto->ConstructionYard &&
						                             pHouse->NumConYards >= 1);
						const bool isIronCurtainedTank =
						    (pU->IsIronCurtained() && !pU->Type->Harvester && !pU->Type->Organic);

						if ((isIronCurtainedTank || isExpansionMCV) && pWaveTargetCell)
						{
							CellClass* pDropCell = FindSafeIslandLandingCell(pHouse, pWaveTargetCell->MapCoords,
							                                                 targetIslandZone, isExpansionMCV);
							if (pDropCell && pU->DistanceFrom(pDropCell) >= 5120)
							{
								chronoSrc = pU->GetMapCoords();
								chronoDst = pDropCell->MapCoords;
								break;
							}
						}
					}

					// Mode 2, 3 & 4: Offensive Chronoshift against visible enemy
					// vehicles/ships
					if (chronoSrc == CellStruct::Empty)
					{
						// Check if there is a SECOND rival enemy house with a visible
						// building (for Enemy-vs-Enemy FFA attrition!)
						CellStruct rivalEnemyBaseCell = CellStruct::Empty;
						int rivalEnemyHouseIdx = -1;
						for (auto const pBld : BuildingClass::Array)
						{
							if (!pBld || pBld->InLimbo || !pBld->IsAlive || !pBld->Owner || !pBld->Type ||
							    pBld->Type->Immune)
								continue;
							auto const pOwn = pBld->Owner;
							if (pOwn == pHouse || pHouse->IsAlliedWith(pOwn) || pOwn->IsNeutral() ||
							    pOwn->IsObserver() || pOwn->Defeated)
								continue;
							if (pOwn->ArrayIndex != pHouse->EnemyHouseIndex && pBld->GetCell() &&
							    IsCellVisibleToAI(pHouse, pBld->GetCell()))
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
							if (pEnemyU->Owner == pHouse || pHouse->IsAlliedWith(pEnemyU->Owner) ||
							    pEnemyU->Owner->IsNeutral())
								continue;
							if (pEnemyU->IsIronCurtained() || pEnemyU->Type->Teleporter || pEnemyU->Type->Organic)
								continue;

							auto const pECell = pEnemyU->GetCell();
							if (!pECell || !IsCellVisibleToAI(pHouse, pECell))
								continue;

							const bool isPureGroundUnit = !pEnemyU->Type->Naval &&
							                              pEnemyU->Type->SpeedType != SpeedType::Hover &&
							                              pEnemyU->Type->MovementZone != MovementZone::Amphibious;
							const bool isPureNavalShip = pEnemyU->Type->Naval &&
							                             pEnemyU->Type->SpeedType != SpeedType::Hover &&
							                             pEnemyU->Type->MovementZone != MovementZone::Amphibious;

							// Mode 2: Pit Enemy A's ground tanks directly inside Enemy B's
							// base to trigger mutual attrition!
							if (isPureGroundUnit && rivalEnemyBaseCell != CellStruct::Empty &&
							    pEnemyU->Owner->ArrayIndex != rivalEnemyHouseIdx)
							{
								chronoSrc = pEnemyU->GetMapCoords();
								chronoDst = rivalEnemyBaseCell;
								break;
							}
							// Mode 3 (Slot 55): Drown Enemy ground tanks/MCVs in water!
							const int drownMinCost = std::clamp(700 - static_cast<int>(scores[55]), 450, 950);
							if (isPureGroundUnit && knownWaterCell != CellStruct::Empty &&
							    pEnemyU->Type->Cost >= drownMinCost)
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
						if (!pEnemyInf || pEnemyInf->InLimbo || !pEnemyInf->IsAlive || !pEnemyInf->Owner ||
						    !pEnemyInf->Type)
							continue;
						if (pEnemyInf->Owner == pHouse || pHouse->IsAlliedWith(pEnemyInf->Owner) ||
						    pEnemyInf->Owner->IsNeutral())
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

				// D) YURI PSYCHIC DOMINATOR: Permanently capture clusters of expensive
				// enemy tanks/MCVs while crushing nearby structures!
				if (swType == SuperWeaponType::PsychicDominator)
				{
					CellStruct bestDomCell = CellStruct::Empty;
					int bestDomValue = 1200;
					for (auto const pEnemyU : UnitClass::Array)
					{
						if (!pEnemyU || pEnemyU->InLimbo || !pEnemyU->IsAlive || !pEnemyU->Owner || !pEnemyU->Type)
							continue;
						if (pEnemyU->Owner == pHouse || pHouse->IsAlliedWith(pEnemyU->Owner) ||
						    pEnemyU->Owner->IsNeutral())
							continue;
						if (pEnemyU->GetCell() && IsCellVisibleToAI(pHouse, pEnemyU->GetCell()) &&
						    !pEnemyU->Type->ImmuneToPsionics)
						{
							const CellStruct uc = pEnemyU->GetMapCoords();
							int val = 0;
							for (auto const pOtherU : UnitClass::Array)
							{
								if (!pOtherU || pOtherU->InLimbo || !pOtherU->IsAlive || !pOtherU->Owner ||
								    !pOtherU->Type)
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

				// E) Reconnaissance Superweapons (Psychic Reveal / Spy Plane): Uncover
				// shrouded enemy base candidates!
				if ((swType == SuperWeaponType::PsychicReveal || swType == SuperWeaponType::SpyPlane) &&
				    pFallbackEnemyCell)
				{
					pSuper->Launch(pFallbackEnemyCell->MapCoords, false);
					break;
				}

				// F) Standard Offensive/Support Superweapons (Nuke, Lightning Storm,
				// Ion Cannon, ParaDrop, etc.)
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
// - Requires valid human TechLevel (0..10), excluding TechLevel=-1 AI script
// hacks/limbo dummies and TechLevel=11 disabled types
// - Excludes legacy *_AI hack clones and TEST_BUILDING
// - Temporarily sets IsHumanPlayer=true and AIBuildThis=true during
// HouseClass::CanBuild so Ares/YRpp enforce real human Prerequisites,
// Prerequisite.Negative, Owner/ForbiddenHouses, BuildLimit, and
// PowersUpBuilding instead of AIBuildThis/PrerequisiteOverride
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
				if (!IsTunnelNetworkBuilding(pBld))
				{
					if (auto const pSWExt = SWTypeExt::Fetch(pSW))
					{
						if (!pSWExt->SW_AllowAI)
							return false; // Respects SW01_Computers.ini .. SW14_Computers.ini
							              // (SW.AllowAI=no)
					}
				}
			}
		}
	}

	// Evaluate full Phobos/Ares human prerequisites: [GenericPrerequisites],
	// Prerequisite<Name>Alternate, Prerequisite.Lists, Prerequisite.Negative,
	// Owner/RequiredHouses/ForbiddenHouses (with ParentCountry),
	// ConsideredSecretLabTech, BuildLimit/BuildLimitGroup, and PowersUpBuilding /
	// PowersUp.Buildings parent upgrade slots.
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
} // namespace

int NeuralAI::UpdateExpertAI(HouseClass* pHouse)
{
	if (!NeuralAI::IsEnabledForHouse(pHouse))
		return 30;

	if (!NeuralAI::IsCampaignRestricted(pHouse))
	{
		pHouse->Production = true;
		pHouse->AIState_1E4 = 0;
	}

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
	const int rawParentIdx = pHouse->Type ? pHouse->Type->FindParentCountryIndex() : -1;
	const int parentCountryIdx = (rawParentIdx >= 0) ? rawParentIdx : (pHouse->Type ? pHouse->Type->ArrayIndex2 : -1);
	const unsigned int ownHouseBit = (parentCountryIdx >= 0 && parentCountryIdx < 32) ? (1u << parentCountryIdx) : 0u;

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

		// Count current owned macro & defense structures so the AI builds a
		// balanced macro base (Power -> Refinery -> Barracks -> War Factory -> 2nd
		// Refinery -> Radar -> Battle Lab -> Superweapons/High-Tier Defenses) and
		// NEVER spams an endless sea of cheap Tier-1 Pillboxes/Turrets!
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
			else if (pOT->BuildCat == BuildCat::Combat && !IsTunnelNetworkBuilding(pOT))
			{
				const auto pWS = pOT->GetWeapon(0);
				const auto pW = pWS ? pWS->WeaponType : nullptr;
				const bool isPureAA = pW && pW->Projectile && pW->Projectile->AA && !pW->Projectile->AG;
				if (isPureAA)
					totalAADefenses++;
				else
					totalGroundDefenses++;
			}
			else if (pOT->SpySat || pOT->OrePurifier ||
			         (pOT->PowerBonus <= pOT->PowerDrain && !pOT->ConstructionYard && pOT->Storage == 0 &&
			          !pOT->UnitRepair))
			{
				ownedTechStructures++;
			}
		}

		// Also count mobile Slave Miners (e.g. Yuri's SMIN when undeployed from
		// YAREFN to harvest ore) as owned Refineries
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
		const int defDensityMod = std::clamp(static_cast<int>(scores[14]) / 240, -1, 2);
		const int maxGroundDefenses =
		    std::clamp(1 + ownedRefineries + ownedWarFactories + ownedTechStructures + defDensityMod, 2, 8);
		const int maxAADefenses = std::clamp(1 + ownedWarFactories + ownedTechStructures + defDensityMod, 2, 6);

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
				// Slot 82 (Cross-Faction Tech Tree): If we already own our native
				// Barracks (ownedBarracks >= 1) and a captured foreign ConYard unlocks
				// a brand-new Foreign Barracks (owned == 0), boost it so we unlock
				// foreign infantry and foreign tech prerequisites!
				if (owned == 0 && ownedBarracks >= 1 && ownedBarracks < 5 && scores[82] > 60)
				{
					score = std::max(static_cast<int>(scores[2]), 350) + 750 +
					        std::clamp(static_cast<int>(scores[82]), 0, 500);
				}
				else
				{
					score = scores[2] - owned * 1100;
				}
			}
			else if (pBld->Factory == AbstractType::UnitType && !pBld->Naval)
			{
				if (owned >= 3)
					continue;
				// Slot 82 (Cross-Faction Tech Tree): If we already own our native War
				// Factory (ownedWarFactories >= 1) and a captured foreign ConYard
				// unlocks a brand-new Foreign War Factory (owned == 0), build it to
				// unlock foreign tanks, Radar, and Battle Lab!
				if (owned == 0 && ownedWarFactories >= 1 && ownedWarFactories < 5 && scores[82] > 60)
				{
					score = std::max(static_cast<int>(scores[3]), 400) + 850 +
					        std::clamp(static_cast<int>(scores[82]), 0, 550);
				}
				else
				{
					score = scores[3] - owned * 750;
				}
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
				if (owned == 0 && pHouse->NumAirpads >= 1 && pHouse->NumAirpads < 4 && scores[82] > 60)
				{
					score = std::max(static_cast<int>(scores[8]), 250) + 550 +
					        std::clamp(static_cast<int>(scores[82]) / 2, 0, 350);
				}
				else
				{
					score = scores[8] - owned * 850;
				}
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
				if (pHouse->NumConYards >= 2 && scores[82] > 60)
					score += std::clamp(static_cast<int>(scores[82]) / 2, 0, 350);
			}
			else if (pBld->Storage > 0 && !isRefineryOrSlaveMiner)
			{
				if (owned >= 2)
					continue;
				score = scores[11] - owned * 900;
			}
			else if (IsTunnelNetworkBuilding(pBld))
			{
				if (!hasCoreEconomyAndProduction || owned >= 1)
					continue;
				score = 680 + std::max({static_cast<int>(scores[28]), static_cast<int>(scores[36]),
				                        static_cast<int>(scores[61]), static_cast<int>(scores[83])});
			}
			else if (pBld->BuildCat == BuildCat::Combat)
			{
				// Never build turrets before establishing core Economy + Barracks + War
				// Factory
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

				// Scale defense score by Cost and TechLevel so once Radar/Battle Lab
				// are online, advanced Tier-2/3 defenses (Prism Tower, Tesla Coil,
				// Psychic Tower, SAM, Patriot) strongly outscore cheap $500 Tier-1
				// Pillboxes/Sentry Guns!
				const int tierQualityBonus = std::min(850, (pBld->Cost / 2) + std::max(0, pBld->TechLevel) * 140);
				score = (isPureAA ? scores[6] : scores[5]) + tierQualityBonus;

				if (isPureAA)
					score -= totalAADefenses * 420 + owned * 500;
				else
					score -= totalGroundDefenses * 480 + owned * 600;

				if (features[72] > 0 && pBld->Sensors)
					score += 350; // Anti-Stealth / Submarine detection turret boost!
			}
			else
			{
				// Tech buildings (Radar, Battle Lab, Ore Purifier, Industrial Plant,
				// Cloning Vats, Grinder): Always build 1 of each unlocked tech
				// structure, never spam duplicates.
				if (owned >= 1)
					continue;
				score = scores[4] + 550 + std::min(600, pBld->Cost / 3);
				if (pHouse->NumConYards >= 2 && scores[82] > 60)
					score += 250 + std::clamp(static_cast<int>(scores[82]) / 2, 0, 300);
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
			else if (pOwnedType->DeploysInto && !pOwnedType->Harvester && !pOwnedType->ResourceGatherer &&
			         (pOwnedType->DeploysInto->BuildCat != BuildCat::Combat || !pOwnedType->GetWeapon(0) ||
			          !pOwnedType->GetWeapon(0)->WeaponType))
			{
				ownsUndeployedKeyStructure = true;
			}
			else if (IsAmphibiousOrAirTransport(pOwnedType))
			{
				ownedAmphibTransports++;
			}
		}
	}

	// Economy capacity: true refineries plus deployed miners define how many
	// harvesting units are useful
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
		if (pEcoType->Harvester || pEcoType->ResourceGatherer ||
		    (pEcoType->DeploysInto && (pEcoType->DeploysInto->Refinery || pEcoType->DeploysInto->ResourceGatherer)))
		{
			ownedEconomyUnits++;
		}
	}

	const int ecoBiasMod = std::clamp(static_cast<int>(scores[39]) / 220, -1, 2);
	const int economyUnitCap = (trueRefineries > 0) ? std::clamp(2 * trueRefineries + 1 + ecoBiasMod, 2, 9) : 4;

	// Emergency ConYard Reconstruction: If we lost all ConYards and have no MCV
	// on the map, check if our War Factory + Service Depot/Grinder/Tech can
	// build a replacement MCV and whether our economy/situation allows saving up
	// the $3000 for it instead of draining credits on cheap units!
	const bool canSafelySaveForReplacementMCV = (pHouse->NumConYards == 0 && !ownsUndeployedMCV) &&
	                                            ((ownedEconomyUnits > 0 && trueRefineries > 0 && features[65] < 45) ||
	                                             money >= 1500 || pHouse->OwnedUnits >= 3);
	bool canRebuildMCVNow = false;
	int rebuildMCVCost = 3000;

	for (auto const pUnit : UnitTypeClass::Array)
	{
		if (!pUnit || !CanBuildUnderHumanRules(pHouse, pUnit))
			continue;

		const bool isMCV = (pUnit->DeploysInto && pUnit->DeploysInto->ConstructionYard);
		if (isMCV)
		{
			if (ownsUndeployedMCV || (pHouse->NumConYards >= 1 && scores[30] < 350))
				continue;
			if (pHouse->NumConYards == 0 && !ownsUndeployedMCV)
			{
				canRebuildMCVNow = true;
				rebuildMCVCost = pUnit->GetActualCost(pHouse);
			}
		}

		if (pUnit->GetActualCost(pHouse) > money + 800 && !(isMCV && canSafelySaveForReplacementMCV))
		{
			continue;
		}

		const auto pWpnStruct = pUnit->GetWeapon(0);
		const auto pWpn = pWpnStruct ? pWpnStruct->WeaponType : nullptr;
		const bool isKeyStructureDeployer =
		    (pUnit->DeploysInto != nullptr && !isMCV && !pUnit->Harvester && !pUnit->ResourceGatherer &&
		     (pUnit->DeploysInto->BuildCat != BuildCat::Combat || pWpn == nullptr));
		if (isKeyStructureDeployer && ownsUndeployedKeyStructure)
			continue; // Deploy the current Mobile Outpost / Key Structure Core before
			          // queuing another one

		const bool isAmphibTransport = IsAmphibiousOrAirTransport(pUnit);

		// Economy units (harvesters and deployable miners) stop being built once
		// the real refinery capacity is covered
		const bool isEconomyUnit =
		    pUnit->Harvester || pUnit->ResourceGatherer ||
		    (pUnit->DeploysInto && (pUnit->DeploysInto->Refinery || pUnit->DeploysInto->ResourceGatherer));

		if (isEconomyUnit && ownedEconomyUnits >= economyUnitCap)
			continue;
		int score = (pUnit->Cost / 4) + (pUnit->Strength / 3);

		// Penalize stacking one unit type so armies stay diverse
		score -= std::min(900, pHouse->CountOwnedNow(pUnit) * 45);
		if (pUnit->Naval)
		{
			if (isAmphibTransport)
			{
				const int maxTransports = (features[21] > 0 && scores[75] > 120) ? 3 : 2;
				if (ownedAmphibTransports >= maxTransports)
					score -= 1200;
				else
					score += scores[28] + ((features[21] > 0 && ownedAmphibTransports == 0) ? 900 : 200);
			}
			else
			{
				score += scores[29];
			}

			if (features[72] > 0 && pUnit->Sensors)
				score += 400; // Anti-Submarine / Sonar detection ship boost!

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
				score += (pHouse->NumConYards == 0) ? 12000 : scores[30];
			else if (isKeyStructureDeployer)
			{
				// Universal Deployable Key Structure Scoring across all mods (Mobile
				// Outposts/Explorers, Mobile Refineries/Factories, Power/Tech/Turret
				// Cores, Nanocores):
				auto const pTargetBld = pUnit->DeploysInto;
				const int ownedTargetBld = BuildingTypeExt::CountOwnedNowWithDeployOrUpgrade(pTargetBld, pHouse);
				const bool isDedicatedOutpost =
				    (pTargetBld->BaseNormal && !pTargetBld->Refinery && pTargetBld->Factory == AbstractType::None &&
				     pTargetBld->PowerBonus <= pTargetBld->PowerDrain);

				if (isDedicatedOutpost)
				{
					// Mobile Outpost / Explorer / Expansion Hub (Tiberian Sun / Firestorm
					// / Mental Omega / YR mods):
					if (scores[30] < 300 || ownedTargetBld >= 2)
						score -= 2000;
					else
						score += scores[30] - (ownedTargetBld * 600);
				}
				else if (pTargetBld->PowerBonus > pTargetBld->PowerDrain && pTargetBld->BuildCat != BuildCat::Combat)
					score += scores[0];
				else if (pTargetBld->Refinery)
					score += scores[1] + ((scores[30] > 700) ? (scores[30] / 2) : 0); // Use Mobile Refinery / Refinery
					                                                                  // Core for Outpost Expansion!
				else if (pTargetBld->Factory == AbstractType::InfantryType)
					score += scores[2];
				else if (pTargetBld->Factory == AbstractType::UnitType && !pTargetBld->Naval)
					score +=
					    scores[3] + ((scores[30] > 800) ? (scores[30] / 3) : 0); // Forward Mobile War Factory / Core!
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

			if (features[77] > 0 && pUnit->ImmuneToPsionics && !isEconomyUnit && !isMCV)
				score += 420; // Counter enemy MindControl with ImmuneToPsionics
				              // vehicles (Drones, Robot Tanks, Automated units)!
			if (features[72] > 0 && (pUnit->Sensors || pUnit->DetectDisguise) && !isEconomyUnit)
				score += 480; // Counter enemy Stealth / Disguised units with mobile Sensor & Detector units!
			if (scores[82] > 60 && ownHouseBit != 0u && !pUnit->InOwners(ownHouseBit) && !isEconomyUnit && !isMCV)
			{
				// Slot 82 (Cross-Faction Arsenal Synergy): Mix newly unlocked
				// foreign-faction tanks/vehicles into our strike force!
				score += 240 + std::clamp(static_cast<int>(scores[82]) / 3, 0, 220);
			}

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

	bool hasCapturableTechOnMap = false;
	for (auto const pMapBld : BuildingClass::Array)
	{
		if (pMapBld && !pMapBld->InLimbo && pMapBld->IsAlive && pMapBld->Type && pMapBld->Owner &&
		    pMapBld->Type->Capturable && pMapBld->Owner->IsNeutral())
		{
			hasCapturableTechOnMap = true;
			break;
		}
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
			if (ownedEngineers >= 3)
				continue;

			const int techBonus =
			    (hasCapturableTechOnMap && ownedEngineers == 0) ? 700 : (hasCapturableTechOnMap ? 300 : 0);
			if (pHouse->OwnedInfantry < 3 && !hasCapturableTechOnMap && scores[36] <= 80)
				score -= 300;
			else
				score += scores[18] + techBonus;
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

		if (features[77] > 0 && !pInf->Engineer && !pInf->Infiltrate &&
		    (pInf->ImmuneToPsionics || (pWpn && pWpn->Range >= (6 * 256))))
			score += 380; // Counter enemy MindControl with Snipers/Virus/Cyborgs/Dogs!
		if (features[72] > 0 && (pInf->Sensors || pInf->DetectDisguise || pInf->Doggie))
			score += 420;
		if (scores[82] > 60 && ownHouseBit != 0u && !pInf->InOwners(ownHouseBit) && !pInf->Engineer)
		{
			score += 220 + std::clamp(static_cast<int>(scores[82]) / 3, 0, 200);
		}

		if (score > bestInfScore)
		{
			bestInfScore = score;
			bestInfIdx = pInf->ArrayIndex;
		}
	}

	if (canRebuildMCVNow && canSafelySaveForReplacementMCV && money < rebuildMCVCost && features[65] < 35 &&
	    pHouse->OwnedInfantry >= 3)
	{
		bestInfIdx = -1; // Save credits to finish the replacement MCV first!
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
			const int crossFactionAirBonus = (scores[82] > 60 && ownHouseBit != 0u && !pAir->InOwners(ownHouseBit))
			                                     ? (220 + std::clamp(static_cast<int>(scores[82]) / 3, 0, 200))
			                                     : 0;
			const int score = (pAir->Cost / 3) + (pAir->Strength / 2) + slotScore + crossFactionAirBonus;
			if (score > bestAirScore)
			{
				bestAirScore = score;
				bestAirIdx = pAir->ArrayIndex;
			}
		}
	}

	if (canRebuildMCVNow && canSafelySaveForReplacementMCV && money < rebuildMCVCost)
		bestAirIdx = -1; // Save credits to finish the replacement MCV first!
	pExt->AIBrain_NextAircraftTarget = bestAirIdx;

	if (Unsorted::CurrentFrame % 450 < evalDelay)
	{
		const int bldIdx = pExt->AIBrain_NextBuildingTarget;
		const char* bldName = (bldIdx >= 0 && bldIdx < BuildingTypeClass::Array.Count)
		                          ? BuildingTypeClass::Array.GetItem(bldIdx)->ID
		                          : "None";
		const char* unitName = (bestUnitIdx >= 0 && bestUnitIdx < UnitTypeClass::Array.Count)
		                           ? UnitTypeClass::Array.GetItem(bestUnitIdx)->ID
		                           : "None";
		const char* infName = (bestInfIdx >= 0 && bestInfIdx < InfantryTypeClass::Array.Count)
		                          ? InfantryTypeClass::Array.GetItem(bestInfIdx)->ID
		                          : "None";
		const char* airName = (bestAirIdx >= 0 && bestAirIdx < AircraftTypeClass::Array.Count)
		                          ? AircraftTypeClass::Array.GetItem(bestAirIdx)->ID
		                          : "None";
		Debug::Log("[NeuralAI] Frame %d | House #%d (%s) | Expert=%d Arch=%d | Credits=%d "
		           "| NextBld=%s | NextUnit=%s | NextInf=%s | NextAir=%s\n",
		           Unsorted::CurrentFrame, pHouse->ArrayIndex,
		           (pHouse->Type && pHouse->Type->ID[0]) ? pHouse->Type->ID : "AI", pExt->AIBrain_ActiveExpert,
		           pExt->AIBrain_Archetype, money, bldName, unitName, infName, airName);
	}

	return evalDelay;
}

void NeuralAI::UpdateBuildingProduction(HouseClass* pHouse)
{
	auto const pExt = HouseExt::Fetch(pHouse);
	if (Unsorted::CurrentFrame - pExt->AIBrain_LastInferenceFrame > 60)
		NeuralAI::UpdateExpertAI(pHouse);

	pHouse->ProducingBuildingTypeIndex = pExt->AIBrain_NextBuildingTarget;
}

void NeuralAI::UpdateUnitProduction(HouseClass* pHouse)
{
	auto const pExt = HouseExt::Fetch(pHouse);
	if (Unsorted::CurrentFrame - pExt->AIBrain_LastInferenceFrame > 60)
		NeuralAI::UpdateExpertAI(pHouse);

	pHouse->ProducingUnitTypeIndex = pExt->AIBrain_NextUnitTarget;
	pExt->ProducingNavalUnitTypeIndex = pExt->AIBrain_NextNavalTarget;
}

void NeuralAI::UpdateInfantryProduction(HouseClass* pHouse)
{
	auto const pExt = HouseExt::Fetch(pHouse);
	if (Unsorted::CurrentFrame - pExt->AIBrain_LastInferenceFrame > 60)
		NeuralAI::UpdateExpertAI(pHouse);

	pHouse->ProducingInfantryTypeIndex = pExt->AIBrain_NextInfantryTarget;
}

void NeuralAI::UpdateAircraftProduction(HouseClass* pHouse)
{
	auto const pExt = HouseExt::Fetch(pHouse);
	if (Unsorted::CurrentFrame - pExt->AIBrain_LastInferenceFrame > 60)
		NeuralAI::UpdateExpertAI(pHouse);

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

		// Reserve 3-cell exit corridor south and east of Factories, Barracks,
		// Refineries, and Repair Depots
		const bool needsExit = (pExType->Factory != AbstractType::None) || pExType->Refinery || pExType->UnitRepair ||
		                       pExType->WeaponsFactory;
		const int marginSouth = needsExit ? 3 : 1;
		const int marginEast = needsExit ? 3 : 1;

		// Enforce minimum 4-cell spacing between Base Defense turrets so the AI
		// spreads defenses along the perimeter instead of clumping
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
	const bool isRefineryOrSlaveMiner =
	    pBuildingType->Refinery || pBuildingType->ResourceGatherer || pBuildingType->ResourceDestination;
	if (pHouse->ConYards.Count > 0)
	{
		// If we own an expansion ConYard (especially an Island Beachhead ConYard!),
		// allow Refineries, Defenses, War Factories, and Barracks to build around
		// it!
		const bool canBuildAtOutpostYard = isRefineryOrSlaveMiner || pBuildingType->BuildCat == BuildCat::Combat ||
		                                   pBuildingType->Factory == AbstractType::UnitType ||
		                                   pBuildingType->Factory == AbstractType::InfantryType;
		const int yardIdx = (pHouse->ConYards.Count > 1 && canBuildAtOutpostYard) ? (pHouse->ConYards.Count - 1) : 0;
		center = pHouse->ConYards.GetItem(yardIdx)->GetMapCoords();
	}
	else if (pHouse->Buildings.Count > 0)
	{
		// When all ConYards are lost (ConYards.Count == 0), anchor around our
		// safest surviving War Factory, Refinery, or Power Plant that is not under
		// enemy fire!
		for (auto const pOwnBld : pHouse->Buildings)
		{
			if (!pOwnBld || pOwnBld->InLimbo || !pOwnBld->IsAlive || !pOwnBld->Type)
				continue;
			if (pOwnBld->Type->Factory == AbstractType::UnitType || pOwnBld->Type->Refinery ||
			    pOwnBld->Type->BaseNormal || pOwnBld->Type->PowerBonus > 0)
			{
				center = pOwnBld->GetMapCoords();
				if (!pOwnBld->HasBeenAttacked)
					break;
			}
		}
	}

	// Support universal Mobile Outpost / Expansion Hub structures (BaseNormal=yes
	// across all mods) for building Refineries & Defenses at outposts!
	if (isRefineryOrSlaveMiner || pBuildingType->BuildCat == BuildCat::Combat)
	{
		for (auto const pOwnBld : pHouse->Buildings)
		{
			if (pOwnBld && !pOwnBld->InLimbo && pOwnBld->IsAlive && pOwnBld->Type && pOwnBld->Type->BaseNormal &&
			    !pOwnBld->Type->ConstructionYard && !pOwnBld->Type->Refinery)
				center = pOwnBld->GetMapCoords();
		}
	}

	// Allied Base-Sharing (BuildOffAlly=yes): If we lost our ConYard OR an Ally's
	// ConYard is under attack (< 80% HP) while placing a Base Defense, build
	// around our Ally's ConYard!
	if (GameModeOptionsClass::Instance.BuildOffAlly &&
	    (pHouse->ConYards.Count == 0 || pBuildingType->BuildCat == BuildCat::Combat))
	{
		for (auto const pAllyBld : BuildingClass::Array)
		{
			if (!pAllyBld || pAllyBld->InLimbo || !pAllyBld->IsAlive || !pAllyBld->Owner || !pAllyBld->Type)
				continue;
			if (pAllyBld->Owner != pHouse && pHouse->IsAlliedWith(pAllyBld->Owner) && !pAllyBld->Owner->IsNeutral() &&
			    pAllyBld->Type->EligibileForAllyBuilding)
			{
				if (pHouse->ConYards.Count == 0 ||
				    (pBuildingType->BuildCat == BuildCat::Combat && pAllyBld->HasBeenAttacked &&
				     pAllyBld->GetHealthPercentage() < 0.80))
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

	auto const pExt = HouseExt::Fetch(pHouse);
	const AIBackpackFile& policy = NeuralAI::PolicyFor(pHouse);
	const int crawlBias =
	    static_cast<int>(policy.OutputBias[34]) + ((pExt && pExt->AIBrain_ActiveExpert == 6) ? 220 : 0);
	const int turretCreepBias =
	    static_cast<int>(policy.OutputBias[38]) + ((pExt && pExt->AIBrain_ActiveExpert == 7) ? 220 : 0);

	// Discover nearest Ore/Tiberium patch across a wide 44-cell radius (for
	// Expert 6 & Slot 34: Resource Base-Crawling). When we already own >= 1
	// Refinery, prefer uncolonized Ore patches >= 8 cells from existing
	// Refineries so each new Refinery crawls further along the Ore field!
	CellStruct nearestOreCell = CellStruct::Empty;
	CellStruct fallbackOreCell = CellStruct::Empty;
	int bestOreDist = 9999;
	int bestFallbackOreDist = 9999;
	for (int dy = -44; dy <= 44; dy += 2)
	{
		for (int dx = -44; dx <= 44; dx += 2)
		{
			const int d = std::abs(dx) + std::abs(dy);
			if (d < 4)
				continue;

			const CellStruct tc{static_cast<short>(center.X + dx), static_cast<short>(center.Y + dy)};
			auto const pCell = MapClass::Instance.TryGetCellAt(tc);
			if (!pCell || pCell->GetContainedTiberiumValue() <= 0)
				continue;

			if (d < bestFallbackOreDist)
			{
				bestFallbackOreDist = d;
				fallbackOreCell = tc;
			}

			if (d < bestOreDist && pHouse->CountResourceDestinations >= 1)
			{
				bool nearExistingRef = false;
				for (auto const pOwnBld : pHouse->Buildings)
				{
					if (!pOwnBld || pOwnBld->InLimbo || !pOwnBld->IsAlive || !pOwnBld->Type)
						continue;
					if (pOwnBld->Type->Refinery || pOwnBld->Type->ResourceGatherer ||
					    pOwnBld->Type->ResourceDestination)
					{
						const CellStruct bc = pOwnBld->GetMapCoords();
						if (std::max(std::abs(tc.X - bc.X), std::abs(tc.Y - bc.Y)) < 8)
						{
							nearExistingRef = true;
							break;
						}
					}
				}
				if (!nearExistingRef)
				{
					bestOreDist = d;
					nearestOreCell = tc;
				}
			}
			else if (d < bestOreDist)
			{
				bestOreDist = d;
				nearestOreCell = tc;
			}
		}
	}
	if (nearestOreCell == CellStruct::Empty)
		nearestOreCell = fallbackOreCell;

	// Discover primary Enemy Threat Vector (for Expert 7 & Slot 38: Forward
	// Defense Crawling & Rear Tech Shielding)
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
			if (!pOther || pOther == pHouse || pHouse->IsAlliedWith(pOther) || pOther->IsNeutral() ||
			    pOther->IsObserver() || pOther->Defeated)
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
	const bool isFactoryOrBarracks =
	    (pBuildingType->Factory != AbstractType::None) || pBuildingType->WeaponsFactory || pBuildingType->UnitRepair;
	const bool isRearProtected = !isResourceBld && !isCombatDefense && !isFactoryOrBarracks;

	// True C&C Base-Crawling Chain & Forward Turret Creep: Instead of always
	// anchoring at ConYards[0] (which forms a static circle around spawn), anchor
	// at our most forward owned structure toward the Ore field or Enemy base so
	// buildings chain across the map!
	const bool enableBaseCrawl =
	    (crawlBias > 30 && (isResourceBld || isFactoryOrBarracks)) || (turretCreepBias > 30 && isCombatDefense);
	if (enableBaseCrawl)
	{
		const CellStruct crawlGoal = isResourceBld
		                                 ? (nearestOreCell != CellStruct::Empty ? nearestOreCell : enemyTargetCell)
		                                 : (enemyTargetCell != CellStruct::Empty ? enemyTargetCell : nearestOreCell);

		if (crawlGoal != CellStruct::Empty)
		{
			int bestAnchorDist = std::abs(center.X - crawlGoal.X) + std::abs(center.Y - crawlGoal.Y);
			for (auto const pOwnBld : pHouse->Buildings)
			{
				if (!pOwnBld || pOwnBld->InLimbo || !pOwnBld->IsAlive || !pOwnBld->Type)
					continue;

				if (!pOwnBld->Type->BaseNormal && !pOwnBld->Type->ConstructionYard && !pOwnBld->Type->Refinery &&
				    pOwnBld->Type->Factory == AbstractType::None && pOwnBld->Type->BuildCat != BuildCat::Combat)
				{
					continue;
				}

				const CellStruct bc = pOwnBld->GetMapCoords();
				const int dGoal = std::abs(bc.X - crawlGoal.X) + std::abs(bc.Y - crawlGoal.Y);
				if (dGoal < bestAnchorDist)
				{
					bestAnchorDist = dGoal;
					center = bc;
				}
			}
		}
	}

	CellStruct bestCandidate = CellStruct::Empty;
	int bestSpatialScore = -999999;
	int validCandidatesChecked = 0;

	// Search outward in spiral rings and score candidate cells directionally
	// across the full base perimeter. Combat defenses start at radius >= 5 so
	// they are placed on the outer perimeter rather than inside the ConYard core.
	const bool isNavalBld = pBuildingType->Naval || (pBuildingType->SpeedType == SpeedType::Float);
	const int startRadius = isCombatDefense ? 5 : (enableBaseCrawl ? 4 : 3);
	const int maxRadius = isNavalBld ? 46 : 26;
	const int maxCandidatesToEvaluate = 500;

	for (int radius = startRadius; radius <= maxRadius; ++radius)
	{
		const int ringStep = (radius > 10) ? 2 : 1;
		for (int dy = -radius; dy <= radius; dy += ringStep)
		{
			for (int dx = -radius; dx <= radius; dx += ringStep)
			{
				if (std::abs(dx) < radius - 1 && std::abs(dy) < radius - 1)
					continue;

				const CellStruct candidate{static_cast<short>(center.X + dx), static_cast<short>(center.Y + dy)};
				if (!MapClass::Instance.CoordinatesLegal(candidate))
					continue;

				bool foundationValid = true;
				for (int fy = 0; fy < h && foundationValid; ++fy)
				{
					for (int fx = 0; fx < w; ++fx)
					{
						const CellStruct fc{static_cast<short>(candidate.X + fx), static_cast<short>(candidate.Y + fy)};
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

				// When placing/deploying a Construction Yard, never pick a cell within
				// 14 cells of an enemy building or combat unit!
				if (pBuildingType->ConstructionYard)
				{
					bool enemyThreatNearCandidate = false;
					for (auto const pEB : BuildingClass::Array)
					{
						if (!pEB || pEB->InLimbo || !pEB->IsAlive || !pEB->Owner || !pEB->Type || pEB->Type->Immune)
							continue;
						if (pEB->Owner == pHouse || pHouse->IsAlliedWith(pEB->Owner) || pEB->Owner->IsNeutral())
							continue;
						const CellStruct ec = pEB->GetMapCoords();
						if (std::max(std::abs(candidate.X - ec.X), std::abs(candidate.Y - ec.Y)) <= 14)
						{
							enemyThreatNearCandidate = true;
							break;
						}
					}
					if (!enemyThreatNearCandidate)
					{
						for (auto const pEU : UnitClass::Array)
						{
							if (!pEU || pEU->InLimbo || !pEU->IsAlive || !pEU->Owner || !pEU->Type ||
							    pEU->Type->Harvester || pEU->Type->ResourceGatherer)
								continue;
							if (pEU->Owner == pHouse || pHouse->IsAlliedWith(pEU->Owner) || pEU->Owner->IsNeutral())
								continue;
							const CellStruct euc = pEU->GetMapCoords();
							if (std::max(std::abs(candidate.X - euc.X), std::abs(candidate.Y - euc.Y)) <= 12)
							{
								enemyThreatNearCandidate = true;
								break;
							}
						}
					}
					if (enemyThreatNearCandidate)
						continue;
				}

				int spatialScore = enableBaseCrawl ? (-radius * 5) : (-radius * 10);

				if (isResourceBld && nearestOreCell != CellStruct::Empty)
				{
					// Expert 6 & Slot 34: Crawl Refineries, Silos & Ore Purifiers
					// directly toward the nearest Ore/Tiberium patch
					const int distToOre =
					    std::abs(candidate.X - nearestOreCell.X) + std::abs(candidate.Y - nearestOreCell.Y);
					const int orePull = std::clamp(45 + (crawlBias / 10), 25, 85);
					spatialScore -= distToOre * orePull;
					if (enableBaseCrawl && radius >= 5 && radius <= 13)
						spatialScore += 180 + std::clamp(crawlBias / 3, 0, 180);
				}
				else if (isGroundDefense && enemyTargetCell != CellStruct::Empty)
				{
					// Expert 7 & Slot 38: Place Ground Defenses (Prism, Tesla, Obelisk,
					// Grand Cannon, Bunkers) on the frontline perimeter facing the enemy
					const int distToEnemy =
					    std::abs(candidate.X - enemyTargetCell.X) + std::abs(candidate.Y - enemyTargetCell.Y);
					const int enemyPull = std::clamp(38 + (turretCreepBias / 10), 20, 80);
					spatialScore -= distToEnemy * enemyPull;
					if (radius >= 6 && radius <= 15)
						spatialScore += 220 + std::clamp(turretCreepBias / 2, 0, 220);
				}
				else if (isPureAA)
				{
					// Spread AA defenses around mid-ring (radius 5..11) to cover both
					// ConYard/Tech and frontline
					if (radius >= 5 && radius <= 11)
						spatialScore += 160;
				}
				else if (isRearProtected && enemyTargetCell != CellStruct::Empty)
				{
					// Expert 1: Tuck Power Plants, Tech Centers & Superweapons safely
					// into the rear perimeter away from enemy attacks
					const int distToEnemy =
					    std::abs(candidate.X - enemyTargetCell.X) + std::abs(candidate.Y - enemyTargetCell.Y);
					spatialScore += distToEnemy * 18;
				}
				else if (isFactoryOrBarracks && enemyTargetCell != CellStruct::Empty)
				{
					// Place War Factories & Barracks forward along the crawl chain with
					// guaranteed 3-cell South/East exit corridors
					const int distToEnemy =
					    std::abs(candidate.X - enemyTargetCell.X) + std::abs(candidate.Y - enemyTargetCell.Y);
					const int fwdPull = std::clamp(14 + (crawlBias / 14), 6, 45);
					spatialScore -= distToEnemy * fwdPull;
					if (enableBaseCrawl && radius >= 5 && radius <= 12)
						spatialScore += 140;
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

// NeuralAI.Lifecycle.inl: Lifecycle: command line, base model, backpack, scenario start / end, headless frame tick.
// Part of NeuralAI.cpp (included in order; one translation unit).


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
		NeuralAI::TrainingMode = true;

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
	if (TelemetryJsonView)
	{
		UnmapViewOfFile(TelemetryJsonView);

		TelemetryJsonView = nullptr;
	}

	if (TelemetryJsonShmHandle)
	{
		CloseHandle(TelemetryJsonShmHandle);

		TelemetryJsonShmHandle = NULL;
	}

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
		if (!pRulesExt->NeuralAI_Enabled)
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
		if (pHouseExt->NeuralAI_Campaign)
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
			// Only + - * / here: the CRT picks CPU-specific exp/tanh code paths (e.g. FMA3) at
			// runtime, and one ulp of difference could flip a truncated LUT entry between two
			// multiplayer clients that bootstrap the base brain locally, causing a desync.
			const double expNeg = DeterministicExp(-norm);
			const double sig = 1.0 / (1.0 + expNeg);
			const double exp2 = DeterministicExp(2.0 * norm);
			const double tnh = (exp2 - 1.0) / (exp2 + 1.0);

			pHdr->SigmoidLUT[i] = ClampInt8(static_cast<int>(sig * 127.0));

			pHdr->TanhLUT[i] = ClampInt8(static_cast<int>(tnh * 127.0));
		}

		uint32_t offset = sizeof(AIBaseHeader);

		uint64_t seed = 0x50484F424F534149ULL;

		for (int e = 0; e < NeuralAI::NumSpecialists; ++e)
		{
			if (e < 8)
				pHdr->ExpertWeightsOffset[e] = offset;
			else
				pHdr->ExtraExpertWeightsOffset[e - 8] = offset;

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

		pHdr->BaseModelHash
			= HashBytesFNV1a(buffer.data() + sizeof(AIBaseHeader), totalBootstrapSize - sizeof(AIBaseHeader));

		HANDLE hOut
			= CreateFileA(basePath, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);

		if (hOut != INVALID_HANDLE_VALUE)
		{
			DWORD written = 0;

			WriteFile(hOut, buffer.data(), totalBootstrapSize, &written, NULL);

			CloseHandle(hOut);
		}
	}

	NeuralAI::BaseFileHandle = CreateFileA(
		basePath, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);

	if (NeuralAI::BaseFileHandle == INVALID_HANDLE_VALUE)
		return;

	LARGE_INTEGER fileSize = {};

	if (!GetFileSizeEx(NeuralAI::BaseFileHandle, &fileSize)
		|| fileSize.QuadPart < static_cast<LONGLONG>(sizeof(AIBaseHeader)))
	{
		CloseHandle(NeuralAI::BaseFileHandle);

		NeuralAI::BaseFileHandle = INVALID_HANDLE_VALUE;

		return;
	}

	NeuralAI::BaseMappedSize = static_cast<size_t>(fileSize.QuadPart);

	NeuralAI::BaseMappingHandle
		= CreateFileMappingA(NeuralAI::BaseFileHandle, NULL, PAGE_READONLY, 0, 0, "Local\\PhobosAIBaseSharedMapping");

	if (!NeuralAI::BaseMappingHandle)
		NeuralAI::BaseMappingHandle = CreateFileMappingA(NeuralAI::BaseFileHandle, NULL, PAGE_READONLY, 0, 0, NULL);

	if (NeuralAI::BaseMappingHandle)
	{
		NeuralAI::BaseMappedView
			= static_cast<const uint8_t*>(MapViewOfFile(NeuralAI::BaseMappingHandle, FILE_MAP_READ, 0, 0, 0));
	}
}

bool NeuralAI::LoadBackpackFile(const char* pPath, AIBackpackFile& outBackpack, bool requireSignature)
{
	HANDLE hFile = CreateFileA(
		pPath, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);

	if (hFile == INVALID_HANDLE_VALUE)
		return false;

	AIBackpackFile loaded = {};

	DWORD bytesRead = 0;

	const bool valid = ReadFile(hFile, &loaded, sizeof(AIBackpackFile), &bytesRead, NULL)
		&& bytesRead == sizeof(AIBackpackFile) && std::memcmp(loaded.Magic, "PHOBOSBP", 8) == 0
		&& loaded.SchemaVersion == 3;

	CloseHandle(hFile);

	if (valid)
	{
		if (requireSignature && loaded.BaseSignature != NeuralAI::CurrentBaseSignature)
			loaded.BaseSignature = NeuralAI::CurrentBaseSignature;

		outBackpack = loaded;
	}

	return valid;
}

// Reads a training policy handed over by the AI Trainer through a named page-file
// mapping (RAM only), so per-match policies never touch the SSD.
static bool LoadBackpackFromSharedMemory(const char* pName, AIBackpackFile& outBackpack)
{
	HANDLE hMapping = OpenFileMappingA(FILE_MAP_READ, FALSE, pName);

	if (!hMapping)
		return false;

	bool valid = false;

	if (auto const pView = static_cast<const uint8_t*>(MapViewOfFile(hMapping, FILE_MAP_READ, 0, 0, sizeof(AIBackpackFile))))
	{
		AIBackpackFile loaded = {};
		std::memcpy(&loaded, pView, sizeof(AIBackpackFile));
		UnmapViewOfFile(pView);

		valid = std::memcmp(loaded.Magic, "PHOBOSBP", 8) == 0 && loaded.SchemaVersion == 3;

		if (valid)
			outBackpack = loaded;
	}

	CloseHandle(hMapping);

	return valid;
}

// Context routing tables (see NeuralContextRouting). Headless training: the challenger's and the
// opponent's from the trainer's RAM handoff, slot files as fallback; the opponent shares the
// challenger's when it has none. Otherwise Resources\AI\ai_context.dat (or AI\ai_context.dat).
static void LoadContextRoutingTables()
{
	ContextRouting = NeuralContextRouting {};
	OpponentContextRouting = NeuralContextRouting {};

	char path[MAX_PATH];

	if (NeuralAI::HeadlessMode)
	{
		std::snprintf(path, sizeof(path), "Local\\PhobosAIContext_%d", NeuralAI::TrainSlotIndex);

		if (!LoadContextRoutingFromSharedMemory(path, ContextRouting))
		{
			std::snprintf(path, sizeof(path), "Resources\\AI\\ai_context_slot_%d.dat", NeuralAI::TrainSlotIndex);
			LoadContextRoutingFile(path, ContextRouting);
		}

		std::snprintf(path, sizeof(path), "Local\\PhobosAIContextOpp_%d", NeuralAI::TrainSlotIndex);

		if (!LoadContextRoutingFromSharedMemory(path, OpponentContextRouting))
		{
			std::snprintf(path, sizeof(path), "Resources\\AI\\ai_context_opp_slot_%d.dat", NeuralAI::TrainSlotIndex);
			LoadContextRoutingFile(path, OpponentContextRouting);
		}
	}
	else
	{
		for (const char* pPath : { "Resources\\AI\\ai_context.dat", "AI\\ai_context.dat" })
		{
			if (LoadContextRoutingFile(pPath, ContextRouting))
				break;
		}
	}

	Debug::Log("[NeuralAI] Context routing: %s, opponent %s.\n", ContextRouting.Loaded ? "loaded" : "none",
		OpponentContextRouting.Loaded ? "loaded" : "shared");
}

const AIBackpackFile& NeuralAI::PolicyFor(const HouseClass* pHouse)
{
	if (NeuralAI::HeadlessMode && NeuralAI::OpponentLoaded)
	{
		auto const pExt = HouseExt::TryFetch(pHouse);

		if (pExt && pExt->NeuralAI_PolicySlot == 1)
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

	// No context routing unless this match loads it below (campaign and human multiplayer never do).
	ContextRouting = NeuralContextRouting {};
	OpponentContextRouting = NeuralContextRouting {};

	// Campaign and standard Multiplayer use only the shared deterministic base
	// brain.
	if (SessionClass::IsCampaign())
	{
		NeuralAI::AllowBackpackInference = false;

		return;
	}

	if (SessionClass::IsMultiplayer() && !NeuralAI::HeadlessMode)
	{
		int humanCount = 0;
		for (auto const pH : HouseClass::Array)
		{
			if (pH && (pH->IsControlledByHuman() || pH->IsObserver()))
				humanCount++;
		}

		if (humanCount > 1)
		{
			NeuralAI::AllowBackpackInference = false;
			return;
		}
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

		// Prefer the trainer's RAM handoff; policy files are the legacy fallback.
		std::snprintf(policyPath, sizeof(policyPath), "Local\\PhobosAIPolicy_%d", NeuralAI::TrainSlotIndex);

		bool challengerLoaded = LoadBackpackFromSharedMemory(policyPath, NeuralAI::Backpack);
		const bool challengerFromShm = challengerLoaded;

		if (!challengerLoaded)
		{
			std::snprintf(
				policyPath, sizeof(policyPath), "Resources\\AI\\ai_backpack_slot_%d.dat", NeuralAI::TrainSlotIndex);

			challengerLoaded = NeuralAI::LoadBackpackFile(policyPath, NeuralAI::Backpack, false);
		}

		std::memcpy(&NeuralAI::OpponentBackpack, &NeuralAI::Backpack, sizeof(AIBackpackFile));

		std::snprintf(policyPath, sizeof(policyPath), "Local\\PhobosAIPolicyOpp_%d", NeuralAI::TrainSlotIndex);

		NeuralAI::OpponentLoaded = LoadBackpackFromSharedMemory(policyPath, NeuralAI::OpponentBackpack);

		if (!NeuralAI::OpponentLoaded)
		{
			std::snprintf(
				policyPath, sizeof(policyPath), "Resources\\AI\\ai_backpack_opp_slot_%d.dat", NeuralAI::TrainSlotIndex);

			NeuralAI::OpponentLoaded = NeuralAI::LoadBackpackFile(policyPath, NeuralAI::OpponentBackpack, false);
		}

		Debug::Log("[NeuralAI] Headless policy source: %s\n", challengerFromShm ? "shared memory" : "files");

		LoadContextRoutingTables();

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

	MapClassLoaded = false;

	for (const char* mcPath : { "Resources\\AI\\ai_mapclass.dat", "AI\\ai_mapclass.dat" })
	{
		HANDLE hMc = CreateFileA(mcPath, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);

		if (hMc == INVALID_HANDLE_VALUE)
			continue;

		struct
		{
			char Magic[8];
			uint32_t Version;
			uint32_t Classes;
		} mcHeader {};

		DWORD read = 0;

		if (ReadFile(hMc, &mcHeader, sizeof(mcHeader), &read, NULL) && read == sizeof(mcHeader)
			&& std::memcmp(mcHeader.Magic, "PHOBOSMC", 8) == 0 && mcHeader.Version == 1 && mcHeader.Classes == NumMapClasses)
		{
			MapClassLoaded = ReadFile(hMc, MapClassDeltas.data(), sizeof(MapClassDeltas), &read, NULL) && read == sizeof(MapClassDeltas);
		}

		CloseHandle(hMc);

		if (MapClassLoaded)
			break;
	}

	LoadContextRoutingTables();

	if (!valid)
	{
		std::memset(&NeuralAI::Backpack, 0, sizeof(AIBackpackFile));

		std::memcpy(NeuralAI::Backpack.Magic, "PHOBOSBP", 8);

		NeuralAI::Backpack.SchemaVersion = 3;

		NeuralAI::Backpack.BaseSignature = NeuralAI::CurrentBaseSignature;

		NeuralAI::Backpack.EstimatedELO = 1200;

		NeuralAI::Backpack.BackpackHash
			= HashBytesFNV1a(&NeuralAI::Backpack.LoRA_A[0], sizeof(NeuralAI::Backpack.LoRA_A));

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

	HANDLE hFile
		= CreateFileA(bpPath, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);

	if (hFile != INVALID_HANDLE_VALUE)
	{
		DWORD written = 0;

		WriteFile(hFile, &NeuralAI::Backpack, sizeof(AIBackpackFile), &written, NULL);

		CloseHandle(hFile);
	}

	if (!NeuralAI::HeadlessMode || NeuralAI::TrainSlotIndex == 0)
	{
		HANDLE hFileAlt = CreateFileA(
			"AI\\ai_backpack.dat", GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);

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
	if (!TelemetryJsonView)
	{
		char jsonShmName[128];

		std::snprintf(jsonShmName, sizeof(jsonShmName), "Local\\PhobosAITrainerJsonShm_%d", NeuralAI::TrainSlotIndex);

		TelemetryJsonShmHandle
			= CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, TelemetryJsonShmCapacity, jsonShmName);

		TelemetryJsonShmFromTrainer = TelemetryJsonShmHandle && GetLastError() == ERROR_ALREADY_EXISTS;

		if (TelemetryJsonShmHandle)
		{
			TelemetryJsonView = static_cast<uint8_t*>(
				MapViewOfFile(TelemetryJsonShmHandle, FILE_MAP_ALL_ACCESS, 0, 0, TelemetryJsonShmCapacity));

			if (TelemetryJsonView)
				std::memset(TelemetryJsonView, 0, 16);
		}
	}

	if (NeuralAI::TelemetryView)
		return;

	char shmName[128];

	std::snprintf(shmName, sizeof(shmName), "Local\\PhobosAITrainerShm_%d", NeuralAI::TrainSlotIndex);

	NeuralAI::TelemetryShmHandle
		= CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, sizeof(AITrainerSharedTelemetry), shmName);

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

	for (auto& ms : MacroStates)
		ms.Reset();

	FerryTrips.fill(NeuralFerryTripRecord {});
	ResetBridgePlanner();
	AirRoutes.clear();
	AirPickups.clear();
	HouseStyleSalt.fill(0);
	AirDropAborts.clear();

	for (auto& counts : DecisionCounts)
		counts.fill(0);

	for (auto& houseGates : GateCounts)
	{
		for (auto& slotGates : houseGates)
			slotGates.fill(0);
	}

	TickCosts.fill(NeuralTickCost {});

	for (auto& sections : SectionTotalMs)
		sections.fill(0.0);

	for (auto& sections : SectionMaxMs)
		sections.fill(0.0);

	for (auto& peaks : TickPeaks)
		peaks.clear();

	for (auto& ticks : ExpertTicks)
		ticks.fill(0);

	for (auto& gains : ExpertGain)
		gains.fill(0);

	for (auto& phases : ExpertTicksByPhase)
	{
		for (auto& ticks : phases)
			ticks.fill(0);
	}

	for (auto& phases : ExpertGainByPhase)
	{
		for (auto& gains : phases)
			gains.fill(0);
	}

	LastHouseAdvantage.fill(0);
	HasLastHouseAdvantage.fill(false);

	TerritoryStats.fill(NeuralTerritoryStat {});
	TerritoryLastFrame = -1;
	MapClassApplied = false;
	OreFieldCache.clear();
	OreFieldCacheFrame = -100000;
	NeuralMatchSerial++;
	WaterZoneCells.clear();
	LargestWaterZoneCells = -1;
	MapInfo = NeuralMapInfo {};

	for (auto& housePlans : PlanStats)
	{
		for (auto& outcome : housePlans)
			outcome.fill(0);
	}

	NeuralAI::LastCombatFrame = 0;

	NeuralAI::LastTotalKillsAndCaptures = 0;

	NeuralAI::LastFPSCheckTick = GetTickCount();

	NeuralAI::FramesSinceLastFPSCheck = 0;

	static const wchar_t* const ArchetypeNamesW[8] = { L"Balanced Grandmaster", L"SpecOps & Infiltration",
		L"Tech & Fortress Turtle", L"Blitzkrieg Rush", L"TechBoom & Heavy Armor", L"Harasser & Guerrilla Raider",
		L"Air & Naval Dominator", L"Siege, Urban & Coalition" };

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
			pExt->NeuralAI_PolicySlot = 0;
		}
		else if (!pFirstAI)
		{
			pFirstAI = pHouse;

			pExt->NeuralAI_PolicySlot = 0;
		}
		else if (pHouse->IsAlliedWith(pFirstAI))
		{
			pExt->NeuralAI_PolicySlot = 0;
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
					const int prevSlot = HouseExt::Fetch(pPrev)->NeuralAI_PolicySlot;

					if (prevSlot == 0)
						hasAlliedSlot0 = true;
					else
						hasAlliedSlot1 = true;
				}
			}

			if (hasAlliedSlot0)
				pExt->NeuralAI_PolicySlot = 0;
			else if (hasAlliedSlot1)
				pExt->NeuralAI_PolicySlot = 1;
			else
				pExt->NeuralAI_PolicySlot = (activeAIHouses - 1) % 2;
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
				int memBonus = 0;
				for (int e = 0; e < NeuralAI::NumSpecialists; ++e)
				{
					const uint32_t eIdx = static_cast<uint32_t>((pHouse->SideIndex * 80 + a * 10 + e) % 512);
					if (policy.Entries[eIdx].VisitCount > 0)
						memBonus += static_cast<int>(policy.Entries[eIdx].ArchetypeScoreDelta[a]) * 3;
				}

				memBonus = std::clamp(memBonus, -1200, 1200);

				const int exploreJitter = ScenarioClass::Instance->Random.RandomRanged(-90, 90);
				const int archBiasSlot = (a < 5) ? (48 + a) : (86 + (a - 5));
				const int aScore = static_cast<int>(policy.OutputBias[archBiasSlot]) + memBonus + exploreJitter;

				if (aScore > bestArchScore)
				{
					bestArchScore = aScore;

					bestArch = a;
				}
			}
		}

		pExt->NeuralAI_Archetype = bestArch;

		std::array<int, 5> openingTrained {};

		if (NeuralAI::AllowBackpackInference && NeuralAI::BackpackLoaded)
		{
			for (size_t o = 0; o < openingTrained.size(); ++o)
			{
				if (OpeningKeySlots[o] >= 0)
					openingTrained[o] = NeuralAI::PolicyFor(pHouse).OutputBias[OpeningKeySlots[o]] / 3;
			}
		}

		DrawMatchStyle(pHouse, openingTrained);

		pExt->NeuralAI_ActiveExpert = 0;

		pExt->NeuralAI_LastInferenceFrame = -1000;

		pExt->NeuralAI_NextBuildingTarget = -1;

		pExt->NeuralAI_NextUnitTarget = -1;

		pExt->NeuralAI_NextNavalTarget = -1;

		pExt->NeuralAI_NextInfantryTarget = -1;

		pExt->NeuralAI_NextAircraftTarget = -1;

		pExt->NeuralAI_PlannedBuildCell = CellStruct::Empty;

		pExt->NeuralAI_ScoutTargetCell = CellStruct::Empty;

		pExt->NeuralAI_AttackTargetCell = CellStruct::Empty;

		pExt->NeuralAI_InfiltratedEnemyMask = 0;

		pExt->NeuralAI_ScoutedEnemyMask = 0;

		pExt->NeuralAI_EstimatedEnemyThreat.fill(0);

		const char* countryId = (pHouse->Type && pHouse->Type->ID[0]) ? pHouse->Type->ID : "AIHouse";

		const int archIdx = std::clamp(pExt->NeuralAI_Archetype, 0, NeuralAI::NumArchetypes - 1);

		Debug::Log("[NeuralAI] Activated for House #%d (%s, Diff=%d) -> "
				"Archetype=%d (%ls)\n",
			pHouse->ArrayIndex, countryId, pHouse->GetAIDifficultyIndex(), archIdx, ArchetypeNamesW[archIdx]);

		if (!NeuralAI::HeadlessMode)
		{
			wchar_t msgBuf[256];

			std::swprintf(msgBuf, 256, L"[Neural AI] House #%d (%hs) -> Style: %ls", pHouse->ArrayIndex, countryId,
				ArchetypeNamesW[archIdx]);

			MessageListClass::Instance.PrintMessage(msgBuf, RulesClass::Instance->MessageDelay,
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

			MessageListClass::Instance.PrintMessage(bannerBuf, RulesClass::Instance->MessageDelay,
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
	else if (auto const pCur = HouseClass::CurrentPlayer)
	{
		const bool isObs = HouseClass::IsCurrentPlayerObserver() || Game::ObserverMode || pCur->IsInitiallyObserver()
			|| (pCur->Type && !_stricmp(pCur->Type->ID, "Observer"));
		if (isObs)
		{
			// 0x6879ED runs BEFORE 0x6879FF (MapClass::Read_INI). Do NOT set Visionary = 1 here,
			// because MapClass::Reveal (0x577E09) and MapClass::RevealTheMap (0x577F9F) early-return
			// if Visionary != 0! Reset Visionary = 0 so Frame 1 reveals all newly loaded cells.
			pCur->Visionary = 0;
		}
	}
}

void NeuralAI::OnScenarioEnd(bool playerWon)
{
	if (SessionClass::IsCampaign())
		return;

	int bestHouseIdx = -1;
	int bestScore = -1;

	SampleTerritory(true);

	for (auto const pHouse : HouseClass::Array)
	{
		if (!pHouse || pHouse->IsNeutral() || pHouse->IsObserver())
			continue;

		const int score = HouseMatchScore(pHouse);

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

			const int arch = std::clamp(pExt->NeuralAI_Archetype, 0, 7);
			// Fair credit: the result is shared by every expert that ran for at least 5% of this
			// house's expert choices, in proportion (twice its share, up to the full delta), not
			// given whole to the expert in charge when the match ended. The most used one carries
			// the archetype memory and the LoRA step.
			std::array<int, NeuralAI::NumSpecialists> ticks {};
			int totalTicks = 0;

			if (pHouse->ArrayIndex >= 0 && pHouse->ArrayIndex < 32)
				ticks = ExpertTicks[pHouse->ArrayIndex];

			for (const int t : ticks)
				totalTicks += t;

			if (totalTicks <= 0)
			{
				ticks.fill(0);
				ticks[std::clamp(pExt->NeuralAI_ActiveExpert, 0, NeuralAI::NumSpecialists - 1)] = 1;
				totalTicks = 1;
			}

			int exp = 0;

			for (int e = 1; e < NeuralAI::NumSpecialists; ++e)
			{
				if (ticks[e] > ticks[exp])
					exp = e;
			}

			const int fastBonus
				= (!NeuralAI::MatchEndStalemate && won && Unsorted::CurrentFrame > 0 && Unsorted::CurrentFrame <= 18000)
				? 4
				: 0;

			// Stalemates are zero-sum and small (+3 ahead on score / -3 behind), so experts that
			// play long games (expansion, crawl, siege) are not penalized for ending on the timer.
			const int delta = NeuralAI::MatchEndStalemate ? (won ? 3 : -3) : (won ? (12 + fastBonus) : -6);

			for (int e = 0; e < NeuralAI::NumSpecialists; ++e)
			{
				if (ticks[e] * 20 < totalTicks)
					continue;

				const int expertDelta = std::clamp((delta * 2 * ticks[e] + (delta >= 0 ? totalTicks / 2 : -totalTicks / 2)) / totalTicks,
					-std::abs(delta), std::abs(delta));
				const uint32_t entryIdx = static_cast<uint32_t>((pHouse->SideIndex * 80 + arch * 10 + e) % 512);

				if (NeuralAI::Backpack.NumEntries < 512)
					NeuralAI::Backpack.NumEntries = std::max(NeuralAI::Backpack.NumEntries, entryIdx + 1);

				auto& entry = NeuralAI::Backpack.Entries[entryIdx];

				entry.StateClusterHash = (static_cast<uint32_t>(pHouse->SideIndex) << 16) | static_cast<uint32_t>(arch << 8)
					| static_cast<uint32_t>(e);

				if (e == exp)
					entry.ArchetypeScoreDelta[arch] = ClampInt16(entry.ArchetypeScoreDelta[arch] + delta);

				if (e < 8)
					entry.ExpertScoreDelta[e] = ClampInt16(entry.ExpertScoreDelta[e] + expertDelta);
				else
					entry.BuildCategoryDelta[14 + (e - 8)] = ClampInt16(entry.BuildCategoryDelta[14 + (e - 8)] + expertDelta);

				entry.VisitCount++;
			}

			// Update LoRA rank-16 adapter weights deterministically
			const int loraIdx = (arch * 64 + exp * 8) % (256 * 16);

			NeuralAI::Backpack.LoRA_A[loraIdx] = ClampInt8(NeuralAI::Backpack.LoRA_A[loraIdx] + (won ? 2 : -1));
		}

		NeuralAI::SaveBackpack();
	}

	if (NeuralAI::HeadlessMode)
	{
		NeuralAI::UpdateTelemetrySnapshot(true,
			NeuralAI::MatchEndWinnerHouse >= 0 ? NeuralAI::MatchEndWinnerHouse : bestHouseIdx,
			NeuralAI::MatchEndStalemate);
	}
}

void NeuralAI::OnAutoBaseBuildingTriggered(HouseClass* pHouse, bool enabled)
{
	if (!pHouse)
		return;

	pHouse->AutoBaseBuilding = enabled;

	if (enabled)
	{
		if (auto const pExt = HouseExt::TryFetch(pHouse))
			pExt->NeuralAI_Campaign = true;
	}
}

void NeuralAI::OnBuildingDamaged(BuildingClass* pBuilding, TechnoClass* pAttacker, HouseClass* pAttackerHouse)
{
	if (!pBuilding || !pBuilding->Owner || !NeuralAI::IsEnabledForHouse(pBuilding->Owner))
		return;

	auto& state = GetMacroState(pBuilding->Owner);

	const CellStruct bldCoords = pBuilding->GetMapCoords();
	CellStruct attackerCoords = CellStruct::Empty;

	TechnoTypeClass* pAttackerType = nullptr;

	NeuralThreatCategory threat = NeuralThreatCategory::None;

	if (pAttacker)
	{
		attackerCoords = pAttacker->GetMapCoords();

		pAttackerType = pAttacker->GetTechnoType();

		if (pAttacker->IsInAir() || pAttacker->WhatAmI() == AbstractType::Aircraft
			|| (pAttackerType && (pAttackerType->JumpJet || pAttackerType->ConsideredAircraft)))
		{
			threat = NeuralThreatCategory::Air;
		}
		else if (pAttacker->WhatAmI() == AbstractType::Infantry)
		{
			threat = NeuralThreatCategory::Infantry;
		}
		else
		{
			threat = NeuralThreatCategory::Vehicle;
		}
	}
	else if (pAttackerHouse && pAttackerHouse->ConYards.Count > 0)
	{
		attackerCoords = pAttackerHouse->ConYards.GetItem(0)->GetMapCoords();
	}

	if (pBuilding->Type && !pBuilding->Type->IsBaseDefense && !pBuilding->Type->ConstructionYard
		&& pBuilding->GetHealthPercentage() < 0.35)
	{
		RecordUnsafePlacementZone(pBuilding->Owner, bldCoords, 2700);
	}

	auto it = std::find_if(state.AttackedBuildingsLIFO.begin(), state.AttackedBuildingsLIFO.end(),
		[&](const NeuralAttackedBuildingRecord& r) { return r.BuildingCoords == bldCoords; });

	if (it != state.AttackedBuildingsLIFO.end())
	{
		NeuralAttackedBuildingRecord rec = *it;

		rec.AttackerCoords = attackerCoords;

		rec.AttackerType = pAttackerType;

		if (threat != NeuralThreatCategory::None)
			rec.Threat = threat;

		rec.AttackFrame = Unsorted::CurrentFrame;

		state.AttackedBuildingsLIFO.erase(it);

		state.AttackedBuildingsLIFO.push_back(rec);
	}
	else
	{
		NeuralAttackedBuildingRecord rec;

		rec.BuildingCoords = bldCoords;

		rec.AttackerCoords = attackerCoords;

		rec.AttackerType = pAttackerType;

		rec.Threat = threat;

		rec.AttackFrame = Unsorted::CurrentFrame;

		state.AttackedBuildingsLIFO.push_back(rec);
	}

	if (state.AttackedBuildingsLIFO.size() > 20)
		state.AttackedBuildingsLIFO.erase(state.AttackedBuildingsLIFO.begin());
}

void NeuralAI::OnHeadlessFrameTick()
{
	if (!NeuralAI::HeadlessMode)
		return;

	NeuralAI::FramesSinceLastFPSCheck++;

	const DWORD now = GetTickCount();

	if (now - NeuralAI::LastFPSCheckTick >= 1000)
	{
		NeuralAI::CurrentMeasuredFPS
			= (NeuralAI::FramesSinceLastFPSCheck * 1000) / std::max<DWORD>(1, now - NeuralAI::LastFPSCheckTick);

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

		totalKillsAndCaptures
			+= pHouse->TotalKilledUnits + pHouse->TotalKilledBuildings + pHouse->StolenBuildingsCredits;

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
			if (!aliveCombatList[i]->IsAlliedWith(aliveCombatList[j])
				|| !aliveCombatList[j]->IsAlliedWith(aliveCombatList[i]))
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

	const bool stalemate = !onlyOneSideLeft
		&& ((frame > 18000 && (frame - NeuralAI::LastCombatFrame) >= noCombatLimit) || (frame >= maxMatchFrames));

	if (stalemate || onlyOneSideLeft)
	{
		int bestHouseIdx = lastAliveHouseIdx;
		int bestScore = -1;

		SampleTerritory(true);

		for (auto const pHouse : HouseClass::Array)
		{
			if (!pHouse || pHouse->IsNeutral() || pHouse->IsObserver() || pHouse->Defeated)
				continue;

			const int score = HouseMatchScore(pHouse);

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

	if (frame % 60 == 0)
		NeuralAI::UpdateTelemetrySnapshot(false, -1, false);
}


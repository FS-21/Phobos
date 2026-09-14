#include "TheaterTypeClass.h"

#include <Phobos.h>
#include <CCFileClass.h>
#include <FootClass.h>
#include <TechnoTypeClass.h>
#include <MapClass.h>
#include <RulesClass.h>
#include <ScenarioClass.h>
#include <VocClass.h>
#include <Utilities/Debug.h>

#include <algorithm>
#include <cctype>

template<>
const char* Enumerable<TheaterTypeClass>::GetMainSection()
{
	return "TheaterTypes";
}

std::vector<MixFileClass*> TheaterTypeClass::LoadedMixes;
std::vector<BrokenIceCell> TheaterTypeClass::BrokenIceCells;

void TheaterTypeClass::AutoInferProperties()
{
	std::string upperName = this->Name.data();
	std::transform(upperName.begin(), upperName.end(), upperName.begin(), [](unsigned char c)
	{
		return static_cast<char>(std::toupper(c));
	});

	if (!this->Root)
		this->Root = upperName.substr(0, 8).c_str();

	if (!this->IsoRoot)
		this->IsoRoot = ("ISO" + upperName.substr(0, 5)).c_str();

	if (!this->Suffix)
		this->Suffix = upperName.substr(0, 3).c_str();

	if (!this->MMSuffix)
	{
		std::string autoMM = "MM";
		autoMM += (this->Suffix[0] != '\0' ? this->Suffix[0] : (!upperName.empty() ? upperName[0] : 'T'));
		this->MMSuffix = autoMM.c_str();
	}

	if (this->ImageLetter == '\0')
		this->ImageLetter = !upperName.empty() ? upperName[0] : 'T';

	if (!this->TerrainControl)
		this->TerrainControl = (std::string(this->Root.data()) + "MD.INI").c_str();

	if (!this->PaletteISO)
		this->PaletteISO = ("ISO" + std::string(this->Suffix.data()) + ".PAL").c_str();

	if (!this->PaletteOverlay)
		this->PaletteOverlay = (std::string(this->Root.data()) + ".PAL").c_str();

	if (!this->PaletteUnit)
		this->PaletteUnit = ("UNIT" + std::string(this->Suffix.data()) + ".PAL").c_str();

	if (this->UIName.Get().empty())
		this->UIName = CSFText(("Name:" + std::string(this->Name.data())).c_str());
}

void TheaterTypeClass::LoadFromINI(CCINIClass* pINI)
{
	const char* pSection = this->Name;

	if (!pINI->GetSection(pSection))
		return;

	INI_EX exINI(pINI);
	this->UIName.Read(exINI, pSection, "UIName");

	this->Root.Read(pINI, pSection, "Root");
	this->IsoRoot.Read(pINI, pSection, "IsoRoot");
	this->Suffix.Read(pINI, pSection, "Suffix");
	this->MMSuffix.Read(pINI, pSection, "MMSuffix");

	if (pINI->ReadString(pSection, "ImageLetter", "", Phobos::readBuffer) && Phobos::readBuffer[0] != '\0')
		this->ImageLetter = static_cast<char>(std::toupper(static_cast<unsigned char>(Phobos::readBuffer[0])));

	this->IsArctic.Read(exINI, pSection, "IsArctic");
	this->IsIceGrowthEnabled.Read(exINI, pSection, "IsIceGrowthEnabled");
	this->IsVeinGrowthEnabled.Read(exINI, pSection, "IsVeinGrowthEnabled");
	this->IsAllowedInMapGenerator.Read(exINI, pSection, "IsAllowedInMapGenerator");
	this->IsGenerateVeinholesInMapGenerator.Read(exINI, pSection, "IsGenerateVeinholesInMapGenerator");
	this->LowRadarBrightness.Read(exINI, pSection, "LowRadarBrightness");
	this->HighRadarBrightness.Read(exINI, pSection, "HighRadarBrightness");

	this->TerrainControl.Read(pINI, pSection, "TerrainControl");
	this->PaletteISO.Read(pINI, pSection, "PaletteISO");
	this->PaletteOverlay.Read(pINI, pSection, "PaletteOverlay");
	this->PaletteUnit.Read(pINI, pSection, "PaletteUnit");

	if (pINI->ReadString(pSection, "Mixes", "", Phobos::readBuffer) && Phobos::readBuffer[0] != '\0')
	{
		this->CustomMixes.clear();
		char* pContext = nullptr;

		for (char* pToken = strtok_s(Phobos::readBuffer, Phobos::readDelims, &pContext); pToken; pToken = strtok_s(nullptr, Phobos::readDelims, &pContext))
		{
			std::string mixName = pToken;
			mixName.erase(0, mixName.find_first_not_of(" \t"));
			mixName.erase(mixName.find_last_not_of(" \t") + 1);

			if (!mixName.empty())
				this->CustomMixes.push_back(mixName);
		}
	}

	this->AutoInferProperties();
}

void TheaterTypeClass::Init()
{
	if (!Array.empty())
		return;

	// Pre-populate vanilla theaters in exact original index order
	struct VanillaDef
	{
		const char* Name;
		const char* UIName;
		const char* Root;
		const char* IsoRoot;
		const char* Suffix;
		const char* MMSuffix;
		char ImageLetter;
		bool IsArctic;
		bool IsIceGrowthEnabled;
		bool IsVeinGrowthEnabled;
		bool IsAllowedInMapGenerator;
		bool IsGenerateVeinholesInMapGenerator;
		float LowRadarBrightness;
		float HighRadarBrightness;
		const char* TerrainControl;
		const char* PaletteISO;
		const char* PaletteOverlay;
		const char* PaletteUnit;
	};

	static const VanillaDef vanillaList[] = {
		{ "TEMPERATE", "Name:Temperate", "TEMPERAT", "ISOTEMP", "TEM", "MMT", 'T', false, false, true,  true,  true,  1.0f, 1.6f, "TEMPERATMD.INI", "ISOTEM.PAL", "TEMPERAT.PAL", "UNITTEM.PAL" },
		{ "SNOW",      "Name:Snow",      "SNOW",     "ISOSNOW", "SNO", "MMS", 'A', true,  true,  true,  true,  true,  0.8f, 1.1f, "SNOWMD.INI",     "ISOSNO.PAL", "SNOW.PAL",     "UNITSNO.PAL" },
		{ "URBAN",     "Name:Urban",     "URBAN",    "ISOURB",  "URB", "MMU", 'U', false, false, false, false, false, 1.0f, 1.0f, "URBANMD.INI",    "ISOURB.PAL", "URBAN.PAL",    "UNITURB.PAL" },
		{ "DESERT",    "Name:Desert",    "DESERT",   "ISODES",  "DES", "MMD", 'D', false, false, false, true,  false, 1.0f, 1.0f, "DESERTMD.INI",   "ISODES.PAL", "DESERT.PAL",   "UNITDES.PAL" },
		{ "NEWURBAN",  "Name:New Urban", "URBANN",   "ISOUBN",  "UBN", "MMT", 'N', false, false, false, false, false, 1.0f, 1.0f, "URBANNMD.INI",   "ISOUBN.PAL", "URBANN.PAL",   "UNITUBN.PAL" },
		{ "LUNAR",     "Name:Lunar",     "LUNAR",    "ISOLUN",  "LUN", "MML", 'L', false, false, false, false, false, 1.0f, 1.0f, "LUNARMD.INI",    "ISOLUN.PAL", "LUNAR.PAL",    "UNITLUN.PAL" }
	};

	for (const auto& def : vanillaList)
	{
		auto pItem = FindOrAllocate(def.Name);
		pItem->UIName = CSFText(def.UIName);
		pItem->Root = def.Root;
		pItem->IsoRoot = def.IsoRoot;
		pItem->Suffix = def.Suffix;
		pItem->MMSuffix = def.MMSuffix;
		pItem->ImageLetter = def.ImageLetter;
		pItem->IsArctic = def.IsArctic;
		pItem->IsIceGrowthEnabled = def.IsIceGrowthEnabled;
		pItem->IsVeinGrowthEnabled = def.IsVeinGrowthEnabled;
		pItem->IsAllowedInMapGenerator = def.IsAllowedInMapGenerator;
		pItem->IsGenerateVeinholesInMapGenerator = def.IsGenerateVeinholesInMapGenerator;
		pItem->LowRadarBrightness = def.LowRadarBrightness;
		pItem->HighRadarBrightness = def.HighRadarBrightness;
		pItem->TerrainControl = def.TerrainControl;
		pItem->PaletteISO = def.PaletteISO;
		pItem->PaletteOverlay = def.PaletteOverlay;
		pItem->PaletteUnit = def.PaletteUnit;
	}
}

void TheaterTypeClass::LoadTheatersINI()
{
	Init();

	const char* const iniFiles[] = { "theatersmd.ini", "theaters.ini" };
	const char* pSelectedFile = nullptr;

	for (const auto& filename : iniFiles)
	{
		CCFileClass file(filename);
		if (file.Exists())
		{
			pSelectedFile = filename;
			break;
		}
	}

	if (!pSelectedFile)
		return;

	CCINIClass ini;
	CCFileClass file(pSelectedFile);
	if (file.Open(FileAccessMode::Read))
	{
		ini.ReadCCFile(&file);
		file.Close();

		LoadFromINIList(&ini);

		Debug::Log("Loaded %zu theater types from %s\n", Array.size(), pSelectedFile);
	}
}

const TheaterTypeClass* TheaterTypeClass::As_Pointer(TheaterType type)
{
	return As_Pointer(static_cast<int>(type));
}

const TheaterTypeClass* TheaterTypeClass::As_Pointer(int index)
{
	if (index >= 0 && index < static_cast<int>(Array.size()))
		return Array[index].get();

	return nullptr;
}

const char* TheaterTypeClass::Suffix_From(TheaterType type)
{
	if (const auto pTheater = As_Pointer(type))
	{
		if (pTheater->Suffix[0] != '\0')
			return pTheater->Suffix.data();
	}

	return "TEM";
}

const char* TheaterTypeClass::MMSuffix_From(TheaterType type)
{
	if (const auto pTheater = As_Pointer(type))
	{
		if (pTheater->MMSuffix[0] != '\0')
			return pTheater->MMSuffix.data();
	}

	return "MMT";
}

char TheaterTypeClass::ImageLetter_From(TheaterType type)
{
	if (const auto pTheater = As_Pointer(type))
	{
		if (pTheater->ImageLetter != '\0')
			return pTheater->ImageLetter;
	}

	return 'T';
}

bool TheaterTypeClass::Is_Arctic(TheaterType type)
{
	if (const auto pTheater = As_Pointer(type))
		return pTheater->IsArctic.Get();

	return false;
}

bool TheaterTypeClass::Ice_Growth_Allowed(TheaterType type)
{
	if (const auto pTheater = As_Pointer(type))
		return pTheater->IsIceGrowthEnabled.Get();

	return false;
}

bool TheaterTypeClass::Vein_Growth_Allowed(TheaterType type)
{
	if (const auto pTheater = As_Pointer(type))
		return pTheater->IsVeinGrowthEnabled.Get();

	return false;
}

bool TheaterTypeClass::Allowed_In_Map_Generator(TheaterType type)
{
	if (const auto pTheater = As_Pointer(type))
		return pTheater->IsAllowedInMapGenerator.Get();

	return false;
}

bool TheaterTypeClass::Veins_Allowed_In_Map_Generator(TheaterType type)
{
	if (const auto pTheater = As_Pointer(type))
		return pTheater->IsGenerateVeinholesInMapGenerator.Get();

	return false;
}

float TheaterTypeClass::Low_Radar_Brightness(TheaterType type)
{
	if (const auto pTheater = As_Pointer(type))
		return pTheater->LowRadarBrightness.Get();

	return 1.0f;
}

float TheaterTypeClass::High_Radar_Brightness(TheaterType type)
{
	if (const auto pTheater = As_Pointer(type))
		return pTheater->HighRadarBrightness.Get();

	return 1.0f;
}

void TheaterTypeClass::UnloadCurrentMixes()
{
	if (LoadedMixes.empty())
		return;

	for (auto pMix : LoadedMixes)
	{
		if (pMix)
			GameDelete(pMix);
	}

	LoadedMixes.clear();
	MixFileClass::DestroyCache();
}

void TheaterTypeClass::LoadCurrentMixes(const TheaterTypeClass* pTheater)
{
	UnloadCurrentMixes();

	if (!pTheater)
		return;

	auto tryLoadMix = [](const char* pFilename)
	{
		CCFileClass testFile(pFilename);
		if (testFile.Exists())
		{
			if (auto pMix = GameCreate<MixFileClass>(pFilename))
				LoadedMixes.push_back(pMix);
		}
	};

	if (!pTheater->CustomMixes.empty())
	{
		for (const auto& mixName : pTheater->CustomMixes)
			tryLoadMix(mixName.c_str());
	}
	else
	{
		char buffer[128];

		// Base theater mix: <Root>.MIX
		_snprintf_s(buffer, sizeof(buffer), "%s.MIX", pTheater->Root.data());
		tryLoadMix(buffer);

		// Isometric tileset mix: <IsoRoot>.MIX
		_snprintf_s(buffer, sizeof(buffer), "%s.MIX", pTheater->IsoRoot.data());
		tryLoadMix(buffer);

		// Tile art and unit palette mix: <Suffix>.MIX
		_snprintf_s(buffer, sizeof(buffer), "%s.MIX", pTheater->Suffix.data());
		tryLoadMix(buffer);

		// Isometric expansion mix: <IsoRoot>MD.MIX
		_snprintf_s(buffer, sizeof(buffer), "%sMD.MIX", pTheater->IsoRoot.data());
		tryLoadMix(buffer);

		// Control expansion mix: <Root>MD.MIX
		_snprintf_s(buffer, sizeof(buffer), "%sMD.MIX", pTheater->Root.data());
		tryLoadMix(buffer);

		// Arctic theater expansion mix
		if (pTheater->IsArctic.Get())
			tryLoadMix("SNOWMD.MIX");
	}
}

void TheaterTypeClass::ProcessIceMovement(FootClass* pThis, const CellStruct& currentCell)
{
	if (!pThis || !ScenarioClass::Instance || !Ice_Growth_Allowed(ScenarioClass::Instance->Theater))
		return;

	CellClass* pCell = MapClass::Instance.GetCellAt(currentCell);
	if (!pCell || pCell->LandType != LandType::Ice)
		return;

	TechnoTypeClass* const pType = pThis->GetTechnoType();
	if (!pType)
		return;

	double const weight = pType->Weight;
	auto const pRules = RulesClass::Instance;
	if (!pRules)
		return;

	if (pRules->IceBreakingWeight > 0.0 && weight >= pRules->IceBreakingWeight)
	{
		if (pRules->IceCrackSounds.Count > 0)
		{
			int const soundIdx = pRules->IceCrackSounds.GetItem(ScenarioClass::Instance->Random.RandomRanged(0, pRules->IceCrackSounds.Count - 1));
			VocClass::PlayAt(soundIdx, pCell->GetCoords());
		}

		pCell->LandType = LandType::Water;

		int const regenTicks = pRules->IceGrowthRate > 0.0
			? static_cast<int>(pRules->IceGrowthRate * 15.0)
			: 450;
		BrokenIceCells.push_back({ currentCell, regenTicks });

		if (pType->SpeedType != SpeedType::Amphibious
			&& pType->SpeedType != SpeedType::Hover
			&& pType->SpeedType != SpeedType::Winged
			&& pType->SpeedType != SpeedType::Float)
		{
			int damage = pThis->Health;
			pThis->ReceiveDamage(&damage, 0, pRules->C4Warhead, nullptr, true, false, nullptr);
		}
	}
	else if (pRules->IceCrackingWeight > 0.0 && weight >= pRules->IceCrackingWeight)
	{
		if (pRules->IceCrackSounds.Count > 0)
		{
			int const soundIdx = pRules->IceCrackSounds.GetItem(ScenarioClass::Instance->Random.RandomRanged(0, pRules->IceCrackSounds.Count - 1));
			VocClass::PlayAt(soundIdx, pCell->GetCoords());
		}
	}
}

void TheaterTypeClass::UpdateIceRegeneration()
{
	if (BrokenIceCells.empty() || !ScenarioClass::Instance || !Ice_Growth_Allowed(ScenarioClass::Instance->Theater))
		return;

	for (auto it = BrokenIceCells.begin(); it != BrokenIceCells.end(); )
	{
		it->RemainingTicks--;

		if (it->RemainingTicks <= 0)
		{
			if (CellClass* pCell = MapClass::Instance.GetCellAt(it->Coords))
			{
				if (pCell->LandType == LandType::Water)
					pCell->LandType = LandType::Ice;
			}

			it = BrokenIceCells.erase(it);
		}
		else
			++it;
	}
}

void TheaterTypeClass::ClearIceState()
{
	BrokenIceCells.clear();
}

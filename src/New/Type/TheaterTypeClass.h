#pragma once

#include <Utilities/Enumerable.h>
#include <Utilities/TemplateDef.h>
#include <Utilities/Constructs.h>
#include <GeneralDefinitions.h>
#include <CCINIClass.h>
#include <MixFileClass.h>
#include <GeneralStructures.h>

#include <string>
#include <vector>

class FootClass;

struct BrokenIceCell
{
	CellStruct Coords;
	int RemainingTicks;
};

class TheaterTypeClass final : public Enumerable<TheaterTypeClass>
{
public:
	Valueable<CSFText> UIName;
	PhobosFixedString<10> Root;
	PhobosFixedString<10> IsoRoot;
	PhobosFixedString<4> Suffix;
	PhobosFixedString<4> MMSuffix;
	char ImageLetter;
	Valueable<bool> IsArctic;
	Valueable<bool> IsIceGrowthEnabled;
	Valueable<bool> IsVeinGrowthEnabled;
	Valueable<bool> IsAllowedInMapGenerator;
	Valueable<bool> IsGenerateVeinholesInMapGenerator;
	Valueable<float> LowRadarBrightness;
	Valueable<float> HighRadarBrightness;

	PhobosFixedString<32> TerrainControl;
	PhobosFixedString<32> PaletteISO;
	PhobosFixedString<32> PaletteOverlay;
	PhobosFixedString<32> PaletteUnit;
	std::vector<std::string> CustomMixes;

	TheaterTypeClass(const char* pTitle = NONE_STR) : Enumerable<TheaterTypeClass>(pTitle)
		, UIName { }
		, Root { }
		, IsoRoot { }
		, Suffix { }
		, MMSuffix { }
		, ImageLetter { '\0' }
		, IsArctic { false }
		, IsIceGrowthEnabled { false }
		, IsVeinGrowthEnabled { false }
		, IsAllowedInMapGenerator { false }
		, IsGenerateVeinholesInMapGenerator { false }
		, LowRadarBrightness { 1.0f }
		, HighRadarBrightness { 1.0f }
		, TerrainControl { }
		, PaletteISO { }
		, PaletteOverlay { }
		, PaletteUnit { }
		, CustomMixes { }
	{ }

	~TheaterTypeClass() = default;

	void LoadFromINI(CCINIClass* pINI);
	void AutoInferProperties();

	static void Init();
	static void LoadTheatersINI();

	static const TheaterTypeClass* As_Pointer(TheaterType type);
	static const TheaterTypeClass* As_Pointer(int index);
	static const char* Suffix_From(TheaterType type);
	static const char* MMSuffix_From(TheaterType type);
	static char ImageLetter_From(TheaterType type);
	static bool Is_Arctic(TheaterType type);
	static bool Ice_Growth_Allowed(TheaterType type);
	static bool Vein_Growth_Allowed(TheaterType type);
	static bool Allowed_In_Map_Generator(TheaterType type);
	static bool Veins_Allowed_In_Map_Generator(TheaterType type);
	static float Low_Radar_Brightness(TheaterType type);
	static float High_Radar_Brightness(TheaterType type);

	static std::vector<MixFileClass*> LoadedMixes;
	static void UnloadCurrentMixes();
	static void LoadCurrentMixes(const TheaterTypeClass* pTheater);

	static std::vector<BrokenIceCell> BrokenIceCells;
	static void ProcessIceMovement(FootClass* pThis, const CellStruct& currentCell);
	static void UpdateIceRegeneration();
	static void ClearIceState();
};


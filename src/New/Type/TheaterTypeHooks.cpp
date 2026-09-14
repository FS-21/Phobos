#include "TheaterTypeClass.h"

#include <Ext/Scenario/Body.h>
#include <Ext/Foot/Body.h>
#include <FileSystem.h>
#include <FootClass.h>
#include <MapClass.h>
#include <MapSeedClass.h>
#include <RulesClass.h>
#include <StringTable.h>
#include <VocClass.h>
#include <OwnerDraw.h>
#include <Utilities/Macro.h>

#include <vector>

// Dynamic theater index lookup supporting all custom theaters
DEFINE_HOOK(0x48DBE5, Theater_FindIndex, 0x7)
{
	GET(const char*, pName, ECX);

	int const idx = TheaterTypeClass::FindIndex(pName);
	R->EAX(idx);

	return 0x48DC0F;
}

// Reimplementation of theater runtime initialization for scenario load
DEFINE_HOOK(0x5349C9, Theater_Init, 0x6)
{
	GET(TheaterType, theater, ECX);

	if (theater == ScenarioClass::LastTheater)
		return 0x534DCA;

	const auto pTheater = TheaterTypeClass::As_Pointer(theater);
	if (!pTheater)
		return 0x534DCA;

	TheaterTypeClass::LoadCurrentMixes(pTheater);

	// Overlay palette loading and 6-bit to 8-bit color conversion
	char palBuffer[64];
	const char* pOverlayPalName = pTheater->PaletteOverlay[0] != '\0'
		? pTheater->PaletteOverlay.data()
		: nullptr;

	if (!pOverlayPalName)
	{
		_snprintf_s(palBuffer, sizeof(palBuffer), "%s.PAL", pTheater->Root.data());
		pOverlayPalName = palBuffer;
	}

	if (auto pRawPal = reinterpret_cast<BytePalette*>(FileSystem::LoadFile(pOverlayPalName)))
	{
		for (int i = 0; i < 256; ++i)
		{
			FileSystem::TEMPERAT_PAL[i].R = static_cast<BYTE>(pRawPal->Entries[i].R << 2);
			FileSystem::TEMPERAT_PAL[i].G = static_cast<BYTE>(pRawPal->Entries[i].G << 2);
			FileSystem::TEMPERAT_PAL[i].B = static_cast<BYTE>(pRawPal->Entries[i].B << 2);
		}

		reinterpret_cast<void(__thiscall*)(void*, BytePalette*)>(0x6260D0)(
			reinterpret_cast<void*>(0x885A80), &FileSystem::TEMPERAT_PAL);
	}

	// Unit palette loading and 6-bit to 8-bit color conversion
	char unitPalBuffer[64];
	const char* pUnitPalName = pTheater->PaletteUnit[0] != '\0'
		? pTheater->PaletteUnit.data()
		: nullptr;

	if (!pUnitPalName)
	{
		_snprintf_s(unitPalBuffer, sizeof(unitPalBuffer), "UNIT%s.PAL", pTheater->Suffix.data());
		pUnitPalName = unitPalBuffer;
	}

	BytePalette* const pUnitPalDest = reinterpret_cast<BytePalette*>(0x886380);
	if (auto pRawUnitPal = reinterpret_cast<BytePalette*>(FileSystem::LoadFile(pUnitPalName)))
	{
		for (int i = 0; i < 256; ++i)
		{
			pUnitPalDest->Entries[i].R = static_cast<BYTE>(pRawUnitPal->Entries[i].R << 2);
			pUnitPalDest->Entries[i].G = static_cast<BYTE>(pRawUnitPal->Entries[i].G << 2);
			pUnitPalDest->Entries[i].B = static_cast<BYTE>(pRawUnitPal->Entries[i].B << 2);
		}

		reinterpret_cast<void(*)()>(0x48D080)();
		reinterpret_cast<void(__thiscall*)(void*, BytePalette*)>(0x6260D0)(
			reinterpret_cast<void*>(0x886380), pUnitPalDest);
	}

	// Rebuild lighting and player color remap surfaces
	int const houseCount = *reinterpret_cast<int*>(0xB054E0);
	void* const* const pHouseRemaps = *reinterpret_cast<void***>(0xB054D4);
	if (pHouseRemaps)
	{
		for (int i = 0; i < houseCount; ++i)
		{
			if (void* pRemap = pHouseRemaps[i])
			{
				reinterpret_cast<void(__thiscall*)(void*, BytePalette*, BytePalette*, int, int, int)>(0x68C860)(
					pRemap, pUnitPalDest, &FileSystem::TEMPERAT_PAL, 1000, 1000, 1000);
			}
		}
	}

	// Update INIColors and visual convert tables
	reinterpret_cast<void(__fastcall*)(int)>(0x6267A0)(static_cast<int>(theater));
	reinterpret_cast<void(*)()>(0x717840)();

	return 0x534DBE;
}

// Isometric tileset filename hooks
DEFINE_HOOK(0x5452F2, IsometricTileTypeClass_SlopeZ, 0x6)
{
	auto const pSuffix = TheaterTypeClass::Suffix_From(ScenarioClass::Instance->Theater);

	static char slopeBuffer[32];
	_snprintf_s(slopeBuffer, sizeof(slopeBuffer), "SLOP01Z.%s", pSuffix);
	R->ECX(slopeBuffer);

	return 0x545315;
}

DEFINE_HOOK(0x54547F, IsometricTileTypeClass_PaletteISO, 0x6)
{
	auto const pTheater = TheaterTypeClass::As_Pointer(ScenarioClass::Instance->Theater);

	static char palBuffer[64];
	const char* pPal = (pTheater && pTheater->PaletteISO[0] != '\0')
		? pTheater->PaletteISO.data()
		: nullptr;

	if (!pPal)
	{
		_snprintf_s(palBuffer, sizeof(palBuffer), "ISO%s.PAL", pTheater ? pTheater->Suffix.data() : "TEM");
		pPal = palBuffer;
	}

	R->ECX(pPal);

	return 0x5454A2;
}

DEFINE_HOOK(0x5454F0, IsometricTileTypeClass_TerrainControl, 0x6)
{
	auto const pTheater = TheaterTypeClass::As_Pointer(ScenarioClass::Instance->Theater);

	static char ctrlBuffer[64];
	const char* pCtrl = (pTheater && pTheater->TerrainControl[0] != '\0')
		? pTheater->TerrainControl.data()
		: nullptr;

	if (!pCtrl)
	{
		_snprintf_s(ctrlBuffer, sizeof(ctrlBuffer), "%sMD.INI", pTheater ? pTheater->Root.data() : "TEMPERAT");
		pCtrl = ctrlBuffer;
	}

	R->ECX(pCtrl);

	return 0x545513;
}

DEFINE_HOOK(0x546753, IsometricTileTypeClass_MarbleMadness, 0x6)
{
	R->EAX(TheaterTypeClass::MMSuffix_From(ScenarioClass::Instance->Theater));
	return 0x546759;
}

// BuildingTypeClass shape suffix hooks
DEFINE_HOOK(0x4279BB, BuildingTypeClass_Graphic1, 0x6)
{
	R->EDX(TheaterTypeClass::Suffix_From(ScenarioClass::Instance->Theater));
	return 0x4279C1;
}

DEFINE_HOOK(0x427AF1, BuildingTypeClass_Graphic2, 0x5)
{
	R->EAX(TheaterTypeClass::Suffix_From(ScenarioClass::Instance->Theater));
	return 0x427AF6;
}

DEFINE_HOOK(0x428903, BuildingTypeClass_Graphic3, 0x6)
{
	R->ECX(TheaterTypeClass::Suffix_From(ScenarioClass::Instance->Theater));
	return 0x428909;
}

DEFINE_HOOK(0x428CBF, BuildingTypeClass_Graphic4, 0x6)
{
	R->ECX(TheaterTypeClass::Suffix_From(ScenarioClass::Instance->Theater));
	return 0x428CC5;
}

// AnimTypeClass art suffix hook
DEFINE_HOOK(0x71DCE4, AnimTypeClass_Load2DArt, 0x6)
{
	R->ECX(TheaterTypeClass::Suffix_From(ScenarioClass::Instance->Theater));
	return 0x71DCEA;
}

// OverlayTypeClass shape suffix hooks
DEFINE_HOOK(0x5FE673, OverlayTypeClass_Init, 0x6)
{
	R->ECX(TheaterTypeClass::Suffix_From(ScenarioClass::Instance->Theater));
	return 0x5FE679;
}

DEFINE_HOOK(0x5FEB94, OverlayTypeClass_Load1, 0x6)
{
	R->ECX(TheaterTypeClass::Suffix_From(ScenarioClass::Instance->Theater));
	return 0x5FEB9A;
}

DEFINE_HOOK(0x5FEE42, OverlayTypeClass_Load2, 0x6)
{
	R->EDX(TheaterTypeClass::Suffix_From(ScenarioClass::Instance->Theater));
	return 0x5FEE48;
}

// SmudgeTypeClass shape suffix hooks
DEFINE_HOOK(0x6B54CF, SmudgeTypeClass_Init, 0x6)
{
	R->ECX(TheaterTypeClass::Suffix_From(ScenarioClass::Instance->Theater));
	return 0x6B54D5;
}

DEFINE_HOOK(0x6B57A7, SmudgeTypeClass_Load, 0x6)
{
	R->ECX(TheaterTypeClass::Suffix_From(ScenarioClass::Instance->Theater));
	return 0x6B57AD;
}

// TerrainTypeClass suffix and image letter hooks
DEFINE_HOOK(0x5F915C, TerrainTypeClass_Suffix, 0x6)
{
	R->EDX(TheaterTypeClass::Suffix_From(ScenarioClass::Instance->Theater));
	return 0x5F9162;
}

DEFINE_HOOK(0x5F91D7, TerrainTypeClass_ImageLetter, 0x6)
{
	R->AL(TheaterTypeClass::ImageLetter_From(ScenarioClass::Instance->Theater));
	return 0x5F91DD;
}

// VoxelAnim / VeinholeMonster suffix hook
DEFINE_HOOK(0x74D463, VoxelAnim_Suffix, 0x5)
{
	R->EAX(TheaterTypeClass::Suffix_From(ScenarioClass::Instance->Theater));
	return 0x74D468;
}

// ObjectTypeClass generic fallback shape hooks
DEFINE_HOOK(0x45E9FD, ObjectTypeClass_Art1, 0x6)
{
	R->ECX(TheaterTypeClass::Suffix_From(ScenarioClass::Instance->Theater));
	return 0x45EA03;
}

DEFINE_HOOK(0x45EA60, ObjectTypeClass_Art2, 0x6)
{
	R->ECX(TheaterTypeClass::Suffix_From(ScenarioClass::Instance->Theater));
	return 0x45EA66;
}

DEFINE_HOOK(0x45F96E, ObjectTypeClass_Art3, 0x6)
{
	R->EDI(TheaterTypeClass::Suffix_From(ScenarioClass::Instance->Theater));
	return 0x45F974;
}

// Minimap radar brightness
DEFINE_HOOK(0x47C318, RadarClass_Brightness, 0x6)
{
	float const brightness = TheaterTypeClass::Low_Radar_Brightness(ScenarioClass::Instance->Theater);
	R->EAX(*reinterpret_cast<const DWORD*>(&brightness));
	return 0x47C31E;
}

// Arctic visual and occupation behavior hooks
DEFINE_HOOK(0x483DF0, IsArctic_Cell, 0x5)
{
	if (TheaterTypeClass::Is_Arctic(ScenarioClass::Instance->Theater))
		return 0x483E0C;

	return 0x483DF5;
}

DEFINE_HOOK(0x65B5EA, IsArctic_Building, 0x8)
{
	if (TheaterTypeClass::Is_Arctic(ScenarioClass::Instance->Theater))
	{
		R->EBX(0xFFFFFF80);
		R->EAX(0xFFFFFF80);
	}
	else
	{
		auto const pBytes = reinterpret_cast<const BYTE*>(R->EDI());
		R->EAX(pBytes[0x1830]);
	}

	return 0x65B604;
}

DEFINE_HOOK(0x6DAE22, IsArctic_RMG, 0x7)
{
	int const lightLevel = TheaterTypeClass::Is_Arctic(ScenarioClass::Instance->Theater) ? 12 : 14;

	R->ESI(0x210);
	R->EBP(0);
	R->EAX(lightLevel);
	*reinterpret_cast<DWORD*>(R->ESP() + 0x10) = 0x210;
	*reinterpret_cast<DWORD*>(R->ESP() + 0x28) = static_cast<DWORD>(lightLevel);
	*reinterpret_cast<DWORD*>(R->ESP() + 0x14) = 0;

	return 0x6DAE46;
}

// Random Map Generator UI combo box options
DEFINE_HOOK(0x5970AF, RMG_MenuOptions, 0x7)
{
	GET(HWND, hWnd, EDI);

	auto addOption = [hWnd](const wchar_t* pLabel, int index)
	{
		auto const listItem = SendMessageW(hWnd, WW_CB_ADDSTRINGW, 0, reinterpret_cast<LPARAM>(pLabel));
		SendMessageA(hWnd, CB_SETITEMDATA, listItem, index);
	};

	for (size_t i = 0; i < TheaterTypeClass::Array.size(); ++i)
	{
		const auto& pTheater = TheaterTypeClass::Array[i];

		if (TheaterTypeClass::Allowed_In_Map_Generator(static_cast<TheaterType>(i)))
		{
			const wchar_t* pUIName = pTheater->UIName.Get();
			addOption(pUIName && *pUIName ? pUIName : StringTable::FetchString(pTheater->Name.data()), static_cast<int>(i));
		}
	}

	int const currentTheater = MapSeedClass::Instance.Theater;
	if (const auto pCurTheater = TheaterTypeClass::As_Pointer(currentTheater))
	{
		const wchar_t* pUIName = pCurTheater->UIName.Get();
		auto const item = SendMessageW(hWnd, WW_CB_FINDSTRINGEXACTW, 0,
			reinterpret_cast<LPARAM>(pUIName && *pUIName ? pUIName : StringTable::FetchString(pCurTheater->Name.data())));
		SendMessageA(hWnd, CB_SETCURSEL, item, 0);
	}

	R->EBP(&MapSeedClass::Instance);
	return 0x59712A;
}

DEFINE_HOOK(0x5997AB, RMG_Generator, 0x9)
{
	int const theaterIdx = MapSeedClass::Instance.Theater;
	if (const auto pTheater = TheaterTypeClass::As_Pointer(theaterIdx))
		R->ECX(pTheater->Name.data());
	else
		R->ECX("TEMPERATE");

	return 0x5997C6;
}

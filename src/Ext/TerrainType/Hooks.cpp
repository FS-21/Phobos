#include "Body.h"

#include <ScenarioClass.h>
#include <TiberiumClass.h>
#include <OverlayTypeClass.h>

#include "../../Utilities/GeneralUtils.h"
#include "../../Utilities/Macro.h"

namespace TerrainTypeTemp
{
	TerrainTypeClass* pCurrentType = nullptr;
	TerrainTypeExt::ExtData* pCurrentExt = nullptr;
}

DEFINE_HOOK(71C853, TerrainTypeClass_Context_Set, 6)
{
	TerrainTypeTemp::pCurrentType = R->ECX<TerrainTypeClass*>();
	TerrainTypeTemp::pCurrentExt = TerrainTypeExt::ExtMap.Find(TerrainTypeTemp::pCurrentType);

	return 0;
}
/*
// I don't want to rewrite timer stuff so I just NOP out argument push
// which the compiler placed way before the call - Kerbiter
DEFINE_PATCH(0x71C8A3, 0x90, 0x90)

DEFINE_HOOK(71C8CE, TerrainClass_AI_CellsPerAnim, 0)
{
	GET(CellClass*, pCell, EAX);

	int cellCount = 1;
	if (TerrainTypeTemp::pCurrentExt)
		cellCount = TerrainTypeTemp::pCurrentExt->GetCellsPerAnim();

	for (int i = 0; i < cellCount; i++)
		pCell->SpreadTiberium(true);

	// stack depth is fixed by the patch above - Kerbiter
	return 0x71C8D5;
}
*/

DEFINE_HOOK(483811, CellClass_SpreadTiberium_TiberiumType, 8)
{
	if (TerrainTypeTemp::pCurrentExt)
	{
		LEA_STACK(int*, pTibType, STACK_OFFS(0x1C, -0x4));
		
		*pTibType = TerrainTypeTemp::pCurrentExt->SpawnsTiberium_Type;

		return 0x483819;
	}

	return 0;
}

DEFINE_HOOK(48381D, CellClass_SpreadTiberium_CellSpread, 6)
{
	enum { SpreadReturn = 0x4838CA, NoSpreadReturn = 0x4838B0 };

	if (TerrainTypeTemp::pCurrentExt)
	{
		GET(CellClass*, pThis, EDI);
		GET(int, tibIndex, EAX);

		TiberiumClass* pTib = TiberiumClass::Array->GetItem(tibIndex);

		std::vector<CellStruct> adjacentCells = GeneralUtils::AdjacentCellsInRange(TerrainTypeTemp::pCurrentExt->SpawnsTiberium_Range);
		size_t size = adjacentCells.size();
		int rand = ScenarioClass::Instance->Random.RandomRanged(0, size - 1);

		for (unsigned int i = 0; i < size; i++)
		{
			unsigned int cellIndex = (i + rand) % size;
			CellStruct tgtPos = pThis->MapCoords + adjacentCells[cellIndex];
			CellClass* tgtCell = MapClass::Instance->GetCellAt(tgtPos);

			if (tgtCell && tgtCell->CanTiberiumGerminate(pTib))
			{
				R->EAX<bool>(tgtCell->IncreaseTiberium(tibIndex,
					TerrainTypeTemp::pCurrentExt->GetTiberiumGrowthStage()));

				return SpreadReturn;
			}
		}

		return NoSpreadReturn;
	}

	return 0;
}

DEFINE_HOOK(71C8D7, TerrainTypeClass_Context_Unset, 5)
{
	TerrainTypeTemp::pCurrentType = nullptr;
	TerrainTypeTemp::pCurrentExt = nullptr;

	return 0;
}

DEFINE_HOOK(71C871, TerrainTypeClass_Update_bug, 5)
{
		GET(TerrainTypeClass*, pTypeTerrain, ECX); // v9 <- in theory this provokes a crash
		// In psudocode:
		// v6 = *(__int16 *)(((int (__thiscall *)(TerrainTypeClass *))v9->vt->GetImage)(v9) + 6) / 2;
		GET(int *, v6, EAX); // v6
		auto object = pTypeTerrain->GetImage();
		if (object && v6 != nullptr)
		{
			return 0;
		}
		else
		{
			Debug::Log("AAAAA Peta\n");
			return 0x71C8D5;
		}
}

DEFINE_HOOK(5657A4, MapClass_Coord_Cell_bug, 5)
{
	GET(CellStruct *, pCell, EDX); // v9 <- in theory this provokes a crash
	// In psudocode:
	// v6 = *(__int16 *)(((int (__thiscall *)(TerrainTypeClass *))v9->vt->GetImage)(v9) + 6) / 2;
	//GET(int, tibIndex, EAX); // v6
	//GET(MapClass *, pThis, ECX); // this
	//auto object = pTypeTerrain->GetImage();
	try
	{

		Debug::Log("PRE-AAAA Y: %d\n", pCell->Y);
	}
	catch (...)
	{
		Debug::Log("POST-AAAA. crashed\n");
		pCell = new CellStruct();
		pCell->X = 0;
		pCell->Y = 0;

		return 0x5657D5;
	}

	return 0;
}

DEFINE_HOOK(5FDD3A, MapClass_Coord_Cell_bug2, 6)
{
	GET(OverlayTypeClass *, pTypeOverlay, EAX); // v9 <- in theory this provokes a crash
	try
	{
		Debug::Log("PRE-BBBB Overlay->Tiberium: %d\n", pTypeOverlay->Tiberium);
	}
	catch (...)
	{
		Debug::Log("POST-BBBB. crashed\n");

		return 0x5FDDD5;
	}

	return 0;
}

DEFINE_HOOK(7231B4, MapClass_Coord_Cell_bug3, 6)
{
	GET(int, value_EAX, EAX);
	GET(int, value_EBX, EBX);
	GET(int, value_ECX, ECX);

	try
	{
		Debug::Log("AAA3 value_ECX: %d\n", value_ECX);
		Debug::Log("AAA3 value_EAX: %d\n", value_EAX);
		Debug::Log("AAA3 value_EBX: %d\n", value_EBX);
	}
	catch (...)
	{
		Debug::Log("POST-BBB3. crashed\n");

		return 0x72324E;// 723255;
	}

	return 0;
}

DEFINE_HOOK(72318D, MapClass_Coord_Cell_bug4, 6)
{
	GET(int, value_EAX, EAX);
	GET(int, value_EDX, EDX);
	GET(int, value_ECX, ECX);

	try
	{
		Debug::Log("CCC4 value_ECX: %d\n", value_ECX);
		Debug::Log("CCC4 value_EAX: %d\n", value_EAX);
		Debug::Log("CCC4 value_EDX: %d\n", value_EDX);
	}
	catch (...)
	{
		Debug::Log("POST-DDD4. crashed\n");
		
		return 0x72324E;
	}

	return 0;
}
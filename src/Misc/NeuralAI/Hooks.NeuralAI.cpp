#include "NeuralAI.h"

#include <BuildingClass.h>
#include <BuildingTypeClass.h>
#include <HouseClass.h>
#include <ScenarioClass.h>
#include <SessionClass.h>

#include <Ext/House/Body.h>
#include <Ext/Rules/Body.h>
#include <Utilities/Macro.h>

// Scenario start initialization
DEFINE_HOOK(0x6879ED, ScenarioClass_Start_NeuralAI, 0x5)
{
	NeuralAI::OnScenarioStart();

	return 0;
}

// Scenario victory & defeat hooks
DEFINE_HOOK(0x6856A5, Do_Win_NeuralAI, 0x7)
{
	NeuralAI::OnScenarioEnd(true);

	return 0;
}

DEFINE_HOOK(0x685DE7, Do_Lose_NeuralAI, 0x5)
{
	NeuralAI::OnScenarioEnd(false);

	return 0;
}

// Headless Turbo Training Mode (-TRAIN_HEADLESS) hooks in Main_Loop
DEFINE_HOOK(0x55D377, Main_Loop_FocusOverride_NeuralAI, 0x5)
{
	if (NeuralAI::HeadlessMode)
	{
		Game::IsFocused = true;
		R->AL(1);
	}

	return 0;
}

DEFINE_HOOK(0x55D8ED, Main_Loop_SkipRender_NeuralAI, 0x5)
{
	enum { SkipRender = 0x55D8F7 };

	if (NeuralAI::HeadlessMode)
		return SkipRender;

	return 0;
}

DEFINE_HOOK(0x55DE9A, Main_Loop_SkipSyncDelay_NeuralAI, 0x5)
{
	enum { SkipSyncDelay = 0x55DE9F };

	if (NeuralAI::HeadlessMode)
	{
		NeuralAI::OnHeadlessFrameTick();

		return SkipSyncDelay;
	}

	return 0;
}

// Map Action 30 (Auto Base Building) enable/disable hooks for Campaign AI
DEFINE_HOOK_AGAIN(0x6DE21B, TActionClass_AutoBaseBuilding_Enable_NeuralAI, 0x7)
DEFINE_HOOK(0x6E0F02, TActionClass_AutoBaseBuilding_Enable_NeuralAI, 0x7)
{
	GET(HouseClass* const, pHouse, ESI);

	NeuralAI::OnAutoBaseBuildingTriggered(pHouse, true);

	return 0;
}

DEFINE_HOOK_AGAIN(0x6DE29F, TActionClass_AutoBaseBuilding_Disable_NeuralAI, 0x7)
DEFINE_HOOK(0x6E0F7F, TActionClass_AutoBaseBuilding_Disable_NeuralAI, 0x7)
{
	GET(HouseClass* const, pHouse, EAX);

	NeuralAI::OnAutoBaseBuildingTriggered(pHouse, false);

	return 0;
}

// Bypass legacy aimd.ini AITriggerTypes / TeamTypes / ScriptTypes in Skirmish & Multiplayer
DEFINE_HOOK(0x4F8A27, HouseClass_AI_SkipLegacyTeams_NeuralAI, 0x5)
{
	enum { SkipLegacyAITriggers = 0x4F8B08 };

	GET(HouseClass* const, pHouse, ESI);

	if (NeuralAI::IsEnabledForHouse(pHouse) && !NeuralAI::IsCampaignRestricted(pHouse))
		return SkipLegacyAITriggers;

	return 0;
}

// HouseClass::Expert_AI evaluation hook after EnemyHouseIndex update, bypassing legacy SuperWeapon_AI, AI_Raise_Money & Fire_Sale
DEFINE_HOOK(0x4FD77C, HouseClass_ExpertAI_NeuralAI, 0x5)
{
	enum { ReturnWithCustomDelay = 0x4FD927 };

	GET(HouseClass* const, pHouse, EBX);

	if (NeuralAI::IsEnabledForHouse(pHouse))
	{
		const int delay = NeuralAI::UpdateExpertAI(pHouse);
		R->EAX(delay - 0x69);

		return ReturnWithCustomDelay;
	}

	return 0;
}

// HouseClass::AI_Building production hook
DEFINE_HOOK(0x4FE3E9, HouseClass_AI_Building_NeuralAI, 0x7)
{
	enum { Epilogue = 0x4FEA48 };

	GET(HouseClass* const, pHouse, EBP);

	if (NeuralAI::IsEnabledForHouse(pHouse))
	{
		NeuralAI::UpdateBuildingProduction(pHouse);

		return Epilogue;
	}

	return 0;
}

// HouseClass::AI_Infantry production hook
DEFINE_HOOK(0x4FEEED, HouseClass_AI_Infantry_NeuralAI, 0x7)
{
	enum { Epilogue = 0x4FEEF6 };

	GET(HouseClass* const, pHouse, EBP);

	if (NeuralAI::IsEnabledForHouse(pHouse))
	{
		NeuralAI::UpdateInfantryProduction(pHouse);

		return Epilogue;
	}

	return 0;
}

// HouseClass::AI_Aircraft production hook
DEFINE_HOOK(0x4FF21D, HouseClass_AI_Aircraft_NeuralAI, 0x7)
{
	enum { Epilogue = 0x4FF226 };

	GET(HouseClass* const, pHouse, EBP);

	if (NeuralAI::IsEnabledForHouse(pHouse))
	{
		NeuralAI::UpdateAircraftProduction(pHouse);

		return Epilogue;
	}

	return 0;
}

// Spatial Tetris Base Architect placement hook in BuildingClass::Exit_Object
DEFINE_HOOK(0x444F39, BuildingClass_ExitObject_PlaceBuilding_NeuralAI, 0x6)
{
	enum { ApplyCustomCoords = 0x4451AA };

	GET(BuildingClass* const, pFactory, ESI);
	GET(BuildingClass* const, pBuilding, EDI);

	auto const pHouse = pFactory->Owner;
	if (!pHouse || !pBuilding || !NeuralAI::IsEnabledForHouse(pHouse))
		return 0;

	if (NeuralAI::IsCampaignRestricted(pHouse))
		return 0;

	const CellStruct bestCell = NeuralAI::FindSpatialTetrisBuildCell(pHouse, pBuilding->Type);
	if (bestCell == CellStruct::Empty)
		return 0;

	pHouse->ProducingBuildingTypeIndex = -1;
	const int coordX = (static_cast<int>(bestCell.X) << 8) + 128;
	const int coordY = (static_cast<int>(bestCell.Y) << 8) + 128;

	R->EAX(coordX);
	R->EBP(coordY);
	R->EBX(0);
	R->Stack<int>(0x14, 0);
	R->Stack<int>(0x1C, reinterpret_cast<int>(pBuilding->Type));
	R->Stack<int>(0x2C, coordX);
	R->Stack<int>(0x30, coordY);
	R->Stack<int>(0x34, 0);

	return ApplyCustomCoords;
}

#include "Body.h"
#include <Ext/Side/Body.h>
#include <Utilities/TemplateDef.h>
#include <FPSCounter.h>
#include <GameOptionsClass.h>
#include <HouseTypeClass.h>
#include <GameStrings.h>
#include <New/Type/RadTypeClass.h>
#include <New/Type/ShieldTypeClass.h>
#include <New/Type/LaserTrailTypeClass.h>

template<> const DWORD Extension<RulesClass>::Canary = 0x12341234;
std::unique_ptr<RulesExt::ExtData> RulesExt::Data = nullptr;

void RulesExt::Allocate(RulesClass* pThis)
{
	Data = std::make_unique<RulesExt::ExtData>(pThis);
}

void RulesExt::Remove(RulesClass* pThis)
{
	Data = nullptr;
}

void RulesExt::LoadFromINIFile(RulesClass* pThis, CCINIClass* pINI)
{
	Data->LoadFromINI(pINI);
}

void RulesExt::LoadBeforeTypeData(RulesClass* pThis, CCINIClass* pINI)
{
	RadTypeClass::LoadFromINIList(pINI);
	ShieldTypeClass::LoadFromINIList(pINI);
	LaserTrailTypeClass::LoadFromINIList(&CCINIClass::INI_Art.get());

	Data->LoadBeforeTypeData(pThis, pINI);
}

void RulesExt::LoadAfterTypeData(RulesClass* pThis, CCINIClass* pINI)
{
	if (pINI == CCINIClass::INI_Rules)
		Data->InitializeAfterTypeData(pThis);

	Data->LoadAfterTypeData(pThis, pINI);
}

void RulesExt::ExtData::InitializeConstants()
{

}

// earliest loader - can't really do much because nothing else is initialized yet, so lookups won't work
void RulesExt::ExtData::LoadFromINIFile(CCINIClass* pINI)
{

}

void RulesExt::ExtData::LoadBeforeTypeData(RulesClass* pThis, CCINIClass* pINI)
{
	RulesExt::ExtData* pData = RulesExt::Global();

	if (!pData)
		return;

	const char* sectionAITargetTypes = "AITargetTypes";
	const char* sectionAIScriptsList = "AIScriptsList";
	const char* sectionAITriggersList = "AITriggersList";
	const char* sectionAIHousesList = "AIHousesList";

	INI_EX exINI(pINI);

	this->Storage_TiberiumIndex.Read(exINI, GameStrings::General, "Storage.TiberiumIndex");
	this->InfantryGainSelfHealCap.Read(exINI, GameStrings::General, "InfantryGainSelfHealCap");
	this->UnitsGainSelfHealCap.Read(exINI, GameStrings::General, "UnitsGainSelfHealCap");
	this->JumpjetAllowLayerDeviation.Read(exINI, "JumpjetControls", "AllowLayerDeviation");
	this->UseGlobalRadApplicationDelay.Read(exINI, GameStrings::Radiation, "UseGlobalRadApplicationDelay");
	this->RadApplicationDelay_Building.Read(exINI, GameStrings::Radiation, "RadApplicationDelay.Building");
	this->RadWarhead_Detonate.Read(exINI, GameStrings::Radiation, "RadSiteWarhead.Detonate");
	this->RadHasOwner.Read(exINI, GameStrings::Radiation, "RadHasOwner");
	this->RadHasInvoker.Read(exINI, GameStrings::Radiation, "RadHasInvoker");
	this->MissingCameo.Read(pINI, GameStrings::AudioVisual, "MissingCameo");
	this->JumpjetTurnToTarget.Read(exINI, "JumpjetControls", "TurnToTarget");

	this->PlacementPreview.Read(exINI, GameStrings::AudioVisual, "PlacementPreview");
	this->PlacementPreview_Translucency.Read(exINI, GameStrings::AudioVisual, "PlacementPreview.Translucency");
	this->PlacementGrid_Translucency.Read(exINI, GameStrings::AudioVisual, "PlacementGrid.Translucency");
	this->Pips_Shield.Read(exINI, GameStrings::AudioVisual, "Pips.Shield");
	this->Pips_Shield_Background.Read(exINI, GameStrings::AudioVisual, "Pips.Shield.Background");
	this->Pips_Shield_Building.Read(exINI, GameStrings::AudioVisual, "Pips.Shield.Building");
	this->Pips_Shield_Building_Empty.Read(exINI, GameStrings::AudioVisual, "Pips.Shield.Building.Empty");
	this->Pips_SelfHeal_Infantry.Read(exINI, GameStrings::AudioVisual, "Pips.SelfHeal.Infantry");
	this->Pips_SelfHeal_Units.Read(exINI, GameStrings::AudioVisual, "Pips.SelfHeal.Units");
	this->Pips_SelfHeal_Buildings.Read(exINI, GameStrings::AudioVisual, "Pips.SelfHeal.Buildings");
	this->Pips_SelfHeal_Infantry_Offset.Read(exINI, GameStrings::AudioVisual, "Pips.SelfHeal.Infantry.Offset");
	this->Pips_SelfHeal_Units_Offset.Read(exINI, GameStrings::AudioVisual, "Pips.SelfHeal.Units.Offset");
	this->Pips_SelfHeal_Buildings_Offset.Read(exINI, GameStrings::AudioVisual, "Pips.SelfHeal.Buildings.Offset");
	this->ToolTip_Background_Color.Read(exINI, GameStrings::AudioVisual, "ToolTip.Background.Color");
	this->ToolTip_Background_Opacity.Read(exINI, GameStrings::AudioVisual, "ToolTip.Background.Opacity");
	this->ToolTip_Background_BlurSize.Read(exINI, GameStrings::AudioVisual, "ToolTip.Background.BlurSize");
	this->RadialIndicatorVisibility.Read(exINI, GameStrings::AudioVisual, "RadialIndicatorVisibility");

	this->ForbidParallelAIQueues_Aircraft.Read(exINI, "GlobalControls", "ForbidParallelAIQueues.Infantry");
	this->ForbidParallelAIQueues_Building.Read(exINI, "GlobalControls", "ForbidParallelAIQueues.Building");
	this->ForbidParallelAIQueues_Infantry.Read(exINI, "GlobalControls", "ForbidParallelAIQueues.Infantry");
	this->ForbidParallelAIQueues_Navy.Read(exINI, "GlobalControls", "ForbidParallelAIQueues.Navy");
	this->ForbidParallelAIQueues_Vehicle.Read(exINI, "GlobalControls", "ForbidParallelAIQueues.Vehicle");

	this->IronCurtain_KeptOnDeploy.Read(exINI, GameStrings::CombatDamage, "IronCurtain.KeptOnDeploy");
	this->ROF_RandomDelay.Read(exINI, GameStrings::CombatDamage, "ROF.RandomDelay");

	// Section AITargetTypes
	int itemsCount = pINI->GetKeyCount(sectionAITargetTypes);
	for (int i = 0; i < itemsCount; ++i)
	{
		DynamicVectorClass<TechnoTypeClass*> objectsList;
		char* context = nullptr;
		pINI->ReadString(sectionAITargetTypes, pINI->GetKeyName(sectionAITargetTypes, i), "", Phobos::readBuffer);

		for (char* cur = strtok_s(Phobos::readBuffer, Phobos::readDelims, &context); cur; cur = strtok_s(nullptr, Phobos::readDelims, &context))
		{
			TechnoTypeClass* buffer;
			if (Parser<TechnoTypeClass*>::TryParse(cur, &buffer))
				objectsList.AddItem(buffer);
			else
				Debug::Log("DEBUG: [AITargetTypes][%d]: Error parsing [%s]\n", AITargetTypesLists.Count, cur);
		}

		AITargetTypesLists.AddItem(objectsList);
		objectsList.Clear();
	}

	// Section AIScriptsList
	int scriptitemsCount = pINI->GetKeyCount(sectionAIScriptsList);
	for (int i = 0; i < scriptitemsCount; ++i)
	{
		DynamicVectorClass<ScriptTypeClass*> objectsList;

		char* context = nullptr;
		pINI->ReadString(sectionAIScriptsList, pINI->GetKeyName(sectionAIScriptsList, i), "", Phobos::readBuffer);

		for (char* cur = strtok_s(Phobos::readBuffer, Phobos::readDelims, &context); cur; cur = strtok_s(nullptr, Phobos::readDelims, &context))
		{
			ScriptTypeClass* pNewScript = ScriptTypeClass::Find(cur);//GameCreate<ScriptTypeClass>(cur);
			if (pNewScript)
				objectsList.AddItem(pNewScript);
		}

		AIScriptsLists.AddItem(objectsList);
		objectsList.Clear();
	}

	// Section AITriggersList
	int triggerItemsCount = pINI->GetKeyCount(sectionAITriggersList);
	for (int i = 0; i < triggerItemsCount; ++i)
	{
		DynamicVectorClass<AITriggerTypeClass*> objectsList;

		char* context = nullptr;
		pINI->ReadString(sectionAITriggersList, pINI->GetKeyName(sectionAITriggersList, i), "", Phobos::readBuffer);

		for (char *cur = strtok_s(Phobos::readBuffer, Phobos::readDelims, &context); cur; cur = strtok_s(nullptr, Phobos::readDelims, &context))
		{
			AITriggerTypeClass* pNewTrigger = AITriggerTypeClass::Find(cur);//GameCreate<AITriggerTypeClass>(cur);
			if (pNewTrigger)
				objectsList.AddItem(pNewTrigger);
		}

		AITriggersLists.AddItem(objectsList);
		objectsList.Clear();
	}

	// Section AIHousesList
	int houseItemsCount = pINI->GetKeyCount(sectionAIHousesList);
	for (int i = 0; i < houseItemsCount; ++i)
	{
		DynamicVectorClass<HouseTypeClass*> objectsList;

		char* context = nullptr;
		pINI->ReadString(sectionAIHousesList, pINI->GetKeyName(sectionAIHousesList, i), "", Phobos::readBuffer);

		for (char *cur = strtok_s(Phobos::readBuffer, Phobos::readDelims, &context); cur; cur = strtok_s(nullptr, Phobos::readDelims, &context))
		{
			HouseTypeClass* pNewHouse = HouseTypeClass::Find(cur);//GameCreate<HouseTypeClass>(cur);
			if (pNewHouse)
				objectsList.AddItem(pNewHouse);
		}

		AIHousesLists.AddItem(objectsList);
		objectsList.Clear();
	}

	this->NewTeamsSelector.Read(exINI, "AI", "NewTeamsSelector");
	this->NewTeamsSelector_SplitTriggersByCategory.Read(exINI, "AI", "NewTeamsSelector.SplitTriggersByCategory");
	this->NewTeamsSelector_EnableFallback.Read(exINI, "AI", "NewTeamsSelector.EnableFallback");
	this->NewTeamsSelector_MergeUnclassifiedCategoryWith.Read(exINI, "AI", "NewTeamsSelector.MergeUnclassifiedCategoryWith");
	this->NewTeamsSelector_UnclassifiedCategoryPercentage.Read(exINI, "AI", "NewTeamsSelector.UnclassifiedCategoryPercentage");
	this->NewTeamsSelector_GroundCategoryPercentage.Read(exINI, "AI", "NewTeamsSelector.GroundCategoryPercentage");
	this->NewTeamsSelector_AirCategoryPercentage.Read(exINI, "AI", "NewTeamsSelector.AirCategoryPercentage");
	this->NewTeamsSelector_NavalCategoryPercentage.Read(exINI, "AI", "NewTeamsSelector.NavalCategoryPercentage");

	// Section Generic Prerequisites
	FillDefaultPrerequisites();
}

// this runs between the before and after type data loading methods for rules ini
void RulesExt::ExtData::InitializeAfterTypeData(RulesClass* const pThis)
{

}

// this should load everything that TypeData is not dependant on
// i.e. InfantryElectrocuted= can go here since nothing refers to it
// but [GenericPrerequisites] have to go earlier because they're used in parsing TypeData
void RulesExt::ExtData::LoadAfterTypeData(RulesClass* pThis, CCINIClass* pINI)
{
	RulesExt::ExtData* pData = RulesExt::Global();

	if (!pData)
		return;

	INI_EX exINI(pINI);
}

bool RulesExt::DetailsCurrentlyEnabled()
{
	// not only checks for the min frame rate from the rules, but also whether
	// the low frame rate is actually desired. in that case, don't reduce.
	auto const current = FPSCounter::CurrentFrameRate();
	auto const wanted = static_cast<unsigned int>(
		60 / Math::clamp(GameOptionsClass::Instance->GameSpeed, 1, 6));

	return current >= wanted || current >= Detail::GetMinFrameRate();
}

bool RulesExt::DetailsCurrentlyEnabled(int const minDetailLevel)
{
	return GameOptionsClass::Instance->DetailLevel >= minDetailLevel
		&& DetailsCurrentlyEnabled();
}

void RulesExt::FillDefaultPrerequisites()
{
	if (RulesExt::Global()->GenericPrerequisitesNames.Count != 0)
		return;

	CCINIClass::INI_Rules;
	DynamicVectorClass<int> empty;

	RulesExt::Global()->GenericPrerequisitesNames.AddItem("POWER"); // Official index: -1
	RulesExt::Global()->GenericPrerequisites.AddItem(RulesClass::Instance->PrerequisitePower);
	RulesExt::Global()->GenericPrerequisitesNames.AddItem("FACTORY"); // -2
	RulesExt::Global()->GenericPrerequisites.AddItem(RulesClass::Instance->PrerequisiteFactory);
	RulesExt::Global()->GenericPrerequisitesNames.AddItem("BARRACKS"); // -3
	RulesExt::Global()->GenericPrerequisites.AddItem(RulesClass::Instance->PrerequisiteBarracks);
	RulesExt::Global()->GenericPrerequisitesNames.AddItem("RADAR"); // -4
	RulesExt::Global()->GenericPrerequisites.AddItem(RulesClass::Instance->PrerequisiteRadar);
	RulesExt::Global()->GenericPrerequisitesNames.AddItem("TECH"); // -5
	RulesExt::Global()->GenericPrerequisites.AddItem(RulesClass::Instance->PrerequisiteTech);
	RulesExt::Global()->GenericPrerequisitesNames.AddItem("PROC"); // -6
	RulesExt::Global()->GenericPrerequisites.AddItem(RulesClass::Instance->PrerequisiteProc);

	// If [GenericPrerequisites] is present will be added after these.
	// Also the originals can be replaced by new ones
	int genericPreqsCount = CCINIClass::INI_Rules->GetKeyCount("GenericPrerequisites");

	for (int i = 0; i < genericPreqsCount; ++i)
	{
		DynamicVectorClass<int> objectsList;
		char* context = nullptr;
		CCINIClass::INI_Rules->ReadString("GenericPrerequisites", CCINIClass::INI_Rules->GetKeyName("GenericPrerequisites", i), "", Phobos::readBuffer);
		char* name = (char*)CCINIClass::INI_Rules->GetKeyName("GenericPrerequisites", i);

		for (char* cur = strtok_s(Phobos::readBuffer, Phobos::readDelims, &context); cur; cur = strtok_s(nullptr, Phobos::readDelims, &context))
		{
			int idx = BuildingTypeClass::FindIndex(cur);
			if (idx >= 0)
				objectsList.AddItem(idx);
		}

		int index = RulesExt::Global()->GenericPrerequisitesNames.FindItemIndex(name);
		if (index < 7 && index > 0)
		{
			// Overwrites a vanilla generic prerequisite
			RulesExt::Global()->GenericPrerequisites[index] = objectsList;
		}
		else
		{
			// New generic prerequisite
			RulesExt::Global()->GenericPrerequisitesNames.AddItem(name);
			RulesExt::Global()->GenericPrerequisites.AddItem(objectsList);
		}

		objectsList.Clear();
	}
}

// =============================
// load / save

template <typename T>
void RulesExt::ExtData::Serialize(T& Stm)
{
	Stm
		.Process(this->AITargetTypesLists)
		.Process(this->AIScriptsLists)
		.Process(this->HarvesterTypes)
		.Process(this->Storage_TiberiumIndex)
		.Process(this->InfantryGainSelfHealCap)
		.Process(this->UnitsGainSelfHealCap)
		.Process(this->UseGlobalRadApplicationDelay)
		.Process(this->RadApplicationDelay_Building)
		.Process(this->RadWarhead_Detonate)
		.Process(this->RadHasOwner)
		.Process(this->RadHasInvoker)
		.Process(this->JumpjetCrash)
		.Process(this->JumpjetNoWobbles)
		.Process(this->JumpjetAllowLayerDeviation)
		.Process(this->AITargetTypesLists)
		.Process(this->AIScriptsLists)
		.Process(this->Storage_TiberiumIndex)
		.Process(this->AIHousesLists)
		.Process(this->JumpjetTurnToTarget)
		.Process(this->MissingCameo)
		.Process(this->PlacementGrid_Translucency)
		.Process(this->PlacementPreview)
		.Process(this->PlacementPreview_Translucency)
		.Process(this->Pips_Shield)
		.Process(this->Pips_Shield_Background)
		.Process(this->Pips_Shield_Building)
		.Process(this->Pips_Shield_Building_Empty)
		.Process(this->Pips_SelfHeal_Infantry)
		.Process(this->Pips_SelfHeal_Units)
		.Process(this->Pips_SelfHeal_Buildings)
		.Process(this->Pips_SelfHeal_Infantry_Offset)
		.Process(this->Pips_SelfHeal_Units_Offset)
		.Process(this->Pips_SelfHeal_Buildings_Offset)
		.Process(this->AITriggersLists)
		.Process(Phobos::Config::AllowParallelAIQueues)
		.Process(this->ForbidParallelAIQueues_Aircraft)
		.Process(this->ForbidParallelAIQueues_Building)
		.Process(this->ForbidParallelAIQueues_Infantry)
		.Process(this->ForbidParallelAIQueues_Navy)
		.Process(this->ForbidParallelAIQueues_Vehicle)
		.Process(this->IronCurtain_KeptOnDeploy)
		.Process(this->ROF_RandomDelay)
		.Process(this->ToolTip_Background_Color)
		.Process(this->ToolTip_Background_Opacity)
		.Process(this->ToolTip_Background_BlurSize)
		.Process(this->RadialIndicatorVisibility)
		.Process(this->GenericPrerequisites)
		.Process(this->GenericPrerequisitesNames)
		.Process(this->NewTeamsSelector)
		.Process(this->NewTeamsSelector_SplitTriggersByCategory)
		.Process(this->NewTeamsSelector_EnableFallback)
		.Process(this->NewTeamsSelector_MergeUnclassifiedCategoryWith)
		.Process(this->NewTeamsSelector_UnclassifiedCategoryPercentage)
		.Process(this->NewTeamsSelector_GroundCategoryPercentage)
		.Process(this->NewTeamsSelector_AirCategoryPercentage)
		.Process(this->NewTeamsSelector_NavalCategoryPercentage)
		;
}

void RulesExt::ExtData::LoadFromStream(PhobosStreamReader& Stm)
{
	Extension<RulesClass>::LoadFromStream(Stm);
	this->Serialize(Stm);
}

void RulesExt::ExtData::SaveToStream(PhobosStreamWriter& Stm)
{
	Extension<RulesClass>::SaveToStream(Stm);
	this->Serialize(Stm);
}

bool RulesExt::LoadGlobals(PhobosStreamReader& Stm)
{
	return Stm.Success();
}

bool RulesExt::SaveGlobals(PhobosStreamWriter& Stm)
{
	return Stm.Success();
}

// =============================
// container hooks

DEFINE_HOOK(0x667A1D, RulesClass_CTOR, 0x5)
{
	GET(RulesClass*, pItem, ESI);

	RulesExt::Allocate(pItem);

	return 0;
}

DEFINE_HOOK(0x667A30, RulesClass_DTOR, 0x5)
{
	GET(RulesClass*, pItem, ECX);

	RulesExt::Remove(pItem);

	return 0;
}

IStream* RulesExt::g_pStm = nullptr;

DEFINE_HOOK_AGAIN(0x674730, RulesClass_SaveLoad_Prefix, 0x6)
DEFINE_HOOK(0x675210, RulesClass_SaveLoad_Prefix, 0x5)
{
	//GET(RulesClass*, pItem, ECX);
	GET_STACK(IStream*, pStm, 0x4);

	RulesExt::g_pStm = pStm;

	return 0;
}

DEFINE_HOOK(0x678841, RulesClass_Load_Suffix, 0x7)
{
	auto buffer = RulesExt::Global();

	PhobosByteStream Stm(0);
	if (Stm.ReadBlockFromStream(RulesExt::g_pStm))
	{
		PhobosStreamReader Reader(Stm);

		if (Reader.Expect(RulesExt::ExtData::Canary) && Reader.RegisterChange(buffer))
			buffer->LoadFromStream(Reader);
	}

	return 0;
}

DEFINE_HOOK(0x675205, RulesClass_Save_Suffix, 0x8)
{
	auto buffer = RulesExt::Global();
	PhobosByteStream saver(sizeof(*buffer));
	PhobosStreamWriter writer(saver);

	writer.Expect(RulesExt::ExtData::Canary);
	writer.RegisterChange(buffer);

	buffer->SaveToStream(writer);
	saver.WriteBlockToStream(RulesExt::g_pStm);

	return 0;
}

// DEFINE_HOOK(0x52D149, InitRules_PostInit, 0x5)
// {
// 	LaserTrailTypeClass::LoadFromINIList(&CCINIClass::INI_Art.get());
// 	return 0;
// }

DEFINE_HOOK(0x668BF0, RulesClass_Addition, 0x5)
{
	GET(RulesClass*, pItem, ECX);
	GET_STACK(CCINIClass*, pINI, 0x4);

	//	RulesClass::Initialized = false;
	RulesExt::LoadFromINIFile(pItem, pINI);

	return 0;
}

DEFINE_HOOK(0x679A15, RulesData_LoadBeforeTypeData, 0x6)
{
	GET(RulesClass*, pItem, ECX);
	GET_STACK(CCINIClass*, pINI, 0x4);

	//	RulesClass::Initialized = true;
	RulesExt::LoadBeforeTypeData(pItem, pINI);

	return 0;
}

DEFINE_HOOK(0x679CAF, RulesData_LoadAfterTypeData, 0x5)
{
	RulesClass* pItem = RulesClass::Instance();
	GET(CCINIClass*, pINI, ESI);

	RulesExt::LoadAfterTypeData(pItem, pINI);

	return 0;
}

#include "Body.h"

#include <AITriggerTypeClass.h>
#include <Ext/House/Body.h>

DEFINE_HOOK(0x687C9B, ReadScenarioINI_AITeamSelector_PreloadValidTriggers, 0x7)
{
	if (!RulesExt::Global()->NewTeamsSelector)
		return 0;

	bool ignoreGlobalAITriggers = ScenarioClass::Instance->IgnoreGlobalAITriggers;

	// For each house a list will be saved with Triggers that can be used by the AI
	for (HouseClass* pHouse : HouseClass::Array)
	{
		auto pHouseExt = HouseExt::ExtMap.Find(pHouse);
		pHouseExt->AITriggers_ValidList.clear();

		int parentCountryTypeIdx = pHouse->Type->FindParentCountryIndex(); // ParentCountry can change the House in a SP map
		int houseTypeIdx = parentCountryTypeIdx >= 0 ? parentCountryTypeIdx : pHouse->Type->ArrayIndex; // Indexes in AITriggers section are 1-based
		int houseIdx = pHouse->ArrayIndex;

		int parentCountrySideTypeIdx = parentCountryTypeIdx >= 0 ? pHouse->Type->FindParentCountry()->SideIndex : pHouse->Type->SideIndex;
		int sideTypeIdx = parentCountrySideTypeIdx >= 0 ? parentCountrySideTypeIdx + 1 : pHouse->Type->SideIndex + 1; // Side indexes in AITriggers section are 1-based
		int sideIdx = pHouse->SideIndex + 1; // Side indexes in AITriggers section are 1-based -> unused variable!!

		for (int i = 0; i < AITriggerTypeClass::Array.Count; i++)
		{
			auto pTrigger = AITriggerTypeClass::Array.GetItem(i);

			// Note: In 0x41F2E0 "AITriggerTypeClass::Read_Scenario_INI(CCINIClass *a2)" the section [AITriggerTypesEnable] is readed and overwrites the trigger's IsEnabled value. Unless I read that section here or I assume I should add all of this house and in the Teams selector check if is enabled...
			if (!pTrigger || !pTrigger->Team1)// || !pTrigger->IsEnabled)// || (ignoreGlobalAITriggers && pTrigger->IsGlobal))
				continue;

			int triggerHouse = pTrigger->HouseIndex;
			int triggerSide = pTrigger->SideIndex;

			// The trigger must be compatible with the owner
			//if ((triggerHouse == -1 || houseIdx == triggerHouse) && (triggerSide == 0 || sideIdx == triggerSide))
			if ((triggerHouse == -1 || houseTypeIdx == triggerHouse) && (triggerSide == 0 || sideTypeIdx == triggerSide))
				pHouseExt->AITriggers_ValidList.push_back(i);
		}

		Debug::Log("AITeamsSelector - The house %d [%s](%s) should be able to use %d AI triggers in this map.\n", pHouse->ArrayIndex, pHouse->Type->ID, pHouse->PlainName, pHouseExt->AITriggers_ValidList.size());

		/*for (int i : pHouseExt->AITriggers_ValidList)
		{
			Debug::Log(" => %s => %s\n", AITriggerTypeClass::Array.GetItem(i)->ID, AITriggerTypeClass::Array.GetItem(i)->Name);
		}*/
	}

	return 0;
}

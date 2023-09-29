#include <AITriggerTypeClass.h>
#include <Helpers/Macro.h>
#include <CRT.h>
#include <Unsorted.h>

#include <filesystem>
#include <fstream> // Stream class to both read and write from/to files.

#include "Body.h"

#include <Ext/Rules/Body.h>
#include <Ext/Scenario/Body.h>
#include <Ext/House/Body.h>

DEFINE_HOOK(0x687C9B, ReadScenarioINI_AITeamSelector_PreloadValidTriggers, 0x7)
{
	// For each house save a list with only AI Triggers that can be used
	for (HouseClass* pHouse : *HouseClass::Array)
	{
		auto pHouseExt = HouseExt::ExtMap.Find(pHouse);
		pHouseExt->AITriggers_ValidList.clear();
		int houseIdx = pHouse->ArrayIndex;
		int sideIdx = pHouse->SideIndex + 1;

		for (int i = 0; i < AITriggerTypeClass::Array->Count; i++)
		{
			auto pTrigger = AITriggerTypeClass::Array->GetItem(i);
			if (!pTrigger)
				continue;

			int triggerHouse = pTrigger->HouseIndex;
			int triggerSide = pTrigger->SideIndex;

			// The trigger must be compatible with the owner
			if ((triggerHouse == -1 || houseIdx == triggerHouse) && (triggerSide == 0 || sideIdx == triggerSide))
				pHouseExt->AITriggers_ValidList.push_back(i);
		}

		Debug::Log("House %d [%s] could use %d AI triggers in this map.\n", pHouse->ArrayIndex, pHouse->Type->ID, pHouseExt->AITriggers_ValidList.size());
	}

	return 0;
}

DEFINE_HOOK(0x6879ED, AILearning_Load, 0x5)
{
	if (!RulesExt::Global()->AILearning || !SessionClass::IsSingleplayer())
		return 0;

	// If "only supported maps" the tag "AILearning.ScenarioName" must containg a valid filename string
	if (RulesExt::Global()->AILearning_OnlySupportedMaps.Get()
		&& RulesExt::Global()->AILearning_ScenarioName.length() == 0)
	{
		return 0;
	}

	// Save original AI Trigger values for comparations in the save process
	if (RulesExt::Global()->AILearning_Weight_Increment.isset() || RulesExt::Global()->AILearning_Weight_Decrement.isset())
	{
		for (int i = 0; i < AITriggerTypeClass::Array->Count; i++)
		{
			ScenarioExt::Global()->AITriggerWeigths.push_back(AITriggerTypeClass::Array->GetItem(i)->Weight_Current);
		}
	}
	
	std::string fileName = "./AI/";

	if (RulesExt::Global()->AILearning_ScenarioName.length() > 0)
		fileName += RulesExt::Global()->AILearning_ScenarioName.c_str();
	else
		fileName += std::string(ScenarioClass::Instance->FileName); // Caution with XNAClient: all multiplayer maps are called "spawnmap.ini"

	std::string line;
	std::ifstream file;
	file.open(fileName);

	if (file.is_open())
	{
		while (getline(file, line))
		{
			const char* triggerID = nullptr;
			char* value = nullptr;

			// Format: <AITriggerTypeClass ID>=<double, between 0 and 5000.0>
			strcpy_s(Phobos::readBuffer, line.c_str());
			triggerID = strtok_s(Phobos::readBuffer, "=", &value);
			double current_weight = std::atof(value);

			// Modify the default game value with the loaded one
			int index = AITriggerTypeClass::FindIndex(triggerID);
			if (index >= 0)
				AITriggerTypeClass::Array->GetItem(index)->Weight_Current = current_weight;
		}

		file.close();
	}
	else
	{
		Debug::Log("AI Learning - file %s not found or can not be opened.\n", fileName.c_str());
	}

	return 0;
}

DEFINE_HOOK_AGAIN(0x6856A5, AILearning_Save, 0x7) // void Do_Win(void)
DEFINE_HOOK(0x685DE7, AILearning_Save, 0x5) // void Do_Lose(void)
{
	if (!RulesExt::Global()->AILearning || !SessionClass::IsSingleplayer())
		return 0;

	// If "only supported maps" the tag "AILearning.ScenarioName" must containg a valid filename string
	if (RulesExt::Global()->AILearning_OnlySupportedMaps.Get()
		&& RulesExt::Global()->AILearning_ScenarioName.length() == 0)
	{
		return 0;
	}

	if (std::filesystem::create_directories("./AI"))
		Debug::Log("AI Learning - Created game folder for AI learning called \"AI\".\n");

	std::string fileName = "./AI/";
	
	if (RulesExt::Global()->AILearning_ScenarioName.length() > 0)
		fileName += RulesExt::Global()->AILearning_ScenarioName.c_str();
	else
		fileName += std::string(ScenarioClass::Instance->FileName); // Caution with XNAClient: all multiplayer maps are renamed as "spawnmap.ini"

	if (FILE* file = CRT::fopen(fileName.c_str(), "w"))
	{
		double increment = RulesExt::Global()->AILearning_Weight_Increment.isset() ? RulesExt::Global()->AILearning_Weight_Increment.Get() : 0;
		increment = increment < 0 ? 0 : increment;

		double decrement = RulesExt::Global()->AILearning_Weight_Decrement.isset() ? RulesExt::Global()->AILearning_Weight_Decrement.Get() : 0;
		decrement = decrement < 0 ? 0 : decrement;

		for (int i = 0; i < AITriggerTypeClass::Array->Count; i++)
		{
			auto pTrigger = AITriggerTypeClass::Array->GetItem(i);

			// Only limit weight if the selected AI trigger ran at least 1 time
			if (pTrigger->TimesExecuted > 0 || pTrigger->TimesCompleted > 0)
			{
				double weightMaximum = pTrigger->Weight_Maximum;
				double weightMinimum = pTrigger->Weight_Minimum;

				// If set, current weight can not be incremented more than the calculated value
				if (RulesExt::Global()->AILearning_Weight_Increment.isset())
				{
					double originalCurrentWeight = ScenarioExt::Global()->AITriggerWeigths[i];

					// Get the minimum increment value
					double newWeight = originalCurrentWeight + increment;
					newWeight = newWeight > weightMaximum ? weightMaximum : newWeight;

					// Update the current weight against the current limit
					if (pTrigger->Weight_Current > newWeight)
						pTrigger->Weight_Current = newWeight;
				}

				// If set, current weight can not be decremented more than the calculated value
				if (RulesExt::Global()->AILearning_Weight_Decrement.isset())
				{
					double originalCurrentWeight = ScenarioExt::Global()->AITriggerWeigths[i];

					// Get the maximum decrement value
					double newWeight = originalCurrentWeight - decrement;
					newWeight = newWeight < weightMinimum ? weightMinimum : newWeight;

					// Update the current weight against the current limit
					if (pTrigger->Weight_Current < newWeight)
						pTrigger->Weight_Current = newWeight;
				}

				// If set, current weight can not exceed the maximum value
				if (RulesExt::Global()->AILearning_Weight_Max.isset())
				{
					// Get the minimum maximum value
					double newWeight = RulesExt::Global()->AILearning_Weight_Max.Get();
					newWeight = newWeight < 0 ? weightMinimum : newWeight;
					newWeight = newWeight > weightMaximum ? weightMaximum : newWeight;

					// Update the current weight against the current limit
					if (pTrigger->Weight_Current > newWeight)
						pTrigger->Weight_Current = newWeight;
				}

				// If set, current weight can not exceed the minimum value
				if (RulesExt::Global()->AILearning_Weight_Min.isset())
				{
					// Get the maximum minimum value
					double newWeight = RulesExt::Global()->AILearning_Weight_Min.Get();
					newWeight = newWeight < 0 ? weightMinimum : newWeight;
					newWeight = newWeight < weightMinimum ? weightMinimum : newWeight;

					// Update the current weight against the current limit
					if (pTrigger->Weight_Current < newWeight)
						pTrigger->Weight_Current = newWeight;
				}
			}

			std::string line = std::string(pTrigger->ID) + '=' + std::to_string(pTrigger->Weight_Current) + "\n";
			CRT::fwrite(line.c_str(), sizeof(char), line.size(), file);
		}

		CRT::fclose(file);
	}
	else
	{
		Debug::Log("AI Learning - The AI state can not be saved in the the file %s\n", fileName);
	}

	return 0;
}

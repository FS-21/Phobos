#include "Body.h"

#include <filesystem>
#include <fstream>
#include <iomanip>
#include <map>
#include <set>
#include <string>
#include <vector>
#include <algorithm>

#include <Ext/Rules/Body.h>
#include <Ext/Event/Body.h>
#include <AITriggerTypeClass.h>
#include <HouseClass.h>
#include <HouseTypeClass.h>
#include <ScenarioClass.h>
#include <SessionClass.h>

namespace
{
	const char* GetDifficultyString(AIDifficulty difficulty)
	{
		switch (difficulty)
		{
		case AIDifficulty::Easy:
			return "Easy";
		case AIDifficulty::Normal:
			return "Medium";
		case AIDifficulty::Hard:
			return "Hard";
		default:
			return "Hard";
		}
	}

	int GetStartingSpotIndex(HouseClass* pHouse)
	{
		if (!pHouse)
			return 0;

		if (pHouse->StartingPoint >= 0 && pHouse->StartingPoint < 32)
			return pHouse->StartingPoint;

		auto& waypoints = ScenarioExt::Global()->Waypoints;
		for (const auto& pair : waypoints)
		{
			if (pair.second.X == pHouse->StartingCell.X && pair.second.Y == pHouse->StartingCell.Y)
				return pair.first;
		}

		return pHouse->ArrayIndex >= 0 ? pHouse->ArrayIndex : 0;
	}

	bool IsTriggerCompatible(AITriggerTypeClass* pTrigger, HouseClass* pHouse)
	{
		if (!pTrigger || !pHouse)
			return false;

		if (pTrigger->OwnerHouseType == AITriggerHouseType::Single)
		{
			if (pTrigger->HouseIndex != -1 && pTrigger->HouseIndex != pHouse->Type->ArrayIndex)
				return false;
		}

		if (pTrigger->SideIndex != -1 && pTrigger->SideIndex != pHouse->Type->SideIndex)
			return false;

		if (pHouse->AIDifficulty == AIDifficulty::Easy && !pTrigger->Enabled_Easy)
			return false;
		if (pHouse->AIDifficulty == AIDifficulty::Normal && !pTrigger->Enabled_Normal)
			return false;
		if (pHouse->AIDifficulty == AIDifficulty::Hard && !pTrigger->Enabled_Hard)
			return false;

		return true;
	}

	void ReadAIMapINI(const std::string& fileName, std::map<std::string, std::map<std::string, double>>& iniData)
	{
		iniData.clear();
		std::ifstream file(fileName);
		if (!file.is_open())
			return;

		std::string line;
		std::string currentSection = "";

		while (std::getline(file, line))
		{
			size_t first = line.find_first_not_of(" \t\r\n");
			if (first == std::string::npos)
				continue;
			size_t last = line.find_last_not_of(" \t\r\n");
			line = line.substr(first, (last - first + 1));

			if (line.empty() || line[0] == ';' || line[0] == '#')
				continue;

			if (line.front() == '[' && line.back() == ']')
			{
				currentSection = line.substr(1, line.length() - 2);
				continue;
			}

			size_t eqPos = line.find('=');
			if (eqPos != std::string::npos && !currentSection.empty())
			{
				std::string key = line.substr(0, eqPos);
				std::string valStr = line.substr(eqPos + 1);

				size_t kFirst = key.find_first_not_of(" \t");
				size_t kLast = key.find_last_not_of(" \t");
				if (kFirst != std::string::npos)
					key = key.substr(kFirst, (kLast - kFirst + 1));

				size_t vFirst = valStr.find_first_not_of(" \t");
				size_t vLast = valStr.find_last_not_of(" \t");
				if (vFirst != std::string::npos)
					valStr = valStr.substr(vFirst, (vLast - vFirst + 1));

				try
				{
					double val = std::stod(valStr);
					iniData[currentSection][key] = val;
				}
				catch (...)
				{
				}
			}
		}
		file.close();
	}

	bool WriteAIMapINI(const std::string& fileName, const std::map<std::string, std::map<std::string, double>>& iniData)
	{
		std::ofstream file(fileName, std::ios::trunc);
		if (!file.is_open())
			return false;

		file << "; ============================================================\n";
		file << "; Phobos AI Learning Knowledge Database\n";
		file << "; ============================================================\n\n";

		for (const auto& sectionPair : iniData)
		{
			file << "[" << sectionPair.first << "]\n";
			for (const auto& entryPair : sectionPair.second)
			{
				file << entryPair.first << "=" << std::fixed << std::setprecision(2) << entryPair.second << "\n";
			}
			file << "\n";
		}

		file.close();
		return true;
	}
}

DEFINE_HOOK(0x6879ED, AILearning_Load, 0x5)
{
	if (!RulesExt::Global()->AILearning)
		return 0;

	bool isSingle = SessionClass::IsSingleplayer();
	bool allowMulti = RulesExt::Global()->AILearning_Multiplayer.Get();

	if (!isSingle && !allowMulti)
		return 0;

	if (!isSingle && !SessionClass::Instance.Am_I_Master())
		return 0;

	if (RulesExt::Global()->AILearning_OnlySupportedMaps.Get()
		&& RulesExt::Global()->AILearning_ScenarioName.length() == 0)
	{
		return 0;
	}

	std::string fileName = "./AI/";
	if (RulesExt::Global()->AILearning_ScenarioName.length() > 0)
		fileName += RulesExt::Global()->AILearning_ScenarioName.c_str();
	else
		fileName += std::string(ScenarioClass::Instance->FileName);

	if (fileName.length() < 4 || fileName.substr(fileName.length() - 4) != ".ini")
		fileName += ".ini";

	std::map<std::string, std::map<std::string, double>> iniData;
	ReadAIMapINI(fileName, iniData);

	if (iniData.empty())
	{
		Debug::Log("AI Learning - No previous data found for %s\n", fileName.c_str());
		return 0;
	}

	double decayRate = RulesExt::Global()->AILearning_DecayRate.Get();
	decayRate = std::clamp(decayRate, 0.0, 1.0);

	std::vector<std::pair<uint16_t, float>> modifiedTriggers;

	for (int h = 0; h < HouseClass::Array.Count; ++h)
	{
		HouseClass* pHouse = HouseClass::Array.GetItem(h);
		if (!pHouse || pHouse->IsControlledByHuman() || pHouse->Type->MultiplayPassive)
			continue;

		const char* diffStr = GetDifficultyString(pHouse->AIDifficulty);
		const char* countryStr = pHouse->Type->ID;
		int spotIndex = GetStartingSpotIndex(pHouse);

		std::string spotSection = "Spot" + std::to_string(spotIndex) + "_" + countryStr + "_" + diffStr;
		std::string generalSection = std::string(countryStr) + "_" + diffStr;

		const std::map<std::string, double>* pSectionData = nullptr;

		auto itSpot = iniData.find(spotSection);
		if (itSpot != iniData.end() && !itSpot->second.empty())
		{
			pSectionData = &itSpot->second;
			Debug::Log("AI Learning - Loading knowledge from [%s] for %s\n", spotSection.c_str(), countryStr);
		}
		else
		{
			auto itGen = iniData.find(generalSection);
			if (itGen != iniData.end() && !itGen->second.empty())
			{
				pSectionData = &itGen->second;
				Debug::Log("AI Learning - Fallback knowledge from [%s] for %s\n", generalSection.c_str(), countryStr);
			}
		}

		if (!pSectionData)
			continue;

		for (const auto& entry : *pSectionData)
		{
			int triggerIdx = AITriggerTypeClass::FindIndex(entry.first.c_str());
			if (triggerIdx >= 0)
			{
				auto pTrigger = AITriggerTypeClass::Array.GetItem(triggerIdx);
				if (IsTriggerCompatible(pTrigger, pHouse))
				{
					double baseWeight = pTrigger->Weight_Current;
					double learnedWeight = entry.second;

					double adjustedWeight = (learnedWeight * (1.0 - decayRate)) + (baseWeight * decayRate);
					adjustedWeight = std::clamp(adjustedWeight, pTrigger->Weight_Minimum, pTrigger->Weight_Maximum);
					pTrigger->Weight_Current = adjustedWeight;

					if (!isSingle)
					{
						modifiedTriggers.push_back({ static_cast<uint16_t>(triggerIdx), static_cast<float>(adjustedWeight) });
					}
				}
			}
		}
	}

	if (!isSingle && !modifiedTriggers.empty())
	{
		EventExt::RaiseAILearningSync(modifiedTriggers);
	}

	return 0;
}

DEFINE_HOOK_AGAIN(0x6856A5, AILearning_Save, 0x7) // void Do_Win(void)
DEFINE_HOOK(0x685DE7, AILearning_Save, 0x5) // void Do_Lose(void)
{
	if (!RulesExt::Global()->AILearning)
		return 0;

	bool isSingle = SessionClass::IsSingleplayer();
	bool allowMulti = RulesExt::Global()->AILearning_Multiplayer.Get();

	if (!isSingle && !allowMulti)
		return 0;

	if (!isSingle && !SessionClass::Instance.Am_I_Master())
		return 0;

	if (RulesExt::Global()->AILearning_OnlySupportedMaps.Get()
		&& RulesExt::Global()->AILearning_ScenarioName.length() == 0)
	{
		return 0;
	}

	try
	{
		std::filesystem::create_directories("./AI");
	}
	catch (...)
	{
		Debug::Log("AI Learning - Failed to create directory ./AI\n");
		return 0;
	}

	std::string fileName = "./AI/";
	if (RulesExt::Global()->AILearning_ScenarioName.length() > 0)
		fileName += RulesExt::Global()->AILearning_ScenarioName.c_str();
	else
		fileName += std::string(ScenarioClass::Instance->FileName);

	if (fileName.length() < 4 || fileName.substr(fileName.length() - 4) != ".ini")
		fileName += ".ini";

	std::map<std::string, std::map<std::string, double>> iniData;
	ReadAIMapINI(fileName, iniData);

	double learningRate = RulesExt::Global()->AILearning_LearningRate.Get();
	learningRate = std::clamp(learningRate, 0.0, 1.0);

	std::set<std::pair<std::string, std::string>> updatedCountryDiffs; // pair of (countryStr, diffStr)

	for (int h = 0; h < HouseClass::Array.Count; ++h)
	{
		HouseClass* pHouse = HouseClass::Array.GetItem(h);
		if (!pHouse || pHouse->IsControlledByHuman() || pHouse->Type->MultiplayPassive)
			continue;

		const char* diffStr = GetDifficultyString(pHouse->AIDifficulty);
		const char* countryStr = pHouse->Type->ID;
		int spotIndex = GetStartingSpotIndex(pHouse);

		std::string spotSection = "Spot" + std::to_string(spotIndex) + "_" + countryStr + "_" + diffStr;

		for (int i = 0; i < AITriggerTypeClass::Array.Count; ++i)
		{
			auto pTrigger = AITriggerTypeClass::Array.GetItem(i);
			if (!pTrigger)
				continue;

			if ((pTrigger->TimesExecuted > 0 || pTrigger->TimesCompleted > 0) && IsTriggerCompatible(pTrigger, pHouse))
			{
				double prevWeight = pTrigger->Weight_Current;
				if (iniData[spotSection].count(pTrigger->ID))
				{
					prevWeight = iniData[spotSection][pTrigger->ID];
				}

				double finalWeight = pTrigger->Weight_Current;
				double newWeight = (prevWeight * (1.0 - learningRate)) + (finalWeight * learningRate);
				newWeight = std::clamp(newWeight, pTrigger->Weight_Minimum, pTrigger->Weight_Maximum);

				iniData[spotSection][pTrigger->ID] = newWeight;
			}
		}

		updatedCountryDiffs.insert({ countryStr, diffStr });
	}

	for (const auto& cdPair : updatedCountryDiffs)
	{
		const std::string& countryStr = cdPair.first;
		const std::string& diffStr = cdPair.second;
		std::string suffix = "_" + countryStr + "_" + diffStr;
		std::string genSec = countryStr + "_" + diffStr;

		std::map<std::string, std::pair<double, int>> triggerSums;

		for (const auto& secPair : iniData)
		{
			// Check if section starts with "Spot" and ends with suffix "_Country_Difficulty"
			if (secPair.first.rfind("Spot", 0) == 0)
			{
				size_t suffixPos = secPair.first.find(suffix);
				if (suffixPos != std::string::npos && suffixPos + suffix.length() == secPair.first.length())
				{
					for (const auto& tPair : secPair.second)
					{
						if (AITriggerTypeClass::FindIndex(tPair.first.c_str()) >= 0)
						{
							triggerSums[tPair.first].first += tPair.second;
							triggerSums[tPair.first].second += 1;
						}
					}
				}
			}
		}

		iniData[genSec].clear();
		for (const auto& sumPair : triggerSums)
		{
			if (sumPair.second.second > 0)
			{
				iniData[genSec][sumPair.first] = sumPair.second.first / static_cast<double>(sumPair.second.second);
			}
		}
	}

	for (auto& secPair : iniData)
	{
		for (auto it = secPair.second.begin(); it != secPair.second.end(); )
		{
			if (AITriggerTypeClass::FindIndex(it->first.c_str()) < 0)
			{
				it = secPair.second.erase(it);
			}
			else
			{
				++it;
			}
		}
	}

	if (WriteAIMapINI(fileName, iniData))
	{
		Debug::Log("AI Learning - Successfully saved knowledge database to %s\n", fileName.c_str());
	}
	else
	{
		Debug::Log("AI Learning - Failed to write knowledge file %s\n", fileName.c_str());
	}

	return 0;
}


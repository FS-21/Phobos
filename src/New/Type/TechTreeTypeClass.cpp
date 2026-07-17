#include "TechTreeTypeClass.h"

#include <HouseClass.h>

#include <Utilities/TemplateDef.h>
#include "Utilities/Debug.h"

TechTreeTypeClass* TechTreeTypeClass::GetForSide(int sideIndex)
{
	if (Array.empty())
		Debug::FatalErrorAndExit("TechTreeTypeClass::GetForSide: Array is empty!");

	for (const auto& pType : Array)
	{
		if (pType->SideIndex == sideIndex)
		{
			return pType.get();
		}
	}

	Debug::Log("TechTreeTypeClass::GetForSide: Could not find tech tree for side %d, returning tech tree 0: %s", sideIndex, Array[0]->Name.data());
	return Array[0].get();
}

TechTreeTypeClass* TechTreeTypeClass::GetAnySuitable(HouseClass* pHouse)
{
	for (const auto& pType : Array)
	{
		if (pHouse->ActiveBuildingTypes.GetItemCount(pType->BuildConst->ArrayIndex) > 0)
		{
			return pType.get();
		}
	}

	return nullptr;	
}

void TechTreeTypeClass::CalculateTotals()
{
	TotalBuildConst.clear();
	TotalBuildPower.clear();
	TotalBuildRefinery.clear();
	TotalBuildBarracks.clear();
	TotalBuildWeapons.clear();
	TotalBuildRadar.clear();
	TotalBuildHelipad.clear();
	TotalBuildNavalYard.clear();
	TotalBuildTech.clear();
	TotalBuildAdvancedPower.clear();
	TotalBuildDefense.clear();
	TotalBuildOther.clear();	TotalBuildServiceDepot.clear();
	TotalBuildSupport.clear();
	TotalBuildSupport.clear();
	TotalPostBuildOtherRandom.clear();

	for (const auto& pTree : Array)
	{
		TotalBuildConst.insert(pTree->BuildConst);
		TotalBuildPower.insert(pTree->BuildPower.begin(), pTree->BuildPower.end());
		TotalBuildRefinery.insert(pTree->BuildRefinery.begin(), pTree->BuildRefinery.end());
		TotalBuildBarracks.insert(pTree->BuildBarracks.begin(), pTree->BuildBarracks.end());
		TotalBuildWeapons.insert(pTree->BuildWeapons.begin(), pTree->BuildWeapons.end());
		TotalBuildRadar.insert(pTree->BuildRadar.begin(), pTree->BuildRadar.end());
		TotalBuildHelipad.insert(pTree->BuildHelipad.begin(), pTree->BuildHelipad.end());
		TotalBuildNavalYard.insert(pTree->BuildNavalYard.begin(), pTree->BuildNavalYard.end());
		TotalBuildTech.insert(pTree->BuildTech.begin(), pTree->BuildTech.end());
		TotalBuildAdvancedPower.insert(pTree->BuildAdvancedPower.begin(), pTree->BuildAdvancedPower.end());
		TotalBuildDefense.insert(pTree->BuildDefense.begin(), pTree->BuildDefense.end());
		TotalBuildOther.insert(pTree->BuildOther.begin(), pTree->BuildOther.end());
		TotalPreBuildOtherRandom.insert(pTree->PreBuildOtherRandom.begin(), pTree->PreBuildOtherRandom.end());
		TotalPostBuildOtherRandom.insert(pTree->PostBuildOtherRandom.begin(), pTree->PostBuildOtherRandom.end());				TotalBuildServiceDepot.insert(pTree->BuildServiceDepot.begin(), pTree->BuildServiceDepot.end());
		TotalBuildSupport.insert(pTree->BuildSupport.begin(), pTree->BuildSupport.end());
	}
	}

size_t TechTreeTypeClass::CountTotalOwnedBuildings(HouseClass* pHouse, BuildType buildType)
{
	std::set<BuildingTypeClass*>* typeList = nullptr;
	switch (buildType)
	{
	case BuildType::BuildPower:
		typeList = &TotalBuildPower;
		break;
	case BuildType::BuildRefinery:
		typeList = &TotalBuildRefinery;
		break;
	case BuildType::BuildBarracks:
		typeList = &TotalBuildBarracks;
		break;
	case BuildType::BuildWeapons:
		typeList = &TotalBuildWeapons;
		break;
	case BuildType::BuildRadar:
		typeList = &TotalBuildRadar;
		break;
	case BuildType::BuildHelipad:
		typeList = &TotalBuildHelipad;
		break;
	case BuildType::BuildNavalYard:
		typeList = &TotalBuildNavalYard;
		break;
	case BuildType::BuildTech:
		typeList = &TotalBuildTech;
		break;
	case BuildType::BuildAdvancedPower:
		typeList = &TotalBuildAdvancedPower;
		break;
	case BuildType::BuildDefense:
		typeList = &TotalBuildDefense;
		break;
	case BuildType::BuildOther:
		typeList = &TotalBuildOther;
		break;
	case BuildType::PreBuildOtherRandom:
		typeList = &TotalPreBuildOtherRandom;
		break;
	case BuildType::PostBuildOtherRandom:
		typeList = &TotalPostBuildOtherRandom;
		break;
	case BuildType::BuildServiceDepot:
		typeList = &TotalBuildServiceDepot;
		break;
	case BuildType::BuildSupport:
		typeList = &TotalBuildSupport;
		break;
	}

	size_t count = 0;
	if (buildType == BuildType::BuildRefinery)
	{
		for (const auto pBld : BuildingClass::Array)
		{
			if (pBld && pBld->IsAlive && !pBld->InLimbo && pBld->Owner == pHouse)
			{
				if (typeList->contains(pBld->Type) || pBld->Type->Refinery)
					count++;
			}
		}

		if (RulesClass::Instance->PrerequisiteProcAlternate != nullptr)
			count += pHouse->ActiveUnitTypes.GetItemCount(RulesClass::Instance->PrerequisiteProcAlternate->ArrayIndex);
	}
	else
	{
		for (const auto pBuilding : *typeList)
			count += pHouse->ActiveBuildingTypes.GetItemCount(pBuilding->ArrayIndex);
	}

	return count;
}

size_t TechTreeTypeClass::CountSideOwnedBuildings(HouseClass* pHouse, BuildType buildType) const
{
	const ValueableVector<BuildingTypeClass*>* typeList = nullptr;
	switch (buildType)
	{
	case BuildType::BuildPower:
		typeList = &this->BuildPower;
		break;
	case BuildType::BuildRefinery:
		typeList = &this->BuildRefinery;
		break;
	case BuildType::BuildBarracks:
		typeList = &this->BuildBarracks;
		break;
	case BuildType::BuildWeapons:
		typeList = &this->BuildWeapons;
		break;
	case BuildType::BuildRadar:
		typeList = &this->BuildRadar;
		break;
	case BuildType::BuildHelipad:
		typeList = &this->BuildHelipad;
		break;
	case BuildType::BuildNavalYard:
		typeList = &this->BuildNavalYard;
		break;
	case BuildType::BuildTech:
		typeList = &this->BuildTech;
		break;
	case BuildType::BuildAdvancedPower:
		typeList = &this->BuildAdvancedPower;
		break;
	case BuildType::BuildDefense:
		typeList = &this->BuildDefense;
		break;
	case BuildType::BuildOther:
		typeList = &this->BuildOther;
		break;
	case BuildType::PreBuildOtherRandom:
		typeList = &this->PreBuildOtherRandom;
		break;
	case BuildType::PostBuildOtherRandom:
		typeList = &this->PostBuildOtherRandom;
		break;
	case BuildType::BuildServiceDepot:
		typeList = &this->BuildServiceDepot;
		break;
	case BuildType::BuildSupport:
		typeList = &this->BuildSupport;
		break;
	}

	size_t count = 0;
	if (buildType == BuildType::BuildRefinery)
	{
		for (const auto pBld : BuildingClass::Array)
		{
			if (pBld && pBld->IsAlive && !pBld->InLimbo && pBld->Owner == pHouse)
			{
				if (typeList->Contains(pBld->Type) || pBld->Type->Refinery)
					count++;
			}
		}

		if (RulesClass::Instance->PrerequisiteProcAlternate != nullptr)
			count += pHouse->ActiveUnitTypes.GetItemCount(RulesClass::Instance->PrerequisiteProcAlternate->ArrayIndex);
	}
	else
	{
		for (const auto pBuilding : *typeList)
			count += pHouse->ActiveBuildingTypes.GetItemCount(pBuilding->ArrayIndex);
	}

	return count;
}

bool TechTreeTypeClass::IsSuitable(HouseClass* pHouse) const
{
	return pHouse->ActiveBuildingTypes.GetItemCount(this->BuildConst->ArrayIndex) > 0;
}

bool TechTreeTypeClass::IsCompleted(HouseClass* pHouse, std::function<bool(BuildingTypeClass*)> const& filter) const
{
	for (BuildType i = BuildType::BuildPower; i < BuildType::BuildOther; i = static_cast<BuildType>(static_cast<int>(i) + 1))
	{
		if (!GetBuildable(i, filter).empty() && CountSideOwnedBuildings(pHouse, i) < 1)
		{
			return false;
		}
	}

	for (const auto& buildOtherPair : BuildOtherCountMap)
	{
		if (filter(buildOtherPair.first) && CountSideOwnedBuildings(pHouse, BuildType::BuildOther) < static_cast<size_t>(buildOtherPair.second))
			return false;
	}

	for (const auto& buildOtherRandomPair : PreBuildOtherRandomCountMap)
	{
		if (filter(buildOtherRandomPair.first) && CountSideOwnedBuildings(pHouse, BuildType::PreBuildOtherRandom) < static_cast<size_t>(buildOtherRandomPair.second))
			return false;
	}

	for (const auto& buildOtherRandom2Pair : PostBuildOtherRandomCountMap)
	{
		if (filter(buildOtherRandom2Pair.first) && CountSideOwnedBuildings(pHouse, BuildType::PostBuildOtherRandom) < static_cast<size_t>(buildOtherRandom2Pair.second))
			return false;
	}

	return true;
}

std::vector<BuildingTypeClass*> TechTreeTypeClass::GetBuildable(BuildType buildType, std::function<bool(BuildingTypeClass*)> const& filter) const
{
	const ValueableVector<BuildingTypeClass*>* typeList = nullptr;
	switch (buildType)
	{
	case BuildType::BuildPower:
		typeList = &this->BuildPower;
		break;
	case BuildType::BuildRefinery:
		typeList = &this->BuildRefinery;
		break;
	case BuildType::BuildBarracks:
		typeList = &this->BuildBarracks;
		break;
	case BuildType::BuildWeapons:
		typeList = &this->BuildWeapons;
		break;
	case BuildType::BuildRadar:
		typeList = &this->BuildRadar;
		break;
	case BuildType::BuildHelipad:
		typeList = &this->BuildHelipad;
		break;
	case BuildType::BuildNavalYard:
		typeList = &this->BuildNavalYard;
		break;
	case BuildType::BuildTech:
		typeList = &this->BuildTech;
		break;
	case BuildType::BuildAdvancedPower:
		typeList = &this->BuildAdvancedPower;
		break;
	case BuildType::BuildDefense:
		typeList = &this->BuildDefense;
		break;
	case BuildType::BuildOther:
		typeList = &this->BuildOther;
		break;
	case BuildType::PreBuildOtherRandom:
		typeList = &this->PreBuildOtherRandom;
		break;
	case BuildType::PostBuildOtherRandom:
		typeList = &this->PostBuildOtherRandom;
		break;
	case BuildType::BuildServiceDepot:
		typeList = &this->BuildServiceDepot;
		break;
	case BuildType::BuildSupport:
		typeList = &this->BuildSupport;
		break;
	}

	std::vector<BuildingTypeClass*> filtered;
	std::ranges::copy_if(*typeList, std::back_inserter(filtered), filter);
	return filtered;
}

BuildingTypeClass* TechTreeTypeClass::GetRandomBuildable(BuildType buildType, std::function<bool(BuildingTypeClass*)> const& filter) const
{
	std::vector<BuildingTypeClass*> buildable = GetBuildable(buildType, filter);
	if (buildable.empty())
	{
		return nullptr;
	}

	return buildable[ScenarioClass::Instance->Random.RandomRanged(0, buildable.size() - 1)];
}

template<>
const char* Enumerable<TechTreeTypeClass>::GetMainSection()
{
	return "TechTreeTypes";
}

void TechTreeTypeClass::LoadFromINI(CCINIClass* pINI)
{
	const char* section = this->Name;

	INI_EX exINI(pINI);

	this->SideIndex.Read(exINI, section, "SideIndex");
	this->BuildConst.Read(exINI, section, "BuildConst");
	this->BuildPower.Read(exINI, section, "BuildPower");
	this->BuildRefinery.Read(exINI, section, "BuildRefinery");
	this->BuildBarracks.Read(exINI, section, "BuildBarracks");
	this->BuildWeapons.Read(exINI, section, "BuildWeapons");
	this->BuildRadar.Read(exINI, section, "BuildRadar");
	this->BuildHelipad.Read(exINI, section, "BuildHelipad");
	this->BuildNavalYard.Read(exINI, section, "BuildNavalYard");
	this->BuildTech.Read(exINI, section, "BuildTech");
	this->BuildAdvancedPower.Read(exINI, section, "BuildAdvancedPower");
	this->BuildDefense.Read(exINI, section, "BuildDefense");
	this->BuildOther.Read(exINI, section, "BuildOther");
	this->PreBuildOtherRandom.Read(exINI, section, "PreBuildOtherRandom");
	this->PostBuildOtherRandom.Read(exINI, section, "PostBuildOtherRandom");		this->BuildServiceDepot.Read(exINI, section, "BuildServiceDepot");
	this->BuildSupport.Read(exINI, section, "BuildSupport");
	this->BuildOtherCounts.Read(exINI, section, "BuildOtherCounts");
	this->PreBuildOtherRandomCounts.Read(exINI, section, "PreBuildOtherRandomCounts");
	this->PostBuildOtherRandomCounts.Read(exINI, section, "PostBuildOtherRandomCounts");
	this->LimitedFactories.Read(exINI, section, "LimitedFactories");

	for (size_t i = 0; i < BuildOther.size(); i++)
	{
		if (i < BuildOtherCounts.size())
		{
			BuildOtherCountMap[BuildOther[i]] = BuildOtherCounts[i];
		}
		else
		{
			BuildOtherCountMap[BuildOther[i]] = 1;
		}
	}

	for (size_t i = 0; i < PreBuildOtherRandom.size(); i++)
	{
		if (i < PreBuildOtherRandomCounts.size())
		{
			PreBuildOtherRandomCountMap[PreBuildOtherRandom[i]] = PreBuildOtherRandomCounts[i];
		}
		else
		{
			PreBuildOtherRandomCountMap[PreBuildOtherRandom[i]] = 1;
		}
	}

	for (size_t i = 0; i < PostBuildOtherRandom.size(); i++)
	{
		if (i < PostBuildOtherRandomCounts.size())
		{
			PostBuildOtherRandomCountMap[PostBuildOtherRandom[i]] = PostBuildOtherRandomCounts[i];
		}
		else
		{
			PostBuildOtherRandomCountMap[PostBuildOtherRandom[i]] = 1;
		}
	}
}

template <typename T>
void TechTreeTypeClass::Serialize(T& Stm)
{
	Stm
		.Process(SideIndex)
		.Process(BuildConst)
		.Process(BuildPower)
		.Process(BuildRefinery)
		.Process(BuildBarracks)
		.Process(BuildWeapons)
		.Process(BuildRadar)
		.Process(BuildHelipad)
		.Process(BuildNavalYard)
		.Process(BuildTech)
		.Process(BuildAdvancedPower)
		.Process(BuildDefense)
		.Process(BuildOther)
		.Process(PreBuildOtherRandom)
		.Process(PostBuildOtherRandom)				.Process(BuildServiceDepot)
		.Process(BuildSupport)
		.Process(BuildOtherCounts)
		.Process(PreBuildOtherRandomCounts)
		.Process(PostBuildOtherRandomCounts)
		.Process(LimitedFactories)
		;
}

void TechTreeTypeClass::LoadFromStream(PhobosStreamReader& Stm)
{
	this->Serialize(Stm);

	BuildOtherCountMap.clear();
	for (size_t i = 0; i < BuildOther.size(); i++)
	{
		if (i < BuildOtherCounts.size())
		{
			BuildOtherCountMap[BuildOther[i]] = BuildOtherCounts[i];
		}
		else
		{
			BuildOtherCountMap[BuildOther[i]] = 1;
		}
	}

	PreBuildOtherRandomCountMap.clear();
	for (size_t i = 0; i < PreBuildOtherRandom.size(); i++)
	{
		if (i < PreBuildOtherRandomCounts.size())
		{
			PreBuildOtherRandomCountMap[PreBuildOtherRandom[i]] = PreBuildOtherRandomCounts[i];
		}
		else
		{
			PreBuildOtherRandomCountMap[PreBuildOtherRandom[i]] = 1;
		}
	}

	PostBuildOtherRandomCountMap.clear();
	for (size_t i = 0; i < PostBuildOtherRandom.size(); i++)
	{
		if (i < PostBuildOtherRandomCounts.size())
		{
			PostBuildOtherRandomCountMap[PostBuildOtherRandom[i]] = PostBuildOtherRandomCounts[i];
		}
		else
		{
			PostBuildOtherRandomCountMap[PostBuildOtherRandom[i]] = 1;
		}
	}
}

void TechTreeTypeClass::SaveToStream(PhobosStreamWriter& Stm)
{
	this->Serialize(Stm);
}
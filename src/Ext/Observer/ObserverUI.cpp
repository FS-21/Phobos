#include "ObserverUI.h"

#include <Ext/TechnoType/Body.h>
#include <Ext/Techno/Body.h>
#include <Ext/SWType/Body.h>
#include <Ext/BuildingType/Body.h>
#include <InfantryClass.h>
#include <UnitClass.h>
#include <AircraftClass.h>
#include <BuildingClass.h>
#include <Utilities/Constructs.h>
#include <Fundamentals.h>
#include <ColorScheme.h>
#include <BitFont.h>
#include <BitText.h>
#include <WWMouseClass.h>
#include <MouseClass.h>
#include <Drawing.h>
#include <StringTable.h>
#include <RulesClass.h>
#include <New/Entity/ShieldClass.h>
#include <PCX.h>

#include <algorithm>
#include <sstream>
#include <windows.h>

ObserverUIClass ObserverUIClass::Instance;

static bool IntersectRect(const RectangleStruct& r1, const RectangleStruct& r2, RectangleStruct& out)
{
	int left = std::max(r1.X, r2.X);
	int top = std::max(r1.Y, r2.Y);
	int right = std::min(r1.X + r1.Width, r2.X + r2.Width);
	int bottom = std::min(r1.Y + r1.Height, r2.Y + r2.Height);

	if (left < right && top < bottom)
	{
		out = RectangleStruct { left, top, right - left, bottom - top };
		return true;
	}
	return false;
}

static int GetFactoryProgressPercent(FactoryClass* pFact)
{
	if (!pFact || !pFact->Object)
		return 0;

	int rate = pFact->Production.Rate;
	if (rate > 0)
	{
		int step = pFact->Production.Value; // 0 to 54
		int timeLeftInStep = pFact->Production.Timer.GetTimeLeft();
		int elapsedInStep = (timeLeftInStep >= 0 && timeLeftInStep <= rate) ? (rate - timeLeftInStep) : 0;
		int totalElapsedFrames = (step * rate) + elapsedInStep;
		int totalFrames = 54 * rate;
		return std::clamp((totalElapsedFrames * 100) / totalFrames, 0, 100);
	}

	int curProgress = pFact->GetProgress(); // 0..54
	return std::clamp((curProgress * 100) / 54, 0, 100);
}

bool ObserverUIClass::IsActive()
{
	if (!ScenarioClass::Instance || HouseClass::Array.Count == 0 || !HouseClass::CurrentPlayer)
		return false;

	return HouseClass::IsCurrentPlayerObserver()
		|| (HouseClass::Observer && HouseClass::CurrentPlayer && HouseClass::CurrentPlayer->IsObserver());
}

void ObserverUIClass::ClearData()
{
	this->DisplayMode = ObserverUIDisplayMode::Hidden;
	this->PlayerRows.clear();
	if (!Phobos::Config::DevelopmentCommands)
	{
		this->FloatingWindows.clear();
		this->FloatingUnitWindows.clear();
	}
	this->EconomyHistory.clear();
	this->CycleIndices.clear();
	this->TabButtons.clear();
	this->SearchFilterText.clear();
	this->IsSearchInputFocused = false;
	this->HoveredItem = {};
	this->HasHoveredItem = false;
	this->pHoveredPlayer = nullptr;
	this->HasHoveredPlayer = false;
	this->WasEnterPressed = false;
	this->VerticalScrollOffset = 0;
	this->MaxVerticalScrollOffset = 0;
}

std::vector<std::wstring> ObserverUIClass::ParseSearchTerms(const std::wstring& query) const
{
	std::vector<std::wstring> terms;
	std::wstring currentTerm;
	bool inQuotes = false;

	for (wchar_t ch : query)
	{
		if (ch == L'"')
		{
			if (inQuotes)
			{
				if (!currentTerm.empty())
				{
					terms.push_back(currentTerm);
					currentTerm.clear();
				}
				inQuotes = false;
			}
			else
			{
				if (!currentTerm.empty())
				{
					terms.push_back(currentTerm);
					currentTerm.clear();
				}
				inQuotes = true;
			}
		}
		else if (ch == L' ' && !inQuotes)
		{
			if (!currentTerm.empty())
			{
				terms.push_back(currentTerm);
				currentTerm.clear();
			}
		}
		else
		{
			currentTerm += ch;
		}
	}

	if (!currentTerm.empty())
	{
		terms.push_back(currentTerm);
	}

	for (auto& term : terms)
	{
		std::transform(term.begin(), term.end(), term.begin(), ::towlower);
	}

	return terms;
}

bool ObserverUIClass::MatchesSearchFilter(AbstractTypeClass* pType) const
{
	if (!pType)
		return false;

	if (pType->WhatAmI() == AbstractType::BuildingType)
	{
		auto pBType = static_cast<BuildingTypeClass*>(pType);
		if (pBType->InvisibleInGame || pBType->Invisible)
			return false;
	}
	else if (pType->WhatAmI() == AbstractType::UnitType || pType->WhatAmI() == AbstractType::InfantryType || pType->WhatAmI() == AbstractType::AircraftType)
	{
		auto pTechType = static_cast<TechnoTypeClass*>(pType);
		if (pTechType->Invisible)
			return false;
	}

	if (this->SearchFilterText.empty())
		return true;

	std::vector<std::wstring> terms = this->ParseSearchTerms(this->SearchFilterText);
	if (terms.empty())
		return true;

	std::wstring nameW = L"";
	if (pType->UIName && *pType->UIName)
	{
		nameW = pType->UIName;
	}
	else if (pType->ID)
	{
		std::string rawIdStr = pType->ID;
		nameW = std::wstring(rawIdStr.begin(), rawIdStr.end());
	}

	std::transform(nameW.begin(), nameW.end(), nameW.begin(), ::towlower);

	for (const auto& term : terms)
	{
		if (nameW.find(term) == std::wstring::npos)
		{
			return false;
		}
	}

	return true;
}

static HouseClass* GetTargetEnemy(HouseClass* pHouse)
{
	if (!pHouse)
		return nullptr;

	if (pHouse->WhoLastHurtMe >= 0 && pHouse->WhoLastHurtMe < HouseClass::Array.Count)
	{
		auto pAttacker = HouseClass::Array.GetItem(pHouse->WhoLastHurtMe);
		if (pAttacker && pAttacker != pHouse && !pAttacker->Defeated && !pHouse->IsAlliedWith(pAttacker)
			&& pAttacker != HouseClass::FindCivilianSide() && pAttacker != HouseClass::FindNeutral() && pAttacker != HouseClass::FindSpecial())
		{
			return pAttacker;
		}
	}

	if (pHouse->LAEnemy >= 0 && pHouse->LAEnemy < HouseClass::Array.Count)
	{
		auto pLAEnemy = HouseClass::Array.GetItem(pHouse->LAEnemy);
		if (pLAEnemy && pLAEnemy != pHouse && !pLAEnemy->Defeated && !pHouse->IsAlliedWith(pLAEnemy)
			&& pLAEnemy != HouseClass::FindCivilianSide() && pLAEnemy != HouseClass::FindNeutral() && pLAEnemy != HouseClass::FindSpecial())
		{
			return pLAEnemy;
		}
	}

	return nullptr;
}

static ColorStruct ConvertHSVToRGB(BYTE h, BYTE s, BYTE v)
{
	if (s == 0)
		return ColorStruct { v, v, v };

	float H = (h / 255.0f) * 360.0f;
	float S = s / 255.0f;
	float V = v / 255.0f;

	float C = V * S;
	float X = C * (1.0f - std::abs(std::fmod(H / 60.0f, 2.0f) - 1.0f));
	float m = V - C;

	float r = 0, g = 0, b = 0;
	if (H >= 0 && H < 60)       { r = C; g = X; b = 0; }
	else if (H >= 60 && H < 120)  { r = X; g = C; b = 0; }
	else if (H >= 120 && H < 180) { r = 0; g = C; b = X; }
	else if (H >= 180 && H < 240) { r = 0; g = X; b = C; }
	else if (H >= 240 && H < 300) { r = X; g = 0; b = C; }
	else                          { r = C; g = 0; b = X; }

	BYTE R = static_cast<BYTE>(std::clamp((r + m) * 255.0f, 0.0f, 255.0f));
	BYTE G = static_cast<BYTE>(std::clamp((g + m) * 255.0f, 0.0f, 255.0f));
	BYTE B = static_cast<BYTE>(std::clamp((b + m) * 255.0f, 0.0f, 255.0f));

	return ColorStruct { R, G, B };
}

static ColorStruct GetHouseColor(HouseClass* pHouse, int fallbackIdx = 0)
{
	if (pHouse)
	{
		if (pHouse->ColorSchemeIndex >= 0 && pHouse->ColorSchemeIndex < ColorScheme::Array.Count)
		{
			auto pScheme = ColorScheme::Array.GetItem(pHouse->ColorSchemeIndex);
			if (pScheme)
			{
				// Convert HSV BaseColor (H, S, V in 0..255) to 8-bit RGB
				return ConvertHSVToRGB(pScheme->BaseColor.R, pScheme->BaseColor.G, pScheme->BaseColor.B);
			}
		}
	}

	static const std::vector<ColorStruct> playerColorPalette = {
		ColorStruct { 0, 102, 255 },   // Player 1 = Blue
		ColorStruct { 255, 0, 0 },     // Player 2 = Red
		ColorStruct { 0, 255, 0 },     // Player 3 = Neon Green
		ColorStruct { 160, 32, 240 },  // Player 4 = Violet
		ColorStruct { 255, 140, 0 },   // Player 5 = Orange
		ColorStruct { 255, 255, 0 },   // Player 6 = Yellow
		ColorStruct { 255, 105, 180 }, // Player 7 = Hot Pink
		ColorStruct { 0, 255, 255 }    // Player 8 = Cyan
	};

	return playerColorPalette[fallbackIdx % playerColorPalette.size()];
}

void ObserverUIClass::CollectPlayerData()
{
	// Preserve existing scroll offsets when refreshing player rows
	std::map<HouseClass*, int> previousScrollOffsets;
	for (const auto& r : this->PlayerRows)
	{
		previousScrollOffsets[r.pHouse] = r.ScrollOffset;
	}

	this->PlayerRows.clear();

	if (!HouseClass::Array.Count)
		return;

	// Curated high-contrast palette (Hot Pink is 7th)
	static const std::vector<ColorStruct> playerColorPalette = {
		ColorStruct { 0, 102, 255 },   // Player 1 = Blue
		ColorStruct { 255, 0, 0 },     // Player 2 = Red
		ColorStruct { 0, 255, 0 },     // Player 3 = Neon Green
		ColorStruct { 160, 32, 240 },  // Player 4 = Violet
		ColorStruct { 255, 140, 0 },   // Player 5 = Orange
		ColorStruct { 255, 255, 0 },   // Player 6 = Yellow
		ColorStruct { 255, 105, 180 }, // Player 7 = Hot Pink
		ColorStruct { 0, 255, 255 }    // Player 8 = Cyan
	};

	for (int i = 0; i < HouseClass::Array.Count; ++i)
	{
		auto pHouse = HouseClass::Array.GetItem(i);
		if (!pHouse || pHouse->Defeated || !pHouse->Type)
			continue;

		// Exclude houses with MultiplayerPassive=true
		if (pHouse->Type->MultiplayPassive)
			continue;

		// Exclude neutral, civilian, defeated or observer houses
		if (pHouse->IsObserver() || pHouse->Defeated)
			continue;

		// Exclude Special & Neutral houses
		if (pHouse == HouseClass::FindNeutral() || pHouse == HouseClass::FindSpecial() || pHouse == HouseClass::FindCivilianSide())
			continue;

		ObserverPlayerRow row;
		row.pHouse = pHouse;
		row.PlayerNumber = pHouse->ArrayIndex + 1; // Direct slot in HouseClass::Array (Base 1)

		// Convert PlainName (char[]) to PlayerName (wstring)
		std::string plainNameStr = pHouse->PlainName;
		if (plainNameStr.empty())
			plainNameStr = pHouse->get_ID();

		std::wstring wPlainName(plainNameStr.begin(), plainNameStr.end());
		bool isMultiplayer = SessionClass::Instance.GameMode == GameMode::Skirmish || SessionClass::Instance.GameMode == GameMode::LAN || SessionClass::Instance.GameMode == GameMode::Internet;
		if (isMultiplayer)
		{
			row.PlayerName = L"P" + std::to_wstring(row.PlayerNumber) + L": " + wPlainName;
		}
		else
		{
			row.PlayerName = wPlainName;
		}
		row.CountryName = pHouse->Type->UIName;

		// Calculate economy rate per minute (+- $X/min) based on rolling sample history
		row.IncomeRatePerMin = 0;
		auto itHist = this->EconomyHistory.find(pHouse);
		if (itHist != this->EconomyHistory.end() && !itHist->second.empty())
		{
			const auto& history = itHist->second;
			const auto& latest = history.back();
			const auto& oldest = history.front();

			int frameDiff = latest.Frame - oldest.Frame;
			if (frameDiff > 0)
			{
				float secondsDiff = frameDiff / 15.0f; // 15 FPS in YR logic engine
				int moneyDiff = latest.Money - oldest.Money;
				if (secondsDiff >= 1.0f)
				{
					row.IncomeRatePerMin = static_cast<int>((moneyDiff / secondsDiff) * 60.0f);
				}
			}
		}

		row.TargetEnemy = GetTargetEnemy(pHouse);
		if (row.TargetEnemy)
		{
			std::string enemyNameStr = row.TargetEnemy->PlainName;
			if (enemyNameStr.empty()) enemyNameStr = row.TargetEnemy->get_ID();
			row.TargetEnemyName = std::wstring(enemyNameStr.begin(), enemyNameStr.end());
		}
		else
		{
			row.TargetEnemyName = L"None";
		}

		// Restore previous scroll offset if valid
		if (previousScrollOffsets.count(pHouse) > 0)
		{
			row.ScrollOffset = previousScrollOffsets[pHouse];
		}

		// Assign actual player house ColorScheme BaseColor
		row.PlayerColor = GetHouseColor(pHouse, row.PlayerNumber - 1);

		// Collect active factory production for this player from FactoryClass::Array grouped by TechnoType
		std::map<TechnoTypeClass*, std::vector<BuildingClass*>> prodGroupMap;
		std::map<TechnoTypeClass*, int> prodProgressMap;

		for (auto const pFact : FactoryClass::Array)
		{
			if (!pFact || pFact->Owner != pHouse || !pFact->Object)
				continue;

			auto const pProducingType = pFact->Object->GetTechnoType();
			if (!pProducingType || !this->MatchesSearchFilter(pProducingType))
				continue;

			int progressPercent = GetFactoryProgressPercent(pFact);

			BuildingClass* pBld = nullptr;
			for (auto const b : pHouse->Buildings)
			{
				if (b && b->Factory == pFact)
				{
					pBld = b;
					break;
				}
			}

			prodGroupMap[pProducingType].push_back(pBld);
			prodProgressMap[pProducingType] = std::max(prodProgressMap[pProducingType], progressPercent);
		}

		for (auto const& [pType, factoryList] : prodGroupMap)
		{
			ObserverCameoItem prodItem;
			prodItem.pType = pType;
			prodItem.ProgressPercent = prodProgressMap[pType];
			prodItem.IsProduction = true;
			prodItem.pOwner = pHouse;
			prodItem.Buildings = factoryList;

			row.ProductionItems.emplace_back(prodItem);
		}

		// Group and filter owned technos based on active filter tab
		std::map<TechnoTypeClass*, std::vector<TechnoClass*>> filterGroupMap;

		auto checkAndAddTechno = [&](TechnoClass* pTechno) {
			if (!pTechno || !pTechno->GetTechnoType() || !pTechno->IsAlive || pTechno->InLimbo)
				return;

			auto pType = pTechno->GetTechnoType();
			if (!this->MatchesSearchFilter(pType))
				return;

			bool include = false;

			AbstractType absType = pTechno->WhatAmI();

			switch (this->ActiveFilterTab)
			{
			case ObserverFilterCategory::Defenses:
				if (absType == AbstractType::Building)
				{
					auto pBldType = static_cast<BuildingTypeClass*>(pType);
					if (pBldType->IsBaseDefense)
						include = true;
				}
				break;

			case ObserverFilterCategory::Structures:
				if (absType == AbstractType::Building)
				{
					auto pBldType = static_cast<BuildingTypeClass*>(pType);
					if (!pBldType->IsBaseDefense)
						include = true;
				}
				break;

			case ObserverFilterCategory::AllStructures:
				if (absType == AbstractType::Building)
					include = true;
				break;

			case ObserverFilterCategory::Infantry:
				if (absType == AbstractType::Infantry)
					include = true;
				break;

			case ObserverFilterCategory::Vehicles:
				if (absType == AbstractType::Unit)
				{
					bool isNaval = pType->Naval;
					bool isAircraft = pType->ConsideredAircraft;
					if (!isNaval && !isAircraft)
						include = true;
				}
				break;

			case ObserverFilterCategory::Naval:
				if (pType->Naval)
					include = true;
				break;

			case ObserverFilterCategory::Aircraft:
				if (absType == AbstractType::Aircraft || pType->ConsideredAircraft)
					include = true;
				break;

			case ObserverFilterCategory::AllUnits:
				if (absType != AbstractType::Building)
					include = true;
				break;

			case ObserverFilterCategory::Everything:
				include = true;
				break;
			}

			if (include)
			{
				filterGroupMap[pType].push_back(pTechno);
			}
		};

		// Collect from BuildingClass::Array
		if (this->ActiveFilterTab == ObserverFilterCategory::Defenses
			|| this->ActiveFilterTab == ObserverFilterCategory::Structures
			|| this->ActiveFilterTab == ObserverFilterCategory::AllStructures
			|| this->ActiveFilterTab == ObserverFilterCategory::Naval
			|| this->ActiveFilterTab == ObserverFilterCategory::Everything)
		{
			for (auto const pBld : BuildingClass::Array)
			{
				if (pBld && pBld->Owner == pHouse)
				{
					checkAndAddTechno(pBld);
				}
			}
		}

		// Collect from InfantryClass::Array
		if (this->ActiveFilterTab == ObserverFilterCategory::Infantry
			|| this->ActiveFilterTab == ObserverFilterCategory::Naval
			|| this->ActiveFilterTab == ObserverFilterCategory::AllUnits
			|| this->ActiveFilterTab == ObserverFilterCategory::Everything)
		{
			for (auto const pInf : InfantryClass::Array)
			{
				if (pInf && pInf->Owner == pHouse)
				{
					checkAndAddTechno(pInf);
				}
			}
		}

		// Collect from UnitClass::Array
		if (this->ActiveFilterTab == ObserverFilterCategory::Vehicles
			|| this->ActiveFilterTab == ObserverFilterCategory::Naval
			|| this->ActiveFilterTab == ObserverFilterCategory::Aircraft
			|| this->ActiveFilterTab == ObserverFilterCategory::AllUnits
			|| this->ActiveFilterTab == ObserverFilterCategory::Everything)
		{
			for (auto const pUnit : UnitClass::Array)
			{
				if (pUnit && pUnit->Owner == pHouse)
				{
					checkAndAddTechno(pUnit);
				}
			}
		}

		// Collect from AircraftClass::Array
		if (this->ActiveFilterTab == ObserverFilterCategory::Aircraft
			|| this->ActiveFilterTab == ObserverFilterCategory::AllUnits
			|| this->ActiveFilterTab == ObserverFilterCategory::Everything)
		{
			for (auto const pAir : AircraftClass::Array)
			{
				if (pAir && pAir->Owner == pHouse)
				{
					checkAndAddTechno(pAir);
				}
			}
		}

		// Collect Superweapons if Superweapons tab is selected
		if (this->ActiveFilterTab == ObserverFilterCategory::Superweapons)
		{
			for (int s = 0; s < pHouse->Supers.Count; ++s)
			{
				auto pSuper = pHouse->Supers.GetItem(s);
				if (!pSuper || !pSuper->Type || !this->MatchesSearchFilter(pSuper->Type))
					continue;

				// Check Phobos SWTypeExt visibility settings
				const auto pSWExt = SWTypeExt::ExtMap.Find(pSuper->Type);
				if (pSWExt && !pSWExt->SW_ShowCameo && pSWExt->SW_AutoFire)
					continue;

				ObserverCameoItem item;
				item.pSuperType = pSuper->Type;
				item.pSuper = pSuper;
				item.IsSuperweapon = true;
				item.pOwner = pHouse;
				item.Count = 1;

				for (int b = 0; b < BuildingClass::Array.Count; ++b)
				{
					auto pBldObj = BuildingClass::Array.GetItem(b);
					if (pBldObj && pBldObj->Owner == pHouse && pBldObj->IsAlive && !pBldObj->InLimbo && pBldObj->Type)
					{
						bool grantsSW = false;
						if (pBldObj->Type->SuperWeapon == pSuper->Type->ArrayIndex || pBldObj->Type->SuperWeapon2 == pSuper->Type->ArrayIndex)
						{
							grantsSW = true;
						}

						auto pBldExt = BuildingTypeExt::ExtMap.Find(pBldObj->Type);
						if (pBldExt)
						{
							for (int swIdx : pBldExt->SuperWeapons)
							{
								if (swIdx == pSuper->Type->ArrayIndex)
								{
									grantsSW = true;
									break;
								}
							}
						}

						if (grantsSW)
						{
							item.Buildings.push_back(pBldObj);
						}
					}
				}

				if (!pSuper->IsPresent && !pSuper->IsReady && !pSuper->IsOneTime && item.Buildings.empty())
					continue;

				int totalFrames = pSuper->GetRechargeTime();
				int framesLeft = pSuper->RechargeTimer.GetTimeLeft();
				if (totalFrames > 0 && framesLeft > 0)
				{
					item.ProgressPercent = std::clamp(((totalFrames - framesLeft) * 100) / totalFrames, 0, 100);
				}
				else
				{
					item.ProgressPercent = 100;
				}

				row.StructureItems.emplace_back(item);
			}
		}

		for (auto const& [pType, technoList] : filterGroupMap)
		{
			ObserverCameoItem item;
			item.pType = pType;
			item.Count = static_cast<int>(technoList.size());
			item.IsProduction = false;
			item.pOwner = pHouse;

			for (auto const pTech : technoList)
			{
				if (pTech->WhatAmI() == AbstractType::BuildingType)
				{
					item.Buildings.push_back(static_cast<BuildingClass*>(pTech));
				}
				item.Technos.push_back(pTech);
			}

			row.StructureItems.emplace_back(item);
		}

		this->PlayerRows.emplace_back(row);
	}

	// Detect mutual alliance teams ONLY for 2 or more mutually allied players
	std::vector<int> assignedTeams(this->PlayerRows.size(), -1);
	std::vector<int> teamMemberCounts(this->PlayerRows.size(), 0);

	int currentTeamID = 0;
	std::map<int, ColorStruct> teamColorMap;
	std::map<int, int> teamCountMap;

	for (size_t i = 0; i < this->PlayerRows.size(); ++i)
	{
		if (assignedTeams[i] != -1)
			continue;

		std::vector<size_t> teamMembers = { i };
		for (size_t j = i + 1; j < this->PlayerRows.size(); ++j)
		{
			if (assignedTeams[j] == -1 && this->PlayerRows[i].pHouse->IsMutualAlly(this->PlayerRows[j].pHouse))
			{
				teamMembers.push_back(j);
			}
		}

		// Team alliance line is rendered ONLY if 2 or more players are mutually allied!
		if (teamMembers.size() >= 2)
		{
			int teamID = currentTeamID++;
			ColorStruct teamColor = playerColorPalette[teamID % playerColorPalette.size()];
			teamColorMap[teamID] = teamColor;
			teamCountMap[teamID] = static_cast<int>(teamMembers.size());

			for (size_t idx : teamMembers)
			{
				assignedTeams[idx] = teamID;
			}
		}
	}

	for (size_t i = 0; i < this->PlayerRows.size(); ++i)
	{
		this->PlayerRows[i].TeamID = assignedTeams[i];
		if (assignedTeams[i] != -1)
		{
			this->PlayerRows[i].TeamColor = teamColorMap[assignedTeams[i]];
			this->PlayerRows[i].TeamMemberCount = teamCountMap[assignedTeams[i]];
		}
		else
		{
			this->PlayerRows[i].TeamMemberCount = 1;
		}
	}

	// Stable sort rows so allied team members are grouped adjacently
	std::stable_sort(this->PlayerRows.begin(), this->PlayerRows.end(), [](const ObserverPlayerRow& a, const ObserverPlayerRow& b) {
		if (a.TeamID != b.TeamID)
		{
			if (a.TeamID == -1) return false;
			if (b.TeamID == -1) return true;
			return a.TeamID < b.TeamID;
		}
		return false;
	});
}

void ObserverUIClass::Update()
{
	bool isActive = IsActive() || (Phobos::Config::DevelopmentCommands && (this->DisplayMode != ObserverUIDisplayMode::Hidden || this->HasFloatingWindows()));
	if (!isActive)
	{
		if (!this->PlayerRows.empty() || !this->EconomyHistory.empty())
		{
			this->ClearData();
		}
		return;
	}

	// Check ENTER key press: If no hotkey is assigned to ObjectInfo in the key options menu, ENTER acts as default key for observers
	bool isEnterPressed = (GetAsyncKeyState(VK_RETURN) & 0x8000) != 0;
	if (isEnterPressed && !this->WasEnterPressed)
	{
		if (this->IsSearchInputFocused)
		{
			// Defocus search box ONLY, do NOT toggle card window!
			this->IsSearchInputFocused = false;
		}
		else
		{
			// If Show Object Card hotkey is unassigned, ENTER opens card for hovered/selected object
			bool cardOpened = false;
			if (!IsShowObjectCardHotkeyBound())
			{
				cardOpened = this->OpenFloatingWindowForSelectedObject();
			}

			// If no card was opened and Toggle Observer UI hotkey is unassigned, ENTER toggles Observer UI display mode
			if (!cardOpened && !IsToggleObserverUIHotkeyBound())
			{
				this->ToggleDisplayMode();
			}
		}
	}
	this->WasEnterPressed = isEnterPressed;

	if (this->DisplayMode == ObserverUIDisplayMode::Hidden && !this->HasFloatingWindows())
		return;

	if (this->IsMouseHoveringUI() || this->IsSearchFocused())
	{
		MouseClass::Instance.UpdateCursor(MouseCursorType::Default, false);
	}

	// Update dragging position of floating card windows
	Point2D mousePos = { 0, 0 };
	if (WWMouseClass::Instance)
	{
		mousePos = Point2D { WWMouseClass::Instance->GetX(), WWMouseClass::Instance->GetY() };
	}

	bool isLeftPressed = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
	bool anyDragging = false;

	for (auto& win : this->FloatingWindows)
	{
		if (win.IsDragging)
		{
			if (isLeftPressed)
			{
				win.Position.X = mousePos.X - win.DragOffset.X;
				win.Position.Y = mousePos.Y - win.DragOffset.Y;

				int screenW = DSurface::Composite ? DSurface::Composite->Width : 1024;
				int screenH = DSurface::Composite ? DSurface::Composite->Height : 768;
				win.Position.X = std::max(0, std::min(win.Position.X, std::max(0, screenW - win.WindowRect.Width)));
				win.Position.Y = std::max(0, std::min(win.Position.Y, std::max(0, screenH - win.WindowRect.Height)));
				anyDragging = true;
			}
			else
			{
				win.IsDragging = false;
			}
		}
	}

	for (auto& win : this->FloatingUnitWindows)
	{
		if (win.IsDragging)
		{
			if (isLeftPressed)
			{
				win.Position.X = mousePos.X - win.DragOffset.X;
				win.Position.Y = mousePos.Y - win.DragOffset.Y;

				int screenW = DSurface::Composite ? DSurface::Composite->Width : 1024;
				int screenH = DSurface::Composite ? DSurface::Composite->Height : 768;
				win.Position.X = std::max(0, std::min(win.Position.X, std::max(0, screenW - win.WindowRect.Width)));
				win.Position.Y = std::max(0, std::min(win.Position.Y, std::max(0, screenH - win.WindowRect.Height)));
				anyDragging = true;
			}
			else
			{
				win.IsDragging = false;
			}
		}
	}

	if (anyDragging)
	{
		DisplayClass::Instance.ClearDragBand();
	}

	// Handle keyboard input & real-time typematic auto-repeat if search input box is focused
	static int heldVK = -1;
	static unsigned long long firstPressTimeMs = 0;
	static unsigned long long lastRepeatTimeMs = 0;

	unsigned long long nowMs = GetTickCount64();

	if (this->IsSearchInputFocused)
	{
		bool textChanged = false;

		int currentPressedVK = -1;
		wchar_t charNormal = L'\0';
		wchar_t charShift = L'\0';
		bool isShift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;

		if ((GetAsyncKeyState(VK_BACK) & 0x8000) != 0)
		{
			currentPressedVK = VK_BACK;
		}
		else if ((GetAsyncKeyState(VK_SPACE) & 0x8000) != 0)
		{
			currentPressedVK = VK_SPACE;
			charNormal = L' ';
			charShift = L' ';
		}
		else if ((GetAsyncKeyState(0xDE) & 0x8000) != 0) // Quotes / apostrophe
		{
			currentPressedVK = 0xDE;
			charNormal = L'\'';
			charShift = L'"';
		}
		else if ((GetAsyncKeyState(0xBD) & 0x8000) != 0) // Hyphen / minus
		{
			currentPressedVK = 0xBD;
			charNormal = L'-';
			charShift = L'_';
		}
		else if ((GetAsyncKeyState(0xBE) & 0x8000) != 0) // Period / dot
		{
			currentPressedVK = 0xBE;
			charNormal = L'.';
			charShift = L'>';
		}
		else if ((GetAsyncKeyState(0xBC) & 0x8000) != 0) // Comma
		{
			currentPressedVK = 0xBC;
			charNormal = L',';
			charShift = L'<';
		}
		else
		{
			for (int vk = 'A'; vk <= 'Z'; ++vk)
			{
				if ((GetAsyncKeyState(vk) & 0x8000) != 0)
				{
					currentPressedVK = vk;
					charNormal = static_cast<wchar_t>('a' + (vk - 'A'));
					charShift = static_cast<wchar_t>('A' + (vk - 'A'));
					break;
				}
			}

			if (currentPressedVK == -1)
			{
				for (int vk = '0'; vk <= '9'; ++vk)
				{
					if ((GetAsyncKeyState(vk) & 0x8000) != 0)
					{
						currentPressedVK = vk;
						charNormal = static_cast<wchar_t>(vk);
						charShift = static_cast<wchar_t>(vk);
						break;
					}
				}
			}
		}

		// Escape or Enter -> Unfocus search box
		if ((GetAsyncKeyState(VK_ESCAPE) & 0x8000) != 0 || (GetAsyncKeyState(VK_RETURN) & 0x8000) != 0)
		{
			this->IsSearchInputFocused = false;
			heldVK = -1;
			firstPressTimeMs = 0;
			lastRepeatTimeMs = 0;
		}
		else if (currentPressedVK != -1)
		{
			if (currentPressedVK != heldVK)
			{
				// First press event: trigger immediately!
				heldVK = currentPressedVK;
				firstPressTimeMs = nowMs;
				lastRepeatTimeMs = nowMs;

				if (currentPressedVK == VK_BACK)
				{
					if (!this->SearchFilterText.empty())
					{
						this->SearchFilterText.pop_back();
						textChanged = true;
					}
				}
				else if (charNormal != L'\0')
				{
					wchar_t ch = isShift ? charShift : charNormal;
					this->SearchFilterText += ch;
					textChanged = true;
				}
			}
			else
			{
				// Key is held down: initial delay 450 ms, repeat rate 60 ms
				if ((nowMs - firstPressTimeMs) >= 450 && (nowMs - lastRepeatTimeMs) >= 60)
				{
					lastRepeatTimeMs = nowMs;

					if (currentPressedVK == VK_BACK)
					{
						if (!this->SearchFilterText.empty())
						{
							this->SearchFilterText.pop_back();
							textChanged = true;
						}
					}
					else if (charNormal != L'\0')
					{
						wchar_t ch = isShift ? charShift : charNormal;
						this->SearchFilterText += ch;
						textChanged = true;
					}
				}
			}
		}
		else
		{
			heldVK = -1;
			firstPressTimeMs = 0;
			lastRepeatTimeMs = 0;
		}

		if (textChanged)
		{
			this->CollectPlayerData();
		}
	}
	else
	{
		heldVK = -1;
		firstPressTimeMs = 0;
		lastRepeatTimeMs = 0;
	}

	int currentFrame = Unsorted::CurrentFrame;

	// Sample money for all active houses once every 15 frames (~1 second)
	static int lastSampleFrame = -1;
	if (lastSampleFrame == -1 || (currentFrame - lastSampleFrame) >= 15)
	{
		lastSampleFrame = currentFrame;

		for (int i = 0; i < HouseClass::Array.Count; ++i)
		{
			auto pHouse = HouseClass::Array.GetItem(i);
			if (!pHouse || pHouse->Defeated || !pHouse->Type)
				continue;

			// Exclude neutral, civilian, defeated or observer houses
			if (pHouse->IsObserver() || pHouse == HouseClass::FindNeutral() || pHouse == HouseClass::FindSpecial() || pHouse == HouseClass::FindCivilianSide())
				continue;

			auto& history = this->EconomyHistory[pHouse];
			int currentMoney = pHouse->Available_Money();
			history.push_back({ currentFrame, currentMoney });

			// Keep at most 60 samples (60 seconds rolling history window)
			while (history.size() > 60)
			{
				history.pop_front();
			}
		}
	}

	// Skip player data collection ONLY if in Minimal mode with no open floating windows (saves CPU)
	bool isMinimalEmpty = (this->DisplayMode == ObserverUIDisplayMode::Minimal && this->FloatingWindows.empty() && this->FloatingUnitWindows.empty());
	if (!isMinimalEmpty)
	{
		this->CollectPlayerData();
	}
}

void ObserverUIClass::Render(DSurface* pSurface)
{
	bool isActive = IsActive() || (Phobos::Config::DevelopmentCommands && (this->DisplayMode != ObserverUIDisplayMode::Hidden || this->HasFloatingWindows()));
	if (!isActive || !pSurface)
		return;

	this->Update();

	if (this->DisplayMode == ObserverUIDisplayMode::Hidden)
		return;

	if (this->DisplayMode == ObserverUIDisplayMode::Minimal)
	{
		Point2D mousePos { 0, 0 };
		if (WWMouseClass::Instance)
		{
			mousePos.X = WWMouseClass::Instance->GetX();
			mousePos.Y = WWMouseClass::Instance->GetY();
		}

		this->HasHoveredItem = false;
		this->HasHoveredPlayer = false;

		// 1. Render Floating Player Windows & Floating Unit Windows
		this->RenderFloatingWindows(pSurface);
		this->RenderFloatingUnitWindows(pSurface);

		// 2. Render Inspect Button at bottom-left in Minimal mode (54x40, with 18px bottom padding to avoid covering bottom info text)
		int screenH = DSurface::ViewBounds.Height;
		this->InspectBtnRect = RectangleStruct { 16, screenH - 58, 54, 40 };
		this->IsHoveringInspectBtn = (mousePos.X >= this->InspectBtnRect.X && mousePos.X <= (this->InspectBtnRect.X + this->InspectBtnRect.Width)
			&& mousePos.Y >= this->InspectBtnRect.Y && mousePos.Y <= (this->InspectBtnRect.Y + this->InspectBtnRect.Height));

		ColorStruct btnBgColor = this->IsHoveringInspectBtn ? ColorStruct { 100, 220, 255 } : ColorStruct { 0, 0, 0 };
		pSurface->FillRectTrans(&this->InspectBtnRect, &btnBgColor, this->IsHoveringInspectBtn ? 180 : 120);
		pSurface->DrawRect(&this->InspectBtnRect, this->IsHoveringInspectBtn ? Drawing::RGB_To_Int(255, 255, 255) : Drawing::RGB_To_Int(140, 140, 140));

		if (BitFont::Instance && BitText::Instance)
		{
			int btnW = 0, btnH = 0;
			BitFont::Instance->GetTextDimension(L"-> [] <-", &btnW, &btnH, this->InspectBtnRect.Width);
			Point2D btnPt = { this->InspectBtnRect.X + (this->InspectBtnRect.Width - btnW) / 2, this->InspectBtnRect.Y + (this->InspectBtnRect.Height - btnH) / 2 };

			LTRBStruct oldBounds = BitFont::Instance->Bounds;
			WORD oldColor = BitFont::Instance->Color;
			bool oldField41 = BitFont::Instance->field_41;

			LTRBStruct ltrbBounds = { this->InspectBtnRect.X, this->InspectBtnRect.Y, this->InspectBtnRect.X + this->InspectBtnRect.Width, this->InspectBtnRect.Y + this->InspectBtnRect.Height };
			BitFont::Instance->field_41 = 1;
			BitFont::Instance->SetBounds(&ltrbBounds);
			BitFont::Instance->Color = static_cast<WORD>(this->IsHoveringInspectBtn ? Drawing::RGB_To_Int(255, 255, 255) : Drawing::RGB_To_Int(200, 200, 200));

			BitText::Instance->DrawText(
				BitFont::Instance,
				pSurface,
				L"-> [] <-",
				btnPt.X,
				btnPt.Y,
				btnW,
				btnH,
				0, 0, 0
			);

			BitFont::Instance->Bounds = oldBounds;
			BitFont::Instance->Color = oldColor;
			BitFont::Instance->field_41 = oldField41;
		}

		// 3. Render Tooltip for Inspect Button or Floating windows in Minimal mode
		if (this->IsHoveringInspectBtn && BitFont::Instance)
		{
			std::wstring tooltipText = L"Inspect Selected Object (Create Card)";
			int textW = 0, textH = 0;
			BitFont::Instance->GetTextDimension(tooltipText.c_str(), &textW, &textH, 300);

			int tipX = std::min(mousePos.X + 12, pSurface->Width - textW - 16);
			int tipY = std::max(10, mousePos.Y - textH - 12);

			RectangleStruct tipBgRect = { tipX - 4, tipY - 4, textW + 8, textH + 8 };
			ColorStruct tipBgColor { 0, 0, 0 };
			pSurface->FillRectTrans(&tipBgRect, &tipBgColor, 200);
			pSurface->DrawRect(&tipBgRect, Drawing::RGB_To_Int(140, 140, 140));

			Point2D tipPt { tipX, tipY };
			pSurface->DrawTextA(tooltipText.c_str(), &DSurface::ViewBounds, &tipPt, Drawing::RGB_To_Int(255, 255, 255), 0, TextPrintType::Point8);
		}
		else if (this->HasHoveredPlayer && this->pHoveredPlayer)
		{
			this->DrawPlayerTooltip(pSurface, this->pHoveredPlayer, mousePos);
		}
		else if (this->HasHoveredItem && this->HoveredItem.pType)
		{
			this->DrawTooltip(pSurface, this->HoveredItem, mousePos);
		}

		return;
	}

	if (this->PlayerRows.empty())
		return;

	int screenWidth = DSurface::ViewBounds.Width;
	int screenHeight = DSurface::ViewBounds.Height;

	int cameoWidth = 60;
	int cameoHeight = 48;
	int padding = 4;
	int sectionGap = 3;
	int rowHeight = cameoHeight + padding * 2;
	int startY = screenHeight - (static_cast<int>(this->PlayerRows.size()) * rowHeight) - 18;
	int startX = 20;

	Point2D mousePos { 0, 0 };
	if (WWMouseClass::Instance)
	{
		mousePos.X = WWMouseClass::Instance->GetX();
		mousePos.Y = WWMouseClass::Instance->GetY();
	}

	// Capture mouse clicks using GetKeyState
	bool isLButtonDown = (GetKeyState(VK_LBUTTON) & 0x8000) != 0;
	static bool wasLButtonDown = false;
	bool isLeftClick = isLButtonDown && !wasLButtonDown;
	wasLButtonDown = isLButtonDown;

	this->HasHoveredItem = false;
	this->HasHoveredPlayer = false;

	// Calculate max production items across all rows so structure sections align perfectly across all players
	size_t maxProdCount = 0;
	for (auto const& row : this->PlayerRows)
	{
		maxProdCount = std::max(maxProdCount, row.ProductionItems.size());
	}

	int infoBoxWidth = 120;
	int playerColorBarWidth = 5;
	int teamColorBarWidth = 10;
	int rightMargin = 20;

	int structStartX = startX + infoBoxWidth + playerColorBarWidth + sectionGap;

	int prodSectionWidth = (maxProdCount > 0) ? (static_cast<int>(maxProdCount) * (cameoWidth + padding) + padding) : 0;
	int prodEndX = screenWidth - rightMargin;
	int prodStartX = (maxProdCount > 0) ? (prodEndX - prodSectionWidth) : prodEndX;

	int maxStructEndX = (maxProdCount > 0) ? (prodStartX - playerColorBarWidth - sectionGap) : prodEndX;
	int availableStructWidth = maxStructEndX - structStartX - 30; // Reserve room for scroll buttons
	if (availableStructWidth < 0) availableStructWidth = 0;

	// Build Tab Buttons positioned top-center directly attached above Section 2 (middle objects panel)
	static const std::vector<std::pair<ObserverFilterCategory, std::wstring>> tabDefs = {
		{ ObserverFilterCategory::Defenses, L"Defenses" },
		{ ObserverFilterCategory::Structures, L"Structures" },
		{ ObserverFilterCategory::AllStructures, L"All Structures" },
		{ ObserverFilterCategory::Infantry, L"Infantry" },
		{ ObserverFilterCategory::Vehicles, L"Vehicles" },
		{ ObserverFilterCategory::Naval, L"Naval" },
		{ ObserverFilterCategory::Aircraft, L"Aircraft" },
		{ ObserverFilterCategory::AllUnits, L"All Units" },
		{ ObserverFilterCategory::Superweapons, L"Superweapons" },
		{ ObserverFilterCategory::Everything, L"Everything" }
	};

	this->TabButtons.clear();
	int tabHeight = 20;
	int tabPadding = 8;
	int tabGap = 3;

	// Measure individual tab widths
	std::vector<int> tabWidths;
	for (const auto& [cat, label] : tabDefs)
	{
		int w = 0, h = 0;
		if (BitFont::Instance)
		{
			BitFont::Instance->GetTextDimension(label.c_str(), &w, &h, 200);
		}
		else
		{
			w = static_cast<int>(label.length() * 8);
		}
		int tabW = w + tabPadding * 2;
		tabWidths.push_back(tabW);
	}

	int maxTabsAvailableWidth = maxStructEndX - structStartX;
	if (maxTabsAvailableWidth < 100) maxTabsAvailableWidth = 100;

	// Automatically wrap tabs into multiple rows if screen resolution is low or space is constrained
	std::vector<std::vector<size_t>> tabRows;
	std::vector<size_t> currentLine;
	int currentLineWidth = 0;

	for (size_t i = 0; i < tabDefs.size(); ++i)
	{
		int tabW = tabWidths[i];
		int neededWidth = currentLine.empty() ? tabW : (currentLineWidth + tabGap + tabW);

		if (!currentLine.empty() && neededWidth > maxTabsAvailableWidth)
		{
			tabRows.push_back(currentLine);
			currentLine.clear();
			currentLine.push_back(i);
			currentLineWidth = tabW;
		}
		else
		{
			currentLine.push_back(i);
			currentLineWidth = neededWidth;
		}
	}
	if (!currentLine.empty())
	{
		tabRows.push_back(currentLine);
	}

	int tabRowGap = 3;
	int totalRows = static_cast<int>(this->PlayerRows.size());
	int totalRowsH = totalRows * rowHeight;
	int calcStartY = screenHeight - totalRowsH - 18;

	int tabRowsCount = static_cast<int>(tabRows.size());
	int searchH = 24;
	int inspectBtnW = 54;
	int clearW = 24;
	int availableSectionW = maxStructEndX - structStartX;
	int searchW = availableSectionW - inspectBtnW - clearW - 6;
	if (searchW < 80) searchW = 80;

	int inspectX = structStartX;
	int searchX = inspectX + inspectBtnW + 3;

	int totalHeaderH = (tabRowsCount * tabHeight) + ((tabRowsCount - 1) * tabRowGap) + searchH + 12;
	int topHeaderMinY = 20;
	int bottomMargin = 45;
	int availablePlayerAreaH = screenHeight - totalHeaderH - bottomMargin;

	int maxAllowedRows = 8;
	int maxVisibleRows = totalRows;
	bool needsScroll = (totalRows > maxAllowedRows) || (calcStartY - totalHeaderH < topHeaderMinY) || (totalRowsH > availablePlayerAreaH);

	int searchY = 0;
	int tabsBaseY = 0;
	int highestTabY = 0;

	if (needsScroll)
	{
		int vertBtnH = 20;
		int playerRowsH = availablePlayerAreaH - vertBtnH - 6;
		maxVisibleRows = std::min(maxAllowedRows, std::max(1, playerRowsH / rowHeight));

		this->MaxVerticalScrollOffset = std::max(0, totalRows - maxVisibleRows);
		this->VerticalScrollOffset = std::clamp(this->VerticalScrollOffset, 0, this->MaxVerticalScrollOffset);

		searchY = topHeaderMinY;
		highestTabY = searchY + searchH + 4;
		tabsBaseY = highestTabY + (tabRowsCount * tabHeight) + ((tabRowsCount - 1) * tabRowGap);

		int scrollBtnW = 40;
		int centerBtnX = structStartX + (availableStructWidth / 2) - scrollBtnW;
		int scrollBtnY = tabsBaseY + 2;

		this->VertScrollUpBtnRect = RectangleStruct { centerBtnX, scrollBtnY, scrollBtnW, vertBtnH };
		this->VertScrollDownBtnRect = RectangleStruct { centerBtnX + scrollBtnW, scrollBtnY, scrollBtnW, vertBtnH };

		this->IsHoveringVertScrollUp = mousePos.X >= this->VertScrollUpBtnRect.X && mousePos.X <= (this->VertScrollUpBtnRect.X + this->VertScrollUpBtnRect.Width)
			&& mousePos.Y >= this->VertScrollUpBtnRect.Y && mousePos.Y <= (this->VertScrollUpBtnRect.Y + this->VertScrollUpBtnRect.Height);
		this->IsHoveringVertScrollDown = mousePos.X >= this->VertScrollDownBtnRect.X && mousePos.X <= (this->VertScrollDownBtnRect.X + this->VertScrollDownBtnRect.Width)
			&& mousePos.Y >= this->VertScrollDownBtnRect.Y && mousePos.Y <= (this->VertScrollDownBtnRect.Y + this->VertScrollDownBtnRect.Height);

		// Render Joined Centered Scroll Buttons [ ▲ ][ ▼ ]
		ColorStruct upBg = this->IsHoveringVertScrollUp ? ColorStruct { 0, 140, 180 } : ColorStruct { 30, 30, 30 };
		pSurface->FillRectTrans(&this->VertScrollUpBtnRect, &upBg, 80);
		COLORREF upBorder = this->IsHoveringVertScrollUp ? Drawing::RGB_To_Int(0, 255, 255) : Drawing::RGB_To_Int(80, 80, 80);
		pSurface->DrawRect(&this->VertScrollUpBtnRect, upBorder);

		ColorStruct downBg = this->IsHoveringVertScrollDown ? ColorStruct { 0, 140, 180 } : ColorStruct { 30, 30, 30 };
		pSurface->FillRectTrans(&this->VertScrollDownBtnRect, &downBg, 80);
		COLORREF downBorder = this->IsHoveringVertScrollDown ? Drawing::RGB_To_Int(0, 255, 255) : Drawing::RGB_To_Int(80, 80, 80);
		pSurface->DrawRect(&this->VertScrollDownBtnRect, downBorder);

		if (BitFont::Instance && BitText::Instance)
		{
			LTRBStruct oldBounds = BitFont::Instance->Bounds;

			LTRBStruct upBounds = { this->VertScrollUpBtnRect.X, this->VertScrollUpBtnRect.Y, this->VertScrollUpBtnRect.X + this->VertScrollUpBtnRect.Width, this->VertScrollUpBtnRect.Y + this->VertScrollUpBtnRect.Height };
			BitFont::Instance->SetBounds(&upBounds);
			Point2D upPt = { this->VertScrollUpBtnRect.X + 16, this->VertScrollUpBtnRect.Y + 2 };
			COLORREF upTextColor = (this->VerticalScrollOffset > 0) ? Drawing::RGB_To_Int(255, 255, 255) : Drawing::RGB_To_Int(90, 90, 90);
			pSurface->DrawTextA(L"^", &DSurface::ViewBounds, &upPt, upTextColor, 0, TextPrintType::FullShadow | TextPrintType::Point8);

			LTRBStruct downBounds = { this->VertScrollDownBtnRect.X, this->VertScrollDownBtnRect.Y, this->VertScrollDownBtnRect.X + this->VertScrollDownBtnRect.Width, this->VertScrollDownBtnRect.Y + this->VertScrollDownBtnRect.Height };
			BitFont::Instance->SetBounds(&downBounds);
			Point2D downPt = { this->VertScrollDownBtnRect.X + 16, this->VertScrollDownBtnRect.Y + 2 };
			COLORREF downTextColor = (this->VerticalScrollOffset < this->MaxVerticalScrollOffset) ? Drawing::RGB_To_Int(255, 255, 255) : Drawing::RGB_To_Int(90, 90, 90);
			pSurface->DrawTextA(L"v", &DSurface::ViewBounds, &downPt, downTextColor, 0, TextPrintType::FullShadow | TextPrintType::Point8);

			BitFont::Instance->Bounds = oldBounds;
		}

		startY = scrollBtnY + vertBtnH + 4;
	}
	else
	{
		this->VerticalScrollOffset = 0;
		this->MaxVerticalScrollOffset = 0;
		this->VertScrollUpBtnRect = RectangleStruct { 0, 0, 0, 0 };
		this->VertScrollDownBtnRect = RectangleStruct { 0, 0, 0, 0 };

		// Dynamic bottom anchor when fewer players: UI moves down to screen bottom!
		startY = calcStartY;
		tabsBaseY = startY - 4;
		highestTabY = tabsBaseY - (tabRowsCount * tabHeight) - ((tabRowsCount - 1) * tabRowGap);
		searchY = highestTabY - searchH - 4;
	}

	// Calculate team Y extents ONLY for teams with 2 or more players!
	std::map<int, std::pair<int, int>> teamYExtents;
	int calcY = startY;
	for (auto const& row : this->PlayerRows)
	{
		if (row.TeamID >= 0 && row.TeamMemberCount >= 2)
		{
			if (teamYExtents.count(row.TeamID) == 0)
			{
				teamYExtents[row.TeamID] = { calcY, calcY + rowHeight };
			}
			else
			{
				teamYExtents[row.TeamID].second = calcY + rowHeight;
			}
		}
		calcY += rowHeight;
	}

	this->InspectBtnRect = RectangleStruct { inspectX, searchY, inspectBtnW, searchH };
	this->SearchBoxRect = RectangleStruct { searchX, searchY, searchW, searchH };
	this->ClearBtnRect = RectangleStruct { searchX + searchW + 3, searchY, clearW, searchH };

	this->IsHoveringInspectBtn = mousePos.X >= this->InspectBtnRect.X && mousePos.X <= (this->InspectBtnRect.X + this->InspectBtnRect.Width)
		&& mousePos.Y >= this->InspectBtnRect.Y && mousePos.Y <= (this->InspectBtnRect.Y + this->InspectBtnRect.Height);
	this->IsHoveringClearBtn = mousePos.X >= this->ClearBtnRect.X && mousePos.X <= (this->ClearBtnRect.X + this->ClearBtnRect.Width)
		&& mousePos.Y >= this->ClearBtnRect.Y && mousePos.Y <= (this->ClearBtnRect.Y + this->ClearBtnRect.Height);

	// Build Tab Buttons
	for (size_t r = 0; r < tabRows.size(); ++r)
	{
		int rowY = 0;
		if (needsScroll)
		{
			rowY = highestTabY + (static_cast<int>(r) * (tabHeight + tabRowGap));
		}
		else
		{
			rowY = tabsBaseY - (static_cast<int>(r + 1) * tabHeight) - (static_cast<int>(r) * tabRowGap);
		}

		const auto& lineIndices = tabRows[r];
		int lineTotalW = 0;
		for (size_t idx : lineIndices)
		{
			lineTotalW += tabWidths[idx];
		}
		lineTotalW += static_cast<int>(lineIndices.size() - 1) * tabGap;

		int lineStartX = structStartX;
		if (lineStartX + lineTotalW > maxStructEndX)
		{
			lineStartX = std::max(structStartX, maxStructEndX - lineTotalW);
		}

		int curX = lineStartX;
		for (size_t idx : lineIndices)
		{
			ObserverTabButton btn;
			btn.Category = tabDefs[idx].first;
			btn.Label = tabDefs[idx].second;
			btn.Rect = RectangleStruct { curX, rowY, tabWidths[idx], tabHeight };
			btn.IsHovered = mousePos.X >= btn.Rect.X && mousePos.X <= (btn.Rect.X + btn.Rect.Width)
				&& mousePos.Y >= btn.Rect.Y && mousePos.Y <= (btn.Rect.Y + btn.Rect.Height);
			this->TabButtons.push_back(btn);

			if (isLeftClick && btn.IsHovered)
			{
				this->ActiveFilterTab = btn.Category;
				this->CollectPlayerData();
			}

			curX += tabWidths[idx] + tabGap;
		}
	}

	// Render Category Filter Tab Buttons
	for (const auto& btn : this->TabButtons)
	{
		bool isTabActive = (btn.Category == this->ActiveFilterTab);

		ColorStruct tabBgColor { 0, 0, 0 };
		pSurface->FillRectTrans(const_cast<RectangleStruct*>(&btn.Rect), &tabBgColor, isTabActive ? 95 : 60);

		// Border color: Neon Cyan for active tab, Soft White for hovered, Dark Gray for inactive
		COLORREF borderColor = Drawing::RGB_To_Int(60, 60, 60);
		if (isTabActive)
		{
			borderColor = Drawing::RGB_To_Int(0, 255, 255); // Cyan active outline
		}
		else if (btn.IsHovered)
		{
			borderColor = Drawing::RGB_To_Int(180, 180, 180);
		}
		pSurface->DrawRect(const_cast<RectangleStruct*>(&btn.Rect), borderColor);

		// Text color: Bright White for active, Soft White for hovered, Silver for inactive
		COLORREF textColor = Drawing::RGB_To_Int(160, 160, 160);
		if (isTabActive)
		{
			textColor = Drawing::RGB_To_Int(255, 255, 255);
		}
		else if (btn.IsHovered)
		{
			textColor = Drawing::RGB_To_Int(220, 220, 220);
		}

		if (BitFont::Instance && BitText::Instance)
		{
			int textW = 0, textH = 0;
			BitFont::Instance->GetTextDimension(btn.Label.c_str(), &textW, &textH, btn.Rect.Width);
			int textX = btn.Rect.X + (btn.Rect.Width - textW) / 2;
			int textY = btn.Rect.Y + (btn.Rect.Height - textH) / 2;

			LTRBStruct oldBounds = BitFont::Instance->Bounds;
			WORD oldColor = BitFont::Instance->Color;
			bool oldField41 = BitFont::Instance->field_41;

			LTRBStruct ltrbBounds = { btn.Rect.X, btn.Rect.Y, btn.Rect.X + btn.Rect.Width, btn.Rect.Y + btn.Rect.Height };
			BitFont::Instance->field_41 = 1;
			BitFont::Instance->SetBounds(&ltrbBounds);
			BitFont::Instance->Color = static_cast<WORD>(textColor);

			BitText::Instance->DrawText(
				BitFont::Instance,
				pSurface,
				btn.Label.c_str(),
				textX,
				textY,
				textW,
				textH,
				0, 0, 0
			);

			BitFont::Instance->Bounds = oldBounds;
			BitFont::Instance->Color = oldColor;
			BitFont::Instance->field_41 = oldField41;
		}
	}

	// Render Inspect Selected Button [-> [] <-]
	ColorStruct inspectBgColor = this->IsHoveringInspectBtn ? ColorStruct { 0, 140, 180 } : ColorStruct { 30, 30, 30 };
	pSurface->FillRectTrans(&this->InspectBtnRect, &inspectBgColor, 80);
	COLORREF inspectBorderColor = this->IsHoveringInspectBtn ? Drawing::RGB_To_Int(0, 255, 255) : Drawing::RGB_To_Int(80, 80, 80);
	pSurface->DrawRect(&this->InspectBtnRect, inspectBorderColor);

	{
		LTRBStruct oldBounds = BitFont::Instance->Bounds;
		LTRBStruct btnBounds = { this->InspectBtnRect.X, this->InspectBtnRect.Y, this->InspectBtnRect.X + this->InspectBtnRect.Width, this->InspectBtnRect.Y + this->InspectBtnRect.Height };
		BitFont::Instance->SetBounds(&btnBounds);

		int textW = 0, textH = 0;
		BitFont::Instance->GetTextDimension(L"-> [] <-", &textW, &textH, inspectBtnW);
		Point2D iconPt = { this->InspectBtnRect.X + (inspectBtnW - textW) / 2, this->InspectBtnRect.Y + 3 };
		pSurface->DrawTextA(L"-> [] <-", &DSurface::ViewBounds, &iconPt, this->IsHoveringInspectBtn ? Drawing::RGB_To_Int(0, 255, 255) : Drawing::RGB_To_Int(220, 220, 220), 0, TextPrintType::FullShadow | TextPrintType::Point8);
		BitFont::Instance->Bounds = oldBounds;
	}

	// Render Search Input Box Background
	ColorStruct searchBgColor { 15, 15, 15 };
	pSurface->FillRectTrans(&this->SearchBoxRect, &searchBgColor, this->IsSearchInputFocused ? 90 : 60);

	COLORREF searchBorderColor = this->IsSearchInputFocused ? Drawing::RGB_To_Int(0, 255, 255) : Drawing::RGB_To_Int(60, 60, 60);
	pSurface->DrawRect(&this->SearchBoxRect, searchBorderColor);

	// Render text inside Search Box
	{
		LTRBStruct oldBounds = BitFont::Instance->Bounds;
		LTRBStruct searchBounds = { this->SearchBoxRect.X + 2, this->SearchBoxRect.Y + 2, this->SearchBoxRect.X + this->SearchBoxRect.Width - 2, this->SearchBoxRect.Y + this->SearchBoxRect.Height - 2 };
		BitFont::Instance->SetBounds(&searchBounds);

		Point2D textPt = { this->SearchBoxRect.X + 8, this->SearchBoxRect.Y + 4 };
		if (this->SearchFilterText.empty() && !this->IsSearchInputFocused)
		{
			pSurface->DrawTextA(L"Filter...", &DSurface::ViewBounds, &textPt, Drawing::RGB_To_Int(120, 120, 120), 0, TextPrintType::FullShadow | TextPrintType::Point8);
		}
		else
		{
			std::wstring displayTextW = this->SearchFilterText;
			if (this->IsSearchInputFocused)
			{
				if ((Unsorted::CurrentFrame / 15) % 2 == 0)
				{
					displayTextW += L"|";
				}
			}

			// Keep visible text length within box width (auto-scroll tail of long query string)
			int maxVisibleChars = (this->SearchBoxRect.Width - 18) / 8;
			if (maxVisibleChars < 4) maxVisibleChars = 4;
			if (displayTextW.length() > static_cast<size_t>(maxVisibleChars))
			{
				displayTextW = displayTextW.substr(displayTextW.length() - maxVisibleChars);
			}

			pSurface->DrawTextA(displayTextW.c_str(), &DSurface::ViewBounds, &textPt, Drawing::RGB_To_Int(255, 255, 255), 0, TextPrintType::FullShadow | TextPrintType::Point8);
		}
		BitFont::Instance->Bounds = oldBounds;
	}

	// Render Clear Button [X]
	ColorStruct clearBgColor = this->IsHoveringClearBtn ? ColorStruct { 180, 40, 40 } : ColorStruct { 30, 30, 30 };
	pSurface->FillRectTrans(&this->ClearBtnRect, &clearBgColor, 80);
	pSurface->DrawRect(&this->ClearBtnRect, Drawing::RGB_To_Int(80, 80, 80));

	{
		LTRBStruct oldBounds = BitFont::Instance->Bounds;
		LTRBStruct clearBounds = { this->ClearBtnRect.X, this->ClearBtnRect.Y, this->ClearBtnRect.X + this->ClearBtnRect.Width, this->ClearBtnRect.Y + this->ClearBtnRect.Height };
		BitFont::Instance->SetBounds(&clearBounds);
		Point2D xPt = { this->ClearBtnRect.X + 8, this->ClearBtnRect.Y + 3 };
		pSurface->DrawTextA(L"X", &DSurface::ViewBounds, &xPt, Drawing::RGB_To_Int(255, 255, 255), 0, TextPrintType::FullShadow | TextPrintType::Point8);
		BitFont::Instance->Bounds = oldBounds;
	}

	int visibleStart = std::clamp(this->VerticalScrollOffset, 0, std::max(0, totalRows - 1));
	int visibleEnd = std::min(totalRows, visibleStart + maxVisibleRows);

	// Render team alliance vertical bars attached directly to the left edge of Section 1 ONLY for 2+ player alliances
	for (auto const& [teamID, yPair] : teamYExtents)
	{
		auto const firstRow = std::find_if(this->PlayerRows.begin(), this->PlayerRows.end(), [teamID](const ObserverPlayerRow& r) {
			return r.TeamID == teamID;
		});

		if (firstRow != this->PlayerRows.end())
		{
			ColorStruct color = firstRow->TeamColor;
			RectangleStruct teamLineRect = { startX - teamColorBarWidth, yPair.first, teamColorBarWidth, yPair.second - yPair.first };
			pSurface->FillRect(&teamLineRect, Drawing::RGB_To_Int(color.R, color.G, color.B));
		}
	}

	for (int rIdx = visibleStart; rIdx < visibleEnd; ++rIdx)
	{
		auto& row = this->PlayerRows[rIdx];
		int currentY = startY + (rIdx - visibleStart) * rowHeight;
		ColorStruct bgPanelColor { 0, 0, 0 };

		// Section 1: Player Info Box + Player Color Bar
		row.InfoRect = RectangleStruct { startX, currentY, infoBoxWidth, rowHeight };
		
		// Translucent 30% opacity background box for Section 1 (opacity = 75)
		pSurface->FillRectTrans(&row.InfoRect, &bgPanelColor, 75);
		pSurface->DrawRect(&row.InfoRect, Drawing::RGB_To_Int(60, 60, 60));

		// Vertical Player Color bar on the right edge of Section 1
		RectangleStruct playerColorBarRect = { startX + infoBoxWidth, currentY, playerColorBarWidth, rowHeight };
		pSurface->FillRect(&playerColorBarRect, Drawing::RGB_To_Int(row.PlayerColor.R, row.PlayerColor.G, row.PlayerColor.B));

		bool isInfoHovered = mousePos.X >= (startX - teamColorBarWidth) && mousePos.X <= (startX + infoBoxWidth + playerColorBarWidth)
			&& mousePos.Y >= row.InfoRect.Y && mousePos.Y <= (row.InfoRect.Y + row.InfoRect.Height);

		if (isInfoHovered)
		{
			this->pHoveredPlayer = row.pHouse;
			this->HasHoveredPlayer = true;
			this->HoveredMousePos = mousePos;
			pSurface->DrawRect(&row.InfoRect, Drawing::RGB_To_Int(row.PlayerColor.R, row.PlayerColor.G, row.PlayerColor.B));
		}

		// Render Player Name (Line 1) and Country Name (Line 2)
		if (BitFont::Instance && BitText::Instance)
		{
			LTRBStruct oldBounds = BitFont::Instance->Bounds;
			LTRBStruct bounds = { row.InfoRect.X, row.InfoRect.Y, row.InfoRect.X + row.InfoRect.Width, row.InfoRect.Y + row.InfoRect.Height };
			BitFont::Instance->SetBounds(&bounds);

			Point2D pNamePt = { row.InfoRect.X + 6, row.InfoRect.Y + 6 };
			pSurface->DrawTextA(row.PlayerName.c_str(), &DSurface::ViewBounds, &pNamePt, Drawing::RGB_To_Int(255, 255, 255), 0, TextPrintType::FullShadow | TextPrintType::Point8);

			Point2D cNamePt = { row.InfoRect.X + 6, row.InfoRect.Y + 24 };
			pSurface->DrawTextA(row.CountryName.c_str(), &DSurface::ViewBounds, &cNamePt, Drawing::RGB_To_Int(180, 180, 180), 0, TextPrintType::FullShadow | TextPrintType::Point8);

			BitFont::Instance->SetBounds(&oldBounds);
		}

		// Section 3: Production Items Section (Anchored to far right of the screen) + Left Player Color Bar
		if (prodSectionWidth > 0)
		{
			// Vertical Player Color bar attached to the left edge of Section 3 (Production Panel)
			RectangleStruct prodColorBarRect = { prodStartX - playerColorBarWidth, currentY, playerColorBarWidth, rowHeight };
			pSurface->FillRect(&prodColorBarRect, Drawing::RGB_To_Int(row.PlayerColor.R, row.PlayerColor.G, row.PlayerColor.B));

			row.ProdPanelRect = RectangleStruct { prodStartX, currentY, prodSectionWidth, rowHeight };
			pSurface->FillRectTrans(&row.ProdPanelRect, &bgPanelColor, 75);
			pSurface->DrawRect(&row.ProdPanelRect, Drawing::RGB_To_Int(60, 60, 60));

			int currentProdX = prodEndX - padding - cameoWidth;
			for (auto& item : row.ProductionItems)
			{
				item.DisplayRect = RectangleStruct { currentProdX, currentY + padding, cameoWidth, cameoHeight };

				// Strictly render ONLY if item display box fits completely inside ProdPanelRect
				if (item.DisplayRect.X >= row.ProdPanelRect.X && (item.DisplayRect.X + item.DisplayRect.Width) <= (row.ProdPanelRect.X + row.ProdPanelRect.Width))
				{
					bool isHovered = mousePos.X >= item.DisplayRect.X && mousePos.X <= (item.DisplayRect.X + item.DisplayRect.Width)
						&& mousePos.Y >= item.DisplayRect.Y && mousePos.Y <= (item.DisplayRect.Y + item.DisplayRect.Height);

					if (isHovered)
					{
						this->HoveredItem = item;
						this->HasHoveredItem = true;
						this->HoveredMousePos = mousePos;
					}

					this->DrawCameoItem(pSurface, item, isHovered, row.ProdPanelRect, row.PlayerColor);
				}

				currentProdX -= (cameoWidth + padding);
			}
		}

		// Section 2: Filtered Objects Section (Middle Section, expands up to Production Panel boundary)
		int totalStructWidth = (!row.StructureItems.empty()) ? (static_cast<int>(row.StructureItems.size()) * (cameoWidth + padding) + padding) : 0;
		row.MaxScrollOffset = std::max(0, totalStructWidth - availableStructWidth);
		row.ScrollOffset = std::clamp(row.ScrollOffset, 0, row.MaxScrollOffset);

		int structPanelWidth = std::min(totalStructWidth, availableStructWidth);
		if (structPanelWidth < 0) structPanelWidth = 0;

		row.StructPanelRect = RectangleStruct { structStartX, currentY, structPanelWidth, rowHeight };
		if (structPanelWidth > 0)
		{
			pSurface->FillRectTrans(&row.StructPanelRect, &bgPanelColor, 75);
			pSurface->DrawRect(&row.StructPanelRect, Drawing::RGB_To_Int(60, 60, 60));

			int currentStructX = structStartX + padding - row.ScrollOffset;
			for (auto& item : row.StructureItems)
			{
				item.DisplayRect = RectangleStruct { currentStructX, currentY + padding, cameoWidth, cameoHeight };

				// Strictly render ONLY if item display box fits completely inside StructPanelRect (does NOT overlap scroll buttons)
				if (item.DisplayRect.X >= row.StructPanelRect.X && (item.DisplayRect.X + item.DisplayRect.Width) <= (row.StructPanelRect.X + row.StructPanelRect.Width))
				{
					bool isHovered = mousePos.X >= item.DisplayRect.X && mousePos.X <= (item.DisplayRect.X + item.DisplayRect.Width)
						&& mousePos.Y >= item.DisplayRect.Y && mousePos.Y <= (item.DisplayRect.Y + item.DisplayRect.Height);

					if (isHovered)
					{
						this->HoveredItem = item;
						this->HasHoveredItem = true;
						this->HoveredMousePos = mousePos;
					}

					this->DrawCameoItem(pSurface, item, isHovered, row.StructPanelRect, row.PlayerColor);
				}

				currentStructX += cameoWidth + padding;
			}
		}

		// Render per-player row scroll buttons if structure list exceeds screen width
		if (row.MaxScrollOffset > 0)
		{
			int btnX = structStartX + structPanelWidth + 4;
			row.ScrollLeftBtnRect = RectangleStruct { btnX, currentY + padding, 20, cameoHeight / 2 - 1 };
			row.ScrollRightBtnRect = RectangleStruct { btnX, currentY + padding + cameoHeight / 2 + 1, 20, cameoHeight / 2 - 1 };

			row.IsHoveringLeftScroll = (mousePos.X >= row.ScrollLeftBtnRect.X && mousePos.X <= row.ScrollLeftBtnRect.X + row.ScrollLeftBtnRect.Width
				&& mousePos.Y >= row.ScrollLeftBtnRect.Y && mousePos.Y <= row.ScrollLeftBtnRect.Y + row.ScrollLeftBtnRect.Height);
			row.IsHoveringRightScroll = (mousePos.X >= row.ScrollRightBtnRect.X && mousePos.X <= row.ScrollRightBtnRect.X + row.ScrollRightBtnRect.Width
				&& mousePos.Y >= row.ScrollRightBtnRect.Y && mousePos.Y <= row.ScrollRightBtnRect.Y + row.ScrollRightBtnRect.Height);

			ColorStruct btnColor = row.IsHoveringLeftScroll ? row.PlayerColor : ColorStruct { 180, 180, 180 };
			pSurface->FillRectTrans(&row.ScrollLeftBtnRect, &btnColor, 200);
			pSurface->DrawRect(&row.ScrollLeftBtnRect, Drawing::RGB_To_Int(255, 255, 255));

			btnColor = row.IsHoveringRightScroll ? row.PlayerColor : ColorStruct { 180, 180, 180 };
			pSurface->FillRectTrans(&row.ScrollRightBtnRect, &btnColor, 200);
			pSurface->DrawRect(&row.ScrollRightBtnRect, Drawing::RGB_To_Int(255, 255, 255));

			if (BitFont::Instance)
			{
				Point2D leftPt = { row.ScrollLeftBtnRect.X + 5, row.ScrollLeftBtnRect.Y + 2 };
				Point2D rightPt = { row.ScrollRightBtnRect.X + 5, row.ScrollRightBtnRect.Y + 2 };
				pSurface->DrawTextA(L"<", &DSurface::ViewBounds, &leftPt, Drawing::RGB_To_Int(0, 0, 0), 0, TextPrintType::Point8);
				pSurface->DrawTextA(L">", &DSurface::ViewBounds, &rightPt, Drawing::RGB_To_Int(0, 0, 0), 0, TextPrintType::Point8);
			}
		}

		currentY += rowHeight;
	}

	// Trigger mouse click action after all layout geometry rects have been updated
	if (isLeftClick)
	{
		this->HandleMouseClick(mousePos, false);
	}

	// Render tooltip for inspect button, player info or cameo item
	if (this->IsHoveringInspectBtn && BitFont::Instance)
	{
		std::wstring tooltipText = L"Inspect Selected Object (Create Card)";
		int textW = 0, textH = 0;
		BitFont::Instance->GetTextDimension(tooltipText.c_str(), &textW, &textH, 300);

		int tipX = std::min(mousePos.X + 12, pSurface->Width - textW - 16);
		int tipY = std::max(10, mousePos.Y - textH - 12);

		RectangleStruct tipBgRect = { tipX - 4, tipY - 4, textW + 8, textH + 8 };
		ColorStruct tipBgColor { 0, 0, 0 };
		pSurface->FillRectTrans(&tipBgRect, &tipBgColor, 200);
		pSurface->DrawRect(&tipBgRect, Drawing::RGB_To_Int(140, 140, 140));

		Point2D tipPt { tipX, tipY };
		pSurface->DrawTextA(tooltipText.c_str(), &DSurface::ViewBounds, &tipPt, Drawing::RGB_To_Int(255, 255, 255), 0, TextPrintType::Point8);
	}
	else if (this->HasHoveredPlayer && this->pHoveredPlayer)
	{
		this->DrawPlayerTooltip(pSurface, this->pHoveredPlayer, this->HoveredMousePos);
	}
	else if (this->HasHoveredItem)
	{
		this->DrawTooltip(pSurface, this->HoveredItem, this->HoveredMousePos);
	}

	// Render Floating Windows on top of UI
	this->RenderFloatingWindows(pSurface);
	this->RenderFloatingUnitWindows(pSurface);
}

static const wchar_t* GetMissionNameString(Mission mission)
{
	switch (mission)
	{
	case Mission::Sleep: return L"Sleep";
	case Mission::Attack: return L"Attack";
	case Mission::Move: return L"Move";
	case Mission::Retreat: return L"Retreat";
	case Mission::Guard: return L"Guard";
	case Mission::Enter: return L"Enter";
	case Mission::Capture: return L"Capture";
	case Mission::Harvest: return L"Harvest";
	case Mission::Area_Guard: return L"Area Guard";
	case Mission::Hunt: return L"Hunt";
	case Mission::Unload: return L"Unload";
	case Mission::Sabotage: return L"Sabotage";
	case Mission::Construction: return L"Construction";
	case Mission::Selling: return L"Selling";
	case Mission::Repair: return L"Repair";
	case Mission::Patrol: return L"Patrol";
	case Mission::AttackMove: return L"Attack Move";
	default: return L"Idle";
	}
}

static bool IsTechnoValidAndAlive(TechnoClass* pTech)
{
	if (!pTech)
		return false;

	if (TechnoClass::Array.FindItemIndex(pTech) < 0)
		return false;

	return pTech->IsAlive && !pTech->InLimbo;
}

static bool IsBuildingValidAndAlive(BuildingClass* pBld)
{
	if (!pBld)
		return false;

	if (BuildingClass::Array.FindItemIndex(pBld) < 0)
		return false;

	return pBld->IsAlive && !pBld->InLimbo;
}

static int GetTechnoBuildTimeFrames(TechnoTypeClass* pType, HouseClass* pOwner)
{
	if (!pType || !pOwner)
		return 0;

	static char pTrick[0x6C8];
	memset(pTrick, 0, sizeof(pTrick));

	switch (pType->WhatAmI())
	{
	case AbstractType::BuildingType:
		VTable::Set(pTrick, BuildingClass::AbsVTable);
		reinterpret_cast<BuildingClass*>(pTrick)->Type = static_cast<BuildingTypeClass*>(pType);
		break;
	case AbstractType::AircraftType:
		VTable::Set(pTrick, AircraftClass::AbsVTable);
		reinterpret_cast<AircraftClass*>(pTrick)->Type = static_cast<AircraftTypeClass*>(pType);
		break;
	case AbstractType::InfantryType:
		VTable::Set(pTrick, InfantryClass::AbsVTable);
		reinterpret_cast<InfantryClass*>(pTrick)->Type = static_cast<InfantryTypeClass*>(pType);
		break;
	case AbstractType::UnitType:
		VTable::Set(pTrick, UnitClass::AbsVTable);
		reinterpret_cast<UnitClass*>(pTrick)->Type = static_cast<UnitTypeClass*>(pType);
		break;
	default:
		return 0;
	}

	reinterpret_cast<TechnoClass*>(pTrick)->Owner = pOwner;
	int nTimeToBuild = reinterpret_cast<TechnoClass*>(pTrick)->TimeToBuild();
	return std::max(54, nTimeToBuild);
}

static std::wstring FormatObjectNameWithDebug(int playerNum, const char* pID, const wchar_t* pUIName, bool isDebugEnabled)
{
	bool isMultiplayer = SessionClass::Instance.GameMode == GameMode::Skirmish || SessionClass::Instance.GameMode == GameMode::LAN || SessionClass::Instance.GameMode == GameMode::Internet;
	int effectivePlayerNum = isMultiplayer ? playerNum : 0;

	std::string idStr = pID ? pID : "";
	std::wstring wID(idStr.begin(), idStr.end());
	std::wstring wName = (pUIName && pUIName[0] != L'\0') ? pUIName : wID;

	std::wostringstream oss;
	if (isDebugEnabled)
	{
		if (effectivePlayerNum > 0)
		{
			oss << L"P" << effectivePlayerNum << L" [" << wID << L"]";
		}
		else
		{
			oss << L"[" << wID << L"]";
		}

		if (!wName.empty() && wName != wID)
		{
			oss << L" (" << wName << L")";
		}
	}
	else
	{
		if (effectivePlayerNum > 0)
		{
			oss << L"[P" << effectivePlayerNum << L"] " << wName;
		}
		else
		{
			oss << wName;
		}
	}
	return oss.str();
}

void ObserverUIClass::RenderFloatingUnitWindows(DSurface* pSurface)
{
	bool isActive = IsActive() || Phobos::Config::DevelopmentCommands;
	if (!isActive || !pSurface || !BitFont::Instance || !BitText::Instance)
		return;

	bool isDebugKeysEnabled = Phobos::Config::DevelopmentCommands;

	Point2D mousePos { 0, 0 };
	if (WWMouseClass::Instance)
	{
		mousePos = Point2D { WWMouseClass::Instance->GetX(), WWMouseClass::Instance->GetY() };
	}

	int maxCardWidth = 360;

	for (auto& win : this->FloatingUnitWindows)
	{
		auto pType = win.pType;
		auto pOwner = win.pOwner;
		if (!pOwner || (!pType && !win.IsSuperweapon && !win.pSuperType))
			continue;

		auto itRow = std::find_if(this->PlayerRows.begin(), this->PlayerRows.end(), [pOwner](const ObserverPlayerRow& r) {
			return r.pHouse == pOwner;
		});

		ColorStruct playerColor { 180, 180, 180 };
		if (itRow != this->PlayerRows.end())
		{
			playerColor = itRow->PlayerColor;
		}

		struct TooltipSegment { std::wstring Text; int Color; };
		struct TooltipLine { std::vector<TooltipSegment> Segments; int Width; int Height; };

		std::vector<TooltipLine> lines;
		int textWidth = 0;
		int textHeight = 0;

		auto addLineSegments = [&](const std::vector<TooltipSegment>& segs) {
			if (segs.empty()) return;
			int lineW = 0;
			int maxH = 0;
			for (const auto& seg : segs)
			{
				int w = 0, h = 0;
				BitFont::Instance->GetTextDimension(seg.Text.c_str(), &w, &h, maxCardWidth);
				lineW += w;
				maxH = std::max(maxH, h);
			}
			lines.push_back({ segs, lineW, maxH });
			textWidth = std::max(textWidth, lineW);
			textHeight += maxH + 2;
		};

		auto addLine = [&](const std::wstring& textStr, int color) {
			addLineSegments({ { textStr, color } });
		};

		BuildingClass* pBld = IsBuildingValidAndAlive(win.pTargetBuilding) ? win.pTargetBuilding : nullptr;
		TechnoClass* pTech = IsTechnoValidAndAlive(win.pTargetTechno) ? win.pTargetTechno : nullptr;
		TechnoTypeClass* pTargetType = win.pType;
		TechnoTypeClass* pBaseType = nullptr;
		if (pBld)
		{
			pBaseType = pBld->Type;
		}
		else if (pTech)
		{
			pBaseType = pTech->GetTechnoType();
		}
		else
		{
			pBaseType = pTargetType;
		}

		FactoryClass* pFact = (pBld && pBld->Factory) ? pBld->Factory : nullptr;
		if (!pFact && win.IsProductionItem && pTargetType)
		{
			pFact = FactoryClass::FindByOwnerAndProduct(pOwner, pTargetType);
		}

		if (!pBld && pFact)
		{
			for (auto pBldObj : BuildingClass::Array)
			{
				if (pBldObj && pBldObj->Owner == pOwner && pBldObj->IsAlive && !pBldObj->InLimbo && pBldObj->Factory == pFact)
				{
					pBld = pBldObj;
					break;
				}
			}
		}

		TechnoTypeClass* pCurProdType = (pFact && pFact->Object) ? pFact->Object->GetTechnoType() : nullptr;

		if (win.IsProductionItem && pFact)
		{
			if (pCurProdType)
			{
				win.pType = pCurProdType;
				pTargetType = pCurProdType;
			}
		}

		bool isProductionView = win.IsProductionItem;
		
		TechnoTypeClass* pCameoType = nullptr;
		if (isProductionView)
		{
			pCameoType = pTargetType ? pTargetType : pCurProdType;
		}
		else
		{
			pCameoType = pBaseType;
		}

		TechnoTypeClass* pTitleType = nullptr;
		if (isProductionView)
		{
			pTitleType = pTargetType ? pTargetType : pCurProdType;
		}
		else if (pBld)
		{
			pTitleType = pBld->Type;
		}
		else
		{
			pTitleType = pBaseType;
		}
		if (!pTitleType) pTitleType = pBaseType;

		if (win.IsSuperweapon || win.pSuperType)
		{
			SuperWeaponTypeClass* pSWType = win.pSuperType;
			SuperClass* pSuper = win.pSuper;

			// Title Line: Superweapon Name
			int pNum = (itRow != this->PlayerRows.end() && itRow->PlayerNumber > 0) ? itRow->PlayerNumber : 0;
			std::string swId = pSWType ? pSWType->get_ID() : "";
			std::wstring titleStr = FormatObjectNameWithDebug(pNum, swId.c_str(), pSWType ? pSWType->UIName : nullptr, isDebugKeysEnabled);
			addLine(titleStr, Drawing::RGB_To_Int(255, 255, 255));

			// Owner Line in Singleplayer / Campaign
			bool isMultiplayer = SessionClass::Instance.GameMode == GameMode::Skirmish || SessionClass::Instance.GameMode == GameMode::LAN || SessionClass::Instance.GameMode == GameMode::Internet;
			if (!isMultiplayer && pOwner)
			{
				std::string ownerPlain = pOwner->PlainName;
				if (ownerPlain.empty()) ownerPlain = pOwner->get_ID();
				std::wstring wOwnerPlain(ownerPlain.begin(), ownerPlain.end());
				std::wstring ownerLine = L"Owner: " + wOwnerPlain + L" (" + pOwner->Type->UIName + L")";
				addLine(ownerLine, Drawing::RGB_To_Int(200, 200, 200));
			}

			// Cooldown Line: Cooldown: 01:45 / 05:00
			int totalFrames = 0;
			if (pSuper)
			{
				totalFrames = pSuper->GetRechargeTime();
			}
			else if (pSWType)
			{
				totalFrames = pSWType->RechargeTime;
			}
			int framesLeft = pSuper ? pSuper->RechargeTimer.GetTimeLeft() : 0;

			int secsLeft = (framesLeft + 14) / 15;
			int minsLeft = secsLeft / 60;
			secsLeft %= 60;

			int secsTotal = (totalFrames + 14) / 15;
			int minsTotal = secsTotal / 60;
			secsTotal %= 60;

			wchar_t cdBuf[64];
			swprintf_s(cdBuf, L"%02d:%02d / %02d:%02d", minsLeft, secsLeft, minsTotal, secsTotal);

			std::wostringstream cdOss;
			cdOss << L"Cooldown: " << cdBuf;
			int cdColor = (framesLeft == 0) ? Drawing::RGB_To_Int(0, 255, 0) : Drawing::RGB_To_Int(100, 220, 255);
			addLine(cdOss.str(), cdColor);

			// Power Line (ONLY shown when superweapon requires power AND owner is in low power state)
			if (pSWType && pSWType->IsPowered && pOwner && pOwner->PowerOutput < pOwner->PowerDrain)
			{
				addLine(L"Power: Low Power", Drawing::RGB_To_Int(255, 50, 50));
			}

			// Coords Line
			BuildingClass* pSWBld = IsBuildingValidAndAlive(win.pTargetBuilding) ? win.pTargetBuilding : nullptr;
			if (!pSWBld && pOwner && pSWType)
			{
				for (int b = 0; b < BuildingClass::Array.Count; ++b)
				{
					auto pBldObj = BuildingClass::Array.GetItem(b);
					if (pBldObj && pBldObj->Owner == pOwner && pBldObj->IsAlive && !pBldObj->InLimbo && pBldObj->Type)
					{
						if (pBldObj->Type->SuperWeapon == pSWType->ArrayIndex || pBldObj->Type->SuperWeapon2 == pSWType->ArrayIndex)
						{
							pSWBld = pBldObj;
							break;
						}
					}
				}
			}

			if (pSWBld)
			{
				CellStruct curCell = CellClass::Coord2Cell(pSWBld->GetCenterCoords());
				std::wostringstream locOss;
				locOss << L"Coords: (" << curCell.X << L", " << curCell.Y << L")";
				addLine(locOss.str(), Drawing::RGB_To_Int(200, 200, 200));
			}
		}
		else if (pBaseType)
		{
			// Standard unit/building lines
			int prodProgressPercent = -1;
			if (pFact && pFact->Object)
			{
				prodProgressPercent = GetFactoryProgressPercent(pFact);
			}

			// Live snapshot tracking while object is alive
			if (pTech)
			{
				win.IsDestroyed = false;
				win.LastHP = pTech->Health;
				win.LastMaxHP = pBaseType ? pBaseType->Strength : pTech->Health;
				win.LastCoords = CellClass::Coord2Cell(pTech->GetCenterCoords());
				win.LastMission = GetMissionNameString(pTech->GetCurrentMission());
				win.LastVeterancy = pTech->Veterancy.Veterancy;
			}
			else if (pBld)
			{
				win.IsDestroyed = false;
				win.LastHP = pBld->Health;
				win.LastMaxHP = pBld->Type ? pBld->Type->Strength : (pBaseType ? pBaseType->Strength : pBld->Health);
				win.LastCoords = CellClass::Coord2Cell(pBld->GetCenterCoords());
				win.LastMission = L"";
				win.LastVeterancy = pBld->Veterancy.Veterancy;
			}
			else if (win.pTargetTechno || win.pTargetBuilding)
			{
				win.IsDestroyed = true;
			}

			// 1. Factory Building Line Determination
			BuildingTypeClass* pFactoryBldType = nullptr;
			if (pBld && pBld->Type)
			{
				pFactoryBldType = pBld->Type;
			}
			else if (pFact && pFact->Object && pFact->Object->GetTechnoType())
			{
				pFactoryBldType = abstract_cast<BuildingTypeClass*>(pFact->Object->GetTechnoType());
			}
			else if (pFact && pOwner)
			{
				for (auto pBldObj : pOwner->Buildings)
				{
					if (pBldObj && pBldObj->IsAlive && !pBldObj->InLimbo && pBldObj->Factory == pFact && pBldObj->Type)
					{
						pFactoryBldType = pBldObj->Type;
						break;
					}
				}
			}

			// Title Line (First Line of Card)
			int pNum = (itRow != this->PlayerRows.end() && itRow->PlayerNumber > 0) ? itRow->PlayerNumber : 0;
			std::wstring titleStr;

			if (isProductionView)
			{
				// First Line of Production Card = Factory Name!
				std::string fId = pFactoryBldType ? pFactoryBldType->get_ID() : (pTitleType ? pTitleType->get_ID() : "");
				titleStr = FormatObjectNameWithDebug(pNum, fId.c_str(), pFactoryBldType ? pFactoryBldType->UIName : (pTitleType ? pTitleType->UIName : nullptr), isDebugKeysEnabled);
			}
			else
			{
				std::string tId = pTitleType ? pTitleType->get_ID() : "";
				titleStr = FormatObjectNameWithDebug(pNum, tId.c_str(), pTitleType ? pTitleType->UIName : nullptr, isDebugKeysEnabled);

				if (win.InstanceNumber > 1 && !pFact)
				{
					titleStr += L" #" + std::to_wstring(win.InstanceNumber);
				}
			}

			addLine(titleStr, Drawing::RGB_To_Int(255, 255, 255));

			// Owner Line in Singleplayer / Campaign
			bool isMultiplayer = SessionClass::Instance.GameMode == GameMode::Skirmish || SessionClass::Instance.GameMode == GameMode::LAN || SessionClass::Instance.GameMode == GameMode::Internet;
			if (!isMultiplayer && pOwner)
			{
				std::string ownerPlain = pOwner->PlainName;
				if (ownerPlain.empty()) ownerPlain = pOwner->get_ID();
				std::wstring wOwnerPlain(ownerPlain.begin(), ownerPlain.end());
				std::wstring ownerLine = L"Owner: " + wOwnerPlain + L" (" + pOwner->Type->UIName + L")";
				addLine(ownerLine, Drawing::RGB_To_Int(200, 200, 200));
			}

			// 2. Production Lines
			bool isProducing = (pCurProdType && pFact && pFact->Object);

			if (isProductionView)
			{
				// For production card view: show product name with percentage ONLY if producing!
				if (isProducing)
				{
					int progressPercent = GetFactoryProgressPercent(pFact);

					std::string pId = pCurProdType->get_ID();
					std::wstring prodName = FormatObjectNameWithDebug(0, pId.c_str(), pCurProdType->UIName, isDebugKeysEnabled);

					std::wostringstream prodOss;
					prodOss << prodName << L" (" << progressPercent << L"%)";
					addLine(prodOss.str(), Drawing::RGB_To_Int(100, 220, 255));
				}
			}
			else if (isProducing)
			{
				// For building card on map: show Production: [HTNK] (Rhino Tank) (74%)
				int progressPercent = GetFactoryProgressPercent(pFact);

				std::string pId = pCurProdType->get_ID();
				std::wstring prodName = FormatObjectNameWithDebug(0, pId.c_str(), pCurProdType->UIName, isDebugKeysEnabled);

				std::wostringstream prodOss;
				prodOss << L"Production: " << prodName << L" (" << progressPercent << L"%)";
				addLine(prodOss.str(), Drawing::RGB_To_Int(100, 220, 255));
			}
			else if (pFact || (pBld && pBld->Type && pBld->Type->Factory != AbstractType::None))
			{
				addLine(L"Production: None", Drawing::RGB_To_Int(160, 160, 160));
			}

			// 3. Health & Shield Line
			if (win.IsDestroyed)
			{
				std::wostringstream hpOss;
				hpOss << L"0 / " << (win.LastMaxHP > 0 ? win.LastMaxHP : (pBaseType ? pBaseType->Strength : 1)) << L" (Destroyed)";
				addLineSegments({
					{ L"HP: ", Drawing::RGB_To_Int(200, 200, 200) },
					{ hpOss.str(), Drawing::RGB_To_Int(255, 50, 50) }
				});
			}
			else if (!isProductionView && (pTech || pBld))
			{
				int currentHP = pTech ? pTech->Health : pBld->Health;
				int maxHP = currentHP;
				if (pTech)
				{
					if (pBaseType)
					{
						maxHP = pBaseType->Strength;
					}
				}
				else if (pBld)
				{
					if (pBld->Type)
					{
						maxHP = pBld->Type->Strength;
					}
					else if (pBaseType)
					{
						maxHP = pBaseType->Strength;
					}
				}
				std::wostringstream hpOss;
				hpOss << currentHP << L" / " << maxHP;

				int hpColor = Drawing::RGB_To_Int(0, 255, 0);
				if (currentHP < maxHP / 2) hpColor = Drawing::RGB_To_Int(255, 255, 0);
				if (currentHP < maxHP / 4) hpColor = Drawing::RGB_To_Int(255, 50, 50);

				addLineSegments({
					{ L"HP: ", Drawing::RGB_To_Int(200, 200, 200) },
					{ hpOss.str(), hpColor }
				});
			}

			// Shield Check (ONLY for real map objects)
			int currentShield = 0;
			int maxShield = 0;

			if (!isProductionView)
			{
				auto pTechExt = pTech ? TechnoExt::ExtMap.Find(pTech) : nullptr;
				if (pTechExt && pTechExt->Shield && pTechExt->Shield->GetType())
				{
					currentShield = pTechExt->Shield->GetHP();
					maxShield = pTechExt->Shield->GetType()->Strength.Get();
				}
				else
				{
					auto pExt = pBaseType ? TechnoTypeExt::ExtMap.Find(pBaseType) : nullptr;
					if (pExt && pExt->ShieldType)
					{
						maxShield = pExt->ShieldType->Strength.Get();
						currentShield = maxShield;
					}
				}
			}
			else
			{
				auto pTechExt = pTech ? TechnoExt::ExtMap.Find(pTech) : nullptr;
				if (pTechExt && pTechExt->Shield && pTechExt->Shield->GetType())
				{
					currentShield = pTechExt->Shield->GetHP();
					maxShield = pTechExt->Shield->GetType()->Strength.Get();
				}
				else
				{
					auto pTypeExt = pBaseType ? TechnoTypeExt::ExtMap.Find(pBaseType) : nullptr;
					if (pTypeExt && pTypeExt->ShieldType)
					{
						maxShield = pTypeExt->ShieldType->Strength.Get();
						currentShield = maxShield;
					}
				}
			}

			if (maxShield > 0)
			{
				std::wostringstream shieldOss;
				shieldOss << currentShield << L" / " << maxShield;
				int shieldColor = Drawing::RGB_To_Int(100, 200, 255);
				if (currentShield <= 0) shieldColor = Drawing::RGB_To_Int(160, 160, 160);

				addLineSegments({
					{ L"Shield: ", Drawing::RGB_To_Int(200, 200, 200) },
					{ shieldOss.str(), shieldColor }
				});
			}

			// 4. Location, Mission, Target & Destination Details
			if (win.IsDestroyed)
			{
				std::wostringstream locOss;
				locOss << L"Coords: (" << win.LastCoords.X << L", " << win.LastCoords.Y << L")";
				if (!win.LastMission.empty()) locOss << L"   Mission: " << win.LastMission;
				addLine(locOss.str(), Drawing::RGB_To_Int(180, 180, 180));
			}
			else if (!isProductionView && (pTech || pBld))
			{
				CellStruct curCell = pTech ? CellClass::Coord2Cell(pTech->GetCenterCoords()) : CellClass::Coord2Cell(pBld->GetCenterCoords());

				std::wostringstream locOss;
				locOss << L"Coords: (" << curCell.X << L", " << curCell.Y << L")";
				
				bool showMission = false;
				if (pTech && pTech->WhatAmI() != AbstractType::Building)
				{
					showMission = true;
				}
				else if (pBld && pBld->Type && (pBld->Type->Weapon[0].WeaponType || pBld->Type->Weapon[1].WeaponType))
				{
					showMission = true;
				}

				if (showMission)
				{
					Mission curMission = pTech ? pTech->GetCurrentMission() : pBld->GetCurrentMission();
					locOss << L"   Mission: " << GetMissionNameString(curMission);
				}
				addLine(locOss.str(), Drawing::RGB_To_Int(200, 200, 200));

				AbstractClass* pRawTarget = nullptr;
				if (pTech)
				{
					pRawTarget = pTech->Target;
				}
				else if (pBld)
				{
					pRawTarget = pBld->Target;
				}
				TechnoClass* pTargetTech = abstract_cast<TechnoClass*>(pRawTarget);
				FootClass* pFoot = pTech ? abstract_cast<FootClass*>(pTech) : nullptr;

				if (!pTargetTech && pFoot) pTargetTech = abstract_cast<TechnoClass*>(pFoot->Destination);

				if (pTargetTech && IsTechnoValidAndAlive(pTargetTech))
				{
					HouseClass* pTargetOwner = pTargetTech->Owner;
					int targetPlayerNum = 0;
					auto itTargetRow = std::find_if(this->PlayerRows.begin(), this->PlayerRows.end(), [pTargetOwner](const ObserverPlayerRow& r) {
						return r.pHouse == pTargetOwner;
					});
					if (itTargetRow != this->PlayerRows.end())
						targetPlayerNum = itTargetRow->PlayerNumber;

					TechnoTypeClass* pTType = pTargetTech->GetTechnoType();
					std::wstring targetUName = (pTType && pTType->UIName) ? pTType->UIName : L"";
					if (targetUName.empty() && pTType)
					{
						std::string targetIdStr = pTType->get_ID();
						targetUName = std::wstring(targetIdStr.begin(), targetIdStr.end());
					}

					std::wostringstream targetOss;
					if (targetPlayerNum > 0)
						targetOss << L"Target: [P" << targetPlayerNum << L"] " << targetUName;
					else
						targetOss << L"Target: " << targetUName;

					addLine(targetOss.str(), Drawing::RGB_To_Int(255, 120, 120));

					CellStruct destCell = CellClass::Coord2Cell(pTargetTech->GetCenterCoords());
					if (destCell != CellStruct::Empty && (destCell.X != curCell.X || destCell.Y != curCell.Y))
					{
						int dx = static_cast<int>(curCell.X) - static_cast<int>(destCell.X);
						int dy = static_cast<int>(curCell.Y) - static_cast<int>(destCell.Y);
						double distCells = std::sqrt(dx * dx + dy * dy);

						wchar_t distBuf[32];
						swprintf_s(distBuf, L"%.1f", distCells);

						std::wostringstream destOss;
						destOss << L"Dest: (" << destCell.X << L", " << destCell.Y << L")   Dist: " << distBuf << L" cells";
						addLine(destOss.str(), Drawing::RGB_To_Int(255, 120, 120));
					}
				}
				else if (pRawTarget)
				{
					CellClass* pCellTarget = abstract_cast<CellClass*>(pRawTarget);
					if (pCellTarget)
					{
						CellStruct destCell = pCellTarget->MapCoords;
						int dx = static_cast<int>(curCell.X) - static_cast<int>(destCell.X);
						int dy = static_cast<int>(curCell.Y) - static_cast<int>(destCell.Y);
						double distCells = std::sqrt(dx * dx + dy * dy);

						wchar_t distBuf[32];
						swprintf_s(distBuf, L"%.1f", distCells);

						std::wostringstream destOss;
						destOss << L"Target: Ground (" << destCell.X << L", " << destCell.Y << L")   Dist: " << distBuf << L" cells";
						addLine(destOss.str(), Drawing::RGB_To_Int(255, 120, 120));
					}
				}
				else if (pFoot && pFoot->WaypointCell != CellStruct::Empty && pFoot->WaypointCell.X > 0)
				{
					CellStruct destCell = pFoot->WaypointCell;
					if (destCell.X != curCell.X || destCell.Y != curCell.Y)
					{
						int dx = static_cast<int>(curCell.X) - static_cast<int>(destCell.X);
						int dy = static_cast<int>(curCell.Y) - static_cast<int>(destCell.Y);
						double distCells = std::sqrt(dx * dx + dy * dy);

						wchar_t distBuf[32];
						swprintf_s(distBuf, L"%.1f", distCells);

						std::wostringstream destOss;
						destOss << L"Dest: (" << destCell.X << L", " << destCell.Y << L")   Dist: " << distBuf << L" cells";
						addLine(destOss.str(), Drawing::RGB_To_Int(180, 220, 255));
					}
				}
			}

			// 5. Ammo Check (ONLY for objects with Ammo > 0, placed right after Target/Dest!)
			if (!isProductionView && pBaseType && pBaseType->Ammo > 0)
			{
				int maxAmmo = pBaseType->Ammo;
				int curAmmo = maxAmmo;
				if (pTech)
				{
					curAmmo = pTech->Ammo;
				}
				else if (pBld)
				{
					curAmmo = pBld->Ammo;
				}
				curAmmo = std::clamp(curAmmo, 0, maxAmmo);

				std::wostringstream ammoOss;
				ammoOss << curAmmo << L" / " << maxAmmo;

				int ammoColor = Drawing::RGB_To_Int(100, 220, 255);
				if (curAmmo == 0) ammoColor = Drawing::RGB_To_Int(255, 50, 50);
				else if (curAmmo < maxAmmo) ammoColor = Drawing::RGB_To_Int(255, 255, 0);

				addLineSegments({
					{ L"Ammo: ", Drawing::RGB_To_Int(200, 200, 200) },
					{ ammoOss.str(), ammoColor }
				});
			}

			// 6. Veterancy (if trainable unit, placed right below Ammo!)
			if (!win.IsProductionItem && pBaseType && pBaseType->Trainable)
			{
				float vetVal = 0.0f;
				if (win.IsDestroyed)
				{
					vetVal = win.LastVeterancy;
				}
				else if (pTech)
				{
					vetVal = pTech->Veterancy.Veterancy;
				}
				else if (pBld)
				{
					vetVal = pBld->Veterancy.Veterancy;
				}

				std::wostringstream vetOss;
				int vetColor = Drawing::RGB_To_Int(200, 200, 200);

				std::wstring rankStr = L"Rookie";
				if (vetVal >= 2.0f) { rankStr = L"Elite"; vetColor = Drawing::RGB_To_Int(255, 215, 0); }
				else if (vetVal >= 1.0f) { rankStr = L"Veteran"; vetColor = Drawing::RGB_To_Int(100, 220, 255); }

				wchar_t scoreBuf[32];
				swprintf_s(scoreBuf, L"%.2f", vetVal);

				vetOss << L"Veterancy: " << rankStr << L" (" << scoreBuf << L")";
				addLine(vetOss.str(), vetColor);
			}

			// 5. Total Build Time Line (MM:SS format) & 6. Cost Line (ONLY for production cards IF producing!)
			if (isProductionView && isProducing)
			{
				int totalBuildFrames = 0;
				if (pFact)
				{
					totalBuildFrames = pFact->GetBuildTimeFrames();
				}
				else if (pCurProdType)
				{
					totalBuildFrames = GetTechnoBuildTimeFrames(pCurProdType, pOwner);
				}

				if (totalBuildFrames > 0)
				{
					int buildTimeSecs = (totalBuildFrames + 14) / 15;
					int mins = buildTimeSecs / 60;
					int secs = buildTimeSecs % 60;

					wchar_t timeBuf[32];
					swprintf_s(timeBuf, L"%02d:%02d", mins, secs);

					std::wostringstream btOss;
					btOss << L"Build Time: " << timeBuf;
					addLine(btOss.str(), Drawing::RGB_To_Int(200, 200, 200));
				}

				if (pCurProdType)
				{
					std::wostringstream costOss;
					costOss << L"Cost: $" << pCurProdType->Cost;
					addLine(costOss.str(), Drawing::RGB_To_Int(200, 200, 200));
				}
			}

			// 8. Debug AI Team Info (ONLY rendered if DebugKeysEnabled=yes, and ALWAYS placed at the very end of the card!)
			FootClass* pFoot = pTech ? abstract_cast<FootClass*>(pTech) : nullptr;
			if (isDebugKeysEnabled && pFoot && pFoot->BelongsToATeam() && pFoot->Team)
			{
				auto const pTeam = pFoot->Team;
				auto const pTeamType = pTeam->Type;

				auto formatIdName = [](AbstractTypeClass* pTypeObj) -> std::wstring {
					if (!pTypeObj) return L"";

					std::string idStr = pTypeObj->get_ID();
					std::wstring wID(idStr.begin(), idStr.end());

					std::wstring wName = (pTypeObj->UIName && pTypeObj->UIName[0] != L'\0') ? pTypeObj->UIName : L"";
					if (wName.empty() && pTypeObj->Name[0] != '\0')
					{
						std::string nStr = pTypeObj->Name;
						wName = std::wstring(nStr.begin(), nStr.end());
					}

					if (!wName.empty() && wName != wID)
					{
						return wID + L" (" + wName + L")";
					}
					return wID;
				};

				if (pTeamType)
				{
					std::wstring teamInfo = formatIdName(pTeamType);
					if (!teamInfo.empty())
					{
						addLine(L"Team: " + teamInfo, Drawing::RGB_To_Int(200, 200, 200));
					}
				}

				if (pTeam->CurrentScript && pTeam->CurrentScript->Type)
				{
					std::wstring scriptInfo = formatIdName(pTeam->CurrentScript->Type);
					if (!scriptInfo.empty())
					{
						addLine(L"Script: " + scriptInfo, Drawing::RGB_To_Int(200, 200, 200));
					}
				}

				if (pTeamType && pTeamType->TaskForce)
				{
					std::wstring tfInfo = formatIdName(pTeamType->TaskForce);
					if (!tfInfo.empty())
					{
						addLine(L"Taskforce: " + tfInfo, Drawing::RGB_To_Int(200, 200, 200));
					}
				}

				if (pTeam->CurrentScript && pTeam->CurrentScript->Type)
				{
					int lineNum = pTeam->CurrentScript->CurrentMission;
					if (lineNum >= 0 && lineNum < pTeam->CurrentScript->Type->ActionsCount)
					{
						int action = pTeam->CurrentScript->Type->ScriptActions[lineNum].Action;
						int arg = pTeam->CurrentScript->Type->ScriptActions[lineNum].Argument;

						std::wostringstream lineOss;
						lineOss << L"Script Line " << lineNum << L": " << action << L", " << arg;
						addLine(lineOss.str(), Drawing::RGB_To_Int(200, 200, 200));
					}
					else if (lineNum >= 0)
					{
						std::wostringstream lineOss;
						lineOss << L"Script Line " << lineNum;
						addLine(lineOss.str(), Drawing::RGB_To_Int(200, 200, 200));
					}
				}
			}
		}

		int cameoBoxW = 60;
		int cameoBoxH = 48;
		int boxPadding = 8;

		// Calculate total layout width & height
		int contentLeftMargin = cameoBoxW + 16;
		int boxWidth = std::max(260, textWidth + contentLeftMargin + boxPadding + 20); // 20px for close btn
		int boxHeight = std::max(cameoBoxH + boxPadding * 2 + 4, textHeight + boxPadding * 2 + 4);

		// Update WindowRect, CloseBtnRect, CameoClickRect
		win.WindowRect = RectangleStruct { win.Position.X, win.Position.Y, boxWidth, boxHeight };
		win.CloseBtnRect = RectangleStruct { win.Position.X + boxWidth - 20, win.Position.Y + 4, 16, 16 };
		win.CameoClickRect = RectangleStruct { win.Position.X + boxPadding, win.Position.Y + boxPadding, cameoBoxW, cameoBoxH };

		ColorStruct bgColor { 0, 0, 0 };
		// Translucent panel background
		pSurface->FillRectTrans(&win.WindowRect, &bgColor, 75);

		// Outer border in player's color
		pSurface->DrawRect(&win.WindowRect, Drawing::RGB_To_Int(playerColor.R, playerColor.G, playerColor.B));

		// Top-Left Cameo rendering using DrawCameoItem
		bool isCameoHovered = mousePos.X >= win.CameoClickRect.X && mousePos.X <= (win.CameoClickRect.X + win.CameoClickRect.Width)
			&& mousePos.Y >= win.CameoClickRect.Y && mousePos.Y <= (win.CameoClickRect.Y + win.CameoClickRect.Height);

		ObserverCameoItem cameoItem;
		if (win.IsSuperweapon || win.pSuperType)
		{
			cameoItem.pSuperType = win.pSuperType;
			cameoItem.pSuper = win.pSuper;
			cameoItem.IsSuperweapon = true;
		}
		else
		{
			bool isProducing = (isProductionView && pFact && pFact->Object && pCurProdType);
			if (isProductionView)
			{
				if (isProducing)
				{
					cameoItem.pType = pCurProdType;
					cameoItem.IsProduction = true;
					cameoItem.ProgressPercent = GetFactoryProgressPercent(pFact);
				}
				else
				{
					cameoItem.pType = nullptr; // no Cameo when not producing!
					cameoItem.IsProduction = false;
					cameoItem.ProgressPercent = -1;
				}
			}
			else
			{
				cameoItem.pType = pCameoType;
			}
		}
		cameoItem.pOwner = pOwner;
		cameoItem.Count = 1;
		cameoItem.DisplayRect = win.CameoClickRect;

		this->DrawCameoItem(pSurface, cameoItem, isCameoHovered, win.CameoClickRect, playerColor);

		// Close Button [X] rendering
		bool isCloseHovered = mousePos.X >= win.CloseBtnRect.X && mousePos.X <= (win.CloseBtnRect.X + win.CloseBtnRect.Width)
			&& mousePos.Y >= win.CloseBtnRect.Y && mousePos.Y <= (win.CloseBtnRect.Y + win.CloseBtnRect.Height);

		int closeBgColor = isCloseHovered ? Drawing::RGB_To_Int(220, 40, 40) : Drawing::RGB_To_Int(60, 60, 60);
		pSurface->FillRect(&win.CloseBtnRect, closeBgColor);
		pSurface->DrawRect(&win.CloseBtnRect, Drawing::RGB_To_Int(140, 140, 140));

		int xW = 0, xH = 0;
		BitFont::Instance->GetTextDimension(L"X", &xW, &xH, win.CloseBtnRect.Width);
		Point2D closeTxtPt { win.CloseBtnRect.X + (win.CloseBtnRect.Width - xW) / 2, win.CloseBtnRect.Y + (win.CloseBtnRect.Height - xH) / 2 };
		pSurface->DrawTextA(L"X", &DSurface::ViewBounds, &closeTxtPt, Drawing::RGB_To_Int(255, 255, 255), 0, TextPrintType::Point8);

		// Render text lines inside window (offset by cameo box width)
		LTRBStruct oldBounds = BitFont::Instance->Bounds;
		WORD oldColor = BitFont::Instance->Color;
		bool oldField41 = BitFont::Instance->field_41;

		LTRBStruct ltrbBounds = { win.WindowRect.X, win.WindowRect.Y, win.WindowRect.X + win.WindowRect.Width, win.WindowRect.Y + win.WindowRect.Height };
		BitFont::Instance->field_41 = 1;
		BitFont::Instance->SetBounds(&ltrbBounds);

		int currentY = win.WindowRect.Y + boxPadding;
		for (const auto& line : lines)
		{
			int currentX = win.WindowRect.X + contentLeftMargin;
			for (const auto& seg : line.Segments)
			{
				int w = 0, h = 0;
				BitFont::Instance->GetTextDimension(seg.Text.c_str(), &w, &h, maxCardWidth);
				BitFont::Instance->Color = static_cast<WORD>(seg.Color);
				BitText::Instance->DrawText(
					BitFont::Instance,
					pSurface,
					seg.Text.c_str(),
					currentX,
					currentY,
					w,
					line.Height,
					0, 0, 0
				);
				currentX += w;
			}
			currentY += line.Height + 2;
		}

		BitFont::Instance->field_41 = oldField41 ? 1 : 0;
		BitFont::Instance->Color = oldColor;
		BitFont::Instance->SetBounds(&oldBounds);
	}
}

void ObserverUIClass::RenderFloatingWindows(DSurface* pSurface)
{
	bool isActive = IsActive() || Phobos::Config::DevelopmentCommands;
	if (!isActive || !pSurface || !BitFont::Instance || !BitText::Instance)
		return;

	Point2D mousePos { 0, 0 };
	if (WWMouseClass::Instance)
	{
		mousePos = Point2D { WWMouseClass::Instance->GetX(), WWMouseClass::Instance->GetY() };
	}

	int maxCardWidth = 350;

	for (auto& win : this->FloatingWindows)
	{
		auto pHouse = win.pHouse;
		if (!pHouse || !pHouse->Type)
			continue;

		auto itRow = std::find_if(this->PlayerRows.begin(), this->PlayerRows.end(), [pHouse](const ObserverPlayerRow& r) {
			return r.pHouse == pHouse;
		});

		ColorStruct playerColor { 180, 180, 180 };
		if (itRow != this->PlayerRows.end())
		{
			playerColor = itRow->PlayerColor;
		}

		struct TooltipSegment { std::wstring Text; int Color; };
		struct TooltipLine { std::vector<TooltipSegment> Segments; int Width; int Height; };

		std::vector<TooltipLine> lines;
		int textWidth = 0;
		int textHeight = 0;

		auto addLineSegments = [&](const std::vector<TooltipSegment>& segs) {
			if (segs.empty()) return;
			int lineW = 0;
			int maxH = 0;
			for (const auto& seg : segs)
			{
				int w = 0, h = 0;
				BitFont::Instance->GetTextDimension(seg.Text.c_str(), &w, &h, maxCardWidth);
				lineW += w;
				maxH = std::max(maxH, h);
			}
			lines.push_back({ segs, lineW, maxH });
			textWidth = std::max(textWidth, lineW);
			textHeight += maxH + 2;
		};

		auto addLine = [&](const std::wstring& textStr, int color) {
			addLineSegments({ { textStr, color } });
		};

		// Title Line: Player Number, Player Name & Country Name
		std::string plainNameStr = pHouse->PlainName;
		if (plainNameStr.empty())
			plainNameStr = pHouse->get_ID();
		std::wstring wPlainName(plainNameStr.begin(), plainNameStr.end());

		std::wstring controlStr = L"";
		if (!pHouse->IsControlledByHuman())
		{
			switch (pHouse->AIDifficulty)
			{
			case AIDifficulty::Easy: controlStr = L" [AI Easy]"; break;
			case AIDifficulty::Normal: controlStr = L" [AI Normal]"; break;
			case AIDifficulty::Hard: controlStr = L" [AI Hard]"; break;
			default: controlStr = L" [AI]"; break;
			}
		}

		bool isMultiplayer = SessionClass::Instance.GameMode == GameMode::Skirmish || SessionClass::Instance.GameMode == GameMode::LAN || SessionClass::Instance.GameMode == GameMode::Internet;
		bool isDebugEnabled = Phobos::Config::DevelopmentCommands;
		std::wostringstream nameOss;

		std::string houseIdStr = pHouse->get_ID();
		std::wstring wHouseID(houseIdStr.begin(), houseIdStr.end());

		if (isMultiplayer && itRow != this->PlayerRows.end() && itRow->PlayerNumber > 0)
		{
			if (isDebugEnabled)
			{
				nameOss << L"P" << itRow->PlayerNumber << L" [" << wHouseID << L"] " << wPlainName << L" (" << pHouse->Type->UIName << L")" << controlStr;
			}
			else
			{
				nameOss << L"[P" << itRow->PlayerNumber << L"] " << wPlainName << L" (" << pHouse->Type->UIName << L")" << controlStr;
			}
		}
		else
		{
			if (isDebugEnabled)
			{
				nameOss << L"[" << wHouseID << L"] " << wPlainName << L" (" << pHouse->Type->UIName << L")" << controlStr;
			}
			else
			{
				nameOss << wPlainName << L" (" << pHouse->Type->UIName << L")" << controlStr;
			}
		}
		addLine(nameOss.str(), Drawing::RGB_To_Int(255, 255, 255));

		// Money Line
		std::wostringstream moneyOss;
		moneyOss << L"Credits: $" << pHouse->Available_Money();
		addLine(moneyOss.str(), Drawing::RGB_To_Int(200, 200, 200));

		// Income Rate Line
		if (itRow != this->PlayerRows.end())
		{
			std::wostringstream rateValOss;
			int valColor = Drawing::RGB_To_Int(180, 180, 180);
			if (itRow->IncomeRatePerMin > 0)
			{
				rateValOss << L"+$" << itRow->IncomeRatePerMin;
				valColor = Drawing::RGB_To_Int(0, 255, 0);
			}
			else if (itRow->IncomeRatePerMin < 0)
			{
				rateValOss << L"-$" << std::abs(itRow->IncomeRatePerMin);
				valColor = Drawing::RGB_To_Int(255, 90, 90);
			}
			else
			{
				rateValOss << L"+$0";
			}
			addLineSegments({
				{ L"Economy/min: ", Drawing::RGB_To_Int(200, 200, 200) },
				{ rateValOss.str(), valColor }
			});
		}

		// Power Line
		int powerOutput = pHouse->PowerOutput;
		int powerDrain = pHouse->PowerDrain;
		int balance = powerOutput - powerDrain;

		std::wostringstream powerMainOss;
		powerMainOss << powerDrain << L" / " << powerOutput << L" (";

		std::wostringstream balanceOss;
		int balanceColor = Drawing::RGB_To_Int(180, 180, 180);
		if (balance > 0)
		{
			balanceOss << L"+" << balance;
			balanceColor = Drawing::RGB_To_Int(0, 255, 0);
		}
		else if (balance < 0)
		{
			balanceOss << balance;
			balanceColor = Drawing::RGB_To_Int(255, 50, 50);
		}
		else
		{
			balanceOss << L"+0";
		}

		addLineSegments({
			{ L"Power: ", Drawing::RGB_To_Int(200, 200, 200) },
			{ powerMainOss.str(), Drawing::RGB_To_Int(200, 200, 200) },
			{ balanceOss.str(), balanceColor },
			{ L")", Drawing::RGB_To_Int(200, 200, 200) }
		});

		// Debug-only AI / Tech lines (ONLY if DebugKeysEnabled=yes in rulesmd.ini)
		bool isDebugKeysEnabled = Phobos::Config::DevelopmentCommands;
		if (isDebugKeysEnabled)
		{
			// 1. IQLevel (only if AI)
			if (!pHouse->IsControlledByHuman())
			{
				addLine(L"AI's IQ Level: " + std::to_wstring(pHouse->IQLevel2), Drawing::RGB_To_Int(200, 200, 200));
			}

			// 2. TechLevel (always when debug)
			addLine(L"Tech Level: " + std::to_wstring(pHouse->TechLevel), Drawing::RGB_To_Int(200, 200, 200));

			// 3. Production (only if AI and false)
			if (!pHouse->IsControlledByHuman() && !pHouse->Production)
			{
				addLineSegments({
					{ L"AI Production: ", Drawing::RGB_To_Int(200, 200, 200) },
					{ L"Disabled", Drawing::RGB_To_Int(255, 90, 90) }
				});
			}

			// 4. AITriggersActive (only if AI and false)
			if (!pHouse->IsControlledByHuman() && !pHouse->AITriggersActive)
			{
				addLineSegments({
					{ L"AI Triggers: ", Drawing::RGB_To_Int(200, 200, 200) },
					{ L"Disabled", Drawing::RGB_To_Int(255, 90, 90) }
				});
			}

			// 5. AutoBaseBuilding (only if AI and false)
			if (!pHouse->IsControlledByHuman() && !pHouse->AutoBaseBuilding)
			{
				addLineSegments({
					{ L"Auto Base Building: ", Drawing::RGB_To_Int(200, 200, 200) },
					{ L"Disabled", Drawing::RGB_To_Int(255, 90, 90) }
				});
			}

			// 5b. Active AI Teams (only if AI and > 0)
			if (!pHouse->IsControlledByHuman())
			{
				int activeAITeams = 0;
				for (int k = 0; k < TeamClass::Array.Count; ++k)
				{
					auto pTeam = TeamClass::Array.GetItem(k);
					if (pTeam && pTeam->Owner == pHouse)
					{
						activeAITeams++;
					}
				}
				if (activeAITeams > 0)
				{
					addLine(L"Active AI Teams: " + std::to_wstring(activeAITeams), Drawing::RGB_To_Int(200, 200, 200));
				}
			}
		}

		// 6. Defeated (only on floating card if defeated)
		if (pHouse->Defeated)
		{
			addLineSegments({
				{ L"Status: ", Drawing::RGB_To_Int(200, 200, 200) },
				{ L"Defeated", Drawing::RGB_To_Int(255, 50, 50) }
			});
		}

		// 7. Refineries (formerly Resource Destinations) (only if > 0)
		if (pHouse->CountResourceDestinations > 0)
		{
			addLine(L"Refineries: " + std::to_wstring(pHouse->CountResourceDestinations), Drawing::RGB_To_Int(200, 200, 200));
		}

		// 8. War Factories (only if > 0)
		if (pHouse->CountWarfactories > 0)
		{
			addLine(L"War Factories: " + std::to_wstring(pHouse->CountWarfactories), Drawing::RGB_To_Int(200, 200, 200));
		}

		// 9. Barracks & Helipads & Airport Docks
		int barracksCount = 0;
		int helipadsCount = 0;
		for (auto const pBld : pHouse->Buildings)
		{
			if (pBld && pBld->IsAlive && !pBld->InLimbo && pBld->Type)
			{
				if (pBld->Type->Factory == AbstractType::InfantryType || pBld->Type->GDIBarracks || pBld->Type->NODBarracks || pBld->Type->YuriBarracks)
				{
					barracksCount++;
				}
				if (pBld->Type->Helipad || pBld->Type->Factory == AbstractType::AircraftType || pBld->Type->UnitReload)
				{
					helipadsCount++;
				}
			}
		}
		if (barracksCount > 0)
		{
			addLine(L"Barracks: " + std::to_wstring(barracksCount), Drawing::RGB_To_Int(200, 200, 200));
		}
		if (helipadsCount > 0 || pHouse->AirportDocks > 0)
		{
			std::wstring helipadsLine = L"Helipads: " + std::to_wstring(helipadsCount);
			if (pHouse->AirportDocks > 0)
			{
				helipadsLine += L" (Docks: " + std::to_wstring(pHouse->AirportDocks) + L")";
			}
			addLine(helipadsLine, Drawing::RGB_To_Int(200, 200, 200));
		}

		// 10. Total Objects Counts (only if > 0)
		if (pHouse->OwnedBuildings > 0)
		{
			addLine(L"Total Buildings: " + std::to_wstring(pHouse->OwnedBuildings), Drawing::RGB_To_Int(200, 200, 200));
		}
		if (pHouse->OwnedInfantry > 0)
		{
			addLine(L"Total Infantry: " + std::to_wstring(pHouse->OwnedInfantry), Drawing::RGB_To_Int(200, 200, 200));
		}
		if (pHouse->OwnedUnits > 0)
		{
			addLine(L"Total Units: " + std::to_wstring(pHouse->OwnedUnits), Drawing::RGB_To_Int(200, 200, 200));
		}
		if (pHouse->OwnedAircraft > 0)
		{
			addLine(L"Total Aircraft: " + std::to_wstring(pHouse->OwnedAircraft), Drawing::RGB_To_Int(200, 200, 200));
		}
		if (pHouse->OwnedNavy > 0)
		{
			addLine(L"Total Navy: " + std::to_wstring(pHouse->OwnedNavy), Drawing::RGB_To_Int(200, 200, 200));
		}

		// 12. Killed Objects Counts (only if > 0)
		int killedUnits = pHouse->KilledUnitTypes.GetUnitCount() + pHouse->KilledInfantryTypes.GetUnitCount() + pHouse->KilledAircraftTypes.GetUnitCount();
		if (killedUnits > 0)
		{
			addLine(L"Killed Units: " + std::to_wstring(killedUnits), Drawing::RGB_To_Int(200, 200, 200));
		}
		int killedBuildings = pHouse->KilledBuildingTypes.GetUnitCount();
		if (killedBuildings > 0)
		{
			addLine(L"Killed Buildings: " + std::to_wstring(killedBuildings), Drawing::RGB_To_Int(200, 200, 200));
		}

		// Allies Line (ONLY if allies exist)
		std::wstring alliesStr = L"";
		for (const auto& r : this->PlayerRows)
		{
			if (r.pHouse && r.pHouse != pHouse && pHouse->IsAlliedWith(r.pHouse))
			{
				if (!alliesStr.empty()) alliesStr += L", ";

				std::string allyIdStr = r.pHouse->get_ID();
				std::wstring wAllyID(allyIdStr.begin(), allyIdStr.end());

				std::string allyPlainStr = r.pHouse->PlainName;
				if (allyPlainStr.empty()) allyPlainStr = allyIdStr;
				std::wstring wAllyPlain(allyPlainStr.begin(), allyPlainStr.end());

				if (isMultiplayer && r.PlayerNumber > 0)
				{
					if (isDebugEnabled)
					{
						alliesStr += L"P" + std::to_wstring(r.PlayerNumber) + L" [" + wAllyID + L"]";
					}
					else
					{
						alliesStr += L"P" + std::to_wstring(r.PlayerNumber);
					}
				}
				else
				{
					// Singleplayer / Campaign mode: use House Name (and [ID] in front if debug mode)
					if (isDebugEnabled)
					{
						alliesStr += L"[" + wAllyID + L"] " + wAllyPlain;
					}
					else
					{
						alliesStr += wAllyPlain;
					}
				}
			}
		}
		if (!alliesStr.empty())
		{
			addLineSegments({
				{ L"Allies: ", Drawing::RGB_To_Int(200, 200, 200) },
				{ alliesStr, Drawing::RGB_To_Int(100, 220, 255) }
			});
		}

		if (!pHouse->IsControlledByHuman())
		{
			auto pEnemyHouse = GetTargetEnemy(pHouse);
			if (pEnemyHouse && pEnemyHouse->Type)
			{
				ColorStruct enemyColor = GetHouseColor(pEnemyHouse);
				std::string enemyPlainName = pEnemyHouse->PlainName;
				if (enemyPlainName.empty())
					enemyPlainName = pEnemyHouse->get_ID();
				std::wstring wEnemyPlain(enemyPlainName.begin(), enemyPlainName.end());

				std::string enemyIdStr = pEnemyHouse->get_ID();
				std::wstring wEnemyID(enemyIdStr.begin(), enemyIdStr.end());

				std::wostringstream enemyValOss;
				auto itEnemyRow = std::find_if(this->PlayerRows.begin(), this->PlayerRows.end(), [pEnemyHouse](const ObserverPlayerRow& r) {
					return r.pHouse == pEnemyHouse;
				});
				if (isMultiplayer && itEnemyRow != this->PlayerRows.end() && itEnemyRow->PlayerNumber > 0)
				{
					if (isDebugEnabled)
					{
						enemyValOss << L"P" << itEnemyRow->PlayerNumber << L" [" << wEnemyID << L"] " << wEnemyPlain << L" (" << pEnemyHouse->Type->UIName << L")";
					}
					else
					{
						enemyValOss << L"[P" << itEnemyRow->PlayerNumber << L"] " << wEnemyPlain << L" (" << pEnemyHouse->Type->UIName << L")";
					}
				}
				else
				{
					if (isDebugEnabled)
					{
						enemyValOss << L"[" << wEnemyID << L"] " << wEnemyPlain << L" (" << pEnemyHouse->Type->UIName << L")";
					}
					else
					{
						enemyValOss << wEnemyPlain << L" (" << pEnemyHouse->Type->UIName << L")";
					}
				}

				addLineSegments({
					{ L"Target Enemy: ", Drawing::RGB_To_Int(200, 200, 200) },
					{ enemyValOss.str(), Drawing::RGB_To_Int(enemyColor.R, enemyColor.G, enemyColor.B) }
				});
			}
			else
			{
				addLineSegments({
					{ L"Target Enemy: ", Drawing::RGB_To_Int(200, 200, 200) },
					{ L"None", Drawing::RGB_To_Int(180, 180, 180) }
				});
			}
		}

		int boxPadding = 8;
		int boxWidth = textWidth + boxPadding * 2 + 20; // 20px extra space for top-right [X] close button
		int boxHeight = textHeight + boxPadding * 2 + 4;

		// Update WindowRect & CloseBtnRect
		win.WindowRect = RectangleStruct { win.Position.X, win.Position.Y, boxWidth, boxHeight };
		win.CloseBtnRect = RectangleStruct { win.Position.X + boxWidth - 20, win.Position.Y + 4, 16, 16 };

		ColorStruct bgColor { 0, 0, 0 };
		// Translucent panel background
		pSurface->FillRectTrans(&win.WindowRect, &bgColor, 75);
		
		// Outer border in player's color
		pSurface->DrawRect(&win.WindowRect, Drawing::RGB_To_Int(playerColor.R, playerColor.G, playerColor.B));

		// Close Button [X] rendering
		bool isCloseHovered = mousePos.X >= win.CloseBtnRect.X && mousePos.X <= (win.CloseBtnRect.X + win.CloseBtnRect.Width)
			&& mousePos.Y >= win.CloseBtnRect.Y && mousePos.Y <= (win.CloseBtnRect.Y + win.CloseBtnRect.Height);

		int closeBgColor = isCloseHovered ? Drawing::RGB_To_Int(220, 40, 40) : Drawing::RGB_To_Int(60, 60, 60);
		pSurface->FillRect(&win.CloseBtnRect, closeBgColor);
		pSurface->DrawRect(&win.CloseBtnRect, Drawing::RGB_To_Int(140, 140, 140));

		int xW = 0, xH = 0;
		BitFont::Instance->GetTextDimension(L"X", &xW, &xH, win.CloseBtnRect.Width);
		Point2D closeTxtPt { win.CloseBtnRect.X + (win.CloseBtnRect.Width - xW) / 2, win.CloseBtnRect.Y + (win.CloseBtnRect.Height - xH) / 2 };
		pSurface->DrawTextA(L"X", &DSurface::ViewBounds, &closeTxtPt, Drawing::RGB_To_Int(255, 255, 255), 0, TextPrintType::Point8);

		// Render text lines inside window
		LTRBStruct oldBounds = BitFont::Instance->Bounds;
		WORD oldColor = BitFont::Instance->Color;
		bool oldField41 = BitFont::Instance->field_41;

		LTRBStruct ltrbBounds = { win.WindowRect.X, win.WindowRect.Y, win.WindowRect.X + win.WindowRect.Width, win.WindowRect.Y + win.WindowRect.Height };
		BitFont::Instance->field_41 = 1;
		BitFont::Instance->SetBounds(&ltrbBounds);

		int currentY = win.WindowRect.Y + boxPadding;
		for (const auto& line : lines)
		{
			int currentX = win.WindowRect.X + boxPadding;
			for (const auto& seg : line.Segments)
			{
				int w = 0, h = 0;
				BitFont::Instance->GetTextDimension(seg.Text.c_str(), &w, &h, maxCardWidth);
				BitFont::Instance->Color = static_cast<WORD>(seg.Color);
				BitText::Instance->DrawText(
					BitFont::Instance,
					pSurface,
					seg.Text.c_str(),
					currentX,
					currentY,
					w,
					line.Height,
					0, 0, 0
				);
				currentX += w;
			}
			currentY += line.Height + 2;
		}

		BitFont::Instance->field_41 = oldField41 ? 1 : 0;
		BitFont::Instance->Color = oldColor;
		BitFont::Instance->SetBounds(&oldBounds);
	}
}

static bool DrawImage(
	DSurface* pSurface,
	RectangleStruct destinationRect,
	BSurface* pPCXSurface,
	SHPStruct* fileSHP,
	ConvertClass* pPalette,
	int frameIndex = 0,
	int zAdjust = -2,
	BlitterFlags blitterFlags = BlitterFlags::None)
{
	if (!pSurface || (!pPCXSurface && !fileSHP))
		return false;

	bool painted = false;

	// Prioritize drawing the PCX file if it's provided and valid
	if (pPCXSurface && pPCXSurface->GetWidth() > 0 && pPCXSurface->GetHeight() > 0)
	{
		PCX::Instance.BlitToSurface(&destinationRect, pSurface, pPCXSurface);
		painted = true;
	}
	// Otherwise, if an SHP is provided, draw it
	else if (fileSHP)
	{
		ConvertClass* pPal = pPalette ? pPalette : FileSystem::CAMEO_PAL;
		if (!pPal) pPal = FileSystem::UNITx_PAL;

		Point2D location = { destinationRect.X, destinationRect.Y };
		pSurface->DrawSHP(pPal, fileSHP, frameIndex, &location, &DSurface::ViewBounds, BlitterFlags::bf_400, 0, zAdjust, ZGradient::Ground, 1000, 0, nullptr, 0, 0, 0);
		painted = true;
	}

	return painted;
}

void ObserverUIClass::DrawCameoItem(DSurface* pSurface, const ObserverCameoItem& item, bool isHovered, const RectangleStruct& clipRect, ColorStruct playerColor)
{
	if (!pSurface)
		return;

	// Calculate intersection clip rect between item display area and section panel clip rect
	RectangleStruct drawRect;
	if (!IntersectRect(item.DisplayRect, clipRect, drawRect))
		return;

	// 1. Try PCX cameo from TechnoTypeExt / SWTypeExt or CameoFile or ID
	BSurface* pPCXSurface = nullptr;
	SHPStruct* pFileSHP = nullptr;

	if (item.IsSuperweapon && item.pSuperType)
	{
		auto pSWExt = SWTypeExt::ExtMap.Find(item.pSuperType);
		if (pSWExt && pSWExt->SidebarPCX.Exists())
		{
			pPCXSurface = pSWExt->SidebarPCX.GetSurface();
		}

		const char* imgFile = item.pSuperType->SidebarImageFile;
		if (!pPCXSurface && imgFile[0] != '\0')
		{
			PhobosPCXFile pcxFile(imgFile);
			if (pcxFile.Exists())
				pPCXSurface = pcxFile.GetSurface();

			if (!pPCXSurface)
			{
				char pcxName[64];
				sprintf_s(pcxName, "%s.pcx", imgFile);
				_strlwr_s(pcxName);
				PhobosPCXFile pcxFile2(pcxName);
				if (pcxFile2.Exists())
					pPCXSurface = pcxFile2.GetSurface();
			}

			if (!pPCXSurface)
			{
				pFileSHP = FileSystem::LoadSHPFile(imgFile);
				if (!pFileSHP)
				{
					char shpName[64];
					sprintf_s(shpName, "%s.shp", imgFile);
					_strlwr_s(shpName);
					pFileSHP = FileSystem::LoadSHPFile(shpName);
				}
			}
		}

		if (!pPCXSurface && !pFileSHP)
		{
			pFileSHP = item.pSuperType->SidebarImage;
		}
	}
	else if (item.pType)
	{
		// 1. Check TechnoTypeExt PCX cameos
		auto pTypeExt = TechnoTypeExt::ExtMap.Find(item.pType);
		if (pTypeExt)
		{
			if (pTypeExt->CameoPCX.Exists())
				pPCXSurface = pTypeExt->CameoPCX.GetSurface();
			else if (pTypeExt->AltCameoPCX.Exists())
				pPCXSurface = pTypeExt->AltCameoPCX.GetSurface();
		}

		// 2. Check CameoFile PCX
		if (!pPCXSurface && item.pType->CameoFile[0] != '\0')
		{
			PhobosPCXFile pcxFile(item.pType->CameoFile);
			if (pcxFile.Exists())
			{
				pPCXSurface = pcxFile.GetSurface();
			}
			else
			{
				char pcxName[64];
				sprintf_s(pcxName, "%s.pcx", item.pType->CameoFile);
				_strlwr_s(pcxName);
				PhobosPCXFile pcxFile2(pcxName);
				if (pcxFile2.Exists())
					pPCXSurface = pcxFile2.GetSurface();
			}
		}

		// 3. Check ID PCX variants (e.g. TSGACNSTicon.pcx, TSGACNST.pcx, GDIPOWRicon.pcx)
		if (!pPCXSurface && item.pType->ID)
		{
			const char* idStr = item.pType->ID;
			char pcxBuf[64];
			sprintf_s(pcxBuf, "%sicon.pcx", idStr);
			_strlwr_s(pcxBuf);
			PhobosPCXFile pcx1(pcxBuf);
			if (pcx1.Exists())
			{
				pPCXSurface = pcx1.GetSurface();
			}
			else
			{
				sprintf_s(pcxBuf, "%s.pcx", idStr);
				_strlwr_s(pcxBuf);
				PhobosPCXFile pcx2(pcxBuf);
				if (pcx2.Exists())
					pPCXSurface = pcx2.GetSurface();
			}
		}

		// 4. Fallback to SHP cameos, ONLY if SHP is not XXICON.SHP placeholder
		if (!pPCXSurface)
		{
			SHPStruct* pCandidateSHP = item.pType->GetCameo();
			if (!pCandidateSHP) pCandidateSHP = item.pType->Cameo;
			if (!pCandidateSHP) pCandidateSHP = item.pType->AltCameo;

			if (pCandidateSHP)
			{
				bool isXXIcon = false;
				SHPReference* pRef = pCandidateSHP->AsReference();
				if (pRef && pRef->Filename)
				{
					const char* fn = pRef->Filename;
					if (_stricmp(fn, "XXICON.SHP") == 0 || _stricmp(fn, "xxicon.shp") == 0 || _stricmp(fn, "XXICON") == 0)
					{
						isXXIcon = true;
					}
				}

				if (!isXXIcon)
				{
					pFileSHP = pCandidateSHP;
				}
			}

			if (!pFileSHP && item.pType->CameoFile[0] != '\0')
			{
				pFileSHP = FileSystem::LoadSHPFile(item.pType->CameoFile);
				if (!pFileSHP)
				{
					char shpBuf[64];
					sprintf_s(shpBuf, "%s.shp", item.pType->CameoFile);
					_strlwr_s(shpBuf);
					pFileSHP = FileSystem::LoadSHPFile(shpBuf);
				}
			}
		}
	}

	if (pPCXSurface && (pPCXSurface->GetWidth() <= 0 || pPCXSurface->GetHeight() <= 0))
	{
		pPCXSurface = nullptr;
	}

	if (!pFileSHP && !pPCXSurface)
		pFileSHP = FileSystem::LoadSHPFile("XXICON.SHP");
	if (!pFileSHP && !pPCXSurface)
		pFileSHP = FileSystem::LoadSHPFile("xxicon.shp");
	if (!pFileSHP && !pPCXSurface)
		pFileSHP = FileSystem::LoadSHPFile("XXICON");
	if (!pFileSHP && !pPCXSurface)
		pFileSHP = FileSystem::LoadSHPFile("xxicon");

	// Draw image using DrawImage helper (exact DropshipLoadout approach)
	bool painted = DrawImage(
		pSurface,
		drawRect,
		pPCXSurface,
		pFileSHP,
		FileSystem::CAMEO_PAL,
		0,
		-2,
		BlitterFlags::None
	);

	if (!painted)
	{
		// Draw missing cameo outline and placeholder text if no image exists
		ColorStruct blackColor { 0, 0, 0 };
		pSurface->FillRectTrans(&drawRect, &blackColor, 75);
		pSurface->DrawRect(&drawRect, Drawing::RGB_To_Int(100, 100, 100));

		if (BitFont::Instance)
		{
			Point2D pt = { drawRect.X + 4, drawRect.Y + drawRect.Height / 2 - 4 };
			pSurface->DrawTextA(L"NO CAMEO", const_cast<RectangleStruct*>(&clipRect), &pt, Drawing::RGB_To_Int(255, 0, 0), 0, TextPrintType::Point6);
		}
	}

	// Draw hover outline matching player's color
	if (isHovered)
	{
		pSurface->DrawRect(const_cast<RectangleStruct*>(&drawRect), Drawing::RGB_To_Int(playerColor.R, playerColor.G, playerColor.B));
	}

	ColorStruct textBgColor { 0, 0, 0 };

	// Draw superweapon percentage overlay centered on cameo (0% -> 100%)
	if (item.IsSuperweapon && item.pSuper)
	{
		int totalFrames = item.pSuper->GetRechargeTime();
		int framesLeft = item.pSuper->RechargeTimer.GetTimeLeft();
		int pctReady = 100;
		if (totalFrames > 0 && framesLeft > 0)
		{
			pctReady = std::clamp(((totalFrames - framesLeft) * 100) / totalFrames, 0, 100);
		}

		std::wostringstream oss;
		oss << pctReady << L"%";
		std::wstring pctStr = oss.str();

		if (BitFont::Instance && BitText::Instance)
		{
			int textW = 0, textH = 0;
			BitFont::Instance->GetTextDimension(pctStr.c_str(), &textW, &textH, item.DisplayRect.Width);
			Point2D textPt = { item.DisplayRect.X + (item.DisplayRect.Width - textW) / 2, item.DisplayRect.Y + (item.DisplayRect.Height - textH) / 2 };

			RectangleStruct textBgRect = { textPt.X - 3, textPt.Y - 1, textW + 6, textH + 2 };
			RectangleStruct clippedBgRect;
			if (IntersectRect(textBgRect, clipRect, clippedBgRect))
			{
				pSurface->FillRectTrans(&clippedBgRect, &textBgColor, 75);
			}

			LTRBStruct oldBounds = BitFont::Instance->Bounds;
			WORD oldColor = BitFont::Instance->Color;
			bool oldField41 = BitFont::Instance->field_41;

			LTRBStruct ltrbBounds = { clipRect.X, clipRect.Y, clipRect.X + clipRect.Width, clipRect.Y + clipRect.Height };
			BitFont::Instance->field_41 = 1;
			BitFont::Instance->SetBounds(&ltrbBounds);
			BitFont::Instance->Color = static_cast<WORD>((pctReady == 100) ? Drawing::RGB_To_Int(0, 255, 0) : Drawing::RGB_To_Int(255, 255, 255));

			BitText::Instance->DrawText(
				BitFont::Instance,
				pSurface,
				pctStr.c_str(),
				textPt.X,
				textPt.Y,
				textW,
				textH,
				0, 0, 0
			);

			BitFont::Instance->Bounds = oldBounds;
			BitFont::Instance->Color = oldColor;
			BitFont::Instance->field_41 = oldField41;
		}
	}
	// Draw production percentage overlay with "%" suffix and 30% translucent dark background
	else if (item.IsProduction && item.ProgressPercent >= 0)
	{
		std::wostringstream oss;
		oss << item.ProgressPercent << L"%";
		std::wstring pctStr = oss.str();

		if (BitFont::Instance && BitText::Instance)
		{
			int textW = 0, textH = 0;
			BitFont::Instance->GetTextDimension(pctStr.c_str(), &textW, &textH, item.DisplayRect.Width);
			Point2D textPt = { item.DisplayRect.X + (item.DisplayRect.Width - textW) / 2, item.DisplayRect.Y + (item.DisplayRect.Height - textH) / 2 };

			// Translucent 30% opacity dark background behind text for readability (opacity = 75)
			RectangleStruct textBgRect = { textPt.X - 3, textPt.Y - 1, textW + 6, textH + 2 };
			RectangleStruct clippedBgRect;
			if (IntersectRect(textBgRect, clipRect, clippedBgRect))
			{
				pSurface->FillRectTrans(&clippedBgRect, &textBgColor, 75);
			}

			LTRBStruct oldBounds = BitFont::Instance->Bounds;
			WORD oldColor = BitFont::Instance->Color;
			bool oldField41 = BitFont::Instance->field_41;

			LTRBStruct ltrbBounds = { clipRect.X, clipRect.Y, clipRect.X + clipRect.Width, clipRect.Y + clipRect.Height };
			BitFont::Instance->field_41 = 1;
			BitFont::Instance->SetBounds(&ltrbBounds);
			BitFont::Instance->Color = static_cast<WORD>(Drawing::RGB_To_Int(0, 255, 0)); // Neon Green

			BitText::Instance->DrawText(
				BitFont::Instance,
				pSurface,
				pctStr.c_str(),
				textPt.X,
				textPt.Y,
				textW,
				textH,
				0, 0, 0
			);

			BitFont::Instance->Bounds = oldBounds;
			BitFont::Instance->Color = oldColor;
			BitFont::Instance->field_41 = oldField41;
		}
	}

	// Draw structure instance count overlay if count > 1 with 30% translucent dark background
	if (!item.IsProduction && item.Count > 1)
	{
		std::wostringstream oss;
		oss << item.Count;
		std::wstring countStr = oss.str();

		if (BitFont::Instance && BitText::Instance)
		{
			int textW = 0, textH = 0;
			BitFont::Instance->GetTextDimension(countStr.c_str(), &textW, &textH, item.DisplayRect.Width);
			Point2D textPt = { item.DisplayRect.X + (item.DisplayRect.Width - textW) / 2, item.DisplayRect.Y + (item.DisplayRect.Height - textH) / 2 };

			// Translucent 30% opacity dark background behind text for readability (opacity = 75)
			RectangleStruct textBgRect = { textPt.X - 3, textPt.Y - 1, textW + 6, textH + 2 };
			RectangleStruct clippedBgRect;
			if (IntersectRect(textBgRect, clipRect, clippedBgRect))
			{
				pSurface->FillRectTrans(&clippedBgRect, &textBgColor, 75);
			}

			LTRBStruct oldBounds = BitFont::Instance->Bounds;
			WORD oldColor = BitFont::Instance->Color;
			bool oldField41 = BitFont::Instance->field_41;

			LTRBStruct ltrbBounds = { clipRect.X, clipRect.Y, clipRect.X + clipRect.Width, clipRect.Y + clipRect.Height };
			BitFont::Instance->field_41 = 1;
			BitFont::Instance->SetBounds(&ltrbBounds);
			BitFont::Instance->Color = static_cast<WORD>(Drawing::RGB_To_Int(255, 255, 255)); // White

			BitText::Instance->DrawText(
				BitFont::Instance,
				pSurface,
				countStr.c_str(),
				textPt.X,
				textPt.Y,
				textW,
				textH,
				0, 0, 0
			);

			BitFont::Instance->Bounds = oldBounds;
			BitFont::Instance->Color = oldColor;
			BitFont::Instance->field_41 = oldField41;
		}
	}
}

void ObserverUIClass::DrawPlayerTooltip(DSurface* pSurface, HouseClass* pHouse, Point2D mousePos)
{
	if (!pHouse || !pHouse->Type || !BitFont::Instance || !BitText::Instance)
		return;

	int maxToolTipWidth = 240;

	struct TooltipSegment
	{
		std::wstring Text;
		int Color;
	};

	struct TooltipLine
	{
		std::vector<TooltipSegment> Segments;
		int Width;
		int Height;
	};

	std::vector<TooltipLine> lines;
	int textWidth = 0;
	int textHeight = 0;

	auto addLineSegments = [&](const std::vector<TooltipSegment>& segs) {
		if (segs.empty()) return;
		int lineW = 0;
		int maxH = 0;
		for (const auto& seg : segs)
		{
			int w = 0, h = 0;
			BitFont::Instance->GetTextDimension(seg.Text.c_str(), &w, &h, maxToolTipWidth);
			lineW += w;
			maxH = std::max(maxH, h);
		}
		lines.push_back({ segs, lineW, maxH });
		textWidth = std::max(textWidth, lineW);
		textHeight += maxH + 2;
	};

	auto addLine = [&](const std::wstring& textStr, int color) {
		addLineSegments({ { textStr, color } });
	};

	// Line 3: Economy Rate (+- $X/min)
	auto itRow = std::find_if(this->PlayerRows.begin(), this->PlayerRows.end(), [pHouse](const ObserverPlayerRow& r) {
		return r.pHouse == pHouse;
	});

	// Title Line: Player Number, Player Name & Country Name
	std::string plainNameStr = pHouse->PlainName;
	if (plainNameStr.empty())
		plainNameStr = pHouse->get_ID();
	std::wstring wPlainName(plainNameStr.begin(), plainNameStr.end());

	std::wstring controlStr = L"";
	if (!pHouse->IsControlledByHuman())
	{
		switch (pHouse->AIDifficulty)
		{
		case AIDifficulty::Easy: controlStr = L" [AI Easy]"; break;
		case AIDifficulty::Normal: controlStr = L" [AI Normal]"; break;
		case AIDifficulty::Hard: controlStr = L" [AI Hard]"; break;
		default: controlStr = L" [AI]"; break;
		}
	}

	std::wostringstream nameOss;
	if (itRow != this->PlayerRows.end() && itRow->PlayerNumber > 0)
	{
		nameOss << L"[P" << itRow->PlayerNumber << L"] " << wPlainName << L" (" << pHouse->Type->UIName << L")" << controlStr;
	}
	else
	{
		nameOss << wPlainName << L" (" << pHouse->Type->UIName << L")" << controlStr;
	}
	addLine(nameOss.str(), Drawing::RGB_To_Int(255, 255, 255));

	// Money Line
	std::wostringstream moneyOss;
	moneyOss << L"Credits: $" << pHouse->Available_Money();
	addLine(moneyOss.str(), Drawing::RGB_To_Int(200, 200, 200));

	if (itRow != this->PlayerRows.end())
	{
		std::wostringstream rateValOss;
		int valColor = Drawing::RGB_To_Int(180, 180, 180);

		if (itRow->IncomeRatePerMin > 0)
		{
			rateValOss << L"+$" << itRow->IncomeRatePerMin;
			valColor = Drawing::RGB_To_Int(0, 255, 0); // Bright Neon Green
		}
		else if (itRow->IncomeRatePerMin < 0)
		{
			rateValOss << L"-$" << std::abs(itRow->IncomeRatePerMin);
			valColor = Drawing::RGB_To_Int(255, 90, 90); // Soft Red
		}
		else
		{
			rateValOss << L"+$0";
			valColor = Drawing::RGB_To_Int(180, 180, 180);
		}

		addLineSegments({
			{ L"Economy/min: ", Drawing::RGB_To_Int(200, 200, 200) },
			{ rateValOss.str(), valColor }
		});
	}

	// Power Line
	int powerOutput = pHouse->PowerOutput;
	int powerDrain = pHouse->PowerDrain;
	int balance = powerOutput - powerDrain;

	std::wostringstream powerMainOss;
	powerMainOss << powerDrain << L" / " << powerOutput << L" (";

	std::wostringstream balanceOss;
	int balanceColor = Drawing::RGB_To_Int(180, 180, 180);
	if (balance > 0)
	{
		balanceOss << L"+" << balance;
		balanceColor = Drawing::RGB_To_Int(0, 255, 0);
	}
	else if (balance < 0)
	{
		balanceOss << balance;
		balanceColor = Drawing::RGB_To_Int(255, 50, 50);
	}
	else
	{
		balanceOss << L"+0";
		balanceColor = Drawing::RGB_To_Int(180, 180, 180);
	}

	addLineSegments({
		{ L"Power: ", Drawing::RGB_To_Int(200, 200, 200) },
		{ powerMainOss.str(), Drawing::RGB_To_Int(200, 200, 200) },
		{ balanceOss.str(), balanceColor },
		{ L")", Drawing::RGB_To_Int(200, 200, 200) }
	});

	// Debug-only AI / Tech lines (ONLY if DebugKeysEnabled=yes in rulesmd.ini)
	bool isDebugKeysEnabled = Phobos::Config::DevelopmentCommands;
	if (isDebugKeysEnabled)
	{
		// 1. IQLevel (only if AI)
		if (!pHouse->IsControlledByHuman())
		{
			addLine(L"AI's IQ Level: " + std::to_wstring(pHouse->IQLevel2), Drawing::RGB_To_Int(200, 200, 200));
		}

		// 2. TechLevel (always when debug)
		addLine(L"Tech Level: " + std::to_wstring(pHouse->TechLevel), Drawing::RGB_To_Int(200, 200, 200));

		// 3. Production (only if AI and false)
		if (!pHouse->IsControlledByHuman() && !pHouse->Production)
		{
			addLineSegments({
				{ L"AI Production: ", Drawing::RGB_To_Int(200, 200, 200) },
				{ L"Disabled", Drawing::RGB_To_Int(255, 90, 90) }
			});
		}

		// 4. AITriggersActive (only if AI and false)
		if (!pHouse->IsControlledByHuman() && !pHouse->AITriggersActive)
		{
			addLineSegments({
				{ L"AI Triggers: ", Drawing::RGB_To_Int(200, 200, 200) },
				{ L"Disabled", Drawing::RGB_To_Int(255, 90, 90) }
			});
		}

		// 5. AutoBaseBuilding (only if AI and false)
		if (!pHouse->IsControlledByHuman() && !pHouse->AutoBaseBuilding)
		{
			addLineSegments({
				{ L"Auto Base Building: ", Drawing::RGB_To_Int(200, 200, 200) },
				{ L"Disabled", Drawing::RGB_To_Int(255, 90, 90) }
			});
		}

		// 5b. Active AI Teams (only if AI and > 0)
		if (!pHouse->IsControlledByHuman())
		{
			int activeAITeams = 0;
			for (int k = 0; k < TeamClass::Array.Count; ++k)
			{
				auto pTeam = TeamClass::Array.GetItem(k);
				if (pTeam && pTeam->Owner == pHouse)
				{
					activeAITeams++;
				}
			}
			if (activeAITeams > 0)
			{
				addLine(L"Active AI Teams: " + std::to_wstring(activeAITeams), Drawing::RGB_To_Int(200, 200, 200));
			}
		}
	}

	// 7. Refineries (formerly Resource Destinations)
	addLine(L"Refineries: " + std::to_wstring(pHouse->CountResourceDestinations), Drawing::RGB_To_Int(200, 200, 200));

	// 8. War Factories
	addLine(L"War Factories: " + std::to_wstring(pHouse->CountWarfactories), Drawing::RGB_To_Int(200, 200, 200));

	// 9. Barracks & Helipads & Airport Docks
	int barracksCount = 0;
	int helipadsCount = 0;
	for (auto const pBld : pHouse->Buildings)
	{
		if (pBld && pBld->IsAlive && !pBld->InLimbo && pBld->Type)
		{
			if (pBld->Type->Factory == AbstractType::InfantryType || pBld->Type->GDIBarracks || pBld->Type->NODBarracks || pBld->Type->YuriBarracks)
			{
				barracksCount++;
			}
			if (pBld->Type->Helipad || pBld->Type->Factory == AbstractType::AircraftType || pBld->Type->UnitReload)
			{
				helipadsCount++;
			}
		}
	}
	if (barracksCount > 0)
	{
		addLine(L"Barracks: " + std::to_wstring(barracksCount), Drawing::RGB_To_Int(200, 200, 200));
	}
	if (helipadsCount > 0 || pHouse->AirportDocks > 0)
	{
		std::wstring helipadsLine = L"Helipads: " + std::to_wstring(helipadsCount);
		if (pHouse->AirportDocks > 0)
		{
			helipadsLine += L" (Docks: " + std::to_wstring(pHouse->AirportDocks) + L")";
		}
		addLine(helipadsLine, Drawing::RGB_To_Int(200, 200, 200));
	}

	// 10. Total Objects Counts (only if > 0)
	if (pHouse->OwnedBuildings > 0)
	{
		addLine(L"Total Buildings: " + std::to_wstring(pHouse->OwnedBuildings), Drawing::RGB_To_Int(200, 200, 200));
	}
	if (pHouse->OwnedInfantry > 0)
	{
		addLine(L"Total Infantry: " + std::to_wstring(pHouse->OwnedInfantry), Drawing::RGB_To_Int(200, 200, 200));
	}
	if (pHouse->OwnedUnits > 0)
	{
		addLine(L"Total Units: " + std::to_wstring(pHouse->OwnedUnits), Drawing::RGB_To_Int(200, 200, 200));
	}
	if (pHouse->OwnedAircraft > 0)
	{
		addLine(L"Total Aircraft: " + std::to_wstring(pHouse->OwnedAircraft), Drawing::RGB_To_Int(200, 200, 200));
	}
	if (pHouse->OwnedNavy > 0)
	{
		addLine(L"Total Navy: " + std::to_wstring(pHouse->OwnedNavy), Drawing::RGB_To_Int(200, 200, 200));
	}

	// 11. Killed Objects Counts (only if > 0)
	int killedUnits = pHouse->KilledUnitTypes.GetUnitCount() + pHouse->KilledInfantryTypes.GetUnitCount() + pHouse->KilledAircraftTypes.GetUnitCount();
	if (killedUnits > 0)
	{
		addLine(L"Killed Units: " + std::to_wstring(killedUnits), Drawing::RGB_To_Int(200, 200, 200));
	}
	int killedBuildings = pHouse->KilledBuildingTypes.GetUnitCount();
	if (killedBuildings > 0)
	{
		addLine(L"Killed Buildings: " + std::to_wstring(killedBuildings), Drawing::RGB_To_Int(200, 200, 200));
	}

	// Target Enemy House & Allies
	std::wstring alliesStr = L"";
	for (const auto& r : this->PlayerRows)
	{
		if (r.pHouse && r.pHouse != pHouse && pHouse->IsAlliedWith(r.pHouse))
		{
			if (!alliesStr.empty()) alliesStr += L", ";
			alliesStr += L"P" + std::to_wstring(r.PlayerNumber);
		}
	}
	if (!alliesStr.empty())
	{
		addLineSegments({
			{ L"Allies: ", Drawing::RGB_To_Int(200, 200, 200) },
			{ alliesStr, Drawing::RGB_To_Int(100, 220, 255) }
		});
	}

	if (!pHouse->IsControlledByHuman())
	{
		auto pEnemyHouse = GetTargetEnemy(pHouse);
		if (pEnemyHouse && pEnemyHouse->Type)
		{
			ColorStruct enemyColor = GetHouseColor(pEnemyHouse);

			std::string enemyPlainName = pEnemyHouse->PlainName;
			if (enemyPlainName.empty())
				enemyPlainName = pEnemyHouse->get_ID();
			std::wstring wEnemyPlain(enemyPlainName.begin(), enemyPlainName.end());

			std::wostringstream enemyValOss;
			auto itEnemyRow = std::find_if(this->PlayerRows.begin(), this->PlayerRows.end(), [pEnemyHouse](const ObserverPlayerRow& r) {
				return r.pHouse == pEnemyHouse;
			});
			if (itEnemyRow != this->PlayerRows.end() && itEnemyRow->PlayerNumber > 0)
			{
				enemyValOss << L"[P" << itEnemyRow->PlayerNumber << L"] " << wEnemyPlain << L" (" << pEnemyHouse->Type->UIName << L")";
			}
			else
			{
				enemyValOss << wEnemyPlain << L" (" << pEnemyHouse->Type->UIName << L")";
			}

			addLineSegments({
				{ L"Target Enemy: ", Drawing::RGB_To_Int(200, 200, 200) },
				{ enemyValOss.str(), Drawing::RGB_To_Int(enemyColor.R, enemyColor.G, enemyColor.B) }
			});
		}
		else
		{
			addLineSegments({
				{ L"Target Enemy: ", Drawing::RGB_To_Int(200, 200, 200) },
				{ L"None", Drawing::RGB_To_Int(180, 180, 180) }
			});
		}
	}

	int boxPadding = 6;
	int boxWidth = textWidth + boxPadding * 2;
	int boxHeight = textHeight + boxPadding * 2;

	int boxX = mousePos.X + 15;
	int boxY = mousePos.Y + 15;

	int maxX = DSurface::ViewBounds.Width;
	int maxY = DSurface::ViewBounds.Height;

	if (boxX + boxWidth > maxX) boxX = mousePos.X - boxWidth - 5;
	if (boxY + boxHeight > maxY) boxY = maxY - boxHeight - 5;

	if (boxX < 0) boxX = 0;
	if (boxY < 0) boxY = 0;

	// Tooltip border outline matches player's color
	ColorStruct playerColor { 180, 180, 180 };
	auto it = std::find_if(this->PlayerRows.begin(), this->PlayerRows.end(), [pHouse](const ObserverPlayerRow& r) {
		return r.pHouse == pHouse;
	});
	if (it != this->PlayerRows.end())
	{
		playerColor = it->PlayerColor;
	}

	RectangleStruct boxRect = { boxX, boxY, boxWidth, boxHeight };
	ColorStruct bgColor { 0, 0, 0 };
	pSurface->FillRectTrans(&boxRect, &bgColor, 75);
	pSurface->DrawRect(&boxRect, Drawing::RGB_To_Int(playerColor.R, playerColor.G, playerColor.B));

	// Save BitFont state to prevent side effects on other parts of UI
	LTRBStruct oldBounds = BitFont::Instance->Bounds;
	WORD oldColor = BitFont::Instance->Color;
	bool oldField41 = BitFont::Instance->field_41;

	LTRBStruct ltrbBounds = { boxRect.X, boxRect.Y, boxRect.X + boxRect.Width, boxRect.Y + boxRect.Height };
	BitFont::Instance->field_41 = 1;
	BitFont::Instance->SetBounds(&ltrbBounds);

	int currentY = boxRect.Y + boxPadding;
	for (const auto& line : lines)
	{
		int currentX = boxRect.X + boxPadding;
		for (const auto& seg : line.Segments)
		{
			int w = 0, h = 0;
			BitFont::Instance->GetTextDimension(seg.Text.c_str(), &w, &h, maxToolTipWidth);
			BitFont::Instance->Color = static_cast<WORD>(seg.Color);
			BitText::Instance->DrawText(
				BitFont::Instance,
				pSurface,
				seg.Text.c_str(),
				currentX,
				currentY,
				w,
				h,
				0, 0, 0
			);
			currentX += w;
		}
		currentY += line.Height + 2;
	}

	BitFont::Instance->Bounds = oldBounds;
	BitFont::Instance->Color = oldColor;
	BitFont::Instance->field_41 = oldField41;
}

void ObserverUIClass::DrawTooltip(DSurface* pSurface, const ObserverCameoItem& item, Point2D mousePos)
{
	if ((!item.pType && !item.pSuperType) || !BitFont::Instance || !BitText::Instance)
		return;

	int maxToolTipWidth = 220;

	struct TooltipLine
	{
		std::wstring Text;
		int Width;
		int Height;
		COLORREF Color;
	};

	std::vector<TooltipLine> lines;
	int textWidth = 0;
	int textHeight = 0;

	auto addLine = [&](const std::wstring& textStr, COLORREF color) {
		if (textStr.empty()) return;
		int w = 0, h = 0;
		BitFont::Instance->GetTextDimension(textStr.c_str(), &w, &h, maxToolTipWidth);
		lines.push_back({ textStr, w, h, color });
		textWidth = std::max(textWidth, w);
		textHeight += h + 2;
	};

	bool isDebugKeysEnabled = Phobos::Config::DevelopmentCommands;

	if (item.IsSuperweapon && item.pSuperType)
	{
		std::string swId = item.pSuperType->get_ID();
		std::wstring swName = FormatObjectNameWithDebug(0, swId.c_str(), item.pSuperType->UIName, isDebugKeysEnabled);
		addLine(swName, Drawing::RGB_To_Int(255, 255, 255));

		int totalFrames = item.pSuper ? item.pSuper->GetRechargeTime() : item.pSuperType->RechargeTime;
		int framesLeft = item.pSuper ? item.pSuper->RechargeTimer.GetTimeLeft() : 0;

		int secsLeft = (framesLeft + 14) / 15;
		int minsLeft = secsLeft / 60;
		secsLeft %= 60;

		int secsTotal = (totalFrames + 14) / 15;
		int minsTotal = secsTotal / 60;
		secsTotal %= 60;

		wchar_t cdBuf[64];
		swprintf_s(cdBuf, L"%02d:%02d / %02d:%02d", minsLeft, secsLeft, minsTotal, secsTotal);
		std::wostringstream cdOss;
		cdOss << L"Cooldown: " << cdBuf;
		addLine(cdOss.str(), (framesLeft == 0) ? Drawing::RGB_To_Int(0, 255, 0) : Drawing::RGB_To_Int(100, 220, 255));

		if (item.pSuperType->IsPowered && item.pOwner && item.pOwner->PowerOutput < item.pOwner->PowerDrain)
		{
			addLine(L"Power: Low Power", Drawing::RGB_To_Int(255, 50, 50));
		}
	}
	else if (item.pType)
	{
		// Top line: Structure / Techno Name
		std::string tId = item.pType->get_ID();
		std::wstring tName = FormatObjectNameWithDebug(0, tId.c_str(), item.pType->UIName, isDebugKeysEnabled);
		addLine(tName, Drawing::RGB_To_Int(255, 255, 255));

		if (item.IsProduction)
		{
			// Production Cost Line
			int cost = item.pType->GetActualCost(item.pOwner ? item.pOwner : HouseClass::CurrentPlayer);
			std::wostringstream costOss;
			costOss << L"Cost: $" << cost;
			addLine(costOss.str(), Drawing::RGB_To_Int(200, 200, 200));
		}
		else if (!item.Buildings.empty())
		{
			// Only display individual stats (HP, Shield, Veterancy) when there is EXACTLY 1 building instance
			if (item.Buildings.size() == 1 && item.Buildings[0])
			{
				auto const pBld = item.Buildings[0];
				std::wostringstream hpOss;
				hpOss << L"HP: " << pBld->Health << L"/" << item.pType->Strength;

				COLORREF hpColor = Drawing::RGB_To_Int(200, 200, 200);
				if (pBld->Health < item.pType->Strength / 4)
					hpColor = Drawing::RGB_To_Int(255, 90, 90);

				addLine(hpOss.str(), hpColor);

				// Shield Status Line
				auto const pExt = TechnoExt::ExtMap.Find(pBld);
				if (pExt && pExt->Shield && pExt->Shield->IsAvailable())
				{
					std::wostringstream shieldOss;
					shieldOss << L"Shield: " << pExt->Shield->GetHP() << L"/" << pExt->Shield->GetType()->Strength.Get();
					addLine(shieldOss.str(), Drawing::RGB_To_Int(200, 200, 200));
				}

				// Experience / Veterancy Line
				if (item.pType->Trainable)
				{
					std::wostringstream expOss;
					int vetPercent = static_cast<int>((pBld->Veterancy.Veterancy / 2.0f) * 100.0f);
					expOss << L"Veterancy: " << std::clamp(vetPercent, 0, 100) << L"%";
					addLine(expOss.str(), Drawing::RGB_To_Int(200, 200, 200));
				}
			}

			// Low Power warning line (applies regardless of building count)
			if (item.pType->WhatAmI() == AbstractType::BuildingType)
			{
				auto pBldType = static_cast<BuildingTypeClass*>(item.pType);
				if (pBldType->Powered && item.pOwner && item.pOwner->HasLowPower())
				{
					addLine(L"Low Power", Drawing::RGB_To_Int(255, 90, 90));
				}
			}
		}

		// Description line (wrapped at maxToolTipWidth)
		auto const pTypeExt = item.pType ? TechnoTypeExt::ExtMap.Find(item.pType) : nullptr;
		if (Phobos::Config::ToolTipDescriptions && pTypeExt && !pTypeExt->UIDescription.Get().empty())
		{
			addLine(pTypeExt->UIDescription.Get().Text, Drawing::RGB_To_Int(180, 180, 180));
		}
	}

	int boxPadding = 6;
	int boxWidth = textWidth + boxPadding * 2;
	int boxHeight = textHeight + boxPadding * 2;

	int boxX = mousePos.X + 15;
	int boxY = mousePos.Y + 15;

	int maxX = DSurface::ViewBounds.Width;
	int maxY = DSurface::ViewBounds.Height;

	if (boxX + boxWidth > maxX) boxX = mousePos.X - boxWidth - 5;
	if (boxY + boxHeight > maxY) boxY = maxY - boxHeight - 5;

	if (boxX < 0) boxX = 0;
	if (boxY < 0) boxY = 0;

	// Tooltip border outline matches player's color
	ColorStruct playerColor { 180, 180, 180 };
	if (item.pOwner)
	{
		auto it = std::find_if(this->PlayerRows.begin(), this->PlayerRows.end(), [item](const ObserverPlayerRow& r) {
			return r.pHouse == item.pOwner;
		});
		if (it != this->PlayerRows.end())
		{
			playerColor = it->PlayerColor;
		}
	}

	RectangleStruct boxRect = { boxX, boxY, boxWidth, boxHeight };
	ColorStruct bgColor { 0, 0, 0 };
	pSurface->FillRectTrans(&boxRect, &bgColor, 75);
	pSurface->DrawRect(&boxRect, Drawing::RGB_To_Int(playerColor.R, playerColor.G, playerColor.B));

	// Save BitFont state to prevent side effects on other parts of UI
	LTRBStruct oldBounds = BitFont::Instance->Bounds;
	WORD oldColor = BitFont::Instance->Color;
	bool oldField41 = BitFont::Instance->field_41;

	LTRBStruct ltrbBounds = { boxRect.X, boxRect.Y, boxRect.X + boxRect.Width, boxRect.Y + boxRect.Height };
	BitFont::Instance->field_41 = 1;
	BitFont::Instance->SetBounds(&ltrbBounds);

	int currentY = boxRect.Y + boxPadding;
	for (const auto& line : lines)
	{
		BitFont::Instance->Color = static_cast<WORD>(line.Color);
		BitText::Instance->DrawText(
			BitFont::Instance,
			pSurface,
			line.Text.c_str(),
			boxRect.X + boxPadding,
			currentY,
			line.Width,
			line.Height,
			0, 0, 0
		);
		currentY += line.Height + 2;
	}

	BitFont::Instance->Bounds = oldBounds;
	BitFont::Instance->Color = oldColor;
	BitFont::Instance->field_41 = oldField41;
}

static CoordStruct GetPlayerStartCoords(HouseClass* pHouse)
{
	if (!pHouse)
		return CoordStruct::Empty;

	// 1. Try Construction Yard or primary base building
	for (auto const pBld : BuildingClass::Array)
	{
		if (pBld && pBld->Owner == pHouse && pBld->IsAlive && !pBld->InLimbo && pBld->Type)
		{
			if (pBld->Type->ConstructionYard)
				return pBld->GetCenterCoords();
		}
	}

	// 2. Try any alive building
	for (auto const pBld : BuildingClass::Array)
	{
		if (pBld && pBld->Owner == pHouse && pBld->IsAlive && !pBld->InLimbo)
			return pBld->GetCenterCoords();
	}

	// 3. Try BaseSpawnCell or BaseCenter
	if (pHouse->BaseSpawnCell != CellStruct::Empty && pHouse->BaseSpawnCell.X > 0)
	{
		return CellClass::Cell2Coord(pHouse->BaseSpawnCell);
	}

	if (pHouse->BaseCenter != CellStruct::Empty && pHouse->BaseCenter.X > 0)
	{
		return CellClass::Cell2Coord(pHouse->BaseCenter);
	}

	return CoordStruct::Empty;
}

bool ObserverUIClass::HandleMouseClick(Point2D mousePos, bool isRightClick)
{
	bool isActive = IsActive() || (Phobos::Config::DevelopmentCommands && (this->DisplayMode != ObserverUIDisplayMode::Hidden || this->HasFloatingWindows()));
	if (!isActive)
		return false;

	if (this->DisplayMode == ObserverUIDisplayMode::Hidden && !this->HasFloatingWindows())
		return false;

	// 1. Handle clicks on Floating Unit Status Windows (in reverse order, top-most first)
	for (int i = static_cast<int>(this->FloatingUnitWindows.size()) - 1; i >= 0; --i)
	{
		auto& win = this->FloatingUnitWindows[i];

		// Check Close Button [X]
		if (win.CloseBtnRect.Width > 0 && mousePos.X >= win.CloseBtnRect.X && mousePos.X <= (win.CloseBtnRect.X + win.CloseBtnRect.Width)
			&& mousePos.Y >= win.CloseBtnRect.Y && mousePos.Y <= (win.CloseBtnRect.Y + win.CloseBtnRect.Height))
		{
			this->FloatingUnitWindows.erase(this->FloatingUnitWindows.begin() + i);
			return true;
		}

		// Check Cameo Click Rect
		if (win.CameoClickRect.Width > 0 && mousePos.X >= win.CameoClickRect.X && mousePos.X <= (win.CameoClickRect.X + win.CameoClickRect.Width)
			&& mousePos.Y >= win.CameoClickRect.Y && mousePos.Y <= (win.CameoClickRect.Y + win.CameoClickRect.Height))
		{
			if (!isRightClick)
			{
				while (ObjectClass::CurrentObjects.Count > 0)
				{
					ObjectClass::CurrentObjects.GetItem(0)->Deselect();
				}

				if (win.pTargetTechno && IsTechnoValidAndAlive(win.pTargetTechno) && TacticalClass::Instance)
				{
					CoordStruct coords = win.pTargetTechno->GetCenterCoords();
					TacticalClass::Instance->SetTacticalPosition(&coords);
					win.pTargetTechno->Select();
					MapClass::Instance.Redraws = TRUE;
				}
				else if (win.pTargetBuilding && IsBuildingValidAndAlive(win.pTargetBuilding) && TacticalClass::Instance)
				{
					CoordStruct coords = win.pTargetBuilding->GetCenterCoords();
					TacticalClass::Instance->SetTacticalPosition(&coords);
					win.pTargetBuilding->Select();
					MapClass::Instance.Redraws = TRUE;
				}
				else if (win.pType && win.pOwner && TacticalClass::Instance)
				{
					TechnoClass* pFoundTech = nullptr;
					BuildingClass* pFoundBld = nullptr;

					for (int k = 0; k < TechnoClass::Array.Count; ++k)
					{
						auto pObj = TechnoClass::Array.GetItem(k);
						if (pObj && pObj->IsAlive && !pObj->InLimbo && pObj->Owner == win.pOwner && pObj->GetTechnoType() == win.pType)
						{
							pFoundTech = pObj;
							break;
						}
					}

					if (!pFoundTech)
					{
						for (int k = 0; k < BuildingClass::Array.Count; ++k)
						{
							auto pBldObj = BuildingClass::Array.GetItem(k);
							if (pBldObj && pBldObj->IsAlive && !pBldObj->InLimbo && pBldObj->Owner == win.pOwner && pBldObj->Type == win.pType)
							{
								pFoundBld = pBldObj;
								break;
							}
						}
					}

					if (pFoundTech)
					{
						CoordStruct coords = pFoundTech->GetCenterCoords();
						TacticalClass::Instance->SetTacticalPosition(&coords);
						pFoundTech->Select();
						MapClass::Instance.Redraws = TRUE;
					}
					else if (pFoundBld)
					{
						CoordStruct coords = pFoundBld->GetCenterCoords();
						TacticalClass::Instance->SetTacticalPosition(&coords);
						pFoundBld->Select();
						MapClass::Instance.Redraws = TRUE;
					}
				}
				return true;
			}
		}

		// Check Window Rect
		if (win.WindowRect.Width > 0 && mousePos.X >= win.WindowRect.X && mousePos.X <= (win.WindowRect.X + win.WindowRect.Width)
			&& mousePos.Y >= win.WindowRect.Y && mousePos.Y <= (win.WindowRect.Y + win.WindowRect.Height))
		{
			if (!isRightClick)
			{
				win.IsDragging = true;
				win.DragOffset = Point2D { mousePos.X - win.Position.X, mousePos.Y - win.Position.Y };

				if (i != static_cast<int>(this->FloatingUnitWindows.size()) - 1)
				{
					ObserverFloatingUnitWindow targetWin = win;
					this->FloatingUnitWindows.erase(this->FloatingUnitWindows.begin() + i);
					this->FloatingUnitWindows.push_back(targetWin);
				}
			}
			return true;
		}
	}

	// 2. Handle clicks on Floating Player Status Windows (in reverse order, top-most first)
	for (int i = static_cast<int>(this->FloatingWindows.size()) - 1; i >= 0; --i)
	{
		auto& win = this->FloatingWindows[i];

		// Check Close Button [X]
		if (win.CloseBtnRect.Width > 0 && mousePos.X >= win.CloseBtnRect.X && mousePos.X <= (win.CloseBtnRect.X + win.CloseBtnRect.Width)
			&& mousePos.Y >= win.CloseBtnRect.Y && mousePos.Y <= (win.CloseBtnRect.Y + win.CloseBtnRect.Height))
		{
			this->FloatingWindows.erase(this->FloatingWindows.begin() + i);
			return true;
		}

		// Check Window Rect
		if (win.WindowRect.Width > 0 && mousePos.X >= win.WindowRect.X && mousePos.X <= (win.WindowRect.X + win.WindowRect.Width)
			&& mousePos.Y >= win.WindowRect.Y && mousePos.Y <= (win.WindowRect.Y + win.WindowRect.Height))
		{
			if (!isRightClick)
			{
				// Left click starts dragging & brings window to front
				win.IsDragging = true;
				win.DragOffset = Point2D { mousePos.X - win.Position.X, mousePos.Y - win.Position.Y };

				if (i != static_cast<int>(this->FloatingWindows.size()) - 1)
				{
					ObserverFloatingWindow targetWin = win;
					this->FloatingWindows.erase(this->FloatingWindows.begin() + i);
					this->FloatingWindows.push_back(targetWin);
				}
			}
			return true;
		}
	}

	// 3. Handle Inspect Selected Button [-> [] <-] click
	if (!isRightClick && this->InspectBtnRect.Width > 0 && mousePos.X >= this->InspectBtnRect.X && mousePos.X <= (this->InspectBtnRect.X + this->InspectBtnRect.Width)
		&& mousePos.Y >= this->InspectBtnRect.Y && mousePos.Y <= (this->InspectBtnRect.Y + this->InspectBtnRect.Height))
	{
		this->OpenFloatingWindowForSelectedObject();
		return true;
	}

	// 3b. Handle Vertical Player Rows Scroll Buttons [ ▲ ][ ▼ ] click
	if (!isRightClick && this->MaxVerticalScrollOffset > 0)
	{
		if (this->VertScrollUpBtnRect.Width > 0 && mousePos.X >= this->VertScrollUpBtnRect.X && mousePos.X <= (this->VertScrollUpBtnRect.X + this->VertScrollUpBtnRect.Width)
			&& mousePos.Y >= this->VertScrollUpBtnRect.Y && mousePos.Y <= (this->VertScrollUpBtnRect.Y + this->VertScrollUpBtnRect.Height))
		{
			this->VerticalScrollOffset = std::max(0, this->VerticalScrollOffset - 1);
			return true;
		}

		if (this->VertScrollDownBtnRect.Width > 0 && mousePos.X >= this->VertScrollDownBtnRect.X && mousePos.X <= (this->VertScrollDownBtnRect.X + this->VertScrollDownBtnRect.Width)
			&& mousePos.Y >= this->VertScrollDownBtnRect.Y && mousePos.Y <= (this->VertScrollDownBtnRect.Y + this->VertScrollDownBtnRect.Height))
		{
			this->VerticalScrollOffset = std::min(this->MaxVerticalScrollOffset, this->VerticalScrollOffset + 1);
			return true;
		}
	}

	if (this->DisplayMode == ObserverUIDisplayMode::Minimal)
	{
		return false;
	}

	// 4. Handle Clear Button [X] click
	if (!isRightClick && this->ClearBtnRect.Width > 0 && mousePos.X >= this->ClearBtnRect.X && mousePos.X <= (this->ClearBtnRect.X + this->ClearBtnRect.Width)
		&& mousePos.Y >= this->ClearBtnRect.Y && mousePos.Y <= (this->ClearBtnRect.Y + this->ClearBtnRect.Height))
	{
		this->SearchFilterText.clear();
		this->IsSearchInputFocused = false;
		this->CollectPlayerData();
		return true;
	}

	// 5. Handle Search Input Box click
	if (!isRightClick && this->SearchBoxRect.Width > 0 && mousePos.X >= this->SearchBoxRect.X && mousePos.X <= (this->SearchBoxRect.X + this->SearchBoxRect.Width)
		&& mousePos.Y >= this->SearchBoxRect.Y && mousePos.Y <= (this->SearchBoxRect.Y + this->SearchBoxRect.Height))
	{
		this->IsSearchInputFocused = true;
		return true;
	}

	// Clicking anywhere else unfocuses search input
	if (!isRightClick)
	{
		this->IsSearchInputFocused = false;
	}

	// 6. Handle clicking on Category Filter Tab Buttons
	if (!isRightClick)
	{
		for (const auto& btn : this->TabButtons)
		{
			if (mousePos.X >= btn.Rect.X && mousePos.X <= (btn.Rect.X + btn.Rect.Width)
				&& mousePos.Y >= btn.Rect.Y && mousePos.Y <= (btn.Rect.Y + btn.Rect.Height))
			{
				this->ActiveFilterTab = btn.Category;
				this->CollectPlayerData();
				return true;
			}
		}
	}

	int playerColorBarWidth = 5;
	int teamColorBarWidth = 10;

	auto openFloatingWindowForCameo = [&](const ObserverCameoItem& item, bool isFromProductionPanel) {
		if (item.IsSuperweapon && item.pSuperType)
		{
			auto itWin = std::find_if(this->FloatingUnitWindows.begin(), this->FloatingUnitWindows.end(), [&item](const ObserverFloatingUnitWindow& w) {
				return w.IsSuperweapon && w.pSuperType == item.pSuperType && w.pOwner == item.pOwner;
			});

			if (itWin != this->FloatingUnitWindows.end())
			{
				ObserverFloatingUnitWindow targetWin = *itWin;
				this->FloatingUnitWindows.erase(itWin);
				this->FloatingUnitWindows.push_back(targetWin);
			}
			else
			{
				ObserverFloatingUnitWindow newWin;
				newWin.IsSuperweapon = true;
				newWin.pSuperType = item.pSuperType;
				newWin.pSuper = item.pSuper;
				newWin.pOwner = item.pOwner;
				if (!item.Buildings.empty()) newWin.pTargetBuilding = item.Buildings[0];

				int screenW = DSurface::Composite ? DSurface::Composite->Width : 1024;
				int cardW = 320;
				int topY = 40;
				int cascadeOffset = static_cast<int>((this->FloatingUnitWindows.size() + this->FloatingWindows.size()) * 24) % 140;

				newWin.Position = Point2D { (screenW - cardW) / 2 + cascadeOffset, topY + cascadeOffset };
				this->FloatingUnitWindows.push_back(newWin);
			}
			return true;
		}

		TechnoClass* pTargetTech = nullptr;
		BuildingClass* pTargetBld = nullptr;
		int instanceNum = 1;

		if (isFromProductionPanel)
		{
			if (!item.Buildings.empty())
			{
				pTargetBld = item.Buildings[0];
			}
		}
		else
		{
			uintptr_t typeKey = reinterpret_cast<uintptr_t>(item.pType);
			auto key = std::make_pair(item.pOwner, typeKey);

			if (!item.Buildings.empty())
			{
				size_t cycleIdx = this->CycleIndices[key];
				size_t idx = (cycleIdx > 0 ? cycleIdx - 1 : 0) % item.Buildings.size();
				pTargetBld = item.Buildings[idx];
				instanceNum = static_cast<int>(idx + 1);
			}
			else if (!item.Technos.empty())
			{
				size_t cycleIdx = this->CycleIndices[key];
				size_t idx = (cycleIdx > 0 ? cycleIdx - 1 : 0) % item.Technos.size();
				pTargetTech = item.Technos[idx];
				instanceNum = static_cast<int>(idx + 1);
			}
		}

		if (!pTargetTech && !pTargetBld && !isFromProductionPanel)
			return true;

		// Check if floating window ALREADY exists for this exact specific instance or factory!
		auto itWin = std::find_if(this->FloatingUnitWindows.begin(), this->FloatingUnitWindows.end(), [&item, pTargetTech, pTargetBld, isFromProductionPanel](const ObserverFloatingUnitWindow& w) {
			if (pTargetBld && w.pTargetBuilding == pTargetBld) return true;
			if (pTargetTech && w.pTargetTechno == pTargetTech) return true;
			if (isFromProductionPanel && w.IsProductionItem && w.pType == item.pType && w.pOwner == item.pOwner) return true;
			return false;
		});

		if (itWin != this->FloatingUnitWindows.end())
		{
			// Bring existing window for this instance/factory to front
			ObserverFloatingUnitWindow targetWin = *itWin;
			this->FloatingUnitWindows.erase(itWin);
			this->FloatingUnitWindows.push_back(targetWin);
		}
		else
		{
			// Spawn new window for this specific instance or factory!
			ObserverFloatingUnitWindow newWin;
			newWin.pType = isFromProductionPanel ? item.pType : (pTargetTech ? pTargetTech->GetTechnoType() : (pTargetBld ? pTargetBld->Type : item.pType));
			newWin.pOwner = item.pOwner;
			newWin.pTargetTechno = pTargetTech;
			newWin.pTargetBuilding = pTargetBld;
			newWin.IsProductionItem = isFromProductionPanel;
			newWin.InstanceNumber = instanceNum;
			int screenW = DSurface::Composite ? DSurface::Composite->Width : 1024;
			int cardW = 320;
			int topY = 40;
			int cascadeOffset = static_cast<int>((this->FloatingUnitWindows.size() + this->FloatingWindows.size()) * 24) % 140;

			newWin.Position = Point2D { (screenW - cardW) / 2 + cascadeOffset, topY + cascadeOffset };
			this->FloatingUnitWindows.push_back(newWin);
		}
		return true;
	};

	for (auto& row : this->PlayerRows)
	{
		// Handle clicking on Player Info Box (Section 1)
		if (mousePos.X >= (row.InfoRect.X - teamColorBarWidth) && mousePos.X <= (row.InfoRect.X + row.InfoRect.Width + playerColorBarWidth)
			&& mousePos.Y >= row.InfoRect.Y && mousePos.Y <= (row.InfoRect.Y + row.InfoRect.Height))
		{
			if (isRightClick)
			{
				// Right click on Section 1 opens/toggles a floating window for this player!
				auto itWin = std::find_if(this->FloatingWindows.begin(), this->FloatingWindows.end(), [&row](const ObserverFloatingWindow& w) {
					return w.pHouse == row.pHouse;
				});

				if (itWin != this->FloatingWindows.end())
				{
					// If already open, bring to front
					ObserverFloatingWindow targetWin = *itWin;
					this->FloatingWindows.erase(itWin);
					this->FloatingWindows.push_back(targetWin);
				}
				else
				{
					// Open new floating card window!
					ObserverFloatingWindow newWin;
					newWin.pHouse = row.pHouse;

					int screenW = DSurface::Composite ? DSurface::Composite->Width : 1024;
					int cardW = 340;
					int topY = 40;
					int cascadeOffset = static_cast<int>((this->FloatingUnitWindows.size() + this->FloatingWindows.size()) * 24) % 140;

					newWin.Position = Point2D { (screenW - cardW) / 2 + cascadeOffset, topY + cascadeOffset };
					this->FloatingWindows.push_back(newWin);
				}
				return true;
			}
			else
			{
				// Left click jumps camera to player's start point / base
				CoordStruct coords = GetPlayerStartCoords(row.pHouse);
				if (coords != CoordStruct::Empty && TacticalClass::Instance)
				{
					TacticalClass::Instance->SetTacticalPosition(&coords);
					MapClass::Instance.Redraws = TRUE;
				}
				return true;
			}
		}

		// Handle per-player row scroll button clicks (64px = 1 cameo width step)
		if (!isRightClick && row.MaxScrollOffset > 0)
		{
			int scrollStep = 64; // 1 cameo width (60 + 4 padding)
			if (mousePos.X >= row.ScrollLeftBtnRect.X && mousePos.X <= row.ScrollLeftBtnRect.X + row.ScrollLeftBtnRect.Width
				&& mousePos.Y >= row.ScrollLeftBtnRect.Y && mousePos.Y <= row.ScrollLeftBtnRect.Y + row.ScrollLeftBtnRect.Height)
			{
				row.ScrollOffset = std::max(0, row.ScrollOffset - scrollStep);
				return true;
			}

			if (mousePos.X >= row.ScrollRightBtnRect.X && mousePos.X <= row.ScrollRightBtnRect.X + row.ScrollRightBtnRect.Width
				&& mousePos.Y >= row.ScrollRightBtnRect.Y && mousePos.Y <= row.ScrollRightBtnRect.Y + row.ScrollRightBtnRect.Height)
			{
				row.ScrollOffset = std::min(row.MaxScrollOffset, row.ScrollOffset + scrollStep);
				return true;
			}
		}

		// Handle clicking on production cameos
		for (auto& item : row.ProductionItems)
		{
			if (mousePos.X >= item.DisplayRect.X && mousePos.X <= item.DisplayRect.X + item.DisplayRect.Width
				&& mousePos.Y >= item.DisplayRect.Y && mousePos.Y <= item.DisplayRect.Y + item.DisplayRect.Height
				&& mousePos.X >= row.ProdPanelRect.X && mousePos.X <= row.ProdPanelRect.X + row.ProdPanelRect.Width)
			{
				if (isRightClick)
				{
					return openFloatingWindowForCameo(item, true);
				}
				else
				{
					this->CenterOnNextBuilding(item);
					return true;
				}
			}
		}

		// Handle clicking on structure cameos
		for (auto& item : row.StructureItems)
		{
			if (mousePos.X >= item.DisplayRect.X && mousePos.X <= item.DisplayRect.X + item.DisplayRect.Width
				&& mousePos.Y >= item.DisplayRect.Y && mousePos.Y <= item.DisplayRect.Y + item.DisplayRect.Height
				&& mousePos.X >= row.StructPanelRect.X && mousePos.X <= row.StructPanelRect.X + row.StructPanelRect.Width)
			{
				if (item.IsSuperweapon)
				{
					if (isRightClick)
					{
						return openFloatingWindowForCameo(item, false);
					}
					else
					{
						openFloatingWindowForCameo(item, false);
						this->CenterOnNextBuilding(item);
						return true;
					}
				}

				if (isRightClick)
				{
					return openFloatingWindowForCameo(item, false);
				}
				else
				{
					this->CenterOnNextBuilding(item);
					return true;
				}
			}
		}
	}

	return false;
}

bool ObserverUIClass::HandleKeyPress(int keyVal)
{
	if (!this->IsSearchInputFocused)
		return false;

	// Ignore key release events (WWKey::Release = 0x800)
	if ((keyVal & 0x800) != 0)
		return true; // Swallow key release event so game doesn't process it

	int vk = keyVal & 0xFF;
	bool isShift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0 || (keyVal & 0x100) != 0;

	// Check Enter or Escape -> Unfocus search box
	if (vk == VK_RETURN || vk == VK_ESCAPE || vk == 0x0D || vk == 0x1B)
	{
		this->IsSearchInputFocused = false;
		return true;
	}

	// Backspace
	if (vk == VK_BACK || vk == 0x08)
	{
		if (!this->SearchFilterText.empty())
		{
			this->SearchFilterText.pop_back();
			this->CollectPlayerData();
		}
		return true;
	}

	// Space
	if (vk == VK_SPACE || vk == 0x20)
	{
		this->SearchFilterText += L' ';
		this->CollectPlayerData();
		return true;
	}

	// Letters A-Z / a-z
	if ((vk >= 'A' && vk <= 'Z') || (vk >= 'a' && vk <= 'z'))
	{
		wchar_t baseChar = static_cast<wchar_t>(std::tolower(vk));
		wchar_t ch = isShift ? static_cast<wchar_t>(std::toupper(vk)) : baseChar;
		this->SearchFilterText += ch;
		this->CollectPlayerData();
		return true;
	}

	// Numbers 0-9
	if (vk >= '0' && vk <= '9')
	{
		this->SearchFilterText += static_cast<wchar_t>(vk);
		this->CollectPlayerData();
		return true;
	}

	// Minus / Hyphen
	if (vk == VK_OEM_MINUS || vk == 0xBD || vk == '-')
	{
		this->SearchFilterText += isShift ? L'_' : L'-';
		this->CollectPlayerData();
		return true;
	}

	// Period / Dot
	if (vk == VK_OEM_PERIOD || vk == 0xBE || vk == '.')
	{
		this->SearchFilterText += isShift ? L'>' : L'.';
		this->CollectPlayerData();
		return true;
	}

	// Comma
	if (vk == VK_OEM_COMMA || vk == 0xBC || vk == ',')
	{
		this->SearchFilterText += isShift ? L'<' : L',';
		this->CollectPlayerData();
		return true;
	}

	// Any printable ASCII character (32..126)
	if (vk >= 32 && vk <= 126)
	{
		this->SearchFilterText += static_cast<wchar_t>(vk);
		this->CollectPlayerData();
		return true;
	}

	return true; // Swallow all other keys while search input box is focused
}

void ObserverUIClass::CenterOnNextBuilding(ObserverCameoItem& item)
{
	if (!TacticalClass::Instance)
		return;

	// Deselect current in-game selection
	while (ObjectClass::CurrentObjects.Count > 0)
	{
		ObjectClass::CurrentObjects.GetItem(0)->Deselect();
	}

	if (!item.Buildings.empty())
	{
		uintptr_t typeKey = item.IsSuperweapon ? reinterpret_cast<uintptr_t>(item.pSuperType) : reinterpret_cast<uintptr_t>(item.pType);
		auto key = std::make_pair(item.pOwner, typeKey);
		size_t currentIdx = this->CycleIndices[key] % item.Buildings.size();

		auto pTargetBld = item.Buildings[currentIdx];
		if (pTargetBld && IsBuildingValidAndAlive(pTargetBld))
		{
			CoordStruct coords = pTargetBld->GetCenterCoords();
			TacticalClass::Instance->SetTacticalPosition(&coords);
			pTargetBld->Select();
			MapClass::Instance.Redraws = TRUE;
		}

		this->CycleIndices[key] = (currentIdx + 1) % item.Buildings.size();
	}
	else if (!item.Technos.empty())
	{
		uintptr_t typeKey = reinterpret_cast<uintptr_t>(item.pType);
		auto key = std::make_pair(item.pOwner, typeKey);
		size_t currentIdx = this->CycleIndices[key] % item.Technos.size();

		auto pTargetTech = item.Technos[currentIdx];
		if (pTargetTech && IsTechnoValidAndAlive(pTargetTech))
		{
			CoordStruct coords = pTargetTech->GetCenterCoords();
			TacticalClass::Instance->SetTacticalPosition(&coords);
			pTargetTech->Select();
			MapClass::Instance.Redraws = TRUE;
		}

		this->CycleIndices[key] = (currentIdx + 1) % item.Technos.size();
	}
}

bool ObserverUIClass::HandleMouseWheel(bool isUp)
{
	bool isActive = IsActive() || (Phobos::Config::DevelopmentCommands && this->DisplayMode != ObserverUIDisplayMode::Hidden);
	if (!isActive || !WWMouseClass::Instance)
		return false;

	if (this->MaxVerticalScrollOffset > 0)
	{
		if (isUp)
		{
			this->VerticalScrollOffset = std::max(0, this->VerticalScrollOffset - 1);
		}
		else
		{
			this->VerticalScrollOffset = std::min(this->MaxVerticalScrollOffset, this->VerticalScrollOffset + 1);
		}
		return true;
	}

	if (this->DisplayMode != ObserverUIDisplayMode::Full)
		return false;

	Point2D mousePos = { WWMouseClass::Instance->GetX(), WWMouseClass::Instance->GetY() };
	int scrollStep = 64; // 1 cameo width (60 + 4 padding)

	for (auto& row : this->PlayerRows)
	{
		if (row.MaxScrollOffset <= 0)
			continue;

		// Check if mouse is hovering over this player row's structure panel or row area
		bool isHoveringRow = (mousePos.Y >= row.StructPanelRect.Y && mousePos.Y <= (row.StructPanelRect.Y + row.StructPanelRect.Height))
			&& (mousePos.X >= row.InfoRect.X && mousePos.X <= (row.StructPanelRect.X + row.StructPanelRect.Width + 30));

		if (isHoveringRow)
		{
			if (isUp)
			{
				row.ScrollOffset = std::max(0, row.ScrollOffset - scrollStep);
			}
			else
			{
				row.ScrollOffset = std::min(row.MaxScrollOffset, row.ScrollOffset + scrollStep);
			}
			return true;
		}
	}

	return false;
}

bool ObserverUIClass::IsMouseHoveringUI() const
{
	if (!WWMouseClass::Instance)
		return false;

	bool isUIOpen = IsActive() || (Phobos::Config::DevelopmentCommands && this->DisplayMode != ObserverUIDisplayMode::Hidden);

	if (!isUIOpen && !Phobos::Config::DevelopmentCommands)
		return false;

	if (this->DisplayMode == ObserverUIDisplayMode::Hidden && !HasFloatingWindows())
		return false;

	Point2D mousePos = { WWMouseClass::Instance->GetX(), WWMouseClass::Instance->GetY() };

	// 0. If any window is currently being dragged, mouse is hovering UI!
	for (const auto& win : this->FloatingUnitWindows)
	{
		if (win.IsDragging) return true;
	}
	for (const auto& win : this->FloatingWindows)
	{
		if (win.IsDragging) return true;
	}

	// 1. Check Floating Windows (Player & Unit cards)
	for (const auto& win : this->FloatingUnitWindows)
	{
		if (win.WindowRect.Width > 0 && mousePos.X >= win.WindowRect.X && mousePos.X <= (win.WindowRect.X + win.WindowRect.Width)
			&& mousePos.Y >= win.WindowRect.Y && mousePos.Y <= (win.WindowRect.Y + win.WindowRect.Height))
		{
			return true;
		}
	}

	for (const auto& win : this->FloatingWindows)
	{
		if (win.WindowRect.Width > 0 && mousePos.X >= win.WindowRect.X && mousePos.X <= (win.WindowRect.X + win.WindowRect.Width)
			&& mousePos.Y >= win.WindowRect.Y && mousePos.Y <= (win.WindowRect.Y + win.WindowRect.Height))
		{
			return true;
		}
	}

	if (!isUIOpen)
		return false;

	// 2. Check Inspect Button
	if (this->InspectBtnRect.Width > 0 && mousePos.X >= this->InspectBtnRect.X && mousePos.X <= (this->InspectBtnRect.X + this->InspectBtnRect.Width)
		&& mousePos.Y >= this->InspectBtnRect.Y && mousePos.Y <= (this->InspectBtnRect.Y + this->InspectBtnRect.Height))
	{
		return true;
	}

	if (this->DisplayMode == ObserverUIDisplayMode::Minimal)
	{
		return false;
	}

	if (this->SearchBoxRect.Width > 0 && mousePos.X >= this->SearchBoxRect.X && mousePos.X <= (this->SearchBoxRect.X + this->SearchBoxRect.Width)
		&& mousePos.Y >= this->SearchBoxRect.Y && mousePos.Y <= (this->SearchBoxRect.Y + this->SearchBoxRect.Height))
	{
		return true;
	}

	if (this->ClearBtnRect.Width > 0 && mousePos.X >= this->ClearBtnRect.X && mousePos.X <= (this->ClearBtnRect.X + this->ClearBtnRect.Width)
		&& mousePos.Y >= this->ClearBtnRect.Y && mousePos.Y <= (this->ClearBtnRect.Y + this->ClearBtnRect.Height))
	{
		return true;
	}

	if (this->VertScrollUpBtnRect.Width > 0 && mousePos.X >= this->VertScrollUpBtnRect.X && mousePos.X <= (this->VertScrollUpBtnRect.X + this->VertScrollUpBtnRect.Width)
		&& mousePos.Y >= this->VertScrollUpBtnRect.Y && mousePos.Y <= (this->VertScrollUpBtnRect.Y + this->VertScrollUpBtnRect.Height))
	{
		return true;
	}

	if (this->VertScrollDownBtnRect.Width > 0 && mousePos.X >= this->VertScrollDownBtnRect.X && mousePos.X <= (this->VertScrollDownBtnRect.X + this->VertScrollDownBtnRect.Width)
		&& mousePos.Y >= this->VertScrollDownBtnRect.Y && mousePos.Y <= (this->VertScrollDownBtnRect.Y + this->VertScrollDownBtnRect.Height))
	{
		return true;
	}

	// 3. Check Category Filter Tab Buttons
	for (const auto& btn : this->TabButtons)
	{
		if (mousePos.X >= btn.Rect.X && mousePos.X <= (btn.Rect.X + btn.Rect.Width)
			&& mousePos.Y >= btn.Rect.Y && mousePos.Y <= (btn.Rect.Y + btn.Rect.Height))
		{
			return true;
		}
	}

	// 4. Check Player Row specific UI panels
	for (auto const& row : this->PlayerRows)
	{
		// Section 1: Player Info Box (+ Team Bar on left, Player Color Bar on right)
		if (row.InfoRect.Width > 0 && mousePos.Y >= row.InfoRect.Y && mousePos.Y <= (row.InfoRect.Y + row.InfoRect.Height))
		{
			if (mousePos.X >= (row.InfoRect.X - 10) && mousePos.X <= (row.InfoRect.X + row.InfoRect.Width + 5))
			{
				return true;
			}
		}

		// Section 2: Filtered Objects Panel & Scroll Buttons
		if (row.StructPanelRect.Width > 0 && mousePos.Y >= row.StructPanelRect.Y && mousePos.Y <= (row.StructPanelRect.Y + row.StructPanelRect.Height))
		{
			if (mousePos.X >= row.StructPanelRect.X && mousePos.X <= (row.StructPanelRect.X + row.StructPanelRect.Width))
			{
				return true;
			}
			if (row.ScrollLeftBtnRect.Width > 0 && mousePos.X >= row.ScrollLeftBtnRect.X && mousePos.X <= (row.ScrollLeftBtnRect.X + row.ScrollLeftBtnRect.Width))
			{
				return true;
			}
			if (row.ScrollRightBtnRect.Width > 0 && mousePos.X >= row.ScrollRightBtnRect.X && mousePos.X <= (row.ScrollRightBtnRect.X + row.ScrollRightBtnRect.Width))
			{
				return true;
			}
		}

		// Section 3: Production Panel (+ Left Player Color Bar)
		if (row.ProdPanelRect.Width > 0 && mousePos.Y >= row.ProdPanelRect.Y && mousePos.Y <= (row.ProdPanelRect.Y + row.ProdPanelRect.Height))
		{
			if (mousePos.X >= (row.ProdPanelRect.X - 5) && mousePos.X <= (row.ProdPanelRect.X + row.ProdPanelRect.Width))
			{
				return true;
			}
		}
	}

	return false;
}

void ObserverUIClass::ClearFloatingWindows()
{
	this->FloatingWindows.clear();
	this->FloatingUnitWindows.clear();
}

void ObserverUIClass::ToggleDisplayMode()
{
	this->DisplayMode = static_cast<ObserverUIDisplayMode>((static_cast<int>(this->DisplayMode) + 1) % static_cast<int>(ObserverUIDisplayMode::Count));
}

bool ObserverUIClass::IsToggleObserverUIHotkeyBound()
{
	for (int idx = 0; idx < CommandClass::Hotkeys.IndexCount; idx++)
	{
		auto const& entry = CommandClass::Hotkeys.IndexTable[idx];
		if (entry.Data && entry.Data->GetName() && _stricmp(entry.Data->GetName(), "ToggleObserverUI") == 0)
		{
			if (entry.ID != 0 && entry.ID != 0xFFFF)
			{
				return true;
			}
		}
	}
	return false;
}

bool ObserverUIClass::IsShowObjectCardHotkeyBound()
{
	for (int idx = 0; idx < CommandClass::Hotkeys.IndexCount; idx++)
	{
		auto const& entry = CommandClass::Hotkeys.IndexTable[idx];
		if (entry.Data && entry.Data->GetName() && _stricmp(entry.Data->GetName(), "ShowObjectCard") == 0)
		{
			if (entry.ID != 0 && entry.ID != 0xFFFF)
			{
				return true;
			}
		}
	}
	return false;
}

bool ObserverUIClass::OpenFloatingWindowForSelectedObject()
{
	std::vector<ObjectClass*> targets;

	// 1. First priority: Check hovered object under mouse cursor
	for (auto const pTechno : TechnoClass::Array)
	{
		if (pTechno && pTechno->IsMouseHovering)
		{
			targets.push_back(pTechno);
			break;
		}
	}

	// 2. Second priority: Check selected objects if no object is hovered
	if (targets.empty() && ObjectClass::CurrentObjects.Count > 0)
	{
		for (int k = 0; k < ObjectClass::CurrentObjects.Count; ++k)
		{
			if (ObjectClass::CurrentObjects.GetItem(k))
			{
				targets.push_back(ObjectClass::CurrentObjects.GetItem(k));
			}
		}
	}

	if (targets.empty())
		return false;

	bool openedAny = false;
	for (auto pObj : targets)
	{
		if (!pObj) continue;

		BuildingClass* pBld = abstract_cast<BuildingClass*>(pObj);
		TechnoClass* pTech = pBld ? nullptr : abstract_cast<TechnoClass*>(pObj);

		BuildingClass* pValidBld = IsBuildingValidAndAlive(pBld) ? pBld : nullptr;
		TechnoClass* pValidTech = pValidBld ? nullptr : (IsTechnoValidAndAlive(pTech) ? pTech : nullptr);

		if (!pValidTech && !pValidBld) continue;

		// Check if floating window ALREADY exists for this exact instance
		auto itWin = std::find_if(this->FloatingUnitWindows.begin(), this->FloatingUnitWindows.end(), [pValidTech, pValidBld](const ObserverFloatingUnitWindow& w) {
			if (pValidBld && w.pTargetBuilding == pValidBld) return true;
			if (pValidTech && w.pTargetTechno == pValidTech) return true;
			return false;
		});

		if (itWin != this->FloatingUnitWindows.end())
		{
			// Bring existing window for this instance to front
			ObserverFloatingUnitWindow targetWin = *itWin;
			this->FloatingUnitWindows.erase(itWin);
			this->FloatingUnitWindows.push_back(targetWin);
		}
		else
		{
			// Spawn new window for this specific instance!
			ObserverFloatingUnitWindow newWin;
			newWin.pType = pValidTech ? pValidTech->GetTechnoType() : (pValidBld ? pValidBld->Type : nullptr);
			newWin.pOwner = pValidTech ? pValidTech->Owner : (pValidBld ? pValidBld->Owner : nullptr);
			newWin.pTargetTechno = pValidTech;
			newWin.pTargetBuilding = pValidBld;
			newWin.IsProductionItem = false;
			newWin.InstanceNumber = 1;

			int screenW = DSurface::Composite ? DSurface::Composite->Width : 1024;
			int cardW = 320;
			int topY = 40;
			int cascadeOffset = static_cast<int>((this->FloatingUnitWindows.size() + this->FloatingWindows.size()) * 24) % 140;

			newWin.Position = Point2D { (screenW - cardW) / 2 + cascadeOffset, topY + cascadeOffset };
			this->FloatingUnitWindows.push_back(newWin);
		}
		openedAny = true;
	}
	return openedAny;
}

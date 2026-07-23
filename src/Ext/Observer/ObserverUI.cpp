#include "ObserverUI.h"

#include <Ext/TechnoType/Body.h>
#include <Ext/Techno/Body.h>
#include <Ext/House/Body.h>
#include <New/Entity/ShieldClass.h>

#include <Utilities/Constructs.h>
#include <Fundamentals.h>
#include <ColorScheme.h>
#include <BitFont.h>
#include <BitText.h>
#include <WWMouseClass.h>
#include <Drawing.h>
#include <StringTable.h>
#include <PCX.h>

#include <algorithm>
#include <sstream>
#include <windows.h>

ObserverUIClass ObserverUIClass::Instance;

static bool IntersectRect(const RectangleStruct& r1, const RectangleStruct& r2, RectangleStruct& out)
{
	int x1 = std::max(r1.X, r2.X);
	int y1 = std::max(r1.Y, r2.Y);
	int x2 = std::min(r1.X + r1.Width, r2.X + r2.Width);
	int y2 = std::min(r1.Y + r1.Height, r2.Y + r2.Height);

	if (x2 > x1 && y2 > y1)
	{
		out = RectangleStruct { x1, y1, x2 - x1, y2 - y1 };
		return true;
	}
	return false;
}

bool ObserverUIClass::IsActive()
{
	return HouseClass::IsCurrentPlayerObserver()
		|| (HouseClass::Observer && HouseClass::CurrentPlayer && HouseClass::CurrentPlayer->IsObserver());
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

	size_t playerIdx = 0;

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

		// Convert PlainName (char[]) to PlayerName (wstring)
		std::string plainNameStr = pHouse->PlainName;
		if (plainNameStr.empty())
			plainNameStr = pHouse->get_ID();

		std::wstring wPlainName(plainNameStr.begin(), plainNameStr.end());
		row.PlayerName = wPlainName;
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
		row.PlayerColor = GetHouseColor(pHouse, playerIdx);
		playerIdx++;

		// Collect active factory production for this player grouped by TechnoType
		std::map<TechnoTypeClass*, std::vector<BuildingClass*>> prodGroupMap;
		std::map<TechnoTypeClass*, int> prodProgressMap;

		for (auto const pBld : pHouse->Buildings)
		{
			if (!pBld || !pBld->Factory || !pBld->Factory->Object)
				continue;

			auto const pProducingType = pBld->Factory->Object->GetTechnoType();
			if (!pProducingType)
				continue;

			int progressPercent = (pBld->Factory->GetProgress() * 100) / 54;
			progressPercent = std::clamp(progressPercent, 0, 100);

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

		// Group owned structures by BuildingTypeClass
		std::map<BuildingTypeClass*, std::vector<BuildingClass*>> bldGroupMap;
		for (auto const pBld : pHouse->Buildings)
		{
			if (!pBld || !pBld->Type || !pBld->IsAlive || pBld->InLimbo)
				continue;

			bldGroupMap[pBld->Type].push_back(pBld);
		}

		for (auto const& [pType, bldList] : bldGroupMap)
		{
			ObserverCameoItem structItem;
			structItem.pType = pType;
			structItem.Count = static_cast<int>(bldList.size());
			structItem.IsProduction = false;
			structItem.pOwner = pHouse;
			structItem.Buildings = bldList;

			row.StructureItems.emplace_back(structItem);
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
	if (!IsActive())
		return;

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

	this->CollectPlayerData();
}

void ObserverUIClass::Render(DSurface* pSurface)
{
	if (!IsActive() || !pSurface)
		return;

	this->Update();

	if (this->PlayerRows.empty())
		return;

	int screenWidth = DSurface::ViewBounds.Width;
	int screenHeight = DSurface::ViewBounds.Height;

	int cameoWidth = 60;
	int cameoHeight = 48;
	int padding = 4;
	int sectionGap = 3;
	int rowHeight = cameoHeight + padding * 2;
	int startY = screenHeight - (static_cast<int>(this->PlayerRows.size()) * rowHeight) - 10;
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
	int prodSectionWidth = (maxProdCount > 0) ? (static_cast<int>(maxProdCount) * (cameoWidth + padding) + padding) : 0;
	int prodStartX = startX + infoBoxWidth + playerColorBarWidth + sectionGap;
	int structStartX = prodStartX + prodSectionWidth + (maxProdCount > 0 ? sectionGap : 0);

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

	int currentY = startY;

	for (auto& row : this->PlayerRows)
	{
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

		// Section 2: Production Items Section (Translucent 30% opacity panel background)
		if (prodSectionWidth > 0)
		{
			row.ProdPanelRect = RectangleStruct { prodStartX, currentY, prodSectionWidth, rowHeight };
			pSurface->FillRectTrans(&row.ProdPanelRect, &bgPanelColor, 75);
			pSurface->DrawRect(&row.ProdPanelRect, Drawing::RGB_To_Int(60, 60, 60));

			int currentProdX = prodStartX + padding;
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

				currentProdX += cameoWidth + padding;
			}
		}

		// Section 3: Owned Structures Section (Translucent 30% opacity panel background)
		int availableStructWidth = screenWidth - structStartX - 50;
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

	// Render tooltip for cameo or player name
	if (this->HasHoveredPlayer && this->pHoveredPlayer)
	{
		this->DrawPlayerTooltip(pSurface, this->pHoveredPlayer, this->HoveredMousePos);
	}
	else if (this->HasHoveredItem)
	{
		this->DrawTooltip(pSurface, this->HoveredItem, this->HoveredMousePos);
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
		if (!pPalette)
			return false;

		Point2D noLocation = { 0, 0 };

		::CC_Draw_Shape(
			pSurface,
			pPalette,
			fileSHP,
			frameIndex,
			&noLocation,
			&destinationRect,
			BlitterFlags::None,
			0, zAdjust, ZGradient::Ground, 1000, 0, nullptr, 0, 0, 0
		);
		painted = true;
	}

	return painted;
}

void ObserverUIClass::DrawCameoItem(DSurface* pSurface, const ObserverCameoItem& item, bool isHovered, const RectangleStruct& clipRect, ColorStruct playerColor)
{
	if (!item.pType || !pSurface)
		return;

	// Calculate intersection clip rect between item display area and section panel clip rect
	RectangleStruct drawRect;
	if (!IntersectRect(item.DisplayRect, clipRect, drawRect))
		return;

	// 1. Try PCX cameo from TechnoTypeExt (CameoPCX or AltCameoPCX) or CameoFile or ID
	BSurface* pPCXSurface = nullptr;
	auto pTypeExt = TechnoTypeExt::ExtMap.Find(item.pType);
	if (pTypeExt)
	{
		if (pTypeExt->CameoPCX.Exists())
			pPCXSurface = pTypeExt->CameoPCX.GetSurface();
		else if (pTypeExt->AltCameoPCX.Exists())
			pPCXSurface = pTypeExt->AltCameoPCX.GetSurface();
	}

	if (!pPCXSurface && item.pType->CameoFile[0] != '\0')
	{
		PhobosPCXFile pcxFile(item.pType->CameoFile);
		if (pcxFile.Exists())
			pPCXSurface = pcxFile.GetSurface();
	}

	if (!pPCXSurface && item.pType->ID)
	{
		char pcxName[64];
		sprintf_s(pcxName, "%sicon.pcx", item.pType->ID);
		_strlwr_s(pcxName);
		PhobosPCXFile pcxFile(pcxName);
		if (pcxFile.Exists())
			pPCXSurface = pcxFile.GetSurface();
	}

	if (pPCXSurface && (pPCXSurface->GetWidth() <= 0 || pPCXSurface->GetHeight() <= 0))
	{
		pPCXSurface = nullptr;
	}

	// 2. Try SHP cameo from TechnoTypeClass or XXICON.SHP fallback
	SHPStruct* pFileSHP = item.pType->GetCameo();
	if (!pFileSHP)
		pFileSHP = item.pType->Cameo;
	if (!pFileSHP)
		pFileSHP = item.pType->AltCameo;
	if (!pFileSHP)
		pFileSHP = FileSystem::LoadSHPFile("XXICON.SHP");
	if (!pFileSHP)
		pFileSHP = FileSystem::LoadSHPFile("xxicon.shp");
	if (!pFileSHP)
		pFileSHP = FileSystem::LoadSHPFile("XXICON");
	if (!pFileSHP)
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

	// Draw production percentage overlay with " %" suffix and 30% translucent dark background
	if (item.IsProduction && item.ProgressPercent >= 0)
	{
		std::wostringstream oss;
		oss << item.ProgressPercent << L" %";
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

	// Title Line: Player Name & Country Name
	std::string plainNameStr = pHouse->PlainName;
	if (plainNameStr.empty())
		plainNameStr = pHouse->get_ID();
	std::wstring wPlainName(plainNameStr.begin(), plainNameStr.end());

	std::wostringstream nameOss;
	nameOss << wPlainName << L" (" << pHouse->Type->UIName << L") [" << (pHouse->IsControlledByHuman() ? L"Human" : L"AI") << L"]";
	addLine(nameOss.str(), Drawing::RGB_To_Int(255, 255, 255));

	// Line 2: Money / Credits
	std::wostringstream moneyOss;
	moneyOss << L"Credits: $" << pHouse->Available_Money();
	addLine(moneyOss.str(), Drawing::RGB_To_Int(200, 200, 200));

	// Line 3: Economy Rate (+- $X/min)
	auto itRow = std::find_if(this->PlayerRows.begin(), this->PlayerRows.end(), [pHouse](const ObserverPlayerRow& r) {
		return r.pHouse == pHouse;
	});

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

	// Line 4: Power Line: Drain / Output (+-Balance)
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
		balanceColor = Drawing::RGB_To_Int(0, 255, 0); // Green surplus
	}
	else if (balance < 0)
	{
		balanceOss << balance; // e.g. -200
		balanceColor = Drawing::RGB_To_Int(255, 50, 50); // Vivid Red for negative deficit
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

	// Line 5: Target Enemy House (ONLY for AI players)
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
			enemyValOss << wEnemyPlain << L" (" << pEnemyHouse->Type->UIName << L")";

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
	if (!item.pType || !BitFont::Instance || !BitText::Instance)
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

	// Top line: Structure / Techno Name
	addLine(item.pType->UIName, Drawing::RGB_To_Int(255, 255, 255));

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
	auto const pTypeExt = TechnoTypeExt::ExtMap.Find(item.pType);
	if (Phobos::Config::ToolTipDescriptions && pTypeExt && !pTypeExt->UIDescription.Get().empty())
	{
		addLine(pTypeExt->UIDescription.Get().Text, Drawing::RGB_To_Int(180, 180, 180));
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
	for (auto const pBld : pHouse->Buildings)
	{
		if (pBld && pBld->IsAlive && !pBld->InLimbo && pBld->Type)
		{
			if (pBld->Type->ConstructionYard)
				return pBld->GetCenterCoords();
		}
	}

	// 2. Try any alive building
	for (auto const pBld : pHouse->Buildings)
	{
		if (pBld && pBld->IsAlive && !pBld->InLimbo)
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
	if (!IsActive())
		return false;

	if (isRightClick)
		return false;

	int playerColorBarWidth = 5;
	int teamColorBarWidth = 10;

	for (auto& row : this->PlayerRows)
	{
		// Handle clicking on Player Info Box (Section 1) to jump to player's start point / base
		if (mousePos.X >= (row.InfoRect.X - teamColorBarWidth) && mousePos.X <= (row.InfoRect.X + row.InfoRect.Width + playerColorBarWidth)
			&& mousePos.Y >= row.InfoRect.Y && mousePos.Y <= (row.InfoRect.Y + row.InfoRect.Height))
		{
			CoordStruct coords = GetPlayerStartCoords(row.pHouse);
			if (coords != CoordStruct::Empty && TacticalClass::Instance)
			{
				TacticalClass::Instance->SetTacticalPosition(&coords);
				MapClass::Instance.Redraws = TRUE;
			}
			return true;
		}

		// Handle per-player row scroll button clicks (64px = 1 cameo width step)
		if (row.MaxScrollOffset > 0)
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

		// Handle clicking on production cameos to center view and rotate focus on producing factories
		for (auto& item : row.ProductionItems)
		{
			if (mousePos.X >= item.DisplayRect.X && mousePos.X <= item.DisplayRect.X + item.DisplayRect.Width
				&& mousePos.Y >= item.DisplayRect.Y && mousePos.Y <= item.DisplayRect.Y + item.DisplayRect.Height
				&& mousePos.X >= row.ProdPanelRect.X && mousePos.X <= row.ProdPanelRect.X + row.ProdPanelRect.Width)
			{
				this->CenterOnNextBuilding(item);
				return true;
			}
		}

		// Handle clicking on structure cameos to center view and rotate focus
		for (auto& item : row.StructureItems)
		{
			if (mousePos.X >= item.DisplayRect.X && mousePos.X <= item.DisplayRect.X + item.DisplayRect.Width
				&& mousePos.Y >= item.DisplayRect.Y && mousePos.Y <= item.DisplayRect.Y + item.DisplayRect.Height
				&& mousePos.X >= row.StructPanelRect.X && mousePos.X <= row.StructPanelRect.X + row.StructPanelRect.Width)
			{
				this->CenterOnNextBuilding(item);
				return true;
			}
		}
	}

	return false;
}

void ObserverUIClass::CenterOnNextBuilding(ObserverCameoItem& item)
{
	if (item.Buildings.empty() || !TacticalClass::Instance)
		return;

	uintptr_t typeKey = reinterpret_cast<uintptr_t>(item.pType);
	auto key = std::make_pair(item.pOwner, typeKey);
	size_t currentIdx = this->CycleIndices[key] % item.Buildings.size();

	auto pTargetBld = item.Buildings[currentIdx];
	if (pTargetBld && pTargetBld->IsAlive && !pTargetBld->InLimbo)
	{
		CoordStruct coords = pTargetBld->GetCenterCoords();
		// Directly move camera position without locking camera controls
		TacticalClass::Instance->SetTacticalPosition(&coords);
		MapClass::Instance.Redraws = TRUE;
	}

	this->CycleIndices[key] = (currentIdx + 1) % item.Buildings.size();
}

bool ObserverUIClass::HandleMouseWheel(bool isUp)
{
	if (!IsActive() || !WWMouseClass::Instance)
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
	if (!IsActive() || !WWMouseClass::Instance)
		return false;

	Point2D mousePos = { WWMouseClass::Instance->GetX(), WWMouseClass::Instance->GetY() };
	for (auto const& row : this->PlayerRows)
	{
		if (mousePos.Y >= row.InfoRect.Y && mousePos.Y <= (row.InfoRect.Y + row.InfoRect.Height))
		{
			return true;
		}
	}

	return false;
}

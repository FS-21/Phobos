#pragma once

#include <HouseClass.h>
#include <BuildingClass.h>
#include <BuildingTypeClass.h>
#include <TechnoTypeClass.h>
#include <FactoryClass.h>
#include <TacticalClass.h>
#include <Surface.h>
#include <GeneralStructures.h>

#include <vector>
#include <map>
#include <deque>
#include <string>

struct PlayerEconomySample
{
	int Frame { 0 };
	int Money { 0 };
};

// Structure representing an individual cameo item (structure or production item)
struct ObserverCameoItem
{
	TechnoTypeClass* pType { nullptr };
	int Count { 0 };               // >1 for structures (if 1, not drawn)
	int ProgressPercent { -1 };    // 0..100 for items currently in production
	bool IsProduction { false };
	HouseClass* pOwner { nullptr };
	std::vector<BuildingClass*> Buildings {}; // List of building instances for camera cycling
	RectangleStruct DisplayRect { 0, 0, 0, 0 }; // Screen area occupied by this cameo
};

// Structure representing an active player row in the Observer UI bar
struct ObserverPlayerRow
{
	HouseClass* pHouse { nullptr };
	ColorStruct PlayerColor { 0, 0, 0 };
	std::wstring PlayerName {};
	std::wstring CountryName {};
	HouseClass* TargetEnemy { nullptr };
	std::wstring TargetEnemyName {};
	int IncomeRatePerMin { 0 }; // Net credits per minute (+- $X/min)
	std::vector<ObserverCameoItem> ProductionItems {};
	std::vector<ObserverCameoItem> StructureItems {};

	RectangleStruct InfoRect { 0, 0, 0, 0 };
	RectangleStruct ProdPanelRect { 0, 0, 0, 0 };
	RectangleStruct StructPanelRect { 0, 0, 0, 0 };

	// Per-player independent scroll state & button rects
	int ScrollOffset { 0 };
	int MaxScrollOffset { 0 };
	RectangleStruct ScrollLeftBtnRect { 0, 0, 0, 0 };
	RectangleStruct ScrollRightBtnRect { 0, 0, 0, 0 };
	bool IsHoveringLeftScroll { false };
	bool IsHoveringRightScroll { false };

	int TeamID { -1 };
	int TeamMemberCount { 0 };
	ColorStruct TeamColor { 0, 0, 0 };
};

class ObserverUIClass
{
public:
	static ObserverUIClass Instance;

	void Update();
	void Render(DSurface* pSurface);
	bool HandleMouseClick(Point2D mousePos, bool isRightClick);
	bool HandleMouseWheel(bool isUp);
	bool IsMouseHoveringUI() const;

	static bool IsActive();

private:
	void CollectPlayerData();
	void DrawCameoItem(DSurface* pSurface, const ObserverCameoItem& item, bool isHovered, const RectangleStruct& clipRect, ColorStruct playerColor);
	void DrawTooltip(DSurface* pSurface, const ObserverCameoItem& item, Point2D mousePos);
	void DrawPlayerTooltip(DSurface* pSurface, HouseClass* pHouse, Point2D mousePos);
	void CenterOnNextBuilding(ObserverCameoItem& item);

	std::vector<ObserverPlayerRow> PlayerRows {};
	std::map<HouseClass*, std::deque<PlayerEconomySample>> EconomyHistory {};

	// Tooltip tracking
	ObserverCameoItem HoveredItem {};
	bool HasHoveredItem { false };

	HouseClass* pHoveredPlayer { nullptr };
	bool HasHoveredPlayer { false };

	Point2D HoveredMousePos { 0, 0 };

	// Tracks camera cycling index per (House, BuildingTypeArrayIndex) pair
	std::map<std::pair<HouseClass*, int>, size_t> CycleIndices {};
};

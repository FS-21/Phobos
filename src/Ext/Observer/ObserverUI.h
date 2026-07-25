#pragma once

#include <HouseClass.h>
#include <BuildingClass.h>
#include <BuildingTypeClass.h>
#include <TechnoTypeClass.h>
#include <SuperClass.h>
#include <SuperWeaponTypeClass.h>
#include <FactoryClass.h>
#include <TacticalClass.h>
#include <ScenarioClass.h>
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

enum class ObserverUIDisplayMode : int
{
	Full = 0,      // Phase 1: Full UI (All panels + floating windows + inspect button)
	Minimal,       // Phase 2: Minimal View (Only floating windows + bottom-left inspect button)
	Hidden,        // Phase 3: Hidden (Everything hidden)
	Count
};

enum class ObserverFilterCategory : int
{
	Defenses = 0,
	Structures,
	AllStructures,
	Infantry,
	Vehicles,
	Naval,
	Aircraft,
	AllUnits,
	Superweapons,
	Everything,
	Count
};

struct ObserverTabButton
{
	ObserverFilterCategory Category;
	std::wstring Label;
	RectangleStruct Rect { 0, 0, 0, 0 };
	bool IsHovered { false };
};

// Structure representing an individual cameo item (structure, unit or production item)
struct ObserverCameoItem
{
	TechnoTypeClass* pType { nullptr };
	SuperWeaponTypeClass* pSuperType { nullptr };
	SuperClass* pSuper { nullptr };
	bool IsSuperweapon { false };
	int Count { 0 };               // Quantity count
	int ProgressPercent { -1 };    // 0..100 for items currently in production
	bool IsProduction { false };
	HouseClass* pOwner { nullptr };
	std::vector<BuildingClass*> Buildings {}; // List of building instances for camera cycling
	std::vector<TechnoClass*> Technos {};     // List of techno instances for camera cycling
	RectangleStruct DisplayRect { 0, 0, 0, 0 }; // Screen area occupied by this cameo
};

// Structure representing an active player row in the Observer UI bar
struct ObserverPlayerRow
{
	HouseClass* pHouse { nullptr };
	int PlayerNumber { 0 }; // 1 for P1, 2 for P2, 3 for P3...
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

struct ObserverFloatingWindow
{
	HouseClass* pHouse { nullptr };
	Point2D Position { 150, 150 };
	RectangleStruct WindowRect { 0, 0, 0, 0 };
	RectangleStruct CloseBtnRect { 0, 0, 0, 0 };
	bool IsDragging { false };
	Point2D DragOffset { 0, 0 };
};

struct ObserverFloatingUnitWindow
{
	TechnoTypeClass* pType { nullptr };
	SuperWeaponTypeClass* pSuperType { nullptr };
	SuperClass* pSuper { nullptr };
	bool IsSuperweapon { false };
	HouseClass* pOwner { nullptr };
	BuildingClass* pTargetBuilding { nullptr };
	TechnoClass* pTargetTechno { nullptr };
	bool IsProductionItem { false };
	int InstanceNumber { 1 };
	Point2D Position { 200, 200 };
	RectangleStruct WindowRect { 0, 0, 0, 0 };
	RectangleStruct CloseBtnRect { 0, 0, 0, 0 };
	RectangleStruct CameoClickRect { 0, 0, 0, 0 };
	bool IsDragging { false };
	Point2D DragOffset { 0, 0 };

	// Last known snapshot before object death
	bool IsDestroyed { false };
	int LastHP { 0 };
	int LastMaxHP { 0 };
	CellStruct LastCoords { CellStruct::Empty };
	std::wstring LastMission { L"Destroyed" };
	float LastVeterancy { 0.0f };
};

class ObserverUIClass
{
public:
	static ObserverUIClass Instance;

	void Update();
	void Render(DSurface* pSurface);
	bool HandleMouseClick(Point2D mousePos, bool isRightClick);
	bool HandleKeyPress(int keyVal);
	bool HandleMouseWheel(bool isUp);
	bool IsMouseHoveringUI() const;
	bool IsSearchFocused() const { return this->IsSearchInputFocused; }
	ObserverUIDisplayMode GetDisplayMode() const { return this->DisplayMode; }
	void ClearData();

	void RenderFloatingWindows(DSurface* pSurface);
	void RenderFloatingUnitWindows(DSurface* pSurface);
	bool OpenFloatingWindowForSelectedObject();
	void ClearFloatingWindows();
	void ToggleDisplayMode();
	static bool IsToggleObserverUIHotkeyBound();
	static bool IsShowObjectCardHotkeyBound();

	bool HasFloatingWindows() const { return !this->FloatingWindows.empty() || !this->FloatingUnitWindows.empty(); }

	static bool IsActive();

private:
	ObserverUIDisplayMode DisplayMode { ObserverUIDisplayMode::Hidden };
	bool WasEnterPressed { false };
	void CollectPlayerData();
	void DrawCameoItem(DSurface* pSurface, const ObserverCameoItem& item, bool isHovered, const RectangleStruct& clipRect, ColorStruct playerColor);
	void DrawTooltip(DSurface* pSurface, const ObserverCameoItem& item, Point2D mousePos);
	void DrawPlayerTooltip(DSurface* pSurface, HouseClass* pHouse, Point2D mousePos);
	void CenterOnNextBuilding(ObserverCameoItem& item);

	ObserverFilterCategory ActiveFilterTab { ObserverFilterCategory::AllStructures };
	std::vector<ObserverTabButton> TabButtons {};

	// Search filter box state
	std::wstring SearchFilterText {};
	bool IsSearchInputFocused { false };
	RectangleStruct InspectBtnRect { 0, 0, 0, 0 };
	RectangleStruct SearchBoxRect { 0, 0, 0, 0 };
	RectangleStruct ClearBtnRect { 0, 0, 0, 0 };
	bool IsHoveringInspectBtn { false };
	bool IsHoveringClearBtn { false };

	// Vertical player rows scrolling state
	int VerticalScrollOffset { 0 };
	int MaxVerticalScrollOffset { 0 };
	RectangleStruct VertScrollUpBtnRect { 0, 0, 0, 0 };
	RectangleStruct VertScrollDownBtnRect { 0, 0, 0, 0 };
	bool IsHoveringVertScrollUp { false };
	bool IsHoveringVertScrollDown { false };

	std::vector<std::wstring> ParseSearchTerms(const std::wstring& query) const;
	bool MatchesSearchFilter(AbstractTypeClass* pType) const;

	std::vector<ObserverPlayerRow> PlayerRows {};
	std::vector<ObserverFloatingWindow> FloatingWindows {};
	std::vector<ObserverFloatingUnitWindow> FloatingUnitWindows {};
	std::map<HouseClass*, std::deque<PlayerEconomySample>> EconomyHistory {};

	// Tooltip tracking
	ObserverCameoItem HoveredItem {};
	bool HasHoveredItem { false };

	HouseClass* pHoveredPlayer { nullptr };
	bool HasHoveredPlayer { false };

	Point2D HoveredMousePos { 0, 0 };

	// Tracks camera cycling index per (House, BuildingTypeArrayIndex) pair
	std::map<std::pair<HouseClass*, uintptr_t>, size_t> CycleIndices {};
};


#include <ScenarioClass.h>

#include <ThemeClass.h>
#include <WWMouseClass.h>
#include <Drawing.h>
#include <BitFont.h>
#include <BitText.h>
#include <Ext/TechnoType/Body.h>
#include <sstream>
#include <iomanip>

#include <Utilities/Macro.h>
#include <Utilities/TemplateDef.h>

#include <Ext/Scenario/Body.h>
#include <ToggleClass.h>
#include <ShapeButtonClass.h>
#include <Ext/House/Body.h>
#include <Ext/HouseType/Body.h>

#include <Utilities/GeneralUtils.h>

static bool bDropshipLoadoutActive = false;
static int pendingScrolls = 0;

bool IsDropshipLoadoutActive()
{
	return bDropshipLoadoutActive;
}

void DropshipLoadout_OnMouseWheelUp()
{
	pendingScrolls--;
}

void DropshipLoadout_OnMouseWheelDown()
{
	pendingScrolls++;
}

static ShapeButtonClass* CreateShapeButton(unsigned int nID, int nX, int nY, int nWidth, int nHeight, bool bIsAlpha)
{
	auto const pButton = GameAllocator<ShapeButtonClass>().allocate(1);
	if (!pButton)
	{
		return nullptr;
	}

	using ShapeButtonConstructor_t = ShapeButtonClass* (__thiscall *)(
		ShapeButtonClass* pThis,
		unsigned int nID,
		int nX,
		int nY,
		int nWidth,
		int nHeight,
		ConvertClass* pDrawer,
		bool bIsAlpha
	);
	auto const pConstructor = reinterpret_cast<ShapeButtonConstructor_t>(0x69DD30);
	return pConstructor(pButton, nID, nX, nY, nWidth, nHeight, nullptr, bIsAlpha);
}

class DropshipLoadoutClass
{
public:
	DropshipLoadoutClass();
	~DropshipLoadoutClass();

	bool Initialize();
	void Run();

private:
	void LoadAssets();
	void CalculateLayout(DSurface* pSurface);
	void CreateControls();
	void HandleInput(int command, int buttonID);
	void UpdateAnimations();
	void Render(DSurface* pSurface);
	void DrawTooltip(DSurface* pSurface);
	void SaveCargo();

	// Extensions
	HouseTypeExt::ExtData* pHouseTypeExt { nullptr };

	// Config & state
	int nStartingDropships { 0 };
	long initialMoney { 0 };
	long currentMoney { 0 };
	int nSidebarCameos { 8 };
	int nDropshipBayCameos { 5 };
	int nDropshipBayTotalSlots { 0 };
	int firstBrowsableCameo { 0 };
	bool pressedSpaceKey { false };
	bool repaintAll { true };
	bool lastTimeWasOverCameos { false };
	bool freeDropshipSlots { false };

	// Assets (Palette, surfaces, SHPs)
	ConvertClass* dropshipLoadout_Palette { nullptr };
	SHPStruct* dropshipLoadout_Background { nullptr };
	SHPStruct* dropshipLoadout_UpArrow { nullptr };
	SHPStruct* dropshipLoadout_DownArrow { nullptr };
	SHPStruct* dropshipLoadout_Loadout { nullptr };
	SHPStruct* dropshipLoadout_PilotLit { nullptr };
	std::vector<SHPStruct*> dropshipLoadout_DGreenList;

	BSurface* dropshipLoadout_BackgroundPCX { nullptr };
	BSurface* dropshipLoadout_UpArrowPCX { nullptr };
	BSurface* dropshipLoadout_DownArrowPCX { nullptr };
	std::vector<BSurface*> dropshipLoadout_LoadoutPCX;
	std::vector<BSurface*> dropshipLoadout_PilotLitPCX;
	std::vector<std::vector<BSurface*>> dropshipLoadout_DGreenListPCX;

	// Unit lists
	std::vector<TechnoTypeClass*> availableUnits;
	std::vector<int> availableUnitsMaximums;
	std::vector<std::vector<TechnoTypeClass*>> dropshipBayChosenUnitsLists;
	std::map<TechnoTypeClass*, int> dropshipBayChosenUnitsCount;
	TechnoTypeClass* lastSelected { nullptr };
	TechnoTypeClass* pHoveredUnitType { nullptr };

	// Layout/Locations
	RectangleStruct windowRectangle;
	int upArrowX { 0 }, upArrowY { 0 };
	int downArrowX { 0 }, downArrowY { 0 };
	RectangleStruct upArrowLocation;
	RectangleStruct downArrowLocation;
	std::vector<RectangleStruct> sidebarCameLocations;
	std::vector<std::vector<RectangleStruct>> dropshipBayCameLocations;
	RectangleStruct loadoutLocation;
	RectangleStruct pilotLitLocation;
	std::vector<RectangleStruct> dGreenLocation;

	// Interactive Buttons
	std::vector<ShapeButtonClass*> buttonsList;
	ToggleClass* commandManager { nullptr };

	// Animations & Timers
	int currentLoadoutFrame { -1 };
	int currentPilotLitFrame { -1 };
	int loadoutFrameDelay { 11 };
	int pilotLitFrameDelay { 15 };
	int loadoutTotalFrames { 0 };
	int pilotLitTotalFrames { 0 };
	int animTimer_StartValue { 15 };
	int animTimer_DelayedStartValue_Loadout { 0 };
	int animTimer_DelayedStartValue_PilotLit { 0 };

	SysTimerClass animTimer_UpdateFrameTimer;
	SysTimerClass animTimer_DelayedStartTimer_Loadout;
	SysTimerClass animTimer_UpdateFrameTimer_Loadout;
	SysTimerClass animTimer_DelayedStartTimer_PilotLit;
	SysTimerClass animTimer_UpdateFrameTimer_PilotLit;

	int sidebarRowAnimationIndex { -1 };
	int currentSidebarRowAnimationFrame { 0 };
	int sidebarRowAnimationFrameDelay { 5 };
	int sidebarRowAnimationTotalFrames { 0 };
	SysTimerClass animTimer_UpdateFrameTimer_SidebarRowAnimation;

	// Sounds
	int buyClickSoundIdx { -1 };
	int sellClickSoundIdx { -1 };
	int arrowsClickSoundIdx { -1 };

	// Drag & Drop state
	bool bIsDragging { false };
	bool bDragPending { false };
	TechnoTypeClass* pDraggedUnitType { nullptr };
	int nSourceDropshipIdx { -1 };
	int nSourceSlotIdx { -1 };
	Point2D dragStartMousePos { 0, 0 };
};

DropshipLoadoutClass::DropshipLoadoutClass()
{
}

DropshipLoadoutClass::~DropshipLoadoutClass()
{
	for (size_t i = 0; i < buttonsList.size(); ++i)
	{
		auto button = buttonsList[i];
		if (button)
		{
			GameDelete(button);
		}
	}
	buttonsList.clear();

	for (size_t i = 0; i < dropshipLoadout_DGreenList.size(); ++i)
	{
		auto dGreen = dropshipLoadout_DGreenList[i];
		if (dGreen)
		{
			bool isGlobal = false;
			if (ScenarioExt::Global() && i < ScenarioExt::Global()->DropshipLoadout_DGreenList.size())
				isGlobal = (dGreen == ScenarioExt::Global()->DropshipLoadout_DGreenList[i]);

			if (!isGlobal)
			{
				GameDelete(dGreen);
			}
		}
	}
	dropshipLoadout_DGreenList.clear();

	if (dropshipLoadout_Palette)
	{
		bool isGlobal = ScenarioExt::Global() && (dropshipLoadout_Palette == ScenarioExt::Global()->DropshipLoadout_Palette);
		if (!isGlobal)
		{
			GameDelete(dropshipLoadout_Palette);
		}
	}

	if (dropshipLoadout_Background)
	{
		bool isGlobal = ScenarioExt::Global() && (dropshipLoadout_Background == ScenarioExt::Global()->DropshipLoadout_Background);
		if (!isGlobal)
		{
			GameDelete(dropshipLoadout_Background);
		}
	}

	if (dropshipLoadout_UpArrow)
	{
		bool isGlobal = ScenarioExt::Global() && (dropshipLoadout_UpArrow == ScenarioExt::Global()->DropshipLoadout_UpArrow);
		if (!isGlobal)
		{
			GameDelete(dropshipLoadout_UpArrow);
		}
	}

	if (dropshipLoadout_DownArrow)
	{
		bool isGlobal = ScenarioExt::Global() && (dropshipLoadout_DownArrow == ScenarioExt::Global()->DropshipLoadout_DownArrow);
		if (!isGlobal)
		{
			GameDelete(dropshipLoadout_DownArrow);
		}
	}

	if (dropshipLoadout_Loadout)
	{
		bool isGlobal = ScenarioExt::Global() && (dropshipLoadout_Loadout == ScenarioExt::Global()->DropshipLoadout_Loadout);
		if (!isGlobal)
		{
			GameDelete(dropshipLoadout_Loadout);
		}
	}

	if (dropshipLoadout_PilotLit)
	{
		bool isGlobal = ScenarioExt::Global() && (dropshipLoadout_PilotLit == ScenarioExt::Global()->DropshipLoadout_PilotLit);
		if (!isGlobal)
		{
			GameDelete(dropshipLoadout_PilotLit);
		}
	}
}

bool DropshipLoadoutClass::Initialize()
{
	if (!HouseClass::CurrentPlayer)
	{
		return false;
	}

	pHouseTypeExt = HouseTypeExt::ExtMap.Find(HouseClass::CurrentPlayer->Type);
	if (!pHouseTypeExt)
	{
		return false;
	}

	if (!ScenarioClass::Instance)
	{
		return false;
	}

	nStartingDropships = pHouseTypeExt->DropshipLoadout_StartingDropships.isset() ? pHouseTypeExt->DropshipLoadout_StartingDropships : ScenarioClass::Instance->StartingDropships;
	if (nStartingDropships <= 0)
	{
		return false;
	}

	LoadAssets();
	return true;
}

void DropshipLoadoutClass::LoadAssets()
{
	auto const pGlobal = ScenarioExt::Global();

	if (pGlobal && pGlobal->DropshipLoadout_Palette)
	{
		dropshipLoadout_Palette = pGlobal->DropshipLoadout_Palette;
	}
	else
	{
		dropshipLoadout_Palette = FileSystem::LoadPALFile("DROPSHIP.PAL", DSurface::Hidden);
	}

	if (pHouseTypeExt->DropshipLoadout_BackgroundPCX.isset() && pHouseTypeExt->DropshipLoadout_BackgroundPCX.Get().Exists())
	{
		dropshipLoadout_BackgroundPCX = pHouseTypeExt->DropshipLoadout_BackgroundPCX.Get().GetSurface();
	}
	else if (pGlobal && pGlobal->DropshipLoadout_BackgroundPCX.Exists())
	{
		dropshipLoadout_BackgroundPCX = pGlobal->DropshipLoadout_BackgroundPCX.GetSurface();
	}

	if (pGlobal && pGlobal->DropshipLoadout_Background)
	{
		dropshipLoadout_Background = pGlobal->DropshipLoadout_Background;
	}
	else
	{
		char tempFilenameBuffer[32];
		_snprintf_s(tempFilenameBuffer, sizeof(tempFilenameBuffer), "DROP%04d.SHP", nStartingDropships);
		dropshipLoadout_Background = FileSystem::LoadSHPFile(_strdup(tempFilenameBuffer));
	}

	if (pHouseTypeExt->DropshipLoadout_LoadoutPCX.size() > 0)
	{
		for (auto& pFilePCX : pHouseTypeExt->DropshipLoadout_LoadoutPCX)
		{
			dropshipLoadout_LoadoutPCX.push_back(pFilePCX.GetSurface());
		}
	}
	else if (pGlobal && pGlobal->DropshipLoadout_LoadoutPCX.size() > 0)
	{
		for (auto &pFilePCX : pGlobal->DropshipLoadout_LoadoutPCX)
		{
			dropshipLoadout_LoadoutPCX.push_back(pFilePCX.GetSurface());
		}
	}

	if (pGlobal && pGlobal->DropshipLoadout_Loadout)
	{
		dropshipLoadout_Loadout = pGlobal->DropshipLoadout_Loadout;
	}
	else
	{
		dropshipLoadout_Loadout = FileSystem::LoadSHPFile("LOADOUT.SHP");
	}

	if (!pHouseTypeExt->DropshipLoadout_PilotLitPCX.empty())
	{
		for (const PhobosPCXFile& frame : pHouseTypeExt->DropshipLoadout_PilotLitPCX)
		{
			dropshipLoadout_PilotLitPCX.push_back(frame.GetSurface());
		}
	}
	else if (pGlobal && !pGlobal->DropshipLoadout_PilotLitPCX.empty())
	{
		for (auto &pFilePCX : pGlobal->DropshipLoadout_PilotLitPCX)
		{
			dropshipLoadout_PilotLitPCX.push_back(pFilePCX.GetSurface());
		}
	}

	if (pGlobal && pGlobal->DropshipLoadout_PilotLit)
	{
		dropshipLoadout_PilotLit = pGlobal->DropshipLoadout_PilotLit;
	}
	else
	{
		dropshipLoadout_PilotLit = FileSystem::LoadSHPFile("PILOTLIT.SHP");
	}

	if (pHouseTypeExt->DropshipLoadout_UpArrowPCX.isset() && pHouseTypeExt->DropshipLoadout_UpArrowPCX.Get().Exists())
	{
		dropshipLoadout_UpArrowPCX = pHouseTypeExt->DropshipLoadout_UpArrowPCX.Get().GetSurface();
	}
	else if (pGlobal && pGlobal->DropshipLoadout_UpArrowPCX.Exists())
	{
		dropshipLoadout_UpArrowPCX = pGlobal->DropshipLoadout_UpArrowPCX.GetSurface();
	}

	if (pGlobal && pGlobal->DropshipLoadout_UpArrow)
	{
		dropshipLoadout_UpArrow = pGlobal->DropshipLoadout_UpArrow;
	}
	else
	{
		dropshipLoadout_UpArrow = FileSystem::LoadSHPFile("DROPUP.SHP");
	}

	if (pHouseTypeExt->DropshipLoadout_DownArrowPCX.isset() && pHouseTypeExt->DropshipLoadout_DownArrowPCX.Get().Exists())
	{
		dropshipLoadout_DownArrowPCX = pHouseTypeExt->DropshipLoadout_DownArrowPCX.Get().GetSurface();
	}
	else if (pGlobal && pGlobal->DropshipLoadout_DownArrowPCX.Exists())
	{
		dropshipLoadout_DownArrowPCX = pGlobal->DropshipLoadout_DownArrowPCX.GetSurface();
	}

	if (pGlobal && pGlobal->DropshipLoadout_DownArrow)
	{
		dropshipLoadout_DownArrow = pGlobal->DropshipLoadout_DownArrow;
	}
	else
	{
		dropshipLoadout_DownArrow = FileSystem::LoadSHPFile("DROPDOWN.SHP");
	}

	if (pHouseTypeExt->DropshipLoadout_DGreenListPCX.size() > 0)
	{
		for (const auto& pAnimationVector : pHouseTypeExt->DropshipLoadout_DGreenListPCX)
		{
			std::vector<BSurface*> rowAnimFrames;
			if (pAnimationVector)
			{
				for (const auto& frame : *pAnimationVector)
				{
					rowAnimFrames.push_back(frame.GetSurface());
				}
			}
			dropshipLoadout_DGreenListPCX.push_back(rowAnimFrames);
		}
	}
	else if (pGlobal && pGlobal->DropshipLoadout_DGreenListPCX.size() > 0)
	{
		for (auto& pFileGroupPCX : pGlobal->DropshipLoadout_DGreenListPCX)
		{
			std::vector<BSurface*> rowAnimFrames;
			if (pFileGroupPCX)
			{
				for (auto& pFilePCX : *pFileGroupPCX)
				{
					rowAnimFrames.push_back(pFilePCX.GetSurface());
				}
			}
			dropshipLoadout_DGreenListPCX.push_back(rowAnimFrames);
		}

		for (int i = 0; i < 4 && dropshipLoadout_DGreenListPCX.size() < 4; i++)
		{
			std::vector<BSurface*> emptyAnimFrames;
			dropshipLoadout_DGreenListPCX.push_back(emptyAnimFrames);
		}
	}

	for (int i = 0; i < 4; i++)
	{
		if (pGlobal && (pGlobal->DropshipLoadout_DGreenList.size() < 4 || pGlobal->DropshipLoadout_DGreenList[i] == nullptr))
		{
			if (i == 0)
				dropshipLoadout_DGreenList.push_back(FileSystem::LoadSHPFile("DGREEN1.SHP"));
			else if (i == 1)
				dropshipLoadout_DGreenList.push_back(FileSystem::LoadSHPFile("DGREEN2.SHP"));
			else if (i == 2)
				dropshipLoadout_DGreenList.push_back(FileSystem::LoadSHPFile("DGREEN3.SHP"));
			else if (i == 3)
				dropshipLoadout_DGreenList.push_back(FileSystem::LoadSHPFile("DGREEN4.SHP"));
			else
				dropshipLoadout_DGreenList.push_back(nullptr);
		}
		else if (pGlobal)
		{
			dropshipLoadout_DGreenList.push_back(pGlobal->DropshipLoadout_DGreenList[i]);
		}
		else
		{
			dropshipLoadout_DGreenList.push_back(nullptr);
		}
	}

	if (pGlobal)
	{
		for (size_t i = 4; i < pGlobal->DropshipLoadout_DGreenList.size(); i++)
		{
			dropshipLoadout_DGreenList.push_back(pGlobal->DropshipLoadout_DGreenList[i]);
		}
	}

	buyClickSoundIdx = RulesClass::Instance->GenericClick;
	sellClickSoundIdx = RulesClass::Instance->SellSound;
	arrowsClickSoundIdx = RulesClass::Instance->GUITabSound;

	if (pHouseTypeExt->DropshipLoadout_BuyClickSound.isset())
		buyClickSoundIdx = pHouseTypeExt->DropshipLoadout_BuyClickSound;
	else if (pGlobal && pGlobal->DropshipLoadout_BuyClickSound.isset())
		buyClickSoundIdx = pGlobal->DropshipLoadout_BuyClickSound;

	if (pHouseTypeExt->DropshipLoadout_SellClickSound.isset())
		sellClickSoundIdx = pHouseTypeExt->DropshipLoadout_SellClickSound;
	else if (pGlobal && pGlobal->DropshipLoadout_SellClickSound.isset())
		sellClickSoundIdx = pGlobal->DropshipLoadout_SellClickSound;

	if (pHouseTypeExt->DropshipLoadout_ArrowsClickSound.isset())
		arrowsClickSoundIdx = pHouseTypeExt->DropshipLoadout_ArrowsClickSound;
	else if (pGlobal && pGlobal->DropshipLoadout_ArrowsClickSound.isset())
		arrowsClickSoundIdx = pGlobal->DropshipLoadout_ArrowsClickSound;


	long dropshipLoadout_InitialMoney = pHouseTypeExt->DropshipLoadout_Money.isset() ? pHouseTypeExt->DropshipLoadout_Money : (pGlobal ? pGlobal->DropshipLoadout_Money : -1);
	dropshipLoadout_InitialMoney = dropshipLoadout_InitialMoney >= 0 ? dropshipLoadout_InitialMoney : HouseClass::CurrentPlayer->Available_Money();

	initialMoney = dropshipLoadout_InitialMoney;
	currentMoney = dropshipLoadout_InitialMoney;

	std::vector<TechnoTypeClass*> allowableUnits;

	if (pHouseTypeExt->DropshipLoadout_AllowableUnits.size() > 0)
	{
		for (auto pUnit : pHouseTypeExt->DropshipLoadout_AllowableUnits)
		{
			allowableUnits.push_back(pUnit);
		}
	}
	else
	{
		for (auto pUnit : ScenarioClass::Instance->AllowableUnits)
		{
			allowableUnits.push_back(pUnit);
		}
	}

	std::vector<int> allowableUnitMaximums;

	if (pHouseTypeExt->DropshipLoadout_AllowableUnitMaximums.size() > 0)
	{
		for (int pUnitCount : pHouseTypeExt->DropshipLoadout_AllowableUnitMaximums)
		{
			allowableUnitMaximums.push_back(pUnitCount);
		}
	}
	else
	{
		for (int pUnitCount : ScenarioClass::Instance->AllowableUnitMaximums)
		{
			allowableUnitMaximums.push_back(pUnitCount);
		}
	}

	if (allowableUnits.size() > 0)
	{
		if (allowableUnitMaximums.size() > 0 && allowableUnits.size() != allowableUnitMaximums.size())
		{
		}
		else
		{
			for (size_t i = 0; i < allowableUnits.size(); ++i)
			{
				int maximumCount = -1;
				if (allowableUnitMaximums.size() > 0)
				{
					maximumCount = allowableUnitMaximums[i];
					if (maximumCount == 0)
						continue;
				}
				availableUnitsMaximums.push_back(maximumCount);
				TechnoTypeClass* pType = allowableUnits[i];
				availableUnits.push_back(pType);
			}
		}
	}
	else
	{
		for (const auto pType : TechnoTypeClass::Array)
		{
			if (pType && (pType->WhatAmI() == AbstractType::InfantryType || pType->WhatAmI() == AbstractType::UnitType))
				availableUnits.push_back(pType);
		}
	}
}

void DropshipLoadoutClass::CalculateLayout(DSurface* pSurface)
{
	if (!pSurface)
	{
		return;
	}

	const int cameoWidth = 60, cameoHeight = 48;
	int backgroundWidth = 0;
	int backgroundHeight = 0;

	if (dropshipLoadout_BackgroundPCX)
	{
		backgroundWidth = dropshipLoadout_BackgroundPCX->Width;
		backgroundHeight = dropshipLoadout_BackgroundPCX->Height;
	}
	else if (dropshipLoadout_Background)
	{
		backgroundWidth = dropshipLoadout_Background->Width;
		backgroundHeight = dropshipLoadout_Background->Height;
	}
	else
	{
		backgroundWidth = 640; // Fallback
		backgroundHeight = 480;
	}

	int backgroundX = (pSurface->GetWidth() - backgroundWidth) / 2;
	int backgroundY = (pSurface->GetHeight() - backgroundHeight) / 2;
	windowRectangle = { backgroundX, backgroundY, backgroundWidth, backgroundHeight };

	auto const pGlobal = ScenarioExt::Global();

	Point2D customUpArrowLocation = Point2D::Empty;
	if (pHouseTypeExt->DropshipLoadout_UpArrowLocation.isset())
		customUpArrowLocation = pHouseTypeExt->DropshipLoadout_UpArrowLocation;
	else if (pGlobal && pGlobal->DropshipLoadout_UpArrowLocation != Point2D::Empty)
		customUpArrowLocation = pGlobal->DropshipLoadout_UpArrowLocation;

	Point2D customDownArrowLocation = Point2D::Empty;
	if (pHouseTypeExt->DropshipLoadout_DownArrowLocation.isset())
		customDownArrowLocation = pHouseTypeExt->DropshipLoadout_DownArrowLocation;
	else if (pGlobal && pGlobal->DropshipLoadout_DownArrowLocation != Point2D::Empty)
		customDownArrowLocation = pGlobal->DropshipLoadout_DownArrowLocation;

	nSidebarCameos = 8;
	sidebarCameLocations.clear();

	if (pHouseTypeExt->DropshipLoadout_SidebarCameosCount.isset() && pHouseTypeExt->DropshipLoadout_SidebarCameosCount > 0)
	{
		nSidebarCameos = pHouseTypeExt->DropshipLoadout_SidebarCameosCount;
		for (int i = 0; i < nSidebarCameos; ++i)
		{
			int cameoX = backgroundX + pHouseTypeExt->DropshipLoadout_SidebarCameoLocations[i].X;
			int cameoY = backgroundY + pHouseTypeExt->DropshipLoadout_SidebarCameoLocations[i].Y;
			sidebarCameLocations.push_back({ cameoX, cameoY, cameoWidth, cameoHeight });
		}
	}
	else if (pGlobal && pGlobal->DropshipLoadout_SidebarCameosCount > 0)
	{
		nSidebarCameos = pGlobal->DropshipLoadout_SidebarCameosCount;
		for (int i = 0; i < nSidebarCameos; ++i)
		{
			int cameoX = backgroundX + pGlobal->DropshipLoadout_SidebarCameoLocations[i].X;
			int cameoY = backgroundY + pGlobal->DropshipLoadout_SidebarCameoLocations[i].Y;
			sidebarCameLocations.push_back({ cameoX, cameoY, cameoWidth, cameoHeight });
		}
	}
	else
	{
		for (int i = 0; i < nSidebarCameos; ++i)
		{
			int cameoX = backgroundX + 493 + 68 * (i % 2);
			int cameoY = backgroundY + 25 + 50 * (i / 2);
			sidebarCameLocations.push_back({ cameoX, cameoY, cameoWidth, cameoHeight });
		}
	}

	int centerOfCameoColumns = 0;
	int arrowsY = 0;
	if (sidebarCameLocations.size() >= 2)
	{
		centerOfCameoColumns = sidebarCameLocations[0].X + sidebarCameLocations[0].Width + (sidebarCameLocations[1].X - (sidebarCameLocations[0].X + sidebarCameLocations[0].Width)) / 2;
		arrowsY = sidebarCameLocations.back().Y + sidebarCameLocations.back().Height + 6;
	}
	else
	{
		centerOfCameoColumns = backgroundX + 500;
		arrowsY = backgroundY + 400;
	}

	int dropshipLoadout_UpArrowWidth = dropshipLoadout_UpArrowPCX ? dropshipLoadout_UpArrowPCX->Width : (dropshipLoadout_UpArrow ? dropshipLoadout_UpArrow->Width : 30);
	int dropshipLoadout_UpArrowHeight = dropshipLoadout_UpArrowPCX ? dropshipLoadout_UpArrowPCX->Height : (dropshipLoadout_UpArrow ? dropshipLoadout_UpArrow->Height : 30);
	upArrowX = customUpArrowLocation != Point2D::Empty ? (backgroundX + customUpArrowLocation.X) : (centerOfCameoColumns - dropshipLoadout_UpArrowWidth);
	upArrowY = customUpArrowLocation != Point2D::Empty ? (backgroundY + customUpArrowLocation.Y) : arrowsY;
	upArrowLocation = { upArrowX, upArrowY, dropshipLoadout_UpArrowWidth, dropshipLoadout_UpArrowHeight };

	int dropshipLoadout_DownArrowWidth = dropshipLoadout_DownArrowPCX ? dropshipLoadout_DownArrowPCX->Width : (dropshipLoadout_DownArrow ? dropshipLoadout_DownArrow->Width : 30);
	int dropshipLoadout_DownArrowHeight = dropshipLoadout_DownArrowPCX ? dropshipLoadout_DownArrowPCX->Height : (dropshipLoadout_DownArrow ? dropshipLoadout_DownArrow->Height : 30);
	downArrowX = customDownArrowLocation != Point2D::Empty ? (backgroundX + customDownArrowLocation.X) : centerOfCameoColumns;
	downArrowY = customDownArrowLocation != Point2D::Empty ? (backgroundY + customDownArrowLocation.Y) : arrowsY;
	downArrowLocation = { downArrowX, downArrowY, dropshipLoadout_DownArrowWidth, dropshipLoadout_DownArrowHeight };

	dGreenLocation.clear();

	if (pHouseTypeExt->DropshipLoadout_DGreenAnimationsCount.isset())
	{
		for (int i = 0; i < pHouseTypeExt->DropshipLoadout_DGreenAnimationsCount; i++)
		{
			Point2D location = pHouseTypeExt->DropshipLoadout_DGreenLocations[i];
			dGreenLocation.push_back({ backgroundX + location.X, backgroundY + location.Y, 0, 0 });
		}
	}
	else if (pGlobal && pGlobal->DropshipLoadout_DGreenAnimationsCount)
	{
		for (int i = 0; i < pGlobal->DropshipLoadout_DGreenAnimationsCount; i++)
		{
			Point2D location = pGlobal->DropshipLoadout_DGreenLocations[i];
			dGreenLocation.push_back({ backgroundX + location.X, backgroundY + location.Y, 0, 0 });
		}
	}
	else
	{
		int dGreenX = 371;
		int dGreenY = 10;

		for (int i = 0; i < 4; i++)
		{
			dGreenLocation.push_back({ backgroundX + dGreenX, backgroundY + dGreenY, 0, 0 });
			dGreenY += 50;
		}

		if (dropshipLoadout_DGreenListPCX.size() > 0)
		{
			for (size_t i = 4; i < dropshipLoadout_DGreenListPCX.size(); i++)
			{
				dGreenLocation.push_back({ backgroundX + dGreenX, backgroundY + dGreenY, 0, 0 });
				dGreenY += 50;
			}
		}
		else if (dropshipLoadout_DGreenList.size() > 0)
		{
			for (size_t i = 4; i < dropshipLoadout_DGreenList.size(); i++)
			{
				dGreenLocation.push_back({ backgroundX + dGreenX, backgroundY + dGreenY, 0, 0 });
				dGreenY += 50;
			}
		}
	}

	if (dropshipLoadout_DGreenListPCX.size() > 0)
	{
		for (size_t i = 0; i < dropshipLoadout_DGreenListPCX.size(); i++)
		{
			if (i < dGreenLocation.size() && dropshipLoadout_DGreenListPCX[i].size() > 0)
			{
				dGreenLocation[i].Width = dropshipLoadout_DGreenListPCX[i][0]->Width;
				dGreenLocation[i].Height = dropshipLoadout_DGreenListPCX[i][0]->Height;
			}
		}
	}
	else if (dropshipLoadout_DGreenList.size() > 0)
	{
		for (size_t i = 0; i < dropshipLoadout_DGreenList.size(); i++)
		{
			if (i < dGreenLocation.size() && dropshipLoadout_DGreenList[i] != nullptr)
			{
				dGreenLocation[i].Width = dropshipLoadout_DGreenList[i]->Width;
				dGreenLocation[i].Height = dropshipLoadout_DGreenList[i]->Height;
			}
		}
	}

	int dropshipLoadout_LoadoutWidth = dropshipLoadout_LoadoutPCX.size() > 0 ? dropshipLoadout_LoadoutPCX[0]->Width : (dropshipLoadout_Loadout ? dropshipLoadout_Loadout->Width : 100);
	int dropshipLoadout_LoadoutHeight = dropshipLoadout_LoadoutPCX.size() > 0 ? dropshipLoadout_LoadoutPCX[0]->Height : (dropshipLoadout_Loadout ? dropshipLoadout_Loadout->Height : 100);
	int dropshipLoadout_LoadoutX = 45;
	int dropshipLoadout_LoadoutY = 2;

	if (pHouseTypeExt->DropshipLoadout_LoadoutLocation.isset())
	{
		dropshipLoadout_LoadoutX = pHouseTypeExt->DropshipLoadout_LoadoutLocation.Get(Point2D::Empty).X;
		dropshipLoadout_LoadoutY = pHouseTypeExt->DropshipLoadout_LoadoutLocation.Get(Point2D::Empty).Y;
	}
	else if (pGlobal && pGlobal->DropshipLoadout_LoadoutLocation != Point2D::Empty)
	{
		dropshipLoadout_LoadoutX = pGlobal->DropshipLoadout_LoadoutLocation.X;
		dropshipLoadout_LoadoutY = pGlobal->DropshipLoadout_LoadoutLocation.Y;
	}

	loadoutLocation = { backgroundX + dropshipLoadout_LoadoutX, backgroundY + dropshipLoadout_LoadoutY, dropshipLoadout_LoadoutWidth, dropshipLoadout_LoadoutHeight };

	int dropshipLoadout_PilotLitWidth = dropshipLoadout_PilotLitPCX.size() > 0 ? dropshipLoadout_PilotLitPCX[0]->Width : (dropshipLoadout_PilotLit ? dropshipLoadout_PilotLit->Width : 100);
	int dropshipLoadout_PilotLitHeight = dropshipLoadout_PilotLitPCX.size() > 0 ? dropshipLoadout_PilotLitPCX[0]->Height : (dropshipLoadout_PilotLit ? dropshipLoadout_PilotLit->Height : 100);
	int dropshipLoadout_PilotLitX = 284;
	int dropshipLoadout_PilotLitY = 151;

	if (pHouseTypeExt->DropshipLoadout_PilotLitLocation.isset())
	{
		dropshipLoadout_PilotLitX = pHouseTypeExt->DropshipLoadout_PilotLitLocation.Get(Point2D::Empty).X;
		dropshipLoadout_PilotLitY = pHouseTypeExt->DropshipLoadout_PilotLitLocation.Get(Point2D::Empty).Y;
	}
	else if (pGlobal && pGlobal->DropshipLoadout_PilotLitLocation != Point2D::Empty)
	{
		dropshipLoadout_PilotLitX = pGlobal->DropshipLoadout_PilotLitLocation.X;
		dropshipLoadout_PilotLitY = pGlobal->DropshipLoadout_PilotLitLocation.Y;
	}

	pilotLitLocation = { backgroundX + dropshipLoadout_PilotLitX, backgroundY + dropshipLoadout_PilotLitY, dropshipLoadout_PilotLitWidth, dropshipLoadout_PilotLitHeight };

	nDropshipBayCameos = 5;
	dropshipBayCameLocations.clear();

	if (pHouseTypeExt->DropshipLoadout_DropshipCameosCount.Get(0) > 0)
	{
		nDropshipBayCameos = pHouseTypeExt->DropshipLoadout_DropshipCameosCount;
		for (int i = 0; i < nStartingDropships; i++)
		{
			std::vector<RectangleStruct> list;
			for (int j = 0; j < nDropshipBayCameos; j++)
			{
				int cameoX = backgroundX + pHouseTypeExt->DropshipLoadout_DropshipCameoLocations[i][j].X;
				int cameoY = backgroundY + pHouseTypeExt->DropshipLoadout_DropshipCameoLocations[i][j].Y;
				list.push_back({ cameoX, cameoY, cameoWidth, cameoHeight });
			}
			dropshipBayCameLocations.push_back(list);
		}
	}
	else if (pGlobal && pGlobal->DropshipLoadout_DropshipCameosCount > 0)
	{
		nDropshipBayCameos = pGlobal->DropshipLoadout_DropshipCameosCount;
		for (int i = 0; i < nStartingDropships; i++)
		{
			std::vector<RectangleStruct> list;
			for (int j = 0; j < nDropshipBayCameos; j++)
			{
				int cameoX = backgroundX + pGlobal->DropshipLoadout_DropshipCameoLocations[i][j].X;
				int cameoY = backgroundY + pGlobal->DropshipLoadout_DropshipCameoLocations[i][j].Y;
				list.push_back({ cameoX, cameoY, cameoWidth, cameoHeight });
			}
			dropshipBayCameLocations.push_back(list);
		}
	}
	else
	{
		if (nStartingDropships == 1 || nStartingDropships == 2)
		{
			int cameoX = backgroundX + 55;
			int cameoY = backgroundY + 69;
			std::vector<RectangleStruct> list;
			list.push_back({ cameoX, cameoY, cameoWidth, cameoHeight });
			list.push_back({ cameoX + 66, cameoY, cameoWidth, cameoHeight });
			list.push_back({ cameoX, cameoY + 50, cameoWidth, cameoHeight });
			list.push_back({ cameoX + 66, cameoY + 50, cameoWidth, cameoHeight });
			list.push_back({ cameoX + 132, cameoY + 50, cameoWidth, cameoHeight });
			dropshipBayCameLocations.push_back(list);
		}
		if (nStartingDropships == 2)
		{
			int cameoX = backgroundX + 55;
			int cameoY = backgroundY + 209;
			std::vector<RectangleStruct> list;
			list.push_back({ cameoX, cameoY, cameoWidth, cameoHeight });
			list.push_back({ cameoX + 66, cameoY, cameoWidth, cameoHeight });
			list.push_back({ cameoX, cameoY + 50, cameoWidth, cameoHeight });
			list.push_back({ cameoX + 66, cameoY + 50, cameoWidth, cameoHeight });
			list.push_back({ cameoX + 132, cameoY + 50, cameoWidth, cameoHeight });
			dropshipBayCameLocations.push_back(list);
		}
		if (nStartingDropships == 3)
		{
			int cameoX = backgroundX + 55;
			int cameoY = backgroundY + 39;
			std::vector<RectangleStruct> list1;
			list1.push_back({ cameoX, cameoY, cameoWidth, cameoHeight });
			list1.push_back({ cameoX + 66, cameoY, cameoWidth, cameoHeight });
			list1.push_back({ cameoX, cameoY + 50, cameoWidth, cameoHeight });
			list1.push_back({ cameoX + 66, cameoY + 50, cameoWidth, cameoHeight });
			list1.push_back({ cameoX + 132, cameoY + 50, cameoWidth, cameoHeight });
			dropshipBayCameLocations.push_back(list1);

			cameoY += 120;
			std::vector<RectangleStruct> list2;
			list2.push_back({ cameoX, cameoY, cameoWidth, cameoHeight });
			list2.push_back({ cameoX + 66, cameoY, cameoWidth, cameoHeight });
			list2.push_back({ cameoX, cameoY + 50, cameoWidth, cameoHeight });
			list2.push_back({ cameoX + 66, cameoY + 50, cameoWidth, cameoHeight });
			list2.push_back({ cameoX + 132, cameoY + 50, cameoWidth, cameoHeight });
			dropshipBayCameLocations.push_back(list2);

			cameoY += 120;
			std::vector<RectangleStruct> list3;
			list3.push_back({ cameoX, cameoY, cameoWidth, cameoHeight });
			list3.push_back({ cameoX + 66, cameoY, cameoWidth, cameoHeight });
			list3.push_back({ cameoX, cameoY + 50, cameoWidth, cameoHeight });
			list3.push_back({ cameoX + 66, cameoY + 50, cameoWidth, cameoHeight });
			list3.push_back({ cameoX + 132, cameoY + 50, cameoWidth, cameoHeight });
			dropshipBayCameLocations.push_back(list3);
		}
		// What if starting dropships is greater than 3? Or 0?
		if (dropshipBayCameLocations.size() < (size_t)nStartingDropships)
		{
			// Generate generic placements so it doesn't crash
			for (int i = (int)dropshipBayCameLocations.size(); i < nStartingDropships; i++)
			{
				int cameoX = backgroundX + 55;
				int cameoY = backgroundY + 39 + i * 120;
				std::vector<RectangleStruct> genericList;
				genericList.push_back({ cameoX, cameoY, cameoWidth, cameoHeight });
				genericList.push_back({ cameoX + 66, cameoY, cameoWidth, cameoHeight });
				genericList.push_back({ cameoX, cameoY + 50, cameoWidth, cameoHeight });
				genericList.push_back({ cameoX + 66, cameoY + 50, cameoWidth, cameoHeight });
				genericList.push_back({ cameoX + 132, cameoY + 50, cameoWidth, cameoHeight });
				dropshipBayCameLocations.push_back(genericList);
			}
		}
	}

	nDropshipBayTotalSlots = nStartingDropships * nDropshipBayCameos;
}

void DropshipLoadoutClass::CreateControls()
{
	const int cameoWidth = 60, cameoHeight = 48;
	buttonsList.clear();

	int btn_ScrollUp_ID = 100;
	ShapeButtonClass* btn_ScrollUp = CreateShapeButton(
		btn_ScrollUp_ID,
		0, 0,
		upArrowLocation.Width, upArrowLocation.Height,
		true
	);
	if (btn_ScrollUp)
	{
		btn_ScrollUp->SetPosition(upArrowLocation.X, upArrowLocation.Y);
		btn_ScrollUp->SetDimension(upArrowLocation.Width, upArrowLocation.Height);
		btn_ScrollUp->DrawPosition.X = upArrowX;
		btn_ScrollUp->DrawPosition.Y = upArrowY;
		buttonsList.push_back(btn_ScrollUp);
		commandManager = btn_ScrollUp;
	}
	else
	{
	}

	int btn_ScrollDown_ID = 101;
	ShapeButtonClass* btn_ScrollDown = CreateShapeButton(
		btn_ScrollDown_ID,
		0, 0,
		downArrowLocation.Width, downArrowLocation.Height,
		true
	);
	if (btn_ScrollDown)
	{
		btn_ScrollDown->SetPosition(downArrowLocation.X, downArrowLocation.Y);
		btn_ScrollDown->SetDimension(downArrowLocation.Width, downArrowLocation.Height);
		btn_ScrollDown->DrawPosition.X = downArrowX;
		btn_ScrollDown->DrawPosition.Y = downArrowY;
		buttonsList.push_back(btn_ScrollDown);
		if (commandManager)
			commandManager->Add(*btn_ScrollDown);
	}
	else
	{
	}

	int btn_BasicDropshipCameo_ID = 200;
	int newID = btn_BasicDropshipCameo_ID;
	dropshipBayChosenUnitsLists.clear();

	for (int i = 0; i < nStartingDropships; i++)
	{
		dropshipBayChosenUnitsLists.push_back(std::vector<TechnoTypeClass*>());
		if (i >= (int)dropshipBayCameLocations.size())
		{
			continue;
		}

		for (int j = 0; j < nDropshipBayCameos; j++)
		{
			if (j >= (int)dropshipBayCameLocations[i].size())
			{
				continue;
			}

			ShapeButtonClass* newButton = CreateShapeButton(
				newID,
				0, 0,
				cameoWidth, cameoHeight,
				true
			);
			if (newButton)
			{
				newButton->SetPosition(dropshipBayCameLocations[i][j].X, dropshipBayCameLocations[i][j].Y);
				newButton->SetDimension(cameoWidth, cameoHeight);
				newButton->DrawPosition.X = dropshipBayCameLocations[i][j].X;
				newButton->DrawPosition.Y = dropshipBayCameLocations[i][j].Y;
				buttonsList.push_back(newButton);
				if (commandManager)
					commandManager->Add(*newButton);
			}
			else
			{
			}
			dropshipBayChosenUnitsLists[i].push_back(nullptr);
			newID++;
		}
	}

	int btn_BasicSidebarCameo_ID = 300;
	for (int i = 0; i < nSidebarCameos; i++)
	{
		if (i >= (int)sidebarCameLocations.size())
		{
			continue;
		}

		int sID = btn_BasicSidebarCameo_ID + i;
		ShapeButtonClass* newButton = CreateShapeButton(
			sID,
			0, 0,
			cameoWidth, cameoHeight,
			true
		);
		if (newButton)
		{
			newButton->SetPosition(sidebarCameLocations[i].X, sidebarCameLocations[i].Y);
			newButton->SetDimension(cameoWidth, cameoHeight);
			newButton->DrawPosition.X = sidebarCameLocations[i].X;
			newButton->DrawPosition.Y = sidebarCameLocations[i].Y;
			buttonsList.push_back(newButton);
			if (commandManager)
				commandManager->Add(*newButton);
		}
		else
		{
		}
	}
}

void DropshipLoadoutClass::Run()
{
	DSurface* pSurface = DSurface::Hidden;
	if (!pSurface)
	{
		return;
	}

	pSurface->Fill(0);

	CalculateLayout(pSurface);
	CreateControls();

	const int voiceEva = pHouseTypeExt->DropshipLoadout_StartEVA.isset() ? pHouseTypeExt->DropshipLoadout_StartEVA.Get(-1) : (ScenarioExt::Global() ? ScenarioExt::Global()->DropshipLoadout_StartEVA.Get(-1) : -1);
	if (voiceEva >= 0)
	{
		VoxClass::PlayIndex(voiceEva);
	}

	const int themeIdx = pHouseTypeExt->DropshipLoadout_Theme.isset() ? pHouseTypeExt->DropshipLoadout_Theme : (ScenarioExt::Global() ? ScenarioExt::Global()->DropshipLoadout_Theme : -1);
	if (themeIdx == -1)
	{
		ThemeClass::Instance.Stop(true);
	}
	else
	{
		ThemeClass::Instance.Play(themeIdx);
	}

	if (WWMouseClass::Instance)
	{
		WWMouseClass::Instance->HideCursor();
		WWMouseClass::Instance->ShowCursor();
		WWMouseClass::Instance->CaptureMouse();
		WWMouseClass::Instance->RefCount = 0;
	}
	else
	{
	}

	if (commandManager)
	{
		commandManager->TurnOn();
	}
	else
	{
	}

	loadoutTotalFrames = dropshipLoadout_LoadoutPCX.size() > 0 ? (int)dropshipLoadout_LoadoutPCX.size() - 1 : (dropshipLoadout_Loadout ? dropshipLoadout_Loadout->Frames : 0);
	pilotLitTotalFrames = dropshipLoadout_PilotLitPCX.size() > 0 ? (int)dropshipLoadout_PilotLitPCX.size() - 1 : (dropshipLoadout_PilotLit ? dropshipLoadout_PilotLit->Frames : 0);

	animTimer_DelayedStartValue_Loadout = ScenarioClass::Instance->Random(0, 0);
	animTimer_DelayedStartValue_PilotLit = ScenarioClass::Instance->Random(100, 300);

	animTimer_DelayedStartTimer_Loadout.Start(animTimer_DelayedStartValue_Loadout);
	animTimer_DelayedStartTimer_PilotLit.Start(animTimer_DelayedStartValue_PilotLit);
	animTimer_UpdateFrameTimer_Loadout.Start(loadoutFrameDelay);
	animTimer_UpdateFrameTimer_PilotLit.Start(pilotLitFrameDelay);

	if (sidebarRowAnimationIndex >= 0)
	{
		if (dropshipLoadout_DGreenListPCX.size() > 0)
		{
			if (sidebarRowAnimationIndex < (int)dropshipLoadout_DGreenListPCX.size())
				sidebarRowAnimationTotalFrames = (int)dropshipLoadout_DGreenListPCX[sidebarRowAnimationIndex].size() - 1;
		}
		else if (sidebarRowAnimationIndex < (int)dropshipLoadout_DGreenList.size() && dropshipLoadout_DGreenList[sidebarRowAnimationIndex] != nullptr)
		{
			sidebarRowAnimationTotalFrames = dropshipLoadout_DGreenList[sidebarRowAnimationIndex]->Frames;
		}
	}

	pressedSpaceKey = false;
	repaintAll = true;
	bDropshipLoadoutActive = true;
	pendingScrolls = 0;
	pHoveredUnitType = nullptr;

	while (!pressedSpaceKey)
	{
		Game::CallBack();

		int command = 0;
		if (commandManager)
		{
			command = commandManager->Input();
		}

		int buttonID = -1;
		if (WWMouseClass::Instance)
		{
			RectangleStruct mouseRect = WWMouseClass::Instance->Rect2;

			for (auto button : buttonsList)
			{
				if (button && mouseRect.X >= button->X
					&& mouseRect.X <= (button->X + button->Width)
					&& mouseRect.Y >= button->Y
					&& mouseRect.Y <= (button->Y + button->Height))
				{
					buttonID = button->ID;
					break;
				}
			}
		}

		if (bDragPending || bIsDragging)
		{
			Point2D mousePos = { 0, 0 };
			if (WWMouseClass::Instance)
			{
				mousePos.X = WWMouseClass::Instance->GetX();
				mousePos.Y = WWMouseClass::Instance->GetY();
			}

			// 1. Check transition from pending to active drag
			if (bDragPending)
			{
				int dist = std::abs(mousePos.X - dragStartMousePos.X) + std::abs(mousePos.Y - dragStartMousePos.Y);
				if (dist >= 15)
				{
					// Transition to active drag!
					bIsDragging = true;
					bDragPending = false;

					// If dragging from a dropship slot, now temporarily remove it and refund it!
					if (nSourceDropshipIdx != -1)
					{
						dropshipBayChosenUnitsLists[nSourceDropshipIdx][nSourceSlotIdx] = nullptr;
						currentMoney += pDraggedUnitType->Cost;
						if (dropshipBayChosenUnitsCount.count(pDraggedUnitType) > 0)
						{
							--dropshipBayChosenUnitsCount[pDraggedUnitType];
						}
						VocClass::PlayGlobal(sellClickSoundIdx, 0x2000, 1.0);
					}
					repaintAll = true;
				}
			}

			// 2. Check if mouse is released
			if (!(GetAsyncKeyState(VK_LBUTTON) & 0x8000))
			{
				// Drag finished or clicked!
				if (bDragPending)
				{
					// Quick Click (button released before moving 15 pixels)
					bDragPending = false;

					if (nSourceDropshipIdx == -1) // Clicked on sidebar
					{
						// Normal purchase to first free slot
						int maxInstances = INT_MAX;
						for (size_t idx = 0; idx < availableUnits.size(); ++idx)
						{
							if (availableUnits[idx] == pDraggedUnitType)
							{
								maxInstances = availableUnitsMaximums[idx] < 0 ? INT_MAX : availableUnitsMaximums[idx];
								break;
							}
						}
						int nInstances = dropshipBayChosenUnitsCount.count(pDraggedUnitType) > 0 ? dropshipBayChosenUnitsCount[pDraggedUnitType] : 0;

						int totalDropshipChosenUnits = 0;
						for (const auto& pair : dropshipBayChosenUnitsCount)
						{
							totalDropshipChosenUnits += pair.second;
						}
						bool dropshipsWithFreeSlots = totalDropshipChosenUnits < nDropshipBayTotalSlots;

						if (nInstances < maxInstances
							&& pDraggedUnitType->Cost <= currentMoney
							&& dropshipsWithFreeSlots)
						{
							bool foundFreeSlot = false;
							for (int i = 0; i < (int)dropshipBayCameLocations.size() && !foundFreeSlot; i++)
							{
								for (int j = 0; j < (int)dropshipBayCameLocations[i].size() && !foundFreeSlot; j++)
								{
									if (!dropshipBayChosenUnitsLists[i][j])
									{
										dropshipBayChosenUnitsLists[i][j] = pDraggedUnitType;
										currentMoney -= pDraggedUnitType->Cost;
										foundFreeSlot = true;
										lastSelected = pDraggedUnitType;
										++dropshipBayChosenUnitsCount[pDraggedUnitType];
										VocClass::PlayGlobal(buyClickSoundIdx, 0x2000, 1.0);
									}
								}
							}
						}
					}
					else // Clicked on dropship slot
					{
						// Do not sell on quick left-click; just select it.
						lastSelected = pDraggedUnitType;
					}

					pDraggedUnitType = nullptr;
					repaintAll = true;
				}
				else if (bIsDragging)
				{
					// Drag & Drop drop logic
					int btn_BasicDropshipCameo_ID = 200;
					int btn_BasicSidebarCameo_ID = 300;
					bool droppedOnSlot = (buttonID >= btn_BasicDropshipCameo_ID && buttonID < (btn_BasicDropshipCameo_ID + nDropshipBayTotalSlots));
					bool droppedOnSidebar = (buttonID >= btn_BasicSidebarCameo_ID && buttonID < (btn_BasicSidebarCameo_ID + nSidebarCameos));
					bool droppedOnSidebarArea = false;
					if (nSourceDropshipIdx != -1 && !sidebarCameLocations.empty())
					{
						int minX = sidebarCameLocations[0].X;
						int minY = sidebarCameLocations[0].Y;
						int maxX = sidebarCameLocations[0].X + sidebarCameLocations[0].Width;
						int maxY = sidebarCameLocations[0].Y + sidebarCameLocations[0].Height;
						for (const auto& rect : sidebarCameLocations)
						{
							if (rect.X < minX) minX = rect.X;
							if (rect.Y < minY) minY = rect.Y;
							if (rect.X + rect.Width > maxX) maxX = rect.X + rect.Width;
							if (rect.Y + rect.Height > maxY) maxY = rect.Y + rect.Height;
						}
						if (upArrowLocation.Y < minY) minY = upArrowLocation.Y;
						if (downArrowLocation.Y < minY) minY = downArrowLocation.Y;
						if (upArrowLocation.Y + upArrowLocation.Height > maxY) maxY = upArrowLocation.Y + upArrowLocation.Height;
						if (downArrowLocation.Y + downArrowLocation.Height > maxY) maxY = downArrowLocation.Y + downArrowLocation.Height;

						int sidebarLeft = minX - 10;
						int sidebarTop = minY - 10;
						int sidebarRight = windowRectangle.X + windowRectangle.Width;
						int sidebarBottom = maxY + 10;

						if (mousePos.X >= sidebarLeft && mousePos.X <= sidebarRight
							&& mousePos.Y >= sidebarTop && mousePos.Y <= sidebarBottom)
						{
							droppedOnSidebarArea = true;
						}
					}

					auto ReturnToSource = [&]() {
						if (nSourceDropshipIdx != -1)
						{
							dropshipBayChosenUnitsLists[nSourceDropshipIdx][nSourceSlotIdx] = pDraggedUnitType;
							currentMoney -= pDraggedUnitType->Cost;
							++dropshipBayChosenUnitsCount[pDraggedUnitType];
							VocClass::PlayGlobal(buyClickSoundIdx, 0x2000, 1.0);
						}
					};

					if (droppedOnSlot)
					{
						int dropshipIndex = (buttonID - btn_BasicDropshipCameo_ID) / nDropshipBayCameos;
						int slotIndex = (buttonID - btn_BasicDropshipCameo_ID) - (dropshipIndex * nDropshipBayCameos);

						if (dropshipIndex < (int)dropshipBayChosenUnitsLists.size() && slotIndex < (int)dropshipBayChosenUnitsLists[dropshipIndex].size())
						{
							auto pTargetUnit = dropshipBayChosenUnitsLists[dropshipIndex][slotIndex];

							int maxInstances = INT_MAX;
							for (size_t idx = 0; idx < availableUnits.size(); ++idx)
							{
								if (availableUnits[idx] == pDraggedUnitType)
								{
									maxInstances = availableUnitsMaximums[idx] < 0 ? INT_MAX : availableUnitsMaximums[idx];
									break;
								}
							}
							int nInstances = dropshipBayChosenUnitsCount.count(pDraggedUnitType) > 0 ? dropshipBayChosenUnitsCount[pDraggedUnitType] : 0;

							if (pTargetUnit == nullptr)
							{
								if (nInstances < maxInstances && pDraggedUnitType->Cost <= currentMoney)
								{
									dropshipBayChosenUnitsLists[dropshipIndex][slotIndex] = pDraggedUnitType;
									currentMoney -= pDraggedUnitType->Cost;
									++dropshipBayChosenUnitsCount[pDraggedUnitType];
									lastSelected = pDraggedUnitType;
									VocClass::PlayGlobal(buyClickSoundIdx, 0x2000, 1.0);
								}
								else
								{
									ReturnToSource();
								}
							}
							else
							{
								if (nSourceDropshipIdx != -1)
								{
									// Dragged from a dropship slot -> SWAP them!
									dropshipBayChosenUnitsLists[dropshipIndex][slotIndex] = pDraggedUnitType;
									dropshipBayChosenUnitsLists[nSourceDropshipIdx][nSourceSlotIdx] = pTargetUnit;
									currentMoney -= pDraggedUnitType->Cost;
									++dropshipBayChosenUnitsCount[pDraggedUnitType];
									lastSelected = pDraggedUnitType;
									VocClass::PlayGlobal(buyClickSoundIdx, 0x2000, 1.0);
								}
								else
								{
									bool hasFreeSlot = false;
									for (auto const pType : dropshipBayChosenUnitsLists[dropshipIndex])
									{
										if (!pType)
										{
											hasFreeSlot = true;
											break;
										}
									}

									if (hasFreeSlot)
									{
										if (nInstances < maxInstances && pDraggedUnitType->Cost <= currentMoney)
										{
											int nullIdx = -1;
											if (nSourceDropshipIdx == dropshipIndex)
											{
												nullIdx = nSourceSlotIdx;
											}
											else
											{
												for (size_t k = 0; k < dropshipBayChosenUnitsLists[dropshipIndex].size(); ++k)
												{
													if (dropshipBayChosenUnitsLists[dropshipIndex][k] == nullptr)
													{
														nullIdx = static_cast<int>(k);
														break;
													}
												}
											}
											if (nullIdx != -1)
											{
												dropshipBayChosenUnitsLists[dropshipIndex].erase(dropshipBayChosenUnitsLists[dropshipIndex].begin() + nullIdx);
												dropshipBayChosenUnitsLists[dropshipIndex].insert(dropshipBayChosenUnitsLists[dropshipIndex].begin() + slotIndex, pDraggedUnitType);
											}

											currentMoney -= pDraggedUnitType->Cost;
											++dropshipBayChosenUnitsCount[pDraggedUnitType];
											lastSelected = pDraggedUnitType;
											VocClass::PlayGlobal(buyClickSoundIdx, 0x2000, 1.0);
										}
										else
										{
											// Can't afford shift, try replacement!
											long netCost = pDraggedUnitType->Cost - pTargetUnit->Cost;
											bool limitOk = (pDraggedUnitType == pTargetUnit) || (nInstances < maxInstances);

											if (limitOk && netCost <= currentMoney)
											{
												currentMoney += pTargetUnit->Cost;
												if (dropshipBayChosenUnitsCount.count(pTargetUnit) > 0)
												{
													--dropshipBayChosenUnitsCount[pTargetUnit];
												}

												dropshipBayChosenUnitsLists[dropshipIndex][slotIndex] = pDraggedUnitType;
												currentMoney -= pDraggedUnitType->Cost;
												++dropshipBayChosenUnitsCount[pDraggedUnitType];
												lastSelected = pDraggedUnitType;
												VocClass::PlayGlobal(buyClickSoundIdx, 0x2000, 1.0);
											}
											else
											{
												ReturnToSource();
											}
										}
									}
									else
									{
										long netCost = pDraggedUnitType->Cost - pTargetUnit->Cost;
										bool limitOk = (pDraggedUnitType == pTargetUnit) || (nInstances < maxInstances);

										if (limitOk && netCost <= currentMoney)
										{
											currentMoney += pTargetUnit->Cost;
											if (dropshipBayChosenUnitsCount.count(pTargetUnit) > 0)
											{
												--dropshipBayChosenUnitsCount[pTargetUnit];
											}

											dropshipBayChosenUnitsLists[dropshipIndex][slotIndex] = pDraggedUnitType;
											currentMoney -= pDraggedUnitType->Cost;
											++dropshipBayChosenUnitsCount[pDraggedUnitType];
											lastSelected = pDraggedUnitType;
											VocClass::PlayGlobal(buyClickSoundIdx, 0x2000, 1.0);
										}
										else
										{
											ReturnToSource();
										}
									}
								}
							}
						}
						else
						{
							ReturnToSource();
						}
					}
					else if ((droppedOnSidebar || droppedOnSidebarArea) && nSourceDropshipIdx != -1)
					{
						// Dropped on sidebar -> permanently sold/removed.
						// We already refunded the money and decremented the count when active drag started.
						// So we just let it be.
					}
					else
					{
						ReturnToSource();
					}

					bIsDragging = false;
					pDraggedUnitType = nullptr;
					repaintAll = true;
				}
			}
		}

		HandleInput(command, buttonID);
		UpdateAnimations();

		if (bIsDragging)
		{
			repaintAll = true;
		}

		if (repaintAll)
		{
			Render(pSurface);
			repaintAll = false;
		}

		GScreenClass::Instance.DoBlit(true, pSurface, nullptr);
	}

	bDropshipLoadoutActive = false;
	SaveCargo();
}

void DropshipLoadoutClass::HandleInput(int command, int buttonID)
{
	int btn_ScrollUp_ID = 100;
	int btn_ScrollDown_ID = 101;
	int btn_BasicDropshipCameo_ID = 200;
	int btn_BasicSidebarCameo_ID = 300;

	if (bIsDragging || bDragPending)
	{
		return;
	}

	bool pressedLeftClick = command == 1;
	if (pressedLeftClick)
	{
		Point2D mousePos = { 0, 0 };
		if (WWMouseClass::Instance)
		{
			mousePos.X = WWMouseClass::Instance->GetX();
			mousePos.Y = WWMouseClass::Instance->GetY();
		}

		if (buttonID >= btn_BasicSidebarCameo_ID && buttonID < (btn_BasicSidebarCameo_ID + nSidebarCameos))
		{
			int sidebarIndex = firstBrowsableCameo + (buttonID - btn_BasicSidebarCameo_ID);
			if (sidebarIndex < (int)availableUnits.size())
			{
				auto const pType = availableUnits[sidebarIndex];
				if (pType)
				{
					int maxInstances = availableUnitsMaximums[sidebarIndex] < 0 ? INT_MAX : availableUnitsMaximums[sidebarIndex];
					int nInstances = dropshipBayChosenUnitsCount.count(pType) > 0 ? dropshipBayChosenUnitsCount[pType] : 0;
					if (nInstances < maxInstances)
					{
						bDragPending = true;
						pDraggedUnitType = pType;
						nSourceDropshipIdx = -1;
						nSourceSlotIdx = -1;
						dragStartMousePos = mousePos;
						return;
					}
				}
			}
		}
		else if (buttonID >= btn_BasicDropshipCameo_ID && buttonID < (btn_BasicDropshipCameo_ID + nDropshipBayTotalSlots))
		{
			int dropshipIndex = (buttonID - btn_BasicDropshipCameo_ID) / nDropshipBayCameos;
			int slotIndex = (buttonID - btn_BasicDropshipCameo_ID) - (dropshipIndex * nDropshipBayCameos);
			if (dropshipIndex < (int)dropshipBayChosenUnitsLists.size() && slotIndex < (int)dropshipBayChosenUnitsLists[dropshipIndex].size())
			{
				auto pType = dropshipBayChosenUnitsLists[dropshipIndex][slotIndex];
				if (pType)
				{
					bDragPending = true;
					pDraggedUnitType = pType;
					nSourceDropshipIdx = dropshipIndex;
					nSourceSlotIdx = slotIndex;
					dragStartMousePos = mousePos;
					return;
				}
			}
		}
	}

	TechnoTypeClass* pPrevHovered = pHoveredUnitType;
	pHoveredUnitType = nullptr;

	if (buttonID >= btn_BasicSidebarCameo_ID && buttonID < (btn_BasicSidebarCameo_ID + nSidebarCameos))
	{
		int sidebarIndex = firstBrowsableCameo + (buttonID - btn_BasicSidebarCameo_ID);
		if (sidebarIndex < (int)availableUnits.size())
		{
			pHoveredUnitType = availableUnits[sidebarIndex];
		}
	}
	else if (buttonID >= btn_BasicDropshipCameo_ID && buttonID < (btn_BasicDropshipCameo_ID + nDropshipBayTotalSlots))
	{
		int dropshipIndex = (buttonID - btn_BasicDropshipCameo_ID) / nDropshipBayCameos;
		int slotIndex = (buttonID - btn_BasicDropshipCameo_ID) - (dropshipIndex * nDropshipBayCameos);
		if (dropshipIndex < (int)dropshipBayChosenUnitsLists.size() && slotIndex < (int)dropshipBayChosenUnitsLists[dropshipIndex].size())
		{
			pHoveredUnitType = dropshipBayChosenUnitsLists[dropshipIndex][slotIndex];
		}
	}

	if (pHoveredUnitType != pPrevHovered)
	{
		repaintAll = true;
	}

	pressedLeftClick = command == 1;
	bool pressedRightClick = command == 2;

	bool isAnySidebarCameo = buttonID >= btn_BasicSidebarCameo_ID && buttonID < (btn_BasicSidebarCameo_ID + nSidebarCameos);
	bool isHoveringOverSidebarCameos = command == 0 && isAnySidebarCameo;
	bool pressedAnySidebarCameo = pressedLeftClick && isAnySidebarCameo;
	bool pressedAnySidebarCameoWithRigthClick = pressedRightClick && isAnySidebarCameo;

	bool isAnyDropshipCameo = buttonID >= btn_BasicDropshipCameo_ID && buttonID < (btn_BasicDropshipCameo_ID + nDropshipBayTotalSlots);
	bool isHoveringOverDropshipCameos = command == 0 && isAnyDropshipCameo;
	bool pressedAnyDropshipCameo = pressedRightClick && isAnyDropshipCameo;
	int mouseOverDropshipCameoID = isHoveringOverDropshipCameos ? buttonID : -1;

	bool isUpArrow = buttonID == btn_ScrollUp_ID;
	bool isDownArrow = buttonID == btn_ScrollDown_ID;
	bool pressedUpArrow = command == VK_UP || ((pressedLeftClick || command == (32768 + btn_ScrollUp_ID)) && isUpArrow);
	bool pressedDownArrow = command == VK_DOWN || (pressedLeftClick && isDownArrow);

	if (pendingScrolls < 0)
	{
		pressedUpArrow = true;
		pendingScrolls++;
	}
	else if (pendingScrolls > 0)
	{
		pressedDownArrow = true;
		pendingScrolls--;
	}

	if (pressedUpArrow)
		command = btn_ScrollUp_ID;
	else if (pressedDownArrow)
		command = btn_ScrollDown_ID;
	else if (pressedAnySidebarCameo || pressedAnyDropshipCameo || pressedAnySidebarCameoWithRigthClick)
		command = buttonID;

	if (command != 0 || buttonID != -1)
	{
	}

	bool validSidebarCameoPurchase = false;
	freeDropshipSlots = false;
	Point2D mouseLocationInDropshipCameos = { 0, 0 };

	for (int i = 0; i < (int)dropshipBayCameLocations.size() && !freeDropshipSlots; i++)
	{
		if (i >= (int)dropshipBayChosenUnitsLists.size())
		{
			continue;
		}

		for (int j = 0; j < (int)dropshipBayCameLocations[i].size() && !freeDropshipSlots; j++)
		{
			if (j >= (int)dropshipBayChosenUnitsLists[i].size())
			{
				continue;
			}

			if (dropshipBayChosenUnitsLists[i][j])
				continue;
			freeDropshipSlots = true;
			break;
		}
	}

	if (isHoveringOverSidebarCameos || pressedAnySidebarCameo)
	{
		int sidebarIndex = firstBrowsableCameo + (buttonID - btn_BasicSidebarCameo_ID);
		if (sidebarIndex < (int)availableUnits.size())
		{
			auto const pType = availableUnits[sidebarIndex];
			if (pType)
			{
				int maxInstances = availableUnitsMaximums[sidebarIndex] < 0 ? INT_MAX : availableUnitsMaximums[sidebarIndex];
				int nInstances = dropshipBayChosenUnitsCount.count(pType) > 0 ? dropshipBayChosenUnitsCount[pType] : 0;

				if (nInstances < maxInstances
					&& pType->Cost <= currentMoney
					&& freeDropshipSlots)
				{
					validSidebarCameoPurchase = true;
				}
			}
		}
	}

	if (isHoveringOverDropshipCameos)
	{
		bool found = false;
		for (int i = 0; i < (int)dropshipBayCameLocations.size() && !found; i++)
		{
			for (int j = 0; j < (int)dropshipBayCameLocations[i].size() && !found; j++)
			{
				int dropshipIndex = (mouseOverDropshipCameoID - btn_BasicDropshipCameo_ID) / nDropshipBayCameos;
				int slotIndex = mouseOverDropshipCameoID - btn_BasicDropshipCameo_ID - (dropshipIndex * nDropshipBayCameos);

				if (i == dropshipIndex && j == slotIndex)
				{
					mouseLocationInDropshipCameos = { i, j };
					found = true;
					break;
				}
			}
		}
	}

	if (pressedUpArrow)
	{
		if (firstBrowsableCameo >= 2)
		{
			firstBrowsableCameo -= 2;
			repaintAll = true;
			VocClass::PlayGlobal(arrowsClickSoundIdx, 0x2000, 1.0);
		}
	}
	else if (pressedDownArrow)
	{
		if (availableUnits.size() > (size_t)(firstBrowsableCameo + nSidebarCameos))
		{
			firstBrowsableCameo += 2;
			repaintAll = true;
			VocClass::PlayGlobal(arrowsClickSoundIdx, 0x2000, 1.0);
		}
	}
	else if (pressedAnySidebarCameoWithRigthClick)
	{
		int newIndex = firstBrowsableCameo + (command - btn_BasicSidebarCameo_ID);
		if (newIndex >= 0 && newIndex < (int)availableUnits.size())
		{
			auto const pType = availableUnits[newIndex];
			if (pType)
			{
				bool found = false;
				for (int i = (int)dropshipBayChosenUnitsLists.size() - 1; i >= 0 && !found; --i)
				{
					auto& dropshipBay = dropshipBayChosenUnitsLists[i];
					for (int j = (int)dropshipBay.size() - 1; j >= 0 && !found; --j)
					{
						if (dropshipBay[j] == pType)
						{
							currentMoney += pType->Cost;
							dropshipBay.erase(dropshipBay.begin() + j);
							dropshipBay.push_back(nullptr);
							found = true;
							repaintAll = true;

							if (dropshipBayChosenUnitsCount.count(pType) > 0)
								--dropshipBayChosenUnitsCount[pType];
							else
								dropshipBayChosenUnitsCount[pType] = 0;

							VocClass::PlayGlobal(sellClickSoundIdx, 0x2000, 1.0);
							break;
						}
					}
				}
			}
		}
	}
	else if (pressedAnySidebarCameo)
	{
		int newIndex = firstBrowsableCameo + (command - btn_BasicSidebarCameo_ID);
		if (newIndex >= 0 && newIndex < (int)availableUnits.size())
		{
			if (validSidebarCameoPurchase)
			{
				auto const pType = availableUnits[newIndex];
				if (pType)
				{
					bool foundFreeSlot = false;

					for (int i = 0; i < (int)dropshipBayCameLocations.size() && !foundFreeSlot; i++)
					{
						if (i >= (int)dropshipBayChosenUnitsLists.size())
						{
							continue;
						}

						for (int j = 0; j < (int)dropshipBayCameLocations[i].size() && !foundFreeSlot; j++)
						{
							if (j >= (int)dropshipBayChosenUnitsLists[i].size())
							{
								continue;
							}

							auto const pDropshipSlotType = dropshipBayChosenUnitsLists[i][j];
							if (pDropshipSlotType)
								continue;

							dropshipBayChosenUnitsLists[i][j] = pType;
							currentMoney -= pType->Cost;
							foundFreeSlot = true;
							lastSelected = pType;

							++dropshipBayChosenUnitsCount[pType];
							VocClass::PlayGlobal(buyClickSoundIdx, 0x2000, 1.0);
							break;
						}
					}

					if (foundFreeSlot)
						repaintAll = true;

					if (sidebarRowAnimationIndex < 0)
					{
						sidebarRowAnimationIndex = ((command - btn_BasicSidebarCameo_ID) / 2);
						if (dropshipLoadout_DGreenListPCX.size() > 0)
						{
							if (sidebarRowAnimationIndex < (int)dropshipLoadout_DGreenListPCX.size())
								animTimer_UpdateFrameTimer_SidebarRowAnimation.Start(sidebarRowAnimationFrameDelay);
							else
								sidebarRowAnimationIndex = -1;

							sidebarRowAnimationTotalFrames = sidebarRowAnimationIndex >= 0 ? (int)dropshipLoadout_DGreenListPCX[sidebarRowAnimationIndex].size() - 1 : 0;
						}
						else
						{
							if (sidebarRowAnimationIndex < (int)dropshipLoadout_DGreenList.size())
								animTimer_UpdateFrameTimer_SidebarRowAnimation.Start(sidebarRowAnimationFrameDelay);
							else
								sidebarRowAnimationIndex = -1;

							sidebarRowAnimationTotalFrames = (sidebarRowAnimationIndex >= 0 && dropshipLoadout_DGreenList[sidebarRowAnimationIndex] != nullptr) ? dropshipLoadout_DGreenList[sidebarRowAnimationIndex]->Frames : 0;
						}
					}
				}
			}
			else
			{
			}
		}
	}
	else if (pressedAnyDropshipCameo)
	{
		if (nDropshipBayCameos > 0)
		{
			int nDropship = (command - btn_BasicDropshipCameo_ID) / nDropshipBayCameos;
			int index = command - btn_BasicDropshipCameo_ID - (nDropship * nDropshipBayCameos);

			if (nDropship >= 0 && nDropship < (int)dropshipBayChosenUnitsLists.size())
			{
				if (index >= 0 && index < (int)dropshipBayChosenUnitsLists[nDropship].size())
				{
					auto pType = dropshipBayChosenUnitsLists[nDropship][index];
					if (pType)
					{
						currentMoney += pType->Cost;
						auto& affectedDropship = dropshipBayChosenUnitsLists[nDropship];
						affectedDropship.erase(affectedDropship.begin() + index);
						affectedDropship.push_back(nullptr);
						repaintAll = true;

						if (dropshipBayChosenUnitsCount.count(pType) > 0)
							--dropshipBayChosenUnitsCount[pType];
						else
							dropshipBayChosenUnitsCount[pType] = 0;

						VocClass::PlayGlobal(sellClickSoundIdx, 0x2000, 1.0);
					}
				}
			}
		}
	}
	else if (isHoveringOverDropshipCameos || isHoveringOverSidebarCameos)
	{
		lastTimeWasOverCameos = true;
		repaintAll = true;
	}
	else if (lastTimeWasOverCameos && !isHoveringOverDropshipCameos && !isHoveringOverSidebarCameos)
	{
		lastTimeWasOverCameos = false;
		repaintAll = true;
	}

	if (command == VK_SPACE)
	{
		pressedSpaceKey = true;
	}

	if (command == VK_ESCAPE)
	{
		bool soldAny = false;
		lastSelected = nullptr;
		dropshipBayChosenUnitsCount.clear();

		for (auto& dropshipBay : dropshipBayChosenUnitsLists)
		{
			for (auto& slot : dropshipBay)
			{
				if (slot != nullptr)
					soldAny = true;
				slot = nullptr;
			}
		}

		currentMoney = initialMoney;
		repaintAll = true;

		if (soldAny)
			VocClass::PlayGlobal(sellClickSoundIdx, 0x2000, 1.0);
	}
}

void DropshipLoadoutClass::UpdateAnimations()
{
	if (animTimer_DelayedStartTimer_Loadout.Completed())
	{
		if (animTimer_UpdateFrameTimer_Loadout.Completed())
		{
			if (currentLoadoutFrame < loadoutTotalFrames)
			{
				currentLoadoutFrame++;
			}
			else
			{
				currentLoadoutFrame = -1;
				animTimer_DelayedStartValue_Loadout = ScenarioClass::Instance->Random(0, 0);
				animTimer_DelayedStartTimer_Loadout.Start(animTimer_DelayedStartValue_Loadout);
			}

			animTimer_UpdateFrameTimer_Loadout.Start(loadoutFrameDelay);
			repaintAll = true;
		}
	}

	if (animTimer_DelayedStartTimer_PilotLit.Completed())
	{
		if (animTimer_UpdateFrameTimer_PilotLit.Completed())
		{
			if (currentPilotLitFrame < pilotLitTotalFrames)
				currentPilotLitFrame++;
			else
			{
				currentPilotLitFrame = -1;
				animTimer_DelayedStartValue_PilotLit = ScenarioClass::Instance->Random(100, 300);
				animTimer_DelayedStartTimer_PilotLit.Start(animTimer_DelayedStartValue_PilotLit);
			}

			animTimer_UpdateFrameTimer_PilotLit.Start(pilotLitFrameDelay);
			repaintAll = true;
		}
	}

	if (sidebarRowAnimationIndex >= 0)
	{
		if (animTimer_UpdateFrameTimer_SidebarRowAnimation.Completed())
		{
			if (currentSidebarRowAnimationFrame < sidebarRowAnimationTotalFrames)
			{
				currentSidebarRowAnimationFrame++;
				animTimer_UpdateFrameTimer_SidebarRowAnimation.Start(sidebarRowAnimationFrameDelay);
			}
			else
			{
				currentSidebarRowAnimationFrame = -1;
				sidebarRowAnimationIndex = -1;
			}

			repaintAll = true;
		}
	}

	if (animTimer_UpdateFrameTimer.Completed())
		animTimer_UpdateFrameTimer.Start(animTimer_StartValue);
}

void DropshipLoadoutClass::Render(DSurface* pSurface)
{
	if (!pSurface)
	{
		return;
	}
	pSurface->Fill(0);
	GeneralUtils::DrawImage(
		pSurface,
		windowRectangle,
		dropshipLoadout_BackgroundPCX,
		dropshipLoadout_Background,
		dropshipLoadout_Palette
	);

	bool isHoveringSidebar = false;
	if (WWMouseClass::Instance)
	{
		RectangleStruct mouseRect = WWMouseClass::Instance->Rect2;
		for (const auto& rect : sidebarCameLocations)
		{
			if (mouseRect.X >= rect.X && mouseRect.X <= (rect.X + rect.Width)
				&& mouseRect.Y >= rect.Y && mouseRect.Y <= (rect.Y + rect.Height))
			{
				isHoveringSidebar = true;
				break;
			}
		}
	}

	bool isMouseOverSidebarArea = false;
	if (WWMouseClass::Instance && !sidebarCameLocations.empty())
	{
		int minX = sidebarCameLocations[0].X;
		int minY = sidebarCameLocations[0].Y;
		int maxX = sidebarCameLocations[0].X + sidebarCameLocations[0].Width;
		int maxY = sidebarCameLocations[0].Y + sidebarCameLocations[0].Height;
		for (const auto& rect : sidebarCameLocations)
		{
			if (rect.X < minX) minX = rect.X;
			if (rect.Y < minY) minY = rect.Y;
			if (rect.X + rect.Width > maxX) maxX = rect.X + rect.Width;
			if (rect.Y + rect.Height > maxY) maxY = rect.Y + rect.Height;
		}
		if (upArrowLocation.Y < minY) minY = upArrowLocation.Y;
		if (downArrowLocation.Y < minY) minY = downArrowLocation.Y;
		if (upArrowLocation.Y + upArrowLocation.Height > maxY) maxY = upArrowLocation.Y + upArrowLocation.Height;
		if (downArrowLocation.Y + downArrowLocation.Height > maxY) maxY = downArrowLocation.Y + downArrowLocation.Height;

		int sidebarLeft = minX - 10;
		int sidebarTop = minY - 10;
		int sidebarRight = windowRectangle.X + windowRectangle.Width;
		int sidebarBottom = maxY + 10;

		RectangleStruct mouseRect = WWMouseClass::Instance->Rect2;
		if (mouseRect.X >= sidebarLeft && mouseRect.X <= sidebarRight
			&& mouseRect.Y >= sidebarTop && mouseRect.Y <= sidebarBottom)
		{
			isMouseOverSidebarArea = true;
		}
	}

	for (int i = 0; i < nSidebarCameos; i++)
	{
		int newIndex = firstBrowsableCameo + i;
		if (newIndex >= (int)availableUnits.size())
			continue;

		if (i >= (int)sidebarCameLocations.size())
		{
			continue;
		}

		auto const pType = availableUnits[newIndex];
		if (!pType)
			continue;

		int maxInstances = availableUnitsMaximums[newIndex] < 0 ? INT_MAX : availableUnitsMaximums[newIndex];
		int nInstances = dropshipBayChosenUnitsCount.count(pType) > 0 ? dropshipBayChosenUnitsCount[pType] : 0;

		int totalDropshipChosenUnits = 0;
		for (const auto& pair : dropshipBayChosenUnitsCount)
		{
			totalDropshipChosenUnits += pair.second;
		}

		bool dropshipsWithFreeSlots = totalDropshipChosenUnits < nDropshipBayTotalSlots;

		BlitterFlags bf = BlitterFlags::None;
		if (nInstances >= maxInstances || !dropshipsWithFreeSlots)
			bf = BlitterFlags::bf_400 | BlitterFlags::Darken;

		bool isHovering = false;
		if (!bIsDragging && !bDragPending && !pDraggedUnitType && WWMouseClass::Instance)
		{
			RectangleStruct mouseRect = WWMouseClass::Instance->Rect2;
			isHovering = mouseRect.X >= sidebarCameLocations[i].X
				&& mouseRect.X <= (sidebarCameLocations[i].X + sidebarCameLocations[i].Width)
				&& mouseRect.Y >= sidebarCameLocations[i].Y
				&& mouseRect.Y <= (sidebarCameLocations[i].Y + sidebarCameLocations[i].Height);
		}

		ColorStruct foreColor;
		bool showHighlight = false;

		if (isHovering)
		{
			showHighlight = true;
			bool limitReached = (nInstances >= maxInstances);
			bool canBuyDirectly = (!limitReached && pType->Cost <= currentMoney && freeDropshipSlots);
			bool canReplaceAny = false;
			if (!limitReached)
			{
				for (auto const& dropship : dropshipBayChosenUnitsLists)
				{
					for (auto const pTarget : dropship)
					{
						if (pTarget && pType != pTarget)
						{
							long netCost = pType->Cost - pTarget->Cost;
							if (netCost <= currentMoney)
							{
								canReplaceAny = true;
								break;
							}
						}
					}
					if (canReplaceAny) break;
				}
			}

			if (canBuyDirectly)
			{
				foreColor = ColorStruct { 0, 255, 0 }; // Green
			}
			else if (canReplaceAny)
			{
				foreColor = ColorStruct { 0, 0, 255 }; // Blue
			}
			else
			{
				foreColor = ColorStruct { 255, 0, 0 }; // Red
			}
		}
		else if (pType == lastSelected)
		{
			showHighlight = true;
			foreColor = ColorStruct { 255, 239, 99 }; // Yellow
		}

		if (showHighlight)
		{
			RectangleStruct newRectangle = sidebarCameLocations[i];
			newRectangle.X -= 2;
			newRectangle.Width += 4;
			pSurface->FillRectTrans(&newRectangle, &foreColor, 255);
		}

		auto const pTypeExt = TechnoTypeExt::ExtMap.Find(pType);
		if (!pTypeExt)
		{
			continue;
		}

		auto const pPCXSurface = pTypeExt->CameoPCX.GetSurface();
		auto pFileSHP = pType->Cameo;
		auto pPalette = FileSystem::CAMEO_PAL;

		GeneralUtils::DrawImage(
			pSurface,
			sidebarCameLocations[i],
			pPCXSurface,
			pFileSHP,
			pPalette,
			0,
			-2,
			bf
		);
	}

	GeneralUtils::DrawImage(
		pSurface,
		upArrowLocation,
		dropshipLoadout_UpArrowPCX,
		dropshipLoadout_UpArrow,
		dropshipLoadout_Palette,
		0,
		-2
	);

	GeneralUtils::DrawImage(
		pSurface,
		downArrowLocation,
		dropshipLoadout_DownArrowPCX,
		dropshipLoadout_DownArrow,
		dropshipLoadout_Palette,
		0,
		-2
	);

	for (size_t i = 0; i < dropshipBayCameLocations.size(); i++)
	{
		if (i >= dropshipBayChosenUnitsLists.size())
		{
			continue;
		}

		for (size_t j = 0; j < dropshipBayCameLocations[i].size(); j++)
		{
			if (j >= dropshipBayChosenUnitsLists[i].size())
			{
				continue;
			}

			auto const pType = dropshipBayChosenUnitsLists[i][j];

			bool isHovering = false;
			if (WWMouseClass::Instance)
			{
				RectangleStruct mouseRect = WWMouseClass::Instance->Rect2;
				isHovering = mouseRect.X >= dropshipBayCameLocations[i][j].X
					&& mouseRect.X <= (dropshipBayCameLocations[i][j].X + dropshipBayCameLocations[i][j].Width)
					&& mouseRect.Y >= dropshipBayCameLocations[i][j].Y
					&& mouseRect.Y <= (dropshipBayCameLocations[i][j].Y + dropshipBayCameLocations[i][j].Height);
			}

			ColorStruct foreColor;
			bool showHighlight = false;

			if (isHovering)
			{
				showHighlight = true;
				foreColor = ColorStruct { 255, 0, 0 };
				if (bIsDragging)
				{
					if (!pType)
					{
						if (pDraggedUnitType->Cost <= currentMoney)
							foreColor = ColorStruct { 0, 0, 255 }; // Blue (empty slot valid drop)
						else
							showHighlight = false; // Cannot afford: no highlight
					}
					else
					{
						if (nSourceDropshipIdx != -1)
						{
							foreColor = ColorStruct { 0, 0, 255 }; // Blue (swap)
						}
						else
						{
							bool targetDropshipHasFreeSlot = false;
							for (auto const pUnit : dropshipBayChosenUnitsLists[i])
							{
								if (!pUnit)
								{
									targetDropshipHasFreeSlot = true;
									break;
								}
							}

							// Can we afford a shift?
							bool canAffordShift = pDraggedUnitType->Cost <= currentMoney;

							// Can we afford a replacement?
							long netCost = pDraggedUnitType->Cost - pType->Cost;
							bool canAffordReplacement = netCost <= currentMoney;

							if (targetDropshipHasFreeSlot && canAffordShift)
								foreColor = ColorStruct { 0, 0, 255 }; // Blue (shift)
							else if (canAffordReplacement && pDraggedUnitType != pType)
								foreColor = ColorStruct { 255, 0, 0 }; // Red (overwrite/replace)
							else
								showHighlight = false; // Cannot afford either or redundant: no highlight
						}
					}
				}
				else
				{
					if (pType)
						foreColor = ColorStruct { 255, 0, 0 }; // Red (sellable hover)
					else
						showHighlight = false; // Don't highlight empty slot if not dragging
				}
			}
			else if (!bIsDragging && pType && isHoveringSidebar && pHoveredUnitType)
			{
				// If hovering a sidebar cameo, see if this slot can be replaced by it
				int maxInstances = INT_MAX;
				for (size_t idx = 0; idx < availableUnits.size(); ++idx)
				{
					if (availableUnits[idx] == pHoveredUnitType)
					{
						maxInstances = availableUnitsMaximums[idx] < 0 ? INT_MAX : availableUnitsMaximums[idx];
						break;
					}
				}
				int nInstances = dropshipBayChosenUnitsCount.count(pHoveredUnitType) > 0 ? dropshipBayChosenUnitsCount[pHoveredUnitType] : 0;
				bool limitReached = (nInstances >= maxInstances);
				bool canBuyDirectly = (!limitReached && pHoveredUnitType->Cost <= currentMoney && freeDropshipSlots);

				if (pHoveredUnitType == pType)
				{
					// Hovering the same unit type: highlight in Red if limit is reached (to show where they are)
					if (limitReached)
					{
						showHighlight = true;
						foreColor = ColorStruct { 255, 0, 0 }; // Red
					}
				}
				else
				{
					// Only show replacement highlights on dropship cargo slots if the hovered unit CANNOT be bought normally
					if (!canBuyDirectly)
					{
						bool limitOk = !limitReached;
						long netCost = pHoveredUnitType->Cost - pType->Cost;
						if (limitOk && netCost <= currentMoney)
						{
							showHighlight = true;
							foreColor = ColorStruct { 0, 0, 255 }; // Blue (can be replaced)
						}
					}
				}
			}

			if (showHighlight)
			{
				RectangleStruct newRectangle = dropshipBayCameLocations[i][j];
				newRectangle.X -= 2;
				newRectangle.Width += 4;
				pSurface->FillRectTrans(&newRectangle, &foreColor, 255);
			}

			if (!pType)
				continue;

			auto const pTypeExt = TechnoTypeExt::ExtMap.Find(pType);
			if (!pTypeExt)
			{
				continue;
			}

			auto const pPCXSurface = pTypeExt->CameoPCX.GetSurface();
			auto pFileSHP = pType->Cameo;
			auto pPalette = FileSystem::CAMEO_PAL;

			GeneralUtils::DrawImage(
				pSurface,
				dropshipBayCameLocations[i][j],
				pPCXSurface,
				pFileSHP,
				pPalette,
				0,
				-2
			);
		}
	}

	if (currentLoadoutFrame >= 0)
	{
		BSurface* framePCX = nullptr;
		if (dropshipLoadout_LoadoutPCX.size() > 0)
		{
			if (currentLoadoutFrame < (int)dropshipLoadout_LoadoutPCX.size())
			{
				framePCX = dropshipLoadout_LoadoutPCX[currentLoadoutFrame];
			}
			else
			{
			}
		}

		GeneralUtils::DrawImage(
			pSurface,
			loadoutLocation,
			framePCX,
			dropshipLoadout_Loadout,
			dropshipLoadout_Palette,
			currentLoadoutFrame,
			-2
		);
	}

	if (currentPilotLitFrame >= 0)
	{
		BSurface* framePCX = nullptr;
		if (dropshipLoadout_PilotLitPCX.size() > 0)
		{
			if (currentPilotLitFrame < (int)dropshipLoadout_PilotLitPCX.size())
			{
				framePCX = dropshipLoadout_PilotLitPCX[currentPilotLitFrame];
			}
			else
			{
			}
		}

		GeneralUtils::DrawImage(
			pSurface,
			pilotLitLocation,
			framePCX,
			dropshipLoadout_PilotLit,
			dropshipLoadout_Palette,
			currentPilotLitFrame,
			-2
		);
	}

	if (sidebarRowAnimationIndex >= 0 && currentSidebarRowAnimationFrame >= 0)
	{
		if (sidebarRowAnimationIndex < (int)dGreenLocation.size())
		{
			BSurface* framePCX = nullptr;
			if (dropshipLoadout_DGreenListPCX.size() > 0)
			{
				if (sidebarRowAnimationIndex < (int)dropshipLoadout_DGreenListPCX.size())
				{
					if (currentSidebarRowAnimationFrame < (int)dropshipLoadout_DGreenListPCX[sidebarRowAnimationIndex].size())
					{
						framePCX = dropshipLoadout_DGreenListPCX[sidebarRowAnimationIndex][currentSidebarRowAnimationFrame];
					}
					else
					{
					}
				}
			}

			SHPStruct* fileSHP = nullptr;
			if (sidebarRowAnimationIndex < (int)dropshipLoadout_DGreenList.size())
			{
				fileSHP = dropshipLoadout_DGreenList[sidebarRowAnimationIndex];
			}
			else
			{
			}

			GeneralUtils::DrawImage(
				pSurface,
				dGreenLocation[sidebarRowAnimationIndex],
				framePCX,
				fileSHP,
				dropshipLoadout_Palette,
				currentSidebarRowAnimationFrame,
				-2
			);
		}
		else
		{
		}
	}

	wchar_t buffer[64];
	swprintf_s(buffer, L"Credits: %d", currentMoney);
	COLORREF foreColor = Drawing::RGB_To_Int(255, 239, 99);
	TextPrintType style = (TextPrintType::FullShadow | TextPrintType::Point6Grad);
	Point2D creditsLabel = {
		windowRectangle.Width - 140,
		windowRectangle.Height - 15
	};
	pSurface->DrawTextA(buffer, &windowRectangle, &creditsLabel, foreColor, 0, style);

	swprintf_s(buffer, L"Press SPACE to start the mission");
	foreColor = Drawing::RGB_To_Int(255, 255, 255);
	style = (TextPrintType::Center | TextPrintType::FullShadow | TextPrintType::Point6Grad);
	Point2D pressSpaceLabel = {
		(windowRectangle.Width - 175) / 2,
		windowRectangle.Height - 15
	};
	pSurface->DrawTextA(buffer, &windowRectangle, &pressSpaceLabel, foreColor, 0, style);

	// Draw Dragged Cameo
	if (bIsDragging && pDraggedUnitType)
	{
		auto const pTypeExt = TechnoTypeExt::ExtMap.Find(pDraggedUnitType);
		if (pTypeExt)
		{
			auto const pPCXSurface = pTypeExt->CameoPCX.GetSurface();
			auto pFileSHP = pDraggedUnitType->Cameo;
			auto pPalette = FileSystem::CAMEO_PAL;

			Point2D mousePos = { 0, 0 };
			if (WWMouseClass::Instance)
			{
				mousePos.X = WWMouseClass::Instance->GetX();
				mousePos.Y = WWMouseClass::Instance->GetY();
			}

			// Center the cameo on the mouse cursor
			const int cameoWidth = 60, cameoHeight = 48;
			RectangleStruct dragLoc = { mousePos.X - cameoWidth / 2, mousePos.Y - cameoHeight / 2, cameoWidth, cameoHeight };

			// Draw Highlight Border first (Blue by default, Red if dragged from dropship and hovering sidebar)
			RectangleStruct newRectangle = dragLoc;
			newRectangle.X -= 2;
			newRectangle.Width += 4;
			ColorStruct dragBorderColor = ColorStruct { 0, 0, 255 }; // Blue
			if (nSourceDropshipIdx != -1 && isMouseOverSidebarArea)
			{
				dragBorderColor = ColorStruct { 255, 0, 0 }; // Red (sell indicator)
			}
			pSurface->FillRectTrans(&newRectangle, &dragBorderColor, 255);

			// Draw the cameo with half transparency to make it look like a drag shadow
			GeneralUtils::DrawImage(
				pSurface,
				dragLoc,
				pPCXSurface,
				pFileSHP,
				pPalette,
				0,
				-2,
				BlitterFlags::bf_400 | BlitterFlags::Darken
			);
		}
	}

	this->DrawTooltip(pSurface);
}

void DropshipLoadoutClass::DrawTooltip(DSurface* pSurface)
{
	if (bIsDragging)
		return;

	if (!pHoveredUnitType)
		return;

	if (!BitFont::Instance || !BitText::Instance)
		return;

	int maxToolTipWidth = Phobos::UI::MaxToolTipWidth > 0 ? Phobos::UI::MaxToolTipWidth : 200;

	// Calculate maxLimit
	int maxLimit = -1;
	for (size_t idx = 0; idx < availableUnits.size(); ++idx)
	{
		if (availableUnits[idx] == pHoveredUnitType)
		{
			maxLimit = availableUnitsMaximums[idx];
			break;
		}
	}

	// Calculate currentCount
	int currentCount = 0;
	if (dropshipBayChosenUnitsCount.count(pHoveredUnitType) > 0)
	{
		currentCount = dropshipBayChosenUnitsCount[pHoveredUnitType];
	}

	// Determine dimensions of each line to compute the total box size
	int textWidth = 0;
	int textHeight = 0;

	// 1. Name line
	int nameWidth = 0, nameHeight = 0;
	std::wstring nameStr = pHoveredUnitType->UIName;
	BitFont::Instance->GetTextDimension(nameStr.c_str(), &nameWidth, &nameHeight, maxToolTipWidth);
	textWidth = std::max(textWidth, nameWidth);
	textHeight += nameHeight;

	// 2. Availability line (if limit exists)
	int availWidth = 0, availHeight = 0;
	std::wstring availLabel = L"Available: ";
	std::wstring availValueStr;
	int availLabelWidth = 0, availLabelHeight = 0;
	int availValueWidth = 0, availValueHeight = 0;
	if (maxLimit > 0)
	{
		BitFont::Instance->GetTextDimension(availLabel.c_str(), &availLabelWidth, &availLabelHeight, maxToolTipWidth);

		std::wostringstream availValueOss;
		availValueOss << (maxLimit - currentCount) << L"/" << maxLimit;
		availValueStr = availValueOss.str();
		BitFont::Instance->GetTextDimension(availValueStr.c_str(), &availValueWidth, &availValueHeight, maxToolTipWidth);

		availWidth = availLabelWidth + availValueWidth;
		availHeight = std::max(availLabelHeight, availValueHeight);
		textWidth = std::max(textWidth, availWidth);
		textHeight += availHeight + 2; // +2 line spacing
	}

	// 3. Cost line
	std::wstring costLabelStr = L"Cost: ";
	int costLabelWidth = 0, costLabelHeight = 0;
	BitFont::Instance->GetTextDimension(costLabelStr.c_str(), &costLabelWidth, &costLabelHeight, maxToolTipWidth);

	int cost = pHoveredUnitType->GetActualCost(HouseClass::CurrentPlayer);
	std::wostringstream costValOss;
	costValOss << Phobos::UI::CostLabel << std::abs(cost);
	std::wstring costValStr = costValOss.str();
	int costValWidth = 0, costValHeight = 0;
	BitFont::Instance->GetTextDimension(costValStr.c_str(), &costValWidth, &costValHeight, maxToolTipWidth);

	int fullCostWidth = costLabelWidth + costValWidth;
	int fullCostHeight = std::max(costLabelHeight, costValHeight);
	textWidth = std::max(textWidth, fullCostWidth);
	textHeight += fullCostHeight + 2; // +2 line spacing

	// 4. Description
	std::wstring descStr;
	int descWidth = 0, descHeight = 0;
	auto const pTypeExt = TechnoTypeExt::ExtMap.Find(pHoveredUnitType);
	if (pTypeExt && Phobos::Config::ToolTipDescriptions && !pTypeExt->UIDescription.Get().empty())
	{
		descStr = pTypeExt->UIDescription.Get().Text;
		BitFont::Instance->GetTextDimension(descStr.c_str(), &descWidth, &descHeight, maxToolTipWidth);
		textWidth = std::max(textWidth, descWidth);
		textHeight += descHeight + 4; // +4 for extra paragraph gap
	}

	// Calculate final box bounds
	int boxPadding = 5;
	int boxWidth = textWidth + boxPadding * 2;
	int boxHeight = textHeight + boxPadding * 2;

	Point2D mousePos = { 0, 0 };
	if (WWMouseClass::Instance)
	{
		mousePos.X = WWMouseClass::Instance->GetX();
		mousePos.Y = WWMouseClass::Instance->GetY();
	}

	int minX = windowRectangle.X;
	int maxX = windowRectangle.X + windowRectangle.Width;
	int minY = windowRectangle.Y;
	int maxY = windowRectangle.Y + windowRectangle.Height;

	int boxX = mousePos.X + 15;
	int boxY = mousePos.Y + 15;

	if (boxX + boxWidth > maxX)
		boxX = mousePos.X - boxWidth - 5;
	if (boxY + boxHeight > maxY)
		boxY = maxY - boxHeight - 5;

	if (boxX < minX) boxX = minX;
	if (boxY < minY) boxY = minY;

	RectangleStruct boxRect = { boxX, boxY, boxWidth, boxHeight };

	// Draw translucent black background
	ColorStruct bgColor(0, 0, 0);
	pSurface->FillRectTrans(&boxRect, &bgColor, 180);

	// Draw border outline
	pSurface->DrawRect(&boxRect, Drawing::RGB_To_Int(120, 120, 120));

	// Save BitFont state to prevent side effects on other parts of UI
	LTRBStruct oldBounds = BitFont::Instance->Bounds;
	WORD oldColor = BitFont::Instance->Color;
	bool oldField41 = BitFont::Instance->field_41;

	// Set shared BitFont properties
	LTRBStruct ltrbBounds = { boxRect.X, boxRect.Y, boxRect.X + boxRect.Width, boxRect.Y + boxRect.Height };
	BitFont::Instance->field_41 = 1;
	BitFont::Instance->SetBounds(&ltrbBounds);

	int currentY = boxRect.Y + boxPadding;

	// 1. Draw Name (Yellow/Gold)
	BitFont::Instance->Color = static_cast<WORD>(Drawing::RGB_To_Int(255, 239, 99));
	BitText::Instance->DrawText(
		BitFont::Instance,
		pSurface,
		nameStr.c_str(),
		boxRect.X + boxPadding,
		currentY,
		nameWidth,
		nameHeight,
		0, 0, 0
	);
	currentY += nameHeight + 2;

	// 2. Draw Availability (if limit exists)
	if (maxLimit > 0)
	{
		// Draw label "Available: " (White)
		BitFont::Instance->Color = static_cast<WORD>(Drawing::RGB_To_Int(255, 255, 255));
		BitText::Instance->DrawText(
			BitFont::Instance,
			pSurface,
			availLabel.c_str(),
			boxRect.X + boxPadding,
			currentY,
			availLabelWidth,
			availLabelHeight,
			0, 0, 0
		);

		// Draw value (Red / Yellow / White)
		int availableCount = maxLimit - currentCount;
		COLORREF availColor = Drawing::RGB_To_Int(255, 255, 255); // White
		if (availableCount == 0)
		{
			availColor = Drawing::RGB_To_Int(255, 0, 0); // Red
		}
		else if (availableCount * 2 <= maxLimit)
		{
			availColor = Drawing::RGB_To_Int(255, 255, 0); // Yellow
		}

		BitFont::Instance->Color = static_cast<WORD>(availColor);
		BitText::Instance->DrawText(
			BitFont::Instance,
			pSurface,
			availValueStr.c_str(),
			boxRect.X + boxPadding + availLabelWidth,
			currentY,
			availValueWidth,
			availValueHeight,
			0, 0, 0
		);

		currentY += availHeight + 2;
	}

	// 3. Draw Cost
	// Draw label "Cost: " (White)
	BitFont::Instance->Color = static_cast<WORD>(Drawing::RGB_To_Int(255, 255, 255));
	BitText::Instance->DrawText(
		BitFont::Instance,
		pSurface,
		costLabelStr.c_str(),
		boxRect.X + boxPadding,
		currentY,
		costLabelWidth,
		costLabelHeight,
		0, 0, 0
	);

	// Draw value (Red / Yellow / White)
	COLORREF costColor = Drawing::RGB_To_Int(255, 255, 255); // White
	if (currentMoney < cost)
	{
		costColor = Drawing::RGB_To_Int(255, 0, 0); // Red
	}
	else if (currentMoney < cost * 2)
	{
		costColor = Drawing::RGB_To_Int(255, 255, 0); // Yellow
	}

	BitFont::Instance->Color = static_cast<WORD>(costColor);
	BitText::Instance->DrawText(
		BitFont::Instance,
		pSurface,
		costValStr.c_str(),
		boxRect.X + boxPadding + costLabelWidth,
		currentY,
		costValWidth,
		costValHeight,
		0, 0, 0
	);

	currentY += fullCostHeight + 2;

	// 4. Draw Description (if exists)
	if (!descStr.empty())
	{
		currentY += 2; // Small gap before description paragraph
		BitFont::Instance->Color = static_cast<WORD>(Drawing::RGB_To_Int(200, 200, 200)); // Light Gray
		BitText::Instance->DrawText(
			BitFont::Instance,
			pSurface,
			descStr.c_str(),
			boxRect.X + boxPadding,
			currentY,
			descWidth,
			descHeight,
			0, 0, 0
		);
	}

	// Restore BitFont state
	BitFont::Instance->Bounds = oldBounds;
	BitFont::Instance->Color = oldColor;
	BitFont::Instance->field_41 = oldField41;
}

void DropshipLoadoutClass::SaveCargo()
{
	if (!HouseClass::CurrentPlayer)
	{
		return;
	}

	auto pHouseExt = HouseExt::ExtMap.Find(HouseClass::CurrentPlayer);
	if (!pHouseExt)
	{
		return;
	}

	pHouseExt->DropshipLoadout_Cargo.clear();
	pHouseExt->DropshipLoadout_Carriers.clear();

	std::vector<TechnoTypeClass*> carriers;

	if (pHouseTypeExt->DropshipLoadout_Carriers.size() > 0)
	{
		for (auto carrier : pHouseTypeExt->DropshipLoadout_Carriers)
		{
			carriers.push_back(carrier);
		}
	}
	else if (ScenarioExt::Global())
	{
		for (auto carrier : ScenarioExt::Global()->DropshipLoadout_Carriers)
		{
			carriers.push_back(carrier);
		}
	}

	int nCarriers = (int)carriers.size();

	for (int i = 0; i < nStartingDropships && i < nCarriers; i++)
	{
		pHouseExt->DropshipLoadout_Carriers.push_back(carriers[i]);
		std::vector<TechnoTypeClass*> unitsList;

		if (i >= (int)dropshipBayChosenUnitsLists.size())
		{
			continue;
		}

		for (auto const pTechno : dropshipBayChosenUnitsLists[i])
		{
			if (pTechno)
			{
				unitsList.push_back(pTechno);
			}
		}

		pHouseExt->DropshipLoadout_Cargo.push_back(unitsList);
		unitsList.clear();
	}

	bool addUnusedMoneyToPlayer = pHouseTypeExt->DropshipLoadout_AddUnusedMoneyToPlayer.isset() ? pHouseTypeExt->DropshipLoadout_AddUnusedMoneyToPlayer : (ScenarioExt::Global() ? ScenarioExt::Global()->DropshipLoadout_AddUnusedMoneyToPlayer : false);

	if (addUnusedMoneyToPlayer)
	{
		HouseClass::CurrentPlayer->TransactMoney(currentMoney);
	}
	else
	{
		long dropshipLoadout_InitialMoney = pHouseTypeExt->DropshipLoadout_Money.isset() ? pHouseTypeExt->DropshipLoadout_Money : (ScenarioExt::Global() ? ScenarioExt::Global()->DropshipLoadout_Money : -1);

		if (dropshipLoadout_InitialMoney < 0)
		{
			long spent = HouseClass::CurrentPlayer->Available_Money() - currentMoney;
			HouseClass::CurrentPlayer->TransactMoney(-spent);
		}
	}
}

DEFINE_HOOK(0x4B6C30, Dropship_Loadout_Remake, 0x0)
{
	enum { EndFunction = 0x4B9690 };

	if (!HouseClass::CurrentPlayer)
	{
		return EndFunction;
	}

	auto const pHouseTypeExt = HouseTypeExt::ExtMap.Find(HouseClass::CurrentPlayer->Type);
	if (!pHouseTypeExt)
	{
		return EndFunction;
	}

	if (!ScenarioClass::Instance)
	{
		return EndFunction;
	}

	int nStartingDropships = pHouseTypeExt->DropshipLoadout_StartingDropships.isset() ? pHouseTypeExt->DropshipLoadout_StartingDropships : ScenarioClass::Instance->StartingDropships;

	if (nStartingDropships <= 0)
	{
		return EndFunction;
	}

	DropshipLoadoutClass loadout;
	if (loadout.Initialize())
	{
		loadout.Run();
	}
	else
	{
	}

	return EndFunction;
}


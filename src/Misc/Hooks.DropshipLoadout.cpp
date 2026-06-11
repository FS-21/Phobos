
#include <ScenarioClass.h>

#include <ThemeClass.h>
#include <WWMouseClass.h>
#include <Drawing.h>
#include <BitFont.h>

#include <Utilities/Macro.h>
#include <Utilities/TemplateDef.h>

#include <Ext/Scenario/Body.h>
#include <ToggleClass.h>
#include <ShapeButtonClass.h>
#include <Ext/House/Body.h>
#include <Ext/HouseType/Body.h>

#include <Utilities/GeneralUtils.h>

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
	upArrowX = customUpArrowLocation != Point2D::Empty ? customUpArrowLocation.X : (centerOfCameoColumns - dropshipLoadout_UpArrowWidth);
	upArrowY = customUpArrowLocation != Point2D::Empty ? customUpArrowLocation.Y : arrowsY;
	upArrowLocation = { backgroundX + upArrowX, backgroundY + upArrowY, dropshipLoadout_UpArrowWidth, dropshipLoadout_UpArrowHeight };

	int dropshipLoadout_DownArrowWidth = dropshipLoadout_DownArrowPCX ? dropshipLoadout_DownArrowPCX->Width : (dropshipLoadout_DownArrow ? dropshipLoadout_DownArrow->Width : 30);
	int dropshipLoadout_DownArrowHeight = dropshipLoadout_DownArrowPCX ? dropshipLoadout_DownArrowPCX->Height : (dropshipLoadout_DownArrow ? dropshipLoadout_DownArrow->Height : 30);
	downArrowX = customDownArrowLocation != Point2D::Empty ? customDownArrowLocation.X : centerOfCameoColumns;
	downArrowY = customDownArrowLocation != Point2D::Empty ? customDownArrowLocation.Y : arrowsY;
	downArrowLocation = { backgroundX + downArrowX, backgroundY + downArrowY, dropshipLoadout_DownArrowWidth, dropshipLoadout_DownArrowHeight };

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

		HandleInput(command, buttonID);
		UpdateAnimations();

		if (repaintAll)
		{
			Render(pSurface);
			repaintAll = false;
		}

		GScreenClass::Instance.DoBlit(true, pSurface, nullptr);
	}

	SaveCargo();
}

void DropshipLoadoutClass::HandleInput(int command, int buttonID)
{
	int btn_ScrollUp_ID = 100;
	int btn_ScrollDown_ID = 101;
	int btn_BasicDropshipCameo_ID = 200;
	int btn_BasicSidebarCameo_ID = 300;

	bool pressedLeftClick = command == 1;
	bool pressedRightClick = command == 2;

	bool isAnySidebarCameo = buttonID >= btn_BasicSidebarCameo_ID && buttonID < (btn_BasicSidebarCameo_ID + nSidebarCameos);
	bool isHoveringOverSidebarCameos = command == 0 && isAnySidebarCameo;
	bool pressedAnySidebarCameo = pressedLeftClick && isAnySidebarCameo;
	bool pressedAnySidebarCameoWithRigthClick = pressedRightClick && isAnySidebarCameo;

	bool isAnyDropshipCameo = buttonID >= btn_BasicDropshipCameo_ID && buttonID < (btn_BasicDropshipCameo_ID + nDropshipBayTotalSlots);
	bool isHoveringOverDropshipCameos = command == 0 && isAnyDropshipCameo;
	bool pressedAnyDropshipCameo = pressedLeftClick && isAnyDropshipCameo;
	int mouseOverDropshipCameoID = isHoveringOverDropshipCameos ? buttonID : -1;

	bool isUpArrow = buttonID == btn_ScrollUp_ID;
	bool isDownArrow = buttonID == btn_ScrollDown_ID;
	bool pressedUpArrow = command == VK_UP || ((pressedLeftClick || command == (32768 + btn_ScrollUp_ID)) && isUpArrow);
	bool pressedDownArrow = command == VK_DOWN || (pressedLeftClick && isDownArrow);

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
	GeneralUtils::DrawImage(
		pSurface,
		windowRectangle,
		dropshipLoadout_BackgroundPCX,
		dropshipLoadout_Background,
		dropshipLoadout_Palette
	);

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
		if (WWMouseClass::Instance)
		{
			RectangleStruct mouseRect = WWMouseClass::Instance->Rect2;
			isHovering = mouseRect.X >= sidebarCameLocations[i].X
				&& mouseRect.X <= (sidebarCameLocations[i].X + sidebarCameLocations[i].Width)
				&& mouseRect.Y >= sidebarCameLocations[i].Y
				&& mouseRect.Y <= (sidebarCameLocations[i].Y + sidebarCameLocations[i].Height);
		}

		bool validSidebarCameoPurchase = false;
		if (isHovering && freeDropshipSlots)
		{
			if (nInstances < maxInstances && pType->Cost <= currentMoney)
				validSidebarCameoPurchase = true;
		}

		if (isHovering && validSidebarCameoPurchase)
		{
			auto foreColor = ColorStruct { 0, 255, 0 };
			RectangleStruct newRectangle = sidebarCameLocations[i];
			newRectangle.X -= 2;
			newRectangle.Width += 4;
			pSurface->FillRectTrans(&newRectangle, &foreColor, 255);
		}
		else if (pType == lastSelected)
		{
			RectangleStruct newRectangle = sidebarCameLocations[i];
			newRectangle.X -= 2;
			newRectangle.Width += 4;
			auto foreColor = ColorStruct { 255, 239, 99 };
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
			if (!pType)
				continue;

			bool isHovering = false;
			if (WWMouseClass::Instance)
			{
				RectangleStruct mouseRect = WWMouseClass::Instance->Rect2;
				isHovering = mouseRect.X >= dropshipBayCameLocations[i][j].X
					&& mouseRect.X <= (dropshipBayCameLocations[i][j].X + dropshipBayCameLocations[i][j].Width)
					&& mouseRect.Y >= dropshipBayCameLocations[i][j].Y
					&& mouseRect.Y <= (dropshipBayCameLocations[i][j].Y + dropshipBayCameLocations[i][j].Height);
			}

			if (isHovering)
			{
				RectangleStruct newRectangle = dropshipBayCameLocations[i][j];
				newRectangle.X -= 2;
				newRectangle.Width += 4;
				auto foreColor = ColorStruct { 255, 0, 0 };
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



#include <ScenarioClass.h>

#include <ThemeClass.h>
#include <WWMouseClass.h>
#include <Drawing.h>
#include <BitFont.h>

#include <Utilities/Macro.h>
#include <Utilities/Debug.h>
#include <Utilities/TemplateDef.h>

#include <Ext/Scenario/Body.h>
#include <ToggleClass.h>
#include <ShapeButtonClass.h>
#include <Ext/House/Body.h>
#include <Ext/HouseType/Body.h>

#include <Utilities/GeneralUtils.h>

static ShapeButtonClass* CreateShapeButton(unsigned int nID, int nX, int nY, int nWidth, int nHeight, bool bIsAlpha)
{
	Debug::Log("[DropshipLoadout] CreateShapeButton - Creating button: ID=%u, X=%d, Y=%d, W=%d, H=%d, Alpha=%d\n", nID, nX, nY, nWidth, nHeight, bIsAlpha ? 1 : 0);

	Debug::Log("[DropshipLoadout] CreateShapeButton - Allocating memory (size=%d)...\n", (int)sizeof(ShapeButtonClass));
	auto const pButton = GameAllocator<ShapeButtonClass>().allocate(1);
	if (!pButton)
	{
		Debug::Log("[DropshipLoadout] CreateShapeButton - Error: Allocation failed!\n");
		return nullptr;
	}
	Debug::Log("[DropshipLoadout] CreateShapeButton - Memory allocated at %p\n", pButton);

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
	Debug::Log("[DropshipLoadout] CreateShapeButton - Invoking constructor at 0x69DD30...\n");
	auto const pResult = pConstructor(pButton, nID, nX, nY, nWidth, nHeight, nullptr, bIsAlpha);
	Debug::Log("[DropshipLoadout] CreateShapeButton - Constructor completed, result=%p\n", pResult);
	return pResult;
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
	Debug::Log("[DropshipLoadout] Constructor\n");
}

DropshipLoadoutClass::~DropshipLoadoutClass()
{
	Debug::Log("[DropshipLoadout] Destructor - Start\n");
	for (size_t i = 0; i < buttonsList.size(); ++i)
	{
		auto button = buttonsList[i];
		Debug::Log("[DropshipLoadout] Destructor - Deleting button %d/%d at %p (ID: %d)\n", (int)i, (int)buttonsList.size(), button, button ? button->ID : -1);
		if (button)
		{
			GameDelete(button);
		}
	}
	buttonsList.clear();
	Debug::Log("[DropshipLoadout] Destructor - Buttons cleared\n");

	for (size_t i = 0; i < dropshipLoadout_DGreenList.size(); ++i)
	{
		auto dGreen = dropshipLoadout_DGreenList[i];
		Debug::Log("[DropshipLoadout] Destructor - Checking dGreen %d/%d at %p\n", (int)i, (int)dropshipLoadout_DGreenList.size(), dGreen);
		if (dGreen)
		{
			bool isGlobal = false;
			if (ScenarioExt::Global() && i < ScenarioExt::Global()->DropshipLoadout_DGreenList.size())
				isGlobal = (dGreen == ScenarioExt::Global()->DropshipLoadout_DGreenList[i]);

			Debug::Log("[DropshipLoadout] Destructor - dGreen %d isGlobal: %d\n", (int)i, isGlobal ? 1 : 0);
			if (!isGlobal)
			{
				Debug::Log("[DropshipLoadout] Destructor - Deleting dGreen %d at %p\n", (int)i, dGreen);
				GameDelete(dGreen);
			}
		}
	}
	dropshipLoadout_DGreenList.clear();
	Debug::Log("[DropshipLoadout] Destructor - dGreen list cleared\n");

	if (dropshipLoadout_Palette)
	{
		bool isGlobal = ScenarioExt::Global() && (dropshipLoadout_Palette == ScenarioExt::Global()->DropshipLoadout_Palette);
		Debug::Log("[DropshipLoadout] Destructor - Palette palette at %p, isGlobal: %d\n", dropshipLoadout_Palette, isGlobal ? 1 : 0);
		if (!isGlobal)
		{
			Debug::Log("[DropshipLoadout] Destructor - Deleting palette\n");
			GameDelete(dropshipLoadout_Palette);
		}
	}

	if (dropshipLoadout_Background)
	{
		bool isGlobal = ScenarioExt::Global() && (dropshipLoadout_Background == ScenarioExt::Global()->DropshipLoadout_Background);
		Debug::Log("[DropshipLoadout] Destructor - Background at %p, isGlobal: %d\n", dropshipLoadout_Background, isGlobal ? 1 : 0);
		if (!isGlobal)
		{
			Debug::Log("[DropshipLoadout] Destructor - Deleting background\n");
			GameDelete(dropshipLoadout_Background);
		}
	}

	if (dropshipLoadout_UpArrow)
	{
		bool isGlobal = ScenarioExt::Global() && (dropshipLoadout_UpArrow == ScenarioExt::Global()->DropshipLoadout_UpArrow);
		Debug::Log("[DropshipLoadout] Destructor - UpArrow at %p, isGlobal: %d\n", dropshipLoadout_UpArrow, isGlobal ? 1 : 0);
		if (!isGlobal)
		{
			Debug::Log("[DropshipLoadout] Destructor - Deleting UpArrow\n");
			GameDelete(dropshipLoadout_UpArrow);
		}
	}

	if (dropshipLoadout_DownArrow)
	{
		bool isGlobal = ScenarioExt::Global() && (dropshipLoadout_DownArrow == ScenarioExt::Global()->DropshipLoadout_DownArrow);
		Debug::Log("[DropshipLoadout] Destructor - DownArrow at %p, isGlobal: %d\n", dropshipLoadout_DownArrow, isGlobal ? 1 : 0);
		if (!isGlobal)
		{
			Debug::Log("[DropshipLoadout] Destructor - Deleting DownArrow\n");
			GameDelete(dropshipLoadout_DownArrow);
		}
	}

	if (dropshipLoadout_Loadout)
	{
		bool isGlobal = ScenarioExt::Global() && (dropshipLoadout_Loadout == ScenarioExt::Global()->DropshipLoadout_Loadout);
		Debug::Log("[DropshipLoadout] Destructor - Loadout at %p, isGlobal: %d\n", dropshipLoadout_Loadout, isGlobal ? 1 : 0);
		if (!isGlobal)
		{
			Debug::Log("[DropshipLoadout] Destructor - Deleting loadout\n");
			GameDelete(dropshipLoadout_Loadout);
		}
	}

	if (dropshipLoadout_PilotLit)
	{
		bool isGlobal = ScenarioExt::Global() && (dropshipLoadout_PilotLit == ScenarioExt::Global()->DropshipLoadout_PilotLit);
		Debug::Log("[DropshipLoadout] Destructor - PilotLit at %p, isGlobal: %d\n", dropshipLoadout_PilotLit, isGlobal ? 1 : 0);
		if (!isGlobal)
		{
			Debug::Log("[DropshipLoadout] Destructor - Deleting pilotlit\n");
			GameDelete(dropshipLoadout_PilotLit);
		}
	}
	Debug::Log("[DropshipLoadout] Destructor - End\n");
}

bool DropshipLoadoutClass::Initialize()
{
	Debug::Log("[DropshipLoadout] Initialize - Start\n");
	if (!HouseClass::CurrentPlayer)
	{
		Debug::Log("[DropshipLoadout] Initialize - Error: HouseClass::CurrentPlayer is null!\n");
		return false;
	}
	Debug::Log("[DropshipLoadout] Initialize - HouseClass::CurrentPlayer: %p, Country: %s\n", HouseClass::CurrentPlayer, HouseClass::CurrentPlayer->Type ? HouseClass::CurrentPlayer->Type->ID : "UNKNOWN");

	pHouseTypeExt = HouseTypeExt::ExtMap.Find(HouseClass::CurrentPlayer->Type);
	Debug::Log("[DropshipLoadout] Initialize - pHouseTypeExt: %p\n", pHouseTypeExt);
	if (!pHouseTypeExt)
	{
		Debug::Log("[DropshipLoadout] Initialize - Error: pHouseTypeExt is null\n");
		return false;
	}

	if (!ScenarioClass::Instance)
	{
		Debug::Log("[DropshipLoadout] Initialize - Error: ScenarioClass::Instance is null!\n");
		return false;
	}

	nStartingDropships = pHouseTypeExt->DropshipLoadout_StartingDropships.isset() ? pHouseTypeExt->DropshipLoadout_StartingDropships : ScenarioClass::Instance->StartingDropships;
	Debug::Log("[DropshipLoadout] Initialize - nStartingDropships = %d\n", nStartingDropships);
	if (nStartingDropships <= 0)
	{
		Debug::Log("[DropshipLoadout] Initialize - nStartingDropships <= 0, returning false\n");
		return false;
	}

	Debug::Log("[DropshipLoadout] Initialize - Calling LoadAssets()\n");
	LoadAssets();
	Debug::Log("[DropshipLoadout] Initialize - End (success)\n");
	return true;
}

void DropshipLoadoutClass::LoadAssets()
{
	Debug::Log("[DropshipLoadout] LoadAssets - Start\n");
	auto const pGlobal = ScenarioExt::Global();
	Debug::Log("[DropshipLoadout] LoadAssets - ScenarioExt::Global() address: %p\n", pGlobal);

	if (pGlobal && pGlobal->DropshipLoadout_Palette)
	{
		Debug::Log("[DropshipLoadout] LoadAssets - Loading global palette: %p\n", pGlobal->DropshipLoadout_Palette);
		dropshipLoadout_Palette = pGlobal->DropshipLoadout_Palette;
	}
	else
	{
		Debug::Log("[DropshipLoadout] LoadAssets - Loading DROPSHIP.PAL file...\n");
		dropshipLoadout_Palette = FileSystem::LoadPALFile("DROPSHIP.PAL", DSurface::Hidden);
		Debug::Log("[DropshipLoadout] LoadAssets - DROPSHIP.PAL loaded: %p\n", dropshipLoadout_Palette);
	}

	Debug::Log("[DropshipLoadout] LoadAssets - Loading Background PCX...\n");
	if (pHouseTypeExt->DropshipLoadout_BackgroundPCX.isset() && pHouseTypeExt->DropshipLoadout_BackgroundPCX.Get().Exists())
	{
		Debug::Log("[DropshipLoadout] LoadAssets - Background PCX from HouseType\n");
		dropshipLoadout_BackgroundPCX = pHouseTypeExt->DropshipLoadout_BackgroundPCX.Get().GetSurface();
	}
	else if (pGlobal && pGlobal->DropshipLoadout_BackgroundPCX.Exists())
	{
		Debug::Log("[DropshipLoadout] LoadAssets - Background PCX from Global\n");
		dropshipLoadout_BackgroundPCX = pGlobal->DropshipLoadout_BackgroundPCX.GetSurface();
	}
	Debug::Log("[DropshipLoadout] LoadAssets - Background PCX surface: %p\n", dropshipLoadout_BackgroundPCX);

	Debug::Log("[DropshipLoadout] LoadAssets - Loading Background SHP...\n");
	if (pGlobal && pGlobal->DropshipLoadout_Background)
	{
		dropshipLoadout_Background = pGlobal->DropshipLoadout_Background;
		Debug::Log("[DropshipLoadout] LoadAssets - Background SHP from Global: %p\n", dropshipLoadout_Background);
	}
	else
	{
		char tempFilenameBuffer[32];
		_snprintf_s(tempFilenameBuffer, sizeof(tempFilenameBuffer), "DROP%04d.SHP", nStartingDropships);
		Debug::Log("[DropshipLoadout] LoadAssets - Loading file: %s\n", tempFilenameBuffer);
		dropshipLoadout_Background = FileSystem::LoadSHPFile(_strdup(tempFilenameBuffer));
		Debug::Log("[DropshipLoadout] LoadAssets - Background SHP loaded: %p\n", dropshipLoadout_Background);
	}

	Debug::Log("[DropshipLoadout] LoadAssets - Loading Loadout PCX...\n");
	if (pHouseTypeExt->DropshipLoadout_LoadoutPCX.size() > 0)
	{
		Debug::Log("[DropshipLoadout] LoadAssets - Loading Loadout PCX from HouseType, size: %d\n", (int)pHouseTypeExt->DropshipLoadout_LoadoutPCX.size());
		for (auto& pFilePCX : pHouseTypeExt->DropshipLoadout_LoadoutPCX)
		{
			dropshipLoadout_LoadoutPCX.push_back(pFilePCX.GetSurface());
		}
	}
	else if (pGlobal && pGlobal->DropshipLoadout_LoadoutPCX.size() > 0)
	{
		Debug::Log("[DropshipLoadout] LoadAssets - Loading Loadout PCX from Global, size: %d\n", (int)pGlobal->DropshipLoadout_LoadoutPCX.size());
		for (auto &pFilePCX : pGlobal->DropshipLoadout_LoadoutPCX)
		{
			dropshipLoadout_LoadoutPCX.push_back(pFilePCX.GetSurface());
		}
	}
	Debug::Log("[DropshipLoadout] LoadAssets - Loadout PCX list loaded size: %d\n", (int)dropshipLoadout_LoadoutPCX.size());

	Debug::Log("[DropshipLoadout] LoadAssets - Loading Loadout SHP...\n");
	if (pGlobal && pGlobal->DropshipLoadout_Loadout)
	{
		dropshipLoadout_Loadout = pGlobal->DropshipLoadout_Loadout;
		Debug::Log("[DropshipLoadout] LoadAssets - Loadout SHP from Global: %p\n", dropshipLoadout_Loadout);
	}
	else
	{
		dropshipLoadout_Loadout = FileSystem::LoadSHPFile("LOADOUT.SHP");
		Debug::Log("[DropshipLoadout] LoadAssets - Loadout SHP loaded: %p\n", dropshipLoadout_Loadout);
	}

	Debug::Log("[DropshipLoadout] LoadAssets - Loading PilotLit PCX...\n");
	if (!pHouseTypeExt->DropshipLoadout_PilotLitPCX.empty())
	{
		Debug::Log("[DropshipLoadout] LoadAssets - Loading PilotLit PCX from HouseType, size: %d\n", (int)pHouseTypeExt->DropshipLoadout_PilotLitPCX.size());
		for (const PhobosPCXFile& frame : pHouseTypeExt->DropshipLoadout_PilotLitPCX)
		{
			dropshipLoadout_PilotLitPCX.push_back(frame.GetSurface());
		}
	}
	else if (pGlobal && !pGlobal->DropshipLoadout_PilotLitPCX.empty())
	{
		Debug::Log("[DropshipLoadout] LoadAssets - Loading PilotLit PCX from Global, size: %d\n", (int)pGlobal->DropshipLoadout_PilotLitPCX.size());
		for (auto &pFilePCX : pGlobal->DropshipLoadout_PilotLitPCX)
		{
			dropshipLoadout_PilotLitPCX.push_back(pFilePCX.GetSurface());
		}
	}
	Debug::Log("[DropshipLoadout] LoadAssets - PilotLit PCX list loaded size: %d\n", (int)dropshipLoadout_PilotLitPCX.size());

	Debug::Log("[DropshipLoadout] LoadAssets - Loading PilotLit SHP...\n");
	if (pGlobal && pGlobal->DropshipLoadout_PilotLit)
	{
		dropshipLoadout_PilotLit = pGlobal->DropshipLoadout_PilotLit;
		Debug::Log("[DropshipLoadout] LoadAssets - PilotLit SHP from Global: %p\n", dropshipLoadout_PilotLit);
	}
	else
	{
		dropshipLoadout_PilotLit = FileSystem::LoadSHPFile("PILOTLIT.SHP");
		Debug::Log("[DropshipLoadout] LoadAssets - PilotLit SHP loaded: %p\n", dropshipLoadout_PilotLit);
	}

	Debug::Log("[DropshipLoadout] LoadAssets - Loading UpArrow PCX...\n");
	if (pHouseTypeExt->DropshipLoadout_UpArrowPCX.isset() && pHouseTypeExt->DropshipLoadout_UpArrowPCX.Get().Exists())
	{
		dropshipLoadout_UpArrowPCX = pHouseTypeExt->DropshipLoadout_UpArrowPCX.Get().GetSurface();
	}
	else if (pGlobal && pGlobal->DropshipLoadout_UpArrowPCX.Exists())
	{
		dropshipLoadout_UpArrowPCX = pGlobal->DropshipLoadout_UpArrowPCX.GetSurface();
	}
	Debug::Log("[DropshipLoadout] LoadAssets - UpArrow PCX: %p\n", dropshipLoadout_UpArrowPCX);

	Debug::Log("[DropshipLoadout] LoadAssets - Loading UpArrow SHP...\n");
	if (pGlobal && pGlobal->DropshipLoadout_UpArrow)
	{
		dropshipLoadout_UpArrow = pGlobal->DropshipLoadout_UpArrow;
	}
	else
	{
		dropshipLoadout_UpArrow = FileSystem::LoadSHPFile("DROPUP.SHP");
	}
	Debug::Log("[DropshipLoadout] LoadAssets - UpArrow SHP: %p\n", dropshipLoadout_UpArrow);

	Debug::Log("[DropshipLoadout] LoadAssets - Loading DownArrow PCX...\n");
	if (pHouseTypeExt->DropshipLoadout_DownArrowPCX.isset() && pHouseTypeExt->DropshipLoadout_DownArrowPCX.Get().Exists())
	{
		dropshipLoadout_DownArrowPCX = pHouseTypeExt->DropshipLoadout_DownArrowPCX.Get().GetSurface();
	}
	else if (pGlobal && pGlobal->DropshipLoadout_DownArrowPCX.Exists())
	{
		dropshipLoadout_DownArrowPCX = pGlobal->DropshipLoadout_DownArrowPCX.GetSurface();
	}
	Debug::Log("[DropshipLoadout] LoadAssets - DownArrow PCX: %p\n", dropshipLoadout_DownArrowPCX);

	Debug::Log("[DropshipLoadout] LoadAssets - Loading DownArrow SHP...\n");
	if (pGlobal && pGlobal->DropshipLoadout_DownArrow)
	{
		dropshipLoadout_DownArrow = pGlobal->DropshipLoadout_DownArrow;
	}
	else
	{
		dropshipLoadout_DownArrow = FileSystem::LoadSHPFile("DROPDOWN.SHP");
	}
	Debug::Log("[DropshipLoadout] LoadAssets - DownArrow SHP: %p\n", dropshipLoadout_DownArrow);

	Debug::Log("[DropshipLoadout] LoadAssets - Loading DGreen list PCX...\n");
	if (pHouseTypeExt->DropshipLoadout_DGreenListPCX.size() > 0)
	{
		Debug::Log("[DropshipLoadout] LoadAssets - Loading DGreen PCX from HouseType, size: %d\n", (int)pHouseTypeExt->DropshipLoadout_DGreenListPCX.size());
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
		Debug::Log("[DropshipLoadout] LoadAssets - Loading DGreen PCX from Global, size: %d\n", (int)pGlobal->DropshipLoadout_DGreenListPCX.size());
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
	Debug::Log("[DropshipLoadout] LoadAssets - DGreen PCX loaded size: %d\n", (int)dropshipLoadout_DGreenListPCX.size());

	Debug::Log("[DropshipLoadout] LoadAssets - Loading DGreen SHP list...\n");
	for (int i = 0; i < 4; i++)
	{
		if (pGlobal && (pGlobal->DropshipLoadout_DGreenList.size() < 4 || pGlobal->DropshipLoadout_DGreenList[i] == nullptr))
		{
			Debug::Log("[DropshipLoadout] LoadAssets - Loading DGREEN%d.SHP file\n", i+1);
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
			Debug::Log("[DropshipLoadout] LoadAssets - DGreen SHP %d from Global\n", i);
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
	Debug::Log("[DropshipLoadout] LoadAssets - DGreen SHP list size: %d\n", (int)dropshipLoadout_DGreenList.size());

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

	Debug::Log("[DropshipLoadout] LoadAssets - Sounds loaded: buy=%d, sell=%d, arrows=%d\n", buyClickSoundIdx, sellClickSoundIdx, arrowsClickSoundIdx);

	long dropshipLoadout_InitialMoney = pHouseTypeExt->DropshipLoadout_Money.isset() ? pHouseTypeExt->DropshipLoadout_Money : (pGlobal ? pGlobal->DropshipLoadout_Money : -1);
	dropshipLoadout_InitialMoney = dropshipLoadout_InitialMoney >= 0 ? dropshipLoadout_InitialMoney : HouseClass::CurrentPlayer->Available_Money();

	initialMoney = dropshipLoadout_InitialMoney;
	currentMoney = dropshipLoadout_InitialMoney;
	Debug::Log("[DropshipLoadout] LoadAssets - Money: initial=%ld, current=%ld\n", initialMoney, currentMoney);

	std::vector<TechnoTypeClass*> allowableUnits;

	if (pHouseTypeExt->DropshipLoadout_AllowableUnits.size() > 0)
	{
		Debug::Log("[DropshipLoadout] LoadAssets - Reading allowable units from HouseType: size=%d\n", (int)pHouseTypeExt->DropshipLoadout_AllowableUnits.size());
		for (auto pUnit : pHouseTypeExt->DropshipLoadout_AllowableUnits)
		{
			allowableUnits.push_back(pUnit);
		}
	}
	else
	{
		Debug::Log("[DropshipLoadout] LoadAssets - Reading allowable units from ScenarioClass: size=%d\n", (int)ScenarioClass::Instance->AllowableUnits.Count);
		for (auto pUnit : ScenarioClass::Instance->AllowableUnits)
		{
			allowableUnits.push_back(pUnit);
		}
	}

	std::vector<int> allowableUnitMaximums;

	if (pHouseTypeExt->DropshipLoadout_AllowableUnitMaximums.size() > 0)
	{
		Debug::Log("[DropshipLoadout] LoadAssets - Reading unit maximums from HouseType: size=%d\n", (int)pHouseTypeExt->DropshipLoadout_AllowableUnitMaximums.size());
		for (int pUnitCount : pHouseTypeExt->DropshipLoadout_AllowableUnitMaximums)
		{
			allowableUnitMaximums.push_back(pUnitCount);
		}
	}
	else
	{
		Debug::Log("[DropshipLoadout] LoadAssets - Reading unit maximums from ScenarioClass: size=%d\n", (int)ScenarioClass::Instance->AllowableUnitMaximums.Count);
		for (int pUnitCount : ScenarioClass::Instance->AllowableUnitMaximums)
		{
			allowableUnitMaximums.push_back(pUnitCount);
		}
	}

	if (allowableUnits.size() > 0)
	{
		if (allowableUnitMaximums.size() > 0 && allowableUnits.size() != allowableUnitMaximums.size())
		{
			Debug::Log("[DropshipLoadout] LoadAssets - WARNING: AllowableUnits (%d) and AllowableUnitMaximums (%d) count mismatch! Disabling units limit.\n", (int)allowableUnits.size(), (int)allowableUnitMaximums.size());
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
		Debug::Log("[DropshipLoadout] LoadAssets - No allowable units specified, scanning all infantry/units...\n");
		for (const auto pType : TechnoTypeClass::Array)
		{
			if (pType && (pType->WhatAmI() == AbstractType::InfantryType || pType->WhatAmI() == AbstractType::UnitType))
				availableUnits.push_back(pType);
		}
	}
	Debug::Log("[DropshipLoadout] LoadAssets - availableUnits size: %d. End\n", (int)availableUnits.size());
}

void DropshipLoadoutClass::CalculateLayout(DSurface* pSurface)
{
	Debug::Log("[DropshipLoadout] CalculateLayout - Start\n");
	if (!pSurface)
	{
		Debug::Log("[DropshipLoadout] CalculateLayout - Error: pSurface is null!\n");
		return;
	}

	const int cameoWidth = 60, cameoHeight = 48;
	int backgroundWidth = 0;
	int backgroundHeight = 0;

	if (dropshipLoadout_BackgroundPCX)
	{
		backgroundWidth = dropshipLoadout_BackgroundPCX->Width;
		backgroundHeight = dropshipLoadout_BackgroundPCX->Height;
		Debug::Log("[DropshipLoadout] CalculateLayout - Background from PCX: W=%d, H=%d\n", backgroundWidth, backgroundHeight);
	}
	else if (dropshipLoadout_Background)
	{
		backgroundWidth = dropshipLoadout_Background->Width;
		backgroundHeight = dropshipLoadout_Background->Height;
		Debug::Log("[DropshipLoadout] CalculateLayout - Background from SHP: W=%d, H=%d\n", backgroundWidth, backgroundHeight);
	}
	else
	{
		Debug::Log("[DropshipLoadout] CalculateLayout - Error: No background asset loaded!\n");
		backgroundWidth = 640; // Fallback
		backgroundHeight = 480;
	}

	int backgroundX = (pSurface->GetWidth() - backgroundWidth) / 2;
	int backgroundY = (pSurface->GetHeight() - backgroundHeight) / 2;
	windowRectangle = { backgroundX, backgroundY, backgroundWidth, backgroundHeight };
	Debug::Log("[DropshipLoadout] CalculateLayout - windowRectangle: X=%d, Y=%d, W=%d, H=%d\n", windowRectangle.X, windowRectangle.Y, windowRectangle.Width, windowRectangle.Height);

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
		Debug::Log("[DropshipLoadout] CalculateLayout - SidebarCameos from HouseType: count=%d\n", nSidebarCameos);
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
		Debug::Log("[DropshipLoadout] CalculateLayout - SidebarCameos from Global: count=%d\n", nSidebarCameos);
		for (int i = 0; i < nSidebarCameos; ++i)
		{
			int cameoX = backgroundX + pGlobal->DropshipLoadout_SidebarCameoLocations[i].X;
			int cameoY = backgroundY + pGlobal->DropshipLoadout_SidebarCameoLocations[i].Y;
			sidebarCameLocations.push_back({ cameoX, cameoY, cameoWidth, cameoHeight });
		}
	}
	else
	{
		Debug::Log("[DropshipLoadout] CalculateLayout - SidebarCameos default layout: count=%d\n", nSidebarCameos);
		for (int i = 0; i < nSidebarCameos; ++i)
		{
			int cameoX = backgroundX + 493 + 68 * (i % 2);
			int cameoY = backgroundY + 25 + 50 * (i / 2);
			sidebarCameLocations.push_back({ cameoX, cameoY, cameoWidth, cameoHeight });
		}
	}
	Debug::Log("[DropshipLoadout] CalculateLayout - sidebarCameLocations populated size: %d\n", (int)sidebarCameLocations.size());

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
	Debug::Log("[DropshipLoadout] CalculateLayout - upArrowLocation: X=%d, Y=%d, W=%d, H=%d\n", upArrowLocation.X, upArrowLocation.Y, upArrowLocation.Width, upArrowLocation.Height);

	int dropshipLoadout_DownArrowWidth = dropshipLoadout_DownArrowPCX ? dropshipLoadout_DownArrowPCX->Width : (dropshipLoadout_DownArrow ? dropshipLoadout_DownArrow->Width : 30);
	int dropshipLoadout_DownArrowHeight = dropshipLoadout_DownArrowPCX ? dropshipLoadout_DownArrowPCX->Height : (dropshipLoadout_DownArrow ? dropshipLoadout_DownArrow->Height : 30);
	downArrowX = customDownArrowLocation != Point2D::Empty ? customDownArrowLocation.X : centerOfCameoColumns;
	downArrowY = customDownArrowLocation != Point2D::Empty ? customDownArrowLocation.Y : arrowsY;
	downArrowLocation = { backgroundX + downArrowX, backgroundY + downArrowY, dropshipLoadout_DownArrowWidth, dropshipLoadout_DownArrowHeight };
	Debug::Log("[DropshipLoadout] CalculateLayout - downArrowLocation: X=%d, Y=%d, W=%d, H=%d\n", downArrowLocation.X, downArrowLocation.Y, downArrowLocation.Width, downArrowLocation.Height);

	dGreenLocation.clear();

	if (pHouseTypeExt->DropshipLoadout_DGreenAnimationsCount.isset())
	{
		Debug::Log("[DropshipLoadout] CalculateLayout - DGreen animations from HouseType: count=%d\n", (int)pHouseTypeExt->DropshipLoadout_DGreenAnimationsCount);
		for (int i = 0; i < pHouseTypeExt->DropshipLoadout_DGreenAnimationsCount; i++)
		{
			Point2D location = pHouseTypeExt->DropshipLoadout_DGreenLocations[i];
			dGreenLocation.push_back({ backgroundX + location.X, backgroundY + location.Y, 0, 0 });
		}
	}
	else if (pGlobal && pGlobal->DropshipLoadout_DGreenAnimationsCount)
	{
		Debug::Log("[DropshipLoadout] CalculateLayout - DGreen animations from Global: count=%d\n", (int)pGlobal->DropshipLoadout_DGreenAnimationsCount);
		for (int i = 0; i < pGlobal->DropshipLoadout_DGreenAnimationsCount; i++)
		{
			Point2D location = pGlobal->DropshipLoadout_DGreenLocations[i];
			dGreenLocation.push_back({ backgroundX + location.X, backgroundY + location.Y, 0, 0 });
		}
	}
	else
	{
		Debug::Log("[DropshipLoadout] CalculateLayout - DGreen default locations\n");
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
	Debug::Log("[DropshipLoadout] CalculateLayout - dGreenLocation populated size: %d\n", (int)dGreenLocation.size());

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
	Debug::Log("[DropshipLoadout] CalculateLayout - loadoutLocation: X=%d, Y=%d, W=%d, H=%d\n", loadoutLocation.X, loadoutLocation.Y, loadoutLocation.Width, loadoutLocation.Height);

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
	Debug::Log("[DropshipLoadout] CalculateLayout - pilotLitLocation: X=%d, Y=%d, W=%d, H=%d\n", pilotLitLocation.X, pilotLitLocation.Y, pilotLitLocation.Width, pilotLitLocation.Height);

	nDropshipBayCameos = 5;
	dropshipBayCameLocations.clear();

	if (pHouseTypeExt->DropshipLoadout_DropshipCameosCount.Get(0) > 0)
	{
		nDropshipBayCameos = pHouseTypeExt->DropshipLoadout_DropshipCameosCount;
		Debug::Log("[DropshipLoadout] CalculateLayout - Dropship cameos from HouseType: count=%d\n", nDropshipBayCameos);
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
		Debug::Log("[DropshipLoadout] CalculateLayout - Dropship cameos from Global: count=%d\n", nDropshipBayCameos);
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
		Debug::Log("[DropshipLoadout] CalculateLayout - Dropship cameos default layout: nStartingDropships=%d\n", nStartingDropships);
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
			Debug::Log("[DropshipLoadout] CalculateLayout - WARNING: dropshipBayCameLocations size (%d) is less than nStartingDropships (%d). Generating generic entries for dropships 4+...\n", (int)dropshipBayCameLocations.size(), nStartingDropships);
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
	Debug::Log("[DropshipLoadout] CalculateLayout - dropshipBayCameLocations size: %d\n", (int)dropshipBayCameLocations.size());

	nDropshipBayTotalSlots = nStartingDropships * nDropshipBayCameos;
	Debug::Log("[DropshipLoadout] CalculateLayout - nDropshipBayTotalSlots: %d. End\n", nDropshipBayTotalSlots);
}

void DropshipLoadoutClass::CreateControls()
{
	Debug::Log("[DropshipLoadout] CreateControls - Start\n");
	const int cameoWidth = 60, cameoHeight = 48;
	buttonsList.clear();

	int btn_ScrollUp_ID = 100;
	Debug::Log("[DropshipLoadout] CreateControls - Creating ScrollUp Button...\n");
	ShapeButtonClass* btn_ScrollUp = CreateShapeButton(
		btn_ScrollUp_ID,
		0, 0,
		upArrowLocation.Width, upArrowLocation.Height,
		true
	);
	Debug::Log("[DropshipLoadout] CreateControls - ScrollUp Button created: %p\n", btn_ScrollUp);
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
		Debug::Log("[DropshipLoadout] CreateControls - CRITICAL: btn_ScrollUp is null!\n");
	}

	int btn_ScrollDown_ID = 101;
	Debug::Log("[DropshipLoadout] CreateControls - Creating ScrollDown Button...\n");
	ShapeButtonClass* btn_ScrollDown = CreateShapeButton(
		btn_ScrollDown_ID,
		0, 0,
		downArrowLocation.Width, downArrowLocation.Height,
		true
	);
	Debug::Log("[DropshipLoadout] CreateControls - ScrollDown Button created: %p\n", btn_ScrollDown);
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
		Debug::Log("[DropshipLoadout] CreateControls - CRITICAL: btn_ScrollDown is null!\n");
	}

	int btn_BasicDropshipCameo_ID = 200;
	int newID = btn_BasicDropshipCameo_ID;
	dropshipBayChosenUnitsLists.clear();

	Debug::Log("[DropshipLoadout] CreateControls - Creating Dropship Cameo Buttons...\n");
	for (int i = 0; i < nStartingDropships; i++)
	{
		dropshipBayChosenUnitsLists.push_back(std::vector<TechnoTypeClass*>());
		if (i >= (int)dropshipBayCameLocations.size())
		{
			Debug::Log("[DropshipLoadout] CreateControls - WARNING: dropship index %d >= dropshipBayCameLocations size %d! Skipping button creation for this dropship.\n", i, (int)dropshipBayCameLocations.size());
			continue;
		}

		for (int j = 0; j < nDropshipBayCameos; j++)
		{
			if (j >= (int)dropshipBayCameLocations[i].size())
			{
				Debug::Log("[DropshipLoadout] CreateControls - WARNING: cameo index %d >= dropshipBayCameLocations[%d] size %d! Skipping.\n", j, i, (int)dropshipBayCameLocations[i].size());
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
				Debug::Log("[DropshipLoadout] CreateControls - WARNING: Failed to create button for dropship %d, cameo %d (ID %d)\n", i, j, newID);
			}
			dropshipBayChosenUnitsLists[i].push_back(nullptr);
			newID++;
		}
	}
	Debug::Log("[DropshipLoadout] CreateControls - Created dropship bay buttons. nStartingDropships=%d\n", nStartingDropships);

	int btn_BasicSidebarCameo_ID = 300;
	Debug::Log("[DropshipLoadout] CreateControls - Creating Sidebar Cameo Buttons...\n");
	for (int i = 0; i < nSidebarCameos; i++)
	{
		if (i >= (int)sidebarCameLocations.size())
		{
			Debug::Log("[DropshipLoadout] CreateControls - WARNING: sidebar index %d >= sidebarCameLocations size %d! Skipping.\n", i, (int)sidebarCameLocations.size());
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
			Debug::Log("[DropshipLoadout] CreateControls - WARNING: Failed to create button for sidebar %d (ID %d)\n", i, sID);
		}
	}
	Debug::Log("[DropshipLoadout] CreateControls - created %d buttons. End\n", (int)buttonsList.size());
}

void DropshipLoadoutClass::Run()
{
	Debug::Log("[DropshipLoadout] Run - Start\n");
	DSurface* pSurface = DSurface::Hidden;
	Debug::Log("[DropshipLoadout] Run - pSurface (DSurface::Hidden) is %p\n", pSurface);
	if (!pSurface)
	{
		Debug::Log("[DropshipLoadout] Run - ERROR: pSurface is null! Cannot run loadout screen.\n");
		return;
	}

	Debug::Log("[DropshipLoadout] Run - Clearing surface...\n");
	pSurface->Fill(0);

	Debug::Log("[DropshipLoadout] Run - Calculating layout...\n");
	CalculateLayout(pSurface);
	Debug::Log("[DropshipLoadout] Run - Creating controls...\n");
	CreateControls();

	const int voiceEva = pHouseTypeExt->DropshipLoadout_StartEVA.isset() ? pHouseTypeExt->DropshipLoadout_StartEVA.Get(-1) : (ScenarioExt::Global() ? ScenarioExt::Global()->DropshipLoadout_StartEVA.Get(-1) : -1);
	Debug::Log("[DropshipLoadout] Run - voiceEva: %d\n", voiceEva);
	if (voiceEva >= 0)
	{
		Debug::Log("[DropshipLoadout] Run - Playing EVA sound index %d\n", voiceEva);
		VoxClass::PlayIndex(voiceEva);
	}

	const int themeIdx = pHouseTypeExt->DropshipLoadout_Theme.isset() ? pHouseTypeExt->DropshipLoadout_Theme : (ScenarioExt::Global() ? ScenarioExt::Global()->DropshipLoadout_Theme : -1);
	Debug::Log("[DropshipLoadout] Run - themeIdx: %d\n", themeIdx);
	if (themeIdx == -1)
	{
		Debug::Log("[DropshipLoadout] Run - Stopping music theme\n");
		ThemeClass::Instance.Stop(true);
	}
	else
	{
		Debug::Log("[DropshipLoadout] Run - Playing music theme index %d\n", themeIdx);
		ThemeClass::Instance.Play(themeIdx);
	}

	Debug::Log("[DropshipLoadout] Run - Configuring mouse...\n");
	if (WWMouseClass::Instance)
	{
		WWMouseClass::Instance->HideCursor();
		WWMouseClass::Instance->ShowCursor();
		WWMouseClass::Instance->CaptureMouse();
		WWMouseClass::Instance->RefCount = 0;
	}
	else
	{
		Debug::Log("[DropshipLoadout] Run - WARNING: WWMouseClass::Instance is null!\n");
	}

	if (commandManager)
	{
		Debug::Log("[DropshipLoadout] Run - Turning on commandManager at %p\n", commandManager);
		commandManager->TurnOn();
	}
	else
	{
		Debug::Log("[DropshipLoadout] Run - ERROR: commandManager is null!\n");
	}

	Debug::Log("[DropshipLoadout] Run - Calculating total animation frames...\n");
	loadoutTotalFrames = dropshipLoadout_LoadoutPCX.size() > 0 ? (int)dropshipLoadout_LoadoutPCX.size() - 1 : (dropshipLoadout_Loadout ? dropshipLoadout_Loadout->Frames : 0);
	pilotLitTotalFrames = dropshipLoadout_PilotLitPCX.size() > 0 ? (int)dropshipLoadout_PilotLitPCX.size() - 1 : (dropshipLoadout_PilotLit ? dropshipLoadout_PilotLit->Frames : 0);
	Debug::Log("[DropshipLoadout] Run - Animation frames: loadout=%d, pilotLit=%d\n", loadoutTotalFrames, pilotLitTotalFrames);

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

	Debug::Log("[DropshipLoadout] Run - Entering main loop\n");
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

	Debug::Log("[DropshipLoadout] Run - Exited main loop. Saving Cargo...\n");
	SaveCargo();
	Debug::Log("[DropshipLoadout] Run - End\n");
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
		Debug::Log("[DropshipLoadout] HandleInput - command=%d, buttonID=%d, leftClick=%d, rightClick=%d\n", command, buttonID, pressedLeftClick ? 1 : 0, pressedRightClick ? 1 : 0);
	}

	bool validSidebarCameoPurchase = false;
	freeDropshipSlots = false;
	Point2D mouseLocationInDropshipCameos = { 0, 0 };

	for (int i = 0; i < (int)dropshipBayCameLocations.size() && !freeDropshipSlots; i++)
	{
		if (i >= (int)dropshipBayChosenUnitsLists.size())
		{
			Debug::Log("[DropshipLoadout] HandleInput - Error: i (%d) >= dropshipBayChosenUnitsLists.size() (%d) during free slot check\n", i, (int)dropshipBayChosenUnitsLists.size());
			continue;
		}

		for (int j = 0; j < (int)dropshipBayCameLocations[i].size() && !freeDropshipSlots; j++)
		{
			if (j >= (int)dropshipBayChosenUnitsLists[i].size())
			{
				Debug::Log("[DropshipLoadout] HandleInput - Error: j (%d) >= dropshipBayChosenUnitsLists[%d].size() (%d) during free slot check\n", j, i, (int)dropshipBayChosenUnitsLists[i].size());
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
		Debug::Log("[DropshipLoadout] HandleInput - Pressed Up Arrow. firstBrowsableCameo = %d\n", firstBrowsableCameo);
		if (firstBrowsableCameo >= 2)
		{
			firstBrowsableCameo -= 2;
			repaintAll = true;
			VocClass::PlayGlobal(arrowsClickSoundIdx, 0x2000, 1.0);
		}
	}
	else if (pressedDownArrow)
	{
		Debug::Log("[DropshipLoadout] HandleInput - Pressed Down Arrow. firstBrowsableCameo = %d, availableUnits = %d\n", firstBrowsableCameo, (int)availableUnits.size());
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
		Debug::Log("[DropshipLoadout] HandleInput - Right Clicked Sidebar Cameo. Index = %d\n", newIndex);
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
							Debug::Log("[DropshipLoadout] HandleInput - Selling unit %s from dropship %d, slot %d\n", pType->ID, i, j);
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
		Debug::Log("[DropshipLoadout] HandleInput - Pressed Sidebar Cameo. Index = %d\n", newIndex);
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
							Debug::Log("[DropshipLoadout] HandleInput - Error: i (%d) >= dropshipBayChosenUnitsLists.size() (%d) during purchase\n", i, (int)dropshipBayChosenUnitsLists.size());
							continue;
						}

						for (int j = 0; j < (int)dropshipBayCameLocations[i].size() && !foundFreeSlot; j++)
						{
							if (j >= (int)dropshipBayChosenUnitsLists[i].size())
							{
								Debug::Log("[DropshipLoadout] HandleInput - Error: j (%d) >= dropshipBayChosenUnitsLists[%d].size() (%d) during purchase\n", j, i, (int)dropshipBayChosenUnitsLists[i].size());
								continue;
							}

							auto const pDropshipSlotType = dropshipBayChosenUnitsLists[i][j];
							if (pDropshipSlotType)
								continue;

							Debug::Log("[DropshipLoadout] HandleInput - Purchasing unit %s in dropship %d, slot %d\n", pType->ID, i, j);
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
						Debug::Log("[DropshipLoadout] HandleInput - Starting sidebar animation row index: %d\n", sidebarRowAnimationIndex);
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
				Debug::Log("[DropshipLoadout] HandleInput - Purchase not allowed (money limit, count limit, or slots full)\n");
			}
		}
	}
	else if (pressedAnyDropshipCameo)
	{
		if (nDropshipBayCameos > 0)
		{
			int nDropship = (command - btn_BasicDropshipCameo_ID) / nDropshipBayCameos;
			int index = command - btn_BasicDropshipCameo_ID - (nDropship * nDropshipBayCameos);
			Debug::Log("[DropshipLoadout] HandleInput - Pressed Dropship Cameo. Dropship: %d, Index: %d\n", nDropship, index);

			if (nDropship >= 0 && nDropship < (int)dropshipBayChosenUnitsLists.size())
			{
				if (index >= 0 && index < (int)dropshipBayChosenUnitsLists[nDropship].size())
				{
					auto pType = dropshipBayChosenUnitsLists[nDropship][index];
					if (pType)
					{
						Debug::Log("[DropshipLoadout] HandleInput - Selling unit %s from dropship %d, slot %d via click\n", pType->ID, nDropship, index);
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
		Debug::Log("[DropshipLoadout] HandleInput - Pressed SPACE. Exiting loadout...\n");
		pressedSpaceKey = true;
	}

	if (command == VK_ESCAPE)
	{
		Debug::Log("[DropshipLoadout] HandleInput - Pressed ESCAPE. Resetting choices...\n");
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
		Debug::Log("[DropshipLoadout] Render - Error: pSurface is null!\n");
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
			Debug::Log("[DropshipLoadout] Render - WARNING: i (%d) >= sidebarCameLocations size (%d)\n", i, (int)sidebarCameLocations.size());
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
			Debug::Log("[DropshipLoadout] Render - WARNING: No TechnoTypeExt for %s!\n", pType->ID);
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
			Debug::Log("[DropshipLoadout] Render - WARNING: dropship index %d >= dropshipBayChosenUnitsLists.size() (%d)\n", (int)i, (int)dropshipBayChosenUnitsLists.size());
			continue;
		}

		for (size_t j = 0; j < dropshipBayCameLocations[i].size(); j++)
		{
			if (j >= dropshipBayChosenUnitsLists[i].size())
			{
				Debug::Log("[DropshipLoadout] Render - WARNING: slot index %d >= dropshipBayChosenUnitsLists[%d].size() (%d)\n", (int)j, (int)i, (int)dropshipBayChosenUnitsLists[i].size());
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
				Debug::Log("[DropshipLoadout] Render - WARNING: No TechnoTypeExt for %s inside dropship slot!\n", pType->ID);
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
				Debug::Log("[DropshipLoadout] Render - Error: currentLoadoutFrame (%d) >= loadoutPCX size (%d)\n", currentLoadoutFrame, (int)dropshipLoadout_LoadoutPCX.size());
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
				Debug::Log("[DropshipLoadout] Render - Error: currentPilotLitFrame (%d) >= pilotLitPCX size (%d)\n", currentPilotLitFrame, (int)dropshipLoadout_PilotLitPCX.size());
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
						Debug::Log("[DropshipLoadout] Render - Error: currentSidebarRowAnimationFrame (%d) >= DGreenListPCX[%d] size (%d)\n", currentSidebarRowAnimationFrame, sidebarRowAnimationIndex, (int)dropshipLoadout_DGreenListPCX[sidebarRowAnimationIndex].size());
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
				Debug::Log("[DropshipLoadout] Render - Error: sidebarRowAnimationIndex (%d) >= DGreenList size (%d)\n", sidebarRowAnimationIndex, (int)dropshipLoadout_DGreenList.size());
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
			Debug::Log("[DropshipLoadout] Render - Error: sidebarRowAnimationIndex (%d) >= dGreenLocation size (%d)\n", sidebarRowAnimationIndex, (int)dGreenLocation.size());
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
	Debug::Log("[DropshipLoadout] SaveCargo - Start\n");
	if (!HouseClass::CurrentPlayer)
	{
		Debug::Log("[DropshipLoadout] SaveCargo - Error: CurrentPlayer is null!\n");
		return;
	}

	auto pHouseExt = HouseExt::ExtMap.Find(HouseClass::CurrentPlayer);
	if (!pHouseExt)
	{
		Debug::Log("[DropshipLoadout] SaveCargo - Error: pHouseExt is null!\n");
		return;
	}

	pHouseExt->DropshipLoadout_Cargo.clear();
	pHouseExt->DropshipLoadout_Carriers.clear();

	std::vector<TechnoTypeClass*> carriers;

	if (pHouseTypeExt->DropshipLoadout_Carriers.size() > 0)
	{
		Debug::Log("[DropshipLoadout] SaveCargo - Carriers from HouseType, size: %d\n", (int)pHouseTypeExt->DropshipLoadout_Carriers.size());
		for (auto carrier : pHouseTypeExt->DropshipLoadout_Carriers)
		{
			carriers.push_back(carrier);
		}
	}
	else if (ScenarioExt::Global())
	{
		Debug::Log("[DropshipLoadout] SaveCargo - Carriers from Global, size: %d\n", (int)ScenarioExt::Global()->DropshipLoadout_Carriers.size());
		for (auto carrier : ScenarioExt::Global()->DropshipLoadout_Carriers)
		{
			carriers.push_back(carrier);
		}
	}

	int nCarriers = (int)carriers.size();
	Debug::Log("[DropshipLoadout] SaveCargo - Saving cargo for %d starting dropships, nCarriers=%d\n", nStartingDropships, nCarriers);

	for (int i = 0; i < nStartingDropships && i < nCarriers; i++)
	{
		pHouseExt->DropshipLoadout_Carriers.push_back(carriers[i]);
		std::vector<TechnoTypeClass*> unitsList;

		if (i >= (int)dropshipBayChosenUnitsLists.size())
		{
			Debug::Log("[DropshipLoadout] SaveCargo - Error: i (%d) >= dropshipBayChosenUnitsLists size (%d)!\n", i, (int)dropshipBayChosenUnitsLists.size());
			continue;
		}

		for (auto const pTechno : dropshipBayChosenUnitsLists[i])
		{
			if (pTechno)
			{
				Debug::Log("[DropshipLoadout] SaveCargo - Adding cargo unit %s to dropship %d\n", pTechno->ID, i);
				unitsList.push_back(pTechno);
			}
		}

		pHouseExt->DropshipLoadout_Cargo.push_back(unitsList);
		unitsList.clear();
	}

	bool addUnusedMoneyToPlayer = pHouseTypeExt->DropshipLoadout_AddUnusedMoneyToPlayer.isset() ? pHouseTypeExt->DropshipLoadout_AddUnusedMoneyToPlayer : (ScenarioExt::Global() ? ScenarioExt::Global()->DropshipLoadout_AddUnusedMoneyToPlayer : false);
	Debug::Log("[DropshipLoadout] SaveCargo - addUnusedMoneyToPlayer=%d\n", addUnusedMoneyToPlayer ? 1 : 0);

	if (addUnusedMoneyToPlayer)
	{
		Debug::Log("[DropshipLoadout] SaveCargo - Transacting currentMoney: %d\n", currentMoney);
		HouseClass::CurrentPlayer->TransactMoney(currentMoney);
	}
	else
	{
		long dropshipLoadout_InitialMoney = pHouseTypeExt->DropshipLoadout_Money.isset() ? pHouseTypeExt->DropshipLoadout_Money : (ScenarioExt::Global() ? ScenarioExt::Global()->DropshipLoadout_Money : -1);
		Debug::Log("[DropshipLoadout] SaveCargo - Non-additive: initialMoney setting=%ld\n", dropshipLoadout_InitialMoney);

		if (dropshipLoadout_InitialMoney < 0)
		{
			long spent = HouseClass::CurrentPlayer->Available_Money() - currentMoney;
			Debug::Log("[DropshipLoadout] SaveCargo - Subtracting spent money: %ld\n", spent);
			HouseClass::CurrentPlayer->TransactMoney(-spent);
		}
	}
	Debug::Log("[DropshipLoadout] SaveCargo - End\n");
}

DEFINE_HOOK(0x4B6C30, Dropship_Loadout_Remake, 0x0)
{
	enum { EndFunction = 0x4B9690 };
	Debug::Log("[DropshipLoadout] HOOK Dropship_Loadout_Remake triggered\n");

	if (!HouseClass::CurrentPlayer)
	{
		Debug::Log("[DropshipLoadout] HOOK - Error: CurrentPlayer is null!\n");
		return EndFunction;
	}

	auto const pHouseTypeExt = HouseTypeExt::ExtMap.Find(HouseClass::CurrentPlayer->Type);
	if (!pHouseTypeExt)
	{
		Debug::Log("[DropshipLoadout] HOOK - Error: pHouseTypeExt is null!\n");
		return EndFunction;
	}

	if (!ScenarioClass::Instance)
	{
		Debug::Log("[DropshipLoadout] HOOK - Error: ScenarioClass::Instance is null!\n");
		return EndFunction;
	}

	int nStartingDropships = pHouseTypeExt->DropshipLoadout_StartingDropships.isset() ? pHouseTypeExt->DropshipLoadout_StartingDropships : ScenarioClass::Instance->StartingDropships;
	Debug::Log("[DropshipLoadout] HOOK - nStartingDropships: %d\n", nStartingDropships);

	if (nStartingDropships <= 0)
	{
		Debug::Log("[DropshipLoadout] HOOK - Starting dropships <= 0, returning\n");
		return EndFunction;
	}

	DropshipLoadoutClass loadout;
	Debug::Log("[DropshipLoadout] HOOK - Initializing DropshipLoadoutClass...\n");
	if (loadout.Initialize())
	{
		Debug::Log("[DropshipLoadout] HOOK - Initialization complete. Running loadout screen...\n");
		loadout.Run();
		Debug::Log("[DropshipLoadout] HOOK - Run complete.\n");
	}
	else
	{
		Debug::Log("[DropshipLoadout] HOOK - Initialization failed.\n");
	}

	Debug::Log("[DropshipLoadout] HOOK Dropship_Loadout_Remake finished\n");
	return EndFunction;
}


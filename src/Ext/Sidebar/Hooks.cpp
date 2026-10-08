#include "Body.h"
#include "SWSidebar/SWSidebarClass.h"

#include <Ext/Side/Body.h>
#include <Ext/TechnoType/Body.h>
#include <Ext/SWType/Body.h>
#include <Ext/Observer/ObserverUI.h>
#include <Misc/MessageColumn.h>
#include <Drawing.h>
#include <GScreenClass.h>

DEFINE_HOOK(0x6ABC60, SidebarClass_GetObjectTabIdx, 0x5)
{
	const auto config = SidebarExt::ActiveConfig();
	GET(AbstractType, abs, ECX);
	GET(int, idxType, EDX);

	int desiredTab = 0;

	const auto pTechnoType = TechnoTypeClass::GetByTypeAndIndex(abs, idxType);
	if (pTechnoType)
	{
		if (const auto pExt = TechnoTypeExt::TryFetch(pTechnoType))
		{
			if (pExt->TabIndex.isset())
			{
				desiredTab = pExt->TabIndex.Get();
				R->EAX(SidebarExt::ResolveVisibleTab(desiredTab, abs, config));
				return 0x6ABC9A;
			}
		}

		if (abs == AbstractType::Infantry || abs == AbstractType::InfantryType)
		{
			desiredTab = 2;
		}
		else if (abs == AbstractType::Unit || abs == AbstractType::UnitType ||
			abs == AbstractType::Aircraft || abs == AbstractType::AircraftType)
		{
			desiredTab = 3;
		}
		else if (abs == AbstractType::Building || abs == AbstractType::BuildingType)
		{
			auto pBType = static_cast<BuildingTypeClass*>(pTechnoType);
			desiredTab = (pBType->BuildCat == BuildCat::Combat) ? 1 : 0;
		}
	}
	else if (abs == AbstractType::Super || abs == AbstractType::SuperWeaponType)
	{
		desiredTab = 1;
		if (idxType >= 0 && idxType < SuperWeaponTypeClass::Array.Count)
		{
			if (const auto pSWType = SuperWeaponTypeClass::Array[idxType])
			{
				if (const auto pSWTypExt = SWTypeExt::Fetch(pSWType))
					desiredTab = pSWTypExt->TabIndex;
			}
		}
	}
	else
	{
		if (abs == AbstractType::Infantry || abs == AbstractType::InfantryType)
			desiredTab = 2;
		else if (abs == AbstractType::Unit || abs == AbstractType::UnitType ||
			abs == AbstractType::Aircraft || abs == AbstractType::AircraftType)
			desiredTab = 3;
		else
			desiredTab = 0;
	}

	R->EAX(SidebarExt::ResolveVisibleTab(desiredTab, abs, config));
	return 0x6ABC9A;
}

DEFINE_HOOK(0x6ABCD0, SidebarClass_GetObjectTabIdx2, 0x5)
{
	const auto config = SidebarExt::ActiveConfig();
	GET(AbstractType, abs, ECX);
	GET(BuildCat, buildCat, EDX);

	int desiredTab = 0;
	if (abs == AbstractType::InfantryType || abs == AbstractType::Infantry)
		desiredTab = 2;
	else if (abs == AbstractType::AircraftType || abs == AbstractType::Aircraft ||
		abs == AbstractType::UnitType || abs == AbstractType::Unit)
		desiredTab = 3;
	else if (abs == AbstractType::BuildingType || abs == AbstractType::Building)
		desiredTab = (buildCat == BuildCat::Combat) ? 1 : 0;
	else if (abs == AbstractType::Super || abs == AbstractType::SuperWeaponType)
		desiredTab = 1;

	R->EAX(SidebarExt::ResolveVisibleTab(desiredTab, abs, config));
	return 0x6ABCF4;
}

DEFINE_HOOK(0x6A7590, SidebarClass_SetTab, 0x5)
{
	const auto config = SidebarExt::ActiveConfig();
	GET_STACK(int, tabIndex, 0x4);

	int resolvedTab = SidebarExt::ResolveVisibleTab(tabIndex, AbstractType::None, config);
	R->Stack(0x4, resolvedTab);

	return 0;
}

// -----------------------------------------------------------------------------
// Tab Button Visibility and Position Hooks
// -----------------------------------------------------------------------------

DEFINE_HOOK(0x69DEC8, ShapeButtonClass_Draw_CheckEnabled, 0x5)
{
	GET(ShapeButtonClass*, pThis, ESI);

	if (pThis->X <= -5000 || pThis->Y <= -5000)
		return 0x69DFAD;

	const auto config = SidebarExt::ActiveConfig();
	int tabCount = config.Tabs.Count.Get(4);
	int visibleTabs = (tabCount == 1) ? 0 : tabCount;

	for (int i = 0; i < 4; ++i)
	{
		if (pThis == &SidebarClass::TabButtons[i])
		{
			int slot = SidebarExt::GetTabSlot(i, config);
			if (slot < 0 || slot >= visibleTabs)
				return 0x69DFAD;
			break;
		}
	}

	return 0;
}

DEFINE_HOOK(0x6A5443, SidebarClass_InitGUI_TabButtonPos, 0x6)
{
	const auto config = SidebarExt::ActiveConfig();
	int tabCount = config.Tabs.Count.Get(4);
	int visibleTabs = (tabCount == 1) ? 0 : tabCount;

	GET(int, tabIdx, EBP);
	GET(DWORD, esiVal, ESI);

	int slot = SidebarExt::GetTabSlot(tabIdx, config);

	int posX = R->EDX();
	int posY = R->EAX();

	if (slot < 0 || slot >= visibleTabs)
	{
		posX = -10000;
		posY = -10000;
	}
	else if (static_cast<size_t>(slot) < config.Tabs.Positions.size())
	{
		posX = config.Tabs.Positions[slot].X;
		posY = config.Tabs.Positions[slot].Y;
	}
	else if (!config.Tabs.Order.empty())
	{
		int baseX = *reinterpret_cast<int*>(0x00B0B4E8);
		int tabWidth = *reinterpret_cast<int*>(0x00B0B4F0);
		posX = baseX + slot * tabWidth;
		posY = *reinterpret_cast<int*>(0x00B0B4EC);
	}

	*reinterpret_cast<int*>(esiVal - 0x18) = posX;
	*reinterpret_cast<int*>(esiVal - 0x14) = posY;

	return 0x6A5449;
}

DEFINE_HOOK(0x6A7E36, SidebarClass_Activate_AddTabButton, 0x8)
{
	const auto config = SidebarExt::ActiveConfig();
	int tabCount = config.Tabs.Count.Get(4);
	int visibleTabs = (tabCount == 1) ? 0 : tabCount;

	GET(ShapeButtonClass*, pButton, EDI);
	int tabIdx = (reinterpret_cast<DWORD>(pButton) - 0x00B07C48) / sizeof(ShapeButtonClass);
	int slot = SidebarExt::GetTabSlot(tabIdx, config);

	if (slot >= 0 && slot < visibleTabs)
	{
		GET(GScreenClass*, pGScreen, ESI);
		pGScreen->AddButton(pButton);
	}
	else
	{
		pButton->SetPosition(-10000, -10000);
		pButton->Disable();
	}

	return 0x6A7E3E;
}

DEFINE_HOOK(0x6ABE6E, SidebarClass_RepositionTabButtons, 0x6)
{
	const auto config = SidebarExt::ActiveConfig();
	int tabCount = config.Tabs.Count.Get(4);
	int visibleTabs = (tabCount == 1) ? 0 : tabCount;

	GET(int, edi, EDI);
	GET(ShapeButtonClass*, pButton, ESI);

	int slot = SidebarExt::GetTabSlot(edi, config);

	if (slot < 0 || slot >= visibleTabs)
	{
		pButton->SetPosition(-10000, -10000);
		pButton->Disable();
		GScreenClass::Instance.RemoveButton(pButton);

		return 0x6ABE94;
	}

	if (static_cast<size_t>(slot) < config.Tabs.Positions.size())
	{
		int posX = config.Tabs.Positions[slot].X;
		int posY = config.Tabs.Positions[slot].Y;
		pButton->SetPosition(posX, posY);
		pButton->MarkRedraw();
		return 0x6ABE94;
	}
	else if (!config.Tabs.Order.empty())
	{
		int baseX = *reinterpret_cast<int*>(0x00B0B4E8);
		int tabWidth = *reinterpret_cast<int*>(0x00B0B4F0);
		int posY = *reinterpret_cast<int*>(0x00B0B4EC);
		pButton->SetPosition(baseX + slot * tabWidth, posY);
		pButton->MarkRedraw();
		return 0x6ABE94;
	}

	R->EDX(*reinterpret_cast<DWORD*>(0x00B0B4F0));
	return 0x6ABE74;
}

DEFINE_HOOK(0x6A6483, SidebarClass_Recalc_EnableTabButton, 0x7)
{
	const auto config = SidebarExt::ActiveConfig();
	int tabCount = config.Tabs.Count.Get(4);
	int visibleTabs = (tabCount == 1) ? 0 : tabCount;

	GET_STACK(int, tabIdx, 0x18);
	GET(ShapeButtonClass*, pButton, ESI);

	int slot = SidebarExt::GetTabSlot(tabIdx, config);

	if (slot >= 0 && slot < visibleTabs)
		pButton->Enable();
	else
	{
		pButton->Disable();
		pButton->SetPosition(-10000, -10000);
	}

	return 0x6A648A;
}

DEFINE_HOOK(0x6A67FD, SidebarClass_Recalc2_EnableTabButton, 0x7)
{
	const auto config = SidebarExt::ActiveConfig();
	int tabCount = config.Tabs.Count.Get(4);
	int visibleTabs = (tabCount == 1) ? 0 : tabCount;

	GET(int, tabIdx, ESI);
	GET(ShapeButtonClass*, pButton, EBX);

	int slot = SidebarExt::GetTabSlot(tabIdx, config);

	if (slot >= 0 && slot < visibleTabs)
		pButton->Enable();
	else
	{
		pButton->Disable();
		pButton->SetPosition(-10000, -10000);
	}

	return 0x6A6804;
}

DEFINE_HOOK(0x6A593E, SidebarClass_InitForHouse_AdditionalFiles, 0x5)
{
	char filename[0x20];

	for (int i = 0; i < 16; i++)
	{
		sprintf_s(filename, "tab%02dpp.shp", i);
		if (FileSystem::LoadSHPFile(filename))
		{
			SidebarExt::TabProducingProgress[i] = GameCreate<SHPReference>(filename);
		}
		else
		{
			SidebarExt::TabProducingProgress[i] = nullptr;
		}
	}

	return 0;
}

DEFINE_HOOK(0x6A5EA1, SidebarClass_UnloadShapes_AdditionalFiles, 0x5)
{
	for (int i = 0; i < 16; i++)
	{
		if (SidebarExt::TabProducingProgress[i])
		{
			GameDelete(SidebarExt::TabProducingProgress[i]);
			SidebarExt::TabProducingProgress[i] = nullptr;
		}
	}

	return 0;
}

DEFINE_HOOK(0x6A6EB1, SidebarClass_DrawIt_ProducingProgress, 0x6)
{
	if (Phobos::UI::ProducingProgress_Show)
	{
		const auto config = SidebarExt::ActiveConfig();
		if (config.Tabs.Count.isset() && config.Tabs.Count.Get() == 1)
			return 0;

		const auto pPlayer = HouseClass::CurrentPlayer;
		const auto pSideExt = SideExt::Fetch(SideClass::Array.GetItem(HouseClass::CurrentPlayer->SideIndex));
		const int XOffset = pSideExt->Sidebar_GDIPositions ? 29 : 32;
		const int XBase = (pSideExt->Sidebar_GDIPositions ? 26 : 20) + pSideExt->Sidebar_ProducingProgress_Offset.Get().X;
		const int YBase = 197 + pSideExt->Sidebar_ProducingProgress_Offset.Get().Y;

		for (int t = 0; t < 16; ++t)
		{
			if (!SidebarExt::IsTabVisible(t, config))
				continue;

			auto pSHP = SidebarExt::TabProducingProgress[t];
			if (!pSHP && t < 4)
				pSHP = SidebarExt::TabProducingProgress[0];

			if (!pSHP)
				continue;

			FactoryClass* pActiveFactory = nullptr;
			auto checkFactory = [&](FactoryClass* pF)
			{
				if (pF && pF->Object)
				{
					if (!pActiveFactory || pF->GetProgress() > pActiveFactory->GetProgress())
						pActiveFactory = pF;
				}
			};

			if (SidebarExt::ResolveVisibleTab(0, AbstractType::BuildingType, config) == t)
				checkFactory(pPlayer->GetPrimaryFactory(AbstractType::BuildingType, false, BuildCat::DontCare));

			if (SidebarExt::ResolveVisibleTab(1, AbstractType::BuildingType, config) == t)
				checkFactory(pPlayer->GetPrimaryFactory(AbstractType::BuildingType, false, BuildCat::Combat));

			if (SidebarExt::ResolveVisibleTab(2, AbstractType::InfantryType, config) == t)
				checkFactory(pPlayer->GetPrimaryFactory(AbstractType::InfantryType, false, BuildCat::DontCare));

			if (SidebarExt::ResolveVisibleTab(3, AbstractType::UnitType, config) == t)
			{
				checkFactory(pPlayer->GetPrimaryFactory(AbstractType::UnitType, false, BuildCat::DontCare));
				checkFactory(pPlayer->GetPrimaryFactory(AbstractType::UnitType, true, BuildCat::DontCare));
				checkFactory(pPlayer->GetPrimaryFactory(AbstractType::AircraftType, false, BuildCat::DontCare));
			}

			const int idxFrame = pActiveFactory
				? static_cast<int>(((double)pActiveFactory->GetProgress() / 54.0) * (pSHP->Frames - 1))
				: -1;

			int slot = SidebarExt::GetTabSlot(t, config);
			Point2D vPos = { XBase + slot * XOffset, YBase };
			RectangleStruct sidebarRect = DSurface::Sidebar->GetRect();

			if (idxFrame != -1)
			{
				DSurface::Sidebar->DrawSHP(FileSystem::SIDEBAR_PAL, pSHP, idxFrame, &vPos,
					&sidebarRect, BlitterFlags::bf_400, 0, 0, ZGradient::Ground, 1000, 0, 0, 0, 0, 0);
			}
		}
	}

	return 0;
}

DEFINE_HOOK(0x6A6EC7, SidebarClass_DrawIt_CustomButtons, 0x6)
{
	SidebarExt::DrawCustomButtons();
	return 0;
}

DEFINE_HOOK(0x72FCB5, InitSideRectangles_CenterBackground, 0x5)
{
	if (Phobos::UI::CenterPauseMenuBackground)
	{
		GET(RectangleStruct*, pRect, EAX);
		GET_STACK(const int, width, STACK_OFFSET(0x18, -0x4));
		GET_STACK(const int, height, STACK_OFFSET(0x18, -0x8));

		pRect->X = (width - 168 - pRect->Width) / 2;
		pRect->Y = (height - 32 - pRect->Height) / 2;

		R->EAX(pRect);
	}

	return 0;
}

// -----------------------------------------------------------------------------
// Credits Rendering Hooks
// -----------------------------------------------------------------------------

DEFINE_HOOK(0x4A23C2, CreditClass_GraphicLogic_Coords1, 0xC)
{
	const auto config = SidebarExt::ActiveConfig();
	int posX = R->EDI();
	int posY = 2;

	if (config.Credits.Position.isset())
	{
		posX = config.Credits.Position.Get().X;
		posY = config.Credits.Position.Get().Y;
	}

	R->Stack<int>(0x0C, posX);
	R->Stack<int>(0x10, posY);

	return 0x4A23CE;
}

DEFINE_HOOK(0x4A254E, CreditClass_GraphicLogic_Coords2, 0x8)
{
	const auto config = SidebarExt::ActiveConfig();
	int posX = R->EDI();
	int posY = R->ESI();

	if (config.Credits.Position.isset())
	{
		posX = config.Credits.Position.Get().X;
		posY = config.Credits.Position.Get().Y;
	}

	R->Stack<int>(0x0C, posX);
	R->Stack<int>(0x10, posY);

	return 0x4A2556;
}

DEFINE_HOOK(0x4A25B8, CreditClass_GraphicLogic_Format, 0x6)
{
	const auto config = SidebarExt::ActiveConfig();
	DWORD printFlags = 0x4100;
	TextAlign align = config.Credits.Align.Get(TextAlign::Center);

	if (align == TextAlign::Left)
	{
		printFlags |= 0;
	}
	else if (align == TextAlign::Right)
	{
		printFlags |= 1;
	}
	else
	{
		printFlags |= 8;
	}

	R->Stack<DWORD>(0x4, printFlags);

	if (config.Credits.Color.isset())
	{
		R->EAX(Drawing::RGB_To_Int(config.Credits.Color.Get()));
	}
	else
	{
		GET(DWORD, edxVal, EDX);
		R->EAX(R->EAX() | edxVal);
	}

	R->EDX(R->ESP() + 0x18);

	return 0x4A25BE;
}

// -----------------------------------------------------------------------------
// PowerBar Hooks
// -----------------------------------------------------------------------------

DEFINE_HOOK(0x63FB72, PowerClass_DrawIt_Offsets, 0x11)
{
	const auto config = SidebarExt::ActiveConfig();

	if (config.PowerBar.Position.isset())
	{
		R->EBX(config.PowerBar.Position.Get().X);
	}

	if (config.PowerBar.Height.isset() && config.PowerBar.Height.Get() > 0)
	{
		*reinterpret_cast<DWORD*>(0x00B0B504) = config.PowerBar.Height.Get();
	}

	DWORD baseOffset = *reinterpret_cast<DWORD*>(0x886F94);
	R->ECX(baseOffset);
	R->Stack<DWORD>(0x10, R->EAX());
	R->EAX(R->ESP() + 0x10);

	if (config.PowerBar.Position.isset())
	{
		R->ESI(config.PowerBar.Position.Get().Y);
	}
	else
	{
		R->ESI(baseOffset + 0x45);
	}

	return 0x63FB83;
}

DEFINE_HOOK(0x63FBE2, PowerClass_DrawIt_Shape, 0x6)
{
	const auto config = SidebarExt::ActiveConfig();
	if (config.PowerBar.Shape[0] != '\0')
	{
		if (auto pSHP = FileSystem::LoadSHPFile(config.PowerBar.Shape.data()))
		{
			R->ECX(pSHP);
			return 0x63FBE8;
		}
	}

	return 0;
}

#pragma region NewButtonsRelated

DEFINE_HOOK(0x692419, DisplayClass_ProcessClickCoords_SkipOnNewButtons, 0x7)
{
	enum { DoNothing = 0x6925FC };

	return (SWSidebarClass::IsEnabled() && SWSidebarClass::Instance.CurrentColumn
		|| SWSidebarClass::Instance.ToggleButton && SWSidebarClass::Instance.ToggleButton->IsHovering
		|| MessageColumnClass::Instance.IsBlocked()
		|| (ObserverUIClass::IsActive() && ObserverUIClass::Instance.IsMouseHoveringUI()))
		? DoNothing : 0;
}

DEFINE_HOOK(0x6A5082, SidebarClass_InitClear_InitializeNewButtons, 0x5)
{
	SidebarExt::InitClear();
	SWSidebarClass::Instance.InitClear();
	MessageColumnClass::Instance.InitClear();
	return 0;
}

DEFINE_HOOK(0x6A5839, SidebarClass_InitIO_InitializeNewButtons, 0x5)
{
	SidebarExt::InitIO();
	SWSidebarClass::Instance.InitIO();
	MessageColumnClass::Instance.InitIO();
	return 0;
}

DEFINE_HOOK_AGAIN(0x4E13B2, GadgetClass_DTOR_ClearCurrentOverGadget, 0x6)
DEFINE_HOOK(0x4E1A84, GadgetClass_DTOR_ClearCurrentOverGadget, 0x6)
{
	GadgetClass* const pThis = (R->Origin() == 0x4E1A84) ? R->ESI<GadgetClass*>() : R->ECX<GadgetClass*>();
	AnnounceInvalidPointer(Make_Global<GadgetClass*>(0x8B3E94), pThis);
	return 0;
}

// -----------------------------------------------------------------------------
// Action Mode Hooks (RequiresBuildings bypass)
// -----------------------------------------------------------------------------

DEFINE_HOOK(0x4AC884, MapClass_SetTogglePowerMode_RequiresBuildings, 0x6)
{
	if (!SidebarExt::IsTogglePowerRequiresBuildings())
	{
		return 0x4AC894;
	}

	return 0;
}

DEFINE_HOOK(0x4AC924, MapClass_SetRepairMode_RequiresBuildings, 0x6)
{
	if (!SidebarExt::IsRepairRequiresBuildings())
	{
		return 0x4AC934;
	}

	return 0;
}

DEFINE_HOOK(0x4AC6C4, MapClass_SetSellMode_RequiresBuildings, 0x6)
{
	if (!SidebarExt::IsSellRequiresBuildings())
	{
		return 0x4AC6D4;
	}

	return 0;
}

#pragma endregion


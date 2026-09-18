#include "Body.h"
#include "SWSidebar/SWSidebarClass.h"

#include <Ext/Side/Body.h>
#include <Ext/TechnoType/Body.h>
#include <Misc/MessageColumn.h>
#include <Drawing.h>

DEFINE_HOOK(0x6ABC60, SidebarClass_GetObjectTabIdx, 0x5)
{
	GET(AbstractType, abs, ECX);
	GET(int, idxType, EDX);

	const auto pTechnoType = TechnoTypeClass::GetByTypeAndIndex(abs, idxType);
	if (pTechnoType)
	{
		if (const auto pExt = TechnoTypeExt::TryFetch(pTechnoType))
		{
			if (pExt->TabIndex.isset())
			{
				const auto config = SidebarExt::ActiveConfig();
				int maxCount = config.Tabs.Count.Get(4);
				int tabIdx = pExt->TabIndex.Get();

				if (tabIdx >= 0 && tabIdx < maxCount)
				{
					R->EAX(tabIdx);
					return 0x6ABC9A;
				}
				else
				{
					Debug::Log("[Sidebar] Warning: TechnoType [%s] has TabIndex=%d out of bounds (Tabs.Count=%d). Falling back to vanilla.\n",
						pTechnoType->ID, tabIdx, maxCount);
				}
			}
		}
	}

	return 0;
}

DEFINE_HOOK(0x6A7590, SidebarClass_SetTab, 0x5)
{
	GET_STACK(int, tabIndex, 0x4);
	const auto config = SidebarExt::ActiveConfig();
	if (!config.Tabs.Order.empty() && tabIndex >= 0 && static_cast<size_t>(tabIndex) < config.Tabs.Order.size())
	{
		R->Stack(0x4, config.Tabs.Order[tabIndex]);
	}
	return 0;
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
		{
			return 0;
		}

		const auto pPlayer = HouseClass::CurrentPlayer;
		const auto pSideExt = SideExt::Fetch(SideClass::Array.GetItem(HouseClass::CurrentPlayer->SideIndex));
		const int XOffset = pSideExt->Sidebar_GDIPositions ? 29 : 32;
		const int XBase = (pSideExt->Sidebar_GDIPositions ? 26 : 20) + pSideExt->Sidebar_ProducingProgress_Offset.Get().X;
		const int YBase = 197 + pSideExt->Sidebar_ProducingProgress_Offset.Get().Y;

		const int maxTabs = config.Tabs.Count.Get(4);
		for (int i = 0; i < maxTabs && i < 16; i++)
		{
			if (const auto pSHP = SidebarExt::TabProducingProgress[i])
			{
				const auto rtti = i == 0 || i == 1 ? AbstractType::BuildingType : AbstractType::InfantryType;
				FactoryClass* pFactory = nullptr;

				if (i != 3)
				{
					pFactory = pPlayer->GetPrimaryFactory(rtti, false, i == 1 ? BuildCat::Combat : BuildCat::DontCare);
				}
				else
				{
					pFactory = pPlayer->GetPrimaryFactory(AbstractType::UnitType, false, BuildCat::DontCare);
					if (!pFactory || !pFactory->Object)
						pFactory = pPlayer->GetPrimaryFactory(AbstractType::UnitType, true, BuildCat::DontCare);
					if (!pFactory || !pFactory->Object)
						pFactory = pPlayer->GetPrimaryFactory(AbstractType::AircraftType, false, BuildCat::DontCare);
				}

				const int idxFrame = pFactory
					? (int)(((double)pFactory->GetProgress() / 54) * (pSHP->Frames - 1))
					: -1;

				Point2D vPos = { XBase + i * XOffset, YBase };
				RectangleStruct sidebarRect = DSurface::Sidebar->GetRect();

				if (idxFrame != -1)
				{
					DSurface::Sidebar->DrawSHP(FileSystem::SIDEBAR_PAL, pSHP, idxFrame, &vPos,
						&sidebarRect, BlitterFlags::bf_400, 0, 0, ZGradient::Ground, 1000, 0, 0, 0, 0, 0);
				}
			}
		}
	}

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
		printFlags |= 0;
	else if (align == TextAlign::Right)
		printFlags |= 1;
	else
		printFlags |= 8;

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

	auto pECX = *reinterpret_cast<DWORD*>(0x886F94);
	R->ECX(pECX);
	R->Stack<DWORD>(0x10, R->EAX());
	R->EAX(R->ESP() + 0x10);

	if (config.PowerBar.Position.isset())
	{
		R->ESI(config.PowerBar.Position.Get().Y);
	}
	else
	{
		R->ESI(pECX + 0x45);
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
		|| MessageColumnClass::Instance.IsBlocked())
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

#pragma endregion

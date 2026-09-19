#include "Body.h"
#include <Ext/Sidebar/Body.h>

namespace SidebarGDIPositionsTemp
{
	bool isNODSidebar = false;

	inline Point2D ResolveCoord(Point2D pos, DWORD sidebarX)
	{
		return SidebarExt::ResolveCoord(pos, sidebarX);
	}
}

using namespace SidebarGDIPositionsTemp;

DEFINE_HOOK(0x534FA7, Prep_For_Side, 0x5)
{
	GET(const int, sideIndex, ECX);
	const auto pSide = SideClass::Array.GetItemOrDefault(sideIndex);
	const auto pSideExt = SideExt::TryFetch(pSide);
	isNODSidebar = pSideExt ? !pSideExt->Sidebar_GDIPositions : sideIndex;

	return 0;
}

DEFINE_HOOK(0x652EAB, RadarClass_InitForHouse, 0x6)
{
	R->EAX(isNODSidebar);
	return 0x652EB7;
}

DEFINE_HOOK(0x652F4F, RadarClass_InitForHouse_Buttons, 0x6)
{
	R->EDX(*reinterpret_cast<DWORD*>(R->ESI() + 0x11F0));

	const auto config = SidebarExt::ActiveConfig();
	DWORD sidebarX = *reinterpret_cast<DWORD*>(0x886F90);

	if (config.DiplomacyButton.Position.isset())
	{
		Point2D dPos = ResolveCoord(config.DiplomacyButton.Position.Get(), sidebarX);
		*reinterpret_cast<DWORD*>(0x00B04A00) = dPos.X;
		*reinterpret_cast<DWORD*>(0x00B04A04) = dPos.Y;
	}
	if (config.DiplomacyButton.Show.isset() && !config.DiplomacyButton.Show.Get())
	{
		*reinterpret_cast<DWORD*>(0x00B04A00) = static_cast<DWORD>(-10000);
		*reinterpret_cast<DWORD*>(0x00B04A04) = static_cast<DWORD>(-10000);
	}

	if (config.MenuButton.Position.isset())
	{
		Point2D mPos = ResolveCoord(config.MenuButton.Position.Get(), sidebarX);
		*reinterpret_cast<DWORD*>(0x00B048C8) = mPos.X;
		*reinterpret_cast<DWORD*>(0x00B048CC) = mPos.Y;
	}
	if (config.MenuButton.Show.isset() && !config.MenuButton.Show.Get())
	{
		*reinterpret_cast<DWORD*>(0x00B048C8) = static_cast<DWORD>(-10000);
		*reinterpret_cast<DWORD*>(0x00B048CC) = static_cast<DWORD>(-10000);
	}

	return 0;
}

DEFINE_HOOK(0x6A5090, SidebarClass_InitPositions, 0x5)
{
	DWORD repairY = isNODSidebar ? 165 : 166;
	DWORD tabsY = 197;
	DWORD cameosY = 227;

	if (!isNODSidebar)
	{
		*reinterpret_cast<DWORD*>(0x00B0B4E4) = 0x40; // Repair Width (64)
		*reinterpret_cast<DWORD*>(0x00B0B4F0) = 0x1D; // Tab Width (29)
		*reinterpret_cast<DWORD*>(0x00B0B4FC) = 0x3F; // Cameo Width (63)
		*reinterpret_cast<DWORD*>(0x00B0B4E0) = repairY;
		*reinterpret_cast<DWORD*>(0x00B0B4EC) = tabsY;
		*reinterpret_cast<DWORD*>(0x00B0B4F8) = cameosY;
	}
	else
	{
		*reinterpret_cast<DWORD*>(0x00B0B4E4) = 0x34; // Repair Width (52)
		*reinterpret_cast<DWORD*>(0x00B0B4F0) = 0x20; // Tab Width (32)
		*reinterpret_cast<DWORD*>(0x00B0B4FC) = 0x40; // Cameo Width (64)
		*reinterpret_cast<DWORD*>(0x00B0B4E0) = repairY;
		*reinterpret_cast<DWORD*>(0x00B0B4EC) = tabsY;
		*reinterpret_cast<DWORD*>(0x00B0B4F8) = cameosY;
	}

	const auto config = SidebarExt::ActiveConfig();
	DWORD sidebarX = *reinterpret_cast<DWORD*>(0x886F90);

	if (config.Cameos.Y.isset())
	{
		*reinterpret_cast<DWORD*>(0x00B0B4F8) = config.Cameos.Y.Get();
	}
	else if (config.Tabs.Count.isset() && config.Tabs.Count.Get() == 1)
	{
		// In single-tab mode without explicit Cameos.Y, cameos start where the tab bar used to start
		*reinterpret_cast<DWORD*>(0x00B0B4F8) = *reinterpret_cast<DWORD*>(0x00B0B4EC);
	}

	if (config.Tabs.Count.isset() && config.Tabs.Count.Get() == 1)
	{
		*reinterpret_cast<DWORD*>(0x00B0B4E8) = static_cast<DWORD>(-10000);
		*reinterpret_cast<DWORD*>(0x00B0B4EC) = static_cast<DWORD>(-10000);
	}

	if (config.RepairButton.Position.isset())
	{
		Point2D rPos = ResolveCoord(config.RepairButton.Position.Get(), sidebarX);
		*reinterpret_cast<DWORD*>(0x00B0B4DC) = rPos.X;
		*reinterpret_cast<DWORD*>(0x00B0B4E0) = rPos.Y;
	}
	if (config.RepairButton.Show.isset() && !config.RepairButton.Show.Get())
	{
		*reinterpret_cast<DWORD*>(0x00B0B4DC) = static_cast<DWORD>(-10000);
		*reinterpret_cast<DWORD*>(0x00B0B4E0) = static_cast<DWORD>(-10000);
	}

	// Baseline SellButton coordinates (independent of RepairButton state)
	int baseSellX = sidebarX + (isNODSidebar ? (0x21 + 0x34) : (0x14 + 0x40));
	int baseSellY = repairY;

	int sellX = baseSellX;
	int sellY = baseSellY;
	if (config.SellButton.Position.isset())
	{
		Point2D sPos = ResolveCoord(config.SellButton.Position.Get(), sidebarX);
		sellX = sPos.X;
		sellY = sPos.Y;
	}
	else if (config.RepairButton.Position.isset() && (!config.RepairButton.Show.isset() || config.RepairButton.Show.Get()))
	{
		Point2D rPos = ResolveCoord(config.RepairButton.Position.Get(), sidebarX);
		sellX = rPos.X + (isNODSidebar ? 0x34 : 0x40);
		sellY = rPos.Y;
	}

	if (config.SellButton.Show.isset() && !config.SellButton.Show.Get())
	{
		sellX = -10000;
		sellY = -10000;
	}
	*reinterpret_cast<DWORD*>(0x00B07E04) = sellX;
	*reinterpret_cast<DWORD*>(0x00B07E08) = sellY;

	return 0x6A50DB;
}

DEFINE_HOOK(0x6A51E9, SidebarClass_InitGUI, 0x6)
{
	DWORD& SidebarClass__OBJECT_HEIGHT = *reinterpret_cast<DWORD*>(0xB0B500);
	SidebarClass__OBJECT_HEIGHT = 0x32;

	const auto config = SidebarExt::ActiveConfig();
	if (config.Cameos.Height.isset() && config.Cameos.Height.Get() > 0)
	{
		DWORD cameosY = *reinterpret_cast<DWORD*>(0x00B0B4F8);
		DWORD topMargin = *reinterpret_cast<DWORD*>(0x886F94);
		*reinterpret_cast<DWORD*>(0x00886F9C) = (cameosY - topMargin) + config.Cameos.Height.Get();
	}
	else if (config.Cameos.MarginBottom.isset())
	{
		int extraMargin = config.Cameos.MarginBottom.Get() - 32;
		*reinterpret_cast<DWORD*>(0x00886F9C) -= extraMargin;
	}

	R->ESI(isNODSidebar);
	R->EDX(isNODSidebar);
	R->EAX(*reinterpret_cast<DWORD*>(0x00886F9C));

	return 0x6A5205;
}

DEFINE_HOOK(0x6A532B, SidebarClass_InitGUI_AfterInitPositions, 0x5)
{
	const auto config = SidebarExt::ActiveConfig();
	DWORD sidebarX = *reinterpret_cast<DWORD*>(0x886F90);

	if (config.Tabs.Count.isset() && config.Tabs.Count.Get() == 1)
	{
		*reinterpret_cast<DWORD*>(0x00B0B4E8) = static_cast<DWORD>(-10000);
		*reinterpret_cast<DWORD*>(0x00B0B4EC) = static_cast<DWORD>(-10000);
	}

	if (config.RepairButton.Position.isset())
	{
		Point2D rPos = ResolveCoord(config.RepairButton.Position.Get(), sidebarX);
		*reinterpret_cast<DWORD*>(0x00B0B4DC) = rPos.X;
		*reinterpret_cast<DWORD*>(0x00B0B4E0) = rPos.Y;
	}
	if (config.RepairButton.Show.isset() && !config.RepairButton.Show.Get())
	{
		*reinterpret_cast<DWORD*>(0x00B0B4DC) = static_cast<DWORD>(-10000);
		*reinterpret_cast<DWORD*>(0x00B0B4E0) = static_cast<DWORD>(-10000);
	}

	return 0;
}

DEFINE_HOOK(0x6A53BF, SidebarClass_InitGUI_SellButtonPos, 0x6)
{
	const auto config = SidebarExt::ActiveConfig();

	DWORD sidebarX = *reinterpret_cast<DWORD*>(0x886F90);
	int baseSellX = sidebarX + (isNODSidebar ? (0x21 + 0x34) : (0x14 + 0x40));
	int baseSellY = isNODSidebar ? 165 : 166;

	int posX = baseSellX;
	int posY = baseSellY;

	if (config.SellButton.Position.isset())
	{
		Point2D sPos = ResolveCoord(config.SellButton.Position.Get(), sidebarX);
		posX = sPos.X;
		posY = sPos.Y;
	}
	else if (config.RepairButton.Position.isset() && (!config.RepairButton.Show.isset() || config.RepairButton.Show.Get()))
	{
		Point2D rPos = ResolveCoord(config.RepairButton.Position.Get(), sidebarX);
		posX = rPos.X + (isNODSidebar ? 0x34 : 0x40);
		posY = rPos.Y;
	}

	if (config.SellButton.Show.isset() && !config.SellButton.Show.Get())
	{
		posX = -10000;
		posY = -10000;
	}

	R->ECX(posX);
	R->EDX(posY);
	*reinterpret_cast<DWORD*>(0x00B07E04) = posX;
	*reinterpret_cast<DWORD*>(0x00B07E08) = posY;
	return 0x6A53C5;
}

DEFINE_HOOK(0x6ABE03, SidebarClass_RepositionButtons, 0x5)
{
	const auto config = SidebarExt::ActiveConfig();
	DWORD sidebarX = *reinterpret_cast<DWORD*>(0x886F90);

	if (config.Tabs.Count.isset() && config.Tabs.Count.Get() == 1)
	{
		*reinterpret_cast<DWORD*>(0x00B0B4E8) = static_cast<DWORD>(-10000);
		*reinterpret_cast<DWORD*>(0x00B0B4EC) = static_cast<DWORD>(-10000);
	}

	if (config.RepairButton.Position.isset())
	{
		Point2D rPos = ResolveCoord(config.RepairButton.Position.Get(), sidebarX);
		*reinterpret_cast<DWORD*>(0x00B0B4DC) = rPos.X;
		*reinterpret_cast<DWORD*>(0x00B0B4E0) = rPos.Y;
	}
	if (config.RepairButton.Show.isset() && !config.RepairButton.Show.Get())
	{
		*reinterpret_cast<DWORD*>(0x00B0B4DC) = static_cast<DWORD>(-10000);
		*reinterpret_cast<DWORD*>(0x00B0B4E0) = static_cast<DWORD>(-10000);
	}

	if (SidebarExt::ActiveTogglePowerButton)
	{
		if (config.TogglePowerButton.Position.isset())
		{
			Point2D tpPos = ResolveCoord(config.TogglePowerButton.Position.Get(), sidebarX);
			SidebarExt::ActiveTogglePowerButton->SetPosition(tpPos.X, tpPos.Y);
			SidebarExt::ActiveTogglePowerButton->MarkRedraw();
		}
	}

	for (auto pBtn : SidebarExt::ActiveCustomButtons)
	{
		if (pBtn && pBtn->Config.Position.isset())
		{
			Point2D cPos = ResolveCoord(pBtn->Config.Position.Get(), sidebarX);
			pBtn->SetPosition(cPos.X, cPos.Y);
			pBtn->MarkRedraw();
		}
	}

	return 0;
}

DEFINE_HOOK(0x6ABE45, SidebarClass_RepositionSellButton, 0x7)
{
	const auto config = SidebarExt::ActiveConfig();

	DWORD sidebarX = *reinterpret_cast<DWORD*>(0x886F90);
	int baseSellX = sidebarX + (isNODSidebar ? (0x21 + 0x34) : (0x14 + 0x40));
	int baseSellY = isNODSidebar ? 165 : 166;

	int posX = baseSellX;
	int posY = baseSellY;

	if (config.SellButton.Position.isset())
	{
		Point2D sPos = ResolveCoord(config.SellButton.Position.Get(), sidebarX);
		posX = sPos.X;
		posY = sPos.Y;
	}
	else if (config.RepairButton.Position.isset() && (!config.RepairButton.Show.isset() || config.RepairButton.Show.Get()))
	{
		Point2D rPos = ResolveCoord(config.RepairButton.Position.Get(), sidebarX);
		posX = rPos.X + (isNODSidebar ? 0x34 : 0x40);
		posY = rPos.Y;
	}

	if (config.SellButton.Show.isset() && !config.SellButton.Show.Get())
	{
		posX = -10000;
		posY = -10000;
	}

	R->EDX(posX);
	R->EAX(posY);
	*reinterpret_cast<DWORD*>(0x00B07E04) = posX;
	*reinterpret_cast<DWORD*>(0x00B07E08) = posY;
	return 0;
}

DEFINE_HOOK(0x6ABEAA, SidebarClass_RepositionButtons_ScrollButtons, 0x6)
{
	const auto config = SidebarExt::ActiveConfig();
	bool customUp = config.ScrollUpButton.Position.isset() || (config.ScrollUpButton.Show.isset() && !config.ScrollUpButton.Show.Get());
	bool customDown = config.ScrollDownButton.Position.isset() || (config.ScrollDownButton.Show.isset() && !config.ScrollDownButton.Show.Get());

	if (customUp || customDown)
	{
		DWORD sidebarX = *reinterpret_cast<DWORD*>(0x886F90);

		if (config.ScrollUpButton.Show.isset() && !config.ScrollUpButton.Show.Get())
		{
			SidebarClass::ScrollUpButton.SetPosition(-10000, -10000);
			SidebarClass::ScrollUpButton.Disable();
		}
		else if (config.ScrollUpButton.Position.isset())
		{
			Point2D uPos = ResolveCoord(config.ScrollUpButton.Position.Get(), sidebarX);
			SidebarClass::ScrollUpButton.SetPosition(uPos.X, uPos.Y);
			SidebarClass::ScrollUpButton.MarkRedraw();
		}

		if (config.ScrollDownButton.Show.isset() && !config.ScrollDownButton.Show.Get())
		{
			SidebarClass::ScrollDownButton.SetPosition(-10000, -10000);
			SidebarClass::ScrollDownButton.Disable();
		}
		else if (config.ScrollDownButton.Position.isset())
		{
			Point2D dPos = ResolveCoord(config.ScrollDownButton.Position.Get(), sidebarX);
			SidebarClass::ScrollDownButton.SetPosition(dPos.X, dPos.Y);
			SidebarClass::ScrollDownButton.MarkRedraw();
		}
		else if (customUp && (!config.ScrollDownButton.Show.isset() || config.ScrollDownButton.Show.Get()))
		{
			// If ScrollUp was customized but ScrollDown was not, follow ScrollUp button by +36px
			SidebarClass::ScrollDownButton.SetPosition(SidebarClass::ScrollUpButton.X + 36, SidebarClass::ScrollUpButton.Y);
			SidebarClass::ScrollDownButton.MarkRedraw();
		}

		return 0x6ABF01;
	}

	return 0;
}

// PowerBar Positions & Visibility
DEFINE_HOOK(0x63FB5D, PowerClass_DrawIt, 0x6)
{
	const auto config = SidebarExt::ActiveConfig();
	if ((config.PowerBar.Show.isset() && !config.PowerBar.Show.Get()) ||
		(config.PowerBar.Height.isset() && config.PowerBar.Height.Get() == 0))
	{
		R->EBX(*reinterpret_cast<DWORD*>(R->ESP()));
		R->ESP(R->ESP() + 4);
		return 0x63FDA5;
	}

	R->EAX(isNODSidebar);
	return 0x63FB63;
}

// PowerBar Tooltip Positions
DEFINE_HOOK(0x6403DF, PowerClass_InitGUI, 0x6)
{
	R->ESI(isNODSidebar);
	return 0x6403E5;
}

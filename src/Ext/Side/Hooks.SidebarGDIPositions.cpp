#include "Body.h"
#include <Ext/Sidebar/Body.h>

bool isNODSidebar = false;

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

	const auto config = SidebarExt::ActiveConfig();
	if (config.DiplomacyButton.Position.isset())
	{
		*reinterpret_cast<DWORD*>(0x00B04A00) = config.DiplomacyButton.Position.Get().X;
		*reinterpret_cast<DWORD*>(0x00B04A04) = config.DiplomacyButton.Position.Get().Y;
	}
	if (config.DiplomacyButton.Show.isset() && !config.DiplomacyButton.Show.Get())
	{
		*reinterpret_cast<DWORD*>(0x00B04A00) = static_cast<DWORD>(-10000);
		*reinterpret_cast<DWORD*>(0x00B04A04) = static_cast<DWORD>(-10000);
	}

	if (config.MenuButton.Position.isset())
	{
		*reinterpret_cast<DWORD*>(0x00B048C8) = config.MenuButton.Position.Get().X;
		*reinterpret_cast<DWORD*>(0x00B048CC) = config.MenuButton.Position.Get().Y;
	}
	if (config.MenuButton.Show.isset() && !config.MenuButton.Show.Get())
	{
		*reinterpret_cast<DWORD*>(0x00B048C8) = static_cast<DWORD>(-10000);
		*reinterpret_cast<DWORD*>(0x00B048CC) = static_cast<DWORD>(-10000);
	}

	return 0x652EB7;
}

DEFINE_HOOK(0x6A5090, SidebarClass_InitPositions, 0x5)
{
	DWORD topMargin = *reinterpret_cast<DWORD*>(0x886F94);

	if (!isNODSidebar)
	{
		*reinterpret_cast<DWORD*>(0x00B0B4E4) = 0x40; // Repair Width (64)
		*reinterpret_cast<DWORD*>(0x00B0B4F0) = 0x1D; // Tab Width (29)
		*reinterpret_cast<DWORD*>(0x00B0B4FC) = 0x3F; // Cameo Width (63)

		DWORD repairY = topMargin + 8;
		*reinterpret_cast<DWORD*>(0x00B0B4E0) = repairY;
		DWORD tabsY = repairY + 0x1F;
		*reinterpret_cast<DWORD*>(0x00B0B4EC) = tabsY;
		DWORD cameosY = tabsY + 0x1E;
		*reinterpret_cast<DWORD*>(0x00B0B4F8) = cameosY;
	}
	else
	{
		*reinterpret_cast<DWORD*>(0x00B0B4E4) = 0x34; // Repair Width (52)
		*reinterpret_cast<DWORD*>(0x00B0B4F0) = 0x20; // Tab Width (32)
		*reinterpret_cast<DWORD*>(0x00B0B4FC) = 0x40; // Cameo Width (64)

		DWORD repairY = topMargin + 7;
		*reinterpret_cast<DWORD*>(0x00B0B4E0) = repairY;
		DWORD tabsY = repairY + 0x20;
		*reinterpret_cast<DWORD*>(0x00B0B4EC) = tabsY;
		DWORD cameosY = tabsY + 0x1E;
		*reinterpret_cast<DWORD*>(0x00B0B4F8) = cameosY;
	}

	const auto config = SidebarExt::ActiveConfig();

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
		*reinterpret_cast<DWORD*>(0x00B0B4DC) = config.RepairButton.Position.Get().X;
		*reinterpret_cast<DWORD*>(0x00B0B4E0) = config.RepairButton.Position.Get().Y;
	}
	if (config.RepairButton.Show.isset() && !config.RepairButton.Show.Get())
	{
		*reinterpret_cast<DWORD*>(0x00B0B4DC) = static_cast<DWORD>(-10000);
		*reinterpret_cast<DWORD*>(0x00B0B4E0) = static_cast<DWORD>(-10000);
	}

	if (config.SellButton.Position.isset())
	{
		*reinterpret_cast<DWORD*>(0x00B07E04) = config.SellButton.Position.Get().X;
		*reinterpret_cast<DWORD*>(0x00B07E08) = config.SellButton.Position.Get().Y;
	}
	if (config.SellButton.Show.isset() && !config.SellButton.Show.Get())
	{
		*reinterpret_cast<DWORD*>(0x00B07E04) = static_cast<DWORD>(-10000);
		*reinterpret_cast<DWORD*>(0x00B07E08) = static_cast<DWORD>(-10000);
	}

	return 0x6A50DB;
}

DEFINE_HOOK(0x6A51E9, SidebarClass_InitGUI, 0x6)
{
	DWORD& SidebarClass__OBJECT_HEIGHT = *reinterpret_cast<DWORD*>(0xB0B500);
	SidebarClass__OBJECT_HEIGHT = 0x32;

	R->ESI(isNODSidebar);
	R->EDX(isNODSidebar);

	return 0x6A5205;
}

DEFINE_HOOK(0x6A532B, SidebarClass_InitGUI_AfterInitPositions, 0x5)
{
	const auto config = SidebarExt::ActiveConfig();

	if (config.Tabs.Count.isset() && config.Tabs.Count.Get() == 1)
	{
		*reinterpret_cast<DWORD*>(0x00B0B4E8) = static_cast<DWORD>(-10000);
		*reinterpret_cast<DWORD*>(0x00B0B4EC) = static_cast<DWORD>(-10000);
	}

	if (config.RepairButton.Position.isset())
	{
		*reinterpret_cast<DWORD*>(0x00B0B4DC) = config.RepairButton.Position.Get().X;
		*reinterpret_cast<DWORD*>(0x00B0B4E0) = config.RepairButton.Position.Get().Y;
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
	int posX = R->ECX();
	int posY = R->EDX();

	if (config.SellButton.Position.isset())
	{
		posX = config.SellButton.Position.Get().X;
		posY = config.SellButton.Position.Get().Y;
	}
	if (config.SellButton.Show.isset() && !config.SellButton.Show.Get())
	{
		posX = -10000;
		posY = -10000;
	}

	R->ECX(posX);
	R->EDX(posY);
	*reinterpret_cast<DWORD*>(0x00B07E04) = posX;
	return 0x6A53C5;
}

DEFINE_HOOK(0x6ABE03, SidebarClass_RepositionButtons, 0x5)
{
	const auto config = SidebarExt::ActiveConfig();

	if (config.Tabs.Count.isset() && config.Tabs.Count.Get() == 1)
	{
		*reinterpret_cast<DWORD*>(0x00B0B4E8) = static_cast<DWORD>(-10000);
		*reinterpret_cast<DWORD*>(0x00B0B4EC) = static_cast<DWORD>(-10000);
	}

	if (config.RepairButton.Position.isset())
	{
		*reinterpret_cast<DWORD*>(0x00B0B4DC) = config.RepairButton.Position.Get().X;
		*reinterpret_cast<DWORD*>(0x00B0B4E0) = config.RepairButton.Position.Get().Y;
	}
	if (config.RepairButton.Show.isset() && !config.RepairButton.Show.Get())
	{
		*reinterpret_cast<DWORD*>(0x00B0B4DC) = static_cast<DWORD>(-10000);
		*reinterpret_cast<DWORD*>(0x00B0B4E0) = static_cast<DWORD>(-10000);
	}

	return 0;
}

DEFINE_HOOK(0x6ABE45, SidebarClass_RepositionSellButton, 0x7)
{
	const auto config = SidebarExt::ActiveConfig();
	int posX = R->EDX();
	int posY = R->EAX();

	if (config.SellButton.Position.isset())
	{
		posX = config.SellButton.Position.Get().X;
		posY = config.SellButton.Position.Get().Y;
	}
	if (config.SellButton.Show.isset() && !config.SellButton.Show.Get())
	{
		posX = -10000;
		posY = -10000;
	}

	R->EDX(posX);
	R->EAX(posY);
	return 0;
}

// PowerBar Positions & Visibility
DEFINE_HOOK(0x63FB5D, PowerClass_DrawIt, 0x6)
{
	const auto config = SidebarExt::ActiveConfig();
	if ((config.PowerBar.Show.isset() && !config.PowerBar.Show.Get()) ||
		(config.PowerBar.Height.isset() && config.PowerBar.Height.Get() == 0))
	{
		return 0x63FC8A;
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

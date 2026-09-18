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
	R->EAX(isNODSidebar);

	const auto config = SidebarExt::ActiveConfig();
	if (config.Cameos.Y.isset())
	{
		*reinterpret_cast<DWORD*>(0x00B0B4F8) = config.Cameos.Y.Get();
	}

	if (config.Tabs.Count.isset() && config.Tabs.Count.Get() == 1)
	{
		// Single tab mode: hide tab buttons by moving offscreen
		*reinterpret_cast<DWORD*>(0x00B0B4E8) = static_cast<DWORD>(-10000);
		*reinterpret_cast<DWORD*>(0x00B0B4EC) = static_cast<DWORD>(-10000);
		if (!config.Cameos.Y.isset())
		{
			// Pull cameo strip up
			*reinterpret_cast<DWORD*>(0x00B0B4F8) -= 20;
		}
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

	return 0x6A509B;
}

DEFINE_HOOK(0x6A51E9, SidebarClass_InitGUI, 0x6)
{
	DWORD& SidebarClass__OBJECT_HEIGHT = *reinterpret_cast<DWORD*>(0xB0B500);
	SidebarClass__OBJECT_HEIGHT = 0x32;

	R->ESI(isNODSidebar);
	R->EDX(isNODSidebar);

	const auto config = SidebarExt::ActiveConfig();
	if (config.Cameos.Y.isset())
	{
		*reinterpret_cast<DWORD*>(0x00B0B4F8) = config.Cameos.Y.Get();
	}

	if (config.Tabs.Count.isset() && config.Tabs.Count.Get() == 1)
	{
		*reinterpret_cast<DWORD*>(0x00B0B4E8) = static_cast<DWORD>(-10000);
		*reinterpret_cast<DWORD*>(0x00B0B4EC) = static_cast<DWORD>(-10000);
		if (!config.Cameos.Y.isset())
		{
			*reinterpret_cast<DWORD*>(0x00B0B4F8) -= 20;
		}
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

	return 0x6A5205;
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

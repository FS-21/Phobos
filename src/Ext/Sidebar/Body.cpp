#include "Body.h"
#include "SWSidebar/SWSidebarClass.h"

#include <EventClass.h>
#include <Drawing.h>
#include <GeneralStructures.h>
#include <SuperWeaponTypeClass.h>
#include <CommandClass.h>
#include <Ext/Side/Body.h>
#include <Ext/SWType/Body.h>
#include <Utilities/GeneralUtils.h>

std::unique_ptr<SidebarExt::ExtData> SidebarExt::Data = nullptr;
SHPStruct* SidebarExt::TabProducingProgress[16];

SidebarConfig SidebarExt::GlobalConfig {};
SidebarConfig SidebarExt::RulesConfig {};
SidebarConfig SidebarExt::ScenarioConfig {};
SidebarConfig SidebarExt::BaselineGlobalConfig {};

std::vector<CustomSidebarButtonClass*> SidebarExt::ActiveCustomButtons {};
CustomSidebarButtonClass* SidebarExt::ActiveTogglePowerButton = nullptr;

void SidebarExt::Allocate(SidebarClass* pThis)
{
	Data = std::make_unique<SidebarExt::ExtData>(pThis);
}

void SidebarExt::Remove(SidebarClass* pThis)
{
	Data = nullptr;
}

// -----------------------------------------------------------------------------
// SidebarButtonConfig
// -----------------------------------------------------------------------------

void SidebarButtonConfig::Read(CCINIClass* pINI, const char* pSection, const char* pPrefix)
{
	if (!pINI) return;
	INI_EX exINI(pINI);

	char keyBuf[0x80];
	auto makeKey = [&](const char* baseKey) -> const char* {
		if (pPrefix && *pPrefix) {
			sprintf_s(keyBuf, "%s%s", pPrefix, baseKey);
			return keyBuf;
		}
		return baseKey;
	};

	this->Show.Read(exINI, pSection, makeKey("Show"));
	this->Position.Read(exINI, pSection, makeKey("Position"));

	if (!this->Position.isset())
	{
		Nullable<int> posX;
		Nullable<int> posY;
		posX.Read(exINI, pSection, makeKey("X"));
		posY.Read(exINI, pSection, makeKey("Y"));
		if (posX.isset() || posY.isset())
		{
			this->Position = Point2D { posX.Get(0), posY.Get(0) };
		}
	}

	this->Size.Read(exINI, pSection, makeKey("Size"));
	if (!this->Size.isset())
	{
		Nullable<int> w;
		Nullable<int> h;
		w.Read(exINI, pSection, makeKey("Width"));
		h.Read(exINI, pSection, makeKey("Height"));
		if (w.isset() || h.isset())
		{
			this->Size = Point2D { w.Get(0), h.Get(0) };
		}
	}

	this->Shape.Read(pINI, pSection, makeKey("Shape"));
	this->RequiresBuildings.Read(exINI, pSection, makeKey("RequiresBuildings"));

	char actionBuf[0x40] = { 0 };
	if (pINI->ReadString(pSection, makeKey("Action"), "", actionBuf, sizeof(actionBuf)) ||
		pINI->ReadString(pSection, makeKey("Type"), "", actionBuf, sizeof(actionBuf)))
	{
		if (actionBuf[0] != '\0')
		{
			if (!_stricmp(actionBuf, "Repair"))
				this->Action = CustomButtonType::Repair;
			else if (!_stricmp(actionBuf, "Sell"))
				this->Action = CustomButtonType::Sell;
			else if (!_stricmp(actionBuf, "TogglePower") || !_stricmp(actionBuf, "Power"))
				this->Action = CustomButtonType::TogglePower;
			else if (!_stricmp(actionBuf, "SuperWeapon") || !_stricmp(actionBuf, "Special"))
				this->Action = CustomButtonType::SuperWeapon;
			else if (!_stricmp(actionBuf, "Command"))
				this->Action = CustomButtonType::Command;
			else if (!_stricmp(actionBuf, "Custom"))
				this->Action = CustomButtonType::Custom;
			else if (!_stricmp(actionBuf, "None"))
				this->Action = CustomButtonType::None;
		}
	}

	this->SuperWeapon.Read(pINI, pSection, makeKey("SuperWeapon"));
	this->SuperWeaponIndex.Read(exINI, pSection, makeKey("SuperWeaponIndex"));
	this->Tooltip.Read(pINI, pSection, makeKey("Tooltip"));
	this->Command.Read(pINI, pSection, makeKey("Command"));
}

void SidebarButtonConfig::Merge(const SidebarButtonConfig& other)
{
	if (other.Show.isset()) this->Show = other.Show;
	if (other.Action.isset()) this->Action = other.Action;
	if (other.Position.isset()) this->Position = other.Position;
	if (other.Size.isset()) this->Size = other.Size;
	if (other.Shape[0] != '\0') this->Shape = other.Shape;
	if (other.RequiresBuildings.isset()) this->RequiresBuildings = other.RequiresBuildings;
	if (other.SuperWeaponIndex.isset()) this->SuperWeaponIndex = other.SuperWeaponIndex;
	if (other.SuperWeapon[0] != '\0') this->SuperWeapon = other.SuperWeapon;
	if (other.Tooltip[0] != '\0') this->Tooltip = other.Tooltip;
	if (other.Command[0] != '\0') this->Command = other.Command;
}

template <typename T>
void SidebarButtonConfig::Serialize(T& Stm)
{
	Stm
		.Process(this->Show)
		.Process(this->Action)
		.Process(this->Position)
		.Process(this->Size)
		.Process(this->Shape)
		.Process(this->RequiresBuildings)
		.Process(this->SuperWeaponIndex)
		.Process(this->SuperWeapon)
		.Process(this->Tooltip)
		.Process(this->Command)
		;
}

// -----------------------------------------------------------------------------
// TogglePowerButtonConfig
// -----------------------------------------------------------------------------

void TogglePowerButtonConfig::Read(CCINIClass* pINI, const char* pSection)
{
	if (!pINI) return;
	INI_EX exINI(pINI);

	this->Enabled.Read(exINI, pSection, "TogglePowerButton.Enabled");
	this->Position.Read(exINI, pSection, "TogglePowerButton.Position");

	if (!this->Position.isset())
	{
		Nullable<int> posX;
		Nullable<int> posY;
		posX.Read(exINI, pSection, "TogglePowerButton.X");
		posY.Read(exINI, pSection, "TogglePowerButton.Y");
		if (posX.isset() || posY.isset())
		{
			this->Position = Point2D { posX.Get(0), posY.Get(0) };
		}
	}

	this->Shape.Read(pINI, pSection, "TogglePowerButton.Shape", this->Shape.data());
	this->RequiresBuildings.Read(exINI, pSection, "TogglePowerButton.RequiresBuildings");
	this->Tooltip.Read(pINI, pSection, "TogglePowerButton.Tooltip", this->Tooltip.data());
}

void TogglePowerButtonConfig::Merge(const TogglePowerButtonConfig& other)
{
	this->Enabled = other.Enabled;
	if (other.Position.isset()) this->Position = other.Position;
	if (other.Shape[0] != '\0') this->Shape = other.Shape;
	if (other.RequiresBuildings.isset()) this->RequiresBuildings = other.RequiresBuildings;
	if (other.Tooltip[0] != '\0') this->Tooltip = other.Tooltip;
}

template <typename T>
void TogglePowerButtonConfig::Serialize(T& Stm)
{
	Stm
		.Process(this->Enabled)
		.Process(this->Position)
		.Process(this->Shape)
		.Process(this->RequiresBuildings)
		.Process(this->Tooltip)
		;
}

// -----------------------------------------------------------------------------
// CreditsConfig
// -----------------------------------------------------------------------------

void CreditsConfig::Read(CCINIClass* pINI, const char* pSection)
{
	if (!pINI) return;
	INI_EX exINI(pINI);

	this->Position.Read(exINI, pSection, "Credits.Position");
	if (!this->Position.isset())
	{
		Nullable<int> posX;
		Nullable<int> posY;
		posX.Read(exINI, pSection, "Credits.X");
		posY.Read(exINI, pSection, "Credits.Y");
		if (posX.isset() || posY.isset())
		{
			this->Position = Point2D { posX.Get(0), posY.Get(0) };
		}
	}

	this->Align.Read(exINI, pSection, "Credits.Align");
	this->Color.Read(exINI, pSection, "Credits.Color");
}

void CreditsConfig::Merge(const CreditsConfig& other)
{
	if (other.Position.isset()) this->Position = other.Position;
	if (other.Align.isset()) this->Align = other.Align;
	if (other.Color.isset()) this->Color = other.Color;
}

template <typename T>
void CreditsConfig::Serialize(T& Stm)
{
	Stm
		.Process(this->Position)
		.Process(this->Align)
		.Process(this->Color)
		;
}

// -----------------------------------------------------------------------------
// PowerBarConfig
// -----------------------------------------------------------------------------

void PowerBarConfig::Read(CCINIClass* pINI, const char* pSection)
{
	if (!pINI) return;
	INI_EX exINI(pINI);

	this->Show.Read(exINI, pSection, "PowerBar.Show");
	this->Position.Read(exINI, pSection, "PowerBar.Position");
	if (!this->Position.isset())
	{
		Nullable<int> posX;
		Nullable<int> posY;
		posX.Read(exINI, pSection, "PowerBar.X");
		posY.Read(exINI, pSection, "PowerBar.Y");
		if (posX.isset() || posY.isset())
		{
			this->Position = Point2D { posX.Get(0), posY.Get(0) };
		}
	}

	this->Height.Read(exINI, pSection, "PowerBar.Height");
	this->Shape.Read(pINI, pSection, "PowerBar.Shape");
}

void PowerBarConfig::Merge(const PowerBarConfig& other)
{
	if (other.Show.isset()) this->Show = other.Show;
	if (other.Position.isset()) this->Position = other.Position;
	if (other.Height.isset()) this->Height = other.Height;
	if (other.Shape[0] != '\0') this->Shape = other.Shape;
}

template <typename T>
void PowerBarConfig::Serialize(T& Stm)
{
	Stm
		.Process(this->Show)
		.Process(this->Position)
		.Process(this->Height)
		.Process(this->Shape)
		;
}

// -----------------------------------------------------------------------------
// TabsConfig
// -----------------------------------------------------------------------------

void TabsConfig::Read(CCINIClass* pINI, const char* pSection)
{
	if (!pINI) return;
	INI_EX exINI(pINI);

	this->Count.Read(exINI, pSection, "Tabs.Count");
	this->Rows.Read(exINI, pSection, "Tabs.Rows");
	this->Columns.Read(exINI, pSection, "Tabs.Columns");

	char orderBuf[0x80] = { 0 };
	if (pINI->ReadString(pSection, "Tabs.Order", "", orderBuf, sizeof(orderBuf)) && orderBuf[0] != '\0')
	{
		this->Order.clear();
		char* context = nullptr;
		char* token = strtok_s(orderBuf, ",", &context);
		while (token)
		{
			this->Order.push_back(atoi(token));
			token = strtok_s(nullptr, ",", &context);
		}
	}
}

void TabsConfig::Merge(const TabsConfig& other)
{
	if (other.Count.isset()) this->Count = other.Count;
	if (other.Rows.isset()) this->Rows = other.Rows;
	if (other.Columns.isset()) this->Columns = other.Columns;
	if (!other.Order.empty()) this->Order = other.Order;
	if (!other.Positions.empty()) this->Positions = other.Positions;
	if (!other.Shapes.empty()) this->Shapes = other.Shapes;
}

template <typename T>
void TabsConfig::Serialize(T& Stm)
{
	Stm
		.Process(this->Count)
		.Process(this->Rows)
		.Process(this->Columns)
		.Process(this->Order)
		.Process(this->Positions)
		.Process(this->Shapes)
		;
}

// -----------------------------------------------------------------------------
// CameosConfig
// -----------------------------------------------------------------------------

void CameosConfig::Read(CCINIClass* pINI, const char* pSection)
{
	if (!pINI) return;
	INI_EX exINI(pINI);
	this->Y.Read(exINI, pSection, "Cameos.Y");
}

void CameosConfig::Merge(const CameosConfig& other)
{
	if (other.Y.isset()) this->Y = other.Y;
}

template <typename T>
void CameosConfig::Serialize(T& Stm)
{
	Stm.Process(this->Y);
}

// -----------------------------------------------------------------------------
// SidebarConfig
// -----------------------------------------------------------------------------

void SidebarConfig::Read(CCINIClass* pINI, const char* pSection)
{
	if (!pINI) return;
	INI_EX exINI(pINI);

	this->RepairButton.Read(pINI, pSection, "RepairButton.");
	this->SellButton.Read(pINI, pSection, "SellButton.");
	this->DiplomacyButton.Read(pINI, pSection, "DiplomacyButton.");
	this->RadarButton.Read(pINI, pSection, "RadarButton.");
	this->DiplomacyButton.Merge(this->RadarButton);
	this->MenuButton.Read(pINI, pSection, "MenuButton.");
	this->TogglePowerButton.Read(pINI, pSection);
	this->Credits.Read(pINI, pSection);
	this->PowerBar.Read(pINI, pSection);
	this->Tabs.Read(pINI, pSection);
	this->Cameos.Read(pINI, pSection);

	char customBtnBuf[0x100] = { 0 };
	if (pINI->ReadString(pSection, "CustomButtons", "", customBtnBuf, sizeof(customBtnBuf)) && customBtnBuf[0] != '\0')
	{
		char* context = nullptr;
		char* token = strtok_s(customBtnBuf, ",", &context);
		while (token)
		{
			while (*token == ' ' || *token == '\t') token++;
			char* end = token + strlen(token) - 1;
			while (end > token && (*end == ' ' || *end == '\t')) *end-- = '\0';

			if (*token)
			{
				SidebarButtonConfig btnCfg;
				btnCfg.Read(pINI, token, nullptr);
				this->CustomButtons.emplace_back(FixedString<0x20>(token), btnCfg);
			}
			token = strtok_s(nullptr, ",", &context);
		}
	}
}

void SidebarConfig::Merge(const SidebarConfig& other)
{
	this->RepairButton.Merge(other.RepairButton);
	this->SellButton.Merge(other.SellButton);
	this->DiplomacyButton.Merge(other.DiplomacyButton);
	this->RadarButton.Merge(other.RadarButton);
	this->DiplomacyButton.Merge(other.RadarButton);
	this->MenuButton.Merge(other.MenuButton);
	this->TogglePowerButton.Merge(other.TogglePowerButton);
	this->Credits.Merge(other.Credits);
	this->PowerBar.Merge(other.PowerBar);
	this->Tabs.Merge(other.Tabs);
	this->Cameos.Merge(other.Cameos);

	for (const auto& btn : other.CustomButtons)
	{
		auto it = std::find_if(this->CustomButtons.begin(), this->CustomButtons.end(),
			[&](const std::pair<FixedString<0x20>, SidebarButtonConfig>& item) {
				return !_stricmp(item.first.data(), btn.first.data());
			});
		if (it != this->CustomButtons.end())
		{
			it->second.Merge(btn.second);
		}
		else
		{
			this->CustomButtons.push_back(btn);
		}
	}
}

template <typename T>
void SidebarConfig::Serialize(T& Stm)
{
	Stm
		.Process(this->RepairButton)
		.Process(this->SellButton)
		.Process(this->DiplomacyButton)
		.Process(this->RadarButton)
		.Process(this->MenuButton)
		.Process(this->TogglePowerButton)
		.Process(this->CustomButtons)
		.Process(this->Credits)
		.Process(this->PowerBar)
		.Process(this->Tabs)
		.Process(this->Cameos)
		;
}

// -----------------------------------------------------------------------------
// CustomSidebarButtonClass
// -----------------------------------------------------------------------------

CustomSidebarButtonClass::CustomSidebarButtonClass(const SidebarButtonConfig& cfg, int x, int y, int width, int height)
	: GadgetClass(x, y, width, height, (GadgetFlag::LeftPress | GadgetFlag::RightPress), false)
	, Config(cfg)
{
}

CustomSidebarButtonClass::~CustomSidebarButtonClass()
{
	if (this == Make_Global<GadgetClass*>(0x8B3E94))
		this->OnMouseLeave();
}

SHPStruct* CustomSidebarButtonClass::GetShape()
{
	if (!this->ShapeData && this->Config.Shape[0] != '\0')
	{
		this->ShapeData = FileSystem::LoadSHPFile(this->Config.Shape.data());
	}
	return this->ShapeData;
}

bool CustomSidebarButtonClass::IsDisabled() const
{
	if (this->Config.RequiresBuildings.Get(false))
	{
		const auto pPlayer = HouseClass::CurrentPlayer;
		if (!pPlayer || pPlayer->Buildings.Count <= 0)
			return true;
	}
	return false;
}

bool CustomSidebarButtonClass::Draw(bool forced)
{
	if (!this->Config.Show.Get(true))
		return false;

	const auto pShape = this->GetShape();
	if (!pShape)
		return false;

	if (this->Config.Action.Get(CustomButtonType::None) == CustomButtonType::TogglePower)
	{
		this->IsToggled = DisplayClass::Instance.PowerToggleMode;
	}

	const bool disabled = this->IsDisabled();
	int frame = 0;
	if (disabled)
		frame = (pShape->Frames > 2) ? 2 : 0;
	else if (this->IsPressed || this->IsToggled)
		frame = (pShape->Frames > 1) ? 1 : 0;
	else
		frame = 0;

	Point2D drawPos = { this->X, this->Y };
	RectangleStruct bounds = DSurface::Sidebar->GetRect();

	DSurface::Sidebar->DrawSHP(FileSystem::SIDEBAR_PAL, pShape, frame, &drawPos, &bounds,
		BlitterFlags::bf_400, 0, 0, ZGradient::Ground, 1000, 0, 0, 0, 0, 0);

	return true;
}

void CustomSidebarButtonClass::OnMouseEnter()
{
	this->IsHovering = true;
	MouseClass::Instance.UpdateCursor(MouseCursorType::Default, false);
}

void CustomSidebarButtonClass::OnMouseLeave()
{
	this->IsHovering = false;
	this->IsPressed = false;
	MouseClass::Instance.UpdateCursor(MouseCursorType::Default, false);
}

bool CustomSidebarButtonClass::Action(GadgetFlag flags, DWORD* pKey, KeyModifier modifier)
{
	if (!this->Config.Show.Get(true))
		return false;

	if (flags & GadgetFlag::RightPress)
	{
		if (this->Config.Action.Get(CustomButtonType::None) == CustomButtonType::TogglePower)
		{
			if (DisplayClass::Instance.PowerToggleMode)
			{
				reinterpret_cast<void(__thiscall*)(DisplayClass*, int)>(0x4AC820)(&DisplayClass::Instance, 0);
				this->IsToggled = false;
				SidebarClass::Instance.SidebarNeedsRedraw = true;
			}
		}
	}

	if (flags & GadgetFlag::LeftPress)
	{
		if (!this->IsDisabled())
			this->IsPressed = true;
	}

	if ((flags & GadgetFlag::LeftRelease) && this->IsPressed)
	{
		this->IsPressed = false;
		if (!this->IsDisabled())
		{
			this->ExecuteAction();
		}
	}

	this->GadgetClass::Action(flags, pKey, KeyModifier::None);
	return true;
}

void CustomSidebarButtonClass::ExecuteAction()
{
	const auto actionType = this->Config.Action.Get(CustomButtonType::None);

	switch (actionType)
	{
	case CustomButtonType::TogglePower:
		reinterpret_cast<void(__thiscall*)(DisplayClass*, int)>(0x4AC820)(&DisplayClass::Instance, -1);
		this->IsToggled = DisplayClass::Instance.PowerToggleMode;
		SidebarClass::Instance.SidebarNeedsRedraw = true;
		break;

	case CustomButtonType::Repair:
		reinterpret_cast<void(__thiscall*)(DisplayClass*, int)>(0x4AC8E0)(&DisplayClass::Instance, -1);
		SidebarClass::Instance.SidebarNeedsRedraw = true;
		break;

	case CustomButtonType::Sell:
		reinterpret_cast<void(__thiscall*)(DisplayClass*, int)>(0x4AC9A0)(&DisplayClass::Instance, -1);
		SidebarClass::Instance.SidebarNeedsRedraw = true;
		break;

	case CustomButtonType::SuperWeapon:
	{
		int swIdx = this->Config.SuperWeaponIndex.Get(-1);
		if (swIdx < 0 && this->Config.SuperWeapon[0] != '\0')
		{
			swIdx = SuperWeaponTypeClass::FindIndex(this->Config.SuperWeapon.data());
		}

		if (swIdx >= 0 && HouseClass::CurrentPlayer && HouseClass::CurrentPlayer->Supers.ValidIndex(swIdx))
		{
			auto pSuper = HouseClass::CurrentPlayer->Supers[swIdx];
			if (pSuper && pSuper->IsPresent && pSuper->IsReady)
			{
				auto pType = pSuper->Type;
				if (pType->Action == Action::None)
				{
					EventClass::OutList.Add(EventClass(HouseClass::CurrentPlayer->ArrayIndex, EventType::SpecialPlace, swIdx, CellStruct { 0, 0 }));
				}
				else
				{
					DisplayClass::Instance.SetActiveFoundation(nullptr);
					DisplayClass::Instance.CurrentBuilding = nullptr;
					DisplayClass::Instance.CurrentBuildingType = nullptr;
					DisplayClass::Instance.CurrentBuildingOwnerArrayIndex = -1;
					MapClass::Instance.SetRepairMode(0);
					MapClass::Instance.SetSellMode(0);
					DisplayClass::Instance.PowerToggleMode = false;
					DisplayClass::Instance.PlanningMode = false;
					DisplayClass::Instance.PlaceBeaconMode = false;
					DisplayClass::Instance.CurrentSWTypeIndex = swIdx;
				}
			}
		}
		break;
	}

	case CustomButtonType::Command:
	{
		if (this->Config.Command[0] != '\0')
		{
			for (auto const pCmd : CommandClass::Array)
			{
				if (pCmd && !_stricmp(pCmd->GetName(), this->Config.Command.data()))
				{
					pCmd->Execute(static_cast<WWKey>(0));
					break;
				}
			}
		}
		break;
	}

	default:
		break;
	}
}

// -----------------------------------------------------------------------------
// SidebarExt Methods
// -----------------------------------------------------------------------------

SidebarConfig SidebarExt::ActiveConfig()
{
	SidebarConfig result = GlobalConfig;
	result.Merge(RulesConfig);

	if (HouseClass::CurrentPlayer)
	{
		const auto pSide = SideClass::Array.GetItemOrDefault(HouseClass::CurrentPlayer->SideIndex);
		if (const auto pSideExt = SideExt::TryFetch(pSide))
		{
			result.Merge(pSideExt->SidebarSettings);
		}
	}

	result.Merge(ScenarioConfig);
	return result;
}

void SidebarExt::LoadFromUIMD(CCINIClass& ini_uimd)
{
	if (ini_uimd.GetSection(SIDEBAR_SECTION))
	{
		GlobalConfig.Read(&ini_uimd, SIDEBAR_SECTION);
	}
}

void SidebarExt::LoadFromRules(CCINIClass* pINI)
{
	if (!pINI) return;
	if (pINI->GetSection("Sidebar"))
	{
		RulesConfig.Read(pINI, "Sidebar");
	}
}

void SidebarExt::LoadFromScenario(CCINIClass* pINI)
{
	if (!pINI) return;
	if (pINI->GetSection("Sidebar"))
	{
		ScenarioConfig.Read(pINI, "Sidebar");
	}
}

void SidebarExt::SaveBaseline()
{
	BaselineGlobalConfig = GlobalConfig;
	BaselineGlobalConfig.Merge(RulesConfig);
}

void SidebarExt::ResetToBaseline()
{
	ScenarioConfig = SidebarConfig();
}

void SidebarExt::InitClear()
{
	for (auto pBtn : ActiveCustomButtons)
	{
		if (pBtn)
		{
			GScreenClass::Instance.RemoveButton(pBtn);
			GameDelete(pBtn);
		}
	}
	ActiveCustomButtons.clear();

	if (ActiveTogglePowerButton)
	{
		GScreenClass::Instance.RemoveButton(ActiveTogglePowerButton);
		GameDelete(ActiveTogglePowerButton);
		ActiveTogglePowerButton = nullptr;
	}
}

void SidebarExt::InitIO()
{
	InitClear();

	const auto config = ActiveConfig();

	// Create TogglePowerButton if enabled
	if (config.TogglePowerButton.Enabled.Get())
	{
		SidebarButtonConfig tpCfg;
		tpCfg.Show = true;
		tpCfg.Action = CustomButtonType::TogglePower;
		tpCfg.Shape = config.TogglePowerButton.Shape;
		tpCfg.RequiresBuildings = config.TogglePowerButton.RequiresBuildings;
		tpCfg.Tooltip = config.TogglePowerButton.Tooltip;

		Point2D pos = config.TogglePowerButton.Position.Get(Point2D { 0, 0 });
		if (!config.TogglePowerButton.Position.isset())
		{
			DWORD sellX = *reinterpret_cast<DWORD*>(0xB07E04);
			DWORD sellY = *reinterpret_cast<DWORD*>(0xB07E08);
			pos.X = static_cast<int>(sellX) + 24;
			pos.Y = static_cast<int>(sellY);
		}

		ActiveTogglePowerButton = GameCreate<CustomSidebarButtonClass>(tpCfg, pos.X, pos.Y, 24, 24);
		GScreenClass::Instance.AddButton(ActiveTogglePowerButton);
	}

	// Create Custom Buttons
	for (const auto& pair : config.CustomButtons)
	{
		const auto& btnCfg = pair.second;
		if (btnCfg.Show.Get(true))
		{
			Point2D pos = btnCfg.Position.Get(Point2D { 0, 0 });
			Point2D sz = btnCfg.Size.Get(Point2D { 24, 24 });
			auto pBtn = GameCreate<CustomSidebarButtonClass>(btnCfg, pos.X, pos.Y, sz.X, sz.Y);
			ActiveCustomButtons.push_back(pBtn);
			GScreenClass::Instance.AddButton(pBtn);
		}
	}
}

// Reversed from Ares source code (In fact, it's the same as Vanilla).
bool __stdcall SidebarExt::AresTabCameo_RemoveCameo(BuildType* pItem)
{
	const auto pTechnoType = TechnoTypeClass::GetByTypeAndIndex(pItem->ItemType, pItem->ItemIndex);
	const auto pCurrent = HouseClass::CurrentPlayer;

	if (pTechnoType)
	{
		const auto pFactory = pTechnoType->FindFactory(true, false, false, pCurrent);

		if (pFactory && pCurrent->CanBuild(pTechnoType, false, true) != CanBuildResult::Unbuildable)
			return false;
	}
	else
	{
		const auto& supers = pCurrent->Supers;

		if (supers.ValidIndex(pItem->ItemIndex) && supers[pItem->ItemIndex]->IsPresent)
		{
			if (SWSidebarClass::Instance.AddButton(pItem->ItemIndex))
				ScenarioExt::Global()->SWSidebar_Indices.emplace_back(pItem->ItemIndex);
			else
				return false;
		}
	}

	auto buildCat = BuildCat::DontCare;

	if (pItem->ItemType == AbstractType::BuildingType || pItem->ItemType == AbstractType::Building)
	{
		__assume(pTechnoType != nullptr);
		buildCat = static_cast<BuildingTypeClass*>(pTechnoType)->BuildCat;
		auto& pDisplay = DisplayClass::Instance;
		pDisplay.SetActiveFoundation(nullptr);
		pDisplay.CurrentBuilding = nullptr;
		pDisplay.CurrentBuildingType = nullptr;
		pDisplay.CurrentBuildingOwnerArrayIndex = -1;
	}

	if (pTechnoType && pCurrent->GetPrimaryFactory(pItem->ItemType, pTechnoType->Naval, buildCat))
	{
		EventClass::OutList.Add(EventClass(
			pCurrent->ArrayIndex,
			EventType::AbandonAll,
			static_cast<int>(pItem->ItemType),
			pItem->ItemIndex,
			pTechnoType->Naval
		));
	}

	return true;
}

// -----------------------------------------------------------------------------
// Serialization
// -----------------------------------------------------------------------------

template <typename T>
void SidebarExt::ExtData::Serialize(T& Stm)
{
	Stm
		.Process(SidebarExt::ScenarioConfig)
		;
}

void SidebarExt::ExtData::LoadFromStream(PhobosStreamReader& Stm)
{
	Extension<SidebarClass>::LoadFromStream(Stm);
	this->Serialize(Stm);
}

void SidebarExt::ExtData::SaveToStream(PhobosStreamWriter& Stm)
{
	Extension<SidebarClass>::SaveToStream(Stm);
	this->Serialize(Stm);
}

// -----------------------------------------------------------------------------
// Container Hooks
// -----------------------------------------------------------------------------

DEFINE_HOOK(0x6A4F0B, SidebarClass_CTOR, 0x5)
{
	GET(SidebarClass*, pItem, EAX);

	SidebarExt::Allocate(pItem);

	return 0;
}

DEFINE_HOOK(0x6AC82F, SidebarClass_DTOR, 0x5)
{
	GET(SidebarClass*, pItem, EBX);

	SidebarExt::Remove(pItem);
	return 0;
}

IStream* SidebarExt::g_pStm = nullptr;

DEFINE_HOOK_AGAIN(0x6AC5D0, SidebarClass_SaveLoad_Prefix, 0x5)
DEFINE_HOOK(0x6AC5E0, SidebarClass_SaveLoad_Prefix, 0x5)
{
	GET_STACK(IStream*, pStm, 0x4);

	SidebarExt::g_pStm = pStm;

	return 0;
}

DEFINE_HOOK(0x6AC5DA, SidebarClass_Load_Suffix, 0x6)
{
	auto buffer = SidebarExt::Global();

	PhobosByteStream Stm(0);
	if (Stm.ReadBlockFromStream(SidebarExt::g_pStm))
	{
		PhobosStreamReader Reader(Stm);

		if (Reader.Expect(SidebarExt::Canary) && Reader.RegisterChange(buffer))
			buffer->LoadFromStream(Reader);
	}

	return 0;
}

DEFINE_HOOK(0x6AC5EA, SidebarClass_Save_Suffix, 0x6)
{
	auto buffer = SidebarExt::Global();
	PhobosByteStream saver(sizeof(*buffer));
	PhobosStreamWriter writer(saver);

	writer.Expect(SidebarExt::Canary);
	writer.RegisterChange(buffer);

	buffer->SaveToStream(writer);
	saver.WriteBlockToStream(SidebarExt::g_pStm);

	return 0;
}

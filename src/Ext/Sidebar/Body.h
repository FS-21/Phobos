#pragma once

#include <SidebarClass.h>
#include <GadgetClass.h>

#include <Utilities/Container.h>
#include <Utilities/TemplateDef.h>
#include <Utilities/Constructs.h>

enum class CustomButtonType : int
{
	None = 0,
	Repair = 1,
	Sell = 2,
	TogglePower = 3,
	SuperWeapon = 4,
	Command = 5,
	Custom = 6
};

class SidebarButtonConfig
{
public:
	Nullable<bool> Show;
	Nullable<CustomButtonType> Action;
	Nullable<Point2D> Position;
	Nullable<Point2D> Size;
	PhobosFixedString<0x20> Shape;
	Nullable<bool> RequiresBuildings;
	Nullable<int> SuperWeaponIndex;
	PhobosFixedString<0x20> SuperWeapon;
	PhobosFixedString<0x40> Tooltip;
	PhobosFixedString<0x40> Command;

	SidebarButtonConfig() = default;

	void Read(CCINIClass* pINI, const char* pSection, const char* pPrefix = nullptr);
	void Merge(const SidebarButtonConfig& other);

	template <typename T>
	void Serialize(T& Stm);
};

class TogglePowerButtonConfig
{
public:
	Nullable<bool> Enabled;
	Nullable<Point2D> Position;
	PhobosFixedString<0x20> Shape;
	Nullable<bool> RequiresBuildings;
	PhobosFixedString<0x40> Tooltip;

	TogglePowerButtonConfig() = default;

	void Read(CCINIClass* pINI, const char* pSection);
	void Merge(const TogglePowerButtonConfig& other);

	template <typename T>
	void Serialize(T& Stm);
};

class CreditsConfig
{
public:
	Nullable<Point2D> Position;
	Nullable<TextAlign> Align;
	Nullable<ColorStruct> Color;

	CreditsConfig() = default;

	void Read(CCINIClass* pINI, const char* pSection);
	void Merge(const CreditsConfig& other);

	template <typename T>
	void Serialize(T& Stm);
};

class PowerBarConfig
{
public:
	Nullable<bool> Show;
	Nullable<Point2D> Position;
	Nullable<int> Height;
	PhobosFixedString<0x20> Shape;

	PowerBarConfig() = default;

	void Read(CCINIClass* pINI, const char* pSection);
	void Merge(const PowerBarConfig& other);

	template <typename T>
	void Serialize(T& Stm);
};

class TabsConfig
{
public:
	Nullable<int> Count;
	Nullable<int> Rows;
	Nullable<int> Columns;
	std::vector<int> Order;
	std::vector<Point2D> Positions;
	std::vector<PhobosFixedString<0x20>> Shapes;

	TabsConfig() = default;

	void Read(CCINIClass* pINI, const char* pSection);
	void Merge(const TabsConfig& other);

	template <typename T>
	void Serialize(T& Stm);
};

class CameosConfig
{
public:
	Nullable<int> Y;
	Nullable<int> Height;
	Nullable<int> MarginBottom;

	CameosConfig() = default;

	void Read(CCINIClass* pINI, const char* pSection);
	void Merge(const CameosConfig& other);

	template <typename T>
	void Serialize(T& Stm);
};

class SidebarConfig
{
public:
	SidebarButtonConfig RepairButton;
	SidebarButtonConfig SellButton;
	SidebarButtonConfig DiplomacyButton;
	SidebarButtonConfig RadarButton;
	SidebarButtonConfig MenuButton;
	TogglePowerButtonConfig TogglePowerButton;
	std::vector<std::pair<FixedString<0x20>, SidebarButtonConfig>> CustomButtons;
	CreditsConfig Credits;
	PowerBarConfig PowerBar;
	TabsConfig Tabs;
	CameosConfig Cameos;
	SidebarButtonConfig ScrollUpButton;
	SidebarButtonConfig ScrollDownButton;

	SidebarConfig() = default;

	void Read(CCINIClass* pINI, const char* pSection);
	void Merge(const SidebarConfig& other);

	template <typename T>
	void Serialize(T& Stm);
};

class CustomSidebarButtonClass : public GadgetClass
{
public:
	SidebarButtonConfig Config;
	SHPStruct* ShapeData { nullptr };
	bool IsHovering { false };
	bool IsPressed { false };
	bool IsToggled { false };

	CustomSidebarButtonClass(const SidebarButtonConfig& cfg, int x, int y, int width, int height);
	virtual ~CustomSidebarButtonClass();

	virtual bool Draw(bool forced) override;
	virtual void OnMouseEnter() override;
	virtual void OnMouseLeave() override;
	virtual bool Action(GadgetFlag flags, DWORD* pKey, KeyModifier modifier) override;

	bool IsDisabled() const;
	void ExecuteAction();
	SHPStruct* GetShape();
};

class SidebarExt
{
public:
	using base_type = SidebarClass;

	static constexpr DWORD Canary = 0x51DEBA12;

	class ExtData final : public Extension<SidebarClass>
	{
	public:
		ExtData(SidebarClass* OwnerObject) : Extension<SidebarClass>(OwnerObject)
		{ }

		virtual ~ExtData() = default;

		virtual void LoadFromStream(PhobosStreamReader& Stm) override;
		virtual void SaveToStream(PhobosStreamWriter& Stm) override;
	private:
		template <typename T>
		void Serialize(T& Stm);
	};

private:
	static std::unique_ptr<ExtData> Data;

public:
	static IStream* g_pStm;

	static SHPStruct* TabProducingProgress[16];

	static SidebarConfig GlobalConfig;
	static SidebarConfig RulesConfig;
	static SidebarConfig ScenarioConfig;
	static SidebarConfig BaselineGlobalConfig;

	static std::vector<CustomSidebarButtonClass*> ActiveCustomButtons;
	static CustomSidebarButtonClass* ActiveTogglePowerButton;

	static void Allocate(SidebarClass* pThis);
	static void Remove(SidebarClass* pThis);

	static ExtData* Global()
	{
		return Data.get();
	}

	static void Clear()
	{
		Allocate(&SidebarClass::Instance);
	}

	static SidebarConfig ActiveConfig();

	static void LoadFromUIMD(CCINIClass& ini_uimd);
	static void LoadFromRules(CCINIClass* pINI);
	static void LoadFromScenario(CCINIClass* pINI);

	static void InitClear();
	static void InitIO();

	static void SaveBaseline();
	static void ResetToBaseline();

	static Point2D ResolveCoord(Point2D pos, DWORD sidebarX)
	{
		if (pos.X < 168 && pos.X >= 0)
		{
			pos.X += static_cast<int>(sidebarX);
		}
		return pos;
	}

	static bool __stdcall AresTabCameo_RemoveCameo(BuildType* pItem);
};

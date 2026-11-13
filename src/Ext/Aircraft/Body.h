#pragma once
#include <Ext/Foot/Body.h>
#include <Ext/AircraftType/Body.h>
#include <AircraftClass.h>

// Concrete leaf extension for AircraftClass.
class AircraftExt final : public FootExt
{
public:
	using base_type = AircraftClass;

	static constexpr DWORD Canary = 0xA1A2A3A4;

	int Strafe_BombsDroppedThisRound;
	CellClass* Strafe_TargetCell;
	int CurrentAircraftWeaponIndex;
	float CurrentRoll;

	explicit AircraftExt(AircraftClass* const OwnerObject) : FootExt(OwnerObject)
		, Strafe_BombsDroppedThisRound { 0 }
		, Strafe_TargetCell { nullptr }
		, CurrentAircraftWeaponIndex {}
		, CurrentRoll { 0.0f }
	{ }

	AircraftClass* OwnerObject() const
	{
		return static_cast<AircraftClass*>(this->GetAttachedObject());
	}

	// an aircraft's type extension is always the AircraftTypeExt leaf
	AircraftTypeExt* GetTypeExtData() const
	{
		return static_cast<AircraftTypeExt*>(this->TypeExtData);
	}

	static void FireWeapon(AircraftClass* pThis, AbstractClass* pTarget);
	static bool CanSelfReload(AircraftClass const* pThis);
	static bool ShouldWaitForReload(AircraftClass const* pThis);
	static int GetSelectedWeaponAmmoCost(AircraftClass* pThis, bool isStrafeLocked = false);
	static bool CanFireWithCurrentAmmo(AircraftClass* pThis, bool isStrafeLocked = false);
	static bool CanUseAnyWeaponWithCurrentAmmo(AircraftClass const* pThis);
	static bool CanContinueAttack(AircraftClass* pThis, bool isStrafeLocked = false);
	static void ConsumeAmmo(AircraftClass* pThis, int amount = -1);
	static bool PlaceReinforcementAircraft(AircraftClass* pThis, CoordStruct edgeCoords);
	static CellStruct PickEdgeCellForPlane(AircraftTypeClass* pPlaneType, CellStruct destCell, Edge edge, bool isOnRetreat = false);
	static DirType GetLandingDir(AircraftClass* pThis, BuildingClass* pDock = nullptr, bool isProduction = false);
	static AircraftTypeClass* GetAircraftTypeExtra(AircraftClass* pAircraft);

	void UpdateRoll();
	void UpdateReload();

	class ExtContainer final : public Container<AircraftExt>
	{
	public:
		ExtContainer();
		~ExtContainer();
	};

	static ExtContainer ExtMap;

	static AircraftExt* Fetch(const AircraftClass* pThis)
	{
		return AbstractExt::Fetch<AircraftExt>(pThis);
	}

	static AircraftExt* TryFetch(const AircraftClass* pThis)
	{
		return AbstractExt::TryFetch<AircraftExt>(pThis);
	}

	virtual void LoadFromStream(PhobosStreamReader& Stm) override;
	virtual void SaveToStream(PhobosStreamWriter& Stm) override;

private:
	template <typename T>
	void Serialize(T& Stm);
};

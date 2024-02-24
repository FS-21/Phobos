#pragma once
#include <InfantryClass.h>
#include <AnimClass.h>

#include <Helpers/Macro.h>
#include <Utilities/Container.h>
#include <Utilities/TemplateDef.h>
#include <Utilities/Macro.h>
#include <Utilities/EnumFunctions.h>
#include <New/Entity/ShieldClass.h>
#include <New/Entity/LaserTrailClass.h>

struct MapPathCellElement
{
	int Distance = -1;
	int X = -1;
	int Y = -1;

	//need to define a == operator so it can be used in array classes
	bool operator==(const MapPathCellElement& other) const
	{
		return (X == other.X && Y == other.Y);
	}

	//unequality
	bool operator!=(const MapPathCellElement& other) const
	{
		return (X != other.X || Y != other.Y);
	}

	bool operator<(const MapPathCellElement& other) const
	{
		return (Distance < other.Distance);
	}

	bool operator>(const MapPathCellElement& other) const
	{
		return (Distance > other.Distance);
	}

	CellStruct ToCellStruct() const
	{
		CellStruct c;
		c.X = (short)X;
		c.Y = (short)Y;
		return c;
	}
};

class BulletClass;

class TechnoExt
{
public:
	using base_type = TechnoClass;

	static constexpr DWORD Canary = 0x55555555;
	static constexpr size_t ExtPointerOffset = 0x34C;

	class ExtData final : public Extension<TechnoClass>
	{
	public:
		TechnoTypeExt::ExtData* TypeExtData;
		std::unique_ptr<ShieldClass> Shield;
		std::vector<LaserTrailClass> LaserTrails;
		bool ReceiveDamage;
		bool LastKillWasTeamTarget;
		CDTimerClass PassengerDeletionTimer;
		ShieldTypeClass* CurrentShieldType;
		int LastWarpDistance;
		CDTimerClass AutoDeathTimer;
		AnimTypeClass* MindControlRingAnimType;
		bool IsLeggedCyborg;
		OptionalStruct<int, false> DamageNumberOffset;
		bool IsInTunnel;
		bool HasBeenPlacedOnMap; // Set to true on first Unlimbo() call.
		CDTimerClass DeployFireTimer;
		bool ForceFullRearmDelay;
		int WHAnimRemainingCreationInterval;
		AbstractClass* OriginalTarget;
		TechnoClass* CurrentRandomTarget;
		bool DelayedFire_Charging;
		bool DelayedFire_Charged;
		AnimClass* DelayedFire_Anim;
		AnimClass* DelayedFire_PostAnim;
		int DelayedFire_Duration;
		int DelayedFire_WeaponIndex;
		CDTimerClass DelayedFire_DurationTimer;

		// Used for Passengers.SyncOwner.RevertOnExit instead of TechnoClass::InitialOwner / OriginallyOwnedByHouse,
		// as neither is guaranteed to point to the house the TechnoClass had prior to entering transport and cannot be safely overridden.
		HouseClass* OriginalPassengerOwner;

		int	WebbyDurationCountDown;
		CDTimerClass WebbyDurationTimer;
		AnimClass* WebbyAnim;
		AbstractClass* WebbyLastTarget;
		Mission WebbyLastMission;

		AnimClass* DeployAnim;
		bool Convert_UniversalDeploy_InProgress;
		bool Convert_UniversalDeploy_MakeInvisible;
		TechnoClass* Convert_UniversalDeploy_TemporalTechno;
		bool Convert_UniversalDeploy_ForceRedraw;
		bool Convert_UniversalDeploy_IsOriginalDeployer;
		AbstractClass* Convert_UniversalDeploy_RememberTarget;
		int Convert_UniversalDeploy_SelectedIdx;

		ExtData(TechnoClass* OwnerObject) : Extension<TechnoClass>(OwnerObject)
			, TypeExtData { nullptr }
			, Shield {}
			, LaserTrails {}
			, ReceiveDamage { false }
			, LastKillWasTeamTarget { false }
			, PassengerDeletionTimer {}
			, CurrentShieldType { nullptr }
			, LastWarpDistance {}
			, AutoDeathTimer {}
			, MindControlRingAnimType { nullptr }
			, DamageNumberOffset {}
			, OriginalPassengerOwner {}
			, IsLeggedCyborg { false }
			, IsInTunnel { false }
			, HasBeenPlacedOnMap { false }
			, DeployFireTimer {}
			, ForceFullRearmDelay { false }
			, WHAnimRemainingCreationInterval { 0 }
			, WebbyDurationCountDown { -1 }
			, WebbyDurationTimer {}
			, WebbyAnim { nullptr }
			, WebbyLastTarget { nullptr }
			, WebbyLastMission { Mission::Sleep }
			, OriginalTarget { nullptr }
			, CurrentRandomTarget { nullptr }
			, DelayedFire_Charging { false }
			, DelayedFire_Charged { false }
			, DelayedFire_Anim { nullptr }
			, DelayedFire_PostAnim { nullptr }
			, DelayedFire_Duration { -1 }
			, DelayedFire_WeaponIndex { -1 }
			, DelayedFire_DurationTimer {}
			, DeployAnim { nullptr }
			, Convert_UniversalDeploy_InProgress { false }
			, Convert_UniversalDeploy_MakeInvisible { false }
			, Convert_UniversalDeploy_TemporalTechno { nullptr }
			, Convert_UniversalDeploy_ForceRedraw { false }
			, Convert_UniversalDeploy_IsOriginalDeployer { true }
			, Convert_UniversalDeploy_RememberTarget { nullptr }
			, Convert_UniversalDeploy_SelectedIdx { -1 }
		{ }

		void ApplyInterceptor();
		bool CheckDeathConditions(bool isInLimbo = false);
		void DepletedAmmoActions();
		void EatPassengers();
		void UpdateShield();
		void UpdateOnTunnelEnter();
		void ApplySpawnLimitRange();
		void UpdateTypeData(TechnoTypeClass* currentType);
		void UpdateLaserTrails();
		void InitializeLaserTrails();
		void UpdateMindControlAnim();
		void WebbyUpdate();
		void UpdateDelayFire();
		void RefreshRandomTargets();

		virtual ~ExtData() override;

		virtual void InvalidatePointer(void* ptr, bool bRemoved) override
		{
			AnnounceInvalidPointer(OriginalPassengerOwner, ptr);
			AnnounceInvalidPointer(WebbyLastTarget, ptr);
			AnnounceInvalidPointer(DelayedFire_Anim, ptr);
			AnnounceInvalidPointer(DelayedFire_PostAnim, ptr);
			AnnounceInvalidPointer(CurrentRandomTarget, ptr);
			AnnounceInvalidPointer(OriginalTarget, ptr);
			AnnounceInvalidPointer(Convert_UniversalDeploy_TemporalTechno, ptr);
			AnnounceInvalidPointer(DeployAnim, ptr);
			AnnounceInvalidPointer(Convert_UniversalDeploy_RememberTarget, ptr);
		}

		virtual void LoadFromStream(PhobosStreamReader& Stm) override;
		virtual void SaveToStream(PhobosStreamWriter& Stm) override;

	private:
		template <typename T>
		void Serialize(T& Stm);
	};

	class ExtContainer final : public Container<TechnoExt>
	{
	public:
		ExtContainer();
		~ExtContainer();

		virtual bool InvalidateExtDataIgnorable(void* const ptr) const override
		{
			auto const abs = static_cast<AbstractClass*>(ptr)->WhatAmI();

			switch (abs)
			{
			case AbstractType::House:
				return false;
			}

			return true;
		}
	};

	static ExtContainer ExtMap;

	static bool LoadGlobals(PhobosStreamReader& Stm);
	static bool SaveGlobals(PhobosStreamWriter& Stm);

	static bool IsActive(TechnoClass* pThis);

	static bool IsHarvesting(TechnoClass* pThis);
	static bool HasAvailableDock(TechnoClass* pThis);

	static CoordStruct GetFLHAbsoluteCoords(TechnoClass* pThis, CoordStruct flh, bool turretFLH = false);

	static CoordStruct GetBurstFLH(TechnoClass* pThis, int weaponIndex, bool& FLHFound);
	static CoordStruct GetSimpleFLH(InfantryClass* pThis, int weaponIndex, bool& FLHFound);

	static void ChangeOwnerMissionFix(FootClass* pThis);
	static void KillSelf(TechnoClass* pThis, AutoDeathBehavior deathOption, AnimTypeClass* pVanishAnimation, bool isInLimbo = false);
	static void TransferMindControlOnDeploy(TechnoClass* pTechnoFrom, TechnoClass* pTechnoTo);
	static void ApplyMindControlRangeLimit(TechnoClass* pThis);
	static void ObjectKilledBy(TechnoClass* pThis, TechnoClass* pKiller);
	static void UpdateSharedAmmo(TechnoClass* pThis);
	static double GetCurrentSpeedMultiplier(FootClass* pThis);
	static void DisplayDamageNumberString(TechnoClass* pThis, int damage, bool isShieldDamage);
	static void DrawSelfHealPips(TechnoClass* pThis, Point2D* pLocation, RectangleStruct* pBounds);
	static void DrawInsignia(TechnoClass* pThis, Point2D* pLocation, RectangleStruct* pBounds);
	static void ApplyGainedSelfHeal(TechnoClass* pThis);
	static void SyncIronCurtainStatus(TechnoClass* pFrom, TechnoClass* pTo);
	static CoordStruct PassengerKickOutLocation(TechnoClass* pThis, FootClass* pPassenger, int maxAttempts = 1);
	static bool AllowedTargetByZone(TechnoClass* pThis, TechnoClass* pTarget, TargetZoneScanType zoneScanType, WeaponTypeClass* pWeapon = nullptr, bool useZone = false, int zone = -1);
	static void UpdateAttachedAnimLayers(TechnoClass* pThis);
	static bool ConvertToType(FootClass* pThis, TechnoTypeClass* toType);
	static bool CanDeployIntoBuilding(UnitClass* pThis, bool noDeploysIntoDefaultValue = false);
	static bool CanDeployIntoBuilding(BuildingClass* pThis, bool noDeploysIntoDefaultValue = false, BuildingTypeClass* pBuildingType = nullptr);
	static bool IsTypeImmune(TechnoClass* pThis, TechnoClass* pSource);
	static void RemoveParasite(TechnoClass* pThis, HouseClass* sourceHouse, WarheadTypeClass* wh);
	static void WebbyUpdate(TechnoClass* pThis);
	static bool UpdateRandomTarget(TechnoClass* pThis = nullptr);
	static TechnoClass* GetRandomTarget(TechnoClass* pThis = nullptr);

	// WeaponHelpers.cpp
	static int PickWeaponIndex(TechnoClass* pThis, TechnoClass* pTargetTechno, AbstractClass* pTarget, int weaponIndexOne, int weaponIndexTwo, bool allowFallback = true, bool allowAAFallback = true);
	static void FireWeaponAtSelf(TechnoClass* pThis, WeaponTypeClass* pWeaponType);
	static bool CanFireNoAmmoWeapon(TechnoClass* pThis, int weaponIndex);
	static WeaponTypeClass* GetDeployFireWeapon(TechnoClass* pThis, int& weaponIndex);
	static WeaponTypeClass* GetDeployFireWeapon(TechnoClass* pThis);
	static WeaponTypeClass* GetCurrentWeapon(TechnoClass* pThis, int& weaponIndex, bool getSecondary = false);
	static WeaponTypeClass* GetCurrentWeapon(TechnoClass* pThis, bool getSecondary = false);
	static Point2D GetScreenLocation(TechnoClass* pThis);
	static Point2D GetFootSelectBracketPosition(TechnoClass* pThis, Anchor anchor);
	static Point2D GetBuildingSelectBracketPosition(TechnoClass* pThis, BuildingSelectBracketPosition bracketPosition);
	static void ProcessDigitalDisplays(TechnoClass* pThis);
	static void GetValuesForDisplay(TechnoClass* pThis, DisplayInfoType infoType, int& value, int& maxValue);

	static TechnoClass* UniversalDeployConversion(TechnoClass* pThis, TechnoTypeClass* pNewType = nullptr);
	static void RunStructureIntoTechnoConversion(TechnoClass* pOld, TechnoTypeClass* pNewType = nullptr);
	static void RunTechnoIntoStructureConversion(TechnoClass* pOld, TechnoTypeClass* pNewType = nullptr);
	static void CreateUniversalDeployAnimation(TechnoClass* pThis);
	static bool Techno2TechnoPropertiesTransfer(TechnoClass* pNew = nullptr, TechnoClass* pOld = nullptr);
	static void UpdateUniversalDeploy(TechnoClass* pThis);
	static void PassengersTransfer(TechnoClass* pTechnoFrom, TechnoClass* pTechnoTo);
};

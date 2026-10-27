#include "SmartAutoDeploy.h"

#include <TechnoClass.h>
#include <UnitClass.h>
#include <BuildingClass.h>
#include <FootClass.h>
#include <InfantryClass.h>
#include <AircraftClass.h>
#include <MapClass.h>
#include <ScenarioClass.h>
#include <TeamClass.h>
#include <JumpjetLocomotionClass.h>
#include <VocClass.h>

#include <Ext/Techno/Body.h>
#include <Ext/TechnoType/Body.h>
#include <Ext/Unit/Body.h>
#include <Ext/Building/Body.h>
#include <New/Entity/ShieldClass.h>
#include <Utilities/GeneralUtils.h>

// ═══════════════════════════════════════════════════════════════════════════
// Section 1: Internal Utility Helpers (anonymous namespace)
// ═══════════════════════════════════════════════════════════════════════════

namespace
{
	// ── General Utilities ────────────────────────────────────────────────

	enum class TargetDomain
	{
		Ground,
		Air,
		Naval,
		Submerged
	};

	bool IsAliveAndValid(AbstractClass* pTarget)
	{
		if (!pTarget)
			return false;

		if (auto const pTechno = abstract_cast<TechnoClass*>(pTarget))
		{
			if (TechnoClass::Array.FindItemIndex(pTechno) == -1)
				return false;

			if (!pTechno->IsAlive || pTechno->InLimbo)
				return false;

			return true;
		}

		if (auto const pObj = abstract_cast<ObjectClass*>(pTarget))
		{
			if (!pObj->IsAlive || pObj->InLimbo)
				return false;

			return true;
		}

		if (pTarget->WhatAmI() == AbstractType::Cell)
		{
			const auto pCell = static_cast<CellClass*>(pTarget);
			return MapClass::Instance.IsWithinUsableArea(pCell->GetCoords());
		}

		return true;
	}

	bool IsTechnoAirborne(TechnoClass* pTechno)
	{
		if (!pTechno)
			return false;

		if (pTechno->InAir || pTechno->GetHeight() > 0)
			return true;

		if (auto const pFoot = abstract_cast<FootClass*>(pTechno))
		{
			if (auto const pJJLoco = locomotion_cast<JumpjetLocomotionClass*>(pFoot->Locomotor))
			{
				if (pJJLoco->State != JumpjetLocomotionClass::State::Grounded)
					return true;
			}
		}

		return false;
	}

	double DistanceXY(const CoordStruct& a, const CoordStruct& b)
	{
		const double dx = static_cast<double>(a.X - b.X);
		const double dy = static_cast<double>(a.Y - b.Y);
		return std::sqrt(dx * dx + dy * dy);
	}

	// ── Target Classification ────────────────────────────────────────────

	TargetDomain GetTargetDomain(AbstractClass* pTarget)
	{
		if (auto const pTechno = abstract_cast<TechnoClass*>(pTarget))
		{
			if (pTechno->IsInAir())
				return TargetDomain::Air;

			if (pTechno->GetTechnoType()->Underwater)
				return TargetDomain::Submerged;

			if (auto const pCell = pTechno->GetCell())
			{
				if (pCell->LandType == LandType::Water || pCell->LandType == LandType::Beach)
					return TargetDomain::Naval;
			}
		}
		return TargetDomain::Ground;
	}

	bool IsTargetDomainAllowed(TechnoTypeExt* pTypeExt, TargetDomain domain)
	{
		switch (domain)
		{
		case TargetDomain::Air:
			return pTypeExt->SmartAutoDeploy_AA.Get();
		case TargetDomain::Naval:
			return pTypeExt->SmartAutoDeploy_AN.Get();
		case TargetDomain::Submerged:
			return pTypeExt->SmartAutoDeploy_AS.Get();
		case TargetDomain::Ground:
		default:
			return pTypeExt->SmartAutoDeploy_AG.Get();
		}
	}

	// ── Weapon Resolution ────────────────────────────────────────────────

	bool CanTypeHitUnderwater(TechnoTypeClass* pType)
	{
		if (!pType)
			return false;

		return pType->NavalTargeting == NavalTargetingType::Underwater_Only
			|| pType->NavalTargeting == NavalTargetingType::Underwater_Secondary
			|| pType->NavalTargeting == NavalTargetingType::Organic_Secondary
			|| pType->NavalTargeting == NavalTargetingType::Naval_All;
	}

	int GetTechnoTypeWeaponCount(TechnoTypeClass* pType)
	{
		if (!pType)
			return 0;
		return pType->WeaponCount > 0 ? pType->WeaponCount : 2;
	}

	int GetDeployWeaponIndex(TechnoTypeClass* pType)
	{
		if (!pType)
			return -1;

		auto const pTypeExt = TechnoTypeExt::Fetch(pType);
		if (pTypeExt->DeployFireWeapon.isset())
			return pTypeExt->DeployFireWeapon.Get();

		if (pType->DeployFireWeapon >= 0)
			return pType->DeployFireWeapon;

		// Units that transform into a different TechnoType on deploy/undeploy use each type's own weapons
		if (pTypeExt->Convert_Deploy || pTypeExt->Convert_Undeploy || pType->DeploysInto || pType->UndeploysInto)
			return -1;

		if (auto const pUnitType = abstract_cast<UnitTypeClass*, true>(pType))
		{
			if (pUnitType->IsSimpleDeployer || pUnitType->DeployFire)
				return 1;
		}

		return -1;
	}

	bool CanFormDamageTarget(TechnoTypeClass* pFormType, WeaponTypeClass* pWeapon, AbstractClass* pTarget);
	double CalculateCombatDPS(TechnoTypeClass* pFormType, WeaponTypeClass* pWeapon, AbstractClass* pTarget);

	int GetFormWeaponIndex(TechnoClass* pThis, TechnoTypeClass* pType, bool deployedForm, AbstractClass* pTarget)
	{
		if (!pType)
			return 0;

		const int deployWeaponIdx = GetDeployWeaponIndex(pType);

		if (deployedForm)
		{
			if (deployWeaponIdx >= 0)
				return deployWeaponIdx;
		}
		else
		{
			// For undeployed form, use the "other" weapon slot
			if (deployWeaponIdx >= 0)
				return (deployWeaponIdx == 0) ? 1 : 0;
		}

		const int selected = (pThis && pThis->GetTechnoType() == pType) ? pThis->SelectWeapon(pTarget) : 0;
		return selected >= 0 ? selected : 0;
	}

	WeaponTypeClass* GetFormWeapon(TechnoClass* pThis, TechnoTypeClass* pAltType, bool deployedForm, AbstractClass* pTarget)
	{
		const bool currentlyDeployed = SmartAutoDeploy::IsCurrentlyDeployed(pThis);
		const auto pCurrentType = pThis ? pThis->GetTechnoType() : nullptr;
		const auto pTargetType = (deployedForm == currentlyDeployed) ? pCurrentType : pAltType;

		// For type-converting forms (no fixed deploy weapon slot), evaluate all weapon slots against the target
		if (pTargetType && pTarget && GetDeployWeaponIndex(pTargetType) < 0)
		{
			const bool isLiveType = (pThis && pTargetType == pCurrentType);
			const int weaponCount = std::min(GetTechnoTypeWeaponCount(pTargetType), TechnoTypeClass::MaxWeapons);
			WeaponTypeClass* pBestWeapon = nullptr;
			double bestDPS = -1.0;
			int bestRange = -1;

			for (int i = 0; i < weaponCount; ++i)
			{
				WeaponTypeClass* pCandidate = nullptr;
				if (isLiveType)
				{
					if (auto const pStruct = pThis->GetWeapon(i))
						pCandidate = pStruct->WeaponType;
				}
				if (!pCandidate)
					pCandidate = pTargetType->Weapon[i].WeaponType;

				if (!pCandidate || !CanFormDamageTarget(pTargetType, pCandidate, pTarget))
					continue;

				const double dps = CalculateCombatDPS(pTargetType, pCandidate, pTarget);
				if (dps > bestDPS || (dps == bestDPS && pCandidate->Range > bestRange))
				{
					bestDPS = dps;
					bestRange = pCandidate->Range;
					pBestWeapon = pCandidate;
				}
			}

			if (pBestWeapon)
				return pBestWeapon;
		}

		// Priority 1: Live weapon from the current unit instance (if form matches current state)
		if (pThis && pCurrentType && deployedForm == currentlyDeployed)
		{
			const int weaponIndex = GetFormWeaponIndex(pThis, pCurrentType, deployedForm, pTarget);
			if (weaponIndex >= 0)
			{
				if (auto const pStruct = pThis->GetWeapon(weaponIndex))
				{
					if (pStruct->WeaponType)
						return pStruct->WeaponType;
				}
			}
		}

		// Priority 2: Static weapon from the target type definition
		if (pTargetType)
		{
			const int weaponIndex = GetFormWeaponIndex(pThis, pTargetType, deployedForm, pTarget);
			if (weaponIndex >= 0 && weaponIndex < TechnoTypeClass::MaxWeapons)
			{
				if (pTargetType->Weapon[weaponIndex].WeaponType)
					return pTargetType->Weapon[weaponIndex].WeaponType;
			}

			// Priority 3: Fallback chain for deployed form
			if (deployedForm)
			{
				const int deployIdx = GetDeployWeaponIndex(pTargetType);
				if (deployIdx >= 0 && deployIdx < TechnoTypeClass::MaxWeapons && pTargetType->Weapon[deployIdx].WeaponType)
					return pTargetType->Weapon[deployIdx].WeaponType;

				if (pTargetType != pCurrentType && pTargetType->Weapon[0].WeaponType)
					return pTargetType->Weapon[0].WeaponType;

				if (pTargetType->Weapon[1].WeaponType)
					return pTargetType->Weapon[1].WeaponType;

				if (pTargetType->Weapon[0].WeaponType)
					return pTargetType->Weapon[0].WeaponType;
			}
			else
			{
				if (pTargetType->Weapon[0].WeaponType)
					return pTargetType->Weapon[0].WeaponType;
			}
		}

		// Priority 4: Default weapon from the unit instance
		if (pThis)
		{
			if (auto const pDefaultStruct = pThis->GetWeapon(0))
				return pDefaultStruct->WeaponType;
		}

		return nullptr;
	}

	// ── Damage Evaluation ────────────────────────────────────────────────

	bool CanEnemyWeaponHitForm(WeaponTypeClass* pWeapon, TechnoTypeClass* pEnemyType, TechnoTypeClass* pFormType, bool deployedForm = false)
	{
		if (!pWeapon || !pWeapon->Warhead || !pFormType)
			return false;

		if (GeneralUtils::GetWarheadVersusArmor(pWeapon->Warhead, pFormType->Armor) <= 0.0)
			return false;

		const bool formIsAir = !(deployedForm && pFormType->DeployToLand)
			&& (pFormType->SpeedType == SpeedType::Winged || pFormType->MovementZone == MovementZone::Fly);
		const bool formIsUnderwater = pFormType->Underwater;
		const bool formIsNavalSurface = !formIsAir && !formIsUnderwater
			&& (pFormType->Naval || pFormType->SpeedType == SpeedType::Float);
		const bool formIsLand = !formIsAir && !formIsUnderwater && !formIsNavalSurface;

		if (formIsAir)
			return pWeapon->Projectile && pWeapon->Projectile->AA;

		if (formIsUnderwater)
			return CanTypeHitUnderwater(pEnemyType);

		if (formIsNavalSurface && pEnemyType)
		{
			if (pEnemyType->NavalTargeting == NavalTargetingType::Naval_None
				|| pEnemyType->NavalTargeting == NavalTargetingType::Underwater_Only)
			{
				return false;
			}
		}

		if (formIsLand)
		{
			if (pEnemyType && pEnemyType->LandTargeting == LandTargetingType::Land_Not_OK)
				return false;

			if (pWeapon->Projectile && pWeapon->Projectile->Level)
				return false;
		}

		return !pWeapon->Projectile || pWeapon->Projectile->AG || !pWeapon->Projectile->AA;
	}

	bool CanTargetDamageForm(TechnoClass* pTargetTechno, TechnoTypeClass* pFormType, bool deployedForm = false)
	{
		if (!pTargetTechno || !pFormType)
			return true;

		const auto pEnemyType = pTargetTechno->GetTechnoType();
		const int enemyWeaponCount = GetTechnoTypeWeaponCount(pEnemyType);
		for (int i = 0; i < enemyWeaponCount; ++i)
		{
			if (auto const pWeaponStruct = pTargetTechno->GetWeapon(i))
			{
				if (CanEnemyWeaponHitForm(pWeaponStruct->WeaponType, pEnemyType, pFormType, deployedForm))
					return true;
			}
		}

		return false;
	}

	int GetEnemyThreatRange(TechnoClass* pTargetTechno, TechnoTypeClass* pFormType)
	{
		if (!pTargetTechno || !pFormType)
			return 0;

		int maxRange = 0;
		const auto pEnemyType = pTargetTechno->GetTechnoType();
		const int enemyWeaponCount = GetTechnoTypeWeaponCount(pEnemyType);
		for (int i = 0; i < enemyWeaponCount; ++i)
		{
			if (auto const pWeaponStruct = pTargetTechno->GetWeapon(i))
			{
				auto const pWeapon = pWeaponStruct->WeaponType;
				if (CanEnemyWeaponHitForm(pWeapon, pEnemyType, pFormType))
					maxRange = std::max(maxRange, pWeapon->Range);
			}
		}

		return maxRange;
	}

	bool CanFormDamageTarget(TechnoTypeClass* pFormType, WeaponTypeClass* pWeapon, AbstractClass* pTarget)
	{
		if (!pWeapon || !pWeapon->Warhead || !pTarget)
			return false;

		const auto targetDomain = GetTargetDomain(pTarget);

		if (targetDomain == TargetDomain::Ground)
		{
			if (pFormType && pFormType->LandTargeting == LandTargetingType::Land_Not_OK)
				return false;

			if (pWeapon->Projectile && pWeapon->Projectile->Level)
				return false;
		}
		else if (targetDomain == TargetDomain::Naval)
		{
			if (pFormType && (pFormType->NavalTargeting == NavalTargetingType::Naval_None
				|| pFormType->NavalTargeting == NavalTargetingType::Underwater_Only))
			{
				return false;
			}
		}
		else if (targetDomain == TargetDomain::Submerged)
		{
			if (!CanTypeHitUnderwater(pFormType))
				return false;
		}

		if (auto const pTargetTechno = abstract_cast<TechnoClass*>(pTarget))
		{
			auto const pTargetType = pTargetTechno->GetTechnoType();
			if (!pTargetType)
				return false;

			if (GeneralUtils::GetWarheadVersusArmor(pWeapon->Warhead, pTargetTechno, pTargetType) <= 0.0)
				return false;

			if (pTargetTechno->IsInAir() && (!pWeapon->Projectile || !pWeapon->Projectile->AA))
				return false;

			if (!pTargetTechno->IsInAir() && pWeapon->Projectile && !pWeapon->Projectile->AG && pWeapon->Projectile->AA)
				return false;

			if (pTargetType->Underwater && !CanTypeHitUnderwater(pFormType))
				return false;
		}

		return true;
	}

	double CalculateCombatDPS(TechnoTypeClass* pFormType, WeaponTypeClass* pWeapon, AbstractClass* pTarget)
	{
		if (!CanFormDamageTarget(pFormType, pWeapon, pTarget))
			return 0.0;

		auto const pTargetTechno = abstract_cast<TechnoClass*>(pTarget);
		if (!pTargetTechno)
			return 0.0;

		auto const pTargetType = pTargetTechno->GetTechnoType();
		if (!pTargetType)
			return 0.0;

		const double verses = GeneralUtils::GetWarheadVersusArmor(pWeapon->Warhead, pTargetTechno, pTargetType);
		if (verses <= 0.0)
			return 0.0;

		double rawDamage = static_cast<double>(pWeapon->Damage);
		if (pWeapon->AmbientDamage > 0)
			rawDamage += static_cast<double>(pWeapon->AmbientDamage);

		if (rawDamage <= 0.0)
			return 0.0;

		const int burst = std::max(1, pWeapon->Burst);
		const double totalVolleyDamage = rawDamage * verses * burst;
		const double rof = std::max(1.0, static_cast<double>(pWeapon->ROF));
		return (totalVolleyDamage * 15.0) / rof;
	}

	// ── Cell Selection ───────────────────────────────────────────────────

	bool IsCellPassableForLanding(CellClass* pCell, UnitClass* pUnit)
	{
		if (pCell->Passability == PassabilityType::Impassable || pCell->Passability == PassabilityType::OutsideMap)
			return false;

		if (pCell->TubeIndex >= 0)
			return false;

		if (pCell->GetBuilding())
			return false;

		if (pUnit)
		{
			if (pUnit->Type->Naval || pUnit->Type->MovementZone == MovementZone::Water)
			{
				if (pCell->LandType != LandType::Water)
					return false;
			}
			else if (pUnit->Type->DeployToLand)
			{
				if (pCell->LandType == LandType::Water || pCell->LandType == LandType::Beach)
					return false;
			}
		}

		for (auto pObj = pCell->FirstObject; pObj; pObj = pObj->NextObject)
		{
			if (pObj != pUnit && !pObj->IsInAir() && abstract_cast<TechnoClass*>(pObj))
				return false;
		}

		return true;
	}

	bool FindClearCellForLanding(CellClass* pCenterCell, UnitClass* pUnit, AbstractClass* pTarget, WeaponTypeClass* pAltWeapon, CellClass*& outCell)
	{
		if (!pCenterCell && pUnit)
			pCenterCell = pUnit->GetCell();

		if (!pCenterCell)
			return false;

		const auto targetCoords = pTarget ? pTarget->GetCoords() : CoordStruct::Empty;

		for (const auto& offset : GeneralUtils::AdjacentCellsInRange(2))
		{
			CellStruct testCoords = pCenterCell->MapCoords + offset;
			auto const pCell = MapClass::Instance.TryGetCellAt(testCoords);
			if (!pCell || !IsCellPassableForLanding(pCell, pUnit))
				continue;

			if (pTarget && pAltWeapon)
			{
				const double dist = pCell->GetCenterCoords().DistanceFrom(targetCoords);
				const double margin = std::min(128.0, static_cast<double>(pAltWeapon->Range) / 16.0);

				if (dist > static_cast<double>(pAltWeapon->Range) - margin)
					continue;

				if (pAltWeapon->MinimumRange > 0 && dist < static_cast<double>(pAltWeapon->MinimumRange) + margin)
					continue;
			}

			outCell = pCell;
			return true;
		}

		return false;
	}

	bool ShouldCheckMinimumRange(TechnoTypeExt* pTypeExt, TechnoTypeExt* pAltTypeExt)
	{
		return pTypeExt->SmartAutoDeploy_CheckMinimumRange.Get() || pAltTypeExt->SmartAutoDeploy_CheckMinimumRange.Get();
	}

	CellClass* CalculateStandoffCell(TechnoClass* pThis, AbstractClass* pTarget, WeaponTypeClass* pWeapon)
	{
		if (!pThis || !pTarget || !pWeapon)
			return nullptr;

		// Standoff at (Range - 1 cell), bounded above MinimumRange to maximize firing distance and safety.
		const double maxRange = static_cast<double>(pWeapon->Range);
		const double margin = 256.0; // 1 cell (256 leptons)
		const double minAllowed = (pWeapon->MinimumRange > 0)
			? (static_cast<double>(pWeapon->MinimumRange) + margin)
			: margin;
		const double desiredDist = std::max(minAllowed, maxRange - margin);

		const CoordStruct myCoords = pThis->GetCoords();
		const CoordStruct targetCoords = pTarget->GetCoords();
		double dx = static_cast<double>(myCoords.X - targetCoords.X);
		double dy = static_cast<double>(myCoords.Y - targetCoords.Y);
		double currentDistLeptons = std::sqrt(dx * dx + dy * dy);

		if (currentDistLeptons < 32.0)
		{
			dx = 1.0;
			dy = 0.0;
			currentDistLeptons = 1.0;
		}

		const double scale = desiredDist / currentDistLeptons;
		CoordStruct retreatCoords {
			static_cast<int>(targetCoords.X + dx * scale),
			static_cast<int>(targetCoords.Y + dy * scale),
			myCoords.Z
		};

		CellStruct retreatCell = CellClass::Coord2Cell(retreatCoords);
		if (auto const pCandidate = MapClass::Instance.TryGetCellAt(retreatCell))
		{
			CellClass* pClearCell = nullptr;
			auto const pUnit = abstract_cast<UnitClass*>(pThis);
			if (FindClearCellForLanding(pCandidate, pUnit, pTarget, pWeapon, pClearCell))
				return pClearCell;
		}

		return nullptr;
	}

	// ── State Management ─────────────────────────────────────────────────

	void ResetSmartAutoDeployState(TechnoExt* pExt)
	{
		pExt->SmartAutoDeploy_IsRepositioning = false;
		pExt->SmartAutoDeploy_RepositionDestination = CoordStruct::Empty;
		pExt->SmartAutoDeploy_SavedTarget = nullptr;
		pExt->SmartAutoDeploy_SavedMission = Mission::None;
		pExt->SmartAutoDeploy_TargetAction = SmartAutoDeployAction::None;
	}

	// ── Threshold Helpers ────────────────────────────────────────────────

	bool ShouldTriggerThreshold(double currentRatio, double threshold, double chance)
	{
		if (currentRatio >= threshold)
			return false;
		return chance >= 1.0 || ScenarioClass::Instance->Random.RandomDouble() <= chance;
	}

	double GetHealthRatio(TechnoClass* pThis, TechnoTypeClass* pType)
	{
		return static_cast<double>(pThis->Health) / pType->Strength;
	}

	double GetShieldRatio(TechnoClass* pThis)
	{
		const auto pShield = TechnoExt::Fetch(pThis)->Shield.get();
		return (pShield && pShield->IsActive()) ? pShield->GetHealthRatio() : 0.0;
	}

	// ── Transformation Progress Tracking ─────────────────────────────────

	bool IsTransformInProgress(TechnoClass* pThis, UnitClass* pUnit, SmartAutoDeployAction action)
	{
		if (action == SmartAutoDeployAction::Deploy)
		{
			if (SmartAutoDeploy::IsCurrentlyDeployed(pThis))
				return pUnit && pUnit->Deploying;

			if (pUnit)
			{
				if (pUnit->Deploying || pUnit->CurrentMission == Mission::Unload || pUnit->QueuedMission == Mission::Unload)
				{
					if (pUnit->Target)
						pUnit->SetTarget(nullptr);

					return true;
				}

				if (pUnit->Type->DeployToLand && IsTechnoAirborne(pUnit))
				{
					if (auto const pLoco = locomotion_cast<JumpjetLocomotionClass*>(pUnit->Locomotor))
						return pLoco->State == JumpjetLocomotionClass::State::Descending;
				}
			}
		}
		else if (action == SmartAutoDeployAction::Undeploy)
		{
			if (!SmartAutoDeploy::IsCurrentlyDeployed(pThis))
				return pUnit && pUnit->Undeploying;

			if (pUnit)
				return pUnit->Undeploying || pUnit->CurrentMission == Mission::Unload || pUnit->QueuedMission == Mission::Unload;
		}

		return false;
	}

	bool IsTransformCompleted(TechnoClass* pThis, UnitClass* pUnit, SmartAutoDeployAction action)
	{
		if (action == SmartAutoDeployAction::Deploy)
			return SmartAutoDeploy::IsCurrentlyDeployed(pThis) && !(pUnit && pUnit->Deploying);

		if (action == SmartAutoDeployAction::Undeploy)
			return !SmartAutoDeploy::IsCurrentlyDeployed(pThis) && !(pUnit && pUnit->Undeploying);

		return false;
	}

} // anonymous namespace

// ═══════════════════════════════════════════════════════════════════════════
// Section 2: Public API (namespace SmartAutoDeploy)
// ═══════════════════════════════════════════════════════════════════════════

namespace SmartAutoDeploy
{
	// ── Type / State Queries ─────────────────────────────────────────────

	TechnoTypeClass* GetAlternateType(TechnoClass* pThis)
	{
		if (!pThis)
			return nullptr;

		const auto pType = pThis->GetTechnoType();
		const auto pTypeExt = TechnoTypeExt::Fetch(pType);

		if (pTypeExt->Convert_Deploy)
			return pTypeExt->Convert_Deploy;
		if (pTypeExt->Convert_Undeploy)
			return pTypeExt->Convert_Undeploy;

		if (pType->DeploysInto)
			return pType->DeploysInto;
		if (pType->UndeploysInto)
			return pType->UndeploysInto;

		if (auto const pUnitType = abstract_cast<UnitTypeClass*, true>(pType))
		{
			if (pUnitType->IsSimpleDeployer)
				return pType;
		}

		return nullptr;
	}

	bool IsCurrentlyDeployed(TechnoClass* pThis)
	{
		if (!pThis)
			return false;

		if (pThis->WhatAmI() == AbstractType::Building)
		{
			const auto pBuilding = static_cast<BuildingClass*>(pThis);
			return pBuilding->Type->UndeploysInto != nullptr || TechnoTypeExt::Fetch(pBuilding->Type)->Convert_Undeploy != nullptr;
		}

		const auto pTypeExt = TechnoTypeExt::Fetch(pThis->GetTechnoType());
		if (pTypeExt->Convert_Deploy || pTypeExt->Convert_Undeploy)
			return pTypeExt->Convert_Undeploy != nullptr && !pTypeExt->Convert_Deploy;

		if (pThis->WhatAmI() == AbstractType::Unit)
		{
			const auto pUnit = static_cast<UnitClass*>(pThis);
			if (pUnit->Type->IsSimpleDeployer)
				return pUnit->Deployed;
		}

		return false;
	}

	// ── Transformation Execution ─────────────────────────────────────────

	void StopTechnoMotion(FootClass* pFoot)
	{
		if (!pFoot)
			return;

		pFoot->AbortMotion();
		pFoot->SetDestination(nullptr, true);
		if (pFoot->Locomotor)
			pFoot->Locomotor->Stop_Moving();
	}

	bool CanPerformTransformation(TechnoClass* pThis, bool toDeploy)
	{
		if (!IsAliveAndValid(pThis) || pThis->IsUnderEMP() || pThis->IsParalyzed())
			return false;

		const auto pType = pThis->GetTechnoType();
		const auto pTypeExt = TechnoTypeExt::Fetch(pType);

		if (toDeploy)
		{
			if (pTypeExt->Convert_Deploy)
			{
				if (auto const pFoot = abstract_cast<FootClass*>(pThis))
				{
					if (auto const pUnit = abstract_cast<UnitClass*>(pFoot))
					{
						if (pUnit->Deploying || pUnit->Undeploying)
							return false;
						if (!UnitExt::HasAmmoToDeploy(pUnit))
							return false;
					}
					return true;
				}
				return false;
			}

			if (auto const pUnit = abstract_cast<UnitClass*>(pThis))
			{
				if (pUnit->Deploying || pUnit->Undeploying)
					return false;

				if (!UnitExt::HasAmmoToDeploy(pUnit))
					return false;

				if (pType->DeploysInto)
					return UnitExt::CanDeployIntoBuilding(pUnit, true);

				if (pUnit->Type->IsSimpleDeployer)
				{
					if (!UnitExt::SimpleDeployerAllowedToDeploy(pUnit, true, true))
						return false;

					if (auto const pCell = pUnit->GetCell())
					{
						if (pCell->TubeIndex >= 0)
							return false;

						if (pUnit->Type->DeployToLand)
						{
							if (pCell->Passability == PassabilityType::Impassable || pCell->Passability == PassabilityType::OutsideMap)
								return false;

							if (pCell->GetBuilding())
								return false;

							for (auto pObj = pCell->FirstObject; pObj; pObj = pObj->NextObject)
							{
								if (pObj != pUnit && !pObj->IsInAir())
								{
									if (auto const pTech = abstract_cast<TechnoClass*>(pObj))
										return false;
								}
							}
						}
					}

					return true;
				}
			}
		}
		else
		{
			if (auto const pBuilding = abstract_cast<BuildingClass*>(pThis))
			{
				if (pBuilding->CurrentMission == Mission::Selling || pBuilding->CurrentMission == Mission::Construction)
					return false;

				if (pBuilding->Type->UndeploysInto)
				{
					if (pBuilding->Type->ConstructionYard)
					{
						if (!GameModeOptionsClass::Instance.MCVRedeploy)
							return false;
						if (!RulesExt::Global()->AllowDeployControlledMCV && pBuilding->MindControlledBy)
							return false;
					}
					return true;
				}

				return false;
			}
			else if (pTypeExt->Convert_Undeploy)
			{
				if (auto const pFoot = abstract_cast<FootClass*>(pThis))
				{
					if (auto const pUnit = abstract_cast<UnitClass*>(pFoot))
					{
						if (pUnit->Deploying || pUnit->Undeploying)
							return false;
					}
					return true;
				}
				return false;
			}
			else if (auto const pUnit = abstract_cast<UnitClass*>(pThis))
			{
				if (pUnit->Deploying || pUnit->Undeploying)
					return false;

				if (pUnit->Type->IsSimpleDeployer)
					return pUnit->Deployed;
			}
		}

		return false;
	}

	bool ExecuteTransformation(TechnoClass* pThis, bool toDeploy)
	{
		if (!CanPerformTransformation(pThis, toDeploy))
			return false;

		const auto pType = pThis->GetTechnoType();
		const auto pTypeExt = TechnoTypeExt::Fetch(pType);

		if (toDeploy)
		{
			pThis->SetTarget(nullptr);

			if (pTypeExt->Convert_Deploy)
			{
				if (auto const pFoot = abstract_cast<FootClass*>(pThis))
				{
					StopTechnoMotion(pFoot);
					if (TechnoExt::ConvertToType(pFoot, pTypeExt->Convert_Deploy))
					{
						if (auto const pUnit = abstract_cast<UnitClass*>(pFoot))
						{
							pUnit->Deployed = false;
							pUnit->Deploying = false;
							pUnit->Undeploying = false;
						}
						if (pType->DeploySound != -1)
							VocClass::PlayAt(pType->DeploySound, pThis->GetCoords());
						return true;
					}
					return false;
				}
			}
			else if (auto const pUnit = abstract_cast<UnitClass*>(pThis))
			{
				if (pType->DeploysInto || pUnit->Type->IsSimpleDeployer)
				{
					pUnit->QueueMission(Mission::Unload, true);
					StopTechnoMotion(pUnit);

					if (pUnit->Type->DeployToLand && IsTechnoAirborne(pUnit))
					{
						if (auto const pJJLoco = locomotion_cast<JumpjetLocomotionClass*>(pUnit->Locomotor))
						{
							if (auto const pCell = pUnit->GetCell())
							{
								pJJLoco->DestinationCoords = pCell->GetCoords();
								pJJLoco->IsMoving = true;
							}
						}
					}

					return true;
				}
			}
		}
		else
		{
			pThis->SetTarget(nullptr);

			if (auto const pBuilding = abstract_cast<BuildingClass*>(pThis))
			{
				if (pBuilding->Type->UndeploysInto)
				{
					if (const auto pCell = MapClass::Instance.TryGetCellAt(pBuilding->GetMapCoords()))
						pBuilding->SetArchiveTarget(pCell);

					pBuilding->Sell(1);
					return true;
				}
			}
			else if (pTypeExt->Convert_Undeploy)
			{
				if (auto const pFoot = abstract_cast<FootClass*>(pThis))
				{
					StopTechnoMotion(pFoot);
					if (TechnoExt::ConvertToType(pFoot, pTypeExt->Convert_Undeploy))
					{
						if (auto const pUnit = abstract_cast<UnitClass*>(pFoot))
						{
							pUnit->Deployed = false;
							pUnit->Deploying = false;
							pUnit->Undeploying = false;
						}
						if (pType->UndeploySound != -1)
							VocClass::PlayAt(pType->UndeploySound, pThis->GetCoords());
						return true;
					}
					return false;
				}
			}
			else if (auto const pUnit = abstract_cast<UnitClass*>(pThis))
			{
				if (pUnit->Type->IsSimpleDeployer)
				{
					StopTechnoMotion(pUnit);
					pUnit->QueueMission(Mission::Unload, true);
					return true;
				}
			}
		}

		return false;
	}

	// Executes a transformation while preserving the unit's current Destination, ArchiveTarget,
	// Mission, and optional Target (used for seamless Travel, Chase, and OpportunityFire mode switches).
	bool ExecuteTravelTransformation(TechnoClass* pThis, bool toDeploy, AbstractClass* pKeepTarget = nullptr)
	{
		if (!CanPerformTransformation(pThis, toDeploy))
			return false;

		auto const pFoot = abstract_cast<FootClass*>(pThis);
		AbstractClass* const pSavedDest = pFoot ? pFoot->Destination : nullptr;
		AbstractClass* const pSavedArchive = pFoot ? pFoot->ArchiveTarget : nullptr;
		const Mission savedMission = pThis->CurrentMission;

		const auto pType = pThis->GetTechnoType();
		const auto pTypeExt = TechnoTypeExt::Fetch(pType);
		TechnoTypeClass* const pConvertTo = toDeploy ? pTypeExt->Convert_Deploy.Get() : pTypeExt->Convert_Undeploy.Get();

		if (pFoot && pConvertTo)
		{
			if (TechnoExt::ConvertToType(pFoot, pConvertTo))
			{
				if (auto const pUnit = abstract_cast<UnitClass*>(pFoot))
				{
					pUnit->Deployed = false;
					pUnit->Deploying = false;
					pUnit->Undeploying = false;
				}

				const int soundIdx = toDeploy ? pType->DeploySound : pType->UndeploySound;
				if (soundIdx != -1)
					VocClass::PlayAt(soundIdx, pThis->GetCoords());

				pThis->SetTarget(pKeepTarget);
				if (pKeepTarget)
					pThis->LastTarget = pKeepTarget;

				if (pSavedDest && pFoot->Destination != pSavedDest)
					pFoot->SetDestination(pSavedDest, true);

				if (pSavedArchive && pFoot->ArchiveTarget != pSavedArchive)
					pFoot->SetArchiveTarget(pSavedArchive);

				if (savedMission != Mission::None && pThis->CurrentMission != savedMission)
				{
					pThis->QueueMission(savedMission, false);
					pThis->NextMission();
				}

				return true;
			}
			return false;
		}

		if (!ExecuteTransformation(pThis, toDeploy))
			return false;

		if (pFoot && pSavedDest)
			pFoot->SetDestination(pSavedDest, true);

		if (pKeepTarget)
		{
			pThis->SetTarget(pKeepTarget);
			pThis->LastTarget = pKeepTarget;
		}

		return true;
	}

	// Saves tactical state and executes a deploy/undeploy transformation.
	// Consolidates repeated save-target + set-action + execute pattern.
	bool SaveAndTransform(TechnoClass* pThis, AbstractClass* pTarget,
		SmartAutoDeployAction action, bool toDeploy,
		Mission savedMission = Mission::Attack, bool repositioning = false)
	{
		const auto pType = pThis->GetTechnoType();
		const auto pTypeExt = TechnoTypeExt::Fetch(pType);
		const bool isInstantConvert = toDeploy ? (pTypeExt->Convert_Deploy != nullptr) : (pTypeExt->Convert_Undeploy != nullptr);

		if (isInstantConvert && !repositioning)
		{
			if (!ExecuteTravelTransformation(pThis, toDeploy, pTarget))
				return false;

			if (savedMission != Mission::None && pThis->CurrentMission != savedMission)
			{
				pThis->QueueMission(savedMission, false);
				pThis->NextMission();
			}

			ResetSmartAutoDeployState(TechnoExt::Fetch(pThis));
			return true;
		}

		if (!ExecuteTransformation(pThis, toDeploy))
			return false;

		auto const pExt = TechnoExt::Fetch(pThis);
		pExt->SmartAutoDeploy_SavedTarget = pTarget;
		pExt->SmartAutoDeploy_SavedMission = savedMission;
		pExt->SmartAutoDeploy_TargetAction = action;
		pExt->SmartAutoDeploy_IsRepositioning = repositioning;
		return true;
	}

	// ── Prerequisites ────────────────────────────────────────────────────

	bool ShouldEvaluate(TechnoClass* pThis)
	{
		if (!IsAliveAndValid(pThis) || pThis->IsUnderEMP() || pThis->IsParalyzed())
			return false;

		const auto pOwner = pThis->Owner;
		if (!pOwner)
			return false;

		const bool isAI = !pOwner->IsControlledByHuman();
		const auto pTypeExt = TechnoTypeExt::Fetch(pThis->GetTechnoType());
		const bool enabledOnCurrent = isAI ? pTypeExt->SmartAutoDeploy_AI.Get() : pTypeExt->SmartAutoDeploy_Player.Get();

		const auto pAltType = GetAlternateType(pThis);
		if (!pAltType)
			return false;

		if (!enabledOnCurrent)
		{
			const auto pAltTypeExt = TechnoTypeExt::Fetch(pAltType);
			const bool enabledOnAlt = isAI ? pAltTypeExt->SmartAutoDeploy_AI.Get() : pAltTypeExt->SmartAutoDeploy_Player.Get();
			if (!enabledOnAlt)
				return false;
		}

		return true;
	}

	// ── Combat Sub-evaluators ────────────────────────────────────────────

	// Evaluates whether a currently deployed unit should undeploy
	// (minimum range, defensive thresholds, target chase).
	bool EvaluateDeployedCombat(TechnoClass* pThis, AbstractClass* pTarget,
		TechnoTypeClass* pType, TechnoTypeClass* pAltType,
		WeaponTypeClass* pCurrentWeapon, WeaponTypeClass* pAltWeapon,
		int currentDist, bool byWeaponDamage)
	{
		const auto pTypeExt = TechnoTypeExt::Fetch(pType);
		const auto pAltTypeExt = TechnoTypeExt::Fetch(pAltType);

		// ── MinimumRange reposition ──────────────────────────────────────
		if (pCurrentWeapon && pCurrentWeapon->MinimumRange > 0 && currentDist < pCurrentWeapon->MinimumRange)
		{
			if (ShouldCheckMinimumRange(pTypeExt, pAltTypeExt))
				return SaveAndTransform(pThis, pTarget, SmartAutoDeployAction::Undeploy, false, Mission::Attack, true);

			if (pAltWeapon && currentDist >= pAltWeapon->MinimumRange && CanFormDamageTarget(pAltType, pAltWeapon, pTarget))
				return SaveAndTransform(pThis, pTarget, SmartAutoDeployAction::Undeploy, false);
		}

		// ── Inverted HP threshold: flee when health is low ───────────────
		if (pTypeExt->SmartAutoDeploy_HP_Threshold.isset() && pTypeExt->SmartAutoDeploy_HP_Threshold_Inverted)
		{
			const double hpThresh = std::clamp(pTypeExt->SmartAutoDeploy_HP_Threshold.Get(), 0.0, 1.0);
			if (ShouldTriggerThreshold(GetHealthRatio(pThis, pType), hpThresh, pTypeExt->SmartAutoDeploy_HP_Threshold_Chance.Get()))
				return SaveAndTransform(pThis, pTarget, SmartAutoDeployAction::Undeploy, false);
		}

		// ── Inverted Shield threshold: flee when shield is low ───────────
		if (pTypeExt->SmartAutoDeploy_SHP_Threshold.isset() && pTypeExt->SmartAutoDeploy_SHP_Threshold_Inverted)
		{
			const double shpThresh = std::clamp(pTypeExt->SmartAutoDeploy_SHP_Threshold.Get(), 0.0, 1.0);
			if (ShouldTriggerThreshold(GetShieldRatio(pThis), shpThresh, pTypeExt->SmartAutoDeploy_SHP_Threshold_Chance.Get()))
				return SaveAndTransform(pThis, pTarget, SmartAutoDeployAction::Undeploy, false);
		}

		// ── Target Chase: undeploy to pursue fleeing target ──────────────
		if (pTypeExt->SmartAutoDeploy_TargetChase || pAltTypeExt->SmartAutoDeploy_TargetChase)
		{
			bool shouldUndeploy = false;

			if (pCurrentWeapon)
			{
				const int currentWeaponIdx = GetFormWeaponIndex(pThis, pType, true, pTarget);
				const bool canAttackCurrent = pThis->IsCloseEnough(pTarget, currentWeaponIdx);

				// Pursue target when it moves outside maximum weapon range.
				// (Targets inside MinimumRange are handled exclusively by MinimumRange reposition above).
				if (!canAttackCurrent && currentDist >= pCurrentWeapon->Range)
				{
					shouldUndeploy = true;
				}
				else if (byWeaponDamage && pAltWeapon)
				{
					const double dpsCurrent = CalculateCombatDPS(pType, pCurrentWeapon, pTarget);
					const double dpsAlt = CalculateCombatDPS(pAltType, pAltWeapon, pTarget);
					if (dpsCurrent <= 0.0 && dpsAlt > 0.0)
						shouldUndeploy = true;
				}
			}
			else
			{
				shouldUndeploy = true;
			}

			if (shouldUndeploy)
				return SaveAndTransform(pThis, pTarget, SmartAutoDeployAction::Undeploy, false);
		}

		return false;
	}

	// Scans for a nearby enemy target that the alternate form can engage when the current form
	// cannot natively acquire targets in that domain (e.g. submerged submarine -> ground/air targets,
	// or surfaced missile sub -> underwater targets).
	TechnoClass* FindAlternateFormTarget(TechnoClass* pThis, TechnoTypeClass* pType, TechnoTypeClass* pAltType, bool currentlyDeployed)
	{
		if (!pThis || !pType || !pAltType || pType == pAltType || !pThis->Owner)
			return nullptr;

		const auto mission = pThis->CurrentMission;
		if (mission != Mission::Guard && mission != Mission::Area_Guard && mission != Mission::Hunt)
			return nullptr;

		const auto pTypeExt = TechnoTypeExt::Fetch(pType);
		const auto pAltTypeExt = TechnoTypeExt::Fetch(pAltType);
		const bool isMobileUnitConvert = (pType != pAltType && pAltType->Speed > 0 && pAltType->WhatAmI() != AbstractType::BuildingType);

		if (!isMobileUnitConvert)
		{
			if (mission == Mission::Guard && !(pTypeExt->SmartAutoDeploy_Guard || pAltTypeExt->SmartAutoDeploy_Guard))
				return nullptr;

			if (mission == Mission::Area_Guard && !(pTypeExt->SmartAutoDeploy_AreaGuard || pAltTypeExt->SmartAutoDeploy_AreaGuard))
				return nullptr;
		}

		if (pTypeExt->SmartAutoDeploy_HP_Threshold.isset() && pTypeExt->SmartAutoDeploy_HP_Threshold_Inverted)
		{
			const double hpThresh = std::clamp(pTypeExt->SmartAutoDeploy_HP_Threshold.Get(), 0.0, 1.0);
			if (GetHealthRatio(pThis, pType) < hpThresh)
				return nullptr;
		}

		if (pTypeExt->SmartAutoDeploy_SHP_Threshold.isset() && pTypeExt->SmartAutoDeploy_SHP_Threshold_Inverted)
		{
			const double shpThresh = std::clamp(pTypeExt->SmartAutoDeploy_SHP_Threshold.Get(), 0.0, 1.0);
			if (GetShieldRatio(pThis) < shpThresh)
				return nullptr;
		}

		if (auto const pFoot = abstract_cast<FootClass*>(pThis))
		{
			if (pFoot->Destination && pFoot->Locomotor && pFoot->Locomotor->Is_Moving_Now())
				return nullptr;
		}

		TechnoClass* pBestTarget = nullptr;
		int bestDist = INT_MAX;
		bool bestIsThreat = false;

		for (auto const pCandidate : TechnoClass::Array)
		{
			if (!pCandidate || pCandidate == pThis || !pCandidate->IsAlive || pCandidate->InLimbo)
				continue;

			if (pCandidate->TemporalTargetingMe || pCandidate->BeingWarpedOut)
				continue;

			if (!pCandidate->Owner || pThis->Owner->IsAlliedWith(pCandidate->Owner) || pCandidate->Owner->IsNeutral())
				continue;

			auto const pCandidateType = pCandidate->GetTechnoType();
			if (!pCandidateType || pCandidateType->Insignificant || pCandidateType->DontScore)
				continue;

			if (auto const pBld = abstract_cast<BuildingClass*>(pCandidate))
			{
				if (pBld->Type->Wall || pBld->Type->Gate)
					continue;
			}

			if (pCandidate->CloakState != CloakState::Uncloaked)
			{
				auto const pCandCell = pCandidate->GetCell();
				const bool sensedByHouse = pCandCell && pCandCell->Sensors_InclHouse(pThis->Owner->ArrayIndex);
				const bool sensedByAlt = pAltType->Sensors && pAltType->SensorsSight > 0
					&& pThis->DistanceFrom(pCandidate) <= pAltType->SensorsSight * 256;
				if (!sensedByHouse && !sensedByAlt)
					continue;
			}

			const auto domain = GetTargetDomain(pCandidate);
			if (!IsTargetDomainAllowed(pAltTypeExt, domain))
				continue;

			auto const pCurrWep = GetFormWeapon(pThis, pAltType, currentlyDeployed, pCandidate);
			if (CanFormDamageTarget(pType, pCurrWep, pCandidate))
				continue;

			auto const pAltWep = GetFormWeapon(pThis, pAltType, !currentlyDeployed, pCandidate);
			if (!pAltWep || !CanFormDamageTarget(pAltType, pAltWep, pCandidate))
				continue;

			const int dist = pThis->DistanceFrom(pCandidate);
			if (pAltWep->MinimumRange > 0 && dist < pAltWep->MinimumRange)
				continue;

			const int airBonus = (pCandidate->IsInAir() && pAltType->AirRangeBonus > 0)
				? pAltType->AirRangeBonus * 256
				: 0;
			const int effectiveRange = pAltWep->Range + airBonus;
			const int deployMargin = std::min(256, effectiveRange / 8);
			const int maxDist = (pAltWep->MinimumRange > 0 && effectiveRange > pAltWep->MinimumRange + deployMargin)
				? std::max(pAltWep->MinimumRange + 128, effectiveRange - deployMargin)
				: effectiveRange - deployMargin;

			if (dist > maxDist)
				continue;

			const bool isThreat = CanTargetDamageForm(pCandidate, pAltType, !currentlyDeployed);
			if (!pBestTarget || (isThreat && !bestIsThreat) || (isThreat == bestIsThreat && dist < bestDist))
			{
				pBestTarget = pCandidate;
				bestDist = dist;
				bestIsThreat = isThreat;
			}
		}

		return pBestTarget;
	}

	// Scans for a nearby enemy target that an OpportunityFire-capable alternate form can engage
	// while the unit is moving in its faster non-OpportunityFire travel form (e.g. RA3APOC2 -> RA3APOC).
	TechnoClass* FindOpportunityFireTarget(TechnoClass* pThis, TechnoTypeClass* pType, TechnoTypeClass* pAltType, bool currentlyDeployed)
	{
		if (!pThis || !pType || !pAltType || pType == pAltType || !pThis->Owner)
			return nullptr;

		const auto pTypeExt = TechnoTypeExt::Fetch(pType);
		const auto pAltTypeExt = TechnoTypeExt::Fetch(pAltType);

		if (pTypeExt->SmartAutoDeploy_HP_Threshold.isset() && pTypeExt->SmartAutoDeploy_HP_Threshold_Inverted)
		{
			const double hpThresh = std::clamp(pTypeExt->SmartAutoDeploy_HP_Threshold.Get(), 0.0, 1.0);
			if (GetHealthRatio(pThis, pType) < hpThresh)
				return nullptr;
		}

		if (pTypeExt->SmartAutoDeploy_SHP_Threshold.isset() && pTypeExt->SmartAutoDeploy_SHP_Threshold_Inverted)
		{
			const double shpThresh = std::clamp(pTypeExt->SmartAutoDeploy_SHP_Threshold.Get(), 0.0, 1.0);
			if (GetShieldRatio(pThis) < shpThresh)
				return nullptr;
		}

		TechnoClass* pBestTarget = nullptr;
		int bestDist = INT_MAX;

		for (auto const pCandidate : TechnoClass::Array)
		{
			if (!pCandidate || pCandidate == pThis || !pCandidate->IsAlive || pCandidate->InLimbo)
				continue;

			if (pCandidate->TemporalTargetingMe || pCandidate->BeingWarpedOut)
				continue;

			if (!pCandidate->Owner || pThis->Owner->IsAlliedWith(pCandidate->Owner) || pCandidate->Owner->IsNeutral())
				continue;

			auto const pCandidateType = pCandidate->GetTechnoType();
			if (!pCandidateType || pCandidateType->Insignificant || pCandidateType->DontScore)
				continue;

			if (auto const pBld = abstract_cast<BuildingClass*>(pCandidate))
			{
				if (pBld->Type->Wall || pBld->Type->Gate)
					continue;
			}

			if (pCandidate->CloakState != CloakState::Uncloaked)
			{
				auto const pCandCell = pCandidate->GetCell();
				const bool sensedByHouse = pCandCell && pCandCell->Sensors_InclHouse(pThis->Owner->ArrayIndex);
				const bool sensedByAlt = pAltType->Sensors && pAltType->SensorsSight > 0
					&& pThis->DistanceFrom(pCandidate) <= pAltType->SensorsSight * 256;
				if (!sensedByHouse && !sensedByAlt)
					continue;
			}

			const auto domain = GetTargetDomain(pCandidate);
			if (!IsTargetDomainAllowed(pAltTypeExt, domain))
				continue;

			auto const pAltWep = GetFormWeapon(pThis, pAltType, !currentlyDeployed, pCandidate);
			if (!pAltWep || !CanFormDamageTarget(pAltType, pAltWep, pCandidate))
				continue;

			const int dist = pThis->DistanceFrom(pCandidate);
			if (pAltWep->MinimumRange > 0 && dist < pAltWep->MinimumRange)
				continue;

			const int airBonus = (pCandidate->IsInAir() && pAltType->AirRangeBonus > 0)
				? pAltType->AirRangeBonus * 256
				: 0;
			const int effectiveRange = pAltWep->Range + airBonus;
			const int deployMargin = std::min(256, effectiveRange / 8);
			const int maxDist = (pAltWep->MinimumRange > 0 && effectiveRange > pAltWep->MinimumRange + deployMargin)
				? std::max(pAltWep->MinimumRange + 128, effectiveRange - deployMargin)
				: effectiveRange - deployMargin;

			if (dist > maxDist)
				continue;

			if (dist < bestDist)
			{
				pBestTarget = pCandidate;
				bestDist = dist;
			}
		}

		return pBestTarget;
	}

	// Evaluates whether a currently undeployed unit should deploy
	// (defensive thresholds, damage comparison, DPS analysis, range advantage).
	bool EvaluateUndeployedCombat(TechnoClass* pThis, AbstractClass* pTarget,
		TechnoTypeClass* pType, TechnoTypeClass* pAltType,
		WeaponTypeClass* pCurrentWeapon, WeaponTypeClass* pAltWeapon,
		int currentDist, bool byWeaponDamage)
	{
		const auto pTypeExt = TechnoTypeExt::Fetch(pType);
		const auto pAltTypeExt = TechnoTypeExt::Fetch(pAltType);
		const bool isMobileUnitConvert = (pType != pAltType && pAltType->Speed > 0 && pAltType->WhatAmI() != AbstractType::BuildingType);

		// ── Guard / AreaGuard mission restrictions ────────────────────────
		if (!isMobileUnitConvert)
		{
			if (pThis->CurrentMission == Mission::Guard)
			{
				if (!(pTypeExt->SmartAutoDeploy_Guard || pAltTypeExt->SmartAutoDeploy_Guard))
					return false;
			}
			else if (pThis->CurrentMission == Mission::Area_Guard)
			{
				if (!(pTypeExt->SmartAutoDeploy_AreaGuard || pAltTypeExt->SmartAutoDeploy_AreaGuard))
					return false;
			}
		}

		bool wantsToDeploy = false;
		bool defensiveDeploy = false;

		// ── HP Threshold ─────────────────────────────────────────────────
		if (pTypeExt->SmartAutoDeploy_HP_Threshold.isset())
		{
			const double hpThresh = std::clamp(pTypeExt->SmartAutoDeploy_HP_Threshold.Get(), 0.0, 1.0);
			const double healthRatio = GetHealthRatio(pThis, pType);

			if (pTypeExt->SmartAutoDeploy_HP_Threshold_Inverted)
			{
				if (healthRatio < hpThresh)
					return false;
			}
			else if (ShouldTriggerThreshold(healthRatio, hpThresh, pTypeExt->SmartAutoDeploy_HP_Threshold_Chance.Get()))
			{
				wantsToDeploy = true;
				defensiveDeploy = true;
			}
		}

		// ── SHP Threshold ────────────────────────────────────────────────
		if (!defensiveDeploy && pTypeExt->SmartAutoDeploy_SHP_Threshold.isset())
		{
			const double shpThresh = std::clamp(pTypeExt->SmartAutoDeploy_SHP_Threshold.Get(), 0.0, 1.0);
			const double shieldRatio = GetShieldRatio(pThis);

			if (pTypeExt->SmartAutoDeploy_SHP_Threshold_Inverted)
			{
				if (shieldRatio < shpThresh)
					return false;
			}
			else if (ShouldTriggerThreshold(shieldRatio, shpThresh, pTypeExt->SmartAutoDeploy_SHP_Threshold_Chance.Get()))
			{
				wantsToDeploy = true;
				defensiveDeploy = true;
			}
		}

		const bool altCanHit = CanFormDamageTarget(pAltType, pAltWeapon, pTarget);

		// Allow emergency dive into a stealth/submerged alternate form even if that form cannot damage the current target
		if (defensiveDeploy && !altCanHit)
		{
			const bool altIsRefuge = (pType != pAltType)
				&& ((pAltType->Underwater && !pType->Underwater) || (pAltType->Cloakable && !pType->Cloakable));
			if (altIsRefuge && CanPerformTransformation(pThis, true))
			{
				ResetSmartAutoDeployState(TechnoExt::Fetch(pThis));
				return ExecuteTransformation(pThis, true);
			}
			return false;
		}

		// ── Can the deployed form even damage this target? ───────────────
		if (!altCanHit)
		{
			// If current form is inside its own MinimumRange and CheckMinimumRange is active, reposition to standoff cell
			if (pCurrentWeapon && pCurrentWeapon->MinimumRange > 0 && currentDist < pCurrentWeapon->MinimumRange
				&& ShouldCheckMinimumRange(pTypeExt, pAltTypeExt)
				&& CanFormDamageTarget(pType, pCurrentWeapon, pTarget))
			{
				if (auto const pStandoffCell = CalculateStandoffCell(pThis, pTarget, pCurrentWeapon))
				{
					if (auto const pFoot = abstract_cast<FootClass*>(pThis))
					{
						auto const pExt = TechnoExt::Fetch(pThis);
						pThis->SetTarget(nullptr);
						pFoot->SetDestination(pStandoffCell, true);
						pFoot->QueueMission(Mission::Move, true);
						pExt->SmartAutoDeploy_SavedTarget = pTarget;
						pExt->SmartAutoDeploy_SavedMission = Mission::Attack;
						pExt->SmartAutoDeploy_IsRepositioning = true;
						pExt->SmartAutoDeploy_RepositionDestination = pStandoffCell->GetCenterCoords();
						pExt->SmartAutoDeploy_TargetAction = SmartAutoDeployAction::None;
						return true;
					}
				}
			}
			return false;
		}

		const auto targetDomain = GetTargetDomain(pTarget);
		if (pType != pAltType && !defensiveDeploy)
		{
			if (!IsTargetDomainAllowed(pAltTypeExt, targetDomain) && IsTargetDomainAllowed(pTypeExt, targetDomain))
				return false;

			if (IsTargetDomainAllowed(pAltTypeExt, targetDomain) && !IsTargetDomainAllowed(pTypeExt, targetDomain))
				wantsToDeploy = true;
		}

		// ── Current form MinimumRange handling (for bidirectional Convert.Deploy units) ──
		const bool currentCanHit = CanFormDamageTarget(pType, pCurrentWeapon, pTarget);
		const bool insideCurrentMinRange = currentCanHit && pCurrentWeapon
			&& pCurrentWeapon->MinimumRange > 0 && currentDist < pCurrentWeapon->MinimumRange;

		if (insideCurrentMinRange)
		{
			if (ShouldCheckMinimumRange(pTypeExt, pAltTypeExt))
			{
				if (auto const pStandoffCell = CalculateStandoffCell(pThis, pTarget, pCurrentWeapon))
				{
					if (auto const pFoot = abstract_cast<FootClass*>(pThis))
					{
						auto const pExt = TechnoExt::Fetch(pThis);
						pThis->SetTarget(nullptr);
						pFoot->SetDestination(pStandoffCell, true);
						pFoot->QueueMission(Mission::Move, true);
						pExt->SmartAutoDeploy_SavedTarget = pTarget;
						pExt->SmartAutoDeploy_SavedMission = Mission::Attack;
						pExt->SmartAutoDeploy_IsRepositioning = true;
						pExt->SmartAutoDeploy_RepositionDestination = pStandoffCell->GetCenterCoords();
						pExt->SmartAutoDeploy_TargetAction = SmartAutoDeployAction::None;
						return true;
					}
				}
			}

			if (currentDist >= pAltWeapon->MinimumRange)
				wantsToDeploy = true;
		}

		// ── Enemy vulnerability & stealth/submerged advantage ─────────────
		const bool currentIsStealthSub = (pType != pAltType)
			&& ((pType->Underwater && !pAltType->Underwater)
				|| (pType->Cloakable && !pAltType->Cloakable && pCurrentWeapon && !pCurrentWeapon->DecloakToFire));
		const bool altIsStealthSub = (pType != pAltType)
			&& ((pAltType->Underwater && !pType->Underwater)
				|| (pAltType->Cloakable && !pType->Cloakable && pAltWeapon && !pAltWeapon->DecloakToFire));

		bool currentHasImmunityAdvantage = false;

		if (!wantsToDeploy)
		{
			auto const pTargetTechno = abstract_cast<TechnoClass*>(pTarget);
			if (pTargetTechno)
			{
				const bool targetCanHitCurrent = CanTargetDamageForm(pTargetTechno, pType, false);
				const bool targetCanHitDeployed = CanTargetDamageForm(pTargetTechno, pAltType, true);

				if (!targetCanHitDeployed && targetCanHitCurrent)
					wantsToDeploy = true;
				else if (!targetCanHitCurrent && targetCanHitDeployed && currentCanHit && !insideCurrentMinRange)
					currentHasImmunityAdvantage = true;
			}

			if (!wantsToDeploy && !currentHasImmunityAdvantage)
			{
				if (altIsStealthSub)
					wantsToDeploy = true;
				else if (currentIsStealthSub && currentCanHit && !insideCurrentMinRange)
					currentHasImmunityAdvantage = true;
			}
		}

		// ── Weapon / DPS comparison ──────────────────────────────────────
		if (!wantsToDeploy && !currentHasImmunityAdvantage)
		{
			auto const pTargetTechno = abstract_cast<TechnoClass*>(pTarget);
			const int currAirBonus = (pTargetTechno && pTargetTechno->IsInAir() && pType->AirRangeBonus > 0)
				? pType->AirRangeBonus * 256
				: 0;
			const int altAirBonus = (pTargetTechno && pTargetTechno->IsInAir() && pAltType->AirRangeBonus > 0)
				? pAltType->AirRangeBonus * 256
				: 0;
			const int currEffectiveRange = pCurrentWeapon ? (pCurrentWeapon->Range + currAirBonus) : 0;
			const int altEffectiveRange = pAltWeapon ? (pAltWeapon->Range + altAirBonus) : 0;
			const int altDeployMargin = std::min(256, altEffectiveRange / 8);
			const int altMaxDeployDist = (pAltWeapon && pAltWeapon->MinimumRange > 0 && altEffectiveRange > pAltWeapon->MinimumRange + altDeployMargin)
				? std::max(pAltWeapon->MinimumRange + 128, altEffectiveRange - altDeployMargin)
				: (altEffectiveRange - altDeployMargin);

			const bool currentInRange = currentCanHit && pCurrentWeapon
				&& currentDist <= currEffectiveRange
				&& currentDist >= pCurrentWeapon->MinimumRange;
			const bool altInRange = altCanHit && pAltWeapon
				&& currentDist <= altMaxDeployDist
				&& currentDist >= pAltWeapon->MinimumRange;

			if (!currentCanHit)
			{
				wantsToDeploy = true;
			}
			else if (!currentInRange && altInRange)
			{
				// Current weapon cannot reach the target at this distance, while the alternate weapon is already in range
				wantsToDeploy = true;
			}
			else if (currentInRange && altInRange)
			{
				// Both forms can hit the target right now (e.g. melee Grinder vs Cannon at point-blank range):
				// prefer whichever form has significantly higher DPS, or longer range if DPS is comparable.
				const double dpsCurrent = CalculateCombatDPS(pType, pCurrentWeapon, pTarget);
				const double dpsDeployed = CalculateCombatDPS(pAltType, pAltWeapon, pTarget);

				if (dpsDeployed > dpsCurrent * 1.15)
					wantsToDeploy = true;
				else if (dpsCurrent > dpsDeployed * 1.15)
					wantsToDeploy = false;
				else if (pCurrentWeapon && pAltWeapon->Range > pCurrentWeapon->Range)
					wantsToDeploy = true;
			}
			else if (byWeaponDamage)
			{
				const double dpsCurrent = CalculateCombatDPS(pType, pCurrentWeapon, pTarget);
				const double dpsDeployed = CalculateCombatDPS(pAltType, pAltWeapon, pTarget);

				if (dpsDeployed > dpsCurrent * 1.10)
					wantsToDeploy = true;
				else if (dpsCurrent > dpsDeployed * 1.10)
					wantsToDeploy = false;
				else if (pCurrentWeapon && pAltWeapon->Range > pCurrentWeapon->Range)
					wantsToDeploy = true;
			}
			else
			{
				if (pCurrentWeapon && pAltWeapon->Range > pCurrentWeapon->Range)
					wantsToDeploy = true;
			}
		}

		if (!wantsToDeploy)
			return false;

		// ── Chance roll ──────────────────────────────────────────────────
		if (!defensiveDeploy && pTypeExt->SmartAutoDeploy_Chance < 1.0)
		{
			if (ScenarioClass::Instance->Random.RandomDouble() > pTypeExt->SmartAutoDeploy_Chance.Get())
				return false;
		}

		// ── MinimumRange repositioning ───────────────────────────────────
		if (pAltWeapon->MinimumRange > 0 && currentDist < pAltWeapon->MinimumRange)
		{
			if (ShouldCheckMinimumRange(pTypeExt, pAltTypeExt))
			{
				if (auto const pStandoffCell = CalculateStandoffCell(pThis, pTarget, pAltWeapon))
				{
					if (auto const pFoot = abstract_cast<FootClass*>(pThis))
					{
						auto const pExt = TechnoExt::Fetch(pThis);
						pThis->SetTarget(nullptr);
						pFoot->SetDestination(pStandoffCell, true);
						pFoot->QueueMission(Mission::Move, true);
						pExt->SmartAutoDeploy_SavedTarget = pTarget;
						pExt->SmartAutoDeploy_SavedMission = Mission::Attack;
						pExt->SmartAutoDeploy_IsRepositioning = true;
						pExt->SmartAutoDeploy_RepositionDestination = pStandoffCell->GetCenterCoords();
						pExt->SmartAutoDeploy_TargetAction = SmartAutoDeployAction::None;
						return true;
					}
				}
			}
			return false;
		}

		// ── Deploy margin: only deploy well within weapon range ──────────
		const bool isMobileStealthSubDive = altIsStealthSub && pAltType->Speed > 0 && pAltType->WhatAmI() != AbstractType::BuildingType;
		if (!defensiveDeploy && !isMobileStealthSubDive)
		{
			auto const pTargetTechno = abstract_cast<TechnoClass*>(pTarget);
			const int airBonus = (pTargetTechno && pTargetTechno->IsInAir() && pAltType->AirRangeBonus > 0)
				? pAltType->AirRangeBonus * 256
				: 0;
			const int effectiveRange = pAltWeapon->Range + airBonus;
			const int deployMargin = std::min(256, effectiveRange / 8);
			const int maxDeployDist = (pAltWeapon->MinimumRange > 0 && effectiveRange > pAltWeapon->MinimumRange + deployMargin)
				? std::max(pAltWeapon->MinimumRange + 128, effectiveRange - deployMargin)
				: effectiveRange - deployMargin;

			if (currentDist > maxDeployDist)
				return false;
		}

		// ── Execute deployment ────────────────────────────────────────────
		if (!CanPerformTransformation(pThis, true))
		{
			// If airborne and needs to land, find a suitable cell
			if (auto const pUnit = abstract_cast<UnitClass*>(pThis))
			{
				if (pUnit->Type->DeployToLand && IsTechnoAirborne(pUnit))
				{
					CellClass* pClearCell = nullptr;
					if (FindClearCellForLanding(nullptr, pUnit, pTarget, pAltWeapon, pClearCell) && pClearCell != pUnit->GetCell())
						pUnit->SetDestination(pClearCell, true);
				}
			}
			return false;
		}

		return SaveAndTransform(pThis, pTarget, SmartAutoDeployAction::Deploy, true, Mission::Attack);
	}

	// ── Combat Evaluator (orchestrator) ──────────────────────────────────

	bool EvaluateCombat(TechnoClass* pThis)
	{
		if (pThis->CurrentMission == Mission::Move)
			return false;

		const auto pType = pThis->GetTechnoType();
		const auto pTypeExt = TechnoTypeExt::Fetch(pType);
		const auto pAltType = GetAlternateType(pThis);
		if (!pAltType)
			return false;

		const bool currentlyDeployed = IsCurrentlyDeployed(pThis);
		auto const pFoot = abstract_cast<FootClass*>(pThis);

		AbstractClass* pTarget = pThis->Target;
		if (!pTarget && (!pFoot || !pFoot->Destination) && IsAliveAndValid(pThis->LastTarget))
		{
			auto const pCurrWep = GetFormWeapon(pThis, pAltType, currentlyDeployed, pThis->LastTarget);
			auto const pAltWep = GetFormWeapon(pThis, pAltType, !currentlyDeployed, pThis->LastTarget);
			const int maxRange = std::max(pCurrWep ? pCurrWep->Range : 0, pAltWep ? pAltWep->Range : 0);
			if (pThis->DistanceFrom(pThis->LastTarget) <= maxRange + 512)
				pTarget = pThis->LastTarget;
		}

		if (!IsAliveAndValid(pTarget))
			pTarget = FindAlternateFormTarget(pThis, pType, pAltType, currentlyDeployed);

		if (!IsAliveAndValid(pTarget))
			return false;

		if (pTarget->WhatAmI() == AbstractType::Cell && pThis->CurrentMission != Mission::Attack)
			return false;

		const auto pAltTypeExt = TechnoTypeExt::Fetch(pAltType);
		const auto targetDomain = GetTargetDomain(pTarget);

		if (!IsTargetDomainAllowed(pTypeExt, targetDomain) && !IsTargetDomainAllowed(pAltTypeExt, targetDomain))
			return false;

		const int currentDist = pThis->DistanceFrom(pTarget);
		auto const pCurrentWeapon = GetFormWeapon(pThis, pAltType, currentlyDeployed, pTarget);
		auto const pAltWeapon = GetFormWeapon(pThis, pAltType, !currentlyDeployed, pTarget);
		const bool byWeaponDamage = pTypeExt->SmartAutoDeploy_ByWeaponDamage || pAltTypeExt->SmartAutoDeploy_ByWeaponDamage;

		if (currentlyDeployed)
			return EvaluateDeployedCombat(pThis, pTarget, pType, pAltType, pCurrentWeapon, pAltWeapon, currentDist, byWeaponDamage);

		if (pAltWeapon)
			return EvaluateUndeployedCombat(pThis, pTarget, pType, pAltType, pCurrentWeapon, pAltWeapon, currentDist, byWeaponDamage);

		return false;
	}

	// ── Travel Evaluator ─────────────────────────────────────────────────

	bool EvaluateTravel(TechnoClass* pThis)
	{
		auto const pFoot = abstract_cast<FootClass*>(pThis);
		if (!pFoot)
			return false;

		const auto pType = pThis->GetTechnoType();
		const auto pTypeExt = TechnoTypeExt::Fetch(pType);
		const auto pAltType = GetAlternateType(pThis);
		if (!pAltType)
			return false;

		const auto pAltTypeExt = TechnoTypeExt::Fetch(pAltType);
		if (!pTypeExt->SmartAutoDeploy_Travel && !pAltTypeExt->SmartAutoDeploy_Travel)
			return false;

		const auto mission = pThis->CurrentMission;
		const bool isPureMove = (mission == Mission::Move || mission == Mission::AttackMove || mission == Mission::Patrol);
		const bool isCombatOrGuardMove = (mission == Mission::Attack || mission == Mission::Area_Guard || mission == Mission::Guard || mission == Mission::Hunt);

		if (!isPureMove && !isCombatOrGuardMove)
			return false;

		const bool currentlyDeployed = IsCurrentlyDeployed(pThis);

		// ── 1. Check if the unit is currently fighting an enemy in weapon range ──
		if (IsAliveAndValid(pThis->Target))
		{
			auto const pTargetTechno = abstract_cast<TechnoClass*>(pThis->Target);
			auto const pCurrWep = GetFormWeapon(pThis, pAltType, currentlyDeployed, pThis->Target);
			auto const pAltWep = GetFormWeapon(pThis, pAltType, !currentlyDeployed, pThis->Target);
			const int distToTarget = pThis->DistanceFrom(pThis->Target);

			const int currAirBonus = (pTargetTechno && pTargetTechno->IsInAir() && pType->AirRangeBonus > 0)
				? pType->AirRangeBonus * 256
				: 0;
			const int altAirBonus = (pTargetTechno && pTargetTechno->IsInAir() && pAltType->AirRangeBonus > 0)
				? pAltType->AirRangeBonus * 256
				: 0;
			const int currEffectiveRange = pCurrWep ? (pCurrWep->Range + currAirBonus) : 0;
			const int altEffectiveRange = pAltWep ? (pAltWep->Range + altAirBonus) : 0;

			const bool targetInCurrentRange = pCurrWep
				&& distToTarget <= currEffectiveRange
				&& distToTarget >= pCurrWep->MinimumRange
				&& CanFormDamageTarget(pType, pCurrWep, pThis->Target);

			if (isPureMove)
			{
				if (targetInCurrentRange && pType->OpportunityFire)
				{
					// Actively opportunity-firing while moving: stay in the OpportunityFire form until out of range or dead
					return false;
				}

				// Target is out of range (left behind while moving) or current form has no OpportunityFire:
				// clear target so the unit can resume fast travel toward its Destination.
				if (!targetInCurrentRange)
				{
					pThis->SetTarget(nullptr);
					pThis->LastTarget = nullptr;
				}
			}
			else
			{
				const bool targetInAltRange = pAltWep
					&& distToTarget <= altEffectiveRange
					&& distToTarget >= pAltWep->MinimumRange
					&& CanFormDamageTarget(pAltType, pAltWep, pThis->Target);

				if (targetInCurrentRange || targetInAltRange)
				{
					// In combat range of the target — let EvaluateCombat manage form selection
					return false;
				}

				// Target is still out of both weapons' ranges (chasing/approaching from afar):
				// if current form has OpportunityFire and another enemy is right beside us, keep OpportunityFire active
				if (pType->OpportunityFire && !pAltType->OpportunityFire)
				{
					if (FindOpportunityFireTarget(pThis, pAltType, pType, !currentlyDeployed))
						return false;
				}
			}
		}

		// Determine travel destination (movement Destination or distant Attack Target)
		AbstractClass* pTravelDest = pFoot->Destination;
		if (IsAliveAndValid(pThis->Target))
		{
			if (!pTravelDest || pThis->DistanceFrom(pThis->Target) > pThis->DistanceFrom(pTravelDest))
				pTravelDest = pThis->Target;
		}

		if (!pTravelDest)
			return false;

		// ── 2. OpportunityFire engagement while traveling in the fast non-OpportunityFire form ──
		// If currently moving in the faster form (e.g. RA3APOC2) and the alternate form (e.g. RA3APOC)
		// has OpportunityFire=yes, switch to the OpportunityFire form when an enemy enters its weapon range
		// while preserving Destination and Mission so it rotates its turret and shoots without deviating.
		if (pType != pAltType && !pType->OpportunityFire && pAltType->OpportunityFire && pType->Speed > pAltType->Speed)
		{
			if (auto const pOppTarget = FindOpportunityFireTarget(pThis, pType, pAltType, currentlyDeployed))
			{
				ResetSmartAutoDeployState(TechnoExt::Fetch(pThis));
				return ExecuteTravelTransformation(pThis, !currentlyDeployed, pOppTarget);
			}
		}

		// ── 3. Transform to the faster travel form when not fighting ──
		const int minTravelDistCells = (pType != pAltType && !pTypeExt->SmartAutoDeploy_Travel && pAltTypeExt->SmartAutoDeploy_Travel)
			? pAltTypeExt->SmartAutoDeploy_TravelMinDistance.Get()
			: pTypeExt->SmartAutoDeploy_TravelMinDistance.Get();
		const int minTravelDistLeptons = minTravelDistCells * 256;
		const int distToTravelDest = pThis->DistanceFrom(pTravelDest);

		const int currentSpeed = pType->Speed;
		const int altSpeed = pAltType->Speed;

		// Allow resuming the fast travel form after an OpportunityFire engagement along the way
		// even if the remaining distance to Destination is now under TravelMinDistance (as long as >= 2.5 cells remain).
		const bool resumingAfterOpportunityFire = (pType != pAltType && pType->OpportunityFire && !pAltType->OpportunityFire
			&& altSpeed > currentSpeed && distToTravelDest >= 640);

		// Also allow chasing a distant combat target when TargetChase is enabled and target is well beyond weapon range.
		bool chasingDistantTarget = false;
		if (isCombatOrGuardMove && IsAliveAndValid(pThis->Target) && altSpeed > currentSpeed
			&& (pTypeExt->SmartAutoDeploy_TargetChase || pAltTypeExt->SmartAutoDeploy_TargetChase))
		{
			auto const pCurrWep = GetFormWeapon(pThis, pAltType, currentlyDeployed, pThis->Target);
			const int chaseThreshold = pCurrWep ? (pCurrWep->Range + 384) : minTravelDistLeptons;
			if (distToTravelDest >= chaseThreshold)
				chasingDistantTarget = true;
		}

		if (distToTravelDest < minTravelDistLeptons && !resumingAfterOpportunityFire && !chasingDistantTarget)
			return false;

		bool shouldTransformForTravel = false;
		if (pType != pAltType && pTypeExt->SmartAutoDeploy_Travel != pAltTypeExt->SmartAutoDeploy_Travel)
		{
			shouldTransformForTravel = pAltTypeExt->SmartAutoDeploy_Travel.Get();
		}
		else
		{
			shouldTransformForTravel = (pType == pAltType)
				? (currentlyDeployed && (currentSpeed > 0 || pType->MovementZone == MovementZone::Fly))
				: ((altSpeed > currentSpeed) ||
					(pAltType->MovementZone == MovementZone::Fly && pType->MovementZone != MovementZone::Fly) ||
					(pAltType->SpeedType == SpeedType::Hover && pType->SpeedType != SpeedType::Hover && pType->SpeedType != SpeedType::Winged));
		}

		if (shouldTransformForTravel)
		{
			ResetSmartAutoDeployState(TechnoExt::Fetch(pThis));
			AbstractClass* const pKeepTarget = isPureMove ? nullptr : pThis->Target;
			return ExecuteTravelTransformation(pThis, !currentlyDeployed, pKeepTarget);
		}

		return false;
	}

	// ── Idle Evaluator ───────────────────────────────────────────────────

	bool EvaluateIdle(TechnoClass* pThis)
	{
		const auto pType = pThis->GetTechnoType();
		const auto pTypeExt = TechnoTypeExt::Fetch(pType);
		const auto pAltType = GetAlternateType(pThis);
		if (!pAltType)
			return false;

		const auto pAltTypeExt = TechnoTypeExt::Fetch(pAltType);
		const bool currentlyDeployed = IsCurrentlyDeployed(pThis);

		if (pType == pAltType)
		{
			if (!pTypeExt->SmartAutoDeploy_Idle)
				return false;

			if (!currentlyDeployed)
			{
				auto const pExt = TechnoExt::Fetch(pThis);
				pExt->SmartAutoDeploy_IdleTimer.Stop();
				return false;
			}
		}
		else
		{
			if (pTypeExt->SmartAutoDeploy_Idle == pAltTypeExt->SmartAutoDeploy_Idle)
				return false;

			if (pTypeExt->SmartAutoDeploy_Idle)
			{
				auto const pExt = TechnoExt::Fetch(pThis);
				pExt->SmartAutoDeploy_IdleTimer.Stop();
				return false;
			}
		}

		if (pThis->Target || pThis->CurrentMission == Mission::Attack || pThis->CurrentMission == Mission::Move)
			return false;

		auto const pFoot = abstract_cast<FootClass*>(pThis);
		if (pFoot)
		{
			if (pFoot->Destination)
				return false;

			if (pFoot->Locomotor && pFoot->Locomotor->Is_Moving_Now())
				return false;
		}

		auto const pExt = TechnoExt::Fetch(pThis);

		if (!pExt->SmartAutoDeploy_IdleTimer.IsTicking())
		{
			const int delay = (pType != pAltType && pAltTypeExt->SmartAutoDeploy_Idle && pTypeExt->SmartAutoDeploy_IdleDelay == 60)
				? pAltTypeExt->SmartAutoDeploy_IdleDelay.Get()
				: pTypeExt->SmartAutoDeploy_IdleDelay.Get();
			pExt->SmartAutoDeploy_IdleTimer.Start(delay);
			return false;
		}

		if (pExt->SmartAutoDeploy_IdleTimer.Completed())
		{
			pExt->SmartAutoDeploy_IdleTimer.Stop();
			ResetSmartAutoDeployState(pExt);
			return ExecuteTransformation(pThis, !currentlyDeployed);
		}

		return false;
	}

	// ── Update Sub-handlers ──────────────────────────────────────────────

	// Handles the repositioning state machine:
	// tracks destination, validates target, and re-engages when arrived.
	void HandleRepositioning(TechnoClass* pThis, TechnoExt* pExt)
	{
		if (!IsAliveAndValid(pExt->SmartAutoDeploy_SavedTarget))
		{
			ResetSmartAutoDeployState(pExt);
			return;
		}

		auto const pFoot = abstract_cast<FootClass*>(pThis);
		auto const pJJLoco = (pFoot && pFoot->Locomotor)
			? locomotion_cast<JumpjetLocomotionClass*>(pFoot->Locomotor)
			: nullptr;

		// Calculate 2D distance to destination (ignoring altitude)
		const double distToDest2D = (pExt->SmartAutoDeploy_RepositionDestination != CoordStruct::Empty)
			? DistanceXY(pThis->GetCoords(), pExt->SmartAutoDeploy_RepositionDestination)
			: 0.0;

		const bool arrivedAtDest = (pExt->SmartAutoDeploy_RepositionDestination != CoordStruct::Empty && distToDest2D <= 384.0);

		if (!arrivedAtDest)
		{
			// Check if player overrode the destination
			CellClass* pCurrentDestCell = nullptr;
			if (pFoot && pFoot->Destination)
			{
				if (auto const pCell = abstract_cast<CellClass*>(pFoot->Destination))
					pCurrentDestCell = pCell;
				else if (auto const pObj = abstract_cast<ObjectClass*>(pFoot->Destination))
					pCurrentDestCell = pObj->GetCell();
				else
					pCurrentDestCell = MapClass::Instance.TryGetCellAt(CellClass::Coord2Cell(pFoot->Destination->GetCoords()));
			}

			const CellStruct expectedRepoCell = CellClass::Coord2Cell(pExt->SmartAutoDeploy_RepositionDestination);
			if (!pCurrentDestCell || pCurrentDestCell->MapCoords != expectedRepoCell)
			{
				// Player overrode destination — abort repositioning
				ResetSmartAutoDeployState(pExt);
				pThis->SetTarget(nullptr);
				pThis->LastTarget = nullptr;
				return;
			}

			// Still in transit — let it fly
			if (pJJLoco)
			{
				if (pJJLoco->State == JumpjetLocomotionClass::State::Ascending ||
					pJJLoco->State == JumpjetLocomotionClass::State::Cruising)
				{
					return;
				}
			}
			else if (pFoot && pFoot->Locomotor && pFoot->Locomotor->Is_Moving_Now())
			{
				return;
			}
		}

		// Arrived at repositioning destination — re-engage target
		pExt->SmartAutoDeploy_IsRepositioning = false;
		pExt->SmartAutoDeploy_RepositionDestination = CoordStruct::Empty;

		auto const pTarget = pExt->SmartAutoDeploy_SavedTarget;
		pExt->SmartAutoDeploy_SavedTarget = nullptr;
		pExt->SmartAutoDeploy_SavedMission = Mission::None;

		if (pTarget && IsAliveAndValid(pTarget))
		{
			pThis->SetTarget(pTarget);
			pThis->LastTarget = pTarget;
			pThis->QueueMission(Mission::Attack, false);
			pThis->NextMission();

			EvaluateCombat(pThis);
		}
	}

	// Tracks deploy/undeploy animation progress and restores target/mission
	// when transformation completes.
	void HandleTransitionCompletion(TechnoClass* pThis, TechnoExt* pExt)
	{
		auto const pUnit = abstract_cast<UnitClass*>(pThis);

		// If the saved target died while an airborne DeployToLand unit was still descending
		// before starting its deploy animation, abort the landing immediately so it stays airborne.
		if (!IsAliveAndValid(pExt->SmartAutoDeploy_SavedTarget)
			&& pExt->SmartAutoDeploy_TargetAction == SmartAutoDeployAction::Deploy
			&& pUnit && pUnit->Type->DeployToLand
			&& !pUnit->Deployed && !pUnit->Deploying
			&& IsTechnoAirborne(pUnit))
		{
			pUnit->InAir = false;
			pUnit->QueueMission(Mission::Guard, true);
			ResetSmartAutoDeployState(pExt);
			return;
		}

		// Step 1: Wait if transformation is still animating
		if (IsTransformInProgress(pThis, pUnit, pExt->SmartAutoDeploy_TargetAction))
			return;

		const bool completed = IsTransformCompleted(pThis, pUnit, pExt->SmartAutoDeploy_TargetAction);

		// Step 2: Validate saved target
		if (!IsAliveAndValid(pExt->SmartAutoDeploy_SavedTarget))
		{
			ResetSmartAutoDeployState(pExt);
			return;
		}

		const Mission resumeMission = (pExt->SmartAutoDeploy_SavedMission != Mission::None)
			? pExt->SmartAutoDeploy_SavedMission
			: Mission::Attack;

		// Step 3: Handle repositioning continuation after undeploy
		if (completed && pExt->SmartAutoDeploy_IsRepositioning)
		{
			const auto pAltType = GetAlternateType(pThis);
			auto const pAltWeapon = GetFormWeapon(pThis, pAltType, true, pExt->SmartAutoDeploy_SavedTarget);
			if (auto const pStandoffCell = CalculateStandoffCell(pThis, pExt->SmartAutoDeploy_SavedTarget, pAltWeapon))
			{
				if (auto const pFoot = abstract_cast<FootClass*>(pThis))
				{
					pThis->SetTarget(nullptr);
					pFoot->SetDestination(pStandoffCell, true);
					pFoot->QueueMission(Mission::Move, true);
					pExt->SmartAutoDeploy_RepositionDestination = pStandoffCell->GetCenterCoords();
					pExt->SmartAutoDeploy_TargetAction = SmartAutoDeployAction::None;
					return;
				}
			}
			pExt->SmartAutoDeploy_IsRepositioning = false;
		}

		if (!completed)
			pExt->SmartAutoDeploy_IsRepositioning = false;

		// Step 4: Abort if player issued manual move
		if (pThis->CurrentMission == Mission::Move)
		{
			ResetSmartAutoDeployState(pExt);
			return;
		}

		// Step 5: Restore target and resume mission
		pThis->SetTarget(pExt->SmartAutoDeploy_SavedTarget);
		pThis->LastTarget = pExt->SmartAutoDeploy_SavedTarget;
		pThis->QueueMission(resumeMission, false);
		pThis->NextMission();

		// Step 6: If airborne after undeploy, find landing cell
		if (!completed && pUnit && pUnit->Type->DeployToLand && IsTechnoAirborne(pUnit))
		{
			const auto pAltType = GetAlternateType(pThis);
			auto const pAltWeapon = GetFormWeapon(pThis, pAltType, true, pExt->SmartAutoDeploy_SavedTarget);
			CellClass* pClearCell = nullptr;
			if (FindClearCellForLanding(nullptr, pUnit, pExt->SmartAutoDeploy_SavedTarget, pAltWeapon, pClearCell) && pClearCell != pUnit->GetCell())
				pUnit->SetDestination(pClearCell, true);
		}

		// Step 7: Clean up
		ResetSmartAutoDeployState(pExt);
	}

	// ── Main Update Entry Point ──────────────────────────────────────────

	void Update(TechnoClass* pThis)
	{
		if ((pThis->UniqueID + Unsorted::CurrentFrame) % 4 != 0)
			return;

		if (!ShouldEvaluate(pThis))
			return;

		// A building that is being constructed (deployed unit) or sold (undeploying) must not be touched,
		// changing its mission here interrupts the build-up/sell animation and leaves the body undrawn.
		if (auto const pBuilding = abstract_cast<BuildingClass*>(pThis))
		{
			if (pBuilding->CurrentMission == Mission::Construction || pBuilding->QueuedMission == Mission::Construction
				|| pBuilding->CurrentMission == Mission::Selling || pBuilding->QueuedMission == Mission::Selling)
				return;
		}

		auto const pExt = TechnoExt::Fetch(pThis);

		// Handle active repositioning maneuver
		if (pExt->SmartAutoDeploy_IsRepositioning && pExt->SmartAutoDeploy_TargetAction == SmartAutoDeployAction::None)
		{
			HandleRepositioning(pThis, pExt);
			return;
		}

		// Handle deploy/undeploy transition completion
		if (pExt->SmartAutoDeploy_TargetAction != SmartAutoDeployAction::None || pExt->SmartAutoDeploy_SavedTarget)
		{
			// Buildings only keep the saved tactical state to hand it over to the unit they undeploy into,
			// they never resume missions by themselves.
			if (abstract_cast<BuildingClass*>(pThis))
			{
				if (pExt->SmartAutoDeploy_TargetAction != SmartAutoDeployAction::None)
					return;
			}
			else
			{
				HandleTransitionCompletion(pThis, pExt);
				return;
			}
		}

		// Standard evaluation pipeline
		if (EvaluateCombat(pThis))
			return;

		if (EvaluateTravel(pThis))
			return;

		EvaluateIdle(pThis);
	}

} // namespace SmartAutoDeploy

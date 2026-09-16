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

		if (auto const pUnitType = abstract_cast<UnitTypeClass*, true>(pType))
		{
			if (pUnitType->IsSimpleDeployer || pUnitType->DeployFire)
				return 1;
		}

		return -1;
	}

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

		const int selected = pThis ? pThis->SelectWeapon(pTarget) : 0;
		return selected >= 0 ? selected : 0;
	}

	WeaponTypeClass* GetFormWeapon(TechnoClass* pThis, TechnoTypeClass* pAltType, bool deployedForm, AbstractClass* pTarget)
	{
		const bool currentlyDeployed = SmartAutoDeploy::IsCurrentlyDeployed(pThis);
		const auto pCurrentType = pThis ? pThis->GetTechnoType() : nullptr;

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
		const auto pTargetType = (deployedForm == currentlyDeployed) ? pCurrentType : pAltType;
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

	bool CanEnemyWeaponHitForm(WeaponTypeClass* pWeapon, TechnoTypeClass* pEnemyType, TechnoTypeClass* pFormType)
	{
		if (!pWeapon || !pWeapon->Warhead || !pFormType)
			return false;

		if (GeneralUtils::GetWarheadVersusArmor(pWeapon->Warhead, pFormType->Armor) <= 0.0)
			return false;

		const bool formIsAir = (pFormType->SpeedType == SpeedType::Winged || pFormType->MovementZone == MovementZone::Fly);
		const bool formIsUnderwater = pFormType->Underwater;

		if (formIsAir)
			return pWeapon->Projectile && pWeapon->Projectile->AA;

		if (formIsUnderwater)
			return CanTypeHitUnderwater(pEnemyType);

		return !pWeapon->Projectile || pWeapon->Projectile->AG || !pWeapon->Projectile->AA;
	}

	bool CanTargetDamageForm(TechnoClass* pTargetTechno, TechnoTypeClass* pFormType)
	{
		if (!pTargetTechno || !pFormType)
			return true;

		const auto pEnemyType = pTargetTechno->GetTechnoType();
		const int enemyWeaponCount = GetTechnoTypeWeaponCount(pEnemyType);
		for (int i = 0; i < enemyWeaponCount; ++i)
		{
			if (auto const pWeaponStruct = pTargetTechno->GetWeapon(i))
			{
				if (CanEnemyWeaponHitForm(pWeaponStruct->WeaponType, pEnemyType, pFormType))
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

		if (pUnit && pUnit->Type->DeployToLand)
		{
			if (pCell->LandType == LandType::Water || pCell->LandType == LandType::Beach)
				return false;
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

	bool ShouldCheckMinimumRange(TechnoClass* pThis, TechnoTypeExt* pTypeExt, TechnoTypeExt* pAltTypeExt)
	{
		const bool isAI = !pThis->Owner || !pThis->Owner->IsControlledByHuman();
		return isAI || pTypeExt->SmartAutoDeploy_CheckMinimumRange.Get() || pAltTypeExt->SmartAutoDeploy_CheckMinimumRange.Get();
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
					return true;

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

		if (pThis->WhatAmI() == AbstractType::Unit)
		{
			const auto pUnit = static_cast<UnitClass*>(pThis);
			if (pUnit->Type->IsSimpleDeployer)
				return pUnit->Deployed;
		}

		const auto pTypeExt = TechnoTypeExt::Fetch(pThis->GetTechnoType());
		return pTypeExt->Convert_Undeploy != nullptr;
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
					return TechnoExt::ConvertToType(pFoot, pTypeExt->Convert_Deploy);
				}
			}
			else if (auto const pUnit = abstract_cast<UnitClass*>(pThis))
			{
				if (pType->DeploysInto || pUnit->Type->IsSimpleDeployer)
				{
					StopTechnoMotion(pUnit);
					pUnit->QueueMission(Mission::Unload, true);
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
					return TechnoExt::ConvertToType(pFoot, pTypeExt->Convert_Undeploy);
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

	// Saves tactical state and executes a deploy/undeploy transformation.
	// Consolidates repeated save-target + set-action + execute pattern.
	bool SaveAndTransform(TechnoClass* pThis, AbstractClass* pTarget,
		SmartAutoDeployAction action, bool toDeploy,
		Mission savedMission = Mission::Attack, bool repositioning = false)
	{
		auto const pExt = TechnoExt::Fetch(pThis);
		pExt->SmartAutoDeploy_SavedTarget = pTarget;
		pExt->SmartAutoDeploy_SavedMission = savedMission;
		pExt->SmartAutoDeploy_TargetAction = action;
		pExt->SmartAutoDeploy_IsRepositioning = repositioning;
		return ExecuteTransformation(pThis, toDeploy);
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
			if (ShouldCheckMinimumRange(pThis, pTypeExt, pAltTypeExt))
				return SaveAndTransform(pThis, pTarget, SmartAutoDeployAction::Undeploy, false, Mission::Attack, true);
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

	// Evaluates whether a currently undeployed unit should deploy
	// (defensive thresholds, damage comparison, DPS analysis, range advantage).
	bool EvaluateUndeployedCombat(TechnoClass* pThis, AbstractClass* pTarget,
		TechnoTypeClass* pType, TechnoTypeClass* pAltType,
		WeaponTypeClass* pCurrentWeapon, WeaponTypeClass* pAltWeapon,
		int currentDist, bool byWeaponDamage)
	{
		const auto pTypeExt = TechnoTypeExt::Fetch(pType);
		const auto pAltTypeExt = TechnoTypeExt::Fetch(pAltType);

		// ── Guard / AreaGuard mission restrictions ────────────────────────
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

		// ── Can the deployed form even damage this target? ───────────────
		if (!CanFormDamageTarget(pAltType, pAltWeapon, pTarget))
			return false;

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

		// ── Enemy vulnerability: deploy if enemy can't hit deployed form ─
		if (!wantsToDeploy)
		{
			auto const pTargetTechno = abstract_cast<TechnoClass*>(pTarget);
			if (pTargetTechno)
			{
				const bool targetCanHitCurrent = CanTargetDamageForm(pTargetTechno, pType);
				const bool targetCanHitDeployed = CanTargetDamageForm(pTargetTechno, pAltType);

				if (!targetCanHitDeployed && targetCanHitCurrent)
					wantsToDeploy = true;
			}
		}

		// ── Weapon / DPS comparison ──────────────────────────────────────
		if (!wantsToDeploy)
		{
			const bool currentCanHit = CanFormDamageTarget(pType, pCurrentWeapon, pTarget);

			if (byWeaponDamage)
			{
				if (!currentCanHit)
				{
					wantsToDeploy = true;
				}
				else
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
			}
			else
			{
				if (!currentCanHit)
					wantsToDeploy = true;
				else if (pCurrentWeapon && pAltWeapon->Range > pCurrentWeapon->Range)
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
			if (ShouldCheckMinimumRange(pThis, pTypeExt, pAltTypeExt))
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
						pExt->SmartAutoDeploy_SavedMission = pThis->CurrentMission;
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
		const int deployMargin = std::min(256, pAltWeapon->Range / 8);
		const int maxDeployDist = (pAltWeapon->MinimumRange > 0 && pAltWeapon->Range > pAltWeapon->MinimumRange + deployMargin)
			? std::max(pAltWeapon->MinimumRange + 128, pAltWeapon->Range - deployMargin)
			: pAltWeapon->Range - deployMargin;

		if (currentDist > maxDeployDist)
			return false;

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

		return SaveAndTransform(pThis, pTarget, SmartAutoDeployAction::Deploy, true, pThis->CurrentMission);
	}

	// ── Combat Evaluator (orchestrator) ──────────────────────────────────

	bool EvaluateCombat(TechnoClass* pThis)
	{
		if (pThis->CurrentMission == Mission::Move)
			return false;

		AbstractClass* pTarget = pThis->Target;
		if (!pTarget)
			pTarget = pThis->LastTarget;

		if (!IsAliveAndValid(pTarget))
			return false;

		if (pTarget->WhatAmI() == AbstractType::Cell && pThis->CurrentMission != Mission::Attack)
			return false;

		const auto pType = pThis->GetTechnoType();
		const auto pTypeExt = TechnoTypeExt::Fetch(pType);
		const auto pAltType = GetAlternateType(pThis);
		if (!pAltType)
			return false;

		const auto pAltTypeExt = TechnoTypeExt::Fetch(pAltType);
		const auto targetDomain = GetTargetDomain(pTarget);

		if (!IsTargetDomainAllowed(pTypeExt, targetDomain) && !IsTargetDomainAllowed(pAltTypeExt, targetDomain))
			return false;

		const bool currentlyDeployed = IsCurrentlyDeployed(pThis);
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
		if (!pFoot || !pFoot->Destination)
			return false;

		const auto pType = pThis->GetTechnoType();
		const auto pTypeExt = TechnoTypeExt::Fetch(pType);
		const auto pAltType = GetAlternateType(pThis);
		if (!pAltType)
			return false;

		const auto pAltTypeExt = TechnoTypeExt::Fetch(pAltType);
		if (!pTypeExt->SmartAutoDeploy_Travel && !pAltTypeExt->SmartAutoDeploy_Travel)
			return false;

		if (pThis->CurrentMission != Mission::Move)
			return false;

		const int minTravelDistCells = pTypeExt->SmartAutoDeploy_TravelMinDistance.Get();
		const int minTravelDistLeptons = minTravelDistCells * 256;

		if (pThis->DistanceFrom(pFoot->Destination) < minTravelDistLeptons)
			return false;

		const bool currentlyDeployed = IsCurrentlyDeployed(pThis);
		const int currentSpeed = pType->Speed;
		const int altSpeed = pAltType->Speed;

		const bool altHasBetterMobility = (altSpeed > currentSpeed) ||
			(pAltType->MovementZone == MovementZone::Fly && pType->MovementZone != MovementZone::Fly) ||
			(pAltType->SpeedType == SpeedType::Hover && pType->SpeedType != SpeedType::Hover && pType->SpeedType != SpeedType::Winged);

		if (altHasBetterMobility)
			return ExecuteTransformation(pThis, !currentlyDeployed);

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

		if (pTypeExt->SmartAutoDeploy_Idle == pAltTypeExt->SmartAutoDeploy_Idle)
			return false;

		if (pTypeExt->SmartAutoDeploy_Idle)
		{
			auto const pExt = TechnoExt::Fetch(pThis);
			pExt->SmartAutoDeploy_IdleTimer.Stop();
			return false;
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
			const int delay = pTypeExt->SmartAutoDeploy_IdleDelay.Get();
			pExt->SmartAutoDeploy_IdleTimer.Start(delay);
			return false;
		}

		if (pExt->SmartAutoDeploy_IdleTimer.Completed())
		{
			pExt->SmartAutoDeploy_IdleTimer.Stop();
			const bool currentlyDeployed = IsCurrentlyDeployed(pThis);
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

		auto const pExt = TechnoExt::Fetch(pThis);

		// Handle active repositioning maneuver
		if (pExt->SmartAutoDeploy_IsRepositioning && pExt->SmartAutoDeploy_TargetAction == SmartAutoDeployAction::None)
		{
			HandleRepositioning(pThis, pExt);
			return;
		}

		// Handle deploy/undeploy transition completion
		if (pExt->SmartAutoDeploy_SavedTarget)
		{
			HandleTransitionCompletion(pThis, pExt);
			return;
		}

		// Standard evaluation pipeline
		if (EvaluateCombat(pThis))
			return;

		if (EvaluateTravel(pThis))
			return;

		EvaluateIdle(pThis);
	}

} // namespace SmartAutoDeploy

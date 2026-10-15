#include "Body.h"

#include <JumpjetLocomotionClass.h>
#include <FileSystem.h>
#include <Surface.h>
#include <TacticalClass.h>

#include <Ext/Rules/Body.h>
#include <Ext/Scenario/Body.h>
#include <Ext/UnitType/Body.h>
#include <Utilities/AresFunctions.h>

UnitExt::ExtContainer UnitExt::ExtMap;

UnitClass* UnitExt::Deployer = nullptr;

UnitExt::~UnitExt()
{
	if (this->UndergroundTracked)
		ScenarioExt::Global()->UndergroundTracker.Remove(this->OwnerObject());

	if (this->CarryallCargo)
	{
		this->CarryallCargo->Transporter = nullptr;
		this->CarryallCargo->IsOnCarryall = false;
		this->CarryallCargo = nullptr;
	}
}

// =============================
// load / save

template <typename T>
void UnitExt::Serialize(T& Stm)
{
	Stm
		.Process(this->SubterraneanHarvStatus)
		.Process(this->SubterraneanHarvRallyPoint)
		.Process(this->ReceiveDamage)
		.Process(this->DeployFireTimer)
		.Process(this->KeepTargetOnMove)
		.Process(this->SimpleDeployerAnimationTimer)
		.Process(this->ExtraTurretRecoil)
		.Process(this->ExtraBarrelRecoil)
		.Process(this->CarryallState)
		.Process(this->CarryallCargo)
		.Process(this->CarryallTarget)
		.Process(this->CarryallDropOnArrival)
		.Process(this->CarryallAlignTimer)
		;
}

// Subterranean harvester factory exit state machine.
void UnitExt::UpdateSubterraneanHarvester()
{
	auto const pThis = this->OwnerObject();

	// Unnecessary for AI players.
	if (!pThis->Owner->IsControlledByHuman())
		return;

	switch (this->SubterraneanHarvStatus)
	{
	case 0: // No state to handle.
		break;
	case 1: // Unit has been created.
		// If we're still in the factory do not advance.
		if (pThis->HasAnyLink())
			break;

		pThis->ClearNavigationList();

		// If we have rally point available, move to it and advance to next state, otherwise end here.
		if (this->SubterraneanHarvRallyPoint)
		{
			pThis->SetDestination(this->SubterraneanHarvRallyPoint, false);
			pThis->QueueMission(Mission::Move, true);
			this->SubterraneanHarvRallyPoint = nullptr;
			this->SubterraneanHarvStatus = 2;
			break;
		}
		else
		{
			this->SubterraneanHarvStatus = 0;
		}

		break;
	case 2: // Out of factory and on move.
		// If we're still moving don't start harvesting.
		if (pThis->Destination || pThis->CurrentMission == Mission::Move)
			break;

		// If harvester stops moving and becomes anything except idle, reset the state machine.
		if (pThis->CurrentMission != Mission::Guard)
		{
			this->SubterraneanHarvStatus = 0;
			break;
		}

		// Go harvest ore.
		pThis->ClearNavigationList();
		pThis->QueueMission(Mission::Harvest, true);
		this->SubterraneanHarvStatus = 0;
		break;
	default:
		break;
	}
}

// Resets target if KeepTargetOnMove unit moves beyond weapon range.
void UnitExt::UpdateKeepTargetOnMove()
{
	if (!this->KeepTargetOnMove)
		return;

	auto const pThis = this->OwnerObject();

	if (!pThis->Target)
	{
		this->KeepTargetOnMove = false;
		return;
	}

	const auto pTypeExt = this->TypeExtData;

	if (!pTypeExt->KeepTargetOnMove)
	{
		pThis->SetTarget(nullptr);
		this->KeepTargetOnMove = false;
		return;
	}

	if (pThis->CurrentMission == Mission::Guard)
	{
		if (!pTypeExt->KeepTargetOnMove_NoMorePursuit)
		{
			pThis->QueueMission(Mission::Attack, false);
			this->KeepTargetOnMove = false;
			return;
		}
	}
	else if (pThis->CurrentMission != Mission::Move)
	{
		return;
	}

	const int weaponIndex = pTypeExt->KeepTargetOnMove_Weapon >= 0 ? pTypeExt->KeepTargetOnMove_Weapon : pThis->SelectWeapon(pThis->Target);

	if (auto const pWeapon = pThis->GetWeapon(weaponIndex)->WeaponType)
	{
		const int extraDistance = static_cast<int>(pTypeExt->KeepTargetOnMove_ExtraDistance.Get());
		const int range = pWeapon->Range;
		pWeapon->Range += extraDistance; // Temporarily adjust weapon range based on the extra distance.

		if (!pThis->IsCloseEnough(pThis->Target, weaponIndex))
		{
			pThis->SetTarget(nullptr);
			this->KeepTargetOnMove = false;
		}

		pWeapon->Range = range;
	}
}

// Queues or cancels auto-deploy depending on the unit's remaining ammo.
void UnitExt::DepletedAmmoActions()
{
	auto const pTypeExt = this->GetTypeExtData();
	const int min = pTypeExt->Ammo_AutoDeployMinimumAmount;
	const int max = pTypeExt->Ammo_AutoDeployMaximumAmount;

	if (min < 0 && max < 0)
		return;

	auto const pType = pTypeExt->OwnerObject();

	if (pType->Ammo <= 0)
		return;

	auto const pThis = this->OwnerObject();
	auto const pUnitType = pThis->Type;

	if (!pUnitType->IsSimpleDeployer && !pUnitType->DeploysInto && !pUnitType->DeployFire
		&& pUnitType->Passengers < 1 && pThis->Passengers.NumPassengers < 1)
	{
		return;
	}

	const int ammo = pThis->Ammo;
	const bool canDeploy = UnitExt::HasAmmoToDeploy(pThis) && (min < 0 || ammo >= min) && (max < 0 || ammo <= max);
	const bool isDeploying = pThis->CurrentMission == Mission::Unload || pThis->QueuedMission == Mission::Unload;

	if (canDeploy && !isDeploying)
	{
		pThis->QueueMission(Mission::Unload, true);
	}
	else if (!canDeploy && isDeploying)
	{
		pThis->QueueMission(Mission::Guard, true);

		if (pUnitType->IsSimpleDeployer && pThis->InAir)
		{
			if (auto const pJJLoco = locomotion_cast<JumpjetLocomotionClass*>(pThis->Locomotor))
				pJJLoco->State = JumpjetLocomotionClass::State::Ascending;
		}
	}
}

bool UnitExt::CannotMove(UnitClass* pThis)
{
	if (pThis->LocomotorSource)
		return false;

	if (TechnoExt::DriverKilled(pThis))
		return true;

	const auto pType = pThis->Type;

	if (pType->Speed == 0)
		return true;

	const auto movementRestrictedTo = pType->MovementRestrictedTo;

	if (movementRestrictedTo == LandType::None)
		return false;

	auto landType = pThis->GetCell()->LandType;

	if (landType == LandType::Tunnel)
		return false;

	if (pThis->OnBridge && (landType == LandType::Water || landType == LandType::Beach))
		landType = LandType::Road;

	if (movementRestrictedTo != landType)
		return true;

	return false;
}

bool UnitExt::HasAmmoToDeploy(UnitClass* pThis)
{
	const auto pTypeExt = UnitTypeExt::Fetch(pThis->Type);

	const int min = pTypeExt->Ammo_DeployUnlockMinimumAmount;
	const int max = pTypeExt->Ammo_DeployUnlockMaximumAmount;

	if (min < 0 && max < 0)
		return true;

	const int ammo = pThis->Ammo;

	if ((min < 0 || ammo >= min) && (max < 0 || ammo <= max))
		return true;

	return false;
}

void UnitExt::HandleOnDeployAmmoChange(UnitClass* pThis, int maxAmmoOverride)
{
	const auto pTypeExt = UnitTypeExt::Fetch(pThis->Type);

	if (const int add = pTypeExt->Ammo_AddOnDeploy)
	{
		const int maxAmmo = maxAmmoOverride >= 0 ? maxAmmoOverride : pTypeExt->OwnerObject()->Ammo;
		const int originalAmmo = pThis->Ammo;
		pThis->Ammo = std::clamp(originalAmmo + add, 0, maxAmmo);

		if (originalAmmo != pThis->Ammo)
		{
			pThis->StartReloading();
			pThis->Mark(MarkType::Change);
		}
	}
}

bool UnitExt::SimpleDeployerAllowedToDeploy(UnitClass* pThis, bool defaultValue, bool alwaysCheckLandTypes)
{
	auto const pType = pThis->Type;

	if (!pType->IsSimpleDeployer)
		return defaultValue;

	auto const pTypeExt = UnitTypeExt::Fetch(pType);

	if (alwaysCheckLandTypes || pTypeExt->IsSimpleDeployer_ConsiderPathfinding)
	{
		LandTypeFlags disallowedLandTypes;

		if (pTypeExt->IsSimpleDeployer_DisallowedLandTypes.isset())
		{
			disallowedLandTypes = pTypeExt->IsSimpleDeployer_DisallowedLandTypes.Get();
		}
		else
		{
			const bool isHover = pType->Locomotor == LocomotionClass::CLSIDs::Hover;
			const bool isJumpjet = pType->Locomotor == LocomotionClass::CLSIDs::Jumpjet;
			const bool isLander = pType->DeployToLand && (isJumpjet || isHover);
			disallowedLandTypes = isLander ? (LandTypeFlags)(LandTypeFlags::Water | LandTypeFlags::Beach) : LandTypeFlags::None;
		}

		if (IsLandTypeInFlags(disallowedLandTypes, pThis->GetCell()->LandType))
			return false;

		if (alwaysCheckLandTypes && !pTypeExt->IsSimpleDeployer_ConsiderPathfinding)
			return true;
	}
	else
	{
		return defaultValue;
	}

	auto const pTypeConvert = pTypeExt->Convert_Deploy;
	SpeedType speed = SpeedType::None;
	MovementZone mZone = MovementZone::None;

	if (AresFunctions::ConvertTypeTo && pTypeConvert)
	{
		speed = pTypeConvert->SpeedType;
		mZone = pTypeConvert->MovementZone;
	}
	else
	{
		speed = pType->SpeedType;
		mZone = pType->MovementZone;
	}

	if (speed != SpeedType::None && mZone != MovementZone::None)
	{
		auto const pCell = pThis->GetCell();
		return pCell->IsClearToMove(speed, true, true, -1, mZone, -1, pCell->ContainsBridge());
	}

	return true;
}

// Checks if vehicle can deploy into a building at its current location. If unit has no DeploysInto set returns noDeploysIntoDefaultValue (def = false) instead.
bool UnitExt::CanDeployIntoBuilding(UnitClass* pThis, bool noDeploysIntoDefaultValue)
{
	if (!pThis)
		return false;

	auto const pDeployType = pThis->Type->DeploysInto;

	if (!pDeployType)
		return noDeploysIntoDefaultValue;

	auto mapCoords = CellClass::Coord2Cell(pThis->GetCoords());

	if (pDeployType->GetFoundationWidth() > 2 || pDeployType->GetFoundationHeight(false) > 2)
		mapCoords += CellStruct { -1, -1 };

	// The vanilla game used an inappropriate approach here, resulting in potential risk of desync.
	// Now, through additional checks, we can directly exclude the unit who want to deploy.
	UnitExt::Deployer = pThis;
	const bool canDeploy = pDeployType->CanCreateHere(mapCoords, pThis->Owner);
	UnitExt::Deployer = nullptr;

	return canDeploy;
}

UnitTypeClass* UnitExt::GetUnitTypeExtra(UnitClass* pUnit, UnitTypeExt* pData)
{
	if (pUnit->IsGreenHP())
	{
		return nullptr;
	}
	else if (pUnit->IsYellowHP())
	{
		if (pUnit->GetCell()->LandType == LandType::Water && !pUnit->OnBridge)
		{
			if (auto const imageYellow = pData->WaterImage_ConditionYellow)
				return imageYellow;
		}
		else if (auto const imageYellow = pData->Image_ConditionYellow)
		{
			return abstract_cast<UnitTypeClass*, true>(imageYellow);
		}
	}
	else
	{
		if (pUnit->GetCell()->LandType == LandType::Water && !pUnit->OnBridge)
		{
			if (auto const imageRed = pData->WaterImage_ConditionRed)
				return imageRed;
			else if (auto const imageYellow = pData->WaterImage_ConditionYellow)
				return imageYellow;
		}
		else if (auto const imageRed = pData->Image_ConditionRed)
		{
			return abstract_cast<UnitTypeClass*, true>(imageRed);
		}
		else if (auto const imageYellow = pData->Image_ConditionYellow)
		{
			return abstract_cast<UnitTypeClass*, true>(imageYellow);
		}
	}

	return nullptr;
}

void UnitExt::InitializeRecoilData()
{
	const auto pTypeExt = static_cast<UnitTypeExt*>(this->TypeExtData);
	const auto pType = pTypeExt->OwnerObject();

	if (!pType->TurretRecoil)
		return;

	// Always resize to match the current type's count so that type conversions
	// (e.g. 9 turrets -> 2) do not leave stale elements that waste memory and
	// inflate the save file.
	this->ExtraTurretRecoil.resize(pTypeExt->ExtraTurretCount);

	if (pTypeExt->ExtraTurretCount)
	{
		const auto& refData = pType->TurretAnimData;

		for (auto& data : this->ExtraTurretRecoil)
		{
			data.Turret.Travel = refData.Travel;
			data.Turret.CompressFrames = refData.CompressFrames;
			data.Turret.RecoverFrames = refData.RecoverFrames;
			data.Turret.HoldFrames = refData.HoldFrames;
			data.TravelPerFrame = 0.0;
			data.TravelSoFar = 0.0;
			data.State = RecoilData::RecoilState::Inactive;
			data.TravelFramesLeft = 0;
		}
	}

	const auto dataCount = (pTypeExt->ExtraBarrelCount + 1) * (pTypeExt->ExtraTurretCount + 1) - 1;
	this->ExtraBarrelRecoil.resize(dataCount);

	if (dataCount)
	{
		const auto& refData = pType->BarrelAnimData;

		for (auto& data : this->ExtraBarrelRecoil)
		{
			data.Turret.Travel = refData.Travel;
			data.Turret.CompressFrames = refData.CompressFrames;
			data.Turret.RecoverFrames = refData.RecoverFrames;
			data.Turret.HoldFrames = refData.HoldFrames;
			data.TravelPerFrame = 0.0;
			data.TravelSoFar = 0.0;
			data.State = RecoilData::RecoilState::Inactive;
			data.TravelFramesLeft = 0;
		}
	}
}

void UnitExt::RecordRecoilData()
{
	const auto pThis = this->OwnerObject();
	const auto pTypeExt = static_cast<UnitTypeExt*>(this->TypeExtData);

	if (auto turretIndex = pTypeExt->BurstPerTurret
		? ((pThis->CurrentBurstIndex / pTypeExt->BurstPerTurret) % (pTypeExt->ExtraTurretCount + 1))
		: 0)
	{
		turretIndex -= 1;
		this->ExtraTurretRecoil[turretIndex].TravelSoFar = 0.0;
		this->ExtraTurretRecoil[turretIndex].Fire();
	}
	else
	{
		pThis->TurretRecoil.TravelSoFar = 0.0;
		pThis->TurretRecoil.Fire();
	}

	if (auto barrelIndex = (pTypeExt->ExtraTurretCount || pTypeExt->ExtraBarrelCount)
		? (pThis->CurrentBurstIndex % ((pTypeExt->ExtraBarrelCount + 1) * (pTypeExt->ExtraTurretCount + 1)))
		: 0)
	{
		barrelIndex -= 1;
		this->ExtraBarrelRecoil[barrelIndex].TravelSoFar = 0.0;
		this->ExtraBarrelRecoil[barrelIndex].Fire();
	}
	else
	{
		pThis->BarrelRecoil.TravelSoFar = 0.0;
		pThis->BarrelRecoil.Fire();
	}
}

// Skip vanilla recoil update
DEFINE_JUMP(LJMP, 0x6FA4D1, 0x6FA4FB);

void UnitExt::UpdateRecoilData()
{
	if (!this->TypeExtData->OwnerObject()->TurretRecoil)
		return;

	const auto pThis = this->OwnerObject();

	pThis->TurretRecoil.Update();
	pThis->BarrelRecoil.Update();

	for (auto& data : this->ExtraTurretRecoil)
		data.Update();

	for (auto& data : this->ExtraBarrelRecoil)
		data.Update();
}

bool UnitExt::CanCargoLandOnCell(UnitClass* pCargo, CellClass* pCell)
{
	if (!pCargo || !pCell)
		return false;

	auto const pCargoType = pCargo->Type;
	if (!pCargoType)
		return false;

	if (pCell->GetContent() != nullptr)
		return false;

	const bool isWater = (pCell->LandType == LandType::Water);
	const bool onBridge = pCell->ContainsBridge();
	const bool isNavalOrWater = pCargoType->Naval
		|| pCargoType->MovementZone == MovementZone::Water
		|| pCargoType->SpeedType == SpeedType::Float;

	if (isNavalOrWater && !isWater && pCell->LandType != LandType::Beach)
	{
		if (pCargoType->MovementZone == MovementZone::Water || pCargoType->SpeedType == SpeedType::Float)
			return false;
	}

	if (!isNavalOrWater && isWater && !onBridge)
	{
		if (pCargoType->SpeedType != SpeedType::Hover && pCargoType->SpeedType != SpeedType::Amphibious)
			return false;
	}

	if (pCargoType->SpeedType != SpeedType::None && pCargoType->MovementZone != MovementZone::None)
	{
		if (!pCell->IsClearToMove(pCargoType->SpeedType, true, true, -1, pCargoType->MovementZone, -1, onBridge))
			return false;
	}

	return true;
}

bool UnitExt::CarryallCanLift(TechnoClass* pTarget) const
{
	if (!pTarget || !pTarget->IsAlive || pTarget->InLimbo)
		return false;

	auto const pThis = this->OwnerObject();
	if (!pThis || pThis == pTarget)
		return false;

	if (this->CarryallCargo != nullptr)
		return false;

	if (pTarget->WhatAmI() != AbstractType::Unit)
		return false;

	auto const pTargetUnit = static_cast<UnitClass*>(pTarget);

	if (pTargetUnit->IsInAir() || pTargetUnit->GetHeight() > 0)
		return false;

	if (pTargetUnit->Transporter || pTargetUnit->IsOnCarryall)
		return false;

	if (!pThis->Owner->IsAlliedWith(pTargetUnit->Owner))
		return false;

	auto const pTargetTypeExt = TechnoTypeExt::Fetch(pTargetUnit->Type);
	if (!pTargetTypeExt->Carryall_Allowed.Get())
		return false;

	auto const pCarrierTypeExt = this->GetTypeExtData();
	if (pCarrierTypeExt->Carryall_SizeLimit.isset() && static_cast<int>(pTargetUnit->GetTechnoType()->Size) > pCarrierTypeExt->Carryall_SizeLimit.Get())
		return false;

	return true;
}

bool UnitExt::CarryallCanDropAt(CellClass* pCell) const
{
	if (!this->CarryallCargo || !pCell)
		return false;

	if (auto const pThis = this->OwnerObject())
	{
		if (auto const pJJLoco = locomotion_cast<JumpjetLocomotionClass*>(pThis->Locomotor))
		{
			const int cruiseHeight = pJJLoco->Height > 0 ? pJJLoco->Height : RulesClass::Instance->CruiseHeight;
			if (pJJLoco->CurrentHeight < (cruiseHeight * 9 / 10))
				return false;
		}
	}

	return CanCargoLandOnCell(this->CarryallCargo, pCell);
}

void UnitExt::CarryallPickup(UnitClass* pTarget)
{
	auto const pThis = this->OwnerObject();
	if (!pThis || !pTarget)
		return;

	if (pTarget->IsSelected)
		pTarget->Deselect();

	pTarget->Limbo();
	pTarget->IsOnCarryall = true;
	pTarget->Transporter = pThis;
	this->CarryallCargo = pTarget;

	pTarget->Location = pThis->Location;
	pTarget->PrimaryFacing = pThis->PrimaryFacing;
	pTarget->SecondaryFacing = pThis->PrimaryFacing;
	pTarget->AngleRotatedSideways = pThis->AngleRotatedSideways;
	pTarget->AngleRotatedForwards = pThis->AngleRotatedForwards;
	pTarget->PitchAngle = pThis->PitchAngle;

	pTarget->SetDestination(nullptr, true);
	pTarget->QueueMission(Mission::Guard, true);
}

void UnitExt::CarryallDeployDrop()
{
	auto const pThis = this->OwnerObject();
	if (!pThis || !this->CarryallCargo)
		return;

	auto const pCargo = this->CarryallCargo;
	auto const pCell = pThis->GetCell();
	const CoordStruct dropCoords = pCell->GetCoordsWithBridge();
	const DirType facing = pThis->PrimaryFacing.Current().GetDir();

	pCargo->IsOnCarryall = false;
	pCargo->Transporter = nullptr;
	this->CarryallCargo = nullptr;

	pCargo->AngleRotatedSideways = 0.0f;
	pCargo->AngleRotatedForwards = 0.0f;
	pCargo->PitchAngle = 0.0f;

	pCargo->Unlimbo(dropCoords, facing);
	pCargo->SetLocation(dropCoords);
	pCargo->InAir = false;

	if (auto const pLoco = pCargo->Locomotor)
		pLoco->Mark_All_Occupation_Bits(MarkType::Down);

	pCargo->QueueMission(Mission::Guard, true);
}

void UnitExt::DrawCarryallSHPGenericShadow(TechnoClass* pTransporter, TechnoClass* pCargo, RectangleStruct* pBounds)
{
	if (!pCargo || !pCargo->IsAlive || pCargo->InLimbo)
		return;

	auto const pCargoType = pCargo->GetTechnoType();
	if (!pCargoType || pCargoType->Voxel)
		return;

	auto const pCell = pCargo->GetCell();
	const int groundZ = pCell ? pCell->GetCoordsWithBridge().Z : MapClass::Instance.GetCellFloorHeight(pCargo->Location);

	// Only draw generic airborne shadow when held above ground
	if (pCargo->Location.Z <= groundZ + 16)
		return;

	static SHPStruct* pShadowSHP = nullptr;
	static bool shadowLoaded = false;
	if (!shadowLoaded)
	{
		shadowLoaded = true;

		pShadowSHP = *reinterpret_cast<SHPStruct**>(0xA8F244);

		if (!pShadowSHP)
			pShadowSHP = FileSystem::LoadSHPFile("infshdw.shp");
	}

	if (!pShadowSHP)
		return;

	CoordStruct groundCoord = pCargo->Location;
	groundCoord.Z = groundZ;

	Point2D shadowScreenLoc = TacticalClass::CoordsToScreen(groundCoord) - TacticalClass::Instance->TacticalPos;

	const int frameIdx = pShadowSHP->Frames > 1 ? 1 : 0;
	const auto pBoundsToUse = pBounds ? pBounds : &DSurface::ViewBounds;

	CC_Draw_Shape(
		DSurface::Temp,
		FileSystem::PALETTE_PAL,
		pShadowSHP,
		frameIdx,
		&shadowScreenLoc,
		pBoundsToUse,
		static_cast<BlitterFlags>(0x2601),
		0,
		0,
		ZGradient::Ground,
		1000,
		0,
		nullptr,
		0,
		0,
		0
	);
}

void UnitExt::UpdateCarryall()
{
	auto const pThis = this->OwnerObject();
	if (!pThis || !pThis->IsAlive || pThis->InLimbo)
		return;

	auto const pType = pThis->Type;
	auto const pTypeExt = this->GetTypeExtData();

	if (!pTypeExt->Carryall && this->CarryallState == JumpjetCarryallState::Idle && !this->CarryallCargo)
		return;

	auto const pJJLoco = locomotion_cast<JumpjetLocomotionClass*>(pThis->Locomotor);
	if (!pJJLoco)
		return;

	if (pThis->InAir && !pJJLoco->Is_Moving_Now())
		pThis->WalkedFramesSoFar++;

	const int cruiseHeight = pType->JumpjetHeight > 0 ? pType->JumpjetHeight : 500;
	const int climbRate = std::max(2, static_cast<int>(pType->JumpjetClimb > 0.0f ? pType->JumpjetClimb : 5.0f));
	const int descendRate = climbRate;

	switch (this->CarryallState)
	{
	case JumpjetCarryallState::Idle:
	{
		if (this->CarryallCargo)
			this->CarryallState = JumpjetCarryallState::Carrying;

		break;
	}

	case JumpjetCarryallState::MovingToPickup:
	{
		pThis->InAir = true;

		auto const pTargetUnit = abstract_cast<UnitClass*>(this->CarryallTarget);
		if (!pTargetUnit || !pTargetUnit->IsAlive || pTargetUnit->InLimbo || !this->CarryallCanLift(pTargetUnit))
		{
			this->CarryallTarget = nullptr;
			this->CarryallAlignTimer = 0;
			this->CarryallState = JumpjetCarryallState::Idle;
			pThis->SetDestination(nullptr, false);
			pThis->QueueMission(Mission::Guard, true);
			break;
		}

		auto const pTargetCell = pTargetUnit->GetCell();
		if (!pTargetCell)
			break;

		const CoordStruct targetPos = pTargetUnit->GetCoords();
		const int dx = targetPos.X - pThis->Location.X;
		const int dy = targetPos.Y - pThis->Location.Y;
		const int distSq = dx * dx + dy * dy;

		// Check if the carryall has reached directly above the target unit.
		if (pThis->GetCell() == pTargetCell && (distSq <= 16 * 16 || pJJLoco->DestinationReached || (!pJJLoco->IsMoving && distSq <= 48 * 48)))
		{
			// Arrived at unit center: halt horizontal movement and hover in the air.
			pThis->SetDestination(nullptr, false);
			pJJLoco->Stop_Moving();
			pJJLoco->CurrentSpeed = 0.0;
			pJJLoco->IsMoving = false;
			pJJLoco->State = JumpjetLocomotionClass::State::Hovering;

			pThis->Location.X = targetPos.X;
			pThis->Location.Y = targetPos.Y;
			pJJLoco->DestinationCoords = pThis->Location;

			// Maintain current arrival height in the air above the target ground.
			const int targetGroundZ = targetPos.Z;
			pJJLoco->CurrentHeight = std::max(32, pThis->Location.Z - targetGroundZ);
			pThis->Location.Z = targetGroundZ + pJJLoco->CurrentHeight;

			this->CarryallAlignTimer = 0;
			this->CarryallState = JumpjetCarryallState::AligningInAir;
		}
		else
		{
			this->CarryallAlignTimer = 0;

			if (pThis->Destination != pTargetCell)
				pThis->SetDestination(pTargetCell, false);

			pJJLoco->DestinationCoords = targetPos;
		}
		break;
	}

	case JumpjetCarryallState::AligningInAir:
	{
		pThis->InAir = true;

		auto const pTargetUnit = abstract_cast<UnitClass*>(this->CarryallTarget);
		if (!pTargetUnit || !pTargetUnit->IsAlive || pTargetUnit->InLimbo || !this->CarryallCanLift(pTargetUnit))
		{
			this->CarryallTarget = nullptr;
			this->CarryallAlignTimer = 0;
			this->CarryallState = JumpjetCarryallState::Idle;
			pThis->SetDestination(nullptr, false);
			pThis->QueueMission(Mission::Guard, true);
			break;
		}

		if (pThis->GetCurrentMission() == Mission::Attack)
		{
			this->CarryallTarget = nullptr;
			this->CarryallAlignTimer = 0;
			this->CarryallState = JumpjetCarryallState::Idle;
			break;
		}

		// If the target moved to a different cell while aligning, resume chasing it.
		if (pThis->GetCell() != pTargetUnit->GetCell())
		{
			this->CarryallAlignTimer = 0;
			this->CarryallState = JumpjetCarryallState::MovingToPickup;
			break;
		}

		// Stay stationary in the air centered directly above the target unit.
		const CoordStruct targetPos = pTargetUnit->GetCoords();
		pThis->Location.X = targetPos.X;
		pThis->Location.Y = targetPos.Y;
		pJJLoco->DestinationCoords = pThis->Location;
		pJJLoco->CurrentSpeed = 0.0;
		pJJLoco->IsMoving = false;
		pJJLoco->State = JumpjetLocomotionClass::State::Hovering;

		const int targetGroundZ = targetPos.Z;
		pThis->Location.Z = targetGroundZ + pJJLoco->CurrentHeight;

		// Smoothly rotate to match the target unit's facing using the jumpjet unit's rotation rate.
		const DirStruct targetFacing = pTargetUnit->PrimaryFacing.Current();
		const int turnRate = pJJLoco->TurnRate > 0 ? pJJLoco->TurnRate : pThis->Type->ROT;
		const int step = std::max(1, turnRate) * 256;

		unsigned short curRaw = pThis->PrimaryFacing.Current().Raw;
		const short diff = static_cast<short>(targetFacing.Raw - curRaw);

		if (std::abs(diff) <= step)
		{
			pThis->PrimaryFacing.SetCurrent(targetFacing);
			pJJLoco->LocomotionFacing.SetCurrent(targetFacing);
			if (!pThis->Type->Turret)
				pThis->SecondaryFacing.SetCurrent(targetFacing);

			this->CarryallAlignTimer = 0;
			this->CarryallState = JumpjetCarryallState::DescendingToPickup;
		}
		else
		{
			if (diff > 0)
				curRaw += static_cast<unsigned short>(step);
			else
				curRaw -= static_cast<unsigned short>(step);

			const DirStruct newFacing { curRaw };
			pThis->PrimaryFacing.SetCurrent(newFacing);
			pJJLoco->LocomotionFacing.SetCurrent(newFacing);
			if (!pThis->Type->Turret)
				pThis->SecondaryFacing.SetCurrent(newFacing);
		}
		break;
	}

	case JumpjetCarryallState::DescendingToPickup:
	{
		pThis->InAir = true;

		auto const pTargetUnit = abstract_cast<UnitClass*>(this->CarryallTarget);
		if (!pTargetUnit || !pTargetUnit->IsAlive || pTargetUnit->InLimbo || !this->CarryallCanLift(pTargetUnit))
		{
			this->CarryallTarget = nullptr;
			this->CarryallState = JumpjetCarryallState::AscendingAfterDrop;
			break;
		}

		if (pThis->GetCurrentMission() == Mission::Attack)
		{
			this->CarryallTarget = nullptr;
			this->CarryallState = JumpjetCarryallState::AscendingAfterDrop;
			break;
		}

		// If the target moved to a different cell while descending, chase it again.
		if (pThis->GetCell() != pTargetUnit->GetCell())
		{
			this->CarryallState = JumpjetCarryallState::MovingToPickup;
			break;
		}

		// Keep centered and facing the target unit without changing cells.
		const CoordStruct targetPos = pTargetUnit->GetCoords();
		pThis->Location.X = targetPos.X;
		pThis->Location.Y = targetPos.Y;
		pJJLoco->DestinationCoords = pThis->Location;
		pJJLoco->CurrentSpeed = 0.0;
		pJJLoco->IsMoving = false;
		pJJLoco->State = JumpjetLocomotionClass::State::Hovering;

		const DirStruct targetFacing = pTargetUnit->PrimaryFacing.Current();
		const int turnRate = pJJLoco->TurnRate > 0 ? pJJLoco->TurnRate : pThis->Type->ROT;
		const int step = std::max(1, turnRate) * 256;

		unsigned short curRaw = pThis->PrimaryFacing.Current().Raw;
		const short diff = static_cast<short>(targetFacing.Raw - curRaw);

		if (std::abs(diff) <= step)
		{
			pThis->PrimaryFacing.SetCurrent(targetFacing);
			pJJLoco->LocomotionFacing.SetCurrent(targetFacing);
			if (!pThis->Type->Turret)
				pThis->SecondaryFacing.SetCurrent(targetFacing);
		}
		else
		{
			if (diff > 0)
				curRaw += static_cast<unsigned short>(step);
			else
				curRaw -= static_cast<unsigned short>(step);

			const DirStruct newFacing { curRaw };
			pThis->PrimaryFacing.SetCurrent(newFacing);
			pJJLoco->LocomotionFacing.SetCurrent(newFacing);
			if (!pThis->Type->Turret)
				pThis->SecondaryFacing.SetCurrent(newFacing);
		}

		const int targetGroundZ = targetPos.Z;

		if (pJJLoco->CurrentHeight > 32)
		{
			pJJLoco->CurrentHeight = std::max(32, pJJLoco->CurrentHeight - descendRate);
			pThis->Location.Z = targetGroundZ + pJJLoco->CurrentHeight;
			pJJLoco->DestinationCoords = pThis->Location;
		}

		if (pJJLoco->CurrentHeight <= 32)
		{
			this->CarryallPickup(pTargetUnit);
			this->CarryallTarget = nullptr;
			this->CarryallState = JumpjetCarryallState::AscendingWithCargo;
			pJJLoco->State = JumpjetLocomotionClass::State::Ascending;
			FootExt::Fetch(pThis)->JumpjetStraightAscend = true;
		}
		break;
	}

	case JumpjetCarryallState::AscendingWithCargo:
	{
		pThis->InAir = true;

		pJJLoco->State = JumpjetLocomotionClass::State::Ascending;
		FootExt::Fetch(pThis)->JumpjetStraightAscend = true;

		pJJLoco->CurrentSpeed = 0.0;
		pJJLoco->IsMoving = false;

		auto const pCell = pThis->GetCell();
		const int targetGroundZ = pCell ? pCell->GetCoordsWithBridge().Z : pThis->Location.Z;

		if (pJJLoco->CurrentHeight < cruiseHeight)
		{
			pJJLoco->CurrentHeight = std::min(cruiseHeight, pJJLoco->CurrentHeight + climbRate);
			pThis->Location.Z = targetGroundZ + pJJLoco->CurrentHeight;
			pJJLoco->DestinationCoords = pThis->Location;
		}
		else
		{
			pJJLoco->CurrentHeight = cruiseHeight;
			pJJLoco->State = JumpjetLocomotionClass::State::Hovering;
			FootExt::Fetch(pThis)->JumpjetStraightAscend = false;
			this->CarryallState = JumpjetCarryallState::Carrying;
			if (!pThis->Destination && pThis->GetCurrentMission() != Mission::Move)
				pThis->QueueMission(Mission::Guard, true);
		}

		if (this->CarryallCargo && this->CarryallCargo->IsAlive)
		{
			this->CarryallCargo->Location = pThis->Location;
			this->CarryallCargo->PrimaryFacing = pThis->PrimaryFacing;
			this->CarryallCargo->SecondaryFacing = pThis->PrimaryFacing;
			this->CarryallCargo->AngleRotatedSideways = pThis->AngleRotatedSideways;
			this->CarryallCargo->AngleRotatedForwards = pThis->AngleRotatedForwards;
			this->CarryallCargo->PitchAngle = pThis->PitchAngle;
		}
		break;
	}

	case JumpjetCarryallState::Carrying:
	{
		pThis->InAir = true;

		if (!this->CarryallCargo || !this->CarryallCargo->IsAlive)
		{
			this->CarryallCargo = nullptr;
			this->CarryallState = JumpjetCarryallState::Idle;
			this->CarryallDropOnArrival = false;
			break;
		}

		this->CarryallCargo->Location = pThis->Location;
		this->CarryallCargo->PrimaryFacing = pThis->PrimaryFacing;
		this->CarryallCargo->SecondaryFacing = pThis->PrimaryFacing;
		this->CarryallCargo->AngleRotatedSideways = pThis->AngleRotatedSideways;
		this->CarryallCargo->AngleRotatedForwards = pThis->AngleRotatedForwards;
		this->CarryallCargo->PitchAngle = pThis->PitchAngle;
		break;
	}

	case JumpjetCarryallState::DescendingToDrop:
	{
		pThis->InAir = true;

		if (!this->CarryallCargo || !this->CarryallCargo->IsAlive)
		{
			this->CarryallCargo = nullptr;
			this->CarryallState = JumpjetCarryallState::AscendingAfterDrop;
			break;
		}

		if (pThis->GetCurrentMission() == Mission::Attack)
		{
			this->CarryallState = JumpjetCarryallState::AscendingWithCargo;
			break;
		}

		pThis->SetDestination(nullptr, false);
		pJJLoco->Stop_Moving();
		pJJLoco->CurrentSpeed = 0.0;
		pJJLoco->IsMoving = false;
		pJJLoco->State = JumpjetLocomotionClass::State::Hovering;

		auto const pCell = pThis->GetCell();
		const CoordStruct dropCoords = pCell ? pCell->GetCoordsWithBridge() : pThis->Location;
		pThis->Location.X = dropCoords.X;
		pThis->Location.Y = dropCoords.Y;
		pJJLoco->DestinationCoords = pThis->Location;

		this->CarryallCargo->Location = pThis->Location;
		this->CarryallCargo->PrimaryFacing = pThis->PrimaryFacing;
		this->CarryallCargo->SecondaryFacing = pThis->PrimaryFacing;
		this->CarryallCargo->AngleRotatedSideways = pThis->AngleRotatedSideways;
		this->CarryallCargo->AngleRotatedForwards = pThis->AngleRotatedForwards;
		this->CarryallCargo->PitchAngle = pThis->PitchAngle;

		const int targetGroundZ = dropCoords.Z;

		if (pJJLoco->CurrentHeight > 32)
		{
			pJJLoco->CurrentHeight = std::max(32, pJJLoco->CurrentHeight - descendRate);
			pThis->Location.Z = targetGroundZ + pJJLoco->CurrentHeight;
			pJJLoco->DestinationCoords = pThis->Location;
		}

		if (pJJLoco->CurrentHeight <= 32)
		{
			this->CarryallDeployDrop();
			this->CarryallState = JumpjetCarryallState::AscendingAfterDrop;
			pJJLoco->State = JumpjetLocomotionClass::State::Ascending;
			FootExt::Fetch(pThis)->JumpjetStraightAscend = true;
		}
		break;
	}

	case JumpjetCarryallState::AscendingAfterDrop:
	{
		pThis->InAir = true;

		pJJLoco->State = JumpjetLocomotionClass::State::Ascending;
		FootExt::Fetch(pThis)->JumpjetStraightAscend = true;

		pJJLoco->CurrentSpeed = 0.0;
		pJJLoco->IsMoving = false;

		auto const pCell = pThis->GetCell();
		const CoordStruct dropCoords = pCell ? pCell->GetCoordsWithBridge() : pThis->Location;
		pThis->Location.X = dropCoords.X;
		pThis->Location.Y = dropCoords.Y;
		pJJLoco->DestinationCoords = pThis->Location;

		const int targetGroundZ = dropCoords.Z;

		if (pJJLoco->CurrentHeight < cruiseHeight)
		{
			pJJLoco->CurrentHeight = std::min(cruiseHeight, pJJLoco->CurrentHeight + climbRate);
			pThis->Location.Z = targetGroundZ + pJJLoco->CurrentHeight;
			pJJLoco->DestinationCoords = pThis->Location;
		}
		else
		{
			pJJLoco->CurrentHeight = cruiseHeight;
			pJJLoco->State = JumpjetLocomotionClass::State::Hovering;
			FootExt::Fetch(pThis)->JumpjetStraightAscend = false;
			this->CarryallState = JumpjetCarryallState::Idle;
			if (!pThis->Destination && pThis->GetCurrentMission() != Mission::Move)
			{
				pThis->QueueMission(Mission::Guard, true);

				if (!pType->BalloonHover)
					pThis->Scatter(CoordStruct::Empty, true, false);
			}
		}
		break;
	}
	}
}

void UnitExt::LoadFromStream(PhobosStreamReader& Stm)
{
	FootExt::LoadFromStream(Stm);
	this->Serialize(Stm);
}

void UnitExt::SaveToStream(PhobosStreamWriter& Stm)
{
	FootExt::SaveToStream(Stm);
	this->Serialize(Stm);
}

// =============================
// container

UnitExt::ExtContainer::ExtContainer() : Container("UnitClass") { }
UnitExt::ExtContainer::~ExtContainer() = default;

// =============================
// container hooks

DEFINE_HOOK(0x7353D3, UnitClass_CTOR, 0x7)
{
	GET(UnitClass*, pItem, ESI);

	UnitExt::ExtMap.Allocate(pItem);

	return 0;
}

// Late in every destructor body of the class, right before it chains into the
// base destructor: the last point where the extension is no longer used.
DEFINE_HOOK_AGAIN(0x7359DA, UnitClass_DTOR, 0x9)
DEFINE_HOOK(0x735967, UnitClass_DTOR, 0x9)
{
	GET(UnitClass*, pItem, ESI);

	if (auto const pExt = UnitExt::Fetch(pItem))
	{
		if (pExt->CarryallCargo && pExt->CarryallCargo->IsAlive)
		{
			auto const pCargo = pExt->CarryallCargo;
			pCargo->IsOnCarryall = false;
			pExt->CarryallCargo = nullptr;
			int damage = pCargo->Health;
			pCargo->ReceiveDamage(&damage, 0, RulesClass::Instance->C4Warhead, nullptr, true, false, nullptr);
		}
	}

	UnitExt::ExtMap.Remove(pItem);

	return 0;
}

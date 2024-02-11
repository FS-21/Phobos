#include "Body.h"

#include <AircraftClass.h>
#include <HouseClass.h>
#include <ScenarioClass.h>

#include <Ext/House/Body.h>

#include <Utilities/AresFunctions.h>

TechnoExt::ExtContainer TechnoExt::ExtMap;

TechnoExt::ExtData::~ExtData()
{
	auto const pTypeExt = this->TypeExtData;
	auto const pType = pTypeExt->OwnerObject();
	auto pThis = this->OwnerObject();

	if (pTypeExt->AutoDeath_Behavior.isset())
	{
		auto const pOwnerExt = HouseExt::ExtMap.Find(pThis->Owner);
		auto& vec = pOwnerExt->OwnedAutoDeathObjects;
		vec.erase(std::remove(vec.begin(), vec.end(), this), vec.end());
	}

	if (pThis->WhatAmI() != AbstractType::Aircraft && pThis->WhatAmI() != AbstractType::Building
		&& pType->Ammo > 0 && pTypeExt->ReloadInTransport)
	{
		auto const pOwnerExt = HouseExt::ExtMap.Find(pThis->Owner);
		auto& vec = pOwnerExt->OwnedTransportReloaders;
		vec.erase(std::remove(vec.begin(), vec.end(), this), vec.end());
	}
}

bool TechnoExt::IsActive(TechnoClass* pThis)
{
	return
		pThis &&
		!pThis->TemporalTargetingMe &&
		!pThis->BeingWarpedOut &&
		!pThis->IsUnderEMP() &&
		pThis->IsAlive &&
		pThis->Health > 0 &&
		!pThis->InLimbo;
}

bool TechnoExt::IsHarvesting(TechnoClass* pThis)
{
	if (!TechnoExt::IsActive(pThis))
		return false;

	auto slave = pThis->SlaveManager;
	if (slave && slave->State != SlaveManagerStatus::Ready)
		return true;

	if (pThis->WhatAmI() == AbstractType::Building)
		return pThis->IsPowerOnline();

	if (TechnoExt::HasAvailableDock(pThis))
	{
		switch (pThis->GetCurrentMission())
		{
		case Mission::Harvest:
		case Mission::Unload:
		case Mission::Enter:
			return true;
		case Mission::Guard: // issue#603: not exactly correct, but idk how to do better
			if (auto pUnit = abstract_cast<UnitClass*>(pThis))
				return pUnit->IsHarvesting || pUnit->Locomotor->Is_Really_Moving_Now() || pUnit->HasAnyLink();
		default:
			return false;
		}
	}

	return false;
}

bool TechnoExt::HasAvailableDock(TechnoClass* pThis)
{
	for (auto pBld : pThis->GetTechnoType()->Dock)
	{
		if (pThis->Owner->CountOwnedAndPresent(pBld))
			return true;
	}

	return false;
}

void TechnoExt::SyncIronCurtainStatus(TechnoClass* pFrom, TechnoClass* pTo)
{
	if (pFrom->IsIronCurtained() && !pFrom->ForceShielded)
	{
		const auto pTypeExt = TechnoTypeExt::ExtMap.Find(pFrom->GetTechnoType());
		if (pTypeExt->IronCurtain_KeptOnDeploy.Get(RulesExt::Global()->IronCurtain_KeptOnDeploy))
		{
			pTo->IronCurtain(pFrom->IronCurtainTimer.GetTimeLeft(), pFrom->Owner, false);
			pTo->IronTintStage = pFrom->IronTintStage;
		}
	}
}

double TechnoExt::GetCurrentSpeedMultiplier(FootClass* pThis)
{
	double houseMultiplier = 1.0;

	if (pThis->WhatAmI() == AbstractType::Aircraft)
		houseMultiplier = pThis->Owner->Type->SpeedAircraftMult;
	else if (pThis->WhatAmI() == AbstractType::Infantry)
		houseMultiplier = pThis->Owner->Type->SpeedInfantryMult;
	else
		houseMultiplier = pThis->Owner->Type->SpeedUnitsMult;

	return pThis->SpeedMultiplier * houseMultiplier *
		(pThis->HasAbility(Ability::Faster) ? RulesClass::Instance->VeteranSpeed : 1.0);
}

CoordStruct TechnoExt::PassengerKickOutLocation(TechnoClass* pThis, FootClass* pPassenger, int maxAttempts = 1)
{
	if (!pThis || !pPassenger)
		return CoordStruct::Empty;

	if (maxAttempts < 1)
		maxAttempts = 1;

	CellClass* pCell;
	CellStruct placeCoords = CellStruct::Empty;
	auto pTypePassenger = pPassenger->GetTechnoType();
	CoordStruct finalLocation = CoordStruct::Empty;
	short extraDistanceX = 1;
	short extraDistanceY = 1;
	SpeedType speedType = pTypePassenger->SpeedType;
	MovementZone movementZone = pTypePassenger->MovementZone;

	if (pTypePassenger->WhatAmI() == AbstractType::AircraftType)
	{
		speedType = SpeedType::Track;
		movementZone = MovementZone::Normal;
	}
	do
	{
		placeCoords = pThis->GetCell()->MapCoords - CellStruct { (short)(extraDistanceX / 2), (short)(extraDistanceY / 2) };
		placeCoords = MapClass::Instance->NearByLocation(placeCoords, speedType, -1, movementZone, false, extraDistanceX, extraDistanceY, true, false, false, false, CellStruct::Empty, false, false);

		pCell = MapClass::Instance->GetCellAt(placeCoords);
		extraDistanceX += 1;
		extraDistanceY += 1;
	}
	while (extraDistanceX < maxAttempts && (pThis->IsCellOccupied(pCell, FacingType::None, -1, nullptr, false) != Move::OK) && pCell->MapCoords != CellStruct::Empty);

	pCell = MapClass::Instance->TryGetCellAt(placeCoords);
	if (pCell)
		finalLocation = pCell->GetCoordsWithBridge();

	return finalLocation;
}

bool TechnoExt::AllowedTargetByZone(TechnoClass* pThis, TechnoClass* pTarget, TargetZoneScanType zoneScanType, WeaponTypeClass* pWeapon, bool useZone, int zone)
{
	if (!pThis || !pTarget)
		return false;

	if (pThis->WhatAmI() == AbstractType::Aircraft)
		return true;

	MovementZone mZone = pThis->GetTechnoType()->MovementZone;
	int currentZone = useZone ? zone : MapClass::Instance->GetMovementZoneType(pThis->GetMapCoords(), mZone, pThis->OnBridge);

	if (currentZone != -1)
	{
		if (zoneScanType == TargetZoneScanType::Any)
			return true;

		int targetZone = MapClass::Instance->GetMovementZoneType(pTarget->GetMapCoords(), mZone, pTarget->OnBridge);

		if (zoneScanType == TargetZoneScanType::Same)
		{
			if (currentZone != targetZone)
				return false;
		}
		else
		{
			if (currentZone == targetZone)
				return true;

			auto const speedType = pThis->GetTechnoType()->SpeedType;
			auto cellStruct = MapClass::Instance->NearByLocation(CellClass::Coord2Cell(pTarget->Location),
				speedType, -1, mZone, false, 1, 1, true,
				false, false, speedType != SpeedType::Float, CellStruct::Empty, false, false);
			auto const pCell = MapClass::Instance->GetCellAt(cellStruct);

			if (!pCell)
				return false;

			double distance = pCell->GetCoordsWithBridge().DistanceFrom(pTarget->GetCenterCoords());

			if (!pWeapon)
			{
				int weaponIndex = pThis->SelectWeapon(pTarget);

				if (weaponIndex < 0)
					return false;

				pWeapon = pThis->GetWeapon(weaponIndex)->WeaponType;
			}

			if (distance > pWeapon->Range)
				return false;
		}
	}

	return true;
}

// Feature for common usage : TechnoType conversion -- Trsdy
// BTW, who said it was merely a Type pointer replacement and he could make a better one than Ares?
bool TechnoExt::ConvertToType(FootClass* pThis, TechnoTypeClass* pToType)
{
	if (IS_ARES_FUN_AVAILABLE(ConvertTypeTo))
		return AresFunctions::ConvertTypeTo(pThis, pToType);
	// In case not using Ares 3.0. Only update necessary vanilla properties
	AbstractType rtti;
	TechnoTypeClass** nowTypePtr;

	// Different types prohibited
	switch (pThis->WhatAmI())
	{
	case AbstractType::Infantry:
		nowTypePtr = reinterpret_cast<TechnoTypeClass**>(&(static_cast<InfantryClass*>(pThis)->Type));
		rtti = AbstractType::InfantryType;
		break;
	case AbstractType::Unit:
		nowTypePtr = reinterpret_cast<TechnoTypeClass**>(&(static_cast<UnitClass*>(pThis)->Type));
		rtti = AbstractType::UnitType;
		break;
	case AbstractType::Aircraft:
		nowTypePtr = reinterpret_cast<TechnoTypeClass**>(&(static_cast<AircraftClass*>(pThis)->Type));
		rtti = AbstractType::AircraftType;
		break;
	default:
		Debug::Log("%s is not FootClass, conversion not allowed\n", pToType->get_ID());
		return false;
	}

	if (pToType->WhatAmI() != rtti)
	{
		Debug::Log("Incompatible types between %s and %s\n", pThis->get_ID(), pToType->get_ID());
		return false;
	}

	// Detach CLEG targeting
	auto tempUsing = pThis->TemporalImUsing;
	if (tempUsing && tempUsing->Target)
		tempUsing->Detach();

	HouseClass* const pOwner = pThis->Owner;

	// Remove tracking of old techno
	if (!pThis->InLimbo)
		pOwner->RegisterLoss(pThis, false);
	pOwner->RemoveTracking(pThis);

	int oldHealth = pThis->Health;

	// Generic type-conversion
	TechnoTypeClass* prevType = *nowTypePtr;
	*nowTypePtr = pToType;

	// Readjust health according to percentage
	pThis->SetHealthPercentage((double)(oldHealth) / (double)prevType->Strength);
	pThis->EstimatedHealth = pThis->Health;

	// Add tracking of new techno
	pOwner->AddTracking(pThis);
	if (!pThis->InLimbo)
		pOwner->RegisterGain(pThis, false);
	pOwner->RecheckTechTree = true;

	// Update Ares AttachEffects -- skipped
	// Ares RecalculateStats -- skipped

	// Adjust ammo
	pThis->Ammo = Math::min(pThis->Ammo, pToType->Ammo);
	// Ares ResetSpotlights -- skipped

	// Adjust ROT
	if (rtti == AbstractType::AircraftType)
		pThis->SecondaryFacing.SetROT(pToType->ROT);
	else
		pThis->PrimaryFacing.SetROT(pToType->ROT);
	// Adjust Ares TurretROT -- skipped
	//  pThis->SecondaryFacing.SetROT(TechnoTypeExt::ExtMap.Find(pToType)->TurretROT.Get(pToType->ROT));

	// Locomotor change, referenced from Ares 0.A's abduction code, not sure if correct, untested
	CLSID nowLocoID;
	ILocomotion* iloco = pThis->Locomotor;
	const auto& toLoco = pToType->Locomotor;
	if ((SUCCEEDED(static_cast<LocomotionClass*>(iloco)->GetClassID(&nowLocoID)) && nowLocoID != toLoco))
	{
		// because we are throwing away the locomotor in a split second, piggybacking
		// has to be stopped. otherwise the object might remain in a weird state.
		while (LocomotionClass::End_Piggyback(pThis->Locomotor));
		// throw away the current locomotor and instantiate
		// a new one of the default type for this unit.
		if (auto newLoco = LocomotionClass::CreateInstance(toLoco))
		{
			newLoco->Link_To_Object(pThis);
			pThis->Locomotor = std::move(newLoco);
		}
	}

	// TODO : Jumpjet locomotor special treatement, some brainfart, must be uncorrect, HELP ME!
	const auto& jjLoco = LocomotionClass::CLSIDs::Jumpjet();
	if (pToType->BalloonHover && pToType->DeployToLand && prevType->Locomotor != jjLoco && toLoco == jjLoco)
		pThis->Locomotor->Move_To(pThis->Location);

	return true;
}

// Checks if vehicle can deploy into a building at its current location. If unit has no DeploysInto set returns noDeploysIntoDefaultValue (def = false) instead.
// If a building is specified then it will be used by default.
bool TechnoExt::CanDeployIntoBuilding(UnitClass* pThis, bool noDeploysIntoDefaultValue, BuildingTypeClass* pBuildingType)
{
	if (!pThis)
		return false;

	auto const pDeployType = !pBuildingType ? pThis->Type->DeploysInto : pBuildingType;

	if (!pDeployType)
		return noDeploysIntoDefaultValue;

	bool canDeploy = true;
	auto mapCoords = CellClass::Coord2Cell(pThis->GetCoords());

	if (pDeployType->GetFoundationWidth() > 2 || pDeployType->GetFoundationHeight(false) > 2)
		mapCoords += CellStruct { -1, -1 };

	pThis->Mark(MarkType::Up);

	pThis->Locomotor->Mark_All_Occupation_Bits(MarkType::Up);

	if (!pDeployType->CanCreateHere(mapCoords, pThis->Owner))
		canDeploy = false;

	pThis->Locomotor->Mark_All_Occupation_Bits(MarkType::Down);
	pThis->Mark(MarkType::Down);

	return canDeploy;
}

bool TechnoExt::IsTypeImmune(TechnoClass* pThis, TechnoClass* pSource)
{
	if (!pThis || !pSource)
		return false;

	auto const pType = pThis->GetTechnoType();

	if (!pType->TypeImmune)
		return false;

	if (pType == pSource->GetTechnoType() && pThis->Owner == pSource->Owner)
		return true;

	return false;
}

void TechnoExt::StartUniversalDeployAnim(TechnoClass* pThis)
{
	if (!pThis)
		return;

	auto pExt = TechnoExt::ExtMap.Find(pThis);
	if (!pExt)
		return;

	if (pExt->DeployAnim)
	{
		pExt->DeployAnim->UnInit();
		pExt->DeployAnim = nullptr;
	}

	auto pTypeExt = TechnoTypeExt::ExtMap.Find(pThis->GetTechnoType());
	if (!pTypeExt)
		return;

	if (pTypeExt->Convert_DeployingAnim.isset())
	{
		if (auto deployAnimType = pTypeExt->Convert_DeployingAnim.Get())
		{
			if (auto pAnim = GameCreate<AnimClass>(deployAnimType, pThis->Location))
			{
				pExt->DeployAnim = pAnim;
				pExt->DeployAnim->SetOwnerObject(pThis);

				pExt->Convert_UniversalDeploy_MakeInvisible = true;

				// Use the unit palette in the animation
				auto lcc = pThis->GetDrawer();
				pExt->DeployAnim->LightConvert = lcc;
			}
			else
			{
				Debug::Log("ERROR! [%s] Deploy animation %s can't be created.\n", pThis->GetTechnoType()->ID, deployAnimType->ID);
			}
		}
	}
}

void TechnoExt::UpdateUniversalDeploy(TechnoClass* pThis)
{
	if (!pThis)
		return;

	if (auto pExt = TechnoExt::ExtMap.Find(pThis))
	{
		if (!pExt->Convert_UniversalDeploy_InProgress)
			return;

		if (pThis->WhatAmI() == AbstractType::Building)
			return;

		auto pFoot = static_cast<FootClass*>(pThis);
		if (!pFoot)
			return;

		TechnoClass* deployed = nullptr;

		auto pTypeExt = TechnoTypeExt::ExtMap.Find(pThis->GetTechnoType());
		if (!pTypeExt)
			return;

		bool deployToLand = pTypeExt->Convert_DeployToLand;

		if (pExt->Convert_UniversalDeploy_InProgress && deployToLand)
			pThis->IsFallingDown = true;

		if (!pThis->IsFallingDown && pFoot->Locomotor->Is_Really_Moving_Now())
			return;

		if (pThis->WhatAmI() != AbstractType::Infantry)
		{
			// Turn the unit to the right deploy facing
			if (!pExt->DeployAnim && pTypeExt->Convert_DeployDir >= 0)
			{
				DirType currentDir = pThis->PrimaryFacing.Current().GetDir(); // Returns current position in format [0 - 7] x 32
				DirType desiredDir = (static_cast<DirType>(pTypeExt->Convert_DeployDir * 32));
				DirStruct desiredFacing;
				desiredFacing.SetDir(desiredDir);

				if (currentDir != desiredDir)
				{
					pFoot->Locomotor->Move_To(CoordStruct::Empty);
					pThis->PrimaryFacing.SetDesired(desiredFacing);

					return;
				}
			}
		}

		AnimTypeClass* pDeployAnimType = pTypeExt->Convert_DeployingAnim.isset() ? pTypeExt->Convert_DeployingAnim.Get() : nullptr;
		bool hasValidDeployAnim = pDeployAnimType ? true : false;
		bool isDeployAnimPlaying = pExt->DeployAnim ? true : false;
		bool hasDeployAnimFinished = (isDeployAnimPlaying && (pExt->DeployAnim->Animation.Value >= (pDeployAnimType->End + pDeployAnimType->Start - 1))) ? true : false;
		int convert_DeploySoundIndex = pTypeExt->Convert_DeploySound.isset() ? pTypeExt->Convert_DeploySound.Get() : -1;
		AnimTypeClass* pAnimFXType = pTypeExt->Convert_AnimFX.isset() ? pTypeExt->Convert_AnimFX.Get() : nullptr;
		bool animFX_FollowDeployer = pTypeExt->Convert_AnimFX_FollowDeployer;

		// Update InAir information
		if (deployToLand && pThis->GetHeight() <= 20)
			pThis->SetHeight(0);

		pThis->InAir = (pThis->GetHeight() > 0);

		if (deployToLand)
		{
			if (pThis->InAir)
				return;

			pFoot->ParalysisTimer.Start(15);
		}

		CoordStruct deployLocation = pThis->GetCoords();

		// Before the conversion we need to play the full deploy animation
		if (hasValidDeployAnim && !isDeployAnimPlaying)
		{
			TechnoExt::StartUniversalDeployAnim(pThis);
			return;
		}

		// Hack for making the Voxel invisible during a deploy
		if (hasValidDeployAnim)
			pExt->Convert_UniversalDeploy_MakeInvisible = true;

		// The unit should remain paralyzed during a deploy animation
		if (isDeployAnimPlaying)
			pFoot->ParalysisTimer.Start(15);

		// Necessary for skipping the passengers ejection
		pFoot->CurrentMission = Mission::None;

		if (hasValidDeployAnim && !hasDeployAnimFinished)
			return;

		/*if (pThis->DeployAnim)
		{
			pThis->DeployAnim->Limbo();
			pThis->DeployAnim->UnInit();
			pThis->DeployAnim = nullptr;
		}*/

		// Time for the object conversion
		deployed = TechnoExt::UniversalConvert(pThis, nullptr);

		if (deployed)
		{
			if (convert_DeploySoundIndex >= 0)
				VocClass::PlayAt(convert_DeploySoundIndex, deployLocation);

			if (pAnimFXType)
			{
				if (auto const pAnim = GameCreate<AnimClass>(pAnimFXType, deployLocation))
				{
					if (animFX_FollowDeployer)
						pAnim->SetOwnerObject(deployed);

					pAnim->Owner = deployed->Owner;
				}
			}
		}
		else
		{
			pFoot->ParalysisTimer.Stop();
			pThis->IsFallingDown = false;
			pThis->ForceMission(Mission::Guard);
			pExt->Convert_UniversalDeploy_InProgress = false;
			pExt->Convert_UniversalDeploy_MakeInvisible = false;
		}
	}
}

// This method transfer all settings from the old object to the new.
// If a new object isn't defined then it will be picked from a list.
TechnoClass* TechnoExt::UniversalConvert(TechnoClass* pThis, TechnoTypeClass* pNewTechnoType)
{
	if (!pThis)
		return nullptr;

	auto pOldTechnoType = pThis->GetTechnoType();
	if (!pOldTechnoType)
		return nullptr;

	auto pOldTechnoTypeExt = TechnoTypeExt::ExtMap.Find(pOldTechnoType);
	if (!pOldTechnoTypeExt)
		return nullptr;

	auto pOldTechno = pThis;
	auto newLocation = pOldTechno->Location;

	// If the new object isn't defined then it will be picked from a list
	if (!pNewTechnoType)
	{
		if (pOldTechnoTypeExt->Convert_UniversalDeploy.size() > 0)
		{
			// TO-DO: having multiple deploy candidate IDs it should pick randomly from the list
			// Probably a new tag should enable this random behaviour...
			int index = 0; //ScenarioClass::Instance->Random.RandomRanged(0, pOldTechnoTypeExt->Convert_UniversalDeploy.size() - 1);
			pNewTechnoType = pOldTechnoTypeExt->Convert_UniversalDeploy.at(index);
		}
		else
		{
			return nullptr;
		}
	}

	auto pOldTechnoExt = TechnoExt::ExtMap.Find(pOldTechno);

	// The "Infantry -> Vehicle" case we need to check if the cell is free of soldiers
	if (pOldTechnoType->WhatAmI() != AbstractType::Building
		&& pNewTechnoType->WhatAmI() != AbstractType::Building)
	{
		int nObjectsInCell = 0;

		for (auto pObject = pOldTechno->GetCell()->FirstObject; pObject; pObject = pObject->NextObject)
		{
			auto const pItem = static_cast<TechnoClass*>(pObject);

			if (pItem && pItem != pOldTechno)
				nObjectsInCell++;
		}

		if (nObjectsInCell > 0)
		{
			pOldTechnoExt->Convert_UniversalDeploy_InProgress = false;
			pThis->IsFallingDown = false;
			CoordStruct loc = CoordStruct::Empty;
			pOldTechno->Scatter(loc, true, false);

			return nullptr;
		}
	}

	auto pOwner = pOldTechno->Owner;
	auto pNewTechno = static_cast<TechnoClass*>(pNewTechnoType->CreateObject(pOwner));

	if (!pNewTechno)
	{
		pOldTechnoExt->Convert_UniversalDeploy_InProgress = false;
		pThis->IsFallingDown = false;
		//CoordStruct loc = CoordStruct::Empty;
		//pOldTechno->Scatter(loc, true, false);

		return nullptr;
	}

	auto pNewTechnoExt = TechnoExt::ExtMap.Find(pNewTechno);

	// Transfer enemies target (part 1/2)
	DynamicVectorClass<TechnoClass*> enemiesTargetingMeList;

	for (auto pEnemy : *TechnoClass::Array)
	{
		if (pEnemy->Target == pOldTechno)
			enemiesTargetingMeList.AddItem(pEnemy);
	}

	// Transfer some stats from the old object to the new:
	// Health update
	double nHealthPercent = (double)(1.0 * pOldTechno->Health / pOldTechnoType->Strength);
	pNewTechno->Health = (int)round(pNewTechnoType->Strength * nHealthPercent);
	pNewTechno->EstimatedHealth = pNewTechno->Health;

	// Shield update
	if (pOldTechnoExt && pNewTechnoExt)
	{
		if (auto pOldShieldData = pOldTechnoExt->Shield.get())
		{
			if (auto pNewShieldData = pNewTechnoExt->Shield.get())
			{
				if (pOldTechnoExt->CurrentShieldType
					&& pOldShieldData
					&& pNewTechnoExt->CurrentShieldType
					&& pNewShieldData)
				{
					double nOldShieldHealthPercent = (double)(1.0 * pOldShieldData->GetHP() / pOldTechnoExt->CurrentShieldType->Strength);

					pNewShieldData->SetHP((int)(nOldShieldHealthPercent * pNewTechnoExt->CurrentShieldType->Strength));
				}
			}
		}
	}

	// Veterancy update
	if (pOldTechnoTypeExt->Convert_TransferVeterancy)
	{
		VeterancyStruct nVeterancy = pOldTechno->Veterancy;
		pNewTechno->Veterancy = nVeterancy;
	}

	// AI team update
	if (pOldTechno->BelongsToATeam())
	{
		auto pOldFoot = static_cast<FootClass*>(pOldTechno);
		auto pNewFoot = static_cast<FootClass*>(pNewTechno);

		if (pNewFoot)
			pNewFoot->Team = pOldFoot->Team;
	}

	// Ammo update
	auto nAmmo = pNewTechno->Ammo;

	if (nAmmo >= pOldTechno->Ammo)
		nAmmo = pOldTechno->Ammo;

	pNewTechno->Ammo = nAmmo;

	// If the object was selected it should remain selected
	bool isSelected = false;
	if (pOldTechno->IsSelected)
		isSelected = true;

	// Mind control update
	if (pOldTechno->IsMindControlled())
		TechnoExt::TransferMindControlOnDeploy(pOldTechno, pNewTechno);

	// Initial mission update
	pNewTechno->QueueMission(Mission::Guard, true);

	// Cloak update
	pNewTechno->CloakState = pOldTechno->CloakState;

	// Facing update
	DirStruct oldPrimaryFacing;
	DirStruct oldSecondaryFacing;

	if (pOldTechnoTypeExt->Convert_DeployDir >= 0)
	{
		DirType desiredDir = (static_cast<DirType>(pOldTechnoTypeExt->Convert_DeployDir * 32));
		oldPrimaryFacing.SetDir(desiredDir);
		oldSecondaryFacing.SetDir(desiredDir);
	}
	else
	{
		oldPrimaryFacing = pOldTechno->PrimaryFacing.Current();
		oldSecondaryFacing = pOldTechno->SecondaryFacing.Current();
	}

	DirType oldPrimaryFacingDir = oldPrimaryFacing.GetDir(); // Returns current position in format [0 - 7] x 32
	//DirType oldSecondaryFacingDir = oldSecondaryFacing.GetDir(); // Returns current position in format [0 - 7] x 32

	// Some vodoo magic
	pOldTechno->Limbo();

	++Unsorted::IKnowWhatImDoing;
	bool limboed = pNewTechno->Unlimbo(newLocation, oldPrimaryFacingDir);
	--Unsorted::IKnowWhatImDoing;

	if (!limboed)
	{
		// Abort operation, restoring old object
		pOldTechno->Unlimbo(newLocation, oldPrimaryFacingDir);
		pNewTechno->UnInit();

		if (isSelected)
			pOldTechno->Select();

		//if (auto pExt = TechnoExt::ExtMap.Find(pOldTechno))
			//pOldTechno->IsFallingDown = false;

		pOldTechnoExt->Convert_UniversalDeploy_InProgress = false;
		pOldTechno->IsFallingDown = false;

		return nullptr;
	}

	// Recover turret direction
	if (pOldTechnoType->Turret && pNewTechnoType->Turret && !pNewTechnoType->TurretSpins)
		pNewTechno->SecondaryFacing.SetCurrent(oldSecondaryFacing);
	
	// Inherit other stuff
	pNewTechno->WasFallingDown = pOldTechno->WasFallingDown;
	pNewTechno->WarpingOut = pOldTechno->WarpingOut;
	pNewTechno->unknown_280 = pOldTechno->unknown_280; // sth related to teleport
	pNewTechno->BeingWarpedOut = pOldTechno->BeingWarpedOut;
	pNewTechno->Deactivated = pOldTechno->Deactivated;
	pNewTechno->Flash(pOldTechno->Flashing.DurationRemaining);
	pNewTechno->IdleActionTimer = pOldTechno->IdleActionTimer;
	pNewTechno->ChronoLockRemaining = pOldTechno->ChronoLockRemaining;
	pNewTechno->Berzerk = pOldTechno->Berzerk;
	pNewTechno->BerzerkDurationLeft = pOldTechno->BerzerkDurationLeft;
	pNewTechno->ChronoWarpedByHouse = pOldTechno->ChronoWarpedByHouse;
	pNewTechno->EMPLockRemaining = pOldTechno->EMPLockRemaining;
	pNewTechno->ShouldLoseTargetNow = pOldTechno->ShouldLoseTargetNow;
	pNewTechno->SetOwningHouse(pOldTechno->GetOwningHouse(), false);// Is really necessary? the object was created with the final owner by default...

	// Jumpjet tricks
	if (pNewTechnoType->JumpJet || pNewTechnoType->BalloonHover)
	{
		auto pFoot = static_cast<FootClass*>(pNewTechno);
		pFoot->Scatter(CoordStruct::Empty, true, false);
		//pFoot->Locomotor->Move_To(CoordStruct::Empty);
		//pFoot->Locomotor->Do_Turn(oldPrimaryFacing);
		//pNewTechno->PrimaryFacing.SetDesired(oldPrimaryFacing);
	}
	else
	{
		auto pNewTechnoTypeExt = TechnoTypeExt::ExtMap.Find(pNewTechnoType);

		if (!pNewTechnoTypeExt->Convert_DeployToLand)
			pNewTechno->IsFallingDown = false;
		else
			pNewTechno->IsFallingDown = true;
	}

	// Transfer passengers/garrisoned units, if possible
	TechnoExt::PassengersTransfer(pOldTechno, pNewTechno);

	// Transfer Iron Courtain effect, if applied
	TechnoExt::SyncIronCurtainStatus(pOldTechno, pNewTechno);

	// Transfer enemies target (part 2/2)
	for (auto pEnemy : enemiesTargetingMeList)
	{
		pEnemy->Target = pNewTechno;
	}

	if (pOldTechno->InLimbo)
		pOwner->RegisterLoss(pOldTechno, false);

	if (!pNewTechno->InLimbo)
		pOwner->RegisterGain(pNewTechno, true);

	pNewTechno->Owner->RecheckTechTree = true;

	// If the object was selected it should remain selected
	if (isSelected)
		pNewTechno->Select();

	return pNewTechno;
}

// Currently is a vehicle to vehicle. TO-DO: Building2Building and vehicle->Building & vice-versa
void TechnoExt::PassengersTransfer(TechnoClass* pTechnoFrom, TechnoClass* pTechnoTo = nullptr)
{
	if (!pTechnoFrom || (pTechnoFrom == pTechnoTo))
		return;

	// Without a target transport this method becomes an "eject passengers from transport"
	auto pTechnoTypeTo = pTechnoTo ? pTechnoTo->GetTechnoType() : nullptr;
	if (!pTechnoTypeTo && pTechnoTo)
		return;

	bool bTechnoToIsBuilding = (pTechnoTo && pTechnoTo->WhatAmI() == AbstractType::Building) ? true : false;
	DynamicVectorClass<FootClass*> passengersList;

	// From bunkered infantry
	if (auto pBuildingFrom = static_cast<BuildingClass*>(pTechnoFrom))
	{
		for (int i = 0; i < pBuildingFrom->Occupants.Count; i++)
		{
			InfantryClass* pPassenger = pBuildingFrom->Occupants.GetItem(i);
			auto pFooter = static_cast<FootClass*>(pPassenger);
			passengersList.AddItem(pFooter);
		}
	}

	// We'll get the passengers list in a more easy data structure
	while (pTechnoFrom->Passengers.NumPassengers > 0)
	{
		FootClass* pPassenger = pTechnoFrom->Passengers.RemoveFirstPassenger();

		pPassenger->Transporter = nullptr;
		pPassenger->ShouldEnterOccupiable = false;
		pPassenger->ShouldGarrisonStructure = false;
		pPassenger->InOpenToppedTransport = false;
		pPassenger->QueueMission(Mission::Guard, false);
		passengersList.AddItem(pPassenger);
	}

	if (passengersList.Count > 0)
	{
		double passengersSizeLimit = pTechnoTo ? pTechnoTypeTo->SizeLimit : 0;
		int passengersLimit = pTechnoTo ? pTechnoTypeTo->Passengers : 0;
		int numPassengers = pTechnoTo ? pTechnoTo->Passengers.NumPassengers : 0;
		TechnoClass* transportReference = pTechnoTo ? pTechnoTo : pTechnoFrom;

		while (passengersList.Count > 0)
		{
			bool forceEject = false;
			FootClass* pPassenger = nullptr;

			// The insertion order is different in buildings
			int passengerIndex = bTechnoToIsBuilding ? 0 : passengersList.Count - 1;

			pPassenger = passengersList.GetItem(passengerIndex);
			passengersList.RemoveItem(passengerIndex);
			auto pFromTypeExt = TechnoTypeExt::ExtMap.Find(pTechnoFrom->GetTechnoType());

			if (!pTechnoTo || (pFromTypeExt && !pFromTypeExt->Convert_TransferPassengers))
				forceEject = true;

			// To bunkered infantry
			if (pTechnoTo->WhatAmI() == AbstractType::Building)
			{
				auto pBuildingTo = static_cast<BuildingClass*>(pTechnoTo);
				int nOccupants = pBuildingTo->Occupants.Count;
				int maxNumberOccupants = pTechnoTo ? pBuildingTo->Type->MaxNumberOccupants : 0;

				InfantryClass* infantry = static_cast<InfantryClass*>(pPassenger);
				bool isOccupier = infantry && (infantry->Type->Occupier || pFromTypeExt->Convert_TransferPassengers_IgnoreInvalidOccupiers) ? true : false;

				if (!forceEject && isOccupier && maxNumberOccupants > 0 && nOccupants < maxNumberOccupants)
				{
					pBuildingTo->Occupants.AddItem(infantry);
				}
				else
				{
					// Not enough space inside the new Bunker, eject the passenger
					CoordStruct passengerLoc = PassengerKickOutLocation(transportReference, pPassenger);
					CellClass* pCell = MapClass::Instance->TryGetCellAt(passengerLoc);
					pPassenger->LastMapCoords = pCell->MapCoords;
					pPassenger->Unlimbo(passengerLoc, DirType::North);
				}
			}
			else
			{
				double passengerSize = pPassenger->GetTechnoType()->Size;
				CoordStruct passengerLoc = PassengerKickOutLocation(transportReference, pPassenger);

				if (!forceEject && passengerSize > 0 && (passengersLimit - numPassengers - passengerSize >= 0) && passengerSize <= passengersSizeLimit)
				{
					if (pTechnoTo->GetTechnoType()->OpenTopped)
					{
						pPassenger->SetLocation(pTechnoTo->Location);
						pTechnoTo->EnteredOpenTopped(pPassenger);
					}

					pPassenger->Transporter = pTechnoTo;
					pTechnoTo->AddPassenger(pPassenger);
					numPassengers += (int)passengerSize;
				}
				else
				{
					// Not enough space inside the new transport, eject the passenger
					CellClass* pCell = MapClass::Instance->TryGetCellAt(passengerLoc);
					pPassenger->LastMapCoords = pCell->MapCoords;
					pPassenger->Unlimbo(passengerLoc, DirType::North);
				}
			}
		}
	}
}

// =============================
// load / save

template <typename T>
void TechnoExt::ExtData::Serialize(T& Stm)
{
	Stm
		.Process(this->TypeExtData)
		.Process(this->Shield)
		.Process(this->LaserTrails)
		.Process(this->ReceiveDamage)
		.Process(this->PassengerDeletionTimer)
		.Process(this->CurrentShieldType)
		.Process(this->LastWarpDistance)
		.Process(this->AutoDeathTimer)
		.Process(this->MindControlRingAnimType)
		.Process(this->OriginalPassengerOwner)
		.Process(this->IsInTunnel)
		.Process(this->HasBeenPlacedOnMap)
		.Process(this->DeployFireTimer)
		.Process(this->ForceFullRearmDelay)
		.Process(this->WHAnimRemainingCreationInterval)

		.Process(this->DeployAnim)
		.Process(this->Convert_UniversalDeploy_InProgress)
		.Process(this->Convert_UniversalDeploy_MakeInvisible)
		;
}

void TechnoExt::ExtData::LoadFromStream(PhobosStreamReader& Stm)
{
	Extension<TechnoClass>::LoadFromStream(Stm);
	this->Serialize(Stm);
}

void TechnoExt::ExtData::SaveToStream(PhobosStreamWriter& Stm)
{
	Extension<TechnoClass>::SaveToStream(Stm);
	this->Serialize(Stm);
}

bool TechnoExt::LoadGlobals(PhobosStreamReader& Stm)
{
	return Stm
		.Success();
}

bool TechnoExt::SaveGlobals(PhobosStreamWriter& Stm)
{
	return Stm
		.Success();
}

// =============================
// container

TechnoExt::ExtContainer::ExtContainer() : Container("TechnoClass") { }

TechnoExt::ExtContainer::~ExtContainer() = default;


// =============================
// container hooks

DEFINE_HOOK(0x6F3260, TechnoClass_CTOR, 0x5)
{
	GET(TechnoClass*, pItem, ESI);

	TechnoExt::ExtMap.TryAllocate(pItem);

	return 0;
}

DEFINE_HOOK(0x6F4500, TechnoClass_DTOR, 0x5)
{
	GET(TechnoClass*, pItem, ECX);

	TechnoExt::ExtMap.Remove(pItem);

	return 0;
}

DEFINE_HOOK_AGAIN(0x70C250, TechnoClass_SaveLoad_Prefix, 0x8)
DEFINE_HOOK(0x70BF50, TechnoClass_SaveLoad_Prefix, 0x5)
{
	GET_STACK(TechnoClass*, pItem, 0x4);
	GET_STACK(IStream*, pStm, 0x8);

	TechnoExt::ExtMap.PrepareStream(pItem, pStm);

	return 0;
}

DEFINE_HOOK(0x70C249, TechnoClass_Load_Suffix, 0x5)
{
	TechnoExt::ExtMap.LoadStatic();

	return 0;
}

DEFINE_HOOK(0x70C264, TechnoClass_Save_Suffix, 0x5)
{
	TechnoExt::ExtMap.SaveStatic();

	return 0;
}


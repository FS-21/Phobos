#include "Body.h"

#include <Ext/Script/Body.h>

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

		TechnoClass* pDeployed = nullptr;

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

		pThis->InAir = (pThis->GetHeight() > 0);// Force the update

		if (deployToLand)
		{
			if (pThis->InAir)
				return;

			pFoot->StopMoving();
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
		pDeployed = TechnoExt::UniversalConvert(pThis, nullptr);

		if (pDeployed)
		{
			if (convert_DeploySoundIndex >= 0)
				VocClass::PlayAt(convert_DeploySoundIndex, deployLocation);

			if (pAnimFXType)
			{
				if (auto const pAnim = GameCreate<AnimClass>(pAnimFXType, deployLocation))
				{
					if (animFX_FollowDeployer)
						pAnim->SetOwnerObject(pDeployed);

					pAnim->Owner = pDeployed->Owner;
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
	if (!pThis || !ScriptExt::IsUnitAvailable(pThis, false))
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

	auto pOldTechnoExt = TechnoExt::ExtMap.Find(pOldTechno); // I'll consider this never gets nullptr

	// The "Infantry into Vehicle" case we need to check if the cell is free of soldiers
	if (pOldTechnoType->WhatAmI() != AbstractType::Building
		&& pNewTechnoType->WhatAmI() != AbstractType::Building)
	{
		bool inf2inf = pOldTechnoType->WhatAmI() == AbstractType::InfantryType && pNewTechnoType->WhatAmI() == AbstractType::InfantryType;
		int nObjectsInCell = 0;

		for (auto pObject = pOldTechno->GetCell()->FirstObject; pObject; pObject = pObject->NextObject)
		{
			auto const pItem = static_cast<TechnoClass*>(pObject);

			if (pItem && pItem != pOldTechno)
				nObjectsInCell++;

			if (inf2inf && pItem != pOldTechno)
			{
				if (pItem->Location.X == pOldTechno->Location.X && pItem->Location.Y == pOldTechno->Location.Y)
					inf2inf = false;
			}
		}

		if (nObjectsInCell > 0 && !inf2inf)
		{
			pOldTechnoExt->Convert_UniversalDeploy_InProgress = false;
			pThis->IsFallingDown = false;
			CoordStruct loc = CoordStruct::Empty;
			pOldTechno->Scatter(loc, true, false);

			return nullptr;
		}
	}

	// The "Unit into a building" conversion, check if the structure can be placed
	bool oldTechnoIsUnit = pOldTechnoType->WhatAmI() == AbstractType::InfantryType || pOldTechnoType->WhatAmI() == AbstractType::UnitType || pOldTechnoType->WhatAmI() == AbstractType::AircraftType;

	if (oldTechnoIsUnit && pNewTechnoType->WhatAmI() == AbstractType::BuildingType)
	{
		auto pFoot = static_cast<FootClass*>(pThis);
		auto pBuildingType = static_cast<BuildingTypeClass*>(pNewTechnoType);
		bool canDeployIntoStructure = pNewTechnoType->CanCreateHere(CellClass::Coord2Cell(pOldTechno->GetCoords()), pOldTechno->Owner);//pNewTechnoType ? TechnoExt::CanDeployIntoBuilding(pFoot, false, pBuildingType) : false;

		if (!canDeployIntoStructure)
		{
			pOldTechnoExt->Convert_UniversalDeploy_InProgress = false;
			pOldTechno->IsFallingDown = false;
			CoordStruct loc = CoordStruct::Empty;
			pOldTechno->Scatter(loc, true, false);

			return 0;
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

	auto pNewTechnoExt = TechnoExt::ExtMap.Find(pNewTechno); // I'll consider this never gets nullptr

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
	bool unlimboed = pNewTechno->Unlimbo(newLocation, oldPrimaryFacingDir);
	--Unsorted::IKnowWhatImDoing;

	if (!unlimboed)
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
	//pNewTechno->WasFallingDown = pOldTechno->WasFallingDown;
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
		//pEnemy->Target = pNewTechno;
		pEnemy->SetTarget(pNewTechno);
	}

	if (pOldTechno->InLimbo)
	{
		pOwner->RegisterLoss(pOldTechno, false);
		pOldTechno->UnInit();
	}

	//if (!pNewTechno->InLimbo)
	pOwner->RegisterGain(pNewTechno, true);
	pNewTechno->Owner->RecheckTechTree = true;

	// If the object was selected it should remain selected
	if (isSelected)
		pNewTechno->Select();

	return pNewTechno;
}

bool TechnoExt::Techno2TechnoPropertiesTransfer(TechnoClass* pOld, TechnoClass* pNew)
{
	if (!pOld || !pNew || !ScriptExt::IsUnitAvailable(pOld, false) || !ScriptExt::IsUnitAvailable(pNew, false))
		return false;

	auto pOldExt = TechnoExt::ExtMap.Find(pOld);
	auto pNewExt = TechnoExt::ExtMap.Find(pNew);

	if (!pOldExt || !pNewExt)
		return false;

	auto const pOldType = pOld->GetTechnoType();
	auto const pNewType = pNew->GetTechnoType();

	if (!pOldType || !pNewType)
		return false;

	auto pOldTypeExt = TechnoTypeExt::ExtMap.Find(pOldType);
	auto pNewTypeExt = TechnoTypeExt::ExtMap.Find(pNewType);

	if (!pOldTypeExt || !pNewTypeExt)
		return false;

	auto newLocation = pOld->Location;
	auto pOwner = pOld->Owner;

	bool isOldBuilding = pOld->WhatAmI() == AbstractType::Building;
	bool isNewBuilding = pNew->WhatAmI() == AbstractType::Building;
	bool isOldInfantry = pOld->WhatAmI() == AbstractType::Infantry;
	bool isNewInfantry = pNew->WhatAmI() == AbstractType::Infantry;
	bool isOldAircraft = pOld->WhatAmI() == AbstractType::Aircraft;
	bool isNewAircraft = pNew->WhatAmI() == AbstractType::Aircraft;
	bool isOldUnit = pOld->WhatAmI() == AbstractType::Unit;
	bool isNewUnit = pNew->WhatAmI() == AbstractType::Unit;

	// Transfer enemies's target from the old object to the new one
	for (auto pEnemy : *TechnoClass::Array)
	{
		if (pEnemy->Target == pOld)
			pEnemy->SetTarget(pNew);
	}

	// Transfer some stats from the old object to the new:
	// Health update
	double nHealthPercent = (double)(1.0 * pOld->Health / pOldType->Strength);
	pNew->Health = (int)round(pNewType->Strength * nHealthPercent);
	pNew->EstimatedHealth = pNew->Health;

	// Shield update
	if (auto pOldShieldData = pOldExt->Shield.get())
	{
		if (auto pNewShieldData = pNewExt->Shield.get())
		{
			if (pOldExt->CurrentShieldType
				&& pOldShieldData
				&& pNewExt->CurrentShieldType
				&& pNewShieldData)
			{
				double nOldShieldHealthPercent = (double)(1.0 * pOldShieldData->GetHP() / pOldExt->CurrentShieldType->Strength);
				pNewShieldData->SetHP((int)(nOldShieldHealthPercent * pNewExt->CurrentShieldType->Strength));
			}
		}
	}

	// Veterancy update
	if (pOldTypeExt->Convert_TransferVeterancy)
	{
		VeterancyStruct nVeterancy = pOld->Veterancy;
		pNew->Veterancy = nVeterancy;
	}

	// AI team update
	if (pOld->BelongsToATeam())
	{
		auto pOldFoot = static_cast<FootClass*>(pOld);
		auto pNewFoot = static_cast<FootClass*>(pNew);

		if (pNewFoot)
			pNewFoot->Team = pOldFoot->Team;
	}

	// Ammo update
	pNew->Ammo = pNew->Ammo > pOld->Ammo ? pOld->Ammo : pNew->Ammo;

	// Mind control update
	if (pOld->IsMindControlled())
		TechnoExt::TransferMindControlOnDeploy(pOld, pNew);

	// Initial mission update
	pNew->QueueMission(Mission::Guard, true);

	// Cloak update
	pNew->CloakState = pOld->CloakState;

	// Facing update
	pNew->PrimaryFacing.SetCurrent(pOld->PrimaryFacing.Current());
	pNew->SecondaryFacing.SetCurrent(pOld->SecondaryFacing.Current());

	if (pOldTypeExt->Convert_DeployDir >= 0)
	{
		DirStruct newDesiredFacing;
		DirType newDesiredDir = (static_cast<DirType>(pOldTypeExt->Convert_DeployDir * 32));

		newDesiredFacing.SetDir(newDesiredDir);
		pNew->PrimaryFacing.SetCurrent(newDesiredFacing); // Body
		pNew->SecondaryFacing.SetCurrent(newDesiredFacing); // Turret
	}

	// Jumpjet tricks
	if (pNewType->JumpJet || pNewType->BalloonHover)
	{
		// Jumpjets should fly if
		auto pFoot = static_cast<FootClass*>(pNew);
		pFoot->Scatter(CoordStruct::Empty, true, false);
		//pFoot->Locomotor->Move_To(CoordStruct::Empty);
		//pFoot->Locomotor->Do_Turn(oldPrimaryFacing);
		//pNew->PrimaryFacing.SetDesired(oldPrimaryFacing);
	}
	else
	{
		if (pNewType->MovementZone == MovementZone::Fly)
			pNew->IsFallingDown = false; // Probably isn't necessary since the new object should not have this "true"
	}

	// Inherit other stuff
	pNew->WarpingOut = pOld->WarpingOut;
	pNew->unknown_280 = pOld->unknown_280; // Sth related to teleport
	pNew->BeingWarpedOut = pOld->BeingWarpedOut;
	pNew->Deactivated = pOld->Deactivated;
	pNew->Flash(pOld->Flashing.DurationRemaining);
	pNew->IdleActionTimer = pOld->IdleActionTimer;
	pNew->ChronoLockRemaining = pOld->ChronoLockRemaining;
	pNew->Berzerk = pOld->Berzerk;
	pNew->BerzerkDurationLeft = pOld->BerzerkDurationLeft;
	pNew->ChronoWarpedByHouse = pOld->ChronoWarpedByHouse;
	pNew->EMPLockRemaining = pOld->EMPLockRemaining;
	pNew->ShouldLoseTargetNow = pOld->ShouldLoseTargetNow;
	pNew->SetOwningHouse(pOld->GetOwningHouse(), false);// Is really necessary? the object was created with the final owner by default... I have to try without this tag

	// Transfer Iron Courtain effect, if applied
	TechnoExt::SyncIronCurtainStatus(pOld, pNew);

	// Transfer passengers/garrisoned units from old object to the new, if possible
	TechnoExt::PassengersTransfer(pOld, pNew);

	// Transfer parasite, if possible
	if (auto const pOldFoot = abstract_cast<FootClass*>(pOld))
	{
		if (auto const pParasite = pOldFoot->ParasiteEatingMe)
		{
			// Remove parasite
			pParasite->ParasiteImUsing->SuppressionTimer.Start(50);
			pParasite->ParasiteImUsing->ExitUnit();

			if (auto const pNewFoot = abstract_cast<FootClass*>(pNew))
			{
				// Try to transfer parasite
				pParasite->ParasiteImUsing->TryInfect(pNewFoot);
			}
		}
	}

	// Transfer AttachEffect (Reminder: add a new tag) - TO-DO: There is a Phobos PR that I should support once is merged into develop branch

	// If the object was selected it should remain selected
	if (pOld->IsSelected)
		pNew->Select();

	return true;
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

	// We'll get the passengers list in a more easy data structure (to me)
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

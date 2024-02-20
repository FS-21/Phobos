#include "Body.h"

#include <Ext/Script/Body.h>

void TechnoExt::CreateUniversalDeployAnimation(TechnoClass* pThis)
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
	if (!pThis || !ScriptExt::IsUnitAvailable(pThis, false))
		return;

	auto pThisExt = TechnoExt::ExtMap.Find(pThis);
	if (!pThisExt || !pThisExt->Convert_UniversalDeploy_InProgress)
		return;

	bool isOriginalDeployer = pThisExt->Convert_UniversalDeploy_IsOriginalDeployer;
	TechnoClass* pOld = !isOriginalDeployer ? pThisExt->Convert_TemporalTechno : pThis;

	auto pOldExt = TechnoExt::ExtMap.Find(pOld);
	if (!pOldExt)
		return;

	auto pOldType = pOld->GetTechnoType();
	if (!pOldType)
		return;

	auto pOldTypeExt = TechnoTypeExt::ExtMap.Find(pOldType);
	if (!pOldTypeExt)
		return;

	TechnoClass* pNew = !isOriginalDeployer ? pThis : nullptr;
	auto const pNewType = !isOriginalDeployer ? pThis->GetTechnoType() : pOldTypeExt->Convert_UniversalDeploy.at(0);

	auto pNewTypeExt = TechnoTypeExt::ExtMap.Find(pNewType);
	if (!pNewTypeExt)
		return;

	bool isOldBuilding = pOldType->WhatAmI() == AbstractType::BuildingType;
	bool isOldInfantry = pOldType->WhatAmI() == AbstractType::InfantryType;
	bool isOldAircraft = pOldType->WhatAmI() == AbstractType::AircraftType || pOldType->ConsideredAircraft;
	bool isOldUnit = pOldType->WhatAmI() == AbstractType::UnitType;
	bool oldTechnoIsUnit = isOldInfantry || isOldUnit || isOldAircraft;

	bool isNewBuilding = pNewType->WhatAmI() == AbstractType::BuildingType;
	bool isNewInfantry = pNewType->WhatAmI() == AbstractType::InfantryType;
	bool isNewAircraft = pNewType->WhatAmI() == AbstractType::AircraftType || pNewType->ConsideredAircraft;
	bool isNewUnit = pNewType->WhatAmI() == AbstractType::UnitType;
	bool newTechnoIsUnit = isNewInfantry || isNewUnit || isNewAircraft;

	auto const pOwner = pOld->Owner;
	bool canDeployIntoStructure = false;
	bool deployToLand = pOldTypeExt->Convert_DeployToLand;
	auto pOldFoot = static_cast<FootClass*>(pOld);
	CoordStruct deployerLocation = pOld->GetCoords();
	CoordStruct deploymentLocation = isOldInfantry && isNewInfantry ? deployerLocation : pOld->GetCell()->GetCenterCoords();
	DirType currentDir = pOld->PrimaryFacing.Current().GetDir(); // Returns current position in format [0 - 7] x 32

	if (oldTechnoIsUnit && !pNew)
	{
		// Deployment to land check: Make sure the object will fall into ground (if some external factor change it it restores the fall)
		if (deployToLand)
			pOld->IsFallingDown = true;

		if (!pOld->IsFallingDown && pOldFoot->Locomotor->Is_Really_Moving_Now())
			return;

		// Deployment to land check: Update InAir information & let it fall into ground
		if (deployToLand && pOld->GetHeight() <= 20)
		{
			pOld->SetHeight(0);
			//pOld->Location.Z = 0;
		}

		pOld->InAir = pOld->GetHeight() > 0; // Force the update

		if (deployToLand)
		{
			if (pOld->InAir)
				return;

			pOldFoot->StopMoving();
			pOldFoot->ParalysisTimer.Start(15); // Be quiet old unit and let me change you...
		}
	}

	// Unit direction check: Update the direction of the unit until it reaches the desired one, if specified
	// TO-DO: Maybe support structures... instead of PrimaryFacing it would check SecondaryFacing (the turret). Maybe it already supports it but I have to do tests
	if (isOldUnit || isOldAircraft) // TO-DO: I have to test aircraft behaviour here...
	{
		// Turn the unit to the right deploy facing
		if (!pOldExt->DeployAnim && pOldTypeExt->Convert_DeployDir >= 0)
		{
			DirType desiredDir = (static_cast<DirType>(pOldTypeExt->Convert_DeployDir * 32));
			DirStruct desiredFacing;
			desiredFacing.SetDir(desiredDir);

			if (currentDir != desiredDir)
			{
				pOldFoot->Locomotor->Move_To(CoordStruct::Empty);
				pOld->PrimaryFacing.SetDesired(desiredFacing);

				return;
			}
		}
	}

	/*if (isNewBuilding)
	{
		pOld->Location.Z = 0; // No flying structures available in this game
		deployLocation.Z = 0;
		canDeployIntoStructure = pNewType->CanCreateHere(CellClass::Coord2Cell(pOld->GetCoords()), pOld->Owner);

		if (!canDeployIntoStructure)
		{
			pOldExt->Convert_UniversalDeploy_InProgress = false;
			pOld->IsFallingDown = false;
			// TO-DO: Play EVA: "Can not deploy here"

			return;
		}
	}*/

	bool selected = false;
	if (pOld->IsSelected)
		selected = true;

	// Here we go, the real conversions! There are 2 cases

	// Case 1: "Unit into something" deploy.
	// Unit should loose the shape and get the structure shape.
	// This also prevents other units enter inside the structure foundation.

	if (oldTechnoIsUnit)// && isNewBuilding)
	{
		//TechnoExt::RunStructureIntoTechnoConversion(pOld, pNewType);

		if (!pOld->InLimbo)
			pOld->Limbo();

		// Create & save it for later.
		// Note: Remember to delete it in case of deployment failure
		if (!pOldExt->Convert_TemporalTechno)
		{
			pNew = static_cast<TechnoClass*>(pNewType->CreateObject(pOwner));

			if (!pNew)
			{
				pOldExt->Convert_TemporalTechno = nullptr;
				pOldExt->Convert_UniversalDeploy_MakeInvisible = false;
				pOldExt->Convert_UniversalDeploy_InProgress = false;
				pOld->IsFallingDown = false;

				++Unsorted::IKnowWhatImDoing;
				pOld->Unlimbo(deployerLocation, currentDir);
				--Unsorted::IKnowWhatImDoing;

				if (selected)
					pOld->Select();

				return;
			}

			pOldExt->Convert_TemporalTechno = pNew;
			bool unlimboed = pNew->Unlimbo(deploymentLocation, currentDir);

			// Failed deployment: restore the old object and abort operation
			if (!unlimboed)
			{
				pOldExt->Convert_TemporalTechno = nullptr;
				pOldExt->Convert_UniversalDeploy_IsOriginalDeployer = true;
				pOldExt->Convert_UniversalDeploy_MakeInvisible = false;
				pOldExt->Convert_UniversalDeploy_InProgress = false;
				pOld->IsFallingDown = false;

				++Unsorted::IKnowWhatImDoing;
				pOld->Unlimbo(deployerLocation, currentDir);
				--Unsorted::IKnowWhatImDoing;

				pOldFoot->ParalysisTimer.Stop();
				pOld->ForceMission(Mission::Guard);

				pNew->UnInit();

				if (selected)
					pOld->Select();

				return;
			}

			if (auto pBuildingNew = static_cast<BuildingClass*>(pNew))
			{
				pBuildingNew->HasPower = false;

				if (pBuildingNew->Factory)
				{
					pBuildingNew->IsPrimaryFactory = false;
					pBuildingNew->Factory->IsSuspended = true;
				}
			}
		}

		// At this point the new object exists & is visible.
		// If exists a deploy animation it must dissappear until the animation finished
		pNew = !isOriginalDeployer ? pThis : pOldExt->Convert_TemporalTechno;
		pOldExt->Convert_UniversalDeploy_IsOriginalDeployer = true;
		auto pNewExt = TechnoExt::ExtMap.Find(pNew);
		pNewExt->Convert_TemporalTechno = pOld;
		pNewExt->Convert_UniversalDeploy_IsOriginalDeployer = false;
		pNewExt->Convert_UniversalDeploy_InProgress = true;

		// Setting the build up animation, if any.
		AnimTypeClass* pDeployAnimType = pOldTypeExt->Convert_DeployingAnim.isset() ? pOldTypeExt->Convert_DeployingAnim.Get() : nullptr;

		if (selected)
			pNew->Select();

		if (pDeployAnimType)
		{
			bool isDeployAnimPlaying = pOldExt->DeployAnim ? true : false;
			bool hasDeployAnimFinished = (isDeployAnimPlaying && (pOldExt->DeployAnim->Animation.Value >= (pDeployAnimType->End + pDeployAnimType->Start - 1))) ? true : false;

			// Hack for making the object invisible during a deploy
			pNewExt->Convert_UniversalDeploy_MakeInvisible = true;

			// The conversion process won't start until the deploy animation ends
			if (!isDeployAnimPlaying)
			{
				TechnoExt::CreateUniversalDeployAnimation(pOld);
				return;
			}

			if (!hasDeployAnimFinished)
				return;

			// Should I uninit() the finished animation?
		}

		// The build up animation finished (if any).
		// In this case we only have to transfer properties to the new object
		pNewExt->Convert_UniversalDeploy_MakeInvisible = false;
		pNewExt->Convert_UniversalDeploy_IsOriginalDeployer = false;
		pNewExt->Convert_UniversalDeploy_ForceRedraw = true;

		if (auto pBuildingNew = static_cast<BuildingClass*>(pNew))
		{
			pBuildingNew->HasPower = true;

			if (pBuildingNew->Factory)
				pBuildingNew->Factory->IsSuspended = false;
		}

		TechnoExt::Techno2TechnoPropertiesTransfer(pOld, pNew);
		pNew->MarkForRedraw();

		// Play post-deploy sound
		int convert_DeploySoundIndex = pOldTypeExt->Convert_DeploySound.isset() ? pOldTypeExt->Convert_DeploySound.Get() : -1;
		AnimTypeClass* pAnimFXType = pOldTypeExt->Convert_AnimFX.isset() ? pOldTypeExt->Convert_AnimFX.Get() : nullptr;
		bool animFX_FollowDeployer = pOldTypeExt->Convert_AnimFX_FollowDeployer;

		if (convert_DeploySoundIndex >= 0)
			VocClass::PlayAt(convert_DeploySoundIndex, deploymentLocation);

		// Play post-deploy animation
		if (pAnimFXType)
		{
			if (auto const pAnim = GameCreate<AnimClass>(pAnimFXType, deploymentLocation))
			{
				if (animFX_FollowDeployer)
					pAnim->SetOwnerObject(pNew);

				pAnim->Owner = pNew->Owner;
			}
		}

		// The conversion process finished. Clean values
		if (pOld->InLimbo)
		{
			pOldExt->Convert_TemporalTechno = nullptr;
			pOldExt->Convert_UniversalDeploy_IsOriginalDeployer = true;
			pOldExt->Convert_UniversalDeploy_InProgress = false;
			pNewExt->Convert_TemporalTechno = nullptr;
			pNewExt->Convert_UniversalDeploy_IsOriginalDeployer = false;
			pNewExt->Convert_UniversalDeploy_InProgress = false;

			pOwner->RegisterLoss(pOld, false);
			pOwner->RemoveTracking(pOld);
			pOld->UnInit();
		}

		pOwner->AddTracking(pNew);
		pOwner->RegisterGain(pNew, false);
		pNew->Owner->RecheckTechTree = true;
		pNew->Owner->RecheckPower = true;
		pNew->Owner->RecheckRadar = true;

		return;
	}

	// Case 2: "Building into something" deploy
	// Structure foundation should remain until the deploy animation ends (if any).
	// This also prevents other units enter inside the structure foundation.
	// This case should cover the "structure into structure".
	if (isOldBuilding)
	{
		// Create & save it for later.
		// Note: Remember to delete it in case of deployment failure
		if (!pOldExt->Convert_TemporalTechno)
		{
			pNew = static_cast<TechnoClass*>(pNewType->CreateObject(pOwner));

			if (!pNew)
			{
				pOldExt->Convert_TemporalTechno = nullptr;
				pOldExt->Convert_UniversalDeploy_MakeInvisible = false;
				pOldExt->Convert_UniversalDeploy_InProgress = false;
				pOldExt->Convert_UniversalDeploy_ForceRedraw = true;
				pOld->IsFallingDown = false;

				++Unsorted::IKnowWhatImDoing;
				pOld->Unlimbo(deployerLocation, currentDir);
				--Unsorted::IKnowWhatImDoing;

				if (selected)
					pOld->Select();

				return;
			}

			pNew->ForceMission(Mission::Guard);
			pOldExt->Convert_TemporalTechno = pNew;

			if (auto pBuildingOld = static_cast<BuildingClass*>(pOld))
			{
				pBuildingOld->HasPower = false;

				if (pBuildingOld->Factory)
				{
					pBuildingOld->IsPrimaryFactory = false;
					pBuildingOld->Factory->IsSuspended = true;
				}
			}
		}

		// At this point the new object exists & is visible.
		// If exists a deploy animation it must dissappear until the animation finished
		pNew = !isOriginalDeployer ? pThis : pOldExt->Convert_TemporalTechno;
		pOldExt->Convert_UniversalDeploy_IsOriginalDeployer = true;
		auto pNewExt = TechnoExt::ExtMap.Find(pNew);
		pNewExt->Convert_TemporalTechno = pOld;
		pNewExt->Convert_UniversalDeploy_IsOriginalDeployer = false;
		pNewExt->Convert_UniversalDeploy_InProgress = true;

		// Setting the build up animation, if any.
		AnimTypeClass* pDeployAnimType = pOldTypeExt->Convert_DeployingAnim.isset() ? pOldTypeExt->Convert_DeployingAnim.Get() : nullptr;

		if (pDeployAnimType)
		{
			bool isDeployAnimPlaying = pOldExt->DeployAnim ? true : false;
			bool hasDeployAnimFinished = (isDeployAnimPlaying && (pOldExt->DeployAnim->Animation.Value >= (pDeployAnimType->End + pDeployAnimType->Start - 1))) ? true : false;

			// Hack for making the object invisible during a deploy
			pOldExt->Convert_UniversalDeploy_MakeInvisible = true;
			pOld->MarkForRedraw();

			// The conversion process won't start until the deploy animation ends
			if (!isDeployAnimPlaying)
			{
				TechnoExt::CreateUniversalDeployAnimation(pOld);
				return;
			}

			if (!hasDeployAnimFinished)
				return;

			// Should I uninit() the finished animation?
		}

		// The build up animation finished (if any).
		if (!pOld->InLimbo)
			pOld->Limbo();

		bool unlimboed = pNew->Unlimbo(deploymentLocation, currentDir);

		// Failed deployment: restore the old object and abort operation
		if (!unlimboed)
		{
			pOldExt->Convert_TemporalTechno = nullptr;
			pOldExt->Convert_UniversalDeploy_IsOriginalDeployer = true;
			pOldExt->Convert_UniversalDeploy_MakeInvisible = false;
			pOldExt->Convert_UniversalDeploy_InProgress = false;
			pOldExt->Convert_UniversalDeploy_ForceRedraw = true;
			pOld->IsFallingDown = false;

			++Unsorted::IKnowWhatImDoing;
			pOld->Unlimbo(deployerLocation, currentDir);
			--Unsorted::IKnowWhatImDoing;

			pOldFoot->ParalysisTimer.Stop();
			pOld->ForceMission(Mission::Guard);

			if (auto pBuildingOld = static_cast<BuildingClass*>(pOld))
			{
				pBuildingOld->HasPower = true;

				if (pBuildingOld->Factory)
					pBuildingOld->Factory->IsSuspended = false;
			}

			pNew->UnInit();

			if (selected)
				pOld->Select();

			return;
		}

		if (selected)
			pNew->Select();

		// Repaint the building graphics. Needed
		if (auto pBuildingNew = static_cast<BuildingClass*>(pNew))
		{
			pBuildingNew->BeginMode(BStateType::Active);
			pNew->CurrentMission = Mission::Guard;
			pBuildingNew->MarkForRedraw();
		}

		TechnoExt::Techno2TechnoPropertiesTransfer(pOld, pNew);

		// Play post-deploy sound
		int convert_DeploySoundIndex = pOldTypeExt->Convert_DeploySound.isset() ? pOldTypeExt->Convert_DeploySound.Get() : -1;
		AnimTypeClass* pAnimFXType = pOldTypeExt->Convert_AnimFX.isset() ? pOldTypeExt->Convert_AnimFX.Get() : nullptr;
		bool animFX_FollowDeployer = pOldTypeExt->Convert_AnimFX_FollowDeployer;

		if (convert_DeploySoundIndex >= 0)
			VocClass::PlayAt(convert_DeploySoundIndex, deploymentLocation);

		// Play post-deploy animation
		if (pAnimFXType)
		{
			if (auto const pAnim = GameCreate<AnimClass>(pAnimFXType, deploymentLocation))
			{
				if (animFX_FollowDeployer)
					pAnim->SetOwnerObject(pNew);

				pAnim->Owner = pNew->Owner;
			}
		}

		// The conversion process finished. Clean values
		if (pOld->InLimbo)
		{
			pOldExt->Convert_TemporalTechno = nullptr;
			pOldExt->Convert_UniversalDeploy_IsOriginalDeployer = false;
			pOldExt->Convert_UniversalDeploy_InProgress = false;
			pNewExt->Convert_TemporalTechno = nullptr;
			pNewExt->Convert_UniversalDeploy_IsOriginalDeployer = false;
			pNewExt->Convert_UniversalDeploy_InProgress = false;
			pNewExt->Convert_UniversalDeploy_ForceRedraw = true;

			pOwner->RegisterLoss(pOld, false);
			pOwner->RemoveTracking(pOld);
			pOld->UnInit();
		}

		if (newTechnoIsUnit)
		{
			auto pCell = pNew->GetCell();
			bool cellIsOnWater = (pCell->LandType == LandType::Water || pCell->LandType == LandType::Beach);

			// Jumpjet tricks: if they are in the ground make air units fly
			if ((!pNew->InAir || cellIsOnWater) && (pNewType->JumpJet || pNewType->BalloonHover))
			{
				// Jumpjets should fly if
				auto pFoot = static_cast<FootClass*>(pNew);
				pFoot->Scatter(CoordStruct::Empty, true, false);
				pNew->IsFallingDown = false; // Probably isn't necessary since the new object should not have this "true"
			}
			else
			{
				if (isNewAircraft && cellIsOnWater)
				{
					auto pFoot = static_cast<FootClass*>(pNew);
					pFoot->Scatter(CoordStruct::Empty, true, false);
					pNew->IsFallingDown = false; // Probably isn't necessary since the new object should not have this "true"
				}
			}
		}

		pNewExt->Convert_UniversalDeploy_InProgress = false;
		pNewExt->Convert_UniversalDeploy_IsOriginalDeployer = false;
		pNew->MarkForRedraw();
		pOwner->AddTracking(pNew);
		pOwner->RegisterGain(pNew, false);
		pNew->Owner->RecheckTechTree = true;
		pNew->Owner->RecheckPower = true;
		pNew->Owner->RecheckRadar = true;
	}
}

// This method transfer all settings from the old object to the new.
// If a new object isn't defined then it will be picked from a list.
// It will have less checks than the Update
TechnoClass* TechnoExt::UniversalDeployConversion(TechnoClass* pOld, TechnoTypeClass* pNewType)
{
	if (!pOld || !ScriptExt::IsUnitAvailable(pOld, false))
		return nullptr;

	auto pOldExt = TechnoExt::ExtMap.Find(pOld);
	if (pOldExt->Convert_UniversalDeploy_InProgress)// TO-DO: handle interference between a conversion in progress and this method
		return nullptr;

	auto pOldType = pOld->GetTechnoType();
	if (!pOldType)
		return nullptr;

	auto pOldTypeExt = TechnoTypeExt::ExtMap.Find(pOldType);

	// If the new object isn't defined then it will be picked from a list
	if (!pNewType)
	{
		if (pOldTypeExt->Convert_UniversalDeploy.size() == 0)
			return nullptr;

		// TO-DO: having multiple deploy candidate IDs it should pick randomly from the list
		// Probably a new tag should enable this random behaviour...
		int index = 0; //ScenarioClass::Instance->Random.RandomRanged(0, pOldTechnoTypeExt->Convert_UniversalDeploy.size() - 1);
		pNewType = pOldTypeExt->Convert_UniversalDeploy.at(index);
	}

	bool isOldBuilding = pOldType->WhatAmI() == AbstractType::BuildingType;
	bool isOldInfantry = pOldType->WhatAmI() == AbstractType::InfantryType;
	bool isOldAircraft = pOldType->WhatAmI() == AbstractType::AircraftType || pOldType->ConsideredAircraft;
	bool isOldUnit = pOldType->WhatAmI() == AbstractType::UnitType;
	bool oldTechnoIsUnit = isOldInfantry || isOldUnit || isOldAircraft;

	bool isNewBuilding = pNewType->WhatAmI() == AbstractType::BuildingType;
	bool isNewInfantry = pNewType->WhatAmI() == AbstractType::InfantryType;
	bool isNewAircraft = pNewType->WhatAmI() == AbstractType::AircraftType || pNewType->ConsideredAircraft;
	bool isNewUnit = pNewType->WhatAmI() == AbstractType::UnitType;
	bool newTechnoIsUnit = isNewInfantry || isNewUnit || isNewAircraft;

	auto pNewTypeExt = TechnoTypeExt::ExtMap.Find(pNewType);
	auto const pOwner = pOld->Owner;
	pOld->InAir = (pOld->GetHeight() > 0); // Force the update
	CoordStruct deployerLocation = pOld->GetCoords();
	CoordStruct deploymentLocation = isOldInfantry && isNewInfantry ? deployerLocation : pOld->GetCell()->GetCenterCoords(); // infantry into infantry allows unlimbo into a sub-cell
	DirType currentDir = pOld->PrimaryFacing.Current().GetDir(); // Returns current position in format [0 - 7] x 32

	if (isNewBuilding && pOld->InAir)
		return nullptr;

	TechnoClass* pNew = static_cast<TechnoClass*>(pNewType->CreateObject(pOwner));
	if (!pNew)
		return nullptr;

	pNew->ForceMission(Mission::Guard);

	bool selected = false;
	if (pOld->IsSelected)
		pOld->Select();

	++Unsorted::IKnowWhatImDoing;
	pOld->Limbo();
	bool unlimboed = pNew->Unlimbo(deploymentLocation, currentDir);
	--Unsorted::IKnowWhatImDoing;

	if (!unlimboed)
	{
		// I think here won't enter I avoided checks
		++Unsorted::IKnowWhatImDoing;
		pNew->UnInit();
		pOld->Unlimbo(deployerLocation, currentDir);
		--Unsorted::IKnowWhatImDoing;
	}

	// If I interrumped an UniversalDeploy process...
	//pOldExt->Convert_TemporalTechno = nullptr;
	//pOldExt->Convert_UniversalDeploy_MakeInvisible = false;
	//pOldExt->Convert_UniversalDeploy_InProgress = false;
	//pOldExt->Convert_UniversalDeploy_ForceRedraw = false;
	//pOld->IsFallingDown = false;

	pNew->InAir = (pOld->GetHeight() > 0); // Force the update

	if (pNew->InAir && !isNewAircraft) // Fall...
		pNew->Scatter(CoordStruct::Empty, true, false);

	if (newTechnoIsUnit)
	{
		auto pCell = pNew->GetCell();
		bool cellIsOnWater = (pCell->LandType == LandType::Water || pCell->LandType == LandType::Beach);

		// Jumpjet tricks: if they are in the ground make air units fly
		if ((!pNew->InAir || cellIsOnWater) && (pNewType->JumpJet || pNewType->BalloonHover))
		{
			// Jumpjets should fly if
			auto pFoot = static_cast<FootClass*>(pNew);
			pFoot->Scatter(CoordStruct::Empty, true, false);
			pNew->IsFallingDown = false; // Probably isn't necessary since the new object should not have this "true"
		}
		else
		{
			if (isNewAircraft && cellIsOnWater)
			{
				auto pFoot = static_cast<FootClass*>(pNew);
				pFoot->Scatter(CoordStruct::Empty, true, false);
				pNew->IsFallingDown = false; // Probably isn't necessary since the new object should not have this "true"
			}
		}
	}

	// Transfer all the important details
	TechnoExt::Techno2TechnoPropertiesTransfer(pOld, pNew);
	
	pOwner->RemoveTracking(pOld);
	pOwner->RegisterLoss(pOld, false);
	pOwner->AddTracking(pNew);
	pOwner->RegisterGain(pNew, false);
	pNew->MarkForRedraw();
	pNew->Owner->RecheckTechTree = true;
	pNew->Owner->RecheckPower = true;
	pNew->Owner->RecheckRadar = true;

	pOld->UnInit();

	return pNew;
}

/*
// This method transfer all settings from the old object to the new.
// If a new object isn't defined then it will be picked from a list.
// It will have less checks than the Update
TechnoClass* TechnoExt::UniversalDeployConversion(TechnoClass* pOld, TechnoTypeClass* pNewType)
{
	if (!pOld || !ScriptExt::IsUnitAvailable(pOld, false))
		return nullptr;

	auto pOldExt = TechnoExt::ExtMap.Find(pOld); // I'll consider this never gets nullptr

	auto pOldType = pOld->GetTechnoType();
	if (!pOldType)
		return nullptr;

	auto pOldTypeExt = TechnoTypeExt::ExtMap.Find(pOldType);

	// If the new object isn't defined then it will be picked from a list
	if (!pNewType)
	{
		if (pOldTypeExt->Convert_UniversalDeploy.size() == 0)
			return nullptr;

		// TO-DO: having multiple deploy candidate IDs it should pick randomly from the list
		// Probably a new tag should enable this random behaviour...
		int index = 0; //ScenarioClass::Instance->Random.RandomRanged(0, pOldTechnoTypeExt->Convert_UniversalDeploy.size() - 1);
		pNewType = pOldTypeExt->Convert_UniversalDeploy.at(index);
	}

	auto pNewTypeExt = TechnoTypeExt::ExtMap.Find(pNewType);
	auto newLocation = pOld->Location;
	auto const pOwner = pOld->Owner;

	bool isOldBuilding = pOldType->WhatAmI() == AbstractType::BuildingType;
	bool isNewBuilding = pNewType->WhatAmI() == AbstractType::BuildingType;
	bool isOldInfantry = pOldType->WhatAmI() == AbstractType::InfantryType;
	bool isNewInfantry = pNewType->WhatAmI() == AbstractType::InfantryType;
	bool isOldAircraft = pOldType->WhatAmI() == AbstractType::AircraftType;
	bool isOldUnit = pOldType->WhatAmI() == AbstractType::UnitType;
	bool oldTechnoIsUnit = isOldInfantry || isOldUnit || isOldAircraft;

	// "Non-building into non-Building" check: abort if 2 o more units are placed in the same cell
	if (!isOldBuilding && !isNewBuilding)
	{
		bool inf2inf = isOldInfantry && isNewInfantry;
		int nObjectsInCell = 0;

		for (auto pObject = pOld->GetCell()->FirstObject; pObject; pObject = pObject->NextObject)
		{
			auto const pItem = static_cast<TechnoClass*>(pObject);

			if (pItem && pItem != pOld)
				nObjectsInCell++;

			// Special case: infantry uses sub-locations, can be placed multiple solders in the same cell but not in the same location
			if (inf2inf && pItem != pOld)
			{
				if (pItem->Location.X == pOld->Location.X && pItem->Location.Y == pOld->Location.Y)
					inf2inf = false;
			}
		}

		if (nObjectsInCell > 0 && !inf2inf)
		{
			pOldExt->Convert_UniversalDeploy_InProgress = false;
			pOld->IsFallingDown = false;
			CoordStruct loc = CoordStruct::Empty;
			pOld->Scatter(loc, true, false);

			return nullptr;
		}
	}

	// The "Unit into a building" conversion, check if the structure can be placed
	if (oldTechnoIsUnit && isNewBuilding)
	{
		auto pFoot = static_cast<FootClass*>(pOld);
		auto pBuildingType = static_cast<BuildingTypeClass*>(pNewType);
		bool canDeployIntoStructure = pNewType->CanCreateHere(CellClass::Coord2Cell(pOld->GetCoords()), pOld->Owner);//pNewType ? TechnoExt::CanDeployIntoBuilding(pFoot, false, pBuildingType) : false;

		if (!canDeployIntoStructure)
		{
			pOldExt->Convert_UniversalDeploy_InProgress = false;
			pOld->IsFallingDown = false;
			CoordStruct loc = CoordStruct::Empty;
			pOld->Scatter(loc, true, false);

			return 0;
		}
	}

	auto pNew = static_cast<TechnoClass*>(pNewType->CreateObject(pOwner));

	if (!pNew)
	{
		pOldExt->Convert_UniversalDeploy_InProgress = false;
		pOld->IsFallingDown = false;
		//CoordStruct loc = CoordStruct::Empty;
		//pOldTechno->Scatter(loc, true, false);

		return nullptr;
	}

	// Facing update
	DirStruct oldPrimaryFacing;
	DirStruct oldSecondaryFacing;

	if (pOldTypeExt->Convert_DeployDir >= 0)
	{
		DirType desiredDir = (static_cast<DirType>(pOldTypeExt->Convert_DeployDir * 32));
		oldPrimaryFacing.SetDir(desiredDir);
		oldSecondaryFacing.SetDir(desiredDir);
	}
	else
	{
		oldPrimaryFacing = pOld->PrimaryFacing.Current();
		oldSecondaryFacing = pOld->SecondaryFacing.Current();
	}

	DirType oldPrimaryFacingDir = oldPrimaryFacing.GetDir(); // Returns current position in format [0 - 7] x 32

	// Some vodoo magic
	pOld->Limbo();

	++Unsorted::IKnowWhatImDoing;
	bool unlimboed = pNew->Unlimbo(newLocation, oldPrimaryFacingDir);
	--Unsorted::IKnowWhatImDoing;

	if (!unlimboed)
	{
		// Abort operation, restoring old object
		pOld->Unlimbo(newLocation, oldPrimaryFacingDir);
		pNew->UnInit();

		pOldExt->Convert_UniversalDeploy_InProgress = false;
		pOld->IsFallingDown = false;

		return nullptr;
	}

	TechnoExt::Techno2TechnoPropertiesTransfer(pOld, pNew);

	if (pOld->InLimbo)
	{
		pOwner->RegisterLoss(pOld, false);
		pOld->UnInit();
	}

	//if (!pNewTechno->InLimbo)
	pOwner->RegisterGain(pNew, true);
	pNew->Owner->RecheckTechTree = true;

	return pNew;
}
*/

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

	// Jumpjet tricks: if they are in the ground make air units fly
	auto pCell = pNew->GetCell();
	bool cellIsOnWater = (pCell->LandType == LandType::Water || pCell->LandType == LandType::Beach);

	// Jumpjet tricks: if they are in the ground make air units fly
	if ((!pNew->InAir || cellIsOnWater) && (pNewType->JumpJet || pNewType->BalloonHover))
	{
		// Jumpjets should fly if
		auto pFoot = static_cast<FootClass*>(pNew);
		pFoot->Scatter(CoordStruct::Empty, true, false);
		pNew->IsFallingDown = false; // Probably isn't necessary since the new object should not have this "true"
	}
	else
	{
		if (isNewAircraft && cellIsOnWater)
		{
			auto pFoot = static_cast<FootClass*>(pNew);
			pFoot->Scatter(CoordStruct::Empty, true, false);
			pNew->IsFallingDown = false; // Probably isn't necessary since the new object should not have this "true"
		}
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
	pNew->AttachedTag = pOld->AttachedTag;
	pNew->AttachedBomb = pOld->AttachedBomb;

	// Detach CLEG targeting
	auto tempUsing = pOld->TemporalImUsing;
	if (tempUsing && tempUsing->Target)
		tempUsing->Detach();

	/*if (pOld->Target)
	{
		pNew->SetTarget(pOld->Target);
		pNew->QueueMission(pOld->CurrentMission, true);
	}*/

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
			if (pTechnoTo && pTechnoTo->WhatAmI() == AbstractType::Building)
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

void TechnoExt::RunStructureIntoTechnoConversion(TechnoClass* pOld, TechnoTypeClass* pNewType)
{

}

void TechnoExt::RunTechnoIntoStructureConversion(TechnoClass* pOld, TechnoTypeClass* pNewType)
{

}

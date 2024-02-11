#include "Body.h"

DEFINE_HOOK(0x730B8F, DeployCommand_UniversalDeploy, 0x6)
{
	GET(int, index, EDI);

	TechnoClass* pThis = static_cast<TechnoClass*>(ObjectClass::CurrentObjects->GetItem(index));

	auto pExt = TechnoExt::ExtMap.Find(pThis);
	if (!pExt)
		return 0;

	if (pExt->Convert_UniversalDeploy_InProgress)
		return 0;

	auto pTypeExt = TechnoTypeExt::ExtMap.Find(pThis->GetTechnoType());
	if (!pTypeExt)
		return 0;

	if (pTypeExt->Convert_UniversalDeploy.size() == 0)
		return 0;

	// Building case, send the undeploy signal
	if (pThis->WhatAmI() == AbstractType::Building)
	{
		pThis->MissionStatus = 0;
		pThis->CurrentMission = Mission::Selling;
		pExt->Convert_UniversalDeploy_InProgress = true;

		return 0x730C10;
	}

	// Unit case, send the undeploy signal only if meets the allcthe requisites
	if (pThis->WhatAmI() == AbstractType::Unit)
	{
		int nObjects = 0;

		// Count objects located in that cell
		for (auto pObject = pThis->GetCell()->FirstObject; pObject; pObject = pObject->NextObject)
		{
			auto const pItem = static_cast<TechnoClass*>(pObject);

			if (pItem && pItem != pThis)
				nObjects++;
		}

		// Abort if the cell is occupied with objects
		if (nObjects > 0)
		{
			CoordStruct loc = CoordStruct::Empty;
			pThis->Scatter(loc, true, false);

			return 0;
		}

		auto const pFoot = static_cast<FootClass*>(pThis);

		// Stop the object
		if (!pThis->IsFallingDown && pThis->CurrentMission != Mission::Guard)
		{
			pFoot->SetDestination(pThis, false);
			pFoot->Locomotor->Stop_Moving();
		}

		// Set the conversion of flying units. If DeployToLand is set is probably a flying unit that only can be deployed in ground
		if (pTypeExt->Convert_DeployToLand)
		{
			auto newCell = MapClass::Instance->GetCellAt(pThis->Location);
			bool isFlying = pThis->GetTechnoType()->MovementZone == MovementZone::Fly;

			// If the cell is occupied abort operation
			//if (pThis->GetHeight() > 0 && pThis->IsCellOccupied(newCell, FacingType::None, -1, nullptr, false) != Move::OK)
			if (isFlying && pThis->IsCellOccupied(newCell, FacingType::None, -1, nullptr, false) != Move::OK)
				return 0;

			pThis->IsFallingDown = true;
		}

		// Set the deployment signal, indicating the process hasn't finished
		pExt->Convert_UniversalDeploy_InProgress = true;

		return 0x730C10;
	}

	return 0;
}

DEFINE_HOOK(0x522510, InfantryClass_UniversalDeploy_DoingDeploy, 0x6)
{
	GET(InfantryClass*, pThis, ECX);

	if (!pThis)
		return 0;

	auto pOldTechno = static_cast<TechnoClass*>(pThis);
	if (!pOldTechno)
		return 0;

	auto const pOldTechnoExt = TechnoExt::ExtMap.Find(pOldTechno);
	if (!pOldTechnoExt)
		return 0;

	if (pOldTechnoExt->Convert_UniversalDeploy_InProgress)
		return 0;

	auto const pOldTechnoTypeExt = TechnoTypeExt::ExtMap.Find(pOldTechno->GetTechnoType());
	if (!pOldTechnoTypeExt)
		return 0;

	// Preparing UniversalDeploy logic
	int nObjects = 0;

	for (auto pObject = pOldTechno->GetCell()->FirstObject; pObject; pObject = pObject->NextObject)
	{
		auto const pItem = static_cast<TechnoClass*>(pObject);

		if (pItem && pItem != pOldTechno)
			nObjects++;
	}

	if (nObjects > 0)
	{
		CoordStruct loc = CoordStruct::Empty;
		pOldTechno->Scatter(loc, true, false);

		return 0;
	}

	if (pOldTechnoTypeExt->Convert_DeployToLand)
	{
		auto newCell = MapClass::Instance->GetCellAt(pThis->Location);

		// If the cell is occupied abort operation
		if (pThis->GetHeight() > 0 &&
			pThis->IsCellOccupied(newCell, FacingType::None, -1, nullptr, false) != Move::OK)
		{
			return 0;
		}

		pThis->IsFallingDown = true;
	}

	pOldTechnoExt->Convert_UniversalDeploy_InProgress = true;

	return 0;
}

DEFINE_HOOK(0x449E6B, BuildingClass_MissionDeconstruction_UniversalDeploy, 0x5)
{
	GET(UnitClass*, pUnit, EBX);
	GET(BuildingClass*, pBuilding, ECX);

	if (!pUnit || !pBuilding)
		return 0;

	auto pTypeExt = TechnoTypeExt::ExtMap.Find(pBuilding->GetTechnoType());
	if (!pTypeExt)
		return 0;

	if (pTypeExt && pTypeExt->Convert_UniversalDeploy.size() == 0)
		return 0;

	pUnit->Limbo();

	auto pOldTechno = static_cast<TechnoClass*>(pBuilding);
	if (!pOldTechno)
		return 0;

	CoordStruct deployLocation = pOldTechno->GetCoords();
	auto deployed = TechnoExt::UniversalConvert(pOldTechno, nullptr);

	if (deployed)
	{
		if (pTypeExt->Convert_AnimFX.isset())
		{
			const auto pAnimType = pTypeExt->Convert_AnimFX.Get();
			if (auto const pAnim = GameCreate<AnimClass>(pAnimType, deployLocation))
			{
				if (pTypeExt->Convert_AnimFX_FollowDeployer)
					pAnim->SetOwnerObject(deployed);

				pAnim->Owner = deployed->Owner;
			}
		}

		pUnit->UnInit();
		return 0x44A1D8;
	}

	// Restoring unit after a failed deploy attempt. I don't know if this can happen.
	if (pTypeExt->Convert_DeployDir >= 0)
	{
		DirStruct desiredFacing;
		desiredFacing.SetDir(static_cast<DirType>(pTypeExt->Convert_DeployDir * 32));
		pUnit->PrimaryFacing.SetCurrent(desiredFacing);
	}
	else
	{
		pUnit->Unlimbo(pUnit->Location, DirType::North);
	}

	return 0;
}

DEFINE_HOOK(0x44725F, BuildingClass_WhatAction_UniversalDeploy_EnableDeployIcon, 0x5)
{
	GET(BuildingClass*, pBuilding, ESI);
	GET_STACK(FootClass*, pFoot, 0x1C);

	if (!pBuilding)
		return 0;

	if (!pFoot)
		return 0;

	auto pFootTechno = static_cast<TechnoClass*>(pFoot);
	if (!pFootTechno)
		return 0;

	if (pFoot->Location == pBuilding->Location)
	{
		auto pTypeExt = TechnoTypeExt::ExtMap.Find(pBuilding->GetTechnoType());
		if (!pTypeExt)
			return 0;

		if (pTypeExt->Convert_UniversalDeploy.size() > 0)
		{
			R->EAX(Action::Self_Deploy);
			return 0x447273;
		}
	}

	return 0;
}

DEFINE_HOOK(0x457DE9, BuildingClass_EvictOccupiers_UniversalDeploy_DontEjectOccupiers, 0xC)
{
	GET(BuildingClass*, pBuilding, ECX);

	if (!pBuilding)
		return 0;

	auto pTypeExt = TechnoTypeExt::ExtMap.Find(pBuilding->Type);
	if (!pTypeExt)
		return 0;

	// Don't eject the infantry if the UniversalDeploy is being used. UniversalDeploy Manages that operation
	if (pTypeExt->Convert_UniversalDeploy.size() > 0)
		return 0x4581DB;

	return 0;
}

DEFINE_HOOK(0x4ABEE9, BuildingClass_MouseLeftRelease_UniversalDeploy_ExecuteDeploy, 0x7)
{
	GET(TechnoClass* const, pTechno, ESI);
	GET(Action const, actionType, EBX);

	if (!pTechno)
		return 0;

	if (!pTechno->IsSelected)
		return 0;

	if (actionType != Action::Self_Deploy)
		return 0;

	auto pExt = TechnoExt::ExtMap.Find(pTechno);
	if (!pExt)
		return 0;

	// Don't allow start again the process if it is still in process
	if (pExt->Convert_UniversalDeploy_InProgress)
		return 0;

	auto pTypeExt = TechnoTypeExt::ExtMap.Find(pTechno->GetTechnoType());
	if (!pTypeExt)
		return 0;

	if (pTypeExt->Convert_UniversalDeploy.size() == 0)
		return 0;

	if (pTechno->WhatAmI() == AbstractType::Building)
	{
		R->EBX(Action::None);
		pTechno->MissionStatus = 0;
		pTechno->CurrentMission = Mission::Selling;
		pExt->Convert_UniversalDeploy_InProgress = true;
	}
	else if (pTechno->WhatAmI() == AbstractType::Unit)
	{
		int nObjects = 0;

		for (auto pObject = pTechno->GetCell()->FirstObject; pObject; pObject = pObject->NextObject)
		{
			auto const pItem = static_cast<TechnoClass*>(pObject);

			if (pItem && pItem != pTechno)
				nObjects++;
		}

		// If the cell is occupied abort operation
		if (nObjects > 0)
		{
			CoordStruct loc = CoordStruct::Empty;
			pTechno->Scatter(loc, true, false);

			return 0;
		}

		auto pFoot = static_cast<FootClass*>(pTechno);

		if (!pTechno->IsFallingDown && pTechno->CurrentMission != Mission::Guard)
		{
			// Can not be converted if the object is moving
			pFoot->SetDestination(pTechno, false);
			pFoot->Locomotor->Stop_Moving();
		}

		// Start the conversion
		auto newCell = MapClass::Instance->GetCellAt(pTechno->Location);

		if (pTypeExt->Convert_DeployToLand)
		{
			// If the cell is occupied abort operation
			if (pTechno->GetHeight() > 0 &&
				pTechno->IsCellOccupied(newCell, FacingType::None, -1, nullptr, false) != Move::OK)
			{
				return 0;
			}

			pTechno->IsFallingDown = true;
			pFoot->ParalysisTimer.Start(15);
		}

		pExt->Convert_UniversalDeploy_InProgress = true;
	}

	return 0;
}

DEFINE_HOOK(0x73B4DA, UnitClass_DrawVoxel_UniversalDeploy_DontRenderObject, 0x6)
{
	enum { Skip = 0x73C5DC };

	GET(UnitClass*, pThis, EBP);

	if (!pThis)
		return 0;

	auto pTechno = static_cast<TechnoClass*>(pThis);
	if (!pTechno)
		return 0;

	auto pExt = TechnoExt::ExtMap.Find(pTechno);
	if (!pExt)
		return 0;

	// VXL units won't draw graphics when deploy
	if (pExt->Convert_UniversalDeploy_MakeInvisible)
		return Skip;

	return 0;
}

// Probably obsolete since I hooked DrawVoxel
/*DEFINE_HOOK(0x5F4CF1, ObjectClass_Render_UniversalDeploy_DontRenderObject, 0x9)
{
	enum { Skip = 0x5F4D09 };
	GET(ObjectClass*, pThis, ECX);
	if (!pThis)
		return 0;
	auto pTechno = static_cast<TechnoClass*>(pThis);
	if (!pTechno)
		return 0;
	auto pExt = TechnoExt::ExtMap.Find(pTechno);
	if (!pExt)
		return 0;
	// Some objects won't draw graphics when deploy
	if (pExt->Convert_UniversalDeploy_MakeInvisible)
		return Skip;
	return 0;
}*/

DEFINE_HOOK(0x73C602, TechnoClass_DrawObject_UniversalDeploy_DontRenderObject, 0x6)
{
	enum { Skip = 0x73CE00 };

	GET(TechnoClass*, pThis, ECX);

	if (!pThis)
		return 0;

	auto pExt = TechnoExt::ExtMap.Find(pThis);
	if (!pExt)
		return 0;

	// SHP units won't draw graphics when deploy
	if (pExt->Convert_UniversalDeploy_MakeInvisible)
		return Skip;

	return 0;
}

DEFINE_HOOK(0x518FBC, InfantryClass_DrawIt_UniversalDeploy_DontRenderObject, 0x6)
{
	enum { Skip = 0x5192B5 };

	GET(InfantryClass*, pThis, EBP);

	if (!pThis)
		return 0;

	auto pTechno = static_cast<TechnoClass*>(pThis);
	if (!pTechno)
		return 0;

	auto pExt = TechnoExt::ExtMap.Find(pTechno);
	if (!pExt)
		return 0;

	// Here enters SHP units when deploy
	if (pExt->Convert_UniversalDeploy_MakeInvisible)
		return Skip;

	return 0;
}

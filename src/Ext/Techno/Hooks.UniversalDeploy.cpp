#include "Body.h"

#include <Ext/Script/Body.h>

DEFINE_HOOK(0x730B8F, DeployCommand_UniversalDeploy, 0x6)
{
	GET(int, index, EDI);

	TechnoClass* pThis = static_cast<TechnoClass*>(ObjectClass::CurrentObjects->GetItem(index));

	if (!pThis || !ScriptExt::IsUnitAvailable(pThis, false))
		return 0;

	auto pExt = TechnoExt::ExtMap.Find(pThis);
	if (!pExt || pExt->Convert_UniversalDeploy_InProgress)
		return 0;

	auto pTypeExt = TechnoTypeExt::ExtMap.Find(pThis->GetTechnoType());
	if (!pTypeExt || pTypeExt->Convert_UniversalDeploy.size() == 0)
		return 0;

	// Building case, send the undeploy signal
	if (pThis->WhatAmI() == AbstractType::Building)
	{
		pThis->MissionStatus = 0;
		pThis->CurrentMission = Mission::Selling;
		pExt->Convert_UniversalDeploy_InProgress = true;

		return 0x730C10;
	}

	// Unit case, send the undeploy signal only if meets the all the requisites
	if (pThis->WhatAmI() == AbstractType::Unit)
	{
		const auto pUnit = abstract_cast<UnitClass*>(pThis);
		const auto pIntoBuildingType = pTypeExt->Convert_UniversalDeploy[0]->WhatAmI() == AbstractType::Building ? abstract_cast<BuildingTypeClass*>(pTypeExt->Convert_UniversalDeploy[0]) : nullptr;
		bool canDeployIntoStructure = pIntoBuildingType ? TechnoExt::CanDeployIntoBuilding(pUnit, false, pIntoBuildingType) : false;
		auto pCell = pThis->GetCell();// MapClass::Instance->GetCellAt(pThis->Location);
		int nObjectsInCell = 0;

		// Count objects located in the desired cell
		for (auto pObject = pCell->FirstObject; pObject; pObject = pObject->NextObject)
		{
			auto const pItem = static_cast<TechnoClass*>(pObject);

			if (pItem && pItem != pThis)
				nObjectsInCell++;
		}

		// Abort if the cell is occupied with objects or can not be deployed into structure. And move the unit to a different nearby location.
		if ((nObjectsInCell > 0 && !pIntoBuildingType) || !canDeployIntoStructure)
		{
			pExt->Convert_UniversalDeploy_InProgress = false;
			pThis->IsFallingDown = false;
			CoordStruct loc = CoordStruct::Empty;
			pThis->Scatter(loc, true, false);
			
			return 0;
		}

		auto const pFoot = static_cast<FootClass*>(pThis);

		// Stop the deployable unit, can not be converted if the object is moving
		if (!pThis->IsFallingDown && pThis->CurrentMission != Mission::Guard)
		{
			//pFoot->SetDestination(pThis, false);
			//pFoot->Locomotor->Stop_Moving();
			// Reset previous command
			pFoot->SetTarget(nullptr);
			pFoot->SetDestination(nullptr, false);
			pFoot->ForceMission(Mission::Guard);
		}

		// If is set DeployToLand then is probably a flying unit that only can be deployed in ground
		if (pTypeExt->Convert_DeployToLand)
		{
			//auto pCell = MapClass::Instance->GetCellAt(pThis->Location);
			bool isFlying = pThis->GetTechnoType()->MovementZone == MovementZone::Fly;

			// If the cell is occupied abort operation
			//if (pThis->GetHeight() > 0 && pThis->IsCellOccupied(pCell, FacingType::None, -1, nullptr, false) != Move::OK)
			if (isFlying && pThis->IsCellOccupied(pCell, FacingType::None, -1, nullptr, false) != Move::OK)
			{
				pExt->Convert_UniversalDeploy_InProgress = false;
				pThis->IsFallingDown = false;
				CoordStruct loc = CoordStruct::Empty;
				pThis->Scatter(loc, true, false);

				return 0;
			}

			// I don't know if is the right action to force air units to land, including units with BalloonHover=yes
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

	if (!pThis || !ScriptExt::IsUnitAvailable(pThis, false))
		return 0;

	auto const pTechno = static_cast<TechnoClass*>(pThis);
	if (!pTechno)
		return 0;

	auto const pExt = TechnoExt::ExtMap.Find(pTechno);
	if (!pExt || pExt->Convert_UniversalDeploy_InProgress)
		return 0;

	auto pTypeExt = TechnoTypeExt::ExtMap.Find(pThis->GetTechnoType());
	if (!pTypeExt || pTypeExt->Convert_UniversalDeploy.size() == 0)
		return 0;

	// Preparing UniversalDeploy logic
	const auto pUnit = abstract_cast<UnitClass*>(pTechno);
	const auto pIntoBuildingType = pTypeExt->Convert_UniversalDeploy[0]->WhatAmI() == AbstractType::Building ? abstract_cast<BuildingTypeClass*>(pTypeExt->Convert_UniversalDeploy[0]) : nullptr;
	bool canDeployIntoStructure = pIntoBuildingType ? TechnoExt::CanDeployIntoBuilding(pUnit, false, pIntoBuildingType) : false;
	auto pCell = pThis->GetCell();// MapClass::Instance->GetCellAt(pThis->Location);
	int nObjectsInCell = 0;

	for (auto pObject = pTechno->GetCell()->FirstObject; pObject; pObject = pObject->NextObject)
	{
		auto const pItem = static_cast<TechnoClass*>(pObject);

		if (pItem && pItem != pTechno)
			nObjectsInCell++;
	}

	// Abort if the cell is occupied with objects or can not be deployed into structure. And move the unit to a different nearby location.
	if ((nObjectsInCell > 0 && !pIntoBuildingType) || !canDeployIntoStructure)
	{
		pExt->Convert_UniversalDeploy_InProgress = false;
		pThis->IsFallingDown = false;
		CoordStruct loc = CoordStruct::Empty;
		pTechno->Scatter(loc, true, false);

		return 0;
	}

	auto const pFoot = static_cast<FootClass*>(pThis);

	// Stop the deployable unit, can not be converted if the object is moving
	if (!pThis->IsFallingDown && pThis->CurrentMission != Mission::Guard)
	{
		//pFoot->SetDestination(pThis, false);
		//pFoot->Locomotor->Stop_Moving();
		// Reset previous command
		pFoot->SetTarget(nullptr);
		pFoot->SetDestination(nullptr, false);
		pFoot->ForceMission(Mission::Guard);
	}

	// If is set DeployToLand then is probably a flying unit that only can be deployed in ground
	if (pTypeExt->Convert_DeployToLand)
	{
		//auto pCell = MapClass::Instance->GetCellAt(pThis->Location);
		bool isFlying = pThis->GetTechnoType()->MovementZone == MovementZone::Fly;

		// If the cell is occupied abort operation
			//if (pThis->GetHeight() > 0 && pThis->IsCellOccupied(pCell, FacingType::None, -1, nullptr, false) != Move::OK)
		if (isFlying && pThis->IsCellOccupied(pCell, FacingType::None, -1, nullptr, false) != Move::OK)
		{
			pExt->Convert_UniversalDeploy_InProgress = false;
			pThis->IsFallingDown = false;
			CoordStruct loc = CoordStruct::Empty;
			pThis->Scatter(loc, true, false);

			return 0;
		}

		// I don't know if is the right action to force air units to land, including units with BalloonHover=yes
		pThis->IsFallingDown = true;
	}

	// Set the deployment signal, indicating the process hasn't finished
	pExt->Convert_UniversalDeploy_InProgress = true;

	return 0;
}

DEFINE_HOOK(0x449E6B, BuildingClass_MissionDeconstruction_UniversalDeploy, 0x5)
{
	GET(UnitClass*, pUnit, EBX);
	GET(BuildingClass*, pBuilding, ECX);

	if (!pUnit || !pBuilding)
		return 0;

	auto pTypeExt = TechnoTypeExt::ExtMap.Find(pBuilding->GetTechnoType());
	if (!pTypeExt || pTypeExt->Convert_UniversalDeploy.size() == 0)
		return 0;

	pUnit->Limbo();

	auto pOldTechno = static_cast<TechnoClass*>(pBuilding);
	if (!pOldTechno)
		return 0;

	CoordStruct deployLocation = pOldTechno->GetCoords();
	auto deployed = TechnoExt::UniversalConvert(pOldTechno);

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

	if (!pBuilding || !pFoot)
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
	if (!pTypeExt || pTypeExt->Convert_UniversalDeploy.size() == 0)
		return 0;

	// Don't eject the infantry if the UniversalDeploy is being used. UniversalDeploy Manages that operation
	return 0x4581DB;
}

DEFINE_HOOK(0x4ABEE9, BuildingClass_MouseLeftRelease_UniversalDeploy_ExecuteDeploy, 0x7)
{
	GET(TechnoClass* const, pTechno, ESI);
	GET(Action const, actionType, EBX);

	if (!pTechno || !pTechno->IsSelected)
		return 0;

	if (actionType != Action::Self_Deploy)
		return 0;

	auto pExt = TechnoExt::ExtMap.Find(pTechno);
	if (!pExt || pExt->Convert_UniversalDeploy_InProgress)
		return 0;

	auto pTypeExt = TechnoTypeExt::ExtMap.Find(pTechno->GetTechnoType());
	if (!pTypeExt || pTypeExt->Convert_UniversalDeploy.size() == 0)
		return 0;

	// Building case, send the undeploy signal
	if (pTechno->WhatAmI() == AbstractType::Building)
	{
		R->EBX(Action::None);
		pTechno->MissionStatus = 0;
		pTechno->CurrentMission = Mission::Selling;
		pExt->Convert_UniversalDeploy_InProgress = true;

		return 0;
	}

	// Unit case, send the undeploy signal only if meets the all the requisites
	if (pTechno->WhatAmI() == AbstractType::Unit)
	{
		const auto pUnit = abstract_cast<UnitClass*>(pTechno);
		const auto pIntoBuildingType = pTypeExt->Convert_UniversalDeploy[0]->WhatAmI() == AbstractType::Building ? abstract_cast<BuildingTypeClass*>(pTypeExt->Convert_UniversalDeploy[0]) : nullptr;
		bool canDeployIntoStructure = pIntoBuildingType ? TechnoExt::CanDeployIntoBuilding(pUnit, false, pIntoBuildingType) : false;
		auto pCell = pTechno->GetCell();// MapClass::Instance->GetCellAt(pTechno->Location);
		int nObjectsInCell = 0;

		// Count objects located in the desired cell
		for (auto pObject = pTechno->GetCell()->FirstObject; pObject; pObject = pObject->NextObject)
		{
			auto const pItem = static_cast<TechnoClass*>(pObject);

			if (pItem && pItem != pTechno)
				nObjectsInCell++;
		}

		// Abort if the cell is occupied with objects or can not be deployed into structure. And move the unit to a different nearby location.
		if ((nObjectsInCell > 0 && !pIntoBuildingType) || !canDeployIntoStructure)
		{
			pExt->Convert_UniversalDeploy_InProgress = false;
			pTechno->IsFallingDown = false;
			CoordStruct loc = CoordStruct::Empty;
			pTechno->Scatter(loc, true, false);

			return 0;
		}

		auto pFoot = static_cast<FootClass*>(pTechno);

		// Stop the deployable unit, can not be converted if the object is moving
		if (!pTechno->IsFallingDown && pTechno->CurrentMission != Mission::Guard)
		{
			//pFoot->SetDestination(pTechno, false);
			//pFoot->Locomotor->Stop_Moving();
			// Reset previous command
			pFoot->SetTarget(nullptr);
			pFoot->SetDestination(nullptr, false);
			pFoot->ForceMission(Mission::Guard);
		}

		if (pTypeExt->Convert_DeployToLand)
		{
			//auto pCell = MapClass::Instance->GetCellAt(pThis->Location);
			bool isFlying = pTechno->GetTechnoType()->MovementZone == MovementZone::Fly;

			// If the cell is occupied abort operation
			//if (pTechno->GetHeight() > 0 && pTechno->IsCellOccupied(pCell, FacingType::None, -1, nullptr, false) != Move::OK)
			if (isFlying && pTechno->IsCellOccupied(pCell, FacingType::None, -1, nullptr, false) != Move::OK)
			{
				pExt->Convert_UniversalDeploy_InProgress = false;
				pTechno->IsFallingDown = false;
				CoordStruct loc = CoordStruct::Empty;
				pTechno->Scatter(loc, true, false);

				return 0;
			}

			// I don't know if is the right action to force air units to land, including units with BalloonHover=yes
			pTechno->IsFallingDown = true;
			//pFoot->ParalysisTimer.Start(15);
		}

		// Set the deployment signal, indicating the process hasn't finished
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

// Make object graphics invisible because they aren't rendered
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

// Make object graphics invisible because they aren't rendered
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

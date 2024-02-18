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
		pExt->Convert_UniversalDeploy_InProgress = true;
		pExt->Convert_UniversalDeploy_IsOriginalDeployer = true;

		return 0x730C10;
	}

	// Unit case, send the undeploy signal only if meets the all the requisites
	if (pThis->WhatAmI() == AbstractType::Unit)
	{
		const auto pUnit = abstract_cast<UnitClass*>(pThis);
		const auto pIntoBuildingType = pTypeExt->Convert_UniversalDeploy.at(0)->WhatAmI() == AbstractType::Building ? abstract_cast<BuildingTypeClass*>(pTypeExt->Convert_UniversalDeploy.at(0)) : nullptr;
		bool canDeployIntoStructure = pIntoBuildingType ? pIntoBuildingType->CanCreateHere(CellClass::Coord2Cell(pThis->GetCoords()), pThis->Owner) : false;//pNewTechnoType ? TechnoExt::CanDeployIntoBuilding(pFoot, false, pBuildingType) : false;
		//bool canDeployIntoStructure = pIntoBuildingType ? TechnoExt::CanDeployIntoBuilding(pUnit, false, pIntoBuildingType) : false;
		auto pCell = pThis->GetCell();
		int nObjectsInCell = 0;

		// Count objects located in the desired cell
		for (auto pObject = pCell->FirstObject; pObject; pObject = pObject->NextObject)
		{
			auto const pItem = static_cast<TechnoClass*>(pObject);

			if (pItem && pItem != pThis)
				nObjectsInCell++;
		}

		// Abort if the cell is occupied with objects or can not be deployed into structure. And move the unit to a different nearby location.
		if ((nObjectsInCell > 0 && !pIntoBuildingType) || pIntoBuildingType && !canDeployIntoStructure)
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
		pExt->Convert_UniversalDeploy_IsOriginalDeployer = true;
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
	
	auto const pNewTechnoType = pOldTechnoTypeExt->Convert_UniversalDeploy.at(0);
	bool pTechnoIsFlying = pOldTechno->GetTechnoType()->MovementZone == MovementZone::Fly;
	auto pCell = pOldTechno->GetCell();
	bool cellIsOnWater = (pCell->LandType == LandType::Water || pCell->LandType == LandType::Beach);
	bool isNewTechnoAmphibious = pNewTechnoType->MovementZone == MovementZone::Amphibious || pNewTechnoType->MovementZone == MovementZone::AmphibiousCrusher ||	pNewTechnoType->MovementZone == MovementZone::AmphibiousDestroyer;
	bool isNewTechnoAircraft = pNewTechnoType->ConsideredAircraft || pNewTechnoType->MovementZone == MovementZone::Fly;

	// If the deployer is over a water cell and the future deployed object doesn't support water cells abort the operation
	if (cellIsOnWater && !(isNewTechnoAmphibious || pNewTechnoType->Naval || isNewTechnoAircraft))
	{
		pOldTechnoExt->Convert_UniversalDeploy_InProgress = false;
		pOldTechno->IsFallingDown = false;
		CoordStruct loc = CoordStruct::Empty;
		pOldTechno->Scatter(loc, true, false);

		return 0;
	}

	if (pNewTechnoType->WhatAmI() == AbstractType::BuildingType)
	{
		auto pFoot = static_cast<FootClass*>(pThis);
		auto pBuildingType = static_cast<BuildingTypeClass*>(pNewTechnoType);
		bool canDeployIntoStructure = pNewTechnoType->CanCreateHere(CellClass::Coord2Cell(pFoot->GetCoords()), pFoot->Owner);//pNewTechnoType ? TechnoExt::CanDeployIntoBuilding(pFoot, false, pBuildingType) : false;
		//bool canDeployIntoStructure = pNewTechnoType ? TechnoExt::CanDeployIntoBuilding(pFoot, false, pBuildingType) : false;

		if (!canDeployIntoStructure)
		{
			pOldTechnoExt->Convert_UniversalDeploy_InProgress = false;
			pOldTechno->IsFallingDown = false;
			CoordStruct loc = CoordStruct::Empty;
			pOldTechno->Scatter(loc, true, false);

			return 0;
		}
	}

	// Preparing UniversalDeploy logic
	bool inf2inf = pNewTechnoType->WhatAmI() == AbstractType::InfantryType;

	int nObjectsInCell = 0;

	for (auto pObject = pOldTechno->GetCell()->FirstObject; pObject; pObject = pObject->NextObject)
	{
		auto const pItem = static_cast<TechnoClass*>(pObject);

		if (pItem && pItem != pOldTechno)
			nObjectsInCell++;
	}

	// It the cell is occupied and isn't a infantry to infantry conversion the operation is aborted
	if (nObjectsInCell > 0 && !inf2inf)
	{
		pOldTechnoExt->Convert_UniversalDeploy_InProgress = false;
		pOldTechno->IsFallingDown = false;
		CoordStruct loc = CoordStruct::Empty;
		pOldTechno->Scatter(loc, true, false);

		return 0;
	}

	if (pOldTechnoTypeExt->Convert_DeployToLand)
	{
		auto newCell = MapClass::Instance->GetCellAt(pThis->Location);

		// If the cell is occupied abort operation
		if (pTechnoIsFlying && pThis->IsCellOccupied(newCell, FacingType::None, -1, nullptr, false) != Move::OK)
		{
			pOldTechnoExt->Convert_UniversalDeploy_InProgress = false;
			pOldTechno->IsFallingDown = false;
			CoordStruct loc = CoordStruct::Empty;
			pOldTechno->Scatter(loc, true, false);

			return 0;
		}

		pThis->IsFallingDown = true;
	}

	pOldTechnoExt->Convert_UniversalDeploy_IsOriginalDeployer = true;
	pOldTechnoExt->Convert_UniversalDeploy_InProgress = true;

	return 0;
}

DEFINE_HOOK(0x449C47, BuildingClass_MissionDeconstruction_UniversalDeploy, 0x6)
{
	GET(BuildingClass*, pBuilding, ECX);

	if (!pBuilding)
		return 0;

	// Check if is the UniversalDeploy or a standard deploy
	auto pTypeExt = TechnoTypeExt::ExtMap.Find(pBuilding->GetTechnoType());
	if (!pTypeExt || pTypeExt->Convert_UniversalDeploy.size() == 0)
		return 0;

	auto pExt = TechnoExt::ExtMap.Find(pBuilding);
	if (!pExt)
		return 0;

	// Start the UniversalDeploy process
	pExt->Convert_UniversalDeploy_InProgress = true;
	pExt->Convert_UniversalDeploy_IsOriginalDeployer = true;
	pBuilding->CurrentMission = Mission::Sleep;

	return 0x449E00;
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

	if (!pTechno || !pTechno->IsSelected || !ScriptExt::IsUnitAvailable(pTechno, false))
		return 0;
	
	if (actionType != Action::Self_Deploy)
		return 0;

	auto pExt = TechnoExt::ExtMap.Find(pTechno);
	if (!pExt || pExt->Convert_UniversalDeploy_InProgress)
		return 0;

	auto pTypeExt = TechnoTypeExt::ExtMap.Find(pTechno->GetTechnoType());
	if (!pTypeExt || pTypeExt->Convert_UniversalDeploy.size() == 0)
		return 0;

	auto const pNewTechnoType = pTypeExt->Convert_UniversalDeploy.at(0);
	bool pTechnoIsFlying = pTechno->GetTechnoType()->MovementZone == MovementZone::Fly;
	auto pCell = pTechno->GetCell();
	bool cellIsOnWater = (pCell->LandType == LandType::Water || pCell->LandType == LandType::Beach);
	bool isNewTechnoAmphibious = pNewTechnoType->MovementZone == MovementZone::Amphibious || pNewTechnoType->MovementZone == MovementZone::AmphibiousCrusher ||
		pNewTechnoType->MovementZone == MovementZone::AmphibiousDestroyer;
	bool isNewTechnoAircraft = pNewTechnoType->ConsideredAircraft || pNewTechnoType->MovementZone == MovementZone::Fly;

	if (pTechno->WhatAmI() == AbstractType::Infantry)
		return 0; //Checked in another hook

	// If the deployer is over a water cell and the future deployed object doesn't support water cells abort the operation
	if (cellIsOnWater && !(isNewTechnoAmphibious || pNewTechnoType->Naval || isNewTechnoAircraft))
	{
		pExt->Convert_UniversalDeploy_InProgress = false;
		pTechno->IsFallingDown = false;
		CoordStruct loc = CoordStruct::Empty;
		pTechno->Scatter(loc, true, false);

		return 0;
	}

	// Building case, send the undeploy signal
	if (pTechno->WhatAmI() == AbstractType::Building)
	{
		R->EBX(Action::None);
		pExt->Convert_UniversalDeploy_InProgress = true;
		pExt->Convert_UniversalDeploy_IsOriginalDeployer = true;

		return 0;
	}

	// vehicles case, send the undeploy signal only if meets the all the requisites
	if (pTechno->WhatAmI() == AbstractType::Unit)
	{
		const auto pUnit = abstract_cast<UnitClass*>(pTechno);
		const auto pIntoBuildingType = pTypeExt->Convert_UniversalDeploy.at(0)->WhatAmI() == AbstractType::Building ? abstract_cast<BuildingTypeClass*>(pTypeExt->Convert_UniversalDeploy.at(0)) : nullptr;
		bool canDeployIntoStructure = pIntoBuildingType ? pIntoBuildingType->CanCreateHere(CellClass::Coord2Cell(pUnit->GetCoords()), pUnit->Owner): false;//pNewTechnoType ? TechnoExt::CanDeployIntoBuilding(pFoot, false, pBuildingType) : false;
		//bool canDeployIntoStructure = pIntoBuildingType ? TechnoExt::CanDeployIntoBuilding(pUnit, false, pIntoBuildingType) : false;
		int nObjectsInCell = 0;

		// Count objects located in the desired cell
		for (auto pObject = pTechno->GetCell()->FirstObject; pObject; pObject = pObject->NextObject)
		{
			auto const pItem = static_cast<TechnoClass*>(pObject);

			if (pItem && pItem != pTechno)
				nObjectsInCell++;
		}

		// Abort if the cell is occupied with objects or can not be deployed into structure. And move the unit to a different nearby location.
		if ((nObjectsInCell > 0 && !pIntoBuildingType) || pIntoBuildingType && !canDeployIntoStructure)
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
			// If the cell is occupied abort operation
			//if (pTechno->GetHeight() > 0 && pTechno->IsCellOccupied(pCell, FacingType::None, -1, nullptr, false) != Move::OK)
			if (pTechnoIsFlying && pTechno->IsCellOccupied(pCell, FacingType::None, -1, nullptr, false) != Move::OK)
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
		pExt->Convert_UniversalDeploy_IsOriginalDeployer = true;
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

// Make object graphics invisible because they aren't rendered
DEFINE_HOOK(0x43D29D, BuildingClass_DrawIt_UniversalDeploy_DontRenderObject, 0xD)
{
	enum { Skip = 0x43D688 };

	GET(BuildingClass*, pThis, ESI);

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
	{
		// Hide building anims
		for (auto pAnim : pThis->Anims)
		{
			if (pAnim)
				pAnim->Invisible = true;
		}

		return Skip;
	}

	// Reset?
	if (pExt->Convert_UniversalDeploy_ForceRedraw)
	{
		for (auto pAnim : pThis->Anims)
		{
			if (pAnim)
				pAnim->Invisible = false;
		}

		pExt->Convert_UniversalDeploy_ForceRedraw = false;
	}

	return 0;
}

// Avoid the sell cursor while the structure is being deployed with the UniversalDeploy
DEFINE_HOOK(0x4494DC, BuildingClass_CanDemolish_UniversalDeploy, 0x6)
{
	GET(BuildingClass*, pThis, ESI);

	auto const pExt = TechnoExt::ExtMap.Find(pThis);
	if (pExt && pExt->Convert_UniversalDeploy_InProgress)
		return 0x449536;

	return 0;
}

// Skip voxel turret drawing while the structure is being deployed with the UniversalDeploy
DEFINE_HOOK(0x43DF6E, BuildingClass_Draw_UniversalDeploy, 0x6)
{
	GET(BuildingClass*, pThis, EBP);

	auto const pExt = TechnoExt::ExtMap.Find(pThis);
	if (pExt && pExt->Convert_UniversalDeploy_InProgress)
		return 0x43E795;

	return 0;
}

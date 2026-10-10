#include <Ext/Building/Body.h>
#include <Ext/House/Body.h>
#include <New/Type/TechTreeTypeClass.h>

bool BuildingTypeExt::CanUpgrade(BuildingClass* pBuilding, BuildingTypeClass* pUpgradeType, HouseClass* pUpgradeOwner)
{
	auto const pUpgradeExt = BuildingTypeExt::TryFetch(pUpgradeType);
	if (pUpgradeExt && EnumFunctions::CanTargetHouse(pUpgradeExt->PowersUp_Owner, pUpgradeOwner, pBuilding->Owner))
	{
		auto const pType = pBuilding->Type;

		// PowersUp.Buildings
		for (auto const pPowerUpBuilding : pUpgradeExt->PowersUp_Buildings)
		{
			if (pPowerUpBuilding == pType)
				return true;
		}
	}

	return false;
}

DEFINE_HOOK(0x452678, BuildingClass_CanUpgrade_UpgradeBuildings, 0x8)
{
	enum { Continue = 0x4526A7, ForbidUpgrade = 0x4526B5 };

	GET(BuildingClass*, pBuilding, ECX);
	GET_STACK(BuildingTypeClass*, pUpgrade, 0xC);
	GET(HouseClass*, pUpgradeOwner, EAX);

	if (BuildingTypeExt::CanUpgrade(pBuilding, pUpgrade, pUpgradeOwner))
	{
		R->EAX(pBuilding->Type->PowersUpToLevel);
		return Continue;
	}

	return ForbidUpgrade;
}

DEFINE_HOOK(0x4408EB, BuildingClass_Unlimbo_UpgradeBuildings, 0xA)
{
	enum { Continue = 0x440912, ForbidUpgrade = 0x440926 };

	GET(BuildingClass*, pBuilding, EDI);
	GET(BuildingClass*, pUpgrade, ESI);

	const auto pType = pUpgrade->Type;

	if (BuildingTypeExt::CanUpgrade(pBuilding, pType, pUpgrade->Owner))
	{
		R->EBX(pType);
		pUpgrade->SetOwningHouse(pBuilding->Owner, false);
		return Continue;
	}

	return ForbidUpgrade;
}

#pragma region UpgradesInteraction

static int BuildLimitRemaining(HouseClass const* const pHouse, BuildingTypeClass const* const pItem)
{
	const int BuildLimit = pItem->BuildLimit;

	if (BuildLimit >= 0)
		return BuildLimit - BuildingTypeExt::GetUpgradesAmount(const_cast<BuildingTypeClass*>(pItem), const_cast<HouseClass*>(pHouse));
	else
		return -BuildLimit - pHouse->CountOwnedEver(pItem);
}

static int CheckBuildLimit(HouseClass const* const pHouse, BuildingTypeClass const* const pItem, bool const includeQueued)
{
	enum { NotReached = 1, ReachedPermanently = -1, ReachedTemporarily = 0 };

	const int BuildLimit = pItem->BuildLimit;
	const int Remaining = BuildLimitRemaining(pHouse, pItem);

	if (BuildLimit >= 0 && Remaining <= 0)
		return (includeQueued && FactoryClass::FindByOwnerAndProduct(pHouse, pItem)) ? NotReached : ReachedPermanently;

	return Remaining > 0 ? NotReached : ReachedTemporarily;

}

DEFINE_HOOK(0x4F8361, HouseClass_CanBuild_UpgradesInteraction, 0x3)
{
	GET(HouseClass const* const, pThis, ECX);
	GET_STACK(TechnoTypeClass const* const, pItem, 0x4);
	GET_STACK(bool const, buildLimitOnly, 0x8);
	GET_STACK(bool const, includeInProduction, 0xC);
	GET(CanBuildResult const, resultOfAres, EAX);

	if (resultOfAres != CanBuildResult::Buildable)
		return 0;

	if (auto const pBuilding = abstract_cast<BuildingTypeClass const* const>(pItem))
	{
		if (BuildingTypeExt::Fetch(pBuilding)->PowersUp_Buildings.size() > 0)
			R->EAX(HouseExt::BuildLimitGroupUpgradeCheck(pThis, pItem, buildLimitOnly, includeInProduction));
	}

	return 0;
}

DEFINE_HOOK(0x4F7877, HouseClass_CanBuild_UpgradesInteraction_WithoutAres, 0x5)
{
	Debug::Log("Hook [HouseClass_CanBuild_UpgradesInteraction] disabled\n");

	Patch::Apply_RAW(0x4F8361, // Disable hook HouseClass_CanBuild_UpgradesInteraction
		{ 0xC2, 0x0C, 0x00, 0x6E, 0x7D }
	);

	Patch::Apply_RAW(0x4F7877, // Disable this hook
		{ 0x53, 0x55, 0x8B, 0xE9, 0x56 }
	);

	return 0;
}

#pragma endregion

#pragma region UpgradeAnimLogic

// Always parse all info for PowerUp anims if building can have even one upgrade, including power settings.
DEFINE_HOOK(0x464749, BuildingTypeClass_ReadINI_PowerUpAnims, 0x6)
{
	enum { SkipGameCode = 0x46492E };

	GET(BuildingTypeClass*, pThis, EBP);

	auto const pTypeExt = BuildingTypeExt::Fetch(pThis);
	auto const pINI = &CCINIClass::INI_Art;

	int index = 1;
	char buffer[0x20];

	pTypeExt->HasPowerUpAnim.clear();

	while (index - 1 < 3)
	{
		auto const animData = &pThis->BuildingAnim[index - 1];

		sprintf_s(buffer, "PowerUp%01dAnim", index);
		pINI->GetString(pThis->ImageFile, buffer, animData->Anim);

		pTypeExt->HasPowerUpAnim.emplace_back(GeneralUtils::IsValidString(animData->Anim));

		sprintf_s(buffer, "PowerUp%01dDamagedAnim", index);
		pINI->GetString(pThis->ImageFile, buffer, animData->Damaged);

		sprintf_s(buffer, "PowerUp%01dLocXX", index);
		animData->Position.X = pINI->ReadInteger(pThis->ImageFile, buffer, animData->Position.X);

		sprintf_s(buffer, "PowerUp%01dLocYY", index);
		animData->Position.Y = pINI->ReadInteger(pThis->ImageFile, buffer, animData->Position.Y);

		sprintf_s(buffer, "PowerUp%01dLocZZ", index);
		animData->ZAdjust = pINI->ReadInteger(pThis->ImageFile, buffer, animData->ZAdjust);

		sprintf_s(buffer, "PowerUp%01dYSort", index);
		animData->YSort = pINI->ReadInteger(pThis->ImageFile, buffer, animData->YSort);

		sprintf_s(buffer, "PowerUp%01dPowered", index);
		animData->Powered = pINI->ReadBool(pThis->ImageFile, buffer, animData->Powered);

		sprintf_s(buffer, "PowerUp%01dPoweredLight", index);
		animData->PoweredLight = pINI->ReadBool(pThis->ImageFile, buffer, animData->PoweredLight);

		sprintf_s(buffer, "PowerUp%01dPoweredEffect", index);
		animData->PoweredEffect = pINI->ReadBool(pThis->ImageFile, buffer, animData->PoweredEffect);

		sprintf_s(buffer, "PowerUp%01dPoweredSpecial", index);
		animData->PoweredSpecial = pINI->ReadBool(pThis->ImageFile, buffer, animData->PoweredSpecial);

		index++;
	}

	return SkipGameCode;
}

DEFINE_HOOK(0x440988, BuildingClass_Unlimbo_UpgradeAnims, 0x7)
{
	enum { SkipGameCode = 0x4409C7 };

	GET(BuildingClass*, pThis, ESI);
	GET(BuildingClass*, pTarget, EDI);

	auto const pTargetExt = BuildingExt::Fetch(pTarget);
	auto const pType = pThis->Type;
	pTargetExt->PoweredUpToLevel = pTarget->UpgradeLevel + 1;
	int animIndex = pTarget->UpgradeLevel;

	if (pType->PowersUpToLevel > 0)
	{
		pTargetExt->PoweredUpToLevel = Math::max(pType->PowersUpToLevel, pTargetExt->PoweredUpToLevel);
		animIndex = pTargetExt->PoweredUpToLevel - 1;
	}

	auto const animData = &pTarget->Type->BuildingAnim[animIndex];

	// Only copy image name to BuildingType anim struct if theres no explicit PowersUpAnim for this level.
	if (!pTargetExt->GetTypeExtData()->HasPowerUpAnim[animIndex])
		strncpy(animData->Anim, pType->ImageFile, 16u);

	return SkipGameCode;
}

DEFINE_HOOK(0x451630, BuildingClass_CreateUpgradeAnims_AnimIndex, 0x7)
{
	enum { SkipGameCode = 0x451638 };

	GET(BuildingClass*, pThis, EBP);

	const int animIndex = BuildingExt::Fetch(pThis)->PoweredUpToLevel - 1;

	if (animIndex)
	{
		R->EAX(animIndex);
		return SkipGameCode;
	}

	return 0;
}

#pragma endregion

#pragma region AI_Upgrade_Target_Selection

void NAKED HouseClass_Powerups_FindUpgradeTarget_Epilogue()
{
	_asm
	{
		retn 8
	}
}

DEFINE_HOOK(0x506B90, HouseClass_Powerups_FindUpgradeTarget, 0x6)
{
	GET(HouseClass*, pHouse, ECX);
	GET_STACK(CellStruct*, pOutCell, 0x4);
	GET_STACK(BuildingTypeClass*, pUpgradeType, 0x8);

	if (!HouseExt::IsAdvancedAIActive(pHouse) || !pUpgradeType || !pOutCell)
		return 0;

	bool isPowerUpgrade = false;
	auto const pUpgradeExt = BuildingTypeExt::TryFetch(pUpgradeType);
	if (pUpgradeExt)
	{
		for (auto const pTargetType : pUpgradeExt->PowersUp_Buildings)
		{
			if (pTargetType && (TechTreeTypeClass::TotalBuildPower.count(pTargetType) > 0 ||
								TechTreeTypeClass::TotalBuildAdvancedPower.count(pTargetType) > 0 ||
								pTargetType->PowerBonus > 0))
			{
				isPowerUpgrade = true;
				break;
			}
		}
	}
	if (!isPowerUpgrade && pUpgradeType->PowersUpBuilding[0] != '\0')
	{
		if (auto const pTargetType = BuildingTypeClass::Find(pUpgradeType->PowersUpBuilding))
		{
			if (TechTreeTypeClass::TotalBuildPower.count(pTargetType) > 0 ||
				TechTreeTypeClass::TotalBuildAdvancedPower.count(pTargetType) > 0 ||
				pTargetType->PowerBonus > 0)
			{
				isPowerUpgrade = true;
			}
		}
	}

	const BuildingClass* pConYard = pHouse->ConYards.Count > 0 ? pHouse->ConYards[0] : nullptr;
	const CellStruct centerCell = pConYard != nullptr ? pConYard->GetMapCoords() : pHouse->Base_Center();

	BuildingClass* pBestBuilding = nullptr;
	double bestDistance = 999999.0;
	int lowestUpgradeLevel = 9999;

	for (const auto pBld : BuildingClass::Array)
	{
		if (!pBld || !pBld->IsAlive || pBld->InLimbo)
			continue;

		if (pBld->CurrentMission == Mission::Selling || pBld->QueuedMission == Mission::Selling)
			continue;

		bool isEligible = false;
		if (pUpgradeExt && BuildingTypeExt::CanUpgrade(pBld, pUpgradeType, pHouse))
			isEligible = true;
		else if (pUpgradeType->PowersUpBuilding[0] != '\0' && _stricmp(pBld->Type->ID, pUpgradeType->PowersUpBuilding) == 0 && pBld->Owner == pHouse)
			isEligible = true;

		if (!isEligible)
			continue;

		const int maxSlots = pBld->Type->Upgrades > 0 ? pBld->Type->Upgrades : pBld->Type->PowersUpToLevel;
		if (pBld->UpgradeLevel >= maxSlots)
			continue;

		const double dist = pBld->GetMapCoords().DistanceFrom(centerCell);

		if (isPowerUpgrade)
		{
			// Only upgrade powerplants within 25.0 cells of the ConYard / base center
			if (dist > 25.0)
				continue;

			// Prefer the closest eligible powerplant to the ConYard
			if (dist < bestDistance - 0.5)
			{
				bestDistance = dist;
				lowestUpgradeLevel = pBld->UpgradeLevel;
				pBestBuilding = pBld;
			}
			else if (std::abs(dist - bestDistance) <= 0.5)
			{
				if (pBld->UpgradeLevel < lowestUpgradeLevel)
				{
					bestDistance = dist;
					lowestUpgradeLevel = pBld->UpgradeLevel;
					pBestBuilding = pBld;
				}
			}
		}
		else
		{
			// Non-power upgrades (e.g. defenses or plugins): lowest upgrade level first, then closest
			if (pBld->UpgradeLevel < lowestUpgradeLevel || (pBld->UpgradeLevel == lowestUpgradeLevel && dist < bestDistance))
			{
				bestDistance = dist;
				lowestUpgradeLevel = pBld->UpgradeLevel;
				pBestBuilding = pBld;
			}
		}
	}

	if (pBestBuilding != nullptr)
	{
		*pOutCell = pBestBuilding->GetMapCoords();

		Debug::Log("AdvAI: House %d placing upgrade %s on %s at (%d,%d) (Dist to ConYard: %.1f, Upgrades: %d/%d)\n",
			pHouse->ArrayIndex, pUpgradeType->ID, pBestBuilding->Type->ID,
			pOutCell->X, pOutCell->Y, pBestBuilding->GetMapCoords().DistanceFrom(centerCell),
			pBestBuilding->UpgradeLevel, pBestBuilding->Type->Upgrades);

		R->EAX(pOutCell);
		return reinterpret_cast<intptr_t>(&HouseClass_Powerups_FindUpgradeTarget_Epilogue);
	}
	else
	{
		// Signal failure so it does not upgrade faraway expansion powerplants
		pOutCell->X = 0;
		pOutCell->Y = 0;
		Debug::Log("AdvAI: House %d found no eligible building within 25 cells for upgrade %s\n",
			pHouse->ArrayIndex, pUpgradeType->ID);

		R->EAX(pOutCell);
		return reinterpret_cast<intptr_t>(&HouseClass_Powerups_FindUpgradeTarget_Epilogue);
	}
}

#pragma endregion

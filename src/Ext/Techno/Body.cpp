#include "Body.h"

#include <AircraftClass.h>
#include <HouseClass.h>
#include <ScenarioClass.h>

#include <Ext/Anim/Body.h>
#include <Ext/Scenario/Body.h>
#include <Ext/House/Body.h>
#include <Ext/Team/Body.h>
#include <Ext/Script/Body.h>
#include <Ext/WarheadType/Body.h>
#include <Ext/WeaponType/Body.h>

#include <Utilities/AresFunctions.h>

TechnoExt::ExtContainer TechnoExt::ExtMap;

TechnoExt::ExtData::~ExtData()
{
	auto const pTypeExt = this->TypeExtData;
	auto const pType = pTypeExt->OwnerObject();
	auto pThis = this->OwnerObject();

	if (pTypeExt->AutoDeath_Behavior.isset())
	{
		auto& vec = ScenarioExt::Global()->AutoDeathObjects;
		vec.erase(std::remove(vec.begin(), vec.end(), this), vec.end());
	}

	if (pThis->WhatAmI() != AbstractType::Aircraft && pThis->WhatAmI() != AbstractType::Building
		&& pType->Ammo > 0 && pTypeExt->ReloadInTransport)
	{
		auto& vec = ScenarioExt::Global()->TransportReloaders;
		vec.erase(std::remove(vec.begin(), vec.end(), this), vec.end());
	}

	AnimExt::InvalidateTechnoPointers(pThis);
}

bool TechnoExt::IsActive(TechnoClass* pThis)
{
	return pThis
		&& pThis->IsAlive
		&& pThis->Health > 0
		&& !pThis->InLimbo
		&& !pThis->TemporalTargetingMe
		&& !pThis->BeingWarpedOut
		&& !pThis->IsUnderEMP()
		;
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

// Syncs Iron Curtain or Force Shield timer to another techno.
void TechnoExt::SyncInvulnerability(TechnoClass* pFrom, TechnoClass* pTo)
{
	if (pFrom->IsIronCurtained())
	{
		const auto pTypeExt = TechnoTypeExt::ExtMap.Find(pFrom->GetTechnoType());
		bool isForceShielded = pFrom->ForceShielded;
		bool allowSyncing = !isForceShielded ? pTypeExt->IronCurtain_KeptOnDeploy.Get(RulesExt::Global()->IronCurtain_KeptOnDeploy) :
			pTypeExt->ForceShield_KeptOnDeploy.Get(RulesExt::Global()->ForceShield_KeptOnDeploy);

		if (allowSyncing)
		{
			pTo->IronCurtain(pFrom->IronCurtainTimer.GetTimeLeft(), pFrom->Owner, false);
			pTo->IronTintStage = pFrom->IronTintStage;
			pTo->ForceShielded = isForceShielded;
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

	auto const pExt = TechnoExt::ExtMap.Find(pThis);

	return pThis->SpeedMultiplier * houseMultiplier * pExt->AE.SpeedMultiplier *
		(pThis->HasAbility(Ability::Faster) ? RulesClass::Instance->VeteranSpeed : 1.0);
}

CoordStruct TechnoExt::PassengerKickOutLocation(TechnoClass* pThis, FootClass* pPassenger, int maxAttempts)
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
bool TechnoExt::CanDeployIntoBuilding(UnitClass* pThis, bool noDeploysIntoDefaultValue)
{
	if (!pThis)
		return false;

	auto const pDeployType = pThis->Type->DeploysInto;

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

// Checks if a structure can deploy into another at its current location. If the building has problems forplacing the new one returns noDeploysIntoDefaultValue (def = false) instead.
// If a building is specified then it will be used by default.
bool TechnoExt::CanDeployIntoBuilding(BuildingClass* pThis, bool noDeploysIntoDefaultValue, BuildingTypeClass* pBuildingType)
{
	if (!pThis)
		return false;

	auto pDeployType = pBuildingType;

	if (!pDeployType)
	{
		auto pBldTypeExt = TechnoTypeExt::ExtMap.Find(pThis->GetTechnoType());
		if (!pBldTypeExt || !pBldTypeExt->Convert_UniversalDeploy.isset())
			return noDeploysIntoDefaultValue;

		pDeployType = static_cast<BuildingTypeClass*>(pBldTypeExt->Convert_UniversalDeploy.Get());
	}

	if (!pDeployType)
		return noDeploysIntoDefaultValue;

	bool canDeploy = true;
	auto mapCoords = CellClass::Coord2Cell(pThis->GetCoords());

	pThis->Mark(MarkType::Up);

	if (!pDeployType->CanCreateHere(mapCoords, pThis->Owner))
		canDeploy = false;

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

/// <summary>
/// Gets whether or not techno has listed AttachEffect types active on it
/// </summary>
/// <param name="attachEffectTypes">Attacheffect types.</param>
/// <param name="requireAll">Whether or not to require all listed types to be present or if only one will satisfy the check.</param>
/// <param name="ignoreSameSource">Ignore AttachEffects that come from set invoker and source.</param>
/// <param name="pInvoker">Invoker Techno used for same source check.</param>
/// <param name="pSource">Source AbstractClass instance used for same source check.</param>
/// <returns>True if techno has active AttachEffects that satisfy the source, false if not.</returns>
bool TechnoExt::ExtData::HasAttachedEffects(std::vector<AttachEffectTypeClass*> attachEffectTypes, bool requireAll, bool ignoreSameSource,
	TechnoClass* pInvoker, AbstractClass* pSource, std::vector<int> const* minCounts, std::vector<int> const* maxCounts) const
{
	unsigned int foundCount = 0;
	unsigned int typeCounter = 1;

	for (auto const& type : attachEffectTypes)
	{
		for (auto const& attachEffect : this->AttachedEffects)
		{
			if (attachEffect->GetType() == type && attachEffect->IsActive())
			{
				if (ignoreSameSource && pInvoker && pSource && attachEffect->IsFromSource(pInvoker, pSource))
					continue;

				unsigned int minSize = minCounts ? minCounts->size() : 0;
				unsigned int maxSize = maxCounts ? maxCounts->size() : 0;

				if (type->Cumulative && (minSize > 0 || maxSize > 0))
				{
					int cumulativeCount = this->GetAttachedEffectCumulativeCount(type, ignoreSameSource, pInvoker, pSource);

					if (minSize > 0)
					{
						if (cumulativeCount < minCounts->at(typeCounter-1 >= minSize ? minSize - 1 : typeCounter - 1))
							continue;
					}
					if (maxSize > 0)
					{
						if (cumulativeCount > maxCounts->at(typeCounter - 1 >= maxSize ? maxSize - 1 : typeCounter - 1))
							continue;
					}
				}

				// Only need to find one match, can stop here.
				if (!requireAll)
					return true;

				foundCount++;
				break;
			}
		}

		// One of the required types was not found, can stop here.
		if (requireAll && foundCount < typeCounter)
			return false;

		typeCounter++;
	}

	if (requireAll && foundCount == attachEffectTypes.size())
		return true;

	return false;
}

/// <summary>
/// Gets how many counts of same cumulative AttachEffect type instance techno has active on it.
/// </summary>
/// <param name="pAttachEffectType">AttachEffect type.</param>
/// <param name="ignoreSameSource">Ignore AttachEffects that come from set invoker and source.</param>
/// <param name="pInvoker">Invoker Techno used for same source check.</param>
/// <param name="pSource">Source AbstractClass instance used for same source check.</param>
/// <returns>Number of active cumulative AttachEffect type instances on the techno. 0 if the AttachEffect type is not cumulative.</returns>
int TechnoExt::ExtData::GetAttachedEffectCumulativeCount(AttachEffectTypeClass* pAttachEffectType, bool ignoreSameSource, TechnoClass* pInvoker, AbstractClass* pSource) const
{
	if (!pAttachEffectType->Cumulative)
		return 0;

	unsigned int foundCount = 0;

	for (auto const& attachEffect : this->AttachedEffects)
	{
		if (attachEffect->GetType() == pAttachEffectType && attachEffect->IsActive())
		{
			if (ignoreSameSource && pInvoker && pSource && attachEffect->IsFromSource(pInvoker, pSource))
				continue;

			foundCount++;
		}
	}

	return foundCount;
}

void TechnoExt::PassengersTransfer(TechnoClass* pFrom, TechnoClass* pTo, bool forceFullTransfer, bool dontCheckInvalidOccupiers)
{
	if (!pFrom || (pFrom == pTo))
		return;

	// Without a valid target this method will be used for ejecting passengers
	bool kickOutAll = !pTo || !forceFullTransfer ? true : false;

	auto pBuildingFrom = abstract_cast<BuildingClass*>(pFrom);
	auto pBuildingTo = pTo ? abstract_cast<BuildingClass*>(pTo) : nullptr;

	DynamicVectorClass<FootClass*> passengersList; // Temporal list

	// The method for extranting the passengers changes if is a building or an unit
	if (pBuildingFrom)
	{
		for (int i = 0; i < pBuildingFrom->Occupants.Count; i++)
		{
			InfantryClass* pPassenger = pBuildingFrom->Occupants.GetItem(i);
			auto pFooter = static_cast<FootClass*>(pPassenger);
			passengersList.AddItem(pFooter);
		}
	}
	else
	{
		while (pFrom->Passengers.NumPassengers > 0)
		{
			FootClass* pPassenger = pFrom->Passengers.RemoveFirstPassenger();
			pPassenger->Transporter = nullptr;
			pPassenger->ShouldEnterOccupiable = false;
			pPassenger->ShouldGarrisonStructure = false;
			pPassenger->InOpenToppedTransport = false;
			pPassenger->QueueMission(Mission::Guard, false);
			passengersList.AddItem(pPassenger);
		}
	}
	
	if (passengersList.Count == 0) // Nothing to transfer
		return;

	const auto pFromType = pFrom->GetTechnoType();
	const auto pToType = pTo ? pTo->GetTechnoType() : nullptr;

	double nToPassengersSizeLimit = pToType ? pToType->SizeLimit : 0;
	int nToPassengersLimit = pToType ? pToType->Passengers : 0;
	int nToPassengers = pToType ? pTo->Passengers.NumPassengers : 0;
	TechnoClass* pTransportReference = pTo ? pTo : pFrom;

	while (passengersList.Count > 0)
	{
		FootClass* pPassenger = nullptr;

		// Note: The insertion order is different in buildings
		int passengerIndex = pBuildingTo ? 0 : passengersList.Count - 1;

		pPassenger = passengersList.GetItem(passengerIndex);
		passengersList.RemoveItem(passengerIndex);
		
		// Garrison infantry in building
		if (pBuildingTo)
		{
			int nOccupants = pBuildingTo->Occupants.Count;
			int maxNumberOccupants = pBuildingTo->Type->MaxNumberOccupants;

			InfantryClass* pInf = abstract_cast<InfantryClass*>(pPassenger);
			bool isOccupier = pInf && (pInf->Type->Occupier || dontCheckInvalidOccupiers) ? true : false;

			// invalid infantry could enter here but is mandatory respect the number of occupants
			if (!kickOutAll && isOccupier && maxNumberOccupants > 0 && nOccupants < maxNumberOccupants)
			{
				pBuildingTo->Occupants.AddItem(pInf);
			}
			else
			{
				// Not enough space inside the garrisonable building, eject the passenger outside
				CoordStruct newLocation = PassengerKickOutLocation(pTransportReference, pPassenger); //pTransportReference->GetCoords();
				auto pCell = MapClass::Instance->TryGetCellAt(CellClass::Coord2Cell(newLocation));
				int bridgeZ = pCell->ContainsBridge() ? CellClass::BridgeHeight : 0;
				int baseHeight = pTransportReference->GetCoords().Z;
				newLocation.Z = Math::max(MapClass::Instance->GetCellFloorHeight(newLocation) + bridgeZ, baseHeight);
				bool inAir = newLocation.Z >= Unsorted::CellHeight * 2; // If the source is flying and must eject passengers they should be parachuted

				pPassenger->LastMapCoords = pCell->MapCoords;

				++Unsorted::IKnowWhatImDoing;

				if (inAir)
					pPassenger->SpawnParachuted(newLocation);
				else
					pPassenger->Unlimbo(newLocation, DirType::North);

				--Unsorted::IKnowWhatImDoing;
			}
		}
		else
		{
			FootClass* pGunner = abstract_cast<FootClass*>(pTo);
			double nToPassengerSize = pPassenger->GetTechnoType()->Size;

			CoordStruct newLocation = PassengerKickOutLocation(pTransportReference, pPassenger); //pTransportReference->GetCoords();
			auto pCell = MapClass::Instance->TryGetCellAt(CellClass::Coord2Cell(newLocation));
			int bridgeZ = pCell->ContainsBridge() ? CellClass::BridgeHeight : 0;
			int baseHeight = pTransportReference->GetCoords().Z;
			newLocation.Z = Math::max(MapClass::Instance->GetCellFloorHeight(newLocation) + bridgeZ, baseHeight);
			bool inAir = newLocation.Z >= Unsorted::CellHeight * 2; // If the source is flying and must eject passengers they should be parachuted

			if (!kickOutAll && nToPassengerSize > 0 && (nToPassengersLimit - nToPassengers - nToPassengerSize >= 0) && nToPassengerSize <= nToPassengersSizeLimit)
			{
				pPassenger->Transporter = pTo;
				nToPassengers += (int)nToPassengerSize;

				if (pToType->OpenTopped)
				{
					pPassenger->SetLocation(pTo->Location);
					pTo->EnteredOpenTopped(pPassenger);
				}
				else if(pToType->Gunner && pGunner)
				{
					pGunner->ReceiveGunner(pPassenger);
				}
				else
				{
					pTo->AddPassenger(pPassenger);
				}
			}
			else
			{
				// Not enough space inside the new transport, eject the passenger
				CellClass* pCell = MapClass::Instance->TryGetCellAt(newLocation);
				pPassenger->LastMapCoords = pCell->MapCoords;

				++Unsorted::IKnowWhatImDoing;

				if (inAir)
					pPassenger->SpawnParachuted(newLocation);
				else
					pPassenger->Unlimbo(newLocation, DirType::North);

				--Unsorted::IKnowWhatImDoing;
			}
		}
	}
}

void TechnoExt::RemoveParasite(TechnoClass* pThis, HouseClass* sourceHouse, WarheadTypeClass* wh)
{
	if (!pThis || !wh)
		return;

	const auto pFoot = abstract_cast<FootClass*>(pThis);
	if (!pFoot)
		return;

	bool isParasiteEatingMe = pFoot->ParasiteEatingMe && pThis->WhatAmI() != AbstractType::Infantry	&& pThis->WhatAmI() != AbstractType::Building;

	// Ignore other cases that aren't useful for this logic
	if (!isParasiteEatingMe)
		return;

	const auto pWHExt = WarheadTypeExt::ExtMap.Find(wh);
	auto parasite = pFoot->ParasiteEatingMe;

	if (!pWHExt || !pWHExt->CanRemoveParasites.Get() || !pWHExt->CanTargetHouse(parasite->Owner, pThis))
		return;

	if (pWHExt->CanRemoveParasites_ReportSound.isset() && pWHExt->CanRemoveParasites_ReportSound.Get() >= 0)
		VocClass::PlayAt(pWHExt->CanRemoveParasites_ReportSound.Get(), parasite->GetCoords());

	// Kill the parasite
	CoordStruct coord = TechnoExt::PassengerKickOutLocation(pThis, parasite, 10);

	auto deleteParasite = [parasite]()
	{
		auto parasiteOwner = parasite->Owner;
		parasite->IsAlive = false;
		parasite->IsOnMap = false;
		parasite->Health = 0;

		parasiteOwner->RegisterLoss(parasite, false);
		parasiteOwner->RemoveTracking(parasite);
		parasite->UnInit();
	};

	if (!pWHExt->CanRemoveParasites_KickOut.Get() || coord == CoordStruct::Empty)
	{
		deleteParasite;
		return;
	}

	// Kick the parasite outside
	pFoot->ParasiteEatingMe = nullptr;

	if (!parasite->Unlimbo(coord, parasite->PrimaryFacing.Current().GetDir()))
	{
		// Failed to kick out the parasite, remove it instead
		deleteParasite;
		return;
	}

	parasite->Target = nullptr;
	int paralysisCountdown = pWHExt->CanRemoveParasites_KickOut_Paralysis.Get() < 0 ? 15 : pWHExt->CanRemoveParasites_KickOut_Paralysis.Get();

	if (paralysisCountdown > 0)
	{
		parasite->ParalysisTimer.Start(paralysisCountdown);
		parasite->RearmTimer.Start(paralysisCountdown);
	}

	if (pWHExt->CanRemoveParasites_KickOut_Anim.isset())
	{
		if (auto const pAnim = GameCreate<AnimClass>(pWHExt->CanRemoveParasites_KickOut_Anim.Get(), parasite->GetCoords()))
		{
			pAnim->Owner = sourceHouse ? sourceHouse : parasite->Owner;
			pAnim->SetOwnerObject(parasite);
		}
	}

	return;
}

bool TechnoExt::IsUnitAvailable(TechnoClass* pTechno, bool checkIfInTransportOrAbsorbed)
{
	if (!pTechno)
		return false;

	bool isAvailable = pTechno->IsAlive && pTechno->Health > 0 && !pTechno->InLimbo && pTechno->IsOnMap;

	if (checkIfInTransportOrAbsorbed)
		isAvailable &= !pTechno->Absorbed && !pTechno->Transporter;

	return isAvailable;
}

bool TechnoExt::UpdateRandomTarget(TechnoClass* pThis)
{
	if (!pThis)
		return false;

	int weaponIndex = pThis->SelectWeapon(pThis->Target);
	auto pWeapon = pThis->GetWeapon(weaponIndex)->WeaponType;
	if (!pWeapon)
		return false;

	const auto pWeaponExt = WeaponTypeExt::ExtMap.Find(pWeapon);
	if (!pWeaponExt || pWeaponExt->RandomTarget <= 0.0)
		return false;

	const auto pExt = TechnoExt::ExtMap.Find(pThis);
	if (!pExt)
		return false;

	if (pThis->CurrentMission == Mission::Move)
	{
		pExt->CurrentRandomTarget = nullptr;
		pExt->OriginalTarget = nullptr;

		return false;
	}

	if (!IsValidTechno(pExt->CurrentRandomTarget))
		pExt->CurrentRandomTarget = nullptr;

	if (pExt->OriginalTarget && !IsValidTechno(static_cast<TechnoClass*>(pExt->OriginalTarget)))
		pExt->OriginalTarget = nullptr;

	if (pThis->Target && !IsValidTechno(static_cast<TechnoClass*>(pThis->Target)))
		pThis->SetTarget(nullptr);

	if (pThis->Target && pExt->CurrentRandomTarget && pThis->CurrentMission == Mission::Guard)
		pThis->ForceMission(Mission::Attack);

	if (pExt->CurrentRandomTarget && pThis->SpawnManager)
		return false;

	if (!pThis->Target && !pExt->OriginalTarget)
		return false;

	if (!pExt->OriginalTarget)
	{
		pExt->CurrentRandomTarget = nullptr;
		pExt->OriginalTarget = nullptr;
	}

	if (pExt->CurrentRandomTarget && pThis->GetCurrentMission() != Mission::Attack)
	{
		pExt->CurrentRandomTarget = nullptr;
		pThis->SetTarget(pExt->OriginalTarget ? pExt->OriginalTarget : nullptr);
		pExt->OriginalTarget = nullptr;

		return false;
	}

	if (!pThis->Target)
		return false;

	if (pThis->DistanceFrom(pExt->OriginalTarget) > pWeapon->Range || pThis->DistanceFrom(pThis->Target) > pWeapon->Range)
	{
		if (pThis->WhatAmI() == AbstractType::Building)
		{
			pThis->SetTarget(nullptr);
			pExt->CurrentRandomTarget = nullptr;
			pExt->OriginalTarget = nullptr;

			return false;
		}

		pThis->SetTarget(pExt->OriginalTarget);
	}

	auto pRandomTarget = FindRandomTarget(pThis);

	if (!pRandomTarget)
		return false;

	pExt->OriginalTarget = !pExt->OriginalTarget ? pThis->Target : pExt->OriginalTarget;
	pExt->CurrentRandomTarget = pRandomTarget;
	pThis->Target = pRandomTarget;

	if (pThis->SpawnManager)
	{
		bool isFirstSpawn = true;

		for (auto pSpawn : pThis->SpawnManager->SpawnedNodes)
		{
			if (!pSpawn->Unit)
				continue;

			TechnoClass* pSpawnTarget = nullptr;

			auto pSpawnExt = TechnoExt::ExtMap.Find(pSpawn->Unit);
			if (!pSpawnExt)
				continue;

			if (isFirstSpawn)
			{
				pSpawnTarget = pExt->CurrentRandomTarget;

				if (pWeaponExt->RandomTarget_Spawners_MultipleTargets)
					isFirstSpawn = false;
			}
			else
			{
				pSpawnTarget = FindRandomTarget(pThis);

				if (!pSpawnTarget)
					pSpawnTarget = static_cast<TechnoClass*>(pExt->OriginalTarget);
			}

			pSpawnExt->CurrentRandomTarget = pSpawnTarget;
			pSpawnExt->OriginalTarget = pExt->OriginalTarget;
		}
	}

	return true;
}

// Find all valid targets in the attacker's area and from the valid ones picks one randomly
TechnoClass* TechnoExt::FindRandomTarget(TechnoClass* pThis)
{
	TechnoClass* selection = nullptr;

	if (!pThis || !pThis->Target)
		return selection;

	int weaponIndex = pThis->SelectWeapon(pThis->Target);
	auto pWeapon = pThis->GetWeapon(weaponIndex)->WeaponType;
	if (!pWeapon)
		return selection;

	auto const pExt = TechnoExt::ExtMap.Find(pThis);
	auto const pWeaponExt = WeaponTypeExt::ExtMap.Find(pWeapon);
	if (!pExt || !pWeaponExt || pWeaponExt->RandomTarget <= 0.0)
		return selection;

	auto const originalTarget = static_cast<TechnoClass*>(pExt->OriginalTarget ? pExt->OriginalTarget : pThis->Target);
	if (!IsValidTechno(originalTarget))
		return selection;

	int retargetProbability = std::min((int)round(pWeaponExt->RandomTarget * 100), 100);
	int dice = ScenarioClass::Instance->Random.RandomRanged(1, 100);

	if (retargetProbability < dice)
		return originalTarget;

	bool friendlyFire = pThis->Owner->IsAlliedWith(originalTarget->Owner);
	auto pThisType = pThis->GetTechnoType();
	int minimumRange = pWeapon->MinimumRange;
	int range = pWeapon->Range;
	int airRange = pWeapon->Range + pThisType->AirRangeBonus;
	std::vector<TechnoClass*> candidates;
	candidates.push_back(originalTarget);

	// Looking for all valid targeting candidates
	for (auto pCandidate : *TechnoClass::Array)
	{
		auto const pCandidateType = pCandidate->GetTechnoType();

		if (pCandidate == pThis
			|| pCandidate == originalTarget
			|| !IsValidTechno(pCandidate)
			|| pCandidateType->Immune
			|| !EnumFunctions::IsTechnoEligible(pCandidate, pWeaponExt->CanTarget, true)
			|| (!pWeapon->Projectile->AA && pCandidate->IsInAir())
			|| (!pWeapon->Projectile->AG && !pCandidate->IsInAir())
			|| (!friendlyFire && (pThis->Owner->IsAlliedWith(pCandidate) || ScriptExt::IsUnitMindControlledFriendly(pThis->Owner, pCandidate)))
			|| pCandidate->TemporalTargetingMe
			|| pCandidate->BeingWarpedOut
			|| (pCandidateType->Underwater && pCandidateType->NavalTargeting == NavalTargetingType::Underwater_Never)
			|| (pCandidateType->Naval && pCandidateType->NavalTargeting == NavalTargetingType::Naval_None)
			|| (pCandidate->CloakState == CloakState::Cloaked && !pCandidateType->Naval)
			|| (pCandidate->InWhichLayer() == Layer::Underground))
		{
			continue;
		}

		int distanceFromAttacker = pThis->DistanceFrom(pCandidate);
		if (distanceFromAttacker < minimumRange)
			continue;

		if (pWeapon->OmniFire)
		{
			if (pCandidate->IsInAir())
				range = airRange;

			if (distanceFromAttacker <= range)
				candidates.push_back(pCandidate);
		}
		else
		{
			int distanceFromOriginalTargetToCandidate = pCandidate->DistanceFrom(pThis->Target);
			int distanceFromOriginalTarget = pThis->DistanceFrom(pThis->Target);

			if (pCandidate->IsInAir())
				range = airRange;

			if (distanceFromAttacker <= range && distanceFromOriginalTargetToCandidate <= distanceFromOriginalTarget)
				candidates.push_back(pCandidate);
		}
	}

	if (candidates.size() == 0)
		return selection;

	// Pick one new target from the list of targets inside the weapon range
	dice = ScenarioClass::Instance->Random.RandomRanged(0, candidates.size() - 1);
	selection = candidates.at(dice);

	return selection;
}

bool TechnoExt::IsValidTechno(TechnoClass* pTechno)
{
	if (!pTechno)
		return false;

	bool isValid = !pTechno->Dirty
		&& TechnoExt::IsUnitAvailable(pTechno, true)
		&& pTechno->Owner
		&& (pTechno->WhatAmI() == AbstractType::Infantry
			|| pTechno->WhatAmI() == AbstractType::Unit
			|| pTechno->WhatAmI() == AbstractType::Building
			|| pTechno->WhatAmI() == AbstractType::Aircraft);

	return isValid;
}

void TechnoExt::SendStopTarNav(TechnoClass* pThis)
{
	auto pFoot = abstract_cast<FootClass*>(pThis);

	EventExt event;
	event.Type = EventTypeExt::SyncStopTarNav;
	event.HouseIndex = (char)HouseClass::CurrentPlayer->ArrayIndex;
	event.Frame = Unsorted::CurrentFrame;

	event.AddEvent();
}

void TechnoExt::HandleStopTarNav(EventExt* event)
{
	int technoUniqueID = event->SyncStopTarNav.TechnoUniqueID;

	for (auto pTechno : *TechnoClass::Array)
	{
		if (pTechno && pTechno->UniqueID == technoUniqueID)
		{
			auto const pExt = TechnoExt::ExtMap.Find(pTechno);

			pExt->CurrentRandomTarget = nullptr;
			pExt->OriginalTarget = nullptr;
			pTechno->ForceMission(Mission::Guard);

			auto pFoot = abstract_cast<FootClass*>(pTechno);

			if (pFoot->Locomotor->Is_Moving_Now())
				pFoot->StopMoving();

			break;
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
		.Process(this->AttachedEffects)
		.Process(this->AE)
		.Process(this->ReceiveDamage)
		.Process(this->PassengerDeletionTimer)
		.Process(this->CurrentShieldType)
		.Process(this->LastWarpDistance)
		.Process(this->AutoDeathTimer)
		.Process(this->MindControlRingAnimType)
		.Process(this->Strafe_BombsDroppedThisRound)
		.Process(this->CurrentAircraftWeaponIndex)
		.Process(this->IsInTunnel)
		.Process(this->IsBurrowed)
		.Process(this->HasBeenPlacedOnMap)
		.Process(this->DeployFireTimer)
		.Process(this->ForceFullRearmDelay)
		.Process(this->CanCloakDuringRearm)
		.Process(this->WHAnimRemainingCreationInterval)
		.Process(this->FiringObstacleCell)
		.Process(this->OriginalPassengerOwner)
		.Process(this->HasRemainingWarpInDelay)
		.Process(this->LastWarpInDelay)
		.Process(this->IsBeingChronoSphered)
		.Process(this->WebbyDurationCountDown)
		.Process(this->WebbyDurationTimer)
		.Process(this->WebbyAnim)
		.Process(this->WebbyLastTarget)
		.Process(this->WebbyLastMission)
		.Process(this->OriginalTarget)
		.Process(this->ResetRandomTarget)
		.Process(this->CurrentRandomTarget)
		.Process(this->Convert_UniversalDeploy_DeployAnim)
		.Process(this->Convert_UniversalDeploy_InProgress)
		.Process(this->Convert_UniversalDeploy_MakeInvisible)
		.Process(this->Convert_UniversalDeploy_TemporalTechno)
		.Process(this->Convert_UniversalDeploy_IsOriginalDeployer)
		.Process(this->Convert_UniversalDeploy_RememberTarget)
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


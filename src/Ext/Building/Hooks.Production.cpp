#include "Body.h"

#include <Ext/House/Body.h>
#include <FactoryClass.h>
#include <Misc/NeuralAI/NeuralAI.h>
#include <Utilities/Debug.h>

DEFINE_HOOK(0x4401BB, BuildingClass_AI_PickWithFreeDocks, 0x6)
{
	enum { SkipGameCode = 0x4401D2 };

	GET(BuildingClass*, pBuilding, ESI);

	if (pBuilding->IsUnderEMP())
		return SkipGameCode;

	auto const pOwner = pBuilding->Owner;
	const int index = pOwner->ProducingAircraftTypeIndex;
	auto const pType = index >= 0 ? AircraftTypeClass::Array.GetItem(index) : nullptr;

	if (RulesExt::Global()->AllowParallelAIQueues && !RulesExt::Global()->ForbidParallelAIQueues_Aircraft && (!pType || !TechnoTypeExt::Fetch(pType)->ForbidParallelAIQueues))
		return 0;

	if (pOwner->IsCurrentPlayer() || pOwner->IsNeutral() || NeuralAI::IsEnabledForHouse(pOwner))
		return 0;

	if (pBuilding->Type->Factory == AbstractType::AircraftType)
	{
		if (pBuilding->Factory
			&& !BuildingExt::HasFreeDocks(pBuilding))
		{
			if (auto const pBldExt = BuildingExt::TryFetch(pBuilding))
				pBldExt->UpdatePrimaryFactoryAI();
		}
	}

	return 0;
}

// The factory a Neural AI house builds a queue at: the primary one among its working factories of
// the kind (the first of them is made primary when none is).
namespace NeuralFactoryQueue
{
	// When the designated factory of a queue last asked the engine for its next object and when it
	// last got through. Per house and queue; rebuilt as the match runs, never saved.
	struct QueueState
	{
		int LastAsk { -1 };
		int LastGrant { -1 };
		int RefusedSince { -1 };
		int ReleaseFrame { -1 };
	};

	QueueState States[32][5];

	// Frames the designated factory is refused before the queue is freed, and before it is freed
	// again.
	constexpr int RefusedFramesBeforeRelease = 30;
	constexpr int ReleaseRetryFrames = 450;

	QueueState* GetState(const HouseClass* pOwner, AbstractType kind, bool naval)
	{
		if (!pOwner || pOwner->ArrayIndex < 0 || pOwner->ArrayIndex >= 32)
			return nullptr;

		auto& states = States[pOwner->ArrayIndex];

		switch (kind)
		{
		case AbstractType::BuildingType:
			return &states[0];
		case AbstractType::UnitType:
			return &states[naval ? 2 : 1];
		case AbstractType::InfantryType:
			return &states[3];
		case AbstractType::AircraftType:
			return &states[4];
		default:
			return nullptr;
		}
	}

	BuildingClass* FindDesignated(HouseClass* pOwner, AbstractType kind, bool naval)
	{
		// Only an aircraft that must land on a dock needs an air base with a free one.
		const int aircraftIndex = pOwner->ProducingAircraftTypeIndex;
		auto const pAircraftType = aircraftIndex >= 0 ? AircraftTypeClass::Array.GetItem(aircraftIndex) : nullptr;
		const bool needsFreeDock = !pAircraftType || pAircraftType->AirportBound;
		BuildingClass* pFallback = nullptr;

		for (auto const pBld : pOwner->Buildings)
		{
			if (!pBld || !pBld->IsAlive || pBld->InLimbo || !pBld->Type || pBld->Type->Factory != kind || pBld->Type->Naval != naval
				|| pBld->IsUnderEMP() || pBld->Deactivated || pBld->CurrentMission == Mission::Selling
				|| pBld->QueuedMission == Mission::Selling || pBld->CurrentMission == Mission::Construction)
			{
				continue;
			}

			if (kind == AbstractType::AircraftType && needsFreeDock && !BuildingExt::HasFreeDocks(pBld))
				continue;

			if (pBld->IsPrimaryFactory)
				return pBld;

			if (!pFallback)
				pFallback = pBld;
		}

		if (pFallback)
			pFallback->IsPrimaryFactory = true;

		return pFallback;
	}

	// The type the house wants next from this queue.
	TechnoTypeClass* NextType(HouseClass* pOwner, AbstractType kind, bool naval)
	{
		int index = -1;

		switch (kind)
		{
		case AbstractType::BuildingType:
			index = pOwner->ProducingBuildingTypeIndex;
			return index >= 0 ? BuildingTypeClass::Array.GetItem(index) : nullptr;
		case AbstractType::UnitType:
			index = naval ? HouseExt::Fetch(pOwner)->ProducingNavalUnitTypeIndex : pOwner->ProducingUnitTypeIndex;
			return index >= 0 ? UnitTypeClass::Array.GetItem(index) : nullptr;
		case AbstractType::InfantryType:
			index = pOwner->ProducingInfantryTypeIndex;
			return index >= 0 ? InfantryTypeClass::Array.GetItem(index) : nullptr;
		case AbstractType::AircraftType:
			index = pOwner->ProducingAircraftTypeIndex;
			return index >= 0 ? AircraftTypeClass::Array.GetItem(index) : nullptr;
		default:
			return nullptr;
		}
	}
}

// Without parallel AI queues one factory per queue builds, and every extension that enforces it
// keeps its own owner of the queue: the first factory that asks for an object while the queue has
// none, dropped only when a unit leaves a factory or a production is abandoned. A Neural AI house
// chooses the factory itself (its primary one), so only that one may ask: any other would take
// the queue and be refused its object by the rule below, and nothing would be built. An owner
// kept from before the choice changed is dropped the same way the engine drops it: a production
// of the wanted type is started and abandoned at once.
DEFINE_HOOK(0x450292, BuildingClass_Update_Factory_NeuralDesignated, 0x6)
{
	enum { Skip = 0x4503CA };

	GET(BuildingClass*, pThis, ESI);

	auto const pOwner = pThis->Owner;

	if (pThis->Factory || !pOwner || !pOwner->Production || RulesExt::Global()->AllowParallelAIQueues
		|| !NeuralAI::IsEnabledForHouse(pOwner))
	{
		return 0;
	}

	const AbstractType kind = pThis->Type->Factory;
	const bool naval = pThis->Type->Naval;
	auto const pQueue = NeuralFactoryQueue::GetState(pOwner, kind, naval);
	auto const pDesignated = NeuralFactoryQueue::FindDesignated(pOwner, kind, naval);

	if (!pQueue || !pDesignated)
		return 0;

	if (pThis != pDesignated)
		return Skip;

	const int now = Unsorted::CurrentFrame;

	if (now < pQueue->LastAsk)
		*pQueue = NeuralFactoryQueue::QueueState {};

	const bool refused = pQueue->LastAsk >= 0 && pQueue->LastGrant < pQueue->LastAsk;

	if (!refused)
		pQueue->RefusedSince = -1;
	else if (pQueue->RefusedSince < 0)
		pQueue->RefusedSince = pQueue->LastAsk;

	pQueue->LastAsk = now;

	const bool releaseDue = refused && now - pQueue->RefusedSince >= NeuralFactoryQueue::RefusedFramesBeforeRelease
		&& (pQueue->ReleaseFrame < 0 || now - pQueue->ReleaseFrame >= NeuralFactoryQueue::ReleaseRetryFrames);

	if (releaseDue)
	{
		if (auto const pType = NeuralFactoryQueue::NextType(pOwner, kind, naval))
		{
			pQueue->ReleaseFrame = now;
			auto const pRelease = GameCreate<FactoryClass>();

			if (pRelease->DemandProduction(pType, pOwner, false))
				pRelease->AbandonProduction();

			GameDelete(pRelease);
			Debug::Log("[NeuralAI] House %d production: queue of %s freed for the designated %s, refused for %d frames, frame %d.\n",
				pOwner->ArrayIndex, pType->ID, pThis->Type->ID, now - pQueue->RefusedSince, now);
		}
	}

	return 0;
}

DEFINE_HOOK(0x4502F4, BuildingClass_Update_Factory_Phobos, 0x6)
{
	enum { SkipFactory = 0x4503CA };

	GET(BuildingClass*, pThis, ESI);
	HouseClass* const pOwner = pThis->Owner;

	// Neural AI: one primary factory per kind does the production (the one marked primary, else the
	// first usable one, which becomes primary); the others skip their update.
	if (pOwner && pOwner->Production && NeuralAI::IsEnabledForHouse(pOwner))
	{
		auto const pOwnerExt = HouseExt::Fetch(pOwner);
		auto const pFactory = pThis->Type->Factory;
		const bool naval = pThis->Type->Naval;
		BuildingClass** currFactory = nullptr;

		switch (pFactory)
		{
		case AbstractType::BuildingType:
			currFactory = &pOwnerExt->Factory_BuildingType;
			break;
		case AbstractType::UnitType:
			currFactory = naval ? &pOwnerExt->Factory_NavyType : &pOwnerExt->Factory_VehicleType;
			break;
		case AbstractType::InfantryType:
			currFactory = &pOwnerExt->Factory_InfantryType;
			break;
		case AbstractType::AircraftType:
			currFactory = &pOwnerExt->Factory_AircraftType;
			break;
		default:
			break;
		}

		auto const pDesignatedPrimary = NeuralFactoryQueue::FindDesignated(pOwner, pFactory, naval);

		if (pDesignatedPrimary)
		{
			if (currFactory)
				*currFactory = pDesignatedPrimary;

			// With parallel AI queues every factory of the kind builds, by the rules below.
			if (!RulesExt::Global()->AllowParallelAIQueues)
			{
				if (pThis != pDesignatedPrimary)
					return SkipFactory;

				if (auto const pQueue = NeuralFactoryQueue::GetState(pOwner, pFactory, naval))
					pQueue->LastGrant = Unsorted::CurrentFrame;

				return 0;
			}
		}
	}

	if (pOwner->Production && RulesExt::Global()->AllowParallelAIQueues)
	{
		auto const pOwnerExt = HouseExt::Fetch(pOwner);
		auto const pFactory = pThis->Type->Factory;
		const bool naval = pThis->Type->Naval;
		BuildingClass** currFactory = nullptr;

		switch (pFactory)
		{
		case AbstractType::BuildingType:
			currFactory = &pOwnerExt->Factory_BuildingType;
			break;
		case AbstractType::UnitType:
			currFactory = naval ? &pOwnerExt->Factory_NavyType : &pOwnerExt->Factory_VehicleType;
			break;
		case AbstractType::InfantryType:
			currFactory = &pOwnerExt->Factory_InfantryType;
			break;
		case AbstractType::AircraftType:
			currFactory = &pOwnerExt->Factory_AircraftType;
			break;
		default:
			break;
		}

		if (!*currFactory)
		{
			*currFactory = pThis;
			return 0;
		}
		else if (*currFactory != pThis)
		{
			enum { Skip = 0x4503CA };


			TechnoTypeClass* pType = nullptr;
			int index = -1;

			switch (pFactory)
			{
			case AbstractType::BuildingType:
				if (RulesExt::Global()->ForbidParallelAIQueues_Building)
					return Skip;

				index = pOwner->ProducingBuildingTypeIndex;
				pType = index >= 0 ? BuildingTypeClass::Array.GetItem(index) : nullptr;
				break;
			case AbstractType::InfantryType:
				if (RulesExt::Global()->ForbidParallelAIQueues_Infantry)
					return Skip;

				index = pOwner->ProducingInfantryTypeIndex;
				pType = index >= 0 ? InfantryTypeClass::Array.GetItem(index) : nullptr;
				break;
			case AbstractType::AircraftType:
				if (RulesExt::Global()->ForbidParallelAIQueues_Aircraft)
					return Skip;

				index = pOwner->ProducingAircraftTypeIndex;
				pType = index >= 0 ? AircraftTypeClass::Array.GetItem(index) : nullptr;
				break;
			case AbstractType::UnitType:
				if (naval ? RulesExt::Global()->ForbidParallelAIQueues_Navy : RulesExt::Global()->ForbidParallelAIQueues_Vehicle)
					return Skip;

				index = naval ? HouseExt::Fetch(pOwner)->ProducingNavalUnitTypeIndex : pOwner->ProducingUnitTypeIndex;
				pType = index >= 0 ? UnitTypeClass::Array.GetItem(index) : nullptr;
				break;
			default:
				break;
			}

			if (pType && TechnoTypeExt::Fetch(pType)->ForbidParallelAIQueues)
				return Skip;
		}
	}

	return 0;
}

//const byte old_empty_log[] = { 0xC3 };
DEFINE_JUMP(CALL, 0x4CA016, 0x4CA19F); // randomly chosen 0xC3

DEFINE_HOOK(0x4CA07A, FactoryClass_AbandonProduction_Phobos, 0x8)
{
	GET(FactoryClass*, pFactory, ESI);
	GET_STACK(DWORD const, calledby, 0x18);

	auto const pTechno = pFactory->Object;

	if (calledby < 0x7F0000) // Replace the old log with this to figure out where keeps flushing the stream
	{
		Debug::LogGame("(%08x) : %s is abandoning production of %s[%s]\n"
			, calledby - 5
			, pFactory->Owner->PlainName
			, pTechno->GetType()->Name
			, pTechno->get_ID());
	}

	if (!RulesExt::Global()->AllowParallelAIQueues)
		return 0;

	auto const pOwnerExt = HouseExt::Fetch(pFactory->Owner);
	auto const pTypeExt = TechnoExt::Fetch(pTechno)->TypeExtData;
	auto const pType = pTypeExt->OwnerObject();
	const bool forbid = pTypeExt->ForbidParallelAIQueues;

	switch (pTechno->WhatAmI())
	{
	case AbstractType::Building:
		if (RulesExt::Global()->ForbidParallelAIQueues_Building || forbid)
			pOwnerExt->Factory_BuildingType = nullptr;
		break;
	case AbstractType::Unit:
		if (!pType->Naval)
		{
			if (RulesExt::Global()->ForbidParallelAIQueues_Vehicle || forbid)
				pOwnerExt->Factory_VehicleType = nullptr;
		}
		else
		{
			if (RulesExt::Global()->ForbidParallelAIQueues_Navy || forbid)
				pOwnerExt->Factory_NavyType = nullptr;
		}
		break;
	case AbstractType::Infantry:
		if (RulesExt::Global()->ForbidParallelAIQueues_Infantry || forbid)
			pOwnerExt->Factory_InfantryType = nullptr;
		break;
	case AbstractType::Aircraft:
		if (RulesExt::Global()->ForbidParallelAIQueues_Aircraft || forbid)
			pOwnerExt->Factory_AircraftType = nullptr;
		break;
	default:
		break;
	}

	return 0;
}

DEFINE_HOOK(0x444119, BuildingClass_KickOutUnit_UnitType_Phobos, 0x6)
{
	GET(UnitClass*, pUnit, EDI);
	GET(BuildingClass*, pFactory, ESI);

	auto const pHouseExt = HouseExt::Fetch(pFactory->Owner);

	if (pUnit->Type->Naval && pHouseExt->Factory_NavyType == pFactory)
		pHouseExt->Factory_NavyType = nullptr;
	else if (pHouseExt->Factory_VehicleType == pFactory)
		pHouseExt->Factory_VehicleType = nullptr;

	return 0;
}

DEFINE_HOOK(0x444131, BuildingClass_KickOutUnit_InfantryType_Phobos, 0x6)
{
	GET(BuildingClass*, pFactory, ESI);

	auto const pHouseExt = HouseExt::Fetch(pFactory->Owner);

	if (pHouseExt->Factory_InfantryType == pFactory)
		pHouseExt->Factory_InfantryType = nullptr;

	return 0;
}

DEFINE_HOOK(0x44531F, BuildingClass_KickOutUnit_BuildingType_Phobos, 0xA)
{
	GET(BuildingClass*, pFactory, ESI);

	auto const pHouseExt = HouseExt::Fetch(pFactory->Owner);

	if (pHouseExt->Factory_BuildingType == pFactory)
		pHouseExt->Factory_BuildingType = nullptr;

	return 0;
}

DEFINE_HOOK(0x443CCA, BuildingClass_KickOutUnit_AircraftType_Phobos, 0xA)
{
	GET(BuildingClass*, pFactory, ESI);

	auto const pHouseExt = HouseExt::Fetch(pFactory->Owner);

	if (pHouseExt->Factory_AircraftType == pFactory)
		pHouseExt->Factory_AircraftType = nullptr;

	return 0;
}

DEFINE_HOOK(0x4449FB, BuildingClass_KickOutUnit_CloningVats, 0x8)
{
	enum { SkipGameCode = 0x444A53 };

	GET(BuildingClass*, pFactory, ESI);
	GET(TechnoTypeClass*, pProductionType, EAX);
	const auto pOwner = pFactory->Owner;
	auto info = std::make_pair(pProductionType, pOwner);

	for (const auto pVat : pOwner->CloningVats)
		BuildingExt::KickOutClone(info, 0, pVat);

	return SkipGameCode;
}

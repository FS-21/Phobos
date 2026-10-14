#include "Body.h"

#include <Ext/TechnoType/Body.h>
#include <Ext/UnitType/Body.h>
#include <JumpjetLocomotionClass.h>
#include <VocClass.h>

DEFINE_HOOK(0x73FD67, UnitClass_WhatAction_Carryall, 0x5)
{
	enum { ReturnAction = 0x73FD72, ReturnDefault = 0 };

	GET(UnitClass*, pThis, ESI);
	GET(ObjectClass*, pTarget, EDI);

	if (!pThis || !pThis->IsAlive)
		return ReturnDefault;

	auto const pTypeExt = TechnoTypeExt::Fetch(pThis->Type);
	if (!pTypeExt || !pTypeExt->Carryall)
		return ReturnDefault;

	auto const pExt = UnitExt::Fetch(pThis);
	if (!pExt)
		return ReturnDefault;

	if (pTarget == pThis)
	{
		if (pExt->CarryallCargo)
		{
			if (pExt->CarryallState == JumpjetCarryallState::Carrying && pExt->CarryallCanDropAt(pThis->GetCell()))
			{
				R->EAX(static_cast<int>(Action::Self_Deploy));
				return ReturnAction;
			}

			R->EAX(static_cast<int>(Action::NoDeploy));
			return ReturnAction;
		}

		if (pExt->CarryallState != JumpjetCarryallState::Idle)
		{
			R->EAX(static_cast<int>(Action::NoDeploy));
			return ReturnAction;
		}
	}

	if (pTarget && pTarget != pThis && !pExt->CarryallCargo && pExt->CarryallState != JumpjetCarryallState::Carrying)
	{
		if (auto const pTechno = abstract_cast<TechnoClass*>(pTarget))
		{
			if (pExt->CarryallCanLift(pTechno))
			{
				R->EAX(static_cast<int>(Action::Tote));
				return ReturnAction;
			}
		}
	}

	return ReturnDefault;
}

DEFINE_HOOK(0x7388A4, UnitClass_Active_Click_With_Tote, 0x6)
{
	enum { ReturnHandled = 0x738907, ReturnDefault = 0 };

	GET(UnitClass*, pThis, ESI);
	GET(ObjectClass*, pTarget, EDI);
	GET(Action, action, EAX);

	if (action != Action::Tote || !pThis || !pThis->IsAlive)
		return ReturnDefault;

	auto const pTypeExt = TechnoTypeExt::Fetch(pThis->Type);
	if (!pTypeExt || !pTypeExt->Carryall)
		return ReturnDefault;

	auto const pExt = UnitExt::Fetch(pThis);
	pExt->CarryallTarget = pTarget;
	pExt->CarryallState = JumpjetCarryallState::MovingToPickup;

	auto const pSendCommand = reinterpret_cast<void(__thiscall*)(TechnoClass*, int, int, AbstractClass*, int)>(0x6FFBE0);
	pSendCommand(pThis, 2, 0, pTarget, 0);

	R->AL(1);
	return ReturnHandled;
}

DEFINE_HOOK(0x700D5D, TechnoClass_CanDeploy_Carryall, 0x5)
{
	enum { ReturnValue = 0x700E3F, ReturnDefault = 0 };

	GET(TechnoClass*, pThis, ESI);

	auto const pUnit = abstract_cast<UnitClass*>(pThis);
	if (!pUnit)
		return ReturnDefault;

	auto const pTypeExt = TechnoTypeExt::Fetch(pUnit->Type);
	if (!pTypeExt || !pTypeExt->Carryall)
		return ReturnDefault;

	auto const pExt = UnitExt::Fetch(pUnit);
	if (!pExt)
		return ReturnDefault;

	if (pExt->CarryallCargo)
	{
		const bool canDrop = (pExt->CarryallState == JumpjetCarryallState::Carrying)
			&& pExt->CarryallCanDropAt(pUnit->GetCell());
		R->AL(canDrop ? 1 : 0);
		return ReturnValue;
	}

	if (pExt->CarryallState != JumpjetCarryallState::Idle)
	{
		R->AL(0);
		return ReturnValue;
	}

	return ReturnDefault;
}

DEFINE_HOOK(0x7393C9, UnitClass_TryToDeploy_Carryall, 0x9)
{
	enum { ReturnSuccess = 0x739A64, ReturnFail = 0x739AAE, ReturnDefault = 0 };

	GET(UnitClass*, pThis, EBP);

	if (!pThis || !pThis->IsAlive)
		return ReturnDefault;

	auto const pTypeExt = TechnoTypeExt::Fetch(pThis->Type);
	if (!pTypeExt || !pTypeExt->Carryall)
		return ReturnDefault;

	auto const pExt = UnitExt::Fetch(pThis);
	if (!pExt)
		return ReturnDefault;

	if (pExt->CarryallCargo)
	{
		if (pExt->CarryallState == JumpjetCarryallState::Carrying && pExt->CarryallCanDropAt(pThis->GetCell()))
		{
			pExt->CarryallState = JumpjetCarryallState::DescendingToDrop;
			pThis->QueueMission(Mission::Guard, true);

			return ReturnSuccess;
		}

		return ReturnFail;
	}

	if (pExt->CarryallState != JumpjetCarryallState::Idle)
		return ReturnFail;

	return ReturnDefault;
}

DEFINE_HOOK(0x741983, UnitClass_AssignDestination_Carryall, 0x6)
{
	enum { ReturnDone = 0x74317A, ReturnDefault = 0 };

	GET(UnitClass*, pThis, EBP);
	GET(AbstractClass*, pTarget, EBX);

	if (!pThis || !pThis->IsAlive)
		return ReturnDefault;

	auto const pTypeExt = TechnoTypeExt::Fetch(pThis->Type);
	if (!pTypeExt || !pTypeExt->Carryall)
		return ReturnDefault;

	auto const pExt = UnitExt::Fetch(pThis);
	if (!pExt)
		return ReturnDefault;

	if (pTarget == pThis)
	{
		if (pExt->CarryallCargo)
		{
			if (pExt->CarryallCanDropAt(pThis->GetCell()))
			{
				pExt->CarryallState = JumpjetCarryallState::DescendingToDrop;
				pThis->QueueMission(Mission::Guard, true);
			}

			return ReturnDone;
		}

		if (pExt->CarryallState != JumpjetCarryallState::Idle)
			return ReturnDone;
	}

	if (pTarget && pTarget != pThis && !pExt->CarryallCargo)
	{
		if (auto const pTechno = abstract_cast<TechnoClass*>(pTarget))
		{
			if (pExt->CarryallCanLift(pTechno))
			{
				pExt->CarryallTarget = pTarget;
				pExt->CarryallState = JumpjetCarryallState::MovingToPickup;

				return ReturnDefault;
			}
		}
	}

	pExt->CarryallDropOnArrival = false;

	if (pTarget != nullptr && pTarget != pExt->CarryallTarget)
	{
		auto const pTargetCell = abstract_cast<CellClass*>(pTarget);
		auto const pTargetTechno = abstract_cast<TechnoClass*>(pExt->CarryallTarget);
		const bool isTargetUnitCell = (pTargetCell && pTargetTechno && pTargetCell == pTargetTechno->GetCell());

		if (!isTargetUnitCell)
		{
			if (pExt->CarryallState == JumpjetCarryallState::MovingToPickup ||
				pExt->CarryallState == JumpjetCarryallState::AligningInAir ||
				pExt->CarryallState == JumpjetCarryallState::DescendingToPickup)
			{
				pExt->CarryallTarget = nullptr;
				pExt->CarryallAlignTimer = 0;
				pExt->CarryallState = JumpjetCarryallState::AscendingAfterDrop;
			}
			else if (pExt->CarryallState == JumpjetCarryallState::DescendingToDrop)
			{
				pExt->CarryallState = JumpjetCarryallState::AscendingWithCargo;
			}
		}
	}

	return ReturnDefault;
}

static bool ShouldEmulateBalloonHover(FootClass* pFoot, TechnoTypeClass* pType)
{
	if (pType && pType->BalloonHover)
		return true;

	if (reinterpret_cast<uintptr_t>(pFoot) >= 0x10000)
	{
		if (auto const pUnit = abstract_cast<UnitClass*>(pFoot))
		{
			auto const pExt = UnitExt::Fetch(pUnit);
			if (pExt && (pExt->CarryallCargo || pExt->CarryallState != JumpjetCarryallState::Idle))
				return true;
		}
	}

	return false;
}

DEFINE_HOOK(0x54B26B, JumpjetLocomotionClass_Move_BalloonHover, 0x6)
{
	enum { ReturnNext = 0x54B271 };

	GET(TechnoTypeClass*, pType, ECX);
	GET(FootClass*, pFoot, EAX);

	R->AL(ShouldEmulateBalloonHover(pFoot, pType) ? 1 : 0);
	return ReturnNext;
}

DEFINE_HOOK(0x54B3F2, JumpjetLocomotionClass_Process_BalloonHover, 0x6)
{
	enum { ReturnNext = 0x54B3F8 };

	GET(TechnoTypeClass*, pType, EAX);
	auto const pFoot = reinterpret_cast<uintptr_t>(R->ESI<void*>()) >= 0x10000
		? *reinterpret_cast<FootClass**>(R->ESI<uintptr_t>() + 8)
		: nullptr;

	R->CL(ShouldEmulateBalloonHover(pFoot, pType) ? 1 : 0);
	return ReturnNext;
}

DEFINE_HOOK(0x54BCEE, JumpjetLocomotionClass_StateAscending_BalloonHover, 0x6)
{
	enum { ReturnNext = 0x54BCF4 };

	GET(TechnoTypeClass*, pType, EAX);
	auto const pFoot = reinterpret_cast<uintptr_t>(R->ESI<void*>()) >= 0x10000
		? *reinterpret_cast<FootClass**>(R->ESI<uintptr_t>() + 0xC)
		: nullptr;

	R->CL(ShouldEmulateBalloonHover(pFoot, pType) ? 1 : 0);
	return ReturnNext;
}

DEFINE_HOOK(0x54BE6D, JumpjetLocomotionClass_StateMove_BalloonHover, 0x6)
{
	enum { ReturnNext = 0x54BE73 };

	GET(TechnoTypeClass*, pType, EAX);
	auto const pFoot = reinterpret_cast<uintptr_t>(R->ESI<void*>()) >= 0x10000
		? *reinterpret_cast<FootClass**>(R->ESI<uintptr_t>() + 0xC)
		: nullptr;

	R->CL(ShouldEmulateBalloonHover(pFoot, pType) ? 1 : 0);
	return ReturnNext;
}

DEFINE_HOOK(0x54C0EB, JumpjetLocomotionClass_StateHovering_BalloonHover, 0x6)
{
	enum { ReturnNext = 0x54C0F1 };

	GET(TechnoTypeClass*, pType, EAX);
	auto const pFoot = reinterpret_cast<uintptr_t>(R->ESI<void*>()) >= 0x10000
		? *reinterpret_cast<FootClass**>(R->ESI<uintptr_t>() + 0xC)
		: nullptr;

	R->AL(ShouldEmulateBalloonHover(pFoot, pType) ? 1 : 0);
	return ReturnNext;
}

static bool InCarryallCargoDraw = false;

DEFINE_HOOK(0x73CEEA, UnitClass_DrawIt_Cargo, 0x6)
{
	GET(UnitClass*, pThis, ESI);
	GET_STACK(Point2D*, pLocation, 0x54);
	GET_STACK(RectangleStruct*, pBounds, 0x58);

	if (InCarryallCargoDraw || !pThis || !pLocation || !pBounds)
		return 0;

	if (reinterpret_cast<uintptr_t>(pLocation) < 0x10000 || reinterpret_cast<uintptr_t>(pBounds) < 0x10000)
		return 0;

	auto const pExt = UnitExt::Fetch(pThis);
	if (!pExt || !pExt->CarryallCargo || !pExt->CarryallCargo->IsAlive)
		return 0;

	auto const pCargo = pExt->CarryallCargo;
	InCarryallCargoDraw = true;

	const auto pCell = pThis->GetCell();
	const int floorZ = pCell ? pCell->GetCoordsWithBridge().Z : pThis->Location.Z;
	const int cargoZ = std::max(floorZ, pThis->Location.Z - 32);

	pCargo->Location.X = pThis->Location.X;
	pCargo->Location.Y = pThis->Location.Y;
	pCargo->Location.Z = cargoZ;
	pCargo->PrimaryFacing = pThis->PrimaryFacing;
	pCargo->SecondaryFacing = pThis->PrimaryFacing;
	pCargo->AngleRotatedSideways = pThis->AngleRotatedSideways;
	pCargo->AngleRotatedForwards = pThis->AngleRotatedForwards;
	pCargo->PitchAngle = pThis->PitchAngle;
	pCargo->IsOnCarryall = true;
	pCargo->Transporter = pThis;

	Point2D cargoScreenLoc = *pLocation;
	cargoScreenLoc.Y += TacticalClass::AdjustForZ(pThis->Location.Z - cargoZ) - 10;

	pCargo->DrawIt(&cargoScreenLoc, pBounds);

	InCarryallCargoDraw = false;

	return 0;
}

DEFINE_HOOK(0x73D63B, UnitClass_MissionUnload_Carryall, 0x6)
{
	enum { ReturnDelay = 0x73D730, ReturnDefault = 0 };

	GET(UnitClass*, pThis, ESI);

	if (!pThis)
		return ReturnDefault;

	auto const pTypeExt = TechnoTypeExt::Fetch(pThis->Type);
	if (!pTypeExt || !pTypeExt->Carryall)
		return ReturnDefault;

	auto const pExt = UnitExt::Fetch(pThis);
	if (!pExt)
		return ReturnDefault;

	if (pExt->CarryallCargo)
	{
		if (pExt->CarryallState == JumpjetCarryallState::Carrying && pExt->CarryallCanDropAt(pThis->GetCell()))
		{
			pExt->CarryallState = JumpjetCarryallState::DescendingToDrop;
			pThis->QueueMission(Mission::Guard, true);
		}

		return ReturnDelay;
	}

	if (pExt->CarryallState != JumpjetCarryallState::Idle)
		return ReturnDelay;

	return ReturnDefault;
}

DEFINE_HOOK(0x4D9290, FootClass_MissionEnter_Carryall, 0x6)
{
	enum { ReturnDone = 0x4D9499, ReturnDefault = 0 };

	GET(FootClass*, pThis, ECX);

	auto const pUnit = abstract_cast<UnitClass*>(pThis);
	if (!pUnit)
		return ReturnDefault;

	auto const pTypeExt = TechnoTypeExt::Fetch(pUnit->Type);
	if (!pTypeExt || !pTypeExt->Carryall)
		return ReturnDefault;

	auto const pExt = UnitExt::Fetch(pUnit);
	if (pExt && pExt->CarryallState != JumpjetCarryallState::Idle)
	{
		R->EAX(15);
		return ReturnDone;
	}

	return ReturnDefault;
}

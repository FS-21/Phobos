#include <AircraftClass.h>
#include <UnitClass.h>
#include <MapClass.h>

#include <Ext/TechnoType/Body.h>
#include <Ext/Unit/Body.h>

DEFINE_HOOK(0x419B1D, AircraftClass_CellSeemsOk_Carryall, 0x8)
{
	enum { ReturnInvalid = 0x419C60, ReturnDefault = 0x419B25, ReturnZero = 0x419C6B };

	GET(AircraftClass*, pThis, ESI);
	GET(CellStruct*, pCellCoord, EBP);

	if (R->AL() == 0)
		return ReturnZero;

	if (!pThis || !pCellCoord)
		return ReturnDefault;

	auto const pTypeExt = TechnoTypeExt::Fetch(pThis->Type);
	const bool isCarryall = pThis->Type->Carryall || (pTypeExt && pTypeExt->Carryall);

	if (isCarryall && pThis->Passengers.FirstPassenger)
	{
		auto const pCargo = abstract_cast<UnitClass*>(pThis->Passengers.FirstPassenger);
		auto const pCell = MapClass::Instance.TryGetCellAt(*pCellCoord);

		if (pCargo && (!pCell || !UnitExt::CanCargoLandOnCell(pCargo, pCell)))
			return ReturnInvalid;
	}

	return ReturnDefault;
}

DEFINE_HOOK(0x41463E, AircraftClass_DrawIt_CarryallCargoSync, 0x7)
{
	GET(TechnoClass*, pCargo, EAX);
	GET(AircraftClass*, pThis, EBP);
	GET_STACK(RectangleStruct*, pBounds, 0xD8);

	if (pCargo && pThis)
	{
		auto const pCell = pThis->GetCell();
		const int floorZ = pCell ? pCell->GetCoordsWithBridge().Z : MapClass::Instance.GetCellFloorHeight(pThis->Location);
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
	}

	R->ECX(pBounds);
	return 0;
}

DEFINE_HOOK(0x41464F, AircraftClass_DrawIt_CallDrawAgain_SuppressShadow, 0x6)
{
	GET(TechnoClass*, pCargo, ECX);
	GET(AircraftClass*, pCarrier, EBP);
	GET_STACK(Point2D*, pLocation, 0x0);
	GET_STACK(RectangleStruct*, pBounds, 0x4);

	if (pCargo)
	{
		UnitExt::DrawCarryallSHPGenericShadow(pCarrier ? pCarrier : pCargo->Transporter, pCargo, pBounds, pLocation);

		auto const pCargoType = pCargo->GetTechnoType();
		const bool origNoShadow = pCargoType ? pCargoType->NoShadow : false;
		const bool origUnderwater = pCargoType ? pCargoType->Underwater : false;
		if (pCargoType)
		{
			pCargoType->NoShadow = true;
			pCargoType->Underwater = true;
		}

		const bool origDrawShadow = std::exchange(Game::bDrawShadow, false);
		pCargo->DrawIt(pLocation, pBounds);
		Game::bDrawShadow = origDrawShadow;

		if (pCargoType)
		{
			pCargoType->NoShadow = origNoShadow;
			pCargoType->Underwater = origUnderwater;
		}
	}

	R->ESP(R->ESP() + 8);
	return 0x414655;
}

DEFINE_HOOK(0x4151AA, AircraftClass_AI_CarryallCargoSync, 0x6)
{
	GET(TechnoClass*, pCargo, ECX);
	auto const pThis = reinterpret_cast<AircraftClass*>(R->EBX<char*>() - 0x114);

	if (pCargo && pThis)
	{
		auto const pCell = pThis->GetCell();
		const int floorZ = pCell ? pCell->GetCoordsWithBridge().Z : MapClass::Instance.GetCellFloorHeight(pThis->Location);
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
	}

	return 0;
}

DEFINE_HOOK(0x416C66, AircraftClass_DropOffCargo_ResetAngles, 0x7)
{
	GET(UnitClass*, pCargo, ESI);

	if (pCargo)
	{
		pCargo->IsOnCarryall = false;
		pCargo->AngleRotatedSideways = 0.0f;
		pCargo->AngleRotatedForwards = 0.0f;
		pCargo->PitchAngle = 0.0f;
		pCargo->Transporter = nullptr;
	}

	return 0;
}


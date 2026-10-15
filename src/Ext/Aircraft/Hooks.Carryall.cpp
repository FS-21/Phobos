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

DEFINE_HOOK(0x4151AA, AircraftClass_AI_CarryallCargoSync, 0x6)
{
	GET(TechnoClass*, pCargo, ECX);
	auto const pThis = reinterpret_cast<AircraftClass*>(R->EBX<char*>() - 0x114);

	if (pCargo && pThis)
	{
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
		pCargo->AngleRotatedSideways = 0.0f;
		pCargo->AngleRotatedForwards = 0.0f;
		pCargo->PitchAngle = 0.0f;
		pCargo->Transporter = nullptr;
	}

	return 0;
}


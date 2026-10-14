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
	enum { ReturnNext = 0x4151B0 };

	GET(TechnoClass*, pCargo, ECX);
	GET(AircraftClass*, pThis, ESI);
	GET_STACK(CoordStruct*, pCoords, 0x0);

	if (pCargo)
	{
		pCargo->SetLocation(*pCoords);

		if (pThis)
		{
			pCargo->AngleRotatedSideways = pThis->AngleRotatedSideways;
			pCargo->AngleRotatedForwards = pThis->AngleRotatedForwards;
			pCargo->PitchAngle = pThis->PitchAngle;
		}
	}

	return ReturnNext;
}

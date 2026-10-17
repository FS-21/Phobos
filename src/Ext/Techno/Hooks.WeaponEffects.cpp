#include "Body.h"

#include <Ext/ParticleSystemType/Body.h>
#include <Ext/WeaponType/Body.h>
#include <Ext/Bullet/Body.h>

#include <unordered_map>

// Contains hooks that fix weapon graphical effects like lasers, railguns, electric bolts, beams and waves not interacting
// correctly with obstacles between firer and target, as well as railgun / railgun particles being cut off by elevation.

namespace FireAtTemp
{
	BulletClass* FireBullet = nullptr;
	WeaponTypeClass* FireWeapon = nullptr;
	CoordStruct OriginalTargetCoords;
	CellClass* pObstacleCell = nullptr;
	AbstractClass* pOriginalTarget = nullptr;
	AbstractClass* pWaveOwnerTarget = nullptr;
	bool IgnoreTargetForWaveAmbientDamage = false;
	bool CheckedOtherWaves = false;
	bool RestoredWaveEC19 = false;
	std::unordered_map<const WaveClass*, WeaponTypeClass*> WaveWeaponMap;

	WeaponTypeClass* GetWaveWeapon(const WaveClass* pWave)
	{
		if (!pWave)
			return nullptr;

		auto const it = WaveWeaponMap.find(pWave);
		if (it != WaveWeaponMap.end())
			return it->second;

		if (FireWeapon)
			return FireWeapon;

		if (auto const pOwner = pWave->Owner)
		{
			for (int i = 0; i < 2; ++i)
			{
				if (auto const pWeaponStruct = pOwner->GetWeapon(i))
				{
					if (auto const pWeapon = pWeaponStruct->WeaponType)
					{
						if ((pWave->Type == WaveType::Sonic && pWeapon->IsSonic)
							|| (pWave->Type == WaveType::Magnetron && pWeapon->IsMagBeam))
						{
							return pWeapon;
						}
					}
				}
			}

			if (auto const pWeaponStruct = pOwner->GetWeapon(0))
				return pWeaponStruct->WeaponType;
		}

		return nullptr;
	}

	bool ShouldDelayWaveROF(const TechnoClass* pTechno, const WeaponTypeClass* pWeapon)
	{
		if (!pTechno || !pTechno->Wave)
			return false;

		if (auto const pWaveWeapon = GetWaveWeapon(pTechno->Wave))
		{
			if (auto const pWaveWeaponExt = WeaponTypeExt::TryFetch(pWaveWeapon))
			{
				if (pWaveWeaponExt->Wave_DelayROF.Get(pWaveWeaponExt->IsClassicSonic))
					return true;
			}
		}

		if (auto const pWeaponExt = WeaponTypeExt::TryFetch(pWeapon))
		{
			if (pWeaponExt->Wave_DelayROF.Get(pWeaponExt->IsClassicSonic))
				return true;
		}

		return false;
	}

	void ApplyWaveGeometry(const WaveClass* pWave, bool isMagnetic)
	{
		if (!pWave)
			return;

		const auto type = pWave->Type;
		const int typeIndex = static_cast<int>(type);

		if (typeIndex < 0 || typeIndex > 3)
			return;

		int defaultWidth = 100;
		int defaultCap = 30;

		switch (type)
		{
		case WaveType::Sonic:
			defaultWidth = 100;
			defaultCap = 30;
			break;
		case WaveType::BigLaser:
			defaultWidth = 44;
			defaultCap = 34;
			break;
		case WaveType::Laser:
			defaultWidth = 34;
			defaultCap = 27;
			break;
		case WaveType::Magnetron:
			defaultWidth = 50;
			defaultCap = isMagnetic ? 0 : 30;
			break;
		default:
			break;
		}

		auto const pWeapon = GetWaveWeapon(pWave);
		auto const pWeaponExt = WeaponTypeExt::TryFetch(pWeapon);

		const int baseWidth = (pWeaponExt && pWeaponExt->Wave_Width.isset())
			? pWeaponExt->Wave_Width.Get() : defaultWidth;
		int startWidth = (pWeaponExt && pWeaponExt->Wave_StartWidth.isset())
			? pWeaponExt->Wave_StartWidth.Get() : baseWidth;
		int endWidth = (pWeaponExt && pWeaponExt->Wave_EndWidth.isset())
			? pWeaponExt->Wave_EndWidth.Get() : baseWidth;
		int capLength = (pWeaponExt && pWeaponExt->Wave_CapLength.isset())
			? pWeaponExt->Wave_CapLength.Get() : defaultCap;

		if (type == WaveType::Sonic || type == WaveType::Magnetron)
		{
			startWidth = std::clamp(startWidth, 1, 149);
			endWidth = std::clamp(endWidth, 1, 149);
		}
		else
		{
			startWidth = std::max(1, startWidth);
			endWidth = std::max(1, endWidth);
		}

		capLength = std::max(0, capLength);

		auto* const pVectors = reinterpret_cast<Vector3D<float>*>((isMagnetic ? 0xB45CA0u : 0xB45DA8u) + typeIndex * 48u);
		const float cap = static_cast<float>(capLength);
		const float startW = static_cast<float>(startWidth);
		const float endW = static_cast<float>(endWidth);

		pVectors[0] = { -cap, -endW, 0.0f };
		pVectors[1] = { -cap, endW, 0.0f };
		pVectors[2] = { cap, -startW, 0.0f };
		pVectors[3] = { cap, startW, 0.0f };
	}
}

DEFINE_HOOK(0x6FF08B, TechnoClass_Fire_RecordBullet, 0x6)
{
	GET(BulletClass*, pBullet, EBX);
	FireAtTemp::FireBullet = pBullet;
	return 0;
}

// Set obstacle cell.
DEFINE_HOOK(0x6FF15F, TechnoClass_FireAt_ObstacleCellSet, 0x6)
{
	GET(TechnoClass*, pThis, ESI);
	GET(WeaponTypeClass*, pWeapon, EBX);
	GET_BASE(AbstractClass*, pTarget, 0x8);
	LEA_STACK(CoordStruct*, pSourceCoords, STACK_OFFSET(0xB0, -0x6C));

	const auto pBuilding = abstract_cast<BuildingClass*, true>(pTarget);
	const auto coords = pBuilding ? pBuilding->GetTargetCoords() : pTarget->GetCenterCoords();

	// This is set to a temp variable as well, as accessing it everywhere needed from TechnoExt would be more complicated.
	FireAtTemp::pObstacleCell = TrajectoryHelper::FindFirstObstacle(*pSourceCoords, coords, pWeapon->Projectile, pThis->Owner);
	TechnoExt::Fetch(pThis)->FiringObstacleCell = FireAtTemp::pObstacleCell;

	return 0;
}

// Apply obstacle logic to fire & spark particle system targets.
DEFINE_HOOK_AGAIN(0x6FF1D7, TechnoClass_FireAt_SparkFireTargetSet, 0x5)
DEFINE_HOOK(0x6FF189, TechnoClass_FireAt_SparkFireTargetSet, 0x5)
{
	if (FireAtTemp::pObstacleCell)
	{
		if (R->Origin() == 0x6FF189)
			R->ECX(FireAtTemp::pObstacleCell);
		else
			R->EDX(FireAtTemp::pObstacleCell);
	}

	return 0;
}

// Fix fire particle target coordinates potentially differing from actual target coords.
DEFINE_HOOK(0x62FA41, ParticleSystemClass_FireAI_TargetCoords, 0x6)
{
	enum { SkipGameCode = 0x62FBAF };

	GET(ParticleSystemClass*, pThis, ESI);

	auto const pTypeExt = ParticleSystemTypeExt::Fetch(pThis->Type);

	if (!pTypeExt->AdjustTargetCoordsOnRotation)
		return SkipGameCode;

	return 0;
}

// Fix fire particles being disallowed from going upwards.
DEFINE_HOOK(0x62D685, ParticleSystemClass_Fire_Coords, 0x5)
{
	enum { SkipGameCode = 0x62D6B7 };

	// Game checks if MapClass::GetCellFloorHeight() for currentCoords is larger than for previousCoords and sets the flags on ParticleClass to
	// remove it if so. Below is an attempt to create a smarter check that allows upwards movement and does not needlessly collide with elevation
	// but removes particles when colliding with flat ground. It doesn't work perfectly and covering all edge-cases is difficult or impossible so
	// preference was to disable it. Keeping the code here commented out, however.

	/*
	GET(ParticleClass*, pThis, ESI);
	REF_STACK(CoordStruct, currentCoords, STACK_OFFSET(0x24, -0x18));
	REF_STACK(CoordStruct, previousCoords, STACK_OFFSET(0x24, -0xC));

	auto const sourceLocation = pThis->ParticleSystem ? pThis->ParticleSystem->Location : CoordStruct { INT_MAX, INT_MAX, INT_MAX };
	auto const pCell = MapClass::Instance.TryGetCellAt(currentCoords);
	int cellFloor = MapClass::Instance.GetCellFloorHeight(currentCoords);
	bool downwardTrajectory = currentCoords.Z < previousCoords.Z;
	bool isBelowSource = cellFloor < sourceLocation.Z - Unsorted::LevelHeight * 2;
	bool isRamp = pCell ? pCell->SlopeIndex : false;

	if (!isRamp && isBelowSource && downwardTrajectory && currentCoords.Z < cellFloor)
	{
		pThis->unknown_12D = 1;
		pThis->unknown_131 = 1;
	}
	*/

	return SkipGameCode;
}

// Fix railgun target coordinates potentially differing from actual target coords.
DEFINE_HOOK(0x70C6B5, TechnoClass_Railgun_TargetCoords, 0x5)
{
	GET(AbstractClass*, pTarget, EBX);
	GET(CoordStruct*, pCoords, EAX);

	if (const auto pBuilding = abstract_cast<BuildingClass*, true>(pTarget))
		*pCoords = pBuilding->GetTargetCoords();
	else if (const auto pCell = abstract_cast<CellClass*, true>(pTarget))
		*pCoords = pCell->GetCoordsWithBridge();

	return 0;
}

// Cut railgun logic off at obstacle coordinates.
DEFINE_HOOK(0x70CA64, TechnoClass_Railgun_Obstacles, 0x5)
{
	enum { Continue = 0x70CA79, Stop = 0x70CAD8 };

	REF_STACK(CoordStruct const, coords, STACK_OFFSET(0xC0, -0x80));

	const auto pCell = MapClass::Instance.GetCellAt(coords);

	if (pCell == FireAtTemp::pObstacleCell)
		return Stop;

	return Continue;
}

DEFINE_HOOK(0x70C862, TechnoClass_Railgun_AmbientDamageIgnoreTarget1, 0x5)
{
	enum { IgnoreTarget = 0x70CA59 };

	GET_BASE(WeaponTypeClass*, pWeapon, 0x14);

	if (WeaponTypeExt::Fetch(pWeapon)->AmbientDamage_IgnoreTarget.Get(RulesExt::Global()->AmbientDamage_IgnoreTarget))
		return IgnoreTarget;

	return 0;
}

DEFINE_HOOK(0x70CA8B, TechnoClass_Railgun_AmbientDamageIgnoreTarget2, 0x6)
{
	enum { IgnoreTarget = 0x70CBB0 };

	GET_BASE(WeaponTypeClass*, pWeapon, 0x14);
	REF_STACK(DynamicVectorClass<ObjectClass*>, objects, STACK_OFFSET(0xC0, -0xAC));

	if (WeaponTypeExt::Fetch(pWeapon)->AmbientDamage_IgnoreTarget.Get(RulesExt::Global()->AmbientDamage_IgnoreTarget))
	{
		R->EAX(objects.Count);
		return IgnoreTarget;
	}

	return 0;
}

DEFINE_HOOK(0x70CBDA, TechnoClass_Railgun_AmbientDamageWarhead, 0x6)
{
	enum { SkipGameCode = 0x70CBE0 };

	GET(WeaponTypeClass*, pWeapon, EDI);

	R->EDX(WeaponTypeExt::Fetch(pWeapon)->AmbientDamage_Warhead.Get(pWeapon->Warhead));

	return SkipGameCode;
}

DEFINE_HOOK(0x75F39D, WaveClass_DamageAI_AmbientDamageWarhead, 0x6)
{
	enum { SkipGameCode = 0x75F3A3 };

	GET(WeaponTypeClass*, pWeapon, EBX);

	auto const pTypeExt = WeaponTypeExt::Fetch(pWeapon);
	FireAtTemp::IgnoreTargetForWaveAmbientDamage = pTypeExt->AmbientDamage_IgnoreTarget.Get(RulesExt::Global()->AmbientDamage_IgnoreTarget);
	R->EAX(pTypeExt->AmbientDamage_Warhead.Get(pWeapon->Warhead));

	return SkipGameCode;
}

DEFINE_HOOK(0x75F415, WaveClass_DamageAI_AmbientDamageIgnoreTarget, 0x6)
{
	enum { IgnoreTarget = 0x75F432 };

	GET(WaveClass*, pThis, EBP);
	GET(ObjectClass*, pObject, ESI);

	if (FireAtTemp::IgnoreTargetForWaveAmbientDamage && pThis->Target == pObject)
		return IgnoreTarget;

	return 0;
}

// Do not adjust map coordinates for railgun or fire stream particles that are below cell coordinates.
DEFINE_HOOK(0x62B8BC, ParticleClass_CTOR_CoordAdjust, 0x6)
{
	enum { SkipCoordAdjust = 0x62B8CB };

	GET(ParticleClass*, pThis, ESI);
	const auto pParticleSys = pThis->ParticleSystem;

	if (pParticleSys && pParticleSys->Type)
	{
		const auto behavesLike = pParticleSys->Type->BehavesLike;

		if (behavesLike == BehavesLike::Railgun || behavesLike == BehavesLike::Fire)
			return SkipCoordAdjust;
	}

	return 0;
}

DEFINE_HOOK_AGAIN(0x6FD70D, TechnoClass_DrawSth_DrawToInvisoFlakScatterLocation, 0x6) // CreateRBeam
DEFINE_HOOK_AGAIN(0x6FD514, TechnoClass_DrawSth_DrawToInvisoFlakScatterLocation, 0x7) // CreateEBolt
DEFINE_HOOK(0x6FD38D, TechnoClass_DrawSth_DrawToInvisoFlakScatterLocation, 0x7) // CreateLaser
{
	GET(CoordStruct*, pTargetCoords, EAX);

	if (const auto pBullet = FireAtTemp::FireBullet)
	{
		// The weapon may not have been set up
		const auto pWeaponExt = WeaponTypeExt::TryFetch(pBullet->WeaponType);

		if (pWeaponExt && pWeaponExt->VisualScatter)
		{
			const auto& pRulesExt = RulesExt::Global();
			const auto radius = ScenarioClass::Instance->Random.RandomRanged(pRulesExt->VisualScatter_Min.Get(), pRulesExt->VisualScatter_Max.Get());
			*pTargetCoords = MapClass::GetRandomCoordsNear(BulletExt::GetTargetCoordsForFiring(pBullet), radius, false);
		}
		else
		{
			*pTargetCoords = BulletExt::GetTargetCoordsForFiring(pBullet);
		}
	}
	else if (const auto pObstacleCell = FireAtTemp::pObstacleCell)
	{
		*pTargetCoords = pObstacleCell->GetCoordsWithBridge();
	}

	R->EAX(pTargetCoords);
	return 0;
}

// Adjust target for bolt / beam / wave drawing.
DEFINE_HOOK(0x6FF43F, TechnoClass_FireAt_TargetSet, 0x6)
{
	GET(WeaponTypeClass*, pWeapon, EBX);
	LEA_STACK(CoordStruct*, pTargetCoords, STACK_OFFSET(0xB0, -0x28));
	GET_BASE(AbstractClass*, pOriginalTarget, 0x8);

	// Store original target & coords
	FireAtTemp::FireWeapon = pWeapon;
	FireAtTemp::CheckedOtherWaves = false;
	FireAtTemp::OriginalTargetCoords = *pTargetCoords;
	FireAtTemp::pOriginalTarget = pOriginalTarget;

	if (FireAtTemp::pObstacleCell)
	{
		*pTargetCoords = FireAtTemp::pObstacleCell->GetCoordsWithBridge();
		R->Base(8, FireAtTemp::pObstacleCell); // Replace original target so it gets used by Ares sonic wave stuff etc. as well.
	}

	return 0;
}

// Allow IsLaser, IsElectricBolt, IsRadBeam, and IsRadEruption weapons to also spawn Wave.IsLaser, Wave.IsBigLaser, or IsMagBeam waves.
DEFINE_HOOK(0x6FF65A, TechnoClass_FireAt_CheckOtherWaves, 0x6)
{
	enum { CheckOtherWaves = 0x6FF5F5 };

	GET(WeaponTypeClass*, pWeapon, EBX);

	if (!FireAtTemp::CheckedOtherWaves && pWeapon
		&& (pWeapon->IsLaser || pWeapon->IsElectricBolt || pWeapon->IsRadBeam || pWeapon->IsRadEruption))
	{
		FireAtTemp::CheckedOtherWaves = true;
		R->EDI(R->Base<AbstractClass*>(0x8));
		return CheckOtherWaves;
	}

	return 0;
}

// Restore original target values and unset obstacle cell.
DEFINE_HOOK(0x6FF660, TechnoClass_FireAt_ObstacleCellUnset, 0x6)
{
	LEA_STACK(CoordStruct*, pTargetCoords, STACK_OFFSET(0xB0, -0x28));

	// Restore original target & coords
	*pTargetCoords = FireAtTemp::OriginalTargetCoords;
	R->Base(8, FireAtTemp::pOriginalTarget);
	R->EDI(FireAtTemp::pOriginalTarget);

	// Reset temp values
	FireAtTemp::FireBullet = nullptr;
	FireAtTemp::FireWeapon = nullptr;
	FireAtTemp::CheckedOtherWaves = false;
	FireAtTemp::OriginalTargetCoords = CoordStruct::Empty;
	FireAtTemp::pObstacleCell = nullptr;
	FireAtTemp::pOriginalTarget = nullptr;

	return 0;
}

// Allow drawing single color lasers with thickness and antialiased lasers.
DEFINE_HOOK(0x6FD446, TechnoClass_LaserZap_IsSingleColor, 0x7)
{
	GET(WeaponTypeClass* const, pWeapon, ECX);
	GET(LaserDrawClass* const, pLaser, EAX);

	if (auto const pWeaponExt = WeaponTypeExt::TryFetch(pWeapon))
	{
		if (!pLaser->IsHouseColor && pWeaponExt->Laser_IsSingleColor)
			pLaser->IsHouseColor = true;

		*reinterpret_cast<bool*>(reinterpret_cast<uintptr_t>(pLaser) + 0x53) = pWeaponExt->IsAntialiasedLaser;
	}

	// Fixes drawing thick lasers for non-PrismSupport building-fired lasers.
	pLaser->IsSupported = pLaser->Thickness > 3;

	return 0;
}

DEFINE_HOOK(0x6FC6EE, TechnoClass_CanFire_WaveDelayROF, 0x6)
{
	enum { ReturnFireErrorRearm = 0x6FC8A1 };

	GET(TechnoClass*, pThis, ESI);
	GET(WeaponTypeClass*, pWeapon, EBP);

	if (pThis->Wave && FireAtTemp::ShouldDelayWaveROF(pThis, pWeapon))
		return ReturnFireErrorRearm;

	return 0;
}

DEFINE_HOOK(0x75E97B, WaveClass_CTOR_Init, 0x6)
{
	GET(WaveClass*, pThis, ESI);
	GET(WaveType, type, ECX);

	// Fix swapped Wave.IsBigLaser and Wave.IsLaser wave types.
	if (type == WaveType::BigLaser)
		type = WaveType::Laser;
	else if (type == WaveType::Laser)
		type = WaveType::BigLaser;

	pThis->Type = type;
	R->ECX(type);

	if (FireAtTemp::FireWeapon)
		FireAtTemp::WaveWeaponMap[pThis] = FireAtTemp::FireWeapon;

	return 0;
}

DEFINE_HOOK(0x75E98E, WaveClass_CTOR_SonicDuration, 0xA)
{
	enum { SkipGameCode = 0x75E998 };

	GET(WaveClass*, pThis, ESI);

	auto const pWeapon = FireAtTemp::GetWaveWeapon(pThis);
	auto const pWeaponExt = WeaponTypeExt::TryFetch(pWeapon);

	pThis->WaveEC = (pWeaponExt && pWeaponExt->Wave_Duration.isset())
		? std::max(1, pWeaponExt->Wave_Duration.Get()) : 100;

	return SkipGameCode;
}

DEFINE_HOOK(0x75EB87, WaveClass_CTOR_LaserDuration, 0xA)
{
	enum { SkipGameCode = 0x75EB91 };

	GET(WaveClass*, pThis, ESI);

	auto const pWeapon = FireAtTemp::GetWaveWeapon(pThis);
	auto const pWeaponExt = WeaponTypeExt::TryFetch(pWeapon);

	pThis->LaserEC = (pWeaponExt && pWeaponExt->Wave_Duration.isset())
		? std::max(33, pWeaponExt->Wave_Duration.Get()) : 160;

	return SkipGameCode;
}

DEFINE_HOOK(0x75ED30, WaveClass_DTOR_Cleanup, 0x6)
{
	GET(WaveClass*, pThis, ECX);
	FireAtTemp::WaveWeaponMap.erase(pThis);
	return 0;
}

DEFINE_HOOK_AGAIN(0x762492, WaveClass_InitSonicAndMagnetic_EndZAdjustAndGeometry, 0x6)
DEFINE_HOOK(0x761A62, WaveClass_InitSonicAndMagnetic_EndZAdjustAndGeometry, 0x6)
{
	GET(WaveClass*, pThis, EDI);
	GET(const int, endZ, EAX);

	const bool isMagnetic = R->Origin() == 0x762492;
	FireAtTemp::ApplyWaveGeometry(pThis, isMagnetic);

	int endZAdjust = (pThis->Type == WaveType::Sonic || pThis->Type == WaveType::Magnetron) ? 50 : 0;

	if (auto const pWeapon = FireAtTemp::GetWaveWeapon(pThis))
	{
		if (auto const pWeaponExt = WeaponTypeExt::TryFetch(pWeapon))
		{
			if (pWeaponExt->Wave_EndZAdjust.isset())
				endZAdjust = pWeaponExt->Wave_EndZAdjust.Get();
		}
	}

	R->Stack(0x3C, endZ + endZAdjust);
	return R->Origin() + 0x17;
}

// WaveClass requires the firer's target and wave's target to match so it needs bit of extra handling here for obstacle cell targets.
DEFINE_HOOK(0x762AFF, WaveClass_AI_TargetSet, 0x6)
{
	enum { SkipCutoffAndMagnetronLoop = 0x762B62 };

	GET(WaveClass*, pThis, ESI);

	if (pThis->Target)
	{
		if (auto const pOwner = pThis->Owner)
		{
			auto const pObstacleCell = TechnoExt::Fetch(pOwner)->FiringObstacleCell;

			if (pObstacleCell == pThis->Target && pOwner->Target)
			{
				FireAtTemp::pWaveOwnerTarget = pOwner->Target;
				pOwner->Target = pThis->Target;
			}
		}
	}

	auto const pWeapon = FireAtTemp::GetWaveWeapon(pThis);
	auto const pWeaponExt = WeaponTypeExt::TryFetch(pWeapon);

	const double step = (pWeaponExt && pWeaponExt->Wave_Step.isset() && pWeaponExt->Wave_Step.Get() > 0.0)
		? pWeaponExt->Wave_Step.Get() : 0.05;
	const int detachThreshold = std::max(1, static_cast<int>(1.0 / step) - 1);

	if (pThis->Type == WaveType::Magnetron)
	{
		if (!pWeaponExt || !pWeaponExt->Wave_Duration.isset())
		{
			if (pThis->WaveEC == detachThreshold && pThis->WaveCount < 4)
			{
				pThis->WaveEC = detachThreshold + 45;
				++pThis->WaveCount;
			}
		}
	}

	if (pThis->WaveEC <= detachThreshold)
	{
		pThis->IsTraveling = false;
		pThis->ShouldEnd = true;
	}
	else if (detachThreshold != 19 && pThis->WaveEC == 19)
	{
		FireAtTemp::RestoredWaveEC19 = true;
		pThis->WaveEC = 20;
	}

	if ((pWeaponExt && pWeaponExt->IsClassicSonic)
		|| (pThis->Type == WaveType::Magnetron && ((pWeaponExt && pWeaponExt->Wave_Duration.isset()) || detachThreshold != 19)))
	{
		R->ECX(pThis->Owner);
		return SkipCutoffAndMagnetronLoop;
	}

	return 0;
}

DEFINE_HOOK(0x762D57, WaveClass_AI_TargetUnset, 0x6)
{
	GET(WaveClass*, pThis, ESI);

	if (FireAtTemp::RestoredWaveEC19)
	{
		pThis->WaveEC = 19;
		FireAtTemp::RestoredWaveEC19 = false;
	}

	if (FireAtTemp::pWaveOwnerTarget)
	{
		if (pThis->Owner->Target)
			pThis->Owner->Target = FireAtTemp::pWaveOwnerTarget;

		FireAtTemp::pWaveOwnerTarget = nullptr;
	}

	return 0;
}

DEFINE_HOOK_AGAIN(0x762F77, WaveClass_AI_StepStart, 0x6)
DEFINE_HOOK(0x762D89, WaveClass_AI_StepStart, 0x6)
{
	GET(WaveClass*, pThis, ESI);

	auto const pWeapon = FireAtTemp::GetWaveWeapon(pThis);
	auto const pWeaponExt = WeaponTypeExt::TryFetch(pWeapon);

	const double step = (pWeaponExt && pWeaponExt->Wave_Step.isset() && pWeaponExt->Wave_Step.Get() > 0.0)
		? pWeaponExt->Wave_Step.Get() : static_cast<double>(0.05f);

	pThis->MatrixScale1 += step;
	if (pThis->MatrixScale1 > 1.0 - step * 0.4)
		pThis->MatrixScale1 = 1.0;

	return R->Origin() + 0x33;
}

DEFINE_HOOK_AGAIN(0x7630A1, WaveClass_AI_StepEnd, 0x6)
DEFINE_HOOK(0x762E65, WaveClass_AI_StepEnd, 0x6)
{
	enum { SkipEndStep = 0x7631DB };

	GET(WaveClass*, pThis, ESI);

	auto const pWeapon = FireAtTemp::GetWaveWeapon(pThis);
	auto const pWeaponExt = WeaponTypeExt::TryFetch(pWeapon);

	const double step = (pWeaponExt && pWeaponExt->Wave_Step.isset() && pWeaponExt->Wave_Step.Get() > 0.0)
		? pWeaponExt->Wave_Step.Get() : static_cast<double>(0.05f);

	if (pThis->MatrixScale2 >= pThis->MatrixScale1 + step * 0.5)
		return SkipEndStep;

	pThis->MatrixScale2 += step - static_cast<double>(0.05f);
	return R->Origin() + 0x23;
}

DEFINE_HOOK(0x761124, WaveClass_UpdateCells_InitialScale, 0x6)
{
	enum { SkipGameCode = 0x76112F };

	GET(WaveClass*, pThis, ESI);
	GET(const int, startX, EAX);

	R->EDX(startX < 0 ? -1 : 0);
	R->Stack<double>(0x40, std::max(static_cast<double>(0.05f), pThis->MatrixScale2));
	return SkipGameCode;
}

DEFINE_HOOK(0x75EE36, WaveClass_PutSonicBeamPixel_SineTableWrap, 0x6)
{
	enum { SkipGameCode = 0x75EE3C };

	GET(const int, baseIndex, EAX);
	GET(const int, waveEC, EDI);
	GET_STACK(const int, buffer, 0x28);

	int index = (baseIndex + waveEC) % 500;
	if (index < 0)
		index += 500;

	R->EAX(index);
	R->EDI(buffer);
	return SkipGameCode;
}

DEFINE_HOOK(0x7601CF, WaveClass_PutMagBeamPixel_SineTableWrap, 0xA)
{
	enum { SkipGameCode = 0x7601D9 };

	GET(const int, baseIndex, EAX);
	GET(const int, waveEC, EBX);

	int index = (baseIndex + waveEC) % 500;
	if (index < 0)
		index += 500;

	R->EAX(static_cast<int>(reinterpret_cast<const short*>(0xB45E68u)[index]));
	return SkipGameCode;
}

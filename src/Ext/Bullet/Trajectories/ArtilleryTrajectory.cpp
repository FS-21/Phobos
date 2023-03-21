#include "ArtilleryTrajectory.h"
#include <Ext/BulletType/Body.h>
#include <ScenarioClass.h>

// Save and Load
bool ArtilleryTrajectoryType::Load(PhobosStreamReader& Stm, bool RegisterForChange)
{
	this->PhobosTrajectoryType::Load(Stm, false);
	Stm.Process(this->MaxHeight, false);
	return true;
}

bool ArtilleryTrajectoryType::Save(PhobosStreamWriter& Stm) const
{
	this->PhobosTrajectoryType::Save(Stm);
	Stm.Process(this->MaxHeight);
	return true;
}

// INI reading stuff
void ArtilleryTrajectoryType::Read(CCINIClass* const pINI, const char* pSection)
{
	this->MaxHeight = pINI->ReadDouble(pSection, "Trajectory.Artillery.MaxHeight", 2000);
}

bool ArtilleryTrajectory::Load(PhobosStreamReader& Stm, bool RegisterForChange)
{
	this->PhobosTrajectory::Load(Stm, false);
	return true;
}

bool ArtilleryTrajectory::Save(PhobosStreamWriter& Stm) const
{
	this->PhobosTrajectory::Save(Stm);
	return true;
}

// Do some math here to set the initial speed of your proj
void ArtilleryTrajectory::OnUnlimbo(BulletClass* pBullet, CoordStruct* pCoord, BulletVelocity* pVelocity)
{
	this->InitialTargetLocation = pBullet->TargetCoords;

	if (pBullet->Type->Inaccurate)
	{
		auto const pTypeExt = BulletTypeExt::ExtMap.Find(pBullet->Type);

		int ballisticScatter = RulesClass::Instance()->BallisticScatter;
		int scatterMax = pTypeExt->BallisticScatter_Max.isset() ? (int)(pTypeExt->BallisticScatter_Max.Get() * 256.0) : ballisticScatter;
		int scatterMin = pTypeExt->BallisticScatter_Min.isset() ? (int)(pTypeExt->BallisticScatter_Min.Get() * 256.0) : (scatterMax / 2);

		double random = ScenarioClass::Instance()->Random.RandomRanged(scatterMin, scatterMax);
		double theta = ScenarioClass::Instance()->Random.RandomDouble() * Math::TwoPi;

		CoordStruct offset
		{
			static_cast<int>(random * Math::cos(theta)),
			static_cast<int>(random * Math::sin(theta)),
			0
		};

		this->InitialTargetLocation += offset;
	}

	this->InitialSourceLocation = pBullet->SourceCoords;

	CoordStruct sourceDist = pBullet->SourceCoords;
	sourceDist.Z = 0;
	CoordStruct targetDist = this->InitialTargetLocation;//pBullet->TargetCoords;
	targetDist.Z = 0;

	this->TotalDistance = sourceDist.DistanceFrom(targetDist);
	this->HalfDistance = TotalDistance / 2;
	this->AttackerHeight = pBullet->Location.Z;

	pBullet->Velocity *= this->GetTrajectorySpeed(pBullet) / pBullet->Velocity.Magnitude();

	return;
}

// Some early checks here, returns whether or not to detonate the bullet
bool ArtilleryTrajectory::OnAI(BulletClass* pBullet)
{
	CoordStruct sourceLocationXY = this->InitialSourceLocation;
	sourceLocationXY.Z = 0;
	CoordStruct bulletCoordsXY = pBullet->Location;
	bulletCoordsXY.Z = 0;

	double fullInitialDistance = this->TotalDistance;
	double halfInitialDistance = this->HalfDistance;
	double currentBulletDistance = sourceLocationXY.DistanceFrom(bulletCoordsXY);

	double gravityElevation = 200;
	double maxHeight = this->GetTrajectoryType<ArtilleryTrajectoryType>(pBullet)->MaxHeight;
	int zDelta = this->InitialTargetLocation.Z > this->InitialSourceLocation.Z ? this->InitialTargetLocation.Z : this->InitialSourceLocation.Z;
	maxHeight += zDelta;

	// Trajectory angle
	int sinDecimalAngle = 90;
	double sinRadianAngle = Math::sin(Math::deg2rad(sinDecimalAngle));

	// Angle of the projectile in the current location
	double angle = (currentBulletDistance * sinDecimalAngle) / halfInitialDistance;
	double sinAngle = Math::sin(Math::deg2rad(angle));

	// Update height of the flying projectile in the current location
	pBullet->Location.Z = (int)((sinAngle * maxHeight) / sinRadianAngle) + this->AttackerHeight;

	if (angle > 90)
	{
		// Bullet is falling, do bullet corrections for the special cases
		double extraZDesc = 0.0;
		double finalDistancePercentage = 0.0;
		int cliffHeight = 416; // Note: I think 416 is the height of 1 cliff
		int cliffHeightMultiplier = zDelta / cliffHeight;

		if (currentBulletDistance > halfInitialDistance)
		{
			finalDistancePercentage = 1.0 - (fullInitialDistance - currentBulletDistance) / (fullInitialDistance / 2);
			extraZDesc += (cliffHeight * finalDistancePercentage);
		}

		// Adjusting projectile fall if the target is in lower location
		if (this->InitialTargetLocation.Z < this->InitialSourceLocation.Z)
			extraZDesc += (gravityElevation * 2) * finalDistancePercentage * cliffHeightMultiplier;

		// Adjusting projectile fall if the target is in upper location
		if (this->InitialTargetLocation.Z > this->InitialSourceLocation.Z)
			extraZDesc = -gravityElevation * finalDistancePercentage * cliffHeightMultiplier;

		// Update height of the flying projectile in the current location
		if (currentBulletDistance > halfInitialDistance)
			pBullet->Location.Z = pBullet->Location.Z - static_cast<int>(extraZDesc);
	}

	return false;
}

// Where you update the speed and position
// pSpeed: The speed of this proj in the next frame
// pPosition: Current position of the proj, and in the next frame it will be *pSpeed + *pPosition
void ArtilleryTrajectory::OnAIVelocity(BulletClass* pBullet, BulletVelocity* pSpeed, BulletVelocity* pPosition)
{ }

// Where additional checks based on bullet reaching its target coordinate can be done.
// Vanilla code will do additional checks regarding buildings on target coordinate and Vertical projectiles and will detonate the projectile if they pass.
// Return value determines what is done regards to the game checks: they can be skipped, executed as normal or treated as if the condition is already satisfied.
TrajectoryCheckReturnType ArtilleryTrajectory::OnAITargetCoordCheck(BulletClass* pBullet)
{
	return TrajectoryCheckReturnType::ExecuteGameCheck; // Execute game checks.
}

// Where additional checks based on a TechnoClass instance in same cell as the bullet can be done.
// Vanilla code will do additional trajectory alterations here if there is an enemy techno in the cell.
// Return value determines what is done regards to the game checks: they can be skipped, executed as normal or treated as if the condition is already satisfied.
// pTechno: TechnoClass instance in same cell as the bullet.
TrajectoryCheckReturnType ArtilleryTrajectory::OnAITechnoCheck(BulletClass* pBullet, TechnoClass* pTechno)
{
	//return TrajectoryCheckReturnType::ExecuteGameCheck; // Execute game checks.
	return TrajectoryCheckReturnType::SkipGameCheck; // Bypass game checks entirely.
}

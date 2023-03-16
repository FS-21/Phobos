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
	//Stm.Process(this->IsFalling, false);
	return true;
}

bool ArtilleryTrajectory::Save(PhobosStreamWriter& Stm) const
{
	this->PhobosTrajectory::Save(Stm);
	//Stm.Process(this->IsFalling);
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
	this->CurrentBulletHeight = pBullet->Location.Z;

	CoordStruct sourceDist = pBullet->SourceCoords;
	sourceDist.Z = 0;
	CoordStruct targetDist = this->InitialTargetLocation;//pBullet->TargetCoords;
	targetDist.Z = 0;
	//CoordStruct bulletDist = pBullet->Location;
	//bulletDist.Z = 0;

	this->TotalDistance = sourceDist.DistanceFrom(targetDist);
	this->HalfDistance = TotalDistance / 2;
	this->AttackerHeight = pBullet->Location.Z;
	this->CurrentBulletDistance = 0;


	//double currentBulletDistance = initialSourceLocation.DistanceFrom(bulletCoords);

	double maxHeight = this->GetTrajectoryType<ArtilleryTrajectoryType>(pBullet)->MaxHeight;
	int zDelta = this->InitialTargetLocation.Z > this->InitialSourceLocation.Z ? this->InitialTargetLocation.Z : this->InitialSourceLocation.Z;
	maxHeight += zDelta;
	//CoordStruct initialSourceLocation = this->InitialSourceLocation; // Obsolete
	//initialSourceLocation.Z = 0;

	pBullet->Velocity.X = static_cast<double>(this->InitialTargetLocation.X - pBullet->SourceCoords.X);
	pBullet->Velocity.Y = static_cast<double>(this->InitialTargetLocation.Y - pBullet->SourceCoords.Y);
	pBullet->Velocity.Z = 21; // 21 because "yes", ok? just a low number...// static_cast<double>(this->GetFirerZPosition(pBullet));
	//pBullet->Velocity.Z = maxHeight/10.0;
	pBullet->Velocity *= this->GetTrajectorySpeed(pBullet) / pBullet->Velocity.Magnitude();

	return;
}

// Some early checks here, returns whether or not to detonate the bullet
bool ArtilleryTrajectory::OnAI(BulletClass* pBullet)
{
	CoordStruct targetLocationXY = this->InitialTargetLocation;
	targetLocationXY.Z = 0;
	CoordStruct sourceLocationXY = this->InitialSourceLocation;
	sourceLocationXY.Z = 0;
	CoordStruct bulletCoordsXY = pBullet->Location;
	bulletCoordsXY.Z = 0;

	double fullInitialDistance = sourceLocationXY.DistanceFrom(targetLocationXY);
	double halfInitialDistance = fullInitialDistance / 2;
	double currentBulletDistance = sourceLocationXY.DistanceFrom(bulletCoordsXY);

	//if (currentBulletDistance > fullInitialDistance)
		//return true;

	double gravityFall = 24;// 18;//RulesClass::Instance()->Gravity * 2;
	double gravityElevation = 30;//24;
	double maxHeight = this->GetTrajectoryType<ArtilleryTrajectoryType>(pBullet)->MaxHeight;
	int zDelta = this->InitialTargetLocation.Z > this->InitialSourceLocation.Z ? this->InitialTargetLocation.Z : this->InitialSourceLocation.Z;
	maxHeight += zDelta;

	if (currentBulletDistance <= 0)
		return false;

	double halfDistancePercentage = (currentBulletDistance) / halfInitialDistance;
	double halfDistanceMultiplier = 4.0 * (1 - halfDistancePercentage);
	double finalDistancePercentage = (currentBulletDistance - halfInitialDistance) / (fullInitialDistance / 2);
	double finalDistanceMultiplier = 8.0 * (finalDistancePercentage);

	double currentHeightPercentage = (pBullet->Location.Z / maxHeight);
	currentHeightPercentage = currentHeightPercentage > 1.0 ? 1.0 : currentHeightPercentage;

	double aaa;

	if (currentBulletDistance <= halfInitialDistance)
		aaa = maxHeight * (1.0 - (currentHeightPercentage < halfDistancePercentage ? halfDistancePercentage : currentHeightPercentage));
	else
		aaa = maxHeight * (finalDistancePercentage);

	//double aaa = maxHeight * (1.0 - currentHeightPercentage);
	double bbb = Math::sqrt(aaa);
	double ccc = bbb;
	double extraZAsc = ccc + (ccc * halfDistanceMultiplier);
	double extraZDesc = ccc + (ccc * finalDistanceMultiplier * 0.5);

	// Tuning projectile falling & the target is in lower location
	if (this->InitialTargetLocation.Z < this->InitialSourceLocation.Z && finalDistancePercentage > 0.50)
		extraZDesc += gravityFall;
	//double newBulletHeight = pBullet->Location.Z + extraZ;

	// Tuning projectile falling & the target is in upper location
	if (this->InitialTargetLocation.Z > this->InitialSourceLocation.Z && finalDistancePercentage > 0.50)
		extraZDesc = ccc + (ccc * finalDistanceMultiplier * 0.25) - gravityElevation;

	if (pBullet->Location.Z > maxHeight)
		pBullet->Location.Z = maxHeight;

	//pBullet->Velocity.Z = currentBulletHeight;//static_cast<double>(maxHeight - pBullet->Location.Z) / 0.5;

	if (currentBulletDistance <= halfInitialDistance)
		pBullet->Location.Z = pBullet->Location.Z + static_cast<int>(extraZAsc);
	else
		pBullet->Location.Z = pBullet->Location.Z - static_cast<int>(extraZDesc);


	int a = 0;



	// Trajectory angle
	//int sinDecimalAngle = 45;
	//double sinRadianAngle = Math::sin(Math::deg2rad(sinDecimalAngle));

	// Angle of the projectile in the current location
	//double angle = (currentBulletDistance * sinDecimalAngle) / halfInitialDistance;

	//if (angle > 90)
		//angle += 12.5;

	//double sinAngle = Math::sin(Math::deg2rad(angle));

	// Height of the flying projectile in the current location
	//double currHeight = (sinAngle * maxHeight) / sinRadianAngle;
	return false;
	// % of the current height compared to the max height the bullet can reach
	//double currentHeightPercentage = (pBullet->Location.Z / maxHeight);
	//currentHeightPercentage = currentHeightPercentage > 1.0 ? 1.0 : currentHeightPercentage;
	//double extraZ = maxHeight * (1.0 - currentHeightPercentage) * 0.5;
	//double newBulletHeight = pBullet->Location.Z + extraZ;

	//double gravity = RulesClass::Instance()->Gravity;
	//pBullet->Velocity.Z = currentBulletHeight;//static_cast<double>(maxHeight - pBullet->Location.Z) / 0.5;

	//if (currentBulletDistance <= halfInitialDistance)
		//pBullet->Location.Z += extraZ;
	//else
		//pBullet->Location.Z -= extraZ;

	//if (currHeight != 0)
		//pBullet->Location.Z = this->AttackerHeight + (int)currHeight;// this->InitialSourceLocation.Z + (int)currHeight;
	//pBullet->Velocity.Z = GetVelocityZ(pBullet);

	//int a = 0;




	/*double closeEnough = pBullet->TargetCoords.DistanceFrom(pBullet->Location);
	if (closeEnough < 50)
	{
		pBullet->Location = pBullet->TargetCoords;
		pBullet->Explode(true);
		pBullet->UnInit();
		pBullet->LastMapCoords = CellClass::Coord2Cell(pBullet->Location);
	}*/



	//f(x) = X^2
	/*
	CoordStruct targetLocation = this->InitialTargetLocation;
	targetLocation.Z = 0;
	CoordStruct sourceLocation = this->InitialSourceLocation;
	sourceLocation.Z = 0;
	CoordStruct bulletCoords = pBullet->Location;
	bulletCoords.Z = 0;

	double fullInitialDistance = sourceLocation.DistanceFrom(targetLocation);
	double halfInitialDistance = fullInitialDistance / 2;
	double currentBulletDistance = sourceLocation.DistanceFrom(bulletCoords);

	double distancePercentage = (currentBulletDistance) / halfInitialDistance;
	double maxHeight = this->GetTrajectoryType<ArtilleryTrajectoryType>(pBullet)->MaxHeight;
	//double newBulletHeight = (int)(maxHeight * distancePercentage);

	double maxHeightSQRT = Math::sqrt(maxHeight);




	return false;




	double maxHeight = this->GetTrajectoryType<ArtilleryTrajectoryType>(pBullet)->MaxHeight;

	CoordStruct targetLocation = this->InitialTargetLocation;
	targetLocation.Z = 0;
	CoordStruct sourceLocation = this->InitialSourceLocation;
	sourceLocation.Z = 0;
	CoordStruct bulletCoords = pBullet->Location;
	bulletCoords.Z = 0;

	double fullInitialDistance = sourceLocation.DistanceFrom(targetLocation);
	double halfInitialDistance = fullInitialDistance / 2;
	double currentBulletDistance = sourceLocation.DistanceFrom(bulletCoords);

	if (this->Stage == 0)
	{
		int zDelta = this->InitialTargetLocation.Z - this->InitialSourceLocation.Z;
		maxHeight += zDelta;

		// Trajectory angle
		int sinDecimalAngle = 90;
		double sinRadianAngle = Math::sin(Math::deg2rad(sinDecimalAngle));

		// Angle of the projectile in the current location
		double angle = (currentBulletDistance * sinDecimalAngle) / halfInitialDistance;
		double sinAngle = Math::sin(Math::deg2rad(angle));

		// Height of the flying projectile in the current location
		double currHeight = (sinAngle * maxHeight) / sinRadianAngle;

		if (currHeight != 0)
			pBullet->Location.Z = this->InitialSourceLocation.Z + (int)currHeight;

		if (currentBulletDistance >= halfInitialDistance)
			Stage = 1;

		return false;
	}

	if (this->Stage == 1)
	{
		// En este punto el origen.Z es el maxHeight y el destino lo que sea... puede estar por enciam o debajo del objeto origen inicial.
		int destHeight = this->InitialTargetLocation.Z;
		int sourceHeight = pBullet->Location.Z;
		int totalHeight = std::abs(destHeight - sourceHeight);
		this->InitialSourceLocation = pBullet->Location;
		Stage = 2;

		return false;
	}

	if (this->Stage == 2)
	{
		//int zDelta = std::abs(this->InitialTargetLocation.Z - this->InitialSourceLocation.Z);

		double finalDistance = sourceLocation.DistanceFrom(targetLocation);
		//double startDistance = 0;
		double currentBulletDistance = sourceLocation.DistanceFrom(bulletCoords);

		// Trajectory angle
		int sinDecimalAngle = 90;
		double sinRadianAngle = Math::sin(Math::deg2rad(sinDecimalAngle));

		// Angle of the projectile in the current location
		double angle = (currentBulletDistance * sinDecimalAngle) / finalDistance;
		double sinAngle = Math::sin(Math::deg2rad(angle));

		// Height of the flying projectile in the current location
		double currHeight = (sinAngle * maxHeight) / sinRadianAngle;

		if (currHeight != 0)
			pBullet->Location.Z = this->InitialSourceLocation.Z - (int)currHeight;

		//if (currentBulletDistance >= finalDistance)//(finalDistance * 0.9))
			//Stage = 3;

		return false;
	}

	// If the projectile is close enough to the target then explode it
	double closeEnough = pBullet->TargetCoords.DistanceFrom(pBullet->Location);
	if (closeEnough < 100)
	{
		pBullet->Explode(true);
		pBullet->UnInit();
		pBullet->LastMapCoords = CellClass::Coord2Cell(pBullet->Location);
	}

	return false; // apartir de aquí no se ejecuta :-P



	// Close enough
	//if (pBullet->TargetCoords.DistanceFrom(pBullet->Location) < 100)
		//return true;
	/* {
		pBullet->Detonate(pBullet->Location);
		pBullet->UnInit();
		pBullet->LastMapCoords = CellClass::Coord2Cell(pBullet->Location);
	}*/

	/*
	if (!this->IsFalling)
	{
		pSpeed->Z += BulletTypeExt::GetAdjustedGravity(pBullet->Type);
		double dx = pBullet->TargetCoords.X - pBullet->Location.X;
		double dy = pBullet->TargetCoords.Y - pBullet->Location.Y;
		if (dx * dx + dy * dy < pBullet->Velocity.X * pBullet->Velocity.X + pBullet->Velocity.Y * pBullet->Velocity.Y)
		{
			this->IsFalling = true;
			pSpeed->X = 0.0;
			pSpeed->Y = 0.0;
			pSpeed->Z = 0.0;
			pPosition->X = pBullet->TargetCoords.X;
			pPosition->Y = pBullet->TargetCoords.Y;
		}
	}*/
	/*int newBulletDist;
	double distancePercentage;
	//double maxHeight;
	int newBulletHeight;

	switch (this->Stage)
	{
	case 0:
		Stage++;
		break;

	case 1:
		// Flying up!
		//this->CurrentBulletHeight = pBullet->Location.Z;;
		//this->CurrentBulletDistance = 0;

		if (this->CurrentBulletDistance >= this->HalfDistance)
		{
			this->Stage++; // Not flying up anymore!
			return false;
		}

		// Calculamos la distancia 2D de la bala al destino
		CoordStruct targetLoc = this->InitialTargetLocation;//pBullet->TargetCoords;
		targetLoc.Z = 0;
		CoordStruct bulletLoc = pBullet->Location;
		bulletLoc.Z = 0;

		newBulletDist = (int)(this->TotalDistance - bulletLoc.DistanceFrom(targetLoc));
		this->CurrentBulletDistance = newBulletDist;
		//Regla de 3: Si "Distancia total"/2 (mitadRecorridoTotal) es el 100% entonces DistanciaActualRecorrida es "X%".
		distancePercentage = (newBulletDist * 1.0) / this->HalfDistance;
		maxHeight = this->GetTrajectoryType<ArtilleryTrajectoryType>(pBullet)->MaxHeight;
		newBulletHeight = (int)(maxHeight * distancePercentage);
		this->CurrentBulletHeight = newBulletHeight < this->CurrentBulletHeight ? this->CurrentBulletHeight : newBulletHeight; // Bullet can fall again!

		pBullet->Location.Z = this->CurrentBulletHeight;
		break;

	case 2:
		pBullet->Detonate(pBullet->Location);
		pBullet->UnInit();
		//pBullet->LastMapCoords = CellClass::Coord2Cell(pBullet->Location);
		break;

	case 3:
		break;

	default:
		break;
	}

	return false;*/
}

// Where you update the speed and position
// pSpeed: The speed of this proj in the next frame
// pPosition: Current position of the proj, and in the next frame it will be *pSpeed + *pPosition
void ArtilleryTrajectory::OnAIVelocity(BulletClass* pBullet, BulletVelocity* pSpeed, BulletVelocity* pPosition)
{





	/*
	double maxHeight = this->GetTrajectoryType<ArtilleryTrajectoryType>(pBullet)->MaxHeight;
	int zDelta = this->InitialTargetLocation.Z - this->InitialSourceLocation.Z;
	maxHeight += zDelta;

	CoordStruct targetLocation = this->InitialTargetLocation;
	targetLocation.Z = 0;
	CoordStruct sourceLocation = this->InitialSourceLocation;
	sourceLocation.Z = 0;
	CoordStruct bulletCoords = pBullet->Location;
	bulletCoords.Z = 0;

	double fullInitialDistance = sourceLocation.DistanceFrom(targetLocation);
	double halfInitialDistance = fullInitialDistance / 2;
	double currentBulletDistance = sourceLocation.DistanceFrom(bulletCoords);

	// Trajectory angle
	int sinDecimalAngle = 90;
	double sinRadianAngle = Math::sin(Math::deg2rad(sinDecimalAngle));

	// Angle of the projectile in the current location
	double angle = (currentBulletDistance * sinDecimalAngle) / halfInitialDistance;
	double sinAngle = Math::sin(Math::deg2rad(angle));

	// Height of the flying projectile in the current location
	double currHeight = (sinAngle * maxHeight) / sinRadianAngle;

	if (currHeight != 0)
		pBullet->Location.Z = this->InitialSourceLocation.Z + (int)currHeight;
	*/
	//pSpeed->Z += BulletTypeExt::GetAdjustedGravity(pBullet->Type); // We don't want to take the gravity into account
}

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

int ArtilleryTrajectory::GetVelocityZ(BulletClass* pBullet)
{
	int velocity = GetTargetZPosition(pBullet) - pBullet->Location.Z;

	return velocity;
}

int ArtilleryTrajectory::GetFirerZPosition(BulletClass* pBullet)
{
	if (!pBullet)
		return 0;

	auto coords = pBullet->SourceCoords;

	if (pBullet->Owner)
	{
		auto const pCell = pBullet->Owner->GetCell();

		if (pCell)
			coords = pCell->GetCoordsWithBridge();
	}

	return coords.Z;
}

int ArtilleryTrajectory::GetTargetZPosition(BulletClass* pBullet)
{
	if (!pBullet)
		return 0;

	auto coords = pBullet->TargetCoords;

	if (pBullet->Target)
	{
		auto const pCell = MapClass::Instance()->TryGetCellAt(pBullet->Target->GetCoords());

		if (pCell)
			coords = pCell->GetCoordsWithBridge();
	}

	return coords.Z;
}

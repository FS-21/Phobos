#pragma once

#include "PhobosTrajectory.h"

/*
* This is a sample class telling you how to make a new type of Trajectory
* Author: secsome
*/

// Used in BulletTypeExt
class ArtilleryTrajectoryType final : public PhobosTrajectoryType
{
public:
	ArtilleryTrajectoryType() : PhobosTrajectoryType(TrajectoryFlag::Artillery)
		, MaxHeight { 0.0 }
	{ }

	virtual bool Load(PhobosStreamReader& Stm, bool RegisterForChange) override;
	virtual bool Save(PhobosStreamWriter& Stm) const override;

	virtual void Read(CCINIClass* const pINI, const char* pSection) override;

	// Your type properties
	double MaxHeight;
};

// Used in BulletExt
class ArtilleryTrajectory final : public PhobosTrajectory
{
public:
	// This constructor is for Save & Load
	ArtilleryTrajectory() : PhobosTrajectory(TrajectoryFlag::Artillery)
		//, IsFalling { false }
		, InitialTargetLocation { CoordStruct::Empty }
		, InitialSourceLocation { CoordStruct::Empty }
		, LastLocation { CoordStruct::Empty }
		, CurrentBulletHeight { 0 }
		, CurrentBulletDistance { 0 }
		, TotalDistance { 0.0 }
		, HalfDistance { 0.0 }
		, AttackerHeight { 0 }
		, Stage { 0 }
	{}

	ArtilleryTrajectory(PhobosTrajectoryType* pType) : PhobosTrajectory(TrajectoryFlag::Artillery)
		//, IsFalling { false }
		, InitialTargetLocation { CoordStruct::Empty }
		, InitialSourceLocation { CoordStruct::Empty }
		, LastLocation { CoordStruct::Empty }
		, CurrentBulletHeight { 0 }
		, CurrentBulletDistance { 0 }
		, TotalDistance { 0.0 }
		, HalfDistance { 0.0 }
		, AttackerHeight { 0 }
		, Stage { 0 }
	{}

	virtual bool Load(PhobosStreamReader& Stm, bool RegisterForChange) override;
	virtual bool Save(PhobosStreamWriter& Stm) const override;

	virtual void OnUnlimbo(BulletClass* pBullet, CoordStruct* pCoord, BulletVelocity* pVelocity) override;
	virtual bool OnAI(BulletClass* pBullet) override;
	virtual void OnAIVelocity(BulletClass* pBullet, BulletVelocity* pSpeed, BulletVelocity* pPosition) override;
	virtual TrajectoryCheckReturnType OnAITargetCoordCheck(BulletClass* pBullet) override;
	virtual TrajectoryCheckReturnType OnAITechnoCheck(BulletClass* pBullet, TechnoClass* pTechno) override;
	virtual int GetVelocityZ(BulletClass* pBullet);
	virtual int GetFirerZPosition(BulletClass* pBullet);
	virtual int GetTargetZPosition(BulletClass* pBullet);

	// Your properties
	//bool IsFalling;
	CoordStruct InitialTargetLocation;
	CoordStruct InitialSourceLocation;
	CoordStruct LastLocation;
	int CurrentBulletHeight;
	int CurrentBulletDistance;
	double TotalDistance;
	double HalfDistance;
	int AttackerHeight;
	int Stage;
};

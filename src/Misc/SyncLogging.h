#pragma once

#include <AbstractClass.h>
#include <GeneralDefinitions.h>
#include <Randomizer.h>
#include <TeamClass.h>

#include <vector>

class BulletClass;
class TechnoClass;
class WarheadTypeClass;

// These determine how many of each type of sync log event are stored in the buffers.
// Any events added beyond this count overwrite old ones.
static constexpr unsigned int RNGCalls_Size = 4096;
static constexpr unsigned int FacingChanges_Size = 1024;
static constexpr unsigned int TargetChanges_Size = 1024;
static constexpr unsigned int DestinationChanges_Size = 1024;
static constexpr unsigned int MissionOverrides_Size = 256;
static constexpr unsigned int AnimCreations_Size = 512;
static constexpr unsigned int DamageEvents_Size = 2048;
static constexpr unsigned int BulletLaunches_Size = 1024;
static constexpr unsigned int BulletSteps_Size = 8192;

// Intention: Data structure that stores sync event records.
// Stores fixed amount of items determined by size template arg. Any further additions
// start to overwrite items from oldest to newest, cycling endlessly where need be.
// Likewise read/get returns items in order they were added from oldest to newest.
// TODO: Clean up / improve
template <typename T, unsigned int size>
class SyncLogEventBuffer
{
private:
	std::vector<T> Data;
	int LastWritePosition;
	int LastReadPosition;
	bool HasBeenFilled;
public:
	SyncLogEventBuffer() : Data(size), LastWritePosition(0), LastReadPosition(-1), HasBeenFilled(false) { };

	bool Add(T item)
	{
		Data[LastWritePosition] = item;
		LastWritePosition++;

		if (static_cast<size_t>(LastWritePosition) >= Data.size())
		{
			HasBeenFilled = true;
			LastWritePosition = 0;
			return true;
		}

		return false;
	}

	T Get()
	{
		if (!LastWritePosition && LastReadPosition == -1 && !HasBeenFilled)
			return T();

		if (LastReadPosition == -1 && HasBeenFilled)
			LastReadPosition = LastWritePosition;
		else if (LastReadPosition == -1 || static_cast<size_t>(LastReadPosition) >= Data.size())
			LastReadPosition = 0;

		return Data[LastReadPosition++];
	}

	size_t Size() { return Data.size(); }
};

struct SyncLogEvent
{
	bool Initialized;
	unsigned int Caller;
	unsigned int Frame;

	SyncLogEvent() : Initialized(false), Caller(0), Frame(0) { }

	SyncLogEvent(unsigned int Caller, unsigned int Frame)
		: Caller(Caller), Frame(Frame)
	{
		Initialized = true;
	}
};

struct RNGCallSyncLogEvent : SyncLogEvent
{
	int Type; // 0 = Invalid, 1 = Unranged, 2 = Ranged
	bool IsCritical;
	unsigned int Index1;
	unsigned int Index2;
	int Min;
	int Max;

	RNGCallSyncLogEvent() : SyncLogEvent() { }

	RNGCallSyncLogEvent(int Type, bool IsCritical, unsigned int Index1, unsigned int Index2, unsigned int Caller, unsigned int Frame, int Min, int Max)
		: Type(Type), IsCritical(IsCritical), Index1(Index1), Index2(Index2), Min(Min), Max(Max), SyncLogEvent(Caller, Frame)
	{ }
};

struct FacingChangeSyncLogEvent : SyncLogEvent
{
	unsigned short Facing;

	FacingChangeSyncLogEvent() : SyncLogEvent() { }

	FacingChangeSyncLogEvent(unsigned short Facing, unsigned int Caller, unsigned int Frame)
		: Facing(Facing), SyncLogEvent(Caller, Frame)
	{ }
};

struct TargetChangeSyncLogEvent : SyncLogEvent
{
	AbstractType Type;
	DWORD ID;
	AbstractType TargetType;
	DWORD TargetID;

	TargetChangeSyncLogEvent() = default;

	TargetChangeSyncLogEvent(const AbstractType& Type, const DWORD& ID, const AbstractType& TargetType, const DWORD& TargetID, unsigned int Caller, unsigned int Frame)
		: Type(Type), ID(ID), TargetType(TargetType), TargetID(TargetID), SyncLogEvent(Caller, Frame)
	{ }
};

struct MissionOverrideSyncLogEvent : SyncLogEvent
{
	AbstractType Type;
	DWORD ID;
	int Mission;

	MissionOverrideSyncLogEvent() : SyncLogEvent() { }

	MissionOverrideSyncLogEvent(const AbstractType& Type, const DWORD& ID, int Mission, unsigned int Caller, unsigned int Frame)
		: Type(Type), ID(ID), Mission(Mission), SyncLogEvent(Caller, Frame)
	{ }
};

struct AnimCreationSyncLogEvent : SyncLogEvent
{
	CoordStruct Coords;

	AnimCreationSyncLogEvent() : SyncLogEvent() { }

	AnimCreationSyncLogEvent(const CoordStruct& Coords, unsigned int Caller, unsigned int Frame)
		: Coords(Coords), SyncLogEvent(Caller, Frame)
	{ }
};

// A techno about to take damage: what it is, what hits it and from whom, and whether it already
// carries its damage smoke (a new one draws from the match random generator).
struct DamageSyncLogEvent : SyncLogEvent
{
	AbstractType Type;
	DWORD ID;
	int TypeIndex;
	int Health;
	int Damage;
	int Warhead;
	AbstractType AttackerType;
	DWORD AttackerID;
	bool HasDamageSmoke;

	DamageSyncLogEvent() : SyncLogEvent() { }

	DamageSyncLogEvent(const AbstractType& Type, const DWORD& ID, int TypeIndex, int Health, int Damage, int Warhead,
		const AbstractType& AttackerType, const DWORD& AttackerID, bool HasDamageSmoke, unsigned int Caller, unsigned int Frame)
		: Type(Type), ID(ID), TypeIndex(TypeIndex), Health(Health), Damage(Damage), Warhead(Warhead), AttackerType(AttackerType)
		, AttackerID(AttackerID), HasDamageSmoke(HasDamageSmoke), SyncLogEvent(Caller, Frame)
	{ }
};

// A projectile a techno is about to launch: from where, how fast and at what, with the stance of the
// firer its launch point is computed from (facings, and the tilt angles, which are floats). The
// velocity and the angles are kept whole and written as raw bits: the LAN desync of 2026-11-26
// (two homing missiles, the first of them one lepton off at its launch) left no trace of which
// of these differed.
struct BulletLaunchSyncLogEvent : SyncLogEvent
{
	DWORD ID;
	int TypeIndex;
	CoordStruct Coords;
	double Velocity[3];
	AbstractType TargetType;
	DWORD TargetID;
	CoordStruct TargetCoords;
	AbstractType FirerType;
	DWORD FirerID;
	CoordStruct FirerCoords;
	unsigned short FirerFacing;
	unsigned short FirerTurretFacing;
	float FirerTiltSideways;
	float FirerTiltForwards;

	BulletLaunchSyncLogEvent() : SyncLogEvent() { }

	BulletLaunchSyncLogEvent(unsigned int Caller, unsigned int Frame) : SyncLogEvent(Caller, Frame)
	{ }
};

// A homing projectile (ROT above 0) at the start of one frame of its flight: where it is, how fast
// it goes and the point it steers at.
struct BulletStepSyncLogEvent : SyncLogEvent
{
	DWORD ID;
	int TypeIndex;
	CoordStruct Coords;
	double Velocity[3];
	AbstractType TargetType;
	DWORD TargetID;
	CoordStruct TargetCoords;

	BulletStepSyncLogEvent() : SyncLogEvent() { }

	BulletStepSyncLogEvent(unsigned int Caller, unsigned int Frame) : SyncLogEvent(Caller, Frame)
	{ }
};

class SyncLogger
{
private:
	static SyncLogEventBuffer<RNGCallSyncLogEvent, RNGCalls_Size> RNGCalls;
	static SyncLogEventBuffer<FacingChangeSyncLogEvent, FacingChanges_Size> FacingChanges;
	static SyncLogEventBuffer<TargetChangeSyncLogEvent, TargetChanges_Size> TargetChanges;
	static SyncLogEventBuffer<TargetChangeSyncLogEvent, DestinationChanges_Size> DestinationChanges;
	static SyncLogEventBuffer<MissionOverrideSyncLogEvent, MissionOverrides_Size> MissionOverrides;
	static SyncLogEventBuffer<AnimCreationSyncLogEvent, AnimCreations_Size> AnimCreations;
	static SyncLogEventBuffer<DamageSyncLogEvent, DamageEvents_Size> DamageEvents;
	static SyncLogEventBuffer<BulletLaunchSyncLogEvent, BulletLaunches_Size> BulletLaunches;
	static SyncLogEventBuffer<BulletStepSyncLogEvent, BulletSteps_Size> BulletSteps;

	static void WriteRNGCalls(FILE* const pLogFile, int frameDigits);
	static void WriteFacingChanges(FILE* const pLogFile, int frameDigits);
	static void WriteTargetChanges(FILE* const pLogFile, int frameDigits);
	static void WriteDestinationChanges(FILE* const pLogFile, int frameDigits);
	static void WriteMissionOverrides(FILE* const pLogFile, int frameDigits);
	static void WriteAnimCreations(FILE* const pLogFile, int frameDigits);
	static void WriteDamageEvents(FILE* const pLogFile, int frameDigits);
	static void WriteBulletLaunches(FILE* const pLogFile, int frameDigits);
	static void WriteBulletSteps(FILE* const pLogFile, int frameDigits);
	static void WriteTeams(FILE* const pLogFile);
public:
	static int AnimCreations_HighestX;
	static int AnimCreations_HighestY;
	static int AnimCreations_HighestZ;
	static int TeamTypeClass_MaxIDLength;
	static int ScriptTypeClass_MaxIDLength;
	static int HouseTypeClass_MaxIDLength;
	static int HouseName_MaxIDLength;

	static void AddRNGCallSyncLogEvent(Randomizer* pRandomizer, int type, unsigned int callerAddress, int min = 0, int max = 0);
	static void AddFacingChangeSyncLogEvent(unsigned short facing, unsigned int callerAddress);
	static void AddTargetChangeSyncLogEvent(AbstractClass* pObject, AbstractClass* pTarget, unsigned int callerAddress);
	static void AddDestinationChangeSyncLogEvent(AbstractClass* pObject, AbstractClass* pTarget, unsigned int callerAddress);
	static void AddMissionOverrideSyncLogEvent(AbstractClass* pObject, int mission, unsigned int callerAddress);
	static void AddAnimCreationSyncLogEvent(const CoordStruct& coords, unsigned int callerAddress);
	static void AddDamageSyncLogEvent(TechnoClass* pTarget, int damage, WarheadTypeClass* pWarhead, TechnoClass* pAttacker, unsigned int callerAddress);
	static void AddBulletLaunchSyncLogEvent(BulletClass* pBullet, const CoordStruct& coords, double velocityX, double velocityY, double velocityZ);
	static void AddBulletStepSyncLogEvent(BulletClass* pBullet);
	static void WriteSyncLog(const char* logFilename);
	static void SetTeamLoggingPadding(TeamClass* pTeam);
};

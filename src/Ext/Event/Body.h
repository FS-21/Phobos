#pragma once
#include <cstddef>
#include <stdint.h>
#include <TechnoClass.h>
#include <FootClass.h>

enum class EventTypeExt : uint8_t
{
	// Vanilla game used Events from 0x00 to 0x2F
	// CnCNet reserved Events from 0x30 to 0x3F
	// Ares used Events 0x60 and 0x61

	SyncTechnoTargetingAndNav = 0x40,
	SyncTechnoSetTarget = 0x41,
	SyncTechnoTarget = 0x42,
	SyncTechnoSetDestination = 0x43,
	SyncTechnoDestination = 0x44,
	SyncEngineerGuardDestination = 0x45,
	SyncTechnoStopMoving = 0x46,
	SyncTechnoCurrentMission = 0x47,
	SyncWeaponizedEngineerGuard = 0x48,
	SyncStopTarNav = 0x49,

	FIRST = SyncTechnoTargetingAndNav,
	LAST = SyncStopTarNav
};

#pragma pack(push, 1)
class EventExt
{
public:
	EventTypeExt Type;
	bool IsExecuted;
	char HouseIndex;
	uint32_t Frame;
	union
	{
		char DataBuffer[104];

		struct SyncTechnoTargetingAndNav
		{
			int TechnoUniqueID;
			AbstractClass* Target;
			AbstractClass* Destination;
			Mission Mission;
		} SyncTechnoTargetingAndNav;

		struct SyncTechnoSetTarget
		{
			int TechnoUniqueID;
			AbstractClass* Target;
		} SyncTechnoSetTarget;

		struct SyncTechnoTarget
		{
			int TechnoUniqueID;
			AbstractClass* Target;
		} SyncTechnoTarget;

		struct SyncTechnoSetDestination
		{
			int TechnoUniqueID;
			AbstractClass* Destination;
			bool RunNow;
		} SyncTechnoSetDestination;

		struct SyncTechnoDestination
		{
			int TechnoUniqueID;
			AbstractClass* Destination;
		} SyncTechnoDestination;

		struct SyncEngineerGuardDestination
		{
			int TechnoUniqueID;
			AbstractClass* GuardDestination;
		} SyncEngineerGuardDestination;

		struct SyncTechnoStopMoving
		{
			int TechnoUniqueID;
		} SyncTechnoStopMoving;

		struct SyncTechnoCurrentMission
		{
			int TechnoUniqueID;
			Mission CurrentMission;
		} SyncTechnoCurrentMission;

		struct SyncWeaponizedEngineerGuard
		{
			int TechnoUniqueID;
		} SyncWeaponizedEngineerGuard;

		struct SyncStopTarNav
		{
			int TechnoUniqueID;
		} SyncStopTarNav;
	};

	bool AddEvent();
	void RespondEvent();

	static size_t GetDataSize(EventTypeExt type);
	static bool IsValidType(EventTypeExt type);
};

static_assert(sizeof(EventExt) == 111);
static_assert(offsetof(EventExt, DataBuffer) == 7);
#pragma pack(pop)

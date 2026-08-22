#include "Body.h"

#include <BuildingClass.h>
#include <InfantryClass.h>
#include <HouseClass.h>

#include <Ext/Scenario/Body.h>
#include <Ext/Techno/Body.h>

//Static init
TriggerExt::ExtContainer TriggerExt::ExtMap;

// =============================
// load / save

template <typename T>
void TriggerExt::Serialize(T& Stm)
{
	Stm
		.Process(this->SortedEventsList)
		.Process(this->SequentialTimers)
		.Process(this->SequentialTimersOriginalValue)
		.Process(this->ParallelTimers)
		.Process(this->ParallelTimersOriginalValue)
		.Process(this->SequentialSwitchModeIndex)
		;
}

void TriggerExt::LoadFromStream(PhobosStreamReader& Stm)
{
	AbstractExt::LoadFromStream(Stm);
	this->Serialize(Stm);
}

void TriggerExt::SaveToStream(PhobosStreamWriter& Stm)
{
	AbstractExt::SaveToStream(Stm);
	this->Serialize(Stm);
}

// =============================
// container

TriggerExt::ExtContainer::ExtContainer() : Container("TriggerClass") { }

TriggerExt::ExtContainer::~ExtContainer() = default;

// =============================
// container hooks

DEFINE_HOOK(0x7260C8, TriggerClass_CTOR, 0x8)
{
	GET(TriggerClass*, pItem, ESI);

	TriggerExt::ExtMap.TryAllocate(pItem);

	return 0;
}

DEFINE_HOOK(0x72617D, TriggerClass_DTOR, 0xF)
{
	GET(TriggerClass*, pItem, ESI);

	TriggerExt::ExtMap.Remove(pItem);

	return 0;
}

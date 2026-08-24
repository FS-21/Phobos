
#include "Body.h"

#include <Ext/House/Body.h>
#include <Ext/Rules/Body.h>
#include <AITriggerTypeClass.h>

#include <Helpers/Macro.h>
#include <ShapeButtonClass.h>

bool EventExt::AddEvent()
{
	return EventClass::OutList.Add(*reinterpret_cast<EventClass*>(this));
}

void EventExt::RespondEvent()
{
	switch (this->Type)
	{
	case EventTypeExt::ApproachObject:
		this->RespondApproachObject();
		break;
	case EventTypeExt::TogglePlayerAutoRepair:
		this->RespondToTogglePlayerAutoRepair();
		break;
	case EventTypeExt::AILearningSync:
		this->RespondToAILearningSync();
		break;
	default:
		break;
	}
}

void EventExt::RaiseTogglePlayerAutoRepair()
{
	EventExt eventExt {};
	eventExt.Type = EventTypeExt::TogglePlayerAutoRepair;
	eventExt.HouseIndex = (char)HouseClass::CurrentPlayer->ArrayIndex;
	eventExt.Frame = Unsorted::CurrentFrame;
	eventExt.AddEvent();
	Debug::LogGame("Adding event TOGGLE_PLAYER_AUTOREPAIR\n");
}

void EventExt::RaiseAILearningSync(const std::vector<std::pair<uint16_t, float>>& triggers)
{
	if (triggers.empty())
		return;

	const size_t maxPerPacket = 16;
	for (size_t i = 0; i < triggers.size(); i += maxPerPacket)
	{
		EventExt eventExt {};
		eventExt.Type = EventTypeExt::AILearningSync;
		eventExt.HouseIndex = HouseClass::CurrentPlayer ? static_cast<char>(HouseClass::CurrentPlayer->ArrayIndex) : 0;
		eventExt.Frame = Unsorted::CurrentFrame;

		uint8_t count = 0;
		for (size_t j = i; j < triggers.size() && j < i + maxPerPacket; ++j)
		{
			eventExt.AILearningSync.Entries[count].TriggerIndex = triggers[j].first;
			eventExt.AILearningSync.Entries[count].Weight = triggers[j].second;
			count++;
		}
		eventExt.AILearningSync.Count = count;
		eventExt.AddEvent();
		Debug::LogGame("Adding event AI_LEARNING_SYNC (%u triggers)\n", count);
	}
}

void EventExt::RespondToAILearningSync()
{
	if (!RulesExt::Global()->AILearning || !RulesExt::Global()->AILearning_Multiplayer.Get())
		return;

	for (uint8_t i = 0; i < this->AILearningSync.Count && i < 16; ++i)
	{
		uint16_t idx = this->AILearningSync.Entries[i].TriggerIndex;
		float weight = this->AILearningSync.Entries[i].Weight;

		if (idx < AITriggerTypeClass::Array.Count)
		{
			auto pTrigger = AITriggerTypeClass::Array.GetItem(idx);
			if (pTrigger)
			{
				pTrigger->Weight_Current = static_cast<double>(weight);
				Debug::LogGame("AI Learning Sync - Trigger [%s] set to weight %.2f\n", pTrigger->ID, pTrigger->Weight_Current);
			}
		}
	}
}

size_t EventExt::GetDataSize(EventTypeExt type)
{
	switch (type)
	{
	case EventTypeExt::ApproachObject:
		return sizeof(EventExt::ApproachObject);
	case EventTypeExt::TogglePlayerAutoRepair:
		return sizeof(EventExt::TogglePlayerAutoRepair);
	case EventTypeExt::AILearningSync:
		return sizeof(EventExt::AILearningSync);
	default:
		break;
	}

	return 0;
}

bool EventExt::IsValidType(EventTypeExt type)
{
	return (type >= EventTypeExt::FIRST && type <= EventTypeExt::LAST);
}

void EventExt::RespondApproachObject()
{
	const auto pSource = this->ApproachObject.Whom.As_Foot();

	if (!pSource || static_cast<char>(pSource->Owner->ArrayIndex) != this->HouseIndex)
		return;

	pSource->ClearPlanningTokens(nullptr);

	if (!pSource->IsAlive || pSource->Health <= 0 || pSource->InLimbo)
		return;

	if (pSource->IsTether)
	{
		const auto pLink = abstract_cast<BuildingClass*>(pSource->GetNthLink());

		if (pLink && pLink->IsAlive && pLink->Type->DockUnload)
		{
			pSource->SendToFirstLink(RadioCommand::NotifyUnlink);
			pSource->IsTether = false;
		}
	}
	else
	{
		pSource->SendToFirstLink(RadioCommand::NotifyUnlink);
	}

	pSource->QueueUpToEnter = nullptr;
	pSource->LastDestination = nullptr;

	if (const auto pManager = pSource->SlaveManager)
		pManager->AllGuard();

	pSource->ClearNavigationList();
	pSource->SetDestination(nullptr, true);
	// According to the report at https://github.com/Phobos-developers/Phobos/pull/2134#issuecomment-4062110663:
	// If the target is not cleared here, it may cause desync. The specific reason has not been fully investigated.
	// Anyone is welcome to provide a more detailed explanation.
	pSource->SetTarget(nullptr);
	pSource->SetArchiveTarget(nullptr);

	const auto pObject = this->ApproachObject.Target.As_Object();

	if (!pObject)
		return;

	pSource->Target = pObject;
	pSource->ApproachTarget(0);
	pSource->Target = nullptr;
}

void EventExt::RespondToTogglePlayerAutoRepair()
{
	if (this->HouseIndex >= HouseClass::Array.Count)
		return;

	if (!RulesExt::Global()->ExtendedPlayerRepair)
		return;

	auto pHouse = HouseClass::Array.GetItem(this->HouseIndex);
	auto pHouseExt = HouseExt::Fetch(pHouse);
	pHouseExt->PlayerAutoRepair = !pHouseExt->PlayerAutoRepair;

	if (HouseClass::CurrentPlayer == pHouse)
	{
		SidebarClass::Instance.SidebarNeedsRedraw = true;

		if (pHouseExt->PlayerAutoRepair)
			SidebarClass::ToggleRepairButton.TurnOn();
		else
			SidebarClass::ToggleRepairButton.TurnOff();
	}
}

// hooks

DEFINE_HOOK(0x4C6CC8, Networking_RespondToEvent, 0x5)
{
	GET(EventExt*, pEvent, ESI);

	if (EventExt::IsValidType(pEvent->Type))
		pEvent->RespondEvent();

	return 0;
}

DEFINE_HOOK(0x64B6FE, sub_64B660_GetEventSize, 0x6)
{
	const auto eventType = static_cast<EventTypeExt>(R->EDI() & 0xFF);

	if (EventExt::IsValidType(eventType))
	{
		const size_t eventSize = EventExt::GetDataSize(eventType);

		R->EDX(eventSize);
		R->EBP(eventSize);
		return 0x64B71D;
	}

	return 0;
}

DEFINE_HOOK(0x64BE7D, sub_64BDD0_GetEventSize1, 0x6)
{
	const auto eventType = static_cast<EventTypeExt>(R->EDI() & 0xFF);

	if (EventExt::IsValidType(eventType))
	{
		const size_t eventSize = EventExt::GetDataSize(eventType);

		REF_STACK(size_t, eventSizeInStack, STACK_OFFSET(0xAC, -0x8C));
		eventSizeInStack = eventSize;
		R->ECX(eventSize);
		R->EBP(eventSize);
		return 0x64BE97;
	}

	return 0;
}

DEFINE_HOOK(0x64C30E, sub_64BDD0_GetEventSize2, 0x6)
{
	const auto eventType = static_cast<EventTypeExt>(R->ESI() & 0xFF);

	if (EventExt::IsValidType(eventType))
	{
		const size_t eventSize = EventExt::GetDataSize(eventType);

		R->ECX(eventSize);
		R->EBP(eventSize);
		return 0x64C321;
	}

	return 0;
}


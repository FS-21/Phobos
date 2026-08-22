#pragma once
#include <TriggerClass.h>
#include <timer.h>

#include <map>

#include <Utilities/Container.h>
#include <Utilities/Constructs.h>
#include <Utilities/Template.h>
#include <Utilities/TemplateDef.h>

class TriggerExt final : public AbstractExt
{
public:
	using base_type = TriggerClass;

	// deprecated: the pre-rework nested data class is now the extension class itself
	using ExtData [[deprecated("use the extension class itself instead")]] = TriggerExt;

	static constexpr DWORD Canary = 0x73171331;

public:
	// typed owner accessor
	TriggerClass* OwnerObject() const
	{
		return static_cast<TriggerClass*>(this->GetAttachedObject());
	}

	std::vector<TEventClass*> SortedEventsList;
	PhobosMap<int, CDTimerClass> SequentialTimers;
	std::map<int, int> SequentialTimersOriginalValue;
	PhobosMap<int, CDTimerClass> ParallelTimers;
	std::map<int, int> ParallelTimersOriginalValue;
	int SequentialSwitchModeIndex { -1 };

	TriggerExt(TriggerClass* const OwnerObject) : AbstractExt(OwnerObject)
		, SortedEventsList {}
		, SequentialTimers {}
		, SequentialTimersOriginalValue {}
		, ParallelTimers {}
		, ParallelTimersOriginalValue {}
		, SequentialSwitchModeIndex { -1 }
	{ }

	virtual ~TriggerExt() = default;

	virtual void LoadFromStream(PhobosStreamReader& Stm) override;
	virtual void SaveToStream(PhobosStreamWriter& Stm) override;

private:
	template <typename T>
	void Serialize(T& Stm);

public:
	class ExtContainer final : public Container<TriggerExt>
	{
	public:
		ExtContainer();
		~ExtContainer();
	};

	static ExtContainer ExtMap;

	static TriggerExt* Fetch(const TriggerClass* pThis)
	{
		return AbstractExt::Fetch<TriggerExt>(pThis);
	}

	static TriggerExt* TryFetch(const TriggerClass* pThis)
	{
		return AbstractExt::TryFetch<TriggerExt>(pThis);
	}
};

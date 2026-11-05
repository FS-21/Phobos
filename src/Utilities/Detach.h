#pragma once

#include <vector>

class AbstractClass;

// Typed pointer-invalidation registry.
// An extension (or a static aggregate for high-volume types) subscribes to deletions
// of a game type T by inheriting Detach::Listener<T> and overriding OnDetach.
// The game's Detach_This_From_All funnel (hook at 0x7258D0) dispatches the dying
// object to Registry<T>::Notify for its concrete type and every base of it.
namespace Detach
{
	template <typename T>
	class Listener;

	template <typename T>
	class Registry final
	{
	public:
		Registry() = delete;

		static void Add(Listener<T>* const pListener)
		{
			auto& listeners = Listeners();
			pListener->RegistryIndex = listeners.size();
			listeners.push_back(pListener);
		}

		static void Remove(Listener<T>* const pListener)
		{
			auto& listeners = Listeners();
			const size_t index = pListener->RegistryIndex;
			listeners[index] = listeners.back();
			listeners[index]->RegistryIndex = index;
			listeners.pop_back();
		}

		static void Notify(T* const pTarget, const bool removed)
		{
			auto& listeners = Listeners();

			// backwards: a listener may unregister itself from within OnDetach
			for (size_t i = listeners.size(); i-- > 0;)
				listeners[i]->OnDetach(pTarget, removed);
		}

	private:
		// Allocated on first use and never freed: listeners with static storage unregister from
		// their destructors at process exit, and the destruction order of statics across
		// translation units is unspecified, so a static vector could already be gone by then.
		static std::vector<Listener<T>*>& Listeners()
		{
			static std::vector<Listener<T>*>* const pListeners = new std::vector<Listener<T>*>();
			return *pListeners;
		}
	};

	template <typename T>
	class Listener
	{
	public:
		Listener()
		{
			Registry<T>::Add(this);
		}

		Listener(const Listener&) = delete;
		Listener& operator=(const Listener&) = delete;

		virtual ~Listener()
		{
			Registry<T>::Remove(this);
		}

		virtual void OnDetach(T* pTarget, bool removed) = 0;

	private:
		size_t RegistryIndex;

		friend class Registry<T>;
	};

	void NotifyAbstract(AbstractClass* pTarget, bool removed);
}

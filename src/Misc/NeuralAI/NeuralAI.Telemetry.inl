// NeuralAI.Telemetry.inl: Trainer telemetry snapshot (RAM only).
// Part of NeuralAI.cpp (included in order; one translation unit).

namespace
{
	// NeuralAI.Preservation.inl (included later).
	void FormatArmyTelemetry(const HouseClass* pHouse, char* pBuffer, size_t size);

	using CreditWeights = std::array<std::array<int64_t, NeuralAI::NumSpecialists>, NumRoutingPhases>;

	void AddExpertCredit(int idx, int expert, int phase, int64_t amount, NeuralCreditPart part, int64_t& attributed)
	{
		if (amount == 0)
			return;

		expert = std::clamp(expert, 0, NeuralAI::NumSpecialists - 1);
		phase = std::clamp(phase, 0, NumRoutingPhases - 1);
		ExpertCredit[idx][expert] += amount;
		ExpertCreditByPhase[idx][phase][expert] += amount;
		ExpertCreditParts[idx][expert][static_cast<size_t>(part)] += amount;
		attributed += amount;
	}

	// Splits an amount over (phase, expert) cells by weight, exactly: the rounding remainder goes
	// to the heaviest cell (the first one on ties). False when there is no weight.
	bool SplitExpertCredit(int idx, int64_t amount, const CreditWeights& weights, NeuralCreditPart part, int64_t& attributed)
	{
		int64_t total = 0;
		int bestPhase = 0;
		int bestExpert = 0;

		for (int p = 0; p < NumRoutingPhases; ++p)
		{
			for (int e = 0; e < NeuralAI::NumSpecialists; ++e)
			{
				total += weights[p][e];

				if (weights[p][e] > weights[bestPhase][bestExpert])
				{
					bestPhase = p;
					bestExpert = e;
				}
			}
		}

		if (total <= 0)
			return false;

		int64_t given = 0;

		for (int p = 0; p < NumRoutingPhases; ++p)
		{
			for (int e = 0; e < NeuralAI::NumSpecialists; ++e)
			{
				if (weights[p][e] <= 0)
					continue;

				const int64_t share = amount * weights[p][e] / total;
				AddExpertCredit(idx, e, p, share, part, attributed);
				given += share;
			}
		}

		AddExpertCredit(idx, bestExpert, bestPhase, amount - given, part, attributed);
		return true;
	}

	// Decision credit sample (see NeuralCreditPart), every 60 frames with the telemetry. The
	// advantage is the one of SampleExpertGains, in credits.
	void SampleExpertCredit()
	{
		if (Unsorted::CurrentFrame == CreditLastFrame)
			return;

		CreditLastFrame = Unsorted::CurrentFrame;
		const int serial = ++CreditSerial;
		const int now = Unsorted::CurrentFrame;
		const int nowPhase = RoutingPhase(now);

		struct Newcomer
		{
			DWORD ID;
			TechnoClass* Techno;
		};

		// An own building that gives a free unit when placed.
		struct FreeUnitSource
		{
			DWORD ID;
			TechnoTypeClass* FreeUnit;
			CoordStruct Location;
		};

		std::array<int64_t, 32> assets {};
		std::array<bool, 32> tracked {};
		std::array<int, 32> active {};
		std::array<CreditWeights, 32> spendWeights {};
		std::array<CreditWeights, 32> incomeWeights {};
		std::array<std::vector<Newcomer>, 32> newcomers {};
		std::array<std::vector<FreeUnitSource>, 32> freeUnitSources {};
		std::array<std::vector<DWORD>, 32> producing {};

		for (auto const pHouse : HouseClass::Array)
		{
			if (!pHouse || pHouse->ArrayIndex < 0 || pHouse->ArrayIndex >= 32)
				continue;

			const int idx = pHouse->ArrayIndex;
			assets[idx] += std::max(0, static_cast<int>(pHouse->Available_Money())) / 2;
			tracked[idx] = !pHouse->IsNeutral() && !pHouse->Defeated && NeuralAI::IsEnabledForHouse(pHouse);
			active[idx] = CreditActiveExpert(pHouse);
		}

		for (auto const pTechno : TechnoClass::Array)
		{
			if (!pTechno || !pTechno->IsAlive || !pTechno->Owner || pTechno->Owner->ArrayIndex < 0 || pTechno->Owner->ArrayIndex >= 32)
				continue;

			auto const pType = pTechno->GetTechnoType();

			if (!pType)
				continue;

			const int idx = pTechno->Owner->ArrayIndex;
			assets[idx] += std::max(0, static_cast<int>(pType->Cost));

			if (!tracked[idx])
				continue;

			auto const pTag = CreditTagFor(idx, pTechno, active[idx], nowPhase);

			if (!pTag)
				continue;

			pTag->Stamp = serial;
			pTag->Location = pTechno->Location;

			if (!pTag->Credited)
				newcomers[idx].push_back({ pTechno->UniqueID, pTechno });

			if (pTag->Economic)
				incomeWeights[idx][pTag->OriginPhase][pTag->Origin] += pTag->Cost;

			if (auto const pBuilding = abstract_cast<BuildingClass*>(pTechno))
			{
				if (pBuilding->Type->FreeUnit && !pBuilding->InLimbo)
					freeUnitSources[idx].push_back({ pTechno->UniqueID, pBuilding->Type->FreeUnit, pTechno->Location });
			}
		}

		// Money goes into the objects in production (they exist, in limbo, from the start).
		for (auto const pFactory : FactoryClass::Array)
		{
			if (!pFactory || !pFactory->Object || !pFactory->Owner || pFactory->Owner->ArrayIndex < 0 || pFactory->Owner->ArrayIndex >= 32
				|| !tracked[pFactory->Owner->ArrayIndex])
			{
				continue;
			}

			const int idx = pFactory->Owner->ArrayIndex;
			auto const& tags = CreditTags[idx];
			auto const it = tags.find(pFactory->Object->UniqueID);
			producing[idx].push_back(pFactory->Object->UniqueID);

			if (it != tags.end())
				spendWeights[idx][it->second.OriginPhase][it->second.Origin] += std::max(1, it->second.Cost);
		}

		std::vector<NeuralCreditGone> gone;

		for (auto const pHouse : HouseClass::Array)
		{
			if (!pHouse || pHouse->ArrayIndex < 0 || pHouse->ArrayIndex >= 32)
				continue;

			const int idx = pHouse->ArrayIndex;
			auto& tags = CreditTags[idx];

			if (!tracked[idx])
			{
				tags.clear();
				CreditHasLast[idx] = false;
				PendingKillCredit[idx] = {};
				PendingHarvestCredit[idx] = {};
				CreditRecentGone[idx].clear();
				continue;
			}

			// Plan members answer to the expert that started their plan, late recruits included.
			auto const& macro = GetMacroState(pHouse);

			for (size_t p = 0; p < macro.Plans.size(); ++p)
			{
				auto const& plan = macro.Plans[p];
				const int starter = PlanStarterExpert[idx][p];

				if (!plan.Active || starter < 0)
					continue;

				for (const DWORD uid : plan.Members)
				{
					auto const it = tags.find(uid);

					if (it == tags.end())
						continue;

					auto& tag = it->second;

					if (tag.CommandSource != static_cast<int16_t>(p) || tag.CommandFrame < plan.StartFrame)
					{
						tag.Command = static_cast<int8_t>(starter);
						tag.CommandPhase = PlanStarterPhase[idx][p];
						tag.CommandSource = static_cast<int16_t>(p);
						tag.CommandFrame = now;
					}
				}
			}

			// Technos gone since the last sample (killed, sold, captured, deployed), by UniqueID.
			gone.clear();

			for (auto it = tags.begin(); it != tags.end();)
			{
				if (it->second.Stamp != serial)
				{
					if (it->second.Credited)
						gone.push_back({ it->first, it->second, false });

					it = tags.erase(it);
				}
				else
				{
					++it;
				}
			}

			auto const byID = [](auto const& a, auto const& b) { return a.ID < b.ID; };
			std::sort(gone.begin(), gone.end(), byID);
			std::sort(newcomers[idx].begin(), newcomers[idx].end(), byID);
			std::sort(freeUnitSources[idx].begin(), freeUnitSources[idx].end(), byID);
			std::sort(producing[idx].begin(), producing[idx].end());

			// A techno made by another takes over its decision tags (and its share of the income
			// weights when it earns money).
			auto const inherit = [&](NeuralCreditTag& tag, const NeuralCreditTag& from)
			{
				if (tag.Economic)
				{
					incomeWeights[idx][tag.OriginPhase][tag.Origin] -= tag.Cost;
					incomeWeights[idx][from.OriginPhase][from.Origin] += tag.Cost;
				}

				tag.Origin = from.Origin;
				tag.OriginPhase = from.OriginPhase;
				tag.Command = from.Command;
				tag.CommandPhase = from.CommandPhase;
				tag.CommandSource = from.CommandSource;
				tag.CommandFrame = from.CommandFrame;
			};

			auto const distanceSq = [](const CoordStruct& a, const CoordStruct& b)
			{
				const int64_t dx = a.X - b.X;
				const int64_t dy = a.Y - b.Y;
				return dx * dx + dy * dy;
			};

			const int64_t rangeSq = static_cast<int64_t>(CreditParentRange) * CreditParentRange;

			for (auto const& n : newcomers[idx])
			{
				auto& tag = tags[n.ID];

				// Production started under the expert in charge, which the tag already says.
				if (std::binary_search(producing[idx].begin(), producing[idx].end(), n.ID))
					continue;

				// A deployer hands its tags to what it deploys into (MCV to construction yard, slave
				// miner to refinery and back).
				bool found = false;

				for (auto& g : gone)
				{
					if (g.Transformed || !g.Tag.Type)
						continue;

					const bool deployed = g.Tag.Type->DeploysInto && static_cast<TechnoTypeClass*>(g.Tag.Type->DeploysInto) == tag.Type;
					const bool undeployed = g.Tag.Type->UndeploysInto && static_cast<TechnoTypeClass*>(g.Tag.Type->UndeploysInto) == tag.Type;

					if (!deployed && !undeployed)
						continue;

					inherit(tag, g.Tag);
					g.Transformed = true;
					found = true;
					break;
				}

				if (found)
					continue;

				// Spawns, mind controlled technos and slaves belong to whoever made or took them.
				auto const pTechno = n.Techno;
				TechnoClass* pParent = pTechno->SpawnOwner;

				if (!pParent)
					pParent = pTechno->MindControlledBy;

				if (!pParent)
					pParent = pTechno->SlaveOwner;

				if (pParent && pParent->IsAlive && pParent->Owner == pHouse)
				{
					auto const it = tags.find(pParent->UniqueID);

					if (it != tags.end())
					{
						inherit(tag, it->second);
						continue;
					}
				}

				// The free unit of a building placed nearby (the harvester of a refinery).
				const NeuralCreditTag* pSource = nullptr;
				int64_t bestSq = rangeSq + 1;

				if (pTechno->WhatAmI() == AbstractType::Unit)
				{
					for (auto const& source : freeUnitSources[idx])
					{
						const int64_t sq = distanceSq(source.Location, pTechno->Location);

						if (source.FreeUnit != tag.Type || sq >= bestSq)
							continue;

						auto const it = tags.find(source.ID);

						if (it != tags.end())
						{
							pSource = &it->second;
							bestSq = sq;
						}
					}
				}

				// Otherwise an own techno gone nearby, now or at the last sample: the wreck its
				// crew left, the engineer that captured it.
				if (!pSource)
				{
					for (auto const pList : { &gone, &CreditRecentGone[idx] })
					{
						for (auto const& g : *pList)
						{
							const int64_t sq = distanceSq(g.Tag.Location, pTechno->Location);

							if (!g.Transformed && g.Tag.Type && sq < bestSq)
							{
								pSource = &g.Tag;
								bestSq = sq;
							}
						}
					}
				}

				if (pSource)
					inherit(tag, *pSource);
			}

			int ticksSum = 0;

			for (const int t : ExpertTicks[idx])
				ticksSum += t;

			// Like expertGain: nothing is credited before the house has a previous sample and has
			// chosen an expert, so the starting units are nobody's decision.
			const bool crediting = CreditHasLast[idx] && ticksSum > 0;
			int64_t attributed = 0;

			// Eligibility trace of the expert choices since the last sample.
			CreditWeights traceWeights {};

			for (int e = 0; e < NeuralAI::NumSpecialists; ++e)
			{
				const int choices = std::max(0, ExpertTicks[idx][e] - CreditTraceTicks[idx][e]);
				CreditTrace[idx][e] = CreditTrace[idx][e] * 7 / 8 + choices * 64;
				CreditTraceTicks[idx][e] = ExpertTicks[idx][e];
				traceWeights[nowPhase][e] = CreditTrace[idx][e];
			}

			// Books an amount to the recently active experts, the expert in charge without a trace.
			auto const addToTrace = [&](int64_t amount, NeuralCreditPart part)
			{
				if (!SplitExpertCredit(idx, amount, traceWeights, part, attributed))
					AddExpertCredit(idx, active[idx], nowPhase, amount, part, attributed);
			};

			for (auto const& n : newcomers[idx])
			{
				auto& tag = tags[n.ID];

				if (crediting)
					AddExpertCredit(idx, tag.Origin, tag.OriginPhase, tag.Cost, NeuralCreditPart::Built, attributed);

				tag.Credited = true;
			}

			if (crediting)
			{
				for (auto const& g : gone)
				{
					const int64_t loss = -static_cast<int64_t>(g.Tag.Cost);

					if (!g.Transformed && g.Tag.Command >= 0)
					{
						AddExpertCredit(idx, g.Tag.Command, g.Tag.CommandPhase, loss, NeuralCreditPart::Lost, attributed);
					}
					else if (!g.Transformed && g.Tag.KilledByEnemy)
					{
						// Nobody sent it there: placing it was one decision, letting the enemy
						// through was another.
						AddExpertCredit(idx, g.Tag.Origin, g.Tag.OriginPhase, loss / 2, NeuralCreditPart::Lost, attributed);
						addToTrace(loss - loss / 2, NeuralCreditPart::Lost);
					}
					else
					{
						AddExpertCredit(idx, g.Tag.Origin, g.Tag.OriginPhase, loss, NeuralCreditPart::Lost, attributed);
					}
				}
			}

			const int halfMoney = std::max(0, static_cast<int>(pHouse->Available_Money())) / 2;
			const int spent = pHouse->CreditsSpent;

			if (crediting)
			{
				// Half the money counts in the advantage: what was paid is half the spending, the
				// rest of the change of half the money is income (sales and refunds included).
				const int64_t spendPart = -static_cast<int64_t>(std::max(0, spent - CreditLastSpent[idx]) / 2);
				const int64_t incomePart = static_cast<int64_t>(halfMoney - CreditLastHalfMoney[idx]) - spendPart;

				SplitExpertCredit(idx, spendPart, spendWeights[idx], NeuralCreditPart::Spent, attributed);

				if (incomePart > 0)
				{
					// Ore unloaded goes to the authors of the gatherer and the dock, the rest of
					// the income (cash buildings, sales, refunds) by the economic technos' cost.
					int64_t harvested = 0;

					for (auto const& byExpert : PendingHarvestCredit[idx])
					{
						for (const int64_t money : byExpert)
							harvested += money;
					}

					const int64_t harvestPart = std::min(incomePart, harvested / 2);
					int64_t rest = incomePart;

					if (harvestPart > 0 && SplitExpertCredit(idx, harvestPart, PendingHarvestCredit[idx], NeuralCreditPart::Income, attributed))
						rest -= harvestPart;

					SplitExpertCredit(idx, rest, incomeWeights[idx], NeuralCreditPart::Income, attributed);
				}
			}

			PendingHarvestCredit[idx] = {};
			int64_t enemySum = 0;
			int enemies = 0;

			for (auto const pOther : HouseClass::Array)
			{
				if (pOther && pOther != pHouse && !pOther->IsNeutral() && !pOther->Defeated && !pHouse->IsAlliedWith(pOther)
					&& pOther->ArrayIndex >= 0 && pOther->ArrayIndex < 32)
				{
					enemySum += assets[pOther->ArrayIndex];
					enemies++;
				}
			}

			if (crediting && enemies > 0)
			{
				for (int p = 0; p < NumRoutingPhases; ++p)
				{
					for (int e = 0; e < NeuralAI::NumSpecialists; ++e)
						AddExpertCredit(idx, e, p, PendingKillCredit[idx][p][e] / enemies, NeuralCreditPart::Kills, attributed);
				}
			}

			PendingKillCredit[idx] = {};
			const int64_t advantage = assets[idx] - (enemies > 0 ? enemySum / enemies : 0);

			if (crediting)
				addToTrace(advantage - CreditLastAdvantage[idx] - attributed, NeuralCreditPart::Shared);

			CreditLastAdvantage[idx] = advantage;
			CreditLastHalfMoney[idx] = halfMoney;
			CreditLastSpent[idx] = spent;
			CreditHasLast[idx] = true;
			CreditRecentGone[idx] = gone;
		}
	}
}

void NeuralAI::OnTechnoKilled(TechnoClass* pVictim, TechnoClass* pKiller)
{
	if (!NeuralAI::HeadlessMode || !pVictim || !pKiller)
		return;

	// A spawned missile or aircraft kills for its launcher.
	if (pKiller->SpawnOwner && pKiller->SpawnOwner->IsAlive)
		pKiller = pKiller->SpawnOwner;

	auto const pKillerHouse = pKiller->Owner;
	auto const pVictimHouse = pVictim->Owner;

	if (!pKillerHouse || !pVictimHouse || pKillerHouse == pVictimHouse || pKillerHouse->IsAlliedWith(pVictimHouse)
		|| pKillerHouse->ArrayIndex < 0 || pKillerHouse->ArrayIndex >= 32 || pVictimHouse->ArrayIndex < 0 || pVictimHouse->ArrayIndex >= 32)
	{
		return;
	}

	// The victim's loss is booked at the next sample, which needs to know an enemy did it.
	auto& victimTags = CreditTags[pVictimHouse->ArrayIndex];
	auto const victimIt = victimTags.find(pVictim->UniqueID);

	if (victimIt != victimTags.end())
		victimIt->second.KilledByEnemy = true;

	if (pVictimHouse->IsNeutral() || pVictimHouse->Defeated || !NeuralAI::IsEnabledForHouse(pKillerHouse))
		return;

	auto const pType = pVictim->GetTechnoType();

	if (!pType || pType->Cost <= 0)
		return;

	const int idx = pKillerHouse->ArrayIndex;
	int expert = CreditActiveExpert(pKillerHouse);
	int phase = RoutingPhase(Unsorted::CurrentFrame);
	auto const& tags = CreditTags[idx];
	auto const it = tags.find(pKiller->UniqueID);

	if (it != tags.end())
	{
		const bool byCommand = it->second.Command >= 0;
		expert = byCommand ? it->second.Command : it->second.Origin;
		phase = byCommand ? it->second.CommandPhase : it->second.OriginPhase;
	}

	PendingKillCredit[idx][std::clamp(phase, 0, NumRoutingPhases - 1)][std::clamp(expert, 0, NeuralAI::NumSpecialists - 1)] += pType->Cost;
}

void NeuralAI::OnHarvestIncome(HouseClass* pHouse, TechnoClass* pGatherer, TechnoClass* pDock, int money)
{
	if (!NeuralAI::HeadlessMode || !pHouse || money <= 0 || pHouse->ArrayIndex < 0 || pHouse->ArrayIndex >= 32)
		return;

	// The money of an unload goes half to the expert that started the gatherer and half to the
	// one that started the dock; an untagged side leaves its half to the other.
	const int idx = pHouse->ArrayIndex;
	auto const& tags = CreditTags[idx];
	std::array<const NeuralCreditTag*, 2> authors { nullptr, nullptr };
	int count = 0;

	for (auto const pTechno : { pGatherer, pDock })
	{
		if (!pTechno || pTechno->Owner != pHouse)
			continue;

		auto const it = tags.find(pTechno->UniqueID);

		if (it != tags.end())
			authors[count++] = &it->second;
	}

	auto& pending = PendingHarvestCredit[idx];

	if (count == 0)
	{
		pending[RoutingPhase(Unsorted::CurrentFrame)][CreditActiveExpert(pHouse)] += money;
		return;
	}

	const int first = count == 2 ? money / 2 : money;
	pending[authors[0]->OriginPhase][authors[0]->Origin] += first;

	if (count == 2)
		pending[authors[1]->OriginPhase][authors[1]->Origin] += money - first;
}

void NeuralAI::UpdateTelemetrySnapshot(bool matchEnded, int winnerHouseIdx, bool stalemate)
{
	AITrainerSharedTelemetry localSnap = {};

	SampleTerritory(matchEnded);
	ComputeMapInfo();

	AITrainerSharedTelemetry* dst = NeuralAI::TelemetryView ? NeuralAI::TelemetryView : &localSnap;

	std::memcpy(dst->Magic, "PHOBOSTM", 8);

	dst->SchemaVersion = 1;

	dst->SlotIndex = static_cast<uint32_t>(NeuralAI::TrainSlotIndex);

	dst->ProcessId = GetCurrentProcessId();

	dst->CurrentFrame = static_cast<uint32_t>(Unsorted::CurrentFrame);

	dst->FramesPerSecond = static_cast<uint32_t>(NeuralAI::CurrentMeasuredFPS);

	dst->MatchFinished = matchEnded ? 1u : 0u;

	dst->WinnerHouseIndex = winnerHouseIdx;

	dst->StalemateTriggered = stalemate ? 1u : 0u;

	dst->RulesSignature = NeuralAI::CurrentRulesSignature;

	uint32_t hCount = 0;

	for (auto const pHouse : HouseClass::Array)
	{
		if (!pHouse || pHouse->IsNeutral() || pHouse->IsObserver() || hCount >= 8)
			continue;

		auto const pExt = HouseExt::Fetch(pHouse);

		auto& hs = dst->Houses[hCount++];

		hs.ArrayIndex = pHouse->ArrayIndex;

		hs.SideIndex = pHouse->SideIndex;

		hs.CountryIndex = pHouse->Type ? pHouse->Type->ArrayIndex : -1;

		hs.Archetype = pExt->NeuralAI_Archetype;

		hs.ActiveExpert = pExt->NeuralAI_ActiveExpert;

		hs.Credits = pHouse->Available_Money();

		hs.KilledUnits = pHouse->TotalKilledUnits;

		hs.KilledBuildings = pHouse->TotalKilledBuildings;

		hs.OwnedUnits = pHouse->OwnedUnits;

		hs.OwnedBuildings = pHouse->OwnedBuildings;

		hs.OwnedInfantry = pHouse->OwnedInfantry;

		hs.OwnedAircraft = pHouse->OwnedAircraft;

		hs.NetWorthScore = HouseMatchScore(pHouse);

		hs.IsWinner = (pHouse->ArrayIndex == winnerHouseIdx || pHouse->IsWinner) ? 1 : 0;

		hs.IsDefeated = pHouse->Defeated ? 1 : 0;

		hs.PolicySlot = static_cast<uint8_t>(pExt->NeuralAI_PolicySlot);

		hs.ColorR = pHouse->Color.R;

		hs.ColorG = pHouse->Color.G;

		hs.ColorB = pHouse->Color.B;
	}

	dst->NumHouses = hCount;

	std::memset(dst->RadarCells, 0, sizeof(dst->RadarCells));

	int minMapX = 99999;
	int maxMapX = -99999;
	int minMapY = 99999;
	int maxMapY = -99999;

	auto trackBounds = [&](const CellStruct& c)
	{
		if (c.X > 0 && c.Y > 0)
		{
			if (c.X < minMapX)
				minMapX = c.X;

			if (c.X > maxMapX)
				maxMapX = c.X;

			if (c.Y < minMapY)
				minMapY = c.Y;

			if (c.Y > maxMapY)
				maxMapY = c.Y;
		}
	};

	for (auto const pBld : BuildingClass::Array)
	{
		if (!pBld || pBld->InLimbo || !pBld->IsAlive || !pBld->Owner || !pBld->Type || pBld->Type->InvisibleInGame
			|| pBld->Owner->IsNeutral())
		{
			continue;
		}

		if (pBld->Type->CanBeOccupied && !pBld->Type->BaseNormal && !pBld->Type->ConstructionYard
			&& pBld->Type->TechLevel < 0)
		{
			continue;
		}

		const CellStruct cell = pBld->GetMapCoords();

		if (cell.X <= 1 && cell.Y <= 1)
			continue;

		trackBounds(cell);

		const int rx = std::clamp(static_cast<int>(cell.X) / 2, 0, 127);
		const int ry = std::clamp(static_cast<int>(cell.Y) / 2, 0, 127);

		dst->RadarCells[ry * 128 + rx] = static_cast<uint8_t>((pBld->Owner->ArrayIndex & 0x07) + 1 + 0x80);
	}

	for (auto const pUnit : UnitClass::Array)
	{
		if (!pUnit || pUnit->InLimbo || !pUnit->IsAlive || !pUnit->Owner || pUnit->Owner->IsNeutral())
			continue;

		const CellStruct cell = pUnit->GetMapCoords();

		trackBounds(cell);

		const int rx = std::clamp(static_cast<int>(cell.X) / 2, 0, 127);
		const int ry = std::clamp(static_cast<int>(cell.Y) / 2, 0, 127);

		dst->RadarCells[ry * 128 + rx] = static_cast<uint8_t>((pUnit->Owner->ArrayIndex & 0x07) + 1);
	}

	for (auto const pInf : InfantryClass::Array)
	{
		if (!pInf || pInf->InLimbo || !pInf->IsAlive || !pInf->Owner || pInf->Owner->IsNeutral())
			continue;

		trackBounds(pInf->GetMapCoords());
	}

	if (minMapX > maxMapX)
	{
		minMapX = 0;

		maxMapX = 256;

		minMapY = 0;

		maxMapY = 256;
	}
	else
	{
		minMapX = std::max(0, minMapX - 10);

		minMapY = std::max(0, minMapY - 10);

		maxMapX += 10;

		maxMapY += 10;
	}

	const int spanX = std::max(32, maxMapX - minMapX);
	const int spanY = std::max(32, maxMapY - minMapY);
	const int mapW = MapClass::Instance.MapRect.Width;
	const int localX = MapClass::Instance.VisibleRect.X;
	const int localY = MapClass::Instance.VisibleRect.Y;
	const int localW = MapClass::Instance.VisibleRect.Width;
	const int localH = MapClass::Instance.VisibleRect.Height;
	const bool hasIsoBounds = (mapW > 0 && localW > 0 && localH > 0);

	auto projectToRadar = [&](const CellStruct& c, float& outRx, float& outRy)
	{
		if (hasIsoBounds)
		{
			const float isoX
				= static_cast<float>((static_cast<int>(c.X) - static_cast<int>(c.Y) + mapW - 1) - (2 * localX));

			const float isoY
				= static_cast<float>((static_cast<int>(c.X) + static_cast<int>(c.Y) - mapW - 1) - (2 * localY));

			outRx = std::clamp(3.0f + (isoX * 121.0f) / std::max(1.0f, 2.0f * localW), 2.0f, 125.0f);

			outRy = std::clamp(3.0f + (isoY * 121.0f) / std::max(1.0f, 2.0f * localH), 2.0f, 125.0f);
		}
		else
		{
			outRx = std::clamp(6.0f + ((c.X - minMapX) * 115.0f) / spanX, 4.0f, 123.0f);

			outRy = std::clamp(6.0f + ((c.Y - minMapY) * 115.0f) / spanY, 4.0f, 123.0f);
		}
	};

	// Cell of a radar point: radar coordinates first, then the real cell, so the dashboard can
	// measure distances in cells. Reads the map only (observer telemetry, never a decision).
	auto projectWithCell = [&](const CellStruct& c, char* out, size_t outCap)
	{
		float rx = 0.0f;
		float ry = 0.0f;
		projectToRadar(c, rx, ry);
		std::snprintf(out, outCap, "%.1f,%.1f,%d,%d", rx, ry, static_cast<int>(c.X), static_cast<int>(c.Y));
	};

	if (matchEnded || (Unsorted::CurrentFrame % 60 == 0))
	{
		SampleExpertGains();
		SampleExpertCredit();

		std::string jsonBuf;

		jsonBuf.reserve(131072);

		char lineBuf[16384]; // one house line carries decisions, plans and gate counts

		int lineLen = std::snprintf(lineBuf, sizeof(lineBuf),
			"{\n  \"slot\": %u,\n  \"pid\": %u,\n  \"frame\": %u,\n  \"fps\": "
			"%u,\n  \"finished\": %s,\n  \"winnerHouse\": %d,\n  \"stalemate\": "
			"%s,\n  \"elo\": %d,\n  \"gamesPlayed\": %u,\n  "
			"\"rulesSignature\": \"%016llX\",\n  \"baseSignature\": \"%016llX\",\n  "
			"\"mapInfo\": {\"cells\": %d, \"waterPct\": %d, \"landLinked\": %s, \"known\": %s},\n "
			" \"contextRouting\": {\"challenger\": %s, \"opponent\": %s, \"phaseFrames\": [%d, %d]},\n "
			" \"mapGeom\": {\"mapRect\": [%d, %d], \"visibleRect\": [%d, %d, %d, %d], \"iso\": %s, \"bounds\": [%d, %d, %d, %d]},\n "
			" \"houses\": [\n",
			dst->SlotIndex, dst->ProcessId, dst->CurrentFrame, dst->FramesPerSecond, matchEnded ? "true" : "false",
			dst->WinnerHouseIndex, stalemate ? "true" : "false", NeuralAI::Backpack.EstimatedELO,
			NeuralAI::Backpack.GamesPlayed, static_cast<unsigned long long>(NeuralAI::CurrentRulesSignature),
			static_cast<unsigned long long>(NeuralAI::CurrentBaseSignature), MapInfo.Cells, MapInfo.WaterPct,
			MapInfo.LandLinked ? "true" : "false", MapInfo.Computed ? "true" : "false", ContextRouting.Loaded ? "true" : "false",
			OpponentContextRouting.Loaded ? "true" : "false", RoutingMidPhaseFrame, RoutingLatePhaseFrame, mapW,
			static_cast<int>(MapClass::Instance.MapRect.Height), localX, localY, localW, localH, hasIsoBounds ? "true" : "false",
			minMapX, minMapY, spanX, spanY);

		if (lineLen > 0)
			jsonBuf.append(lineBuf, std::min<size_t>(static_cast<size_t>(lineLen), sizeof(lineBuf) - 1));

		for (uint32_t i = 0; i < dst->NumHouses; ++i)
		{
			const auto& h = dst->Houses[i];

			auto const pOwner = (h.ArrayIndex >= 0 && h.ArrayIndex < HouseClass::Array.Count)
				? HouseClass::Array.GetItem(h.ArrayIndex)
				: nullptr;

			int defenses = 0;

			std::map<const BuildingTypeClass*, int> bldComp;

			if (pOwner)
			{
				for (auto const pOwned : pOwner->Buildings)
				{
					if (pOwned && pOwned->Type && !pOwned->InLimbo && pOwned->IsAlive && !pOwned->Type->InvisibleInGame)
					{
						if (pOwned->Type->CanBeOccupied && !pOwned->Type->BaseNormal && !pOwned->Type->ConstructionYard
							&& pOwned->Type->TechLevel < 0)
						{
							continue;
						}

						const bool isDeployedVehicle = pOwned->Type->ResourceGatherer
							|| (pOwned->Type->UndeploysInto != nullptr && !pOwned->Type->ConstructionYard
								&& !pOwned->Type->BaseNormal && !pOwned->Type->Refinery);
						if (isDeployedVehicle)
							continue;

						if (pOwned->Type->IsBaseDefense)
							defenses++;

						bldComp[pOwned->Type]++;
					}
				}
			}

			std::vector<std::pair<int, const BuildingTypeClass*>> rankedBlds;

			for (auto const& bEntry : bldComp)
				rankedBlds.emplace_back(bEntry.second, bEntry.first);

			std::sort(
				rankedBlds.begin(), rankedBlds.end(), [](auto const& a, auto const& b) { return a.first > b.first; });

			std::string bldText;

			for (size_t rb = 0; rb < rankedBlds.size() && rb < 12; ++rb)
			{
				if (rb > 0)
					bldText += ";";

				bldText += rankedBlds[rb].second->ID;

				bldText += ":" + std::to_string(rankedBlds[rb].first);
			}

			std::map<const TechnoTypeClass*, int> composition;

			if (pOwner)
			{
				for (auto const pOwnedUnit : UnitClass::Array)
				{
					if (pOwnedUnit && pOwnedUnit->Owner == pOwner && pOwnedUnit->Type && !pOwnedUnit->InLimbo
						&& pOwnedUnit->IsAlive)
					{
						composition[pOwnedUnit->Type]++;
					}
				}

				for (auto const pOwnedInf : InfantryClass::Array)
				{
					if (pOwnedInf && pOwnedInf->Owner == pOwner && pOwnedInf->Type && !pOwnedInf->InLimbo
						&& pOwnedInf->IsAlive)
					{
						composition[pOwnedInf->Type]++;
					}
				}

				for (auto const pOwnedAir : AircraftClass::Array)
				{
					if (pOwnedAir && pOwnedAir->Owner == pOwner && pOwnedAir->Type && !pOwnedAir->InLimbo
						&& pOwnedAir->IsAlive)
					{
						composition[pOwnedAir->Type]++;
					}
				}
			}

			std::vector<std::pair<int, const TechnoTypeClass*>> ranked;

			for (auto const& entry : composition)
				ranked.emplace_back(entry.second, entry.first);

			std::sort(ranked.begin(), ranked.end(), [](auto const& a, auto const& b) { return a.first > b.first; });

			std::string compositionText;

			for (size_t r = 0; r < ranked.size() && r < 10; ++r)
			{
				if (r > 0)
					compositionText += ";";

				compositionText += ranked[r].second->ID;

				compositionText += ":" + std::to_string(ranked[r].first);
			}

			std::string decisionsText;
			std::string enemyMixText;
			std::string plansText;
			std::string gatesText;
			std::string sectionsText;
			std::string peaksText;
			std::string expertTicksText;
			std::string expertGainText;
			std::string expertTicksByPhaseText;
			std::string expertGainByPhaseText;

			// Expert choices of this match, one count per expert (index = expert).
			for (int e = 0; e < NeuralAI::NumSpecialists; ++e)
			{
				const int ticks = (h.ArrayIndex >= 0 && h.ArrayIndex < 32) ? ExpertTicks[h.ArrayIndex][e] : 0;
				expertTicksText += (e ? ", " : "") + std::to_string(ticks);
				const int gain = (h.ArrayIndex >= 0 && h.ArrayIndex < 32) ? ExpertGain[h.ArrayIndex][e] : 0;
				expertGainText += (e ? ", " : "") + std::to_string(gain);
			}

			// The same per game phase (early, mid, late; see RoutingPhase): one row of experts per phase.
			for (int phase = 0; phase < NumRoutingPhases; ++phase)
			{
				const bool known = h.ArrayIndex >= 0 && h.ArrayIndex < 32;
				std::string ticksRow;
				std::string gainRow;

				for (int e = 0; e < NeuralAI::NumSpecialists; ++e)
				{
					ticksRow += (e ? ", " : "") + std::to_string(known ? ExpertTicksByPhase[h.ArrayIndex][phase][e] : 0);
					gainRow += (e ? ", " : "") + std::to_string(known ? ExpertGainByPhase[h.ArrayIndex][phase][e] : 0);
				}

				expertTicksByPhaseText += (phase ? ", [" : "[") + ticksRow + "]";
				expertGainByPhaseText += (phase ? ", [" : "[") + gainRow + "]";
			}

			// Decision credit (see NeuralCreditPart), in tens of credits like expertGain: per expert,
			// per phase of the decision, and per expert its parts [built, lost, spent, income, kills,
			// shared].
			std::string expertCreditText;
			std::string expertCreditByPhaseText;
			std::string expertCreditPartsText;

			if (h.ArrayIndex >= 0 && h.ArrayIndex < 32)
			{
				for (int e = 0; e < NeuralAI::NumSpecialists; ++e)
				{
					expertCreditText += (e ? ", " : "") + std::to_string(ExpertCredit[h.ArrayIndex][e] / 10);
					std::string partsRow;

					for (size_t part = 0; part < static_cast<size_t>(NeuralCreditPart::Count); ++part)
						partsRow += (part ? ", " : "") + std::to_string(ExpertCreditParts[h.ArrayIndex][e][part] / 10);

					expertCreditPartsText += (e ? ", [" : "[") + partsRow + "]";
				}

				for (int phase = 0; phase < NumRoutingPhases; ++phase)
				{
					std::string creditRow;

					for (int e = 0; e < NeuralAI::NumSpecialists; ++e)
						creditRow += (e ? ", " : "") + std::to_string(ExpertCreditByPhase[h.ArrayIndex][phase][e] / 10);

					expertCreditByPhaseText += (phase ? ", [" : "[") + creditRow + "]";
				}
			}

			// Slowest ticks: [frame, ms, live objects, "three heaviest phases"].
			if (h.ArrayIndex >= 0 && h.ArrayIndex < 32)
			{
				for (auto const& peak : TickPeaks[h.ArrayIndex])
				{
					char peakBuf[192];
					std::snprintf(peakBuf, sizeof(peakBuf), "%s[%d, %.1f, %d, \"%s\"]", peaksText.empty() ? "" : ", ", peak.Frame, peak.Ms,
						peak.Objects, peak.Breakdown.c_str());
					peaksText += peakBuf;
				}
			}

			// Average and peak ms per AI tick of each section: "name": [avg, max].
			if (h.ArrayIndex >= 0 && h.ArrayIndex < 32 && TickCosts[h.ArrayIndex].Ticks > 0)
			{
				for (size_t sec = 0; sec < static_cast<size_t>(NeuralSection::Count); ++sec)
				{
					char secBuf[64];
					std::snprintf(secBuf, sizeof(secBuf), "%s\"%s\": [%.2f, %.1f]", sec > 0 ? ", " : "", NeuralSectionNames[sec],
						SectionTotalMs[h.ArrayIndex][sec] / TickCosts[h.ArrayIndex].Ticks, SectionMaxMs[h.ArrayIndex][sec]);
					sectionsText += secBuf;
				}
			}

			if (h.ArrayIndex >= 0 && h.ArrayIndex < 32)
			{
				for (int gs = 0; gs < 128; ++gs)
				{
					auto const& gc = GateCounts[h.ArrayIndex][gs];

					if (gc[0] + gc[1] == 0)
						continue;

					char gateBuf[48];
					std::snprintf(gateBuf, sizeof(gateBuf), "%s\"%d\": [%u, %u]", gatesText.empty() ? "" : ", ", gs, gc[0], gc[1]);
					gatesText += gateBuf;
				}
			}

			if (h.ArrayIndex >= 0 && h.ArrayIndex < 32)
			{
				for (size_t pl = 0; pl < static_cast<size_t>(NeuralPlanId::Count); ++pl)
				{
					auto const& st = PlanStats[h.ArrayIndex][pl];
					char planBuf[128];
					std::snprintf(planBuf, sizeof(planBuf), "%s\"%s\": [%u, %u, %u, %u, %u, %u]", pl > 0 ? ", " : "", NeuralPlanNames[pl],
						st[0], st[1], st[2], st[3], st[4], st[5]);
					plansText += planBuf;
				}
			}

			if (pOwner)
			{
				const auto& foe = GetMacroState(pOwner).OpponentProfile;
				char mixBuf[160];
				std::snprintf(mixBuf, sizeof(mixBuf), "inf:%d veh:%d art:%d nav:%d air:%d def:%d rush:%d conf:%d",
					foe.Share(EnemyForceCategory::Infantry), foe.Share(EnemyForceCategory::Vehicle),
					foe.Share(EnemyForceCategory::Artillery), foe.Share(EnemyForceCategory::Naval),
					foe.Share(EnemyForceCategory::Air), foe.KnownDefenses, foe.RushPressure, foe.Confidence());
				enemyMixText = mixBuf;
			}

			if (h.ArrayIndex >= 0 && h.ArrayIndex < 32)
			{
				for (size_t d = 0; d < static_cast<size_t>(NeuralDecision::Count); ++d)
				{
					if (d > 0)
						decisionsText += ", ";

					decisionsText += "\"";
					decisionsText += NeuralDecisionNames[d];
					decisionsText += "\": ";
					decisionsText += std::to_string(DecisionCounts[h.ArrayIndex][d]);
				}
			}

			const NeuralTerritoryStat terr
				= (h.ArrayIndex >= 0 && h.ArrayIndex < 32) ? TerritoryStats[h.ArrayIndex] : NeuralTerritoryStat {};

			// Resource claim (UpdateResourceClaim): [rx, ry, cellX, cellY, tier, treeX, treeY] or null.
			char claimText[96] = "null";

			if (pOwner)
			{
				auto const& claimMacro = GetMacroState(pOwner);

				if (claimMacro.ResourceClaimCell != CellStruct::Empty)
				{
					char claimCell[48];
					projectWithCell(claimMacro.ResourceClaimCell, claimCell, sizeof(claimCell));
					const bool hasTree = claimMacro.ResourceClaimTreeCell != CellStruct::Empty;
					std::snprintf(claimText, sizeof(claimText), "[%s,%d,%d,%d]", claimCell, claimMacro.ResourceClaimTier,
						hasTree ? static_cast<int>(claimMacro.ResourceClaimTreeCell.X) : -1,
						hasTree ? static_cast<int>(claimMacro.ResourceClaimTreeCell.Y) : -1);
				}
			}

			// Team blackboard (FormatTeamTelemetry): [phase, leader, targetX, targetY, split, front, answers,
			// calling] or null without an ally.
			char teamText[96] = "null";

			if (pOwner)
				FormatTeamTelemetry(pOwner, teamText, sizeof(teamText));

			// Economy experts (0 and 6): credits earned so far (spent + held: harvests, sales, crates),
			// Expert 0 trouble, Expert 6 opportunity, harvesters under fire, refineries no harvester
			// serves, storage nearly full.
			char econText[96] = "0, 0, 0, 0, 0, 0";

			if (pOwner)
			{
				auto const& econMacro = GetMacroState(pOwner);
				std::snprintf(econText, sizeof(econText), "%d, %d, %d, %d, %d, %d", pOwner->CreditsSpent + pOwner->Available_Money(),
					econMacro.EconomyTrouble, econMacro.ExpansionOpportunity, econMacro.HarvestersUnderFire,
					econMacro.StarvedRefineries, econMacro.StorageNearlyFull ? 1 : 0);
			}

			// Army preservation: fighting units lost and their cost (NeuralArmyLedger), repair trips
			// under way.
			char armyText[64] = "0, 0, 0";

			if (pOwner)
				FormatArmyTelemetry(pOwner, armyText, sizeof(armyText));

			// Expert 3 tacit coalition (UpdateTacitCoalition): [active frames, target now, last target,
			// joined, declined, declined as second strongest, last decline reason, truces, truces
			// revoked, waves at the target, at truce partners, at others while on, waves with none on,
			// coalition targets standing now over all houses, peak of that this match].
			char coalitionText[160] = "0, -1, -1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0";
			// Expert 3 diplomacy details by name (NeuralAI.Diplomacy.inl).
			char diplomacyText[1400] = "";

			if (pOwner)
			{
				auto const& coal = GetMacroState(pOwner).Coalition;
				std::snprintf(coalitionText, sizeof(coalitionText), "%d, %d, %d, %d, %d, %d, %d, %d, %d, %d, %d, %d, %d, %d, %d",
					coal.ActiveFrames, coal.Target, coal.LastTarget, coal.Joined, coal.Declined, coal.DeclinedSecond,
					coal.LastDeclineReason, coal.Truces, coal.Revoked, coal.TargetWaves, coal.PartnerWaves, coal.OtherWaves,
					coal.FreeWaves, CoalitionConcurrentNow, CoalitionConcurrentPeak);
				std::snprintf(diplomacyText, sizeof(diplomacyText),
					"\"reason\": %d, \"choice\": %d, \"joinElimination\": %d, \"joinLeader\": %d, \"joinStalemate\": %d, "
					"\"joinTraitor\": %d, \"choiceGrudge\": %d, \"choiceFragile\": %d, \"choicePoorest\": %d, \"choiceTraitor\": %d, "
					"\"declinedStrongest\": %d, \"endBetrayal\": %d, \"endWeakened\": %d, \"endTargetGone\": %d, \"endTimeout\": %d, "
					"\"endTeam\": %d, \"truceEndBetrayal\": %d, \"truceEndCollateral\": %d, \"betrayalsSuffered\": %d, "
					"\"betrayalsCommitted\": %d, \"betrayalsWitnessed\": %d, \"distrustRefusals\": %d, \"plansCancelled\": %d, "
					"\"unitsRecalled\": %d, \"unitsRedirected\": %d, \"partnerTargetsCleared\": %d, \"graceDamageIgnored\": %d, "
					"\"swCancelled\": %d, \"lowProfileFrames\": %d, \"duelHoldFrames\": %d, \"postDuelStrikes\": %d, "
					"\"dyingRushes\": %d, \"dyingWaves\": %d, \"stalemateUpdates\": %d",
					coal.Reason, coal.TargetChoice, coal.JoinsByReason[1], coal.JoinsByReason[2], coal.JoinsByReason[3],
					coal.JoinsByReason[4], coal.JoinsByChoice[1], coal.JoinsByChoice[2], coal.JoinsByChoice[3], coal.JoinsByChoice[4],
					coal.DeclinedStrongest, coal.PactEnds[0], coal.PactEnds[1], coal.PactEnds[2], coal.PactEnds[3], coal.PactEnds[4],
					coal.TruceEnds[0], coal.TruceEnds[1], coal.BetrayalsSuffered,
					(h.ArrayIndex >= 0 && h.ArrayIndex < 32) ? BetrayalsCommitted[h.ArrayIndex] : 0, coal.BetrayalsWitnessed,
					coal.DistrustRefusals, coal.PlansCancelled, coal.UnitsRecalled, coal.UnitsRedirected, coal.PartnerTargetsCleared,
					coal.GraceDamageIgnored, coal.SuperweaponsCancelled, coal.LowProfileFrames, coal.DuelHoldFrames,
					coal.PostDuelStrikes, coal.DyingRushes, coal.DyingWaves, coal.StalemateUpdates);
			}

			// Bank: [end credits, peak, average, spent, army value, AI ticks, rich ticks, governor
			// ticks, no target per queue (bld, veh, nav, inf, air), idle factory per queue].
			char bankText[192] = "0";

			if (pOwner && h.ArrayIndex >= 0 && h.ArrayIndex < 32)
			{
				int armyValue = 0;

				for (auto const pTechno : TechnoClass::Array)
				{
					if (!pTechno || pTechno->Owner != pOwner || !pTechno->IsAlive || pTechno->InLimbo
						|| pTechno->WhatAmI() == AbstractType::Building)
					{
						continue;
					}

					auto const pType = pTechno->GetTechnoType();
					auto const pUnitType = abstract_cast<UnitTypeClass*>(pType);

					if (!pType || (pUnitType && (pUnitType->Harvester || pUnitType->ResourceGatherer || pUnitType->DeploysInto)))
						continue;

					armyValue += std::max(0, pType->Cost);
				}

				auto const& bank = BankStats[h.ArrayIndex];
				std::snprintf(bankText, sizeof(bankText), "%d, %d, %d, %d, %d, %u, %u, %u, %u, %u, %u, %u, %u, %u, %u, %u, %u, %u",
					pOwner->Available_Money(), bank.PeakCredits, bank.Ticks ? static_cast<int>(bank.CreditsSum / bank.Ticks) : 0,
					pOwner->CreditsSpent, armyValue, bank.Ticks, bank.RichTicks, bank.GovernorTicks, bank.NoTarget[0],
					bank.NoTarget[1], bank.NoTarget[2], bank.NoTarget[3], bank.NoTarget[4], bank.FactoryIdle[0], bank.FactoryIdle[1],
					bank.FactoryIdle[2], bank.FactoryIdle[3], bank.FactoryIdle[4]);
			}

			// Defense ledger: [spent on defenses, built, lost, lost without a kill, kills, killed value,
			// total credits spent, value of our defenses now, defense budget, payback percent, Expert 7 pressure].
			char defenseText[160] = "";

			if (pOwner)
			{
				auto const& defMacro = GetMacroState(pOwner);
				std::snprintf(defenseText, sizeof(defenseText), "%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d", defMacro.DefenseSpent,
					defMacro.DefensesBuilt, defMacro.DefensesLost, defMacro.DefensesLostNoKill, defMacro.DefenseKills,
					defMacro.DefenseKillValue, pOwner->CreditsSpent, defMacro.OwnDefenseValue, defMacro.DefenseValueCap,
					defMacro.DefenseEfficiencyPct, defMacro.DefensePressure);
			}

			lineLen = std::snprintf(lineBuf, sizeof(lineBuf),
				"    {\"idx\": %d, \"side\": %d, \"country\": %d, \"archetype\": %d, \"opening\": %d, "
				"\"expert\": %d, \"credits\": %d, \"killedUnits\": %d, "
				"\"killedBlds\": %d, \"ownedUnits\": %d, \"ownedBlds\": "
				"%d, \"score\": %d, \"winner\": %u, \"defeated\": %u, \"policy\": "
				"%u, \"conYards\": %d, \"harvesters\": %d, \"refineries\": %d, "
				"\"warFactories\": %d, \"barracks\": %d, "
				"\"defenses\": %d, \"territory\": %d, \"territoryAvg\": %.1f, \"oreFields\": %d, \"oreFieldsAvg\": %.1f, "
				"\"bldValue\": %d, \"blds\": \"%s\", \"units\": \"%s\", \"airThreat\": %d, "
				"\"ownAA\": %d, \"aiMsAvg\": %.2f, \"aiMsMax\": %.2f, \"aiSections\": {%s}, \"aiPeaks\": [%s], \"expertTicks\": [%s], \"expertGain\": [%s], \"expertTicksByPhase\": [%s], \"expertGainByPhase\": [%s], \"expertCredit\": [%s], \"expertCreditByPhase\": [%s], \"expertCreditParts\": [%s], \"defense\": [%s], \"enemyMix\": \"%s\", \"claim\": %s, \"team\": %s, \"econ\": [%s], \"bank\": [%s], \"army\": [%s], \"coalition\": [%s], \"diplomacy\": {%s}, \"decisions\": {%s}, \"plans\": {%s}, \"gates\": {%s}}%s\n",
				h.ArrayIndex, h.SideIndex, h.CountryIndex, h.Archetype, pOwner ? GetMacroState(pOwner).Opening : 0, h.ActiveExpert, h.Credits, h.KilledUnits,
				h.KilledBuildings, h.OwnedUnits, h.OwnedBuildings, h.NetWorthScore, h.IsWinner, h.IsDefeated,
				h.PolicySlot, pOwner ? pOwner->NumConYards : 0, pOwner ? pOwner->CountResourceGatherers : 0,
				pOwner ? pOwner->CountResourceDestinations : 0, pOwner ? pOwner->NumWarFactories : 0,
				pOwner ? pOwner->NumBarracks : 0, defenses, terr.Territory, terr.TerritoryAvg(), terr.OreFields,
				terr.OreFieldsAvg(), terr.BuildingValue, bldText.c_str(), compositionText.c_str(),
				pOwner ? GetMacroState(pOwner).AirThreatValue : 0, pOwner ? GetMacroState(pOwner).OwnAACapacity : 0,
				(h.ArrayIndex >= 0 && h.ArrayIndex < 32 && TickCosts[h.ArrayIndex].Ticks > 0)
					? TickCosts[h.ArrayIndex].TotalMs / TickCosts[h.ArrayIndex].Ticks
					: 0.0,
				(h.ArrayIndex >= 0 && h.ArrayIndex < 32) ? TickCosts[h.ArrayIndex].MaxMs : 0.0, sectionsText.c_str(), peaksText.c_str(), expertTicksText.c_str(), expertGainText.c_str(), expertTicksByPhaseText.c_str(), expertGainByPhaseText.c_str(), expertCreditText.c_str(), expertCreditByPhaseText.c_str(), expertCreditPartsText.c_str(), defenseText, enemyMixText.c_str(), claimText, teamText, econText, bankText, armyText, coalitionText, diplomacyText, decisionsText.c_str(), plansText.c_str(), gatesText.c_str(), (i + 1 < dst->NumHouses) ? "," : "");

			if (lineLen > 0)
				jsonBuf.append(lineBuf, std::min<size_t>(static_cast<size_t>(lineLen), sizeof(lineBuf) - 1));
		}

		// Ore fields (the GetOreFields cache the AI already keeps; read as is, so telemetry never
		// changes when the AI refreshes it): [rx, ry, cellX, cellY, ore cells, spawner tree, holder]
		// where holder is the house with the nearest refinery / resource destination within 14
		// cells (as GetHeldOreBlocks counts it), else -1. Rebuilt at most every 900 frames.
		static std::string oreFieldsText;
		static int oreFieldsFrame = -100000;
		const int nowFrame = Unsorted::CurrentFrame;

		if (nowFrame < oreFieldsFrame || nowFrame - oreFieldsFrame >= 900 || (oreFieldsText.empty() && !OreFieldCache.empty()))
		{
			oreFieldsFrame = nowFrame;
			oreFieldsText.clear();

			std::vector<std::pair<CellStruct, int>> hubs;

			for (auto const pBld : BuildingClass::Array)
			{
				if (pBld && pBld->IsAlive && !pBld->InLimbo && pBld->Type && pBld->Owner && !pBld->Owner->IsNeutral()
					&& (pBld->Type->Refinery || pBld->Type->ResourceDestination))
				{
					hubs.emplace_back(pBld->GetMapCoords(), pBld->Owner->ArrayIndex);
				}
			}

			for (auto const& field : OreFieldCache)
			{
				if (oreFieldsText.size() > 12000)
					break;

				int holder = -1;
				int holderDist = 15;

				for (auto const& [hubCell, hubHouse] : hubs)
				{
					const int d = std::max(std::abs(hubCell.X - field.Cell.X), std::abs(hubCell.Y - field.Cell.Y));

					if (d < holderDist)
					{
						holderDist = d;
						holder = hubHouse;
					}
				}

				char fieldCell[48];
				char fieldBuf[96];
				projectWithCell(field.Cell, fieldCell, sizeof(fieldCell));
				std::snprintf(fieldBuf, sizeof(fieldBuf), "%s[%s,%d,%d,%d]", oreFieldsText.empty() ? "" : ",", fieldCell,
					field.OreCells, field.SpawnerTree ? 1 : 0, holder);
				oreFieldsText += fieldBuf;
			}
		}

		jsonBuf.append("  ],\n  \"oreFields\": [");
		jsonBuf.append(oreFieldsText);
		jsonBuf.append("],\n  \"routeHeat\": [");

		// Route heat (NeuralAI.RouteHeat.inl), one entry per house that has any: bucket count and
		// total heat, the 24 hottest buckets [rx, ry, cellX, cellY, heat], the defense and ambush
		// spots [rx, ry, cellX, cellY, heat, width, flags, baseDist, cover, score] and the towers
		// placed covering a defense spot [rx, ry, cellX, cellY, spotX, spotY].
		{
			bool firstHouse = true;
			char routeCell[48];
			char routeBuf[160];

			for (int hi = 0; hi < 32; ++hi)
			{
				auto const& routeHeat = RouteHeatStates[hi];

				if (routeHeat.Buckets.empty())
					continue;

				std::vector<std::pair<int, CellStruct>> hottest;
				int totalHeat = 0;

				for (auto const& [index, bucket] : routeHeat.Buckets)
				{
					const int heat = DecayedRouteHeat(bucket);
					totalHeat += heat;

					if (heat <= 0)
						continue;

					const CellStruct cell = bucket.RepCell != CellStruct::Empty
						? bucket.RepCell
						: CellStruct { static_cast<short>((index % RouteHeatBucketsPerRow) * RouteHeatBucketCells + RouteHeatBucketCells / 2),
							static_cast<short>((index / RouteHeatBucketsPerRow) * RouteHeatBucketCells + RouteHeatBucketCells / 2) };
					hottest.emplace_back(heat, cell);
				}

				std::stable_sort(hottest.begin(), hottest.end(), [](const auto& a, const auto& b) { return a.first > b.first; });

				if (hottest.size() > 24)
					hottest.resize(24);

				std::snprintf(routeBuf, sizeof(routeBuf), "%s\n    {\"idx\": %d, \"buckets\": %d, \"heat\": %d, \"cells\": [",
					firstHouse ? "" : ",", hi, static_cast<int>(routeHeat.Buckets.size()), totalHeat);
				jsonBuf.append(routeBuf);
				firstHouse = false;

				for (size_t k = 0; k < hottest.size(); ++k)
				{
					projectWithCell(hottest[k].second, routeCell, sizeof(routeCell));
					std::snprintf(routeBuf, sizeof(routeBuf), "%s[%s,%d]", k ? "," : "", routeCell, hottest[k].first);
					jsonBuf.append(routeBuf);
				}

				auto appendSpots = [&](const char* name, const std::vector<NeuralRouteSpot>& spots)
				{
					std::snprintf(routeBuf, sizeof(routeBuf), "], \"%s\": [", name);
					jsonBuf.append(routeBuf);

					for (size_t k = 0; k < spots.size(); ++k)
					{
						auto const& spot = spots[k];
						projectWithCell(spot.Cell, routeCell, sizeof(routeCell));
						std::snprintf(routeBuf, sizeof(routeBuf), "%s[%s,%d,%d,%d,%d,%d,%d]", k ? "," : "", routeCell, spot.Heat, spot.Width,
							spot.Flags, spot.BaseDist, spot.Cover, spot.Score);
						jsonBuf.append(routeBuf);
					}
				};

				appendSpots("defenseSpots", routeHeat.DefenseSpots);
				appendSpots("ambushSpots", routeHeat.AmbushSpots);
				jsonBuf.append("], \"towers\": [");

				for (size_t k = 0; k < routeHeat.RouteTowers.size(); ++k)
				{
					auto const& [towerCell, spotCell] = routeHeat.RouteTowers[k];
					projectWithCell(towerCell, routeCell, sizeof(routeCell));
					std::snprintf(routeBuf, sizeof(routeBuf), "%s[%s,%d,%d]", k ? "," : "", routeCell, static_cast<int>(spotCell.X),
						static_cast<int>(spotCell.Y));
					jsonBuf.append(routeBuf);
				}

				jsonBuf.append("]}");
			}
		}

		jsonBuf.append("\n  ],\n  \"radar\": [");

		auto sanitizeJsonStr = [](const char* src, char* out, size_t outCap)
		{
			if (!out || outCap == 0)
				return;

			size_t w = 0;
			if (src)
			{
				for (size_t i = 0; src[i] != '\0' && w + 1 < outCap; ++i)
				{
					const unsigned char ch = static_cast<unsigned char>(src[i]);
					if (ch >= 32 && ch != '"' && ch != '\\')
						out[w++] = static_cast<char>(ch);
				}
			}

			out[w] = '\0';
		};

		// Map inspector details appended to every radar point (observer telemetry, never read by a
		// decision): ,uid,health%,mission,megaMission,target,destination,transform where target is
		// [rx,ry,health% or -1,uid,cellX,cellY] or 0 and destination is [rx,ry,cellX,cellY] or 0.
		// The point's own cell follows as the last two fields.
		auto objectDetail = [&](TechnoClass* pTechno, char* out, size_t outCap)
		{
			if (!out || outCap == 0)
				return;

			out[0] = '\0';

			if (!pTechno)
				return;

			const int hp = static_cast<int>(pTechno->GetHealthPercentage() * 100.0 + 0.5);
			auto const pFoot = abstract_cast<FootClass*>(pTechno);
			const int mega = pFoot ? static_cast<int>(pFoot->MegaMission) : -1;
			char targetText[80] = "0";
			char destText[48] = "0";

			if (AbstractClass* const pTarget = pTechno->Target)
			{
				float tx = 0.0f;
				float ty = 0.0f;
				const CellStruct targetCell = CellClass::Coord2Cell(pTarget->GetCoords());
				projectToRadar(targetCell, tx, ty);
				auto const pTargetTechno = abstract_cast<TechnoClass*>(pTarget);
				const int targetHp = pTargetTechno ? static_cast<int>(pTargetTechno->GetHealthPercentage() * 100.0 + 0.5) : -1;
				std::snprintf(targetText, sizeof(targetText), "[%.1f,%.1f,%d,%u,%d,%d]", tx, ty, targetHp,
					pTargetTechno ? static_cast<unsigned>(pTargetTechno->UniqueID) : 0u, static_cast<int>(targetCell.X),
					static_cast<int>(targetCell.Y));
			}

			if (pFoot)
			{
				AbstractClass* const pDest = pFoot->Destination ? pFoot->Destination : pFoot->MegaDestination;

				if (pDest)
				{
					char destCell[48];
					projectWithCell(CellClass::Coord2Cell(pDest->GetCoords()), destCell, sizeof(destCell));
					std::snprintf(destText, sizeof(destText), "[%s]", destCell);
				}
			}

			// Deploy / undeploy counterpart type ID: the dashboard follows an object into its new
			// form only when the new object is exactly that type.
			const char* transformId = "";
			auto const pType = pTechno->GetTechnoType();

			if (auto const pUnitType = abstract_cast<UnitTypeClass*>(pType); pUnitType && pUnitType->DeploysInto)
				transformId = pUnitType->DeploysInto->ID;
			else if (auto const pBldType = abstract_cast<BuildingTypeClass*>(pType); pBldType && pBldType->UndeploysInto)
				transformId = pBldType->UndeploysInto->ID;

			char transformClean[32];
			sanitizeJsonStr(transformId, transformClean, sizeof(transformClean));

			const CellStruct ownCell = pTechno->GetMapCoords();
			std::snprintf(out, outCap, ",%u,%d,%d,%d,%s,%s,\"%s\",%d,%d", static_cast<unsigned>(pTechno->UniqueID), hp,
				static_cast<int>(pTechno->CurrentMission), mega, targetText, destText, transformClean, static_cast<int>(ownCell.X),
				static_cast<int>(ownCell.Y));
		};

		char detailBuf[224];
		bool firstPt = true;
		int ptCount = 0;

		for (auto const pBld : BuildingClass::Array)
		{
			if (!pBld || pBld->InLimbo || !pBld->IsAlive || !pBld->Owner || !pBld->Type || pBld->Type->InvisibleInGame
				|| pBld->Owner->IsNeutral() || pBld->Owner->IsObserver())
			{
				continue;
			}

			if (pBld->Type->CanBeOccupied && !pBld->Type->BaseNormal && !pBld->Type->ConstructionYard
				&& pBld->Type->TechLevel < 0)
			{
				continue;
			}

			if (ptCount >= 900 || jsonBuf.size() > 200000)
				break;

			const CellStruct c = pBld->GetMapCoords();

			if (c.X <= 1 && c.Y <= 1)
				continue;

			float rx = 0.0f;
			float ry = 0.0f;

			projectToRadar(c, rx, ry);

			const bool isDeployedVehicleBld = pBld->Type->ResourceGatherer
				|| (pBld->Type->UndeploysInto != nullptr && !pBld->Type->ConstructionYard && !pBld->Type->BaseNormal
					&& !pBld->Type->Refinery);
			// ptKind: 0 = unit/deployed vehicle, 1 = structure, 2 = defense structure
			// (IsBaseDefense=yes)
			const int ptKind = isDeployedVehicleBld ? 0 : (pBld->Type->IsBaseDefense ? 2 : 1);

			char idClean[32];
			char nameClean[64];
			sanitizeJsonStr(pBld->Type->ID, idClean, sizeof(idClean));
			sanitizeJsonStr(pBld->Type->Name, nameClean, sizeof(nameClean));

			objectDetail(pBld, detailBuf, sizeof(detailBuf));
			lineLen = std::snprintf(lineBuf, sizeof(lineBuf), "%s[%.1f,%.1f,%d,%d,\"%s\",\"%s\"%s]", firstPt ? "" : ",",
				rx, ry, pBld->Owner->ArrayIndex, ptKind, idClean, nameClean, detailBuf);

			if (lineLen > 0)
				jsonBuf.append(lineBuf, std::min<size_t>(static_cast<size_t>(lineLen), sizeof(lineBuf) - 1));

			firstPt = false;

			ptCount++;
		}

		for (auto const pUnit : UnitClass::Array)
		{
			if (!pUnit || pUnit->InLimbo || !pUnit->IsAlive || !pUnit->Owner || pUnit->Owner->IsNeutral()
				|| pUnit->Owner->IsObserver())
			{
				continue;
			}

			if (ptCount >= 1700 || jsonBuf.size() > 225000)
				break;

			const CellStruct c = pUnit->GetMapCoords();
			float rx = 0.0f;
			float ry = 0.0f;

			projectToRadar(c, rx, ry);

			char idClean[32];
			char nameClean[64];
			sanitizeJsonStr(pUnit->Type ? pUnit->Type->ID : "", idClean, sizeof(idClean));
			sanitizeJsonStr(pUnit->Type ? pUnit->Type->Name : "", nameClean, sizeof(nameClean));

			objectDetail(pUnit, detailBuf, sizeof(detailBuf));
			lineLen = std::snprintf(lineBuf, sizeof(lineBuf), "%s[%.1f,%.1f,%d,0,\"%s\",\"%s\"%s]", firstPt ? "" : ",",
				rx, ry, pUnit->Owner->ArrayIndex, idClean, nameClean, detailBuf);

			if (lineLen > 0)
				jsonBuf.append(lineBuf, std::min<size_t>(static_cast<size_t>(lineLen), sizeof(lineBuf) - 1));

			firstPt = false;

			ptCount++;
		}

		int infStep = 0;

		for (auto const pInf : InfantryClass::Array)
		{
			if (!pInf || pInf->InLimbo || !pInf->IsAlive || !pInf->Owner || pInf->Owner->IsNeutral()
				|| pInf->Owner->IsObserver())
			{
				continue;
			}

			infStep++;

			if (ptCount >= 2400 || jsonBuf.size() > 250000)
				break;

			const CellStruct c = pInf->GetMapCoords();
			float rx = 0.0f;
			float ry = 0.0f;

			projectToRadar(c, rx, ry);

			char idClean[32];
			char nameClean[64];
			sanitizeJsonStr(pInf->Type ? pInf->Type->ID : "", idClean, sizeof(idClean));
			sanitizeJsonStr(pInf->Type ? pInf->Type->Name : "", nameClean, sizeof(nameClean));

			objectDetail(pInf, detailBuf, sizeof(detailBuf));
			lineLen = std::snprintf(lineBuf, sizeof(lineBuf), "%s[%.1f,%.1f,%d,0,\"%s\",\"%s\"%s]", firstPt ? "" : ",",
				rx, ry, pInf->Owner->ArrayIndex, idClean, nameClean, detailBuf);

			if (lineLen > 0)
				jsonBuf.append(lineBuf, std::min<size_t>(static_cast<size_t>(lineLen), sizeof(lineBuf) - 1));

			firstPt = false;

			ptCount++;
		}

		jsonBuf.append("]\n}\n");

		if (TelemetryJsonView)
		{
			const uint32_t maxPayload = TelemetryJsonShmCapacity - 16;

			const uint32_t copyLen = static_cast<uint32_t>(std::min<size_t>(jsonBuf.size(), maxPayload));

			volatile uint32_t* pHdr = reinterpret_cast<volatile uint32_t*>(TelemetryJsonView);

			const uint32_t nextOdd = (pHdr[0] | 1u) + 2u;

			pHdr[0] = nextOdd;

			MemoryBarrier();

			pHdr[1] = copyLen;

			std::memcpy(TelemetryJsonView + 8, jsonBuf.data(), copyLen);

			TelemetryJsonView[8 + copyLen] = '\0';

			MemoryBarrier();

			pHdr[0] = nextOdd + 1u;
		}

		// No trainer reading the mapping (developer repro runs): the same JSON goes to the
		// instance's AI folder, written to a temporary file first and swapped in, so a reader
		// never sees half a snapshot.
		if (!TelemetryJsonView || !TelemetryJsonShmFromTrainer)
		{
			char jsonPath[MAX_PATH];
			char tmpPath[MAX_PATH];

			std::snprintf(jsonPath, sizeof(jsonPath), "AI\\train_telemetry_%d.json", NeuralAI::TrainSlotIndex);
			std::snprintf(tmpPath, sizeof(tmpPath), "%s.tmp", jsonPath);

			static bool loggedFileTelemetry = false;

			if (!loggedFileTelemetry)
			{
				loggedFileTelemetry = true;
				Debug::Log("[NeuralAI] No trainer telemetry mapping: writing %s every 60 frames.\n", jsonPath);
			}

			FILE* fp = nullptr;

			if (fopen_s(&fp, tmpPath, "wb") == 0 && fp)
			{
				const bool written = std::fwrite(jsonBuf.data(), 1, jsonBuf.size(), fp) == jsonBuf.size();

				std::fclose(fp);

				if (!written || !MoveFileExA(tmpPath, jsonPath, MOVEFILE_REPLACE_EXISTING))
					DeleteFileA(tmpPath);
			}
		}
	}
}


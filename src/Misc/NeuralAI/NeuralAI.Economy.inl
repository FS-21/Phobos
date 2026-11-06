// NeuralAI.Economy.inl: Economy upkeep: obsolete refinery recycling and emergency money.
// Part of NeuralAI.cpp (included in order; one translation unit).

namespace
{
	// Checks if a distant static refinery (> 20 cells from ConYard) has exhausted
	// all ore within 27 cells and trees within 22 cells. Strict (the resource hoarder, Expert 6):
	// > 14 cells from the ConYard, no ore within 12 and no spawner tree within 14, so refineries
	// left behind by a field that ran out are sold and rebuilt where the ore is.
	bool IsRefineryExhausted(BuildingClass* pBld, CellStruct baseCenter, bool strict = false)
	{
		if (!pBld || !pBld->IsAlive || pBld->InLimbo || !pBld->Type)
			return false;

		if ((!pBld->Type->Refinery && !pBld->Type->ResourceDestination) || pBld->Type->ResourceGatherer
			|| pBld->Type->Enslaves != nullptr || pBld->Type->UndeploysInto != nullptr)
		{
			return false;
		}

		if (pBld->CurrentMission == Mission::Selling || pBld->QueuedMission == Mission::Selling
			|| pBld->CurrentMission == Mission::Construction || pBld->QueuedMission == Mission::Construction)
		{
			return false;
		}

		const CellStruct refCoords = pBld->GetMapCoords();

		const int oreRadius = strict ? 12 : 27;
		const int treeRadius = strict ? 14 : 22;

		if (baseCenter != CellStruct::Empty && refCoords.DistanceFrom(baseCenter) < (strict ? 14.0 : 20.0))
			return false;

		const int scanRadius = std::max(oreRadius, treeRadius);

		for (int dy = -scanRadius; dy <= scanRadius; ++dy)
		{
			for (int dx = -scanRadius; dx <= scanRadius; ++dx)
			{
				if (dx * dx + dy * dy > scanRadius * scanRadius)
					continue;

				const CellStruct scanCell { static_cast<short>(refCoords.X + dx),
					static_cast<short>(refCoords.Y + dy) };

				if (!MapClass::Instance.CoordinatesLegal(scanCell))
					continue;

				if (auto const pCell = MapClass::Instance.TryGetCellAt(scanCell))
				{
					if (dx * dx + dy * dy <= oreRadius * oreRadius && pCell->GetContainedTiberiumValue() > 0)
						return false;

					if (dx * dx + dy * dy <= treeRadius * treeRadius)
					{
						if (auto const pTerrain = pCell->GetTerrain(false))
						{
							if (pTerrain->IsAlive && pTerrain->Type && pTerrain->Type->SpawnsTiberium)
								return false;
						}
					}
				}
			}
		}

		return true;
	}

	// strict: the resource hoarder's audit (IsRefineryExhausted strict), never applied to the
	// refineries in relaxedReach (placed as near their field as a rugged map allowed).
	void RecycleObsoleteRefineries(HouseClass* pHouse, bool strict, const std::vector<DWORD>& relaxedReach)
	{
		if (!pHouse || pHouse->ConYards.Count <= 0)
			return;

		int staticRefineryCount = 0;

		for (auto const pBld : pHouse->Buildings)
		{
			if (pBld && pBld->IsAlive && !pBld->InLimbo && pBld->Type
				&& (pBld->Type->Refinery || pBld->Type->ResourceDestination) && !pBld->Type->ResourceGatherer)
			{
				staticRefineryCount++;
			}
		}

		// Never sell our last 2 refineries proactively
		if (staticRefineryCount <= 2)
			return;

		const CellStruct baseCenter = pHouse->ConYards.GetItem(0)->GetMapCoords();

		for (auto const pBld : pHouse->Buildings)
		{
			// Selling a refinery removes its storage: only when what we have stored still fits in
			// the rest (nothing stored is lost), as AdvAI-style recycling should.
			const int storageAfterSale = pHouse->TotalStorage - (pBld && pBld->Type ? std::max(0, pBld->Type->Storage) : 0);
			const bool storedResourcesFit = pHouse->OwnedTiberium.GetTotalAmount() <= static_cast<float>(storageAfterSale);

			if (storedResourcesFit && IsRefineryExhausted(pBld, baseCenter,
					strict && std::find(relaxedReach.begin(), relaxedReach.end(), pBld->UniqueID) == relaxedReach.end())
				&& !pBld->HasAnyLink())
			{
				Debug::Log("[NeuralAI] House %d recycling exhausted distant refinery %s "
						"at (%d,%d).\n",
					pHouse->ArrayIndex, pBld->Type->ID, pBld->GetMapCoords().X, pBld->GetMapCoords().Y);

				pBld->Sell(1);

				break;
			}
		}
	}

	// Factory cap by difficulty, per category: every barracks counts together, every ground war
	// factory together (whatever faction it comes from, captured trees included), every shipyard
	// together. Easy 4, Normal 10, Hard none. Airfields are not capped here: they follow the demand
	// of the docking aircraft.
	int GetFactoryCategoryCap(HouseClass* pHouse)
	{
		const int diffIdx = pHouse ? static_cast<int>(pHouse->GetAIDifficultyIndex()) : 0;
		return diffIdx >= 2 ? 4 : (diffIdx == 1 ? 10 : INT_MAX);
	}

	// 0 barracks, 1 ground war factory, 2 shipyard, -1 not capped. A deployed mobile factory (a unit
	// that unfolds into a factory and can pack up again, UndeploysInto) is a unit, not a ConYard
	// building: never counted nor sold here.
	int FactoryCapCategory(BuildingTypeClass* pType)
	{
		if (!pType || pType->ConstructionYard || pType->UndeploysInto)
			return -1;

		if (pType->Factory == AbstractType::InfantryType)
			return 0;

		if (pType->Factory == AbstractType::UnitType)
			return pType->Naval ? 2 : 1;

		return -1;
	}

	// Living factories of a category that are not being sold (under construction included).
	int CountFactoriesInCategory(HouseClass* pHouse, int category)
	{
		int count = 0;

		for (auto const pBld : pHouse->Buildings)
		{
			if (pBld && pBld->IsAlive && !pBld->InLimbo && FactoryCapCategory(pBld->Type) == category
				&& pBld->CurrentMission != Mission::Selling && pBld->QueuedMission != Mission::Selling)
			{
				count++;
			}
		}

		return count;
	}

	// Sells one factory of a category above the difficulty cap (an outpost factory, or the first of
	// a new factory type a captured tree opened, was placed past it). Never the newest one, never
	// the last of its type (our own faction's factory stays, and so does the new one), never one
	// whose sale would leave a ConYard (18 cells) or the newest forward stone of the offensive crawl
	// (15 cells) without a factory of that category. The type with the most copies goes first,
	// outside the main base before inside. Nothing qualifies: the extra factory stays.
	// True when one was sold.
	bool SellFactoriesOverDifficultyCap(HouseClass* pHouse, const CellStruct& frontStoneCell)
	{
		const int cap = GetFactoryCategoryCap(pHouse);
		if (!pHouse || cap == INT_MAX)
			return false;

		const CellStruct mainBase = (pHouse->BaseSpawnCell != CellStruct::Empty)
			? pHouse->BaseSpawnCell
			: (pHouse->ConYards.Count > 0 ? pHouse->ConYards.GetItem(0)->GetMapCoords() : CellStruct::Empty);

		auto chebyshev = [](const CellStruct& a, const CellStruct& b)
		{
			return std::max(std::abs(a.X - b.X), std::abs(a.Y - b.Y));
		};

		std::array<std::vector<BuildingClass*>, 3> byCategory;
		std::array<bool, 3> underConstruction {};
		std::map<int, int> typeCounts;

		for (auto const pBld : pHouse->Buildings)
		{
			if (!pBld || !pBld->IsAlive || pBld->InLimbo)
				continue;

			const int category = FactoryCapCategory(pBld->Type);

			if (category < 0 || pBld->CurrentMission == Mission::Selling || pBld->QueuedMission == Mission::Selling)
				continue;

			if (pBld->CurrentMission == Mission::Construction || pBld->QueuedMission == Mission::Construction)
				underConstruction[category] = true;

			byCategory[category].push_back(pBld);
			typeCounts[pBld->Type->ArrayIndex]++;
		}

		// Anchors that need a factory of each category nearby, with their radius.
		std::vector<std::pair<CellStruct, int>> anchors;

		for (int i = 0; i < pHouse->ConYards.Count; ++i)
		{
			if (auto const pCY = pHouse->ConYards.GetItem(i); pCY && pCY->IsAlive && !pCY->InLimbo)
				anchors.emplace_back(pCY->GetMapCoords(), 18);
		}

		if (frontStoneCell != CellStruct::Empty)
			anchors.emplace_back(frontStoneCell, 15);

		for (int category = 0; category < 3; ++category)
		{
			auto const& blds = byCategory[category];

			if (static_cast<int>(blds.size()) <= cap || underConstruction[category])
				continue;

			BuildingClass* pNewest = blds.front();

			for (auto const pBld : blds)
			{
				if (pBld->UniqueID > pNewest->UniqueID)
					pNewest = pBld;
			}

			BuildingClass* pBest = nullptr;
			int bestCopies = 0;
			bool bestInMainBase = true;

			for (auto const pBld : blds)
			{
				const int copies = typeCounts[pBld->Type->ArrayIndex];

				if (pBld == pNewest || copies < 2)
					continue;

				const CellStruct bc = pBld->GetMapCoords();
				bool soleCover = false;

				for (auto const& [anchorCell, radius] : anchors)
				{
					if (chebyshev(bc, anchorCell) > radius)
						continue;

					bool otherCovers = false;

					for (auto const pOther : blds)
					{
						if (pOther != pBld && chebyshev(pOther->GetMapCoords(), anchorCell) <= radius)
						{
							otherCovers = true;
							break;
						}
					}

					if (!otherCovers)
					{
						soleCover = true;
						break;
					}
				}

				if (soleCover)
					continue;

				const bool inMainBase = mainBase != CellStruct::Empty && chebyshev(bc, mainBase) <= 20;

				if (!pBest || copies > bestCopies || (copies == bestCopies && bestInMainBase && !inMainBase))
				{
					pBest = pBld;
					bestCopies = copies;
					bestInMainBase = inMainBase;
				}
			}

			if (pBest)
			{
				Debug::Log("[NeuralAI] House %d over the factory cap (%d per category): selling %s at (%d,%d).\n",
					pHouse->ArrayIndex, cap, pBest->Type->ID, pBest->GetMapCoords().X, pBest->GetMapCoords().Y);

				pBest->Sell(1);
				return true; // one per check
			}
		}

		return false;
	}

	// Emergency cash recovery (AdvAI_Raise_Money): ONLY when the AI has 0
	// refineries, 0 harvesters, and < 1000 credits
	void RaiseEmergencyMoneyForRefinery(HouseClass* pHouse, int ownedRefineries, int ownedEconomyUnits, int money)
	{
		if (!pHouse || pHouse->ConYards.Count <= 0 || ownedRefineries > 0 || ownedEconomyUnits > 0 || money >= 1000)
			return;

		BuildingClass* pBestToSell = nullptr;
		int bestPriority = 999;

		for (auto const pBld : pHouse->Buildings)
		{
			if (!pBld || !pBld->IsAlive || pBld->InLimbo || !pBld->Type || pBld->Type->ToProtect
				|| pBld->Type->ConstructionYard)
			{
				continue;
			}

			if (pBld->CurrentMission == Mission::Selling || pBld->QueuedMission == Mission::Selling
				|| pBld->CurrentMission == Mission::Construction)
			{
				continue;
			}

			auto const pOT = pBld->Type;
			int prio = 999;

			if (pOT->Storage > 0 && !pOT->Refinery)
				prio = 1; // Silo in total bankruptcy
			else if (pOT->UnitRepair)
				prio = 2; // Service Depot
			else if (pOT->SuperWeapon != -1 && !pOT->IsBaseDefense)
				prio = 3; // Superweapon
			else if (pOT->SpySat || pOT->Radar)
				prio = 4; // Radar
			else if (pOT->IsBaseDefense)
				prio = 5; // Defense

			if (prio < bestPriority)
			{
				bestPriority = prio;

				pBestToSell = pBld;
			}
		}

		if (pBestToSell)
		{
			Debug::Log("[NeuralAI] House %d in total economic emergency (0 Refineries, "
					"0 Harvesters, %d credits): selling %s to fund recovery!\n",
				pHouse->ArrayIndex, money, pBestToSell->Type->ID);

			pBestToSell->Sell(1);
		}
	}

} // namespace

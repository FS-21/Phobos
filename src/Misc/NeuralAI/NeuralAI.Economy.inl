// NeuralAI.Economy.inl: Economy upkeep: obsolete refinery recycling and emergency money.
// Part of NeuralAI.cpp (included in order; one translation unit).

namespace
{
	// Checks if a distant static refinery (> 20 cells from ConYard) has exhausted
	// all ore within 27 cells and trees within 22 cells
	bool IsRefineryExhausted(BuildingClass* pBld, CellStruct baseCenter)
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

		if (baseCenter != CellStruct::Empty && refCoords.DistanceFrom(baseCenter) < 20.0)
			return false;

		for (int dy = -27; dy <= 27; ++dy)
		{
			for (int dx = -27; dx <= 27; ++dx)
			{
				if (dx * dx + dy * dy > 729)
					continue;

				const CellStruct scanCell { static_cast<short>(refCoords.X + dx),
					static_cast<short>(refCoords.Y + dy) };

				if (!MapClass::Instance.CoordinatesLegal(scanCell))
					continue;

				if (auto const pCell = MapClass::Instance.TryGetCellAt(scanCell))
				{
					if (pCell->GetContainedTiberiumValue() > 0)
						return false;

					if (dx * dx + dy * dy <= 484)
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

	void RecycleObsoleteRefineries(HouseClass* pHouse)
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

			if (storedResourcesFit && IsRefineryExhausted(pBld, baseCenter) && !pBld->HasAnyLink())
			{
				Debug::Log("[NeuralAI] House %d recycling exhausted distant refinery %s "
						"at (%d,%d).\n",
					pHouse->ArrayIndex, pBld->Type->ID, pBld->GetMapCoords().X, pBld->GetMapCoords().Y);

				pBld->Sell(1);

				break;
			}
		}
	}

	// Factory cap of one type by difficulty (Easy 4, Normal 10, Hard none). An outpost may still
	// get its factory past the cap; one factory of that type is then sold here.
	int GetFactoryTypeCap(HouseClass* pHouse)
	{
		const int diffIdx = pHouse ? static_cast<int>(pHouse->GetAIDifficultyIndex()) : 0;
		return diffIdx >= 2 ? 4 : (diffIdx == 1 ? 10 : INT_MAX);
	}

	// Sells one factory of a type above the difficulty cap (an outpost factory was placed past
	// it). Never the newest one (the outpost's own) nor one whose sale would leave a ConYard
	// (18 cells) or the newest forward stone of the offensive crawl (15 cells) without a factory
	// of that type, or the next check would ask for it again; outside the main base first.
	// Nothing qualifies: the extra factory stays. True when one was sold.
	bool SellFactoriesOverDifficultyCap(HouseClass* pHouse, const CellStruct& frontStoneCell)
	{
		const int cap = GetFactoryTypeCap(pHouse);
		if (!pHouse || cap == INT_MAX)
			return false;

		const CellStruct mainBase = (pHouse->BaseSpawnCell != CellStruct::Empty)
			? pHouse->BaseSpawnCell
			: (pHouse->ConYards.Count > 0 ? pHouse->ConYards.GetItem(0)->GetMapCoords() : CellStruct::Empty);

		auto chebyshev = [](const CellStruct& a, const CellStruct& b)
		{
			return std::max(std::abs(a.X - b.X), std::abs(a.Y - b.Y));
		};

		std::map<int, std::vector<BuildingClass*>> byType;
		std::unordered_set<int> typesUnderConstruction;

		for (auto const pBld : pHouse->Buildings)
		{
			if (!pBld || !pBld->IsAlive || pBld->InLimbo || !pBld->Type || pBld->Type->Factory == AbstractType::None
				|| pBld->Type->ConstructionYard)
			{
				continue;
			}

			if (pBld->CurrentMission == Mission::Selling || pBld->QueuedMission == Mission::Selling)
				continue;

			if (pBld->CurrentMission == Mission::Construction || pBld->QueuedMission == Mission::Construction)
				typesUnderConstruction.insert(pBld->Type->ArrayIndex);

			byType[pBld->Type->ArrayIndex].push_back(pBld);
		}

		// Anchors that need a factory of each type nearby, with their radius.
		std::vector<std::pair<CellStruct, int>> anchors;

		for (int i = 0; i < pHouse->ConYards.Count; ++i)
		{
			if (auto const pCY = pHouse->ConYards.GetItem(i); pCY && pCY->IsAlive && !pCY->InLimbo)
				anchors.emplace_back(pCY->GetMapCoords(), 18);
		}

		if (frontStoneCell != CellStruct::Empty)
			anchors.emplace_back(frontStoneCell, 15);

		for (auto& [typeIdx, blds] : byType)
		{
			if (static_cast<int>(blds.size()) <= cap || typesUnderConstruction.count(typeIdx))
				continue;

			BuildingClass* pNewest = blds.front();

			for (auto const pBld : blds)
			{
				if (pBld->UniqueID > pNewest->UniqueID)
					pNewest = pBld;
			}

			BuildingClass* pBest = nullptr;
			int bestTier = INT_MAX;

			for (auto const pBld : blds)
			{
				if (pBld == pNewest)
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

				const int tier = (mainBase != CellStruct::Empty && chebyshev(bc, mainBase) <= 20) ? 1 : 0;

				if (tier < bestTier)
				{
					bestTier = tier;
					pBest = pBld;
				}
			}

			if (pBest)
			{
				Debug::Log("[NeuralAI] House %d over the factory cap (%d) for %s: selling the one at (%d,%d).\n",
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

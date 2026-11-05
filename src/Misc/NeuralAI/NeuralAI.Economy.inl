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

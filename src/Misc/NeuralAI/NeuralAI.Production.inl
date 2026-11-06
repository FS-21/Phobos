// NeuralAI.Production.inl: Main AI tick and production.
// Part of NeuralAI.cpp (included in order; one translation unit).

namespace
{
	// Evaluates buildability strictly under Human Player rules:
	// - Requires valid human TechLevel (0..10), excluding TechLevel=-1 AI script
	// hacks/limbo dummies and TechLevel=11 disabled types
	// - Excludes legacy *_AI hack clones and TEST_BUILDING
	// - Temporarily sets IsHumanPlayer=true and AIBuildThis=true during
	// HouseClass::CanBuild so Ares/YRpp enforce real human Prerequisites,
	// Prerequisite.Negative, Owner/ForbiddenHouses, BuildLimit, and
	// PowersUpBuilding instead of AIBuildThis/PrerequisiteOverride
	bool CanBuildUnderHumanRules(HouseClass* pHouse, TechnoTypeClass* pType)
	{
		if (!pHouse || !pType)
			return false;

		if (pType->TechLevel < 0 || pType->TechLevel > 10 || pType->TechLevel > pHouse->TechLevel)
			return false;

		const char* id = pType->ID;

		if (!id || !id[0])
			return false;

		// Mod-configurable exclusions ([AI] NeuralAI.IgnoreTypes=): AI-only variants, fake
		// structures, test types... Never matched by type name in the engine.
		if (auto const pRulesExt = RulesExt::Global())
		{
			auto const& ignored = pRulesExt->NeuralAI_IgnoreTypes;

			if (std::find(ignored.begin(), ignored.end(), pType) != ignored.end())
				return false;
		}

		auto const pBld = abstract_cast<BuildingTypeClass*>(pType);

		if (pBld && pBld->SuperWeapon >= 0 && pBld->SuperWeapon < SuperWeaponTypeClass::Array.Count)
		{
			if (RulesClass::Instance->BuildTech.FindItemIndex(pBld) == -1)
			{
				if (auto const pSW = SuperWeaponTypeClass::Array.GetItem(pBld->SuperWeapon))
				{
					if (!IsTunnelNetworkBuilding(pBld))
					{
						if (auto const pSWExt = SWTypeExt::Fetch(pSW))
						{
							if (!pSWExt->SW_AllowAI)
								return false; // Respects SW01_Computers.ini .. SW14_Computers.ini

							// (SW.AllowAI=no)
						}
					}
				}
			}
		}

		// Evaluate full Phobos/Ares human prerequisites: [GenericPrerequisites],
		// Prerequisite<Name>Alternate, Prerequisite.Lists, Prerequisite.Negative,
		// Owner/RequiredHouses/ForbiddenHouses (with ParentCountry),
		// ConsideredSecretLabTech, BuildLimit/BuildLimitGroup, and PowersUpBuilding /
		// PowersUp.Buildings parent upgrade slots.
		if (!HouseExt::PrerequisitesMet(pHouse, pType))
			return false;

		const bool prevHuman = pHouse->IsHumanPlayer;
		const bool prevAIBuild = pBld ? pBld->AIBuildThis : true;

		pHouse->IsHumanPlayer = true;

		if (pBld)
			pBld->AIBuildThis = true;

		const CanBuildResult res = pHouse->CanBuild(pType, false, false);

		pHouse->IsHumanPlayer = prevHuman;

		if (pBld)
			pBld->AIBuildThis = prevAIBuild;

		return res == CanBuildResult::Buildable;
	}

	int CountOwnedUnitWithConversions(HouseClass* pHouse, const UnitTypeClass* pUnit)
	{
		if (!pHouse || !pUnit)
			return 0;

		int total = pHouse->CountOwnedNow(pUnit);
		const char* uiLabel = pUnit->UINameLabel;
		const TechnoTypeClass* pConvDeploy = GetConvertDeployTargetType(pUnit);

		for (auto const pOther : UnitTypeClass::Array)
		{
			if (!pOther || pOther == pUnit)
				continue;

			const bool sameUIName = (uiLabel[0] != '\0' && pOther->UINameLabel[0] != '\0'
				&& _stricmp(pOther->UINameLabel, uiLabel) == 0);
			const bool isDirectConvert = (pConvDeploy == pOther) || (GetConvertDeployTargetType(pOther) == pUnit);

			if (sameUIName || isDirectConvert)
				total += pHouse->CountOwnedNow(pOther);
		}

		return total;
	}
} // namespace

int NeuralAI::UpdateExpertAI(HouseClass* pHouse)
{
	if (!NeuralAI::IsEnabledForHouse(pHouse))
		return 30;

	if (!NeuralAI::IsCampaignRestricted(pHouse))
	{
		pHouse->Production = true;

		pHouse->AIState_1E4 = 0;
	}

	// Measures this whole AI tick (telemetry "aiMsAvg" / "aiMsMax").
	const NeuralTickTimer tickTimer { pHouse->ArrayIndex };

	// Map class policy correction (outside training: the trainer sends combined policies).
	if (!NeuralAI::HeadlessMode && MapClassLoaded && !MapClassApplied && NeuralAI::AllowBackpackInference)
	{
		ComputeMapInfo();
		const int mapClass = CurrentMapClass();

		if (mapClass >= 0)
		{
			MapClassApplied = true;
			MapClassOriginalBias = std::to_array(NeuralAI::Backpack.OutputBias);

			for (int i = 0; i < 128; ++i)
			{
				NeuralAI::Backpack.OutputBias[i] = static_cast<int16_t>(
					std::clamp(static_cast<int>(NeuralAI::Backpack.OutputBias[i]) + MapClassDeltas[mapClass][i], -1500, 1500));
			}

			Debug::Log("[NeuralAI] Map class %d policy correction applied.\n", mapClass);
		}
	}

	auto const pExt = HouseExt::Fetch(pHouse);
	const int diffIdx = pHouse->GetAIDifficultyIndex();
	const int evalDelay = (diffIdx == 0) ? 15 : ((diffIdx == 1) ? 45 : 105);

	// Every periodic window evaluated during this AI tick spans one full tick.
	const AITickCadenceScope cadenceScope { evalDelay, pHouse->ArrayIndex };

	std::array<int8_t, InputVectorSize> features;

	std::array<int16_t, OutputVectorSize> scores;

	{
		const NeuralSectionTimer sectionTimer { pHouse->ArrayIndex, NeuralSection::Features };
		UpdateBridgePlanner(pHouse);
		NeuralAI::ExtractFogFilteredFeatures(pHouse, features);
	}

	// The fog scan may have marked new enemy buildings as seen: rebuild the known-building cache.
	InvalidateKnownEnemyBuildings(pHouse);

	{
		const NeuralSectionTimer sectionTimer { pHouse->ArrayIndex, NeuralSection::MoE };
		NeuralAI::RunDeterministicMoE(pHouse, features, scores);
	}

	// Per-match style, opening and surprise tech switch on top of the trained scores.
	ApplyMatchStyle(pHouse, scores);

	pExt->NeuralAI_LastInferenceFrame = Unsorted::CurrentFrame;

	// Before any air lane is planned this tick: remember where our flyers were shot down.
	{
		const NeuralSectionTimer sectionTimer { pHouse->ArrayIndex, NeuralSection::Tactics };
		UpdateAirLossMemory(pHouse);
		UpdateMatchTactics(pHouse, scores);
	}

	{
		const NeuralSectionTimer sectionTimer { pHouse->ArrayIndex, NeuralSection::Micro };
		NeuralAI::ExecuteTacticalAndSpecOpsMicro(pHouse, scores);
	}

	// Strategic plans run after the tactical micro so they keep the last word on their units.
	{
		const NeuralSectionTimer sectionTimer { pHouse->ArrayIndex, NeuralSection::Plans };
		UpdateStrategicPlans(pHouse, scores);
	}

	// Release units parked by orders they cannot complete (destroyed bridge, endless jam).
	{
	const NeuralSectionTimer watchdogTimer { pHouse->ArrayIndex, NeuralSection::Watchdogs };
	UpdateStuckUnits(pHouse);

	// Empty harvesters waiting where the ore ran out go to the nearest free field.
	UpdateIdleHarvesters(pHouse);

	// Forward crawl stones placed / lost (trainer credit for the offensive crawl).
	UpdateOffensiveStones(pHouse);

	// Idle airships / jumpjet bombers near known enemy structures attack them.
	UpdateIdleAirborneStrikers(pHouse);

	// One scout at a time checks free ore fields before expanding there.
	UpdateFieldScouting(pHouse);

	// Best ore field not mined yet (Expert 6 hoards: claims contested fields too).
	UpdateResourceClaim(pHouse);

	// What each missing building would unlock in the tech tree.
	UpdateTechUnlocks(pHouse);

	// Highest unit tier allowed now (super units come step by step).
	UpdateUnitTierCap(pHouse);

	// Expert 0: economy peaks and crisis actions (blackout, broke, harvester balance).
	UpdateEconomyCrisis(pHouse);

	// Expert 3: which enemy to hit in a free-for-all.
	UpdateFfaTargets(pHouse);

	// Expert 2: which buildable types and missing tech answer the enemy army seen so far.
	UpdateCounterComposition(pHouse);

	// Expert 4: neutral techs seen, capture / spy / bridge work and the engineers and spies out.
	UpdateSpecOps(pHouse);
	}

	// Production: from here to the end of the tick.
	const NeuralSectionTimer productionTimer { pHouse->ArrayIndex, NeuralSection::Production };

	// Select next targets for Building, Unit, Infantry, and Aircraft queues
	const int money = pHouse->Available_Money();
	const int rawParentIdx = pHouse->Type ? pHouse->Type->FindParentCountryIndex() : -1;
	const int parentCountryIdx = (rawParentIdx >= 0) ? rawParentIdx : (pHouse->Type ? pHouse->Type->ArrayIndex2 : -1);
	const unsigned int ownHouseBit = (parentCountryIdx >= 0 && parentCountryIdx < 32) ? (1u << parentCountryIdx) : 0u;

	NeuralPhaseClock productionClock { pHouse->ArrayIndex, NeuralSection::ProdBuildings };

	// Building Target Selection
	if (NeuralAI::IsCampaignRestricted(pHouse))
	{
		// Campaign restriction: only build what is in Base.BaseNodes
		pExt->NeuralAI_NextBuildingTarget = -1;

		for (int i = 0; i < pHouse->Base.BaseNodes.Count; ++i)
		{
			const auto& node = pHouse->Base.BaseNodes.Items[i];

			if (!node.Placed && node.BuildingTypeIndex >= 0 && node.BuildingTypeIndex < BuildingTypeClass::Array.Count)
			{
				auto const pBldType = BuildingTypeClass::Array.GetItem(node.BuildingTypeIndex);

				if (pBldType && pHouse->CanBuild(pBldType, false, false) == CanBuildResult::Buildable)
				{
					pExt->NeuralAI_NextBuildingTarget = node.BuildingTypeIndex;

					break;
				}
			}
		}
	}
	else
	{
		int bestBldIdx = -1;
		int bestBldScore = -32000;
		int bestPowerPlantIdx = -1;
		int bestPowerPlantScore = -32000;

		// Count current owned macro & defense structures so the AI builds a
		// balanced macro base (Power -> Refinery -> Barracks -> War Factory -> 2nd
		// Refinery -> Radar -> Battle Lab -> Superweapons/High-Tier Defenses) and
		// NEVER spams an endless sea of cheap Tier-1 Pillboxes/Turrets or Power
		// Plants!
		int ownedRefineries = 0;
		int ownedBarracks = 0;
		int ownedWarFactories = 0;
		int ownedShipyards = 0;
		int ownedAirpads = 0;
		int ownedTechStructures = 0;
		int totalGroundDefenses = 0;
		int totalAADefenses = 0;
		int ownedBattleBunkers = 0;
		int emptyOwnedBattleBunkers = 0;
		int ownedTankBunkers = 0;
		int emptyOwnedTankBunkers = 0;
		int compatibleOccupierInfCount = 0;
		int compatibleBunkerableTankCount = 0;
		int ownedHarvestersCount = 0;
		bool hasTechCenter = false;
		int unfilledBioReactorSlots = 0;
		int bioReactorPotentialPower = 0;

		for (auto const pOwnInf : InfantryClass::Array)
		{
			if (pOwnInf && pOwnInf->Owner == pHouse && !pOwnInf->InLimbo && pOwnInf->IsAlive && pOwnInf->Type
				&& pOwnInf->Type->Occupier && !pOwnInf->Type->Engineer && !pOwnInf->Type->Infiltrate)
			{
				compatibleOccupierInfCount++;
			}
		}

		for (auto const pOwnU : UnitClass::Array)
		{
			if (pOwnU && pOwnU->Owner == pHouse && !pOwnU->InLimbo && pOwnU->IsAlive && pOwnU->Type
				&& pOwnU->Type->Bunkerable && !pOwnU->Type->Naval && !pOwnU->Type->Harvester
				&& !pOwnU->Type->ResourceGatherer && pOwnU->BunkerLinkedItem == nullptr)
			{
				compatibleBunkerableTankCount++;
			}
		}

		for (auto const pOwnBld : pHouse->Buildings)
		{
			if (!pOwnBld || pOwnBld->InLimbo || !pOwnBld->IsAlive || !pOwnBld->Type)
				continue;

			auto const pOT = pOwnBld->Type;

			if (pOT->InfantryAbsorb && (pOT->PowerBonus > 0 || pOT->ExtraPowerBonus > 0)
				&& !IsTunnelNetworkBuilding(pOT) && pOwnBld->Passengers.NumPassengers < pOT->Passengers)
			{
				const int freeSlots = pOT->Passengers - pOwnBld->Passengers.NumPassengers;
				unfilledBioReactorSlots += freeSlots;
				bioReactorPotentialPower
					+= freeSlots * std::max(50, pOT->ExtraPowerBonus > 0 ? pOT->ExtraPowerBonus : 100);
			}

			if (pOT->Refinery || pOT->ResourceGatherer || pOT->ResourceDestination)
			{
				ownedRefineries++;
			}
			else if (pOT->Factory == AbstractType::InfantryType)
			{
				ownedBarracks++;
			}
			else if (pOT->Factory == AbstractType::UnitType && !pOT->Naval)
			{
				ownedWarFactories++;
			}
			else if (pOT->Factory == AbstractType::UnitType && pOT->Naval)
			{
				ownedShipyards++;
			}
			else if (pOT->Factory == AbstractType::AircraftType || pOT->Helipad)
			{
				ownedAirpads++;
			}
			else if (pOT->IsBaseDefense && !IsTunnelNetworkBuilding(pOT))
			{
				const auto pWS = pOT->GetWeapon(0);
				const auto pW = pWS ? pWS->WeaponType : nullptr;
				const bool isUnarmedBunker = (pOT->CanBeOccupied || pOT->Bunker) && (pW == nullptr || pW->Damage <= 0);

				if (pOT->CanBeOccupied && isUnarmedBunker)
				{
					ownedBattleBunkers++;

					if (pOwnBld->Occupants.Count == 0)
						emptyOwnedBattleBunkers++;
				}

				if (pOT->Bunker && isUnarmedBunker)
				{
					ownedTankBunkers++;

					if (pOwnBld->BunkerLinkedItem == nullptr)
						emptyOwnedTankBunkers++;
				}

				const bool isPureAA = pW && pW->Projectile && pW->Projectile->AA && !pW->Projectile->AG;

				if (isPureAA)
				{
					totalAADefenses++;
				}
				else if (!isUnarmedBunker || pOwnBld->Occupants.Count > 0 || pOwnBld->BunkerLinkedItem != nullptr)
				{
					totalGroundDefenses++;
				}
			}
			else if (pOT->SpySat || pOT->OrePurifier
				|| (pOT->PowerBonus <= pOT->PowerDrain && !pOT->ConstructionYard && pOT->Storage == 0
					&& !pOT->UnitRepair))
			{
				ownedTechStructures++;

				if ((RulesClass::Instance && RulesClass::Instance->BuildTech.FindItemIndex(pOT) != -1)
					|| (pOT->TechLevel >= 6 && pOT->Cost >= 1200))
				{
					hasTechCenter = true;
				}
			}
		}

		// Also count mobile Slave Miners (e.g. Yuri's SMIN when undeployed from
		// YAREFN to harvest ore) as owned Refineries, and count active Harvesters
		for (auto const pOwnUnit : UnitClass::Array)
		{
			if (!pOwnUnit || pOwnUnit->Owner != pHouse || pOwnUnit->InLimbo || !pOwnUnit->IsAlive || !pOwnUnit->Type)
				continue;

			if (pOwnUnit->Type->Harvester || pOwnUnit->Type->ResourceGatherer)
				ownedHarvestersCount++;

			if (auto const pDeploys = pOwnUnit->Type->DeploysInto)
			{
				if (pDeploys->Refinery || pDeploys->ResourceGatherer || pDeploys->ResourceDestination)
				{
					ownedRefineries++;

					ownedHarvestersCount++;
				}
			}
		}

		const int houseStagger = (std::max(0, pHouse->ArrayIndex) % 8) * 31;

		// Sell any temporary Harvester Recovery Refinery once its buildup finishes
		// and its FreeUnit Harvester has spawned (from AdvAI)
		if ((Unsorted::CurrentFrame + houseStagger) % 15 == 0
			&& !GetMacroState(pHouse).TemporaryRecoveryRefineryPlacedFrame.empty())
		{
			auto& tempMap = GetMacroState(pHouse).TemporaryRecoveryRefineryPlacedFrame;
			for (auto const pBld : pHouse->Buildings)
			{
				if (!pBld || !pBld->IsAlive || pBld->InLimbo)
					continue;

				auto const it = tempMap.find(pBld->UniqueID);
				if (it == tempMap.end())
					continue;

				if (pBld->CurrentMission == Mission::Selling || pBld->QueuedMission == Mission::Selling)
				{
					tempMap.erase(it);
					break;
				}

				if (pBld->CurrentMission != Mission::Construction && pBld->QueuedMission != Mission::Construction
					&& pBld->BState != static_cast<int>(BStateType::Construction)
					&& Unsorted::CurrentFrame >= it->second + 75)
				{
					for (auto const pUnit : UnitClass::Array)
					{
						if (pUnit && pUnit->Owner == pHouse && pUnit->IsAlive && !pUnit->InLimbo && pUnit->Type
							&& (pUnit->Type->Harvester || pUnit->Type->ResourceGatherer))
						{
							if (pUnit->GetMapCoords().DistanceFrom(pBld->GetMapCoords()) <= 8.0
								&& pUnit->CurrentMission != Mission::Harvest)
							{
								pUnit->QueueMission(Mission::Harvest, false);
							}
						}
					}

					tempMap.erase(it);
					GetMacroState(pHouse).NextHarvesterRecoveryFrame = Unsorted::CurrentFrame + 1800;
					Debug::Log("[NeuralAI] HarvesterRecovery: House %d selling temporary "
							"recovery refinery %s at (%d,%d) after spawning FreeUnit "
							"harvester.\n",
						pHouse->ArrayIndex, pBld->Type->ID, pBld->GetMapCoords().X, pBld->GetMapCoords().Y);
					pBld->Sell(1);
					break;
				}
			}
		}

		// Periodic Obsolete Refinery Recycling & Emergency Cash Recovery (from
		// AdvAI)
		if (Unsorted::CurrentFrame > pExt->LastObsoleteRefineryCheckFrame + 450 + houseStagger)
		{
			pExt->LastObsoleteRefineryCheckFrame = Unsorted::CurrentFrame;
			auto& relaxedReach = GetMacroState(pHouse).RelaxedReachRefineries;
			relaxedReach.erase(std::remove_if(relaxedReach.begin(), relaxedReach.end(), [pHouse](DWORD id)
				{
					return std::none_of(pHouse->Buildings.begin(), pHouse->Buildings.end(),
						[id](BuildingClass* pB) { return pB && pB->IsAlive && pB->UniqueID == id; });
				}), relaxedReach.end());
			RecycleObsoleteRefineries(pHouse, pExt->NeuralAI_ActiveExpert == 6, relaxedReach);
			if (SellFactoriesOverDifficultyCap(pHouse, GetNewestOffensiveStoneCell(pHouse)))
				GetMacroState(pHouse).LastFactoryCapSaleFrame = Unsorted::CurrentFrame;
			RaiseEmergencyMoneyForRefinery(pHouse, ownedRefineries, ownedHarvestersCount, money);
		}

		// Expert 1 Selective Wall Unblocking & Demolition:
		// - If an obstructing wall belongs to the AI house, sell that specific wall cell/segment.
		// - If an obstructing wall is neutral or enemy and is destructible (!Immune), dispatch a
		//   nearby friendly unit/infantry with a wall-capable warhead (or crusher) to demolish it!
		if ((Unsorted::CurrentFrame + houseStagger) % 45 == 0)
		{
			auto canFootDemolishWall = [](FootClass* pFoot, bool wallCrushable, Armor wallArmor) -> bool
			{
				if (!pFoot || !pFoot->IsAlive || pFoot->InLimbo)
					return false;

				if (auto const pU = abstract_cast<UnitClass*>(pFoot))
				{
					if (pU->Type && (pU->Type->Harvester || pU->Type->ResourceGatherer))
						return false;

					if (wallCrushable && pU->Type && pU->Type->Crusher)
						return true;
				}

				for (int wIdx = 0; wIdx < 2; ++wIdx)
				{
					auto const pWS = pFoot->GetWeapon(wIdx);
					auto const pWpn = pWS ? pWS->WeaponType : nullptr;
					if (!pWpn || pWpn->Damage <= 0 || !pWpn->Warhead)
						continue;

					auto const pWH = pWpn->Warhead;
					if (pWH->Wall || pWH->WallAbsoluteDestroyer || (wallArmor == Armor::Wood && pWH->Wood))
						return true;
				}

				return false;
			};

			auto dispatchWallDemolition = [&](AbstractClass* pTarget, const CellStruct& wc, bool wallCrushable,
											Armor wallArmor, FootClass* pBlockedFoot) -> bool
			{
				if (pBlockedFoot && canFootDemolishWall(pBlockedFoot, wallCrushable, wallArmor))
				{
					if (pBlockedFoot->Target != pTarget)
					{
						pBlockedFoot->SetTarget(pTarget);
						pBlockedFoot->QueueMission(Mission::Attack, true);
					}

					return true;
				}

				FootClass* pBestDemo = nullptr;
				int bestDistSq = 14 * 14 + 1;

				for (auto const pOwnU : UnitClass::Array)
				{
					if (!pOwnU || pOwnU->Owner != pHouse || !canFootDemolishWall(pOwnU, wallCrushable, wallArmor))
						continue;

					if (pOwnU->Target == pTarget)
						return true;

					if (pOwnU->Target != nullptr || pOwnU->Destination != nullptr
						|| (pOwnU->CurrentMission != Mission::Guard && pOwnU->CurrentMission != Mission::Area_Guard))
					{
						continue;
					}

					const CellStruct uc = pOwnU->GetMapCoords();
					const int dx = static_cast<int>(uc.X) - static_cast<int>(wc.X);
					const int dy = static_cast<int>(uc.Y) - static_cast<int>(wc.Y);
					const int dSq = dx * dx + dy * dy;
					if (dSq < bestDistSq)
					{
						bestDistSq = dSq;
						pBestDemo = pOwnU;
					}
				}

				for (auto const pOwnI : InfantryClass::Array)
				{
					if (!pOwnI || pOwnI->Owner != pHouse || !canFootDemolishWall(pOwnI, false, wallArmor))
						continue;

					if (pOwnI->Target == pTarget)
						return true;

					if (pOwnI->Target != nullptr || pOwnI->Destination != nullptr
						|| (pOwnI->CurrentMission != Mission::Guard && pOwnI->CurrentMission != Mission::Area_Guard))
					{
						continue;
					}

					const CellStruct ic = pOwnI->GetMapCoords();
					const int dx = static_cast<int>(ic.X) - static_cast<int>(wc.X);
					const int dy = static_cast<int>(ic.Y) - static_cast<int>(wc.Y);
					const int dSq = dx * dx + dy * dy;
					if (dSq < bestDistSq)
					{
						bestDistSq = dSq;
						pBestDemo = pOwnI;
					}
				}

				if (pBestDemo)
				{
					pBestDemo->SetTarget(pTarget);
					pBestDemo->QueueMission(Mission::Attack, true);

					return true;
				}

				return false;
			};

			auto tryUnblockWallAtCell = [&](const CellStruct& wc, FootClass* pBlockedFoot) -> bool
			{
				auto const pWCell = MapClass::Instance.TryGetCellAt(wc);
				if (!pWCell)
					return false;

				if (pWCell->OverlayTypeIndex >= 0)
				{
					if (auto const pOv = OverlayTypeClass::Array.GetItem(pWCell->OverlayTypeIndex))
					{
						if (pOv->Wall)
						{
							if (pWCell->WallOwnerIndex == pHouse->ArrayIndex)
							{
								CellStruct wallCell = wc;
								pHouse->SellWall(wallCell, true);

								return true;
							}

							const bool isAlliedWall = (pWCell->WallOwnerIndex >= 0
								&& pWCell->WallOwnerIndex < HouseClass::Array.Count
								&& pHouse->IsAlliedWith(HouseClass::Array.GetItem(pWCell->WallOwnerIndex)));
							if (!isAlliedWall && !pOv->Immune)
							{
								if (dispatchWallDemolition(pWCell, wc, pOv->Crushable, pOv->Armor, pBlockedFoot))
									return true;
							}
						}
					}
				}

				if (auto const pWBld = pWCell->GetBuilding())
				{
					if (pWBld->IsAlive && !pWBld->InLimbo && pWBld->Type
						&& (pWBld->Type->Wall || pWBld->Type->FirestormWall || pWBld->Type->LaserFence
							|| pWBld->Type->LaserFencePost))
					{
						if (pWBld->Owner == pHouse && pWBld->CurrentMission != Mission::Selling)
						{
							pWBld->Sell(1);

							return true;
						}

						if (pWBld->Owner != pHouse && (!pWBld->Owner || !pHouse->IsAlliedWith(pWBld->Owner))
							&& !pWBld->Type->Immune)
						{
							if (dispatchWallDemolition(pWBld, wc, false, pWBld->Type->Armor, pBlockedFoot))
								return true;
						}
					}
				}

				return false;
			};

			bool unblockedOneWall = false;
			for (auto const pOwnB : pHouse->Buildings)
			{
				if (unblockedOneWall || !pOwnB || pOwnB->InLimbo || !pOwnB->IsAlive || !pOwnB->Type)
					continue;

				if (pOwnB->Type->Factory == AbstractType::None && !pOwnB->Type->Refinery
					&& !pOwnB->Type->ResourceDestination && !pOwnB->Type->UnitRepair)
				{
					continue;
				}

				const CellStruct bc = pOwnB->GetMapCoords();
				const int bw = pOwnB->Type->GetFoundationWidth();
				const int bh = pOwnB->Type->GetFoundationHeight(false);
				for (int dy = -1; dy <= bh + 1 && !unblockedOneWall; ++dy)
				{
					for (int dx = -1; dx <= bw + 1 && !unblockedOneWall; ++dx)
					{
						if (dx >= 0 && dx < bw && dy >= 0 && dy < bh)
							continue;

						if (tryUnblockWallAtCell(
								CellStruct { static_cast<short>(bc.X + dx), static_cast<short>(bc.Y + dy) }, nullptr))
						{
							unblockedOneWall = true;
						}
					}
				}
			}

			for (auto const pOwnU : UnitClass::Array)
			{
				if (unblockedOneWall || !pOwnU || pOwnU->Owner != pHouse || pOwnU->InLimbo || !pOwnU->IsAlive
					|| !pOwnU->Destination)
				{
					continue;
				}

				if (pOwnU->CurrentMission != Mission::Move && pOwnU->CurrentMission != Mission::Harvest
					&& pOwnU->CurrentMission != Mission::AttackMove && pOwnU->CurrentMission != Mission::Enter)
				{
					continue;
				}

				const CellStruct uc = pOwnU->GetMapCoords();
				const CoordStruct destC = pOwnU->Destination->GetCoords();
				const CellStruct dc = CellClass::Coord2Cell(destC);
				const int stepX = std::clamp(static_cast<int>(dc.X) - static_cast<int>(uc.X), -1, 1);
				const int stepY = std::clamp(static_cast<int>(dc.Y) - static_cast<int>(uc.Y), -1, 1);
				if (stepX != 0 || stepY != 0)
				{
					const CellStruct aheadCell { static_cast<short>(uc.X + stepX), static_cast<short>(uc.Y + stepY) };
					if (tryUnblockWallAtCell(aheadCell, pOwnU))
						unblockedOneWall = true;
				}
			}

			for (auto const pOwnI : InfantryClass::Array)
			{
				if (unblockedOneWall || !pOwnI || pOwnI->Owner != pHouse || pOwnI->InLimbo || !pOwnI->IsAlive
					|| !pOwnI->Destination)
				{
					continue;
				}

				if (pOwnI->CurrentMission != Mission::Move && pOwnI->CurrentMission != Mission::AttackMove
					&& pOwnI->CurrentMission != Mission::Enter)
				{
					continue;
				}

				const CellStruct ic = pOwnI->GetMapCoords();
				const CoordStruct destC = pOwnI->Destination->GetCoords();
				const CellStruct dc = CellClass::Coord2Cell(destC);
				const int stepX = std::clamp(static_cast<int>(dc.X) - static_cast<int>(ic.X), -1, 1);
				const int stepY = std::clamp(static_cast<int>(dc.Y) - static_cast<int>(ic.Y), -1, 1);
				if (stepX != 0 || stepY != 0)
				{
					const CellStruct aheadCell { static_cast<short>(ic.X + stepX), static_cast<short>(ic.Y + stepY) };
					if (tryUnblockWallAtCell(aheadCell, pOwnI))
						unblockedOneWall = true;
				}
			}
		}

		// Periodic Dynamic Primary Factory Switching (from AdvAI)
		if (Unsorted::CurrentFrame > pExt->LastPrimaryFactoryCheckFrame + 60 + houseStagger)
		{
			pExt->LastPrimaryFactoryCheckFrame = Unsorted::CurrentFrame;

			CellStruct frontTarget = pExt->NeuralAI_AttackTargetCell;

			if (frontTarget == CellStruct::Empty && pHouse->EnemyHouseIndex >= 0
				&& pHouse->EnemyHouseIndex < HouseClass::Array.Count)
			{
				if (auto const pEnemy = HouseClass::Array.GetItem(pHouse->EnemyHouseIndex))
					frontTarget = pEnemy->BaseCenter != CellStruct::Empty ? pEnemy->BaseCenter : pEnemy->BaseSpawnCell;
			}

			UpdatePrimaryFactories(pHouse, frontTarget);
		}

		// Real Power Drain & Damaged-Plant Safety Cushion:
		// Maintain a generous buffer (+150 baseline, scaling to +300..+550 when
		// owning powered defenses/superweapons or large reactors) so 1 damaged or
		// destroyed Power Plant never plunges the base into LowPower!
		bool hasUnpoweredBuildings = false;
		int poweredDefAndSWCount = 0;
		int maxSinglePlantBonus = 0;
		bool hasAdvancedPowerPlants = false;

		const int realPowerDrain = GetRealPowerDrainAndGridMetrics(
			pHouse, hasUnpoweredBuildings, poweredDefAndSWCount, maxSinglePlantBonus, hasAdvancedPowerPlants);

		const int realPowerSurplus = pHouse->PowerOutput - realPowerDrain;
		const int plantLossBuffer = std::min(500, std::max(150, (maxSinglePlantBonus * 3) / 4));

		const int requiredPowerCushion = (poweredDefAndSWCount > 0)
			? std::max(plantLossBuffer, std::min(550, 220 + poweredDefAndSWCount * 45))
			: std::max(150, plantLossBuffer);

		// Competitive MultipleFactory Arms Race:
		// Measure strongest enemy's factory counts and scale our factory limits by
		// both economy (ownedRefineries) AND enemy production capacity!
		int maxEnemyWarFactories = 0;
		int maxEnemyBarracks = 0;
		int maxEnemyShipyards = 0;
		int maxEnemyAirpads = 0;

		// Fog-fair: only enemy factories this house has actually seen count (the
		// HouseClass::Num* counters would reveal the whole enemy base through the fog).
		{
			std::array<int, 32> knownWarFactories {};
			std::array<int, 32> knownBarracks {};
			std::array<int, 32> knownShipyards {};
			std::array<int, 32> knownAirpads {};

			for (auto const pEnemyBld : BuildingClass::Array)
			{
				if (!pEnemyBld || pEnemyBld->InLimbo || !pEnemyBld->IsAlive || !pEnemyBld->Owner || !pEnemyBld->Type)
					continue;

				auto const pOther = pEnemyBld->Owner;

				if (pOther == pHouse || pHouse->IsAlliedWith(pOther) || pOther->IsNeutral() || pOther->IsObserver()
					|| pOther->Defeated || !IsEnemyBuildingKnown(pHouse, pEnemyBld))
				{
					continue;
				}

				const int ownerIdx = std::clamp(pOther->ArrayIndex, 0, 31);
				auto const pT = pEnemyBld->Type;

				if (pT->Factory == AbstractType::UnitType && !pT->Naval)
					maxEnemyWarFactories = std::max(maxEnemyWarFactories, ++knownWarFactories[ownerIdx]);
				else if (pT->Factory == AbstractType::UnitType && pT->Naval)
					maxEnemyShipyards = std::max(maxEnemyShipyards, ++knownShipyards[ownerIdx]);
				else if (pT->Factory == AbstractType::InfantryType)
					maxEnemyBarracks = std::max(maxEnemyBarracks, ++knownBarracks[ownerIdx]);
				else if (pT->Factory == AbstractType::AircraftType || pT->Helipad)
					maxEnemyAirpads = std::max(maxEnemyAirpads, ++knownAirpads[ownerIdx]);
			}
		}

		const int ecoWarFactoryCap = (ownedRefineries >= 6) ? (ownedRefineries / 2) : (1 + ownedRefineries / 3);

		// Banked money is wasted production: with a big bank, extra factories (production speed
		// multipliers stack) turn it into an army. One step per 15000 credits (max 6), only while
		// placements succeed (there is room to build).
		// Only when (a) there is room to build: a real placement probe for a war factory every 900
		// frames (narrow maps) plus no streak of failed placements, and (b) the tech tree is
		// mostly covered: >= 80% of the non-defense structures we can build now are owned, so
		// the money does not go into factories before the tech that makes their units good.
		auto& bankMacro = GetMacroState(pHouse);

		if (money >= 15000 && Unsorted::CurrentFrame - bankMacro.BankCheckFrame >= 900)
		{
			bankMacro.BankCheckFrame = Unsorted::CurrentFrame;
			int buildableTypes = 0;
			int ownedTypes = 0;
			BuildingTypeClass* pRoomProbe = nullptr;

			for (auto const pBT : BuildingTypeClass::Array)
			{
				// Defenses count too: the tech tree is "covered" when we own one of each kind.
				if (!pBT || pBT->Wall || pBT->Insignificant || pBT->InvisibleInGame || pBT->Cost <= 0
					|| pBT->ConstructionYard || !CanBuildUnderHumanRules(pHouse, pBT))
				{
					continue;
				}

				buildableTypes++;

				if (pHouse->CountOwnedNow(pBT) > 0)
					ownedTypes++;

				if (!pRoomProbe && pBT->Factory == AbstractType::UnitType && !pBT->Naval)
					pRoomProbe = pBT;
			}

			bankMacro.TechCoveragePct = buildableTypes > 0 ? (ownedTypes * 100) / buildableTypes : 100;
			bankMacro.BankFactoryRoom = pRoomProbe && CachedTetrisBuildCell(pHouse, pRoomProbe) != CellStruct::Empty;
		}

		// Never a production loop: the boost only exists in every other 900-frame window (in the
		// others, defenses and tech compete as before), at most one boosted factory per window,
		// and none once a placement failed in the window (until the next room probe).
		const int bankWindow = Unsorted::CurrentFrame / 900;
		const int ownedFactoriesNow = pHouse->NumWarFactories + pHouse->NumBarracks;

		if (bankWindow != bankMacro.BankWindow)
		{
			bankMacro.BankWindow = bankWindow;
			bankMacro.BankWindowFactories = ownedFactoriesNow;
			bankMacro.BankWindowFailures = bankMacro.ConsecutivePlacementFailures;
		}

		const bool bankWindowOpen = (bankWindow % 2) == 0 && ownedFactoriesNow <= bankMacro.BankWindowFactories
			&& bankMacro.ConsecutivePlacementFailures <= bankMacro.BankWindowFailures;

		// Expansion first: while one of our MCVs is on its way to expand, the bank pays for that.
		bool expansionMcvUnderway = false;

		for (auto const pOwnUnit : UnitClass::Array)
		{
			if (pOwnUnit && pOwnUnit->Owner == pHouse && pOwnUnit->IsAlive && !pOwnUnit->InLimbo && pOwnUnit->Type
				&& pOwnUnit->Type->DeploysInto && pOwnUnit->Type->DeploysInto->ConstructionYard)
			{
				expansionMcvUnderway = true;
				break;
			}
		}

		// No fixed cap: room bounds it (the placement probe and the failure checks above), so a
		// huge map can keep turning its bank into factories.
		const int bankSteps = (money >= 15000 && bankMacro.TechCoveragePct >= 90 && bankMacro.BankFactoryRoom
			&& bankMacro.ConsecutivePlacementFailures <= 2 && bankWindowOpen && !expansionMcvUnderway)
			? money / 15000
			: 0;

		// Offensive fronts: forward stones alive raise the factory caps (2 more per 4 stones, up
		// to 8), so the hot zones keep receiving factories after the home base has its share.
		const int frontFactoryRoom = std::min(8, static_cast<int>(bankMacro.OffensiveStones.size()) / 2);

		const int maxWarFactories = (hasTechCenter || money >= 2400)
			? std::clamp(std::max(ecoWarFactoryCap, maxEnemyWarFactories + 1), 2, 7) + bankSteps + frontFactoryRoom
			: std::clamp(ecoWarFactoryCap, 1, 3);

		const int ecoBarracksCap = (ownedRefineries >= 4) ? 2 : 1;

		const int maxBarracks = (hasTechCenter || money >= 2500)
			? std::clamp(std::max(ecoBarracksCap, maxEnemyBarracks), 2, 4) + bankSteps / 2 + frontFactoryRoom / 2
			: std::clamp(ecoBarracksCap, 1, 2);

		const int maxShipyards
			= (hasTechCenter && maxEnemyShipyards >= 2) ? std::clamp(maxEnemyShipyards + 1, 2, 4) : 2;

		int ownedDockingAircraft = 0;
		for (auto const pOwnAir : AircraftClass::Array)
		{
			if (pOwnAir && pOwnAir->Owner == pHouse && !pOwnAir->InLimbo && pOwnAir->IsAlive && pOwnAir->Type
				&& !pOwnAir->SpawnOwner && (pOwnAir->Type->AirportBound || pOwnAir->Type->Dock.Count > 0))
			{
				ownedDockingAircraft++;
			}
		}

		const bool expert8NeedsMoreAirpadDocks = (ownedAirpads >= 1 && ownedDockingAircraft >= pHouse->AirportDocks
			&& (scores[32] > 160 || scores[33] > 160 || pExt->NeuralAI_ActiveExpert == 8
				|| pExt->NeuralAI_Archetype == 6));
		const int dockDemandAirpadCap = expert8NeedsMoreAirpadDocks ? std::clamp((ownedDockingAircraft / 4) + 2, 2, 6) : 2;
		const int maxAirpads = std::max(
			dockDemandAirpadCap, (hasTechCenter && maxEnemyAirpads >= 2) ? std::clamp(maxEnemyAirpads + 1, 2, 5) : 2);

		// Threat & Defense Moderation Analysis:
		// Prune stale attack records and check if our base was recently attacked
		PruneAttackedBuildingsLIFO(pHouse);

		auto& macroState = GetMacroState(pHouse);
		if (macroState.FrontlineThreatNeedsDefenses > 0
			&& Unsorted::CurrentFrame > macroState.FrontlineThreatActiveFrames)
		{
			macroState.FrontlineThreatNeedsDefenses = 0;
			macroState.FrontlineThreatCoords = CellStruct::Empty;
			macroState.FrontlineThreatBuildingCoords = CellStruct::Empty;
		}

		const bool frontlineThreatActive = (macroState.FrontlineThreatNeedsDefenses > 0
			&& Unsorted::CurrentFrame <= macroState.FrontlineThreatActiveFrames);

		const bool wasRecentlyAttacked = (pHouse->LATime > 0 && (Unsorted::CurrentFrame - pHouse->LATime) < 1350)
			|| !macroState.AttackedBuildingsLIFO.empty();

		bool underInfantryAttack = false;
		bool underVehicleAttack = false;
		bool underAirAttack = false;
		NeuralThreatCategory activeThreatCat = NeuralThreatCategory::None;

		for (int i = static_cast<int>(macroState.AttackedBuildingsLIFO.size()) - 1; i >= 0; --i)
		{
			const auto& rec = macroState.AttackedBuildingsLIFO[i];
			if (activeThreatCat == NeuralThreatCategory::None)
				activeThreatCat = rec.Threat;

			if (rec.Threat == NeuralThreatCategory::Infantry)
				underInfantryAttack = true;
			else if (rec.Threat == NeuralThreatCategory::Vehicle)
				underVehicleAttack = true;
			else if (rec.Threat == NeuralThreatCategory::Air)
				underAirAttack = true;
		}

		const CellStruct homeRefCenter = (pHouse->ConYards.Count > 0 && pHouse->ConYards.GetItem(0))
			? pHouse->ConYards.GetItem(0)->GetMapCoords()
			: (pHouse->BaseCenter != CellStruct::Empty ? pHouse->BaseCenter : pHouse->BaseSpawnCell);

		if (activeThreatCat == NeuralThreatCategory::None && (wasRecentlyAttacked || frontlineThreatActive))
		{
			const CellStruct threatCenter
				= (frontlineThreatActive && macroState.FrontlineThreatBuildingCoords != CellStruct::Empty)
				? macroState.FrontlineThreatBuildingCoords
				: homeRefCenter;
			activeThreatCat = GetActiveThreatCategory(pHouse, threatCenter);
			if (activeThreatCat == NeuralThreatCategory::Infantry)
				underInfantryAttack = true;
			else if (activeThreatCat == NeuralThreatCategory::Vehicle)
				underVehicleAttack = true;
			else if (activeThreatCat == NeuralThreatCategory::Air)
				underAirAttack = true;
		}

		const int enemyAirValue = CalculateEnemyAircraftValue(pHouse);
		const bool hasCoreEconomyAndProduction = (ownedRefineries >= 1 && ownedBarracks >= 1 && ownedWarFactories >= 1);

		const bool isBehindInFactories = hasCoreEconomyAndProduction
			&& ((ownedWarFactories < maxEnemyWarFactories && ownedRefineries >= 2) || (ownedRefineries < 2));

		const bool hasPendingCoreTechOrEco = !hasTechCenter || isBehindInFactories || (ownedRefineries < 2);

		// Detect remote captured Tech Buildings (Oil Derricks, Tech Outposts,
		// Hospitals, Airfields) & remote Expansion Refineries that are far from our
		// main base (> 16 cells) and currently lack a nearby point-defense turret
		// within 10 cells!
		int undefendedRemoteAssetsCount = 0;
		std::vector<CellStruct> scanOrigins;
		if (homeRefCenter != CellStruct::Empty)
			scanOrigins.push_back(homeRefCenter);

		for (int yi = 1; yi < pHouse->ConYards.Count; ++yi)
		{
			if (auto const pCY = pHouse->ConYards.GetItem(yi))
				scanOrigins.push_back(pCY->GetMapCoords());
		}

		for (auto const pOwnBld : pHouse->Buildings)
		{
			if (!pOwnBld || pOwnBld->InLimbo || !pOwnBld->IsAlive || !pOwnBld->Type || pOwnBld->Type->InvisibleInGame)
				continue;

			auto const pOT = pOwnBld->Type;
			const CellStruct bc = pOwnBld->GetMapCoords();
			const bool isCapturedTechOrCash = (pOT->ProduceCashAmount > 0 || pOT->ProduceCashStartup > 0
				|| (pOT->Capturable && pOT->NeedsEngineer));
			const bool isRemoteFromHome = (homeRefCenter != CellStruct::Empty
				&& std::max(std::abs(bc.X - homeRefCenter.X), std::abs(bc.Y - homeRefCenter.Y)) > 16);

			if ((isCapturedTechOrCash || isRemoteFromHome)
				&& (pOT->BaseNormal || pOT->ConstructionYard || BuildingTypeExt::IsAIBaseNormal(pOT)))
			{
				bool nearExistingOrigin = false;
				for (const CellStruct& so : scanOrigins)
				{
					if (std::max(std::abs(bc.X - so.X), std::abs(bc.Y - so.Y)) <= 24)
					{
						nearExistingOrigin = true;
						break;
					}
				}

				if (!nearExistingOrigin && scanOrigins.size() < 5)
					scanOrigins.push_back(bc);
			}

			if (isCapturedTechOrCash
				|| (isRemoteFromHome && (pOT->Refinery || pOT->ConstructionYard || pOT->Factory != AbstractType::None)))
			{
				int nearbyGroundTurrets = 0;
				for (auto const pDefBld : pHouse->Buildings)
				{
					if (!pDefBld || pDefBld->InLimbo || !pDefBld->IsAlive || !pDefBld->Type
						|| !pDefBld->Type->IsBaseDefense || IsTunnelNetworkBuilding(pDefBld->Type))
					{
						continue;
					}

					const auto pWS = pDefBld->Type->GetWeapon(0);
					const auto pW = pWS ? pWS->WeaponType : nullptr;
					const bool isEmptyUnarmedBunker = (pDefBld->Type->CanBeOccupied || pDefBld->Type->Bunker)
						&& (pW == nullptr || pW->Damage <= 0) && pDefBld->Occupants.Count == 0
						&& pDefBld->BunkerLinkedItem == nullptr;
					if (isEmptyUnarmedBunker)
						continue;

					const bool pureAA = pW && pW->Projectile && pW->Projectile->AA && !pW->Projectile->AG;
					if (pureAA)
						continue;

					const CellStruct dc = pDefBld->GetMapCoords();
					if (std::max(std::abs(bc.X - dc.X), std::abs(bc.Y - dc.Y)) <= 10)
						nearbyGroundTurrets++;
				}

				if (nearbyGroundTurrets < 2)
					undefendedRemoteAssetsCount += (2 - nearbyGroundTurrets);
			}
		}

		// Continuous Frontline Proximity, Base-Crawl Siege & Resource-Choke Defense Deficit Scan:
		// Check if any owned building is close to enemy structures (<= 26 cells) or enemy ground units
		// (<= 18 cells) or contesting an Ore/Gem zone where we have fewer than 3..4 local ground defenses!
		int frontlineDefenseDeficit = 0;
		bool enemyCloseToOurBuildings = false;
		CellStruct urgentFrontlineBldCell = CellStruct::Empty;
		CellStruct urgentFrontlineEnemyCell = CellStruct::Empty;
		int bestFrontlineUrgency = 0;

		for (auto const pOwnBld : pHouse->Buildings)
		{
			if (!pOwnBld || pOwnBld->InLimbo || !pOwnBld->IsAlive || !pOwnBld->Type || pOwnBld->Type->Insignificant
				|| pOwnBld->Type->InvisibleInGame || pOwnBld->Type->IsBaseDefense)
			{
				continue;
			}

			const CellStruct bc = pOwnBld->GetMapCoords();
			int closestEnemyDist = 9999;
			CellStruct closestEnemyPos = CellStruct::Empty;
			int nearbyEnemyCount = 0;

			for (auto const pEB : BuildingClass::Array)
			{
				if (!pEB || pEB->InLimbo || !pEB->IsAlive || !pEB->Owner || !pEB->Type || pEB->Type->Insignificant
					|| pEB->Type->InvisibleInGame || pEB->Owner == pHouse || pHouse->IsAlliedWith(pEB->Owner) || !IsEnemyBuildingKnown(pHouse, pEB)
					|| pEB->Owner->IsNeutral() || pEB->Owner->IsObserver() || pEB->Owner->Defeated)
				{
					continue;
				}

				const CellStruct ec = pEB->GetMapCoords();
				const int dCheb = std::max(std::abs(bc.X - ec.X), std::abs(bc.Y - ec.Y));
				if (dCheb <= 26)
				{
					nearbyEnemyCount += (pEB->Type->IsBaseDefense || pEB->Type->Refinery || pEB->Type->Factory != AbstractType::None) ? 2 : 1;
					if (dCheb < closestEnemyDist)
					{
						closestEnemyDist = dCheb;
						closestEnemyPos = ec;
					}
				}
			}

			for (auto const pEU : UnitClass::Array)
			{
				if (!pEU || pEU->InLimbo || !pEU->IsAlive || !pEU->Owner || !pEU->Type || pEU->Owner == pHouse
					|| pHouse->IsAlliedWith(pEU->Owner) || !IsCellVisibleToAI(pHouse, pEU->GetCell()) || pEU->Owner->IsNeutral() || pEU->Owner->IsObserver()
					|| pEU->Owner->Defeated)
				{
					continue;
				}

				const CellStruct euc = pEU->GetMapCoords();
				const int dCheb = std::max(std::abs(bc.X - euc.X), std::abs(bc.Y - euc.Y));
				if (dCheb <= 20)
				{
					nearbyEnemyCount += (pEU->Type->Harvester || pEU->Type->ResourceGatherer) ? 2 : 1;
					if (dCheb < closestEnemyDist)
					{
						closestEnemyDist = dCheb;
						closestEnemyPos = euc;
					}
				}
			}

			if (closestEnemyPos == CellStruct::Empty)
				continue;

			enemyCloseToOurBuildings = true;
			int localGroundDefsNearBld = 0;
			for (auto const pDefBld : pHouse->Buildings)
			{
				if (!pDefBld || pDefBld->InLimbo || !pDefBld->IsAlive || !pDefBld->Type
					|| !pDefBld->Type->IsBaseDefense || IsTunnelNetworkBuilding(pDefBld->Type))
				{
					continue;
				}

				const auto pWS = pDefBld->Type->GetWeapon(0);
				const auto pW = pWS ? pWS->WeaponType : nullptr;
				const bool isEmptyUnarmedBunker = (pDefBld->Type->CanBeOccupied || pDefBld->Type->Bunker)
					&& (pW == nullptr || pW->Damage <= 0) && pDefBld->Occupants.Count == 0
					&& pDefBld->BunkerLinkedItem == nullptr;
				if (isEmptyUnarmedBunker)
					continue;

				const bool pureAA = pW && pW->Projectile && pW->Projectile->AA && !pW->Projectile->AG;
				if (pureAA)
					continue;

				const CellStruct dc = pDefBld->GetMapCoords();
				if (std::max(std::abs(bc.X - dc.X), std::abs(bc.Y - dc.Y)) <= 10)
					localGroundDefsNearBld++;
			}

			const int desiredLocalDefs = (closestEnemyDist <= 14 || nearbyEnemyCount >= 4) ? 4 : 3;
			if (localGroundDefsNearBld < desiredLocalDefs)
			{
				const int deficitHere = desiredLocalDefs - localGroundDefsNearBld;
				frontlineDefenseDeficit = std::max(frontlineDefenseDeficit, deficitHere);
				const int urgency = (30 - std::min(28, closestEnemyDist)) * 12 + nearbyEnemyCount * 25 + deficitHere * 40;
				if (urgency > bestFrontlineUrgency)
				{
					bestFrontlineUrgency = urgency;
					urgentFrontlineBldCell = bc;
					urgentFrontlineEnemyCell = closestEnemyPos;
				}
			}
		}

		// Feed continuous frontline/base-crawl proximity directly into macroState so both
		// target selection and FindSpatialTetrisBuildCell directional placement act immediately!
		if (urgentFrontlineBldCell != CellStruct::Empty && urgentFrontlineEnemyCell != CellStruct::Empty
			&& macroState.FrontlineThreatNeedsDefenses < frontlineDefenseDeficit)
		{
			macroState.FrontlineThreatBuildingCoords = urgentFrontlineBldCell;
			macroState.FrontlineThreatCoords = urgentFrontlineEnemyCell;
			macroState.FrontlineThreatActiveFrames = Unsorted::CurrentFrame + 900;
			macroState.FrontlineThreatNeedsDefenses = frontlineDefenseDeficit;
		}

		const bool effectiveFrontlineThreat = frontlineThreatActive || (frontlineDefenseDeficit > 0);
		const int defDensityMod = std::clamp(static_cast<int>(scores[14]) / 200, 0, 4);
		const int enemyArmorPressureBonus = (features[65] >= 24) ? 3 : ((features[65] >= 12) ? 2 : (features[80] > 0 ? 1 : 0));

		const int baseGroundDefCap = 2 + ownedRefineries * 2 + ownedWarFactories + ownedTechStructures
			+ undefendedRemoteAssetsCount + defDensityMod + enemyArmorPressureBonus + (wasRecentlyAttacked ? 3 : 0)
			+ std::max(frontlineDefenseDeficit, frontlineThreatActive ? macroState.FrontlineThreatNeedsDefenses : 0);

		const int maxGroundDefenses = std::clamp(baseGroundDefCap, 4, (hasTechCenter || money > 3000 || effectiveFrontlineThreat) ? 20 : 14);
		const int baseMaxAA = 1 + ownedWarFactories + ownedTechStructures + defDensityMod + (underAirAttack ? 2 : 0);
		const int maxAADefenses = std::clamp(std::max(baseMaxAA, 1 + enemyAirValue / 30), 2, 8);

		// AdvAI & StarCraft Resource Expansion State Machine:
		// Measure minimum Chebyshev distance from our closest
		// BaseNormal/ConYard/CapturedTech anchor to the nearest uncolonized Ore/Gem
		// field (>= 14 cells from any owned Refinery, outside the ore blocks we already
		// hold: the edge of our own field is not a new one).
		// - If minAnchorDistToUncolonizedOre <= 9: our BaseNormal chain, Captured
		// Tech Building, or Expansion ConYard has reached the new Ore field!
		// Trigger ShouldBuildRefinery (+1950)!
		// - If minAnchorDistToUncolonizedOre is 10..60: boost BaseNormal=yes
		// stepping-stone structures (Power Plants / Barracks / Outposts) so the
		// base actively crawls to reach the Ore!
		CellStruct nearestUncolonizedOreCell = CellStruct::Empty;
		int minAnchorDistToUncolonizedOre = 9999;
		const std::vector<int> heldOreBlocks = GetHeldOreBlocks(pHouse);
		// With two refineries standing, a building within 8 cells of one does not count as having
		// reached a field (AdvAI spacing): the refinery would stand next to the others again, so
		// the stepping stones go on until one stands clear of them.
		std::vector<CellStruct> spacingRefineryCells;

		for (auto const pRefBld : pHouse->Buildings)
		{
			if (pRefBld && pRefBld->IsAlive && !pRefBld->InLimbo && pRefBld->Type
				&& (pRefBld->Type->Refinery || pRefBld->Type->ResourceDestination))
			{
				spacingRefineryCells.push_back(pRefBld->GetMapCoords());
			}
		}

		auto const crowdsRefinery = [&spacingRefineryCells](const CellStruct& cell)
		{
			return spacingRefineryCells.size() >= 2
				&& std::any_of(spacingRefineryCells.begin(), spacingRefineryCells.end(), [&cell](const CellStruct& rc)
					{ return std::max(std::abs(cell.X - rc.X), std::abs(cell.Y - rc.Y)) <= 8; });
		};

		for (const CellStruct& scanOrigin : scanOrigins)
		{
			if (scanOrigin == CellStruct::Empty)
				continue;

			const int scanRad = (&scanOrigin == &scanOrigins[0]) ? 78 : 42;
			for (int dy = -scanRad; dy <= scanRad; dy += 3)
			{
				for (int dx = -scanRad; dx <= scanRad; dx += 3)
				{
					const CellStruct tc { static_cast<short>(scanOrigin.X + dx),
						static_cast<short>(scanOrigin.Y + dy) };
					auto const pCell = MapClass::Instance.TryGetCellAt(tc);
					if (!pCell || pCell->GetContainedTiberiumValue() <= 0)
						continue;

					const int oreZone = MapClass::Instance.GetMovementZoneType(tc, MovementZone::Normal, false);
					if (oreZone < 0 || IsOreCellInHeldField(heldOreBlocks, tc))
						continue;

					bool alreadyColonizedByRef = false;
					int closestAnchorDist = 9999;
					for (auto const pOwnBld : pHouse->Buildings)
					{
						if (!pOwnBld || pOwnBld->InLimbo || !pOwnBld->IsAlive || !pOwnBld->Type
							|| pOwnBld->Type->Insignificant || pOwnBld->Type->InvisibleInGame)
						{
							continue;
						}

						const CellStruct bc = pOwnBld->GetMapCoords();
						const int bz = MapClass::Instance.GetMovementZoneType(bc, MovementZone::Normal, pOwnBld->OnBridge);
						if (bz != oreZone)
							continue;

						const int dCheb = std::max(std::abs(tc.X - bc.X), std::abs(tc.Y - bc.Y));
						if ((pOwnBld->Type->Refinery || pOwnBld->Type->ResourceGatherer
								|| pOwnBld->Type->ResourceDestination)
							&& dCheb < 14)
						{
							alreadyColonizedByRef = true;
							break;
						}

						if ((pOwnBld->Type->BaseNormal || pOwnBld->Type->ConstructionYard
								|| BuildingTypeExt::IsAIBaseNormal(pOwnBld->Type))
							&& !crowdsRefinery(bc))
						{
							if (dCheb < closestAnchorDist)
								closestAnchorDist = dCheb;
						}
					}

					if (!alreadyColonizedByRef && closestAnchorDist < minAnchorDistToUncolonizedOre)
					{
						minAnchorDistToUncolonizedOre = closestAnchorDist;
						nearestUncolonizedOreCell = tc;
					}
				}
			}
		}

		bool conYardWithNoRefineryInZone = false;
		bool forwardConYardNeedsWarFactory = false;
		bool forwardConYardNeedsBarracks = false;
		bool forwardConYardNeedsDefense = false;
		// A ConYard of ours on another landmass with useful water close by and no shipyard of ours
		// within 22 cells: each conquered island gets its own shipyard (sea control there).
		bool islandYardNeedsShipyard = false;
		const int spawnHomeZone = (pHouse->BaseSpawnCell != CellStruct::Empty)
			? MapClass::Instance.GetMovementZoneType(pHouse->BaseSpawnCell, MovementZone::Normal, false)
			: -1;

		// Island Hold plan (UpdateStrategicPlans): a fresh island ConYard is fortified before its
		// economy exists, with anti-air too once enemy air has been seen.
		auto const& holdMacro = GetMacroState(pHouse);
		const bool islandHoldActive
			= holdMacro.IslandHoldUntilFrame > Unsorted::CurrentFrame && holdMacro.IslandHoldCell != CellStruct::Empty;
		bool islandHoldNeedsAA = false;

		if (islandHoldActive
			&& (holdMacro.OpponentProfile.Share(EnemyForceCategory::Air) >= 15 || holdMacro.AirThreatValue > 0))
		{
			islandHoldNeedsAA = true;

			for (auto const pOwnB : pHouse->Buildings)
			{
				if (pOwnB && pOwnB->IsAlive && !pOwnB->InLimbo && pOwnB->Type && pOwnB->Type->IsBaseDefense
					&& GetCombatValueOfType(pOwnB->Type, true) > 0 && pOwnB->GetMapCoords().DistanceFrom(holdMacro.IslandHoldCell) <= 12.0)
				{
					islandHoldNeedsAA = false;
					break;
				}
			}
		}

		for (int yi = 0; yi < pHouse->ConYards.Count; ++yi)
		{
			auto const pCY = pHouse->ConYards.GetItem(yi);
			if (!pCY || pCY->InLimbo || !pCY->IsAlive)
				continue;

			const CellStruct cyc = pCY->GetMapCoords();
			const int cyz = MapClass::Instance.GetMovementZoneType(cyc, MovementZone::Normal, pCY->OnBridge);
			if (cyz < 0)
				continue;

			// Remote as placement sees a hub (FindSpatialTetrisBuildCell): another landmass, or more
			// than 16 cells from the spawn. A second yard deployed inside the home base is not an
			// outpost: its missing factory was never placed next to it (placement anchors at hubs
			// only), so it kept asking for one forever.
			const bool isRemoteOrNomadicCY = (spawnHomeZone >= 0 && cyz != spawnHomeZone)
				|| (pHouse->BaseSpawnCell != CellStruct::Empty
					? std::max(std::abs(cyc.X - pHouse->BaseSpawnCell.X), std::abs(cyc.Y - pHouse->BaseSpawnCell.Y)) > 16
					: yi >= 1);
			if (!isRemoteOrNomadicCY)
				continue;

			int refsNearCY = 0;
			bool hasWarFacNearCY = false;
			bool hasBarracksNearCY = false;
			int defsNearCY = 0;
			for (auto const pOwnB : pHouse->Buildings)
			{
				if (!pOwnB || pOwnB->InLimbo || !pOwnB->IsAlive || !pOwnB->Type)
					continue;

				const CellStruct bc = pOwnB->GetMapCoords();
				const int dCheb = std::max(std::abs(cyc.X - bc.X), std::abs(cyc.Y - bc.Y));
				if (dCheb <= 18 && MapClass::Instance.GetMovementZoneType(bc, MovementZone::Normal, pOwnB->OnBridge) == cyz)
				{
					if (pOwnB->Type->Refinery || pOwnB->Type->ResourceGatherer || pOwnB->Type->ResourceDestination)
					{
						refsNearCY++;
					}
					else if (pOwnB->Type->Factory == AbstractType::UnitType && !pOwnB->Type->Naval)
					{
						hasWarFacNearCY = true;
					}
					else if (pOwnB->Type->Factory == AbstractType::InfantryType)
					{
						hasBarracksNearCY = true;
					}
					else if (pOwnB->Type->IsBaseDefense && !IsTunnelNetworkBuilding(pOwnB->Type))
					{
						const auto pDWS = pOwnB->Type->GetWeapon(0);
						const auto pDW = pDWS ? pDWS->WeaponType : nullptr;
						const bool emptyUnarmedBunker = (pOwnB->Type->CanBeOccupied || pOwnB->Type->Bunker)
							&& (pDW == nullptr || pDW->Damage <= 0) && pOwnB->Occupants.Count == 0
							&& pOwnB->BunkerLinkedItem == nullptr;
						if (!emptyUnarmedBunker)
							defsNearCY++;
					}
				}
			}

			const bool hasRefNearCY = (refsNearCY >= 1);
			int oreCellsNearCY = 0;
			int tiberiumTreesNearCY = 0;
			for (int dy = -24; dy <= 24; dy += 3)
			{
				for (int dx = -24; dx <= 24; dx += 3)
				{
					const CellStruct tc { static_cast<short>(cyc.X + dx), static_cast<short>(cyc.Y + dy) };
					if (auto const pC = MapClass::Instance.TryGetCellAt(tc))
					{
						if (MapClass::Instance.GetMovementZoneType(tc, MovementZone::Normal, false) != cyz)
							continue;

						if (pC->GetContainedTiberiumValue() > 0)
							oreCellsNearCY++;

						if (auto const pTerr = pC->GetTerrain(false))
						{
							if (pTerr->Type && pTerr->Type->SpawnsTiberium)
								tiberiumTreesNearCY++;
						}
					}
				}
			}

			const bool hasOreNearCY = (oreCellsNearCY > 0 || tiberiumTreesNearCY > 0);
			const bool wantsSecondIslandRefinery = (refsNearCY == 1 && hasWarFacNearCY
				&& (tiberiumTreesNearCY >= 1 || oreCellsNearCY >= 8) && money >= 1400);

			bool enemyNearCY = false;
			for (auto const pEB : BuildingClass::Array)
			{
				if (!pEB || pEB->InLimbo || !pEB->IsAlive || !pEB->Owner || !pEB->Type || pEB->Owner == pHouse
					|| pHouse->IsAlliedWith(pEB->Owner) || !IsEnemyBuildingKnown(pHouse, pEB) || pEB->Owner->IsNeutral())
				{
					continue;
				}

				const CellStruct ec = pEB->GetMapCoords();
				if (std::max(std::abs(cyc.X - ec.X), std::abs(cyc.Y - ec.Y)) <= 36
					|| MapClass::Instance.GetMovementZoneType(ec, MovementZone::Normal, pEB->OnBridge) == cyz)
				{
					enemyNearCY = true;
					break;
				}
			}

			if ((!hasRefNearCY && hasOreNearCY) || wantsSecondIslandRefinery)
				conYardWithNoRefineryInZone = true;

			if (enemyNearCY || (spawnHomeZone >= 0 && cyz != spawnHomeZone) || !hasOreNearCY)
			{
				if (!hasWarFacNearCY && ownedRefineries >= 1)
					forwardConYardNeedsWarFactory = true;

				if (!hasBarracksNearCY && ownedRefineries >= 1)
					forwardConYardNeedsBarracks = true;

				if (defsNearCY < 2 && (hasRefNearCY || hasWarFacNearCY || enemyNearCY))
					forwardConYardNeedsDefense = true;
			}

			if (!islandYardNeedsShipyard && spawnHomeZone >= 0 && cyz != spawnHomeZone && !enemyNearCY)
			{
				const bool hasShipyardNear = std::any_of(pHouse->Buildings.begin(), pHouse->Buildings.end(), [&cyc](BuildingClass* pB)
					{
						return pB && pB->IsAlive && !pB->InLimbo && pB->Type && pB->Type->Factory == AbstractType::UnitType
							&& pB->Type->Naval && pB->GetMapCoords().DistanceFrom(cyc) <= 22.0;
					});

				if (!hasShipyardNear)
				{
					for (int wy = -12; wy <= 12 && !islandYardNeedsShipyard; wy += 3)
					{
						for (int wx = -12; wx <= 12; wx += 3)
						{
							const CellStruct wc { static_cast<short>(cyc.X + wx), static_cast<short>(cyc.Y + wy) };
							auto const pWaterCell = MapClass::Instance.TryGetCellAt(wc);

							if (pWaterCell && pWaterCell->LandType == LandType::Water && IsUsefulWaterCell(wc))
							{
								islandYardNeedsShipyard = true;
								break;
							}
						}
					}
				}
			}

			if (islandHoldActive && defsNearCY < 3 && cyc.DistanceFrom(holdMacro.IslandHoldCell) <= 2.0)
				forwardConYardNeedsDefense = true;
		}

		const bool reachedNewOreField
			= (conYardWithNoRefineryInZone
				|| (nearestUncolonizedOreCell != CellStruct::Empty && minAnchorDistToUncolonizedOre <= 9))
			&& ownedRefineries >= 1;

		// Permanent refinery cap: a home cap, raised to 3 per ConYard while a new
		// field waits for its refinery. Past it a refinery is only built as a Harvester Recovery
		// refinery (replaced in place or sold once its FreeUnit spawns), never kept: a permanent
		// one past the cap raised the harvester target (2 per refinery), which asked for another
		// recovery refinery, and the chain filled the map with refineries far from any field.
		const int homeRefineryCap = (reachedNewOreField || pHouse->NumConYards >= 2 || features[21] > 0
										|| (hasTechCenter && money < 3000))
			? std::max(6, pHouse->NumConYards + 4)
			: 4;
		const bool permanentRefineryCapReached = reachedNewOreField
			? ownedRefineries >= std::max(homeRefineryCap, pHouse->NumConYards * 3)
			: ownedRefineries >= homeRefineryCap;

		// Outposts that may get a factory past the difficulty cap (SellFactoriesOverDifficultyCap
		// then sells another of its type): a forward or island ConYard without one, or the front of
		// an offensive crawl (newest forward stone) without one within 15 cells.
		const int factoryCategoryCap = GetFactoryCategoryCap(pHouse);
		std::array<int, 3> factoryCategoryCounts {};

		if (factoryCategoryCap != INT_MAX)
		{
			for (int category = 0; category < 3; ++category)
				factoryCategoryCounts[category] = CountFactoriesInCategory(pHouse, category);
		}
		bool frontNeedsBarracks = false;
		bool frontNeedsWarFactory = false;

		if (factoryCategoryCap != INT_MAX)
		{
			const CellStruct stoneCell = GetNewestOffensiveStoneCell(pHouse);

			if (stoneCell != CellStruct::Empty)
			{
				frontNeedsBarracks = true;
				frontNeedsWarFactory = true;

				for (auto const pOwnB : pHouse->Buildings)
				{
					if (!pOwnB || !pOwnB->IsAlive || pOwnB->InLimbo || !pOwnB->Type)
						continue;

					const CellStruct bc = pOwnB->GetMapCoords();
					if (std::max(std::abs(bc.X - stoneCell.X), std::abs(bc.Y - stoneCell.Y)) > 15)
						continue;

					if (pOwnB->Type->Factory == AbstractType::InfantryType)
						frontNeedsBarracks = false;
					else if (pOwnB->Type->Factory == AbstractType::UnitType && !pOwnB->Type->Naval)
						frontNeedsWarFactory = false;
				}
			}
		}

		const bool needsSteppingStoneCrawlToOre = (GetMacroState(pHouse).ConsecutivePlacementFailures < 2
			&& nearestUncolonizedOreCell != CellStruct::Empty && minAnchorDistToUncolonizedOre > 9
			&& minAnchorDistToUncolonizedOre <= 60 && hasCoreEconomyAndProduction
			&& (features[19] < 32 || ownedRefineries < 3 || scores[34] > 100 || Unsorted::CurrentFrame > 2100));

		// AdvAI Start-Rush Emergency Defense Interception:
		// If we are rushed early (CurrentFrame <= with at least 1 Refinery + 1 Barracks
		// and 0 ground defenses while our ConYard is constructing a non-essential building,
		// abandon the non-essential construction so the ConYard can immediately build a defense!
		if (Unsorted::CurrentFrame <= 10000 && ownedRefineries >= 1 && ownedBarracks >= 1 && totalGroundDefenses == 0
			&& (wasRecentlyAttacked || frontlineThreatActive))
		{
			if (FactoryClass* pBldFac
				= pHouse->GetPrimaryFactory(AbstractType::BuildingType, false, BuildCat::DontCare))
			{
				if (auto const pInProgBld = abstract_cast<BuildingClass*>(pBldFac->Object))
				{
					auto const pInProgType = pInProgBld->Type;
					if (pInProgType && !pInProgType->IsBaseDefense && pInProgType->PowerBonus <= pInProgType->PowerDrain
						&& !pInProgType->Refinery && !pInProgType->ResourceGatherer && !pInProgType->ResourceDestination
						&& pInProgType->Factory != AbstractType::InfantryType
						&& (pInProgType->Factory != AbstractType::UnitType || pInProgType->Naval)
						&& pBldFac->GetProgress() < 42)
					{
						pBldFac->AbandonProduction();
					}
				}
			}
		}

		int inProgressBldIdx = -1;
		int inProgressDefIdx = -1;
		if (FactoryClass* pBldFac = pHouse->GetPrimaryFactory(AbstractType::BuildingType, false, BuildCat::DontCare))
		{
			if (pBldFac->Object && pBldFac->Object->GetType())
				inProgressBldIdx = pBldFac->Object->GetType()->GetArrayIndex();
		}

		if (FactoryClass* pDefFac = pHouse->GetPrimaryFactory(AbstractType::BuildingType, false, BuildCat::Combat))
		{
			if (pDefFac->Object && pDefFac->Object->GetType())
				inProgressDefIdx = pDefFac->Object->GetType()->GetArrayIndex();
		}

		int ownedCashBuildings = 0;
		int ownedSlaveMinerBuildings = 0;
		int staticRefineriesCount = 0;
		int orphanRefineryZoneNeedsReplacement = -1;
		for (auto const pOwnB : pHouse->Buildings)
		{
			if (!pOwnB || pOwnB->InLimbo || !pOwnB->IsAlive || !pOwnB->Type)
				continue;

			if (pOwnB->Type->ProduceCashAmount > 0)
				ownedCashBuildings++;

			if (pOwnB->Type->ResourceGatherer)
			{
				ownedSlaveMinerBuildings++;
			}
			else if ((pOwnB->Type->Refinery || pOwnB->Type->ResourceDestination) && pOwnB->Type->Enslaves == nullptr
				&& pOwnB->Type->UndeploysInto == nullptr && pOwnB->CurrentMission != Mission::Selling
				&& pOwnB->QueuedMission != Mission::Selling)
			{
				staticRefineriesCount++;
				const CellStruct rCoords = pOwnB->GetMapCoords();
				const int rz = MapClass::Instance.GetMovementZoneType(rCoords, MovementZone::Normal, pOwnB->OnBridge);
				if (rz >= 0 && orphanRefineryZoneNeedsReplacement < 0)
				{
					int harvsInRz = 0;
					for (auto const pHU : UnitClass::Array)
					{
						if (pHU && pHU->Owner == pHouse && !pHU->InLimbo && pHU->IsAlive && pHU->Type
							&& (pHU->Type->Harvester || pHU->Type->ResourceGatherer)
							&& ObjectZone(pHU, MovementZone::Normal)
								== rz)
						{
							harvsInRz++;
						}
					}

					if (harvsInRz == 0)
					{
						const int rW = pOwnB->Type->GetFoundationWidth();
						const int rH = pOwnB->Type->GetFoundationHeight(false);
						const int adj = std::max(1, pOwnB->Type->Adjacent);
						bool hasAnchorInRz = false;
						for (auto const pAnc : pHouse->Buildings)
						{
							if (!pAnc || pAnc == pOwnB || pAnc->InLimbo || !pAnc->IsAlive || !pAnc->Type)
								continue;

							if (!pAnc->Type->BaseNormal && !pAnc->Type->ConstructionYard
								&& !BuildingTypeExt::IsAIBaseNormal(pAnc->Type))
							{
								continue;
							}

							const CellStruct ac = pAnc->GetMapCoords();
							const int aW = pAnc->Type->GetFoundationWidth();
							const int aH = pAnc->Type->GetFoundationHeight(false);
							const int gapX = std::max(0,
								std::max(static_cast<int>(rCoords.X) - (static_cast<int>(ac.X) + aW - 1),
									static_cast<int>(ac.X) - (static_cast<int>(rCoords.X) + rW - 1)));
							const int gapY = std::max(0,
								std::max(static_cast<int>(rCoords.Y) - (static_cast<int>(ac.Y) + aH - 1),
									static_cast<int>(ac.Y) - (static_cast<int>(rCoords.Y) + rH - 1)));
							if (std::max(gapX, gapY) <= adj)
							{
								hasAnchorInRz = true;
								break;
							}
						}

						if (hasAnchorInRz && !IsRefineryExhausted(pOwnB, CellStruct::Empty))
							orphanRefineryZoneNeedsReplacement = rz;
					}
				}
			}
		}

		// Harvester recovery through a refinery's FreeUnit (sell-and-replace or a temporary one) only
		// up to one harvester per refinery: the war factory builds the second (economyUnitCap). Aiming
		// at two kept the ConYard selling and rebuilding refineries all match (30 times in one test
		// house), so radar, tech center and defenses never got their turn.
		// Not while broke (UpdateEconomyCrisis): selling a working refinery to rebuild it splits the
		// last credits between the refinery and the harvester the war factory builds.
		const bool wantsHarvesterRecoveryViaRefinery = (pHouse->ConYards.Count >= 1 && staticRefineriesCount >= 1
			&& !GetMacroState(pHouse).BrokeHoldActive
			&& Unsorted::CurrentFrame >= GetMacroState(pHouse).NextHarvesterRecoveryFrame
			&& GetMacroState(pHouse).TemporaryRecoveryRefineryPlacedFrame.empty()
			&& (orphanRefineryZoneNeedsReplacement >= 0
				|| (ownedHarvestersCount < staticRefineriesCount
					&& (ownedHarvestersCount == 0 || ownedWarFactories >= 1) && features[19] > 0
					&& (money >= 900 || ownedHarvestersCount == 0))));
		const bool hasActiveBuildingIncome = (ownedCashBuildings >= 1)
			|| (((ownedRefineries >= 1 && ownedHarvestersCount >= 1) || ownedSlaveMinerBuildings >= 1)
				&& (features[19] > 0 || pHouse->OwnedTiberium.GetTotalAmount() > 0));

		// Expansion MCV prerequisites: a house that wants to expand (slot 30, free ore fields) but
		// cannot build any MCV yet is often missing one building of the MCV's Prerequisite list
		// (YR asks for a service depot besides the war factory). Those buildings get a bonus.
		std::vector<int> mcvPrerequisites;

		if (pHouse->NumConYards >= 1 && pHouse->NumConYards < 6 && scores[30] >= 450 && money >= 2000
			&& GetMacroState(pHouse).UncolonizedOreFields > 0)
		{
			bool mcvBuildable = false;
			std::vector<int> missing;

			for (auto const pMcvType : UnitTypeClass::Array)
			{
				if (!pMcvType || !pMcvType->DeploysInto || !pMcvType->DeploysInto->ConstructionYard || pMcvType->TechLevel < 0
					|| (ownHouseBit != 0u && !pMcvType->InOwners(ownHouseBit)))
				{
					continue;
				}

				if (pHouse->CanBuild(pMcvType, false, false) == CanBuildResult::Buildable)
				{
					mcvBuildable = true;
					break;
				}

				for (int pi = 0; pi < pMcvType->Prerequisite.Count; ++pi)
					missing.push_back(pMcvType->Prerequisite.GetItem(pi));
			}

			if (!mcvBuildable)
				mcvPrerequisites = std::move(missing);
		}

		// Opening order of a player: a refinery or two, then the war factory. Without one, and with a
		// ground war factory buildable now, the first war factory is pushed and further refineries
		// wait (one Soviet test house built six refineries, most failing to place, before its first
		// war factory at frame 24000+).
		bool groundWarFactoryBuildableNow = false;

		if (ownedWarFactories == 0)
		{
			for (auto const pFactoryType : BuildingTypeClass::Array)
			{
				if (pFactoryType && pFactoryType->Factory == AbstractType::UnitType && !pFactoryType->Naval
					&& !pFactoryType->UndeploysInto && CanBuildUnderHumanRules(pHouse, pFactoryType))
				{
					groundWarFactoryBuildableNow = true;
					break;
				}
			}
		}

		// Naval matches (HouseNeedsNaval: starts not linked by land, free ore only across water, enemy
		// warships seen): the sea decides where we can expand, so the first shipyard comes before the
		// first war factory, when shipyards are allowed and buildable now.
		bool navalFirst = false;

		if (ownedShipyards == 0 && HouseNeedsNaval(pHouse))
		{
			for (auto const pYardType : BuildingTypeClass::Array)
			{
				if (pYardType && pYardType->Factory == AbstractType::UnitType && pYardType->Naval
					&& CanBuildUnderHumanRules(pHouse, pYardType))
				{
					navalFirst = true;
					break;
				}
			}
		}

		const bool firstFactoryPending = groundWarFactoryBuildableNow || navalFirst;

		for (auto const pBld : BuildingTypeClass::Array)
		{
			if (!pBld || pBld->Wall || pBld->FirestormWall || pBld->Gate || pBld->LaserFence || pBld->LaserFencePost
				|| pBld->ToTile || !pBld->Selectable)
			{
				continue;
			}

			if (HouseExt::IsDisabledFromShell(pHouse, pBld))
				continue;

			if (!CanBuildUnderHumanRules(pHouse, pBld))
				continue;

			auto const& cooldownMap = GetMacroState(pHouse).BuildingPlacementCooldownUntilFrame;
			auto const cdIt = cooldownMap.find(pBld->ArrayIndex);
			if (cdIt != cooldownMap.end() && Unsorted::CurrentFrame < cdIt->second)
				continue;

			const bool isBldInProgress = (pBld->ArrayIndex == inProgressBldIdx || pBld->ArrayIndex == inProgressDefIdx);
			const bool isRefineryOrSlaveMiner = pBld->Refinery || pBld->ResourceGatherer || pBld->ResourceDestination;
			const bool isIncomeRecoveryBld = isRefineryOrSlaveMiner
				|| (pBld->PowerBonus > pBld->PowerDrain && !pBld->IsBaseDefense
					&& (pHouse->HasLowPower() || hasUnpoweredBuildings))
				|| (pBld->Factory == AbstractType::UnitType && !pBld->Naval && ownedWarFactories == 0
					&& ownedHarvestersCount == 0)
				|| (pBld->Factory == AbstractType::InfantryType && ownedBarracks == 0 && features[20] > 0)
				|| (pBld->Factory == AbstractType::UnitType && pBld->Naval && ownedShipyards == 0 && features[19] == 0
					&& features[21] > 0);

			// Human-like credit management:
			// - When active income is flowing (hasActiveBuildingIncome), never block
			// or delay selecting buildings by upfront cost; start building
			// immediately and pay step-by-step!
			// - When all income sources are lost (!hasActiveBuildingIncome), strictly
			// ration remaining credits to recover an income source first!
			if (!hasActiveBuildingIncome && !isBldInProgress && !isIncomeRecoveryBld
				&& pBld->GetActualCost(pHouse) > money)
			{
				continue;
			}

			const int owned = BuildingTypeExt::CountOwnedNowWithDeployOrUpgrade(pBld, pHouse);

			// Difficulty cap per factory category (all barracks together, all ground war factories
			// together, all shipyards together: 4 on Easy, 10 on Normal, none on Hard). At the cap one
			// more is allowed for an outpost that needs it or for the first of a new type a captured
			// tree opened (not within 4500 frames of the last cap sale); SellFactoriesOverDifficultyCap
			// then sells a copy of that category.
			if (const int category = FactoryCapCategory(pBld);
				factoryCategoryCap != INT_MAX && category >= 0 && factoryCategoryCounts[category] >= factoryCategoryCap)
			{
				const bool outpostNeedsIt = (category == 0)
					? (forwardConYardNeedsBarracks || frontNeedsBarracks)
					: (category == 1 ? (forwardConYardNeedsWarFactory || frontNeedsWarFactory) : islandYardNeedsShipyard);
				const bool newFactoryType = owned == 0;
				const bool saleCooldownOver = Unsorted::CurrentFrame - GetMacroState(pHouse).LastFactoryCapSaleFrame >= 4500;

				if (!(outpostNeedsIt || newFactoryType) || !saleCooldownOver || factoryCategoryCounts[category] > factoryCategoryCap)
					continue;
			}

			// Tech before copies, as a human player: no second Barracks or War Factory of a type
			// while a building that unlocks new units or buildings can be built right now, unless
			// an outpost needs it. Airfields keep following the demand of the docking aircraft.
			const bool isGroundFactoryOrBarracks = pBld->Factory == AbstractType::InfantryType
				|| (pBld->Factory == AbstractType::UnitType && !pBld->Naval);

			if (isGroundFactoryOrBarracks && owned >= 1 && GetMacroState(pHouse).HasBuildableTechUnlock)
			{
				const bool outpostNeedsCopy = (pBld->Factory == AbstractType::InfantryType)
					? (forwardConYardNeedsBarracks || frontNeedsBarracks)
					: (forwardConYardNeedsWarFactory || frontNeedsWarFactory);

				if (!outpostNeedsCopy)
					continue;
			}

			// Tech tree unlockers (UpdateTechUnlocks): the first of a building that opens new units
			// or buildings gets 300 + 120 per type it unlocks (up to 1400) once the core economy
			// and production stand.
			int techUnlockBonus = 0;

			if (owned == 0 && hasCoreEconomyAndProduction)
			{
				auto const& unlocks = GetMacroState(pHouse).TechUnlocks;

				if (auto const it = unlocks.find(pBld->ArrayIndex); it != unlocks.end() && it->second > 0)
					techUnlockBonus = std::min(1400, 300 + 120 * it->second);

				// Expert 2: more when it opens a better counter of the seen enemy army.
				techUnlockBonus += CounterUnlockBonusFor(pHouse, pBld);
			}

			// Branch choices (Prerequisite.Negative: two tech labs, rival defense lines): the first of
			// each gets a per-match taste of +-250, so one match goes one way and another the other,
			// as players do. Deterministic in multiplayer (game seed, house, building).
			if (owned == 0 && IsBranchChoiceBuilding(pBld->ArrayIndex))
				techUnlockBonus += MatchTaste(pHouse, pBld->ArrayIndex, 250);

			int score = 0;

			if (pBld->PowerBonus > pBld->PowerDrain && !pBld->IsBaseDefense)
			{
				// Hard anti-spam cap: never queue more Power Plants when surplus is
				// already well above our damaged-plant safety cushion (relaxed by +350
				// when stepping-stone crawling to a distant Ore patch)!
				const int maxAllowedSurplus = requiredPowerCushion
					+ ((needsSteppingStoneCrawlToOre && pBld->BaseNormal && money >= 1400) ? 700 : 350);
				if (!pHouse->HasLowPower() && !hasUnpoweredBuildings && realPowerSurplus >= maxAllowedSurplus)
					continue;

				// Track best buildable power plant for pre-build power interception
				// (only after verifying we are below maxAllowedSurplus and not
				// placement-locked!)
				const int plantEfficiencyScore = pBld->PowerBonus * 3 - (pBld->GetActualCost(pHouse) / 5);
				if ((GetMacroState(pHouse).ConsecutivePlacementFailures < 2 || pHouse->HasLowPower()
						|| hasUnpoweredBuildings)
					&& plantEfficiencyScore > bestPowerPlantScore
					&& pBld->GetActualCost(pHouse) <= std::max(money + 1000, 1200))
				{
					bestPowerPlantScore = plantEfficiencyScore;
					bestPowerPlantIdx = pBld->ArrayIndex;
				}

				score = scores[0] + std::min(650, pBld->PowerBonus * 2);

				// Avoid spamming new Bio-Reactors from the ConYard when existing owned
				// Bio-Reactors still have free passenger slots that our Barracks can
				// fill with cheap infantry or mind-controlled enemies for +100W each!
				if (pBld->InfantryAbsorb && unfilledBioReactorSlots >= 2 && ownedBarracks >= 1 && !pHouse->HasLowPower()
					&& !hasUnpoweredBuildings && realPowerSurplus >= 40
					&& (realPowerSurplus + bioReactorPotentialPower >= requiredPowerCushion)
					&& !needsSteppingStoneCrawlToOre)
				{
					score -= 1100;
				}

				// Prefer Advanced Reactors (Nuclear/Bio/Tesla >= 300 PowerBonus) over
				// basic power plants once unlocked and affordable, EXCEPT when building
				// a cheap 2x2 BaseNormal stepping stone to reach Ore!
				if (pBld->PowerBonus >= 300 && money >= pBld->GetActualCost(pHouse) && !needsSteppingStoneCrawlToOre)
				{
					score += 350;
				}
				else if (hasAdvancedPowerPlants && pBld->PowerBonus < 250 && money >= 1200
					&& !needsSteppingStoneCrawlToOre)
				{
					score -= 300;
				}

				if (pHouse->HasLowPower() || hasUnpoweredBuildings || realPowerSurplus < 60)
				{
					score += 1400;
				}
				else if (realPowerSurplus < requiredPowerCushion)
				{
					score += 450 + ((needsSteppingStoneCrawlToOre && pBld->BaseNormal) ? 450 : 0);
				}
				else if (needsSteppingStoneCrawlToOre && pBld->BaseNormal && pBld->GetFoundationWidth() <= 3
					&& money >= 1400)
				{
					// AdvAI Stepping-Stone BaseNormal Crawler: use cheap Power Plants to
					// step the legal Adjacent=2 chain out of spawn toward the uncolonized
					// Ore field!
					score += 680;
				}
				else
				{
					score -= 1400;
				}
			}
			else if (isRefineryOrSlaveMiner)
			{
				const bool spawnsFreeHarvester = !pBld->ResourceGatherer && pBld->Enslaves == nullptr
					&& pBld->UndeploysInto == nullptr && pBld->FreeUnit != nullptr
					&& (pBld->FreeUnit->Harvester || pBld->FreeUnit->ResourceGatherer);
				const bool isHarvRecoveryCandidate = spawnsFreeHarvester && wantsHarvesterRecoveryViaRefinery;
				if (permanentRefineryCapReached && !isHarvRecoveryCandidate)
					continue;

				score = scores[1] - owned * 480;

				if (owned == 0)
				{
					score += 1200;
				}
				else if (reachedNewOreField && !permanentRefineryCapReached)
				{
					// AdvAI ShouldBuildRefinery: our BaseNormal chain or Expansion MCV reached
					// a new Ore field! Claim it immediately! Like a forward Barracks, it is not
					// penalized by the refineries of the home field (6 of them sank it below
					// any other building, so expansion ConYards stood on unmined ore).
					score = std::max(static_cast<int>(scores[1]), 300) + 1950;
				}
				else if (isHarvRecoveryCandidate
					&& (orphanRefineryZoneNeedsReplacement >= 0 || ownedHarvestersCount < staticRefineriesCount))
				{
					score = std::max(score, static_cast<int>(scores[1]))
						+ 2150; // AdvAI Harvester Recovery via Refinery Sell-and-Replace
								// (spawns FreeUnit Harvester without tying up War Factory!)
				}
				else if (isHarvRecoveryCandidate)
				{
					score = std::max(score, static_cast<int>(scores[1]))
						+ 1150; // Replenish up to 2 Harvesters per Refinery via ConYard
								// while War Factory builds tanks!
				}
				else if (owned == 1 && ownedWarFactories >= 1)
				{
					score += 450;
				}

				// Opening order (firstFactoryPending): after two refineries, or after one when the next
				// refinery has just failed to find a cell, the first war factory (or shipyard) comes first. Applied
				// after every branch above, the new-field one included.
				if (firstFactoryPending && !isHarvRecoveryCandidate
					&& (ownedRefineries >= 2 || (ownedRefineries >= 1 && GetMacroState(pHouse).RefineryReachFailures > 0)))
				{
					score -= 2500;
				}

				// Expert 0 after a raid: refineries back to their peak first.
				if (pExt->NeuralAI_ActiveExpert == 0 && GetMacroState(pHouse).EconomyRecoveryActive
					&& ownedRefineries < GetMacroState(pHouse).PeakRefineries)
				{
					score += 1500;
				}
			}
			else if (pBld->Factory == AbstractType::InfantryType)
			{
				const int effectiveMaxBarracks = forwardConYardNeedsBarracks
					? std::max(maxBarracks + 2, 5) + pHouse->NumConYards
					: ((needsSteppingStoneCrawlToOre && pBld->BaseNormal) ? std::max(maxBarracks + 1, 4) : maxBarracks);
				if (owned >= effectiveMaxBarracks)
					continue;

				// Slot 82 (Cross-Faction Tech Tree): If we already own our native
				// Barracks (ownedBarracks >= and a captured foreign ConYard unlocks
				// a brand-new Foreign Barracks (owned == 0), boost it so we unlock
				// foreign infantry and foreign tech prerequisites!
				if (owned == 0 && ownedBarracks >= 1 && ownedBarracks < 5 && SlotGate(scores[82], 60, 82))
				{
					score = std::max(static_cast<int>(scores[2]), 350) + 750
						+ std::clamp(static_cast<int>(scores[82]), 0, 500);
				}
				else if (forwardConYardNeedsBarracks)
				{
					// Forward Outpost / Island Beachhead ConYard lacks its own Barracks:
					// do not penalize by home-island Barracks count!
					score = std::max(static_cast<int>(scores[2]), 300) + 1450;
				}
				else
				{
					score = scores[2] - owned * (bankSteps > 0 ? 420 : 820) + (bankSteps > 0 ? 250 + 120 * std::min(6, bankSteps) : 0);

					if (ownedBarracks < maxEnemyBarracks && hasTechCenter && money > 1500)
						score += 420;

					if (needsSteppingStoneCrawlToOre && pBld->BaseNormal && money >= 1300)
					{
						score += 780; // Forward Barracks stepping-stone toward uncolonized
									// Ore / Frontline!
					}
				}
			}
			else if (pBld->Factory == AbstractType::UnitType && !pBld->Naval)
			{
				const int effectiveMaxWarFactories
					= forwardConYardNeedsWarFactory ? std::max(maxWarFactories + 2, 6) + pHouse->NumConYards : maxWarFactories;
				if (owned >= effectiveMaxWarFactories)
					continue;

				// Slot 82 (Cross-Faction Tech Tree): If we already own our native War
				// Factory (ownedWarFactories >= and a captured foreign ConYard
				// unlocks a brand-new Foreign War Factory (owned == 0), build it to
				// unlock foreign tanks, Radar, and Battle Lab!
				if (owned == 0 && ownedWarFactories >= 1 && ownedWarFactories < 5 && SlotGate(scores[82], 60, 82))
				{
					score = std::max(static_cast<int>(scores[3]), 400) + 850
						+ std::clamp(static_cast<int>(scores[82]), 0, 550);
				}
				else if (forwardConYardNeedsWarFactory)
				{
					// Forward Outpost / Island Beachhead ConYard lacks its own War
					// Factory: do not penalize by home-island War Factory count!
					score = std::max(static_cast<int>(scores[3]), 400) + 1750;
				}
				else
				{
					score = scores[3] - owned * (bankSteps > 0 ? 300 : 620) + (bankSteps > 0 ? 400 + 150 * std::min(6, bankSteps) : 0);

					// The first war factory once a refinery stands (see groundWarFactoryBuildableNow),
					// after the first shipyard on a naval match (navalFirst).
					if (ownedWarFactories == 0 && ownedRefineries >= 1)
						score += navalFirst ? -800 : 1200;

					// Competitive MultipleFactory Arms Race boost: never fall behind
					// the enemy in War Factory count or when floating macro cash!
					if (ownedWarFactories < maxEnemyWarFactories && ownedRefineries >= 2)
						score += 550;

					if (ownedWarFactories < ecoWarFactoryCap && money >= 2000)
						score += 420;
				}
			}
			else if (pBld->Factory == AbstractType::UnitType && pBld->Naval)
			{
				if (owned >= maxShipyards + (islandYardNeedsShipyard ? 1 : 0))
					continue;

				if (!(HouseNeedsNaval(pHouse) || islandYardNeedsShipyard) || CachedTetrisBuildCell(pHouse, pBld) == CellStruct::Empty)
					continue;

				score = scores[7] - owned * 950;

				// The first shipyard of a naval match once a refinery stands (navalFirst).
				if (navalFirst && ownedRefineries >= 1)
					score += 1600;

				// A conquered island's own shipyard (islandYardNeedsShipyard).
				if (islandYardNeedsShipyard)
					score += 1300;

				if (ownedShipyards < maxEnemyShipyards && ownedShipyards >= 1 && money >= 1800)
					score += 400;
			}
			else if (pBld->Factory == AbstractType::AircraftType)
			{
				if (owned >= maxAirpads)
					continue;

				if (owned == 0 && pHouse->NumAirpads >= 1 && pHouse->NumAirpads < 4 && SlotGate(scores[82], 60, 82))
				{
					score = std::max(static_cast<int>(scores[8]), 250) + 550
						+ std::clamp(static_cast<int>(scores[82]) / 2, 0, 350);
				}
				else
				{
					score = scores[8] - owned * 750 + (expert8NeedsMoreAirpadDocks ? 850 : 0);

					if (ownedAirpads < maxEnemyAirpads && hasTechCenter && money >= 2000)
						score += 350;
				}
			}
			else if (pBld->UnitRepair)
			{
				// Forward Repair plan: one more station near the front.
				auto const& repairMacro = GetMacroState(pHouse);
				const bool wantsForwardRepair = !pBld->Naval && owned == 1
					&& repairMacro.ForwardRepairUntilFrame > Unsorted::CurrentFrame && money >= 1500;

				if (owned >= 1 && !wantsForwardRepair)
					continue;

				score = scores[9] + (wantsForwardRepair ? 500 : 0);
			}
			else if (pBld->SuperWeapon != -1)
			{
				if (owned >= 1)
					continue;

				score = scores[10] + 450;

				if (pHouse->NumConYards >= 2 && SlotGate(scores[82], 60, 82))
					score += std::clamp(static_cast<int>(scores[82]) / 2, 0, 350);
			}
			else if (pBld->Storage > 0 && !isRefineryOrSlaveMiner)
			{
				// Only build Ore Silos when storage is actually full/short, max 2
				if (owned >= 2 || (!pHouse->IsTiberiumShort && money < 3500))
					continue;

				score = scores[11] - owned * 900;
			}
			else if (IsTunnelNetworkBuilding(pBld))
			{
				// Support both Superweapon-deployed 2nd Tunnel Mouths (C&C: Reloaded
				// ROBOTTUNNEL BuildLimit=1) AND mods where multiple Tunnel Mouths are
				// built directly from the Construction Yard (owned < 3)!
				if (!hasCoreEconomyAndProduction || owned >= 3)
					continue;

				const bool hasTunnelDeliverySW = (pBld->SuperWeapon != -1 || pBld->SuperWeapon2 != -1);

				const int tacticalTunnelBias = std::max({ static_cast<int>(scores[28]), static_cast<int>(scores[36]),
					static_cast<int>(scores[61]), static_cast<int>(scores[83]) });

				if (owned == 0)
					score = 680 + tacticalTunnelBias;
				else if (owned == 1 && !hasTunnelDeliverySW)
					score = 760 + tacticalTunnelBias; // Urgent 2nd Tunnel Mouth to

				// connect the two-way portal!
				else
					score = 460 + tacticalTunnelBias - owned * 140;
			}
			else if (pBld->IsBaseDefense)
			{
				// Allow building base defenses once core Economy + Barracks + War Factory exist,
				// OR during an early rush / frontline threat once at least 1 Refinery + 1 Barracks exist (from AdvAI)
				const bool canBuildEmergencyDefense = (ownedRefineries >= 1 && ownedBarracks >= 1
					&& (wasRecentlyAttacked || frontlineThreatActive || undefendedRemoteAssetsCount > 0));
				if (!hasCoreEconomyAndProduction && !canBuildEmergencyDefense && pHouse->ConYards.Count > 0)
					continue;

				bool isAntiInf = false;
				bool isAntiArmor = false;
				bool isAntiAir = false;
				bool isPureAA = false;
				bool isUnarmedBunker = false;
				GetDefenseCapabilities(pBld, isAntiInf, isAntiArmor, isAntiAir, isPureAA, isUnarmedBunker);

				// Never spam unarmed bunkers (Battle Bunkers / Tank Bunkers):
				// block them completely when under attack or frontline threat, when an empty bunker already exists,
				// when we already own 2, or when we lack compatible idle units to garrison them!
				if (isUnarmedBunker)
				{
					if (wasRecentlyAttacked || frontlineThreatActive || underVehicleAttack || underInfantryAttack
						|| underAirAttack)
					{
						continue;
					}

					if (pBld->CanBeOccupied
						&& (emptyOwnedBattleBunkers >= 1 || ownedBattleBunkers >= 2 || owned >= 2
							|| compatibleOccupierInfCount < 2))
					{
						continue;
					}

					if (pBld->Bunker
						&& (emptyOwnedTankBunkers >= 1 || ownedTankBunkers >= 2 || owned >= 2
							|| compatibleBunkerableTankCount < 1))
					{
						continue;
					}
				}

				if (isPureAA && totalAADefenses >= maxAADefenses && !islandHoldNeedsAA)
					continue;

				const bool needsOutpostPointDefense = !isPureAA && !isUnarmedBunker
					&& (undefendedRemoteAssetsCount > 0 || forwardConYardNeedsDefense || frontlineDefenseDeficit > 0);
				const int effectiveMaxGroundDefenses = needsOutpostPointDefense
					? std::max(maxGroundDefenses + 3, totalGroundDefenses + std::max(1, frontlineDefenseDeficit))
					: maxGroundDefenses;

				if (!isPureAA && totalGroundDefenses >= effectiveMaxGroundDefenses)
					continue;

				// Do not queue powered defenses when power cushion is already tight
				if ((pBld->Powered || pBld->PowerDrain >= 50)
					&& (pHouse->HasLowPower() || realPowerSurplus - pBld->PowerDrain < 75))
				{
					continue;
				}

				const bool isAntiArmorOrHighTierDefense = !isPureAA && !isUnarmedBunker && isAntiArmor;
				const bool isBasicAntiInfantryDefense = !isPureAA && !isUnarmedBunker && isAntiInf && !isAntiArmor;

				const int tierQualityBonus
					= isUnarmedBunker ? 100 : std::min(950, (pBld->Cost / 2) + std::max(0, pBld->TechLevel) * 150);

				score = (isPureAA ? scores[6] : std::max(static_cast<int>(scores[5]), 320)) + tierQualityBonus
					+ CounterBonusFor(pHouse, pBld);

				if (islandHoldActive && (isPureAA ? islandHoldNeedsAA : !isUnarmedBunker))
					score += 450;

				if (isPureAA)
				{
					score -= totalAADefenses * 320 + owned * 380;
				}
				else if (frontlineDefenseDeficit > 0 || enemyCloseToOurBuildings)
				{
					// Frontline Base-Crawling / Enemy Proximity / Resource Choke Harassment:
					// Do NOT penalize by global home-base defense count when a forward sector
					// or base-crawl choke needs a battery of 3..4 defenses!
					score += 680 + frontlineDefenseDeficit * 220 + (isAntiArmorOrHighTierDefense ? 340 : 0)
						- totalGroundDefenses * 90 - owned * 120;
				}
				else if (needsOutpostPointDefense)
				{
					score += 580 + (isAntiArmorOrHighTierDefense ? 260 : 0) - owned * 220
						- totalGroundDefenses * 120;
				}
				else
				{
					score -= totalGroundDefenses * 210 + owned * 250 + (isUnarmedBunker ? 450 : 0);
				}

				// Proactive Heavy-Armor Shielding: build anti-armor / high-tier defenses BEFORE
				// the enemy's tank column arrives at an undefended base!
				const int desiredCoreGroundDefs = std::clamp(2 + ownedRefineries + (features[65] >= 18 ? 2 : 0), 3, 8);
				if (!isPureAA && !isUnarmedBunker && totalGroundDefenses < desiredCoreGroundDefs)
				{
					score += (desiredCoreGroundDefs - totalGroundDefenses) * 240
						+ ((isAntiArmorOrHighTierDefense && (features[65] >= 12 || hasTechCenter)) ? 380 : 0);
				}

				const bool hasInitialCoreDefenses
					= isPureAA ? (totalAADefenses >= 2) : (totalGroundDefenses >= desiredCoreGroundDefs);

				if (!needsOutpostPointDefense && !effectiveFrontlineThreat && hasInitialCoreDefenses
					&& !wasRecentlyAttacked && hasPendingCoreTechOrEco && money < 2800)
				{
					score -= 650;
				}

				// AdvAI Proactive Enemy Air Value Scaling
				if (isAntiAir && enemyAirValue > 0 && totalAADefenses < (1 + enemyAirValue / 35))
				{
					score += std::min(550, enemyAirValue * 4);
				}

				if (wasRecentlyAttacked || effectiveFrontlineThreat)
				{
					if ((underAirAttack || activeThreatCat == NeuralThreatCategory::Air) && isAntiAir)
					{
						score += 650 + (isPureAA ? 250 : 0);
					}
					else if (!isPureAA && (underVehicleAttack || activeThreatCat == NeuralThreatCategory::Vehicle || features[65] >= 18))
					{
						if (isAntiArmorOrHighTierDefense)
							score += 780;
						else if (isBasicAntiInfantryDefense && (totalGroundDefenses >= 2 || owned >= 2))
							score -= 650;
					}
					else if (!isPureAA && (underInfantryAttack || activeThreatCat == NeuralThreatCategory::Infantry)
						&& isAntiInf && !isUnarmedBunker)
					{
						score += 520 + (pBld->Cost <= 800 ? 220 : 0);
					}
					else if (!isUnarmedBunker)
					{
						score += 360;
					}

					if (effectiveFrontlineThreat && !isPureAA && !isUnarmedBunker)
					{
						score += 680 + (isAntiArmorOrHighTierDefense ? 280 : 0);
					}

					// AdvAI Cost-Efficiency Emergency Bonus when credits are below 2000
					if (money < 2000 && !isUnarmedBunker)
					{
						score += std::clamp((1500 - pBld->GetActualCost(pHouse)) / 3, 0, 380);
					}
				}

				if (features[72] > 0 && pBld->Sensors)
					score += 350;
			}
			else
			{
				// Tech & Support buildings (Radar, Battle Lab, Ore Purifier, Industrial Plant,
				// Cloning Vats, Grinder, Gap/Cloak Generator)
				const int supportRadius = GetSupportRadius(pBld);
				const int maxAllowed = (supportRadius > 0 && hasTechCenter && money > 3000) ? 2 : 1;

				if (owned >= maxAllowed)
					continue;

				score = scores[4] + 550 + std::min(600, pBld->Cost / 3) - owned * 700;

				const bool isCoreTechUnlock = pBld->SpySat
					|| (RulesClass::Instance
						&& (RulesClass::Instance->BuildTech.FindItemIndex(pBld) != -1
							|| RulesClass::Instance->BuildRadar.FindItemIndex(pBld) != -1))
					|| (pBld->TechLevel >= 3 && pBld->Cost >= 1000 && pBld->PowerDrain > 0);

				if (isCoreTechUnlock && owned == 0 && hasCoreEconomyAndProduction)
				{
					score += 750 + (underVehicleAttack ? 450 : 0);
				}

				if (pHouse->NumConYards >= 2 && SlotGate(scores[82], 60, 82))
					score += 250 + std::clamp(static_cast<int>(scores[82]) / 2, 0, 300);
			}

			score += techUnlockBonus;

			if (GetMacroState(pHouse).ConsecutivePlacementFailures == 1 && pBld->BaseNormal
				&& pBld->GetFoundationWidth() <= 2 && pBld->GetFoundationHeight(false) <= 2)
			{
				score += 900; // Try a compact 2x2 BaseNormal anchor once to expand
							// legal Adjacent= perimeter out of tight terrain!
			}

			if (!hasActiveBuildingIncome)
			{
				if (isIncomeRecoveryBld)
				{
					score += 4500; // Zero-Income Emergency: prioritize restoring an
								// income source above all else!
				}
				else
				{
					score -= 3500;
				}
			}

			if (!mcvPrerequisites.empty() && pHouse->CountOwnedNow(pBld) == 0
				&& std::find(mcvPrerequisites.begin(), mcvPrerequisites.end(), pBld->ArrayIndex) != mcvPrerequisites.end())
			{
				// Ahead of routine picks, whatever the learned slot of that building says: without it
				// the house never builds another MCV.
				score = std::max(score + 900, 2600 + static_cast<int>(scores[30]) / 4);
			}

			if (isBldInProgress && (!pHouse->HasLowPower() || pBld->PowerBonus > pBld->PowerDrain))
			{
				score += 850; // Commit to finishing partially constructed buildings in
							// the ConYard!
			}

			if (score > bestBldScore)
			{
				bestBldScore = score;
				bestBldIdx = pBld->ArrayIndex;
			}
		}

		// Pre-Build Power Cushion Interception (from AdvAI):
		// If the highest-scoring building drains power and constructing it would
		// drop our real power surplus below our safety cushion (risking LowPower
		// if a Power Plant is damaged), build a Power Plant first!
		if (bestBldIdx >= 0 && bestPowerPlantIdx >= 0 && bestBldIdx != bestPowerPlantIdx)
		{
			if (auto const pChosen = BuildingTypeClass::Array.GetItem(bestBldIdx))
			{
				if (pChosen->PowerDrain > 0 && pChosen->PowerBonus <= pChosen->PowerDrain)
				{
					const int projectedSurplus = realPowerSurplus - pChosen->PowerDrain;

					const int minSafeAfterBuild
						= (poweredDefAndSWCount > 0 || (pChosen->IsBaseDefense && pChosen->Powered))
						? std::max(120, requiredPowerCushion / 2)
						: 50;

					if (pHouse->HasLowPower() || hasUnpoweredBuildings || projectedSurplus < minSafeAfterBuild)
						bestBldIdx = bestPowerPlantIdx;
				}
			}
		}

		if (bestBldIdx >= 0)
		{
			auto const pChosenBld = BuildingTypeClass::Array.GetItem(bestBldIdx);
			const bool chosenIsStaticRefWithHarv = pChosenBld
				&& (pChosenBld->Refinery || pChosenBld->ResourceDestination) && !pChosenBld->ResourceGatherer
				&& pChosenBld->Enslaves == nullptr && pChosenBld->UndeploysInto == nullptr
				&& pChosenBld->FreeUnit != nullptr
				&& (pChosenBld->FreeUnit->Harvester || pChosenBld->FreeUnit->ResourceGatherer);
			if (chosenIsStaticRefWithHarv && wantsHarvesterRecoveryViaRefinery
				&& (!reachedNewOreField || orphanRefineryZoneNeedsReplacement >= 0 || permanentRefineryCapReached)
				&& staticRefineriesCount >= 1)
			{
				GetMacroState(pHouse).IsBuildingRecoveryRefinery = true;
				GetMacroState(pHouse).TargetRecoveryRefineryZone = orphanRefineryZoneNeedsReplacement;
			}
			else if (!chosenIsStaticRefWithHarv)
			{
				GetMacroState(pHouse).IsBuildingRecoveryRefinery = false;
				GetMacroState(pHouse).TargetRecoveryRefineryZone = -1;
			}
		}

		pExt->NeuralAI_NextBuildingTarget = bestBldIdx;
	}

	productionClock.Mark(NeuralSection::ProdUnits);

	// Unit & Naval Target Selection
	int bestUnitIdx = -1;
	int bestUnitScore = -32000;
	int bestNavalIdx = -1;
	int bestNavalScore = -32000;
	bool ownsUndeployedMCV = false;
	bool ownsUndeployedKeyStructure = false;
	int ownedAmphibTransports = 0;
	int ownedHeavyIslandTransports = 0;
	int ownedUnarmedSupportVehicles = 0;
	int ownedTransportTotalCapacity = 0;
	int waitingBattalionSizeOnIslands = 0;
	int maxWaitingUnitSizeOnIslands = 0;
	int waitingSizeByUnitSize[16] = { 0 };
	int ownedTransCountByLimit[16] = { 0 };
	int ownedTransCapByLimit[16] = { 0 };
	int ownedIdleEmptyTransByLimit[16] = { 0 };
	int ownedNavalOrAmphibCombatUnits = 0;
	int ownedGroundCombatUnits = 0;
	int ownedLandStrikeTransports = 0;
	int idleLandStrikeTransports = 0;
	int ownedExpensiveVehicles = 0;
	int carryallLiftJobs = 0;
	int liftableUndeployedMCVs = 0;
	int liftableHarvesters = 0;

	auto getTransportEffectiveSizeLimit = [](const TechnoTypeClass* pTType) -> int
	{
		if (!pTType)
			return 0;

		if (auto const pExt = TechnoTypeExt::Fetch(pTType))
		{
			if (pExt->Carryall)
			{
				if (pExt->Carryall_SizeLimit.isset() && pExt->Carryall_SizeLimit.Get() > 0)
					return std::clamp(pExt->Carryall_SizeLimit.Get(), 1, 15);
				if (static_cast<int>(pTType->SizeLimit) > 0)
					return std::clamp(static_cast<int>(pTType->SizeLimit), 1, 15);
				return 15;
			}
		}

		return std::clamp(static_cast<int>(pTType->SizeLimit), 0, 15);
	};

	int maxBuildableTransportSizeLimit = 0;
	for (auto const pCheckUT : UnitTypeClass::Array)
	{
		if (!pCheckUT || !CanBuildUnderHumanRules(pHouse, pCheckUT))
			continue;

		if (IsAmphibiousOrAirTransport(pCheckUT)
			|| (TechnoTypeExt::Fetch(pCheckUT) && TechnoTypeExt::Fetch(pCheckUT)->Carryall))
		{
			maxBuildableTransportSizeLimit = std::max(
				maxBuildableTransportSizeLimit, getTransportEffectiveSizeLimit(pCheckUT));
		}
	}

	for (auto const pOwnedU : UnitClass::Array)
	{
		if (pOwnedU && pOwnedU->Owner == pHouse && !pOwnedU->InLimbo && pOwnedU->IsAlive && pOwnedU->Type)
		{
			auto const pOwnedType = pOwnedU->Type;
			const auto pOwnedWpn = pOwnedType->GetWeapon(0) ? pOwnedType->GetWeapon(0)->WeaponType : nullptr;
			if (!pOwnedType->Harvester && !pOwnedType->ResourceGatherer
				&& !(pOwnedType->DeploysInto && pOwnedType->DeploysInto->ConstructionYard)
				&& !IsAmphibiousOrAirTransport(pOwnedType))
			{
				const bool isNavalOrConvertedShip = pOwnedType->Naval
					|| ((pOwnedType->MovementZone == MovementZone::Amphibious
							|| pOwnedType->MovementZone == MovementZone::AmphibiousDestroyer
							|| pOwnedType->MovementZone == MovementZone::AmphibiousCrusher)
						&& pOwnedType->TechLevel > 10);
				if (isNavalOrConvertedShip)
					ownedNavalOrAmphibCombatUnits++;
				else if (pOwnedWpn != nullptr)
					ownedGroundCombatUnits++;
			}

			if (pOwnedType->DeploysInto && pOwnedType->DeploysInto->ConstructionYard)
			{
				ownsUndeployedMCV = true;
			}
			else if (pOwnedType->DeploysInto && IsTrueBaseStructureTarget(pOwnedType->DeploysInto)
				&& !pOwnedType->Harvester && !pOwnedType->ResourceGatherer
				&& (!pOwnedType->DeploysInto->IsBaseDefense || !pOwnedWpn))
			{
				ownsUndeployedKeyStructure = true;
			}
			else if (IsAmphibiousOrAirTransport(pOwnedType)
				|| (TechnoTypeExt::Fetch(pOwnedType) && TechnoTypeExt::Fetch(pOwnedType)->Carryall))
			{
				const int effLimit = getTransportEffectiveSizeLimit(pOwnedType);
				const int passCap = std::max(1, static_cast<int>(pOwnedType->Passengers));
				ownedTransCountByLimit[effLimit]++;
				ownedTransCapByLimit[effLimit] += passCap;

				if (effLimit >= 6)
					ownedHeavyIslandTransports++;

				// An empty transport parked without orders is unused shuttle capacity.
				const bool isParkedEmpty = pOwnedU->Passengers.NumPassengers == 0 && pOwnedU->Destination == nullptr
					&& (pOwnedU->CurrentMission == Mission::Guard || pOwnedU->CurrentMission == Mission::Area_Guard
						|| pOwnedU->CurrentMission == Mission::Sleep);

				if (isParkedEmpty)
					ownedIdleEmptyTransByLimit[effLimit]++;

				if (!pOwnedType->Naval && IsAmphibiousOrAirTransport(pOwnedType))
				{
					ownedLandStrikeTransports++;

					if (isParkedEmpty)
						idleLandStrikeTransports++;
				}
			}
			else if (!pOwnedWpn && !pOwnedType->Harvester && !pOwnedType->ResourceGatherer
				&& pOwnedType->Passengers == 0)
			{
				ownedUnarmedSupportVehicles++;
			}

			// Airlift jobs the carryalls really have (same rules as the carryall dispatcher):
			// critically damaged expensive vehicles to rescue and loaded strike APCs to drop.
			auto const pOwnedTypeExt = TechnoTypeExt::Fetch(pOwnedType);

			if (!pOwnedType->Naval && !pOwnedType->Organic && !pOwnedType->NonVehicle
				&& (!pOwnedTypeExt || pOwnedTypeExt->Carryall_Allowed.Get()))
			{
				if (pOwnedType->DeploysInto && pOwnedType->DeploysInto->ConstructionYard)
					liftableUndeployedMCVs++;
				else if (pOwnedType->Harvester || pOwnedType->Weeder)
					liftableHarvesters++;

				if (pOwnedType->Cost >= 1000)
				{
					ownedExpensiveVehicles++;

					if (pHouse->UnitRepairStations.Count > 0 && pOwnedU->GetHealthPercentage() < 0.40)
						carryallLiftJobs++;
				}

				if (pOwnedType->Passengers >= 3 && pOwnedU->Passengers.NumPassengers >= 1
					&& !IsAmphibiousOrAirTransport(pOwnedType))
				{
					carryallLiftJobs++;
				}
			}
		}
	}

	// Aircraft passenger transports (not carryalls) shuttle like amphibious transports, so
	// they count toward the owned ferry capacity: no extra ship is built for the same demand.
	int ownedCarryallAircraft = 0;
	int idleCarryallAircraft = 0;

	for (auto const pOwnAir : AircraftClass::Array)
	{
		if (!pOwnAir || pOwnAir->Owner != pHouse || pOwnAir->InLimbo || !pOwnAir->IsAlive || !pOwnAir->Type
			|| pOwnAir->SpawnOwner)
		{
			continue;
		}

		auto const pAirType = pOwnAir->Type;
		const bool isAirIdle = pOwnAir->Passengers.NumPassengers == 0
			&& (pOwnAir->CurrentMission == Mission::Guard || pOwnAir->CurrentMission == Mission::Area_Guard
				|| pOwnAir->CurrentMission == Mission::Sleep);

		if (pAirType->Carryall)
		{
			ownedCarryallAircraft++;

			if (isAirIdle)
				idleCarryallAircraft++;
		}
		else if (pAirType->Passengers >= 3)
		{
			const int effLimit = std::clamp(static_cast<int>(pAirType->SizeLimit), 0, 15);
			ownedTransCountByLimit[effLimit]++;
			ownedTransCapByLimit[effLimit] += pAirType->Passengers;

			if (isAirIdle)
				ownedIdleEmptyTransByLimit[effLimit]++;
		}
	}

	// Riders for land strike transports: engineers / C4 infantry always board one, any idle
	// combat infantry boards a subterranean one.
	int ownedStrikeInfantry = 0;
	int idleCombatInfantry = 0;

	for (auto const pOwnInf : InfantryClass::Array)
	{
		if (!pOwnInf || pOwnInf->Owner != pHouse || pOwnInf->InLimbo || !pOwnInf->IsAlive || !pOwnInf->Type)
			continue;

		if (pOwnInf->Type->Engineer || pOwnInf->Type->C4)
		{
			ownedStrikeInfantry++;
		}
		else if (!pOwnInf->Type->Doggie && pOwnInf->Type->GetWeapon(0) && pOwnInf->Type->GetWeapon(0)->WeaponType
			&& (pOwnInf->CurrentMission == Mission::Guard || pOwnInf->CurrentMission == Mission::Area_Guard))
		{
			idleCombatInfantry++;
		}
	}

	// Carryalls wanted: one per pending airlift job (at most 3), plus one standing carryall
	// once there is a repair station and a sizeable expensive vehicle force to rescue.
	int wantedCarryalls = std::max(
		(pHouse->UnitRepairStations.Count > 0 && ownedExpensiveVehicles >= 6) ? 1 : 0, std::min(3, carryallLiftJobs));

	// Ferry demand is what the transport controller really has to carry this tick
	// (published by ExecuteTacticalAndSpecOpsMicro), never every ground unit we own:
	// units already fighting or standing on the enemy island need no transport.
	{
		auto const& macro = GetMacroState(pHouse);
		const bool isDemandFresh = macro.FerryDemandFrame >= 0
			&& (Unsorted::CurrentFrame - macro.FerryDemandFrame) <= 600;

		for (int size = 1; size <= 15; ++size)
		{
			const int demandSize = isDemandFresh ? macro.FerryDemandSizeBySize[size] : 0;

			if (demandSize <= 0)
				continue;

			waitingSizeByUnitSize[size] = demandSize;
			waitingBattalionSizeOnIslands += demandSize;
			maxWaitingUnitSizeOnIslands = std::max(maxWaitingUnitSizeOnIslands, size);
		}
	}

	// Compute ownedAmphibTransports and ownedTransportTotalCapacity against the
	// largest passenger that is really waiting for a ferry:
	ownedAmphibTransports = 0;
	ownedTransportTotalCapacity = 0;
	const int minReqLimitForMainArmy = std::max(3, maxWaitingUnitSizeOnIslands);
	for (int lim = minReqLimitForMainArmy; lim <= 15; ++lim)
	{
		ownedAmphibTransports += ownedTransCountByLimit[lim];
		ownedTransportTotalCapacity += ownedTransCapByLimit[lim];
	}

	for (auto const pOwnB : pHouse->Buildings)
	{
		if (pOwnB && !pOwnB->InLimbo && pOwnB->IsAlive && pOwnB->Type && pOwnB->Type->UndeploysInto != nullptr
			&& !IsTrueBaseStructureTarget(pOwnB->Type))
		{
			ownedUnarmedSupportVehicles++;
		}
	}

	// Economy capacity: true refineries plus deployed miners define how many
	// harvesting units are useful
	int trueRefineries = 0;
	int ownedEconomyUnits = 0;
	int ownedCashProduces = 0;
	for (auto const pOwnBldType : pHouse->Buildings)
	{
		if (!pOwnBldType || pOwnBldType->InLimbo || !pOwnBldType->IsAlive || !pOwnBldType->Type)
			continue;

		if (pOwnBldType->Type->ProduceCashAmount > 0)
			ownedCashProduces++;

		if (pOwnBldType->Type->ResourceGatherer)
			ownedEconomyUnits++;
		else if (pOwnBldType->Type->Refinery)
			trueRefineries++;
	}

	for (auto const pEcoUnit : UnitClass::Array)
	{
		if (!pEcoUnit || pEcoUnit->Owner != pHouse || pEcoUnit->InLimbo || !pEcoUnit->IsAlive || !pEcoUnit->Type)
			continue;

		auto const pEcoType = pEcoUnit->Type;

		if (pEcoType->Harvester || pEcoType->ResourceGatherer
			|| (pEcoType->DeploysInto && (pEcoType->DeploysInto->Refinery || pEcoType->DeploysInto->ResourceGatherer)))
		{
			ownedEconomyUnits++;
		}
	}

	const int ecoBiasMod = std::clamp(static_cast<int>(scores[39]) / 220, -1, 2);
	const int economyUnitCap = (trueRefineries > 0) ? std::clamp(2 * trueRefineries + 1 + ecoBiasMod, 2, 18) : 4;

	// Emergency ConYard Reconstruction: If we lost all ConYards and have no MCV
	// on the map, check if our War Factory + Service Depot/Grinder/Tech can
	// build a replacement MCV and whether our economy/situation allows saving up
	// the $3000 for it instead of draining credits on cheap units!
	// MCV airlift: can a carryall (aircraft Carryall=yes or Phobos unit Carryall) lift this
	// vehicle type? Same rules as the dispatcher's CanCarryallLiftUnit, at type level.
	auto const carryallCanLiftType = [](const TechnoTypeClass* pCarryType, bool isCarryall, const UnitTypeClass* pCargo) -> bool
	{
		if (!isCarryall || !pCarryType || !pCargo || pCargo->Organic || pCargo->Naval || pCargo->NonVehicle)
			return false;

		if (auto const pCargoExt = TechnoTypeExt::Fetch(pCargo))
		{
			if (!pCargoExt->Carryall_Allowed.Get())
				return false;
		}

		const int cargoSize = static_cast<int>(pCargo->TechnoTypeClass::Size);

		if (pCarryType->SizeLimit > 0 && cargoSize > static_cast<int>(pCarryType->SizeLimit))
			return false;

		if (auto const pCarryExt = TechnoTypeExt::Fetch(pCarryType))
		{
			if (pCarryExt->Carryall_SizeLimit.isset() && cargoSize > pCarryExt->Carryall_SizeLimit.Get())
				return false;
		}

		return true;
	};

	auto const ownsCarryallLifting = [&](const UnitTypeClass* pCargo) -> bool
	{
		for (auto const pOwnAir : AircraftClass::Array)
		{
			if (pOwnAir && pOwnAir->Owner == pHouse && !pOwnAir->InLimbo && pOwnAir->IsAlive && pOwnAir->Type
				&& carryallCanLiftType(pOwnAir->Type, pOwnAir->Type->Carryall, pCargo))
			{
				return true;
			}
		}

		for (auto const pOwnU : UnitClass::Array)
		{
			if (pOwnU && pOwnU->Owner == pHouse && !pOwnU->InLimbo && pOwnU->IsAlive && pOwnU->Type
				&& carryallCanLiftType(pOwnU->Type, TechnoTypeExt::Fetch(pOwnU->Type) && TechnoTypeExt::Fetch(pOwnU->Type)->Carryall, pCargo))
			{
				return true;
			}
		}

		return false;
	};

	// Does a buildable MCV have an airlifter (owned, or a buildable aircraft carryall)?
	bool ownsMcvAirlifter = false;
	bool canBuildMcvAirlifter = false;

	for (auto const pCheckMcv : UnitTypeClass::Array)
	{
		if (!pCheckMcv || !pCheckMcv->DeploysInto || !pCheckMcv->DeploysInto->ConstructionYard
			|| !CanBuildUnderHumanRules(pHouse, pCheckMcv))
		{
			continue;
		}

		if (!ownsMcvAirlifter && ownsCarryallLifting(pCheckMcv))
			ownsMcvAirlifter = true;

		if (!canBuildMcvAirlifter && pHouse->NumAirpads > 0)
		{
			for (auto const pCarryAir : AircraftTypeClass::Array)
			{
				if (pCarryAir && pCarryAir->Carryall && CanBuildUnderHumanRules(pHouse, pCarryAir)
					&& carryallCanLiftType(pCarryAir, true, pCheckMcv))
				{
					canBuildMcvAirlifter = true;
					break;
				}
			}
		}
	}

	// Island MCV transport capacity: a heavy ferry or a carryall able to lift the MCV.
	const bool hasIslandMcvLift = ownedHeavyIslandTransports >= 1 || ownsMcvAirlifter;

	bool canFerryAnyMCVAcrossWater = hasIslandMcvLift;
	if (!canFerryAnyMCVAcrossWater)
	{
		for (auto const pCheckU : UnitTypeClass::Array)
		{
			if (pCheckU && pCheckU->DeploysInto && pCheckU->DeploysInto->ConstructionYard
				&& CanBuildUnderHumanRules(pHouse, pCheckU))
			{
				if (CanHouseFerryTypeAcrossWater(pHouse, pCheckU))
				{
					canFerryAnyMCVAcrossWater = true;
					break;
				}
			}
		}
	}

	const bool hasUncolonizedIslandTarget
		= (features[21] == 50 || features[21] >= 100 || (features[21] > 0 && features[20] > 6));
	// Free ore fields on our landmass raise the ConYard cap like the slot 30 score does (one more
	// per free field up to 3, one more with a big bank): a fixed 3 kept rich houses at home.
	// They also come before islands: an island waiting to be colonized used to send every
	// expansion MCV to wait for a ferry while the home landmass stayed empty.
	const int freeOreFieldsForYards = GetMacroState(pHouse).UncolonizedOreFields;
	const CellStruct mcvHomeCell = (pHouse->ConYards.Count > 0 && pHouse->ConYards.GetItem(0))
		? pHouse->ConYards.GetItem(0)->GetMapCoords()
		: pHouse->BaseSpawnCell;
	const bool hasFreeHomeLandField = mcvHomeCell != CellStruct::Empty
		&& FindFreeOreFieldCell(pHouse, mcvHomeCell, MapClass::Instance.GetMovementZoneType(mcvHomeCell, MovementZone::Normal, false), 0) != nullptr;
	const int landYardCap = std::clamp(2 + std::min(3, freeOreFieldsForYards) + (money >= 15000 ? 1 : 0), 3, 6);
	const int maxAllowedConYards = (features[21] > 0)
		? std::max(hasUncolonizedIslandTarget ? 4 : 2, (hasFreeHomeLandField && features[20] > 12) ? landYardCap : 0)
		: ((features[20] > 12) ? landYardCap : 2);
	const bool canSafelySaveForReplacementMCV = (pHouse->NumConYards == 0 && !ownsUndeployedMCV)
		&& ((ownedEconomyUnits > 0 && trueRefineries > 0 && features[65] < 45) || money >= 1500
			|| pHouse->OwnedUnits >= 3);
	const bool needsCrossIslandMCV = (features[21] > 0 && !hasFreeHomeLandField && (features[19] == 0 || pHouse->NumConYards >= 1));

	// Island expansion is one more airlift job when a carryall may lift the MCV (or a spare
	// harvester for the new island's resources): mods can forbid it with Carryall.Allowed=no.
	if (hasUncolonizedIslandTarget && (liftableUndeployedMCVs > 0 || liftableHarvesters >= 2))
		wantedCarryalls = std::min(3, wantedCarryalls + 1);
	// Chicken-and-egg: with no heavy ferry, the carryall that will lift the expansion MCV is
	// wanted before the MCV exists (the MCV itself is only built once it has a lift).
	if (hasUncolonizedIslandTarget && canBuildMcvAirlifter && !hasIslandMcvLift && pHouse->NumConYards >= 1
		&& pHouse->NumConYards < maxAllowedConYards)
	{
		wantedCarryalls = std::max(wantedCarryalls, 1);
	}

	const bool wantsArchipelagoExpansion
		= (hasUncolonizedIslandTarget && hasIslandMcvLift && pHouse->NumConYards < maxAllowedConYards
			&& (features[19] < 28 || pHouse->Buildings.Count >= 12 || money >= 2200));
	const bool desperateIslandOrOreExpansion
		= (pHouse->NumConYards >= 1 && pHouse->NumConYards < maxAllowedConYards && !ownsUndeployedMCV
			&& (features[20] > 6 || hasUncolonizedIslandTarget) && (scores[30] >= 450 || wantsArchipelagoExpansion)
			&& (!needsCrossIslandMCV || ownedHeavyIslandTransports >= 1 || canFerryAnyMCVAcrossWater)
			&& (features[19] < 24 || pHouse->Buildings.Count >= 14 || wantsArchipelagoExpansion || SlotGate(scores[81], 85, 81)));
	const bool canSafelySaveForExpansionMCV
		= (pHouse->NumConYards >= 1 && pHouse->NumConYards < maxAllowedConYards && !ownsUndeployedMCV
			&& (features[20] > 6 || hasUncolonizedIslandTarget) && (scores[30] >= 450 || wantsArchipelagoExpansion)
			&& (!needsCrossIslandMCV || ownedHeavyIslandTransports >= 1 || canFerryAnyMCVAcrossWater)
			&& (money >= 1400 || desperateIslandOrOreExpansion));
	const bool canSafelySaveForMCV = canSafelySaveForReplacementMCV || canSafelySaveForExpansionMCV;
	const bool savingForIslandTransport = (features[21] > 0 && pHouse->NumShipyards >= 1
		&& ownedHeavyIslandTransports == 0 && (scores[30] >= 450 || wantsArchipelagoExpansion || features[25] > 0)
		&& money < 1200 && pHouse->OwnedUnits >= 2);

	bool canRebuildMCVNow = false;
	int rebuildMCVCost = 3000;
	int inProgressUnitIdx = -1;
	int inProgressNavalIdx = -1;
	if (FactoryClass* pUnitFac = pHouse->GetPrimaryFactory(AbstractType::UnitType, false, BuildCat::DontCare))
	{
		if (pUnitFac->Object && pUnitFac->Object->GetType())
			inProgressUnitIdx = pUnitFac->Object->GetType()->GetArrayIndex();
	}

	if (FactoryClass* pNavalFac = pHouse->GetPrimaryFactory(AbstractType::UnitType, true, BuildCat::DontCare))
	{
		if (pNavalFac->Object && pNavalFac->Object->GetType())
			inProgressNavalIdx = pNavalFac->Object->GetType()->GetArrayIndex();
	}

	const bool hasActiveUnitIncome = (ownedCashProduces >= 1)
		|| ((trueRefineries >= 1 && ownedEconomyUnits >= 1)
			&& (features[19] > 0 || pHouse->OwnedTiberium.GetTotalAmount() > 0));

	// Ferry transport demand-driven scoring shared by the Shipyard and War Factory.
	// Transports are reusable shuttles: one stays ready on island maps, and more are
	// only built (up to 3) while the real waiting cargo exceeds one trip of the fleet.
	auto scoreFerryTransport = [&](const TechnoTypeClass* pCandidate) -> int
	{
		const int candSizeLimit = std::max(1, getTransportEffectiveSizeLimit(pCandidate));
		const int candPassCap = std::max(1, static_cast<int>(pCandidate->Passengers));

		int fittingDemandSize = 0;
		int unfitDemandSize = 0;

		for (int size = 1; size <= 15; ++size)
		{
			if (size <= candSizeLimit)
				fittingDemandSize += waitingSizeByUnitSize[size];
			else
				unfitDemandSize += waitingSizeByUnitSize[size];
		}

		int ownedForLimit = 0;
		int ownedCapForLimit = 0;
		int idleEmptyForLimit = 0;

		for (int lim = candSizeLimit; lim <= 15; ++lim)
		{
			ownedForLimit += ownedTransCountByLimit[lim];
			ownedCapForLimit += ownedTransCapByLimit[lim];
			idleEmptyForLimit += ownedIdleEmptyTransByLimit[lim];
		}

		// A compatible transport already being built in the other factory counts as owned.
		bool compatibleHeavyInProgress = false;

		for (const int inProgressIdx : { inProgressUnitIdx, inProgressNavalIdx })
		{
			if (inProgressIdx < 0 || inProgressIdx >= UnitTypeClass::Array.Count)
				continue;

			auto const pBuilding = UnitTypeClass::Array.GetItem(inProgressIdx);

			if (pBuilding == pCandidate)
				continue;

			auto const pBuildingExt = TechnoTypeExt::Fetch(pBuilding);

			if (!IsAmphibiousOrAirTransport(pBuilding) && !(pBuildingExt && pBuildingExt->Carryall))
				continue;

			const int buildingLimit = getTransportEffectiveSizeLimit(pBuilding);

			if (buildingLimit >= candSizeLimit)
				ownedForLimit++;

			if (buildingLimit >= 6)
				compatibleHeavyInProgress = true;
		}

		const bool isIslandWar = features[21] >= 75;
		const bool isIslandMap = features[21] > 0;
		const int neededForDemand = (fittingDemandSize + candPassCap - 1) / candPassCap;
		const int targetFleet = isIslandWar
			? std::clamp(neededForDemand, 1, 3)
			: ((isIslandMap || neededForDemand > 0) ? 1 : 0);
		const bool needsHeavyMcvTransport = candSizeLimit >= 6 && ownedHeavyIslandTransports == 0
			&& !compatibleHeavyInProgress && (isIslandMap || scores[30] >= 450);

		if (needsHeavyMcvTransport)
			return scores[28] + 2200;

		// Cannot carry what is actually waiting (e.g. light APC vs. heavy tanks).
		if (fittingDemandSize == 0 && unfitDemandSize > 0)
			return -1400;

		// No water crossing is waiting: the transport is still worth building for its other
		// real jobs, sized by that demand (never a fixed fleet):
		if (fittingDemandSize == 0)
		{
			auto const pCandExt = TechnoTypeExt::Fetch(pCandidate);

			if (pCandExt && pCandExt->Carryall)
			{
				// Airlift (damaged vehicle rescue, loaded strike APC drops).
				if (ownedForLimit + ownedCarryallAircraft < wantedCarryalls && idleEmptyForLimit == 0
					&& idleCarryallAircraft == 0)
				{
					return std::max(static_cast<int>(scores[28]), 0) + (carryallLiftJobs > 0 ? 700 : 350);
				}
			}
			else if (pCandidate->WhatAmI() == AbstractType::UnitType && !pCandidate->Naval
				&& IsAmphibiousOrAirTransport(pCandidate))
			{
				// Land strike APC: one per load of the riders that would really board it.
				const int riders = ownedStrikeInfantry
					+ ((pCandidate->MovementZone == MovementZone::Subterrannean) ? idleCombatInfantry : 0);
				const int wantedLand = std::clamp((riders + candPassCap - 1) / candPassCap, 0, 2);

				if (ownedLandStrikeTransports < wantedLand && idleLandStrikeTransports == 0)
					return std::max(static_cast<int>(scores[28]), 0) + 350;
			}
		}

		if (ownedForLimit >= targetFleet || idleEmptyForLimit > 0)
			return -1500;

		if (ownedForLimit == 0)
			return std::max(static_cast<int>(scores[28]), 0) + (fittingDemandSize > 0 ? 1100 : 600);

		if (fittingDemandSize > ownedCapForLimit)
			return std::max(static_cast<int>(scores[28]), 0) + 900;

		return -900;
	};

	for (auto const pUnit : UnitTypeClass::Array)
	{
		if (!pUnit || !CanBuildUnderHumanRules(pHouse, pUnit))
			continue;

		const bool isUnitInProgress
			= (pUnit->ArrayIndex == inProgressUnitIdx || pUnit->ArrayIndex == inProgressNavalIdx);

		if (!isUnitInProgress && !IsUnitTierAllowed(pHouse, pUnit))
			continue;

		const bool isMCV = (pUnit->DeploysInto && pUnit->DeploysInto->ConstructionYard);
		if (isMCV)
		{
			const bool isSelfAmphibOrAirMcv = (pUnit->MovementZone == MovementZone::Amphibious
				|| pUnit->MovementZone == MovementZone::AmphibiousDestroyer
				|| pUnit->MovementZone == MovementZone::AmphibiousCrusher || pUnit->MovementZone == MovementZone::Fly
				|| pUnit->JumpJet);
			if (ownsUndeployedMCV || (pHouse->NumConYards >= 1 && scores[30] < 400 && scores[81] < 85)
				|| pHouse->NumConYards >= maxAllowedConYards
				|| (pHouse->NumConYards >= 1 && (pHouse->Buildings.Count >= 35 || ownedGroundCombatUnits + ownedNavalOrAmphibCombatUnits >= 40)
					&& (freeOreFieldsForYards == 0 || money < 5000)))
			{
				continue;
			}

			if (pHouse->NumConYards >= 1 && features[20] <= 6 && !hasUncolonizedIslandTarget)
				continue;

			// A house with a ConYard but nothing gathering rebuilds its income first (a harvester, or a
			// refinery and its free harvester): an expansion MCV would take its last credits and the
			// new base would have no harvester either.
			if (pHouse->NumConYards >= 1 && ownedEconomyUnits == 0 && ownedCashProduces == 0)
				continue;

			if (pHouse->NumConYards >= 1 && needsCrossIslandMCV
				&& (!CanHouseFerryTypeAcrossWater(pHouse, pUnit)
					|| (!isSelfAmphibOrAirMcv && ownedHeavyIslandTransports == 0 && !ownsCarryallLifting(pUnit))))
			{
				continue;
			}

			if (canSafelySaveForMCV)
			{
				canRebuildMCVNow = true;
				rebuildMCVCost = pUnit->GetActualCost(pHouse);
			}
		}

		const auto pWpnStruct = pUnit->GetWeapon(0);
		const auto pWpn = pWpnStruct ? pWpnStruct->WeaponType : nullptr;

		const bool isKeyStructureDeployer = (pUnit->DeploysInto != nullptr
			&& IsTrueBaseStructureTarget(pUnit->DeploysInto) && !isMCV && !pUnit->Harvester && !pUnit->ResourceGatherer
			&& (!pUnit->DeploysInto->IsBaseDefense || pWpn == nullptr));

		if (isKeyStructureDeployer && ownsUndeployedKeyStructure)
			continue;

		// A mobile factory is built only with a spot waiting for it (forward production, an island or
		// expansion hub without that factory kind, a lost main factory, see NeuralAI.MobileFactory.inl);
		// otherwise it would unfold next to its own maker.
		const bool isMobileFactory = IsMobileFactoryUnit(pUnit);

		if (isMobileFactory && !isUnitInProgress && !HouseWantsMobileFactory(pHouse, pUnit))
			continue;

		const bool isAmphibTransport = IsAmphibiousOrAirTransport(pUnit);

		if (pUnit->Naval && !isAmphibTransport && pUnit->MovementZone != MovementZone::Amphibious
			&& pUnit->MovementZone != MovementZone::AmphibiousDestroyer && pUnit->MovementZone != MovementZone::AmphibiousCrusher)
		{
			bool shipyardOnUsefulWater = false;

			for (auto const pOwnB : pHouse->Buildings)
			{
				if (pOwnB && pOwnB->IsAlive && !pOwnB->InLimbo && pOwnB->Type && pOwnB->Type->Naval
					&& pOwnB->Type->Factory == AbstractType::UnitType && IsUsefulWaterCell(pOwnB->GetMapCoords()))
				{
					shipyardOnUsefulWater = true;
					break;
				}
			}

			if (!shipyardOnUsefulWater || !HouseNeedsNaval(pHouse))
				continue;
		}

		const bool isDeployableRefineryOrMiner = (pUnit->DeploysInto
			&& (pUnit->DeploysInto->Refinery || pUnit->DeploysInto->ResourceGatherer
				|| pUnit->DeploysInto->ResourceDestination));
		const bool allowExpansionRefineryDeployer = isDeployableRefineryOrMiner
			&& (features[21] > 0 || scores[30] >= 500) && (trueRefineries < 10 && ownedEconomyUnits < 18)
			&& !ownsUndeployedKeyStructure && (features[21] == 0 || CanHouseFerryTypeAcrossWater(pHouse, pUnit));

		bool hasRefineryZoneWithZeroHarvesters = false;
		bool hasUnderharvestedIslandWithWarFactory = false;
		// Only for real harvesters: a slave miner deploys into its own refinery and then counts as
		// a building, never as a harvester of the zone, so these exceptions let Yuri build them
		// without end (77 deployed slave miners in one match).
		if ((pUnit->Harvester || pUnit->ResourceGatherer) && !isDeployableRefineryOrMiner)
		{
			for (auto const pRefB : pHouse->Buildings)
			{
				if (!pRefB || pRefB->InLimbo || !pRefB->IsAlive || !pRefB->Type
					|| (!pRefB->Type->Refinery && !pRefB->Type->ResourceDestination))
				{
					continue;
				}

				const int rz
					= ObjectZone(pRefB, MovementZone::Normal);
				if (rz < 0)
					continue;

				int refsInRz = 0;
				int warFacsInRz = 0;
				for (auto const pOB : pHouse->Buildings)
				{
					if (!pOB || pOB->InLimbo || !pOB->IsAlive || !pOB->Type
						|| ObjectZone(pOB, MovementZone::Normal)
							!= rz)
					{
						continue;
					}

					if (pOB->Type->Refinery || pOB->Type->ResourceDestination)
						refsInRz++;
					else if (pOB->Type->Factory == AbstractType::UnitType && !pOB->Type->Naval)
						warFacsInRz++;
				}

				int harvsInRz = 0;
				for (auto const pHU : UnitClass::Array)
				{
					if (pHU && pHU->Owner == pHouse && !pHU->InLimbo && pHU->IsAlive && pHU->Type
						&& (pHU->Type->Harvester || pHU->Type->ResourceGatherer)
						&& ObjectZone(pHU, MovementZone::Normal)
							== rz)
					{
						harvsInRz++;
					}
				}

				if (harvsInRz == 0 && (warFacsInRz >= 1 || CanHouseFerryTypeAcrossWater(pHouse, pUnit)))
					hasRefineryZoneWithZeroHarvesters = true;
				else if (warFacsInRz >= 1 && harvsInRz < refsInRz * 2)
					hasUnderharvestedIslandWithWarFactory = true;
			}
		}

		const bool isEconomyUnit = pUnit->Harvester || pUnit->ResourceGatherer || isDeployableRefineryOrMiner;
		if (isEconomyUnit && ownedEconomyUnits >= economyUnitCap && !allowExpansionRefineryDeployer
			&& !hasRefineryZoneWithZeroHarvesters && !hasUnderharvestedIslandWithWarFactory)
		{
			continue;
		}

		TechnoTypeClass* pConvDeployType = GetConvertDeployTargetType(pUnit);
		const bool isConvertToOpenToppedOrCombat = (pConvDeployType != nullptr)
			&& ((pConvDeployType->OpenTopped && pConvDeployType->Passengers > 0)
				|| (pConvDeployType->GetWeapon(0) && pConvDeployType->GetWeapon(0)->WeaponType
					&& pConvDeployType->GetWeapon(0)->WeaponType->Damage > 0));
		if (isConvertToOpenToppedOrCombat && pConvDeployType->OpenTopped && pConvDeployType->Passengers > 0)
		{
			const int ownedConvTotal = pHouse->CountOwnedNow(pUnit)
				+ (abstract_cast<UnitTypeClass*>(pConvDeployType)
						? pHouse->CountOwnedNow(abstract_cast<UnitTypeClass*>(pConvDeployType))
						: 0);
			if (ownedConvTotal >= 2 || pHouse->OwnedInfantry < 3 || canSafelySaveForMCV || savingForIslandTransport)
				continue;
		}

		const bool isUnarmedSupportVehicle = (!pUnit->Naval && pWpn == nullptr && !isMCV && !isKeyStructureDeployer
			&& !isEconomyUnit && !isConvertToOpenToppedOrCombat && pUnit->Passengers == 0
			&& !(TechnoTypeExt::Fetch(pUnit) && TechnoTypeExt::Fetch(pUnit)->Carryall));
		if (isUnarmedSupportVehicle)
		{
			// Hard cap unarmed support/detector/jammer vehicles (Mobile Radar Jammer
			// MRJ_SOV/MRJ_ALL, Mobile Stealth Generator SGEN/GGEN, Mobile Sensor
			// Array LPST/NLPST, Limpet Drone) at 1 across the house so they are never
			// spammed!
			if (ownedUnarmedSupportVehicles >= 1 || pHouse->OwnedUnits < 8 || canSafelySaveForMCV
				|| savingForIslandTransport)
			{
				continue;
			}
		}

		const bool isEmpOrMindControlSupport = (pWpn != nullptr && pUnit->Spawns == nullptr && !pWpn->Spawner
			&& ((pWpn->Warhead != nullptr
					&& (pWpn->Warhead->MindControl || (pWpn->Warhead->EMEffect && pWpn->Damage <= 15)))
				|| pWpn->Damage <= 5));
		if (isEmpOrMindControlSupport)
		{
			// Cap Mind-Control vehicles (Mastermind MIND), non-damaging EMP/Scrambler
			// support vehicles/ships (Mobile EMP CMOBILEMP, CABAL Gorgon ROBOTGORGON2) at 2 owned!
			if (CountOwnedUnitWithConversions(pHouse, pUnit) >= 2 || (pHouse->OwnedUnits + pHouse->OwnedNavy) < 6
				|| canSafelySaveForMCV || savingForIslandTransport)
			{
				continue;
			}
		}

		const bool isIncomeRecoveryUnit = isEconomyUnit || (isMCV && canSafelySaveForMCV)
			|| (isAmphibTransport && ownedAmphibTransports == 0 && (features[21] > 0 || features[19] == 0));
		// Only restrict unit choices by remaining cash when all income sources are
		// lost (!hasActiveUnitIncome)!
		if (!hasActiveUnitIncome && !isUnitInProgress && !isIncomeRecoveryUnit && pUnit->GetActualCost(pHouse) > money)
			continue;

		const int ownedUnitWithConv = CountOwnedUnitWithConversions(pHouse, pUnit);
		if (!isEconomyUnit && !isMCV && !isKeyStructureDeployer && !isAmphibTransport)
		{
			const int maxPerCombatType = pUnit->Naval ? 10 : 18;
			if (ownedUnitWithConv >= maxPerCombatType)
				continue;

			const int maxNavalFleet = (features[21] > 0) ? 20 : 12;
			if (pUnit->Naval && ownedNavalOrAmphibCombatUnits >= maxNavalFleet)
				continue;

			if (!pUnit->Naval && ownedGroundCombatUnits >= 50)
				continue;
		}

		int score = (pUnit->Cost / 4) + (pUnit->Strength / 3);
		score -= std::min(1200, ownedUnitWithConv * 55);
		if (!hasActiveUnitIncome)
		{
			if (isIncomeRecoveryUnit)
			{
				score += 4500; // Zero-Income Emergency: focus War Factory / Shipyard on
							// restoring Harvester/Miner/MCV/Transport!
			}
			else
			{
				score -= 3500;
			}
		}

		if (isUnitInProgress)
		{
			score += 850; // Commit to finishing partially constructed vehicles/ships
						// in the War Factory / Shipyard!
		}

		// Expert 2: counters of the seen enemy army lead their factory (combat types only).
		score += CounterBonusFor(pHouse, pUnit);

		if (pUnit->Naval)
		{
			if (isAmphibTransport)
			{
				// A transport the demand rules reject is not a candidate at all: otherwise the
				// queue keeps building it whenever nothing else scores higher (e.g. no buildable
				// warship), producing whole fleets of unneeded transports.
				const int ferryDelta = scoreFerryTransport(pUnit);

				if (ferryDelta <= -900)
					continue;

				score += ferryDelta;
			}
			else
			{
				score += scores[29];
				const auto pWpn1 = pUnit->GetWeapon(1) ? pUnit->GetWeapon(1)->WeaponType : nullptr;
				const bool isUnitCarrierSpawner
					= (pUnit->Spawns != nullptr) || (pWpn && pWpn->Spawner) || (pWpn1 && pWpn1->Spawner);
				const bool wpn0HitsLand = pWpn && (pWpn->Damage > 5 || pWpn->Spawner)
					&& (!pWpn->Projectile || pWpn->Projectile->AG || pWpn->Spawner || isUnitCarrierSpawner);
				const bool wpn1HitsLand = pWpn1 && (pWpn1->Damage > 5 || pWpn1->Spawner)
					&& (!pWpn1->Projectile || pWpn1->Projectile->AG || pWpn1->Spawner);
				const int maxLandSiegeRange = std::max(
					wpn0HitsLand ? (isUnitCarrierSpawner ? std::max(pWpn->Range, 18 * 256) : pWpn->Range) : 0,
					wpn1HitsLand ? pWpn1->Range : 0);
				const bool isLongRangeCoastalSiegeShip
					= (isUnitCarrierSpawner && (wpn0HitsLand || wpn1HitsLand)) || (maxLandSiegeRange >= (10 * 256));
				const bool isCoastalBombardShip = (maxLandSiegeRange >= (5 * 256));

				if (features[21] > 0 || features[67] > 10)
				{
					if (isLongRangeCoastalSiegeShip)
					{
						// Continuously produce long-range capital siege warships
						// (Dreadnought, Aircraft Carrier, Boomer, Nod Missile Sub) to
						// saturate the enemy coastline from afar and end island games
						// rapidly!
						score += 850 + std::min(500, maxLandSiegeRange / 6);
					}
					else if (isCoastalBombardShip)
					{
						score += 520 + std::min(300, maxLandSiegeRange / 10);
					}
				}
			}

			if (features[72] > 0 && pUnit->Sensors)
				score += 400;

			if (score > bestNavalScore)
			{
				bestNavalScore = score;
				bestNavalIdx = pUnit->ArrayIndex;
			}
		}
		else
		{
			const bool isAA = pWpn && pWpn->Projectile && pWpn->Projectile->AA;
			const bool isArtillery = pWpn && pWpn->Range >= (8 * 256);

			if (isMCV)
			{
				score += (pHouse->NumConYards == 0)
					? 12000
					: (scores[30] + (canSafelySaveForExpansionMCV ? 1800 : 0)
						+ ((wantsArchipelagoExpansion && hasIslandMcvLift) ? 900 : 0));
			}
			else if (isKeyStructureDeployer)
			{
				// Universal Deployable Key Structure Scoring across all mods (Mobile
				// Outposts/Explorers, Mobile Refineries/Factories, Power/Tech/Turret
				// Cores, Nanocores):
				auto const pTargetBld = pUnit->DeploysInto;
				const int ownedTargetBld = BuildingTypeExt::CountOwnedNowWithDeployOrUpgrade(pTargetBld, pHouse);

				// Tech role (radar, spy satellite or superweapon carrier): one is enough, it only
				// unlocks the tech tree. It must never be mistaken for an expansion outpost.
				const bool isTechStructure = pTargetBld->Radar || pTargetBld->SpySat || pTargetBld->SuperWeapon != -1
					|| pTargetBld->SuperWeapon2 != -1
					|| (BuildingTypeExt::Fetch(pTargetBld) && !BuildingTypeExt::Fetch(pTargetBld)->SuperWeapons.empty());
				const bool isDedicatedOutpost = !isTechStructure && pTargetBld->BaseNormal && !pTargetBld->Refinery
					&& pTargetBld->Factory == AbstractType::None && pTargetBld->PowerBonus <= pTargetBld->PowerDrain;

				if (isTechStructure && !pTargetBld->Refinery && pTargetBld->Factory == AbstractType::None
					&& pTargetBld->PowerBonus <= pTargetBld->PowerDrain)
				{
					if (ownedTargetBld >= 1)
						score -= 2000;
					else
						score += scores[4];
				}
				else if (isDedicatedOutpost)
				{
					// Mobile Outpost / Explorer / Expansion Hub (Tiberian Sun / Firestorm
					// / Mental Omega / YR mods):
					const int maxDedicatedOutposts = (features[21] > 0) ? 8 : 4;
					if (scores[30] < 300 || ownedTargetBld >= maxDedicatedOutposts)
						score -= 2000;
					else
						score += scores[30] - (ownedTargetBld * 250);
				}
				else if (pTargetBld->PowerBonus > pTargetBld->PowerDrain && !pTargetBld->IsBaseDefense)
				{
					score += scores[0];
				}
				else if (pTargetBld->Refinery)
				{
					score += scores[1] + ((scores[30] > 700) ? (scores[30] / 2) : 0); // Use Mobile Refinery / Refinery
				}

				// Core for Outpost Expansion!
				else if (pTargetBld->Factory == AbstractType::InfantryType)
					score += scores[2];
				else if (pTargetBld->Factory == AbstractType::UnitType && !pTargetBld->Naval)
					score += scores[3]
						+ ((scores[30] > 800) ? (scores[30] / 3) : 0); // Forward Mobile War Factory / Core!
				else if (pTargetBld->Factory == AbstractType::UnitType && pTargetBld->Naval)
					score += scores[7];
				else if (pTargetBld->Factory == AbstractType::AircraftType)
					score += scores[8];
				else if (pTargetBld->UnitRepair)
					score += scores[9] - (ownedTargetBld * 900);
				else if (pTargetBld->SuperWeapon != -1)
					score += scores[10] - (ownedTargetBld * 900);
				else if (pTargetBld->Storage > 0 && !pTargetBld->Refinery)
					score += scores[11];
				else if (pTargetBld->IsBaseDefense)
				{
					const auto pBldWpn = pTargetBld->GetWeapon(0) ? pTargetBld->GetWeapon(0)->WeaponType : nullptr;
					const bool isBldAA = pBldWpn && pBldWpn->Projectile && pBldWpn->Projectile->AA;

					score += isBldAA ? scores[6] : scores[5];
				}
				else
				{
					score += scores[4] - (ownedTargetBld * 900);
				}

				// A mobile factory past the reason gate above has a spot waiting for it.
				if (isMobileFactory)
					score += 500;
			}
			else if (pUnit->Harvester || pUnit->ResourceGatherer)
			{
				const bool conYardHandlingHarvRecovery = (GetMacroState(pHouse).IsBuildingRecoveryRefinery
					&& ownedEconomyUnits >= 1 && pHouse->ConYards.Count >= 1);
				if (conYardHandlingHarvRecovery)
				{
					score += scores[31] - 900; // Keep War Factory queue focused on combat vehicles while
											// ConYard replenishes Harvester via Refinery replacement!
				}
				else
				{
					score += scores[31]
						+ (hasRefineryZoneWithZeroHarvesters
								? 2600
								: (hasUnderharvestedIslandWithWarFactory
										? 1350
										: ((ownedEconomyUnits < 2)
													? 1200
													: (allowExpansionRefineryDeployer ? scores[30] : 0))));
				}
			}
			else if (pUnit->Passengers > 0 || (TechnoTypeExt::Fetch(pUnit) && TechnoTypeExt::Fetch(pUnit)->Carryall))
			{
				const bool isAirOrCarryallVehicle
					= isAmphibTransport || (TechnoTypeExt::Fetch(pUnit) && TechnoTypeExt::Fetch(pUnit)->Carryall);
				if (!isAirOrCarryallVehicle)
				{
					score += scores[28] + (features[21] > 0 ? -500 : 0);
				}
				else
				{
					const int ferryDelta = scoreFerryTransport(pUnit);

					if (ferryDelta <= -900)
						continue; // Not needed: never built just because nothing else scores higher.

					score += ferryDelta;
				}
			}
			else if (false && pUnit->Passengers > 0)
			{
				score += scores[28] + (isAmphibTransport ? 350 : (features[21] > 0 ? -500 : 0));
			}
			else if (isUnarmedSupportVehicle)
			{
				score += scores[60] - 250;
			}
			else if (isAA)
			{
				score += scores[26];
			}
			else if (isArtillery)
			{
				score += scores[27];
			}
			else if (pUnit->Strength < 300)
			{
				score += scores[25];
			}
			else
			{
				score += scores[24];
			}

			if (features[77] > 0 && pUnit->ImmuneToPsionics && !isEconomyUnit && !isMCV)
				score += 420; // Counter enemy MindControl with ImmuneToPsionics

			// vehicles (Drones, Robot Tanks, Automated units)!
			if (features[72] > 0 && (pUnit->Sensors || pUnit->DetectDisguise) && !isEconomyUnit)
				score += 480; // Counter enemy Stealth / Disguised units with mobile

			// Sensor & Detector units!
			if (SlotGate(scores[82], 60, 82) && ownHouseBit != 0u && !pUnit->InOwners(ownHouseBit) && !isEconomyUnit && !isMCV)
			{
				// Slot 82 (Cross-Faction Arsenal Synergy): Mix newly unlocked
				// foreign-faction tanks/vehicles into our strike force!
				score += 240 + std::clamp(static_cast<int>(scores[82]) / 3, 0, 220);
			}

			// Logistics & Anti-Saturation Check on Islands:
			// If our home island already has a large battalion waiting for amphibious/airlift
			// transport, shift War Factory production toward self-ferrying vehicles (Amphibious,
			// Hover, JumpJet, Flying, Teleporter) or Transports so ground-only tanks never
			// saturate and gridlock the home base!
			if (features[21] > 0 && !isEconomyUnit && !isMCV && !isKeyStructureDeployer && !isAmphibTransport
				&& pUnit->Passengers == 0)
			{
				const bool canSelfCrossWater = (pUnit->MovementZone == MovementZone::Amphibious
					|| pUnit->MovementZone == MovementZone::AmphibiousDestroyer
					|| pUnit->MovementZone == MovementZone::AmphibiousCrusher
					|| pUnit->MovementZone == MovementZone::Fly
					|| pUnit->MovementZone == MovementZone::Subterrannean || pUnit->SpeedType == SpeedType::Hover
					|| pUnit->SpeedType == SpeedType::Winged || pUnit->JumpJet || pUnit->Teleporter);

				if (waitingBattalionSizeOnIslands > ownedTransportTotalCapacity + 8)
				{
					if (canSelfCrossWater)
						score += 620;
					else
						score -= 950;
				}
				else if (canSelfCrossWater && money >= 3500)
				{
					score += 280;
				}
			}

			// Expert 0 after a raid: harvesters back to their peak first.
			if ((pUnit->Harvester || pUnit->ResourceGatherer) && !isDeployableRefineryOrMiner
				&& pExt->NeuralAI_ActiveExpert == 0 && GetMacroState(pHouse).EconomyRecoveryActive
				&& ownedEconomyUnits < GetMacroState(pHouse).PeakHarvesters)
			{
				score += 1500;
			}

			if (score > bestUnitScore)
			{
				bestUnitScore = score;

				bestUnitIdx = pUnit->ArrayIndex;
			}
		}
	}

	pExt->NeuralAI_NextUnitTarget = bestUnitIdx;

	pExt->NeuralAI_NextNavalTarget = bestNavalIdx;

	productionClock.Mark(NeuralSection::ProdInfantry);

	// Infantry Target Selection
	int ownedEngineers = 0;
	int ownedInfiltrators = 0;

	for (auto const pOwnInf : InfantryClass::Array)
	{
		if (!pOwnInf || pOwnInf->Owner != pHouse || pOwnInf->InLimbo || !pOwnInf->IsAlive || !pOwnInf->Type)
			continue;

		if (pOwnInf->Type->Engineer)
			ownedEngineers++;
		else if (pOwnInf->Type->Infiltrate)
			ownedInfiltrators++;
	}

	int neutralCapturableCount = 0;
	int neutralCashDerrickCount = 0;

	// The resource hoarder (Expert 6) also counts visible enemy oil derricks as engineer work.
	auto const pHoarderExt = HouseExt::Fetch(pHouse);
	const bool resourceHoarder = pHoarderExt && pHoarderExt->NeuralAI_ActiveExpert == 6;

	for (auto const pMapBld : BuildingClass::Array)
	{
		if (!pMapBld || pMapBld->InLimbo || !pMapBld->IsAlive || !pMapBld->Type || !pMapBld->Owner
			|| !pMapBld->Type->Capturable)
		{
			continue;
		}

		const bool isCashDerrick = pMapBld->Type->ProduceCashAmount > 0 || pMapBld->Type->ProduceCashStartup > 0;
		// Fog-fair: only neutral techs this house has seen (and still remembers as neutral).
		const bool isNeutral = pMapBld->Owner->IsNeutral() && IsKnownNeutralTech(pHouse, pMapBld);
		const bool isVisibleEnemyDerrick = resourceHoarder && isCashDerrick && !pMapBld->Owner->IsNeutral() && pMapBld->Owner != pHouse
			&& !pHouse->IsAlliedWith(pMapBld->Owner) && IsCellVisibleToAI(pHouse, pMapBld->GetCell());

		if (isNeutral || isVisibleEnemyDerrick)
		{
			neutralCapturableCount++;

			if (isCashDerrick)
				neutralCashDerrickCount++;
		}
	}

	const bool hasCapturableTechOnMap = (neutralCapturableCount > 0);
	int unfilledBioReactorSlotsForInf = 0;
	for (auto const pOwnBld : pHouse->Buildings)
	{
		if (pOwnBld && !pOwnBld->InLimbo && pOwnBld->IsAlive && pOwnBld->Type && pOwnBld->Type->InfantryAbsorb
			&& (pOwnBld->Type->PowerBonus > 0 || pOwnBld->Type->ExtraPowerBonus > 0)
			&& !IsTunnelNetworkBuilding(pOwnBld->Type) && pOwnBld->Passengers.NumPassengers < pOwnBld->Type->Passengers)
		{
			unfilledBioReactorSlotsForInf += (pOwnBld->Type->Passengers - pOwnBld->Passengers.NumPassengers);
		}
	}

	const bool needsCheapInfForBioReactor = (unfilledBioReactorSlotsForInf > 0
		&& (pHouse->HasLowPower() || (pHouse->PowerOutput - pHouse->PowerDrain) < 350));

	int bestInfIdx = -1;
	int bestInfScore = -32000;
	int inProgressInfIdx = -1;
	if (FactoryClass* pInfFac = pHouse->GetPrimaryFactory(AbstractType::InfantryType, false, BuildCat::DontCare))
	{
		if (pInfFac->Object && pInfFac->Object->GetType())
			inProgressInfIdx = pInfFac->Object->GetType()->GetArrayIndex();
	}

	for (auto const pInf : InfantryTypeClass::Array)
	{
		if (!pInf || !CanBuildUnderHumanRules(pHouse, pInf))
			continue;

		const bool isInfInProgress = (pInf->ArrayIndex == inProgressInfIdx);

		if (!isInfInProgress && !IsUnitTierAllowed(pHouse, pInf))
			continue;

		const bool isIncomeRecoveryEng = (pInf->Engineer && neutralCashDerrickCount > ownedEngineers);
		if (!hasActiveUnitIncome && !isInfInProgress && !isIncomeRecoveryEng && pInf->GetActualCost(pHouse) > money)
			continue;

		int score = (pInf->Cost / 2) + pInf->Strength + (isInfInProgress ? 450 : 0);

		// Penalize stacking one infantry type so squads stay mixed
		score -= std::min(600, pHouse->CountOwnedNow(pInf) * 20);

		const auto pWpnStruct = pInf->GetWeapon(0);
		const auto pWpn = pWpnStruct ? pWpnStruct->WeaponType : nullptr;
		const bool isAA = pWpn && pWpn->Projectile && pWpn->Projectile->AA;

		if (pInf->Engineer)
		{
			const int maxActiveEngineers = (neutralCapturableCount >= 2) ? std::min(4, neutralCapturableCount) : 3;
			if (ownedEngineers >= maxActiveEngineers)
				continue;

			const bool moreTechThanEngineers = (ownedEngineers < neutralCapturableCount);
			const int cashDerrickBonus
				= (neutralCashDerrickCount > ownedEngineers) ? 880 : (moreTechThanEngineers ? 680 : 320);
			const int techBonus = hasCapturableTechOnMap ? cashDerrickBonus : 0;

			if (pHouse->OwnedInfantry < 3 && !hasCapturableTechOnMap && scores[36] <= 80)
				score -= 300;
			else
				score += scores[18] + techBonus;
		}
		else if (pInf->Infiltrate)
		{
			const int maxActiveSpies
				= (scores[19] > 180 || pExt->NeuralAI_Archetype == 1 || pExt->NeuralAI_ActiveExpert == 3) ? 3 : 2;
			if (ownedInfiltrators >= maxActiveSpies)
				continue;

			score += (pHouse->OwnedInfantry < 6) ? -400 : scores[19];
		}
		else if (isAA || pInf->C4)
		{
			score += scores[20];
		}
		else if (pInf->Cost >= 250)
		{
			score += scores[17];
		}
		else
		{
			score += scores[16];
		}

		if (needsCheapInfForBioReactor && !pInf->Engineer && !pInf->Infiltrate && !pInf->Doggie && pInf->Cost <= 220)
		{
			score += 520;
		}

		// Train compatible Occupier infantry whenever we own an empty or under-filled Battle Bunker
		for (auto const pOwnB : pHouse->Buildings)
		{
			if (pOwnB && !pOwnB->InLimbo && pOwnB->IsAlive && pOwnB->Type && pOwnB->Type->IsBaseDefense
				&& pOwnB->Type->CanBeOccupied
				&& pOwnB->Occupants.Count < std::clamp(pOwnB->Type->MaxNumberOccupants, 1, 10) && pInf->Occupier
				&& !pInf->Engineer && !pInf->Infiltrate)
			{
				score += 650;
				break;
			}
		}

		if (features[77] > 0 && !pInf->Engineer && !pInf->Infiltrate
			&& (pInf->ImmuneToPsionics || (pWpn && pWpn->Range >= (6 * 256))))
		{
			score += 380; // Counter enemy MindControl with Snipers/Virus/Cyborgs/Dogs!
		}

		if (features[72] > 0 && (pInf->Sensors || pInf->DetectDisguise || pInf->Doggie))
			score += 420;

		if (SlotGate(scores[82], 60, 82) && ownHouseBit != 0u && !pInf->InOwners(ownHouseBit) && !pInf->Engineer)
			score += 220 + std::clamp(static_cast<int>(scores[82]) / 3, 0, 200);

		const bool infCanSelfCrossWater = (pInf->MovementZone == MovementZone::Amphibious
			|| pInf->MovementZone == MovementZone::AmphibiousDestroyer
			|| pInf->MovementZone == MovementZone::AmphibiousCrusher || pInf->MovementZone == MovementZone::Fly
			|| pInf->SpeedType == SpeedType::Hover || pInf->SpeedType == SpeedType::Winged || pInf->JumpJet
			|| pInf->Teleporter);

		if (features[21] > 0 && !pInf->Engineer)
		{
			if (waitingBattalionSizeOnIslands > ownedTransportTotalCapacity + 8)
			{
				if (infCanSelfCrossWater)
					score += 550;
				else
					score -= 750;
			}
			else if (infCanSelfCrossWater && money >= 3000)
			{
				score += 250;
			}
		}

		score += CounterBonusFor(pHouse, pInf);

		if (score > bestInfScore)
		{
			bestInfScore = score;

			bestInfIdx = pInf->ArrayIndex;
		}
	}

	// Dynamic Base-Congestion & High-Money Infantry Scaling:
	// Allow continuous Barracks production when money is abundant and logistics
	// can deploy troops across the map, pausing only if idle infantry congests
	// the home base or island transport queues!
	int congestedBaseInfantry = 0;
	const CellStruct infHomeCenter = (pHouse->ConYards.Count > 0 && pHouse->ConYards.GetItem(0))
		? pHouse->ConYards.GetItem(0)->GetMapCoords()
		: (pHouse->BaseCenter != CellStruct::Empty ? pHouse->BaseCenter : pHouse->BaseSpawnCell);

	if (infHomeCenter != CellStruct::Empty)
	{
		for (auto const pOwnI : InfantryClass::Array)
		{
			if (!pOwnI || pOwnI->Owner != pHouse || pOwnI->InLimbo || !pOwnI->IsAlive)
				continue;

			if (pOwnI->CurrentMission == Mission::Move || pOwnI->CurrentMission == Mission::Attack
				|| pOwnI->CurrentMission == Mission::AttackMove || pOwnI->CurrentMission == Mission::Enter)
			{
				continue;
			}

			const CellStruct ic = pOwnI->GetMapCoords();
			const int dx = static_cast<int>(ic.X) - static_cast<int>(infHomeCenter.X);
			const int dy = static_cast<int>(ic.Y) - static_cast<int>(infHomeCenter.Y);
			if (dx * dx + dy * dy <= 12 * 12)
				congestedBaseInfantry++;
		}
	}

	const bool islandInfantryQueueFull
		= (features[21] > 0 && waitingBattalionSizeOnIslands > ownedTransportTotalCapacity + 12);
	const int dynamicMaxInfantry = (money >= 4000 && congestedBaseInfantry < 20 && !islandInfantryQueueFull)
		? 85
		: ((money >= 2500 && congestedBaseInfantry < 18 && !islandInfantryQueueFull) ? 62 : 45);
	const bool baseInfantrySaturated = (pHouse->OwnedInfantry >= dynamicMaxInfantry) || (congestedBaseInfantry >= 24);

	// Only pause secondary queues to ration finite credits when all income
	// sources are lost (!hasActiveUnitIncome) or when the base is saturated
	if (!hasActiveUnitIncome
			&& (((canRebuildMCVNow && canSafelySaveForMCV && money < rebuildMCVCost) || savingForIslandTransport
					|| ownedEconomyUnits == 0 || trueRefineries == 0)
				&& features[65] < 35)
		|| baseInfantrySaturated)
	{
		if (bestInfIdx < 0 || !InfantryTypeClass::Array.GetItem(bestInfIdx)->Engineer
			|| ownedEngineers >= std::max(1, neutralCashDerrickCount))
		{
			bestInfIdx = -1; // Zero-Income Emergency: ration remaining finite credits to
							// recover Refinery/Harvester/MCV or capture an Oil Derrick!
		}
	}

	if (!hasActiveUnitIncome
		&& ((canRebuildMCVNow && canSafelySaveForMCV && money < rebuildMCVCost) || savingForIslandTransport)
		&& !pHouse->HasLowPower() && pHouse->CountResourceDestinations >= 1 && pHouse->NumWarFactories >= 1
		&& pHouse->NumBarracks >= 1 && (features[21] == 0 || pHouse->NumShipyards >= 1)
		&& pHouse->UnitRepairStations.Count >= 1)
	{
		pExt->NeuralAI_NextBuildingTarget = -1;
	}

	if (!hasActiveUnitIncome && savingForIslandTransport && bestUnitIdx >= 0)
	{
		if (auto const pChosenU = UnitTypeClass::Array.GetItem(bestUnitIdx))
		{
			if (!pChosenU->Harvester && !pChosenU->ResourceGatherer
				&& !(pChosenU->DeploysInto && pChosenU->DeploysInto->ConstructionYard))
			{
				pExt->NeuralAI_NextUnitTarget = -1;
			}
		}
	}

	pExt->NeuralAI_NextInfantryTarget = bestInfIdx;

	// Ares replaces HouseClass::AI_Infantry from its first instruction (and only picks a type while
	// the index is -1), so the AI_Infantry hook of UpdateInfantryProduction never runs with Ares:
	// hand the choice to the barracks here, or no infantry (engineers, spies) is ever trained.
	pHouse->ProducingInfantryTypeIndex = bestInfIdx;

	productionClock.Mark(NeuralSection::ProdAircraft);

	// Aircraft Target Selection
	int bestAirIdx = -1;
	int bestAirScore = -32000;

	if (pHouse->NumAirpads > 0)
	{
		int ownedDockingAircraft = 0;
		for (auto const pOwnAir : AircraftClass::Array)
		{
			if (pOwnAir && pOwnAir->Owner == pHouse && !pOwnAir->InLimbo && pOwnAir->IsAlive && pOwnAir->Type
				&& !pOwnAir->SpawnOwner && (pOwnAir->Type->AirportBound || pOwnAir->Type->Dock.Count > 0))
			{
				ownedDockingAircraft++;
			}
		}

		int inProgressAirIdx = -1;
		if (FactoryClass* pAirFac = pHouse->GetPrimaryFactory(AbstractType::AircraftType, false, BuildCat::DontCare))
		{
			if (pAirFac->Object && pAirFac->Object->GetType())
				inProgressAirIdx = pAirFac->Object->GetType()->GetArrayIndex();
		}

		for (auto const pAir : AircraftTypeClass::Array)
		{
			if (!pAir || !CanBuildUnderHumanRules(pHouse, pAir))
				continue;

			const bool isAirInProgress = (pAir->ArrayIndex == inProgressAirIdx);

			if (!isAirInProgress && !IsUnitTierAllowed(pHouse, pAir))
				continue;

			if (!hasActiveUnitIncome && !isAirInProgress && !pAir->Carryall && pAir->GetActualCost(pHouse) > money)
				continue;

			const int slotScore = (pAir->Strength >= 300 || pAir->Cost >= 1500) ? scores[33] : scores[32];

			const int crossFactionAirBonus = (SlotGate(scores[82], 60, 82) && ownHouseBit != 0u && !pAir->InOwners(ownHouseBit))
				? (220 + std::clamp(static_cast<int>(scores[82]) / 3, 0, 200))
				: 0;

			// Air transports are built from their real demand, never as a fixed fleet: a type
			// the demand rejects is not a candidate (see the naval queue note).
			int airTransBonus = 0;
			if (pAir->Carryall)
			{
				// Airlift jobs (damaged vehicle rescue, loaded strike APC drops). An idle
				// carryall already covers the next job.
				if (!isAirInProgress && (ownedCarryallAircraft >= wantedCarryalls || idleCarryallAircraft > 0))
					continue;

				airTransBonus = std::max(static_cast<int>(scores[28]), 0) + (carryallLiftJobs > 0 ? 700 : 350);
			}
			else if (pAir->Passengers >= 3)
			{
				// Passenger aircraft shuttle across water like amphibious transports, under
				// the same ferry demand (aircraft and ground transports counted together).
				const int ferryDelta = scoreFerryTransport(pAir);

				if (ferryDelta <= -900 && !isAirInProgress)
					continue;

				airTransBonus = std::max(ferryDelta, 0);
			}

			const int nonDockBoundBonus = (ownedDockingAircraft >= pHouse->AirportDocks
				&& !pAir->AirportBound && pAir->Dock.Count == 0)
				? 650
				: 0;

			// Air-to-air interceptors answer an uncovered enemy air threat.
			const auto& airMacro = GetMacroState(pHouse);
			const int airDeficit = std::max(0, airMacro.AirThreatValue - airMacro.OwnAACapacity);
			const int interceptorBonus = (airDeficit > 0 && GetCombatValueOfType(pAir, true) > 0)
				? std::clamp(airDeficit * 4, 0, 800)
				: 0;

			const int score = (pAir->Cost / 3) + (pAir->Strength / 2) + slotScore + crossFactionAirBonus + airTransBonus
				+ nonDockBoundBonus + interceptorBonus + (isAirInProgress ? 850 : 0) + CounterBonusFor(pHouse, pAir);

			if (score > bestAirScore)
			{
				bestAirScore = score;

				bestAirIdx = pAir->ArrayIndex;
			}
		}
	}

	if (!hasActiveUnitIncome
		&& ((canRebuildMCVNow && canSafelySaveForMCV && money < rebuildMCVCost) || ownedEconomyUnits == 0
			|| trueRefineries == 0))
	{
		bestAirIdx = -1; // Zero-Income Emergency: save remaining finite credits to
						// restore economy/MCV first!
	}

	pExt->NeuralAI_NextAircraftTarget = bestAirIdx;

	if (IsAITickWindow(450, evalDelay))
	{
		const int bldIdx = pExt->NeuralAI_NextBuildingTarget;

		const char* bldName = (bldIdx >= 0 && bldIdx < BuildingTypeClass::Array.Count)
			? BuildingTypeClass::Array.GetItem(bldIdx)->ID
			: "None";

		const char* unitName = (bestUnitIdx >= 0 && bestUnitIdx < UnitTypeClass::Array.Count)
			? UnitTypeClass::Array.GetItem(bestUnitIdx)->ID
			: "None";

		const char* infName = (bestInfIdx >= 0 && bestInfIdx < InfantryTypeClass::Array.Count)
			? InfantryTypeClass::Array.GetItem(bestInfIdx)->ID
			: "None";

		const char* navalName = (bestNavalIdx >= 0 && bestNavalIdx < UnitTypeClass::Array.Count)
			? UnitTypeClass::Array.GetItem(bestNavalIdx)->ID
			: "None";
		const char* airName = (bestAirIdx >= 0 && bestAirIdx < AircraftTypeClass::Array.Count)
			? AircraftTypeClass::Array.GetItem(bestAirIdx)->ID
			: "None";

		Debug::Log("[NeuralAI] Frame %d | House #%d (%s) | Expert=%d Arch=%d | Credits=%d "
				"| NextBld=%s | NextUnit=%s | NextNaval=%s | NextInf=%s | NextAir=%s\n",
			Unsorted::CurrentFrame, pHouse->ArrayIndex, (pHouse->Type && pHouse->Type->ID[0]) ? pHouse->Type->ID : "AI",
			pExt->NeuralAI_ActiveExpert, pExt->NeuralAI_Archetype, money, bldName, unitName, navalName, infName, airName);
	}

	return evalDelay;
}

void NeuralAI::UpdateBuildingProduction(HouseClass* pHouse)
{
	auto const pExt = HouseExt::Fetch(pHouse);

	if (Unsorted::CurrentFrame - pExt->NeuralAI_LastInferenceFrame > 60)
		NeuralAI::UpdateExpertAI(pHouse);

	pHouse->ProducingBuildingTypeIndex = pExt->NeuralAI_NextBuildingTarget;
}

void NeuralAI::UpdateUnitProduction(HouseClass* pHouse)
{
	auto const pExt = HouseExt::Fetch(pHouse);

	if (Unsorted::CurrentFrame - pExt->NeuralAI_LastInferenceFrame > 60)
		NeuralAI::UpdateExpertAI(pHouse);

	pHouse->ProducingUnitTypeIndex = pExt->NeuralAI_NextUnitTarget;

	pExt->ProducingNavalUnitTypeIndex = pExt->NeuralAI_NextNavalTarget;
}

void NeuralAI::UpdateInfantryProduction(HouseClass* pHouse)
{
	auto const pExt = HouseExt::Fetch(pHouse);

	if (Unsorted::CurrentFrame - pExt->NeuralAI_LastInferenceFrame > 60)
		NeuralAI::UpdateExpertAI(pHouse);

	pHouse->ProducingInfantryTypeIndex = pExt->NeuralAI_NextInfantryTarget;
}

void NeuralAI::UpdateAircraftProduction(HouseClass* pHouse)
{
	auto const pExt = HouseExt::Fetch(pHouse);

	if (Unsorted::CurrentFrame - pExt->NeuralAI_LastInferenceFrame > 60)
		NeuralAI::UpdateExpertAI(pHouse);

	pHouse->ProducingAircraftTypeIndex = pExt->NeuralAI_NextAircraftTarget;
}


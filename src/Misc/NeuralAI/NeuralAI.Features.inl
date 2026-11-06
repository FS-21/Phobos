// NeuralAI.Features.inl: Fog-filtered feature extraction.
// Part of NeuralAI.cpp (included in order; one translation unit).

void NeuralAI::ExtractFogFilteredFeatures(HouseClass* pHouse, std::array<int8_t, InputVectorSize>& features)
{
	features.fill(0);

	auto const pExt = HouseExt::Fetch(pHouse);

	// 0..31: Own Economy, Power & Infrastructure
	const int credits = pHouse->Available_Money();

	features[0] = ClampInt8(credits / 250);

	features[1] = ClampInt8(pHouse->CountResourceGatherers * 16);

	features[2] = ClampInt8(pHouse->CountResourceDestinations * 20);

	features[3] = ClampInt8((pHouse->PowerOutput - pHouse->PowerDrain) / 10);

	features[4] = pHouse->HasLowPower() ? -100 : 60;

	features[5] = ClampInt8(pHouse->NumConYards * 32);

	features[6] = ClampInt8(pHouse->NumBarracks * 24);

	features[7] = ClampInt8(pHouse->NumWarFactories * 24);

	features[8] = ClampInt8(pHouse->NumShipyards * 24);

	features[9] = ClampInt8(pHouse->NumAirpads * 24);

	features[10] = ClampInt8(pHouse->NumOrePurifiers * 40);

	features[11] = ClampInt8(pHouse->UnitRepairStations.Count * 40);

	features[12] = ClampInt8(pHouse->CloningVats.Count * 40);

	features[13] = pHouse->SpySatActive ? 100 : 0;

	features[14] = pHouse->IsTiberiumShort ? -80 : 40;

	features[15] = ClampInt8(Unsorted::CurrentFrame / 450);

	// 32..63: Own Force Composition
	features[32] = ClampInt8(pHouse->OwnedInfantry * 4);

	features[33] = ClampInt8(pHouse->OwnedUnits * 6);

	features[34] = ClampInt8(pHouse->OwnedNavy * 8);

	features[35] = ClampInt8(pHouse->OwnedAircraft * 10);

	features[36] = ClampInt8(pHouse->OwnedBuildings * 4);

	features[37] = ClampInt8(pHouse->SideIndex * 32);

	features[38] = ClampInt8(pExt->NeuralAI_Archetype * 16);

	// 64..95: Strict Fog-of-War Filtered Enemy Intelligence
	// We ONLY inspect enemy objects that are on unshrouded cells, or houses
	// infiltrated by our Spies.
	int visibleEnemyInfantry = 0;
	int visibleEnemyTanks = 0;
	int visibleEnemyAir = 0;
	int visibleEnemyNavy = 0;
	int visibleEnemyDefenses = 0;
	int visibleEnemyBuildings = 0;
	int visibleEnemiesOnBridge = 0;
	int ownParasitedOrControlled = 0;
	int ownEliteUnits = 0;
	int visibleEnemyAirShips = 0;
	int visibleEnemyStealthUnits = 0;
	int visibleEnemyGarrisons = 0;
	int visibleEnemyElites = 0;
	int visibleEnemyArtillery = 0;
	int visibleEnemyMindControllers = 0;
	int visibleEnemyHarvesters = 0;
	int neutralCapturableTechs = 0;
	int emptyUrbanOccupiables = 0;
	int damagedUrbanGarrisons = 0;
	int ownHeavyAndSiegeCount = 0;
	int ownDisguiseAndDetectorUnits = 0;
	int enemySlaveMinersAndOwnOverpowerCoils = 0;
	int ownRadDeployersAndGapStealthUnits = 0;
	int parasiteWarfareScore = 0;
	int grinderAndMindCapturedUnits = 0;
	int empUnitsAndEMPedEnemies = 0;
	int tankBunkersAndAmmoReloadUnits = 0;
	int sniperPoisonChaosAndTiberiumHealers = 0;
	int submarinesAndAntiSubDestroyers = 0;
	int dyingLoadedTransportsCount = 0;
	int ownTemporaledOrIvanBombedCount = 0;
	int occupiedTankBunkersAndDrainedBlds = 0;
	int amphibiousFleetAndBeachheadScore = 0;
	int woundedFrontlineAndFocusTargets = 0;
	int enemyExposedPowerAndFactories = 0;
	int foreignConYardAndCaptureScore = 0;
	int camouflageAmbushAndGroveScore = 0;
	int ownDamagedBuildings = 0;
	int ownDamagedVehicles = 0;
	int ownHarvestersUnderAttack = 0;
	const DWORD ownCountryBit = (pHouse->Type ? (1u << pHouse->Type->ArrayIndex2) : 0);

	const BuildingTypeClass* pPrimaryConYardType
		= (pHouse->ConYards.Count > 0 && pHouse->ConYards.GetItem(0)) ? pHouse->ConYards.GetItem(0)->Type : nullptr;
	CellStruct earlyHomeCoords = pHouse->BaseCenter != CellStruct::Empty ? pHouse->BaseCenter : pHouse->BaseSpawnCell;
	if (pHouse->ConYards.Count > 0 && pHouse->ConYards.GetItem(0))
		earlyHomeCoords = pHouse->ConYards.GetItem(0)->GetMapCoords();

	const int earlyHomeZone = (earlyHomeCoords != CellStruct::Empty)
		? MapClass::Instance.GetMovementZoneType(earlyHomeCoords, MovementZone::Normal, false)
		: -1;
	int islandCapturableTechs = 0;

	for (auto const pBld : BuildingClass::Array)
	{
		if (!pBld || pBld->InLimbo || !pBld->IsAlive || !pBld->Type || pBld->Type->Insignificant
			|| pBld->Type->InvisibleInGame)
		{
			continue;
		}

		// Neutral techs count once their cell has been seen (fog-fair).
		if (pBld->Owner && pBld->Owner->IsNeutral() && pBld->Type->Capturable && IsCellVisibleToAI(pHouse, pBld->GetCell()))
		{
			neutralCapturableTechs++;
			if (earlyHomeZone >= 0)
			{
				const int bz
					= ObjectZone(pBld, MovementZone::Normal);
				if (bz >= 0 && bz != earlyHomeZone)
				{
					islandCapturableTechs
						+= ((pBld->Type->ProduceCashAmount > 0 || pBld->Type->ProduceCashStartup > 0) ? 2 : 1);
				}
			}
		}

		if (pBld->Type->CanBeOccupied)
		{
			if (pBld->Occupants.Count == 0)
			{
				emptyUrbanOccupiables++;

				if (pBld->Type->Repairable && pBld->GetHealthPercentage() < 0.72)
					damagedUrbanGarrisons++;
			}
			else if (pBld->Occupants.GetItem(0) && pHouse->IsAlliedWith(pBld->Occupants.GetItem(0)->Owner)
				&& pBld->Type->Repairable && pBld->GetHealthPercentage() < 0.75)
			{
				damagedUrbanGarrisons++;
			}
		}

		if (pBld->Owner == pHouse)
		{
			if (pBld->GetHealthPercentage() < 0.75)
				ownDamagedBuildings++;

			if (pBld->Type->Overpowerable && (!pBld->IsOverpowered || pHouse->HasLowPower()))
				enemySlaveMinersAndOwnOverpowerCoils++;

			if (pBld->Type->Grinding)
				grinderAndMindCapturedUnits += 2;

			if (pBld->Type->Bunker)
			{
				if (pBld->BunkerLinkedItem == nullptr)
					tankBunkersAndAmmoReloadUnits += 2;
				else
					occupiedTankBunkersAndDrainedBlds++;
			}

			if (pBld->DrainingMe != nullptr)
				occupiedTankBunkersAndDrainedBlds += 3;

			if (pBld->TemporalTargetingMe != nullptr || pBld->AttachedBomb != nullptr)
				ownTemporaledOrIvanBombedCount += 3;

			if ((pBld->Type->ConstructionYard || pBld->Type->Factory != AbstractType::None)
				&& ((pPrimaryConYardType && pBld->Type->ConstructionYard && pBld->Type != pPrimaryConYardType)
					|| (ownCountryBit != 0 && !pBld->Type->InOwners(ownCountryBit))))
			{
				foreignConYardAndCaptureScore += 3;
			}

			if (IsTunnelNetworkBuilding(pBld->Type))
			{
				amphibiousFleetAndBeachheadScore += 3;

				foreignConYardAndCaptureScore += 2;

				camouflageAmbushAndGroveScore += 3;
			}
		}
	}

	int ownReadySupers = 0;
	int enemyImminentSuperUrgency = 0;

	for (auto const pOtherHouse : HouseClass::Array)
	{
		if (!pOtherHouse || pOtherHouse->IsNeutral() || pOtherHouse->IsObserver() || pOtherHouse->Defeated)
			continue;

		const bool isEnemyH = (pOtherHouse != pHouse && !pHouse->IsAlliedWith(pOtherHouse));

		for (auto const pSuper : pOtherHouse->Supers)
		{
			if (!pSuper || !pSuper->Type || !pSuper->Type->DisableableFromShell)
				continue;

			if (pOtherHouse == pHouse && pSuper->IsReady)
			{
				ownReadySupers++;
			}
			else if (isEnemyH && pSuper->IsPresent)
			{
				const int left = pSuper->RechargeTimer.GetTimeLeft();

				if (pSuper->IsReady || (left > 0 && left <= 900))
					enemyImminentSuperUrgency = std::max(enemyImminentSuperUrgency, 110);
				else if (left > 0 && left <= 2700)
					enemyImminentSuperUrgency = std::max(enemyImminentSuperUrgency, 65);
				else
					enemyImminentSuperUrgency = std::max(enemyImminentSuperUrgency, 30);
			}
		}
	}

	pExt->NeuralAI_EstimatedEnemyThreat.fill(0);

	CellStruct homeCoords = pHouse->BaseCenter != CellStruct::Empty ? pHouse->BaseCenter : pHouse->BaseSpawnCell;

	if (pHouse->ConYards.Count > 0 && pHouse->ConYards.GetItem(0))
		homeCoords = pHouse->ConYards.GetItem(0)->GetMapCoords();

	const int homeGroundZone = (homeCoords != CellStruct::Empty)
		? MapClass::Instance.GetMovementZoneType(homeCoords, MovementZone::Normal, false)
		: -1;

	int islandEnemyBuildings = 0;

	for (auto const pTechno : TechnoClass::Array)
	{
		if (!pTechno || pTechno->InLimbo || !pTechno->IsAlive || !pTechno->Owner)
			continue;

		auto const pOwner = pTechno->Owner;

		if (pOwner == pHouse)
		{
			if (pTechno->Veterancy.IsElite())
				ownEliteUnits++;

			if (pTechno->MindControlledBy != nullptr)
				ownParasitedOrControlled++;

			if (auto const pFoot = abstract_cast<FootClass*>(pTechno))
			{
				if (pFoot->ParasiteEatingMe != nullptr)
					ownParasitedOrControlled++;
			}

			if (auto const pOwnU = abstract_cast<UnitClass*>(pTechno))
			{
				if (pOwnU->Type)
				{
					if ((pOwnU->Type->Harvester || pOwnU->Type->ResourceGatherer)
						&& pOwnU->GetHealthPercentage() < 0.85)
					{
						ownHarvestersUnderAttack++;
					}
					else if (pOwnU->Type->Cost >= 700 && pOwnU->GetHealthPercentage() < 0.75)
					{
						ownDamagedVehicles++;
					}

					if (!pOwnU->Type->Harvester && !pOwnU->Type->ResourceGatherer)
					{
						const auto pOWS = pOwnU->Type->GetWeapon(0);
						const auto pOW = pOWS ? pOWS->WeaponType : nullptr;

						if (pOwnU->Type->Cost >= 1000 || (pOW && (pOW->Range >= (8 * 256) || pOW->Spawner)))
							ownHeavyAndSiegeCount++;
					}

					if (pOwnU->Type->GapGenerator)
						ownRadDeployersAndGapStealthUnits++;

					if (pOwnU->Type->Passengers >= 4)
						amphibiousFleetAndBeachheadScore++;

					if (pOwnU->Type->DeploysInto && pOwnU->Type->DeploysInto->ConstructionYard && pPrimaryConYardType
						&& pOwnU->Type->DeploysInto != pPrimaryConYardType)
					{
						foreignConYardAndCaptureScore += 4;
					}

					if (!pOwnU->Type->Naval && !pOwnU->Type->Harvester && !pOwnU->Type->ResourceGatherer
						&& (pOwnU->Type->CanDisguise || pOwnU->Type->PermaDisguise || pOwnU->Type->Cloakable))
					{
						camouflageAmbushAndGroveScore += 3;
					}
				}
			}

			if (pTechno->MindControlledBy != nullptr)
				grinderAndMindCapturedUnits += 2;

			if (pTechno->TemporalTargetingMe != nullptr || pTechno->AttachedBomb != nullptr)
				ownTemporaledOrIvanBombedCount += 3;

			if (pTechno->Passengers.NumPassengers > 0 && pTechno->GetHealthPercentage() < 0.35)
				dyingLoadedTransportsCount += 3;

			if (pTechno->Target != nullptr && pTechno->GetHealthPercentage() < 0.45)
				woundedFrontlineAndFocusTargets++;

			if (auto const pOwnTType = pTechno->GetTechnoType())
			{
				if (pOwnTType->CanDisguise || pOwnTType->DetectDisguise)
					ownDisguiseAndDetectorUnits++;

				if (pOwnTType->ManualReload && pOwnTType->Ammo > 0 && pTechno->Ammo == 0)
					tankBunkersAndAmmoReloadUnits += 2;

				if (pOwnTType->TiberiumHeal && pTechno->GetHealthPercentage() < 0.65)
					sniperPoisonChaosAndTiberiumHealers += 2;

				if (pOwnTType->Naval && (pOwnTType->Cloakable || pOwnTType->Sensors))
					submarinesAndAntiSubDestroyers += 2;

				const auto pOwnWS = pOwnTType->GetWeapon(0);
				const auto pOwnW = pOwnWS ? pOwnWS->WeaponType : nullptr;

				if (pOwnW && pOwnW->Warhead)
				{
					if (pOwnW->Warhead->Parasite || pOwnW->Warhead->Sonic || pOwnW->Damage < 0)
						parasiteWarfareScore++;

					if (pOwnW->Warhead->EMEffect)
						empUnitsAndEMPedEnemies += 2;

					if (pOwnW->Warhead->Poison || pOwnW->Warhead->Psychedelic)
						sniperPoisonChaosAndTiberiumHealers += 2;
				}

				if (auto const pOwnIType = abstract_cast<InfantryTypeClass*>(pOwnTType))
				{
					if (pOwnIType->Deployer && pOwnIType->ImmuneToRadiation)
						ownRadDeployersAndGapStealthUnits++;
				}
			}

			continue;
		}

		if (pHouse->IsAlliedWith(pOwner) || pOwner->IsNeutral() || pOwner->IsObserver())
			continue;

		const CellStruct cellCoords = pTechno->GetMapCoords();
		auto const pCell = MapClass::Instance.TryGetCellAt(cellCoords);

		if (!pCell)
			continue;

		const int ownerIdx = std::clamp(pOwner->ArrayIndex, 0, 7);
		const bool spyInfiltrated = (pExt->NeuralAI_InfiltratedEnemyMask & (1 << ownerIdx)) != 0;

		const bool cellVisibleToAI = pHouse->SpySatActive || IsCellVisibleToAI(pHouse, pCell) || spyInfiltrated;

		if (!cellVisibleToAI)
		{
			if (auto const pHiddenTType = pTechno->GetTechnoType())
			{
				const bool isHiddenThreat = (pTechno->CloakState != CloakState::Uncloaked || pTechno->Disguised
					|| pHiddenTType->Cloakable || pHiddenTType->CanDisguise || pHiddenTType->PermaDisguise);

				if (isHiddenThreat
					&& (pOwner->OwnedBuildings == 0
						|| (Unsorted::CurrentFrame > 5400
							&& (Unsorted::CurrentFrame - NeuralAI::LastCombatFrame) >= 3600)))
				{
					visibleEnemyStealthUnits += 2;
				}
			}

			continue;
		}

		pExt->NeuralAI_ScoutedEnemyMask |= (1 << ownerIdx);

		// Aircraft plus JumpJet / Fly vehicles and infantry (Rocketeers, Siege Choppers...).
		RecordEnemySighting(pHouse, pTechno);

		if (IsAirborneThreatTechno(pTechno))
			RecordAirThreatSighting(pHouse, pTechno);
		else if (pTechno->WhatAmI() == AbstractType::Building
			&& (pHouse->SpySatActive || IsCellVisibleToAI(pHouse, pCell)))
		{
			// Only real sight: "WhoLastHurtMe" visibility must not leak a whole base.
			GetMacroState(pHouse).SeenEnemyBuildings.insert(pTechno->UniqueID);
		}

		if (pTechno->OnBridge && pCell->ContainsBridge())
			visibleEnemiesOnBridge++;

		if (pTechno->Veterancy.IsElite())
			visibleEnemyElites++;

		if (auto const pTType = pTechno->GetTechnoType())
		{
			if (pTType->Cloakable || pTechno->CloakState != CloakState::Uncloaked || pTechno->Disguised
				|| pTType->CanDisguise || pTType->PermaDisguise)
			{
				visibleEnemyStealthUnits++;
			}

			const auto pWS = pTType->GetWeapon(0);
			const auto pW = pWS ? pWS->WeaponType : nullptr;

			if (pW && pW->Range >= (8 * 256) && pTechno->WhatAmI() != AbstractType::Building)
				visibleEnemyArtillery++;

			if (pW && pW->Warhead && pW->Warhead->MindControl)
				visibleEnemyMindControllers++;

			if (pTechno->Disguised)
				ownDisguiseAndDetectorUnits += 2;

			if (pTType->Enslaves != nullptr)
				enemySlaveMinersAndOwnOverpowerCoils += 2;

			if (pTechno->IsUnderEMP())
				empUnitsAndEMPedEnemies += 2;

			if (auto const pEIType = abstract_cast<InfantryTypeClass*>(pTType))
			{
				if (pEIType->Engineer || IsSpyInfantryType(pEIType) || pEIType->C4 || pEIType->Ivan || pEIType->Deployer)
					sniperPoisonChaosAndTiberiumHealers++;
			}

			if (auto const pETUnit = abstract_cast<UnitTypeClass*>(pTType))
			{
				if (pETUnit->Harvester || pETUnit->ResourceGatherer)
					visibleEnemyHarvesters++;
			}
		}

		switch (pTechno->WhatAmI())
		{
		case AbstractType::Infantry:
			visibleEnemyInfantry++;

			pExt->NeuralAI_EstimatedEnemyThreat[ownerIdx] += 10;

			break;

		case AbstractType::Unit:
			if (pTechno->GetTechnoType()->Naval)
				visibleEnemyNavy++;
			else
				visibleEnemyTanks++;

			pExt->NeuralAI_EstimatedEnemyThreat[ownerIdx] += 25;

			break;

		case AbstractType::Aircraft:
			visibleEnemyAir++;

			if (pTechno->GetTechnoType() && pTechno->GetTechnoType()->Strength >= 600)
				visibleEnemyAirShips++; // Kirov Airship / Floating Disc / Heavy Bomber

			// alert!
			pExt->NeuralAI_EstimatedEnemyThreat[ownerIdx] += 30;

			break;

		case AbstractType::Building:
		{
			auto const pBld = static_cast<BuildingClass*>(pTechno);
			auto const pBldType = pBld->Type;
			if (pBldType && (pBldType->Immune || pBldType->Insignificant || pBldType->InvisibleInGame))
				break;

			visibleEnemyBuildings++;

			if (pBld->Occupants.Count > 0)
				visibleEnemyGarrisons++;

			if (pBldType && pBldType->IsBaseDefense)
				visibleEnemyDefenses++;

			if (pBldType
				&& (pBldType->PowerBonus > 0 || pBldType->SuperWeapon != -1 || pBldType->Factory != AbstractType::None
					|| pBldType->ConstructionYard))
			{
				enemyExposedPowerAndFactories++;
			}

			if (pBldType && pBldType->Capturable
				&& (pBldType->ConstructionYard || pBldType->Factory != AbstractType::None || pBldType->TechLevel >= 6))
			{
				foreignConYardAndCaptureScore += (pBldType->ConstructionYard ? 3 : 1);
			}

			if (homeGroundZone >= 0)
			{
				const int enemyZone = MapClass::Instance.GetMovementZoneType(cellCoords, MovementZone::Normal, pTechno->OnBridge);

				if (enemyZone >= 0 && enemyZone != homeGroundZone)
					islandEnemyBuildings++;
			}

			pExt->NeuralAI_EstimatedEnemyThreat[ownerIdx] += 20;

			break;
		}

		default:
			break;
		}
	}

	features[64] = ClampInt8(visibleEnemyInfantry * 4);

	features[65] = ClampInt8(visibleEnemyTanks * 6);

	// Air threat weighted by cost/durability/firepower and remembered for a while
	// after the fog hides it: one Kirov already counts as a serious air threat.
	RefreshAirThreatSummary(pHouse);
	RefreshOpponentModel(pHouse);
	const auto& airMacro = GetMacroState(pHouse);
	visibleEnemyAirShips = std::max(visibleEnemyAirShips, airMacro.HeavyAirThreats);

	features[66] = ClampInt8(std::max(visibleEnemyAir * 12, airMacro.AirThreatValue / 8));

	features[67] = ClampInt8(visibleEnemyNavy * 10);

	features[68] = ClampInt8(visibleEnemyDefenses * 8);

	features[69] = ClampInt8(visibleEnemyBuildings * 4);

	features[70] = ClampInt8(std::max(visibleEnemiesOnBridge, GetBridgeState(pHouse).AmbushCount) * 35);

	features[71] = ClampInt8(visibleEnemyAirShips * 45);

	features[72] = ClampInt8(visibleEnemyStealthUnits * 30);

	features[73] = ClampInt8(visibleEnemyGarrisons * 25);

	features[74] = ClampInt8(visibleEnemyElites * 25);

	features[75] = ClampInt8(enemyImminentSuperUrgency);

	features[76] = ClampInt8(visibleEnemyArtillery * 25);

	features[77] = ClampInt8(visibleEnemyMindControllers * 35);

	features[78] = ClampInt8(visibleEnemyHarvesters * 25);

	features[79] = ClampInt8(damagedUrbanGarrisons * 30);

	features[80] = ClampInt8(ownHeavyAndSiegeCount * 15);

	features[81] = ClampInt8(ownDisguiseAndDetectorUnits * 20);

	features[82] = ClampInt8(enemySlaveMinersAndOwnOverpowerCoils * 25);

	features[83] = ClampInt8(ownRadDeployersAndGapStealthUnits * 25);

	features[84] = ClampInt8((parasiteWarfareScore + ownParasitedOrControlled * 2) * 20);

	features[85] = ClampInt8(grinderAndMindCapturedUnits * 25);

	features[86] = ClampInt8(empUnitsAndEMPedEnemies * 25);

	features[87] = ClampInt8(tankBunkersAndAmmoReloadUnits * 25);

	features[88] = ClampInt8(sniperPoisonChaosAndTiberiumHealers * 20);

	features[89] = ClampInt8((submarinesAndAntiSubDestroyers + visibleEnemyNavy) * 18);

	features[90] = ClampInt8(dyingLoadedTransportsCount * 35);

	features[91] = ClampInt8(ownTemporaledOrIvanBombedCount * 35);

	features[92] = ClampInt8(occupiedTankBunkersAndDrainedBlds * 30);

	features[93] = ClampInt8((amphibiousFleetAndBeachheadScore * 2 + islandEnemyBuildings * 3 + visibleEnemyNavy) * 15);

	features[94] = ClampInt8((woundedFrontlineAndFocusTargets + visibleEnemyTanks) * 12);

	features[95] = ClampInt8((enemyExposedPowerAndFactories + visibleEnemyArtillery * 2) * 18);

	auto const& bridgeState = GetBridgeState(pHouse);

	// Bridges this house has seen broken (fog-fair: a bridge broken in the fog still reads as standing).
	features[18] = ClampInt8(bridgeState.KnownBroken * 30);

	features[22] = ClampInt8(ownParasitedOrControlled * 40);

	features[23] = ClampInt8(ownEliteUnits * 20);

	features[24] = ClampInt8((pHouse->OwnedAircraft - (pHouse->NumAirpads * 4)) * 20);

	features[25] = ClampInt8(neutralCapturableTechs * 25);

	features[26] = ClampInt8(emptyUrbanOccupiables * 8);

	features[28] = ClampInt8(ownReadySupers * 60);

	features[29] = ClampInt8(ownDamagedBuildings * 25);

	features[30] = ClampInt8(ownDamagedVehicles * 25);

	features[31] = ClampInt8(ownHarvestersUnderAttack * 60);

	features[16] = GameModeOptionsClass::Instance.MCVRedeploy
		? ClampInt8((pHouse->NumConYards * 25) + (features[20] > 0 ? 40 : 15))
		: 0;

	features[17] = ClampInt8(foreignConYardAndCaptureScore * 20);

	// 96..127: Map Spatial Congestion & Allied Beacons
	int alliedBeaconActive = 0;

	for (auto const pAlly : HouseClass::Array)
	{
		if (!pAlly || pAlly == pHouse || !pHouse->IsAlliedWith(pAlly))
			continue;

		const int allyIdx = pAlly->ArrayIndex;

		if (allyIdx >= 0 && allyIdx < 8)
		{
			for (int b = 0; b < 3; ++b)
			{
				if (auto const pBeacon = BeaconManagerClass::Instance.Beacons[allyIdx][b])
				{
					alliedBeaconActive = 100;

					pExt->NeuralAI_AttackTargetCell = CellClass::Coord2Cell(pBeacon->Coord);

					break;
				}
			}
		}
	}

	features[96] = ClampInt8(alliedBeaconActive);

	features[97] = ClampInt8(pHouse->Occupiables.Count * 16);

	// Scan local vs. uncolonized distant Ore/Tiberium patches across all owned
	// ConYards for Expert 6 (Multi-Outpost Expansion)
	CellStruct baseCenter = pHouse->BaseCenter != CellStruct::Empty ? pHouse->BaseCenter : pHouse->BaseSpawnCell;

	if (pHouse->ConYards.Count > 0)
	{
		baseCenter =

			pHouse->ConYards.GetItem(pHouse->ConYards.Count - 1)->GetMapCoords();
	}

	int localOreCells = 0;
	int expansionOreCells = 0;
	int islandExpansionOreCells = 0;
	std::vector<int> expansionFieldBlocks;
	int visibleCratesNearby = 0;
	int nearbyTreeGroveCells = 0;
	int uncolonizedIslandLandCells = 0;
	struct ScanHub
	{
		CellStruct Cell;
		int Zone;
		bool IsConYard;
	};

	std::vector<ScanHub> ownScanHubs;

	for (auto const pOwnB : pHouse->Buildings)
	{
		if (!pOwnB || pOwnB->InLimbo || !pOwnB->IsAlive || !pOwnB->Type || pOwnB->Type->Insignificant || pOwnB->Type->InvisibleInGame)
			continue;

		if (!pOwnB->Type->ConstructionYard && !pOwnB->Type->Refinery && !pOwnB->Type->ResourceDestination && !pOwnB->Type->BaseNormal)
			continue;

		const CellStruct yc = pOwnB->GetMapCoords();
		ownScanHubs.push_back({ yc, MapClass::Instance.GetMovementZoneType(yc, MovementZone::Normal, pOwnB->OnBridge),
			pOwnB->Type->ConstructionYard });
	}

	if (baseCenter != CellStruct::Empty)
	{
		for (int dy = -144; dy <= 144; dy += (std::abs(dy) <= 48 ? 3 : (std::abs(dy) <= 96 ? 6 : 8)))
		{
			for (int dx = -144; dx <= 144; dx += (std::abs(dx) <= 48 ? 3 : (std::abs(dx) <= 96 ? 6 : 8)))
			{
				const CellStruct tc { static_cast<short>(baseCenter.X + dx), static_cast<short>(baseCenter.Y + dy) };
				auto const pCell = MapClass::Instance.TryGetCellAt(tc);
				if (!pCell)
					continue;

				const bool withinNearScan = (std::abs(dx) <= 48 && std::abs(dy) <= 48);
				if (withinNearScan && pCell->GetTerrain(false) != nullptr)
					nearbyTreeGroveCells++;

				if (withinNearScan && pCell->OverlayTypeIndex >= 0
					&& pCell->OverlayTypeIndex < OverlayTypeClass::Array.Count && IsCellVisibleToAI(pHouse, pCell))
				{
					if (auto const pOvl = OverlayTypeClass::Array.GetItem(pCell->OverlayTypeIndex))
					{
						if (pOvl->Crate)
							visibleCratesNearby++;
					}
				}

				if (pCell->LandType == LandType::Water || pCell->LandType == LandType::Rock)
					continue;

				const int cellZone = MapClass::Instance.GetMovementZoneType(tc, MovementZone::Normal, false);
				int minSameZoneYardDist = 9999;
				bool hasConYardInZone = false;

				// Own hubs gathered once before the scan (it walked every building for every sampled
				// cell: the heaviest part of the feature extraction).
				for (auto const& hub : ownScanHubs)
				{
					if (cellZone >= 0 && hub.Zone != cellZone)
						continue;

					if (hub.IsConYard)
						hasConYardInZone = true;

					const int d = std::max(std::abs(tc.X - hub.Cell.X), std::abs(tc.Y - hub.Cell.Y));
					if (d < minSameZoneYardDist)
						minSameZoneYardDist = d;
				}

				if (homeGroundZone >= 0 && cellZone >= 0 && cellZone != homeGroundZone && !hasConYardInZone
					&& minSameZoneYardDist > 24 && pCell->GetBuilding() == nullptr)
				{
					uncolonizedIslandLandCells++;
				}

				if (pCell->GetContainedTiberiumValue() <= 0)
					continue;

				if (minSameZoneYardDist <= 14)
				{
					localOreCells++;
				}
				else if (minSameZoneYardDist > 14)
				{
					expansionOreCells++;
					expansionFieldBlocks.push_back((tc.X / 12) * 4096 + (tc.Y / 12));
					if (homeGroundZone >= 0 && cellZone >= 0 && cellZone != homeGroundZone && !hasConYardInZone)
						islandExpansionOreCells++;
				}
			}
		}
	}

	std::sort(expansionFieldBlocks.begin(), expansionFieldBlocks.end());
	expansionFieldBlocks.erase(std::unique(expansionFieldBlocks.begin(), expansionFieldBlocks.end()), expansionFieldBlocks.end());
	// Map-wide count on our landmass (the local scan above stops at 144 cells); the local block
	// count stays as a floor.
	GetMacroState(pHouse).UncolonizedOreFields = std::max(static_cast<int>(expansionFieldBlocks.size()),
		CountFreeOreFields(pHouse, baseCenter,
			baseCenter != CellStruct::Empty ? MapClass::Instance.GetMovementZoneType(baseCenter, MovementZone::Normal, false) : -1));

	const bool lowMoneyAndScarceLocalOre = (pHouse->Available_Money() < 1600 && localOreCells <= 8
		&& (expansionOreCells > 0 || islandExpansionOreCells > 0 || islandCapturableTechs > 0));
	const bool homeIslandCongestedOrDepleted = (uncolonizedIslandLandCells > 12
		&& (localOreCells <= 6 || pHouse->Buildings.Count >= 14 || pHouse->Available_Money() >= 3500
			|| lowMoneyAndScarceLocalOre));
	features[19] = ClampInt8(localOreCells * 4);
	features[20] = ClampInt8(
		expansionOreCells * 3 + (homeIslandCongestedOrDepleted ? 28 : 0) + (lowMoneyAndScarceLocalOre ? 24 : 0));
	features[21] = ClampInt8((islandEnemyBuildings > 0 ? 75 : 0)
		+ ((islandExpansionOreCells > 1 || islandCapturableTechs > 0 || homeIslandCongestedOrDepleted) ? 50 : 0));

	features[27] = ClampInt8(visibleCratesNearby * 45);

	features[98] = ClampInt8(
		camouflageAmbushAndGroveScore * 16 + std::min(40, nearbyTreeGroveCells * 2) + visibleEnemyHarvesters * 12);

	if (features[21] > 0 && features[67] < 40)
		features[67] = 40; // Ensure Shipyard is prioritized whenever an enemy or

	// Ore expansion is on a disconnected island!
	// 128..191: Dynamic Rules & GameModeOptions Context
	features[128] = GameModeOptionsClass::Instance.SWAllowed ? 80 : -80;

	features[129] = GameModeOptionsClass::Instance.Crates ? 60 : -60;

	features[130] = GameModeOptionsClass::Instance.ShortGame ? 80 : 0;

	features[131] = GameModeOptionsClass::Instance.BuildOffAlly ? 60 : 0;

	features[132] = GameModeOptionsClass::Instance.MCVRedeploy ? 60 : -60;

	features[134] = AreBridgesDestroyable() ? 80 : -80;

	const bool multiEngineerActive = RulesClass::Instance && RulesClass::Instance->EngineerCaptureLevel < 0.99;

	features[135] = multiEngineerActive ? 80 : -80;

	// Our fleet waits behind a standing low bridge with its targets on the other side.
	features[136] = bridgeState.SeaLaneSpan >= 0 ? 80 : 0;

	// An enemy fleet is about to sail through the gap of a broken low bridge toward us.
	features[137] = bridgeState.FleetBlockSpan >= 0 ? 80 : 0;
}


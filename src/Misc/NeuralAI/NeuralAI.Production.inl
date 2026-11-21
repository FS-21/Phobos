// NeuralAI.Production.inl: Main AI tick and production.
// Part of NeuralAI.cpp (included in order; one translation unit).

namespace
{
	// Picks that never come out of their factory. A pick the engine would suspend (no factory of
	// ours may build it while the power is on: HouseClass::Update_Factories suspends it and
	// BuildingClass::Factory_AI abandons it) is banned at once for EngineRefusedBanFrames. A type
	// that stays the pick of its queue for UndeliveredPickFrames, with a factory of the kind and the
	// money for it during UndeliveredPickPaidFrames of them, and never comes out is banned for
	// UndeliveredPickBanFrames, twice as long each time it is banned again (up to 8 times). A Nod
	// house whose primary war factory was not in the Venom's BuiltAt picked it all match and was
	// refused it every time (2026-11-17); a type the engine would build at the designated factory
	// is never banned, its factory is held up instead.
	constexpr int EngineRefusedBanFrames = 1800;
	constexpr int UndeliveredPickFrames = 5400;
	constexpr int UndeliveredPickPaidFrames = 3600;
	constexpr int UndeliveredPickBanFrames = 9000;
	constexpr int UndeliveredPickBanDoublings = 3;
	// A pick left for less than this keeps its streak (the choice flickers between ticks).
	constexpr int PickStreakGapFrames = 900;

	void BanProductionPick(HouseClass* pHouse, TechnoTypeClass* pType, int queue, int banFrames, const char* reason)
	{
		auto& macro = GetMacroState(pHouse);
		const int index = pType->GetArrayIndex();
		const int key = queue * 100000 + index;
		macro.ProductionBanUntil[key] = Unsorted::CurrentFrame + banFrames;
		macro.PickStreaks.erase(key);
		CountDecision(NeuralDecision::ProductionPickBanned, pHouse->ArrayIndex);
		Debug::Log("[NeuralAI] House %d production: %s banned for %d frames, %s, frame %d.\n", pHouse->ArrayIndex, pType->ID,
			banFrames, reason, Unsorted::CurrentFrame);

		auto const pExt = HouseExt::Fetch(pHouse);
		auto const dropPick = [index](int& pick)
		{
			if (pick == index)
				pick = -1;
		};

		switch (queue)
		{
		case 1:
			dropPick(pExt->NeuralAI_NextUnitTarget);
			dropPick(pHouse->ProducingUnitTypeIndex);
			break;
		case 2:
			dropPick(pExt->NeuralAI_NextInfantryTarget);
			dropPick(pHouse->ProducingInfantryTypeIndex);
			break;
		case 3:
			dropPick(pExt->NeuralAI_NextNavalTarget);
			dropPick(pExt->ProducingNavalUnitTypeIndex);
			break;
		case 4:
			dropPick(pExt->NeuralAI_NextAircraftTarget);
			dropPick(pHouse->ProducingAircraftTypeIndex);
			break;
		default:
			break;
		}
	}

	void WatchUndeliveredPick(HouseClass* pHouse, TechnoTypeClass* pType, int factories)
	{
		const int queue = ProductionQueueOf(pType);

		if (queue == 0 || factories <= 0 || IsProductionBanned(pHouse, queue, pType->GetArrayIndex()))
			return;

		auto const pEngineFactory = pType->FindFactory(true, true, true, pHouse);

		if (!pEngineFactory && !pHouse->HasLowPower())
		{
			BanProductionPick(pHouse, pType, queue, EngineRefusedBanFrames, "no factory of ours may build it");
			return;
		}

		auto& macro = GetMacroState(pHouse);
		const int key = queue * 100000 + pType->GetArrayIndex();
		const int now = Unsorted::CurrentFrame;
		auto& streak = macro.PickStreaks[key];

		if (streak.LastSeen < 0 || now < streak.LastSeen || now - streak.LastSeen > PickStreakGapFrames)
		{
			streak = {};
			streak.Since = now;
		}
		else if (pHouse->Available_Money() >= pType->GetActualCost(pHouse))
		{
			streak.PaidFrames += now - streak.LastSeen;
		}

		streak.LastSeen = now;

		if (now - streak.Since < UndeliveredPickFrames || streak.PaidFrames < UndeliveredPickPaidFrames)
			return;

		if (auto const it = macro.LastDeliveredFrame.find(key); it != macro.LastDeliveredFrame.end() && it->second >= streak.Since)
			return;

		// The engine would build it at the factory our queue builds at: the type is fine and that
		// factory is held up (its exit blocked, its queue owned by another one), no ban then.
		const AbstractType kind = queue == 2 ? AbstractType::InfantryType : (queue == 4 ? AbstractType::AircraftType : AbstractType::UnitType);

		if (pEngineFactory && pEngineFactory == NeuralFactoryQueue::FindDesignated(pHouse, kind, queue == 3))
			return;

		const int bans = macro.ProductionBanCount[key]++;
		char reason[96];
		_snprintf_s(reason, _TRUNCATE, "its pick since frame %d never came out of a factory", streak.Since);
		BanProductionPick(pHouse, pType, queue, UndeliveredPickBanFrames << std::min(bans, UndeliveredPickBanDoublings), reason);
	}

	// Evaluates buildability under Human Player rules:
	// - Requires valid human TechLevel (0..10), excluding TechLevel=-1 AI script
	// hacks/limbo dummies and TechLevel=11 disabled types. What a captured secret lab
	// grants the house is outside those levels on purpose: for it the level is not
	// looked at and the other prerequisites decide (HouseExt::PrerequisitesMet)
	// - AIBuildThis as the INI writes it decides for the AI (IsKeptFromAI, IsAIOnlyBuilding):
	// =no keeps a building from it, =yes lets it build a TechLevel=-1 one (the AI variants of
	// a mod) under the other prerequisites; NeuralAI::OnScenarioStart lists what =no keeps out
	// - Excludes [AI] NeuralAI.IgnoreTypes
	// - Temporarily sets IsHumanPlayer=true during HouseClass::CanBuild so Ares/YRpp enforce
	// real human Prerequisites, Prerequisite.Negative, Owner/ForbiddenHouses, BuildLimit,
	// and PowersUpBuilding instead of PrerequisiteOverride; an AI-only building is asked as
	// the AI it is, since Ares refuses TechLevel=-1 to a human
	bool CanBuildUnderHumanRules(HouseClass* pHouse, TechnoTypeClass* pType)
	{
		if (!pHouse || !pType)
			return false;

		auto const pBld = abstract_cast<BuildingTypeClass*>(pType);

		if (pBld && IsKeptFromAI(pBld))
			return false;

		const bool aiOnly = IsAIOnlyBuilding(pType);

		if (!aiOnly && !pHouse->HasFromSecretLab(pType)
			&& (pType->TechLevel < 0 || pType->TechLevel > 10 || pType->TechLevel > pHouse->TechLevel))
		{
			return false;
		}

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

		// Ares BuiltAt: one of the factory types it names has to stand (the queue is built at one of
		// them, see BuildingClass_Update_Factory_NeuralDesignated).
		if (auto const& builtAt = TechnoTypeExt::Fetch(pType)->BuiltAt; !builtAt.empty())
		{
			const bool hasFactory = std::any_of(pHouse->Buildings.begin(), pHouse->Buildings.end(), [&builtAt](BuildingClass* pOwnBld)
				{
					return pOwnBld && pOwnBld->IsAlive && !pOwnBld->InLimbo && builtAt.Contains(pOwnBld->Type);
				});

			if (!hasFactory)
				return false;
		}

		// The INI's AIBuildThis is judged above, so the call sees it set.
		const bool prevHuman = pHouse->IsHumanPlayer;
		const bool prevAIBuild = pBld ? pBld->AIBuildThis : true;

		pHouse->IsHumanPlayer = !aiOnly;

		if (pBld)
			pBld->AIBuildThis = true;

		const CanBuildResult res = pHouse->CanBuild(pType, false, false);

		pHouse->IsHumanPlayer = prevHuman;

		if (pBld)
			pBld->AIBuildThis = prevAIBuild;

		return res == CanBuildResult::Buildable;
	}

	// The buildings a type still needs from us: of its prerequisite lists (Prerequisite and the
	// Prerequisite.ListN alternatives), the one with the fewest entries missing. A missing generic
	// group (NODTECH, GDIWEAP...) lists every building in it; the production builds the one it can.
	// The vanilla list drops generic names, so the Phobos lists are read first.
	std::vector<int> MissingTypePrerequisites(HouseClass* pHouse, TechnoTypeClass* pType)
	{
		std::vector<int> best;
		int bestMissing = INT_MAX;
		auto const pExt = pType ? TechnoTypeExt::Fetch(pType) : nullptr;

		if (!pHouse || !pExt)
			return best;

		auto const& generics = RulesExt::Global()->GenericPrerequisites;
		auto const consider = [&](auto const& list)
		{
			std::vector<int> buildings;
			int missing = 0;

			for (const int idx : list)
			{
				if (HouseExt::HasPrerequisite(pHouse, idx))
					continue;

				missing++;

				if (idx >= 0)
				{
					buildings.push_back(idx);
				}
				else if (-idx < generics.Count)
				{
					for (const int member : generics.GetItem(-idx))
					{
						if (member >= 0)
							buildings.push_back(member);
					}
				}
			}

			if (missing < bestMissing)
			{
				bestMissing = missing;
				best = std::move(buildings);
			}
		};

		if (!pExt->Prerequisite.empty())
			consider(pExt->Prerequisite);
		else
			consider(pType->Prerequisite);

		for (auto const& list : pExt->Prerequisite_ListVector)
			consider(list);

		return best;
	}

	// Can the house ever meet one of the type's prerequisite lists? Every entry it lacks has to be a
	// building of its side and tech level, or a generic group with one such building. A list that asks
	// for a building nobody builds (TechLevel=-1: the mod's hack that keeps a type for AI scripts only)
	// is never met.
	bool CanEverMeetPrerequisites(HouseClass* pHouse, TechnoTypeClass* pType)
	{
		auto const pExt = pType ? TechnoTypeExt::Fetch(pType) : nullptr;

		if (!pHouse || !pExt)
			return false;

		const int typeIndex = pHouse->Type ? pHouse->Type->ArrayIndex : -1;
		const unsigned int ownHouseBit = (typeIndex >= 0 && typeIndex < 32) ? (1u << typeIndex) : 0u;
		auto const& generics = RulesExt::Global()->GenericPrerequisites;

		auto const buildable = [&](int idx)
		{
			auto const pBld = idx >= 0 && idx < BuildingTypeClass::Array.Count ? BuildingTypeClass::Array.GetItem(idx) : nullptr;

			return pBld && ((pBld->TechLevel >= 0 && pBld->TechLevel <= pHouse->TechLevel) || IsAIOnlyBuilding(pBld))
				&& !IsKeptFromAI(pBld) && (ownHouseBit == 0u || pBld->InOwners(ownHouseBit));
		};

		auto const met = [&](auto const& list)
		{
			for (const int idx : list)
			{
				if (HouseExt::HasPrerequisite(pHouse, idx))
					continue;

				if (idx >= 0)
				{
					if (!buildable(idx))
						return false;

					continue;
				}

				bool any = false;

				if (-idx < generics.Count)
				{
					for (const int member : generics.GetItem(-idx))
					{
						if (buildable(member))
						{
							any = true;
							break;
						}
					}
				}

				if (!any)
					return false;
			}

			return true;
		};

		if (!pExt->Prerequisite.empty() ? met(pExt->Prerequisite) : met(pType->Prerequisite))
			return true;

		for (auto const& list : pExt->Prerequisite_ListVector)
		{
			if (met(list))
				return true;
		}

		return false;
	}

	// Defined in NeuralAI.Placement.inl.
	void SellFactoryRoadBlock(HouseClass* pHouse);
	void SellPocketBlock(HouseClass* pHouse);

	// A further coverage support building that found no key building to cover waits this long
	// before it is tried again (AdvAI's support group cooldown).
	constexpr int SupportNoCoverFrames = 3600;

	// Coverage support buildings (AdvAI BuildSupport): gap and cloak generators, inhibitors and radar
	// jammers protect the key buildings within their radius. Sensors are left to the detection need.
	bool IsCoverageSupport(const BuildingTypeClass* pType)
	{
		if (!pType || pType->IsBaseDefense)
			return false;

		auto const pBldExt = BuildingTypeExt::Fetch(pType);
		const bool gapGenerator = (pBldExt && pBldExt->GapGenerator) || pType->GapGenerator;
		auto const pTechExt = TechnoTypeExt::Fetch(pType);

		return (pType->CloakGenerator && pType->CloakRadiusInCells > 0) || (gapGenerator && GetSupportRadius(pType) > 0)
			|| (pTechExt && pTechExt->InhibitorRange.isset() && pTechExt->InhibitorRange.Get() > 0)
			|| (pTechExt && pTechExt->RadarJamRadius.Get() > 0);
	}

	// Two support buildings of one network cover for each other: the same type, the same GroupAs or
	// the same function.
	bool IsSameSupportNetwork(const BuildingTypeClass* pA, const BuildingTypeClass* pB)
	{
		if (!pA || !pB)
			return false;

		if (pA == pB
			|| _strcmpi(TechnoTypeExt::GetSelectionGroupID(const_cast<BuildingTypeClass*>(pA)),
				TechnoTypeExt::GetSelectionGroupID(const_cast<BuildingTypeClass*>(pB))) == 0)
		{
			return true;
		}

		auto const pExtA = BuildingTypeExt::Fetch(pA);
		auto const pExtB = BuildingTypeExt::Fetch(pB);
		const bool gapA = (pExtA && pExtA->GapGenerator) || pA->GapGenerator;
		const bool gapB = (pExtB && pExtB->GapGenerator) || pB->GapGenerator;
		auto const pTechA = TechnoTypeExt::Fetch(pA);
		auto const pTechB = TechnoTypeExt::Fetch(pB);
		const bool inhibitorA = pTechA && pTechA->InhibitorRange.isset() && pTechA->InhibitorRange.Get() > 0;
		const bool inhibitorB = pTechB && pTechB->InhibitorRange.isset() && pTechB->InhibitorRange.Get() > 0;
		const bool jammerA = pTechA && pTechA->RadarJamRadius.Get() > 0;
		const bool jammerB = pTechB && pTechB->RadarJamRadius.Get() > 0;

		return (gapA && gapB) || (pA->CloakGenerator && pB->CloakGenerator) || (pA->SensorArray && pB->SensorArray)
			|| (inhibitorA && inhibitorB) || (jammerA && jammerB);
	}

	// The buildings a support network should cover (AdvAI's base buildings): construction yards,
	// factories, airfields and helipads, refineries, radars, superweapons, service depots and tech.
	bool IsSupportKeyBuilding(BuildingTypeClass* pType)
	{
		if (!pType || pType->IsBaseDefense || pType->Wall || pType->InvisibleInGame)
			return false;

		auto const pBldExt = BuildingTypeExt::Fetch(pType);

		return pType->ConstructionYard || pType->Factory != AbstractType::None || pType->Helipad || pType->Refinery
			|| pType->Radar || pType->SpySat || pType->UnitRepair || pType->SuperWeapon != -1 || pType->SuperWeapon2 != -1
			|| (pBldExt && !pBldExt->SuperWeapons.empty()) || IsCoreTechBuilding(pType);
	}

	// Distance from a support building within which a key building counts as covered.
	int SupportCoverageCells(const BuildingTypeClass* pType)
	{
		return std::max(3, GetSupportRadius(pType) - 1);
	}

	// Separation between two support buildings of one network: 1.8 times the radius (AdvAI).
	int SupportSeparationCells(const BuildingTypeClass* pType)
	{
		const int radius = GetSupportRadius(pType);

		return radius > 1 ? std::max(7, radius * 9 / 5) : 8;
	}

	// The key buildings no support building of the network covers yet, and how many buildings the
	// network has.
	void GatherSupportCoverage(HouseClass* pHouse, const BuildingTypeClass* pSupport, std::vector<CellStruct>& networkCells,
		std::vector<CellStruct>& uncoveredKeyCells)
	{
		networkCells.clear();
		uncoveredKeyCells.clear();
		std::vector<CellStruct> keyCells;

		for (auto const pOwnBld : pHouse->Buildings)
		{
			if (!pOwnBld || pOwnBld->InLimbo || !pOwnBld->IsAlive || !pOwnBld->Type)
				continue;

			const CellStruct cell = pOwnBld->GetMapCoords();

			if (IsSameSupportNetwork(pOwnBld->Type, pSupport))
				networkCells.push_back(cell);

			if (IsSupportKeyBuilding(pOwnBld->Type))
				keyCells.push_back(cell);
		}

		const double coverage = static_cast<double>(SupportCoverageCells(pSupport));

		for (const CellStruct& keyCell : keyCells)
		{
			const bool covered = std::any_of(networkCells.begin(), networkCells.end(),
				[&keyCell, coverage](const CellStruct& supportCell) { return keyCell.DistanceFrom(supportCell) <= coverage; });

			if (!covered)
				uncoveredKeyCells.push_back(keyCell);
		}
	}

	// The first coverage support building of a network is wanted as before; one more while a key
	// building lies outside the network and its power drain leaves the base powered (user,
	// 2026-11-20: the AdvAI BuildSupport rules).
	bool SupportCopyWanted(HouseClass* pHouse, const BuildingTypeClass* pSupport, int ownedOfType)
	{
		std::vector<CellStruct> networkCells;
		std::vector<CellStruct> uncoveredKeyCells;
		GatherSupportCoverage(pHouse, pSupport, networkCells, uncoveredKeyCells);

		if (networkCells.empty() && ownedOfType == 0)
			return true;

		if (pHouse->HasLowPower() || pHouse->PowerOutput - pHouse->PowerDrain < pSupport->PowerDrain)
			return false;

		return !uncoveredKeyCells.empty();
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
	// LAN desync test only: house checksum before and after this tick.
	const NeuralDesyncTraceScope desyncTrace { pHouse };

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

	// TEMP diagnostic: parked transports moved since the last tick of this house.
	TraceParkedFerries(pHouse->ArrayIndex, "the engine or another house between ticks");
	// TEMP diagnostic: goals changed since the last tick of this house, and by which code; the goals at
	// the end of this tick are looked at when it is over.
	UpdateOrderChurn(pHouse);
	const NeuralOrderChurnTickEnd orderChurnTickEnd { pHouse };

	{
		const NeuralSectionTimer sectionTimer { pHouse->ArrayIndex, NeuralSection::Features };
		UpdateBridgePlanner(pHouse);
		NeuralAI::ExtractFogFilteredFeatures(pHouse, features);

		// Defense and ambush spots from the enemy routes the fog scan just recorded.
		UpdateRouteHeat(pHouse);
	}

	// The fog scan may have marked new enemy buildings as seen: rebuild the known-building cache.
	InvalidateKnownEnemyBuildings(pHouse);

	{
		const NeuralSectionTimer sectionTimer { pHouse->ArrayIndex, NeuralSection::MoE };
		NeuralAI::RunDeterministicMoE(pHouse, features, scores);
	}

	// Per-match style, opening and surprise tech switch on top of the trained scores.
	ApplyMatchStyle(pHouse, scores);

	// The main battle tank is the most versatile unit of the war factory: in no archetype its class
	// weighs less than the light vehicles (user: their near absence is unnatural; outside the tank mass
	// slot 24 had the lowest median of the five vehicle classes, 331 to 772 against 1398 to 1994).
	scores[24] = std::max(scores[24], scores[25]);

	// Detection need (Detection*): what this house needs detectors for now, the cap of its mobile
	// detectors, and the reach of the vehicle detectors of ours for that need. A detector building at home
	// never goes with the army, so only vehicles fill the vehicles' cap (a Psychic Sensor of 15 cells kept
	// every radar jammer of 8 out: jammers fell from 0.88% to 0.09% of the NextUnit lines); the buildings
	// are counted apart for the diagnostic.
	const int detectionNeed = DetectionNeedOf(pHouse);
	const int detectionCap = DetectionCap + (pHouse->ConYards.Count >= 2 ? 1 : 0);
	const bool detectionSpotUncovered = (detectionNeed & DetectionNeedStealth) && DetectionSpotUncovered(pHouse);
	std::vector<int> ownedDetectorReaches;
	int ownedDetectorBuildings = 0;
	int ownedDetectorBuildingReach = 0;

	if (detectionNeed)
	{
		for (auto const pOwnB : pHouse->Buildings)
		{
			if (pOwnB && pOwnB->IsAlive && !pOwnB->InLimbo && pOwnB->Type)
			{
				if (const int reach = DetectionReachCells(pOwnB->Type, detectionNeed); reach > 0)
				{
					ownedDetectorBuildings++;
					ownedDetectorBuildingReach = std::max(ownedDetectorBuildingReach, reach);
				}
			}
		}

		for (auto const pOwnU : UnitClass::Array)
		{
			if (pOwnU && pOwnU->Owner == pHouse && pOwnU->IsAlive && !pOwnU->InLimbo && pOwnU->Type && !pOwnU->Type->Naval)
			{
				if (const int reach = DetectionReachCells(pOwnU->Type, detectionNeed); reach > 0)
					ownedDetectorReaches.push_back(reach);
			}
		}
	}

	// The detectors of ours that reach as far as `reach` or farther: they keep a shorter one out.
	auto const detectorsReaching = [&ownedDetectorReaches](int reach)
	{
		return static_cast<int>(std::count_if(ownedDetectorReaches.begin(), ownedDetectorReaches.end(), [reach](int owned) { return owned >= reach; }));
	};

	// TEMP diagnostic (detection need): every DetectionLogFrames per house.
	{
		auto& detectMacro = GetMacroState(pHouse);
		const int now = Unsorted::CurrentFrame;

		if (now - detectMacro.DetectionLogFrame >= DetectionLogFrames || now < detectMacro.DetectionLogFrame)
		{
			detectMacro.DetectionLogFrame = now;
			int sightings = 0;
			std::array<int, 3> losses {};

			for (auto const& [uid, seen] : detectMacro.StealthSightings)
				sightings += now - seen.second <= SensorLookoutSightingFrames ? 1 : 0;

			for (auto const& loss : detectMacro.HiddenLosses)
			{
				if (now - loss.Frame <= SensorLookoutSightingFrames)
					losses[static_cast<size_t>(std::clamp(loss.Kind, 0, 2))]++;
			}

			Debug::Log("[NeuralAI] Frame %d | House #%d detection: need%s%s%s%s, last enemy seen %d frames ago, stealth sightings %d, hidden losses %d dark %d cloaked %d naval, detectors %d (best reach %d), cap %d, spot uncovered %d, detector buildings %d (best reach %d)\n",
				now, pHouse->ArrayIndex, detectionNeed ? "" : " none", (detectionNeed & DetectionNeedBlind) ? " blind" : "",
				(detectionNeed & DetectionNeedStealth) ? " stealth" : "", (detectionNeed & DetectionNeedSub) ? " sub" : "",
				now - detectMacro.LastEnemySeenFrame, sightings, losses[0], losses[1], losses[2], static_cast<int>(ownedDetectorReaches.size()),
				ownedDetectorReaches.empty() ? 0 : *std::max_element(ownedDetectorReaches.begin(), ownedDetectorReaches.end()), detectionCap,
				detectionSpotUncovered ? 1 : 0, ownedDetectorBuildings, ownedDetectorBuildingReach);
		}
	}

	pExt->NeuralAI_LastInferenceFrame = Unsorted::CurrentFrame;

	// Before any air lane is planned this tick: remember where our flyers were shot down.
	{
		const NeuralSectionTimer sectionTimer { pHouse->ArrayIndex, NeuralSection::Tactics };
		UpdateAirLossMemory(pHouse);
		UpdateMatchTactics(pHouse, scores);
		UpdateEnemyStrength(pHouse);
		TraceParkedFerries(pHouse->ArrayIndex, "features, style and tactics");
	}

	{
		const NeuralSectionTimer sectionTimer { pHouse->ArrayIndex, NeuralSection::Micro };
		UpdateArmyLedger(pHouse);
		UpdateRepairTrips(pHouse);
		// Seated fighting vehicles fill before the wave dispatch reads who is free.
		UpdatePassengerFill(pHouse);
		TraceParkedFerries(pHouse->ArrayIndex, "army ledger, repair trips and passenger fill");
		NeuralAI::ExecuteTacticalAndSpecOpsMicro(pHouse, scores);
		TraceParkedFerries(pHouse->ArrayIndex, "tactical micro");
	}

	// Strategic plans run after the tactical micro so they keep the last word on their units.
	{
		const NeuralSectionTimer sectionTimer { pHouse->ArrayIndex, NeuralSection::Plans };
		UpdateStrategicPlans(pHouse, scores);
		TraceParkedFerries(pHouse->ArrayIndex, "strategic plans");
	}

	// Release units parked by orders they cannot complete (destroyed bridge, endless jam).
	{
	const NeuralSectionTimer watchdogTimer { pHouse->ArrayIndex, NeuralSection::Watchdogs };
	{
		// It frees units of plans too (an operation order: plan members take no routine order).
		const NeuralOrderPriorityScope orderPriority { NeuralOrderPriority::Operation };
		UpdateStuckUnits(pHouse);
	}
	TraceParkedFerries(pHouse->ArrayIndex, "stuck units watchdog");

	// Soldiers shut in a building they no longer walk into are put down outside it.
	FreeSoldiersShutInBuildings(pHouse);

	// Empty harvesters waiting where the ore ran out go to the nearest free field.
	UpdateIdleHarvesters(pHouse);
	TraceParkedFerries(pHouse->ArrayIndex, "idle harvesters");

	// Deployed slave miners with no ore in reach pack up for a field that has some.
	UpdateStrandedSlaveMiners(pHouse);

	// Every war factory closed in by the base and vehicles jammed at home: a spare building that
	// closes a road is sold.
	SellFactoryRoadBlock(pHouse);

	// Units shut in a pocket by our own buildings: the one that closes it is sold.
	SellPocketBlock(pHouse);
	TraceParkedFerries(pHouse->ArrayIndex, "stranded slave miners");

	// Forward crawl stones placed / lost (trainer credit for the offensive crawl).
	UpdateOffensiveStones(pHouse);
	TraceParkedFerries(pHouse->ArrayIndex, "offensive stones");

	// Capital units gather and strike in groups.
	UpdateCapitalGroups(pHouse);
	TraceParkedFerries(pHouse->ArrayIndex, "capital groups");

	// Part of the main battle tanks guards our sites and hunts the enemy siege near them.
	UpdateTankGuard(pHouse);

	// Submarines keep out of known enemy sonar.
	UpdateSubmarineStealth(pHouse);
	TraceParkedFerries(pHouse->ArrayIndex, "submarine stealth");

	// Veinhole monsters near us and destroyable cliffs that open a way.
	UpdateMapObstacles(pHouse);
	TraceParkedFerries(pHouse->ArrayIndex, "map obstacles");

	// Idle airships / jumpjet bombers near known enemy structures attack them.
	UpdateIdleAirborneStrikers(pHouse);
	TraceParkedFerries(pHouse->ArrayIndex, "idle airborne strikers");

	// Idle ground units in enemy territory attack the nearest enemy building or unit they can hurt.
	UpdateIdleGroundStrikers(pHouse);
	TraceParkedFerries(pHouse->ArrayIndex, "idle ground strikers");

	// Transports that cannot unload where they stand try a free cell nearby.
	UpdateStuckUnloads(pHouse);
	TraceParkedFerries(pHouse->ArrayIndex, "stuck unloads");

	// Scouts go where information is worth most (stale fields, approaches, enemy bases and armies).
	UpdatePurposefulScouting(pHouse);
	TraceParkedFerries(pHouse->ArrayIndex, "scouting");

	// Best ore field not mined yet (Expert 6 hoards: claims contested fields too).
	UpdateResourceClaim(pHouse);

	// What each missing building would unlock in the tech tree.
	UpdateTechUnlocks(pHouse);

	// Highest unit tier allowed now (super units come step by step).
	UpdateUnitTierCap(pHouse);

	// Factories the bank can pay for (the production queues spend at a fixed rate per factory).
	UpdateBankGovernor(pHouse);

	// Expert 0: economy peaks and crisis actions (blackout, broke, harvester balance).
	UpdateEconomyCrisis(pHouse);
	TraceParkedFerries(pHouse->ArrayIndex, "economy crisis");

	// Expert 3: tacit coalition against a dangerous house (outside preset teams only).
	UpdateTacitCoalition(pHouse);
	TraceParkedFerries(pHouse->ArrayIndex, "coalition");

	// Expert 3: which enemy to hit in a free-for-all.
	UpdateFfaTargets(pHouse);
	TraceParkedFerries(pHouse->ArrayIndex, "free-for-all targets");

	// Expert 2: which buildable types and missing tech answer the enemy army seen so far.
	UpdateCounterComposition(pHouse);

	// Expert 4: neutral techs seen, capture / spy / bridge work and the engineers and spies out.
	UpdateSpecOps(pHouse);
	TraceParkedFerries(pHouse->ArrayIndex, "special operations");

	// Expert 4: demolition trucks and terrorists sneaking in without a transport.
	UpdateSneakStrikes(pHouse);
	TraceParkedFerries(pHouse->ArrayIndex, "sneak strikes");
	// Expert 9: occupiable buildings seen, garrisons to hold, evacuate or clear, coalition work.
	UpdateUrban(pHouse);
	TraceParkedFerries(pHouse->ArrayIndex, "urban");

	// Transport operations last: they have the final word over their units and transports.
	UpdateTransportOperations(pHouse);
	}

	// Production: from here to the end of the tick.
	const NeuralSectionTimer productionTimer { pHouse->ArrayIndex, NeuralSection::Production };

	// Select next targets for Building, Unit, Infantry, and Aircraft queues
	const int money = pHouse->Available_Money();
	const int rawParentIdx = pHouse->Type ? pHouse->Type->FindParentCountryIndex() : -1;
	const int parentCountryIdx = (rawParentIdx >= 0) ? rawParentIdx : (pHouse->Type ? pHouse->Type->ArrayIndex2 : -1);
	const unsigned int ownHouseBit = (parentCountryIdx >= 0 && parentCountryIdx < 32) ? (1u << parentCountryIdx) : 0u;

	NeuralPhaseClock productionClock { pHouse->ArrayIndex, NeuralSection::ProdBuildings };

	// The best refinery past the rules of the building pick, with its score (none: no refinery got
	// scored), for the economy hold (RefinerySaveTarget). The score of the scored building pick is for
	// the TEMP economy hold status line.
	int awaitedRefineryIdx = -1;
	int awaitedRefineryScore = -32000;
	int scoredBuildingScore = -32000;

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
		// Defenses with any gun that shoots up, pure anti-air or not.
		int totalAntiAirDefenses = 0;
		// Their worth in AntiAirValue points, the measure the Advanced AI uses; one that declares none counts as a typical one.
		constexpr int airDefenseTypicalValue = 20;
		int ownedAntiAirValue = 0;
		// The worth of our ground defenses in AntiInfantryValue + AntiArmorValue points.
		int ownedGroundDefenseValue = 0;
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
				&& pOwnInf->Type->Occupier && !pOwnInf->Type->Engineer && !IsSpyInfantryType(pOwnInf->Type))
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

				const auto pSecondWS = pOT->GetWeapon(1);
				const auto pSecondW = pSecondWS ? pSecondWS->WeaponType : nullptr;
				const bool shootsUp = (pW && pW->Projectile && pW->Projectile->AA)
					|| (pSecondW && pSecondW->Projectile && pSecondW->Projectile->AA);

				if (shootsUp)
				{
					totalAntiAirDefenses++;
					ownedAntiAirValue += pOT->AntiAirValue > 0 ? pOT->AntiAirValue : airDefenseTypicalValue;
				}

				const bool isPureAA = pW && pW->Projectile && pW->Projectile->AA && !pW->Projectile->AG;

				if (isPureAA)
				{
					totalAADefenses++;
				}
				else if (!isUnarmedBunker || pOwnBld->Occupants.Count > 0 || pOwnBld->BunkerLinkedItem != nullptr)
				{
					totalGroundDefenses++;
					const int groundValue = pOT->AntiInfantryValue + pOT->AntiArmorValue;
					ownedGroundDefenseValue += groundValue > 0 ? groundValue : GroundDefenseTypicalValue;
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

				auto& gathererMap = GetMacroState(pHouse).TemporaryRecoveryRefineryGatherers;

				if (pBld->CurrentMission == Mission::Selling || pBld->QueuedMission == Mission::Selling)
				{
					gathererMap.erase(pBld->UniqueID);
					tempMap.erase(it);
					break;
				}

				// Sold once its FreeUnit is out: one gatherer more than when it was placed (user,
				// 2026-11-18: it was sold 75 frames after its buildup whether the harvester had come out
				// or not), or after TemporaryRefineryMaxFrames anyway.
				auto const gathererIt = gathererMap.find(pBld->UniqueID);
				const bool freeUnitOut = gathererIt == gathererMap.end() || CountOwnGatherers(pHouse) > gathererIt->second;

				if (pBld->CurrentMission != Mission::Construction && pBld->QueuedMission != Mission::Construction
					&& pBld->BState != static_cast<int>(BStateType::Construction)
					&& Unsorted::CurrentFrame >= it->second + 75
					&& (freeUnitOut || Unsorted::CurrentFrame >= it->second + TemporaryRefineryMaxFrames))
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
					gathererMap.erase(pBld->UniqueID);
					GetMacroState(pHouse).NextHarvesterRecoveryFrame = Unsorted::CurrentFrame + 1800;
					Debug::Log("[NeuralAI] HarvesterRecovery: House %d selling temporary "
							"recovery refinery %s at (%d,%d) %s.\n",
						pHouse->ArrayIndex, pBld->Type->ID, pBld->GetMapCoords().X, pBld->GetMapCoords().Y,
						freeUnitOut ? "after spawning FreeUnit harvester" : "with no new harvester after the longest wait");
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

		// A building switched off asks for more power only while the real surplus is short of the
		// cushion. One that stays off with power to spare (an EMP, a saboteur) is not cured by more
		// plants: a Yuri house kept picking a bio reactor over its scored pick with 12000 of
		// surplus, built 89 of them and walled in the mouth of its own bridge (match #8920).
		if (hasUnpoweredBuildings && realPowerSurplus >= requiredPowerCushion)
		{
			hasUnpoweredBuildings = false;
		}

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

		// Bank governor (UpdateBankGovernor): the factories it wants come past the caps, never into
		// a streak of failed placements. The best of them is remembered for the pick after the loop.
		const bool bankGovernorBuilds = bankMacro.BankGovernorActive && bankMacro.ConsecutivePlacementFailures <= 2;
		int bankFactoryIdx = -1;
		int bankFactoryBest = -32000;

		// The queue with the largest deficit first, then the best score among its factories.
		auto const rememberBankFactory = [&bankFactoryIdx, &bankFactoryBest](BuildingTypeClass* pType, int score, int deficit)
		{
			const int rank = std::min(deficit, 20) * 10000 + std::clamp(score, -9999, 9999);

			if (rank > bankFactoryBest)
			{
				bankFactoryBest = rank;
				bankFactoryIdx = pType->ArrayIndex;
			}
		};

		// Offensive fronts: forward stones alive raise the factory caps (2 more per 4 stones, up
		// to 8), so the hot zones keep receiving factories after the home base has its share.
		const int frontFactoryRoom = std::min(8, static_cast<int>(bankMacro.OffensiveStones.size()) / 2);

		// The tank mass may build its war factories ahead of the bank: the second with one refinery
		// less than TankMassRefineries, each one after it with a refinery more.
		const int tankMassWarFactories = pExt->NeuralAI_Archetype == TankMassArchetype
			? std::clamp(ownedRefineries - TankMassRefineries + 3, 1, TankMassWarFactories)
			: 0;

		// No fixed ceiling (user: the caps nobody asked for kept a house from matching the enemy's
		// production): the economy or one more than the known factories of the strongest enemy,
		// whichever is more, bounded only by room, money and the difficulty cap per category.
		const int maxWarFactories = std::max(tankMassWarFactories, (hasTechCenter || money >= 2400)
			? std::max({ 2, ecoWarFactoryCap, maxEnemyWarFactories + 1 }) + bankSteps + frontFactoryRoom
			: std::clamp(ecoWarFactoryCap, 1, 3));

		const int ecoBarracksCap = (ownedRefineries >= 4) ? 2 : 1;

		const int maxBarracks = (hasTechCenter || money >= 2500)
			? std::max({ 2, ecoBarracksCap, maxEnemyBarracks + 1 }) + bankSteps / 2 + frontFactoryRoom / 2
			: std::clamp(ecoBarracksCap, 1, 2);

		// Shipyards (user): on an island map (starting bases not linked by land, or enemy buildings
		// on another landmass) no cap of their own, only the category cap by difficulty: placement
		// spreads them over the conquered islands (an island without one first), not all the eggs in
		// one basket. Elsewhere, with water to use against the enemy, Easy 1, Normal 2, Hard 3.
		const bool islandNavalWar = (MapInfo.Computed && !MapInfo.LandLinked) || features[21] >= 75;
		const int houseDifficulty = static_cast<int>(pHouse->GetAIDifficultyIndex());
		const int maxShipyards = islandNavalWar
			? GetFactoryCategoryCap(pHouse, 2)
			: (houseDifficulty >= 2 ? 1 : (houseDifficulty == 1 ? 2 : 3));

		// Aircraft that need a dock, the ones in production included (they hold one already).
		int ownedDockingAircraft = 0;
		for (auto const pOwnAir : AircraftClass::Array)
		{
			if (pOwnAir && pOwnAir->Owner == pHouse && pOwnAir->IsAlive && pOwnAir->Type
				&& !pOwnAir->SpawnOwner && (pOwnAir->Type->AirportBound || pOwnAir->Type->Dock.Count > 0))
			{
				ownedDockingAircraft++;
			}
		}

		// Airfields follow the docks (user): another one while fewer docks than MinFreeAirDocks for
		// the difficulty stand free (Easy 4, Normal 6, Hard 8), with no cap; an airfield is a hangar,
		// not a faster factory.
		const bool airDocksFull = ownedDockingAircraft >= pHouse->AirportDocks;
		const int freeAirDocks = pHouse->AirportDocks - ownedDockingAircraft;
		const int wantedFreeAirDocks = houseDifficulty >= 2 ? MinFreeAirDocksEasy : (houseDifficulty == 1 ? MinFreeAirDocksNormal : MinFreeAirDocksHard);
		const bool airDocksShort = freeAirDocks < wantedFreeAirDocks;

		const bool expert8NeedsMoreAirpadDocks = (ownedAirpads >= 1 && ownedDockingAircraft >= pHouse->AirportDocks
			&& (scores[32] > 160 || scores[33] > 160 || pExt->NeuralAI_ActiveExpert == 8
				|| pExt->NeuralAI_Archetype == 6));
		// The aircraft pick wanted a type that needs a dock and had none free: one more airfield.
		const bool airDockWanted = airDocksFull && ownedAirpads >= 1
			&& Unsorted::CurrentFrame - GetMacroState(pHouse).AirDockWantedFrame <= AirDockWantedFrames;

		// The docks every house keeps (AirDocksFloor*), whatever its archetype: below them, or with every
		// dock taken (the next aircraft would have none, as the Advanced AI grows them), the best airfield
		// is remembered for the pick after the loop.
		const int airDocksFloor = houseDifficulty >= 2 ? AirDocksFloorEasy : (houseDifficulty == 1 ? AirDocksFloorNormal : AirDocksFloorHard);
		const bool airDocksBelowFloor = pHouse->AirportDocks < airDocksFloor;
		int bestPriorityAirfieldIdx = -1;
		int bestPriorityAirfieldScore = -32000;

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

				// Flying vehicles are an air raid: ground towers cannot hit them, so they ask for none.
				if (pEU->Type->JumpJet || pEU->Type->MovementZone == MovementZone::Fly || pEU->IsInAir())
					continue;

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

		// Defenses that shoot up the base wants, measured as the Advanced AI does: points of
		// AntiAirValue (a typical defense gives airDefenseTypicalValue). A base with a factory keeps
		// a standing amount that grows with its economy before any enemy aircraft shows up:
		// aircraft are fragile but strike hard and fast, and guns started after the first raid come
		// too late. The enemy air bases we know of announce that air power and count ahead of it.
		// Once the enemy air power is worth a gun the standing amount is larger and that power is
		// added; while a raid lasts, a margin on top. Of that total, the enemy air power plus the
		// margin is what the raid itself asks for: only that part goes ahead of other picks, the
		// rest is built at leisure.
		// The base and the most by difficulty (user, as the Advanced AI). Only the level of the base
		// stops at DefenseMaxValue: the enemy air power comes on top of it, with no cap on the number
		// of guns (user, 2026-11-21: a big base is not covered with 11 guns; the Advanced AI adds the
		// enemy aircraft value past its cap). The fog-fair memory of that power bounds it.
		const int defenseDifficulty = std::clamp(houseDifficulty, 0, 2);
		constexpr int airDefenseUrgentValue = 20;
		constexpr int airDefenseStandingValue = 25;
		const int airDefenseBaseValue = AntiAirBaseValue[defenseDifficulty];
		constexpr int airDefenseValuePerRefinery = 10;
		constexpr int airDefenseValuePerEnemyAirBase = 20;
		constexpr int airDefenseEnemyAirBaseCap = 60;
		constexpr int airDefenseRaidValue = 60;
		const bool airRaid = underAirAttack || activeThreatCat == NeuralThreatCategory::Air;
		const int raidAirValue = airRaid ? enemyAirValue + airDefenseRaidValue : 0;
		const bool enemyAirKnown = airRaid || enemyAirValue >= airDefenseUrgentValue;
		int knownEnemyAirBases = 0;

		for (auto const& info : GetKnownEnemyBuildingCache(pHouse).Entries)
		{
			if (info.Building->Type->Factory == AbstractType::AircraftType)
				knownEnemyAirBases++;
		}

		const int announcedAirValue = std::min(knownEnemyAirBases * airDefenseValuePerEnemyAirBase, airDefenseEnemyAirBaseCap);
		const bool keepsStandingAirDefense = ownedWarFactories >= 1 || ownedTechStructures >= 1 || announcedAirValue > 0;
		const int wantedAirValue = enemyAirKnown
			? std::min(DefenseMaxValue[defenseDifficulty], airDefenseBaseValue + ownedRefineries * airDefenseValuePerRefinery)
				+ std::max(enemyAirValue, announcedAirValue) + (airRaid ? airDefenseRaidValue : 0)
			: (keepsStandingAirDefense
				? std::min(DefenseMaxValue[defenseDifficulty], airDefenseStandingValue + ownedRefineries * airDefenseValuePerRefinery + announcedAirValue)
				: 0);
		auto const airDefensesMissingFor = [&](int value) -> int
		{
			return (std::max(0, value - ownedAntiAirValue) + airDefenseTypicalValue - 1) / airDefenseTypicalValue;
		};

		const int airDefenseDeficit = airDefensesMissingFor(wantedAirValue);
		const int raidDefenseDeficit = airDefensesMissingFor(raidAirValue);
		const int wantedAirDefenses = totalAntiAirDefenses + airDefenseDeficit;
		const int baseMaxAA = 1 + ownedWarFactories + ownedTechStructures + defDensityMod + (underAirAttack ? 2 : 0);
		const int maxAADefenses = std::max({ 2, baseMaxAA, wantedAirDefenses });
		const bool airRaidShortOfDefenses = raidDefenseDeficit > 0;
		// With no ground attack on top, a raid short of guns leaves the towers that cannot shoot up for later.
		const bool airRaidOnly = airRaidShortOfDefenses && !underVehicleAttack && !underInfantryAttack && !effectiveFrontlineThreat;

		// Ground defense level in points (user, as the Advanced AI): base by difficulty, per refinery,
		// per functional structure, with an expansion pending; half again after a ground attack and
		// more while attacks last; at most DefenseMaxValue. And the key assets each tower or gun
		// covers within CoverageDefenseCells: ToProtect structures, superweapons, captured tech and
		// cash buildings, refineries, yards and factories away from home, and the stones of the
		// offensive crawl; anti-air too while the enemy has aircraft or air bases (sneak raids). The
		// coverage comes on top of the level and of the defense budget, two towers per asset at most
		// past the level: a coverage that was never met stood 98 towers in one base (match #9885).
		// The anti-air cover counts every building a raid goes for (IsAirCoverAssetType) at home too,
		// AirCoverGunsPerAsset guns each, since aircraft fly over the ground towers to them.
		int functionalStructures = 0;
		std::vector<CellStruct> coverageAssets;
		// The reach of the ground cover of each of them (GroundCoverCellsFor).
		std::vector<int> coverageAssetCells;
		std::vector<CellStruct> airCoverAssets;
		std::vector<CellStruct> groundDefenseCells;
		std::vector<CellStruct> antiAirDefenseCells;

		{
			auto const& stones = GetMacroState(pHouse).OffensiveStones;

			for (auto const pOwnBld : pHouse->Buildings)
			{
				if (!pOwnBld || pOwnBld->InLimbo || !pOwnBld->IsAlive || !pOwnBld->Type || pOwnBld->Type->InvisibleInGame)
					continue;

				auto const pOT = pOwnBld->Type;
				const CellStruct bc = pOwnBld->GetMapCoords();

				if (pOT->IsBaseDefense)
				{
					if (IsTunnelNetworkBuilding(pOT))
						continue;

					const auto pW0 = pOT->GetWeapon(0) ? pOT->GetWeapon(0)->WeaponType : nullptr;
					const auto pW1 = pOT->GetWeapon(1) ? pOT->GetWeapon(1)->WeaponType : nullptr;
					const bool shootsUp = (pW0 && pW0->Projectile && pW0->Projectile->AA) || (pW1 && pW1->Projectile && pW1->Projectile->AA);
					const bool pureAA = pW0 && pW0->Projectile && pW0->Projectile->AA && !pW0->Projectile->AG;
					const bool armed = (pW0 && pW0->Damage > 0) || pOwnBld->Occupants.Count > 0 || pOwnBld->BunkerLinkedItem != nullptr;

					if (shootsUp)
						antiAirDefenseCells.push_back(bc);

					if (!pureAA && armed)
						groundDefenseCells.push_back(bc);

					continue;
				}

				if (pOT->Wall)
					continue;

				if (pOT->PowerBonus <= pOT->PowerDrain)
					functionalStructures++;

				const bool awayFromHome = homeRefCenter != CellStruct::Empty
					&& std::max(std::abs(bc.X - homeRefCenter.X), std::abs(bc.Y - homeRefCenter.Y)) > 16;
				const bool keyAsset = pOT->ToProtect || pOT->SuperWeapon != -1 || pOT->ProduceCashAmount > 0
					|| pOT->ProduceCashStartup > 0 || (pOT->Capturable && pOT->NeedsEngineer)
					|| (awayFromHome && (pOT->Refinery || pOT->ResourceGatherer || pOT->ConstructionYard || pOT->Factory != AbstractType::None))
					|| std::find(stones.begin(), stones.end(), pOwnBld->UniqueID) != stones.end();

				if (keyAsset)
				{
					coverageAssets.push_back(bc);
					coverageAssetCells.push_back(GroundCoverCellsFor(pOT));
				}

				if (keyAsset || IsAirCoverAssetType(pOT))
					airCoverAssets.push_back(bc);
			}
		}

		const bool enemyHasAircraft = enemyAirValue > 0 || knownEnemyAirBases > 0;
		const int airCoverGunsPerAsset = AirCoverGunsPerAsset(enemyAirValue);
		const int enemyGroundThreat = EnemyGroundThreatValue(pHouse);
		const int groundCoverTowersPerAsset = GroundCoverTowersPerAsset(enemyGroundThreat);
		int groundCoverageDeficit = 0;
		int antiAirCoverageDeficit = 0;

		for (size_t i = 0; i < coverageAssets.size(); ++i)
		{
			auto const& asset = coverageAssets[i];
			const int reach = coverageAssetCells[i];
			const auto towersNear = std::count_if(groundDefenseCells.begin(), groundDefenseCells.end(), [&asset, reach](const CellStruct& dc)
				{ return std::max(std::abs(asset.X - dc.X), std::abs(asset.Y - dc.Y)) <= reach; });

			if (towersNear < groundCoverTowersPerAsset)
				groundCoverageDeficit++;
		}

		if (enemyHasAircraft)
		{
			for (auto const& asset : airCoverAssets)
			{
				const auto gunsNear = std::count_if(antiAirDefenseCells.begin(), antiAirDefenseCells.end(), [&asset](const CellStruct& dc)
					{ return std::max(std::abs(asset.X - dc.X), std::abs(asset.Y - dc.Y)) <= CoverageDefenseCells; });

				if (gunsNear < airCoverGunsPerAsset)
					antiAirCoverageDeficit++;
			}
		}

		int optimalGroundDefenseValue = DefenseBaseValue[defenseDifficulty] + ownedRefineries * DefenseValuePerRefinery
			+ functionalStructures * DefenseValuePerStructure + (GetMacroState(pHouse).UncolonizedOreFields > 0 ? DefenseValueExpansionPending : 0);

		if (underVehicleAttack || underInfantryAttack)
			optimalGroundDefenseValue = optimalGroundDefenseValue * 3 / 2;

		if (wasRecentlyAttacked)
			optimalGroundDefenseValue += DefenseValueRecentAttack;

		// The level stops at DefenseMaxValue; the enemy forces that came at our base come on top of it
		// (GroundThreatValuePerPoint), as the anti-air adds the enemy air value.
		optimalGroundDefenseValue = std::min(optimalGroundDefenseValue, DefenseMaxValue[defenseDifficulty]) + enemyGroundThreat;
		// Towers still missing for that value while the threat stands (none without one).
		const int groundThreatDefenseDeficit = enemyGroundThreat > 0
			? (std::max(0, optimalGroundDefenseValue - ownedGroundDefenseValue) + GroundDefenseTypicalValue - 1) / GroundDefenseTypicalValue
			: 0;
		// The cover asks for its towers per asset at most past the level: with more, a house kept
		// building towers next to assets already covered while another went without (54 ground
		// towers, match #10523).
		const int coverageAssetCount = static_cast<int>(coverageAssets.size());
		const bool groundCoverageWanted = groundCoverageDeficit > 0
			&& totalGroundDefenses < optimalGroundDefenseValue / GroundDefenseTypicalValue + coverageAssetCount * groundCoverTowersPerAsset;
		const bool antiAirCoverageWanted = antiAirCoverageDeficit > 0
			&& totalAntiAirDefenses < wantedAirValue / airDefenseTypicalValue + static_cast<int>(airCoverAssets.size()) * airCoverGunsPerAsset;
		bool bestFillsAirDeficit = false;
		int bestAirDefenseIdx = -1;
		int bestAirDefenseScore = -32000;
		// Best defense the Island Hold yard still lacks (IslandHoldWants).
		int bestIslandHoldIdx = -1;
		int bestIslandHoldScore = -32000;
		// The second repair station the Forward Repair plan asks for.
		int bestForwardRepairIdx = -1;
		int bestForwardRepairScore = -32000;
		// The building a crossing asks for (crossingPrerequisites).
		int bestCrossingIdx = -1;
		int bestCrossingScore = -32000;
		// The next step of the tech progression floor (TechStepFloorFrames), and whether the yard
		// builds one now.
		int bestTechStepIdx = -1;
		int bestTechStepScore = -32000;
		bool techStepInProgress = false;
		// The buildings the yards of this house build now, read from the factories: GetPrimaryFactory
		// finds none for an AI house (inProgressBldIdx was none in all 8893 samples of 2026-11-20).
		std::vector<int> yardBuildIdxs;

		for (auto const pFactory : FactoryClass::Array)
		{
			if (pFactory && pFactory->Owner == pHouse && pFactory->Object && pFactory->Object->WhatAmI() == AbstractType::Building)
				yardBuildIdxs.push_back(static_cast<BuildingClass*>(pFactory->Object)->Type->ArrayIndex);
		}
		// The barracks an urgent capture site asks for (UpdateCaptureBarracks).
		int bestCaptureBarracksIdx = -1;
		int bestCaptureBarracksScore = -32000;

		// A raid short of guns with no defense that shoots up buildable yet: the buildings those
		// defenses still miss come ahead of routine picks.
		std::vector<int> airDefensePrerequisites;

		if (airRaidShortOfDefenses)
		{
			bool airDefenseBuildable = false;
			std::vector<int> missing;

			for (auto const pDefType : BuildingTypeClass::Array)
			{
				if (!pDefType || !pDefType->IsBaseDefense || (pDefType->TechLevel < 0 && !IsAIOnlyBuilding(pDefType))
					|| IsKeptFromAI(pDefType) || (ownHouseBit != 0u && !pDefType->InOwners(ownHouseBit)))
				{
					continue;
				}

				bool antiInf = false;
				bool antiArmor = false;
				bool antiAir = false;
				bool pureAA = false;
				bool unarmedBunker = false;
				GetDefenseCapabilities(pDefType, antiInf, antiArmor, antiAir, pureAA, unarmedBunker);

				if (!antiAir)
					continue;

				if (CanBuildUnderHumanRules(pHouse, pDefType))
				{
					airDefenseBuildable = true;

					break;
				}

				const std::vector<int> typeMissing = MissingTypePrerequisites(pHouse, pDefType);
				missing.insert(missing.end(), typeMissing.begin(), typeMissing.end());
			}

			if (!airDefenseBuildable)
				airDefensePrerequisites = std::move(missing);
		}

		// TEMP diagnostic: how far the best defense that shoots up got in this pass and with what score.
		int airDiagStage = 0;
		int airDiagScore = 0;
		const char* airDiagName = "none";
		auto noteAirDefense = [&](const BuildingTypeClass* pType, int stage, int stageScore)
		{
			if (stage > airDiagStage || (stage == airDiagStage && stageScore > airDiagScore))
			{
				airDiagStage = stage;
				airDiagScore = stageScore;
				airDiagName = pType->ID;
			}
		};

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

		// The answer of the scan below, kept MapScanKeepFrames per house.
		struct OreScan
		{
			int Serial { -1 };
			int Frame { -100000 };
			int Buildings { -1 };
			size_t Origins { 0 };
			CellStruct FirstOrigin { CellStruct::Empty };
			CellStruct Cell { CellStruct::Empty };
			int Distance { 9999 };
		};

		static std::array<OreScan, 32> oreScans;
		OreScan* const pOreScan = (pHouse->ArrayIndex >= 0 && pHouse->ArrayIndex < 32) ? &oreScans[pHouse->ArrayIndex] : nullptr;
		const CellStruct firstScanOrigin = scanOrigins.empty() ? CellStruct::Empty : scanOrigins[0];
		const bool oreScanKept = pOreScan && pOreScan->Serial == NeuralMatchSerial
			&& Unsorted::CurrentFrame >= pOreScan->Frame && Unsorted::CurrentFrame - pOreScan->Frame < MapScanKeepFrames
			&& pOreScan->Buildings == pHouse->Buildings.Count && pOreScan->Origins == scanOrigins.size()
			&& pOreScan->FirstOrigin == firstScanOrigin;

		for (const CellStruct& scanOrigin : scanOrigins)
		{
			if (scanOrigin == CellStruct::Empty)
				continue;

			// A kept answer skips the scan (no cell is within a negative radius).
			const int scanRad = oreScanKept ? -1 : ((&scanOrigin == &scanOrigins[0]) ? 78 : 42);
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

		if (oreScanKept)
		{
			nearestUncolonizedOreCell = pOreScan->Cell;
			minAnchorDistToUncolonizedOre = pOreScan->Distance;
		}
		else if (pOreScan)
		{
			*pOreScan = OreScan { NeuralMatchSerial, Unsorted::CurrentFrame, pHouse->Buildings.Count, scanOrigins.size(), firstScanOrigin,
				nearestUncolonizedOreCell, minAnchorDistToUncolonizedOre };
		}

		bool conYardWithNoRefineryInZone = false;
		CellStruct noRefineryYardCell = CellStruct::Empty;
		bool forwardConYardNeedsWarFactory = false;
		bool forwardConYardNeedsBarracks = false;
		// That ConYard stands on the land of a known enemy building (a beachhead): its factories come
		// before almost anything, they end the transport trips.
		bool beachheadNeedsFactories = false;
		bool forwardConYardNeedsDefense = false;
		// A ConYard of ours on another landmass with useful water close by and no shipyard of ours
		// within 22 cells: each conquered island gets its own shipyard (sea control there).
		bool islandYardNeedsShipyard = false;
		const int spawnHomeZone = (pHouse->BaseSpawnCell != CellStruct::Empty)
			? MapClass::Instance.GetMovementZoneType(pHouse->BaseSpawnCell, MovementZone::Normal, false)
			: -1;

		// Island Hold plan (UpdateStrategicPlans): a fresh island ConYard is fortified before its
		// economy exists, with anti-air too once enemy air has been seen. The plan says what the
		// yard still lacks; while placement finds no cell next to it the plan asks for nothing.
		auto const& holdMacro = GetMacroState(pHouse);
		const bool islandHoldNeedsGround = holdMacro.IslandHoldNeedsGround
			&& holdMacro.IslandHoldUntilFrame > Unsorted::CurrentFrame && holdMacro.IslandHoldCell != CellStruct::Empty
			&& Unsorted::CurrentFrame >= holdMacro.IslandHoldNoRoomUntilFrame;

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
			std::vector<CellStruct> oreSamplesNearCY;
			std::vector<CellStruct> treeCellsNearCY;
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
						{
							oreCellsNearCY++;
							oreSamplesNearCY.push_back(tc);
						}

						if (auto const pTerr = pC->GetTerrain(false))
						{
							if (pTerr->Type && pTerr->Type->SpawnsTiberium)
							{
								tiberiumTreesNearCY++;
								treeCellsNearCY.push_back(tc);
							}
						}
					}
				}
			}

			const bool hasOreNearCY = (oreCellsNearCY > 0 || tiberiumTreesNearCY > 0);
			const bool wantsSecondIslandRefinery = (refsNearCY == 1 && hasWarFacNearCY
				&& (tiberiumTreesNearCY >= 1 || oreCellsNearCY >= 8) && money >= 1400);

			// A yard asks for its refinery only for a real field within 24 cells of it (user, 2026-11-18):
			// RefineryNewFieldOreCells ore cells in a 7x7, or a spawner tree, not within 12 cells of a known
			// enemy building (as placement judges a field). Any ore sample did: the yard hub refinery stood
			// on the few cells left there, was recycled as exhausted and the yard asked again (one yard got
			// 6 refineries in a row on 0 to 7 ore cells, and a pillbox was sold to make room for one).
			bool hasRealFieldNearCY = false;

			if (hasOreNearCY && (!hasRefNearCY || wantsSecondIslandRefinery))
			{
				std::vector<CellStruct> enemyCellsNearCY;

				for (auto const pEB : BuildingClass::Array)
				{
					if (pEB && !pEB->InLimbo && pEB->IsAlive && pEB->Owner && pEB->Type && !pEB->Type->Insignificant
						&& !pEB->Type->InvisibleInGame && pEB->Owner != pHouse && !pHouse->IsAlliedWith(pEB->Owner)
						&& !pEB->Owner->IsNeutral() && IsEnemyBuildingKnown(pHouse, pEB))
					{
						const CellStruct ec = pEB->GetMapCoords();

						if (std::max(std::abs(cyc.X - ec.X), std::abs(cyc.Y - ec.Y)) <= 24 + 12)
							enemyCellsNearCY.push_back(ec);
					}
				}

				auto const awayFromEnemy = [&enemyCellsNearCY](const CellStruct& cell)
				{
					return std::none_of(enemyCellsNearCY.begin(), enemyCellsNearCY.end(), [&cell](const CellStruct& ec)
						{ return std::max(std::abs(cell.X - ec.X), std::abs(cell.Y - ec.Y)) <= 12; });
				};

				hasRealFieldNearCY = std::any_of(treeCellsNearCY.begin(), treeCellsNearCY.end(), awayFromEnemy);

				for (size_t si = 0; !hasRealFieldNearCY && si < oreSamplesNearCY.size(); ++si)
				{
					const CellStruct oc = oreSamplesNearCY[si];
					int fieldOreCells = 0;

					for (int fy = -3; fy <= 3; ++fy)
					{
						for (int fx = -3; fx <= 3; ++fx)
						{
							auto const pFieldCell = MapClass::Instance.TryGetCellAt(CellStruct { static_cast<short>(oc.X + fx), static_cast<short>(oc.Y + fy) });

							if (pFieldCell && pFieldCell->GetContainedTiberiumValue() > 0)
								fieldOreCells++;
						}
					}

					hasRealFieldNearCY = fieldOreCells >= RefineryNewFieldOreCells && awayFromEnemy(oc);
				}
			}

			bool enemyNearCY = false;
			bool enemyOnCYLand = false;
			for (auto const pEB : BuildingClass::Array)
			{
				if (!pEB || pEB->InLimbo || !pEB->IsAlive || !pEB->Owner || !pEB->Type || pEB->Owner == pHouse
					|| pHouse->IsAlliedWith(pEB->Owner) || !IsEnemyBuildingKnown(pHouse, pEB) || pEB->Owner->IsNeutral())
				{
					continue;
				}

				const CellStruct ec = pEB->GetMapCoords();
				const bool sameLand = MapClass::Instance.GetMovementZoneType(ec, MovementZone::Normal, pEB->OnBridge) == cyz;
				if (std::max(std::abs(cyc.X - ec.X), std::abs(cyc.Y - ec.Y)) <= 36 || sameLand)
				{
					enemyNearCY = true;
					enemyOnCYLand = sameLand && spawnHomeZone >= 0 && cyz != spawnHomeZone;
					break;
				}
			}

			if (hasRealFieldNearCY && (!hasRefNearCY || wantsSecondIslandRefinery))
			{
				if (!conYardWithNoRefineryInZone)
					noRefineryYardCell = cyc;

				conYardWithNoRefineryInZone = true;
			}

			if (enemyNearCY || (spawnHomeZone >= 0 && cyz != spawnHomeZone) || !hasOreNearCY)
			{
				if (!hasWarFacNearCY && ownedRefineries >= 1)
					forwardConYardNeedsWarFactory = true;

				if (!hasBarracksNearCY && ownedRefineries >= 1)
					forwardConYardNeedsBarracks = true;

				if (enemyOnCYLand && (!hasWarFacNearCY || !hasBarracksNearCY))
					beachheadNeedsFactories = true;

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
		}

		if (islandHoldNeedsGround)
			forwardConYardNeedsDefense = true;

		const bool reachedNewOreField
			= (conYardWithNoRefineryInZone
				|| (nearestUncolonizedOreCell != CellStruct::Empty && minAnchorDistToUncolonizedOre <= 9))
			&& ownedRefineries >= 1;

		// Little room first goes to the minimum of everything (user): while a placement just failed
		// and the core base still lacks a refinery, a barracks, a war factory, a tech center or power,
		// the copies that follow demand with no cap (refineries, silos, shipyards, airfields) wait.
		const bool minimumFirst = GetMacroState(pHouse).ConsecutivePlacementFailures >= 1
			&& (ownedRefineries == 0 || ownedBarracks == 0 || ownedWarFactories == 0 || !hasTechCenter || pHouse->HasLowPower());

		// Permanent refineries follow the fields we hold (user): each held ore block with ore takes
		// one, two from RefineryOreCellsPerExtra ore cells and three from twice that (the Advanced AI
		// allows up to 3 by the ore around), and a newly reached field one more; a fixed home cap of 4
		// to 6 kept a house from taking every field it reached. Past it a refinery is only built as a
		// Harvester Recovery refinery (replaced in place or sold once its FreeUnit spawns), never
		// kept: a permanent one past the cap raised the harvester target (2 per refinery), which asked
		// for another recovery refinery, and the chain filled the map with refineries far from any
		// field (31 on Majestic Canyon).
		int fieldRefineryRoom = 0;
		{
			auto const heldBlocks = GetHeldOreBlocks(pHouse);

			for (auto const& field : GetOreFields())
			{
				if (field.OreCells > 0 && std::binary_search(heldBlocks.begin(), heldBlocks.end(), field.BlockKey))
					fieldRefineryRoom += std::min(1 + field.OreCells / RefineryOreCellsPerExtra, 3);
			}
		}

		// A newly reached field always gets its refinery (user, 2026-11-18: expanding like a virus wins
		// in the long run, capping refineries is a synonym of future defeat). The held fields' room
		// counts only fields that already have a refinery, so a house that owns more than that room
		// (recovery refineries, home fields running dry) had no room left for the new field: on Fluddle
		// GDI held the defeated CABAL's island with a yard, its factories and 8 defenses and never a
		// refinery. One refinery past what we own every RefineryNewFieldFrames at most, and only for a
		// yard without a refinery or a real field (RefineryNewFieldOreCells in the 7x7 of the free ore
		// cell, as placement judges a field), never again for one whose refinery went elsewhere
		// (MissedNewFields, PlaceCompletedBuilding): any lone ore cell counted as a new field, placement
		// aims at fields only, so the refinery landed away, the cell stayed free and the slot opened
		// again every 3000 frames (Campy Festival: 5 refineries to 19 with the cap at 3 to 8, 9 of 22
		// on fewer than 5 ore cells).
		const int heldRefineryCap = std::max(1, fieldRefineryRoom);
		CellStruct slotFieldCell = CellStruct::Empty;
		int slotFieldReach = 0;

		if (conYardWithNoRefineryInZone && noRefineryYardCell != CellStruct::Empty)
		{
			slotFieldCell = noRefineryYardCell;
			slotFieldReach = RefineryMissedYardCells;
		}
		else if (nearestUncolonizedOreCell != CellStruct::Empty && minAnchorDistToUncolonizedOre <= 9)
		{
			int fieldOreCells = 0;

			for (int fy = -3; fy <= 3; ++fy)
			{
				for (int fx = -3; fx <= 3; ++fx)
				{
					auto const pFieldCell = MapClass::Instance.TryGetCellAt(CellStruct { static_cast<short>(nearestUncolonizedOreCell.X + fx),
						static_cast<short>(nearestUncolonizedOreCell.Y + fy) });

					if (pFieldCell && pFieldCell->GetContainedTiberiumValue() > 0)
						fieldOreCells++;
				}
			}

			if (fieldOreCells >= RefineryNewFieldOreCells)
			{
				slotFieldCell = nearestUncolonizedOreCell;
				slotFieldReach = RefineryMissedFieldCells;
			}
		}

		auto& slotMacro = GetMacroState(pHouse);
		auto& missedFields = slotMacro.MissedNewFields;
		missedFields.erase(std::remove_if(missedFields.begin(), missedFields.end(), [](const std::pair<CellStruct, int>& missed)
			{ return Unsorted::CurrentFrame - missed.second >= RefineryMissedFieldFrames || Unsorted::CurrentFrame < missed.second; }),
			missedFields.end());
		const bool slotFieldMissed = slotFieldCell != CellStruct::Empty
			&& std::any_of(missedFields.begin(), missedFields.end(), [&slotFieldCell](const std::pair<CellStruct, int>& missed)
				{ return std::max(std::abs(missed.first.X - slotFieldCell.X), std::abs(missed.first.Y - slotFieldCell.Y)) <= RefineryMissedSameFieldCells; });
		const bool newFieldRefinerySlot = reachedNewOreField && slotFieldCell != CellStruct::Empty && !slotFieldMissed
			&& Unsorted::CurrentFrame - slotMacro.LastRefineryPlacedFrame >= RefineryNewFieldFrames;

		// The field this slot is for: the next refinery placed is judged against it.
		if (newFieldRefinerySlot)
		{
			slotMacro.NewFieldSlotCell = slotFieldCell;
			slotMacro.NewFieldSlotReach = slotFieldReach;
			slotMacro.NewFieldSlotFrame = Unsorted::CurrentFrame;
		}
		const int fieldRefineryCap = newFieldRefinerySlot ? std::max(heldRefineryCap, ownedRefineries) + 1
			: heldRefineryCap + (reachedNewOreField ? 1 : 0);
		const bool permanentRefineryCapReached = ownedRefineries >= fieldRefineryCap;

		// TEMP diagnostic (expansion refineries, 2026-11-18): the refinery cap of each house.
		if (IsAITickWindow(4500, 0))
		{
			Debug::Log("[NeuralAI] House %d refinery cap DIAG: %d refineries, held field room %d, cap %d (new field %d: yard without refinery %d, "
				"anchor %d cells from free ore, slot %d, real field %d, missed fields %d), yards %d, frame %d.\n", pHouse->ArrayIndex,
				ownedRefineries, fieldRefineryRoom, fieldRefineryCap, reachedNewOreField ? 1 : 0, conYardWithNoRefineryInZone ? 1 : 0,
				minAnchorDistToUncolonizedOre, newFieldRefinerySlot ? 1 : 0, slotFieldCell != CellStruct::Empty ? 1 : 0,
				static_cast<int>(missedFields.size()), pHouse->ConYards.Count, Unsorted::CurrentFrame);
		}

		// A newly reached field without its refinery (cap not reached): the cheapest buildable
		// refinery is an economic investment the economy hold saves for (UpdateEconomyCrisis).
		{
			int refineryCost = 0;

			if (reachedNewOreField && !permanentRefineryCapReached)
			{
				for (auto const pType : BuildingTypeClass::Array)
				{
					if (pType && pType->Refinery && !pType->ResourceGatherer && CanBuildUnderHumanRules(pHouse, pType))
					{
						const int cost = pType->GetActualCost(pHouse);
						refineryCost = refineryCost == 0 ? cost : std::min(refineryCost, cost);
					}
				}
			}

			GetMacroState(pHouse).RefinerySaveTarget = refineryCost;
		}

		// A beachhead is any group of our buildings we can build around (no ConYard needed there) on
		// another landmass where a known enemy building stands: one without a war factory or a
		// barracks of ours within 18 cells gets them first, as a forward ConYard does (placement
		// anchors them at such a hub).
		if (ownedRefineries >= 1 && spawnHomeZone >= 0 && (!beachheadNeedsFactories || !forwardConYardNeedsWarFactory || !forwardConYardNeedsBarracks))
		{
			std::vector<int> enemyZones;

			for (auto const pEnemyBld : GetKnownEnemyBuildingCache(pHouse).Buildings)
			{
				if (pEnemyBld && pEnemyBld->IsAlive)
				{
					const int z = ObjectZone(pEnemyBld, MovementZone::Normal);

					if (z > 1 && z != spawnHomeZone && std::find(enemyZones.begin(), enemyZones.end(), z) == enemyZones.end())
						enemyZones.push_back(z);
				}
			}

			for (auto const pHub : pHouse->Buildings)
			{
				if (enemyZones.empty())
					break;

				if (!pHub || !pHub->IsAlive || pHub->InLimbo || !pHub->Type || !pHub->Type->BaseNormal || pHub->Type->Insignificant)
					continue;

				const int hubZone = ObjectZone(pHub, MovementZone::Normal);

				if (std::find(enemyZones.begin(), enemyZones.end(), hubZone) == enemyZones.end())
					continue;

				bool warFactoryNear = false;
				bool barracksNear = false;
				const CellStruct hc = pHub->GetMapCoords();

				for (auto const pOther : pHouse->Buildings)
				{
					if (!pOther || !pOther->IsAlive || pOther->InLimbo || !pOther->Type || pOther->Type->Naval
						|| pOther->GetMapCoords().DistanceFrom(hc) > 18.0 || ObjectZone(pOther, MovementZone::Normal) != hubZone)
					{
						continue;
					}

					warFactoryNear |= pOther->Type->Factory == AbstractType::UnitType;
					barracksNear |= pOther->Type->Factory == AbstractType::InfantryType;
				}

				if (!warFactoryNear)
					forwardConYardNeedsWarFactory = true;

				if (!barracksNear)
					forwardConYardNeedsBarracks = true;

				if (!warFactoryNear || !barracksNear)
				{
					beachheadNeedsFactories = true;
					break;
				}
			}
		}

		// Capture barracks (UpdateCaptureBarracks): a tech our engineers would take stands on a land with
		// buildings of ours and no barracks of ours; its barracks is wanted like an outpost's.
		const bool captureSiteNeedsBarracks = IsCaptureSiteWaiting(pHouse);
		const bool captureSiteUrgent = captureSiteNeedsBarracks && IsCaptureSiteUrgent(pHouse);

		// Outposts that may get a factory past the difficulty cap (SellFactoriesOverDifficultyCap
		// then sells another of its type): a forward or island ConYard without one, or the front of
		// an offensive crawl (newest forward stone) without one within 15 cells.
		std::array<int, 3> factoryCategoryCaps {};
		std::array<int, 3> factoryCategoryCounts {};

		for (int category = 0; category < 3; ++category)
		{
			factoryCategoryCaps[category] = GetFactoryCategoryCap(pHouse, category);
			factoryCategoryCounts[category] = CountFactoriesInCategory(pHouse, category);
		}

		bool frontNeedsBarracks = false;
		bool frontNeedsWarFactory = false;

		if (const CellStruct stoneCell = GetNewestOffensiveStoneCell(pHouse); stoneCell != CellStruct::Empty)
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

		const bool needsSteppingStoneCrawlToOre = (GetMacroState(pHouse).ConsecutivePlacementFailures < 2
			&& nearestUncolonizedOreCell != CellStruct::Empty && minAnchorDistToUncolonizedOre > 9
			&& minAnchorDistToUncolonizedOre <= 60 && hasCoreEconomyAndProduction
			&& (features[19] < 32 || ownedRefineries < 3 || scores[34] > 100 || Unsorted::CurrentFrame > 2100));

		// TEMP diagnostic (crawl DIAG, user approved 2026-11-19: in 20 matches of a31220939 the nearest free
		// ore stood 10 to 20 cells from every anchor in 18% of the refinery cap lines and 38% of those were
		// no closer 9000 frames later): every 4500 frames, for a house whose nearest free ore is 10 to 60
		// cells from an anchor, the gates of the stepping stone crawl, what the yard builds now and the last
		// building that crawled toward ore (crawl DIAG (placement)).
		if (IsAITickWindow(4500, 0) && nearestUncolonizedOreCell != CellStruct::Empty && minAnchorDistToUncolonizedOre > 9
			&& minAnchorDistToUncolonizedOre <= 60)
		{
			auto const& crawlMacro = GetMacroState(pHouse);
			FactoryClass* const pYardFactory = pHouse->GetPrimaryFactory(AbstractType::BuildingType, false, BuildCat::DontCare);
			TechnoTypeClass* const pBuildingNow = (pYardFactory && pYardFactory->Object) ? pYardFactory->Object->GetTechnoType() : nullptr;
			BuildingTypeClass* const pLastCrawlType = (crawlMacro.LastCrawlType >= 0 && crawlMacro.LastCrawlType < BuildingTypeClass::Array.Count)
				? BuildingTypeClass::Array.GetItem(crawlMacro.LastCrawlType)
				: nullptr;
			Debug::Log("[NeuralAI] House %d crawl DIAG (production): free ore (%d,%d) %d cells from an anchor, needs stones %d (placement failures %d, "
				"core economy %d, refineries %d, features19 %d, slot34 %d), money %d (stone bonus from 1400), power %d of %d (surplus %d, cushion %d), "
				"building %s, last crawl %s %d frames ago from %d to %d cells, frame %d.\n",
				pHouse->ArrayIndex, nearestUncolonizedOreCell.X, nearestUncolonizedOreCell.Y, minAnchorDistToUncolonizedOre,
				needsSteppingStoneCrawlToOre ? 1 : 0, crawlMacro.ConsecutivePlacementFailures, hasCoreEconomyAndProduction ? 1 : 0, ownedRefineries,
				features[19], scores[34], money, pHouse->PowerDrain, pHouse->PowerOutput, realPowerSurplus, requiredPowerCushion,
				pBuildingNow ? pBuildingNow->ID : "nothing", pLastCrawlType ? pLastCrawlType->ID : "none",
				crawlMacro.LastCrawlFrame >= 0 ? Unsorted::CurrentFrame - crawlMacro.LastCrawlFrame : -1, crawlMacro.LastCrawlBefore,
				crawlMacro.LastCrawlAfter, Unsorted::CurrentFrame);
		}

		// Offensive bank (HasOffensiveBank) with a known enemy base on the land of one of our ConYards
		// (our own land, or a beachhead on the enemy island): the house keeps building stepping stones,
		// which the placement sends toward the enemy (offensive crawl), up to 16 forward stones alive.
		// Across the water only when none of our ground factories stands on the land of a known enemy
		// building: with a war factory or barracks next to the enemy the war goes on by land.
		{
			bool factoryOnEnemyLand = false;

			if (features[21] > 0)
			{
				std::vector<int> enemyZones;

				for (auto const pEnemyBld : GetKnownEnemyBuildingCache(pHouse).Buildings)
				{
					if (pEnemyBld && pEnemyBld->IsAlive)
					{
						const int z = ObjectZone(pEnemyBld, MovementZone::Normal);

						if (z > 1 && std::find(enemyZones.begin(), enemyZones.end(), z) == enemyZones.end())
							enemyZones.push_back(z);
					}
				}

				for (auto const pOwnBld : pHouse->Buildings)
				{
					if (pOwnBld && pOwnBld->IsAlive && !pOwnBld->InLimbo && pOwnBld->Type && !pOwnBld->Type->Naval
						&& (pOwnBld->Type->Factory == AbstractType::UnitType || pOwnBld->Type->Factory == AbstractType::InfantryType)
						&& std::find(enemyZones.begin(), enemyZones.end(), ObjectZone(pOwnBld, MovementZone::Normal)) != enemyZones.end())
					{
						factoryOnEnemyLand = true;
						break;
					}
				}
			}

			GetMacroState(pHouse).EnemyAcrossWater = features[21] > 0 && !factoryOnEnemyLand;
		}
		bool bankOffensiveCrawl = HasOffensiveBank(pHouse) && hasCoreEconomyAndProduction
			&& GetMacroState(pHouse).ConsecutivePlacementFailures < 2 && GetMacroState(pHouse).OffensiveStones.size() < 16;

		if (bankOffensiveCrawl && features[21] > 0)
		{
			bool enemyOnYardLand = false;

			for (int i = 0; i < pHouse->ConYards.Count && !enemyOnYardLand; ++i)
			{
				auto const pYard = pHouse->ConYards.GetItem(i);

				if (!pYard || !pYard->IsAlive || pYard->InLimbo)
					continue;

				const int yardZone = ObjectZone(pYard, MovementZone::Normal);

				for (auto const pEnemyBld : BuildingClass::Array)
				{
					if (pEnemyBld && pEnemyBld->IsAlive && !pEnemyBld->InLimbo && pEnemyBld->Owner && pEnemyBld->Owner != pHouse
						&& !pHouse->IsAlliedWith(pEnemyBld->Owner) && !pEnemyBld->Owner->IsNeutral()
						&& IsEnemyBuildingKnown(pHouse, pEnemyBld) && ObjectZone(pEnemyBld, MovementZone::Normal) == yardZone)
					{
						enemyOnYardLand = true;
						break;
					}
				}
			}

			bankOffensiveCrawl = enemyOnYardLand;
		}

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

		// Harvester recovery through a refinery's FreeUnit (sell-and-replace or a temporary one) up to
		// one harvester per refinery; the second only while the war factory is busy with the army and
		// at most every SecondHarvesterRecoveryFrames (wantsSecondHarvesterViaRefinery), else the war
		// factory builds it (economyUnitCap). Aiming at two every 1800 frames kept the ConYard selling
		// and rebuilding refineries all match (30 times in one test house), so radar, tech center and
		// defenses never got their turn.
		// Not while broke (UpdateEconomyCrisis): selling a working refinery to rebuild it splits the
		// last credits between the refinery and the harvester the war factory builds.
		// The second one (user, 2026-11-18: the FreeUnit takes no war factory time): while the war
		// factory builds a unit that gathers nothing and the income is not falling (a thinning field
		// wants no more harvesters).
		bool warFactoryBuildsArmy = false;

		if (auto const pArmyFactory = pHouse->GetPrimaryFactory(AbstractType::UnitType, false, BuildCat::DontCare))
		{
			if (auto const pQueued = abstract_cast<UnitClass*>(pArmyFactory->Object))
				warFactoryBuildsArmy = pQueued->Type && !pQueued->Type->Harvester && !pQueued->Type->ResourceGatherer;
		}

		const bool wantsSecondHarvesterViaRefinery = warFactoryBuildsArmy && !GetMacroState(pHouse).IncomeFalling
			&& ownedHarvestersCount >= staticRefineriesCount && ownedHarvestersCount < 2 * staticRefineriesCount
			&& Unsorted::CurrentFrame - GetMacroState(pHouse).LastHarvesterRecoveryFrame >= SecondHarvesterRecoveryFrames
			&& features[19] > 0 && money >= 900;
		const bool wantsHarvesterRecoveryViaRefinery = (pHouse->ConYards.Count >= 1 && staticRefineriesCount >= 1
			&& !GetMacroState(pHouse).BrokeHoldActive
			&& Unsorted::CurrentFrame >= GetMacroState(pHouse).NextHarvesterRecoveryFrame
			&& GetMacroState(pHouse).TemporaryRecoveryRefineryPlacedFrame.empty()
			&& (orphanRefineryZoneNeedsReplacement >= 0 || wantsSecondHarvesterViaRefinery
				|| (ownedHarvestersCount < staticRefineriesCount
					&& (ownedHarvestersCount == 0 || ownedWarFactories >= 1) && features[19] > 0
					&& (money >= 900 || ownedHarvestersCount == 0))));

		// TEMP diagnostic (income forecast, 2026-11-18): why the second harvester trick fires or not (once
		// in 8 matches).
		if (IsAITickWindow(4500, 0) && staticRefineriesCount >= 1)
		{
			Debug::Log("[NeuralAI] House %d second harvester DIAG: war factory builds army %d, harvesters %d at %d refineries, "
				"%d frames since the last recovery, money %d, falling %d, broke %d, next recovery in %d, temporary pending %d, ore %d, "
				"wants second %d, wants recovery %d, frame %d.\n", pHouse->ArrayIndex, warFactoryBuildsArmy ? 1 : 0, ownedHarvestersCount,
				staticRefineriesCount, Unsorted::CurrentFrame - GetMacroState(pHouse).LastHarvesterRecoveryFrame, money,
				GetMacroState(pHouse).IncomeFalling ? 1 : 0, GetMacroState(pHouse).BrokeHoldActive ? 1 : 0,
				std::max(0, GetMacroState(pHouse).NextHarvesterRecoveryFrame - Unsorted::CurrentFrame),
				static_cast<int>(GetMacroState(pHouse).TemporaryRecoveryRefineryPlacedFrame.size()), static_cast<int>(features[19]),
				wantsSecondHarvesterViaRefinery ? 1 : 0, wantsHarvesterRecoveryViaRefinery ? 1 : 0, Unsorted::CurrentFrame);
		}
		const bool hasActiveBuildingIncome = (ownedCashBuildings >= 1)
			|| (((ownedRefineries >= 1 && ownedHarvestersCount >= 1) || ownedSlaveMinerBuildings >= 1)
				&& (features[19] > 0 || pHouse->OwnedTiberium.GetTotalAmount() > 0));

		// Team games: an ally building under attack now (an armed enemy in our sight within 8 cells,
		// the building below 80%) while our own base is not (no armed enemy in sight within 16 cells
		// of any ConYard of ours): armed ground defenses get a bonus and placement puts them there
		// (AllyDefenseCell), while we have fewer than 2 within 10 cells. Not within 15 cells of a
		// ConYard of ours: there our own defenses already cover both bases.
		CellStruct allyDefenseCell = CellStruct::Empty;
		const bool ownYardsQuiet = std::none_of(pHouse->ConYards.begin(), pHouse->ConYards.end(), [pHouse](BuildingClass* pYard)
			{
				return pYard && VisibleEnemyThreatNear(pHouse, pYard->GetMapCoords(), 16) > 0;
			});

		if (GameModeOptionsClass::Instance.BuildOffAlly && pHouse->ConYards.Count > 0 && ownYardsQuiet
			&& !frontlineThreatActive && money >= 1500)
		{
			int bestThreat = 0;

			for (auto const pAllyBld : BuildingClass::Array)
			{
				if (!pAllyBld || !pAllyBld->IsAlive || pAllyBld->InLimbo || !pAllyBld->Type || !pAllyBld->Owner
					|| pAllyBld->Owner == pHouse || !pHouse->IsAlliedWith(pAllyBld->Owner) || pAllyBld->Owner->IsNeutral()
					|| !pAllyBld->Type->EligibileForAllyBuilding || pAllyBld->GetHealthPercentage() >= 0.80)
				{
					continue;
				}

				const CellStruct bc = pAllyBld->GetMapCoords();

				if (std::any_of(pHouse->ConYards.begin(), pHouse->ConYards.end(),
						[&bc](BuildingClass* pYard) { return pYard && pYard->GetMapCoords().DistanceFrom(bc) < 15.0; }))
				{
					continue;
				}

				const int threat = VisibleEnemyThreatNear(pHouse, bc, 8);

				if (threat <= bestThreat)
					continue;

				const int ownDefensesThere = static_cast<int>(std::count_if(pHouse->Buildings.begin(), pHouse->Buildings.end(),
					[&bc](BuildingClass* pOwn)
					{
						return pOwn && pOwn->IsAlive && !pOwn->InLimbo && pOwn->Type && pOwn->Type->IsBaseDefense
							&& pOwn->GetMapCoords().DistanceFrom(bc) <= 10.0;
					}));

				if (ownDefensesThere >= 2)
					continue;

				bestThreat = threat;
				allyDefenseCell = bc;
			}
		}

		// The spot stays valid 1800 frames after the threat was last seen: the defense chosen for it
		// takes a while to build and must still go there once ready.
		if (allyDefenseCell != CellStruct::Empty)
		{
			GetMacroState(pHouse).AllyDefenseCell = allyDefenseCell;
			GetMacroState(pHouse).AllyDefenseUntilFrame = Unsorted::CurrentFrame + 1800;
		}
		else if (GetMacroState(pHouse).AllyDefenseUntilFrame <= Unsorted::CurrentFrame)
		{
			GetMacroState(pHouse).AllyDefenseCell = CellStruct::Empty;
			GetMacroState(pHouse).AllyDefenseUntilFrame = -1;
		}

		// Expansion MCV prerequisites: a house that wants to expand (slot 30, free ore fields) but
		// cannot build any MCV yet is often missing one building of the MCV's Prerequisite list
		// (YR asks for a service depot besides the war factory). Those buildings get a bonus. An
		// island expansion due (UpdateIslandOreOutlook) asks for them whatever slot 30 and the bank
		// say: its houses live on the trickle of income and the depot never came (Heisenberg repro).
		std::vector<int> mcvPrerequisites;

		// With no yard left the MCV's missing buildings come first too (user approved, 2026-11-21: as in Red
		// Alert 3, a house with deployers rebuilds what its MCV needs, then the MCV and a yard): only a
		// deployer brings them then (the source choice after this pick).
		if (pHouse->NumConYards == 0
			|| (pHouse->NumConYards >= 1 && pHouse->NumConYards < 6
				&& ((scores[30] >= 450 && money >= 2000 && GetMacroState(pHouse).UncolonizedOreFields > 0)
					|| GetMacroState(pHouse).IslandExpansionUrgent)))
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

				// Human rules, as the MCV production itself: the engine's AI check let an MCV pass
				// without its service depot, so the depot was never asked for.
				if (CanBuildUnderHumanRules(pHouse, pMcvType))
				{
					mcvBuildable = true;
					break;
				}

				const std::vector<int> typeMissing = MissingTypePrerequisites(pHouse, pMcvType);
				missing.insert(missing.end(), typeMissing.begin(), typeMissing.end());
			}

			if (!mcvBuildable)
				mcvPrerequisites = std::move(missing);
		}

		// A crossing our units wait for that only a transport we cannot build yet makes (NoteCrossingNeed):
		// the buildings that transport still lacks get a bonus, those of the one that lacks the fewest.
		// A missing building we cannot build yet brings its own missing ones (a tech center that needs a
		// radar first).
		std::vector<int> crossingPrerequisites;

		if (auto& crossingMacro = GetMacroState(pHouse); crossingMacro.CrossingNeedFrame >= 0
			&& Unsorted::CurrentFrame >= crossingMacro.CrossingNeedFrame
			&& Unsorted::CurrentFrame - crossingMacro.CrossingNeedFrame <= CrossingNeedFrames && !crossingMacro.CrossingNeedTypes.empty())
		{
			bool crossingBuildable = false;
			size_t fewestMissing = SIZE_MAX;

			for (auto const pNeedType : crossingMacro.CrossingNeedTypes)
			{
				auto const pType = const_cast<TechnoTypeClass*>(pNeedType);

				if (CanBuildUnderHumanRules(pHouse, pType))
				{
					crossingBuildable = true;
					break;
				}

				if (!CanEverMeetPrerequisites(pHouse, pType))
					continue;

				std::vector<int> missing = MissingTypePrerequisites(pHouse, pType);
				const size_t direct = missing.size();

				for (size_t i = 0; i < direct; ++i)
				{
					auto const pMissing = missing[i] >= 0 && missing[i] < BuildingTypeClass::Array.Count
						? BuildingTypeClass::Array.GetItem(missing[i]) : nullptr;

					if (pMissing && !CanBuildUnderHumanRules(pHouse, pMissing))
					{
						const std::vector<int> deeper = MissingTypePrerequisites(pHouse, pMissing);
						missing.insert(missing.end(), deeper.begin(), deeper.end());
					}
				}

				if (!missing.empty() && missing.size() < fewestMissing)
				{
					fewestMissing = missing.size();
					crossingPrerequisites = std::move(missing);
				}
			}

			if (crossingBuildable)
				crossingPrerequisites.clear();
		}

		// The island expansion's saving hold lets these buildings through (UpdateEconomyCrisis).
		{
			auto& islandPrereqs = GetMacroState(pHouse).IslandMcvPrerequisites;
			islandPrereqs.clear();

			if (GetMacroState(pHouse).IslandExpansionUrgent)
			{
				std::copy_if(mcvPrerequisites.begin(), mcvPrerequisites.end(), std::back_inserter(islandPrereqs),
					[](int idx) { return idx >= 0 && idx < BuildingTypeClass::Array.Count; });
			}
		}

		// Opening order of a player: a refinery or two, then the war factory. Without one, and with a
		// ground war factory buildable now, the first war factory is pushed and further refineries
		// wait (one Soviet test house built six refineries, most failing to place, before its first
		// war factory at frame 24000+).
		bool groundWarFactoryBuildableNow = false;

		if (ownedWarFactories == 0)
		{
			// A war factory whose placement just failed (in its cooldown) holds nothing back: the
			// towers and refineries that wait for it would wait for a factory that cannot stand.
			auto const& placementCooldowns = GetMacroState(pHouse).BuildingPlacementCooldownUntilFrame;

			for (auto const pFactoryType : BuildingTypeClass::Array)
			{
				if (pFactoryType && pFactoryType->Factory == AbstractType::UnitType && !pFactoryType->Naval
					&& !pFactoryType->UndeploysInto && CanBuildUnderHumanRules(pHouse, pFactoryType))
				{
					auto const cooldown = placementCooldowns.find(pFactoryType->ArrayIndex);

					if (cooldown != placementCooldowns.end() && Unsorted::CurrentFrame < cooldown->second)
						continue;

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

		// Buildings a deployer of ours can bring (NeuralAI.MobileFactory.inl; user approved, 2026-11-21,
		// «Apruebo el plan»): of the deployers our vehicle factories can build now the cheapest of each
		// building, a mobile factory only with a spot waiting for it (HouseWantsMobileFactory). Such a
		// building is a candidate of this pick as one the yard builds, and after the pick the yard or a
		// deployer brings it. One a deployer brings already (asked for, or on its way) waits for it when one
		// is enough (a factory, a tech or support building); the yard may add a refinery, a power plant or
		// a defense meanwhile.
		std::unordered_map<int, UnitTypeClass*> deployerOfBuilding;
		std::vector<int> buildingsOnTheWay;
		int yardBestIdx = -1;
		int yardBestScore = INT_MIN;
		int yardSecondIdx = -1;
		int yardSecondScore = INT_MIN;

		for (auto const& build : GetMacroState(pHouse).DeployerBuilds)
			buildingsOnTheWay.push_back(build.BuildingType);

		for (auto const& [uid, order] : GetMacroState(pHouse).DeployOrders)
			buildingsOnTheWay.push_back(order.BuildingType);

		for (auto const pDeployerType : UnitTypeClass::Array)
		{
			if (!IsBaseDeployerUnit(pDeployerType) || pDeployerType->Naval || !CanBuildUnderHumanRules(pHouse, pDeployerType)
				|| (IsMobileFactoryUnit(pDeployerType) && !HouseWantsMobileFactory(pHouse, pDeployerType)))
			{
				continue;
			}

			auto& pCheapest = deployerOfBuilding[pDeployerType->DeploysInto->ArrayIndex];

			if (!pCheapest || pDeployerType->GetActualCost(pHouse) < pCheapest->GetActualCost(pHouse))
				pCheapest = pDeployerType;
		}

		auto const isOnTheWay = [&buildingsOnTheWay](int buildingIdx)
		{
			return std::find(buildingsOnTheWay.begin(), buildingsOnTheWay.end(), buildingIdx) != buildingsOnTheWay.end();
		};

		for (auto const pBld : BuildingTypeClass::Array)
		{
			if (!pBld || pBld->Wall || pBld->FirestormWall || pBld->Gate || pBld->LaserFence || pBld->LaserFencePost
				|| pBld->ToTile || !pBld->Selectable)
			{
				continue;
			}

			if (HouseExt::IsDisabledFromShell(pHouse, pBld))
				continue;

			const bool yardBuildsIt = CanBuildUnderHumanRules(pHouse, pBld);

			if (!yardBuildsIt && !deployerOfBuilding.contains(pBld->ArrayIndex))
				continue;

			if (isOnTheWay(pBld->ArrayIndex))
			{
				const DeployerRole onTheWayRole = DeployedBuildingRole(pBld);

				if (!yardBuildsIt || (onTheWayRole != DeployerRole::Refinery && onTheWayRole != DeployerRole::Power
					&& onTheWayRole != DeployerRole::Defense))
				{
					continue;
				}
			}

			auto const& cooldownMap = GetMacroState(pHouse).BuildingPlacementCooldownUntilFrame;
			auto const cdIt = cooldownMap.find(pBld->ArrayIndex);
			if (cdIt != cooldownMap.end() && Unsorted::CurrentFrame < cdIt->second)
				continue;

			const bool isBldInProgress = (pBld->ArrayIndex == inProgressBldIdx || pBld->ArrayIndex == inProgressDefIdx);
			const bool isRefineryOrSlaveMiner = pBld->Refinery || pBld->ResourceGatherer || pBld->ResourceDestination;

			// Dry economy (UpdateEconomyCrisis): no refinery or silo while our gatherers bring nothing in.
			if (!isBldInProgress && GetMacroState(pHouse).DryEconomyActive && (isRefineryOrSlaveMiner || (pBld->Storage > 0 && !pBld->Refinery)))
				continue;
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
			// together, all shipyards together: 4 on Easy, 10 on Normal, on Hard one more than the
			// main enemy has, 10 to 20; see GetFactoryCategoryCap). At the cap one more is allowed for
			// an outpost that needs it or for the first of a new type a captured tree opened (not
			// within 4500 frames of the last cap sale); SellFactoriesOverDifficultyCap then sells a
			// copy of that category once it passes the difficulty ceiling.
			if (const int category = FactoryCapCategory(pBld);
				category >= 0 && factoryCategoryCounts[category] >= factoryCategoryCaps[category])
			{
				const bool outpostNeedsIt = (category == 0)
					? (forwardConYardNeedsBarracks || frontNeedsBarracks || captureSiteNeedsBarracks)
					: (category == 1 ? (forwardConYardNeedsWarFactory || frontNeedsWarFactory) : islandYardNeedsShipyard);
				const bool newFactoryType = owned == 0;
				const bool saleCooldownOver = Unsorted::CurrentFrame - GetMacroState(pHouse).LastFactoryCapSaleFrame >= 4500;

				if (!(outpostNeedsIt || newFactoryType) || !saleCooldownOver || factoryCategoryCounts[category] > factoryCategoryCaps[category])
					continue;
			}

			// Tech before copies, as a human player: no second Barracks or War Factory of a type
			// while a building that unlocks new units or buildings can be built right now, unless
			// an outpost needs it. Airfields keep following the demand of the docking aircraft.
			const bool isGroundFactoryOrBarracks = pBld->Factory == AbstractType::InfantryType
				|| (pBld->Factory == AbstractType::UnitType && !pBld->Naval);

			// The bank governor's copies stay: a bank that outgrows the queues pays for both, and the
			// tech keeps the windows the governor leaves (bankFactoryWindow).
			if (isGroundFactoryOrBarracks && owned >= 1 && GetMacroState(pHouse).HasBuildableTechUnlock)
			{
				const bool outpostNeedsCopy = (pBld->Factory == AbstractType::InfantryType)
					? (forwardConYardNeedsBarracks || frontNeedsBarracks || captureSiteNeedsBarracks)
					: (forwardConYardNeedsWarFactory || frontNeedsWarFactory);
				const bool bankNeedsCopy = bankGovernorBuilds
					&& ((pBld->Factory == AbstractType::InfantryType)
						? BankFactoryDeficit(pHouse, BankQueue::Infantry, pHouse->NumBarracks) > 0
						: BankFactoryDeficit(pHouse, BankQueue::Vehicle, pHouse->NumWarFactories) > 0);

				// The war factories of the tank mass do not wait for the tech either.
				const bool tankMassNeedsCopy = pBld->Factory == AbstractType::UnitType && owned < tankMassWarFactories;

				// Nor the copies that bring us level with the known factories of the strongest enemy.
				const bool parityNeedsCopy = (pBld->Factory == AbstractType::InfantryType)
					? ownedBarracks < maxEnemyBarracks
					: ownedWarFactories < maxEnemyWarFactories;

				if (!outpostNeedsCopy && !bankNeedsCopy && !tankMassNeedsCopy && !parityNeedsCopy)
					continue;
			}

			// Tech tree unlockers (UpdateTechUnlocks): the first of a building that opens new units
			// or buildings gets 300 + 120 per type it unlocks (up to 1400) once the core economy
			// and production stand.
			int techUnlockBonus = 0;
			bool opensTechTypes = false;

			if (owned == 0 && hasCoreEconomyAndProduction)
			{
				auto const& unlocks = GetMacroState(pHouse).TechUnlocks;

				if (auto const it = unlocks.find(pBld->ArrayIndex); it != unlocks.end() && it->second > 0)
				{
					techUnlockBonus = std::min(1400, 300 + 120 * it->second);
					opensTechTypes = true;
				}

				// Expert 2: more when it opens a better counter of the seen enemy army.
				techUnlockBonus += CounterUnlockBonusFor(pHouse, pBld);
			}

			// A radar or tech center of a group we do not hold yet (IsCoreTechBuilding), and once a tech
			// center stands a superweapon building of our tree: with the unlockers, the steps of the tech
			// progression floor (TechStepFloorFrames).
			const bool isCoreTechStep = owned == 0 && hasCoreEconomyAndProduction && IsCoreTechBuilding(pBld)
				&& !OwnsCoreTechGroupOf(pHouse, pBld);
			const bool isSuperWeaponStep = owned == 0 && hasCoreEconomyAndProduction && hasTechCenter
				&& !IsCoreTechBuilding(pBld) && !pBld->IsBaseDefense && pBld->Factory == AbstractType::None
				&& !IsTunnelNetworkBuilding(pBld) && (pBld->SuperWeapon != -1 || pBld->SuperWeapon2 != -1);
			const bool isTechStep = opensTechTypes || isCoreTechStep || isSuperWeaponStep;

			// Branch choices (Prerequisite.Negative: two tech labs, rival defense lines): the first of
			// each gets a per-match taste of +-250, so one match goes one way and another the other,
			// as players do. Deterministic in multiplayer (game seed, house, building).
			if (owned == 0 && IsBranchChoiceBuilding(pBld->ArrayIndex))
				techUnlockBonus += MatchTaste(pHouse, pBld->ArrayIndex, 250);

			int score = 0;
			bool candidateFillsAirDeficit = false;

			if (pBld->PowerBonus > pBld->PowerDrain && !pBld->IsBaseDefense)
			{
				// Hard anti-spam cap: never queue more Power Plants when surplus is
				// already well above our damaged-plant safety cushion (relaxed by +350
				// when stepping-stone crawling to a distant Ore patch)!
				// A stone of the offensive crawl may be a plant while the surplus is within
				// CrawlPowerSurplus of the cushion; past it the crawl takes other buildings. With 2000
				// over the cushion a rich house picked a power plant 59 times in 86000 frames with
				// 1150 to spare (match #9885).
				// A long-range gun tower waiting for power alone (DefensePowerWanted) raises the cushion
				// to what it needs, so a plant comes for it.
				const int defensePowerWanted = (Unsorted::CurrentFrame - GetMacroState(pHouse).DefensePowerWantedFrame <= DefensePowerWantedFrames)
					? GetMacroState(pHouse).DefensePowerWanted : 0;
				const int plantPowerCushion = std::max(requiredPowerCushion, defensePowerWanted);
				const int maxAllowedSurplus = plantPowerCushion
					+ (((needsSteppingStoneCrawlToOre && money >= 1400) || bankOffensiveCrawl) && pBld->BaseNormal ? CrawlPowerSurplus : 350);
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
				else if (realPowerSurplus < plantPowerCushion)
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
				else if (bankOffensiveCrawl && pBld->BaseNormal && pBld->GetFoundationWidth() <= 3)
				{
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

				if (minimumFirst && owned >= 1 && !reachedNewOreField && !isHarvRecoveryCandidate)
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

					// The income falls (IncomeFalling) and the field is a real one or a yard's: its refinery
					// comes before the other copies (user, 2026-11-18). A house whose income fell at frame
					// 31500 with a real field 2 cells from an anchor took a second war factory four times
					// (3650 to 6372 against 3522) before the refinery.
					if (GetMacroState(pHouse).IncomeFalling && slotFieldCell != CellStruct::Empty)
						score += IncomeFallingRefineryBonus;
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
				const int bankBarracks = bankGovernorBuilds ? BankFactoryDeficit(pHouse, BankQueue::Infantry, pHouse->NumBarracks) : 0;

				// A capture site asks for one barracks past any count (placement takes it to the site).
				if (owned >= effectiveMaxBarracks && bankBarracks == 0 && !captureSiteNeedsBarracks)
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
					score = std::max(static_cast<int>(scores[2]), 300) + 1450 + (beachheadNeedsFactories ? 1500 : 0);
				}
				else if (captureSiteNeedsBarracks)
				{
					// A capture site lacks its barracks: the home barracks count does not lower it either.
					score = std::max(static_cast<int>(scores[2]), 300) + CaptureBarracksBonus;
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

				if (captureSiteUrgent && score > bestCaptureBarracksScore)
				{
					bestCaptureBarracksScore = score;
					bestCaptureBarracksIdx = pBld->ArrayIndex;
				}

				if (bankBarracks > 0)
					rememberBankFactory(pBld, score, bankBarracks);
			}
			else if (pBld->Factory == AbstractType::UnitType && !pBld->Naval)
			{
				const int effectiveMaxWarFactories
					= forwardConYardNeedsWarFactory ? std::max(maxWarFactories + 2, 6) + pHouse->NumConYards : maxWarFactories;
				const int bankWarFactories = bankGovernorBuilds ? BankFactoryDeficit(pHouse, BankQueue::Vehicle, pHouse->NumWarFactories) : 0;

				if (owned >= effectiveMaxWarFactories && bankWarFactories == 0)
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
					score = std::max(static_cast<int>(scores[3]), 400) + 1750 + (beachheadNeedsFactories ? 1500 : 0);
				}
				else
				{
					score = scores[3] - owned * (bankSteps > 0 ? 300 : 620) + (bankSteps > 0 ? 400 + 150 * std::min(6, bankSteps) : 0);

					// The first war factory once a refinery stands (see groundWarFactoryBuildableNow),
					// after the first shipyard on a naval match (navalFirst).
					if (ownedWarFactories == 0 && ownedRefineries >= 1)
						score += navalFirst ? -800 : 1200;

					// The island MCV comes from it (UpdateIslandOreOutlook): before the money is gone.
					if (ownedWarFactories == 0 && !navalFirst && GetMacroState(pHouse).IslandExpansionUrgent)
						score += 900;

					// Competitive MultipleFactory Arms Race boost: never fall behind
					// the enemy in War Factory count or when floating macro cash!
					if (ownedWarFactories < maxEnemyWarFactories && ownedRefineries >= 2)
						score += 550;

					if (ownedWarFactories < ecoWarFactoryCap && money >= 2000)
						score += 420;
				}

				// The tank mass builds the war factory before the other factories the governor wants.
				if (bankWarFactories > 0)
				{
					rememberBankFactory(pBld, score,
						bankWarFactories + (pExt->NeuralAI_Archetype == TankMassArchetype ? TankMassBankFactoryLead : 0));
				}
			}
			else if (pBld->Factory == AbstractType::UnitType && pBld->Naval)
			{
				const int bankShipyards = bankGovernorBuilds ? BankFactoryDeficit(pHouse, BankQueue::Naval, pHouse->NumShipyards) : 0;

				if (owned >= maxShipyards + (islandYardNeedsShipyard ? 1 : 0) && (bankShipyards == 0 || !islandNavalWar))
					continue;

				if (minimumFirst && owned >= 1 && !islandYardNeedsShipyard)
					continue;

				// A crossing that waits for a transport this shipyard builds (crossingPrerequisites) is reason
				// enough: a Yuri house with 31000 credits never built the yard its hovercraft needs.
				const bool crossingNeedsYard = owned == 0
					&& std::find(crossingPrerequisites.begin(), crossingPrerequisites.end(), pBld->ArrayIndex) != crossingPrerequisites.end();

				if (!(HouseNeedsNaval(pHouse) || islandYardNeedsShipyard || crossingNeedsYard)
					|| CachedTetrisBuildCell(pHouse, pBld) == CellStruct::Empty)
				{
					continue;
				}

				score = scores[7] - owned * 950;

				// The first shipyard of a naval match once a refinery stands (navalFirst).
				if (navalFirst && ownedRefineries >= 1)
					score += 1600;

				// A conquered island's own shipyard (islandYardNeedsShipyard).
				if (islandYardNeedsShipyard)
					score += 1300;

				if (ownedShipyards < maxEnemyShipyards && ownedShipyards >= 1 && money >= 1800)
					score += 400;

				if (bankShipyards > 0)
					rememberBankFactory(pBld, score, bankShipyards);
			}
			else if (pBld->Factory == AbstractType::AircraftType)
			{
				// Airfields stay apart from the other factories (no bank governor, no category cap):
				// another one only while the free docks are short of the difficulty minimum, or the
				// first of a new type (a captured tree), which opens other aircraft.
				const bool newAirfieldType = owned == 0 && pHouse->NumAirpads >= 1;

				if (pHouse->NumAirpads >= 1 && !airDocksShort && !newAirfieldType && !airDocksBelowFloor)
					continue;

				// With little room only a full set of docks asks for another airfield.
				if (minimumFirst && owned >= 1 && !newAirfieldType && !airDocksFull)
					continue;

				if (owned == 0 && pHouse->NumAirpads >= 1 && pHouse->NumAirpads < 4 && SlotGate(scores[82], 60, 82))
				{
					score = std::max(static_cast<int>(scores[8]), 250) + 550
						+ std::clamp(static_cast<int>(scores[82]) / 2, 0, 350);
				}
				else
				{
					// The docks below the floor carry no penalty for the airfields already owned.
					score = scores[8] - (airDocksBelowFloor ? 0 : owned * 750)
						+ ((expert8NeedsMoreAirpadDocks || airDockWanted || airDocksShort) ? 850 : 0);

					if (ownedAirpads < maxEnemyAirpads && hasTechCenter && money >= 2000)
						score += 350;
				}

				// An airfield that is our radar (IsCoreTechBuilding) is a tech step as well.
				if (isCoreTechStep)
					score += CoreTechStepBonus;
			}
			else if (pBld->UnitRepair)
			{
				// Forward Repair plan: one more station near the front. The plan counts the house's
				// stations of every type (a captured Tech Outpost too), so a type it owns none of is
				// asked for as well: in the focus batch of 2026-11-15 11 of 26 plans ran out of time
				// with the only station of another type than the one the house could build.
				auto const& repairMacro = GetMacroState(pHouse);
				const bool wantsForwardRepair = !pBld->Naval && owned <= 1
					&& repairMacro.ForwardRepairUntilFrame > Unsorted::CurrentFrame && money >= 1500;

				if (owned >= 1 && !wantsForwardRepair)
					continue;

				score = scores[9] + (wantsForwardRepair ? 500 : 0);

				if (wantsForwardRepair && score > bestForwardRepairScore)
				{
					bestForwardRepairScore = score;
					bestForwardRepairIdx = pBld->ArrayIndex;
				}
			}
			else if (pBld->SuperWeapon != -1 && !IsCoreTechBuilding(pBld))
			{
				// A tech center or radar that grants a superweapon (a force shield, a spy plane) is
				// scored with the tech buildings below.
				if (owned >= 1)
					continue;

				score = scores[10] + 450;

				if (pHouse->NumConYards >= 2 && SlotGate(scores[82], 60, 82))
					score += std::clamp(static_cast<int>(scores[82]) / 2, 0, 350);
			}
			else if (pBld->Storage > 0 && !isRefineryOrSlaveMiner)
			{
				// Silos only while storage really limits our income (refineries with
				// Refinery.UseStorage nearly full), as many as that lasts (user, as the Advanced AI): each
				// one lowers the share of storage in use. Without that a silo is money thrown away.
				if (!GetMacroState(pHouse).StorageNearlyFull || (minimumFirst && owned >= 1))
					continue;

				score = scores[11] - std::min(owned, 6) * 150;
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
				bool isAntiInf = false;
				bool isAntiArmor = false;
				bool isAntiAir = false;
				bool isPureAA = false;
				bool isUnarmedBunker = false;
				GetDefenseCapabilities(pBld, isAntiInf, isAntiArmor, isAntiAir, isPureAA, isUnarmedBunker);
				// A long-range gun with a dead zone (a Grand Cannon), see LongRangeTowerBonusPerCell.
				const bool isDeadZoneTower = IsDeadZoneTower(pBld);

				// A raid short of guns: this defense shoots up, so the caps and penalties that keep a
				// quiet base from piling up towers do not hold it back.
				const bool fillsAirDeficit = isAntiAir && airRaidShortOfDefenses;
				// One the Island Hold yard still lacks: the house's caps and budget count the towers
				// of its other bases, which do nothing for that yard, so only being broke holds it back.
				const bool fillsIslandHold = IslandHoldWants(pHouse, pBld);

				const bool canBuildEmergencyDefense = (ownedRefineries >= 1 && ownedBarracks >= 1
					&& (wasRecentlyAttacked || frontlineThreatActive || undefendedRemoteAssetsCount > 0 || underAirAttack));
				// Sides that open with the war factory have no barracks yet when the first raid comes.
				const bool canBuildRaidDefense = fillsAirDeficit && ownedRefineries >= 1 && (ownedBarracks >= 1 || ownedWarFactories >= 1);

				if (!hasCoreEconomyAndProduction && !canBuildEmergencyDefense && !canBuildRaidDefense && pHouse->ConYards.Count > 0)
				{
					if (isAntiAir)
						noteAirDefense(pBld, 1, 0);

					continue;
				}

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

				// Guns that only shoot up: up to the anti-air level in points, and past it while a key
				// asset lacks anti-air cover.
				if (isPureAA && ownedAntiAirValue >= wantedAirValue && !antiAirCoverageWanted && !fillsIslandHold)
				{
					noteAirDefense(pBld, 2, 0);

					continue;
				}

				// An outpost or front that needs a tower raises the cap by what it needs, up to
				// OutpostDefenseRoomPerYard per ConYard. The cap used to be "what we have plus one" for
				// as long as one asset counted as undefended, which is no cap at all: a house whose
				// towers went elsewhere kept that flag and stood with 98 ground defenses (match #9885).
				const bool needsOutpostPointDefense = !isPureAA && !isUnarmedBunker
					&& (undefendedRemoteAssetsCount > 0 || forwardConYardNeedsDefense || frontlineDefenseDeficit > 0);
				const int outpostDefenseRoom = std::min(OutpostDefenseRoomPerYard * std::max(1, pHouse->NumConYards),
					3 + undefendedRemoteAssetsCount * 2 + std::max(1, frontlineDefenseDeficit) + (forwardConYardNeedsDefense ? 3 : 0));
				const int effectiveMaxGroundDefenses = needsOutpostPointDefense
					? maxGroundDefenses + outpostDefenseRoom
					: maxGroundDefenses;

				// Ground towers: up to the ground level in points, past it while a key asset lacks a tower
				// within reach, or for an outpost or front within the room above.
				const bool frontlineRoomLeft = needsOutpostPointDefense && totalGroundDefenses < effectiveMaxGroundDefenses;

				if (!isPureAA && !fillsAirDeficit && !fillsIslandHold && ownedGroundDefenseValue >= optimalGroundDefenseValue
					&& !groundCoverageWanted && !frontlineRoomLeft)
				{
					if (isAntiAir)
						noteAirDefense(pBld, 3, 0);

					continue;
				}

				// Cover the house can pay for now: a broke house (180 credits) kept picking towers for it.
				const bool fillsCoverage = ((!isPureAA && groundCoverageWanted) || (isAntiAir && antiAirCoverageWanted))
					&& money >= pBld->GetActualCost(pHouse);

				// Do not queue powered defenses when power cushion is already tight
				if ((pBld->Powered || pBld->PowerDrain >= 50)
					&& (pHouse->HasLowPower() || realPowerSurplus - pBld->PowerDrain < PoweredDefenseSurplusMargin))
				{
					if (isAntiAir)
						noteAirDefense(pBld, 4, realPowerSurplus);

					// A long-range gun held back by power alone asks for a plant: the power branch reads
					// it at the next ticks (DefensePowerWantedFrames). So does a gun that shoots up while
					// the anti-air is short: the plant came instead of it and nothing asked for it.
					if (isDeadZoneTower || (isAntiAir && airDefenseDeficit > 0))
					{
						auto& gunMacro = GetMacroState(pHouse);

						gunMacro.DefensePowerWanted = pBld->PowerDrain + PoweredDefenseSurplusMargin;
						gunMacro.DefensePowerWantedFrame = Unsorted::CurrentFrame;
					}

					continue;
				}

				// Defense budget (NeuralAI.Defense.inl): none while broke, none beyond the value our army
				// and economy justify, fewer when our towers die without paying back or seen enemy
				// artillery outranges this one. A tower already in construction is finished. A raid
				// short of guns and the Island Hold yard are only held back while broke.
				int defenseBudgetShift = 0;

				if (!isBldInProgress)
				{
					bool budgetBlocked = false;
					defenseBudgetShift = DefenseBudgetShift(pHouse, pBld, isPureAA, totalGroundDefenses, totalAADefenses, budgetBlocked);

					// The cover of a key asset is no extra spending: the budget does not hold it back.
					if (budgetBlocked && ((!fillsAirDeficit && !fillsIslandHold && !fillsCoverage) || GetMacroState(pHouse).BrokeHoldActive))
					{
						if (isAntiAir)
							noteAirDefense(pBld, 5, 0);

						continue;
					}

					if (fillsAirDeficit || fillsIslandHold || fillsCoverage)
						defenseBudgetShift = std::max(defenseBudgetShift, 0);
				}

				const bool isAntiArmorOrHighTierDefense = !isPureAA && !isUnarmedBunker && isAntiArmor;
				const bool isBasicAntiInfantryDefense = !isPureAA && !isUnarmedBunker && isAntiInf && !isAntiArmor;

				const int tierQualityBonus
					= isUnarmedBunker ? 100 : std::min(950, (pBld->Cost / 2) + std::max(0, pBld->TechLevel) * 150);

				score = (isPureAA ? scores[6] : std::max(static_cast<int>(scores[5]), 320)) + tierQualityBonus
					+ CounterBonusFor(pHouse, pBld) + defenseBudgetShift;

				// Core ground towers a base wants before anything else in defense.
				const int desiredCoreGroundDefs = std::clamp(2 + ownedRefineries + (features[65] >= 18 ? 2 : 0), 3, 8);

				// A long-range gun with a dead zone: its reach counts past the tier cap, most against
				// seen artillery that does not out-reach it, and it is no first line: short of the core
				// ground towers, which fire at what gets next to it, it comes after them.
				if (isDeadZoneTower)
				{
					auto const& gunMacro = GetMacroState(pHouse);
					const int reachCells = GroundRangeOf(pBld) / Unsorted::LeptonsPerCell;
					score += std::min(LongRangeTowerBonusMax, (reachCells - ArtilleryRangeCells) * LongRangeTowerBonusPerCell);

					if (gunMacro.SeenArtilleryShare >= 15 && reachCells >= gunMacro.SeenArtilleryRangeCells)
						score += std::min(CounterBatteryBonusMax, gunMacro.SeenArtilleryShare * CounterBatteryBonusPerShare);

					if (!isBldInProgress && totalGroundDefenses < desiredCoreGroundDefs)
						score -= DeadZoneTowerEarlyPenalty;
				}

				constexpr int islandHoldDefenseBonus = 450;

				if (fillsIslandHold)
					score += islandHoldDefenseBonus;

				if (isPureAA)
				{
					// A raid short of guns or a base short of its anti-air value takes no count penalty:
					// each one that shoots up is one it still needs.
					if (!fillsAirDeficit && airDefenseDeficit == 0)
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
					score += 580 + (isAntiArmorOrHighTierDefense ? 260 : 0)
						- (groundThreatDefenseDeficit > 0 ? 0 : owned * 220 + totalGroundDefenses * 120);
				}
				else if (!fillsAirDeficit)
				{
					// A base short of the towers the enemy forces that came at it ask for takes no count penalty.
					score -= (groundThreatDefenseDeficit > 0 && !isUnarmedBunker ? 0 : totalGroundDefenses * 210 + owned * 250)
						+ (isUnarmedBunker ? 450 : 0);
				}

				// The ground threat (EnemyGroundThreatValue): by the towers still missing for it.
				if (!isPureAA && !isUnarmedBunker && groundThreatDefenseDeficit > 0)
					score += std::min(GroundDefenseMissingTowerMost, groundThreatDefenseDeficit * GroundDefenseMissingTowerBonus);

				// Proactive Heavy-Armor Shielding: build anti-armor / high-tier defenses BEFORE
				// the enemy's tank column arrives at an undefended base!
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

				// AdvAI Proactive Enemy Air Value Scaling: by the guns still missing for the wanted value.
				if (isAntiAir && enemyAirValue > 0 && airDefenseDeficit > 0)
					score += std::min(AirDefenseMissingGunMost, airDefenseDeficit * AirDefenseMissingGunBonus);

				// An air raid asks for anti-air whether or not the engine counts it as an attack on the
				// house: flying units that hit our units leave that mark untouched. With nothing of ours
				// that shoots up, such a defense comes before anything else; while the raid is short of
				// guns each one comes before the towers that cannot shoot up.
				constexpr int airDefenseFirstBonus = 900;
				constexpr int airDefenseDeficitBonus = 600;
				constexpr int airDefenseStandingBonus = 350;
				constexpr int groundDefenseRaidPenalty = 1500;

				if (isAntiAir && totalAntiAirDefenses == 0 && (airRaid || enemyAirValue >= airDefenseUrgentValue))
					score += airDefenseFirstBonus;

				// The standing amount and what known enemy air bases announce: wanted, with no raid to
				// make it urgent.
				if (isAntiAir && airDefenseDeficit > 0 && !fillsAirDeficit)
					score += airDefenseStandingBonus;

				if (fillsAirDeficit)
					score += airDefenseDeficitBonus;
				else if (airRaidOnly && !isAntiAir && !isBldInProgress)
					score -= groundDefenseRaidPenalty;

				// A key asset without cover (placement puts the defense next to it).
				if (fillsCoverage)
					score += CoverageDefenseBonus;

				if (wasRecentlyAttacked || effectiveFrontlineThreat || (airRaid && isAntiAir))
				{
					if (airRaid && isAntiAir)
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

				if ((features[72] > 0 || (detectionNeed & (DetectionNeedStealth | DetectionNeedSub))) && pBld->Sensors)
					score += 350;

				// Production before towers: with a refinery standing and the first war factory (or the
				// first shipyard of a naval match) buildable, a tower waits unless the base is under
				// attack or an air raid or an island hold asks for it. A tower near an enemy building
				// ever seen scored 5000 to 6000 and the first war factory about 3500, so houses with
				// 6000 credits and more stood without one until frame 20000 (14 of 126 houses).
				constexpr int towerBeforeFirstFactoryPenalty = 3000;

				if (firstFactoryPending && ownedRefineries >= 1 && !wasRecentlyAttacked && !fillsAirDeficit
					&& !fillsIslandHold && !(airRaid && isAntiAir) && !isBldInProgress)
				{
					score -= towerBeforeFirstFactoryPenalty;
				}

				candidateFillsAirDeficit = fillsAirDeficit;

				if (fillsAirDeficit && score > bestAirDefenseScore)
				{
					bestAirDefenseScore = score;
					bestAirDefenseIdx = pBld->ArrayIndex;
				}

				if (fillsIslandHold && score > bestIslandHoldScore)
				{
					bestIslandHoldScore = score;
					bestIslandHoldIdx = pBld->ArrayIndex;
				}

				if (isAntiAir)
					noteAirDefense(pBld, 6, score);
			}
			else if (ResearchPaidByProducer(pHouse, pBld) > 0)
			{
				// A producer of what our research still to buy costs (battle points in CnCReloaded) is
				// income for it: scored like a refinery, up to ResearchGeneratorMost.
				if (owned >= ResearchGeneratorMost)
					continue;

				score = scores[1] + 1200 - owned * 480;
			}
			else
			{
				// Tech & Support buildings (Radar, Battle Lab, Ore Purifier, Industrial Plant,
				// Cloning Vats, Grinder, Gap/Cloak Generator)
				const int supportRadius = GetSupportRadius(pBld);
				// Detection need: a sensor building (SensorArray, no base defense) for a stealth sighting or
				// loss no detector of ours covers, one past the support cap, scored by its reach.
				const int sensorBuildingReach = pBld->SensorArray && detectionSpotUncovered
					? DetectionReachCells(pBld, DetectionNeedStealth) : 0;
				const int maxAllowed = ((supportRadius > 0 && hasTechCenter && money > 3000) ? 2 : 1) + (sensorBuildingReach > 0 ? 1 : 0);

				// Gap and cloak generators, inhibitors and radar jammers: one more while a key building
				// is outside the network and power allows (AdvAI BuildSupport), placed to cover it
				// (FindSpatialTetrisBuildCell); the other support buildings keep the cap.
				if (IsCoverageSupport(pBld))
				{
					if (!SupportCopyWanted(pHouse, pBld, owned))
						continue;
				}
				else if (owned >= maxAllowed)
				{
					continue;
				}

				score = scores[4] + 550 + std::min(600, pBld->Cost / 3) - owned * 700;

				if (sensorBuildingReach > 0)
					score += std::min(DetectionScoreMost, DetectionScorePerCell * sensorBuildingReach);

				// A radar or tech center counts once per group (isCoreTechStep), a big support building
				// that drains power once per type.
				const bool isCoreTechUnlock = IsCoreTechBuilding(pBld) ? isCoreTechStep
					: (pBld->TechLevel >= 3 && pBld->Cost >= 1000 && pBld->PowerDrain > 0 && owned == 0 && hasCoreEconomyAndProduction);

				if (isCoreTechUnlock)
					score += CoreTechStepBonus + (underVehicleAttack ? 450 : 0);

				if (pHouse->NumConYards >= 2 && SlotGate(scores[82], 60, 82))
					score += 250 + std::clamp(static_cast<int>(scores[82]) / 2, 0, 300);
			}

			score += techUnlockBonus;

			if (allyDefenseCell != CellStruct::Empty && pBld->IsBaseDefense && GroundRangeOf(pBld) > 0 && !pBld->CanBeOccupied
				&& !pBld->Bunker)
			{
				score += 900;
			}

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
				// the house never builds another MCV. For the island expansion it comes before
				// everything else (Heisenberg repro: an Allied house with 9600 credits put them into
				// turrets and destroyers and never built the service depot its MCV needs).
				score = std::max(score + 900, 2600 + static_cast<int>(scores[30]) / 4);

				if (GetMacroState(pHouse).IslandExpansionUrgent)
					score = std::max(score, 5000);
			}

			if (!crossingPrerequisites.empty() && pHouse->CountOwnedNow(pBld) == 0
				&& std::find(crossingPrerequisites.begin(), crossingPrerequisites.end(), pBld->ArrayIndex) != crossingPrerequisites.end())
			{
				score += CrossingPrerequisiteBonus;

				if (score > bestCrossingScore)
				{
					bestCrossingScore = score;
					bestCrossingIdx = pBld->ArrayIndex;
				}
			}

			constexpr int airDefensePrerequisiteBonus = 1500;

			if (!airDefensePrerequisites.empty() && pHouse->CountOwnedNow(pBld) == 0
				&& std::find(airDefensePrerequisites.begin(), airDefensePrerequisites.end(), pBld->ArrayIndex) != airDefensePrerequisites.end())
			{
				score += airDefensePrerequisiteBonus;
			}

			if (isBldInProgress && (!pHouse->HasLowPower() || pBld->PowerBonus > pBld->PowerDrain))
			{
				score += 850; // Commit to finishing partially constructed buildings in
							// the ConYard!
			}

			if (pBld->Refinery && !pBld->ResourceGatherer && score > awaitedRefineryScore)
			{
				awaitedRefineryScore = score;
				awaitedRefineryIdx = pBld->ArrayIndex;
			}

			// An airfield for the docks every house keeps or for a full set of docks (AirDocksFloor*).
			if (pBld->Factory == AbstractType::AircraftType && hasCoreEconomyAndProduction
				&& (airDocksBelowFloor || (airDocksFull && pHouse->NumAirpads >= 1)) && score > bestPriorityAirfieldScore)
			{
				bestPriorityAirfieldScore = score;
				bestPriorityAirfieldIdx = pBld->ArrayIndex;
			}

			if (isTechStep)
			{
				if (std::find(yardBuildIdxs.begin(), yardBuildIdxs.end(), pBld->ArrayIndex) != yardBuildIdxs.end())
					techStepInProgress = true;

				if (score > bestTechStepScore)
				{
					bestTechStepScore = score;
					bestTechStepIdx = pBld->ArrayIndex;
				}
			}

			if (score > bestBldScore)
			{
				bestBldScore = score;
				bestBldIdx = pBld->ArrayIndex;
				bestFillsAirDeficit = candidateFillsAirDeficit;
			}

			// The two best the yard builds itself: its pick when a deployer brings the best one.
			if (yardBuildsIt && score > yardBestScore)
			{
				yardSecondIdx = yardBestIdx;
				yardSecondScore = yardBestScore;
				yardBestIdx = pBld->ArrayIndex;
				yardBestScore = score;
			}
			else if (yardBuildsIt && score > yardSecondScore)
			{
				yardSecondIdx = pBld->ArrayIndex;
				yardSecondScore = score;
			}
		}

		const int scoredBldIdx = bestBldIdx;
		scoredBuildingScore = bestBldScore;

		// A raid with nothing of ours that shoots up: the best such defense comes before any other
		// pick, except restoring income and a building the yard is already working on.
		if (airRaidShortOfDefenses && totalAntiAirDefenses == 0 && bestAirDefenseIdx >= 0 && bestBldIdx != bestAirDefenseIdx
			&& hasActiveBuildingIncome && inProgressBldIdx < 0 && inProgressDefIdx < 0)
		{
			bestBldIdx = bestAirDefenseIdx;
			bestBldScore = std::max(bestBldScore, bestAirDefenseScore);
			bestFillsAirDeficit = true;
		}

		// Bank governor: in its share of the 900-frame windows the next structure is the factory it
		// wants most; the other windows keep the usual picks (tech, defenses, power, refineries).
		// Building time is what a big bank lacks, so the share grows with the surplus: two windows
		// in four, three from 50000 credits beyond the reserve, all from 100000. Under attack or
		// facing a frontline threat none, and OffensiveBankThreatWindows with an offensive bank
		// (match #12322: a Nod house under attack in every sample kept 50000 to 70000 credits and
		// gave half its yard time to towers that died while the enemy grew from 6 to 17 war
		// factories). Never while restoring income.
		const bool bankUnderThreat = wasRecentlyAttacked || effectiveFrontlineThreat;
		const int bankSurplus = bankMacro.BankSurplus;
		// The tank mass answers a threat with tanks, and tanks come from war factories: it keeps
		// TankMassBankWindowsThreat windows under threat and TankMassBankWindows otherwise.
		const int usualBankWindowShare = bankUnderThreat
			? (bankSurplus >= OffensiveBankSurplus ? OffensiveBankThreatWindows : 0)
			: (bankSurplus >= 100000 ? 4 : (bankSurplus >= 50000 ? 3 : 2));
		const int bankWindowShare = pExt->NeuralAI_Archetype == TankMassArchetype
			? std::max(usualBankWindowShare, bankUnderThreat ? TankMassBankWindowsThreat : TankMassBankWindows)
			: usualBankWindowShare;
		const bool bankFactoryWindow = (bankWindow % 4) < bankWindowShare;

		// The docks every house keeps and a full set of docks (AirDocksFloor*): that airfield comes before
		// the routine picks, never before restoring income, a building under construction or a gun an air
		// raid still needs. Below the floor it comes before the bank governor's factories too; above it the
		// governor keeps its windows.
		const bool priorityAirfieldReady = bestPriorityAirfieldIdx >= 0 && bestBldIdx != bestPriorityAirfieldIdx
			&& hasActiveBuildingIncome && !bestFillsAirDeficit && inProgressBldIdx < 0 && inProgressDefIdx < 0;

		if (priorityAirfieldReady && !airDocksBelowFloor)
		{
			bestBldIdx = bestPriorityAirfieldIdx;
			bestBldScore = std::max(bestBldScore, bestPriorityAirfieldScore);
		}

		// A gun the raid still needs keeps its window.
		if (bankFactoryIdx >= 0 && bestBldIdx != bankFactoryIdx && bankFactoryWindow && hasActiveBuildingIncome && !bestFillsAirDeficit)
		{
			bestBldIdx = bankFactoryIdx;

			if (IsAITickWindow(450, evalDelay))
				CountDecision(NeuralDecision::BankFactoryPick, pHouse->ArrayIndex);
		}

		if (priorityAirfieldReady && airDocksBelowFloor)
		{
			bestBldIdx = bestPriorityAirfieldIdx;
			bestBldScore = std::max(bestBldScore, bestPriorityAirfieldScore);
		}

		// Tech progression floor (user approved, 2026-11-20, «se supone que ha de desarrollar todo su
		// árbol tecnológico»): once the core economy and production stand, when no tech step (a first
		// building that opens types of our tree or of a captured one, a radar or tech center of a group
		// we lack, after the tech center a superweapon building) has been in construction for
		// TechStepFloorFrames, the best of them is the pick, before the bank governor's factories.
		// Defenses outscored it: over 46 matches 32 of 74 houses alive at frame 36000 had picked a tech
		// center by then and 6 of 49 a superweapon by frame 60000, with banks of 100000 credits and
		// more. Paid as it goes; never over restoring income, a building already under construction or
		// a gun an air raid still needs; the plans below and the power plant before it still come first.
		auto& techMacro = GetMacroState(pHouse);
		const int techNow = Unsorted::CurrentFrame;

		if (!hasCoreEconomyAndProduction || techStepInProgress || bestTechStepIdx < 0 || techMacro.TechStepWaitSince > techNow)
			techMacro.TechStepWaitSince = techNow;

		const int techStepWaited = techNow - techMacro.TechStepWaitSince;
		const bool techFloorPick = bestTechStepIdx >= 0 && techStepWaited >= TechStepFloorFrames && hasActiveBuildingIncome
			&& !bestFillsAirDeficit && inProgressBldIdx < 0 && inProgressDefIdx < 0;

		if (techFloorPick && bestBldIdx != bestTechStepIdx)
		{
			// TEMP diagnostic: what the floor takes the yard from (TechFloorLogLines a match).
			if (techMacro.TechFloorLogCount < TechFloorLogLines && techNow - techMacro.TechFloorLogFrame >= TechFloorLogFrames)
			{
				++techMacro.TechFloorLogCount;
				techMacro.TechFloorLogFrame = techNow;

				// What the yards build now, with their progress, and whether the pick before the floor came
				// from the scored loop or from a later override (power plant, plans, bank governor).
				std::string yardBuilds;

				for (auto const pFactory : FactoryClass::Array)
				{
					if (pFactory && pFactory->Owner == pHouse && pFactory->Object && pFactory->Object->WhatAmI() == AbstractType::Building)
					{
						char part[64];
						std::snprintf(part, sizeof(part), " %s %d/54%s", static_cast<BuildingClass*>(pFactory->Object)->Type->ID,
							pFactory->GetProgress(), pFactory->IsSuspended ? " held" : "");
						yardBuilds += part;
					}
				}

				Debug::Log("[NeuralAI] House %d tech floor: %s after %d frames instead of %s (score %d against %d, scored %s), yard builds%s, "
					"credits %d, frame %d.\n",
					pHouse->ArrayIndex, BuildingTypeClass::Array.GetItem(bestTechStepIdx)->ID, techStepWaited,
					bestBldIdx >= 0 ? BuildingTypeClass::Array.GetItem(bestBldIdx)->ID : "none", bestTechStepScore, bestBldScore,
					scoredBldIdx >= 0 ? BuildingTypeClass::Array.GetItem(scoredBldIdx)->ID : "none",
					yardBuilds.empty() ? " nothing" : yardBuilds.c_str(), money, techNow);
			}

			bestBldIdx = bestTechStepIdx;
			bestBldScore = std::max(bestBldScore, bestTechStepScore);
		}

		// Forward Repair: the second repair station comes before the other picks, the bank governor's
		// factories included, under the same terms as Island Hold below (which wins when both ask).
		// The plan has 5400 frames for one building and its score alone never won: in the archive of
		// 2026-11-14 night 38 of 47 plans ran out of time with no repair station among the picks
		// sampled in their window, 24 of them never below 20000 credits.
		if (bestForwardRepairIdx >= 0 && bestBldIdx != bestForwardRepairIdx && hasActiveBuildingIncome && !bestFillsAirDeficit
			&& inProgressBldIdx < 0 && inProgressDefIdx < 0)
		{
			bestBldIdx = bestForwardRepairIdx;
			bestBldScore = std::max(bestBldScore, bestForwardRepairScore);
		}

		// A crossing our units wait for: the building its transport lacks comes before the other picks,
		// the bank governor's factories included. Its bonus alone lost to the towers: in the archive of
		// 2026-11-17 the rich houses waited 11000 to 20000 frames for their shipyard (a Yuri house with
		// 38000 credits built gattling cannons and psychic towers scored 6000 to 9250 against a yard
		// near 3500). It needs no money saved up (user): like a human's, the build is paid as it goes
		// and only stops without income. Never before restoring income, a building already under
		// construction or a gun an air raid still needs.
		if (bestCrossingIdx >= 0 && bestBldIdx != bestCrossingIdx && hasActiveBuildingIncome && !bestFillsAirDeficit
			&& inProgressBldIdx < 0 && inProgressDefIdx < 0)
		{
			bestBldIdx = bestCrossingIdx;
			bestBldScore = std::max(bestBldScore, bestCrossingScore);
		}

		// An urgent capture site (UpdateCaptureBarracks: our engineers failed on the way to its tech, or
		// the crossing to it is dangerous): its barracks comes before the other picks, the bank governor's
		// factories included, under the terms of Island Hold below (which wins when both ask).
		if (bestCaptureBarracksIdx >= 0 && bestBldIdx != bestCaptureBarracksIdx && hasActiveBuildingIncome && !bestFillsAirDeficit
			&& inProgressBldIdx < 0 && inProgressDefIdx < 0)
		{
			bestBldIdx = bestCaptureBarracksIdx;
			bestBldScore = std::max(bestBldScore, bestCaptureBarracksScore);
		}

		// Island Hold: the defense its yard still lacks comes before the other picks, the bank
		// governor's factories included: the plan has 5400 frames for up to four buildings, the house
		// makes one at a time and its score alone rarely wins (match #8871: the yard waited 10800
		// frames for one anti-air defense while 23 of the 24 picks sampled were ground towers and
		// factories). Never before restoring income, a building already under construction or a gun
		// an air raid still needs.
		if (bestIslandHoldIdx >= 0 && bestBldIdx != bestIslandHoldIdx && hasActiveBuildingIncome && !bestFillsAirDeficit
			&& inProgressBldIdx < 0 && inProgressDefIdx < 0)
		{
			bestBldIdx = bestIslandHoldIdx;
			bestBldScore = std::max(bestBldScore, bestIslandHoldScore);
		}

		// Ally site plans (Ally Backup, Ally Forward Outpost) ask for one building next to an ally's
		// base: it comes before routine picks, never before restoring income, a building already
		// under construction or anything scored as an emergency (2700+).
		{
			auto const& siteMacro = GetMacroState(pHouse);

			if (siteMacro.AllySiteUntilFrame > Unsorted::CurrentFrame && siteMacro.AllySiteBuildType >= 0
				&& siteMacro.AllySiteBuildType < BuildingTypeClass::Array.Count && hasActiveBuildingIncome
				&& inProgressBldIdx < 0 && bestBldScore < 2700)
			{
				auto const pSiteType = BuildingTypeClass::Array.GetItem(siteMacro.AllySiteBuildType);
				auto const cdIt = siteMacro.BuildingPlacementCooldownUntilFrame.find(siteMacro.AllySiteBuildType);
				const bool coolingDown = cdIt != siteMacro.BuildingPlacementCooldownUntilFrame.end()
					&& Unsorted::CurrentFrame < cdIt->second;

				if (pSiteType && !coolingDown && !HouseExt::IsDisabledFromShell(pHouse, pSiteType)
					&& CanBuildUnderHumanRules(pHouse, pSiteType) && money >= pSiteType->GetActualCost(pHouse))
				{
					bestBldIdx = siteMacro.AllySiteBuildType;
					bestBldScore = 2700;
				}
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

		// The refinery the economy hold saves for (RefinerySaveTarget, a newly reached field) is the
		// pick of the yard as soon as it is wanted, paid as the credits come in (user: AI builds pay as
		// they go). On its score it lost the yard to a building worth about twice as much, the hold
		// cancelled that building again and again, and once the credits reached the price the yard built
		// something else (user approved, 2026-11-20, P1; over 39 matches: 709 holds, 1726 cancels, a
		// refinery more 1800 frames after the price was reached in 53 of 152). With no refinery past
		// the rules of this pick the hold does not save for one. A harvester the hold saves for comes
		// first, and so do the power plant of a house short of power and anything under attack.
		{
			auto& saveMacro = GetMacroState(pHouse);

			if (saveMacro.RefinerySaveTarget > 0 && awaitedRefineryIdx < 0)
			{
				saveMacro.RefinerySaveTarget = 0;
			}
			else if (saveMacro.RefinerySaveTarget > 0 && saveMacro.HarvesterSaveTarget <= 0 && !saveMacro.CounterUnderAttack
				&& bestBldIdx != awaitedRefineryIdx)
			{
				auto const pPicked = bestBldIdx >= 0 ? BuildingTypeClass::Array.GetItem(bestBldIdx) : nullptr;
				// The tech floor yields to the refinery (expansion is crucial) until it has waited
				// TechStepRefineryYieldFrames, then takes one building between refineries.
				const bool keepPick = pPicked
					&& (pPicked->Refinery || (pHouse->HasLowPower() && pPicked->PowerBonus > pPicked->PowerDrain)
						|| (techFloorPick && bestBldIdx == bestTechStepIdx && techStepWaited >= TechStepRefineryYieldFrames));

				if (!keepPick)
					bestBldIdx = awaitedRefineryIdx;
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

		// TEMP diagnostic: what the base knows of the air threat, what it has against it and what
		// became of the best defense that shoots up. Stage 0 none buildable, 1 no core buildings,
		// 2 anti-air cap, 3 ground cap, 4 power, 5 budget, 6 scored.
		if ((airRaid || enemyAirValue > 0) && IsAITickWindow(900, 0))
		{
			auto typeName = [](int idx) -> const char*
			{
				return idx >= 0 && idx < BuildingTypeClass::Array.Count ? BuildingTypeClass::Array.GetItem(idx)->ID : "none";
			};

			Debug::Log("[NeuralAI] House %d AirDefense DIAG: enemy air value %d, under air attack %d, threat category %d, recently attacked %d, "
				"AA defenses %d of %d, shooting up %d of %d wanted (%d for the raid), ground defenses %d, credits %d, best %s stage %d score %d, "
				"scored pick %s score %d, final pick %s, in progress %s and %s, yards %d, barracks %d, factories %d, power %d, frame %d.\n",
				pHouse->ArrayIndex, enemyAirValue, underAirAttack ? 1 : 0, static_cast<int>(activeThreatCat), wasRecentlyAttacked ? 1 : 0,
				totalAADefenses, maxAADefenses, totalAntiAirDefenses, wantedAirDefenses, totalAntiAirDefenses + raidDefenseDeficit, totalGroundDefenses, money,
				airDiagName, airDiagStage, airDiagScore, typeName(scoredBldIdx), bestBldScore, typeName(bestBldIdx),
				typeName(inProgressBldIdx), typeName(inProgressDefIdx), pHouse->ConYards.Count, ownedBarracks, ownedWarFactories,
				realPowerSurplus, Unsorted::CurrentFrame);
		}

		// TEMP diagnostic: the enemy forces that came at us and the towers they ask for.
		if (enemyGroundThreat > 0 && IsAITickWindow(900, 0))
		{
			Debug::Log("[NeuralAI] House %d GroundDefense DIAG: enemy ground threat %d, ground value %d of %d wanted, towers %d (%d missing), "
				"towers per asset %d, assets short of cover %d of %d, credits %d, frame %d.\n",
				pHouse->ArrayIndex, enemyGroundThreat, ownedGroundDefenseValue, optimalGroundDefenseValue, totalGroundDefenses,
				groundThreatDefenseDeficit, groundCoverTowersPerAsset, groundCoverageDeficit, coverageAssetCount, money, Unsorted::CurrentFrame);
		}

		// Who brings the pick (NeuralAI.MobileFactory.inl; user approved, 2026-11-21, «Apruebo el plan»; user:
		// the deployer for the far spots, which the yard's adjacency does not reach, near ones too when that
		// does better): the yard when it builds it and its cell stands within DeployerNearSpotCells of the
		// role goal (FindDeployerGoal), a deployer when the yard cannot build it or the goal lies farther, and
		// for a near spot while the yard queue builds something else, when the deployer costs at most
		// DeployerNearCostPct of the building. A deployer is asked of the vehicle factory (DeployerBuilds, at
		// most DeployerOrdersMost with the orders under way, never two for one building) and the yard takes
		// its next best pick. A building only a deployer brings, when none may be asked now, leaves the yard
		// to that next pick too.
		if (bestBldIdx >= 0)
		{
			auto& orderMacro = GetMacroState(pHouse);
			auto const pPick = BuildingTypeClass::Array.GetItem(bestBldIdx);
			auto const itDeployer = deployerOfBuilding.find(bestBldIdx);
			const bool yardBuildsPick = CanBuildUnderHumanRules(pHouse, pPick);
			const bool yardBuildingPick = inProgressBldIdx == bestBldIdx || inProgressDefIdx == bestBldIdx;
			const int ordersUnderWay = static_cast<int>(orderMacro.DeployerBuilds.size() + orderMacro.DeployOrders.size());
			const int yardNextIdx = yardBestIdx != bestBldIdx ? yardBestIdx : yardSecondIdx;
			bool deployerBrings = false;

			if (itDeployer != deployerOfBuilding.end() && !yardBuildingPick && !isOnTheWay(bestBldIdx) && ordersUnderWay < DeployerOrdersMost)
			{
				auto const pDeployerType = itDeployer->second;
				NeuralPlacementGoalKind goalKind = NeuralPlacementGoalKind::None;
				const CellStruct goal = FindDeployerGoal(pHouse, pPick, pDeployerType, nullptr, goalKind);
				const CellStruct yardCell = yardBuildsPick ? CachedTetrisBuildCell(pHouse, pPick) : CellStruct::Empty;
				const int yardGap = (yardCell != CellStruct::Empty && goal != CellStruct::Empty) ? static_cast<int>(yardCell.DistanceFrom(goal)) : -1;
				const bool farSpot = yardCell == CellStruct::Empty || yardGap > DeployerNearSpotCells;
				const int queueIdx = pPick->IsBaseDefense ? inProgressDefIdx : inProgressBldIdx;
				const bool yardBusy = queueIdx >= 0 && queueIdx != bestBldIdx;
				const bool cheapEnough = pDeployerType->GetActualCost(pHouse) * 100 <= pPick->GetActualCost(pHouse) * DeployerNearCostPct;
				deployerBrings = goal != CellStruct::Empty && (!yardBuildsPick || farSpot || (yardBusy && cheapEnough));

				if (deployerBrings)
				{
					orderMacro.DeployerBuilds.push_back({ pDeployerType->ArrayIndex, bestBldIdx, goal, goalKind, Unsorted::CurrentFrame });

					// TEMP diagnostic: each deployer the building pick asks for.
					if (DeployerLogAllowed(pHouse))
					{
						Debug::Log("[NeuralAI] House %d deployer order: %s from %s, %s goal (%d,%d), the yard %s (its cell %d cells from the goal), "
							"yard queue busy %d, %d credits, the yard takes %s, frame %d.\n",
							pHouse->ArrayIndex, pPick->ID, pDeployerType->ID, PlacementGoalKindName(goalKind), goal.X, goal.Y,
							yardBuildsPick ? "builds it" : "cannot build it", yardGap, yardBusy ? 1 : 0, money,
							yardNextIdx >= 0 ? BuildingTypeClass::Array.GetItem(yardNextIdx)->ID : "nothing", Unsorted::CurrentFrame);
					}
				}
			}

			if (deployerBrings || !yardBuildsPick)
				bestBldIdx = yardNextIdx;
		}

		pExt->NeuralAI_NextBuildingTarget = bestBldIdx;
	}

	productionClock.Mark(NeuralSection::ProdUnits);

	// Sea control (SeaControl*): read by the naval pick below, the crossing vehicles, the aircraft
	// and the bank governor.
	UpdateSeaControl(pHouse);
	auto& seaMacro = GetMacroState(pHouse);
	const bool seaControl = seaMacro.SeaControl;
	const bool seaControlAcross = seaControl && seaMacro.EnemyAcrossWater;
	seaMacro.SeaControlLongRangeShip = false;
	// Land control (LandControl*): the opposite, the enemy's remnants are at sea.
	UpdateLandControl(pHouse);
	const bool landControl = seaMacro.LandControl;

	// Unit & Naval Target Selection
	int bestUnitIdx = -1;
	int bestUnitScore = -32000;
	// The tank floor (TankFloor*): the best scored main battle tank, and whether the best pick restores
	// the income.
	int bestTankIdx = -1;
	int bestTankScore = -32000;
	bool bestUnitRestoresIncome = false;
	// The light complement (LightComplement*): the best scored armed light vehicle.
	int bestLightIdx = -1;
	int bestLightScore = -32000;
	// The repairer complement (Repairer*): the best scored repairer type the repairer cap lets through.
	int bestRepairerIdx = -1;
	int bestRepairerScore = -32000;
	// TEMP diagnostic: the ground vehicle candidates of this pick with the parts of their score (base
	// of cost and health, type penalty, counters, the class slot with its extras, the rest), logged
	// every 1800 frames per house: why main battle tanks lose the pick.
	struct UnitPickDiag
	{
		const UnitTypeClass* Type;
		char Class;
		int Base;
		int TypePenalty;
		int Counter;
		int ClassPart;
		int Rest;
		int Total;
	};

	std::vector<UnitPickDiag> unitPickDiags;
	// At most UnitPickDiagLines lines per match.
	constexpr int UnitPickDiagLines = 240;
	static int unitPickDiagSerial = -1;
	static int unitPickDiagCount = 0;

	if (unitPickDiagSerial != NeuralMatchSerial)
	{
		unitPickDiagSerial = NeuralMatchSerial;
		unitPickDiagCount = 0;
	}

	const bool logUnitPicks = unitPickDiagCount < UnitPickDiagLines && IsAITickWindow(1800, 0);
	// Tank bunkers of ours standing empty beyond the free tanks that fit them: a tank is built for
	// each on purpose (user: the original AI built units for its bunkers), the best one that fits.
	int bestBunkerTankIdx = -1;
	int bestBunkerTankScore = -32000;
	int tankBunkersToFill = 0;

	for (auto const pOwnBld : pHouse->Buildings)
	{
		if (pOwnBld && pOwnBld->IsAlive && !pOwnBld->InLimbo && pOwnBld->Type && pOwnBld->Type->Bunker
			&& pOwnBld->BunkerLinkedItem == nullptr)
		{
			tankBunkersToFill++;
		}
	}

	for (auto const pOwnU : UnitClass::Array)
	{
		if (tankBunkersToFill <= 0)
			break;

		if (pOwnU && pOwnU->Owner == pHouse && pOwnU->IsAlive && !pOwnU->InLimbo && pOwnU->Type && pOwnU->Type->Bunkerable
			&& !pOwnU->Type->Naval && !pOwnU->Type->Harvester && !pOwnU->Type->ResourceGatherer && pOwnU->BunkerLinkedItem == nullptr)
		{
			tankBunkersToFill--;
		}
	}
	int bestNavalIdx = -1;
	int bestNavalScore = -32000;
	bool ownsUndeployedMCV = false;
	bool ownsUndeployedKeyStructure = false;
	int ownedAmphibTransports = 0;
	int ownedHeavyIslandTransports = 0;
	int ownedUnarmedSupportVehicles = 0;
	// The support the tank floor escorts: unarmed vehicles, folded or not, without the unfolded siege
	// guns (they count as artillery).
	int ownedEscortedSupportVehicles = 0;
	// Mobile sensor arrays, folded or not (SensorLookoutMinSight).
	int ownedSensorLookouts = 0;
	int ownedTransportTotalCapacity = 0;
	int waitingBattalionSizeOnIslands = 0;
	int maxWaitingUnitSizeOnIslands = 0;
	int waitingSizeByUnitSize[16] = { 0 };
	int ownedTransCountByLimit[16] = { 0 };
	int ownedTransCapByLimit[16] = { 0 };
	int ownedIdleEmptyTransByLimit[16] = { 0 };
	int ownedNavalOrAmphibCombatUnits = 0;
	int ownedGroundCombatUnits = 0;
	// Main battle tanks alive (IsMainBattleTankType), for the tank floor (TankFloor*).
	int ownedMainTanks = 0;
	// Light complement (LightComplement*): armed light vehicles of ours, and whether the newest of our
	// main battle tanks and light vehicles is a light one.
	int ownedLightVehicles = 0;
	// The newest of our main battle tanks, light vehicles and repairers, and which of them it is.
	DWORD newestTankOrLightUid = 0;
	bool newestTankOrLightIsLight = false;
	bool newestTankOrLightIsRepairer = false;
	// Repairers of ours (IsRepairerUnit: an IFV with an engineer too), within ownedGroundCombatUnits.
	int ownedRepairerVehicles = 0;
	// Armed ground vehicles whose main gun reaches ArtilleryRangeCells, unfolded siege guns included
	// (the rest of ownedGroundCombatUnits is their front line).
	int ownedArtilleryVehicles = 0;
	// Armed ground vehicles that shoot at aircraft (ShootsAtAircraft): the escort of the tank mass.
	int ownedAntiAirVehicles = 0;
	// Of them the ones that are neither main battle tanks nor artillery: the tank floor escorts them.
	int ownedEscortedAntiAirVehicles = 0;
	int ownedLandStrikeTransports = 0;
	int idleLandStrikeTransports = 0;
	// Unit carryalls (transport helicopters with Carryall=yes): owned, and empty ones doing nothing.
	int ownedUnitCarryalls = 0;
	int idleUnitCarryalls = 0;
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
		// An MCV aboard one of our transports is still ours and still undeployed: out of sight in the
		// hold it let the next one be built, five MCVs for one yard (match #10267).
		if (pOwnedU && pOwnedU->Owner == pHouse && pOwnedU->InLimbo && pOwnedU->IsAlive && pOwnedU->Type
			&& pOwnedU->Transporter && pOwnedU->Transporter->Owner == pHouse
			&& pOwnedU->Type->DeploysInto && pOwnedU->Type->DeploysInto->ConstructionYard)
		{
			ownsUndeployedMCV = true;
		}

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
				{
					ownedNavalOrAmphibCombatUnits++;
				}
				else if (pOwnedWpn != nullptr)
				{
					ownedGroundCombatUnits++;
					const bool isOwnedRepairer = IsRepairerUnit(pOwnedU);

					if (isOwnedRepairer)
						ownedRepairerVehicles++;

					if (IsMainBattleTankType(pOwnedType))
						ownedMainTanks++;
					else if (pOwnedWpn->Range >= ArtilleryRangeCells * Unsorted::LeptonsPerCell)
						ownedArtilleryVehicles++;
					else if (ShootsAtAircraft(pOwnedType))
						ownedEscortedAntiAirVehicles++;

					// The same test as the light candidates of the pick (class L); a repairer is no fighter.
					const bool isOwnedLight = !IsMainBattleTankType(pOwnedType) && pOwnedWpn->Range < ArtilleryRangeCells * Unsorted::LeptonsPerCell
						&& !(pOwnedWpn->Projectile && pOwnedWpn->Projectile->AA) && pOwnedType->Passengers < 2 && !IsRepairerUnit(pOwnedU)
						&& !(TechnoTypeExt::Fetch(pOwnedType) && TechnoTypeExt::Fetch(pOwnedType)->Carryall);

					if (isOwnedLight)
						ownedLightVehicles++;

					// Below the tank floor the light and repairer complements alternate with it by the newest of them.
					if ((isOwnedLight || isOwnedRepairer || IsMainBattleTankType(pOwnedType)) && pOwnedU->UniqueID > newestTankOrLightUid)
					{
						newestTankOrLightUid = pOwnedU->UniqueID;
						newestTankOrLightIsLight = isOwnedLight;
						newestTankOrLightIsRepairer = isOwnedRepairer;
					}

					if (ShootsAtAircraft(pOwnedType))
						ownedAntiAirVehicles++;
				}
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
				// A unit carryall (e.g. a transport helicopter with Carryall=yes) slings one vehicle per
				// trip: as ferry capacity it only counts with its passenger hold (Passengers up to its
				// own SizeLimit, infantry in practice). Counted as Passengers places for vehicles up to
				// Carryall.SizeLimit, three of them "covered" the whole fleet and no amphibious
				// transport was ever built for an army waiting on its island (match #6164).
				auto const pOwnedTransExt = TechnoTypeExt::Fetch(pOwnedType);
				const bool unitCarryall = pOwnedTransExt && pOwnedTransExt->Carryall;
				const int effLimit = unitCarryall ? std::clamp(static_cast<int>(pOwnedType->SizeLimit), 0, 15)
					: getTransportEffectiveSizeLimit(pOwnedType);
				const int passCap = unitCarryall ? static_cast<int>(pOwnedType->Passengers)
					: std::max(1, static_cast<int>(pOwnedType->Passengers));

				if (passCap > 0)
				{
					ownedTransCountByLimit[effLimit]++;
					ownedTransCapByLimit[effLimit] += passCap;
				}

				if (effLimit >= 6 && passCap > 0)
					ownedHeavyIslandTransports++;

				// An empty transport parked without orders is unused shuttle capacity (an empty one left
				// on Unload with nothing to unload too: transport helicopters sat like that by the dozen).
				// One parked to load is not: its passengers are walking up, and counted as idle it kept the
				// factory from building the transports a battalion needed.
				const bool isParkedEmpty = pOwnedU->Passengers.NumPassengers == 0 && pOwnedU->Destination == nullptr
					&& (pOwnedU->CurrentMission == Mission::Guard || pOwnedU->CurrentMission == Mission::Area_Guard
						|| pOwnedU->CurrentMission == Mission::Sleep || pOwnedU->CurrentMission == Mission::Unload)
					&& !IsFerryParkedToLoad(pHouse, pOwnedU->UniqueID) && !IsInTransportOp(pHouse, pOwnedU->UniqueID);

				if (unitCarryall)
				{
					ownedUnitCarryalls++;
					auto const pCarryState = UnitExt::Fetch(pOwnedU);

					if (isParkedEmpty && (!pCarryState || !pCarryState->CarryallCargo))
						idleUnitCarryalls++;
				}

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
				ownedEscortedSupportVehicles++;
			}

			if (IsSensorLookoutType(pOwnedType))
				ownedSensorLookouts++;

			// Airlift jobs the carryalls really have (same rules as the carryall dispatcher):
			// critically damaged expensive vehicles to rescue and loaded strike APCs to drop, each one
			// a vehicle the engine gives the ride whose pickup did not go wrong lately.
			auto const pOwnedTypeExt = TechnoTypeExt::Fetch(pOwnedType);
			const bool airliftable = CarryallRideAccepted(pOwnedU) && !IsAirliftRefused(pOwnedU);

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

					if (airliftable && pHouse->UnitRepairStations.Count > 0 && pOwnedU->GetHealthPercentage() < 0.40)
						carryallLiftJobs++;
				}

				if (airliftable && pOwnedType->Passengers >= 3 && pOwnedU->Passengers.NumPassengers >= 1
					&& !IsAmphibiousOrAirTransport(pOwnedType))
				{
					carryallLiftJobs++;
				}
			}
		}
	}

	// The front line artillery needs: our other armed ground vehicles. Unfolded siege guns stand as
	// buildings and are artillery all the same.
	const int ownedFrontLineVehicles = std::max(0, ownedGroundCombatUnits - ownedArtilleryVehicles);

	for (auto const pOwnBld : pHouse->Buildings)
	{
		if (pOwnBld && pOwnBld->IsAlive && !pOwnBld->InLimbo && IsDeployedSiegeBuildingType(pOwnBld->Type)
			&& GroundRangeOf(pOwnBld->Type) >= ArtilleryRangeCells * Unsorted::LeptonsPerCell)
		{
			ownedArtilleryVehicles++;
		}
	}

	// The tank mass builds its front line first: no free gun, and one per TankMassFrontLinePerGun.
	// With a front line of TankMassBaseWave the mass may be of any kind (user: many offensive units
	// of other types are a mass too, mixed with the tanks) and it gets the guns of any house.
	const bool tankMass = pExt->NeuralAI_Archetype == TankMassArchetype;
	const int artilleryAllowance = tankMass
		? ownedFrontLineVehicles / TankMassFrontLinePerGun
		: ArtilleryFreeVehicles + ownedFrontLineVehicles / ArtilleryFrontLinePerGun;
	// The anti-air escort the tank mass wants with its front line (TankMassVehiclesPerEscort).
	const int wantedAntiAirEscorts = (tankMass && ownedFrontLineVehicles >= TankMassEscortFromVehicles)
		? std::max(1, ownedFrontLineVehicles / TankMassVehiclesPerEscort)
		: 0;
	// Anti-air vehicles in proportion to the uncovered air threat, any archetype (AntiAirUnits*).
	const int antiAirVehicleRoom = std::max(0, std::max(AntiAirUnitsFloor, ownedFrontLineVehicles * AntiAirUnitsMostPercent / 100) - ownedAntiAirVehicles);

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

		// A carryall of a transport operation parked by its vehicle is at work, not idle.
		auto const pAirType = pOwnAir->Type;
		const bool isAirIdle = pOwnAir->Passengers.NumPassengers == 0
			&& (pOwnAir->CurrentMission == Mission::Guard || pOwnAir->CurrentMission == Mission::Area_Guard
				|| pOwnAir->CurrentMission == Mission::Sleep)
			&& !IsInTransportOp(pHouse, pOwnAir->UniqueID);

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

			if (!IsDeployedSiegeBuildingType(pOwnB->Type) || GroundRangeOf(pOwnB->Type) < ArtilleryRangeCells * Unsorted::LeptonsPerCell)
				ownedEscortedSupportVehicles++;

			if (IsSensorLookoutType(pOwnB->Type->UndeploysInto))
				ownedSensorLookouts++;
		}
	}

	// Economy capacity: true refineries plus deployed miners define how many
	// harvesting units are useful
	int trueRefineries = 0;
	int ownedEconomyUnits = 0;
	int ownedCashProduces = 0;
	int deployedGatherers = 0;
	int productiveMiners = 0;
	int ownedMinerUnits = 0;
	auto const& minerOreSamples = GetMacroState(pHouse).MinerOreSamples;

	for (auto const pOwnBldType : pHouse->Buildings)
	{
		if (!pOwnBldType || pOwnBldType->InLimbo || !pOwnBldType->IsAlive || !pOwnBldType->Type)
			continue;

		if (pOwnBldType->Type->ProduceCashAmount > 0)
			ownedCashProduces++;

		if (pOwnBldType->Type->ResourceGatherer)
		{
			deployedGatherers++;

			// A deployed miner with no ore left in its slaves' reach earns nothing and takes no place
			// of the cap: a new miner goes to a field that has ore (UpdateStrandedSlaveMiners packs
			// the old one up as well).
			auto const itSamples = minerOreSamples.find(pOwnBldType->UniqueID);

			if (itSamples == minerOreSamples.end() || itSamples->second >= MinerProductiveOreSamples)
			{
				ownedEconomyUnits++;
				productiveMiners++;
			}
		}
		else if (pOwnBldType->Type->Refinery)
		{
			trueRefineries++;
		}
	}

	// Harvesters in limbo count too (mid chrono jump, inside a transport, being built).
	for (auto const pEcoUnit : UnitClass::Array)
	{
		if (!pEcoUnit || pEcoUnit->Owner != pHouse || !pEcoUnit->IsAlive || !pEcoUnit->Type)
			continue;

		auto const pEcoType = pEcoUnit->Type;

		if (pEcoType->Harvester || pEcoType->ResourceGatherer
			|| (pEcoType->DeploysInto && (pEcoType->DeploysInto->Refinery || pEcoType->DeploysInto->ResourceGatherer)))
		{
			ownedEconomyUnits++;
		}

		if (pEcoType->DeploysInto && pEcoType->DeploysInto->ResourceGatherer)
			ownedMinerUnits++;
	}

	const int ecoBiasMod = std::clamp(static_cast<int>(scores[39]) / 220, -1, 2);
	// About 2 harvesters per refinery, as many as the refineries ask for (they follow the fields we
	// hold, user); one per refinery with a bank of 50000+ credits (income is not what limits that
	// house: more harvesters only crowd the refineries).
	// A falling income (IncomeFalling, the harvesters drive farther to thinner ore): none is built past
	// one per refinery and one more (the ones we have stay); more of them only queue on the thinning
	// field, the expansion brings the next ones (user, 2026-11-18).
	const int economyUnitCap = (trueRefineries > 0)
		? (GetMacroState(pHouse).IncomeFalling
			? std::max(2, trueRefineries + 1)
			: std::max(2, money >= 50000 ? trueRefineries + 1 : 2 * trueRefineries + 1 + ecoBiasMod))
		: 4;

	// Mobile refineries: a house mining with deployed miners, each one refinery and harvesters in
	// one, matches its main enemy (user): as many miners as refineries we know that enemy has,
	// MinMobileMiners at least, never more than the ones mining now plus the free fields of its land
	// with ore to spare (ForEachMinerOreField), MaxMobileMiners at most. A miner on its way counts
	// against a free field. One per free field made Yuri the richest house by far (a deployed miner
	// earned 7500 per 4500 frames against 5600 for a refinery with its harvesters, 11 matches); a
	// fixed 4 had kept it poor with 68 free fields on its land (match #10437). Miners are no
	// expansion: nothing is built around them.
	int minerTarget = 0;
	int minerParity = MinMobileMiners;

	if (deployedGatherers > 0 || ownedMinerUnits > 0 || trueRefineries == 0)
	{
		const CellStruct minerHome = (pHouse->ConYards.Count > 0 && pHouse->ConYards.GetItem(0))
			? pHouse->ConYards.GetItem(0)->GetMapCoords()
			: pHouse->BaseSpawnCell;
		const int minerHomeZone = minerHome != CellStruct::Empty
			? MapClass::Instance.GetMovementZoneType(minerHome, MovementZone::Normal, false)
			: -1;
		const int freeMinerFields = CountMinerOreFields(pHouse, IsGroundZone(minerHomeZone) ? minerHomeZone : -1, MaxMobileMiners);

		// Refineries and deployed miners we know of, by owner: the main enemy's, or the most any
		// enemy has when we know none of the main enemy's.
		std::map<int, int> knownRefineriesByOwner;

		for (auto const& info : GetKnownEnemyBuildingCache(pHouse).Entries)
		{
			auto const pKnownBld = info.Building;

			if (pKnownBld && pKnownBld->Type && pKnownBld->Owner && (pKnownBld->Type->Refinery || pKnownBld->Type->ResourceGatherer))
				knownRefineriesByOwner[pKnownBld->Owner->ArrayIndex]++;
		}

		int enemyRefineries = 0;

		if (auto const itMain = knownRefineriesByOwner.find(pHouse->EnemyHouseIndex); itMain != knownRefineriesByOwner.end())
		{
			enemyRefineries = itMain->second;
		}
		else
		{
			for (auto const& [owner, count] : knownRefineriesByOwner)
				enemyRefineries = std::max(enemyRefineries, count);
		}

		minerParity = std::min(std::max(MinMobileMiners, enemyRefineries), MaxMobileMiners);
		minerTarget = std::clamp(std::min(minerParity, productiveMiners + freeMinerFields), 1, MaxMobileMiners);
	}

	// Fewer harvesters than refineries (a refinery nobody feeds): the cheapest buildable harvester
	// is an economic investment the economy hold saves for (UpdateEconomyCrisis).
	{
		int harvesterCost = 0;

		if (trueRefineries > 0 && ownedEconomyUnits < trueRefineries)
		{
			for (auto const pType : UnitTypeClass::Array)
			{
				if (pType && pType->Harvester && !pType->DeploysInto && CanBuildUnderHumanRules(pHouse, pType))
				{
					const int cost = pType->GetActualCost(pHouse);
					harvesterCost = harvesterCost == 0 ? cost : std::min(harvesterCost, cost);
				}
			}
		}

		GetMacroState(pHouse).HarvesterSaveTarget = harvesterCost;
	}

	// Emergency ConYard Reconstruction: If we lost all ConYards and have no MCV
	// on the map, check if our War Factory + Service Depot/Grinder/Tech can
	// build a replacement MCV and whether our economy/situation allows saving up
	// the $3000 for it instead of draining credits on cheap units!
	// MCV airlift: can a carryall (aircraft Carryall=yes or Phobos unit Carryall) lift this
	// vehicle type? Same rules as the dispatcher's CanCarryallLiftUnit, at type level.
	auto const carryallCanLiftType = [](const TechnoTypeClass* pCarryType, bool isCarryall, const UnitTypeClass* pCargo) -> bool
	{
		return isCarryall && CarryallTypeCanLiftType(pCarryType, pCargo);
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

	// The ore of the landmasses we hold runs out soon and the next field is across the water
	// (UpdateIslandOreOutlook): the island MCV is wanted now, while it can still be paid.
	UpdateIslandOreOutlook(pHouse);
	const bool islandOreRunningOut = GetMacroState(pHouse).IslandExpansionUrgent;
	const bool hasUncolonizedIslandTarget
		= (features[21] == 50 || features[21] >= 100 || (features[21] > 0 && features[20] > 6) || islandOreRunningOut);

	// Our fields running dry (half or more of our refineries with fewer than 6 ore cells left
	// within 12 cells) while a free field or island is there to take: the next MCV is wanted
	// whatever the learned expansion slots say, and saving for it starts below 1400 credits.
	{
		auto& depletion = GetMacroState(pHouse);

		if (Unsorted::CurrentFrame - depletion.RefineryDepletionFrame >= 900 || depletion.RefineryDepletionFrame > Unsorted::CurrentFrame)
		{
			depletion.RefineryDepletionFrame = Unsorted::CurrentFrame;
			depletion.RefineriesChecked = 0;
			depletion.RefineriesDepleted = 0;

			for (auto const pRef : pHouse->Buildings)
			{
				if (!pRef || !pRef->IsAlive || pRef->InLimbo || !pRef->Type || !(pRef->Type->Refinery || pRef->Type->ResourceDestination)
					|| pRef->Type->ResourceGatherer || pRef->Type->UndeploysInto)
				{
					continue;
				}

				depletion.RefineriesChecked++;
				const CellStruct rc = pRef->GetMapCoords();
				int oreCells = 0;

				for (int dy = -12; dy <= 12 && oreCells < 6; dy += 2)
				{
					for (int dx = -12; dx <= 12 && oreCells < 6; dx += 2)
					{
						auto const pCell = MapClass::Instance.TryGetCellAt(CellStruct { static_cast<short>(rc.X + dx), static_cast<short>(rc.Y + dy) });

						if (pCell && pCell->GetContainedTiberiumValue() > 0)
							oreCells++;
					}
				}

				if (oreCells < 6)
					depletion.RefineriesDepleted++;
			}
		}
	}

	// Half our refineries depleted, or a falling income (IncomeFalling, user 2026-11-18: plan the
	// expansion before the credits run out, the depleted refineries came long after the income fell),
	// with a free field to go to: the expansion MCV is planned now.
	const bool fieldsRunningDry = ((GetMacroState(pHouse).RefineriesChecked > 0
			&& GetMacroState(pHouse).RefineriesDepleted * 2 >= GetMacroState(pHouse).RefineriesChecked)
		|| GetMacroState(pHouse).IncomeFalling)
		&& (hasUncolonizedIslandTarget || GetMacroState(pHouse).UncolonizedOreFields > 0);
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
	const bool needsCrossIslandMCV = islandOreRunningOut
		|| (features[21] > 0 && !hasFreeHomeLandField && (features[19] == 0 || pHouse->NumConYards >= 1));
	// An island expansion MCV that unfolded while no ride came (land before islands, Micro) is the
	// island's MCV: it packs up when a transport is free, so no other one is built for the crossing.
	const bool islandMcvWaitsAsYard = needsCrossIslandMCV && PruneIslandWaitYards(pHouse) > 0;

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
		= (pHouse->NumConYards >= 1 && pHouse->NumConYards < maxAllowedConYards && !ownsUndeployedMCV && !islandMcvWaitsAsYard
			&& (features[20] > 6 || hasUncolonizedIslandTarget || fieldsRunningDry)
			&& (scores[30] >= 450 || wantsArchipelagoExpansion || fieldsRunningDry || islandOreRunningOut)
			&& (!needsCrossIslandMCV || ownedHeavyIslandTransports >= 1 || canFerryAnyMCVAcrossWater)
			&& (money >= 1400 || desperateIslandOrOreExpansion || fieldsRunningDry || islandOreRunningOut));
	const bool canSafelySaveForMCV = canSafelySaveForReplacementMCV || canSafelySaveForExpansionMCV;

	// Fields dry (or the island ones running out) and an expansion MCV wanted: the price of the
	// cheapest buildable MCV is the saving target the economy crisis logic holds the rest of the
	// production for.
	{
		int expansionCost = 0;

		if ((fieldsRunningDry || islandOreRunningOut) && canSafelySaveForExpansionMCV && !ownsUndeployedMCV)
		{
			for (auto const pType : UnitTypeClass::Array)
			{
				if (pType && pType->DeploysInto && pType->DeploysInto->ConstructionYard && CanBuildUnderHumanRules(pHouse, pType))
					expansionCost = expansionCost == 0 ? pType->GetActualCost(pHouse) : std::min(expansionCost, pType->GetActualCost(pHouse));
			}
		}

		// An island expansion saves for its MCV while the MCV's missing buildings (a service depot,
		// a tech center) are built, as long as one of them can be built now: the credits went to the
		// army before the MCV became buildable (Heisenberg repro). Those buildings are not held.
		if (expansionCost == 0 && islandOreRunningOut && canSafelySaveForExpansionMCV && !ownsUndeployedMCV)
		{
			auto const& islandPrereqs = GetMacroState(pHouse).IslandMcvPrerequisites;
			const bool prereqBuildable = std::any_of(islandPrereqs.begin(), islandPrereqs.end(),
				[pHouse](int idx) { return CanBuildUnderHumanRules(pHouse, BuildingTypeClass::Array.GetItem(idx)); });

			for (auto const pType : UnitTypeClass::Array)
			{
				if (prereqBuildable && pType && pType->DeploysInto && pType->DeploysInto->ConstructionYard && pType->TechLevel >= 0
					&& (ownHouseBit == 0u || pType->InOwners(ownHouseBit)))
				{
					expansionCost = expansionCost == 0 ? pType->GetActualCost(pHouse) : std::min(expansionCost, pType->GetActualCost(pHouse));
				}
			}
		}

		GetMacroState(pHouse).ExpansionSaveTarget = expansionCost;
	}

	// TEMP diagnostic (income forecast, 2026-11-18): while the income falls, why the expansion MCV is
	// planned or not (none of 14 falling houses gained a yard within 9000 frames).
	if (GetMacroState(pHouse).IncomeFalling && IsAITickWindow(4500, 0))
	{
		Debug::Log("[NeuralAI] House %d income falling DIAG (expansion): fields running dry %d (depleted %d of %d refineries), free fields %d, "
			"island target %d, yards %d of %d allowed, undeployed MCV %d, features20 %d, slot30 %d, cross-island %d (heavy transports %d, "
			"ferry %d), can save %d, save target %d, money %d, frame %d.\n", pHouse->ArrayIndex, fieldsRunningDry ? 1 : 0,
			GetMacroState(pHouse).RefineriesDepleted, GetMacroState(pHouse).RefineriesChecked, GetMacroState(pHouse).UncolonizedOreFields,
			hasUncolonizedIslandTarget ? 1 : 0, pHouse->NumConYards, maxAllowedConYards, ownsUndeployedMCV ? 1 : 0,
			static_cast<int>(features[20]), static_cast<int>(scores[30]), needsCrossIslandMCV ? 1 : 0, ownedHeavyIslandTransports,
			canFerryAnyMCVAcrossWater ? 1 : 0, canSafelySaveForExpansionMCV ? 1 : 0, GetMacroState(pHouse).ExpansionSaveTarget, money,
			Unsorted::CurrentFrame);
	}

	// With each IslandExpansion outlook line, what the island MCV waits for.
	if (islandOreRunningOut && GetMacroState(pHouse).IslandExpansionLogFrame == Unsorted::CurrentFrame)
	{
		// Our side's MCVs: buildable, or the buildings of their Prerequisite list we lack.
		std::string mcvState;

		for (auto const pMcvType : UnitTypeClass::Array)
		{
			if (!pMcvType || !pMcvType->DeploysInto || !pMcvType->DeploysInto->ConstructionYard || pMcvType->TechLevel < 0
				|| (ownHouseBit != 0u && !pMcvType->InOwners(ownHouseBit)))
			{
				continue;
			}

			mcvState += ' ';
			mcvState += pMcvType->ID;

			if (CanBuildUnderHumanRules(pHouse, pMcvType))
			{
				mcvState += " buildable";
				continue;
			}

			mcvState += " lacks";

			for (const int prereq : MissingTypePrerequisites(pHouse, pMcvType))
			{
				if (prereq < BuildingTypeClass::Array.Count)
				{
					mcvState += ' ';
					mcvState += BuildingTypeClass::Array.GetItem(prereq)->ID;
				}
			}
		}

		Debug::Log("[NeuralAI] House %d IslandExpansion gates: war factories %d, shipyards %d, heavy transports %d, MCV ferryable %d, "
			"MCV owned %d, yards %d/%d, saving %d (target %d), %d credits, MCV:%s.\n",
			pHouse->ArrayIndex, pHouse->NumWarFactories, pHouse->NumShipyards, ownedHeavyIslandTransports, canFerryAnyMCVAcrossWater ? 1 : 0,
			ownsUndeployedMCV ? 1 : 0, pHouse->NumConYards, maxAllowedConYards, canSafelySaveForExpansionMCV ? 1 : 0,
			GetMacroState(pHouse).ExpansionSaveTarget, money, mcvState.c_str());
	}

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

	// A deployed miner (a building that gathers by itself, like the Slave Miner) is refinery and
	// harvester in one: a house living on those owns no refinery at all and still has an income.
	// Money that keeps coming in (the bank governor's measure, credits per 1000 frames) is income
	// too, whatever brings it.
	const bool hasActiveUnitIncome = (ownedCashProduces >= 1)
		|| (((trueRefineries >= 1 && ownedEconomyUnits >= 1) || deployedGatherers >= 1)
			&& (features[19] > 0 || pHouse->OwnedTiberium.GetTotalAmount() > 0))
		|| GetMacroState(pHouse).BankIncomeRate >= 1000;

	// Raid transport demand (AirborneFront plan): a raider or spy of ours waits for a ride behind
	// the enemy lines and no flying or amphibious passenger transport (3 seats or more) is owned: one is
	// worth building even without a water crossing.
	bool raidTransportWanted = false;
	{
		int raidCandidates = 0;
		int raidTransports = 0;

		for (auto const pOwnFoot : FootClass::Array)
		{
			if (!pOwnFoot || pOwnFoot->Owner != pHouse || !pOwnFoot->IsAlive)
				continue;

			if (auto const pOwnInf = abstract_cast<InfantryClass*>(pOwnFoot))
			{
				if (IsRaiderInfantryType(pOwnInf->Type) || IsSpyInfantryType(pOwnInf->Type))
					raidCandidates++;
			}
			else if (auto const pOwnType = pOwnFoot->GetTechnoType(); pOwnType && pOwnType->Passengers >= 3
				&& (pOwnFoot->WhatAmI() == AbstractType::Aircraft || pOwnType->JumpJet || pOwnType->ConsideredAircraft
					|| pOwnType->MovementZone == MovementZone::Fly
					|| IsAmphibiousGroundTransportType(abstract_cast<UnitTypeClass*>(pOwnType))))
			{
				raidTransports++;
			}
		}

		raidTransportWanted = raidCandidates > 0 && raidTransports == 0;
	}

	// Bonus for a flying or amphibious passenger transport that would answer the raid demand.
	auto raidTransportBonus = [&](const TechnoTypeClass* pCandidate) -> int
	{
		if (!raidTransportWanted || !pCandidate || pCandidate->Passengers < 3)
			return -1000;

		const bool flies = pCandidate->WhatAmI() == AbstractType::AircraftType || pCandidate->JumpJet
			|| pCandidate->ConsideredAircraft || pCandidate->MovementZone == MovementZone::Fly;
		const bool amphibious = IsAmphibiousGroundTransportType(abstract_cast<const UnitTypeClass*>(pCandidate));

		return (flies || amphibious) ? 350 : -1000;
	};

	// A transport that makes a crossing our units wait for and no transport of ours makes
	// (NoteCrossingNeed): one of it is built.
	auto crossingNeedBonus = [&](const TechnoTypeClass* pCandidate) -> int
	{
		auto const& macro = GetMacroState(pHouse);

		if (macro.CrossingNeedFrame < 0 || Unsorted::CurrentFrame < macro.CrossingNeedFrame
			|| Unsorted::CurrentFrame - macro.CrossingNeedFrame > CrossingNeedFrames
			|| std::find(macro.CrossingNeedTypes.begin(), macro.CrossingNeedTypes.end(), pCandidate) == macro.CrossingNeedTypes.end()
			|| pHouse->CountOwnedNow(pCandidate) > 0)
		{
			return -1000;
		}

		return std::max(static_cast<int>(scores[28]), 0) + CrossingTransportBonus;
	};

	// Ferry transport demand-driven scoring shared by the Shipyard and War Factory.
	// Transports are reusable shuttles: one stays ready on island maps, and more are
	// only built (up to 3) while the real waiting cargo exceeds one trip of the fleet.
	auto scoreFerryTransport = [&](const TechnoTypeClass* pCandidate) -> int
	{
		// A unit carryall ferries with its passenger hold (own SizeLimit), as the owned ones are
		// counted: judged by its Carryall.SizeLimit it never saw the ones already built and the
		// queue built transport helicopters without end (match #6194: 22 HIND, 17 parked).
		auto const pCandTypeExt = TechnoTypeExt::Fetch(pCandidate);
		const bool candUnitCarryall = pCandidate->WhatAmI() == AbstractType::UnitType && pCandTypeExt && pCandTypeExt->Carryall;
		const int candSizeLimit = std::max(1, candUnitCarryall ? static_cast<int>(pCandidate->SizeLimit)
			: getTransportEffectiveSizeLimit(pCandidate));
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

			const int buildingLimit = (pBuildingExt && pBuildingExt->Carryall)
				? static_cast<int>(pBuilding->SizeLimit) : getTransportEffectiveSizeLimit(pBuilding);

			if (buildingLimit >= candSizeLimit)
				ownedForLimit++;

			if (buildingLimit >= 6)
				compatibleHeavyInProgress = true;
		}

		const bool isIslandWar = features[21] >= 75;
		const bool isIslandMap = features[21] > 0;
		// The fleet follows what is waiting, with no fixed cap (a cap of 3 left armies of 100+ units
		// on their island with three boats shuttling, match #6164): the transports that take it in
		// OpTripsPlanned trips. One for every load built 39 amphibious transports for the 290 units of one
		// wave operation while its staging beach was the bottleneck (match #12411).
		// What keeps it from spamming is reuse: while a transport of this size sits empty and idle
		// nothing new is built (below), and the transport controller gives it the waiting cargo. An
		// island map keeps one at least.
		const int loadsPerTransport = candPassCap * OpTripsPlanned;
		const int neededForDemand = (fittingDemandSize + loadsPerTransport - 1) / loadsPerTransport;
		const int targetFleet = isIslandWar ? std::max(1, neededForDemand)
			: (neededForDemand > 0 ? neededForDemand : (isIslandMap ? 1 : 0));
		const bool needsHeavyMcvTransport = candSizeLimit >= 6 && ownedHeavyIslandTransports == 0
			&& !compatibleHeavyInProgress && (isIslandMap || scores[30] >= 450 || islandOreRunningOut);
		// The kind of transport by the danger of the crossings waiting for one (user): a ship scores lower
		// under a sea of warships, a flyer under heavy anti-air, a burrower never.
		const int crossingPenalty = OpCrossingDangerPenalty(pHouse, pCandidate);

		if (needsHeavyMcvTransport)
			return scores[28] + 2200 - crossingPenalty;

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
				const int ownedCarriers = (candUnitCarryall ? ownedUnitCarryalls : ownedForLimit) + ownedCarryallAircraft;
				const int idleCarriers = (candUnitCarryall ? idleUnitCarryalls : idleEmptyForLimit) + idleCarryallAircraft;

				if (ownedCarriers < wantedCarryalls && idleCarriers == 0)
				{
					return std::max(static_cast<int>(scores[28]), 0) + (carryallLiftJobs > 0 ? 700 : 350);
				}
			}
			else if (pCandidate->WhatAmI() == AbstractType::UnitType && !pCandidate->Naval
				&& IsAmphibiousOrAirTransport(pCandidate))
			{
				// Land strike APC: one per load of the riders that would really board it, no fixed cap
				// (user: the transport operations manager is to handle them, but for stealth and commando
				// missions).
				const int riders = ownedStrikeInfantry
					+ ((pCandidate->MovementZone == MovementZone::Subterrannean) ? idleCombatInfantry : 0);
				const int wantedLand = std::max(0, (riders + candPassCap - 1) / candPassCap);

				if (ownedLandStrikeTransports < wantedLand && idleLandStrikeTransports == 0)
					return std::max(static_cast<int>(scores[28]), 0) + 350;
			}
		}

		if (ownedForLimit >= targetFleet || idleEmptyForLimit > 0)
			return -1500;

		if (ownedForLimit == 0)
			return std::max(static_cast<int>(scores[28]), 0) + (fittingDemandSize > 0 ? 1100 : 600) - crossingPenalty;

		if (fittingDemandSize > ownedCapForLimit * OpTripsPlanned)
			return std::max(static_cast<int>(scores[28]), 0) + 900 - crossingPenalty;

		return -900;
	};

	// Exceptions to the economy unit cap for real harvesters, computed once per tick: a refinery
	// zone no harvester of ours works (with or without a war factory there), or another landmass
	// than the main base with a war factory and fewer than 2 harvesters per refinery. A building
	// stands in the zone of the cells around its foundation (the cells under it can carry another
	// zone), harvesters in limbo (being built, inside a transport, mid chrono jump) still count,
	// and the main base never takes the island exception (2 harvesters per refinery there would
	// pass the cap whenever the base has more than 9 refineries).
	bool economyZonesComputed = false;
	bool zeroHarvesterZoneWithFactory = false;
	bool zeroHarvesterZoneNoFactory = false;
	bool underharvestedIslandWithFactory = false;

	auto const computeEconomyZones = [&]()
	{
		economyZonesComputed = true;

		auto const groundZoneOf = [](const BuildingClass* pBld) -> int
		{
			const CellStruct cell = pBld->GetMapCoords();
			const int width = pBld->Type->GetFoundationWidth();
			const int height = pBld->Type->GetFoundationHeight(false);
			std::map<int, int> zoneCounts;

			for (int dy = -1; dy <= height; ++dy)
			{
				for (int dx = -1; dx <= width; ++dx)
				{
					if (dx >= 0 && dx < width && dy >= 0 && dy < height)
						continue;

					const CellStruct around { static_cast<short>(cell.X + dx), static_cast<short>(cell.Y + dy) };

					if (!MapClass::Instance.CoordinatesLegal(around))
						continue;

					const int zone = MapClass::Instance.GetMovementZoneType(around, MovementZone::Normal, false);

					if (zone > 0)
						zoneCounts[zone]++;
				}
			}

			int bestZone = -1;
			int bestCount = 0;

			for (auto const& [zone, count] : zoneCounts)
			{
				if (count > bestCount)
				{
					bestCount = count;
					bestZone = zone;
				}
			}

			return bestZone;
		};

		std::map<int, int> refineriesInZone;
		std::map<int, int> factoriesInZone;
		std::map<int, int> harvestersInZone;
		int mainZone = -1;

		for (auto const pOB : pHouse->Buildings)
		{
			if (!pOB || pOB->InLimbo || !pOB->IsAlive || !pOB->Type)
				continue;

			const bool isRefinery = pOB->Type->Refinery || pOB->Type->ResourceDestination;
			const bool isWarFactory = pOB->Type->Factory == AbstractType::UnitType && !pOB->Type->Naval;

			if (!isRefinery && !isWarFactory && !(pOB->Type->ConstructionYard && mainZone < 0))
				continue;

			const int zone = groundZoneOf(pOB);

			if (zone < 0)
				continue;

			if (isRefinery)
				refineriesInZone[zone]++;

			if (isWarFactory)
				factoriesInZone[zone]++;

			if (pOB->Type->ConstructionYard && mainZone < 0)
				mainZone = zone;
		}

		if (mainZone < 0 && pHouse->BaseSpawnCell != CellStruct::Empty)
			mainZone = MapClass::Instance.GetMovementZoneType(pHouse->BaseSpawnCell, MovementZone::Normal, false);

		for (auto const pHU : UnitClass::Array)
		{
			if (pHU && pHU->Owner == pHouse && pHU->IsAlive && pHU->Type && (pHU->Type->Harvester || pHU->Type->ResourceGatherer)
				&& !pHU->Type->DeploysInto)
			{
				harvestersInZone[ObjectZone(pHU, MovementZone::Normal)]++;
			}
		}

		for (auto const& [zone, refineries] : refineriesInZone)
		{
			auto const harvIt = harvestersInZone.find(zone);
			const int harvesters = harvIt != harvestersInZone.end() ? harvIt->second : 0;
			auto const facIt = factoriesInZone.find(zone);
			const bool hasFactory = facIt != factoriesInZone.end() && facIt->second > 0;

			if (harvesters == 0)
			{
				if (hasFactory)
					zeroHarvesterZoneWithFactory = true;
				else
					zeroHarvesterZoneNoFactory = true;
			}
			else if (hasFactory && zone != mainZone && harvesters < refineries * 2)
			{
				underharvestedIslandWithFactory = true;
			}
		}
	};

	// Bank governor (UpdateBankGovernor): one more ConYard for the building queue on land maps,
	// past the expansion gates; the MCV unfolds near the base (wantsMultiConYardSpeed in Micro).
	const bool bankWantsYard = GetMacroState(pHouse).BankGovernorActive && GetMacroState(pHouse).BankWantedYards > pHouse->NumConYards
		&& pHouse->NumConYards >= 1 && features[21] == 0;
	int bankYardMcvIdx = -1;

	// The tank floor (TankFloor*): how many main battle tanks this house keeps alive, the base defense
	// and one escort for each artillery (unfolded siege guns too), anti-air and support vehicle.
	const int tankFloor = std::min(TankFloorMost, std::max(TankFloorMin
		+ std::clamp(trueRefineries - TankFloorRefineries, 0, TankFloorEconomyMost), ownedGroundCombatUnits / TankFloorShare)
		+ ownedArtilleryVehicles + ownedEscortedAntiAirVehicles + ownedEscortedSupportVehicles);
	const bool belowTankFloor = ownedMainTanks < tankFloor;
	GetMacroState(pHouse).TankFloorBasePart = TankFloorMin + std::clamp(trueRefineries - TankFloorRefineries, 0, TankFloorEconomyMost);
	GetMacroState(pHouse).TankFloorEscortPart = ownedArtilleryVehicles + ownedEscortedAntiAirVehicles + ownedEscortedSupportVehicles;

	// TEMP diagnostic (economy hold): how far the unit a hold saves for (a harvester, an MCV) gets in this
	// pick: 1 buildable, 2 past the ban, tier, repairer and MCV gates, 3 past the economy unit gates,
	// 4 past the cash gate, 5 scored (awaitedUnitScore).
	auto& holdMacro = GetMacroState(pHouse);
	const int awaitedUnitKind = holdMacro.EconomyHoldKind == 4 ? 1 : holdMacro.EconomyHoldKind;
	int awaitedUnitStage = 0;
	int awaitedUnitScore = -32000;
	// The best harvester and the best MCV past the rules of this pick, for the economy hold
	// (HarvesterSaveTarget, ExpansionSaveTarget).
	int awaitedHarvesterIdx = -1;
	int awaitedHarvesterScore = -32000;
	int awaitedMcvIdx = -1;
	int awaitedMcvScore = -32000;

	// No expansion MCV while losing (ExpansionMcvAdvantageMin, ExpansionMcvLossFrames): our army
	// against the army of the strongest enemy seen, and the last ConYard of ours destroyed.
	auto const& mcvWaveState = GetWaveTimingState(pHouse);
	int strongestEnemyArmy = 0;

	for (auto const& enemy : mcvWaveState.Enemies)
		strongestEnemyArmy = std::max(strongestEnemyArmy, enemy.Army);

	const bool expansionMcvWaits = pHouse->NumConYards >= 1
		&& ((strongestEnemyArmy > 0 && mcvWaveState.OwnArmyValue * 100 < strongestEnemyArmy * ExpansionMcvAdvantageMin)
			|| Unsorted::CurrentFrame - GetMacroState(pHouse).ConYardLostFrame < ExpansionMcvLossFrames);

	for (auto const pUnit : UnitTypeClass::Array)
	{
		if (!pUnit || !CanBuildUnderHumanRules(pHouse, pUnit))
			continue;

		const bool isAwaitedUnit = (awaitedUnitKind == 1 && pUnit->Harvester && !pUnit->DeploysInto)
			|| (awaitedUnitKind == 3 && pUnit->DeploysInto && pUnit->DeploysInto->ConstructionYard);

		if (isAwaitedUnit)
			awaitedUnitStage = std::max(awaitedUnitStage, 1);

		const bool isUnitInProgress
			= (pUnit->ArrayIndex == inProgressUnitIdx || pUnit->ArrayIndex == inProgressNavalIdx);

		// A pick that never came out of its factory (WatchUndeliveredPick).
		if (!isUnitInProgress && IsProductionBanned(pHouse, pUnit->Naval ? 3 : 1, pUnit->ArrayIndex))
			continue;

		if (!isUnitInProgress && !IsUnitTierAllowed(pHouse, pUnit))
			continue;

		// Repairers are support (Repairer*): one for each RepairerPerVehicles armed ground vehicles of ours.
		if (!isUnitInProgress && IsRepairerUnitType(pUnit)
			&& (ownedRepairerVehicles + 1) * RepairerPerVehicles > ownedGroundCombatUnits - ownedRepairerVehicles)
		{
			continue;
		}

		const bool isMCV = (pUnit->DeploysInto && pUnit->DeploysInto->ConstructionYard);
		if (isMCV)
		{
			const bool isSelfAmphibOrAirMcv = (pUnit->MovementZone == MovementZone::Amphibious
				|| pUnit->MovementZone == MovementZone::AmphibiousDestroyer
				|| pUnit->MovementZone == MovementZone::AmphibiousCrusher || pUnit->MovementZone == MovementZone::Fly
				|| pUnit->JumpJet);
			if (ownsUndeployedMCV
				|| (!bankWantsYard
					&& ((pHouse->NumConYards >= 1 && scores[30] < 400 && scores[81] < 85 && !fieldsRunningDry && !islandOreRunningOut)
						|| pHouse->NumConYards >= maxAllowedConYards
						|| (pHouse->NumConYards >= 1
							&& (pHouse->Buildings.Count >= 35 || ownedGroundCombatUnits + ownedNavalOrAmphibCombatUnits >= 40)
							&& (freeOreFieldsForYards == 0 || money < 5000)))))
			{
				continue;
			}

			if (!bankWantsYard && pHouse->NumConYards >= 1 && features[20] <= 6 && !hasUncolonizedIslandTarget && !fieldsRunningDry)
				continue;

			// While losing, the vehicle queue builds the army, not another base (one under way is finished).
			if (expansionMcvWaits && !isUnitInProgress)
				continue;

			// A house with a ConYard but nothing gathering rebuilds its income first (a harvester, or a
			// refinery and its free harvester): an expansion MCV would take its last credits and the
			// new base would have no harvester either.
			if (pHouse->NumConYards >= 1 && ownedEconomyUnits == 0 && ownedCashProduces == 0)
				continue;

			if (pHouse->NumConYards >= 1 && needsCrossIslandMCV
				&& (!CanHouseFerryTypeAcrossWater(pHouse, pUnit)
					|| (!isSelfAmphibOrAirMcv && ownedHeavyIslandTransports == 0 && !ownsCarryallLifting(pUnit))
					|| (islandMcvWaitsAsYard && !bankWantsYard)))
			{
				continue;
			}

			if (canSafelySaveForMCV)
			{
				canRebuildMCVNow = true;
				rebuildMCVCost = pUnit->GetActualCost(pHouse);
			}
		}

		if (isAwaitedUnit)
			awaitedUnitStage = std::max(awaitedUnitStage, 2);

		const auto pWpnStruct = pUnit->GetWeapon(0);
		const auto pWpn = pWpnStruct ? pWpnStruct->WeaponType : nullptr;
		// A main battle tank stays one whatever its passengers, range, projectile or damage figure say.
		const bool isMainTank = IsMainBattleTankType(pUnit);

		// A base deployer (Nanocore, mobile factory, outpost: NeuralAI.MobileFactory.inl) never competes as a
		// vehicle of the army: the building pick weighs it as the building it becomes and asks the vehicle
		// factory for it (DeployerBuilds, after this pick). As a combat unit the mobile factories won 13 picks
		// of matches #13416..#13473 and the tank floor took all 13.
		if (IsBaseDeployerUnit(pUnit))
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
		// A slave miner (deploys into a building that gathers by itself): a mobile refinery.
		const bool isMobileMiner = isDeployableRefineryOrMiner && pUnit->DeploysInto->ResourceGatherer;
		// Not with 50000+ credits in the bank: income is not what limits that house, and the miners
		// would keep the vehicle queue from the army. A mobile miner is no expansion (nothing is built
		// around it, user): on land minerTarget alone sets their number; across water it is income
		// from another island, with MaxMobileMiners at most.
		const bool allowExpansionRefineryDeployer = isDeployableRefineryOrMiner && money < 50000
			&& (isMobileMiner ? features[21] > 0 && productiveMiners + ownedMinerUnits < minerParity : (features[21] > 0 || scores[30] >= 500))
			&& (trueRefineries < 10 && ownedEconomyUnits < 18)
			&& !ownsUndeployedKeyStructure && (features[21] == 0 || CanHouseFerryTypeAcrossWater(pHouse, pUnit));

		bool hasRefineryZoneWithZeroHarvesters = false;
		bool hasUnderharvestedIslandWithWarFactory = false;
		// Only for real harvesters: a slave miner deploys into its own refinery and then counts as
		// a building, never as a harvester of the zone, so these exceptions let Yuri build them
		// without end (77 deployed slave miners in one match).
		if ((pUnit->Harvester || pUnit->ResourceGatherer) && !isDeployableRefineryOrMiner)
		{
			if (!economyZonesComputed)
				computeEconomyZones();

			hasRefineryZoneWithZeroHarvesters = zeroHarvesterZoneWithFactory
				|| (zeroHarvesterZoneNoFactory && CanHouseFerryTypeAcrossWater(pHouse, pUnit));
			hasUnderharvestedIslandWithWarFactory = underharvestedIslandWithFactory;
		}

		const bool isEconomyUnit = pUnit->Harvester || pUnit->ResourceGatherer || isDeployableRefineryOrMiner;
		if (isEconomyUnit && !isMobileMiner && ownedEconomyUnits >= economyUnitCap && !allowExpansionRefineryDeployer
			&& !hasRefineryZoneWithZeroHarvesters && !hasUnderharvestedIslandWithWarFactory)
		{
			continue;
		}

		// The army of the tank mass comes once its economy stands (TankMassRefineries). A house with
		// no income left, or with a refinery that has no harvester, is not held.
		if (tankMass && isEconomyUnit && !isUnitInProgress && hasActiveUnitIncome && !hasRefineryZoneWithZeroHarvesters
			&& ownedFrontLineVehicles < TankMassBaseWave && pHouse->CountResourceDestinations >= TankMassRefineries
			&& (isMobileMiner || isDeployableRefineryOrMiner || ownedEconomyUnits >= TankMassGatherersPerRefinery * TankMassRefineries))
		{
			continue;
		}

		// Mobile miners follow the free fields (minerTarget): the ones mining (a miner without ore
		// takes no place) and the ones on their way.
		if (isMobileMiner && !isUnitInProgress && !allowExpansionRefineryDeployer
			&& productiveMiners + ownedMinerUnits >= minerTarget)
		{
			continue;
		}

		// The zone exceptions never pass 2 harvesters per refinery + 2: a harvester built for an
		// island or an empty refinery zone may not stay there (a chrono miner teleports to any
		// refinery and mines where the ore is), the zone keeps looking empty and every pick built
		// another one (match #6439, No Rest for the Wicked: 200 Chrono Miners jammed on two bridges,
		// 6 refineries).
		if (isEconomyUnit && !isDeployableRefineryOrMiner && !isUnitInProgress
			&& ownedEconomyUnits >= std::max(economyUnitCap, 2 * trueRefineries + 2))
			continue;

		// Dry economy (UpdateEconomyCrisis): our gatherers bring nothing in, another one would not either.
		if (isEconomyUnit && !isUnitInProgress && GetMacroState(pHouse).DryEconomyActive)
			continue;

		if (isAwaitedUnit)
			awaitedUnitStage = std::max(awaitedUnitStage, 3);

		TechnoTypeClass* pConvDeployType = GetConvertDeployTargetType(pUnit);
		const bool isConvertToOpenToppedOrCombat = (pConvDeployType != nullptr)
			&& ((pConvDeployType->OpenTopped && pConvDeployType->Passengers > 0)
				|| (pConvDeployType->GetWeapon(0) && pConvDeployType->GetWeapon(0)->WeaponType
					&& pConvDeployType->GetWeapon(0)->WeaponType->Damage > 0));
		if (isConvertToOpenToppedOrCombat && pConvDeployType->OpenTopped && pConvDeployType->Passengers > 0)
		{
			// A vehicle that deploys into a bunker soldiers fire from (a support vehicle for harassment
			// and holding ground, user): as many as our soldiers can fill, by the room of the deployed
			// form, never a fixed number.
			const int ownedConvTotal = pHouse->CountOwnedNow(pUnit)
				+ (abstract_cast<UnitTypeClass*>(pConvDeployType)
						? pHouse->CountOwnedNow(abstract_cast<UnitTypeClass*>(pConvDeployType))
						: 0);
			const int bunkerRoom = std::max(1, static_cast<int>(pConvDeployType->Passengers));

			if (ownedConvTotal >= pHouse->OwnedInfantry / bunkerRoom || canSafelySaveForMCV || savingForIslandTransport)
				continue;
		}

		const bool isUnarmedSupportVehicle = (!pUnit->Naval && pWpn == nullptr && !isMCV
			&& !isEconomyUnit && !isConvertToOpenToppedOrCombat && pUnit->Passengers == 0
			&& !(TechnoTypeExt::Fetch(pUnit) && TechnoTypeExt::Fetch(pUnit)->Carryall));

		// Detection need (Detection*): how far this vehicle finds what the house cannot see, and whether
		// the detectors of ours that reach as far or farther already fill the detectors' own cap.
		const int detectReach = pUnit->Naval ? 0 : DetectionReachCells(pUnit, detectionNeed);
		const bool detectorWanted = detectReach > 0 && detectorsReaching(detectReach) < detectionCap;

		if (isUnarmedSupportVehicle)
		{
			// A mobile sensor array has a cap of its own (one) while enemy stealth units were seen in the
			// last SensorLookoutSightingFrames or the house searched for remnants lately (user: it
			// finds the stealth units hiding in the area it covers).
			const bool sensorWanted = IsSensorLookoutType(pUnit)
				&& (Unsorted::CurrentFrame - GetMacroState(pHouse).RemnantSearchFrame < 2 * RemnantSearchEveryFrames
					|| std::any_of(GetMacroState(pHouse).StealthSightings.begin(), GetMacroState(pHouse).StealthSightings.end(),
						[](const auto& seen) { return Unsorted::CurrentFrame - seen.second.second <= SensorLookoutSightingFrames; }));

			// Hard cap unarmed support/detector/jammer vehicles (Mobile Radar Jammer
			// MRJ_SOV/MRJ_ALL, Mobile Stealth Generator SGEN/GGEN, Mobile Sensor
			// Array LPST/NLPST, Limpet Drone) at 1 across the house so they are never
			// spammed! A detector the detection need asks for has the detectors' cap instead
			// (detectorWanted): a jammer no longer keeps a farther reaching sensor array out.
			if ((!detectorWanted && (sensorWanted ? ownedSensorLookouts >= 1 : ownedUnarmedSupportVehicles >= 1)) || pHouse->OwnedUnits < 8
				|| canSafelySaveForMCV || savingForIslandTransport)
			{
				continue;
			}
		}

		// Mind control, and a weapon that turns the enemy against his own (Psychedelic warhead: the
		// Chaos Drone kills nothing, user: a few in a battalion help it beat another): support units,
		// a few per army.
		const bool isMindControlVehicle = pWpn != nullptr && pUnit->Spawns == nullptr && !pWpn->Spawner
			&& pWpn->Warhead != nullptr && (pWpn->Warhead->MindControl || pWpn->Warhead->Psychedelic);
		const bool isEmpSupport = !isMindControlVehicle && !isMainTank && pWpn != nullptr && pUnit->Spawns == nullptr && !pWpn->Spawner
			&& ((pWpn->Warhead != nullptr && pWpn->Warhead->EMEffect && pWpn->Damage <= 15) || pWpn->Damage <= 5);
		const int ownVehiclesAndShips = pHouse->OwnedUnits + pHouse->OwnedNavy;

		if (isMindControlVehicle)
		{
			// Mind-control vehicles from 3 vehicles of ours on (user: they should be common), one per
			// MindControlVehiclesPer of them, 2 at least.
			const int mindControlCap = std::max(2, ownVehiclesAndShips / MindControlVehiclesPer);

			if (CountOwnedUnitWithConversions(pHouse, pUnit) >= mindControlCap || ownVehiclesAndShips < 3
				|| canSafelySaveForMCV || savingForIslandTransport)
			{
				continue;
			}
		}
		else if (isEmpSupport)
		{
			// Non-damaging EMP and scrambler support vehicles and ships: 2 at most, from 6 of ours on.
			if (CountOwnedUnitWithConversions(pHouse, pUnit) >= 2 || ownVehiclesAndShips < 6
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

		if (isAwaitedUnit)
			awaitedUnitStage = std::max(awaitedUnitStage, 4);

		const int ownedUnitWithConv = CountOwnedUnitWithConversions(pHouse, pUnit);
		if (!isEconomyUnit && !isMCV && !isAmphibTransport)
		{
			// No ceiling by number, in all or of one type: a kind is built for as long as its units
			// are put to work, and waits while those it has go unused (IsArmyKindUnused).
			if (IsArmyKindUnused(pHouse, pUnit->Naval ? ArmyKind::Ships : ArmyKind::Vehicles) && !(isMainTank && belowTankFloor))
				continue;

			// The land is ours and the remnants are at sea (LandControl*): armed vehicles that cannot go on
			// water stop at LandControlVehicles, detectors the detection need asks for apart.
			const bool goesOnWater = pUnit->MovementZone == MovementZone::Amphibious
				|| pUnit->MovementZone == MovementZone::AmphibiousDestroyer
				|| pUnit->MovementZone == MovementZone::AmphibiousCrusher || pUnit->SpeedType == SpeedType::Hover;

			if (landControl && !pUnit->Naval && pWpn && !goesOnWater && !detectorWanted && !isUnitInProgress
				&& ownedGroundCombatUnits >= LandControlVehicles)
			{
				continue;
			}
		}

		// Cost and strength, and what its weapon does (VehicleFirepowerScore).
		const int unitFirepower = VehicleFirepowerScore(pUnit);
		int score = (pUnit->Cost / 4) + (pUnit->Strength / 3) + unitFirepower;
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

		// Counters of the seen enemy army and of what attacks the base lead their factory (combat
		// types only).
		const int diagCounter = CounterBonusFor(pHouse, pUnit);
		score += diagCounter;

		// Under attack the answer to the threat is built first. What the house cannot do without
		// stays: the MCV when there is no yard, a harvester per refinery, anything while no income is left.
		if (GetMacroState(pHouse).CounterUnderAttack && hasActiveUnitIncome
			&& ((isMCV && pHouse->NumConYards > 0)
				|| (isEconomyUnit && ownedEconomyUnits >= std::max(1, trueRefineries))))
		{
			score -= ThreatFirstPenalty;
		}

		if (pUnit->Naval)
		{
			if (isAmphibTransport)
			{
				// A transport the demand rules reject is not a candidate at all: otherwise the
				// queue keeps building it whenever nothing else scores higher (e.g. no buildable
				// warship), producing whole fleets of unneeded transports.
				const int ferryDelta = std::max({ scoreFerryTransport(pUnit), raidTransportBonus(pUnit), crossingNeedBonus(pUnit) });

				if (ferryDelta <= -900)
					continue;

				score += ferryDelta;

				// With the sea ours, a transport the ferry demand wants carries the war to the island.
				if (seaControlAcross && ferryDelta > 0)
					score += SeaControlTransportBonus;
			}
			else
			{
				int maxLandSiegeRange = 0;
				const bool isLongRangeCoastalSiegeShip = IsLongRangeSiegeShipType(pUnit, &maxLandSiegeRange);
				const bool isCoastalBombardShip = (maxLandSiegeRange >= (5 * 256));
				const bool sonarWanted = pUnit->Sensors && (detectionNeed & (DetectionNeedStealth | DetectionNeedSub));

				// The sea is ours (SeaControl*): warships that neither shell from afar nor hunt with a sonar the
				// detection need asks for stop at SeaControlWarships, escorts and the yard's guard.
				if (seaControl && !isLongRangeCoastalSiegeShip && !sonarWanted && !isUnitInProgress
					&& seaMacro.PlainWarships >= SeaControlWarships)
				{
					continue;
				}

				if (isLongRangeCoastalSiegeShip)
					seaMacro.SeaControlLongRangeShip = true;

				score += scores[29];

				// Only enemy ships or submarines are left: sonar (Sensors) to find them and weapons
				// that hit under water (NavalTargeting) to sink them.
				// An enemy submarine near our base: the same, with less weight.
				const bool remnantHunt = GetMacroState(pHouse).NavalRemnantUntil > Unsorted::CurrentFrame;

				if (remnantHunt || GetMacroState(pHouse).SubThreatUntil > Unsorted::CurrentFrame)
				{
					const auto targeting = pUnit->NavalTargeting;
					const bool hitsUnderwater = targeting == NavalTargetingType::Underwater_Secondary
						|| targeting == NavalTargetingType::Underwater_Only || targeting == NavalTargetingType::Organic_Secondary;

					score += remnantHunt ? ((hitsUnderwater ? 900 : -600) + (pUnit->Sensors ? 400 : 0))
										 : ((hitsUnderwater ? 500 : 0) + (pUnit->Sensors ? 250 : 0));
				}

				if (features[21] > 0 || features[67] > 10)
				{
					if (isLongRangeCoastalSiegeShip)
					{
						// Continuously produce long-range capital siege warships
						// (Dreadnought, Aircraft Carrier, Boomer, Nod Missile Sub) to
						// saturate the enemy coastline from afar and end island games
						// rapidly!
						score += 850 + std::min(500, maxLandSiegeRange / 6) + (seaControl ? SeaControlLongRangeBonus : 0);
					}
					else if (isCoastalBombardShip && !seaControl)
					{
						score += 520 + std::min(300, maxLandSiegeRange / 10);
					}
				}
			}

			// Detection need: a sonar or sensor ship scored by its reach while stealth or submerged enemies
			// hide (no cap: the land detectors are another matter).
			if ((detectionNeed & (DetectionNeedStealth | DetectionNeedSub)) && pUnit->Sensors)
				score += std::min(DetectionScoreMost, DetectionScorePerCell * DetectionReachCells(pUnit, detectionNeed & (DetectionNeedStealth | DetectionNeedSub)));
			else if (features[72] > 0 && pUnit->Sensors)
				score += 400;

			if (score > bestNavalScore)
			{
				bestNavalScore = score;
				bestNavalIdx = pUnit->ArrayIndex;
			}
		}
		else
		{
			const bool isAA = !isMainTank && pWpn && pWpn->Projectile && pWpn->Projectile->AA;
			const bool isArtillery = !isMainTank && pWpn && pWpn->Range >= ArtilleryRangeCells * Unsorted::LeptonsPerCell;
			// A transport carries two or more, has no weapon or is a Carryall: an armed vehicle with one seat
			// (the IFV gunner, the Venom and Avatar passenger hacks) scores by its combat role (user: the
			// transport class, the heaviest of the war factory, went mostly to VENOM, FV and HTK).
			const bool isTransportClass = (pUnit->Passengers > 0 && !isMainTank && (pUnit->Passengers >= 2 || pWpn == nullptr))
				|| (TechnoTypeExt::Fetch(pUnit) && TechnoTypeExt::Fetch(pUnit)->Carryall);
			const int diagPreClass = score;

			// Artillery past what our front line can cover loses ground for every gun over it, with no
			// cap: the main battle tanks come next (ArtilleryFreeVehicles).
			if (isArtillery && !isEconomyUnit && !isMCV && pUnit->Passengers == 0
				&& ownedArtilleryVehicles >= artilleryAllowance)
			{
				score -= (ownedArtilleryVehicles - artilleryAllowance + 1) * ArtilleryOverFrontPenalty
					* (tankMass ? TankMassArtilleryPenaltyPercent : 100) / 100;
			}

			if (isMCV)
			{
				score += (pHouse->NumConYards == 0)
					? 12000
					: (scores[30] + (canSafelySaveForExpansionMCV ? 1800 : 0)
						+ ((wantsArchipelagoExpansion && hasIslandMcvLift) ? 900 : 0) + (fieldsRunningDry ? 2500 : 0));

				if (bankWantsYard)
					bankYardMcvIdx = pUnit->ArrayIndex;
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
			else if (isTransportClass)
			{
				const bool isAirOrCarryallVehicle
					= isAmphibTransport || (TechnoTypeExt::Fetch(pUnit) && TechnoTypeExt::Fetch(pUnit)->Carryall);
				if (!isAirOrCarryallVehicle)
				{
					score += scores[28] + (features[21] > 0 ? -500 : 0);
				}
				else
				{
					const int ferryDelta = std::max({ scoreFerryTransport(pUnit), raidTransportBonus(pUnit), crossingNeedBonus(pUnit) });

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

				// The escort of the tank mass comes before any aircraft shows up.
				if (ownedAntiAirVehicles < wantedAntiAirEscorts)
					score += TankMassEscortBonus;

				// As many as the air threat our anti-air leaves uncovered asks for, within the room of the front line.
				const int missingAntiAir = std::min(AntiAirUnitsMissing(pHouse, pUnit), antiAirVehicleRoom);

				if (missingAntiAir > 0)
					score += std::min(AntiAirUnitMissingMost, missingAntiAir * AntiAirUnitMissingBonus);
			}
			else if (isArtillery)
			{
				score += scores[27];
			}
			// The rest: a main battle tank, or a light vehicle. Rules with no Category on their units
			// fall back on the health.
			else if (!isMainTank && (pUnit->Category != Category::Invalid || pUnit->Strength < 300))
			{
				score += scores[25];

				// The other front line vehicles of the mass keep a larger share of the type penalty.
				if (tankMass)
					score += std::min(1200, ownedUnitWithConv * 55) * (100 - TankMassOtherTypePenaltyPercent) / 100;
			}
			else
			{
				score += scores[24];

				// A mass is many tanks of one type: the tank mass keeps only a share of the type penalty.
				if (tankMass)
					score += std::min(1200, ownedUnitWithConv * 55) * (100 - TankMassTypePenaltyPercent) / 100;
			}

			const int diagPostClass = score;

			if (features[77] > 0 && pUnit->ImmuneToPsionics && !isEconomyUnit && !isMCV)
				score += 420; // Counter enemy MindControl with ImmuneToPsionics

			// vehicles (Drones, Robot Tanks, Automated units)!
			// Detection need (Detection*): a detector scored by how far it finds what the house cannot
			// see (it was a flat 480 for Sensors or DetectDisguise while a stealth unit was in sight).
			if (detectorWanted && !isEconomyUnit)
				score += std::min(DetectionScoreMost, DetectionScorePerCell * detectReach);

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
			if (features[21] > 0 && !isEconomyUnit && !isMCV && !isAmphibTransport
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

				// With the sea ours, the armed vehicles that cross it carry the war to the island.
				if (seaControlAcross && canSelfCrossWater && pWpn)
					score += SeaControlCrossingBonus;
			}

			// Expert 0 after a raid: harvesters back to their peak first.
			if ((pUnit->Harvester || pUnit->ResourceGatherer) && !isDeployableRefineryOrMiner
				&& pExt->NeuralAI_ActiveExpert == 0 && GetMacroState(pHouse).EconomyRecoveryActive
				&& ownedEconomyUnits < GetMacroState(pHouse).PeakHarvesters)
			{
				score += 1500;
			}

			// A Rotating Raids recruiting the bigger group of a lesson (RaidRecruitWanted) asks for fast
			// armed vehicles: the raid takes the fastest idle units first.
			if (GetMacroState(pHouse).RaidRecruitWanted > 0 && pWpn && !pUnit->Naval && !isEconomyUnit && !isMCV && !isTransportClass
				&& !isAA)
			{
				score += std::min(RaidRecruitMostBonus, std::max(0, pUnit->Speed) * RaidRecruitSpeedBonus);
			}

			if (logUnitPicks)
			{
				const char diagClass = isMCV ? 'M' : ((pUnit->Harvester || pUnit->ResourceGatherer) ? 'H'
					: (isTransportClass ? 'P'
						: ((isUnarmedSupportVehicle || IsRepairerUnitType(pUnit)) ? 'S'
							: (isAA ? 'A' : (isArtillery ? 'G' : (isMainTank ? 'T' : 'L'))))));
				const int diagBase = pUnit->Cost / 4 + pUnit->Strength / 3 + unitFirepower;
				const int diagPenalty = std::min(1200, ownedUnitWithConv * 55);
				unitPickDiags.push_back({ pUnit, diagClass, diagBase, diagPenalty, diagCounter, diagPostClass - diagPreClass,
					score - diagPostClass + (diagPreClass - diagBase + diagPenalty - diagCounter), score });
			}

			if (isAwaitedUnit)
			{
				awaitedUnitStage = 5;
				awaitedUnitScore = std::max(awaitedUnitScore, score);
			}

			if (pUnit->Harvester && !pUnit->DeploysInto && score > awaitedHarvesterScore)
			{
				awaitedHarvesterScore = score;
				awaitedHarvesterIdx = pUnit->ArrayIndex;
			}

			if (isMCV && score > awaitedMcvScore)
			{
				awaitedMcvScore = score;
				awaitedMcvIdx = pUnit->ArrayIndex;
			}

			if (score > bestUnitScore)
			{
				bestUnitScore = score;

				bestUnitIdx = pUnit->ArrayIndex;
				bestUnitRestoresIncome = isEconomyUnit || isMCV || isIncomeRecoveryUnit;
			}

			if (isMainTank && score > bestTankScore)
			{
				bestTankScore = score;
				bestTankIdx = pUnit->ArrayIndex;
			}

			// An armed light vehicle (the pick line's class L): no main battle tank, artillery, anti-air,
			// transport, economy unit, deployer or repairer (IsRepairerUnitType).
			if (pWpn && !IsRepairerUnitType(pUnit) && !isMainTank && !isAA && !isArtillery && !isTransportClass && !isEconomyUnit && !isMCV
				&& !pUnit->Harvester && !pUnit->ResourceGatherer && score > bestLightScore)
			{
				bestLightScore = score;
				bestLightIdx = pUnit->ArrayIndex;
			}

			// A repairer the cap let through (Repairer*).
			if (IsRepairerUnitType(pUnit) && score > bestRepairerScore)
			{
				bestRepairerScore = score;
				bestRepairerIdx = pUnit->ArrayIndex;
			}

			if (tankBunkersToFill > 0 && pUnit->Bunkerable && !isEconomyUnit && !isMCV
				&& score > bestBunkerTankScore)
			{
				bestBunkerTankScore = score;
				bestBunkerTankIdx = pUnit->ArrayIndex;
			}
		}
	}

	// TEMP diagnostic (economy hold): the scored unit pick, before the floors and complements take it.
	const int scoredUnitIdx = bestUnitIdx;
	const int scoredUnitScore = bestUnitScore;

	// TEMP diagnostic: the best five candidates, and the best main battle tank when it is not one of
	// them. Class: T tank, L light, G artillery, A anti-air, P transport, S support, H harvester, D
	// deployer, M MCV. Rest: what is neither base, type penalty, counters nor class (in progress,
	// threat first, income, sensors, island logistics and the like).
	if (logUnitPicks && !unitPickDiags.empty())
	{
		++unitPickDiagCount;
		std::stable_sort(unitPickDiags.begin(), unitPickDiags.end(), [](const UnitPickDiag& a, const UnitPickDiag& b) { return a.Total > b.Total; });
		std::string text;
		bool tankShown = false;

		for (size_t i = 0; i < unitPickDiags.size(); ++i)
		{
			auto const& d = unitPickDiags[i];

			if (i >= 5 && (tankShown || d.Class != 'T'))
				continue;

			tankShown = tankShown || d.Class == 'T';
			char part[160];
			std::snprintf(part, sizeof(part), " %s%s %c %d=b%d-p%d+c%d+k%d+r%d;", i >= 5 ? "... " : "", d.Type->ID, d.Class, d.Total, d.Base,
				d.TypePenalty, d.Counter, d.ClassPart, d.Rest);
			text += part;

			if (i >= 5)
				break;
		}

		Debug::Log("[NeuralAI] Frame %d | House #%d unit pick DIAG arch %d slots tank %d light %d aa %d guns %d transport %d:%s\n",
			Unsorted::CurrentFrame, pHouse->ArrayIndex, pExt->NeuralAI_Archetype, static_cast<int>(scores[24]), static_cast<int>(scores[25]),
			static_cast<int>(scores[26]), static_cast<int>(scores[27]), static_cast<int>(scores[28]), text.c_str());
	}

	// The tank floor: below it the best main battle tank goes ahead of the pick (TankFloor*).
	{
		const bool belowFloor = belowTankFloor;
		// Only an attack from the air alone keeps the scored pick: against a ground attack the tank goes first.
		const bool airAttackOnly = GetMacroState(pHouse).CounterUnderAttack && !GetMacroState(pHouse).CounterUnderGroundAttack;
		const bool floorTakes = belowFloor && bestTankIdx >= 0 && bestUnitIdx != bestTankIdx && hasActiveUnitIncome
			&& !bestUnitRestoresIncome && !airAttackOnly;
		const char* floorWhy = !belowFloor ? "" : (floorTakes ? ", the tank goes first"
			: (bestTankIdx < 0 ? ", kept: no tank to build" : (bestUnitIdx == bestTankIdx ? ", the pick is a tank"
				: (!hasActiveUnitIncome ? ", kept: no income" : (bestUnitRestoresIncome ? ", kept: the pick restores the income" : ", kept: under air attack")))));

		// TEMP diagnostic: the floor, the tanks alive and what the floor did, with the vehicle pick line.
		if (logUnitPicks)
		{
			++unitPickDiagCount;
			Debug::Log("[NeuralAI] Frame %d | House #%d tank floor %d, main battle tanks %d of %d armed ground vehicles, best tank %s %d, "
				"pick %s %d%s\n", Unsorted::CurrentFrame, pHouse->ArrayIndex, tankFloor, ownedMainTanks, ownedGroundCombatUnits,
				bestTankIdx >= 0 ? UnitTypeClass::Array.GetItem(bestTankIdx)->ID : "none", bestTankScore,
				bestUnitIdx >= 0 ? UnitTypeClass::Array.GetItem(bestUnitIdx)->ID : "none", bestUnitScore, floorWhy);

			// TEMP diagnostic (tank census, NoteMainTankDeath): the main battle tanks built and lost so far.
			auto const& census = GetMacroState(pHouse);
			auto const& lost = census.TankCensusLost;
			auto const& by = census.TankCensusKillers;
			int lostTotal = 0;

			for (const int n : lost)
				lostTotal += n;

			Debug::Log("[NeuralAI] Frame %d | House #%d tank census: built %d, lost %d (guard %d, intercept %d, plan %d, wave %d, scout %d, "
				"repair %d, home %d, away %d), by vehicle %d infantry %d defense %d aircraft %d other %d, %d near our sites, "
				"lived %d frames on average (%d known)\n", Unsorted::CurrentFrame, pHouse->ArrayIndex, census.TankCensusBuilt, lostTotal,
				lost[0], lost[1], lost[2], lost[3], lost[4], lost[5], lost[6], lost[7], by[0], by[1], by[2], by[3], by[4],
				census.TankCensusLostAtHome, census.TankCensusLifeCount > 0 ? census.TankCensusLifeFrames / census.TankCensusLifeCount : -1,
				census.TankCensusLifeCount);
		}

		if (floorTakes)
		{
			bestUnitIdx = bestTankIdx;
			bestUnitScore = bestTankScore;
		}

		// The light complement (LightComplement*): one armed light vehicle for each LightComplementPerTanks
		// main battle tanks; below it the best light vehicle goes first, below the tank floor as well when
		// the newest of our main battle tanks and light vehicles is a tank (the two alternate).
		const int lightFloor = ownedMainTanks >= LightComplementMinTanks ? ownedMainTanks / LightComplementPerTanks : 0;
		const bool belowLight = ownedLightVehicles < lightFloor;
		const bool newestIsTank = newestTankOrLightUid != 0 && !newestTankOrLightIsLight && !newestTankOrLightIsRepairer;
		const bool lightTurn = !belowFloor || newestIsTank;
		const bool lightTakes = belowLight && lightTurn && bestLightIdx >= 0 && bestUnitIdx != bestLightIdx && hasActiveUnitIncome
			&& !bestUnitRestoresIncome && !airAttackOnly;

		// TEMP diagnostic: the light complement, with the vehicle pick line.
		if (logUnitPicks && belowLight)
		{
			++unitPickDiagCount;
			Debug::Log("[NeuralAI] Frame %d | House #%d light complement %d, light vehicles %d, main battle tanks %d, best light %s %d, pick %s %d%s\n",
				Unsorted::CurrentFrame, pHouse->ArrayIndex, lightFloor, ownedLightVehicles, ownedMainTanks,
				bestLightIdx >= 0 ? UnitTypeClass::Array.GetItem(bestLightIdx)->ID : "none", bestLightScore,
				bestUnitIdx >= 0 ? UnitTypeClass::Array.GetItem(bestUnitIdx)->ID : "none", bestUnitScore,
				lightTakes ? (belowFloor ? ", the light vehicle goes first, its turn below the tank floor" : ", the light vehicle goes first")
					: (!lightTurn ? ", kept: below the tank floor, the newest was a light vehicle"
					: (bestLightIdx < 0 ? ", kept: no light vehicle to build" : (bestUnitIdx == bestLightIdx ? ", the pick is a light vehicle"
						: (!hasActiveUnitIncome ? ", kept: no income" : (bestUnitRestoresIncome ? ", kept: the pick restores the income"
							: ", kept: under air attack"))))));
		}

		if (lightTakes)
		{
			bestUnitIdx = bestLightIdx;
			bestUnitScore = bestLightScore;
		}

		// The repairer complement (user, 2026-11-19: approved «Sí, hazlo»; RepairerPerVehicles): with that
		// many armed ground vehicles of ours or more and fewer than one repairer for each of them, the best
		// repairer goes first, unless the light complement took this pick; below the tank floor only when the
		// newest of our main battle tanks, light vehicles and repairers is a tank, and never twice in a row;
		// the tank floor's exceptions. As support a repairer gets no class weight and lost every pick of 10
		// matches of 2026-11-19 (13 times a candidate, MDISK 457 to 1252).
		const int armedNonRepairers = ownedGroundCombatUnits - ownedRepairerVehicles;
		const bool belowRepairers = armedNonRepairers >= RepairerPerVehicles
			&& (ownedRepairerVehicles + 1) * RepairerPerVehicles <= armedNonRepairers;
		const bool repairerTurn = belowFloor ? newestIsTank : !newestTankOrLightIsRepairer;
		const bool repairerTakes = belowRepairers && !lightTakes && repairerTurn && bestRepairerIdx >= 0 && bestUnitIdx != bestRepairerIdx
			&& hasActiveUnitIncome && !bestUnitRestoresIncome && !airAttackOnly;

		// TEMP diagnostic: the repairer complement, with the vehicle pick line.
		if (logUnitPicks && belowRepairers)
		{
			++unitPickDiagCount;
			Debug::Log("[NeuralAI] Frame %d | House #%d repairer complement: repairers %d, armed vehicles %d, best repairer %s %d, pick %s %d%s\n",
				Unsorted::CurrentFrame, pHouse->ArrayIndex, ownedRepairerVehicles, armedNonRepairers,
				bestRepairerIdx >= 0 ? UnitTypeClass::Array.GetItem(bestRepairerIdx)->ID : "none", bestRepairerScore,
				bestUnitIdx >= 0 ? UnitTypeClass::Array.GetItem(bestUnitIdx)->ID : "none", bestUnitScore,
				repairerTakes ? (belowFloor ? ", the repairer goes first, its turn below the tank floor" : ", the repairer goes first")
					: (lightTakes ? ", kept: the light vehicle's turn" : (!repairerTurn ? ", kept: not its turn"
					: (bestRepairerIdx < 0 ? ", kept: no repairer to build" : (bestUnitIdx == bestRepairerIdx ? ", the pick is a repairer"
						: (!hasActiveUnitIncome ? ", kept: no income" : (bestUnitRestoresIncome ? ", kept: the pick restores the income"
							: ", kept: under air attack")))))));
		}

		if (repairerTakes)
		{
			bestUnitIdx = bestRepairerIdx;
			bestUnitScore = bestRepairerScore;
		}
	}

	// A tank for an empty tank bunker goes ahead of the scored pick while the house has income and
	// the money for it; under attack the answer to the threat goes first.
	if (bestBunkerTankIdx >= 0 && bestUnitIdx != bestBunkerTankIdx && hasActiveUnitIncome
		&& !GetMacroState(pHouse).CounterUnderAttack
		&& money >= UnitTypeClass::Array.GetItem(bestBunkerTankIdx)->GetActualCost(pHouse) + GarrisonTrainingReserve)
	{
		bestUnitIdx = bestBunkerTankIdx;
		CountDecision(NeuralDecision::GarrisonTraining, pHouse->ArrayIndex);
	}

	// The governor's MCV starts in every other 900-frame window (the odd ones: the building queue
	// takes the governor's factories in the even ones).
	if (bankYardMcvIdx >= 0 && bestUnitIdx != bankYardMcvIdx && ((Unsorted::CurrentFrame / 900) % 2) != 0
		&& hasActiveUnitIncome)
	{
		bestUnitIdx = bankYardMcvIdx;

		if (IsAITickWindow(450, evalDelay))
			CountDecision(NeuralDecision::BankYardPick, pHouse->ArrayIndex);
	}

	// A deployer the building pick asked for (DeployerBuilds, NeuralAI.MobileFactory.inl) goes ahead of the
	// scored pick, its floors and complements: the building pick weighed it as the building it becomes.
	// Under attack only a defense keeps that place, and anything for a house without a yard; an MCV that
	// such a house can build comes first. One the factory has not started in DeployerBuildWaitFrames is
	// given up, and so is one no factory of ours can build any more.
	{
		auto& builds = GetMacroState(pHouse).DeployerBuilds;
		const int now = Unsorted::CurrentFrame;

		std::erase_if(builds, [&](const NeuralDeployerBuild& build)
		{
			if (build.UnitType == inProgressUnitIdx)
				return false;

			return now < build.Frame || now - build.Frame > DeployerBuildWaitFrames
				|| !CanBuildUnderHumanRules(pHouse, UnitTypeClass::Array.GetItem(build.UnitType));
		});

		auto const pScored = bestUnitIdx >= 0 ? UnitTypeClass::Array.GetItem(bestUnitIdx) : nullptr;
		const bool yardMcvFirst = pHouse->NumConYards == 0 && pScored && pScored->DeploysInto && pScored->DeploysInto->ConstructionYard;

		if (!builds.empty() && !yardMcvFirst)
		{
			auto const& build = builds.front();
			auto const pBuilding = BuildingTypeClass::Array.GetItem(build.BuildingType);

			if (!GetMacroState(pHouse).CounterUnderAttack || pBuilding->IsBaseDefense || pHouse->NumConYards == 0)
				bestUnitIdx = build.UnitType;
		}
	}

	// The harvester the economy hold saves for (fewer than refineries) or, with no harvester nor
	// refinery wanted, its expansion MCV is the pick of the war factory as soon as it is wanted, paid
	// as the credits come in (user: AI builds pay as they go). With none past the rules of this pick
	// (the tank mass's gatherer wait, the gatherer cap, the MCV gates) the hold does not save for it,
	// except the island MCV, whose missing buildings come first. Nothing under attack. (User approved,
	// 2026-11-20, P1; over 39 matches, once the credits reached the price a harvester more came within
	// 1800 frames in 2 of 17 checks and an MCV in 5 of 22: the harvester stopped at the economy unit
	// rules in 53 of 68 hold status lines, the MCV at its gates in 47 of 89, match #11911: an MCV saved
	// for while the pick built a REDEEMER, cancelled 528 times.)
	{
		auto& saveMacro = GetMacroState(pHouse);

		if (saveMacro.HarvesterSaveTarget > 0 && awaitedHarvesterIdx < 0)
			saveMacro.HarvesterSaveTarget = 0;

		if (saveMacro.ExpansionSaveTarget > 0 && awaitedMcvIdx < 0 && !islandOreRunningOut)
			saveMacro.ExpansionSaveTarget = 0;

		const int savedForIdx = saveMacro.HarvesterSaveTarget > 0 ? awaitedHarvesterIdx
			: ((saveMacro.RefinerySaveTarget <= 0 && saveMacro.ExpansionSaveTarget > 0) ? awaitedMcvIdx : -1);

		if (savedForIdx >= 0 && !saveMacro.CounterUnderAttack)
			bestUnitIdx = savedForIdx;
	}

	// TEMP diagnostic (economy hold, 2026-11-20, matches #11905 and #11911: a war factory pick cancelled
	// hundreds of times while the awaited item never started): every 900 frames of a hold, what it saves
	// for, its price, the credits, how long it lasts, what it cancelled since the last line, what stands
	// in the factories, the picks, how far the awaited item gets in them, and the gates of its kind.
	// UpdateEconomyCrisis writes the line of its end.
	if (holdMacro.EconomyHoldKind != 0 && holdMacro.EconomyHoldLogLines < EconomyHoldLogLinesMost && IsAITickWindow(900, 0))
	{
		holdMacro.EconomyHoldLogLines++;
		const int kind = holdMacro.EconomyHoldKind;
		const int price = kind == 1 ? holdMacro.HarvesterSaveTarget
			: (kind == 2 ? holdMacro.RefinerySaveTarget : (kind == 3 ? holdMacro.ExpansionSaveTarget : 0));
		auto const unitName = [](int idx) { return idx >= 0 && idx < UnitTypeClass::Array.Count ? UnitTypeClass::Array.GetItem(idx)->ID : "none"; };
		auto const buildingName = [](int idx) { return idx >= 0 && idx < BuildingTypeClass::Array.Count ? BuildingTypeClass::Array.GetItem(idx)->ID : "none"; };
		std::string factories;

		for (auto const pFactory : FactoryClass::Array)
		{
			if (!pFactory || pFactory->Owner != pHouse || !pFactory->Object)
				continue;

			auto const pType = pFactory->Object->GetTechnoType();
			char part[96];
			std::snprintf(part, sizeof(part), " %s %d/54%s", pType ? pType->ID : "?", pFactory->GetProgress(), pFactory->IsSuspended ? " held" : "");
			factories += part;
		}

		char gates[256];

		if (kind == 3)
		{
			std::snprintf(gates, sizeof(gates), "MCV gates: undeployed %d, bank yard %d, yards %d of %d, buildings %d, army %d, free yard fields %d, "
				"slot30 %d, slot81 %d, features20 %d, fields dry %d, island %d (target %d, cross %d, waits as yard %d), gatherers %d",
				ownsUndeployedMCV ? 1 : 0, bankWantsYard ? 1 : 0, pHouse->NumConYards, maxAllowedConYards, pHouse->Buildings.Count,
				ownedGroundCombatUnits + ownedNavalOrAmphibCombatUnits, freeOreFieldsForYards, static_cast<int>(scores[30]),
				static_cast<int>(scores[81]), static_cast<int>(features[20]), fieldsRunningDry ? 1 : 0, islandOreRunningOut ? 1 : 0,
				hasUncolonizedIslandTarget ? 1 : 0, needsCrossIslandMCV ? 1 : 0, islandMcvWaitsAsYard ? 1 : 0, ownedEconomyUnits + ownedCashProduces);
		}
		else if (kind == 2)
		{
			std::snprintf(gates, sizeof(gates), "refinery gates: best refinery score %d, scored building score %d, refineries %d",
				awaitedRefineryScore, scoredBuildingScore, trueRefineries);
		}
		else
		{
			std::snprintf(gates, sizeof(gates), "harvester gates: gatherers %d, cap %d, refineries %d, tank mass %d (front line %d of %d), dry %d, income %d",
				ownedEconomyUnits, economyUnitCap, trueRefineries, tankMass ? 1 : 0, ownedFrontLineVehicles, TankMassBaseWave,
				holdMacro.DryEconomyActive ? 1 : 0, hasActiveUnitIncome ? 1 : 0);
		}

		Debug::Log("[NeuralAI] House %d economy hold DIAG: %s, price %d, credits %d, held %d frames, %d cancels since the last line, "
			"%d on hold, in production %d, factories:%s; picks: building %s, unit %s (scored %s %d), awaited unit stage %d score %d; %s, frame %d.\n",
			pHouse->ArrayIndex, EconomyHoldKindName(kind), price, money, Unsorted::CurrentFrame - holdMacro.EconomyHoldStartFrame,
			holdMacro.EconomyHoldLineCancels, static_cast<int>(holdMacro.BrokeHeldObjects.size()), EconomyHoldItemInProduction(pHouse, kind) ? 1 : 0,
			factories.empty() ? " none" : factories.c_str(), buildingName(pExt->NeuralAI_NextBuildingTarget), unitName(bestUnitIdx),
			unitName(scoredUnitIdx), scoredUnitScore, awaitedUnitStage, awaitedUnitScore, gates, Unsorted::CurrentFrame);
		holdMacro.EconomyHoldLineCancels = 0;
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

		if (IsCaptureInfantryType(pOwnInf->Type))
			ownedEngineers++;
		else if (IsSpyInfantryType(pOwnInf->Type))
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
	// The best soldier for a seat in a building worth holding, and the seats left for such soldiers
	// under any expert and on any map (the squad for the best positions, or the city).
	int bestGarrisonIdx = -1;
	int bestGarrisonScore = -32000;
	int garrisonSeatsToFill = 0;
	{
		bool unusedNeedClearer = false;
		GetUrbanProductionNeeds(pHouse, garrisonSeatsToFill, unusedNeedClearer);
	}
	int inProgressInfIdx = -1;

	// Expert 9 in charge, or an urban map: fill the buildings worth holding (soldiers with the
	// strongest weapon inside first) and bring a clearer when an enemy garrison near us has nobody
	// to answer it. Without Expert 9 the garrison bonus is halved.
	int urbanGarrisonRoom = 0;
	bool urbanNeedClearer = false;
	const bool urbanExpertInCharge = pExt->NeuralAI_ActiveExpert == 9;

	// Strategic positions from the start (user: holding them can decide a match, the vanilla AI took
	// them steadily too): buildings worth holding (urban opportunity >= 150) count under any expert.
	const bool urbanOpportunityNow = UrbanOpportunity(pHouse) >= 150;

	if (urbanExpertInCharge || IsUrbanMapFor(pHouse) || urbanOpportunityNow)
		GetUrbanProductionNeeds(pHouse, urbanGarrisonRoom, urbanNeedClearer);

	// Soldiers inside buildings by type (type index, count): they hold their seats, so they count
	// neither for the type mix of the squads nor for the infantry cap below.
	std::vector<std::pair<int, int>> garrisonedByType;
	int garrisonedInfantry = 0;

	for (auto const pOwnBld : pHouse->Buildings)
	{
		if (!pOwnBld || pOwnBld->InLimbo || !pOwnBld->IsAlive || !pOwnBld->Type || !pOwnBld->Type->CanBeOccupied)
			continue;

		for (auto const pOccupant : pOwnBld->Occupants)
		{
			if (!pOccupant || !pOccupant->Type)
				continue;

			const int typeIndex = pOccupant->Type->ArrayIndex;
			auto const it = std::find_if(garrisonedByType.begin(), garrisonedByType.end(),
				[typeIndex](const std::pair<int, int>& entry) { return entry.first == typeIndex; });

			if (it != garrisonedByType.end())
				it->second++;
			else
				garrisonedByType.emplace_back(typeIndex, 1);

			garrisonedInfantry++;
		}
	}

	// Soldiers that shoot up in proportion to the uncovered air threat: at most AntiAirUnitsMostPercent
	// of our infantry (AntiAirUnitsFloor at least).
	int ownedInfantryCount = 0;
	int ownedAntiAirInfantry = 0;

	for (auto const pOwnInf : InfantryClass::Array)
	{
		if (!pOwnInf || pOwnInf->Owner != pHouse || pOwnInf->InLimbo || !pOwnInf->IsAlive || !pOwnInf->Type)
			continue;

		ownedInfantryCount++;

		auto const pOwnWS = pOwnInf->Type->GetWeapon(0);
		auto const pOwnW = pOwnWS ? pOwnWS->WeaponType : nullptr;

		if (pOwnW && pOwnW->Projectile && pOwnW->Projectile->AA)
			ownedAntiAirInfantry++;
	}

	const int antiAirInfantryRoom = std::max(0, std::max(AntiAirUnitsFloor, ownedInfantryCount * AntiAirUnitsMostPercent / 100) - ownedAntiAirInfantry);

	if (FactoryClass* pInfFac = pHouse->GetPrimaryFactory(AbstractType::InfantryType, false, BuildCat::DontCare))
	{
		if (pInfFac->Object && pInfFac->Object->GetType())
			inProgressInfIdx = pInfFac->Object->GetType()->GetArrayIndex();
	}

	// Healers (medics, field mechanics) are support: one for five fighting soldiers, two for a squad
	// of ten (a wave takes two at most, two escort one soldier group). The score below grows with the
	// cost, so an expensive medic used to win every pick (match #6368, Capital Coup: 39 medics and 3
	// riflemen, no soldier left to garrison the city).
	int ownedHealerInfantry = 0;
	int ownedFighterInfantry = 0;

	for (auto const pOwnI : InfantryClass::Array)
	{
		if (!pOwnI || pOwnI->Owner != pHouse || !pOwnI->IsAlive || !pOwnI->Type)
			continue;

		if (IsHealerInfantryType(pOwnI->Type))
			ownedHealerInfantry++;
		else if (!IsSpecialistInfantryType(pOwnI->Type) && pOwnI->Type->GetWeapon(0) && pOwnI->Type->GetWeapon(0)->WeaponType)
			ownedFighterInfantry++;
	}

	for (auto const pInf : InfantryTypeClass::Array)
	{
		if (!pInf || !CanBuildUnderHumanRules(pHouse, pInf))
			continue;

		const bool isInfInProgress = (pInf->ArrayIndex == inProgressInfIdx);

		// A pick that never came out of its factory (WatchUndeliveredPick).
		if (!isInfInProgress && IsProductionBanned(pHouse, 2, pInf->ArrayIndex))
			continue;

		if (!isInfInProgress && !IsUnitTierAllowed(pHouse, pInf))
			continue;

		if (!isInfInProgress && IsHealerInfantryType(pInf) && ownedHealerInfantry * 5 >= std::max(1, ownedFighterInfantry))
			continue;

		const bool isIncomeRecoveryEng = (IsCaptureInfantryType(pInf) && neutralCashDerrickCount > ownedEngineers);
		if (!hasActiveUnitIncome && !isInfInProgress && !isIncomeRecoveryEng && pInf->GetActualCost(pHouse) > money)
			continue;

		int score = (pInf->Cost / 2) + pInf->Strength + (isInfInProgress ? 450 : 0);

		// Penalize stacking one infantry type so squads stay mixed (soldiers in garrisons aside)
		auto const itGarrisoned = std::find_if(garrisonedByType.begin(), garrisonedByType.end(),
			[pInf](const std::pair<int, int>& entry) { return entry.first == pInf->ArrayIndex; });
		const int garrisonedOfType = itGarrisoned != garrisonedByType.end() ? itGarrisoned->second : 0;
		score -= std::min(600, std::max(0, pHouse->CountOwnedNow(pInf) - garrisonedOfType) * 20);

		// Seats waiting in our seated fighting vehicles (UpdatePassengerFill) ask for fit soldiers.
		const int passengerSeatsWanted = GetMacroState(pHouse).PassengerSeatsWanted;

		if (passengerSeatsWanted > 0 && IsFitPassengerSoldierType(pInf))
			score += std::min(PassengerSeatInfantryMost, passengerSeatsWanted * PassengerSeatInfantryBonus);

		const auto pWpnStruct = pInf->GetWeapon(0);
		const auto pWpn = pWpnStruct ? pWpnStruct->WeaponType : nullptr;
		const bool isAA = pWpn && pWpn->Projectile && pWpn->Projectile->AA;

		if (IsCaptureInfantryType(pInf))
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
		else if (IsSpyInfantryType(pInf) || pInf->VehicleThief)
		{
			const int maxActiveSpies
				= (scores[19] > 180 || pExt->NeuralAI_Archetype == 1 || pExt->NeuralAI_ActiveExpert == 3) ? 3 : 2;
			if (ownedInfiltrators >= maxActiveSpies)
				continue;

			// A luxury: never saved for (a 1500 credit spy stayed the next soldier for 25 minutes
			// while its house ran out of infantry), and its price is no measure of its worth.
			if (!isInfInProgress && pInf->GetActualCost(pHouse) * SpyLuxuryMoneyFactor > money)
				continue;

			score += ((pHouse->OwnedInfantry < 6) ? -400 : scores[19]) - (pInf->Cost / 2);
		}
		else if (isAA || pInf->C4)
		{
			score += scores[20];

			// Soldiers that shoot up, as many as the uncovered air threat asks for within their room.
			if (isAA)
				score += std::min(AntiAirUnitMissingMost, std::min(AntiAirUnitsMissing(pHouse, pInf), antiAirInfantryRoom) * AntiAirUnitMissingBonus);
		}
		else if (pInf->Cost >= 250)
		{
			score += scores[17];
		}
		else
		{
			score += scores[16];
		}

		if (needsCheapInfForBioReactor && !IsCaptureInfantryType(pInf) && !IsSpyInfantryType(pInf) && !pInf->Doggie && pInf->Cost <= 220)
		{
			score += 520;
		}

		if (urbanGarrisonRoom > 0 && IsGarrisonOccupierType(pInf))
		{
			// A city to fill (more than a squad of seats left): the bonus grows with the seats, and a
			// cheap soldier holds a seat as well as an expensive one, so the cost no longer adds.
			constexpr int SquadSeats = 4;
			constexpr int CitySeatsCounted = 24;
			constexpr int CitySeatBonus = 40;
			const int cityRoom = std::min(CitySeatsCounted, std::max(0, urbanGarrisonRoom - SquadSeats));
			const int garrisonBonus = 260 + std::min(240, GarrisonFirepowerOf(pInf)) + std::min(SquadSeats, urbanGarrisonRoom) * 30
				+ cityRoom * CitySeatBonus;
			score += (urbanExpertInCharge || urbanOpportunityNow) ? garrisonBonus : garrisonBonus / 2;

			if (cityRoom > 0)
				score -= pInf->Cost / 2;
		}

		if (urbanNeedClearer && IsGarrisonClearerType(pInf))
			score += 450;

		// Train compatible Occupier infantry whenever we own an empty or under-filled Battle Bunker
		for (auto const pOwnB : pHouse->Buildings)
		{
			if (pOwnB && !pOwnB->InLimbo && pOwnB->IsAlive && pOwnB->Type && pOwnB->Type->IsBaseDefense
				&& pOwnB->Type->CanBeOccupied
				&& pOwnB->Occupants.Count < std::clamp(pOwnB->Type->MaxNumberOccupants, 1, 10) && pInf->Occupier
				&& !IsCaptureInfantryType(pInf) && !IsSpyInfantryType(pInf))
			{
				score += 650;
				break;
			}
		}

		if (features[77] > 0 && !IsCaptureInfantryType(pInf) && !IsSpyInfantryType(pInf)
			&& (pInf->ImmuneToPsionics || (pWpn && pWpn->Range >= (6 * 256))))
		{
			score += 380; // Counter enemy MindControl with Snipers/Virus/Cyborgs/Dogs!
		}

		if ((features[72] > 0 || (detectionNeed & DetectionNeedStealth)) && (pInf->Sensors || pInf->DetectDisguise || pInf->Doggie))
			score += 420;

		if (SlotGate(scores[82], 60, 82) && ownHouseBit != 0u && !pInf->InOwners(ownHouseBit) && !IsCaptureInfantryType(pInf))
			score += 220 + std::clamp(static_cast<int>(scores[82]) / 3, 0, 200);

		const bool infCanSelfCrossWater = (pInf->MovementZone == MovementZone::Amphibious
			|| pInf->MovementZone == MovementZone::AmphibiousDestroyer
			|| pInf->MovementZone == MovementZone::AmphibiousCrusher || pInf->MovementZone == MovementZone::Fly
			|| pInf->SpeedType == SpeedType::Hover || pInf->SpeedType == SpeedType::Winged || pInf->JumpJet
			|| pInf->Teleporter);

		if (features[21] > 0 && !IsCaptureInfantryType(pInf))
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

		// Under attack captures and infiltrations wait for the answer to the threat.
		if (GetMacroState(pHouse).CounterUnderAttack && (IsCaptureInfantryType(pInf) || IsSpyInfantryType(pInf)))
			score -= ThreatFirstPenalty;

		if (score > bestInfScore)
		{
			bestInfScore = score;

			bestInfIdx = pInf->ArrayIndex;
		}

		if (garrisonSeatsToFill > 0 && IsGarrisonOccupierType(pInf) && score > bestGarrisonScore)
		{
			bestGarrisonScore = score;
			bestGarrisonIdx = pInf->ArrayIndex;
		}
	}

	// Soldiers for the buildings are trained on purpose (user: do not wait for soldiers that happen
	// to be idle, produce them for those bunkers): while buildings worth holding have seats left
	// beyond the occupiers we have outside, the barracks train the best soldier for a seat ahead of
	// the scored pick, with or without money in hand (a soldier is cheap and is paid as the money
	// comes in; a house is often broke for minutes and the seat would go to a dearer pick). The
	// first engineer still goes first, and under attack the answer to the threat does.
	if (bestGarrisonIdx >= 0 && bestInfIdx != bestGarrisonIdx && !GetMacroState(pHouse).CounterUnderAttack)
	{
		auto const pScoredPick = bestInfIdx >= 0 ? InfantryTypeClass::Array.GetItem(bestInfIdx) : nullptr;

		if (!pScoredPick || !IsCaptureInfantryType(pScoredPick) || ownedEngineers >= 1)
		{
			bestInfIdx = bestGarrisonIdx;
			CountDecision(NeuralDecision::GarrisonTraining, pHouse->ArrayIndex);
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
	// No ceiling by number: the barracks wait while soldiers go unused (IsArmyKindUnused), crowd
	// the base, or wait on the islands for more transports than we own.
	constexpr int IslandQueueInfantry = 45;
	const bool baseInfantrySaturated = IsArmyKindUnused(pHouse, ArmyKind::Infantry) || congestedBaseInfantry >= 24
		|| (islandInfantryQueueFull && pHouse->OwnedInfantry - garrisonedInfantry >= IslandQueueInfantry);

	// Only pause secondary queues to ration finite credits when all income
	// sources are lost (!hasActiveUnitIncome) or when the base is saturated
	if (!hasActiveUnitIncome
			&& (((canRebuildMCVNow && canSafelySaveForMCV && money < rebuildMCVCost) || savingForIslandTransport
					|| ownedEconomyUnits == 0 || trueRefineries == 0)
				&& features[65] < 35)
		|| baseInfantrySaturated)
	{
		if (bestInfIdx < 0 || !IsCaptureInfantryType(InfantryTypeClass::Array.GetItem(bestInfIdx))
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

	// The war factories and shipyards get the choice at once, like the barracks and airfields below
	// (RefreshProductionIndices repeats it every 8 frames).
	pHouse->ProducingUnitTypeIndex = pExt->NeuralAI_NextUnitTarget;
	pExt->ProducingNavalUnitTypeIndex = pExt->NeuralAI_NextNavalTarget;

	// Ares replaces HouseClass::AI_Infantry from its first instruction (and only picks a type while
	// the index is -1), so the AI_Infantry hook of UpdateInfantryProduction never runs with Ares:
	// hand the choice to the barracks here, or no infantry (engineers, spies) is ever trained.
	pHouse->ProducingInfantryTypeIndex = bestInfIdx;

	// WatchProductionPick: a pick that the engine leaves unstarted for 900 frames while its queue is
	// empty, a factory of the kind exists and the money covers it is banned for 5400 frames, so the
	// next best type is picked (match #7132: a Yuri AI with 230000-290000 credits and 11 war
	// factories built nothing for 40 minutes; training: "Production stall | ... next unit HTK inf
	// SENGINEER | unit=none inf=none" with 66000 credits). The engine's own AI verdict is logged to
	// find the rule behind it (the picks use the human-rules check).
	{
		auto& pickMacro = GetMacroState(pHouse);
		const int pickNow = Unsorted::CurrentFrame;
		auto const watchPick = [&](int queue, int typeIdx, TechnoTypeClass* pType, AbstractType kind, int factories)
		{
			int& watchedIdx = pickMacro.ProductionPickIdx[static_cast<size_t>(queue)];
			int& watchedSince = pickMacro.ProductionPickSince[static_cast<size_t>(queue)];
			// The queue is busy while any factory of ours holds an object of its kind (the house's
			// primary factory pointer is empty for AI houses, which build through their buildings).
			bool queueBusy = false;

			for (auto const pFactory : FactoryClass::Array)
			{
				auto const pObjType = (pFactory && pFactory->Owner == pHouse && pFactory->Object) ? pFactory->Object->GetTechnoType() : nullptr;

				if (pObjType && pObjType->WhatAmI() == kind
					&& (kind != AbstractType::UnitType || !static_cast<UnitTypeClass*>(pObjType)->Naval))
				{
					queueBusy = true;
					break;
				}
			}

			if (typeIdx < 0 || !pType || queueBusy || factories <= 0
				|| pHouse->Available_Money() < pType->GetActualCost(pHouse))
			{
				watchedIdx = -1;
				return;
			}

			if (watchedIdx != typeIdx || pickNow < watchedSince)
			{
				watchedIdx = typeIdx;
				watchedSince = pickNow;
				return;
			}

			if (pickNow - watchedSince < 900)
				return;

			// Diagnostic only for now: the first training run banned harvesters, engineers and riflemen
			// the engine called buildable (AI verdict 1 in all 31 cases), so the block is elsewhere and
			// a ban only removed good picks. The log keeps what the engine sees at that moment.
			watchedIdx = -1;

			static int banLogSerial = -1;
			static int banLogCount = 0;

			if (banLogSerial != NeuralMatchSerial)
			{
				banLogSerial = NeuralMatchSerial;
				banLogCount = 0;
			}

			if (banLogCount++ < 30)
			{
				auto const pFactoryBld = pType->FindFactory(true, true, true, pHouse);
				Debug::Log("[NeuralAI] House %d production: %s never started in 900 frames (%d credits, %d factories, engine AI verdict %d, "
					"factory found %s, engine indices unit %d inf %d bld %d), frame %d.\n",
					pHouse->ArrayIndex, pType->ID, pHouse->Available_Money(), factories,
					static_cast<int>(pHouse->CanBuild(pType, false, false)), pFactoryBld ? pFactoryBld->Type->ID : "none",
					pHouse->ProducingUnitTypeIndex, pHouse->ProducingInfantryTypeIndex, pHouse->ProducingBuildingTypeIndex, pickNow);
			}

			CountDecision(NeuralDecision::ProductionPickBanned, pHouse->ArrayIndex);
		};

		const int unitPick = pExt->NeuralAI_NextUnitTarget;
		watchPick(1, unitPick, (unitPick >= 0 && unitPick < UnitTypeClass::Array.Count) ? UnitTypeClass::Array.GetItem(unitPick) : nullptr,
			AbstractType::UnitType, pHouse->NumWarFactories);
		watchPick(2, bestInfIdx, (bestInfIdx >= 0 && bestInfIdx < InfantryTypeClass::Array.Count) ? InfantryTypeClass::Array.GetItem(bestInfIdx) : nullptr,
			AbstractType::InfantryType, pHouse->NumBarracks);
	}

	// Picks that never come out of their factory are banned (WatchUndeliveredPick).
	if (const int unitPick = pExt->NeuralAI_NextUnitTarget; unitPick >= 0 && unitPick < UnitTypeClass::Array.Count)
		WatchUndeliveredPick(pHouse, UnitTypeClass::Array.GetItem(unitPick), pHouse->NumWarFactories);

	if (const int navalPick = pExt->NeuralAI_NextNavalTarget; navalPick >= 0 && navalPick < UnitTypeClass::Array.Count)
		WatchUndeliveredPick(pHouse, UnitTypeClass::Array.GetItem(navalPick), pHouse->NumShipyards);

	if (bestInfIdx >= 0 && bestInfIdx < InfantryTypeClass::Array.Count)
		WatchUndeliveredPick(pHouse, InfantryTypeClass::Array.GetItem(bestInfIdx), pHouse->NumBarracks);

	productionClock.Mark(NeuralSection::ProdAircraft);

	// Aircraft Target Selection
	int bestAirIdx = -1;
	int bestAirScore = -32000;
	int bestDockWaitScore = -32000;

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

		const bool airForceShotDown = AirForceSurvivalPercent(RefreshAirForceSummary(pHouse)) < 50;

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

			// A pick that never came out of its factory (WatchUndeliveredPick).
			if (!isAirInProgress && IsProductionBanned(pHouse, 4, pAir->ArrayIndex))
				continue;

			if (!isAirInProgress && !IsUnitTierAllowed(pHouse, pAir))
				continue;

			// A type that needs a dock is started only while one is free. A finished aircraft with no
			// dock cannot leave the airfield: the engine abandons it when the factory's retry timer
			// runs out and the queue starts the same type again, so the production shows an aircraft
			// that never comes (user; 459 Banshee productions abandoned for 107 fielded in 25 matches).
			const bool waitsForDock = !isAirInProgress && (pAir->AirportBound || pAir->Dock.Count > 0)
				&& ownedDockingAircraft >= pHouse->AirportDocks;

			// An air force shot down as fast as it is built gets no more combat aircraft until the
			// losses fade (NeuralAI.AirForce.inl); transports and carryalls are not affected.
			if (!isAirInProgress && airForceShotDown && IsAirForceCraft(pAir))
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
				const int ferryDelta = std::max({ scoreFerryTransport(pAir), raidTransportBonus(pAir), crossingNeedBonus(pAir) });

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
				? std::clamp(airDeficit * 4, 0, 800) + AirDeficitSlotLongRamp(airDeficit)
				: 0;

			// An offensive bank against an enemy across the water buys aircraft that strike the ground.
			const int bankStrikeBonus = (HasOffensiveBank(pHouse) && GetMacroState(pHouse).EnemyAcrossWater && GroundRangeOf(pAir) > 0) ? 450 : 0;
			// With the sea ours, the same aircraft reach the island the warships cannot (SeaControl*).
			const int seaControlStrikeBonus = (seaControlAcross && !pAir->Carryall && GroundRangeOf(pAir) > 0) ? SeaControlAirStrikeBonus : 0;
			// With the land ours and the remnants at sea, aircraft reach the ships anywhere (LandControl*).
			const int landControlStrikeBonus = (landControl && !pAir->Carryall && GroundRangeOf(pAir) > 0) ? LandControlAirBonus : 0;

			const int score = (pAir->Cost / 3) + (pAir->Strength / 2) + slotScore + crossFactionAirBonus + airTransBonus
				+ nonDockBoundBonus + interceptorBonus + (isAirInProgress ? 850 : 0) + CounterBonusFor(pHouse, pAir)
				+ AirForceProductionBonus(pHouse, pAir) + bankStrikeBonus + seaControlStrikeBonus + landControlStrikeBonus;

			// Not started, but wanted: the best of these asks for another airfield below.
			if (waitsForDock)
			{
				bestDockWaitScore = std::max(bestDockWaitScore, score);
				continue;
			}

			if (score > bestAirScore)
			{
				bestAirScore = score;

				bestAirIdx = pAir->ArrayIndex;
			}
		}

		// The aircraft the house would build next if it had a dock for it: the building pass of the
		// next ticks raises its airfield cap (AirDockWantedFrame).
		if (bestDockWaitScore > bestAirScore && bestDockWaitScore > 0)
			GetMacroState(pHouse).AirDockWantedFrame = Unsorted::CurrentFrame;
	}

	if (!hasActiveUnitIncome
		&& ((canRebuildMCVNow && canSafelySaveForMCV && money < rebuildMCVCost) || ownedEconomyUnits == 0
			|| trueRefineries == 0))
	{
		bestAirIdx = -1; // Zero-Income Emergency: save remaining finite credits to
						// restore economy/MCV first!
	}

	pExt->NeuralAI_NextAircraftTarget = bestAirIdx;

	// Ares replaces HouseClass::AI_Aircraft the same way (0x4FF210 jumps past the
	// UpdateAircraftProduction hook), so the airfields get the choice here too.
	pHouse->ProducingAircraftTypeIndex = bestAirIdx;

	if (bestAirIdx >= 0 && bestAirIdx < AircraftTypeClass::Array.Count)
		WatchUndeliveredPick(pHouse, AircraftTypeClass::Array.GetItem(bestAirIdx), pHouse->NumAirpads);

	// TEMP diagnostic: productions that are finished and still in their factory (no dock, no cell to
	// place or leave to, a build limit). The engine abandons them when the factory's retry timer
	// runs out ("Computer is abandoning production of ...", 10 to 30 per 10000 frames of a match).
	if (IsAITickWindow(900, 0))
	{
		int dockingAircraft = 0;

		for (auto const pOwnAir : AircraftClass::Array)
		{
			if (pOwnAir && pOwnAir->Owner == pHouse && !pOwnAir->InLimbo && pOwnAir->IsAlive && pOwnAir->Type
				&& !pOwnAir->SpawnOwner && (pOwnAir->Type->AirportBound || pOwnAir->Type->Dock.Count > 0))
			{
				dockingAircraft++;
			}
		}

		for (auto const pFactory : FactoryClass::Array)
		{
			if (!pFactory || pFactory->Owner != pHouse || !pFactory->Object || !pFactory->IsDone())
				continue;

			auto const pDoneType = pFactory->Object->GetTechnoType();

			Debug::Log("[NeuralAI] House %d production DIAG: %s is finished and still in its factory (kind %d, on hold %d, suspended %d), "
				"docks %d with %d aircraft that need one, frame %d.\n", pHouse->ArrayIndex, pDoneType ? pDoneType->ID : "?",
				static_cast<int>(pFactory->Object->WhatAmI()), pFactory->OnHold ? 1 : 0, pFactory->IsSuspended ? 1 : 0,
				pHouse->AirportDocks, dockingAircraft, Unsorted::CurrentFrame);
		}
	}

	// Bank telemetry: which queues stay empty while the bank is rich.
	// 0 when one of our factories is building an object of that kind, 1 when none is although a
	// factory building of that kind exists, -1 without such a building.
	auto const queueFactoryIdle = [pHouse](AbstractType kind, bool naval) -> int
	{
		for (auto const pFactory : FactoryClass::Array)
		{
			if (!pFactory || pFactory->Owner != pHouse || !pFactory->Object || pFactory->OnHold)
				continue;

			auto const pObjType = pFactory->Object->GetTechnoType();

			if (!pObjType || pObjType->WhatAmI() != kind
				|| (kind == AbstractType::UnitType && static_cast<UnitTypeClass*>(pObjType)->Naval != naval))
			{
				continue;
			}

			return 0;
		}

		for (auto const pOwnBld : pHouse->Buildings)
		{
			if (pOwnBld && pOwnBld->IsAlive && !pOwnBld->InLimbo && pOwnBld->Type && pOwnBld->Type->Factory == kind
				&& (kind != AbstractType::UnitType || pOwnBld->Type->Naval == naval))
			{
				return 1;
			}
		}

		return -1;
	};

	// Bank governor input (UpdateBankGovernor): whether each queue it may grow is producing.
	{
		auto& queueMacro = GetMacroState(pHouse);
		const int queueIdle[] = { queueFactoryIdle(AbstractType::UnitType, false), queueFactoryIdle(AbstractType::InfantryType, false),
			queueFactoryIdle(AbstractType::AircraftType, false), queueFactoryIdle(AbstractType::UnitType, true) };

		for (size_t q = 0; q < std::size(queueIdle); ++q)
		{
			if (queueIdle[q] < 0)
				continue;

			queueMacro.BankQueueTicks[q]++;

			if (queueIdle[q] == 0)
				queueMacro.BankQueueBusyTicks[q]++;
		}
	}

	if (pHouse->ArrayIndex >= 0 && pHouse->ArrayIndex < 32)
	{
		auto& bank = BankStats[pHouse->ArrayIndex];
		bank.PeakCredits = std::max(bank.PeakCredits, money);
		bank.CreditsSum += money;
		bank.Ticks++;

		if (GetMacroState(pHouse).BankGovernorActive)
			bank.GovernorTicks++;

		if (money >= 5000)
		{
			bank.RichTicks++;
			const int targets[] = { pExt->NeuralAI_NextBuildingTarget, bestUnitIdx, bestNavalIdx, bestInfIdx, bestAirIdx };
			const int idle[] = { queueFactoryIdle(AbstractType::BuildingType, false), queueFactoryIdle(AbstractType::UnitType, false),
				queueFactoryIdle(AbstractType::UnitType, true), queueFactoryIdle(AbstractType::InfantryType, false),
				queueFactoryIdle(AbstractType::AircraftType, false) };

			for (size_t q = 0; q < static_cast<size_t>(NeuralBankQueue::Count); ++q)
			{
				if (idle[q] < 0)
					continue;

				if (targets[q] < 0)
					bank.NoTarget[q]++;

				if (idle[q] > 0)
					bank.FactoryIdle[q]++;
			}
		}
	}

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

		// Bank governor state (UpdateBankGovernor): the army, what of it went unused (vehicles,
		// soldiers, ships; IsArmyKindUnused), factory counts and what the queues can spend against
		// the income and the surplus to spend (credits per 1000 frames).
		auto const& govMacro = GetMacroState(pHouse);
		Debug::Log("[NeuralAI] Bank | House #%d | Spent=%d | Gnd=%d Nav=%d Inf=%d Unused=%d/%d/%d Air=%d Blds=%d | cy=%d wf=%d rax=%d air=%d "
				"ship=%d | governor=%d income=%d capacity=%d needed=%d surplus=%d wants=%d/%d/%d/%d yards=%d\n",
			pHouse->ArrayIndex, pHouse->CreditsSpent, ownedGroundCombatUnits, ownedNavalOrAmphibCombatUnits, pHouse->OwnedInfantry,
			govMacro.ArmyUnused[0], govMacro.ArmyUnused[1], govMacro.ArmyUnused[2],
			pHouse->OwnedAircraft, pHouse->Buildings.Count, pHouse->NumConYards, pHouse->NumWarFactories,
			pHouse->NumBarracks, pHouse->NumAirpads, pHouse->NumShipyards, govMacro.BankGovernorActive ? 1 : 0,
			govMacro.BankIncomeRate, govMacro.BankSpendCapacity, govMacro.BankSpendNeeded, govMacro.BankSurplus,
			govMacro.BankWantedFactories[0], govMacro.BankWantedFactories[1], govMacro.BankWantedFactories[2],
			govMacro.BankWantedFactories[3], govMacro.BankWantedYards);

		// Army mix: the front line vehicles against the guns they allow (artilleryAllowance), the
		// vehicles that shoot at aircraft against the escort the tank mass wants, the refineries,
		// and for the tank mass the unit count its next wave waits for with the waves in a row
		// that took no building.
		Debug::Log("[NeuralAI] Mix | House #%d | Arch=%d | front=%d guns=%d allowed=%d aa=%d wanted=%d | refineries=%d | wave=%d "
				"lesson=%d\n",
			pHouse->ArrayIndex, pExt->NeuralAI_Archetype, ownedFrontLineVehicles, ownedArtilleryVehicles, artilleryAllowance,
			ownedAntiAirVehicles, wantedAntiAirEscorts, pHouse->CountResourceDestinations, tankMass ? TankMassWaveUnits(pHouse) : 0,
			WaveLessonStepsNow(pHouse));
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

// The house production check (every 8 frames) hands every queue the pick of the last AI tick and
// skips the legacy AI_Building / AI_Unit / AI_Infantry / AI_Aircraft chain. That chain ran whenever
// one queue had no free factory (an airfield with every dock taken), and the Ares AI_Infantry and
// AI_Aircraft in it cleared the Neural AI picks, so barracks and airfields sat idle with money in
// the bank. A queue whose object just left also gets its next pick at once instead of at the next
// AI tick (15 to 105 frames by difficulty), like a human's repeat queue.
void NeuralAI::RefreshProductionIndices(HouseClass* pHouse)
{
	NeuralAI::UpdateBuildingProduction(pHouse);
	NeuralAI::UpdateUnitProduction(pHouse);
	NeuralAI::UpdateInfantryProduction(pHouse);
	NeuralAI::UpdateAircraftProduction(pHouse);
}

// One-off picks (MCVs and other deployers, harvesters, engineers, spies, unarmed transports and
// support units) are never repeated by RefreshProductionIndices: the next AI tick decides again
// whether another one is needed.
void NeuralAI::OnTechnoProduced(HouseClass* pHouse, TechnoClass* pTechno)
{
	auto const pType = pTechno ? pTechno->GetTechnoType() : nullptr;

	if (!pType || pTechno->WhatAmI() == AbstractType::Building)
		return;

	// What came out of a factory (WatchUndeliveredPick).
	if (const int queue = ProductionQueueOf(pType); queue != 0)
		GetMacroState(pHouse).LastDeliveredFrame[queue * 100000 + pType->GetArrayIndex()] = Unsorted::CurrentFrame;

	// TEMP diagnostic (tank census, NoteMainTankDeath): a main battle tank out of a factory.
	if (auto const pTankType = abstract_cast<UnitTypeClass*>(pType); pTankType && IsMainBattleTankType(pTankType))
	{
		auto& macro = GetMacroState(pHouse);
		macro.TankCensusBuilt++;
		macro.TankCensusBirth[pTechno->UniqueID] = Unsorted::CurrentFrame;
	}

	auto const pUnitType = abstract_cast<UnitTypeClass*>(pType);
	auto const pInfType = abstract_cast<InfantryTypeClass*>(pType);

	// A deployer the building pick asked for takes its order: the building and the goal it was asked
	// for (runBaseDeployer, NeuralAI.Micro.inl).
	if (pUnitType && IsBaseDeployerUnit(pUnitType))
	{
		auto& macro = GetMacroState(pHouse);
		auto const itBuild = std::find_if(macro.DeployerBuilds.begin(), macro.DeployerBuilds.end(),
			[pUnitType](const NeuralDeployerBuild& build) { return build.UnitType == pUnitType->ArrayIndex; });

		if (itBuild != macro.DeployerBuilds.end())
		{
			auto& order = macro.DeployOrders[pTechno->UniqueID];
			order = NeuralDeployOrder {};
			order.BuildingType = itBuild->BuildingType;
			order.Goal = itBuild->Goal;
			order.Kind = itBuild->Kind;
			order.StartFrame = Unsorted::CurrentFrame;
			order.ProgressFrame = Unsorted::CurrentFrame;
			order.SeenFrame = Unsorted::CurrentFrame;
			macro.DeployerBuilds.erase(itBuild);
		}
	}

	auto const pWeapon = pType->GetWeapon(0);
	const bool armed = pWeapon && pWeapon->WeaponType && pWeapon->WeaponType->Damage > 0;
	const bool oneOff = !armed || (pUnitType && (pUnitType->DeploysInto || pUnitType->Harvester || pUnitType->ResourceGatherer))
		|| (pInfType && (pInfType->Engineer || IsSpyInfantryType(pInfType)));

	if (!oneOff)
		return;

	auto const pExt = HouseExt::Fetch(pHouse);
	const int index = pType->GetArrayIndex();

	if (pUnitType)
	{
		if (pExt->NeuralAI_NextUnitTarget == index)
			pExt->NeuralAI_NextUnitTarget = -1;

		if (pExt->NeuralAI_NextNavalTarget == index)
			pExt->NeuralAI_NextNavalTarget = -1;
	}
	else if (pInfType)
	{
		if (pExt->NeuralAI_NextInfantryTarget == index)
			pExt->NeuralAI_NextInfantryTarget = -1;
	}
	else if (pExt->NeuralAI_NextAircraftTarget == index)
	{
		pExt->NeuralAI_NextAircraftTarget = -1;
	}
}

void NeuralAI::UpdateAircraftProduction(HouseClass* pHouse)
{
	auto const pExt = HouseExt::Fetch(pHouse);

	if (Unsorted::CurrentFrame - pExt->NeuralAI_LastInferenceFrame > 60)
		NeuralAI::UpdateExpertAI(pHouse);

	pHouse->ProducingAircraftTypeIndex = pExt->NeuralAI_NextAircraftTarget;
}


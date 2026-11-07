// NeuralAI.Experts.inl: Deterministic mixture of experts and slot scoring.
// Part of NeuralAI.cpp (included in order; one translation unit).

namespace
{
	// Whether this house can ever field an air force (an aircraft or an aircraft factory its side
	// and tech level allow, prerequisites aside). Cached per house for 1800 frames.
	std::array<int, 32> AirTechCheckFrame {};
	std::array<bool, 32> AirTechPossible {};
	int AirTechMatchSerial { -1 };

	bool HouseCanEverFieldAircraft(HouseClass* pHouse)
	{
		const int idx = pHouse->ArrayIndex;

		if (idx < 0 || idx >= 32)
			return true;

		const int now = Unsorted::CurrentFrame;

		if (AirTechMatchSerial != NeuralMatchSerial)
		{
			AirTechMatchSerial = NeuralMatchSerial;
			AirTechCheckFrame.fill(0);
		}

		if (AirTechCheckFrame[idx] > 0 && now >= AirTechCheckFrame[idx] && now - AirTechCheckFrame[idx] < 1800)
			return AirTechPossible[idx];

		AirTechCheckFrame[idx] = std::max(1, now);
		AirTechPossible[idx] = false;

		for (auto const pType : AircraftTypeClass::Array)
		{
			if (pType && !pType->Spawned && pHouse->CanBuild(pType, false, true) != CanBuildResult::Unbuildable)
			{
				AirTechPossible[idx] = true;
				return true;
			}
		}

		for (auto const pType : BuildingTypeClass::Array)
		{
			if (pType && pType->Factory == AbstractType::AircraftType && pHouse->CanBuild(pType, false, true) != CanBuildResult::Unbuildable)
			{
				AirTechPossible[idx] = true;
				return true;
			}
		}

		return false;
	}

	// Expert 4 work summary (NeuralAI.SpecOps.inl, refreshed every 150 frames).
	int SpecOpsOpportunity(HouseClass* pHouse);
	void GetSpecOpsWork(HouseClass* pHouse, int& outTechValue, int& outSpyTargets, bool& outBridgeRepair, bool& outBridgeDemolition);

	// Expert 9 work summary (NeuralAI.Urban.inl, refreshed every 150 frames).
	int UrbanOpportunity(HouseClass* pHouse);
}

void NeuralAI::RunDeterministicMoE(HouseClass* pHouse, const std::array<int8_t, InputVectorSize>& features,
	std::array<int16_t, OutputVectorSize>& outScores)
{
	outScores.fill(0);

	auto const pExt = HouseExt::Fetch(pHouse);

	const AIBackpackFile& policy = NeuralAI::PolicyFor(pHouse);

	// Select active specialist expert (0..9) combining situational priors +
	// evolved Expert Routing Bias (Slots 40..47 & 84..85) + learned Faction Memory + context routing
	std::array<int, 10> expScores = { 100, 100, 100, 80, 100, 160, 110, 110, 95, 95 };

	if (pHouse->HasLowPower() || pHouse->CountResourceGatherers == 0 || pHouse->CountResourceDestinations == 0
		|| pHouse->NumConYards == 0)
	{
		expScores[0] += 950; // Expert 0: Economy, Silos & Crisis Micro
	}

	if (pHouse->NumBarracks == 0 || pHouse->NumWarFactories == 0
		|| (pHouse->Available_Money() > 2500 && pHouse->OwnedBuildings < 8))
	{
		expScores[1] += 700; // Expert 1: Spatial Tetris Base Architect
	}

	if (features[66] > 24 || features[65] > 36)
		expScores[2] += 420; // Expert 2: Dynamic Counter-Strategy

	// A counter-composition pays most against a lopsided army: 8 points per share point of the
	// dominant seen enemy category above 45% (up to 320), scaled by the opponent model confidence.
	{
		auto const& seenFoe = GetMacroState(pHouse).OpponentProfile;
		int dominantShare = 0;

		for (int c = 0; c < static_cast<int>(EnemyForceCategory::Count); ++c)
			dominantShare = std::max(dominantShare, seenFoe.Share(static_cast<EnemyForceCategory>(c)));

		if (dominantShare > 45)
			expScores[2] += std::min(320, (dominantShare - 45) * 8) * seenFoe.Confidence() / 100;
	}

	if (HouseClass::Array.Count > 4 && (IsAITickWindow(240, 60)))
		expScores[3] += 380; // Expert 3: Multi-Player Opportunism & Diplomacy

	// Expert 4: Special Operations, graded by the work there is for it now (fog-fair): known
	// reachable neutral techs out of enemy defense reach and an engineer to take them, enemy
	// buildings worth a spy, bridges to repair or bring down, captures under way (up to 780).
	const int specOpsOpportunity = SpecOpsOpportunity(pHouse);
	expScores[4] += specOpsOpportunity + (pExt->NeuralAI_Archetype == 1 ? 200 : 0);

	// Expert 5: Tactical Squad Micro, Kiting & Beacons: an army in contact (enemy tanks seen),
	// stronger the bigger it is.
	if (pHouse->OwnedUnits >= 3 || pHouse->OwnedInfantry >= 5)
		expScores[5] += 300 + (features[65] > 0 ? std::min(300, (pHouse->OwnedUnits + pHouse->OwnedInfantry / 2) * 10) : 0);

	const bool desperateForExpansionResources
		= (features[19] < 28 && (features[20] > 6 || features[21] > 0 || features[25] > 0));
	if (desperateForExpansionResources
		|| ((pHouse->CountResourceDestinations < 3 || features[19] < 28) && features[20] > 10
			&& pHouse->Available_Money() >= 1200))
	{
		expScores[6] += desperateForExpansionResources ? 720 : 480; // Expert 6: Base-Crawling & Ore Outpost Expansion
	}
	else if (GetMacroState(pHouse).UncolonizedOreFields >= 2 && pHouse->Available_Money() >= 15000)
	{
		// Plenty of money and ore fields nobody of ours holds yet: claim them anyway. Fields are a
		// life insurance (stolen or drained money comes back) and denied ground for the enemy.
		expScores[6] += 380;
	}

	// Expert 7: Forward Defense Crawling & Chokepoint Siege, graded by real pressure: enemy defenses
	// facing us (8 per visible one), enemy tanks seen (6 each), our buildings damaged, harvesters
	// under attack. Merely seeing the enemy base (visible buildings) used to give the full bonus,
	// so Expert 7 ran 94% of the time and the others never trained.
	if (pHouse->OwnedBuildings >= 5)
	{
		const int pressure = (features[68] / 8) * 60 + (features[65] / 6) * 25 + (features[29] > 0 ? 200 : 0)
			+ (features[31] > 0 ? 250 : 0) + ((pExt->NeuralAI_Archetype == 2 || pExt->NeuralAI_Archetype == 4) ? 200 : 0);

		if (pressure >= 150)
			expScores[7] += std::min(680, pressure);
	}

	if (pHouse->NumAirpads > 0 || pHouse->OwnedAircraft >= 2 || features[66] > 20 || features[24] > 0
		|| pExt->NeuralAI_Archetype == 6)
	{
		expScores[8] += 450; // Expert 8: Airforce & Sky Dominance
	}

	// Expert 9: Urban Garrison & Allied Coalition Warfare, graded by the work there is for it now
	// (fog-fair): known buildings worth holding and occupiers to fill them, enemy garrisons seen
	// near our base, battle bunkers to man, garrisons to evacuate, allies to rescue or join (up to
	// 720). It used to get +430 whenever any occupiable building stood anywhere on the map.
	const int urbanOpportunity = UrbanOpportunity(pHouse);
	expScores[9] += urbanOpportunity + (pExt->NeuralAI_Archetype == 7 ? 200 : 0);

	// Common-sense vetoes: an expert that cannot do its job here is never chosen, whatever the
	// learned bias says. Airforce with no aircraft we could ever build, own or fight; Urban with no
	// building to occupy, no garrison and no allied coalition; Expansion on a map without any
	// ore field or capturable tech; Special Operations with nothing to capture, spy or bridge.
	std::array<bool, 10> vetoed {};
	vetoed[4] = specOpsOpportunity == 0 && pExt->NeuralAI_Archetype != 1;
	vetoed[8] = pHouse->OwnedAircraft == 0 && pHouse->NumAirpads == 0 && features[66] <= 20 && !HouseCanEverFieldAircraft(pHouse);
	vetoed[9] = urbanOpportunity == 0 && pExt->NeuralAI_Archetype != 7;
	vetoed[6] = GetOreFields().empty() && features[25] == 0;

	const int curArch = std::clamp(pExt->NeuralAI_Archetype, 0, NeuralAI::NumArchetypes - 1);
	const int previousExpert = std::clamp(pExt->NeuralAI_ActiveExpert, 0, NeuralAI::NumSpecialists - 1);
	std::array<int, 10> finalScores {};
	const int hysteresis = (NeuralAI::AllowBackpackInference && NeuralAI::BackpackLoaded)
		? std::clamp(60 + static_cast<int>(policy.OutputBias[127]), 0, 200)
		: 60;
	int activeExpert = 0;
	int bestExpScore = -999999;
	const int routingPhase = RoutingPhase(Unsorted::CurrentFrame);

	// Context routing (learned per game phase and map class), capped at +-150 so the situation
	// priors still decide. Only with a loaded table and a known map class: otherwise 0.
	const NeuralContextRouting& contextRouting = ContextRoutingFor(pHouse);
	int routingMapClass = -1;

	if (NeuralAI::AllowBackpackInference && NeuralAI::BackpackLoaded && contextRouting.Loaded)
	{
		ComputeMapInfo();
		routingMapClass = CurrentMapClass();
	}

	for (int e = 0; e < NeuralAI::NumSpecialists; ++e)
	{
		int s = expScores[e];

		if (NeuralAI::AllowBackpackInference && NeuralAI::BackpackLoaded)
		{
			// Learning tilts the choice, the situation makes it: the learned routing bias is capped
			// at +-200 and the per-faction memory at +-150, so no expert is shut out (or made
			// permanent) by a bias the others never get the chance to answer.
			s += std::clamp(static_cast<int>(policy.OutputBias[(e < 8) ? (40 + e) : (84 + (e - 8))]), -200, 200);

			const uint32_t eIdx = static_cast<uint32_t>((pHouse->SideIndex * 80 + curArch * 10 + e) % 512);

			if (policy.Entries[eIdx].VisitCount > 0)
			{
				s += std::clamp(static_cast<int>(
					(e < 8)
						? policy.Entries[eIdx].ExpertScoreDelta[e]
						: policy.Entries[eIdx].BuildCategoryDelta[14 + (e - 8)]) * 6, -150, 150);
			}

			if (routingMapClass >= 0)
				s += std::clamp(static_cast<int>(contextRouting.Bias[routingPhase][routingMapClass][e]), -ContextRoutingCap, ContextRoutingCap);
		}

		// Hysteresis: the current expert keeps the lead on near ties instead of flapping each tick.
		// 60 points plus the learned slot 127 (Expert Hysteresis), from 0 to 200.
		if (e == previousExpert)
			s += hysteresis;

		if (vetoed[e])
			s = -999999;

		finalScores[e] = s;

		if (s > bestExpScore)
		{
			bestExpScore = s;

			activeExpert = e;
		}
	}

	// Training exploration (trainer matches only, PHOBOS_TRAIN_SLOT): every 900-frame window an
	// expert within 300 points of the best takes over for that window, the best one twice as
	// often, so close alternatives get matches to prove themselves. The less an expert has been
	// tried for this side and archetype (faction memory visits), the further behind it may be and
	// still get its turn: up to 250 more points when untried. Deterministic (frame, house), never
	// the game RNG. Real games always take the best.
	if (NeuralAI::TrainingMode)
	{
		std::array<int, 11> pool {};
		int poolSize = 0;

		pool[poolSize++] = activeExpert;

		for (int e = 0; e < NeuralAI::NumSpecialists; ++e)
		{
			if (vetoed[e])
				continue;

			int uncertainty = 250;

			if (NeuralAI::AllowBackpackInference && NeuralAI::BackpackLoaded)
			{
				const uint32_t eIdx = static_cast<uint32_t>((pHouse->SideIndex * 80 + curArch * 10 + e) % 512);
				uncertainty = static_cast<int>(250.0 / std::sqrt(1.0 + static_cast<double>(policy.Entries[eIdx].VisitCount)));
			}

			if (finalScores[e] + uncertainty >= bestExpScore - 300)
				pool[poolSize++] = e;
		}

		const uint32_t window = static_cast<uint32_t>(Unsorted::CurrentFrame / 900);
		const uint32_t mix = (window * 2654435761u) ^ (static_cast<uint32_t>(pHouse->ArrayIndex + 1) * 40503u);
		activeExpert = pool[mix % static_cast<uint32_t>(poolSize)];
	}

	// Easy difficulty disables Spec-Ops Trickster and advanced tactical kiting
	const int diffIdx = pHouse->GetAIDifficultyIndex();

	if (diffIdx == 2 && (activeExpert == 4 || activeExpert == 5 || activeExpert == 7))
		activeExpert = 2;

	pExt->NeuralAI_ActiveExpert = activeExpert;

	if (pHouse->ArrayIndex >= 0 && pHouse->ArrayIndex < 32)
	{
		ExpertTicks[pHouse->ArrayIndex][activeExpert]++;
		ExpertTicksByPhase[pHouse->ArrayIndex][routingPhase][activeExpert]++;
	}

	// Run INT8 Fixed-Point Matrix-Vector Inference on the mapped Base Model
	if (NeuralAI::BaseMappedView && NeuralAI::BaseMappedSize >= sizeof(AIBaseHeader))
	{
		auto const pHdr = reinterpret_cast<const AIBaseHeader*>(NeuralAI::BaseMappedView);

		uint32_t offset = (activeExpert < 8)
			? pHdr->ExpertWeightsOffset[activeExpert]
			: pHdr->ExtraExpertWeightsOffset[activeExpert - 8];

		if (offset == 0)
			offset = pHdr->ExpertWeightsOffset[activeExpert % 8];

		if (offset == 0)
			offset = pHdr->ExpertWeightsOffset[activeExpert % 6];

		const size_t requiredBytes
			= static_cast<size_t>(offset) + (NeuralAI::InputVectorSize * NeuralAI::OutputVectorSize);

		if (offset >= sizeof(AIBaseHeader) && requiredBytes <= NeuralAI::BaseMappedSize)
		{
			const int8_t* weights = reinterpret_cast<const int8_t*>(NeuralAI::BaseMappedView + offset);

			for (int o = 0; o < NeuralAI::OutputVectorSize; ++o)
			{
				int32_t acc = 0;

				const int8_t* row = weights + (o * NeuralAI::InputVectorSize);

				for (int i = 0; i < NeuralAI::InputVectorSize; ++i)
				{
					acc +=

						static_cast<int32_t>(features[i]) * static_cast<int32_t>(row[i]);
				}

				const int lutIdx = std::clamp((acc >> 10) + 128, 0, 255);

				outScores[o] = static_cast<int16_t>(pHdr->TanhLUT[lutIdx]) * 8;
			}
		}
	}

	// Apply Signed Backpack LoRA delta & Learned Faction Strategy Memory in
	// Skirmish / Headless Training
	if (NeuralAI::AllowBackpackInference && NeuralAI::BackpackLoaded)
	{
		std::array<int32_t, 16> rankAct = {};

		for (int r = 0; r < 16; ++r)
		{
			int32_t acc = 0;

			for (int i = 0; i < NeuralAI::InputVectorSize; ++i)
			{
				acc += static_cast<int32_t>(features[i]) *

					static_cast<int32_t>(policy.LoRA_A[r * NeuralAI::InputVectorSize + i]);
			}

			rankAct[r] = acc >> 8;
		}

		for (int o = 0; o < NeuralAI::OutputVectorSize; ++o)
		{
			int32_t delta = 0;

			for (int r = 0; r < 16; ++r)
			{
				delta +=

					rankAct[r] * static_cast<int32_t>(policy.LoRA_B[r * NeuralAI::OutputVectorSize + o]);
			}

			outScores[o] = ClampInt16(outScores[o] + (delta >> 7) + policy.OutputBias[o]);
		}

		// Recall learned strategy outcome deltas from Backpack Entries for this
		// Faction + Archetype + Expert
		const uint32_t entryIdx = static_cast<uint32_t>((pHouse->SideIndex * 80 + curArch * 10 + activeExpert) % 512);

		const auto& memEntry = policy.Entries[entryIdx];

		if (memEntry.VisitCount > 0)
		{
			const int16_t expDelta = (activeExpert < 8)
				? memEntry.ExpertScoreDelta[activeExpert]
				: memEntry.BuildCategoryDelta[14 + (activeExpert - 8)];
			const int16_t learnedBoost = ClampInt16(memEntry.ArchetypeScoreDelta[curArch] * 4 + expDelta * 4);

			for (int o = 16; o < 36; ++o)
				outScores[o] = ClampInt16(outScores[o] + learnedBoost);

			// If this Archetype has a negative win/loss record in the Backpack for
			// this faction, pivot to the highest-scoring alternative Archetype
			// (weighted by Slots 48..52 & 86..88)
			if (memEntry.ArchetypeScoreDelta[curArch] < -18 && (IsAITickWindow(900, 30)))
			{
				int bestAltArch = (curArch + 1) % NeuralAI::NumArchetypes;
				int bestAltScore = -999999;

				for (int a = 0; a < NeuralAI::NumArchetypes; ++a)
				{
					if (a == curArch)
						continue;

					const int altScore = static_cast<int>(outScores[(a < 5) ? (48 + a) : (86 + (a - 5))])
						+ static_cast<int>(memEntry.ArchetypeScoreDelta[a]) * 6;

					if (altScore > bestAltScore)
					{
						bestAltScore = altScore;

						bestAltArch = a;
					}
				}

				pExt->NeuralAI_Archetype = bestAltArch;
			}
		}
	}

	// Mid-Match Adaptive Strategy Pivot: If our current strategy is stalled (e.g.
	// high frame count, low army vs strong enemy), pivot to the strongest
	// alternative Archetype (weighted by Slots 48..52) to surprise the opponent!
	if (Unsorted::CurrentFrame > 5400 && (IsAITickPeriod(2700)) && pHouse->OwnedUnits < 4
		&& features[65] > 20)
	{
		int bestPivotArch = (pExt->NeuralAI_Archetype + 1) % NeuralAI::NumArchetypes;
		int bestPivotScore = -999999;

		for (int a = 0; a < NeuralAI::NumArchetypes; ++a)
		{
			if (a == pExt->NeuralAI_Archetype)
				continue;

			const int pScore = static_cast<int>(outScores[(a < 5) ? (48 + a) : (86 + (a - 5))]);

			if (pScore > bestPivotScore)
			{
				bestPivotScore = pScore;

				bestPivotArch = a;
			}
		}

		pExt->NeuralAI_Archetype = bestPivotArch;
	}

	// Controlled Deterministic Exploration: Inject a small synchronized variation
	// across unit/infantry/aircraft action slots so the AI experiments with
	// alternative unit compositions and tech branches rather than repeating a
	// rigid script
	const int exploreSeed = ((Unsorted::CurrentFrame / 450) * 31 + pHouse->ArrayIndex * 17 + pHouse->SideIndex * 13);

	for (int o = 16; o <= 33; ++o)
	{
		const int exploreBonus = (((exploreSeed + o * 19) % 11) - 5) * 28; // -140 .. +140 exploration window

		outScores[o] = ClampInt16(outScores[o] + exploreBonus);
	}

	// Apply League Archetype & Expert structural priors so the AI executes crisp
	// RTS build orders immediately while integrating all 64 Output Slots:
	const int arch = pExt->NeuralAI_Archetype;

	// Anti-air pressure proportional to the remembered enemy air threat that our
	// current anti-air (units + defenses) does not cover yet.
	const auto& airMacro = GetMacroState(pHouse);
	const bool hasAirThreat = airMacro.AirThreatValue > 0;
	const int airDeficit = std::max(0, airMacro.AirThreatValue - airMacro.OwnAACapacity);

	// Opponent model counters: shift production toward what beats the enemy's observed
	// force mix (weighted by how much of it we have actually seen).
	const auto& foe = airMacro.OpponentProfile;
	const int foeConfidence = foe.Confidence();
	auto counterBonus = [foeConfidence](int sharePct, int maxBonus) -> int
	{
		return maxBonus * sharePct / 100 * foeConfidence / 100;
	};
	const int foeInfantry = foe.Share(EnemyForceCategory::Infantry);
	const int foeVehicles = foe.Share(EnemyForceCategory::Vehicle);
	const int foeArtillery = foe.Share(EnemyForceCategory::Artillery);
	const int foeNaval = foe.Share(EnemyForceCategory::Naval);
	const int foeAir = foe.Share(EnemyForceCategory::Air);

	outScores[0] = ClampInt16(outScores[0]
		+ (pHouse->HasLowPower() ? 2200 : ((pHouse->PowerOutput - pHouse->PowerDrain < 60) ? 1050 : -350)));

	outScores[1] = ClampInt16(outScores[1]
		+ ((pHouse->CountResourceDestinations == 0)
				? 2100
				: ((pHouse->CountResourceDestinations < 2)
						? 1150
						: ((pHouse->CountResourceDestinations < 3 && pHouse->Available_Money() > 2200) ? 650
																										: -150))));

	// Base crawl reached a free ore field (a building anchor within 12 cells of it, seen by the
	// placement scan in the last 1800 frames): a refinery there claims it (AdvAI's
	// ShouldBuildRefinery after a crawl step lands next to Tiberium).
	{
		auto const& fieldMacro = GetMacroState(pHouse);

		if (fieldMacro.FreeFieldAnchorDist <= 12 && Unsorted::CurrentFrame - fieldMacro.FreeFieldFrame <= 1800
			&& pHouse->CountResourceDestinations >= 1)
		{
			outScores[1] = ClampInt16(outScores[1] + 950);
		}
	}

	outScores[2] = ClampInt16(outScores[2] + ((pHouse->NumBarracks == 0) ? 1750 : -200));

	outScores[3] = ClampInt16(outScores[3]
		+ ((pHouse->NumWarFactories == 0)
				? 1950
				: ((pHouse->Available_Money() > 3200 && pHouse->NumWarFactories < 2) ? 850 : -100)));

	outScores[4] = ClampInt16(outScores[4] + ((pHouse->NumWarFactories > 0) ? ((arch == 4) ? 1450 : 1150) : 100));

	outScores[5] = ClampInt16(outScores[5]
		+ ((features[65] > 18 || features[29] > 0 || features[31] > 0 || arch == 2 || activeExpert == 7)
			? 680
			: ((features[65] > 6 || activeExpert == 6) ? 420 : 240)));

	outScores[6] = ClampInt16(outScores[6] + 80 + (hasAirThreat ? 220 : 0) + std::clamp(airDeficit * 5, 0, 950));

	outScores[7] = ClampInt16(
		outScores[7] + (((features[67] > 10 || features[21] > 0) && pHouse->NumShipyards == 0) ? 1550 : -200));

	outScores[8] = ClampInt16(
		outScores[8]
		+ ((pHouse->NumWarFactories > 0 && pHouse->NumAirpads == 0)
			? ((arch == 6 || activeExpert == 8) ? 1100 : 550)
			: -150));

	const bool needsDepotForMCVExpansion = (pHouse->NumWarFactories > 0 && pHouse->CountResourceDestinations >= 1
		&& pHouse->UnitRepairStations.Count == 0
		&& (features[19] < 28 || features[20] > 12 || features[21] > 0 || pHouse->Buildings.Count >= 20));
	outScores[9] = ClampInt16(outScores[9]
		+ ((pHouse->NumWarFactories > 0 && pHouse->CountResourceDestinations >= 1
			&& pHouse->UnitRepairStations.Count == 0)
				? (needsDepotForMCVExpansion ? 1650 : 1050)
				: -250));

	outScores[10] = ClampInt16(outScores[10]
		+ ((GameModeOptionsClass::Instance.SWAllowed && pHouse->Available_Money() > (arch == 4 ? 3200 : 4500))
			? (arch == 4 ? 1150 : 850)
			: -600));

	outScores[11] = ClampInt16(outScores[11] + ((pHouse->IsTiberiumShort || features[14] < 0) ? 1450 : -300));

	// Macro Scaling & Tech/Defense/Naval Bias Slots (12..15): 12: Dual Factory
	// Production Scaling (2nd War Factory / 2nd Barracks) 13: Early Tech Center &
	// Battle Lab Rush 14: Base Defense Perimeter Density 15: Naval Supremacy &
	// Amphibious Commitment
	outScores[12] = ClampInt16(outScores[12] + ((pHouse->Available_Money() > 2600) ? 240 : -80));

	outScores[13] = ClampInt16(outScores[13] + ((arch == 2 || arch == 4) ? 280 : 60));

	outScores[14] = ClampInt16(outScores[14] + ((arch == 2 || activeExpert == 7) ? 260 : 40));

	outScores[15] = ClampInt16(outScores[15] + ((features[67] > 10 || features[21] > 0) ? 350 : -120));

	outScores[2] = ClampInt16(outScores[2] + std::clamp(static_cast<int>(outScores[12]) / 3, -150, 300));

	outScores[3] = ClampInt16(outScores[3] + std::clamp(static_cast<int>(outScores[12]) / 2, -200, 450));

	outScores[4] = ClampInt16(outScores[4] + std::clamp(static_cast<int>(outScores[13]) / 2, -250, 500));

	outScores[5] = ClampInt16(outScores[5] + std::clamp(static_cast<int>(outScores[14]) / 2, -250, 450));

	outScores[6] = ClampInt16(outScores[6] + std::clamp(static_cast<int>(outScores[14]) / 3, -200, 350));

	outScores[7] = ClampInt16(outScores[7] + std::clamp(static_cast<int>(outScores[15]) / 2, -250, 500));

	outScores[10] = ClampInt16(outScores[10] + std::clamp(static_cast<int>(outScores[55]) / 2, -300, 500));

	outScores[16] = ClampInt16(outScores[16] + ((arch == 3) ? 600 : 350));

	outScores[17] = ClampInt16(outScores[17] + ((features[65] > 12) ? 750 : 400));

	outScores[18] = ClampInt16(outScores[18]
		+ ((arch == 1 || pHouse->ToCapture >= 0 || (features[18] > 0 && pHouse->OwnedUnits >= 5)) ? 650 : 250));

	outScores[19] = ClampInt16(outScores[19] + ((arch == 1 && pExt->NeuralAI_InfiltratedEnemyMask == 0) ? 500 : 100));

	outScores[20] = ClampInt16(outScores[20] + 300 + std::clamp(airDeficit * 4, 0, 750));

	outScores[23] = ClampInt16(outScores[23] + ((arch == 2 || activeExpert == 7) ? 260 : 80));

	// Tactical & Spatial Strategy Output Slots (34..39) driven by Neural MoE +
	// LoRA + Evolved OutputBias + StarCraft/AdvAI Depleting-Spawn Urgency:
	const bool localOreDepleting
		= (features[19] < 28 || (Unsorted::CurrentFrame > 2700 && pHouse->CountResourceDestinations < 3));
	outScores[34]
		= ClampInt16(outScores[34] + ((activeExpert == 6 || localOreDepleting || features[20] > 12) ? 420 : 90));

	outScores[35]
		= ClampInt16(outScores[35] + ((activeExpert == 6 || localOreDepleting || features[20] > 10) ? 440 : 60));

	outScores[36] = ClampInt16(outScores[36] + ((arch == 1 || pHouse->ToCapture >= 0) ? 300 : 80));

	outScores[37] = ClampInt16(outScores[37] + ((arch == 3) ? 260 : ((arch == 2) ? -220 : 0)));

	outScores[38] = ClampInt16(outScores[38] + ((activeExpert == 7 || arch == 2) ? 280 : 40));

	outScores[39]
		= ClampInt16(outScores[39] + ((activeExpert == 0 || activeExpert == 6) ? 220 : ((arch == 3) ? -220 : 0)));

	// Feed evolved tactical/spatial slots into production & expansion priorities
	outScores[1] = ClampInt16(outScores[1]
		+ std::clamp(static_cast<int>(outScores[34]) / 3 + static_cast<int>(outScores[39]) / 2, -300, 550));

	outScores[3] = ClampInt16(outScores[3] + std::clamp(static_cast<int>(outScores[34]) / 3, -200, 400));

	outScores[5] = ClampInt16(outScores[5] + std::clamp(static_cast<int>(outScores[38]) / 2, -250, 500));

	outScores[6] = ClampInt16(outScores[6] + std::clamp(static_cast<int>(outScores[38]) / 3, -200, 350));

	outScores[18] = ClampInt16(outScores[18] + std::clamp(static_cast<int>(outScores[36]), -300, 750));

	// Slot 21: Repair Destroyed Bridge via Engineer + BridgeRepairHut (to close the gap
	// in front of an enemy fleet, or to reopen a ground route when ready to push)
	if (features[18] > 0)
	{
		if (features[137] > 0)
			outScores[21] = ClampInt16(outScores[21] + 1600); // Close the channel before the enemy fleet sails through
		else if (pHouse->OwnedUnits * 6 >= features[65] && pHouse->OwnedUnits >= 4)
			outScores[21] = ClampInt16(outScores[21] + 1200); // We have ground superiority: repair bridge
		else
			outScores[21] = ClampInt16(outScores[21] - 1500); // Outnumbered: keep broken bridge as a moat!
	}
	else
	{
		outScores[21] = -2000;
	}

	// Slot 22: Demolish Intact Bridge (Ambush enemy column on bridge, or
	// scorched-earth defense when outnumbered)
	if (features[134] > 0) // Bridges are destroyable (Immune Bridges GameOption is OFF)
	{
		if (features[70] > 0)
			outScores[22] = ClampInt16(outScores[22] + 1900);
		else if (features[65] > (pHouse->OwnedUnits * 6 + 18))
			outScores[22] = ClampInt16(outScores[22] + 1100);
		else
			outScores[22] = ClampInt16(outScores[22] - 1000);

		if (features[136] > 0)
			outScores[22] = ClampInt16(outScores[22] + 2000); // Open a sea lane for our fleet
	}
	else
	{
		outScores[22] = -3000; // Immune Bridges ON: never waste ammo shooting bridges
	}

	// Expert 4 in charge: train the engineers and spies its work needs and back the bridge
	// planner's repair and demolition calls over the generic strength checks.
	if (activeExpert == 4)
	{
		int techValue = 0;
		int spyTargets = 0;
		bool bridgeRepair = false;
		bool bridgeDemolition = false;
		GetSpecOpsWork(pHouse, techValue, spyTargets, bridgeRepair, bridgeDemolition);

		if (techValue > 0 || bridgeRepair)
			outScores[18] = ClampInt16(outScores[18] + 300 + std::min(400, techValue * 60));

		if (techValue > 0)
			outScores[36] = ClampInt16(outScores[36] + 300);

		if (spyTargets > 0)
			outScores[19] = ClampInt16(outScores[19] + 250 + spyTargets * 80);

		if (bridgeRepair && features[18] > 0)
			outScores[21] = ClampInt16(outScores[21] + 700);

		if (bridgeDemolition && features[134] > 0)
			outScores[22] = ClampInt16(outScores[22] + 600);
	}

	outScores[24] = ClampInt16(outScores[24] + 800 + ((arch == 3 || arch == 4) ? 320 : 0)
		- std::clamp(static_cast<int>(outScores[39]) / 2, -450, 350));

	outScores[25] = ClampInt16(outScores[25] + ((features[64] > 20) ? 700 : 300));

	outScores[26] = ClampInt16(outScores[26] + 250 + (hasAirThreat ? 150 : 0) + std::clamp(airDeficit * 6, 0, 1000));

	// Infantry swarm -> anti-infantry vehicles and defenses.
	outScores[25] = ClampInt16(outScores[25] + counterBonus(foeInfantry, 900));
	// Armor columns -> anti-tank infantry and main battle tanks.
	outScores[17] = ClampInt16(outScores[17] + counterBonus(foeVehicles, 700));
	outScores[24] = ClampInt16(outScores[24] + counterBonus(foeVehicles, 500));
	// Long-range siege -> fast raiders and air strikes; static defenses only feed it.
	outScores[32] = ClampInt16(outScores[32] + counterBonus(foeArtillery, 650));
	outScores[14] = ClampInt16(outScores[14] - counterBonus(foeArtillery, 450));
	// Navy -> naval commitment, warships and shipyard.
	outScores[15] = ClampInt16(outScores[15] + counterBonus(foeNaval, 700));
	outScores[29] = ClampInt16(outScores[29] + counterBonus(foeNaval, 700));
	// Air force beyond the live air threat memory (e.g. seen earlier, now refueling).
	outScores[26] = ClampInt16(outScores[26] + counterBonus(foeAir, 300));
	// Turtle (many known defenses) -> siege artillery and heavy bombers.
	outScores[27] = ClampInt16(outScores[27] + std::clamp(foe.KnownDefenses * 60, 0, 900));
	outScores[33] = ClampInt16(outScores[33] + std::clamp(foe.KnownDefenses * 35, 0, 600));
	// Early pressure near our base (rush) -> ground defenses now.
	outScores[5] = ClampInt16(outScores[5] + std::clamp(foe.RushPressure * 3, 0, 900));
	outScores[14] = ClampInt16(outScores[14] + std::clamp(foe.RushPressure * 2, 0, 500));

	// Expert 2 picks the counter type itself (UpdateCounterComposition); here it asks for the
	// buildings those counters need: anti-air defenses and an airfield for fighters against an air
	// force, a shipyard against a navy (only placed where naval play is possible), and fewer static
	// defenses against artillery that outranges them.
	if (activeExpert == 2)
	{
		outScores[6] = ClampInt16(outScores[6] + counterBonus(foeAir, 700));
		outScores[8] = ClampInt16(outScores[8] + counterBonus(foeAir, 500));
		outScores[20] = ClampInt16(outScores[20] + counterBonus(foeAir, 400));
		outScores[26] = ClampInt16(outScores[26] + counterBonus(foeAir, 400));
		outScores[7] = ClampInt16(outScores[7] + counterBonus(foeNaval, 600));
		outScores[29] = ClampInt16(outScores[29] + counterBonus(foeNaval, 400));
		outScores[14] = ClampInt16(outScores[14] - counterBonus(foeArtillery, 300));
		outScores[80] = ClampInt16(outScores[80] + counterBonus(foeArtillery, 500));
	}

	outScores[27] = ClampInt16(outScores[27] + ((features[68] > 16 || arch == 4 || activeExpert == 7) ? 950 : 350));

	outScores[28] = ClampInt16(outScores[28]
		+ ((features[21] > 0) ? 1450 : ((arch == 1 && pHouse->OwnedInfantry >= 3) ? 650 : 150))
		+ std::clamp(static_cast<int>(outScores[53]) / 2, -250, 450));

	outScores[29] = ClampInt16(outScores[29] + 600 + std::clamp(static_cast<int>(outScores[15]) / 2, -200, 450));

	// Slot 30: Multi-Outpost Expansion MCV, Multi-ConYard Production Speed Boost
	// & Nomadic MCV (No hard cap on NumConYards: the neural network weighs
	// whether to build more MCVs for expansion, nomadic hopping, or
	// MultipleFactory building speed vs being moderate based on economy & biases)
	const bool hasUncolonizedIslandExpansion = (features[21] == 50 || features[21] >= 100);
	// ConYard cap from what there is to take, not a fixed 2: one more per free ore field on our
	// landmass (up to 3), one more with a big bank (money is no limit, building speed is), 4 when
	// an island waits to be colonized. Nomadic yards (slot 81) leapfrog past it anyway.
	const int freeOreFields = GetMacroState(pHouse).UncolonizedOreFields;
	const bool richBank = pHouse->Available_Money() >= 15000;
	const int maxConYardsForScore = std::clamp(
		std::max(hasUncolonizedIslandExpansion ? 4 : 2, 2 + std::min(3, freeOreFields) + (richBank ? 1 : 0)), 2, 6);
	const bool homeCongestedOrIslandTarget = (hasUncolonizedIslandExpansion
		|| (features[20] > 10
			&& (pHouse->Buildings.Count >= 20 || GetMacroState(pHouse).ConsecutivePlacementFailures > 0)));
	const bool urgentResourceOrIslandExpansion
		= (localOreDepleting && (features[20] > 6 || homeCongestedOrIslandTarget))
		|| (hasUncolonizedIslandExpansion && pHouse->NumConYards < 3);
	const int depleteDiscount = (features[19] < 20 && (features[20] > 6 || homeCongestedOrIslandTarget))
		? 850
		: ((features[19] < 28 || homeCongestedOrIslandTarget) ? 500 : 0);
	const int mcvReserveThreshold = std::clamp(1800 + (pHouse->NumConYards * (features[21] > 0 ? 550 : 850))
			- depleteDiscount - (static_cast<int>(outScores[35]) * 2) - (static_cast<int>(outScores[81]) * 2),
		1100, 4800);
	if (pHouse->NumConYards >= 1 && pHouse->NumConYards < maxConYardsForScore && pHouse->NumWarFactories > 0
		&& (urgentResourceOrIslandExpansion || pHouse->Available_Money() > mcvReserveThreshold)
		&& (features[20] > 6 || localOreDepleting || homeCongestedOrIslandTarget || outScores[35] > 80
			|| outScores[81] > 80 || outScores[30] > 160 || freeOreFields > 0)
		&& (features[19] < 32 || homeCongestedOrIslandTarget || pHouse->Available_Money() > mcvReserveThreshold + 1000
			|| outScores[30] > 220 || outScores[81] > 120 || (freeOreFields > 0 && richBank)))
	{
		const int perYardPenalty = richBank ? 180 : ((features[21] > 0) ? 280 : 380);
		outScores[30] = ClampInt16(outScores[30] + 1050 + (localOreDepleting ? 420 : 0)
			+ (homeCongestedOrIslandTarget ? 400 : 0) + (urgentResourceOrIslandExpansion ? 350 : 0)
			+ (freeOreFields > 0 ? 250 + 80 * std::min(3, freeOreFields) : 0) + (richBank ? 300 : 0)
			+ std::clamp(static_cast<int>(outScores[35]) + (static_cast<int>(outScores[81]) / 2), -400, 800)
			- ((pHouse->NumConYards - 1) * perYardPenalty));
	}
	else
	{
		outScores[30] = ClampInt16(outScores[30] - 1200);
	}

	// Slot 31: War Factory Harvester Boom (Dynamic Harvester-to-Refinery ratio
	// scaled by Ore/Gem abundance on map)
	const int refCount = std::max(1, pHouse->CountResourceDestinations);
	const bool richResources = (features[19] >= 32 || (features[19] + features[20]) >= 56);
	const bool scarceResources = (features[19] < 12 && features[20] < 18);
	const int desiredHarvesters = scarceResources ? refCount : std::min(10, refCount * (richResources ? 3 : 2));

	if (pHouse->CountResourceGatherers == 0)
		outScores[31] = ClampInt16(outScores[31] + 2600);
	else if (pHouse->CountResourceGatherers < desiredHarvesters)
		outScores[31] = ClampInt16(outScores[31] + (richResources ? 1650 : 1400));
	else
		outScores[31] = ClampInt16(outScores[31] - 1200);

	// Expert 6 & Expert 7 spatial specialization boosts
	if (activeExpert == 6)
	{
		outScores[1] = ClampInt16(outScores[1] + 650);

		outScores[3] = ClampInt16(outScores[3] + 350);

		outScores[5] = ClampInt16(outScores[5] + 520); // Base-crawling also fortifies outposts & resource chokes with defenses!

		outScores[38] = ClampInt16(outScores[38] + 320);

		outScores[31] = ClampInt16(outScores[31] + 300);
	}
	else if (activeExpert == 7)
	{
		outScores[5] = ClampInt16(outScores[5] + 850);

		outScores[6] = ClampInt16(outScores[6] + 520);

		outScores[38] = ClampInt16(outScores[38] + 480);

		outScores[27] = ClampInt16(outScores[27] + 400);
	}

	outScores[32] = ClampInt16(outScores[32] + ((arch == 6 || activeExpert == 8) ? 750 : 400));

	outScores[33] = ClampInt16(outScores[33] + ((arch == 6 || activeExpert == 8) ? 800 : 450));

	// New Real Tactical & Economic Hands (Slots 56..60):
	// Slot 56: Eco-Raid / Enemy Harvester Hunting & Own Harvester Defense
	outScores[56] = ClampInt16(outScores[56] + (features[78] * 8) + (features[31] * 10)
		+ ((arch == 1 || arch == 3 || activeExpert == 4 || activeExpert == 5) ? 280 : 60));

	// Slot 57: Siege, Urban Garrison Repair/Assault & Combat Deploy/Undeploy
	outScores[57] = ClampInt16(outScores[57] + (features[73] * 10) + (features[79] * 9) + (features[68] * 6)
		+ (features[76] * 6) + ((arch == 4 || activeExpert == 7) ? 320 : 80));

	outScores[27] = ClampInt16(outScores[27] + std::clamp(static_cast<int>(outScores[57]) / 3, -200, 450));

	if (features[79] > 0)
	{
		outScores[18] = ClampInt16(outScores[18] + 320); // Train Engineer to repair damaged urban strongholds!

		outScores[23] = ClampInt16(outScores[23] + 280); // Train Occupier infantry to hold repaired urban buildings!
	}

	// Slot 58: Tactical Kiting, Mind-Control Breaker, Tank Crush & Anti-SW
	// Scatter
	outScores[58] = ClampInt16(outScores[58] + (features[75] * 8) + (features[76] * 7) + (features[77] * 9)
		+ (features[22] * 8) + ((activeExpert == 5) ? 340 : 100));

	// Slot 59: Smart Wrench Building Repair & Map Crate Pickup
	outScores[59] = ClampInt16(
		outScores[59] + (features[27] * 14) + (features[27] > 0 ? 280 : 0) + (features[29] * 9)
		+ ((pHouse->Available_Money() > 1400) ? 240 : (features[27] > 0 ? 120 : -150)));

	// Slot 60: AA & Sensor Escort, LowPower Defense Toggle & Spawner Standoff
	outScores[60] = ClampInt16(outScores[60] + (features[80] * 10) + (features[66] * 8) + (features[72] * 9)
		+ (pHouse->HasLowPower() ? 360 : 80));

	outScores[26] = ClampInt16(outScores[26] + std::clamp(static_cast<int>(outScores[60]) / 3, -150, 380));

	// Slot 61: Dynamic Spy/Mirage Disguise (DisguiseAs) & Counter-Spy Detection
	// (DetectDisguise)
	outScores[61] = ClampInt16(
		outScores[61] + (features[81] * 10) + (features[72] * 8) + ((arch == 1 || activeExpert == 4) ? 320 : 90));

	// Slot 62: Slave Miner Liberation (Kill SlaveOwner -> Free Slaves) & Tesla
	// Coil Overpowering
	outScores[62]
		= ClampInt16(outScores[62] + (features[82] * 11) + (features[78] * 6) + (pHouse->HasLowPower() ? 280 : 90));

	// Slot 63: Desolator Rad-Field Chokepoint Deploy & Mobile Gap/Stealth Shroud
	// Escort
	outScores[63] = ClampInt16(outScores[63] + (features[83] * 10) + (features[64] * 7) + (features[80] * 6)
		+ ((activeExpert == 5 || activeExpert == 7) ? 260 : 80));

	// Slot 64: Parasite Warfare (Terror Drone/Squid), De-Parasiting
	// (Sonic/RemoveParasite/Repair) & Smart OpenTopped
	outScores[64] = ClampInt16(outScores[64] + (features[84] * 10) + (features[22] * 9)
		+ ((activeExpert == 4 || activeExpert == 5) ? 280 : 90));

	// Slot 65: Grinder Mind-Control Recycling & Emergency Salvage
	outScores[65] = ClampInt16(outScores[65] + (features[85] * 11) + (features[77] * 6)
		+ ((activeExpert == 0 || activeExpert == 4) ? 260 : 80));

	// Slot 66: EMP Pulse Targeting (Warhead->EMEffect) & EMP-Disabled Finisher
	// (IsUnderEMP)
	outScores[66] = ClampInt16(outScores[66] + (features[86] * 11) + (features[65] * 6)
		+ ((activeExpert == 2 || activeExpert == 5) ? 280 : 90));

	// Slot 67: Tank Bunker Garrisoning (Bunker/Bunkerable) & Manual Ammo Reload
	// (ManualReload)
	outScores[67] = ClampInt16(outScores[67] + (features[87] * 10) + (features[68] * 6)
		+ ((activeExpert == 1 || activeExpert == 7) ? 260 : 80));

	// Slot 68: Sniper/Poison Specialist Hunter, Chaos Gas (Psychedelic) &
	// TiberiumHeal Regeneration
	outScores[68] = ClampInt16(outScores[68] + (features[88] * 10) + (features[64] * 6)
		+ ((activeExpert == 2 || activeExpert == 4) ? 280 : 90));

	// Slot 69: Submarine Cloak Ambush & Anti-Sub Destroyer Escort (Naval
	// Cloakable & Sensors)
	outScores[69] = ClampInt16(
		outScores[69] + (features[89] * 10) + (features[67] * 8) + ((arch == 4 || activeExpert == 2) ? 260 : 80));

	// Slot 70: Dying Transport / Battle Fortress Emergency Passenger Unload
	outScores[70] = ClampInt16(outScores[70] + (features[90] * 12) + (features[30] * 6) + 140);

	// Slot 71: Chrono Legionnaire Temporal Erasure Breaker (TemporalTargetingMe
	// -> Kill Owner)
	outScores[71] = ClampInt16(outScores[71] + (features[91] * 12) + 160);

	// Slot 72: Crazy Ivan AttachedBomb Disarm or Kamikaze Charge into Enemy Ranks
	outScores[72] = ClampInt16(outScores[72] + (features[91] * 11) + (features[64] * 5) + 140);

	// Slot 73: Dynamic Tank Bunker Unload (UnloadBunker on Low HP or Offensive
	// Wave)
	outScores[73] = ClampInt16(outScores[73] + (features[92] * 10) + (features[87] * 6) + 120);

	// Slot 74: Anti-Drain Floating Disc Interceptor (DrainingMe -> Priority #1 AA
	// Target)
	outScores[74] = ClampInt16(outScores[74] + (features[92] * 12) + (features[71] * 8) + 150);

	// Slot 75: Naval War Multi-Transport D-Day Convoy, Shuttle Ferry & Island MCV
	// Beachhead Factory
	const int base75
		= (features[21] > 0) ? std::max(static_cast<int>(outScores[75]), 150) : static_cast<int>(outScores[75]);
	outScores[75] = ClampInt16(
		base75 + (features[93] * 11) + (features[21] * 10) + ((activeExpert == 4 || activeExpert == 6) ? 320 : 110));

	if (features[21] > 0 && pHouse->NumConYards >= 1
		&& (pHouse->Available_Money() > 1400 || localOreDepleting || pHouse->Buildings.Count >= 20))
	{
		outScores[30] = ClampInt16(std::max(static_cast<int>(outScores[30]), 250) + 680);
		outScores[28] = ClampInt16(std::max(static_cast<int>(outScores[28]), 200) + 650);
	}

	// Slot 76: Squad Focus-Fire on Wounded In-Range Enemy (Lanchester
	// Target-Fire)
	outScores[76]
		= ClampInt16(outScores[76] + (features[94] * 10) + (features[65] * 6) + ((activeExpert == 5) ? 300 : 120));

	// Slot 77: Wounded Frontline Tank Pull-Back Micro (Step Behind Healthy Tanks)
	outScores[77]
		= ClampInt16(outScores[77] + (features[94] * 9) + (features[30] * 8) + ((activeExpert == 5) ? 280 : 110));

	// Slot 78: High-Ground Cliff Advantage Positioning (CellClass::Level)
	outScores[78]
		= ClampInt16(outScores[78] + (features[80] * 8) + (features[68] * 6) + ((activeExpert == 7) ? 280 : 100));

	// Slot 79: Surgical Enemy Power, Superweapon & Factory Snipe inside Enemy
	// Base
	outScores[79] = ClampInt16(outScores[79] + (features[95] * 11) + (features[75] * 8)
		+ ((activeExpert == 4 || activeExpert == 5) ? 300 : 120));

	// Slot 80: Fast Flanker & Jumpjet Counter-Battery vs Enemy Artillery
	outScores[80] = ClampInt16(outScores[80] + (features[76] * 12) + (features[95] * 7)
		+ ((arch == 3 || activeExpert == 2 || activeExpert == 5) ? 300 : 110));

	// Slot 81: Nomadic MCV Hop, Outpost Drop & Forward Siege Turret-Push
	// (MCVRedeploy)
	outScores[81] = ClampInt16(outScores[81] + (features[16] * 10) + (features[20] * 7) + (features[68] * 5)
		+ ((activeExpert == 6 || activeExpert == 7) ? 320 : 100));

	// Slot 82: Enemy ConYard Heist, Stolen MCV Safe Extraction & Cross-Faction
	// Tech Tree
	outScores[82] = ClampInt16(outScores[82] + (features[17] * 11) + (features[95] * 6)
		+ ((arch == 1 || activeExpert == 1 || activeExpert == 4) ? 320 : 120));

	if (features[17] >= 40)
	{
		outScores[18] = ClampInt16(outScores[18] + 450); // Train Engineers to capture Enemy ConYard/Tech!

		outScores[4] = ClampInt16(outScores[4] + 400); // Climb newly captured Foreign Tech Tree!
	}

	// Slot 83: Mirage Tank & Camouflage Tree-Grove Ambush, Retreat When Spotted &
	// Ambush Zone Rotation
	outScores[83] = ClampInt16(outScores[83] + (features[98] * 9) + (features[78] * 6)
		+ ((arch == 1 || activeExpert == 4 || activeExpert == 5) ? 300 : 110));

	// Expert 8 (Airforce & Sky Dominance), Expert 9 (Urban Garrison & Allied
	// Coalition), and Archetypes 5..7 structural boosts & active routing/bias
	// slots (84..88)
	outScores[84] = ClampInt16(outScores[84] + (features[66] * 6) + (features[24] * 8)
		+ ((activeExpert == 8 || arch == 6) ? 340 : 90));
	outScores[85] = ClampInt16(outScores[85] + (features[26] * 8) + (features[96] * 6) + (features[97] * 7)
		+ ((activeExpert == 9 || arch == 7) ? 340 : 90));
	outScores[86] = ClampInt16(outScores[86] + (features[78] * 7) + (features[98] * 6) + ((arch == 5) ? 260 : 40));
	outScores[87] = ClampInt16(outScores[87] + (features[67] * 7) + (features[21] * 7) + ((arch == 6) ? 260 : 40));
	outScores[88] = ClampInt16(outScores[88] + (features[68] * 6) + (features[26] * 6) + ((arch == 7) ? 260 : 40));

	if (activeExpert == 8 || arch == 6)
	{
		outScores[8] = ClampInt16(outScores[8] + 620);
		outScores[32] = ClampInt16(outScores[32] + 550);
		outScores[33] = ClampInt16(outScores[33] + 600);
		outScores[74] = ClampInt16(outScores[74] + 240);

		if (arch == 6 && (features[67] > 10 || features[21] > 0))
		{
			outScores[7] = ClampInt16(outScores[7] + 550);
			outScores[29] = ClampInt16(outScores[29] + 450);
		}
	}

	if (activeExpert == 9 || arch == 7)
	{
		outScores[16] = ClampInt16(outScores[16] + 320);
		outScores[18] = ClampInt16(outScores[18] + 280);
		outScores[23] = ClampInt16(outScores[23] + 450);
		outScores[27] = ClampInt16(outScores[27] + 480);
		outScores[57] = ClampInt16(outScores[57] + 360);
	}

	if (arch == 5)
	{
		outScores[25] = ClampInt16(outScores[25] + 480);
		outScores[37] = ClampInt16(outScores[37] + 320);
		outScores[56] = ClampInt16(outScores[56] + 450);
		outScores[80] = ClampInt16(outScores[80] + 360);
		outScores[83] = ClampInt16(outScores[83] + 340);
	}

	// Slots 89..126 are the strategic plan preferences (NeuralPlanSlots, learned by ES only);
	// slot 127 is a FREE RESERVE SLOT kept at 0 until assigned
}


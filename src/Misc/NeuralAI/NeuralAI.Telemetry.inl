// NeuralAI.Telemetry.inl: Trainer telemetry snapshot (RAM only).
// Part of NeuralAI.cpp (included in order; one translation unit).

namespace
{
	// NeuralAI.Preservation.inl (included later).
	void FormatArmyTelemetry(const HouseClass* pHouse, char* pBuffer, size_t size);
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
					std::snprintf(planBuf, sizeof(planBuf), "%s\"%s\": [%u, %u, %u, %u, %u]", pl > 0 ? ", " : "", NeuralPlanNames[pl],
						st[0], st[1], st[2], st[3], st[4]);
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

			lineLen = std::snprintf(lineBuf, sizeof(lineBuf),
				"    {\"idx\": %d, \"side\": %d, \"country\": %d, \"archetype\": %d, \"opening\": %d, "
				"\"expert\": %d, \"credits\": %d, \"killedUnits\": %d, "
				"\"killedBlds\": %d, \"ownedUnits\": %d, \"ownedBlds\": "
				"%d, \"score\": %d, \"winner\": %u, \"defeated\": %u, \"policy\": "
				"%u, \"conYards\": %d, \"harvesters\": %d, \"refineries\": %d, "
				"\"warFactories\": %d, \"barracks\": %d, "
				"\"defenses\": %d, \"territory\": %d, \"territoryAvg\": %.1f, \"oreFields\": %d, \"oreFieldsAvg\": %.1f, "
				"\"bldValue\": %d, \"blds\": \"%s\", \"units\": \"%s\", \"airThreat\": %d, "
				"\"ownAA\": %d, \"aiMsAvg\": %.2f, \"aiMsMax\": %.2f, \"aiSections\": {%s}, \"aiPeaks\": [%s], \"expertTicks\": [%s], \"expertGain\": [%s], \"expertTicksByPhase\": [%s], \"expertGainByPhase\": [%s], \"enemyMix\": \"%s\", \"claim\": %s, \"econ\": [%s], \"army\": [%s], \"decisions\": {%s}, \"plans\": {%s}, \"gates\": {%s}}%s\n",
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
				(h.ArrayIndex >= 0 && h.ArrayIndex < 32) ? TickCosts[h.ArrayIndex].MaxMs : 0.0, sectionsText.c_str(), peaksText.c_str(), expertTicksText.c_str(), expertGainText.c_str(), expertTicksByPhaseText.c_str(), expertGainByPhaseText.c_str(), enemyMixText.c_str(), claimText, econText, armyText, decisionsText.c_str(), plansText.c_str(), gatesText.c_str(), (i + 1 < dst->NumHouses) ? "," : "");

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
		jsonBuf.append("],\n  \"radar\": [");

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


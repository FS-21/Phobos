#pragma once

#include <ScriptClass.h>
#include <ScriptTypeClass.h>
#include <TeamClass.h>
#include <AITriggerTypeClass.h>

#include <HouseClass.h>
#include <AircraftClass.h>
#include <MapClass.h>
#include <BulletClass.h>
#include <Helpers/Enumerators.h>
#include <WarheadTypeClass.h>
#include <SpawnManagerClass.h>

#include <Ext/Team/Body.h>
#include <Utilities/Container.h>
#include <Phobos.h>

class ScriptExt
{
public:
	using base_type = ScriptClass;

	class ExtData final : public Extension<ScriptClass>
	{
	public:
		// Nothing yet

		ExtData(ScriptClass* OwnerObject) : Extension<ScriptClass>(OwnerObject)
			// Nothing yet
		{ }

		virtual ~ExtData() = default;

		virtual void InvalidatePointer(void* ptr, bool bRemoved) override {}

		virtual void LoadFromStream(PhobosStreamReader& Stm);
		virtual void SaveToStream(PhobosStreamWriter& Stm);

	};

	class ExtContainer final : public Container<ScriptExt> {
	public:
		ExtContainer();
		~ExtContainer();
	};

	static void ProcessAction(TeamClass * pTeam);
	static void ExecuteTimedAreaGuardAction(TeamClass * pTeam);
	static void LoadIntoTransports(TeamClass * pTeam);
	static void WaitUntilFullAmmoAction(TeamClass * pTeam);
	static void Mission_Gather_NearTheLeader(TeamClass *pTeam, int countdown);
	static void Mission_Attack(TeamClass* pTeam, bool repeatAction, int calcThreatMode, int attackAITargetType, int IdxAITargetTypeItem);
	static TechnoClass* GreatestThreat(TechnoClass* pTechno, int method, int calcThreatMode, HouseClass* onlyTargetThisHouseEnemy, int attackAITargetType, int idxAITargetTypeItem, bool agentMode);
	static bool EvaluateObjectWithMask(TechnoClass* pTechno, int mask, int attackAITargetType, int idxAITargetTypeItem, TechnoClass *pTeamLeader);

	static void DecreaseCurrentTriggerWeight(TeamClass* pTeam, bool forceJumpLine, double modifier);
	static void IncreaseCurrentTriggerWeight(TeamClass* pTeam, bool forceJumpLine, double modifier);
	static void WaitIfNoTarget(TeamClass *pTeam, int attempts);
	static void TeamWeightReward(TeamClass *pTeam, double award);
	static void PickRandomScript(TeamClass * pTeam, int idxScriptsList);
	static void Mission_Move(TeamClass* pTeam, int calcThreatMode, bool pickAllies, int attackAITargetType, int idxAITargetTypeItem);
	static TechnoClass* FindBestObject(TechnoClass *pTechno, int method, int calcThreatMode, bool pickAllies, int attackAITargetType, int idxAITargetTypeItem);
	static void UnregisterGreatSuccess(TeamClass * pTeam);

	static void Mission_Attack_List(TeamClass *pTeam, bool repeatAction, int calcThreatMode, int attackAITargetType);

	static void ResetAngerAgainstHouses(TeamClass* pTeam);
	static void SetHouseAngerModifier(TeamClass* pTeam, int modifier);
	static void ModifyHateHouses_List(TeamClass* pTeam, int idxHousesList);
	static void ModifyHateHouses_List1Random(TeamClass* pTeam, int idxHousesList);
	static void ModifyHateHouse_Index(TeamClass* pTeam, int idxHouse);
	static void SetTheMostHatedHouse(TeamClass* pTeam, int mask, int mode, bool random);
	static void OverrideOnlyTargetHouseEnemy(TeamClass* pTeam, int mode);
	static void AggroHouse(TeamClass* pTeam, int index);

	static ExtContainer ExtMap;

private:
	static void ModifyCurrentTriggerWeight(TeamClass* pTeam, bool forceJumpLine, double modifier);
	static HouseClass* GetTheMostHatedHouse(TeamClass* pTeam, int mask, int mode);
	static void UpdateEnemyHouseIndex(HouseClass* pHouse);
};

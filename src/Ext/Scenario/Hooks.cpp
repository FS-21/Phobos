#include <Ext/Scenario/Body.h>
#include <Ext/Techno/Body.h>
#include <Helpers/Macro.h>
#include <Utilities/Debug.h>

DEFINE_HOOK(0x6870D7, ReadScenario_LoadingScreens, 0x5)
{
	enum { SkipGameCode = 0x6873AB };

	LEA_STACK(CCINIClass*, pINI, STACK_OFFSET(0x174, -0x158));

	auto const pScenario = ScenarioClass::Instance.get();
	auto const scenarioName = pScenario->FileName;
	auto const defaultsSection = "Defaults";

	pScenario->LS640BriefLocX = pINI->ReadInteger(scenarioName, "LS640BriefLocX", pINI->ReadInteger(defaultsSection, "DefaultLS640BriefLocX", 0));
	pScenario->LS640BriefLocY = pINI->ReadInteger(scenarioName, "LS640BriefLocY", pINI->ReadInteger(defaultsSection, "DefaultLS640BriefLocY", 0));
	pScenario->LS800BriefLocX = pINI->ReadInteger(scenarioName, "LS800BriefLocX", pINI->ReadInteger(defaultsSection, "DefaultLS800BriefLocX", 0));
	pScenario->LS800BriefLocY = pINI->ReadInteger(scenarioName, "LS800BriefLocY", pINI->ReadInteger(defaultsSection, "DefaultLS800BriefLocY", 0));
	pINI->ReadString(defaultsSection, "DefaultLS640BkgdName", pScenario->LS640BkgdName, pScenario->LS640BkgdName, 64);
	pINI->ReadString(scenarioName, "LS640BkgdName", pScenario->LS640BkgdName, pScenario->LS640BkgdName, 64);
	pINI->ReadString(defaultsSection, "DefaultLS800BkgdName", pScenario->LS800BkgdName, pScenario->LS800BkgdName, 64);
	pINI->ReadString(scenarioName, "LS800BkgdName", pScenario->LS800BkgdName, pScenario->LS800BkgdName, 64);
	pINI->ReadString(defaultsSection, "DefaultLS800BkgdPal", pScenario->LS800BkgdPal, pScenario->LS800BkgdPal, 64);
	pINI->ReadString(scenarioName, "LS800BkgdPal", pScenario->LS800BkgdPal, pScenario->LS800BkgdPal, 64);

	return SkipGameCode;
}

// Note: same address used by the Ares hook "LogicClass_Update"
//DEFINE_HOOK(0x55AFB3, LogicClass_AI_UniversalDeploy, 0x6)
DEFINE_HOOK(0x55DCA3, LogicClass_AI_UniversalDeploy, 0x5)

{
	auto& vec = ScenarioExt::Global()->UniversalDeployers;
	int originalSize = vec.size();

	for (int i = 0; i < vec.size(); i++)
	{
		//TechnoExt::UpdateUniversalDeploy(vec.at(i));

		auto pOld = vec.at(i);
		// Remove techno from list of UniversalDeploy in progress
		vec.erase(std::remove(vec.begin(), vec.end(), pOld), vec.end());

		pOld->Limbo();
		pOld->Owner->Buildings.Remove(static_cast<BuildingClass*>(pOld));
		pOld->Owner->RemoveTracking(pOld);
		pOld->Owner->RegisterLoss(pOld, false);
		pOld->Owner->UpdatePower();
		pOld->Owner->RecheckTechTree = true;
		pOld->Owner->RecheckPower = true;
		pOld->Owner->RecheckRadar = true;
		//TechnoClass::Array->Remove(pOld);
		pOld->UnInit();
		break;
		int currentSize = vec.size();
		if (currentSize < originalSize)
			i--;
	}

	/*for (auto pTechno : ScenarioExt::Global()->UniversalDeployers)
	{
		TechnoExt::UpdateUniversalDeploy(pTechno);
	}*/



	return 0;
}

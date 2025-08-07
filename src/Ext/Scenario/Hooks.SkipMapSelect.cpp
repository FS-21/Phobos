#include "Body.h"

#include <Ext/Scenario/Body.h>
#include <Utilities/Debug.h>

// Save the file name specified in these NextScenario variables as the next file to be readed or we'll get the "Unable to load scenario" error
DEFINE_HOOK(0x6859AB, ScenarioClass_DoWin_SkipMapSelect_PreFix, 0x6)
{
	ScenarioClass* pThis = ScenarioClass::Instance;

	if (!pThis->SkipMapSelect)
		return 0;

	auto const GlobalVariables = ScenarioExt::Global()->Variables[true];

	// Find which map is the next one to be loaded
	if (GlobalVariables.contains(1) && GlobalVariables.find(1)->second.Value > 0)
	{
		if (strcmp(pThis->AltNextScenario, "") == 0)
			return 0;

		CCFileClass pFile(pThis->AltNextScenario);
		if (!pFile.Exists() || !pFile.Open(FileAccessMode::Read))
		{
			Debug::Log("SkipMapSelect - Error loading map AltNextScenario=%s\n", pThis->AltNextScenario);
			return 0;
		}

		strcpy_s(pThis->FileName, pThis->AltNextScenario);
		strcpy_s(ScenarioExt::Global()->ScenarioFileName, pThis->AltNextScenario);
	}
	else
	{
		if (strcmp(pThis->NextScenario, "") == 0)
			return 0;

		CCFileClass pFile(pThis->NextScenario);
		if (!pFile.Exists() || !pFile.Open(FileAccessMode::Read))
		{
			Debug::Log("SkipMapSelect - Error loading map NextScenario=%s\n", pThis->NextScenario);
			return 0;
		}

		strcpy_s(pThis->FileName, pThis->NextScenario);
		strcpy_s(ScenarioExt::Global()->ScenarioFileName, pThis->NextScenario);
	}

	//pThis->ReadScenarioINI(ScenarioExt::Global()->ScenarioFileName);
	//--pThis->TechLevel;

	return 0;
}

// Skip this SkipMapSelect code block that only produces the "Unable to load scenario" error message
DEFINE_HOOK(0x685A42, ScenarioClass_DoWin_SkipMapSelect_Fix, 0x6)
{
	enum { SkipGameCode = 0x685A84 };

	if (strcmp(ScenarioExt::Global()->ScenarioFileName, "") == 0)
		return 0;

	return SkipGameCode;
}

// Scenario FileName variable is cleaned at some point so it must be reloaded
DEFINE_HOOK(0x686C36, ScenarioClass_ReadScenarioINI_SkipMapSelect_ScenarioFileName, 0x6)
{
	ScenarioClass* pThis = ScenarioClass::Instance;

	if (strcmp(pThis->FileName, "") != 0 && strcmp(ScenarioExt::Global()->ScenarioFileName, "") == 0)
	{
		strcpy_s(ScenarioExt::Global()->ScenarioFileName, pThis->FileName);
		return 0;
	}

	if (strcmp(ScenarioExt::Global()->ScenarioFileName, "") == 0)
		return 0;

	// Update scenario file name
	//pThis->ReadScenarioINI(ScenarioExt::Global()->ScenarioFileName); // Infinite loop
	strcpy_s(pThis->FileName, ScenarioExt::Global()->ScenarioFileName);

	return 0;
}

/*
DEFINE_HOOK(0x683D21, ScenarioClass_ReadScenarioINI_SkipMapSelect_ScenarioFileName_000, 0x5)
{
	GET(char*, root, EBP);
	GET(char*, root2, ECX);

	ScenarioClass* pThis = ScenarioClass::Instance;

	R->EBP(pThis->FileName);
	R->ECX(pThis->FileName);

	return 0;
}*/


/*DEFINE_HOOK(0x684630, ScenarioClass_ReadScenarioINI_SkipMapSelect_ScenarioFileName_0000000, 0x5)
{
	//REF_STACK(float, amount, 0x1C);
	GET(char*, root, EBP);
	GET(char*, root2, ECX);

	ScenarioClass* pThis = ScenarioClass::Instance;

	//R->EBP(pThis->FileName);
	//R->ECX(pThis->FileName);

	return 0;
}*/

/*DEFINE_HOOK(0x683D02, ScenarioClass_ReadScenarioINI_SkipMapSelect_ScenarioFileName_0000000, 0x8)
{
	//REF_STACK(float, amount, 0x1C);
	//GET(char*, root, EBP);
	//GET(char*, root2, ECX);
	GET(char*, v3, EBP);

	ScenarioClass* pThis = ScenarioClass::Instance;

	//R->EBP(pThis->FileName);
	//R->ECX(pThis->FileName);

	return 0;
}*/

/*DEFINE_HOOK(0x683AB8, ScenarioClass_StartScenario_ScenarioFileName, 0x8)
{
	//REF_STACK(float, amount, 0x1C);
	//GET(char*, root, EBP);
	GET(char*, root, ECX);
	GET(char*, v3, EBP);

	ScenarioClass* pThis = ScenarioClass::Instance;

	//R->EBP(pThis->FileName);
	//R->ECX(pThis->FileName);

	return 0;
}*/

/*DEFINE_HOOK(0x686D7F, ScenarioClass_ReadScenarioINI_ScenarioFileName00000, 0x6)
{
	ScenarioClass* pThis = ScenarioClass::Instance;
	GET(char*, root, ECX);
	GET(char*, root2, EDX);
	GET(char*, root3, EAX);


	return 0;
}*/

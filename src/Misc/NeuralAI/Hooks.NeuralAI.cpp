#include "NeuralAI.h"

#include <AircraftClass.h>
#include <BuildingClass.h>
#include <BuildingTypeClass.h>
#include <HouseClass.h>
#include <MapClass.h>
#include <ScenarioClass.h>
#include <SessionClass.h>
#include <UnitClass.h>
#include <Ext/House/Body.h>
#include <Ext/Rules/Body.h>
#include <Utilities/Macro.h>
#include <cstring>

namespace
{
	using SetFocus_t = HWND(WINAPI*)(HWND);

	using SetForegroundWindow_t = BOOL(WINAPI*)(HWND);

	HWND WINAPI Headless_NoOp_SetFocus(HWND hWnd)
	{
		return hWnd;
	}

	BOOL WINAPI Headless_NoOp_SetForegroundWindow(HWND)
	{
		return TRUE;
	}

	void PatchModuleFocusIAT(HMODULE hMod)
	{
		if (!hMod)
			return;

		auto* const base = reinterpret_cast<BYTE*>(hMod);
		auto* const dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);

		if (dos->e_magic != IMAGE_DOS_SIGNATURE)
			return;

		auto* const nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);

		if (nt->Signature != IMAGE_NT_SIGNATURE)
			return;

		const auto& impDir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];

		if (!impDir.VirtualAddress || !impDir.Size)
			return;

		for (auto* desc = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + impDir.VirtualAddress); desc->Name != 0;
			++desc)
		{
			const char* const dllName = reinterpret_cast<const char*>(base + desc->Name);

			if (_stricmp(dllName, "user32.dll") != 0)
				continue;

			if (!desc->OriginalFirstThunk || !desc->FirstThunk)
				continue;

			auto* origThunk = reinterpret_cast<IMAGE_THUNK_DATA*>(base + desc->OriginalFirstThunk);
			auto* iatThunk = reinterpret_cast<IMAGE_THUNK_DATA*>(base + desc->FirstThunk);

			for (; origThunk->u1.AddressOfData != 0; ++origThunk, ++iatThunk)
			{
				if (IMAGE_SNAP_BY_ORDINAL(origThunk->u1.Ordinal))
					continue;

				auto* const byName = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + origThunk->u1.AddressOfData);

				const char* const fnName = reinterpret_cast<const char*>(byName->Name);

				void* replacement = nullptr;

				if (std::strcmp(fnName, "SetFocus") == 0)
					replacement = reinterpret_cast<void*>(&Headless_NoOp_SetFocus);
				else if (std::strcmp(fnName, "SetForegroundWindow") == 0)
					replacement = reinterpret_cast<void*>(&Headless_NoOp_SetForegroundWindow);

				if (replacement)
				{
					DWORD oldProt = 0;

					if (VirtualProtect(&iatThunk->u1.Function, sizeof(ULONG_PTR), PAGE_READWRITE, &oldProt))
					{
						iatThunk->u1.Function = reinterpret_cast<ULONG_PTR>(replacement);

						VirtualProtect(&iatThunk->u1.Function, sizeof(ULONG_PTR), oldProt, &oldProt);
					}
				}
			}
		}
	}

	void ApplyHeadlessStealthWindow(HWND hWnd)
	{
		if (!hWnd || !IsWindow(hWnd))
			return;

		const LONG exStyle = GetWindowLongA(hWnd, GWL_EXSTYLE);

		const LONG targetEx
			= (exStyle | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_LAYERED | WS_EX_TRANSPARENT) & ~WS_EX_APPWINDOW;

		if (exStyle != targetEx)
			SetWindowLongA(hWnd, GWL_EXSTYLE, targetEx);

		SetLayeredWindowAttributes(hWnd, 0, 0, LWA_ALPHA);

		SetWindowPos(hWnd, HWND_BOTTOM, 32000, 32000, 0, 0,
			SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_NOSENDCHANGING);
	}
} // namespace

// Scenario start initialization
DEFINE_HOOK(0x6879ED, ScenarioClass_Start_NeuralAI, 0x5)
{
	if (NeuralAI::HeadlessMode && Game::hWnd)
	{
		ReleaseCapture();

		ApplyHeadlessStealthWindow(Game::hWnd);
	}

	NeuralAI::OnScenarioStart();

	return 0;
}

// The engine's path finder gave up on a way for a unit (the "Regular findpath failure" line of the
// debug log, written just after this point).
DEFINE_HOOK(0x42CC48, AStarClass_FindPath_Failure_NeuralAI, 0x5)
{
	GET(FootClass*, pFoot, ESI);
	GET_STACK(CellStruct, to, STACK_OFFSET(0x30, -0x20));
	GET_STACK(CellStruct, from, STACK_OFFSET(0x30, -0x1C));

	NeuralAI::OnPathFailure(pFoot, from, to);

	return 0;
}

// Scenario victory & defeat hooks
DEFINE_HOOK(0x6856A5, Do_Win_NeuralAI, 0x7)
{
	NeuralAI::OnScenarioEnd(true);

	return 0;
}

DEFINE_HOOK(0x685DE7, Do_Lose_NeuralAI, 0x5)
{
	NeuralAI::OnScenarioEnd(false);

	return 0;
}

// Headless Turbo Training Mode (-TRAIN_HEADLESS) multi-instance mutex, window & Main_Loop hooks
DEFINE_HOOK(0x6BBE56, WinMain_SkipSingleInstanceMutex_NeuralAI, 0x5)
{
	enum
	{
		ContinueWinMain = 0x6BBFAD
	};

	const char* const cmdLine = GetCommandLineA();

	if (NeuralAI::HeadlessMode
		|| (cmdLine && (std::strstr(cmdLine, "-SPAWN") != nullptr || std::strstr(cmdLine, "-spawn") != nullptr)))
	{
		*reinterpret_cast<HANDLE*>(0xB0BCE4) = NULL;

		*reinterpret_cast<HANDLE*>(0xB0BCE8) = NULL;

		return ContinueWinMain;
	}

	return 0;
}

DEFINE_HOOK(0x777C30, Create_Main_Window_Entry_NeuralAI, 0x6)
{
	if (NeuralAI::HeadlessMode)
	{
		PatchModuleFocusIAT(GetModuleHandleA(nullptr));

		PatchModuleFocusIAT(GetModuleHandleA("ddraw.dll"));
	}

	return 0;
}

DEFINE_HOOK(0x777DA1, Create_Main_Window_ShowWindow_NeuralAI, 0x5)
{
	enum
	{
		SkipShowAndFocus = 0x777DCF
	};

	if (NeuralAI::HeadlessMode)
	{
		if (Game::hWnd)
		{
			ApplyHeadlessStealthWindow(Game::hWnd);

			ShowWindow(Game::hWnd, SW_SHOWNOACTIVATE);

			ApplyHeadlessStealthWindow(Game::hWnd);
		}

		Game::IsFocused = true;

		return SkipShowAndFocus;
	}

	return 0;
}

DEFINE_HOOK(0x4A44D2, Set_Video_Mode_Done_NeuralAI, 0x5)
{
	if (NeuralAI::HeadlessMode && Game::hWnd)
	{
		ReleaseCapture();

		ApplyHeadlessStealthWindow(Game::hWnd);

		Game::IsFocused = true;
	}

	return 0;
}

DEFINE_HOOK(0x7778A0, Main_Window_Proc_ActivateApp_NeuralAI, 0x6)
{
	enum
	{
		IgnoreFocusChange = 0x777949
	};

	if (NeuralAI::HeadlessMode)
	{
		Game::IsFocused = true;

		return IgnoreFocusChange;
	}

	return 0;
}

DEFINE_HOOK(0x55D377, Main_Loop_FocusOverride_NeuralAI, 0x5)
{
	if (NeuralAI::HeadlessMode)
	{
		if (Game::hWnd)
		{
			LONG exStyle = GetWindowLongA(Game::hWnd, GWL_EXSTYLE);

			if ((exStyle & WS_EX_LAYERED) == 0)
			{
				ReleaseCapture();

				ApplyHeadlessStealthWindow(Game::hWnd);
			}
		}

		Game::IsFocused = true;

		R->AL(1);
	}

	return 0;
}

DEFINE_HOOK(0x55D8ED, Main_Loop_SkipRender_NeuralAI, 0x5)
{
	enum
	{
		SkipRender = 0x55D8F7
	};

	if (NeuralAI::HeadlessMode)
		return SkipRender;

	return 0;
}

DEFINE_HOOK(0x55DE9A, Main_Loop_SkipSyncDelay_NeuralAI, 0x5)
{
	enum
	{
		SkipSyncDelay = 0x55DE9F
	};

	if (NeuralAI::HeadlessMode)
	{
		NeuralAI::OnHeadlessFrameTick();

		return SkipSyncDelay;
	}

	return 0;
}

// Map Action 30 (Auto Base Building) enable/disable hooks for Campaign AI
DEFINE_HOOK_AGAIN(0x6DE21B, TActionClass_AutoBaseBuilding_Enable_NeuralAI, 0x7)

DEFINE_HOOK(0x6E0F02, TActionClass_AutoBaseBuilding_Enable_NeuralAI, 0x7)
{
	GET(HouseClass* const, pHouse, ESI);

	NeuralAI::OnAutoBaseBuildingTriggered(pHouse, true);

	return 0;
}

DEFINE_HOOK_AGAIN(0x6DE29F, TActionClass_AutoBaseBuilding_Disable_NeuralAI, 0x7)

DEFINE_HOOK(0x6E0F7F, TActionClass_AutoBaseBuilding_Disable_NeuralAI, 0x7)
{
	GET(HouseClass* const, pHouse, EAX);

	NeuralAI::OnAutoBaseBuildingTriggered(pHouse, false);

	return 0;
}

// Bypass legacy aimd.ini AITriggerTypes / TeamTypes / ScriptTypes in Skirmish & Multiplayer
DEFINE_HOOK(0x4F8A27, HouseClass_AI_SkipLegacyTeams_NeuralAI, 0x5)
{
	enum
	{
		SkipLegacyAITriggers = 0x4F8B08
	};

	GET(HouseClass* const, pHouse, ESI);

	if (NeuralAI::IsEnabledForHouse(pHouse) && !NeuralAI::IsCampaignRestricted(pHouse))
		return SkipLegacyAITriggers;

	return 0;
}

// HouseClass::Expert_AI evaluation hook after EnemyHouseIndex update, bypassing legacy SuperWeapon_AI, AI_Raise_Money &
// Fire_Sale
DEFINE_HOOK(0x4FD77C, HouseClass_ExpertAI_NeuralAI, 0x5)
{
	enum
	{
		ReturnWithCustomDelay = 0x4FD927
	};

	GET(HouseClass* const, pHouse, EBX);

	if (pHouse && (pHouse->Defeated || pHouse->IsObserver()))
	{
		R->EAX(100 - 0x69);
		return ReturnWithCustomDelay;
	}

	if (NeuralAI::IsEnabledForHouse(pHouse))
	{
		if (!NeuralAI::HeadlessMode && Unsorted::CurrentFrame >= 1)
		{
			if (auto const pCur = HouseClass::CurrentPlayer)
			{
				if (HouseClass::IsCurrentPlayerObserver() || Game::ObserverMode || pCur->IsInitiallyObserver()
					|| (pCur->Type && !_stricmp(pCur->Type->ID, "Observer")))
				{
					static int lastObserverRevealUniqueID = -1;
					const int curUniqueID = ScenarioClass::Instance ? ScenarioClass::Instance->UniqueID : 0;
					pCur->MakeObserver();
					if (lastObserverRevealUniqueID != curUniqueID)
					{
						lastObserverRevealUniqueID = curUniqueID;
						// MapClass::Reveal (0x577D90) checks `if (CurrentPlayer->Visionary) return;` at 0x577E09,
						// reveals all map cells, and then calls CreateAllGaps (0x4ADCD0) which clears Visionary = 0
						// at 0x6FB43F. Only run Reveal once per scenario and restore Visionary = 1 afterwards!
						pCur->Visionary = 0;
						MapClass::Instance.Reveal(pCur);
						pCur->Visionary = 1;
					}
				}
			}
		}

		const int delay = NeuralAI::UpdateExpertAI(pHouse);

		R->EAX(delay - 0x69);

		return ReturnWithCustomDelay;
	}

	return 0;
}

// HouseClass::AI_Building production hook
DEFINE_HOOK(0x4FE3E9, HouseClass_AI_Building_NeuralAI, 0x7)
{
	enum
	{
		Epilogue = 0x4FEA48
	};

	GET(HouseClass* const, pHouse, EBP);

	if (NeuralAI::IsEnabledForHouse(pHouse))
	{
		NeuralAI::UpdateBuildingProduction(pHouse);

		return Epilogue;
	}

	return 0;
}

// HouseClass::AI_Infantry production hook
DEFINE_HOOK(0x4FEEED, HouseClass_AI_Infantry_NeuralAI, 0x7)
{
	enum
	{
		Epilogue = 0x4FEEF6
	};

	GET(HouseClass* const, pHouse, EBP);

	if (NeuralAI::IsEnabledForHouse(pHouse))
	{
		NeuralAI::UpdateInfantryProduction(pHouse);

		return Epilogue;
	}

	return 0;
}

// HouseClass::AI_Aircraft production hook
DEFINE_HOOK(0x4FF21D, HouseClass_AI_Aircraft_NeuralAI, 0x7)
{
	enum
	{
		Epilogue = 0x4FF226
	};

	GET(HouseClass* const, pHouse, EBP);

	if (NeuralAI::IsEnabledForHouse(pHouse))
	{
		NeuralAI::UpdateAircraftProduction(pHouse);

		return Epilogue;
	}

	return 0;
}

// Reconstructed function epilogue for BuildingClass::Exit_Object when returning custom status
void NAKED BuildingClass_Exit_Object_Seek_Building_Position_Epilogue_NeuralAI()
{
	_asm
		{
		pop edi
		pop esi
		pop ebp
		pop ebx
		add esp, 130h
		retn 8
		}
}

// Spatial Tetris Base Architect & Surgical Space Clearance placement hook in BuildingClass::Exit_Object
DEFINE_HOOK(0x444F39, BuildingClass_ExitObject_PlaceBuilding_NeuralAI, 0x6)
{
	enum
	{
		ApplyCustomCoords = 0x4451AA
	};

	GET(BuildingClass* const, pFactory, ESI);

	GET(BuildingClass* const, pBuilding, EDI);

	auto const pHouse = pFactory ? pFactory->Owner : (pBuilding ? pBuilding->Owner : nullptr);

	if (!pHouse || !pBuilding || !pBuilding->Type || !NeuralAI::IsEnabledForHouse(pHouse))
		return 0;

	if (NeuralAI::IsCampaignRestricted(pHouse) || pBuilding->Type->PowersUpBuilding[0] != '\0')
		return 0;

	CellStruct bestCell = CellStruct::Empty;
	const int placeStatus = NeuralAI::PlaceCompletedBuilding(pFactory, pBuilding, bestCell);

	if (placeStatus == 1)
	{
		// Keep building waiting in Construction Yard while obstructing structure sells or units scatter
		R->EAX(1);

		return reinterpret_cast<intptr_t>(&BuildingClass_Exit_Object_Seek_Building_Position_Epilogue_NeuralAI);
	}

	if (placeStatus != 2 || bestCell == CellStruct::Empty)
	{
		// Never fall through to vanilla 0x444F39 placement (which glues buildings chaotically); return 0 in EAX to
		// cancel/refund
		pHouse->ProducingBuildingTypeIndex = -1;

		R->EAX(0);

		return reinterpret_cast<intptr_t>(&BuildingClass_Exit_Object_Seek_Building_Position_Epilogue_NeuralAI);
	}

	pHouse->ProducingBuildingTypeIndex = -1;

	const int coordX = (static_cast<int>(bestCell.X) << 8) + 128;
	const int coordY = (static_cast<int>(bestCell.Y) << 8) + 128;

	R->EAX(coordX);

	R->EBP(coordY);

	R->EBX(0);

	R->Stack<int>(0x14, 0);

	R->Stack<int>(0x1C, reinterpret_cast<int>(pBuilding->Type));

	R->Stack<int>(0x2C, coordX);

	R->Stack<int>(0x30, coordY);

	R->Stack<int>(0x34, 0);

	return ApplyCustomCoords;
}

// TechnoClass::Base_Is_Attacked sends the nearest units of a computer house at whoever hit its base
// (Mission::Rescue or Area_Guard, whatever they are: artillery and anti-air into melee) and
// suspends its teams. The Neural AI answers base threats itself (Base Threat Intercept) and moves
// the units of its houses in campaigns too (there only the buildings follow the scenario), so the
// engine response is skipped for all of them.
void NAKED TechnoClass_BaseIsAttacked_Return_NeuralAI()
{
	_asm
		{
		retn 4
		}
}

DEFINE_HOOK(0x708080, TechnoClass_BaseIsAttacked_NeuralAI, 0x6)
{
	GET(TechnoClass* const, pThis, ECX);

	if (NeuralAI::IsEnabledForHouse(pThis ? pThis->Owner : nullptr))
		return reinterpret_cast<DWORD>(&TechnoClass_BaseIsAttacked_Return_NeuralAI);

	return 0;
}

// BuildingClass::Repair_AI sells a computer house's building in the red when its money is below
// the repair threshold (IQSellBack, at random). The Neural AI decides its own sales (broke mode,
// factory caps, obsolete refineries), so that branch is skipped for its houses; the repair branch
// stays, and so does the sale in campaigns, where the buildings follow the scenario.
DEFINE_HOOK(0x450781, BuildingClass_RepairAI_SkipSellBack_NeuralAI, 0x5)
{
	enum
	{
		SkipSellBack = 0x450813
	};

	GET(BuildingClass* const, pThis, ESI);

	auto const pHouse = pThis ? pThis->Owner : nullptr;

	if (NeuralAI::IsEnabledForHouse(pHouse) && !NeuralAI::IsCampaignRestricted(pHouse))
		return SkipSellBack;

	return 0;
}

// UnitClass::AI sends any damaged vehicle of a computer house that idles in Guard or Area_Guard to a
// repair station every 16 frames, even at 99% health. The Neural AI plans its repair trips itself
// (thresholds, reserves, broke mode), so that branch is skipped for its houses.
DEFINE_HOOK(0x736740, UnitClass_AI_SkipAutoRepair_NeuralAI, 0x6)
{
	enum
	{
		SkipAutoRepair = 0x73681D
	};

	GET(UnitClass* const, pThis, ESI);

	if (NeuralAI::IsEnabledForHouse(pThis->Owner))
		return SkipAutoRepair;

	return 0;
}

// UnitClass::AI puts an MCV of a computer house that has no ConYard in Mission::Hunt (find a spot
// and deploy), and UnitClass::Mission_Guard deploys an idle one where it stands. The Neural AI
// decides where its MCVs go and deploy (expansions, islands, ferries), so both are skipped for its
// houses; in campaigns that follow the scenario base they stay.
DEFINE_HOOK(0x736414, UnitClass_AI_SkipMcvHunt_NeuralAI, 0x6)
{
	enum
	{
		SkipMcvHunt = 0x736461
	};

	GET(UnitClass* const, pThis, ESI);

	if (NeuralAI::IsEnabledForHouse(pThis->Owner) && !NeuralAI::IsCampaignRestricted(pThis->Owner))
		return SkipMcvHunt;

	return 0;
}

DEFINE_HOOK(0x7409F8, UnitClass_MissionGuard_SkipMcvDeploy_NeuralAI, 0x6)
{
	enum
	{
		SkipMcvDeploy = 0x740A50
	};

	GET(UnitClass* const, pThis, ESI);

	if (NeuralAI::IsEnabledForHouse(pThis->Owner) && !NeuralAI::IsCampaignRestricted(pThis->Owner))
		return SkipMcvDeploy;

	return 0;
}

// AircraftClass::Mission_Guard sends an idle armed aircraft of a computer house at the house's
// "juicy target" (HouseClass::Find_Juicy_Target) on its own. The Neural AI air experts pick strikes
// by damage, safe lanes and escorts, so that branch is skipped for its houses.
DEFINE_HOOK(0x41A882, AircraftClass_MissionGuard_SkipJuicyTarget_NeuralAI, 0x6)
{
	enum
	{
		SkipJuicyTarget = 0x41A8DC
	};

	GET(AircraftClass* const, pThis, ESI);

	if (NeuralAI::IsEnabledForHouse(pThis->Owner))
		return SkipJuicyTarget;

	return 0;
}

// Track directional building attacks for NeuralAI reactive defense placement and threat moderation
DEFINE_HOOK(0x44242A, BuildingClass_ReceiveDamage_TrackThreat_NeuralAI, 0x8)
{
	GET(BuildingClass* const, pThis, ESI);

	GET(const DamageState, state, EAX);

	GET(TechnoClass* const, pAttacker, EBP);

	GET_STACK(HouseClass* const, pAttackerHouse, STACK_OFFSET(0x9C, 0x1C));

	auto const pFromHouse = pAttacker ? pAttacker->GetOwningHouse() : pAttackerHouse;

	if (pThis && pThis->Owner && pFromHouse && state != DamageState::Unaffected && !pThis->IsStrange()
		&& !pThis->Owner->IsAlliedWith(pFromHouse))
	{
		NeuralAI::OnBuildingDamaged(pThis, pAttacker, pFromHouse);
	}

	return 0;
}

namespace
{
	inline bool IsLocalObserverPlayer()
	{
		if (HouseClass::IsCurrentPlayerObserver() || Game::ObserverMode)
			return true;

		if (auto const pCur = HouseClass::CurrentPlayer)
		{
			if (pCur->IsInitiallyObserver() || (pCur->Type && !_stricmp(pCur->Type->ID, "Observer")))
				return true;
		}

		return false;
	}
} // namespace

// Observer MapVisibility & Gap Generator Immunity:
// MapVisibility options (such as "Eternal darkness" / CoverMapTrigger Action 51 ReshroudMap and 127-cell GapGenerator
// buildings) must only affect actual players, never Spectators/Observers.
DEFINE_HOOK(0x50BD10, HouseClass_ReshroudMap_ObserverImmunity, 0x6)
{
	enum
	{
		SkipReshroudMap = 0x50BD25
	};
	if (IsLocalObserverPlayer())
		return SkipReshroudMap;

	return 0;
}

DEFINE_HOOK(0x6E0F90, TActionClass_GrowShroud_ObserverImmunity, 0x5)
{
	enum
	{
		SkipGrowShroud = 0x6E0F9A
	};
	if (IsLocalObserverPlayer())
		return SkipGrowShroud;

	return 0;
}


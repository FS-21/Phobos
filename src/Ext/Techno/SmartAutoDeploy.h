#pragma once

#include <TechnoClass.h>

namespace SmartAutoDeploy
{
	// State & Capability Queries
	bool ShouldEvaluate(TechnoClass* pThis);
	TechnoTypeClass* GetAlternateType(TechnoClass* pThis);
	bool IsCurrentlyDeployed(TechnoClass* pThis);
	bool CanPerformTransformation(TechnoClass* pThis, bool toDeploy);
	bool ExecuteTransformation(TechnoClass* pThis, bool toDeploy);

	// Context Evaluators
	bool EvaluateCombat(TechnoClass* pThis);
	bool EvaluateTravel(TechnoClass* pThis);
	bool EvaluateIdle(TechnoClass* pThis);

	// Main Update Entry Point
	void Update(TechnoClass* pThis);
}

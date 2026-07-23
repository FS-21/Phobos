#include "ObserverUI.h"

#include <Phobos.h>
#include <WWMouseClass.h>
#include <Surface.h>

DEFINE_HOOK(0x4F4589, GScreenClass_Draw_ObserverUI, 0x5)
{
	if (ObserverUIClass::IsActive())
	{
		ObserverUIClass::Instance.Update();
		ObserverUIClass::Instance.Render(DSurface::Composite);
	}
	return 0;
}

DEFINE_HOOK(0x4F43BE, GScreenClass_Input_ObserverUI, 0x7)
{
	if (ObserverUIClass::IsActive() && WWMouseClass::Instance)
	{
		DWORD dwKey = R->ESI();
		if (dwKey & 1)
		{
			Point2D mousePos { WWMouseClass::Instance->GetX(), WWMouseClass::Instance->GetY() };
			if (ObserverUIClass::Instance.HandleMouseClick(mousePos, false))
			{
				R->ESI(0);
			}
		}
	}

	return 0;
}

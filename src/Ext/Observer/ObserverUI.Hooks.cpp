#include "ObserverUI.h"

#include <Phobos.h>
#include <WWMouseClass.h>
#include <MouseClass.h>
#include <Surface.h>

DEFINE_HOOK(0x4F4589, GScreenClass_Draw_ObserverUI, 0x5)
{
	if (ObserverUIClass::IsActive())
	{
		ObserverUIClass::Instance.Update();
		ObserverUIClass::Instance.Render(DSurface::Composite);
	}
	else
	{
		ObserverUIClass::Instance.ClearData();
	}
	return 0;
}

DEFINE_HOOK(0x4F43BE, GScreenClass_Input_ObserverUI, 0x7)
{
	if (ObserverUIClass::IsActive() && WWMouseClass::Instance)
	{
		auto const pKey = R->ESI<int*>();
		if (pKey)
		{
			int keyVal = *pKey;
			Point2D mousePos { WWMouseClass::Instance->GetX(), WWMouseClass::Instance->GetY() };

			if (ObserverUIClass::Instance.IsMouseHoveringUI() || ObserverUIClass::Instance.IsSearchFocused())
			{
				MouseClass::Instance.UpdateCursor(MouseCursorType::Default, false);

				if (keyVal == 1)
				{
					ObserverUIClass::Instance.HandleMouseClick(mousePos, false);
				}
				else if (keyVal == 2)
				{
					ObserverUIClass::Instance.HandleMouseClick(mousePos, true);
				}

				// Zero out key value at pointer so downstream code reads 0 without NULL dereference crash
				*pKey = 0;
			}
		}
	}

	return 0;
}

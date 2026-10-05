#include "ObserverUI.h"

#include <Phobos.h>
#include <WWMouseClass.h>
#include <MouseClass.h>
#include <Surface.h>

DEFINE_HOOK(0x4F4589, GScreenClass_Draw_ObserverUI, 0x5)
{
	bool isActive = ObserverUIClass::IsActive();
	bool isDevUIOpen = Phobos::Config::DevelopmentCommands && ObserverUIClass::Instance.GetDisplayMode() != ObserverUIDisplayMode::Hidden;

	if (isActive || isDevUIOpen)
	{
		ObserverUIClass::Instance.Update();
		ObserverUIClass::Instance.Render(DSurface::Composite);
	}
	return 0;
}

DEFINE_HOOK(0x4F43BE, GScreenClass_Input_ObserverUI, 0x7)
{
	bool isActive = ObserverUIClass::IsActive();
	bool isDevUIOpen = Phobos::Config::DevelopmentCommands && ObserverUIClass::Instance.GetDisplayMode() != ObserverUIDisplayMode::Hidden;
	bool isDevCardsOpen = Phobos::Config::DevelopmentCommands && ObserverUIClass::Instance.HasFloatingWindows();

	if ((isActive || isDevUIOpen || isDevCardsOpen) && WWMouseClass::Instance)
	{
		auto const pKey = R->ESI<int*>();
		if (pKey)
		{
			int keyVal = *pKey;
			Point2D mousePos { WWMouseClass::Instance->GetX(), WWMouseClass::Instance->GetY() };

			if (ObserverUIClass::Instance.IsSearchFocused())
			{
				MouseClass::Instance.UpdateCursor(MouseCursorType::Default, false);

				if (keyVal == 1)
				{
					ObserverUIClass::Instance.HandleMouseClick(mousePos, false);
					*pKey = 0;
				}
				else if (keyVal == 2 || keyVal == 4)
				{
					ObserverUIClass::Instance.HandleMouseClick(mousePos, true);
					*pKey = 0;
				}
				else if (keyVal != 0)
				{
					if (ObserverUIClass::Instance.HandleKeyPress(keyVal))
					{
						*pKey = 0;
					}
				}
			}
			else if (ObserverUIClass::Instance.IsMouseHoveringUI())
			{
				MouseClass::Instance.UpdateCursor(MouseCursorType::Default, false);

				if (keyVal == 1)
				{
					ObserverUIClass::Instance.HandleMouseClick(mousePos, false);
					*pKey = 0;
				}
				else if (keyVal == 2 || keyVal == 4)
				{
					ObserverUIClass::Instance.HandleMouseClick(mousePos, true);
					*pKey = 0;
				}
			}
		}
	}

	return 0;
}

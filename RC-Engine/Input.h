/*========================================================================================
|                                   RC-Engine (c) 2016                                   |
|                             Project: RC-Engine                                         |
|                             File: Input.h                                              |
|                             Author: Ruscris2                                           |
==========================================================================================*/
#pragma once

#define KEYBOARD_KEY_ESCAPE 0x1B
#define KEYBOARD_KEY_F 0x46
#define KEYBOARD_KEY_W 0x57
#define KEYBOARD_KEY_A 0x41
#define KEYBOARD_KEY_S 0x53
#define KEYBOARD_KEY_D 0x44
#define MOUSE_LEFTBUTTON 0x01

class Input
{
	private:
		int prevCursorX, prevCursorY;
		int cursorRelativeX, cursorRelativeY;
	public:
		Input();

		bool IsKeyPressed(int key);
		void Update();
		int GetCursorRelativeX();
		int GetCursorRelativeY();
};
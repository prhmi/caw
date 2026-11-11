#pragma once

void appSetupStyle(float dpiScale);  // dark theme + DPI scaling
void appInit();                      // creates the demo project
void appFrame(float dt);
struct ImFont;
void appSetCodeFonts(ImFont* regular, ImFont* bold, ImFont* italic);             // draws the whole UI, call between NewFrame and Render

void appOpenProjectPath(const char* path);  // accepts a .caw path from Windows file association or drag/drop
bool appQuitRequested();                     // Ctrl+Q / File > Exit

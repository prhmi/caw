#pragma once

void appSetupStyle(float dpiScale);  // dark theme + DPI scaling
void appInit();                      // creates the demo project
void appDrawStartupSplash();         // draw one startup frame before loading
void appFrame(float dt, bool visible = true);
struct ImFont;
void appSetCodeFonts(ImFont* regular, ImFont* bold, ImFont* italic);             // draws the whole UI, call between NewFrame and Render
void appSetMarkFonts(ImFont* ui, ImFont* code);
void appPrepareLogo();               // add original-resolution logo to the font texture
bool appNeedsResponsiveFrames();
bool appEnginesIdleRunning();     // engine/transport/render work still needs full UI cadence

void appOpenProjectPath(const char* path);  // accepts a .caw path from Windows file association or drag/drop
bool appQuitRequested();
void appRequestClose();                      // window close button: asks to save when the project has unsaved changes                     // Ctrl+Q / File > Exit

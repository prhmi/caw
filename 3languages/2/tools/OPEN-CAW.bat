@echo off
setlocal EnableExtensions
 title CAW - Start with libpd
 echo.
 echo Select your CAW executable in the file picker.
 echo This launcher temporarily adds the libpd folder to CAW's library search path.
 set "PATH=%~dp0;%PATH%"
 set "CAW_APP="
 for /f "usebackq delims=" %%F in (`powershell.exe -NoProfile -STA -Command "Add-Type -AssemblyName System.Windows.Forms; $d = New-Object System.Windows.Forms.OpenFileDialog; $d.Title = 'Select caw.exe'; $d.Filter = 'CAW executable (caw.exe)|caw.exe|Programs (*.exe)|*.exe'; if ($d.ShowDialog() -eq [System.Windows.Forms.DialogResult]::OK) { $d.FileName }"`) do set "CAW_APP=%%F"
 if not defined CAW_APP goto :cancel
 if not exist "%CAW_APP%" goto :missing
 start "" "%CAW_APP%"
 echo.
 echo CAW started. In PD Preview browse to this folder and select libpd.dll:
 echo %~dp0
 pause
 exit /b 0
:missing
 echo The selected file was not found.
 pause
 exit /b 1
:cancel
 echo No program selected.
 pause
 exit /b 1

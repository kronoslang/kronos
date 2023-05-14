SET KRONOS_BIN=%~dp0bin
SET KRONOS_RUNTIME_LIBRARY=%~dp0runtime\library
SET PATH=%~dp0bin;%PATH%
start powershell -NoExit -Command Write-Host ""Welcome to Kronos!`n`nBinaries: %KRONOS_BIN%`nLibrary : %KRONOS_RUNTIME_LIBRARY%`n""; cd ~
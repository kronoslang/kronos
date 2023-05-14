@echo off
rem set URL="http://download.microsoft.com/download/0/4/1/041224F6-A7DC-486B-BD66-BCAAF74B6919/vc_redist.x64.exe"
set URL="http://download.microsoft.com/download/9/3/F/93FCF1E7-E6A4-478B-96E7-D4B285925B00/vc_redist.x64.exe"
set FILE=%TMP%\vcredist_x64.exe

if exist %FILE% ( goto EOF )

echo Acquiring Visual Studio 2015 RC runtime libraries ... 
echo %URL%

call wget --help >nul 2>&1
if NOT ERRORLEVEL 1 (
    call wget -O %FILE% %URL%
    goto EOF
)
call curl --help >nul 2>&1
if NOT ERRORLEVEL 1 (
    rem We set CURL_PROXY to a space character below to pose as a no-op argument
    set CURL_PROXY= 
    if NOT "x%HTTPS_PROXY%" == "x" set CURL_PROXY="-x %HTTPS_PROXY%"
    call curl %CURL_PROXY% -f -L -o  %FILE% %URL%
    goto EOF
)
call powershell -? >nul 2>&1
if NOT ERRORLEVEL 1 (
    powershell -Command "& {param($a,$f) (new-object System.Net.WebClient).DownloadFile($a, $f)}" ""%URL%"" ""%FILE%""
    goto EOF
)

:EOF

%FILE% /passive /norestart


@echo off
rem NoX Core for tracking down a problem: a console shows the log as it is written (it is also
rem saved to noxcore-log.txt beside noxcore.exe), with a line on frame times every 5 seconds.
rem Options given to this file are passed on to noxcore.exe.
cd /d "%~dp0"
noxcore.exe -cmd --frame-stats %*
set NOX_EXIT=%ERRORLEVEL%
if not "%NOX_EXIT%"=="0" (
    echo.
    echo noxcore.exe stopped with code %NOX_EXIT%. Its log is in noxcore-log.txt beside it.
    pause
)

@echo off
setlocal
set "APP_DIR=%~dp0"
if exist "%~dp0dist\" set "APP_DIR=%~dp0dist\"
if not exist "%APP_DIR%DeribitGrid.exe" (
    echo Application not found. Build the project or restore the complete dist folder.
    pause
    exit /b 1
)
pushd "%APP_DIR%"
start "" "%APP_DIR%DeribitGrid.exe" %*
popd

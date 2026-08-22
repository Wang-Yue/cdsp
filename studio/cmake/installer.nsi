; ==============================================================================
; NSIS Modern Installer Script for CDSP Monitor (Windows x86_64)
; ==============================================================================

Unicode True
!include "MUI2.nsh"
!include "FileFunc.nsh"

; Define default parameters if not passed via command line
!ifndef APP_NAME
    !define APP_NAME "CDSP Monitor"
!endif

!ifndef APP_VERSION
    !define APP_VERSION "1.0.0"
!endif

!ifndef APP_PUBLISHER
    !define APP_PUBLISHER "DSPMonitor"
!endif

!ifndef APP_EXE
    !define APP_EXE "MonitorQt.exe"
!endif

!ifndef SOURCE_DIR
    !define SOURCE_DIR ".."
!endif

!ifndef STAGE_DIR
    !define STAGE_DIR "..\build-windows\dist\MonitorQt"
!endif

!ifndef OUTPUT_EXE
    !define OUTPUT_EXE "..\build-windows\MonitorQt-Setup-${APP_VERSION}.exe"
!endif

Name "${APP_NAME}"
OutFile "${OUTPUT_EXE}"
InstallDir "$PROGRAMFILES64\${APP_NAME}"
InstallDirRegKey HKLM "Software\${APP_PUBLISHER}\${APP_NAME}" "InstallDir"
RequestExecutionLevel admin

; UI & Branding
!define MUI_ABORTWARNING
!define MUI_ICON "${SOURCE_DIR}/src/resources/icons/app.ico"
!define MUI_UNICON "${SOURCE_DIR}/src/resources/icons/app.ico"
!define MUI_HEADERIMAGE
!define MUI_FINISHPAGE_RUN "$INSTDIR\${APP_EXE}"
!define MUI_FINISHPAGE_RUN_TEXT "Launch ${APP_NAME}"

; Pages
!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH

!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES

; Languages
!insertmacro MUI_LANGUAGE "English"

; Installer Section
Section "MainSection" SEC01
    SetOutPath "$INSTDIR"
    SetOverwrite on

    ; Copy all files and subdirectories from staging directory
    File /r "${STAGE_DIR}\*.*"

    ; Create Uninstaller
    WriteUninstaller "$INSTDIR\Uninstall.exe"

    ; Store install location in registry
    WriteRegStr HKLM "Software\${APP_PUBLISHER}\${APP_NAME}" "InstallDir" "$INSTDIR"

    ; Windows Add/Remove Programs registration
    !define UNINST_KEY "Software\Microsoft\Windows\CurrentVersion\Uninstall\${APP_NAME}"
    WriteRegStr HKLM "${UNINST_KEY}" "DisplayName" "${APP_NAME}"
    WriteRegStr HKLM "${UNINST_KEY}" "DisplayVersion" "${APP_VERSION}"
    WriteRegStr HKLM "${UNINST_KEY}" "Publisher" "${APP_PUBLISHER}"
    WriteRegStr HKLM "${UNINST_KEY}" "DisplayIcon" "$INSTDIR\${APP_EXE},0"
    WriteRegStr HKLM "${UNINST_KEY}" "UninstallString" '"$INSTDIR\Uninstall.exe"'
    WriteRegStr HKLM "${UNINST_KEY}" "QuietUninstallString" '"$INSTDIR\Uninstall.exe" /S'
    WriteRegDWORD HKLM "${UNINST_KEY}" "NoModify" 1
    WriteRegDWORD HKLM "${UNINST_KEY}" "NoRepair" 1

    ; Compute estimated size
    ${GetSize} "$INSTDIR" "/S=0K" $0 $1 $2
    IntFmt $0 "0x%08X" $0
    WriteRegDWORD HKLM "${UNINST_KEY}" "EstimatedSize" "$0"

    ; Start Menu Shortcuts
    CreateDirectory "$SMPROGRAMS\${APP_NAME}"
    CreateShortcut "$SMPROGRAMS\${APP_NAME}\${APP_NAME}.lnk" "$INSTDIR\${APP_EXE}" "" "$INSTDIR\${APP_EXE}" 0
    CreateShortcut "$SMPROGRAMS\${APP_NAME}\Uninstall ${APP_NAME}.lnk" "$INSTDIR\Uninstall.exe" "" "$INSTDIR\Uninstall.exe" 0

    ; Desktop Shortcut
    CreateShortcut "$DESKTOP\${APP_NAME}.lnk" "$INSTDIR\${APP_EXE}" "" "$INSTDIR\${APP_EXE}" 0
SectionEnd

; Uninstaller Section
Section "Uninstall"
    ; Remove Desktop shortcut
    Delete "$DESKTOP\${APP_NAME}.lnk"

    ; Remove Start Menu shortcuts
    Delete "$SMPROGRAMS\${APP_NAME}\${APP_NAME}.lnk"
    Delete "$SMPROGRAMS\${APP_NAME}\Uninstall ${APP_NAME}.lnk"
    RMDir "$SMPROGRAMS\${APP_NAME}"

    ; Remove application files and directories
    RMDir /r "$INSTDIR"

    ; Remove Registry keys
    DeleteRegKey HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\${APP_NAME}"
    DeleteRegKey HKLM "Software\${APP_PUBLISHER}\${APP_NAME}"
    DeleteRegKey /ifempty HKLM "Software\${APP_PUBLISHER}"
SectionEnd

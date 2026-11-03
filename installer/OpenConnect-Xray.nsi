Unicode true
SetCompressor /SOLID lzma
SetCompressorDictSize 64
SetDatablockOptimize on
SetOverwrite on

!include "MUI2.nsh"
!include "LogicLib.nsh"
!include "x64.nsh"
!include "FileFunc.nsh"

!define APP_NAME "OpenConnect VPN + Xray"
!define APP_VERSION "1.6.2"
!define APP_PUBLISHER "OpenConnect-GUI Team"
!define APP_URL "https://gitlab.com/openconnect/openconnect-gui"
!define APP_EXE "openconnect-gui.exe"
!define APP_REGKEY "Software\Microsoft\Windows\CurrentVersion\Uninstall\OpenConnectXray"
!define STARTMENU "OpenConnect Xray"
!define ROOT "${__FILEDIR__}\.."
!define TAP_DIR "${ROOT}\third_party\tap-windows\tap9.27\dist.win10\amd64"

Name "${APP_NAME} ${APP_VERSION}"
BrandingText "${APP_NAME}"

!ifdef TEST_INSTALL
  OutFile "${ROOT}\dist\OpenConnect-Xray-Setup-test.exe"
  RequestExecutionLevel user
  InstallDir "$TEMP\OpenConnectXrayTest"
!else
  OutFile "${ROOT}\dist\OpenConnect-Xray-Setup-${APP_VERSION}.exe"
  RequestExecutionLevel admin
  InstallDir "$PROGRAMFILES64\OpenConnect Xray"
!endif

InstallDirRegKey HKLM "${APP_REGKEY}" "InstallLocation"

VIProductVersion "${APP_VERSION}.0"
VIFileVersion "${APP_VERSION}.0"
VIAddVersionKey /LANG=1033 "ProductName" "${APP_NAME}"
VIAddVersionKey /LANG=1033 "FileDescription" "${APP_NAME} Setup"
VIAddVersionKey /LANG=1033 "FileVersion" "${APP_VERSION}"
VIAddVersionKey /LANG=1033 "ProductVersion" "${APP_VERSION}"
VIAddVersionKey /LANG=1033 "CompanyName" "${APP_PUBLISHER}"
VIAddVersionKey /LANG=1033 "LegalCopyright" "GPLv2 / third-party licenses included"

!define MUI_ABORTWARNING
!define MUI_ICON "${ROOT}\src\openconnect-gui.ico"
!define MUI_UNICON "${ROOT}\src\openconnect-gui.ico"
!define MUI_HEADERIMAGE
!define MUI_HEADERIMAGE_BITMAP "${ROOT}\nsis\images\header-openconnect.bmp"
!define MUI_WELCOMEFINISHPAGE_BITMAP "${ROOT}\nsis\images\install-openconnect.bmp"
!define MUI_FINISHPAGE_RUN "$INSTDIR\${APP_EXE}"
!define MUI_FINISHPAGE_RUN_TEXT "Launch ${APP_NAME}"
!define MUI_FINISHPAGE_RUN_NOTCHECKED

!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_LICENSE "${ROOT}\LICENSE.txt"
!insertmacro MUI_PAGE_COMPONENTS
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH

!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES

!insertmacro MUI_LANGUAGE "English"

Function CheckRunning
retry:
  System::Call 'user32::FindWindowW(w 0, w "OpenConnect VPN") i .r1'
  StrCmp $1 0 done
  MessageBox MB_RETRYCANCEL|MB_ICONEXCLAMATION \
    "OpenConnect VPN is running. Close it (including the tray icon), then click Retry." \
    IDRETRY retry
  Abort
done:
FunctionEnd

Function un.CheckRunning
unretry:
  System::Call 'user32::FindWindowW(w 0, w "OpenConnect VPN") i .r1'
  StrCmp $1 0 undoned
  MessageBox MB_RETRYCANCEL|MB_ICONEXCLAMATION \
    "OpenConnect VPN is running. Close it (including the tray icon), then click Retry." \
    IDRETRY unretry
  Abort
undoned:
FunctionEnd

Function .onInit
  ${IfNot} ${RunningX64}
    MessageBox MB_ICONSTOP "This installer requires 64-bit Windows."
    Abort
  ${EndIf}
  SetRegView 64
  !ifdef TEST_INSTALL
    SetShellVarContext current
  !else
    SetShellVarContext all
  !endif
  Call CheckRunning
FunctionEnd

Function un.onInit
  ${IfNot} ${RunningX64}
    MessageBox MB_ICONSTOP "This uninstaller requires 64-bit Windows."
    Abort
  ${EndIf}
  SetRegView 64
  !ifdef TEST_INSTALL
    SetShellVarContext current
  !else
    SetShellVarContext all
  !endif
  Call un.CheckRunning
FunctionEnd

Section "Application files (required)" SecApp
  SectionIn RO

  SetOutPath "$INSTDIR"
  File /r "${ROOT}\dist\OpenConnect-Xray\*"
  File "/oname=XRAY-GUIDE.md" "${ROOT}\XRAY-GUIDE.md"

  WriteUninstaller "$INSTDIR\Uninstall.exe"

  CreateDirectory "$SMPROGRAMS\${STARTMENU}"
  CreateShortCut "$SMPROGRAMS\${STARTMENU}\OpenConnect VPN + Xray.lnk" "$INSTDIR\${APP_EXE}"
  CreateShortCut "$SMPROGRAMS\${STARTMENU}\Guide.lnk" "$INSTDIR\XRAY-GUIDE.md"
  CreateShortCut "$SMPROGRAMS\${STARTMENU}\Uninstall.lnk" "$INSTDIR\Uninstall.exe"

  !ifndef TEST_INSTALL
    WriteRegStr HKLM "${APP_REGKEY}" "DisplayName" "${APP_NAME}"
    WriteRegStr HKLM "${APP_REGKEY}" "DisplayVersion" "${APP_VERSION}"
    WriteRegStr HKLM "${APP_REGKEY}" "Publisher" "${APP_PUBLISHER}"
    WriteRegStr HKLM "${APP_REGKEY}" "InstallLocation" "$INSTDIR"
    WriteRegStr HKLM "${APP_REGKEY}" "DisplayIcon" "$INSTDIR\${APP_EXE}"
    WriteRegStr HKLM "${APP_REGKEY}" "UninstallString" '"$INSTDIR\Uninstall.exe"'
    WriteRegStr HKLM "${APP_REGKEY}" "QuietUninstallString" '"$INSTDIR\Uninstall.exe" /S'
    WriteRegStr HKLM "${APP_REGKEY}" "URLInfoAbout" "${APP_URL}"
    WriteRegDWORD HKLM "${APP_REGKEY}" "NoModify" 1
    WriteRegDWORD HKLM "${APP_REGKEY}" "NoRepair" 1

    ${GetSize} "$INSTDIR" "/S=0K" $0 $1 $2
    WriteRegDWORD HKLM "${APP_REGKEY}" "EstimatedSize" $0
  !endif
SectionEnd

Section "TAP-Windows driver for OpenConnect" SecTap
  !ifndef TEST_INSTALL
    SetOutPath "$INSTDIR\Drivers\tap-windows"
    File "${TAP_DIR}\devcon.exe"
    File "${TAP_DIR}\OemVista.inf"
    File "${TAP_DIR}\tap0901.cat"
    File "${TAP_DIR}\tap0901.sys"
    File "/oname=LICENSE-TAP-GPL.txt" "${ROOT}\third_party\tap-windows\COPYRIGHT.GPL"
    File "/oname=LICENSE-TAP-MIT.txt" "${ROOT}\third_party\tap-windows\COPYRIGHT.MIT"

    ExecWait '"$INSTDIR\Drivers\tap-windows\devcon.exe" install "$INSTDIR\Drivers\tap-windows\OemVista.inf" tap0901' $0
  !endif
SectionEnd

Section "Desktop shortcut" SecDesktop
  CreateShortCut "$DESKTOP\OpenConnect VPN + Xray.lnk" "$INSTDIR\${APP_EXE}"
SectionEnd

Section "Uninstall"
  Delete "$DESKTOP\OpenConnect VPN + Xray.lnk"
  RMDir /r "$SMPROGRAMS\${STARTMENU}"

  !ifndef TEST_INSTALL
    MessageBox MB_YESNO|MB_ICONQUESTION|MB_DEFBUTTON2 \
      "Also remove saved OpenConnect/Xray profiles and settings?$\r$\n$\r$\nChoose No to keep them for a future reinstall." \
      IDNO skipProfiles
    DeleteRegKey HKCU "Software\OpenConnect-GUI Team\OpenConnect-GUI"
  skipProfiles:
    DeleteRegKey HKLM "${APP_REGKEY}"
  !endif

  RMDir /r "$INSTDIR"
SectionEnd

!insertmacro MUI_FUNCTION_DESCRIPTION_BEGIN
  !insertmacro MUI_DESCRIPTION_TEXT ${SecApp} "OpenConnect VPN GUI, bundled Xray core, Qt runtime, licenses and guide."
  !insertmacro MUI_DESCRIPTION_TEXT ${SecTap} "Signed TAP-Windows driver used by OpenConnect SSL-VPN profiles. Xray TUN uses the bundled Wintun DLL and does not need this driver."
  !insertmacro MUI_DESCRIPTION_TEXT ${SecDesktop} "Create a desktop shortcut."
!insertmacro MUI_FUNCTION_DESCRIPTION_END

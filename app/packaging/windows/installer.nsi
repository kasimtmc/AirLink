; AirLink's Windows installer: per user (no administrator), in %LOCALAPPDATA%\Programs\AirLink.
; Built by scripts/release/build-windows.sh: makensis -DVERSION=x.y.z -DSOURCE=<unpacked zip> -DOUTFILE=...
Unicode true
SetCompressor /SOLID lzma
RequestExecutionLevel user

!include "MUI2.nsh"

!ifndef VERSION
  !error "VERSION is not set"
!endif
!ifndef SOURCE
  !error "SOURCE is not set"
!endif

Name "AirLink"
OutFile "${OUTFILE}"
InstallDir "$LOCALAPPDATA\Programs\AirLink"
InstallDirRegKey HKCU "Software\AirLink" "InstallDir"
BrandingText "AirLink ${VERSION}"
VIProductVersion "${VERSION}.0"
VIAddVersionKey "ProductName" "AirLink"
VIAddVersionKey "FileDescription" "AirLink kurulumu"
VIAddVersionKey "FileVersion" "${VERSION}"
VIAddVersionKey "ProductVersion" "${VERSION}"
VIAddVersionKey "LegalCopyright" "Kasim"

!define UNINSTALL_KEY "Software\Microsoft\Windows\CurrentVersion\Uninstall\AirLink"
!define MUI_ICON "${NSISDIR}\Contrib\Graphics\Icons\modern-install.ico"
!define MUI_UNICON "${NSISDIR}\Contrib\Graphics\Icons\modern-uninstall.ico"
!define MUI_FINISHPAGE_RUN "$INSTDIR\airlink.exe"
!define MUI_FINISHPAGE_RUN_TEXT "AirLink'i başlat"

!insertmacro MUI_PAGE_LICENSE "${SOURCE}\LICENSE.txt"
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_LANGUAGE "Turkish"
!insertmacro MUI_LANGUAGE "English"

Section "AirLink"
  ; A running AirLink holds airlink.exe open: close it first (an update).
  nsExec::Exec 'taskkill /IM airlink.exe /F'
  Sleep 500
  SetOutPath "$INSTDIR"
  File "${SOURCE}\airlink.exe"
  File "${SOURCE}\libEGL.dll"
  File "${SOURCE}\libGLESv2.dll"
  File "${SOURCE}\LICENSE.txt"
  File "${SOURCE}\diagnose.cmd"
  SetOutPath "$INSTDIR\fonts"
  File /r "${SOURCE}\fonts\*"
  SetOutPath "$INSTDIR\licenses"
  File /r "${SOURCE}\licenses\*"
  SetOutPath "$INSTDIR"

  WriteUninstaller "$INSTDIR\uninstall.exe"
  CreateShortcut "$SMPROGRAMS\AirLink.lnk" "$INSTDIR\airlink.exe"
  WriteRegStr HKCU "Software\AirLink" "InstallDir" "$INSTDIR"
  WriteRegStr HKCU "${UNINSTALL_KEY}" "DisplayName" "AirLink"
  WriteRegStr HKCU "${UNINSTALL_KEY}" "DisplayVersion" "${VERSION}"
  WriteRegStr HKCU "${UNINSTALL_KEY}" "Publisher" "Kasim"
  WriteRegStr HKCU "${UNINSTALL_KEY}" "DisplayIcon" "$INSTDIR\airlink.exe"
  WriteRegStr HKCU "${UNINSTALL_KEY}" "InstallLocation" "$INSTDIR"
  WriteRegStr HKCU "${UNINSTALL_KEY}" "URLInfoAbout" "https://github.com/kasimtmc/AirLink"
  WriteRegStr HKCU "${UNINSTALL_KEY}" "UninstallString" '"$INSTDIR\uninstall.exe"'
  WriteRegDWORD HKCU "${UNINSTALL_KEY}" "NoModify" 1
  WriteRegDWORD HKCU "${UNINSTALL_KEY}" "NoRepair" 1
SectionEnd

Section "Uninstall"
  nsExec::Exec 'taskkill /IM airlink.exe /F'
  Sleep 500
  ; Explorer's "AirLink ile gönder", the Send To shortcut and the start-at-login entry.
  nsExec::Exec '"$INSTDIR\airlink.exe" --unregister'
  Delete "$SMPROGRAMS\AirLink.lnk"
  RMDir /r "$INSTDIR\fonts"
  RMDir /r "$INSTDIR\licenses"
  Delete "$INSTDIR\airlink.exe"
  Delete "$INSTDIR\libEGL.dll"
  Delete "$INSTDIR\libGLESv2.dll"
  Delete "$INSTDIR\LICENSE.txt"
  Delete "$INSTDIR\diagnose.cmd"
  Delete "$INSTDIR\uninstall.exe"
  RMDir "$INSTDIR"
  DeleteRegKey HKCU "${UNINSTALL_KEY}"
  DeleteRegKey HKCU "Software\AirLink"
  ; Pairings and received files stay (AppData, Downloads\AirLink).
SectionEnd

; =========================
; BNL Installer (separate x86/x64)
; File: installer/windows/bnl_installer.nsi
; Build:
;   makensis /DARCH=x64 installer/windows/bnl_installer.nsi
;   makensis /DARCH=x86 installer/windows/bnl_installer.nsi
;
; Expects the CMake release builds to already exist:
;   build/windows-release/bin/bnl.exe
;   build/windows-x86-release/bin/bnl.exe
;
; PATH manipulation uses ReadRegStr/WriteRegExpandStr — for very long system
; PATHs (>1024 chars with the default NSIS build, >8192 with the Large Strings
; build) the read will truncate. Use the Large Strings NSIS build if your
; target systems have very long PATH values.
; =========================

!ifndef ARCH
  !define ARCH "x64"         ; default if not passed
!endif

!define PRODUCT_NAME        "Bnlang"
!define PRODUCT_VERSION     "1.0.0"
!define PRODUCT_PUBLISHER   "Bnlang"
!define PRODUCT_UNINST_KEY  "Bnlang"
!define INSTALL_DIR_NAME    "Bnlang"  ; Folder name under Program Files
!define MUI_WELCOMEFINISHPAGE_BITMAP ".\\images\\welcome.bmp"
!define MUI_UNWELCOMEFINISHPAGE_BITMAP ".\\images\\welcome.bmp"

!define MUI_ICON "..\\..\\resources\\bnlang.ico"
!define MUI_UNICON "..\\..\\resources\\bnlang.ico"

Unicode True
RequestExecutionLevel admin

!include "MUI2.nsh"
!include "LogicLib.nsh"
!include "x64.nsh"
!include "WinMessages.nsh"
!include "StrFunc.nsh"

; StrFunc macros must be "called" at script scope before use. The Un-prefixed
; variants are needed for the uninstaller section.
${StrStr}
${UnStrRep}

Name "${PRODUCT_NAME} ${PRODUCT_VERSION} (${ARCH})"
OutFile "bnlang-windows-${ARCH}-v${PRODUCT_VERSION}-installer.exe"

; ----- Install location & registry view per arch -----
!if "${ARCH}" == "x64"
  InstallDir "$PROGRAMFILES64\\${INSTALL_DIR_NAME}"
!else
  InstallDir "$PROGRAMFILES32\\${INSTALL_DIR_NAME}"
!endif

SetCompress auto
SetCompressor /SOLID lzma

; -------------------------
; MUI Pages
; -------------------------
!define MUI_ABORTWARNING
!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_LICENSE "..\\..\\resources\\LICENSE.txt"
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH

!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_UNPAGE_FINISH

!insertmacro MUI_LANGUAGE "English"
BrandingText "Bnlang Installer"

; ============================================================
;                      Functions
; ============================================================

; Append $INSTDIR to system PATH (HKLM). Idempotent: a second install won't
; create a duplicate entry. Broadcasts WM_SETTINGCHANGE so already-running
; Explorer/cmd shells pick up the change without a reboot (new shells only —
; existing process environments are not retroactively updated).
Function AddToSystemPath
  ReadRegStr $0 HKLM "System\\CurrentControlSet\\Control\\Session Manager\\Environment" "Path"
  ${StrStr} $1 ";$0;" ";$INSTDIR;"
  ${If} $1 == ""
    ${If} $0 == ""
      StrCpy $0 "$INSTDIR"
    ${Else}
      StrCpy $0 "$0;$INSTDIR"
    ${EndIf}
    WriteRegExpandStr HKLM "System\\CurrentControlSet\\Control\\Session Manager\\Environment" "Path" "$0"
    SendMessage ${HWND_BROADCAST} ${WM_WININICHANGE} 0 "STR:Environment" /TIMEOUT=5000
  ${EndIf}
FunctionEnd

; Remove $INSTDIR from system PATH. Handles all four arrangements:
; "$INSTDIR" only / leading ";$INSTDIR" / trailing "$INSTDIR;" / middle ";$INSTDIR;".
Function un.RemoveFromSystemPath
  ReadRegStr $0 HKLM "System\\CurrentControlSet\\Control\\Session Manager\\Environment" "Path"
  ${UnStrRep} $0 $0 ";$INSTDIR;" ";"
  ${UnStrRep} $0 $0 ";$INSTDIR"  ""
  ${UnStrRep} $0 $0 "$INSTDIR;"  ""
  ${UnStrRep} $0 $0 "$INSTDIR"   ""
  WriteRegExpandStr HKLM "System\\CurrentControlSet\\Control\\Session Manager\\Environment" "Path" "$0"
  SendMessage ${HWND_BROADCAST} ${WM_WININICHANGE} 0 "STR:Environment" /TIMEOUT=5000
FunctionEnd

; ============================================================
;                      Sections
; ============================================================

Section "Core (required)" SEC_CORE
  SectionIn RO

  !if "${ARCH}" == "x64"
    ${IfNot} ${RunningX64}
      MessageBox MB_ICONSTOP "This is the 64-bit installer, but your Windows is 32-bit. Please use the x86 installer."
      Abort
    ${EndIf}
  !endif

  !if "${ARCH}" == "x64"
    SetRegView 64
  !else
    SetRegView 32
  !endif

  SetOutPath "$INSTDIR"

  !if "${ARCH}" == "x64"
    File /oname=bnl.exe "..\\..\\build\\windows-release\\bin\\bnl.exe"
  !else
    File /oname=bnl.exe "..\\..\\build\\windows-x86-release\\bin\\bnl.exe"
  !endif

  File /nonfatal "..\\..\\resources\\README.txt"
  File /nonfatal "..\\..\\resources\\LICENSE.txt"

  WriteUninstaller "$INSTDIR\\Uninstall.exe"

  WriteRegStr   HKLM "Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\${PRODUCT_UNINST_KEY}" "DisplayName"     "${PRODUCT_NAME} (${ARCH})"
  WriteRegStr   HKLM "Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\${PRODUCT_UNINST_KEY}" "Publisher"        "${PRODUCT_PUBLISHER}"
  WriteRegStr   HKLM "Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\${PRODUCT_UNINST_KEY}" "DisplayVersion"   "${PRODUCT_VERSION}"
  WriteRegStr   HKLM "Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\${PRODUCT_UNINST_KEY}" "InstallLocation"  "$INSTDIR"
  WriteRegStr   HKLM "Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\${PRODUCT_UNINST_KEY}" "DisplayIcon"      "$INSTDIR\\bnl.exe"
  WriteRegStr   HKLM "Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\${PRODUCT_UNINST_KEY}" "UninstallString"  "$\"$INSTDIR\\Uninstall.exe$\""
  WriteRegDWORD HKLM "Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\${PRODUCT_UNINST_KEY}" "NoModify"         1
  WriteRegDWORD HKLM "Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\${PRODUCT_UNINST_KEY}" "NoRepair"         1

  Call AddToSystemPath
SectionEnd

; -------------------------
; Uninstaller
; -------------------------
Section "Uninstall"
  !if "${ARCH}" == "x64"
    SetRegView 64
  !else
    SetRegView 32
  !endif

  Call un.RemoveFromSystemPath

  Delete /REBOOTOK "$INSTDIR\\bnl.exe"
  Delete /REBOOTOK "$INSTDIR\\README.txt"
  Delete /REBOOTOK "$INSTDIR\\LICENSE.txt"
  Delete /REBOOTOK "$INSTDIR\\Uninstall.exe"
  RMDir /r "$INSTDIR"
  DeleteRegKey HKLM "Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\${PRODUCT_UNINST_KEY}"
SectionEnd

; =========================
; BNL Installer (separate x86/x64), per-user or system-wide
; File: installer/windows/bnl_installer.nsi
; Build:
;   makensis /DARCH=x64 installer/windows/bnl_installer.nsi
;   makensis /DARCH=x86 installer/windows/bnl_installer.nsi
;
; Runtime behavior:
;   - Double-clicked as a normal user -> installs to %LOCALAPPDATA%\Programs\Bnlang
;                                        and writes to HKCU (user PATH)
;   - Run as administrator            -> installs to Program Files and writes to
;                                        HKLM (system PATH)
;
; PATH manipulation uses ReadRegStr/WriteRegExpandStr. For very long system
; PATHs (>1024 chars with the default NSIS build, >8192 with the Large Strings
; build) the read will truncate. Use the Large Strings NSIS build if your
; target systems have very long PATH values.
; =========================

!ifndef ARCH
  !define ARCH "x64"         ; default if not passed
!endif

!define PRODUCT_NAME        "Bnlang"
!define PRODUCT_VERSION     "2.0.0"
!define PRODUCT_PUBLISHER   "Bnlang"
!define PRODUCT_UNINST_KEY  "Bnlang"
!define INSTALL_DIR_NAME    "Bnlang"  ; Folder name under Program Files / LocalAppData
!define MUI_WELCOMEFINISHPAGE_BITMAP ".\\images\\welcome.bmp"
!define MUI_UNWELCOMEFINISHPAGE_BITMAP ".\\images\\welcome.bmp"

!define MUI_ICON "..\\..\\resources\\bnlang.ico"
!define MUI_UNICON "..\\..\\resources\\bnlang.ico"

Unicode True
; Do not auto-elevate. The user chooses scope by how they launch:
;   double-click          -> per-user install
;   Run as administrator  -> system-wide install
RequestExecutionLevel user

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

; Placeholder. Real value is set in .onInit / un.onInit based on privileges.
InstallDir "$LOCALAPPDATA\\Programs\\${INSTALL_DIR_NAME}"

SetCompress auto
SetCompressor /SOLID lzma

; -------------------------
; Runtime state
; -------------------------
Var IsAdmin       ; "1" when elevated, "0" otherwise
Var PathRegKey    ; subkey where the PATH value lives (differs HKLM vs HKCU)

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
;                    Init: pick scope
; ============================================================

; Shared body for installer/uninstaller scope detection.
!macro _PickScope un
  !if "${ARCH}" == "x64"
    SetRegView 64
  !else
    SetRegView 32
  !endif

  UserInfo::GetAccountType
  Pop $0
  ${If} $0 == "Admin"
    StrCpy $IsAdmin "1"
    SetShellVarContext all
    StrCpy $PathRegKey "System\\CurrentControlSet\\Control\\Session Manager\\Environment"
  ${Else}
    StrCpy $IsAdmin "0"
    SetShellVarContext current
    StrCpy $PathRegKey "Environment"
  ${EndIf}
!macroend

Function .onInit
  !if "${ARCH}" == "x64"
    ${IfNot} ${RunningX64}
      MessageBox MB_ICONSTOP "This is the 64-bit installer, but your Windows is 32-bit. Please use the x86 installer."
      Abort
    ${EndIf}
  !endif

  !insertmacro _PickScope ""

  ${If} $IsAdmin == "1"
    !if "${ARCH}" == "x64"
      StrCpy $INSTDIR "$PROGRAMFILES64\\${INSTALL_DIR_NAME}"
    !else
      StrCpy $INSTDIR "$PROGRAMFILES32\\${INSTALL_DIR_NAME}"
    !endif
  ${Else}
    StrCpy $INSTDIR "$LOCALAPPDATA\\Programs\\${INSTALL_DIR_NAME}"
  ${EndIf}
FunctionEnd

Function un.onInit
  !insertmacro _PickScope "un."
FunctionEnd

; ============================================================
;                    PATH helpers
; SHCTX = HKLM when SetShellVarContext=all, HKCU when =current.
; ============================================================

Function AddToPath
  ReadRegStr $0 SHCTX $PathRegKey "Path"
  ${StrStr} $1 ";$0;" ";$INSTDIR;"
  ${If} $1 == ""
    ${If} $0 == ""
      StrCpy $0 "$INSTDIR"
    ${Else}
      StrCpy $0 "$0;$INSTDIR"
    ${EndIf}
    WriteRegExpandStr SHCTX $PathRegKey "Path" "$0"
    SendMessage ${HWND_BROADCAST} ${WM_WININICHANGE} 0 "STR:Environment" /TIMEOUT=5000
  ${EndIf}
FunctionEnd

Function un.RemoveFromPath
  ReadRegStr $0 SHCTX $PathRegKey "Path"
  ${UnStrRep} $0 $0 ";$INSTDIR;" ";"
  ${UnStrRep} $0 $0 ";$INSTDIR"  ""
  ${UnStrRep} $0 $0 "$INSTDIR;"  ""
  ${UnStrRep} $0 $0 "$INSTDIR"   ""
  WriteRegExpandStr SHCTX $PathRegKey "Path" "$0"
  SendMessage ${HWND_BROADCAST} ${WM_WININICHANGE} 0 "STR:Environment" /TIMEOUT=5000
FunctionEnd

; ============================================================
;                      Sections
; ============================================================

Section "Core (required)" SEC_CORE
  SectionIn RO

  SetOutPath "$INSTDIR"

  !if "${ARCH}" == "x64"
    File /oname=bnl.exe "..\\..\\build\\windows-release\\bin\\bnl.exe"
  !else
    File /oname=bnl.exe "..\\..\\build\\windows-x86-release\\bin\\bnl.exe"
  !endif

  File /nonfatal "..\\..\\resources\\README.txt"
  File /nonfatal "..\\..\\resources\\LICENSE.txt"

  WriteUninstaller "$INSTDIR\\Uninstall.exe"

  WriteRegStr   SHCTX "Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\${PRODUCT_UNINST_KEY}" "DisplayName"     "${PRODUCT_NAME} (${ARCH})"
  WriteRegStr   SHCTX "Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\${PRODUCT_UNINST_KEY}" "Publisher"        "${PRODUCT_PUBLISHER}"
  WriteRegStr   SHCTX "Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\${PRODUCT_UNINST_KEY}" "DisplayVersion"   "${PRODUCT_VERSION}"
  WriteRegStr   SHCTX "Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\${PRODUCT_UNINST_KEY}" "InstallLocation"  "$INSTDIR"
  WriteRegStr   SHCTX "Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\${PRODUCT_UNINST_KEY}" "DisplayIcon"      "$INSTDIR\\bnl.exe"
  WriteRegStr   SHCTX "Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\${PRODUCT_UNINST_KEY}" "UninstallString"  "$\"$INSTDIR\\Uninstall.exe$\""
  WriteRegDWORD SHCTX "Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\${PRODUCT_UNINST_KEY}" "NoModify"         1
  WriteRegDWORD SHCTX "Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\${PRODUCT_UNINST_KEY}" "NoRepair"         1

  Call AddToPath
SectionEnd

; -------------------------
; Uninstaller
; -------------------------
Section "Uninstall"
  Call un.RemoveFromPath

  Delete /REBOOTOK "$INSTDIR\\bnl.exe"
  Delete /REBOOTOK "$INSTDIR\\README.txt"
  Delete /REBOOTOK "$INSTDIR\\LICENSE.txt"
  Delete /REBOOTOK "$INSTDIR\\Uninstall.exe"
  RMDir /r "$INSTDIR"
  DeleteRegKey SHCTX "Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\${PRODUCT_UNINST_KEY}"
SectionEnd

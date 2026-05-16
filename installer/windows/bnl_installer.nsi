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
; PATH manipulation is delegated to PowerShell ([Environment]::
; GetEnvironmentVariable / SetEnvironmentVariable) so that long PATH values
; round-trip safely regardless of NSIS_MAX_STRLEN. See PATH helpers section
; below for the rationale.
; =========================

!ifndef ARCH
  !define ARCH "x64"         ; default if not passed
!endif

!define PRODUCT_NAME        "Bnlang"
!define PRODUCT_VERSION     "2.0.0"
!define PRODUCT_PUBLISHER   "Bnlang"
!define PRODUCT_UNINST_KEY  "Bnlang"
!define INSTALL_DIR_NAME    "Bnlang"  ; Folder name under Program Files / LocalAppData
!define MUI_WELCOMEFINISHPAGE_BITMAP ".\images\welcome.bmp"
!define MUI_UNWELCOMEFINISHPAGE_BITMAP ".\images\welcome.bmp"

!define MUI_ICON "..\..\resources\bnlang.ico"
!define MUI_UNICON "..\..\resources\bnlang.ico"

Unicode True
; Do not auto-elevate. The user chooses scope by how they launch:
;   double-click          -> per-user install
;   Run as administrator  -> system-wide install
RequestExecutionLevel user

!include "MUI2.nsh"
!include "LogicLib.nsh"
!include "x64.nsh"

Name "${PRODUCT_NAME} ${PRODUCT_VERSION} (${ARCH})"
OutFile "bnlang-windows-${ARCH}-v${PRODUCT_VERSION}-installer.exe"

; Placeholder. Real value is set in .onInit / un.onInit based on privileges.
InstallDir "$LOCALAPPDATA\Programs\${INSTALL_DIR_NAME}"

SetCompress auto
SetCompressor /SOLID lzma

; -------------------------
; Runtime state
; -------------------------
Var IsAdmin       ; "1" when elevated, "0" otherwise

; -------------------------
; MUI Pages
; -------------------------
!define MUI_ABORTWARNING
!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_LICENSE "..\..\resources\LICENSE.txt"
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
  ${Else}
    StrCpy $IsAdmin "0"
    SetShellVarContext current
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
      StrCpy $INSTDIR "$PROGRAMFILES64\${INSTALL_DIR_NAME}"
    !else
      StrCpy $INSTDIR "$PROGRAMFILES32\${INSTALL_DIR_NAME}"
    !endif
  ${Else}
    StrCpy $INSTDIR "$LOCALAPPDATA\Programs\${INSTALL_DIR_NAME}"
  ${EndIf}
FunctionEnd

Function un.onInit
  !insertmacro _PickScope "un."
FunctionEnd

; ============================================================
;                    PATH helpers
;
; We delegate registry writes to PowerShell's
; [Environment]::SetEnvironmentVariable, which uses the .NET registry API.
; That API has no equivalent of NSIS's NSIS_MAX_STRLEN (default 1024), so it
; correctly round-trips PATH values of any length, preserves the
; REG_EXPAND_SZ type, and broadcasts WM_SETTINGCHANGE automatically.
;
; A native NSIS ReadRegStr would silently truncate the existing PATH at
; NSIS_MAX_STRLEN, and writing that truncated value back would wipe
; everything past the cutoff -- exactly the bug that motivated this rewrite.
;
; Comparisons go through Normalize-PathEntry so that
;   "C:\Foo\\Bar" and "C:\Foo\Bar\" and "C:\Foo\Bar"
; all collapse to the same key. Without this, a malformed legacy entry left
; by an earlier buggy install would (a) not be detected as a duplicate on
; reinstall and (b) not be cleaned up on uninstall.
;
; The PS script is written to $PLUGINSDIR (auto-cleaned on exit) as UTF-16 LE
; with BOM so PowerShell 5.1 detects the encoding correctly.
;
; Macro emits the shared script header into FILEHANDLE: defines $dir / $scope,
; the Normalize-PathEntry function, $dirN (normalized $dir), and $p (current
; PATH). Inside the FileWriteUTF16LE strings, the regex pattern '\\+' is
; intentional -- it's a PowerShell regex matching one-or-more backslashes,
; NOT an NSIS path. NSIS does not escape \, so the bytes hit the .ps1 file
; verbatim.
; ============================================================

!macro WritePathScriptHeader HANDLE DIR SCOPE
  FileWriteUTF16LE /BOM ${HANDLE} "$$dir = '${DIR}'$\r$\n"
  FileWriteUTF16LE ${HANDLE} "$$scope = '${SCOPE}'$\r$\n"
  FileWriteUTF16LE ${HANDLE} "function Normalize-PathEntry {$\r$\n"
  FileWriteUTF16LE ${HANDLE} "  param([string]$$s)$\r$\n"
  FileWriteUTF16LE ${HANDLE} "  if ([string]::IsNullOrWhiteSpace($$s)) { return '' }$\r$\n"
  FileWriteUTF16LE ${HANDLE} "  $$s = $$s.Trim()$\r$\n"
  FileWriteUTF16LE ${HANDLE} "  if ($$s.StartsWith('\\')) { $$s = '\\' + (($$s.TrimStart('\')) -replace '\\+','\') }$\r$\n"
  FileWriteUTF16LE ${HANDLE} "  else { $$s = $$s -replace '\\+','\' }$\r$\n"
  FileWriteUTF16LE ${HANDLE} "  return $$s.TrimEnd('\')$\r$\n"
  FileWriteUTF16LE ${HANDLE} "}$\r$\n"
  FileWriteUTF16LE ${HANDLE} "$$dirN = Normalize-PathEntry $$dir$\r$\n"
  FileWriteUTF16LE ${HANDLE} "$$p = [Environment]::GetEnvironmentVariable('Path', $$scope)$\r$\n"
!macroend

Function AddToPath
  StrCpy $9 "Machine"
  ${If} $IsAdmin == "0"
    StrCpy $9 "User"
  ${EndIf}

  InitPluginsDir
  StrCpy $8 "$PLUGINSDIR\bnl_addpath.ps1"

  FileOpen $7 $8 w
  !insertmacro WritePathScriptHeader $7 "$INSTDIR" "$9"
  FileWriteUTF16LE $7 "$$already = $$false$\r$\n"
  FileWriteUTF16LE $7 "if ($$p) {$\r$\n"
  FileWriteUTF16LE $7 "  foreach ($$e in $$p -split ';') {$\r$\n"
  FileWriteUTF16LE $7 "    if ((Normalize-PathEntry $$e) -ieq $$dirN) { $$already = $$true; break }$\r$\n"
  FileWriteUTF16LE $7 "  }$\r$\n"
  FileWriteUTF16LE $7 "}$\r$\n"
  FileWriteUTF16LE $7 "if (-not $$already) {$\r$\n"
  FileWriteUTF16LE $7 "  $$new = if ([string]::IsNullOrEmpty($$p)) { $$dirN } else { $$p + ';' + $$dirN }$\r$\n"
  FileWriteUTF16LE $7 "  [Environment]::SetEnvironmentVariable('Path', $$new, $$scope)$\r$\n"
  FileWriteUTF16LE $7 "}$\r$\n"
  FileClose $7

  nsExec::ExecToLog 'powershell.exe -NoProfile -NonInteractive -ExecutionPolicy Bypass -File "$8"'
  Pop $7
  ${If} $7 != "0"
    DetailPrint "Warning: PATH update returned exit code $7. Add $INSTDIR to PATH manually if 'bnl' is not found."
  ${EndIf}
FunctionEnd

Function un.RemoveFromPath
  StrCpy $9 "Machine"
  ${If} $IsAdmin == "0"
    StrCpy $9 "User"
  ${EndIf}

  InitPluginsDir
  StrCpy $8 "$PLUGINSDIR\bnl_rmpath.ps1"

  FileOpen $7 $8 w
  !insertmacro WritePathScriptHeader $7 "$INSTDIR" "$9"
  FileWriteUTF16LE $7 "if ($$p) {$\r$\n"
  FileWriteUTF16LE $7 "  $$kept = @()$\r$\n"
  FileWriteUTF16LE $7 "  foreach ($$e in $$p -split ';') {$\r$\n"
  FileWriteUTF16LE $7 "    if ((Normalize-PathEntry $$e) -ine $$dirN) { $$kept += $$e }$\r$\n"
  FileWriteUTF16LE $7 "  }$\r$\n"
  FileWriteUTF16LE $7 "  [Environment]::SetEnvironmentVariable('Path', ($$kept -join ';'), $$scope)$\r$\n"
  FileWriteUTF16LE $7 "}$\r$\n"
  FileClose $7

  nsExec::ExecToLog 'powershell.exe -NoProfile -NonInteractive -ExecutionPolicy Bypass -File "$8"'
  Pop $7
FunctionEnd

; ============================================================
;                      Sections
; ============================================================

Section "Core (required)" SEC_CORE
  SectionIn RO

  SetOutPath "$INSTDIR"

  !if "${ARCH}" == "x64"
    File /oname=bnl.exe "..\..\build\windows-release\bin\bnl.exe"
  !else
    File /oname=bnl.exe "..\..\build\windows-x86-release\bin\bnl.exe"
  !endif

  File /nonfatal "..\..\resources\README.txt"
  File /nonfatal "..\..\resources\LICENSE.txt"

  WriteUninstaller "$INSTDIR\Uninstall.exe"

  WriteRegStr   SHCTX "Software\Microsoft\Windows\CurrentVersion\Uninstall\${PRODUCT_UNINST_KEY}" "DisplayName"     "${PRODUCT_NAME} (${ARCH})"
  WriteRegStr   SHCTX "Software\Microsoft\Windows\CurrentVersion\Uninstall\${PRODUCT_UNINST_KEY}" "Publisher"        "${PRODUCT_PUBLISHER}"
  WriteRegStr   SHCTX "Software\Microsoft\Windows\CurrentVersion\Uninstall\${PRODUCT_UNINST_KEY}" "DisplayVersion"   "${PRODUCT_VERSION}"
  WriteRegStr   SHCTX "Software\Microsoft\Windows\CurrentVersion\Uninstall\${PRODUCT_UNINST_KEY}" "InstallLocation"  "$INSTDIR"
  WriteRegStr   SHCTX "Software\Microsoft\Windows\CurrentVersion\Uninstall\${PRODUCT_UNINST_KEY}" "DisplayIcon"      "$INSTDIR\bnl.exe"
  WriteRegStr   SHCTX "Software\Microsoft\Windows\CurrentVersion\Uninstall\${PRODUCT_UNINST_KEY}" "UninstallString"  "$\"$INSTDIR\Uninstall.exe$\""
  WriteRegDWORD SHCTX "Software\Microsoft\Windows\CurrentVersion\Uninstall\${PRODUCT_UNINST_KEY}" "NoModify"         1
  WriteRegDWORD SHCTX "Software\Microsoft\Windows\CurrentVersion\Uninstall\${PRODUCT_UNINST_KEY}" "NoRepair"         1

  Call AddToPath
SectionEnd

; -------------------------
; Uninstaller
; -------------------------
Section "Uninstall"
  Call un.RemoveFromPath

  Delete /REBOOTOK "$INSTDIR\bnl.exe"
  Delete /REBOOTOK "$INSTDIR\README.txt"
  Delete /REBOOTOK "$INSTDIR\LICENSE.txt"
  Delete /REBOOTOK "$INSTDIR\Uninstall.exe"
  RMDir /r "$INSTDIR"
  DeleteRegKey SHCTX "Software\Microsoft\Windows\CurrentVersion\Uninstall\${PRODUCT_UNINST_KEY}"
SectionEnd

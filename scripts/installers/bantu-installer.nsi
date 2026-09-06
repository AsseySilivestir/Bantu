; ════════════════════════════════════════════════════════════════════
;   Bantu Programming Language — v1.3.1 Windows Installer (NSIS 3.x)
;   ───────────────────────────────────────────────────────────────────
;   Produces:   Bantu-v1.3.1-windows-x64-setup.exe
;
;   Highlights
;   ───────────
;   • Per-user install (no admin / UAC prompt): $INSTDIR = $LOCALAPPDATA\Bantu
;   • Embedded brand icon (bantu.ico) — 6 resolutions (16/32/48/64/128/256)
;   • Bundles the v1.3.1 interpreter + runtime DLLs + standard library
;   • Adds $INSTDIR\bin to user PATH (idempotent, removed on uninstall)
;   • Start Menu shortcuts with the brand icon
;   • Optional .b file association — double-click → bantu run
;   • Add/Remove Programs entry with version + size + icon
;   • Full uninstaller
;   • Live linting in VS Code: install the bundled .vsix and syntax errors
;     show as red squiggles as you type (powered by `bantu lint --json`)
; ════════════════════════════════════════════════════════════════════

# Runtime switches
Unicode true
ManifestDPIAware true
SetCompressor /SOLID lzma
SetCompressorDictSize 64
RequestExecutionLevel user

# Build-time variables
!define APPNAME       "Bantu"
!define APPFULLNAME   "Bantu Programming Language"
!define VERSION       "1.3.1"
!define PUBLISHER     "Bantu Project"
!define URL           "https://github.com/AsseySilivestir/Bantu"
!define UNINST_KEY    "Software\Microsoft\Windows\CurrentVersion\Uninstall\${APPNAME}"

# ─── Includes ──────────────────────────────────────────────────────
!include "MUI2.nsh"
!include "LogicLib.nsh"
!include "FileFunc.nsh"
!include "nsis-include\EnvVarUpdate.nsh"

# ─── Output ────────────────────────────────────────────────────────
Name "${APPFULLNAME} ${VERSION}"
OutFile "Bantu-v${VERSION}-windows-x64-setup.exe"
InstallDir "$LOCALAPPDATA\${APPNAME}"
InstallDirRegKey HKCU "Software\${APPNAME}" "InstallDir"
ShowInstDetails show
ShowUnInstDetails show
BrandingText "${APPFULLNAME} v${VERSION}"

# ─── The brand icon ────────────────────────────────────────────────
# Shows in: installer wizard title bar, Add/Remove Programs, Start Menu
# shortcuts, .b file association icon, the .exe file in Explorer.
Icon "bantu.ico"
UninstallIcon "bantu.ico"

# ─── Welcome / Finish page banner (the big image on the left) ──────
# Required: 164×314 .bmp, 24-bit, no alpha. Shows on the Welcome and
# Finish pages of the wizard.
!define MUI_WELCOMEFINISHPAGE_BITMAP "bantu-welcome.bmp"

# ─── Header bitmap (smaller image, top-right of inner pages) ──────
# Required: 150×57 .bmp. Shows on the Components / Directory /
# InstallFiles pages.
!define MUI_HEADERIMAGE
!define MUI_HEADERIMAGE_BITMAP "bantu-header.bmp"
!define MUI_HEADERIMAGE_RIGHT

# ─── Version info embedded in the .exe (right-click → Properties) ─
VIProductVersion "1.3.1.0"
VIFileVersion    "1.3.1.0"
VIAddVersionKey "ProductName"      "${APPFULLNAME}"
VIAddVersionKey "ProductVersion"   "${VERSION}"
VIAddVersionKey "FileVersion"      "${VERSION}"
VIAddVersionKey "CompanyName"      "${PUBLISHER}"
VIAddVersionKey "LegalCopyright"   "© Bantu Project — MIT License"
VIAddVersionKey "FileDescription"  "${APPFULLNAME} v${VERSION} Setup"
VIAddVersionKey "OriginalFilename" "Bantu-v${VERSION}-windows-x64-setup.exe"

# ─── MUI pages ─────────────────────────────────────────────────────
!define MUI_ABORTWARNING
!define MUI_FINISHPAGE_RUN_TEXT "Launch Bantu REPL"
!define MUI_FINISHPAGE_RUN "$INSTDIR\bin\bantu.exe"
!define MUI_FINISHPAGE_SHOWREADME "$INSTDIR\docs\QUICKSTART.md"
!define MUI_FINISHPAGE_SHOWREADME_NOTCHECKED
!define MUI_FINISHPAGE_LINK "Visit Bantu on GitHub →"
!define MUI_FINISHPAGE_LINK_LOCATION "${URL}"
!define MUI_FINISHPAGE_NOREBOUNNOWARNING

!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_LICENSE "license.txt"
!insertmacro MUI_PAGE_COMPONENTS
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH

!insertmacro MUI_UNPAGE_WELCOME
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_UNPAGE_FINISH

# ─── Languages ─────────────────────────────────────────────────────
!insertmacro MUI_LANGUAGE "English"

# ─── Sections ──────────────────────────────────────────────────────
Section "-Core" SecCore
  SectionIn RO
  SetOutPath "$INSTDIR"
  File "stage\bantu.exe"
  File "stage\bantu-vscode-1.3.1.vsix"

  SetOutPath "$INSTDIR\bin"
  File "stage\bin\bantu.exe"
  File "stage\bin\sqlite3.dll"
  File "stage\bin\libcurl-x64.dll"
  File "stage\bin\curl-ca-bundle.crt"

  SetOutPath "$INSTDIR\dlls"
  File "stage\dlls\sqlite3.dll"
  File "stage\dlls\libcurl-x64.dll"
  File "stage\dlls\curl-ca-bundle.crt"

  ; Write the install dir to the registry (for upgrades + uninstaller)
  WriteRegStr HKCU "Software\${APPNAME}" "InstallDir" "$INSTDIR"
  WriteRegStr HKCU "Software\${APPNAME}" "Version"   "${VERSION}"

  ; Create the uninstaller
  WriteUninstaller "$INSTDIR\Uninstall.exe"

  ; Register Add/Remove Programs entry (with brand icon)
  WriteRegStr   HKCU "${UNINST_KEY}" "DisplayName"     "${APPFULLNAME} ${VERSION}"
  WriteRegStr   HKCU "${UNINST_KEY}" "DisplayVersion"  "${VERSION}"
  WriteRegStr   HKCU "${UNINST_KEY}" "Publisher"        "${PUBLISHER}"
  WriteRegStr   HKCU "${UNINST_KEY}" "DisplayIcon"     "$INSTDIR\bin\bantu.exe"
  WriteRegStr   HKCU "${UNINST_KEY}" "URLInfoAbout"    "${URL}"
  WriteRegStr   HKCU "${UNINST_KEY}" "InstallLocation" "$INSTDIR"
  WriteRegStr   HKCU "${UNINST_KEY}" "UninstallString" "$\"$INSTDIR\Uninstall.exe$\""
  WriteRegDWORD HKCU "${UNINST_KEY}" "NoModify" 1
  WriteRegDWORD HKCU "${UNINST_KEY}" "NoRepair" 1

  ; Compute installed size for ARP view (KB)
  ${GetSize} "$INSTDIR" "/S=0K" $0 $1 $2
  IntFmt $0 "0x%08X" $0
  WriteRegDWORD HKCU "${UNINST_KEY}" "EstimatedSize" "$0"
SectionEnd

Section "Standard library (hash, crypto, uuid, random, orm)" SecStdLib
  SetOutPath "$INSTDIR\lib\hash"
  File "stage\lib\hash\hash.b"
  File "stage\lib\hash\package.json"

  SetOutPath "$INSTDIR\lib\crypto"
  File "stage\lib\crypto\crypto.b"
  File "stage\lib\crypto\package.json"

  SetOutPath "$INSTDIR\lib\uuid"
  File "stage\lib\uuid\uuid.b"
  File "stage\lib\uuid\package.json"

  SetOutPath "$INSTDIR\lib\random"
  File "stage\lib\random\random.b"
  File "stage\lib\random\package.json"

  SetOutPath "$INSTDIR\lib\orm"
  File "stage\lib\orm\orm.b"
  File "stage\lib\orm\package.json"
SectionEnd

Section "Example programs" SecExamples
  SetOutPath "$INSTDIR\examples"
  File "stage\examples\*.b"
SectionEnd

Section "Documentation" SecDocs
  SetOutPath "$INSTDIR\docs"
  File "stage\docs\README.md"
  File "stage\docs\CHANGELOG.md"
  File "stage\docs\QUICKSTART.md"
  File "stage\docs\LICENSE"
SectionEnd

Section "Add Bantu to PATH" SecPath
  ${AddToUserPath} $0 "$INSTDIR\bin"
SectionEnd

Section "Start Menu shortcuts" SecShortcut
  CreateDirectory "$SMPROGRAMS\${APPNAME}"
  CreateShortcut  "$SMPROGRAMS\${APPNAME}\Bantu REPL.lnk" \
                  "$INSTDIR\bin\bantu.exe" "" \
                  "$INSTDIR\bin\bantu.exe" 0
  CreateShortcut  "$SMPROGRAMS\${APPNAME}\Uninstall Bantu.lnk" \
                  "$INSTDIR\Uninstall.exe" "" \
                  "$INSTDIR\Uninstall.exe" 0
  CreateShortcut  "$SMPROGRAMS\${APPNAME}\Bantu Documentation.lnk" \
                  "$INSTDIR\docs\README.md" "" "" 0
SectionEnd

Section "Associate .b files with Bantu" SecAssoc
  ; .b file association — uses the brand icon
  WriteRegStr HKCU "Software\Classes\.b"            "" "Bantu.Source"
  WriteRegStr HKCU "Software\Classes\.b"            "Content Type" "text/plain"
  WriteRegStr HKCU "Software\Classes\Bantu.Source"  "" "Bantu Source File"
  WriteRegStr HKCU "Software\Classes\Bantu.Source\DefaultIcon" "" "$INSTDIR\bin\bantu.exe,0"
  WriteRegStr HKCU "Software\Classes\Bantu.Source\shell\open\command" "" '$\"$INSTDIR\bin\bantu.exe$\" run $\"%1$\"'
SectionEnd

# ─── Section descriptions ─────────────────────────────────────────
LangString DESC_SecCore      ${LANG_ENGLISH} "Bantu v${VERSION} interpreter (bantu.exe) and runtime DLLs. Required."
LangString DESC_SecStdLib    ${LANG_ENGLISH} "Standard library modules: hash (SHA-256 etc.), crypto (HMAC, HKDF), uuid (RFC 4122), random (seedable PRNG), orm (parameterized SQL)."
LangString DESC_SecExamples  ${LANG_ENGLISH} "Sample .b programs — Hello World, fib, webserver, sqlite — to get you started."
LangString DESC_SecDocs      ${LANG_ENGLISH} "README, CHANGELOG, QUICKSTART, and LICENSE."
LangString DESC_SecPath      ${LANG_ENGLISH} "Adds $INSTDIR\bin to your user PATH so you can type 'bantu' from any terminal."
LangString DESC_SecShortcut  ${LANG_ENGLISH} "Creates Start Menu shortcuts for the Bantu REPL and uninstaller."
LangString DESC_SecAssoc     ${LANG_ENGLISH} "Double-click .b files in Explorer to run them with bantu. .b files show the Bantu icon."

!insertmacro MUI_FUNCTION_DESCRIPTION_BEGIN
  !insertmacro MUI_DESCRIPTION_TEXT ${SecCore}     $(DESC_SecCore)
  !insertmacro MUI_DESCRIPTION_TEXT ${SecStdLib}   $(DESC_SecStdLib)
  !insertmacro MUI_DESCRIPTION_TEXT ${SecExamples} $(DESC_SecExamples)
  !insertmacro MUI_DESCRIPTION_TEXT ${SecDocs}     $(DESC_SecDocs)
  !insertmacro MUI_DESCRIPTION_TEXT ${SecPath}     $(DESC_SecPath)
  !insertmacro MUI_DESCRIPTION_TEXT ${SecShortcut} $(DESC_SecShortcut)
  !insertmacro MUI_DESCRIPTION_TEXT ${SecAssoc}    $(DESC_SecAssoc)
!insertmacro MUI_FUNCTION_DESCRIPTION_END

# ─── Install success callback ─────────────────────────────────────
Function .onInstSuccess
  WriteRegStr HKCU "Environment" "BANTU_VERSION" "${VERSION}"
  SendMessage ${HWND_BROADCAST} ${WM_SETTINGCHANGE} 0 "STR:Environment" /TIMEOUT=1000
FunctionEnd

# ─── Uninstaller ──────────────────────────────────────────────────
Function un.onInit
  SetShellVarContext current
FunctionEnd

Section "Uninstall"
  Delete "$INSTDIR\Uninstall.exe"
  Delete "$INSTDIR\bantu.exe"
  Delete "$INSTDIR\bantu-vscode-1.3.1.vsix"

  Delete "$INSTDIR\bin\bantu.exe"
  Delete "$INSTDIR\bin\sqlite3.dll"
  Delete "$INSTDIR\bin\libcurl-x64.dll"
  Delete "$INSTDIR\bin\curl-ca-bundle.crt"
  RMDir "$INSTDIR\bin"

  Delete "$INSTDIR\dlls\sqlite3.dll"
  Delete "$INSTDIR\dlls\libcurl-x64.dll"
  Delete "$INSTDIR\dlls\curl-ca-bundle.crt"
  RMDir "$INSTDIR\dlls"

  Delete "$INSTDIR\lib\hash\hash.b"
  Delete "$INSTDIR\lib\hash\package.json"
  RMDir "$INSTDIR\lib\hash"
  Delete "$INSTDIR\lib\crypto\crypto.b"
  Delete "$INSTDIR\lib\crypto\package.json"
  RMDir "$INSTDIR\lib\crypto"
  Delete "$INSTDIR\lib\uuid\uuid.b"
  Delete "$INSTDIR\lib\uuid\package.json"
  RMDir "$INSTDIR\lib\uuid"
  Delete "$INSTDIR\lib\random\random.b"
  Delete "$INSTDIR\lib\random\package.json"
  RMDir "$INSTDIR\lib\random"
  Delete "$INSTDIR\lib\orm\orm.b"
  Delete "$INSTDIR\lib\orm\package.json"
  RMDir "$INSTDIR\lib\orm"
  RMDir "$INSTDIR\lib"

  Delete "$INSTDIR\examples\*.b"
  RMDir "$INSTDIR\examples"

  Delete "$INSTDIR\docs\README.md"
  Delete "$INSTDIR\docs\CHANGELOG.md"
  Delete "$INSTDIR\docs\QUICKSTART.md"
  Delete "$INSTDIR\docs\LICENSE"
  RMDir "$INSTDIR\docs"

  RMDir "$INSTDIR"

  ; Start Menu shortcuts
  Delete "$SMPROGRAMS\${APPNAME}\Bantu REPL.lnk"
  Delete "$SMPROGRAMS\${APPNAME}\Uninstall Bantu.lnk"
  Delete "$SMPROGRAMS\${APPNAME}\Bantu Documentation.lnk"
  RMDir "$SMPROGRAMS\${APPNAME}"

  ; Registry
  DeleteRegKey HKCU "${UNINST_KEY}"
  DeleteRegKey HKCU "Software\${APPNAME}"
  DeleteRegValue HKCU "Environment" "BANTU_VERSION"

  ; PATH cleanup
  ${RemoveFromUserPath} $0 "$INSTDIR\bin"

  ; File association cleanup
  DeleteRegKey HKCU "Software\Classes\.b"
  DeleteRegKey HKCU "Software\Classes\Bantu.Source"

  SendMessage ${HWND_BROADCAST} ${WM_SETTINGCHANGE} 0 "STR:Environment" /TIMEOUT=1000
SectionEnd

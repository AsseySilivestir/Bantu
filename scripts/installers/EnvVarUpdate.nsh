; ════════════════════════════════════════════════════════════════════
;  EnvVarUpdate.nsh — minimal in-script PATH helpers for NSIS
;  ───────────────────────────────────────────────────────────────────
;  Adds/removes a directory from the user PATH env var, idempotently.
;  Hand-rolled to avoid the dependency on the sourceforge-hosted
;  EnvVarUpdate.nsh (which is not in apt's nsis package).
; ════════════════════════════════════════════════════════════════════

!ifndef ENV_VAR_UPDATE_NSH
!define ENV_VAR_UPDATE_NSH

!include "LogicLib.nsh"

; ${AddToUserPath} $out_var "C:\path\to\add"
!macro _AddToUserPath OUT_VAR DIR
  Push "${DIR}"
  Call AddToUserPath
  Pop "${OUT_VAR}"
!macroend
!define AddToUserPath `!insertmacro _AddToUserPath`

; ${RemoveFromUserPath} $out_var "C:\path\to\remove"
!macro _RemoveFromUserPath OUT_VAR DIR
  Push "${DIR}"
  Call un.RemoveFromUserPath
  Pop "${OUT_VAR}"
!macroend
!define RemoveFromUserPath `!insertmacro _RemoveFromUserPath`

; ── Implementation ────────────────────────────────────────────────
Function AddToUserPath
  Pop $R0   ; dir to add
  ReadRegStr $R1 HKCU "Environment" "PATH"
  ${If} $R1 == ""
    StrCpy $R1 ""
  ${EndIf}

  ; Already present? (check ; delimited)
  Push $R1
  Push ";"
  Push "$R0;"
  Call StrContains
  Pop $R2
  ${If} $R2 != ""
    Push "exists"
    Return
  ${EndIf}
  Push $R1
  Push ";"
  Push ";$R0;"
  Call StrContains
  Pop $R2
  ${If} $R2 != ""
    Push "exists"
    Return
  ${EndIf}
  ${If} $R1 == $R0
    Push "exists"
    Return
  ${EndIf}

  ; Append
  ${If} $R1 == ""
    StrCpy $R1 "$R0"
  ${Else}
    StrCpy $R1 "$R1;$R0"
  ${EndIf}
  WriteRegExpandStr HKCU "Environment" "PATH" "$R1"
  Push "added"
FunctionEnd

Function un.RemoveFromUserPath
  Pop $R0
  ReadRegStr $R1 HKCU "Environment" "PATH"
  ${If} $R1 == ""
    Push "missing"
    Return
  ${EndIf}

  Push $R1
  Push ";$R0;"
  Push ";"
  Call un.StrReplace
  Pop $R1

  Push $R1
  Push "$R0;"
  Push ""
  Call un.StrReplace
  Pop $R1

  Push $R1
  Push ";$R0"
  Push ""
  Call un.StrReplace
  Pop $R1

  Push $R1
  Push "$R0"
  Push ""
  Call un.StrReplace
  Pop $R1

  WriteRegExpandStr HKCU "Environment" "PATH" "$R1"
  Push "removed"
FunctionEnd

; ── String helpers ────────────────────────────────────────────────
Function StrReplace
  Exch $R0   ; replacement
  Exch
  Exch $R1   ; needle
  Exch
  Exch 2
  Exch $R2   ; haystack
  Push $R3
  Push $R4
  Push $R5
  Push $R6
  Push $R7
  Push $R8
  StrLen $R3 $R1
  StrCpy $R4 0
  loop:
    StrCpy $R5 $R2 $R3 $R4
    StrCmp $R5 $R1 found
    StrCmp $R5 "" done
    IntOp $R4 $R4 + 1
    Goto loop
  found:
    StrCpy $R5 $R2 $R4
    IntOp $R6 $R4 + $R3
    StrCpy $R7 $R2 "" $R6
    StrCpy $R2 "$R5$R0$R7"
    IntOp $R4 $R4 + $R3
    Goto loop
  done:
  StrCpy $R0 $R2
  Pop $R8
  Pop $R7
  Pop $R6
  Pop $R5
  Pop $R4
  Pop $R3
  Exch $R0
  Exch
  Pop $R1
  Exch
  Pop $R2
FunctionEnd

Function un.StrReplace
  Exch $R0
  Exch
  Exch $R1
  Exch
  Exch 2
  Exch $R2
  Push $R3
  Push $R4
  Push $R5
  Push $R6
  Push $R7
  Push $R8
  StrLen $R3 $R1
  StrCpy $R4 0
  loop:
    StrCpy $R5 $R2 $R3 $R4
    StrCmp $R5 $R1 found
    StrCmp $R5 "" done
    IntOp $R4 $R4 + 1
    Goto loop
  found:
    StrCpy $R5 $R2 $R4
    IntOp $R6 $R4 + $R3
    StrCpy $R7 $R2 "" $R6
    StrCpy $R2 "$R5$R0$R7"
    IntOp $R4 $R4 + $R3
    Goto loop
  done:
  StrCpy $R0 $R2
  Pop $R8
  Pop $R7
  Pop $R6
  Pop $R5
  Pop $R4
  Pop $R3
  Exch $R0
  Exch
  Pop $R1
  Exch
  Pop $R2
FunctionEnd

Function StrContains
  Exch $R1
  Exch
  Exch $R2
  Push $R3
  Push $R4
  Push $R5
  StrCpy $R3 ""
  StrLen $R4 $R1
  StrCpy $R5 0
  loop:
    StrCpy $R3 $R2 $R4 $R5
    StrCmp $R3 $R1 found
    StrCmp $R3 "" not_found
    IntOp $R5 $R5 + 1
    Goto loop
  found:
    StrCpy $R0 "yes"
    Goto end
  not_found:
    StrCpy $R0 ""
  end:
  Pop $R5
  Pop $R4
  Pop $R3
  Pop $R2
  Pop $R1
  Push $R0
FunctionEnd

!endif  ; ENV_VAR_UPDATE_NSH

; Pre-uninstall: stop the service, prompt for the protection code, verify, then uninstall.
; Called by the CPack/NSIS uninstaller before files are removed.

  ; Stop service first
  DetailPrint "Stopping Orbis Agent service..."
  ExecWait 'sc stop orbis-agent'

  ; Use a VBScript InputBox to collect the uninstall code (single FileWrite line)
  GetTempFileName $R8
  FileOpen $R9 "$R8.vbs" w
  FileWrite $R9 "Dim c : c = InputBox($\"Enter Orbis uninstall code:$\", $\"Orbis Agent$\", $\"\") : WScript.Echo c"
  FileClose $R9

  nsExec::ExecToStack 'wscript //NoLogo "$R8.vbs"'
  Pop $0
  Pop $R0
  Delete "$R8.vbs"
  Delete "$R8"

  ; Trim trailing CR+LF that wscript appends
  StrCpy $R0 $R0 -2

  StrCmp $R0 "" 0 +3
    MessageBox MB_ICONEXCLAMATION "No code entered - uninstall cancelled."
    Abort

  ; Verify the code via the binary (exit 0 = ok, 1 = wrong)
  ExecWait '"$INSTDIR\orbis-agent.exe" --verify-uninstall-code "$R0"' $1
  IntCmp $1 0 orbis_code_ok
    MessageBox MB_ICONEXCLAMATION "Invalid uninstall code. Uninstall aborted."
    Abort
  orbis_code_ok:

  ; Code accepted - unregister the service
  ExecWait '"$INSTDIR\orbis-agent.exe" --uninstall --uninstall-code "$R0"'

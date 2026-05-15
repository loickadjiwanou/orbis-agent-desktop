; Pre-uninstall: stop the service, prompt for the uninstall code, verify, then unregister.
; Called by the CPack/NSIS uninstaller before files are removed.

  ; Stop the service so its binary is not locked during removal
  ExecWait 'sc stop orbis-agent'

  ; Prompt for the protection code via a dialog page (MessageBox/InputBox unavailable
  ; as a built-in; use the agent binary's built-in prompt mode instead).
  ; The agent reads the code interactively when --uninstall is passed without a code.
  ExecWait '"$INSTDIR\orbis-agent.exe" --uninstall'

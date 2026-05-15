; Post-install: register the Orbis Agent Windows service.
; Called by the CPack/NSIS installer after files are copied.
; Non-zero exit (e.g. service already exists) is treated as a warning, not an error.

  DetailPrint "Installing Orbis Agent service..."
  ExecWait '"$INSTDIR\orbis-agent.exe" --install'
  DetailPrint "Orbis Agent service registration complete."

; Post-install: register the Orbis Agent Windows service.
; Called by the CPack/NSIS installer after files are copied.

  DetailPrint "Installing Orbis Agent service..."
  ExecWait '"$INSTDIR\orbis-agent.exe" --install --server "YOUR_SERVER_HOST" --config "$INSTDIR\config.toml"' $0
  IntCmp $0 0 orbis_install_ok
    DetailPrint "Service install returned error $0 (may already be installed)"
  orbis_install_ok:

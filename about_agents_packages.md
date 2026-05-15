# Orbis Agent — Packaging, CI/CD & Security

This document covers three topics:
1. **GitHub Actions CI** — automated build of `.deb` / `.dmg` / `.exe` packages
2. **Windows privilege elevation** — why and how the agent auto-elevates
3. **Uninstall protection** — code/hash mechanism across all platforms

---

## 1. GitHub Actions CI

### File

```
orbis-orbis-agent-desktop/.github/workflows/build-orbis-orbis-agent-desktop.yml
```

> The workflow lives inside `orbis-orbis-agent-desktop/` because that folder has its own git repository. GitHub Actions picks up `.github/workflows/` relative to the repo root.

### Triggers

| Event | Action |
|---|---|
| Push to `main` | Builds all 3 packages |
| Pull request to `main` | Build (verification only) |
| Tag `v*` (e.g. `v1.2.0`) | Build + creates a GitHub Release with all 3 packages as assets |
| `workflow_dispatch` | Manual trigger from the Actions tab |

### Build matrix

| GitHub Runner | Package produced | vcpkg triplet |
|---|---|---|
| `ubuntu-22.04` | `orbis-agent_1.0.0_amd64.deb` | `x64-linux` |
| `macos-13` | `orbis-agent-1.0.0-macos.dmg` | `x64-osx` |
| `windows-2022` | `orbis-agent-1.0.0-windows-x64-setup.exe` | `x64-windows-static` |

### vcpkg caching

C++ dependencies (paho-mqtt, spdlog, openssl, curl, sqlite3, etc.) are compiled once and then cached via the GitHub Actions cache API:

```yaml
env:
  VCPKG_BINARY_SOURCES: "clear;x-gha,readwrite"
```

This avoids recompiling ~20 minutes of dependencies on every push. The cache is automatically invalidated when `vcpkg.json` changes.

### Produced artifacts

- Each job uploads its package as a GitHub Actions artifact (30-day retention)
- On a `v*` tag, the `release` job downloads all artifacts and attaches them to a GitHub Release with auto-generated release notes

---

## 2. Windows Privilege Elevation

### Problem

The Orbis agent must interact with the Windows **Service Control Manager (SCM)** to:
- Register itself as a system service (`CreateService`)
- Start/stop that service (`StartService`, `ControlService`)
- Modify the service configuration

These operations require **Administrator** rights. Without elevation they fail silently.

### Solution: embedded UAC manifest

The file [`packaging/windows/orbis-agent.manifest`](packaging/windows/orbis-agent.manifest) declares the required execution level:

```xml
<requestedExecutionLevel level="requireAdministrator" uiAccess="false"/>
```

This manifest is **compiled and embedded directly into the `.exe` binary** via:

1. **[`packaging/windows/orbis-agent.rc`](packaging/windows/orbis-agent.rc)** — Windows resource file that references the manifest and adds version metadata (name, version, copyright)

2. **`CMakeLists.txt`** — compiles the `.rc` and forces manifest embedding at MSVC link time:
   ```cmake
   target_link_options(orbis-agent PRIVATE
       /MANIFEST:EMBED
       /MANIFESTINPUT:packaging/windows/orbis-agent.manifest
   )
   ```

3. **NSIS installer** — the installer itself also requests elevation:
   ```nsis
   RequestExecutionLevel admin
   ```

### Behaviour by context

| Context | Behaviour |
|---|---|
| Standard user double-clicks the binary | Windows UAC prompt (asks for admin password) |
| Deployed via GPO (enterprise) | Silent elevation, no user interaction |
| Launched by the SCM (service) | Already running as SYSTEM, no prompt |
| GitHub Actions CI/CD | `windows-2022` runner already runs as admin |

**In enterprise environments** (deployed via Intune, SCCM, or GPO), elevation is completely silent — no user interaction required at install time.

---

## 3. Uninstall Protection

### How it works

At install time, the agent hashes a protection code with **SHA-256** and stores it in a root-only file. Any uninstall attempt fails without the correct code.

```
Install   : plaintext_code → SHA-256 → stored in .uninstall_key (root-only)
Uninstall : provided plaintext_code → SHA-256 → compared with .uninstall_key
```

### Key file location

| Platform | Path |
|---|---|
| Linux | `/etc/orbis-agent/.uninstall_key` |
| macOS | `/etc/orbis-agent/.uninstall_key` |
| Windows | `C:\ProgramData\Orbis\.uninstall_key` |

The file is created with `0600` permissions (root read-only on Unix). On Windows, the `ProgramData\Orbis` folder is protected by system ACLs.

### CLI commands

#### Install with a custom code

```bash
# Linux / macOS
sudo orbis-agent --install --server mqtt.mycompany.com --uninstall-code "MySecretCode2024"

# Windows (PowerShell as Administrator)
.\orbis-agent.exe --install --server mqtt.mycompany.com --uninstall-code "MySecretCode2024"
```

> If `--uninstall-code` is omitted, the default code `orbis2026` is used. **It is strongly recommended to set a custom code.**

#### Uninstall

```bash
# Linux / macOS
sudo orbis-agent --uninstall --uninstall-code "MySecretCode2024"

# Windows (PowerShell as Administrator)
.\orbis-agent.exe --uninstall --uninstall-code "MySecretCode2024"
```

Without the correct code:
```
Error: invalid uninstall code.
```

#### Verify the code only (without uninstalling)

```bash
# Returns exit code 0 if correct, 1 if incorrect
orbis-agent --verify-uninstall-code --uninstall-code "MySecretCode2024"
echo $?   # 0 = correct, 1 = incorrect
```

This command is used internally by the Linux `prerm` script and the Windows NSIS dialog.

### Per-platform enforcement

#### Linux (`.deb` package)

The [`packaging/linux/prerm`](packaging/linux/prerm) script runs **before** `dpkg` removes any files.

**Interactive uninstall:**
```bash
sudo dpkg -r orbis-agent
# → Prompt: "Enter uninstall code: " (hidden input)
```

**Scripted / silent uninstall:**
```bash
sudo ORBIS_UNINSTALL_CODE="MySecretCode2024" dpkg -r orbis-agent
```

If the code is missing or incorrect, `prerm` returns an error and `dpkg` cancels the removal.

#### Windows (`.exe` NSIS installer)

The NSIS uninstaller shows a dialog via VBScript (`InputBox`) that asks for the code before proceeding. If the code is wrong, the uninstall is blocked with an error message.

The user cannot bypass this by manually deleting the folder — the Windows service would remain active and be restarted automatically by the SCM (recovery actions configured: restart × 3).

#### macOS

Protection is enforced at the binary level. To cleanly uninstall:
```bash
sudo orbis-agent --uninstall --uninstall-code "MySecretCode2024"
sudo launchctl unload /Library/LaunchDaemons/com.orbis.agent.plist
sudo rm /Library/LaunchDaemons/com.orbis.agent.plist
```

### Technical implementation (C++)

In [`src/main.cpp`](src/main.cpp):

```cpp
// SHA-256 hash via OpenSSL EVP (already a project dependency)
static std::string sha256hex(const std::string& input);

// Save the hash to the protected key file
static bool saveUninstallKey(const std::string& code);

// Compare the provided code against the stored hash
static bool verifyUninstallCode(const std::string& code);
```

The `UNINSTALL_KEY_PATH` constant is defined in [`src/constants.hpp`](src/constants.hpp) and varies by platform (`#if defined(PLATFORM_WINDOWS)`, etc.).

---

## 4. Running the Builds

### Option A — GitHub Actions (recommended)

No local dependencies required. GitHub handles everything.

**Automatic build on every push:**
```bash
git add .
git commit -m "feat: add packaging and CI"
git push origin main
```
→ Go to `github.com/<your-orbis-orbis-agent-desktop-repo>/actions` → workflow **"Build Agent Desktop"** → 3 jobs run in parallel.

**Build + official release (downloadable assets for end users):**
```bash
git tag v1.0.0
git push origin v1.0.0
```
→ GitHub Actions builds all 3 packages and automatically creates a Release with the `.deb`, `.dmg`, and `.exe` as assets.

**Manual trigger from the GitHub UI:**
→ Actions → Build Agent Desktop → **"Run workflow"** button (top right)

---

### Option B — Local build

#### Common prerequisites

- [vcpkg](https://github.com/microsoft/vcpkg) installed and bootstrapped
- `VCPKG_ROOT` environment variable pointing to the vcpkg folder
- CMake ≥ 3.20

#### Linux → `.deb`

```bash
cd orbis-orbis-agent-desktop
export VCPKG_ROOT=~/vcpkg

cmake -S . -B build \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE=$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake \
  -DVCPKG_TARGET_TRIPLET=x64-linux \
  -G Ninja

cmake --build build --parallel
cd build && cpack -G DEB
# → orbis-agent_1.0.0_amd64.deb
```

#### macOS → `.dmg`

```bash
cd orbis-orbis-agent-desktop
export VCPKG_ROOT=~/vcpkg

cmake -S . -B build \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE=$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake \
  -DVCPKG_TARGET_TRIPLET=x64-osx \
  -G Ninja

cmake --build build --parallel
cd build && cpack -G DragNDrop
# → orbis-agent-1.0.0-macos.dmg
```

#### Windows → `.exe` (PowerShell as Administrator)

```powershell
cd orbis-orbis-agent-desktop
$env:VCPKG_ROOT = "C:\vcpkg"

cmake -S . -B build `
  -DCMAKE_BUILD_TYPE=Release `
  -DCMAKE_TOOLCHAIN_FILE="$env:VCPKG_ROOT\scripts\buildsystems\vcpkg.cmake" `
  -DVCPKG_TARGET_TRIPLET=x64-windows-static `
  -G "Visual Studio 17 2022" -A x64

cmake --build build --config Release
cd build; cpack -G NSIS -C Release
# → orbis-agent-1.0.0-windows-x64-setup.exe
```

> **Note:** the `.exe` requires MSVC (Visual Studio) and can only be built on Windows. To produce the `.deb` from macOS, use GitHub Actions.

---

## File Summary

| File | Purpose |
|---|---|
| `.github/workflows/build-orbis-orbis-agent-desktop.yml` | GitHub Actions CI/CD pipeline |
| `packaging/linux/orbis-agent.service` | systemd unit |
| `packaging/linux/postinst` | dpkg post-install script |
| `packaging/linux/prerm` | dpkg pre-remove script (code verification) |
| `packaging/linux/postrm` | dpkg post-remove script (purge) |
| `packaging/linux/config.toml.default` | Default config for Linux |
| `packaging/macos/com.orbis.agent.plist` | macOS LaunchDaemon |
| `packaging/macos/config.toml.default` | Default config for macOS |
| `packaging/windows/orbis-agent.manifest` | UAC manifest (`requireAdministrator`) |
| `packaging/windows/orbis-agent.rc` | Resource file (manifest + version info) |
| `packaging/windows/config.toml.default` | Default config for Windows |
| `CMakeLists.txt` | CPack DEB/DMG/NSIS + manifest embedding + install rules |
| `src/constants.hpp` | `UNINSTALL_KEY_PATH`, `DEFAULT_UNINSTALL_CODE` constants |
| `src/main.cpp` | SHA-256 logic, `--uninstall-code`, `--verify-uninstall-code` |

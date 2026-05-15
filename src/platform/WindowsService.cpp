#include "ServiceInstaller.hpp"
#include "constants.hpp"

#include <spdlog/spdlog.h>

#ifdef PLATFORM_WINDOWS
#include <windows.h>
#include <winsvc.h>
#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;

namespace orbis {

// ---------------------------------------------------------------------------
// Windows SCM global state for the service main loop
// ---------------------------------------------------------------------------
static SERVICE_STATUS_HANDLE g_svc_status_handle = nullptr;
static SERVICE_STATUS        g_svc_status{};
static HANDLE                g_stop_event = nullptr;

static void reportStatus(DWORD current_state, DWORD exit_code = NO_ERROR,
                          DWORD wait_hint = 0)
{
    static DWORD checkpoint = 1;
    g_svc_status.dwServiceType             = SERVICE_WIN32_OWN_PROCESS;
    g_svc_status.dwCurrentState            = current_state;
    g_svc_status.dwWin32ExitCode           = exit_code;
    g_svc_status.dwWaitHint                = wait_hint;
    g_svc_status.dwCheckPoint              = (current_state == SERVICE_RUNNING ||
                                              current_state == SERVICE_STOPPED) ? 0 : checkpoint++;
    g_svc_status.dwControlsAccepted        = (current_state == SERVICE_START_PENDING)
                                             ? 0 : SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN;
    SetServiceStatus(g_svc_status_handle, &g_svc_status);
}

static VOID WINAPI svcCtrlHandler(DWORD ctrl) {
    switch (ctrl) {
        case SERVICE_CONTROL_STOP:
        case SERVICE_CONTROL_SHUTDOWN:
            reportStatus(SERVICE_STOP_PENDING, NO_ERROR, 5000);
            SetEvent(g_stop_event);
            break;
        default:
            break;
    }
}

// Entry point called by SCM when starting the service
static VOID WINAPI svcMain(DWORD /*argc*/, LPSTR* /*argv*/) {
    g_svc_status_handle = RegisterServiceCtrlHandlerA(
        constants::SERVICE_NAME, svcCtrlHandler);
    if (!g_svc_status_handle) return;

    g_stop_event = CreateEvent(nullptr, TRUE, FALSE, nullptr);
    reportStatus(SERVICE_START_PENDING, NO_ERROR, 3000);
    reportStatus(SERVICE_RUNNING);

    // The actual agent logic is launched via the normal main() path
    // when the SCM starts the binary.  This function just keeps the
    // service alive until the stop event is signalled.
    WaitForSingleObject(g_stop_event, INFINITE);

    reportStatus(SERVICE_STOPPED);
    CloseHandle(g_stop_event);
}

// ---------------------------------------------------------------------------
// Windows SCM installer
// ---------------------------------------------------------------------------

class WindowsService final : public ServiceInstaller {
public:
    bool install(const std::string& exec_path,
                 const std::string& config_path) override
    {
        // 1. Create config directory
        std::error_code ec;
        fs::create_directories(R"(C:\ProgramData\Orbis)", ec);

        // 2. Copy binary
        const std::string dest_bin = constants::BINARY_INSTALL_PATH;
        fs::copy_file(exec_path, dest_bin, fs::copy_options::overwrite_existing, ec);
        if (ec) {
            spdlog::error("WindowsService: cannot copy binary: {}", ec.message());
            return false;
        }

        // 3. Copy config
        const std::string dest_cfg = constants::DEFAULT_CONFIG_PATH;
        if (!fs::exists(dest_cfg) && !config_path.empty() && fs::exists(config_path)) {
            fs::copy_file(config_path, dest_cfg, ec);
        }

        // 4. Open SCM
        SC_HANDLE scm = OpenSCManager(nullptr, nullptr, SC_MANAGER_ALL_ACCESS);
        if (!scm) {
            spdlog::error("WindowsService: OpenSCManager failed: {}", GetLastError());
            return false;
        }
        struct ScmGuard { SC_HANDLE h; ~ScmGuard() { CloseServiceHandle(h); } } scm_g{scm};

        // Build image path with --config argument
        std::string image_path = "\"" + dest_bin + "\" --config \"" + dest_cfg + "\"";

        // 5. Create service
        SC_HANDLE svc = CreateServiceA(
            scm,
            constants::SERVICE_NAME,
            constants::SERVICE_DISPLAY_NAME,
            SERVICE_ALL_ACCESS,
            SERVICE_WIN32_OWN_PROCESS,
            SERVICE_AUTO_START,
            SERVICE_ERROR_NORMAL,
            image_path.c_str(),
            nullptr, nullptr, nullptr,
            nullptr,  // LocalSystem
            nullptr);

        if (!svc) {
            DWORD err = GetLastError();
            if (err == ERROR_SERVICE_EXISTS) {
                spdlog::warn("WindowsService: service already exists, updating");
                svc = OpenServiceA(scm, constants::SERVICE_NAME, SERVICE_ALL_ACCESS);
                if (svc) {
                    ChangeServiceConfigA(svc, SERVICE_NO_CHANGE, SERVICE_AUTO_START,
                                        SERVICE_NO_CHANGE, image_path.c_str(),
                                        nullptr, nullptr, nullptr, nullptr, nullptr,
                                        constants::SERVICE_DISPLAY_NAME);
                }
            } else {
                spdlog::error("WindowsService: CreateService failed: {}", err);
                return false;
            }
        }
        struct SvcGuard { SC_HANDLE h; ~SvcGuard() { CloseServiceHandle(h); } } svc_g{svc};

        // 6. Set description
        SERVICE_DESCRIPTIONA desc{const_cast<LPSTR>(constants::SERVICE_DESCRIPTION)};
        ChangeServiceConfig2A(svc, SERVICE_CONFIG_DESCRIPTION, &desc);

        // 7. Set recovery actions: restart 3 times
        SERVICE_FAILURE_ACTIONSA fa{};
        SC_ACTION actions[3] = {
            {SC_ACTION_RESTART, 1000},
            {SC_ACTION_RESTART, 1000},
            {SC_ACTION_RESTART, 1000}
        };
        fa.dwResetPeriod = INFINITE;
        fa.cActions      = 3;
        fa.lpsaActions   = actions;
        ChangeServiceConfig2A(svc, SERVICE_CONFIG_FAILURE_ACTIONS, &fa);

        // 8. Start
        if (!StartServiceA(svc, 0, nullptr)) {
            DWORD err = GetLastError();
            if (err != ERROR_SERVICE_ALREADY_RUNNING) {
                spdlog::error("WindowsService: StartService failed: {}", err);
                return false;
            }
        }

        spdlog::info("WindowsService: {} installed and started", constants::SERVICE_NAME);
        return true;
    }

    bool uninstall() override {
        stop();
        SC_HANDLE scm = OpenSCManager(nullptr, nullptr, SC_MANAGER_ALL_ACCESS);
        if (!scm) return false;
        struct ScmGuard { SC_HANDLE h; ~ScmGuard() { CloseServiceHandle(h); } } g{scm};

        SC_HANDLE svc = OpenServiceA(scm, constants::SERVICE_NAME, DELETE);
        if (!svc) return false;
        struct SvcGuard { SC_HANDLE h; ~SvcGuard() { CloseServiceHandle(h); } } sg{svc};

        bool ok = DeleteService(svc) != 0;
        spdlog::info("WindowsService: uninstalled ({})", ok ? "ok" : "failed");
        return ok;
    }

    bool start() override {
        SC_HANDLE scm = OpenSCManager(nullptr, nullptr, SC_MANAGER_ALL_ACCESS);
        if (!scm) return false;
        struct ScmGuard { SC_HANDLE h; ~ScmGuard() { CloseServiceHandle(h); } } g{scm};
        SC_HANDLE svc = OpenServiceA(scm, constants::SERVICE_NAME, SERVICE_START);
        if (!svc) return false;
        struct SvcGuard { SC_HANDLE h; ~SvcGuard() { CloseServiceHandle(h); } } sg{svc};
        return StartServiceA(svc, 0, nullptr) != 0;
    }

    bool stop() override {
        SC_HANDLE scm = OpenSCManager(nullptr, nullptr, SC_MANAGER_ALL_ACCESS);
        if (!scm) return false;
        struct ScmGuard { SC_HANDLE h; ~ScmGuard() { CloseServiceHandle(h); } } g{scm};
        SC_HANDLE svc = OpenServiceA(scm, constants::SERVICE_NAME, SERVICE_STOP);
        if (!svc) return false;
        struct SvcGuard { SC_HANDLE h; ~SvcGuard() { CloseServiceHandle(h); } } sg{svc};
        SERVICE_STATUS st{};
        return ControlService(svc, SERVICE_CONTROL_STOP, &st) != 0;
    }

    bool isRunning() override {
        SC_HANDLE scm = OpenSCManager(nullptr, nullptr, SC_MANAGER_CONNECT);
        if (!scm) return false;
        struct ScmGuard { SC_HANDLE h; ~ScmGuard() { CloseServiceHandle(h); } } g{scm};
        SC_HANDLE svc = OpenServiceA(scm, constants::SERVICE_NAME, SERVICE_QUERY_STATUS);
        if (!svc) return false;
        struct SvcGuard { SC_HANDLE h; ~SvcGuard() { CloseServiceHandle(h); } } sg{svc};
        SERVICE_STATUS st{};
        QueryServiceStatus(svc, &st);
        return st.dwCurrentState == SERVICE_RUNNING;
    }
};

std::unique_ptr<ServiceInstaller> ServiceInstaller::create() {
    return std::make_unique<WindowsService>();
}

} // namespace orbis

#endif // PLATFORM_WINDOWS

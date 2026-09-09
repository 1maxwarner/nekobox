#include <nekobox/sys/windows/PacketFilter.hpp>

#ifdef Q_OS_WIN

#include <nekobox/dataStore/Configs.hpp>
#include <nekobox/sys/windows/guihelper.h>

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QStandardPaths>

#include <WinSock2.h>
#include <windows.h>
#include <shellapi.h>

// The vendor umbrella header establishes the WinSock/NDISAPI/netlib include
// order required by socks_local_router.h.
#include "../../../../3rdparty/packetfilter/socksify/unmanaged.h"

#include <optional>
#include <string>

namespace {

constexpr DWORD kMsiSuccessRebootRequired = 3010;
constexpr DWORD kMsiSuccessRebootInitiated = 1641;

QString userRuntimePath() {
    return QDir(QStandardPaths::writableLocation(
                    QStandardPaths::AppLocalDataLocation))
        .filePath("packetfilter");
}

QString native(const QString &path) {
    return QDir::toNativeSeparators(QDir::cleanPath(path));
}

void setError(QString *error, const QString &message) {
    if (error != nullptr)
        *error = message;
}

std::wstring processPattern(const QString &value) {
    return value.trimmed().toStdWString();
}

} // namespace

namespace Configs_sys {

struct PacketFilterController::NativeState {
    std::unique_ptr<proxy::socks_local_router> router;
};

PacketFilterController::PacketFilterController() = default;

PacketFilterController::~PacketFilterController() {
    stop();
}

bool PacketFilterController::cleanupInstalledRuntime(QString *error) {
    const QStringList paths{
        userRuntimePath(),
        QDir(Configs::GetBasePath()).filePath("packetfilter")};
    for (const auto &path : paths) {
        if (!QDir(path).exists())
            continue;
        if (!QDir(path).removeRecursively()) {
            setError(error, "Cannot remove the Packet Filter runtime directory: " +
                               native(path));
            return false;
        }
    }
    return true;
}

QString PacketFilterController::findDriverInstaller(const QString &directory) const {
    QDir dir(directory);
    const auto installers = dir.entryList(
        {"Windows.Packet.Filter*.msi", "*ndisapi*.msi", "*WinpkFilter*.msi"},
        QDir::Files, QDir::Name);
    return installers.isEmpty() ? QString()
                                : native(dir.filePath(installers.constLast()));
}

bool PacketFilterController::isDriverAvailable(QString *error) const {
    const auto device = CreateFileW(L"\\\\.\\NDISRD", GENERIC_READ | GENERIC_WRITE,
                                    FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (device != INVALID_HANDLE_VALUE) {
        CloseHandle(device);
        return true;
    }
    if (GetLastError() == ERROR_ACCESS_DENIED)
        return true;
    setError(error,
             "Windows Packet Filter driver is not installed or is not running "
             "(NDISRD device is unavailable).");
    return false;
}

bool PacketFilterController::installDriver(const QString &installer,
                                           QString *error) const {
    if (installer.isEmpty()) {
        setError(error,
                 "Windows Packet Filter driver MSI is missing. Put the signed "
                 "NDISAPI MSI in the packetfilter directory and retry.");
        return false;
    }

    const QString logPath = native(QDir(QFileInfo(installer).absolutePath())
                                       .filePath("packetfilter-install.log"));
    SHELLEXECUTEINFOW info{};
    info.cbSize = sizeof(info);
    info.fMask = SEE_MASK_NOCLOSEPROCESS;
    info.lpVerb = L"runas";
    info.lpFile = L"msiexec.exe";
    const auto args = QString("/i \"%1\" /passive /norestart /L*V \"%2\"")
                          .arg(installer, logPath)
                          .toStdWString();
    info.lpParameters = args.c_str();
    info.nShow = SW_HIDE;
    if (!ShellExecuteExW(&info) || info.hProcess == nullptr) {
        setError(error, "Windows refused to install the Packet Filter driver. "
                       "Accept the UAC prompt and retry.");
        return false;
    }
    const auto waitResult = WaitForSingleObject(info.hProcess, 180000);
    DWORD exitCode = ERROR_INSTALL_FAILURE;
    if (waitResult == WAIT_OBJECT_0)
        GetExitCodeProcess(info.hProcess, &exitCode);
    CloseHandle(info.hProcess);

    if (waitResult != WAIT_OBJECT_0 ||
        (exitCode != ERROR_SUCCESS && exitCode != kMsiSuccessRebootRequired &&
         exitCode != kMsiSuccessRebootInitiated)) {
        setError(error, QString("Packet Filter driver installation failed with MSI "
                                "error %1. A detailed log was written to %2.")
                            .arg(exitCode)
                            .arg(logPath));
        return false;
    }
    if (exitCode == kMsiSuccessRebootRequired ||
        exitCode == kMsiSuccessRebootInitiated) {
        setError(error, "The Packet Filter driver was installed, but Windows must "
                       "be restarted before the mode can be enabled.");
        return false;
    }
    return true;
}

void PacketFilterController::renameAdapter() const {
    const auto script = QDir(runtimeDir).filePath("rename_packet_filter.ps1");
    if (!QFileInfo::exists(script))
        return;
    SHELLEXECUTEINFOW info{};
    info.cbSize = sizeof(info);
    info.fMask = SEE_MASK_NOCLOSEPROCESS;
    info.lpVerb = Windows_IsInAdmin() ? nullptr : L"runas";
    info.lpFile = L"powershell.exe";
    const auto args = QString("-NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden "
                              "-File \"%1\"")
                          .arg(native(script))
                          .toStdWString();
    info.lpParameters = args.c_str();
    info.nShow = SW_HIDE;
    if (ShellExecuteExW(&info) && info.hProcess != nullptr) {
        WaitForSingleObject(info.hProcess, 30000);
        CloseHandle(info.hProcess);
    }
}

bool PacketFilterController::startNative(
    int socksPort, const QString &username, const QString &password,
    const QStringList &includedProcesses,
    const QStringList &excludedProcesses, QString *error) {
    try {
        nativeState = std::make_unique<NativeState>();
        nativeState->router = std::make_unique<proxy::socks_local_router>(
            netlib::log::log_level::error, nullptr, nullptr,
            false /* preserve process attribution for elevated/game mode */);

        // Keep LAN bypass behavior from the previous Packet Filter mode.
        nativeState->router->set_bypass_lan();

        std::optional<std::pair<std::string, std::string>> credentials;
        if (!username.isEmpty() && !password.isEmpty())
            credentials = std::make_pair(username.toStdString(), password.toStdString());

        const auto proxyId = nativeState->router->add_socks5_proxy(
            QString("127.0.0.1:%1").arg(socksPort).toStdString(),
            proxy::socks_local_router::supported_protocols::both,
            credentials, proxy::socks_local_router::supported_address_families::all,
            {}, false);
        if (!proxyId) {
            setError(error, "Cannot configure the native Packet Filter SOCKS5 endpoint.");
            nativeState.reset();
            return false;
        }

        if (includedProcesses.isEmpty()) {
            if (!nativeState->router->associate_process_name_to_proxy(L"", *proxyId)) {
                setError(error, "Cannot enable catch-all Packet Filter routing.");
                nativeState.reset();
                return false;
            }
        } else {
            for (const auto &process : includedProcesses) {
                if (!process.trimmed().isEmpty() &&
                    !nativeState->router->associate_process_name_to_proxy(
                        processPattern(process), *proxyId)) {
                    setError(error, "Cannot configure Packet Filter process routing for " + process);
                    nativeState.reset();
                    return false;
                }
            }
        }
        for (const auto &process : excludedProcesses) {
            if (!process.trimmed().isEmpty() &&
                !nativeState->router->exclude_process_name(processPattern(process))) {
                setError(error, "Cannot configure Packet Filter exclusion for " + process);
                nativeState.reset();
                return false;
            }
        }

        if (!nativeState->router->start()) {
            setError(error, "Native Packet Filter failed to start. Verify the NDISAPI driver and run NekoBox as administrator.");
            nativeState.reset();
            return false;
        }
        return true;
    } catch (const std::exception &exception) {
        setError(error, QString("Native Packet Filter initialization failed: %1")
                            .arg(exception.what()));
        nativeState.reset();
        return false;
    }
}

bool PacketFilterController::start(int socksPort, const QString &username,
                                   const QString &password,
                                   const QStringList &includedProcesses,
                                   const QStringList &excludedProcesses,
                                   QString *error) {
    std::lock_guard<std::mutex> lock(processMutex);
    if (running)
        return true;
    if (socksPort <= 0 || socksPort > 65535) {
        setError(error, "Invalid local SOCKS5 port for the Packet Filter.");
        return false;
    }

    runtimeDir = QDir(QCoreApplication::applicationDirPath()).filePath("packetfilter");
    if (!isDriverAvailable(nullptr)) {
        const auto installer = findDriverInstaller(runtimeDir);
        if (!installDriver(installer, error))
            return false;
    }
    renameAdapter();
    if (!isDriverAvailable(error))
        return false;

    running = startNative(socksPort, username, password, includedProcesses,
                          excludedProcesses, error);
    return running;
}

void PacketFilterController::stop() {
    std::lock_guard<std::mutex> lock(processMutex);
    stopUnlocked();
}

bool PacketFilterController::isRunning() const {
    std::lock_guard<std::mutex> lock(processMutex);
    return running;
}

void PacketFilterController::stopUnlocked() {
    if (nativeState && nativeState->router)
        nativeState->router->stop();
    nativeState.reset();
    running = false;
}

} // namespace Configs_sys

#else

namespace Configs_sys {
PacketFilterController::PacketFilterController() = default;
PacketFilterController::~PacketFilterController() = default;
bool PacketFilterController::start(int, const QString &, const QString &,
                                   const QStringList &, const QStringList &,
                                   QString *) { return false; }
void PacketFilterController::stop() {}
bool PacketFilterController::isRunning() const { return false; }
bool PacketFilterController::cleanupInstalledRuntime(QString *) { return false; }
} // namespace Configs_sys

#endif

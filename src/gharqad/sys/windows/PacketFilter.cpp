#include <nekobox/sys/windows/PacketFilter.hpp>

#ifdef Q_OS_WIN

#include <nekobox/dataStore/Configs.hpp>
#include <nekobox/sys/Settings.h>
#include <nekobox/sys/windows/guihelper.h>

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QThread>

#include <shellapi.h>
#include <windows.h>

namespace {

// MSI return codes are kept local so this controller does not need to link
// against the Windows Installer SDK just to classify a reboot requirement.
constexpr DWORD kMsiSuccessRebootRequired = 3010;
constexpr DWORD kMsiSuccessRebootInitiated = 1641;
constexpr DWORD kMsiInstallFailure = 1603;

QString native(const QString &path) {
    return QDir::toNativeSeparators(QDir::cleanPath(path));
}

void setError(QString *error, const QString &message) {
    if (error != nullptr)
        *error = message;
}

} // namespace

namespace Configs_sys {

PacketFilterController::PacketFilterController() = default;

PacketFilterController::~PacketFilterController() {
    stop();
}

QString PacketFilterController::findHelper(const QString &directory) const {
    const auto helper = QDir(directory).filePath("ProxiFyre.exe");
    return QFileInfo::exists(helper) ? native(helper) : QString();
}

QString PacketFilterController::findDriverInstaller(
    const QString &directory) const {
    QDir dir(directory);
    const auto installers = dir.entryList(
        {"Windows.Packet.Filter*.msi", "*ndisapi*.msi"}, QDir::Files,
        QDir::Name);
    return installers.isEmpty() ? QString()
                                : native(dir.filePath(installers.constLast()));
}

QString PacketFilterController::stageRuntime(QString *error) {
    const auto appRuntime = QDir(QCoreApplication::applicationDirPath())
                                .filePath("packetfilter");
    const auto userRuntime = QDir(Configs::GetBasePath()).filePath("packetfilter");
    const auto sourceHelper = findHelper(appRuntime);
    const auto existingHelper = findHelper(userRuntime);

    if (sourceHelper.isEmpty() && existingHelper.isEmpty()) {
        setError(error,
                 "ProxiFyre.exe is missing. Install the NekoBox packet-filter "
                 "runtime beside the application.");
        return {};
    }

    const auto sourceDriver = QDir(appRuntime).filePath("socksify.dll");
    const auto existingDriver = QDir(userRuntime).filePath("socksify.dll");
    if (!QFileInfo::exists(sourceDriver) && !QFileInfo::exists(existingDriver)) {
        setError(error,
                 "socksify.dll is missing from the packet-filter runtime.");
        return {};
    }

    if (!QDir().mkpath(userRuntime)) {
        setError(error, "Cannot create the packet-filter runtime directory: " +
                           native(userRuntime));
        return {};
    }

    // Keep a per-user copy because installed applications are often located in
    // Program Files and cannot write app-config.json beside the executable.
    if (!sourceHelper.isEmpty()) {
        QDir sourceDir(appRuntime);
        QDir destinationDir(userRuntime);
        const auto files = sourceDir.entryList(QDir::Files | QDir::NoDotAndDotDot);
        for (const auto &file : files) {
            if (file == "app-config.json")
                continue;
            const auto source = sourceDir.filePath(file);
            const auto destination = destinationDir.filePath(file);
            if (QFileInfo::exists(destination) && !QFile::remove(destination)) {
                setError(error, "Cannot replace packet-filter runtime file: " +
                                   native(file));
                return {};
            }
            if (!QFile::copy(source, destination)) {
                setError(error, "Cannot stage packet-filter runtime file: " +
                                   native(file));
                return {};
            }
        }
    }

    const auto helper = findHelper(userRuntime);
    if (helper.isEmpty()) {
        setError(error, "Cannot stage ProxiFyre.exe in the writable runtime directory.");
        return {};
    }
    runtimeDir = userRuntime;
    return helper;
}

bool PacketFilterController::writeConfig(
    const QString &directory, int socksPort,
    const QString &username, const QString &password,
    const QStringList &excludedProcesses, QString *error) {
    QJsonObject proxy{
        {"appNames", QJsonArray{QString()}},
        {"socks5ProxyEndpoint", QString("127.0.0.1:%1").arg(socksPort)},
        {"socks5Transport", "TCP"},
        {"supportedProtocols", QJsonArray{"TCP", "UDP"}},
        {"supportedAddressFamilies", QJsonArray{"IPv4", "IPv6"}},
    };
    if (!username.isEmpty() && !password.isEmpty()) {
        proxy["username"] = username;
        proxy["password"] = password;
    }

    QJsonArray excludes;
    for (const auto &processName : excludedProcesses) {
        if (!processName.trimmed().isEmpty())
            excludes.append(processName.trimmed());
    }

    QJsonObject config{
        {"logLevel", "Error"},
        {"bypassLan", true},
        {"proxies", QJsonArray{proxy}},
        {"excludes", excludes},
    };

    configPath = QDir(directory).filePath("app-config.json");
    QFile file(configPath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        setError(error, "Cannot write packet-filter configuration: " +
                           native(configPath));
        return false;
    }
    const auto data = QJsonDocument(config).toJson(QJsonDocument::Indented);
    if (file.write(data) != data.size()) {
        setError(error, "Cannot finish writing packet-filter configuration.");
        return false;
    }
    return true;
}

bool PacketFilterController::startElevated(const QString &helper,
                                           const QString &directory,
                                           QString *error) {
    SHELLEXECUTEINFOW executeInfo{};
    executeInfo.cbSize = sizeof(executeInfo);
    executeInfo.fMask = SEE_MASK_NOCLOSEPROCESS;
    executeInfo.lpVerb = L"runas";
    const auto helperWide = helper.toStdWString();
    const auto directoryWide = directory.toStdWString();
    executeInfo.lpFile = helperWide.c_str();
    executeInfo.lpDirectory = directoryWide.c_str();
    executeInfo.nShow = SW_HIDE;

    if (!ShellExecuteExW(&executeInfo) || executeInfo.hProcess == nullptr) {
        setError(error,
                 "Windows refused to start the elevated packet-filter helper. "
                 "Run NekoBox as administrator and try again.");
        return false;
    }

    processHandle = executeInfo.hProcess;
    // ProxiFyre is a console/service host and does not signal readiness. Give
    // the NDIS filter a short window to attach, while still detecting an early
    // failure such as a missing Windows Packet Filter driver.
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < 1200) {
        DWORD exitCode = STILL_ACTIVE;
        if (!GetExitCodeProcess(static_cast<HANDLE>(processHandle),
                                &exitCode) || exitCode != STILL_ACTIVE) {
            setError(error,
                     "The packet-filter helper exited immediately. Install the "
                     "Windows Packet Filter driver and retry.");
            stopUnlocked();
            return false;
        }
        QThread::msleep(100);
    }
    return true;
}

bool PacketFilterController::startNormal(const QString &helper,
                                         const QString &directory,
                                         QString *error) {
    STARTUPINFOW startupInfo{};
    startupInfo.cb = sizeof(startupInfo);
    PROCESS_INFORMATION processInfo{};
    const auto helperWide = helper.toStdWString();
    const auto directoryWide = directory.toStdWString();

    if (!CreateProcessW(helperWide.c_str(), nullptr, nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW, nullptr, directoryWide.c_str(),
                        &startupInfo, &processInfo)) {
        setError(error, QString("Cannot start ProxiFyre (Windows error %1).")
                            .arg(GetLastError()));
        return false;
    }

    CloseHandle(processInfo.hThread);
    processHandle = processInfo.hProcess;
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < 1200) {
        if (WaitForSingleObject(processInfo.hProcess, 100) == WAIT_OBJECT_0) {
            setError(error,
                     "The packet-filter helper exited immediately. Install the "
                     "Windows Packet Filter driver and retry.");
            stopUnlocked();
            return false;
        }
    }
    return true;
}

bool PacketFilterController::isDriverAvailable(QString *error) const {
    // NDISAPI exposes the NDISRD device. Checking it before starting the
    // helper gives a deterministic message instead of silently running with
    // no adapter interception.
    const auto device = CreateFileW(L"\\\\.\\NDISRD", GENERIC_READ | GENERIC_WRITE,
                                    FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (device != INVALID_HANDLE_VALUE) {
        CloseHandle(device);
        return true;
    }
    const auto code = GetLastError();
    if (code == ERROR_ACCESS_DENIED)
        return true;
    setError(error,
             "Windows Packet Filter driver is not installed or is not running "
             "(NDISRD device is unavailable). Install the NDISAPI driver and "
             "retry.");
    return false;
}

bool PacketFilterController::installDriver(const QString &installer,
                                           QString *error) const {
    if (installer.isEmpty()) {
        setError(error,
                 "Windows Packet Filter driver is missing. Install the signed "
                 "NDISAPI package or include its MSI in packetfilter/.");
        return false;
    }

    SHELLEXECUTEINFOW executeInfo{};
    executeInfo.cbSize = sizeof(executeInfo);
    executeInfo.fMask = SEE_MASK_NOCLOSEPROCESS;
    executeInfo.lpVerb = L"runas";
    executeInfo.lpFile = L"msiexec.exe";
    const auto parameters =
        QString("/i \"%1\" /passive /norestart").arg(installer).toStdWString();
    executeInfo.lpParameters = parameters.c_str();
    executeInfo.nShow = SW_HIDE;

    if (!ShellExecuteExW(&executeInfo) || executeInfo.hProcess == nullptr) {
        setError(error,
                 "Windows refused to install the Packet Filter driver. Accept "
                 "the UAC prompt or install the packaged MSI manually.");
        return false;
    }

    const auto waitResult = WaitForSingleObject(executeInfo.hProcess, 180000);
    DWORD exitCode = kMsiInstallFailure;
    if (waitResult == WAIT_OBJECT_0)
        GetExitCodeProcess(executeInfo.hProcess, &exitCode);
    CloseHandle(executeInfo.hProcess);

    if (waitResult != WAIT_OBJECT_0) {
        setError(error, "Timed out while installing the Packet Filter driver.");
        return false;
    }
    if (exitCode == kMsiSuccessRebootRequired ||
        exitCode == kMsiSuccessRebootInitiated) {
        setError(error,
                 "The Packet Filter driver was installed, but Windows must be "
                 "restarted before the mode can be enabled.");
        return false;
    }
    if (exitCode != ERROR_SUCCESS) {
        setError(error,
                 QString("Packet Filter driver installation failed with MSI "
                         "error %1.")
                     .arg(exitCode));
        return false;
    }

    for (int attempt = 0; attempt < 30; ++attempt) {
        if (isDriverAvailable(nullptr))
            return true;
        QThread::msleep(100);
    }
    setError(error,
             "The Packet Filter driver installation completed, but the NDISRD "
             "device did not become available. Restart Windows and retry.");
    return false;
}

bool PacketFilterController::start(int socksPort, const QString &username,
                                   const QString &password,
                                   const QStringList &excludedProcesses,
                                   QString *error) {
    std::lock_guard<std::mutex> lock(processMutex);
    if (isRunningUnlocked())
        return true;
    if (processHandle != nullptr) {
        CloseHandle(static_cast<HANDLE>(processHandle));
        processHandle = nullptr;
    }
    if (socksPort <= 0 || socksPort > 65535) {
        setError(error, "Invalid local SOCKS5 port for the packet filter.");
        return false;
    }

    const auto helper = stageRuntime(error);
    if (helper.isEmpty())
        return false;
    if (!writeConfig(runtimeDir, socksPort, username, password,
                     excludedProcesses, error))
        return false;
    if (!isDriverAvailable(nullptr)) {
        const auto installer = findDriverInstaller(runtimeDir);
        if (!installDriver(installer, error))
            return false;
    }

    if (!Windows_IsInAdmin()) {
        return startElevated(helper, runtimeDir, error);
    }
    return startNormal(helper, runtimeDir, error);
}

void PacketFilterController::stop() {
    std::lock_guard<std::mutex> lock(processMutex);
    stopUnlocked();
}

bool PacketFilterController::isRunning() const {
    std::lock_guard<std::mutex> lock(processMutex);
    return isRunningUnlocked();
}

bool PacketFilterController::isRunningUnlocked() const {
    if (processHandle == nullptr)
        return false;
    DWORD exitCode = 0;
    return GetExitCodeProcess(static_cast<HANDLE>(processHandle), &exitCode) &&
           exitCode == STILL_ACTIVE;
}

void PacketFilterController::stopUnlocked() {
    if (processHandle == nullptr)
        return;
    const auto handle = static_cast<HANDLE>(processHandle);
    DWORD exitCode = STILL_ACTIVE;
    if (GetExitCodeProcess(handle, &exitCode) && exitCode == STILL_ACTIVE) {
        TerminateProcess(handle, 0);
        WaitForSingleObject(handle, 1500);
    }
    CloseHandle(handle);
    processHandle = nullptr;
}

} // namespace Configs_sys

#else

namespace Configs_sys {
PacketFilterController::PacketFilterController() = default;
PacketFilterController::~PacketFilterController() = default;
bool PacketFilterController::start(int, const QString &, const QString &,
                                   const QStringList &, QString *) { return false; }
void PacketFilterController::stop() {}
bool PacketFilterController::isRunning() const { return false; }
} // namespace Configs_sys

#endif

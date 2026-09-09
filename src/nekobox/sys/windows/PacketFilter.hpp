#pragma once

#include <QString>
#include <QStringList>

#ifdef Q_OS_WIN
#include <memory>
#include <mutex>
#endif

namespace Configs_sys {

/**
 * Native Windows packet filter controller. The NDISAPI capture/redirect
 * engine is linked into nekobox.exe; no helper process is started.
 *
 * The backend receives ordinary SOCKS5 traffic from sing-box and redirects
 * packets on physical adapters without changing the system default route.
 */
class PacketFilterController {
public:
    PacketFilterController();
    ~PacketFilterController();

    PacketFilterController(const PacketFilterController &) = delete;
    PacketFilterController &operator=(const PacketFilterController &) = delete;

    bool start(int socksPort, const QString &username, const QString &password,
               const QStringList &includedProcesses,
               const QStringList &excludedProcesses, QString *error = nullptr);
    void stop();
    bool isRunning() const;
    static bool cleanupInstalledRuntime(QString *error = nullptr);

private:
#ifdef Q_OS_WIN
    struct NativeState;
    mutable std::mutex processMutex;
    std::unique_ptr<NativeState> nativeState;
    bool running = false;
    QString runtimeDir;

    QString findDriverInstaller(const QString &directory) const;
    bool installDriver(const QString &installer, QString *error) const;
    void renameAdapter() const;
    bool isDriverAvailable(QString *error) const;
    bool startNative(int socksPort, const QString &username,
                     const QString &password,
                     const QStringList &includedProcesses,
                     const QStringList &excludedProcesses, QString *error);
    void stopUnlocked();
#endif
};

} // namespace Configs_sys

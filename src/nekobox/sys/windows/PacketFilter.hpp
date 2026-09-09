#pragma once

#include <QString>
#include <QStringList>

#ifdef Q_OS_WIN
#include <mutex>
#endif

namespace Configs_sys {

/**
 * Controls the optional Windows Packet Filter/ProxiFyre backend.
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
               const QStringList &excludedProcesses, QString *error = nullptr);
    void stop();
    bool isRunning() const;

private:
#ifdef Q_OS_WIN
    mutable std::mutex processMutex;
    void *processHandle = nullptr;
    QString runtimeDir;
    QString configPath;

    QString stageRuntime(QString *error);
    QString findHelper(const QString &directory) const;
    QString findDriverInstaller(const QString &directory) const;
    bool writeConfig(const QString &directory, int socksPort,
                     const QString &username, const QString &password,
                     const QStringList &excludedProcesses, QString *error);
    bool startElevated(const QString &helper, const QString &directory,
                       QString *error);
    bool startNormal(const QString &helper, const QString &directory,
                     QString *error);
    bool installDriver(const QString &installer, QString *error) const;
    bool isDriverAvailable(QString *error) const;
    bool isRunningUnlocked() const;
    void stopUnlocked();
#endif
};

} // namespace Configs_sys





#include <QThread>
#include <QSet>
#include <nekobox/api/RPC.h>
#include <nekobox/ui/mainwindow_interface.h>
#include <nekobox/stats/connections/connectionLister.hpp>
#include <nekobox/global/GuiUtils.hpp>
#ifdef Q_OS_WIN
#include <nekobox/sys/windows/PacketFilter.hpp>
#endif

namespace {
#ifdef Q_OS_WIN
QString normalizePacketFilterEndpoint(QString endpoint) {
    endpoint = endpoint.trimmed().toLower();
    if (endpoint.startsWith(QLatin1Char('['))) {
        endpoint.remove(0, 1);
        const auto close = endpoint.indexOf(QLatin1Char(']'));
        if (close >= 0)
            endpoint.remove(close, 1);
    }
    return endpoint;
}

void restorePacketFilterProcess(Stats::ConnectionMetadata &connection,
                                const std::vector<Configs_sys::PacketFilterController::Attribution> &events,
                                QSet<int> &usedEvents) {
    const auto processLower = connection.process.toLower();
    if (!connection.process.isEmpty() && !processLower.contains(QStringLiteral("nekobox"))) {
        return;
    }

    int bestIndex = -1;
    qint64 bestAge = 15001;
    for (int index = 0; index < static_cast<int>(events.size()); ++index) {
        if (usedEvents.contains(index))
            continue;
        const auto &event = events[static_cast<std::size_t>(index)];
        if (!event.network.isEmpty() &&
            event.network.compare(connection.network, Qt::CaseInsensitive) != 0) {
            continue;
        }
        if (normalizePacketFilterEndpoint(event.destination) !=
            normalizePacketFilterEndpoint(connection.dest))
            continue;
        const qint64 age = qAbs(connection.createdAtMs - event.timestampMs);
        if (age < bestAge) {
            bestAge = age;
            bestIndex = index;
        }
    }
    if (bestIndex >= 0 && !events[static_cast<std::size_t>(bestIndex)].process.isEmpty()) {
        connection.process = events[static_cast<std::size_t>(bestIndex)].process;
        usedEvents.insert(bestIndex);
    }
}
#endif
}

namespace Stats
{
    ConnectionLister* connection_lister = new ConnectionLister();

    ConnectionLister::ConnectionLister()
    {
        state = std::make_shared<QSet<QString>>();
    }

    void ConnectionLister::ForceUpdate()
    {
        mu.lock();
        update();
        mu.unlock();
    }


    void ConnectionLister::Loop()
    {
        while (true)
        {
            if (stop) return;
            QThread::msleep(1000);

            if (suspend || !Configs::dataStore->connection_statistics) continue;

            mu.lock();
            update();
            mu.unlock();
        }
    }

    void ConnectionLister::update()
    {
        bool ok;
        std::optional<libcore::ListConnectionsResp> resp = API::defaultClient->ListConnections(&ok);
        if (!ok)
        {
            return;
        }

        QMap<QString, ConnectionMetadata> toUpdate;
        QMap<QString, ConnectionMetadata> toAdd;
        QSet<QString> newState;
        QList<ConnectionMetadata> sorted;
        auto conns = resp->connections;
#ifdef Q_OS_WIN
        const auto packetFilterEvents = Configs_sys::PacketFilterController::recentAttributions();
        QSet<int> usedPacketFilterEvents;
#endif
        for (auto conn : conns)
        {
            auto c = ConnectionMetadata();
            c.id = QString::fromUtf8(conn.id.c_str());
            c.createdAtMs = conn.created_at;
            c.dest = QString::fromUtf8(conn.dest.c_str());
            c.upload = conn.upload;
            c.download = conn.download;
            c.domain = QString::fromUtf8(conn.domain.c_str());
            c.network = QString::fromUtf8(conn.network.c_str());
            c.outbound = QString::fromUtf8(conn.outbound.c_str());
            c.process = QString::fromUtf8(conn.process.c_str());
            c.protocol = QString::fromUtf8(conn.protocol.c_str());
#ifdef Q_OS_WIN
            restorePacketFilterProcess(c, packetFilterEvents, usedPacketFilterEvents);
#endif
            if (sort == Default)
            {
                if (state->contains(c.id))
                {
                    toUpdate[c.id] = c;
                } else
                {
                    toAdd[c.id] = c;
                }
            } else
            {
                sorted.append(c);
            }
            newState.insert(c.id);
        }

        *state = std::move(newState);

        if (sort == Default)
        {
            runOnUiThread([=,this] {
                auto m = GetMainWindow();
                m->UpdateConnectionList(toUpdate, toAdd);
            });
        } else
        {
            if (sort == ByDownload)
            {
                std::sort(sorted.begin(), sorted.end(), [=,this](const ConnectionMetadata& a, const ConnectionMetadata& b)
                {
                    if (a.download == b.download) return asc ? a.id > b.id : a.id < b.id;
                    return asc ? a.download < b.download : a.download > b.download;
                });
            }
            if (sort == ByUpload)
            {
                std::sort(sorted.begin(), sorted.end(), [=,this](const ConnectionMetadata& a, const ConnectionMetadata& b)
                {
                   if (a.upload == b.upload) return asc ? a.id > b.id : a.id < b.id;
                   return asc ? a.upload < b.upload : a.upload > b.upload;
                });
            }
            if (sort == ByProcess)
            {
                std::sort(sorted.begin(), sorted.end(), [=,this](const ConnectionMetadata& a, const ConnectionMetadata& b)
                {
                    if (a.process == b.process) return asc ? a.id > b.id : a.id < b.id;
                    return asc ? a.process > b.process : a.process < b.process;
                });
            }
            if (sort == ByOutbound)
            {
                std::sort(sorted.begin(), sorted.end(), [=,this](const ConnectionMetadata& a, const ConnectionMetadata& b)
                    {
                        if (a.outbound == b.outbound) return asc ? a.id > b.id : a.id < b.id;
                        return asc ? a.outbound > b.outbound : a.outbound < b.outbound;
                    });
            }
            if (sort == ByProtocol)
            {
                std::sort(sorted.begin(), sorted.end(), [=,this](const ConnectionMetadata& a, const ConnectionMetadata& b)
                    {
                        if (a.protocol == b.protocol) return asc ? a.id > b.id : a.id < b.id;
                        return asc ? a.protocol > b.protocol : a.protocol < b.protocol;
                    });
            }
            runOnUiThread([=,this] {
                auto m = GetMainWindow();
                m->UpdateConnectionListWithRecreate(sorted);
            });
        }
    }

    void ConnectionLister::stopLoop()
    {
        stop = true;
    }

    void ConnectionLister::setSort(const ConnectionSort newSort)
    {
        if (newSort == ByTraffic)
        {
            if (sort == ByDownload && asc)
            {
                sort = ByUpload;
                asc = false;
                return;
            }
            if (sort == ByUpload && asc)
            {
                sort = ByDownload;
                asc = false;
                return;
            }
            if (sort == ByDownload)
            {
                asc = true;
                return;
            }
            if (sort == ByUpload)
            {
                asc = true;
                return;
            }
            sort = ByDownload;
            asc = false;
            return;
        }
        if (sort == newSort) asc = !asc;
        else
        {
            sort = newSort;
            asc = false;
        }
    }

}

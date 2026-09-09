#pragma once

#include <QJsonArray>
#include <QHash>
#include <QList>
#include <QPixmap>
#include <QRect>
#include <QString>
#include <QStringList>

namespace GameMod {

struct Service {
    QString id;
    QString name;
    QString category;
    QString source;
    QRect iconRect;
    QStringList keywords;
    QStringList aliases;
    QStringList legacyIds;
    QStringList domains;
    int directRuleCount = 0;
    int proxyRuleCount = 0;
};

QList<Service> LoadServices(QString *error = nullptr);
QPixmap LoadIconAtlas(QString *error = nullptr);
QJsonArray BuildRules(const QStringList &enabledServiceIds,
                      const QHash<QString, QString> &serviceOutbounds = {},
                      QString *error = nullptr);
// Returns the deduplicated Windows executable names the enabled services route
// through the given outbound ("proxy" or "direct"). Used to drive the native
// per-application interception in Windows Packet Filter mode.
QStringList ProcessNamesForOutbound(const QStringList &enabledServiceIds,
                                    const QString &outbound,
                                    QString *error = nullptr);
QString CategoryDisplayName(const QString &category);

} // namespace GameMod

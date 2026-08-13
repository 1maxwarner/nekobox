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
QString CategoryDisplayName(const QString &category);

} // namespace GameMod

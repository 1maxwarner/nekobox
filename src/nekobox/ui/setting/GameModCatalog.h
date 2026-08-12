#pragma once

#include <QJsonArray>
#include <QList>
#include <QPixmap>
#include <QRect>
#include <QString>
#include <QStringList>

namespace GameMod {

struct Service {
    QString id;
    QString name;
    QRect iconRect;
    QStringList keywords;
    int directRuleCount = 0;
    int proxyRuleCount = 0;
};

QList<Service> LoadServices(QString *error = nullptr);
QPixmap LoadIconAtlas(QString *error = nullptr);
QJsonArray BuildRules(const QStringList &enabledServiceIds,
                      QString *error = nullptr);

} // namespace GameMod

#include <nekobox/ui/setting/GameModCatalog.h>

#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QObject>
#include <QSet>

#include <nekobox/sys/Settings.h>

namespace GameMod {
namespace {

QJsonObject LoadCatalog(QString *error) {
    QFile file(getResource(QStringLiteral("game_mod/catalog.json")));
    if (!file.open(QIODevice::ReadOnly)) {
        if (error != nullptr)
            *error = QObject::tr("Game Mod catalog is not available");
        return {};
    }

    QJsonParseError parseError;
    const auto document = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        if (error != nullptr)
            *error = QObject::tr("Game Mod catalog is invalid: %1")
                         .arg(parseError.errorString());
        return {};
    }
    return document.object();
}

QStringList StringArray(const QJsonArray &array) {
    QStringList result;
    result.reserve(array.size());
    for (const auto &value : array) {
        const auto text = value.toString();
        if (!text.isEmpty())
            result.append(text);
    }
    return result;
}

} // namespace

QList<Service> LoadServices(QString *error) {
    const auto root = LoadCatalog(error);
    QList<Service> result;
    const auto services = root.value(QStringLiteral("services")).toArray();
    result.reserve(services.size());

    for (const auto &value : services) {
        const auto object = value.toObject();
        const auto icon = object.value(QStringLiteral("icon")).toArray();
        if (icon.size() != 4)
            continue;
        Service service;
        service.id = object.value(QStringLiteral("id")).toString();
        service.name = object.value(QStringLiteral("name")).toString();
        service.iconRect = QRect(icon[0].toInt(), icon[1].toInt(),
                                 icon[2].toInt(), icon[3].toInt());
        service.keywords =
            StringArray(object.value(QStringLiteral("keywords")).toArray());
        service.directRuleCount =
            object.value(QStringLiteral("direct_rule_count")).toInt();
        service.proxyRuleCount =
            object.value(QStringLiteral("proxy_rule_count")).toInt();
        if (!service.id.isEmpty() && !service.name.isEmpty())
            result.append(service);
    }
    return result;
}

QPixmap LoadIconAtlas(QString *error) {
    QPixmap atlas;
    if (!atlas.load(getResource(QStringLiteral("game_mod/icons.png"))) &&
        error != nullptr) {
        *error = QObject::tr("Game Mod icon atlas is not available");
    }
    return atlas;
}

QJsonArray BuildRules(const QStringList &enabledServiceIds, QString *error) {
    if (enabledServiceIds.isEmpty())
        return {};

    const QSet<QString> enabled(enabledServiceIds.cbegin(),
                                enabledServiceIds.cend());
    const auto root = LoadCatalog(error);
    QJsonArray result;
    for (const auto &value : root.value(QStringLiteral("services")).toArray()) {
        const auto service = value.toObject();
        if (!enabled.contains(service.value(QStringLiteral("id")).toString()))
            continue;
        for (const auto &rule : service.value(QStringLiteral("rules")).toArray())
            result.append(rule);
    }
    return result;
}

} // namespace GameMod

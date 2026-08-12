#include <nekobox/ui/setting/GameModCatalog.h>

#include <QFile>
#include <QCoreApplication>
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
            *error = QCoreApplication::translate(
                "GameMod", "Game Mod catalog is not available");
        return {};
    }

    QJsonParseError parseError;
    const auto document = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        if (error != nullptr)
            *error = QCoreApplication::translate(
                         "GameMod", "Game Mod catalog is invalid: %1")
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
        service.category = object.value(QStringLiteral("category")).toString();
        service.source = object.value(QStringLiteral("source")).toString();
        service.iconRect = QRect(icon[0].toInt(), icon[1].toInt(),
                                 icon[2].toInt(), icon[3].toInt());
        service.keywords =
            StringArray(object.value(QStringLiteral("keywords")).toArray());
        service.aliases =
            StringArray(object.value(QStringLiteral("aliases")).toArray());
        for (const auto &ruleValue : object.value(QStringLiteral("rules")).toArray()) {
            const auto rule = ruleValue.toObject();
            service.domains += StringArray(rule.value(QStringLiteral("domain")).toArray());
            service.domains += StringArray(rule.value(QStringLiteral("domain_suffix")).toArray());
        }
        service.domains.removeDuplicates();
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
        *error = QCoreApplication::translate(
            "GameMod", "Game Mod icon atlas is not available");
    }
    return atlas;
}

QJsonArray BuildRules(const QStringList &enabledServiceIds,
                      const QHash<QString, QString> &serviceOutbounds,
                      QString *error) {
    if (enabledServiceIds.isEmpty())
        return {};

    const QSet<QString> enabled(enabledServiceIds.cbegin(),
                                enabledServiceIds.cend());
    const auto root = LoadCatalog(error);
    QJsonArray existingRules;
    QJsonArray supplementalRules;
    for (const auto &value : root.value(QStringLiteral("services")).toArray()) {
        const auto service = value.toObject();
        if (!enabled.contains(service.value(QStringLiteral("id")).toString()))
            continue;
        const auto serviceId = service.value(QStringLiteral("id")).toString();
        for (const auto &ruleValue : service.value(QStringLiteral("rules")).toArray()) {
            auto rule = ruleValue.toObject();
            const bool supplemental =
                rule.take(QStringLiteral("catalog_source")).toString() ==
                QStringLiteral("opencck");
            if (rule.value(QStringLiteral("outbound")).toString() == QStringLiteral("proxy")) {
                const auto outbound = serviceOutbounds.value(serviceId);
                if (!outbound.isEmpty())
                    rule.insert(QStringLiteral("outbound"), outbound);
            }
            (supplemental ? supplementalRules : existingRules).append(rule);
        }
    }
    for (const auto &rule : supplementalRules)
        existingRules.append(rule);
    return existingRules;
}

QString CategoryDisplayName(const QString &category) {
    if (category == QStringLiteral("games"))
        return QCoreApplication::translate("GameMod", "Games");
    if (category == QStringLiteral("streaming"))
        return QCoreApplication::translate("GameMod", "Streaming and media");
    if (category == QStringLiteral("messaging"))
        return QCoreApplication::translate("GameMod", "Messaging");
    if (category == QStringLiteral("social"))
        return QCoreApplication::translate("GameMod", "Social networks");
    if (category == QStringLiteral("ai"))
        return QCoreApplication::translate("GameMod", "AI services");
    if (category == QStringLiteral("education"))
        return QCoreApplication::translate("GameMod", "Education");
    if (category == QStringLiteral("finance"))
        return QCoreApplication::translate("GameMod", "Finance");
    if (category == QStringLiteral("shopping"))
        return QCoreApplication::translate("GameMod", "Shopping");
    if (category == QStringLiteral("creative"))
        return QCoreApplication::translate("GameMod", "Creative tools");
    if (category == QStringLiteral("news"))
        return QCoreApplication::translate("GameMod", "News");
    if (category == QStringLiteral("tools"))
        return QCoreApplication::translate("GameMod", "Tools and platforms");
    return QCoreApplication::translate("GameMod", "Other");
}

} // namespace GameMod

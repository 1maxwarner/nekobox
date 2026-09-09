#include <nekobox/ui/setting/GameModCatalog.h>

#include <QFile>
#include <QCoreApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QObject>
#include <QRegularExpression>
#include <QSet>

#include <nekobox/sys/Settings.h>

namespace GameMod {
namespace {

QJsonObject LoadCatalog(QString *error) {
    struct CachedCatalog {
        QJsonObject root;
        QString error;
    };
    static const CachedCatalog cache = [] {
        CachedCatalog result;
        QFile file(getResource(QStringLiteral("game_mod/catalog.json")));
        if (!file.open(QIODevice::ReadOnly)) {
            result.error = QCoreApplication::translate(
                "GameMod", "Game Mod catalog is not available");
            return result;
        }
        QJsonParseError parseError;
        const auto document = QJsonDocument::fromJson(file.readAll(), &parseError);
        if (parseError.error != QJsonParseError::NoError ||
            !document.isObject()) {
            result.error =
                QCoreApplication::translate(
                    "GameMod", "Game Mod catalog is invalid: %1")
                    .arg(parseError.errorString());
            return result;
        }
        result.root = document.object();
        return result;
    }();
    if (error != nullptr)
        *error = cache.error;
    return cache.root;
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

QString CaseInsensitiveProcessPathPattern(const QString &processName) {
    // sing-box compares process_name literally. ProcessPathRegex is based on
    // the same discovered process path, but supports RE2's inline (?i) flag.
    // Match the executable basename so a Windows path using either slash is
    // accepted, regardless of the casing reported by the operating system.
    QString normalized = processName;
    const auto suffix = QStringLiteral(".exe");
    const bool windowsSuffix = normalized.endsWith(suffix, Qt::CaseInsensitive);
    if (windowsSuffix)
        normalized.chop(suffix.size());

    QString pattern = QStringLiteral("(?i)(?:^|.*[\\\\/])");
    for (const auto character : normalized) {
        if (character == QLatin1Char('*')) {
            pattern += QStringLiteral(".*");
        } else if (character == QLatin1Char('?')) {
            pattern += QLatin1Char('.');
        } else {
            pattern += QRegularExpression::escape(QString(character));
        }
    }
    // The catalog lists Windows executables, but sing-box reports the real
    // image path on every platform. Match the optional ".exe" suffix so the
    // same rule catches both the Windows binary (C:\...\telegram.exe) and the
    // extensionless Linux/macOS binary (/usr/bin/telegram). The regex string
    // must contain a single backslash before the dot ("\.") so RE2 treats it
    // as a literal period; a doubled backslash would demand a real backslash
    // in the path and never match a genuine ".exe" ending.
    pattern += QStringLiteral("(?:\\.exe)?");
    return pattern + QLatin1Char('$');
}

QJsonArray CaseInsensitiveProcessPathPatterns(const QJsonArray &processNames) {
    QJsonArray result;
    QSet<QString> seen;
    for (const auto &value : processNames) {
        const auto processName = value.toString().trimmed();
        if (processName.isEmpty())
            continue;
        const auto pattern = CaseInsensitiveProcessPathPattern(processName);
        if (!seen.contains(pattern)) {
            seen.insert(pattern);
            result.append(pattern);
        }
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
        service.legacyIds =
            StringArray(object.value(QStringLiteral("legacy_ids")).toArray());
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
    static const QPixmap atlas = [] {
        QPixmap result;
        result.load(getResource(QStringLiteral("game_mod/icons.png")));
        return result;
    }();
    if (atlas.isNull() && error != nullptr) {
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
    QJsonArray result;
    for (const auto &value : root.value(QStringLiteral("services")).toArray()) {
        const auto service = value.toObject();
        const auto serviceId = service.value(QStringLiteral("id")).toString();
        const auto legacyIds =
            StringArray(service.value(QStringLiteral("legacy_ids")).toArray());
        QString matchedId;
        if (enabled.contains(serviceId)) {
            matchedId = serviceId;
        } else {
            for (const auto &legacyId : legacyIds) {
                if (enabled.contains(legacyId)) {
                    matchedId = legacyId;
                    break;
                }
            }
        }
        if (matchedId.isEmpty())
            continue;
        for (const auto &ruleValue : service.value(QStringLiteral("rules")).toArray()) {
            auto rule = ruleValue.toObject();
            rule.remove(QStringLiteral("catalog_source"));
            const auto processNames = rule.value(QStringLiteral("process_name")).toArray();
            if (!processNames.isEmpty()) {
                // The catalog keeps the canonical executable names for search
                // and display. At runtime emit a case-insensitive basename
                // matcher, so telegram.exe also catches Telegram.exe,
                // TELEGRAM.EXE, and every other casing.
                rule.remove(QStringLiteral("process_name"));
                rule.insert(QStringLiteral("process_path_regex"),
                            CaseInsensitiveProcessPathPatterns(processNames));
            }
            if (rule.value(QStringLiteral("outbound")).toString() == QStringLiteral("proxy")) {
                auto outbound = serviceOutbounds.value(serviceId);
                if (outbound.isEmpty())
                    outbound = serviceOutbounds.value(matchedId);
                if (!outbound.isEmpty())
                    rule.insert(QStringLiteral("outbound"), outbound);
            }
            result.append(rule);
        }
    }
    return result;
}

QStringList ProcessNamesForOutbound(const QStringList &enabledServiceIds,
                                    const QString &outbound, QString *error) {
    if (enabledServiceIds.isEmpty())
        return {};

    const QSet<QString> enabled(enabledServiceIds.cbegin(),
                                enabledServiceIds.cend());
    const auto root = LoadCatalog(error);
    QStringList result;
    QSet<QString> seen;
    for (const auto &value : root.value(QStringLiteral("services")).toArray()) {
        const auto service = value.toObject();
        const auto serviceId = service.value(QStringLiteral("id")).toString();
        const auto legacyIds =
            StringArray(service.value(QStringLiteral("legacy_ids")).toArray());
        bool matched = enabled.contains(serviceId);
        for (auto it = legacyIds.cbegin(); !matched && it != legacyIds.cend();
             ++it)
            matched = enabled.contains(*it);
        if (!matched)
            continue;
        for (const auto &ruleValue :
             service.value(QStringLiteral("rules")).toArray()) {
            const auto rule = ruleValue.toObject();
            if (rule.value(QStringLiteral("outbound")).toString() != outbound)
                continue;
            for (const auto &name :
                 rule.value(QStringLiteral("process_name")).toArray()) {
                const auto processName = name.toString().trimmed();
                // The native packet filter matches process names case-insensitively,
                // so the executable basename works regardless of install directory.
                if (!processName.isEmpty() &&
                    !seen.contains(processName.toCaseFolded())) {
                    seen.insert(processName.toCaseFolded());
                    result.append(processName);
                }
            }
        }
    }
    return result;
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

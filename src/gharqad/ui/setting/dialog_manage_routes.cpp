#include <nekobox/ui/setting/dialog_manage_routes.h>
#include <nekobox/ui/setting/GameModCatalog.h>
#include <nekobox/configs/warp/warp.hpp>
#include <nekobox/configs/proxy/WireguardBean.h>

#include <QClipboard>
#include <QAbstractListModel>
#include <QAbstractItemView>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QIcon>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QListWidget>
#include <QPainter>
#include <QPersistentModelIndex>
#include <QPointer>
#include <QPushButton>
#include <QRegularExpression>
#include <QSet>
#include <QSignalBlocker>
#include <QSortFilterProxyModel>
#include <QSplitter>
#include <QStyle>
#include <QTabWidget>
#include <QToolButton>
#include <QVBoxLayout>
#include <algorithm>
#include <memory>
#include <utility>

#include <nekobox/dataStore/Database.hpp>

#include <3rdparty/qv2ray/v2/ui/widgets/editors/w_JsonEditor.hpp>
#include <nekobox/global/GuiUtils.hpp>
#include <nekobox/global/CountryHelper.hpp>
#include <nekobox/configs/proxy/Preset.hpp>

#include <QFile>
#include <QMessageBox>
#include <QShortcut>
#include <QTimer>
#include <QToolTip>
#include <nekobox/api/RPC.h>
#include <nekobox/ui/mainwindow.h>

#include <QtGlobal> // For QT_VERSION_CHECK
#if QT_VERSION >= QT_VERSION_CHECK(6, 9, 0)
#define STATE_CHANGED &QCheckBox::checkStateChanged
#else
#define STATE_CHANGED &QCheckBox::stateChanged
#endif

namespace {
enum GameModItemRole {
    ServiceIdRole = Qt::UserRole,
    SearchRole,
    CompactSearchRole,
    CategoryRole,
    ProfileIdRole,
    ActiveRole,
    BaseIconRole,
    DisplayNameRole,
    DetailTextRole,
};

QString NormalizeGameModSearch(QString value) {
    value = value.toCaseFolded();
    value.replace(QRegularExpression(QStringLiteral("[^\\p{L}\\p{N}]+")),
                  QStringLiteral(" "));
    return value.simplified();
}

bool SmartGameModMatch(const QString &corpus, const QString &compactCorpus,
                       const QString &normalizedQuery,
                       const QString &compactQuery,
                       const QStringList &queryTokens) {
    if (normalizedQuery.isEmpty())
        return true;
    if (corpus.contains(normalizedQuery) || compactCorpus.contains(compactQuery))
        return true;
    return std::all_of(queryTokens.cbegin(), queryTokens.cend(),
                       [&corpus](const QString &token) {
                           return corpus.contains(token);
                       });
}

QHash<QString, int> ParseGameModProfileAssignments(const QString &json) {
    QHash<QString, int> result;
    const auto object = QJsonDocument::fromJson(json.toUtf8()).object();
    for (auto it = object.constBegin(); it != object.constEnd(); ++it) {
        if (it.value().isDouble())
            result.insert(it.key(), it.value().toInt(-1));
    }
    return result;
}

QIcon GameModIconWithStatus(const QIcon &base, bool active) {
    if (!active)
        return base;
    auto pixmap = base.pixmap(QSize(40, 40));
    if (pixmap.isNull())
        pixmap = QPixmap(40, 40);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(QPen(QApplication::palette().base().color(), 2));
    painter.setBrush(QColor(QStringLiteral("#2ecc71")));
    painter.drawEllipse(QRectF(28, 28, 10, 10));
    return QIcon(pixmap);
}

QString GameModProfileLabel(int profileId, bool compact = false) {
    auto profile = Configs::profileManager->GetProfile(profileId);
    if (profile == nullptr)
        return QCoreApplication::translate("DialogManageRoutes",
                                           "Current active configuration");
    QString label;
    if (!profile->test_country.isEmpty()) {
        const auto countryCode = profile->test_country.size() == 2
                                     ? profile->test_country
                                     : CountryNameToCode(profile->test_country);
        if (countryCode.size() == 2)
            label = CountryCodeToFlag(countryCode);
    }
    if (label.isEmpty() || !compact)
        label += (label.isEmpty() ? QString() : QStringLiteral(" ")) +
                 profile->DisplayName();
    if (profile->latencyInt > 0)
        label += QStringLiteral("  %1 ms").arg(profile->latencyInt);
    else if (compact)
        label += QStringLiteral("  %1").arg(QCoreApplication::translate(
            "DialogManageRoutes", "Ping unavailable"));
    return label;
}
} // namespace

struct GameModServiceEntry {
    GameMod::Service service;
    QString displayName;
    QString details;
    QString searchCorpus;
    QString compactSearchCorpus;
    int profileId = -1;
    bool saved = false;
    bool enabled = false;
};

class GameModServiceModel final : public QAbstractListModel {
public:
    GameModServiceModel(const QList<GameMod::Service> &services,
                        const QPixmap &atlas,
                        const QSet<QString> &saved,
                        const QSet<QString> &enabled,
                        const QHash<QString, int> &assignments,
                        QObject *parent)
        : QAbstractListModel(parent), atlas(atlas) {
        entries.reserve(services.size());
        iconCache.resize(services.size());
        for (const auto &service : services) {
            GameModServiceEntry entry;
            entry.service = service;
            entry.displayName = service.name;
            if (entry.displayName.compare(QStringLiteral("faceit"),
                                          Qt::CaseInsensitive) == 0) {
                entry.displayName = QStringLiteral("FACEIT");
            } else if (!entry.displayName.isEmpty()) {
                entry.displayName[0] = entry.displayName.at(0).toUpper();
            }
            entry.details = QCoreApplication::translate(
                                "DialogManageRoutes",
                                "%1 rules  ·  %2 domains")
                                .arg(service.proxyRuleCount +
                                     service.directRuleCount)
                                .arg(service.domains.size());
            entry.searchCorpus = NormalizeGameModSearch(
                QStringList{service.id, service.name, service.category,
                            service.aliases.join(QLatin1Char(' ')),
                            service.keywords.join(QLatin1Char(' ')),
                            service.domains.join(QLatin1Char(' '))}
                    .join(QLatin1Char(' ')));
            entry.compactSearchCorpus =
                QString(entry.searchCorpus).remove(QLatin1Char(' '));
            entry.profileId = assignments.value(service.id, -1);
            if (entry.profileId < 0) {
                for (const auto &legacyId : service.legacyIds) {
                    if (assignments.contains(legacyId)) {
                        entry.profileId = assignments.value(legacyId);
                        break;
                    }
                }
            }
            entry.saved = saved.contains(service.id);
            if (!entry.saved) {
                entry.saved = std::any_of(
                    service.legacyIds.cbegin(), service.legacyIds.cend(),
                    [&saved](const QString &legacyId) {
                        return saved.contains(legacyId);
                    });
            }
            entry.enabled = enabled.contains(service.id);
            if (!entry.enabled) {
                entry.enabled = std::any_of(
                    service.legacyIds.cbegin(), service.legacyIds.cend(),
                    [&enabled](const QString &legacyId) {
                        return enabled.contains(legacyId);
                    });
            }
            entry.saved = entry.saved || entry.enabled;
            entries.append(std::move(entry));
        }
    }

    int rowCount(const QModelIndex &parent = QModelIndex()) const override {
        return parent.isValid() ? 0 : entries.size();
    }

    QVariant data(const QModelIndex &index, int role) const override {
        if (!index.isValid() || index.row() < 0 || index.row() >= entries.size())
            return {};
        const auto &entry = entries.at(index.row());
        switch (role) {
        case Qt::DisplayRole:
            return entry.displayName;
        case Qt::DecorationRole: {
            const auto resolvedProfile =
                entry.profileId >= 0 ? entry.profileId
                                     : Configs::dataStore->started_id;
            const bool active = entry.enabled && resolvedProfile >= 0 &&
                                resolvedProfile == Configs::dataStore->started_id;
            return GameModIconWithStatus(baseIcon(index.row()), active);
        }
        case Qt::CheckStateRole:
            return entry.saved ? Qt::Checked : Qt::Unchecked;
        case Qt::SizeHintRole:
            return QSize(154, 94);
        case Qt::TextAlignmentRole:
            return int(Qt::AlignHCenter | Qt::AlignTop);
        case Qt::ToolTipRole:
            return QCoreApplication::translate(
                       "DialogManageRoutes",
                       "%1\n%2\nExecutables: %3\nDomains: %4")
                .arg(entry.displayName)
                .arg(entry.details)
                .arg(entry.service.keywords.mid(0, 16).join(
                    QStringLiteral(", ")))
                .arg(entry.service.domains.mid(0, 16).join(
                    QStringLiteral(", ")));
        case ServiceIdRole:
            return entry.service.id;
        case SearchRole:
            return entry.searchCorpus;
        case CompactSearchRole:
            return entry.compactSearchCorpus;
        case CategoryRole:
            return entry.service.category;
        case ProfileIdRole:
            return entry.profileId;
        case ActiveRole:
            return entry.enabled;
        case BaseIconRole:
            return baseIcon(index.row());
        case DisplayNameRole:
            return entry.displayName;
        case DetailTextRole:
            return entry.details;
        default:
            return {};
        }
    }

    bool setData(const QModelIndex &index, const QVariant &value,
                 int role) override {
        if (!index.isValid() || index.row() < 0 || index.row() >= entries.size())
            return false;
        auto &entry = entries[index.row()];
        if (role == Qt::CheckStateRole) {
            const bool saved = value.toInt() == Qt::Checked;
            if (entry.saved == saved)
                return false;
            entry.saved = saved;
            // Adding from the library keeps the familiar behavior of making
            // the service active immediately. Removing also stops routing it.
            entry.enabled = saved;
            emit dataChanged(index, index,
                             {Qt::CheckStateRole, ActiveRole,
                              Qt::DecorationRole});
            return true;
        }
        if (role == ActiveRole) {
            const bool enabled = value.toBool();
            if (entry.enabled == enabled)
                return false;
            entry.enabled = enabled;
            entry.saved = entry.saved || enabled;
            emit dataChanged(index, index,
                             {Qt::CheckStateRole, ActiveRole,
                              Qt::DecorationRole});
            return true;
        }
        if (role == ProfileIdRole) {
            const auto profileId = value.toInt();
            if (entry.profileId == profileId)
                return false;
            entry.profileId = profileId;
            emit dataChanged(index, index,
                             {ProfileIdRole, Qt::DecorationRole});
            return true;
        }
        return false;
    }

    Qt::ItemFlags flags(const QModelIndex &index) const override {
        if (!index.isValid())
            return Qt::NoItemFlags;
        return Qt::ItemIsEnabled | Qt::ItemIsSelectable |
               Qt::ItemIsUserCheckable;
    }

    int enabledCount() const {
        return std::count_if(entries.cbegin(), entries.cend(),
                             [](const auto &entry) { return entry.enabled; });
    }

    int savedCount() const {
        return std::count_if(entries.cbegin(), entries.cend(),
                             [](const auto &entry) { return entry.saved; });
    }

    void setRowsChecked(const QList<int> &rows, bool checked) {
        bool changed = false;
        for (const auto row : rows) {
            if (row < 0 || row >= entries.size() ||
                entries[row].saved == checked)
                continue;
            entries[row].saved = checked;
            entries[row].enabled = checked;
            changed = true;
        }
        if (changed && !entries.isEmpty())
            emit dataChanged(index(0), index(entries.size() - 1),
                             {Qt::CheckStateRole, ActiveRole,
                              Qt::DecorationRole});
    }

    QStringList savedServiceIds() const {
        QStringList result;
        for (const auto &entry : entries) {
            if (entry.saved)
                result.append(entry.service.id);
        }
        return result;
    }

    QStringList enabledServiceIds() const {
        QStringList result;
        for (const auto &entry : entries) {
            if (entry.enabled)
                result.append(entry.service.id);
        }
        return result;
    }

    QJsonObject profileAssignments() const {
        QJsonObject result;
        for (const auto &entry : entries) {
            if (entry.profileId >= 0)
                result.insert(entry.service.id, entry.profileId);
        }
        return result;
    }

    QIcon baseIcon(int row) const {
        if (row < 0 || row >= entries.size())
            return {};
        if (iconCache[row].isNull() && !atlas.isNull() &&
            entries.at(row).service.iconRect.isValid()) {
            iconCache[row] =
                QIcon(atlas.copy(entries.at(row).service.iconRect));
        }
        return iconCache.at(row);
    }

private:
    QList<GameModServiceEntry> entries;
    QPixmap atlas;
    mutable QList<QIcon> iconCache;
};

class GameModFilterProxyModel final : public QSortFilterProxyModel {
public:
    explicit GameModFilterProxyModel(QObject *parent)
        : QSortFilterProxyModel(parent) {
        setDynamicSortFilter(true);
        setSortCaseSensitivity(Qt::CaseInsensitive);
    }

    void setCategory(QString value) {
        if (category == value)
            return;
        category = std::move(value);
        invalidateFilter();
    }

    void setQuery(QString value) {
        value = NormalizeGameModSearch(std::move(value));
        if (normalizedQuery == value)
            return;
        normalizedQuery = std::move(value);
        compactQuery = QString(normalizedQuery).remove(QLatin1Char(' '));
        queryTokens = normalizedQuery.split(QLatin1Char(' '),
                                            Qt::SkipEmptyParts);
        invalidateFilter();
    }

protected:
    bool filterAcceptsRow(int sourceRow,
                          const QModelIndex &sourceParent) const override {
        const auto sourceIndex = sourceModel()->index(sourceRow, 0, sourceParent);
        return (category.isEmpty() ||
                sourceIndex.data(CategoryRole).toString() == category) &&
               SmartGameModMatch(
                   sourceIndex.data(SearchRole).toString(),
                   sourceIndex.data(CompactSearchRole).toString(),
                   normalizedQuery, compactQuery, queryTokens);
    }

    bool lessThan(const QModelIndex &left,
                  const QModelIndex &right) const override {
        return left.data(DisplayNameRole)
                   .toString()
                   .localeAwareCompare(right.data(DisplayNameRole).toString()) < 0;
    }

private:
    QString category;
    QString normalizedQuery;
    QString compactQuery;
    QStringList queryTokens;
};


void DialogManageRoutes::reloadProfileItems() {
    if (chainList.empty()) {
        MessageBoxWarning(tr("Invalid state"), tr("The list of routing profiles is empty, this should be an unreachable state, crashes may occur now"));
        return;
    }

    std::shared_ptr<Configs::RoutingChain> selectedChain;
    const auto oldRow = ui->route_profiles->currentRow();
    if (oldRow >= 0 && oldRow < chainList.size())
        selectedChain = chainList[oldRow];
    if (selectedChain == nullptr)
        selectedChain = currentRoute;

    std::stable_sort(chainList.begin(), chainList.end(),
                     [](const auto &left, const auto &right) {
                         if (left->priority != right->priority)
                             return left->priority < right->priority;
                         return left->id < right->id;
                     });

    QSignalBlocker comboBlocker(ui->route_prof);
    QSignalBlocker listBlocker(ui->route_profiles);
    ui->route_prof->clear();
    ui->route_profiles->clear();

    if (currentRoute == nullptr || !currentRoute->enabled) {
        currentRoute.reset();
        for (const auto &item : chainList) {
            if (item->enabled) {
                currentRoute = item;
                break;
            }
        }
    }

    int selectedListRow = -1;
    int selectedComboRow = -1;
    for (int i = 0; i < chainList.size(); ++i) {
        const auto &item = chainList[i];
        auto *listItem = new QListWidgetItem(
            QStringLiteral("%1  ·  %2").arg(item->priority).arg(item->chain_name),
            ui->route_profiles);
        listItem->setFlags(listItem->flags() | Qt::ItemIsUserCheckable);
        listItem->setCheckState(item->enabled ? Qt::Checked : Qt::Unchecked);
        listItem->setToolTip(
            item->enabled
                ? tr("Enabled; priority %1").arg(item->priority)
                : tr("Disabled; priority %1").arg(item->priority));

        if (item == selectedChain)
            selectedListRow = i;
        if (item->enabled) {
            ui->route_prof->addItem(item->chain_name, i);
            if (item == currentRoute)
                selectedComboRow = ui->route_prof->count() - 1;
        }
    }

    if (selectedListRow < 0 && currentRoute != nullptr)
        selectedListRow = chainList.indexOf(currentRoute);
    if (selectedListRow < 0)
        selectedListRow = 0;
    ui->route_profiles->setCurrentRow(selectedListRow);
    ui->route_prof->setCurrentIndex(selectedComboRow);
    updateRouteProfileControls();
}

int DialogManageRoutes::selectedRouteIndex() const {
    const auto row = ui->route_profiles->currentRow();
    return row >= 0 && row < chainList.size() ? row : -1;
}

void DialogManageRoutes::updateRouteProfileControls() {
    const auto idx = selectedRouteIndex();
    const bool hasSelection = idx >= 0;
    ui->route_priority->setEnabled(hasSelection);
    ui->toggle_route->setEnabled(hasSelection);
    if (!hasSelection)
        return;

    const auto &route = chainList[idx];
    QSignalBlocker blocker(ui->route_priority);
    ui->route_priority->setValue(route->priority);
    ui->toggle_route->setText(route->enabled ? tr("Disable selected")
                                              : tr("Enable selected"));
}

void DialogManageRoutes::set_dns_hijack_enability(const bool enable) const {
    ui->dnshijack_allow_lan->setEnabled(enable);
    ui->dnshijack_listenport->setEnabled(enable);
    ui->dnshijack_rules->setEnabled(enable);
    ui->dnshijack_v4resp->setEnabled(enable);
    ui->dnshijack_v6resp->setEnabled(enable);
}

bool DialogManageRoutes::validate_dns_rules(const QString &rawString) {
    auto rules = rawString.split("\n");
    for (const auto& rule : rules) {
        if (!rule.trimmed().isEmpty() && !rule.startsWith("ruleset:") && !rule.startsWith("domain:") && !rule.startsWith("suffix:") && !rule.startsWith("regex:")) return false;
    }
    return true;
}

void DialogManageRoutes::setupGameModTab() {
    gameModTab = new QWidget(ui->routes_tab);
    gameModTab->setObjectName(QStringLiteral("game_mod_tab"));
    auto *layout = new QVBoxLayout(gameModTab);
    layout->setContentsMargins(18, 18, 18, 18);
    layout->setSpacing(12);

    auto *title = new QLabel(tr("Game Mod"), gameModTab);
    auto titleFont = title->font();
    titleFont.setBold(true);
    titleFont.setPointSize(titleFont.pointSize() + 2);
    title->setFont(titleFont);
    layout->addWidget(title);

    auto *description = new QLabel(
        tr("Add games and services to Enabled now, then choose which ones are "
           "active. Paused services keep their server and settings without "
           "adding routing rules."),
        gameModTab);
    description->setWordWrap(true);
    description->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(description);

    gameModTab->setStyleSheet(QStringLiteral(
        "#game_mod_category_panel { border: 1px solid palette(mid); border-radius: 10px; }"
        "#game_mod_category { border: 0; background: transparent; }"
        "#game_mod_services { border: 0; background: transparent; }"
        "#game_mod_enabled_services { border: 0; background: transparent; }"
        "#game_mod_enabled_card { border: 1px solid palette(mid); "
        "border-radius: 10px; background: palette(alternate-base); }"));

    auto *pages = new QTabWidget(gameModTab);
    pages->setObjectName(QStringLiteral("game_mod_pages"));

    auto *enabledPage = new QWidget(pages);
    auto *enabledPageLayout = new QVBoxLayout(enabledPage);
    enabledPageLayout->setContentsMargins(10, 12, 10, 10);
    enabledPageLayout->setSpacing(8);
    auto *enabledHint = new QLabel(
        tr("Pause routing, test ping, change the server, or remove a saved service."),
        enabledPage);
    enabledHint->setWordWrap(true);
    enabledPageLayout->addWidget(enabledHint);
    gameModEnabledServices = new QListWidget(enabledPage);
    gameModEnabledServices->setObjectName(
        QStringLiteral("game_mod_enabled_services"));
    gameModEnabledServices->setViewMode(QListView::IconMode);
    gameModEnabledServices->setResizeMode(QListView::Adjust);
    gameModEnabledServices->setMovement(QListView::Static);
    gameModEnabledServices->setWrapping(true);
    gameModEnabledServices->setSelectionMode(QAbstractItemView::NoSelection);
    gameModEnabledServices->setGridSize(QSize(430, 122));
    gameModEnabledServices->setSpacing(8);
    gameModEnabledServices->setVerticalScrollMode(
        QAbstractItemView::ScrollPerPixel);
    enabledPageLayout->addWidget(gameModEnabledServices, 1);

    auto *libraryPage = new QWidget(pages);
    auto *libraryLayout = new QVBoxLayout(libraryPage);
    libraryLayout->setContentsMargins(10, 12, 10, 10);
    libraryLayout->setSpacing(10);

    auto *toolbar = new QHBoxLayout();
    toolbar->setSpacing(8);
    gameModSearch = new QLineEdit(gameModTab);
    gameModSearch->setObjectName(QStringLiteral("game_mod_search"));
    gameModSearch->setPlaceholderText(
        tr("Search by service, alias, domain, or executable"));
    gameModSearch->setClearButtonEnabled(true);
    gameModSearch->setMinimumHeight(34);
    toolbar->addWidget(gameModSearch, 1);

    gameModCategory = new QListWidget(gameModTab);
    gameModCategory->setObjectName(QStringLiteral("game_mod_category"));
    gameModSelectVisible = new QPushButton(tr("Add shown"), gameModTab);
    gameModClearVisible = new QPushButton(tr("Remove shown"), gameModTab);
    gameModSelectVisible->setObjectName(QStringLiteral("game_mod_enable_shown"));
    gameModClearVisible->setObjectName(QStringLiteral("game_mod_disable_shown"));
    toolbar->addWidget(gameModSelectVisible);
    toolbar->addWidget(gameModClearVisible);
    libraryLayout->addLayout(toolbar);

    gameModSummary = new QLabel(gameModTab);
    gameModSummary->setObjectName(QStringLiteral("game_mod_summary"));
    libraryLayout->addWidget(gameModSummary);

    auto *splitter = new QSplitter(Qt::Horizontal, libraryPage);
    splitter->setChildrenCollapsible(false);

    auto *categoryPanel = new QWidget(splitter);
    categoryPanel->setObjectName(QStringLiteral("game_mod_category_panel"));
    auto *categoryLayout = new QVBoxLayout(categoryPanel);
    categoryLayout->setContentsMargins(8, 8, 8, 8);
    auto *categoryTitle = new QLabel(tr("Categories"), categoryPanel);
    auto categoryTitleFont = categoryTitle->font();
    categoryTitleFont.setBold(true);
    categoryTitle->setFont(categoryTitleFont);
    categoryLayout->addWidget(categoryTitle);
    gameModCategory->setMinimumWidth(170);
    gameModCategory->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    categoryLayout->addWidget(gameModCategory, 1);

    gameModServices = new QListView(splitter);
    gameModServices->setObjectName(QStringLiteral("game_mod_services"));
    gameModServices->setViewMode(QListView::IconMode);
    gameModServices->setResizeMode(QListView::Adjust);
    gameModServices->setMovement(QListView::Static);
    gameModServices->setWrapping(true);
    gameModServices->setWordWrap(true);
    gameModServices->setSelectionMode(QAbstractItemView::NoSelection);
    gameModServices->setIconSize(QSize(52, 52));
    gameModServices->setGridSize(QSize(162, 102));
    gameModServices->setSpacing(6);
    gameModServices->setUniformItemSizes(true);
    gameModServices->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);

    splitter->addWidget(categoryPanel);
    splitter->addWidget(gameModServices);
    splitter->setStretchFactor(0, 0);
    splitter->setStretchFactor(1, 3);
    splitter->setSizes({190, 1050});
    libraryLayout->addWidget(splitter, 1);

    pages->addTab(enabledPage, tr("Enabled now"));
    pages->addTab(libraryPage, tr("Library"));
    pages->setCurrentWidget(libraryPage);
    layout->addWidget(pages, 1);

    QString catalogError;
    const auto services = GameMod::LoadServices(&catalogError);
    const auto atlas = GameMod::LoadIconAtlas(&catalogError);
    QSet<QString> enabled(
        Configs::dataStore->routing->game_mod_enabled_services.cbegin(),
        Configs::dataStore->routing->game_mod_enabled_services.cend());
    QSet<QString> saved(
        Configs::dataStore->routing->game_mod_saved_services.cbegin(),
        Configs::dataStore->routing->game_mod_saved_services.cend());
    QSet<QString> availableServiceIds;
    for (const auto &service : services) {
        availableServiceIds.insert(service.id);
        for (const auto &legacyId : service.legacyIds)
            availableServiceIds.insert(legacyId);
    }
    enabled.intersect(availableServiceIds);
    saved.intersect(availableServiceIds);
    // Existing installations only have the active list. Treat those entries
    // as saved during the one-time migration to the two-state model.
    saved.unite(enabled);
    const auto profileAssignments = ParseGameModProfileAssignments(
        Configs::dataStore->routing->game_mod_service_profiles);

    QSet<int> profileIds;
    for (const auto &[groupId, group] : Configs::profileManager->groups) {
        Q_UNUSED(groupId);
        if (group == nullptr)
            continue;
        for (const auto profileId : group->profiles) {
            if (profileIds.contains(profileId))
                continue;
            const auto profile = Configs::profileManager->GetProfile(profileId);
            if (profile == nullptr)
                continue;
            profileIds.insert(profileId);
            gameModProfileChoices.append(
                {profileId, QStringLiteral("%1  ·  %2")
                                .arg(group->name, profile->DisplayName())});
        }
    }
    std::sort(gameModProfileChoices.begin(), gameModProfileChoices.end(),
              [](const auto &left, const auto &right) {
                  return left.second.localeAwareCompare(right.second) < 0;
              });

    gameModServiceModel = new GameModServiceModel(
        services, atlas, saved, enabled, profileAssignments, this);
    gameModServiceProxy = new GameModFilterProxyModel(this);
    gameModServiceProxy->setSourceModel(gameModServiceModel);
    gameModServiceProxy->sort(0, Qt::AscendingOrder);
    gameModServices->setModel(gameModServiceProxy);

    QSet<QString> categories;
    for (const auto &service : services)
        categories.insert(service.category);

    QList<QPair<QString, QString>> categoryChoices;
    for (const auto &category : categories)
        categoryChoices.append(
            {GameMod::CategoryDisplayName(category), category});
    std::sort(categoryChoices.begin(), categoryChoices.end(),
              [](const auto &left, const auto &right) {
                  return left.first.localeAwareCompare(right.first) < 0;
              });
    auto *allCategories = new QListWidgetItem(tr("All categories"), gameModCategory);
    allCategories->setData(CategoryRole, QString());
    for (const auto &[label, category] : categoryChoices) {
        auto *item = new QListWidgetItem(label, gameModCategory);
        item->setData(CategoryRole, category);
    }
    gameModCategory->setCurrentRow(0);

    if (services.isEmpty()) {
        gameModSummary->setText(
            catalogError.isEmpty() ? tr("No Game Mod services found")
                                   : catalogError);
    }

    connect(gameModSearch, &QLineEdit::textChanged, this,
            &DialogManageRoutes::filterGameModServices);
    connect(gameModCategory, &QListWidget::currentItemChanged, this,
            [this] { filterGameModServices(gameModSearch->text()); });
    connect(gameModServiceModel, &QAbstractItemModel::dataChanged, this,
            [this] { updateGameModSummary(); });
    connect(gameModSelectVisible, &QPushButton::clicked, this,
            [this] { setVisibleGameModServicesChecked(true); });
    connect(gameModClearVisible, &QPushButton::clicked, this,
            [this] { setVisibleGameModServicesChecked(false); });

    ui->routes_tab->addTab(gameModTab, tr("Game Mod"));
    updateGameModSummary();
}

void DialogManageRoutes::filterGameModServices(const QString &query) {
    const auto category = gameModCategory->currentItem() == nullptr
                              ? QString()
                              : gameModCategory->currentItem()
                                    ->data(CategoryRole)
                                    .toString();
    gameModServiceProxy->setCategory(category);
    gameModServiceProxy->setQuery(query);
    updateGameModSummary(false);
}

void DialogManageRoutes::updateGameModSummary(bool refreshEnabled) {
    if (gameModServiceModel == nullptr || gameModServiceProxy == nullptr)
        return;
    gameModSummary->setText(
        tr("%1 active  |  %2 saved  |  %3 shown of %4 services")
            .arg(gameModServiceModel->enabledCount())
            .arg(gameModServiceModel->savedCount())
            .arg(gameModServiceProxy->rowCount())
            .arg(gameModServiceModel->rowCount()));
    gameModServices->viewport()->update();
    if (refreshEnabled)
        refreshGameModEnabledServices();
}

void DialogManageRoutes::refreshGameModEnabledServices() {
    gameModEnabledServices->clear();
    for (int row = 0; row < gameModServiceModel->rowCount(); ++row) {
        const QPersistentModelIndex sourceIndex =
            gameModServiceModel->index(row, 0);
        if (sourceIndex.data(Qt::CheckStateRole).toInt() != Qt::Checked)
            continue;
        const bool serviceEnabled = sourceIndex.data(ActiveRole).toBool();
        const auto configuredProfile = sourceIndex.data(ProfileIdRole).toInt();
        const auto resolvedProfile = configuredProfile >= 0
                                         ? configuredProfile
                                         : Configs::dataStore->started_id;
        const bool active = serviceEnabled && resolvedProfile >= 0 &&
                            resolvedProfile == Configs::dataStore->started_id;
        auto *item = new QListWidgetItem(gameModEnabledServices);
        item->setSizeHint(QSize(414, 112));
        auto *rowWidget = new QWidget(gameModEnabledServices);
        rowWidget->setObjectName(QStringLiteral("game_mod_enabled_card"));
        auto *rowLayout = new QHBoxLayout(rowWidget);
        rowLayout->setContentsMargins(12, 10, 10, 10);
        rowLayout->setSpacing(10);
        auto *icon = new QLabel(rowWidget);
        icon->setPixmap(GameModIconWithStatus(
                            qvariant_cast<QIcon>(sourceIndex.data(BaseIconRole)),
                            active)
                            .pixmap(48, 48));
        icon->setFixedSize(48, 48);
        rowLayout->addWidget(icon);
        auto *content = new QVBoxLayout();
        content->setSpacing(4);
        auto *name = new QLabel(sourceIndex.data(DisplayNameRole).toString(),
                                rowWidget);
        auto nameFont = name->font();
        nameFont.setBold(true);
        name->setFont(nameFont);
        content->addWidget(name);
        auto *profileCombo = new QComboBox(rowWidget);
        profileCombo->addItem(tr("Current active configuration"), -1);
        for (const auto &[profileId, label] : gameModProfileChoices)
            profileCombo->addItem(label, profileId);
        const auto profileIndex = profileCombo->findData(configuredProfile);
        profileCombo->setCurrentIndex(profileIndex >= 0 ? profileIndex : 0);
        content->addWidget(profileCombo);
        auto statusText =
            resolvedProfile >= 0 ? GameModProfileLabel(resolvedProfile, true)
                                 : tr("No active configuration");
        if (!serviceEnabled)
            statusText = tr("Paused") + QStringLiteral("  ·  ") + statusText;
        auto *status = new QLabel(statusText, rowWidget);
        content->addWidget(status);
        rowLayout->addLayout(content, 1);
        auto *activeToggle = new QCheckBox(tr("Active"), rowWidget);
        activeToggle->setChecked(serviceEnabled);
        activeToggle->setToolTip(
            tr("Pause routing without removing this saved service"));
        rowLayout->addWidget(activeToggle);
        auto *ping = new QToolButton(rowWidget);
        ping->setText(tr("Ping"));
        ping->setToolTip(tr("Test this server now"));
        ping->setIcon(style()->standardIcon(QStyle::SP_BrowserReload));
        rowLayout->addWidget(ping);
        auto *remove = new QToolButton(rowWidget);
        remove->setIcon(style()->standardIcon(QStyle::SP_TitleBarCloseButton));
        remove->setAutoRaise(true);
        remove->setToolTip(tr("Remove this saved service"));
        rowLayout->addWidget(remove);
        gameModEnabledServices->setItemWidget(item, rowWidget);

        connect(profileCombo,
                QOverload<int>::of(&QComboBox::activated), this,
                [this, sourceIndex, profileCombo](int) {
                    const auto profileId = profileCombo->currentData().toInt();
                    QTimer::singleShot(0, this, [this, sourceIndex, profileId] {
                        if (sourceIndex.isValid())
                            gameModServiceModel->setData(sourceIndex, profileId,
                                                         ProfileIdRole);
                    });
                });
        connect(activeToggle, STATE_CHANGED, this,
                [this, sourceIndex](int state) {
                    const bool enabled = state == Qt::Checked;
                    QTimer::singleShot(0, this,
                                       [this, sourceIndex, enabled] {
                        if (sourceIndex.isValid())
                            gameModServiceModel->setData(
                                sourceIndex, enabled, ActiveRole);
                    });
                });
        connect(remove, &QToolButton::clicked, this, [this, sourceIndex] {
            QTimer::singleShot(0, this, [this, sourceIndex] {
                if (sourceIndex.isValid())
                    gameModServiceModel->setData(sourceIndex, Qt::Unchecked,
                                                 Qt::CheckStateRole);
            });
        });
        connect(ping, &QToolButton::clicked, this,
                [this, ping, resolvedProfile] {
                    const auto profile =
                        Configs::profileManager->GetProfile(resolvedProfile);
                    auto *mainWindow = GetMainWindow();
                    if (profile == nullptr || mainWindow == nullptr) {
                        MessageBoxWarning(tr("Ping unavailable"),
                                          tr("No active configuration"));
                        return;
                    }
                    ping->setEnabled(false);
                    ping->setText(tr("Testing…"));
                    QPointer<DialogManageRoutes> guard(this);
                    const bool started = mainWindow->testProfileLatency(
                        profile, [guard](const QList<int> &) {
                            if (guard == nullptr)
                                return;
                            QMetaObject::invokeMethod(
                                guard.data(),
                                [guard] {
                                    if (guard != nullptr)
                                        guard->refreshGameModEnabledServices();
                                },
                                Qt::QueuedConnection);
                        });
                    if (!started) {
                        ping->setEnabled(true);
                        ping->setText(tr("Ping"));
                    }
                });
    }
    if (gameModEnabledServices->count() == 0) {
        auto *empty = new QListWidgetItem(tr("No saved services"),
                                          gameModEnabledServices);
        empty->setFlags(Qt::NoItemFlags);
    }
}

void DialogManageRoutes::setVisibleGameModServicesChecked(bool checked) {
    QList<int> sourceRows;
    sourceRows.reserve(gameModServiceProxy->rowCount());
    for (int row = 0; row < gameModServiceProxy->rowCount(); ++row) {
        const auto sourceIndex =
            gameModServiceProxy->mapToSource(gameModServiceProxy->index(row, 0));
        if (sourceIndex.isValid())
            sourceRows.append(sourceIndex.row());
    }
    gameModServiceModel->setRowsChecked(sourceRows, checked);
}

DialogManageRoutes::DialogManageRoutes(QWidget *parent, bool EditRouteProfiles,
                                       bool GameMod)
    : QDialog(parent), ui(new Ui::DialogManageRoutes) {
    CHECK_SETTINGS_ACCESS
    ui->setupUi(this);
    auto profiles = Configs::profileManager->routes;
    for (const auto &item: profiles) {
        chainList << item.second;
    }
    if (chainList.empty()) {
        auto defaultChain = Configs::RoutingChain::GetDefaultChain();
        Configs::profileManager->AddRouteChain(defaultChain);
        chainList.append(defaultChain);
    }
    currentRoute = Configs::profileManager->GetRouteChain(Configs::dataStore->routing->current_route_id);
    if (currentRoute == nullptr) currentRoute = chainList[0];

    QStringList qsValue = {""};
    QString dnsHelpDocumentUrl;

    ui->outbound_domain_strategy->addItems(Preset::SingBox::DomainStrategy);
    ui->domainStrategyCombo->addItems(Preset::SingBox::DomainStrategy);
    qsValue += QString("prefer_ipv4 prefer_ipv6 ipv4_only ipv6_only").split(" ");
    ui->dns_object->setPlaceholderText(DecodeB64IfValid("ewogICJzZXJ2ZXJzIjogW10sCiAgInJ1bGVzIjogW10sCiAgImZpbmFsIjogIiIsCiAgInN0cmF0ZWd5IjogIiIsCiAgImRpc2FibGVfY2FjaGUiOiBmYWxzZSwKICAiZGlzYWJsZV9leHBpcmUiOiBmYWxzZSwKICAiaW5kZXBlbmRlbnRfY2FjaGUiOiBmYWxzZSwKICAicmV2ZXJzZV9tYXBwaW5nIjogZmFsc2UsCiAgImZha2VpcCI6IHt9Cn0="));
    dnsHelpDocumentUrl = "https://sing-box.sagernet.org/configuration/dns/";

    ui->direct_dns_strategy->addItems(qsValue);
    ui->remote_dns_strategy->addItems(qsValue);
    ui->local_override->setText(Configs::dataStore->core_box_underlying_dns);
    ui->enable_fakeip->setChecked(Configs::dataStore->fake_dns);
    //
    connect(ui->use_dns_object, STATE_CHANGED, this, [=,this](int state) {
        auto useDNSObject = state == Qt::Checked;
        ui->simple_dns_box->setDisabled(useDNSObject);
        ui->dns_object->setDisabled(!useDNSObject);
    });
#if QT_VERSION >= QT_VERSION_CHECK(6, 9, 0)
    ui->use_dns_object->checkStateChanged(Qt::Unchecked); // uncheck to uncheck
#else
    ui->use_dns_object->stateChanged(Qt::Unchecked); // uncheck to uncheck
#endif
    connect(ui->dns_document, &QPushButton::clicked, this, [=,this] {
        MessageBoxInfo("DNS", dnsHelpDocumentUrl);
    });
    connect(ui->format_dns_object, &QPushButton::clicked, this, [=,this] {
        auto obj = QString2QJsonObject(ui->dns_object->toPlainText());
        if (obj.isEmpty()) {
            MessageBoxInfo("DNS", "invaild json");
        } else {
            ui->dns_object->setPlainText(QJsonObject2QString(obj, false));
        }
    });
    ui->ruleset_json_url->setText(Configs::dataStore->routing->ruleset_json_url);
    ui->sniffing_mode->setCurrentIndex(Configs::dataStore->routing->sniffing_mode);
    ui->ruleset_mirror->setCurrentIndex(Configs::dataStore->routing->ruleset_mirror);
    ui->outbound_domain_strategy->setCurrentText(Configs::dataStore->routing->outbound_domain_strategy);
    ui->domainStrategyCombo->setCurrentText(Configs::dataStore->routing->domain_strategy);
    ui->use_dns_object->setChecked(Configs::dataStore->routing->use_dns_object);
    ui->dns_object->setPlainText(Configs::dataStore->routing->dns_object);
    ui->remote_dns->setCurrentText(Configs::dataStore->routing->remote_dns);
    ui->remote_dns_strategy->setCurrentText(Configs::dataStore->routing->remote_dns_strategy);
    ui->direct_dns->setCurrentText(Configs::dataStore->routing->direct_dns);
    ui->direct_dns_strategy->setCurrentText(Configs::dataStore->routing->direct_dns_strategy);
    ui->dns_final_out->setCurrentIndex(Configs::dataStore->routing->dns_final_out_direct ? 1 : 0);
    reloadProfileItems();

    connect(ui->route_profiles, &QListWidget::itemDoubleClicked, this, [=,this](const QListWidgetItem* item){
        on_edit_route_clicked();
    });

    connect(ui->route_prof, SIGNAL(currentIndexChanged(int)), this, SLOT(updateCurrentRouteProfile(int)));
    connect(ui->route_profiles, &QListWidget::currentRowChanged, this,
            [this](int) { updateRouteProfileControls(); });
    connect(ui->route_profiles, &QListWidget::itemChanged, this,
            [this](QListWidgetItem *item) {
                const auto idx = ui->route_profiles->row(item);
                if (idx < 0 || idx >= chainList.size())
                    return;
                const bool enabled = item->checkState() == Qt::Checked;
                if (!enabled && chainList[idx]->enabled) {
                    const auto enabledCount =
                        std::count_if(chainList.cbegin(), chainList.cend(),
                                      [](const auto &route) {
                                          return route->enabled;
                                      });
                    if (enabledCount <= 1) {
                        QSignalBlocker blocker(ui->route_profiles);
                        item->setCheckState(Qt::Checked);
                        MessageBoxWarning(
                            tr("Invalid operation"),
                            tr("At least one routing profile must remain enabled"));
                        return;
                    }
                }
                chainList[idx]->enabled = enabled;
                updateRouteProfileControls();
                QTimer::singleShot(0, this,
                                   [this] { reloadProfileItems(); });
            });
    connect(ui->toggle_route, &QPushButton::clicked, this, [this] {
        const auto idx = selectedRouteIndex();
        if (idx < 0)
            return;
        if (chainList[idx]->enabled) {
            const auto enabledCount =
                std::count_if(chainList.cbegin(), chainList.cend(),
                              [](const auto &route) {
                                  return route->enabled;
                              });
            if (enabledCount <= 1) {
                MessageBoxWarning(
                    tr("Invalid operation"),
                    tr("At least one routing profile must remain enabled"));
                return;
            }
        }
        chainList[idx]->enabled = !chainList[idx]->enabled;
        reloadProfileItems();
    });
    connect(ui->route_priority, &QSpinBox::valueChanged, this,
            [this](int priority) {
                const auto idx = selectedRouteIndex();
                if (idx < 0 || chainList[idx]->priority == priority)
                    return;
                chainList[idx]->priority = priority;
                reloadProfileItems();
            });

    deleteShortcut = new QShortcut(QKeySequence(Qt::Key_Delete), this);

    connect(deleteShortcut, &QShortcut::activated, this, [=,this]{
        on_delete_route_clicked();
    });

    #define CHECK_LINE_EDIT_EMPTY(X) if (ui->X->text().isEmpty()) { goto generate_warp; }

    connect(ui->save_wireguard, &QPushButton::clicked, this, [=, this]{
        if (!warp_save.try_lock()){
            return;
        }

        CHECK_LINE_EDIT_EMPTY(warp_ep)
        CHECK_LINE_EDIT_EMPTY(warp_private_key)
        CHECK_LINE_EDIT_EMPTY(warp_public_key)
        CHECK_LINE_EDIT_EMPTY(warp_ifc_addrs)
        goto skip_warp;
        generate_warp:
        GenerateWarpConfig(
        ui->warp_autogen,
        ui->warp_private_key,
        ui->warp_public_key,
        ui->warp_ep,
        ui->warp_ifc_addrs,
        this
        );
        skip_warp:
        bool ok = false;
        QString text = QInputDialog::getText(
            this,
            tr("New warp profile"),
            ("Profile name"),
            QLineEdit::Normal,
            "",
            &ok
        );
        if (ok){
            std::shared_ptr<Configs::ProxyEntity> proxy = Configs::ProfileManager::NewProxyEntity("wireguard"); 
            std::shared_ptr<Configs::WireguardBean> bean = proxy->unlock(proxy->WireguardBean());
            bean->privateKey = ui->warp_private_key->text();
            bean->publicKey = ui->warp_public_key->text();
            bean->localAddress = ui->warp_ifc_addrs->text().replace(" ", "").split(",");
            QString serverAddress = ui->warp_ep->text();
            int index = serverAddress.lastIndexOf(':');
            if (index != -1) {
                QString lastPart = serverAddress.mid(index + 1);
                bool ok = false;
                int serverPort = lastPart.toInt(&ok);
                if (ok){
                    proxy->serverPort = serverPort;
                    serverAddress = serverAddress.left(index);
                }
            }
            proxy->serverAddress = serverAddress;
            Configs::profileManager->AddProfile(proxy);
            proxy->name = text;
            proxy->Save();
        }
        warp_save.unlock();
    });

    #undef CHECK_LINE_EDIT_EMPTY

    // hijack
    ui->dnshijack_enable->setChecked(Configs::dataStore->enable_dns_server);
    set_dns_hijack_enability(Configs::dataStore->enable_dns_server);
    ui->dnshijack_allow_lan->setChecked(Configs::dataStore->dns_server_listen_lan);
    ui->dnshijack_listenport->setValidator(QRegExpValidator_Number);
    ui->dnshijack_listenport->setText(QString::number(Configs::dataStore->dns_server_listen_port));
    ui->dnshijack_v4resp->setText(Configs::dataStore->dns_v4_resp);
    ui->dnshijack_v6resp->setText(Configs::dataStore->dns_v6_resp);

    QStringList ruleItems = {"domain:", "suffix:", "regex:"};
    for (const auto& item : ruleSetMap.keys()) {
        ruleItems.append("ruleset:" + item);
    }
    rule_editor = new AutoCompleteTextEdit("", ruleItems, this);
    ui->hijack_box->layout()->replaceWidget(ui->dnshijack_rules, rule_editor);
    rule_editor->setPlainText(Configs::dataStore->dns_server_rules.join("\n"));
    ui->dnshijack_rules->hide();
#ifndef Q_OS_UNIX
    ui->dnshijack_listenport->setVisible(false);
    ui->dnshijack_listenport_l->setVisible(false);
#endif

    ui->redirect_enable->setChecked(Configs::dataStore->enable_redirect);
    ui->redirect_listenaddr->setEnabled(Configs::dataStore->enable_redirect);
    ui->redirect_listenaddr->setText(Configs::dataStore->redirect_listen_address);
    ui->redirect_listenport->setEnabled(Configs::dataStore->enable_redirect);
    ui->redirect_listenport->setValidator(QRegExpValidator_Number);
    ui->redirect_listenport->setText(QString::number(Configs::dataStore->redirect_listen_port));

    connect(ui->dnshijack_enable, STATE_CHANGED, this, [=,this](bool state) {
        set_dns_hijack_enability(state);
    });
    connect(ui->redirect_enable, STATE_CHANGED, this, [=,this](bool state) {
        ui->redirect_listenaddr->setEnabled(state);
        ui->redirect_listenport->setEnabled(state);
    });

    setupGameModTab();
    if (GameMod) {
        resize(1440, 900);
        setMinimumSize(1100, 720);
        ui->routes_tab->setCurrentWidget(gameModTab);
    } else
        ui->routes_tab->setCurrentIndex(EditRouteProfiles ? 3 : 0);

    // warp
    BindWarpGenerator(ui->warp_autogen,
        ui->warp_private_key,
        ui->warp_public_key,
        ui->warp_ep,
        ui->warp_ifc_addrs,
        this
    );

    ADD_ASTERISK(this)
}

void DialogManageRoutes::GenerateWarpConfig(
        QPushButton * warp_button, 
        QLineEdit * warp_private_key, 
        QLineEdit * warp_public_key,
        QLineEdit * warp_ep,
        QLineEdit * warp_ifc_addrs, 
        QWidget * context,
        QLineEdit * port){
    auto originalText = warp_button->text();
    warp_button->setText(tr("Getting keypair..."));
    bool ok;
    auto keyPair = API::defaultClient->GenWgKeyPair(&ok);
    if (!ok) {
        runOnUiThread([context, keyPair] {
            QMessageBox::warning(context, tr("Failed to get key pair"), keyPair->error.c_str());
        });
        warp_button->setText(originalText);
        return;
    }
    warp_button->setText(tr("Generating config..."));
    QString error;
    auto conf = Configs_network::genWarpConfig(&error, keyPair->private_key.c_str(), keyPair->public_key.c_str());
    if (!error.isEmpty()) {
        runOnUiThread([context, error] {
            QMessageBox::warning(context, tr("Failed to generate warp config"), error);
        });
        warp_button->setText(originalText);
        return;
    }
    warp_private_key->setText(conf->privateKey);
    warp_public_key->setText(conf->publicKey);
    warp_ifc_addrs->setText(conf->ipv4Address + "/32," + conf->ipv6Address + "/128");
    if (port == nullptr) {
        warp_set_endpoint:
        warp_ep->setText(conf->endpoint);
    } else {
        QString serverAddress = conf->endpoint;
        int index = serverAddress.lastIndexOf(':');
        if (index != -1) {
            port->setText(serverAddress.mid(index + 1));
            warp_ep->setText(serverAddress.left(index));
        } else {
            goto warp_set_endpoint;
        }
    }

    warp_button->setText(tr("Success!"));
    setTimeout([=] { warp_button->setText(originalText); }, context, 2000);
}

void DialogManageRoutes::BindWarpGenerator(
        QPushButton * warp_button, 
        QLineEdit * warp_private_key, 
        QLineEdit * warp_public_key,
        QLineEdit * warp_ep,
        QLineEdit * warp_ifc_addrs, 
        QWidget * context,
        QLineEdit * port){
  connect(warp_button, &QPushButton::clicked, context, [=](){
    GenerateWarpConfig(
        warp_button,
        warp_private_key,
        warp_public_key,
        warp_ep, 
        warp_ifc_addrs,
        context,
        port
    );
  });
};

void DialogManageRoutes::updateCurrentRouteProfile(int idx) {
    if (idx < 0)
        return;
    const auto chainIndex = ui->route_prof->itemData(idx).toInt();
    if (chainIndex >= 0 && chainIndex < chainList.size())
        currentRoute = chainList[chainIndex];
}

DialogManageRoutes::~DialogManageRoutes() {
    delete ui;
}

void DialogManageRoutes::accept() {
    if (chainList.empty()) {
        MessageBoxInfo(tr("Invalid settings"), tr("Routing profile cannot be empty"));
        return;
    }
    if (std::none_of(chainList.cbegin(), chainList.cend(),
                     [](const auto &route) { return route->enabled; })) {
        MessageBoxInfo(tr("Invalid settings"),
                       tr("At least one routing profile must remain enabled"));
        return;
    }
    if (!validate_dns_rules(rule_editor->toPlainText())) {
        MessageBoxInfo(tr("Invalid settings"), tr("DNS Rules are not valid"));
        return;
    }
    Configs::dataStore->routing->ruleset_json_url = ui->ruleset_json_url->text();
    Configs::dataStore->routing->sniffing_mode = ui->sniffing_mode->currentIndex();
    Configs::dataStore->routing->ruleset_mirror = ui->ruleset_mirror->currentIndex();
    Configs::dataStore->routing->domain_strategy = ui->domainStrategyCombo->currentText();
    Configs::dataStore->routing->outbound_domain_strategy = ui->outbound_domain_strategy->currentText();
    Configs::dataStore->routing->use_dns_object = ui->use_dns_object->isChecked();
    Configs::dataStore->routing->dns_object = ui->dns_object->toPlainText();
    Configs::dataStore->routing->remote_dns = ui->remote_dns->currentText();
    Configs::dataStore->routing->remote_dns_strategy = ui->remote_dns_strategy->currentText();
    Configs::dataStore->routing->direct_dns = ui->direct_dns->currentText();
    Configs::dataStore->routing->direct_dns_strategy = ui->direct_dns_strategy->currentText();
    Configs::dataStore->core_box_underlying_dns = ui->local_override->text().trimmed();
    Configs::dataStore->routing->dns_final_out_direct = ui->dns_final_out->currentIndex() == 1;
    Configs::dataStore->fake_dns = ui->enable_fakeip->isChecked();

    Configs::dataStore->routing->game_mod_enabled_services =
        gameModServiceModel->enabledServiceIds();
    Configs::dataStore->routing->game_mod_saved_services =
        gameModServiceModel->savedServiceIds();
    Configs::dataStore->routing->game_mod_service_profiles =
        QString::fromUtf8(QJsonDocument(gameModServiceModel->profileAssignments()).toJson(
            QJsonDocument::Compact));

    Configs::profileManager->UpdateRouteChains(chainList);
    Configs::dataStore->routing->current_route_id = currentRoute->id;

    Configs::dataStore->enable_dns_server = ui->dnshijack_enable->isChecked();
    Configs::dataStore->dns_server_listen_port = ui->dnshijack_listenport->text().toInt();
    Configs::dataStore->dns_v4_resp = ui->dnshijack_v4resp->text();
    Configs::dataStore->dns_v6_resp = ui->dnshijack_v6resp->text();
    auto rawRules = rule_editor->toPlainText().split("\n");
    QStringList dnsRules;
    for (const auto& rawRule : rawRules) {
        if (rawRule.trimmed().isEmpty()) continue;
        dnsRules.append(rawRule.trimmed());
    }
    Configs::dataStore->dns_server_rules = dnsRules;

    {
        bool newLan = ui->dnshijack_allow_lan->isChecked();
        bool wasLan = Configs::dataStore->dns_server_listen_lan;
        if (newLan && !wasLan) {
            auto btn = QMessageBox::warning(
                this,
                tr("Security Warning"),
                tr("Enabling LAN DNS server will listen on 0.0.0.0:%1.\n\n"
                   "Any device on your local network will be able to query this DNS server, "
                   "which may reveal your routing rules, blocked domains, and network topology.\n\n"
                   "Only enable this if you intend to share DNS with trusted LAN devices.\n\n"
                   "Are you sure?").arg(Configs::dataStore->dns_server_listen_port),
                QMessageBox::Yes | QMessageBox::No,
                QMessageBox::No
            );
            if (btn != QMessageBox::Yes) {
                ui->dnshijack_allow_lan->setChecked(false);
                newLan = false;
            }
        }
        Configs::dataStore->dns_server_listen_lan = newLan;
    }
    Configs::dataStore->enable_redirect = ui->redirect_enable->isChecked();
    Configs::dataStore->redirect_listen_address = ui->redirect_listenaddr->text();
    Configs::dataStore->redirect_listen_port = ui->redirect_listenport->text().toInt();

    //
    QStringList msg{"UpdateDataStore"};
    msg << "RouteChanged";
    MW_dialog_message("", msg.join(","));

    QDialog::accept();
}

void DialogManageRoutes::on_new_route_clicked() {
    auto route = Configs::ProfileManager::NewRouteChain();
    if (!chainList.isEmpty()) {
        route->priority =
            (*std::max_element(chainList.cbegin(), chainList.cend(),
                               [](const auto &left, const auto &right) {
                                   return left->priority < right->priority;
                               }))
                ->priority +
            10;
    }
    routeChainWidget = new RouteItem(this, route);
    routeChainWidget->setWindowModality(Qt::ApplicationModal);
    routeChainWidget->show();
    connect(routeChainWidget, &RouteItem::settingsChanged, this, [=,this](const std::shared_ptr<Configs::RoutingChain>& chain) {
        chainList << chain;
        reloadProfileItems();
    });
}

void DialogManageRoutes::on_export_route_clicked()
{
    auto idx = ui->route_profiles->currentRow();
    if (idx < 0) return;

    auto chain = chainList[idx];

    QJsonObject ret = {
        {"name", chain->chain_name},
        {"url", chain->update_url},
        {"proxy", chain->defaultOutboundID},
        {"skip_update", chain->skip_update},
        {"enabled", chain->enabled},
        {"priority", chain->priority},
        {"rules",  chain->get_route_rules(true, true, {})}
    };
    QStringList res;
    QApplication::clipboard()->setText(
        QJsonDocument(ret).toJson(QJsonDocument::JsonFormat::Indented)
    );

    QToolTip::showText(QCursor::pos(), "Copied!", this);
    int r = ++tooltipID;
    QTimer::singleShot(1500, [=,this] {
        if (tooltipID != r) return;
        QToolTip::hideText();
    });
}

void DialogManageRoutes::on_clone_route_clicked() {
    auto idx = ui->route_profiles->currentRow();
    if (idx < 0) return;

    auto chainCopy = std::make_shared<Configs::RoutingChain>(*chainList[idx]);
    chainCopy->chain_name = chainCopy->chain_name + " clone";
    chainCopy->priority++;
    chainCopy->id = -1;
    chainCopy->save_control_no_save(false);
    chainList.append(chainCopy);
    reloadProfileItems();
}

void DialogManageRoutes::on_edit_route_clicked() {
    auto idx = ui->route_profiles->currentRow();
    if (idx < 0) return;

    routeChainWidget = new RouteItem(this, chainList[idx]);
    routeChainWidget->setWindowModality(Qt::ApplicationModal);
    routeChainWidget->show();
    connect(routeChainWidget, &RouteItem::settingsChanged, this, [=,this](const std::shared_ptr<Configs::RoutingChain>& chain) {
        if (currentRoute == chainList[idx]) currentRoute = chain;
        chainList[idx] = chain;
        reloadProfileItems();
    });

}

void DialogManageRoutes::on_delete_route_clicked() {
    auto idx = ui->route_profiles->currentRow();
    if (idx < 0) return;
    if (chainList.size() == 1) {
        MessageBoxWarning(tr("Invalid operation"), tr("Routing Profiles cannot be empty, try adding another profile or editing this one"));
        return;
    }

    auto profileToDel = chainList[idx];
    chainList.removeAt(idx);
    if (profileToDel == currentRoute) {
        currentRoute = chainList[0];
    }
    reloadProfileItems();
}

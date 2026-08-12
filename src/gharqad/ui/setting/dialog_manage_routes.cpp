#include <nekobox/ui/setting/dialog_manage_routes.h>
#include <nekobox/ui/setting/GameModCatalog.h>
#include <nekobox/configs/warp/warp.hpp>
#include <nekobox/configs/proxy/WireguardBean.h>

#include <QClipboard>
#include <QAbstractItemView>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QSet>
#include <QSignalBlocker>
#include <QVBoxLayout>
#include <algorithm>
#include <memory>

#include <nekobox/dataStore/Database.hpp>

#include <3rdparty/qv2ray/v2/ui/widgets/editors/w_JsonEditor.hpp>
#include <nekobox/global/GuiUtils.hpp>
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
    layout->setContentsMargins(16, 16, 16, 16);
    layout->setSpacing(12);

    auto *title = new QLabel(tr("Game Mod"), gameModTab);
    auto titleFont = title->font();
    titleFont.setBold(true);
    titleFont.setPointSize(titleFont.pointSize() + 2);
    title->setFont(titleFont);
    layout->addWidget(title);

    auto *description = new QLabel(
        tr("Choose which games and services should use the proxy. "
           "Checked services add their optimized process, domain, address, "
           "and port rules before the current routing profile."),
        gameModTab);
    description->setWordWrap(true);
    description->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(description);

    auto *toolbar = new QHBoxLayout();
    toolbar->setSpacing(8);
    gameModSearch = new QLineEdit(gameModTab);
    gameModSearch->setObjectName(QStringLiteral("game_mod_search"));
    gameModSearch->setPlaceholderText(tr("Search games, services, or processes"));
    gameModSearch->setClearButtonEnabled(true);
    gameModSearch->setMinimumHeight(34);
    toolbar->addWidget(gameModSearch, 1);

    gameModSelectVisible = new QPushButton(tr("Enable shown"), gameModTab);
    gameModClearVisible = new QPushButton(tr("Disable shown"), gameModTab);
    gameModSelectVisible->setObjectName(QStringLiteral("game_mod_enable_shown"));
    gameModClearVisible->setObjectName(QStringLiteral("game_mod_disable_shown"));
    toolbar->addWidget(gameModSelectVisible);
    toolbar->addWidget(gameModClearVisible);
    layout->addLayout(toolbar);

    gameModSummary = new QLabel(gameModTab);
    gameModSummary->setObjectName(QStringLiteral("game_mod_summary"));
    layout->addWidget(gameModSummary);

    gameModServices = new QListWidget(gameModTab);
    gameModServices->setObjectName(QStringLiteral("game_mod_services"));
    gameModServices->setIconSize(QSize(40, 40));
    gameModServices->setUniformItemSizes(true);
    gameModServices->setAlternatingRowColors(true);
    gameModServices->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    layout->addWidget(gameModServices, 1);

    QString catalogError;
    const auto services = GameMod::LoadServices(&catalogError);
    const auto atlas = GameMod::LoadIconAtlas(&catalogError);
    const QSet<QString> enabled(
        Configs::dataStore->routing->game_mod_enabled_services.cbegin(),
        Configs::dataStore->routing->game_mod_enabled_services.cend());

    for (const auto &service : services) {
        auto displayName = service.name;
        if (!displayName.isEmpty())
            displayName[0] = displayName.at(0).toUpper();
        const auto details = tr("%1 proxy rules, %2 direct rules")
                                 .arg(service.proxyRuleCount)
                                 .arg(service.directRuleCount);
        auto *item = new QListWidgetItem(
            atlas.isNull() ? QIcon() : QIcon(atlas.copy(service.iconRect)),
            displayName + QStringLiteral("\n") + details,
            gameModServices);
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setCheckState(enabled.contains(service.id) ? Qt::Checked
                                                         : Qt::Unchecked);
        item->setData(Qt::UserRole, service.id);
        item->setData(Qt::UserRole + 1,
                      (service.name + QLatin1Char(' ') +
                       service.keywords.join(QLatin1Char(' ')))
                          .toCaseFolded());
        item->setToolTip(tr("Processes: %1\n%2")
                             .arg(service.keywords.join(QStringLiteral(", ")),
                                  details));
        item->setSizeHint(QSize(0, 58));
    }

    if (services.isEmpty()) {
        auto *item = new QListWidgetItem(
            catalogError.isEmpty() ? tr("No Game Mod services found")
                                   : catalogError,
            gameModServices);
        item->setFlags(Qt::NoItemFlags);
    }

    connect(gameModSearch, &QLineEdit::textChanged, this,
            &DialogManageRoutes::filterGameModServices);
    connect(gameModServices, &QListWidget::itemChanged, this,
            [this](QListWidgetItem *) { updateGameModSummary(); });
    connect(gameModSelectVisible, &QPushButton::clicked, this,
            [this] { setVisibleGameModServicesChecked(true); });
    connect(gameModClearVisible, &QPushButton::clicked, this,
            [this] { setVisibleGameModServicesChecked(false); });

    ui->routes_tab->addTab(gameModTab, tr("Game Mod"));
    updateGameModSummary();
}

void DialogManageRoutes::filterGameModServices(const QString &query) {
    const auto needle = query.trimmed().toCaseFolded();
    for (int row = 0; row < gameModServices->count(); ++row) {
        auto *item = gameModServices->item(row);
        item->setHidden(!needle.isEmpty() &&
                        !item->data(Qt::UserRole + 1)
                             .toString()
                             .contains(needle));
    }
    updateGameModSummary();
}

void DialogManageRoutes::updateGameModSummary() {
    int enabled = 0;
    int visible = 0;
    for (int row = 0; row < gameModServices->count(); ++row) {
        const auto *item = gameModServices->item(row);
        if (!item->isHidden())
            ++visible;
        if (item->checkState() == Qt::Checked)
            ++enabled;
    }
    gameModSummary->setText(
        tr("%1 enabled  |  %2 shown of %3 services")
            .arg(enabled)
            .arg(visible)
            .arg(gameModServices->count()));
}

void DialogManageRoutes::setVisibleGameModServicesChecked(bool checked) {
    QSignalBlocker blocker(gameModServices);
    for (int row = 0; row < gameModServices->count(); ++row) {
        auto *item = gameModServices->item(row);
        if (!item->isHidden() && (item->flags() & Qt::ItemIsUserCheckable))
            item->setCheckState(checked ? Qt::Checked : Qt::Unchecked);
    }
    updateGameModSummary();
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
    if (GameMod)
        ui->routes_tab->setCurrentWidget(gameModTab);
    else
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

    QStringList enabledGameModServices;
    for (int row = 0; row < gameModServices->count(); ++row) {
        const auto *item = gameModServices->item(row);
        const auto serviceId = item->data(Qt::UserRole).toString();
        if (!serviceId.isEmpty() && item->checkState() == Qt::Checked)
            enabledGameModServices.append(serviceId);
    }
    Configs::dataStore->routing->game_mod_enabled_services =
        enabledGameModServices;

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

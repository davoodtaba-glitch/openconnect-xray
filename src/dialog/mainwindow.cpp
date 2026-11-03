/*
 * Copyright (C) 2014, 2015 Red Hat
 *
 * This file is part of openconnect-gui.
 *
 * openconnect-gui is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "mainwindow.h"
#include "NewProfileDialog.h"
#include "XrayProfileDialog.h"
#include "config.h"
#include "editdialog.h"
#include "logdialog.h"
#include "openconnect-gui.h"
#include "server_storage.h"
#include "timestamp.h"
#include "ui_mainwindow.h"
#include "vpninfo.h"
#include "xray/XrayClient.h"
#include "xray/XrayConfig.h"
#include "xray/XraySubscriptionUpdater.h"
#include "logger.h"

extern "C" {
#include <gnutls/gnutls.h>
}
#include <spdlog/spdlog.h>

#include <QAbstractButton>
#include <QCloseEvent>
#include <QDateTime>
#include <QDesktopServices>
#include <QDialog>
#include <QEventTransition>
#include <QFileSelector>
#include <QFutureWatcher>
#include <QInputDialog>
#include <QLineEdit>
#include <QMessageBox>
#include <OcSettings.h>
#include <QSignalTransition>
#include <QStateMachine>
#include <QFrame>
#include <QRegularExpression>
#include <QScrollArea>
#include <QUrl>
#include <QTimer>
#include <QtConcurrent/QtConcurrentRun>
#include <QtNetwork/QNetworkProxy>
#include <QtNetwork/QNetworkProxyFactory>
#include <QtNetwork/QNetworkProxyQuery>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QNetworkAccessManager>
#include <QProgressDialog>

#include <cmath>
#include <cstdarg>
#include <cstdio>

#ifdef _WIN32
#define pipe_write(x, y, z) send(x, y, z, 0)
#else
#define pipe_write(x, y, z) write(x, y, z)
#endif

static int app_loglevel_tab(int mode)
{
    // keep in sync with the order of the QAction items in src/dialog/mainwindow.ui
    switch (mode) {
    case PRG_ERR:
        return 0;
    case PRG_INFO:
        return 1;
    case PRG_DEBUG:
        return 2;
    case PRG_TRACE:
        return 3;
    default:
        return -1;
    }
}

static int app_loglevel_rtab[] = {
    // keep in sync with the order of the QAction items in src/dialog/mainwindow.ui
    PRG_ERR,   // [0]
    PRG_INFO,  // [1]
    PRG_DEBUG, // [2]
    PRG_TRACE  // [3]
};


MainWindow::MainWindow(QWidget* parent, bool useTray, const QString profileName)
    : QMainWindow(parent)
    , ui(new Ui::MainWindow)
{
    ui->setupUi(this);

    auto wrapTabInScroll = [this](int index) {
        QWidget* page = ui->tabWidget->widget(index);
        if (page == nullptr) {
            return;
        }
        const QString title = ui->tabWidget->tabText(index);
        const QIcon icon = ui->tabWidget->tabIcon(index);
        ui->tabWidget->removeTab(index);

        auto* scroll = new QScrollArea(ui->tabWidget);
        scroll->setFrameShape(QFrame::NoFrame);
        scroll->setWidgetResizable(true);
        scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
        scroll->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
        scroll->setWidget(page);
        ui->tabWidget->insertTab(index, scroll, icon, title);
    };
    wrapTabInScroll(0);
    wrapTabInScroll(1);

    XrayClient::restoreStaleSystemProxy();
    XrayClient::restoreStalePhysicalDns();

    connect(ui->viewLogButton, &QPushButton::clicked,
        this, &MainWindow::createLogDialog);

    timer = new QTimer(this);
    blink_timer = new QTimer(this);
    this->cmd_fd = INVALID_SOCKET;

    downloadProgress = nullptr;
    manager = new QNetworkAccessManager();

    connect(ui->actionQuit, &QAction::triggered,
        [=]() {
            const bool vpnActive = (m_xray != nullptr && m_xray->isRunning())
                || (m_trayIcon != nullptr && m_disconnectAction->isEnabled());
            if (vpnActive) {
                connect(this, &MainWindow::readyToShutdown,
                    qApp, &QApplication::quit);
                on_disconnectClicked();
            } else {
                qApp->quit();
            }
        });

    connect(blink_timer, &QTimer::timeout,
        this, &MainWindow::blink_ui,
        Qt::QueuedConnection);
    connect(timer, &QTimer::timeout,
        this, &MainWindow::request_update_stats,
        Qt::QueuedConnection);
    connect(ui->serverList->lineEdit(), &QLineEdit::returnPressed,
        this, &MainWindow::on_connectClicked,
        Qt::QueuedConnection);
    connect(this, &MainWindow::vpn_status_changed_sig,
        this, &MainWindow::changeStatus,
        Qt::QueuedConnection);
    connect(ui->connectionButton, &QPushButton::clicked,
        this, &MainWindow::on_connectClicked,
        Qt::QueuedConnection);
    connect(this, &MainWindow::stats_changed_sig,
        this, &MainWindow::statsChanged,
        Qt::QueuedConnection);

    ui->iconLabel->setPixmap(OFF_ICON);
    QNetworkProxyFactory::setUseSystemConfiguration(true);
    last_check_time = 0;

    if (useTray) {
        createTrayIcon();

        connect(m_trayIcon, &QSystemTrayIcon::activated,
            this, &MainWindow::iconActivated);

        QFileSelector selector;
        QIcon icon(selector.select(QStringLiteral(":/images/network-disconnected.png")));
        icon.setIsMask(true);
        m_trayIcon->setIcon(icon);
        m_trayIcon->show();
    } else {
        Logger::instance().addMessage(QLatin1String("System doesn't support tray icon"));
        m_trayIcon = nullptr;
    }

    // TODO: initial state machine
    QStateMachine* machine = new QStateMachine(this);
    QState* s1_noProfiles = new QState();
    s1_noProfiles->assignProperty(ui->connectionButton, "enabled", false);
    s1_noProfiles->assignProperty(ui->serverList, "enabled", true);
    s1_noProfiles->assignProperty(ui->actionEditSelectedProfile, "enabled", false);
    s1_noProfiles->assignProperty(ui->actionRemoveSelectedProfile, "enabled", false);

    if (m_trayIcon) {
        s1_noProfiles->assignProperty(m_trayIconMenuConnections, "title", tr("(no servers to connect)"));
        s1_noProfiles->assignProperty(m_trayIconMenuConnections, "enabled", false);
        s1_noProfiles->assignProperty(m_disconnectAction, "enabled", false);
    }
    machine->addState(s1_noProfiles);

    QState* s2_connectionReady = new QState();
    s2_connectionReady->assignProperty(ui->connectionButton, "enabled", true);
    s2_connectionReady->assignProperty(ui->serverList, "enabled", true);
    s2_connectionReady->assignProperty(ui->actionEditSelectedProfile, "enabled", true);
    s2_connectionReady->assignProperty(ui->actionRemoveSelectedProfile, "enabled", true);

    if (m_trayIcon) {
        s2_connectionReady->assignProperty(m_trayIconMenuConnections, "title", tr("Connect to..."));
        s2_connectionReady->assignProperty(m_trayIconMenuConnections, "enabled", true);
    }
    machine->addState(s2_connectionReady);

    class ServerListTransition : public QSignalTransition {
    public:
        ServerListTransition(QComboBox* cb, bool hasServers)
            : QSignalTransition(cb, SIGNAL(currentIndexChanged(int)))
            , hasServers(hasServers)
        {
        }

    protected:
        bool eventTest(QEvent* e)
        {
            if (!QSignalTransition::eventTest(e)) {
                return false;
            }
            QStateMachine::SignalEvent* se = static_cast<QStateMachine::SignalEvent*>(e);
            bool isEmpty = se->arguments().at(0).toInt() == -1;
            return (hasServers ? !isEmpty : isEmpty);
        }

    private:
        bool hasServers;
    };

    ServerListTransition* t1 = new ServerListTransition(ui->serverList, true);
    t1->setTargetState(s2_connectionReady);
    s1_noProfiles->addTransition(t1);

    ServerListTransition* t2 = new ServerListTransition(ui->serverList, false);
    t2->setTargetState(s1_noProfiles);
    s2_connectionReady->addTransition(t2);

    machine->setInitialState(s1_noProfiles);
    machine->start();
    connect(machine, &QStateMachine::started, [=]() {
        // LCA: find better way to load/fill combobox...
        this->reload_settings();

        if (!profileName.isEmpty()) {
            // TODO: better place when refactor SM...
            const int profileIndex = ui->serverList->findText(profileName);
            if (profileIndex != -1) {
                ui->serverList->setCurrentIndex(profileIndex);
                emit on_connectClicked();
                return;
            } else {
                QMessageBox::warning(this,
                    tr("Connection failed"),
                    tr("Selected VPN profile '<b>%1</b>' does not exist.").arg(profileName));
            }
        }

        OcSettings settings;
        const int currentIndex = settings.value("Profiles/currentIndex", -1).toInt();
        if (currentIndex != -1 && currentIndex < ui->serverList->count()) {
            ui->serverList->setCurrentIndex(currentIndex);
        }
    });

    QMenu* serverProfilesMenu = new QMenu(this);
    serverProfilesMenu->addAction(ui->actionNewProfile);
    serverProfilesMenu->addAction(ui->actionNewProfileAdvanced);

    m_actionNewXray = new QAction(tr("New Xray / V2Ray Profile..."), this);
    connect(m_actionNewXray, &QAction::triggered,
        this, &MainWindow::on_actionNewXrayProfile_triggered);
    serverProfilesMenu->addAction(m_actionNewXray);
    ui->menuProfiles->insertAction(ui->actionEditSelectedProfile, m_actionNewXray);

    m_actionUpdateXraySubscription = new QAction(tr("Update Xray Subscription..."), this);
    connect(m_actionUpdateXraySubscription, &QAction::triggered,
        this, &MainWindow::on_actionUpdateXraySubscription_triggered);
    serverProfilesMenu->addAction(m_actionUpdateXraySubscription);
    ui->menuProfiles->insertAction(ui->actionEditSelectedProfile, m_actionUpdateXraySubscription);

    serverProfilesMenu->addAction(ui->actionEditSelectedProfile);
    serverProfilesMenu->addAction(ui->actionRemoveSelectedProfile);

    QAction* removeAllProfiles = new QAction(tr("Remove All Profiles..."), this);
    connect(removeAllProfiles, &QAction::triggered,
        this, &MainWindow::on_actionRemoveAllProfiles_triggered);
    serverProfilesMenu->addSeparator();
    serverProfilesMenu->addAction(removeAllProfiles);
    ui->menuProfiles->addSeparator();
    ui->menuProfiles->addAction(removeAllProfiles);

    ui->serverListControl->setMenu(serverProfilesMenu);

    m_xray = new XrayClient(this);
    connect(m_xray, &XrayClient::statusChanged,
        this, [this](int status) { this->vpn_status_changed(status); });
    connect(m_xray, &XrayClient::connectedInfo,
        this, [this](QString endpoint, QString dnsText, QString cipher) {
            this->ip = endpoint;
            this->ip6.clear();
            this->dns = dnsText;
            this->cstp_cipher = cipher;
            this->dtls_cipher = cipher;
        });
    connect(m_xray, &XrayClient::statsChanged,
        this, [this](quint64 upload, quint64 download, QString detail) {
            emit stats_changed_sig(upload, download, detail);
        });
    connect(m_xray, &XrayClient::errorOccurred,
        this, [this](QString message) {
            Logger::instance().addMessage(tr("Xray: %1").arg(message), Logger::MessageType::CRITICAL);
            if (m_trayIcon != nullptr && isHidden()) {
                m_trayIcon->showMessage(tr("Xray error"), message, QSystemTrayIcon::Warning, 10000);
            } else {
                QMessageBox::warning(this, tr("Xray"), message);
            }
        });

    readSettings();
    // TODO: initial app window state machine
    m_appWindowStateMachine = new QStateMachine(this);

    QState* s111_normalWindow = new QState();
    m_appWindowStateMachine->addState(s111_normalWindow);
    s111_normalWindow->assignProperty(ui->actionRestore, "enabled", false);
    s111_normalWindow->assignProperty(ui->actionMinimize, "enabled", true);

    QState* s112_minimizedWindow = new QState();
    m_appWindowStateMachine->addState(s112_minimizedWindow);
    connect(s112_minimizedWindow, &QState::entered, [=]() {
        showMinimized();
        if (ui->actionMinimizeToTheNotificationArea->isChecked()) {
            QTimer::singleShot(10, this, SLOT(hide()));
        }
    });
    connect(s112_minimizedWindow, &QState::exited, [=]() {
        this->showNormal();
        if (ui->actionMinimizeToTheNotificationArea->isChecked()) {
            show();
            raise();
            activateWindow();
        }
    });
    s112_minimizedWindow->assignProperty(ui->actionRestore, "enabled", true);
    s112_minimizedWindow->assignProperty(ui->actionMinimize, "enabled", false);

    if (ui->actionStartMinimized->isChecked()) {
        m_appWindowStateMachine->setInitialState(s112_minimizedWindow);
    } else {
        m_appWindowStateMachine->setInitialState(s111_normalWindow);
    }

    // TODO: move outside...
    class MinimizeEventTransition : public QEventTransition {
    public:
        MinimizeEventTransition(QMainWindow* mw, Qt::WindowState state)
            : QEventTransition(mw, QEvent::WindowStateChange)
            , m_mw(mw)
            , m_state(state)
        {
        }

    protected:
        bool eventTest(QEvent* e) override
        {
            if (!QEventTransition::eventTest(e)) {
                return false;
            }
            QStateMachine::WrappedEvent* we = static_cast<QStateMachine::WrappedEvent*>(e);
            if (we->event()->type() == QEvent::WindowStateChange) {
                return (m_mw->windowState() == m_state);
            }
            return false;
        }

    private:
        QMainWindow* m_mw;
        Qt::WindowState m_state;
    };
    // TODO: move outside...
    class RestoreEventTransition : public QEventTransition {
    public:
        RestoreEventTransition(QMainWindow* mw, Qt::WindowState state)
            : QEventTransition(mw, QEvent::WindowStateChange)
            , m_mw(mw)
            , m_state(state)
        {
        }

    protected:
        bool eventTest(QEvent* e) override
        {
            if (!QEventTransition::eventTest(e)) {
                return false;
            }
            QStateMachine::WrappedEvent* we = static_cast<QStateMachine::WrappedEvent*>(e);
            if (we->event()->type() == QEvent::WindowStateChange) {
                return (m_mw->windowState() == m_state);
            }
            return false;
        }

    private:
        QMainWindow* m_mw;
        Qt::WindowState m_state;
    };

    MinimizeEventTransition* minimizeEvent = new MinimizeEventTransition(this, Qt::WindowMinimized);
    minimizeEvent->setTargetState(s112_minimizedWindow);
    s111_normalWindow->addTransition(minimizeEvent);

    RestoreEventTransition* restoreEvent = new RestoreEventTransition(this, Qt::WindowNoState);
    restoreEvent->setTargetState(s111_normalWindow);
    s112_minimizedWindow->addTransition(restoreEvent);

    // start timer to check latest version
    QTimer::singleShot(4000, this, &MainWindow::tryCheckLatestVersion);

    m_appWindowStateMachine->start();
}

static void term_thread(MainWindow* m, SOCKET* fd)
{
    char cmd = OC_CMD_CANCEL;

    if (*fd != INVALID_SOCKET) {
        m->vpn_status_changed(STATUS_DISCONNECTING);
        int ret = pipe_write(*fd, &cmd, 1);
        if (ret < 0) {
            Logger::instance().addMessage(QObject::tr("term_thread: IPC error: %1").arg(net_errno));
        }
        *fd = INVALID_SOCKET;
        ms_sleep(200);
    } else {
        m->vpn_status_changed(STATUS_DISCONNECTED);
    }
}

MainWindow::~MainWindow()
{
    int counter = 10;
    if (this->timer->isActive()) {
        timer->stop();
    }

    if (this->futureWatcher.isRunning() == true) {
        term_thread(this, &this->cmd_fd);
    }
    while (this->futureWatcher.isRunning() == true && counter > 0) {
        ms_sleep(200);
        counter--;
    }

    if (m_xray != nullptr) {
        m_xray->stop();
    }

    writeSettings();

    delete ui;
    delete timer;
    delete blink_timer;
    delete manager;
}

void MainWindow::checkLatestVersion() const
{
    QNetworkRequest req(QUrl(GITLAB_LATEST_RELEASE_URL));

    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);

    connect(manager, &QNetworkAccessManager::finished,
            this, &MainWindow::gotLatestVersion);

    Logger::instance().addMessage(QObject::tr("Checking for current version"));
    manager->get(req);
}

void MainWindow::tryCheckLatestVersion()
{
    time_t now = time(0);

    // Check during start up only a time every a few days to avoid
    // overloading gitlab.
    if (last_check_time == 0) {
        OcSettings settings;
        last_check_time = settings.value("Settings/last-check-time").toLongLong();
    }

    if (now - last_check_time < 5*86400) {
        Logger::instance().addMessage(QObject::tr("Skipping automatic check for current version"));
        return;
    }

    checkLatestVersion();
}

void MainWindow::gotLatestVersion(QNetworkReply *reply)
{
    QString version;
    version = reply->rawHeader("Location");

    Logger::instance().addMessage(QObject::tr("Version location: %1").arg(version));

    if (version.isEmpty() != true) {
        qsizetype n=version.lastIndexOf("/");
        if (n != -1) {
            // skip '/v'
            this->latest_version = version.mid(n+2);
            Logger::instance().addMessage(QObject::tr("Latest available version is %1, current %2").arg(this->latest_version).arg(INTERNAL_PROJECT_VERSION));

            if (m_trayIcon && m_trayIcon->supportsMessages() && latest_version.compare(INTERNAL_PROJECT_VERSION) != 0) {
                m_trayIcon->showMessage(tr("New version available"), tr("%1 version %2 is available!").arg(QLatin1String(PRODUCT_NAME_SHORT)).arg(this->latest_version));
            }
        } else {
            Logger::instance().addMessage(QObject::tr("Unable to identify current version from %1").arg(version));
        }
    } else {
        Logger::instance().addMessage(QObject::tr("Unable to identify current version: %1").arg(reply->errorString()));
    }

    emit version_download_completed_sig();
    reply->deleteLater();
}

void MainWindow::vpn_status_changed(int connected)
{
    emit vpn_status_changed_sig(connected);
}

void MainWindow::vpn_status_changed(int connected, QString& dns, QString& ip, QString& ip6, QString& cstp_cipher, QString& dtls_cipher)
{
    this->dns = dns;
    this->ip = ip;
    this->ip6 = ip6;
    this->dtls_cipher = dtls_cipher;
    this->cstp_cipher = cstp_cipher;

    emit vpn_status_changed_sig(connected);
}

QString MainWindow::normalize_byte_size(uint64_t bytes)
{
    const unsigned unit = 1024; // TODO: add support for SI units? (optional)
    if (bytes < unit) {
        return QString("%1 B").arg(QString::number(bytes));
    }
    const int exp = static_cast<int>(std::log(bytes) / std::log(unit));
    static const char suffixChar[] = "KMGTPE";
    return QString("%1 %2B").arg(QString::number(bytes / std::pow(unit, exp), 'f', 3)).arg(suffixChar[exp - 1]);
}

QString MainWindow::normalize_speed(double bytes_per_second)
{
    const unsigned unit = 1024;
    if (bytes_per_second < 1.0) {
        return QStringLiteral("0 B/s");
    }
    if (bytes_per_second < unit) {
        return QStringLiteral("%1 B/s").arg(QString::number(bytes_per_second, 'f', 0));
    }
    const int exp = static_cast<int>(std::log(bytes_per_second) / std::log(unit));
    static const char suffixChar[] = "KMGTPE";
    return QStringLiteral("%1 %2B/s")
        .arg(QString::number(bytes_per_second / std::pow(unit, exp), 'f', 2))
        .arg(suffixChar[exp - 1]);
}

void MainWindow::resetTransferStats()
{
    m_lastTxBytes = 0;
    m_lastRxBytes = 0;
    m_lastStatsAtMs = 0;
}

void MainWindow::statsChanged(quint64 tx, quint64 rx, QString dtls)
{
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    double upload_speed = 0.0;
    double download_speed = 0.0;

    if (m_lastStatsAtMs > 0 && now > m_lastStatsAtMs) {
        const qint64 elapsed = now - m_lastStatsAtMs;
        if (tx >= m_lastTxBytes) {
            upload_speed = static_cast<double>(tx - m_lastTxBytes) * 1000.0 / elapsed;
        }
        if (rx >= m_lastRxBytes) {
            download_speed = static_cast<double>(rx - m_lastRxBytes) * 1000.0 / elapsed;
        }
    }

    m_lastTxBytes = tx;
    m_lastRxBytes = rx;
    m_lastStatsAtMs = now;

    ui->downloadLabel->setText(QStringLiteral("%1  \u00b7  %2")
                                   .arg(normalize_speed(download_speed), normalize_byte_size(rx)));
    ui->uploadLabel->setText(QStringLiteral("%1  \u00b7  %2")
                                 .arg(normalize_speed(upload_speed), normalize_byte_size(tx)));
    ui->cipherDTLSLabel->setText(dtls);
}

void MainWindow::updateStats(const struct oc_stats* stats, QString dtls)
{
    emit stats_changed_sig(stats->tx_bytes, stats->rx_bytes, dtls);
}

#define PREFIX "server:" // LCA: remove this...
void MainWindow::reload_settings()
{
    ui->serverList->clear();
    if (m_trayIcon) {
        m_trayIconMenuConnections->clear();
    }

    OcSettings settings;
    for (const auto& key : settings.allKeys()) {
        if (key.startsWith(PREFIX) && key.endsWith("/server")) {
            QString str{ key };
            str.remove(0, sizeof(PREFIX) - 1); /* remove prefix */
            str.remove(str.size() - 7, 7); /* remove /server suffix */
            ui->serverList->addItem(str);
            if (settings.contains(QStringLiteral("server:%1/xray-config").arg(str))) {
                ui->serverList->setItemData(ui->serverList->count() - 1,
                    tr("Xray / V2Ray profile"), Qt::ToolTipRole);
            }

            if (m_trayIcon) {
                QAction* act = m_trayIconMenuConnections->addAction(str);
                connect(act, &QAction::triggered, [act, this]() {
                    int idx = ui->serverList->findText(act->text());
                    if (idx != -1) {
                        ui->serverList->setCurrentIndex(idx);
                        on_connectClicked();
                    }
                });
            }
        }
    }
}

void MainWindow::blink_ui()
{
    static unsigned t = 1;

    if (t % 2 == 0) {
        ui->iconLabel->setPixmap(CONNECTING_ICON);
    } else {
        ui->iconLabel->setPixmap(CONNECTING_ICON2);
    }
    ++t;
}

void MainWindow::changeStatus(int val)
{
    if (val == STATUS_CONNECTED) {

        blink_timer->stop();

        ui->serverList->setEnabled(false);

        if (m_trayIcon) {
            m_trayIconMenuConnections->setEnabled(false);
            m_disconnectAction->setEnabled(true);
        }

        ui->iconLabel->setPixmap(ON_ICON);
        ui->connectionButton->setIcon(QIcon(":/images/process-stop.png"));
        ui->connectionButton->setText(tr("Disconnect"));

        QFileSelector selector;
        if (m_trayIcon) {
            QIcon icon(selector.select(QStringLiteral(":/images/network-connected.png")));
            icon.setIsMask(true);
            m_trayIcon->setIcon(icon);
        }

        this->ui->ipV4Label->setText(ip);
        this->ui->ipV6Label->setText(ip6);
        this->ui->dnsLabel->setText(dns);
        this->ui->cipherCSTPLabel->setText(cstp_cipher);
        this->ui->cipherDTLSLabel->setText(dtls_cipher);

        timer->start(UPDATE_TIMER);

        if (this->minimize_on_connect) {
            if (m_trayIcon) {
                hide();
                m_trayIcon->showMessage(QLatin1String("Connected"), QLatin1String("You are connected to ") + ui->serverList->currentText(),
                    QSystemTrayIcon::Information,
                    10000);
            } else {
                this->setWindowState(Qt::WindowMinimized);
            }
        }

        if (m_trayIcon) {
            m_trayIcon->setToolTip(QLatin1String("Connected to ") + ui->serverList->currentText());
        }
    } else if (val == STATUS_CONNECTING) {
        resetTransferStats();

        if (m_trayIcon) {
            QFileSelector selector;
            QIcon icon(selector.select(QStringLiteral(":/images/network-disconnected.png")));
            icon.setIsMask(true);
            m_trayIcon->setIcon(icon);
            m_trayIcon->setToolTip(QLatin1String("Connecting to ") + ui->serverList->currentText());
        }

        ui->serverList->setEnabled(false);

        if (m_trayIcon) {
            m_trayIconMenuConnections->setEnabled(false);
            m_disconnectAction->setEnabled(true);
        }

        ui->iconLabel->setPixmap(CONNECTING_ICON);
        ui->connectionButton->setIcon(QIcon(":/images/process-stop.png"));
        ui->connectionButton->setText(tr("Cancel"));
        blink_timer->start(1500);

        disconnect(ui->connectionButton, &QPushButton::clicked,
            this, &MainWindow::on_connectClicked);
        connect(ui->connectionButton, &QPushButton::clicked,
            this, &MainWindow::on_disconnectClicked,
            Qt::QueuedConnection);
    } else if (val == STATUS_DISCONNECTED) {
        resetTransferStats();
        blink_timer->stop();
        if (this->timer->isActive()) {
            timer->stop();
        }
        cmd_fd = INVALID_SOCKET;

        ui->ipV4Label->clear();
        ui->ipV6Label->clear();
        ui->dnsLabel->clear();
        ui->uploadLabel->clear();
        ui->downloadLabel->clear();
        ui->cipherCSTPLabel->clear();
        ui->cipherDTLSLabel->clear();
        Logger::instance().addMessage(QObject::tr("Disconnected"));

        ui->serverList->setEnabled(true);

        if (m_trayIcon) {
            m_trayIconMenuConnections->setEnabled(true);
            m_disconnectAction->setEnabled(false);
        }

        ui->iconLabel->setPixmap(OFF_ICON);
        ui->connectionButton->setEnabled(true);
        ui->connectionButton->setIcon(QIcon(":/images/network-wired.png"));
        ui->connectionButton->setText(tr("Connect"));

        if (m_trayIcon) {
            QFileSelector selector;
            QIcon icon(selector.select(QStringLiteral(":/images/network-disconnected.png")));
            icon.setIsMask(true);
            m_trayIcon->setIcon(icon);

            if (this->isHidden() == true)
                m_trayIcon->showMessage(QLatin1String("Disconnected"), QLatin1String("You were disconnected from the VPN"),
                    QSystemTrayIcon::Warning,
                    10000);

            m_trayIcon->setToolTip(QLatin1String("Disconnected"));
        }
        disconnect(ui->connectionButton, &QPushButton::clicked,
            this, &MainWindow::on_disconnectClicked);
        connect(ui->connectionButton, &QPushButton::clicked,
            this, &MainWindow::on_connectClicked,
            Qt::QueuedConnection);

        emit readyToShutdown();
    } else if (val == STATUS_DISCONNECTING) {
        ui->iconLabel->setPixmap(CONNECTING_ICON);
        ui->connectionButton->setIcon(QIcon(":/images/process-stop.png"));
        ui->connectionButton->setEnabled(false);
        blink_timer->start(1500);

        if (m_trayIcon)
            m_trayIcon->setToolTip(QLatin1String("Disconnecting from ") + ui->serverList->currentText());
    } else {
        qDebug() << "TODO: was is das?";
    }
}

static void main_loop(VpnInfo* vpninfo, MainWindow* m)
{
    m->vpn_status_changed(STATUS_CONNECTING);

    bool pass_was_empty;
    bool reset_password = false;
    pass_was_empty = vpninfo->ss->get_password().isEmpty();

    QString ip, ip6, dns, cstp, dtls;

    int ret = 0;
    bool retry = false;
    int retries = 2;
    do {
        retry = false;
        ret = vpninfo->connect();
        if (ret != 0) {
            if (retries-- <= 0)
                goto fail;

            QString oldpass, oldgroup;
            if (pass_was_empty != true) {
                /* authentication failed in batch mode? switch to non
                 * batch and retry */
                oldpass = vpninfo->ss->get_password();
                oldgroup = vpninfo->ss->get_groupname();
                vpninfo->ss->clear_password();
                vpninfo->ss->clear_groupname();
                retry = true;
                reset_password = true;
                Logger::instance().addMessage(QObject::tr("Authentication failed in batch mode, retrying with batch mode disabled"));
                vpninfo->reset_vpn();
                continue;
            }

            /* if we didn't manage to connect on a retry, the failure reason
             * may not have been a changed password, reset it */
            if (reset_password == true) {
                vpninfo->ss->set_password(oldpass);
                vpninfo->ss->set_groupname(oldgroup);
            }

            Logger::instance().addMessage(vpninfo->last_err);
            goto fail;
        }

    } while (retry == true);

    vpninfo->get_info(dns, ip, ip6);
    vpninfo->get_cipher_info(cstp, dtls);
    m->vpn_status_changed(STATUS_CONNECTED, dns, ip, ip6, cstp, dtls);

    vpninfo->ss->save();
    vpninfo->mainloop();

fail: // LCA: drop this 'goto' and optimize values...
    m->vpn_status_changed(STATUS_DISCONNECTED);

    delete vpninfo;
}

void MainWindow::on_disconnectClicked()
{
    if (this->timer->isActive()) {
        this->timer->stop();
    }
    Logger::instance().addMessage(QObject::tr("Disconnecting..."));

    if (m_xray != nullptr && m_xray->isRunning()) {
        m_xray->stop();
        return;
    }

    term_thread(this, &this->cmd_fd);
}

void MainWindow::connectXray(const QString& label, const QString& config, const XrayRuntimeOptions& options)
{
    if (m_xray == nullptr) {
        m_xray = new XrayClient(this);
    }
    this->minimize_on_connect = false;
    m_xray->start(label, config, options);
}

void MainWindow::on_connectClicked()
{
    VpnInfo* vpninfo = nullptr;
    StoredServer* ss = nullptr;
    QFuture<void> future;
    QString name, url;
    QList<QNetworkProxy> proxies;
    QUrl turl;
    QNetworkProxyQuery query;
    int rval;

    if (this->cmd_fd != INVALID_SOCKET) {
        QMessageBox::information(this,
            qApp->applicationName(),
            tr("A previous VPN instance is still running (socket is active)"));
        return;
    }

    if (this->futureWatcher.isRunning() == true) {
        QMessageBox::information(this,
            qApp->applicationName(),
            tr("A previous VPN instance is still running"));
        return;
    }

    if (m_xray != nullptr && m_xray->isRunning()) {
        QMessageBox::information(this,
            qApp->applicationName(),
            tr("A previous Xray connection is still running"));
        return;
    }

    if (ui->serverList->currentText().isEmpty()) {
        QMessageBox::information(this,
            qApp->applicationName(),
            tr("You need to specify a gateway. e.g. vpn.example.com:443"));
        return;
    }

    ss = new StoredServer();

    name = ui->serverList->currentText();
    rval = ss->load(name);

    if (ss->is_xray()) {
        const QString label = name;
        const QString config = ss->get_xray_config();

        XrayRuntimeOptions options;
        options.tunMode = ss->get_xray_mode().compare(QStringLiteral("proxy"), Qt::CaseInsensitive) != 0;
        options.dnsServers = ss->get_xray_dns().split(
            QRegularExpression(QStringLiteral("[\\s,;]+")), Qt::SkipEmptyParts);
        options.sniffing = ss->get_xray_sniffing();
        options.blockQuic = ss->get_xray_block_quic();
        options.fragment = ss->get_xray_fragment();
        options.fragmentPackets = ss->get_xray_fragment_packets();
        options.fragmentLength = ss->get_xray_fragment_length();
        options.fragmentDelay = ss->get_xray_fragment_interval();
        options.serverOverride = ss->get_xray_server_override();
        options.portOverride = ss->get_xray_port_override();

        delete ss;
        connectXray(label, config, options);
        return;
    }

    if (rval == 0) { // new entry
        // remove http?:// from string
        if (name.contains("/")) {
            turl = QUrl::fromUserInput(name);
            name = NewProfileDialog::urlToName(turl);
        } else {
            turl = QUrl::fromUserInput("https://" + name);
        }

        if (turl.isValid() == false) {
            delete ss;
            goto fail;
        }

        if (rval == 0) { // if a new server ask and set the protocol
            NewProfileDialog dialog(this);

            dialog.setUrl(turl);
            dialog.setQuickConnect();
            if (dialog.exec() != QDialog::Accepted) {
                delete ss;
                return;
            }
            //dialog saves the host, so we load again
            name = dialog.getNewProfileName();
            ss->load(name);
        }

        if (name.compare(ui->serverList->currentText()) != 0) {
            // user typed https:// to a new entry. Replace the text of it
            // with the actual name.
            ui->serverList->setItemText(ui->serverList->currentIndex(), name);
        }
    } else {
        name = ss->get_server_gateway();
        if (name.contains("https://", Qt::CaseInsensitive)) {
            turl.setUrl(ss->get_server_gateway());
        } else {
            turl.setUrl("https://" + ss->get_server_gateway());
        }
    }

    query.setUrl(turl);

    /* ss is now deallocated by vpninfo */
    try {
        vpninfo = new VpnInfo(QStringLiteral("AnyConnect-compatible OpenConnect GUI VPN Agent"), ss, this);
    } catch (std::exception& ex) {
        QMessageBox::information(this,
            qApp->applicationName(),
            tr("There was an issue initializing the VPN (%1).").arg(ex.what()));
        goto fail;
    }

    this->minimize_on_connect = vpninfo->get_minimize();

    vpninfo->setUrl(turl);

    this->cmd_fd = vpninfo->get_cmd_fd();
    if (this->cmd_fd == INVALID_SOCKET) {
        QMessageBox::information(this,
            qApp->applicationName(),
            tr("There was an issue establishing IPC with openconnect; try restarting the application."));
        goto fail;
    }

    if (ss->get_proxy()) {
        proxies = QNetworkProxyFactory::systemProxyForQuery(query);
        if (proxies.size() > 0 && proxies.at(0).type() != QNetworkProxy::NoProxy) {
            if (proxies.at(0).type() == QNetworkProxy::Socks5Proxy)
                url = "socks5://";
            else if (proxies.at(0).type() == QNetworkProxy::HttpCachingProxy
                || proxies.at(0).type() == QNetworkProxy::HttpProxy)
                url = "http://";

            if (url.isEmpty() == false) {

                QString str;
                if (proxies.at(0).user().isEmpty() != true) {
                    str = proxies.at(0).user() + ":" + proxies.at(0).password() + "@";
                }
                str += proxies.at(0).hostName();
                if (proxies.at(0).port() != 0) {
                    str += ":" + QString::number(proxies.at(0).port());
                }

                Logger::instance().addMessage(tr("Setting proxy to: %1").arg(str));

                int ret = openconnect_set_http_proxy(vpninfo->vpninfo, str.toUtf8().data());
                if (ret != 0) {
                    Logger::instance().addMessage(tr("Unexpected error setting proxy"));
                }
            }
        }
    }

    future = QtConcurrent::run(main_loop, vpninfo, this);

    this->futureWatcher.setFuture(future);

    return;
fail: // LCA: remote 'fail' label :/
    delete vpninfo;
}

void MainWindow::closeEvent(QCloseEvent* event)
{
    if (m_trayIcon && m_trayIcon->isVisible() && ui->actionMinimizeTheApplicationInsteadOfClosing->isChecked()) {
        this->showMinimized();
        event->ignore();
    } else {
        event->accept();

        const bool vpnActive = (m_xray != nullptr && m_xray->isRunning())
            || (m_trayIcon != nullptr && m_disconnectAction->isEnabled());
        if (vpnActive) {
            connect(this, &MainWindow::readyToShutdown,
                qApp, &QApplication::quit);
            on_disconnectClicked();
        } else {
            qApp->quit();
        }
    }
    QMainWindow::closeEvent(event);
}

void MainWindow::request_update_stats()
{
    if (m_xray != nullptr && m_xray->isRunning()) {
        return;
    }

    char cmd = OC_CMD_STATS;
    if (this->cmd_fd != INVALID_SOCKET) {
        int ret = pipe_write(this->cmd_fd, &cmd, 1);
        if (ret < 0) {
            Logger::instance().addMessage(QObject::tr("update_stats: IPC error: %1").arg(net_errno));
            if (this->timer->isActive())
                this->timer->stop();
        }
    } else {
        Logger::instance().addMessage(QObject::tr("update_stats: invalid socket"));
        if (this->timer->isActive())
            this->timer->stop();
    }
}

void MainWindow::readSettings()
{
    OcSettings settings;
    settings.beginGroup("MainWindow");
    if (settings.contains("geometry")) {
        restoreGeometry(settings.value("geometry").toByteArray());
    }

    //remove old settings if they exist
    if (settings.contains("size")) {
        settings.remove("size");
    }
    if (settings.contains("pos")) {
        settings.remove("pos");
    }
    settings.endGroup();

    settings.beginGroup("Settings");
    ui->actionMinimizeToTheNotificationArea->setChecked(settings.value("minimizeToTheNotificationArea", true).toBool());
    ui->actionMinimizeTheApplicationInsteadOfClosing->setChecked(settings.value("minimizeTheApplicationInsteadOfClosing", true).toBool());
    ui->actionStartMinimized->setChecked(settings.value("startMinimized", false).toBool());

    ui->actionSingleInstanceMode->setChecked(settings.value("singleInstanceMode", true).toBool());
    connect(ui->actionSingleInstanceMode, &QAction::toggled, [](bool checked) {
        OcSettings settings;
        settings.setValue("Settings/singleInstanceMode", checked);
    });

    int loglevel = settings.value("logLevel", PRG_INFO).toInt();
    int action_idx = app_loglevel_tab(loglevel);

    if (action_idx == -1 )
        action_idx = app_loglevel_tab(PRG_INFO); //fallback to default

    ui->LogLevelGroup->actions().at(action_idx)->setChecked(true);

    settings.endGroup();
}

void MainWindow::writeSettings()
{
    OcSettings settings;
    settings.beginGroup("MainWindow");
    settings.setValue("geometry", saveGeometry());
    settings.endGroup();

    settings.beginGroup("Settings");
    if (last_check_time > 0)
        settings.setValue("last-check-time", qint64(last_check_time));
    settings.setValue("minimizeToTheNotificationArea", ui->actionMinimizeToTheNotificationArea->isChecked());
    settings.setValue("minimizeTheApplicationInsteadOfClosing", ui->actionMinimizeTheApplicationInsteadOfClosing->isChecked());
    settings.setValue("startMinimized", ui->actionStartMinimized->isChecked());
    settings.setValue("singleInstanceMode", ui->actionSingleInstanceMode->isChecked());
    settings.setValue("logLevel", this->get_log_level());
    settings.endGroup();

    settings.setValue("Profiles/currentIndex", ui->serverList->currentIndex());
}

void MainWindow::createLogDialog()
{
    auto dialog{ new LogDialog() };

    disconnect(ui->viewLogButton, &QPushButton::clicked,
        this, &MainWindow::createLogDialog);

    connect(ui->viewLogButton, &QPushButton::clicked,
        dialog, &QDialog::show);
    connect(ui->viewLogButton, &QPushButton::clicked,
        dialog, &QDialog::raise);
    connect(ui->viewLogButton, &QPushButton::clicked,
        dialog, &QDialog::activateWindow);

    connect(dialog, &QDialog::finished,
        [this]() {
            connect(ui->viewLogButton, &QPushButton::clicked,
                this, &MainWindow::createLogDialog);
        });
    connect(dialog, &QDialog::finished,
        dialog, &QDialog::deleteLater);

    dialog->show();
    dialog->raise();
    dialog->activateWindow();
}

void MainWindow::createTrayIcon()
{
    m_trayIconMenu = new QMenu(this);

    m_trayIconMenuConnections = new QMenu(this);
    m_trayIconMenu->addMenu(m_trayIconMenuConnections);
    m_disconnectAction = new QAction(tr("Disconnect"), this);
    m_trayIconMenu->addAction(m_disconnectAction);
    connect(m_disconnectAction, &QAction::triggered,
        this, &MainWindow::on_disconnectClicked);

    m_trayIconMenu->addSeparator();
    m_trayIconMenu->addAction(ui->actionLogWindow);
    m_trayIconMenu->addSeparator();
    m_trayIconMenu->addAction(ui->actionMinimize);
    m_trayIconMenu->addAction(ui->actionRestore);
    m_trayIconMenu->addSeparator();
    m_trayIconMenu->addAction(ui->actionQuit);

    m_trayIcon = new QSystemTrayIcon(this);
    m_trayIcon->setContextMenu(m_trayIconMenu);
    m_trayIcon->setToolTip(QLatin1String("Disconnected"));
}

void MainWindow::iconActivated(QSystemTrayIcon::ActivationReason reason)
{
    switch (reason) {
    case QSystemTrayIcon::Trigger:
    case QSystemTrayIcon::DoubleClick:
    case QSystemTrayIcon::MiddleClick:
#ifdef Q_OS_WIN
        if (isMinimized()) {
            showNormal();
        } else {
            showMinimized();
        }
#endif
        break;
    default:
        break;
    }
}

void MainWindow::on_actionNewProfile_triggered()
{
    NewProfileDialog dialog(this);
    connect(&dialog, &NewProfileDialog::connect,
        this, &MainWindow::on_connectClicked,
        Qt::QueuedConnection);
    if (dialog.exec() != QDialog::Accepted) {
        return;
    }

    reload_settings();
    ui->serverList->setCurrentText(dialog.getNewProfileName());
}

void MainWindow::on_actionNewProfileAdvanced_triggered()
{
    // TODO: the new profile has no name yet...
    EditDialog dialog("", this);
    if (dialog.exec() != QDialog::Accepted) {
        return;
    }

    reload_settings();
    ui->serverList->setCurrentText(dialog.getEditedProfileName());
}

void MainWindow::on_actionNewXrayProfile_triggered()
{
    XrayProfileDialog dialog(this);
    if (dialog.exec() != QDialog::Accepted) {
        return;
    }

    reload_settings();
    const QStringList profiles = dialog.getSavedProfiles();
    if (profiles.isEmpty() == false) {
        ui->serverList->setCurrentText(profiles.first());
    }
}

void MainWindow::on_actionUpdateXraySubscription_triggered()
{
    if (m_subscriptionUpdateRunning) {
        return;
    }

    QStringList urls = XraySubscriptionUpdater::subscriptionUrls();
    if (urls.isEmpty()) {
        QMessageBox::information(this, tr("Update subscription"),
            tr("No Xray profile has a saved subscription URL. Edit a profile and save its subscription URL first."));
        return;
    }

    QString url = urls.first();
    if (urls.size() > 1) {
        bool ok = false;
        url = QInputDialog::getItem(this, tr("Update subscription"),
            tr("Choose the subscription to update:"), urls, 0, false, &ok);
        if (ok == false) {
            return;
        }
    }

    m_subscriptionUpdateRunning = true;
    if (m_actionUpdateXraySubscription != nullptr) {
        m_actionUpdateXraySubscription->setEnabled(false);
    }

    QNetworkAccessManager* manager = new QNetworkAccessManager(this);
    QNetworkRequest request{QUrl(url)};
    request.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("OpenConnect-GUI-Xray/1.0"));
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    request.setTransferTimeout(20000);

    auto progress = new QProgressDialog(tr("Updating Xray subscription..."), QString(), 0, 0, this);
    progress->setWindowTitle(tr("Update subscription"));
    progress->setWindowModality(Qt::WindowModal);
    progress->setCancelButton(nullptr);
    progress->setMinimumDuration(0);
    progress->show();

    QNetworkReply* reply = manager->get(request);
    connect(reply, &QNetworkReply::finished, this, [this, reply, manager, progress, url]() {
        m_subscriptionUpdateRunning = false;
        if (m_actionUpdateXraySubscription != nullptr) {
            m_actionUpdateXraySubscription->setEnabled(true);
        }
        progress->close();
        progress->deleteLater();
        manager->deleteLater();

        const bool networkError = reply->error() != QNetworkReply::NoError;
        const QString networkMessage = reply->errorString();
        const QByteArray payload = reply->readAll();
        reply->deleteLater();

        if (networkError) {
            QMessageBox::warning(this, tr("Update subscription"),
                tr("Could not fetch the subscription: %1").arg(networkMessage));
            return;
        }

        QString text;
        QString error;
        if (XrayConfig::decodeSubscription(payload, text, error) == false) {
            QMessageBox::warning(this, tr("Update subscription"), error);
            return;
        }

        QVector<XrayImportedProfile> nodes;
        if (XrayConfig::importText(text, nodes, error) == false) {
            QMessageBox::warning(this, tr("Update subscription"), error);
            return;
        }

        const XraySubscriptionSyncResult result = XraySubscriptionUpdater::sync(url, nodes);
        if (result.ok == false) {
            QMessageBox::warning(this, tr("Update subscription"), result.error);
            return;
        }

        int removed = 0;
        if (result.stale.isEmpty() == false) {
            const auto answer = QMessageBox::question(this, tr("Update subscription"),
                tr("%1 profile(s) from this subscription are no longer listed:\n%2\n\nRemove them?")
                    .arg(result.stale.size())
                    .arg(result.stale.join(QStringLiteral(", "))),
                QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes);
            if (answer == QMessageBox::Yes) {
                removed = XraySubscriptionUpdater::removeProfiles(result.stale);
            }
        }

        const QString current = ui->serverList->currentText();
        reload_settings();
        if (current.isEmpty() == false && ui->serverList->findText(current) >= 0) {
            ui->serverList->setCurrentText(current);
        }

        const int kept = result.stale.size() - removed;
        Logger::instance().addMessage(tr("Subscription update: %1 updated, %2 added, %3 removed, %4 kept")
                                          .arg(result.updated)
                                          .arg(result.added)
                                          .arg(removed)
                                          .arg(kept));

        QMessageBox::information(this, tr("Subscription updated"),
            tr("Updated: %1\nAdded: %2\nRemoved: %3\nKept (no longer in subscription): %4")
                .arg(result.updated)
                .arg(result.added)
                .arg(removed)
                .arg(kept));
    });
}

void MainWindow::on_actionEditSelectedProfile_triggered()
{
    QString current = ui->serverList->currentText();

    StoredServer probe;
    probe.load(current);
    if (probe.is_xray()) {
        XrayProfileDialog dialog(current, this);
        if (dialog.exec() != QDialog::Accepted) {
            return;
        }

        reload_settings();
        const QStringList profiles = dialog.getSavedProfiles();
        ui->serverList->setCurrentText(profiles.isEmpty() ? current : profiles.first());
        return;
    }

    EditDialog dialog(current, this);
    if (dialog.exec() != QDialog::Accepted) {
        return;
    }

    reload_settings();
    ui->serverList->setCurrentText(dialog.getEditedProfileName());
}

#define PREFIX "server:" // LCA: remote this...
void MainWindow::on_actionRemoveSelectedProfile_triggered()
{
    QMessageBox mbox;
    mbox.setText(tr("Are you sure you want to remove '%1' host?").arg(ui->serverList->currentText()));
    mbox.setStandardButtons(QMessageBox::Cancel | QMessageBox::Ok);
    mbox.setDefaultButton(QMessageBox::Cancel);
    mbox.addButton(tr("Remove"), QMessageBox::DestructiveRole);

    if (mbox.exec() == QMessageBox::Ok) {
        OcSettings settings;
        QString prefix = PREFIX;
        for (const auto& key : settings.allKeys()) {
            //qDebug() << key << ":" << QString(prefix + ui->serverList->currentText());
            if (key.startsWith(prefix + ui->serverList->currentText() + "/")) {
                settings.remove(key);
            }
        }

        reload_settings(); // LCA: remove this feature...
    }
}

void MainWindow::on_actionRemoveAllProfiles_triggered()
{
    const bool active = (m_xray != nullptr && m_xray->isRunning())
        || this->futureWatcher.isRunning()
        || this->cmd_fd != INVALID_SOCKET;
    if (active) {
        QMessageBox::information(this, tr("Remove all profiles"),
            tr("Disconnect the active connection before removing profiles."));
        return;
    }

    OcSettings settings;
    int count = 0;
    for (const QString& key : settings.allKeys()) {
        if (key.startsWith(QStringLiteral("server:")) && key.endsWith(QStringLiteral("/server"))) {
            ++count;
        }
    }

    if (count == 0) {
        QMessageBox::information(this, tr("Remove all profiles"), tr("There are no profiles to remove."));
        return;
    }

    QMessageBox mbox;
    mbox.setIcon(QMessageBox::Warning);
    mbox.setText(tr("Remove all %1 profiles?").arg(count));
    mbox.setInformativeText(tr("This removes every OpenConnect and Xray/V2Ray profile and cannot be undone."));
    mbox.setStandardButtons(QMessageBox::Cancel);
    QAbstractButton* removeButton = mbox.addButton(tr("Remove All"), QMessageBox::DestructiveRole);
    mbox.setDefaultButton(QMessageBox::Cancel);
    mbox.exec();

    if (mbox.clickedButton() != removeButton) {
        return;
    }

    const QStringList keys = settings.allKeys();
    for (const QString& key : keys) {
        if (key.startsWith(QStringLiteral("server:"))) {
            settings.remove(key);
        }
    }
    settings.remove(QStringLiteral("Profiles/currentIndex"));

    Logger::instance().addMessage(tr("All VPN profiles removed"));
    reload_settings();
    ui->serverList->setCurrentIndex(-1);
}

void MainWindow::on_actionAbout_triggered()
{
    QString txt = QLatin1String("<h2>") + QLatin1String(PRODUCT_NAME_LONG) + QLatin1String("</h2>");

    if (QLatin1String(PROJECT_VERSION).contains(QLatin1String("-g"))) {
        txt += tr("Development snapshot <i>%1</i> (%2 bit)<br>").arg(PROJECT_VERSION).arg(QSysInfo::buildCpuArchitecture() == QLatin1String("i386") ? 32 : 64);
        txt += tr("Built at <i>%1</i><br>").arg(QLatin1String(appBuildOn));
    } else {
        txt += tr("Version <i>%1</i> (%2 bit)<br>").arg(PROJECT_VERSION).arg(QSysInfo::buildCpuArchitecture() == QLatin1String("i386") ? 32 : 64);
    }

    txt += tr("<br><i>%1</i> is free software developed by the OpenConnect GUI project community. See the license for more information.<br>").arg(APP_NAME);

    txt += tr("<br>Visit <a href=\"%1\">our community web site</a> for more information, to contribute, file a bug or suggest a new feature.<br>").arg(CMAKE_PROJECT_HOMEPAGE_URL);

    QMessageBox::about(this, QLatin1String("About"), txt);
}

void MainWindow::checkForUpdatesDialog()
{
    if (downloadProgress != nullptr) {
        disconnect(this, &MainWindow::version_download_completed_sig,
                   this, &MainWindow::checkForUpdatesDialog);
        this->downloadProgress->setValue(100);
        downloadProgress->done(0);
        delete downloadProgress;
        downloadProgress = nullptr;
    }


    QMessageBox mbox;
    QUrl getUri;
    mbox.setStandardButtons(QMessageBox::Ok);
    mbox.setDefaultButton(QMessageBox::Ok);
    QString txt = QLatin1String("<h2>") + QLatin1String(PRODUCT_NAME_LONG) + QLatin1String("</h2>");

    txt += tr("<h3>Current version</h3>");
    if (QLatin1String(PROJECT_VERSION).contains(QLatin1String("-g"))) {
        txt += tr("Development snapshot <i>%1</i> (%2 bit)<br>").arg(PROJECT_VERSION).arg(QSysInfo::buildCpuArchitecture() == QLatin1String("i386") ? 32 : 64);
        txt += tr("Built at <i>%1</i><br>").arg(QLatin1String(appBuildOn));
    } else {
        txt += tr("Version <i>%1</i> (%2 bit)<br>").arg(PROJECT_VERSION).arg(QSysInfo::buildCpuArchitecture() == QLatin1String("i386") ? 32 : 64);
    }

    txt += tr("<h3>Latest version</h3>");
    if (latest_version.isEmpty()) {
        txt += tr("N/A<br>");
    } else {
        if (latest_version.compare(INTERNAL_PROJECT_VERSION) != 0) {
            txt += tr("Latest version is <i>%1</i><br><br>").arg(latest_version);
#ifdef Q_OS_WIN
            mbox.addButton(tr("Download %1").arg(latest_version), QMessageBox::AcceptRole);
            getUri = QUrl(tr(APP_DOWNLOAD_WIN_URL).arg(latest_version));
#else
            mbox.addButton(tr("Get %1").arg(latest_version), QMessageBox::AcceptRole);
            getUri = QUrl(APP_RELEASES_URL);
#endif
        } else {
            txt += tr("You are up to date. Latest version is %1.<br>").arg(latest_version);
        }
    }

    mbox.setInformativeText(txt);
    mbox.setWindowTitle(QLatin1String("Check for updates"));

    if (mbox.exec() == QMessageBox::Ok) {
        return;
    } else { // Download
        QDesktopServices::openUrl(getUri);
    }
    mbox.close();
}

void MainWindow::on_actionCheckForUpdates_triggered()
{
    // If we haven't checked the version already, force the check
    if (latest_version.isEmpty() && downloadProgress == nullptr) {
        const unsigned progress_max_value = 100;

        downloadProgress = new QProgressDialog("Checking for latest version...", "Abort", 0, progress_max_value, this);

        // ensure that this is called when download is complete
        connect(this, &MainWindow::version_download_completed_sig,
                this, &MainWindow::checkForUpdatesDialog,
                Qt::QueuedConnection);

        downloadProgress->setValue(25);
        checkLatestVersion();
        downloadProgress->show();
        downloadProgress->raise();
        downloadProgress->grabMouse();
        downloadProgress->grabKeyboard();
        return;
    }


    checkForUpdatesDialog();
}

void MainWindow::on_actionLicense_triggered()
{
    QString txt = QLatin1String("<h2>") + QLatin1String(PRODUCT_NAME_LONG) + QLatin1String("</h2>");

    txt += tr("<br><br>Based on");
    txt += tr("<br>- <a href=\"https://www.infradead.org/openconnect\">OpenConnect</a> ") + QLatin1String(openconnect_get_version());
    txt += tr("<br>- <a href=\"https://www.gnutls.org\">GnuTLS</a> v") + QLatin1String(gnutls_check_version(nullptr));
    txt += tr("<br>- <a href=\"https://github.com/gabime/spdlog\">spdlog</a> v%1.%2.%3").arg(QString::number(SPDLOG_VER_MAJOR)).arg(QString::number(SPDLOG_VER_MINOR)).arg(QString::number(SPDLOG_VER_PATCH));
    txt += tr("<br>- <a href=\"https://www.qt.io\">Qt</a> v%1").arg(QT_VERSION_STR);
    txt += tr("<br>- <a href=\"https://github.com/XTLS/Xray-core\">Xray-core</a> (bundled)");

    txt += tr("<br><br>%1<br>").arg(PRODUCT_NAME_COPYRIGHT_FULL);
    txt += tr("<br><i>%1</i> comes with ABSOLUTELY NO WARRANTY. This is free software, "
              "and you are welcome to redistribute it under the conditions "
              "of the GNU General Public License version 2.<br>")
               .arg(APP_NAME);

    QMessageBox::information(this, QLatin1String("License"), txt);
}

void MainWindow::on_actionWebSite_triggered()
{
    QDesktopServices::openUrl(QUrl(CMAKE_PROJECT_HOMEPAGE_URL));
}

void MainWindow::on_actionReport_an_issue_triggered()
{
    QDesktopServices::openUrl(QUrl(APP_ISSUES_URL));
}

int MainWindow::get_log_level()
{
    int ret = app_loglevel_tab(PRG_INFO);
    QAction* checked = ui->LogLevelGroup->checkedAction();

    if (checked != nullptr)
        ret = ui->LogLevelGroup->actions().indexOf(checked);

    return app_loglevel_rtab[ret];
}

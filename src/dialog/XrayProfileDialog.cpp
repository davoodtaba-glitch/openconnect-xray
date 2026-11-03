#include "XrayProfileDialog.h"
#include "ui_XrayProfileDialog.h"

#include "OcSettings.h"
#include "server_storage.h"
#include "xray/XrayConfig.h"

#include <QCheckBox>
#include <QClipboard>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontDatabase>
#include <QGuiApplication>
#include <QHostAddress>
#include <QInputDialog>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMessageBox>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPushButton>
#include <QRegularExpression>
#include <QSpinBox>
#include <QUrl>

namespace {

bool normalizeDnsList(const QString& text, QString& result, QString& error)
{
    static const QRegularExpression separator(QStringLiteral("[\\s,;]+"));
    const QStringList parts = text.split(separator, Qt::SkipEmptyParts);
    QStringList servers;
    for (const QString& part : parts) {
        const QHostAddress address(part);
        if (address.protocol() != QAbstractSocket::IPv4Protocol) {
            error = QStringLiteral("'%1' is not a valid IPv4 DNS server address").arg(part);
            return false;
        }
        servers.append(part);
    }
    result = servers.join(QStringLiteral(", "));
    return true;
}

bool validRange(const QString& value)
{
    static const QRegularExpression pattern(QStringLiteral("^(\\d+)(?:-(\\d+))?$"));
    const QRegularExpressionMatch match = pattern.match(value);
    if (match.hasMatch() == false) {
        return false;
    }
    const qlonglong from = match.captured(1).toLongLong();
    const qlonglong to = match.captured(2).isEmpty() ? from : match.captured(2).toLongLong();
    return from > 0 && to >= from;
}

} // namespace

XrayProfileDialog::XrayProfileDialog(QWidget* parent)
    : QDialog(parent)
    , ui(new Ui::XrayProfileDialog)
    , m_network(new QNetworkAccessManager(this))
{
    ui->setupUi(this);
    ui->configEdit->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    ui->buttonBox->button(QDialogButtonBox::Save)->setText(tr("Save"));
    ui->modeCombo->setItemData(0, tr("Route all IPv4 traffic through Xray (requires administrator rights)"), Qt::ToolTipRole);
    ui->modeCombo->setItemData(1, tr("Point the Windows system proxy at Xray; other applications keep direct connections"), Qt::ToolTipRole);

    connect(ui->pasteButton, &QPushButton::clicked, this, &XrayProfileDialog::pasteClicked);
    connect(ui->loadButton, &QPushButton::clicked, this, &XrayProfileDialog::loadClicked);
    connect(ui->fetchButton, &QPushButton::clicked, this, &XrayProfileDialog::fetchClicked);
    connect(ui->fragmentCheck, &QCheckBox::toggled, this, [this](bool checked) {
        ui->fragmentTypeCombo->setEnabled(checked);
        ui->fragmentLengthEdit->setEnabled(checked);
        ui->fragmentDelayEdit->setEnabled(checked);
    });
    ui->fragmentTypeCombo->setEnabled(false);
    ui->fragmentLengthEdit->setEnabled(false);
    ui->fragmentDelayEdit->setEnabled(false);

    setStatus(tr("Paste a config or link to continue."));
}

XrayProfileDialog::XrayProfileDialog(const QString& profileName, QWidget* parent)
    : XrayProfileDialog(parent)
{
    m_editing = true;
    setWindowTitle(tr("Edit Xray / V2Ray Profile"));
    loadExisting(profileName);
}

XrayProfileDialog::~XrayProfileDialog()
{
    delete ui;
}

QStringList XrayProfileDialog::getSavedProfiles() const
{
    return m_savedNames;
}

QString XrayProfileDialog::getOriginalName() const
{
    return m_originalName;
}

void XrayProfileDialog::changeEvent(QEvent* e)
{
    QDialog::changeEvent(e);
    if (e->type() == QEvent::LanguageChange) {
        ui->retranslateUi(this);
    }
}

void XrayProfileDialog::loadExisting(const QString& profileName)
{
    StoredServer server;
    QString name = profileName;
    if (server.load(name) < 0 || server.is_xray() == false) {
        QMessageBox::warning(this, tr("Edit profile"), tr("The selected Xray profile could not be loaded."));
        return;
    }

    m_originalName = profileName;
    ui->nameEdit->setText(profileName);
    ui->modeCombo->setCurrentIndex(server.get_xray_mode().compare(QStringLiteral("proxy"), Qt::CaseInsensitive) == 0 ? 1 : 0);
    ui->configEdit->setPlainText(XrayConfig::prettyJson(server.get_xray_config()));

    const QJsonObject config = QJsonDocument::fromJson(server.get_xray_config().toUtf8()).object();
    QString host;
    int port = 0;
    XrayConfig::primaryEndpoint(config, host, port);
    if (server.get_xray_server_override().isEmpty() == false) {
        host = server.get_xray_server_override();
    }
    if (server.get_xray_port_override() > 0) {
        port = server.get_xray_port_override();
    }
    ui->serverEdit->setText(host);
    ui->portSpin->setValue(port);
    m_initialServer = host;
    m_initialPort = port;
    m_hadExplicitOverride = server.get_xray_server_override().isEmpty() == false
        || server.get_xray_port_override() > 0;
    ui->dnsEdit->setText(server.get_xray_dns());
    ui->sniffingCheck->setChecked(server.get_xray_sniffing());
    ui->blockQuicCheck->setChecked(server.get_xray_block_quic());
    ui->fragmentCheck->setChecked(server.get_xray_fragment());
    ui->fragmentTypeCombo->setCurrentText(server.get_xray_fragment_packets().isEmpty()
            ? QStringLiteral("all packets")
            : server.get_xray_fragment_packets());
    ui->fragmentLengthEdit->setText(server.get_xray_fragment_length());
    ui->fragmentDelayEdit->setText(server.get_xray_fragment_interval());
    ui->subscriptionEdit->setText(server.get_xray_subscription_url());

    if (server.get_xray_subscription_url().isEmpty()) {
        setStatus(tr("Editing '%1'. You can paste a replacement config or link.").arg(profileName));
    } else {
        setStatus(tr("Editing '%1'. Press Fetch / Update to refresh from its subscription.").arg(profileName));
    }
}

void XrayProfileDialog::pasteClicked()
{
    const QString text = QGuiApplication::clipboard()->text();
    if (text.trimmed().isEmpty()) {
        setStatus(tr("The clipboard is empty."), true);
        return;
    }
    ui->configEdit->setPlainText(text);
    ui->subscriptionEdit->clear();
    setStatus(tr("Loaded from the clipboard."));
}

void XrayProfileDialog::loadClicked()
{
    const QString file = QFileDialog::getOpenFileName(this, tr("Load Xray config"),
        QString(), tr("Config files (*.json *.txt *.conf);;All files (*)"));
    if (file.isEmpty()) {
        return;
    }

    QFile input(file);
    if (input.open(QIODevice::ReadOnly) == false) {
        setStatus(tr("Could not read %1").arg(file), true);
        return;
    }

    ui->configEdit->setPlainText(QString::fromUtf8(input.readAll()));
    ui->subscriptionEdit->clear();
    if (m_editing == false && ui->nameEdit->text().trimmed().isEmpty()) {
        ui->nameEdit->setText(QFileInfo(file).completeBaseName());
    }
    setStatus(tr("Loaded %1").arg(QFileInfo(file).fileName()));
}

bool XrayProfileDialog::isSubscriptionUrl(const QString& text)
{
    const QString value = text.trimmed();
    if (value.contains(QLatin1Char('\n')) || value.contains(QLatin1Char('\r'))) {
        return false;
    }
    const QUrl url(value, QUrl::TolerantMode);
    if (url.isValid() == false) {
        return false;
    }
    const QString scheme = url.scheme().toLower();
    if (scheme != QLatin1String("http") && scheme != QLatin1String("https")) {
        return false;
    }
    return url.path().size() > 1 || url.hasQuery();
}

void XrayProfileDialog::fetchClicked()
{
    QString urlText = ui->subscriptionEdit->text().trimmed();
    if (urlText.isEmpty()) {
        const QString pasted = ui->configEdit->toPlainText().trimmed();
        if (isSubscriptionUrl(pasted)) {
            urlText = pasted;
        }
    }

    const QUrl url(urlText, QUrl::TolerantMode);
    if (urlText.isEmpty() || url.isValid() == false
        || (url.scheme().compare(QStringLiteral("http"), Qt::CaseInsensitive) != 0
            && url.scheme().compare(QStringLiteral("https"), Qt::CaseInsensitive) != 0)) {
        setStatus(tr("Enter a valid http:// or https:// subscription URL."), true);
        return;
    }

    if (m_reply != nullptr) {
        m_reply->abort();
    }

    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("OpenConnect-GUI-Xray/1.0"));
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    request.setTransferTimeout(20000);

    ui->fetchButton->setEnabled(false);
    ui->subscriptionEdit->setText(urlText);
    setStatus(tr("Fetching %1 ...").arg(url.host()));

    m_reply = m_network->get(request);
    connect(m_reply, &QNetworkReply::finished, this, &XrayProfileDialog::fetchFinished);
}

void XrayProfileDialog::fetchFinished()
{
    QNetworkReply* reply = qobject_cast<QNetworkReply*>(sender());
    if (reply == nullptr) {
        return;
    }
    if (reply != m_reply) {
        reply->deleteLater();
        return;
    }

    ui->fetchButton->setEnabled(true);
    m_reply = nullptr;
    reply->deleteLater();

    if (reply->error() != QNetworkReply::NoError) {
        setStatus(tr("Could not fetch the subscription: %1").arg(reply->errorString()), true);
        return;
    }

    QString text;
    QString error;
    if (XrayConfig::decodeSubscription(reply->readAll(), text, error) == false) {
        setStatus(error, true);
        return;
    }

    QVector<XrayImportedProfile> profiles;
    const bool parsed = XrayConfig::importText(text, profiles, error);
    if (parsed && profiles.size() == 1 && ui->nameEdit->text().trimmed().isEmpty()) {
        ui->nameEdit->setText(profiles.at(0).name);
    }

    if (m_editing && m_hadExplicitOverride == false && parsed) {
        QString host;
        int port = 0;
        bool found = false;
        if (profiles.size() == 1) {
            const QJsonObject config = QJsonDocument::fromJson(profiles.at(0).configJson.toUtf8()).object();
            found = XrayConfig::primaryEndpoint(config, host, port);
        }
        ui->serverEdit->setText(found ? host : QString());
        ui->portSpin->setValue(found ? port : 0);
        m_initialServer = found ? host : QString();
        m_initialPort = found ? port : 0;
    }

    ui->configEdit->setPlainText(text);
    if (parsed) {
        if (m_editing && profiles.size() > 1) {
            setStatus(tr("Fetched %1 profiles. Press Save and choose which node to update.").arg(profiles.size()));
        } else if (m_editing) {
            setStatus(tr("Fetched 1 profile. Press Save to update this profile."));
        } else {
            setStatus(tr("Fetched %1 profile(s). Review the content and press Save.").arg(profiles.size()));
        }
    } else {
        setStatus(tr("Fetched the subscription, but it could not be parsed: %1").arg(error), true);
    }
}

QString XrayProfileDialog::uniqueName(const QString& desired, const QString& exclude) const
{
    QString base = desired.trimmed();
    if (base.isEmpty()) {
        base = QStringLiteral("Xray profile");
    }
    base.replace(QLatin1Char('/'), QLatin1Char('-'));
    base.replace(QLatin1Char('\\'), QLatin1Char('-'));

    QStringList existing;
    OcSettings settings;
    static const QString prefix = QStringLiteral("server:");
    for (const QString& key : settings.allKeys()) {
        if (key.startsWith(prefix) && key.endsWith(QStringLiteral("/server"))) {
            QString name = key.mid(prefix.size());
            name.chop(QStringLiteral("/server").size());
            if (name.compare(exclude, Qt::CaseInsensitive) != 0) {
                existing.append(name);
            }
        }
    }

    QString candidate = base;
    int suffix = 2;
    bool duplicate = true;
    while (duplicate) {
        duplicate = false;
        for (const QString& name : existing) {
            if (name.compare(candidate, Qt::CaseInsensitive) == 0) {
                candidate = QStringLiteral("%1 (%2)").arg(base).arg(suffix++);
                duplicate = true;
                break;
            }
        }
    }
    return candidate;
}

void XrayProfileDialog::setStatus(const QString& text, bool error)
{
    ui->statusLabel->setText(text);
    if (error) {
        ui->statusLabel->setStyleSheet(QStringLiteral("color: #c62828;"));
    } else {
        ui->statusLabel->setStyleSheet(QString());
    }
}

void XrayProfileDialog::on_buttonBox_accepted()
{
    const QString text = ui->configEdit->toPlainText().trimmed();
    if (text.isEmpty()) {
        if (ui->subscriptionEdit->text().trimmed().isEmpty() == false) {
            fetchClicked();
            setStatus(tr("Fetching the subscription; press Save again when it has loaded."));
            return;
        }
        setStatus(tr("Paste a config or link first."), true);
        return;
    }

    if (isSubscriptionUrl(text)) {
        ui->subscriptionEdit->setText(text);
        fetchClicked();
        setStatus(tr("Fetching the subscription; press Save again when it has loaded."));
        return;
    }

    QVector<XrayImportedProfile> profiles;
    QString error;
    if (XrayConfig::importText(text, profiles, error) == false) {
        setStatus(error, true);
        QMessageBox::warning(this, tr("Xray profile"), error);
        return;
    }

    if (profiles.isEmpty()) {
        setStatus(tr("No profiles were found in the input."), true);
        return;
    }

    const QString subscriptionUrl = ui->subscriptionEdit->text().trimmed();
    if (subscriptionUrl.isEmpty() == false) {
        const QUrl url(subscriptionUrl, QUrl::TolerantMode);
        if (url.isValid() == false
            || (url.scheme().compare(QStringLiteral("http"), Qt::CaseInsensitive) != 0
                && url.scheme().compare(QStringLiteral("https"), Qt::CaseInsensitive) != 0)) {
            const QString message = tr("The saved subscription URL must be a valid http:// or https:// address.");
            setStatus(message, true);
            QMessageBox::warning(this, tr("Xray profile"), message);
            return;
        }
    }

    if (m_editing && profiles.size() > 1) {
        QStringList labels;
        for (const XrayImportedProfile& profile : profiles) {
            labels.append(profile.name);
        }

        int defaultIndex = labels.indexOf(ui->nameEdit->text().trimmed(), 0, Qt::CaseInsensitive);
        if (defaultIndex < 0) {
            defaultIndex = labels.indexOf(m_originalName, 0, Qt::CaseInsensitive);
        }
        if (defaultIndex < 0) {
            defaultIndex = 0;
        }

        bool ok = false;
        const QString chosen = QInputDialog::getItem(this,
            tr("Update from subscription"),
            tr("Choose the node to use for this profile:"),
            labels,
            defaultIndex,
            false,
            &ok);
        if (ok == false) {
            return;
        }

        const int chosenIndex = labels.indexOf(chosen);
        if (chosenIndex < 0) {
            return;
        }
        const XrayImportedProfile selected = profiles.at(chosenIndex);
        profiles.clear();
        profiles.append(selected);
    }

    QString advancedError;
    QString dnsText;
    if (normalizeDnsList(ui->dnsEdit->text(), dnsText, advancedError) == false) {
        setStatus(advancedError, true);
        QMessageBox::warning(this, tr("Xray profile"), advancedError);
        return;
    }

    QString fragmentPackets = ui->fragmentTypeCombo->currentText().trimmed();
    if (fragmentPackets.compare(QStringLiteral("all packets"), Qt::CaseInsensitive) == 0) {
        fragmentPackets.clear();
    } else if (fragmentPackets.compare(QStringLiteral("tlshello"), Qt::CaseInsensitive) == 0) {
        fragmentPackets = QStringLiteral("tlshello");
    }

    QString fragmentLength = ui->fragmentLengthEdit->text().trimmed();
    QString fragmentDelay = ui->fragmentDelayEdit->text().trimmed();
    if (fragmentLength.isEmpty()) {
        fragmentLength = QStringLiteral("100-200");
    }
    if (fragmentDelay.isEmpty()) {
        fragmentDelay = QStringLiteral("10-20");
    }
    if (ui->fragmentCheck->isChecked()) {
        if (fragmentPackets.isEmpty() == false && fragmentPackets != QStringLiteral("tlshello")
            && validRange(fragmentPackets) == false) {
            const QString message = tr("Fragment type must be tlshello, all packets, or a range such as 1-3.");
            setStatus(message, true);
            QMessageBox::warning(this, tr("Xray profile"), message);
            return;
        }
        if (validRange(fragmentLength) == false) {
            const QString message = tr("Fragment length must be a number or range such as 100-200.");
            setStatus(message, true);
            QMessageBox::warning(this, tr("Xray profile"), message);
            return;
        }
        if (validRange(fragmentDelay) == false) {
            const QString message = tr("Fragment delay must be a number or range such as 10-20.");
            setStatus(message, true);
            QMessageBox::warning(this, tr("Xray profile"), message);
            return;
        }
    }

    const QString serverOverride = ui->serverEdit->text().trimmed();
    if (serverOverride.contains(QRegularExpression(QStringLiteral("\\s")))) {
        const QString message = tr("The server override must not contain whitespace.");
        setStatus(message, true);
        QMessageBox::warning(this, tr("Xray profile"), message);
        return;
    }
    const int portOverride = ui->portSpin->value();

    const QString mode = ui->modeCombo->currentIndex() == 1 ? QStringLiteral("proxy") : QStringLiteral("tun");
    const QString baseName = ui->nameEdit->text().trimmed();

    m_savedNames.clear();
    for (const XrayImportedProfile& profile : profiles) {
        QString desired;
        if (baseName.isEmpty()) {
            desired = profile.name;
        } else if (profiles.size() > 1) {
            desired = QStringLiteral("%1 - %2").arg(baseName, profile.name);
        } else {
            desired = baseName;
        }

        const QString name = uniqueName(desired,
            m_editing && m_savedNames.isEmpty() ? m_originalName : QString());

        const bool storeOverride = m_hadExplicitOverride
            || serverOverride != m_initialServer
            || portOverride != m_initialPort;
        const QString storedServer = storeOverride ? serverOverride : QString();
        const int storedPort = storeOverride ? portOverride : 0;

        QString displayServer = profile.server;
        if (storedServer.isEmpty() == false) {
            displayServer = storedPort > 0
                ? QStringLiteral("%1:%2").arg(storedServer).arg(storedPort)
                : storedServer;
        }

        StoredServer server;
        server.set_label(name);
        server.set_server_gateway(displayServer);
        server.set_xray_config(profile.configJson);
        server.set_xray_mode(mode);
        server.set_xray_subscription_url(subscriptionUrl);
        server.set_xray_dns(dnsText);
        server.set_xray_sniffing(ui->sniffingCheck->isChecked());
        server.set_xray_block_quic(ui->blockQuicCheck->isChecked());
        server.set_xray_fragment(ui->fragmentCheck->isChecked());
        server.set_xray_fragment_packets(fragmentPackets);
        server.set_xray_fragment_length(fragmentLength);
        server.set_xray_fragment_interval(fragmentDelay);
        server.set_xray_server_override(storedServer);
        server.set_xray_port_override(storedPort);
        server.save();

        m_savedNames.append(name);
    }

    if (m_editing && m_originalName.isEmpty() == false && m_savedNames.isEmpty() == false
        && m_savedNames.first().compare(m_originalName, Qt::CaseSensitive) != 0) {
        OcSettings settings;
        const QString prefix = QStringLiteral("server:%1/").arg(m_originalName);
        for (const QString& key : settings.allKeys()) {
            if (key.startsWith(prefix)) {
                settings.remove(key);
            }
        }
    }

    accept();
}

void XrayProfileDialog::on_buttonBox_rejected()
{
    reject();
}

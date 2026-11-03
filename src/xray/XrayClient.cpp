#include "XrayClient.h"

#include "XrayConfig.h"
#include "logger.h"
#include "OcSettings.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QFile>
#include <QHostAddress>
#include <QProcess>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QSettings>
#include <QStandardPaths>
#include <QStringList>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>

#ifdef Q_OS_WIN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <windows.h>
#include <wininet.h>
#endif

namespace {

constexpr int readyPollIntervalMs = 500;
constexpr int readyTimeoutMs = 20000;
constexpr int statsIntervalMs = 2000;

QString xrayExecutable()
{
#ifdef Q_OS_WIN
    const QString bundled = QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("xray.exe"));
    if (QFileInfo::exists(bundled)) {
        return bundled;
    }
#endif
    return QStringLiteral("xray");
}

int findFreePort()
{
    QTcpServer server;
    if (server.listen(QHostAddress::LocalHost, 0) == false) {
        return 0;
    }
    const int port = server.serverPort();
    server.close();
    return port;
}

bool runCommand(const QString& program, const QStringList& arguments, QString& output)
{
    QProcess process;
    process.setProgram(program);
    process.setArguments(arguments);
    process.start();
    if (process.waitForStarted(3000) == false) {
        output = process.errorString();
        return false;
    }
    if (process.waitForFinished(5000) == false) {
        process.kill();
        process.waitForFinished(1000);
        output = QStringLiteral("timed out");
        return false;
    }

    output = QString::fromLocal8Bit(process.readAllStandardOutput()
        + process.readAllStandardError()).trimmed();
    return process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0;
}

bool findTunAdapter(QString& name, QString& output)
{
#ifdef Q_OS_WIN
    if (runCommand(QStringLiteral("netsh.exe"),
            { QStringLiteral("interface"), QStringLiteral("ipv4"), QStringLiteral("show"),
                QStringLiteral("addresses") },
            output)
        == false) {
        return false;
    }

    static const QRegularExpression separator(QStringLiteral("\\r?\\n\\s*\\r?\\n"));
    static const QRegularExpression quotedName(QStringLiteral("\"([^\"]+)\""));
    const QStringList blocks = output.split(separator, Qt::SkipEmptyParts);
    for (const QString& block : blocks) {
        if (block.contains(QStringLiteral("198.18.0.1")) == false) {
            continue;
        }
        const QRegularExpressionMatch match = quotedName.match(block);
        if (match.hasMatch()) {
            name = match.captured(1);
            return true;
        }
    }

    const int marker = output.indexOf(QStringLiteral("198.18.0.1"));
    if (marker > 0) {
        const QString before = output.left(marker);
        const int lastQuote = before.lastIndexOf(QLatin1Char('"'));
        const int firstQuote = lastQuote > 0 ? before.lastIndexOf(QLatin1Char('"'), lastQuote - 1) : -1;
        if (firstQuote >= 0) {
            name = before.mid(firstQuote + 1, lastQuote - firstQuote - 1);
            return name.isEmpty() == false;
        }
    }
    return false;
#else
    Q_UNUSED(name);
    Q_UNUSED(output);
    return false;
#endif
}

QStringList ipv4DnsServers(const QStringList& servers)
{
    QStringList result;
    for (const QString& value : servers) {
        const QString candidate = value.trimmed();
        QHostAddress address(candidate);
        if (address.isNull() == false && address.protocol() == QAbstractSocket::IPv4Protocol) {
            result.append(candidate);
        }
    }
    return result;
}

bool setAdapterDns(const QString& adapter, bool useStatic, const QStringList& servers, QString& error)
{
    const QString nameArg = QStringLiteral("name=%1").arg(adapter);
    QString output;

    if (useStatic == false || servers.isEmpty()) {
        if (runCommand(QStringLiteral("netsh.exe"),
                { QStringLiteral("interface"), QStringLiteral("ipv4"), QStringLiteral("set"),
                    QStringLiteral("dnsservers"), nameArg, QStringLiteral("source=dhcp") },
                output)
            == false) {
            error = output;
            return false;
        }
        runCommand(QStringLiteral("ipconfig.exe"), { QStringLiteral("/flushdns") }, output);
        return true;
    }

    if (runCommand(QStringLiteral("netsh.exe"),
            { QStringLiteral("interface"), QStringLiteral("ipv4"), QStringLiteral("set"),
                QStringLiteral("dnsservers"), nameArg, QStringLiteral("source=static"),
                QStringLiteral("address=%1").arg(servers.first()), QStringLiteral("register=none"),
                QStringLiteral("validate=no") },
            output)
        == false) {
        error = output;
        return false;
    }

    for (int i = 1; i < servers.size(); ++i) {
        if (runCommand(QStringLiteral("netsh.exe"),
                { QStringLiteral("interface"), QStringLiteral("ipv4"), QStringLiteral("add"),
                    QStringLiteral("dnsservers"), nameArg,
                    QStringLiteral("address=%1").arg(servers.at(i)),
                    QStringLiteral("index=%1").arg(i + 1), QStringLiteral("validate=no") },
                output)
            == false) {
            error = output;
            return false;
        }
    }

    runCommand(QStringLiteral("ipconfig.exe"), { QStringLiteral("/flushdns") }, output);
    return true;
}

void clearPhysicalDnsBackup()
{
    OcSettings app;
    app.remove(QStringLiteral("Xray/physical-dns-valid"));
    app.remove(QStringLiteral("Xray/physical-dns-adapter"));
    app.remove(QStringLiteral("Xray/physical-dns-was-static"));
    app.remove(QStringLiteral("Xray/physical-dns-servers"));
    app.sync();
}

#ifdef Q_OS_WIN
QString internetSettingsPath()
{
    return QStringLiteral("HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Internet Settings");
}

void notifyProxyChanged()
{
    InternetSetOptionW(nullptr, INTERNET_OPTION_SETTINGS_CHANGED, nullptr, 0);
    InternetSetOptionW(nullptr, INTERNET_OPTION_REFRESH, nullptr, 0);
}

bool restoreProxyFromBackup()
{
    OcSettings app;
    if (app.value(QStringLiteral("Xray/proxy-backup-valid"), false).toBool() == false) {
        return false;
    }

    const bool enabled = app.value(QStringLiteral("Xray/proxy-backup-enable"), 0).toInt() != 0;
    const QString server = app.value(QStringLiteral("Xray/proxy-backup-server")).toString();

    QSettings reg(internetSettingsPath(), QSettings::NativeFormat);
    reg.setValue(QStringLiteral("ProxyEnable"), enabled ? 1 : 0);
    if (server.isEmpty()) {
        reg.remove(QStringLiteral("ProxyServer"));
    } else {
        reg.setValue(QStringLiteral("ProxyServer"), server);
    }
    reg.sync();
    notifyProxyChanged();

    app.remove(QStringLiteral("Xray/proxy-backup-valid"));
    app.remove(QStringLiteral("Xray/proxy-backup-enable"));
    app.remove(QStringLiteral("Xray/proxy-backup-server"));
    app.sync();
    return true;
}

bool applyProxy(int port, QString& error)
{
    QSettings reg(internetSettingsPath(), QSettings::NativeFormat);
    OcSettings app;

    if (app.value(QStringLiteral("Xray/proxy-backup-valid"), false).toBool() == false) {
        app.setValue(QStringLiteral("Xray/proxy-backup-enable"), reg.value(QStringLiteral("ProxyEnable"), 0).toInt());
        app.setValue(QStringLiteral("Xray/proxy-backup-server"), reg.value(QStringLiteral("ProxyServer")).toString());
        app.setValue(QStringLiteral("Xray/proxy-backup-valid"), true);
        app.sync();
    }

    reg.setValue(QStringLiteral("ProxyEnable"), 1);
    reg.setValue(QStringLiteral("ProxyServer"), QStringLiteral("127.0.0.1:%1").arg(port));
    reg.sync();

    if (reg.status() != QSettings::NoError) {
        restoreProxyFromBackup();
        error = QStringLiteral("Could not update the Windows proxy settings");
        return false;
    }

    notifyProxyChanged();
    return true;
}
#endif

} // namespace

XrayClient::XrayClient(QObject* parent)
    : QObject(parent)
{
    m_process = nullptr;
    m_statsProcess = new QProcess(this);
    m_statsProcess->setProcessChannelMode(QProcess::MergedChannels);

    m_readyTimer = new QTimer(this);
    m_readyTimer->setInterval(readyPollIntervalMs);
    connect(m_readyTimer, &QTimer::timeout, this, &XrayClient::checkReady);

    m_statsTimer = new QTimer(this);
    m_statsTimer->setInterval(statsIntervalMs);
    connect(m_statsTimer, &QTimer::timeout, this, &XrayClient::requestStats);

    connect(m_statsProcess, &QProcess::finished,
        this, &XrayClient::statsFinished);
}

XrayClient::~XrayClient()
{
    if (m_process != nullptr || m_ready) {
        m_stopping = true;
        cleanup(true);
    }
}

QString XrayClient::coreVersion()
{
    static QString version;
    if (version.isEmpty() == false) {
        return version;
    }

    QProcess process;
    process.start(xrayExecutable(), { QStringLiteral("version") });
    if (process.waitForFinished(3000)) {
        const QStringList lines = QString::fromUtf8(process.readAllStandardOutput()).split(QRegularExpression(QStringLiteral("[\\r\\n]+")), Qt::SkipEmptyParts);
        for (const QString& line : lines) {
            if (line.startsWith(QStringLiteral("Xray "), Qt::CaseInsensitive)) {
                const QStringList parts = line.split(QLatin1Char(' '), Qt::SkipEmptyParts);
                if (parts.size() >= 2) {
                    version = parts.at(1);
                    break;
                }
            }
        }
    }
    if (version.isEmpty()) {
        version = QStringLiteral("unknown");
    }
    return version;
}

void XrayClient::restoreStaleSystemProxy()
{
#ifdef Q_OS_WIN
    restoreProxyFromBackup();
#endif
}

bool XrayClient::isRunning() const
{
    return m_process != nullptr && m_process->state() != QProcess::NotRunning;
}

QString XrayClient::modeName() const
{
    return m_tunMode ? QStringLiteral("TUN") : QStringLiteral("Proxy");
}

QString XrayClient::endpoint() const
{
    if (m_tunMode) {
        return QStringLiteral("198.18.0.1/16 (TUN)");
    }
    return QStringLiteral("127.0.0.1:%1").arg(m_proxyPort);
}

bool XrayClient::writeRuntimeConfig(const QString& configJson, const XrayRuntimeOptions& options, QString& error)
{
    QJsonParseError parseError;
    const QJsonDocument stored = QJsonDocument::fromJson(configJson.toUtf8(), &parseError);
    if (stored.isObject() == false) {
        error = QStringLiteral("Invalid profile JSON: %1").arg(parseError.errorString());
        return false;
    }

    m_proxyPort = options.tunMode ? 0 : findFreePort();
    m_apiPort = findFreePort();
    if (m_apiPort == 0 || (options.tunMode == false && m_proxyPort == 0)) {
        error = QStringLiteral("Could not reserve local ports for Xray");
        return false;
    }

    const QJsonObject runtime = XrayConfig::buildRuntimeConfig(stored.object(), options, m_proxyPort, m_apiPort, error);
    if (runtime.isEmpty()) {
        return false;
    }

    const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    if (QDir().mkpath(dir) == false) {
        error = QStringLiteral("Could not create the Xray configuration directory");
        return false;
    }

    m_configPath = QDir(dir).filePath(QStringLiteral("xray-runtime.json"));
    QFile file(m_configPath);
    if (file.open(QIODevice::WriteOnly | QIODevice::Truncate) == false) {
        error = QStringLiteral("Could not write %1").arg(m_configPath);
        return false;
    }
    file.write(QJsonDocument(runtime).toJson(QJsonDocument::Indented));
    file.close();
    return true;
}

bool XrayClient::start(const QString& label, const QString& configJson, const XrayRuntimeOptions& options)
{
#ifndef Q_OS_WIN
    emit errorOccurred(tr("Xray profiles are currently supported on Windows only"));
    emit statusChanged(Disconnected);
    return false;
#endif

    if (m_process != nullptr) {
        emit errorOccurred(tr("An Xray connection is already running"));
        return false;
    }

    m_label = label;
    m_tunMode = options.tunMode;
    m_dnsServers = options.dnsServers;
    m_dnsServers.removeAll(QString());
    if (m_dnsServers.isEmpty()) {
        m_dnsServers = XrayConfig::defaultDnsServers();
    }
    m_stopping = false;
    m_ready = false;
    m_readyChecks = 0;
    m_output.clear();
    m_tunAdapterName.clear();
    m_physicalDns = {};
    m_physicalDnsApplied = false;

    if (m_tunMode) {
        QString dnsWarning;
        if (capturePhysicalDns(dnsWarning) == false) {
            Logger::instance().addMessage(tr("DNS leak protection: %1").arg(dnsWarning),
                Logger::MessageType::WARNING);
        }
    }

    QString error;
    if (writeRuntimeConfig(configJson, options, error) == false) {
        emit errorOccurred(error);
        emit statusChanged(Disconnected);
        return false;
    }

    emit statusChanged(Connecting);
    Logger::instance().addMessage(tr("Starting Xray core for '%1' (%2 mode)").arg(label, modeName()));

    m_process = new QProcess(this);
    m_process->setProcessChannelMode(QProcess::MergedChannels);
    m_process->setWorkingDirectory(QCoreApplication::applicationDirPath());
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(QStringLiteral("XRAY_LOCATION_ASSET"), QCoreApplication::applicationDirPath());
    m_process->setProcessEnvironment(env);
    m_process->setProgram(xrayExecutable());
    m_process->setArguments({ QStringLiteral("run"), QStringLiteral("-c"), m_configPath });

    connect(m_process, &QProcess::readyReadStandardOutput,
        this, &XrayClient::readCoreOutput);
    connect(m_process, &QProcess::finished,
        this, &XrayClient::coreFinished);
    connect(m_process, &QProcess::errorOccurred,
        this, &XrayClient::coreError);

    m_process->start();
    if (m_process->waitForStarted(5000) == false) {
        error = tr("Could not start xray.exe: %1").arg(m_process->errorString());
        Logger::instance().addMessage(error, Logger::MessageType::CRITICAL);
        cleanup(true);
        emit errorOccurred(error);
        emit statusChanged(Disconnected);
        return false;
    }

    attachToJobObject();
    m_readyTimer->start();
    return true;
}

void XrayClient::stop()
{
    if (m_process == nullptr && m_ready == false) {
        return;
    }

    emit statusChanged(Disconnecting);
    m_stopping = true;
    cleanup(true);
    m_stopping = false;
    emit statusChanged(Disconnected);
}

void XrayClient::cleanup(bool restoreProxy)
{
    m_readyTimer->stop();
    m_statsTimer->stop();

    if (m_tunMode && m_ready) {
        clearTunAdapterDns();
    }

    if (restoreProxy) {
#ifdef Q_OS_WIN
        if (m_proxyApplied) {
            restoreProxyFromBackup();
        }
        m_proxyApplied = false;
#endif
    }

    if (m_statsProcess != nullptr && m_statsProcess->state() != QProcess::NotRunning) {
        m_statsProcess->kill();
        m_statsProcess->waitForFinished(1000);
    }

    if (m_process != nullptr) {
        m_process->blockSignals(true);
        if (m_process->state() != QProcess::NotRunning) {
            m_process->terminate();
            if (m_process->waitForFinished(2000) == false) {
                m_process->kill();
                m_process->waitForFinished(1000);
            }
        }
        m_process->deleteLater();
        m_process = nullptr;
    }

#ifdef Q_OS_WIN
    if (m_job != nullptr) {
        CloseHandle(static_cast<HANDLE>(m_job));
        m_job = nullptr;
    }
#endif

    if (m_tunMode && m_physicalDnsApplied) {
        restorePhysicalDns();
    }

    if (m_configPath.isEmpty() == false) {
        QFile::remove(m_configPath);
    }

    m_ready = false;
    m_tunAdapterName.clear();
    m_dnsServers.clear();
    m_physicalDns = {};
    m_physicalDnsApplied = false;
    m_proxyPort = 0;
    m_apiPort = 0;
}

void XrayClient::attachToJobObject()
{
#ifdef Q_OS_WIN
    if (m_process == nullptr) {
        return;
    }

    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    if (job == nullptr) {
        return;
    }

    JOBOBJECT_EXTENDED_LIMIT_INFORMATION info = {};
    info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    SetInformationJobObject(job, JobObjectExtendedLimitInformation, &info, sizeof(info));

    HANDLE process = OpenProcess(PROCESS_SET_QUOTA | PROCESS_TERMINATE, FALSE,
        static_cast<DWORD>(m_process->processId()));
    if (process == nullptr) {
        CloseHandle(job);
        return;
    }

    if (AssignProcessToJobObject(job, process) == FALSE) {
        CloseHandle(process);
        CloseHandle(job);
        return;
    }

    CloseHandle(process);
    m_job = job;
#endif
}

void XrayClient::applyTunAdapterDns()
{
#ifdef Q_OS_WIN
    const QString adapter = m_tunAdapterName.isEmpty() ? QStringLiteral("oc-xray") : m_tunAdapterName;
    const QString adapterArg = QStringLiteral("name=%1").arg(adapter);

    QStringList dns;
    for (const QString& server : m_dnsServers) {
        QHostAddress address(server);
        if (address.protocol() == QAbstractSocket::IPv4Protocol) {
            dns.append(server);
        }
    }

    if (dns.isEmpty()) {
        Logger::instance().addMessage(tr("No IPv4 DNS server was configured for the '%1' adapter").arg(adapter),
            Logger::MessageType::WARNING);
        return;
    }

    QString output;
    if (runCommand(QStringLiteral("netsh.exe"),
            { QStringLiteral("interface"), QStringLiteral("ipv4"), QStringLiteral("set"),
                QStringLiteral("dnsservers"), adapterArg, QStringLiteral("source=static"),
                QStringLiteral("address=%1").arg(dns.first()), QStringLiteral("register=none"),
                QStringLiteral("validate=no") },
            output)
        == false) {
        Logger::instance().addMessage(tr("Could not set the primary DNS on the '%1' adapter: %2").arg(adapter, output),
            Logger::MessageType::WARNING);
    }

    for (int i = 1; i < dns.size(); ++i) {
        if (runCommand(QStringLiteral("netsh.exe"),
                { QStringLiteral("interface"), QStringLiteral("ipv4"), QStringLiteral("add"),
                    QStringLiteral("dnsservers"), adapterArg,
                    QStringLiteral("address=%1").arg(dns.at(i)),
                    QStringLiteral("index=%1").arg(i + 1), QStringLiteral("validate=no") },
                output)
            == false) {
            Logger::instance().addMessage(tr("Could not add DNS '%1' on the '%2' adapter: %3")
                                              .arg(dns.at(i), adapter, output),
                Logger::MessageType::WARNING);
        }
    }

    runCommand(QStringLiteral("ipconfig.exe"), { QStringLiteral("/flushdns") }, output);
#endif
}

void XrayClient::clearTunAdapterDns()
{
#ifdef Q_OS_WIN
    const QString adapter = m_tunAdapterName.isEmpty() ? QStringLiteral("oc-xray") : m_tunAdapterName;
    QString output;
    runCommand(QStringLiteral("netsh.exe"),
        { QStringLiteral("interface"), QStringLiteral("ipv4"), QStringLiteral("set"),
            QStringLiteral("dnsservers"), QStringLiteral("name=%1").arg(adapter), QStringLiteral("source=dhcp") },
        output);
    runCommand(QStringLiteral("ipconfig.exe"), { QStringLiteral("/flushdns") }, output);
#endif
}

bool XrayClient::capturePhysicalDns(QString& error)
{
#ifdef Q_OS_WIN
    m_physicalDns = {};

    ULONG size = 0;
    const ULONG flags = GAA_FLAG_INCLUDE_GATEWAYS | GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST;
    ULONG ret = GetAdaptersAddresses(AF_INET, flags, nullptr, nullptr, &size);
    if (ret != ERROR_BUFFER_OVERFLOW) {
        error = tr("could not enumerate network adapters (error %1)").arg(ret);
        return false;
    }

    QByteArray buffer(static_cast<int>(size), 0);
    auto* addresses = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data());
    ret = GetAdaptersAddresses(AF_INET, flags, nullptr, addresses, &size);
    if (ret != NO_ERROR) {
        error = tr("could not enumerate network adapters (error %1)").arg(ret);
        return false;
    }

    static const QStringList virtualMarkers = {
        QStringLiteral("tap"),
        QStringLiteral("wintun"),
        QStringLiteral("tunnel"),
        QStringLiteral("vpn"),
        QStringLiteral("openconnect"),
        QStringLiteral("wireguard"),
        QStringLiteral("tailscale"),
        QStringLiteral("zerotier"),
        QStringLiteral("virtual")
    };

    IP_ADAPTER_ADDRESSES* chosen = nullptr;
    bool chosenHasGateway = false;
    ULONG chosenMetric = 0;

    for (IP_ADAPTER_ADDRESSES* adapter = addresses; adapter != nullptr; adapter = adapter->Next) {
        if (adapter->OperStatus != IfOperStatusUp || adapter->IfIndex == 0) {
            continue;
        }
        if (adapter->IfType == IF_TYPE_SOFTWARE_LOOPBACK || adapter->IfType == IF_TYPE_TUNNEL) {
            continue;
        }

        const QString friendly = QString::fromWCharArray(adapter->FriendlyName);
        const QString description = QString::fromWCharArray(adapter->Description);
        const QString marker = (friendly + QLatin1Char(' ') + description).toLower();
        bool virtualAdapter = false;
        for (const QString& virtualMarker : virtualMarkers) {
            if (marker.contains(virtualMarker)) {
                virtualAdapter = true;
                break;
            }
        }
        if (virtualAdapter) {
            continue;
        }

        bool hasIpv4 = false;
        for (auto* address = adapter->FirstUnicastAddress;
             address != nullptr; address = address->Next) {
            if (address->Address.lpSockaddr->sa_family == AF_INET) {
                const auto* ipv4 = reinterpret_cast<const sockaddr_in*>(address->Address.lpSockaddr);
                if (ipv4->sin_addr.s_addr != htonl(0xA9FE0000)) { // 169.254.0.0/16
                    hasIpv4 = true;
                    break;
                }
            }
        }
        if (hasIpv4 == false) {
            continue;
        }

        bool hasGateway = false;
        for (auto* gateway = adapter->FirstGatewayAddress;
             gateway != nullptr; gateway = gateway->Next) {
            if (gateway->Address.lpSockaddr->sa_family == AF_INET) {
                hasGateway = true;
                break;
            }
        }

        const ULONG metric = adapter->Ipv4Metric;
        if (chosen == nullptr
            || (hasGateway && chosenHasGateway == false)
            || (hasGateway == chosenHasGateway && metric < chosenMetric)) {
            chosen = adapter;
            chosenHasGateway = hasGateway;
            chosenMetric = metric;
        }
    }

    if (chosen == nullptr) {
        error = tr("no active physical network adapter was found");
        return false;
    }

    const QString friendly = QString::fromWCharArray(chosen->FriendlyName);
    const QString guid = QString::fromLatin1(chosen->AdapterName);
    QSettings registry(QStringLiteral("HKEY_LOCAL_MACHINE\\SYSTEM\\CurrentControlSet\\Services\\Tcpip\\Parameters\\Interfaces\\%1").arg(guid),
        QSettings::NativeFormat);
    const QString nameServer = registry.value(QStringLiteral("NameServer")).toString().trimmed();

    QStringList servers;
    for (const QString& part : nameServer.split(QRegularExpression(QStringLiteral("[\\s,;]+")), Qt::SkipEmptyParts)) {
        QHostAddress address(part);
        if (address.protocol() == QAbstractSocket::IPv4Protocol) {
            servers.append(part);
        }
    }

    m_physicalDns.adapterName = friendly.isEmpty() ? QString::number(chosen->IfIndex) : friendly;
    m_physicalDns.ifIndex = static_cast<int>(chosen->IfIndex);
    m_physicalDns.wasStatic = servers.isEmpty() == false;
    m_physicalDns.servers = servers;
    m_physicalDns.valid = true;

    Logger::instance().addMessage(tr("DNS leak protection: physical adapter is '%1' (%2 DNS)")
                                      .arg(m_physicalDns.adapterName,
                                          m_physicalDns.wasStatic ? QStringLiteral("static") : QStringLiteral("DHCP")),
        Logger::MessageType::DEBUG);
    return true;
#else
    Q_UNUSED(error);
    return false;
#endif
}

bool XrayClient::applyPhysicalDns(QString& error)
{
#ifdef Q_OS_WIN
    if (m_physicalDns.valid == false) {
        error = tr("physical adapter was not detected");
        return false;
    }

    const QStringList servers = ipv4DnsServers(m_dnsServers);
    if (servers.isEmpty()) {
        error = tr("no IPv4 DNS servers were configured");
        return false;
    }

    OcSettings app;
    app.setValue(QStringLiteral("Xray/physical-dns-valid"), true);
    app.setValue(QStringLiteral("Xray/physical-dns-adapter"), m_physicalDns.adapterName);
    app.setValue(QStringLiteral("Xray/physical-dns-was-static"), m_physicalDns.wasStatic);
    app.setValue(QStringLiteral("Xray/physical-dns-servers"), m_physicalDns.servers.join(QStringLiteral(", ")));
    app.sync();

    if (setAdapterDns(m_physicalDns.adapterName, true, servers, error) == false) {
        restorePhysicalDns();
        return false;
    }

    m_physicalDnsApplied = true;
    Logger::instance().addMessage(tr("DNS leak protection: set '%1' DNS to %2")
                                      .arg(m_physicalDns.adapterName, servers.join(QStringLiteral(", "))),
        Logger::MessageType::INFO);
    return true;
#else
    Q_UNUSED(error);
    return false;
#endif
}

void XrayClient::restorePhysicalDns()
{
#ifdef Q_OS_WIN
    if (m_physicalDns.valid == false) {
        return;
    }

    QString error;
    if (setAdapterDns(m_physicalDns.adapterName, m_physicalDns.wasStatic, m_physicalDns.servers, error) == false) {
        Logger::instance().addMessage(tr("Could not restore DNS on '%1': %2")
                                          .arg(m_physicalDns.adapterName, error),
            Logger::MessageType::WARNING);
        return;
    }

    clearPhysicalDnsBackup();
    m_physicalDnsApplied = false;
    Logger::instance().addMessage(tr("DNS leak protection: restored '%1' DNS (%2)")
                                      .arg(m_physicalDns.adapterName,
                                          m_physicalDns.wasStatic ? QStringLiteral("static") : QStringLiteral("DHCP")),
        Logger::MessageType::INFO);
#endif
}

void XrayClient::restoreStalePhysicalDns()
{
#ifdef Q_OS_WIN
    OcSettings app;
    if (app.value(QStringLiteral("Xray/physical-dns-valid"), false).toBool() == false) {
        return;
    }

    const QString adapter = app.value(QStringLiteral("Xray/physical-dns-adapter")).toString();
    const bool wasStatic = app.value(QStringLiteral("Xray/physical-dns-was-static"), false).toBool();
    QStringList servers;
    const QString serversValue = app.value(QStringLiteral("Xray/physical-dns-servers")).toString();
    for (const QString& part : serversValue.split(QRegularExpression(QStringLiteral("[\\s,;]+")), Qt::SkipEmptyParts)) {
        QHostAddress address(part);
        if (address.protocol() == QAbstractSocket::IPv4Protocol) {
            servers.append(part);
        }
    }

    if (adapter.isEmpty()) {
        clearPhysicalDnsBackup();
        return;
    }

    QString error;
    if (setAdapterDns(adapter, wasStatic, servers, error)) {
        clearPhysicalDnsBackup();
        Logger::instance().addMessage(tr("Restored leftover DNS settings on '%1'").arg(adapter),
            Logger::MessageType::INFO);
    } else {
        Logger::instance().addMessage(tr("Could not restore leftover DNS settings on '%1': %2")
                                          .arg(adapter, error),
            Logger::MessageType::WARNING);
    }
#endif
}

void XrayClient::readCoreOutput()
{
    if (m_process == nullptr) {
        return;
    }

    const QString text = QString::fromUtf8(m_process->readAllStandardOutput());
    if (text.isEmpty()) {
        return;
    }

    m_output += text;
    if (m_output.size() > 16384) {
        m_output = m_output.right(16384);
    }

    static const QRegularExpression separator(QStringLiteral("[\\r\\n]+"));
    const QStringList lines = text.split(separator, Qt::SkipEmptyParts);
    for (const QString& line : lines) {
        const QString trimmed = line.trimmed();
        if (trimmed.isEmpty()) {
            continue;
        }
        const bool important = trimmed.contains(QStringLiteral("failed"), Qt::CaseInsensitive)
            || trimmed.contains(QStringLiteral("error"), Qt::CaseInsensitive)
            || trimmed.contains(QStringLiteral("warning"), Qt::CaseInsensitive);
        Logger::instance().addMessage(QStringLiteral("Xray: %1").arg(trimmed),
            important ? Logger::MessageType::WARNING : Logger::MessageType::DEBUG,
            Logger::ComponentType::OCONNECT);
    }
}

void XrayClient::checkReady()
{
    if (m_process == nullptr || m_ready) {
        m_readyTimer->stop();
        return;
    }

    if (m_process->state() == QProcess::NotRunning) {
        m_readyTimer->stop();
        return;
    }

    QTcpSocket probe;
    probe.connectToHost(QHostAddress::LocalHost, static_cast<quint16>(m_apiPort));
    const bool reachable = probe.waitForConnected(100);
    probe.abort();

    constexpr int maxChecks = readyTimeoutMs / readyPollIntervalMs;
    const auto failTimeout = [this](const QString& adapterDetail) {
        m_readyTimer->stop();

        QString detail = m_output.trimmed();
        if (detail.size() > 400) {
            detail = detail.right(400);
        }
        QString adapter = adapterDetail.trimmed();
        if (adapter.isEmpty() == false) {
            if (adapter.size() > 400) {
                adapter = adapter.right(400);
            }
            detail += QStringLiteral("\n") + adapter;
        }

        const QString error = QStringLiteral("Xray did not become ready in time. %1").arg(detail);
        Logger::instance().addMessage(error, Logger::MessageType::CRITICAL);
        m_stopping = true;
        cleanup(true);
        m_stopping = false;
        emit errorOccurred(error);
        emit statusChanged(Disconnected);
    };

    if (reachable == false) {
        if (m_readyTimer->isActive() && ++m_readyChecks > maxChecks) {
            failTimeout(QString());
        }
        return;
    }

    if (m_tunMode) {
        QString adapterName;
        QString adapterOutput;
        if (findTunAdapter(adapterName, adapterOutput) == false) {
            if (m_readyTimer->isActive() && ++m_readyChecks > maxChecks) {
                failTimeout(adapterOutput);
            }
            return;
        }

        if (m_tunAdapterName != adapterName) {
            m_tunAdapterName = adapterName;
            Logger::instance().addMessage(tr("TUN adapter '%1' is configured with address 198.18.0.1").arg(adapterName));
        }
    }

    m_ready = true;
    m_readyTimer->stop();

    if (m_tunMode) {
        applyTunAdapterDns();

        if (m_physicalDns.valid) {
            QString dnsError;
            if (applyPhysicalDns(dnsError) == false) {
                Logger::instance().addMessage(tr("DNS leak protection: %1").arg(dnsError),
                    Logger::MessageType::WARNING);
            }
        }
    }

    if (m_tunMode == false) {
#ifdef Q_OS_WIN
        QString proxyError;
        if (applyProxy(m_proxyPort, proxyError)) {
            m_proxyApplied = true;
        } else {
            const QString error = QStringLiteral("The Xray core is running, but %1").arg(proxyError);
            Logger::instance().addMessage(error, Logger::MessageType::CRITICAL);
            m_stopping = true;
            cleanup(true);
            m_stopping = false;
            emit errorOccurred(error);
            emit statusChanged(Disconnected);
            return;
        }
#endif
    }

    const QString cipher = QStringLiteral("Xray %1 (%2)").arg(coreVersion(), modeName());
    const QString dns = m_tunMode
        ? tr("%1 via TUN").arg(m_dnsServers.join(QStringLiteral(", ")))
        : QStringLiteral("Windows system proxy");
    Logger::instance().addMessage(tr("Xray is connected (%1 mode)").arg(modeName()));
    emit connectedInfo(endpoint(), dns, cipher);
    emit statusChanged(Connected);

    m_statsTimer->start();
    QTimer::singleShot(400, this, &XrayClient::requestStats);
}

void XrayClient::coreFinished(int exitCode, QProcess::ExitStatus status)
{
    if (m_stopping) {
        return;
    }

    QString detail = m_output.trimmed();
    if (detail.size() > 400) {
        detail = detail.right(400);
    }
    const QString error = QStringLiteral("Xray core stopped (exit %1, %2). %3")
                              .arg(exitCode)
                              .arg(status == QProcess::CrashExit ? QStringLiteral("crashed") : QStringLiteral("exited"))
                              .arg(detail);
    Logger::instance().addMessage(error, Logger::MessageType::CRITICAL);
    cleanup(true);
    emit errorOccurred(error);
    emit statusChanged(Disconnected);
}

void XrayClient::coreError(QProcess::ProcessError error)
{
    if (m_stopping || error == QProcess::Crashed) {
        return;
    }
    if (error == QProcess::FailedToStart) {
        const QString message = tr("Could not run xray.exe: %1").arg(m_process != nullptr ? m_process->errorString() : QString());
        Logger::instance().addMessage(message, Logger::MessageType::CRITICAL);
        cleanup(true);
        emit errorOccurred(message);
        emit statusChanged(Disconnected);
    }
}

void XrayClient::requestStats()
{
    if (m_ready == false || m_apiPort == 0 || m_statsProcess->state() != QProcess::NotRunning) {
        return;
    }

    m_statsProcess->setProgram(xrayExecutable());
    m_statsProcess->setWorkingDirectory(QCoreApplication::applicationDirPath());
    m_statsProcess->setArguments({
        QStringLiteral("api"),
        QStringLiteral("statsquery"),
        QStringLiteral("--server=127.0.0.1:%1").arg(m_apiPort),
        QStringLiteral("-pattern"),
        QStringLiteral("inbound>>>oc-in>>>traffic>>>"),
    });
    m_statsProcess->start();
}

void XrayClient::statsFinished()
{
    const QByteArray output = m_statsProcess->readAllStandardOutput();
    if (output.isEmpty()) {
        return;
    }

    const QJsonDocument doc = QJsonDocument::fromJson(output);
    if (doc.isObject() == false) {
        return;
    }

    quint64 upload = 0;
    quint64 download = 0;
    const QJsonArray stats = doc.object().value(QStringLiteral("stat")).toArray();
    for (const QJsonValue& value : stats) {
        const QJsonObject entry = value.toObject();
        const QString name = entry.value(QStringLiteral("name")).toString();
        const QJsonValue raw = entry.value(QStringLiteral("value"));
        const quint64 counter = static_cast<quint64>(raw.toVariant().toDouble());
        if (name.endsWith(QStringLiteral("uplink"))) {
            upload = counter;
        } else if (name.endsWith(QStringLiteral("downlink"))) {
            download = counter;
        }
    }

    emit statsChanged(upload, download,
        QStringLiteral("Xray %1 \u00b7 %2").arg(coreVersion(), modeName()));
}

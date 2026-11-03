#include "XraySubscriptionUpdater.h"

#include "OcSettings.h"
#include "server_storage.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>

namespace {

const QString configSuffix = QStringLiteral("/xray-config");
const QString subscriptionSuffix = QStringLiteral("/xray-subscription-url");

QString profileNameFromKey(const QString& key, const QString& suffix)
{
    QString name = key;
    name.remove(0, QStringLiteral("server:").size());
    name.chop(suffix.size());
    return name;
}

QString endpointKey(const QString& configJson)
{
    const QJsonObject config = QJsonDocument::fromJson(configJson.toUtf8()).object();
    QString host;
    int port = 0;
    if (XrayConfig::primaryEndpoint(config, host, port) == false) {
        return {};
    }
    return QStringLiteral("%1:%2").arg(host.trimmed().toLower()).arg(port);
}

QString uniqueName(const QString& desired, const QStringList& names)
{
    QString base = desired.trimmed();
    if (base.isEmpty()) {
        base = QStringLiteral("Xray profile");
    }

    QString candidate = base;
    int suffix = 2;
    bool duplicate = true;
    while (duplicate) {
        duplicate = false;
        for (const QString& name : names) {
            if (name.compare(candidate, Qt::CaseInsensitive) == 0) {
                candidate = QStringLiteral("%1 (%2)").arg(base).arg(suffix++);
                duplicate = true;
                break;
            }
        }
    }
    return candidate;
}

struct ExistingProfile {
    QString name;
    QString endpoint;
    bool used = false;
};

} // namespace

QStringList XraySubscriptionUpdater::subscriptionUrls()
{
    OcSettings settings;
    QSet<QString> urls;
    for (const QString& key : settings.allKeys()) {
        if (key.startsWith(QStringLiteral("server:")) && key.endsWith(subscriptionSuffix)) {
            const QString url = settings.value(key).toString().trimmed();
            if (url.isEmpty() == false) {
                urls.insert(url);
            }
        }
    }

    QStringList result = urls.values();
    result.sort(Qt::CaseInsensitive);
    return result;
}

XraySubscriptionSyncResult XraySubscriptionUpdater::sync(const QString& url,
    const QVector<XrayImportedProfile>& nodes)
{
    XraySubscriptionSyncResult result;

    OcSettings settings;
    QStringList allNames;
    for (const QString& key : settings.allKeys()) {
        if (key.startsWith(QStringLiteral("server:")) && key.endsWith(QStringLiteral("/server"))) {
            allNames.append(profileNameFromKey(key, QStringLiteral("/server")));
        }
    }

    QVector<ExistingProfile> group;
    for (const QString& key : settings.allKeys()) {
        if (key.startsWith(QStringLiteral("server:")) == false || key.endsWith(subscriptionSuffix) == false) {
            continue;
        }
        if (settings.value(key).toString() != url) {
            continue;
        }

        const QString name = profileNameFromKey(key, subscriptionSuffix);
        const QString config = settings.value(QStringLiteral("server:%1%2").arg(name, configSuffix)).toString();
        if (config.isEmpty()) {
            continue;
        }

        ExistingProfile existing;
        existing.name = name;
        existing.endpoint = endpointKey(config);
        group.append(existing);
    }

    bool hasTemplate = false;
    QString templateMode = QStringLiteral("tun");
    QString templateDns;
    bool templateSniffing = false;
    bool templateBlockQuic = true;
    bool templateFragment = false;
    QString templatePackets = QStringLiteral("tlshello");
    QString templateLength = QStringLiteral("100-200");
    QString templateInterval = QStringLiteral("10-20");

    if (group.isEmpty() == false) {
        StoredServer templateServer;
        QString templateName = group.first().name;
        templateServer.load(templateName);
        hasTemplate = true;
        templateMode = templateServer.get_xray_mode();
        templateDns = templateServer.get_xray_dns();
        templateSniffing = templateServer.get_xray_sniffing();
        templateBlockQuic = templateServer.get_xray_block_quic();
        templateFragment = templateServer.get_xray_fragment();
        templatePackets = templateServer.get_xray_fragment_packets();
        templateLength = templateServer.get_xray_fragment_length();
        templateInterval = templateServer.get_xray_fragment_interval();
    }

    for (const XrayImportedProfile& node : nodes) {
        QString nodeEndpoint = endpointKey(node.configJson);
        if (nodeEndpoint.isEmpty()) {
            nodeEndpoint = node.server.trimmed().toLower();
        }

        int match = -1;
        for (int i = 0; i < group.size(); ++i) {
            if (group.at(i).used == false
                && group.at(i).name.compare(node.name, Qt::CaseInsensitive) == 0) {
                match = i;
                break;
            }
        }
        if (match < 0 && nodeEndpoint.isEmpty() == false) {
            for (int i = 0; i < group.size(); ++i) {
                if (group.at(i).used == false
                    && group.at(i).endpoint.compare(nodeEndpoint, Qt::CaseInsensitive) == 0) {
                    match = i;
                    break;
                }
            }
        }

        if (match >= 0) {
            ExistingProfile& existing = group[match];
            StoredServer server;
            QString name = existing.name;
            server.load(name);
            server.set_xray_config(node.configJson);
            server.set_xray_subscription_url(url);
            if (server.get_xray_server_override().isEmpty() && server.get_xray_port_override() == 0) {
                server.set_server_gateway(node.server);
            }
            server.save();

            existing.used = true;
            ++result.updated;
            continue;
        }

        const QString name = uniqueName(node.name, allNames);
        allNames.append(name);

        StoredServer server;
        server.set_label(name);
        server.set_server_gateway(node.server);
        server.set_xray_config(node.configJson);
        server.set_xray_subscription_url(url);

        if (hasTemplate) {
            server.set_xray_mode(templateMode);
            server.set_xray_dns(templateDns);
            server.set_xray_sniffing(templateSniffing);
            server.set_xray_block_quic(templateBlockQuic);
            server.set_xray_fragment(templateFragment);
            server.set_xray_fragment_packets(templatePackets);
            server.set_xray_fragment_length(templateLength);
            server.set_xray_fragment_interval(templateInterval);
        } else {
            server.set_xray_mode(QStringLiteral("tun"));
        }

        server.save();
        ++result.added;
    }

    for (const ExistingProfile& existing : group) {
        if (existing.used == false) {
            result.stale.append(existing.name);
        }
    }

    result.ok = true;
    return result;
}

int XraySubscriptionUpdater::removeProfiles(const QStringList& names)
{
    OcSettings settings;
    int removed = 0;
    for (const QString& name : names) {
        const QString prefix = QStringLiteral("server:%1/").arg(name);
        for (const QString& key : settings.allKeys()) {
            if (key.startsWith(prefix)) {
                settings.remove(key);
            }
        }
        ++removed;
    }
    return removed;
}

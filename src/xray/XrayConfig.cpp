#include "XrayConfig.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QRegularExpression>
#include <QSet>
#include <QUrl>
#include <QUrlQuery>

namespace {

QByteArray decodeBase64(const QString& input)
{
    QString s = input.trimmed();
    s.replace(QLatin1Char('-'), QLatin1Char('+'));
    s.replace(QLatin1Char('_'), QLatin1Char('/'));
    while (s.size() % 4 != 0) {
        s += QLatin1Char('=');
    }
    return QByteArray::fromBase64(s.toLatin1());
}

QString withoutWhitespace(QString value)
{
    value.remove(QRegularExpression(QStringLiteral("[\\s]+")));
    return value;
}

QString normalizedSpiderX(QString value)
{
    value = QUrl::fromPercentEncoding(value.toUtf8());
    value = withoutWhitespace(value);
    if (value.isEmpty()) {
        return QStringLiteral("/");
    }
    if (value.startsWith(QLatin1Char('/')) == false) {
        value.prepend(QLatin1Char('/'));
    }
    return value;
}

void normalizeTlsObject(QJsonObject& tls, bool reality)
{
    QString serverName = tls.value(QStringLiteral("serverName")).toString().trimmed();
    if (serverName.isEmpty() == false) {
        tls.insert(QStringLiteral("serverName"), serverName);
    }

    QString fingerprint = tls.value(QStringLiteral("fingerprint")).toString().trimmed().toLower();
    if (fingerprint.isEmpty() && reality) {
        fingerprint = QStringLiteral("chrome");
    }
    if (fingerprint.isEmpty() == false) {
        tls.insert(QStringLiteral("fingerprint"), fingerprint);
    }

    if (reality) {
        QString publicKey = withoutWhitespace(tls.value(QStringLiteral("publicKey")).toString());
        while (publicKey.endsWith(QLatin1Char('='))) {
            publicKey.chop(1);
        }
        tls.insert(QStringLiteral("publicKey"), publicKey);
        tls.insert(QStringLiteral("shortId"), withoutWhitespace(tls.value(QStringLiteral("shortId")).toString()));
        tls.insert(QStringLiteral("spiderX"), normalizedSpiderX(tls.value(QStringLiteral("spiderX")).toString()));
    } else if (tls.contains(QStringLiteral("alpn"))) {
        QJsonArray alpn;
        for (const QJsonValue& item : tls.value(QStringLiteral("alpn")).toArray()) {
            const QString value = item.toString().trimmed();
            if (value.isEmpty() == false) {
                alpn.append(value);
            }
        }
        tls.insert(QStringLiteral("alpn"), alpn);
    }
}

void normalizeOutbound(QJsonObject& outbound)
{
    QJsonObject stream = outbound.value(QStringLiteral("streamSettings")).toObject();
    if (stream.isEmpty()) {
        return;
    }

    const QString security = stream.value(QStringLiteral("security")).toString().toLower();
    if (security == QLatin1String("reality")) {
        QJsonObject reality = stream.value(QStringLiteral("realitySettings")).toObject();
        normalizeTlsObject(reality, true);
        stream.insert(QStringLiteral("realitySettings"), reality);
        outbound.insert(QStringLiteral("streamSettings"), stream);
    } else if (security == QLatin1String("tls")) {
        QJsonObject tls = stream.value(QStringLiteral("tlsSettings")).toObject();
        normalizeTlsObject(tls, false);
        stream.insert(QStringLiteral("tlsSettings"), tls);
        outbound.insert(QStringLiteral("streamSettings"), stream);
    }
}

QString queryValue(const QUrlQuery& query, const QStringList& names, const QString& fallback = {})
{
    const auto items = query.queryItems(QUrl::FullyDecoded);
    for (const QString& name : names) {
        for (const auto& item : items) {
            if (item.first.compare(name, Qt::CaseInsensitive) == 0) {
                return item.second;
            }
        }
    }
    return fallback;
}

QJsonArray splitCsv(const QString& value)
{
    QJsonArray array;
    for (const QString& part : value.split(QLatin1Char(','), Qt::SkipEmptyParts)) {
        array.append(part.trimmed());
    }
    return array;
}

QJsonArray hostArray(const QString& host)
{
    QJsonArray array;
    if (host.isEmpty() == false) {
        array.append(host);
    }
    return array;
}

QJsonObject buildStreamSettings(const QString& network,
    const QString& security,
    const QString& host,
    const QString& path,
    const QString& serviceName,
    const QString& sni,
    const QString& fingerprint,
    const QString& alpn,
    const QString& publicKey,
    const QString& shortId,
    const QString& spiderX,
    const QString& headerType)
{
    QString net = network.trimmed().toLower();
    if (net.isEmpty()) {
        net = QStringLiteral("tcp");
    }
    if (net == QLatin1String("http")) {
        net = QStringLiteral("h2");
    }

    QJsonObject stream;
    stream.insert(QStringLiteral("network"), net);

    const QString sec = security.trimmed().toLower();
    if (sec == QLatin1String("tls") || sec == QLatin1String("reality")) {
        stream.insert(QStringLiteral("security"), sec);

        QJsonObject tls;
        QString serverName = sni.trimmed();
        if (serverName.isEmpty()) {
            serverName = host.trimmed();
        }
        if (serverName.isEmpty() == false) {
            tls.insert(QStringLiteral("serverName"), serverName);
        }
        const QString fingerprintValue = fingerprint.trimmed().toLower();
        if (fingerprintValue.isEmpty() == false) {
            tls.insert(QStringLiteral("fingerprint"), fingerprintValue);
        }
        if (alpn.isEmpty() == false) {
            tls.insert(QStringLiteral("alpn"), splitCsv(alpn));
        }

        if (sec == QLatin1String("reality")) {
            QJsonObject reality = tls;
            if (reality.contains(QStringLiteral("fingerprint")) == false) {
                reality.insert(QStringLiteral("fingerprint"), QStringLiteral("chrome"));
            }
            QString key = withoutWhitespace(publicKey);
            while (key.endsWith(QLatin1Char('='))) {
                key.chop(1);
            }
            reality.insert(QStringLiteral("publicKey"), key);
            reality.insert(QStringLiteral("shortId"), withoutWhitespace(shortId));
            reality.insert(QStringLiteral("spiderX"), normalizedSpiderX(spiderX));
            reality.insert(QStringLiteral("show"), false);
            stream.insert(QStringLiteral("realitySettings"), reality);
        } else {
            stream.insert(QStringLiteral("tlsSettings"), tls);
        }
    }

    if (net == QLatin1String("ws")) {
        QJsonObject ws;
        ws.insert(QStringLiteral("path"), path.isEmpty() ? QStringLiteral("/") : path);
        if (host.isEmpty() == false) {
            QJsonObject headers;
            headers.insert(QStringLiteral("Host"), host);
            ws.insert(QStringLiteral("headers"), headers);
        }
        stream.insert(QStringLiteral("wsSettings"), ws);
    } else if (net == QLatin1String("grpc")) {
        QJsonObject grpc;
        QString name = serviceName;
        if (name.isEmpty()) {
            name = path;
            while (name.startsWith(QLatin1Char('/'))) {
                name.remove(0, 1);
            }
        }
        grpc.insert(QStringLiteral("serviceName"), name);
        stream.insert(QStringLiteral("grpcSettings"), grpc);
    } else if (net == QLatin1String("h2")) {
        QJsonObject http;
        if (host.isEmpty() == false) {
            http.insert(QStringLiteral("host"), hostArray(host));
        }
        if (path.isEmpty() == false) {
            http.insert(QStringLiteral("path"), path);
        }
        stream.insert(QStringLiteral("httpSettings"), http);
    } else if (net == QLatin1String("httpupgrade")) {
        QJsonObject upgrade;
        upgrade.insert(QStringLiteral("path"), path.isEmpty() ? QStringLiteral("/") : path);
        if (host.isEmpty() == false) {
            upgrade.insert(QStringLiteral("host"), host);
        }
        stream.insert(QStringLiteral("httpupgradeSettings"), upgrade);
    } else if (net == QLatin1String("xhttp") || net == QLatin1String("splithttp")) {
        QJsonObject xhttp;
        xhttp.insert(QStringLiteral("path"), path.isEmpty() ? QStringLiteral("/") : path);
        if (host.isEmpty() == false) {
            xhttp.insert(QStringLiteral("host"), host);
        }
        stream.insert(QStringLiteral("xhttpSettings"), xhttp);
    } else if (net == QLatin1String("kcp")) {
        QJsonObject kcp;
        if (headerType.isEmpty() == false) {
            QJsonObject header;
            header.insert(QStringLiteral("type"), headerType);
            kcp.insert(QStringLiteral("header"), header);
        }
        if (path.isEmpty() == false) {
            kcp.insert(QStringLiteral("seed"), path);
        }
        stream.insert(QStringLiteral("kcpSettings"), kcp);
    } else if (net == QLatin1String("quic")) {
        QJsonObject quic;
        quic.insert(QStringLiteral("security"), QStringLiteral("none"));
        if (headerType.isEmpty() == false) {
            QJsonObject header;
            header.insert(QStringLiteral("type"), headerType);
            quic.insert(QStringLiteral("header"), header);
        }
        stream.insert(QStringLiteral("quicSettings"), quic);
    } else if (net == QLatin1String("tcp") && headerType == QLatin1String("http")) {
        QJsonObject tcp;
        QJsonObject header;
        header.insert(QStringLiteral("type"), QStringLiteral("http"));
        QJsonObject request;
        QJsonArray paths;
        paths.append(path.isEmpty() ? QStringLiteral("/") : path);
        request.insert(QStringLiteral("path"), paths);
        if (host.isEmpty() == false) {
            QJsonObject headers;
            headers.insert(QStringLiteral("Host"), hostArray(host));
            request.insert(QStringLiteral("headers"), headers);
        }
        header.insert(QStringLiteral("request"), request);
        tcp.insert(QStringLiteral("header"), header);
        stream.insert(QStringLiteral("tcpSettings"), tcp);
    }

    return stream;
}

QString cleanName(QString name, const QString& fallback)
{
    name = name.trimmed();
    if (name.isEmpty()) {
        name = fallback;
    }
    name = name.simplified();
    name.replace(QLatin1Char('/'), QLatin1Char('-'));
    name.replace(QLatin1Char('\\'), QLatin1Char('-'));
    if (name.size() > 64) {
        name.truncate(61);
        name += QStringLiteral("...");
    }
    return name;
}

QString nameFromFragment(const QUrl& url, const QString& fallback)
{
    return cleanName(url.fragment(), fallback);
}

bool parseHostPort(const QString& input, QString& host, int& port)
{
    QString value = input.trimmed();
    if (value.startsWith(QLatin1Char('['))) {
        const int end = value.indexOf(QLatin1Char(']'));
        if (end < 0) {
            return false;
        }
        host = value.mid(1, end - 1);
        const int colon = value.indexOf(QLatin1Char(':'), end);
        if (colon < 0) {
            return false;
        }
        port = value.mid(colon + 1).toInt();
    } else {
        const int colon = value.lastIndexOf(QLatin1Char(':'));
        if (colon <= 0) {
            return false;
        }
        host = value.left(colon);
        port = value.mid(colon + 1).toInt();
    }
    return host.isEmpty() == false && port > 0 && port <= 65535;
}

QJsonObject outboundFromLink(const QString& protocol,
    const QJsonObject& settings,
    const QJsonObject& stream)
{
    QJsonObject outbound;
    outbound.insert(QStringLiteral("tag"), QStringLiteral("proxy"));
    outbound.insert(QStringLiteral("protocol"), protocol);
    outbound.insert(QStringLiteral("settings"), settings);
    if (stream.isEmpty() == false) {
        outbound.insert(QStringLiteral("streamSettings"), stream);
    }
    return outbound;
}

bool singleOutboundConfig(const QJsonObject& outbound, XrayImportedProfile& profile, QString& error)
{
    QJsonObject config;
    QJsonArray outbounds;
    outbounds.append(outbound);
    config.insert(QStringLiteral("outbounds"), outbounds);

    QString host;
    int port = 0;

    const QJsonObject settings = outbound.value(QStringLiteral("settings")).toObject();
    if (settings.contains(QStringLiteral("vnext"))) {
        const QJsonObject server = settings.value(QStringLiteral("vnext")).toArray().at(0).toObject();
        host = server.value(QStringLiteral("address")).toString();
        port = server.value(QStringLiteral("port")).toInt();
    } else if (settings.contains(QStringLiteral("servers"))) {
        const QJsonObject server = settings.value(QStringLiteral("servers")).toArray().at(0).toObject();
        host = server.value(QStringLiteral("address")).toString();
        port = server.value(QStringLiteral("port")).toInt();
    }

    if (host.isEmpty() || port <= 0) {
        error = QStringLiteral("The imported config has no usable server address");
        return false;
    }

    profile.server = QStringLiteral("%1:%2").arg(host).arg(port);
    profile.name = cleanName(QString(), profile.server);
    profile.configJson = QString::fromUtf8(QJsonDocument(config).toJson(QJsonDocument::Compact));
    return true;
}

} // namespace

bool XrayConfig::looksLikeJson(const QString& text)
{
    return text.trimmed().startsWith(QLatin1Char('{'));
}

bool XrayConfig::looksLikeUri(const QString& text)
{
    static const QStringList schemes = {
        QStringLiteral("vmess://"),
        QStringLiteral("vless://"),
        QStringLiteral("trojan://"),
        QStringLiteral("ss://"),
        QStringLiteral("socks://"),
        QStringLiteral("socks5://"),
        QStringLiteral("http://"),
        QStringLiteral("https://")
    };
    const QString value = text.trimmed();
    for (const QString& scheme : schemes) {
        if (value.startsWith(scheme, Qt::CaseInsensitive)) {
            return true;
        }
    }
    return false;
}

bool XrayConfig::decodeSubscription(const QByteArray& data, QString& text, QString& error)
{
    QByteArray trimmed = data.trimmed();
    if (trimmed.isEmpty()) {
        error = QStringLiteral("The subscription response is empty");
        return false;
    }

    QString candidate = QString::fromUtf8(decodeBase64(QString::fromLatin1(trimmed)));
    if (candidate.contains(QStringLiteral("://")) == false && candidate.trimmed().startsWith(QLatin1Char('{')) == false) {
        candidate = QString::fromUtf8(trimmed);
    }

    if (candidate.contains(QStringLiteral("://")) == false && candidate.trimmed().startsWith(QLatin1Char('{')) == false) {
        error = QStringLiteral("The subscription does not contain links or a JSON config");
        return false;
    }

    text = candidate;
    return true;
}

bool XrayConfig::importJson(const QString& text, XrayImportedProfile& profile, QString& error)
{
    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(text.toUtf8(), &parseError);
    if (doc.isObject() == false) {
        error = QStringLiteral("Invalid JSON: %1").arg(parseError.errorString());
        return false;
    }

    QJsonObject root = doc.object();
    if (root.contains(QStringLiteral("outbounds"))) {
        if (root.value(QStringLiteral("outbounds")).isArray() == false
            || root.value(QStringLiteral("outbounds")).toArray().isEmpty()) {
            error = QStringLiteral("The config has no outbounds");
            return false;
        }
    } else if (root.contains(QStringLiteral("protocol"))) {
        QJsonObject wrapper;
        QJsonArray outbounds;
        outbounds.append(root);
        wrapper.insert(QStringLiteral("outbounds"), outbounds);
        root = wrapper;
    } else {
        error = QStringLiteral("The JSON does not look like a V2Ray/Xray config");
        return false;
    }

    QJsonArray outbounds = root.value(QStringLiteral("outbounds")).toArray();
    for (int i = 0; i < outbounds.size(); ++i) {
        QJsonObject outbound = outbounds.at(i).toObject();
        if (outbound.value(QStringLiteral("tag")).toString().isEmpty()) {
            outbound.insert(QStringLiteral("tag"),
                i == 0 ? QStringLiteral("proxy") : QStringLiteral("out-%1").arg(i + 1));
            outbounds.replace(i, outbound);
        }
    }
    root.insert(QStringLiteral("outbounds"), outbounds);

    const QJsonObject first = outbounds.at(0).toObject();
    const QJsonObject settings = first.value(QStringLiteral("settings")).toObject();
    QString host;
    int port = 0;
    if (settings.contains(QStringLiteral("vnext"))) {
        const QJsonObject server = settings.value(QStringLiteral("vnext")).toArray().at(0).toObject();
        host = server.value(QStringLiteral("address")).toString();
        port = server.value(QStringLiteral("port")).toInt();
    } else if (settings.contains(QStringLiteral("servers"))) {
        const QJsonObject server = settings.value(QStringLiteral("servers")).toArray().at(0).toObject();
        host = server.value(QStringLiteral("address")).toString();
        port = server.value(QStringLiteral("port")).toInt();
    }

    profile.server = host.isEmpty() ? QStringLiteral("xray") : QStringLiteral("%1:%2").arg(host).arg(port);
    QString name = root.value(QStringLiteral("remarks")).toString();
    if (name.isEmpty()) {
        name = root.value(QStringLiteral("ps")).toString();
    }
    if (name.isEmpty()) {
        name = root.value(QStringLiteral("name")).toString();
    }
    profile.name = cleanName(name, profile.server);
    profile.configJson = QString::fromUtf8(QJsonDocument(root).toJson(QJsonDocument::Compact));
    return true;
}

bool XrayConfig::importUri(const QString& uri, XrayImportedProfile& profile, QString& error)
{
    const QString value = uri.trimmed();
    const QString scheme = value.section(QStringLiteral("://"), 0, 0).toLower();

    if (scheme == QLatin1String("vmess")) {
        const QString payload = value.mid(value.indexOf(QStringLiteral("://")) + 3);
        QJsonParseError parseError;
        const QJsonDocument doc = QJsonDocument::fromJson(decodeBase64(payload), &parseError);
        if (doc.isObject() == false) {
            error = QStringLiteral("Invalid vmess:// link");
            return false;
        }
        const QJsonObject v = doc.object();
        const QString host = v.value(QStringLiteral("add")).toString();
        const int port = v.value(QStringLiteral("port")).toVariant().toInt();
        const QString id = v.value(QStringLiteral("id")).toString();
        if (host.isEmpty() || port <= 0 || id.isEmpty()) {
            error = QStringLiteral("The vmess:// link is missing address, port or id");
            return false;
        }

        const QString net = v.value(QStringLiteral("net")).toString(QStringLiteral("tcp"));
        const QString headerType = v.value(QStringLiteral("type")).toString();
        const QString path = v.value(QStringLiteral("path")).toString();
        const QString headerHost = v.value(QStringLiteral("host")).toString();
        QString sni = v.value(QStringLiteral("sni")).toString();
        if (sni.isEmpty()) {
            sni = headerHost;
        }
        const QString tlsValue = v.value(QStringLiteral("tls")).toString();
        const QString security = (tlsValue == QLatin1String("tls") || v.value(QStringLiteral("tls")).toBool())
            ? QStringLiteral("tls")
            : QString();

        QJsonObject user;
        user.insert(QStringLiteral("id"), id);
        user.insert(QStringLiteral("alterId"), v.value(QStringLiteral("aid")).toVariant().toInt());
        user.insert(QStringLiteral("security"), v.value(QStringLiteral("scy")).toString(QStringLiteral("auto")));

        QJsonObject server;
        server.insert(QStringLiteral("address"), host);
        server.insert(QStringLiteral("port"), port);
        server.insert(QStringLiteral("users"), QJsonArray{ user });

        QJsonObject settings;
        settings.insert(QStringLiteral("vnext"), QJsonArray{ server });

        const QJsonObject stream = buildStreamSettings(net, security, headerHost, path,
            QString(), sni, v.value(QStringLiteral("fp")).toString(),
            v.value(QStringLiteral("alpn")).toString(), QString(), QString(), QString(), headerType);

        const QJsonObject outbound = outboundFromLink(QStringLiteral("vmess"), settings, stream);
        if (singleOutboundConfig(outbound, profile, error) == false) {
            return false;
        }
        profile.name = cleanName(v.value(QStringLiteral("ps")).toString(), profile.server);
        return true;
    }

    QUrl url(value, QUrl::TolerantMode);
    if (scheme != QLatin1String("ss") && (url.isValid() == false || url.host().isEmpty())) {
        error = QStringLiteral("Invalid %1 link").arg(scheme);
        return false;
    }

    QUrlQuery query(url.query());
    const QString host = url.host();
    const QString network = queryValue(query, { QStringLiteral("type"), QStringLiteral("network"), QStringLiteral("net") },
        QStringLiteral("tcp"));
    const QString path = queryValue(query, { QStringLiteral("path") });
    const QString headerHost = queryValue(query, { QStringLiteral("host") });
    const QString serviceName = queryValue(query, { QStringLiteral("serviceName") });
    const QString headerType = queryValue(query, { QStringLiteral("headerType") });
    const QString fingerprint = queryValue(query, { QStringLiteral("fp"), QStringLiteral("fingerprint") });
    const QString alpn = queryValue(query, { QStringLiteral("alpn") });
    QString sni = queryValue(query, { QStringLiteral("sni"), QStringLiteral("peer") });
    if (sni.isEmpty()) {
        sni = headerHost;
    }

    if (scheme == QLatin1String("vless")) {
        const QString id = url.userName();
        const int port = url.port(443);
        if (id.isEmpty()) {
            error = QStringLiteral("The vless:// link has no user id");
            return false;
        }
        QString security = queryValue(query, { QStringLiteral("security") }, QStringLiteral("none"));
        const QString publicKey = queryValue(query, { QStringLiteral("pbk"), QStringLiteral("publicKey") });
        const QString shortId = queryValue(query, { QStringLiteral("sid"), QStringLiteral("shortId") });
        const QString spiderX = queryValue(query, { QStringLiteral("spx"), QStringLiteral("spiderX") });
        if (security == QLatin1String("reality") && publicKey.isEmpty()) {
            error = QStringLiteral("The reality link is missing its public key (pbk)");
            return false;
        }

        QJsonObject user;
        user.insert(QStringLiteral("id"), id);
        user.insert(QStringLiteral("encryption"), QStringLiteral("none"));
        const QString flow = queryValue(query, { QStringLiteral("flow") });
        if (flow.isEmpty() == false) {
            user.insert(QStringLiteral("flow"), flow);
        }

        QJsonObject server;
        server.insert(QStringLiteral("address"), host);
        server.insert(QStringLiteral("port"), port);
        server.insert(QStringLiteral("users"), QJsonArray{ user });

        QJsonObject settings;
        settings.insert(QStringLiteral("vnext"), QJsonArray{ server });

        const QJsonObject stream = buildStreamSettings(network, security, headerHost, path,
            serviceName, sni, fingerprint, alpn, publicKey, shortId, spiderX, headerType);

        const QJsonObject outbound = outboundFromLink(QStringLiteral("vless"), settings, stream);
        if (singleOutboundConfig(outbound, profile, error) == false) {
            return false;
        }
        profile.name = nameFromFragment(url, profile.server);
        return true;
    }

    if (scheme == QLatin1String("trojan")) {
        const QString password = url.userName();
        const int port = url.port(443);
        if (password.isEmpty()) {
            error = QStringLiteral("The trojan:// link has no password");
            return false;
        }
        QString security = queryValue(query, { QStringLiteral("security") }, QStringLiteral("tls"));

        QJsonObject server;
        server.insert(QStringLiteral("address"), host);
        server.insert(QStringLiteral("port"), port);
        server.insert(QStringLiteral("password"), password);

        QJsonObject settings;
        settings.insert(QStringLiteral("servers"), QJsonArray{ server });

        const QJsonObject stream = buildStreamSettings(network, security, headerHost, path,
            serviceName, sni, fingerprint, alpn, QString(), QString(), QString(), headerType);

        const QJsonObject outbound = outboundFromLink(QStringLiteral("trojan"), settings, stream);
        if (singleOutboundConfig(outbound, profile, error) == false) {
            return false;
        }
        profile.name = nameFromFragment(url, profile.server);
        return true;
    }

    if (scheme == QLatin1String("ss")) {
        QString body = value.mid(value.indexOf(QStringLiteral("://")) + 3);
        QString fragment;
        const int hash = body.indexOf(QLatin1Char('#'));
        if (hash >= 0) {
            fragment = QUrl::fromPercentEncoding(body.mid(hash + 1).toUtf8());
            body.truncate(hash);
        }
        QString queryString;
        const int question = body.indexOf(QLatin1Char('?'));
        if (question >= 0) {
            queryString = body.mid(question + 1);
            body.truncate(question);
        }

        QString userinfo;
        QString hostport;
        if (body.contains(QLatin1Char('@'))) {
            const int at = body.lastIndexOf(QLatin1Char('@'));
            userinfo = body.left(at);
            hostport = body.mid(at + 1);
        } else {
            const QString decoded = QString::fromUtf8(decodeBase64(body));
            const int at = decoded.lastIndexOf(QLatin1Char('@'));
            if (at < 0) {
                error = QStringLiteral("Invalid ss:// link");
                return false;
            }
            userinfo = decoded.left(at);
            hostport = decoded.mid(at + 1);
        }

        if (userinfo.contains(QLatin1Char(':')) == false) {
            userinfo = QString::fromUtf8(decodeBase64(userinfo));
        }
        const int colon = userinfo.indexOf(QLatin1Char(':'));
        if (colon <= 0) {
            error = QStringLiteral("The ss:// link has no method or password");
            return false;
        }
        const QString method = userinfo.left(colon);
        const QString password = userinfo.mid(colon + 1);

        QString serverHost;
        int port = 0;
        if (parseHostPort(hostport, serverHost, port) == false) {
            error = QStringLiteral("The ss:// link has an invalid server address");
            return false;
        }

        const QUrlQuery ssQuery(queryString);
        if (queryValue(ssQuery, { QStringLiteral("plugin") }).isEmpty() == false) {
            error = QStringLiteral("Shadowsocks plugins are not supported");
            return false;
        }

        QJsonObject server;
        server.insert(QStringLiteral("address"), serverHost);
        server.insert(QStringLiteral("port"), port);
        server.insert(QStringLiteral("method"), method);
        server.insert(QStringLiteral("password"), password);

        QJsonObject settings;
        settings.insert(QStringLiteral("servers"), QJsonArray{ server });

        const QJsonObject outbound = outboundFromLink(QStringLiteral("shadowsocks"), settings, QJsonObject());
        if (singleOutboundConfig(outbound, profile, error) == false) {
            return false;
        }
        profile.name = cleanName(fragment, profile.server);
        return true;
    }

    if (scheme == QLatin1String("socks") || scheme == QLatin1String("socks5")
        || scheme == QLatin1String("http")) {
        const bool isSocks = scheme.startsWith(QLatin1String("socks"));
        const int port = url.port(isSocks ? 1080 : 8080);
        QString user = url.userName();
        QString password = url.password();
        if (user.isEmpty() == false && password.isEmpty()) {
            const QString decoded = QString::fromUtf8(decodeBase64(user));
            if (decoded.contains(QLatin1Char(':'))) {
                user = decoded.section(QLatin1Char(':'), 0, 0);
                password = decoded.section(QLatin1Char(':'), 1);
            }
        }

        QJsonObject server;
        server.insert(QStringLiteral("address"), host);
        server.insert(QStringLiteral("port"), port);
        if (user.isEmpty() == false) {
            QJsonObject credential;
            credential.insert(QStringLiteral("user"), user);
            credential.insert(QStringLiteral("pass"), password);
            server.insert(QStringLiteral("users"), QJsonArray{ credential });
        }

        QJsonObject settings;
        settings.insert(QStringLiteral("servers"), QJsonArray{ server });

        const QJsonObject outbound = outboundFromLink(
            isSocks ? QStringLiteral("socks") : QStringLiteral("http"), settings, QJsonObject());
        if (singleOutboundConfig(outbound, profile, error) == false) {
            return false;
        }
        profile.name = nameFromFragment(url, profile.server);
        return true;
    }

    error = QStringLiteral("Unsupported link type: %1").arg(scheme);
    return false;
}

bool XrayConfig::importText(const QString& text, QVector<XrayImportedProfile>& profiles, QString& error)
{
    QString body = text.trimmed();
    if (body.isEmpty()) {
        error = QStringLiteral("Paste a config, a share link or a subscription");
        return false;
    }

    if (looksLikeJson(body) == false && body.contains(QStringLiteral("://")) == false) {
        QString decoded;
        if (decodeSubscription(body.toUtf8(), decoded, error) == false) {
            return false;
        }
        body = decoded.trimmed();
    }

    profiles.clear();
    if (looksLikeJson(body)) {
        XrayImportedProfile profile;
        if (importJson(body, profile, error) == false) {
            return false;
        }
        profiles.append(profile);
        return true;
    }

    static const QRegularExpression separator(QStringLiteral("[\\r\\n]+"));
    const QStringList lines = body.split(separator, Qt::SkipEmptyParts);
    int lineNumber = 0;
    for (const QString& rawLine : lines) {
        const QString line = rawLine.trimmed();
        ++lineNumber;
        if (line.isEmpty() || line.startsWith(QLatin1Char('#')) || line.startsWith(QStringLiteral("//"))) {
            continue;
        }
        if (line.contains(QStringLiteral("://")) == false) {
            error = QStringLiteral("Line %1 is not a supported share link").arg(lineNumber);
            return false;
        }

        XrayImportedProfile profile;
        QString uriError;
        if (importUri(line, profile, uriError) == false) {
            error = QStringLiteral("Line %1: %2").arg(lineNumber).arg(uriError);
            return false;
        }
        profiles.append(profile);
    }

    if (profiles.isEmpty()) {
        error = QStringLiteral("No usable links were found");
        return false;
    }

    for (int i = 0; i < profiles.size(); ++i) {
        QString name = profiles.at(i).name;
        int suffix = 2;
        bool duplicate = true;
        while (duplicate) {
            duplicate = false;
            for (int j = 0; j < i; ++j) {
                if (profiles.at(j).name == name) {
                    duplicate = true;
                    name = QStringLiteral("%1 (%2)").arg(profiles.at(i).name).arg(suffix++);
                    break;
                }
            }
        }
        profiles[i].name = name;
    }

    return true;
}

namespace {

int primaryProxyIndex(const QJsonArray& outbounds)
{
    for (int i = 0; i < outbounds.size(); ++i) {
        const QJsonObject outbound = outbounds.at(i).toObject();
        const QString protocol = outbound.value(QStringLiteral("protocol")).toString().toLower();
        if (protocol == QLatin1String("freedom") || protocol == QLatin1String("blackhole")
            || protocol == QLatin1String("dns")) {
            continue;
        }
        const QJsonObject settings = outbound.value(QStringLiteral("settings")).toObject();
        if (settings.contains(QStringLiteral("vnext")) || settings.contains(QStringLiteral("servers"))) {
            return i;
        }
    }
    return -1;
}

bool applyEndpointOverride(QJsonObject& outbound, const QString& server, int port, QString& error)
{
    QJsonObject settings = outbound.value(QStringLiteral("settings")).toObject();

    auto applyToServer = [&](QJsonObject& entry) {
        if (server.isEmpty() == false) {
            entry.insert(QStringLiteral("address"), server);
        }
        if (port > 0) {
            entry.insert(QStringLiteral("port"), port);
        }
    };

    if (settings.contains(QStringLiteral("vnext"))) {
        QJsonArray servers = settings.value(QStringLiteral("vnext")).toArray();
        if (servers.isEmpty()) {
            error = QStringLiteral("The profile has an empty vnext list");
            return false;
        }
        QJsonObject entry = servers.at(0).toObject();
        applyToServer(entry);
        servers.replace(0, entry);
        settings.insert(QStringLiteral("vnext"), servers);
    } else if (settings.contains(QStringLiteral("servers"))) {
        QJsonArray servers = settings.value(QStringLiteral("servers")).toArray();
        if (servers.isEmpty()) {
            error = QStringLiteral("The profile has an empty server list");
            return false;
        }
        QJsonObject entry = servers.at(0).toObject();
        applyToServer(entry);
        servers.replace(0, entry);
        settings.insert(QStringLiteral("servers"), servers);
    } else {
        error = QStringLiteral("This outbound does not support a server/port override");
        return false;
    }

    outbound.insert(QStringLiteral("settings"), settings);
    return true;
}

void applyFragment(QJsonObject& outbound, const QString& packets, const QString& length, const QString& delay)
{
    QJsonObject stream = outbound.value(QStringLiteral("streamSettings")).toObject();
    QJsonObject finalmask = stream.value(QStringLiteral("finalmask")).toObject();
    QJsonArray tcp = finalmask.value(QStringLiteral("tcp")).toArray();

    QJsonObject settings;
    settings.insert(QStringLiteral("packets"), packets);
    settings.insert(QStringLiteral("length"),
        length.isEmpty() ? QStringLiteral("100-200") : length);
    settings.insert(QStringLiteral("delay"),
        delay.isEmpty() ? QStringLiteral("10-20") : delay);

    QJsonObject mask;
    mask.insert(QStringLiteral("type"), QStringLiteral("fragment"));
    mask.insert(QStringLiteral("settings"), settings);
    tcp.append(mask);

    finalmask.insert(QStringLiteral("tcp"), tcp);
    stream.insert(QStringLiteral("finalmask"), finalmask);
    outbound.insert(QStringLiteral("streamSettings"), stream);
}

} // namespace

QJsonObject XrayConfig::buildRuntimeConfig(const QJsonObject& stored,
    const XrayRuntimeOptions& options,
    int proxyPort,
    int apiPort,
    QString& error)
{
    QJsonObject root = stored;
    if (root.value(QStringLiteral("outbounds")).isArray() == false
        || root.value(QStringLiteral("outbounds")).toArray().isEmpty()) {
        error = QStringLiteral("The profile has no outbounds");
        return {};
    }

    QJsonArray outbounds = root.value(QStringLiteral("outbounds")).toArray();
    for (int i = 0; i < outbounds.size(); ++i) {
        QJsonObject outbound = outbounds.at(i).toObject();
        normalizeOutbound(outbound);
        outbounds.replace(i, outbound);
    }

    const int proxyIndex = primaryProxyIndex(outbounds);
    if (options.serverOverride.isEmpty() == false || options.portOverride > 0) {
        if (proxyIndex < 0) {
            error = QStringLiteral("This profile has no outbound that supports a server/port override");
            return {};
        }
        QJsonObject outbound = outbounds.at(proxyIndex).toObject();
        if (applyEndpointOverride(outbound, options.serverOverride, options.portOverride, error) == false) {
            return {};
        }
        outbounds.replace(proxyIndex, outbound);
    }

    if (options.fragment && proxyIndex >= 0) {
        QJsonObject outbound = outbounds.at(proxyIndex).toObject();
        applyFragment(outbound, options.fragmentPackets, options.fragmentLength, options.fragmentDelay);
        outbounds.replace(proxyIndex, outbound);
    }

    root.insert(QStringLiteral("outbounds"), outbounds);

    QStringList dnsServers = options.dnsServers;
    dnsServers.removeAll(QString());
    if (dnsServers.isEmpty()) {
        dnsServers = defaultDnsServers();
    }
    QJsonArray dnsArray;
    for (const QString& server : dnsServers) {
        dnsArray.append(server);
    }

    QJsonObject log;
    log.insert(QStringLiteral("loglevel"), QStringLiteral("warning"));
    log.insert(QStringLiteral("access"), QStringLiteral("none"));
    root.insert(QStringLiteral("log"), log);

    root.insert(QStringLiteral("stats"), QJsonObject());
    QJsonObject api;
    api.insert(QStringLiteral("tag"), QStringLiteral("api"));
    api.insert(QStringLiteral("services"), QJsonArray{ QStringLiteral("StatsService") });
    root.insert(QStringLiteral("api"), api);

    QJsonObject policy;
    QJsonObject system;
    system.insert(QStringLiteral("statsInboundUplink"), true);
    system.insert(QStringLiteral("statsInboundDownlink"), true);
    system.insert(QStringLiteral("statsOutboundUplink"), true);
    system.insert(QStringLiteral("statsOutboundDownlink"), true);
    policy.insert(QStringLiteral("system"), system);
    root.insert(QStringLiteral("policy"), policy);

    QJsonObject apiInbound;
    apiInbound.insert(QStringLiteral("tag"), QStringLiteral("api"));
    apiInbound.insert(QStringLiteral("listen"), QStringLiteral("127.0.0.1"));
    apiInbound.insert(QStringLiteral("port"), apiPort);
    apiInbound.insert(QStringLiteral("protocol"), QStringLiteral("dokodemo-door"));
    QJsonObject apiSettings;
    apiSettings.insert(QStringLiteral("address"), QStringLiteral("127.0.0.1"));
    apiInbound.insert(QStringLiteral("settings"), apiSettings);

    QJsonObject proxyInbound;
    proxyInbound.insert(QStringLiteral("tag"), QStringLiteral("oc-in"));
    if (options.tunMode) {
        proxyInbound.insert(QStringLiteral("protocol"), QStringLiteral("tun"));
        QJsonObject settings;
        settings.insert(QStringLiteral("name"), QStringLiteral("oc-xray"));
        settings.insert(QStringLiteral("desc"), QStringLiteral("OpenConnect Xray"));
        settings.insert(QStringLiteral("mtu"), 1500);
        settings.insert(QStringLiteral("gateway"), QJsonArray{ QStringLiteral("198.18.0.1/16") });
        settings.insert(QStringLiteral("dns"), dnsArray);
        settings.insert(QStringLiteral("autoSystemRoutingTable"), QJsonArray{ QStringLiteral("0.0.0.0/0") });
        proxyInbound.insert(QStringLiteral("settings"), settings);
    } else {
        proxyInbound.insert(QStringLiteral("listen"), QStringLiteral("127.0.0.1"));
        proxyInbound.insert(QStringLiteral("port"), proxyPort);
        proxyInbound.insert(QStringLiteral("protocol"), QStringLiteral("mixed"));
        QJsonObject settings;
        settings.insert(QStringLiteral("udp"), true);
        proxyInbound.insert(QStringLiteral("settings"), settings);
    }

    if (options.sniffing) {
        QJsonObject sniffing;
        sniffing.insert(QStringLiteral("enabled"), true);
        sniffing.insert(QStringLiteral("destOverride"),
            QJsonArray{ QStringLiteral("http"), QStringLiteral("tls"), QStringLiteral("quic") });
        if (options.tunMode) {
            sniffing.insert(QStringLiteral("routeOnly"), true);
        }
        proxyInbound.insert(QStringLiteral("sniffing"), sniffing);
    }

    root.insert(QStringLiteral("inbounds"), QJsonArray{ apiInbound, proxyInbound });

    QJsonObject routing = root.value(QStringLiteral("routing")).toObject();
    QJsonArray rules = routing.value(QStringLiteral("rules")).toArray();
    QJsonObject apiRule;
    apiRule.insert(QStringLiteral("type"), QStringLiteral("field"));
    apiRule.insert(QStringLiteral("inboundTag"), QJsonArray{ QStringLiteral("api") });
    apiRule.insert(QStringLiteral("outboundTag"), QStringLiteral("api"));
    rules.prepend(apiRule);

    if (options.blockQuic) {
        QJsonArray routeOutbounds = root.value(QStringLiteral("outbounds")).toArray();
        QSet<QString> usedTags;
        QString blockTag;
        int blackholeIndex = -1;
        for (int i = 0; i < routeOutbounds.size(); ++i) {
            const QJsonObject outbound = routeOutbounds.at(i).toObject();
            const QString tag = outbound.value(QStringLiteral("tag")).toString();
            if (tag.isEmpty() == false) {
                usedTags.insert(tag);
            }
            if (blackholeIndex < 0
                && outbound.value(QStringLiteral("protocol")).toString() == QLatin1String("blackhole")) {
                blackholeIndex = i;
                blockTag = tag;
            }
        }

        QString candidate = QStringLiteral("oc-block");
        int suffix = 2;
        while (usedTags.contains(candidate)) {
            candidate = QStringLiteral("oc-block-%1").arg(suffix++);
        }

        if (blackholeIndex >= 0) {
            if (blockTag.isEmpty()) {
                QJsonObject outbound = routeOutbounds.at(blackholeIndex).toObject();
                outbound.insert(QStringLiteral("tag"), candidate);
                routeOutbounds.replace(blackholeIndex, outbound);
                blockTag = candidate;
            }
        } else {
            QJsonObject outbound;
            outbound.insert(QStringLiteral("tag"), candidate);
            outbound.insert(QStringLiteral("protocol"), QStringLiteral("blackhole"));
            routeOutbounds.append(outbound);
            blockTag = candidate;
        }

        root.insert(QStringLiteral("outbounds"), routeOutbounds);

        QJsonObject blockRule;
        blockRule.insert(QStringLiteral("type"), QStringLiteral("field"));
        blockRule.insert(QStringLiteral("network"), QStringLiteral("udp"));
        blockRule.insert(QStringLiteral("port"), 443);
        blockRule.insert(QStringLiteral("outboundTag"), blockTag);
        rules.insert(1, blockRule);
    }

    routing.insert(QStringLiteral("rules"), rules);
    root.insert(QStringLiteral("routing"), routing);

    return root;
}

QStringList XrayConfig::defaultDnsServers()
{
    return { QStringLiteral("1.1.1.1"), QStringLiteral("1.0.0.1") };
}

bool XrayConfig::primaryEndpoint(const QJsonObject& config, QString& host, int& port)
{
    const QJsonArray outbounds = config.value(QStringLiteral("outbounds")).toArray();
    for (const QJsonValue& value : outbounds) {
        const QJsonObject outbound = value.toObject();
        const QJsonObject settings = outbound.value(QStringLiteral("settings")).toObject();

        QJsonObject server;
        if (settings.contains(QStringLiteral("vnext"))) {
            const QJsonArray servers = settings.value(QStringLiteral("vnext")).toArray();
            if (servers.isEmpty() == false) {
                server = servers.at(0).toObject();
            }
        } else if (settings.contains(QStringLiteral("servers"))) {
            const QJsonArray servers = settings.value(QStringLiteral("servers")).toArray();
            if (servers.isEmpty() == false) {
                server = servers.at(0).toObject();
            }
        }

        if (server.isEmpty() == false) {
            host = server.value(QStringLiteral("address")).toString();
            port = server.value(QStringLiteral("port")).toInt();
            return host.isEmpty() == false && port > 0;
        }
    }
    return false;
}

QString XrayConfig::outboundSummary(const QJsonObject& config)
{
    const QJsonArray outbounds = config.value(QStringLiteral("outbounds")).toArray();
    if (outbounds.isEmpty()) {
        return QStringLiteral("No outbound");
    }

    const QJsonObject outbound = outbounds.at(0).toObject();
    const QString protocol = outbound.value(QStringLiteral("protocol")).toString();
    const QJsonObject settings = outbound.value(QStringLiteral("settings")).toObject();

    QString host;
    int port = 0;
    if (settings.contains(QStringLiteral("vnext"))) {
        const QJsonObject server = settings.value(QStringLiteral("vnext")).toArray().at(0).toObject();
        host = server.value(QStringLiteral("address")).toString();
        port = server.value(QStringLiteral("port")).toInt();
    } else if (settings.contains(QStringLiteral("servers"))) {
        const QJsonObject server = settings.value(QStringLiteral("servers")).toArray().at(0).toObject();
        host = server.value(QStringLiteral("address")).toString();
        port = server.value(QStringLiteral("port")).toInt();
    }

    if (host.isEmpty()) {
        return protocol.isEmpty() ? QStringLiteral("Custom config") : protocol;
    }
    return QStringLiteral("%1  %2:%3").arg(protocol, host).arg(port);
}

QString XrayConfig::prettyJson(const QString& compactJson)
{
    const QJsonDocument doc = QJsonDocument::fromJson(compactJson.toUtf8());
    if (doc.isNull()) {
        return compactJson;
    }
    return QString::fromUtf8(doc.toJson(QJsonDocument::Indented));
}

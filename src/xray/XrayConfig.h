#pragma once

#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QVector>

struct XrayImportedProfile {
    QString name;
    QString server;
    QString configJson;
};

struct XrayRuntimeOptions {
    bool tunMode = false;
    QStringList dnsServers;
    bool sniffing = false;
    bool blockQuic = true;
    bool fragment = false;
    QString fragmentPackets = QStringLiteral("tlshello");
    QString fragmentLength;
    QString fragmentDelay;
    QString serverOverride;
    int portOverride = 0;
};

class XrayConfig {
public:
    static bool looksLikeJson(const QString& text);
    static bool looksLikeUri(const QString& text);

    // Accepts a JSON config, one or more share links, or a base64 subscription.
    static bool importText(const QString& text,
        QVector<XrayImportedProfile>& profiles,
        QString& error);

    // Builds the config that is actually passed to xray.exe.
    static QJsonObject buildRuntimeConfig(const QJsonObject& stored,
        const XrayRuntimeOptions& options,
        int proxyPort,
        int apiPort,
        QString& error);

    static bool decodeSubscription(const QByteArray& data, QString& text, QString& error);

    static QStringList defaultDnsServers();
    static bool primaryEndpoint(const QJsonObject& config, QString& host, int& port);
    static QString outboundSummary(const QJsonObject& config);
    static QString prettyJson(const QString& compactJson);

private:
    static bool importJson(const QString& text, XrayImportedProfile& profile, QString& error);
    static bool importUri(const QString& uri, XrayImportedProfile& profile, QString& error);
};

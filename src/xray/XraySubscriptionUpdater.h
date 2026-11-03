#pragma once

#include <QStringList>
#include <QVector>

#include "XrayConfig.h"

struct XraySubscriptionSyncResult {
    bool ok = false;
    QString error;
    int updated = 0;
    int added = 0;
    QStringList stale;
};

class XraySubscriptionUpdater {
public:
    static QStringList subscriptionUrls();
    static XraySubscriptionSyncResult sync(const QString& url,
        const QVector<XrayImportedProfile>& nodes);
    static int removeProfiles(const QStringList& names);
};

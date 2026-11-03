#pragma once

#include <QMetaType>
#include <QString>

struct VpnProtocol {
    unsigned index;
    QString name;
    QString prettyName;
    QString description;
};

//Q_DECLARE_METATYPE(VpnProtocol)

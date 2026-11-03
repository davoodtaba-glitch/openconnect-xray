#pragma once

#include "VpnProtocol.h"

#include <QAbstractListModel>

#define ROLE_PROTOCOL_NAME (Qt::UserRole + 1)

class VpnProtocolModel : public QAbstractListModel {
    Q_OBJECT

public:
    explicit VpnProtocolModel(QObject* parent = nullptr);

    // Basic functionality:
    //    QHash<int, QByteArray> roleNames() const override;
    int rowCount(const QModelIndex& parent = QModelIndex()) const override;

    // Returns the index of the provided name or zero (default)
    unsigned findIndex(const QString name);

    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;

private:
    void loadProtocols();

    QList<VpnProtocol> m_protocols;
};

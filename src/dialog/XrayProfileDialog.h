#pragma once

#include <QDialog>
#include <QStringList>
#include <QVector>

#include "xray/XrayConfig.h"

class QNetworkAccessManager;
class QNetworkReply;

namespace Ui {
class XrayProfileDialog;
}

class XrayProfileDialog : public QDialog {
    Q_OBJECT

public:
    explicit XrayProfileDialog(QWidget* parent = nullptr);
    XrayProfileDialog(const QString& profileName, QWidget* parent);
    ~XrayProfileDialog();

    QStringList getSavedProfiles() const;
    QString getOriginalName() const;

protected:
    void changeEvent(QEvent* e) override;

private slots:
    void on_buttonBox_accepted();
    void on_buttonBox_rejected();

    void pasteClicked();
    void loadClicked();
    void fetchClicked();
    void fetchFinished();

private:
    void loadExisting(const QString& profileName);
    QString uniqueName(const QString& desired, const QString& exclude) const;
    void setStatus(const QString& text, bool error = false);
    static bool isSubscriptionUrl(const QString& text);

    Ui::XrayProfileDialog* ui;
    QNetworkAccessManager* m_network;
    QNetworkReply* m_reply = nullptr;
    QStringList m_savedNames;
    QString m_originalName;
    QString m_initialServer;
    int m_initialPort = 0;
    bool m_hadExplicitOverride = false;
    bool m_editing = false;
};

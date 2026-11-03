#pragma once

#include <QObject>
#include <QProcess>
#include <QString>
#include <QStringList>

struct XrayRuntimeOptions;

class QTimer;

class XrayClient : public QObject {
    Q_OBJECT

public:
    enum Status {
        Disconnecting = 0,
        Disconnected = 1,
        Connecting = 2,
        Connected = 3
    };

    explicit XrayClient(QObject* parent = nullptr);
    ~XrayClient();

    bool start(const QString& label, const QString& configJson, const XrayRuntimeOptions& options);
    void stop();
    bool isRunning() const;

    QString modeName() const;
    QString endpoint() const;

    void requestStats();

    static void restoreStaleSystemProxy();
    static void restoreStalePhysicalDns();
    static QString coreVersion();

signals:
    void statusChanged(int status);
    void connectedInfo(QString ip, QString dns, QString cipher);
    void statsChanged(quint64 upload, quint64 download, QString detail);
    void errorOccurred(QString message);

private slots:
    void readCoreOutput();
    void coreFinished(int exitCode, QProcess::ExitStatus status);
    void coreError(QProcess::ProcessError error);
    void checkReady();
    void statsFinished();

private:
    bool writeRuntimeConfig(const QString& configJson, const XrayRuntimeOptions& options, QString& error);
    void cleanup(bool restoreProxy);
    void attachToJobObject();
    void applyTunAdapterDns();
    void clearTunAdapterDns();
    bool capturePhysicalDns(QString& error);
    bool applyPhysicalDns(QString& error);
    void restorePhysicalDns();

    struct PhysicalDnsBackup {
        QString adapterName;
        int ifIndex = 0;
        bool valid = false;
        bool wasStatic = false;
        QStringList servers;
    };
    PhysicalDnsBackup m_physicalDns;
    bool m_physicalDnsApplied = false;

    QProcess* m_process = nullptr;
    QProcess* m_statsProcess = nullptr;
    QTimer* m_readyTimer = nullptr;
    QTimer* m_statsTimer = nullptr;
    QTimer* m_readyDeadline = nullptr;
    QString m_configPath;
    QString m_output;
    QString m_label;
    QString m_tunAdapterName;
    QStringList m_dnsServers;
    bool m_tunMode = false;
    bool m_ready = false;
    bool m_stopping = false;
    bool m_proxyApplied = false;
    int m_proxyPort = 0;
    int m_apiPort = 0;
    int m_readyChecks = 0;
#ifdef Q_OS_WIN
    void* m_job = nullptr;
#endif
};

#pragma once

#include <QtCore>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QMutex>

#include <exception>

#define RAVEN_HERE Raven::locationInfo(__FILE__, __FUNCTION__, __LINE__)

#define RAVEN_DEBUG Raven::Debug, RAVEN_HERE
#define RAVEN_INFO Raven::Info, RAVEN_HERE
#define RAVEN_WARNING Raven::Warning, RAVEN_HERE
#define RAVEN_ERROR Raven::Error, RAVEN_HERE
#define RAVEN_FATAL Raven::Fatal, RAVEN_HERE

class Raven;

using RavenTag = QPair<QString, QString>;

class RavenMessage {
    QString m_logger;
    QString m_platform;
    QString m_sdk;
    QString m_device;
    QJsonObject m_body;
    QJsonObject m_tags;

    friend class Raven;

public:
    RavenMessage& operator<<(const QString& message);
    RavenMessage& operator<<(const std::exception& exc);
    RavenMessage& operator<<(const RavenTag& tag);
};

class Raven : public QObject {
    Q_OBJECT

    bool m_initialized;

    QNetworkAccessManager* m_networkAccessManager;

    QString m_protocol;
    QString m_publicKey;
    QString m_secretKey;
    QString m_host;
    QString m_path;
    QString m_projectId;

    QJsonObject m_eventTemplate;
    QJsonObject m_tagsTemplate;

    QMap<QString, QByteArray> m_pendingRequest;
    QMutex m_pendingMutex;

    void parseDsn(const QString& dsn);

    void save(const QString& uuid, QByteArray& message);
    void send(QJsonObject& message);

private slots:
    void requestFinished(QNetworkReply* reply);
    void sslErrors(QNetworkReply* reply, const QList<QSslError>& errors);

public:
    enum RavenLevel { Fatal, Error, Warning, Info, Debug };
    Raven(const QString& dsn);
    ~Raven();
    RavenMessage operator()(RavenLevel level, QString culprit);

    static QString locationInfo(const char* file, const char* func, int line);

    bool isInitialized() const;

    void capture(const RavenMessage& message);
    void sendAllPending();

signals:
    void eventSent(const QString& uuid);
};

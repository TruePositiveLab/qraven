#include "raven.h"

#include <QHostInfo>
#include <QDebug>
#include <QUrl>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QSysInfo>

#include <dlfcn.h>
#include <execinfo.h>

#define RAVEN_CLIENT_NAME QString("QRaven")
#define RAVEN_CLIENT_VERSION QString("0.1")

const static QNetworkRequest::Attribute RavenUuidAttribute
    = static_cast<QNetworkRequest::Attribute>(QNetworkRequest::User + 19);

Raven::Raven(const QString& dsn, QObject* parent)
    : QObject(parent)
    , m_initialized(false)
    , m_networkAccessManager(new QNetworkAccessManager(this))
{
    m_eventTemplate["server_name"] = QHostInfo::localHostName();
    m_eventTemplate["logger"] = RAVEN_CLIENT_NAME;
    m_eventTemplate["platform"] = "c";
    m_eventTemplate["release"] = QCoreApplication::applicationVersion();
    m_tagsTemplate["os_type"] = QSysInfo::productType();
    m_tagsTemplate["os_version"] = QSysInfo::productVersion();
    parseDsn(dsn);
    connect(m_networkAccessManager, &QNetworkAccessManager::finished, this,
        &Raven::requestFinished);
    connect(m_networkAccessManager, &QNetworkAccessManager::sslErrors, this,
        &Raven::sslErrors);
    connect(
        this, &Raven::capture, this, &Raven::_capture, Qt::QueuedConnection);
    connect(this, &Raven::sendAllPending, this, &Raven::_sendAllPending,
        Qt::QueuedConnection);
}

Raven::~Raven()
{
    QEventLoop loop;
    connect(this, &Raven::eventSent, &loop, &QEventLoop::quit);
    while (!m_pendingRequest.isEmpty()) {
        loop.exec(QEventLoop::ExcludeUserInputEvents);
    }
}

void Raven::parseDsn(const QString& dsn)
{
    if (dsn.isEmpty()) {
        qWarning() << "DSN is empty, client disabled";
        return;
    }

    QUrl url(dsn);
    if (!url.isValid()) {
        qWarning() << "DSN is not valid, client disabled";
        return;
    }

    m_protocol = url.scheme();
    m_publicKey = url.userName();
    m_secretKey = url.password();
    m_host = url.host();
    m_path = url.path();
    m_projectId = url.fileName();

    int i = m_path.lastIndexOf('/');
    if (i >= 0)
        m_path = m_path.left(i);

    int port = url.port(80);
    if (port != 80)
        m_host.append(":").append(QString::number(port));

    qDebug() << "Raven client is ready";

    m_initialized = true;
}

QString levelString(Raven::RavenLevel level)
{
    switch (level) {
    case Raven::Fatal:
        return "fatal";
    case Raven::Error:
        return "error";
    case Raven::Warning:
        return "warning";
    case Raven::Info:
        return "info";
    case Raven::Debug:
        return "debug";
    }
    return "debug";
}

RavenMessage Raven::operator()(Raven::RavenLevel level, QString culprit)
{
    RavenMessage event;
    event.m_body = QJsonObject(m_eventTemplate);
    event.m_tags = QJsonObject(m_tagsTemplate);
    event.m_body["event_id"]
        = QUuid::createUuid().toString().remove('{').remove('}').remove('-');
    event.m_body["level"] = levelString(level);
    event.m_body["timestamp"]
        = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
    event.m_body["culprit"] = culprit;
    event.m_instance = this;
    return event;
}

Raven& Raven::operator<<(const RavenTag& tag)
{
    m_tagsTemplate[tag.first] = tag.second;
    return *this;
}

bool Raven::isInitialized() const { return m_initialized; }

static const QList<QString> _requiredAttributes
    = { "event_id", "message", "timestamp", "level", "logger", "platform",
        /* "sdk", "device" */ };

void Raven::_capture(const RavenMessage& message)
{
    if (!isInitialized())
        return;

    QJsonObject body = QJsonObject(message.m_body);
    body["tags"] = message.m_tags;

    for (const auto& attributeName : _requiredAttributes) {
        Q_ASSERT(body.contains(attributeName));
    }

    send(body);
}

void Raven::send(QJsonObject& message)
{
    QString clientInfo
        = QString("%1/%2").arg(RAVEN_CLIENT_NAME, RAVEN_CLIENT_VERSION);
    QString authInfo = QString("Sentry sentry_version=7,"
                               "sentry_client=%1,"
                               "sentry_timestamp=%2,"
                               "sentry_key=%3,"
                               "sentry_secret=%4")
                           .arg(clientInfo, QString::number(time(NULL)),
                               m_publicKey, m_secretKey);
    QString url = QString("%1://%2%3/api/%4/store/")
                      .arg(m_protocol, m_host, m_path, m_projectId);

    QString uuid = message["event_id"].toString();
    qDebug() << "url=" << url << ",uuid=" << uuid;
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    request.setHeader(QNetworkRequest::UserAgentHeader, clientInfo);
    request.setAttribute(RavenUuidAttribute, uuid);
    request.setRawHeader("X-Sentry-Auth", authInfo.toUtf8());

    QByteArray body = QJsonDocument(message).toJson(QJsonDocument::Indented);
    qDebug() << body;
    {
        QMutexLocker lk(&m_pendingMutex);
        m_pendingRequest[uuid] = body;
    }

    m_networkAccessManager->post(request, body);
}

void Raven::requestFinished(QNetworkReply* reply)
{
    QUrl redirectUrl
        = reply->attribute(QNetworkRequest::RedirectionTargetAttribute)
              .toUrl();
    QString uuid = reply->request().attribute(RavenUuidAttribute).toString();
    QByteArray body = m_pendingRequest[uuid];
    if (!redirectUrl.isEmpty() && redirectUrl != reply->url()) {
        QNetworkRequest request = reply->request();
        request.setUrl(redirectUrl);
        m_networkAccessManager->post(request, body);
    }
    else {
        if (reply->error() == QNetworkReply::NoError) {
            qDebug() << "Event sent " << reply->readAll();
        }
        else {
            qDebug() << "Failed to send message to sentry: " << reply->error();
            qDebug() << "Sentry answer: " << reply->readAll();
            save(uuid, body);
        }
        {
            QMutexLocker lk(&m_pendingMutex);
            m_pendingRequest.remove(uuid);
        }
        emit eventSent(uuid);
    }
    reply->deleteLater();
}

void Raven::sslErrors(QNetworkReply* reply, const QList<QSslError>& errors)
{
    Q_UNUSED(reply)
    Q_UNUSED(errors)
    reply->ignoreSslErrors();
}

void Raven::save(const QString& uuid, QByteArray& message)
{
    QString messageDir = QStandardPaths::writableLocation(
        QStandardPaths::AppLocalDataLocation);
    messageDir = QDir::cleanPath(messageDir + QDir::separator() + "messages");

    QString messageFile
        = QDir::cleanPath(messageDir + QDir::separator() + uuid);
    QFile file(messageFile);
    if (file.open(QIODevice::WriteOnly)) {
        file.write(message);
    }
    else {
        qWarning() << "Could not save message, it will be discarded";
    }
}

RavenMessage& Raven::send(RavenMessage& message)
{
    message.m_instance->capture(message);
    return message;
}

RavenTag Raven::tag(const QString& name, const QString& value)
{
    return RavenTag(name, value);
}

void Raven::_sendAllPending()
{
    QString messageDir = QStandardPaths::writableLocation(
        QStandardPaths::AppLocalDataLocation);
    messageDir = QDir::cleanPath(messageDir + QDir::separator() + "messages");
    QDirIterator it(messageDir, QDir::Files);
    while (it.hasNext()) {
        auto messageFile = it.next();
        QFile file(messageFile);
        if (file.open(QIODevice::ReadOnly)) {
            QByteArray body = file.readAll();
            QJsonParseError error;
            QJsonDocument doc = QJsonDocument::fromJson(body, &error);
            if (error.error == QJsonParseError::NoError) {
                QJsonObject object = doc.object();
                send(object);
            }
            else {
                qDebug() << "Could not parse message " << messageFile << ": "
                         << error.errorString();
            }
        }
        else {
            qDebug() << "Could not open file " << messageFile;
        }
    }
}

RavenMessage& RavenMessage::operator<<(const QString& message)
{
    m_body["message"] = message;
    return *this;
}
#include <cxxabi.h>

using namespace __cxxabiv1;

QString util_demangle(std::string to_demangle)
{
    int status = 0;
    char* buff
        = __cxxabiv1::__cxa_demangle(to_demangle.c_str(), NULL, NULL, &status);
    QString demangled(buff);
    free(buff);

    if (demangled.isEmpty()) {
        return QString::fromStdString(to_demangle);
    }
    return demangled;
}

RavenMessage& RavenMessage::operator<<(const std::exception& exc)
{
    m_body["message"] = exc.what();
    QJsonArray frameList;
    void* callstack[128];
    int i, frames = backtrace(callstack, 128);
    QString moduleName;
    for (i = 0; i < frames; ++i) {
        QJsonObject frame;
        Dl_info dlinfo;
        if (dladdr(callstack[i], &dlinfo) == 0)
            continue;
        if (i == 0) {
            moduleName = dlinfo.dli_fname;
            continue;
        }
        frame["function"] = util_demangle(dlinfo.dli_sname);
        frame["module"] = dlinfo.dli_fname;
        frame["in_app"] = false;
        frameList.push_back(frame);
    }
    QJsonObject frameHash;
    frameHash["frames"] = frameList;

    QJsonObject _exc;
    _exc["type"] = util_demangle(__cxa_current_exception_type()->name());
    _exc["stacktrace"] = frameHash;
    _exc["module"] = moduleName;
    _exc["value"] = exc.what();

    QJsonArray values;
    values.push_back(_exc);

    QJsonObject exceptionReport;
    exceptionReport["values"] = values;

    m_body["exception"] = values;
    return *this;
}

RavenMessage& RavenMessage::operator<<(const RavenTag& tag)
{
    m_tags[tag.first] = tag.second;
    return *this;
}

RavenMessage& RavenMessage::operator<<(RavenMessage& (*pf)(RavenMessage&))
{
    return (*pf)(*this);
}

QString Raven::locationInfo(const char* file, const char* func, int line)
{
    return QString("%1 in %2 at %3")
        .arg(file)
        .arg(func)
        .arg(QString::number(line));
}

/*
 *
 */

#include "imagestoreupdatesource.h"

#include "progressinterface.h"
#include "softwaremanagerconfig.h"
#include "softwaremanagerinterface.h"

#include <HemeraCore/Literals>
#include <HemeraCore/Operation>
#include <HemeraCore/Fingerprints>
#include <HemeraCore/CommonOperations>
#include <HemeraCore/NetworkDownloadOperation>
#include <HemeraSoftwareManagement/ApplianceManager>
#include <HemeraSoftwareManagement/SystemUpdate>

#include <private/HemeraCore/hemeraasyncinitobject_p.h>
#include <private/HemeraSoftwareManagement/hemerasoftwaremanagementconstructors_p.h>

#include <QtCore/QCryptographicHash>
#include <QtCore/QDir>
#include <QtCore/QDebug>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QSettings>
#include <QtCore/QTemporaryFile>
#include <QtCore/QUrl>
#include <QtCore/QUrlQuery>

#include <QtNetwork/QNetworkAccessManager>
#include <QtNetwork/QNetworkRequest>
#include <QtNetwork/QNetworkReply>

#define LATEST_IMAGE_ENDPOINT QStringLiteral("/images/%1/latest")
#define LATEST_UPDATE_ENDPOINT QStringLiteral("/updates/%1/latest")
//TODO: we shouldn't harcode this path here
#define ASTARTE_API_KEY_CONFIG_PATH "/var/lib/astarte/endpoint/CHANGE_DOMAIN_HERE/endpoint_crypto.conf"

class ImageStoreUpdateOperation : public Hemera::UrlOperation
{
    Q_OBJECT
    Q_DISABLE_COPY(ImageStoreUpdateOperation)

public:
    explicit ImageStoreUpdateOperation(const QString &filename, const QNetworkRequest &request, QNetworkAccessManager *nam,
                                       const QByteArray &checksum, QObject *parent = nullptr);
    explicit ImageStoreUpdateOperation(const QString &filename, QObject *parent = nullptr);
    virtual ~ImageStoreUpdateOperation();

public Q_SLOTS:
    virtual QUrl result() const override final;
    virtual void startImpl() override final;

private:
    QString m_filename;
    QNetworkRequest m_req;
    QNetworkAccessManager *m_nam;
    QByteArray m_checksum;

    QUrl m_url;
};

class CheckForImageStoreUpdatesOperation : public Hemera::Operation
{
    Q_OBJECT
    Q_DISABLE_COPY(CheckForImageStoreUpdatesOperation)

public:
    explicit CheckForImageStoreUpdatesOperation(Hemera::SoftwareManagement::SystemUpdate::UpdateType preferredUpdateType,
                                                ImageStoreUpdateSource *parent);
    virtual ~CheckForImageStoreUpdatesOperation();

public Q_SLOTS:
    virtual void startImpl() override final;

private:
    ImageStoreUpdateSource *m_parent;
    Hemera::SoftwareManagement::SystemUpdate::UpdateType m_updateType;
};

ImageStoreUpdateOperation::ImageStoreUpdateOperation(const QString &filename, const QNetworkRequest &request, QNetworkAccessManager *nam,
                                                     const QByteArray &checksum, QObject *parent)
    : Hemera::UrlOperation(parent)
    , m_filename(filename)
    , m_req(request)
    , m_nam(nam)
    , m_checksum(checksum)
{
}

ImageStoreUpdateOperation::ImageStoreUpdateOperation(const QString &filename, QObject *parent)
    : Hemera::UrlOperation(parent)
    , m_filename(filename)
{
    if (!m_filename.isEmpty()) {
        m_url = QUrl::fromLocalFile(m_filename);
    }
}

ImageStoreUpdateOperation::~ImageStoreUpdateOperation()
{
}

QUrl ImageStoreUpdateOperation::result() const
{
    return m_url;
}

void ImageStoreUpdateOperation::startImpl()
{
    // Succeed immediately?
    if (!m_url.isEmpty()) {
        setFinished();
        return;
    }

    // Should we fail?
    if (m_filename.isEmpty()) {
        setFinishedWithError(Hemera::Literals::literal(Hemera::Literals::Errors::badRequest()),
                             tr("The update source has invalid metadata. Did you check for available updates first?"));
        return;
    }

    // Create a new cache file
    QFile *file = new QFile(m_filename, this);

    if (!file->open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        // wtf
        setFinishedWithError(Hemera::SoftwareManagement::ApplianceManager::Errors::fileCreationError(), QString());
        return;
    }

    // Start the operation at the system level
    LocalDownloadOperation *op = ProgressInterface::instance()->startLocalDownloadOperation();

    Hemera::NetworkDownloadOperation *downloadOp = new Hemera::NetworkDownloadOperation(m_req, file, m_nam, m_checksum, this);
    connect(downloadOp, &Hemera::NetworkDownloadOperation::finished, this, [this, file, op] (Hemera::Operation *downOp) {
        op->setFinished();

        if (downOp->isError()) {
            setFinishedWithError(downOp->errorName(), downOp->errorMessage());
            return;
        }

        // All is good.
        m_url = QUrl::fromLocalFile(m_filename);

        setFinished();
    });

    connect(downloadOp, &Hemera::NetworkDownloadOperation::progressChanged, this, [this, downloadOp, op] (int percent) {
        op->setProgress(percent, downloadOp->rate());
    });
}

CheckForImageStoreUpdatesOperation::CheckForImageStoreUpdatesOperation(Hemera::SoftwareManagement::SystemUpdate::UpdateType preferredUpdateType,
                                                                       ImageStoreUpdateSource *parent)
    : Hemera::Operation(parent)
    , m_parent(parent)
    , m_updateType(preferredUpdateType)
{
}

CheckForImageStoreUpdatesOperation::~CheckForImageStoreUpdatesOperation()
{
}

void CheckForImageStoreUpdatesOperation::startImpl()
{
    // Cache current version information to begin with.
    QSettings applianceData(QStringLiteral("/etc/hemera/appliance_manifest"), QSettings::IniFormat);
    QString currentVersion = applianceData.value(QStringLiteral("APPLIANCE_VERSION")).toString();

    // Prepare requests
    QUrl incrementalRequestUrl = QUrl::fromUserInput(m_parent->m_endpointUrl.toString() + LATEST_UPDATE_ENDPOINT.arg(m_parent->m_applianceName));
    QUrlQuery incUrlQuery;
    incUrlQuery.addQueryItem(QStringLiteral("from_version"), currentVersion);
    incUrlQuery.addQueryItem(QStringLiteral("device_id"), QLatin1String(m_parent->m_hardwareId));
    incrementalRequestUrl.setQuery(incUrlQuery);

    QNetworkRequest incrementalRequest(incrementalRequestUrl);
    m_parent->setupRequestHeaders(&incrementalRequest);

    QUrl recoveryRequestUrl = QUrl::fromUserInput(m_parent->m_endpointUrl.toString() + LATEST_UPDATE_ENDPOINT.arg(m_parent->m_applianceName));
    QUrlQuery recoveryUrlQuery;
    recoveryUrlQuery.addQueryItem(QStringLiteral("device_id"), QLatin1String(m_parent->m_hardwareId));
    recoveryRequestUrl.setQuery(recoveryUrlQuery);

    QNetworkRequest recoveryRequest(recoveryRequestUrl);
    m_parent->setupRequestHeaders(&recoveryRequest);

    QNetworkReply *r;
    if (m_updateType == Hemera::SoftwareManagement::SystemUpdate::UpdateType::RecoveryUpdate) {
        r = m_parent->m_nam->sendCustomRequest(recoveryRequest, "OPTIONS");
    } else {
        r = m_parent->m_nam->sendCustomRequest(incrementalRequest, "OPTIONS");
    }

    connect(r, &QNetworkReply::finished, this, [this, r, recoveryRequest, currentVersion] {
        if (r->error() != QNetworkReply::NoError) {
            qDebug() << "Error in request" << r->error() << r->errorString();
            m_parent->setUpdate();

            switch (r->error()) {
                case QNetworkReply::ContentNotFoundError:
                    // Set finished correctly, but notify.
                    qWarning() << "No such update was found in store! This isn't supposed to happen!";
                    setFinished();
                    return;
                default:
                    // Set finished with error.
                    setFinishedWithError(Hemera::Literals::literal(Hemera::Literals::Errors::failedRequest()),
                                                                   tr("Image store returned %1: %2").arg(r->error()).arg(r->errorString()));
                    return;
            }
        }

        QJsonObject updateMetadata = QJsonDocument::fromJson(r->readAll()).object();
        qDebug() << "Got update metadata!!! " << updateMetadata;
        if (m_updateType == Hemera::SoftwareManagement::SystemUpdate::UpdateType::RecoveryUpdate) {
            Hemera::SoftwareManagement::SystemUpdate recoveryUpdate = Hemera::SoftwareManagement::Constructors::systemUpdateFromJson(updateMetadata);
            if (recoveryUpdate > currentVersion) {
                qDebug() << "Recovery update found!";
                m_parent->setUpdate(recoveryUpdate);
                m_parent->m_metadata = updateMetadata;
            }
        } else {
            Hemera::SoftwareManagement::SystemUpdate incrementalUpdate = Hemera::SoftwareManagement::Constructors::systemUpdateFromJson(updateMetadata);
            if (incrementalUpdate > currentVersion) {
                qDebug() << "Incremental update found!";
                m_parent->setUpdate(incrementalUpdate);
                m_parent->m_metadata = updateMetadata;
            }
        }

        setFinished();
    });
}

ImageStoreUpdateSource::ImageStoreUpdateSource(const QUrl &endpointUrl, const QString &apiKey, QObject *parent)
    : UpdateSource(parent)
    , m_endpointUrl(endpointUrl)
    , m_apiKey(apiKey)
{
}

ImageStoreUpdateSource::~ImageStoreUpdateSource()
{
}

void ImageStoreUpdateSource::initImpl()
{
    setParts(2);

    m_nam = new QNetworkAccessManager(this);
    connect(m_nam, &QNetworkAccessManager::sslErrors, this, [this] (QNetworkReply *r, const QList<QSslError> &errors) {
        qDebug() << "Errors";
        qDebug() << "SSL errors"  << errors;
    });

    // Fetch and cache information for our queries.
    QSettings applianceData(QStringLiteral("/etc/hemera/appliance_manifest"), QSettings::IniFormat);
    m_applianceName = applianceData.value(QStringLiteral("APPLIANCE_NAME")).toString();
    if (!applianceData.value(QStringLiteral("APPLIANCE_VARIANT")).toString().isEmpty()) {
        m_applianceName += QStringLiteral("_%1").arg(applianceData.value(QStringLiteral("APPLIANCE_VARIANT")).toString());
    }

    // Get our needed Fingerprints.
    Hemera::ByteArrayOperation *op = Hemera::Fingerprints::globalHardwareId();
    connect(op, &Hemera::Operation::finished, this, [this, op] {
        if (op->isError()) {
            setInitError(Hemera::Literals::literal(Hemera::Literals::Errors::failedRequest()), tr("Could not retrieve global hardware ID!"));
            return;
        }

        m_hardwareId = op->result();
        setOnePartIsReady();
    });

    setOnePartIsReady();
}

Hemera::Operation *ImageStoreUpdateSource::checkForUpdates(Hemera::SoftwareManagement::SystemUpdate::UpdateType preferredUpdateType)
{
    return new CheckForImageStoreUpdatesOperation(preferredUpdateType, this);
}

Hemera::UrlOperation *ImageStoreUpdateSource::downloadAvailableUpdate()
{
    // Cache current version information to begin with.
    QSettings applianceData(QStringLiteral("/etc/hemera/appliance_manifest"), QSettings::IniFormat);
    QString currentVersion = applianceData.value(QStringLiteral("APPLIANCE_VERSION")).toString();

    QUrl downloadUrl = QUrl::fromUserInput(m_endpointUrl.toString() + LATEST_UPDATE_ENDPOINT.arg(m_applianceName));
    QUrlQuery urlQuery;
    QString fileName;
    urlQuery.addQueryItem(QStringLiteral("device_id"), QLatin1String(m_hardwareId));

    if (m_metadata.value(QStringLiteral("artifact_type")).toString() == QStringLiteral("recovery")) {
        fileName = SoftwareManagerInterface::cacheEntryForUpdate(Hemera::SoftwareManagement::SystemUpdate::UpdateType::RecoveryUpdate,
                                                                 m_metadata.value(QStringLiteral("version")).toString());
    } else if (m_metadata.value(QStringLiteral("artifact_type")).toString() == QStringLiteral("update")) {
        urlQuery.addQueryItem(QStringLiteral("from_version"), currentVersion);
        fileName = SoftwareManagerInterface::cacheEntryForUpdate(Hemera::SoftwareManagement::SystemUpdate::UpdateType::IncrementalUpdate,
                                                                 m_metadata.value(QStringLiteral("version")).toString());
    } else {
        // Incompatible artifact. Abort.
        qWarning() << "Incompatible metadata!" << m_metadata;
        return new ImageStoreUpdateOperation(QString(), this);
    }

    // Does the file already exist?
    if (QFile::exists(fileName)) {
        qDebug() << "A potential cache entry already exists!";
        // Verify checksum!
        QCryptographicHash checksumHash(QCryptographicHash::Sha1);
        QFile oldEntry(fileName);
        oldEntry.open(QIODevice::ReadOnly);
        checksumHash.addData(&oldEntry);
        QByteArray checksum = checksumHash.result().toHex();
        if (checksum == m_metadata.value(QStringLiteral("checksum")).toString().toLatin1()) {
            qDebug() << "Checksum match! Skipping download and returning success.";
            return new ImageStoreUpdateOperation(fileName, this);
        } else {
            qDebug() << "Wrong checksum! Erasing cache entry and continuing.";
            oldEntry.remove();
        }
    }

    downloadUrl.setQuery(urlQuery);

    QNetworkRequest req(downloadUrl);
    setupRequestHeaders(&req);

    return new ImageStoreUpdateOperation(fileName, req, m_nam, m_metadata.value(QStringLiteral("checksum")).toString().toLatin1(), this);
}

void ImageStoreUpdateSource::setupRequestHeaders(QNetworkRequest *request)
{
    request->setRawHeader("Authorization", m_apiKey.toLatin1());
    request->setRawHeader("X-API-Key", astarteAPIKey());
    request->setRawHeader("X-Hardware-ID", m_hardwareId);
    request->setRawHeader("X-Image-Store-Plugin-Version", QStringLiteral("%1.%2.%3")
                                                     .arg(StaticConfig::softwareManagerPluginMajorVersion())
                                                     .arg(StaticConfig::softwareManagerPluginMinorVersion())
                                                     .arg(StaticConfig::softwareManagerPluginReleaseVersion())
                                                     .toLatin1());
}

QByteArray ImageStoreUpdateSource::astarteAPIKey()
{
    if (m_astarteAPIKey.isEmpty()) {
        QSettings settings(QStringLiteral(ASTARTE_API_KEY_CONFIG_PATH), QSettings::IniFormat);
        m_astarteAPIKey = settings.value(QStringLiteral("apiKey")).toString().toLatin1();
    }

    return m_astarteAPIKey;
}

#include "imagestoreupdatesource.moc"

/*
 *
 */

#include "softwaremanagerinterface.h"

#include "progressinterface.h"
#include "remoteupdateinterface.h"

#include <QtCore/QDateTime>
#include <QtCore/QDebug>
#include <QtCore/QDir>
#include <QtCore/QTemporaryDir>
#include <QtCore/QTimer>
#include <QtCore/QSettings>
#include <QtCore/QStorageInfo>

#include <QtConcurrent/QtConcurrentRun>

#include <QtDBus/QDBusConnection>
#include <QtDBus/QDBusConnectionInterface>
#include <QtDBus/QDBusMessage>
#include <QtDBus/QDBusReply>

#include <QtNetwork/QNetworkAccessManager>
#include <QtNetwork/QSslCertificate>
#include <QtNetwork/QSslKey>

#include <HemeraCore/ApplianceCrypto>
#include <HemeraCore/Literals>
#include <HemeraCore/CommonOperations>
#include <HemeraCore/SetSystemConfigOperation>

#include <HemeraSoftwareManagement/ApplianceManager>
#include <private/HemeraSoftwareManagement/hemerasoftwaremanagementconstructors_p.h>

#include <GravitySupermassive/GalaxyManager>
#include <GravitySupermassive/Operations>
#include <GravitySupermassive/StarSequence>

#include "imagestoreupdatesource.h"

#include "appliancemanageradaptor.h"

#include "incrementalupdateoperation.h"
#include "recoveryupdateoperation.h"

#include <softwaremanagerconfig.h>

// Set the call timeout to 15 minutes
constexpr int callTimeout() { return 1000 * 60 * 15; }

#define HANDLE_DBUS_REPLY(DMessage) \
QDBusMessage request;\
QDBusConnection requestConnection = QDBusConnection::systemBus();\
if (calledFromDBus()) {\
    request = message();\
    requestConnection = connection();\
    setDelayedReply(true);\
}\
QDBusPendingCall reply = QDBusConnection::systemBus().asyncCall(DMessage, callTimeout());\
QDBusPendingCallWatcher *watcher = new QDBusPendingCallWatcher(reply, this);\
\
connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, request, requestConnection] (QDBusPendingCallWatcher *call) {\
    QDBusPendingReply<void> reply = *call;\
    if (request.type() == QDBusMessage::MethodCallMessage) {\
        if (reply.isError()) {\
            requestConnection.send(request.createErrorReply(reply.error()));\
        } else {\
            requestConnection.send(request.createReply());\
        }\
    }\
    call->deleteLater();\
}, Qt::QueuedConnection);


#define SOFTWARE_MANAGER_VOLATILE_SETTINGS QSettings settings(QSettings::IniFormat, QSettings::SystemScope, QStringLiteral("Hemera"), QStringLiteral("SoftwareManager"))

// By default, 3 days
#define CHECK_UPDATES_EVERY_MSECS 3 * 24 * 60 * 60 * 1000

#define FILE_CHUNK 65536

SoftwareManagerInterface::SoftwareManagerInterface(Gravity::GalaxyManager* manager, QObject* parent)
    : AsyncInitDBusObject(parent)
    , m_manager(manager)
    , m_lastCheckForUpdates(0)
    , m_nam(new QNetworkAccessManager(this))
{
}

SoftwareManagerInterface::~SoftwareManagerInterface()
{
}

void SoftwareManagerInterface::initImpl()
{
    // First of all, let's register our DBus object.
    if (!QDBusConnection::systemBus().registerService(Hemera::Literals::literal(Hemera::Literals::DBus::softwareManagerService()))) {
        setInitError(Hemera::Literals::literal(Hemera::Literals::Errors::registerServiceFailed()),
                    QStringLiteral("Failed to register the service on the bus"));
        return;
    }
    if (!QDBusConnection::systemBus().registerService(Hemera::Literals::literal(Hemera::Literals::DBus::applianceManagerService()))) {
        setInitError(Hemera::Literals::literal(Hemera::Literals::Errors::registerServiceFailed()),
                    QStringLiteral("Failed to register the service on the bus"));
        return;
    }
    if (!QDBusConnection::systemBus().registerObject(Hemera::Literals::literal(Hemera::Literals::DBus::applianceManagerPath()), this)) {
        setInitError(Hemera::Literals::literal(Hemera::Literals::Errors::registerObjectFailed()),
                    QStringLiteral("Failed to register the object on the bus"));
        return;
    }

    // Time to gather cryptography!
    Hemera::ByteArrayOperation *deviceKeyOperation = Hemera::ApplianceCrypto::deviceKey();
    Hemera::SSLCertificateOperation *clientSSLCertificateOperation = Hemera::ApplianceCrypto::clientSSLCertificate();

    Hemera::CompositeOperation *op = new Hemera::CompositeOperation(QList< Hemera::Operation* >() << deviceKeyOperation << clientSSLCertificateOperation, this);
    connect(op, &Hemera::Operation::finished, this, [this, deviceKeyOperation, clientSSLCertificateOperation, op] {
        if (op->isError()) {
            qWarning() << "Could not initialise cryptography!! It's likely things won't work!";
        }

        QSettings updateConf(QStringLiteral("%1/update.conf").arg(StaticConfig::configGravityPath()), QSettings::IniFormat);

        // TODO: make it configurable
        m_remoteUpdate = new RemoteUpdateInterface(this);

        m_encryptionKey = deviceKeyOperation->result();
        if (updateConf.contains(QStringLiteral("updateOrbit"))) {
            m_updateOrbit = updateConf.value(QStringLiteral("updateOrbit")).toString();
        }

        // Astarte
        updateConf.beginGroup(QStringLiteral("Astarte")); {
            if (updateConf.value(QStringLiteral("enabled")).toBool()) {
                // TBD
            }
        } updateConf.endGroup();
        // Mass Storage
        updateConf.beginGroup(QStringLiteral("MassStorage")); {
            if (updateConf.value(QStringLiteral("enabled")).toBool()) {
                // TBD
            }
        } updateConf.endGroup();
        // Network Endpoint
        updateConf.beginGroup(QStringLiteral("ImageStore")); {
            if (updateConf.value(QStringLiteral("enabled")).toBool()) {
                UpdateSource *u = new ImageStoreUpdateSource(updateConf.value(QStringLiteral("endpoint")).toUrl(),
                                                             updateConf.value(QStringLiteral("apiKey")).toString(), this);
                m_updateSources.append(u);
                connect(u, &UpdateSource::updateChanged, this, &SoftwareManagerInterface::computeAvailableUpdate);
                u->init();
            }
        } updateConf.endGroup();

        setReady();
    });

    // Create the adaptor
    new ApplianceManagerAdaptor(this);
}

QByteArray SoftwareManagerInterface::systemUpdateAsByteArray() const
{
    return QJsonDocument(Hemera::SoftwareManagement::Constructors::toJson(m_systemUpdate.second)).toJson(QJsonDocument::Compact);
}

QString SoftwareManagerInterface::cacheEntryForUpdate(Hemera::SoftwareManagement::SystemUpdate update)
{
    return cacheEntryForUpdate(update.updateType(), update.applianceVersion());
}

QString SoftwareManagerInterface::cacheEntryForUpdate(Hemera::SoftwareManagement::SystemUpdate::UpdateType type, const QString &version)
{
    QString cacheEntry;
    if (type == Hemera::SoftwareManagement::SystemUpdate::UpdateType::IncrementalUpdate) {
        cacheEntry = QStringLiteral("%1%2/incremental_%2.hpd").arg(StaticConfig::softwareUpdateCacheDir(), version);
    } else {
        cacheEntry = QStringLiteral("%1%2/recovery_%2.hpd").arg(StaticConfig::softwareUpdateCacheDir(), version);
    }

    // Create the directory, if it does not exist
    QDir cacheDir(QStringLiteral("%1%2").arg(StaticConfig::softwareUpdateCacheDir(), version));
    if (!cacheDir.exists()) {
        cacheDir.mkpath(QStringLiteral("%1%2").arg(StaticConfig::softwareUpdateCacheDir(), version));
    }

    qDebug() << "Cache entry is" << cacheDir;

    return cacheEntry;
}

void SoftwareManagerInterface::cleanCache()
{
    qDebug() << "Cleaning cache";
    doCacheCleaning(QStringList{ m_systemUpdate.second.applianceVersion() });
}

void SoftwareManagerInterface::clearCache()
{
    qDebug() << "Clearing cache!";
    doCacheCleaning();
}

void SoftwareManagerInterface::doCacheCleaning(const QStringList &skipDirectories)
{
    QDir dir = QDir(StaticConfig::softwareUpdateCacheDir());
    dir.setFilter(QDir::Dirs | QDir::NoDotAndDotDot);
    for (const QString &entry : dir.entryList()) {
        if (skipDirectories.contains(entry)) {
            // Skip
            continue;
        }
        // Recursively remove and live happily.
        QDir(dir.filePath(entry)).removeRecursively();
    }
}

void SoftwareManagerInterface::checkForUpdates(quint16 preferredUpdateTypeAsInt)
{
    QDBusMessage request;
    QDBusConnection requestConnection = QDBusConnection::systemBus();
    if (calledFromDBus()) {
        request = message();
        requestConnection = connection();
        setDelayedReply(true);
    }

    connect(checkForUpdatesOperation(preferredUpdateTypeAsInt), &Hemera::Operation::finished, this, [this, requestConnection, request] (Hemera::Operation *op) {
        if (op->isError()) {
            requestConnection.send(request.createErrorReply(op->errorName(), op->errorMessage()));
        } else {
            requestConnection.send(request.createReply());
        }
    });
}

Hemera::Operation* SoftwareManagerInterface::checkForUpdatesOperation(quint16 preferredUpdateTypeAsInt)
{
    Hemera::SoftwareManagement::SystemUpdate::UpdateType preferredUpdateType =
                                        (Hemera::SoftwareManagement::SystemUpdate::UpdateType)preferredUpdateTypeAsInt;

    // First of all, let's check in.
    SOFTWARE_MANAGER_VOLATILE_SETTINGS;
    settings.beginGroup(QStringLiteral("status")); {
        settings.setValue(QStringLiteral("lastCheckForUpdates"), QDateTime::currentDateTime().toMSecsSinceEpoch());
    } settings.endGroup();

    QList< Hemera::Operation* > opList;
    for (UpdateSource *u : m_updateSources) {
        opList << u->checkForUpdates(preferredUpdateType);
    }

    Hemera::CompositeOperation *compOp = new Hemera::CompositeOperation(opList, this);
    connect(compOp, &Hemera::Operation::finished, this, [this, compOp] {
        // Do we have an update available?
        if (m_systemUpdate.second.isValid()) {
            // Clean the cache automatically.
            cleanCache();
        }
    });
    return compOp;
}

void SoftwareManagerInterface::computeAvailableUpdate()
{
    // We need to find out what kind of pattern we have.
    Hemera::SoftwareManagement::SystemUpdate systemUpdate;

    // Check if we have it.
    QSettings applianceData(QStringLiteral("/etc/hemera/appliance_manifest"), QSettings::IniFormat);
    QString applianceVersion = applianceData.value(QStringLiteral("APPLIANCE_VERSION")).toString();

    bool changed = false;
    bool atLeastOneAvailable = false;

    for (UpdateSource *u : m_updateSources) {
        if (u->updateMetadata() > applianceVersion && u->updateMetadata() > m_systemUpdate.second) {
            m_systemUpdate = qMakePair(u, u->updateMetadata());
            atLeastOneAvailable = true;
            changed = true;
        } else if (u->updateMetadata().isValid()) {
            atLeastOneAvailable = true;
        }
    }

    if (!atLeastOneAvailable && m_systemUpdate.second.isValid()) {
        // There are no more updates available.
        m_systemUpdate = qMakePair(nullptr, Hemera::SoftwareManagement::SystemUpdate());
        Q_EMIT systemUpdateChanged();
    } else if (changed) {
        Q_EMIT systemUpdateChanged();
        Q_EMIT systemUpdateAvailable();
    }
}

void SoftwareManagerInterface::downloadSystemUpdate()
{
    QDBusMessage request;
    QDBusConnection requestConnection = QDBusConnection::systemBus();
    if (calledFromDBus()) {
        request = message();
        requestConnection = connection();
        setDelayedReply(true);
    }

    connect(downloadSystemUpdateOperation(), &Hemera::Operation::finished, this, [this, request, requestConnection] (Hemera::Operation *op) {
        if (op->isError()) {
            requestConnection.send(request.createErrorReply(op->errorName(), op->errorMessage()));
        } else {
            requestConnection.send(request.createReply());
        }
    });
}

Hemera::Operation* SoftwareManagerInterface::downloadSystemUpdateOperation()
{
    UpdateSource *u = m_systemUpdate.first;
    if (!u) {
        // Utterly failed.
        return new Hemera::FailureOperation(Hemera::Literals::literal(Hemera::Literals::Errors::badRequest()),
                                            tr("No update currently cached. Did you check for updates first?"));
    }

    // Enough space on disk
    QStorageInfo cacheDisk(StaticConfig::softwareUpdateCacheDir());
    if (cacheDisk.isValid() && cacheDisk.isReady()) {
        if ((qint64)m_systemUpdate.second.downloadSize() >= cacheDisk.bytesFree()) {
            // Utterly failed.
            return new Hemera::FailureOperation(Hemera::SoftwareManagement::ApplianceManager::Errors::noSpaceLeftOnDisk(),
                                                tr("Update cache directory has only %1 bytes available for %2.")
                                                .arg(cacheDisk.bytesFree(), m_systemUpdate.second.downloadSize()));
        }
    } else {
        qWarning() << "Could not compute available disk space! Moving on regardless.";
    }

    return u->downloadAvailableUpdate();
}

void SoftwareManagerInterface::updateSystem()
{
    QDBusMessage request;
    QDBusConnection requestConnection = QDBusConnection::systemBus();
    if (calledFromDBus()) {
        request = message();
        requestConnection = connection();
        setDelayedReply(true);
    }

    connect(updateSystemOperation(), &Hemera::Operation::finished, this, [this, request, requestConnection] (Hemera::Operation *op) {
        if (op->isError()) {
            requestConnection.send(request.createErrorReply(op->errorName(), op->errorMessage()));
        } else {
            requestConnection.send(request.createReply());
        }
    });
}

Hemera::Operation *SoftwareManagerInterface::updateSystemOperation()
{
    switch (m_systemUpdate.second.updateType()) {
        case Hemera::SoftwareManagement::SystemUpdate::UpdateType::IncrementalUpdate:
            return new IncrementalUpdateOperation(m_manager, m_updateOrbit, m_systemUpdate.second, m_encryptionKey, this);
        case Hemera::SoftwareManagement::SystemUpdate::UpdateType::RecoveryUpdate:
            return new RecoveryUpdateOperation(m_systemUpdate.second, m_encryptionKey, this);
        default:
            return new Hemera::FailureOperation(Hemera::Literals::literal(Hemera::Literals::Errors::badRequest()),
                                                tr("No valid update found in cache."));
    }
}

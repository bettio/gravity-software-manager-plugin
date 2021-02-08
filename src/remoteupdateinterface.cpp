#include "remoteupdateinterface.h"
#include "remoteupdateconsumer.h"

#include <QtCore/QEventLoop>
#include <QtCore/QLoggingCategory>
#include <QtCore/QSettings>
#include <QtCore/QTimer>
#include <QtCore/QVersionNumber>

#include <HemeraCore/Operation>

Q_LOGGING_CATEGORY(LOG_REMOTEUPDATE, "RemoteUpdateInterface")

RemoteUpdateInterface::RemoteUpdateInterface(SoftwareManagerInterface *parent, Hemera::SoftwareManagement::SystemUpdate::UpdateType preferredUpdateType)
    : QObject(parent)
    , m_softwareManager(parent)
    , m_remoteUpdateConsumer(new RemoteUpdateConsumer(this))
    , m_preferredUpdateType(static_cast<quint16>(preferredUpdateType))
{
    connect(m_softwareManager, &SoftwareManagerInterface::systemUpdateAvailable, this, [this] { QTimer::singleShot(10000, this, &RemoteUpdateInterface::onSystemUpdateAvailable); });
}

RemoteUpdateInterface::~RemoteUpdateInterface()
{
    qCDebug(LOG_REMOTEUPDATE) << "Remote update interface activated";
}

void RemoteUpdateInterface::setTargetVersion(const QString &version)
{
    if (m_cachedTargetVersion == version) {
        // Nothing new here
        qCDebug(LOG_REMOTEUPDATE) << "Requested update to version " << version << " but this version was already set";
        return;
    }

    m_cachedTargetVersion = version;
    qCDebug(LOG_REMOTEUPDATE) << "Requested update to version " << version;

    if (isConfiguredVersionNewer()) {
        connect(m_softwareManager->checkForUpdatesOperation(m_preferredUpdateType), &Hemera::Operation::finished, this, [this] {
            qCDebug(LOG_REMOTEUPDATE) << "Updates checked";
            // Nothing to do here, systemUpdateAvailable will be emitted
        });
    }
}

void RemoteUpdateInterface::unsetTargetVersion()
{
    m_cachedTargetVersion = QString();
}

void RemoteUpdateInterface::onSystemUpdateAvailable()
{
    qCDebug(LOG_REMOTEUPDATE) << "Update available! Check if we need to do anything";

    if (isConfiguredVersionNewer()) {
        connect(m_softwareManager->downloadSystemUpdateOperation(), &Hemera::Operation::finished, this, [this] (Hemera::Operation *downloadOp) {
            if (!downloadOp->isError()) {
                QEventLoop e;
                QTimer::singleShot(10000, &e, &QEventLoop::quit);
                e.exec();

                qCDebug(LOG_REMOTEUPDATE) << "Download completed, starting system update";

                connect(m_softwareManager->updateSystemOperation(), &Hemera::Operation::finished, this, [this] (Hemera::Operation *updateOp) {
                    if (!updateOp->isError()) {
                        qCDebug(LOG_REMOTEUPDATE) << "System update completed";
                    } else {
                        qCWarning(LOG_REMOTEUPDATE) << "System update failed: " << updateOp->errorName() << updateOp->errorMessage();
                    }
                });
            } else {
                qCWarning(LOG_REMOTEUPDATE) << "Download failed: " << downloadOp->errorName() << downloadOp->errorMessage();
            }
        });
    }
}

bool RemoteUpdateInterface::isConfiguredVersionNewer()
{
    QSettings applianceData(QStringLiteral("/etc/hemera/appliance_manifest"), QSettings::IniFormat);
    QVersionNumber applianceVersion = QVersionNumber::fromString(applianceData.value(QStringLiteral("APPLIANCE_VERSION")).toString());

    QVersionNumber requiredVersion = QVersionNumber::fromString(m_cachedTargetVersion);

    if (requiredVersion > applianceVersion) {
        qCDebug(LOG_REMOTEUPDATE) << "Current version is lower (" << applianceVersion << ")";
        return true;

    } else {
        qCDebug(LOG_REMOTEUPDATE) << "Current version is higher (" << applianceVersion << "), aborting the update";
        return false;
    }
}

#include "incrementalupdateoperation.h"

#include "softwaremanagerinterface.h"

#include <QtCore/QDebug>
#include <QtCore/QFile>
#include <QtCore/QSettings>

#include <QtDBus/QDBusConnection>
#include <QtDBus/QDBusPendingCall>
#include <QtDBus/QDBusPendingCallWatcher>
#include <QtDBus/QDBusPendingReply>

#include <GravitySupermassive/GalaxyManager>
#include <GravitySupermassive/Operations>
#include <GravitySupermassive/StarSequence>

#include <HemeraCore/CommonOperations>

#include <HemeraSoftwareManagement/ApplianceManager>

constexpr int callTimeout() { return 1000 * 60 * 15; }

IncrementalUpdateOperation::IncrementalUpdateOperation(Gravity::GalaxyManager *manager, const QString &updateOrbit,
                                                       const Hemera::SoftwareManagement::SystemUpdate &systemUpdate,
                                                       const QByteArray &encryptionKey, QObject *parent)
    : UpdateOperation(systemUpdate, encryptionKey, parent)
    , m_manager(manager)
    , m_updateOrbit(updateOrbit)
{
}

IncrementalUpdateOperation::~IncrementalUpdateOperation()
{
}

void IncrementalUpdateOperation::startImpl()
{
    // Let's get started. Find out our cache entry.
    QSettings applianceData(QStringLiteral("/etc/hemera/appliance_manifest"), QSettings::IniFormat);
    QString applianceVersion = applianceData.value(QStringLiteral("APPLIANCE_VERSION")).toString();
    QString cacheEntry = SoftwareManagerInterface::cacheEntryForUpdate(m_systemUpdate);
    if (!QFile::exists(cacheEntry)) {
        // Needs to be downloaded first.
        qDebug() << "Cache entry does not exist" << cacheEntry;
        setFinishedWithError(Hemera::SoftwareManagement::ApplianceManager::Errors::downloadError(),
                             QString());
        return;
    }

    // Prepare squash package.
    QString packagePath = prepareSquashPackage(cacheEntry);
    if (packagePath.isEmpty()) {
        // Utterly failed.
        qDebug() << "Could not prepare squash package" << cacheEntry;
        setFinishedWithError(Hemera::SoftwareManagement::ApplianceManager::Errors::invalidPackage(),
                             QString());
        return;
    }

    // Time to remount!
    Hemera::Operation *mountOp = new Gravity::ControlUnitOperation(QStringLiteral("gravity-remount-helper.service"), QString(),
                                                                   Gravity::ControlUnitOperation::Mode::StartMode, nullptr, this);
    connect(mountOp, &Hemera::Operation::finished, this, [this, mountOp, packagePath] {
        if (mountOp->isError()) {
            // Utterly failed.
            unmountPackage(packagePath);
            setFinishedWithError(Hemera::SoftwareManagement::ApplianceManager::Errors::remountError(),
                                 QString());
            return;
        }

        if (!m_updateOrbit.isEmpty()) {
            // We have to inject the orbit everywhere. First of all, let's send an OK message to our caller.
            setFinished();

            // Find stars
            QList< Hemera::Operation* > injections;
            for (Gravity::StarSequence *ss : m_manager->stars()) {
                Hemera::Operation *op = ss->injectOrbit(m_updateOrbit);
                if (op) {
                    injections << op;
                }
            }
            Hemera::CompositeOperation *compositeOp = new Hemera::CompositeOperation(injections, this);
            connect(compositeOp, &Hemera::Operation::finished, this, [this, packagePath] {
                performIncrementalPackageUpdate(packagePath);
            });
            return;
        }

        performIncrementalPackageUpdate(packagePath);
    });
}

void IncrementalUpdateOperation::performIncrementalPackageUpdate(const QString &packagePath)
{
    QDBusMessage updateSystemCall = createBackendCall(QStringLiteral("updateSystem"), QVariantList() << packagePath);
    QDBusPendingCall reply = QDBusConnection::systemBus().asyncCall(updateSystemCall, callTimeout());
    QDBusPendingCallWatcher *watcher = new QDBusPendingCallWatcher(reply, this);

    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, packagePath] (QDBusPendingCallWatcher *call) {
        QDBusPendingReply<void> reply = *call;
        if (!reply.isError()) {
            // Success! We need to update the appliance manifest before we remount.
            QSettings applianceData(QStringLiteral("/etc/hemera/appliance_manifest"), QSettings::IniFormat);

            applianceData.setValue(QStringLiteral("APPLIANCE_VERSION"), m_systemUpdate.applianceVersion());
        }

        // Remount filesystem
        new Gravity::ControlUnitOperation(QStringLiteral("gravity-remount-helper.service"), QString(),
                                            Gravity::ControlUnitOperation::Mode::StopMode, nullptr, this);
        // Unmount package
        unmountPackage(packagePath);

        call->deleteLater();
        if (!m_updateOrbit.isEmpty()) {
            // Deinject orbits
            for (Gravity::StarSequence *ss : m_manager->stars()) {
                ss->deinjectOrbit();
            }
        } else {
            // Send reply, and let the orbit decide what to do.
            if (reply.isError()) {
                setFinishedWithError(reply.error());
            } else {
                setFinished();
            }
        }
    }, Qt::QueuedConnection);
}

#include "recoveryupdateoperation.h"

#include "progressinterface.h"
#include "softwaremanagerinterface.h"

#include <QtCore/QDebug>
#include <QtCore/QDir>
#include <QtCore/QFuture>
#include <QtCore/QFutureWatcher>
#include <QtCore/QProcess>
#include <QtCore/QSettings>
#include <QtCore/QTimer>

#include <QtConcurrent/QtConcurrentRun>

#include <HemeraCore/Literals>
#include <HemeraCore/SetSystemConfigOperation>

#include <HemeraSoftwareManagement/ApplianceManager>

#define FILE_CHUNK 65536

RecoveryUpdateOperation::RecoveryUpdateOperation(const Hemera::SoftwareManagement::SystemUpdate &systemUpdate,
                                                 const QByteArray &encryptionKey, QObject *parent)
    : UpdateOperation(systemUpdate, encryptionKey, parent)
{
}

RecoveryUpdateOperation::~RecoveryUpdateOperation()
{
}

void RecoveryUpdateOperation::startImpl()
{
    // Let's get started. Find out our cache entry.
    QSettings applianceData(QStringLiteral("/etc/hemera/appliance_manifest"), QSettings::IniFormat);
    QString applianceVersion = applianceData.value(QStringLiteral("APPLIANCE_VERSION")).toString();
    QString cacheEntry = SoftwareManagerInterface::cacheEntryForUpdate(m_systemUpdate);

    // Let's mount /recovery
    QProcess *mountRecovery = new QProcess(this);
    mountRecovery->start(QStringLiteral("/bin/mount"), QStringList{ QStringLiteral("/recovery") });
    mountRecovery->waitForFinished();
    if (mountRecovery->exitCode() != 0) {
        setFinishedWithError(Hemera::Literals::literal(Hemera::Literals::Errors::failedRequest()),
                             tr("Could not mount recovery partition."));
        mountRecovery->deleteLater();
        return;
    }
    mountRecovery->deleteLater();

    // Prepare recovery umount
    QProcess *umountRecovery = new QProcess(this);
    umountRecovery->setProgram(QStringLiteral("/bin/umount"));
    umountRecovery->setArguments(QStringList{ QStringLiteral("/recovery") });

    connect(umountRecovery, static_cast<void (QProcess::*)(int, QProcess::ExitStatus)>(&QProcess::finished), umountRecovery, &QObject::deleteLater);

    // OK now, let's erase the recovery partition
    QDir recoveryDir(QStringLiteral("/recovery"));
    recoveryDir.setFilter(QDir::Files | QDir::Hidden);
    for (const QFileInfo &fileInfo : recoveryDir.entryInfoList()) {
        QFile::remove(fileInfo.absoluteFilePath());
    }

    // Prepare squash package.
    QString packagePath = prepareSquashPackage(cacheEntry);
    if (packagePath.isEmpty()) {
        // Utterly failed.
        qDebug() << "Could not prepare squash package" << cacheEntry;
        umountRecovery->start();
        setFinishedWithError(Hemera::SoftwareManagement::ApplianceManager::Errors::invalidPackage(),
                                                        QString());
        return;
    }

    // And let's extract the archive concurrently.
    LocalDownloadOperation *downloadOp = ProgressInterface::instance()->startLocalDownloadOperation();
    QFuture< bool > extraction = QtConcurrent::run([this, packagePath, downloadOp] () -> bool {
        QDir packageDir(packagePath);
        packageDir.setFilter(QDir::NoDotAndDotDot | QDir::Files);

        // Cache total size
        qint64 totalCopySize = 0;
        qint64 totalCopied = 0;
        for (const QFileInfo &info : packageDir.entryInfoList()) {
            totalCopySize += info.size();
        }

        // Now, for the actual copy
        for (const QFileInfo &info : packageDir.entryInfoList()) {
            QFile file(info.absoluteFilePath());
            if (!file.open(QIODevice::ReadOnly)) {
                qWarning() << "Could not open file!" << info.absoluteFilePath();
                return false;
            }
            QFile destFile(QStringLiteral("/recovery/%1").arg(info.fileName()));
            if (!destFile.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
                qWarning() << "Could not open destination file!" << info.fileName();
                return false;
            }

            // Copy in a loop, and report progress.
            while (!file.atEnd()) {
                totalCopied += file.bytesAvailable() >= FILE_CHUNK ? FILE_CHUNK : file.bytesAvailable();
                destFile.write(file.read(FILE_CHUNK));
                QMetaObject::invokeMethod(downloadOp, "setProgress", Qt::AutoConnection,
                                          Q_ARG(int, (totalCopied*100)/totalCopySize), Q_ARG(int, -1));
            }

            destFile.flush();
        }

        return true;
    });

    QFutureWatcher< bool > *extractionWatcher = new QFutureWatcher< bool >(this);
    extractionWatcher->setFuture(extraction);
    connect(extractionWatcher, &QFutureWatcher< bool >::finished, this, [this, umountRecovery,
                                                                         extractionWatcher, packagePath, downloadOp] {
        unmountPackage(packagePath);
        QTimer::singleShot(0, downloadOp, &LocalDownloadOperation::setFinished);
        // Unmount recovery regardless, at this stage.
        umountRecovery->start();

        if (!extractionWatcher->result()) {
            setFinishedWithError(Hemera::SoftwareManagement::ApplianceManager::Errors::extractionError(),
                                 QString());
            return;
        }

        qDebug() << "Recovery update extracted successfully";

        Hemera::SetSystemConfigOperation *recoveryOp = new Hemera::SetSystemConfigOperation(QStringLiteral("hemera_recovery_boot"),
                                                                                            QString::number(1), this);
        connect(recoveryOp, &Hemera::Operation::finished, this, [this, recoveryOp] {
            if (recoveryOp->isError()) {
                setFinishedWithError(Hemera::SoftwareManagement::ApplianceManager::Errors::recoveryNotSet(),
                                     QString());
                return;
            }

            setFinished();
        });
    });
}

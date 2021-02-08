#include "updateoperation.h"

#include "softwaremanagerinterface.h"

#include <QtCore/QDebug>
#include <QtCore/QDir>
#include <QtCore/QProcess>

#include <QtDBus/QDBusMessage>

#include <softwaremanagerconfig.h>

UpdateOperation::UpdateOperation(const Hemera::SoftwareManagement::SystemUpdate &systemUpdate, const QByteArray &encryptionKey, QObject *parent)
    : Hemera::Operation(parent)
    , m_systemUpdate(systemUpdate)
    , m_encryptionKey(encryptionKey)
{
}

UpdateOperation::~UpdateOperation()
{
}

QString UpdateOperation::prepareSquashPackage(const QString &packagePath)
{
    QDir dir(QStringLiteral("%1repository").arg(StaticConfig::softwareUpdateCacheDir()));
    if (!dir.exists()) {
        dir.mkpath(QStringLiteral("%1repository").arg(StaticConfig::softwareUpdateCacheDir()));
    }

    QString mountPath = dir.absolutePath();
    QStringList arguments { packagePath, mountPath };
    if (!m_encryptionKey.isEmpty()) {
        arguments.append(QLatin1String(m_encryptionKey));
    }

    QProcess *mountProcess = new QProcess(this);
    mountProcess->setProgram(QStringLiteral("%1/mount-squash-package").arg(StaticConfig::binInstallDir()));
    mountProcess->setArguments(arguments);
    mountProcess->start();

    mountProcess->waitForFinished();
    if (mountProcess->exitCode() != 0) {
        qDebug() << "Mount process failed!!!" << mountProcess->readAll() << mountProcess->exitCode();
        // FIXME: Handle errors!
        mountPath = QString();
    }

    mountProcess->deleteLater();
    return mountPath;
}

bool UpdateOperation::unmountPackage(const QString &packagePath)
{
    QStringList arguments { packagePath };
    if (!m_encryptionKey.isEmpty()) {
        arguments.append(QLatin1String(m_encryptionKey));
    }

    QProcess *umountProcess = new QProcess(this);
    umountProcess->setProgram(QStringLiteral("%1/umount-squash-package").arg(StaticConfig::binInstallDir()));
    umountProcess->setArguments(arguments);
    umountProcess->start();

    umountProcess->waitForFinished();
    if (umountProcess->exitCode() != 0) {
        umountProcess->deleteLater();
        return false;
    }

    umountProcess->deleteLater();
    return true;
}

QDBusMessage UpdateOperation::createBackendCall(const QString& method, const QVariantList& args)
{
    // Defines are taken from softwaremanagerinterface.h
    QDBusMessage call = QDBusMessage::createMethodCall(BACKEND_SERVICE, BACKEND_PATH, BACKEND_INTERFACE, method);
    call.setArguments(args);
    return call;
}

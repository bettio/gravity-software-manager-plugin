#ifndef REMOTE_UPDATE_INTERFACE_H
#define REMOTE_UPDATE_INTERFACE_H

#include <QtCore/QObject>

#include "softwaremanagerinterface.h"

class RemoteUpdateConsumer;

class RemoteUpdateInterface : public QObject
{
    Q_OBJECT
    Q_DISABLE_COPY(RemoteUpdateInterface)

public:
    explicit RemoteUpdateInterface(SoftwareManagerInterface *parent,
                                   Hemera::SoftwareManagement::SystemUpdate::UpdateType preferredUpdateType = Hemera::SoftwareManagement::SystemUpdate::UpdateType::RecoveryUpdate);
    virtual ~RemoteUpdateInterface();

    void setTargetVersion(const QString &value);
    void unsetTargetVersion();

private Q_SLOTS:
    void onSystemUpdateAvailable();

private:
    bool isConfiguredVersionNewer();

    SoftwareManagerInterface *m_softwareManager;
    RemoteUpdateConsumer *m_remoteUpdateConsumer;
    QString m_cachedTargetVersion;
    quint16 m_preferredUpdateType;
};

#endif // REMOTE_UPDATE_INTERFACE_H

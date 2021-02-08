/*
 *
 */

#ifndef SOFTWAREMANAGERINTERFACE_H
#define SOFTWAREMANAGERINTERFACE_H

#include <HemeraCore/AsyncInitDBusObject>

#include <HemeraSoftwareManagement/SystemUpdate>

class ProgressInterface;
class RemoteUpdateInterface;
class QTimer;
class QDBusMessage;
class QNetworkAccessManager;
class UpdateSource;
class OrgFreedesktopSystemd1ManagerInterface;
namespace Gravity {
class GalaxyManager;
}

#define BACKEND_SERVICE QStringLiteral("com.ispirata.Hemera.SoftwareManager.Backend")
#define BACKEND_INTERFACE QStringLiteral("com.ispirata.Hemera.SoftwareManager.Backend")
#define BACKEND_PATH QStringLiteral("/com/ispirata/Hemera/SoftwareManager/Backend")

class SoftwareManagerInterface : public Hemera::AsyncInitDBusObject
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "com.ispirata.Hemera.SoftwareManager")

    Q_PROPERTY(QByteArray systemUpdate READ systemUpdateAsByteArray NOTIFY systemUpdateChanged)
    Q_PROPERTY(qint64 lastCheckForUpdates MEMBER m_lastCheckForUpdates NOTIFY lastCheckForUpdatesChanged)

public:
    explicit SoftwareManagerInterface(Gravity::GalaxyManager *manager, QObject *parent = nullptr);
    virtual ~SoftwareManagerInterface();

public Q_SLOTS:
    void checkForUpdates(quint16 preferredImageType);
    Hemera::Operation* checkForUpdatesOperation(quint16 preferredImageType);

    void downloadSystemUpdate();
    Hemera::Operation* downloadSystemUpdateOperation();
    void updateSystem();
    Hemera::Operation* updateSystemOperation();

    void cleanCache();
    void clearCache();

    QByteArray systemUpdateAsByteArray() const;

    static QString cacheEntryForUpdate(Hemera::SoftwareManagement::SystemUpdate update);
    static QString cacheEntryForUpdate(Hemera::SoftwareManagement::SystemUpdate::UpdateType type, const QString &version);

protected:
    virtual void initImpl() override final;

Q_SIGNALS:
    void systemUpdateChanged();
    void lastCheckForUpdatesChanged(qint64 lastCheckForUpdates);
    void systemUpdateAvailable();

private Q_SLOTS:
    void computeAvailableUpdate();

private:
    void doCacheCleaning(const QStringList &skipDirectories = QStringList());

    Gravity::GalaxyManager *m_manager;
    QList<UpdateSource*> m_updateSources;
    QPair< UpdateSource*, Hemera::SoftwareManagement::SystemUpdate > m_systemUpdate;
    qint64 m_lastCheckForUpdates;
    QNetworkAccessManager *m_nam;
    QByteArray m_encryptionKey;
    QString m_updateOrbit;
    RemoteUpdateInterface *m_remoteUpdate;

    OrgFreedesktopSystemd1ManagerInterface *m_systemdManager;

    friend class ProgressInterface;
};

#endif // SOFTWAREMANAGERINTERFACE_H

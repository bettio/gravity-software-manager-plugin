/*
 *
 */

#ifndef APPSTOREINTERFACE_H
#define APPSTOREINTERFACE_H

#include <HemeraCore/AsyncInitDBusObject>

class ProgressInterface;
class QTimer;
class QDBusMessage;
namespace Gravity {
class GalaxyManager;
}

class ApplicationManagerInterface : public Hemera::AsyncInitDBusObject
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "com.ispirata.Hemera.SoftwareManagement.ApplicationManager")

    Q_PROPERTY(QByteArray applicationUpdates MEMBER m_applicationUpdates NOTIFY applicationUpdatesChanged)
    Q_PROPERTY(QByteArray installedApplications MEMBER m_installedApplications NOTIFY installedApplicationsChanged)
    Q_PROPERTY(qint64 lastCheckForApplicationUpdates MEMBER m_lastCheckForUpdates NOTIFY lastCheckForApplicationUpdatesChanged)

public:
    explicit ApplicationManagerInterface(Gravity::GalaxyManager *manager, QObject *parent = nullptr);
    virtual ~ApplicationManagerInterface();

public Q_SLOTS:
    void checkForApplicationUpdates();

    void addRepository(const QString &name, const QStringList &urls);
    void removeRepository(const QString &name);

    void installApplications(const QByteArray &applications);
    void removeApplications(const QByteArray &applications);

    void downloadApplicationUpdates(const QByteArray &updates);
    void updateApplications(const QByteArray &updates);

    void refreshUpdateList();
    void refreshInstalledApplicationsList();

protected:
    virtual void initImpl() override final;

Q_SIGNALS:
    void applicationUpdatesChanged(const QByteArray &applicationUpdates);
    void lastCheckForApplicationUpdatesChanged(qint64 lastCheckForUpdates);
    void installedApplicationsChanged(const QByteArray &installedApplications);

private Q_SLOTS:
    void onReportProgress(uint actionType, uint progress, uint rate);
    void restartAutoCheckTimer();

private:
    QDBusMessage createBackendCall(const QString &method, const QVariantList &args = QVariantList());
    void setProgressSubscriptionState(bool subscribed);

    Gravity::GalaxyManager *m_manager;
    QByteArray m_applicationUpdates;
    QByteArray m_systemUpdate;
    QByteArray m_installedApplications;
    qint64 m_lastCheckForUpdates;
    QTimer *m_autoCheckTimer;
    bool m_subscribedToProgress;

    friend class ProgressInterface;
};

#endif // APPSTOREINTERFACE_H

/*
 *
 */

#include "applicationmanagerinterface.h"

#include "softwaremanagerinterface.h"
#include "progressinterface.h"

#include <QtCore/QDateTime>
#include <QtCore/QDebug>
#include <QtCore/QTimer>
#include <QtCore/QSettings>

#include <QtDBus/QDBusConnection>
#include <QtDBus/QDBusConnectionInterface>
#include <QtDBus/QDBusMessage>
#include <QtDBus/QDBusReply>

#include <HemeraCore/Literals>
#include <HemeraCore/Operation>

#include <GravitySupermassive/GalaxyManager>

#include "applicationmanageradaptor.h"

#define HANDLE_DBUS_REPLY(DMessage) \
QDBusMessage request;\
QDBusConnection requestConnection = QDBusConnection::systemBus();\
if (calledFromDBus()) {\
    request = message();\
    requestConnection = connection();\
    setDelayedReply(true);\
}\
QDBusPendingCall reply = QDBusConnection::systemBus().asyncCall(DMessage);\
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


#define SOFTWARE_MANAGER_VOLATILE_SETTINGS QSettings settings(QSettings::IniFormat, QSettings::SystemScope, QStringLiteral("Hemera"), QStringLiteral("AppStore"))

#define PROGRESS_SUBSCRIPTION_METHOD QStringLiteral("setSubscribedToProgress")

// By default, 3 days
#define CHECK_UPDATES_EVERY_MSECS 3 * 24 * 60 * 60 * 1000

ApplicationManagerInterface::ApplicationManagerInterface(Gravity::GalaxyManager* manager, QObject* parent)
    : AsyncInitDBusObject(parent)
    , m_manager(manager)
    , m_lastCheckForUpdates(0)
    , m_subscribedToProgress(false)
{
}

ApplicationManagerInterface::~ApplicationManagerInterface()
{
}

void ApplicationManagerInterface::initImpl()
{
    // First of all, let's register our DBus object.
    if (!QDBusConnection::systemBus().registerService(Hemera::Literals::literal(Hemera::Literals::DBus::applicationManagerService()))) {
        setInitError(Hemera::Literals::literal(Hemera::Literals::Errors::registerServiceFailed()),
                    QStringLiteral("Failed to register the service on the bus"));
        return;
    }
    if (!QDBusConnection::systemBus().registerObject(Hemera::Literals::literal(Hemera::Literals::DBus::applicationManagerPath()), this)) {
        setInitError(Hemera::Literals::literal(Hemera::Literals::Errors::registerObjectFailed()),
                    QStringLiteral("Failed to register the object on the bus"));
        return;
    }

    // Create the adaptor
    new ApplicationManagerAdaptor(this);

    // Monitor DBus signals coming from the backend's report progress. For how DBus works, we are just "monitoring" more than
    // connecting, so this connection will be valid even if the object goes up and down.
    if (!QDBusConnection::systemBus().connect(BACKEND_SERVICE, BACKEND_PATH, BACKEND_INTERFACE,
                                              QStringLiteral("reportProgress"), this, SLOT(onReportProgress(uint,uint,uint)))) {
        qWarning() << "Could not connect to the report progress interface! Transaction Progress won't be available." << QDBusConnection::systemBus().lastError();
    }

    // On startup, we want to ask the backend if there's something ready for us. At ease, tho.
    connect(this, &Hemera::AsyncInitObject::ready, this, &ApplicationManagerInterface::refreshUpdateList, Qt::QueuedConnection);
    connect(this, &Hemera::AsyncInitObject::ready, this, &ApplicationManagerInterface::refreshInstalledApplicationsList, Qt::QueuedConnection);

    // Auto-check for updates, maybe? Anyway, let's do this after the initialization: we sure don't want to block.
    m_autoCheckTimer = new QTimer(this);
    m_autoCheckTimer->setSingleShot(true);
    connect(m_autoCheckTimer, &QTimer::timeout, this, &ApplicationManagerInterface::checkForApplicationUpdates);
    connect(this, &Hemera::AsyncInitObject::ready, this, &ApplicationManagerInterface::restartAutoCheckTimer, Qt::QueuedConnection);
}

void ApplicationManagerInterface::restartAutoCheckTimer()
{
    // First of all, let's see if we have to update our last update check.
    SOFTWARE_MANAGER_VOLATILE_SETTINGS;
    settings.beginGroup(QStringLiteral("status")); {
        if (settings.value(QStringLiteral("lastCheckForUpdates"), 0).toLongLong() != m_lastCheckForUpdates) {
            m_lastCheckForUpdates = settings.value(QStringLiteral("lastCheckForUpdates"), 0).toLongLong();
            Q_EMIT lastCheckForApplicationUpdatesChanged(m_lastCheckForUpdates);
        }
    } settings.endGroup();

    // Now, let's see what we have to do with our timer
    // TODO: Take those from gravity's settings.
    bool shouldCheckForUpdates = true;
    qint64 checkEveryMsecs = CHECK_UPDATES_EVERY_MSECS;
    if (shouldCheckForUpdates) {
        // Let's see what we have here.
        qint64 msecsSinceLastUpdate = QDateTime::currentMSecsSinceEpoch() - m_lastCheckForUpdates;
        if (msecsSinceLastUpdate >= checkEveryMsecs) {
            qDebug() << "Triggering an update check immediately: updates have been checked"
                     << QDateTime::fromMSecsSinceEpoch(m_lastCheckForUpdates).daysTo(QDateTime::currentDateTime()) << "days ago";
            // The timer will be restarted after the refresh.
            m_autoCheckTimer->stop();
            checkForApplicationUpdates();
        } else {
            qint64 msecsToNextCheck = checkEveryMsecs - msecsSinceLastUpdate;
            qDebug() << "Scheduling an update check in" << msecsToNextCheck / 1000 / 60 / 60 << "hours";
            m_autoCheckTimer->start(msecsToNextCheck);
        }
    } else {
        m_autoCheckTimer->stop();
    }
}

void ApplicationManagerInterface::setProgressSubscriptionState(bool subscribed)
{
    m_subscribedToProgress = subscribed;

    // If the service is already registered, update it
    if (QDBusConnection::systemBus().interface()->isServiceRegistered(BACKEND_SERVICE)) {
        QDBusMessage subscriptionCall = QDBusMessage::createMethodCall(BACKEND_SERVICE, BACKEND_PATH, BACKEND_INTERFACE, PROGRESS_SUBSCRIPTION_METHOD);
        subscriptionCall.setArguments(QVariantList() << m_subscribedToProgress);
        QDBusConnection::systemBus().asyncCall(subscriptionCall);
    }
}

QDBusMessage ApplicationManagerInterface::createBackendCall(const QString& method, const QVariantList& args)
{
    // Before anything else, send a call with the progress subscription state (unless it's false).
    if (!QDBusConnection::systemBus().interface()->isServiceRegistered(BACKEND_SERVICE) && m_subscribedToProgress) {
        QDBusMessage subscriptionCall = QDBusMessage::createMethodCall(BACKEND_SERVICE, BACKEND_PATH, BACKEND_INTERFACE, PROGRESS_SUBSCRIPTION_METHOD);
        subscriptionCall.setArguments(QVariantList() << m_subscribedToProgress);
        QDBusConnection::systemBus().asyncCall(subscriptionCall);
    }

    QDBusMessage call = QDBusMessage::createMethodCall(BACKEND_SERVICE, BACKEND_PATH, BACKEND_INTERFACE, method);
    call.setArguments(args);
    return call;
}

void ApplicationManagerInterface::onReportProgress(uint actionType, uint progress, uint rate)
{
    qDebug() << Q_FUNC_INFO << actionType << progress << rate;
}

void ApplicationManagerInterface::checkForApplicationUpdates()
{
    HANDLE_DBUS_REPLY(createBackendCall(QStringLiteral("refreshRepositories")))

    connect(watcher, &QDBusPendingCallWatcher::finished, [this, request, requestConnection] (QDBusPendingCallWatcher *call) {
        QDBusPendingReply<void> reply = *call;

        if (!reply.isError()) {
            // Good. Reset our counter.
            SOFTWARE_MANAGER_VOLATILE_SETTINGS;
            settings.beginGroup(QStringLiteral("status")); {
                settings.setValue(QStringLiteral("lastCheckForUpdates"), QDateTime::currentMSecsSinceEpoch());
            } settings.endGroup();
            settings.sync();

            // Now process the autocheck again
            restartAutoCheckTimer();
        }

        if (!reply.isError()) {
            // On a successful refresh, we also want to refresh the update list.
            refreshUpdateList();
        }
    });
}

void ApplicationManagerInterface::addRepository(const QString& name, const QStringList &urls)
{
    HANDLE_DBUS_REPLY(createBackendCall(QStringLiteral("addRepository"), QVariantList() << name << urls))
}

void ApplicationManagerInterface::removeRepository(const QString& name)
{
    HANDLE_DBUS_REPLY(createBackendCall(QStringLiteral("removeRepository"), QVariantList() << name))
}

void ApplicationManagerInterface::refreshInstalledApplicationsList()
{
    QDBusPendingCall reply = QDBusConnection::systemBus().asyncCall(createBackendCall(QStringLiteral("listInstalledApplications")));

    connect(new QDBusPendingCallWatcher(reply, this), &QDBusPendingCallWatcher::finished, [this] (QDBusPendingCallWatcher *call) {
        QDBusPendingReply<QByteArray> reply = *call;
        if (reply.isError()) {
            qWarning() << "Could not retrieve the installed applications list!";
            m_installedApplications.clear();
        } else {
            // Good. Reassign the variables.
            QByteArray installedApplications = reply.value();
            if (installedApplications != m_installedApplications) {
                m_installedApplications = installedApplications;
                Q_EMIT installedApplicationsChanged(m_installedApplications);
            }
        }

        call->deleteLater();
    });
}

void ApplicationManagerInterface::refreshUpdateList()
{
    QDBusPendingCall reply = QDBusConnection::systemBus().asyncCall(createBackendCall(QStringLiteral("listUpdates")));

    connect(new QDBusPendingCallWatcher(reply, this), &QDBusPendingCallWatcher::finished, [this] (QDBusPendingCallWatcher *call) {
        QDBusPendingReply<QByteArray, QByteArray> reply = *call;
        if (reply.isError()) {
            qWarning() << "Could not retrieve the update list!";
            m_applicationUpdates.clear();
            m_systemUpdate.clear();
        } else {
            // Good. Reassign the variables.
            QByteArray applicationUpdates = reply.argumentAt(0).toByteArray();
            if (applicationUpdates != m_applicationUpdates) {
                m_applicationUpdates = applicationUpdates;
                Q_EMIT applicationUpdatesChanged(m_applicationUpdates);
            }
        }

        call->deleteLater();
    });
}

void ApplicationManagerInterface::installApplications(const QByteArray& applications)
{
    HANDLE_DBUS_REPLY(createBackendCall(QStringLiteral("installApplications"), QVariantList() << applications))
}

void ApplicationManagerInterface::removeApplications(const QByteArray& applications)
{
    HANDLE_DBUS_REPLY(createBackendCall(QStringLiteral("removeApplications"), QVariantList() << applications))
}

void ApplicationManagerInterface::downloadApplicationUpdates(const QByteArray& updates)
{
    HANDLE_DBUS_REPLY(createBackendCall(QStringLiteral("downloadApplicationUpdates"), QVariantList() << updates))
}

void ApplicationManagerInterface::updateApplications(const QByteArray& updates)
{
    HANDLE_DBUS_REPLY(createBackendCall(QStringLiteral("updateApplications"), QVariantList() << updates))
}

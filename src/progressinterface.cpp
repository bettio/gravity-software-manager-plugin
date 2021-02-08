/*
 *
 */

#include "progressinterface.h"

#include "softwaremanagerinterface.h"

#include <QtCore/QDateTime>
#include <QtCore/QDebug>
#include <QtCore/QTimer>
#include <QtCore/QSettings>

#include <QtDBus/QDBusConnection>
#include <QtDBus/QDBusConnectionInterface>
#include <QtDBus/QDBusMessage>
#include <QtDBus/QDBusReply>
#include <QtDBus/QDBusServiceWatcher>

#include <HemeraCore/Literals>

#include <HemeraSoftwareManagement/ProgressReporter>

#include "progressreporteradaptor.h"

#define PROGRESS_SUBSCRIPTION_METHOD QStringLiteral("setSubscribedToProgress")

class ProgressInterfaceHelper
{
public:
    ProgressInterfaceHelper() : q(nullptr) {}
    ~ProgressInterfaceHelper() {
        // Do not delete - the object will be cleaned up already.
    }
    ProgressInterface *q;
};

Q_GLOBAL_STATIC(ProgressInterfaceHelper, s_globalProgressInterface)

ProgressInterface *ProgressInterface::instance()
{
    if (!s_globalProgressInterface()->q) {
        new ProgressInterface(nullptr);
    }

    return s_globalProgressInterface()->q;
}


ProgressInterface::ProgressInterface(QObject *parent)
    : AsyncInitDBusObject(parent)
    , m_startDateTime(0)
    , m_operationType(0)
    , m_availableSteps(0)
    , m_currentStep(0)
    , m_globalSubscriptions(0)
    , m_serviceWatcher(new QDBusServiceWatcher(this))
{
    Q_ASSERT(!s_globalProgressInterface()->q);
    s_globalProgressInterface()->q = this;
    connect(this, &QObject::destroyed, [] {
        s_globalProgressInterface()->q = nullptr;
    });
}

ProgressInterface::~ProgressInterface()
{
}

void ProgressInterface::initImpl()
{
    // First of all, let's register our DBus object.
    if (!QDBusConnection::systemBus().registerObject(Hemera::Literals::literal(Hemera::Literals::DBus::softwareManagementProgressReporterPath()), this)) {
        setInitError(Hemera::Literals::literal(Hemera::Literals::Errors::registerObjectFailed()),
                    QStringLiteral("Failed to register the object on the bus"));
        return;
    }

    // Create the adaptor
    new ProgressReporterAdaptor(this);

    // Setup the DBus watcher for subscribers.
    m_serviceWatcher->setConnection(QDBusConnection::systemBus());
    m_serviceWatcher->setWatchMode(QDBusServiceWatcher::WatchForUnregistration);
    // This not only removes the service from our cache, but also handles services behaving badly in the odd occurrence of a crash,
    // or severed DBus connections on shutdown.
    connect(m_serviceWatcher, &QDBusServiceWatcher::serviceUnregistered, [this] (const QString &service) {
        // Check if available
        if (m_serviceSubscriptions.contains(service)) {
            uint subscriptions = m_serviceSubscriptions.take(service);
            if (subscriptions != 0) {
                manageGlobalSubscriptions(0 - subscriptions);
            }
        }

        m_serviceWatcher->removeWatchedService(service);
    });

    // Monitor properties coming from the backend's report progress. For how DBus works, we are just "monitoring" more than
    // connecting, so this connection will be valid even if the object goes up and down.
    if (!QDBusConnection::systemBus().connect(BACKEND_SERVICE, BACKEND_PATH, Hemera::Literals::literal(Hemera::Literals::DBus::dbusObjectInterface()),
                                              QStringLiteral("propertiesChanged"), this, SLOT(onPropertiesChanged(QVariantMap)))) {
        qWarning() << "Could not connect to the progress reporter interface! Transaction Progress won't be available." << QDBusConnection::systemBus().lastError();
    }

    // Register watcher for the Backend service.
    m_backendWatcher = new QDBusServiceWatcher(BACKEND_SERVICE, QDBusConnection::systemBus(), QDBusServiceWatcher::WatchForRegistration, this);
    connect(m_backendWatcher, &QDBusServiceWatcher::serviceRegistered, this, &ProgressInterface::onBackendRegistered);

    if (QDBusConnection::systemBus().interface()->isServiceRegistered(BACKEND_SERVICE)) {
        // Cache properties
        onBackendRegistered();
    }

    setReady();
}

void ProgressInterface::onBackendRegistered()
{
    // Cache properties on startup - one never really knows.
    QDBusPendingCall reply = QDBusConnection::systemBus().asyncCall(
                                        QDBusMessage::createMethodCall(BACKEND_SERVICE, BACKEND_PATH,
                                                                       Hemera::Literals::literal(Hemera::Literals::DBus::dbusObjectInterface()),
                                                                       QStringLiteral("allProperties")));
    QDBusPendingCallWatcher *watcher = new QDBusPendingCallWatcher(reply, this);

    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this] (QDBusPendingCallWatcher *call) {
        QDBusPendingReply<QVariantMap> reply = *call;
        if (reply.isError()) {
            qWarning() << "Could not cache remote properties!" << reply.error().name() << reply.error().message();
        }

        onPropertiesChanged(reply.value());
        call->deleteLater();
    });

    // Reset subscriptions
    QDBusMessage subscriptionCall = QDBusMessage::createMethodCall(BACKEND_SERVICE, BACKEND_PATH, BACKEND_INTERFACE, PROGRESS_SUBSCRIPTION_METHOD);
    subscriptionCall.setArguments(QVariantList() << (m_globalSubscriptions > 0));
    QDBusConnection::systemBus().asyncCall(subscriptionCall);
}

void ProgressInterface::onPropertiesChanged(const QVariantMap &changed)
{
    QList<QMetaMethod> signalsToInvoke;
    for (QVariantMap::const_iterator i = changed.constBegin(); i != changed.constEnd(); ++i) {
        if (metaObject()->indexOfProperty(i.key().toLatin1().constData()) >= 0) {
            setProperty(i.key().toLatin1().constData(), i.value());
            QMetaProperty metaProperty = metaObject()->property(metaObject()->indexOfProperty(i.key().toLatin1().constData()));
            if (metaProperty.hasNotifySignal()) {
                QMetaMethod notifySignal = metaProperty.notifySignal();
                if (!signalsToInvoke.contains(notifySignal)) {
                    signalsToInvoke << notifySignal;
                }
            }
        }
    }

    // Trigger signals
    for (const QMetaMethod &notifySignal : signalsToInvoke) {
        qWarning() << "Shooting a signal!!" << notifySignal.methodSignature();
        notifySignal.invoke(this);
    }
}


void ProgressInterface::subscribe()
{
    if (!calledFromDBus()) {
        // Ignore.
        return;
    }

    // Get the caller.
    QString service = message().service();

    // Is it in already?
    if (m_serviceSubscriptions.contains(service)) {
        ++m_serviceSubscriptions[service];
    } else {
        // Time to monitor this service.
        m_serviceWatcher->addWatchedService(service);
        m_serviceSubscriptions.insert(service, 1);
    }

    manageGlobalSubscriptions(1);
}

void ProgressInterface::unsubscribe()
{
    if (!calledFromDBus()) {
        // Ignore.
        return;
    }

    // Get the caller.
    QString service = message().service();

    // Is it in already?
    if (!m_serviceSubscriptions.contains(service)) {
        // Wat.
        return;
    } else {
        --m_serviceSubscriptions[service];
    }

    manageGlobalSubscriptions(-1);
}

void ProgressInterface::manageGlobalSubscriptions(int differential)
{
    int newGlobalSubscriptions = m_globalSubscriptions + differential;
    if (newGlobalSubscriptions < 0) {
        // Wait, wat?
        return;
    }

    bool updateBackend = false;
    if ((newGlobalSubscriptions > 0 && m_globalSubscriptions == 0) || (newGlobalSubscriptions == 0 && m_globalSubscriptions > 0)) {
        // We have to update the backend
        updateBackend = true;
    }

    m_globalSubscriptions = newGlobalSubscriptions;

    // If the service is already registered, update it
    if (QDBusConnection::systemBus().interface()->isServiceRegistered(BACKEND_SERVICE) && updateBackend) {
        QDBusMessage subscriptionCall = QDBusMessage::createMethodCall(BACKEND_SERVICE, BACKEND_PATH, BACKEND_INTERFACE, PROGRESS_SUBSCRIPTION_METHOD);
        subscriptionCall.setArguments(QVariantList() << (m_globalSubscriptions > 0));
        QDBusConnection::systemBus().asyncCall(subscriptionCall);
    }
}

bool ProgressInterface::isSubscribed() const
{
    return m_globalSubscriptions > 0;
}

uint ProgressInterface::availableSteps() const
{
    return m_availableSteps;
}

uint ProgressInterface::currentStep() const
{
    return m_currentStep;
}

QString ProgressInterface::description() const
{
    return m_description;
}

QByteArray ProgressInterface::operationId() const
{
    return m_operationId;
}

uint ProgressInterface::operationType() const
{
    return m_operationType;
}

int ProgressInterface::percent() const
{
    return m_percent;
}

int ProgressInterface::rate() const
{
    return m_rate;
}

qint64 ProgressInterface::startDateTime() const
{
    return m_startDateTime;
}

LocalDownloadOperation *ProgressInterface::startLocalDownloadOperation()
{
    if (!m_localDownloadOperation.isNull() || !m_operationId.isEmpty()) {
        return nullptr;
    }

    m_localDownloadOperation = new LocalDownloadOperation(this);

    QUuid uuid = QUuid::createUuid();
    QDateTime start = QDateTime::currentDateTime();
    m_operationId = QString::fromLatin1("%1_%2").arg(start.toString(QStringLiteral("yyyyMMddHHmmss")), uuid.toString()).toLatin1();
    m_startDateTime = start.toMSecsSinceEpoch();
    m_availableSteps = static_cast<uint>(Hemera::SoftwareManagement::ProgressReporter::OperationStep::Download);
    m_operationType = static_cast<uint>(Hemera::SoftwareManagement::ProgressReporter::OperationType::UpdateSystem);
    m_currentStep = static_cast<uint>(Hemera::SoftwareManagement::ProgressReporter::OperationStep::NoStep);

    Q_EMIT operationTypeChanged();

    m_currentStep = static_cast<uint>(Hemera::SoftwareManagement::ProgressReporter::OperationStep::Download);
    Q_EMIT currentStepChanged();

    connect(m_localDownloadOperation, &LocalDownloadOperation::progressChanged, [this] (int percent, int rate) {
        m_percent = percent;
        m_rate = rate;
        Q_EMIT progressChanged();
    });
    connect(m_localDownloadOperation, &LocalDownloadOperation::finished, [this] {
        m_operationId.clear();
        m_startDateTime = 0;
        m_availableSteps = 0;
        m_operationType = 0;
        m_currentStep = 0;

        Q_EMIT currentStepChanged();
        Q_EMIT operationTypeChanged();

        m_localDownloadOperation->deleteLater();
    });

    return m_localDownloadOperation.data();
}


LocalDownloadOperation::LocalDownloadOperation(QObject* parent)
    : QObject(parent)
{
}

LocalDownloadOperation::~LocalDownloadOperation()
{
}

void LocalDownloadOperation::setFinished()
{
    Q_EMIT finished();
}

void LocalDownloadOperation::setProgress(int progress, int rate)
{
    Q_EMIT progressChanged(progress, rate);
}

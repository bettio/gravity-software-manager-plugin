/*
 *
 */

#ifndef PROGRESSINTERFACE_H
#define PROGRESSINTERFACE_H

#include <HemeraCore/AsyncInitDBusObject>

#include <QtCore/QPointer>

class SoftwareManagerInterface;
class QDBusServiceWatcher;

class LocalDownloadOperation : public QObject
{
    Q_OBJECT
    Q_DISABLE_COPY(LocalDownloadOperation)

public Q_SLOTS:
    void setProgress(int progress, int rate = -1);
    void setFinished();

Q_SIGNALS:
    void progressChanged(int, int);
    void finished();

private:
    LocalDownloadOperation(QObject *parent = nullptr);
    virtual ~LocalDownloadOperation();

    friend class ProgressInterface;
};

class ProgressInterface : public Hemera::AsyncInitDBusObject
{
    Q_OBJECT
    Q_DISABLE_COPY(ProgressInterface)
    Q_CLASSINFO("D-Bus Interface", "com.ispirata.Hemera.SoftwareManagement.ProgressReporter")

    Q_PROPERTY(QByteArray operationId MEMBER m_operationId NOTIFY operationTypeChanged)
    Q_PROPERTY(qint64 startDateTime MEMBER m_startDateTime NOTIFY operationTypeChanged)
    Q_PROPERTY(uint operationType MEMBER m_operationType NOTIFY operationTypeChanged)
    Q_PROPERTY(uint availableSteps MEMBER m_availableSteps NOTIFY operationTypeChanged)

    Q_PROPERTY(uint currentStep MEMBER m_currentStep NOTIFY currentStepChanged)
    Q_PROPERTY(QString description MEMBER m_description NOTIFY descriptionChanged)
    Q_PROPERTY(int percent MEMBER m_percent NOTIFY progressChanged)
    Q_PROPERTY(int rate MEMBER m_rate NOTIFY progressChanged)

public:
    static ProgressInterface *instance();

    virtual ~ProgressInterface();

    bool isSubscribed() const;

    QByteArray operationId() const;
    qint64 startDateTime() const;
    uint operationType() const;
    uint availableSteps() const;
    uint currentStep() const;

    QString description() const;
    int percent() const;
    int rate() const;

    LocalDownloadOperation *startLocalDownloadOperation();

// DBus methods
public Q_SLOTS:
    void subscribe();
    void unsubscribe();

protected:
    virtual void initImpl() override final;

Q_SIGNALS:
    void operationTypeChanged();
    void currentStepChanged();
    void descriptionChanged();
    void progressChanged();

private Q_SLOTS:
    void onPropertiesChanged(const QVariantMap &changed);
    void onBackendRegistered();

private:
    explicit ProgressInterface(QObject *parent);

    QByteArray m_operationId;
    qint64 m_startDateTime;
    uint m_operationType;
    uint m_availableSteps;
    uint m_currentStep;

    QString m_description;
    int m_percent;
    int m_rate;

    QHash< QString, uint > m_serviceSubscriptions;
    uint m_globalSubscriptions;
    QDBusServiceWatcher *m_serviceWatcher;
    QDBusServiceWatcher *m_backendWatcher;

    QPointer< LocalDownloadOperation > m_localDownloadOperation;

    void manageGlobalSubscriptions(int differential);
};

#endif // PROGRESSINTERFACE_H

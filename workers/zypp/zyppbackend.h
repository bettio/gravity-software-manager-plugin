#ifndef ZYPPBACKEND_H
#define ZYPPBACKEND_H

#include <HemeraCore/AsyncInitDBusObject>

#include <HemeraCore/Operation>

#include <QtDBus/QDBusMessage>

#include <zypp/ZYpp.h>
#include <zypp/RepoManager.h>

class CallbacksManager;
class QTimer;
class ZyppBackend : public Hemera::AsyncInitDBusObject
{
    Q_OBJECT
    Q_DISABLE_COPY(ZyppBackend)
    Q_CLASSINFO("D-Bus Interface", "com.ispirata.Hemera.Gravity.SoftwareManager.Backend")

    Q_PROPERTY(uint status MEMBER m_status NOTIFY statusChanged)

    Q_PROPERTY(QByteArray operationId READ progressOperationId NOTIFY progressOperationTypeChanged)
    Q_PROPERTY(qint64 startDateTime READ progressStartDateTime NOTIFY progressOperationTypeChanged)
    Q_PROPERTY(uint operationType READ progressOperationType NOTIFY progressOperationTypeChanged)
    Q_PROPERTY(uint availableSteps READ progressAvailableSteps NOTIFY progressOperationTypeChanged)

    Q_PROPERTY(uint currentStep READ progressCurrentStep NOTIFY progressCurrentStepChanged)
    Q_PROPERTY(QString description READ progressDescription NOTIFY progressDescriptionChanged)
    Q_PROPERTY(int percent READ progressPercent NOTIFY progressChanged)
    Q_PROPERTY(int rate READ progressRate NOTIFY progressChanged)

public:
    enum class Status : uint {
        Unknown = 0,
        Uninitialized,
        Idle,
        Processing,
        Failed = 254
    };

    explicit ZyppBackend(QObject *parent = 0);
    virtual ~ZyppBackend();

    enum class PackageOperation : uint {
        Install,
        Remove,
        Update,
        InstallOrUpdate
    };

    static bool markPackage(const std::list<zypp::RepoInfo> &repos, const std::string &packageName, PackageOperation operation, bool force = false);

public Q_SLOTS:
    void addRepository(const QString &name, const QStringList &urls);
    void removeRepository(const QString &name);
    void refreshRepositories();

    void downloadApplicationUpdates(const QByteArray &updates);
    void updateApplications(const QByteArray &updates);
    void updateSystem(const QString &updatePath);

    void installApplications(const QByteArray &applications);
    void removeApplications(const QByteArray &applications);
    void installLocalPackage(const QString &package);

    QByteArray listUpdates();
    QByteArray listInstalledApplications();
    QByteArray listRepositories();

    void setSubscribedToProgress(bool subscribed);

    QByteArray progressOperationId() const;
    qint64 progressStartDateTime() const;
    uint progressOperationType() const;
    uint progressAvailableSteps() const;
    uint progressCurrentStep() const;
    QString progressDescription() const;
    int progressPercent() const;
    int progressRate() const;

    int configureCallbacksManager(zypp::sat::Transaction transaction);
    void resetCallbacksManager();

protected:
    virtual void initImpl() override final;

Q_SIGNALS:
    void statusChanged(uint status);
    void explode();

    void progressOperationTypeChanged();
    void progressCurrentStepChanged();
    void progressDescriptionChanged();
    void progressChanged();

private:
    void refreshTarget();

    void setStatus(Status status);

    bool addRepositoryInternal(const QString &alias, const QStringList &urls, const QDBusMessage &message = QDBusMessage());
    bool removeRepositoryInternal(const QString &alias, const QDBusMessage &message = QDBusMessage());

    zypp::RepoManager *prepareLocalRepositoryTransaction(const QString &dirPath, bool force = false);

    zypp::ZYpp::Ptr m_zypp;
    uint m_status;
    QTimer *m_timebomb;
    CallbacksManager *m_callbacks;

    QByteArray m_progressOperationId;
    qint64 m_progressStartDateTime;
    uint m_progressOperationType;
    uint m_progressAvailableSteps;
    uint m_progressCurrentStep;
    QString m_progressDescription;
    int m_progressPercent;
    int m_progressRate;

    friend class CallbacksManager;
    friend class ZyppCommitOperation;
    friend class ZyppRefreshRepositoriesOperation;
};

// Operations
class ZyppRefreshRepositoriesOperation : public Hemera::Operation
{
    Q_OBJECT
    Q_DISABLE_COPY(ZyppRefreshRepositoriesOperation)

public:
    explicit ZyppRefreshRepositoriesOperation(zypp::ZYpp::Ptr zypp, ZyppBackend *backend, CallbacksManager *callbacks, QObject *parent = nullptr);
    virtual ~ZyppRefreshRepositoriesOperation();

protected:
    virtual void startImpl() override final;

private:
    zypp::ZYpp::Ptr m_zypp;
    ZyppBackend *m_backend;
    CallbacksManager *m_callbacks;
};

class ZyppPackageOperation : public Hemera::Operation
{
    Q_OBJECT
    Q_DISABLE_COPY(ZyppPackageOperation)

public:
    explicit ZyppPackageOperation(zypp::ZYpp::Ptr zypp, ZyppBackend *backend, const QStringList &packages,
                                  ZyppBackend::PackageOperation operation, bool downloadOnly, QObject *parent = nullptr);
    virtual ~ZyppPackageOperation();

protected:
    virtual void startImpl() override final;

private:
    zypp::ZYpp::Ptr m_zypp;
    ZyppBackend *m_backend;
    QStringList m_packages;
    ZyppBackend::PackageOperation m_operation;
    bool m_downloadOnly;
};

class ZyppCommitOperation : public Hemera::Operation
{
    Q_OBJECT
    Q_DISABLE_COPY(ZyppCommitOperation)

public:
    explicit ZyppCommitOperation(zypp::ZYpp::Ptr zypp, ZyppBackend *backend, zypp::RepoManager *manager, zypp::ZYppCommitPolicy commitPolicy, QObject* parent = nullptr);
    virtual ~ZyppCommitOperation();

    int items() const;

protected:
    virtual void startImpl() override final;

private:
    zypp::ZYpp::Ptr m_zypp;
    ZyppBackend *m_backend;
    zypp::RepoManager *m_manager;
    zypp::ZYppCommitPolicy m_policy;

    int m_items;
};

#endif // ZYPPBACKEND_H

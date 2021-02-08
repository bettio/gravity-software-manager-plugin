/*
 *
 */

#include "zyppbackend.h"

#include "zyppworkercallbacks.h"
#include "workersglobalhelpers.h"
#include "softwaremanagerinterface.h"

#include <QtCore/QDebug>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QTimer>
#include <QtCore/QUuid>

#include <QtDBus/QDBusConnection>
#include <QtDBus/QDBusMessage>

#include <HemeraCore/Literals>

#include <HemeraSoftwareManagement/ApplicationPackage>
#include <HemeraSoftwareManagement/ApplicationUpdate>
#include <HemeraSoftwareManagement/Repository>
#include <HemeraSoftwareManagement/SystemUpdate>

#include <private/HemeraSoftwareManagement/hemerasoftwaremanagementconstructors_p.h>

#include <zypp/FileChecker.h>
#include <zypp/ZYppFactory.h>
#include <zypp/Pathname.h>
#include <zypp/RepoManager.h>
#include <zypp/PathInfo.h>
#include <zypp/PoolItemBest.h>
#include <zypp/PoolQuery.h>
#include <zypp/RepoInfo.h>

#include <zypp/media/MediaException.h>
#include <zypp/parser/ParseException.h>
#include <zypp/target/rpm/RpmHeader.h>

#include "backendadaptor.h"

#include <softwaremanagerconfig.h>


#define CHECK_DBUS_CALLER(BaseReturnType) \
if (!calledFromDBus()) { \
    return BaseReturnType(); \
} \
QDBusMessage request = message(); \
if (connection().name() != QDBusConnection::systemBus().name()) { \
    qWarning() << "Someone just tried to hijack the service from a different bus." << connection().name() << request; \
    sendErrorReply(QDBusError::errorString(QDBusError::AccessDenied), QStringLiteral("Sorry, you're not allowed to chime in here.")); \
    return BaseReturnType(); \
}

#define CHECK_DBUS_CALLER_VOID \
if (!calledFromDBus()) { \
    return; \
} \
QDBusMessage request = message(); \
if (connection().name() != QDBusConnection::systemBus().name()) { \
    qWarning() << "Someone just tried to hijack the service from a different bus." << connection().name() << request; \
    sendErrorReply(QDBusError::errorString(QDBusError::AccessDenied), QStringLiteral("Sorry, you're not allowed to chime in here.")); \
    return; \
}

#define ENQUEUE_OPERATION \
while (m_status != static_cast<uint>(Status::Idle)) {\
    QEventLoop e;\
    connect(this, &ZyppBackend::statusChanged, &e, &QEventLoop::quit, Qt::QueuedConnection);\
    e.exec();\
}

#define HANDLE_OPERATION_DBUS(op)\
connect(op, &Hemera::Operation::finished, [this, op, request] {\
    if (!op->isError()) {\
        QDBusConnection::systemBus().send(request.createReply());\
    } else {\
        QDBusConnection::systemBus().send(request.createErrorReply(op->errorName(), op->errorMessage()));\
    }\
\
    setStatus(Status::Idle);\
});

#define TMP_RPM_REPO_ALIAS "hemera-temp-local-repo"

zypp::PoolItem zypp_get_installed_obj(zypp::ui::Selectable::Ptr & s)
{
    zypp::PoolItem installed;
    if (zypp::traits::isPseudoInstalled(s->kind())) {
        for_(it, s->availableBegin(), s->availableEnd())
        // this is OK also for patches - isSatisfied() excludes !isRelevant()
        if (it->status().isSatisfied()
            && (!installed || installed->edition() < (*it)->edition())) {
            installed = *it;
        }
    } else {
        installed = s->installedObj();
    }

    return installed;
}

void zypp_prepare_pool(zypp::ZYpp::Ptr m_zypp, zypp::RepoManager *manager)
{
    // Load resolvables first, from repos and our target.
    std::list<zypp::RepoInfo> repos;
    repos.insert(repos.end(), manager->repoBegin(), manager->repoEnd());
    qDebug() << "Found " << repos.size() << " repos.";

    for (std::list<zypp::RepoInfo>::const_iterator it = repos.begin(); it != repos.end(); ++it) {
        zypp::RepoInfo repo(*it);

        if (!it->enabled()) {
            // Skip disabled repos
            continue;
        }

        try {
            bool error = false;
            // if there is no metadata locally
            if (manager->metadataStatus(repo).empty()) {
                // TODO: See refresh_raw_metadata. Here we need to force a raw metadata refresh if
                //       the metadata is empty.
            }

            if (!error && !manager->isCached(repo)) {
                try {
                    manager->buildCache(repo, zypp::RepoManager::BuildIfNeeded);
                } catch (const zypp::parser::ParseException & e) {
                    ZYPP_CAUGHT(e);

                    qWarning() << "Error parsing metadata for" << repo.alias().c_str();
                    continue;
                } catch (const zypp::repo::RepoMetadataException & e) {
                    ZYPP_CAUGHT(e);

                    // this should not happen and is probably a bug.
                    qWarning() << "Repository metadata for" << repo.alias().c_str() << "not found in local cache. This should not happen.";
                    continue;
                } catch (const zypp::Exception &e) {
                    ZYPP_CAUGHT(e);

                    qWarning() << "Error writing to cache db";
                    continue;
                }
            }

            if (error) {
                qDebug() << "Error loading resolvables for " << repo.alias().c_str();
                continue;
            }

            manager->loadFromCache(repo);

            // check that the metadata is not outdated
            zypp::Repository robj = zypp::sat::Pool::instance().reposFind(repo.alias());
            if (robj != zypp::Repository::noRepository && robj.maybeOutdated()) {
                qWarning() << "Repository" << repo.alias().c_str() << "appears to be outdated. Consider using a different mirror or server.";
            }
        } catch (const zypp::Exception & e) {
            ZYPP_CAUGHT(e);

            qWarning() << "Resolvables from" << repo.alias().c_str() << "not loaded because of error.";
        }
    }
    try {
        m_zypp->target()->load();
    } catch ( const zypp::Exception & e ) {
        ZYPP_CAUGHT(e);
        qWarning() << "Problem occured while reading the installed packages:" << e.asUserHistory().c_str();
    }

    // Ok, resolvables loaded.

    m_zypp->resolver()->setAllowVendorChange(false);
    m_zypp->resolver()->setCleandepsOnRemove(true);
    m_zypp->resolver()->setForceResolve(true);
    m_zypp->resolver()->setIgnoreAlreadyRecommended(true);
    m_zypp->resolver()->setOnlyRequires(false);
    m_zypp->resolver()->setSolveSrcPackages(false);
    m_zypp->resolver()->setSystemVerification(true);
}

bool ZyppBackend::markPackage(const std::list< zypp::RepoInfo > &repos, const std::string &packageName, ZyppBackend::PackageOperation operation, bool force)
{
    zypp::PoolQuery q;
    q.addKind(zypp::ResKind::package);
    q.setMatchGlob();
    for (std::list<zypp::RepoInfo>::const_iterator it = repos.begin(); it != repos.end(); ++it) {
        q.addRepo((*it).alias());
    }

    // Look for package names
    q.addDependency(zypp::sat::SolvAttr::name, packageName);

    // get the best matching items and tag them for installation.
    // FIXME this ignores vendor lock - we need some way to do --from which
    // would respect vendor lock: e.g. a new Selectable::updateCandidateObj(Options&)
    zypp::PoolItemBest bestMatches(q.begin(), q.end());
    if (!bestMatches.empty()) {
        bool completed = false;
        for (zypp::PoolItemBest::iterator sit = bestMatches.begin(); sit != bestMatches.end(); ++sit) {
            zypp::ui::Selectable::Ptr s(zypp::ui::asSelectable()(*sit));

            // What are we doing?
            switch (operation) {
                case PackageOperation::Install:
                    if (s->hasInstalledObj()) {
                        // Are we forcing?
                        if (force) {
                            // It's ok then, let's just insist.
                            (*sit).status().setToBeInstalled(zypp::ResStatus::USER);
                            qDebug() << "A candidate is already installed, forcing the selected one to be installed";
                            completed = true;
                        } else {
                            // Ouch. Move on, even though it's going to be a failure 99%
                        }
                    } else {
                        s->setOnSystem(*sit);
                        qDebug() << "Marked a solvable to be on system";
                        completed = true;
                    }
                    break;
                case PackageOperation::Remove:
                    if (sit->status().isInstalled()) {
                        // Got it!
                        (*sit).status().setToBeUninstalled(zypp::ResStatus::USER);
                        qDebug() << "Marked a package for removal";
                        completed = true;
                    } else {
                        // Move on...
                    }
                    break;
                case PackageOperation::Update: {
                        zypp::PoolItem instobj = zypp_get_installed_obj(s);
                        if (instobj) {
                            if (s->availableEmpty()) {
                                // Damn.
                                break;
                            }

                            // check vendor (since PoolItemBest does not do it)
                            // FIXME: Do we need this?
                            // bool changes_vendor = ! VendorAttr::instance().equivalent(
                            //           instobj->vendor(), (*sit)->vendor());

                            zypp::PoolItem best;
                            if ((best = s->updateCandidateObj())) {
                                zypp::ui::asSelectable()(best)->setOnSystem(best);
                                completed = true;
                            } else {
                                // No match.
                            }
                        }
                    }
                    break;
                case PackageOperation::InstallOrUpdate: {
                    if (s->hasInstalledObj()) {
                        // Can we update?
                        zypp::PoolItem instobj = zypp_get_installed_obj(s);
                        if (instobj) {
                            if (s->availableEmpty()) {
                                // Are we forcing?
                                if (force) {
                                    // It's ok then, let's just insist.
                                    (*sit).status().setToBeInstalled(zypp::ResStatus::USER);
                                    qDebug() << "A candidate is already installed, forcing the selected one to be installed";
                                    completed = true;
                                } else {
                                    // Ouch. Move on, even though it's going to be a failure 99%
                                }
                            }

                            // check vendor (since PoolItemBest does not do it)
                            // FIXME: Do we need this?
                            // bool changes_vendor = ! VendorAttr::instance().equivalent(
                            //           instobj->vendor(), (*sit)->vendor());

                            zypp::PoolItem best;
                            if ((best = s->updateCandidateObj())) {
                                zypp::ui::asSelectable()(best)->setOnSystem(best);
                                completed = true;
                            } else {
                                // No match.
                            }
                        }
                    } else {
                        s->setOnSystem(*sit);
                        qDebug() << "Marked a solvable to be on system";
                        completed = true;
                    }
                    break;
                }
                default:
                    // Wat?
                    return false;
            }

            if (completed) {
                // We're done
                return true;
            }
        }
    } else {
        // Ouch.
        return false;
    }

    // If we got here, it didn't go that well.
    return false;
}

ZyppBackend::ZyppBackend(QObject *parent)
    : Hemera::AsyncInitDBusObject(parent)
    , m_status(static_cast<uint>(Status::Uninitialized))
    , m_timebomb(new QTimer(this))
    , m_callbacks(nullptr)
{
}

ZyppBackend::~ZyppBackend()
{
    // Destroy the callbacks
    delete m_callbacks;
}

void ZyppBackend::setStatus(ZyppBackend::Status status)
{
    if (static_cast<uint>(status) != m_status) {
        // Timebomb first.
        if (status == Status::Idle) {
            qDebug() << "Starting our idle timebomb.";
            m_timebomb->start();

            // Also, reset callbacks
            m_callbacks->setOperationType(CallbacksManager::OperationType::NoOperation);
            if (!m_progressOperationId.isEmpty()) {
                // And reset the progress interface
                m_progressOperationId = QByteArray();
                m_progressCurrentStep = 0;
                m_progressStartDateTime = 0;
                m_progressAvailableSteps = 0;
                m_progressOperationType = 0;
                // Transaction type has changed
                Q_EMIT progressOperationTypeChanged();
            }
        } else if (static_cast<Status>(m_status) == Status::Idle) {
            qDebug() << "Stopping our idle timebomb";
            m_timebomb->stop();
        }

        m_status = static_cast<uint>(status);
        Q_EMIT statusChanged(m_status);
    }
}

void ZyppBackend::setSubscribedToProgress(bool subscribed)
{
    m_callbacks->setProgressStreamIsActive(subscribed);
}

uint ZyppBackend::progressOperationType() const
{
    return m_progressOperationType;
}

uint ZyppBackend::progressAvailableSteps() const
{
    return m_progressAvailableSteps;
}

uint ZyppBackend::progressCurrentStep() const
{
    return m_progressCurrentStep;
}

QByteArray ZyppBackend::progressOperationId() const
{
    return m_progressOperationId;
}

qint64 ZyppBackend::progressStartDateTime() const
{
    return m_progressStartDateTime;
}

QString ZyppBackend::progressDescription() const
{
    return m_progressDescription;
}

int ZyppBackend::progressPercent() const
{
    return m_progressPercent;
}

int ZyppBackend::progressRate() const
{
    return m_progressRate;
}

void ZyppBackend::initImpl()
{
    // Set our timebomb in 15 seconds.
    m_timebomb->setInterval(15 * 1000);
    m_timebomb->setSingleShot(true);
    connect(m_timebomb, &QTimer::timeout, this, &ZyppBackend::explode);

    try {
        m_zypp = zypp::getZYpp();
    } catch (const zypp::ZYppFactoryException &excpt_r) {
        ZYPP_CAUGHT (excpt_r);
        setInitError(QStringLiteral("backend"), QStringLiteral("Could not initialize Zypp, as the backend is already locked."));
        return;
    } catch (const zypp::Exception &excpt_r) {
        ZYPP_CAUGHT (excpt_r);
        setInitError(QStringLiteral("backend"), QString::fromStdString(excpt_r.asUserHistory()));
        return;
    }

    m_zypp->initializeTarget("/");
    m_zypp->target()->load();

    // Connect the callbacks
    m_callbacks = new CallbacksManager(this);

    // Bring up the bus!
    if (!QDBusConnection::systemBus().registerObject(BACKEND_PATH, this)) {
        setInitError(Hemera::Literals::literal(Hemera::Literals::Errors::registerObjectFailed()),
                    QStringLiteral("Failed to register the object on the bus"));
        return;
    }
    if (!QDBusConnection::systemBus().registerService(BACKEND_SERVICE)) {
        setInitError(Hemera::Literals::literal(Hemera::Literals::Errors::registerServiceFailed()),
                     QStringLiteral("Failed to register the service on the bus"));
        return;
    }

    new BackendAdaptor(this);

    // Make the backend ready and ignite the timebomb
    setStatus(Status::Idle);

    setReady();
}

int ZyppBackend::configureCallbacksManager(zypp::sat::Transaction transaction)
{
    // Iterate each step to find out the real transaction size.
    quint64 items = 0;
    quint64 downloadSize = 0;
    for (zypp::sat::Transaction::const_iterator it = transaction.begin(); it != transaction.end(); ++it) {
        zypp::sat::Transaction::Step step = *it;
        if (step.stepType() == zypp::sat::Transaction::TRANSACTION_IGNORE) {
            // Ignore steps are not interesting for our callbacks.
            continue;
        }

        ++items;
        zypp::ResObject::Ptr o(zypp::makeResObject(step.satSolvable()));
        downloadSize += o->downloadSize();
    }

    m_callbacks->setOperationType(CallbacksManager::OperationType::Package);
    m_callbacks->setTotalItems(items, downloadSize);

    qDebug() << "Callbacks have been configured for a transaction consisting of" << items << "items, and" << downloadSize << "bytes to be downloaded.";
    zypp::sat::dumpOn(std::cout, m_zypp->resolver()->getTransaction());

    return items;
}

void ZyppBackend::resetCallbacksManager()
{
    m_callbacks->setOperationType(CallbacksManager::OperationType::NoOperation);
    qDebug() << "Callbacks reset and idling";
}

void ZyppBackend::addRepository(const QString &name, const QStringList &urls)
{
    CHECK_DBUS_CALLER_VOID
    ENQUEUE_OPERATION

    setDelayedReply(true);

    setStatus(Status::Processing);

    if (addRepositoryInternal(name, urls, request)) {
        QDBusConnection::systemBus().send(request.createReply());
    }

    setStatus(Status::Idle);
}

bool ZyppBackend::addRepositoryInternal(const QString &alias, const QStringList &urls, const QDBusMessage &request)
{
    zypp::RepoInfo repo;

    // Heuristics for determining which repository we are dealing with
    // NOTE: Hemera uses only rpm-md repositories.
    if (urls.size() > 1) {
        // No doubts here.
        repo.setType(zypp::repo::RepoType::RPMMD);
        for (const QString &url : urls) {
            zypp::Url zyppUrl(url.toStdString());
            if (!zyppUrl.isValid()) {
                if (request.type() == QDBusMessage::MethodCallMessage) {
                    QDBusConnection::systemBus().send(request.createErrorReply(QDBusError::errorString(QDBusError::InvalidArgs), QStringLiteral("Invalid repo URL")));
                }
                return false;
            }
            repo.addBaseUrl(zyppUrl);
        }
    } else {
        // Inspect the URL.
        zypp::Url url(urls.first().toStdString());
        if (!url.isValid()) {
            if (request.type() == QDBusMessage::MethodCallMessage) {
                QDBusConnection::systemBus().send(request.createErrorReply(QDBusError::errorString(QDBusError::InvalidArgs), QStringLiteral("Invalid repo URL")));
            }
            return false;
        }
        if (url.schemeIsLocal()) {
            // It's a plain dir repository
            repo.setType(zypp::repo::RepoType::RPMPLAINDIR);
            // empty packages path would cause unwanted removal of installed rpms
            // in current working directory (bnc #445504)
            // OTOH packages path == ZYPPER_RPM_CACHE_DIR (the same as repo URI)
            // causes cp file thesamefile, which fails silently. This may be worth
            // fixing in libzypp.
            repo.setPackagesPath("/tmp");
        } else {
            // It's an md repo
            repo.setType(zypp::repo::RepoType::RPMMD);
        }

        // Add the URL
        repo.addBaseUrl(url);
    }

    repo.setEnabled(true);
    repo.setAutorefresh(true);
    repo.setAlias(alias.toStdString());
    repo.setName(alias.toStdString());
    // Hemera's policy is to delete package from the cache to save space.
    repo.setKeepPackages(false);

    zypp::RepoManager manager;

    try {
        manager.addRepository(repo);
    } catch (const zypp::repo::RepoAlreadyExistsException & e) {
        // It's ok to be here
        qWarning() << "Warning: the repo already exists";
    /* We might, one day, care about more specialized exception handling, even if I don't think so.
    } catch (const zypp::repo::RepoInvalidAliasException & e) {
    } catch (const zypp::repo::RepoUnknownTypeException & e) {
    } catch (const zypp::repo::RepoException & e) {
    */
    } catch (const zypp::Exception & e) {
        if (request.type() == QDBusMessage::MethodCallMessage) {
            QDBusConnection::systemBus().send(request.createErrorReply(QDBusError::errorString(QDBusError::InternalError), QString::fromStdString(e.asUserHistory())));
        }
        return false;
    }

    return true;
}


void ZyppBackend::removeRepository(const QString &name)
{
    CHECK_DBUS_CALLER_VOID
    ENQUEUE_OPERATION

    setDelayedReply(true);

    setStatus(Status::Processing);

    if (removeRepositoryInternal(name, request)) {
        QDBusConnection::systemBus().send(request.createReply());
    }

    setStatus(Status::Idle);
}

bool ZyppBackend::removeRepositoryInternal(const QString &alias, const QDBusMessage &request)
{
    zypp::RepoManager manager;

    try {
        manager.removeRepository(manager.getRepo(alias.toStdString()));
    } catch (const zypp::Exception & e) {
        if (request.type() == QDBusMessage::MethodCallMessage) {
            QDBusConnection::systemBus().send(request.createErrorReply(QDBusError::errorString(QDBusError::InternalError), QString::fromStdString(e.asUserHistory())));
        }
        return false;
    }

    return true;
}

void ZyppBackend::refreshRepositories()
{
    CHECK_DBUS_CALLER_VOID
    ENQUEUE_OPERATION

    setStatus(Status::Processing);

    // Delay our reply
    setDelayedReply(true);

    Hemera::Operation *op = new ZyppRefreshRepositoriesOperation(m_zypp, this, m_callbacks, this);
    HANDLE_OPERATION_DBUS(op)
}

void ZyppBackend::downloadApplicationUpdates(const QByteArray &updates)
{
    CHECK_DBUS_CALLER_VOID
    ENQUEUE_OPERATION

    using namespace Hemera::SoftwareManagement;

    setStatus(Status::Processing);

    // Delay our reply
    setDelayedReply(true);

    QStringList packages;
    ApplicationUpdates applicationUpdates = Constructors::applicationUpdatesFromJson(QJsonDocument::fromJson(updates).array());
    for (const ApplicationUpdate &update : applicationUpdates) {
        packages.append(update.applicationId());
    }

    m_progressAvailableSteps = static_cast<uint>(ProgressReporter::OperationStep::Download);
    m_progressOperationType = static_cast<uint>(ProgressReporter::OperationType::UpdateSystem);
    Hemera::Operation *op = new ZyppPackageOperation(m_zypp, this, packages, PackageOperation::Update, true, this);
    HANDLE_OPERATION_DBUS(op)
}

void ZyppBackend::updateApplications(const QByteArray &updates)
{
    CHECK_DBUS_CALLER_VOID
    ENQUEUE_OPERATION

    using namespace Hemera::SoftwareManagement;

    setStatus(Status::Processing);

    // Delay our reply
    setDelayedReply(true);

    QStringList packages;
    ApplicationUpdates applicationUpdates = Constructors::applicationUpdatesFromJson(QJsonDocument::fromJson(updates).array());
    for (const ApplicationUpdate &update : applicationUpdates) {
        packages.append(update.applicationId());
    }

    m_progressAvailableSteps = static_cast<uint>(ProgressReporter::OperationStep::Download | ProgressReporter::OperationStep::Process);
    m_progressOperationType = static_cast<uint>(ProgressReporter::OperationType::UpdateApplications);
    Hemera::Operation *op = new ZyppPackageOperation(m_zypp, this, packages, PackageOperation::Update, false, this);
    HANDLE_OPERATION_DBUS(op)
}

zypp::RepoManager *ZyppBackend::prepareLocalRepositoryTransaction(const QString &updatePath, bool force)
{
    QDir packagesDir(updatePath);
    QStringList installPackages;
    for (const QFileInfo &package : packagesDir.entryInfoList(QStringList() << QStringLiteral("*.rpm"))) {
        using zypp::target::rpm::RpmHeader;
        // rpm header (need name-version-release)
        RpmHeader::constPtr header =
            RpmHeader::readPackage(package.absoluteFilePath().toStdString(), RpmHeader::NOSIGNATURE);
        if (header) {
            qDebug() << "Going to install package " << header->tag_name().c_str() << header->tag_edition().asString().c_str();
            installPackages << QString::fromStdString(header->tag_name());
        } else {
            sendErrorReply(QDBusError::errorString(QDBusError::InternalError),
                            QStringLiteral("Contents of the package appear to be corrupted or invalid."));
            setStatus(Status::Idle);
            return nullptr;
        }
    }

    // add a plaindir repo
    if (!addRepositoryInternal(QLatin1String(TMP_RPM_REPO_ALIAS), QStringList() << QString::fromLatin1("dir://%1").arg(updatePath))) {
        sendErrorReply(QDBusError::errorString(QDBusError::InternalError), QStringLiteral("Could not create temporary repository"));
        setStatus(Status::Idle);
        return nullptr;
    }

    zypp::RepoManager *manager = new zypp::RepoManager;
    zypp::RepoInfo repo;

    try {
        repo = manager->getRepo(TMP_RPM_REPO_ALIAS);
        manager->refreshMetadata(repo, zypp::RepoManager::RefreshIfNeeded);
        manager->buildCache(repo);
    } catch (const zypp::Exception & e) {
        sendErrorReply(QDBusError::errorString(QDBusError::InternalError), QStringLiteral("Could not load temporary repository"));
        qWarning() << "Warning: could not load temporary repository!" << updatePath << QString::fromStdString(e.asString());
        if (!removeRepositoryInternal(QString::fromStdString(repo.alias()))) {
            qWarning() << "Warning: could not remove temporary repository!" << updatePath;
        }
        setStatus(Status::Idle);
        return nullptr;
    }

    zypp_prepare_pool(m_zypp, manager);

    // tell the solver what we want
    std::list< zypp::RepoInfo > repos;
    repos.insert(repos.begin(), repo);

    // Mark packages for installation
    for (const QString &package : installPackages) {
        markPackage(repos, package.toStdString(), PackageOperation::InstallOrUpdate, force);
    }
    // Package removal?
    if (QFile::exists(QStringLiteral("%1%2remove.json").arg(packagesDir.path(), QDir::separator()))) {
        QFile removePackages(QStringLiteral("%1%2remove.json").arg(packagesDir.path(), QDir::separator()));
        if (!removePackages.open(QIODevice::ReadOnly | QIODevice::Text)) {
            sendErrorReply(QDBusError::errorString(QDBusError::InternalError),
                            QStringLiteral("Contents of the package appear to be corrupted or invalid."));
            if (!removeRepositoryInternal(QString::fromStdString(repo.alias()))) {
                qWarning() << "Warning: could not remove temporary repository!";
            }
            setStatus(Status::Idle);
            return nullptr;
        }

        QJsonObject rp = QJsonDocument::fromJson(removePackages.readAll()).object();

        for (const QVariant &package : rp.value(QStringLiteral("removePackages")).toArray().toVariantList()) {
            markPackage(repos, package.toString().toStdString(), PackageOperation::Remove, force);
        }
    }

    return manager;
}

void ZyppBackend::updateSystem(const QString &updatePath)
{
    CHECK_DBUS_CALLER_VOID
    ENQUEUE_OPERATION

    setStatus(Status::Processing);

    // Delay our reply
    setDelayedReply(true);

    // Add packages
    zypp::RepoManager *manager = prepareLocalRepositoryTransaction(updatePath);

    if (!manager) {
        qWarning() << "Could not prepare local repository transaction!";
        QDBusConnection::systemBus().send(request.createErrorReply(QDBusError::errorString(QDBusError::InternalError),
                                                                   QStringLiteral("Could not prepare local repository transaction!")));
        return;
    }

    // Set resolver options
    m_zypp->resolver()->setUpgradeMode(true);
    // Create our commit policy
    zypp::ZYppCommitPolicy policy;
    policy = policy.rpmExcludeDocs(true);

    m_progressAvailableSteps = static_cast<uint>(Hemera::SoftwareManagement::ProgressReporter::OperationStep::Process);
    m_progressOperationType = static_cast<uint>(Hemera::SoftwareManagement::ProgressReporter::OperationType::UpdateSystem);
    Hemera::Operation *op = new ZyppCommitOperation(m_zypp, this, manager, policy, this);
    HANDLE_OPERATION_DBUS(op)
    connect(op, &Hemera::Operation::finished, [this, manager] {
        // We remove our repo, before being done with this.
        if (!removeRepositoryInternal(QString::fromStdString(manager->getRepo(TMP_RPM_REPO_ALIAS).alias()))) {
            qWarning() << "Warning: could not remove temporary repository!";
        }
        delete manager;
    });
}

QByteArray ZyppBackend::listUpdates()
{
    CHECK_DBUS_CALLER(QByteArray)
    ENQUEUE_OPERATION

    setStatus(Status::Processing);

    // Always delay our reply.
    setDelayedReply(true);

    zypp::RepoManager manager;

    zypp_prepare_pool(m_zypp, &manager);

    // Set resolver options
    m_zypp->resolver()->setUpgradeMode(false);

    qDebug() << "Invoking the solver!";
    if (!m_zypp->resolver()->resolvePool()) {
        qWarning() << "Could not resolve the pool!";

        QDBusMessage reply = request.createErrorReply(QDBusError::errorString(QDBusError::InternalError), QStringLiteral("Could not resolve the pool"));
        QDBusConnection::systemBus().send(reply);

        setStatus(Status::Idle);
        return QByteArray();
    }

    // If we got here, we're ready to list.
    const zypp::ResPool &pool = m_zypp->pool();
    m_zypp->resolver()->doUpdate();

    // We have a special ha- prefix for hemera application packages
    std::string hemeraAppPrefix = "ha-";

    // Cache applicationIds to be matched
    QHash< QString, QString > trimmedIds;
    QDir hemeraServices(StaticConfig::hemeraServicesPath());
    hemeraServices.setFilter(QDir::Files | QDir::NoSymLinks);
    for (const QFileInfo &file : hemeraServices.entryInfoList(QStringList() << QStringLiteral("*.ha"))) {
        QString trimmed = file.completeBaseName();
        trimmed.remove(QLatin1Char('.'));
        trimmedIds.insert(trimmed, file.completeBaseName());
    }

    // Cache app updates
    Hemera::SoftwareManagement::ApplicationUpdates applicationUpdates;

    // Go
    for (zypp::ResPool::const_iterator it = pool.begin(); it != pool.end(); ++it) {
        zypp::PoolItem item = *it;
        zypp::ResObject::constPtr res = item.resolvable();
        // We care about package updates here.
        if (res->kind() != zypp::ResKind::package) {
            continue;
        }

        if (item.status().isToBeInstalled()) {
            // show every package picked by doUpdate for installation, if it's an application.
            if (std::equal(hemeraAppPrefix.begin(), hemeraAppPrefix.end(), res->name().begin())) {
                // Verify the existence of the corresponding application in the system
                QString packageName = QString::fromStdString(res->name());
                QString trimmedPackageName = packageName.right(packageName.length() - 3);
                trimmedPackageName.remove(QLatin1Char('.'));

                if (trimmedIds.contains(trimmedPackageName)) {
                    qDebug() << "Found application update for " << packageName << ", applicationId is" << trimmedIds.value(trimmedPackageName);
                } else {
                    qWarning() << "Found an update for" << packageName << ", but no matching application id has been found. This is quite strange. Skipping...";
                }

                // It's a hemera application. Construct the update.
                // Get the installed package first of all.
                zypp::ui::Selectable::constPtr s = zypp::ui::Selectable::get(res->kind(), res->name());
                zypp::ResObject::constPtr installed;
                if (s->hasInstalledObj()) {
                    installed = s->installedObj().resolvable();
                }

                using namespace Hemera::SoftwareManagement;

                ApplicationUpdate update = Constructors::applicationUpdateFromData(trimmedIds.value(trimmedPackageName),
                                                 QString::fromStdString(res->summary()),
                                                 QString::fromStdString(res->description()),
                                                 QString::fromStdString(installed->edition().asString()),
                                                 QString::fromStdString(res->edition().asString()),
                                                 res->downloadSize(),
                                                 // Compute installed size (for the update only)
                                                 s->hasInstalledObj() ? res->installSize().blocks(zypp::ByteCount::B) - installed->installSize().blocks(zypp::ByteCount::B)
                                                                      : res->installSize().blocks(zypp::ByteCount::B),
                                                 // TODO: How to handle changelog?
                                                 QString());

                applicationUpdates.append(update);
            }
        }
    }

    qDebug() << "Done." << applicationUpdates.size();

    m_zypp->resolver()->undo();
    m_zypp->resolver()->reset();

    // Send reply
    QDBusMessage reply = request.createReply(QVariantList() << QJsonDocument(Hemera::SoftwareManagement::Constructors::toJson(applicationUpdates)).toJson(QJsonDocument::Compact));
    QDBusConnection::systemBus().send(reply);

    // Done.
    setStatus(Status::Idle);

    // Who cares
    return QByteArray();
}

void ZyppBackend::installApplications(const QByteArray &applications)
{
    CHECK_DBUS_CALLER_VOID
    ENQUEUE_OPERATION

    using namespace Hemera::SoftwareManagement;

    setStatus(Status::Processing);

    setDelayedReply(true);

    QStringList packages;
    ApplicationPackages applicationPackages = Constructors::applicationPackagesFromJson(QJsonDocument::fromJson(applications).array());
    for (const ApplicationPackage &package : applicationPackages) {
        packages.append(package.applicationId());
    }

    m_progressAvailableSteps = static_cast<uint>(ProgressReporter::OperationStep::Download | ProgressReporter::OperationStep::Process);
    m_progressOperationType = static_cast<uint>(ProgressReporter::OperationType::InstallApplications);
    Hemera::Operation *op = new ZyppPackageOperation(m_zypp, this, packages, PackageOperation::Install, false, this);
    HANDLE_OPERATION_DBUS(op)
}

void ZyppBackend::installLocalPackage(const QString &package)
{
    CHECK_DBUS_CALLER_VOID
    ENQUEUE_OPERATION

    setStatus(Status::Processing);

    setDelayedReply(true);

    // Create temporary dir for the repo
    QTemporaryDir *dir = new QTemporaryDir(QStringLiteral("/var/tmp/hemera-zypp-worker-XXXXXX"));

    QString filename = package.split(QLatin1Char('/')).last();
    QString cachedPackage = QString::fromLatin1("%1/%2").arg(dir->path(), filename);

    // download the rpm into the cache
    QFile::copy(package, cachedPackage);

    zypp::RepoManager *manager = prepareLocalRepositoryTransaction(dir->path(), true);

    // Set resolver options
    m_zypp->resolver()->setUpgradeMode(false);
    // Create our commit policy
    zypp::ZYppCommitPolicy policy;
    policy = policy.rpmExcludeDocs(true);

    m_progressAvailableSteps = static_cast<uint>(Hemera::SoftwareManagement::ProgressReporter::OperationStep::Process);
    m_progressOperationType = static_cast<uint>(Hemera::SoftwareManagement::ProgressReporter::OperationType::InstallApplications);
    ZyppCommitOperation *op = new ZyppCommitOperation(m_zypp, this, manager, policy, this);
    connect(op, &Hemera::Operation::finished, [this, request, manager, op, dir] {
            if (op->isError()) {
                QDBusConnection::systemBus().send(request.createErrorReply(op->errorName(), op->errorMessage()));
            } else if (op->items() < 1) {
                QDBusConnection::systemBus().send(request.createErrorReply(Hemera::Literals::literal(Hemera::Literals::Errors::failedRequest()),
                                                                           QStringLiteral("No packages were processed, likely due to conflicts.")));
            } else {
                QDBusConnection::systemBus().send(request.createReply());
            }

            // We remove our repo, before being done with this.
            if (!removeRepositoryInternal(QString::fromStdString(manager->getRepo(TMP_RPM_REPO_ALIAS).alias()))) {
                qWarning() << "Warning: could not remove temporary repository!";
            }

            delete dir;
            delete manager;

            setStatus(Status::Idle);
    });
}

void ZyppBackend::removeApplications(const QByteArray &applications)
{
    CHECK_DBUS_CALLER_VOID
    ENQUEUE_OPERATION

    using namespace Hemera::SoftwareManagement;

    setStatus(Status::Processing);

    setDelayedReply(true);

    QStringList packages;
    ApplicationPackages applicationPackages = Constructors::applicationPackagesFromJson(QJsonDocument::fromJson(applications).array());
    for (const ApplicationPackage &package : applicationPackages) {
        packages.append(package.applicationId());
    }

    m_progressAvailableSteps = static_cast<uint>(ProgressReporter::OperationStep::Process);
    m_progressOperationType = static_cast<uint>(ProgressReporter::OperationType::RemoveApplications);
    Hemera::Operation *op = new ZyppPackageOperation(m_zypp, this, packages, PackageOperation::Remove, false, this);
    HANDLE_OPERATION_DBUS(op)
}

QByteArray ZyppBackend::listInstalledApplications()
{
    CHECK_DBUS_CALLER(QByteArray)
    ENQUEUE_OPERATION

    setStatus(Status::Processing);

    // Always delay our reply, we don't want to block.
    setDelayedReply(true);

    zypp::RepoManager manager;

    zypp_prepare_pool(m_zypp, &manager);

    // Set resolver options
    m_zypp->resolver()->setUpgradeMode(false);

    QStringList installedHa;
    QDir hemeraServices(StaticConfig::hemeraServicesPath());
    hemeraServices.setFilter(QDir::Files | QDir::NoSymLinks);

    Hemera::SoftwareManagement::ApplicationPackages packages;
    for (const QFileInfo &file : hemeraServices.entryInfoList(QStringList() << QStringLiteral("*.ha"))) {
        // Query for each package
        QString applicationId = file.completeBaseName();
        QString applicationIdTruncated = applicationId;
        applicationIdTruncated.remove(QLatin1Char('.'));
        std::string packageName = QString::fromLatin1("ha-%1").arg(applicationIdTruncated).toStdString();

        zypp::PoolQuery q;
        q.addKind(zypp::ResKind::package);
        q.setMatchGlob();
        // Only system packages
        q.setInstalledOnly();

        // Look for package names
        q.addDependency(zypp::sat::SolvAttr::name, packageName);

        zypp::PoolItemBest bestMatches(q.begin(), q.end());
        if (!bestMatches.empty()) {
            for (zypp::PoolItemBest::iterator sit = bestMatches.begin(); sit != bestMatches.end(); ++sit) {
                zypp::ui::Selectable::Ptr s(zypp::ui::asSelectable()(*sit));

                zypp::PoolItem instobj = zypp_get_installed_obj(s);
                if (instobj) {
                    zypp::ResObject::constPtr resolvable = instobj.resolvable();

                    using namespace Hemera::SoftwareManagement;

                    ApplicationPackage package = Constructors::applicationPackageFromData(applicationId, QString::fromStdString(resolvable->summary()),
                                                       QString::fromStdString(resolvable->description()), QUrl(), QString::fromStdString(resolvable->name()),
                                                       QString::fromStdString(resolvable->edition().asString()),
                                                       resolvable->downloadSize().blocks(zypp::ByteCount::B),
                                                       resolvable->installSize().blocks(zypp::ByteCount::B), true);
                    packages.append(package);
                } else {
                    qDebug() << "Failed to get installable object! Package was" << packageName.c_str();
                }
            }
        } else {
            qWarning() << "Package" << packageName.c_str() << "not found, even though a matching hemera service" << applicationId << "is installed!";
        }
    }

    // Send reply
    QDBusMessage reply = request.createReply(QVariantList() << QJsonDocument(Hemera::SoftwareManagement::Constructors::toJson(packages)).toJson(QJsonDocument::Compact));
    QDBusConnection::systemBus().send(reply);

    // Done.
    setStatus(Status::Idle);

    // Who cares
    return QByteArray();
}

QByteArray ZyppBackend::listRepositories()
{
    CHECK_DBUS_CALLER(QByteArray)
    ENQUEUE_OPERATION

    zypp::RepoManager manager;

    std::list<zypp::RepoInfo> repos;
    repos.insert(repos.end(), manager.repoBegin(), manager.repoEnd());
    qDebug() << "Found " << repos.size() << " repos.";

    QJsonArray repositories;

    for (std::list<zypp::RepoInfo>::const_iterator it = repos.begin(); it != repos.end(); ++it) {
        const zypp::RepoInfo repo(*it);

        if (it->enabled()) {
            QStringList urls;
            urls.reserve(repo.baseUrls().size());
            for (std::set<zypp::Url>::const_iterator uit = repo.baseUrls().begin(); uit != repo.baseUrls().end(); ++uit) {
                urls.append(QString::fromStdString((*uit).asCompleteString()));
            }

            using namespace Hemera::SoftwareManagement;
            repositories.append(Constructors::toJson(Constructors::repositoryFromData(QString::fromStdString(repo.alias()), urls)));
        } else {
            qDebug() << "Skipping disabled repo '" << repo.alias().c_str() << "'" << endl;
            continue;     // #217297
        }
    }

    return QJsonDocument(repositories).toJson(QJsonDocument::Compact);
}


// Operations
ZyppRefreshRepositoriesOperation::ZyppRefreshRepositoriesOperation(zypp::ZYpp::Ptr zypp, ZyppBackend *backend, CallbacksManager *callbacks, QObject *parent)
    : Hemera::Operation(parent)
    , m_zypp(zypp)
    , m_backend(backend)
    , m_callbacks(callbacks)
{
}

ZyppRefreshRepositoriesOperation::~ZyppRefreshRepositoriesOperation()
{
}

void ZyppRefreshRepositoriesOperation::startImpl()
{
    zypp::RepoManager manager;

    QString errorString = QStringLiteral("Errors: ");

    std::list<zypp::RepoInfo> repos;
    repos.insert(repos.end(), manager.repoBegin(), manager.repoEnd());
    qDebug() << "Found " << repos.size() << " repos.";

    // Set up callbacks and progress
    QDateTime transactionStart = QDateTime::currentDateTime();
    m_backend->m_progressOperationId = Workers::generateTransactionId(transactionStart);
    m_backend->m_progressCurrentStep = static_cast<uint>(Hemera::SoftwareManagement::ProgressReporter::OperationStep::NoStep);
    m_backend->m_progressStartDateTime = transactionStart.toMSecsSinceEpoch();
    m_backend->m_progressAvailableSteps = static_cast<uint>(Hemera::SoftwareManagement::ProgressReporter::OperationStep::Download);
    m_backend->m_progressOperationType = static_cast<uint>(Hemera::SoftwareManagement::ProgressReporter::OperationType::UpdateSystem);
    // Transaction type has changed
    Q_EMIT m_backend->progressOperationTypeChanged();

    m_callbacks->setOperationType(CallbacksManager::OperationType::Repository);
    m_callbacks->setTotalItems(repos.size());

    unsigned repocount = 0, errcount = 0;
    for (std::list<zypp::RepoInfo>::const_iterator it = repos.begin(); it != repos.end(); ++it, ++repocount) {
        zypp::Url url = it->url();
        std::string scheme(url.getScheme());

        if (scheme == "cd" || scheme == "dvd") {
            qDebug() << "Skipping CD/DVD repository: "
            "alias:[" << it->alias().c_str() << "] "
            "url:[" << url.asCompleteString().c_str() << "] ";
            m_callbacks->notifyRepositoryDone();
            continue;
        }

        // refresh only enabled repos with enabled autorefresh (bnc #410791)
        if (!(it->enabled() /*&& it->autorefresh()*/)) {
            qDebug() << "Skipping disabled/no-autorefresh repository: "
            "alias:[" << it->alias().c_str() << "] "
            "url:[" << url.asCompleteString().c_str() << "] ";
            m_callbacks->notifyRepositoryDone();
            continue;
        }

        try {
            manager.refreshMetadata(*it);
            manager.buildCache(*it);
        } catch (const zypp::Exception &excpt_r ) {
            qWarning() << " Error:" << endl
            << "Could not refresh repository " << it->name().c_str() << excpt_r.asUserString().c_str() << excpt_r.historyAsString().c_str();
            errorString.append(QString::fromStdString(excpt_r.asUserString().c_str()));
            ++errcount;
        }
        m_callbacks->notifyRepositoryDone();
    }

    if (errcount) {
        if (repocount == errcount) {
            // the whole operation failed (all of the repos)
            qDebug() << "The whole operation failed!";
            setFinishedWithError(QStringLiteral("Repositories could not be refreshed"), errorString);
            return;
        }

        if (repocount > errcount) {
            // some of the repos failed
            qDebug() << "Repositories successfully updated, some not";
            qWarning() << errorString;
        }
    } else {
        qDebug() << "Repositories successfully updated!";
    }

    setFinished();
}

ZyppPackageOperation::ZyppPackageOperation(zypp::ZYpp::Ptr zypp, ZyppBackend *backend, const QStringList &packages,
                                           ZyppBackend::PackageOperation operation, bool downloadOnly, QObject *parent)
    : Operation(parent)
    , m_zypp(zypp)
    , m_backend(backend)
    , m_packages(packages)
    , m_operation(operation)
    , m_downloadOnly(downloadOnly)
{
}

ZyppPackageOperation::~ZyppPackageOperation()
{

}


void ZyppPackageOperation::startImpl()
{
    zypp::RepoManager *manager = new zypp::RepoManager;

    zypp_prepare_pool(m_zypp, manager);

    // Set resolver options
    m_zypp->resolver()->setUpgradeMode(false);

    std::list<zypp::RepoInfo> repos;
    repos.insert(repos.end(), manager->repoBegin(), manager->repoEnd());
    // Mark each package for planned operation
    for (const QString &package : m_packages) {
        ZyppBackend::markPackage(repos, package.toStdString(), m_operation);
    }

    // Ok, we should have selected all that was needed. Now it's time to prepare and commit the transaction.
    // Set resolver options
    m_zypp->resolver()->setUpgradeMode(false);

    // Create our commit policy
    zypp::ZYppCommitPolicy policy;
    if (m_downloadOnly) {
        policy = policy.downloadMode(zypp::DownloadOnly);
    }
    policy.rpmExcludeDocs(true);

    connect(new ZyppCommitOperation(m_zypp, m_backend, manager, policy, this), &Hemera::Operation::finished, [this, manager] (Hemera::Operation *op) {
            if (op->isError()) {
                setFinishedWithError(op->errorName(), op->errorMessage());
            } else {
                setFinished();
            }

            delete manager;
    });
}

ZyppCommitOperation::ZyppCommitOperation(zypp::ZYpp::Ptr zypp, ZyppBackend* backend, zypp::RepoManager *manager, zypp::ZYppCommitPolicy commitPolicy, QObject* parent)
    : Operation(parent)
    , m_zypp(zypp)
    , m_backend(backend)
    , m_manager(manager)
    , m_policy(commitPolicy)
{
}

ZyppCommitOperation::~ZyppCommitOperation()
{
}

void ZyppCommitOperation::startImpl()
{
    // Transaction starts now. Let's generate it!
    QDateTime transactionStart = QDateTime::currentDateTime();
    m_backend->m_progressOperationId = Workers::generateTransactionId(transactionStart);
    m_backend->m_progressCurrentStep = static_cast<uint>(Hemera::SoftwareManagement::ProgressReporter::OperationStep::NoStep);
    m_backend->m_progressStartDateTime = transactionStart.toMSecsSinceEpoch();
    // Transaction type has changed
    Q_EMIT m_backend->progressOperationTypeChanged();

    // Call the solver
    qDebug() << "Invoking the solver!";
    if (!m_zypp->resolver()->resolvePool()) {
        qWarning() << "Could not resolve the pool!";
        setFinishedWithError(QStringLiteral("Could not resolve the pool!"), QString());
        return;
    }

    qDebug() << "Solver found a solution!";

    // COMMIT
    // TODO: Confirm licenses

    try {
        qDebug() << "committing transaction";

        // Give information to our callbacks manager. This also gives more information to the transaction types.
        m_items = m_backend->configureCallbacksManager(m_zypp->resolver()->getTransaction());

        // WARNING: This blocks the fuck out of everything!
        zypp::ZYppCommitResult result = m_zypp->commit(m_policy);

        if (!result.noError()) {
            setFinishedWithError(QStringLiteral("Committing transaction failed!"), QString());
            return;
        }

        // TODO: Handle messages
        //show_update_messages(zypper, result.updateMessages());
    } catch (const zypp::media::MediaException &e) {
        ZYPP_CAUGHT(e);
        setFinishedWithError(QString::fromStdString(e.asUserString()), QString::fromStdString(e.asUserHistory()));
        return;
    } catch (zypp::repo::RepoException &e) {
        ZYPP_CAUGHT(e);

        bool refresh_needed = false;
        try {
            if (!e.info().baseUrlsEmpty()) {
                for (zypp::RepoInfo::urls_const_iterator it = e.info().baseUrlsBegin(); it != e.info().baseUrlsEnd(); ++it) {
                    zypp::RepoManager::RefreshCheckStatus stat = m_manager->checkIfToRefreshMetadata(e.info(), *it, zypp::RepoManager::RefreshForced);
                    if (stat == zypp::RepoManager::REFRESH_NEEDED) {
                        refresh_needed = true;
                        break;
                    }
                }
            }
        } catch (const zypp::Exception &) {
            qDebug() << "check if to refresh exception caught, ignoring" << endl;
        }

        if (refresh_needed) {
            setFinishedWithError(QStringLiteral("Repositories need to be refreshed first."), QString::fromStdString(e.asUserHistory()));
        } else {
            setFinishedWithError(QString::fromStdString(e.asUserString()), QString::fromStdString(e.asUserHistory()));
        }

        return;
    } catch (const zypp::FileCheckException &e) {
        ZYPP_CAUGHT(e);
        setFinishedWithError(QString::fromStdString(e.asUserString()), QString::fromStdString(e.asUserHistory()));
//                 zypper.out().error(e,
//                     _("The package integrity check failed. This may be a problem"
//                     " with the repository or media. Try one of the following:\n"
//                     "\n"
//                     "- just retry previous command\n"
//                     "- refresh the repositories using 'zypper refresh'\n"
//                     "- use another installation medium (if e.g. damaged)\n"
//                     "- use another repository"));
//                 zypper.setExitCode(ZYPPER_EXIT_ERR_ZYPP);
        return;
    } catch (const zypp::Exception &e) {
        ZYPP_CAUGHT(e);
        setFinishedWithError(QString::fromStdString(e.asUserString()), QString::fromStdString(e.asUserHistory()));
    }

    // TODO: Handle reboot? We should only if we are updating system, regardless.
    // TODO: Handle restart? Probably yes.
    // TODO: Zypper is clever enough to restart affected processes. Maybe we should.

    m_backend->resetCallbacksManager();

    // After a commit, we indeed want to quit right after to clean up zypp.
    // FIXME: This is probably due to a slight misuse of zypp. Not critical, but would be nice to investigate in the future.
    // To reproduce: install a package, try to install the same package right after.
    // Would be nice to fix as it invalidates the operation queue.
    m_backend->m_timebomb->setInterval(100);

    setFinished();
}

int ZyppCommitOperation::items() const
{
    return m_items;
}


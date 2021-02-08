#include "zyppworkercallbacks.h"

#include "zyppbackend.h"

#include <zypp/PoolItem.h>

#include <QtCore/QDebug>
#include <QtCore/QElapsedTimer>

#define MSECS_RATE_LIMIT 300


CallbacksManager::CallbacksManager(ZyppBackend *backend)
    // Zypp callbacks
    : m_mediaChangeReport(MediaChangeReportReceiver(this))
    , m_mediaDownloadReport(DownloadProgressReportReceiver(this))
    , m_mediaAuthenticationReport(AuthenticationReportReceiver(this))
    , m_installReceiver(InstallResolvableReportReceiver(this))
    , m_removeReceiver(RemoveResolvableReportReceiver(this))
    , m_repoReport(RepoReportReceiver(this))
    , m_downloadReport(DownloadResolvableReportReceiver(this))
    , m_progressReport(ProgressReportReceiver(this))
    , m_keyRingReport(KeyRingReceive(this))
    , m_digestReport(DigestReceive(this))
    // The rest
    , m_backend(backend)
    , m_progressActive(false)
    , m_operationType(OperationType::NoOperation)
    , m_operationStep(Hemera::SoftwareManagement::ProgressReporter::OperationStep::NoStep)
    , m_items(0)
    , m_downloadSize(0)
{
    // Connect all
    m_digestReport.connect();
    m_keyRingReport.connect();
    m_mediaAuthenticationReport.connect();
    m_mediaChangeReport.connect();

    m_rateLimiter.start();
}

CallbacksManager::~CallbacksManager()
{
    // Disconnect all
    m_digestReport.disconnect();
    m_keyRingReport.disconnect();
    m_mediaAuthenticationReport.disconnect();
    m_mediaChangeReport.disconnect();

    setProgressStreamIsActive(false);
}

void CallbacksManager::setProgressStreamIsActive(bool active)
{
    if (active && !m_progressActive) {
        m_progressActive = true;

        // Connect all
        m_downloadReport.connect();
        m_installReceiver.connect();
        m_mediaDownloadReport.connect();
        m_progressReport.connect();
        m_removeReceiver.connect();
        m_repoReport.connect();
    }

    if (!active && m_progressActive) {
        // Disconnect all
        m_downloadReport.disconnect();
        m_installReceiver.disconnect();
        m_mediaDownloadReport.disconnect();
        m_progressReport.disconnect();
        m_removeReceiver.disconnect();
        m_repoReport.disconnect();
    }
}

void CallbacksManager::setTotalItems(quint64 items, quint64 downloadSize)
{
    m_items = items;
    m_downloadSize = downloadSize;
    m_downloaded = 0;
    m_processed = 0;
}

void CallbacksManager::setOperationType(CallbacksManager::OperationType type)
{
    m_operationType = type;
    m_operationStep = Hemera::SoftwareManagement::ProgressReporter::OperationStep::NoStep;
    m_items = 0;
    m_downloadSize = 0;
}

void CallbacksManager::notifyDownloadStart(quint64 size)
{
    if (m_operationType != OperationType::Package || m_operationType == OperationType::NoOperation) {
        // We disregard anything which is not a package
        return;
    }

    // Store the current size, we need it for computing the progress.
    m_currentItemSize = size;
    setCurrentStep(Hemera::SoftwareManagement::ProgressReporter::OperationStep::Download);
}

void CallbacksManager::notifyDownloadProgress(int percent, int rate)
{
    if (m_operationType != OperationType::Package || m_operationType == OperationType::NoOperation) {
        // We disregard anything which is not a package
        return;
    }

    // Compute our percentage and stream
    rateLimitAndStream(((percent / 100) * m_currentItemSize + m_downloaded) / m_downloadSize, rate);
}

void CallbacksManager::notifyDownloadFinish()
{
    if (m_operationType != OperationType::Package || m_operationType == OperationType::NoOperation) {
        // We disregard anything which is not a package
        return;
    }

    // Add to downloaded, and stream
    m_downloaded += m_currentItemSize;
    m_currentItemSize = 0;
    rateLimitAndStream(m_downloaded / m_downloadSize);
}

void CallbacksManager::notifyOperationProgress(int percent)
{
    if (m_operationType == OperationType::Repository || m_operationType == OperationType::NoOperation) {
        // We disregard repositories
        return;
    }

    setCurrentStep(Hemera::SoftwareManagement::ProgressReporter::OperationStep::Process);
    if (percent >= 0) {
        rateLimitAndStream((percent / m_items) + ((m_processed * 100) / m_items));
    } else {
        rateLimitAndStream((m_processed * 100) / m_items);
    }
}

void CallbacksManager::notifyOperationFinish()
{
    if (m_operationType == OperationType::Repository || m_operationType == OperationType::NoOperation) {
        // We disregard repositories
        return;
    }

    // Add to processed, and stream
    ++m_processed;
    rateLimitAndStream((m_processed * 100) / m_items);
}

void CallbacksManager::notifyRepositoryDone()
{
    if (m_operationType != OperationType::Repository || m_operationType == OperationType::NoOperation) {
        // We disregard non-repositories
        return;
    }

    // Add to processed, and stream
    ++m_processed;
    if (m_progressActive) {
        setCurrentStep(Hemera::SoftwareManagement::ProgressReporter::OperationStep::Process);
        rateLimitAndStream((m_processed * 100) / m_items);
    }
}

void CallbacksManager::rateLimitAndStream(int percent, int downloadRate)
{
    if (m_rateLimiter.elapsed() < MSECS_RATE_LIMIT && percent < 100) {
        // Rate limit our output, unless we're done. Don't be obsessed about it.
        return;
    } else {
        m_rateLimiter.start();
    }

    if (m_operationStep == Hemera::SoftwareManagement::ProgressReporter::OperationStep::Download) {
        qDebug() << "Download progress" << percent << " at rate" << downloadRate;
    } else {
        qDebug() << "Operation progress" << percent;
    }

    m_backend->m_progressPercent = percent;
    m_backend->m_progressRate = downloadRate;
    Q_EMIT m_backend->progressChanged();
}

void CallbacksManager::setCurrentStep(Hemera::SoftwareManagement::ProgressReporter::OperationStep step)
{
    if (step == m_operationStep) {
        return;
    }

    m_backend->m_progressCurrentStep = static_cast<uint>(step);
    m_operationStep = step;
    if (m_backend->m_progressPercent != 0 || m_backend->m_progressRate != 0) {
        m_backend->m_progressPercent = 0;
        m_backend->m_progressRate = 0;
        Q_EMIT m_backend->progressChanged();
    }

    Q_EMIT m_backend->progressCurrentStepChanged();
}


////////

bool AuthenticationReportReceiver::prompt(const zypp::Url& url, const std::string& description, zypp::media::AuthData& auth_data)
{
    qDebug() << Q_FUNC_INFO;
    return zypp::media::AuthenticationReport::prompt(url, description, auth_data);
}

void DownloadProgressReportReceiver::finish(const zypp::Url& uri, zypp::media::DownloadProgressReport::Error error, const std::string& description)
{
    m_manager->notifyDownloadFinish();
    zypp::media::DownloadProgressReport::finish(uri, error, description);
}

zypp::media::DownloadProgressReport::Action DownloadProgressReportReceiver::problem(const zypp::Url& uri, zypp::media::DownloadProgressReport::Error error,
                                                                                    const std::string& reason)
{
    qDebug() << Q_FUNC_INFO;
    return zypp::media::DownloadProgressReport::problem(uri, error, reason);
}

bool DownloadProgressReportReceiver::progress(int value, const zypp::Url& uri, double drate_avg, double drate_now)
{
    m_manager->notifyDownloadProgress(value, drate_now);
    return zypp::media::DownloadProgressReport::progress(value, uri, drate_avg, drate_now);
}

void DownloadProgressReportReceiver::start(const zypp::Url& uri, zypp::filesystem::Pathname localfile)
{
    // We ignore this. We care about what the rpm backend reports.
    zypp::media::DownloadProgressReport::start(uri, localfile);
}

zypp::repo::DownloadResolvableReport::Action DownloadResolvableReportReceiver::problem(zypp::Resolvable::constPtr resolvable,
                                                                                       zypp::repo::DownloadResolvableReport::Error error,
                                                                                       const std::string& description)
{
    qDebug() << Q_FUNC_INFO;
    return zypp::repo::DownloadResolvableReport::problem(resolvable, error, description);
}

bool DownloadResolvableReportReceiver::progress(int value, zypp::Resolvable::constPtr resolvable)
{
    qDebug() << Q_FUNC_INFO << value;
    return zypp::repo::DownloadResolvableReport::progress(value, resolvable);
}

void DownloadResolvableReportReceiver::start(zypp::Resolvable::constPtr resolvable, const zypp::Url& url)
{
    m_manager->notifyDownloadStart(resolvable->poolItem().resolvable()->downloadSize());
    zypp::repo::DownloadResolvableReport::start(resolvable, url);
}

void InstallResolvableReportReceiver::finish(zypp::Resolvable::constPtr resolvable, zypp::target::rpm::InstallResolvableReport::Error error,
                                             const std::string& reason, zypp::target::rpm::InstallResolvableReport::RpmLevel level)
{
    m_manager->notifyOperationFinish();
    zypp::target::rpm::InstallResolvableReport::finish(resolvable, error, reason, level);
}

zypp::target::rpm::InstallResolvableReport::Action InstallResolvableReportReceiver::problem(zypp::Resolvable::constPtr resolvable,
                                                                                            zypp::target::rpm::InstallResolvableReport::Error error,
                                                                                            const std::string& description,
                                                                                            zypp::target::rpm::InstallResolvableReport::RpmLevel level)
{
    qDebug() << Q_FUNC_INFO;
    return zypp::target::rpm::InstallResolvableReport::problem(resolvable, error, description, level);
}

bool InstallResolvableReportReceiver::progress(int value, zypp::Resolvable::constPtr resolvable)
{
    m_manager->notifyOperationProgress(value);
    return zypp::target::rpm::InstallResolvableReport::progress(value, resolvable);
}

void InstallResolvableReportReceiver::reportend()
{
    qDebug() << Q_FUNC_INFO;
    zypp::callback::ReceiveReport< zypp::target::rpm::InstallResolvableReport >::reportend();
}

void InstallResolvableReportReceiver::start(zypp::Resolvable::constPtr resolvable)
{
    zypp::target::rpm::InstallResolvableReport::start(resolvable);
}

zypp::media::MediaChangeReport::Action MediaChangeReportReceiver::requestMedia(zypp::Url& url, unsigned int mediumNr,
                                                                               const std::string& label, zypp::media::MediaChangeReport::Error error,
                                                                               const std::string& description, const std::vector< std::string >& devices,
                                                                               unsigned int& index)
{
    qDebug() << Q_FUNC_INFO;
    return zypp::media::MediaChangeReport::requestMedia(url, mediumNr, label, error, description, devices, index);
}

void ProgressReportReceiver::finish(const zypp::ProgressData& data)
{
    m_manager->notifyOperationFinish();
    zypp::ProgressReport::finish(data);
}

bool ProgressReportReceiver::progress(const zypp::ProgressData& data)
{
    m_manager->notifyOperationProgress(data.reportValue());
    return zypp::ProgressReport::progress(data);
}

void ProgressReportReceiver::start(const zypp::ProgressData& data)
{
    qDebug() << Q_FUNC_INFO << data.reportValue();
    zypp::ProgressReport::start(data);
}

void RemoveResolvableReportReceiver::finish(zypp::Resolvable::constPtr resolvable, zypp::target::rpm::RemoveResolvableReport::Error error,
                                            const std::string& reason)
{
    m_manager->notifyOperationFinish();
    zypp::target::rpm::RemoveResolvableReport::finish(resolvable, error, reason);
}

zypp::target::rpm::RemoveResolvableReport::Action RemoveResolvableReportReceiver::problem(zypp::Resolvable::constPtr resolvable,
                                                                                          zypp::target::rpm::RemoveResolvableReport::Error error,
                                                                                          const std::string& description)
{
    qDebug() << Q_FUNC_INFO;
    return zypp::target::rpm::RemoveResolvableReport::problem(resolvable, error, description);
}

bool RemoveResolvableReportReceiver::progress(int value, zypp::Resolvable::constPtr resolvable)
{
    m_manager->notifyOperationProgress(value);
    return zypp::target::rpm::RemoveResolvableReport::progress(value, resolvable);
}

void RemoveResolvableReportReceiver::reportend()
{
    qDebug() << Q_FUNC_INFO;
    zypp::callback::ReceiveReport< zypp::target::rpm::RemoveResolvableReport >::reportend();
}

void RemoveResolvableReportReceiver::start(zypp::Resolvable::constPtr resolvable)
{
    qDebug() << Q_FUNC_INFO;
    zypp::target::rpm::RemoveResolvableReport::start(resolvable);
}

void RepoReportReceiver::finish(zypp::Repository repo, const std::string& task, zypp::repo::RepoReport::Error error, const std::string& reason)
{
    m_manager->notifyOperationFinish();
    zypp::repo::RepoReport::finish(repo, task, error, reason);
}

zypp::repo::RepoReport::Action RepoReportReceiver::problem(zypp::Repository repo, zypp::repo::RepoReport::Error error, const std::string& description)
{
    qDebug() << Q_FUNC_INFO;
    return zypp::repo::RepoReport::problem(repo, error, description);
}

bool RepoReportReceiver::progress(const zypp::ProgressData& data)
{
    m_manager->notifyOperationProgress(data.reportValue());
    return zypp::repo::RepoReport::progress(data);
}

void RepoReportReceiver::start(const zypp::ProgressData& data, const zypp::RepoInfo repo)
{
    qDebug() << Q_FUNC_INFO << data.reportValue();
    zypp::repo::RepoReport::start(data, repo);
}

#ifndef ZYPPCALLBACKS_H
#define ZYPPCALLBACKS_H

#include <zypp/ZYppCallbacks.h>

#include <zypp/Digest.h>
#include <zypp/KeyRing.h>

#include <zypp/sat/Queue.h>

#include <QtCore/QElapsedTimer>

#include <HemeraSoftwareManagement/ProgressReporter>

#include <iostream>

class ZyppBackend;
class ZyppRefreshRepositoriesOperation;
class CallbacksManager;

static bool readCallbackAnswer() { return false; }

struct KeyRingReceive : public zypp::callback::ReceiveReport<zypp::KeyRingReport>
{
    explicit KeyRingReceive(CallbacksManager *manager) : m_manager(manager) {}
    ~KeyRingReceive() {}

    virtual bool askUserToAcceptUnsignedFile( const std::string &file, const zypp::KeyContext & context )
    { std::cerr << ". Error:" << std::endl << "refusing unsigned file " << file << std::endl;  return readCallbackAnswer(); }
    virtual bool askUserToAcceptUnknownKey( const std::string &file, const std::string &id, const zypp::KeyContext & context )
    { std::cerr << ". Error:" << std::endl << "refusing unknown key, id: '" << id << "' from file '" << file << "'" << std::endl; return readCallbackAnswer(); }
    virtual KeyRingReport::KeyTrust askUserToAcceptKey( const zypp::PublicKey &key, const zypp::KeyContext & context )
    { std::cerr << ". Error:" << std::endl << "not trusting key '" << key << "'" << std::endl; return KeyRingReport::KEY_DONT_TRUST; }
    virtual bool askUserToAcceptVerificationFailed( const std::string &file, const zypp::PublicKey &key, const zypp::KeyContext & context )
    { std::cerr << ". Error:" << std::endl << "verification of '" << file << "' with key '" << key << "' failed" << std::endl; return readCallbackAnswer(); }

private:
    CallbacksManager *m_manager;
};

struct DigestReceive : public zypp::callback::ReceiveReport<zypp::DigestReport>
{
    DigestReceive(CallbacksManager *manager) : m_manager(manager) {}
    ~DigestReceive() {}

    virtual bool askUserToAcceptNoDigest( const zypp::Pathname &file )
    { std::cerr << ". Error:" << std::endl << "refusing file '" << file << "': no digest" << std::endl; return readCallbackAnswer(); }
    virtual bool askUserToAccepUnknownDigest( const zypp::Pathname &file, const std::string &name )
    { std::cerr << ". Error:" << std::endl << "refusing file '" << file << "': unknown digest" << std::endl; return readCallbackAnswer(); }
    virtual bool askUserToAcceptWrongDigest( const zypp::Pathname &file, const std::string &requested, const std::string &found )
    { std::cerr << ". Error:" << std::endl << "refusing file '" << file << "': wrong digest" << std::endl; return readCallbackAnswer(); }

private:
    CallbacksManager *m_manager;
};

struct MediaChangeReportReceiver : public zypp::callback::ReceiveReport<zypp::media::MediaChangeReport>
{
    explicit MediaChangeReportReceiver(CallbacksManager *manager) : m_manager(manager) {}
    ~MediaChangeReportReceiver() {}

    virtual zypp::media::MediaChangeReport::Action requestMedia(zypp::Url & url,
                unsigned                         mediumNr,
                const std::string &              label,
                MediaChangeReport::Error         error,
                const std::string &              description,
                const std::vector<std::string> & devices,
                unsigned int &                   index);

private:
    CallbacksManager *m_manager;
};

// progress for downloading a file
struct DownloadProgressReportReceiver : public zypp::callback::ReceiveReport<zypp::media::DownloadProgressReport>
{
    explicit DownloadProgressReportReceiver(CallbacksManager *manager) : m_manager(manager) {}
    ~DownloadProgressReportReceiver() {}

    virtual void start(const zypp::Url & uri, zypp::Pathname localfile) override final;
    virtual bool progress(int value, const zypp::Url & uri, double drate_avg, double drate_now) override final;
    virtual void finish(const zypp::Url &uri , Error error, const std::string &description) override final;
    virtual Action problem(const zypp::Url &uri, Error error, const std::string &reason) override final;

private:
    CallbacksManager *m_manager;
};

struct AuthenticationReportReceiver : public zypp::callback::ReceiveReport<zypp::media::AuthenticationReport>
{
    explicit AuthenticationReportReceiver(CallbacksManager *manager) : m_manager(manager) {}
    ~AuthenticationReportReceiver() {}

    virtual bool prompt(const zypp::Url & url,
                        const std::string & description,
                        zypp::media::AuthData & auth_data) override final;

private:
    CallbacksManager *m_manager;
};

struct RemoveResolvableReportReceiver : public zypp::callback::ReceiveReport<zypp::target::rpm::RemoveResolvableReport>
{
    explicit RemoveResolvableReportReceiver(CallbacksManager *manager) : m_manager(manager) {}
    ~RemoveResolvableReportReceiver() {}
    virtual void start(zypp::Resolvable::constPtr resolvable) override final;
    virtual bool progress(int value, zypp::Resolvable::constPtr resolvable) override final;
    virtual Action problem(zypp::Resolvable::constPtr resolvable, Error error, const std::string &description) override final;
    virtual void finish(zypp::Resolvable::constPtr /*resolvable*/, Error error, const std::string &reason) override final;
    virtual void reportend() override final;

private:
    CallbacksManager *m_manager;
};

struct InstallResolvableReportReceiver : public zypp::callback::ReceiveReport<zypp::target::rpm::InstallResolvableReport>
{
    explicit InstallResolvableReportReceiver(CallbacksManager *manager) : m_manager(manager) {}
    ~InstallResolvableReportReceiver() {}

    virtual void start(zypp::Resolvable::constPtr resolvable) override final;
    virtual bool progress(int value, zypp::Resolvable::constPtr resolvable) override final;
    virtual Action problem(zypp::Resolvable::constPtr resolvable, Error error, const std::string &description, RpmLevel /*unused*/) override final;
    virtual void finish(zypp::Resolvable::constPtr /*resolvable*/, Error error, const std::string &reason, RpmLevel /*unused*/) override final;
    virtual void reportend() override final;

private:
    CallbacksManager *m_manager;
};

struct DownloadResolvableReportReceiver : public zypp::callback::ReceiveReport<zypp::repo::DownloadResolvableReport>
{
    // This class is mostly about deltas and patches, we don't support them so screw that.
    explicit DownloadResolvableReportReceiver(CallbacksManager *manager) : m_manager(manager) {}
    ~DownloadResolvableReportReceiver() {}

    /* This is interesting because we have full resolvable data at hand here
     * The media backend has only the file URI
     */
    virtual void start( zypp::Resolvable::constPtr resolvable_ptr, const zypp::Url & url) override final;
    // return false if the download should be aborted right now
    virtual bool progress(int value, zypp::Resolvable::constPtr /*resolvable_ptr*/) override final;
    virtual Action problem(zypp::Resolvable::constPtr resolvable_ptr, Error error, const std::string &description) override final;

private:
    CallbacksManager *m_manager;
};

struct ProgressReportReceiver  : public zypp::callback::ReceiveReport<zypp::ProgressReport>
{
    explicit ProgressReportReceiver(CallbacksManager *manager) : m_manager(manager) {}
    ~ProgressReportReceiver() {}

    virtual void start(const zypp::ProgressData &data) override final;
    virtual bool progress(const zypp::ProgressData &data) override final;
//   virtual Action problem( zypp::Repository /*repo*/, Error error, const std::string & description ) override final;
    virtual void finish(const zypp::ProgressData &data) override final;

private:
    CallbacksManager *m_manager;
};


struct RepoReportReceiver : public zypp::callback::ReceiveReport<zypp::repo::RepoReport>
{
    explicit RepoReportReceiver(CallbacksManager *manager) : m_manager(manager) {}
    ~RepoReportReceiver() {}

    virtual void start(const zypp::ProgressData & pd, const zypp::RepoInfo repo) override final;
    virtual bool progress(const zypp::ProgressData & pd) override final;
    virtual Action problem(zypp::Repository /*repo*/, Error error, const std::string & description) override final;
    virtual void finish(zypp::Repository /*repo*/, const std::string &task, Error error, const std::string &reason) override final;

private:
    CallbacksManager *m_manager;
};

///////////////////////////////////////////////////////////////////

class CallbacksManager {
public:
    enum class OperationType : uint {
        NoOperation = 0,
        Repository,
        Package
    };

    CallbacksManager(ZyppBackend *backend);
    ~CallbacksManager();

    void setProgressStreamIsActive(bool active);

    void setCurrentStep(Hemera::SoftwareManagement::ProgressReporter::OperationStep step);
    void setOperationType(OperationType type);
    void setTotalItems(quint64 items, quint64 downloadSize = 0);

private:
    // Proxied callback functions, for convenience
    void notifyDownloadStart(quint64 size);
    void notifyDownloadProgress(int percent, int rate);
    void notifyDownloadFinish();

    void notifyOperationProgress(int percent);
    void notifyOperationFinish();

    void notifyRepositoryDone();

private:
    // Zypp callbacks handled here.
    MediaChangeReportReceiver m_mediaChangeReport;
    DownloadProgressReportReceiver m_mediaDownloadReport;
    AuthenticationReportReceiver m_mediaAuthenticationReport;
    InstallResolvableReportReceiver m_installReceiver;
    RemoveResolvableReportReceiver m_removeReceiver;
    RepoReportReceiver m_repoReport;
    DownloadResolvableReportReceiver m_downloadReport;
    ProgressReportReceiver m_progressReport;
    KeyRingReceive m_keyRingReport;
    DigestReceive m_digestReport;

    ZyppBackend *m_backend;

    bool m_progressActive;

    OperationType m_operationType;
    Hemera::SoftwareManagement::ProgressReporter::OperationStep m_operationStep;
    quint64 m_items;
    quint64 m_downloadSize;

    quint64 m_downloaded;
    quint64 m_processed;
    quint64 m_currentItemSize;

    QElapsedTimer m_rateLimiter;

    void rateLimitAndStream(int percent, int downloadRate = 0);

    // All of our friends.
    friend struct MediaChangeReportReceiver;
    friend struct DownloadProgressReportReceiver;
    friend struct AuthenticationReportReceiver;
    friend struct InstallResolvableReportReceiver;
    friend struct RemoveResolvableReportReceiver;
    friend struct RepoReportReceiver;
    friend struct DownloadResolvableReportReceiver;
    friend struct ProgressReportReceiver;
    friend struct KeyRingReceive;
    friend struct DigestReceive;

    friend class ZyppRefreshRepositoriesOperation;
};


#endif

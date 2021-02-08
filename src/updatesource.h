/*
 *
 */

#ifndef GRAVITY_UPDATESOURCE_H
#define GRAVITY_UPDATESOURCE_H

#include <HemeraCore/AsyncInitObject>

#include <QtCore/QUrl>

#include <HemeraSoftwareManagement/SystemUpdate>

namespace Hemera {
class UrlOperation;
}

class UpdateSourcePrivate;
class UpdateSource : public Hemera::AsyncInitObject
{
    Q_OBJECT
    Q_DISABLE_COPY(UpdateSource)
    Q_DECLARE_PRIVATE_D(d_h_ptr, UpdateSource)

public:
    explicit UpdateSource(QObject *parent);
    virtual ~UpdateSource();

    Hemera::SoftwareManagement::SystemUpdate updateMetadata() const;

public Q_SLOTS:
    virtual Hemera::Operation *checkForUpdates(Hemera::SoftwareManagement::SystemUpdate::UpdateType preferredUpdateType) = 0;
    virtual Hemera::UrlOperation *downloadAvailableUpdate() = 0;

protected:
    void setUpdate(const Hemera::SoftwareManagement::SystemUpdate &updateMetadata = Hemera::SoftwareManagement::SystemUpdate());

Q_SIGNALS:
    void updateChanged();
};

#endif // GRAVITY_UPDATESOURCE_H

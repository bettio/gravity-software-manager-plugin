/*
 *
 */

#include "updatesource.h"

#include <HemeraCore/Literals>
#include <HemeraCore/Operation>

#include <private/HemeraCore/hemeraasyncinitobject_p.h>

#include <QtCore/QDebug>
#include <QtCore/QUrl>

#include <QtDBus/QDBusConnection>
#include <QtDBus/QDBusConnectionInterface>
#include <QtDBus/QDBusInterface>
#include <QtDBus/QDBusPendingCall>
#include <QtDBus/QDBusServiceWatcher>

class UpdateSourcePrivate : public Hemera::AsyncInitObjectPrivate
{
protected:
    Q_DECLARE_PUBLIC(UpdateSource)
public:
    UpdateSourcePrivate(UpdateSource *q) : Hemera::AsyncInitObjectPrivate(q) {}
    virtual ~UpdateSourcePrivate() {}

    Hemera::SoftwareManagement::SystemUpdate updateMetadata;
};

UpdateSource::UpdateSource(QObject *parent)
    : Hemera::AsyncInitObject(*new UpdateSourcePrivate(this), parent)
{
}

UpdateSource::~UpdateSource()
{
}

Hemera::SoftwareManagement::SystemUpdate UpdateSource::updateMetadata() const
{
    Q_D(const UpdateSource);
    return d->updateMetadata;
}

void UpdateSource::setUpdate(const Hemera::SoftwareManagement::SystemUpdate &updateMetadata)
{
    Q_D(UpdateSource);

    if (d->updateMetadata == updateMetadata) {
        return;
    }

    d->updateMetadata = updateMetadata;

    Q_EMIT updateChanged();
}

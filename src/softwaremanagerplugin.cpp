/*
 *
 */

#include "softwaremanagerplugin.h"

#include "softwaremanagerinterface.h"
#include "progressinterface.h"

#include <GravitySupermassive/GalaxyManager>

#include <HemeraCore/Literals>
#include <HemeraCore/Operation>

#include <QtCore/QDebug>

#include <QtDBus/QDBusConnection>
#include <QtDBus/QDBusConnectionInterface>
#include <QtDBus/QDBusInterface>
#include <QtDBus/QDBusPendingCall>
#include <QtDBus/QDBusServiceWatcher>

namespace Gravity {

SoftwareManagerPlugin::SoftwareManagerPlugin()
    : Gravity::Plugin()
{
    setName(QStringLiteral("Hemera Device Info"));
}

SoftwareManagerPlugin::~SoftwareManagerPlugin()
{
}

void SoftwareManagerPlugin::unload()
{
    // Kill the interface
    m_interface->deleteLater();
    setUnloaded();
}

void SoftwareManagerPlugin::load()
{
    // There's not much we have to do here, to be fairly honest.
    m_interface = new SoftwareManagerInterface(galaxyManager(), this);
    connect(m_interface->init(), &Hemera::Operation::finished, [this] (Hemera::Operation *op) {
        if (op->isError()) {
            qWarning() << "SoftwareManager interface encountered an error. The Software Manager won't be available." << op->errorName() << op->errorMessage();
        } else {
            // Create the progress interface and initialize it. When it's ready, we are.
            ProgressInterface *progressInterface = ProgressInterface::instance();
            connect(progressInterface->init(), &Hemera::Operation::finished, [this] (Hemera::Operation *opp) {
                    if (opp->isError()) {
                        qWarning() << "SoftwareManager interface encountered an error. The Software Manager won't be available."
                                   << opp->errorName() << opp->errorMessage();
                    } else {
                        qDebug() << "SoftwareManager Plugin loaded";
                        setLoaded();
                    }
            });
        }
    });
}

}

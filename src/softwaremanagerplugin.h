/*
 *
 */

#ifndef GRAVITY_SOFTWAREMANAGERPLUGIN_H
#define GRAVITY_SOFTWAREMANAGERPLUGIN_H

#include <GravitySupermassive/Plugin>

class SoftwareManagerInterface;
namespace Hemera {
    class Operation;
}

namespace Gravity {

struct Orbit;

class SoftwareManagerPlugin : public Gravity::Plugin
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID "com.ispirata.Hemera.GravityCenter.Plugin")
    Q_CLASSINFO("D-Bus Interface", "com.ispirata.Hemera.GravityCenter.Plugins.SoftwareManager")
    Q_INTERFACES(Gravity::Plugin)

public:
    explicit SoftwareManagerPlugin();
    virtual ~SoftwareManagerPlugin();

protected:
    virtual void unload() override final;
    virtual void load() override final;

private:
    SoftwareManagerInterface *m_interface;
};
}

#endif // GRAVITY_SOFTWAREMANAGERPLUGIN_H

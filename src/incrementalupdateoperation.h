#ifndef INCREMENTAL_UPDATE_OPERATION_H
#define INCREMENTAL_UPDATE_OPERATION_H

#include "updateoperation.h"

namespace Gravity {
class GalaxyManager;
}

class IncrementalUpdateOperation : public UpdateOperation
{
    Q_OBJECT
    Q_DISABLE_COPY(IncrementalUpdateOperation)

public:
    explicit IncrementalUpdateOperation(Gravity::GalaxyManager *manager, const QString &updateOrbit,
                                        const Hemera::SoftwareManagement::SystemUpdate &systemUpdate,
                                        const QByteArray &encryptionKey, QObject *parent = nullptr);
    virtual ~IncrementalUpdateOperation();

protected:
    virtual void startImpl() override final;

private:
    void performIncrementalPackageUpdate(const QString &packagePath);

    Gravity::GalaxyManager *m_manager;
    QString m_updateOrbit;
};

#endif // INCREMENTAL_UPDATE_OPERATION_H

#ifndef RECOVERY_UPDATE_OPERATION_H
#define RECOVERY_UPDATE_OPERATION_H

#include "updateoperation.h"

class RecoveryUpdateOperation : public UpdateOperation
{
    Q_OBJECT
    Q_DISABLE_COPY(RecoveryUpdateOperation)

public:
    explicit RecoveryUpdateOperation(const Hemera::SoftwareManagement::SystemUpdate &systemUpdate, const QByteArray &encryptionKey, QObject *parent = nullptr);
    virtual ~RecoveryUpdateOperation();

protected:
    virtual void startImpl() override final;
};

#endif // RECOVERY_UPDATE_OPERATION_H

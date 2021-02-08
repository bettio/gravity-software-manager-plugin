#ifndef UPDATE_OPERATION_H
#define UPDATE_OPERATION_H

#include <HemeraCore/Operation>

#include <HemeraSoftwareManagement/SystemUpdate>

class QDBusMessage;

class UpdateOperation : public Hemera::Operation
{
    Q_OBJECT
    Q_DISABLE_COPY(UpdateOperation)

public:
    explicit UpdateOperation(const Hemera::SoftwareManagement::SystemUpdate &systemUpdate, const QByteArray &encryptionKey, QObject *parent = nullptr);
    virtual ~UpdateOperation();

protected:
    virtual void startImpl() = 0;

    QString prepareSquashPackage(const QString &packagePath);
    bool unmountPackage(const QString &packagePath);

    QDBusMessage createBackendCall(const QString& method, const QVariantList& args);

    Hemera::SoftwareManagement::SystemUpdate m_systemUpdate;
    QByteArray m_encryptionKey;
};

#endif // UPDATE_OPERATION_H

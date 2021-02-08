/*
 *
 */

#ifndef GRAVITY_NETWORKENDPOINTUPDATESOURCE_H
#define GRAVITY_NETWORKENDPOINTUPDATESOURCE_H

#include "updatesource.h"

#include <QtCore/QUrl>

#include <QtNetwork/QSslConfiguration>

class QNetworkAccessManager;
class QNetworkRequest;

namespace Hemera {
class Operation;
}

class ImageStoreUpdateSourcePrivate;
class ImageStoreUpdateSource : public UpdateSource
{
    Q_OBJECT
    Q_DISABLE_COPY(ImageStoreUpdateSource)

public:
    explicit ImageStoreUpdateSource(const QUrl &endpointUrl, const QString &apiKey, QObject *parent = nullptr);
    virtual ~ImageStoreUpdateSource();

public Q_SLOTS:
    virtual Hemera::Operation *checkForUpdates(Hemera::SoftwareManagement::SystemUpdate::UpdateType preferredUpdateType) override final;
    virtual Hemera::UrlOperation *downloadAvailableUpdate() override final;

protected:
    virtual void initImpl() override final;

private:
    QByteArray astarteAPIKey();
    void setupRequestHeaders(QNetworkRequest *request);

    QUrl m_endpointUrl;
    QString m_apiKey;
    QString m_applianceName;
    QByteArray m_hardwareId;
    QByteArray m_astarteAPIKey;
    QNetworkAccessManager *m_nam;

    QJsonObject m_metadata;

    friend class CheckForImageStoreUpdatesOperation;
};

#endif // GRAVITY_NETWORKENDPOINTUPDATESOURCE_H

#ifndef WORKERSGLOBALHELPERS_H
#define WORKERSGLOBALHELPERS_H

#include <QtCore/QDateTime>
#include <QtCore/QUuid>

namespace Workers {

inline QByteArray generateTransactionId(const QDateTime &start) {
    QUuid uuid = QUuid::createUuid();
    QString transactionId = QString::fromLatin1("%1_%2").arg(start.toString(QStringLiteral("yyyyMMddHHmmss")), uuid.toString());
    return transactionId.toLatin1();
}

}

#endif

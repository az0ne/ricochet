/* Ricochet - https://ricochet.im/
 * Copyright (C) 2014, John Brooks <john.brooks@dereferenced.net>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *
 *    * Redistributions of source code must retain the above copyright
 *      notice, this list of conditions and the following disclaimer.
 *
 *    * Redistributions in binary form must reproduce the above
 *      copyright notice, this list of conditions and the following disclaimer
 *      in the documentation and/or other materials provided with the
 *      distribution.
 *
 *    * Neither the names of the copyright owners nor the names of its
 *      contributors may be used to endorse or promote products derived from
 *      this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "OutboundConnector.h"
#include "utils/Useful.h"
#include "tor/TorSocket.h"
#include "ControlChannel.h"
#include "AuthHiddenServiceChannel.h"

using namespace Protocol;

namespace Protocol
{

class OutboundConnectorPrivate : public QObject
{
    Q_OBJECT

public:
    OutboundConnector *q;
    Tor::TorSocket *socket;
    Connection *connection;
    QString hostname;
    quint16 port;
    OutboundConnector::Status status;
    CryptoKey authPrivateKey;
    QString errorMessage;
    QTimer errorRetryTimer;
    int errorRetryCount;

    OutboundConnectorPrivate(OutboundConnector *q)
        : QObject(q)
        , q(q)
        , socket(0)
        , connection(0)
        , port(0)
        , status(OutboundConnector::Inactive)
        , errorRetryCount(0)
    {
        connect(&errorRetryTimer, &QTimer::timeout, this, &OutboundConnectorPrivate::retryAfterError);
    }

    void setStatus(OutboundConnector::Status status);
    void setError(const QString &errorMessage);

public slots:
    void onConnected();
    void startAuthentication();
    void abort();
    void retryAfterError();
};

}

OutboundConnector::OutboundConnector(QObject *parent)
    : QObject(parent), d(new OutboundConnectorPrivate(this))
{
}

OutboundConnector::~OutboundConnector()
{
}

void OutboundConnector::setAuthPrivateKey(const CryptoKey &key)
{
    if (!key.isLoaded() || !key.isPrivate()) {
        BUG() << "Cannot make outbound connection without a valid private key";
        return;
    }

    d->authPrivateKey = key;
}

bool OutboundConnector::connectToHost(const QString &hostname, quint16 port)
{
    if (port <= 0 || hostname.isEmpty()) {
        d->errorMessage = QStringLiteral("Invalid hostname or port");
        d->setStatus(Error);
        return false;
    }

    if (d->status == Ready) {
        BUG() << "Reusing an OutboundConnector object";
        d->errorMessage = QStringLiteral("Outbound connection handler was already used");
        d->setStatus(Error);
        return false;
    }

    if (isActive() && hostname == d->hostname && port == d->port)
        return true;

    // There is no reason to be connecting to anything but onions for now, so add a safety net here
    if (!hostname.endsWith(QLatin1String(".onion"))) {
        d->errorMessage = QStringLiteral("Invalid (non-onion) hostname");
        d->setStatus(Error);
        return false;
    }

    abort();

    d->hostname = hostname;
    d->port = port;

    d->socket = new Tor::TorSocket(this);
    connect(d->socket, &Tor::TorSocket::connected, d, &OutboundConnectorPrivate::onConnected);
    d->setStatus(Connecting);
    d->socket->connectToHost(d->hostname, d->port);
    return true;
}

void OutboundConnector::abort()
{
    d->abort();
    d->hostname.clear();
    d->port = 0;
    d->errorRetryCount = 0;
    d->errorRetryTimer.stop();
    d->errorMessage.clear();
    d->setStatus(Inactive);
}

void OutboundConnectorPrivate::abort()
{
    if (connection) {
        connection->close();
        connection->deleteLater();
        connection = 0;
    }

    if (socket) {
        socket->disconnect(this);
        delete socket;
        socket = 0;
    }
}

OutboundConnector::Status OutboundConnector::status() const
{
    return d->status;
}

bool OutboundConnector::isActive() const
{
    return d->status > Inactive && d->status < Ready;
}

QString OutboundConnector::errorMessage() const
{
    return d->errorMessage;
}

Connection *OutboundConnector::takeConnection(QObject *newParent)
{
    if (status() != Ready || !d->connection) {
        BUG() << "Cannot take connection when not in the Ready state";
        return 0;
    }

    Q_ASSERT(newParent);
    Connection *c = d->connection;
    c->setParent(newParent);

    Q_ASSERT(!d->socket);
    d->connection = 0;
    d->setStatus(Inactive);

    return c;
}

void OutboundConnectorPrivate::setStatus(OutboundConnector::Status value)
{
    if (status == value)
        return;

    bool wasActive = q->isActive();
    status = value;
    emit q->statusChanged();
    if (wasActive != q->isActive())
        emit q->isActiveChanged();
}

void OutboundConnectorPrivate::setError(const QString &message)
{
    abort();
    errorMessage = message;
    setStatus(OutboundConnector::Error);

    // XXX This is a bad solution, but it will hold until we can revisit the
    // reconnecting and connection error behavior as a whole.
    if (++errorRetryCount > 5) {
        qDebug() << "Outbound connection attempt has had five errors in a row, stopping attempts";
        return;
    }

    errorRetryTimer.setSingleShot(true);
    errorRetryTimer.start(60 * 1000);
    qDebug() << "Retrying outbound connection attempt in 60 seconds after an error";
}

void OutboundConnectorPrivate::retryAfterError()
{
    if (status != OutboundConnector::Error) {
        qDebug() << "Error retry timer triggered, but not in an error state anymore. Ignoring.";
        return;
    }

    if (hostname.isEmpty() || port <= 0) {
        qDebug() << "Connection info cleared during error retry period, stopping OutboundConnector";
        q->abort();
        return;
    }

    q->connectToHost(hostname, port);
}

void OutboundConnectorPrivate::onConnected()
{
    if (!socket || status != OutboundConnector::Connecting) {
        BUG() << "OutboundConnector connected in an unexpected state";
        setError(QStringLiteral("Connected in an unexpected state"));
        return;
    }

    connection = new Connection(socket, Connection::ClientSide, q);

    // Socket is now owned by connection
    Q_ASSERT(socket->parent() == connection);
    socket->setReconnectEnabled(false);
    socket = 0;

    connect(connection, &Connection::ready, this, &OutboundConnectorPrivate::startAuthentication);
    // XXX Needs special treatment in UI (along with some other error types here)
    connect(connection, &Connection::versionNegotiationFailed, this,
        [this]() {
            setError(QStringLiteral("Protocol version negotiation failed with peer"));
        }
    );
    connect(connection, &Connection::oldVersionNegotiated, q, &OutboundConnector::oldVersionNegotiated);
    setStatus(OutboundConnector::Initializing);
}

void OutboundConnectorPrivate::startAuthentication()
{
    if (!connection || status != OutboundConnector::Initializing) {
        BUG() << "OutboundConnector startAuthentication in an unexpected state";
        setError(QStringLiteral("Connected in an unexpected state"));
        return;
    }

    if (!authPrivateKey.isLoaded() || !authPrivateKey.isPrivate()) {
        setError(QStringLiteral("Cannot authenticate outbound connection without a valid private key"));
        return;
    }

    // XXX Timeouts and errors and all of that
    AuthHiddenServiceChannel *authChannel = new AuthHiddenServiceChannel(Channel::Outbound, connection);
    connect(authChannel, &AuthHiddenServiceChannel::authSuccessful, this,
        [this]() {
            setStatus(OutboundConnector::Ready);
            emit q->ready();
        }
    );
    connect(authChannel, &AuthHiddenServiceChannel::authFailed, this,
        [this]() {
            qDebug() << "Authentication failed for outbound connection to" << hostname;
            setError(QStringLiteral("Authentication failed"));
        }
    );

    // Set the Authenticating state when we send the actual authentication message
    connect(authChannel, &Channel::channelOpened, this,
        [this]() {
            setStatus(OutboundConnector::Authenticating);
        }
    );

    authChannel->setPrivateKey(authPrivateKey);
    if (!authChannel->openChannel()) {
        setError(QStringLiteral("Unable to open authentication channel"));
    }
}

#include "OutboundConnector.moc"

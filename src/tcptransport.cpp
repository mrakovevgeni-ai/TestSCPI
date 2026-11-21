#include "tcptransport.h"

#include <QNetworkProxy>
#include <QTcpSocket>

#include <utility>

namespace scpi::detail {
namespace {

using Error = ScpiClient::Error;
constexpr qint64 readBufferSize = 64 * 1024;

} // namespace

TcpTransport::TcpTransport(QObject *parent) : QObject(parent)
{
}

TcpTransport::~TcpTransport()
{
    close();
}

void TcpTransport::open(const QHostAddress &address, quint16 port)
{
    Q_ASSERT(!m_socket);
    auto *source = new QTcpSocket(this);
    m_socket = source;
    source->setReadBufferSize(readBufferSize);
    source->setProxy(QNetworkProxy::NoProxy);

    connect(source, &QTcpSocket::connected, this, [this, source] {
        if (source == m_socket) {
            m_connected = true;
            emit connected();
        }
    });
    connect(source, &QTcpSocket::bytesWritten, this, [this, source](qint64 count) {
        if (source == m_socket && !m_outgoing.isEmpty()) {
            m_written += count;
            writePending(source);
        }
    });
    connect(source, &QTcpSocket::readyRead, this, [this, source] { readAvailable(source); });
    connect(source, &QTcpSocket::errorOccurred, this, [this, source](QAbstractSocket::SocketError error) {
        Error result = Error::SocketError;
        if (!m_connected)
            result = Error::ConnectionFailed;
        else if (error == QAbstractSocket::RemoteHostClosedError)
            result = Error::Disconnected;
        socketFailed(source, result, source->errorString());
    });
    connect(source, &QTcpSocket::disconnected, this, [this, source] {
        const auto error = m_connected ? Error::Disconnected : Error::ConnectionFailed;
        socketFailed(source, error, QStringLiteral("The TCP peer disconnected."));
    });

    source->connectToHost(address, port);
}

void TcpTransport::close()
{
    m_connected = false;
    m_outgoing.clear();
    m_submitted = 0;
    m_written = 0;
    if (auto *source = std::exchange(m_socket, nullptr)) {
        // A new connection always uses a different socket and signal source.
        disconnect(source, nullptr, this, nullptr);
        source->abort();
        source->deleteLater();
    }
}

bool TcpTransport::hasPendingInput() const
{
    return m_socket && m_socket->bytesAvailable() > 0;
}

void TcpTransport::write(QByteArray request)
{
    Q_ASSERT(m_socket && m_connected && m_outgoing.isEmpty() && !request.isEmpty());
    m_outgoing = std::move(request);
    writePending(m_socket);
}

void TcpTransport::writePending(QTcpSocket *source)
{
    const auto size = m_outgoing.size();
    while (m_submitted < size) {
        const auto count = source->write(m_outgoing.constData() + m_submitted, size - m_submitted);
        if (source != m_socket)
            return;
        if (count < 0 || (count == 0 && source->bytesToWrite() == 0)) {
            const auto detail = source->errorString();
            close();
            emit failed(Error::SocketError, detail);
            return;
        }
        if (count == 0)
            return;
        m_submitted += count;
    }

    if (m_written == size) {
        m_outgoing.clear();
        m_submitted = 0;
        m_written = 0;
        emit writeFinished();
    }
}

void TcpTransport::readAvailable(QTcpSocket *source)
{
    if (source == m_socket) {
        const auto bytes = source->readAll();
        if (!bytes.isEmpty())
            emit received(bytes);
    }
}

void TcpTransport::socketFailed(QTcpSocket *source, Error error, const QString &detail)
{
    if (source == m_socket && source->bytesAvailable() > 0)
        readAvailable(source);
    if (source == m_socket) {
        close();
        emit failed(error, detail);
    }
}

} // namespace scpi::detail

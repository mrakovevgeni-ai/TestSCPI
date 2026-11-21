#pragma once

#include <scpi/scpiclient.h>

class QTcpSocket;

namespace scpi::detail {

class TcpTransport final : public QObject
{
    Q_OBJECT

public:
    explicit TcpTransport(QObject *parent = nullptr);
    ~TcpTransport() override;

    void open(const QHostAddress &address, quint16 port);
    void close();
    void write(QByteArray request);
    bool hasPendingInput() const;

signals:
    void connected();
    void writeFinished();
    void received(const QByteArray &bytes);
    void failed(scpi::ScpiClient::Error error, const QString &detail);

private:
    void writePending(QTcpSocket *source);
    void readAvailable(QTcpSocket *source);
    void socketFailed(QTcpSocket *source, ScpiClient::Error error, const QString &detail);

    QTcpSocket *m_socket = nullptr;
    bool m_connected = false;
    QByteArray m_outgoing;
    qint64 m_submitted = 0;
    qint64 m_written = 0;
};

} // namespace scpi::detail

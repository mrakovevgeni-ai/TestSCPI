#pragma once

#include <scpi/scpi_export.h>

#include <QByteArray>
#include <QHostAddress>
#include <QObject>
#include <QString>

#include <memory>

namespace scpi {

class SCPI_EXPORT ScpiClient final : public QObject
{
    Q_OBJECT

public:
    using OperationId = quint64;

    static constexpr quint16 DefaultPort = 5025;
    static constexpr qsizetype DefaultMaximumResponseSize = 16 * 1024 * 1024;

    enum class SessionState { Closed, Connecting, Connected };
    Q_ENUM(SessionState)

    enum class Availability { Unknown, Available, Unavailable };
    Q_ENUM(Availability)

    enum class ResponseFormat { Text, IeeeBlockLf, IeeeBlockCrlf, IeeeBlockNoTerminator };
    Q_ENUM(ResponseFormat)

    enum class Error {
        None,
        InvalidEndpoint,
        InvalidCommand,
        NotConnected,
        ConnectionFailed,
        ConnectionTimeout,
        WriteTimeout,
        ResponseTimeout,
        AvailabilityTimeout,
        Disconnected,
        SocketError,
        MalformedResponse,
        UnsupportedFormat,
        ResponseTooLarge,
        UnexpectedData,
        UnexpectedResponse,
        SessionLost,
        Cancelled
    };
    Q_ENUM(Error)

    struct Timeouts {
        int connectMs = 3000;
        int responseMs = 3000;
        int availabilityMs = 1000;
    };

    explicit ScpiClient(QObject *parent = nullptr);
    ~ScpiClient() override;

    bool setEndpoint(const QHostAddress &address, quint16 port = DefaultPort);
    QHostAddress address() const;
    quint16 port() const;

    bool setTimeouts(Timeouts timeouts);
    Timeouts timeouts() const;
    bool setMaximumResponseSize(qsizetype bytes);
    qsizetype maximumResponseSize() const;

    bool setAvailabilityCommand(QByteArray command);
    QByteArray availabilityCommand() const;

    SessionState sessionState() const;
    Availability availability() const;
    bool isMonitoring() const;

    OperationId open();
    OperationId close();
    OperationId send(QByteArray command);
    OperationId sendConfirmed(QByteArray command);
    OperationId query(QByteArray command, ResponseFormat format = ResponseFormat::Text);
    OperationId checkAvailability();

    bool startMonitoring(int intervalMs);
    void stopMonitoring();

signals:
    void sessionStateChanged(scpi::ScpiClient::SessionState state);
    void availabilityChanged(scpi::ScpiClient::Availability availability);

    void operationFinished(quint64 id, const QByteArray &response);
    void operationFailed(quint64 id, scpi::ScpiClient::Error error, const QString &detail);
    void availabilityCheckFinished(quint64 id, bool available, scpi::ScpiClient::Error error,
                                   const QString &detail);

private:
    struct Private;
    std::unique_ptr<Private> d;
};

} // namespace scpi

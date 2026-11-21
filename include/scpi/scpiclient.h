#pragma once

#include <scpi/scpi_export.h>

#include <QByteArray>
#include <QHostAddress>
#include <QObject>
#include <QString>

#include <memory>

namespace scpi {

namespace detail {
class ScpiSession;
}

class SCPI_EXPORT ScpiClient final : public QObject
{
    Q_OBJECT

public:
    using OperationId = quint64;

    static constexpr quint16 DefaultPort = 5025;
    static constexpr qsizetype DefaultMaximumResponseSize = 16 * 1024 * 1024;

    enum class SessionState { Closed, Connecting, Connected, Reconnecting };
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
        Cancelled,
        InvalidMessageCount
    };
    Q_ENUM(Error)

    struct Timeouts {
        int connectMs = 3000;
        int responseMs = 3000;
        int availabilityMs = 1000;
    };

    struct RecoveryPolicy {
        bool enabled = true;
        int maxAttempts = 5;
        int initialDelayMs = 250;
        int maxDelayMs = 4000;
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
    bool setRecoveryPolicy(RecoveryPolicy policy);
    RecoveryPolicy recoveryPolicy() const;

    bool setAvailabilityCommand(QByteArray command);
    QByteArray availabilityCommand() const;

    SessionState sessionState() const;
    Availability availability() const;
    bool isMonitoring() const;

    OperationId open();
    OperationId close();
    OperationId send(QByteArray command);
    // A confirmed send waits for the reply to *OPC? in the same queued operation.
    OperationId send(QByteArray command, bool confirm);
    OperationId sendConfirmed(QByteArray command);
    OperationId query(QByteArray command, ResponseFormat format = ResponseFormat::Text);
    // Deliver the payload of all expected messages in one operationFinished signal.
    OperationId query(QByteArray command, ResponseFormat format, qsizetype messageCount);
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
    std::unique_ptr<detail::ScpiSession> d;
};

} // namespace scpi

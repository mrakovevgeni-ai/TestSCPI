#pragma once

#include "reconnectbackoff.h"
#include "scpioperation.h"
#include "tcptransport.h"

#include <QTimer>

#include <deque>
#include <vector>

namespace scpi::detail {

class ScpiSession final : public QObject
{
    using Id = ScpiClient::OperationId;
    using State = ScpiClient::SessionState;
    using Availability = ScpiClient::Availability;
    using Error = ScpiClient::Error;
    using Format = ScpiClient::ResponseFormat;
    using Timeouts = ScpiClient::Timeouts;
    using RecoveryPolicy = ScpiClient::RecoveryPolicy;

public:
    explicit ScpiSession(ScpiClient *client);

    bool setEndpoint(const QHostAddress &address, quint16 port);
    QHostAddress address() const { return m_address; }
    quint16 port() const { return m_port; }

    bool setTimeouts(Timeouts timeouts);
    Timeouts timeouts() const { return m_timeouts; }
    bool setMaximumResponseSize(qsizetype bytes);
    qsizetype maximumResponseSize() const { return m_maxResponseSize; }
    bool setRecoveryPolicy(RecoveryPolicy policy);
    RecoveryPolicy recoveryPolicy() const { return m_recoveryPolicy; }
    bool setAvailabilityCommand(QByteArray command);
    QByteArray availabilityCommand() const { return m_checkCommand; }

    State sessionState() const { return m_state; }
    Availability availability() const { return m_availability; }
    bool isMonitoring() const { return m_monitorTimer.isActive(); }

    Id open();
    Id close();
    Id submit(OperationKind kind, QByteArray command, Format format = Format::Text,
              qsizetype messageCount = 1);
    Id checkAvailability();
    bool startMonitoring(int intervalMs);
    void stopMonitoring();

private:
    enum class Role { Idle, Working, Probe };

    bool hasEndpoint() const;
    Operation checkOperation(Id id) const;
    void scheduleDispatch();
    void dispatch();
    void monitorTick();
    void start(Operation operation);

    void openTransport(Role role);
    void retireTransport();
    void transportConnected();
    void startWriting();
    void receive(const QByteArray &bytes);
    void written();
    void handleResult(std::optional<OperationResult> result);
    void failTransport(Error error, const QString &detail);
    bool scheduleRecovery();

    void armDeadline(QDeadlineTimer target, Error error);
    void clearDeadline();
    bool expireIfNeeded();

    void setState(State state);
    void setAvailability(Availability state);
    void reportOperation(Id id, Error error = Error::None, const QString &detail = {},
                         QByteArray response = {});
    void reportCheck(Id id, Error error, const QString &detail);
    void report(const Operation &operation, Error error, const QString &detail,
                QByteArray response = {});
    void finishOpenRequests(Error error = Error::None, const QString &detail = {});
    void failQueue(Error error, const QString &detail);
    void settleActive(Error error, const QString &detail, QByteArray response = {});
    void finishActive(Error error, const QString &detail = {}, QByteArray response = {});

    ScpiClient *m_client;
    QHostAddress m_address;
    quint16 m_port = ScpiClient::DefaultPort;
    Timeouts m_timeouts;
    RecoveryPolicy m_recoveryPolicy;
    qsizetype m_maxResponseSize = ScpiClient::DefaultMaximumResponseSize;
    QByteArray m_checkCommand = QByteArrayLiteral("*IDN?");

    State m_state = State::Closed;
    Availability m_availability = Availability::Unknown;
    TcpTransport m_transport;
    Role m_role = Role::Idle;

    QTimer m_deadlineTimer;
    QDeadlineTimer m_deadline;
    Error m_timeoutError = Error::None;
    QTimer m_monitorTimer;
    QTimer m_recoveryTimer;
    std::optional<ReconnectBackoff> m_backoff;

    Id m_nextId = 1;
    std::vector<Id> m_openRequests;
    std::deque<Operation> m_queue;
    std::optional<ActiveOperation> m_active;
    bool m_dispatchScheduled = false;
};

} // namespace scpi::detail

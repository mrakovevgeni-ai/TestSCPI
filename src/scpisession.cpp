#include "scpisession.h"

#include <QMetaObject>

#include <utility>

namespace scpi::detail {
namespace {

using Error = ScpiClient::Error;
constexpr ScpiClient::OperationId periodicCheck = 0;

QString timeoutDetail(Error error)
{
    switch (error) {
    case Error::ConnectionTimeout:
        return QStringLiteral("TCP connection timed out.");
    case Error::WriteTimeout:
        return QStringLiteral("Writing the SCPI command timed out.");
    case Error::ResponseTimeout:
        return QStringLiteral("SCPI request timed out.");
    case Error::AvailabilityTimeout:
        return QStringLiteral("SCPI availability check timed out.");
    default:
        Q_UNREACHABLE();
    }
}

} // namespace

ScpiSession::ScpiSession(ScpiClient *client)
    : QObject(client), m_client(client), m_transport(this), m_deadlineTimer(this),
      m_monitorTimer(this), m_recoveryTimer(this)
{
    m_deadlineTimer.setSingleShot(true);
    m_deadlineTimer.setTimerType(Qt::PreciseTimer);
    m_recoveryTimer.setSingleShot(true);
    m_recoveryTimer.setTimerType(Qt::PreciseTimer);

    connect(&m_deadlineTimer, &QTimer::timeout, this, [this] {
        if (m_timeoutError != Error::None && !expireIfNeeded())
            m_deadlineTimer.start(static_cast<int>(m_deadline.remainingTime()));
    });
    connect(&m_monitorTimer, &QTimer::timeout, this, &ScpiSession::monitorTick);
    connect(&m_recoveryTimer, &QTimer::timeout, this, &ScpiSession::dispatch);
    connect(&m_transport, &TcpTransport::connected, this, &ScpiSession::transportConnected);
    connect(&m_transport, &TcpTransport::received, this, &ScpiSession::receive);
    connect(&m_transport, &TcpTransport::writeFinished, this, &ScpiSession::written);
    connect(&m_transport, &TcpTransport::failed, this, [this](Error error, const QString &detail) {
        if (!expireIfNeeded())
            failTransport(error, detail);
    });
}

bool ScpiSession::setEndpoint(const QHostAddress &address, quint16 port)
{
    if (address.isNull() || port == 0)
        return false;
    if (m_address == address && m_port == port)
        return true;
    if (m_state != State::Closed || m_role != Role::Idle || !m_queue.empty())
        return false;

    m_address = address;
    m_port = port;
    setAvailability(Availability::Unknown);
    return true;
}

bool ScpiSession::setTimeouts(Timeouts timeouts)
{
    if (timeouts.connectMs <= 0 || timeouts.responseMs <= 0 || timeouts.availabilityMs <= 0)
        return false;
    m_timeouts = timeouts;
    return true;
}

bool ScpiSession::setMaximumResponseSize(qsizetype bytes)
{
    if (bytes <= 0)
        return false;
    m_maxResponseSize = bytes;
    return true;
}

bool ScpiSession::setRecoveryPolicy(RecoveryPolicy policy)
{
    if (!ReconnectBackoff::isValid(policy))
        return false;
    m_recoveryPolicy = policy;
    return true;
}

bool ScpiSession::setAvailabilityCommand(QByteArray command)
{
    command = normalizeCommand(std::move(command));
    if (command.isEmpty())
        return false;
    m_checkCommand = std::move(command);
    return true;
}

ScpiSession::Id ScpiSession::open()
{
    const auto id = m_nextId++;
    if (!hasEndpoint()) {
        reportOperation(id, Error::InvalidEndpoint, QStringLiteral("Set a valid IP address and TCP port first."));
    } else if (m_state == State::Connected) {
        reportOperation(id);
    } else {
        m_openRequests.push_back(id);
        if (m_state == State::Closed) {
            setState(State::Connecting);
            scheduleDispatch();
        }
    }
    return id;
}

ScpiSession::Id ScpiSession::close()
{
    const auto id = m_nextId++;
    if (m_state != State::Closed) {
        m_recoveryTimer.stop();
        m_backoff.reset();

        // Closing the working session leaves a previously started offline probe alone.
        const bool hasProbe = m_role == Role::Probe;
        if (!hasProbe)
            retireTransport();
        setState(State::Closed);

        const auto detail = QStringLiteral("The working SCPI session was closed.");
        if (!hasProbe && m_active)
            settleActive(Error::Cancelled, detail);
        finishOpenRequests(Error::Cancelled, detail);
        failQueue(Error::Cancelled, detail);
    }
    reportOperation(id);
    return id;
}

ScpiSession::Id ScpiSession::submit(OperationKind kind, QByteArray command, Format format,
                                   qsizetype messageCount)
{
    const auto id = m_nextId++;
    command = normalizeCommand(std::move(command));
    if (command.isEmpty()) {
        reportOperation(id, Error::InvalidCommand, QStringLiteral("Expected one nonempty SCPI command line."));
    } else if (!supportsFormat(format)) {
        reportOperation(id, Error::UnsupportedFormat, QStringLiteral("Unknown SCPI response format."));
    } else if (messageCount <= 0) {
        reportOperation(id, Error::InvalidMessageCount, QStringLiteral("Expected a positive SCPI message count."));
    } else if (m_state == State::Closed) {
        reportOperation(id, Error::NotConnected, QStringLiteral("The working SCPI session is closed."));
    } else {
        m_queue.push_back({id, kind, encodeCommand(std::move(command), kind), format, messageCount});
        scheduleDispatch();
    }
    return id;
}

ScpiSession::Id ScpiSession::checkAvailability()
{
    const auto id = m_nextId++;
    if (!hasEndpoint()) {
        reportCheck(id, Error::InvalidEndpoint, QStringLiteral("Set a valid IP address and TCP port first."));
    } else {
        m_queue.push_back(checkOperation(id));
        scheduleDispatch();
    }
    return id;
}

bool ScpiSession::startMonitoring(int intervalMs)
{
    if (intervalMs <= 0)
        return false;
    m_monitorTimer.start(intervalMs);
    return true;
}

void ScpiSession::stopMonitoring()
{
    m_monitorTimer.stop();
    if (m_role == Role::Probe && m_active->operation().id == periodicCheck
        && !m_active->startedWriting()) {
        finishActive(Error::Cancelled);
    }
}

bool ScpiSession::hasEndpoint() const
{
    return !m_address.isNull() && m_port != 0;
}

Operation ScpiSession::checkOperation(Id id) const
{
    return {id, OperationKind::Check, encodeCommand(m_checkCommand, OperationKind::Check)};
}

void ScpiSession::scheduleDispatch()
{
    if (m_dispatchScheduled)
        return;
    m_dispatchScheduled = true;
    QMetaObject::invokeMethod(this, [this] {
        m_dispatchScheduled = false;
        dispatch();
    }, Qt::QueuedConnection);
}

void ScpiSession::dispatch()
{
    if (m_active)
        return;
    if (m_state == State::Connecting || m_state == State::Reconnecting) {
        if (m_role == Role::Idle && !m_recoveryTimer.isActive())
            openTransport(Role::Working);
        return;
    }
    if (!m_queue.empty()) {
        auto operation = std::move(m_queue.front());
        m_queue.pop_front();
        start(std::move(operation));
    }
}

void ScpiSession::monitorTick()
{
    const bool idleState = m_state == State::Closed || m_state == State::Connected;
    if (idleState && !m_active && m_queue.empty() && hasEndpoint()) {
        m_queue.push_back(checkOperation(periodicCheck));
        dispatch();
    }
}

void ScpiSession::start(Operation operation)
{
    m_active.emplace(std::move(operation), m_timeouts, m_maxResponseSize);
    if (m_state == State::Closed) {
        Q_ASSERT(m_active->operation().kind == OperationKind::Check && m_role == Role::Idle);
        openTransport(Role::Probe);
    } else {
        Q_ASSERT(m_state == State::Connected && m_role == Role::Working);
        startWriting();
    }
}

void ScpiSession::openTransport(Role role)
{
    m_role = role;
    const QDeadlineTimer connectDeadline(m_timeouts.connectMs, Qt::PreciseTimer);
    if (role == Role::Probe && m_active->deadline() <= connectDeadline)
        armDeadline(m_active->deadline(), Error::AvailabilityTimeout);
    else
        armDeadline(connectDeadline, Error::ConnectionTimeout);
    m_transport.open(m_address, m_port);
}

void ScpiSession::retireTransport()
{
    clearDeadline();
    m_role = Role::Idle;
    m_transport.close();
}

void ScpiSession::transportConnected()
{
    if (expireIfNeeded())
        return;
    if (m_role == Role::Probe) {
        startWriting();
    } else {
        clearDeadline();
        m_backoff.reset();
        setState(State::Connected);
        finishOpenRequests();
        scheduleDispatch();
    }
}

void ScpiSession::startWriting()
{
    if (m_transport.hasPendingInput()) {
        failTransport(Error::UnexpectedData, QStringLiteral("Received data before sending a SCPI request."));
        return;
    }

    armDeadline(m_active->deadline(), m_active->timeoutError());
    if (!expireIfNeeded())
        m_transport.write(m_active->takeRequest());
}

void ScpiSession::receive(const QByteArray &bytes)
{
    if (expireIfNeeded())
        return;
    if (m_active)
        handleResult(m_active->receive(bytes));
    else
        failTransport(Error::UnexpectedData, QStringLiteral("Received data with no SCPI request pending."));
}

void ScpiSession::written()
{
    if (m_active && !expireIfNeeded())
        handleResult(m_active->written());
}

void ScpiSession::handleResult(std::optional<OperationResult> result)
{
    if (!result)
        return;
    if (result->framingLost)
        failTransport(result->error, result->detail);
    else
        finishActive(result->error, result->detail, std::move(result->response));
}

void ScpiSession::failTransport(Error error, const QString &detail)
{
    if (m_role == Role::Probe) {
        finishActive(error, detail);
        return;
    }

    const bool lostWorkingSession = m_state == State::Connected;
    retireTransport();
    const bool retrying = scheduleRecovery();
    if (m_active)
        settleActive(error, detail);
    if (!retrying)
        finishOpenRequests(error, detail);
    if (lostWorkingSession || !retrying)
        failQueue(Error::SessionLost, QStringLiteral("The working SCPI session was lost."));
}

bool ScpiSession::scheduleRecovery()
{
    if (!m_backoff)
        m_backoff.emplace(m_recoveryPolicy);
    if (const auto delay = m_backoff->nextDelay()) {
        setState(State::Reconnecting);
        m_recoveryTimer.start(*delay);
        return true;
    }

    m_backoff.reset();
    setState(State::Closed);
    return false;
}

void ScpiSession::armDeadline(QDeadlineTimer target, Error error)
{
    m_deadline = target;
    m_timeoutError = error;
    m_deadlineTimer.start(static_cast<int>(target.remainingTime()));
}

void ScpiSession::clearDeadline()
{
    m_timeoutError = Error::None;
    m_deadlineTimer.stop();
}

bool ScpiSession::expireIfNeeded()
{
    if (m_timeoutError == Error::None || !m_deadline.hasExpired())
        return false;
    failTransport(m_timeoutError, timeoutDetail(m_timeoutError));
    return true;
}

void ScpiSession::setState(State state)
{
    if (m_state != state) {
        m_state = state;
        QMetaObject::invokeMethod(m_client, [client = m_client, state] {
            emit client->sessionStateChanged(state);
        }, Qt::QueuedConnection);
    }
}

void ScpiSession::setAvailability(Availability state)
{
    if (m_availability != state) {
        m_availability = state;
        QMetaObject::invokeMethod(m_client, [client = m_client, state] {
            emit client->availabilityChanged(state);
        }, Qt::QueuedConnection);
    }
}

void ScpiSession::reportOperation(Id id, Error error, const QString &detail, QByteArray response)
{
    QMetaObject::invokeMethod(m_client, [client = m_client, id, error, detail,
                                         response = std::move(response)] {
        if (error == Error::None)
            emit client->operationFinished(id, response);
        else
            emit client->operationFailed(id, error, detail);
    }, Qt::QueuedConnection);
}

void ScpiSession::reportCheck(Id id, Error error, const QString &detail)
{
    if (id != periodicCheck) {
        QMetaObject::invokeMethod(m_client, [client = m_client, id, error, detail] {
            emit client->availabilityCheckFinished(id, error == Error::None, error, detail);
        }, Qt::QueuedConnection);
    }
}

void ScpiSession::report(const Operation &operation, Error error, const QString &detail,
                         QByteArray response)
{
    if (operation.kind == OperationKind::Check)
        reportCheck(operation.id, error, detail);
    else
        reportOperation(operation.id, error, detail, std::move(response));
}

void ScpiSession::finishOpenRequests(Error error, const QString &detail)
{
    for (Id id : m_openRequests)
        reportOperation(id, error, detail);
    m_openRequests.clear();
}

void ScpiSession::failQueue(Error error, const QString &detail)
{
    for (const auto &operation : m_queue)
        report(operation, error, detail);
    m_queue.clear();
}

void ScpiSession::settleActive(Error error, const QString &detail, QByteArray response)
{
    const auto &operation = m_active->operation();
    if (operation.kind == OperationKind::Check && error != Error::Cancelled)
        setAvailability(error == Error::None ? Availability::Available : Availability::Unavailable);
    report(operation, error, detail, std::move(response));
    m_active.reset();
}

void ScpiSession::finishActive(Error error, const QString &detail, QByteArray response)
{
    if (m_role == Role::Probe)
        retireTransport();
    else
        clearDeadline();
    settleActive(error, detail, std::move(response));
    scheduleDispatch();
}

} // namespace scpi::detail

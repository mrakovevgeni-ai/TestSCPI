#include <scpi/scpiclient.h>

#include "responseparser.h"

#include <QByteArrayView>
#include <QDeadlineTimer>
#include <QMetaObject>
#include <QNetworkProxy>
#include <QTcpSocket>
#include <QTimer>

#include <deque>
#include <optional>
#include <utility>
#include <vector>

namespace scpi {
namespace {

using OperationId = ScpiClient::OperationId;
using Format = ScpiClient::ResponseFormat;
using Error = ScpiClient::Error;

enum class Kind { Send, Confirmed, Query, Check };
enum class SocketRole { None, Working, Probe };

constexpr OperationId periodicCheckId = 0;
constexpr qint64 socketReadBufferSize = 64 * 1024;

struct Operation {
    OperationId id;
    Kind kind;
    Format format;
    QByteArray message;
};

QByteArray normalizeCommand(QByteArray command)
{
    if (command.endsWith('\n')) {
        command.chop(1);
        if (command.endsWith('\r'))
            command.chop(1);
    }
    if (QByteArrayView(command).trimmed().isEmpty() || command.contains('\n') || command.contains('\r'))
        return {};
    return command;
}

bool isSupported(Format format)
{
    switch (format) {
    case Format::Text:
    case Format::IeeeBlockLf:
    case Format::IeeeBlockCrlf:
    case Format::IeeeBlockNoTerminator:
        return true;
    }
    return false;
}

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

struct ScpiClient::Private {
    struct Active {
        explicit Active(Operation op) : operation(std::move(op)) {}

        Operation operation;
        std::optional<detail::ResponseParser> parser;
        std::optional<QByteArray> reply;
        QDeadlineTimer deadline;
        qint64 submitted = 0;
        qint64 written = 0;
    };

    explicit Private(ScpiClient *owner) : q(owner), deadlineTimer(owner), monitorTimer(owner)
    {
        deadlineTimer.setSingleShot(true);
        deadlineTimer.setTimerType(Qt::PreciseTimer);
        QObject::connect(&deadlineTimer, &QTimer::timeout, q, [this] {
            if (timeoutError != Error::None && !expireIfNeeded())
                deadlineTimer.start(static_cast<int>(deadline.remainingTime()));
        });
        QObject::connect(&monitorTimer, &QTimer::timeout, q, [this] { monitorTick(); });
    }

    ~Private()
    {
        retireSocket();
    }

    void setSession(SessionState state)
    {
        if (session != state) {
            session = state;
            QMetaObject::invokeMethod(q, [owner = q, state] {
                emit owner->sessionStateChanged(state);
            }, Qt::QueuedConnection);
        }
    }

    void setAvailability(Availability state)
    {
        if (availability != state) {
            availability = state;
            QMetaObject::invokeMethod(q, [owner = q, state] {
                emit owner->availabilityChanged(state);
            }, Qt::QueuedConnection);
        }
    }

    void reportOperation(OperationId id, Error error = Error::None, QString detail = {},
                         QByteArray response = {})
    {
        QMetaObject::invokeMethod(q, [owner = q, id, error, detail = std::move(detail),
                                      response = std::move(response)] {
            if (error == Error::None)
                emit owner->operationFinished(id, response);
            else
                emit owner->operationFailed(id, error, detail);
        }, Qt::QueuedConnection);
    }

    void reportCheck(OperationId id, Error error, QString detail)
    {
        if (id != periodicCheckId) {
            QMetaObject::invokeMethod(q, [owner = q, id, error, detail = std::move(detail)] {
                emit owner->availabilityCheckFinished(id, error == Error::None, error, detail);
            }, Qt::QueuedConnection);
        }
    }

    void report(const Operation &operation, Error error, const QString &detail,
                QByteArray response = {})
    {
        if (operation.kind == Kind::Check)
            reportCheck(operation.id, error, detail);
        else
            reportOperation(operation.id, error, detail, std::move(response));
    }

    void finishOpenRequests(Error error = Error::None, const QString &detail = {})
    {
        for (OperationId id : openRequests)
            reportOperation(id, error, detail);
        openRequests.clear();
    }

    bool hasEndpoint() const
    {
        return !address.isNull() && port != 0;
    }

    OperationId submit(Kind kind, QByteArray command, Format format = Format::Text)
    {
        const auto id = nextId++;
        command = normalizeCommand(std::move(command));
        if (command.isEmpty()) {
            reportOperation(id, Error::InvalidCommand, QStringLiteral("Expected one nonempty SCPI command line."));
            return id;
        }
        if (!isSupported(format)) {
            reportOperation(id, Error::UnsupportedFormat, QStringLiteral("Unknown SCPI response format."));
            return id;
        }
        if (session == SessionState::Closed) {
            reportOperation(id, Error::NotConnected, QStringLiteral("The working SCPI session is closed."));
            return id;
        }

        command.append('\n');
        if (kind == Kind::Confirmed)
            command.append("*OPC?\n");

        queue.push_back({id, kind, format, std::move(command)});
        scheduleNext();
        return id;
    }

    Operation checkOperation(OperationId id) const
    {
        return {id, Kind::Check, Format::Text, checkCommand + '\n'};
    }

    void scheduleNext()
    {
        if (dispatchScheduled)
            return;
        dispatchScheduled = true;
        QMetaObject::invokeMethod(q, [this] {
            dispatchScheduled = false;
            dispatch();
        }, Qt::QueuedConnection);
    }

    void dispatch()
    {
        if (active)
            return;
        if (session == SessionState::Connecting) {
            if (!socket)
                startConnection(SocketRole::Working);
            return;
        }
        if (!queue.empty()) {
            Operation operation = std::move(queue.front());
            queue.pop_front();
            start(std::move(operation));
        }
    }

    void monitorTick()
    {
        if (!active && queue.empty() && session != SessionState::Connecting && hasEndpoint()) {
            queue.push_back(checkOperation(periodicCheckId));
            dispatch();
        }
    }

    void start(Operation operation)
    {
        Q_ASSERT(!active);
        active.emplace(std::move(operation));
        const Kind kind = active->operation.kind;
        if (kind != Kind::Send)
            active->parser.emplace(active->operation.format, maxResponseSize);

        const int budget = kind == Kind::Check ? timeouts.availabilityMs : timeouts.responseMs;
        active->deadline = QDeadlineTimer(budget, Qt::PreciseTimer);

        if (session == SessionState::Closed) {
            Q_ASSERT(kind == Kind::Check && !socket);
            startConnection(SocketRole::Probe);
        } else {
            Q_ASSERT(session == SessionState::Connected && socketRole == SocketRole::Working);
            startWriting();
        }
    }

    void armDeadline(QDeadlineTimer target, Error error)
    {
        deadline = target;
        timeoutError = error;
        deadlineTimer.start(static_cast<int>(deadline.remainingTime()));
    }

    void clearDeadline()
    {
        timeoutError = Error::None;
        deadlineTimer.stop();
    }

    bool expireIfNeeded()
    {
        if (timeoutError == Error::None || !deadline.hasExpired())
            return false;
        failTransport(timeoutError, timeoutDetail(timeoutError));
        return true;
    }

    void startConnection(SocketRole role)
    {
        Q_ASSERT(!socket);
        auto *source = new QTcpSocket(q);
        socket = source;
        socketRole = role;
        source->setReadBufferSize(socketReadBufferSize);
        source->setProxy(QNetworkProxy::NoProxy);

        QObject::connect(source, &QTcpSocket::connected, q, [this, source] {
            if (source != socket || expireIfNeeded())
                return;
            transportConnected = true;
            if (socketRole == SocketRole::Probe) {
                startWriting();
            } else {
                clearDeadline();
                setSession(SessionState::Connected);
                finishOpenRequests();
                scheduleNext();
            }
        });
        QObject::connect(source, &QTcpSocket::bytesWritten, q, [this, source](qint64 count) {
            if (source == socket && active && !expireIfNeeded()) {
                active->written += count;
                writePending();
            }
        });
        QObject::connect(source, &QTcpSocket::readyRead, q, [this, source] { readAvailable(source); });
        QObject::connect(source, &QTcpSocket::errorOccurred, q, [this, source](QAbstractSocket::SocketError error) {
            if (source != socket)
                return;
            Error result = Error::SocketError;
            if (!transportConnected)
                result = Error::ConnectionFailed;
            else if (error == QAbstractSocket::RemoteHostClosedError)
                result = Error::Disconnected;
            socketFailed(source, result, source->errorString());
        });
        QObject::connect(source, &QTcpSocket::disconnected, q, [this, source] {
            const auto error = transportConnected ? Error::Disconnected : Error::ConnectionFailed;
            socketFailed(source, error, QStringLiteral("The TCP peer disconnected."));
        });

        const QDeadlineTimer connectDeadline(timeouts.connectMs, Qt::PreciseTimer);
        if (role == SocketRole::Probe && active->deadline <= connectDeadline)
            armDeadline(active->deadline, Error::AvailabilityTimeout);
        else
            armDeadline(connectDeadline, Error::ConnectionTimeout);

        source->connectToHost(address, port);
    }

    void retireSocket()
    {
        clearDeadline();
        socketRole = SocketRole::None;
        transportConnected = false;
        if (auto *source = std::exchange(socket, nullptr)) {
            // Any delayed events from this connection must stay with the retired socket.
            QObject::disconnect(source, nullptr, q, nullptr);
            source->abort();
            source->deleteLater();
        }
    }

    void startWriting()
    {
        if (socket->bytesAvailable() > 0) {
            failTransport(Error::UnexpectedData, QStringLiteral("Received data before sending a SCPI request."));
            return;
        }

        Error error = Error::ResponseTimeout;
        if (active->operation.kind == Kind::Check)
            error = Error::AvailabilityTimeout;
        else if (active->operation.kind == Kind::Send)
            error = Error::WriteTimeout;
        armDeadline(active->deadline, error);
        if (!expireIfNeeded())
            writePending();
    }

    void writePending()
    {
        const auto *data = active->operation.message.constData();
        const auto size = active->operation.message.size();
        auto *source = socket;
        while (active->submitted < size) {
            const qint64 count = source->write(data + active->submitted, size - active->submitted);
            if (source != socket)
                return;
            if (count < 0 || (count == 0 && source->bytesToWrite() == 0)) {
                failTransport(Error::SocketError, source->errorString());
                return;
            }
            if (count == 0)
                return;
            active->submitted += count;
        }
        tryFinish();
    }

    void readAvailable(QTcpSocket *source)
    {
        if (source != socket || expireIfNeeded())
            return;

        const QByteArray bytes = source->readAll();
        if (bytes.isEmpty())
            return;
        if (!active || !active->parser) {
            failTransport(Error::UnexpectedData, QStringLiteral("Received data with no SCPI response pending."));
            return;
        }

        auto result = active->parser->append(bytes);
        switch (result.status) {
        case detail::ParseStatus::NeedMore:
            return;
        case detail::ParseStatus::Error:
            failTransport(result.error, result.detail);
            return;
        case detail::ParseStatus::Complete:
            active->reply.emplace(std::move(result.payload));
            active->parser.reset();
            tryFinish();
            return;
        }
    }

    void socketFailed(QTcpSocket *source, Error error, const QString &detail)
    {
        if (source != socket || expireIfNeeded())
            return;
        if (source->bytesAvailable() > 0)
            readAvailable(source);
        if (source == socket)
            failTransport(error, detail);
    }

    void tryFinish()
    {
        if (active->written != active->operation.message.size())
            return;
        if (active->operation.kind == Kind::Send) {
            finish(Error::None);
            return;
        }
        if (!active->reply)
            return;

        const auto kind = active->operation.kind;
        const auto &reply = *active->reply;
        if (kind == Kind::Confirmed && reply != "1" && reply != "+1") {
            finish(Error::UnexpectedResponse, QStringLiteral("Expected a complete 1 or +1 reply to *OPC?."));
        } else if (kind == Kind::Check && QByteArrayView(reply).trimmed().isEmpty()) {
            finish(Error::UnexpectedResponse, QStringLiteral("The SCPI availability reply is empty."));
        } else {
            QByteArray payload;
            if (kind == Kind::Query)
                payload = std::move(*active->reply);
            finish(Error::None, {}, std::move(payload));
        }
    }

    void settleActive(Error error, const QString &detail, QByteArray response = {})
    {
        Operation operation = std::move(active->operation);
        active.reset();
        if (operation.kind == Kind::Check && error != Error::Cancelled)
            setAvailability(error == Error::None ? Availability::Available : Availability::Unavailable);
        report(operation, error, detail, std::move(response));
    }

    void finish(Error error, const QString &detail = {}, QByteArray response = {})
    {
        if (socketRole == SocketRole::Probe)
            retireSocket();
        else
            clearDeadline();
        settleActive(error, detail, std::move(response));
        scheduleNext();
    }

    void failTransport(Error error, const QString &detail)
    {
        if (socketRole == SocketRole::Probe)
            finish(error, detail);
        else
            endSession(error, detail);
    }

    void endSession(Error error, const QString &detail)
    {
        // A pending open may have been waiting for a previously started probe.
        const bool keepProbe = socketRole == SocketRole::Probe;
        if (!keepProbe)
            retireSocket();
        setSession(SessionState::Closed);
        if (!keepProbe && active)
            settleActive(error, detail);
        finishOpenRequests(error, detail);

        const bool cancelled = error == Error::Cancelled;
        const auto queuedError = cancelled ? Error::Cancelled : Error::SessionLost;
        const auto queuedDetail = cancelled ? detail : QStringLiteral("The working SCPI session was lost.");
        for (const auto &operation : queue)
            report(operation, queuedError, queuedDetail);
        queue.clear();
    }

    ScpiClient *q;
    QHostAddress address;
    quint16 port = DefaultPort;
    Timeouts timeouts;
    qsizetype maxResponseSize = DefaultMaximumResponseSize;
    QByteArray checkCommand = QByteArrayLiteral("*IDN?");

    SessionState session = SessionState::Closed;
    Availability availability = Availability::Unknown;

    QTcpSocket *socket = nullptr;
    SocketRole socketRole = SocketRole::None;
    bool transportConnected = false;
    QTimer deadlineTimer;
    QDeadlineTimer deadline;
    Error timeoutError = Error::None;
    QTimer monitorTimer;

    OperationId nextId = 1;
    std::vector<OperationId> openRequests;
    std::deque<Operation> queue;
    std::optional<Active> active;
    bool dispatchScheduled = false;
};

ScpiClient::ScpiClient(QObject *parent) : QObject(parent), d(std::make_unique<Private>(this))
{
}

ScpiClient::~ScpiClient() = default;

bool ScpiClient::setEndpoint(const QHostAddress &address, quint16 port)
{
    if (address.isNull() || port == 0)
        return false;
    if (d->address == address && d->port == port)
        return true;
    if (d->session != SessionState::Closed || d->socket || !d->queue.empty())
        return false;

    d->address = address;
    d->port = port;
    d->setAvailability(Availability::Unknown);
    return true;
}

QHostAddress ScpiClient::address() const
{
    return d->address;
}

quint16 ScpiClient::port() const
{
    return d->port;
}

bool ScpiClient::setTimeouts(Timeouts timeouts)
{
    if (timeouts.connectMs <= 0 || timeouts.responseMs <= 0 || timeouts.availabilityMs <= 0)
        return false;
    d->timeouts = timeouts;
    return true;
}

ScpiClient::Timeouts ScpiClient::timeouts() const
{
    return d->timeouts;
}

bool ScpiClient::setMaximumResponseSize(qsizetype bytes)
{
    if (bytes <= 0)
        return false;
    d->maxResponseSize = bytes;
    return true;
}

qsizetype ScpiClient::maximumResponseSize() const
{
    return d->maxResponseSize;
}

bool ScpiClient::setAvailabilityCommand(QByteArray command)
{
    command = normalizeCommand(std::move(command));
    if (command.isEmpty())
        return false;
    d->checkCommand = std::move(command);
    return true;
}

QByteArray ScpiClient::availabilityCommand() const
{
    return d->checkCommand;
}

ScpiClient::SessionState ScpiClient::sessionState() const
{
    return d->session;
}

ScpiClient::Availability ScpiClient::availability() const
{
    return d->availability;
}

bool ScpiClient::isMonitoring() const
{
    return d->monitorTimer.isActive();
}

ScpiClient::OperationId ScpiClient::open()
{
    const auto id = d->nextId++;
    if (!d->hasEndpoint()) {
        d->reportOperation(id, Error::InvalidEndpoint, QStringLiteral("Set a valid IP address and TCP port first."));
    } else if (d->session == SessionState::Connected) {
        d->reportOperation(id);
    } else {
        d->openRequests.push_back(id);
        d->setSession(SessionState::Connecting);
        d->scheduleNext();
    }
    return id;
}

ScpiClient::OperationId ScpiClient::close()
{
    const auto id = d->nextId++;
    if (d->session != SessionState::Closed)
        d->endSession(Error::Cancelled, QStringLiteral("The working SCPI session was closed."));
    d->reportOperation(id);
    return id;
}

ScpiClient::OperationId ScpiClient::send(QByteArray command)
{
    return d->submit(Kind::Send, std::move(command));
}

ScpiClient::OperationId ScpiClient::sendConfirmed(QByteArray command)
{
    return d->submit(Kind::Confirmed, std::move(command));
}

ScpiClient::OperationId ScpiClient::query(QByteArray command, ResponseFormat format)
{
    return d->submit(Kind::Query, std::move(command), format);
}

ScpiClient::OperationId ScpiClient::checkAvailability()
{
    const auto id = d->nextId++;
    if (!d->hasEndpoint()) {
        d->reportCheck(id, Error::InvalidEndpoint, QStringLiteral("Set a valid IP address and TCP port first."));
    } else {
        d->queue.push_back(d->checkOperation(id));
        d->scheduleNext();
    }
    return id;
}

bool ScpiClient::startMonitoring(int intervalMs)
{
    if (intervalMs <= 0)
        return false;
    d->monitorTimer.start(intervalMs);
    return true;
}

void ScpiClient::stopMonitoring()
{
    d->monitorTimer.stop();
    if (d->socketRole == SocketRole::Probe && d->active->operation.id == periodicCheckId
        && d->active->submitted == 0) {
        d->finish(Error::Cancelled);
    }
}

} // namespace scpi

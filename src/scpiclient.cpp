#include <scpi/scpiclient.h>

#include "scpisession.h"

#include <utility>

namespace scpi {

ScpiClient::ScpiClient(QObject *parent)
    : QObject(parent), d(std::make_unique<detail::ScpiSession>(this))
{
}

ScpiClient::~ScpiClient() = default;

bool ScpiClient::setEndpoint(const QHostAddress &address, quint16 port)
{
    return d->setEndpoint(address, port);
}

QHostAddress ScpiClient::address() const
{
    return d->address();
}

quint16 ScpiClient::port() const
{
    return d->port();
}

bool ScpiClient::setTimeouts(Timeouts timeouts)
{
    return d->setTimeouts(timeouts);
}

ScpiClient::Timeouts ScpiClient::timeouts() const
{
    return d->timeouts();
}

bool ScpiClient::setMaximumResponseSize(qsizetype bytes)
{
    return d->setMaximumResponseSize(bytes);
}

qsizetype ScpiClient::maximumResponseSize() const
{
    return d->maximumResponseSize();
}

bool ScpiClient::setRecoveryPolicy(RecoveryPolicy policy)
{
    return d->setRecoveryPolicy(policy);
}

ScpiClient::RecoveryPolicy ScpiClient::recoveryPolicy() const
{
    return d->recoveryPolicy();
}

bool ScpiClient::setAvailabilityCommand(QByteArray command)
{
    return d->setAvailabilityCommand(std::move(command));
}

QByteArray ScpiClient::availabilityCommand() const
{
    return d->availabilityCommand();
}

ScpiClient::SessionState ScpiClient::sessionState() const
{
    return d->sessionState();
}

ScpiClient::Availability ScpiClient::availability() const
{
    return d->availability();
}

bool ScpiClient::isMonitoring() const
{
    return d->isMonitoring();
}

ScpiClient::OperationId ScpiClient::open()
{
    return d->open();
}

ScpiClient::OperationId ScpiClient::close()
{
    return d->close();
}

ScpiClient::OperationId ScpiClient::send(QByteArray command)
{
    return send(std::move(command), false);
}

ScpiClient::OperationId ScpiClient::send(QByteArray command, bool confirm)
{
    const auto kind = confirm ? detail::OperationKind::ConfirmedSend : detail::OperationKind::Send;
    return d->submit(kind, std::move(command));
}

ScpiClient::OperationId ScpiClient::sendConfirmed(QByteArray command)
{
    return send(std::move(command), true);
}

ScpiClient::OperationId ScpiClient::query(QByteArray command, ResponseFormat format)
{
    return query(std::move(command), format, 1);
}

ScpiClient::OperationId ScpiClient::query(QByteArray command, ResponseFormat format,
                                          qsizetype messageCount)
{
    return d->submit(detail::OperationKind::Query, std::move(command), format, messageCount);
}

ScpiClient::OperationId ScpiClient::checkAvailability()
{
    return d->checkAvailability();
}

bool ScpiClient::startMonitoring(int intervalMs)
{
    return d->startMonitoring(intervalMs);
}

void ScpiClient::stopMonitoring()
{
    d->stopMonitoring();
}

} // namespace scpi

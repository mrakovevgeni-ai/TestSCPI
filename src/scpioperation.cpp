#include "scpioperation.h"

#include <QByteArrayView>

#include <utility>

namespace scpi::detail {
namespace {

using Error = ScpiClient::Error;
using Format = ScpiClient::ResponseFormat;

} // namespace

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

QByteArray encodeCommand(QByteArray command, OperationKind kind)
{
    command.append('\n');
    if (kind == OperationKind::ConfirmedSend)
        command.append("*OPC?\n");
    return command;
}

bool supportsFormat(Format format)
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

ActiveOperation::ActiveOperation(Operation operation, ScpiClient::Timeouts timeouts,
                                 qsizetype maxResponseSize)
    : m_operation(std::move(operation)),
      m_deadline(m_operation.kind == OperationKind::Check ? timeouts.availabilityMs
                                                        : timeouts.responseMs,
                 Qt::PreciseTimer)
{
    if (m_operation.kind != OperationKind::Send)
        m_parser.emplace(m_operation.format, maxResponseSize, m_operation.messageCount);
}

Error ActiveOperation::timeoutError() const
{
    if (m_operation.kind == OperationKind::Check)
        return Error::AvailabilityTimeout;
    if (m_operation.kind == OperationKind::Send)
        return Error::WriteTimeout;
    return Error::ResponseTimeout;
}

QByteArray ActiveOperation::takeRequest()
{
    m_startedWriting = true;
    return std::move(m_operation.message);
}

std::optional<OperationResult> ActiveOperation::written()
{
    m_written = true;
    return result();
}

std::optional<OperationResult> ActiveOperation::receive(const QByteArray &bytes)
{
    if (!m_parser) {
        return OperationResult{Error::UnexpectedData,
                               QStringLiteral("Received data with no SCPI response pending."), {}, true};
    }

    auto parsed = m_parser->append(bytes);
    if (parsed.status == ParseStatus::NeedMore)
        return std::nullopt;
    if (parsed.status == ParseStatus::Error)
        return OperationResult{parsed.error, std::move(parsed.detail), {}, true};

    m_reply.emplace(std::move(parsed.payload));
    m_parser.reset();
    return result();
}

std::optional<OperationResult> ActiveOperation::result()
{
    if (!m_written)
        return std::nullopt;
    if (m_operation.kind == OperationKind::Send)
        return OperationResult{};
    if (!m_reply)
        return std::nullopt;

    switch (m_operation.kind) {
    case OperationKind::ConfirmedSend:
        if (*m_reply != "1" && *m_reply != "+1") {
            return OperationResult{Error::UnexpectedResponse,
                                   QStringLiteral("Expected a complete 1 or +1 reply to *OPC?."), {}};
        }
        break;
    case OperationKind::Check:
        if (QByteArrayView(*m_reply).trimmed().isEmpty()) {
            return OperationResult{Error::UnexpectedResponse,
                                   QStringLiteral("The SCPI availability reply is empty."), {}};
        }
        break;
    case OperationKind::Query:
        return OperationResult{Error::None, {}, std::move(*m_reply)};
    case OperationKind::Send:
        break;
    }

    return OperationResult{};
}

} // namespace scpi::detail

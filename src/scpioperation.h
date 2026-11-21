#pragma once

#include "responseparser.h"

#include <QDeadlineTimer>

#include <optional>

namespace scpi::detail {

enum class OperationKind { Send, ConfirmedSend, Query, Check };

struct Operation {
    ScpiClient::OperationId id;
    OperationKind kind;
    QByteArray message;
    ScpiClient::ResponseFormat format = ScpiClient::ResponseFormat::Text;
    qsizetype messageCount = 1;
};

QByteArray normalizeCommand(QByteArray command);
QByteArray encodeCommand(QByteArray command, OperationKind kind);
bool supportsFormat(ScpiClient::ResponseFormat format);

struct OperationResult {
    ScpiClient::Error error = ScpiClient::Error::None;
    QString detail;
    QByteArray response;
    bool framingLost = false;
};

class ActiveOperation final
{
public:
    ActiveOperation(Operation operation, ScpiClient::Timeouts timeouts, qsizetype maxResponseSize);

    const Operation &operation() const { return m_operation; }
    const QDeadlineTimer &deadline() const { return m_deadline; }
    ScpiClient::Error timeoutError() const;
    bool startedWriting() const { return m_startedWriting; }

    QByteArray takeRequest();
    std::optional<OperationResult> written();
    std::optional<OperationResult> receive(const QByteArray &bytes);

private:
    std::optional<OperationResult> result();

    Operation m_operation;
    QDeadlineTimer m_deadline;
    std::optional<ResponseParser> m_parser;
    std::optional<QByteArray> m_reply;
    bool m_startedWriting = false;
    bool m_written = false;
};

} // namespace scpi::detail

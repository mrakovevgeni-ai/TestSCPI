#pragma once

#include <scpi/scpiclient.h>

namespace scpi::detail {

enum class ParseStatus { NeedMore, Complete, Error };

struct ParseResult {
    ParseStatus status = ParseStatus::NeedMore;
    QByteArray payload;
    ScpiClient::Error error = ScpiClient::Error::None;
    QString detail;
};

// Feed one response, stopping on Complete or Error.
class ResponseParser
{
public:
    ResponseParser(ScpiClient::ResponseFormat format, qsizetype maxPayloadSize);

    ParseResult append(const QByteArray &bytes);

private:
    ParseResult parseText();
    ParseResult parseBlock();

    ScpiClient::ResponseFormat m_format;
    qsizetype m_maxPayloadSize;
    QByteArray m_buffer;
    qsizetype m_textScanOffset = 0;
    qsizetype m_headerSize = 0;
    qsizetype m_payloadSize = 0;
};

} // namespace scpi::detail

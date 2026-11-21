#pragma once

#include <scpi/scpiclient.h>

#include <QByteArrayView>

namespace scpi::detail {

enum class ParseStatus { NeedMore, Complete, Error };

struct ParseResult {
    ParseStatus status = ParseStatus::NeedMore;
    QByteArray payload;
    ScpiClient::Error error = ScpiClient::Error::None;
    QString detail;
};

class ResponseParser final
{
public:
    ResponseParser(ScpiClient::ResponseFormat format, qsizetype maxPayloadSize,
                   qsizetype messageCount = 1);

    // Feed bytes until Complete or Error. Only Complete contains a payload.
    ParseResult append(const QByteArray &bytes);

private:
    struct Frame {
        ParseStatus status = ParseStatus::NeedMore;
        qsizetype payloadOffset = 0;
        qsizetype payloadSize = 0;
        qsizetype wireSize = 0;
        ScpiClient::Error error = ScpiClient::Error::None;
        QString detail;

        static Frame failure(ScpiClient::Error error, const QString &detail);
    };

    Frame parseText(QByteArrayView input, qsizetype budget);
    Frame parseBlock(QByteArrayView input, qsizetype budget) const;

    ScpiClient::ResponseFormat m_format;
    qsizetype m_maxPayloadSize;
    qsizetype m_messageCount;
    qsizetype m_received = 0;
    qsizetype m_textScanOffset = 0;
    QByteArray m_buffer;
    QByteArray m_payload;
};

} // namespace scpi::detail

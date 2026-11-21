#include "responseparser.h"

#include <QByteArrayView>

#include <algorithm>
#include <utility>

namespace scpi::detail {
namespace {

using Error = ScpiClient::Error;
using Format = ScpiClient::ResponseFormat;

ParseResult parseFailure(Error error, const QString &detail)
{
    return {ParseStatus::Error, {}, error, detail};
}

ParseResult complete(QByteArray payload)
{
    return {ParseStatus::Complete, std::move(payload), Error::None, {}};
}

} // namespace

ResponseParser::ResponseParser(Format format, qsizetype maxPayloadSize)
    : m_format(format), m_maxPayloadSize(maxPayloadSize)
{
}

ParseResult ResponseParser::append(const QByteArray &bytes)
{
    m_buffer.append(bytes);
    if (m_format == Format::Text)
        return parseText();
    return parseBlock();
}

ParseResult ResponseParser::parseText()
{
    const qsizetype lf = m_buffer.indexOf('\n', m_textScanOffset);
    if (lf >= 0) {
        const bool hasCr = lf > 0 && m_buffer.at(lf - 1) == '\r';
        const qsizetype payloadSize = lf - hasCr;
        if (payloadSize > m_maxPayloadSize)
            return parseFailure(Error::ResponseTooLarge, QStringLiteral("Text response exceeds the size limit."));
        if (lf + 1 != m_buffer.size())
            return parseFailure(Error::UnexpectedData, QStringLiteral("Extra bytes after the text response."));
        m_buffer.truncate(payloadSize);
        return complete(std::move(m_buffer));
    }

    m_textScanOffset = m_buffer.size();
    const qsizetype possiblePayloadSize = m_buffer.size() - m_buffer.endsWith('\r');
    if (possiblePayloadSize > m_maxPayloadSize)
        return parseFailure(Error::ResponseTooLarge, QStringLiteral("Text response exceeds the size limit."));
    return {};
}

ParseResult ResponseParser::parseBlock()
{
    if (m_headerSize == 0) {
        if (!m_buffer.isEmpty() && m_buffer.at(0) != '#')
            return parseFailure(Error::MalformedResponse, QStringLiteral("Expected an IEEE 488.2 block header."));
        if (m_buffer.size() < 2)
            return {};

        const char digits = m_buffer.at(1);
        if (digits == '0')
            return parseFailure(Error::UnsupportedFormat, QStringLiteral("Indefinite IEEE 488.2 blocks are unsupported."));
        if (digits < '1' || digits > '9')
            return parseFailure(Error::MalformedResponse, QStringLiteral("Invalid IEEE 488.2 length field width."));

        const qsizetype headerSize = 2 + (digits - '0');
        qsizetype payloadSize = 0;
        for (qsizetype i = 2; i < std::min(headerSize, m_buffer.size()); ++i) {
            const char digit = m_buffer.at(i);
            if (digit < '0' || digit > '9')
                return parseFailure(Error::MalformedResponse, QStringLiteral("Invalid digit in the IEEE 488.2 length."));
            payloadSize = payloadSize * 10 + (digit - '0');
        }
        if (m_buffer.size() < headerSize)
            return {};
        if (payloadSize > m_maxPayloadSize)
            return parseFailure(Error::ResponseTooLarge, QStringLiteral("Binary response exceeds the size limit."));

        m_headerSize = headerSize;
        m_payloadSize = payloadSize;
    }

    const qsizetype payloadEnd = m_headerSize + m_payloadSize;
    if (m_buffer.size() < payloadEnd)
        return {};

    QByteArrayView terminator;
    if (m_format == Format::IeeeBlockLf)
        terminator = "\n";
    else if (m_format == Format::IeeeBlockCrlf)
        terminator = "\r\n";

    const qsizetype suffixSize = m_buffer.size() - payloadEnd;
    for (qsizetype i = 0; i < std::min(suffixSize, terminator.size()); ++i) {
        if (m_buffer.at(payloadEnd + i) != terminator.at(i))
            return parseFailure(Error::MalformedResponse, QStringLiteral("Invalid IEEE 488.2 block terminator."));
    }
    if (suffixSize < terminator.size())
        return {};
    if (suffixSize > terminator.size())
        return parseFailure(Error::UnexpectedData, QStringLiteral("Extra bytes after the IEEE 488.2 block."));

    m_buffer.truncate(payloadEnd);
    m_buffer.remove(0, m_headerSize);
    return complete(std::move(m_buffer));
}

} // namespace scpi::detail

#include "responseparser.h"

#include <algorithm>
#include <utility>

namespace scpi::detail {
namespace {

using Error = ScpiClient::Error;
using Format = ScpiClient::ResponseFormat;

QByteArrayView blockTerminator(Format format)
{
    if (format == Format::IeeeBlockLf)
        return "\n";
    if (format == Format::IeeeBlockCrlf)
        return "\r\n";
    return {};
}

} // namespace

ResponseParser::ResponseParser(Format format, qsizetype maxPayloadSize, qsizetype messageCount)
    : m_format(format), m_maxPayloadSize(maxPayloadSize), m_messageCount(messageCount)
{
}

ParseResult ResponseParser::append(const QByteArray &bytes)
{
    m_buffer.append(bytes);
    qsizetype consumed = 0;
    while (consumed < m_buffer.size()) {
        const bool text = m_format == Format::Text;
        const bool separator = text && m_received > 0;
        const qsizetype budget = m_maxPayloadSize - m_payload.size() - separator;
        const auto input = QByteArrayView(m_buffer).sliced(consumed);
        const auto frame = text ? parseText(input, budget) : parseBlock(input, budget);

        if (frame.status == ParseStatus::Error)
            return {ParseStatus::Error, {}, frame.error, frame.detail};
        if (frame.status == ParseStatus::NeedMore)
            break;

        if (separator)
            m_payload.append('\n');
        m_payload.append(input.data() + frame.payloadOffset, frame.payloadSize);
        consumed += frame.wireSize;
        m_textScanOffset = 0;

        if (++m_received == m_messageCount) {
            if (consumed != m_buffer.size()) {
                return {ParseStatus::Error, {}, Error::UnexpectedData,
                        QStringLiteral("Extra bytes after the final SCPI message.")};
            }
            return {ParseStatus::Complete, std::move(m_payload), Error::None, {}};
        }
    }

    // Keep only the incomplete frame. Its text search offset stays relative to its start.
    if (consumed > 0)
        m_buffer.remove(0, consumed);
    return {};
}

ResponseParser::Frame ResponseParser::Frame::failure(Error error, const QString &detail)
{
    return {ParseStatus::Error, 0, 0, 0, error, detail};
}

ResponseParser::Frame ResponseParser::parseText(QByteArrayView input, qsizetype budget)
{
    const qsizetype lf = input.indexOf('\n', m_textScanOffset);
    if (lf >= 0) {
        const bool hasCr = lf > 0 && input.at(lf - 1) == '\r';
        const qsizetype payloadSize = lf - hasCr;
        if (payloadSize > budget)
            return Frame::failure(Error::ResponseTooLarge, QStringLiteral("Text response exceeds the size limit."));
        return {ParseStatus::Complete, 0, payloadSize, lf + 1, Error::None, {}};
    }

    m_textScanOffset = input.size();
    const qsizetype possiblePayloadSize = input.size() - (input.back() == '\r');
    if (possiblePayloadSize > budget)
        return Frame::failure(Error::ResponseTooLarge, QStringLiteral("Text response exceeds the size limit."));
    return {};
}

ResponseParser::Frame ResponseParser::parseBlock(QByteArrayView input, qsizetype budget) const
{
    if (input.front() != '#')
        return Frame::failure(Error::MalformedResponse, QStringLiteral("Expected an IEEE 488.2 block header."));
    if (input.size() < 2)
        return {};

    const char digits = input.at(1);
    if (digits == '0')
        return Frame::failure(Error::UnsupportedFormat, QStringLiteral("Indefinite IEEE 488.2 blocks are unsupported."));
    if (digits < '1' || digits > '9')
        return Frame::failure(Error::MalformedResponse, QStringLiteral("Invalid IEEE 488.2 length field width."));

    const qsizetype headerSize = 2 + (digits - '0');
    qsizetype payloadSize = 0;
    for (qsizetype i = 2; i < std::min(headerSize, input.size()); ++i) {
        const char digit = input.at(i);
        if (digit < '0' || digit > '9')
            return Frame::failure(Error::MalformedResponse, QStringLiteral("Invalid digit in the IEEE 488.2 length."));
        payloadSize = payloadSize * 10 + (digit - '0');
    }
    if (input.size() < headerSize)
        return {};
    if (payloadSize > budget)
        return Frame::failure(Error::ResponseTooLarge, QStringLiteral("Binary response exceeds the size limit."));

    const qsizetype payloadEnd = headerSize + payloadSize;
    if (input.size() < payloadEnd)
        return {};

    const auto terminator = blockTerminator(m_format);
    const qsizetype suffixSize = input.size() - payloadEnd;
    for (qsizetype i = 0; i < std::min(suffixSize, terminator.size()); ++i) {
        if (input.at(payloadEnd + i) != terminator.at(i))
            return Frame::failure(Error::MalformedResponse, QStringLiteral("Invalid IEEE 488.2 block terminator."));
    }
    if (suffixSize < terminator.size())
        return {};
    return {ParseStatus::Complete, headerSize, payloadSize, payloadEnd + terminator.size(), Error::None, {}};
}

} // namespace scpi::detail

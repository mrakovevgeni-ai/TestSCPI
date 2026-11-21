#include "responseparser.h"

#include <QtTest>

#include <algorithm>

using scpi::ScpiClient;
using scpi::detail::ParseStatus;
using scpi::detail::ResponseParser;

using Format = ScpiClient::ResponseFormat;
using Error = ScpiClient::Error;

class ParserTest : public QObject
{
    Q_OBJECT

private slots:
    void fragmented_data();
    void fragmented();
    void invalidFrame_data();
    void invalidFrame();
    void multipleMessages_data();
    void multipleMessages();
    void combinedLimitAndExtraData();
};

void ParserTest::fragmented_data()
{
    QTest::addColumn<Format>("format");
    QTest::addColumn<QByteArray>("frame");
    QTest::addColumn<QByteArray>("payload");

    QTest::newRow("text-lf") << Format::Text << QByteArray("MODEL,123\n") << QByteArray("MODEL,123");
    QTest::newRow("text-crlf") << Format::Text << QByteArray("12.50\r\n") << QByteArray("12.50");
    QTest::newRow("empty-text") << Format::Text << QByteArray("\r\n") << QByteArray();

    const auto binary = QByteArray::fromHex("0a000d23ff");
    QTest::newRow("block-lf") << Format::IeeeBlockLf << ("#205" + binary + '\n') << binary;
    QTest::newRow("block-crlf") << Format::IeeeBlockCrlf << ("#15" + binary + "\r\n") << binary;
    QTest::newRow("block-no-terminator") << Format::IeeeBlockNoTerminator << ("#15" + binary) << binary;
    QTest::newRow("block-empty") << Format::IeeeBlockNoTerminator << QByteArray("#10") << QByteArray();
    QTest::newRow("block-nine-length-digits") << Format::IeeeBlockLf
        << QByteArray("#9000000002AB\n") << QByteArray("AB");
}

void ParserTest::fragmented()
{
    QFETCH(Format, format);
    QFETCH(QByteArray, frame);
    QFETCH(QByteArray, payload);

    const auto limit = std::max<qsizetype>(1, payload.size());
    for (qsizetype split = 0; split < frame.size(); ++split) {
        ResponseParser parser(format, limit);
        QCOMPARE(parser.append(frame.first(split)).status, ParseStatus::NeedMore);
        const auto result = parser.append(frame.sliced(split));
        QCOMPARE(result.status, ParseStatus::Complete);
        QCOMPARE(result.error, Error::None);
        QCOMPARE(result.payload, payload);
    }

    ResponseParser parser(format, limit);
    for (qsizetype i = 0; i < frame.size() - 1; ++i)
        QCOMPARE(parser.append(frame.sliced(i, 1)).status, ParseStatus::NeedMore);
    const auto result = parser.append(frame.last(1));
    QCOMPARE(result.status, ParseStatus::Complete);
    QCOMPARE(result.payload, payload);
}

void ParserTest::invalidFrame_data()
{
    QTest::addColumn<Format>("format");
    QTest::addColumn<QByteArray>("prefix");
    QTest::addColumn<QByteArray>("tail");
    QTest::addColumn<Error>("error");

    QTest::newRow("bad-marker") << Format::IeeeBlockLf << QByteArray() << QByteArray("x")
        << Error::MalformedResponse;
    QTest::newRow("indefinite-block") << Format::IeeeBlockLf << QByteArray("#") << QByteArray("0")
        << Error::UnsupportedFormat;
    QTest::newRow("bad-digit-count") << Format::IeeeBlockLf << QByteArray("#") << QByteArray("x")
        << Error::MalformedResponse;
    QTest::newRow("bad-length") << Format::IeeeBlockLf << QByteArray("#2") << QByteArray("x")
        << Error::MalformedResponse;
    QTest::newRow("bad-lf") << Format::IeeeBlockLf << QByteArray("#11A") << QByteArray("\r")
        << Error::MalformedResponse;
    QTest::newRow("bad-crlf") << Format::IeeeBlockCrlf << QByteArray("#11A\r") << QByteArray("x")
        << Error::MalformedResponse;
    QTest::newRow("binary-limit-from-header") << Format::IeeeBlockCrlf << QByteArray("#2") << QByteArray("04")
        << Error::ResponseTooLarge;
    QTest::newRow("text-limit-before-lf") << Format::Text << QByteArray("abc\r") << QByteArray("x")
        << Error::ResponseTooLarge;
    QTest::newRow("text-limit-with-lf") << Format::Text << QByteArray("abc") << QByteArray("d\n")
        << Error::ResponseTooLarge;
    QTest::newRow("extra-text-bytes") << Format::Text << QByteArray("abc") << QByteArray("\nX")
        << Error::UnexpectedData;
    QTest::newRow("extra-binary-bytes") << Format::IeeeBlockLf << QByteArray("#11") << QByteArray("A\nX")
        << Error::UnexpectedData;
    QTest::newRow("unexpected-terminator") << Format::IeeeBlockNoTerminator << QByteArray("#11") << QByteArray("A\n")
        << Error::UnexpectedData;
}

void ParserTest::invalidFrame()
{
    QFETCH(Format, format);
    QFETCH(QByteArray, prefix);
    QFETCH(QByteArray, tail);
    QFETCH(Error, error);

    ResponseParser parser(format, 3);
    QCOMPARE(parser.append(prefix).status, ParseStatus::NeedMore);
    const auto result = parser.append(tail);
    QCOMPARE(result.status, ParseStatus::Error);
    QCOMPARE(result.error, error);
    QVERIFY(!result.detail.isEmpty());
}

void ParserTest::multipleMessages_data()
{
    QTest::addColumn<Format>("format");
    QTest::addColumn<QByteArray>("wire");
    QTest::addColumn<QByteArray>("payload");

    QTest::newRow("text") << Format::Text << QByteArray("first\r\nsecond\n\r\n")
        << QByteArray("first\nsecond\n");
    const auto a = QByteArray::fromHex("000a");
    const auto b = QByteArray::fromHex("230d");
    QTest::newRow("blocks-lf") << Format::IeeeBlockLf << ("#12" + a + "\n#10\n#12" + b + '\n') << (a + b);
    QTest::newRow("blocks-crlf") << Format::IeeeBlockCrlf
        << ("#12" + a + "\r\n#10\r\n#12" + b + "\r\n") << (a + b);
    QTest::newRow("blocks-no-terminator") << Format::IeeeBlockNoTerminator
        << ("#12" + a + "#10#12" + b) << (a + b);
}

void ParserTest::multipleMessages()
{
    QFETCH(Format, format);
    QFETCH(QByteArray, wire);
    QFETCH(QByteArray, payload);

    for (qsizetype split = 0; split < wire.size(); ++split) {
        ResponseParser parser(format, payload.size(), 3);
        QCOMPARE(parser.append(wire.first(split)).status, ParseStatus::NeedMore);
        const auto result = parser.append(wire.sliced(split));
        QCOMPARE(result.status, ParseStatus::Complete);
        QCOMPARE(result.payload, payload);
    }

    ResponseParser parser(format, payload.size(), 3);
    for (qsizetype i = 0; i < wire.size() - 1; ++i)
        QCOMPARE(parser.append(wire.sliced(i, 1)).status, ParseStatus::NeedMore);
    const auto result = parser.append(wire.last(1));
    QCOMPARE(result.status, ParseStatus::Complete);
    QCOMPARE(result.payload, payload);
}

void ParserTest::combinedLimitAndExtraData()
{
    ResponseParser text(Format::Text, 6, 2);
    QCOMPARE(text.append("abc\n").status, ParseStatus::NeedMore);
    QCOMPARE(text.append("def\n").error, Error::ResponseTooLarge);

    ResponseParser binary(Format::IeeeBlockNoTerminator, 5, 2);
    QCOMPARE(binary.append("#13abc#").status, ParseStatus::NeedMore);
    QCOMPARE(binary.append("13").error, Error::ResponseTooLarge);

    ResponseParser extra(Format::Text, 16, 2);
    QCOMPARE(extra.append("one\ntwo\nextra\n").error, Error::UnexpectedData);
}

QTEST_GUILESS_MAIN(ParserTest)
#include "parser_test.moc"

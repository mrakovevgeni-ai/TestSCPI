#include "instrumentemulator.h"

#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>

#include <algorithm>
#include <cmath>
#include <optional>
#include <utility>

namespace scpi::emulator {
namespace {

constexpr int maximumReplyBytes = 8 * 1024 * 1024;
constexpr int maximumMessages = 1024;
constexpr qint64 maximumCommandBytes = 64 * 1024;

struct Reply {
    QByteArray bytes;
    int delayMs = 0;
    bool disconnect = false;
};

struct ResponseSize {
    int bytesPerMessage;
    int messages;
};

std::optional<ResponseSize> parseSize(const QByteArray &argument, int defaultWidth)
{
    if (argument.isEmpty())
        return ResponseSize{defaultWidth, 1};
    const auto fields = argument.split(',');
    if (fields.size() > 2)
        return std::nullopt;

    bool widthOk = false;
    bool countOk = true;
    const auto width = fields.at(0).toInt(&widthOk);
    const auto count = fields.size() == 2 ? fields.at(1).toInt(&countOk) : 1;
    if (!widthOk || !countOk || width < 1 || count < 1 || count > maximumMessages
        || qint64(width) * count > maximumReplyBytes) {
        return std::nullopt;
    }
    return ResponseSize{width, count};
}

class InstrumentModel final
{
public:
    explicit InstrumentModel(QByteArray binaryTerminator) : m_binaryTerminator(std::move(binaryTerminator))
    {
    }

    Reply execute(QByteArray command)
    {
        command = command.trimmed();
        const auto separator = command.indexOf(' ');
        auto name = (separator < 0 ? command : command.first(separator)).toUpper();
        if (name.startsWith(':'))
            name.remove(0, 1);
        const auto argument = separator < 0 ? QByteArray{} : command.sliced(separator + 1).trimmed();

        if (name == "*IDN?")
            return {"SCPI,Instrument emulator,0001,1.1\n"};
        if (name == "*OPC?")
            return {"+1\r\n"};
        if (name == "VOLT" || name == "SOUR:VOLT")
            return setVoltage(argument);
        if (name == "VOLT?" || name == "SOUR:VOLT?" || name == "MEAS:VOLT?")
            return {QByteArray::number(m_voltage, 'g', 15) + '\n'};
        if (name == "SYST:ERR?")
            return {std::exchange(m_error, QByteArrayLiteral("0,\"No error\"")) + '\n'};
        if (name == "TRAC:DATA?")
            return dataReply(argument, false);
        if (name == "SIM:TEXT?")
            return dataReply(argument, true);
        if (name == "SIM:DROP?")
            return {{}, 0, true};
        if (name == "SIM:BAD?")
            return {"#0unsupported\n"};
        if (name == "SIM:DELAY?") {
            bool ok = false;
            const auto delay = argument.toInt(&ok);
            if (ok && delay >= 0 && delay <= 60000)
                return {"1\n", delay};
            return error("-222,\"Data out of range\"", true);
        }
        return error("-113,\"Undefined header\"", name.endsWith('?'));
    }

private:
    Reply error(QByteArray value, bool respond)
    {
        m_error = std::move(value);
        return {respond ? m_error + '\n' : QByteArray{}};
    }

    Reply setVoltage(const QByteArray &argument)
    {
        bool ok = false;
        const auto value = argument.toDouble(&ok);
        if (!ok || !std::isfinite(value))
            return error("-128,\"Numeric data error\"", false);
        m_voltage = value;
        return {};
    }

    Reply dataReply(const QByteArray &argument, bool text)
    {
        const auto size = parseSize(argument, 16);
        if (!size)
            return error("-222,\"Data out of range\"", true);
        if (text)
            return {textMessages(*size)};
        return {binaryBlocks(*size)};
    }

    static QByteArray textMessages(ResponseSize size)
    {
        QByteArray response;
        response.reserve(qsizetype(size.bytesPerMessage + 1) * size.messages);
        for (int i = 0; i < size.messages; ++i) {
            response.append(QByteArray(size.bytesPerMessage, char('A' + i % 26)));
            response.append('\n');
        }
        return response;
    }

    QByteArray binaryBlocks(ResponseSize size) const
    {
        QByteArray payload(size.bytesPerMessage, '\0');
        for (qsizetype i = 0; i < payload.size(); ++i)
            payload[i] = static_cast<char>(i % 128);

        const auto length = QByteArray::number(payload.size());
        const auto header = '#' + QByteArray::number(length.size()) + length;
        const auto frame = header + payload + m_binaryTerminator;
        return frame.repeated(size.messages);
    }

    QByteArray m_binaryTerminator;
    QByteArray m_error = QByteArrayLiteral("0,\"No error\"");
    double m_voltage = 0.0;
};

class InstrumentConnection final : public QObject
{
public:
    InstrumentConnection(QTcpSocket *socket, InstrumentModel &instrument,
                         const InstrumentEmulator::Options &options, QObject *parent)
        : QObject(parent), m_socket(socket), m_instrument(instrument),
          m_chunkBytes(options.chunkBytes), m_chunkDelayMs(options.chunkDelayMs), m_chunkTimer(this)
    {
        m_socket->setParent(this);
        m_socket->setReadBufferSize(maximumCommandBytes);
        m_socket->setSocketOption(QAbstractSocket::LowDelayOption, 1);
        m_chunkTimer.setSingleShot(true);
        m_chunkTimer.setTimerType(Qt::PreciseTimer);
        connect(m_socket, &QTcpSocket::readyRead, this, &InstrumentConnection::processCommands);
        connect(m_socket, &QTcpSocket::bytesWritten, this, &InstrumentConnection::chunkWritten);
        connect(m_socket, &QTcpSocket::disconnected, this, &QObject::deleteLater);
        connect(&m_chunkTimer, &QTimer::timeout, this, &InstrumentConnection::sendChunk);
        processCommands();
    }

private:
    void processCommands()
    {
        while (m_response.isEmpty() && m_socket->canReadLine()) {
            auto reply = m_instrument.execute(m_socket->readLine());
            if (reply.disconnect) {
                m_socket->abort();
                return;
            }
            if (!reply.bytes.isEmpty()) {
                m_response = std::move(reply.bytes);
                m_offset = 0;
                m_chunkTimer.start(reply.delayMs);
            }
        }
        if (m_response.isEmpty() && m_socket->bytesAvailable() >= maximumCommandBytes)
            m_socket->abort();
    }

    void sendChunk()
    {
        if (m_socket->state() != QAbstractSocket::ConnectedState)
            return;

        const auto count = std::min(m_chunkBytes, m_response.size() - m_offset);
        const auto accepted = m_socket->write(m_response.constData() + m_offset, count);
        if (accepted <= 0) {
            m_socket->abort();
            return;
        }
        m_offset += accepted;
    }

    void chunkWritten()
    {
        if (m_response.isEmpty() || m_socket->bytesToWrite() > 0)
            return;
        if (m_offset == m_response.size()) {
            m_response.clear();
            processCommands();
        } else {
            m_chunkTimer.start(m_chunkDelayMs);
        }
    }

    QTcpSocket *m_socket;
    InstrumentModel &m_instrument;
    qsizetype m_chunkBytes;
    int m_chunkDelayMs;
    QTimer m_chunkTimer;
    QByteArray m_response;
    qsizetype m_offset = 0;
};

} // namespace

struct InstrumentEmulator::Private {
    explicit Private(Options settings, InstrumentEmulator *owner)
        : options(std::move(settings)), instrument(options.binaryTerminator), server(owner)
    {
        connect(&server, &QTcpServer::newConnection, owner, [this] {
            while (auto *socket = server.nextPendingConnection())
                new InstrumentConnection(socket, instrument, options, &server);
        });
    }

    Options options;
    InstrumentModel instrument;
    QTcpServer server;
};

InstrumentEmulator::InstrumentEmulator(Options options, QObject *parent)
    : QObject(parent), d(std::make_unique<Private>(std::move(options), this))
{
}

InstrumentEmulator::~InstrumentEmulator() = default;

bool InstrumentEmulator::listen(const QHostAddress &address, quint16 port)
{
    return d->server.listen(address, port);
}

quint16 InstrumentEmulator::port() const
{
    return d->server.serverPort();
}

QString InstrumentEmulator::errorString() const
{
    return d->server.errorString();
}

} // namespace scpi::emulator

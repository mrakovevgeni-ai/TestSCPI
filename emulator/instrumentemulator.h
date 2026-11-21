#pragma once

#include <QByteArray>
#include <QHostAddress>
#include <QObject>

#include <memory>

namespace scpi::emulator {

class InstrumentEmulator final : public QObject
{
public:
    struct Options {
        qsizetype chunkBytes = 4096;
        int chunkDelayMs = 0;
        QByteArray binaryTerminator = "\n";
    };

    explicit InstrumentEmulator(Options options, QObject *parent = nullptr);
    ~InstrumentEmulator() override;

    bool listen(const QHostAddress &address, quint16 port);
    quint16 port() const;
    QString errorString() const;

private:
    struct Private;
    std::unique_ptr<Private> d;
};

} // namespace scpi::emulator

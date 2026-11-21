#pragma once

#include <scpi/scpiclient.h>

#include <algorithm>
#include <optional>

namespace scpi::detail {

class ReconnectBackoff final
{
public:
    explicit ReconnectBackoff(ScpiClient::RecoveryPolicy policy)
        : m_policy(policy), m_delay(policy.initialDelayMs)
    {
    }

    static bool isValid(ScpiClient::RecoveryPolicy policy)
    {
        return policy.maxAttempts > 0 && policy.initialDelayMs > 0
               && policy.maxDelayMs >= policy.initialDelayMs;
    }

    std::optional<int> nextDelay()
    {
        if (!m_policy.enabled || m_attempts >= m_policy.maxAttempts)
            return std::nullopt;

        ++m_attempts;
        const int delay = m_delay;
        m_delay = static_cast<int>(std::min<qint64>(qint64(delay) * 2, m_policy.maxDelayMs));
        return delay;
    }

private:
    ScpiClient::RecoveryPolicy m_policy;
    int m_delay;
    int m_attempts = 0;
};

} // namespace scpi::detail

/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include <QString>
#include <QtGlobal>

#include <algorithm>

// Tracks only fingerprints that were successfully reconciled. A failed cache
// read must not advance the successful fingerprint: the same on-disk state
// remains eligible for retry, with bounded backoff to avoid warning/read loops.
class CacheFingerprintTracker
{
public:
    bool needsReconciliation(const QString &currentFingerprint, qint64 nowMs) const
    {
        if (currentFingerprint.isEmpty()) {
            return false;
        }
        // A failure takes precedence over a previous success for the same
        // fingerprint. This matters when a later recovery attempt (for
        // example, after a config change or revision gap) cannot read a cache
        // that was successfully read earlier.
        if (currentFingerprint == m_failedFingerprint) {
            return nowMs >= m_nextRetryMs;
        }
        return currentFingerprint != m_lastReconciledFingerprint;
    }

    void markFailed(const QString &fingerprint, qint64 nowMs)
    {
        if (fingerprint.isEmpty()) {
            return;
        }
        if (fingerprint != m_failedFingerprint) {
            m_failedFingerprint = fingerprint;
            m_failedAttempts = 0;
        }
        static constexpr qint64 retryDelaysMs[] = {2000, 4000, 8000, 16000, 32000, 60000};
        const int delayIndex = std::min(m_failedAttempts, 5);
        m_nextRetryMs = nowMs + retryDelaysMs[delayIndex];
        m_failedAttempts = std::min(m_failedAttempts + 1, 6);
    }

    void markReconciled(const QString &fingerprint)
    {
        m_lastReconciledFingerprint = fingerprint;
        m_failedFingerprint.clear();
        m_failedAttempts = 0;
        m_nextRetryMs = 0;
    }

private:
    QString m_lastReconciledFingerprint;
    QString m_failedFingerprint;
    qint64 m_nextRetryMs = 0;
    int m_failedAttempts = 0;
};

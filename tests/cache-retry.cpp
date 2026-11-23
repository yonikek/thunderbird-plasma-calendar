/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "../src/cachefingerprinttracker.h"

#include <cstdlib>
#include <iostream>

namespace
{
void require(bool condition, const char *message)
{
    if (!condition) {
        std::cerr << "Cache retry test failed: " << message << '\n';
        std::exit(1);
    }
}
}

int main()
{
    CacheFingerprintTracker tracker;
    require(tracker.needsReconciliation(QStringLiteral("snapshot-a"), 0),
            "initial cache must be considered unreconciled");

    tracker.markReconciled(QStringLiteral("snapshot-a"));
    require(!tracker.needsReconciliation(QStringLiteral("snapshot-a"), 0),
            "successfully reconciled cache must not be reread unnecessarily");

    // A later recovery attempt can fail even for the same fingerprint that
    // previously succeeded. That failed attempt must override the success
    // suppression until a subsequent retry succeeds.
    tracker.markFailed(QStringLiteral("snapshot-a"), 100);
    require(!tracker.needsReconciliation(QStringLiteral("snapshot-a"), 2099),
            "a failed recovery should respect the retry backoff even after an earlier success");
    require(tracker.needsReconciliation(QStringLiteral("snapshot-a"), 2100),
            "a failed recovery of a previously successful fingerprint must be retried");
    tracker.markReconciled(QStringLiteral("snapshot-a"));
    require(!tracker.needsReconciliation(QStringLiteral("snapshot-a"), 2100),
            "a successful retry should clear the failed fingerprint");

    require(tracker.needsReconciliation(QStringLiteral("snapshot-b"), 2100),
            "changed cache must trigger reconciliation immediately");
    tracker.markFailed(QStringLiteral("snapshot-b"), 2100);
    require(!tracker.needsReconciliation(QStringLiteral("snapshot-b"), 4099),
            "retry backoff should avoid repeatedly reading/logging a broken cache");
    require(tracker.needsReconciliation(QStringLiteral("snapshot-b"), 4100),
            "failed cache recovery must retry the unchanged fingerprint after the first poll interval");

    tracker.markFailed(QStringLiteral("snapshot-b"), 4100);
    require(!tracker.needsReconciliation(QStringLiteral("snapshot-b"), 8099),
            "repeated failures should use bounded exponential backoff");
    require(tracker.needsReconciliation(QStringLiteral("snapshot-b"), 8100),
            "repeated failures must remain retryable after the backoff interval");

    // A new fingerprint bypasses the previous fingerprint's backoff.
    require(tracker.needsReconciliation(QStringLiteral("snapshot-c"), 8101),
            "a new cache revision must be attempted immediately");
    tracker.markReconciled(QStringLiteral("snapshot-c"));
    require(!tracker.needsReconciliation(QStringLiteral("snapshot-c"), 8101),
            "successful retry should advance the reconciled fingerprint");
    require(!tracker.needsReconciliation(QString(), 9000), "an empty fingerprint must not trigger a read");

    std::cout << "Cache retry tests passed: failed reads, including failures after an earlier success, retry with bounded backoff.\n";
    return 0;
}

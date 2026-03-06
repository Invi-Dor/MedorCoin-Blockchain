export { IncompatibleHostnameMultiProcessMutexError, IncompatibleMultiProcessMutexError, IncompatiblePlatformMultiProcessMutexError, IncompatibleUidMultiProcessMutexError, InvalidMultiProcessMutexPathError, MultiProcessMutexError, MultiProcessMutexTimeoutError, StaleMultiProcessMutexError, } from "./errors/synchronization.js";
/**
 * A class that implements an inter-process mutex.
 *
 * This Mutex is implemented using hard-link-based atomic file creation. A
 * temporary file containing JSON metadata (PID, hostname, platform, uid,
 * session ID, and creation timestamp) is written first, then hard-linked to
 * the lock path via `fs.linkSync`. `linkSync` fails atomically with `EEXIST`
 * if the lock already exists, ensuring only one process can hold the lock at
 * a time.
 *
 * Staleness is determined by PID liveness only — timestamps are stored for
 * debugging purposes but are never used to determine staleness. This avoids the
 * clock-skew and long-running-task problems that time-based staleness detection
 * has (where a second process can break into a lock that's still legitimately
 * held).
 *
 * Incompatible locks — those created by a different hostname, platform, or
 * uid — are rejected immediately with specific subclasses of
 * `IncompatibleMultiProcessMutexError`
 * (`IncompatibleHostnameMultiProcessMutexError`,
 * `IncompatiblePlatformMultiProcessMutexError`, or
 * `IncompatibleUidMultiProcessMutexError`) because their PID liveness cannot
 * be verified or their lock file cannot be removed. These must be removed
 * manually.
 *
 * When the lock is held by a live process, the caller polls with exponential
 * backoff (default: 5ms → 10ms → ...  → 160ms → 200ms cap) until the lock is
 * released or a timeout (default: 60s) is reached.
 *
 * If the filesystem does not support hard links (e.g., certain network
 * filesystems), acquisition fails fast with a `MultiProcessMutexError` rather
 * than degrading into timeout-based retries.
 *
 * ## Performance characteristics
 *
 * - **Uncontended acquisition:** One temp file write + one `linkSync` — takes
 *   less than 1ms on most systems.
 * - **Stale lock recovery:** One `readFileSync` to read metadata, one
 *   `process.kill(pid, 0)` liveness check, and one `unlinkSync` to remove the
 *   stale lock file before retrying acquisition. The retry is immediate (no
 *   sleep), so recovery adds sub-millisecond overhead.
 * - **Contended (live holder):** Polls with exponential backoff starting at
 *   5ms and doubling each iteration until capped at 200ms. Worst-case latency
 *   after the lock is released is up to `MAX_POLL_INTERVAL_MS` (200ms).
 * - **Release:** A single `unlinkSync` call.
 *
 * ## Limitations
 *
 * - **Polling-based:** There is no filesystem notification; callers discover
 *   that the lock is free only on the next poll, so there can be up to 200ms
 *   of wasted wait time after the lock is released.
 * - **Not reentrant:** The same process (or even the same `MultiProcessMutex`
 *   instance) calling `use()` while already holding the lock will deadlock
 *   until the timeout fires.
 * - **Single-host, single-user only:** Encountering a lock from a different
 *   hostname throws `IncompatibleHostnameMultiProcessMutexError`, a different
 *   platform throws `IncompatiblePlatformMultiProcessMutexError`, and a
 *   different uid throws `IncompatibleUidMultiProcessMutexError`. All extend
 *   `IncompatibleMultiProcessMutexError`. This means the lock is not safe to
 *   use on shared/networked filesystems (e.g., NFS) where multiple hosts or
 *   users may access the same path.
 * - **Requires hard-link support:** The underlying filesystem must support
 *   `linkSync`. If hard links are unsupported, acquisition fails immediately
 *   with `MultiProcessMutexError`.
 * - **PID recycling:** If a process dies and the OS reassigns its PID to a new
 *   unrelated process before the stale check runs, the lock is incorrectly
 *   considered live. This is extremely unlikely in practice due to the large
 *   PID space on modern systems.
 * - **No fairness guarantee:** Multiple waiters polling concurrently have no
 *   guaranteed ordering — whichever one succeeds at `linkSync` first after the
 *   lock is released wins.
 */
export declare class MultiProcessMutex {
    #private;
    /**
     * Creates an inter-process mutex given an absolute path.
     *
     * @param absolutePathToLock The absolute path of the mutex.
     * @param timeout The max amount of time to spend trying to acquire the lock
     *  in milliseconds. Defaults to 60000.
     * @param initialPollInterval The initial poll interval in milliseconds.
     *  Defaults to 5.
     */
    constructor(absolutePathToLock: string, timeout?: number, initialPollInterval?: number);
    /**
     * Runs the function f while holding the mutex, returning its result.
     *
     * @param f The function to run.
     * @returns The result of the function.
     */
    use<T>(f: () => Promise<T>): Promise<T>;
    /**
     * Acquires the mutex, returning an async function to release it.
     * The function MUST be called after using the mutex.
     *
     * If this function throws, no cleanup is necessary — the lock was never
     * acquired.
     *
     * @returns The mutex's release function.
     */
    acquire(): Promise<() => Promise<void>>;
}
/**
 * A class that implements an asynchronous mutex (mutual exclusion) lock.
 *
 * The mutex ensures that only one asynchronous operation can be executed at a time,
 * providing exclusive access to a shared resource.
 */
export declare class AsyncMutex {
    #private;
    /**
     * Acquires the mutex, running the provided function exclusively,
     * and releasing it afterwards.
     *
     * @param f The function to run.
     * @returns The result of the function.
     */
    exclusiveRun<ReturnT>(f: () => ReturnT): Promise<Awaited<ReturnT>>;
}
//# sourceMappingURL=synchronization.d.ts.map
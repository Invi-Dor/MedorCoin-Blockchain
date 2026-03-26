/**
 * MedorCoin - Scheduler / Event Loop
 * Isolates failures in one module so the rest of the node continues.
 * Manages recurring tasks and safe shutdown.
 *
 * Fixes:
 *  1. Recursive setTimeout — no task overlap
 *  2. Per-task running lock — concurrency guard
 *  3. Configurable retryDelayMs + maxErrors per task
 *  4. Parallel shutdown hooks with per-hook timeout
 */

const EventEmitter = require("events");
const logger = require("./logger");
const metrics = require("./metrics");

const DEFAULT_MAX_ERRORS = 10;
const DEFAULT_RETRY_DELAY_MS = 5 * 60 * 1000;  // 5 minutes
const SHUTDOWN_HOOK_TIMEOUT_MS = 10_000;         // 10s per hook

class Scheduler extends EventEmitter {
  constructor() {
    super();
    this.tasks = new Map(); // name -> TaskEntry
    this.running = false;
    this.shutdownHooks = [];
  }

  /**
   * Register a recurring task.
   * @param {string}   name
   * @param {Function} fn              - Async function to run
   * @param {number}   intervalMs      - How often to run
   * @param {Object}   options
   * @param {number}   options.maxErrors      - Errors before disabling (default: 10)
   * @param {number}   options.retryDelayMs   - Re-enable delay after disabling (default: 5min)
   * @param {boolean}  options.runImmediately - Run once on register
   * @param {boolean}  options.critical       - If true, never auto-re-enable; emit alert instead
   */
  register(name, fn, intervalMs, options = {}) {
    const {
      maxErrors = DEFAULT_MAX_ERRORS,
      retryDelayMs = DEFAULT_RETRY_DELAY_MS,
      runImmediately = false,
      critical = false,
    } = options;

    if (this.tasks.has(name)) {
      logger.warn("SCHEDULER", "scheduler.js:46", `Task already registered: ${name}`);
      return;
    }

    const task = {
      fn,
      intervalMs,
      timer: null,
      lastRun: null,
      errorCount: 0,
      maxErrors,
      retryDelayMs,
      critical,
      disabled: false,
      running: false, // FIX 2: concurrency lock
    };

    this.tasks.set(name, task);
    logger.info("SCHEDULER", "scheduler.js:64", `Task registered: ${name} (every ${intervalMs}ms)`);

    if (runImmediately) this._runTask(name);
    if (this.running) this._scheduleTask(name);
  }

  start() {
    this.running = true;
    for (const name of this.tasks.keys()) {
      this._scheduleTask(name);
    }
    logger.info("SCHEDULER", "scheduler.js:75", `Scheduler started with ${this.tasks.size} tasks.`);
  }

  // FIX 1: recursive setTimeout prevents overlap
  _scheduleTask(name) {
    const task = this.tasks.get(name);
    if (!task || task.disabled || !this.running) return;

    task.timer = setTimeout(async () => {
      await this._runTask(name);
      // Re-schedule only after the task finishes
      if (!task.disabled && this.running) {
        this._scheduleTask(name);
      }
    }, task.intervalMs);
  }

  async _runTask(name) {
    const task = this.tasks.get(name);
    if (!task || task.disabled) return;

    // FIX 2: skip if already running (concurrency guard)
    if (task.running) {
      logger.warn("SCHEDULER", "scheduler.js:96", `Task ${name} still running, skipping cycle.`);
      metrics.increment("scheduler_task_skipped", { task: name });
      return;
    }

    task.running = true;
    try {
      const start = Date.now();
      await task.fn();
      task.lastRun = Date.now();
      task.errorCount = 0;
      const elapsed = Date.now() - start;

      if (elapsed > task.intervalMs * 0.8) {
        logger.warn("SCHEDULER", "scheduler.js:109", `Task ${name} slow: took ${elapsed}ms`);
      }
    } catch (err) {
      task.errorCount++;
      logger.error(
        "SCHEDULER",
        "scheduler.js:115",
        `Task ${name} error (${task.errorCount}/${task.maxErrors}): ${err.message}`
      );
      metrics.increment("scheduler_task_errors", { task: name });

      if (task.errorCount >= task.maxErrors) {
        task.disabled = true;
        clearTimeout(task.timer);

        // FIX 3: critical tasks never silently re-enable — emit alert instead
        if (task.critical) {
          logger.error(
            "SCHEDULER",
            "scheduler.js:128",
            `CRITICAL task ${name} permanently disabled. Manual intervention required.`
          );
          this.emit("task:critical_failure", { name, error: err.message });
        } else {
          logger.error(
            "SCHEDULER",
            "scheduler.js:135",
            `Task ${name} disabled. Re-enabling in ${task.retryDelayMs}ms.`
          );
          this.emit("task:disabled", { name, error: err.message });
          // FIX 3: configurable retryDelayMs
          setTimeout(() => this._reEnable(name), task.retryDelayMs);
        }
      }
    } finally {
      task.running = false; // always release lock
    }
  }

  _reEnable(name) {
    const task = this.tasks.get(name);
    if (!task || task.critical) return;
    task.disabled = false;
    task.errorCount = 0;
    this._scheduleTask(name);
    logger.info("SCHEDULER", "scheduler.js:152", `Task ${name} re-enabled.`);
  }

  stop(name) {
    const task = this.tasks.get(name);
    if (!task) return;
    clearTimeout(task.timer);
    task.disabled = true;
    logger.info("SCHEDULER", "scheduler.js:160", `Task ${name} stopped.`);
  }

  onShutdown(fn) {
    this.shutdownHooks.push(fn);
  }

  /**
   * Graceful shutdown.
   * FIX 4: hooks run in parallel, each with an individual timeout.
   */
  async shutdown() {
    logger.info("SCHEDULER", "scheduler.js:172", "Scheduler shutting down...");
    this.running = false;

    // Stop all timers
    for (const [name, task] of this.tasks) {
      clearTimeout(task.timer);
      logger.info("SCHEDULER", "scheduler.js:178", `Task stopped: ${name}`);
    }

    // FIX 4: run all hooks in parallel, each races against a timeout
    const hookResults = await Promise.allSettled(
      this.shutdownHooks.map((hook, i) =>
        Promise.race([
          hook(),
          new Promise((_, reject) =>
            setTimeout(
              () => reject(new Error(`Shutdown hook #${i} timed out`)),
              SHUTDOWN_HOOK_TIMEOUT_MS
            )
          ),
        ])
      )
    );

    for (let i = 0; i < hookResults.length; i++) {
      if (hookResults[i].status === "rejected") {
        logger.error(
          "SCHEDULER",
          "scheduler.js:198",
          `Shutdown hook #${i} failed: ${hookResults[i].reason}`
        );
      }
    }

    logger.info("SCHEDULER", "scheduler.js:205", "Scheduler shutdown complete.");
  }

  status() {
    const result = {};
    for (const [name, task] of this.tasks) {
      result[name] = {
        disabled: task.disabled,
        running: task.running,
        errorCount: task.errorCount,
        lastRun: task.lastRun,
        intervalMs: task.intervalMs,
        critical: task.critical,
      };
    }
    return result;
  }
}

module.exports = new Scheduler();

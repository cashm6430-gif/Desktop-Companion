import { spawn } from "node:child_process";
import { randomUUID } from "node:crypto";

const startType = "session.execution.started";
const stopTypes = new Set([
  "session.execution.succeeded", "session.execution.failed", "session.execution.interrupted",
]);

// The helper receives identifiers and lifecycle only. Never forward a whole
// OpenCode event: errors, prompts, tool output and file paths can be private.
export function helperTransport(helperPath, { endpoint, timeoutMs = 1500 } = {}) {
  return (payload) => new Promise((resolve) => {
    const args = ["--client", "opencode", "--require-delivery"];
    if (endpoint) args.push("--endpoint", endpoint);
    let child;
    let timer;
    let finished = false;
    const finish = (ok) => {
      if (finished) return;
      finished = true;
      clearTimeout(timer);
      resolve(ok);
    };
    try {
      child = spawn(helperPath, args, { windowsHide: true, shell: false, stdio: ["pipe", "ignore", "ignore"] });
      child.once("error", () => finish(false));
      child.once("exit", (code) => finish(code === 0));
      child.stdin.on("error", () => {});
      timer = setTimeout(() => { child.kill(); finish(false); }, timeoutMs);
      child.stdin.end(JSON.stringify(payload) + "\n");
    } catch { finish(false); }
  });
}

export function createBridge(send, { scope = randomUUID(), now = Date.now } = {}) {
  const owners = new Set();
  const active = new Map();
  const sequences = new Map();
  const pending = new Map();
  let work = Promise.resolve();
  let closing = false;
  const enqueue = (fn) => (work = work.then(fn).catch(() => {}));
  const sessionKey = (id) => `${scope}:${id}`;
  const payload = (id, hook_event_name, turn = "execution") => ({
    hook_event_name, session_id: sessionKey(id), turn_id: turn,
  });
  async function deliver(id, message) {
    let ok = false;
    try { ok = await send(message); } catch { /* Companion absence must not fail a turn. */ }
    if (ok) pending.delete(id);
    else pending.set(id, { message, expires: now() + 30_000 });
  }
  async function dropOwner(owner) {
    for (const [id, entry] of active) {
      entry.owners.delete(owner);
      if (entry.owners.size) continue;
      active.delete(id);
      await deliver(id, payload(id, "SessionEnd"));
    }
  }
  return {
    get closed() { return closing; },
    acquire() {
      if (closing) throw new Error("Bridge is closed");
      const owner = Symbol();
      owners.add(owner);
      return owner;
    },
    accept(owner, event) {
      if (closing || !owners.has(owner)) return work;
      return enqueue(async () => {
        if (!owners.has(owner)) return;
        const type = event?.type;
        if (type !== startType && !stopTypes.has(type)
            && type !== "session.deleted" && type !== "session.moved") return;
        const id = event?.data?.sessionID;
        if (typeof id !== "string" || !id.trim() || id.length > 256 || id.includes("\0")) return;
        const seq = event?.durable?.seq;
        if (!Number.isSafeInteger(seq) || seq < 0 || event.durable.aggregateID !== id) return;
        const prior = sequences.get(id);
        if (prior !== undefined && seq <= prior) {
          if (seq === prior && type === startType) active.get(id)?.owners.add(owner);
          return;
        }
        sequences.delete(id);
        sequences.set(id, seq);
        // Bound completed-session history, never evict a live session's clock.
        if (sequences.size > 2048) {
          for (const key of sequences.keys()) {
            if (!active.has(key)) { sequences.delete(key); break; }
          }
        }
        if (type === startType) {
          const current = active.get(id);
          if (current) { current.owners.add(owner); return; }
          if (active.size >= 1024) return;
          active.set(id, { owners: new Set([owner]) });
          await deliver(id, payload(id, "UserPromptSubmit"));
        } else {
          const tracked = active.has(id) || pending.has(id);
          active.delete(id);
          if (tracked) await deliver(id, payload(id, stopTypes.has(type) ? "Stop" : "SessionEnd"));
        }
      });
    },
    retry() {
      if (closing) return work;
      return enqueue(async () => {
        for (const [id, receipt] of pending) {
          if (now() >= receipt.expires) { pending.delete(id); continue; }
          let ok = false;
          try { ok = await send(receipt.message); } catch {}
          if (ok) pending.delete(id);
        }
      });
    },
    suspend(owner) {
      if (!owners.has(owner)) return work;
      return enqueue(() => dropOwner(owner));
    },
    release(owner) {
      if (!owners.has(owner)) return work;
      // Stop admitting callbacks immediately, but drain already-admitted events
      // before removing their owner and clearing its live work.
      return enqueue(async () => {
        owners.delete(owner);
        await dropOwner(owner);
        if (!owners.size) { closing = true; pending.clear(); sequences.clear(); }
      });
    },
    drain() { return work; },
  };
}

export async function observe(ctx, bridge, owner, signal) {
  const pause = () => new Promise((resolve) => {
    const done = () => { clearTimeout(timer); signal.removeEventListener("abort", done); resolve(); };
    const timer = setTimeout(done, 1500);
    signal.addEventListener("abort", done, { once: true });
    if (signal.aborted) done();
  });
  while (!signal.aborted) {
    try {
      for await (const event of ctx.event.subscribe({ signal })) {
        if (signal.aborted) break;
        await bridge.accept(owner, event);
      }
    } catch { /* Public stream can disconnect; reconnect without logging payloads. */ }
    // OpenCode does not replay events missed by a disconnected consumer. Clear
    // only this observer's work as bookkeeping, never fabricate a completion.
    if (!signal.aborted) { await bridge.suspend(owner); await pause(); }
  }
}

import { spawn } from "node:child_process";
import { randomUUID } from "node:crypto";

const startType = "session.execution.started";
const stopTypes = new Set([
  "session.execution.succeeded", "session.execution.failed", "session.execution.interrupted",
]);
const permissionTypes = new Set(["permission.asked", "permission.replied"]);
const validId = (id) => typeof id === "string" && Boolean(id.trim()) && id.length <= 256 && !id.includes("\0");

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
  const permissionEvents = new Map();
  const resolvedRequests = new Map();
  let work = Promise.resolve();
  let closing = false;
  const enqueue = (fn) => (work = work.then(fn).catch(() => {}));
  const sessionKey = (id) => `${scope}:${id}`;
  const payload = (id, hook_event_name, turn = "execution") => ({
    hook_event_name, session_id: sessionKey(id), turn_id: turn,
  });
  const snapshot = (id, entry) => ({ ...payload(id, "PermissionSnapshot"), request_ids: [...entry.requests.keys()].sort() });
  const requestKey = (id, request) => JSON.stringify([id, request]);
  function boundedSet(map, key, value) {
    map.set(key, value);
    if (map.size > 4096) map.delete(map.keys().next().value);
  }
  function resolveRequest(id, request) {
    boundedSet(resolvedRequests, requestKey(id, request), true);
  }
  async function attempt(message) {
    let ok = false;
    try { ok = await send(message); } catch { /* Companion absence must not fail a turn. */ }
    return ok;
  }
  async function flush(id, receipt) {
    if (receipt.lifecycle) {
      if (!await attempt(receipt.lifecycle)) return;
      if (receipt.lifecycle.hook_event_name === "UserPromptSubmit") {
        const entry = active.get(id);
        if (entry && entry.generation === receipt.generation) entry.startedDelivered = true;
      }
      receipt.lifecycle = null;
    }
    if (receipt.snapshot) {
      if (!await attempt(receipt.snapshot)) return;
      receipt.snapshot = null;
    }
    if (pending.get(id) === receipt) pending.delete(id);
  }
  async function deliver(id, message, generation) {
    // A new real lifecycle transition supersedes older state and owns its own
    // delivery window. A late Stop must not inherit an almost-expired wait.
    const receipt = { lifecycle: message,
      snapshot: message.hook_event_name === "UserPromptSubmit" ? snapshot(id, active.get(id)) : null,
      generation, expires: now() + 30_000 };
    pending.set(id, receipt);
    await flush(id, receipt);
  }
  async function deliverSnapshot(id, entry) {
    let receipt = pending.get(id);
    if (receipt && now() >= receipt.expires) { pending.delete(id); receipt = undefined; }
    // A snapshot cannot establish Busy by itself. Once an undelivered start's
    // retry window expired, do not resurrect that old execution from a wait.
    if (!entry.startedDelivered && !receipt?.lifecycle) return;
    if (!receipt) {
      receipt = { lifecycle: null, snapshot: null, generation: entry.generation, expires: now() + 30_000 };
      pending.set(id, receipt);
    } else if (!receipt.lifecycle) {
      // A genuine request-set change gets a fresh window after start delivery.
      // In contrast, changes queued behind an undelivered start must retain its
      // original deadline so approvals cannot resurrect old work indefinitely.
      receipt.expires = now() + 30_000;
    }
    receipt.snapshot = snapshot(id, entry); // Only the newest complete state survives.
    await flush(id, receipt);
  }
  function clearRequests(id, entry) {
    for (const request of entry.requests.keys()) resolveRequest(id, request);
    entry.requests.clear();
  }
  async function dropOwner(owner) {
    for (const [id, entry] of active) {
      entry.owners.delete(owner);
      if (entry.owners.size) {
        let changed = false;
        for (const [request, info] of entry.requests) {
          info.owners.delete(owner);
          if (info.owners.size) continue;
          entry.requests.delete(request);
          resolveRequest(id, request);
          changed = true;
        }
        if (changed) await deliverSnapshot(id, entry);
        continue;
      }
      clearRequests(id, entry);
      active.delete(id);
      await deliver(id, payload(id, "SessionEnd"), entry.generation);
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
            && type !== "session.deleted" && type !== "session.moved" && !permissionTypes.has(type)) return;
        const id = event?.data?.sessionID;
        if (!validId(id)) return;
        if (permissionTypes.has(type)) {
          const asked = type === "permission.asked";
          const request = event.data[asked ? "id" : "requestID"];
          // These are ephemeral events, not session-log entries. Never advance
          // durable sequence numbers using an approval receipt.
          if (!validId(request) || !validId(event.id) || !Number.isFinite(event.created)
              || event.created < 0 || event.durable !== undefined
              || (!asked && !["once", "always", "reject"].includes(event.data.reply))) return;
          const key = requestKey(id, request);
          const identity = JSON.stringify([type, id, request, event.created]);
          const seen = permissionEvents.get(event.id);
          const entry = active.get(id);
          if (seen !== undefined) {
            if (seen === identity && asked && entry?.owners.has(owner)) entry.requests.get(request)?.owners.add(owner);
            return;
          }
          if (!asked) {
            boundedSet(permissionEvents, event.id, identity);
            resolveRequest(id, request); // Reply-before-ask must never resurrect a wait.
            if (entry?.requests.delete(request)) await deliverSnapshot(id, entry);
            return;
          }
          if (!entry?.owners.has(owner) || resolvedRequests.has(key)
              || entry.startedAt === undefined || event.created < entry.startedAt) return;
          boundedSet(permissionEvents, event.id, identity);
          const current = entry.requests.get(request);
          if (current) { current.owners.add(owner); return; }
          if (entry.requests.size >= 32) return;
          entry.requests.set(request, { owners: new Set([owner]) });
          await deliverSnapshot(id, entry);
          return;
        }
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
          // A newer durable Started marks a new busy period even if a terminal
          // was missed. Previous observers must attach to this generation again.
          if (current) clearRequests(id, current);
          if (!current && active.size >= 1024) return;
          active.set(id, { owners: new Set([owner]), requests: new Map(), generation: seq,
            startedAt: Number.isFinite(event.created) ? event.created : undefined, startedDelivered: false });
          await deliver(id, payload(id, "UserPromptSubmit"), seq);
        } else {
          const tracked = active.has(id) || pending.has(id);
          const entry = active.get(id);
          if (entry) clearRequests(id, entry);
          active.delete(id);
          if (tracked) await deliver(id, payload(id, stopTypes.has(type) ? "Stop" : "SessionEnd"), seq);
        }
      });
    },
    retry() {
      if (closing) return work;
      return enqueue(async () => {
        for (const [id, receipt] of pending) {
          if (now() >= receipt.expires) { pending.delete(id); continue; }
          await flush(id, receipt);
        }
      });
    },
    permissionChecks(owner) {
      if (closing || !owners.has(owner)) return [];
      return [...active].filter(([, entry]) => entry.owners.has(owner) && entry.requests.size)
        .map(([sessionID, entry]) => ({ sessionID, generation: entry.generation, request_ids: [...entry.requests.keys()] }));
    },
    reconcile(owner, check, present) {
      if (closing || !owners.has(owner) || !Array.isArray(present) || present.some((id) => !validId(id))) return work;
      return enqueue(async () => {
        const entry = active.get(check?.sessionID);
        if (!entry?.owners.has(owner) || entry.generation !== check.generation || !Array.isArray(check.request_ids)) return;
        const current = new Set(present);
        let changed = false;
        // Only prune IDs captured BEFORE the read. A request arriving while
        // list() was in flight must not be removed by an older snapshot.
        for (const request of check.request_ids) {
          if (current.has(request) || !entry.requests.delete(request)) continue;
          resolveRequest(check.sessionID, request);
          changed = true;
        }
        if (changed) await deliverSnapshot(check.sessionID, entry);
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
        if (!owners.size) { closing = true; pending.clear(); sequences.clear(); permissionEvents.clear(); resolvedRequests.clear(); }
      });
    },
    drain() { return work; },
  };
}

export async function observe(ctx, bridge, owner, signal) {
  let checking = false;
  let cursor = 0;
  const timer = typeof ctx.permission?.list === "function" && typeof bridge.permissionChecks === "function"
    ? setInterval(() => {
      if (checking || signal.aborted) return;
      checking = true;
      void (async () => {
        const checks = bridge.permissionChecks(owner);
        // Bound reads per observer tick and rotate fairly through busy sessions.
        const count = Math.min(checks.length, 8);
        for (let offset = 0; offset < count && !signal.aborted; ++offset) {
          const check = checks[(cursor + offset) % checks.length];
          const rows = await ctx.permission.list({ sessionID: check.sessionID });
          if (signal.aborted) break;
          if (!Array.isArray(rows) || rows.length > 1024
              || rows.some((row) => !validId(row?.id) || row.sessionID !== check.sessionID)) continue;
          await bridge.reconcile(owner, check, rows.map((row) => row.id));
        }
        cursor += count;
      })().catch(() => {}).finally(() => { checking = false; });
    }, 1500) : undefined;
  timer?.unref?.();
  const pause = () => new Promise((resolve) => {
    const done = () => { clearTimeout(timer); signal.removeEventListener("abort", done); resolve(); };
    const timer = setTimeout(done, 1500);
    signal.addEventListener("abort", done, { once: true });
    if (signal.aborted) done();
  });
  try {
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
  } finally { clearInterval(timer); }
}

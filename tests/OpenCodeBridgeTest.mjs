import assert from "node:assert/strict";
import { spawn } from "node:child_process";
import { randomUUID } from "node:crypto";
import { existsSync } from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";
import test from "node:test";

import { createBridge, helperTransport, observe } from "../tools/opencode-plugin/bridge.mjs";

const start = "session.execution.started";
const terminal = ["session.execution.succeeded", "session.execution.failed", "session.execution.interrupted"];
const event = (type, id = "session-a", seq = 1, extra = {}) => ({
  type, id: `evt_${id}_${seq}`, created: seq * 10, data: { sessionID: id }, durable: { aggregateID: id, seq }, ...extra,
});
const message = (hook, id = "session-a", scope = "fixture") => ({
  hook_event_name: hook, session_id: `${scope}:${id}`, turn_id: "execution",
});
const fixture = (send) => {
  const sent = [];
  const bridge = createBridge(send ?? (async (payload) => { sent.push(structuredClone(payload)); return true; }),
    { scope: "fixture", now: () => 0 });
  return { bridge, sent, owner: bridge.acquire() };
};

const startedMessages = (id = "session-a", scope = "fixture") => [message("UserPromptSubmit", id, scope), waitMessage([], id, scope)];

test("one execution starts once; duplicate, old and repeated terminal receipts cannot replay work", async () => {
  const { bridge, sent, owner } = fixture();
  await bridge.accept(owner, event(start, "session-a", 3));
  await bridge.accept(owner, event(start, "session-a", 3));
  await bridge.accept(owner, event(start, "session-a", 2));
  await bridge.accept(owner, event(terminal[0], "session-a", 4));
  await bridge.accept(owner, event(terminal[0], "session-a", 4));
  await bridge.accept(owner, event(start, "session-a", 3));
  assert.deepEqual(sent, [...startedMessages(), message("Stop")]);
  await bridge.release(owner);
  assert.equal(sent.length, 3);
});

test("all terminal outcomes stop busy without inventing a success payload", async () => {
  const { bridge, sent, owner } = fixture();
  for (const [index, type] of terminal.entries()) {
    await bridge.accept(owner, event(start, `session-${index}`, 1));
    await bridge.accept(owner, event(type, `session-${index}`, 2));
  }
  assert.deepEqual(sent.filter((row) => row.hook_event_name !== "PermissionSnapshot").map((row) => row.hook_event_name),
    ["UserPromptSubmit", "Stop", "UserPromptSubmit", "Stop", "UserPromptSubmit", "Stop"]);
  assert.ok(sent.filter((row) => row.hook_event_name !== "PermissionSnapshot").every((row) => Object.keys(row).sort().join() === "hook_event_name,session_id,turn_id"));
  await bridge.release(owner);
});

test("session deletion and relocation clean up an active turn", async () => {
  const { bridge, sent, owner } = fixture();
  for (const [index, type] of ["session.deleted", "session.moved"].entries()) {
    await bridge.accept(owner, event(start, `session-${index}`, 1));
    await bridge.accept(owner, event(type, `session-${index}`, 2));
  }
  assert.deepEqual(sent, [...startedMessages("session-0"), message("SessionEnd", "session-0"),
    ...startedMessages("session-1"), message("SessionEnd", "session-1")]);
  await bridge.release(owner);
});

test("multiple sessions and owners preserve independent busy work", async () => {
  const { bridge, sent, owner } = fixture();
  const other = bridge.acquire();
  await bridge.accept(owner, event(start, "one", 1));
  await bridge.accept(other, event(start, "two", 1));
  await bridge.accept(other, event(start, "one", 1));
  await bridge.release(owner);
  assert.equal(sent.length, 4); // A second stream still owns session one.
  assert.equal(bridge.closed, false);
  await bridge.accept(other, event(terminal[1], "two", 2));
  await bridge.release(other);
  assert.deepEqual(sent, [...startedMessages("one"), ...startedMessages("two"),
    message("Stop", "two"), message("SessionEnd", "one")]);
  assert.equal(bridge.closed, true);
});

test("queued callbacks drain in order and callbacks after release do not restart work", async () => {
  const sent = [];
  let releaseFirst;
  const firstDelivery = new Promise((resolve) => { releaseFirst = resolve; });
  const { bridge, owner } = fixture(async (payload) => {
    sent.push(structuredClone(payload));
    if (sent.length === 1) await firstDelivery;
    return true;
  });
  const pendingStart = bridge.accept(owner, event(start));
  const pendingStop = bridge.accept(owner, event(terminal[0], "session-a", 2));
  await Promise.resolve();
  assert.equal(sent.length, 1);
  releaseFirst();
  await Promise.all([pendingStart, pendingStop, bridge.drain()]);
  assert.deepEqual(sent, [...startedMessages(), message("Stop")]);
  const end = bridge.release(owner);
  await bridge.accept(owner, event(start, "late", 1));
  await end;
  assert.equal(sent.length, 3);
  assert.equal(bridge.closed, true);
  assert.throws(() => bridge.acquire(), /closed/i);
});

test("unrecognized events and invalid identifiers or sequence metadata do not reach helper", async () => {
  const { bridge, sent, owner } = fixture();
  const invalid = [null, {}, event("session.status"), event(start, ""), event(start, " "),
    event(start, "a\0b"), event(start, "x".repeat(257)), event(start, "session-a", -1),
    event(start, "session-a", 1.5), event(start, "session-a", Number.MAX_SAFE_INTEGER + 1),
    { ...event(start), durable: { seq: 1, aggregateID: "another-session" } },
    { ...event(start), durable: {} }, { ...event(start), data: { sessionID: 4 } }];
  for (const value of invalid) await bridge.accept(owner, value);
  assert.deepEqual(sent, []);
  await bridge.accept(Symbol("unregistered"), event(start));
  assert.deepEqual(sent, []);
  await bridge.accept(owner, event(start, "valid-zero", 0));
  assert.deepEqual(sent, [...startedMessages("valid-zero")]);
  await bridge.release(owner);
});

test("only identifiers leave the bridge even when events contain private prompt/error/tool data", async () => {
  const { bridge, sent, owner } = fixture();
  await bridge.accept(owner, event(start, "session-a", 1, {
    prompt: "PRIVATE_PROMPT", error: { message: "PRIVATE_ERROR" },
    data: { sessionID: "session-a", text: "PRIVATE_TOOL_OUTPUT", filePath: "C:/PRIVATE_PATH" },
  }));
  await bridge.accept(owner, event(terminal[1], "session-a", 2, {
    error: { message: "PRIVATE_FAILURE", stack: "PRIVATE_STACK" },
  }));
  const serialized = JSON.stringify(sent);
  assert.equal(serialized.includes("PRIVATE"), false);
  assert.deepEqual(sent, [...startedMessages(), message("Stop")]);
  await bridge.release(owner);
});

test("missing pet is fail-open, retries only latest transition and never replays obsolete starts", async () => {
  let online = false;
  const sent = [];
  const { bridge, owner } = fixture(async (payload) => {
    sent.push(structuredClone(payload));
    if (!online) throw new Error("offline");
    return true;
  });
  await bridge.accept(owner, event(start));
  await bridge.accept(owner, event(terminal[0], "session-a", 2));
  online = true;
  await bridge.retry();
  await bridge.retry();
  assert.deepEqual(sent, [message("UserPromptSubmit"), message("Stop"), message("Stop")]);
  await bridge.release(owner);
});

test("failed transition expires after 30 seconds and retries do not extend its lifetime", async () => {
  let time = 0;
  const sent = [];
  const bridge = createBridge(async (payload) => { sent.push(structuredClone(payload)); return false; },
    { scope: "fixture", now: () => time });
  const owner = bridge.acquire();
  await bridge.accept(owner, event(start));
  time = 29_999;
  await bridge.retry();
  assert.equal(sent.length, 2);
  time = 30_000;
  await bridge.retry();
  assert.equal(sent.length, 2);
  await bridge.release(owner);
});

test("retry keeps pending sessions independent and cleanup replaces an undelivered start", async () => {
  const sent = [];
  let online = false;
  const { bridge, owner } = fixture(async (payload) => { sent.push(structuredClone(payload)); return online; });
  const keeper = bridge.acquire();
  await bridge.accept(owner, event(start, "one", 1));
  await bridge.accept(keeper, event(start, "two", 1));
  await bridge.release(owner);
  online = true;
  await bridge.retry();
  assert.deepEqual(sent.slice(3), [message("SessionEnd", "one"), ...startedMessages("two")]);
  await bridge.release(keeper);
});

test("stream suspension cleans its work but keeps owner reusable and shared streams active", async () => {
  const { bridge, sent, owner } = fixture();
  const shared = bridge.acquire();
  await bridge.accept(owner, event(start, "shared", 1));
  await bridge.accept(shared, event(start, "shared", 1));
  await bridge.accept(owner, event(start, "only-owner", 1));
  await bridge.suspend(owner);
  assert.deepEqual(sent, [...startedMessages("shared"), ...startedMessages("only-owner"),
    message("SessionEnd", "only-owner")]);
  assert.equal(bridge.closed, false);
  await bridge.accept(owner, event(start, "reconnected", 2));
  assert.deepEqual(sent.at(-1), waitMessage([], "reconnected"));
  await bridge.suspend(shared);
  assert.deepEqual(sent.at(-1), message("SessionEnd", "shared"));
  await bridge.release(owner);
  await bridge.release(shared);
});

test("unexpected stream disconnect suspends work and reconnects with the same abort signal", async () => {
  const abort = new AbortController();
  const actions = [];
  let subscriptions = 0;
  const ctx = { event: { subscribe({ signal }) {
    assert.equal(signal, abort.signal);
    ++subscriptions;
    return (async function* () {
      if (subscriptions === 1) yield event(start);
      else abort.abort();
    })();
  } } };
  const bridge = {
    async accept(owner, value) { actions.push(["accept", owner, value.type]); },
    async suspend(owner) { actions.push(["suspend", owner]); },
  };
  const owner = Symbol("owner");
  await observe(ctx, bridge, owner, abort.signal);
  assert.equal(subscriptions, 2);
  assert.deepEqual(actions, [["accept", owner, start], ["suspend", owner]]);
});

test("helper transport returns false for absent executable without shell execution", async () => {
  const send = helperTransport(path.join(path.dirname(fileURLToPath(import.meta.url)), "missing-helper.exe"),
    { endpoint: fixtureEndpoint(), timeoutMs: 1000 });
  assert.equal(await send(message("UserPromptSubmit")), false);
});

const permission = (type = "asked", request = "per-one", session = "session-a", created = 20, extra = {}) => ({
  id: `evt_${session}_${type}_${request}_${created}`, created, type: `permission.${type}`,
  data: { sessionID: session, ...(type === "asked" ? { id: request } : { requestID: request, reply: "once" }) }, ...extra,
});
const waitMessage = (requests, id = "session-a", scope = "fixture") => ({
  ...message("PermissionSnapshot", id, scope), request_ids: [...requests].sort(),
});

test("permission receipts publish complete identifier-only snapshots without ending busy or advancing durable sequence", async () => {
  const { bridge, sent, owner } = fixture();
  await bridge.accept(owner, event(start, "session-a", 1));
  await bridge.accept(owner, permission("asked", "per-two", "session-a", 20, {
    data: { sessionID: "session-a", id: "per-two", action: "PRIVATE_ACTION", resources: ["PRIVATE_PATH"],
      metadata: { command: "PRIVATE_COMMAND" }, message: "PRIVATE_MESSAGE" },
  }));
  await bridge.accept(owner, permission("asked", "per-one"));
  await bridge.accept(owner, permission("replied", "per-two", "session-a", 30));
  await bridge.accept(owner, permission("replied", "per-one", "session-a", 40));
  // Permission creation times are independent of the durable session sequence.
  await bridge.accept(owner, event(terminal[0], "session-a", 2));
  assert.deepEqual(sent, [...startedMessages(), waitMessage(["per-two"]), waitMessage(["per-one", "per-two"]),
    waitMessage(["per-one"]), waitMessage([]), message("Stop")]);
  assert.equal(JSON.stringify(sent).includes("PRIVATE"), false);
  await bridge.release(owner);
});

test("permission asks require an attached observed execution and a valid ephemeral envelope", async () => {
  const { bridge, sent, owner } = fixture();
  const other = bridge.acquire();
  await bridge.accept(owner, permission()); // Cannot bootstrap already-running work.
  await bridge.accept(owner, event(start));
  await bridge.accept(other, permission()); // This observer never saw execution start.
  for (const invalid of [
    permission("asked", ""), permission("asked", "x".repeat(257)), permission("asked", "a\0b"),
    permission("asked", "per-a", "session-a", 9), permission("asked", "per-a", "session-a", -1),
    permission("asked", "per-a", "session-a", NaN), permission("asked", "per-a", "session-a", 20, { id: "" }),
    permission("asked", "per-a", "session-a", 20, { durable: { aggregateID: "session-a", seq: 99 } }),
  ]) await bridge.accept(owner, invalid);
  assert.deepEqual(sent, [...startedMessages()]);
  await bridge.accept(owner, permission());
  assert.deepEqual(sent.at(-1), waitMessage(["per-one"]));
  await bridge.release(owner);
  await bridge.release(other);
});

test("duplicate observers retain waiting ownership; their final suspension clears the snapshot", async () => {
  const { bridge, sent, owner } = fixture();
  const other = bridge.acquire();
  await bridge.accept(owner, event(start));
  await bridge.accept(other, event(start));
  await bridge.accept(owner, permission());
  await bridge.accept(other, permission());
  await bridge.release(owner);
  assert.equal(sent.length, 3);
  await bridge.suspend(other);
  assert.deepEqual(sent.at(-1), message("SessionEnd"));
  assert.equal(bridge.permissionChecks(other).length, 0);
  await bridge.release(other);
});

test("suspending the only approval observer clears waiting while another observer keeps busy", async () => {
  const { bridge, sent, owner } = fixture();
  const other = bridge.acquire();
  await bridge.accept(owner, event(start));
  await bridge.accept(other, event(start));
  await bridge.accept(owner, permission());
  await bridge.suspend(owner);
  assert.deepEqual(sent, [...startedMessages(), waitMessage(["per-one"]), waitMessage([])]);
  await bridge.accept(other, permission()); // Old duplicate cannot reassert a disconnected wait.
  assert.equal(sent.length, 4);
  await bridge.release(owner);
  await bridge.release(other);
});

test("reply-before-ask, duplicates, old generation and terminal receipts never resurrect waiting", async () => {
  const { bridge, sent, owner } = fixture();
  await bridge.accept(owner, event(start));
  await bridge.accept(owner, permission("replied"));
  await bridge.accept(owner, permission());
  await bridge.accept(owner, permission("asked", "per-one", "session-a", 21));
  assert.equal(sent.length, 2);
  await bridge.accept(owner, permission("asked", "per-old"));
  await bridge.accept(owner, event(terminal[1], "session-a", 3));
  await bridge.accept(owner, event(start, "session-a", 5));
  await bridge.accept(owner, permission("asked", "per-old", "session-a", 60));
  await bridge.accept(owner, permission("asked", "per-late", "session-a", 40));
  assert.deepEqual(sent.at(-1), waitMessage([]));
  await bridge.accept(owner, permission("asked", "per-new", "session-a", 60));
  assert.deepEqual(sent.at(-1), waitMessage(["per-new"]));
  await bridge.release(owner);
});

test("parallel approvals resolve individually for once, always and reject and respect the 32-ID limit", async () => {
  const { bridge, sent, owner } = fixture();
  await bridge.accept(owner, event(start));
  for (let index = 0; index < 33; ++index)
    await bridge.accept(owner, permission("asked", `per-${index}`));
  assert.equal(sent.at(-1).request_ids.length, 32);
  for (let index = 0; index < 32; ++index)
    await bridge.accept(owner, permission("replied", `per-${index}`, "session-a", 40,
      { data: { sessionID: "session-a", requestID: `per-${index}`, reply: ["once", "always", "reject"][index % 3] } }));
  assert.deepEqual(sent.at(-1), waitMessage([]));
  assert.ok(sent.filter((row) => row.request_ids).every((row) => row.request_ids.length <= 32));
  await bridge.release(owner);
});

test("pending start is delivered before the latest approval snapshot; stop cancels both obsolete states", async () => {
  let online = false;
  const sent = [];
  const { bridge, owner } = fixture(async (payload) => { sent.push(structuredClone(payload)); return online; });
  await bridge.accept(owner, event(start));
  await bridge.accept(owner, permission());
  await bridge.accept(owner, permission("replied"));
  assert.ok(sent.every((row) => row.hook_event_name === "UserPromptSubmit"));
  online = true;
  await bridge.retry();
  assert.deepEqual(sent.slice(-2), [message("UserPromptSubmit"), waitMessage([])]);
  await bridge.release(owner);

  online = false;
  const second = fixture(async (payload) => { sent.push(structuredClone(payload)); return online; });
  await second.bridge.accept(second.owner, event(start));
  await second.bridge.accept(second.owner, permission());
  await second.bridge.accept(second.owner, event(terminal[2], "session-a", 2));
  const stopped = sent.length;
  online = true;
  await second.bridge.retry();
  assert.deepEqual(sent.slice(stopped), [message("Stop")]);
  await second.bridge.release(second.owner);
});

test("approval changes cannot extend an undelivered start's 30-second retry batch", async () => {
  let time = 0;
  const sent = [];
  const bridge = createBridge(async (payload) => { sent.push(structuredClone(payload)); return false; },
    { scope: "fixture", now: () => time });
  const owner = bridge.acquire();
  await bridge.accept(owner, event(start));
  time = 29_999;
  await bridge.accept(owner, permission());
  const attempts = sent.length;
  time = 30_000;
  await bridge.retry();
  await bridge.accept(owner, permission("asked", "per-next"));
  await bridge.retry();
  assert.equal(sent.length, attempts);
  await bridge.release(owner);
});

test("a wait arriving after retry expiry cannot flush an old start before the retry timer ticks", async () => {
  let time = 0;
  const sent = [];
  const bridge = createBridge(async (payload) => { sent.push(structuredClone(payload)); return false; },
    { scope: "fixture", now: () => time });
  const owner = bridge.acquire();
  await bridge.accept(owner, event(start));
  time = 30_000;
  await bridge.accept(owner, permission());
  assert.deepEqual(sent, [message("UserPromptSubmit")]);
  await bridge.release(owner);
});

test("a real terminal or session cleanup owns 30 seconds instead of inheriting an old snapshot deadline", async () => {
  for (const cleanup of [terminal[0], "session.deleted", "disconnect"]) {
    let time = 0;
    let online = false;
    const sent = [];
    const bridge = createBridge(async (payload) => {
      sent.push(structuredClone(payload));
      return payload.hook_event_name === "UserPromptSubmit" || online;
    }, { scope: "fixture", now: () => time });
    const owner = bridge.acquire();
    await bridge.accept(owner, event(start)); // Start delivered; initial empty snapshot fails.
    assert.deepEqual(sent, startedMessages());
    time = 29_000;
    if (cleanup === "disconnect") await bridge.suspend(owner);
    else await bridge.accept(owner, event(cleanup, "session-a", 2));
    const expected = message(cleanup === terminal[0] ? "Stop" : "SessionEnd");
    assert.deepEqual(sent.at(-1), expected);
    const beforeRetry = sent.length;
    time = 31_000;
    online = true;
    await bridge.retry();
    assert.deepEqual(sent.slice(beforeRetry), [expected], cleanup);
    await bridge.retry();
    assert.equal(sent.length, beforeRetry + 1);
    await bridge.release(owner);
  }
});

test("a real approval clear owns a fresh window without retry attempts extending it", async () => {
  let time = 0;
  let online = true;
  const sent = [];
  let receiverRequests = [];
  const bridge = createBridge(async (payload) => {
    sent.push(structuredClone(payload));
    if (!online) return false;
    if (payload.hook_event_name === "PermissionSnapshot") receiverRequests = [...payload.request_ids];
    return true;
  }, { scope: "fixture", now: () => time });
  const owner = bridge.acquire();
  await bridge.accept(owner, event(start));
  await bridge.accept(owner, permission());
  assert.deepEqual(receiverRequests, ["per-one"]);
  online = false;
  time = 1_000;
  await bridge.accept(owner, permission("asked", "per-two"));
  time = 29_000;
  await bridge.accept(owner, permission("replied", "per-one", "session-a", 30));
  await bridge.accept(owner, permission("replied", "per-two", "session-a", 40));
  const beforeRetry = sent.length;
  time = 31_000;
  online = true;
  await bridge.retry();
  assert.deepEqual(sent.slice(beforeRetry), [waitMessage([])]);
  assert.deepEqual(receiverRequests, []);
  assert.equal(bridge.permissionChecks(owner).length, 0);
  await bridge.release(owner);

  // In a second undeliverable window, retry alone cannot postpone expiry.
  online = false;
  time = 0;
  const expiring = createBridge(async (payload) => {
    sent.push(structuredClone(payload));
    return payload.hook_event_name === "UserPromptSubmit";
  }, { scope: "fixture", now: () => time });
  const other = expiring.acquire();
  await expiring.accept(other, event(start));
  time = 29_000;
  await expiring.accept(other, permission());
  time = 58_999;
  await expiring.retry();
  const lastAttempt = sent.length;
  time = 59_000;
  await expiring.retry();
  assert.equal(sent.length, lastAttempt);
  await expiring.release(other);
});

test("new execution clears receiver approvals even when the preceding stop was undelivered", async () => {
  let receiverRequests = [];
  let rejectStop = false;
  const sent = [];
  const { bridge, owner } = fixture(async (payload) => {
    sent.push(structuredClone(payload));
    if (rejectStop && payload.hook_event_name === "Stop") return false;
    if (payload.hook_event_name === "PermissionSnapshot") receiverRequests = [...payload.request_ids];
    return true;
  });
  await bridge.accept(owner, event(start));
  await bridge.accept(owner, permission());
  assert.deepEqual(receiverRequests, ["per-one"]);
  rejectStop = true;
  await bridge.accept(owner, event(terminal[0], "session-a", 3));
  assert.deepEqual(receiverRequests, ["per-one"]);
  await bridge.accept(owner, event(start, "session-a", 5));
  assert.deepEqual(sent.slice(-2), startedMessages());
  assert.deepEqual(receiverRequests, []);
  await bridge.release(owner);
});

test("newer start replaces a busy generation whose terminal was missed and requires observer reattachment", async () => {
  const { bridge, sent, owner } = fixture();
  const other = bridge.acquire();
  await bridge.accept(owner, event(start));
  await bridge.accept(other, event(start));
  await bridge.accept(owner, permission());
  await bridge.accept(owner, event(start, "session-a", 5));
  assert.deepEqual(sent.slice(-2), startedMessages());
  const count = sent.length;
  await bridge.accept(other, permission("asked", "per-new", "session-a", 60));
  await bridge.accept(owner, permission("asked", "per-one", "session-a", 60));
  assert.equal(sent.length, count);
  await bridge.accept(other, event(start, "session-a", 5));
  await bridge.accept(other, permission("asked", "per-new", "session-a", 60));
  assert.deepEqual(sent.at(-1), waitMessage(["per-new"]));
  await bridge.release(owner);
  await bridge.release(other);
});

test("reconciliation only removes captured IDs and cannot add waits or cross execution generations", async () => {
  const { bridge, sent, owner } = fixture();
  await bridge.accept(owner, event(start));
  await bridge.accept(owner, permission());
  const [check] = bridge.permissionChecks(owner);
  await bridge.accept(owner, permission("asked", "per-new"));
  await bridge.reconcile(owner, check, ["per-unseen"]);
  assert.deepEqual(sent.at(-1), waitMessage(["per-new"]));
  await bridge.accept(owner, event(terminal[0], "session-a", 3));
  await bridge.accept(owner, event(start, "session-a", 5));
  await bridge.accept(owner, permission("asked", "per-current", "session-a", 60));
  const count = sent.length;
  await bridge.reconcile(owner, check, []);
  assert.equal(sent.length, count);
  await bridge.release(owner);
});

test("observer periodically prunes canceled approvals without bootstrapping unseen permission rows", { timeout: 6000 }, async () => {
  const abort = new AbortController();
  const { bridge, sent, owner } = fixture();
  let reads = 0;
  const ctx = {
    event: { subscribe() { return (async function* () {
      yield event(start);
      yield permission();
      await new Promise((resolve) => abort.signal.addEventListener("abort", resolve, { once: true }));
    })(); } },
    permission: { async list(input) {
      assert.deepEqual(input, { sessionID: "session-a" });
      ++reads;
      return [{ id: "per-unseen", sessionID: "session-a", message: "PRIVATE" }];
    } },
  };
  const observed = observe(ctx, bridge, owner, abort.signal);
  try {
    await new Promise((resolve, reject) => {
      const timer = setInterval(() => {
        if (reads < 1 || sent.at(-1)?.request_ids?.length !== 0) return;
        clearInterval(timer); clearTimeout(deadline); resolve();
      }, 20);
      const deadline = setTimeout(() => { clearInterval(timer); reject(new Error("reconciliation did not run")); }, 4000);
    });
    assert.equal(reads, 1);
    assert.deepEqual(sent, [...startedMessages(), waitMessage(["per-one"]), waitMessage([])]);
  } finally { abort.abort(); await observed; await bridge.release(owner); }
});

const productionEndpoint = "deepseek-whale-companion-v1";
function fixtureEndpoint() { return `pet-opencode-node-test-${randomUUID()}`; }
function assertFixtureEndpoint(endpoint) {
  if (!endpoint.startsWith("pet-opencode-node-test-") || endpoint === productionEndpoint)
    throw new Error("Refusing a non-fixture IPC endpoint");
  return endpoint;
}
const helper = process.env.PET_HOOK_HELPER ?? path.resolve(path.dirname(fileURLToPath(import.meta.url)),
  "../build/DesktopCompanionHook" + (process.platform === "win32" ? ".exe" : ""));
const ipcFixture = process.env.PET_OPENCODE_IPC_FIXTURE ?? path.resolve(path.dirname(fileURLToPath(import.meta.url)),
  "../build/OpenCodeIpcFixture" + (process.platform === "win32" ? ".exe" : ""));

test("IPC fixture refuses production endpoint", () => {
  assert.throws(() => assertFixtureEndpoint(productionEndpoint), /Refusing/);
});

test("Node bridge, actual Qt helper and Qt controller preserve private scoped IDs and busy counts", {
  skip: existsSync(helper) && existsSync(ipcFixture) ? false : "Build Qt hook and OpenCodeIpcFixture",
  timeout: 12_000,
}, async () => {
  const endpoint = fixtureEndpoint();
  assertFixtureEndpoint(endpoint); // Refuse unsafe input before launching either child.
  const child = spawn(ipcFixture, [endpoint], { windowsHide: true, shell: false, stdio: ["ignore", "pipe", "pipe"] });
  let stdout = "";
  let stderr = "";
  let ready;
  let failed;
  const readiness = new Promise((resolve, reject) => { ready = resolve; failed = reject; });
  const done = new Promise((resolve, reject) => {
    child.once("error", (error) => { failed(error); reject(error); });
    child.once("exit", (code) => { if (!stdout.includes('"ready":true')) failed(new Error("Fixture exited before ready")); resolve(code); });
  });
  done.catch(() => {}); // Readiness may reject first when the child cannot start.
  child.stdout.setEncoding("utf8");
  child.stderr.setEncoding("utf8");
  child.stdout.on("data", (bytes) => {
    stdout += bytes.toString("utf8");
    if (stdout.includes('"ready":true') && stdout.includes("\n")) ready();
  });
  child.stderr.on("data", (bytes) => { stderr += bytes.toString("utf8"); });
  try {
    await readiness;
    const bridge = createBridge(helperTransport(helper, { endpoint }), { scope: "native-fixture" });
    const owner = bridge.acquire();
    await bridge.accept(owner, event(start, "unicode-鲸鱼-引号\"", 1, { prompt: "PRIVATE_PROMPT" }));
    await bridge.accept(owner, event(terminal[0], "unicode-鲸鱼-引号\"", 2));
    await bridge.release(owner);
    assert.equal(await done, 0);
    assert.equal(stderr.trim(), "");
    const report = stdout.trim().split(/\r?\n/).map((line) => JSON.parse(line)).at(-1);
    assert.equal(report.passed, true);
    const scoped = JSON.stringify(["opencode", 'native-fixture:unicode-鲸鱼-引号"']);
    assert.deepEqual(report.receipts, [
      { event: "start", client: "opencode", scoped_session: scoped, turn_id: "execution", active_turns: 1 },
      { event: "stop", client: "opencode", scoped_session: scoped, turn_id: "execution", active_turns: 0 },
    ]);
  } finally {
    if (child.exitCode === null) child.kill(); // Only this fixture's own child.
  }
});

test("actual helper missing pet reports undelivered to bridge for bounded retry", {
  skip: existsSync(helper) ? false : "Build DesktopCompanionHook or set PET_HOOK_HELPER",
  timeout: 5000,
}, async () => {
  assert.equal(await helperTransport(helper, { endpoint: fixtureEndpoint() })(message("UserPromptSubmit")), false);
});

test("Node permission bridge, actual Qt helper and controller preserve request counts independently of busy", {
  skip: existsSync(helper) && existsSync(ipcFixture) ? false : "Build Qt hook and OpenCodeIpcFixture",
  timeout: 12_000,
}, async () => {
  const endpoint = assertFixtureEndpoint(fixtureEndpoint());
  const child = spawn(ipcFixture, [endpoint, "permission"], { windowsHide: true, shell: false, stdio: ["ignore", "pipe", "pipe"] });
  let stdout = "";
  let stderr = "";
  let ready;
  let failed;
  const readiness = new Promise((resolve, reject) => { ready = resolve; failed = reject; });
  const done = new Promise((resolve, reject) => {
    child.once("error", (error) => { failed(error); reject(error); });
    child.once("exit", (code) => {
      if (!stdout.includes('"ready":true')) failed(new Error("Permission fixture exited before ready"));
      resolve(code);
    });
  });
  done.catch(() => {});
  child.stdout.setEncoding("utf8");
  child.stderr.setEncoding("utf8");
  child.stdout.on("data", (bytes) => {
    stdout += bytes;
    if (stdout.includes('"ready":true') && stdout.includes("\n")) ready();
  });
  child.stderr.on("data", (bytes) => { stderr += bytes; });
  try {
    await readiness;
    const bridge = createBridge(helperTransport(helper, { endpoint }), { scope: "permission-fixture" });
    const owner = bridge.acquire();
    await bridge.accept(owner, event(start));
    await bridge.accept(owner, permission("asked", "per-one"));
    await bridge.accept(owner, permission("asked", "per-two"));
    await bridge.accept(owner, permission("replied", "per-two", "session-a", 30));
    await bridge.accept(owner, permission("replied", "per-one", "session-a", 40));
    await bridge.accept(owner, event(terminal[0], "session-a", 2));
    await bridge.release(owner);
    assert.equal(await done, 0);
    assert.equal(stderr.trim(), "");
    const report = stdout.trim().split(/\r?\n/).map((line) => JSON.parse(line)).at(-1);
    assert.equal(report.passed, true);
    assert.deepEqual(report.receipts.map((row) => row.event),
      ["start", "permission_state", "permission_state", "permission_state", "permission_state", "permission_state", "stop"]);
    assert.deepEqual(report.receipts.map((row) => row.pending_permissions), [0, 0, 1, 2, 1, 0, 0]);
    assert.deepEqual(report.receipts.map((row) => row.active_turns), [1, 1, 1, 1, 1, 1, 0]);
    assert.ok(report.receipts.every((row) => row.scoped_session === '["opencode","permission-fixture:session-a"]'));
  } finally { if (child.exitCode === null) child.kill(); }
});

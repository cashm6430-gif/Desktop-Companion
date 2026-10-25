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
  type, data: { sessionID: id }, durable: { aggregateID: id, seq }, ...extra,
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

test("one execution starts once; duplicate, old and repeated terminal receipts cannot replay work", async () => {
  const { bridge, sent, owner } = fixture();
  await bridge.accept(owner, event(start, "session-a", 3));
  await bridge.accept(owner, event(start, "session-a", 3));
  await bridge.accept(owner, event(start, "session-a", 2));
  await bridge.accept(owner, event(terminal[0], "session-a", 4));
  await bridge.accept(owner, event(terminal[0], "session-a", 4));
  await bridge.accept(owner, event(start, "session-a", 3));
  assert.deepEqual(sent, [message("UserPromptSubmit"), message("Stop")]);
  await bridge.release(owner);
  assert.equal(sent.length, 2);
});

test("all terminal outcomes stop busy without inventing a success payload", async () => {
  const { bridge, sent, owner } = fixture();
  for (const [index, type] of terminal.entries()) {
    await bridge.accept(owner, event(start, `session-${index}`, 1));
    await bridge.accept(owner, event(type, `session-${index}`, 2));
  }
  assert.deepEqual(sent.map((row) => row.hook_event_name),
    ["UserPromptSubmit", "Stop", "UserPromptSubmit", "Stop", "UserPromptSubmit", "Stop"]);
  assert.ok(sent.every((row) => Object.keys(row).sort().join() === "hook_event_name,session_id,turn_id"));
  await bridge.release(owner);
});

test("session deletion and relocation clean up an active turn", async () => {
  const { bridge, sent, owner } = fixture();
  for (const [index, type] of ["session.deleted", "session.moved"].entries()) {
    await bridge.accept(owner, event(start, `session-${index}`, 1));
    await bridge.accept(owner, event(type, `session-${index}`, 2));
  }
  assert.deepEqual(sent, [message("UserPromptSubmit", "session-0"), message("SessionEnd", "session-0"),
    message("UserPromptSubmit", "session-1"), message("SessionEnd", "session-1")]);
  await bridge.release(owner);
});

test("multiple sessions and owners preserve independent busy work", async () => {
  const { bridge, sent, owner } = fixture();
  const other = bridge.acquire();
  await bridge.accept(owner, event(start, "one", 1));
  await bridge.accept(other, event(start, "two", 1));
  await bridge.accept(other, event(start, "one", 1));
  await bridge.release(owner);
  assert.equal(sent.length, 2); // A second stream still owns session one.
  assert.equal(bridge.closed, false);
  await bridge.accept(other, event(terminal[1], "two", 2));
  await bridge.release(other);
  assert.deepEqual(sent, [message("UserPromptSubmit", "one"), message("UserPromptSubmit", "two"),
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
  assert.deepEqual(sent, [message("UserPromptSubmit"), message("Stop")]);
  const end = bridge.release(owner);
  await bridge.accept(owner, event(start, "late", 1));
  await end;
  assert.equal(sent.length, 2);
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
  assert.deepEqual(sent, [message("UserPromptSubmit", "valid-zero")]);
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
  assert.deepEqual(sent, [message("UserPromptSubmit"), message("Stop")]);
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
  assert.deepEqual(sent.slice(3), [message("SessionEnd", "one"), message("UserPromptSubmit", "two")]);
  await bridge.release(keeper);
});

test("stream suspension cleans its work but keeps owner reusable and shared streams active", async () => {
  const { bridge, sent, owner } = fixture();
  const shared = bridge.acquire();
  await bridge.accept(owner, event(start, "shared", 1));
  await bridge.accept(shared, event(start, "shared", 1));
  await bridge.accept(owner, event(start, "only-owner", 1));
  await bridge.suspend(owner);
  assert.deepEqual(sent, [message("UserPromptSubmit", "shared"), message("UserPromptSubmit", "only-owner"),
    message("SessionEnd", "only-owner")]);
  assert.equal(bridge.closed, false);
  await bridge.accept(owner, event(start, "reconnected", 2));
  assert.deepEqual(sent.at(-1), message("UserPromptSubmit", "reconnected"));
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

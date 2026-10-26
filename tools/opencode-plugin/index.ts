import { readFile } from "node:fs/promises";
import { createBridge, helperTransport, observe } from "./bridge.mjs";

const registry = Symbol.for("desktop-companion.opencode.v2.bridge.2");

// OpenCode 2.1.4 Plugin.define is an identity function. A plain definition keeps
// this local plugin dependency-free; OpenCode supplies the setup context.
export default {
  id: "desktop-companion",
  async setup(ctx) {
    const config = JSON.parse(await readFile(new URL("./config.json", import.meta.url), "utf8"));
    if (typeof config.helperPath !== "string" || !config.helperPath.trim())
      throw new Error("Desktop Companion helper is not configured");
    let shared = globalThis[registry];
    if (!shared || shared.bridge.closed || shared.helperPath !== config.helperPath) {
      const bridge = createBridge(helperTransport(config.helperPath));
      const timer = setInterval(() => { void bridge.retry(); }, 1500);
      timer.unref?.();
      shared = globalThis[registry] = { bridge, timer, refs: 0, helperPath: config.helperPath };
    }
    shared.refs++;
    const owner = shared.bridge.acquire();
    const controller = new AbortController();
    // Each context owns a location-scoped stream, even when the event envelope
    // omits location. Shared state deduplicates a session seen by two contexts.
    const task = observe(ctx, shared.bridge, owner, controller.signal);
    let disposed = false;
    return async () => {
      if (disposed) return;
      disposed = true;
      controller.abort();
      await task;
      if (--shared.refs === 0) {
        clearInterval(shared.timer);
        if (globalThis[registry] === shared) delete globalThis[registry];
      }
      await shared.bridge.release(owner);
    };
  },
};

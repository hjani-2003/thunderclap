// Minimal Chrome DevTools Protocol driver (Node ≥ 22, no dependencies).
// Used to screenshot the web UI and to print the user guide to PDF.
import { spawn } from 'node:child_process';
import { mkdtempSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';

const CHROME = process.env.CHROME || 'google-chrome';
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

export async function launch() {
  const profile = mkdtempSync(join(tmpdir(), 'beam-cdp-'));
  const port = 9300 + Math.floor(Math.random() * 500);
  const proc = spawn(CHROME, [
    '--headless=new', '--disable-gpu', '--hide-scrollbars', '--no-first-run',
    '--no-default-browser-check', `--remote-debugging-port=${port}`, `--user-data-dir=${profile}`,
    'about:blank',
  ], { stdio: 'ignore' });

  let target;
  for (let i = 0; i < 100 && !target; i++) {
    await sleep(100);
    try {
      const list = await (await fetch(`http://127.0.0.1:${port}/json/list`)).json();
      target = list.find((t) => t.type === 'page');
    } catch { /* not up yet */ }
  }
  if (!target) throw new Error('Chrome did not start');

  const ws = new WebSocket(target.webSocketDebuggerUrl);
  await new Promise((res, rej) => { ws.onopen = res; ws.onerror = rej; });
  let seq = 0;
  const pending = new Map();
  const listeners = [];
  ws.onmessage = (ev) => {
    const msg = JSON.parse(ev.data);
    if (msg.id && pending.has(msg.id)) {
      const { resolve, reject } = pending.get(msg.id);
      pending.delete(msg.id);
      msg.error ? reject(new Error(msg.error.message)) : resolve(msg.result);
    } else if (msg.method) {
      for (const l of listeners) l(msg);
    }
  };
  const send = (method, params = {}) => new Promise((resolve, reject) => {
    const id = ++seq;
    pending.set(id, { resolve, reject });
    ws.send(JSON.stringify({ id, method, params }));
  });
  const waitFor = (method) => new Promise((res) => listeners.push((m) => m.method === method && res(m.params)));

  await send('Page.enable');
  return {
    send,
    async goto(url, settleMs = 1500) {
      const loaded = waitFor('Page.loadEventFired');
      await send('Page.navigate', { url });
      await Promise.race([loaded, sleep(10000)]);
      await sleep(settleMs);
    },
    async viewport(width, height, scale = 2, mobile = false) {
      await send('Emulation.setDeviceMetricsOverride', { width, height, deviceScaleFactor: scale, mobile });
    },
    async colorScheme(value) {
      await send('Emulation.setEmulatedMedia', { features: [{ name: 'prefers-color-scheme', value }] });
    },
    async eval(expression) {
      const r = await send('Runtime.evaluate', { expression, awaitPromise: true, returnByValue: true });
      return r.result?.value;
    },
    async screenshot(clip) {
      const r = await send('Page.captureScreenshot', { format: 'png', ...(clip ? { clip: { ...clip, scale: 1 } } : {}), captureBeyondViewport: !!clip });
      return Buffer.from(r.data, 'base64');
    },
    async pdf(opts = {}) {
      const r = await send('Page.printToPDF', { printBackground: true, preferCSSPageSize: true, ...opts });
      return Buffer.from(r.data, 'base64');
    },
    async close() {
      ws.close();
      proc.kill();
      await sleep(300);
      rmSync(profile, { recursive: true, force: true });
    },
    sleep,
  };
}

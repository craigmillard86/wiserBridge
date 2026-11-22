// WiserBridge Web Serial console. Plain JS, no dependencies.
"use strict";

const $ = (id) => document.getElementById(id);
const term = $("term");
const ESPRESSIF_VID = 0x303a;
const MAX_CHARS = 400000;

let port = null;
let connecting = false;
let reader = null;
let writer = null;
let log = "";
const history = [];
let histPos = 0;

function append(text) {
  text = text.replace(/\x1b\[[0-9;]*[A-Za-z]/g, "").replace(/\r\n?/g, "\n");
  log += text;
  if (log.length > MAX_CHARS) log = log.slice(-MAX_CHARS);
  const atBottom = term.scrollTop + term.clientHeight >= term.scrollHeight - 30;
  term.textContent = log;
  if (atBottom) term.scrollTop = term.scrollHeight;
}

function note(text) {
  append(`\n[${text}]\n`);
}

function setConnected(on) {
  $("connect").disabled = on;
  $("disconnect").disabled = !on;
  $("restart").disabled = !on;
  $("cmd").disabled = !on;
  $("send").disabled = !on;
  $("epid").disabled = !on;
  $("setepid").disabled = !on;
  for (const b of document.querySelectorAll("[data-cmd],[data-cmds]")) b.disabled = !on;
  $("state").textContent = on
    ? "Connected. Type a command or use the buttons below."
    : "Not connected.";
  if (on) $("cmd").focus();
}

async function send(cmd) {
  if (!writer) return;
  await writer.write(new TextEncoder().encode(cmd + "\r\n"));
}

async function sendSeq(cmds) {
  for (const c of cmds) {
    await send(c);
    await new Promise((r) => setTimeout(r, 800));
  }
}

async function readLoop() {
  const decoder = new TextDecoder();
  while (port && port.readable) {
    reader = port.readable.getReader();
    try {
      for (;;) {
        const { value, done } = await reader.read();
        if (done) break;
        if (value) append(decoder.decode(value, { stream: true }));
      }
    } catch (e) {
      note(`read stopped: ${e.message}`);
      break;
    } finally {
      reader.releaseLock();
      reader = null;
    }
  }
}

async function connect() {
  if (connecting || port) return;
  connecting = true;
  $("connect").disabled = true;
  let p = null;
  try {
    const filters = $("allports").checked ? [] : [{ usbVendorId: ESPRESSIF_VID }];
    p = await navigator.serial.requestPort({ filters });
    await p.open({ baudRate: 115200 });
    // Keep EN and BOOT released (same as tools/wb_monitor.py), so the board runs normally.
    await p.setSignals({ dataTerminalReady: false, requestToSend: false });
    writer = p.writable.getWriter();
    port = p;
    setConnected(true);
    note("connected");
    readLoop().then(() => { if (port) disconnect(); });
    setTimeout(() => send(""), 1500);
  } catch (e) {
    if (e.name !== "NotFoundError") note(`could not connect: ${e.message}`);
    try { if (writer) { writer.releaseLock(); writer = null; } } catch (_) {}
    try { if (p) await p.close(); } catch (_) {}
    port = null;
    setConnected(false);
  } finally {
    connecting = false;
  }
}

async function disconnect() {
  const p = port;
  port = null;
  try { if (reader) await reader.cancel(); } catch (_) {}
  try { if (writer) { writer.releaseLock(); writer = null; } } catch (_) {}
  try { if (p) await p.close(); } catch (_) {}
  setConnected(false);
  note("disconnected");
}

async function restart() {
  // Hardware reset, same as esptool's hard reset: pulse EN (RTS) with BOOT (DTR) released.
  await port.setSignals({ dataTerminalReady: false, requestToSend: true });
  await new Promise((r) => setTimeout(r, 120));
  await port.setSignals({ dataTerminalReady: false, requestToSend: false });
  note("restart");
}

function submit() {
  const cmd = $("cmd").value.trim();
  $("cmd").value = "";
  if (cmd && history[history.length - 1] !== cmd) history.push(cmd);
  histPos = history.length;
  send(cmd);
}

if (!("serial" in navigator)) {
  $("unsupported").classList.remove("hidden");
  $("connect").disabled = true;
}

$("connect").onclick = connect;
$("disconnect").onclick = disconnect;
$("restart").onclick = restart;
$("send").onclick = submit;
$("cmd").addEventListener("keydown", (e) => {
  if (e.key === "Enter") { submit(); e.preventDefault(); }
  else if (e.key === "ArrowUp" && histPos > 0) { $("cmd").value = history[--histPos]; e.preventDefault(); }
  else if (e.key === "ArrowDown") {
    histPos = Math.min(history.length, histPos + 1);
    $("cmd").value = history[histPos] || "";
    e.preventDefault();
  }
});
for (const b of document.querySelectorAll("[data-cmd]")) b.onclick = () => send(b.dataset.cmd);
for (const b of document.querySelectorAll("[data-cmds]")) b.onclick = () => sendSeq(b.dataset.cmds.split("|"));
$("setepid").onclick = () => {
  const v = $("epid").value.trim().toLowerCase();
  if (!/^([0-9a-f]{2}:){7}[0-9a-f]{2}$/.test(v)) { note("EPID must look like 8c:d7:02:ab:04:01:5e:10"); return; }
  send(`epid ${v}`);
};
$("clear").onclick = () => { log = ""; term.textContent = ""; };
$("savelog").onclick = () => {
  const a = document.createElement("a");
  a.href = URL.createObjectURL(new Blob([log], { type: "text/plain" }));
  a.download = `wiserbridge-${new Date().toISOString().replace(/[:.]/g, "-")}.log`;
  a.click();
  URL.revokeObjectURL(a.href);
};
if (navigator.serial) {
  navigator.serial.addEventListener("disconnect", (e) => { if (port && e.target === port) disconnect(); });
}

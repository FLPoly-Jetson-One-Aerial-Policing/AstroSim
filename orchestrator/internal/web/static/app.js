// AstroSim dashboard. Renders /api/v1/status snapshots; all wording lives in
// the TEXT dictionary below so it can be reviewed and translated in one place.

const TEXT = {
  // Sentence shown under the main button for each phase.
  phase: {
    checking: "Checking that everything is ready…",
    downloading: "Downloading the simulator. This happens once for each version and can take several minutes…",
    unavailable: "AstroSim can't start yet.",
    stopped: "Everything is ready. Press Start to launch the simulator.",
    starting: "Starting the simulator. This can take a minute…",
    running: "The simulator is running.",
    recovering: (s) => s.maxAttempts > 0
      ? `Something stopped, so AstroSim is restarting it (attempt ${s.attempt} of ${s.maxAttempts})…`
      // maxAttempts of 0 means unlimited retries.
      : `Something stopped, so AstroSim is restarting it (attempt ${s.attempt})…`,
    stopping: "Stopping the simulator…",
    failed: "The simulator stopped and could not be restarted.",
  },
  // Plain explanation for a reason code (problems found before starting, or failures).
  reason: {
    runtime_missing: "Docker or Podman isn't installed. Install one of them, and this page will update by itself.",
    runtime_not_running: "Docker or Podman isn't running. Start it, and this page will update by itself.",
    runtime_too_old: "Your Docker or Podman is too old. Please update it.",
    images_missing: "Some parts of the simulator haven't been installed yet. See below for what to run.",
    port_busy: "Another program is using a network port the simulator needs. Close it, or stop any simulator you started by hand, and this page will update by itself.",
    component_exited: "One part of the simulator stopped unexpectedly.",
    component_unhealthy: "One part of the simulator stopped responding.",
    ready_timeout: "One part of the simulator took too long to start.",
    launch_failed: "One part of the simulator could not be started.",
    invalid_config: "One part of the simulator rejected its settings.",
    restart_limit: "The simulator kept failing, so AstroSim gave up restarting it.",
    pull_network: "AstroSim can't reach the download server. Check your internet connection; AstroSim will try again by itself.",
    pull_denied: "The download server refused access. If you once ran `podman login ghcr.io` (or docker login), log out and AstroSim will try again by itself.",
    pull_not_found: "This version of the simulator isn't available for download yet, or not for this kind of computer. If AstroSim was just released, wait a few minutes; it will try again by itself.",
    pull_no_space: "There isn't enough free disk space to download the simulator (it needs a few GB). Free some space; AstroSim will try again by itself.",
    pull_failed: "AstroSim couldn't download the simulator. It will try again by itself.",
    pull_cancelled: "The download was cancelled. Press Start to download the simulator and start it.",
  },
  // Label and look of each component state: [text, icon, tone].
  state: {
    pending: ["Waiting", "○", "idle"],
    launching: ["Starting", "↻", "warn"],
    starting: ["Starting", "↻", "warn"],
    ready: ["Ready", "✔", "good"],
    stale: ["Not responding", "▲", "bad"],
    stopping: ["Stopping", "↻", "warn"],
    stopped: ["Stopped", "■", "idle"],
    exited: ["Stopped unexpectedly", "✖", "bad"],
    failed: ["Failed", "✖", "bad"],
  },
  button: { start: "Start", stop: "Stop", cancel: "Cancel download", wait: "Please wait", busy: "Working…" },
  notInstalledNote: "Run this in the AstroSim folder",
  pullNote: "Or download it yourself by running this in a terminal",
  // Download progress of one part: badge labels and the line under the bar.
  download: {
    active: "Downloading",
    done: "Downloaded",
    failed: "Download failed",
    elapsed: (t) => `Downloading for ${t}`,
  },
  // The Logs card and the hint on the failed panel.
  logs: {
    heading: "Logs",
    where: "AstroSim saves technical logs of every part of the simulator in this folder:",
    download: "Download logs",
    failedHint: "Download the logs and attach them when you report the problem.",
  },
  copy: "Copy",
  copied: "Copied",
  copyFailed: "Select and copy",
  lost: "Lost connection to AstroSim. Is it still running? This page will keep trying.",
  requestFailed: "That didn't work. Please try again.",
  notAllowed: "That isn't possible right now.",
  unknown: (code) => `Something unexpected happened (${code}).`,
  runtime: (r) => `Using ${r.name} ${r.version}, ${r.network} network.`,
};

const $ = (id) => document.getElementById(id);
const el = (tag, props = {}, ...children) => {
  const node = document.createElement(tag);
  for (const [k, v] of Object.entries(props)) {
    if (k === "class") node.className = v;
    else node.setAttribute(k, v);
  }
  node.append(...children);
  return node;
};

let snapshot = null;
let inFlight = false;
let lost = false;
let lastStatusText = "";

function phaseText(s) {
  const t = TEXT.phase[s.phase];
  if (typeof t === "function") return t(s);
  if (t) return t;
  return TEXT.unknown(s.phase);
}

function reasonText(s) {
  const code = s.reason && s.reason.code;
  if (!code) return "";
  return TEXT.reason[code] || TEXT.unknown(code);
}

function statusSentence(s) {
  if (lost) return TEXT.lost;
  switch (s.phase) {
    case "unavailable":
    case "stopped":
      // A stopped stack has a reason only after a cancelled download.
      return reasonText(s) || phaseText(s);
    case "failed":
      return `${phaseText(s)} ${reasonText(s)}`.trim();
    default:
      return phaseText(s);
  }
}

function renderButton(s) {
  const b = $("action");
  const stop = s.canStop && !s.canStart;
  b.classList.toggle("stop", stop);
  if (inFlight) {
    b.textContent = TEXT.button.busy;
    b.disabled = true;
  } else if (s.canStart) {
    b.textContent = TEXT.button.start;
    b.disabled = false;
  } else if (s.canStop) {
    b.textContent = s.phase === "downloading" ? TEXT.button.cancel : TEXT.button.stop;
    b.disabled = false;
  } else {
    b.textContent = TEXT.button.wait;
    b.disabled = true;
  }
  b.dataset.action = s.canStart ? "start" : s.canStop ? "stop" : "";
}

function stateBadge(state) {
  const [text, icon, tone] = TEXT.state[state] || [state, "?", "idle"];
  const spin = tone === "warn" && icon === "↻";
  const iconEl = el("span", { "aria-hidden": "true", class: spin ? "spin" : "" }, icon);
  return el("span", { class: `badge ${tone}` }, iconEl, el("span", {}, text));
}

// Formats a number of seconds as m:ss.
function clock(seconds) {
  const t = Math.max(0, Math.floor(seconds));
  return `${Math.floor(t / 60)}:${String(t % 60).padStart(2, "0")}`;
}

// The badge for a component whose image is being downloaded.
function downloadBadge(d) {
  if (d.state === "done") {
    return el("span", { class: "badge good" },
      el("span", { "aria-hidden": "true" }, "✔"), el("span", {}, TEXT.download.done));
  }
  if (d.state === "failed") {
    return el("span", { class: "badge bad" },
      el("span", { "aria-hidden": "true" }, "✖"), el("span", {}, TEXT.download.failed));
  }
  return el("span", { class: "badge warn" },
    el("span", { "aria-hidden": "true", class: "spin" }, "↻"), el("span", {}, TEXT.download.active));
}

// The progress bar and the line under it; the bar has no value because the
// runtime does not report how many bytes are done.
function downloadProgress(d) {
  const line = [TEXT.download.elapsed(clock(d.elapsedSeconds)), d.output].filter(Boolean).join(" · ");
  return [
    el("progress", { "aria-label": TEXT.download.active }),
    el("p", { class: "download" }, line),
  ];
}

function renderComponents(s) {
  const list = $("components");
  list.replaceChildren(...s.components.map((c) => {
    const li = el("li");
    li.append(
      el("div", { class: "comp-head" },
        el("span", { class: "comp-name" }, c.name),
        c.download ? downloadBadge(c.download) : c.installed ? stateBadge(c.state) : el("span", { class: "badge bad" },
          el("span", { "aria-hidden": "true" }, "✖"), el("span", {}, "Not installed")),
      ),
      el("p", { class: "comp-desc" }, c.description),
    );
    if (c.download && c.download.state === "downloading") li.append(...downloadProgress(c.download));
    if (c.facts && c.facts.length) {
      const dl = el("dl", { class: "facts" });
      for (const f of c.facts) dl.append(el("dt", {}, f.label), el("dd", {}, f.value));
      li.append(dl);
    }
    return li;
  }));
}

function renderNotInstalled(s) {
  // A pull command is shown for images that can be downloaded; the build
  // command for those that can only be built.
  const commandOf = (c) => c.pullCommand || c.buildCommand;
  const missing = s.components.filter((c) => !c.installed && commandOf(c));
  const code = s.reason.code;
  const show = s.phase === "unavailable" && (code === "images_missing" || code.startsWith("pull_")) && missing.length > 0;
  $("not-installed").hidden = !show;
  if (!show) return;
  const pulls = missing.every((c) => c.pullCommand);
  $("not-installed").querySelector("p").textContent = `${pulls ? TEXT.pullNote : TEXT.notInstalledNote}:`;
  $("ni-list").replaceChildren(...missing.map((c) => {
    const copy = el("button", { type: "button", class: "copy" }, TEXT.copy);
    copy.addEventListener("click", () => copyText(commandOf(c), copy));
    return el("li", {},
      el("div", { class: "comp-name" }, c.name),
      el("div", { class: "cmd" }, el("code", {}, commandOf(c)), copy));
  }));
}

async function copyText(text, button) {
  try {
    await navigator.clipboard.writeText(text);
    button.textContent = TEXT.copied;
  } catch {
    button.textContent = TEXT.copyFailed;
  }
  setTimeout(() => { button.textContent = TEXT.copy; }, 2000);
}

function renderFailed(s) {
  const show = s.phase === "failed";
  $("failed").hidden = !show;
  if (!show) return;
  $("failed-text").textContent = reasonText(s);
  $("failed-hint").textContent = s.logDir ? TEXT.logs.failedHint : "";
  const box = $("failed-details");
  const parts = [];
  if (s.reason.detail) parts.push(el("p", {}, s.reason.detail));
  for (const c of s.components) {
    if (c.lastLog && c.lastLog.length) {
      parts.push(el("h3", {}, c.name), el("pre", {}, c.lastLog.join("\n")));
    }
  }
  box.replaceChildren(...parts);
}

function renderLogs(s) {
  // logDir is empty when logging could not be set up; then there is nothing to show.
  const show = Boolean(s.logDir);
  $("logs").hidden = !show;
  if (!show) return;
  $("logs-heading").textContent = TEXT.logs.heading;
  $("logs-where").textContent = TEXT.logs.where;
  $("logs-dir").textContent = s.logDir;
  $("logs-download").textContent = TEXT.logs.download;
  const copy = $("logs-copy");
  if (!copy.dataset.ready) {
    copy.dataset.ready = "1";
    copy.textContent = TEXT.copy;
    copy.addEventListener("click", () => copyText($("logs-dir").textContent, copy));
  }
}

function renderBanner(s) {
  // Only shown for problems found before starting that have no panel of their own.
  const show = s.phase === "unavailable" && s.reason.code !== "images_missing";
  $("banner").hidden = !show;
  if (!show) return;
  $("banner").className = "card banner warn";
  $("banner-title").textContent = "Needs your attention";
  $("banner-text").textContent = reasonText(s);
}

function render() {
  const s = snapshot;
  if (!s) return;
  const text = statusSentence(s);
  // Only touch the live region when the sentence changes, so screen readers
  // are not interrupted by the periodic fact updates.
  if (text !== lastStatusText) {
    $("status-text").textContent = text;
    lastStatusText = text;
  }
  renderButton(s);
  renderBanner(s);
  renderNotInstalled(s);
  renderFailed(s);
  renderComponents(s);
  renderLogs(s);
  $("runtime-info").textContent = s.runtime && s.runtime.name ? TEXT.runtime(s.runtime) : "";
}

function setSnapshot(s) {
  snapshot = s;
  lost = false;
  render();
}

async function sendCommand(action) {
  inFlight = true;
  render();
  try {
    const res = await fetch(`/api/v1/${action}`, {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: "{}",
    });
    if (res.status === 202) {
      setSnapshot(await res.json());
    } else if (res.status === 409) {
      $("status-text").textContent = TEXT.notAllowed;
      lastStatusText = "";
    } else {
      throw new Error(`status ${res.status}`);
    }
  } catch {
    $("status-text").textContent = TEXT.requestFailed;
    lastStatusText = "";
  } finally {
    inFlight = false;
    render();
  }
}

$("action").addEventListener("click", () => {
  const a = $("action").dataset.action;
  if (a && !inFlight) sendCommand(a);
});

// --- live updates: SSE first, polling when SSE keeps failing -------------

let poller = null;
let failures = 0;

function markLost() {
  lost = true;
  if (snapshot) $("action").disabled = true;
  $("status-text").textContent = TEXT.lost;
  lastStatusText = TEXT.lost;
}

async function pollOnce() {
  try {
    const res = await fetch("/api/v1/status", { cache: "no-store" });
    if (!res.ok) throw new Error(String(res.status));
    setSnapshot(await res.json());
    return true;
  } catch {
    markLost();
    return false;
  }
}

function startPolling() {
  if (poller) return;
  pollOnce();
  poller = setInterval(pollOnce, 2000);
}

function stopPolling() {
  if (poller) clearInterval(poller);
  poller = null;
}

function connect() {
  if (typeof EventSource === "undefined") {
    startPolling();
    return;
  }
  const es = new EventSource("/api/v1/events");
  es.addEventListener("status", (e) => {
    failures = 0;
    stopPolling();
    try {
      setSnapshot(JSON.parse(e.data));
    } catch {
      /* ignore a malformed event; the next one replaces it */
    }
  });
  // EventSource reconnects by itself. After repeated failures also poll, so
  // the page still updates and can tell whether AstroSim is reachable.
  es.addEventListener("error", () => {
    failures += 1;
    if (failures >= 3) startPolling();
  });
}

connect();

/* The OpenSmash Launcher page (tools/weblauncher/launcher.py serves it and does the work). */
"use strict";
const $ = s => document.querySelector(s);
const $$ = s => [...document.querySelectorAll(s)];
const KEY = new URLSearchParams(location.search).get("key") || "";
let state = null;

async function api(path, body) {
  const opt = body === undefined ? { headers: { "X-Key": KEY } } :
    { method: "POST", headers: { "X-Key": KEY, "Content-Type": "application/json" }, body: JSON.stringify(body) };
  const r = await fetch(path, opt);
  const data = await r.json().catch(() => ({ error: `HTTP ${r.status}` }));
  if (!r.ok) throw new Error(data.error || `HTTP ${r.status}`);
  return data;
}

function esc(s) { return String(s ?? "").replace(/[&<>"']/g, c => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;" })[c]); }

/* ---- views ---- */
function show(view) {
  $$(".tab").forEach(t => t.classList.toggle("active", t.dataset.view === view));
  $$(".view").forEach(v => v.classList.toggle("active", v.id === "view-" + view));
  if (view === "mods") refreshMods();
  if (view === "setup") loadFolders();
  if (view === "update" && !(state && state.update)) checkUpdate();
  try { localStorage.setItem("oslView", view); } catch (e) { /* (no storage: fine) */ }
}
$$(".tab").forEach(t => t.addEventListener("click", () => show(t.dataset.view)));
window.gpBack = () => { if (!$(".tab.active") || $(".tab.active").dataset.view !== "play") { show("play"); $("#play").focus(); } };

/* ---- a question with buttons: resolves with the button's value ---- */
function ask(title, text, buttons) {
  return new Promise(resolve => {
    const d = $("#ask");
    $("#ask-title").textContent = title;
    $("#ask-text").textContent = text;
    const box = $("#ask-buttons");
    box.innerHTML = "";
    for (const [value, label, primary] of buttons) {
      const b = document.createElement("button");
      b.textContent = label;
      if (primary) b.className = "primary";
      b.onclick = () => { d.close(); resolve(value); };
      box.appendChild(b);
    }
    d.onclose = () => resolve(null);
    d.showModal();
    box.firstChild && box.firstChild.focus();
  });
}

/* ---- jobs: a program running, its log in a dialog ---- */
async function follow(job) {
  const d = $("#job");
  $("#job-title").textContent = job.title;
  $("#job-log").textContent = "";
  $("#job-detail").textContent = "";
  $("#job-detail").className = "said";
  $("#job-cancel").hidden = false;
  $("#job-close").hidden = true;
  let finished = false;
  d.oncancel = e => { if (!finished) e.preventDefault(); };   /* (Escape / B: not while it runs - Stop stops it) */
  if (!d.open) d.showModal();
  $("#job-cancel").focus();
  $("#job-cancel").onclick = () => api("/api/job/cancel", { id: job.id }).catch(() => {});
  for (;;) {
    let j;
    try { j = await api("/api/job/" + job.id); } catch (e) { await sleep(1500); continue; }
    const log = $("#job-log");
    const atEnd = log.scrollTop + log.clientHeight >= log.scrollHeight - 30;
    log.textContent = j.log.join("\n");
    if (atEnd) log.scrollTop = log.scrollHeight;
    $(".bar").classList.toggle("busy", j.progress === null && !j.done);
    $("#job-bar").style.width = j.progress !== null ? Math.round(j.progress * 100) + "%" : "0";
    const last = [...j.log].reverse().find(l => l.trim() && !l.startsWith("$ "));
    if (!j.done) $("#job-detail").textContent = last || "starting…";
    if (j.done) {
      finished = true;
      $("#job-detail").textContent = j.ok ? "Done." : (j.error || "It stopped.");
      $("#job-detail").className = "said " + (j.ok ? "ok" : "bad");
      $("#job-cancel").hidden = true;
      $("#job-close").hidden = false;
      $("#job-close").focus();
      await new Promise(r => { $("#job-close").onclick = () => { d.close(); r(); }; d.onclose = r; });
      refresh();
      return j;
    }
    await sleep(800);
  }
}
const sleep = ms => new Promise(r => setTimeout(r, ms));

async function run(path, body) {
  try {
    const job = await api(path, body || {});
    return await follow(job);
  } catch (e) { await ask("It didn't start", e.message, [["ok", "OK", true]]); return null; }
}

/* ---- the state ---- */
async function refresh() {
  try { state = await api("/api/state"); } catch (e) { $("#play-state").textContent = e.message; return; }
  document.documentElement.className = (document.documentElement.className.replace(/host-\w+/g, "") + " host-" + state.host).trim();
  $("#version").textContent = state.commit ? `version ${state.commit}` : "";
  const built = state.built;
  $("#play-buttons").hidden = !built;
  $("#not-built").hidden = built;
  $("#play-state").textContent = !built ? (state.files_ready ? "Your game files are read; the game itself isn't built yet."
    : "Set the game up first.") : state.game_running ? "The game is running." : `Ready (${state.program}).`;
  const s = state.settings;
  const lobby = { public: "the public lobby", private: `a private lobby (code ${s.code})`, custom: `your own server, ${s.server || "127.0.0.1:39420"}`, offline: "offline" }[s.lobby];
  $("#play-online").textContent = s.lobby === "offline" ? "Offline: Play doesn't go online." :
    `Play joins ${lobby}${s.name ? ", as " + s.name : ""}. Local Wireless lists everyone's rooms.`;
  $("#game-box").hidden = !state.game_running;
  ["play", "play-offline", "play-vanilla"].forEach(id => $("#" + id).disabled = state.game_running || !!state.job);
  fillSettings(s, state.choices);
  if (state.job && !$("#job").open) follow(state.job);
  if (state.game_running) pollGame();
  refreshModsSummary();
}

let modsCache = null;
async function refreshModsSummary() {
  if (!state.python) { $("#play-mods").textContent = "After the setup."; return; }
  try { modsCache = await api("/api/mods/status"); } catch (e) { $("#play-mods").textContent = e.message; return; }
  const m = modsCache;
  if (!m.available) { $("#play-mods").textContent = m.error || "Not available yet."; return; }
  $("#play-mods").textContent = m.installed ? `${m.on} of ${m.installed} turned on` + (m.needs_build ? " · a plugin build is needed" : "") : "None installed.";
  $("#play-vanilla").hidden = !m.on;
}

async function refreshMods() {
  if (!state) return;
  if (!state.python) { $("#mods-state").textContent = "The mod manager comes with the setup: run it first."; $("#mods-open").disabled = true; return; }
  $("#mods-open").disabled = false;
  await refreshModsSummary();
  const m = modsCache || {};
  $("#mods-state").textContent = !m.available ? (m.error || "…") : m.installed ? `${m.on} of ${m.installed} mods turned on.` : "No mods installed yet.";
  $("#mods-build").hidden = !(m.available && m.needs_build && state.built);
  $("#mods-list").innerHTML = (m.names || []).map(n => `<li>${esc(n)}</li>`).join("") +
    (m.needs_build ? `<li class="said bad">Plugins to build into the game: ${esc(m.needs_build)}</li>` : "");
}

/* ---- play ---- */
async function play(mode, extra) {
  try {
    const r = await api("/api/play", { mode, ...(extra || {}) });
    if (r.ask_build) {
      const waiting = r.waiting && r.waiting.length ? "\n\nWaiting: " + r.waiting.join(", ") : "";
      const choice = await ask("The game needs a build first",
        `${r.reason}.${waiting}\n\nBuilding takes a few minutes (longer after an update). Without it, a moveset's fighter plays as the original.`,
        [["build", "Build, then play", true], ["anyway", "Play anyway"], [null, "Cancel"]]);
      if (choice) return play(mode, { [choice]: true });
      return;
    }
    if (r.job) { const j = await follow(r.job); if (!j || !j.ok) return; }
    pollGame();
    refresh();
  } catch (e) { await ask("The game didn't start", e.message, [["ok", "OK", true]]); }
}
$("#play").onclick = () => play("online");
$("#play-offline").onclick = () => play("offline");
$("#play-vanilla").onclick = () => play("vanilla");
$("#go-setup").onclick = () => show("setup");

let gamePolling = false;
async function pollGame() {
  if (gamePolling) return;
  gamePolling = true;
  try {
    for (;;) {
      const g = await api("/api/game");
      $("#game-box").hidden = !g.running;
      $("#game-log").textContent = (g.log || []).join("\n");
      $("#game-log").scrollTop = 1e9;
      if (!g.running) break;
      await sleep(2000);
    }
  } catch (e) { /* (the server ended) */ }
  gamePolling = false;
  refresh();
}

/* ---- setup ---- */
let foldersLoaded = false;
async function loadFolders() {
  if (foldersLoaded) return;
  try {
    const r = await api("/api/setup/folders");
    for (const k of ["game", "keys", "firmware"]) if (!$("#f-" + k).value) $("#f-" + k).value = r.folders[k] || "";
    showChecks(r.checks);
    foldersLoaded = true;
  } catch (e) { $("#c-game").textContent = e.message; }
  if (state && !$("#f-name").value) $("#f-name").value = state.settings.name || "";
  $("#build-run").hidden = !(state && state.files_ready);
}
function showChecks(c) {
  for (const k of ["game", "keys", "firmware"]) {
    const el = $("#c-" + k);
    el.textContent = c[k] ? c[k].say : "";
    el.className = "check " + (c[k] && c[k].ok ? "ok" : "bad");
  }
}
let checkTimer = 0;
["game", "keys", "firmware"].forEach(k => $("#f-" + k).addEventListener("input", () => {
  clearTimeout(checkTimer);
  checkTimer = setTimeout(async () => { try { showChecks((await api("/api/setup/check", folders())).checks); } catch (e) { /* */ } }, 500);
}));
const folders = () => ({ game: $("#f-game").value.trim(), keys: $("#f-keys").value.trim(), firmware: $("#f-firmware").value.trim() });
$("#setup-run").onclick = async () => {
  const f = folders();
  if (!f.keys || !f.game) {
    if (!state.files_ready) { await ask("Folders missing", "Choose the game folder and the keys folder first.", [["ok", "OK", true]]); return; }
  }
  await run("/api/setup/run", { ...(state.files_ready && !f.game ? {} : f), name: $("#f-name").value.trim(),
    jobs: $("#f-jobs").value || undefined, no_packages: $("#f-nopackages").checked });
};
$("#build-run").onclick = () => run("/api/build", { jobs: $("#f-jobs").value || undefined });

/* the folder picker (a pad can't use the system's file dialogs, and a page can't learn a folder's path from them) */
let pickFor = null, pickPath = "";
async function pick(path) {
  try {
    const r = await api("/api/browse?path=" + encodeURIComponent(path || "~"));
    pickPath = r.path;
    $("#picker-path").textContent = r.path + (r.files ? `  (${r.files} game or key file(s) here)` : "");
    $("#picker-up").disabled = !r.parent;
    $("#picker-up").onclick = () => pick(r.parent);
    $("#picker-home").onclick = () => pick(r.home);
    const list = $("#picker-list");
    list.innerHTML = "";
    for (const name of r.dirs) {
      const li = document.createElement("li");
      const b = document.createElement("button");
      b.textContent = "\u{1F4C1} " + name;
      b.onclick = () => pick(r.path.replace(/\/$/, "") + "/" + name);
      li.appendChild(b);
      list.appendChild(li);
    }
    (list.querySelector("button") || $("#picker-use")).focus();
  } catch (e) { $("#picker-path").textContent = e.message; }
}
$$(".browse").forEach(b => b.addEventListener("click", () => {
  pickFor = b.dataset.for;
  $("#picker").showModal();
  pick($("#f-" + pickFor).value || "~");
}));
$("#picker-use").onclick = () => {
  $("#f-" + pickFor).value = pickPath;
  $("#f-" + pickFor).dispatchEvent(new Event("input"));
  $("#picker").close();
  $(`.browse[data-for="${pickFor}"]`).focus();
};
$("#picker-cancel").onclick = () => $("#picker").close();

/* ---- mods ---- */
$("#mods-open").onclick = async () => {
  try {
    const r = await api("/api/mods/open", { back: location.href.split("#")[0] });
    location.href = r.url;
  } catch (e) { await ask("WindowSmash Mods didn't open", e.message, [["ok", "OK", true]]); }
};
$("#mods-build").onclick = () => run("/api/mods/build");

/* ---- settings ---- */
let settingsFilled = false;
function fillSettings(s, choices) {
  if (settingsFilled) return;
  const form = $("#settings");
  for (let [key, options] of Object.entries(choices)) {
    const sel = form.elements[key];
    if (!sel) continue;
    const values = options.map(o => o[0]);
    if (!values.includes(s[key]) && s[key]) options = [[s[key], s[key]], ...options];
    sel.innerHTML = options.map(([v, l]) => `<option value="${esc(v)}">${esc(l)}</option>`).join("");
    sel.value = s[key];
  }
  for (const key of ["name", "code", "server", "volume"]) form.elements[key].value = s[key] || (key === "volume" ? 100 : "");
  $("#volume-out").textContent = s.volume;
  lobbyFields();
  settingsFilled = true;
}
function lobbyFields() {
  const lobby = $("#settings").elements.lobby.value;
  $(".lobby-code").hidden = !(lobby === "private" || lobby === "custom");
  $(".lobby-server").hidden = lobby !== "custom";
}
$("#settings").elements.lobby.addEventListener("change", lobbyFields);
$("#settings").elements.volume.addEventListener("input", e => $("#volume-out").textContent = e.target.value);
$("#settings").addEventListener("submit", async e => {
  e.preventDefault();
  const body = Object.fromEntries(new FormData($("#settings")).entries());
  try {
    const s = await api("/api/settings", body);
    $("#settings-said").textContent = "Saved.";
    $("#settings-said").className = "said ok";
    settingsFilled = false;
    fillSettings(s, state.choices);
    refresh();
  } catch (err) {
    $("#settings-said").textContent = err.message;
    $("#settings-said").className = "said bad";
  }
});

/* ---- update ---- */
async function checkUpdate() {
  $("#update-state").textContent = "Asking GitHub…";
  try {
    const u = await api("/api/update");
    $("#update-state").textContent = u.say;
    $("#update-titles").innerHTML = (u.titles || []).map(t => `<li>${esc(t)}</li>`).join("");
    $("#update-run").hidden = u.state !== "available";
    if (state) state.update = u;
  } catch (e) { $("#update-state").textContent = e.message; }
}
$("#update-check").onclick = checkUpdate;
$("#update-run").onclick = async () => {
  const j = await run("/api/update");
  if (j && j.ok) {
    await checkUpdate();
    const go = await ask("Updated", "OpenSmash is updated. Build what changed now? (Play asks for it too.)",
      [["build", "Build now", true], [null, "Later"]]);
    if (go) run("/api/build");
  }
};

/* ---- Discord: sign-in and the player's GSP and position (discord_account.py) ---- */
let discordTimer = null;
async function refreshDiscord() {
  let d;
  try { d = await api("/api/discord"); } catch (e) { $("#discord-box").innerHTML = `<p class="muted">${esc(e.message)}</p>`; return; }
  const box = $("#discord-box");
  if (!d.signed_in) {
    box.innerHTML = `<p class="muted">Sign in to show your Discord name, rank and GSP, and to play online as the same player on every PC.</p>
      ${d.error ? `<p class="small" style="color:var(--bad)">${esc(d.error)}</p>` : ""}
      <div class="buttons"><button id="discord-signin" class="primary"${d.busy ? " disabled" : ""}>${d.busy ? "Finish in the browser…" : "Sign in with Discord"}</button></div>`;
    $("#discord-signin").onclick = async () => { await api("/api/discord/signin", {}).catch(() => {}); refreshDiscord(); };
  } else {
    const rows = (d.fighters || []).slice(0, 10).map(f =>
      `<tr><td>${esc(f.name)}</td><td class="num">${f.rank ? "#" + f.rank + " · " : ""}${Number(f.gsp).toLocaleString("en-US")}</td></tr>`).join("");
    box.innerHTML = `<div class="dc-head">${d.avatar ? `<img src="${esc(d.avatar)}" alt="">` : "<img alt=\"\">"}
        <div><span class="dc-name">${esc(d.name)}</span>${d.rank ? `<span class="dc-rank">#${d.rank}</span>` : ""}
        <div class="muted small">${d.rank ? "in the GSP leaderboard" : "not ranked yet"}</div></div></div>
      ${rows ? `<table class="dc-gsp">${rows}</table>` : `<p class="muted small">No online matches yet: play Quickplay to get a GSP.</p>`}
      <div class="buttons"><button id="discord-signout" class="ghost">Sign out</button></div>`;
    $("#discord-signout").onclick = async () => { await api("/api/discord/signout", {}).catch(() => {}); refreshDiscord(); };
  }
  clearTimeout(discordTimer);
  discordTimer = setTimeout(refreshDiscord, d.busy ? 1500 : 60000);
}
refreshDiscord();

/* ---- the window ---- */
$("#quit").onclick = async () => {
  try { await api("/api/quit", {}); } catch (e) { /* */ }
  document.body.innerHTML = "<main><h1>OpenSmash Launcher closed</h1><p class='lead'>You can close this window.</p></main>";
  window.close();
};
setInterval(() => api("/api/ping", {}).catch(() => {}), 15000);
window.addEventListener("pagehide", () => { try { fetch("/api/bye", { method: "POST", headers: { "X-Key": KEY }, keepalive: true }); } catch (e) { /* */ } });

(async () => {
  await refresh();
  let view = "play";
  try { view = localStorage.getItem("oslView") || "play"; } catch (e) { /* */ }
  if (state && !state.built && view === "play" && !state.files_ready) view = "play";
  show(view);
  const shown = el => el.offsetParent !== null;
  const first = [...document.querySelectorAll(".view.active [data-gp-start], .view.active button")].find(shown);
  (first || document.querySelector(".tab.active")).focus();
})();

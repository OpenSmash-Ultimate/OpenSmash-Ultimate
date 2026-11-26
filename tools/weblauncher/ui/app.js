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
  if (view === "competitive") refreshRanked();
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
  const rl = $("#report-link");
  if (rl) rl.href = "https://github.com/OpenSmash-Ultimate/OpenSmash-Ultimate/issues/new?template=bug_report.yml" +
    (state.commit ? "&version=" + encodeURIComponent(state.commit) : "");
  const built = state.built;
  $("#play-buttons").hidden = !built;
  $("#not-built").hidden = built;
  $("#play-state").textContent = !built ? (state.files_ready ? "Your game files are read; the game itself isn't built yet."
    : "Set the game up first.") : state.game_running ? "The game is running." : `Ready (${state.program}).`;
  const s = state.settings;
  const where = s.region_shown || ({ eu: "Europe", "na-east": "North America East", sa: "South America" })[s.region] || "";
  const lobby = { public: "the official lobby" + (where ? ` (${where})` : ""), private: `a private lobby (code ${s.code})`, custom: `your own server, ${s.server || "127.0.0.1:39420"}`, lan: "the players on this network (LAN)", offline: "offline" }[s.lobby];
  $("#play-online").textContent = s.lobby === "offline" ? "Offline: Play doesn't go online." :
    `Play joins ${lobby}${s.name ? ", as " + s.name : ""}. Local Wireless lists everyone's rooms.`;
  // (v0.5.0: the official lobbies need a signed-in launcher's lobby ticket; this page cannot sign in)
  const acc = $("#play-account");
  if (acc) { acc.textContent = state.account_say || ""; acc.hidden = !state.account_say; }
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
$("#rebuild-all-run").onclick = () => run("/api/rebuild-all", { jobs: $("#f-jobs").value || undefined });

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
  } catch (e) { await ask("OpenSmash Mods didn't open", e.message, [["ok", "OK", true]]); }
};
$("#mods-build").onclick = () => run("/api/mods/build");

/* ---- settings ---- */
/* The files are the truth: the game's settings menu (Esc) writes them too. The form is filled again from them at each
   refresh (the window back in front, the game closed), except the fields changed here and not saved yet; Save sends
   only those, so it never writes back a value the game changed meanwhile. */
let filled = null;   // (the values the form was last filled with)
const formValues = () => Object.fromEntries(new FormData($("#settings")).entries());
function fillSettings(s, choices, all) {
  const form = $("#settings");
  const now = filled ? formValues() : {};
  const kept = new Set(all || !filled ? [] : Object.keys(now).filter(k => now[k] !== filled[k]));
  for (let [key, options] of Object.entries(choices)) {
    const sel = form.elements[key];
    if (!sel || kept.has(key)) continue;
    const values = options.map(o => o[0]);
    if (!values.includes(s[key]) && s[key]) options = [[s[key], s[key]], ...options];
    const html = options.map(([v, l]) => `<option value="${esc(v)}">${esc(l)}</option>`).join("");
    if (sel.dataset.options !== html) { sel.innerHTML = html; sel.dataset.options = html; }   // (an open list is not rebuilt)
    if (sel.value !== s[key]) sel.value = s[key];
  }
  for (const key of ["name", "code", "server", "volume"])
    if (!kept.has(key)) form.elements[key].value = s[key] || (key === "volume" ? 100 : "");
  $("#volume-out").textContent = form.elements.volume.value;
  lobbyFields();
  const after = formValues();
  for (const k of kept) after[k] = filled[k];
  filled = after;
}
/* The Online rows (OpenSmash.Core/OnlineSettings.cs, the same in every launcher): the official servers (the official lobby,
   the private lobby) are relay only - no Direct connections; the official lobby has no code and no server of one's own,
   the private lobby no server of one's own: hidden and emptied, so nothing stale is saved. Leaving them for a server of
   one's own or the LAN shows those rows again, empty, with Direct connections back to "On". */
const OFFICIAL_SERVERS = ["public", "private"];
let shownLobby = null;
function lobbyFields() {
  const f = $("#settings").elements, lobby = f.lobby.value;
  $(".lobby-code").hidden = !(lobby === "private" || lobby === "custom");
  $(".lobby-server").hidden = lobby !== "custom";
  $(".lobby-region").hidden = !OFFICIAL_SERVERS.includes(lobby) || !!(state && state.settings.region_shown);   // (an override: no regions)
  $(".lobby-direct").hidden = !(lobby === "custom" || lobby === "lan");
  if (lobby === "public") f.code.value = "";
  if (OFFICIAL_SERVERS.includes(lobby)) { f.server.value = ""; f.direct.value = "off"; }
  else if (shownLobby && OFFICIAL_SERVERS.includes(shownLobby) && (lobby === "custom" || lobby === "lan")) f.direct.value = "on";
  shownLobby = lobby;
}
$("#settings").elements.lobby.addEventListener("change", lobbyFields);
$("#settings").elements.volume.addEventListener("input", e => $("#volume-out").textContent = e.target.value);
$("#settings").addEventListener("submit", async e => {
  e.preventDefault();
  const now = formValues();
  const body = Object.fromEntries(Object.entries(now).filter(([k, v]) => !filled || v !== filled[k]));
  try {
    const s = await api("/api/settings", body);
    $("#settings-said").textContent = Object.keys(body).length ? "Saved." : "Nothing changed.";
    $("#settings-said").className = "said ok";
    fillSettings(s, state.choices, true);
    refresh();
  } catch (err) {
    $("#settings-said").textContent = err.message;
    $("#settings-said").className = "said bad";
  }
});
/* (back in front: the game's menu, or a text editor, may have changed the files) */
let lastFront = 0;
function backInFront() {
  if (!state || Date.now() - lastFront < 2000) return;
  lastFront = Date.now();
  refresh();
}
window.addEventListener("focus", backInFront);
document.addEventListener("visibilitychange", () => { if (!document.hidden) backInFront(); });

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

/* ---- Discord: sign-in, the player's GSP and position, their name on the leaderboard (discord_account.py) ---- */
let discordTimer = null;
async function refreshDiscord() {
  let d;
  try { d = await api("/api/discord"); } catch (e) { $("#discord-box").innerHTML = `<p class="muted">${esc(e.message)}</p>`; return; }
  $("#discord-card").hidden = !d.available;
  if (!d.available) return;
  const box = $("#discord-box");
  const error = d.error ? `<p class="small" style="color:var(--bad)">${esc(d.error)}</p>` : "";
  if (!d.signed_in) {
    box.innerHTML = `<p class="muted">Sign in to show your Discord name, position and GSP.</p>${error}
      <div class="buttons"><button id="discord-signin" class="primary"${d.busy ? " disabled" : ""}>${d.busy ? "Finish in the browser…" : "Sign in with Discord"}</button></div>`;
    $("#discord-signin").onclick = async () => { await api("/api/discord/signin", {}).catch(() => {}); refreshDiscord(); };
  } else if (masteryOn) {
    /* (mastery: the points per fighter are in the Mastery card; no place, mastery is not a ranking) */
    box.innerHTML = `<div class="dc-head">${d.avatar ? `<img src="${esc(d.avatar)}" alt="">` : "<img alt=\"\">"}
        <div><span class="dc-name">${esc(d.name)}</span><div class="muted small">Signed in with Discord</div></div></div>
      <label class="dc-shown"><input type="checkbox" id="discord-shown"${d.shown ? " checked" : ""}> Show my name on the leaderboard</label>${error}
      <div class="buttons"><button id="discord-signout" class="ghost">Sign out</button></div>`;
    $("#discord-shown").onchange = async e => { await api("/api/discord/shown", { shown: e.target.checked }).catch(() => {}); refreshDiscord(); };
    $("#discord-signout").onclick = async () => { await api("/api/discord/signout", {}).catch(() => {}); refreshDiscord(); };
  } else {
    const rows = (d.fighters || []).slice(0, 10).map(f =>
      `<tr><td>${esc(f.name)}</td><td class="num">${f.rank ? "#" + f.rank + " · " : ""}${Number(f.gsp).toLocaleString("en-US")}</td></tr>`).join("");
    box.innerHTML = `<div class="dc-head">${d.avatar ? `<img src="${esc(d.avatar)}" alt="">` : "<img alt=\"\">"}
        <div><span class="dc-name">${esc(d.name)}</span>${d.rank ? `<span class="dc-rank">#${d.rank}</span>` : ""}
        <div class="muted small">${d.rank ? "in the GSP leaderboard" : "not ranked yet"}</div></div></div>
      ${rows ? `<table class="dc-gsp">${rows}</table>` : `<p class="muted small">No online matches yet: play Quickplay to get a GSP.</p>`}
      <label class="dc-shown"><input type="checkbox" id="discord-shown"${d.shown ? " checked" : ""}> Show my name on the leaderboard</label>${error}
      <div class="buttons"><button id="discord-signout" class="ghost">Sign out</button></div>`;
    $("#discord-shown").onchange = async e => { await api("/api/discord/shown", { shown: e.target.checked }).catch(() => {}); refreshDiscord(); };
    $("#discord-signout").onclick = async () => { await api("/api/discord/signout", {}).catch(() => {}); refreshDiscord(); };
  }
  clearTimeout(discordTimer);
  discordTimer = setTimeout(refreshDiscord, d.busy ? 1500 : 60000);
}

/* ---- mastery points (discord_account.py mastery_view; OPENSMASH_MASTERY_UI=0: GSP as before), each fighter with its stock
   icon; the top mastery with the site's full list. SSBU Arena has its own Competitive page (refreshRanked). ---- */
let masteryOn = false, masteryTimer = null;
const num = n => Number(n || 0).toLocaleString("en-US");
const SITE_MASTERY = "https://denkishi.github.io/opensmash-site/leaderboard/";
/* a fighter's stock icon (as SSBU Arena serves it), or its initial when there is none or it fails to load */
function ficon(f) {
  if (!f) return "";
  const initial = `<span class="ficon initial" title="${esc(f.name)}">${esc((f.name || "?").charAt(0).toUpperCase())}</span>`;
  return f.icon ? `<img class="ficon" src="${esc(f.icon)}" alt="" title="${esc(f.name)}" loading="lazy" referrerpolicy="no-referrer"
    onerror="this.outerHTML='${initial.replace(/'/g, "&#39;").replace(/"/g, "&quot;")}'">` : initial;
}
async function refreshMastery() {
  let m;
  try { m = await api("/api/mastery"); } catch (e) { return; }
  masteryOn = !!m.on;
  $("#mastery-card").hidden = !m.on;
  if (!m.on) return;
  const rows = (m.fighters || []).slice(0, 10).map(f =>
    `<tr><td>${ficon({ name: f.name, icon: f.icon })}${esc(f.name)}</td><td class="num">${num(f.points)}</td><td class="num muted">${num(f.games)}</td><td class="num muted">${num(f.wins)}</td></tr>`).join("");
  const top = (m.top || []).map((r, i) => `<tr><td class="num muted ms-rank">${i + 1}</td><td>${ficon(r.fighter)}${esc(r.name)}</td><td class="num">${num(r.points)}</td></tr>`).join("");
  $("#mastery-box").innerHTML = (!m.ticket && !m.available
    ? `<p class="muted small">Sign in to play online: your mastery points show here.</p>`
    : !m.available ? `<p class="muted small">Mastery points are not available on the lobby yet.</p>`
    : `<p><span class="ms-total">${num(m.total)}</span> <span class="muted small">points in all · ${num(m.games)} games · ${num(m.wins)} wins</span></p>
       ${rows ? `<table class="dc-gsp ms-table"><tr class="muted small"><th></th><th class="num">Points</th><th class="num">Games</th><th class="num">Wins</th></tr>${rows}</table>`
              : `<p class="muted small">No mastery points yet: play Quickplay or Battle Arena online.</p>`}
       <p class="muted small">Points for playing, they only go up. Ranked lives in SSBU Arena (Competitive).</p>`)
    + (m.top_on ? `<h4 class="ms-top">Top mastery</h4>${top ? `<table class="dc-gsp">${top}</table>` : `<p class="muted small">Not available now.</p>`}
       <a class="ms-full" href="${SITE_MASTERY}" target="_blank" rel="noopener">Full leaderboard →</a>` : "");
  clearTimeout(masteryTimer);
  masteryTimer = setTimeout(whenShown("mastery", refreshMastery), 60000);
}
/* (a page in the background asks nothing: what it skipped is asked when it is shown again. The accounts service's part,
   the top mastery and the ranked board, is asked at most every 5 minutes by the launcher itself: discord_account.py) */
const skipped = {};
function whenShown(name, fn) { return () => { if (document.hidden) skipped[name] = fn; else fn(); }; }
document.addEventListener("visibilitychange", () => {
  if (document.hidden) return;
  for (const [name, fn] of Object.entries(skipped)) { delete skipped[name]; fn(); }
});
/* ---- the Competitive page: SSBU Arena's ranked top 10 (discord_account.py ranked_view), All / EU / NA East / SA ---- */
let rankedRegion = "", rankedTimer = null;
async function refreshRanked() {
  let r;
  try { r = await api("/api/ranked?region=" + encodeURIComponent(rankedRegion)); } catch (e) { r = { rows: null, failed: true }; }
  if (r.arena_url) $("#arena-get").href = r.arena_url;
  if (r.leaderboard_url) $("#arena-full").href = r.leaderboard_url;
  const rows = r.rows || [];
  $("#ranked-box").innerHTML = !r.rows
    ? `<p class="muted">${r.failed ? "The ranked board could not be read. It is asked again in a few minutes." : "Loading…"}</p>`
    : !rows.length ? `<p><b>No ranked results yet.</b></p><p class="muted small">Ranked 1v1 has just started. Players show here after their placement games in SSBU Arena.</p>`
    : `<table class="ranked"><tr><th class="num">#</th><th>Fighters</th><th>Player</th><th>Tier</th><th class="num">W / L</th><th class="num">Rating</th></tr>${rows.map((x, i) =>
        `<tr><td class="pos">${x.rank || i + 1}</td><td>${(x.fighters || []).map(ficon).join("")}</td><td><b>${esc(x.name)}</b><span class="tag">#${esc(x.tag)}</span></td>
         <td><span class="tier" style="color:${esc(x.tier_color)}">${esc(x.tier)}</span></td><td class="num muted">${num(x.wins)} / ${num(x.losses)}</td><td class="num rating">${num(x.rating)}</td></tr>`).join("")}</table>`;
  clearTimeout(rankedTimer);
  if ($("#view-competitive").classList.contains("active")) rankedTimer = setTimeout(whenShown("ranked", refreshRanked), 60000);
}
$$("#ranked-regions button").forEach(b => b.addEventListener("click", () => {
  rankedRegion = b.dataset.region;
  $$("#ranked-regions button").forEach(x => x.classList.toggle("active", x === b));
  $("#ranked-box").innerHTML = `<p class="muted">Loading…</p>`;
  refreshRanked();
}));refreshMastery().then(refreshDiscord);

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

// WindowSmash Mods: the page. It asks the app's own server (server.py) for everything.
"use strict";
const KEY = new URLSearchParams(location.search).get("key") || "";
const $ = (s, el = document) => el.querySelector(s);
const $$ = (s, el = document) => [...el.querySelectorAll(s)];
const esc = (s) => String(s ?? "").replace(/[&<>"']/g, (c) => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;" }[c]));
const mb = (n) => n >= 1073741824 ? (n / 1073741824).toFixed(1) + " GB" : n >= 1048576 ? (n / 1048576).toFixed(1) + " MB" : Math.max(1, Math.round(n / 1024)) + " KB";
const ONLINE = { unsafe: "⚠ Not online safe", same: "👥 Everyone needs it online", caution: "Check before online", safe: "✓ Online safe" };
const SAME_TIP = "Online, every player in the room needs this mod too (the same version, turned on): each game plays the same match and results screen in step, and a game without it can hang there. Offline it's fine.";
function onlineBadge(o, showSafe = false) {
  if (!o || o.level === "unknown" || (o.level === "safe" && !showSafe)) return "";
  const why = o.reasons && o.reasons.length ? o.reasons.join("; ") : "";
  const tip = o.level === "unsafe" ? `Don't use this in public online lobbies${why ? ": " + why : ""}. It's fine offline, and with friends who have the same mods.`
    : o.level === "same" ? `It changes more than the fighter's look${why ? ": " + why : ""}. ${SAME_TIP}`
    : o.level === "caution" ? `This may not be safe in public online lobbies${why ? ": " + why : ""}.` : why || "Its author says it's online safe.";
  return `<span class="online ${o.level}" title="${esc(tip)}">${ONLINE[o.level]}</span>`;
}
function onlineBanner(o) {
  if (!o || o.level === "unknown") return "";
  const head = { unsafe: "Not online safe", same: "Online, everyone needs it", caution: "Check before playing online", safe: "Online safe" }[o.level];
  const body = o.level === "unsafe" ? "Don't use it in public online lobbies (Quickplay, Elite, public arenas): " + o.reasons.join("; ") + ". Offline, or online with friends who have the same mods, it's fine."
    : o.level === "same" ? "It changes more than the fighter's look: " + o.reasons.join("; ") + ". " + SAME_TIP
    : o.level === "caution" ? "It may not be safe in public online lobbies: " + o.reasons.join("; ") + "." : (o.reasons[0] || "") + ".";
  return `<div class="online-banner ${o.level}"><div class="ic">${o.level === "safe" ? "✅" : o.level === "unsafe" ? "⛔" : o.level === "same" ? "👥" : "⚠️"}</div><div><b>${head}</b><span>${esc(body)}</span></div></div>`;
}
const short = (n) => n >= 1000 ? (n / 1000).toFixed(1).replace(/\.0$/, "") + "k" : String(n);

async function api(path, body) {
  const r = await fetch(path, {
    method: body === undefined ? "GET" : "POST",
    headers: { "X-Key": KEY, "Content-Type": "application/json" },
    body: body === undefined ? undefined : JSON.stringify(body),
  });
  const data = await r.json().catch(() => ({ error: `HTTP ${r.status}` }));
  if (!r.ok || data.error) throw new Error(data.error || `HTTP ${r.status}`);
  return data;
}

function toast(text, ms = 3200) {
  const t = $("#toast");
  t.textContent = text;
  t.classList.remove("hidden");
  clearTimeout(toast.timer);
  toast.timer = setTimeout(() => t.classList.add("hidden"), ms);
}
const fail = (e) => toast("⚠️ " + (e.message || e), 6000);

// ---- views ----------------------------------------------------------------------------------
function show(view) {
  $$(".tab").forEach((t) => t.classList.toggle("active", t.dataset.view === view));
  $$(".view").forEach((v) => v.classList.toggle("active", v.id === "view-" + view));
  if (view === "browse" && !state.gbLoaded) loadBrowse();
  if (view === "packs") loadPacks();
  if (view === "settings") showSettings();
}
$$(".tab").forEach((t) => t.addEventListener("click", () => show(t.dataset.view)));
document.addEventListener("click", (e) => {
  const g = e.target.closest("[data-goto]");
  if (g) { e.preventDefault(); show(g.dataset.goto); }
  if (e.target.closest("[data-close]")) closeOverlays();
});
document.addEventListener("keydown", (e) => { if (e.key === "Escape") closeOverlays(); });
function closeOverlays() { $("#drawer").classList.add("hidden"); $("#dialog").classList.add("hidden"); }

const state = { lib: null, picked: new Set(), gbLoaded: false, gbPage: 1, gbCat: null, gbFighter: null, installedIds: new Set() };

// ---- my mods --------------------------------------------------------------------------------
async function loadLibrary() {
  try { render(await api("/api/library")); } catch (e) { fail(e); }
}

function render(lib) {
  state.lib = lib;
  state.installedIds = new Set(lib.mods.filter((m) => m.gamebanana).map((m) => +m.gamebanana));
  const on = lib.mods.filter((m) => m.enabled);
  $("#count-on").textContent = lib.mods.length ? `${on.length}/${lib.mods.length}` : "";
  const gs = $("#game-state");
  gs.textContent = lib.game_running ? "● Game is open" : "Game closed";
  gs.classList.toggle("on", lib.game_running);
  const applied = lib.applied || [];
  const same = lib.modded && applied.length === on.length && on.every((m, i) => applied[i] === m.name);
  const as = $("#applied-state");
  as.textContent = !lib.modded ? (on.length ? "The game runs without mods: apply to use them." : "The game runs without mods.")
    : same ? `The game uses these ${on.length} mods.` : "Changed since the last apply.";
  as.classList.toggle("stale", !same && on.length > 0);
  const os = $("#online-state");
  const unsafeOn = lib.unsafe_on || [], sameOn = lib.same_on || [];
  os.classList.toggle("on", !!(unsafeOn.length || sameOn.length));
  os.classList.remove("same-only");
  // (short: the rail keeps Apply in reach; which mods, on hover)
  os.innerHTML = unsafeOn.length || sameOn.length
    ? "<b>⚠ Not for public online</b>Your mods aren't safe for public online. Make sure everyone has the same mods so no one crashes." : "";
  os.title = [unsafeOn.length ? "Not online safe: " + unsafeOn.join(", ") : "", sameOn.length ? "Everyone needs the same: " + sameOn.join(", ") : ""]
    .filter(Boolean).join("\n\n");
  $("#apply").disabled = lib.game_running;
  const unbuilt = lib.unbuilt || [];
  $("#build-plugins").hidden = !unbuilt.length;
  $("#build-plugins").disabled = lib.game_running;
  $("#build-plugins").title = `${unbuilt.join(", ")}: ${unbuilt.length > 1 ? "their plugins aren't" : "its plugin isn't"} in the game yet. Compiles ${unbuilt.length > 1 ? "them" : "it"} in (a few minutes).`;
  $("#vanilla").disabled = lib.game_running || !lib.modded;

  const q = $("#lib-filter").value.trim().toLowerCase();
  const list = $("#lib-list");
  list.innerHTML = "";
  $("#lib-empty").classList.toggle("hidden", lib.mods.length > 0);
  const shown = lib.mods.filter((m) => !q || ((m.title || m.name) + " " + (m.part || "") + " " + (m.category || "") + " " + (m.author || "") + " " + (m.about || "")).toLowerCase().includes(q));
  const groups = {};   // (parts of one download, shown under one row: its own parts' list opens below it)
  for (const m of shown) if (m.group) (groups[m.group] = groups[m.group] || []).push(m);
  for (const m of shown) {
    const parts = m.group && groups[m.group].length > 1 ? groups[m.group] : null;
    if (!parts) { list.appendChild(modRow(m, !q)); continue; }
    if (parts[0] !== m) continue;
    list.appendChild(groupRow(m.group, lib.groups[m.group] || {}, parts, !q));
  }
  updatePick();
}

const openGroups = new Set(JSON.parse((() => { try { return localStorage.getItem("openGroups") || "[]"; } catch (e) { return "[]"; } })()));
function groupRow(key, g, parts, draggable) {
  const box = document.createElement("div");
  const on = parts.filter((p) => p.enabled).length;
  box.className = "group" + (openGroups.has(key) ? " open" : "") + (on ? "" : " off");
  const img = g.image || "";
  box.innerHTML = `
    <div class="mod head">
      <button class="caret ghost small" title="Show its parts">▸</button>
      <div></div>
      ${img ? `<img class="thumb" src="${esc(img)}" alt="" loading="lazy">` : '<div class="thumb"></div>'}
      <div>
        <div class="title">${esc(g.title || parts[0].title || parts[0].name)}</div>
        <div class="meta"><span>${parts.length} parts in one download</span><span>${on} on</span>${
          parts.some((p) => p.online && p.online.level === "unsafe") ? onlineBadge({ level: "unsafe", reasons: [] }) : ""}</div>
        <div class="about">${esc(parts.map((p) => p.part).join(" · "))}</div>
      </div>
      <div class="end">
        <label class="switch${on > 0 && on < parts.length ? " mixed" : ""}" title="All its parts on / off"><input type="checkbox" ${on === parts.length ? "checked" : ""}><span></span></label>
      </div>
    </div>
    <div class="parts"></div>`;
  const all = $(".head .switch input", box);
  all.indeterminate = on > 0 && on < parts.length;
  all.addEventListener("change", (e) => setEnabled(parts.map((p) => p.name), e.target.checked));
  const toggle = () => {
    box.classList.toggle("open");
    box.classList.contains("open") ? openGroups.add(key) : openGroups.delete(key);
    try { localStorage.setItem("openGroups", JSON.stringify([...openGroups])); } catch (e) {}
  };
  $(".caret", box).addEventListener("click", toggle);
  $(".head .title", box).addEventListener("click", toggle);
  for (const p of parts) $(".parts", box).appendChild(modRow(p, draggable));
  return box;
}

function modRow(m, draggable) {
  const title = m.part || m.title || m.name;
  const img = m.part ? "" : m.image || m.local_image || "";
  const el = document.createElement("div");
  el.className = "mod" + (m.part ? " part" : "") + (m.enabled ? "" : " off") + (m.online && m.online.level === "unsafe" ? " unsafe" : "");
  el.draggable = draggable;
  el.dataset.name = m.name;
  const badges = [onlineBadge(m.online)];
  if (m.new_slots) badges.push('<span class="badge gold">new costume slots</span>');
  if (m.added) badges.push(`<span class="badge">${m.added} new files</span>`);
  if (m.merged) badges.push(`<span class="badge">${m.merged} patches</span>`);
  const plugs = m.plugin_state || [], waiting = plugs.filter((p) => !p.built).map((p) => p.file);
  if (waiting.length) badges.push(`<span class="badge red" title="${esc(waiting.join(", ") + ": its code (the new moves, or the new character) runs once it's compiled into the game. Turn it on, then press Build plugins into the game (a few minutes).")}">plugin not built yet</span>`);
  else if (plugs.length) badges.push(`<span class="badge gold" title="${esc(plugs.map((p) => p.file).join(", ") + ": compiled into the game, so its new moves and characters work.")}">plugin built in</span>`);
  if (m.kept_shared) badges.push(`<span class="badge" title="${m.kept_shared} of its files are shared with the fighter's other costumes, and stay shared: made this costume's own, they'd stop those costumes' matches from loading. So its version of them shows on every costume that shares them.">${m.kept_shared} on all costumes</span>`);
  if (m.skipped) badges.push(`<span class="badge red" title="files the game couldn't take: see the clash check">${m.skipped} not loaded</span>`);
  el.innerHTML = `
    <input type="checkbox" class="pick" title="Pick for a modpack" ${state.picked.has(m.name) ? "checked" : ""}>
    <div class="grip" title="Drag to reorder">⋮⋮</div>
    ${img ? `<img class="thumb" src="${esc(img)}" alt="" loading="lazy">` : '<div class="thumb"></div>'}
    <div>
      <div class="title">${esc(title)}</div>
      <div class="meta">${m.category && !m.part ? `<span>${esc(m.category)}</span>` : ""}${m.author && !m.part ? `<span>by ${esc(m.author)}</span>` : ""}${m.files ? `<span>${m.files} files</span>` : ""}${badges.join("")}</div>
      ${m.about ? `<div class="about">${esc(m.about)}</div>` : ""}
    </div>
    <div class="end">
      ${m.gamebanana ? `<button class="ghost small" data-gb="${m.gamebanana}" title="Its GameBanana page">Page</button>` : ""}
      <button class="ghost small danger" data-remove title="Uninstall">✕</button>
      <label class="switch" title="On / off"><input type="checkbox" ${m.enabled ? "checked" : ""}><span></span></label>
    </div>`;
  $(".switch input", el).addEventListener("change", (e) => setEnabled([m.name], e.target.checked));
  $(".pick", el).addEventListener("change", (e) => { e.target.checked ? state.picked.add(m.name) : state.picked.delete(m.name); updatePick(); });
  $("[data-remove]", el).addEventListener("click", () => removeMod(m));
  const gb = $("[data-gb]", el);
  if (gb) gb.addEventListener("click", () => openMod(+gb.dataset.gb));
  dragify(el);
  return el;
}

function updatePick() {
  const b = $("#make-pack");
  b.disabled = state.picked.size === 0 && !(state.lib && state.lib.mods.some((m) => m.enabled));
  b.textContent = state.picked.size ? `Make a modpack (${state.picked.size})` : "Make a modpack";
}

async function setEnabled(names, enabled) {
  try { render(await api("/api/enable", { names, enabled })); } catch (e) { fail(e); }
}
async function removeMod(m) {
  if (!confirm(`Uninstall ${m.title || m.name}? Its files are deleted (it can be downloaded again).`)) return;
  try { render(await api("/api/remove", { names: [m.name] })); toast("Uninstalled"); } catch (e) { fail(e); }
}

let dragged = null;
function dragify(el) {
  el.addEventListener("dragstart", (e) => { dragged = el; el.classList.add("dragging"); e.dataTransfer.effectAllowed = "move"; e.dataTransfer.setData("text/x-mod", el.dataset.name); });
  el.addEventListener("dragend", () => { el.classList.remove("dragging"); $$(".mod.over").forEach((x) => x.classList.remove("over")); dragged = null; });
  el.addEventListener("dragover", (e) => { if (!dragged || dragged === el) return; e.preventDefault(); el.classList.add("over"); });
  el.addEventListener("dragleave", () => el.classList.remove("over"));
  el.addEventListener("drop", async (e) => {
    if (!dragged || dragged === el) return;
    e.preventDefault();
    e.stopPropagation();
    const r = el.getBoundingClientRect();
    el.parentNode.insertBefore(dragged, e.clientY > r.top + r.height / 2 ? el.nextSibling : el);
    const names = $$(".mod[data-name]", $("#lib-list")).map((x) => x.dataset.name);
    try { render(await api("/api/order", { names })); } catch (err) { fail(err); }
  });
}

$("#lib-filter").addEventListener("input", () => state.lib && render(state.lib));

const ICON = { error: "⛔", online: "🌐", warn: "⚠️", info: "ℹ️" };
function renderIssues(issues, into = $("#issues")) {
  if (!issues.length) { into.innerHTML = '<div class="issues-ok"><b>No clashes.</b> These mods should run together.</div>'; return; }
  into.innerHTML = issues.map((i, k) => `<div class="issue ${i.level === "online" ? "online-issue" : i.level}"><div class="ic">${ICON[i.level]}</div><div><b>${esc(i.title)}</b><span>${esc(i.detail)}</span>${
    i.fix ? `<button class="small fix" data-fix="${k}">${esc(i.fix.label)}</button>` : ""}</div></div>`).join("");
  $$(".fix", into).forEach((b) => b.addEventListener("click", async () => {   // (a fix that is a GameBanana mod: installed and turned on)
    const f = issues[+b.dataset.fix].fix;
    b.disabled = true;
    try {
      runJob(await api("/api/install/gamebanana", { id: f.gamebanana, file: f.file, title: f.label }), (j) => {
        if (!j.error) toast("Installed: Apply to game when you're ready");
        loadLibrary();
        api("/api/check", {}).then((c) => renderIssues(c.issues)).catch(() => {});
      });
    } catch (e) { b.disabled = false; fail(e); }
  }));
}
$("#check").addEventListener("click", async () => {
  try { renderIssues((await api("/api/check", {})).issues); } catch (e) { fail(e); }
});

$("#apply").addEventListener("click", async () => {
  try {
    const { issues } = await api("/api/check", {});
    const errors = issues.filter((i) => i.level === "error");
    if (errors.length) {
      renderIssues(issues);
      show("library");
      if (!confirm(`${errors.length} clash${errors.length > 1 ? "es" : ""} that can stop mods loading or crash the game (listed on My mods). Apply anyway?`)) return;
    }
    runJob(await api("/api/apply", {}), () => loadLibrary());
  } catch (e) { fail(e); }
});
$("#build-plugins").addEventListener("click", async () => {
  try { runJob(await api("/api/plugins/build", {}), () => loadLibrary()); } catch (e) { fail(e); }
});
$("#vanilla").addEventListener("click", async () => {
  try { runJob(await api("/api/vanilla", {}), () => loadLibrary()); } catch (e) { fail(e); }
});

// ---- jobs -----------------------------------------------------------------------------------
function stepText(s) {
  s = (s || "").trim();
  if (!s) return "Starting";
  return s.charAt(0).toUpperCase() + s.slice(1);
}

function runJob(job, done) {
  const el = document.createElement("div");
  el.className = "job";
  el.innerHTML = `<div class="jobhead"><b>${esc(job.title)}</b><span class="pct"></span></div><div class="detail"></div><div class="bar spin"><i></i></div>`;
  $("#jobs").appendChild(el);
  const tick = async () => {
    let j;
    try { j = await api("/api/job/" + job.id); } catch (e) { setTimeout(tick, 1500); return; }
    const bar = $(".bar", el);
    const detail = j.detail || (j.log && j.log.length ? j.log[j.log.length - 1] : "");
    $(".detail", el).textContent = j.error ? j.error : stepText(detail);
    if (typeof j.progress === "number" && j.progress > 0) {
      bar.classList.remove("spin");
      const pct = Math.round(j.progress * 100);
      $("i", bar).style.width = pct + "%";
      $(".pct", el).textContent = j.eta ? `${pct}%   ${j.eta}` : `${pct}%`;
    }
    if (!j.done) { setTimeout(tick, 600); return; }
    bar.classList.remove("spin");
    $("i", bar).style.width = "100%";
    if (!j.error) $(".pct", el).textContent = "100%   done";
    if (j.error) {
      el.classList.add("err");
      $(".detail", el).textContent = j.error + (state.lib && state.lib.log ? ` (details: ${state.lib.log})` : "");
      el.title = "Click to close";
      el.addEventListener("click", () => el.remove());
    }
    else { el.classList.add("ok"); setTimeout(() => el.remove(), 4000); }
    done && done(j);
  };
  tick();
}

// ---- GameBanana -----------------------------------------------------------------------------
async function loadBrowse(append = false) {
  state.gbLoaded = true;
  if (!append) state.gbPage = 1;
  const params = new URLSearchParams({ page: state.gbPage, sort: $("#gb-sort").value });
  const q = $("#gb-search").value.trim();
  if (q) params.set("q", q);
  else if (state.gbFighter || state.gbCat) params.set("category", state.gbFighter || state.gbCat);
  const grid = $("#gb-grid");
  if (!append) grid.innerHTML = '<div class="hint">Loading…</div>';
  try {
    if (!$("#gb-cats").children.length) loadCats();
    const r = await api("/api/gb/browse?" + params);
    if (!append) grid.innerHTML = "";
    for (const m of r.mods) grid.appendChild(tile(m));
    if (!r.mods.length && !append) grid.innerHTML = '<div class="empty"><h2>Nothing here</h2><p>Try another search.</p></div>';
    $("#gb-more").classList.toggle("hidden", !r.more);
  } catch (e) { grid.innerHTML = `<div class="empty"><h2>GameBanana didn't answer</h2><p>${esc(e.message)}</p></div>`; }
}

function tile(m) {
  const el = document.createElement("div");
  el.className = "tile";
  el.innerHTML = `${m.image ? `<img src="${esc(m.image)}" alt="" loading="lazy">` : "<img alt=''>"}
    ${state.installedIds.has(m.id) ? '<span class="badge green have">installed</span>' : ""}
    ${onlineBadge(m.online)}
    <div class="tbody"><div class="tname">${esc(m.name)}</div>
    <div class="tmeta"><span>${esc(m.category)}</span><span>♥ ${short(m.likes)}</span><span>👁 ${short(m.views)}</span></div></div>`;
  el.addEventListener("click", () => openMod(m.id));
  return el;
}

async function loadCats() {
  try {
    const cats = await api("/api/gb/categories");
    const box = $("#gb-cats");
    box.innerHTML = "";
    const all = chip("All", null, true);
    box.appendChild(all);
    for (const c of cats) box.appendChild(chip(`${c.name} · ${short(c.count)}`, c.id));
  } catch (e) { /* (browsing still works) */ }
}
function chip(label, id, active = false) {
  const c = document.createElement("button");
  c.className = "chip" + (active ? " active" : "");
  c.textContent = label;
  c.addEventListener("click", async () => {
    $$("#gb-cats .chip").forEach((x) => x.classList.remove("active"));
    c.classList.add("active");
    state.gbCat = id;
    state.gbFighter = null;
    $("#gb-search").value = "";
    const f = $("#gb-fighters");
    f.classList.add("hidden");
    if (id) {   // (a category's subcategories: Skins has one a fighter)
      try {
        const subs = await api("/api/gb/categories?parent=" + id);
        if (subs.length) {
          f.innerHTML = "";
          for (const s of subs) {
            const b = document.createElement("button");
            b.className = "chip";
            b.textContent = `${s.name} · ${short(s.count)}`;
            b.addEventListener("click", () => {
              $$("#gb-fighters .chip").forEach((x) => x.classList.remove("active"));
              b.classList.add("active");
              state.gbFighter = s.id;
              loadBrowse();
            });
            f.appendChild(b);
          }
          f.classList.remove("hidden");
        }
      } catch (e) { /* (none) */ }
    }
    loadBrowse();
  });
  return c;
}
let searchTimer;
$("#gb-search").addEventListener("input", () => { clearTimeout(searchTimer); searchTimer = setTimeout(() => loadBrowse(), 450); });
$("#gb-sort").addEventListener("change", () => loadBrowse());
$("#gb-more").addEventListener("click", () => { state.gbPage++; loadBrowse(true); });

async function openMod(id) {
  const d = $("#mod-detail");
  d.innerHTML = '<div class="hint">Loading…</div>';
  $("#drawer").classList.remove("hidden");
  try {
    const m = await api("/api/gb/mod?id=" + id);
    const have = state.installedIds.has(id);
    d.innerHTML = `<div class="detail">
      <h2>${esc(m.name)}</h2>
      <div class="by">${esc(m.category)}${m.author ? " · by " + esc(m.author) : ""} · ♥ ${short(m.likes)} · ⬇ ${short(m.downloads)} ${have ? '<span class="badge green">installed</span>' : ""}</div>
      ${m.works ? `<div class="online-banner ${m.works.level === "no" ? "" : "caution"}"><div class="ic">🧩</div><div><b>${m.works.level === "no" ? "Won't work in WindowSmash" : "Only partly works in WindowSmash"}</b><span>${esc(m.works.text)}</span></div></div>` : ""}
      ${onlineBanner(m.online)}
      ${m.smash ? "" : '<div class="issue warn"><div class="ic">⚠️</div><div><b>Not a Smash Ultimate mod</b><span>It won\'t work in WindowSmash.</span></div></div>'}
      <div class="gallery">${m.images.map((u) => `<img src="${esc(u)}" alt="" loading="lazy">`).join("")}</div>
      <h3>Files</h3>
      <div class="files">${m.files.map((f) => `<div class="file"><div><b>${esc(f.name)}</b> ${onlineBadge(f.online, true)}<small>${mb(f.size)} · ⬇ ${short(f.downloads)}${f.description ? " · " + esc(f.description) : ""}${f.scan && f.scan !== "clean" ? " · virus scan: " + esc(f.scan) : ""}</small></div>
        <button data-file="${f.id}" ${f.scan && f.scan !== "clean" ? "disabled" : ""}>Install</button></div>`).join("") || '<p class="hint">No files.</p>'}</div>
      <div class="desc">${esc(m.text) || "No description."}</div>
      <p><a href="${esc(m.url)}" target="_blank" rel="noopener">Open on GameBanana</a></p>
    </div>`;
    $$("[data-file]", d).forEach((b) => b.addEventListener("click", async () => {
      b.disabled = true;
      b.textContent = "Installing…";
      try {
        runJob(await api("/api/install/gamebanana", { id, file: +b.dataset.file, title: m.name }), (j) => {
          b.textContent = j.error ? "Install" : "Installed";
          b.disabled = !j.error;
          if (!j.error) toast(`${m.name} installed: turn it on in My mods, then apply`);
          loadLibrary();
        });
      } catch (e) { fail(e); b.disabled = false; b.textContent = "Install"; }
    }));
  } catch (e) { d.innerHTML = `<div class="empty"><h2>Couldn't open it</h2><p>${esc(e.message)}</p></div>`; }
}

// ---- adding mods ----------------------------------------------------------------------------
$("#url-go").addEventListener("click", async () => {
  const url = $("#url").value.trim();
  if (!url) return;
  try { runJob(await api("/api/install/url", { url }), (j) => { if (!j.error) { $("#url").value = ""; toast("Installed"); } loadLibrary(); }); } catch (e) { fail(e); }
});
$("#inst-plain").addEventListener("click", () => makeInstaller(null));
$("#uninstall").addEventListener("click", () => {
  const n = state.lib ? state.lib.mods.length : 0;
  const d = dialog(`<h2>Uninstall WindowSmash Mods?</h2>
    <p class="hint">The game goes back to its own files, the modded game data is deleted and this app is removed. Your save isn't touched.</p>
    <label><input type="checkbox" id="un-mods"> Also delete my ${n} downloaded mod${n === 1 ? "" : "s"} (the mods folder)</label>
    <div class="dialog-actions"><button class="ghost" data-close>Cancel</button><button class="big" id="un-go">Uninstall</button></div>`);
  $("#un-go", d).addEventListener("click", async () => {
    try {
      const job = await api("/api/uninstall", { delete_mods: $("#un-mods", d).checked });
      closeOverlays();
      runJob(job, (j) => {
        if (j.error) return;
        document.body.innerHTML = `<div class="empty" style="height:100vh;display:grid;place-content:center"><h2>WindowSmash Mods is uninstalled</h2><p>${esc(j.log.slice(-6).join(" · "))}</p><p>You can close this window.</p></div>`;
      });
    } catch (e) { fail(e); }
  });
});
$("#file-pick").addEventListener("change", (e) => { uploadFiles([...e.target.files]); e.target.value = ""; });

async function uploadFiles(files) {
  for (const f of files) {
    try {
      const r = await fetch("/api/upload?name=" + encodeURIComponent(f.name), { method: "POST", headers: { "X-Key": KEY }, body: f });
      const job = await r.json();
      if (job.error) throw new Error(job.error);
      runJob(job, (j) => {
        if (j.error) return;
        if (j.result && j.result.pack) { show("packs"); openPack({ pack: j.result.pack }); }
        else toast(`${f.name} installed`);
        loadLibrary();
      });
    } catch (e) { fail(e); }
  }
}
let dragDepth = 0;
const hasFiles = (e) => e.dataTransfer && [...e.dataTransfer.types].includes("Files");
window.addEventListener("dragenter", (e) => { if (hasFiles(e)) { dragDepth++; $("#dropzone").classList.remove("hidden"); } });
window.addEventListener("dragleave", (e) => { if (hasFiles(e) && --dragDepth <= 0) { dragDepth = 0; $("#dropzone").classList.add("hidden"); } });
window.addEventListener("dragover", (e) => { if (hasFiles(e)) e.preventDefault(); });
window.addEventListener("drop", (e) => {
  if (!hasFiles(e)) return;
  e.preventDefault();
  dragDepth = 0;
  $("#dropzone").classList.add("hidden");
  uploadFiles([...e.dataTransfer.files]);
});

// ---- modpacks -------------------------------------------------------------------------------
async function loadPacks() {
  try {
    const { packs } = await api("/api/packs");
    const box = $("#pack-list");
    box.innerHTML = "";
    if (!packs.length) box.innerHTML = '<div class="empty"><h2>No modpacks yet</h2><p>Tick some mods on My mods and make one, or open one a friend sent.</p></div>';
    for (const p of packs) box.appendChild(packTile(p));
  } catch (e) { fail(e); }
}

function packTile(p) {
  const el = document.createElement("div");
  el.className = "tile pack";
  const imgs = p.mods.map((m) => m.image).filter(Boolean).slice(0, 4);
  el.innerHTML = `<div class="pcover">${imgs.map((u) => `<img src="${esc(u)}" alt="">`).join("")}</div>
    <div class="pbody"><div class="pname">${esc(p.name)}</div>
    <div class="pmods">${p.mods.length} mods${p.author ? " · by " + esc(p.author) : ""}<br>${esc(p.mods.map((m) => m.title || m.name).join(", "))}</div>
    ${p.unsafe && p.unsafe.length ? `<div class="punsafe"><span class="online unsafe" title="${esc("Not online safe: " + p.unsafe.join(", "))}">⚠ Not online safe</span></div>` : ""}
    <div class="pactions">
      <button data-use>Use</button>
      <button class="ghost" data-share>Share</button>
      <button class="ghost" data-file>Save file</button>
      <button class="ghost" data-inst title="One download that installs WindowSmash Mods and this pack">Installer</button>
      <button class="ghost danger" data-del>Delete</button>
    </div></div>`;
  $("[data-use]", el).addEventListener("click", () => openPack({ pack: p }));
  $("[data-share]", el).addEventListener("click", () => sharePack(p));
  $("[data-file]", el).addEventListener("click", () => savePackFile(p));
  $("[data-inst]", el).addEventListener("click", () => makeInstaller(p));
  $("[data-del]", el).addEventListener("click", async () => {
    if (!confirm(`Delete the modpack ${p.name}? (Its mods stay installed.)`)) return;
    try { await api("/api/pack/delete", { id: p.id }); loadPacks(); } catch (e) { fail(e); }
  });
  return el;
}

function dialog(html) {
  $("#dialog-body").innerHTML = html;
  $("#dialog").classList.remove("hidden");
  return $("#dialog-body");
}

function newPack() {
  const lib = state.lib;
  const names = state.picked.size ? lib.mods.filter((m) => state.picked.has(m.name)).map((m) => m.name)
    : lib.mods.filter((m) => m.enabled).map((m) => m.name);
  if (!names.length) { toast("Tick some mods (or turn some on) first"); return; }
  const d = dialog(`<h2>New modpack</h2>
    <p class="hint">${names.length} mods, in your order: ${esc(names.join(", "))}</p>
    <label>Name</label><input type="text" id="np-name" value="My modpack">
    <label>Your name (shown to people you share it with)</label><input type="text" id="np-author">
    <label>Description</label><textarea id="np-desc" rows="3"></textarea>
    <div id="np-issues"></div>
    <div class="dialog-actions"><button class="ghost" data-close>Cancel</button><button class="big" id="np-save">Save</button></div>`);
  api("/api/check", { names }).then((r) => renderIssues(r.issues, $("#np-issues", d))).catch(() => {});
  $("#np-save", d).addEventListener("click", async () => {
    try {
      await api("/api/pack/save", { names, name: $("#np-name", d).value.trim() || "My modpack", author: $("#np-author", d).value.trim(), description: $("#np-desc", d).value.trim() });
      closeOverlays();
      state.picked.clear();
      render(state.lib);
      show("packs");
      toast("Modpack saved: share it from here");
    } catch (e) { fail(e); }
  });
}
$("#make-pack").addEventListener("click", newPack);
$("#pack-new").addEventListener("click", () => { if (state.lib) newPack(); });

async function sharePack(p) {
  const code = p.code;
  if (!code) {
    dialog(`<h2>Share ${esc(p.name)}</h2><p>A mod in this pack wasn't downloaded from GameBanana or a link, so a code can't fetch it. Save the pack as a file and send the mod with it.</p><div class="dialog-actions"><button data-close>OK</button></div>`);
    return;
  }
  const d = dialog(`<h2>Share ${esc(p.name)}</h2>
    <p class="hint">Send this code (Discord, a message…). Your friend pastes it into Modpacks; WindowSmash Mods downloads the same files, in the same order.</p>
    ${p.unsafe && p.unsafe.length ? onlineBanner({ level: "unsafe", reasons: [p.unsafe.join(", ") + (p.unsafe.length > 1 ? " aren't" : " isn't") + " online safe: play this pack offline, or online only with friends who have it"] }) : ""}
    <div class="code">${esc(code)}</div>
    <div class="dialog-actions"><button class="ghost" data-close>Close</button><button id="copy">Copy code</button></div>`);
  $("#copy", d).addEventListener("click", async () => {
    try { await navigator.clipboard.writeText(code); toast("Copied"); }
    catch (e) { const r = document.createRange(); r.selectNodeContents($(".code", d)); getSelection().removeAllRanges(); getSelection().addRange(r); toast("Selected: press Ctrl/Cmd+C"); }
  });
}

function savePackFile(p) {
  const pack = { ...p };
  delete pack.id;
  delete pack.code;
  const blob = new Blob([JSON.stringify(pack, null, 1)], { type: "application/json" });
  const a = document.createElement("a");
  a.href = URL.createObjectURL(blob);
  a.download = (p.name || "modpack").replace(/[^\w .()-]+/g, "_") + ".wsmodpack";
  document.body.appendChild(a);
  a.click();
  a.remove();
  toast("Saved to your Downloads folder");
}

function makeInstaller(p) {
  const d = dialog(`<h2>${p ? "Installer for " + esc(p.name) : "WindowSmash Mods installer"}</h2>
    <p class="hint">One .zip for Windows and Mac. Your friend unzips it and double-clicks <b>Install WindowSmash Mods</b>: it adds this mod manager to their WindowSmash${p ? " and sets up the pack's mods, turned on and applied" : ""}.</p>
    ${p ? '<label><input type="checkbox" id="mi-all"> Carry every mod\'s files in the zip (bigger, but nothing to download while installing)</label>' : ""}
    <div class="dialog-actions"><button class="ghost" data-close>Cancel</button><button class="big" id="mi-go">Make it</button></div>`);
  $("#mi-go", d).addEventListener("click", async () => {
    try {
      const job = await api("/api/installer", { id: p ? p.id : null, bundle_all: p ? $("#mi-all", d).checked : false });
      closeOverlays();
      runJob(job, (j) => { if (!j.error) toast(`Saved to Downloads (${mb(j.result.size)}): send it to your friends`, 6000); });
    } catch (e) { fail(e); }
  });
}

$("#pack-read").addEventListener("click", () => {
  const code = $("#pack-code").value.trim();
  if (code) openPack({ code });
});

async function openPack(src) {
  try {
    const r = await api("/api/pack/read", src);
    const p = r.pack;
    const missing = new Set(r.missing.map((m) => m.name));
    const d = dialog(`<h2>${esc(p.name)}</h2>
      <p class="hint">${p.mods.length} mods${p.author ? " · by " + esc(p.author) : ""}${p.description ? "<br>" + esc(p.description) : ""}</p>
      <div class="mini">${p.mods.map((m) => `<div>${esc(m.title || m.name)}<span class="badge ${missing.has(m.name) ? (m.bundled ? "red" : "gold") : "green"}">${missing.has(m.name) ? (m.bundled ? "not here" : "will download") : "installed"}</span></div>`).join("")}</div>
      <label><input type="checkbox" id="op-only" checked> Turn my other mods off (play exactly this pack)</label>
      <label><input type="checkbox" id="op-apply" checked> Apply to the game when it's ready</label>
      <div class="dialog-actions"><button class="ghost" data-close>Cancel</button><button class="big" id="op-go">${missing.size ? "Download & use" : "Use this pack"}</button></div>`);
    $("#op-go", d).addEventListener("click", async () => {
      try {
        const job = await api("/api/pack/use", { ...src, only: $("#op-only", d).checked, apply: $("#op-apply", d).checked });
        closeOverlays();
        runJob(job, (j) => {
          loadLibrary();
          loadPacks();
          if (!j.error) {
            toast(j.result && j.result.left && j.result.left.length ? `Done, without: ${j.result.left.join(", ")}` : `${p.name} is ready`);
            api("/api/check", {}).then((c) => renderIssues(c.issues)).catch(() => {});
          }
        });
      } catch (e) { fail(e); }
    });
  } catch (e) { fail(e); }
}

// ---- settings ---------------------------------------------------------------------------------
function setTheme(theme) {
  document.documentElement.dataset.theme = theme;
  $$("[data-theme-choice]").forEach((b) => b.classList.toggle("on", b.dataset.themeChoice === theme));
  const dark = theme === "dark" || (theme === "system" && !matchMedia("(prefers-color-scheme: light)").matches);
  try { window.webkit.messageHandlers.theme.postMessage(dark ? "dark" : "light"); } catch (e) { /* (not the macOS app) */ }
}
$$("[data-theme-choice]").forEach((b) => b.addEventListener("click", async () => {
  setTheme(b.dataset.themeChoice);
  try { await api("/api/settings", { theme: b.dataset.themeChoice }); } catch (e) { fail(e); }
}));
matchMedia("(prefers-color-scheme: light)").addEventListener("change", () => setTheme(document.documentElement.dataset.theme || "dark"));
function showSettings() {
  setTheme(document.documentElement.dataset.theme || "dark");
  const lib = state.lib || {};
  $("#about").innerHTML = `WindowSmash Mods · ${esc(({ darwin: "macOS", win32: "Windows", linux: "Linux" })[lib.platform] || lib.platform || "")}<br>
    WindowSmash folder: <code>${esc(lib.root || "")}</code><br>
    Log (send it if something goes wrong): <code>${esc(lib.log || "")}</code>`;
  showPlugins();
}
const PLUGIN_NAME = { "libarcropolis.nro": "ARCropolis", "libsmashline_plugin.nro": "Smashline 2", "libone_slot_eff.nro": "One Slot Effects",
  "libthe_csk_collection.nro": "The CSK Collection", "libnro_hook.nro": "nro_hook", "libparam_config.nro": "lib_paramconfig" };
async function showPlugins() {
  try {
    const { plugins } = await api("/api/plugins/standard");
    $("#std-plugins").innerHTML = plugins.map((p) => `<div><b>${esc(PLUGIN_NAME[p.name] || p.name)}</b> <small>${esc(p.version)}${
      p.ported ? " · ported to 13.0.5" : ""}</small><span>${p.status !== "runs" ? '<span class="badge">built into WindowSmash</span>'
      : p.have ? '<span class="badge green">installed</span>' : '<span class="badge gold">downloaded when you build plugins</span>'}</span></div>`).join("");
  } catch (e) { $("#std-plugins").textContent = ""; }
}
$("#std-fetch").addEventListener("click", async () => {
  try { runJob(await api("/api/plugins/fetch", {}), () => showPlugins()); } catch (e) { fail(e); }
});
setTheme(document.documentElement.dataset.theme || "dark");

// ---- keep the app's server alive while this page is open ------------------------------------
setInterval(() => api("/api/ping").catch(() => {}), 5000);
setInterval(() => { if (document.visibilityState === "visible" && !$$(".job").length) loadLibrary(); }, 15000);
api("/api/ping").catch(() => {});
// the window closing: the server ends (a reload pings again before it does)
addEventListener("pagehide", () => { try { navigator.sendBeacon("/api/bye", "{}"); } catch (e) {} });
loadLibrary();

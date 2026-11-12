/* Game pad and keyboard navigation for the OpenSmash pages (the Linux/macOS launcher, WindowSmash Mods), so they
 * work from the couch and in Steam's Game Mode, where a pad is all there is.
 *
 *   D-pad / left stick / arrow keys   move to the nearest button, field or link that way
 *   A (Enter)                         press it
 *   B (Escape)                        back: close the open dialog, else the page's own back (window.gpBack)
 *   LB / RB                           the previous / next tab (elements with class "tab", or [data-gp-tab])
 *   Start                             the page's main button ([data-gp-start])
 *   right stick                       scroll
 *
 * The Gamepad API sees a pad once a button has been pressed with the page in front (browsers' rule). In Steam's
 * Game Mode, Steam Input gives the browser a pad (a "Gamepad" controller layout) or keys and a mouse (a
 * "Keyboard"/"Web browser" layout): both work. */
(function () {
  "use strict";
  const FOCUSABLE = 'button, a[href], input:not([type="hidden"]), select, textarea, summary, [tabindex]:not([tabindex="-1"])';
  const REPEAT_FIRST = 380, REPEAT_NEXT = 110, DEAD = 0.5;
  const style = document.createElement("style");
  style.textContent = `
    html.gp-active :focus { outline: 3px solid #ffcf3a !important; outline-offset: 2px; box-shadow: 0 0 0 6px rgba(255, 207, 58, .25) !important; }
  `;
  document.head.appendChild(style);

  function visible(el) {
    if (el.disabled || el.closest("[hidden], [inert]")) return false;
    const r = el.getBoundingClientRect();
    if (r.width < 2 || r.height < 2) return false;
    const cs = getComputedStyle(el);
    return cs.visibility !== "hidden" && cs.display !== "none";
  }

  /* The open dialog's elements if one is open (a <dialog open>, or an element with role=dialog / class modal shown),
   * else the page's. */
  function scope() {
    const dialogs = [...document.querySelectorAll('dialog[open], [role="dialog"], [data-gp-scope], .modal, .overlay')].filter(
      d => d.tagName === "DIALOG" || visible(d));
    return dialogs.length ? dialogs[dialogs.length - 1] : document;
  }

  function candidates() {
    return [...scope().querySelectorAll(FOCUSABLE)].filter(visible);
  }

  function center(r) { return { x: r.left + r.width / 2, y: r.top + r.height / 2 }; }

  /* The nearest element in a direction: distance along it, plus twice the distance across it. */
  function move(dx, dy) {
    document.documentElement.classList.add("gp-active");
    const all = candidates();
    if (!all.length) return;
    const cur = document.activeElement;
    if (!cur || cur === document.body || !all.includes(cur)) { focus(all[0]); return; }
    const a = center(cur.getBoundingClientRect());
    let best = null, bestScore = Infinity;
    for (const el of all) {
      if (el === cur) continue;
      const b = center(el.getBoundingClientRect());
      const along = (b.x - a.x) * dx + (b.y - a.y) * dy;
      if (along <= 4) continue;
      const across = Math.abs((b.x - a.x) * dy) + Math.abs((b.y - a.y) * dx);
      const score = along + across * 2;
      if (score < bestScore) { bestScore = score; best = el; }
    }
    if (best) focus(best);
  }

  function focus(el) {
    el.focus({ preventScroll: true });
    el.scrollIntoView({ block: "nearest", inline: "nearest", behavior: "smooth" });
  }

  function press() {
    const el = document.activeElement;
    if (!el || el === document.body) { move(0, 1); return; }
    if (el.tagName === "SELECT") { cycleSelect(el, 1); return; }
    if (el.tagName === "INPUT" && /^(text|search|number|password|url|email)$/.test(el.type)) { el.select && el.select(); return; }
    el.click();
  }

  /* A <select>'s next option (a pad can't open the browser's own list): A goes round them. */
  function cycleSelect(el, step) {
    const n = el.options.length;
    if (!n) return;
    el.selectedIndex = (el.selectedIndex + step + n) % n;
    el.dispatchEvent(new Event("input", { bubbles: true }));
    el.dispatchEvent(new Event("change", { bubbles: true }));
  }

  /* B: what Escape does on the page (its handlers close its overlays; a <dialog> closes itself); only with nothing
   * open, the page's own back (window.gpBack: the launcher's Play tab, WindowSmash Mods' way back to the launcher). */
  function back() {
    const open = scope() !== document;
    const target = document.activeElement || document.body;
    const ev = new KeyboardEvent("keydown", { key: "Escape", bubbles: true, cancelable: true });
    target.dispatchEvent(ev);
    if (open) {
      const dlg = [...document.querySelectorAll("dialog[open]")].pop();
      if (dlg && !ev.defaultPrevented) dlg.dispatchEvent(new Event("cancel", { cancelable: true })) && dlg.close();
      return;
    }
    if (!ev.defaultPrevented && typeof window.gpBack === "function") window.gpBack();
  }

  function tab(step) {
    const tabs = [...document.querySelectorAll(".tab, [data-gp-tab]")].filter(visible);
    if (!tabs.length) return;
    let i = tabs.findIndex(t => t.classList.contains("active") || t.getAttribute("aria-selected") === "true");
    i = (i + step + tabs.length) % tabs.length;
    tabs[i].click();
    focus(tabs[i]);
  }

  function start() {
    const el = [...document.querySelectorAll("[data-gp-start]")].find(visible);
    if (el) { focus(el); el.click(); }
  }

  /* ---- the keyboard: arrows move (not inside a text field, where they move the caret) ---- */
  document.addEventListener("keydown", e => {
    if (e.defaultPrevented || e.altKey || e.ctrlKey || e.metaKey) return;
    const el = document.activeElement;
    const typing = el && (el.tagName === "TEXTAREA" || (el.tagName === "INPUT" && !/^(checkbox|radio|button|submit|range)$/.test(el.type)));
    const dirs = { ArrowUp: [0, -1], ArrowDown: [0, 1], ArrowLeft: [-1, 0], ArrowRight: [1, 0] };
    if (dirs[e.key] && !(typing && (e.key === "ArrowLeft" || e.key === "ArrowRight"))) {
      if (el && el.tagName === "SELECT") return;   /* (the select's own choice) */
      e.preventDefault();
      move(...dirs[e.key]);
    }
  });
  let overlayAtEscape = false;   /* (whether something was open before the page's handlers closed it) */
  document.addEventListener("keydown", e => { if (e.key === "Escape") overlayAtEscape = scope() !== document; }, true);
  document.addEventListener("keydown", e => {   /* (after the page's own Escape handlers: bubbling order) */
    if (e.key === "Escape" && !e.defaultPrevented && e.isTrusted && !overlayAtEscape && typeof window.gpBack === "function") {
      e.preventDefault();
      window.gpBack();
    }
  });
  document.addEventListener("mousedown", () => document.documentElement.classList.remove("gp-active"));

  /* ---- the pad ---- */
  const held = {};   /* (control name: when it repeats next) */
  function edge(name, down, now, fn, repeat) {
    if (!down) { delete held[name]; return; }
    if (!(name in held)) { held[name] = now + REPEAT_FIRST; fn(); return; }
    if (repeat && now >= held[name]) { held[name] = now + REPEAT_NEXT; fn(); }
  }
  function poll() {
    const pads = navigator.getGamepads ? [...navigator.getGamepads()].filter(Boolean) : [];
    const now = performance.now();
    if (pads.length && document.hasFocus()) {
      const b = i => pads.some(p => p.buttons[i] && p.buttons[i].pressed);
      const ax = i => pads.reduce((v, p) => Math.abs(p.axes[i] || 0) > Math.abs(v) ? p.axes[i] : v, 0);
      if ([0, 1, 4, 5, 9, 12, 13, 14, 15].some(b) || Math.abs(ax(0)) > DEAD || Math.abs(ax(1)) > DEAD)
        document.documentElement.classList.add("gp-active");
      edge("up", b(12) || ax(1) < -DEAD, now, () => move(0, -1), true);
      edge("down", b(13) || ax(1) > DEAD, now, () => move(0, 1), true);
      edge("left", b(14) || ax(0) < -DEAD, now, () => {
        const el = document.activeElement;
        if (el && el.tagName === "SELECT") cycleSelect(el, -1);
        else if (el && el.type === "range") { el.stepDown(5); el.dispatchEvent(new Event("input", { bubbles: true })); el.dispatchEvent(new Event("change", { bubbles: true })); }
        else move(-1, 0);
      }, true);
      edge("right", b(15) || ax(0) > DEAD, now, () => {
        const el = document.activeElement;
        if (el && el.tagName === "SELECT") cycleSelect(el, 1);
        else if (el && el.type === "range") { el.stepUp(5); el.dispatchEvent(new Event("input", { bubbles: true })); el.dispatchEvent(new Event("change", { bubbles: true })); }
        else move(1, 0);
      }, true);
      edge("a", b(0), now, press, false);
      edge("b", b(1), now, back, false);
      edge("lb", b(4), now, () => tab(-1), false);
      edge("rb", b(5), now, () => tab(1), false);
      edge("start", b(9), now, start, false);
      const sy = ax(3);
      if (Math.abs(sy) > 0.2) (scope() === document ? (document.querySelector("main") || document.scrollingElement) : scope()).scrollBy(0, sy * 18);
    }
    requestAnimationFrame(poll);
  }
  window.addEventListener("gamepadconnected", () => document.documentElement.classList.add("gp-active"));
  requestAnimationFrame(poll);
  window.gpFocusFirst = function () { const all = candidates(); if (all.length) focus(all[0]); };
})();

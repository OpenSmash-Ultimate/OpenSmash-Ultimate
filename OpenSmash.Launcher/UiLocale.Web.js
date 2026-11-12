/* Localizes the embedded browser's own chrome from the host's shared catalog.
 * Submission names, descriptions, authors, filenames and option values stay verbatim. */
(() => {
  "use strict";
  const selectors = "button,option,.tab,.eyebrow,.state,.empty,.desc-placeholder,.sec-head,.umc-loading,[data-msg],#pageInfo,#search";
  const external = ".title,.author,.byline,.filename,.file-name,#dTitle,#dByline,#dTags,#dCrumb,#reqTitle,#reqList,svg";
  const originals = new WeakMap();
  let locale = window.__opensmashLocale || {language: "en", messages: {}};
  let observer;
  const lookup = text => Object.prototype.hasOwnProperty.call(locale.messages, text) ? locale.messages[text] : text;
  function value(owner, key, text) {
    let fields = originals.get(owner);
    if (!fields) { fields = {}; originals.set(owner, fields); }
    let old = fields[key];
    if (!old || text !== old.output) old = {source: text, output: text};
    const source = old.source;
    const trimmed = source.trim();
    const translated = lookup(trimmed);
    old.output = translated === trimmed ? source : source.replace(trimmed, translated);
    fields[key] = old;
    return old.output;
  }
  function apply() {
    if (!document.body) return;
    if (observer) observer.disconnect();
    document.documentElement.lang = locale.language;
    for (const element of document.querySelectorAll(selectors)) {
      if (element.closest(external)) continue;
      const walk = document.createTreeWalker(element, NodeFilter.SHOW_TEXT);
      let node;
      while ((node = walk.nextNode())) {
        if (node.parentElement.closest(external)) continue;
        const next = value(node, "text", node.nodeValue);
        if (next !== node.nodeValue) node.nodeValue = next;
      }
      for (const attribute of ["title", "placeholder", "aria-label"]) {
        if (!element.hasAttribute(attribute)) continue;
        const before = element.getAttribute(attribute), next = value(element, attribute, before);
        if (next !== before) element.setAttribute(attribute, next);
      }
    }
    if (observer) observer.observe(document.body, {childList: true, subtree: true, characterData: true, attributes: true, attributeFilter: ["title", "placeholder", "aria-label"]});
  }
  window.__opensmashSetLocale = next => { locale = next; apply(); window.dispatchEvent(new Event("opensmashlocalechange")); };
  window.__opensmashText = source => lookup(source);
  window.__opensmashFormat = (source, ...args) => lookup(source).replace(/\{(\d+)\}/g, (_, index) => String(args[Number(index)]));
  function start() { observer = new MutationObserver(apply); apply(); }
  if (document.readyState === "loading") document.addEventListener("DOMContentLoaded", start, {once: true});
  else start();
})();

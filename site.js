// Copy buttons and the ls listing that follows the file you are reading.
// The page reads the same without any of this.
(function () {
  "use strict";
  var lang = document.documentElement.lang === "pl" ? "pl" : "en";
  var said = {
    en: { done: "copied", copied: "copied: paste it into your shell", failed: "select the line and copy it by hand" },
    pl: { done: "skopiowano", copied: "skopiowano: wklej w powłoce", failed: "zaznacz linię i skopiuj ją ręcznie" }
  }[lang];

  document.querySelectorAll("button[data-copy]").forEach(function (button) {
    var label = button.textContent;
    var status = document.querySelector("[data-status]");
    var before = status ? status.textContent : "";
    button.addEventListener("click", function () {
      var src = document.getElementById(button.getAttribute("data-copy"));
      var text = src.textContent.replace(/^\$\s*/, "").trim();
      var done = function (ok) {
        button.textContent = ok ? said.done : label;
        if (ok) button.setAttribute("data-done", "");
        if (status) status.textContent = ok ? said.copied : said.failed;
        setTimeout(function () {
          button.textContent = label;
          button.removeAttribute("data-done");
          if (status) status.textContent = before;
        }, 2600);
      };
      if (navigator.clipboard && window.isSecureContext) {
        navigator.clipboard.writeText(text).then(function () { done(true); }, function () { done(false); });
      } else {
        done(false);
      }
    });
  });

  var links = Array.prototype.slice.call(document.querySelectorAll(".ls ol a"));
  if (!("IntersectionObserver" in window) || !links.length) return;
  var byId = {};
  links.forEach(function (a) { byId[a.getAttribute("href").slice(1)] = a; });
  var visible = {};
  var io = new IntersectionObserver(function (entries) {
    entries.forEach(function (e) { visible[e.target.id] = e.isIntersecting; });
    var current = null;
    document.querySelectorAll("section.file").forEach(function (s) {
      if (!current && visible[s.id]) current = s.id;
    });
    links.forEach(function (a) { a.removeAttribute("aria-current"); });
    if (current && byId[current]) byId[current].setAttribute("aria-current", "true");
  }, { rootMargin: "-35% 0px -55% 0px" });
  document.querySelectorAll("section.file").forEach(function (s) { io.observe(s); });
})();

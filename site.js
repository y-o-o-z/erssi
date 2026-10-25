// Copy buttons and the bar's border once the page scrolls.
// The page reads the same without any of this.
(function () {
  "use strict";
  var pl = document.documentElement.lang === "pl";
  var said = pl
    ? { done: "Skopiowano", ok: "Polecenie skopiowane: wklej je w powłoce.", failed: "Zaznacz polecenie i skopiuj je ręcznie." }
    : { done: "Copied", ok: "Command copied: paste it into your shell.", failed: "Select the command and copy it by hand." };
  var status = document.querySelector("[data-status]");

  document.querySelectorAll("button[data-copy]").forEach(function (button) {
    var label = button.querySelector("[data-label]");
    var before = label ? label.textContent : "";
    button.addEventListener("click", function () {
      var src = document.getElementById(button.getAttribute("data-copy"));
      var text = src.textContent.replace(/^\$\s*/, "").trim();
      var done = function (ok) {
        if (ok && label) { label.textContent = said.done; button.setAttribute("data-done", ""); }
        if (status) status.textContent = ok ? said.ok : said.failed;
        if (!ok) { src.focus && src.focus(); }
        setTimeout(function () {
          if (label) label.textContent = before;
          button.removeAttribute("data-done");
        }, 2200);
      };
      if (navigator.clipboard && window.isSecureContext) {
        navigator.clipboard.writeText(text).then(function () { done(true); }, function () { done(false); });
      } else {
        done(false);
      }
    });
  });

  var bar = document.querySelector("[data-bar]");
  if (bar) {
    var onScroll = function () { bar.classList.toggle("scrolled", window.scrollY > 4); };
    window.addEventListener("scroll", onScroll, { passive: true });
    onScroll();
  }
})();

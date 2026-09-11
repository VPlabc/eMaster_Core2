/* Theme controller.
 *
 * Loaded from <head>, BEFORE the stylesheet renders anything, so the stored
 * theme is on <html> at first paint. Loading it with the other scripts at
 * the end of <body> would paint the default theme first and then repaint --
 * a visible white flash on every navigation for dark-mode users.
 *
 * State lives on <html> as two attributes:
 *   data-theme     -- ours, drives every var in css/app.css
 *   data-bs-theme  -- Bootstrap 5.3's own, so its built-in components
 *                     (dropdowns, modals, form controls) recolour too
 */
(function () {
  "use strict";

  var STORAGE_KEY = "hsf.theme";
  var root = document.documentElement;

  function systemPrefersDark() {
    return window.matchMedia && window.matchMedia("(prefers-color-scheme: dark)").matches;
  }

  function stored() {
    try {
      var v = localStorage.getItem(STORAGE_KEY);
      return v === "light" || v === "dark" ? v : null;
    } catch (e) {
      // Private mode / storage disabled: fall back to the system preference
      // rather than failing to theme at all.
      return null;
    }
  }

  function apply(theme) {
    root.setAttribute("data-theme", theme);
    root.setAttribute("data-bs-theme", theme);
  }

  // Suppress transitions for the initial application only, so the very first
  // paint doesn't animate from the default theme into the stored one.
  root.classList.add("hsf-theme-booting");
  apply(stored() || (systemPrefersDark() ? "dark" : "light"));

  function unboot() {
    // Two frames: one for the themed styles to land, one for the browser to
    // paint them, before transitions are allowed back on.
    requestAnimationFrame(function () {
      requestAnimationFrame(function () {
        root.classList.remove("hsf-theme-booting");
      });
    });
  }
  if (document.readyState === "loading") {
    document.addEventListener("DOMContentLoaded", unboot);
  } else {
    unboot();
  }

  var HsfTheme = {
    get: function () {
      return root.getAttribute("data-theme") || "light";
    },

    set: function (theme) {
      if (theme !== "light" && theme !== "dark") return;
      apply(theme);
      try {
        localStorage.setItem(STORAGE_KEY, theme);
      } catch (e) {
        // Non-fatal: the theme still applies for this page view.
      }
      window.dispatchEvent(new CustomEvent("hsf:themechange", { detail: { theme: theme } }));
    },

    toggle: function () {
      this.set(this.get() === "dark" ? "light" : "dark");
    },

    /* Chart.js can't read CSS variables, so anything canvas-based has to ask
       for resolved colours and re-read them on hsf:themechange. */
    colors: function () {
      var s = getComputedStyle(root);
      function v(name, fallback) {
        return (s.getPropertyValue(name) || fallback).trim();
      }
      return {
        text: v("--hsf-text", "#1c2430"),
        textMuted: v("--hsf-text-muted", "#5f6b7c"),
        border: v("--hsf-border", "#d5dbe5"),
        accent: v("--hsf-accent", "#2563eb"),
        ok: v("--hsf-ok", "#16a34a"),
        warn: v("--hsf-warn", "#d97706"),
        bad: v("--hsf-bad", "#dc2626"),
        idle: v("--hsf-idle", "#94a3b8"),
        busy: v("--hsf-busy", "#2563eb")
      };
    }
  };

  window.HsfTheme = HsfTheme;

  // Follow the OS only while the user hasn't made an explicit choice.
  if (window.matchMedia) {
    var mq = window.matchMedia("(prefers-color-scheme: dark)");
    var onChange = function (e) {
      if (!stored()) apply(e.matches ? "dark" : "light");
    };
    if (mq.addEventListener) mq.addEventListener("change", onChange);
    else if (mq.addListener) mq.addListener(onChange);
  }

  // Wire up any toggle button on the page once the DOM exists.
  function bind() {
    var buttons = document.querySelectorAll("[data-hsf-theme-toggle]");
    for (var i = 0; i < buttons.length; i++) {
      buttons[i].addEventListener("click", function () {
        HsfTheme.toggle();
      });
    }
  }
  if (document.readyState === "loading") {
    document.addEventListener("DOMContentLoaded", bind);
  } else {
    bind();
  }
})();

/* Turn the existing, server-independent reference markup into a compact
 * documentation workspace. Keeping the source entries in HTML preserves the
 * documented API text and makes this enhancement work without an API call. */
(function () {
  "use strict";

  function slug(value) {
    return value.toLowerCase().replace(/[^a-z0-9]+/g, "-").replace(/^-|-$/g, "");
  }

  function initialise() {
    var source = document.querySelector(".container.py-4");
    if (!source) return;
    var title = source.querySelector("h3");
    var headings = Array.from(source.children).filter(function (node) { return node.tagName === "H4"; });
    if (!title || !headings.length) return;

    title.classList.add("hsf-doc-title");
    var searchLabel = document.createElement("label");
    searchLabel.className = "visually-hidden";
    searchLabel.htmlFor = "luaApiSearch";
    searchLabel.textContent = "Search Lua API documentation";
    var search = document.createElement("input");
    search.id = "luaApiSearch";
    search.type = "search";
    search.className = "form-control hsf-doc-search";
    search.placeholder = "Search Lua API…";
    search.autocomplete = "off";
    title.insertAdjacentElement("afterend", searchLabel);
    searchLabel.insertAdjacentElement("afterend", search);

    var layout = document.createElement("div");
    layout.className = "hsf-doc-layout";
    var toc = document.createElement("nav");
    toc.className = "hsf-doc-toc";
    toc.setAttribute("aria-label", "Lua API table of contents");
    toc.innerHTML = '<div class="hsf-doc-toc-title">Table of contents</div>';
    var body = document.createElement("div");
    body.className = "hsf-doc-content";
    layout.append(toc, body);
    search.insertAdjacentElement("afterend", layout);

    var entries = [];
    headings.forEach(function (heading, index) {
      var name = heading.textContent.trim();
      var id = "lua-api-" + slug(name);
      var section = document.createElement("section");
      section.className = "hsf-doc-section";
      section.id = id;
      heading.classList.add("hsf-doc-section-title");
      var stop = headings[index + 1];
      var node = heading;
      while (node && node !== stop) {
        var next = node.nextSibling;
        section.appendChild(node);
        node = next;
      }
      body.appendChild(section);
      var link = document.createElement("a");
      link.href = "#" + id;
      link.textContent = name;
      toc.appendChild(link);
      entries.push({ section: section, link: link, name: name });
    });

    if ("IntersectionObserver" in window) {
      var observer = new IntersectionObserver(function (observations) {
        var current = observations.filter(function (item) { return item.isIntersecting; }).sort(function (a, b) {
          return a.boundingClientRect.top - b.boundingClientRect.top;
        })[0];
        if (!current) return;
        entries.forEach(function (entry) { entry.link.classList.toggle("active", entry.section === current.target); });
      }, { rootMargin: "-20% 0px -70% 0px" });
      entries.forEach(function (entry) { observer.observe(entry.section); });
    }

    function filter() {
      var term = search.value.trim().toLowerCase();
      entries.forEach(function (entry) {
        var blocks = Array.from(entry.section.querySelectorAll(".api-doc-block"));
        var visible = !term || entry.section.textContent.toLowerCase().indexOf(term) !== -1;
        blocks.forEach(function (block) {
          block.hidden = !!term && block.textContent.toLowerCase().indexOf(term) === -1;
        });
        entry.section.hidden = !visible;
        entry.link.hidden = !visible;
      });
      body.classList.toggle("hsf-doc-no-results", !!term && !entries.some(function (entry) { return !entry.section.hidden; }));
    }
    search.addEventListener("input", filter);
    filter();
  }

  if (document.readyState === "loading") document.addEventListener("DOMContentLoaded", initialise);
  else initialise();
})();

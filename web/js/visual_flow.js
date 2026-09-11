(function () {
  "use strict";
  var canvas = document.getElementById("flowCanvas");
  var json = document.getElementById("flowJson");
  var result = document.getElementById("flowResult");
  var state = document.getElementById("flowState");
  var definition = { version: 1, name: "Untitled flow", nodes: [], connections: [] };
  var categories = ["TRIGGER", "CONDITION", "ACTION", "DATA", "SYSTEM"];
  var selectedId = null;
  var runtimeEvents = 0;

  function render() {
    canvas.textContent = "";
    definition.nodes.forEach(function (node, index) {
      var item = document.createElement("button"); item.type = "button";
      item.className = "btn text-start " + (node.id === selectedId ? "btn-primary" : "btn-outline-primary");
      item.textContent = (index + 1) + ". " + node.type;
      item.title = "Click to select; double-click to remove";
      item.addEventListener("click", function () { selectedId = node.id; showProperties(node); render(); });
      item.addEventListener("dblclick", function () { definition.nodes.splice(index, 1); definition.connections = definition.connections.filter(function (c) { return c.from !== node.id && c.to !== node.id; }); selectedId = null; render(); });
      canvas.appendChild(item);
    });
    json.value = JSON.stringify(definition, null, 2);
  }
  function showProperties(node) {
    document.getElementById("flowSelection").textContent = node.type + " · " + node.id;
    var editor = document.getElementById("flowProperties"); editor.disabled = false; editor.value = JSON.stringify(node.parameters || {}, null, 2);
    document.getElementById("flowApplyProperties").disabled = false;
  }
  function setResult(message, ok) { result.textContent = message; result.className = "small mt-2 " + (ok ? "text-success" : "text-danger"); }
  function parse() { try { var value = JSON.parse(json.value); if (!value || value.version !== 1 || !Array.isArray(value.nodes) || !Array.isArray(value.connections)) throw new Error("version, nodes and connections are required"); definition = value; return true; } catch (e) { setResult(e.message, false); return false; } }
  categories.forEach(function (category) { var button = document.createElement("button"); button.type = "button"; button.className = "list-group-item list-group-item-action"; button.textContent = category; button.addEventListener("click", function () { var node = { id: "node-" + Date.now() + "-" + definition.nodes.length, type: category, parameters: {} }; definition.nodes.push(node); selectedId = node.id; render(); showProperties(node); }); document.getElementById("flowNodeLibrary").appendChild(button); });
  document.getElementById("flowApplyProperties").addEventListener("click", function () { var node = definition.nodes.find(function (n) { return n.id === selectedId; }); if (!node) return; try { node.parameters = JSON.parse(document.getElementById("flowProperties").value || "{}"); render(); showProperties(node); setResult("Node properties applied.", true); } catch (e) { setResult("Invalid node properties: " + e.message, false); } });
  document.getElementById("flowValidate").addEventListener("click", function () { var valid = parse(); setResult(valid ? "Flow definition is valid." : "Flow definition is invalid.", valid); });
  document.getElementById("flowSave").addEventListener("click", function () { if (parse()) { localStorage.setItem("hsf.visualFlow", JSON.stringify(definition)); setResult("Flow saved on this browser.", true); } });
  document.getElementById("flowLoad").addEventListener("click", function () { var saved = localStorage.getItem("hsf.visualFlow"); if (!saved) return setResult("No saved flow found.", false); try { definition = JSON.parse(saved); render(); setResult("Flow loaded.", true); } catch (e) { setResult("Saved flow is invalid.", false); } });
  function startFlow() { if (parse()) { state.textContent = "Running"; state.className = "badge text-bg-success"; runtimeEvents++; document.getElementById("flowEventCount").textContent = runtimeEvents + " runtime events"; setResult("Flow validated and marked running (runtime adapter pending).", true); } }
  document.getElementById("flowStart").addEventListener("click", startFlow);
  document.getElementById("flowRestart").addEventListener("click", function () { document.getElementById("flowStop").click(); startFlow(); });
  document.getElementById("flowStop").addEventListener("click", function () { state.textContent = "Stopped"; state.className = "badge text-bg-secondary"; });
  render();
})();

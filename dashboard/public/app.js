const state = {
  activeTab: "topology",
  activeSettingsSection: "general",
  status: null,
  adapters: [],
  configState: null,
  logs: [],
  selectedSubDeviceIndex: 0,
  isSaving: false,
  headerData: null
};

const $ = (selector) => document.querySelector(selector);
const $$ = (selector) => Array.from(document.querySelectorAll(selector));

const levelClass = (level) => ({
  good: "status-good",
  warning: "status-warning",
  error: "status-error"
}[level] || "status-muted");

function setValueIfIdle(selector, value) {
  const node = $(selector);
  if (node && document.activeElement !== node) node.value = value;
}

function setCheckedIfIdle(selector, checked) {
  const node = $(selector);
  if (node && document.activeElement !== node) node.checked = checked;
}

function toast(message) {
  const node = $("#toast");
  node.textContent = message;
  node.classList.remove("hidden");
  setTimeout(() => node.classList.add("hidden"), 2200);
}

async function api(path, options = {}) {
  const response = await fetch(path, {
    headers: options.body instanceof FormData ? undefined : { "content-type": "application/json" },
    ...options
  });
  if (!response.ok) {
    const text = await response.text();
    throw new Error(text || `Request failed: ${response.status}`);
  }
  return response;
}

async function loadStatus() {
  const response = await api("/api/status");
  const data = await response.json();
  state.status = data.status;
  state.adapters = data.adapters;
  state.configState = data.configState;
  renderHeader(data);
  renderGlobalFailsafe(data);
  renderTopology();
  if (!state.isSaving) {
    renderSettings(data);
  }
}

async function loadConfig() {
  const response = await api("/api/config");
  const data = await response.json();
  state.configState = data.configState;
  renderSettings({
    configState: data.configState,
    adapters: state.adapters,
    status: state.status,
    dashboardVersion: "",
    daemonVersion: "",
    system: { model: "", memAvailableMb: 0, memTotalMb: 0, networkBytes: 0, preemptRtActive: false }
  });
}

function renderGlobalFailsafe(data) {
  const failedConfig = data.isConfigLoaded === false || data.configState?.regeneratedFailsafe;
  $("#globalFailsafeBanner").classList.toggle("hidden", !failedConfig);
}

function renderHeader(data) {
  const s = data.status;
  const daemonLevel = s.daemonStatus > 0 ? "good" : "error";
  const adapterLevel = data.adapters.length > 0 ? "good" : "warning";
  const deviceLevel = s.subDeviceCount > 0 ? "good" : "warning";
  const faultLevel = s.activeFaults > 0 ? "error" : "good";
  
  state.headerData = { daemonVersion: data.daemonVersion, dashboardVersion: data.dashboardVersion, adapters: data.adapters };
  
  const dots = [
    ["Daemon", daemonLevel, `Daemon: v${data.daemonVersion}`],
    ["Dashboard", "good", `Dashboard: v${data.dashboardVersion}`],
    ["Adapters", adapterLevel, `${data.adapters.length} detected adapters`],
    ["Devices", deviceLevel, `${s.subDeviceCount} active SubDevices`],
    ["Faults", faultLevel, `${s.activeFaults} active faults`, "faults"]
  ];
  $("#statusDots").innerHTML = dots.map(([label, level, title, action]) =>
    `<button class="status-pill ${levelClass(level)}" title="${title}" data-action="${action || ""}" type="button">● ${label}</button>`
  ).join("");

  renderHeaderTelemetry();
}

function renderHeaderTelemetry() {
  const s = state.status || {};
  const jitterLevel = s.mainDeviceJitterUs < 50 ? "good" : s.mainDeviceJitterUs <= 200 ? "warning" : "error";
  const jitter = $("#jitterIndicator");
  jitter.textContent = `● Jitter: ${s.mainDeviceJitterUs || 0}µs`;
  jitter.className = `rounded border px-3 py-1 text-sm font-semibold ${levelClass(jitterLevel)}`;
}

function renderTopology() {
  const map = $("#topologyMap");
  const subDeviceCount = state.status?.subDeviceCount || 0;
  const adapterHtml = state.adapters.map((adapter, index) => `
    <button class="topology-node adapter-node" data-adapter="${index}" type="button">
      <span class="node-kicker">Adapter</span>
      <strong>${adapter.logicalName}</strong>
      <small>${adapter.name} · ${adapter.mac}</small>
    </button>
  `).join("");
  const devices = Array.from({ length: subDeviceCount }, (_, index) => `
    <button class="topology-node subdevice-node" data-subdevice="${index + 1}" type="button">
      <span class="node-kicker">SubDevice ${index + 1}</span>
      <strong>Operational Node</strong>
      <small>Index ${index + 1} · Diagnostics OK</small>
    </button>
  `).join("");
  map.innerHTML = adapterHtml + devices || `<div class="empty-state">No active EtherCAT topology detected.</div>`;
}

async function loadLogs() {
  const response = await api("/api/logs");
  state.logs = (await response.json()).logs;
  renderLogs();
}

function renderLogs() {
  const list = $("#logFileList");
  list.innerHTML = state.logs.map((log) => `
    <label class="log-row">
      <input type="checkbox" value="${log.name}" />
      <span class="min-w-0 flex-1 truncate">${log.name}</span>
      <span class="text-xs text-slate-500">${formatBytes(log.size)}</span>
    </label>
  `).join("") || `<div class="empty-state">No .wpilog files found.</div>`;
}

function selectedLogNames() {
  return $$("#logFileList input:checked").map((item) => item.value);
}

function formatBytes(bytes) {
  if (bytes < 1024) return `${bytes} B`;
  if (bytes < 1024 * 1024) return `${Math.round(bytes / 1024)} KB`;
  return `${(bytes / 1024 / 1024).toFixed(1)} MB`;
}

function renderSettings(data) {
  const cfg = data.configState.config;
  setValueIfIdle("#logDirectory", cfg.log_directory);
  setValueIfIdle("#logLimit", String(cfg.log_count_limit));
  setValueIfIdle("#freeSpaceThreshold", String(cfg.free_space_threshold_mb));
  setCheckedIfIdle("#allowRestricted", Boolean(cfg.allow_restricted_interfaces));

  const locked = data.configState.regeneratedFailsafe;
  $("#failsafeBanner").classList.toggle("hidden", !locked);
  $$("#settingsView input, #settingsView select, #saveConfig").forEach((node) => {
    if (node.id !== "allowRestricted") node.disabled = locked;
  });

  if (!document.activeElement?.closest("#networkList")) {
    $("#networkList").innerHTML = data.adapters.map((adapter) => `
      <div class="network-row" data-interface="${adapter.name}">
        <div class="min-w-0 grid gap-2">
          <label class="field-label">Logical name
            <input class="field-input network-logical-name" value="${adapter.logicalName}" ${data.configState.regeneratedFailsafe ? "disabled" : ""} />
          </label>
          <label class="toggle-row">
            <input class="network-lock" type="checkbox" ${adapter.locked ? "checked" : ""} ${data.configState.regeneratedFailsafe ? "disabled" : ""} />
            Lock to physical port ${adapter.name}
          </label>
          <div class="text-xs text-slate-500">${adapter.mac}</div>
        </div>
        <div class="text-right text-xs text-slate-400">
          <div>RX ${formatBytes(adapter.rxBytesPerSecond)}/s</div>
          <div>TX ${formatBytes(adapter.txBytesPerSecond)}/s</div>
        </div>
      </div>
    `).join("") || `<div class="empty-state">No usable adapters detected.</div>`;
  }

  const count = data.status.subDeviceCount || 0;
  $("#subDeviceList").innerHTML = Array.from({ length: count }, (_, index) => `
    <button class="dense-row w-full text-left" data-subdevice="${index + 1}" type="button">
      <span>Index ${index + 1}</span>
      <span class="status-good rounded px-2 py-1 text-xs">Diagnostic OK</span>
    </button>
  `).join("") || `<div class="empty-state">No SubDevices reporting.</div>`;

  $("#aboutInfo").innerHTML = `
    <div>EC Dashboard | Version ${data.dashboardVersion} | Copyright (c) EC Dashboard maintainers and contributors.</div>
    <div>CPU: ${data.system.model}</div>
    <div>RAM: ${data.system.memAvailableMb} MB available / ${data.system.memTotalMb} MB total</div>
    <div>Network usage: ${formatBytes(data.system.networkBytes)}</div>
    <div>PREEMPT_RT: ${data.system.preemptRtActive ? "Active" : "Not detected"}</div>
  `;
}

function appendConsoleLine(line) {
  const out = $("#consoleOutput");
  const lowered = line.toLowerCase();
  const level = lowered.includes("error") ? "console-error" : lowered.includes("warning") ? "console-warning" : "console-line";
  const row = document.createElement("div");
  row.className = level;
  row.textContent = line;
  out.appendChild(row);
  out.scrollTop = out.scrollHeight;
  while (out.children.length > 800) out.firstChild.remove();
}

function connectLogs() {
  const protocol = location.protocol === "https:" ? "wss:" : "ws:";
  const ws = new WebSocket(`${protocol}//${location.host}/ws/logs`);
  ws.addEventListener("open", () => $("#socketState").textContent = "Streaming");
  ws.addEventListener("close", () => {
    $("#socketState").textContent = "Disconnected";
    setTimeout(connectLogs, 1500);
  });
  ws.addEventListener("message", (event) => {
    const message = JSON.parse(event.data);
    if (message.type === "log") appendConsoleLine(message.line);
    if (message.type === "status") {
      state.status = message.status;
      renderHeaderTelemetry();
      renderTopology();
    }
    if (message.type === "config_loaded") {
      state.configState = message.configState;
      renderGlobalFailsafe({ configState: message.configState, isConfigLoaded: message.isConfigLoaded });
    }
  });
}

function showFaultModal() {
  const faults = state.status?.activeFaults || 0;
  $("#faultList").innerHTML = faults
    ? `<div class="dense-row"><span>Active fault counters</span><strong>${faults}</strong></div><div class="dense-row"><span>Lost frames</span><strong>${state.status.lostFrames}</strong></div>`
    : `<div class="empty-state">No active faults.</div>`;
  $("#faultModal").showModal();
}

function showAdapterModal(index) {
  const adapter = state.adapters[index];
  if (!adapter) return;
  $("#adapterDetails").innerHTML = `
    <div class="dense-row"><span>Logical name</span><strong>${adapter.logicalName}</strong></div>
    <div class="dense-row"><span>Interface</span><strong>${adapter.name}</strong></div>
    <div class="dense-row"><span>MAC</span><strong>${adapter.mac}</strong></div>
    <div class="dense-row"><span>Restricted</span><strong>${adapter.restricted ? "Yes" : "No"}</strong></div>
    <div class="dense-row"><span>Locked</span><strong>${adapter.locked ? "Yes" : "No"}</strong></div>
  `;
  $("#adapterModal").showModal();
}

function showSubDeviceModal(index) {
  state.selectedSubDeviceIndex = index;
  $("#subDeviceDetails").innerHTML = `
    <div class="dense-row"><span>SubDevice index</span><strong>${index}</strong></div>
    <div class="dense-row"><span>ESI/XML</span><strong>Not loaded</strong></div>
    <div class="dense-row"><span>State</span><strong>Operational</strong></div>
  `;
  $("#subDeviceModal").showModal();
}

function bindEvents() {
  $$(".tab-button").forEach((button) => button.addEventListener("click", () => {
    state.activeTab = button.dataset.tab;
    $$(".tab-button").forEach((item) => item.classList.toggle("active", item === button));
    $$(".view-block").forEach((view) => view.classList.add("hidden"));
    $(`#${state.activeTab}View`).classList.remove("hidden");
    if (state.activeTab === "logs") loadLogs().catch(console.error);
  }));

  $$(".settings-nav").forEach((button) => button.addEventListener("click", () => {
    state.activeSettingsSection = button.dataset.settingsSection;
    $$(".settings-nav").forEach((item) => item.classList.toggle("active", item === button));
    $$(".settings-panel").forEach((panel) => panel.classList.add("hidden"));
    $(`#settingsPanel${state.activeSettingsSection[0].toUpperCase()}${state.activeSettingsSection.slice(1)}`).classList.remove("hidden");
  }));

  document.addEventListener("click", (event) => {
    const target = event.target.closest("[data-action], [data-adapter], [data-subdevice], [data-close-modal]");
    if (!target) return;
    if (target.dataset.action === "faults") showFaultModal();
    if (target.dataset.adapter) showAdapterModal(Number(target.dataset.adapter));
    if (target.dataset.subdevice) showSubDeviceModal(Number(target.dataset.subdevice));
    if (target.dataset.closeModal !== undefined) target.closest("dialog").close();
  });

  $("#refreshTopology").addEventListener("click", () => loadStatus().catch(console.error));
  $("#refreshLogs").addEventListener("click", () => loadLogs().catch(console.error));

  $("#deleteSelected").addEventListener("click", async () => {
    const names = selectedLogNames();
    if (!names.length || !window.confirm(`Delete ${names.length} selected log file(s)?`)) return;
    await api("/api/logs/delete", { method: "POST", body: JSON.stringify({ names }) });
    toast("Selected logs deleted");
    await loadLogs();
  });

  $("#deleteAll").addEventListener("click", async () => {
    if (!window.confirm("Delete ALL log files?")) return;
    await api("/api/logs/delete-all", { method: "POST", body: "{}" });
    toast("All logs deleted");
    await loadLogs();
  });

  $("#downloadSelected").addEventListener("click", async () => {
    const names = selectedLogNames();
    if (!names.length) return;
    const response = await api("/api/logs/download", { method: "POST", body: JSON.stringify({ names }) });
    const blob = await response.blob();
    const url = URL.createObjectURL(blob);
    const link = document.createElement("a");
    link.href = url;
    link.download = `ec_logs_${Date.now()}.zip`;
    link.click();
    URL.revokeObjectURL(url);
  });

  $("#saveConfig").addEventListener("click", async () => {
    if (!window.confirm("Save configuration and allow the daemon to reload?")) return;
    
    state.isSaving = true;
    $("#saveConfig").disabled = true;
    $$("#settingsView input, #settingsView select").forEach((node) => { node.disabled = true; });
    
    try {
      const mappings = $$("#networkList .network-row").flatMap((row) => {
        const locked = row.querySelector(".network-lock").checked;
        const logicalName = row.querySelector(".network-logical-name").value.trim();
        return locked ? [{ logical_name: logicalName || row.dataset.interface, physical_interface: row.dataset.interface }] : [];
      });
      await api("/api/config", {
        method: "POST",
        body: JSON.stringify({
          allow_restricted_interfaces: $("#allowRestricted").checked,
          log_directory: $("#logDirectory").value,
          log_count_limit: Number($("#logLimit").value),
          free_space_threshold_mb: Number($("#freeSpaceThreshold").value),
          interface_mappings: mappings
        })
      });
      toast("Config Saved");
      await loadConfig();
    } finally {
      state.isSaving = false;
      $("#saveConfig").disabled = false;
      $$("#settingsView input, #settingsView select").forEach((node) => { node.disabled = false; });
    }
  });

  $("#applyDefaults").addEventListener("click", async () => {
    if (!window.confirm("Apply default configuration and unlock settings?")) return;
    await api("/api/config/apply-defaults", { method: "POST", body: "{}" });
    toast("Defaults Applied");
    await loadStatus();
  });

  $("#clearFaults").addEventListener("click", async () => {
    if (!window.confirm("Clear all daemon and hardware fault counters?")) return;
    await api("/api/faults/clear", { method: "POST", body: "{}" });
    toast("Faults Cleared");
    $("#faultModal").close();
    await loadStatus();
  });

  $$("[data-restart]").forEach((button) => button.addEventListener("click", async () => {
    const target = button.dataset.restart;
    if (!window.confirm(`Restart ${target}?`)) return;
    await api("/api/system/restart", { method: "POST", body: JSON.stringify({ target }) });
    toast("Restart command sent");
  }));

  $("#esiUploadForm").addEventListener("submit", async (event) => {
    event.preventDefault();
    const file = $("#esiFile").files[0];
    if (!file) return;
    const form = new FormData();
    form.append("file", file);
    await api("/api/esi/upload", { method: "POST", body: form });
    toast("ESI/XML Uploaded");
  });

  $("#setStationId").addEventListener("click", async () => {
    if (!window.confirm("Set Station ID for this SubDevice?")) return;
    await api("/api/subdevices/station-id", {
      method: "POST",
      body: JSON.stringify({
        subDeviceIndex: state.selectedSubDeviceIndex,
        stationId: $("#stationIdInput").value
      })
    });
    toast("Station ID Sent");
  });
}

bindEvents();
connectLogs();
loadStatus().then(() => Promise.all([loadConfig(), loadLogs()])).catch((error) => {
  appendConsoleLine(`error: ${error.message}`);
});

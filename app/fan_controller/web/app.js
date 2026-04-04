const dom = {
  apState: document.getElementById("apState"),
  apInfo: document.getElementById("apInfo"),
  staState: document.getElementById("staState"),
  staInfo: document.getElementById("staInfo"),
  ssid: document.getElementById("ssid"),
  psk: document.getElementById("psk"),
  saveWifiBtn: document.getElementById("saveWifiBtn"),
  refreshBtn: document.getElementById("refreshBtn"),
  refreshState: document.getElementById("refreshState"),
  lastRefresh: document.getElementById("lastRefresh"),
  configEditor: document.getElementById("configEditor"),
  loadConfigBtn: document.getElementById("loadConfigBtn"),
  saveConfigBtn: document.getElementById("saveConfigBtn"),
  loadFieldsBtn: document.getElementById("loadFieldsBtn"),
  fieldDefinitions: document.getElementById("fieldDefinitions"),
  fsPath: document.getElementById("fsPath"),
  browseFsBtn: document.getElementById("browseFsBtn"),
  mkdirFsBtn: document.getElementById("mkdirFsBtn"),
  fsEntries: document.getElementById("fsEntries"),
  filePath: document.getElementById("filePath"),
  fileEditor: document.getElementById("fileEditor"),
  loadFileBtn: document.getElementById("loadFileBtn"),
  saveFileBtn: document.getElementById("saveFileBtn"),
  deleteFileBtn: document.getElementById("deleteFileBtn"),
};

const fanUiState = {
  1: { dirty: false, percent: 0, targetRpm: 0 },
  2: { dirty: false, percent: 0, targetRpm: 0 },
};

let refreshInFlight = false;

async function requestText(path, options = {}) {
  const response = await fetch(path, options);
  const text = await response.text();

  if (!response.ok) {
    throw new Error(text || response.statusText);
  }

  return text;
}

async function requestJson(path, options = {}) {
  return JSON.parse(await requestText(path, options));
}

function fanRefs(id) {
  return {
    badge: document.getElementById(`fan${id}Badge`),
    enable: document.getElementById(`fan${id}Enable`),
    useAdcTarget: document.getElementById(`fan${id}UseAdcTarget`),
    targetRpm: document.getElementById(`fan${id}TargetRpm`),
    percent: document.getElementById(`fan${id}Percent`),
    effectivePercent: document.getElementById(`fan${id}EffectivePercent`),
    adcRaw: document.getElementById(`fan${id}AdcRaw`),
    adc: document.getElementById(`fan${id}Adc`),
    mappedVoltage: document.getElementById(`fan${id}MappedVoltage`),
    adcTargetPercent: document.getElementById(`fan${id}AdcTargetPercent`),
    pwmPercent: document.getElementById(`fan${id}PwmPercent`),
    targetRpmValue: document.getElementById(`fan${id}TargetRpmValue`),
    rpm: document.getElementById(`fan${id}Rpm`),
    save: document.getElementById(`saveFan${id}Btn`),
    saveDefaults: document.getElementById(`saveFan${id}DefaultsBtn`),
  };
}

function markFanDirty(id) {
  fanUiState[id].dirty = true;
}

function clearFanDirty(id) {
  fanUiState[id].dirty = false;
}

function updateTargetInputState(id) {
  const refs = fanRefs(id);
  const manualMode = !refs.useAdcTarget.checked;
  refs.targetRpm.disabled = !manualMode;
  refs.targetRpm.placeholder = manualMode ? "输入目标转速" : "ADC 模式自动计算";
}

function applyFan(id, fan) {
  const refs = fanRefs(id);
  const ui = fanUiState[id];

  ui.percent = fan.percent;
  ui.targetRpm = fan.target_rpm;

  if (!ui.dirty) {
    refs.enable.checked = fan.enabled;
    refs.useAdcTarget.checked = fan.use_adc_target;
    refs.targetRpm.value = String(fan.target_rpm);
  }

  updateTargetInputState(id);

  refs.percent.textContent = `手动 ${fan.percent}%`;
  refs.effectivePercent.textContent = `${fan.effective_percent}%`;
  refs.adcRaw.textContent = String(fan.adc_raw);
  refs.adc.textContent = `${fan.adc_mv} mV`;
  refs.mappedVoltage.textContent = `${fan.mapped_voltage_mv} mV`;
  refs.adcTargetPercent.textContent = `${fan.adc_target_percent}%`;
  refs.pwmPercent.textContent = `${fan.pwm_percent}%`;
  refs.targetRpmValue.textContent = `${fan.target_rpm} rpm`;
  refs.rpm.textContent = `${fan.actual_rpm} rpm`;

  if (!fan.enabled) {
    refs.badge.textContent = "stopped";
  } else if (fan.use_adc_target) {
    refs.badge.textContent = "adc-target";
  } else {
    refs.badge.textContent = "rpm-target";
  }
}

function shouldPreserveUserInput(element) {
  return document.activeElement === element;
}

async function refreshStatus({ silent = false } = {}) {
  if (refreshInFlight) {
    return;
  }

  refreshInFlight = true;

  try {
    const status = await requestJson("/api/status");

    dom.apState.textContent = status.ap.enabled ? "enabled" : "disabled";
    dom.staState.textContent = status.sta.connected ? "connected" : status.sta.state;
    dom.apInfo.textContent =
      `SSID: ${status.ap.ssid}\nPSK: ${status.ap.psk}\nIP: ${status.ap.ip}\nClients: ${status.ap.clients}`;
    dom.staInfo.textContent =
      `Saved SSID: ${status.sta.ssid || "-"}\nState: ${status.sta.state}\nRSSI: ${status.sta.rssi}`;

    if (!shouldPreserveUserInput(dom.ssid)) {
      dom.ssid.value = status.sta.ssid || "";
    }

    applyFan(1, status.fans[0]);
    applyFan(2, status.fans[1]);

    dom.refreshState.textContent = "运行中";
    dom.lastRefresh.textContent = new Date().toLocaleTimeString("zh-CN", { hour12: false });
  } catch (error) {
    dom.refreshState.textContent = "连接失败";
    if (!silent) {
      throw error;
    }
  } finally {
    refreshInFlight = false;
  }
}

async function saveFan(id) {
  const refs = fanRefs(id);
  const body = new URLSearchParams({
    id: String(id),
    enabled: refs.enable.checked ? "1" : "0",
    use_adc_target: refs.useAdcTarget.checked ? "1" : "0",
  });

  if (!refs.useAdcTarget.checked) {
    const targetRpm = Number(refs.targetRpm.value);
    if (!Number.isFinite(targetRpm) || targetRpm < 0) {
      window.alert("请输入有效的目标转速。");
      return;
    }

    body.set("target_rpm", String(Math.round(targetRpm)));
  }

  await requestText("/api/fan", {
    method: "POST",
    headers: {
      "content-type": "application/x-www-form-urlencoded",
    },
    body,
  });

  clearFanDirty(id);
  await refreshStatus();
}

async function saveFanDefaults(id) {
  const refs = fanRefs(id);
  const body = new URLSearchParams({
    id: String(id),
    enabled: refs.enable.checked ? "1" : "0",
    use_adc_target: refs.useAdcTarget.checked ? "1" : "0",
  });

  if (!refs.useAdcTarget.checked) {
    const targetRpm = Number(refs.targetRpm.value);
    if (!Number.isFinite(targetRpm) || targetRpm < 0) {
      window.alert("请输入有效的目标转速。");
      return;
    }

    body.set("target_rpm", String(Math.round(targetRpm)));
  }

  await requestText("/api/fan/defaults", {
    method: "POST",
    headers: {
      "content-type": "application/x-www-form-urlencoded",
    },
    body,
  });

  await loadConfig();
  window.alert(`风扇 ${id} 的开机默认值已保存。`);
}

async function saveWifi() {
  const body = new URLSearchParams({
    ssid: dom.ssid.value.trim(),
    psk: dom.psk.value,
  });

  await requestText("/api/wifi", {
    method: "POST",
    headers: {
      "content-type": "application/x-www-form-urlencoded",
    },
    body,
  });

  await refreshStatus();
  await loadConfig();
  window.alert("Wi‑Fi 凭据已保存，设备开始尝试联网。");
}

async function loadConfig() {
  dom.configEditor.value = await requestText("/api/config");
}

async function saveConfig() {
  const saved = await requestText("/api/config", {
    method: "POST",
    headers: {
      "content-type": "application/json",
    },
    body: dom.configEditor.value,
  });

  dom.configEditor.value = saved;
  await refreshStatus();
  await browseFs(dom.fsPath.value || "/");
  window.alert("JSON 配置已保存。风扇相关字段只在下次开机时生效。");
}

async function loadFieldDefinitions() {
  dom.fieldDefinitions.textContent = await requestText("/api/config/fields");
}

function renderEntries(entries) {
  dom.fsEntries.innerHTML = "";

  for (const entry of entries) {
    const button = document.createElement("button");
    button.type = "button";
    button.className = `file-entry ${entry.type}`;
    button.textContent = `${entry.type === "dir" ? "[DIR]" : "[FILE]"} ${entry.path}`;
    button.addEventListener("click", () => {
      if (entry.type === "dir") {
        void browseFs(entry.path);
      } else {
        dom.filePath.value = entry.path;
        void loadFile(entry.path);
      }
    });
    dom.fsEntries.appendChild(button);
  }
}

async function browseFs(path = "/") {
  const data = await requestJson(`/api/fs/list?path=${encodeURIComponent(path)}`);
  dom.fsPath.value = data.path;
  renderEntries(data.entries);
}

async function loadFile(path = dom.filePath.value.trim()) {
  if (!path) {
    window.alert("请输入文件路径。");
    return;
  }

  dom.filePath.value = path;
  dom.fileEditor.value = await requestText(`/api/fs/file?path=${encodeURIComponent(path)}`);
}

async function saveFile() {
  const path = dom.filePath.value.trim();
  if (!path) {
    window.alert("请输入文件路径。");
    return;
  }

  await requestText(`/api/fs/file?path=${encodeURIComponent(path)}`, {
    method: "POST",
    headers: {
      "content-type": "text/plain; charset=utf-8",
    },
    body: dom.fileEditor.value,
  });

  await browseFs(dom.fsPath.value || "/");
}

async function mkdirFs() {
  const path = window.prompt("输入要创建的目录路径", dom.fsPath.value || "/etc/fanctl/new_dir");
  if (!path) {
    return;
  }

  await requestText(`/api/fs/mkdir?path=${encodeURIComponent(path)}`, {
    method: "POST",
  });

  await browseFs(dom.fsPath.value || "/");
}

async function deleteFile() {
  const path = dom.filePath.value.trim();
  if (!path) {
    window.alert("请输入文件路径。");
    return;
  }

  if (!window.confirm(`确认删除 ${path} ?`)) {
    return;
  }

  await requestText(`/api/fs/delete?path=${encodeURIComponent(path)}`, {
    method: "POST",
  });

  dom.fileEditor.value = "";
  await browseFs(dom.fsPath.value || "/");
}

for (const id of [1, 2]) {
  const refs = fanRefs(id);
  refs.enable.addEventListener("change", () => markFanDirty(id));
  refs.useAdcTarget.addEventListener("change", () => {
    markFanDirty(id);
    updateTargetInputState(id);
  });
  refs.targetRpm.addEventListener("input", () => markFanDirty(id));
  refs.save.addEventListener("click", () => {
    void saveFan(id).catch((error) => window.alert(error.message));
  });
  refs.saveDefaults.addEventListener("click", () => {
    void saveFanDefaults(id).catch((error) => window.alert(error.message));
  });
}

dom.saveWifiBtn.addEventListener("click", () => {
  void saveWifi().catch((error) => window.alert(error.message));
});

dom.refreshBtn.addEventListener("click", () => {
  void refreshStatus().catch((error) => window.alert(error.message));
});

dom.loadConfigBtn.addEventListener("click", () => {
  void loadConfig().catch((error) => window.alert(error.message));
});

dom.saveConfigBtn.addEventListener("click", () => {
  void saveConfig().catch((error) => window.alert(error.message));
});

dom.loadFieldsBtn.addEventListener("click", () => {
  void loadFieldDefinitions().catch((error) => window.alert(error.message));
});

dom.browseFsBtn.addEventListener("click", () => {
  void browseFs(dom.fsPath.value.trim() || "/").catch((error) => window.alert(error.message));
});

dom.mkdirFsBtn.addEventListener("click", () => {
  void mkdirFs().catch((error) => window.alert(error.message));
});

dom.loadFileBtn.addEventListener("click", () => {
  void loadFile().catch((error) => window.alert(error.message));
});

dom.saveFileBtn.addEventListener("click", () => {
  void saveFile().catch((error) => window.alert(error.message));
});

dom.deleteFileBtn.addEventListener("click", () => {
  void deleteFile().catch((error) => window.alert(error.message));
});

await refreshStatus();
await loadConfig();
await loadFieldDefinitions();
await browseFs("/");

window.setInterval(() => {
  void refreshStatus({ silent: true });
}, 2000);

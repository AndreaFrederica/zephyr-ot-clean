const cardsEl = document.getElementById("cards");
const eventsEl = document.getElementById("events");
const statusTextEl = document.getElementById("statusText");
const onlineCountEl = document.getElementById("onlineCount");
const refreshBtn = document.getElementById("refreshBtn");
const soundBtn = document.getElementById("soundBtn");
const themeBtn = document.getElementById("themeBtn");
const cardTpl = document.getElementById("nodeCardTpl");
const FLAME_MAX_MV = 3300;
const TEMP_MIN_C = 0;
const TEMP_MAX_C = 60;
const HUMI_MAX = 100;
let ws = null;
let alarmEnabled = false;
let audioCtx = null;
let lastAlarmAt = 0;

// 自动重试状态
const pendingLocks = new Map();
const lastUpdatedAt = new Map();
const LOCK_MAX_RETRIES = 20;

// 主题
function applyTheme(theme) {
  document.documentElement.setAttribute("data-theme", theme);
  themeBtn.textContent = theme === "dark" ? "☀️" : "🌙";
}

function initTheme() {
  const saved = localStorage.getItem("parking_lock_theme");
  if (saved === "dark" || saved === "light") {
    applyTheme(saved);
    return;
  }
  const prefersDark = window.matchMedia && window.matchMedia("(prefers-color-scheme: dark)").matches;
  applyTheme(prefersDark ? "dark" : "light");
}

function toggleTheme() {
  const current = document.documentElement.getAttribute("data-theme") || "light";
  const next = current === "dark" ? "light" : "dark";
  applyTheme(next);
  localStorage.setItem("parking_lock_theme", next);
}

if (window.matchMedia) {
  window.matchMedia("(prefers-color-scheme: dark)").addEventListener("change", (e) => {
    if (!localStorage.getItem("parking_lock_theme")) {
      applyTheme(e.matches ? "dark" : "light");
    }
  });
}

async function getJson(url) {
  const resp = await fetch(url);
  if (!resp.ok) {
    throw new Error(`${url} -> ${resp.status}`);
  }
  return await resp.json();
}

async function postJson(url, body) {
  const resp = await fetch(url, {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify(body),
  });
  if (!resp.ok) {
    const text = await resp.text();
    throw new Error(`${url} -> ${resp.status} ${text}`);
  }
  return await resp.json();
}

function lockText(lockState) {
  if (lockState === "1") return "LOCKED";
  if (lockState === "0") return "UNLOCKED";
  return "-";
}

function fireText(fd) {
  if (fd === "0") return "ALARM";
  if (fd === "1") return "NORMAL";
  return "-";
}

function clamp(num, min, max) {
  return Math.max(min, Math.min(max, num));
}

function numberValue(text, fallback = 0) {
  const v = Number(text);
  return Number.isFinite(v) ? v : fallback;
}

function flameRiskPercent(valueText) {
  const v = Number(valueText);
  if (!Number.isFinite(v)) return 100;
  return clamp(Math.round(((FLAME_MAX_MV - v) / FLAME_MAX_MV) * 100), 0, 100);
}

function tempPercent(valueText) {
  const v = numberValue(valueText, TEMP_MIN_C);
  return clamp(Math.round(((v - TEMP_MIN_C) / (TEMP_MAX_C - TEMP_MIN_C)) * 100), 0, 100);
}

function humiPercent(valueText) {
  const v = numberValue(valueText, 0);
  return clamp(Math.round((v / HUMI_MAX) * 100), 0, 100);
}

function flameLevelClass(fd, riskPercent) {
  if (fd === "0" || riskPercent >= 70) return "risk-high";
  if (riskPercent >= 40) return "risk-mid";
  return "risk-low";
}

function lockClass(lockState) {
  if (lockState === "1") return "chip-lock";
  if (lockState === "0") return "chip-unlock";
  return "chip-unknown";
}

function renderNodes(nodes) {
  cardsEl.innerHTML = "";
  const online = nodes.filter((n) => n.online);
  onlineCountEl.textContent = `${online.length} 个在线节点`;

  if (online.length === 0) {
    const empty = document.createElement("div");
    empty.className = "subtle";
    empty.textContent = "暂无在线节点";
    cardsEl.appendChild(empty);
    return;
  }

  for (const node of online) {
    const fragment = cardTpl.content.cloneNode(true);
    const root = fragment.querySelector(".card");
    const flameRiskPct = flameRiskPercent(node.flame_analog);
    const tempPct = tempPercent(node.temp);
    const humiPct = humiPercent(node.humi);
    const fireState = fireText(node.flame_digital);
    const riskCls = flameLevelClass(node.flame_digital, flameRiskPct);
    const lockStateText = lockText(node.lock_state);
    fragment.querySelector(".node-title").textContent = `节点 ${node.node_id}`;
    fragment.querySelector(".metrics").innerHTML = [
      `<div class="metric-grid">`,
      `  <div class="metric-item"><span>温度</span><strong>${node.temp} C</strong></div>`,
      `  <div class="metric-item"><span>湿度</span><strong>${node.humi} %</strong></div>`,
      `  <div class="metric-item"><span>锁状态</span><strong class="${lockClass(node.lock_state)}">${lockStateText}</strong></div>`,
      `  <div class="metric-item"><span>火焰数字</span><strong class="${riskCls}">${node.flame_digital} (${fireState})</strong></div>`,
      `</div>`,
      `<div class="gauge-grid">`,
      `  <div class="gauge-item">`,
      `    <div class="gauge-head"><span>温度仪表</span><span>${node.temp} C</span></div>`,
      `    <div class="gauge-track"><div class="gauge-fill gauge-temp" style="width:${tempPct}%"></div></div>`,
      `  </div>`,
      `  <div class="gauge-item">`,
      `    <div class="gauge-head"><span>湿度仪表</span><span>${node.humi} %</span></div>`,
      `    <div class="gauge-track"><div class="gauge-fill gauge-humi" style="width:${humiPct}%"></div></div>`,
      `  </div>`,
      `</div>`,
      `<div class="flame-wrap">`,
      `  <div class="flame-head"><span>火焰风险（3300=无火）</span><span>${node.flame_analog} mV | 风险 ${flameRiskPct}%</span></div>`,
      `  <div class="flame-track"><div class="flame-fill ${riskCls}-bg" style="width:${flameRiskPct}%"></div></div>`,
      `</div>`,
      `<div class="extra">原因: ${node.reason} | UID: ${node.uid}</div>`,
      `<div class="extra subtle">更新时间: ${node.updated_at}</div>`,
    ].join("");

    root.querySelector(".btn-get").addEventListener("click", async () => {
      pendingLocks.delete(node.node_id);
      await sendCommand(node.node_id, "GET", null);
    });
    root.querySelector(".btn-lock").addEventListener("click", async () => {
      pendingLocks.set(node.node_id, { expectedValue: "1", retriesLeft: LOCK_MAX_RETRIES });
      await sendCommand(node.node_id, "LOCK", 1);
    });
    root.querySelector(".btn-unlock").addEventListener("click", async () => {
      pendingLocks.set(node.node_id, { expectedValue: "0", retriesLeft: LOCK_MAX_RETRIES });
      await sendCommand(node.node_id, "LOCK", 0);
    });
    cardsEl.appendChild(fragment);
  }
}

function renderEvents(events) {
  eventsEl.innerHTML = "";
  for (const ev of events.slice().reverse()) {
    const li = document.createElement("li");
    li.textContent = `[${ev.ts}] ${ev.event} ${ev.raw}`;
    eventsEl.appendChild(li);
  }
}

async function sendCommand(nodeId, action, value) {
  try {
    statusTextEl.textContent = `发送命令中: ${action} -> 节点${nodeId}`;
    await postJson("/v1/commands", { node_id: nodeId, action, value });
    statusTextEl.textContent = `命令已入队: ${action} -> 节点${nodeId}`;
  } catch (err) {
    statusTextEl.textContent = `命令失败: ${err.message}`;
  }
}

function checkPendingLocks(nodes, changedNodeIds) {
  for (const node of nodes) {
    const pending = pendingLocks.get(node.node_id);
    if (!pending) continue;

    if (String(node.lock_state) === pending.expectedValue) {
      pendingLocks.delete(node.node_id);
      statusTextEl.textContent = `节点${node.node_id} 锁状态已达成 ${lockText(pending.expectedValue)}`;
      continue;
    }

    // 只有该节点收到了自然回包（updated_at 有更新）时才消耗重试次数；
    // ACK 执行回执不会更新 updated_at，因此不会误触发重试。
    if (!changedNodeIds.has(node.node_id)) {
      continue;
    }

    if (pending.retriesLeft > 0) {
      pending.retriesLeft -= 1;
      const value = Number(pending.expectedValue);
      statusTextEl.textContent = `重试 LOCK -> 节点${node.node_id} (剩余${pending.retriesLeft}次)`;
      sendCommand(node.node_id, "LOCK", value);
    } else {
      pendingLocks.delete(node.node_id);
      statusTextEl.textContent = `节点${node.node_id} 自动重试结束，未达成目标状态`;
    }
  }
}

function applySnapshot(snapshot) {
  if (!snapshot) return;
  const nodes = Array.isArray(snapshot.nodes) ? snapshot.nodes : [];
  const events = Array.isArray(snapshot.events) ? snapshot.events : [];

  // 过滤：只有自然回包（REPORT/OFFLINE）会更新节点的 updated_at；
  // ACK 等执行回执不会更新。用 updated_at 变化来判断是否收到了新的自然回包。
  const changedNodeIds = new Set();
  for (const node of nodes) {
    if (lastUpdatedAt.has(node.node_id) && lastUpdatedAt.get(node.node_id) !== node.updated_at) {
      changedNodeIds.add(node.node_id);
    }
    lastUpdatedAt.set(node.node_id, node.updated_at);
  }

  renderNodes(nodes);
  renderEvents(events);
  checkPendingLocks(nodes, changedNodeIds);

  const mqttMark = snapshot.mqtt_connected ? "MQTT OK" : "MQTT OFF";
  statusTextEl.textContent = `实时更新 ${new Date().toLocaleTimeString()} | ${mqttMark}`;
  maybeAlarm(nodes);
}

function wsUrl() {
  const proto = window.location.protocol === "https:" ? "wss" : "ws";
  return `${proto}://${window.location.host}/ws`;
}

function connectWs() {
  if (ws) {
    ws.close();
  }
  ws = new WebSocket(wsUrl());
  ws.onopen = () => {
    statusTextEl.textContent = `WS 已连接 ${new Date().toLocaleTimeString()}`;
  };
  ws.onmessage = (ev) => {
    try {
      const msg = JSON.parse(ev.data);
      if (msg.type === "snapshot") {
        applySnapshot(msg);
      }
    } catch (err) {
      statusTextEl.textContent = `WS 数据错误: ${err.message}`;
    }
  };
  ws.onclose = () => {
    statusTextEl.textContent = "WS 已断开，重连中...";
    window.setTimeout(connectWs, 1500);
  };
  ws.onerror = () => {
    statusTextEl.textContent = "WS 错误，尝试重连...";
  };
}

function ensureAudio() {
  if (!audioCtx) {
    audioCtx = new (window.AudioContext || window.webkitAudioContext)();
  }
  return audioCtx;
}

function beepOnce() {
  const ctx = ensureAudio();
  const osc = ctx.createOscillator();
  const gain = ctx.createGain();
  osc.type = "square";
  osc.frequency.value = 880;
  gain.gain.value = 0.05;
  osc.connect(gain);
  gain.connect(ctx.destination);
  osc.start();
  osc.stop(ctx.currentTime + 0.15);
}

function maybeAlarm(nodes) {
  if (!alarmEnabled) return;
  const high = nodes.some((n) => {
    if (!n.online) return false;
    const risk = flameRiskPercent(n.flame_analog);
    return n.flame_digital === "0" || risk >= 70;
  });
  if (!high) return;

  const now = Date.now();
  if (now - lastAlarmAt < 2000) return;
  lastAlarmAt = now;
  beepOnce();
}

async function refreshAll() {
  try {
    const [nodes, events, health] = await Promise.all([
      getJson("/v1/nodes"),
      getJson("/v1/events?limit=80"),
      getJson("/health"),
    ]);
    applySnapshot({
      type: "snapshot",
      nodes,
      events,
      mqtt_connected: Boolean(health.mqtt_connected),
    });
    statusTextEl.textContent = `已更新 ${new Date().toLocaleTimeString()}`;
  } catch (err) {
    statusTextEl.textContent = `刷新失败: ${err.message}`;
  }
}

refreshBtn.addEventListener("click", refreshAll);
themeBtn.addEventListener("click", toggleTheme);
soundBtn.addEventListener("click", async () => {
  try {
    ensureAudio();
    if (audioCtx.state === "suspended") {
      await audioCtx.resume();
    }
    alarmEnabled = !alarmEnabled;
    soundBtn.textContent = alarmEnabled ? "关闭警示音" : "启用警示音";
  } catch (err) {
    statusTextEl.textContent = `音频初始化失败: ${err.message}`;
  }
});

initTheme();
refreshAll();
connectWs();
setInterval(refreshAll, 30000);

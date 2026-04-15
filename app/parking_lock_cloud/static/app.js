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

// 事件委托：监听器绑在不会被销毁的父容器上，避免高频重渲染时按钮失效
cardsEl.addEventListener("click", async (e) => {
  const btn = e.target.closest("[data-cmd]");
  if (!btn) return;
  const card = btn.closest(".card");
  if (!card) return;
  const nodeId = Number(card.dataset.nodeId);
  const cmd = btn.dataset.cmd;
  const val = btn.dataset.val !== undefined ? Number(btn.dataset.val) : null;
  await sendCommand(nodeId, cmd, val);
});
let audioCtx = null;
let lastAlarmAt = 0;


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

// 卡片缓存：key=node_id，value={ el: .card DOM, sig: 上次渲染的数据签名 }
// 保持卡片 DOM 稳定，避免 mousedown→mouseup 之间 innerHTML="" 导致 click 不触发
const _cardCache = new Map();

function nodeSignature(node) {
  return `${node.temp}|${node.humi}|${node.lock_state}|${node.flame_digital}|${node.flame_analog}|${node.reason}|${node.uid}|${node.updated_at}`;
}

function updateCardMetrics(root, node) {
  const flameRiskPct = flameRiskPercent(node.flame_analog);
  const tempPct = tempPercent(node.temp);
  const humiPct = humiPercent(node.humi);
  const fireState = fireText(node.flame_digital);
  const riskCls = flameLevelClass(node.flame_digital, flameRiskPct);
  const lockStateText = lockText(node.lock_state);
  root.querySelector(".metrics").innerHTML = [
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
}

function renderNodes(nodes) {
  const online = nodes.filter((n) => n.online);
  onlineCountEl.textContent = `${online.length} 个在线节点`;

  // 移除已下线节点的卡片
  const onlineIds = new Set(online.map((n) => n.node_id));
  for (const [id, el] of _cardCache) {
    if (!onlineIds.has(id)) {
      el.remove();
      _cardCache.delete(id);
    }
  }

  if (online.length === 0) {
    // 无在线节点时显示占位文字（仅在没有卡片时插入）
    if (!cardsEl.querySelector(".no-nodes-hint")) {
      cardsEl.innerHTML = "";
      const empty = document.createElement("div");
      empty.className = "subtle no-nodes-hint";
      empty.textContent = "暂无在线节点";
      cardsEl.appendChild(empty);
    }
    return;
  }

  // 移除占位文字
  const hint = cardsEl.querySelector(".no-nodes-hint");
  if (hint) hint.remove();

  for (const node of online) {
    const sig = nodeSignature(node);
    const cached = _cardCache.get(node.node_id);
    if (!cached) {
      // 首次出现：从模板克隆，加入 DOM 和缓存
      const fragment = cardTpl.content.cloneNode(true);
      const root = fragment.querySelector(".card");
      root.dataset.nodeId = node.node_id;
      root.querySelector(".node-title").textContent = `节点 ${node.node_id}`;
      updateCardMetrics(root, node);
      _cardCache.set(node.node_id, { el: root, sig });
      cardsEl.appendChild(fragment);
    } else if (cached.sig !== sig) {
      // 数据有变化才更新，避免无意义的 DOM 写入
      updateCardMetrics(cached.el, node);
      cached.sig = sig;
    }
    // sig 相同 → 跳过，完全不触碰 DOM
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

function applySnapshot(snapshot) {
  if (!snapshot) return;
  const nodes = Array.isArray(snapshot.nodes) ? snapshot.nodes : [];
  const events = Array.isArray(snapshot.events) ? snapshot.events : [];

  renderNodes(nodes);
  renderEvents(events);

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

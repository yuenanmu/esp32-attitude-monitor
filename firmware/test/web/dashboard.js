import { renderLatest } from "./student-live.js";

const el = (id) => document.getElementById(id);
const axes = ["roll", "pitch", "yaw"];
const integer = new Intl.NumberFormat("zh-CN");
const points = [];
const windowUs = 10_000_000;
const pollMs = 150;
let mode = "live";
let modeEpoch = 0;
let lastSample = null;
let pose = null;
let controlState = null;
let submitting = false;
let awaitingId = null;
let controlRevision = 0;
let submitError = "";

import("./pose.js").then(({ createPoseView }) => {
  pose = createPoseView(el("pose"), el("reset-view"));
  el("pose-fallback").hidden = true;
  if (lastSample) pose.setAngles(lastSample);
}).catch((error) => {
  el("pose-fallback").textContent = "3D 无法启动，仍可查看角度和趋势。";
  console.warn("3D view unavailable:", error);
});

function setStatus(text, tone) {
  el("connection-text").textContent = text;
  el("connection").dataset.tone = tone;
}

function drawChart() {
  const canvas = el("trend");
  const ctx = canvas.getContext("2d");
  const width = canvas.clientWidth;
  const height = canvas.clientHeight;
  if (!ctx || width < 90 || height < 65) return;
  const scale = Math.min(devicePixelRatio || 1, 2);
  const pixelWidth = Math.round(width * scale);
  const pixelHeight = Math.round(height * scale);
  if (canvas.width !== pixelWidth) canvas.width = pixelWidth;
  if (canvas.height !== pixelHeight) canvas.height = pixelHeight;
  ctx.setTransform(scale, 0, 0, scale, 0, 0);
  ctx.clearRect(0, 0, width, height);

  const left = 42, right = width - 18, top = 15, bottom = height - 26;
  const y = (value) => top + (180 - value) / 360 * (bottom - top);
  const x = (ageUs) => right - ageUs / windowUs * (right - left);
  ctx.font = "11px Consolas, monospace";
  ctx.fillStyle = "#a2b1c3";
  ctx.textAlign = "right";
  for (const value of [180, 90, 0, -90, -180]) {
    ctx.strokeStyle = value === 0 ? "#415469" : "#243345";
    ctx.beginPath(); ctx.moveTo(left, y(value)); ctx.lineTo(right, y(value)); ctx.stroke();
    ctx.fillText(String(value), left - 6, y(value) + 4);
  }
  for (const seconds of [10, 8, 6, 4, 2, 0]) {
    const xpos = x(seconds * 1_000_000);
    ctx.strokeStyle = "#243345";
    ctx.beginPath(); ctx.moveTo(xpos, top); ctx.lineTo(xpos, bottom); ctx.stroke();
    ctx.textAlign = seconds === 0 ? "right" : "center";
    ctx.fillText(seconds === 0 ? "0s" : `-${seconds}s`, xpos, height - 8);
  }

  el("chart-empty").hidden = points.length > 1;
  if (points.length < 2) return;
  const end = points.at(-1).t_us;
  ctx.save();
  ctx.beginPath(); ctx.rect(left, top, right - left, bottom - top); ctx.clip();
  for (const [axis, color] of [["roll", "#55decf"], ["pitch", "#ffc878"], ["yaw", "#b9a1ff"]]) {
    ctx.strokeStyle = color;
    ctx.lineWidth = 1.8;
    ctx.lineJoin = "round";
    ctx.beginPath();
    let previous;
    for (const sample of points) {
      const xpos = x(end - sample.t_us);
      const ypos = y(sample[axis]);
      // Latest-only polling skips sequence IDs by design. Break only at time gaps or angle wrap.
      if (!previous || sample.t_us - previous.t_us > 1_000_000 ||
          Math.abs(sample[axis] - previous[axis]) > 180) {
        ctx.moveTo(xpos, ypos);
      } else {
        ctx.lineTo(xpos, ypos);
      }
      previous = sample;
    }
    ctx.stroke();
  }
  ctx.restore();
}

function renderSample(sample) {
  if (![sample.seq, sample.t_us, ...axes.map((axis) => sample[axis])].every(Number.isFinite)) return;
  if (lastSample && lastSample.seq === sample.seq && lastSample.t_us === sample.t_us) return;
  if (lastSample && (sample.seq < lastSample.seq || sample.t_us <= lastSample.t_us)) points.length = 0;
  lastSample = sample;
  points.push(sample);
  while (points.length > 120 || sample.t_us - points[0].t_us > windowUs) points.shift();
  for (const axis of axes) el(axis).textContent = sample[axis].toFixed(1);
  el("seq").textContent = integer.format(sample.seq);
  pose?.setAngles(sample);
  drawChart();
}

function renderStats(state) {
  el("received").textContent = Number.isFinite(state.received_count) ? integer.format(state.received_count) : "—";
  el("missing").textContent = Number.isFinite(state.missing_count) ? integer.format(state.missing_count) : "—";
  el("last-seen").textContent = Number.isFinite(state.last_received_at)
    ? new Date(state.last_received_at * 1000).toLocaleTimeString("zh-CN", { hour12: false }) : "—";
}

function renderControls() {
  const device = controlState?.device_status;
  const operation = controlState?.control;
  if (awaitingId !== null && operation?.id >= awaitingId) awaitingId = null;
  const live = mode === "live";
  const connected = live && Boolean(controlState?.connected);
  const busy = submitting || awaitingId !== null || operation?.phase === "queued" ||
    operation?.phase === "running" || device?.pending || device?.mode === "CALIBRATING";
  for (const id of ["calibrate", "sample-rate", "apply-rate"]) {
    el(id).disabled = !connected || Boolean(busy);
  }
  el("device-rate").textContent = connected && device ? `${device.rate_hz} Hz` : "—";

  let message = "重新校准时请保持开发板静止。";
  let tone = "";
  const name = operation?.action === "calibrate"
    ? "陀螺仪校准" : `设置采样率为 ${operation?.rate_hz} Hz`;
  if (!live) {
    message = "演示模式不能控制开发板。";
  } else if (!connected) {
    message = operation?.phase === "unknown" ? "设备已断开，操作结果未确认。" : "等待开发板连接。";
  } else if (submitting) {
    message = "正在提交请求…";
  } else if (awaitingId !== null) {
    message = "请求已提交，等待设备处理。";
  } else if (submitError) {
    message = submitError;
    tone = "warn";
  } else if (operation?.phase === "queued") {
    message = `${name}：已排队。`;
  } else if (device?.mode === "CALIBRATING") {
    message = "正在校准，请保持开发板静止。";
  } else if (operation?.phase === "running") {
    message = `${name}：等待设备确认。`;
  } else if (operation?.phase === "succeeded") {
    message = `${name}成功。`;
    tone = "live";
  } else if (operation?.phase === "failed" || operation?.phase === "unknown") {
    const result = operation.phase === "failed" ? "失败" : "结果未确认";
    message = `${name}${result}${operation.detail ? `：${operation.detail}` : "。"}`;
    tone = "warn";
  }
  const feedback = el("control-feedback");
  if (feedback.textContent !== message) feedback.textContent = message;
  feedback.dataset.tone = tone;
}

async function submitControl(payload) {
  if (submitting || mode !== "live" || !controlState?.connected) return;
  submitting = true;
  submitError = "";
  renderControls();
  try {
    const response = await fetch("/api/control", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify(payload),
      signal: AbortSignal.timeout(3000),
    });
    const body = await response.json();
    if (response.status === 202) {
      awaitingId = body.id;
      controlRevision++;
    } else {
      submitError = body.error || `提交失败（HTTP ${response.status}）。`;
    }
  } catch {
    submitError = "请求结果未确认，请先查看设备状态。";
  } finally {
    submitting = false;
    renderControls();
  }
}

function switchMode(nextMode) {
  if (mode === nextMode) return;
  mode = nextMode;
  modeEpoch++;
  el("live-mode").setAttribute("aria-pressed", String(mode === "live"));
  el("demo-mode").setAttribute("aria-pressed", String(mode === "demo"));
  for (const id of [...axes, "seq", "received", "missing", "last-seen"]) el(id).textContent = "—";
  lastSample = null;
  points.length = 0;
  pose?.clear();
  drawChart();
  el("connection").title = "";
  setStatus(mode === "demo" ? "演示模式 · 模拟数据" : "等待设备", mode === "demo" ? "demo" : "idle");
  el("source-note").textContent = mode === "demo" ? "演示模式 · 模拟数据" : "设备数据 · USB → Python → Web";
  if (mode === "demo") controlState = null;
  renderControls();
}

function demoSample() {
  const now = performance.now();
  const seconds = now / 1000;
  return {
    seq: Math.floor(now / pollMs) + 1, t_us: Math.round(now * 1000),
    roll: 32 * Math.sin(seconds * 0.9),
    pitch: 23 * Math.cos(seconds * 1.15),
    yaw: 68 * Math.sin(seconds * 0.54)
  };
}

async function poll() {
  const epoch = modeEpoch;
  const revision = controlRevision;
  try {
    if (mode === "demo") {
      renderSample(demoSample());
      return;
    }
    const response = await fetch("/api/state", { cache: "no-store", signal: AbortSignal.timeout(2200) });
    if (!response.ok) throw new Error(`HTTP ${response.status}`);
    const state = await response.json();
    if (epoch !== modeEpoch) return;
    if (revision === controlRevision) {
      if (awaitingId !== null && (!state.control || state.control.id < awaitingId)) {
        submitError = "找不到本次请求，请检查桥接是否已重启。";
        awaitingId = null;
      }
      controlState = state;
      renderControls();
    }
    renderStats(state);
    if (!state.connected) {
      setStatus(lastSample ? "设备未连接 · 保留旧值" : "设备未连接", "warn");
      el("connection").title = state.last_error || "";
      return;
    }
    el("connection").title = "";
    if (!state.latest) {
      setStatus("已连接 · 等待姿态", "idle");
      return;
    }
    const age = Date.now() / 1000 - state.last_received_at;
    if (Number.isFinite(state.last_received_at) && age > 2) {
      setStatus("设备在线 · 暂无新姿态", "warn");
    } else {
      setStatus("设备在线", "live");
    }
    renderLatest(state.latest, renderSample);
  } catch (error) {
    if (epoch === modeEpoch) {
      controlState = null;
      awaitingId = null;
      renderControls();
      setStatus(lastSample ? "桥接不可用 · 保留旧值" : "桥接不可用", "warn");
      el("connection").title = error.message;
    }
  } finally {
    setTimeout(poll, pollMs);
  }
}

el("demo-mode").addEventListener("click", () => switchMode("demo"));
el("live-mode").addEventListener("click", () => switchMode("live"));
el("calibrate").addEventListener("click", () => submitControl({ action: "calibrate" }));
el("apply-rate").addEventListener("click", () => submitControl({
  action: "set_rate", rate_hz: Number(el("sample-rate").value),
}));
new ResizeObserver(drawChart).observe(el("trend"));
poll();

// Polls the inspector for a fresh read of the target's memory and draws it.
// Every URL here is relative so the page works behind a proxy as well as on localhost.

const WORLD_RANGE = 460; // world units drawn from the centre to the edge of the radar
const POLL_MS = 100;

const el = (id) => document.getElementById(id);
const canvas = el("radar");
const ctx = canvas.getContext("2d");

let lastCount = -1;
let lastState = null;

async function poll() {
  try {
    const res = await fetch("api/state", { cache: "no-store" });
    const state = await res.json();
    state.ok ? render(state) : showProblem(state.error || "target not available");
  } catch (err) {
    showProblem(String(err));
  }
}

function showProblem(message) {
  const pill = el("status-pill");
  const starting = /LookupError|no process named/.test(message);
  pill.textContent = starting ? "waiting for target_sim" : "error";
  pill.className = "pill " + (starting ? "waiting" : "error");
  el("read-time").textContent = starting ? "run: make run-target" : message;
  lastState = null;
  drawRadar(null);
}

function render(state) {
  const t = state.target;

  el("status-pill").textContent = "attached to pid " + t.pid;
  el("status-pill").className = "pill ok";
  el("read-time").textContent =
    `${state.world.entity_count} entities, ${state.read_us} us per read, tick ${state.world.tick}`;

  el("v-pid").textContent = `pid ${t.pid}`;
  el("v-base").textContent = `${t.module_base}${t.pie ? "  (pie)" : "  (fixed load address)"}`;
  el("v-symbol").textContent = `${t.symbol} + ${t.symbol_offset}`;
  el("v-pointer").textContent = t.pointer_addr;
  el("v-world").textContent = `${t.world_addr}  ${t.magic_ok ? "magic ok" : "magic mismatch"}`;
  el("v-exe").textContent = t.exe;

  renderTable(state.entities);
  renderWriteOptions(state.entities);
  el("hex-header").textContent = state.hex.header.join("\n");
  el("hex-entity").textContent = state.hex.entity0.join("\n");

  lastState = state;
  drawRadar(state);
}

function renderTable(entities) {
  const rows = entities.map((e) => {
    const o = e.origin;
    const hp = Math.max(0, Math.min(100, e.health));
    return `<tr>
      <td>${e.index}</td>
      <td>${e.id}</td>
      <td class="team-${e.team}">${e.name}</td>
      <td class="team-${e.team}">${e.team}</td>
      <td><div class="hp"><div class="hp-bar"><div class="hp-fill" style="width:${hp}%"></div></div>${String(e.health).padStart(3, " ")}</div></td>
      <td>${fmt(o.x)}, ${fmt(o.y)}, ${fmt(o.z)}</td>
      <td class="addr">${e.address}</td>
    </tr>`;
  });
  el("entity-rows").innerHTML =
    rows.join("") || '<tr class="empty"><td colspan="7">no entities</td></tr>';
}

function renderWriteOptions(entities) {
  if (entities.length === lastCount) return;
  lastCount = entities.length;
  el("write-index").innerHTML = entities
    .map((e) => `<option value="${e.index}">${e.index} - ${e.name}</option>`)
    .join("");
}

const fmt = (v) => (v >= 0 ? " " : "") + v.toFixed(1).padStart(6, " ");

function drawRadar(state) {
  const size = canvas.width;
  const c = size / 2;
  const scale = (c - 28) / WORLD_RANGE;

  ctx.clearRect(0, 0, size, size);
  ctx.fillStyle = "#0a0e14";
  ctx.fillRect(0, 0, size, size);

  // range rings every 100 world units
  ctx.strokeStyle = "#1b2431";
  ctx.lineWidth = 1;
  for (let r = 100; r <= WORLD_RANGE; r += 100) {
    ctx.beginPath();
    ctx.arc(c, c, r * scale, 0, Math.PI * 2);
    ctx.stroke();
  }
  ctx.beginPath();
  ctx.moveTo(c, 10); ctx.lineTo(c, size - 10);
  ctx.moveTo(10, c); ctx.lineTo(size - 10, c);
  ctx.stroke();

  if (!state) {
    ctx.fillStyle = "#7d8795";
    ctx.font = "13px ui-monospace, monospace";
    ctx.textAlign = "center";
    ctx.fillText("waiting for target_sim", c, c - 8);
    ctx.fillText("make run-target", c, c + 12);
    return;
  }

  // camera: position plus the direction it is facing
  const cam = state.world.camera;
  const camX = c + cam.x * scale;
  const camY = c - cam.y * scale;
  ctx.strokeStyle = "#4ea3ff";
  ctx.lineWidth = 1.5;
  ctx.beginPath();
  ctx.moveTo(camX, camY);
  ctx.lineTo(camX + Math.cos(state.world.yaw) * 34, camY - Math.sin(state.world.yaw) * 34);
  ctx.stroke();
  ctx.fillStyle = "#4ea3ff";
  ctx.beginPath();
  ctx.arc(camX, camY, 4, 0, Math.PI * 2);
  ctx.fill();

  ctx.font = "11px ui-monospace, monospace";
  ctx.textAlign = "left";

  for (const e of state.entities) {
    const x = c + e.origin.x * scale;
    const y = c - e.origin.y * scale;
    const color = e.team === 2 ? "#e0a300" : "#4ea3ff";

    // height above the ground plane shown as a soft halo
    ctx.fillStyle = color + "22";
    ctx.beginPath();
    ctx.arc(x, y, 9 + Math.abs(e.origin.z) * 0.12, 0, Math.PI * 2);
    ctx.fill();

    ctx.fillStyle = color;
    ctx.beginPath();
    ctx.arc(x, y, 5, 0, Math.PI * 2);
    ctx.fill();

    ctx.fillStyle = "#d7dee8";
    ctx.fillText(e.name, x + 10, y - 2);
    ctx.fillStyle = "#7d8795";
    ctx.fillText(e.health + " hp", x + 10, y + 10);
  }
}

el("write-button").addEventListener("click", async () => {
  const index = Number(el("write-index").value);
  const value = Number(el("write-value").value);
  const out = el("write-result");
  out.textContent = "writing...";
  try {
    const res = await fetch("api/write", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ index, value }),
    });
    const data = await res.json();
    out.textContent = data.ok
      ? `wrote ${data.value} to ${data.address} - the target's console reports the change`
      : `failed: ${data.error}`;
  } catch (err) {
    out.textContent = "failed: " + err;
  }
});

drawRadar(null);
poll();
setInterval(poll, POLL_MS);

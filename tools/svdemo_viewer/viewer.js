/* Top-down .sbd viewer. No build step, no external assets. */
(function () {
  "use strict";

  const S = window.Svdemo;
  const COLORS = [
    "#ff6b6b",
    "#4ecdc4",
    "#ffd166",
    "#a78bfa",
    "#60a5fa",
    "#34d399",
    "#fb923c",
    "#f472b6",
    "#e879f9",
    "#facc15",
    "#38bdf8",
    "#fca5a5",
    "#86efac",
    "#c4b5fd",
    "#fdba74",
    "#67e8f9",
  ];
  const TRAIL_WINDOW_MS = 60_000;
  const MAX_TRAIL_POINTS = 1400;

  const $ = (id) => document.getElementById(id);

  const dom = {
    canvas: $("canvas"),
    stage: $("stage"),
    hud: $("hud"),
    empty: $("empty"),
    toast: $("toast"),
    file: $("file"),
    bspFile: $("bspfile"),
    sample: $("sample"),
    play: $("play"),
    timeline: $("timeline"),
    clock: $("clock"),
    speed: $("speed"),
    view: $("view"),
    pov: $("pov"),
    trails: $("trails"),
    cones: $("cones"),
    roster: $("roster"),
    events: $("events"),
    fileLabel: $("file-label"),
    meta: {
      map: $("meta-map"),
      level: $("meta-level"),
      server: $("meta-server"),
      created: $("meta-created"),
      format: $("meta-format"),
      configstrings: $("meta-configstrings"),
      bsp: $("meta-bsp"),
      frames: $("meta-frames"),
      duration: $("meta-duration"),
    },
  };

  const ctx = dom.canvas.getContext("2d");
  const state = {
    demo: null,
    label: "",
    bsp: null,
    bspName: "",
    freeCam: null,
    groupIndex: 0,
    timeMs: 0,
    playing: false,
    speed: 1,
    viewMode: "top",
    pov: null,
    showTrails: true,
    showCones: true,
    lastTs: 0,
    width: 0,
    height: 0,
  };

  const LIGHT = (() => {
    const v = [0.45, 0.72, 0.53];
    const n = Math.hypot(v[0], v[1], v[2]);
    return [v[0] / n, v[1] / n, v[2] / n];
  })();

  // ---------------------------------------------------------------------
  // Small utilities.
  // ---------------------------------------------------------------------
  function fmtTime(ms) {
    const total = Math.max(0, ms) / 1000;
    const minutes = Math.floor(total / 60);
    const seconds = total - minutes * 60;
    return minutes > 0
      ? `${minutes}:${seconds.toFixed(1).padStart(4, "0")}`
      : `${seconds.toFixed(1)}s`;
  }

  function fmtCreated(unix) {
    if (!unix) return "—";
    try {
      return new Date(unix * 1000).toLocaleString();
    } catch {
      return String(unix);
    }
  }

  function slotColor(slot) {
    return COLORS[(Math.max(1, slot) - 1) % COLORS.length];
  }

  function hexToRgba(hex, alpha) {
    const h = hex.replace("#", "");
    const n = parseInt(h, 16);
    const r = (n >> 16) & 255;
    const g = (n >> 8) & 255;
    const b = n & 255;
    return `rgba(${r}, ${g}, ${b}, ${alpha})`;
  }

  function esc(value) {
    return String(value).replace(/[&<>"']/g, (c) => {
      switch (c) {
        case "&":
          return "&amp;";
        case "<":
          return "&lt;";
        case ">":
          return "&gt;";
        case '"':
          return "&quot;";
        default:
          return "&#39;";
      }
    });
  }

  function rosterFor(slot) {
    return state.demo ? state.demo.rosterBySlot.get(slot) : null;
  }

  function userinfoValue(userinfo, key) {
    if (!userinfo) return null;
    const needle = `\\${key.toLowerCase()}\\`;
    const start = userinfo.toLowerCase().indexOf(needle);
    if (start < 0) return null;
    const from = start + needle.length;
    const end = userinfo.indexOf("\\", from);
    const value = userinfo.slice(from, end < 0 ? userinfo.length : end);
    return value || null;
  }

  function displayName(slot) {
    const roster = rosterFor(slot);
    return roster && roster.name ? roster.name : `slot ${slot}`;
  }

  function currentGroup() {
    return state.demo ? state.demo.groups[state.groupIndex] : null;
  }

  function currentFrame() {
    const group = currentGroup();
    if (!group) return null;
    if (state.pov != null) return group.slots.get(state.pov) || null;
    return group.slots.values().next().value || null;
  }

  function setText(node, text) {
    if (node) node.textContent = text;
  }

  function showToast(message) {
    dom.toast.textContent = message;
    dom.toast.classList.add("show");
    clearTimeout(showToast.timer);
    showToast.timer = setTimeout(() => dom.toast.classList.remove("show"), 4500);
  }

  // ---------------------------------------------------------------------
  // Loading and side panels.
  // ---------------------------------------------------------------------
  function loadDemo(demo, label) {
    state.demo = demo;
    state.label = label;
    state.playing = false;
    state.speed = Number(dom.speed.value) || 1;
    state.pov = demo.slots.length ? demo.slots[0] : null;
    state.groupIndex = 0;
    state.timeMs = demo.firstTime;

    dom.empty.style.display = "none";
    dom.play.disabled = demo.groups.length === 0;
    dom.timeline.disabled = demo.groups.length === 0;
    dom.pov.disabled = demo.slots.length === 0;
    dom.timeline.max = String(Math.max(0, demo.groups.length - 1));
    dom.timeline.value = "0";
    updateViewLabel();

    populateMeta();
    populateRoster();
    populateEvents();
    populatePov();
    updatePlayButton();
    updateTimelineLabel();
    updateHud();
    resizeCanvas();
    draw();
  }

  function sceneBounds() {
    const d = state.demo ? state.demo.bounds : null;
    const m = state.bsp ? state.bsp.bounds : null;
    if (!d) return m || { minX: -512, minY: -512, maxX: 512, maxY: 512 };
    if (!m) return d;
    return {
      minX: Math.min(d.minX, m.minX),
      minY: Math.min(d.minY, m.minY),
      maxX: Math.max(d.maxX, m.maxX),
      maxY: Math.max(d.maxY, m.maxY),
    };
  }

  function updateBspMeta() {
    setText(
      dom.meta.bsp,
      state.bsp
        ? `${state.bspName} · ${state.bsp.stats.faces} faces · v${state.bsp.version}`
        : "—"
    );
  }

  function faceRenderColor(face) {
    const style = face.style;
    if (style.kind !== "solid" || style.hue == null) return style.color;
    const d = Math.abs(
      face.normal[0] * LIGHT[0] +
        face.normal[1] * LIGHT[1] +
        face.normal[2] * LIGHT[2]
    );
    let shade = 0.42 + 0.58 * d;
    if (face.light) {
      const lum =
        0.2126 * face.light[0] + 0.7152 * face.light[1] + 0.0722 * face.light[2];
      shade *= 0.55 + 0.75 * Math.min(1, lum);
    }
    const light = Math.max(7, Math.min(82, style.light * shade));
    return style.alpha < 1
      ? `hsla(${style.hue} ${style.sat}% ${light}% / ${style.alpha})`
      : `hsl(${style.hue} ${style.sat}% ${light}%)`;
  }

  function urlFreeCam() {
    const raw = new URLSearchParams(window.location.search).get("cam");
    if (!raw) return null;
    const values = raw.split(",").map((v) => Number(v));
    if (values.length !== 5 || values.some((v) => !Number.isFinite(v)))
      return null;
    return {
      eye: [values[0], values[1], values[2]],
      viewangles: [values[4], values[3], 0],
      fov: 90,
    };
  }

  function loadMap(buffer, name) {
    const map = window.SofBsp.parse(buffer);
    for (const face of map.faces) face.renderColor = faceRenderColor(face);
    state.bsp = map;
    state.bspName = String(name || "map.bsp").replace(/^.*[\\/]/, "");
    const view = map.intermission || map.spawn;
    if (view) {
      const b = map.bounds;
      const centerX = (b.minX + b.maxX) / 2;
      const centerY = (b.minY + b.maxY) / 2;
      const yaw =
        view.yaw != null
          ? view.yaw
          : (Math.atan2(centerY - view.origin[1], centerX - view.origin[0]) *
              180) /
            Math.PI;
      const eyeZ =
        view === map.intermission ? view.origin[2] : view.origin[2] + 22;
      state.freeCam = {
        eye: [view.origin[0], view.origin[1], eyeZ],
        viewangles: [0, yaw, 0],
        fov: 90,
      };
    } else {
      state.freeCam = null;
    }
    const camOverride = urlFreeCam();
    if (camOverride) state.freeCam = camOverride;
    dom.empty.style.display = "none";
    updateBspMeta();
    updateViewLabel();
    updateHud();
    draw();
  }

  function populateMeta() {
    const d = state.demo;
    const h = d.header;
    setText(dom.meta.map, h.mapName || "—");
    setText(dom.meta.level, h.levelName || "—");
    setText(dom.meta.server, h.serverName ? `${h.serverName} (${h.gamedir || "?"})` : "—");
    setText(dom.meta.created, fmtCreated(h.createdUnix));
    setText(
      dom.meta.format,
      `v${h.version} · ${h.tickMs} ms tick · entity ${h.flags & 1 ? "on" : "off"}`
    );
    setText(
      dom.meta.configstrings,
      d.configstringCount ? String(d.configstringCount) : "none"
    );
    setText(
      dom.meta.frames,
      `${d.clientFrames} client-frames / ${d.groups.length} ticks`
    );
    setText(
      dom.meta.duration,
      `${fmtTime(d.durationMs)} · ${d.slots.length} slot(s)`
    );
    updateBspMeta();
  }

  function populateRoster() {
    dom.roster.innerHTML = "";
    if (!state.demo.slots.length) {
      const empty = document.createElement("div");
      empty.className = "event-item";
      empty.textContent = "No roster entries.";
      dom.roster.appendChild(empty);
      return;
    }
    for (const slot of state.demo.slots) {
      const roster = rosterFor(slot);
      const button = document.createElement("button");
      button.type = "button";
      button.className = "roster-item";
      button.dataset.slot = String(slot);
      if (slot === state.pov) button.classList.add("active");

      const dot = document.createElement("span");
      dot.className = "dot";
      dot.style.background = slotColor(slot);
      button.appendChild(dot);

      const name = document.createElement("span");
      name.className = "roster-name";
      name.textContent = displayName(slot);
      button.appendChild(name);

      if (roster && roster.flags & S.CLIENT_FLAG.SPECTATOR) {
        const badge = document.createElement("span");
        badge.className = "badge";
        badge.textContent = "spec";
        button.appendChild(badge);
      }

      const slotText = document.createElement("span");
      slotText.className = "roster-slot";
      slotText.textContent = `#${slot}`;
      button.appendChild(slotText);

      button.addEventListener("click", () => selectPov(slot));
      dom.roster.appendChild(button);
    }
  }

  function populateEvents() {
    dom.events.innerHTML = "";
    if (!state.demo.events.length) {
      const empty = document.createElement("div");
      empty.className = "event-item";
      empty.textContent = "No marks in this recording.";
      dom.events.appendChild(empty);
      return;
    }
    for (const event of state.demo.events) {
      const button = document.createElement("button");
      button.type = "button";
      button.className = "event-item";
      const time = document.createElement("span");
      time.className = "event-time";
      time.textContent = fmtTime(event.timeMs - state.demo.firstTime);
      button.appendChild(time);
      const text = document.createElement("span");
      text.textContent = event.text || `event ${event.kind}`;
      button.appendChild(text);
      button.addEventListener("click", () => {
        const idx = S.findGroupIndex(state.demo.groups, event.timeMs);
        setGroupIndex(idx, { pause: true });
      });
      dom.events.appendChild(button);
    }
  }

  function populatePov() {
    dom.pov.innerHTML = "";
    if (!state.demo.slots.length) return;
    for (const slot of state.demo.slots) {
      const option = document.createElement("option");
      option.value = String(slot);
      option.textContent = `#${slot} ${displayName(slot)}`;
      dom.pov.appendChild(option);
    }
    if (state.pov != null) dom.pov.value = String(state.pov);
  }

  function selectPov(slot) {
    state.pov = slot;
    dom.pov.value = String(slot);
    for (const item of dom.roster.querySelectorAll(".roster-item")) {
      item.classList.toggle("active", Number(item.dataset.slot) === slot);
    }
    updateHud();
    draw();
  }

  function setGroupIndex(index, options) {
    if (!state.demo || !state.demo.groups.length) return;
    const clamped = Math.max(0, Math.min(state.demo.groups.length - 1, index));
    state.groupIndex = clamped;
    state.timeMs = state.demo.groups[clamped].timeMs;
    if (options && options.pause) state.playing = false;
    dom.timeline.value = String(clamped);
    updatePlayButton();
    updateTimelineLabel();
    updateHud();
    draw();
  }

  function updateTimelineLabel() {
    if (!state.demo) {
      dom.clock.textContent = "0.0s";
      return;
    }
    dom.clock.textContent = `${fmtTime(state.timeMs)} / ${fmtTime(
      state.demo.lastTime
    )}`;
  }

  function updatePlayButton() {
    dom.play.textContent = state.playing ? "Pause" : "Play";
  }

  function updateViewLabel() {
    const suffix = state.viewMode === "fp" ? "first-person" : "top-down";
    if (!state.demo) {
      dom.fileLabel.textContent = state.bsp
        ? `${state.bspName} · map preview · ${suffix}`
        : "state demo viewer";
      return;
    }
    dom.fileLabel.textContent = `${state.label || "recording"} · ${suffix}`;
  }

  function updateHud() {
    if (!state.demo) {
      if (!state.bsp) {
        dom.hud.innerHTML = "";
        return;
      }
      dom.hud.innerHTML = `
        <div class="hud-card">
          <h3>${esc(state.bspName)}</h3>
          <div class="hud-line"><span>Map preview</span><span>${state.bsp.stats.faces} faces</span></div>
          <div class="hud-line"><span>Mode</span><span>${state.viewMode === "fp" ? "free camera" : "top-down"}</span></div>
        </div>`;
      return;
    }
    const group = currentGroup();
    const frame = currentFrame();
    const name = state.pov != null ? displayName(state.pov) : "—";
    const left = `
      <div class="hud-card">
        <h3>${esc(state.demo.header.mapName || "recording")}</h3>
        <div class="hud-line"><span>Time</span><span>${fmtTime(state.timeMs)}</span></div>
        <div class="hud-line"><span>Frame</span><span>${group ? group.frame : "—"}</span></div>
        <div class="hud-line"><span>POV</span><span>${esc(name)}</span></div>
      </div>`;
    let right;
    if (!frame) {
      right = `
        <div class="hud-card">
          <h3>${esc(name)}</h3>
          <div class="hud-line"><span>No frame for this slot at this tick.</span></div>
        </div>`;
    } else {
      const st = frame.ps.stats;
      const roster = state.demo.rosterBySlot.get(frame.slot);
      let skin =
        frame.slot >= 1 && frame.slot <= 32
          ? state.demo.configstrings.get(S.CS.PLAYERSKINS + frame.slot - 1)
          : null;
      if (!skin && roster) {
        const model = userinfoValue(roster.userinfo, "model");
        const skinName = userinfoValue(roster.userinfo, "skin");
        if (model && skinName) skin = `${model}/${skinName}`;
      }
      const model =
        frame.es && frame.es.modelindex
          ? state.demo.configstrings.get(S.CS.MODELS + frame.es.modelindex)
          : null;
      const extra = [
        skin
          ? `<div class="hud-line"><span>Skin</span><span class="path">${esc(skin)}</span></div>`
          : "",
        model
          ? `<div class="hud-line"><span>Model</span><span class="path">${esc(model)}</span></div>`
          : "",
      ].join("");
      right = `
        <div class="hud-card">
          <h3>${esc(name)}${frame.flags & S.CLIENT_FLAG.SPECTATOR ? " · spectator" : ""}</h3>
          <div class="hud-stats">
            <span class="label">Health</span><span class="value">${st[S.STAT.HEALTH] || 0}</span>
            <span class="label">Armor</span><span class="value">${st[S.STAT.ARMOR] || 0}</span>
            <span class="label">Frags</span><span class="value">${st[S.STAT.FRAGS] || 0}</span>
            <span class="label">Clip</span><span class="value">${st[S.STAT.CLIP_AMMO] || 0}</span>
            <span class="label">Ammo</span><span class="value">${st[S.STAT.AMMO] || 0}</span>
            <span class="label">FOV</span><span class="value">${Math.round(frame.ps.fov || 0)}</span>
          </div>
          ${extra}
        </div>`;
    }
    dom.hud.innerHTML = left + right;
  }

  // ---------------------------------------------------------------------
  // Canvas.
  // ---------------------------------------------------------------------
  function resizeCanvas() {
    const dpr = window.devicePixelRatio || 1;
    const width = Math.max(1, dom.stage.clientWidth);
    const height = Math.max(1, dom.stage.clientHeight);
    state.width = width;
    state.height = height;
    dom.canvas.width = Math.max(1, Math.floor(width * dpr));
    dom.canvas.height = Math.max(1, Math.floor(height * dpr));
    ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
  }

  function transformFor(bounds, width, height) {
    const b = bounds;
    const pad = 34;
    const bw = Math.max(1, b.maxX - b.minX);
    const bh = Math.max(1, b.maxY - b.minY);
    const scale = Math.min(
      (width - pad * 2) / bw || 1,
      (height - pad * 2) / bh || 1
    );
    const usedW = bw * scale;
    const usedH = bh * scale;
    const ox = (width - usedW) / 2;
    const oy = (height - usedH) / 2;
    return {
      width,
      height,
      pad,
      minX: b.minX,
      minY: b.minY,
      scale,
      ox,
      oy,
      toScreen(x, y) {
        return [ox + (x - b.minX) * scale, height - oy - (y - b.minY) * scale];
      },
    };
  }

  function drawGrid(t, bounds) {
    const b = bounds;
    const steps = [32, 64, 128, 256, 512, 1024, 2048, 4096];
    let step = steps[steps.length - 1];
    for (const candidate of steps) {
      if (candidate * t.scale >= 46) {
        step = candidate;
        break;
      }
    }

    ctx.save();
    ctx.strokeStyle = "rgba(255, 255, 255, 0.045)";
    ctx.lineWidth = 1;
    ctx.beginPath();
    const startX = Math.ceil(b.minX / step) * step;
    for (let x = startX; x <= b.maxX; x += step) {
      const [sx, sy0] = t.toScreen(x, b.minY);
      const [, sy1] = t.toScreen(x, b.maxY);
      ctx.moveTo(sx, sy0);
      ctx.lineTo(sx, sy1);
    }
    const startY = Math.ceil(b.minY / step) * step;
    for (let y = startY; y <= b.maxY; y += step) {
      const [sx0, sy] = t.toScreen(b.minX, y);
      const [sx1] = t.toScreen(b.maxX, y);
      ctx.moveTo(sx0, sy);
      ctx.lineTo(sx1, sy);
    }
    ctx.stroke();

    ctx.fillStyle = "rgba(139, 151, 173, 0.55)";
    ctx.font = "10px ui-monospace, SFMono-Regular, Menlo, monospace";
    ctx.textAlign = "left";
    ctx.textBaseline = "alphabetic";
    for (let x = startX; x <= b.maxX; x += step) {
      const [sx] = t.toScreen(x, b.minY);
      ctx.fillText(String(Math.round(x)), sx + 3, t.height - 6);
    }
    ctx.textAlign = "right";
    ctx.textBaseline = "middle";
    for (let y = startY; y <= b.maxY; y += step) {
      const [, sy] = t.toScreen(b.minX, y);
      ctx.fillText(String(Math.round(y)), 30, sy - 3);
    }

    ctx.strokeStyle = "rgba(255, 255, 255, 0.09)";
    ctx.strokeRect(t.ox, t.oy, t.width - t.ox * 2, t.height - t.oy * 2);
    ctx.restore();
  }

  function drawMapTopDown(t) {
    if (!state.bsp) return;
    ctx.save();
    ctx.strokeStyle = "rgba(160, 180, 210, 0.09)";
    ctx.lineWidth = 1;
    ctx.beginPath();
    for (const face of state.bsp.faces) {
      if (face.style.kind === "sky" || face.style.kind === "nodraw") continue;
      const verts = face.vertices;
      if (verts.length < 3) continue;
      const first = t.toScreen(verts[0][0], verts[0][1]);
      ctx.moveTo(first[0], first[1]);
      for (let i = 1; i < verts.length; i++) {
        const p = t.toScreen(verts[i][0], verts[i][1]);
        ctx.lineTo(p[0], p[1]);
      }
      ctx.closePath();
    }
    ctx.stroke();
    ctx.restore();
  }

  function drawTrails(t) {
    const demo = state.demo;
    const cutoff = state.timeMs - TRAIL_WINDOW_MS;
    for (const slot of demo.slots) {
      const points = demo.slotTrails[slot] || [];
      const visible = [];
      for (const p of points) {
        if (p.timeMs < cutoff) continue;
        if (p.timeMs > state.timeMs) break;
        visible.push(p);
      }
      if (visible.length < 2) continue;
      const stride = Math.max(1, Math.ceil(visible.length / MAX_TRAIL_POINTS));
      const color = slotColor(slot);
      ctx.save();
      ctx.strokeStyle = hexToRgba(color, slot === state.pov ? 0.6 : 0.28);
      ctx.lineWidth = slot === state.pov ? 2 : 1.25;
      ctx.beginPath();
      for (let i = 0; i < visible.length; i += stride) {
        const [sx, sy] = t.toScreen(visible[i].x, visible[i].y);
        if (i === 0) ctx.moveTo(sx, sy);
        else ctx.lineTo(sx, sy);
      }
      ctx.stroke();
      ctx.restore();
    }
  }

  function drawPlayer(t, frame, selected) {
    const slot = frame.slot;
    const color = slotColor(slot);
    const [sx, sy] = t.toScreen(
      frame.ps.pmove.origin[0],
      frame.ps.pmove.origin[1]
    );
    const yaw = ((frame.ps.viewangles[1] || 0) * Math.PI) / 180;
    const dirX = Math.cos(yaw);
    const dirY = -Math.sin(yaw);
    const lineLength = selected ? 48 : 34;

    ctx.save();

    if (frame.es) {
      const [bx, by] = t.toScreen(frame.es.origin[0], frame.es.origin[1]);
      const dx = bx - sx;
      const dy = by - sy;
      if (Math.hypot(dx, dy) > 1.5) {
        ctx.beginPath();
        ctx.arc(bx, by, 3, 0, Math.PI * 2);
        ctx.strokeStyle = hexToRgba(color, 0.35);
        ctx.lineWidth = 1;
        ctx.stroke();
      }
    }

    ctx.beginPath();
    ctx.moveTo(sx, sy);
    ctx.lineTo(sx + dirX * lineLength, sy + dirY * lineLength);
    ctx.strokeStyle = hexToRgba(color, selected ? 0.95 : 0.55);
    ctx.lineWidth = selected ? 2 : 1.25;
    ctx.stroke();

    ctx.beginPath();
    ctx.arc(sx, sy, selected ? 8 : 5.5, 0, Math.PI * 2);
    ctx.fillStyle = color;
    ctx.fill();
    ctx.strokeStyle = selected ? "#ffffff" : "rgba(0, 0, 0, 0.65)";
    ctx.lineWidth = selected ? 2 : 1;
    ctx.stroke();

    if (frame.flags & S.CLIENT_FLAG.SPECTATOR) {
      ctx.beginPath();
      ctx.setLineDash([3, 3]);
      ctx.arc(sx, sy, selected ? 13 : 10, 0, Math.PI * 2);
      ctx.strokeStyle = hexToRgba(color, 0.7);
      ctx.lineWidth = 1;
      ctx.stroke();
      ctx.setLineDash([]);
    }

    const name = displayName(slot);
    ctx.font = selected
      ? "600 12px ui-sans-serif, system-ui, sans-serif"
      : "11px ui-sans-serif, system-ui, sans-serif";
    ctx.textAlign = "left";
    ctx.textBaseline = "middle";
    ctx.lineWidth = 3;
    ctx.strokeStyle = "rgba(0, 0, 0, 0.8)";
    ctx.strokeText(name, sx + 11, sy - 10);
    ctx.fillStyle = selected ? "#ffffff" : color;
    ctx.fillText(name, sx + 11, sy - 10);

    ctx.restore();
  }

  function lerp(a, b, t) {
    return a + (b - a) * t;
  }

  function lerpVec(a, b, t) {
    return [lerp(a[0], b[0], t), lerp(a[1], b[1], t), lerp(a[2], b[2], t)];
  }

  function lerpAngle(a, b, t) {
    let delta = ((b - a + 540) % 360) - 180;
    return a + delta * t;
  }

  function lerpAngles(a, b, t) {
    return [lerpAngle(a[0], b[0], t), lerpAngle(a[1], b[1], t), lerpAngle(a[2], b[2], t)];
  }

  function interpolateFrame(f0, f1, alpha) {
    const ps = Object.assign({}, f0.ps);
    ps.pmove = Object.assign({}, f0.ps.pmove, {
      origin: lerpVec(f0.ps.pmove.origin, f1.ps.pmove.origin, alpha),
    });
    ps.viewangles = lerpAngles(f0.ps.viewangles, f1.ps.viewangles, alpha);
    ps.viewoffset = lerpVec(f0.ps.viewoffset, f1.ps.viewoffset, alpha);
    ps.origin = ps.pmove.origin;
    ps.eye = [
      ps.pmove.origin[0] + ps.viewoffset[0],
      ps.pmove.origin[1] + ps.viewoffset[1],
      ps.pmove.origin[2] + ps.viewoffset[2],
    ];
    const out = Object.assign({}, f0, { ps });
    if (f0.es && f1.es) {
      out.es = Object.assign({}, f0.es, {
        origin: lerpVec(f0.es.origin, f1.es.origin, alpha),
        angles: lerpAngles(f0.es.angles, f1.es.angles, alpha),
      });
    }
    return out;
  }

  function framesForCurrentGroup(group) {
    const next = state.demo.groups[state.groupIndex + 1];
    const alpha =
      next && next.timeMs > group.timeMs
        ? Math.max(
            0,
            Math.min(1, (state.timeMs - group.timeMs) / (next.timeMs - group.timeMs))
          )
        : 0;
    const frames = new Map();
    for (const slot of group.slots.keys()) {
      const f0 = group.slots.get(slot);
      const f1 = next ? next.slots.get(slot) : null;
      frames.set(slot, f1 && alpha > 0 ? interpolateFrame(f0, f1, alpha) : f0);
    }
    return frames;
  }

  function drawCone(t, frame) {
    if (!frame) return;
    const color = slotColor(state.pov);
    const [sx, sy] = t.toScreen(
      frame.ps.pmove.origin[0],
      frame.ps.pmove.origin[1]
    );
    const yaw = ((frame.ps.viewangles[1] || 0) * Math.PI) / 180;
    const screenAngle = -yaw;
    const fov = Math.min(
      Math.PI * 1.8,
      Math.max(Math.PI / 12, ((frame.ps.fov || 90) * Math.PI) / 180)
    );
    const radius = Math.min(260, Math.max(60, 220 * t.scale));
    ctx.save();
    ctx.beginPath();
    ctx.moveTo(sx, sy);
    ctx.arc(sx, sy, radius, screenAngle - fov / 2, screenAngle + fov / 2);
    ctx.closePath();
    ctx.fillStyle = hexToRgba(color, 0.08);
    ctx.fill();
    ctx.strokeStyle = hexToRgba(color, 0.22);
    ctx.lineWidth = 1;
    ctx.stroke();
    ctx.restore();
  }

  function drawPlayers(t, group) {
    const frames = framesForCurrentGroup(group);
    const slots = Array.from(frames.keys()).sort((a, b) => a - b);
    const selected = slots.filter((slot) => slot === state.pov);
    const others = slots.filter((slot) => slot !== state.pov);
    for (const slot of others) drawPlayer(t, frames.get(slot), false);
    for (const slot of selected) drawPlayer(t, frames.get(slot), true);
    if (state.showCones && state.pov != null)
      drawCone(t, frames.get(state.pov) || null);
  }

  function cameraBasis(frame) {
    const deg = Math.PI / 180;
    const pitch = (frame.ps.viewangles[0] || 0) * deg;
    const yaw = (frame.ps.viewangles[1] || 0) * deg;
    const roll = (frame.ps.viewangles[2] || 0) * deg;
    const cp = Math.cos(pitch);
    const sp = Math.sin(pitch);
    const cy = Math.cos(yaw);
    const sy = Math.sin(yaw);
    const cr = Math.cos(roll);
    const sr = Math.sin(roll);
    return {
      eye: frame.ps.eye || [
        frame.ps.pmove.origin[0] + frame.ps.viewoffset[0],
        frame.ps.pmove.origin[1] + frame.ps.viewoffset[1],
        frame.ps.pmove.origin[2] + frame.ps.viewoffset[2],
      ],
      forward: [cp * cy, cp * sy, -sp],
      right: [-sr * sp * cy + cr * sy, -sr * sp * sy - cr * cy, -sr * cp],
      up: [cr * sp * cy + sr * sy, cr * sp * sy - sr * cy, cr * cp],
    };
  }

  function cameraProjector(basis, fovDeg, width, height) {
    const deg = Math.PI / 180;
    const fov = Math.min(170, Math.max(30, fovDeg)) * deg;
    const focal = width / 2 / Math.tan(fov / 2);
    const near = 4;
    return {
      focal,
      near,
      width,
      height,
      toCamera(p) {
        const dx = p[0] - basis.eye[0];
        const dy = p[1] - basis.eye[1];
        const dz = p[2] - basis.eye[2];
        return [
          dx * basis.right[0] + dy * basis.right[1] + dz * basis.right[2],
          dx * basis.up[0] + dy * basis.up[1] + dz * basis.up[2],
          dx * basis.forward[0] + dy * basis.forward[1] + dz * basis.forward[2],
        ];
      },
      project(c) {
        return [
          width / 2 + (c[0] / c[2]) * focal,
          height / 2 - (c[1] / c[2]) * focal,
        ];
      },
    };
  }

  function clipPolygonNear(poly, near) {
    const out = [];
    for (let i = 0; i < poly.length; i++) {
      const a = poly[i];
      const b = poly[(i + 1) % poly.length];
      const aIn = a[2] > near;
      const bIn = b[2] > near;
      if (aIn) out.push(a);
      if (aIn !== bIn) {
        const t = (near - a[2]) / (b[2] - a[2]);
        out.push([
          a[0] + (b[0] - a[0]) * t,
          a[1] + (b[1] - a[1]) * t,
          near,
        ]);
      }
    }
    return out;
  }

  function drawBspFirstPerson(proj) {
    const map = state.bsp;
    if (!map) return;
    const visible = [];
    for (const face of map.faces) {
      if (face.style.kind === "sky" || face.style.kind === "nodraw") continue;
      const camVerts = new Array(face.vertices.length);
      let anyInFront = false;
      for (let i = 0; i < face.vertices.length; i++) {
        const c = proj.toCamera(face.vertices[i]);
        camVerts[i] = c;
        if (c[2] > proj.near) anyInFront = true;
      }
      if (!anyInFront) continue;
      const clipped = clipPolygonNear(camVerts, proj.near);
      if (clipped.length < 3) continue;
      const points = clipped.map((c) => proj.project(c));
      let minX = Infinity;
      let minY = Infinity;
      let maxX = -Infinity;
      let maxY = -Infinity;
      let depth = 0;
      for (let i = 0; i < clipped.length; i++) {
        depth += clipped[i][2];
        const p = points[i];
        if (p[0] < minX) minX = p[0];
        if (p[1] < minY) minY = p[1];
        if (p[0] > maxX) maxX = p[0];
        if (p[1] > maxY) maxY = p[1];
      }
      if (maxX < 0 || minX > proj.width || maxY < 0 || minY > proj.height)
        continue;
      visible.push({
        depth: depth / clipped.length,
        points,
        color: face.renderColor,
      });
    }

    visible.sort((a, b) => b.depth - a.depth);
    for (const face of visible) {
      const p = face.points;
      ctx.beginPath();
      ctx.moveTo(p[0][0], p[0][1]);
      for (let i = 1; i < p.length; i++) ctx.lineTo(p[i][0], p[i][1]);
      ctx.closePath();
      ctx.fillStyle = face.color;
      ctx.fill();
      if (visible.length < 3000) {
        ctx.strokeStyle = "rgba(0, 0, 0, 0.14)";
        ctx.lineWidth = 0.5;
        ctx.stroke();
      }
    }
  }

  function drawFirstPerson() {
    const group = currentGroup();
    const frames = group ? framesForCurrentGroup(group) : null;
    let frame = frames && state.pov != null ? frames.get(state.pov) : null;
    if (!frame && state.freeCam) {
      frame = {
        slot: 0,
        flags: 0,
        ps: {
          eye: state.freeCam.eye,
          viewangles: state.freeCam.viewangles,
          fov: state.freeCam.fov,
          stats: new Array(16).fill(0),
        },
      };
    }
    if (!frame) {
      ctx.fillStyle = "#0f121a";
      ctx.fillRect(0, 0, state.width, state.height);
      ctx.fillStyle = "rgba(139, 151, 173, 0.9)";
      ctx.font = "14px ui-sans-serif, system-ui, sans-serif";
      ctx.textAlign = "center";
      ctx.textBaseline = "middle";
      ctx.fillText(
        state.bsp
          ? "No spawn point or POV frame for this map"
          : "No frame for the selected POV at this tick",
        state.width / 2,
        state.height / 2
      );
      return;
    }

    const width = state.width;
    const height = state.height;
    const basis = cameraBasis(frame);
    const proj = cameraProjector(
      basis,
      frame.ps.fov || 90,
      width,
      height
    );
    const { focal, near, toCamera, project } = proj;

    const sky = ctx.createLinearGradient(0, 0, 0, height);
    sky.addColorStop(0, "#090c12");
    sky.addColorStop(0.5, "#0f1420");
    sky.addColorStop(1, "#07090d");
    ctx.fillStyle = sky;
    ctx.fillRect(0, 0, width, height);

    if (state.bsp) {
      drawBspFirstPerson(proj);
    }

    const clipSegment = (a, b) => {
      const aIn = a[2] > near;
      const bIn = b[2] > near;
      if (!aIn && !bIn) return null;
      if (aIn && bIn) return [a, b];
      const t = (near - a[2]) / (b[2] - a[2]);
      const p = [
        a[0] + (b[0] - a[0]) * t,
        a[1] + (b[1] - a[1]) * t,
        near,
      ];
      return aIn ? [a, p] : [p, b];
    };

    const grid = (ctx, x0, y0, z0, x1, y1, z1) => {
      const clipped = clipSegment(
        toCamera([x0, y0, z0]),
        toCamera([x1, y1, z1])
      );
      if (!clipped) return;
      const a = project(clipped[0]);
      const b = project(clipped[1]);
      ctx.moveTo(a[0], a[1]);
      ctx.lineTo(b[0], b[1]);
    };

    if (!state.bsp) {
      const step = 128;
      const range = 4096;
      const minX = Math.floor((basis.eye[0] - range) / step) * step;
      const maxX = Math.ceil((basis.eye[0] + range) / step) * step;
      const minY = Math.floor((basis.eye[1] - range) / step) * step;
      const maxY = Math.ceil((basis.eye[1] + range) / step) * step;

      ctx.save();
      ctx.strokeStyle = "rgba(255, 255, 255, 0.065)";
      ctx.lineWidth = 1;
      ctx.beginPath();
      for (let x = minX; x <= maxX; x += step)
        grid(ctx, x, minY, 0, x, maxY, 0);
      for (let y = minY; y <= maxY; y += step)
        grid(ctx, minX, y, 0, maxX, y, 0);
      ctx.stroke();
      ctx.restore();
    }

    const players = [];
    if (frames) {
      for (const [slot, f] of frames) {
        if (slot === state.pov) continue;
        const base = f.es ? f.es.origin : f.ps.pmove.origin;
        const head = [base[0], base[1], base[2] + 56];
        const cBase = toCamera(base);
        const cHead = toCamera(head);
        if (cBase[2] <= near || cHead[2] <= near) continue;
        players.push({ slot, cBase, cHead, z: cBase[2] });
      }
    }
    players.sort((a, b) => b.z - a.z);

    for (const p of players) {
      const base = project(p.cBase);
      const head = project(p.cHead);
      const color = slotColor(p.slot);
      const bodyWidth = Math.max(2, Math.min(10, 220 / Math.max(1, p.z)));
      ctx.save();
      ctx.strokeStyle = hexToRgba(color, 0.85);
      ctx.lineWidth = bodyWidth;
      ctx.beginPath();
      ctx.moveTo(base[0], base[1]);
      ctx.lineTo(head[0], head[1]);
      ctx.stroke();

      const radius = Math.max(2, Math.min(8, bodyWidth * 0.7));
      ctx.beginPath();
      ctx.arc(head[0], head[1], radius, 0, Math.PI * 2);
      ctx.fillStyle = color;
      ctx.fill();
      ctx.strokeStyle = "rgba(0, 0, 0, 0.6)";
      ctx.lineWidth = 1;
      ctx.stroke();

      const name = displayName(p.slot);
      ctx.font = "11px ui-sans-serif, system-ui, sans-serif";
      ctx.textAlign = "left";
      ctx.textBaseline = "middle";
      ctx.lineWidth = 3;
      ctx.strokeStyle = "rgba(0, 0, 0, 0.85)";
      ctx.strokeText(name, head[0] + radius + 5, head[1] - 2);
      ctx.fillStyle = color;
      ctx.fillText(name, head[0] + radius + 5, head[1] - 2);
      ctx.restore();
    }

    const cx = width / 2;
    const cy = height / 2;
    ctx.save();
    ctx.strokeStyle = "rgba(255, 255, 255, 0.35)";
    ctx.lineWidth = 1;
    ctx.beginPath();
    ctx.moveTo(cx - 9, cy);
    ctx.lineTo(cx - 3, cy);
    ctx.moveTo(cx + 3, cy);
    ctx.lineTo(cx + 9, cy);
    ctx.moveTo(cx, cy - 9);
    ctx.lineTo(cx, cy - 3);
    ctx.moveTo(cx, cy + 3);
    ctx.lineTo(cx, cy + 9);
    ctx.stroke();
    ctx.restore();
  }

  function draw() {
    if (!ctx) return;
    const width = state.width;
    const height = state.height;
    ctx.clearRect(0, 0, width, height);
    if (!state.demo && !state.bsp) return;

    if (state.viewMode === "fp") {
      drawFirstPerson();
      return;
    }

    ctx.fillStyle = "#0f121a";
    ctx.fillRect(0, 0, width, height);

    const bounds = sceneBounds();
    const t = transformFor(bounds, width, height);
    drawGrid(t, bounds);
    if (state.bsp) drawMapTopDown(t);
    if (state.demo && state.showTrails) drawTrails(t);
    const group = currentGroup();
    if (group) drawPlayers(t, group);
  }

  // ---------------------------------------------------------------------
  // Playback.
  // ---------------------------------------------------------------------
  function tick(timestamp) {
    if (!state.lastTs) state.lastTs = timestamp;
    const dt = Math.min(250, timestamp - state.lastTs);
    state.lastTs = timestamp;

    if (state.demo && state.playing) {
      let next = state.timeMs + dt * state.speed;
      if (next > state.demo.lastTime) next = state.demo.firstTime;
      state.timeMs = next;
      const index = S.findGroupIndex(state.demo.groups, next);
      if (index !== state.groupIndex) {
        state.groupIndex = index;
        dom.timeline.value = String(index);
        updateHud();
      }
      updateTimelineLabel();
      draw();
    }
    requestAnimationFrame(tick);
  }

  function togglePlay() {
    if (!state.demo || !state.demo.groups.length) return;
    if (!state.playing && state.timeMs >= state.demo.lastTime) {
      setGroupIndex(0, { pause: false });
    }
    state.playing = !state.playing;
    updatePlayButton();
  }

  function step(delta) {
    if (!state.demo) return;
    setGroupIndex(state.groupIndex + delta, { pause: true });
  }

  // ---------------------------------------------------------------------
  // Input.
  // ---------------------------------------------------------------------
  async function loadFile(file) {
    if (!file) return;
    try {
      const buffer = await file.arrayBuffer();
      const demo = S.parse(buffer);
      loadDemo(demo, file.name);
    } catch (error) {
      showToast(`Could not read ${file.name}: ${error.message || error}`);
      console.error(error);
    }
  }

  async function loadBspFile(file) {
    if (!file) return;
    try {
      const buffer = await file.arrayBuffer();
      loadMap(buffer, file.name);
    } catch (error) {
      showToast(`Could not read ${file.name}: ${error.message || error}`);
      console.error(error);
    }
  }

  dom.file.addEventListener("change", () => {
    if (dom.file.files && dom.file.files[0]) loadFile(dom.file.files[0]);
    dom.file.value = "";
  });

  dom.bspFile.addEventListener("change", () => {
    if (dom.bspFile.files && dom.bspFile.files[0])
      loadBspFile(dom.bspFile.files[0]);
    dom.bspFile.value = "";
  });

  function loadSample() {
    try {
      const demo = S.parse(S.buildSample());
      loadDemo(demo, "synthetic sample");
    } catch (error) {
      showToast(`Sample failed: ${error.message || error}`);
      console.error(error);
    }
  }

  dom.sample.addEventListener("click", loadSample);

  dom.play.addEventListener("click", togglePlay);

  dom.timeline.addEventListener("input", () => {
    const index = Number(dom.timeline.value) || 0;
    setGroupIndex(index, { pause: true });
  });

  dom.speed.addEventListener("change", () => {
    state.speed = Number(dom.speed.value) || 1;
  });

  dom.view.addEventListener("change", () => {
    state.viewMode = dom.view.value === "fp" ? "fp" : "top";
    dom.trails.disabled = state.viewMode === "fp";
    dom.cones.disabled = state.viewMode === "fp";
    updateViewLabel();
    draw();
  });

  dom.pov.addEventListener("change", () => {
    selectPov(Number(dom.pov.value));
  });

  dom.trails.addEventListener("change", () => {
    state.showTrails = dom.trails.checked;
    draw();
  });

  dom.cones.addEventListener("change", () => {
    state.showCones = dom.cones.checked;
    draw();
  });

  window.addEventListener("keydown", (event) => {
    if (!state.demo) return;
    const tag = (event.target && event.target.tagName) || "";
    if (tag === "INPUT" || tag === "SELECT" || tag === "TEXTAREA") return;
    if (event.code === "Space") {
      event.preventDefault();
      togglePlay();
    } else if (event.code === "ArrowLeft") {
      event.preventDefault();
      step(-1);
    } else if (event.code === "ArrowRight") {
      event.preventDefault();
      step(1);
    }
  });

  window.addEventListener("dragover", (event) => {
    event.preventDefault();
  });

  window.addEventListener("drop", (event) => {
    event.preventDefault();
    const files = event.dataTransfer ? Array.from(event.dataTransfer.files) : [];
    for (const file of files) {
      if (/\.bsp$/i.test(file.name)) loadBspFile(file);
      else if (/\.sbd$/i.test(file.name)) loadFile(file);
    }
  });

  const resizeObserver = new ResizeObserver(() => {
    resizeCanvas();
    draw();
  });
  resizeObserver.observe(dom.stage);
  window.addEventListener("resize", () => {
    resizeCanvas();
    draw();
  });

  resizeCanvas();
  draw();
  requestAnimationFrame(tick);

  // `?sample=1&t=6000&play=1` makes screenshots and quick demos one URL away.
  const params = new URLSearchParams(window.location.search);
  if (params.get("view") === "fp") {
    dom.view.value = "fp";
    state.viewMode = "fp";
    dom.trails.disabled = true;
    dom.cones.disabled = true;
  }
  const bspParam = params.get("bsp");
  if (bspParam) {
    fetch(bspParam)
      .then((response) => {
        if (!response.ok) throw new Error(`HTTP ${response.status}`);
        return response.arrayBuffer();
      })
      .then((buffer) => loadMap(buffer, bspParam.split("/").pop()))
      .catch((error) => {
        showToast(`Could not load BSP ${bspParam}: ${error.message || error}`);
        console.error(error);
      });
  }
  if (params.has("sample")) {
    loadSample();
    const seek = Number(params.get("t"));
    if (state.demo && Number.isFinite(seek)) {
      setGroupIndex(S.findGroupIndex(state.demo.groups, seek), {
        pause: !params.has("play"),
      });
    }
    if (state.demo && params.has("play")) {
      state.playing = true;
      updatePlayButton();
    }
  }
})();

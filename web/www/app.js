/* CANGO page. Each tab filters locally. Do not send set_filter on WiFi:
 * the board has one stream filter, and a second tab would starve the first. */

const params = new URLSearchParams(location.search);
const selftest = params.get("selftest") === "1";
let ws = null;
let tabs = [];
let active = 0;
let armed = false;
let profileBound = false;
let sentFilter = 0;
let captureSave = null;
let gotHello = false;
let holdArmedUntil = 0;

function blankFilter() {
  return { active: false, id: 0, mask: 0x7ff, extended: "any", changeOnly: false, minIntervalMs: 0 };
}

function makeTab() {
  return { title: "Generic", filter: blankFilter(), state: new Map(), rows: [] };
}

function send(obj) {
  if (obj.op === "set_filter") {
    sentFilter += 1;
  }
  if (ws && ws.readyState === WebSocket.OPEN) {
    ws.send(JSON.stringify(obj));
  }
}

function note(text) {
  document.querySelector("#op-note").textContent = text || "";
}

function parseHex(selector, fallback) {
  const raw = document.querySelector(selector).value.trim();
  if (!raw) {
    return fallback;
  }
  const n = Number.parseInt(raw, 16);
  return Number.isFinite(n) ? n : fallback;
}

function num(selector) {
  const n = Number(document.querySelector(selector).value);
  return Number.isFinite(n) ? n : 0;
}

function padData(bytes) {
  const out = new Uint8Array(8);
  out.set(bytes.subarray(0, 8));
  return out;
}

function sameData(a, b) {
  for (let i = 0; i < 8; i++) {
    if (a[i] !== b[i]) {
      return false;
    }
  }
  return true;
}

function acceptFrame(tab, frame) {
  const f = tab.filter;
  if (f.active) {
    const ext = (frame.flags & 0x01) !== 0;
    if (f.extended === "std" && ext) {
      return;
    }
    if (f.extended === "ext" && !ext) {
      return;
    }
    if ((frame.id & f.mask) !== (f.id & f.mask)) {
      return;
    }
    if (f.changeOnly || f.minIntervalMs > 0) {
      const key = String(frame.id) + ":" + (ext ? "e" : "s");
      let slot = tab.state.get(key);
      const data = padData(frame.data);
      if (!slot) {
        slot = { data: null, passed: false, last: 0 };
        tab.state.set(key, slot);
      }
      const same = slot.data !== null && sameData(slot.data, data);
      slot.data = data;
      if (f.changeOnly && same) {
        return;
      }
      if (f.minIntervalMs > 0 && slot.passed) {
        const dt = (frame.tUs - slot.last) >>> 0;
        if (dt < f.minIntervalMs * 1000) {
          return;
        }
      }
      slot.passed = true;
      slot.last = frame.tUs;
    }
  }
  tab.rows.push(frame);
  if (tab.rows.length > 80) {
    tab.rows.shift();
  }
}

function readFilterForm() {
  return {
    active: document.querySelector("#filter-active").checked,
    id: parseHex("#filter-id", 0),
    mask: parseHex("#filter-mask", 0x7ff),
    extended: document.querySelector("#filter-ext").value,
    changeOnly: document.querySelector("#filter-change").checked,
    minIntervalMs: num("#filter-interval"),
  };
}

function writeFilterForm(f) {
  document.querySelector("#filter-active").checked = f.active;
  document.querySelector("#filter-id").value = f.id.toString(16);
  document.querySelector("#filter-mask").value = f.mask.toString(16);
  document.querySelector("#filter-ext").value = f.extended;
  document.querySelector("#filter-change").checked = f.changeOnly;
  document.querySelector("#filter-interval").value = String(f.minIntervalMs);
}

function flagText(flags) {
  const parts = [];
  if (flags & 0x01) {
    parts.push("EXT");
  }
  if (flags & 0x02) {
    parts.push("RTR");
  }
  if (flags & 0x04) {
    parts.push("TX");
  }
  return parts.join(" ");
}

function hexBytes(bytes) {
  return [...bytes].map((b) => b.toString(16).padStart(2, "0")).join("");
}

function renderTabs() {
  const nav = document.querySelector("#tabs");
  nav.replaceChildren();
  tabs.forEach((tab, i) => {
    const btn = document.createElement("button");
    btn.type = "button";
    btn.id = "tab-" + i;
    btn.textContent = tab.title;
    btn.setAttribute("aria-selected", i === active ? "true" : "false");
    btn.addEventListener("click", () => selectTab(i));
    nav.appendChild(btn);
  });
  const add = document.createElement("button");
  add.type = "button";
  add.id = "add-tab";
  add.textContent = "+";
  add.title = "New Generic tab";
  add.addEventListener("click", addTab);
  nav.appendChild(add);
}

function renderRows() {
  const body = document.querySelector("#rows");
  const tab = tabs[active];
  body.replaceChildren();
  tab.rows.slice(-40).forEach((frame) => {
    const tr = document.createElement("tr");
    const cells = [
      String(frame.tUs >>> 0),
      frame.id.toString(16).toUpperCase(),
      flagText(frame.flags),
      hexBytes(frame.data),
    ];
    cells.forEach((text) => {
      const td = document.createElement("td");
      td.textContent = text;
      tr.appendChild(td);
    });
    body.appendChild(tr);
  });
}

function render() {
  renderTabs();
  renderRows();
}

function selectTab(i) {
  active = i;
  writeFilterForm(tabs[i].filter);
  render();
}

function addTab() {
  tabs.push(makeTab());
  active = tabs.length - 1;
  writeFilterForm(tabs[active].filter);
  render();
}

function paintArmed() {
  const confirm = document.querySelector("#confirm-arm").checked;
  document.querySelector("#arm").disabled = armed || !confirm;
  document.querySelector("#disarm").disabled = !armed;
  document.querySelector("#send-frame").disabled = !armed;
  const del = document.querySelector("#delete-project");
  const current = document.querySelector("#project").value;
  del.disabled = !current || current === "generic";
}

function paintStatus(msg) {
  document.querySelector("#status").textContent = [
    msg.mode, msg.ap, msg.state, String(msg.bitrate), msg.profile, msg.project,
    "rx " + msg.rx, msg.armed ? "armed" : "listen",
  ].join(" · ");
  if (Date.now() >= holdArmedUntil) {
    armed = !!msg.armed;
  }
  if (!profileBound && msg.profile) {
    const sel = document.querySelector("#profile");
    if ([...sel.options].some((opt) => opt.value === msg.profile)) {
      sel.value = msg.profile;
    }
    profileBound = true;
  }
  paintArmed();
}

function showProjects(msg) {
  const sel = document.querySelector("#project");
  const previous = sel.value;
  sel.replaceChildren();
  (msg.projects || []).forEach((project) => {
    const opt = document.createElement("option");
    opt.value = project.id;
    opt.textContent = project.name + " (" + project.id + ")";
    sel.appendChild(opt);
  });
  if (previous) {
    sel.value = previous;
  }
  paintArmed();
}

function showLogs(msg) {
  const list = document.querySelector("#log-list");
  list.replaceChildren();
  (msg.logs || []).forEach((log) => {
    const li = document.createElement("li");
    const a = document.createElement("a");
    a.href = "/logs/" + log.kind + "/" + log.name;
    a.textContent = log.kind + "/" + log.name + " (" + log.bytes + " B)";
    li.appendChild(a);
    list.appendChild(li);
  });
}

function showWifi(msg) {
  (msg.entries || []).forEach((entry) => {
    if (entry.slot === 0 || entry.slot === 1) {
      document.querySelector("#wifi" + entry.slot + "-ssid").value = entry.ssid || "";
      document.querySelector("#wifi" + entry.slot + "-pass").value = entry.password || "";
    }
  });
}

function loggingPayload() {
  return {
    capture_profile: document.querySelector("#profile").value,
    startup_max_s: num("#startup-max"),
    startup_quiet_s: num("#startup-quiet"),
    dealer_quiet_s: num("#dealer-quiet"),
    dealer_context_s: num("#dealer-context"),
    complete_on_user_events: num("#user-events"),
    min_free_mb: 64,
  };
}

function decodeBatch(buffer) {
  const bytes = new Uint8Array(buffer);
  if (bytes.length < 6 || bytes[0] !== 0x43 || bytes[1] !== 0x47 || bytes[2] !== 1) {
    return [];
  }
  const count = bytes[3];
  const frames = [];
  const view = new DataView(buffer);
  let off = 6;
  for (let i = 0; i < count; i++) {
    if (off + 20 > bytes.length) {
      break;
    }
    const length = bytes[off + 9];
    frames.push({
      tUs: view.getUint32(off, true),
      id: view.getUint32(off + 4, true),
      flags: bytes[off + 8],
      data: bytes.slice(off + 10, off + 10 + Math.min(length, 8)),
    });
    off += 20;
  }
  return frames;
}

function onJson(msg) {
  if (msg.op === "hello") {
    gotHello = true;
    note(msg.ap + " " + msg.version);
    if (selftest && !window.__cangoSelftest) {
      window.__cangoSelftest = true;
      runSelfTest();
    }
  } else if (msg.op === "bus_status") {
    paintStatus(msg);
  } else if (msg.op === "get_projects") {
    showProjects(msg);
  } else if (msg.op === "list_logs") {
    showLogs(msg);
  } else if (msg.op === "get_wifi") {
    showWifi(msg);
  } else if (msg.op === "set_capture") {
    if (captureSave) {
      captureSave.ok = !!msg.ok;
    }
    note(msg.ok ? "Capture profile saved" : "Capture profile rejected");
  } else if (msg.op === "arm_tx") {
    if (msg.ok) {
      armed = true;
      holdArmedUntil = Date.now() + 500;
    }
    paintArmed();
    note(msg.ok ? "Armed for 60 seconds" : "Arm rejected");
  } else if (msg.op === "disarm") {
    armed = false;
    holdArmedUntil = Date.now() + 500;
    paintArmed();
    note("Listen only");
  } else if (msg.op === "send_frame" || msg.op === "put_project" || msg.op === "apply_project" || msg.op === "set_wifi") {
    note(msg.op + (msg.ok ? " ok" : " " + (msg.error || "failed")));
  } else if (msg.ok === false) {
    note((msg.op || "request") + " " + (msg.error || "failed"));
  }
}

function connect() {
  const scheme = location.protocol === "https:" ? "wss" : "ws";
  ws = new WebSocket(scheme + "://" + location.host + "/ws");
  ws.binaryType = "arraybuffer";
  ws.onopen = () => {
    send({ op: "hello" });
    send({ op: "get_projects" });
    send({ op: "list_logs" });
    send({ op: "get_wifi" });
    send({ op: "status" });
  };
  ws.onmessage = (ev) => {
    if (typeof ev.data === "string") {
      ev.data.split("\n").filter(Boolean).forEach((line) => {
        try {
          onJson(JSON.parse(line));
        } catch (_err) {
          note("bad json");
        }
      });
      return;
    }
    const frames = decodeBatch(ev.data);
    frames.forEach((frame) => tabs.forEach((tab) => acceptFrame(tab, frame)));
    renderRows();
  };
  ws.onclose = () => {
    document.querySelector("#status").textContent = "disconnected";
    setTimeout(connect, 1000);
  };
}

function allowList(text) {
  return text.split(/[,\s]+/).filter(Boolean).map((part) => Number.parseInt(part, 16)).filter((n) => Number.isFinite(n));
}

function waitFor(pred, ms) {
  const start = Date.now();
  return new Promise((resolve, reject) => {
    const tick = () => {
      if (pred()) {
        resolve();
      } else if (Date.now() - start > ms) {
        reject(new Error("timeout"));
      } else {
        setTimeout(tick, 40);
      }
    };
    tick();
  });
}

async function runSelfTest() {
  const checks = {};
  let error = "";
  try {
    await waitFor(() => gotHello, 3000);
    checks.profileDefault = document.querySelector("#profile").value === "dealer";
    checks.modulesDisabled = document.querySelector("#find-modules").disabled;
    checks.noRunAction = document.querySelector("#run-action") === null;
    checks.addVehicle = document.querySelector("#add-vehicle") !== null;
    checks.zeroXb = document.querySelector("#pick-zero-xb") !== null;
    checks.injectDisabled = document.querySelector("#send-frame").disabled;
    checks.noModeSwitch = document.querySelector("#mode-switch") === null;
    document.querySelector("#filter-active").checked = true;
    document.querySelector("#filter-id").value = "100";
    document.querySelector("#filter-mask").value = "7FF";
    document.querySelector("#apply-filter").click();
    checks.tab0Filter = tabs[0].filter.id === 0x100 && tabs[0].filter.active;
    document.querySelector("#add-tab").click();
    document.querySelector("#filter-active").checked = true;
    document.querySelector("#filter-id").value = "200";
    document.querySelector("#filter-mask").value = "7FF";
    document.querySelector("#apply-filter").click();
    checks.tab0Unchanged = tabs[0].filter.id === 0x100;
    checks.tab1Filter = tabs[1].filter.id === 0x200;
    await waitFor(() => tabs[0].rows.length >= 2 && tabs[1].rows.length >= 2, 5000);
    checks.tab0Ids = tabs[0].rows.every((row) => row.id === 0x100);
    checks.tab1Ids = tabs[1].rows.every((row) => row.id === 0x200);
    document.querySelector("#tab-0").click();
    const ids0 = [...document.querySelectorAll("#rows td:nth-child(2)")];
    checks.tab0Visible = ids0.length > 0 && ids0.every((cell) => cell.textContent === "100");
    document.querySelector("#tab-1").click();
    const ids1 = [...document.querySelectorAll("#rows td:nth-child(2)")];
    checks.tab1Visible = ids1.length > 0 && ids1.every((cell) => cell.textContent === "200");
    document.querySelector("#profile").value = "startup";
    document.querySelector("#save-capture").click();
    await waitFor(() => captureSave && captureSave.ok && captureSave.profile === "startup", 2000);
    checks.savedStartup = true;
    document.querySelector("#confirm-arm").checked = true;
    document.querySelector("#confirm-arm").dispatchEvent(new Event("change"));
    document.querySelector("#arm").click();
    await waitFor(() => !document.querySelector("#send-frame").disabled, 2000);
    checks.armed = true;
    document.querySelector("#disarm").click();
    await waitFor(() => document.querySelector("#send-frame").disabled, 2000);
    checks.disarmed = true;
    const log = await fetch("/logs/dealer/sample.cango");
    const buf = new Uint8Array(await log.arrayBuffer());
    checks.logDownload = log.ok && buf[0] === 0x43 && buf[1] === 0x47 && buf[2] === 0x30 && buf[3] === 0x31;
    checks.noDeviceFilter = sentFilter === 0;
    document.querySelector("#add-vehicle").click();
    checks.pickerOpen = document.querySelector("#vehicle-picker").open === true;
    document.querySelector("#pick-zero-xb").click();
    checks.pickerClosed = document.querySelector("#vehicle-picker").open === false;
    await waitFor(() => document.querySelector("#op-note").textContent.indexOf("apply_project ok") !== -1, 2000);
    checks.zeroApplied = true;
  } catch (err) {
    error = String(err);
  }
  const report = { ok: error === "" && Object.values(checks).every(Boolean), checks, error, sentFilter };
  document.body.dataset.selftest = report.ok ? "pass" : "fail";
  try {
    await fetch("/selftest", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify(report),
    });
  } catch (_err) {
    document.body.dataset.selftest = "fail";
  }
}

document.querySelector("#apply-filter").addEventListener("click", () => {
  tabs[active].filter = readFilterForm();
  tabs[active].state = new Map();
  tabs[active].rows = [];
  renderRows();
  note("Tab filter updated");
});
document.querySelector("#clear-filter").addEventListener("click", () => {
  tabs[active].filter = blankFilter();
  tabs[active].state = new Map();
  tabs[active].rows = [];
  writeFilterForm(tabs[active].filter);
  renderRows();
});
document.querySelector("#save-capture").addEventListener("click", () => {
  const profile = document.querySelector("#profile").value;
  captureSave = { profile, ok: false };
  send({
    op: "set_capture",
    profile,
    startup_max_s: num("#startup-max"),
    startup_quiet_s: num("#startup-quiet"),
    dealer_quiet_s: num("#dealer-quiet"),
    dealer_context_s: num("#dealer-context"),
    complete_on_user_events: num("#user-events"),
  });
});
document.querySelector("#apply-project").addEventListener("click", () => {
  send({ op: "apply_project", id: document.querySelector("#project").value });
});
document.querySelector("#add-vehicle").addEventListener("click", () => {
  document.querySelector("#vehicle-picker").showModal();
});
document.querySelector("#close-vehicle").addEventListener("click", () => {
  document.querySelector("#vehicle-picker").close();
});
document.querySelector("#pick-zero-xb").addEventListener("click", () => {
  document.querySelector("#vehicle-picker").close();
  send({ op: "apply_project", id: "zero-xb" });
});
document.querySelector("#delete-project").addEventListener("click", () => {
  const id = document.querySelector("#project").value;
  if (!id || id === "generic") {
    note("Generic stays");
    return;
  }
  send({ op: "delete_project", id });
});
document.querySelector("#save-project").addEventListener("click", () => {
  const id = document.querySelector("#new-id").value.trim();
  if (!id || id === "generic") {
    note("Generic cannot be saved over");
    return;
  }
  send({
    op: "put_project",
    id,
    name: document.querySelector("#new-name").value.trim() || id,
    bitrate: num("#new-bitrate"),
    listen_only: true,
    logging: loggingPayload(),
    tx_allow: allowList(document.querySelector("#new-allow").value),
    min_free_mb: 64,
  });
});
document.querySelector("#reload-logs").addEventListener("click", () => send({ op: "list_logs" }));
document.querySelector("#confirm-arm").addEventListener("change", paintArmed);
document.querySelector("#arm").addEventListener("click", () => {
  if (!document.querySelector("#confirm-arm").checked) {
    return;
  }
  send({ op: "arm_tx", confirm: true });
});
document.querySelector("#disarm").addEventListener("click", () => send({ op: "disarm" }));
document.querySelector("#send-frame").addEventListener("click", () => {
  send({ op: "send_frame", id: parseHex("#send-id", 0), data: document.querySelector("#send-data").value.trim() });
});
document.querySelector("#save-wifi").addEventListener("click", () => {
  send({
    op: "set_wifi",
    slot: 0,
    ssid: document.querySelector("#wifi0-ssid").value,
    password: document.querySelector("#wifi0-pass").value,
  });
  send({
    op: "set_wifi",
    slot: 1,
    ssid: document.querySelector("#wifi1-ssid").value,
    password: document.querySelector("#wifi1-pass").value,
  });
});
document.querySelector("#project").addEventListener("change", paintArmed);

tabs = [makeTab()];
render();
paintArmed();
connect();

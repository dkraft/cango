/* CANGO page. Each tab filters locally. Do not send set_filter on WiFi:
 * the board has one stream filter, and a second tab would starve the first. */

const params = new URLSearchParams(location.search);
const selftest = params.get("selftest") === "1";
let ws = null;
let tabs = [];
let active = 0;
let armed = false;
let sentFilter = 0;
let gotHello = false;
let holdArmedUntil = 0;
let cardPath = "";
let cardSkip = 0;
let pendingDelete = "";

function blankFilter() {
  return { mode: "all", ids: [], from: null, to: null };
}

function makeTab() {
  return { title: "Generic", filter: blankFilter(), sniff: new Map(), hold: false };
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

function isChecked(filter, id) {
  if (filter.mode === "only") {
    if (filter.ids.indexOf(id) >= 0) {
      return true;
    }
    return filter.from != null && filter.to != null && id >= filter.from && id <= filter.to;
  }
  return filter.ids.indexOf(id) < 0;
}

function toggleId(tab, id) {
  const on = isChecked(tab.filter, id);
  if (on && tab.filter.mode === "only" && tab.filter.ids.indexOf(id) < 0) {
    tab.filter.from = null;
    tab.filter.to = null;
    render();
    return;
  }
  if (tab.filter.mode === "all") {
    if (on) {
      if (tab.filter.ids.length < 32) {
        tab.filter.ids.push(id);
      }
    } else {
      tab.filter.ids = tab.filter.ids.filter((item) => item !== id);
    }
  } else if (on) {
    tab.filter.ids = tab.filter.ids.filter((item) => item !== id);
  } else if (tab.filter.ids.length < 32) {
    tab.filter.ids.push(id);
  }
  render();
}

function visibleRows(tab) {
  const now = Date.now();
  const rows = [];
  tab.sniff.forEach((row) => {
    if (!isChecked(tab.filter, row.id)) {
      return;
    }
    if (!tab.hold && now - row.lastMs > 5000) {
      return;
    }
    rows.push(row);
  });
  rows.sort((a, b) => a.id - b.id);
  return rows;
}

function ingest(tab, frame) {
  const now = Date.now();
  const key = String(frame.id) + ((frame.flags & 1) ? "e" : "s");
  let row = tab.sniff.get(key);
  const incoming = new Uint8Array(8);
  for (let i = 0; i < 8; i++) {
    incoming[i] = i < frame.data.length ? frame.data[i] : 0;
  }
  if (!row) {
    if (tab.sniff.size >= 64) {
      let oldest = null;
      let oldestKey = null;
      tab.sniff.forEach((candidate, candidateKey) => {
        if (oldest === null || candidate.lastMs < oldest) {
          oldest = candidate.lastMs;
          oldestKey = candidateKey;
        }
      });
      if (oldestKey !== null) {
        tab.sniff.delete(oldestKey);
      }
    }
    tab.sniff.set(key, {
      id: frame.id,
      flags: frame.flags,
      data: incoming,
      prev: incoming.slice(),
      changedAt: new Array(8).fill(0),
      notch: new Uint8Array(8),
      lastMs: now,
    });
    return;
  }
  for (let i = 0; i < 8; i++) {
    if (incoming[i] !== row.data[i]) {
      row.prev[i] = row.data[i];
      row.data[i] = incoming[i];
      row.changedAt[i] = now;
    }
  }
  row.flags = frame.flags;
  row.lastMs = now;
}

function byteColor(row, i, now) {
  const visNew = row.data[i] & (~row.notch[i] & 0xff);
  const visOld = row.prev[i] & (~row.notch[i] & 0xff);
  if (now - row.changedAt[i] < 200 && visNew !== visOld) {
    return visNew > visOld ? "#3DDC6A" : "#E25B5B";
  }
  if (now - row.lastMs > 2000) {
    return "#8E98A0";
  }
  return "#ffffff";
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
  add.addEventListener("click", addTab);
  nav.appendChild(add);
}

function renderRows() {
  const body = document.querySelector("#rows");
  const tab = tabs[active];
  const now = Date.now();
  body.replaceChildren();
  visibleRows(tab).forEach((row) => {
    const tr = document.createElement("tr");
    const check = document.createElement("td");
    const box = document.createElement("input");
    box.type = "checkbox";
    box.checked = isChecked(tab.filter, row.id);
    box.addEventListener("change", () => toggleId(tab, row.id));
    check.appendChild(box);
    tr.appendChild(check);
    const idCell = document.createElement("td");
    idCell.className = "id";
    idCell.textContent = row.id.toString(16).toUpperCase();
    idCell.style.color = now - row.lastMs > 2000 ? "#8E98A0" : "#ffffff";
    tr.appendChild(idCell);
    for (let i = 0; i < 8; i++) {
      const cell = document.createElement("td");
      cell.className = "b";
      cell.textContent = row.data[i].toString(16).toUpperCase().padStart(2, "0");
      cell.style.color = byteColor(row, i, now);
      tr.appendChild(cell);
    }
    body.appendChild(tr);
  });
  const hold = document.querySelector("#hold");
  if (hold) {
    hold.textContent = tab.hold ? "●" : "○";
  }
  renderChips();
}

function renderChips() {
  const host = document.querySelector("#filter-chips");
  const tab = tabs[active];
  if (!host) {
    return;
  }
  host.replaceChildren();
  const seen = [];
  tab.sniff.forEach((row) => seen.push(row.id));
  tab.filter.ids.forEach((id) => {
    if (seen.indexOf(id) < 0) {
      seen.push(id);
    }
  });
  seen.sort((a, b) => a - b);
  seen.forEach((id) => {
    const label = document.createElement("label");
    label.className = "check";
    const box = document.createElement("input");
    box.type = "checkbox";
    box.checked = isChecked(tab.filter, id);
    box.addEventListener("change", () => toggleId(tab, id));
    label.appendChild(box);
    label.appendChild(document.createTextNode(id.toString(16).toUpperCase()));
    host.appendChild(label);
  });
}

function render() {
  renderTabs();
  renderRows();
}

function selectTab(i) {
  active = i;
  render();
}

function addTab() {
  tabs.push(makeTab());
  active = tabs.length - 1;
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

function cardRel(name) {
  return cardPath ? cardPath + "/" + name : name;
}

function requestCard() {
  send({ op: "list_dir", path: cardPath, skip: cardSkip });
}

function showFiles(msg) {
  const list = document.querySelector("#file-list");
  if (!msg.ok) {
    if ((msg.path || "") === cardPath) {
      list.replaceChildren();
      document.querySelector("#file-more").hidden = true;
    }
    return;
  }
  if ((msg.path || "") !== cardPath || msg.skip !== cardSkip) {
    return;
  }
  if (msg.skip === 0) {
    list.replaceChildren();
  }
  (msg.entries || []).forEach((entry) => {
    const li = document.createElement("li");
    if (entry.dir) {
      const open = document.createElement("button");
      open.type = "button";
      open.className = "enter";
      open.textContent = entry.name + "/";
      open.addEventListener("click", () => {
        cardPath = cardRel(entry.name);
        cardSkip = 0;
        requestCard();
      });
      li.appendChild(open);
    } else {
      const a = document.createElement("a");
      a.href = "/sd/" + cardRel(entry.name).split("/").map(encodeURIComponent).join("/");
      a.textContent = entry.name;
      li.appendChild(a);
      const size = document.createElement("span");
      size.className = "bytes";
      size.textContent = String(entry.bytes);
      li.appendChild(size);
    }
    const drop = document.createElement("button");
    drop.type = "button";
    drop.className = "drop";
    drop.setAttribute("aria-label", "Delete");
    drop.textContent = "×";
    drop.addEventListener("click", () => {
      pendingDelete = cardRel(entry.name);
      document.querySelector("#file-delete-name").textContent = pendingDelete;
      document.querySelector("#file-sheet").showModal();
    });
    li.appendChild(drop);
    list.appendChild(li);
  });
  document.querySelector("#file-path").textContent = cardPath;
  document.querySelector("#file-more").hidden = !msg.more;
}

function showWifi(msg) {
  (msg.entries || []).forEach((entry) => {
    if (entry.slot === 0 || entry.slot === 1) {
      document.querySelector("#wifi" + entry.slot + "-ssid").value = entry.ssid || "";
      document.querySelector("#wifi" + entry.slot + "-pass").value = entry.password || "";
    }
  });
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
  } else if (msg.op === "list_dir") {
    showFiles(msg);
  } else if (msg.op === "get_wifi") {
    showWifi(msg);
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
    send({ op: "list_dir", path: "", skip: 0 });
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
    frames.forEach((frame) => tabs.forEach((tab) => ingest(tab, frame)));
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
    checks.noCaptureConfig = document.querySelector("#profile") === null && document.querySelector("#save-capture") === null;
    checks.modulesDisabled = document.querySelector("#find-modules").disabled;
    checks.noRunAction = document.querySelector("#run-action") === null;
    checks.addVehicle = document.querySelector("#add-vehicle") !== null;
    checks.zeroXb = document.querySelector("#pick-zero-xb") !== null;
    checks.injectDisabled = document.querySelector("#send-frame").disabled;
    checks.noModeSwitch = document.querySelector("#mode-switch") === null;
    document.querySelector("#filter-none").click();
    document.querySelector("#filter-id").value = "100";
    document.querySelector("#filter-add").click();
    checks.tab0Filter = tabs[0].filter.mode === "only" && tabs[0].filter.ids[0] === 0x100;
    document.querySelector("#add-tab").click();
    document.querySelector("#filter-none").click();
    document.querySelector("#filter-id").value = "200";
    document.querySelector("#filter-add").click();
    checks.tab0Unchanged = tabs[0].filter.mode === "only" && tabs[0].filter.ids[0] === 0x100;
    checks.tab1Filter = tabs[1].filter.mode === "only" && tabs[1].filter.ids[0] === 0x200;
    await waitFor(() => visibleRows(tabs[0]).length === 1 && visibleRows(tabs[1]).length === 1, 5000);
    checks.tab0Ids = visibleRows(tabs[0])[0].id === 0x100;
    checks.tab1Ids = visibleRows(tabs[1])[0].id === 0x200;
    document.querySelector("#tab-0").click();
    const ids0 = [...document.querySelectorAll("#rows td:nth-child(2)")];
    checks.tab0Visible = ids0.length === 1 && ids0.every((cell) => cell.textContent === "100");
    document.querySelector("#tab-1").click();
    const ids1 = [...document.querySelectorAll("#rows td:nth-child(2)")];
    checks.tab1Visible = ids1.length === 1 && ids1.every((cell) => cell.textContent === "200");
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
    checks.files = document.querySelector("#file-list") !== null && document.querySelector("#file-up") !== null && document.querySelector("#file-sheet") !== null;
    const enterNamed = (label) => {
      const node = [...document.querySelectorAll("#file-list .enter")].find((item) => item.textContent === label);
      if (!node) {
        throw new Error(label);
      }
      node.click();
    };
    await waitFor(() => document.querySelector("#file-list .enter") !== null, 3000);
    enterNamed("logs/");
    await waitFor(() => document.querySelector("#file-path").textContent === "logs", 2000);
    enterNamed("dealer/");
    await waitFor(() => document.querySelector("#file-path").textContent === "logs/dealer", 2000);
    const fileLink = document.querySelector("#file-list a");
    checks.fileLink = fileLink !== null && fileLink.getAttribute("href") === "/sd/logs/dealer/sample.cango" && fileLink.textContent === "sample.cango";
    const sd = await fetch("/sd/logs/dealer/sample.cango");
    const sdBuf = new Uint8Array(await sd.arrayBuffer());
    checks.sdDownload = sd.ok && sdBuf[0] === 0x43 && sdBuf[1] === 0x47;
    document.querySelector("#file-list .drop").click();
    checks.deleteOpen = document.querySelector("#file-sheet").open === true;
    document.querySelector("#file-delete").click();
    await waitFor(() => document.querySelector("#file-list a") === null && document.querySelector("#file-sheet").open === false, 2000);
    checks.deleted = true;
    checks.noDeviceFilter = sentFilter === 0;
    checks.help = document.querySelector("#help") !== null && document.querySelector("#help-sheet") !== null;
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

function addTypedId() {
  const tab = tabs[active];
  const id = parseHex("#filter-id", NaN);
  if (!Number.isFinite(id)) {
    return;
  }
  const toRaw = document.querySelector("#filter-to").value.trim();
  if (tab.filter.mode !== "only") {
    tab.filter.ids = tab.filter.ids.filter((item) => item !== id);
    render();
    return;
  }
  if (toRaw) {
    const to = Number.parseInt(toRaw, 16);
    if (Number.isFinite(to)) {
      tab.filter.from = Math.min(id, to);
      tab.filter.to = Math.max(id, to);
    }
  } else if (tab.filter.ids.indexOf(id) < 0 && tab.filter.ids.length < 32) {
    tab.filter.ids.push(id);
  }
  render();
}

function addNotch() {
  const now = Date.now();
  tabs[active].sniff.forEach((row) => {
    for (let i = 0; i < 8; i++) {
      if (now - row.changedAt[i] < 200) {
        row.notch[i] |= row.data[i] ^ row.prev[i];
      }
    }
  });
  renderRows();
}

function clearNotch() {
  tabs[active].sniff.forEach((row) => row.notch.fill(0));
  renderRows();
}

document.querySelector("#filter-all").addEventListener("click", () => {
  tabs[active].filter = blankFilter();
  render();
});
document.querySelector("#filter-none").addEventListener("click", () => {
  tabs[active].filter = { mode: "only", ids: [], from: null, to: null };
  render();
});
document.querySelector("#filter-add").addEventListener("click", addTypedId);
document.querySelector("#funnel").addEventListener("click", () => {
  document.querySelector("#funnel-sheet").showModal();
});
document.querySelector("#funnel-close").addEventListener("click", () => {
  document.querySelector("#funnel-sheet").close();
});
document.querySelector("#help").addEventListener("click", () => {
  document.querySelector("#help-sheet").showModal();
});
document.querySelector("#help-close").addEventListener("click", () => {
  document.querySelector("#help-sheet").close();
});
document.querySelector("#hold").addEventListener("click", () => {
  tabs[active].hold = !tabs[active].hold;
  renderRows();
});
(function wireNotch() {
  const notch = document.querySelector("#notch");
  let timer = 0;
  notch.addEventListener("pointerdown", (ev) => {
    timer = window.setTimeout(() => {
      timer = 0;
      clearNotch();
    }, 600);
    ev.preventDefault();
  });
  notch.addEventListener("pointerup", () => {
    if (timer) {
      window.clearTimeout(timer);
      timer = 0;
      addNotch();
    }
  });
  notch.addEventListener("pointercancel", () => {
    if (timer) {
      window.clearTimeout(timer);
      timer = 0;
    }
  });
})();
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
    tx_allow: allowList(document.querySelector("#new-allow").value),
    min_free_mb: 64,
  });
});
document.querySelector("#file-up").addEventListener("click", () => {
  const cut = cardPath.lastIndexOf("/");
  cardPath = cut < 0 ? "" : cardPath.slice(0, cut);
  cardSkip = 0;
  requestCard();
});
document.querySelector("#file-refresh").addEventListener("click", () => {
  cardSkip = 0;
  requestCard();
});
document.querySelector("#file-more").addEventListener("click", () => {
  cardSkip = document.querySelectorAll("#file-list li").length;
  requestCard();
});
document.querySelector("#file-delete").addEventListener("click", () => {
  const rel = pendingDelete;
  pendingDelete = "";
  document.querySelector("#file-sheet").close();
  if (!rel) {
    return;
  }
  send({ op: "delete_path", path: rel });
  cardSkip = 0;
  requestCard();
});
document.querySelector("#file-delete-close").addEventListener("click", () => {
  pendingDelete = "";
  document.querySelector("#file-sheet").close();
});
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
setInterval(() => {
  if (!document.hidden) {
    renderRows();
  }
}, 200);

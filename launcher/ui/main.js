// The launcher window. Everything persistent lives in the Rust side's launcher.json (and the
// keychain, for passwords); this page edits it and saves on every change.
const { invoke } = window.__TAURI__.core;
const { listen } = window.__TAURI__.event;
const dialog = window.__TAURI__.dialog;

const $ = (s) => document.querySelector(s);
let cfg = null;
let selected = null; // account id
let running = false;
let gameDefaults = null;

const accountForm = $("#account");
const gameForm = $("#game-settings");
const launcherForm = $("#launcher-settings");

function newId() {
  return "acct-" + Date.now().toString(36) + Math.random().toString(36).slice(2, 7);
}

async function save(form) {
  await invoke("save_config", { cfg });
  const note = form && form.querySelector(".saved");
  if (note) {
    note.hidden = false;
    clearTimeout(note._t);
    note._t = setTimeout(() => (note.hidden = true), 1200);
  }
}

// --- tabs ------------------------------------------------------------------------------------------
document.querySelectorAll(".tab").forEach((t) =>
  t.addEventListener("click", () => {
    document.querySelectorAll(".tab, .page").forEach((e) => e.classList.remove("active"));
    t.classList.add("active");
    $("#" + t.dataset.tab).classList.add("active");
  })
);

// --- accounts --------------------------------------------------------------------------------------
const KIND_LABEL = { pol: "PlayOnline", lsb: "LandSandBoat" };

function account() {
  return cfg.accounts.find((a) => a.id === selected);
}

function renderAccounts() {
  const ul = $("#accounts");
  ul.innerHTML = "";
  for (const a of cfg.accounts) {
    const li = document.createElement("li");
    li.className = a.id === selected ? "selected" : "";
    const name = document.createElement("span");
    name.textContent = a.name || a.login || "New account";
    const sub = document.createElement("small");
    sub.textContent = a.server || "no server";
    li.append(name, sub);
    li.addEventListener("click", () => select(a.id));
    ul.append(li);
  }
  $("#no-account").hidden = cfg.accounts.length > 0;
  accountForm.hidden = !account();
}

function showKind(kind) {
  accountForm.querySelectorAll("[data-kind]").forEach((e) => (e.hidden = e.dataset.kind !== kind));
}

async function select(id) {
  selected = id;
  const a = account();
  renderAccounts();
  if (!a) return;
  for (const k of ["name", "kind", "login", "server", "pol_server", "update_url"]) accountForm.elements[k].value = a[k] ?? "";
  for (const k of ["auth_port", "data_port", "view_port"]) accountForm.elements[k].value = a[k] || "";
  accountForm.elements.save_password.checked = a.save_password;
  accountForm.elements.password.value = "";
  accountForm.elements.otp.value = "";
  accountForm.elements.password.placeholder = a.password_saved ? "stored in the keychain" : "";
  showKind(a.kind);
}

accountForm.addEventListener("change", async (e) => {
  const a = account();
  const name = e.target.name;
  if (!a || name === "password" || name === "otp") return;
  if (name === "save_password") {
    a.save_password = e.target.checked;
    if (!a.save_password) {
      await invoke("forget_password", { accountId: a.id });
      a.password_saved = false;
      accountForm.elements.password.placeholder = "";
    }
  } else if (name.endsWith("_port")) {
    a[name] = parseInt(e.target.value, 10) || 0;
  } else {
    a[name] = e.target.value.trim();
  }
  if (name === "kind") showKind(a.kind);
  await save();
  renderAccounts();
});

$("#add-account").addEventListener("click", async () => {
  const a = {
    id: newId(), name: "", kind: "lsb", login: "", server: "127.0.0.1", pol_server: "",
    save_password: true, password_saved: false, auth_port: 0, data_port: 0, view_port: 0, update_url: "", game_path: "",
  };
  cfg.accounts.push(a);
  await save();
  await select(a.id);
  accountForm.elements.name.focus();
});

$("#delete-account").addEventListener("click", async () => {
  const a = account();
  if (!a || !confirm(`Delete ${a.name || a.login || "this account"}? Its stored password goes too.`)) return;
  cfg.accounts = cfg.accounts.filter((x) => x.id !== a.id);
  await save();
  await select(cfg.accounts[0]?.id ?? null);
});

// --- playing ---------------------------------------------------------------------------------------
function setStatus(text, error) {
  $("#status").textContent = text;
  $("#status").className = error ? "error" : "";
}

function setRunning(r) {
  running = r;
  document.querySelectorAll(".running-note").forEach((e) => (e.hidden = !r));
  $("#play-button").disabled = r;
  $("#stop").hidden = !r;
}

// --- the server's version, before playing (versions.rs) ------------------------------------------
// resolves with the task-done / build-done event of the job `start` begins
function waitFor(event, task) {
  return new Promise((resolve) => {
    let un;
    listen(event, ({ payload }) => {
      if (task && payload.task !== task) return;
      un?.();
      resolve(payload);
    }).then((u) => (un = u));
  });
}

function playProgress(fraction, text) {
  const bar = $("#play-progress");
  bar.hidden = fraction === null;
  if (fraction !== null) bar.querySelector(".bar").style.width = (fraction * 100).toFixed(1) + "%";
  if (text) setStatus(text);
}

// Brings the account the version its server wants, and makes the game for it. Throws to stop Play.
async function readyForServer(a) {
  if (a.update_url) setStatus("Checking which version the server wants…");
  let sv;
  try {
    sv = await invoke("check_server", { accountId: a.id });
  } catch (err) {
    // a server that publishes nothing: play what there is; one that does but did not answer: say so
    if (a.update_url) setStatus(`Could not reach the game updates address (${err}); playing the version you have.`);
    return;
  }
  cfg = await invoke("get_config"); // the address, when it was found on the server itself
  if (!sv.supported) throw `The server wants version ${sv.current}, which this launcher cannot run yet: the game's code changed in that version and needs new metadata (see "Supporting a new client version" in the README).`;
  if (!sv.have) {
    if (!confirm(`${a.name || a.server} wants version ${sv.current} of the game. Download it now? Only the files you do not have are downloaded; your install is not changed.`))
      throw "The server needs another version of the game.";
    const done = waitFor("task-done", "update");
    playProgress(0, `Getting version ${sv.current}…`);
    await invoke("update_for_server", { accountId: a.id });
    const r = await done;
    if (!r.ok) throw r.message;
    cfg = await invoke("get_config");
  }
  const gs = await invoke("game_status", { gamePath: sv.game_path });
  if (!gs.ready) {
    const done = waitFor("build-done");
    playProgress(0, `Making the game for version ${sv.current}…`);
    await invoke("start_build", { gamePath: sv.game_path });
    const r = await done;
    if (!r.ok) throw r.message;
  }
  playProgress(null);
}

// The game made for this install, and made by this launcher: a game from an older one may not run
// with its host (a new module ABI), so Play makes it again first (about half a minute).
async function ensureGame(gamePath) {
  const gs = await invoke("game_status", { gamePath });
  if (gs.ready && !gs.outdated) return;
  if (gs.error) throw gs.error;
  const done = waitFor("build-done");
  playProgress(0, gs.ready ? "Updating the game for this launcher…" : "Making the game…");
  await invoke("start_build", { gamePath });
  const r = await done;
  if (!r.ok) throw r.message;
  playProgress(null);
  await refreshGame();
}

listen("task-progress", ({ payload }) => {
  if (payload.task === "update") playProgress(payload.fraction, payload.step);
  else taskProgress(payload.fraction, payload.step);
});
listen("build-progress", ({ payload }) => {
  if (running) playProgress(payload.fraction, payload.step);
});

accountForm.addEventListener("submit", async (e) => {
  e.preventDefault();
  let a = account();
  if (!a || running) return;
  $("#log").textContent = "";
  setStatus("Starting…");
  setRunning(true);
  try {
    await readyForServer(a);
    a = account();
    await ensureGame(a.game_path || cfg.game_path);
    a.password_saved = await invoke("launch", {
      accountId: a.id,
      password: accountForm.elements.password.value,
      otp: accountForm.elements.otp.value.trim(),
    });
    cfg.last_account = a.id;
    await save();
    accountForm.elements.password.value = "";
    accountForm.elements.otp.value = "";
    accountForm.elements.password.placeholder = a.password_saved ? "stored in the keychain" : "";
  } catch (err) {
    setRunning(false);
    playProgress(null);
    setStatus(String(err), true);
  }
});

$("#stop").addEventListener("click", () => invoke("stop"));

listen("lobby-error", ({ payload }) => {
  if (payload !== 331) return;
  const a = account();
  setStatus(
    a?.update_url
      ? "The server needs a different version of the game. Press Play again to get it."
      : "The server needs a different version of the game. If it publishes its versions, add its game updates address under Advanced.",
    true
  );
});

listen("game-state", ({ payload }) => {
  setStatus(payload.message, payload.state === "error");
  setRunning(payload.state === "signing-in" || payload.state === "running");
});

const log = $("#log");
listen("game-log", ({ payload }) => {
  const atEnd = log.scrollTop + log.clientHeight >= log.scrollHeight - 4;
  log.append(payload + "\n");
  // keep the view bounded: the game can log a lot
  if (log.textContent.length > 400000) log.textContent = log.textContent.slice(-300000);
  if (atEnd) log.scrollTop = log.scrollHeight;
});

// --- game settings ---------------------------------------------------------------------------------
function fillSettings(form, obj) {
  for (const el of form.elements) {
    if (!el.name || !(el.name in obj)) continue;
    if (el.type === "checkbox") el.checked = !!obj[el.name];
    else el.value = obj[el.name];
  }
  if (form === gameForm) syncQuality();
}

// the size the player gave the game's window, saved by the launcher: kept here too, so a later save
// of these settings does not put the old one back
listen("window-size", (e) => {
  if (cfg?.game) cfg.game.window_points = e.payload;
});

// the background resolution fields only for a custom 3D quality
function syncQuality() {
  $("#background-custom").hidden = (cfg.game?.render_quality ?? "quality") !== "custom";
}

gameForm.addEventListener("change", async (e) => {
  const el = e.target;
  if (el.dataset.fx) return setFx(el.dataset.fx, el.type === "checkbox" ? (el.checked ? 1 : 0) : parseFloat(el.value), true);
  if (!el.name) return;
  cfg.game[el.name] = el.type === "checkbox" ? el.checked : "num" in el.dataset ? parseInt(el.value, 10) || 0 : el.value;
  if (el.name === "render_quality") syncQuality();
  await save(gameForm);
});

$("#game-defaults").addEventListener("click", async () => {
  cfg.game = { ...gameDefaults, fx: { ...gameDefaults.fx } };
  fillSettings(gameForm, cfg.game);
  renderFx();
  await save(gameForm);
});

// --- scene effects (the Metal back end's; written to live.txt with the rest) -----------------------
const IS_MAC = navigator.userAgent.includes("Mac");
$("#fx-section").hidden = !IS_MAC;
document.querySelectorAll(".mac-only").forEach((e) => (e.hidden = !IS_MAC));

let fxDefaults = {};
const FX_CONTROLS = [
  { key: "ao", label: "Ambient occlusion", min: 0, max: 1.5, step: 0.05 },
  { key: "shadow", label: "Contact shadows", min: 0, max: 1, step: 0.05 },
  { key: "sun", label: "Sun shadows", min: 0, max: 1, step: 0.05 },
  { key: "bloom", label: "Bloom", min: 0, max: 1, step: 0.05 },
  { key: "rays", label: "Light rays", min: 0, max: 1.5, step: 0.05 },
  { key: "fog", label: "Atmospheric fog", min: 0, max: 0.02, step: 0.0005 },
  { key: "sat", label: "Color saturation", min: 0.8, max: 1.5, step: 0.01 },
  { key: "contrast", label: "Contrast", min: 0, max: 0.6, step: 0.01 },
  { key: "sharpen", label: "Sharpening", min: 0, max: 1, step: 0.05 },
  { key: "upscale", label: "MetalFX upscaling (a sharp frame on a high-resolution screen: 0 off, 1 on)", min: 0, max: 1, step: 1 },
  { key: "aniso", label: "Texture filtering", min: 1, max: 16, step: 1 },
  { key: "temporal", label: "Temporal smoothing", min: 0, max: 0.95, step: 0.05 },
  { key: "light", label: "Per-pixel lighting", check: true },
];
// fx = 1 plus these over the engine's defaults; "enhanced" is the defaults as they are
const FX_PRESETS = {
  off: { fx: 0 },
  subtle: { fx: 1, ao: 0.5, shadow: 0.2, sun: 0.3, bloom: 0.15, rays: 0.3, fog: 0.002, sat: 1.05, contrast: 0.1, sharpen: 0.2 },
  enhanced: { fx: 1 },
  cinematic: { fx: 1, ao: 1.1, shadow: 0.45, sun: 0.7, bloom: 0.45, rays: 0.9, fog: 0.006, sat: 1.18, contrast: 0.3, sharpen: 0.35 },
};

function fxValue(key) {
  const v = cfg.game.fx?.[key];
  return v === undefined ? fxDefaults[key] : v;
}

function renderFx() {
  const div = $("#fx-sliders");
  div.innerHTML = "";
  div.classList.toggle("off", !fxValue("fx"));
  const on = document.createElement("label");
  on.className = "check";
  on.innerHTML = '<input type="checkbox" data-fx="fx"> Scene effects on';
  on.firstChild.checked = !!fxValue("fx");
  div.append(on);
  for (const c of FX_CONTROLS) {
    if (c.check) {
      const l = document.createElement("label");
      l.className = "check";
      l.innerHTML = `<input type="checkbox" data-fx="${c.key}"> ${c.label}`;
      l.firstChild.checked = !!fxValue(c.key);
      div.append(l);
      continue;
    }
    const row = document.createElement("label");
    row.className = "slider";
    const name = document.createElement("span");
    name.textContent = c.label;
    const input = document.createElement("input");
    Object.assign(input, { type: "range", min: c.min, max: c.max, step: c.step, value: fxValue(c.key) });
    input.dataset.fx = c.key;
    const out = document.createElement("span");
    out.textContent = fmt(fxValue(c.key));
    input.addEventListener("input", () => {
      out.textContent = fmt(input.value);
      setFx(c.key, parseFloat(input.value), false);
    });
    row.append(name, input, out);
    div.append(row);
  }
  document.querySelectorAll("[data-preset]").forEach((b) => b.classList.toggle("active", b.dataset.preset === currentPreset()));
}

function fmt(v) {
  v = parseFloat(v);
  return Math.abs(v) >= 1 || v === 0 ? String(+v.toFixed(2)) : String(+v.toFixed(4));
}

function currentPreset() {
  for (const [name, p] of Object.entries(FX_PRESETS)) {
    const want = { ...fxDefaults, ...p };
    if (name === "off" ? !fxValue("fx") : Object.keys(fxDefaults).every((k) => Math.abs(fxValue(k) - want[k]) < 1e-6)) return name;
  }
  return null;
}

// a slider being dragged saves at most every 150 ms; the game reads the file twice a second
let fxTimer = null;
function setFx(key, value, now) {
  cfg.game.fx = { ...(cfg.game.fx || {}), [key]: value };
  if (key === "fx") $("#fx-sliders").classList.toggle("off", !value);
  document.querySelectorAll("[data-preset]").forEach((b) => b.classList.toggle("active", b.dataset.preset === currentPreset()));
  clearTimeout(fxTimer);
  fxTimer = setTimeout(() => save(gameForm), now ? 0 : 150);
}

document.querySelectorAll("[data-preset]").forEach((b) =>
  b.addEventListener("click", async () => {
    cfg.game.fx = { ...fxDefaults, ...FX_PRESETS[b.dataset.preset] };
    renderFx();
    await save(gameForm);
  })
);

// the game asked for this page (its hotkey), or the lobby turned it away
listen("open-page", ({ payload }) => {
  closeSetup();
  document.querySelector(`.tab[data-tab="${payload}"]`)?.click();
});

// --- launcher settings -----------------------------------------------------------------------------
function fillLauncher() {
  fillSettings(launcherForm, cfg);
  launcherForm.elements.dats.value = cfg.dats.join("\n");
}

launcherForm.addEventListener("change", async (e) => {
  const el = e.target;
  if (!el.name) return;
  if (el.name === "game_path") return chooseFolder(el.value.trim());
  if (el.name === "vault_dir") {
    cfg.vault_dir = el.value.trim();
    await save(launcherForm);
    return refreshVersions();
  }
  if (el.name === "dats") cfg.dats = el.value.split("\n").map((s) => s.trim()).filter(Boolean);
  else cfg[el.name] = el.value.trim();
  await save(launcherForm);
});

launcherForm.querySelectorAll("[data-pick]").forEach((b) =>
  b.addEventListener("click", async () => {
    const path = await dialog.open({ directory: b.dataset.pick === "folder", multiple: false });
    if (!path) return;
    if (b.dataset.append) {
      const ta = launcherForm.elements[b.dataset.append];
      ta.value = (ta.value.trim() ? ta.value.trim() + "\n" : "") + path;
      ta.dispatchEvent(new Event("change", { bubbles: true }));
    } else if (b.dataset.for === "game_path") {
      await chooseFolder(path);
    } else {
      const input = launcherForm.elements[b.dataset.for];
      input.value = path;
      input.dispatchEvent(new Event("change", { bubbles: true }));
    }
  })
);

// --- setup: the game folder, making the game for it, the first server ------------------------------
const setup = $("#setup");
let setupStatus = null;

function showStep(n) {
  setup.querySelectorAll(".step").forEach((s) => (s.hidden = +s.dataset.step !== n));
  setup.querySelectorAll(".stepper li").forEach((li) => {
    li.classList.toggle("current", +li.dataset.step === n);
    li.classList.toggle("done", +li.dataset.step < n);
  });
  if (n === 2) refreshPrereqs();
  if (n === 3) {
    $("#viewer-hint").hidden = !!setupStatus?.viewer;
    firstForm.elements.name.focus();
  }
}

async function openSetup(step) {
  setup.hidden = false;
  await refreshGame();
  renderInstalls();
  showStep(step ?? 1);
}

function closeSetup() {
  setup.hidden = true;
}

function buildName(b) {
  return b && b.label ? `version ${b.version || "?"} (build ${b.label})` : "";
}

// the chip in the tab bar: what the launcher will play, or what it still needs
function renderChip() {
  const chip = $("#game-chip");
  const st = setupStatus;
  chip.className = "chip";
  if (!st || !st.game_path) {
    chip.textContent = "Choose your game";
    chip.classList.add("attention");
  } else if (!st.ready) {
    chip.textContent = st.error ? "Game: not supported" : "Make the game";
    chip.classList.add("attention");
  } else {
    chip.textContent = `FFXI ${st.build.version}` + (st.outdated ? " · update ready" : "");
    chip.classList.add(st.outdated ? "attention" : "ready");
  }
}

async function refreshGame(st) {
  setupStatus = st ?? (await invoke("setup_status"));
  const box = $("#game-status");
  const s = setupStatus;
  box.hidden = !s.game_path;
  box.className = "status-box " + (s.error ? "error" : s.build?.label ? "ok" : "");
  box.innerHTML = "";
  const title = document.createElement("div");
  title.textContent = s.error
    ? s.error
    : `FINAL FANTASY XI ${buildName(s.build)}` + (s.ready ? (s.outdated ? ". Made by an older launcher: make it again to update." : ". Ready to play.") : ". Supported.");
  const where = document.createElement("small");
  where.textContent = s.game_path;
  box.append(title, where);
  $("#step1-next").disabled = !s.build?.label;
  renderChip();
  renderInstalls();
}

let foundInstalls = [];
function renderInstalls() {
  const div = $("#found-installs");
  div.innerHTML = "";
  if (!foundInstalls.length) return;
  const label = document.createElement("div");
  label.className = "label";
  label.textContent = foundInstalls.length === 1 ? "Found on this computer:" : "Found on this computer (choose one):";
  div.append(label);
  for (const path of foundInstalls) {
    const b = document.createElement("button");
    b.type = "button";
    b.textContent = path;
    b.classList.toggle("selected", setupStatus?.game_path === path);
    b.addEventListener("click", () => chooseFolder(path));
    div.append(b);
  }
}

async function chooseFolder(path) {
  if (!path) return;
  try {
    const st = await invoke("set_game_folder", { path });
    cfg.game_path = st.game_path;
    fillLauncher();
    await refreshGame(st);
  } catch (err) {
    const box = $("#game-status");
    box.hidden = false;
    box.className = "status-box error";
    box.textContent = String(err);
    $("#step1-next").disabled = true;
  }
}

$("#pick-game").addEventListener("click", async () => {
  const path = await dialog.open({ directory: true, multiple: false, title: "The FINAL FANTASY XI folder" });
  if (path) await chooseFolder(path);
});

$("#step1-next").addEventListener("click", () => {
  if (setupStatus?.ready && !setupStatus.outdated) afterGameReady();
  else showStep(2);
});

function afterGameReady() {
  if (cfg.accounts.length) closeSetup();
  else showStep(3);
}

setup.querySelectorAll("[data-close-setup]").forEach((b) => b.addEventListener("click", closeSetup));
setup.querySelectorAll("[data-step-to]").forEach((b) => b.addEventListener("click", () => showStep(+b.dataset.stepTo)));
$("#game-chip").addEventListener("click", () => openSetup(setupStatus?.game_path ? (setupStatus.ready ? 1 : 2) : 1));
$("#reopen-setup").addEventListener("click", () => openSetup(1));

// what making the game needs
let prereqsOk = false;
async function refreshPrereqs() {
  const ul = $("#prereqs");
  ul.innerHTML = '<li class="muted">Checking…</li>';
  const list = await invoke("check_prereqs");
  ul.innerHTML = "";
  for (const p of list) {
    const li = document.createElement("li");
    li.className = p.ok ? "ok" : "missing";
    const mark = document.createElement("span");
    mark.className = "mark";
    mark.textContent = p.ok ? "✓" : "✗";
    const name = document.createElement("span");
    name.textContent = p.name;
    const detail = document.createElement("small");
    detail.textContent = p.detail;
    li.append(mark, name, detail);
    if (!p.ok && p.fix) {
      const code = document.createElement("code");
      code.textContent = p.fix;
      li.append(code);
    }
    ul.append(li);
  }
  prereqsOk = list.every((p) => p.ok);
  $("#install-prereqs").hidden = !list.some((p) => !p.ok && p.installable);
  $("#make-game").disabled = !prereqsOk || building;
  if (!building) {
    const st = $("#build-status");
    st.className = "";
    st.textContent = prereqsOk
      ? setupStatus?.ready ? "The game is already made; making it again updates it." : ""
      : "Install what is missing, then check again.";
  }
}

$("#check-prereqs").addEventListener("click", refreshPrereqs);

let building = false;
function setBuilding(b) {
  building = b;
  $("#make-game").disabled = b || !prereqsOk;
  $("#cancel-build").hidden = !b;
  $("#check-prereqs").disabled = b;
  $("#install-prereqs").disabled = b;
  setup.querySelectorAll("[data-step-to], [data-close-setup]").forEach((e) => (e.disabled = b));
}

function buildStatus(text, kind) {
  const st = $("#build-status");
  st.textContent = text;
  st.className = kind || "";
}

const buildLog = $("#build-log");
function appendBuildLog(line) {
  const atEnd = buildLog.scrollTop + buildLog.clientHeight >= buildLog.scrollHeight - 4;
  buildLog.append(line + "\n");
  if (buildLog.textContent.length > 300000) buildLog.textContent = buildLog.textContent.slice(-200000);
  if (atEnd) buildLog.scrollTop = buildLog.scrollHeight;
}

$("#install-prereqs").addEventListener("click", async () => {
  buildLog.textContent = "";
  buildStatus("Installing…");
  $("#build-details").open = true;
  setBuilding(true);
  try {
    await invoke("install_prereqs");
  } catch (err) {
    setBuilding(false);
    buildStatus(String(err), "error");
  }
});

listen("install-done", async ({ payload }) => {
  setBuilding(false);
  await refreshPrereqs();
  buildStatus(payload.message, payload.ok ? "ok" : "error");
});

$("#make-game").addEventListener("click", async () => {
  buildLog.textContent = "";
  buildStatus("");
  $("#build-box").hidden = false;
  $("#build-bar").style.width = "0%";
  $("#build-step").textContent = "Starting…";
  setBuilding(true);
  try {
    await invoke("start_build");
  } catch (err) {
    setBuilding(false);
    $("#build-box").hidden = true;
    buildStatus(String(err), "error");
  }
});

$("#cancel-build").addEventListener("click", () => invoke("cancel_build"));

listen("build-progress", ({ payload }) => {
  $("#build-box").hidden = false;
  $("#build-bar").style.width = (payload.fraction * 100).toFixed(1) + "%";
  $("#build-step").textContent = payload.step;
});
listen("build-log", ({ payload }) => appendBuildLog(payload));
listen("build-done", async ({ payload }) => {
  setBuilding(false);
  await refreshGame();
  if (payload.ok) {
    buildStatus(payload.message, "ok");
    setTimeout(afterGameReady, 700);
  } else {
    $("#build-box").hidden = true;
    buildStatus(payload.message, "error");
    $("#build-details").open = true;
  }
});

// the first server
const firstForm = $("#first-account");
firstForm.elements.kind.addEventListener("change", () => {
  const kind = firstForm.elements.kind.value;
  firstForm.querySelectorAll("[data-kind]").forEach((e) => (e.hidden = e.dataset.kind !== kind));
});
firstForm.addEventListener("submit", async (e) => {
  e.preventDefault();
  const f = firstForm.elements;
  const a = {
    id: newId(), name: f.name.value.trim(), kind: f.kind.value, login: f.login.value.trim(),
    server: f.server.value.trim(), pol_server: "", save_password: true, password_saved: false,
    auth_port: 0, data_port: 0, view_port: 0,
  };
  cfg.accounts.push(a);
  cfg.last_account = a.id;
  await save();
  await select(a.id);
  closeSetup();
  document.querySelector('.tab[data-tab="play"]').click();
  accountForm.elements.password.focus();
});

// --- game files: backups, checking, versions (versions.rs) ---------------------------------------
function taskProgress(fraction, text) {
  const bar = $("#task-progress");
  bar.hidden = fraction === null;
  if (fraction !== null) bar.querySelector(".bar").style.width = (fraction * 100).toFixed(1) + "%";
  if (text !== undefined) {
    $("#task-status").textContent = text;
    $("#task-status").className = "";
  }
}

async function refreshVersions() {
  let st;
  try {
    st = await invoke("versions_status");
  } catch (err) {
    $("#files-status").textContent = String(err);
    return;
  }
  launcherForm.elements.vault_dir.placeholder = st.vault_dir;
  $("#files-status").textContent = !cfg.game_path
    ? "Choose the game folder first."
    : st.install_version
      ? `This install is version ${st.install_version}, backed up.`
      : "This install is not backed up yet.";
  $("#check-files").disabled = !st.install_version;
  const ul = $("#versions");
  ul.innerHTML = "";
  for (const v of st.versions) {
    const li = document.createElement("li");
    const name = document.createElement("span");
    name.textContent = `${v.version}` + (v.build ? ` (build ${v.build})` : "");
    const info = document.createElement("small");
    info.textContent = `${v.files} files · ${(v.bytes / 1e9).toFixed(2)} GB` + (v.installed ? " · put together" : "") + (v.version === st.install_version ? " · your install" : "");
    li.append(name, info);
    ul.append(li);
  }
}

async function startTask(command, args, task) {
  const done = waitFor("task-done", task);
  taskProgress(0, "Starting…");
  ["#backup", "#check-files", "#repair-files"].forEach((s) => ($(s).disabled = true));
  try {
    await invoke(command, args);
    const r = await done;
    taskProgress(null);
    $("#task-status").textContent = r.message;
    $("#task-status").className = r.ok ? "ok" : "error";
    return r;
  } catch (err) {
    taskProgress(null);
    $("#task-status").textContent = String(err);
    $("#task-status").className = "error";
  } finally {
    ["#backup", "#check-files", "#repair-files"].forEach((s) => ($(s).disabled = false));
    await refreshVersions();
  }
}

$("#backup").addEventListener("click", () => startTask("backup_install", {}, "backup"));
$("#check-files").addEventListener("click", async () => {
  const r = await startTask("check_install", { full: true, repair: false }, "check");
  $("#repair-files").hidden = !(r?.ok && r.detail && r.detail.missing + r.detail.wrong > 0);
});
$("#repair-files").addEventListener("click", async () => {
  if (!confirm("Put the missing and damaged files back from the backup? Only those files in the game folder are written.")) return;
  const r = await startTask("check_install", { full: true, repair: true }, "repair");
  if (r?.ok) $("#repair-files").hidden = true;
});

// --- start -----------------------------------------------------------------------------------------
(async () => {
  cfg = await invoke("get_config");
  const d = await invoke("get_defaults");
  launcherForm.elements.host_program.placeholder = d.host_program;
  launcherForm.elements.base_registry.placeholder = d.base_registry;
  $("#paths").textContent = `Settings: ${d.config_dir} · Logs: ${d.log_dir}`;
  gameDefaults = await invoke("game_defaults");
  fxDefaults = await invoke("fx_defaults");
  fillSettings(gameForm, cfg.game);
  renderFx();
  fillLauncher();
  setRunning(await invoke("is_running"));
  await select(cfg.accounts.some((a) => a.id === cfg.last_account) ? cfg.last_account : cfg.accounts[0]?.id ?? null);
  refreshVersions();
  foundInstalls = await invoke("detect_installs");
  await refreshGame();
  setBuilding(await invoke("is_building"));
  // first run, or something missing: straight to setup, at the step that needs doing
  if (!setupStatus.ready) {
    if (!cfg.game_path && foundInstalls.length === 1) await chooseFolder(foundInstalls[0]);
    await openSetup(setupStatus.build?.label ? 2 : 1);
  } else if (!cfg.accounts.length) {
    await openSetup(3);
  }
})();

// A new account on a LandSandBoat server, made from the form's name and password (as xiloader can)
$("#create-account").addEventListener("click", async () => {
  const a = account();
  const f = new FormData(accountForm);
  const login = (f.get("login") || "").trim();
  const password = f.get("password") || "";
  const again = $("#new-password-again").value;
  if (!login || !password) return setStatus("Fill in the account name and the password first.", true);
  if (password !== again) return setStatus("The two passwords are not the same.", true);
  const server = (f.get("server") || (a && a.server) || "").trim();
  const port = parseInt(f.get("auth_port"), 10) || 0;
  const button = $("#create-account");
  button.disabled = true;
  setStatus("Making the account…");
  try {
    const message = await invoke("create_account", { server, port, login, password });
    setStatus(message);
    $("#new-password-again").value = "";
    $("#new-account").open = false;
  } catch (e) {
    setStatus(String(e), true);
  } finally {
    button.disabled = false;
  }
});

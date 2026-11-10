let config = null;
let dirty = false;

const collapsedSensors  = new Set();
const collapsedChannels = new Set();

const $ = (id) => document.getElementById(id);

function setStatus(text, ok = true) {
    $("statusText").textContent = text;
    $("statusDot").style.background = ok ? "#22c55e" : "#ef4444";
}

function setDirty(v) {
    dirty = v;
    const el = $("dirtyIndicator");
    el.classList.toggle("dirty", v);
    el.textContent = v ? "Unsaved changes" : "No unsaved changes";
}

function switchTab(name) {
    document.querySelectorAll(".tab").forEach(t =>
        t.classList.toggle("active", t.dataset.tab === name));
    document.querySelectorAll(".tab-panel").forEach(p =>
        p.classList.toggle("active", p.id === "tab-" + name));

    if (name === "sensors") loadStage();
    if (name === "data") {
        refreshSdFiles();
        populateSensorFilter();
    }
}

// ---------- Load ----------

async function loadConfig() {
    try {
        setStatus("Loading...", true);
        const r = await fetch("/api/config");
        if (!r.ok) throw new Error("HTTP " + r.status);
        config = await r.json();

        config.device  = config.device  || {};
        config.network = config.network || {};
        config.sensors = config.sensors || [];

        // Normalize each sensor with new fields
        config.sensors.forEach(s => {
            if (s.sensor_enable === undefined) s.sensor_enable = true;
            if (s.sd_logging === undefined) s.sd_logging = false;
            if (s.poll_interval_ms === undefined) s.poll_interval_ms = 5000;
            if (!s.sensor_type) s.sensor_type = 'MODBUS';
            if (s.sensor_type === 'MODBUS') {
                s.modbus = s.modbus || {};
                if (s.modbus.batch_read === undefined) s.modbus.batch_read = false;
                if (!s.modbus.batch_start) s.modbus.batch_start = '0x0000';
                if (!s.modbus.batch_reg_type) s.modbus.batch_reg_type = 'HOLDING';
                if (s.modbus.batch_qty === undefined) s.modbus.batch_qty = 1;
            }
            if (s.sensor_type === 'SDI12') {
                s.sdi12 = s.sdi12 || {};
                if (!s.sdi12.address) s.sdi12.address = '0';
                if (!s.sdi12.data_unit) s.sdi12.data_unit = 'METER';
                if (s.sdi12.warmup_ms === undefined) s.sdi12.warmup_ms = 500;
                if (s.sdi12.retry_attempts === undefined) s.sdi12.retry_attempts = 3;
                if (s.sdi12.recv_timeout_ms === undefined) s.sdi12.recv_timeout_ms = 1500;
            }
        });

        collapsedSensors.clear();
        collapsedChannels.clear();
        config.sensors.forEach((s, i) => {
            if (i > 0) collapsedSensors.add(sensorKey(s, i));
            (s.channels || []).forEach((c, ci) => {
                if (ci > 0) collapsedChannels.add(channelKey(s, i, c, ci));
            });
        });

        render();
        setDirty(false);
        setStatus("Connected", true);
    } catch (e) {
        console.error(e);
        setStatus("Connection error", false);
        showMessage("Could not load configuration.");
    }
}

// ---------- Identity keys ----------

function sensorKey(s, i)  { return `s:${s.id ?? i}`; }
function channelKey(s, si, c, ci) { return `s:${s.id ?? si}:c:${c.id ?? ci}`; }

// ---------- Render ----------

function render() {
    if (!config) return;

    $("deviceId").value   = config.device.id || "";
    $("deviceName").value = config.device.name || "";
    $("deviceSubtitle").textContent =
        config.device.name ? config.device.name : "Device configuration";

    const n = config.network;
    $("apn").value        = n.apn        || "";
    $("apnUser").value    = n.apn_user   || "";
    $("apnPass").value    = n.apn_pass   || "";
    $("broker").value     = n.broker     || "";
    $("brokerPort").value = n.broker_port ?? 1883;
    $("mqttUser").value   = n.mqtt_user  || "";
    $("mqttPass").value   = n.mqtt_pass  || "";
    $("pubTopic").value   = n.pub_topic  || "";
    $("subTopic").value   = n.sub_topic  || "";
    $("ackTopic").value   = n.ack_topic  || "";

    renderSensors();
    updateSensorBadge();
    populateSensorFilter();
}

function updateSensorBadge() {
    const n = (config.sensors || []).length;
    $("tabSensorBadge").textContent = n;
    $("sensorCount").textContent = `${n} / 12 sensors configured`;
}

function renderSensors() {
    const container = $("sensors");
    container.innerHTML = "";

    const sensors = config.sensors || [];

    if (sensors.length === 0) {
        container.innerHTML = `
            <div class="empty">
                <strong>No sensors configured</strong>
                Click <em>+ Add Sensor</em> to create your first sensor.
            </div>`;
        return;
    }

    sensors.forEach((sensor, si) => {
        container.appendChild(buildSensorCard(sensor, si));
    });
}

function buildSensorCard(sensor, si) {
    const key = sensorKey(sensor, si);
    const isCollapsed = collapsedSensors.has(key);
    const enabled = sensor.sensor_enable !== false;

    const div = document.createElement("div");
    div.className = "sensor" + (isCollapsed ? " collapsed" : "") + (enabled ? "" : " disabled");
    div.dataset.sensorKey = key;

    const channels = sensor.channels || [];
    const typeLabel = (sensor.sensor_type || 'MODBUS').toUpperCase();

    // Meta pills
    const metaPills = [
        `<span class="sensor-type-badge">${typeLabel}</span>`,
        `<span class="pill pill-info">${channels.length} ch</span>`
    ];
    if (sensor.sensor_type === 'MODBUS' && sensor.modbus?.batch_read) {
        metaPills.push(`<span class="pill pill-warn">Batch</span>`);
    }

    div.innerHTML = `
        <div class="sensor-head" onclick="toggleSensor('${key}')">
            <div class="sensor-head-left">
                <span class="sensor-toggle">▶</span>
                <span class="sensor-num">S${sensor.id ?? si + 1}</span>
                <div style="min-width:0;">
                    <div class="sensor-title-row">
                        <div class="sensor-title">${esc(sensor.name || "Unnamed sensor")}</div>
                    </div>
                    <div class="sensor-meta">
                        ${metaPills.join('')}
                    </div>
                </div>
            </div>
            <div class="sensor-head-right" onclick="event.stopPropagation()">
                <div class="sensor-toggles">
                    <label class="toggle-switch compact" title="SD Card Logging">
                        <input type="checkbox" ${sensor.sd_logging ? 'checked' : ''}
                            onchange="setSensor(${si}, 'sd_logging', this.checked)">
                        <span class="slider"></span>
                        <span>SD</span>
                    </label>
                    <label class="toggle-switch compact" title="Enable / Disable Sensor">
                        <input type="checkbox" ${enabled ? 'checked' : ''}
                            onchange="setSensor(${si}, 'enabled', this.checked)">
                        <span class="slider"></span>
                        <span>${enabled ? 'ON' : 'OFF'}</span>
                    </label>
                    <button class="icon-btn danger"
                        title="Delete sensor"
                        onclick="deleteSensor(${si})">✕</button>
                </div>
            </div>
        </div>

        ${enabled ? `
        <div class="sensor-body" ${isCollapsed ? 'style="display:none"' : ""}>
            ${sensorBody(sensor, si)}
        </div>` : ``}
    `;

    return div;
}

function sensorBody(sensor, si) {
    const st = sensor.sensor_type || 'MODBUS';
    const modbus = sensor.modbus || {};

    let html = `
        <div class="group">
            <div class="group-title">Basic Setup</div>
            <div class="grid">
                <label class="field">
                    <span class="field-label">Sensor ID</span>
                    <input type="number" min="1"
                        value="${sensor.id ?? si + 1}"
                        oninput="setSensor(${si}, 'id', Number(this.value))">
                </label>
                <label class="field">
                    <span class="field-label">Sensor Name</span>
                    <input value="${esc(sensor.name || '')}"
                        placeholder="e.g. Weather Station"
                        oninput="setSensor(${si}, 'name', this.value)">
                </label>
                <label class="field">
                    <span class="field-label">Sensor Type</span>
                    <select onchange="setSensor(${si}, 'sensor_type', this.value)">
                        ${enumOptions(
                            ["MODBUS", "SDI12", "SPI", "ANALOG"],
                            st)}
                    </select>
                </label>
                <label class="field">
                    <span class="field-label">Poll Interval (ms)</span>
                    <input type="number" min="100" step="100"
                        value="${sensor.poll_interval_ms ?? 5000}"
                        oninput="setSensor(${si}, 'poll_interval_ms', Number(this.value))">
                </label>
            </div>
        </div>
    `;

    if (st === 'MODBUS') {
        html += modbusSection(sensor, si, modbus);
        html += channelsSection(sensor, si);
    } else if (st === 'SDI12') {
        html += sdi12Section(sensor, si, sensor.sdi12 || {});
        html += sdi12ChannelsSection(sensor, si);
    } else {
        html += `
            <div class="group">
                <div class="group-title">${st} Settings</div>
                <div class="empty" style="padding:20px;">
                    Configuration for <strong>${st}</strong> will appear here.
                </div>
            </div>
        `;
    }

    return html;
}

// ---------- MODBUS ----------

function modbusSection(sensor, si, modbus) {
    const batchRead = modbus.batch_read || false;

    return `
        <div class="group">
            <div class="group-title">Modbus RTU — Serial</div>
            <div class="grid cols-3">
                <label class="field">
                    <span class="field-label">Slave ID</span>
                    <input type="number" min="0" max="255"
                        value="${modbus.slave_id ?? 1}"
                        oninput="setSensorModbus(${si}, 'slave_id', Number(this.value))">
                </label>
                <label class="field">
                    <span class="field-label">Baudrate</span>
                    <select onchange="setSensorModbus(${si}, 'baud', Number(this.value))">
                        ${baudOptions(modbus.baud)}
                    </select>
                </label>
                <label class="field">
                    <span class="field-label">Parity</span>
                    <select onchange="setSensorModbus(${si}, 'parity', this.value)">
                        ${enumOptions(["NONE","EVEN","ODD"], modbus.parity || "NONE")}
                    </select>
                </label>
                <label class="field">
                    <span class="field-label">Data Bits</span>
                    <select onchange="setSensorModbus(${si}, 'data_bits', Number(this.value))">
                        ${numOptions([7,8], modbus.data_bits ?? 8)}
                    </select>
                </label>
                <label class="field">
                    <span class="field-label">Stop Bits</span>
                    <select onchange="setSensorModbus(${si}, 'stop_bits', Number(this.value))">
                        ${numOptions([1,2], modbus.stop_bits ?? 1)}
                    </select>
                </label>
                <label class="field">
                    <span class="field-label">Retry Attempts</span>
                    <input type="number" min="0" max="10"
                        value="${modbus.retry_attempts ?? 3}"
                        oninput="setSensorModbus(${si}, 'retry_attempts', Number(this.value))">
                </label>
                <label class="field">
                    <span class="field-label">Receive Timeout (ms)</span>
                    <input type="number" min="100" step="50"
                        value="${modbus.recv_timeout_ms ?? 1000}"
                        oninput="setSensorModbus(${si}, 'recv_timeout_ms', Number(this.value))">
                </label>
                <label class="field">
                    <span class="field-label">Min Interval (ms)</span>
                    <input type="number" min="0" step="10"
                        value="${modbus.min_interval_ms ?? 100}"
                        oninput="setSensorModbus(${si}, 'min_interval_ms', Number(this.value))">
                </label>
            </div>
        </div>

        <div class="group">
            <div class="group-title">Batch Read</div>
            <div class="grid">
                <label class="field" style="flex-direction:row; align-items:center; gap:10px;">
                    <span class="field-label">Enable Batch Read</span>
                    <label class="toggle-switch">
                        <input type="checkbox" ${batchRead ? 'checked' : ''}
                            onchange="setSensorModbus(${si}, 'batch_read', this.checked)">
                        <span class="slider"></span>
                    </label>
                </label>
            </div>
            <div class="grid cols-3" style="margin-top:12px; ${batchRead ? '' : 'opacity:0.4; pointer-events:none;'}">
                <label class="field">
                    <span class="field-label">Start Address</span>
                    <input value="${esc(modbus.batch_start || '0x0000')}"
                        placeholder="0x0000"
                        style="font-family:monospace;"
                        oninput="setSensorModbus(${si}, 'batch_start', this.value)"
                        ${batchRead ? '' : 'disabled'}>
                </label>
                <label class="field">
                    <span class="field-label">Register Type</span>
                    <select onchange="setSensorModbus(${si}, 'batch_reg_type', this.value)"
                        ${batchRead ? '' : 'disabled'}>
                        ${enumOptions(["HOLDING","INPUT","COIL","DISCRETE"],
                            modbus.batch_reg_type || "HOLDING")}
                    </select>
                </label>
                <label class="field">
                    <span class="field-label">Quantity</span>
                    <input type="number" min="1" max="125"
                        value="${modbus.batch_qty ?? 1}"
                        oninput="setSensorModbus(${si}, 'batch_qty', Number(this.value))"
                        ${batchRead ? '' : 'disabled'}>
                </label>
            </div>
        </div>
    `;
}

// ---------- SDI-12 ----------

function sdi12Section(sensor, si, sdi) {
    const units = ["METER", "FOOT", "MILLIMETER", "CENTIMETER", "INCH"];
    const addrs = Array.from({length: 10}, (_, i) => String(i));  // '0'..'9'

    return `
        <div class="group">
            <div class="group-title">SDI-12 — Bus</div>
            <div class="grid cols-3">
                <label class="field">
                    <span class="field-label">Sensor Address</span>
                    <select onchange="setSensorSdi12(${si}, 'address', this.value)">
                        ${addrs.map(a =>
                            `<option value="${a}" ${a === (sdi.address ?? '0') ? 'selected' : ''}>${a}</option>`
                        ).join('')}
                    </select>
                    <small class="field-help">0–9 (single character on the bus).</small>
                </label>
                <label class="field">
                    <span class="field-label">Data Unit</span>
                    <select onchange="setSensorSdi12(${si}, 'data_unit', this.value)">
                        ${units.map(u =>
                            `<option value="${u}" ${u === (sdi.data_unit || 'METER') ? 'selected' : ''}>${u}</option>`
                        ).join('')}
                    </select>
                    <small class="field-help">Unit reported by the sensor.</small>
                </label>
                <label class="field">
                    <span class="field-label">Warm-up (ms)</span>
                    <input type="number" min="0" step="50"
                        value="${sdi.warmup_ms ?? 500}"
                        oninput="setSensorSdi12(${si}, 'warmup_ms', Number(this.value))">
                </label>
                <label class="field">
                    <span class="field-label">Retry Attempts</span>
                    <input type="number" min="0" max="10"
                        value="${sdi.retry_attempts ?? 3}"
                        oninput="setSensorSdi12(${si}, 'retry_attempts', Number(this.value))">
                </label>
                <label class="field">
                    <span class="field-label">Receive Timeout (ms)</span>
                    <input type="number" min="100" step="50"
                        value="${sdi.recv_timeout_ms ?? 1500}"
                        oninput="setSensorSdi12(${si}, 'recv_timeout_ms', Number(this.value))">
                </label>
            </div>
            <div class="empty" style="padding:14px; margin-top:12px;">
                SDI-12 returns a comma/plus-separated measurement string
                (e.g. <code>0+48.008+3.043+0.000014+44+16+12.2</code>).
                Map each index to a channel below.
            </div>
        </div>
    `;
}

function setSensorSdi12(si, key, value) {
    const sensor = config.sensors[si];
    if (!sensor.sdi12) sensor.sdi12 = {};
    sensor.sdi12[key] = value;
    setDirty(true);
    refreshSensorHeader(si);
}

// ---------- SDI-12 Channels (value-index mapping) ----------

function sdi12ChannelsSection(sensor, si) {
    const channels = sensor.channels || [];
    const canAdd = channels.length < 12;

    // return `
    //     <div class="channels-header">
    //         <strong>SDI-12 Values (${channels.length} / 12)</strong>
    //         <button onclick="addChannel(${si})" ${canAdd ? "" : "disabled"}>
    //             + Add Value
    //         </button>
    //     </div>
    //     <div id="channels-${si}">
    //         ${
    //             channels.length === 0
    //                 ? `<div class="empty" style="padding:20px;">
    //                        No values mapped. Add one to read a field from the SDI-12 response.
    //                    </div>`
    //                 : channels.map((c, ci) => sdiChannelHtml(sensor, si, c, ci)).join("")
    //         }
    //     </div>
    // `;
}

function sdiChannelHtml(sensor, si, ch, ci) {
    const key = channelKey(sensor, si, ch, ci);
    const isCollapsed = collapsedChannels.has(key);

    const idx = (ch.value_index ?? ci);
    const meta =
        `idx ${idx}` +
        ` × ${ch.scale_factor ?? 1}` +
        (ch.offset ? ` +${ch.offset}` : '') +
        (ch.unit ? ` [${ch.unit}]` : '');

    return `
        <div class="channel${isCollapsed ? " collapsed" : ""}" data-channel-key="${key}">
            <div class="channel-head" onclick="toggleChannel('${key}')">
                <div class="channel-head-left">
                    <span class="channel-num">V${ch.id ?? ci + 1}</span>
                    <div style="min-width:0;">
                        <div class="channel-alias">${esc(ch.alias || "Unnamed value")}</div>
                        <div class="channel-meta">${esc(meta)}</div>
                    </div>
                </div>
                <div onclick="event.stopPropagation()">
                    <button class="icon-btn danger"
                        title="Delete value"
                        onclick="deleteChannel(${si}, ${ci})">✕</button>
                </div>
            </div>

            <div class="channel-body" ${isCollapsed ? 'style="display:none"' : ""}>
                <div class="channel-grid">
                    <label class="field">
                        <span class="field-label">Channel ID</span>
                        <input type="number" min="1"
                            value="${ch.id ?? ci + 1}"
                            oninput="setChannel(${si}, ${ci}, 'id', Number(this.value))">
                    </label>
                    <label class="field">
                        <span class="field-label">Alias</span>
                        <input value="${esc(ch.alias || '')}"
                            placeholder="e.g. Distance"
                            oninput="setChannel(${si}, ${ci}, 'alias', this.value)">
                    </label>
                    <label class="field">
                        <span class="field-label">Unit</span>
                        <input value="${esc(ch.unit || '')}"
                            placeholder="e.g. m, °C, V"
                            oninput="setChannel(${si}, ${ci}, 'unit', this.value)">
                    </label>
                    <label class="field">
                        <span class="field-label">Value Index</span>
                        <input type="number" min="0"
                            value="${idx}"
                            oninput="setChannel(${si}, ${ci}, 'value_index', Number(this.value))">
                        <small class="field-help">Position in the SDI-12 value list (0-based).</small>
                    </label>
                    <label class="field">
                        <span class="field-label">Scale Factor</span>
                        <input type="number" step="any"
                            value="${ch.scale_factor ?? 1}"
                            oninput="setChannel(${si}, ${ci}, 'scale_factor', Number(this.value))">
                    </label>
                    <label class="field">
                        <span class="field-label">Offset</span>
                        <input type="number" step="any"
                            value="${ch.offset ?? 0}"
                            oninput="setChannel(${si}, ${ci}, 'offset', Number(this.value))">
                        <small class="field-help">Added after scaling.</small>
                    </label>
                    <label class="field field-wide">
                        <span class="field-label">Description</span>
                        <input value="${esc(ch.description || '')}"
                            placeholder="Optional description"
                            oninput="setChannel(${si}, ${ci}, 'description', this.value)">
                    </label>
                </div>
            </div>
        </div>
    `;
}

// ---------- MODBUS Channels ----------

function channelsSection(sensor, si) {
    const channels = sensor.channels || [];
    const canAdd = channels.length < 12;

    return `
        <div class="channels-header">
            <strong>Channels (${channels.length} / 12)</strong>
            <button onclick="addChannel(${si})" ${canAdd ? "" : "disabled"}>
                + Add Channel
            </button>
        </div>
        <div id="channels-${si}">
            ${
                channels.length === 0
                    ? `<div class="empty" style="padding:20px;">
                           No channels. Add one to start reading values.
                       </div>`
                    : channels.map((c, ci) => channelHtml(sensor, si, c, ci)).join("")
            }
        </div>
    `;
}

function channelHtml(sensor, si, ch, ci) {
    const key = channelKey(sensor, si, ch, ci);
    const isCollapsed = collapsedChannels.has(key);

    const regAddr = ch.register_address || ch.register_number || "0x0000";
    const meta =
        `${ch.data_type || "INT16"}` +
        ` @ ${regAddr}` +
        ` × ${ch.scale_factor ?? 1}` +
        (ch.offset ? ` +${ch.offset}` : '') +
        (ch.unit ? ` [${ch.unit}]` : '');

    return `
        <div class="channel${isCollapsed ? " collapsed" : ""}" data-channel-key="${key}">
            <div class="channel-head" onclick="toggleChannel('${key}')">
                <div class="channel-head-left">
                    <span class="channel-num">C${ch.id ?? ci + 1}</span>
                    <div style="min-width:0;">
                        <div class="channel-alias">${esc(ch.alias || "Unnamed channel")}</div>
                        <div class="channel-meta">${esc(meta)}</div>
                    </div>
                </div>
                <div onclick="event.stopPropagation()">
                    <button class="icon-btn danger"
                        title="Delete channel"
                        onclick="deleteChannel(${si}, ${ci})">✕</button>
                </div>
            </div>

            <div class="channel-body" ${isCollapsed ? 'style="display:none"' : ""}>
                <div class="channel-grid">
                    <label class="field">
                        <span class="field-label">Channel ID</span>
                        <input type="number" min="1"
                            value="${ch.id ?? ci + 1}"
                            oninput="setChannel(${si}, ${ci}, 'id', Number(this.value))">
                    </label>
                    <label class="field">
                        <span class="field-label">Alias</span>
                        <input value="${esc(ch.alias || '')}"
                            placeholder="e.g. Temperature"
                            oninput="setChannel(${si}, ${ci}, 'alias', this.value)">
                    </label>
                    <label class="field">
                        <span class="field-label">Description</span>
                        <input value="${esc(ch.description || '')}"
                            placeholder="Optional description"
                            oninput="setChannel(${si}, ${ci}, 'description', this.value)">
                    </label>
                    <label class="field">
                        <span class="field-label">Unit</span>
                        <input value="${esc(ch.unit || '')}"
                            placeholder="e.g. °C, %RH, V"
                            oninput="setChannel(${si}, ${ci}, 'unit', this.value)">
                    </label>
                    <label class="field">
                        <span class="field-label">Register Address (hex)</span>
                        <input value="${esc(regAddr)}"
                            placeholder="0x0001"
                            style="font-family:monospace;"
                            oninput="setChannel(${si}, ${ci}, 'register_address', this.value)">
                        <small class="field-help">e.g. 0x0000, 0x03E8</small>
                    </label>
                    <label class="field">
                        <span class="field-label">Register Type</span>
                        <select onchange="setChannel(${si}, ${ci}, 'register_type', this.value)">
                            ${enumOptions(
                                ["HOLDING","INPUT","COIL","DISCRETE"],
                                ch.register_type || "HOLDING")}
                        </select>
                    </label>
                    <label class="field">
                        <span class="field-label">Data Type</span>
                        <select onchange="setChannel(${si}, ${ci}, 'data_type', this.value)">
                            ${enumOptions(
                                ["UINT16","INT16","UINT32","INT32",
                                 "UINT64","INT64","FLOAT32","FLOAT64"],
                                ch.data_type || "INT16")}
                        </select>
                    </label>
                    <label class="field">
                        <span class="field-label">Scale Factor</span>
                        <input type="number" step="any"
                            value="${ch.scale_factor ?? 1}"
                            oninput="setChannel(${si}, ${ci}, 'scale_factor', Number(this.value))">
                    </label>
                    <label class="field">
                        <span class="field-label">Offset</span>
                        <input type="number" step="any"
                            value="${ch.offset ?? 0}"
                            oninput="setChannel(${si}, ${ci}, 'offset', Number(this.value))">
                        <small class="field-help">Added after scaling.</small>
                    </label>
                    <label class="field">
                        <span class="field-label">Register Width</span>
                        <select onchange="setChannel(${si}, ${ci}, 'register_width', Number(this.value))">
                            ${numOptions([1,2,4], ch.register_width ?? 1)}
                        </select>
                    </label>
                    <label class="field">
                        <span class="field-label">Bit Index</span>
                        <input type="number" min="-1" max="63"
                            value="${ch.bit_index ?? -1}"
                            oninput="setChannel(${si}, ${ci}, 'bit_index', Number(this.value))">
                        <small class="field-help">-1 = use entire register.</small>
                    </label>
                    <label class="field field-wide">
                        <span class="field-label">Byte Order</span>
                        <select onchange="setChannel(${si}, ${ci}, 'byte_order', this.value)">
                            ${enumOptions(
                                ["BIG_ENDIAN","LITTLE_ENDIAN",
                                 "BIG_ENDIAN_BYTE_SWAP","LITTLE_ENDIAN_BYTE_SWAP"],
                                ch.byte_order || "BIG_ENDIAN")}
                        </select>
                    </label>
                </div>
            </div>
        </div>
    `;
}

// ---------- Option helpers ----------

function enumOptions(list, selected) {
    return list.map(v =>
        `<option value="${v}" ${v === selected ? "selected" : ""}>${v}</option>`
    ).join("");
}
function numOptions(list, selected) {
    return list.map(v =>
        `<option value="${v}" ${Number(v) === Number(selected) ? "selected" : ""}>${v}</option>`
    ).join("");
}
function baudOptions(selected) {
    return numOptions([1200,2400,4800,9600,19200,38400,57600,115200],
                      selected ?? 9600);
}

// ---------- Collapse / expand ----------

function toggleSensor(key) {
    if (collapsedSensors.has(key)) collapsedSensors.delete(key);
    else                           collapsedSensors.add(key);

    const card = document.querySelector(`[data-sensor-key="${key}"]`);
    if (card) {
        card.classList.toggle("collapsed");
        const body = card.querySelector(".sensor-body");
        if (body) {
            body.style.display =
                card.classList.contains("collapsed") ? "none" : "";
        }
    }
}

function toggleChannel(key) {
    if (collapsedChannels.has(key)) collapsedChannels.delete(key);
    else                            collapsedChannels.add(key);

    const el = document.querySelector(`[data-channel-key="${key}"]`);
    if (el) {
        el.classList.toggle("collapsed");
        el.querySelector(".channel-body").style.display =
            el.classList.contains("collapsed") ? "none" : "";
    }
}

function collapseAllSensors() {
    config.sensors.forEach((s, i) => collapsedSensors.add(sensorKey(s, i)));
    config.sensors.forEach((s, si) =>
        (s.channels || []).forEach((c, ci) =>
            collapsedChannels.add(channelKey(s, si, c, ci))));
    renderSensors();
}

// ---------- Mutators ----------

function setSensor(si, key, value) {
    config.sensors[si][key] = value;
    setDirty(true);

    if (key === 'sensor_type' || key === 'enabled') {
        renderSensors();
        updateSensorBadge();
        return;
    }

    refreshSensorHeader(si);
}

function setSensorModbus(si, key, value) {
    const sensor = config.sensors[si];
    if (!sensor.modbus) sensor.modbus = {};
    sensor.modbus[key] = value;
    setDirty(true);

    if (key === 'batch_read') {
        renderSensors();
        return;
    }

    refreshSensorHeader(si);
}

function setChannel(si, ci, key, value) {
    config.sensors[si].channels[ci][key] = value;
    setDirty(true);
    refreshChannelHeader(si, ci);
}

function refreshSensorHeader(si) {
    const sensor = config.sensors[si];
    const card = document.querySelector(`[data-sensor-key="${sensorKey(sensor, si)}"]`);
    if (!card) return;

    const title = card.querySelector(".sensor-title");
    if (title) title.textContent = sensor.name || "Unnamed sensor";

    const meta = card.querySelector(".sensor-meta");
    if (meta) {
        const channels = sensor.channels || [];
        const typeLabel = (sensor.sensor_type || 'MODBUS').toUpperCase();

        const pills = [
            `<span class="sensor-type-badge">${typeLabel}</span>`,
            `<span class="pill pill-info">${channels.length} ch</span>`
        ];
        if (sensor.sensor_type === 'MODBUS' && sensor.modbus?.batch_read) {
            pills.push(`<span class="pill pill-warn">Batch</span>`);
        }

        meta.innerHTML = pills.join('');
    }
}

function refreshChannelHeader(si, ci) {
    const sensor = config.sensors[si];
    const ch = sensor.channels[ci];
    const key = channelKey(sensor, si, ch, ci);
    const el = document.querySelector(`[data-channel-key="${key}"]`);
    if (!el) return;

    const alias = el.querySelector(".channel-alias");
    if (alias) alias.textContent = ch.alias || "Unnamed channel";

    const meta = el.querySelector(".channel-meta");
    if (meta) {
        if (sensor.sensor_type === 'SDI12') {
            meta.textContent =
                `idx ${ch.value_index ?? ci}` +
                ` × ${ch.scale_factor ?? 1}` +
                (ch.offset ? ` +${ch.offset}` : '') +
                (ch.unit ? ` [${ch.unit}]` : '');
        } else {
            const regAddr = ch.register_address || ch.register_number || "0x0000";
            meta.textContent =
                `${ch.data_type || "INT16"}` +
                ` @ ${regAddr}` +
                ` × ${ch.scale_factor ?? 1}` +
                (ch.offset ? ` +${ch.offset}` : '') +
                (ch.unit ? ` [${ch.unit}]` : '');
        }
    }
}

function addSensor() {
    if (config.sensors.length >= 12) {
        showMessage("Maximum 12 sensors.");
        return;
    }
    const s = {
        id: nextSensorId(),
        name: "New Sensor",

        sensor_enable: true,
        sd_logging: false,
        poll_interval_ms: 5000,
        sensor_type: "MODBUS",
        modbus: {
            slave_id: 1,
            baud: 9600,
            parity: "NONE",
            data_bits: 8,
            stop_bits: 1,
            retry_attempts: 3,
            recv_timeout_ms: 1000,
            min_interval_ms: 100,
            batch_read: false,
            batch_start: "0x0000",
            batch_reg_type: "HOLDING",
            batch_qty: 1
        },
        sdi12: {
            address: "0",
            data_unit: "METER",
            warmup_ms: 500,
            retry_attempts: 3,
            recv_timeout_ms: 1500
        },
        channels: []
    };
    config.sensors.push(s);
    setDirty(true);
    renderSensors();
    updateSensorBadge();
}

function addChannel(si) {
    const s = config.sensors[si];
    s.channels = s.channels || [];
    if (s.channels.length >= 12) {
        showMessage("Maximum 12 channels per sensor.");
        return;
    }

    const isSdi = (s.sensor_type === 'SDI12');
    const newCh = {
        id: nextChannelId(s),
        alias: isSdi ? "New Value" : "New Channel",
        description: "",
        unit: "",
        scale_factor: 1,
        offset: 0
    };

    if (isSdi) {
        newCh.value_index = s.channels.length;
    } else {
        Object.assign(newCh, {
            register_address: "0x00",
            register_type: "HOLDING",
            data_type: "INT16",
            register_width: 1,
            bit_index: -1,
            byte_order: "BIG_ENDIAN"
        });
    }

    s.channels.push(newCh);
    setDirty(true);

    const sensor = config.sensors[si];
    const card = document.querySelector(`[data-sensor-key="${sensorKey(sensor, si)}"]`);
    if (card) {
        const body = card.querySelector(".sensor-body");
        if (body) body.innerHTML = sensorBody(sensor, si);
    }
    updateSensorBadge();
}

async function deleteSensor(si) {
    if (!await showDeleteConfirm("Delete this sensor and all its channels?")) return;
    const key = sensorKey(config.sensors[si], si);
    collapsedSensors.delete(key);
    config.sensors.splice(si, 1);
    setDirty(true);
    renderSensors();
    updateSensorBadge();
}

async function deleteChannel(si, ci) {
    if (!await showDeleteConfirm("Delete this channel?")) return;
    const sensor = config.sensors[si];
    const key = channelKey(sensor, si, sensor.channels[ci], ci);
    collapsedChannels.delete(key);
    sensor.channels.splice(ci, 1);
    setDirty(true);

    const card = document.querySelector(`[data-sensor-key="${sensorKey(sensor, si)}"]`);
    if (card) {
        const body = card.querySelector(".sensor-body");
        if (body) body.innerHTML = sensorBody(sensor, si);
    }
    updateSensorBadge();
}

function nextSensorId() {
    const ids = config.sensors.map(s => Number(s.id));
    let id = 1;
    while (ids.includes(id)) id++;
    return id;
}
function nextChannelId(sensor) {
    const ids = (sensor.channels || []).map(c => Number(c.id));
    let id = 1;
    while (ids.includes(id)) id++;
    return id;
}

function showMessage(message, success = true) {
    const msg = document.getElementById("message");
    msg.textContent = message;
    msg.className = success ? "message success" : "message error";
    msg.style.display = "block";
    setTimeout(() => { msg.style.display = "none"; }, 3000);
}

function showDeleteConfirm(message) {
    return new Promise(resolve => {
        const box = document.createElement("div");
        box.style.cssText = `
            position: fixed; top: 50%; left: 50%;
            transform: translate(-50%, -50%);
            background: white; padding: 20px; border-radius: 8px;
            box-shadow: 0 5px 25px rgba(0,0,0,.3);
            z-index: 10000; min-width: 300px; text-align: center;
        `;
        box.innerHTML = `
            <div style="margin-bottom:15px;">${message}</div>
            <button class="secondary" id="cancelDelete">Cancel</button>
            <button class="danger" id="confirmDelete">Delete</button>
        `;
        document.body.appendChild(box);
        box.querySelector("#cancelDelete").onclick = () => { box.remove(); resolve(false); };
        box.querySelector("#confirmDelete").onclick = () => { box.remove(); resolve(true); };
    });
}

// ---------- Data Retrieve ----------

function populateSensorFilter() {
    const sel = $("dataSensorFilter");
    if (!sel) return;
    if (!sel || !config) return;
    const current = sel.value;
    sel.innerHTML = `<option value="">All sensors</option>` +
        config.sensors.map(s =>
            `<option value="${esc(s.name || '')}">${esc(s.name || `Sensor ${s.id}`)}</option>`
        ).join("");
    sel.value = current;
}

async function refreshSdFiles() {
    const box = $("sdFileList");
    if (!box) return;
    box.innerHTML = `<span class="sd-file-chip empty-chip">Loading files…</span>`;

    try {
        const r = await fetch("/api/files");
        if (!r.ok) throw new Error("HTTP " + r.status);
        const data = await r.json();
        const files = data.files || [];
        if (files.length === 0) {
            box.innerHTML = `<span class="sd-file-chip empty-chip">No log files on SD card</span>`;
            return;
        }
        box.innerHTML = files.map(f => {
            const name = typeof f === "string" ? f : (f.name || "");
            const size = typeof f === "string" ? "" : formatSize(f.size);
            return `<span class="sd-file-chip" title="Preview this file"
                        onclick="loadFileIntoRange('${esc(name)}')"
                        style="cursor:pointer;">
                        📄 ${esc(name)}${size ? " · " + size : ""}
                    </span>`;
        }).join("");
    } catch (e) {
        box.innerHTML = `<span class="sd-file-chip empty-chip">Could not list files</span>`;
    }
}

function loadFileIntoRange(name) {
    // name looks like "202601.csv" -> set date range to that whole month
    const m = name.match(/^(\d{4})(\d{2})/);
    if (!m) return;
    const y = m[1], mo = m[2];
    $("dataFrom").value = `${y}-${mo}-01`;
    const last = new Date(Number(y), Number(mo), 0).getDate();  // last day
    $("dataTo").value = `${y}-${mo}-${String(last).padStart(2,'0')}`;
    previewData();
}

function formatSize(bytes) {
    if (!bytes || bytes < 0) return "";
    if (bytes < 1024) return bytes + " B";
    if (bytes < 1024 * 1024) return (bytes / 1024).toFixed(1) + " KB";
    return (bytes / (1024 * 1024)).toFixed(2) + " MB";
}

function getDataQuery() {
    const from = $("dataFrom").value;
    const to   = $("dataTo").value;
    if (!from || !to) {
        showMessage("Select both start and end dates.", false);
        return null;
    }
    if (from > to) {
        showMessage("Start date must be before end date.", false);
        return null;
    }
    const params = new URLSearchParams();
    params.set("from", from);
    params.set("to", to);
    const sensorEl = $("dataSensorFilter");
    if (sensorEl && sensorEl.value) params.set("sensor", sensorEl.value);
    return params;
}

async function previewData() {
    const params = getDataQuery();
    if (!params) return;
    params.set("limit", "200");
    params.set("format", "json");

    const info = $("previewInfo");
    info.textContent = "Loading…";

    try {
        const r = await fetch("/api/data?" + params.toString());
        if (!r.ok) throw new Error("HTTP " + r.status);
        const ct = r.headers.get("content-type") || "";

        if (params.get("format") === "json" || ct.includes("json")) {
            const data = await r.json();
            renderPreviewTable(data.rows || [], data.headers || null);
            info.textContent = `${(data.rows || []).length} row(s) shown`;
        } else {
            const text = await r.text();
            renderPreviewCsv(text);
            const rows = Math.max(0, text.trim().split(/\r?\n/).length - 1);
            info.textContent = `${rows} row(s) shown`;
        }
    } catch (e) {
        info.textContent = "";
        showMessage("Preview failed: " + e.message, false);
    }
}

function renderPreviewCsv(csvText) {
    const lines = csvText.trim().split(/\r?\n/).filter(l => l.length);
    if (!lines.length) {
        renderPreviewTable([], null);
        return;
    }
    const headers = lines[0].split("\t").length > 1
        ? lines[0].split("\t")
        : lines[0].split(",");
    const sep = lines[0].includes("\t") ? "\t" : ",";
    const rows = lines.slice(1).map(l => l.split(sep));
    renderPreviewTable(rows, headers);
}

function renderPreviewTable(rows, headers) {
    const table  = $("previewTable");
    const thead  = table.querySelector("thead");
    const tbody  = table.querySelector("tbody");

    if (!rows.length) {
        thead.innerHTML = "";
        tbody.innerHTML = "";
        table.classList.remove("has-data");
        $("previewEmpty").style.display = "";
        return;
    }

    if (!headers) {
        headers = ["timestamp","device_id","AOS","LOS","csq","fwv","hwv",
                   "heap","uptime","solar_voltage","battery_voltage",
                   "sensor_name","raw_data"];
    }

    thead.innerHTML = `<tr>${headers.map(h => `<th>${esc(h)}</th>`).join("")}</tr>`;
    tbody.innerHTML = rows.map(row =>
        `<tr>${row.map(v => `<td>${esc(v)}</td>`).join("")}</tr>`
    ).join("");

    table.classList.add("has-data");
    $("previewEmpty").style.display = "none";
}

async function downloadData() {
    const params = getDataQuery();
    if (!params) return;

    const btns = document.querySelectorAll(".data-actions button");
    btns.forEach(b => b.disabled = true);
    const info = $("previewInfo");
    info.textContent = "Preparing file on device…";

    try {
        // Step 1: device filters the range into a temp file on the SD card.
        const r = await fetch("/api/retrieve?" + params.toString(), { cache: "no-store" });
        const d = await r.json().catch(() => ({}));
        if (!r.ok || !d.ok) throw new Error(d.error || ("HTTP " + r.status));
        if (!d.records) {
            info.textContent = "";
            showMessage("No records found in the selected range.", false);
            return;
        }
        // Step 2: browser downloads the finished file directly.
        info.textContent = `${d.records} row(s), ${formatSize(d.bytes)} — downloading…`;
        const a = document.createElement("a");
        a.href = d.downloadUrl || "/api/retrieve/download";
        a.download = d.filename || "data.csv";
        document.body.appendChild(a);
        a.click();
        a.remove();
        showMessage("Download started.");
    } catch (e) {
        info.textContent = "";
        showMessage("Download failed: " + e.message, false);
    } finally {
        btns.forEach(b => b.disabled = false);
    }
}

function downloadSingleFile(name) {
    // Strip a leading slash if any; the server adds it back.
    const clean = name.replace(/^\//, "");
    const url = "/api/sd/download?file=" + encodeURIComponent(clean);
    const a = document.createElement("a");
    a.href = url;
    a.download = clean.split("/").pop();
    document.body.appendChild(a);
    a.click();
    a.remove();
    showMessage("Download started.");
}

async function loadSDFiles() {
    const box = $("sdFiles");
    const status = $("sdStatus");
    if (!box) return;
    box.innerHTML = `<span class="sd-file-chip empty-chip">Loading…</span>`;
    try {
        const r = await fetch("/api/sd/files");
        if (!r.ok) throw new Error("HTTP " + r.status);
        const files = await r.json();
        if (!files.length) {
            box.innerHTML = `<span class="sd-file-chip empty-chip">No files</span>`;
            status.textContent = "SD card mounted — empty";
            return;
        }
        status.textContent = `SD card mounted — ${files.length} file(s)`;
        box.innerHTML = files.map(f => {
            const n = f.name.replace(/^\//, "");
            return `<span class="sd-file-chip">📄 ${esc(n)} · ${formatSize(f.size)}
                <button class="icon-btn" onclick="downloadSingleFile('${esc(n)}')">⬇</button>
                <button class="icon-btn danger" onclick="deleteSdFile('${esc(n)}')">✕</button>
            </span>`;
        }).join("");
    } catch (e) {
        box.innerHTML = `<span class="sd-file-chip empty-chip">Error: ${esc(e.message)}</span>`;
        status.textContent = "SD card error";
    }
}

// ---------- Save / reset ----------

// Each sensor is saved with only the settings of its own protocol.
function configForSave() {
    const out = JSON.parse(JSON.stringify(config));
    (out.sensors || []).forEach(s => {
        if (s.sensor_type === 'SDI12') delete s.modbus;
        else delete s.sdi12;
    });
    return out;
}

async function saveConfig() {
    if (!config) return;

    config.device.id   = $("deviceId").value;
    config.device.name = $("deviceName").value;

    config.network = {
        apn:         $("apn").value,
        apn_user:    $("apnUser").value,
        apn_pass:    $("apnPass").value,
        broker:      $("broker").value,
        broker_port: Number($("brokerPort").value),
        mqtt_user:   $("mqttUser").value,
        mqtt_pass:   $("mqttPass").value,
        pub_topic:   $("pubTopic").value,
        sub_topic:   $("subTopic").value,
        ack_topic:   $("ackTopic").value
    };

    try {
        setStatus("Saving...", true);
        const r = await fetch("/api/config", {
            method: "POST",
            headers: { "Content-Type": "application/json" },
            body: JSON.stringify(configForSave())
        });
        const result = await r.json();
        if (!r.ok || !result.ok) throw new Error(result.message || "Save failed");
        setStatus("Saved", true);
        setDirty(false);
        showMessage("Configuration saved successfully.", true);
    } catch (e) {
        console.error(e);
        setStatus("Save failed", false);
        showMessage("Save failed: " + e.message, false);
    }
}

async function resetConfig() {
    if (!await showDeleteConfirm("Reset all configuration?")) return;
    try {
        const r = await fetch("/api/config/reset", { method: "POST" });
        const result = await r.json();
        if (!r.ok || !result.ok) throw new Error(result.message || "Reset failed");
        await loadConfig();
        showMessage("Configuration reset.");
    } catch (e) {
        showMessage("Reset failed: " + e.message);
    }
}

function esc(v) {
    return String(v)
        .replaceAll("&", "&amp;")
        .replaceAll("<", "&lt;")
        .replaceAll(">", "&gt;")
        .replaceAll('"', "&quot;")
        .replaceAll("'", "&#039;");
}

loadConfig();

// ---------- Radar stage height ----------

async function loadStage() {
    const el = $("stageHeight");
    if (!el) return;
    try {
        const r = await fetch("/api/stage", { cache: "no-store" });
        const d = await r.json();
        if (r.ok && d.ok && typeof d.stage === "number") el.value = d.stage;
        else showMessage(d.message || "Could not read stage height", false);
    } catch (e) {
        showMessage("Could not read stage height: " + e.message, false);
    }
}

async function saveStage() {
    const el = $("stageHeight");
    const v = parseFloat(el.value);
    if (!isFinite(v) || v < 1 || v > 1000) {
        showMessage("Stage height must be between 1 and 1000.", false);
        return;
    }
    const btn = $("stageSaveBtn");
    btn.disabled = true;
    try {
        const r = await fetch("/api/stage", {
            method: "POST",
            headers: { "Content-Type": "application/json" },
            body: JSON.stringify({ stage: v })
        });
        const d = await r.json();
        if (!r.ok || !d.ok) throw new Error(d.message || ("HTTP " + r.status));
        el.value = d.stage;
        showMessage("Stage height set to " + d.stage);
    } catch (e) {
        showMessage("Failed: " + e.message, false);
    } finally {
        btn.disabled = false;
    }
}


// ---------- Firmware upload ----------

function uploadFirmware() {
    const file = $("fwFile").files[0];
    if (!file) { showMessage("Choose a firmware .bin file first.", false); return; }
    if (!file.name.endsWith(".bin")) { showMessage("File must be a .bin firmware image.", false); return; }
    if (!confirm(`Install ${file.name} (${formatSize(file.size)})? The device will reboot.`)) return;

    const btn = $("fwBtn"), wrap = $("fwProgressWrap"), bar = $("fwBar"), pct = $("fwPct");
    btn.disabled = true;
    wrap.style.display = "";
    bar.style.width = "0%";
    pct.textContent = "0%";

    const fd = new FormData();
    fd.append("firmware", file, file.name);

    const xhr = new XMLHttpRequest();
    xhr.open("POST", "/api/update");
    xhr.upload.onprogress = (e) => {
        if (!e.lengthComputable) return;
        const p = Math.round(100 * e.loaded / e.total);
        bar.style.width = p + "%";
        pct.textContent = p + "%";
    };
    xhr.onload = () => {
        let d = {};
        try { d = JSON.parse(xhr.responseText); } catch (_) {}
        if (xhr.status === 200 && d.ok) {
            pct.textContent = "Installed — device is rebooting…";
            showMessage("Firmware installed. Rebooting…");
        } else {
            btn.disabled = false;
            showMessage("Update failed: " + (d.message || ("HTTP " + xhr.status)), false);
        }
    };
    xhr.onerror = () => {
        btn.disabled = false;
        showMessage("Upload failed (connection lost).", false);
    };
    xhr.send(fd);
}

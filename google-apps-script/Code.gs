/**
 * Google Apps Script — Generic IoT logger (multi-project, multi-device)
 *
 * One Apps Script + one Spreadsheet can serve many projects. Each
 * project gets its own tab (sheet) inside the same spreadsheet,
 * named after the `project` field in the POST body.
 *
 * Devices POST JSON to /exec, e.g.:
 *   { "project":"room_temp", "device":"room1", "temp":24.7, "hum":58, "rssi":-61 }
 *   { "project":"power",     "device":"meter1","watts":120, "kwh":35.4 }
 *
 * Schema is **dynamic**: any extra keys become extra columns.
 * The first POST creates the tab and writes the header row from the
 * keys it sees. Later POSTs that introduce new keys grow the header.
 *
 * Special fields:
 *   project  -> picks/creates the tab (defaults to DEFAULT_PROJECT)
 *   ts       -> timestamp; if absent, the script stamps `new Date()`
 *
 * READ endpoints:
 *   GET /exec?project=room_temp&action=latest  [&device=room1]
 *   GET /exec?project=room_temp&action=history [&device=room1&n=100]
 *   GET /exec?project=room_temp&action=summary           (latest per device)
 *   GET /exec?project=room_temp&action=daily_history   [&device=room1&n=60]   (daily aggregates as JSON)
 *   GET /exec?project=room_temp&action=monthly_history [&device=room1&n=24]   (monthly aggregates as JSON)
 *   GET /exec?project=room_temp&action=report_daily      (refresh daily report)
 *   GET /exec?project=room_temp&action=report_monthly    (refresh monthly report)
 *   GET /exec?project=room_temp&action=refresh_reports   (refresh both reports)
 *   GET /exec?action=projects                            (list of tabs)
 *
 * SETUP:
 *   1) Set SHEET_ID below.
 *   2) Deploy → New deployment → Web app
 *        Execute as: Me   |   Access: Anyone
 *   3) Use the same /exec URL from every project's firmware.
 *      Each project just sends its own `project` value.
 */

const SHEET_ID        = '1nYyChrB_bBy2wSPH3w4wVxHWwKy7d3gQhFDK23xHZB8';
const DEFAULT_PROJECT = 'room_temp';
const DAILY_REPORT_SUFFIX = '_daily_report';
const MONTHLY_REPORT_SUFFIX = '_monthly_report';

// --- internals ---------------------------------------------------------

function _ss() { return SpreadsheetApp.openById(SHEET_ID); }

function _sanitize(name) {
  // Sheet tab names: max 100 chars, no : \ / ? * [ ]
  return String(name).replace(/[:\\\/\?\*\[\]]/g, '_').slice(0, 100) || DEFAULT_PROJECT;
}

function _getOrCreateSheet(project) {
  const ss = _ss();
  const name = _sanitize(project);
  let sh = ss.getSheetByName(name);
  if (!sh) {
    sh = ss.insertSheet(name);
    sh.appendRow(['ts']); // minimal header; expanded on first append
    sh.setFrozenRows(1);
  }
  return sh;
}

function _readHeader(sh) {
  const lc = sh.getLastColumn();
  if (lc === 0) return [];
  return sh.getRange(1, 1, 1, lc).getValues()[0].map(String);
}

function _ensureColumns(sh, keys) {
  let header = _readHeader(sh);
  const missing = keys.filter(k => header.indexOf(k) === -1);
  if (!missing.length) return header;
  const start = header.length + 1;
  sh.getRange(1, start, 1, missing.length).setValues([missing]);
  return header.concat(missing);
}

// --- POST: append a row ------------------------------------------------

function doPost(e) {
  try {
    const data = JSON.parse(e.postData.contents);
    const project = data.project || DEFAULT_PROJECT;
    delete data.project;                       // tab name, not a column
    if (!data.ts) data.ts = new Date();         // stamp if not provided

    const sh = _getOrCreateSheet(project);
    const keys = Object.keys(data);
    const header = _ensureColumns(sh, ['ts'].concat(keys.filter(k => k !== 'ts')));
    const row = header.map(h => (data[h] !== undefined ? data[h] : ''));
    sh.appendRow(row);

    return _json({ ok: true, project, cols: header.length });
  } catch (err) {
    return _json({ ok: false, error: String(err) });
  }
}

// --- GET: read rows ----------------------------------------------------

function doGet(e) {
  const action  = (e.parameter.action || 'latest').toLowerCase();
  const project = e.parameter.project || DEFAULT_PROJECT;
  const device  = e.parameter.device  || '';

  if (action === 'projects') {
    return _json(_ss().getSheets().map(s => s.getName()));
  }
  if (action === 'report_daily') {
    return _json(refreshDailyReport(project));
  }
  if (action === 'report_monthly') {
    return _json(refreshMonthlyReport(project));
  }
  if (action === 'refresh_reports') {
    return _json(refreshReports(project));
  }
  if (action === 'daily_history' || action === 'monthly_history') {
    const isMonthly = action === 'monthly_history';
    if (isMonthly) refreshMonthlyReport(project);
    else refreshDailyReport(project);
    const suffix = isMonthly ? MONTHLY_REPORT_SUFFIX : DAILY_REPORT_SUFFIX;
    const reportRows = _readSheetRecords(_sanitize(project + suffix))
      .filter(r => !device || String(r.device) === device);
    const n = Math.max(1, Math.min(500, parseInt(e.parameter.n || (isMonthly ? '24' : '60'), 10)));
    return _json(reportRows.slice(-n));
  }

  const sh = _getOrCreateSheet(project);
  const values = sh.getDataRange().getValues();
  if (values.length < 2) return _json(action === 'history' || action === 'summary' ? [] : null);

  const header = values[0].map(String);
  const idxDevice = header.indexOf('device');
  const rows = values.slice(1)
    .filter(r => !device || (idxDevice >= 0 && r[idxDevice] === device))
    .map(r => {
      const o = {};
      header.forEach((h, i) => { o[h] = r[i]; });
      return o;
    });

  let payload;
  if (action === 'history') {
    const n = Math.max(1, Math.min(2000, parseInt(e.parameter.n || '100', 10)));
    payload = rows.slice(-n);
  } else if (action === 'summary') {
    const latest = {};
    for (const r of rows) latest[r.device || ''] = r;
    payload = Object.values(latest);
  } else { // latest
    payload = rows.length ? rows[rows.length - 1] : null;
  }
  return _json(payload);
}

function _json(obj) {
  return ContentService
    .createTextOutput(JSON.stringify(obj))
    .setMimeType(ContentService.MimeType.JSON);
}

// --- Daily / monthly reports -----------------------------------------

function refreshReports(project) {
  const projectName = project || DEFAULT_PROJECT;
  return {
    ok: true,
    project: projectName,
    daily: refreshDailyReport(projectName),
    monthly: refreshMonthlyReport(projectName),
  };
}

function refreshDailyReports() {
  return refreshReports(DEFAULT_PROJECT);
}

function refreshMonthlyReports() {
  return refreshMonthlyReport(DEFAULT_PROJECT);
}

function refreshDailyReport(project) {
  const projectName = project || DEFAULT_PROJECT;
  const rows = _buildReportRows(projectName, 'day');
  const sheetName = _sanitize(projectName + DAILY_REPORT_SUFFIX);
  _writeReportSheet(sheetName, rows);
  return { ok: true, sheet: sheetName, rows: rows.length };
}

function refreshMonthlyReport(project) {
  const projectName = project || DEFAULT_PROJECT;
  const rows = _buildReportRows(projectName, 'month');
  const sheetName = _sanitize(projectName + MONTHLY_REPORT_SUFFIX);
  _writeReportSheet(sheetName, rows);
  return { ok: true, sheet: sheetName, rows: rows.length };
}

function setupReportTriggers() {
  ScriptApp.getProjectTriggers().forEach(trigger => {
    const handler = trigger.getHandlerFunction();
    if (handler === 'refreshDailyReports' || handler === 'refreshMonthlyReports') {
      ScriptApp.deleteTrigger(trigger);
    }
  });

  ScriptApp.newTrigger('refreshDailyReports')
    .timeBased()
    .everyDays(1)
    .atHour(0)
    .create();

  ScriptApp.newTrigger('refreshMonthlyReports')
    .timeBased()
    .onMonthDay(1)
    .atHour(1)
    .create();

  return { ok: true, daily: '00:00', monthly: 'day 1 01:00', timezone: Session.getScriptTimeZone() };
}

function _buildReportRows(project, periodType) {
  const data = _readProjectRecords(project);
  const timezone = Session.getScriptTimeZone();
  const groups = {};

  data.forEach(record => {
    const date = _asDate(record.ts);
    if (!date) return;
    const device = String(record.device || '').trim() || '(unknown)';
    const period = Utilities.formatDate(date, timezone, periodType === 'month' ? 'yyyy-MM' : 'yyyy-MM-dd');
    const key = period + '\u0001' + device;
    if (!groups[key]) {
      groups[key] = {
        period,
        device,
        count: 0,
        temps: [],
        hums: [],
        rssis: [],
        lastSeen: null,
        lastTemp: '',
        lastHum: '',
        lastIp: '',
        lastUptime: '',
      };
    }
    const group = groups[key];
    group.count++;
    _pushNumber(group.temps, record.temp);
    _pushNumber(group.hums, record.hum);
    _pushNumber(group.rssis, record.rssi);
    if (!group.lastSeen || date.getTime() >= group.lastSeen.getTime()) {
      group.lastSeen = date;
      group.lastTemp = _numberOrBlank(record.temp);
      group.lastHum = _numberOrBlank(record.hum);
      group.lastIp = record.ip || '';
      group.lastUptime = _numberOrBlank(record.uptime);
    }
  });

  return Object.values(groups)
    .sort((a, b) => a.period === b.period ? a.device.localeCompare(b.device) : a.period.localeCompare(b.period))
    .map(group => [
      group.period,
      group.device,
      group.count,
      _avg(group.temps),
      _min(group.temps),
      _max(group.temps),
      _avg(group.hums),
      _min(group.hums),
      _max(group.hums),
      _avg(group.rssis),
      group.lastTemp,
      group.lastHum,
      group.lastSeen ? group.lastSeen : '',
      group.lastIp,
      group.lastUptime,
    ]);
}

function _readProjectRecords(project) {
  return _recordsFromValues(_getOrCreateSheet(project).getDataRange().getValues());
}

function _readSheetRecords(sheetName) {
  const sh = _ss().getSheetByName(sheetName);
  if (!sh) return [];
  return _recordsFromValues(sh.getDataRange().getValues());
}

function _recordsFromValues(values) {
  if (values.length < 2) return [];
  const header = values[0].map(String);
  return values.slice(1).map(row => {
    const record = {};
    header.forEach((key, index) => { record[key] = row[index]; });
    return record;
  });
}

function _writeReportSheet(sheetName, rows) {
  const ss = _ss();
  let sh = ss.getSheetByName(sheetName);
  if (!sh) sh = ss.insertSheet(sheetName);
  sh.clearContents();
  const header = [
    'period', 'device', 'count',
    'avg_temp', 'min_temp', 'max_temp',
    'avg_hum', 'min_hum', 'max_hum',
    'avg_rssi', 'last_temp', 'last_hum', 'last_seen', 'last_ip', 'last_uptime'
  ];
  sh.getRange(1, 1, 1, header.length).setValues([header]);
  if (rows.length) sh.getRange(2, 1, rows.length, header.length).setValues(rows);
  sh.setFrozenRows(1);
  sh.autoResizeColumns(1, header.length);
}

function _asDate(value) {
  if (value instanceof Date && !isNaN(value.getTime())) return value;
  const date = new Date(value);
  return isNaN(date.getTime()) ? null : date;
}

function _pushNumber(values, value) {
  const num = Number(value);
  if (!isNaN(num)) values.push(num);
}

function _numberOrBlank(value) {
  const num = Number(value);
  return isNaN(num) ? '' : num;
}

function _avg(values) {
  if (!values.length) return '';
  return Math.round((values.reduce((sum, value) => sum + value, 0) / values.length) * 100) / 100;
}

function _min(values) { return values.length ? Math.min.apply(null, values) : ''; }
function _max(values) { return values.length ? Math.max.apply(null, values) : ''; }

// Diagnostic helpers for `clasp run`.
function checkProjects() {
  return _ss().getSheets().map(s => s.getName());
}

function checkRoomTempSummary() {
  const sh = _getOrCreateSheet(DEFAULT_PROJECT);
  const values = sh.getDataRange().getValues();
  if (values.length < 2) return [];
  const header = values[0].map(String);
  const latest = {};
  values.slice(1).forEach(row => {
    const record = {};
    header.forEach((key, index) => { record[key] = row[index]; });
    latest[record.device || ''] = record;
  });
  return Object.values(latest);
}

function checkRoom1History() {
  const sh = _getOrCreateSheet(DEFAULT_PROJECT);
  const values = sh.getDataRange().getValues();
  if (values.length < 2) return [];
  const header = values[0].map(String);
  const idxDevice = header.indexOf('device');
  return values.slice(1)
    .filter(row => idxDevice >= 0 && row[idxDevice] === 'room1')
    .slice(-5)
    .map(row => {
      const record = {};
      header.forEach((key, index) => { record[key] = row[index]; });
      return record;
    });
}

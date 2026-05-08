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

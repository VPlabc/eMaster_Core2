/* HSF Platform — Sơ đồ tủ cấp đồ (SmartLocker floor plan)
 *
 * The "Hoàng Sơn Dashboard Format" page with its sample arrays replaced by the
 * gateway's own data:
 *
 *   GET  /api/locker/state                 the whole model, on load
 *   WS   /ws                               the same model whenever it changes
 *   GET  /api/locker/history?…             the history modal, filtered and
 *                                          paged by SQLite rather than here
 *   POST /api/locker/lockers/<id>/unlock   forwarded to the SmartLocker script
 *   POST /api/locker/lockers/<id>/release            "
 *   POST /api/locker/sync                            "
 *
 * The three POSTs do not touch the database: the gateway hands them to the Lua
 * application, which owns the PLC and the state machine. This page asks; it
 * never decides.
 *
 * TWO WORDS THAT LOOK ALIKE AND ARE NOT. `status` is the DOOR (EMPTY /
 * ASSIGNED / EXPIRED / ERROR, from the lockers table). `usage_status` is the
 * CONTRACTOR'S DAY (NOT_STARTED / USING / COMPLETED / EXPIRED, from the
 * employees table -- CardScanPlan section 1). A door can be ASSIGNED while its
 * holder's day is COMPLETED, and the page never renders one in place of the
 * other.
 */

'use strict';

/* ================= constants ================= */

const STATUS = {
  EMPTY:    { label: 'Đang trống',   bg: '#f0eee9', border: '#d8d5cd', dot: '#c9c9c9', fg: '#8b867a' },
  ASSIGNED: { label: 'Đã đăng ký',   bg: '#e4f1e6', border: '#a9cfb2', dot: '#3aa76d', fg: '#2f7a4f' },
  EXPIRED:  { label: 'Hết hạn',      bg: '#fdefc6', border: '#e6c778', dot: '#d9a326', fg: '#a97a10' },
  ERROR:    { label: 'Lỗi khóa',     bg: '#f8e0dc', border: '#e3a99f', dot: '#d0483c', fg: '#a8452f' }
};

// The contractor's working day. `short` is what fits on a locker tile.
const USAGE = {
  NOT_STARTED: { label: 'Chưa sử dụng',    short: 'Chưa dùng', bg: '#f4f2ee', border: '#ddd9d1', fg: '#7a7a75' },
  USING:       { label: 'Đang sử dụng',    short: 'Đang dùng', bg: '#e4f1e6', border: '#a9cfb2', fg: '#2f7a4f' },
  COMPLETED:   { label: 'Đã hoàn tất ngày', short: 'Xong',     bg: '#e7edf8', border: '#b9c8e4', fg: '#2f4b8b' },
  EXPIRED:     { label: 'Đã hết hiệu lực', short: 'Hết hạn',   bg: '#f8e0dc', border: '#e3a99f', fg: '#a8452f' }
};

const ICON_LOCKED = ['M6.4 10.6h11.2v9.4H6.4z', 'M9 10.6V8.4a3 3 0 016 0v2.2', 'M12 14v2.6'];
const ICON_UNLOCK = ['M6.4 10.6h11.2v9.4H6.4z', 'M9 10.6V8.4a3 3 0 015.9-.7', 'M12 14v2.6'];
const ICON_CLOCK  = ['M12 3.6a8.4 8.4 0 100 16.8 8.4 8.4 0 000-16.8', 'M12 7.6V12l3.4 2'];
const ICON_WRENCH = ['M18.5 5.5l-3 3 0 0-3-3 3-3a5 5 0 00-5.6 6.6L5 13.9a2 2 0 102.8 2.8l4.8-4.8A5 5 0 0019.2 6.3z'];

// Event codes the Lua application writes into locker_logs, in Vietnamese. An
// unknown code falls back to the code itself rather than to "unknown" -- a new
// event type should still be readable here.
const EVENT_TEXT = {
  CARD_SCAN: 'Quẹt thẻ',
  ACCESS_GRANTED: 'Mở khóa thành công',
  ACCESS_DENIED: 'Từ chối',
  CARD_NOT_FOUND: 'Thẻ không tồn tại',
  CARD_INACTIVE: 'Thẻ bị khóa',
  CARD_EXPIRED: 'Thẻ hết hạn',
  WRONG_LOCKER_TYPE: 'Sai loại tủ',
  NO_LOCKER_ASSIGNED: 'Chưa được cấp ngăn',
  LOCKER_ASSIGNED: 'Cấp phát',
  LOCKER_RELEASED: 'Thu hồi',
  LOCKER_ASSIGN_FAILED: 'Không còn ngăn trống',
  LOCKER_UNLOCK: 'Kích mở khóa',
  DOOR_OPEN: 'Mở cửa',
  DOOR_CLOSE: 'Đóng cửa',
  DOOR_OPEN_TIMEOUT: 'Quá hạn mở cửa',
  DOOR_CLOSE_TIMEOUT: 'Quá hạn đóng cửa',
  LOCKER_ERROR: 'Lỗi ngăn',
  EMPLOYEE_SYNC: 'Đồng bộ nhân sự',
  RABBITMQ_UPDATE: 'Cập nhật realtime',
  PLC_ERROR: 'Lỗi PLC',
  READER_ERROR: 'Lỗi đầu đọc',
  // Contractor day + admin card (CardScanPlan)
  CONTRACTOR_USAGE: 'Ngày sử dụng nhà thầu',
  USAGE_NOT_STARTED: 'Chưa đến ngày sử dụng',
  USAGE_OUT_OF_HOURS: 'Ngoài giờ cho phép',
  OVERTIME_UPDATED: 'Cập nhật giờ tăng ca',
  ADMIN_CARD_SCAN: 'Quẹt thẻ admin',
  ADMIN_SCAN_SEQUENCE: 'Chuỗi quẹt admin',
  ADMIN_SCAN_TIMEOUT: 'Hết thời gian chuỗi admin',
  ADMIN_MODE_ENTER: 'Vào chế độ admin',
  ADMIN_MODE_TIMEOUT: 'Chế độ admin hết hạn',
  ADMIN_MODE_EXIT: 'Thoát chế độ admin',
  ADMIN_OVERRIDE_ACCESS: 'Admin mở hộ',
  ADMIN_OVERRIDE_DENIED: 'Admin bị từ chối',
  ADMIN_EXPIRED_LOCKER_OPEN: 'Admin mở ngăn hết hạn'
};

// The history modal's filter chips. Each is a set of event codes rather than a
// keyword match on the label: the codes are the stable thing in the database,
// the Vietnamese wording above is not.
const KINDS = [
  { label: 'Tất cả', codes: null, dot: '#c9c9c9' },
  { label: 'Cấp phát', dot: '#3aa76d', codes: ['LOCKER_ASSIGNED'] },
  { label: 'Mở khóa', dot: '#e0912f',
    codes: ['ACCESS_GRANTED', 'LOCKER_UNLOCK', 'DOOR_OPEN', 'DOOR_CLOSE',
            'ADMIN_OVERRIDE_ACCESS', 'ADMIN_EXPIRED_LOCKER_OPEN'] },
  { label: 'Thu hồi', dot: '#9a9a94', codes: ['LOCKER_RELEASED'] },
  { label: 'Từ chối', dot: '#c0663a',
    codes: ['ACCESS_DENIED', 'CARD_NOT_FOUND', 'CARD_INACTIVE', 'CARD_EXPIRED',
            'WRONG_LOCKER_TYPE', 'NO_LOCKER_ASSIGNED', 'USAGE_NOT_STARTED',
            'USAGE_OUT_OF_HOURS', 'ADMIN_OVERRIDE_DENIED'] },
  { label: 'Lỗi', dot: '#d0483c',
    codes: ['LOCKER_ERROR', 'DOOR_OPEN_TIMEOUT', 'DOOR_CLOSE_TIMEOUT',
            'PLC_ERROR', 'READER_ERROR', 'LOCKER_ASSIGN_FAILED'] }
];

const MONTHS = ['Tháng 1', 'Tháng 2', 'Tháng 3', 'Tháng 4', 'Tháng 5', 'Tháng 6',
                'Tháng 7', 'Tháng 8', 'Tháng 9', 'Tháng 10', 'Tháng 11', 'Tháng 12'];

/* ---------- roles ----------
 *
 * A VIEW MODE, not a security boundary. The locker port serves a kiosk display
 * and has no login (see the note in the permissions modal): this only decides
 * which controls the page offers, so a supervisor's wall display is not one
 * mis-click away from unlocking a cabinet. Anything that must actually be
 * enforced has to be enforced by the gateway, and it currently is not -- which
 * is why the port is meant to be firewalled rather than published.
 */
const ROLES = [
  { label: 'Quản trị hệ thống', short: 'QTHT',      scope: 'Toàn hệ thống',   desc: 'Toàn quyền cấu hình, phân quyền và vận hành tủ.' },
  { label: 'Quản lý xưởng',     short: 'QL xưởng',  scope: 'Xưởng phụ trách', desc: 'Cấp phát, thu hồi và theo dõi toàn bộ tủ trong xưởng.' },
  { label: 'Kỹ thuật',          short: 'Kỹ thuật',  scope: 'Ngăn lỗi',        desc: 'Xử lý ngăn lỗi, bảo trì khóa, ghi nhận sự cố.' },
  { label: 'Nhân viên',         short: 'Nhân viên', scope: 'Ngăn của mình',   desc: 'Chỉ xem và mở ngăn đang đăng ký cho mình.' },
  { label: 'Giám sát',          short: 'Giám sát',  scope: 'Chỉ xem',         desc: 'Xem trạng thái và lịch sử, không thao tác.' }
];

// `key` is what the markup's data-perm attributes refer to; rows with no key are
// documentation only. Order of the marks matches ROLES.
const PERMS = [
  { key: null,      name: 'Xem sơ đồ tủ',              hint: 'Trạng thái từng ngăn theo thời gian thực', marks: ['y','y','y','p','y'] },
  { key: null,      name: 'Xem chi tiết người đăng ký', hint: 'Họ tên, mã thẻ, ngày sử dụng',            marks: ['y','y','p','p','y'] },
  { key: 'sync',    name: 'Đồng bộ nhân sự',            hint: 'Tải lại danh sách thẻ từ máy chủ',        marks: ['y','y','n','n','n'] },
  { key: 'release', name: 'Thu hồi ngăn',               hint: 'Kết thúc lượt sử dụng, đưa ngăn về trống', marks: ['y','y','n','n','n'] },
  { key: 'unlock',  name: 'Mở khóa từ xa',              hint: 'Mở ngăn không cần thẻ tại tủ',            marks: ['y','y','p','p','n'] },
  { key: null,      name: 'Xử lý ngăn lỗi',             hint: 'Đánh dấu bảo trì, nghiệm thu sau sửa',    marks: ['y','p','y','n','n'] },
  { key: 'history', name: 'Xem lịch sử toàn tủ',        hint: 'Nhật ký thao tác của mọi ngăn',           marks: ['y','y','y','p','y'] },
  { key: 'export',  name: 'Xuất dữ liệu',               hint: 'Tải nhật ký ra tệp mở được bằng Excel',   marks: ['y','y','n','n','y'] },
  { key: null,      name: 'Cấu hình tủ & ngăn',         hint: 'Thêm tủ, đổi mã ngăn, vô hiệu ngăn',      marks: ['y','n','n','n','n'] },
  { key: null,      name: 'Phân quyền người dùng',      hint: 'Gán vai trò cho tài khoản',               marks: ['y','n','n','n','n'] }
];

// How many rows of doors the cabinet is drawn with. Three columns, filled from
// the RIGHT so locker 1 sits in the last column -- that is the arrangement the
// design source draws, and it is a fact about the physical cabinet rather than a
// style choice. If the real cabinet is numbered the other way round, this is the
// one line to change.
const GRID_COLUMNS = 3;
const GRID_FILL_FROM_RIGHT = true;

// Rows per page of the history modal. SQLite filters and counts; the browser
// only ever holds one page, so this is a display choice rather than a cap on
// what can be searched.
const HISTORY_PAGE = 200;

// Ceiling on a single export. The endpoint would happily page through more, but
// a CSV nobody can open is not a useful answer -- and the operator is told when
// it bites rather than being handed a silently short file.
const EXPORT_MAX = 20000;

/* ================= state ================= */

const state = {
  data: null,          // last snapshot from the gateway
  selected: null,      // locker id
  tab: 'detail',
  statFilter: null,    // one of STATUS's keys
  block: 'all',
  logQuery: '',

  role: ROLES[0].label,
  roleMenuOpen: false,
  permsOpen: false,
  permRole: ROLES[0].label,

  historyOpen: false,
  historyRows: null,   // null = not fetched yet; otherwise ONE page
  historyTotal: 0,     // matching the filter, as counted by SQLite
  historyOffset: 0,
  historyLoading: false,
  historyError: null,
  histQuery: '',
  histKind: 'Tất cả',
  chipsOpen: true,
  rangeStart: null,
  rangeEnd: null,
  rangeOpen: false,
  calYear: new Date().getFullYear(),
  calMonth: new Date().getMonth(),

  socket: null,
  reconnectDelay: 1000
};

// The chosen view role survives a reload: a wall display set to "Giám sát"
// should still be read-only after the gateway restarts.
try {
  const saved = localStorage.getItem('hsf.locker.role');
  if (saved && ROLES.some(r => r.label === saved)) {
    state.role = saved;
    state.permRole = saved;
  }
} catch (error) { /* private mode: the default role is fine */ }

const el = id => document.getElementById(id);

/* ================= helpers ================= */

function svg(size, paths, stroke, width) {
  return '<svg width="' + size + '" height="' + size + '" viewBox="0 0 24 24" fill="none" stroke="' +
    (stroke || 'currentColor') + '" stroke-width="' + (width || 1.9) +
    '" stroke-linecap="round" stroke-linejoin="round">' +
    paths.map(d => '<path d="' + d + '"></path>').join('') + '</svg>';
}

// Everything from the database is rendered as text, never as markup: a username
// arrives from the HR server and a last_error from a PLC fault, and neither is
// ours to trust with innerHTML.
function esc(value) {
  if (value === null || value === undefined) return '';
  return String(value)
    .replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;')
    .replace(/"/g, '&quot;').replace(/'/g, '&#39;');
}

function statusOf(locker) {
  return STATUS[locker.status] || STATUS.EMPTY;
}

// "D01" style label, per cabinet. The database id is what the API uses; this is
// only what the operator reads off the door.
function lockerLabel(locker) {
  const n = locker.locker_number || 0;
  return 'D' + (n < 10 ? '0' + n : n);
}

// Stored as "YYYY-MM-DD HH:MM:SS" local time by the Lua side.
function fmtTime(value) {
  if (!value) return '—';
  const parts = String(value).split(' ');
  return parts.length === 2 ? parts[1].slice(0, 5) + ' · ' + fmtDate(parts[0]) : String(value);
}

function fmtDate(value) {
  if (!value) return '—';
  const date = String(value).split(' ')[0].split('T')[0];
  const bits = date.split('-');
  return bits.length === 3 ? bits[2] + '-' + bits[1] + '-' + bits[0] : date;
}

function fmtDay(ts) {
  const d = new Date(ts), p = n => (n < 10 ? '0' : '') + n;
  return p(d.getDate()) + '/' + p(d.getMonth() + 1) + '/' + d.getFullYear();
}

// Midnight of the day a "YYYY-MM-DD ..." timestamp falls on, for range
// comparison. Parsed by hand rather than with Date(string): the value has no
// timezone and browsers disagree about what that means.
function dayOf(value) {
  const bits = String(value || '').split(' ')[0].split('-');
  if (bits.length !== 3) return null;
  return new Date(+bits[0], +bits[1] - 1, +bits[2]).getTime();
}

function eventText(code) {
  return EVENT_TEXT[code] || code || '—';
}

function kindOf(code) {
  for (const kind of KINDS) {
    if (kind.codes && kind.codes.indexOf(code) > -1) return kind;
  }
  return KINDS[0];
}

let toastTimer = null;
function toast(message, kind) {
  const box = el('toast');
  box.textContent = message;
  box.className = 'toast' + (kind ? ' toast--' + kind : '');
  box.hidden = false;
  clearTimeout(toastTimer);
  toastTimer = setTimeout(() => { box.hidden = true; }, 4000);
}

/* ---------- permissions ---------- */

function permMark(key) {
  const row = PERMS.find(p => p.key === key);
  if (!row) return 'y';
  const index = ROLES.findIndex(r => r.label === state.role);
  return row.marks[index < 0 ? 0 : index];
}

// 'p' (giới hạn phạm vi) is treated as allowed here: the scope it is limited to
// is not something this page can check without a logged-in identity, and
// pretending otherwise would hide a control the person may legitimately need.
function can(key) {
  return permMark(key) !== 'n';
}

function applyPermissions() {
  document.querySelectorAll('[data-perm]').forEach(node => {
    const allowed = can(node.dataset.perm);
    if (node.tagName === 'BUTTON') {
      node.classList.toggle('hidden', !allowed);
    } else {
      node.classList.toggle('btn-line--off', !allowed);
      node.title = allowed ? '' : 'Vai trò "' + state.role + '" không có quyền này';
    }
  });
}

/* ================= data ================= */

function allLockers() {
  return (state.data && Array.isArray(state.data.lockers)) ? state.data.lockers : [];
}

function lockers() {
  if (state.block === 'all') return allLockers();
  return allLockers().filter(l => String(l.block_id) === String(state.block));
}

function selectedLocker() {
  return lockers().find(l => l.id === state.selected) || null;
}

/* ---------- remaining usage time ----------
 *
 * WHICH DEADLINE APPLIES depends on the holding model the script is running
 * (`policy.contractor_mode`, published by the Lua side into the database):
 *
 *   mode 2  the HOLD: assigned_at + hold_hours. The locker goes back in the
 *           pool then, whatever state it is in.
 *   mode 1  the CARD: its expiry date. The locker is theirs until they finish
 *           and that passes.
 *
 * Both are shown the same way, because to the person standing at the cabinet
 * the question is the same one: how long is this mine.
 */

function policy() {
  const data = (state.data && state.data.policy) || {};
  return {
    mode: Number(data.contractor_mode) || 1,
    holdHours: data.hold_hours === undefined ? 24 : Number(data.hold_hours)
  };
}

// "YYYY-MM-DD HH:MM:SS" (local, no zone) -> epoch ms. Parsed by hand rather
// than with Date(string): the value carries no timezone and browsers disagree
// about what that means.
function stampToMs(value) {
  const parts = String(value || '').split(' ');
  const date = (parts[0] || '').split('-');
  if (date.length !== 3) return null;

  const time = (parts[1] || '00:00:00').split(':');
  const ms = new Date(+date[0], +date[1] - 1, +date[2],
                      +(time[0] || 0), +(time[1] || 0), +(time[2] || 0)).getTime();

  return isNaN(ms) ? null : ms;
}

// -> { ms, expired, deadline, source } or null when nothing bounds this locker.
function remainingFor(locker) {
  if (!locker || !locker.card_code) return null;

  const rules = policy();

  if (rules.mode === 2 && locker.locker_type === 'contractor') {
    if (!rules.holdHours) return null;   // hold_hours 0 disables the rule

    const from = stampToMs(locker.assigned_at);
    if (from === null) return null;      // no usable stamp: the script will not
                                         // reclaim it either, so show nothing

    const deadline = from + rules.holdHours * 3600 * 1000;
    return { ms: deadline - Date.now(), deadline: deadline, source: 'hold' };
  }

  // Mode 1, or an employee locker in either mode: the card's own expiry. A
  // date-only value covers the whole of that day, the same reading
  // sync.expire_at_end_of_day gives it.
  const day = stampToMs(locker.card_expire_at);
  if (day === null) return null;

  const deadline = day + 24 * 3600 * 1000 - 1000;
  return { ms: deadline - Date.now(), deadline: deadline, source: 'card' };
}

// Epoch ms -> the "YYYY-MM-DD HH:MM:SS" shape fmtTime() reads, in LOCAL time.
// Not toISOString(), which would shift the deadline by the UTC offset -- seven
// hours here, which is the difference between "reclaimed this evening" and
// "reclaimed at lunchtime".
function localStamp(ms) {
  const d = new Date(ms), p = n => (n < 10 ? '0' : '') + n;
  return d.getFullYear() + '-' + p(d.getMonth() + 1) + '-' + p(d.getDate()) + ' ' +
         p(d.getHours()) + ':' + p(d.getMinutes()) + ':' + p(d.getSeconds());
}

// Short enough for a locker tile: "18h 42m", "45m", "2 ngày".
function shortDuration(ms) {
  if (ms <= 0) return 'hết hạn';

  const minutes = Math.floor(ms / 60000);
  if (minutes < 60) return minutes + 'm';

  const hours = Math.floor(minutes / 60);
  if (hours < 48) return hours + 'h ' + (minutes % 60) + 'm';

  return Math.floor(hours / 24) + ' ngày';
}

// Under two hours is worth noticing; expired is worth acting on.
function urgencyOf(remaining) {
  if (!remaining) return null;
  if (remaining.ms <= 0) return 'over';
  if (remaining.ms <= 2 * 3600 * 1000) return 'soon';
  return 'ok';
}

// The contractor's day for a locker's holder, or null. Employees have no
// working day; neither does an empty locker.
function usageOf(locker) {
  if (!locker || locker.role !== 'contractor' || !locker.card_code) return null;

  const stored = locker.usage_status;
  if (!stored) return null;

  // The same two rules core/contractor_usage.lua applies in its status(), in
  // the same order, because the page renders whatever the snapshot happens to
  // hold and the snapshot is a row, not a decision:
  //
  //   1. the PERIOD is over -> EXPIRED, whatever the last day said
  //   2. the stored day is not today -> NOT_STARTED; yesterday's record is
  //      not today's state
  const pad = n => (n < 10 ? '0' : '') + n;
  const now = new Date();
  const key = now.getFullYear() + '-' + pad(now.getMonth() + 1) + '-' + pad(now.getDate());

  // A date-only expiry covers the whole of that day (sync.expire_at_end_of_day),
  // so the comparison is day against day rather than instant against instant.
  const expiry = dayOf(locker.card_expire_at);
  const lapsed = expiry !== null && expiry < dayOf(key);

  const stale = locker.usage_date !== key;
  const code = lapsed ? 'EXPIRED' : stale ? 'NOT_STARTED' : stored;

  return { code: code, stale: stale, lapsed: lapsed, meta: USAGE[code] || USAGE.NOT_STARTED };
}

function matchesFilter(locker) {
  return !state.statFilter || locker.status === state.statFilter;
}

/* ---------- cabinet arrangement ---------- */

// Splits a block's doors into GRID_COLUMNS stacks and emits them row-major, so
// the CSS grid draws vertical columns of consecutive numbers. Holes (a block
// whose count does not divide evenly) become placeholders rather than shifting
// every door after them into the wrong column.
function arrange(list) {
  if (!list.length) return [];

  const rows = Math.ceil(list.length / GRID_COLUMNS);
  const columns = [];

  for (let c = 0; c < GRID_COLUMNS; c++) {
    columns.push(list.slice(c * rows, (c + 1) * rows));
  }

  if (GRID_FILL_FROM_RIGHT) columns.reverse();

  const out = [];
  for (let r = 0; r < rows; r++) {
    for (let c = 0; c < GRID_COLUMNS; c++) out.push(columns[c][r] || null);
  }

  return out;
}

// Each cabinet arranged in turn, so "all blocks" is several cabinets one under
// the other rather than one interleaved mess.
function arrangedLockers() {
  const list = lockers();
  if (!list.length) return [];

  const byBlock = [];
  const seen = {};

  list.forEach(locker => {
    const key = String(locker.block_id);
    if (!seen[key]) { seen[key] = []; byBlock.push(seen[key]); }
    seen[key].push(locker);
  });

  return byBlock.reduce((all, block) => all.concat(arrange(block)), []);
}

/* ================= rendering ================= */

function renderHeader() {
  const data = state.data || {};
  el('area').textContent = data.device_name || data.machine_id || '—';
  el('userRole').textContent = state.role;

  const list = lockers();
  const blockName = state.block === 'all'
    ? 'Tất cả tủ'
    : (list[0] && list[0].block_name) || ('Tủ ' + state.block);

  el('toolbarMeta').textContent = (data.machine_id ? data.machine_id + ' · ' : '') + blockName;
  el('countNum').textContent = list.length;

  // The picker only appears when there is more than one cabinet -- a
  // single-cabinet site should not have to choose.
  const blocks = data.blocks || [];
  const picker = el('blockPicker');

  el('blockSelect').classList.toggle('hidden', blocks.length < 2);

  if (blocks.length > 1) {
    const wanted = ['all'].concat(blocks.map(b => String(b.id))).join(',');
    if (picker.dataset.keys !== wanted) {
      picker.dataset.keys = wanted;
      picker.innerHTML = '<option value="all">Tất cả tủ</option>' +
        blocks.map(b => '<option value="' + esc(b.id) + '">' +
                        esc(b.name || ('Tủ ' + b.id)) + ' (' + esc(b.count) + ')</option>').join('');
    }
    picker.value = state.block;
  }
}

function renderLegend() {
  const list = lockers();
  const count = key => list.filter(l => l.status === key).length;

  const broken = count('ERROR');
  // Doors that can actually be handed out. A locker in ERROR is not one of them,
  // so it is excluded from the denominator rather than counted as capacity.
  const usable = Math.max(0, list.length - broken);

  const items = [
    { key: 'ASSIGNED', label: 'Đang sử dụng', icon: ICON_LOCKED, count: count('ASSIGNED'), total: usable, big: true },
    { key: 'EMPTY',    label: 'Đang trống',   icon: ICON_UNLOCK, count: count('EMPTY'),    total: usable, big: true },
    { key: 'EXPIRED',  label: 'Hết hạn',      icon: ICON_CLOCK,  count: count('EXPIRED'),  total: usable, big: false },
    { key: 'ERROR',    label: 'Lỗi',          icon: ICON_WRENCH, count: broken,            total: list.length, big: false }
  ];

  el('legend').innerHTML = items.map(item => {
    const style = STATUS[item.key];
    return '<div class="stat' + (item.big ? ' stat--big' : '') +
        (state.statFilter === item.key ? ' stat--on' : '') + '" data-filter="' + item.key + '">' +
      '<span class="stat__icon" style="background:' + style.bg + ';color:' + style.fg + '">' +
        svg(item.big ? 20 : 17, item.icon) + '</span>' +
      '<span class="stat__text">' +
        '<span class="stat__label">' + item.label + '</span>' +
        '<span class="stat__num"><b>' + item.count + '</b><span>/' + item.total + '</span></span>' +
      '</span>' +
    '</div>';
  }).join('');

  const on = !!state.statFilter;
  el('filterBar').classList.toggle('hidden', !on);
  if (on) {
    const label = (STATUS[state.statFilter] || {}).label || state.statFilter;
    el('filterHint').textContent = 'Đang lọc: ' + label + ' — bấm lại thẻ để bỏ lọc';
  }
}

function renderGrid() {
  const list = arrangedLockers();
  const grid = el('grid');

  if (!list.length) {
    grid.innerHTML = '<div class="cabinet__empty">' +
      (state.data && state.data.ok
        ? 'Chưa có ngăn nào được cấu hình.<br>Kiểm tra <b>locker.blocks</b> trong smartlocker.json.'
        : 'Chưa đọc được dữ liệu tủ.<br>Ứng dụng <b>SmartLocker</b> đã chạy chưa?') +
      '</div>';
    return;
  }

  const filtering = !!state.statFilter;

  grid.innerHTML = list.map(locker => {
    if (!locker) return '<div class="cabinet__gap"></div>';

    const style = statusOf(locker);
    const dim = filtering && !matchesFilter(locker);
    const open = !!locker.door_open;
    const noOutput = locker.output_register === null || locker.output_register === undefined;
    const usage = usageOf(locker);

    // Dimmed doors keep their shape but lose their colour, so a filtered view
    // still reads as a cabinet rather than as a grid with holes in it.
    const bg = dim ? '#d6d3cd' : style.bg;
    const bd = dim ? '#c9c6bf' : style.border;
    const dot = dim ? '#bcb9b3' : style.dot;
    const lockBg = dim ? '#c9c6bf' : (open ? '#e9e4d8' : '#b9b3a4');
    const lockFg = dim ? '#a6a39d' : (open ? '#c9741a' : '#4b4b46');

    let badge = '';
    if (open) badge = '<div class="locker__badge locker__badge--open">Tủ mở</div>';
    else if (noOutput) badge = '<div class="locker__badge locker__badge--nolock">Không khóa</div>';
    else if (locker.runtime_state && locker.runtime_state !== 'IDLE') {
      badge = '<div class="locker__badge">' + esc(locker.runtime_state) + '</div>';
    }

    // The contractor's day, on the tile. Only for a contractor whose day has
    // actually started -- a chip reading "Chưa dùng" on every door would be
    // noise, not information.
    const usageChip = (usage && usage.code !== 'NOT_STARTED' && !dim)
      ? '<div class="locker__usage" style="background:' + usage.meta.bg +
        ';border-color:' + usage.meta.border + ';color:' + usage.meta.fg + '">' +
        esc(usage.meta.short) + '</div>'
      : '';

    // How long this locker is still theirs. On the tile because it is the
    // question somebody standing at the cabinet actually has.
    const remaining = dim ? null : remainingFor(locker);
    const holdChip = remaining
      ? '<div class="locker__hold locker__hold--' + urgencyOf(remaining) + '" title="' +
        esc(remaining.source === 'hold' ? 'Hết hạn giữ hộc' : 'Hạn sử dụng thẻ') + '">' +
        esc(shortDuration(remaining.ms)) + '</div>'
      : '';

    const person = locker.username || locker.card_code ||
                   (locker.status === 'ERROR' ? 'Lỗi khóa' : 'Đang trống');

    const cls = 'locker' +
      (state.selected === locker.id ? ' locker--selected'
        : (filtering && !dim ? ' locker--match' : ''));

    return '<div class="' + cls + '" data-id="' + esc(locker.id) + '"' +
        ' style="background:' + bg + ';border-color:' + bd + '">' +
      badge +
      '<div class="locker__id">' + esc(lockerLabel(locker)) + '</div>' +
      '<div class="locker__lock" style="background:' + lockBg + ';color:' + lockFg + '">' +
        svg(16, open ? ICON_UNLOCK : ICON_LOCKED) + '</div>' +
      '<div class="locker__slot"></div>' +
      usageChip +
      holdChip +
      '<div class="locker__foot">' +
        '<span class="locker__user" style="color:' + (dim ? '#98958f' : '#4b4b46') + '">' +
          esc(person) + '</span>' +
        '<span class="locker__dot" style="background:' + dot + '"></span>' +
      '</div>' +
    '</div>';
  }).join('');
}

function usageChipHtml(usage) {
  return '<span class="usage usage--' + usage.code + '">' +
    '<span class="usage__dot"></span>' + esc(usage.meta.label) + '</span>';
}

function renderPanel() {
  const locker = selectedLocker();

  el('aside').classList.toggle('aside--hidden', !locker);
  el('cabinet').classList.toggle('cabinet--centered', !locker);
  if (!locker) return;

  const style = statusOf(locker);
  el('panelTitle').textContent = 'Ngăn ' + lockerLabel(locker);

  const usage = usageOf(locker);
  const isContractor = locker.role === 'contractor';

  const rows = [
    ['Trạng thái', style.label],
    ['Cửa tủ', locker.door_open ? 'Đang mở' : 'Đang đóng'],
    ['Loại tủ', locker.locker_type === 'contractor' ? 'Nhà thầu' : 'Nhân viên'],
    ['Người đăng ký', locker.username || '—'],
    ['Mã thẻ', locker.card_code || '—'],
    ['Giới tính', locker.gender === 'female' ? 'Nữ' : locker.gender === 'male' ? 'Nam' : '—']
  ];

  // The contractor's working day, spelled out. Nothing here for an employee:
  // they have no day, and empty rows would suggest the data is missing.
  if (isContractor) {
    rows.push(['Ngày sử dụng', usage ? { html: usageChipHtml(usage) } : '—']);

    if (usage && usage.stale && locker.usage_date) {
      rows.push(['Ghi nhận gần nhất', fmtDate(locker.usage_date)]);
    }
    if (locker.usage_started_at) rows.push(['Bắt đầu', fmtTime(locker.usage_started_at)]);
    if (locker.usage_completed_at) rows.push(['Hoàn tất', fmtTime(locker.usage_completed_at)]);
    if (locker.card_start_at) rows.push(['Bắt đầu hiệu lực', fmtDate(locker.card_start_at)]);
  }

  // The countdown, spelled out. The tile shows "18h 42m"; here it says what
  // that is counting down TO, because "mine until when" and "mine for how long"
  // are different questions and the panel is where there is room for both.
  const remaining = remainingFor(locker);

  if (remaining) {
    const urgency = urgencyOf(remaining);
    const label = remaining.source === 'hold' ? 'Còn lại (giữ hộc)' : 'Còn lại (hạn thẻ)';

    rows.push([label, {
      html: '<span class="remain remain--' + urgency + '">' +
            esc(shortDuration(remaining.ms)) + '</span>'
    }]);

    rows.push([remaining.source === 'hold' ? 'Tự động thu hồi lúc' : 'Hết hạn lúc',
               fmtTime(localStamp(remaining.deadline))]);
  }

  rows.push(['Thời gian cấp', locker.assigned_at ? fmtTime(locker.assigned_at) : '—']);
  rows.push(['Hạn sử dụng thẻ', locker.card_expire_at ? fmtDate(locker.card_expire_at) : 'Không giới hạn']);
  rows.push(['Mở gần nhất', fmtTime(locker.last_open_at)]);
  rows.push(['Đóng gần nhất', fmtTime(locker.last_close_at)]);

  if (locker.last_error) rows.push(['Lỗi gần nhất', locker.last_error, true]);

  el('detailRows').innerHTML = rows.map(row => {
    const value = (row[1] && row[1].html) ? row[1].html : esc(row[1]);
    return '<div class="detail__row"><span class="detail__k">' + esc(row[0]) + '</span>' +
      '<span class="detail__v' + (row[2] ? ' detail__v--error' : '') + '">' + value + '</span></div>';
  }).join('');

  // A door with no unlock output cannot be opened from here, and saying so on
  // the button beats a failure the operator has to interpret.
  const noOutput = locker.output_register === null || locker.output_register === undefined;
  el('btnUnlock').disabled = noOutput;
  el('btnUnlock').title = noOutput ? 'Ngăn này chưa được đấu ngõ ra mở khóa' : '';
  el('btnRelease').disabled = !locker.card_code;

  renderLockerLogs(locker);

  el('detailPane').classList.toggle('hidden', state.tab !== 'detail');
  el('logsPane').classList.toggle('hidden', state.tab !== 'logs');
  document.querySelectorAll('.tab').forEach(tab =>
    tab.classList.toggle('tab--active', tab.dataset.tab === state.tab));
}

function renderLockerLogs(locker) {
  const query = state.logQuery.trim().toLowerCase();

  const all = ((state.data && state.data.events) || []).filter(e => e.locker_id === locker.id);

  const matching = all.filter(entry => {
    if (!query) return true;
    const haystack = [entry.created_at, eventText(entry.event), entry.event,
                      entry.reason, entry.card_code].join(' ').toLowerCase();
    return haystack.indexOf(query) > -1;
  });

  const shown = matching.slice(0, 10);

  el('logsHint').textContent = query
    ? matching.length + ' hoạt động khớp'
    : (all.length ? Math.min(10, all.length) + ' hoạt động gần nhất' : '');

  el('logsList').innerHTML = shown.length
    ? shown.map(entry => {
        const kind = entry.result === 'GRANTED' ? ' log__icon--granted'
                   : entry.result === 'DENIED' ? ' log__icon--denied' : '';
        return '<div class="log">' +
          '<span class="log__icon' + kind + '">' + svg(14, ICON_CLOCK, 'currentColor', 2) + '</span>' +
          '<span class="log__time">' + esc(fmtTime(entry.created_at)) + '</span>' +
          '<span class="log__text">' + esc(eventText(entry.event)) +
            (entry.reason ? ' — ' + esc(entry.reason) : '') + '</span>' +
        '</div>';
      }).join('')
    : '<div class="empty">' +
      (all.length ? 'Không tìm thấy hoạt động' : 'Chưa có hoạt động nào cho ngăn này') + '</div>';
}

/* ---------- role menu + permissions ---------- */

function renderRoleMenu() {
  const current = ROLES.find(r => r.label === state.role) || ROLES[0];

  el('userRole').textContent = state.role;
  el('roleMenu').classList.toggle('hidden', !state.roleMenuOpen);

  if (!state.roleMenuOpen) return;

  el('roleMenu').innerHTML =
    '<div class="role-menu__head">' +
      '<div class="role-menu__kicker">Vai trò đang dùng</div>' +
      '<div class="role-menu__cur">' + esc(current.label) + '</div>' +
      '<div class="role-menu__desc">' + esc(current.desc) + '</div>' +
    '</div>' +
    ROLES.map(role =>
      '<div class="role' + (role.label === state.role ? ' role--on' : '') +
        '" data-role="' + esc(role.label) + '">' +
        '<span class="role__radio"><i></i></span>' +
        '<span class="role__label">' + esc(role.label) + '</span>' +
        '<span class="role__scope">' + esc(role.scope) + '</span>' +
      '</div>').join('') +
    '<div class="role-menu__link" id="openPerms">Xem bảng phân quyền' +
      svg(13, ['M9.5 6l6 6-6 6'], '#c9741a', 2.2) + '</div>';
}

function renderPerms() {
  el('permsOverlay').classList.toggle('hidden', !state.permsOpen);
  if (!state.permsOpen) return;

  el('permRoleChips').innerHTML = ROLES.map(role =>
    '<span class="pill' + (role.label === state.permRole ? ' pill--on' : '') +
      '" data-permrole="' + esc(role.label) + '">' + esc(role.label) + '</span>').join('');

  const index = ROLES.findIndex(r => r.label === state.permRole);
  const mark = (value, on) => '<span class="mark mark--' + value + (on ? ' mark--on' : '') + '">' +
    (value === 'y' ? '✓' : value === 'p' ? '◐' : '–') + '</span>';

  el('permBody').innerHTML =
    '<div class="prow prow--head"><span class="prow__k">Quyền</span>' +
      ROLES.map((role, i) =>
        '<span class="prow__c' + (i === index ? ' prow__c--on' : '') + '">' +
        esc(role.short) + '</span>').join('') +
    '</div>' +
    PERMS.map(perm => '<div class="prow">' +
      '<span class="prow__label"><b>' + esc(perm.name) + '</b><i>' + esc(perm.hint) + '</i></span>' +
      perm.marks.map((value, i) => '<span class="prow__cell">' + mark(value, i === index) + '</span>').join('') +
    '</div>').join('');
}

/* ---------- history ---------- */

function historyRows() {
  return state.historyRows || [];
}

// "2026-08-14", the form GET /api/locker/history wants, from a calendar
// timestamp. Built from the local date parts rather than toISOString(), which
// would shift the day for anyone east of Greenwich -- including here.
function isoDay(ts) {
  if (!ts) return null;
  const d = new Date(ts), p = n => (n < 10 ? '0' : '') + n;
  return d.getFullYear() + '-' + p(d.getMonth() + 1) + '-' + p(d.getDate());
}

// The filter as query parameters. One place, so the table, the pager and the
// export cannot disagree about what is being asked for.
function historyParams(limit, offset) {
  const params = new URLSearchParams();
  const kind = KINDS.find(k => k.label === state.histKind) || KINDS[0];

  if (kind.codes) params.set('event', kind.codes.join(','));

  const from = isoDay(state.rangeStart);
  const to = isoDay(state.rangeEnd);
  if (from) params.set('from', from);
  if (to) params.set('to', to);

  const query = state.histQuery.trim();
  if (query) params.set('q', query);

  // The modal follows the cabinet picker: looking at one cabinet and getting
  // another one's history back would be a quiet lie.
  if (state.block !== 'all') params.set('block', state.block);

  params.set('limit', limit);
  params.set('offset', offset);

  return params;
}

function renderCalendar() {
  el('cal').classList.toggle('hidden', !state.rangeOpen);
  if (!state.rangeOpen) return;

  const year = state.calYear, month = state.calMonth;
  // Monday-first, which is how the header row below is labelled.
  const lead = (new Date(year, month, 1).getDay() + 6) % 7;
  const days = new Date(year, month + 1, 0).getDate();
  const start = state.rangeStart, end = state.rangeEnd;

  let cells = '';
  for (let i = 0; i < lead; i++) cells += '<span class="cal__d cal__d--pad"></span>';

  for (let d = 1; d <= days; d++) {
    const ts = new Date(year, month, d).getTime();
    const on = start === ts || end === ts;
    const mid = start && end && ts > start && ts < end;
    cells += '<span class="cal__d' + (on ? ' cal__d--on' : mid ? ' cal__d--mid' : '') +
      '" data-ts="' + ts + '">' + d + '</span>';
  }

  el('cal').innerHTML =
    '<div class="cal__head">' +
      '<span class="cal__nav" data-mon="-1">' + svg(15, ['M14.5 6l-6 6 6 6'], 'currentColor', 2.2) + '</span>' +
      '<span class="cal__title">' + MONTHS[month] + ' ' + year + '</span>' +
      '<span class="cal__nav" data-mon="1">' + svg(15, ['M9.5 6l6 6-6 6'], 'currentColor', 2.2) + '</span>' +
    '</div>' +
    '<div class="cal__grid">' +
      ['T2','T3','T4','T5','T6','T7','CN'].map(w => '<span class="cal__wd">' + w + '</span>').join('') +
    '</div>' +
    '<div class="cal__grid" style="margin-top:4px">' + cells + '</div>' +
    '<div class="cal__foot">' +
      '<span class="cal__hint">' + (start && !end ? 'Chọn ngày kết thúc' : 'Chọn ngày bắt đầu') + '</span>' +
      '<span class="cal__clear" id="calClear">Xóa</span>' +
      '<span class="cal__apply" id="calApply">Áp dụng</span>' +
    '</div>';
}

function renderHistory() {
  el('historyOverlay').classList.toggle('hidden', !state.historyOpen);
  if (!state.historyOpen) return;

  const data = state.data || {};
  const blockName = state.block === 'all' ? 'toàn bộ tủ' : ((lockers()[0] || {}).block_name || 'tủ');
  el('histTitle').textContent = 'Lịch sử ' + blockName +
    (data.machine_id ? ' — ' + data.machine_id : '');

  const start = state.rangeStart, end = state.rangeEnd;
  el('rangeLabel').textContent = start && end ? fmtDay(start) + ' - ' + fmtDay(end)
    : start ? fmtDay(start) + ' - …' : 'Chọn khoảng ngày';

  el('funnel').classList.toggle('funnel--on', state.chipsOpen);
  el('histChips').classList.toggle('hidden', !state.chipsOpen);
  el('histChips').innerHTML = KINDS.map(kind =>
    '<span class="pill' + (kind.label === state.histKind ? ' pill--on' : '') +
      '" data-kind="' + esc(kind.label) + '">' + esc(kind.label) + '</span>').join('');

  if (state.historyLoading) {
    el('histCount').textContent = 'đang tải…';
    el('histBody').innerHTML = '<div class="empty" style="padding:38px 0">Đang tải nhật ký…</div>';
    el('histNote').textContent = '';
    el('histPrev').disabled = true;
    el('histNext').disabled = true;
    renderCalendar();
    return;
  }

  const rows = historyRows();
  const total = state.historyTotal;
  const first = total ? state.historyOffset + 1 : 0;
  const last = state.historyOffset + rows.length;

  // The count is SQLite's, over the whole retention window -- not "how many
  // rows this page happens to be holding".
  el('histCount').textContent = total + ' bản ghi';
  el('histNote').textContent = total > rows.length
    ? first + '–' + last + ' / ' + total
    : (total ? total + ' bản ghi' : '');

  el('histPrev').disabled = state.historyOffset <= 0;
  el('histNext').disabled = last >= total;

  el('histBody').innerHTML =
    '<div class="hrow hrow--head"><span>Thời gian</span><span>Ngăn</span>' +
      '<span>Hoạt động</span><span>Người thực hiện</span></div>' +
    (state.historyError
      ? '<div class="empty" style="padding:38px 0">' + esc(state.historyError) + '</div>'
      : rows.length
        ? rows.map(row => {
            const kind = kindOf(row.event);
            // The row carries its own locker number and username, so a door that
            // has since been re-wired -- or a card that has since been removed --
            // still reads correctly in its own history.
            const label = row.locker_number
              ? lockerLabel({ locker_number: row.locker_number }) : '—';
            const actor = row.username || row.card_code || 'Hệ thống';
            const what = eventText(row.event) + (row.reason ? ' — ' + row.reason : '');
            return '<div class="hrow">' +
              '<span class="hrow__time">' + esc(fmtTime(row.created_at)) + '</span>' +
              '<span class="hrow__locker">' + esc(label) + '</span>' +
              '<span class="hrow__act"><span class="hrow__dot" style="background:' + kind.dot + '"></span>' +
                '<span title="' + esc(what) + '">' + esc(what) + '</span></span>' +
              '<span class="hrow__actor">' + esc(actor) + '</span>' +
            '</div>';
          }).join('')
        : '<div class="empty" style="padding:38px 0">Không có bản ghi nào khớp bộ lọc</div>');

  renderCalendar();
}

// Every filter change comes back here rather than re-filtering what is already
// loaded: SQLite is the only thing that has seen the whole table.
async function loadHistory(offset) {
  state.historyOffset = Math.max(0, offset || 0);
  state.historyLoading = true;
  state.historyError = null;
  renderHistory();

  try {
    const url = '/api/locker/history?' + historyParams(HISTORY_PAGE, state.historyOffset);
    const response = await fetch(url);
    const body = await response.json().catch(() => null);

    if (!response.ok || (body && body.ok === false)) {
      throw new Error((body && body.error) || ('HTTP ' + response.status));
    }

    state.historyRows = Array.isArray(body.rows) ? body.rows : [];
    state.historyTotal = Number(body.total) || 0;
  } catch (error) {
    state.historyRows = [];
    state.historyTotal = 0;
    state.historyError = 'Không tải được nhật ký: ' + error.message;
    toast(state.historyError, 'error');
  } finally {
    state.historyLoading = false;
    renderHistory();
  }
}

// Typing should not fire a query per keystroke at a gateway that is also
// driving a PLC.
let historyDebounce = null;
function reloadHistorySoon() {
  clearTimeout(historyDebounce);
  historyDebounce = setTimeout(() => loadHistory(0), 250);
}

// Exported as CSV with a UTF-8 BOM and semicolon separators, which is what
// Excel on a Vietnamese Windows opens directly without an import dialog.
//
// The export is of the WHOLE filtered set, not the page on screen: it re-runs
// the same filter with a large page size and walks it, so what lands in the file
// is what the count in the corner says.
async function exportHistory() {
  if (!state.historyTotal) {
    toast('Không có bản ghi nào để xuất', 'error');
    return;
  }

  const wanted = Math.min(state.historyTotal, EXPORT_MAX);
  const rows = [];

  toast('Đang chuẩn bị ' + wanted + ' bản ghi…');

  try {
    while (rows.length < wanted) {
      const page = Math.min(1000, wanted - rows.length);
      const url = '/api/locker/history?' + historyParams(page, rows.length);
      const response = await fetch(url);
      const body = await response.json().catch(() => null);

      if (!response.ok || !body || body.ok === false) {
        throw new Error((body && body.error) || ('HTTP ' + response.status));
      }

      const batch = Array.isArray(body.rows) ? body.rows : [];
      if (!batch.length) break;   // the table shrank under us; take what we have

      rows.push.apply(rows, batch);
    }
  } catch (error) {
    toast('Không xuất được: ' + error.message, 'error');
    return;
  }

  const cell = value => '"' + String(value === null || value === undefined ? '' : value)
    .replace(/"/g, '""') + '"';

  const lines = [['Thời gian', 'Ngăn', 'Hoạt động', 'Mã sự kiện', 'Kết quả', 'Lý do',
                  'Mã thẻ', 'Người thực hiện'].map(cell).join(';')];

  rows.forEach(row => {
    lines.push([
      row.created_at,
      row.locker_number ? lockerLabel({ locker_number: row.locker_number }) : '',
      eventText(row.event), row.event, row.result, row.reason, row.card_code,
      row.username || ''
    ].map(cell).join(';'));
  });

  const blob = new Blob(['﻿' + lines.join('\r\n')], { type: 'text/csv;charset=utf-8' });
  const url = URL.createObjectURL(blob);
  const link = document.createElement('a');
  const stamp = new Date().toISOString().slice(0, 19).replace(/[:T]/g, '-');

  link.href = url;
  link.download = 'lich-su-tu-cap-do-' + stamp + '.csv';
  document.body.appendChild(link);
  link.click();
  document.body.removeChild(link);
  URL.revokeObjectURL(url);

  toast(rows.length < state.historyTotal
    ? 'Đã xuất ' + rows.length + ' / ' + state.historyTotal + ' bản ghi (giới hạn ' + EXPORT_MAX + ')'
    : 'Đã xuất ' + rows.length + ' bản ghi', 'ok');
}

/* ---------- everything ---------- */

function render() {
  renderHeader();
  renderLegend();
  renderGrid();
  renderPanel();
  renderRoleMenu();
  renderPerms();
  renderHistory();
  applyPermissions();
}

/* ================= transport ================= */

function setLink(status, text) {
  el('linkStatus').className = 'link-status' + (status ? ' link-status--' + status : '');
  el('linkText').textContent = text;
}

function apply(data) {
  state.data = data;

  // A locker that disappeared (layout change) must not leave the panel showing
  // a door that no longer exists.
  if (state.selected !== null && !selectedLocker()) state.selected = null;

  render();
}

async function loadOnce() {
  try {
    const response = await fetch('/api/locker/state');
    if (!response.ok) throw new Error('HTTP ' + response.status);
    apply(await response.json());
    if (!state.socket) setLink('', 'Đã tải');
  } catch (error) {
    setLink('lost', 'Không tải được dữ liệu');
    toast('Không tải được dữ liệu: ' + error.message, 'error');
  }
}

function connect() {
  const scheme = location.protocol === 'https:' ? 'wss' : 'ws';
  const socket = new WebSocket(scheme + '://' + location.host + '/ws');

  socket.onopen = () => {
    state.socket = socket;
    state.reconnectDelay = 1000;
    setLink('live', 'Trực tuyến');
  };

  socket.onmessage = event => {
    try {
      const payload = JSON.parse(event.data);
      if (payload && payload.type === 'state') apply(payload);
    } catch (error) {
      // A frame we cannot parse is not worth tearing the page down over.
      console.warn('bad frame', error);
    }
  };

  socket.onclose = () => {
    state.socket = null;
    setLink('lost', 'Mất kết nối — đang thử lại');
    // Backing off to 10s: a gateway that is restarting should not be hammered
    // by every wall display in the building.
    setTimeout(connect, state.reconnectDelay);
    state.reconnectDelay = Math.min(state.reconnectDelay * 2, 10000);
  };

  socket.onerror = () => socket.close();
}

async function post(url, label) {
  try {
    const response = await fetch(url, { method: 'POST', headers: { 'Content-Type': 'application/json' } });
    const text = await response.text();

    let body = null;
    try { body = JSON.parse(text); } catch (error) { /* not JSON: shown raw below */ }

    if (!response.ok || (body && body.ok === false)) {
      const message = (body && (body.error || body.message)) || text || ('HTTP ' + response.status);
      toast(label + ' thất bại: ' + message, 'error');
      return false;
    }

    toast((body && body.message) || (label + ' thành công'), 'ok');
    // The WebSocket brings the new state along within a second; asking for it
    // now just makes the button feel immediate.
    loadOnce();
    return true;
  } catch (error) {
    toast(label + ' thất bại: ' + error.message, 'error');
    return false;
  }
}

/* ================= events ================= */

el('grid').addEventListener('click', event => {
  const card = event.target.closest('.locker');
  if (!card) return;
  const id = Number(card.dataset.id);
  state.selected = state.selected === id ? null : id;
  render();
});

el('legend').addEventListener('click', event => {
  const card = event.target.closest('.stat');
  if (!card) return;
  state.statFilter = state.statFilter === card.dataset.filter ? null : card.dataset.filter;
  renderLegend();
  renderGrid();
});

el('filterClear').addEventListener('click', () => {
  state.statFilter = null;
  renderLegend();
  renderGrid();
});

el('panelClose').addEventListener('click', () => { state.selected = null; render(); });

document.querySelectorAll('.tab').forEach(tab => tab.addEventListener('click', () => {
  state.tab = tab.dataset.tab;
  renderPanel();
}));

el('logSearch').addEventListener('input', event => {
  state.logQuery = event.target.value;
  const locker = selectedLocker();
  if (locker) renderLockerLogs(locker);
});

el('blockPicker').addEventListener('change', () => {
  state.block = el('blockPicker').value;
  state.selected = null;
  render();
});

/* ---------- role menu ---------- */

el('userChip').addEventListener('click', () => {
  state.roleMenuOpen = !state.roleMenuOpen;
  renderRoleMenu();
});

el('roleMenu').addEventListener('click', event => {
  const role = event.target.closest('[data-role]');

  if (role) {
    state.role = role.dataset.role;
    state.permRole = role.dataset.role;
    state.roleMenuOpen = false;
    try { localStorage.setItem('hsf.locker.role', state.role); } catch (error) { /* ignore */ }
    render();
    return;
  }

  if (event.target.closest('#openPerms')) {
    state.permsOpen = true;
    state.roleMenuOpen = false;
    render();
  }
});

el('permRoleChips').addEventListener('click', event => {
  const chip = event.target.closest('[data-permrole]');
  if (!chip) return;
  state.permRole = chip.dataset.permrole;
  renderPerms();
});

/* ---------- history ---------- */

el('historyBtn').addEventListener('click', () => {
  if (!can('history')) return;
  state.historyOpen = true;
  render();
  // Re-fetched every time it is opened: the modal is a point-in-time report,
  // and the WebSocket only carries the most recent handful of events.
  loadHistory(0);
});

// Only shows or hides the chips; it changes nothing about the filter, so there
// is nothing to re-fetch.
el('funnel').addEventListener('click', () => {
  state.chipsOpen = !state.chipsOpen;
  renderHistory();
});

el('histSearch').addEventListener('input', event => {
  state.histQuery = event.target.value;
  reloadHistorySoon();
});

el('histChips').addEventListener('click', event => {
  const chip = event.target.closest('[data-kind]');
  if (!chip) return;
  state.histKind = chip.dataset.kind;
  loadHistory(0);
});

el('histPrev').addEventListener('click', () => {
  loadHistory(state.historyOffset - HISTORY_PAGE);
});

el('histNext').addEventListener('click', () => {
  loadHistory(state.historyOffset + HISTORY_PAGE);
});

el('histExport').addEventListener('click', exportHistory);

el('rangeBtn').addEventListener('click', () => {
  state.rangeOpen = !state.rangeOpen;
  renderCalendar();
});

el('cal').addEventListener('click', event => {
  const month = event.target.closest('[data-mon]');

  if (month) {
    const moved = new Date(state.calYear, state.calMonth + Number(month.dataset.mon), 1);
    state.calYear = moved.getFullYear();
    state.calMonth = moved.getMonth();
    renderCalendar();
    return;
  }

  if (event.target.closest('#calClear')) {
    state.rangeStart = null;
    state.rangeEnd = null;
    loadHistory(0);
    return;
  }

  if (event.target.closest('#calApply')) {
    state.rangeOpen = false;
    loadHistory(0);
    return;
  }

  const day = event.target.closest('[data-ts]');
  if (!day) return;

  const ts = Number(day.dataset.ts);

  // First click starts a range, second closes it; a second click before the
  // first moves the start rather than producing an inverted range.
  if (!state.rangeStart || state.rangeEnd) {
    state.rangeStart = ts;
    state.rangeEnd = null;
  } else if (ts >= state.rangeStart) {
    state.rangeEnd = ts;
  } else {
    state.rangeStart = ts;
  }

  // A half-picked range is not a filter yet -- it is redrawn so the calendar
  // shows the choice, but nothing is asked of the gateway until the range
  // closes (or the operator presses Áp dụng).
  if (state.rangeEnd) loadHistory(0); else renderHistory();
});

/* ---------- actions ---------- */

el('btnUnlock').addEventListener('click', () => {
  const locker = selectedLocker();
  if (!locker || !can('unlock')) return;
  post('/api/locker/lockers/' + locker.id + '/unlock', 'Mở khóa');
});

el('btnRelease').addEventListener('click', () => {
  const locker = selectedLocker();
  if (!locker || !can('release')) return;
  if (!confirm('Thu hồi ngăn ' + lockerLabel(locker) + ' của ' +
               (locker.username || locker.card_code || 'người dùng này') + '?')) return;
  post('/api/locker/lockers/' + locker.id + '/release', 'Thu hồi');
});

el('syncBtn').addEventListener('click', () => {
  if (!can('sync')) return;
  post('/api/locker/sync', 'Đồng bộ');
});

/* ---------- closing things ---------- */

document.querySelectorAll('[data-close]').forEach(button => button.addEventListener('click', () => {
  if (button.dataset.close === 'history') state.historyOpen = false;
  if (button.dataset.close === 'perms') state.permsOpen = false;
  render();
}));

el('historyOverlay').addEventListener('click', event => {
  if (event.target === el('historyOverlay')) { state.historyOpen = false; render(); }
});

el('permsOverlay').addEventListener('click', event => {
  if (event.target === el('permsOverlay')) { state.permsOpen = false; render(); }
});

document.addEventListener('click', event => {
  if (state.roleMenuOpen && !event.target.closest('.user-wrap')) {
    state.roleMenuOpen = false;
    renderRoleMenu();
  }
}, true);

document.addEventListener('keydown', event => {
  if (event.key !== 'Escape') return;
  if (state.rangeOpen) { state.rangeOpen = false; renderCalendar(); return; }
  if (state.permsOpen) { state.permsOpen = false; render(); return; }
  if (state.historyOpen) { state.historyOpen = false; render(); return; }
  if (state.selected !== null) { state.selected = null; render(); }
});

/* ================= start ================= */

render();
loadOnce();
connect();

// The countdowns are the only thing on this page that changes without the
// gateway saying anything, so they get their own tick. A minute is fine for a
// figure quoted in hours and minutes, and it keeps a wall display from
// repainting every second all night.
setInterval(() => {
  if (!state.data) return;
  renderGrid();
  renderPanel();
}, 60000);

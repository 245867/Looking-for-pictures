const $ = id => document.getElementById(id);
const invoke = (channel, data = {}) => typeof jade === 'undefined' ? Promise.resolve(null) : Promise.resolve(jade.invoke(channel, JSON.stringify(data)));
function parse(value, fallback) { try { return typeof value === 'string' ? JSON.parse(value) : value; } catch { return fallback; } }
function log(message, type = '') { const el = document.createElement('div'); el.className = type; el.textContent = `[${new Date().toLocaleTimeString()}] ${message}`; $('log').appendChild(el); requestAnimationFrame(() => { $('log').scrollTop = $('log').scrollHeight; }); }
function minimizeWindow() { invoke('win:minimize'); }
function closeWindow() { invoke('win:close'); }
function toggleTopmost() { invoke('win:always_on_top').then(r => { const v = parse(r, r); const active = v === 'top' || v?.value === 'top' || v?.top === true; $('topmostBtn').classList.toggle('active', active); }); }
function act(action) { invoke('find:action', { action }).then(() => { if (action === 'refresh_groups') { loadGroups(); log('图片组已刷新', 'ok'); } else if (action !== 'start' && action !== 'stop') log(`已执行：${action}`, 'ok'); syncLogs(); }); }
function setDetectionRunning(running) { const run = $('runBtn'), stop = $('stopBtn'); if (run) run.textContent = running ? '停止检测' : '启动检测'; if (stop) stop.disabled = !running; }
function toggleDetection() { const running = $('runBtn')?.textContent === '停止检测'; if (running) stopDetection(); else { invoke('find:action', { action: 'start' }).then(() => { setDetectionRunning(true); syncLogs(); }); } }
function stopDetection() { invoke('find:action', { action: 'stop' }).then(() => { setDetectionRunning(false); syncLogs(); }); }
function save() { invoke('find:set', { threshold: $('threshold').value, interval: $('interval').value, cooldown: $('cooldown').value, away: $('away').value, hwnd: $('hwnd').value, force_start: $('forceStart').value, force_end: $('forceEnd').value, click: $('click').checked, flash: $('flash').checked, email: $('email').checked, squeeze: $('squeeze').checked }).then(() => log('配置已保存', 'ok')); }
function clearLogView() { $('log').innerHTML = ''; }
function beginDragPick(event) {
  event.preventDefault(); event.stopPropagation();
  const button = $('pickBtn'); if (!button || button.dataset.picking === '1') return;
  if (event.pointerId !== undefined) button.setPointerCapture?.(event.pointerId);
  button.dataset.picking = '1'; button.textContent = '拖到目标后松开';
  log('瞄准镜已启动：请保持鼠标左键，拖到目标窗口；红色边框闪烁后松开。', 'ok');
  invoke('find:pick_window_drag_begin').then(r => {
    const data = parse(r, {});
    if (!data?.ok) { button.dataset.picking = '0'; button.textContent = '🎯 拖住寻找'; log('窗口选择启动失败', 'err'); }
  }).catch(() => { button.dataset.picking = '0'; button.textContent = '🎯 拖住寻找'; log('窗口选择启动失败', 'err'); });
}
function finishDragPick(event) {
  const button = $('pickBtn'); if (!button || button.dataset.picking !== '1') return;
  event.preventDefault(); event.stopPropagation();
  invoke('find:pick_window_drag_end').then(r => {
    const data = parse(r, {});
    if (data?.ok && data.hwnd) { $('hwnd').value = data.hwnd; log(`已选择目标窗口：${data.hwnd}`, 'ok'); }
    else log('窗口选择已取消', 'warn');
  }).catch(() => log('窗口选择失败', 'err')).finally(() => {
    button.dataset.picking = '0'; button.textContent = '🎯 拖住寻找'; syncLogs();
  });
}
function loadConfig() { invoke('find:get').then(r => { const c = parse(r, {}); ['threshold','interval','cooldown','away','hwnd'].forEach(k => { if (c[k] !== undefined) $(k).value = c[k]; }); if (c.force_start !== undefined) $('forceStart').value = c.force_start; if (c.force_end !== undefined) $('forceEnd').value = c.force_end; ['click','flash','email','squeeze'].forEach(k => { if (c[k] !== undefined) $(k).checked = !!c[k]; }); setDetectionRunning(!!c.running); }); }
let groupSignature = '';
function loadGroups() { invoke('find:groups').then(r => { const box = $('groups'); const groups = parse(r, []); const signature = groups.map(g => `${g.name}:${g.count}:${g.enabled ? 1 : 0}`).join('|'); if (signature === groupSignature) return; groupSignature = signature; box.innerHTML = ''; if (!groups.length) { box.innerHTML = '<span class="loading">没有找到图片组，请检查 img 目录</span>'; return; } groups.forEach(g => { const label = document.createElement('label'); const checkbox = document.createElement('input'); checkbox.type = 'checkbox'; checkbox.checked = !!g.enabled; checkbox.onchange = () => { invoke('find:group_set', { group: g.name, enabled: checkbox.checked }); log(`${checkbox.checked ? '已启用' : '已禁用'}：${g.name}`, 'ok'); }; label.append(checkbox, document.createTextNode(`${g.name} (${g.count}张)`)); box.appendChild(label); }); }); }
function loadSchemes() { const box = $('schemes'); for (let n = 1; n <= 10; n++) { const item = document.createElement('div'); item.className = 'scheme-item'; const select = document.createElement('button'); select.className = 'scheme-select'; select.textContent = n; select.onclick = () => { const wasSelected = select.classList.contains('selected'); document.querySelectorAll('.scheme-select').forEach(x => x.classList.remove('selected')); if (!wasSelected) select.classList.add('selected'); invoke('find:action', { action: `scheme_${n}` }); log(wasSelected ? '已取消窗口截图方案，将使用旧版全屏找图' : `已选择截图方案 ${n}`, 'ok'); }; const test = document.createElement('button'); test.className = 'scheme-test'; test.textContent = '测'; test.onclick = () => act(`test_scheme_${n}`); item.append(select, test); box.appendChild(item); } }
const seenServerLogs = new Set();
function syncLogs() { invoke('find:logs').then(r => { const lines = parse(r, []); lines.forEach(line => { if (seenServerLogs.has(line)) return; seenServerLogs.add(line); log(line.replace(/^\[[^\]]+\]\s*/, ''), line.includes('错误') ? 'err' : line.includes('失败') ? 'warn' : ''); }); }).catch(() => log('无法读取后端日志', 'err')); }
// Use the timestamped file log as the only display source.  Listening to both
// Jade events and the file duplicated every startup/DM message in the UI.
document.addEventListener('DOMContentLoaded', () => { const pick = $('pickBtn'); pick.addEventListener('pointerdown', beginDragPick); pick.addEventListener('pointerup', finishDragPick); pick.addEventListener('pointercancel', finishDragPick); loadConfig(); loadGroups(); loadSchemes(); syncLogs(); setInterval(syncLogs, 500); setInterval(loadGroups, 5000); window.addEventListener('focus', loadGroups); log('JadeView 前端已加载', 'ok'); });

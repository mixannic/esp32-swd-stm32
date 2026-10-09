// =============================================================================
//  web_ui.h -- HTML-страница веб-интерфейса (хранится во flash, отдаётся send_P)
// =============================================================================
#pragma once

static const char WEB_PAGE[] =
R"HTMLPAGE(<!DOCTYPE html>
<html lang="ru">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>ESP32 -&gt; STM32 SWD</title>
<style>
:root{--bg:#12151c;--card:#1b2029;--fg:#e6e9ef;--mut:#93a0b4;--acc:#4fa3ff;--ok:#39c07a;--err:#ff5f56}
*{box-sizing:border-box}
body{margin:0;background:var(--bg);color:var(--fg);font:15px/1.45 system-ui,"Segoe UI",Roboto,sans-serif}
.wrap{max-width:860px;margin:0 auto;padding:16px}
h1{font-size:20px;margin:6px 0 16px}
h2{font-size:13px;margin:0 0 10px;color:var(--mut);text-transform:uppercase;letter-spacing:.07em}
.card{background:var(--card);border:1px solid #262d3a;border-radius:12px;padding:14px;margin-bottom:14px}
button{background:var(--acc);color:#08111c;border:0;border-radius:8px;padding:10px 14px;font-weight:600;cursor:pointer;margin:2px 4px 2px 0}
button.sec{background:#2b3444;color:var(--fg)}
button:disabled{opacity:.4;cursor:not-allowed}
input[type=file]{color:var(--fg);max-width:100%}
label{display:block;margin:5px 0}
select{background:#0e1117;color:var(--fg);border:1px solid #2b3444;border-radius:6px;padding:4px}
pre{background:#0e1117;border:1px solid #262d3a;border-radius:8px;padding:10px;max-height:320px;overflow:auto;font:12px/1.35 ui-monospace,Consolas,monospace;white-space:pre-wrap;margin:0}
progress{width:100%;height:14px;border-radius:7px;display:block}
table{width:100%;border-collapse:collapse;font-size:14px}
td{padding:3px 6px;border-bottom:1px solid #232a36;vertical-align:top}
td:first-child{color:var(--mut);width:38%}
.mut{color:var(--mut)}
.ok{color:var(--ok)} .err{color:var(--err)}
</style>
</head>
<body>
<div class="wrap">
<h1>ESP32 &rarr; STM32F103 (SWD) прошивальщик</h1>

<div class="card">
  <h2>1. Файл прошивки .bin</h2>
  <input type="file" id="fw" accept=".bin,.BIN">
  <div id="fwinfo" class="mut" style="margin-top:8px">файл не загружен</div>
  <progress id="upbar" max="100" value="0" style="display:none;margin-top:8px"></progress>
</div>

<div class="card">
  <h2>2. Цель (STM32)</h2>
  <table id="tinfo"><tr><td>состояние</td><td>не подключено</td></tr></table>
  <div id="fstat" class="mut" style="margin-top:6px">статус флеша не рассчитан</div>
  <div style="margin-top:10px">
    <button onclick="api('connect')">Подключиться / прочитать ID</button>
    <button class="sec" onclick="api('erase')">Массовое стирание</button>
    <button class="sec" onclick="api('flashstat')" id="btnFstat">Статус флеша</button>
  </div>
</div>

<div class="card">
  <h2>3. Прошивка</h2>
  <label>Скорость SWD:
    <select id="speed">
      <option value="8">быстро (~2 МГц)</option>
      <option value="24" selected>нормально (~1 МГц)</option>
      <option value="60">медленно (~400 кГц)</option>
      <option value="150">очень медленно (~150 кГц)</option>
    </select>
  </label>
  <label><input type="checkbox" id="verify" checked> проверять чтением после записи</label>
  <label><input type="checkbox" id="skip" checked> пропускать страницы, которые уже совпадают</label>
  <label><input type="checkbox" id="reset" checked> сбросить и запустить после прошивки</label>
  <div style="margin-top:10px">
    <button onclick="flash()" id="btnFlash">Прошить</button>
    <button class="sec" onclick="api('cancel')" id="btnCancel" disabled>Отмена</button>
  </div>
</div>

<div class="card">
  <h2>4. Чтение флеша</h2>
  <div class="mut" id="dlinfo">выгрузка содержимого флеша STM32 в файл .bin (сохранится на компьютер)</div>
  <progress id="dlbar" max="100" value="0" style="display:none;margin-top:8px"></progress>
  <div style="margin-top:10px">
    <button class="sec" onclick="dumpFlash()" id="btnRead">Выгрузить флеш (.bin)</button>
  </div>
</div>

<div class="card">
  <h2>Прогресс</h2>
  <progress id="bar" max="100" value="0"></progress>
  <div id="stage" class="mut" style="margin-top:8px">ожидание</div>
</div>

<div class="card">
  <h2>Лог</h2>
  <pre id="log"></pre>
</div>
<p class="mut" style="font-size:12px">ESP32: <span id="ip">?</span> <span id="rssi"></span></p>
</div>
<script>
var $=function(id){return document.getElementById(id);};
function esc(s){return String(s).replace(/&/g,'&amp;').replace(/</g,'&lt;').replace(/>/g,'&gt;');}
function r2(k,v){return '<tr><td>'+k+'</td><td>'+v+'</td></tr>';}
async function jget(u){var r=await fetch(u,{cache:'no-store'});return await r.json();}
async function api(a){
  if(a==='erase' && !confirm('Стереть ВЕСЬ флеш STM32? Операция необратима.')) return;
  try{
    var r=await fetch('/api/'+a,{method:'POST'});
    if(!r.ok){ var e=await r.json(); alert(e.error||'не удалось запустить операцию'); }
  }catch(e){}
  poll();
}
function upload(f){
  return new Promise(function(res){
    var xhr=new XMLHttpRequest();
    xhr.open('POST','/api/upload');
    $('upbar').style.display='block'; $('upbar').value=0;
    xhr.upload.onprogress=function(e){ if(e.lengthComputable) $('upbar').value=Math.round(e.loaded*100/e.total); };
    var done=function(){ setTimeout(function(){$('upbar').style.display='none';},500); res(); };
    xhr.onload=done; xhr.onerror=done; xhr.onabort=done;
    var fd=new FormData(); fd.append('firmware',f,f.name);
    xhr.send(fd);
  });
}
$('fw').addEventListener('change', async function(e){
  var f=e.target.files[0]; if(!f) return;
  await upload(f); poll();
});
async function flash(){
  var q='?verify='+($('verify').checked?1:0)+'&skip='+($('skip').checked?1:0)+
        '&reset='+($('reset').checked?1:0)+'&speed='+$('speed').value;
  $('btnFlash').disabled=true;
  try{
    var r=await fetch('/api/flash'+q,{method:'POST'});
    if(!r.ok){ var e=await r.json(); alert(e.error||'не удалось запустить прошивку'); }
  }catch(e){}
  poll();
}
var tmr=null;
function dumpFlash(){
  var xhr=new XMLHttpRequest();
  xhr.open('GET','/api/readflash');
  xhr.responseType='blob';
  $('dlbar').style.display='block'; $('dlbar').value=0;
  $('dlinfo').textContent='чтение флеша, подождите...';
  xhr.onprogress=function(e){ if(e.lengthComputable) $('dlbar').value=Math.round(e.loaded*100/e.total); };
  xhr.onload=function(){
    $('dlbar').style.display='none';
    if(xhr.status===200){
      var cd=xhr.getResponseHeader('Content-Disposition')||'';
      var m=/filename="?([^";]+)"?/.exec(cd);
      var name=(m&&m[1])?m[1]:'stm32_flash.bin';
      var a=document.createElement('a');
      a.href=URL.createObjectURL(xhr.response);
      a.download=name;
      document.body.appendChild(a); a.click(); a.remove();
      setTimeout(function(){URL.revokeObjectURL(a.href);},10000);
      $('dlinfo').textContent='сохранено: '+name+' ('+xhr.response.size+' байт)';
    } else {
      xhr.response.text().then(function(t){
        var e={}; try{e=JSON.parse(t);}catch(x){}
        var msg=e.error||('HTTP '+xhr.status);
        $('dlinfo').textContent='ошибка: '+msg;
        alert('Не удалось выгрузить флеш: '+msg);
      });
    }
    poll();
  };
  xhr.onerror=function(){
    $('dlbar').style.display='none';
    $('dlinfo').textContent='ошибка соединения при выгрузке';
  };
  xhr.send();
}
async function poll(){
  clearTimeout(tmr);
  try{
    var s=await jget('/api/status');
    $('ip').textContent=s.ip||'?';
    $('rssi').textContent=(s.rssi!==undefined?('('+s.rssi+' dBm)'):'');
    if(s.fw && s.fw.exists){
      $('fwinfo').innerHTML='загружен: <b>'+esc(s.fw.name)+'</b>, '+s.fw.size+' байт, CRC32 0x'+s.fw.crc;
    } else {
      $('fwinfo').textContent='файл не загружен';
    }
    var t=s.target||{}, rows='';
    if(t.valid){
      rows+=r2('DPIDR','0x'+t.idcode);
      rows+=r2('DEV_ID',t.dev+', rev 0x'+t.rev+' &mdash; '+esc(t.density));
      rows+=r2('Flash',t.flash+' КБ (страница '+t.page+' Б)');
      rows+=r2('Защита (RDP)',esc(t.rdp));
      rows+=r2('AHB-AP IDR','0x'+t.apidr);
    } else {
      rows=r2('состояние','<span class="mut">не подключено</span>');
    }
    $('tinfo').innerHTML=rows;
    var f=s.fstat||{};
    if(f.valid){
      $('fstat').innerHTML='флеш: всего <b>'+f.total+'</b> байт &middot; занято <b>'+f.used+
                           '</b> байт &middot; свободно <b>'+f.free+'</b> байт';
    } else {
      $('fstat').textContent='статус флеша не рассчитан (нажмите «Статус флеша»)';
    }
    $('bar').value=s.pct||0;
    var st=esc(s.stage||'');
    if(s.result==='ok') st+=' &mdash; <b class="ok">OK</b>';
    if(s.result==='err') st+=' &mdash; <b class="err">ОШИБКА</b>';
    if(s.msg) st+='<br>'+esc(s.msg);
    $('stage').innerHTML=st;
    $('log').textContent=s.log||'';
    $('log').scrollTop=$('log').scrollHeight;
    $('btnFlash').disabled=!!s.busy;
    $('btnCancel').disabled=!s.busy;
    $('btnRead').disabled=!!s.busy;
    $('btnFstat').disabled=!!s.busy;
  }catch(e){}
  tmr=setTimeout(poll,700);
}
poll();
</script>
</body>
</html>
)HTMLPAGE";

#!/usr/bin/env python3
"""Create an offline shader profiling report from a GPU Support dump.

The report is intended for platforms implementing
VK_KHR_pipeline_executable_properties. It preserves vendor-defined executable
statistics and recognizes the current QCOM and Mali statistic conventions.
"""

import argparse
import csv
import json
import io
import os
import re
from collections import defaultdict
from pathlib import Path
from urllib.parse import quote


def sha(value):
    return value.removeprefix('sha1:')


def label_group(label):
    if not label:
        return '<unlabelled>'
    value = re.sub(r'^Frame\s+\d+/', '', label)
    value = re.sub(r'\s+Mesh=.*$', '', value)
    value = re.sub(r'\s+Num(?:Lights|Captures|PrimitiveDataUploads)\s+\d+.*$', '', value)
    return value or '<unlabelled>'


def metric_role(name, mali):
    name = name.lower()
    if mali:
        return {
            'spill size': 'scratch',
            'registers allocated': 'register',
            'registers used': 'register_used',
            'tls size': 'tls',
            'wls size': 'wls',
            'arithmetic': 'arithmetic',
            'texture': 'texture',
            'load store': 'load_store',
            'varying': 'varying',
        }.get(name)
    if 'scratch memory usage' in name:
        return 'scratch'
    if 'overall register footprint' in name:
        return 'register'
    if name == 'instruction count all':
        return 'instructions'
    if name == 'alu fiber occupancy percentage':
        return 'occupancy'
    if name == 'texture read instruction count':
        return 'texture'
    if name in ('memory read instruction count', 'memory write instruction count'):
        return name.removeprefix('memory ').replace(' instruction count', '')
    if name in ('alu instruction count 32bit', 'alu instruction count 16bit'):
        return 'alu'
    return None


def as_number(value):
    try:
        return float(value)
    except (ValueError, TypeError):
        return None


def percentile(values, value):
    return round(100 * sum(item <= value for item in values) / len(values)) if values else 0


def read_pipelines(root):
    result = {}
    for path in sorted((root / 'pipelines').glob('*.json')):
        data = json.loads(path.read_text(encoding='utf-8'))
        key = sha(data.get('pipeline_signature', ''))
        if not key:
            continue
        item = result.setdefault(key, {'kind': data.get('kind', 'unknown'), 'shaders': set(),
                                       'executables': [], 'capture_status': 'unknown'})
        item['kind'] = data.get('kind', item['kind'])
        item['capture_status'] = data.get('capture_status', item['capture_status'])
        item['executables'].extend(data.get('executables', []))
        for shader in data.get('shaders', []):
            if shader.get('sha1'):
                item['shaders'].add((shader['sha1'], shader.get('stage', 'unknown'), shader.get('entry_point', '')))
    return result


def read_usage(root):
    result = defaultdict(lambda: {'draw_count': 0, 'dispatch_count': 0, 'trace_rays_count': 0,
                                  'frames': set(), 'labels': set()})
    path = root / 'usage_by_label.csv'
    if not path.exists():
        return result
    with path.open(newline='', encoding='utf-8') as handle:
        for row in csv.DictReader(handle):
            item = result[sha(row.get('pipeline_signature', ''))]
            for field in ('draw_count', 'dispatch_count', 'trace_rays_count'):
                item[field] += int(row.get(field, 0) or 0)
            if row.get('frame'):
                item['frames'].add(int(row['frame']))
            item['labels'].add(row.get('label_path') or '<unlabelled>')
    return result


def write_csv(path, fields, rows):
    buffer = io.StringIO(newline='')
    writer = csv.DictWriter(buffer, fieldnames=fields, extrasaction='ignore')
    writer.writeheader()
    writer.writerows(rows)
    content = buffer.getvalue()
    if path.exists() and path.read_text(encoding='utf-8') == content.replace('\r\n', '\n'):
        return
    with path.open('w', newline='', encoding='utf-8') as handle:
        handle.write(content)


def write_html(path, report):
    data = json.dumps(report, ensure_ascii=False).replace('</', r'<\/')
    document = r'''<!doctype html><html lang="en"><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>Pipeline Executable Shader Profiling</title>
<style>
:root{color-scheme:dark;--bg:#0d131c;--panel:#151f2c;--line:#29394c;--text:#e8eef6;--muted:#91a1b5;--blue:#194f89;--green:#176849;--orange:#9a5715;--amber:#765319;--red:#812f3d}*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--text);font:13px system-ui,sans-serif}button,input,select{font:inherit}button{color:inherit;background:#213148;border:1px solid #3a506d;border-radius:5px;padding:.38rem .6rem;cursor:pointer}button:hover,tr.click:hover{background:#304563}a{color:#87c4ff}.mono{font-family:ui-monospace,Consolas,monospace}.muted{color:var(--muted)}.wrap{overflow-wrap:anywhere}header{padding:1rem max(1.25rem,calc((100vw - 1550px)/2));border-bottom:1px solid var(--line)}h1{margin:0;font-size:1.25rem}h2{font-size:1rem;margin:.35rem 0}.status{font-size:14px;display:flex;gap:1rem;flex-wrap:wrap;margin-top:.4rem;color:var(--muted)}.status b{color:var(--text);font-size:16px}.tag.vertex{background:#ad650f;color:#fff2cc}.tag.fragment{background:var(--blue)}main{max-width:1550px;margin:auto;padding:1rem 1.25rem 3rem}.tabs{display:flex;gap:0;align-items:end;margin:0;border-bottom:1px solid var(--line);padding-left:.35rem;background:#111a25}.tab{position:relative;border-radius:6px 6px 0 0;border:1px solid transparent;border-bottom:0;background:transparent;color:var(--muted);padding:.55rem 1rem;margin:0}.tab:hover{background:#1b2838;color:var(--text)}.tab.active{background:var(--panel);color:var(--text);border-color:var(--line);margin-bottom:-1px;z-index:1}.toolbar{display:flex;gap:.5rem;align-items:center;flex-wrap:wrap;margin:0 0 .7rem;padding:.7rem;background:var(--panel);border:1px solid var(--line);border-top:0;border-radius:0 0 7px 7px}.kind-group{display:flex;gap:0}.kind-group .kind{border-radius:0;margin-left:-1px}.kind-group .kind:first-child{border-radius:5px 0 0 5px;margin-left:0}.kind-group .kind:last-child{border-radius:0 5px 5px 0}.kind.active.graphics,.graphics{background:var(--blue)}.kind.active.compute,.compute{background:var(--green)}.kind.active.ray_tracing,.ray_tracing{background:var(--orange)}.priority.active{background:var(--amber)}input,select{background:#0c121a;color:var(--text);border:1px solid #354a65;border-radius:5px;padding:.45rem .55rem}input[type="checkbox"]{min-width:0}input:not([type="checkbox"]){min-width:23rem;max-width:100%}.table-wrap{overflow:auto;border:1px solid var(--line);border-radius:7px;max-height:72vh}table{border-collapse:collapse;width:100%;background:var(--panel)}th,td{padding:.47rem .55rem;border-bottom:1px solid var(--line);text-align:left;vertical-align:top;white-space:nowrap}th{position:sticky;top:0;background:#1d2a3a;z-index:1}th button{padding:0;border:0;background:none;font-weight:700}.click{cursor:pointer}.tag{display:inline-block;border-radius:3px;padding:.12rem .38rem;margin:0 .2rem .15rem 0;font-size:.76rem;font-weight:700;letter-spacing:.02em}.risk{background:var(--red)}.warn{background:var(--amber)}.neutral{background:#35465c}.pressure{background:#56385d}.leaf{max-width:230px;overflow:hidden;text-overflow:ellipsis}.hidden{display:none!important}#detail{position:fixed;right:0;top:0;width:min(820px,100vw);height:100%;z-index:5;overflow:auto;background:#101a26;border-left:1px solid #3d5573;box-shadow:-10px 0 32px #000a;padding:1rem}.close{float:right}.detail-head{display:flex;gap:.5rem;align-items:center;flex-wrap:wrap}.section{margin:1rem 0}.metric-panel{position:sticky;bottom:0;background:#202f41;border:1px solid #486282;padding:.7rem;border-radius:6px;margin-top:.6rem}.empty{padding:2rem;text-align:center;color:var(--muted)}@media(max-width:700px){main{padding:.75rem}.tabs{overflow:auto}header{padding:.8rem}input:not([type="checkbox"]){min-width:100%}th,td{padding:.4rem}}
.status{gap:.6rem;align-items:stretch}.status-group{display:flex;flex-wrap:wrap;gap:.6rem 1rem;padding:.5rem .7rem;background:var(--panel);border:1px solid var(--line);border-radius:6px}.status-group>span{display:inline-flex;align-items:center;gap:.4rem;white-space:nowrap}.status-group>span+span{border-left:1px solid var(--line);padding-left:1rem}.status span[title]{cursor:help}.status-scope{font-size:12px;color:var(--muted);align-self:center}.results{margin:.6rem 0;color:var(--muted)}.copy-sha{font:11px system-ui;padding:.15rem .3rem;margin-left:.4rem}.detail-nav{display:flex;justify-content:flex-end;margin-bottom:.7rem}.help-link{padding:0 .2rem;border:0;background:transparent;color:#87c4ff;font-size:15px}.help-page{max-width:1000px;line-height:1.65}.help-page section{margin:1.5rem 0;padding:1rem;background:var(--panel);border:1px solid var(--line);border-radius:7px;scroll-margin-top:1rem}.help-page h2{font-size:18px}.help-page details{margin-top:.7rem}.help-page summary{cursor:pointer;color:#87c4ff}.help-table{overflow:auto}.help-table th,.help-table td{white-space:normal;min-width:110px}.help-table td:nth-child(2){min-width:240px}.help-contents{display:flex;flex-wrap:wrap;gap:.5rem;margin-top:1rem}tr.selected{background:#213d57;box-shadow:inset 3px 0 #87c4ff}tr[data-shader]:focus-visible{outline:2px solid #87c4ff;outline-offset:-2px}.pass-cell{display:inline-flex;align-items:center;gap:.25rem}.pass-cell .tag{margin-bottom:0}.pass-more{padding:.12rem .35rem;font-size:12px}.pass-popover{background:var(--panel);color:var(--text);border:1px solid var(--line);border-radius:7px;padding:1rem;width:min(600px,90vw);max-height:65vh;overflow:auto;white-space:normal;box-shadow:0 8px 32px #000a}.pass-popover>div{padding:.4rem 0;border-top:1px solid var(--line)}.pass-popover::backdrop{background:#0003}th.sortable{cursor:pointer;padding:0}th.sortable button{display:block;width:100%;min-height:2.3rem;padding:.47rem .55rem;text-align:left;border-radius:0}th.sortable:hover{background:#304563}th.sortable:focus-within{outline:2px solid #87c4ff;outline-offset:-2px}.unused-toggle{display:inline-flex;align-items:center;justify-content:center;width:34px;height:32px;padding:5px}.unused-toggle.active,.filter-tag.active{background:#194f89;border-color:#87c4ff;color:#fff}.tag-filters{display:flex;flex-wrap:wrap;gap:.3rem;align-items:center}.filter-tag{font-size:11px;padding:.3rem .4rem}.filter-tag:focus-visible,.unused-toggle:focus-visible{outline:2px solid #87c4ff;outline-offset:2px}.clear-tags{display:inline-flex;align-items:center;justify-content:center;padding:.3rem;width:28px;height:27px}.clear-tags[aria-disabled="true"]{opacity:.4}.clear-tags:focus-visible{outline:2px solid #87c4ff;outline-offset:2px}.referencing-head{display:flex;align-items:center;gap:.6rem;flex-wrap:nowrap}.referencing-head h3{margin:0}.referencing-head .unused-toggle{flex-shrink:0}.kind.active.vertex{background:#ad650f;color:#fff2cc}.kind.active.fragment{background:var(--blue)}</style><body><header><h1 id="report-title">GPU Shader Profiling Template</h1><div class="status" id="status"></div></header><main><nav class="tabs" id="tabs"></nav><section class="toolbar" id="toolbar"></section><div id="results" class="results" role="status" aria-live="polite"></div><section id="content"></section></main><aside id="detail" class="hidden"></aside><script id="report-data" type="application/json">__DATA__</script>
<script>
const D=JSON.parse(document.querySelector('#report-data').textContent),$=s=>document.querySelector(s),esc=s=>String(s??'').replace(/[&<>"']/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c])),n=v=>Number(v||0).toLocaleString(),short=s=>s?`${s.slice(0,12)}…`:'',leaf=s=>(s||'<unlabelled>').split('/').pop(),kindClass=k=>k.replace(/[^a-z0-9]+/gi,'_').toLowerCase(),kindLabel=k=>k.replace(/[_-]/g,' '),kindTitle=k=>kindLabel(k).replace(/\b\w/g,c=>c.toUpperCase()),typeTag=k=>`<span class="tag ${kindClass(k)}">${esc(kindLabel(k).toUpperCase())}</span>`;
const P=new Map(D.pipelines.map(x=>[x.pipeline_signature,x])),S=new Map(D.shaders.map(x=>[x.shader_sha1,x])),LP=new Map(),LS=new Map();for(const l of D.links){(LP.get(l.pipeline_signature)||LP.set(l.pipeline_signature,[]).get(l.pipeline_signature)).push(l);(LS.get(l.shader_sha1)||LS.set(l.shader_sha1,[]).get(l.shader_sha1)).push(l)}
const used=p=>p.command_count>0||p.active_frames>0;
function shaCell(id){return `<span class="mono" title="${esc(id)}">${esc(short(id))}</span>`}
let detailOrigin=null,selectedShader=null;
function navigateDetail(){if($('#detail').classList.contains('hidden'))detailOrigin=document.activeElement}
function detailNav(){return `<div class="detail-nav"><button onclick="closeDetail()">Close (Esc)</button></div>`}
function focusShaderRow(id){document.querySelectorAll('[data-shader]').forEach(row=>{const selected=row.dataset.shader===id;row.classList.toggle('selected',selected);row.setAttribute('aria-selected',String(selected));if(selected){row.focus({preventScroll:true});row.scrollIntoView({block:'nearest',inline:'nearest'})}})}
document.addEventListener('keydown',e=>{if(e.key==='Escape'&&!$('#detail').classList.contains('hidden')){e.preventDefault();closeDetail()}});
let order={key:'command_count',dir:-1},shaderOrder={key:'pipeline_command_count',dir:-1};
const textKeys=new Set(['pipeline_signature','kind','label_groups','pressures']);
const val=(p,k)=>textKeys.has(k)?(Array.isArray(p[k])?p[k].join('; '):String(p[k]||'')):(p.metrics?.[k]?Number(p.metrics[k].value):p[k]??null);
function sort(rows,o){return [...rows].sort((a,b)=>{const x=val(a,o.key),y=val(b,o.key);if(x===null||y===null)return x===y?0:x===null?1:-1;return (typeof x==='number'?x-y:x.localeCompare(y))*o.dir||a.pipeline_signature.localeCompare(b.pipeline_signature)})}
const head=(label,key)=>`<th class="sortable" data-sort="${key}" aria-sort="${order.key===key?(order.dir<0?'descending':'ascending'):'none'}" onclick="setOrder('${key}')"><button>${label}${order.key===key?(order.dir<0?' ↓':' ↑'):''}</button></th>`;
function tags(p){const tags=p.pressures||[];return tags.map(t=>`<span class="tag ${t==='SCRATCH'||t==='SPILL'||t==='LOW-OCCUPANCY'?'risk':t==='MIXED'||t==='TLS'||t==='WLS'?'warn':'pressure'}">${esc(t)}</span>`).join('')||'<span class="muted">—</span>'}let passPopoverId=0;function pass(p){const names=p.label_groups||[];if(!names.length)return '<span class="muted">—</span>';const id=`pass-list-${++passPopoverId}`;return `<span class="pass-cell"><span class="tag neutral leaf" title="${esc(names[0])}">${esc(leaf(names[0]))}</span>${names.length>1?`<button class="pass-more" popovertarget="${id}" aria-label="Show ${names.length-1} More Passes" onclick="event.stopPropagation()">+${names.length-1}</button><div id="${id}" class="pass-popover" popover onclick="event.stopPropagation()"><h3>Additional Passes</h3>${names.slice(1).map(name=>`<div class="mono wrap">${esc(name)}</div>`).join('')}</div>`:''}</span>`}function metric(p,key){const x=p.metrics?.[key];return x?`<span title="${esc(x.name)}">${n(x.value)}</span>`:'—'}
function setOrder(key){const source=document.activeElement?.closest('th[data-sort]')?.closest('#shader-pipelines')?'#shader-pipelines':'#content';const positions=['#content .table-wrap','#shader-pipelines .table-wrap'].map(selector=>{const el=$(selector);return {selector,left:el?.scrollLeft||0,top:el?.scrollTop||0}});order={key,dir:order.key===key?-order.dir:(textKeys.has(key)?1:-1)};render(false);if(currentShader)renderShaderPipelines();for(const pos of positions){const el=$(pos.selector);if(el){el.scrollLeft=pos.left;el.scrollTop=pos.top}}$(`${source} th[data-sort="${key}"] button`)?.focus({preventScroll:true})}function triage(rows,title='Priority triage'){const r=sort(rows,order),cols=D.summary_metrics||[];return `<div class="table-wrap"><table><thead><tr>${head('Pipeline SHA1','pipeline_signature')}${head('Type','kind')}${head('Pass','label_groups')}${head('Flags','pressures')}${head('Commands','command_count')}${head('Frames','active_frames')}${cols.map(x=>head(x.label,x.key)).join('')}</tr></thead><tbody>${r.map((p,i)=>`<tr class="click" onclick="openPipeline('${p.pipeline_signature}')"><td class="mono">${shaCell(p.pipeline_signature)}</td><td>${typeTag(p.kind)}</td><td>${pass(p)}</td><td>${tags(p)}</td><td>${n(p.command_count)}</td><td>${n(p.active_frames)}</td>${cols.map(x=>`<td>${metric(p,x.key)}</td>`).join('')}</tr>`).join('')||`<tr><td colspan="${6+cols.length}" class="empty">No matching pipelines.</td></tr>`}</tbody></table></div>`}
function shaderSortValue(s,key){if(key==='label_groups')return s.label_groups.join('; ');if(key==='stage'||key==='entry_point')return [...new Set(s.uses.map(u=>u[key]))].sort().join('; ');return s[key]}function setShaderOrder(key){const wrap=$('#content .table-wrap'),left=wrap?.scrollLeft||0,top=wrap?.scrollTop||0;shaderOrder={key,dir:shaderOrder.key===key?-shaderOrder.dir:(key==='pipeline_command_count'||key==='pipeline_count'?-1:1)};render(false);const next=$('#content .table-wrap');if(next){next.scrollLeft=left;next.scrollTop=top}$(`#content th[data-sort="${key}"] button`)?.focus({preventScroll:true})}window.setShaderOrder=setShaderOrder;function shaderHead(label,key){const active=shaderOrder.key===key;return `<th class="sortable" data-sort="${key}" onclick="setShaderOrder('${key}')" aria-sort="${active?(shaderOrder.dir<0?'descending':'ascending'):'none'}"><button>${label}${active?(shaderOrder.dir<0?' ↓':' ↑'):''}</button></th>`}
function shaderTable(rows){rows=[...rows].sort((a,b)=>{const x=shaderSortValue(a,shaderOrder.key),y=shaderSortValue(b,shaderOrder.key);return (typeof x==='number'?x-y:String(x).localeCompare(String(y)))*shaderOrder.dir||a.shader_sha1.localeCompare(b.shader_sha1)});return `<div class="table-wrap"><table><thead><tr>${shaderHead('Shader SHA1','shader_sha1')}${shaderHead('Stage','stage')}${shaderHead('Entry Point','entry_point')}${shaderHead('Pass','label_groups')}${shaderHead('Inherited Commands','pipeline_command_count')}${shaderHead('Pipeline Variants','pipeline_count')}</tr></thead><tbody>${rows.map(s=>`<tr class="click ${selectedShader===s.shader_sha1?'selected':''}" tabindex="0" data-shader="${esc(s.shader_sha1)}" aria-selected="${selectedShader===s.shader_sha1}" onkeydown="if(event.key==='Enter'||event.key===' '){event.preventDefault();openShader('${s.shader_sha1}')}" onclick="openShader('${s.shader_sha1}')"><td class="mono" title="${esc(s.shader_sha1)}">${esc(short(s.shader_sha1))}</td><td>${[...new Set(s.uses.map(u=>u.stage))].map(typeTag).join(' ')}</td><td>${[...new Set(s.uses.map(u=>u.entry_point))].map(esc).join('<br>')}</td><td>${pass(s)}</td><td>${n(s.pipeline_command_count)}</td><td>${s.pipeline_count}</td></tr>`).join('')||'<tr><td colspan="6" class="empty">No matching shaders.</td></tr>'}</tbody></table></div>`}
const preferredStats=D.detail_order||[];let selectedMetric='',metricSort='recommended',selectedExecutable=0;
function statValue(v){const n=Number(v);return Number.isFinite(n)?n:-Infinity}function statRows(executables){const byName=new Map();for(const exe of executables)for(const stat of exe.statistics){const row=byName.get(stat.name)||{name:stat.name,description:stat.description||'',values:{}};row.values[exe.index]=stat.value;byName.set(stat.name,row)}return [...byName.values()]}
function chooseMetric(name){selectedMetric=name;renderStats()}function selectExecutable(index){selectedExecutable=index;metricSort='selected';renderStats()}function selectMetricOrder(value){metricSort=value;renderStats()}function renderStats(){const box=$('#stats');if(!box)return;const executables=box._executables||[],rows=statRows(executables),rank=name=>{const i=preferredStats.indexOf(name);return i<0?1000:i};rows.sort((a,b)=>{if(metricSort==='max')return Math.max(...Object.values(b.values).map(statValue))-Math.max(...Object.values(a.values).map(statValue));if(metricSort==='spread'){const spread=x=>{const v=Object.values(x.values).map(statValue);return Math.max(...v)-Math.min(...v)};return spread(b)-spread(a)}if(metricSort==='selected')return statValue(b.values[selectedExecutable])-statValue(a.values[selectedExecutable]);return rank(a.name)-rank(b.name)||a.name.localeCompare(b.name)});const current=rows.find(x=>x.name===selectedMetric)||rows[0];box.innerHTML=`<div class="toolbar"><label class="muted">Sort metric rows: <select onchange="selectMetricOrder(this.value)"><option value="recommended" ${metricSort==='recommended'?'selected':''}>Recommended order</option><option value="max" ${metricSort==='max'?'selected':''}>Max across executables</option><option value="spread" ${metricSort==='spread'?'selected':''}>Largest spread</option><option value="selected" ${metricSort==='selected'?'selected':''}>Selected executable</option></select></label><span class="muted">Click an executable header to select it; click a metric row for its description.</span></div><div class="table-wrap"><table><thead><tr><th>Statistic</th>${executables.map(e=>`<th><button onclick="selectExecutable(${e.index})">${esc(e.name||'Executable')} #${e.index}${selectedExecutable===e.index?' ↓':''}</button></th>`).join('')}</tr></thead><tbody>${rows.map(r=>`<tr class="click" onclick='chooseMetric(${JSON.stringify(r.name)})'><td>${esc(r.name)}</td>${executables.map(e=>`<td>${r.values[e.index]===undefined?'—':esc(r.values[e.index])}</td>`).join('')}</tr>`).join('')}</tbody></table></div><div class="metric-panel"><b>${esc(current?.name||'No statistics')}</b><div class="muted">${esc(current?.description||'The driver supplied no description for this statistic.')}</div></div>`}window.chooseMetric=chooseMetric;window.selectExecutable=selectExecutable;window.selectMetricOrder=selectMetricOrder;
let currentShader=null,showUnusedShaderPipelines=false;function renderShaderPipelines(){const ids=new Set((LS.get(currentShader)||[]).map(l=>l.pipeline_signature)),rows=[...ids].map(id=>P.get(id)).filter(p=>p&&(showUnusedShaderPipelines||p.command_count>0||p.active_frames>0));$('#shader-pipelines').innerHTML=`<div class="results">Showing ${n(rows.length)} / ${n(ids.size)} pipelines</div>`+triage(rows,'')}function toggleUnusedShaderPipelines(value){showUnusedShaderPipelines=value;const button=$('#detail .referencing-head .unused-toggle');if(button){button.classList.toggle('active',value);button.setAttribute('aria-pressed',String(value))}renderShaderPipelines()}window.toggleUnusedShaderPipelines=toggleUnusedShaderPipelines;function openPipeline(id){const p=P.get(id);if(!p)return;navigateDetail();currentShader=null;const links=LP.get(id)||[],executables=D.statistics[id]||[];$('#detail').innerHTML=`${detailNav()}<div class="detail-head"><h2>Pipeline</h2>${typeTag(p.kind)}${tags(p)}</div><p class="mono wrap">${esc(id)}</p><div class="section"><b>${n(p.command_count)} CPU commands</b> · ${n(p.active_frames)} active frames<br>${pass(p)}<details><summary>Raw labels (${p.labels.length})</summary><p class="wrap">${p.labels.map(esc).join('<br>')}</p></details></div><div class="section"><h3>Shader Candidates</h3><p class="muted">Pipeline pressure is executable-level; these are related stages, not per-stage attribution.</p>${links.map(l=>`<div><button class="mono" onclick="openShader('${l.shader_sha1}')">${esc(short(l.shader_sha1))}</button> ${typeTag(l.stage)} ${esc(l.entry_point)} · <a href="../shaders/${l.shader_sha1}.spirv">SPIR-V</a></div>`).join('')||'<p class="muted">No shader metadata.</p>'}</div><div class="section"><h3>Executable Statistics Matrix</h3><div id="stats"></div></div>`;const box=$('#stats');box._executables=executables;selectedMetric='';metricSort='recommended';selectedExecutable=executables[0]?.index??0;renderStats();$('#detail').classList.remove('hidden');$('#detail').scrollTop=0;$('#detail .detail-nav button:not([disabled])').focus();location.hash=`pipeline/${id}`}function openShader(id){if(!S.has(id))return;navigateDetail();selectedShader=id;currentShader=id;showUnusedShaderPipelines=false;const s=S.get(id),variants=[...new Set((LS.get(id)||[]).map(l=>l.pipeline_signature))].map(x=>P.get(x)).filter(Boolean);if(!s)return;$('#detail').innerHTML=`${detailNav()}<h2>Shader Candidate</h2><p class="mono wrap">${esc(id)}</p><p><a href="../shaders/${id}.spirv">Open SPIR-V</a> · ${n(s.pipeline_command_count)} inherited commands · ${variants.length} pipeline variants</p><div class="section"><div class="referencing-head"><h3>Referencing Pipelines</h3>${unusedButton(showUnusedShaderPipelines,'toggleUnusedShaderPipelines(!showUnusedShaderPipelines)')}</div><div id="shader-pipelines"></div></div>`;$('#detail').classList.remove('hidden');$('#detail').scrollTop=0;$('#detail .detail-nav button:not([disabled])').focus();renderShaderPipelines();focusShaderRow(id);location.hash=`shader/${id}`}function closeDetail(){currentShader=null;if(detailOrigin?.isConnected)detailOrigin.focus();$('#detail').classList.add('hidden');history.replaceState(null,'',location.pathname+location.search)}window.openPipeline=openPipeline;window.openShader=openShader;window.closeDetail=closeDetail;
function helpLink(section,label){return `<button class="help-link" title="View explanation in Help" aria-label="Help: ${esc(label)}" onclick="openHelp('${section}')">ⓘ</button>`}
function openHelp(section){if(!$('#detail').classList.contains('hidden'))closeDetail();setTab('help');const target=document.getElementById('help-'+section);if(target){target.scrollIntoView({block:'start'});target.focus({preventScroll:true})}}window.openHelp=openHelp;
function helpPage(){return `<article class="help-page">
<nav class="help-contents" aria-label="Help Sections"><button onclick="openHelp('scope')">Data Scope</button><button onclick="openHelp('priority')">Flags</button><button onclick="openHelp('fields')">Fields &amp; Navigation</button></nav>
<section id="help-scope" tabindex="-1"><h2>Data Scope</h2><p><b>Usage counts are not GPU timings.</b> Flags are investigation hints, not performance diagnoses.</p><ul><li><b>Capture Totals:</b> all captured records.</li><li><b>Used Pipelines:</b> pipelines with recorded commands or frames; overview type counts use this scope.</li><li><b>Unused:</b> no usage recorded in this capture window. Enable the list-plus icon (Show Unused Pipelines) to include them.</li></ul><p>Overview counts stay fixed; Showing X / Y reflects table filters.</p></section>
<section id="help-priority" tabindex="-1"><h2>Flags</h2><p><b>Flags are capture-relative heuristics, not verified bottlenecks.</b> With Pressure Flags keeps any flagged pipeline. Toggle tags beside the search box to match any selected flag; no tags selected means no tag filter. Clear Tags resets only tag selections. Search and Type filters still apply.</p><p>Tables open with Commands sorted highest first. Every column supports sorting; click anywhere in its header cell or focus its button and press Enter or Space.</p><details><summary>Pressure Flag Rules</summary><div class="help-table"><table><thead><tr><th>Flag</th><th>Condition</th></tr></thead><tbody><tr><td>SPILL / SCRATCH</td><td>Spill/scratch &gt; 0.</td></tr><tr><td>REG-PRESSURE / ALU-HEAVY / TEXTURE-HEAVY</td><td>Corresponding metric percentile ≥ 90.</td></tr><tr><td>TLS / WLS / MEMORY-HEAVY</td><td>Metric &gt; 0 and percentile ≥ 90. Memory: Mali load-store or QCOM reads/writes.</td></tr><tr><td>LOW-OCCUPANCY</td><td>QCOM occupancy percentile ≤ 25.</td></tr><tr><td>MIXED</td><td>At least two HEAVY flags.</td></tr></tbody></table></div></details><details><summary>Percentiles And Tie-Breaking</summary><p>Metric percentiles compare pipelines providing that metric, using its maximum across executables. A percentile counts values ≤ the current value, including ties.</p><p>Equal values use SHA order. Missing metrics stay at the bottom in either direction. Pass and Flags sort alphabetically by their full displayed-order lists. CSV summaries contain usage counts without ranks. Unused pipelines with metrics are included in metric percentiles.</p></details></section>
<section id="help-fields" tabindex="-1"><h2>Fields &amp; Navigation</h2><div class="help-table"><table><thead><tr><th>Field</th><th>Meaning</th></tr></thead><tbody><tr><td>Commands</td><td>Recorded draw + dispatch + trace-rays counts, not GPU time.</td></tr><tr><td>Frames</td><td>Distinct recorded frames; may be unavailable in aggregate-only captures.</td></tr><tr><td>Inherited Commands</td><td>Summed commands from pipelines referencing a shader. Zero means no recorded commands in this capture; Show Unused Shaders includes these entries.</td></tr><tr><td>Pipeline Variants</td><td>Distinct referencing pipelines, including unused ones.</td></tr><tr><td>Raw Statistics</td><td>Driver statistic entries. A dash means unavailable, not zero.</td></tr></tbody></table></div><p>Click a row to open details; Esc closes them. Shader rows stay highlighted. Hover over SHA for the full value. Tabs retain search, filters and sorting.</p></section></article>`}
function unusedButton(active,action,label='Show Unused Pipelines'){return `<button class="unused-toggle ${active?'active':''}" title="${esc(label)}" aria-label="${esc(label)}" aria-pressed="${active}" onclick="${action}"><svg viewBox="0 0 24 24" width="20" height="20" aria-hidden="true" fill="none" stroke="currentColor" stroke-width="1.8"><path d="M4 5h15M4 10h15" stroke-linecap="round"/><path d="M4 16h8" stroke-dasharray="2 2" stroke-linecap="round"/><path d="M18 14v8m-4-4h8" stroke-linecap="round"/></svg></button>`}
const availableTags=[...new Set(D.pipelines.flatMap(p=>p.pressures))].sort(),selectedTags=new Set();
function togglePipelineTag(tag){selectedTags.has(tag)?selectedTags.delete(tag):selectedTags.add(tag);render();$(`[data-filter-tag="${tag}"]`)?.focus({preventScroll:true})}window.togglePipelineTag=togglePipelineTag;
function clearPipelineTags(){selectedTags.clear();render();$('#toolbar .clear-tags')?.focus({preventScroll:true})}window.clearPipelineTags=clearPipelineTags;
function tagFilters(){return availableTags.length?`<span class="tag-filters" role="group" aria-label="Filter By Flags (Match Any)">${availableTags.map(tag=>`<button data-filter-tag="${tag}" class="filter-tag ${selectedTags.has(tag)?'active':''}" aria-pressed="${selectedTags.has(tag)}" title="Filter ${esc(tag)} (match any selected flag)" onclick="togglePipelineTag('${tag}')">${esc(tag)}</button>`).join('')}<button class="clear-tags" title="Clear Tags" aria-label="Clear Tags" aria-disabled="${!selectedTags.size}" onclick="clearPipelineTags()"><svg viewBox="0 0 24 24" width="17" height="17" aria-hidden="true" fill="none" stroke="currentColor" stroke-width="1.8" stroke-linecap="round" stroke-linejoin="round"><path d="M3 4h18l-7 8v5l-4 3V12Z"/><path d="m17 16 5 5m0-5-5 5"/></svg></button></span>`:''}
const shaderStages=[...new Set(['compute','vertex','fragment',...D.shaders.flatMap(s=>s.uses.map(u=>u.stage))])],activeShaderStages=new Set(shaderStages);
function toggleShaderStage(stage){activeShaderStages.has(stage)?activeShaderStages.delete(stage):activeShaderStages.add(stage);if(!activeShaderStages.size)shaderStages.forEach(s=>activeShaderStages.add(s));render();$(`[data-shader-stage="${stage}"]`)?.focus({preventScroll:true})}window.toggleShaderStage=toggleShaderStage;
const kinds=[...new Set(D.pipelines.map(p=>p.kind))].sort(),activeKinds=new Set(kinds);let activeTab='pipelines',priorityOnly=false,showUnusedPipelines=false,showUnusedShaders=false,pipelineQuery='',shaderQuery='';function setTab(tab){activeTab=tab;render()}function toggleKind(kind){activeKinds.has(kind)?activeKinds.delete(kind):activeKinds.add(kind);if(!activeKinds.size)kinds.forEach(k=>activeKinds.add(k));render()}function togglePriority(){priorityOnly=!priorityOnly;render()}function toggleUnusedPipelines(value){showUnusedPipelines=value;render();$('#toolbar .unused-toggle')?.focus({preventScroll:true})}function setPipelineQuery(value){pipelineQuery=value;render(false)}function toggleUnusedShaders(){showUnusedShaders=!showUnusedShaders;render();$('#toolbar .unused-toggle')?.focus({preventScroll:true})}window.toggleUnusedShaders=toggleUnusedShaders;function setShaderQuery(value){shaderQuery=value;render(false)}function pipelineToolbar(){return `<span class="muted">Type:</span><span class="kind-group">${kinds.map(k=>`<button class="kind ${activeKinds.has(k)?'active '+kindClass(k):''}" onclick="toggleKind('${k}')">${esc(kindTitle(k))}</button>`).join('')}</span><button title="Show pipelines with pressure flags" class="priority ${priorityOnly?'active':''}" onclick="togglePriority()">With Pressure Flags</button>${helpLink('priority','Pressure Flags')}${unusedButton(showUnusedPipelines,'toggleUnusedPipelines(!showUnusedPipelines)')}<input value="${esc(pipelineQuery)}" oninput="setPipelineQuery(this.value)" placeholder="Search pipeline SHA / pass label">${tagFilters()}`}function shaderToolbar(){return `<span class="muted">Type:</span><span class="kind-group" role="group" aria-label="Shader Types">${shaderStages.map(stage=>`<button data-shader-stage="${esc(stage)}" class="kind ${activeShaderStages.has(stage)?'active '+kindClass(stage):''}" aria-pressed="${activeShaderStages.has(stage)}" onclick="toggleShaderStage('${stage}')">${esc(kindTitle(stage))}</button>`).join('')}</span>${unusedButton(showUnusedShaders,'toggleUnusedShaders()','Show Unused Shaders')}<input value="${esc(shaderQuery)}" oninput="setShaderQuery(this.value)" placeholder="Search shader SHA / entry point / stage">`}function render(updateToolbar=true){const active=D.pipelines.filter(used);$('#report-title').textContent=`${D.platform} Shader Profiling Report`;$('#status').innerHTML=`<div class="status-group"><span class="status-scope">Capture Totals</span><span><b>${n(D.shaders.length)}</b> shaders</span><span><b>${n(D.pipelines.length)}</b> pipelines</span><span><b>${n(D.statistics_count)}</b> raw statistics</span></div><div class="status-group"><span class="status-scope">Used Pipelines</span><span><b>${n(active.length)}</b> total</span><span>${typeTag('graphics')} <b>${n(active.filter(p=>p.kind==='graphics').length)}</b></span><span>${typeTag('compute')} <b>${n(active.filter(p=>p.kind==='compute').length)}</b></span></div>`;$('#tabs').innerHTML=`<button class="tab ${activeTab==='pipelines'?'active':''}" onclick="setTab('pipelines')">Pipelines</button><button class="tab ${activeTab==='shaders'?'active':''}" onclick="setTab('shaders')">Shaders</button><button class="tab ${activeTab==='help'?'active':''}" onclick="setTab('help')">Help</button>`;$('#toolbar').classList.toggle('hidden',activeTab==='help');$('#results').classList.toggle('hidden',activeTab==='help');if(activeTab==='pipelines'){const q=pipelineQuery.trim().toLowerCase().replace(/^sha1:/,''),match=p=>!q||JSON.stringify(p).toLowerCase().includes(q);let rows=D.pipelines.filter(p=>activeKinds.has(p.kind)&&(showUnusedPipelines||p.command_count>0||p.active_frames>0)&&match(p));if(priorityOnly)rows=rows.filter(p=>p.pressures.length);if(selectedTags.size)rows=rows.filter(p=>p.pressures.some(tag=>selectedTags.has(tag)));if(updateToolbar)$('#toolbar').innerHTML=pipelineToolbar();$('#results').textContent=`Showing ${n(rows.length)} / ${n(D.pipelines.length)} pipelines`;$('#content').innerHTML=triage(rows,'Pipelines')}else if(activeTab==='help'){$('#content').innerHTML=helpPage()}else{const q=shaderQuery.trim().toLowerCase().replace(/^sha1:/,''),rows=D.shaders.filter(s=>s.uses.some(u=>activeShaderStages.has(u.stage))&&(showUnusedShaders||s.pipeline_command_count>0)&&(!q||JSON.stringify(s).toLowerCase().includes(q)));if(updateToolbar)$('#toolbar').innerHTML=shaderToolbar();$('#results').textContent=`Showing ${n(rows.length)} / ${n(D.shaders.length)} shaders`;$('#content').innerHTML=shaderTable(rows)}}window.setTab=setTab;window.toggleKind=toggleKind;window.togglePriority=togglePriority;window.toggleUnusedPipelines=toggleUnusedPipelines;window.setPipelineQuery=setPipelineQuery;window.setShaderQuery=setShaderQuery;render();
</script></body></html>'''
    document = document.replace('../shaders/', '${D.shader_base}/')
    path.write_text(document.replace('__DATA__', data), encoding='utf-8')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('dump_root', type=Path)
    parser.add_argument('--output', type=Path)
    parser.add_argument('--top', type=int, default=50, help='Deprecated; all report data is embedded.')
    args = parser.parse_args()
    root = args.dump_root
    output = args.output or root
    try:
        shader_base = quote(Path(os.path.relpath(root / 'shaders', output)).as_posix(), safe='/')
    except ValueError:
        # Windows cannot express relative links across drive letters.
        shader_base = (root / 'shaders').resolve().as_uri()
    output.mkdir(parents=True, exist_ok=True)
    pipelines, usage = read_pipelines(root), read_usage(root)
    mali = any(stat.get('name') == 'Spill size'
               for pipeline in pipelines.values() for executable in pipeline['executables']
               for stat in executable.get('statistics', []))
    platform = 'Mali' if mali else 'QCOM'
    metric_roles = (('scratch', 'register', 'register_used', 'tls', 'wls', 'arithmetic', 'texture', 'load_store', 'varying')
                    if mali else ('scratch', 'register', 'instructions', 'occupancy', 'texture', 'read', 'write', 'alu'))
    rows, links, stats, stats_csv = [], [], defaultdict(list), []
    for key in sorted(set(pipelines) | set(usage)):
        meta, count = pipelines.get(key, {}), usage[key]
        labels = sorted(count['labels'])
        commands = sum(count[x] for x in ('draw_count', 'dispatch_count', 'trace_rays_count'))
        row = {'pipeline_signature': key, 'kind': meta.get('kind', 'unknown'), 'command_count': commands,
               'draw_count': count['draw_count'], 'dispatch_count': count['dispatch_count'], 'trace_rays_count': count['trace_rays_count'],
               'active_frames': len(count['frames']), 'labels': labels, 'label_groups': sorted({label_group(x) for x in labels}),
               'shader_count': len(meta.get('shaders', [])), 'capture_status': meta.get('capture_status', 'unknown'),
               'statistics_available': bool(meta.get('executables')), 'metrics': {}}
        rows.append(row)
        for shader_id, stage, entry in sorted(meta.get('shaders', [])):
            links.append({'pipeline_signature': key, 'shader_sha1': shader_id, 'stage': stage, 'entry_point': entry,
                          'pipeline_command_count': commands, 'label_groups': '; '.join(row['label_groups'])})
        for exe in meta.get('executables', []):
            info = {'index': exe.get('index', 0), 'name': exe.get('name', ''), 'statistics': exe.get('statistics', [])}
            stats[key].append(info)
            for stat in info['statistics']:
                name, value, role = stat.get('name', ''), as_number(stat.get('value')), metric_role(stat.get('name', ''), mali)
                stats_csv.append({'pipeline_signature': key, 'executable_index': info['index'], 'executable_name': info['name'],
                                  'statistic_name': name, 'value': stat.get('value', ''), 'format': stat.get('format', ''),
                                  'description': stat.get('description', '')})
                if role and value is not None:
                    old = row['metrics'].get(role)
                    if old is None or value > old['value']:
                        row['metrics'][role] = {'name': name, 'value': value}
    rows.sort(key=lambda x: x['command_count'], reverse=True)
    values = {role: [x['metrics'][role]['value'] for x in rows if role in x['metrics']]
              for role in metric_roles}
    for row in rows:
        for role, item in row['metrics'].items():
            item['percentile'] = percentile(values[role], item['value'])
        pressure = []
        if row['metrics'].get('scratch', {}).get('value', 0) > 0: pressure.append('SPILL' if mali else 'SCRATCH')
        if row['metrics'].get('register', {}).get('percentile', 0) >= 90: pressure.append('REG-PRESSURE')
        if mali:
            if row['metrics'].get('tls', {}).get('value', 0) > 0 and row['metrics']['tls'].get('percentile', 0) >= 90: pressure.append('TLS')
            if row['metrics'].get('wls', {}).get('value', 0) > 0 and row['metrics']['wls'].get('percentile', 0) >= 90: pressure.append('WLS')
            if row['metrics'].get('arithmetic', {}).get('percentile', 0) >= 90: pressure.append('ALU-HEAVY')
            if row['metrics'].get('load_store', {}).get('percentile', 0) >= 90 and row['metrics']['load_store'].get('value', 0) > 0: pressure.append('MEMORY-HEAVY')
        else:
            if row['metrics'].get('occupancy', {}).get('percentile', 100) <= 25: pressure.append('LOW-OCCUPANCY')
            if row['metrics'].get('alu', {}).get('percentile', 0) >= 90: pressure.append('ALU-HEAVY')
        if row['metrics'].get('texture', {}).get('percentile', 0) >= 90: pressure.append('TEXTURE-HEAVY')
        if not mali:
            read = row['metrics'].get('read', {})
            write = row['metrics'].get('write', {})
            if ((read.get('value', 0) > 0 and read.get('percentile', 0) >= 90) or
                    (write.get('value', 0) > 0 and write.get('percentile', 0) >= 90)):
                pressure.append('MEMORY-HEAVY')
        if sum(tag.endswith('HEAVY') for tag in pressure) >= 2: pressure.append('MIXED')
        row['pressures'] = pressure
    shader_use = defaultdict(lambda: {'commands': 0, 'pipelines': set(), 'uses': set(), 'labels': set()})
    pipeline_by_signature = {row['pipeline_signature']: row for row in rows}
    for link in links:
        item = shader_use[link['shader_sha1']]
        signature = link['pipeline_signature']
        if signature not in item['pipelines']:
            item['commands'] += link['pipeline_command_count']
        item['pipelines'].add(signature)
        item['uses'].add((link['stage'], link['entry_point']))
        item['labels'].update(pipeline_by_signature[signature]['label_groups'])
    shaders = [{'shader_sha1': key, 'pipeline_command_count': x['commands'], 'pipeline_count': len(x['pipelines']), 'label_groups': sorted(x['labels']),
                'uses': [{'stage': stage, 'entry_point': entry} for stage, entry in sorted(x['uses'])]}
               for key, x in shader_use.items()]
    shaders.sort(key=lambda x: x['pipeline_command_count'], reverse=True)
    pipeline_csv = [dict(x, label_groups='; '.join(x['label_groups'])) for x in rows]
    shader_csv = [dict(x, label_groups='; '.join(x['label_groups']), uses='; '.join(f"{u['stage']}:{u['entry_point']}" for u in x['uses'])) for x in shaders]
    write_csv(output / 'pipeline_summary.csv', ['pipeline_signature','kind','command_count','draw_count','dispatch_count','trace_rays_count','active_frames','label_groups','shader_count','capture_status'], pipeline_csv)
    write_csv(output / 'shader_summary.csv', ['shader_sha1','pipeline_command_count','pipeline_count','label_groups','uses'], shader_csv)
    for obsolete in ('hot_pipelines.csv', 'hot_shaders.csv'):
        try:
            (output / obsolete).unlink(missing_ok=True)
        except OSError as error:
            print(f'WARNING: Obsolete report CSV retained: {output / obsolete}: {error}')
    write_csv(output / 'pipeline_shader_links.csv', ['pipeline_signature','shader_sha1','stage','entry_point','pipeline_command_count','label_groups'], links)
    write_csv(output / 'pipeline_statistics.csv', ['pipeline_signature','executable_index','executable_name','statistic_name','value','format','description'], stats_csv)
    summary_metrics = ([
        {'key': 'scratch', 'label': 'Spill (B)'}, {'key': 'register', 'label': 'Regs alloc'},
        {'key': 'tls', 'label': 'TLS (B)'}, {'key': 'wls', 'label': 'WLS (B)'},
        {'key': 'arithmetic', 'label': 'Arithmetic'}, {'key': 'texture', 'label': 'Texture'},
        {'key': 'load_store', 'label': 'Load/store'},
    ] if mali else [
        {'key': 'scratch', 'label': 'Scratch'}, {'key': 'register', 'label': 'Registers'},
        {'key': 'occupancy', 'label': 'Occupancy'}, {'key': 'instructions', 'label': 'Instructions'},
    ])
    detail_order = (['Spill size', 'Registers allocated', 'Registers used', 'TLS size', 'WLS size', 'Arithmetic',
                     'Arithmetic FMA (Valhall only)', 'Arithmetic CVT (Valhall only)', 'Arithmetic SFU (Valhall only)',
                     'Texture', 'Load Store', 'Varying'] if mali else
                    ['Scratch memory usage per shader instance', 'Overall register footprint per shader instance',
                     'ALU fiber occupancy percentage', 'Instruction count all', 'ALU instruction count 32bit',
                     'ALU instruction count 16bit', 'Complex instruction count', 'Texture read instruction count',
                     'Memory read instruction count', 'Memory write instruction count'])
    write_html(output / 'shader_usage_report.html', {'pipelines': rows, 'shaders': shaders, 'links': links, 'statistics': stats,
                                       'shader_base': shader_base,
                                       'statistics_count': len(stats_csv), 'graphics': sum(x['kind'] == 'graphics' for x in rows),
                                       'compute': sum(x['kind'] == 'compute' for x in rows), 'platform': platform,
                                       'summary_metrics': summary_metrics, 'detail_order': detail_order})
    print(f'Wrote report to: {output}')


if __name__ == '__main__':
    main()

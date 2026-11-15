// Usage: node test_shader_viewer.js <generated shader_usage_report.html>
// Logic checks with a minimal DOM double; this does not verify browser layout.
const fs = require('fs');
const vm = require('vm');
const assert = require('assert');
const html = fs.readFileSync(process.argv[2], 'utf8');
assert(!html.includes('id="source-copy"'), 'remove redundant copy-code control');
assert(html.includes('class="tabs shader-detail-tabs"'), 'reuse main tab styling');
assert(html.includes('id="shader-tab-code" class="tab"'), 'reuse main tab button styling');
const data = JSON.parse(html.match(/type="application\/json">(.*?)<\/script>/s)[1]);
const nodes = new Map();
class Element {
    constructor() { this.textContent = ''; this.attributes = {}; this.hidden = false; this.classList = { toggle: (_, value) => { this.hidden = value; } }; }
    setAttribute(key, value) { this.attributes[key] = value; }
}
const get = selector => {
    if (!nodes.has(selector)) nodes.set(selector, new Element());
    return nodes.get(selector);
};
get('#report-data').textContent = JSON.stringify(data);
const context = vm.createContext({
    assert, console,
    document: { querySelector: get, addEventListener() {} },
    window: { addEventListener() {} },
});
const script = html.match(/<script>(.*?)<\/script>/s)[1].replace(/render\(\);\s*$/, '');
vm.runInContext(script, context);
vm.runInContext(`renderSourceView=()=>{};`, context);
vm.runInContext(`{
 const rows=statRows([
  {key:'instance1:0',index:0,statistics:[{name:'Registers',value:5}]},
  {key:'instance2:0',index:0,statistics:[{name:'Registers',value:28}]}
 ]);
 assert.equal(rows[0].values['instance1:0'],5);
 assert.equal(rows[0].values['instance2:0'],28);
 assert.equal(Object.keys(rows[0].values).length,2);
}`,context);
vm.runInContext(`(async()=>{
    const ids=D.shaders.slice(0,2).map(s=>s.shader_sha1);
    assert.equal(ids.length,2,'use a report containing at least two shaders');
    const file=(id,format,text,modified=1)=>({name:id+'.'+format,size:text.length,lastModified:modified,text:async()=>text});
    const files=[file(ids[0],'glsl','float a;'),file(ids[1],'hlsl','float b;'),file('0'.repeat(40),'glsl','unknown')];
    currentShader=ids[0];
    await loadSourceFolderFiles({files,value:'selected'});
    assert.equal(sourceFileIndex.size,2,'index only report shaders');
    assert.equal(shaderSources.get(ids[0]).size,1);
    assert(!shaderSources.has(ids[1]),'read code only for selected shader');
    currentShader=ids[1];await loadIndexedSources();
    assert.equal(shaderSources.get(ids[1]).get(ids[1]+'.hlsl').text,'float b;');
    await importSourceFiles([file(ids[0],'glsl','wrong')]);
    assert(getStatus().includes('skipped'),'reject mismatched manual imports');
    const directoryFiles=[...files.slice(0,2)];
    sourceDirectory={async *values(){for(const f of directoryFiles)yield {kind:'file',name:f.name,getFile:async()=>f}}};
    directoryFiles.push(file(ids[1],'spvasm','; newly converted'));
    await refreshSourceFolder();
    assert.equal(shaderSources.get(ids[1]).size,2,'discover conversion after folder selection');
    directoryFiles[1]=file(ids[1],'hlsl','float changed;',2);
    await refreshSourceFolder();
    assert.equal(shaderSources.get(ids[1]).get(ids[1]+'.hlsl').text,'float changed;');
    setShaderDetailTab('pipelines');
    assert.equal(document.querySelector('#shader-tab-pipelines').attributes['aria-selected'],'true');
    assert.equal(document.querySelector('#shader-panel-code').hidden,true);
    setShaderDetailTab('code');
    assert.equal(document.querySelector('#shader-panel-pipelines').hidden,true);
    const code='/* comment\\ncontinued */\\nfloat4 main() { return 1.0; }\\n"<script>"';
    const colored=highlightSource(code,'hlsl');
    assert(colored.includes('tok-comment')&&colored.includes('tok-type')&&colored.includes('tok-keyword'));
    assert(!colored.includes('<script>'),'escape HTML before inserting highlighting');
    assert.equal(colored.split('\\n').length,code.split('\\n').length,'preserve line numbers');
    assert(highlightSource('%1 = OpTypeFloat 32 ; note','spvasm').includes('tok-instruction'));
    console.log('PASS folder matching, lazy reading, refresh, SHA checks, detail tabs and safe highlighting');
})()`, Object.assign(context, { getStatus: () => get('#source-status').textContent })).catch(error => { console.error(error); process.exitCode = 1; });

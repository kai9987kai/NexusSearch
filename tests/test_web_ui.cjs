const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const header = fs.readFileSync(path.join(__dirname, '../src/server/nx_web_ui.h'), 'utf8');
const html = Buffer.from([...header.matchAll(/0x([0-9a-f]{2})/g)].map(m => parseInt(m[1], 16))).toString('utf8');
const script = html.match(/<script>([\s\S]*?)<\/script>/)[1];
const elements = new Map();
const element = () => ({style:{}, textContent:'', innerHTML:'', addEventListener(){}, setAttribute(){}, replaceChildren(...children){this.children = children;}});
const doc = {
  getElementById(id) { if (!elements.has(id)) elements.set(id,element()); return elements.get(id); },
  querySelectorAll(){return [];}, createElement:element,
};
const context = vm.createContext({document:doc, console:{error(){}}, fetch:async()=>({ok:false}), performance:{now:()=>0}});
vm.runInContext(script, context);
const hostile = '<img src=x onerror="alert(1)">';
context.payload = {total:1,count:1,execution:{work:99,numeric_indexes:2,scanned_cells:8,vectors_scored:3},hits:[{_id:hostile,row:0,score:0.5,document:{title:hostile,body:'Visible body',custom:hostile}}]};
vm.runInContext('renderResults(payload, "1.2")', context);
assert.equal(doc.getElementById('metricWork').textContent,'99');
assert.equal(doc.getElementById('metricVectors').textContent,'3');
assert.equal(doc.getElementById('metricIndexed').textContent,'2');
const rendered=doc.getElementById('resultsList').innerHTML;
assert(rendered.includes('Visible body'));
assert(rendered.includes('&lt;img src=x onerror=&quot;alert(1)&quot;&gt;'));
assert(!rendered.includes('<img'));
context.message=hostile;
vm.runInContext('renderError(message)',context);
assert.equal(doc.getElementById('resultsList').children[0].textContent,'Search error: '+hostile);
assert.equal(doc.getElementById('metricsBar').style.display,'none');
console.log('Browser renderer regression checks passed: nested statistics, object documents, escaped fields and text-only errors.');

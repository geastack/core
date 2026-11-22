import fs from 'node:fs'
import vm from 'node:vm'
import path from 'node:path'
import {createRequire} from 'node:module'
import assert from 'node:assert/strict'
import ts from 'typescript'
if(!process.env.GEA_JSDOM_PACKAGE)throw Error('GEA_JSDOM_PACKAGE must name the actual installed package')
const require=createRequire(path.join(path.resolve(process.env.GEA_JSDOM_PACKAGE),'package.json'))
const {JSDOM}=require('./lib/api.js')
const source=fs.readFileSync(new URL('./native-remove-child-contract.ts',import.meta.url),'utf8')
const js=ts.transpileModule(source,{compilerOptions:{module:ts.ModuleKind.ESNext,target:ts.ScriptTarget.ES2022}}).outputText.replace(/^export \{\};?\s*$/m,'')
const dom=new JSDOM('<div id="app"></div>'),output=[]
try {
 vm.runInNewContext(js,{document:dom.window.document,console:{log:value=>output.push(String(value))}})
 assert.deepEqual(output,['true','true','true','true','true','true'])
 assert.equal(dom.window.document.querySelector('#remove-child-child').parentNode.id,'remove-child-parent')
 console.log(output.join('\n'))
}finally{dom.window.close()}

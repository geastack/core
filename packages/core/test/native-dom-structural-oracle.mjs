import fs from 'node:fs'
import vm from 'node:vm'
import path from 'node:path'
import {createRequire} from 'node:module'
import assert from 'node:assert/strict'
import ts from 'typescript'
if(!process.env.GEA_JSDOM_PACKAGE)throw Error('GEA_JSDOM_PACKAGE must name the actual installed package')
const require=createRequire(path.join(path.resolve(process.env.GEA_JSDOM_PACKAGE),'package.json'))
const {JSDOM}=require('./lib/api.js')
const source=fs.readFileSync(new URL('./native-dom-structural-contract.ts',import.meta.url),'utf8')
const js=ts.transpileModule(source,{compilerOptions:{module:ts.ModuleKind.ESNext,target:ts.ScriptTarget.ES2022}}).outputText.replace(/^export \{\};?\s*$/m,'')
const dom=new JSDOM('<div id="app"></div>'),output=[]
try{
 vm.runInNewContext(js,{document:dom.window.document,console:{log:value=>output.push(String(value))}})
 assert.deepEqual(output,['true','true','true','8','11','8','1','true','true','0'])
 console.log(output.join('\n'))
}finally{dom.window.close()}

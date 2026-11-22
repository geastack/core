import fs from 'node:fs'
import vm from 'node:vm'
import assert from 'node:assert/strict'
import ts from 'typescript'
import { createRequire } from 'node:module'
import path from 'node:path'
if (!process.env.GEA_JSDOM_PACKAGE) throw new Error('GEA_JSDOM_PACKAGE must name the actual installed package')
const require = createRequire(path.join(path.resolve(process.env.GEA_JSDOM_PACKAGE), 'package.json'))
const { JSDOM } = require('./lib/api.js')
const source=fs.readFileSync(new URL('./native-input-contract.ts',import.meta.url),'utf8')
const js=ts.transpileModule(source,{compilerOptions:{module:ts.ModuleKind.ESNext,target:ts.ScriptTarget.ES2022}}).outputText.replace(/^export \{\};?\s*$/m,'')
const dom=new JSDOM('<div id="app"></div>'),output=[]
vm.runInNewContext(js,{document:dom.window.document,console:{log:value=>output.push(String(value))}})
assert.deepEqual(output,['seed','edited text','false','true','true','false',''])
console.log(output.join('\n'))
dom.window.close()

import {test} from 'node:test'
import assert from 'node:assert/strict'
import fs from 'node:fs'
import {fileURLToPath} from 'node:url'
import ts from 'typescript'
const config=ts.readConfigFile(new URL('./tsconfig-native-insert-before.json',import.meta.url).pathname,ts.sys.readFile)
const parsed=ts.parseJsonConfigFileContent(config.config,ts.sys,fileURLToPath(new URL('./',import.meta.url)))
const declaration=fileURLToPath(new URL('../index.d.ts',import.meta.url))
function diagnostics(old){
 const host=ts.createCompilerHost(parsed.options),read=host.readFile.bind(host)
 if(old)host.readFile=file=>file===declaration?old:read(file)
 return ts.getPreEmitDiagnostics(ts.createProgram(parsed.fileNames,parsed.options,host))
}
test('the same-source contract refuses a declaration with insertBefore absent',()=>{
 const current=fs.readFileSync(declaration,'utf8')
 const old=current.replace('  insertBefore(child: Node, reference: Node | null): Node\n','')
 assert.notEqual(old,current)
 const errors=diagnostics(old)
 assert.ok(errors.some(error=>error.code===2339&&ts.flattenDiagnosticMessageText(error.messageText,' ').includes('insertBefore')))
})
test('actual same-source contract accepts native Node return and null references without lib.dom',()=>{
 const errors=diagnostics()
 assert.deepEqual(errors.map(error=>ts.flattenDiagnosticMessageText(error.messageText,' ')),[])
})

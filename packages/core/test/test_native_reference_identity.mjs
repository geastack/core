import assert from 'node:assert/strict'
import {execFileSync} from 'node:child_process'
import fs from 'node:fs'
import path from 'node:path'
import {fileURLToPath} from 'node:url'
const root=path.resolve(path.dirname(fileURLToPath(import.meta.url)),'../../..')
const output=path.join(root,'dist/identity-probe')
fs.mkdirSync(output,{recursive:true})
// clang names a Windows executable with .exe whatever -o says, and spawn needs the real name.
const carriers=path.join(output,process.platform==='win32'?'carriers.exe':'carriers')
execFileSync(process.env.CXX||'clang++',['-std=c++20','-I',path.join(root,'packages/core/include'),'-I',path.join(root,'packages/engine/ui'),'-I',path.join(root,'packages/engine'),path.join(root,'packages/core/test/test_native_reference_identity.cpp'),'-o',carriers],{stdio:'inherit'})
execFileSync(carriers,[],{stdio:'inherit'})
// These are the only Tree allocation retirement seams. Carrier tests above
// independently prove that retiring each seam prevents numeric-id ABA.
const lifecycle=fs.readFileSync(path.join(root,'packages/engine/ui/tree_nodes.cpp'),'utf8')
assert.match(lifecycle,/void resetNodeSlot\([^]*?identity::retire\(id\)/)
assert.match(lifecycle,/void Tree::clear\(\)\s*\{\s*gea::framework::identity::clear\(\)/)
assert.match(lifecycle,/int Tree::createNode\([^]*?identity::activate\(id\);\s*const auto allocatedOwner = gea::framework::identity::node\(id\);\s*state.nodeActive\[id\] = 1/)
console.log('native carrier identity and lifecycle seam tests passed; no GUI/runtime launch')

// jsdom is a DOM reference only; the compared executable uses the real native Tree.
import {createRequire} from 'node:module';
import {spawnSync} from 'node:child_process';
import assert from 'node:assert/strict';
import path from 'node:path';
assert.ok(process.env.GEA_JSDOM_PACKAGE,'GEA_JSDOM_PACKAGE must name the actual installed package');
const require=createRequire(path.join(path.resolve(process.env.GEA_JSDOM_PACKAGE),'package.json'));
const {JSDOM}=require('./lib/api.js');
const binary=process.argv[2];assert.ok(binary,'explicit native binary required');
const document=new JSDOM(`<main id="app"><fieldset id="group" disabled><legend id="legend"><input id="exempt"></legend><input id="range" class="control selected" data-act="range-gain" data-tags="one two" data-lang="en-US" data-comma="a,b" type="range"></fieldset><div id="generic" disabled></div></main>`).window.document;
const queries=['input[type=RANGE]', 'input','fieldset > input','main input.control.selected','[data-act]','[data-act=range-gain]','[data-act^=range]','[data-act$=gain]','[data-act*=ge-g]','[data-tags~=two]','[data-lang|=en]',`[data-comma='a,b']`,':disabled',':enabled','input:not([type=text], [data-missing])','input:not([type=range])','[data-act="RANGE-GAIN" i]','fieldset, input, #range','#generic[disabled]','input:not(fieldset > input)',':not(:not(input))'];
// Windows' text-mode stdout ends each line with CRLF.
for(const query of queries){const actual=spawnSync(binary,['--query',query],{encoding:'utf8'});assert.equal(actual.status,0,actual.stderr);assert.deepEqual(actual.stdout.trim().split(/\r?\n/).filter(Boolean),[...document.querySelectorAll(query)].map(n=>n.id),query);}
// jsdom's selector engine rejects the Selectors 4 `s` flag; it forces a
// case-sensitive compare, so differently cased values match nothing.
for(const query of ['input[type=RANGE s]','[data-act="RANGE-GAIN" s]']){const actual=spawnSync(binary,['--query',query],{encoding:'utf8'});assert.equal(actual.status,0,actual.stderr);assert.deepEqual(actual.stdout.trim().split(/\r?\n/).filter(Boolean),[],query);}
console.log(`native/DOM differential ${queries.length} selectors passed, plus 2 case-sensitive flags`);

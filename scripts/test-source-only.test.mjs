import {test} from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import {fileURLToPath} from 'node:url';
const root=path.resolve(path.dirname(fileURLToPath(import.meta.url)),'..');
test('ordinary pushes and pull requests keep source checks and never compile components',()=>{
  const files=fs.readdirSync(path.join(root,'.github/workflows')).filter(x=>x.endsWith('.yml'));
  const text=files.map(x=>fs.readFileSync(path.join(root,'.github/workflows',x),'utf8')).join('\n');
  assert.match(text,/source:\n    runs-on:/);
  assert.ok(text.includes("node --test scripts/test-policy.test.mjs scripts/test-store-context.test.mjs scripts/test-source-only.test.mjs"));
  assert.match(text,/component:\n    if: github\.event_name == 'workflow_dispatch' && github\.ref == 'refs\/heads\/main'/);
  assert.match(text,/workflow_dispatch:/);
  assert.ok(!text.includes('[source-only]'));
  const workflow=fs.readFileSync(path.join(root,'.github/workflows/test.yml'),'utf8');
  const source=workflow.slice(workflow.indexOf('  source:'),workflow.indexOf('  component:'));
  assert.ok(!/^\s+if:/m.test(source),'source job must stay unconditional');
  const evaluate=(event,ref)=>event==='workflow_dispatch'&&ref==='refs/heads/main';
  for(const event of ['push','pull_request'])for(const ref of ['refs/heads/main','refs/heads/develop'])
    assert.equal(evaluate(event,ref),false);
  assert.equal(evaluate('workflow_dispatch','refs/heads/main'),true);
  for(const ref of ['refs/heads/develop','refs/heads/feature/example'])
    assert.equal(evaluate('workflow_dispatch',ref),false);
});
test('shared waiter regression remains mandatory and its shim is fixture-only',()=>{
  const cmake=fs.readFileSync(path.join(root,'tests/CMakeLists.txt'),'utf8');
  const runner=fs.readFileSync(path.join(root,'scripts/test-component.mjs'),'utf8');
  assert.match(cmake,/add_executable\(nexus_taskqueue-composite-waiters\s+Nexus\/taskqueue-composite-waiters\.cpp/);
  assert.match(cmake,/target_compile_options\(nexus_taskqueue-composite-waiters PRIVATE\s+-include/);
  assert.ok(runner.includes("['nexus_taskqueue-composite-waiters', 96]"));
  assert.ok(!fs.readFileSync(path.join(root,'src/CMakeLists.txt'),'utf8').includes('taskqueue-composite-waiters-shim'));
});

test('actual queue unwind regression remains a required native component',()=>{
  const cmake=fs.readFileSync(path.join(root,'tests/CMakeLists.txt'),'utf8');
  const runner=fs.readFileSync(path.join(root,'scripts/test-component.mjs'),'utf8');
  assert.match(cmake,/add_executable\(nexus_taskqueue-unwind-wait\s+Nexus\/taskqueue-unwind-wait\.cpp/);
  assert.ok(runner.includes("['nexus_taskqueue-unwind-wait', 17]"));
});

// Optional browser integration test. The application itself has no Node runtime dependency.
// npm install --no-save --package-lock=false --prefix .cache/browser playwright-core@1.58.2 @sparticuz/chromium@143.0.4
// node tests/browser_smoke.mjs http://127.0.0.1:3000
import assert from 'node:assert/strict';
import {createRequire} from 'node:module';
import path from 'node:path';
import fs from 'node:fs/promises';
const modules=path.resolve(process.env.EDGECOURT_BROWSER_MODULES || '.cache/browser/node_modules');
const require=createRequire(path.join(modules,'package.json'));
const {chromium:playwright}=require('playwright-core');
let executablePath=process.env.EDGECOURT_CHROMIUM,args=['--no-sandbox'];
if(!executablePath){
  const pkg=require('@sparticuz/chromium');const chromium=pkg.default || pkg;
  const chromiumDir=path.join(modules,'@sparticuz/chromium');
  const {inflate}=require(path.join(chromiumDir,'build/cjs/lambdafs.cjs'));
  const libs=await inflate(path.join(chromiumDir,'bin/al2023.tar.br'));
  process.env.LD_LIBRARY_PATH=path.join(libs,'lib');process.env.FONTCONFIG_PATH='/tmp/fonts';
  executablePath=await chromium.executablePath();
  args=chromium.args.filter(a=>a!=='--disable-web-security');
}
const base=process.argv[2] || 'http://127.0.0.1:3000';
const browser=await playwright.launch({executablePath,args,headless:true});
const context=await browser.newContext({viewport:{width:1440,height:1000},acceptDownloads:true});
const page=await context.newPage();const errors=[];page.on('pageerror',e=>errors.push(e.message));

try {
  await page.goto(base,{waitUntil:'networkidle'});
  await page.locator('#loading-overlay').waitFor({state:'hidden'});
  await page.locator('.primary-nav [data-view="activity"]').click();
  assert.ok(await page.locator('#activity-view').isVisible());
  assert.match(await page.locator('#activity-view').innerText(),/SYNTHETIC DEMO/);
  assert.equal(await page.locator('#whale-table tbody tr').count(),4);
  await page.locator('#activity-minimum').selectOption('25000');
  assert.equal(await page.locator('#whale-table tbody tr').count(),1);
  await page.locator('#activity-side').selectOption('SELL');
  assert.match(await page.locator('#whale-table').innerText(),/No observed fills/);
  await page.locator('#activity-reset').click();
  await page.locator('#whale-table [data-wallet]').first().click();
  assert.equal(await page.locator('#whale-table tbody tr').count(),2);
  await page.locator('#activity-reset').click();
  await page.locator('#activity-provider').selectOption('kalshi');
  assert.match(await page.locator('#activity-results').innerText(),/no public wallet identities/);
  assert.match(await page.locator('#whale-table').innerText(),/No observed fills/);
  await page.locator('#activity-reset').click();
  await page.locator('#activity-game').selectOption('demo-2');
  assert.match(await page.locator('#activity-results').innerText(),/No matched market volume/);
  await page.locator('#activity-reset').click();
  const downloadPromise=page.waitForEvent('download');
  await page.locator('#activity-export').click();
  const download=await downloadPromise;
  await fs.mkdir('.cache/browser-results',{recursive:true});
  await download.saveAs('.cache/browser-results/activity.json');
  const exported=JSON.parse(await fs.readFile('.cache/browser-results/activity.json','utf8'));
  assert.equal(exported.mode,'demo');assert.equal(exported.trades.length,4);
  await page.screenshot({path:'.cache/browser-results/activity-desktop.png',fullPage:true});
  await page.locator('#theme-toggle').click();
  await page.screenshot({path:'.cache/browser-results/activity-dark.png',fullPage:true});
  await page.setViewportSize({width:390,height:844});
  assert.equal(await page.evaluate(()=>document.documentElement.scrollWidth),390);
  assert.ok(await page.locator('#activity-minimum').isVisible());
  await page.screenshot({path:'.cache/browser-results/activity-mobile.png',fullPage:true});
  // This test requires an --offline server so mode switches never contact providers.
  const data=await page.evaluate(async()=>await(await fetch('/api/state')).json());
  assert.equal(data.publicFeedsEnabled,false,'Start browser test server with --offline');
  await page.evaluate(async token=>{await fetch('/api/mode',{method:'POST',headers:{'Content-Type':'application/json','X-Edgecourt-Token':token},body:JSON.stringify({mode:'live'})});},data.authToken);
  await page.reload({waitUntil:'networkidle'});
  await page.locator('.primary-nav [data-view="activity"]').click();
  assert.match(await page.locator('#activity-results').innerText(),/No matched market volume/);
  assert.equal(await page.locator('#activity-view .synthetic').count(),0);
  await page.evaluate(async token=>{await fetch('/api/mode',{method:'POST',headers:{'Content-Type':'application/json','X-Edgecourt-Token':token},body:JSON.stringify({mode:'demo'})});},data.authToken);
  await page.reload({waitUntil:'networkidle'});
  await page.locator('.primary-nav [data-view="activity"]').click();
  assert.equal(await page.locator('#whale-table tbody tr').count(),4);
  assert.deepEqual(errors,[]);
  console.log('PASS activity browser: volume units, threshold/side/exchange/game/wallet filters, export, light/dark/mobile layout, empty states and demo/live isolation');
} finally {await browser.close();}

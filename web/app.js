'use strict';

const $ = (selector, root = document) => root.querySelector(selector);
const $$ = (selector, root = document) => [...root.querySelectorAll(selector)];
const esc = value => String(value ?? '').replace(/[&<>"']/g, c => ({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]));
const icons = {
  basketball:'<circle cx="12" cy="12" r="9"/><path d="M5.6 5.6l12.8 12.8M5.6 18.4L18.4 5.6M3 12c5-1 10 4 9 9M12 3c-1 5 4 10 9 9"/>',
  grid:'<rect x="3" y="3" width="7" height="7" rx="1.5"/><rect x="14" y="3" width="7" height="7" rx="1.5"/><rect x="3" y="14" width="7" height="7" rx="1.5"/><rect x="14" y="14" width="7" height="7" rx="1.5"/>',
  trend:'<path d="M3 17l6-6 4 4 8-10M15 5h6v6"/>',
  user:'<circle cx="12" cy="7" r="3"/><path d="M5 21v-3a7 7 0 0114 0v3"/>',
  layers:'<path d="M12 3L2 8l10 5 10-5-10-5zM2 13l10 5 10-5M2 18l10 5 10-5"/>',
  star:'<path d="M12 3l2.8 5.7 6.3.9-4.6 4.5 1.1 6.3-5.6-3-5.6 3 1.1-6.3L2.9 9.6l6.3-.9z"/>',
  bookmark:'<path d="M6 3h12v18l-6-4-6 4z"/>',
  flag:'<path d="M4 22V3M4 3c4-4 8 4 15 0v10c-7 4-11-4-15 0"/>',
  target:'<circle cx="12" cy="12" r="9"/><circle cx="12" cy="12" r="5"/><circle cx="12" cy="12" r="1"/>',
  settings:'<path d="M9 3h6l1 3 3 1 2 5-2 5-3 1-1 3H9l-1-3-3-1-2-5 2-5 3-1z"/><circle cx="12" cy="12" r="3"/>',
  shield:'<path d="M12 3l8 3v6c0 5-8 9-8 9s-8-4-8-9V6zM8 12l3 3 5-6"/>',
  flask:'<path d="M9 3h6M10 3v6L4 19c-1 2 0 2 2 2h12c2 0 3 0 2-2L14 9V3M7 15h10"/>',
  arrowRight:'<path d="M4 12h16M14 6l6 6-6 6"/>',
  arrowUpRight:'<path d="M6 18L18 6M6 6h12v12"/>',
  refresh:'<path d="M20 7a9 9 0 00-15-1L2 9m0-6v6h6M4 17a9 9 0 0015 1l3-3m0 6v-6h-6"/>',
  search:'<circle cx="10.5" cy="10.5" r="6.5"/><path d="M16 16l5 5"/>',
  sparkle:'<path d="M12 3l2.5 6.5L21 12l-6.5 2.5L12 21l-2.5-6.5L3 12l6.5-2.5z"/>',
  chevronDown:'<path d="M6 9l6 6 6-6"/>',
  chevronLeft:'<path d="M15 5l-7 7 7 7"/>',
  chevronRight:'<path d="M9 5l7 7-7 7"/>',
  sliders:'<path d="M4 4v4m0 4v8M12 4v10m0 4v2M20 4v1m0 4v11M1 8h6M9 14h6M17 5h6"/>',
  download:'<path d="M12 3v12m-5-5l5 5 5-5M4 16v5h16v-5"/>',
  upload:'<path d="M12 16V3m-5 5l5-5 5 5M4 16v5h16v-5"/>',
  plus:'<path d="M12 4v16M4 12h16"/>',
  x:'<path d="M6 6l12 12M18 6L6 18"/>',
  info:'<circle cx="12" cy="12" r="9"/><path d="M12 11v6M12 7v.1"/>',
  clock:'<circle cx="12" cy="12" r="9"/><path d="M12 6v6l4 2"/>',
  database:'<ellipse cx="12" cy="5" rx="8" ry="3"/><path d="M4 5v14c0 4 16 4 16 0V5M4 12c0 4 16 4 16 0"/>',
  activity:'<path d="M2 12h5l3-9 4 18 3-9h5"/>',
  code:'<path d="M8 5l-6 7 6 7M16 5l6 7-6 7M14 3l-4 18"/>',
  warning:'<path d="M12 3L2 21h20zM12 9v5m0 3v.1"/>',
  moon:'<path d="M20 13.5A8.5 8.5 0 1110.5 4a7 7 0 009.5 9.5z"/>',
  sun:'<circle cx="12" cy="12" r="4"/><path d="M12 2v2m0 16v2M4.9 4.9l1.4 1.4m11.4 11.4 1.4 1.4M2 12h2m16 0h2M4.9 19.1l1.4-1.4M17.7 6.3l1.4-1.4"/>'
};
const icon = name => `<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.6" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true">${icons[name] || icons.grid}</svg>`;
function hydrateIcons(root = document) { $$('[data-icon]', root).forEach(el => { el.innerHTML = icon(el.dataset.icon); }); }
const categories = [
  {id:'all',label:'All markets',icon:'grid'},
  {id:'moneyline',label:'Moneyline',icon:'trend'},
  {id:'props',label:'Player props',icon:'user'},
  {id:'points',label:'Points (O/U)',sub:true},
  {id:'rebounds',label:'Rebounds (O/U)',sub:true},
  {id:'assists',label:'Assists (O/U)',sub:true},
  {id:'threes',label:'3-pointers made',sub:true},
  {id:'pra',label:'PRA combo',icon:'layers'},
  {id:'double_double',label:'Double-double',icon:'target'},
  {id:'triple_double',label:'Triple-double',icon:'sparkle'},
  {id:'first_basket',label:'First basket',icon:'flag'},
  {id:'team_total',label:'Team totals (O/U)',icon:'basketball'}
];
const marketLabels = {moneyline:'Moneyline',points:'Points (O/U)',rebounds:'Rebounds (O/U)',assists:'Assists (O/U)',threes:'3-pointers made',pra:'PRA combo',double_double:'Double-double',triple_double:'Triple-double',first_basket:'First basket',team_total:'Team total (O/U)'};
const shortLabels = {moneyline:'Moneyline',points:'Points',rebounds:'Rebounds',assists:'Assists',threes:'3PM',pra:'PRA',double_double:'Double-double',triple_double:'Triple-double',first_basket:'First basket',team_total:'Team total'};
const teamColors = {BOS:['#38836a','#edf7f1','#dbece3'],NYK:['#cf8746','#fcf4eb','#f0e4d8'],GSW:['#5f86ba','#f0f5fc','#e0e9f5'],LAL:['#9b83bc','#f5f0fa','#e9dff4'],DEN:['#5b8ca6','#eef6fa','#dcebf1'],MIN:['#6b958c','#eef7f3','#dcebe6'],DAL:['#658faf','#f0f5fc','#e0eaf5'],PHX:['#a684ba','#f7f0fb','#eee1f5'],OKC:['#cd9062','#fdf5ed','#f1e6da'],MIL:['#649886','#eef6f2','#dcebe3'],MIA:['#be8290','#fcf1f4','#f0dfe5']};
const teamStyle = team => { const c = teamColors[team] || ['#6a83a2','#f0f4fa','#e1e8f2']; return `--team-color:${c[0]};--team-bg:${c[1]};--team-border:${c[2]}`; };
const initials = name => String(name).split(/[\s-]+/).filter(Boolean).map(x => x[0]).slice(0,2).join('').toUpperCase();
const num = (n, digits=1) => n == null || !Number.isFinite(Number(n)) ? '—' : Number(n).toFixed(digits);
const pct = n => n == null ? '—' : `${num(n*100)}%`;
const signed = (n,digits=1) => n == null ? '—' : `${n>0?'+':''}${num(n,digits)}`;
// All bookmaker prices are percentages, not model forecasts or de-vigged probabilities.
const odds = n => n == null || !Number.isFinite(Number(n)) ? '—' : `${num(n,2)}%`;
const money = n => new Intl.NumberFormat('en-US',{style:'currency',currency:'USD',maximumFractionDigits:2}).format(Number(n)||0);
const isBinary = market => ['double_double','triple_double','first_basket'].includes(market);
const isPlayer = market => !['moneyline','team_total'].includes(market);
const nowISO = () => new Date().toISOString().replace(/\.\d{3}Z$/,'Z');
const readPreference = (key, fallback) => { try { const value=localStorage.getItem(`edgecourt.${key}`); return value==null ? fallback : JSON.parse(value); } catch { return fallback; } };
const storePreference = (key, value) => { try { localStorage.setItem(`edgecourt.${key}`,JSON.stringify(value)); } catch {} };
// Appearance: 'system' (default) follows the OS, 'light'/'dark' are explicit.
// The effective theme lives on <html data-theme> so styles.css can theme every
// surface; only the preference is persisted, in this browser.
const themeMedia=window.matchMedia('(prefers-color-scheme: dark)');
const storedTheme=readPreference('theme','system');
let themePreference=['light','dark','system'].includes(storedTheme)?storedTheme:'system';
let theme=themePreference==='system'?(themeMedia.matches?'dark':'light'):themePreference;
function applyTheme() {
  theme=themePreference==='system'?(themeMedia.matches?'dark':'light'):themePreference;
  document.documentElement.dataset.theme=theme;
  document.documentElement.dataset.themePreference=themePreference;
  document.documentElement.style.colorScheme=theme;
  const meta=$('#theme-color-meta');if(meta)meta.content=theme==='dark'?'#0b1220':'#ffffff';
  const toggle=$('#theme-toggle');
  if(toggle){toggle.innerHTML=icon(theme==='dark'?'sun':'moon');toggle.dataset.icon=theme==='dark'?'sun':'moon';toggle.title=theme==='dark'?'Switch to light mode':'Switch to dark mode';toggle.setAttribute('aria-label',toggle.title);toggle.setAttribute('aria-pressed',String(theme==='dark'));}
  const select=$('#theme-input');if(select)select.value=themePreference;
}
function setThemePreference(preference) { themePreference=['light','dark','system'].includes(preference)?preference:'system';storePreference('theme',themePreference);applyTheme(); }
themeMedia.addEventListener?.('change',()=>{if(themePreference==='system')applyTheme();});
applyTheme();
const savedPref=readPreference('saved',[]);
const state = {data:null,token:'',view:'markets',category:'all',time:'all',gameId:'',query:'',book:'',evOnly:false,minEv:0,includeUnpriced:false,sort:'ev',page:0,pageSize:8,selected:'',saved:new Set(Array.isArray(savedPref)?savedPref.filter(x=>typeof x==='string'):[]),bankroll:readPreference('bankroll',1000),showSizing:readPreference('showSizing',false),loading:false,lastMode:''};
let toastTimer, pollTimer;
function toast(message, error=false) { $('#toast').textContent=message;$('#toast').classList.remove('hidden');$('#toast').classList.toggle('error',error);clearTimeout(toastTimer);toastTimer=setTimeout(()=>$('#toast').classList.add('hidden'),error?6500:4000); }
async function api(path, body) {
  const options=body==null?{}:{method:'POST',headers:{'Content-Type':'application/json','X-Edgecourt-Token':state.token},body:JSON.stringify(body)};
  const response=await fetch(path,options);let result;
  try{result=await response.json();}catch{throw new Error('The local server returned an unexpected response.');}
  if(!response.ok)throw new Error(result.error || `Request failed (${response.status}).`);
  return result;
}
async function getState(silent=false) {
  if(state.loading)return;state.loading=true;
  try{
    const data=await api('/api/state');state.data=data;state.token=data.authToken;
    if(state.lastMode!==data.mode){state.lastMode=data.mode;state.selected='';state.gameId='';state.page=0;state.includeUnpriced=data.mode==='live' && !data.summary.quotes;$('#unpriced-filter').checked=state.includeUnpriced;}
    if(!state.selected)state.selected=(data.markets.find(m=>m.player==='Stephen Curry' && m.market==='points' && m.side==='over') || data.markets.find(m=>m.eligible) || data.markets[0] || {}).id || '';
    $('#loading-overlay').classList.add('hidden');$('#connection-banner').classList.add('hidden');render();
  }catch(error){
    if(!silent || !state.data){$('#loading-overlay').classList.add('hidden');$('#connection-banner').textContent=`Can't reach the local C++ server. Keep the executable window open, then reload. ${error.message}`;$('#connection-banner').classList.remove('hidden');}
  }finally{state.loading=false;}
}
const seasonSuffix = g => g?.seasonType==='preseason'?' · Preseason':g?.seasonType==='playoffs'?' · Playoffs':'';
function localStart(date) {
  const d=new Date(date);if(Number.isNaN(d.getTime()))return 'Time unavailable';
  const today=new Date();const same=d.toDateString()===today.toDateString();
  const tomorrow=new Date();tomorrow.setDate(tomorrow.getDate()+1);
  const day=same?'Today':d.toDateString()===tomorrow.toDateString()?'Tomorrow':d.toLocaleDateString(undefined,{month:'short',day:'numeric'});
  return `${day} · ${d.toLocaleTimeString(undefined,{hour:'numeric',minute:'2-digit'})}`;
}
function ageLabel(date) {
  const age=Math.max(0,(Date.now()-new Date(date).getTime())/1000);if(!Number.isFinite(age))return 'No timestamp';
  if(age<60)return 'Just retrieved';if(age<3600)return `${Math.floor(age/60)}m old`;if(age<86400)return `${Math.floor(age/3600)}h old`;return `${Math.floor(age/86400)}d old`;
}
function selectionLabel(m) {
  if(m.market==='moneyline')return `${m.team} to win`;
  if(isBinary(m.market))return `${m.marketLabel || shortLabels[m.market]} · ${m.side==='no'?'No':'Yes'}`;
  return `${m.side==='under'?'Under':'Over'} ${m.line==null?'—':num(m.line,Number.isInteger(m.line)?0:1)} ${shortLabels[m.market]?.toLowerCase() || ''}`;
}
function filteredMarkets() {
  if(!state.data)return [];
  const query=state.query.trim().toLowerCase();
  const filtered=state.data.markets.filter(m=>{
    if(state.view==='watchlist' && !state.saved.has(m.id))return false;
    if(state.category==='props' && !['points','rebounds','assists','threes'].includes(m.market))return false;
    if(!['all','props'].includes(state.category) && state.category!==m.market)return false;
    if(state.time!=='all' && m.game?.status!==state.time)return false;
    if(state.gameId && state.gameId!==m.gameId)return false;
    if(state.book && state.book!==m.book)return false;
    if(!state.includeUnpriced && m.oddsPercent==null)return false;
    if(state.evOnly && (!m.eligible || m.ev==null || m.ev<=Math.max(0,state.minEv)))return false;
    if(!state.evOnly && state.minEv!==0 && (m.ev==null || m.ev<state.minEv))return false;
    if(query && !`${m.player} ${m.team} ${m.matchup} ${m.marketLabel} ${selectionLabel(m)} ${m.book}`.toLowerCase().includes(query))return false;
    return true;
  });
  return filtered.sort((a,b)=>{
    if(state.sort==='probability')return (b.probability??-1)-(a.probability??-1);
    if(state.sort==='start')return new Date(a.game?.startsAt||0)-new Date(b.game?.startsAt||0);
    if(state.sort==='player')return (a.player||a.team).localeCompare(b.player||b.team);
    return (b.ev??-1e6)-(a.ev??-1e6) || (a.player||a.team).localeCompare(b.player||b.team);
  });
}
function render() {
  const data=state.data;if(!data)return;
  const demo=data.mode==='demo';
  $('#mode-banner').classList.toggle('live-mode',!demo);
  $('.demo-icon').innerHTML=icon(demo?'flask':'shield');
  $('#mode-banner>span:nth-child(2)').innerHTML=demo?'<strong>Demo workspace</strong> <span class="banner-description">Synthetic games, prices & player histories. Explore the workflow, not real betting opportunities.</span>':'<strong>Public-data workspace</strong> <span class="banner-description">Keyless sources + your imported odds. Indicative prices. Experimental estimates. Verify before betting.</span>';
  $('#connect-live').innerHTML=demo?`Connect live data ${icon('arrowRight')}`:`Data health ${icon('arrowRight')}`;
  const unavailable=data.sources.filter(s=>s.state==='unavailable').length;
  $('#health-label').textContent=demo?'Demo workspace':data.refreshing?'Refreshing sources…':unavailable?'Some feeds unavailable':'Public-data workspace';
  $('.health-dot').style.background=demo?'#e3ae45':unavailable?'#df8790':'#6fac8a';
  $$('[data-mode]').forEach(b=>b.classList.toggle('active',b.dataset.mode===data.mode));
  $('#refresh-button').classList.toggle('spinning',data.refreshing);$('#refresh-button').disabled=data.refreshing;
  $$('[data-view]').forEach(b=>b.classList.toggle('active',b.dataset.view===state.view));
  $('#watch-count').textContent=state.saved.size;$('#side-watch-count').textContent=state.saved.size;
  $('#markets-view').classList.toggle('hidden',state.view==='lab');$('#lab-view').classList.toggle('hidden',state.view!=='lab');
  if(state.view==='lab'){renderLab();return;}
  $('#page-title').textContent=state.view==='watchlist'?'Your markets. Your watchlist.':'A clearer view of the court.';
  $('#page-subtitle').textContent=state.view==='watchlist'?'Saved for closer inspection. A saved quote is not a locked-in price.':'Compare prices. Understand the model. Find the difference.';
  renderSidebar();renderStats();renderGames();renderBooks();renderRows();renderPanel();
}
function renderSidebar() {
  const data=state.data;
  $('#category-nav').innerHTML=categories.map(c=>{
    const count=data.markets.filter(m=>(c.id==='all' || c.id==='props'?c.id==='all' || ['points','rebounds','assists','threes'].includes(m.market):m.market===c.id) && m.oddsPercent!=null).length;
    return `<button class="category-link ${c.sub?'subcategory':''} ${state.category===c.id?'active':''}" data-category="${c.id}">${c.icon?`<span>${icon(c.icon)}</span>`:''}<span>${c.label}</span>${!c.sub?`<span class="side-count">${count}</span>`:''}</button>`;
  }).join('');
  if($('#category-filter'))$('#category-filter').value=state.category;
}
function renderStats() {
  const summary=state.data.summary;let positive=summary.positiveEv,priced=summary.priced;
  if(state.view==='watchlist'){const watched=state.data.markets.filter(m=>state.saved.has(m.id));positive=watched.filter(m=>m.eligible && m.ev>0).length;priced=watched.filter(m=>m.eligible).length;}
  const tiles=[
    {icon:'sparkle',label:'Model +EV markets',value:positive,small:'estimates',detail:'Not a validated edge'},
    {icon:'layers',label:'Evaluable quotes',value:priced,small:`/ ${summary.quotes}`,detail:'Fresh price + inputs'},
    {icon:'activity',label:'On the court',value:summary.games,small:summary.liveGames?`${summary.liveGames} ${state.data.mode==='demo'?'simulated live':'live'}`:'scheduled',detail:'Upcoming & live'},
    {icon:'database',label:'Player histories',value:summary.players,small:'players',detail:state.data.mode==='demo'?'Synthetic sample':'Public / imported'}
  ];
  $('#stats-strip').innerHTML=tiles.map(t=>`<div class="stat-tile"><span class="stat-icon">${icon(t.icon)}</span><div><div class="stat-label">${t.label}</div><div class="stat-value">${t.value}<small>${t.small}</small></div></div><span class="stat-detail">${t.detail}</span></div>`).join('');
}
function renderGames() {
  let games=state.data.games.filter(g=>g.status!=='final');if(state.time!=='all')games=games.filter(g=>g.status===state.time);
  if(state.view==='watchlist'){const ids=new Set(state.data.markets.filter(m=>state.saved.has(m.id)).map(m=>m.gameId));games=games.filter(g=>ids.has(g.id));}
  games.sort((a,b)=>(a.status==='live'?-1:1)-(b.status==='live'?-1:1) || new Date(a.startsAt)-new Date(b.startsAt));
  $('#game-count').textContent=`${games.length} ${games.length===1?'game':'games'}`;
  $$('[data-time]').forEach(b=>b.classList.toggle('active',b.dataset.time===state.time));
  $('#games-strip').innerHTML=games.length?games.slice(0,12).map(g=>{
    const isLive=g.status==='live';
    const quote=side=>state.data.markets.find(m=>m.gameId===g.id && m.market==='moneyline' && m.side===side && m.oddsPercent!=null && m.freshness==='fresh');
    const teamRow=side=>{const q=quote(side);return `<div class="game-team"><span class="team-badge" style="${teamStyle(g[side])}">${esc(g[side])}</span><strong>${esc(g[`${side}Name`] || g[side])}</strong><span class="game-odds" title="Bookmaker implied probability, including margin">${odds(q?.oddsPercent)}</span>${isLive?`<span class="game-score">${g[`${side}Score`]||0}</span>`:''}</div>`;};
    return `<button class="game-card ${state.gameId===g.id?'selected':''}" data-game="${esc(g.id)}" title="Filter to this matchup" aria-pressed="${state.gameId===g.id}"><div class="game-meta"><span class="status-pill ${isLive?'live':''}">${isLive?'<i class="live-dot"></i>':''}${state.data.mode==='demo' && isLive?'SIMULATED LIVE':isLive?'LIVE':g.status==='upcoming'?'UPCOMING':esc(g.status.toUpperCase())}</span><span>${isLive?`Q${g.period} · ${esc(g.clock)}`:esc(localStart(g.startsAt))}</span></div>${teamRow('away')}${teamRow('home')}<div class="game-bottom"><span>${esc(g.venue || 'NBA · Full game')}${g.seasonType==='preseason'?' · Preseason':g.seasonType==='playoffs'?' · Playoffs':''}</span>${icon('arrowUpRight')}</div></button>`;
  }).join(''):`<div class="game-empty"><strong>${state.data.refreshing?'Connecting to public NBA sources…':state.view==='watchlist'?'Save a market to follow its matchup.':'No games available in this view.'}</strong>${state.data.refreshing?'Public sources can be slow or blocked. The app will not substitute fictional games.':state.data.mode==='live'?'Check Model lab → Data sources. You can also import a schedule and your own feature history.':'Change the status filter to see more demo matchups.'}</div>`;
}
function renderBooks() {
  const books=[...new Set(state.data.markets.filter(m=>m.oddsPercent!=null).map(m=>m.book))].sort();
  if(state.book && !books.includes(state.book))state.book='';
  const html='<option value="">All books</option>'+books.map(b=>`<option value="${esc(b)}">${esc(b)}</option>`).join('');
  if($('#book-filter').innerHTML!==html)$('#book-filter').innerHTML=html;
  $('#book-filter').value=state.book;
}
function renderRows() {
  const rows=filteredMarkets();const pages=Math.max(1,Math.ceil(rows.length/state.pageSize));state.page=Math.min(state.page,pages-1);
  const visible=rows.slice(state.page*state.pageSize,(state.page+1)*state.pageSize);
  if(rows.length && !rows.some(m=>m.id===state.selected))state.selected=visible[0].id;
  if(!rows.length)state.selected='';
  $('#board-title').textContent=state.view==='watchlist'?'Saved markets':state.category==='all'?'All NBA markets':categories.find(c=>c.id===state.category)?.label || 'NBA markets';
  const ready=rows.filter(m=>m.eligible).length;
  $('#result-count').textContent=`${rows.length} markets · ${ready} evaluable · ${state.data.mode==='demo'?'synthetic prices':'public / imported prices'}`;
  $('#ev-filter').classList.toggle('active',state.evOnly);$('#ev-filter').setAttribute('aria-pressed',String(state.evOnly));
  $('#market-rows').innerHTML=visible.map(m=>{
    const title=m.player || (m.market==='moneyline'?`${m.team} to win`:`${m.team} team total`);
    const live=m.game?.status==='live';const selected=state.selected===m.id;
    const ev=m.ev==null?`<span class="ev-chip neutral" title="${esc(m.reason || 'Current price or model inputs unavailable')}">${m.freshness==='stale'?'Stale':'—'}</span>`:`<span class="ev-chip ${m.ev>0?'':'negative'}">${m.ev>0?icon('trend'):''}${signed(m.ev)}%</span>`;
    const avatar=m.player?initials(m.player):m.team;
    return `<tr class="${selected?'selected':''}" data-market="${esc(m.id)}" tabindex="0" aria-label="Inspect ${esc(title)} ${esc(selectionLabel(m))}"><td><div class="market-cell"><span class="player-avatar" style="${teamStyle(m.team)}">${esc(avatar)}</span><div class="market-identity"><div class="market-name">${esc(title)}</div><div class="market-line">${live?'<i class="tiny-status"></i>':''}<span>${esc(shortLabels[m.market])}</span><span>·</span><span class="line-value">${m.market==='moneyline'?'Full game':esc(m.side==='yes'?'Yes':m.side==='no'?'No':`${m.side==='over'?'O':'U'} ${m.line==null?'—':num(m.line,Number.isInteger(m.line)?0:1)}`)}</span></div></div></div></td><td><div class="matchup-label">${esc(m.matchup || '—')}<small>${live?`Q${m.game.period} · ${esc(m.game.clock)}`:esc(localStart(m.game?.startsAt))}${esc(seasonSuffix(m.game))}</small></div></td><td><div class="price-value" title="Bookmaker implied probability, including margin">${odds(m.oddsPercent)}</div><div class="book-label" title="${esc(m.book)}">${esc(m.book)}</div></td><td><div class="model-value">${pct(m.probability)}</div><div class="model-sub">${m.probability==null?'Missing inputs':'experimental'}</div></td><td>${ev}</td><td><button class="watch-button ${state.saved.has(m.id)?'saved':''}" data-save="${esc(m.id)}" title="${state.saved.has(m.id)?'Unsave':'Save'} market" aria-label="${state.saved.has(m.id)?'Unsave':'Save'} ${esc(title)}" aria-pressed="${state.saved.has(m.id)}">${icon('star')}</button></td></tr>`;
  }).join('');
  $('#empty-state').classList.toggle('hidden',rows.length>0);
  if(!rows.length){const watch=state.view==='watchlist';$('#empty-state').innerHTML=`<span>${icon(watch?'bookmark':'search')}</span><h3>${watch && !state.saved.size?'A little research worth saving.':'No markets match this view.'}</h3><p>${watch && !state.saved.size?'Tap the star beside a market to keep it here. Prices can change; saved does not mean locked.':state.data.mode==='live' && !state.data.summary.quotes?'Public sources have not returned usable prices. Import current odds or enable “Include unpriced markets” to inspect coverage.':'Try a different category, game or filter. Positive estimated EV still requires current prices and complete model inputs.'}</p><button class="button button-outline" data-empty-action="${watch && !state.saved.size?'markets':'reset'}">${watch && !state.saved.size?'Explore markets':'Reset filters'} ${icon('arrowRight')}</button>`;}
  $('#rows-status').textContent=rows.length?`${state.page*state.pageSize+1}–${Math.min((state.page+1)*state.pageSize,rows.length)} of ${rows.length} markets`:'No matching markets';
  $('#page-status').textContent=`${state.page+1} / ${pages}`;$('#prev-page').disabled=state.page===0;$('#next-page').disabled=state.page>=pages-1;
  $('#active-game-filter').classList.toggle('hidden',!state.gameId);
  if(state.gameId){const g=state.data.games.find(g=>g.id===state.gameId);$('#active-game-filter').innerHTML=`Viewing ${esc(g?`${g.away} @ ${g.home}`:state.gameId)}<button id="remove-game-filter">Clear matchup ${icon('x')}</button>`;}
}
function renderPanel() {
  const m=state.data.markets.find(m=>m.id===state.selected);const panel=$('#research-panel');
  if(!m){panel.innerHTML=`<div class="research-head"><span class="eyebrow">THE RESEARCH DESK</span><span class="experimental-pill">EXPERIMENTAL</span></div><div class="panel-empty">${icon('target')}Choose a market to see its projection, supporting inputs and the limitations behind the number.</div>`;return;}
  const name=m.player || (m.market==='moneyline'?`${m.team} moneyline`:`${m.team} team total`);
  const available=m.probability!=null;const percentProjection=isBinary(m.market);const fresh=m.freshness==='fresh';
  const featureRow=f=>`<div class="feature-row ${f.role==='context'?'context':''}"><span>${esc(f.label)}</span><strong>${num(f.value,Math.abs(f.value)<2?2:1)} ${esc(f.unit)}</strong></div>`;
  const samples=available?`<div class="projection-sub">${num(m.effectiveSampleSize,0)} effective historical games · ${esc(m.quality)}</div>`:'';
  const chart=m.recent?.length?(()=>{const recent=[...m.recent].reverse();const maximum=Math.max(1,...recent.map(r=>r.value))*1.12;return `<div class="recent-block"><div class="mini-heading">Last ${recent.length} games <span>${m.recentHitRate==null?'Historical scores':`${num(m.recentHitRate,0)}% raw hit rate`}</span></div><div class="recent-chart" aria-label="Historical game stats, oldest to newest">${recent.map(r=>`<div class="recent-bar ${r.hit?'hit':''}" style="height:${Math.max(3,r.value/maximum*100)}%" title="${esc(r.date)} · ${r.value}${r.hit?' · Hit':''}"></div>`).join('')}</div><div class="chart-labels"><span>OLDEST</span><span>NEWEST</span></div></div>`;})():'';
  const preview=(m.features||[]).filter(f=>f.role==='used').slice(0,4);
  const warnings=m.caveats || [];
  panel.innerHTML=`<div class="research-head"><span class="eyebrow">THE RESEARCH DESK</span><span class="experimental-pill">EXPERIMENTAL</span></div>
    <div class="research-identity"><span class="player-avatar" style="${teamStyle(m.team)}">${esc(m.player?initials(m.player):m.team)}</span><div><h3>${esc(name)}</h3><p>${esc(m.matchup || '')} · ${m.game?.status==='live'?'In play':esc(localStart(m.game?.startsAt))}${esc(seasonSuffix(m.game))}</p></div></div>
    <div class="research-selection"><span>${esc(selectionLabel(m))}</span><span title="Bookmaker implied probability, including margin">${odds(m.oddsPercent)}</span></div>
    <div class="research-quote-meta"><span>${esc(m.book)}</span><span class="research-price-age ${fresh?'':'stale'}">${m.oddsPercent==null?'No quote':esc(ageLabel(m.observedAt))}</span></div>
    ${available?`<div class="projection-block"><div class="projection-caption">${esc(m.projectionLabel || 'Model projection')}</div><div class="projection-value">${num(m.projected)}<small>${percentProjection?'%':m.market==='moneyline'?'pts margin':'projected'}</small></div>${samples}<div class="probability-track"><div><i style="width:${m.probability*100}%"></i></div><strong>${pct(m.probability)}</strong></div><div class="projection-footer"><span>Model win probability</span><span>${m.pushProbability>0?`${pct(m.pushProbability)} push`:'No push estimated'}</span></div><div class="projection-sub">Sampling interval: ${pct(m.samplingInterval?.lower)}–${pct(m.samplingInterval?.upper)}. Not model confidence.</div></div>
    <div class="research-stat-grid"><div class="research-stat"><span>MODEL EV / $100</span><strong class="${m.ev>0?'positive':''}">${m.ev==null?'—':`${signed(m.ev)}%`}</strong><small>${m.ev==null?'Disabled — stale quote':`${money(100+m.ev)} expected return`}</small></div><div class="research-stat"><span>NO-VIG EDGE</span><strong>${m.edge==null?'—':`${signed(m.edge)}pp`}</strong><small>${m.fairProbability==null?'No matching opposite quote':`${pct(m.fairProbability)} market fair %`}</small></div></div>${chart}
    <div class="feature-preview"><div class="mini-heading">Inputs used <span>${(m.features||[]).filter(f=>f.role==='used').length} available</span></div><div class="features-list">${preview.map(featureRow).join('')}</div></div>
    <details class="feature-detail"><summary>All inputs & context</summary><div class="features-list">${(m.features||[]).map(featureRow).join('')}</div></details>`:`<div class="research-unavailable"><strong>Not enough to call an edge.</strong>${esc(m.reason || 'Missing model inputs or a current bookmaker quote.')}<br><br>Missing data stays missing. We do not generate replacement odds.</div>`}
    ${state.showSizing && m.kellyFraction!=null?`<div class="sizing-block">ILLUSTRATIVE ONLY · HIGH MODEL RISK<strong>${money(state.bankroll*m.kellyFraction)} at ¼ Kelly</strong>Capped at 2% of ${money(state.bankroll)}. Uncalibrated probabilities make this unreliable. You can lose the entire stake.</div>`:''}
    <div class="research-warning">${icon('info')}<span>${state.data.mode==='demo'?'This entire market is synthetic. These are not prices you can bet.':'Public/imported quotes are indicative. Confirm availability, price, player status and settlement at your sportsbook.'}</span></div>
    <button class="button button-outline save-selection ${state.saved.has(m.id)?'saved':''}" data-save="${esc(m.id)}">${icon('star')}${state.saved.has(m.id)?'Saved to watchlist':'Save to watchlist'}</button>
    <details class="caveats-detail"><summary>Limitations & provenance (${warnings.length})</summary><ul>${warnings.map(w=>`<li>${esc(w)}</li>`).join('')}</ul></details>`;
}
async function reloadHistory() {
  try{const result=await api('/api/history/reload',{});await getState();toast(result.message);}catch(error){toast(error.message,true);}
}
function renderHistory() {
  const h=state.data.historicalData || {state:'not_installed',message:'No historical archive installed.'};
  const count=n=>new Intl.NumberFormat().format(n || 0);
  return `<section class="lab-card full history-card"><h2>Historical play-by-play <span>Basketball Reference · local archive</span></h2>
    <div class="history-metrics"><div><strong>${count(h.games)}</strong><span>archived games</span></div><div><strong>${count(h.events)}</strong><span>play-by-play events</span></div><div><strong>${count(h.players)}</strong><span>recent player histories</span></div><div><strong>${count(h.missingGames)}</strong><span>missing / rejected games</span></div></div>
    <p><strong>${esc(h.state.replace(/_/g,' '))}</strong> · ${esc(h.message)}</p>
    ${h.firstDate?`<p class="muted">${esc(h.firstDate)}–${esc(h.lastDate)} · ${esc(h.coverage || 'Partial coverage')} · ${state.data.mode==='demo'?'Not applied to synthetic Demo':'Applied to Live-data estimates when eligibility guards pass'}</p>`:''}
    <p>All collected events stay in a resumable SQLite archive. The model uses recent complete game vectors and dated first-made-field-goal opportunities for historical starters. It still needs a current schedule, prices and confirmed starters; it does not train itself or invent missing history.</p>
    <pre>python scripts/basketball_reference_history.py status --output edgecourt-data/history
Edgecourt --live --data-dir edgecourt-data</pre>
    <button class="button button-outline" id="history-reload">${icon('refresh')}Reload local history</button>
    <div class="coverage-note">The full crawl has not been run in this sandbox. Basketball Reference access is restricted here. Only run the optional collector if you have permission for your use under Sports Reference's data-use terms. It respects robots rules, waits at least 3.1 seconds between requests, and stops on access or rate-limit denials. See docs/HISTORICAL_DATA.md for collection, saved-page ingestion and coverage details.</div></section>`;
}
function renderLab() {
  const data=state.data;const roles=data.featureCatalog.reduce((a,f)=>(a[f.role]=(a[f.role]||0)+1,a),{});
  const sourceCard=s=>`<div class="source-card"><span class="source-icon">${icon(s.id==='demo'?'flask':s.id==='prop-odds'?'upload':'database')}</span><div class="source-body"><div class="source-title"><span>${esc(s.label)}</span><span class="source-badge ${esc(s.state)}">${esc(s.state.replace(/_/g,' '))}</span></div><p>${esc(s.message)}</p><small>${s.updatedAt?esc(ageLabel(s.updatedAt)):'No retrieval timestamp'} · ${s.count||0} records / groups</small></div></div>`;
  $('#lab-view').innerHTML=`<div class="lab-heading"><div class="eyebrow">OPEN THE BLACK BOX</div><h1>More context. Less guesswork.</h1><p>A transparent baseline, honest feed coverage, and a clear distinction between inputs used and inputs we do not have.</p></div>
    <div class="lab-warning">${icon('warning')}<div><strong>This model has not established an edge.</strong>It is an experimental research baseline, not a trained or calibrated betting model. There is no measured out-of-sample win rate or profitability claim. More features do not automatically make better predictions. Never treat simulated accuracy as evidence of a profitable strategy.</div></div>
    <div class="lab-grid">${renderHistory()}<section class="lab-card"><h2>Data sources <span>keyless · no logins</span></h2>${data.sources.map(sourceCard).join('')}<button class="button button-outline" id="lab-refresh">${icon('refresh')}Refresh public sources</button><div class="coverage-note">Odds are always displayed as bookmaker-implied percentages (including the book's margin), separately from Model %. The sandbox preview cannot access ESPN/NBA/Basketball Reference hosts. Public adapters are implemented and fixture-tested, but their live responses are not verified here. On your Windows computer, availability depends on the provider, your network and location. Unavailable feeds never fall back to demo data.</div></section>
    <section class="lab-card"><h2>How estimates are produced</h2><ol><li><strong>Guard the inputs.</strong> Check event state, quote freshness, player availability, settlement scope and pre-game history. Regular-season histories are not used for preseason projections.</li><li><strong>Project the opportunity.</strong> Recent minutes are blended with weighted historical minutes. Team/opponent pace adjusts the stat rate. Injury or manual minutes overrides are exposed.</li><li><strong>Resample complete games.</strong> Draw 8,192 joint stat vectors with recency/location weights and a small shared rate perturbation. PRA and double/triple-doubles preserve historical covariance, including steals and blocks.</li><li><strong>Condition live estimates.</strong> Accrued stats and remaining minutes/score matter. Stale live box scores disable in-play props. Substitutions and possession-level dynamics are not modeled.</li><li><strong>Compare actual prices.</strong> Separate win, loss and push outcomes. De-vig only matched opposite selections from the same book, line and price window.</li></ol><pre>EV = P(win) / (book_odds_% / 100) + P(push) − 1\nNo-vig p = implied_p / paired_implied_sum\nEffective N = (sum weights)² / sum weights²</pre><p>Moneyline uses team scoring ratings, chronological Elo, rest differential and home court with a normal margin baseline. Team totals use pace, offense/defense and historical scoring variance. First basket requires all 10 confirmed starters and explicit historical first-FG winners; points-per-game is not a proxy.</p><div class="coverage-note">8,192 simulated draws are not 8,192 independent games. The displayed interval uses effective historical sample size and captures only sampling variation, not model misspecification, injuries or price latency.</div></section>
    <section class="lab-card full"><h2>Feature inventory <span>${data.featureCatalog.length} distinct signals · ${roles.used||0} used · ${roles.context||0} contextual · ${roles.guard||0} guards</span></h2><p>Per-market research panels show the inputs actually available for that estimate. Contextual fields do not secretly affect the probability; future inputs are explicitly not implemented.</p><div class="feature-catalog" style="margin-top:16px">${data.featureCatalog.map(f=>`<div class="catalog-item"><div><span>${esc(f.group)}</span><span class="role-tag ${esc(f.role.replace(/ /g,'-'))}">${esc(f.role)}</span></div><strong>${esc(f.label)}</strong><p>${esc(f.source)}</p></div>`).join('')}</div></section>
    <section class="lab-card"><h2>Validation before conviction</h2><p>The C++ CLI scores supplied predictions with Brier score, log loss, push-aware flat-stake ROI and a pre-game timestamp guard. To compare against the closing market, give every row a closing no-vig probability (or both closing prices), a closing timestamp and game ID. It reports paired scores overall and by market; negative model-minus-market loss favors the model. This does not train the estimator or authenticate source timestamps.</p><pre>Edgecourt.exe --backtest predictions.json</pre><p>Use true walk-forward forecasts, obtainable prices, complete matched markets, every predeclared prediction, and enough independent games. Pushes are excluded from probability scores and refunded in ROI. The approximate game-clustered interval is shown only with 30+ resolved game clusters, and is not proof of independence or profitability. No real market-beating result is established in this workspace.</p></section>
    <section class="lab-card"><h2>Built small. Built locally.</h2><p>The prediction engine, HTTP server, import validation and live public-data clients run in C++. The optional historical collector uses Python’s standard library, outside the app. The browser renders embedded HTML/CSS/JavaScript. Windows HTTPS uses the OS WinHTTP stack; no Python, Node, account or API key is required to run the executable.</p><div class="lab-ref"><a href="https://github.com/yhirose/cpp-httplib" target="_blank" rel="noopener noreferrer">cpp-httplib ${icon('arrowUpRight')}</a><a href="https://github.com/nlohmann/json" target="_blank" rel="noopener noreferrer">nlohmann/json ${icon('arrowUpRight')}</a><a href="https://github.com/swar/nba_api" target="_blank" rel="noopener noreferrer">NBA endpoint reference ${icon('arrowUpRight')}</a></div><div class="coverage-note">GitHub references are documented in the source package. Unlicensed betting repositories were researched but their code and trained model artifacts are not included.</div></section></div>`;
}
function setView(view) { state.view=view;state.page=0;render();window.scrollTo({top:0,behavior:'smooth'}); }
function resetFilters() { state.category='all';state.time='all';state.gameId='';state.query='';state.book='';state.evOnly=false;state.minEv=0;state.sort='ev';state.page=0;state.includeUnpriced=state.data?.mode==='live' && !state.data?.summary.quotes;$('#search-input').value='';$('#min-ev').value='0';$('#sort-filter').value='ev';$('#unpriced-filter').checked=state.includeUnpriced;render(); }
function saveMarket(id) { state.saved.has(id)?state.saved.delete(id):state.saved.add(id);storePreference('saved',[...state.saved]);render(); }
async function changeMode(mode) {
  if(!state.data || mode===state.data.mode)return;
  try{await api('/api/mode',{mode});await getState();toast(mode==='live'?'Requesting public feeds. Missing prices will remain unpriced.':'Synthetic demo workspace opened.');}catch(error){toast(error.message,true);}
}
async function refreshData() {
  try{$('#refresh-button').classList.add('spinning');await api('/api/refresh',{});await getState();toast(state.data.mode==='demo'?'Synthetic demonstration reset.':'Refresh queued. Sources may take a moment.');}catch(error){toast(error.message,true);}finally{$('#refresh-button').classList.toggle('spinning',Boolean(state.data?.refreshing));}
}
function openImport() {
  if(!state.data)return;
  $('#import-game').innerHTML=state.data.games.filter(g=>!['final','cancelled','postponed'].includes(g.status)).map(g=>`<option value="${esc(g.id)}">${esc(g.away)} @ ${esc(g.home)} · ${esc(g.id)} · ${esc(localStart(g.startsAt))}</option>`).join('') || '<option value="">No games — import a schedule with JSON first</option>';
  $('#import-market').innerHTML=Object.entries(marketLabels).map(([key,label])=>`<option value="${key}">${label}</option>`).join('');
  $('#import-market').value=state.category!=='all' && state.category!=='props'?state.category:'points';
  if(state.gameId)$('#import-game').value=state.gameId;
  $('#import-mode-note').textContent=state.data.mode==='demo'?'DEMO: Imports affect synthetic data only and reset on refresh. Switch to Live data to save real research inputs.':'Imports are saved locally. Quote timestamps are checked; an old imported price does not become fresh on refresh.';
  $('#manual-error').classList.add('hidden');$('#file-error').classList.add('hidden');updateImportFields();$('#import-dialog').showModal();
}
function updateImportFields() {
  const market=$('#import-market').value;const game=state.data?.games.find(g=>g.id===$('#import-game').value);
  const oldTeam=$('#import-team').value;$('#import-team').innerHTML=game?`<option value="${esc(game.home)}">${esc(game.home)}</option><option value="${esc(game.away)}">${esc(game.away)}</option>`:'';
  if(game && [game.home,game.away].includes(oldTeam))$('#import-team').value=oldTeam;
  $('#player-field').classList.toggle('hidden',!isPlayer(market));$('#import-player').required=isPlayer(market);$('#line-field').classList.toggle('hidden',isBinary(market) || market==='moneyline');$('#import-line').required=!isBinary(market) && market!=='moneyline';
  $('#import-side').innerHTML=market==='moneyline'?'<option value="home">Home</option><option value="away">Away</option>':isBinary(market)?'<option value="yes">Yes</option><option value="no">No</option>':'<option value="over">Over</option><option value="under">Under</option>';
  if(market==='moneyline' && game)$('#import-side').value=$('#import-team').value===game.home?'home':'away';
  updatePlayerOptions();
}
function updatePlayerOptions() {
  const team=$('#import-team').value;$('#player-options').innerHTML=(state.data?.players||[]).filter(p=>p.team===team).map(p=>`<option value="${esc(p.name)}"></option>`).join('');
}
async function submitImport(payload, formType) {
  const errorBox=$(`#${formType}-error`);const button=$(`#${formType}-submit`);errorBox.classList.add('hidden');button.disabled=true;
  try{const result=await api('/api/import',payload);$('#import-dialog').close();await getState();toast(`Imported ${result.quotes} quotes, ${result.players} player histories, ${result.teams} teams, ${result.games} games and ${result.lineups||0} lineup confirmations. ${result.message}`);}catch(error){errorBox.textContent=error.message;errorBox.classList.remove('hidden');}finally{button.disabled=false;}
}
function exportMarkets() {
  const rows=filteredMarkets();const headers=['gameId','player','team','market','side','line','book','oddsPercent','observedAt','modelWinProbabilityPercent','pushProbabilityPercent','modelEVPercent','availability','reason','mode','modelVersion'];
  const cell=v=>{const text=String(v??'');const safe=typeof v==='string' && /^[\t\r ]*[=+@-]/.test(text)?"'"+text:text;return `"${safe.replace(/"/g,'""')}"`;};
  const csv=headers.join(',')+'\r\n'+rows.map(m=>[m.gameId,m.player,m.team,m.market,m.side,m.line,m.book,m.oddsPercent,m.observedAt,m.probability==null?null:m.probability*100,m.pushProbability==null?null:m.pushProbability*100,m.ev,m.availability,m.reason,state.data.mode,state.data.modelVersion].map(cell).join(',')).join('\r\n');
  const url=URL.createObjectURL(new Blob(['\ufeff'+csv],{type:'text/csv;charset=utf-8'}));const a=document.createElement('a');a.href=url;a.download=`edgecourt-${state.data.mode}-${nowISO().slice(0,10)}.csv`;document.body.append(a);a.click();a.remove();setTimeout(()=>URL.revokeObjectURL(url),2000);toast(`Exported ${rows.length} markets. Model estimates remain experimental.`);
}

function bindEvents() {
  document.addEventListener('click',event=>{
    const view=event.target.closest('[data-view]');if(view){setView(view.dataset.view);return;}
    const mode=event.target.closest('[data-mode]');if(mode){changeMode(mode.dataset.mode);return;}
    const category=event.target.closest('[data-category]');if(category){state.category=category.dataset.category;state.view='markets';state.page=0;render();return;}
    const time=event.target.closest('[data-time]');if(time){state.time=time.dataset.time;state.page=0;render();return;}
    const game=event.target.closest('[data-game]');if(game){state.gameId=state.gameId===game.dataset.game?'':game.dataset.game;state.page=0;render();return;}
    const save=event.target.closest('[data-save]');if(save){saveMarket(save.dataset.save);return;}
    const row=event.target.closest('[data-market]');if(row){state.selected=row.dataset.market;renderRows();renderPanel();return;}
    const close=event.target.closest('[data-close]');if(close){$(`#${close.dataset.close}`).close();return;}
    const tab=event.target.closest('[data-import-tab]');if(tab){$$('[data-import-tab]').forEach(b=>b.classList.toggle('active',b===tab));$('#manual-form').classList.toggle('hidden',tab.dataset.importTab!=='manual');$('#file-form').classList.toggle('hidden',tab.dataset.importTab!=='file');return;}
    const empty=event.target.closest('[data-empty-action]');if(empty){empty.dataset.emptyAction==='markets'?setView('markets'):resetFilters();return;}
    if(event.target.closest('#remove-game-filter')){state.gameId='';state.page=0;render();}
    if(event.target.closest('#lab-refresh'))refreshData();
    if(event.target.closest('#history-reload'))reloadHistory();
  });
  $('#market-rows').addEventListener('keydown',event=>{if(event.target.closest('button'))return;if(event.key==='Enter' || event.key===' '){const row=event.target.closest('[data-market]');if(row){event.preventDefault();state.selected=row.dataset.market;renderRows();renderPanel();}}});
  $('#connect-live').addEventListener('click',()=>state.data?.mode==='demo'?changeMode('live'):setView('lab'));
  $('#health-button').addEventListener('click',()=>setView('lab'));
  $('#refresh-button').addEventListener('click',refreshData);$('#import-button').addEventListener('click',openImport);$('#export-button').addEventListener('click',exportMarkets);
  $('#search-input').addEventListener('input',event=>{state.query=event.target.value;state.page=0;renderRows();renderPanel();});
  $('#ev-filter').addEventListener('click',()=>{state.evOnly=!state.evOnly;state.page=0;renderRows();renderPanel();});
  $('#book-filter').addEventListener('change',event=>{state.book=event.target.value;state.page=0;renderRows();renderPanel();});
  $('#more-filters').addEventListener('click',()=>{const open=$('#extra-filters').classList.toggle('hidden')===false;$('#more-filters').setAttribute('aria-expanded',String(open));});
  $('#min-ev').addEventListener('input',event=>{state.minEv=Math.min(500,Math.max(-100,Number(event.target.value)||0));state.page=0;renderRows();renderPanel();});
  $('#unpriced-filter').addEventListener('change',event=>{state.includeUnpriced=event.target.checked;state.page=0;renderRows();renderPanel();});
  $('#sort-filter').addEventListener('change',event=>{state.sort=event.target.value;state.page=0;renderRows();renderPanel();});
  $('#category-filter')?.addEventListener('change',event=>{state.category=event.target.value;state.page=0;render();});
  $('#clear-filters').addEventListener('click',resetFilters);
  $('#prev-page').addEventListener('click',()=>{state.page=Math.max(0,state.page-1);renderRows();});$('#next-page').addEventListener('click',()=>{state.page++;renderRows();});
  $('#import-game').addEventListener('change',updateImportFields);$('#import-market').addEventListener('change',updateImportFields);
  $('#import-team').addEventListener('change',()=>{updatePlayerOptions();const game=state.data.games.find(g=>g.id===$('#import-game').value);if($('#import-market').value==='moneyline' && game)$('#import-side').value=$('#import-team').value===game.home?'home':'away';});
  $('#import-side').addEventListener('change',()=>{if($('#import-market').value==='moneyline'){const game=state.data.games.find(g=>g.id===$('#import-game').value);if(game)$('#import-team').value=game[$('#import-side').value];}});
  $('#manual-form').addEventListener('submit',event=>{
    event.preventDefault();const f=new FormData(event.target);const market=f.get('market');const quote={gameId:f.get('gameId'),market,player:isPlayer(market)?String(f.get('player')).trim():'',team:f.get('team'),side:f.get('side'),line:isBinary(market) || market==='moneyline'?null:Number(f.get('line')),oddsPercent:Number(f.get('oddsPercent')),book:String(f.get('book')).trim(),observedAt:nowISO(),settlement:'full_game_including_ot'};
    submitImport({format:'json',data:JSON.stringify({quotes:[quote]})},'manual');
  });
  $('#file-format').addEventListener('change',()=>$('#html-fields').classList.toggle('hidden',$('#file-format').value!=='html'));
  $('#import-file').addEventListener('change',async event=>{const file=event.target.files[0];if(!file)return;if(file.size>4*1024*1024){toast('Import files must be under 4 MB.',true);return;}try{$('#import-content').value=await file.text();const ext=file.name.toLowerCase().split('.').pop();$('#file-format').value=ext==='json'?'json':['html','htm'].includes(ext)?'html':'csv';$('#html-fields').classList.toggle('hidden',$('#file-format').value!=='html');}catch{toast('Could not read this file.',true);}});
  $('#file-form').addEventListener('submit',event=>{event.preventDefault();submitImport({format:$('#file-format').value,data:$('#import-content').value,player:$('#html-player').value.trim(),team:$('#html-team').value.trim().toUpperCase(),observedAt:$('#html-observed-at').value.trim()},'file');});
  $('#theme-toggle').addEventListener('click',()=>setThemePreference(theme==='dark'?'light':'dark'));
  $('#settings-button').addEventListener('click',()=>{$('#bankroll-input').value=state.bankroll;$('#sizing-input').checked=state.showSizing;$('#theme-input').value=themePreference;$('#settings-dialog').showModal();});
  $('#save-settings').addEventListener('click',()=>{const amount=Number($('#bankroll-input').value);if(!Number.isFinite(amount) || amount<1 || amount>1e9){toast('Choose a bankroll between $1 and $1 billion.',true);return;}state.bankroll=amount;state.showSizing=$('#sizing-input').checked;storePreference('bankroll',amount);storePreference('showSizing',state.showSizing);setThemePreference($('#theme-input').value);$('#settings-dialog').close();renderPanel();toast('Research preferences saved in this browser.');});
  $('#clear-imports').addEventListener('click',async()=>{if(!window.confirm('Remove all locally saved quote, feature and schedule imports? This cannot be undone.'))return;try{await api('/api/clear-imports',{});$('#settings-dialog').close();await getState();toast('Saved imports cleared. Public caches were not deleted.');}catch(error){toast(error.message,true);}});
  document.addEventListener('keydown',event=>{if(event.key==='/' && !['INPUT','TEXTAREA','SELECT'].includes(document.activeElement?.tagName) && !$$('dialog[open]').length){event.preventDefault();if(state.view==='lab')setView('markets');$('#search-input').focus();}});
  document.addEventListener('visibilitychange',()=>{if(!document.hidden)getState(true);});
  window.addEventListener('online',()=>getState(true));
}

hydrateIcons();
if($('#category-filter'))$('#category-filter').innerHTML=categories.map(c=>`<option value="${c.id}">${c.label}</option>`).join('');
bindEvents();getState();pollTimer=setInterval(()=>{if(!document.hidden)getState(true);},15000);

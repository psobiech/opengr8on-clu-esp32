/**
 * @file ota_page.h
 * @brief GET / of the OTA server: upload form served by the CLU itself (same origin as POST /ota,
 * so it works from any browser without CORS or mixed-content issues). The running firmware
 * version goes between OTA_PAGE_HEAD and OTA_PAGE_TAIL.
 *
 * The page first sends an empty POST /ota to check the password (401 = wrong, 411 = right), so a
 * wrong password is reported before the image is uploaded.
 */
#pragma once

static const char OTA_PAGE_HEAD[] =
    "<!doctype html><html lang=\"en\"><head><meta charset=\"utf-8\">"
    "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
    "<title>CLU firmware update</title><style>"
    ":root{color-scheme:light dark;--fg:#1d1d1f;--bg:#fff;--mute:#666;--bad:#b3261e;--ok:#1b6e3c}"
    "@media(prefers-color-scheme:dark){:root{--fg:#e8e8e8;--bg:#141414;--mute:#999;--bad:#f2867e;--ok:#7fd49c}}"
    "body{font:15px/1.5 system-ui,sans-serif;max-width:32rem;margin:2rem auto;padding:0 16px;color:var(--fg);"
    "background:var(--bg)}h1{font-size:1.4rem}small,.mute{color:var(--mute)}"
    "label{display:block;margin:1rem 0}input{font:inherit;width:100%;box-sizing:border-box;margin-top:.25rem}"
    "button{font:inherit;padding:.4rem 1.4rem}progress{width:100%;margin-top:1rem}"
    "#msg{white-space:pre-wrap}.bad{color:var(--bad)}.ok{color:var(--ok)}</style></head><body>"
    "<h1>CLU firmware update</h1><p class=\"mute\">Running: <b>";

static const char OTA_PAGE_TAIL[] =
    "</b></p><form id=\"f\">"
    "<label>Application image <small>(opengr8on-clu-&lt;board&gt;-&lt;version&gt;-ota.bin from the release)</small>"
    "<input type=\"file\" id=\"img\" accept=\".bin\" required></label>"
    "<label>OTA password<input type=\"password\" id=\"pw\" required minlength=\"8\" maxlength=\"64\" "
    "autocomplete=\"current-password\"></label><button id=\"go\">Update</button></form>"
    "<progress id=\"p\" value=\"0\" max=\"1\" hidden></progress><p id=\"msg\" aria-live=\"polite\"></p>"
    "<script>"
    "const $=i=>document.getElementById(i);"
    "const say=(t,c)=>{$('msg').textContent=t;$('msg').className=c||'';};"
    "const post=(auth,body,prog)=>new Promise(res=>{const x=new XMLHttpRequest();x.open('POST','/ota');"
    "x.setRequestHeader('Authorization',auth);x.setRequestHeader('Content-Type','application/octet-stream');"
    "if(prog)x.upload.onprogress=e=>{if(e.lengthComputable)prog(e.loaded/e.total);};"
    "x.onload=()=>res({s:x.status,t:x.responseText});x.onerror=()=>res({s:0,t:''});x.send(body);});"
    "$('f').onsubmit=async e=>{e.preventDefault();const file=$('img').files[0];if(!file)return;"
    "const pt=new Uint8Array(await file.slice(0x8000,0x8022).arrayBuffer());"
    "if(pt[0]===0xaa&&pt[1]===0x50&&pt[32]===0xaa&&pt[33]===0x50){say(file.name+' is the full flash image (USB install): pick the -ota.bin image.','bad');return;}"
    "const auth='Basic '+btoa(String.fromCharCode(...new TextEncoder().encode('ota:'+$('pw').value)));"
    "$('go').disabled=true;say('Checking the password\\u2026');"
    "let r=await post(auth,null);"
    "if(r.s===401){say('Wrong password.','bad');$('go').disabled=false;return;}"
    "$('p').hidden=false;$('p').value=0;say('Uploading '+file.name+'\\u2026');"
    "r=await post(auth,file,v=>$('p').value=v);"
    "if(r.s===200){say('Written. The CLU is restarting: reload this page in ~20 s to see the new version.','ok');return;}"
    "say(r.s?r.s+': '+r.t:'Connection lost (wrong file or image too large?).','bad');$('go').disabled=false;};"
    "</script></body></html>";

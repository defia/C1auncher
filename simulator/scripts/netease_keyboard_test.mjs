#!/usr/bin/env node
// Compare actual browser inputs with the app's native symbol chord in the original
// Netease login field. Both pass through QEMU and the unmodified app binary.
import fs from "node:fs";
import vm from "node:vm";
import assert from "node:assert/strict";
import path from "node:path";
import {fileURLToPath} from "node:url";
import {execFileSync} from "node:child_process";
import {createHash} from "node:crypto";
const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..");
const url = process.argv[2];
assert.ok(url, "需要独立测试模拟器 URL");
const sleep = ms => new Promise(resolve => setTimeout(resolve, ms));
const get = async path => (await fetch(url + path)).json();
const state = await get("/api/state");
assert.equal(state.app, "launcher", "必须由原版启动器打开网易云，覆盖子应用路径");
const post = async (path, data) => {
  const response = await fetch(url + path, {method:"POST", headers:{"Content-Type":"application/json", "X-C1Sim-Token":state.token}, body:JSON.stringify(data)});
  assert.ok(response.ok, await response.text());
};
const stroke = async code => {
  await post("/api/key", {code, value:1}); await sleep(40);
  await post("/api/key", {code, value:0}); await sleep(40);
};
const field = async () => {
  await sleep(200);
  const pixels = Buffer.from((await get("/api/frame")).pixels, "base64");
  // Only compare the login input row, excluding the changing clock/status.
  const field = Buffer.from(Array.from({length:296 * 18}, (_, i) => {
    const x = i % 296, y = 32 + Math.floor(i / 296);
    return (pixels[Math.floor(y / 8) * 296 + x] >> (7 - y % 8)) & 1;
  }));
  return createHash("sha256").update(field).digest("hex");
};
const clear = async () => { for(let i=0; i<12; i++) await stroke(111); };
const empty = await field();
await post("/api/key", {code:115, value:1}); await sleep(80);
for(let code=16; code<=25; code++) await stroke(code);
await post("/api/key", {code:115, value:0}); await sleep(80);
const expected = await field();
// Golden login row, visually verified as literal 1234567890 in official 0.2.9.
assert.equal(expected, "945c2d71d7290f7339705316e99bb1a3f6c723c1264f73b3476ed8e1c1f831f3",
             "原版组合必须在手机号框绘制 1234567890，不能以字母结果作为参考");
execFileSync("python3", [path.join(root,"scripts/capture_frame.py"), path.join(root,"build/verification/netease-keyboard-native.png"), "--url", url]);
assert.notEqual(expected, empty, "设备数字组合未进入网易云手机号框");
await clear();
assert.deepEqual(await field(), empty, "无法清空测试输入框");
const elements = new Map(), allElements = [], listeners = {};
class Element {
  constructor(tag="DIV") { this.tagName=tag.toUpperCase(); this.children=[]; this.dataset={}; this.classList={add(){},remove(){},toggle(){}}; this.listeners={}; allElements.push(this); }
  append(...children){this.children.push(...children);}
  prepend(...children){this.children.unshift(...children);}
  setAttribute(){}
  addEventListener(type, listener){this.listeners[type]=listener;}
  setPointerCapture(){}
  getContext(){return {};}
}
const document = {
  querySelector(selector){if(!elements.has(selector))elements.set(selector,new Element()); return elements.get(selector);},
  querySelectorAll(selector){return allElements.filter(element=>selector.startsWith("[data-key") ? element.dataset.key!==undefined : selector==="[data-app]" ? element.dataset.app!==undefined : false);},
  createElement(tag){return new Element(tag);},
  addEventListener(type, listener){listeners[type]=listener;}
};
const context = vm.createContext({document, window:{addEventListener(){}}, EventSource:class{addEventListener(){}},
  fetch:(endpoint, options)=>fetch(url+endpoint, options), setTimeout, clearTimeout, atob, Uint8Array, console});
vm.runInContext(fs.readFileSync(path.join(root,"public/app.js"),"utf8"),context);
vm.runInContext("state(" + JSON.stringify(await get("/api/state")) + ")",context);
const flush = async () => { await vm.runInContext("commands",context); await sleep(40); };
for(const digit of "1234567890") {
  for(const type of ["keydown","keyup"]) {
    listeners[type]({code:"Digit"+digit, key:digit, repeat:false, metaKey:false, ctrlKey:false,
                     target:{tagName:"DIV"}, preventDefault(){}});
    await flush();
  }
}
assert.deepEqual(await field(), expected, "网页 1–9 输入成了 Q–O，未产生原生数字输入的屏幕结果");
await clear();
for(const digit of "1234567890") {
  const button = allElements.find(element=>element.dataset.character===digit);
  assert.ok(button);
  button.listeners.pointerdown({pointerId:1, preventDefault(){}}); await flush();
  button.listeners.pointerup(); await flush();
}
assert.deepEqual(await field(), expected, "屏幕数字按钮未产生原生数字输入的屏幕结果");
console.log("通过：由原版启动器打开网易云，电脑 1–9/0 与屏幕数字按钮均输入真实数字。");

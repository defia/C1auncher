#!/usr/bin/env node
// Run the real browser keyboard handlers against QEMU and read the resulting
// bytes from the original launcher's PTY. No app binary or character decoder is replaced.
import fs from "node:fs";
import vm from "node:vm";
import assert from "node:assert/strict";
import {execFileSync} from "node:child_process";
import path from "node:path";
import {fileURLToPath} from "node:url";
const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..");
const url = process.argv[2] || "http://127.0.0.1:8765";
const shell = command => execFileSync("python3", [path.join(root, "scripts/console.py"), "--url", url, command], {encoding:"utf8"});
const state = await (await fetch(url + "/api/state")).json();
assert.equal(state.app, "launcher", "请先打开 C1ancher 的 TERMINAL 页面");
const processList = shell("ps");
const pid = processList.match(/^\s*(\d+)\s+root\s+\d+\s+\S+\s+sh -i\s*$/m)?.[1];
assert.ok(pid, "请先打开 C1ancher 的 TERMINAL 页面");
const tty = shell("readlink /proc/" + pid + "/fd/0").match(/^\/dev\/pts\/\d+$/m)?.[0];
assert.ok(tty, "没有找到原版终端 PTY");
const allElements = [];
class Element {
  constructor(tag="DIV") { this.tagName=tag.toUpperCase(); this.children=[]; this.dataset={}; this.classList={add(){},remove(){},toggle(){}}; this.textContent=""; this.listeners={}; allElements.push(this); }
  append(...children){this.children.push(...children);}
  prepend(...children){this.children.unshift(...children);}
  setAttribute(){}
  addEventListener(type,listener){this.listeners[type]=listener;}
  setPointerCapture(){}
  getContext(){return {};}
}
const elements = new Map();
const listeners = {};
const document = {
  querySelector(selector){if(!elements.has(selector))elements.set(selector,new Element()); return elements.get(selector);},
  querySelectorAll(selector){return allElements.filter(element=>selector.startsWith("[data-key") ? element.dataset.key !== undefined : selector==="[data-app]" ? element.dataset.app !== undefined : false);},
  createElement(tag){return new Element(tag);},
  addEventListener(type,listener){listeners[type]=listener;}
};
const moreSymbols = new Element("button");
moreSymbols.dataset.key = "143";
moreSymbols.dataset.shift = "true";
const context = vm.createContext({
  document, window:{addEventListener(){}}, EventSource:class{addEventListener(){}},
  fetch:(endpoint, options)=>fetch(url + endpoint,options), setTimeout, clearTimeout,
  atob, Uint8Array, console
});
vm.runInContext(fs.readFileSync(path.join(root,"public/app.js"),"utf8"),context);
vm.runInContext("state(" + JSON.stringify(state) + ")", context);
const send = async (code,key="",type="keydown") => {
  listeners[type]({code,key,repeat:false,metaKey:false,ctrlKey:false,shiftKey:false,
                   target:{tagName:"DIV"},preventDefault(){}});
  await vm.runInContext("commands",context);
  await new Promise(resolve=>setTimeout(resolve,25));
};
shell("kill -STOP " + pid + "; rm -f /tmp/c1sim-key-input; head -n 1 " + tty + " > /tmp/c1sim-key-input & true");
try {
  for(const digit of "1234567890") { await send("Digit"+digit,digit); await send("Digit"+digit,digit,"keyup"); }
  const character = async (code,key) => { await send(code,key); await send(code,key,"keyup"); };
  await character("KeyA","a");
  await character("ShiftLeft","Shift");
  await character("KeyA","a");
  await character("ShiftLeft","Shift");
  await character("KeyA","a");
  await character("ShiftLeft","Shift");
  await character("KeyA","a");
  for(const [code,key] of [["Period","."],["Slash","/"],["Minus","-"],["Slash","?"],["Semicolon",":"],["Digit2","@"]]) await character(code,key);
  for(const value of ["2","/","'"]) {
    const button = allElements.find(element=>element.dataset.character===value);
    assert.ok(button, "缺少字符按钮 " + value);
    button.listeners.pointerdown({pointerId:1,preventDefault(){}});
    await vm.runInContext("commands",context);
    await new Promise(resolve=>setTimeout(resolve,25));
    button.listeners.pointerup();
    await vm.runInContext("commands",context);
  }
  // A desktop digit must not leave synthetic Shift held while another key is
  // pressed, even when the physical digit has not yet been released.
  await send("Digit1","1");
  await character("KeyB","b");
  await send("Digit1","1","keyup");
  moreSymbols.listeners.pointerdown({pointerId:1,preventDefault(){}});
  await vm.runInContext("commands",context);
  moreSymbols.listeners.pointerup();
  await vm.runInContext("commands",context);
  await character("Enter","Enter"); // Select the original symbol picker's first entry: !
  await send("Enter","Enter"); await send("Enter","Enter","keyup");
  await new Promise(resolve=>setTimeout(resolve,150));
  const actual = shell("echo C1_INPUT_BEGIN; cat /tmp/c1sim-key-input; echo C1_INPUT_END").match(/\nC1_INPUT_BEGIN\n([^\n]*)\nC1_INPUT_END/)?.[1];
  assert.equal(actual, "1234567890aA'a./-?:@2/'1b!");
  console.log("通过：电脑数字/符号、Shift 大小写切换、屏幕字符按钮、更多符号、重叠按键 → 原版终端实际输入 " + actual);
} finally {
  shell("kill -CONT " + pid);
}

#!/usr/bin/env node
// smoke_routing_judge.js —— W4 Q6: 双模路由判定 localInfeasibility() 的宿主机单测。
//
// 判定逻辑的真身在 custom_nodes/ohos_external_api/web/api_keys.js(前端扩展, 依赖 LiteGraph
// 的 app.graph)。本脚本**从源码抽出该函数执行**, 不复制一份逻辑 —— 复制出来的"验证副本"
// 会与真身漂移(本项目已踩过: 文档声称的强制比对实际不存在)。抽不出/函数被改名即 FAIL。
//
// 用法: node scripts/smoke_routing_judge.js   (退出码 0 = 全绿)
const fs = require("fs");
const path = require("path");

const SRC = path.join(
  __dirname,
  "../externals/comfyui-src/custom_nodes/ohos_external_api/web/api_keys.js",
);

let fails = 0;
function check(name, cond, detail) {
  console.log(`  [${cond ? "OK " : "FAIL"}] ${name}` + (cond && detail ? ` — ${detail}` : ""));
  if (!cond) fails++;
}

// ── 抽函数 ───────────────────────────────────────────────────────────────────
let src = fs.readFileSync(SRC, "utf8").replace(/^import .*$/gm, ""); // ESM import 去掉(不以文件形式加载)
check("源码含 localInfeasibility", /function localInfeasibility\s*\(/.test(src));

const fakeApp = { graph: { _nodes: [] }, registerExtension: () => {} };
const fakeApi = { fetchApi: async () => ({ json: async () => ({}) }) };
let judge;
try {
  judge = new Function("app", "api", src + "\nreturn localInfeasibility;")(fakeApp, fakeApi);
  check("抽出可执行", typeof judge === "function");
} catch (e) {
  check("抽出可执行", false, e.message);
}
if (typeof judge !== "function") {
  console.log(`\nFAILED: 无法抽取判定函数`);
  process.exit(1);
}

// ── 造图(LiteGraph 节点形态子集) ─────────────────────────────────────────────
function latent(w, h) {
  return {
    type: "EmptyLatentImage",
    widgets: [{ name: "width", value: w }, { name: "height", value: h }],
    constructor: class {},
  };
}
function ckpt(value, options) {
  class FakeNode {
    static nodeData = { input: { required: { ckpt_name: [options] } } };
  }
  return {
    type: "CheckpointLoaderSimple",
    widgets: [{ name: "ckpt_name", value }],
    constructor: FakeNode,
  };
}
const run = (nodes) => {
  fakeApp.graph._nodes = nodes;
  return judge();
};

console.log("== [Q6] 本地跑不动判定 ==");
const r1 = run([latent(512, 512), ckpt("sd_turbo.safetensors", ["sd_turbo.safetensors"])]);
check("512×512 判为跑不动", r1.some((x) => x.includes("512") && x.includes("256")), JSON.stringify(r1));

const r2 = run([latent(256, 256), ckpt("flux1-dev.safetensors", ["sd_turbo.safetensors"])]);
check("模型不在本地库判为跑不动", r2.some((x) => x.includes("不在本机模型库")), JSON.stringify(r2));

const r3 = run([latent(256, 256), ckpt("sd_turbo.safetensors", ["sd_turbo.safetensors"])]);
check("256 + 本地模型判为可跑(空 reasons)", Array.isArray(r3) && r3.length === 0, JSON.stringify(r3));

const r4 = run([latent(512, 256)]);
check("单边超限也判跑不动(高度 256 合规)", r4.length === 1 && r4[0].includes("512×256"), JSON.stringify(r4));

console.log();
if (fails) {
  console.log(`FAILED: ${fails} 项`);
  process.exit(1);
}
console.log("ALL GREEN");

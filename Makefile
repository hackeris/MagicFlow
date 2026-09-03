# ComfyUI on HarmonyOS —— 主线构建（复现链见 README「复现链」）。
# 组织方式沿用原 wineohos/参考（多目标 + scripts/env.sh 提供环境）。
# 主线：all → hap ← check + zip + prebuilt ← extract ← fetch
# 铁律：一切 .so 只经 HAP libs/<abi>/；禁止 patchelf；仅 arm64。
SHELL := /bin/bash
NATIVE_ARCH ?= arm64-v8a
export NATIVE_ARCH

TOOL_HOME ?= /apps/harmony
OHOS_SDK ?= $(TOOL_HOME)/sdk/default/openharmony
EXT_DIR ?= $(CURDIR)/externals
BUNDLE = app.hackeris.hium
# aarch64 设备要求签名 → 用 signed.hap（debug 签名材料 .ohos/, gitignored,见 README「签名材料」）
HAP = entry/build/default/outputs/default/entry-default-signed.hap

.PHONY: all hap check fetch extract stage zip rust prebuilt install deploy verify-device log clean clean-all help

all: hap

# ── 前置检查：SDK / 签名材料（构建 HAP 的硬前提）──
check:
	@[ -x "$(OHOS_SDK)/toolchains/hdc" ] || { echo "[CHECK] 缺 OHOS SDK @ $(OHOS_SDK)（TOOL_HOME=/apps/harmony?）"; exit 1; }
	@ls .ohos/*.p12 >/dev/null 2>&1 || { echo "[CHECK] 缺签名材料 .ohos/*.p12（DevEco 生成,见 README）"; exit 1; }

# ── ① 外部输入（下载+sha 校验,幂等; 断网可用 --offline 复用本地副本）──
fetch:
	bash scripts/fetch_externals.sh

# ── ② 解包 skh 栈（唯一数据源契约：后续 stage/zip/prebuilt/CMakeLists 全部只读 build/skh-run-extract）──
extract: fetch
	@if [ ! -d build/skh-run-extract/skh-run/usr/lib/python3.12 ]; then \
		mkdir -p build/skh-run-extract; \
		tar -xzf $(EXT_DIR)/skh-run.tar.gz -C build/skh-run-extract; \
		echo "[extract] skh-run.tar.gz → build/skh-run-extract/"; \
	else echo "[extract] 已存在"; fi

# ── ③ comfyui 源码 → staging（校验：官方 input 包/前端锚/0 个 .so）──
stage: extract
	python3 scripts/make_comfyui_stage.py

# ── ④ rust 扩展重编译（thirdparty/ submodule 源码; 产物 sha 与 prebuilt 锚比对）──
rust:
	bash scripts/build_rust_exts.sh

# ── ⑤ python312.zip（stage + skh stdlib → rawfile; manifest 锚）──
zip: extract stage
	python3 scripts/make_py312_zip.py

# ── ⑥ prebuilt .so 收集（清单驱动; 计数/NEEDED 闭包/死文件断言, 违规即退出）──
prebuilt: extract rust
	bash scripts/collect_prebuilt.sh

# ── ⑦ HAP ──
hap: check zip prebuilt
	source scripts/env.sh && hvigorw assembleHap --mode module -p product=default -p buildMode=debug --no-daemon

# ── ⑧ 部署真机（bundle/HDC 来自 env.sh; HDC_TARGET 可覆盖）──
install: hap
	source scripts/env.sh && $$HDC shell "aa force-stop $(BUNDLE)" || true
	source scripts/env.sh && $$HDC file send $(HAP) /data/local/tmp/comfy.hap
	source scripts/env.sh && $$HDC shell "bm install -p /data/local/tmp/comfy.hap"
	source scripts/env.sh && $$HDC shell "aa start -a EntryAbility -b $(BUNDLE)"

deploy: install

# ── ⑨ 真机验证一轮（run_and_capture.sh：采集→部署→信号轮询→摘要）──
verify-device: install
	bash scripts/run_and_capture.sh

# 抓子进程/父进程日志
log:
	source scripts/env.sh && $$HDC hilog | grep -E "ComfyChild|ComfyNapi|ComfyUI|CRASH|SIGSEGV|ComfyP"

# ── 清理（stub/ 与 externals/ 是源码/外部输入, clean 不碰它们; clean-all 连外部输入一起删）──
clean:
	rm -rf build entry/build entry/.cxx entry/src/main/cpp/prebuilt entry/src/main/resources/rawfile/python312.zip

clean-all: clean
	rm -rf externals

help:
	@echo "主线: all(=hap) | hap | install(部署真机) | verify-device(真机一轮验证)"
	@echo "中间: fetch(外部输入) | extract(skh 解包) | stage | zip | rust | prebuilt"
	@echo "辅助: check | log | clean | clean-all"

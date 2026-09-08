// ImageSave.ets — 图片"下载"的 ArkWeb 桥实现(2026-09-08)。
// 前端下载按钮 → window.__ohosImageSave.pickAndSave(url, filename) →
// 此对象: ① DocumentViewPicker 弹"另存为"(用户选目标位置/文件名)
//         ② NetworkKit 下载该 URL(图片在后端 127.0.0.1:8188, 同机可直连)
//         ③ 写入用户选择的位置(同 uid 有写权限; 路径经 FileUri 归一)
//         ④ 返回 JSON 字符串 {ok, path?, msg?}(关闭对话框/取消 → ok=false, msg=cancelled)
// 注册: Index.ets (registerJavaScriptProxy(client, '__ohosImageSave', ['pickAndSave'], ['pickAndSave']))
import { fileIo, fileUri, picker } from '@kit.CoreFileKit';
import { http } from '@kit.NetworkKit';
import { hilog } from '@kit.PerformanceAnalysisKit';
import { common } from '@kit.AbilityKit';

const DOMAIN = 0xD001;
const TAG = 'ImageSave';

function normUri(uri: string): string {
  try {
    if (uri.startsWith('file://')) {
      const p = new fileUri.FileUri(uri).path;
      return p.startsWith('file://') ? p.substring(7) : p;
    }
  } catch (_) { /* 原样返回 */ }
  return uri;
}

export class ImageSaveClient {
  private ctx: common.Context;

  constructor(ctx: common.Context) {
    this.ctx = ctx;
  }

  async pickAndSave(url: string, filename: string): Promise<string> {
    // 0) url 归一: 前端(同源构建)传来的是相对路径 /view?... → http 层要绝对 URL;
    //    后端实现契约: 监听本机 127.0.0.1:8188(与 WebView origin 一致)
    if (!/^https?:\/\//i.test(url)) {
      url = 'http://127.0.0.1:8188' + (url.startsWith('/') ? '' : '/') + url;
    }
    // 1) 弹"另存为"(预填文件名/可选类型; 用户可改)。取消 → res 为空
    let picked: string[] = [];
    try {
      const docPicker = new picker.DocumentViewPicker(this.ctx);
      const opts = new picker.DocumentSaveOptions();
      opts.newFileNames = [filename || 'download.png'];
      opts.fileSuffixChoices = ['PNG|png', 'JPG|jpg', 'JPEG|jpeg', 'WEBP|webp'];
      picked = await docPicker.save(opts);
    } catch (e) {
      hilog.warn(DOMAIN, TAG, 'picker save failed: %{public}s', JSON.stringify(e));
      return JSON.stringify({ ok: false, msg: 'picker-failed' });
    }
    if (!picked || picked.length === 0) {
      return JSON.stringify({ ok: false, msg: 'cancelled' });
    }
    const target = picked[0];

    // 2) 下载图片数据
    const httpRequest = http.createHttp();
    let data: ArrayBuffer;
    try {
      const resp = await httpRequest.request(url, {
        method: http.RequestMethod.GET,
        expectDataType: http.HttpDataType.ARRAY_BUFFER,
        connectTimeout: 30000,
        readTimeout: 120000
      });
      if (resp.responseCode !== 200) {
        return JSON.stringify({ ok: false, msg: `HTTP ${resp.responseCode}` });
      }
      data = resp.result as ArrayBuffer;
    } catch (e) {
      hilog.error(DOMAIN, TAG, 'image download failed: %{public}s', JSON.stringify(e));
      return JSON.stringify({ ok: false, msg: 'download-failed' });
    } finally {
      httpRequest.destroy();
    }

    // 3) 写入用户选择的位置(覆盖式: CREATE|TRUNC)。
    //    目标优先原样透传(picker 授权 URI file://docs/... 由 FileKit 直开, 归一化反出错);
    //    normUri 只作 fallback: 个别路径前 3 次 open 失败再降级。
    let fd: fileIo.File;
    try {
      fd = fileIo.openSync(target,
        fileIo.OpenMode.CREATE | fileIo.OpenMode.TRUNC | fileIo.OpenMode.READ_WRITE);
    } catch (e1) {
      fd = fileIo.openSync(normUri(target),
        fileIo.OpenMode.CREATE | fileIo.OpenMode.TRUNC | fileIo.OpenMode.READ_WRITE);
    }
    try {
      fileIo.writeSync(fd.fd, data);
      fileIo.closeSync(fd);
    } catch (e) {
      hilog.error(DOMAIN, TAG, 'image write failed: %{public}s', JSON.stringify(e));
      return JSON.stringify({ ok: false, msg: 'write-failed' });
    }
    hilog.info(DOMAIN, TAG, 'image saved: %{public}s', target);
    return JSON.stringify({ ok: true, path: target });
  }
}

// 系统输入法会话（搜索关键词输入用）
//
// 移植自 deepseek-x5/src/services/im.js 的 textEdit 部分（该实现在 youdao-x5-fw3.4.6
// 真机验证过：能可靠拉起系统输入法、支持中文、多形态回调兼容）：
//   import globalModule from 'global'         ← 运行时原生模块（构建器会提示"原生模块请忽略"）
//   new globalModule.Global()
//   startTextEdit(JSON字符串{ text, placeholder, maxlength, autofocus }) → uuid（同步）
//   完成回调双通道：module.textEditFinished.on / $falcon.on('textEditFinished')
//   真机实测第二参数形如 {"text":"…","cursorIndex":N,"editConfirmed":true}
// 契约：open(options) → Promise<{confirmed, text, error?}>；uuid 不匹配的回调忽略；
//       90s 看门狗防永久等待；dispose() 成对清理（页面 onUnload 调用）。
//
// 日志卫生：只记录文本长度，不记录输入内容。
import globalModule from 'global';

function inLog(msg) {
  try {
    console.warn('[input] ' + msg);
  } catch (e) {
    /* 忽略 */
  }
}

function isIdLike(s) {
  return typeof s === 'string' && (/^[0-9a-f]{32}$/i.test(s) || /^[0-9a-f-]{36}$/i.test(s));
}

// 兼容 {value,text}/字符串/对象包裹的取值形态
export function normalizeTextEditValue(v) {
  if (typeof v === 'string') return v;
  if (v && typeof v === 'object') {
    if (typeof v.value === 'string') return v.value;
    if (typeof v.text === 'string') return v.text;
  }
  return '';
}

export class InputSession {
  constructor() {
    this.manager = null;
    this.uuid = '';
    this.moduleHandler = null;
    this.busHandler = null;
    this.busToken = 0;
    this.pending = null;
    this.watchdog = 0;
  }

  _ensureManager() {
    if (!this.manager) {
      this.manager = new globalModule.Global();
      inLog('Global 就绪');
    }
    return this.manager;
  }

  _resolvePending(result) {
    const p = this.pending;
    this.pending = null;
    if (this.watchdog) {
      clearTimeout(this.watchdog);
      this.watchdog = 0;
    }
    if (p) p.resolve(result);
  }

  _subscribe(m) {
    if (!this.moduleHandler) {
      const self = this;
      this.moduleHandler = function () {
        self._onFinished(Array.prototype.slice.call(arguments), 'module');
      };
      try {
        m.textEditFinished.on(this.moduleHandler);
        inLog('模块信号订阅成功');
      } catch (e) {
        this.moduleHandler = null;
        inLog('模块信号订阅失败: ' + (e && e.message));
      }
    }
    if (!this.busHandler) {
      const self = this;
      this.busHandler = function () {
        self._onFinished(Array.prototype.slice.call(arguments), 'bus');
      };
      try {
        this.busToken = $falcon.on('textEditFinished', this.busHandler);
        inLog('总线订阅成功');
      } catch (e) {
        this.busHandler = null;
        inLog('总线订阅失败: ' + (e && e.message));
      }
    }
  }

  // 只处理自己发起的会话；去重 + 多形态参数吸收
  _onFinished(args, source) {
    if (!this.pending) return;
    const items = Array.isArray(args) ? args : [args];
    const acc = { uuid: '', confirmed: false, canceled: false, text: '' };
    for (let i = 0; i < items.length; i++) this._absorb(items[i], acc, 0);
    inLog(
      'textEditFinished(' + source + ') args=' + items.length +
        ' uuid=' + String(acc.uuid).slice(0, 8) +
        ' confirmed=' + acc.confirmed + ' canceled=' + acc.canceled + ' textLen=' + acc.text.length
    );
    if (this.uuid && acc.uuid && String(acc.uuid) !== String(this.uuid)) {
      inLog('忽略非当前会话回调');
      return;
    }
    let text = acc.text;
    if (!text) {
      const probed = this._probeText(acc.uuid || this.uuid);
      if (probed) {
        text = probed;
        inLog('取值探测 len=' + probed.length);
      }
    }
    if (!acc.confirmed && !acc.canceled && text) acc.confirmed = true;
    const cur = this.uuid;
    this.uuid = '';
    // 先返回结果再延迟关闭（信号回调栈内直接 closeTextEdit 有重入风险）
    this._resolvePending({ confirmed: acc.confirmed && !acc.canceled, text: acc.confirmed && !acc.canceled ? text : '' });
    this._deferClose(acc.uuid || cur);
  }

  _absorb(item, acc, depth) {
    if (depth > 3 || item === undefined || item === null) return;
    if (typeof item === 'string') {
      const s = item.trim();
      if (!s) return;
      if (isIdLike(s)) {
        if (!acc.uuid) acc.uuid = s;
        return;
      }
      const head = s.charAt(0);
      if (head === '{' || head === '[') {
        try {
          this._absorb(JSON.parse(s), acc, depth + 1);
          return;
        } catch (e) {
          /* 普通文本 */
        }
      }
      if (s.length > acc.text.length) acc.text = s;
      return;
    }
    if (typeof item === 'boolean') {
      if (item) acc.confirmed = true;
      return;
    }
    if (typeof item === 'number') return;
    if (typeof item === 'object') {
      if (item.editConfirmed === true || item.confirmed === true) acc.confirmed = true;
      if (item.editCanceled === true || item.canceled === true) acc.canceled = true;
      const u = item.uuid || item.calledTaskUuid || item.taskUuid;
      if (typeof u === 'string' && u) acc.uuid = u;
      const t = normalizeTextEditValue(
        item.editText !== undefined
          ? item.editText
          : item.text !== undefined
            ? item.text
            : item.value !== undefined
              ? item.value
              : item.contents !== undefined
                ? item.contents
                : item.content
      );
      if (t && !isIdLike(t) && t.length >= acc.text.length) acc.text = t;
      if (!t && typeof item.data === 'string') this._absorb(item.data, acc, depth + 1);
    }
  }

  // 探测模块上可能的文本取值接口（部分固件回调不带文本）
  _probeText(uuid) {
    const m = this.manager;
    if (!m) return '';
    const names = ['getTextEditValue', 'getTextEditContent', 'getTextEditText', 'getCurrentTextEditValue', 'getEditText', 'getContent', 'getText', 'getValue'];
    let hit = '';
    for (let i = 0; i < names.length; i++) {
      let fn = null;
      try {
        fn = m[names[i]];
      } catch (e) {
        fn = null;
      }
      if (typeof fn !== 'function') continue;
      let v = null;
      try {
        v = uuid ? fn.call(m, uuid) : fn.call(m);
      } catch (e1) {
        try {
          v = fn.call(m);
        } catch (e2) {
          v = null;
        }
      }
      if (!hit && typeof v === 'string' && v && !isIdLike(v)) hit = v;
    }
    return hit;
  }

  _deferClose(uuid) {
    if (!uuid) return;
    const self = this;
    setTimeout(function () {
      if (!self.manager) return;
      try {
        self.manager.closeTextEdit(uuid);
      } catch (e) {
        /* 忽略 */
      }
    }, 60);
  }

  // options: { value, placeholder, maxlength } → Promise<{confirmed, text, error?}>
  open(options) {
    const opts = options || {};
    const self = this;
    return new Promise((resolve) => {
      let m;
      try {
        m = this._ensureManager();
      } catch (e) {
        inLog('Global 创建失败: ' + (e && e.message));
        resolve({ confirmed: false, text: '', error: 'IM_UNAVAILABLE' });
        return;
      }
      // 关旧会话（resolve 掉挂起的）
      if (this.uuid && this.manager) {
        try {
          this.manager.closeTextEdit(this.uuid);
        } catch (e2) {
          /* 忽略 */
        }
        this.uuid = '';
        this._resolvePending({ confirmed: false, text: '' });
      }
      this._subscribe(m);
      const config = JSON.stringify({
        text: typeof opts.value === 'string' ? opts.value : '',
        placeholder: opts.placeholder || '请输入',
        maxlength: opts.maxlength || 200,
        autofocus: true
      });
      let uuid = '';
      try {
        uuid = m.startTextEdit(config);
      } catch (e) {
        inLog('startTextEdit 抛错: ' + (e && e.message));
        resolve({ confirmed: false, text: '', error: 'IM_START_FAIL' });
        return;
      }
      if (uuid && typeof uuid === 'object') uuid = uuid.uuid || uuid.value || '';
      this.uuid = typeof uuid === 'string' ? uuid : '';
      this.pending = { resolve: resolve };
      inLog('startTextEdit uuid=' + (this.uuid ? this.uuid.slice(0, 8) : '(空)') + ' textLen=' + String(opts.value || '').length);
      // 看门狗：面板被关且无任何回调时不让调用方永久等待
      this.watchdog = setTimeout(function () {
        if (self.pending) {
          inLog('输入法等待超时(90s)，自动结束');
          if (self.pending) {
            const u = self.uuid;
            self.uuid = '';
            self._resolvePending({ confirmed: false, text: '' });
            if (u) self._deferClose(u);
          }
        }
      }, 90000);
    });
  }

  cancelPending(reason, closePanel) {
    if (!this.pending) return false;
    const uuid = this.uuid;
    this.uuid = '';
    inLog('取消等待(' + (reason || 'closed') + ')');
    this._resolvePending({ confirmed: false, text: '', canceled: true });
    if (closePanel) this._deferClose(uuid);
    return true;
  }

  // 页面销毁：退订 + 结束等待（默认不关面板——可能正在打字中途）
  dispose() {
    if (this.manager && this.moduleHandler) {
      try {
        this.manager.textEditFinished.off(this.moduleHandler);
      } catch (e) {
        /* 忽略 */
      }
      this.moduleHandler = null;
    }
    if (this.busHandler) {
      try {
        if (this.busToken) $falcon.off('textEditFinished', this.busToken);
        else $falcon.off('textEditFinished', this.busHandler);
      } catch (e) {
        /* 忽略 */
      }
      this.busHandler = null;
      this.busToken = 0;
    }
    this.cancelPending('dispose', false);
  }
}

// 页面级单例
export const input = new InputSession();

(() => {
    "use strict";
    function t(t) {
        t.onload = t.onerror = t.ontimeout = null
    }
    const isWPE = navigator.userAgent.indexOf("WPE") >= 0;
    class s {
        constructor(t, e, s, n, o)
        {
            this.id = t, this.src = e, this.onLoad = n, this.onError = o, this.canceled = !1, this.start(s)
        }
        start(s, n=0)
        {
            const o = this.xhr = new XMLHttpRequest;
            o.open("GET", this.src, !0)
            o.responseType = "blob"
            o.timeout = s[n] || 0
            o.onerror = () => {this.error(0, o.status), t(o)}
            o.onload = () => {
                setTimeout(() => {
                    this.onLoad(this.id, {})
                }, 10, o.response)
                t(o)
            }
            o.ontimeout = () => {t(o), o.abort(), n < s.length - 1 ? this.start(s, ++n) : this.error(1, o.status)}
            o.send()
        }
        cancel()
        {
            this.canceled || (this.xhr && this.xhr.abort(), this.canceled = !0)
        }
        error(t, e)
        {
            this.canceled || this.onError(this.id, {
                errorType: t,
                httpStatusCode: e
            })
        }
    }
    const n = new Map;
    let o;
    function i(t, e) {
        postMessage({
            id: t,
            info: e,
            type: 0
        }), n.delete(t)
    }
    function r(t, e) {
        postMessage({
            id: t,
            info: e,
            type: 1
        }), n.delete(t)
    }
    onmessage = function(t) {
        const e = t.data;
        switch (e.type) {
        case 0:
            o = e.timeouts;
            break;
        case 1:
            n.set(e.id, new s(e.id, e.src, o, i, r));
            break;
        case 2:
            const t = n.get(e.id);
            t && (t.cancel(), n.delete(e.id))
        }
    }
    postMessage({}) // start the loop
})();


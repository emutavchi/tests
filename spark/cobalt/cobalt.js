function deactivatePlugin(appInfo, callsign, thunderJS)
{
    appInfo.view.a = 0
    appInfo.listeners.forEach(l => l.dispose())
    appInfo.listeners = []
    return thunderJS.Controller.call('deactivate', {callsign})
}

function activatePlugin(appInfo, callsign, thunderJS)
{
    const controller = thunderJS.Controller
    return controller.call('configuration@' + callsign)
        .then(conf => {
            conf['display'] = appInfo.view.displayName
            return controller.call('configuration@' + callsign, conf)
        })
        .then(() => {
            return controller.call('activate', {callsign})
        })
        .then(() => {
            appInfo.view.a = 1
            const listener = thunderJS.on(callsign, 'pageclosure',
                event => deactivatePlugin(appInfo, callsign, thunderJS))
            appInfo.listeners.push(listener)
        }).catch(err => {
            console.error(err)
        })
}

px.import({
    scene: 'px:scene.1.js',
    keys: 'px:tools.keys.js',
    ws: 'ws',
    ThunderJS: './thunderJS.js'
}).then(function importsAreReady(imports) {

    let scene = imports.scene
    let ThunderJS = imports.ThunderJS
    let ws = imports.ws
    let keys = imports.keys

    let launchPromise = null
    let thunderJS = null

    const config = {
        host: '127.0.0.1',
        port: 9998,
        debug: true
    }
    thunderJS = new ThunderJS(ws, config)

    const appInfo = {
        view: null,
        listeners: []
    }

    appInfo.view = scene.create({
        t:"external",
        parent:scene.root,
        w:scene.w,
        h:scene.h,
        hasApi:false,
        interactive: true,
        focus: true
    })

    launchPromise = activatePlugin(appInfo, 'Cobalt.1', thunderJS)

    scene.root.on("onPreKeyDown", (e) => {
        var code  = e.keyCode
        if(code != keys.ONE)
            return

        launchPromise = launchPromise.then(() => {
            return deactivatePlugin(appInfo, 'Cobalt.1', thunderJS)
        }).then(() => {
            return activatePlugin(appInfo, 'Cobalt.1', thunderJS)
        })
    })

}).catch(function importFailed(err) {
    console.error('px.import failed', err)
})

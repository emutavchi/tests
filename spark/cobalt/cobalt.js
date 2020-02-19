function prepareAppView(scene, appInfo)
{
    return new Promise((resolve, reject) => {
        if (!appInfo.view) {
            appInfo.view = scene.create({
                t: "external",
                parent: scene.root,
                w: scene.w,
                h: scene.h,
                hasApi: false,
                interactitve: true,
                focus: true
            })
        }
        appInfo.view.ready
            .then(() => setTimeout(() => resolve()))
            .catch(err => reject(err))
    })
}

function deactivatePlugin(appInfo, thunderJS)
{
    const callsign = appInfo.callsign
    appInfo.listeners.forEach(l => l.dispose())
    appInfo.listeners = []
    return thunderJS.Controller.deactivate({callsign})
        .finally(() => {
            if (appInfo.view) {
                appInfo.view.remove()
                appInfo.view.dispose()
                appInfo.view = null
            }
        })
}

function activatePlugin(scene, appInfo, thunderJS)
{
    const callsign = appInfo.callsign
    const controller = thunderJS.Controller
    return controller['status@' + callsign]()
        .then(status => {
            if (status[0].state !== 'deactivated')
                throw ('cannot use ' + callsign + ', plugin status = ' + JSON.stringify(status))
        })
        .then(() => prepareAppView(scene, appInfo))
        .then(() => controller['configuration@' + callsign]())
        .then(conf => {
            const updateInfo = {url: appInfo.url, display: appInfo.view.displayName}
            return controller['configuration@' + callsign]({...conf, ...updateInfo})
        })
        .then(() => controller.activate({callsign}))
        .then(() => {
            appInfo.view.a = 1
            const listener = thunderJS.on(callsign, 'pageclosure', event => deactivatePlugin(appInfo, thunderJS))
            appInfo.listeners.push(listener)
        })
        .catch(err => console.error('got error =', JSON.stringify(err)))
}

px.import({
    scene: 'px:scene.1.js',
    keys: 'px:tools.keys.js',
    ws: 'ws',
    ThunderJS: './thunderJS.js'
}).then(function(imports) {
    let scene = imports.scene
    let ThunderJS = imports.ThunderJS
    let ws = imports.ws
    let keys = imports.keys

    const config = {
        host: '127.0.0.1',
        port: 9998,
        debug: true
    }
    const thunderJS = new ThunderJS(ws, config)

    const appInfo = {
        url: 'https://www.youtube.com/tv',
        callsign: 'Cobalt',
        view: null,
        listeners: []
    }
    let launchPromise = activatePlugin(scene, appInfo, thunderJS)

    scene.root.on("onPreKeyDown", (e) => {
        var code  = e.keyCode
        if(code != keys.ONE)
            return
        launchPromise = launchPromise.then(() => {
            return deactivatePlugin(appInfo, thunderJS)
        }).then(() => {
            return activatePlugin(scene, appInfo, thunderJS)
        })
    })

}).catch(function(err) {
    console.error('px.import failed', err)
})

module.exports.wantsClearscreen = function() { return false; }

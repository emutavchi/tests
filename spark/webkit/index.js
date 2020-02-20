function startTest(scene) {
    let browserServer = scene.create(
        {
            t:"external",
            parent:scene.root,
            server:"wl-rdkbrowser2-server",
            w:1280,
            h:720,
            hasApi: true,
            focus:  true,
            draw:   true,
        }
    )

    browserServer.remoteReady.then((obj) => {
        let browserAPI = browserServer.api.createWindow(browserServer.displayName, false)
        browserAPI.url = 'http://emutavchi.github.io/tests/text_rendering/index.html'
    })
}

px.import({
    scene: 'px:scene.1.js',
}).then(function(imports) {
    console.log('px.import ready')

    let scene = imports.scene
    startTest(scene)
}).catch(function(err) {
    console.error('px.import failed', err)
})

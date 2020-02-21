px.import({
    scene: 'px:scene.1.js',
}).then(function(imports) {
    console.log('px.import ready')

    let scene = imports.scene

    let background = scene.create({
        t:"rect",
        fillColor:0x000000ff,
        x:0,
        y:0,
        w:1280,
        h:720,
        a:0,
        parent:scene.root
    })

    let browser = scene.create({
        t:"external",
        parent:scene.root,
        server:"wl-rdkbrowser2-server",
        x:0,
        y:0,
        w:1280,
        h:720,
        hasApi: true,
        focus:  true,
        draw:   true,
    })

    browser.remoteReady.then((obj) => {
        let browserAPI = browser.api.createWindow(browser.displayName, false)
        browserAPI.url = 'http://emutavchi.github.io/tests/text_rendering/index.html'
    })

    scene.root.on("onPreKeyDown", (e) => background.a = background.a > 0 ? 0 : 1)

}).catch(function(err) {
    console.error('px.import failed', err)
})

module.exports.wantsClearscreen = function() { return false; }

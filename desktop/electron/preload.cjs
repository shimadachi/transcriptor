// The only bridge between the page and the app around it. Deliberately small:
// the page talks to the engine through /api/* like any page would, and needs
// this only for what a page cannot do -- the tray.
//
// CommonJS because a sandboxed preload cannot be an ES module.
const {contextBridge, ipcRenderer} = require('electron');

contextBridge.exposeInMainWorld('transcriptorShell', {
  // Whether there is a tray here at all, so the setting is not shown for
  // nothing on macOS.
  hasTray: process.argv.includes('--transcriptor-tray'),
  // The page's own take and the settings the tray follows.
  trayReport: report => ipcRenderer.invoke('tray:report', report),
  // Bring the window back from the tray.
  trayShow: () => ipcRenderer.invoke('tray:show'),
  // The tray's Start/Stop and Pause. They are pressed in the page, because
  // the source to record from is chosen there. Returns the way to stop
  // listening.
  onTrayAction: handler => {
    const listener = (_event, what) => handler(what);
    ipcRenderer.on('tray:action', listener);
    return () => ipcRenderer.removeListener('tray:action', listener);
  },
});

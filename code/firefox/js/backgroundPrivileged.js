const Cc = Components.classes;
const Ci = Components.interfaces;
const Cu = Components.utils;

Cu.import('resource://gre/modules/Services.jsm');
Cu.import('resource://ancho/modules/Require.jsm');
Cu.import('resource://ancho/modules/External.jsm');

var sandbox = Cu.Sandbox(window);
sandbox.console = {};
var baseURI = Services.io.newURI('resource://ancho/js/', '', null);
var require = Require.createRequireForWindow(sandbox, baseURI);

var ConsoleAPI = require('./console');
var console = new ConsoleAPI();
for (var prop in console) {
  if (console.hasOwnProperty(prop)) {
    sandbox.console[prop] = console[prop];
  }
}

var Global = require('./state').Global;
var loadHtml = require('./scripting').loadHtml;
var BrowserEvents = require('./browserEvents');
var Binder = require('./binder');

var BackgroundWindow = {
  _extension: null,
  _browserEvent: null,

  init: function(rootDirectory, firstRun) {
    var id;

    window.removeEventListener('load', arguments.callee, false);

    var xulWindow = window.QueryInterface(Ci.nsIInterfaceRequestor)
      .getInterface(Ci.nsIWebNavigation)
      .QueryInterface(Ci.nsIDocShellTreeItem)
      .treeOwner
      .QueryInterface(Ci.nsIInterfaceRequestor)
      .getInterface(Ci.nsIXULWindow);

    // Unregister our hidden window so it doesn't appear in the Window menu.
    Services.appShell.unregisterTopLevelWindow(xulWindow);

    // Tell Gecko that we closed the window already so it doesn't hold Firefox open
    // if all other windows are closed.
    Services.startup.QueryInterface(Ci.nsIObserver).observe(null, 'xul-window-destroyed', null);

    var extensionId = rootDirectory.leafName;

    Global.once('unload', function() {
      // Register the window again so that the window count remains accurate.
      // Otherwise the window mediator will think we have one less window than we really do.
      Services.startup.QueryInterface(Ci.nsIObserver).observe(xulWindow, 'xul-window-registered', null);
      window.close();
    });

    // Instantiate and load the extension object.
    this._extension = Global.loadExtension(extensionId, rootDirectory, firstRun);

    // Instantiate a BrowserEvents object whenever a new content window is created.
    this._extension.windowWatcher.register(
      function(win, context) {
        context.browserEvents = new BrowserEvents(this._extension, win);
        context.browserEvents.load();
      }.bind(this), function(win, context) {
        context.browserEvents.unload();
      }.bind(this)
    );

    // Load the background page.
    // We don't want the background scripts to run until the main browser window
    // has loaded since some of them may depend on it being ready.
    var browserWindow = Services.wm.getMostRecentWindow('navigator:browser');
    if ('complete' === browserWindow.document.readyState) {
      this._runBackground();
    }
    else {
      browserWindow.document.addEventListener('readystatechange', Binder.bindAnonymous(this, function(e) {
        if ('complete' === browserWindow.document.readyState) {
          browserWindow.document.removeEventListener('readystatechange', Binder.unbindAnonymous(), false);
          // TODO: Figure out why loading the background window directly from the event listener
          // causes its compartment to leak when the extension is disabled.
          // Using setTimeout() is a workaround and appears to fix the problem.
          setTimeout(this._runBackground.bind(this), 500);
        }
      }), false);
    }
  },

  _runBackground: function() {
    var manifest = this._extension.manifest;
    var spec = manifest.background.page ? this._extension.getURL(manifest.background.page) :
      // Cannot use 'about:blank' here, because DOM for 'about:blank'
      // is inappropriate for script inserting: neither 'document.head'
      // nor 'document.body' are defined.
      'chrome://ancho/content/html/blank.html';

    var browser = document.getElementById('content');

    loadHtml(this._extension, document, browser, spec, function(targetWindow) {
      // load background scripts, if any
      if (manifest.background.scripts) {
        for (var i=0; i<manifest.background.scripts.length; i++) {
          var script = targetWindow.document.createElement('script');
          script.src = this._extension.getURL(manifest.background.scripts[i]);
          targetWindow.document.head.appendChild(script);
        }
      }
      AnchoExternal.__set(targetWindow.ancho.external);
    }.bind(this));
  }
};

window.addEventListener('load', function() {
  window.removeEventListener('load', arguments.callee, false);
  var extensionRoot = window.arguments[0].QueryInterface(Ci.nsIFile);
  var firstRun = window.arguments[1];
  BackgroundWindow.init(extensionRoot, firstRun);
}, false);

window.addEventListener('unload', function(event) {
  window.removeEventListener('unload', arguments.callee, false);
  AnchoExternal.__set(null);
}, false);

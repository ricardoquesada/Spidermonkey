/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

const {Constructor: CC, classes: Cc, interfaces: Ci, utils: Cu} = Components;

const MARIONETTE_CONTRACTID = "@mozilla.org/marionette;1";
const MARIONETTE_CID = Components.ID("{786a1369-dca5-4adc-8486-33d23c88010a}");
const DEBUGGER_ENABLED_PREF = 'devtools.debugger.remote-enabled';
const MARIONETTE_ENABLED_PREF = 'marionette.defaultPrefs.enabled';
const DEBUGGER_FORCELOCAL_PREF = 'devtools.debugger.force-local';
const MARIONETTE_FORCELOCAL_PREF = 'marionette.force-local';

const ServerSocket = CC("@mozilla.org/network/server-socket;1",
                        "nsIServerSocket",
                        "initSpecialConnection");

Cu.import("resource://gre/modules/XPCOMUtils.jsm");
Cu.import("resource://gre/modules/Services.jsm");
Cu.import("resource://gre/modules/FileUtils.jsm");
Cu.import("resource://gre/modules/services-common/log4moz.js");

function MarionetteComponent() {
  this._loaded = false;
  // set up the logger
  this.logger = Log4Moz.repository.getLogger("Marionette");
  this.logger.level = Log4Moz.Level["INFO"];
  let logf = FileUtils.getFile('ProfD', ['marionette.log']);
  
  let formatter = new Log4Moz.BasicFormatter();
  this.logger.addAppender(new Log4Moz.RotatingFileAppender(logf, formatter));
  this.logger.addAppender(new Log4Moz.DumpAppender(formatter));
  this.logger.info("MarionetteComponent loaded");
}

MarionetteComponent.prototype = {
  classDescription: "Marionette component",
  classID: MARIONETTE_CID,
  contractID: MARIONETTE_CONTRACTID,
  QueryInterface: XPCOMUtils.generateQI([Ci.nsIObserver]),
  _xpcom_categories: [{category: "profile-after-change", service: true}],
  original_forcelocal: null,
  appName: Services.appinfo.name,

  onSocketAccepted: function mc_onSocketAccepted(aSocket, aTransport) {
    this.logger.info("onSocketAccepted for Marionette dummy socket");
  },

  onStopListening: function mc_onStopListening(aSocket, status) {
    this.logger.info("onStopListening for Marionette dummy socket, code " + status);
    aSocket.close();
  },

  observe: function mc_observe(aSubject, aTopic, aData) {
    let observerService = Services.obs;
    switch (aTopic) {
      case "profile-after-change":
        let enabled = false;
        try {
          enabled = Services.prefs.getBoolPref(MARIONETTE_ENABLED_PREF);
        } catch(e) {}
        if (enabled) {
          this.logger.info("marionette enabled");

          //add observers
          observerService.addObserver(this, "final-ui-startup", false);
          observerService.addObserver(this, "xpcom-shutdown", false);
        }
        else {
          this.logger.info("marionette not enabled");
        }
        break;
      case "final-ui-startup":
        this.logger.info("marionette initializing at " + aTopic);
        observerService.removeObserver(this, aTopic);

        try {
          this.original_forcelocal = Services.prefs.getBoolPref(DEBUGGER_FORCELOCAL_PREF);
        }
        catch(e) {}

        let marionette_forcelocal = this.appName == 'B2G' ? false : true;
        try {
          marionette_forcelocal = Services.prefs.getBoolPref(MARIONETTE_FORCELOCAL_PREF);
        }
        catch(e) {}
        Services.prefs.setBoolPref(DEBUGGER_FORCELOCAL_PREF, marionette_forcelocal);

        if (!marionette_forcelocal) {
          // See bug 800138.  Because the first socket that opens with
          // force-local=false fails, we open a dummy socket that will fail.
	  // keepWhenOffline=true so that it still work when offline (local).
          // This allows the following attempt by Marionette to open a socket
          // to succeed.
          let insaneSacrificialGoat = new ServerSocket(666, Ci.nsIServerSocket.KeepWhenOffline, 4);
          insaneSacrificialGoat.asyncListen(this);
        }

        this.init();
        break;
      case "xpcom-shutdown":
        observerService.removeObserver(this, "xpcom-shutdown");
        this.uninit();
        break;
    }
  },

  init: function mc_init() {
    if (!this._loaded) {
      this._loaded = true;
      let port;
      try {
        port = Services.prefs.getIntPref('marionette.defaultPrefs.port');
      }
      catch(e) {
        port = 2828;
      }
      try {
        Cu.import('resource://gre/modules/devtools/dbg-server.jsm');
        DebuggerServer.addActors('chrome://marionette/content/marionette-actors.js');
        // This pref is required for the remote debugger to open a socket,
        // so force it to true.  See bug 761252.

        let original = false;
        try {
          original = Services.prefs.getBoolPref(DEBUGGER_ENABLED_PREF);
        }
        catch(e) { }
        Services.prefs.setBoolPref(DEBUGGER_ENABLED_PREF, true);

        // Always allow remote connections.
        DebuggerServer.initTransport(function () { return true; });
        DebuggerServer.openListener(port);

        Services.prefs.setBoolPref(DEBUGGER_ENABLED_PREF, original);
        if (this.original_forcelocal != null) {
          Services.prefs.setBoolPref(DEBUGGER_FORCELOCAL_PREF,
                                     this.original_forcelocal);
        }
        this.logger.info("marionette listener opened");
      }
      catch(e) {
        this.logger.error('exception: ' + e.name + ', ' + e.message);
      }
    }
  },

  uninit: function mc_uninit() {
    DebuggerServer.closeListener();
    this._loaded = false;
  },

};

this.NSGetFactory = XPCOMUtils.generateNSGetFactory([MarionetteComponent]);

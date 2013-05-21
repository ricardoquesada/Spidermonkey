# SpiderMonkey VM

[SpiderMonkey](https://developer.mozilla.org/en-US/docs/SpiderMonkey) is [Mozilla](http://www.mozilla.org)'s JavaScript engine.

## About this repository

 - This is NOT the official SpiderMonkey standalone project. Official repo is [here](https://developer.mozilla.org/en-US/docs/SpiderMonkey)
 - Only contains the SpiderMonkey source code and needed files to compile it
 - Contains a few patches to make it compile on iOS (device and simulator)
 - Contains build scripts for iOS, Android, Win32 and OS X
 
 
## About builds

### iOS
 - JIT is disabled
 - Device only: compiled in RELEASE mode
 - Simulator only: compiled in DEBUG mode

### Android

 - JIT is disabled (Why???)
 - compiled in RELEASE mode
 

### OS X

 - JIT enabled
 - compiled in DEBUG mode
 

### Windows

 - JIT disabled (Why???)
 - compiled in RELEASE mode


## About the patches
 
 - Patches for [v21](https://github.com/ricardoquesada/Spidermonkey/wiki/Migrating-to-v21)
 - Patches for [v20](https://github.com/ricardoquesada/Spidermonkey/wiki/Migrating-to-v20)
 


 

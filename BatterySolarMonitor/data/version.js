// Shared constants to avoid circular dependencies
// Exports:    window.JS_VERSION (string), module.exports.JS_VERSION for Node.js
// Depends on: nothing
// Called by:  pfodWeb.js (reads window.JS_VERSION on load and stores in this.js_ver,
//             logs version after bundle load)
  var JS_VERSION = "V4.1.14-- 14th Oct 2026";  
// V4.1.14 fixed parsing of formatting tags    
// V4.1.13 limited raw data screen to last 500 lines. Raw Message View limit is also 500 line (pfod+raw msgs)    
// V4.1.12 minor designer fix for /urls, improved MVC separation  
// V4.1.11 supports .md style urls [..](..) in prompts and labels (not in dwgs)    
// V4.1.10 set dwg menu item name to dwg name  
// V4.1.9 added more AI restictions to assisted UI coding, fixed code generation bug when menu had two dwg menu items  
// V4.1.8 optimized for AI assisted UI coding  
// V4.1.7 tighten up validation, breaking change remove all browser storage 
// V4.1.6 improved raw message viewer and docs
// V4.1.5 changed code generation to support sub-classing
// V4.1.4 fixed code generation for charts
// V4.1.3 fixed dwg update bug, revised code generation
// V4.1.2 added dwg designer 
// V4.1.1 added supoort for for ESP32 and Pi Pico and ESP8266 and for connections BLE, TCP/IP and HTTP 
// V4.1.0 revised pfodProxy added password and extraFonts
// V4.0.2 added Mouse over show data values in chart
// V4.0.1 added Minimal C Code target
// V4.0.0 added Designer
// V3.1.0 added menu items
// V3.0.8 combined .js files to reduce downloads, added Chart Config
// V3.0.7 added Chart Only option
// V3.0.6 increase allowable length of HTTP ip:port to 50, Also keep http response when timeout resends cmd
// V3.0.5 now handles port no's via NAT for example targetIP=xx.xx.xx.xx:54989
// V3.0.4 added stacked charts
// V3.0.3 fixed chart updates and added timestamp to csv data
// V3.0.2 disable refresh in chart mode fixed value scaling
// V3.0.1 auto chart option on startup
// V3.0.0 added initial charting support
// V2.0.4 fixed scaling for nested dwgs
// V2.0.3 fixed transform for nested dwgs
// V2.0.2 fixed transform pushZero for nested dwgs
// V2.0.1 edit to .ino files
// V2.0.0 removed nodejs server, bundled all files in single htmls
// V1.1.5 added init() of drawings
// V1.1.4 added pfodMainDrawing.h generated file
// V1.1.3 dwg updates as response received

// Make available globally for browser use
if (typeof window !== 'undefined') {
    window.JS_VERSION = JS_VERSION;
}

// Export for Node.js use
if (typeof module !== 'undefined' && module.exports) {
    module.exports = { JS_VERSION };
}
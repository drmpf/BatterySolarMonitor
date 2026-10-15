#ifndef _WEB_PAGES_H
#define _WEB_PAGES_H
/*
   webPages.h
   by Matthew Ford,  2021/12/06
   (c)2021 Forward Computing and Control Pty. Ltd.
   NSW, Australia  www.forward.com.au
   This code may be freely used for both private and commerical use.
   Provide this copyright is maintained.
*/

// Registers the /settz.html page and its supporting /setTZstr, /resetTZ and
// /setTime handlers on the shared pfodWeb server (ESP_PicoW_pfodWebServer.cpp).
// Call once from setup(), after start_pfodWebServer().
void addSetTzWebPages();

#endif

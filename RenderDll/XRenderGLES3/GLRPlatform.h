/*=============================================================================
  GLRPlatform.h : start-up, texture conversion pumping and shutdown of the
  native backend's surroundings (texture module, DXT->ETC2 converter).
=============================================================================*/
#pragma once
bool GLRPlatformInit(void (*log)(const char*));   // after the context exists, before GLR::Init
void GLRPumpTextures();                           // frame start: upload finished conversions
void GLRFinishTextures();                         // level load end: all conversions done before drawing
void GLRPlatformShutdown();

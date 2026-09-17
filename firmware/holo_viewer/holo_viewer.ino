/********************************************************************
 * Holo Viewer — ESP32-P4 + ST7789 + tgx
 *
 * Render stage of a holographic desk-pet prototype:
 *
 *   photo  ->  Hunyuan 3D API  ->  .obj + PBR maps
 *          ->  tools/obj_to_h.py  ->  model_data.h
 *          ->  this firmware (software 3D rendering, tgx)
 *          ->  ST7789 240x240  ->  45 deg beam-splitter prism
 *          ->  a 3D image that appears to float inside the glass
 *
 * The mesh currently flashed is a decimated Egyptian pharaoh bust
 * (models/museum), used as the demo subject for the clip in docs/.
 *
 * Display driver : Adafruit_ST7789 (240x240, SPI)
 * Renderer       : tgx (3D software renderer for MCUs)
 * Model data     : model_data.h / model_texture.h (generated, see tools/)
 ********************************************************************/

#include <Arduino.h>
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <tgx.h>

#include "model_data.h"   // generated from a mesh in ../models/
#define MESH &model       // mesh symbol defined in model_data.h

using namespace tgx;

// === Display pins ===
#define TFT_CS   25
#define TFT_DC   2
#define TFT_RST  3
#define BL_PIN   48
#define TFT_MOSI 33
#define TFT_SCLK 26

// === Display geometry ===
#define SCREEN_WIDTH  240
#define SCREEN_HEIGHT 240

// === Refresh cap (~30 FPS) ===
#define FRAME_INTERVAL_MS 33

// === ST7789 instance ===
// Adafruit_ST7789 has no constructor taking an SPIClass, so we use the
// default SPI bus and call begin() manually.
Adafruit_ST7789 tft = Adafruit_ST7789(TFT_CS, TFT_DC, TFT_RST);

// === Framebuffers ===
uint16_t fb[SCREEN_WIDTH * SCREEN_HEIGHT];
uint16_t zbuf[SCREEN_WIDTH * SCREEN_HEIGHT];
Image<RGB565> imfb;

// === tgx renderer configuration ===
const Shader LOADED_SHADERS = SHADER_PERSPECTIVE | SHADER_ZBUFFER |
                              SHADER_FLAT | SHADER_GOURAUD |
                              SHADER_NOTEXTURE | SHADER_TEXTURE_NEAREST |
                              SHADER_TEXTURE_WRAP_POW2;

Renderer3D<RGB565, LOADED_SHADERS, uint16_t> renderer;
int slx, sly;

// === Setup ===
void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("=== Holo Viewer | ESP32-P4 + ST7789 + tgx ===");

  // Backlight on
  pinMode(BL_PIN, OUTPUT);
  digitalWrite(BL_PIN, HIGH);

  // SPI bus
  SPI.begin(TFT_SCLK, -1, TFT_MOSI, TFT_CS);
  SPI.setFrequency(80 * 1000000);  // 80 MHz
  delay(100);

  // Display
  tft.init(SCREEN_WIDTH, SCREEN_HEIGHT);
  tft.setRotation(2);
  tft.fillScreen(ST77XX_BLACK);
  delay(500);

  Serial.println("ST7789 init done!");

  // === Framebuffer and renderer ===
  slx = SCREEN_WIDTH;
  sly = SCREEN_HEIGHT;

  imfb.set(fb, slx, sly);
  renderer.setViewportSize(slx, sly);
  renderer.setOffset(0, 0);
  renderer.setImage(&imfb);
  renderer.setZbuffer(zbuf);
  renderer.setPerspective(45, ((float)slx) / sly, 1.0f, 100.0f);
  renderer.setMaterial(RGBf(0.85f, 0.55f, 0.25f), 0.2f, 0.7f, 0.8f, 64);
  renderer.setCulling(1);
  renderer.setTextureQuality(SHADER_TEXTURE_NEAREST);
  renderer.setTextureWrappingMode(SHADER_TEXTURE_WRAP_POW2);

  Serial.println("Renderer setup done!");
}

// === Camera animation: rotation plus a push-in / pull-back loop ===
// Stage 1 (6000 ms): wide shot, rotating
// Stage 2 (2000 ms): push in and move down
// Stage 3 (6000 ms): close-up
// Stage 4 (2000 ms): pull back and reset
tgx::fMat4 moveModel(int &loopnumber) {
  const float end1 = 6000;
  const float end2 = 2000;
  const float end3 = 6000;
  const float end4 = 2000;

  int tot = (int)(end1 + end2 + end3 + end4);
  int m = millis();
  loopnumber = m / tot;
  float t = m % tot;
  const float roty = 360 * (t / 4000);
  float tz, ty;
  const float dilat = 9;
  const float nearz = 7;
  const float upy = 6.5;

  if (t < end1) {
    tz = -25;
    ty = 0;
  } else {
    t -= end1;
    if (t < end2) {
      t /= end2;
      tz = -25 + (25 - nearz) * t;
      ty = -upy * t;
    } else {
      t -= end2;
      if (t < end3) {
        tz = -nearz;
        ty = -upy;
      } else {
        t -= end3;
        t /= end4;
        tz = -nearz - (25 - nearz) * t;
        ty = upy * (t - 1);
      }
    }
  }

  fMat4 M;
  M.setScale({dilat, dilat, dilat});
  M.multRotate(-roty, {0, 1, 0});
  M.multTranslate({0, ty, tz});
  return M;
}

// === Print the frame rate once per second over serial ===
void infos(int loopnumber) {
  static uint32_t prev_millis = 0;
  static int nbframes = 0;

  nbframes++;
  uint32_t m = millis();
  if (m > prev_millis + 1000) {
    Serial.printf("FPS: %d\n", nbframes);
    prev_millis = m;
    nbframes = 0;
  }
}

// === Main loop ===
int loopnumber = 0;

void loop() {
  // 1. Update the model transform matrix
  fMat4 M = moveModel(loopnumber);
  renderer.setModelMatrix(M);

  // 2. Clear the framebuffers
  imfb.fillScreen(RGB565(0, 0, 0));
  renderer.clearZbuffer();

  // 3. Cycle through one rendering mode per animation loop
  switch (loopnumber % 4) {
    case 0:  // texture + Gouraud shading
      renderer.setShaders(SHADER_GOURAUD | SHADER_TEXTURE);
      renderer.drawMesh(MESH, false);
      break;
    case 1:  // wireframe
      renderer.drawWireFrameMesh(MESH, true);
      break;
    case 2:  // flat shading
      renderer.setShaders(SHADER_FLAT);
      renderer.drawMesh(MESH, false);
      break;
    case 3:  // Gouraud shading
      renderer.setShaders(SHADER_GOURAUD);
      renderer.drawMesh(MESH, false);
      break;
  }

  infos(loopnumber);

  // 4. Blit the framebuffer to the display, rate limited
  static uint32_t lastRefresh = 0;
  int x = (tft.width() - slx) / 2;
  int y = (tft.height() - sly) / 2;

  if (millis() - lastRefresh > FRAME_INTERVAL_MS) {
    tft.drawRGBBitmap(x, y, fb, slx, sly);
    lastRefresh = millis();
  }
}

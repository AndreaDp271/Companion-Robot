/*
 *  Desktop Companion - il personaggio (occhioni, sopracciglia, bocca) e tutte le scene della pagina COMPANION.
 *
 *  Si disegna in coordinate "virtuali", le stesse dell'anteprima: l'area sotto la barra è alta 100,
 *  con l'origine al centro (x da -99 a 99). V:: porta tutto in pixel, con traslazioni, scale e rotazioni.
 */
#include <initializer_list>
#define TAU_F 6.2831853f
#define PI_F  3.1415927f
#define C565(h) (uint16_t)((((h) >> 8) & 0xF800) | (((h) >> 5) & 0x07E0) | (((h) >> 3) & 0x001F))

uint16_t mixC(uint16_t a, uint16_t b, float k){
  int ar = (a >> 11) & 31, ag = (a >> 5) & 63, ab = a & 31, br = (b >> 11) & 31, bg = (b >> 5) & 63, bb = b & 31;
  return ((int)(ar + (br - ar) * k) << 11) | ((int)(ag + (bg - ag) * k) << 5) | (int)(ab + (bb - ab) * k);
}
uint16_t hueL(float h){ return mixC(hueColor((int)h), TFT_WHITE, 0.25f); }
float fblink(float t, float off){ float p = fmodf(t + off, 4.3f); return p < 0.16f ? fabsf(p - 0.08f) / 0.08f : 1; }
float flook(float t){ return sinf(t * 0.7f) * 2.5f + sinf(t * 1.9f) * 0.8f; }
float frnd(float i){ float s = sinf(i * 12.9898f) * 43758.5453f; return s - floorf(s); }
float fmodp(float a, float b){ float r = fmodf(a, b); return r < 0 ? r + b : r; }
bool eq(const char* a, const char* b){ return strcmp(a, b) == 0; }

// ---------------------------------------------------------------- motore di disegno
struct V {
  inline static float m[6] = {1, 0, 0, 1, 0, 0};          // x' = a x + c y + e ; y' = b x + d y + f
  inline static float stk[12][6]; inline static int16_t cstk[12][4]; inline static int sp = 0;
  inline static int16_t cl[4] = {0, 0, 240, 135};          // ritaglio in pixel: x0 y0 x1 y1
  inline static float px[200], py[200]; inline static int pn = 0;
  inline static int sub[12]; inline static int nsub = 0;
  inline static float lx = 0, ly = 0;                      // ultimo punto (virtuale) del percorso

  static void base(){
    const float u = (CH - CTOP) / 100.0f;
    m[0] = u; m[1] = 0; m[2] = 0; m[3] = u; m[4] = CW / 2.0f; m[5] = CTOP + (CH - CTOP) / 2.0f;
    sp = 0; cl[0] = 0; cl[1] = CTOP; cl[2] = CW; cl[3] = CH; applyClip();
  }
  static void applyClip(){ cc.setClipRect(cl[0], cl[1], max(0, cl[2] - cl[0]), max(0, cl[3] - cl[1])); }
  static void done(){ cc.clearClipRect(); }
  static void save(){ if(sp >= 12) return; memcpy(stk[sp], m, sizeof m); memcpy(cstk[sp], cl, sizeof cl); sp++; }
  static void restore(){ if(sp <= 0) return; sp--; memcpy(m, stk[sp], sizeof m); memcpy(cl, cstk[sp], sizeof cl); applyClip(); }
  static void translate(float x, float y){ m[4] += m[0] * x + m[2] * y; m[5] += m[1] * x + m[3] * y; }
  static void scale(float s){ for(int i = 0; i < 4; i++) m[i] *= s; }
  static void rotate(float r){
    float cs = cosf(r), sn = sinf(r), a = m[0], b = m[1], c = m[2], d = m[3];
    m[0] = a * cs + c * sn; m[1] = b * cs + d * sn; m[2] = -a * sn + c * cs; m[3] = -b * sn + d * cs;
  }
  static void map(float x, float y, float& ox, float& oy){ ox = m[0] * x + m[2] * y + m[4]; oy = m[1] * x + m[3] * y + m[5]; }
  static bool axis(){ return fabsf(m[1]) < 1e-4f && fabsf(m[2]) < 1e-4f; }
  static float sc(){ return sqrtf(fabsf(m[0] * m[3] - m[1] * m[2])); }
  static void clipRect(float x, float y, float w, float h){
    float x0, y0, x1, y1; map(x, y, x0, y0); map(x + w, y + h, x1, y1);
    cl[0] = max<int>(cl[0], lroundf(min(x0, x1))); cl[1] = max<int>(cl[1], lroundf(min(y0, y1)));
    cl[2] = min<int>(cl[2], lroundf(max(x0, x1))); cl[3] = min<int>(cl[3], lroundf(max(y0, y1)));
    applyClip();
  }

  // ---- percorsi: punti già in pixel, più sottopercorsi ----
  static void begin(){ pn = 0; nsub = 0; }
  static void add(float x, float y){ if(pn < 200){ map(x, y, px[pn], py[pn]); pn++; } lx = x; ly = y; }
  static void moveTo(float x, float y){ if(nsub < 12) sub[nsub++] = pn; add(x, y); }
  static void lineTo(float x, float y){ if(!nsub) moveTo(x, y); else add(x, y); }
  static void closePath(){ if(nsub){ int s = sub[nsub - 1]; if(pn > s && pn < 200){ px[pn] = px[s]; py[pn] = py[s]; pn++; } } }
  static void quadTo(float cx, float cy, float x, float y){
    float x0 = lx, y0 = ly;
    for(int i = 1; i <= 8; i++){ float k = i / 8.0f, a = (1 - k) * (1 - k), b = 2 * (1 - k) * k, c = k * k; lineTo(a * x0 + b * cx + c * x, a * y0 + b * cy + c * y); }
  }
  static void arc(float cx, float cy, float r, float a0, float a1){
    int n = max(4, (int)(fabsf(a1 - a0) / TAU_F * 28) + 2);
    for(int i = 0; i <= n; i++){ float a = a0 + (a1 - a0) * i / n; float x = cx + cosf(a) * r, y = cy + sinf(a) * r; if(i == 0 && !nsub) moveTo(x, y); else lineTo(x, y); }
  }
  static void ellipse(float cx, float cy, float rx, float ry){
    moveTo(cx + rx, cy);
    for(int i = 1; i <= 24; i++){ float a = i * TAU_F / 24; add(cx + cosf(a) * rx, cy + sinf(a) * ry); }
  }
  static void rectPath(float x, float y, float w, float h){ moveTo(x, y); add(x + w, y); add(x + w, y + h); add(x, y + h); }

  // riempimento a scansione (pari/dispari); alpha < 1 = retinatura, per le cose trasparenti
  static void fill(uint16_t col, float alpha = 1){
    if(pn < 3) return;
    float y0 = 1e9, y1 = -1e9;
    for(int i = 0; i < pn; i++){ y0 = min(y0, py[i]); y1 = max(y1, py[i]); }
    int ya = max<int>(cl[1], floorf(y0)), yb = min<int>(cl[3] - 1, ceilf(y1));
    float xs[48];
    for(int y = ya; y <= yb; y++){
      float yc = y + 0.5f; int n = 0;
      for(int s = 0; s < nsub; s++){
        int a = sub[s], b = s + 1 < nsub ? sub[s + 1] : pn;
        for(int i = a; i < b; i++){
          int j = i == a ? b - 1 : i - 1;
          if((py[i] > yc) != (py[j] > yc) && n < 48) xs[n++] = px[i] + (yc - py[i]) * (px[j] - px[i]) / (py[j] - py[i]);
        }
      }
      for(int i = 1; i < n; i++){ float v = xs[i]; int j = i - 1; while(j >= 0 && xs[j] > v){ xs[j + 1] = xs[j]; j--; } xs[j + 1] = v; }
      for(int k = 0; k + 1 < n; k += 2){
        int xa = max<int>(cl[0], lroundf(xs[k])), xb = min<int>(cl[2] - 1, lroundf(xs[k + 1]) - 1);
        if(xb < xa) continue;
        if(alpha >= 0.75f) cc.drawFastHLine(xa, y, xb - xa + 1, col);
        else for(int x = xa; x <= xb; x++){
          bool on = alpha >= 0.4f ? ((x + y) & 1) == 0 : alpha >= 0.2f ? ((x & 1) == 0 && (y & 1) == 0) : (x % 3 == 0 && y % 3 == 0);
          if(on) cc.drawPixel(x, y, col);
        }
      }
    }
  }
  static void stroke(float w, uint16_t col){
    float r = max(0.5f, w * sc() / 2);
    for(int s = 0; s < nsub; s++){
      int a = sub[s], b = s + 1 < nsub ? sub[s + 1] : pn;
      for(int i = a + 1; i < b; i++) cc.drawWideLine(px[i - 1], py[i - 1], px[i], py[i], r, col);
      if(b - a == 1) cc.fillCircle(lroundf(px[a]), lroundf(py[a]), lroundf(r), col);
    }
  }

  // ---- primitive come nell'anteprima ----
  static void E(float x, float y, float rx, float ry, uint16_t col){
    if(rx <= 0 || ry <= 0) return;
    if(axis()){
      float cx, cy; map(x, y, cx, cy); float a = fabsf(m[0]) * rx, b = fabsf(m[3]) * ry;
      if(a < 0.7f && b < 0.7f) cc.drawPixel(lroundf(cx), lroundf(cy), col);
      else cc.fillEllipse(lroundf(cx), lroundf(cy), max(0L, lroundf(a)), max(0L, lroundf(b)), col);
    } else { begin(); ellipse(x, y, rx, ry); fill(col); }
  }
  static void Ea(float x, float y, float rx, float ry, uint16_t col, float alpha){ begin(); ellipse(x, y, rx, ry); fill(col, alpha); }
  static void R(float x, float y, float w, float h, float r, uint16_t col){
    if(w <= 0 || h <= 0) return;
    if(axis()){
      float x0, y0, x1, y1; map(x, y, x0, y0); map(x + w, y + h, x1, y1);
      int ix = lroundf(min(x0, x1)), iy = lroundf(min(y0, y1)), iw = max(1L, lroundf(max(x0, x1)) - ix), ih = max(1L, lroundf(max(y0, y1)) - iy);
      int rr = min<int>(lroundf(r * fabsf(m[0])), min(iw, ih) / 2);
      if(rr > 0) cc.fillRoundRect(ix, iy, iw, ih, rr, col); else cc.fillRect(ix, iy, iw, ih, col);
    } else { begin(); rectPath(x, y, w, h); fill(col); }
  }
  static void SR(float x, float y, float w, float h, float lw, uint16_t col){ begin(); rectPath(x, y, w, h); closePath(); stroke(lw, col); }
  static void L(std::initializer_list<float> p, float w, uint16_t col){
    begin(); auto it = p.begin(); bool first = true;
    while(it != p.end()){ float x = *it++; if(it == p.end()) break; float y = *it++; if(first){ moveTo(x, y); first = false; } else add(x, y); }
    stroke(w, col);
  }
  static void A(float x, float y, float r, float a0, float a1, float w, uint16_t col){ begin(); arc(x, y, r, a0, a1); stroke(w, col); }
  static void P(std::initializer_list<float> p, uint16_t col, float alpha = 1){
    begin(); auto it = p.begin(); bool first = true;
    while(it != p.end()){ float x = *it++; if(it == p.end()) break; float y = *it++; if(first){ moveTo(x, y); first = false; } else add(x, y); }
    fill(col, alpha);
  }
  static void T(const char* s, float x, float y, float size, uint16_t col, textdatum_t d = middle_center){
    float cx, cy; map(x, y, cx, cy); float h = size * sc();
    const lgfx::IFont* f = &fonts::Font4;
    if(h < 11) f = &fonts::Font0; else if(h < 21) f = &fonts::Font2;
    cc.setTextFont(f);
    cc.setTextDatum(d); cc.setTextColor(col); cc.drawString(s, lroundf(cx), lroundf(cy));
  }
  // sfumatura verticale (fino a 3 colori)
  static void grad(float x, float y, float w, float h, uint32_t c0, uint32_t c1, uint32_t c2 = 0xFFFFFFFF, float mid = 0.5f){
    float x0, y0, x1, y1; map(x, y, x0, y0); map(x + w, y + h, x1, y1);
    int ya = max<int>(cl[1], lroundf(y0)), yb = min<int>(cl[3], lroundf(y1)), xa = max<int>(cl[0], lroundf(x0)), xb = min<int>(cl[2], lroundf(x1));
    uint16_t a = C565(c0), b = C565(c1), c = c2 == 0xFFFFFFFF ? b : C565(c2);
    for(int yy = ya; yy < yb; yy++){
      float k = (yy - y0) / max(1.0f, y1 - y0);
      uint16_t col = c2 == 0xFFFFFFFF ? mixC(a, b, k) : (k < mid ? mixC(a, b, k / mid) : mixC(b, c, (k - mid) / (1 - mid)));
      cc.drawFastHLine(xa, yy, xb - xa, col);
    }
  }
};

// ---------------------------------------------------------------- il personaggio e le scene
struct S {
  static constexpr uint16_t BG = 0x0000, WH = 0xFFFF;
  static constexpr uint16_t EYE = C565(0x5ad1ff), BROW = C565(0xcfe9ff), TONGUE = C565(0xff7b93), LOVE = C565(0xff4f7a),
                            ACC = C565(0xffd166), DROP = C565(0x7cc8ff), BAR = C565(0x151a28), CLAWD = C565(0xd97757);

  static float eyeH(const char* m){
    if(eq(m, "excited") || eq(m, "ask")) return 40; if(eq(m, "work")) return 20; if(eq(m, "sleep")) return 4;
    if(eq(m, "sleepy")) return 15; if(eq(m, "hot")) return 22; if(eq(m, "sad")) return 30; if(eq(m, "stress")) return 28;
    return 34;
  }
  static void browOf(const char* m, float& raise, float& k){
    struct B { const char* m; float r, k; };
    static const B tb[] = {{"happy", 5, .18f}, {"excited", 8, .2f}, {"love", 4, .15f}, {"surprised", 10, 0}, {"sad", 1, .3f},
      {"stress", -3, -.3f}, {"sleepy", -3, .15f}, {"sleep", -2, .08f}, {"work", -3, -.38f}, {"ask", 8, 0}, {"cold", 2, .3f},
      {"hot", 0, .25f}, {"music", 0, .12f}};
    raise = 0; k = 0;
    for(auto& b : tb) if(eq(m, b.m)){ raise = b.r; k = b.k; return; }
  }
  static void heart(float x, float y, float size, uint16_t col){
    float r = size * 0.28f;
    V::E(x - r * 0.95f, y - r * 0.4f, r, r, col); V::E(x + r * 0.95f, y - r * 0.4f, r, r, col);
    V::P({x - size * 0.53f, y - r * 0.15f, x + size * 0.53f, y - r * 0.15f, x, y + size * 0.55f}, col);
  }
  static void brow(float x, float y, float ang, uint16_t col){
    V::save(); V::translate(x, y); V::rotate(ang); V::R(-13, -2.6f, 26, 5.2f, 2.6f, col); V::restore();
  }
  static void mouth(const char* m, float t, uint16_t col){
    V::begin(); bool fill = false; int tongue = 0;
    if(eq(m, "happy") || eq(m, "love")){ V::moveTo(-10, 21); V::arc(0, 21, 10, 0, PI_F); fill = true; tongue = 1; }
    else if(eq(m, "excited")){ V::moveTo(-12, 20); V::arc(0, 20, 12, 0, PI_F); fill = true; tongue = 2; }
    else if(eq(m, "surprised")){ V::ellipse(0, 27, 6, 7.5f); fill = true; }
    else if(eq(m, "ask") || eq(m, "talk")){ float o = eq(m, "talk") ? 1.5f + fabsf(sinf(t * 9)) * 4 : 5.5f; V::ellipse(0, 26, 4.5f, o); fill = true; }
    else if(eq(m, "music")){ V::ellipse(0, 26, 5, 2.5f + fabsf(sinf(t * PI_F * 2)) * 4.5f); fill = true; }
    else if(eq(m, "sleep")){ float r = 2.2f + sinf(t * 1.5f) * 0.8f; V::ellipse(0, 26, r, r); fill = true; }
    else if(eq(m, "sleepy")){ V::ellipse(0, 26, 2.5f, 3); fill = true; }
    else if(eq(m, "sad")) V::arc(0, 35, 9, PI_F * 1.22f, PI_F * 1.78f);
    else if(eq(m, "stress")){ V::moveTo(-10, 26); for(int i = 1; i <= 5; i++) V::lineTo(-10 + i * 4, i % 2 ? 23 : 27); }
    else if(eq(m, "bored")){ if(fmodf(t, 5) < 1.1f){ V::ellipse(0, 26, 5, 6.5f); fill = true; } else { V::moveTo(-8, 27); V::lineTo(8, 25); } }
    else if(eq(m, "work")){ V::moveTo(-7, 26); V::lineTo(4, 25); tongue = 3; }
    else if(eq(m, "cold")){ V::rectPath(-9, 21, 18, 9); V::closePath(); }
    else if(eq(m, "hot")){ V::ellipse(0, 24, 6, 4.5f); fill = true; tongue = 4; }
    else V::arc(0, 17, 9, PI_F * 0.22f, PI_F * 0.78f);
    if(fill) V::fill(col); else V::stroke(3.2f, col);
    if(eq(m, "cold")){ float j = ((int)(t * 14)) % 2; V::L({-3 + j, 22, -3 + j, 29}, 1.6f, col); V::L({3 + j, 22, 3 + j, 29}, 1.6f, col); }
    if(tongue == 1) V::E(0, 27.5f, 5, 2.6f, TONGUE);
    if(tongue == 2) V::E(0, 28, 6, 3, TONGUE);
    if(tongue == 3) V::E(5, 28, 2.8f, 2.6f, TONGUE);
    if(tongue == 4) V::E(2, 29, 3.5f, 5, TONGUE);
  }
  static void face(const char* m, float t, float x = 0, float y = 0, float s = 1, float lkx = 0, float lky = 0,
                   int32_t col = -1, int32_t browCol = -1, int32_t mouthCol = -1, const char* mm = nullptr, bool brows = true){
    uint16_t ec = col < 0 ? EYE : col, bc = browCol < 0 ? BROW : browCol;
    V::save(); V::translate(x, y); V::scale(s);
    for(int sd = -1; sd <= 1; sd += 2){
      float ex = sd * 28 + lkx, ey = -2 + lky, w = 26, h = eyeH(m);
      if(eq(m, "cold")) ex += ((int)(t * 20)) % 2 ? 1.2f : -1.2f;
      if(eq(m, "normal") || eq(m, "cold")) h *= fblink(t, 0);
      float raise, k; browOf(m, raise, k); float ang = sd * k;
      if(eq(m, "bored")){ raise = sd < 0 ? -2 : 6; ang = sd < 0 ? 0 : -0.15f; }
      if(eq(m, "music")) raise = fabsf(sinf(t * PI_F * 2)) * 6;
      if(eq(m, "normal")) raise = fmaxf(0, sinf(t * 0.9f)) * 3;
      float top = ey - h / 2, outX = ex + sd * (w / 2 + 1), inX = ex - sd * (w / 2 + 1);
      if(eq(m, "love")) heart(ex, ey, 30, LOVE);
      else if(eq(m, "surprised")){ V::E(ex, ey, 15, 19, ec); V::E(ex, ey, 5.5f, 5.5f, BG); }
      else {
        V::R(ex - w / 2, top, w, fmaxf(2.5f, h), 8, ec);
        if(eq(m, "happy") || eq(m, "excited") || eq(m, "music")) V::E(ex, ey + h / 2 + 5, w / 2 + 5, h / 2 + 1, BG);
        if(eq(m, "bored")) V::R(ex - w / 2 - 1, top - 1, w + 2, h * 0.5f, 0, BG);
        if(eq(m, "ask")) V::E(ex, ey, 6.5f, 6.5f, BG);
        if(eq(m, "sad") || eq(m, "sleepy") || eq(m, "hot")) V::P({outX, top - 1, inX, top - 1, outX, top + h * 0.6f}, BG);
        if(eq(m, "stress")) V::P({outX, top - 1, inX, top - 1, inX, top + h * 0.5f}, BG);
      }
      if(eq(m, "excited") && ((int)(t * 4)) % 2){ V::L({ex + sd * 18, ey - 20, ex + sd * 18, ey - 12}, 1.6f, ACC); V::L({ex + sd * 14, ey - 16, ex + sd * 22, ey - 16}, 1.6f, ACC); }
      float by = eq(m, "sleep") ? ey - 12 : ey - fmaxf(h, eq(m, "surprised") ? 38 : 20) / 2 - 8 - raise;
      if(brows) brow(ex, by, ang, bc);
    }
    mouth(mm ? mm : m, t, mouthCol < 0 ? ec : mouthCol);
    V::restore();
  }

  // ---- piccoli oggetti ----
  static void zzz(float x, float y, float t, uint16_t col){ for(int i = 0; i < 3; i++){ float p = fmodf(t * 0.55f + i / 3.0f, 1); V::T("z", x + p * 12, y - p * 18, 7 + p * 6, col); } }
  static void notes(float x, float y, float t, float spread, int hueOff, bool rainbow){
    for(int i = 0; i < 3; i++){
      float p = fmodf(t * 0.45f + i / 3.0f, 1), nx = x + (i - 1) * spread + sinf(p * 6 + i) * 4, ny = y - p * 26;
      uint16_t c = rainbow ? hueL(i * 90 + hueOff + t * 60) : ACC;
      V::E(nx, ny, 2.8f, 2.1f, c); V::L({nx + 2.4f, ny, nx + 2.4f, ny - 8, nx + 5.5f, ny - 6}, 1.3f, c);
    }
  }
  static void sweat(float x, float y, uint16_t col){ V::E(x, y, 2.3f, 2.8f, col); V::P({x - 2.2f, y - 1, x, y - 6, x + 2.2f, y - 1}, col); }
  static void sparkle(float x, float y, float r, uint16_t col){ V::L({x - r, y, x + r, y}, 1.6f, col); V::L({x, y - r, x, y + r}, 1.6f, col); }
  static void star(float cx, float cy, float r, float ang, uint16_t col){ for(int i = 0; i < 4; i++){ float a = ang + i * PI_F / 4; V::L({cx - cosf(a) * r, cy - sinf(a) * r, cx + cosf(a) * r, cy + sinf(a) * r}, 2, col); } }
  static void headphones(float cx, float cy, float r, uint16_t band, uint16_t cup){
    V::A(cx, cy, r, PI_F * 1.06f, PI_F * 1.94f, 3.4f, band);
    V::R(cx - r - 5, cy - 7, 10, 18, 4, cup); V::R(cx + r - 5, cy - 7, 10, 18, 4, cup);
  }

  // ---- umori ----
  static void mood(const char* m, float t){
    float y = 4, lkx = 0, lky = 0;
    if(eq(m, "normal")){ lkx = flook(t) * 2.4f; lky = sinf(t * 0.5f) * 1.5f; }
    if(eq(m, "happy") || eq(m, "excited")) y -= fabsf(sinf(t * 4)) * 6;
    if(eq(m, "stress")) lkx = (((int)(t * 30)) % 3 - 1) * 1.2f;
    if(eq(m, "bored")){ lkx = sinf(t * 0.5f) * 10; lky = 3; }
    face(m, t, 0, y, 1, lkx, lky);
    if(eq(m, "excited")){ const float xs[] = {-78, 78, -66, 70}, ys[] = {-30, -26, 30, 34}; for(int i = 0; i < 4; i++) if(((int)(t * 3 + i)) % 3) sparkle(xs[i], ys[i], 4, ACC); }
    if(eq(m, "love")){ const float xs[] = {-80, 80, -66, 68}; for(int i = 0; i < 4; i++){ float p = fmodf(t * 0.4f + i / 4.0f, 1); heart(xs[i], 44 - p * 90, 9, LOVE); } }
    if(eq(m, "surprised")) V::T("!", 72, -26, 20, ACC);
    if(eq(m, "sad")){ float p = fmodf(t * 0.6f, 1); V::E(-40, 10 + p * 30, 2.4f, 3.2f, DROP); }
    if(eq(m, "stress")) sweat(58, -22 + fmodf(t * 20, 14), DROP);
    if(eq(m, "sleepy") && fmodf(t, 6) < 1.2f) V::T("Uaaah", 66, -34, 8, ACC);
    if(eq(m, "sleep")) zzz(52, -24, t, ACC);
  }

  // ---- VS Code ----
  static void laptop(float t, float x, float y){
    V::R(x - 46, y - 10, 92, 46, 5, C565(0x3a4055)); V::R(x - 42, y - 6, 84, 38, 3, C565(0x0d1220));
    static const float lines[][2] = {{0, 40}, {8, 30}, {8, 50}, {16, 24}, {8, 36}, {0, 18}};
    static const uint32_t cols[] = {0xc792ea, 0x82aaff, 0xc3e88d, 0xf78c6c, 0x89ddff, 0xc792ea};
    float typed = fmodf(t * 30, 220), acc = 0;
    for(int i = 0; i < 6; i++){ float w = fmaxf(0, fminf(lines[i][1], typed - acc)); acc += lines[i][1]; V::R(x - 38 + lines[i][0], y - 2 + i * 5.6f, w, 3, 1, C565(cols[i])); }
    if(((int)(t * 3)) % 2) V::R(x - 38 + 40, y - 2 + 5 * 5.6f, 2, 4, 0, WH);
    V::P({x - 56, y + 36, x + 56, y + 36, x + 50, y + 44, x - 50, y + 44}, C565(0x5a6178));
  }
  static void code(float t, bool music){
    if(music){
      float b = -fabsf(sinf(t * PI_F * 2)) * 2;
      face("music", t, 0, -27 + b, 0.55f, 0, 4); headphones(0, -29 + b, 28, C565(0x8a8fa3), C565(0xff5aa5));
    } else face("normal", t, 0, -27, 0.55f, sinf(t * 1.3f) * 5, 4);
    laptop(t, 0, 10);
  }

  // ---- giochi ----
  static void gameAny(float t){
    const float sx = -66, sy = -12, sw = 132, sh = 56, period = 2.2f;
    float ph = fmodf(t, period) / period, obX = sx + sw + 12 - ph * (sw + 30), heroX = sx + 34, d = obX - heroX;
    float jump = fabsf(d) < 18 ? cosf(d / 18 * PI_F / 2) * 17 : 0;
    face(jump > 4 ? "excited" : "stress", t, 0, -33, 0.4f, 0, 7, -1, -1, -1, jump > 4 ? "happy" : "work");
    V::R(sx - 5, sy - 5, sw + 10, sh + 10, 7, C565(0x2b3042)); V::R(sx, sy, sw, sh, 2, BG);
    V::save(); V::clipRect(sx, sy, sw, sh);
    V::grad(sx, sy, sw, sh, 0x4fb6ff, 0xc8efff);
    for(int i = 0; i < 3; i++){ float cx = sx + fmodp(i * 60 - t * 6, 180) - 20; V::E(cx, sy + 12, 9, 4.5f, WH); V::E(cx + 7, sy + 10, 7, 5, WH); }
    for(int i = 0; i < 4; i++){ float hx = sx + fmodp(i * 50 - t * 14, 200) - 25; V::E(hx, sy + sh - 8, 26, 16, i % 2 ? C565(0x6cc65a) : C565(0x58b048)); }
    float gy = sy + sh - 9, off = fmodf(t * 34, 10);
    for(float x = sx - off; x < sx + sw; x += 10){ V::R(x, gy, 10, 9, 0, C565(0xc8763c)); V::R(x, gy, 10, 3, 0, C565(0x5fd13f)); V::R(x + 2, gy + 5, 2, 2, 0, C565(0x9a5528)); }
    V::R(obX - 6, gy - 14, 12, 14, 1, C565(0x2fa84f)); V::R(obX - 8, gy - 17, 16, 5, 1, C565(0x3cc75f));
    float cw = fabsf(cosf(t * 6)) * 3.2f + 0.6f, coinX = obX + 26;
    V::E(coinX, gy - 30, cw, 3.8f, C565(0xffd23c)); V::E(coinX, gy - 30, cw * 0.4f, 2.4f, C565(0xffef9a));
    float hy = gy - jump; int step = ((int)(t * 10)) % 2;
    V::R(heroX - 4, hy - 16, 8, 3, 0, C565(0xe23b3b)); V::R(heroX - 3, hy - 13, 6, 4, 0, C565(0xffc79a)); V::R(heroX - 4, hy - 9, 8, 5, 0, C565(0x2b62d8));
    if(jump < 1){ V::R(heroX - 4 + step, hy - 4, 3, 4, 0, C565(0x6b3a1a)); V::R(heroX + 1 - step, hy - 4, 3, 4, 0, C565(0x6b3a1a)); }
    else { V::R(heroX - 4, hy - 4, 3, 3, 0, C565(0x6b3a1a)); V::R(heroX + 2, hy - 5, 3, 3, 0, C565(0x6b3a1a)); }
    char sc[12]; snprintf(sc, 12, "x %d", 12 + (int)(t / period)); V::T(sc, sx + 4, sy + 6, 6, WH, middle_left);
    V::restore();
  }
  static void minecraft(float t){
    const float B = 12;
    V::grad(-99, -50, 198, 100, 0x6aaeff, 0xbfe2ff);
    V::R(66, -44, 14, 14, 0, C565(0xfff2a0));
    for(int i = 0; i < 2; i++){ float cx = fmodf(i * 110 + t * 6, 260) - 130; V::R(cx, -40 + i * 10, 30, 5, 0, C565(0xf6fbff)); V::R(cx + 6, -45 + i * 10, 18, 5, 0, C565(0xf6fbff)); V::R(cx + 30, -40 + i * 10, 12, 5, 0, C565(0xf6fbff)); }
    int i = 0;
    for(float gx = -99; gx < 99; gx += B, i++) for(int r = 0; r < 2; r++){
      float y = 26 + r * B;
      V::R(gx, y, B, B, 0, C565(0x8a5a33));
      for(int k = 0; k < 4; k++) V::R(gx + floorf(frnd(i * 7 + k + r * 31) * 10), y + floorf(frnd(i * 13 + k * 3 + r) * 10), 2, 2, 0, C565(0x6a4222));
      if(r == 0){ V::R(gx, y, B, 3.5f, 0, C565(0x5cbf3a)); for(int k = 0; k < 3; k++) V::R(gx + k * 4 + 1, y + 3.5f, 2, 1.5f + frnd(i + k) * 2, 0, C565(0x5cbf3a)); }
    }
    float tx = -86, flick = sinf(t * 20) * 0.6f;
    V::R(tx - 1.5f, 10, 3, 16, 0, C565(0x8b5a2b)); V::E(tx, 8 + flick, 3.2f, 4.4f, C565(0xffb43c)); V::E(tx, 9, 1.6f, 2.2f, C565(0xfff2a0));
    float ph = fmodf(t, 4) / 4, bx = 30, by = 2; bool gone = ph > 0.78f;
    if(!gone){
      V::R(bx, by, 24, 24, 0, C565(0x8e8e8e));
      for(int k = 0; k < 10; k++) V::R(bx + floorf(frnd(k * 3.1f) * 21), by + floorf(frnd(k * 5.7f) * 21), 3, 3, 0, k % 2 ? C565(0xa6a6a6) : C565(0x6f6f6f));
      const float ore[][2] = {{5, 6}, {15, 4}, {9, 15}, {17, 16}}; for(auto& o : ore) V::R(bx + o[0], by + o[1], 3, 3, 0, C565(0x4fe3e0));
      int stage = (int)(ph / 0.78f * 5);
      const float cr[][4] = {{12, 12, 4, 6}, {12, 12, 20, 8}, {12, 12, 8, 20}, {12, 12, 21, 19}, {4, 6, 2, 2}};
      for(int k = 0; k < stage; k++) V::L({bx + cr[k][0], by + cr[k][1], bx + cr[k][2], by + cr[k][3]}, 1.4f, C565(0x2a2a2a));
    } else {
      float p = (ph - 0.78f) / 0.22f;
      for(int k = 0; k < 8; k++){ float a = k * TAU_F / 8; V::R(bx + 12 + cosf(a) * p * 18, by + 12 + sinf(a) * p * 14 + p * p * 10, 3, 3, 0, k % 2 ? C565(0x8e8e8e) : C565(0x4fe3e0)); }
      float s = fabsf(cosf(t * 3)) * 5 + 1; V::P({bx + 12, by + 6 - sinf(t * 4) * 2, bx + 12 + s, by + 12, bx + 12, by + 18, bx + 12 - s, by + 12}, C565(0x6ff6ff));
    }
    float swing = gone ? -0.4f : sinf(t * 11) * 0.55f - 0.15f;
    V::save(); V::translate(12, -4); V::rotate(swing);
    V::L({0, 4, 18, -14}, 3, C565(0x8b5a2b));
    V::P({8, -24, 14, -20, 22, -14, 28, -6, 24, -6, 18, -12, 12, -16, 6, -20}, C565(0x5ff0ee)); V::restore();
    uint16_t dk = C565(0x16223f);
    face("work", t, -38, -12, 0.5f, 7, 3, dk, dk, dk);
  }
  static void wave(float y0, float amp, float k, float sp, float t, uint16_t col){
    V::begin(); V::moveTo(-99, 50);
    for(float x = -99; x <= 99; x += 4) V::lineTo(x, y0 + sinf(x / k + t * sp) * amp);
    V::lineTo(99, 50); V::fill(col);
  }
  static void sail(float x0, float y0, float x1, float y1, float bulge){
    V::begin(); V::moveTo(x0, y0); V::quadTo((x0 + x1) / 2 + bulge, y0 - 2, x1, y0); V::lineTo(x1, y1); V::quadTo((x0 + x1) / 2 + bulge * 1.4f, y1 + 4, x0, y1); V::fill(C565(0xf2e6c8));
  }
  static void seaOfThieves(float t){
    const float hz = 10;
    V::grad(-99, -50, 198, 60, 0x2a1c52, 0xc4457a, 0xff9e4d, 0.55f);
    V::save(); V::clipRect(-99, -50, 198, 60); V::E(46, hz + 2, 15, 15, C565(0xffd36b)); V::restore();
    V::grad(-99, hz, 198, 40, 0x2a3d72, 0x0d1838);
    for(int i = 0; i < 6; i++){ float y = hz + 4 + i * 4, w = 16 - i * 1.6f, shf = sinf(t * 3 + i) * 3; V::R(46 - w / 2 + shf, y, w, 1.2f, 0, C565(0xffcf7a)); }
    for(int i = 0; i < 3; i++){ float p = fmodf(t * 0.25f + i / 3.0f, 1), x = -80 + p * 160, y = -34 + i * 7 + sinf(t * 4 + i) * 2; V::L({x - 4, y - 2, x, y, x + 4, y - 2}, 1.2f, C565(0x1b1530)); }
    wave(hz + 10, 2, 14, 1.5f, t, C565(0x1e3a73));
    float bob = sinf(t * 1.6f) * 2.5f, roll = sinf(t * 1.3f) * 0.05f;
    V::save(); V::translate(-6, bob + 2); V::rotate(roll);
    uint16_t wood = C565(0x5a3417);
    V::L({0, 18, 0, -46}, 2.6f, wood); V::L({-26, 18, -26, -30}, 2.2f, wood); V::L({44, 12, 64, 2}, 1.8f, wood); V::R(-6, -48, 12, 5, 1, wood);
    float bl = sinf(t * 2) * 1.5f;
    sail(-23, -26, -2, 8, 4 + bl); sail(4, -40, 40, 6, 6 + bl);
    uint16_t navy = C565(0x263655);
    face("happy", t, 24, -18, 0.31f, 0, 0, navy, navy);
    float fw = sinf(t * 6) * 2; V::P({0, -46, 0, -38, 14, -40 + fw, 14, -46 + fw}, C565(0x141414)); V::E(7, -42.5f + fw / 2, 1.8f, 1.8f, WH);
    V::begin(); V::moveTo(-50, 10); V::lineTo(52, 10); V::quadTo(46, 30, 30, 32); V::lineTo(-34, 32); V::quadTo(-48, 28, -50, 10); V::fill(C565(0x6b3f1f));
    for(float y = 15; y < 31; y += 4) V::L({-46, y, 48 - (y - 10) * 0.4f, y}, 0.8f, C565(0x4e2d14));
    V::R(-50, 9, 102, 2.5f, 1, C565(0xd9a441)); for(float x = -36; x < 40; x += 14) V::R(x, 16, 5, 4, 1, C565(0x24140a));
    V::restore();
    wave(36, 2.6f, 11, 2.4f, t, C565(0x2556a0));
    for(float x = -99; x < 99; x += 4){ float s = sinf(x / 11 + t * 2.4f); if(s < -0.6f) V::R(x, 36 + s * 2.6f - 0.5f, 4, 1.4f, 0, C565(0xd8efff)); }
  }
  static void cities(float t){
    V::grad(-99, -50, 198, 100, 0x0a0f2e, 0x3b2b63, 0x6b3b6e, 0.7f);
    for(int i = 0; i < 14; i++) if(((int)(t * 2) + i) % 5) V::R(-96 + frnd(i) * 192, -48 + frnd(i + 40) * 34, 1, 1, 0, WH);
    V::E(74, -36, 7, 7, C565(0xf4efd0));
    V::begin(); V::moveTo(-99, 30); for(float x = -99; x <= 99; x += 11){ float h = 10 + frnd(x) * 22; V::lineTo(x, 30 - h); V::lineTo(x + 11, 30 - h); } V::lineTo(99, 30); V::fill(C565(0x1a2044));
    static const float bs[][3] = {{-92, 34, 16}, {-74, 50, 18}, {-54, 28, 14}, {-38, 62, 20}, {-16, 40, 16}, {2, 54, 18}, {22, 32, 14}, {38, 46, 18}, {58, 66, 20}, {80, 38, 16}};
    static const uint32_t bc[] = {0x38507a, 0x4a3f6e, 0x2f5a66, 0x5a4a7a};
    float lit = fminf(1, fmodf(t, 10) / 6);
    for(int i = 0; i < 10; i++){
      float x = bs[i][0], h = bs[i][1], w = bs[i][2], top = 38 - h;
      V::R(x, top, w, h, 1, C565(bc[i % 4]));
      if(i == 3){ V::L({x + w / 2, top, x + w / 2, top - 8}, 1, C565(0xaaaabb)); if(((int)(t * 2)) % 2) V::E(x + w / 2, top - 8, 1.4f, 1.4f, C565(0xff4040)); }
      for(float wy = top + 4; wy < 34; wy += 5) for(float wx = x + 3; wx < x + w - 3; wx += 5) if(frnd(i * 97 + wy * 3 + wx) < lit * 0.85f) V::R(wx, wy, 2.4f, 2.4f, 0, frnd(wx + wy) > 0.2f ? C565(0xffe39a) : C565(0x9fe3ff));
    }
    float ch = fmodf(t, 6) / 6, hx = 36, hy = -38 + sinf(ch * PI_F) * 20; uint16_t y1 = C565(0xffb000);
    V::L({70, 38, 70, -40}, 2, y1); V::L({70, -40, 22, -40}, 2, y1); V::L({70, -40, 78, -34, 70, -30}, 1.2f, y1);
    for(float y = 34; y > -38; y -= 6) V::L({68, y, 72, y - 6}, 0.8f, C565(0xc98a00));
    V::L({hx, -40, hx, hy}, 0.8f, C565(0xccccdd)); V::R(hx - 5, hy, 10, 6, 1, C565(0x6aa0ff));
    V::R(-99, 38, 198, 12, 0, C565(0x14161f));
    for(float x = -99; x < 99; x += 12) V::R(x + fmodf(t * 20, 12), 44, 6, 1, 0, C565(0xf2d45c));
    for(int i = 0; i < 4; i++){ float x = fmodf(i * 55 + t * 40, 230) - 115; V::R(x, 40.5f, 5, 2, 0, C565(0xff4848)); float x2 = 115 - fmodf(i * 60 + t * 34, 230); V::R(x2, 46.5f, 5, 2, 0, C565(0xfff6c2)); V::begin(); V::rectPath(x2 - 12, 46, 12, 3); V::fill(C565(0xfff6c2), 0.25f); }
    face("happy", t, -54, -32, 0.34f, 6, 3);
  }
  static void euroTruck(float t){
    const float hz = -6;
    V::grad(-99, -50, 198, 46, 0x3a4f8c, 0xf2a65a, 0xffd28a, 0.7f);
    V::E(-60, hz - 4, 9, 9, C565(0xfff1b0));
    for(int layer = 0; layer < 2; layer++){
      float amp = layer ? 9 : 16, off = layer ? 9 : 3;
      V::begin(); V::moveTo(-99, hz); for(float x = -99; x <= 99; x += 9) V::lineTo(x, hz - amp * (0.4f + frnd(x + off) * 0.8f)); V::lineTo(99, hz); V::fill(layer ? C565(0x5a4f7c) : C565(0x7b6aa0));
    }
    V::grad(-99, hz, 198, 56, 0x3d6b3a, 0x24401f);
    V::P({-99, 50, -4, hz, 4, hz, 99, 50}, C565(0x2c3140));
    for(int i = 0; i < 5; i++){ float p = 1 - fmodf(t * 0.8f + i / 5.0f, 1), k = p * p, y = hz + k * 56; for(int s = -1; s <= 1; s += 2){ float x = s * (6 + k * 110); V::L({x, y, x, y - 2 - k * 12}, 0.8f + k * 2, C565(0xd8d8d8)); V::E(x, y - 2 - k * 12, 0.6f + k * 1.2f, 0.6f + k * 1.2f, C565(0xff5a3a)); } }
    for(int i = 0; i < 3; i++){ float p = 1 - fmodf(t * 0.55f + i / 3.0f, 1), k = p * p, y = hz + k * 56, x = -(14 + k * 150), s = 1 + k * 10; V::P({x, y - s * 3, x - s, y, x + s, y}, C565(0x1f5a2a)); V::R(x - s * 0.15f, y, s * 0.3f, s * 0.6f, 0, C565(0x5a3a1a)); }
    float v = sinf(t * 9) * 0.5f, X = 0, Y = -2 + v;
    V::P({X - 36, Y + 34, X - 70, 50, X - 10, 50}, C565(0xfff6be), 0.25f); V::P({X + 36, Y + 34, X + 70, 50, X + 10, 50}, C565(0xfff6be), 0.25f);
    for(int s = -1; s <= 1; s += 2){ V::L({X + s * 44, Y - 14, X + s * 54, Y - 18}, 1.6f, C565(0x2a2a2a)); V::R(X + s * 54 - 3, Y - 24, 6, 14, 2, C565(0x222831)); }
    V::R(X - 44, Y - 44, 88, 82, 9, C565(0xd23434)); V::R(X - 44, Y + 10, 88, 28, 9, C565(0xb82c2c));
    V::R(X - 40, Y - 49, 80, 7, 3, C565(0xb02828));
    for(int i = 0; i < 5; i++) V::E(X - 24 + i * 12, Y - 45.5f, 2, 1.6f, (((int)(t * 3)) + i) % 2 ? C565(0xffb21e) : C565(0x7a4a10));
    V::R(X - 37, Y - 38, 74, 6, 2, C565(0x1b1b1b));
    V::R(X - 37, Y - 32, 74, 30, 5, C565(0x1d3a5c));
    face("happy", t, X, Y - 18, 0.34f);
    float wp = sinf(t * 2.4f) * 0.6f; V::L({X - 20, Y - 3, X - 20 + cosf(-1.2f + wp) * 22, Y - 3 + sinf(-1.2f + wp) * 22}, 1.2f, C565(0x111111));
    V::R(X - 34, Y + 2, 68, 18, 3, C565(0xc9ccd6)); for(float x = X - 30; x < X + 31; x += 5) V::R(x, Y + 4, 2.4f, 14, 1, C565(0x6d7383));
    V::T("CMP", X, Y + 11, 6, C565(0xd23434));
    for(int s = -1; s <= 1; s += 2){ V::R(X + s * 39 - 5, Y + 4, 10, 8, 2, C565(0xfff6c2)); V::R(X + s * 39 - 4, Y + 15, 8, 4, 1, C565(0xffb21e)); }
    V::R(X - 44, Y + 22, 88, 10, 3, C565(0x2a2d36)); V::R(X - 12, Y + 24, 24, 6, 1, C565(0xffd23c));
    for(int s = -1; s <= 1; s += 2) V::R(X + s * 34 - 8, Y + 32, 16, 10, 3, C565(0x121318));
  }
  static void isonzo(float t){
    V::grad(-99, -50, 198, 70, 0x4f5d73, 0xd2bea2);
    for(int layer = 0; layer < 2; layer++){
      float amp = layer ? 16 : 30, base = layer ? 4 : -6, off = layer ? 5 : 1;
      V::begin(); V::moveTo(-99, base + 10);
      for(float x = -99; x <= 99; x += 12) V::lineTo(x, base - amp * (0.3f + frnd(x * 0.7f + off)));
      V::lineTo(99, base + 10); V::fill(layer ? C565(0x5f6a5a) : C565(0x7c889a));
      if(!layer) for(float x = -99; x <= 99; x += 12){ float y = base - amp * (0.3f + frnd(x * 0.7f + off)); if(y < base - amp * 0.75f) V::P({x - 4, y + 4, x, y, x + 4, y + 4}, C565(0xeef2f6)); }
    }
    float ex = fmodf(t, 3.1f);
    if(ex < 1.6f){ float p = ex / 1.6f, x = -60 + ((int)(t / 3.1f)) % 3 * 50; if(p < 0.15f) V::E(x, -2, 10 * (p / 0.15f), 7 * (p / 0.15f), C565(0xffd27a)); for(int k = 0; k < 3; k++) V::E(x + (k - 1) * 6, -4 - p * 18 - k * 3, 5 + p * 6, 4 + p * 4, mixC(C565(0x5a5a5a), C565(0xa89a86), p)); }
    float fl = fmodf(t * 0.3f, 1), fx = 50 - fl * 20, fy = -46 + fl * 40; if(fl < 0.9f){ V::Ea(fx, fy, 9, 9, C565(0xfff0aa), 0.25f); V::E(fx, fy, 2, 2, C565(0xfff7c0)); }
    for(float x = -96; x < 99; x += 32) V::L({x, 6, x, 18}, 1.4f, C565(0x3a2e22));
    V::begin(); for(float x = -99; x <= 99; x += 4) V::lineTo(x, 9 + (((int)(x / 4)) % 2 ? 2 : -1)); V::stroke(0.8f, C565(0x2e2a26));
    V::begin(); for(float x = -99; x <= 99; x += 4) V::lineTo(x, 14 + (((int)(x / 4)) % 2 ? -1 : 2)); V::stroke(0.8f, C565(0x2e2a26));
    face("stress", t, 0, 14, 0.62f, (((int)(t * 1.5f)) % 3 - 1) * 6, -2, -1, -1, -1, "sad", false);
    V::save(); V::clipRect(-99, -50, 198, 54); V::E(0, 4, 36, 24, C565(0x68763f)); V::E(-10, -6, 12, 6, C565(0x7c8a52)); V::restore();
    V::R(-46, 1, 92, 6, 3, C565(0x525e2f)); V::R(-2, -22, 4, 8, 1, C565(0x525e2f));
    for(int row = 0; row < 2; row++) for(int i = -1; i < 10; i++){
      float x = -99 + i * 22 + (row ? 11 : 0), y = 44 - row * 9;
      V::E(x, y, 12, 6, row ? C565(0xb8a271) : C565(0xa08c5c)); V::E(x - 2, y - 2, 8, 2.5f, row ? C565(0xc9b588) : C565(0xb29f72)); V::L({x - 6, y, x + 6, y}, 0.6f, C565(0x7d6c44));
    }
  }
  static void lastOfUs(float t){
    V::grad(-99, -50, 198, 100, 0x06090f, 0x101a14);
    static const float ruin[][3] = {{-99, -6, 30}, {-70, -30, 26}, {-40, -14, 22}, {-16, -40, 28}, {16, -22, 24}, {42, -34, 30}, {74, -12, 26}};
    for(int i = 0; i < 7; i++){
      float x = ruin[i][0], top = ruin[i][1], w = ruin[i][2];
      V::P({x, 30, x, top + 6, x + w * 0.3f, top, x + w * 0.5f, top + 8, x + w * 0.75f, top + 2, x + w, top + 10, x + w, 30}, C565(0x141c24));
      for(float wy = top + 12; wy < 26; wy += 7) for(float wx = x + 4; wx < x + w - 4; wx += 7) if(frnd(i * 31 + wy + wx) > 0.75f) V::R(wx, wy, 3, 4, 0, C565(0x2a3a30));
      for(int k = 0; k < 3; k++){ float vx = x + 4 + k * w / 3; V::L({vx, top + 6, vx + sinf(t + k) * 1.5f, top + 14 + frnd(i + k) * 14}, 1.2f, C565(0x2e5a32)); }
    }
    V::R(-99, 30, 198, 20, 0, C565(0x0e1410));
    float flick = sinf(t * 37) > 0.93f ? 0.4f : 1;
    V::P({-40, 6, 20, -9, 20, 22}, C565(0xfff0be), 0.45f * flick); V::P({20, -9, 60, -19, 60, 32, 20, 22}, C565(0xfff0be), 0.3f * flick); V::P({60, -19, 99, -30, 99, 48, 60, 32}, C565(0xfff0be), 0.15f * flick);
    float cx = 92 - fmodf(t * 10, 70), tw = sinf(t * 14) * 0.25f;
    uint16_t body = C565(0x4a4436);
    V::R(cx - 7, 4, 14, 22, 5, body);
    V::L({cx - 6, 8, cx - 14, 18 + sinf(t * 6) * 3}, 3, body); V::L({cx + 4, 9, cx - 10, 14 + cosf(t * 6) * 3}, 3, body);
    V::L({cx - 3, 26, cx - 5, 34}, 3, C565(0x3a3428)); V::L({cx + 3, 26, cx + 4, 34}, 3, C565(0x3a3428));
    V::save(); V::translate(cx, -2); V::rotate(tw);
    V::E(0, 0, 9, 10, C565(0x9a8658));
    for(int k = 0; k < 6; k++){ float a = -PI_F / 2 + (k - 2.5f) * 0.5f; V::P({cosf(a) * 5, sinf(a) * 5, cosf(a - 0.2f) * 15, sinf(a - 0.2f) * 15, cosf(a + 0.25f) * 13, sinf(a + 0.25f) * 13}, C565(0xc2a86a)); }
    for(int k = 0; k < 4; k++) V::E(-4 + k * 2.5f, 2 + (k % 2) * 3, 1.6f, 1.6f, C565(0x5a4a2a));
    V::restore();
    float rx = 110 - fmodf(t * 16, 120); uint16_t rb = C565(0x2f3a2c);
    V::R(rx - 5, 6, 10, 20, 3, rb); V::E(rx, 2, 5.5f, 6, C565(0x3a4636));
    V::L({rx - 3, 10, rx - 14, 8 + sinf(t * 12) * 2.5f}, 2.4f, rb); V::L({rx + 2, 11, rx - 10, 12 - sinf(t * 12) * 2.5f}, 2.4f, rb);
    V::L({rx - 2, 26, rx - 6 + sinf(t * 12) * 3, 34}, 2.4f, rb); V::L({rx + 2, 26, rx + 5 - sinf(t * 12) * 3, 34}, 2.4f, rb);
    for(int i = 0; i < 16; i++){ float x = -96 + fmodf(i * 47 + t * 4, 192), y = -40 + fmodp(i * 29 + sinf(t + i) * 8, 80); if(frnd(i) > 0.3f) V::E(x, y, 0.8f, 0.8f, C565(0x8a9060)); }
    V::R(-99, 18, 62, 26, 8, C565(0x3a2f2a)); V::R(-94, 10, 44, 12, 5, C565(0x4a3b33)); V::R(-90, 12, 16, 7, 2, C565(0x1a2228));
    V::E(-84, 44, 7, 7, C565(0x151515)); V::E(-50, 44, 7, 7, C565(0x151515)); V::R(-80, 26, 10, 6, 1, C565(0x6a4a2a));
    face("surprised", t, -58, -6, 0.4f, 6, 0, -1, -1, -1, "stress");
    sweat(-40, -18 + fmodf(t * 14, 8), DROP);
    V::R(-38, -2, 9, 5, 2, C565(0x555555));
  }
  static void game(const String& g, float t){
    if(g == "minecraft") minecraft(t); else if(g == "sot") seaOfThieves(t); else if(g == "cities") cities(t);
    else if(g == "ets2") euroTruck(t); else if(g == "isonzo") isonzo(t); else if(g == "tlou") lastOfUs(t); else gameAny(t);
  }

  // ---- musica e video (la striscia in basso con titolo e tempo la disegna compMediaFooter) ----
  static void music(float t){
    float beat = fmodf(t * 2, 1), sway = sinf(t * PI_F) * 12, b = -sinf(PI_F * beat) * 5;
    uint16_t col = hueL(t * 45);
    face("music", t, sway, 1 + b, 0.74f, 0, 0, col, -1, col);
    headphones(sway, -2 + b, 33, C565(0x8a8fa3), hueL(t * 45 + 180));
    notes(-72, 6, t, 10, 0, true); notes(74, 2, t + 0.5f, 10, 180, true);
  }
  static void popcorn(float t, float x, float y){
    V::P({x - 16, y - 12, x + 16, y - 12, x + 12, y + 20, x - 12, y + 20}, WH);
    for(int i = -1; i <= 1; i++) V::P({x + i * 10 - 3, y - 12, x + i * 10 + 3, y - 12, x + i * 8 + 2, y + 20, x + i * 8 - 2, y + 20}, C565(0xe23b3b));
    for(int i = 0; i < 6; i++) V::E(x - 12 + i * 5, y - 14 - (i % 2) * 3, 4, 3.4f, C565(0xfff3c4));
    float p = fmodf(t * 1.3f, 1); V::E(x + 6, y - 18 - sinf(p * PI_F) * 14, 2.8f, 2.4f, C565(0xfff3c4));
  }
  static void video(float t){
    bool fl = ((int)(t * 5.5f)) % 5 == 0;
    face("normal", t, -22, -8, 0.76f, -4, 0, fl ? C565(0xd8ecff) : EYE, -1, -1, ((int)(t / 3)) % 3 == 2 ? "surprised" : "normal");
    popcorn(t, 62, 4);
  }

  // ---- OpenStreetMap ----
  static void mapBg(){
    V::R(-99, -50, 198, 100, 0, C565(0xefe9dc));
    V::R(-80, -42, 40, 26, 4, C565(0xb9e3a4)); V::R(30, 12, 50, 30, 4, C565(0xb9e3a4)); V::E(55, -30, 22, 12, C565(0xa9d3f5));
    static const float bl[][4] = {{-30, -40, 22, 16}, {0, -42, 18, 22}, {-82, 2, 26, 18}, {-50, 20, 24, 20}, {-10, 8, 20, 16}, {8, -10, 14, 12}};
    for(auto& b : bl) V::R(b[0], b[1], b[2], b[3], 1.5f, C565(0xd9cfc1));
    V::L({-99, -8, 99, -2}, 5, WH); V::L({-20, -50, -14, 50}, 5, WH); V::L({-99, 34, 99, 26}, 4, C565(0xf7c95c)); V::L({24, -50, 30, 50}, 3, WH);
  }
  static void osmView(float t){
    mapBg();
    float lx = sinf(t * 0.6f) * 40, ly = sinf(t * 0.9f) * 10;
    V::E(lx, ly, 34, 34, C565(0xf8f6f0));
    uint16_t b = C565(0x2d7dd2);
    face("happy", t, lx, ly + 1, 0.48f, 0, 0, b, C565(0x1d4f86), b);
    V::A(lx, ly, 34, 0, TAU_F, 4, C565(0x4a5268));
    V::L({lx + 24, ly + 24, lx + 40, ly + 40}, 6, C565(0x4a5268));
  }
  static void osmEdit(float t){
    mapBg();
    const float pts[4][2] = {{-10, -6}, {40, -10}, {44, 26}, {-6, 30}};
    int n = ((int)(t * 1.2f)) % 6; uint16_t oc = C565(0xff8a2a);
    if(n >= 4) V::P({pts[0][0], pts[0][1], pts[1][0], pts[1][1], pts[2][0], pts[2][1], pts[3][0], pts[3][1]}, oc, 0.4f);
    for(int i = 0; i < min(n, 4); i++) if(i < n - 1 || n >= 4) V::L({pts[i][0], pts[i][1], pts[(i + 1) % 4][0], pts[(i + 1) % 4][1]}, 2.2f, oc);
    for(int i = 0; i < min(n + 1, 4); i++){ V::E(pts[i][0], pts[i][1], 3.4f, 3.4f, WH); V::E(pts[i][0], pts[i][1], 2, 2, oc); }
    const float* c = pts[min(n, 3)];
    V::P({c[0] + 2, c[1] + 2, c[0] + 2, c[1] + 14, c[0] + 6, c[1] + 10, c[0] + 11, c[1] + 10}, C565(0x222222));
    V::R(-99, -50, 64, 44, 6, C565(0x14161f));
    face("work", t, -67, -30, 0.4f, 6, 3);
  }

  // ---- frasi ----
  static const char* speechMood(const String& emo){
    if(emo == "happy") return "happy"; if(emo == "excited") return "excited"; if(emo == "love") return "love";
    if(emo == "surprised") return "surprised"; if(emo == "sad") return "sad"; if(emo == "sleepy") return "sleepy";
    if(emo == "bored") return "bored"; return "normal";
  }
  static void speech(const String& emo, const String& text, float t){
    face(speechMood(emo), t, -64, 8, 0.48f, 0, 0, -1, -1, -1, "talk");
    V::R(-34, -44, 130, 70, 12, WH); V::P({-34, -2, -46, 10, -22, 6}, WH);
    V::done();
    // testo a capo sulla parola: fino a 3 righe da 16 caratteri
    String rest = text, lines[3]; int n = 0;
    while(rest.length() && n < 3){
      if(rest.length() <= 16){ lines[n++] = rest; break; }
      int cut = rest.lastIndexOf(' ', 16); if(cut <= 0) cut = 16;
      lines[n++] = rest.substring(0, cut); rest = rest.substring(cut + (rest[cut] == ' ' ? 1 : 0));
    }
    cc.setTextFont(&fonts::Font2); cc.setTextDatum(middle_center); cc.setTextColor(TFT_BLACK);
    float cx, cy; V::map(31, -9, cx, cy);
    for(int i = 0; i < n; i++) cc.drawString(lines[i].c_str(), lroundf(cx), lroundf(cy + (i - (n - 1) / 2.0f) * 17));
  }

  // ---- meteo ----
  static void sunglasses(float ex, float ey){
    for(int s = -1; s <= 1; s += 2){ V::R(ex + s * 28 - 18, ey - 13, 36, 26, 9, WH); V::R(ex + s * 28 - 16, ey - 11, 32, 22, 7, C565(0x151a28)); V::L({ex + s * 28 - 9, ey + 5, ex + s * 28 - 1, ey - 5}, 1.4f, C565(0x8a93ad)); }
    V::R(ex - 10, ey - 9, 20, 3, 1, WH); V::L({ex - 46, ey - 8, -99, ey - 10}, 2, WH); V::L({ex + 46, ey - 8, 99, ey - 10}, 2, WH);
  }
  static void sunCorner(float t, float x, float y){
    uint16_t c = C565(0xffd23c); V::E(x, y, 8, 8, c);
    for(int i = 0; i < 8; i++){ float a = i * TAU_F / 8 + t * 0.6f; V::L({x + cosf(a) * 11, y + sinf(a) * 11, x + cosf(a) * 15, y + sinf(a) * 15}, 2, c); }
  }
  static void umbrella(float t){
    V::save(); V::clipRect(-99, -50, 198, 34);
    V::E(0, -16, 70, 30, C565(0xe63c46)); V::E(0, -16, 34, 30, C565(0xffd23c)); V::E(0, -16, 12, 30, C565(0xe63c46)); V::restore();
    for(float x = -60; x <= 60; x += 20) V::E(x, -16, 10, 4, BG);
    V::L({0, -46, 0, -50}, 2, C565(0xcfd3e0));
    for(int i = 0; i < 16; i++){ float x = -96 + fmodf(i * 37, 192); bool under = fabsf(x) < 72; float y = under ? -50 + fmodf(t * 60 + i * 9, 6) : -50 + fmodf(t * 90 + i * 17, 100); V::L({x, y, x - 1, y + 5}, 1.4f, C565(0x5ca0ff)); }
    V::L({0, -16, 0, 44}, 2.4f, C565(0xcfd3e0));
  }
  static void flakes(float t, int n){ for(int i = 0; i < n; i++){ float x = -96 + fmodp(i * 37 + sinf(t * 0.8f + i) * 6, 192), y = -50 + fmodf(t * 16 + i * 29, 100); sparkle(x, y, 1.8f, WH); } }
  static void stormFace(float t, float k){   // da tranquillo (k = 0) a spaventato (k = 1)
    uint16_t col = mixC(C565(0x78beff), C565(0xffe678), k);
    float shake = k > 0.6f ? (((int)(t * 30)) % 2 ? 1.5f : -1.5f) * k : 0;
    V::save(); V::translate(shake, 14); V::scale(0.8f);
    for(int sd = -1; sd <= 1; sd += 2){
      float ex = sd * 28, ey = -2 - 2 * (1 - k), w = 26 + 4 * k, h = (34 + 4 * k) * (k < 0.05f ? fblink(t, 0) : 1);
      V::R(ex - w / 2, ey - h / 2, w, fmaxf(2.5f, h), 8 + (w / 2 - 8) * k, col);
      if(k > 0.15f) V::E(ex, ey, 5.5f * k, 5.5f * k, BG);
      brow(ex, ey - h / 2 - 8 - 10 * k, sd * 0.12f * (1 - k), BROW);
    }
    if(k < 0.25f) mouth("normal", t, col); else V::E(0, 26 + k, 2 + 4 * k, 2 + 5.5f * k, col);
    V::restore();
  }
  static void weather(const String& kind, float t){
    if(kind == "sun"){ face("happy", t, 0, 4); sunglasses(0, 2); sunCorner(t, 80, -34); }
    else if(kind == "rain"){ umbrella(t); face("normal", t, 0, 14, 0.8f, 0, 2, C565(0x78beff)); }
    else if(kind == "storm"){
      float ph = fmodf(t, 1.8f), fear = ph < 0.08f ? ph / 0.08f : ph < 0.5f ? 1 : fmaxf(0, 1 - (ph - 0.5f) / 0.7f);
      if(ph < 0.12f) V::R(-99, -50, 198, 100, 0, C565(0xfff8c4));
      umbrella(t); stormFace(t, fear);
      if(ph < 0.25f) V::P({-86, -50, -74, -50, -80, -32, -70, -32, -88, -4, -82, -26, -90, -26}, C565(0xffeb3c));
    }
    else if(kind == "snow"){ flakes(t, 22); face("happy", t, 0, 4, 1, 0, 0, C565(0xe6f5ff)); }
    else if(kind == "cold"){
      face("cold", t, 0, -2, 1, 0, 0, C565(0xaae6ff));
      V::R(-50, 36, 100, 10, 5, C565(0xe64b4b)); for(float x = -44; x < 50; x += 14) V::R(x, 36, 6, 10, 0, C565(0xf2f2f2)); V::R(26, 40, 10, 12, 3, C565(0xe64b4b));
      if(((int)(t * 2.5f)) % 2) V::T("Brrr!", 74, -36, 9, C565(0xaae6ff));
    }
    else if(kind == "hot"){
      face("hot", t, 0, 2, 1, 0, 0, C565(0xff9a3c)); sunCorner(t, 80, -34);
      sweat(-60, -14 + fmodf(t * 18, 26), DROP); sweat(62, -6 + fmodf(t * 18 + 13, 26), DROP);
      for(float x = -96; x < 99; x += 6) V::R(x, 46 + (fmodp(x / 6 + (int)(t * 6), 4) < 2 ? 1 : 0), 4, 1.6f, 0, C565(0xff7828));
    }
    else if(kind == "moon"){
      V::R(-99, -50, 198, 100, 0, C565(0x080a28));
      V::E(78, -32, 10, 10, C565(0xfaf0be)); V::E(83, -36, 9, 9, C565(0x080a28));
      static const float st[][2] = {{-86, -36}, {-50, -20}, {-12, -42}, {40, -30}, {-70, 40}, {66, 38}, {24, 42}};
      for(int i = 0; i < 7; i++){ int ph = ((int)(t * 3 + i)) % 3; if(ph) sparkle(st[i][0], st[i][1], ph == 1 ? 2.5f : 1, i % 2 ? C565(0xfff0a0) : WH); }
      face("sleepy", t, 0, 2);
    }
    else if(kind == "fog"){
      face("normal", t, 0, 2, 1, sinf(t) * 4, 0, C565(0xa0a5af));
      for(int r = 0; r < 5; r++){ float y = -40 + r * 20, off = fmodf(t * 12, 40) * (r % 2 ? 1 : -1); for(float x = -140 + off; x < 99; x += 40) V::R(x, y, 28, 5, 2, C565(0x8c929e)); }
    }
    else {   // nuvoloso
      float x = 110 - fmodf(t * 22, 240);
      face("normal", t, 0, 8, 1, fmaxf(-6, fminf(6, x / 12)), -4);
      const float cl[][3] = {{0, -36, 10}, {12, -42, 12}, {24, -36, 10}, {12, -34, 10}};
      for(auto& c : cl) V::E(x + c[0], c[1], c[2], c[2], C565(0xe1e4ee));
    }
  }

  // ---- feste ----
  static void pumpkin(float x, float y){
    V::E(x, y, 13, 10, C565(0xff8214)); V::L({x, y - 10, x + 1, y - 14}, 3, C565(0x3c9c3c));
    uint16_t l = C565(0xffe65a); V::P({x - 8, y - 1, x - 3, y - 1, x - 5.5f, y - 6}, l); V::P({x + 3, y - 1, x + 8, y - 1, x + 5.5f, y - 6}, l); V::R(x - 6, y + 3, 12, 3, 1, l);
  }
  static void bat(float x, float y, float t){
    bool up = ((int)(t * 7)) % 2; uint16_t c = C565(0xd7c7ff);
    V::E(x, y, 3, 3, c); V::P({x - 2, y, x - 13, up ? y - 6 : y + 4, x - 6, y + 2}, c); V::P({x + 2, y, x + 13, up ? y - 6 : y + 4, x + 6, y + 2}, c);
  }
  static void burst(float x, float y, float p, uint16_t col){
    float r1 = 3 + 16 * p, r2 = 8 + 22 * p;
    for(int i = 0; i < 12; i++){ float a = i * TAU_F / 12; V::L({x + cosf(a) * r1, y + sinf(a) * r1, x + cosf(a) * r2, y + sinf(a) * r2}, 1.6f, col); }
  }
  static void egg(float x, float y, uint16_t a, uint16_t b){
    V::E(x, y, 8, 10.5f, a); for(float dx = -7; dx <= 5; dx += 4) V::P({x + dx, y, x + dx + 2, y - 3, x + dx + 4, y}, b); V::R(x - 7, y + 4, 15, 2, 0, b);
  }
  static void feast(const String& kind, float t){
    if(kind == "halloween"){
      V::R(-99, -50, 198, 100, 0, C565(0x1a0828)); V::E(78, -34, 11, 11, C565(0xfaf0c8));
      for(int i = 0; i < 3; i++) bat(-110 + fmodf(t * 30 + i * 80, 230), -38 + i * 8 + sinf(t * 3 + i) * 4, t + i * 0.1f);
      face("stress", t, 0, 10, 0.82f, 0, 0, C565(0xff8c14), -1, -1, "excited");
      uint16_t hat = C565(0x46207a);
      V::P({-28, -14, 28, -14, 18, -50}, hat); V::R(-54, -17, 108, 7, 3, hat); V::R(-24, -22, 48, 4, 0, C565(0xff8c14));
      pumpkin(-80, 38); pumpkin(80, 38);
    } else if(kind == "natale"){
      flakes(t, 18);
      face("happy", t, 0, 10, 0.85f, 0, 0, C565(0x96e6ff));
      V::P({-48, -18, 40, -18, 64, -44}, C565(0xdc1e28)); V::E(66, -44, 6, 6, WH); V::R(-54, -22, 100, 9, 4, WH);
      uint16_t g = C565(0x1e963c); V::P({-80, -34, -94, -6, -66, -6}, g); V::P({-80, -22, -98, 10, -62, 10}, g); V::R(-83, 10, 6, 7, 0, C565(0x784620));
      for(int i = 0; i < 5; i++) V::E(-90 + fmodf(i * 7, 22), -12 + i * 4.5f, 1.8f, 1.8f, hueL(i * 70 + t * 200)); V::E(-80, -35, 2.6f, 2.6f, C565(0xffdc3c));
    } else if(kind == "capodanno"){
      V::R(-99, -50, 198, 100, 0, C565(0x05051a));
      const float fw[][2] = {{-74, -26}, {74, -22}, {-20, -40}, {40, 34}, {-70, 30}};
      for(int i = 0; i < 5; i++){ float p = fmodf(t * 0.6f + i / 5.0f, 1); if(p < 0.8f) burst(fw[i][0], fw[i][1], p / 0.8f, hueL(i * 72 + t * 40)); }
      face("excited", t, 0, 10, 0.8f, 0, 0, C565(0xffdc50));
      V::P({-12, -16, 12, -16, 0, -46}, C565(0x5096ff)); V::E(0, -46, 3.5f, 3.5f, C565(0xff50a0));
      V::T("Buon anno!", 0, 44, 8, C565(0xffdc50));
    } else if(kind == "befana"){
      V::R(-99, -50, 198, 100, 0, C565(0x0a0c2d));
      float x = 110 - fmodf(t * 26, 240), y = -32 + sinf(t * 5) * 3;
      V::L({x - 22, y + 8, x + 18, y + 3}, 2, C565(0x966432)); for(int d = 0; d < 5; d++) V::L({x + 18, y + 3, x + 28, y - 3 + d * 3}, 1.2f, C565(0xdcb450));
      V::R(x - 7, y - 7, 10, 12, 3, C565(0x3c2850)); V::E(x - 2, y - 9, 3.5f, 3.5f, C565(0xf0c8aa)); V::P({x - 9, y - 11, x + 5, y - 11, x - 3, y - 24}, C565(0xc8b4ff));
      face("surprised", t, 0, 12, 0.75f, fmaxf(-6, fminf(6, x / 14)), -3, C565(0xdcc8ff));
    } else if(kind == "sanvalentino"){
      for(int i = 0; i < 8; i++){ float x = i % 2 ? -86 + fmodf(i * 7, 20) : 86 - fmodf(i * 7, 20), y = 56 - fmodf(t * 24 + i * 29, 120); heart(x, y, 8 + (i % 3) * 3, hueL(335 + i * 5)); }
      face("love", t, 0, 4);
    } else if(kind == "carnevale"){
      for(int i = 0; i < 26; i++){ float x = -96 + fmodp(i * 41 + sinf(t * 2 + i) * 5, 192), y = -50 + fmodf(t * 26 + i * 13, 100); V::R(x, y, 2.6f, 2.6f, 0, hueL(i * 47)); }
      for(int s = -1; s <= 1; s += 2){ V::R(s * 28 - 22, -28, 44, 44, 14, C565(0xf0be28)); V::R(s * 28 - 19, -25, 38, 38, 12, C565(0x8228aa)); }
      V::R(-8, -18, 16, 8, 0, C565(0xf0be28));
      face("happy", t, 0, 4, 0.9f, 0, 0, WH, C565(0xf0be28));
    } else if(kind == "pasqua"){
      for(int s = -1; s <= 1; s += 2){ float ex = s * 20 + (s > 0 ? sinf(t * 4) * 2 : 0); V::E(ex, -38, 8, 16, WH); V::E(ex, -36, 4, 12, C565(0xffaac8)); }
      face("happy", t, 0, 10, 0.82f, 0, 0, C565(0xaaf096));
      egg(-80, 38, C565(0xff78b4), C565(0xffe65a)); egg(80, 38, C565(0x78c8ff), WH); egg(-62, 42, C565(0xb4f078), C565(0xc878ff));
    } else if(kind == "italia"){
      for(float x = -99; x < 99; x += 2){ float dy = sinf(t * 5 + x / 16) * 3; V::R(x, -46 + dy, 2, 92, 0, x < -33 ? C565(0x008c46) : (x < 33 ? C565(0xf0f0f0) : C565(0xd22832))); }
      uint16_t d = C565(0x191923); face("happy", t, 0, 4, 1, 0, 0, d, d);
    } else {   // ferragosto
      face("happy", t, 0, 4); sunglasses(0, 2); sunCorner(t, 80, -34);
      float by = 38 - fabsf(sinf(t * 4.5f)) * 18;
      V::E(-78, by, 10, 10, WH);
      V::begin(); V::moveTo(-78, by); V::arc(-78, by, 10, 0, 1); V::fill(C565(0xe63c46));
      V::begin(); V::moveTo(-78, by); V::arc(-78, by, 10, 2.1f, 3.1f); V::fill(C565(0x2878e6));
      V::begin(); V::moveTo(-78, by); V::arc(-78, by, 10, 4.2f, 5.2f); V::fill(C565(0xfac828));
    }
  }
};

// ---------------------------------------------------------------- funzioni chiamate dalla pagina COMPANION
void scnBegin(){ V::base(); }
void scnEnd(){ V::done(); }
void scnFace(const char* m, float t, float x, float y, float s){ S::face(m, t, x, y, s); }
void scnMood(const char* m, float t){ S::mood(m, t); }
void scnSpeech(const String& emo, const String& text, float t){ S::speech(emo, text, t); }
void scnWeather(const String& kind, float t){ S::weather(kind, t); }
void scnFeast(const String& kind, float t){ S::feast(kind, t); }
void scnMusic(float t){ S::music(t); }
void scnVideo(float t){ S::video(t); }
void scnCode(float t, bool music){ S::code(t, music); }
void scnGame(const String& g, float t){ S::game(g, t); }
void scnOsm(bool edit, float t){ if(edit) S::osmEdit(t); else S::osmView(t); }

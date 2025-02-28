#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>
#include <assert.h>
#include <math.h>
#include <ft2build.h>
#include <freetype/ftlcdfil.h>
#include <freetype/ftoutln.h>
#include FT_FREETYPE_H

#include "renderer.h"
#include "renwindow.h"

#define MAX_GLYPHSET 256
#define MAX_LOADABLE_GLYPHSETS 1024
#define SUBPIXEL_BITMAPS_CACHED 3

static RenWindow window_renderer = {0};
static FT_Library library;

static void* check_alloc(void *ptr) {
  if (!ptr) {
    fprintf(stderr, "Fatal error: memory allocation failed\n");
    exit(EXIT_FAILURE);
  }
  return ptr;
}

/************************* Fonts *************************/

typedef struct {
  unsigned short x0, x1, y0, y1, loaded;
  short bitmap_left, bitmap_top;
  float xadvance;
} GlyphMetric;

typedef struct {
  SDL_Surface* surface;
  GlyphMetric metrics[MAX_GLYPHSET]; 
} GlyphSet;

typedef struct RenFont {
  FT_Face face;
  GlyphSet* sets[SUBPIXEL_BITMAPS_CACHED][MAX_LOADABLE_GLYPHSETS];
  float size, space_advance, tab_advance;
  short max_height, baseline, height;
  ERenFontAntialiasing antialiasing;
  ERenFontHinting hinting;
  unsigned char style;
  char path[0];
} RenFont;

static const char* utf8_to_codepoint(const char *p, unsigned *dst) {
  unsigned res, n;
  switch (*p & 0xf0) {
    case 0xf0 :  res = *p & 0x07;  n = 3;  break;
    case 0xe0 :  res = *p & 0x0f;  n = 2;  break;
    case 0xd0 :
    case 0xc0 :  res = *p & 0x1f;  n = 1;  break;
    default   :  res = *p;         n = 0;  break;
  }
  while (n--) {
    res = (res << 6) | (*(++p) & 0x3f);
  }
  *dst = res;
  return p + 1;
}

static int font_set_load_options(RenFont* font) {
  int load_target = font->antialiasing == FONT_ANTIALIASING_NONE ? FT_LOAD_TARGET_MONO 
    : (font->hinting == FONT_HINTING_SLIGHT ? FT_LOAD_TARGET_LIGHT : FT_LOAD_TARGET_NORMAL);
  int hinting = font->hinting == FONT_HINTING_NONE ? FT_LOAD_NO_HINTING : FT_LOAD_FORCE_AUTOHINT;
  return load_target | hinting;
}

static int font_set_render_options(RenFont* font) {
  if (font->antialiasing == FONT_ANTIALIASING_NONE)
    return FT_RENDER_MODE_MONO;
  if (font->antialiasing == FONT_ANTIALIASING_SUBPIXEL) {
    unsigned char weights[] = { 0x10, 0x40, 0x70, 0x40, 0x10 } ;
    switch (font->hinting) {
      case FONT_HINTING_NONE:   FT_Library_SetLcdFilter(library, FT_LCD_FILTER_NONE); break;
      case FONT_HINTING_SLIGHT:
      case FONT_HINTING_FULL: FT_Library_SetLcdFilterWeights(library, weights); break;
    }
    return FT_RENDER_MODE_LCD;
  } else {
    switch (font->hinting) {
      case FONT_HINTING_NONE:   return FT_RENDER_MODE_NORMAL; break;
      case FONT_HINTING_SLIGHT: return FT_RENDER_MODE_LIGHT; break;
      case FONT_HINTING_FULL:   return FT_RENDER_MODE_LIGHT; break;
    }
  }
  return 0;
}

static int font_set_style(FT_Outline* outline, int x_translation, unsigned char style) {
  FT_Outline_Translate(outline, x_translation, 0 );
  if (style & FONT_STYLE_BOLD)
    FT_Outline_EmboldenXY(outline, 1 << 5, 0);
  if (style & FONT_STYLE_ITALIC) {
    FT_Matrix matrix = { 1 << 16, 1 << 14, 0, 1 << 16 };
    FT_Outline_Transform(outline, &matrix);
  }
  return 0;
}

static void font_load_glyphset(RenFont* font, int idx) {
  unsigned int render_option = font_set_render_options(font), load_option = font_set_load_options(font);
  int bitmaps_cached = font->antialiasing == FONT_ANTIALIASING_SUBPIXEL ? SUBPIXEL_BITMAPS_CACHED : 1;
  unsigned int byte_width = font->antialiasing == FONT_ANTIALIASING_SUBPIXEL ? 3 : 1;
  for (int j = 0, pen_x = 0; j < bitmaps_cached; ++j) {
    GlyphSet* set = check_alloc(calloc(1, sizeof(GlyphSet)));
    font->sets[j][idx] = set;
    for (int i = 0; i < MAX_GLYPHSET; ++i) {
      int glyph_index = FT_Get_Char_Index(font->face, i + idx * MAX_GLYPHSET);
      if (!glyph_index || FT_Load_Glyph(font->face, glyph_index, load_option | FT_LOAD_BITMAP_METRICS_ONLY)
        || font_set_style(&font->face->glyph->outline, j * (64 / SUBPIXEL_BITMAPS_CACHED), font->style) || FT_Render_Glyph(font->face->glyph, render_option)) {
        continue;
      }
      FT_GlyphSlot slot = font->face->glyph;
      int glyph_width = slot->bitmap.width / byte_width;
      if (font->antialiasing == FONT_ANTIALIASING_NONE)
        glyph_width *= 8;
      set->metrics[i] = (GlyphMetric){ pen_x, pen_x + glyph_width, 0, slot->bitmap.rows, true, slot->bitmap_left, slot->bitmap_top, (slot->advance.x + slot->lsb_delta - slot->rsb_delta) / 64.0f};
      pen_x += glyph_width;
      font->max_height = slot->bitmap.rows > font->max_height ? slot->bitmap.rows : font->max_height;
      // In order to fix issues with monospacing; we need the unhinted xadvance; as FreeType doesn't correctly report the hinted advance for spaces on monospace fonts (like RobotoMono). See #843.
      if (!glyph_index || FT_Load_Glyph(font->face, glyph_index, (load_option | FT_LOAD_BITMAP_METRICS_ONLY | FT_LOAD_NO_HINTING) & ~FT_LOAD_FORCE_AUTOHINT)
        || font_set_style(&font->face->glyph->outline, j * (64 / SUBPIXEL_BITMAPS_CACHED), font->style) || FT_Render_Glyph(font->face->glyph, render_option)) {
        continue;
      }
      slot = font->face->glyph;
      set->metrics[i].xadvance = slot->advance.x / 64.0f;
    }
    if (pen_x == 0)
      continue;
    set->surface = check_alloc(SDL_CreateRGBSurface(0, pen_x, font->max_height, font->antialiasing == FONT_ANTIALIASING_SUBPIXEL ? 24 : 8, 0, 0, 0, 0));
    unsigned char* pixels = set->surface->pixels;
    for (int i = 0; i < MAX_GLYPHSET; ++i) {
      int glyph_index = FT_Get_Char_Index(font->face, i + idx * MAX_GLYPHSET);
      if (!glyph_index || FT_Load_Glyph(font->face, glyph_index, load_option))
        continue;
      FT_GlyphSlot slot = font->face->glyph;
      font_set_style(&slot->outline, (64 / bitmaps_cached) * j, font->style);
      if (FT_Render_Glyph(slot, render_option))
        continue; 
      for (int line = 0; line < slot->bitmap.rows; ++line) {
        int target_offset = set->surface->pitch * line + set->metrics[i].x0 * byte_width;
        int source_offset = line * slot->bitmap.pitch;
        if (font->antialiasing == FONT_ANTIALIASING_NONE) {
          for (int column = 0; column < slot->bitmap.width; ++column) {
            int current_source_offset = source_offset + (column / 8);
            int source_pixel = slot->bitmap.buffer[current_source_offset];
            pixels[++target_offset] = ((source_pixel >> (7 - (column % 8))) & 0x1) << 7;
          }
        } else
          memcpy(&pixels[target_offset], &slot->bitmap.buffer[source_offset], slot->bitmap.width);
      }
    }
  }
}

static GlyphSet* font_get_glyphset(RenFont* font, unsigned int codepoint, int subpixel_idx) {
  int idx = (codepoint >> 8) % MAX_LOADABLE_GLYPHSETS;
  if (!font->sets[font->antialiasing == FONT_ANTIALIASING_SUBPIXEL ? subpixel_idx : 0][idx])
    font_load_glyphset(font, idx);
  return font->sets[font->antialiasing == FONT_ANTIALIASING_SUBPIXEL ? subpixel_idx : 0][idx];
}

static RenFont* font_group_get_glyph(GlyphSet** set, GlyphMetric** metric, RenFont** fonts, unsigned int codepoint, int bitmap_index) {
  if (bitmap_index < 0)
    bitmap_index += SUBPIXEL_BITMAPS_CACHED;
  for (int i = 0; i < FONT_FALLBACK_MAX && fonts[i]; ++i) {
    *set = font_get_glyphset(fonts[i], codepoint, bitmap_index);
    *metric = &(*set)->metrics[codepoint % 256];
    if ((*metric)->loaded || codepoint < 0xFF)
      return fonts[i];
  }
  if (!(*metric)->loaded && codepoint > 0xFF && codepoint != 0x25A1)
    return font_group_get_glyph(set, metric, fonts, 0x25A1, bitmap_index);
  return fonts[0];
}

RenFont* ren_font_load(const char* path, float size, ERenFontAntialiasing antialiasing, ERenFontHinting hinting, unsigned char style) {
  FT_Face face;
  if (FT_New_Face( library, path, 0, &face))
    return NULL;
  const int surface_scale = renwin_surface_scale(&window_renderer);
  if (FT_Set_Pixel_Sizes(face, 0, (int)(size*surface_scale)))
    goto failure;
  int len = strlen(path);
  RenFont* font = check_alloc(calloc(1, sizeof(RenFont) + len + 1));
  strcpy(font->path, path);
  font->face = face;
  font->size = size;
  font->height = (short)((face->height / (float)face->units_per_EM) * font->size);
  font->baseline = (short)((face->bbox.yMax / (float)face->units_per_EM) * font->size);
  font->antialiasing = antialiasing;
  font->hinting = hinting;
  font->style = style;
  font->space_advance = font_get_glyphset(font, ' ', 0)->metrics[' '].xadvance;
  font->tab_advance = font->space_advance * 2;
  return font;
  failure:  
  FT_Done_Face(face);
  return NULL;
}

RenFont* ren_font_copy(RenFont* font, float size) {
  return ren_font_load(font->path, size, font->antialiasing, font->hinting, font->style);
}

void ren_font_free(RenFont* font) {
  for (int i = 0; i < SUBPIXEL_BITMAPS_CACHED; ++i) {
    for (int j = 0; j < MAX_GLYPHSET; ++j) {
      if (font->sets[i][j]) {
        if (font->sets[i][j]->surface)
          SDL_FreeSurface(font->sets[i][j]->surface);
        free(font->sets[i][j]);
      }
    }
  }
  FT_Done_Face(font->face);
  free(font);
}

void ren_font_group_set_tab_size(RenFont **fonts, int n) {
  for (int j = 0; j < FONT_FALLBACK_MAX && fonts[j]; ++j) {
    for (int i = 0; i < (fonts[j]->antialiasing == FONT_ANTIALIASING_SUBPIXEL ? SUBPIXEL_BITMAPS_CACHED : 1); ++i) 
      font_get_glyphset(fonts[j], '\t', i)->metrics['\t'].xadvance = fonts[j]->space_advance * n;
  }
}

int ren_font_group_get_tab_size(RenFont **fonts) {
  return font_get_glyphset(fonts[0], '\t', 0)->metrics['\t'].xadvance / fonts[0]->space_advance;
}

float ren_font_group_get_size(RenFont **fonts) {
  return fonts[0]->size;
}
int ren_font_group_get_height(RenFont **fonts) {
  return fonts[0]->height;
}

float ren_font_group_get_width(RenFont **fonts, const char *text) {
  float width = 0;
  const char* end = text + strlen(text);
  GlyphMetric* metric = NULL; GlyphSet* set = NULL;
  while (text < end) {
    unsigned int codepoint;
    text = utf8_to_codepoint(text, &codepoint);
    RenFont* font = font_group_get_glyph(&set, &metric, fonts, codepoint, 0);
    width += (!font || metric->xadvance) ? metric->xadvance : fonts[0]->space_advance;
  }
  const int surface_scale = renwin_surface_scale(&window_renderer);
  return width / surface_scale;
}

float ren_draw_text(RenFont **fonts, const char *text, float x, int y, RenColor color) {
  SDL_Surface *surface = renwin_get_surface(&window_renderer);
  const RenRect clip = window_renderer.clip;

  const int surface_scale = renwin_surface_scale(&window_renderer);
  float pen_x = x * surface_scale;
  y *= surface_scale;
  int bytes_per_pixel = surface->format->BytesPerPixel;
  const char* end = text + strlen(text);
  unsigned char* destination_pixels = surface->pixels;
  int clip_end_x = clip.x + clip.width, clip_end_y = clip.y + clip.height;
  
  while (text < end) {
    unsigned int codepoint, r, g, b;
    text = utf8_to_codepoint(text, &codepoint);
    GlyphSet* set = NULL; GlyphMetric* metric = NULL; 
    RenFont* font = font_group_get_glyph(&set, &metric, fonts, codepoint, (int)(fmod(pen_x, 1.0) * SUBPIXEL_BITMAPS_CACHED));
    int start_x = floor(pen_x) + metric->bitmap_left;
    int end_x = (metric->x1 - metric->x0) + start_x;
    int glyph_end = metric->x1, glyph_start = metric->x0;
    if (!metric->loaded && codepoint > 0xFF)
      ren_draw_rect((RenRect){ start_x + 1, y, font->space_advance - 1, ren_font_group_get_height(fonts) }, color);
    if (set->surface && color.a > 0 && end_x >= clip.x && start_x < clip_end_x) {
      unsigned char* source_pixels = set->surface->pixels;
      for (int line = metric->y0; line < metric->y1; ++line) {
        int target_y = line + y - metric->bitmap_top + font->baseline * surface_scale;
        if (target_y < clip.y)
          continue;
        if (target_y >= clip_end_y)
          break;
        if (start_x + (glyph_end - glyph_start) >= clip_end_x)
          glyph_end = glyph_start + (clip_end_x - start_x);
        else if (start_x < clip.x) {
          int offset = clip.x - start_x;
          start_x += offset;
          glyph_start += offset;
        }
        unsigned int* destination_pixel = (unsigned int*)&destination_pixels[surface->pitch * target_y + start_x * bytes_per_pixel];
        unsigned char* source_pixel = &source_pixels[line * set->surface->pitch + glyph_start * (font->antialiasing == FONT_ANTIALIASING_SUBPIXEL ? 3 : 1)];
        for (int x = glyph_start; x < glyph_end; ++x) {
          unsigned int destination_color = *destination_pixel;
          SDL_Color dst = { (destination_color & surface->format->Rmask) >> surface->format->Rshift, (destination_color & surface->format->Gmask) >> surface->format->Gshift, (destination_color & surface->format->Bmask) >> surface->format->Bshift, (destination_color & surface->format->Amask) >> surface->format->Ashift };
          SDL_Color src = { *(font->antialiasing == FONT_ANTIALIASING_SUBPIXEL ? source_pixel++ : source_pixel), *(font->antialiasing == FONT_ANTIALIASING_SUBPIXEL ? source_pixel++ : source_pixel), *source_pixel++ };
          r = (color.r * src.r * color.a + dst.r * (65025 - src.r * color.a) + 32767) / 65025;
          g = (color.g * src.g * color.a + dst.g * (65025 - src.g * color.a) + 32767) / 65025;
          b = (color.b * src.b * color.a + dst.b * (65025 - src.b * color.a) + 32767) / 65025;
          *destination_pixel++ = dst.a << surface->format->Ashift | r << surface->format->Rshift | g << surface->format->Gshift | b << surface->format->Bshift;
        }
      }
    }
    pen_x += metric->xadvance ? metric->xadvance : font->space_advance;
  }
  if (fonts[0]->style & FONT_STYLE_UNDERLINE)
    ren_draw_rect((RenRect){ x, y / surface_scale + ren_font_group_get_height(fonts) - 1, (pen_x - x) / surface_scale, 1 }, color);
  return pen_x / surface_scale;
}

/******************* Rectangles **********************/
static inline RenColor blend_pixel(RenColor dst, RenColor src) {
  int ia = 0xff - src.a;
  dst.r = ((src.r * src.a) + (dst.r * ia)) >> 8;
  dst.g = ((src.g * src.a) + (dst.g * ia)) >> 8;
  dst.b = ((src.b * src.a) + (dst.b * ia)) >> 8;
  return dst;
}

void ren_draw_rect(RenRect rect, RenColor color) {
  if (color.a == 0) { return; }

  const int surface_scale = renwin_surface_scale(&window_renderer);

  /* transforms coordinates in pixels. */
  rect.x      *= surface_scale;
  rect.y      *= surface_scale;
  rect.width  *= surface_scale;
  rect.height *= surface_scale;

  const RenRect clip = window_renderer.clip;
  int x1 = rect.x < clip.x ? clip.x : rect.x;
  int y1 = rect.y < clip.y ? clip.y : rect.y;
  int x2 = rect.x + rect.width;
  int y2 = rect.y + rect.height;
  x2 = x2 > clip.x + clip.width ? clip.x + clip.width : x2;
  y2 = y2 > clip.y + clip.height ? clip.y + clip.height : y2;

  SDL_Surface *surface = renwin_get_surface(&window_renderer);
  uint32_t *d = surface->pixels;
  d += x1 + y1 * surface->w;
  int dr = surface->w - (x2 - x1);
  if (color.a == 0xff) {
    uint32_t translated = SDL_MapRGB(surface->format, color.r, color.g, color.b);
    SDL_Rect rect = { x1, y1, x2 - x1, y2 - y1 };
    SDL_FillRect(surface, &rect, translated);
  } else {
    RenColor current_color;
    RenColor blended_color;
    for (int j = y1; j < y2; j++) {
      for (int i = x1; i < x2; i++, d++)
      {
        SDL_GetRGB(*d, surface->format, &current_color.r, &current_color.g, &current_color.b);
        blended_color = blend_pixel(current_color, color);
        *d = SDL_MapRGB(surface->format, blended_color.r, blended_color.g, blended_color.b);
      }
      d += dr;
    }
  }
}

/*************** Window Management ****************/
void ren_free_window_resources() {
  renwin_free(&window_renderer);
}

void ren_init(SDL_Window *win) {
  assert(win);
  int error = FT_Init_FreeType( &library );
  if ( error ) {
    fprintf(stderr, "internal font error when starting the application\n");
    return;
  }
  window_renderer.window = win;
  renwin_init_surface(&window_renderer);
  renwin_clip_to_surface(&window_renderer);
}


void ren_resize_window() {
  renwin_resize_surface(&window_renderer);
}


void ren_update_rects(RenRect *rects, int count) {
  static bool initial_frame = true;
  if (initial_frame) {
    renwin_show_window(&window_renderer);
    initial_frame = false;
  }
  renwin_update_rects(&window_renderer, rects, count);
}


void ren_set_clip_rect(RenRect rect) {
  renwin_set_clip_rect(&window_renderer, rect);
}


void ren_get_size(int *x, int *y) {
  RenWindow *ren = &window_renderer;
  const int scale = renwin_surface_scale(ren);
  SDL_Surface *surface = renwin_get_surface(ren);
  *x = surface->w / scale;
  *y = surface->h / scale;
}


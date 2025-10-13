#define _GNU_SOURCE
#include <GL/glx.h>
#include <SDL2/SDL.h>
#include <SDL2/SDL_image.h>
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <dirent.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define VIDEO_MAX_FRAMES 300
#define GIF_FRAME_DELAY 100

typedef struct {
  char **path;
  unsigned size;
} Paths;

typedef struct {
  SDL_Texture **texture;
  unsigned size;
} Textures;

typedef struct {
  char *base;
  int speed;
  SDL_Rect rect;
} ViewConfig;

typedef struct {
  bool create_desktop;
  int view_count;
  ViewConfig *views;
} Config;

typedef struct {
  Uint32 *delays;
  SDL_Texture **frames;
  unsigned count;
} Animated;

typedef struct {
  SDL_Surface **surfaces;
  Uint32 *delays;
  unsigned count;
  bool is_animated;
  bool is_gif;
} MediaData;

typedef struct View {
  bool is_anim;
  bool is_gif;
  Animated anim;
  SDL_Texture *static_tex;
  SDL_Rect dst;
  unsigned idx;
  Uint32 last_ts;
  int speed;
  struct View *next;
} View;

typedef struct {
  Display *x11d;
  SDL_Window *window;
  SDL_Renderer *renderer;
} Video;

typedef struct {
  const ViewConfig *vc;
  MediaData *result;
  int index;
} ThreadData;

static void die(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vfprintf(stderr, fmt, ap);
  va_end(ap);
  exit(1);
}

static char *strdup_safe(const char *s) {
  size_t n = strlen(s) + 1;
  char *d = malloc(n);
  memcpy(d, s, n);
  return d;
}

static bool has_ext(const char *fn, const char *ext) {
  const char *e = strrchr(fn, '.');
  return e && strcasecmp(e, ext) == 0;
}
static bool is_video(const char *fn) {
  const char *exts[] = {".mp4", ".webm", ".mkv", ".avi", ".mov", NULL};
  for (int i = 0; exts[i]; i++)
    if (has_ext(fn, exts[i]))
      return true;
  return false;
}
static bool is_gif(const char *fn) { return has_ext(fn, ".gif"); }
static bool is_image(const char *fn) {
  const char *exts[] = {".png",  ".jpg",  ".jpeg", ".bmp",
                        ".tiff", ".webp", ".tga",  NULL};
  for (int i = 0; exts[i]; i++)
    if (has_ext(fn, exts[i]))
      return true;
  return false;
}

static char **collect_media(const char *base, int *out_count) {
  struct stat st;
  if (stat(base, &st) == 0 && S_ISREG(st.st_mode)) {
    char **a = malloc(sizeof(char *));
    a[0] = strdup_safe(base);
    *out_count = 1;
    return a;
  }
  DIR *d = opendir(base);
  if (!d)
    die("Failed to open '%s'\n", base);
  char **a = NULL;
  int cap = 0, cnt = 0;
  struct dirent *e;
  while ((e = readdir(d))) {
    if (e->d_name[0] == '.')
      continue;
    char path[4096];
    snprintf(path, sizeof(path), "%s/%s", base, e->d_name);
    if (is_video(e->d_name) || is_gif(e->d_name) || is_image(e->d_name)) {
      if (cnt == cap) {
        cap = cap ? cap * 2 : 8;
        a = realloc(a, cap * sizeof(*a));
      }
      a[cnt++] = strdup_safe(path);
    }
  }
  closedir(d);
  *out_count = cnt;
  return a;
}

static SDL_Texture *load_image(SDL_Renderer *R, const char *p) {
  SDL_Surface *s = IMG_Load(p);
  if (!s)
    die("IMG_Load %s: %s\n", p, IMG_GetError());
  SDL_Texture *t = SDL_CreateTextureFromSurface(R, s);
  SDL_FreeSurface(s);
  if (!t)
    die("SDL_CreateTexture: %s\n", SDL_GetError());
  return t;
}

static int load_gif_surfaces(const char *p, SDL_Surface ***out,
                             Uint32 **delays) {
  IMG_Animation *a = IMG_LoadAnimation(p);
  if (!a)
    return 0;
  *out = malloc(a->count * sizeof(SDL_Surface *));
  *delays = malloc(a->count * sizeof(Uint32));
  for (int i = 0; i < a->count; i++) {
    (*out)[i] = SDL_ConvertSurface(a->frames[i], a->frames[i]->format, 0);
    (*delays)[i] = GIF_FRAME_DELAY;
  }
  int c = a->count;
  IMG_FreeAnimation(a);
  return c;
}

static int load_video_surfaces(const char *p, SDL_Surface ***out,
                               Uint32 **delays) {
  avformat_network_init();
  AVFormatContext *fmt = NULL;
  if (avformat_open_input(&fmt, p, NULL, NULL) < 0)
    return 0;
  if (avformat_find_stream_info(fmt, NULL) < 0) {
    avformat_close_input(&fmt);
    return 0;
  }
  int vs = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
  AVCodecParameters *cp = fmt->streams[vs]->codecpar;
  const AVCodec *cod = avcodec_find_decoder(cp->codec_id);
  AVCodecContext *dec = avcodec_alloc_context3(cod);
  avcodec_parameters_to_context(dec, cp);
  avcodec_open2(dec, cod, NULL);
  AVFrame *f = av_frame_alloc(), *rgb = av_frame_alloc();
  struct SwsContext *sws = NULL;
  *out = calloc(VIDEO_MAX_FRAMES, sizeof(SDL_Surface *));
  *delays = calloc(VIDEO_MAX_FRAMES, sizeof(Uint32));
  int cnt = 0;
  AVPacket pkt;
  av_init_packet(&pkt);
  double fps = av_q2d(fmt->streams[vs]->avg_frame_rate);
  Uint32 d = fps > 0 ? (Uint32)(1000.0 / fps) : 33;
  while (av_read_frame(fmt, &pkt) >= 0 && cnt < VIDEO_MAX_FRAMES) {
    if (pkt.stream_index == vs && avcodec_send_packet(dec, &pkt) >= 0 &&
        avcodec_receive_frame(dec, f) == 0) {
      if (!sws)
        sws = sws_getContext(f->width, f->height, dec->pix_fmt, f->width,
                             f->height, AV_PIX_FMT_RGB24, SWS_BILINEAR, NULL,
                             NULL, NULL);
      int bs =
          av_image_get_buffer_size(AV_PIX_FMT_RGB24, f->width, f->height, 1);
      uint8_t *buf = av_malloc(bs);
      av_image_fill_arrays(rgb->data, rgb->linesize, buf, AV_PIX_FMT_RGB24,
                           f->width, f->height, 1);
      sws_scale(sws, (const uint8_t *const *)f->data, f->linesize, 0, f->height,
                rgb->data, rgb->linesize);
      SDL_Surface *surf = SDL_CreateRGBSurfaceFrom(buf, f->width, f->height, 24,
                                                   rgb->linesize[0], 0x0000FF,
                                                   0x00FF00, 0xFF0000, 0);
      if (surf) {
        (*out)[cnt] = SDL_ConvertSurface(surf, surf->format, 0);
        (*delays)[cnt] = d;
        SDL_FreeSurface(surf);
        cnt++;
      }
      av_free(buf);
    }
    av_packet_unref(&pkt);
  }
  if (sws)
    sws_freeContext(sws);
  av_frame_free(&f);
  av_frame_free(&rgb);
  avcodec_free_context(&dec);
  avformat_close_input(&fmt);
  return cnt;
}

static int load_gif_frames(SDL_Renderer *R, const char *p, SDL_Texture ***out,
                           Uint32 **delays) {
  IMG_Animation *a = IMG_LoadAnimation(p);
  if (!a)
    return 0;
  *out = malloc(a->count * sizeof(SDL_Texture *));
  *delays = malloc(a->count * sizeof(Uint32));
  for (int i = 0; i < a->count; i++) {
    (*out)[i] = SDL_CreateTextureFromSurface(R, a->frames[i]);
    (*delays)[i] = GIF_FRAME_DELAY;
  }
  int c = a->count;
  IMG_FreeAnimation(a);
  return c;
}

static int load_video_frames(const char *p, SDL_Renderer *R, SDL_Texture ***out,
                             Uint32 **delays) {
  avformat_network_init();
  AVFormatContext *fmt = NULL;
  if (avformat_open_input(&fmt, p, NULL, NULL) < 0)
    return 0;
  if (avformat_find_stream_info(fmt, NULL) < 0) {
    avformat_close_input(&fmt);
    return 0;
  }
  int vs = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
  AVCodecParameters *cp = fmt->streams[vs]->codecpar;
  const AVCodec *cod = avcodec_find_decoder(cp->codec_id);
  AVCodecContext *dec = avcodec_alloc_context3(cod);
  avcodec_parameters_to_context(dec, cp);
  avcodec_open2(dec, cod, NULL);
  AVFrame *f = av_frame_alloc(), *rgb = av_frame_alloc();
  struct SwsContext *sws = NULL;
  *out = calloc(VIDEO_MAX_FRAMES, sizeof(SDL_Texture *));
  *delays = calloc(VIDEO_MAX_FRAMES, sizeof(Uint32));
  int cnt = 0;
  AVPacket pkt;
  av_init_packet(&pkt);
  double fps = av_q2d(fmt->streams[vs]->avg_frame_rate);
  Uint32 d = fps > 0 ? (Uint32)(1000.0 / fps) : 33;
  while (av_read_frame(fmt, &pkt) >= 0 && cnt < VIDEO_MAX_FRAMES) {
    if (pkt.stream_index == vs && avcodec_send_packet(dec, &pkt) >= 0 &&
        avcodec_receive_frame(dec, f) == 0) {
      if (!sws)
        sws = sws_getContext(f->width, f->height, dec->pix_fmt, f->width,
                             f->height, AV_PIX_FMT_RGB24, SWS_BILINEAR, NULL,
                             NULL, NULL);
      int bs =
          av_image_get_buffer_size(AV_PIX_FMT_RGB24, f->width, f->height, 1);
      uint8_t *buf = av_malloc(bs);
      av_image_fill_arrays(rgb->data, rgb->linesize, buf, AV_PIX_FMT_RGB24,
                           f->width, f->height, 1);
      sws_scale(sws, (const uint8_t *const *)f->data, f->linesize, 0, f->height,
                rgb->data, rgb->linesize);
      SDL_Surface *surf = SDL_CreateRGBSurfaceFrom(buf, f->width, f->height, 24,
                                                   rgb->linesize[0], 0x0000FF,
                                                   0x00FF00, 0xFF0000, 0);
      if (surf) {
        (*out)[cnt] = SDL_CreateTextureFromSurface(R, surf);
        (*delays)[cnt] = d;
        SDL_FreeSurface(surf);
        cnt++;
      }
      av_free(buf);
    }
    av_packet_unref(&pkt);
  }
  if (sws)
    sws_freeContext(sws);
  av_frame_free(&f);
  av_frame_free(&rgb);
  avcodec_free_context(&dec);
  avformat_close_input(&fmt);
  return cnt;
}

static Window create_desktop(Display *x11d) {
  Window root = RootWindow(x11d, DefaultScreen(x11d));
  int w = DisplayWidth(x11d, 0), h = DisplayHeight(x11d, 0);
  int vis_attribs[] = {GLX_RGBA, GLX_DEPTH_SIZE, 24, GLX_DOUBLEBUFFER, None};
  XVisualInfo *vi = glXChooseVisual(x11d, 0, vis_attribs);
  if (!vi)
    die("No visual\n");
  Colormap cmap = XCreateColormap(x11d, root, vi->visual, AllocNone);
  XSetWindowAttributes swa = {
      .colormap = cmap, .override_redirect = True, .border_pixel = 0};
  Window win = XCreateWindow(
      x11d, root, 0, 0, w, h, 0, vi->depth, InputOutput, vi->visual,
      CWColormap | CWOverrideRedirect | CWBorderPixel, &swa);
  Atom type = XInternAtom(x11d, "_NET_WM_WINDOW_TYPE", False),
       desktop = XInternAtom(x11d, "_NET_WM_WINDOW_TYPE_DESKTOP", False);
  XChangeProperty(x11d, win, type, XA_ATOM, 32, PropModeReplace,
                  (unsigned char *)&desktop, 1);
  Atom state = XInternAtom(x11d, "_NET_WM_STATE", False),
       below = XInternAtom(x11d, "_NET_WM_STATE_BELOW", False);
  XChangeProperty(x11d, win, state, XA_ATOM, 32, PropModeAppend,
                  (unsigned char *)&below, 1);
  XMapWindow(x11d, win);
  XLowerWindow(x11d, win);
  XSync(x11d, False);
  return win;
}

static Video Setup(bool cd) {
  Video v;
  v.x11d = XOpenDisplay(NULL);
  if (!v.x11d)
    die("XOpenDisplay failed\n");
  Window xw =
      cd ? create_desktop(v.x11d) : RootWindow(v.x11d, DefaultScreen(v.x11d));
  SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS | SDL_INIT_TIMER);
  IMG_Init(IMG_INIT_PNG | IMG_INIT_JPG | IMG_INIT_TIF | IMG_INIT_WEBP);
  v.window = SDL_CreateWindowFrom((void *)xw);
  v.renderer = SDL_CreateRenderer(
      v.window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
  return v;
}

static void teardown(Video *v) {
  SDL_DestroyRenderer(v->renderer);
  SDL_DestroyWindow(v->window);
  XCloseDisplay(v->x11d);
  IMG_Quit();
  SDL_Quit();
}

static Config Parse(int argc, char **argv) {
  Config cfg = {0};
  cfg.create_desktop = (argc > 1 && strcmp(argv[1], "--compositor") == 0);
  int off = cfg.create_desktop ? 2 : 1;
  int args = argc - off;
  if (args < 6 || args % 6 != 0)
    die("Usage: %s [--compositor] PATH SPEED X Y W H [...]\n", argv[0]);
  cfg.view_count = args / 6;
  cfg.views = calloc(cfg.view_count, sizeof(ViewConfig));
  for (int i = 0; i < cfg.view_count; i++) {
    int idx = off + i * 6;
    cfg.views[i].base = argv[idx];
    cfg.views[i].speed = atoi(argv[idx + 1]);
    if (cfg.views[i].speed <= 0)
      cfg.views[i].speed = 1;
    cfg.views[i].rect.x = atoi(argv[idx + 2]);
    cfg.views[i].rect.y = atoi(argv[idx + 3]);
    cfg.views[i].rect.w = atoi(argv[idx + 4]);
    cfg.views[i].rect.h = atoi(argv[idx + 5]);
  }
  return cfg;
}

static void *LoadMediaThread(void *arg) {
  ThreadData *td = (ThreadData *)arg;
  const ViewConfig *vc = td->vc;
  MediaData *md = calloc(1, sizeof(MediaData));

  int n;
  char **arr = collect_media(vc->base, &n);
  if (n > 0) {
    if (is_image(arr[0])) {
      SDL_Surface *s = IMG_Load(arr[0]);
      if (s) {
        md->surfaces = malloc(sizeof(SDL_Surface *));
        md->surfaces[0] = s;
        md->count = 1;
        md->is_animated = false;
      }
    } else {
      md->is_animated = true;
      md->is_gif = is_gif(arr[0]);
      if (md->is_gif)
        md->count = load_gif_surfaces(arr[0], &md->surfaces, &md->delays);
      else
        md->count = load_video_surfaces(arr[0], &md->surfaces, &md->delays);
    }
  }

  for (int i = 0; i < n; i++)
    free(arr[i]);
  free(arr);

  td->result = md;
  return NULL;
}

static View *InitView(const ViewConfig *vc, Video *vid) {
  View *v = calloc(1, sizeof(*v));
  v->dst = vc->rect;
  v->last_ts = SDL_GetTicks();
  v->speed = vc->speed;
  int n;
  char **arr = collect_media(vc->base, &n);
  if (n > 0) {
    if (is_image(arr[0])) {
      v->static_tex = load_image(vid->renderer, arr[0]);
    } else {
      v->is_anim = true;
      v->is_gif = is_gif(arr[0]);
      if (v->is_gif)
        v->anim.count = load_gif_frames(vid->renderer, arr[0], &v->anim.frames,
                                        &v->anim.delays);
      else
        v->anim.count = load_video_frames(arr[0], vid->renderer,
                                          &v->anim.frames, &v->anim.delays);
    }
  }
  for (int i = 0; i < n; i++)
    free(arr[i]);
  free(arr);
  return v;
}

static View *MediaDataToView(const ViewConfig *vc, MediaData *md, Video *vid) {
  View *v = calloc(1, sizeof(*v));
  v->dst = vc->rect;
  v->last_ts = SDL_GetTicks();
  v->speed = vc->speed;

  if (md && md->count > 0) {
    if (!md->is_animated) {
      v->static_tex =
          SDL_CreateTextureFromSurface(vid->renderer, md->surfaces[0]);
    } else {
      v->is_anim = true;
      v->is_gif = md->is_gif;
      v->anim.count = md->count;
      v->anim.frames = malloc(md->count * sizeof(SDL_Texture *));
      v->anim.delays = malloc(md->count * sizeof(Uint32));

      for (unsigned i = 0; i < md->count; i++) {
        v->anim.frames[i] =
            SDL_CreateTextureFromSurface(vid->renderer, md->surfaces[i]);
        v->anim.delays[i] = md->delays[i];
      }
    }
  }

  return v;
}

static void FreeMediaData(MediaData *md) {
  if (!md)
    return;
  if (md->surfaces) {
    for (unsigned i = 0; i < md->count; i++) {
      if (md->surfaces[i])
        SDL_FreeSurface(md->surfaces[i]);
    }
    free(md->surfaces);
  }
  if (md->delays)
    free(md->delays);
  free(md);
}

static View *LoadViews(const Config *cfg, Video *vid) {
  pthread_t *threads = malloc(cfg->view_count * sizeof(pthread_t));
  ThreadData *thread_data = malloc(cfg->view_count * sizeof(ThreadData));

  for (int i = 0; i < cfg->view_count; i++) {
    thread_data[i].vc = &cfg->views[i];
    thread_data[i].result = NULL;
    thread_data[i].index = i;
    pthread_create(&threads[i], NULL, LoadMediaThread, &thread_data[i]);
  }

  for (int i = 0; i < cfg->view_count; i++) {
    pthread_join(threads[i], NULL);
  }

  View *list = NULL;
  for (int i = 0; i < cfg->view_count; i++) {
    View *v = MediaDataToView(thread_data[i].vc, thread_data[i].result, vid);
    if (v) {
      v->next = list;
      list = v;
    }
    FreeMediaData(thread_data[i].result);
  }

  free(threads);
  free(thread_data);
  return list;
}

static void Cleanup(View *views) {
  while (views) {
    View *n = views->next;
    if (views->is_anim) {
      for (unsigned i = 0; i < views->anim.count; i++)
        SDL_DestroyTexture(views->anim.frames[i]);
      free(views->anim.frames);
      free(views->anim.delays);
    } else if (views->static_tex) {
      SDL_DestroyTexture(views->static_tex);
    }
    free(views);
    views = n;
  }
}

int main(int argc, char **argv) {
  Config cfg = Parse(argc, argv);
  Video vid = Setup(cfg.create_desktop);
  View *views = LoadViews(&cfg, &vid);

  bool run = true;
  while (run) {
    SDL_Event e;
    while (SDL_PollEvent(&e))
      if (e.type == SDL_QUIT)
        run = false;

    SDL_RenderClear(vid.renderer);
    Uint32 now = SDL_GetTicks();
    for (View *v = views; v; v = v->next) {
      if (v->is_anim && v->anim.count > 0) {
        Uint32 frame_delay = v->is_gif ? GIF_FRAME_DELAY / v->speed
                                       : v->anim.delays[v->idx] / v->speed;
        if (now - v->last_ts >= frame_delay) {
          v->idx = (v->idx + 1) % v->anim.count;
          v->last_ts = now;
        }
        SDL_RenderCopy(vid.renderer, v->anim.frames[v->idx], NULL, &v->dst);
      } else if (v->static_tex) {
        SDL_RenderCopy(vid.renderer, v->static_tex, NULL, &v->dst);
      }
    }
    SDL_RenderPresent(vid.renderer);
    SDL_Delay(10);
  }

  Cleanup(views);
  teardown(&vid);
  return 0;
}

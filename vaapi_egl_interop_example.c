#if 0  // self-compiling code: chmod +x this file and run it like a script
BINARY=vaapi_egl_interop_example
gcc -Wall -Wextra -pedantic -Werror -g -fsanitize=address -o $BINARY $0 \
    `pkg-config libavcodec libavformat libavutil libva gl egl libdrm --cflags --libs` \
    -lX11 -lva-x11 -lva-drm || exit 1
test "$1" = "--compile-only" && exit 0
exec env ASAN_OPTIONS=fast_unwind_on_malloc=0 ./$BINARY $*
#endif  /*

Minimal example application for hardware video decoding on Linux and display
over VA-API/EGL interoperability into an X11 window. This is essentially how
MPV, Kodi etc. work, just in very condensed and easier-to-understand form.
Takes a video file as an argument and plays it back in a window, without audio,
and without time synchronization (i.e. it will play at whatever rate the GPU
can decode the frames, or at VSync rate).
*/

// configuration section: switch between the many parts that are implemented
// in two or more possible ways in this program
#define USE_LAYERS       1  // 0 = use VA_EXPORT_SURFACE_COMPOSED_LAYERS
                            // 1 = use VA_EXPORT_SURFACE_SEPARATE_LAYERS
#define SWAP_INTERVAL    2  // 0 = decode and display as fast as possible
                            // 1 = run at VSync framerate (typically 60 Hz)
                            // 2 = run at half VSync framerate (30 Hz)
// (*) this currently crashes due to bugs(?) somewhere in the Mesa stack

// request OpenGL 3.3 for Core Profile
#define CORE_PROFILE_MAJOR_VERSION 3
#define CORE_PROFILE_MINOR_VERSION 3

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_vaapi.h>

#include <va/va.h>
#include <va/va_x11.h>
#include <va/va_drmcommon.h>

#include <drm_fourcc.h>

#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/Xutil.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>

#include <GL/gl.h>
#include <GL/glext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>


// exit with a simple error message
void fail(const char *msg) {
    fprintf(stderr, "\nERROR: %s failed\n", msg);
    exit(1);
}

// callback to negotiate the output pixel format.
// we don't negotiate here, we just want VA-API.
static enum AVPixelFormat get_hw_format(AVCodecContext *ctx, const enum AVPixelFormat *pix_fmts) {
    (void)ctx, (void)pix_fmts;
    return AV_PIX_FMT_VAAPI;
}

// configure a single OpenGL texture
static void setup_texture() {
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
}

// set a suitable OpenGL viewport for a specified window size
static void resize_window(int screen_width, int screen_height, const AVCodecContext* ctx) {
    int display_width = screen_width;
    int display_height = (screen_width * ctx->height + ctx->width / 2) / ctx->width;
    if (display_height > screen_height) {
        display_width = (screen_height * ctx->width + ctx->height / 2) / ctx->height;
        display_height = screen_height;
    }
    glViewport((screen_width  - display_width)  / 2,
               (screen_height - display_height) / 2,
               display_width, display_height);
}

void show_help(int argc, char* argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <input.mp4> [/dev/dri/renderDxxx]\n", argv[0]);
        exit(2);
    }
}

Display* open_x11_display() {
    // open X11 display
    Display *x_display;
    x_display = XOpenDisplay(NULL);
    if (!x_display) {
        fail("XOpenDisplay");
    }
    return x_display;
}

VADisplay initialize_vaapi(Display* x_display) {
    // initialize VA-API
    VADisplay va_display = 0;
    va_display = vaGetDisplay(x_display);
    if (!va_display) {
        fail("vaGetDisplay");
    }
    int major, minor;
    if (vaInitialize(va_display, &major, &minor) != VA_STATUS_SUCCESS) {
        fail("vaInitialize");
    }
    return va_display;
}

// open input file, video stream and decoder
void open_source(AVCodecContext **decoder_ctx, int *video_stream, char* argv[], AVFormatContext **input_ctx, AVCodec **decoder)
{
  #if LIBAVFORMAT_VERSION_INT < AV_VERSION_INT(58, 9, 100)
      av_register_all();
  #endif
  if (avformat_open_input(input_ctx, argv[1], NULL, NULL) != 0) {
      fail("avformat_open_input");
  }
  if (avformat_find_stream_info(*input_ctx, NULL) < 0) {
      fail("avformat_find_stream_info");
  }
  *video_stream = av_find_best_stream(*input_ctx, AVMEDIA_TYPE_VIDEO, -1, -1, decoder, 0);
  if (*video_stream < 0) {
      fail("av_find_best_stream");
  }
  *decoder_ctx = avcodec_alloc_context3(*decoder);
  if (!*decoder_ctx) {
      fail("avcodec_alloc_context3");
  }
  if (avcodec_parameters_to_context(*decoder_ctx, (*input_ctx)->streams[*video_stream]->codecpar) < 0) {
      fail("avcodec_parameters_to_context");
  }
}

// use av_hwdevice_ctx_alloc() and populate the underlying structure
// to use the VA-API context ("display") we created before
void populate_context(AVCodec *decoder, VADisplay va_display, AVCodecContext *decoder_ctx, AVBufferRef **hw_device_ctx)
{
  *hw_device_ctx = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_VAAPI);
  if (!*hw_device_ctx) {
      fail("av_hwdevice_ctx_alloc");
  }
  AVHWDeviceContext *hwctx = (void*) (*hw_device_ctx)->data;
  AVVAAPIDeviceContext *vactx = hwctx->hwctx;
  vactx->display = va_display;
  if (av_hwdevice_ctx_init(*hw_device_ctx) < 0) {
      fail("av_hwdevice_ctx_init");
  }
  decoder_ctx->get_format = get_hw_format;
  decoder_ctx->hw_device_ctx = av_buffer_ref(*hw_device_ctx);
  if (avcodec_open2(decoder_ctx, decoder, NULL) < 0) {
      fail("avcodec_open2");
  }
  printf("Opened input video stream: %dx%d\n", decoder_ctx->width, decoder_ctx->height);
}

Atom create_x11_window(Display* x_display, AVCodecContext *decoder_ctx, Window *window)
{
  XSetWindowAttributes xattr;
  xattr.override_redirect = False;
  xattr.border_pixel = 0;
  *window = XCreateWindow(x_display, DefaultRootWindow(x_display),
           0, 0, decoder_ctx->width, decoder_ctx->height,
           0, CopyFromParent, InputOutput, CopyFromParent,
           CWOverrideRedirect | CWBorderPixel, &xattr);
  if (!*window) {
      fail("XCreateWindow");
  }
  XStoreName(x_display, *window, "VA-API EGL Interop Test");
  XMapWindow(x_display, *window);
  XSelectInput(x_display, *window, ExposureMask | StructureNotifyMask | KeyPressMask);
  Atom WM_DELETE_WINDOW = XInternAtom(x_display, "WM_DELETE_WINDOW", True);
  XSetWMProtocols(x_display, *window, &WM_DELETE_WINDOW, 1);

  return WM_DELETE_WINDOW;
}

EGLDisplay initialize_egl(Display* x_display)
{
  EGLDisplay egl_display;
  egl_display = eglGetDisplay((EGLNativeDisplayType)x_display);
  if (egl_display == EGL_NO_DISPLAY) {
      fail("eglGetDisplay");
  }
  if (!eglInitialize(egl_display, NULL, NULL)) {
      fail("eglInitialize");
  }
  if (!eglBindAPI(EGL_OPENGL_API)) {
      fail("eglBindAPI");
  }

  return egl_display;
}

// create the OpenGL rendering context using EGL
void create_opengl_ctx(EGLContext *egl_context, EGLDisplay egl_display, EGLSurface *egl_surface, Window window)
{
  EGLint visual_attr[] = {
      EGL_SURFACE_TYPE,    EGL_WINDOW_BIT,
      EGL_RED_SIZE,        8,
      EGL_GREEN_SIZE,      8,
      EGL_BLUE_SIZE,       8,
      EGL_ALPHA_SIZE,      8,
      EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
      EGL_NONE
  };
  EGLConfig cfg;
  EGLint cfg_count;
  if (!eglChooseConfig(egl_display, visual_attr, &cfg, 1, &cfg_count) || (cfg_count < 1)) {
      fail("eglChooseConfig");
  }
  *egl_surface = eglCreateWindowSurface(egl_display, cfg, window, NULL);
  if (*egl_surface == EGL_NO_SURFACE) {
      fail("eglCreateWindowSurface");
  }
  EGLint ctx_attr[] = {
      EGL_CONTEXT_OPENGL_PROFILE_MASK,
          EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT,
          EGL_CONTEXT_MAJOR_VERSION, CORE_PROFILE_MAJOR_VERSION,
          EGL_CONTEXT_MINOR_VERSION, CORE_PROFILE_MINOR_VERSION,
      EGL_NONE
  };
  *egl_context = eglCreateContext(egl_display, cfg, EGL_NO_CONTEXT, ctx_attr);
  if (*egl_context == EGL_NO_CONTEXT) {
      fail("eglCreateContext");
  }
  eglMakeCurrent(egl_display, *egl_surface, *egl_surface, *egl_context);
  eglSwapInterval(egl_display, SWAP_INTERVAL);
}

// dump OpenGL configuration (for reference)
void dump_opengl_cfg()
{
  printf("OpenGL vendor:   %s\n", glGetString(GL_VENDOR));
  printf("OpenGL renderer: %s\n", glGetString(GL_RENDERER));
  printf("OpenGL version:  %s\n", glGetString(GL_VERSION));
}

// look up required EGL and OpenGL extension functions
#define LOOKUP_FUNCTION(type, func) \
    type func = (type) eglGetProcAddress(#func); \
    if (!func) { fail("eglGetProcAddress(" #func ")"); }

// OpenGL shader setup
GLuint opengl_shader_setup()
{
  GLuint vao;                   // OpenGL Core Profile requires
  LOOKUP_FUNCTION(PFNGLGENVERTEXARRAYSPROC,            glGenVertexArrays);
  glGenVertexArrays(1, &vao);   // using VAOs even in trivial cases,
  LOOKUP_FUNCTION(PFNGLBINDVERTEXARRAYPROC,            glBindVertexArray);
  glBindVertexArray(vao);       // so let's set up a dummy VAO
  #define DECLARE_YUV2RGB_MATRIX_GLSL \
      "const mat4 yuv2rgb = mat4(\n" \
      "    vec4(  1.1644,  1.1644,  1.1644,  0.0000 ),\n" \
      "    vec4(  0.0000, -0.2132,  2.1124,  0.0000 ),\n" \
      "    vec4(  1.7927, -0.5329,  0.0000,  0.0000 ),\n" \
      "    vec4( -0.9729,  0.3015, -1.1334,  1.0000 ));"
  const char *vs_src =
           "#version 130"
      "\n" "const vec2 coords[4] = vec2[]( vec2(0.,0.), vec2(1.,0.), vec2(0.,1.), vec2(1.,1.) );"
      "\n" "uniform vec2 uTexCoordScale;"
      "\n" "out vec2 vTexCoord;"
      "\n" "void main() {"
      "\n" "    vec2 c = coords[gl_VertexID];"
      "\n" "    vTexCoord = c * uTexCoordScale;"
      "\n" "    gl_Position = vec4(c * vec2(2.,-2.) + vec2(-1.,1.), 0., 1.);"
      "\n" "}";
  const char *fs_src =
           "#version 130"
      "\n" "in vec2 vTexCoord;"
      "\n" "uniform sampler2D uTexY, uTexC;"
      "\n" DECLARE_YUV2RGB_MATRIX_GLSL
      "\n" "out vec4 oColor;"
      "\n" "void main() {"
      "\n" "    oColor = yuv2rgb * vec4(texture(uTexY, vTexCoord).x, "
                                       "texture(uTexC, vTexCoord).xy, 1.);"
      "\n" "}";
  GLuint prog = glCreateProgram();
  GLuint vs = glCreateShader(GL_VERTEX_SHADER);
  GLuint fs = glCreateShader(GL_FRAGMENT_SHADER);
  if (!prog) { fail("glCreateProgram"); }
  if (!vs || !fs) { fail("glCreateShader"); }
  glShaderSource(vs, 1, &vs_src, NULL);
  glShaderSource(fs, 1, &fs_src, NULL);
  GLint ok;
  while (glGetError()) {}
  glCompileShader(vs);  glGetShaderiv(vs, GL_COMPILE_STATUS, &ok);
  if (glGetError() || (ok != GL_TRUE)) { fail("glCompileShader(GL_VERTEX_SHADER)"); }
  glCompileShader(fs); glGetShaderiv(fs, GL_COMPILE_STATUS, &ok);
  if (glGetError() || (ok != GL_TRUE)) { fail("glCompileShader(GL_FRAGMENT_SHADER)"); }
  glAttachShader(prog, vs);
  glAttachShader(prog, fs);
  glLinkProgram(prog);
  if (glGetError()) { fail("glLinkProgram"); }
  glUseProgram(prog);
  glUniform1i(glGetUniformLocation(prog, "uTexY"), 0);
  glUniform1i(glGetUniformLocation(prog, "uTexC"), 1);

  return prog;
}

// OpenGL texture setup
void opengl_texture_setup(GLuint textures[2])
{
  glGenTextures(2, textures);
  for (int i = 0;  i < 2;  ++i) {
      glBindTexture(GL_TEXTURE_2D, textures[i]);
      setup_texture();
  }
  glBindTexture(GL_TEXTURE_2D, 0);
}

void handle_x11_events(Display* x_display, Atom WM_DELETE_WINDOW, bool *running, AVCodecContext *decoder_ctx) {
  // handle X11 events
  while (XPending(x_display)) {
      XEvent ev;
      XNextEvent(x_display, &ev);
      switch (ev.type) {
          case ClientMessage:
              if (((Atom) ev.xclient.data.l[0]) == WM_DELETE_WINDOW) {
                  *running = false;
              }
              break;
          case KeyPress:
              switch (XLookupKeysym(&ev.xkey, 0)) {
                  case 'q':
                      *running = false;
                      break;
                  case 'a':
                      decoder_ctx->skip_frame = AVDISCARD_NONE;
                      break;
                  case 'b':
                      decoder_ctx->skip_frame = AVDISCARD_NONREF;
                      break;
                  case 'p':
                      decoder_ctx->skip_frame = AVDISCARD_BIDIR;
                      break;
                  default: break;
              }
              break;
          case ConfigureNotify:
              resize_window(((XConfigureEvent*)&ev)->width, ((XConfigureEvent*)&ev)->height, decoder_ctx);
              break;
          default:
              break;
      }
  }
}

// retrieve a frame from the decoder
bool retrieve_frame(AVCodecContext *decoder_ctx, AVFrame *frame, bool *want_new_packet,
                    int *frameno, VASurfaceID *va_surface) {
  int ret = avcodec_receive_frame(decoder_ctx, frame);
  if ((ret == AVERROR(EAGAIN)) || (ret == AVERROR_EOF)) {
      *want_new_packet = true;
      return false;  // no more frames ready from the decoder -> decode new ones
  }
  else if (ret < 0) {
      fail("avcodec_receive_frame");
  }
  *va_surface = (uintptr_t)frame->data[3];
  printf("\rframe #%d (%c) ", ++*frameno, av_get_picture_type_char(frame->pict_type));
  fflush(stdout);
  return true;
}

void main_loop(Display* x_display, GLuint textures[2], EGLDisplay egl_display, VADisplay va_display,
               float texcoord_x1, GLuint prog, AVFrame *frame,
               bool packet_valid, EGLSurface egl_surface, float texcoord_y1,
               bool texture_size_valid, int video_stream, bool running, AVFormatContext *input_ctx,
               AVCodecContext *decoder_ctx, Atom WM_DELETE_WINDOW, AVPacket packet)
{
  LOOKUP_FUNCTION(PFNEGLCREATEIMAGEKHRPROC,            eglCreateImageKHR)
  LOOKUP_FUNCTION(PFNEGLDESTROYIMAGEKHRPROC,           eglDestroyImageKHR)
  LOOKUP_FUNCTION(PFNGLEGLIMAGETARGETTEXTURE2DOESPROC, glEGLImageTargetTexture2DOES)
  bool want_new_packet = true;
  int frameno = 0;

  while (running) {
      handle_x11_events(x_display, WM_DELETE_WINDOW, &running, decoder_ctx);

      // prepare frame and packet for re-use
      if (packet_valid) { av_packet_unref(&packet); packet_valid = false; }

      // read compressed data from stream and send it to the decoder
      if (want_new_packet) {
          if (av_read_frame(input_ctx, &packet) < 0) {
              break;  // end of stream
          }
          packet_valid = true;
          if (packet.stream_index != video_stream) {
              continue;  // not a video packet
          }
          if (avcodec_send_packet(decoder_ctx, &packet) < 0) {
              fail("avcodec_send_packet");
          }
          want_new_packet = false;
      }

      VASurfaceID va_surface;
      if (!retrieve_frame(decoder_ctx, frame, &want_new_packet, &frameno, &va_surface)) continue;

	  // convert the frame into a pair of DRM-PRIME FDs
      VADRMPRIMESurfaceDescriptor prime;
      if (vaExportSurfaceHandle(va_display, va_surface,
          VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2,
          VA_EXPORT_SURFACE_READ_ONLY |
          #if USE_LAYERS
              VA_EXPORT_SURFACE_SEPARATE_LAYERS,
          #else
              VA_EXPORT_SURFACE_COMPOSED_LAYERS,
          #endif
          &prime) != VA_STATUS_SUCCESS)
          { fail("vaExportSurfaceHandle"); }
      if (prime.fourcc != VA_FOURCC_NV12) {
          fail("export format check");  // we only support NV12 here
      }
      vaSyncSurface(va_display, va_surface);

      // check the actual size of the frame
      if (!texture_size_valid) {
          texcoord_x1 = (float)((double) decoder_ctx->width  / (double) prime.width);
          texcoord_y1 = (float)((double) decoder_ctx->height / (double) prime.height);
          glUniform2f(glGetUniformLocation(prog, "uTexCoordScale"), texcoord_x1, texcoord_y1);
          texture_size_valid = true;
      }

      // import the frame into OpenGL
      EGLImage images[2];
      for (int i = 0;  i < 2;  ++i) {
          static const uint32_t formats[2] = { DRM_FORMAT_R8, DRM_FORMAT_GR88 };
          #if USE_LAYERS
              #define LAYER i
              #define PLANE 0
              if (prime.layers[i].drm_format != formats[i]) {
                  fail("expected DRM format check");
              }
          #else
              #define LAYER 0
              #define PLANE i
          #endif
          EGLint img_attr[] = {
              EGL_LINUX_DRM_FOURCC_EXT,      formats[i],
              EGL_WIDTH,                     prime.width  / (i + 1),  // half size
              EGL_HEIGHT,                    prime.height / (i + 1),  // for chroma
              EGL_DMA_BUF_PLANE0_FD_EXT,     prime.objects[prime.layers[LAYER].object_index[PLANE]].fd,
              EGL_DMA_BUF_PLANE0_OFFSET_EXT, prime.layers[LAYER].offset[PLANE],
              EGL_DMA_BUF_PLANE0_PITCH_EXT,  prime.layers[LAYER].pitch[PLANE],
              EGL_NONE
          };
          images[i] = eglCreateImageKHR(egl_display, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, NULL, img_attr);
          if (!images[i]) {
              fail(i ? "chroma eglCreateImageKHR" : "luma eglCreateImageKHR");
          }
          glActiveTexture(GL_TEXTURE0 + i);
          glBindTexture(GL_TEXTURE_2D, textures[i]);
          while (glGetError()) {}
          glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, images[i]);
          if (glGetError()) {
              fail("glEGLImageTargetTexture2DOES");
          }
      }
      for (int i = 0;  i < (int)prime.num_objects;  ++i) {
          close(prime.objects[i].fd);
      }

      // draw the frame
      glClear(GL_COLOR_BUFFER_BIT);
      while (glGetError()) {}
      glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
      if (glGetError()) { fail("drawing"); }

      // display the frame
         eglSwapBuffers(egl_display, egl_surface);

      // clean up the interop images
      for (int i = 0;  i < 2;  ++i) {
          glActiveTexture(GL_TEXTURE0 + i);
          glBindTexture(GL_TEXTURE_2D, 0);
          eglDestroyImageKHR(egl_display, images[i]);
      }
  }
}

int main(int argc, char* argv[]) {
	show_help(argc, argv);
	Display* x_display = open_x11_display();
	VADisplay va_display = initialize_vaapi(x_display);

    AVFormatContext *input_ctx = NULL;
    AVCodec *decoder = NULL;
    AVCodecContext *decoder_ctx = NULL;
    AVBufferRef *hw_device_ctx = NULL;
    int video_stream = -1;
    open_source(&decoder_ctx, &video_stream, argv, &input_ctx, &decoder);
    populate_context(decoder, va_display, decoder_ctx, &hw_device_ctx);

    Window window;
    Atom WM_DELETE_WINDOW = create_x11_window(x_display, decoder_ctx, &window);

    EGLDisplay egl_display = initialize_egl(x_display);

    EGLSurface egl_surface;
    EGLContext egl_context;
    create_opengl_ctx(&egl_context, egl_display, &egl_surface, window);

    dump_opengl_cfg();

    GLuint prog = opengl_shader_setup();

    GLuint textures[2];
    opengl_texture_setup(textures);

    // initial window size setup
    GLint vp[4];
    glGetIntegerv(GL_VIEWPORT, vp);
    resize_window(vp[2], vp[3], decoder_ctx);

    // allocate AVFrame for display
    AVFrame *frame = av_frame_alloc();
    if (!frame) {
        fail("av_frame_alloc");
    }

    // main loop
    AVPacket packet;
    bool packet_valid = false;
    bool running = true;
    bool texture_size_valid = false;
    float texcoord_x1 = 1.0f, texcoord_y1 = 1.0f;
    main_loop(x_display, textures, egl_display, va_display, texcoord_x1, prog,
              frame, packet_valid, egl_surface,
              texcoord_y1, texture_size_valid, video_stream, running,
              input_ctx, decoder_ctx, WM_DELETE_WINDOW, packet);

    // normally, we'd flush the decoder here to ensure we've shown *all* frames
    // of the video, but this is left out as an exercise for the reader ;)

    // clean up all the mess we made
    if (packet_valid) { av_packet_unref(&packet); }
    av_frame_free(&frame);
    eglMakeCurrent(egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroyContext(egl_display, egl_context);
    eglDestroySurface(egl_display, egl_surface);
    eglTerminate(egl_display);
    XDestroyWindow(x_display, window);
    XCloseDisplay(x_display);
    avcodec_free_context(&decoder_ctx);
    avformat_close_input(&input_ctx);
    av_buffer_unref(&hw_device_ctx);
    vaTerminate(va_display);
	// TODO: TO HERE
    printf("\nBye.\n");
    return 0;
}

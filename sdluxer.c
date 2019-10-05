#define _GNU_SOURCE
#include <SDL/SDL.h>
#include "lux.h"
#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/mman.h>
#include <sys/un.h>
#include <unistd.h>
#include <poll.h>
#include <errno.h>
#include <signal.h>

#include "sdluxer.h"

#define LOG(level, fmt, ...)
#define LOG_INFO(fmt, ...)
#define LOG_WARN(fmt, ...)
#define LOG_ERROR(fmt, ...)
#define LOG_DEBUG(fmt, ...)

#ifdef LOG_LEVEL
#if LOG_LEVEL >= 1
#define LOG_ERROR(fmt, ...) fprintf(stderr, "   ***   " fmt "\n", ##__VA_ARGS__);
#if LOG_LEVEL >= 2
#define LOG_WARN(fmt, ...) fprintf(stderr, "   !!!   " fmt "\n", ##__VA_ARGS__);
#if LOG_LEVEL >= 3
#define LOG_INFO(fmt, ...) fprintf(stderr, "   ---   " fmt "\n", ##__VA_ARGS__);
#if LOG_LEVEL >= 4
#define LOG_DEBUG(fmt, ...) fprintf(stderr, "   ...   " fmt "\n", ##__VA_ARGS__);
#endif
#endif
#endif
#endif
#endif

#define START_HANDLERS if (false) {
#define HANDLE(T) } else if (type == T && length >= sizeof(T ## Msg)) { T##Msg * msg = (void*)(buf+4);
#define OMSG(T,V) obuf.type = T; T##Msg * V = (void*)&obuf.data[0];

struct ObufType
{
  int32_t type;
  char data[1024];
} __attribute__ ((aligned (4)));

static struct ObufType obuf; // Temporary space for sending packets


static bool quitting = false;

static int screen_width = 640, screen_height = 480;

static bool parse_dimensions (const char * d, int * ww, int * hh)
{
  int w = *ww, h = *hh;
  if (sscanf(d, "%i,%i", &w, &h) != 2)
  {
    if (sscanf(d, "%ix%i", &w, &h) != 2)
    {
      return false;
    }
  }

  if (w < 20) w = 20;
  if (h < 10) h = 10;

  *ww = w; *hh = h;

  return true;
}


static void about_key_handler (Window * w, SDL_keysym * k, bool down)
{
  if (k->unicode == 'q' || k->unicode == 'Q') quitting = true;
}

void about_click_handler (Window * w, int x, int y, int buttons, int type, bool raised)
{
  window_close(w);
}

bool about_draw_handler (Window * w, SDL_Surface * scr, SDL_Rect r)
{
  window_clear_client(w, w->bg_color);
  text_draw(scr, "SDLuxer by Murphy McCauley\n\nPress 'Q' to quit.", r.x, r.y, 0xffFFffFF);
  return true;
}

static void f1_handler (FKey * fkey)
{
  Window * w = window_create(200, lux_sysfont_h()*3, "SDLuxer", 0);
  w->on_mousedown = about_click_handler;
  w->bg_color = lux_get_theme().win.face;
  w->on_draw = about_draw_handler;
  w->on_keydown = about_key_handler;
}

typedef struct SavedBuffer_tag
{
  struct SavedBuffer_tag * next;
  int size;
  char data[0];
} SavedBuffer;

typedef struct
{
  Window * wnd;
  SDL_Surface * surf1;
  SDL_Surface * surf2;
  void * shmem;
  size_t shmem_size;
  char * shmem_name;
  SavedBuffer * buffered_out;
  int fd;
  bool flip_wait;
  bool do_draw;
  int num_cursors;
  SDL_Cursor ** cursors;
} Session;

// Since we use poll(), we keep a 1:1 mapping between file descriptors and
// sessions.  So this really is like "max file descriptors".  We could keep
// a map or something, but the real solution is to use epoll or some other
// well-designed API.
#define SDLUX_MAX_SESSIONS 128
struct pollfd session_fds[SDLUX_MAX_SESSIONS] = {};
Session sessions[SDLUX_MAX_SESSIONS] = {};
int max_fd;
int listen_fd;


char * listen_sock_name = NULL;

bool senddata (int fd, int size)
{
  char * buf = (char*)&obuf;
  size += 4;
  Session * s = sessions+fd;
  while (true)
  {
    bool save = false;
    if (s->buffered_out)
    {
      save = true;
    }
    else
    {
      int r = send(fd, buf, size, 0);
      if (r == size)
      {
        LOG_DEBUG("Send okay");
        return true;
      }
      if (r == -1 && errno == EINTR) continue;

      if (r == -1 && (errno == EAGAIN || errno == EWOULDBLOCK))
      {
        save = true;
      }
    }

    if (save)
    {
      SavedBuffer * saved = malloc(sizeof(SavedBuffer) + size);
      if (saved)
      {
        saved->size = size;
        saved->next = NULL;
        memcpy(saved->data, buf, size);
        if (!s->buffered_out)
        {
          s->buffered_out = saved;
        }
        else
        {
          SavedBuffer * old = s->buffered_out;
          while (old->next) old = old->next;
          old->next = saved;
        }
        session_fds[fd].events |= POLLOUT;
        LOG_DEBUG("Send buffered");
        return true;
      }
      LOG_ERROR("Couldn't buffer outgoing message");
      return false;
    }

    LOG_ERROR("Incomplete send!");
    return false;
  }
  LOG_ERROR("Unreachable code reached?!");
  return false; // Unreachable
}


bool new_session (int fd)
{
  if (fd == listen_fd) return false;
  if (fd >= SDLUX_MAX_SESSIONS) return false;
  if (session_fds[fd].fd >= 0) return false;

  session_fds[fd].fd = fd;
  session_fds[fd].events = POLLIN;

  sessions[fd].fd = fd;

  if (fd > max_fd) max_fd = fd;
}

void close_session (int fd)
{
  // Reset max_fd
  if (fd < 0) return;
  if (fd > SDLUX_MAX_SESSIONS) return;
  close(fd);
  //TODO: dup the max fd down into the closed fd!

  LOG_DEBUG("close_session(%i)", fd);
  if (fd == max_fd)
  {
    max_fd = -1;
    for (int i = fd-1; i >= 0; i--)
    {
      if (session_fds[i].fd >= 0)
      {
        max_fd = i;
        break;
      }
    }
  }

  session_fds[fd].fd = -1;
  session_fds[fd].events = 0;
  session_fds[fd].revents = 0;

  if (fd == listen_fd) return;

  Session * s = sessions+fd;

  if (s->shmem_name)
  {
    shm_unlink(s->shmem_name);
    free(s->shmem_name);
    s->shmem_name = NULL;
  }

  if (s->surf1) SDL_FreeSurface(s->surf1);
  if (s->surf2) SDL_FreeSurface(s->surf2);
  if (s->shmem) munmap(s->shmem, s->shmem_size);
  s->surf1 = s->surf2 = s->shmem = NULL;

  if (s->cursors)
  {
    for (int i = 0; i < s->num_cursors; i++)
    {
      if (!s->cursors[i]) continue;
      SDL_FreeCursor(s->cursors[i]);
    }
    free(s->cursors);
  }

  if (s->wnd)
  {
    window_close(s->wnd);
    s->wnd = NULL;
  }

  if (s->buffered_out)
  {
    SavedBuffer * sb = s->buffered_out;
    s->buffered_out = NULL;
    while (sb)
    {
      SavedBuffer * nxt = sb->next;
      free(sb);
      sb = nxt;
    }
  }

  memset(s, 0, sizeof(*s));
}


static void sdl_resized_handler (Window * w)
{
  Session * s = (void *)w->opaque_ptr;
  if (!s) return;
  OMSG(ResizedEvent, m);
  m->event.type = SDL_VIDEORESIZE;
  SDL_Rect r;
  window_get_client_rect(w, &r);
  m->event.w = r.w;
  m->event.h = r.h;
  if (!senddata(s->fd, sizeof(*m))) close_session(s->fd);
}

static void sdl_raiselower_handler (Window * w, bool raised)
{
  Session * s = (void *)w->opaque_ptr;
  if (!s) return;
  OMSG(ActiveEvent, m);
  m->event.type = SDL_ACTIVEEVENT;
  m->event.gain = raised ? 1 : 0;
  m->event.state = SDL_APPINPUTFOCUS;
  if (!senddata(s->fd, sizeof(*m))) close_session(s->fd);
}

static void sdl_mouseinout_handler (Window * w, bool in)
{
  Session * s = (void *)w->opaque_ptr;
  if (!s) return;
  OMSG(ActiveEvent, m);
  m->event.type = SDL_ACTIVEEVENT;
  m->event.gain = in ? 1 : 0;
  m->event.state = SDL_APPMOUSEFOCUS;
  if (!senddata(s->fd, sizeof(*m))) close_session(s->fd);
}

static void sdl_close_handler (Window * w)
{
  Session * s = (void *)w->opaque_ptr;
  if (!s) return; // Maybe just close it?
  OMSG(QuitEvent, msg);
  senddata(s->fd, sizeof(*msg));
  close_session(s->fd);
}

static bool sdl_draw_handler (Window * w, SDL_Surface * scr, SDL_Rect rect)
{
  Session * s = (Session *)w->opaque_ptr;
  if (!s || !s->surf1) return false;
  SDL_Rect r = {0,0,rect.w,rect.h};
  SDL_BlitSurface(s->surf1, &r, scr, &rect);
  return true;
}

static void sdl_key_handler (Window * w, SDL_keysym * k, bool down)
{
  Session * s = (void *)w->opaque_ptr;
  if (!s) return;
  OMSG(KeyEvent, em);
  SDL_KeyboardEvent * e = &em->event;
  e->type = down ? SDL_KEYDOWN : SDL_KEYUP;
  e->state = down ? SDL_PRESSED : SDL_RELEASED;
  e->keysym = *k;
  if (!senddata(s->fd, sizeof(*em))) close_session(s->fd);
}

static void sdl_mouse_button_handler (Window * w, int x, int y, int button, int type, bool raised)
{
  Session * s = (void *)w->opaque_ptr;
  if (!s) return;
  OMSG(MouseButtonEvent, em);
  em->event.type = type;
  em->event.state = (type == SDL_MOUSEBUTTONDOWN) ? SDL_PRESSED : SDL_RELEASED;
  em->event.x = x;
  em->event.y = y;
  em->event.button = button;
  if (!senddata(s->fd, sizeof(*em))) close_session(s->fd);
}

static void sdl_mouse_move_handler (Window * w, int x, int y, int buttons, int dx, int dy)
{
  Session * s = (void *)w->opaque_ptr;
  if (!s) return;
  OMSG(MouseMoveEvent, em);
  em->event.type = SDL_MOUSEMOTION;
  em->event.state = buttons;
  em->event.x = x;
  em->event.y = y;
  em->event.xrel = dx;
  em->event.yrel = dy;
  LOG_DEBUG("Mouse move fd:%i pos:%i,%i", s->fd, x, y);
  if (!senddata(s->fd, sizeof(*em))) close_session(s->fd);
}



static bool set_video_mode (int fd, Session * s, Window * w, SetVideoModeMsg * msg, VideoModeSetMsg * out)
{
  out->success = true;
  out->w = msg->w;
  out->h = msg->h;
  out->pitch = out->w * 4;
  out->depth = 32;
  out->rmask = rmask;
  out->gmask = gmask;
  out->bmask = bmask;
  out->double_buf = msg->double_buf;

  static int mem_id = 0;
  sprintf(out->name, "/sdluxer_%i_%i", getpid(), mem_id++);
  int memfd = shm_open(out->name, O_CREAT | O_RDWR | O_EXCL, 0666);
  if (memfd < 0)
  {
    LOG_ERROR("Shared memory creation failed");
    return false;
  }

  s->shmem_name = strdup(out->name);

  int size = msg->w * msg->h * 4;
  if (out->double_buf) size *= 2;

  if (ftruncate(memfd, size) != 0)
  {
    LOG_ERROR("ftruncate() failed due to errno:%i", errno);
    close(memfd);
    return false;
  }

  char * pixels = mmap(NULL, size, PROT_WRITE|PROT_READ, MAP_SHARED, memfd, 0);
  if (!pixels)
  {
    close(memfd);
    LOG_ERROR("Couldn't map memory");
    return false;
  }

  SDL_Surface * ns1 = NULL;
  SDL_Surface * ns2 = NULL;

  ns1 = SDL_CreateRGBSurfaceFrom(pixels, out->w, out->h, out->depth, out->pitch, out->rmask, out->gmask, out->bmask, 0);
  if (out->double_buf)
  {
    ns2 = SDL_CreateRGBSurfaceFrom(pixels + out->pitch * out->h, out->w, out->h, out->depth, out->pitch, out->rmask, out->gmask, out->bmask, 0);
  }

  if ( (!ns1) || (out->double_buf && (!ns2)) )
  {
    LOG_ERROR("Couldn't create surfaces");
    if (ns1) SDL_FreeSurface(ns1);
    if (ns2) SDL_FreeSurface(ns2);
    close(memfd);
    return false;
  }

  // Whew, that's everything!
  close(memfd);

  if (!w)
  {
    w = window_create(out->w, out->h, "", msg->resizable ? WIN_F_RESIZE : 0);
    w->on_keydown   = sdl_key_handler;
    w->on_keyup     = sdl_key_handler;
    w->on_mousedown = sdl_mouse_button_handler;
    w->on_mouseup   = sdl_mouse_button_handler;
    w->on_mousemove = sdl_mouse_move_handler;
    w->on_close     = sdl_close_handler;
    w->on_draw      = sdl_draw_handler;
    w->on_raise     = sdl_raiselower_handler;
    w->on_lower     = sdl_raiselower_handler;
    w->on_mousein   = sdl_mouseinout_handler;
    w->on_mouseout  = sdl_mouseinout_handler;
    w->on_resized   = sdl_resized_handler;
    w->opaque_ptr = s;
    s->wnd = w;
    if (!w)
    {
      LOG_ERROR("Couldn't create window");
      munmap(pixels, size);
      if (ns1) SDL_FreeSurface(ns1);
      if (ns2) SDL_FreeSurface(ns2);
      return false;
    }
  }
  else
  {
    window_resize(w, out->w, out->h);
    window_dirty(w);
  }

  if (s->surf1) SDL_FreeSurface(s->surf1);
  if (s->surf2) SDL_FreeSurface(s->surf2);
  if (s->shmem) munmap(s->shmem, s->shmem_size);
  if (ns2)
  {
    // Start swapped so that client draws on the one we're not showing
    s->surf2 = ns1;
    s->surf1 = ns2;
  }
  else
  {
    s->surf1 = ns1;
    s->surf2 = ns2;
  }
  s->shmem = pixels;
  s->shmem_size = size;

  LOG_DEBUG("set_video_mode success!");
  return true;
}

void handle_message (int fd, char * buf, int length)
{
  Session * s = &sessions[fd];
  Window * w = s->wnd;
  int32_t type = -1;
  if (length >= 4) type = *(int32_t*)buf;
  else LOG_ERROR("Message length is only %i", length);

  START_HANDLERS
  HANDLE(SetVideoMode)
    OMSG(VideoModeSet, out);
    if (!set_video_mode(fd, s, w, msg, out))
    {
      out->success = false;
      if (!senddata(fd, sizeof(*out))) close_session(fd);
    }
    else
    {
      if (!senddata(fd, sizeof(*out) + strlen(out->name) + 1)) close_session(fd);
    }
  HANDLE(WarpMouse)
    if (w && window_is_top(w))
    {
      SDL_Rect r;
      window_get_client_rect(w, &r);
      int x = msg->x;
      int y = msg->y;
      if (x < 0) x = 0;
      else if (x >= r.w) x = r.w;
      if (y < 0) y = 0;
      else if (y >= r.h) y = r.h;
      window_rect_window_to_screen(w, &r);
      x += r.x;
      y += r.y;
      SDL_WarpMouse(x, y);
    }
  HANDLE(AddCursor)
    int index = -1;
    if (msg->w % 8)
    {
      LOG_WARN("Cursor width isn't multiple of 8");
    }
    else
    {
      int size = msg->w / 8 * msg->h;
      int needed = 2*size + sizeof(*msg);
      if (needed > length)
      {
        LOG_WARN("Not enough cursor data");
      }
      else
      {
        SDL_Cursor * nc = SDL_CreateCursor(msg->data, msg->data + size, msg->w, msg->h, msg->hotx, msg->hoty);
        if (!nc) LOG_WARN("Couldn't create cursor");
        // We could look for a "free" spot, but we don't.
        if (s->cursors == NULL)
        {
          s->cursors = malloc(sizeof(SDL_Cursor*));
        }
        else
        {
          s->cursors = realloc(s->cursors, sizeof(SDL_Cursor*) * (s->num_cursors+1));
        }
        if (!s->cursors)
        {
          LOG_ERROR("Couldn't allocate cursor memory");
        }
        else
        {
          s->cursors[s->num_cursors] = nc;
          index = s->num_cursors;
          ++s->num_cursors;
        }
      }
    }

    OMSG(CursorAdded, out);
    out->index = index;
    if (!senddata(fd, sizeof(*out))) close_session(fd);

  HANDLE(ManageCursor)
    if (w)
    {
      // Fail silently if bad index
      int index = msg->index;
      int op = msg->op;
      if (index == -1 && op == CursorOpSet)
      {
        // Special case
        window_cursor_set(w, NULL);
      }
      else if (op == CursorOpShow || op == CursorOpHide)
      {
        window_cursor_show(w, op == 2);
      }
      else if (index >= 0 && index < s->num_cursors)
      {
        if (op == CursorOpSet) // Set
        {
          window_cursor_set(w, s->cursors[index]);
        }
        else if (op == CursorOpDel) // Delete
        {
          if (w->cursor == s->cursors[index]) window_cursor_set(w, NULL);
          if (s->cursors[index]) SDL_FreeCursor(s->cursors[index]);
          s->cursors[msg->index] = NULL;
        }
      }
    }
  HANDLE(WM_SetCaption)
    if (w) window_set_title(w, msg->caption);
  HANDLE(Draw)
    if (!w)
    {
      LOG_WARN("Got a flip request from session with no window!\n");
      close_session(fd);
    }
    else
    {
      s->flip_wait = msg->flip;
      s->do_draw = true;
    }
  }
  else
  {
    LOG_WARN("Unrecognized message type:%i on fd:%i\n", type, fd);
    close_session(fd);
  }
}




void main_loop ()
{
#ifndef NO_PPOLL
  sigset_t sigmask;
  sigprocmask(0, NULL, &sigmask);
#endif

  Uint32 start_time = SDL_GetTicks();
  Uint32 last_time = start_time;
  Uint32 target_time = start_time;
  uint64_t num_frames = (uint64_t)0; // Will eventually wrap, but not any time soon
  bool draw_pending = true;

  int idle_count = 0; // Number of frames in a row we've been idle

  while (max_fd > 0 && !quitting)
  {
    bool idle = true;

    while (max_fd > 0 && !quitting)
    {
      Uint32 now = SDL_GetTicks();
      if (now < last_time)
      {
        // Wrapped.  Resync.
        start_time = now;
        target_time = now;
      }
      last_time = now;
      int delta = target_time - now;

      // Between frames
      if (delta <= 0 && draw_pending)
      {
        delta = 0;
        for (int i = 0; i <= max_fd; i++)
        {
          if (sessions[i].do_draw)
          {
            Session * s = sessions+i;
            if (s->flip_wait)
            {
              OMSG(Flipped, fm);
              if (!senddata(s->fd, sizeof(*fm))) close_session(s->fd);
            }
            s->flip_wait = false;
            s->do_draw = false;
            if (s->surf1 && s->surf2)
            {
              SDL_Surface * tmp = s->surf1;
              s->surf1 = s->surf2;
              s->surf2 = tmp;
            }
            if (s->wnd)
            {
              window_dirty(s->wnd);
            }
          }
        }

        draw_pending = false;
        lux_draw();
        if (idle_count)
        {
          last_time = start_time = now;
          num_frames = 0;
        }
        idle_count = 0;
        ++num_frames;
        target_time = num_frames * 1000 / 60 + start_time;
      }
      else if (delta <= 0)
      {
        delta = 0;
        last_time = start_time = now;
        num_frames = 0;
        idle_count++;

        if (idle_count > (60*10+30*60+10*120))
        {
          // We've been idle for 3m10s -- cut rate down to 5 fps
          target_time = now + 200;
        }
        else if (idle_count > (60*10+30*60))
        {
          // We've been idle for 70 seconds -- cut rate down to 10 fps
          target_time = now + 100;
        }
        else if (idle_count > 60*10)
        {
          // Been idle for 10 seconds -- cut rate down to 30 fps
          target_time = now + 33;
        }
        else
        {
          target_time = now + 17;
        }

        delta = target_time - now;
      }

  #ifdef NO_PPOLL
      int count = poll(session_fds, max_fd+1, delta);
  #else
      struct timespec ts;
      ts.tv_sec = delta / 1000;
      ts.tv_nsec = (delta % 1000) * 1000000;
      int count = ppoll(session_fds, max_fd+1, &ts, &sigmask);
  #endif
      if (count > 0) idle = false;

      if (count == -1)
      {
        if (errno == EINTR)
        {
        }
        else
        {
          LOG_ERROR("poll() failed with errno %i\n", errno);
          break;
        }
        continue;
      }
      for (int i = 0; count; i++)
      {
        if (session_fds[i].fd < 0) continue;
        if (session_fds[i].revents == 0) continue;
        count--;
        if (i == listen_fd)
        {
          LOG_DEBUG("New session arrived");
          int fd = accept(listen_fd, NULL, NULL);
          int flags = fcntl(fd, F_GETFL, 0);
          fcntl(fd, F_SETFL, flags | O_NONBLOCK);
          new_session(fd);
        }
        else
        {
          //LOG_DEBUG("Handling session %i (events:0x%08x 0%o)", i, session_fds[i].revents, session_fds[i].revents);

          if (session_fds[i].revents & POLLOUT)
          {
            Session * s = sessions+i;
            while (s->buffered_out)
            {
              int r = send(i, s->buffered_out->data, s->buffered_out->size, 0);
              if (r == s->buffered_out->size)
              {
                void * old = s->buffered_out;
                s->buffered_out = s->buffered_out->next;
                free(old);
                if (s->buffered_out == NULL)
                {
                  session_fds[i].revents &= ~POLLOUT;
                }
              }
              else if (r == -1 && (errno == EAGAIN || errno == EWOULDBLOCK))
              {
                break;
              }
              else if (r == -1 && errno == EINTR)
              {
                break;
              }
              else
              {
                LOG_ERROR("Failed delayed send with errno:%i\n", errno);
                close_session(i);
              }
            }
          }

          if (session_fds[i].revents & POLLIN)
          {
            static char buf[1024] = {};
            int readsize = read(session_fds[i].fd, buf, sizeof(buf)-1);
            if (readsize > 0)
            {
              handle_message(i, buf, readsize);
            }
            else
            {
              close_session(i);
            }
          }

          if (session_fds[i].revents & (POLLERR|POLLHUP|POLLRDHUP))
          {
            close_session(i);
          }
        }
      }

      break;
    }

    if (quitting) break;
    if (max_fd < 0) break;

    SDL_Event event;
    while (SDL_PollEvent(&event))
    {
      idle = false;
      if (event.type == SDL_QUIT) quitting = true;
      lux_do_event(&event);
    }

    if (!idle) draw_pending = true;

  }

  lux_terminate();

  SDL_Quit();
}



static void unlink_listener (void)
{
  if (listen_sock_name)
  {
    LOG_INFO("Removing listening socket...\n");
    unlink(listen_sock_name);
    free(listen_sock_name);
    listen_sock_name = NULL;
  }
}


sighandler_t old_sigint_handler = NULL;
void handle_sigint (int arg)
{
  quitting = true;
  unlink_listener();
  exit(0);
}

int main (int argc, char * argv[])
{
  int opt;
  uint32_t def_bg_color = 0x54699e;
  listen_sock_name = strdup("sdluxersock");
  while ((opt = getopt(argc, argv, "d:n:")) != -1)
  {
    switch (opt)
    {
      case 'd':
        parse_dimensions(optarg, &screen_width, &screen_height);
        break;
      case 'n':
        free(listen_sock_name);
        listen_sock_name = strdup(optarg);
        break;
    }
  }
  lux_set_bg_color(def_bg_color);

  memset(session_fds, -1, sizeof(session_fds));

  int fd = socket(AF_UNIX, SOCK_SEQPACKET, 0);
  max_fd = listen_fd = fd;
  struct sockaddr_un addr = {};
  addr.sun_family = AF_UNIX;
  strncpy(addr.sun_path, listen_sock_name, sizeof(addr.sun_path)-1);
  if (bind(fd, (struct sockaddr*)&addr, sizeof(addr)))
  {
    LOG_ERROR("Could not bind listening socket '%s' (errno:%i)\n", listen_sock_name, errno);
    exit(1);
  }
  atexit(unlink_listener);
  int tmp = -1;
  int flags = fcntl(fd, F_GETFL, 0);
  fcntl(fd, F_SETFL, flags | O_NONBLOCK);
  session_fds[listen_fd].fd = fd;
  session_fds[listen_fd].events = POLLIN;
  if (listen(listen_fd, 8))
  {
    LOG_ERROR("Could not listen on listening socket\n");
    exit(1);
  }

  old_sigint_handler = signal(SIGINT, handle_sigint);

  SDL_Init(SDL_INIT_VIDEO);
  SDL_EnableUNICODE(1);

  lux_init(screen_width, screen_height, NULL);

  key_register_fkey(SDLK_F1, KMOD_NONE, f1_handler);

  main_loop();

  return 0;
}

typedef struct // CS
{
  int w;
  int h;
  bool double_buf;
  bool resizable;
} SetVideoModeMsg;

typedef struct // CS
{
  int x;
  int y;
} WarpMouseMsg;

typedef struct // CS
{
  char caption[0];
} WM_SetCaptionMsg;

typedef struct // SC
{
  bool success;
  bool double_buf;
  int w;
  int h;
  int pitch;
  int depth;
  uint32_t rmask;
  uint32_t gmask;
  uint32_t bmask;
  char name[0];
} VideoModeSetMsg;

typedef struct // CS - request a flip
{
  bool flip; // Otherwise, it's just a draw
} DrawMsg;

typedef struct // SC - flip done
{
} FlippedMsg;

typedef struct // SC - SDL event
{
  SDL_KeyboardEvent event;
} KeyEventMsg;

typedef struct // SC - SDL event
{
  SDL_MouseButtonEvent event;
} MouseButtonEventMsg;

typedef struct // SC - SDL event
{
  SDL_MouseMotionEvent event;
} MouseMoveEventMsg;

typedef struct // SC - SDL event
{
} QuitEventMsg;

typedef struct // SC - SDL event
{
  SDL_ResizeEvent event;
} ResizedEventMsg;

typedef struct // SC - SDL event
{
  SDL_ActiveEvent event;
} ActiveEventMsg;

typedef struct // CS
{
  int w, h;
  int hotx, hoty;
  char data[0]; // And the mask
} AddCursorMsg;

typedef struct // CS
{
  int op; // 0 = set cursor, 1 = del cursor, 2 = show, 3 = hide
  int index; // For 0 and 1
} ManageCursorMsg;

typedef enum
{
  CursorOpSet = 0,
  CursorOpDel = 1,
  CursorOpShow = 2,
  CursorOpHide = 3,
} ManageCursorOps;

typedef struct // SC
{
  int index; // -1 on error
} CursorAddedMsg;

typedef enum MsgType
{
  Dummy=0,
  SetVideoMode=1,
  VideoModeSet=2,
  Draw=4,
  Flipped=8,
  WarpMouse=16,
  WM_SetCaption=32,
  KeyEvent=64,
  MouseButtonEvent=128,
  MouseMoveEvent=256,
  ResizedEvent=512,
  ActiveEvent=1024,
  QuitEvent=2048,
  AddCursor=4096,
  CursorAdded=8192,
  ManageCursor=16384,
} MsgType;

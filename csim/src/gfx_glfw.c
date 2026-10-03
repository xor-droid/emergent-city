/* gfx_glfw.c — GfxBackend via GLFW + legacy OpenGL; text from stb_easy_font. */
#include "gfx.h"
#include <GLFW/glfw3.h>
#include <GL/gl.h>
#include <math.h>
#include <string.h>
#include <stdlib.h>
#include "stb_easy_font.h"
#include "stb_image_write.h"

static GLFWwindow *win = NULL;
static int   down[GFX_KEY__COUNT], pressed[GFX_KEY__COUNT];
static int   mdown[2], mpressed[2];
static float wheel_acc = 0;

static int gl_key(int k){
    switch(k){
        case GFX_KEY_SPACE:return GLFW_KEY_SPACE; case GFX_KEY_TAB:return GLFW_KEY_TAB; case GFX_KEY_ESC:return GLFW_KEY_ESCAPE;
        case GFX_KEY_UP:return GLFW_KEY_UP; case GFX_KEY_DOWN:return GLFW_KEY_DOWN;
        case GFX_KEY_LEFT:return GLFW_KEY_LEFT; case GFX_KEY_RIGHT:return GLFW_KEY_RIGHT;
        case GFX_KEY_PGUP:return GLFW_KEY_PAGE_UP; case GFX_KEY_PGDN:return GLFW_KEY_PAGE_DOWN;
        case GFX_KEY_G:return GLFW_KEY_G; case GFX_KEY_J:return GLFW_KEY_J; case GFX_KEY_F:return GLFW_KEY_F; case GFX_KEY_L:return GLFW_KEY_L;
        default: if(k>=GFX_KEY_1 && k<=GFX_KEY_9) return GLFW_KEY_1+(k-GFX_KEY_1); return 0;
    }
}
static int gfx_from_glfw(int gk){
    for(int i=0;i<GFX_KEY__COUNT;i++) if(gl_key(i)==gk) return i;
    return -1;
}

static void key_cb(GLFWwindow *w,int key,int sc,int action,int mods){ (void)w;(void)sc;(void)mods;
    int k=gfx_from_glfw(key); if(k<0) return;
    if(action==GLFW_PRESS){ pressed[k]=1; down[k]=1; } else if(action==GLFW_RELEASE){ down[k]=0; } }
static void mbtn_cb(GLFWwindow *w,int button,int action,int mods){ (void)w;(void)mods;
    int b=(button==GLFW_MOUSE_BUTTON_RIGHT)?1:(button==GLFW_MOUSE_BUTTON_LEFT)?0:-1; if(b<0) return;
    if(action==GLFW_PRESS){ mpressed[b]=1; mdown[b]=1; } else if(action==GLFW_RELEASE){ mdown[b]=0; } }
static void scroll_cb(GLFWwindow *w,double xo,double yo){ (void)w;(void)xo; wheel_acc+=(float)yo; }

static int gl_init(const char *title,int w,int h){
    if(!glfwInit()) return 1;
    win = glfwCreateWindow(w,h,title,NULL,NULL);
    if(!win){ glfwTerminate(); return 1; }
    glfwMakeContextCurrent(win); glfwSwapInterval(getenv("CSIM_BENCH")?0:1);
    glfwSetKeyCallback(win,key_cb); glfwSetMouseButtonCallback(win,mbtn_cb); glfwSetScrollCallback(win,scroll_cb);
    return 0;
}
static void gl_shutdown(void){ if(win) glfwDestroyWindow(win); glfwTerminate(); }
static int  gl_should_close(void){ return glfwWindowShouldClose(win); }
static void gl_poll(void){ memset(pressed,0,sizeof(pressed)); mpressed[0]=mpressed[1]=0; wheel_acc=0; glfwPollEvents(); }
static int  gl_width(void){ int w,h; glfwGetFramebufferSize(win,&w,&h); return w; }
static int  gl_height(void){ int w,h; glfwGetFramebufferSize(win,&w,&h); return h; }

static void gl_begin(GfxColor c){
    int w,h; glfwGetFramebufferSize(win,&w,&h);
    glViewport(0,0,w,h);
    glMatrixMode(GL_PROJECTION); glLoadIdentity(); glOrtho(0,w,h,0,-1,1);
    glMatrixMode(GL_MODELVIEW); glLoadIdentity();
    glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA,GL_ONE_MINUS_SRC_ALPHA); glDisable(GL_TEXTURE_2D);
    glClearColor(c.r/255.0f,c.g/255.0f,c.b/255.0f,1.0f); glClear(GL_COLOR_BUFFER_BIT);
}
static void gl_present(void){ glfwSwapBuffers(win); }

static void col(GfxColor c){ glColor4ub(c.r,c.g,c.b,c.a); }
static void gl_fill_rect(int x,int y,int w,int h,GfxColor c){ col(c);
    glBegin(GL_QUADS); glVertex2i(x,y); glVertex2i(x+w,y); glVertex2i(x+w,y+h); glVertex2i(x,y+h); glEnd(); }
static void gl_rect_lines(int x,int y,int w,int h,GfxColor c){ col(c);
    glBegin(GL_LINE_LOOP); glVertex2f(x+0.5f,y+0.5f); glVertex2f(x+w-0.5f,y+0.5f);
    glVertex2f(x+w-0.5f,y+h-0.5f); glVertex2f(x+0.5f,y+h-0.5f); glEnd(); }
static void gl_line(int x1,int y1,int x2,int y2,GfxColor c){ col(c);
    glBegin(GL_LINES); glVertex2f(x1+0.5f,y1+0.5f); glVertex2f(x2+0.5f,y2+0.5f); glEnd(); }
static void gl_circle(int cx,int cy,float r,GfxColor c){ col(c);
    glBegin(GL_TRIANGLE_FAN); glVertex2f((float)cx,(float)cy);
    for(int i=0;i<=16;i++){ float a=(float)i/16.0f*6.2831853f; glVertex2f(cx+cosf(a)*r, cy+sinf(a)*r); } glEnd(); }

static float tscale(int size){ return size/12.0f; }
static void gl_text(const char *s,int x,int y,int size,GfxColor c){
    static char vbuf[70000];
    int nq = stb_easy_font_print(0,0,(char*)s,NULL,vbuf,sizeof(vbuf));
    float sc=tscale(size);
    glPushMatrix(); glTranslatef((float)x,(float)y,0); glScalef(sc,sc,1);
    col(c);
    glEnableClientState(GL_VERTEX_ARRAY);
    glVertexPointer(2, GL_FLOAT, 16, vbuf);
    glDrawArrays(GL_QUADS, 0, nq*4);
    glDisableClientState(GL_VERTEX_ARRAY);
    glPopMatrix();
}
static int gl_text_w(const char *s,int size){ return (int)(stb_easy_font_width((char*)s)*tscale(size)); }

static int  gl_key_pressed(int k){ return (k>=0&&k<GFX_KEY__COUNT)?pressed[k]:0; }
static int  gl_key_down(int k){ return (k>=0&&k<GFX_KEY__COUNT)?down[k]:0; }
static void gl_mouse(int *x,int *y){ double mx,my; glfwGetCursorPos(win,&mx,&my);
    int ww,wh,fw,fh; glfwGetWindowSize(win,&ww,&wh); glfwGetFramebufferSize(win,&fw,&fh);
    *x=(int)(mx*(ww>0?(double)fw/ww:1.0)); *y=(int)(my*(wh>0?(double)fh/wh:1.0)); }
static int  gl_mouse_pressed(int b){ return mpressed[b==GFX_MBTN_RIGHT?1:0]; }
static int  gl_mouse_down(int b){ return mdown[b==GFX_MBTN_RIGHT?1:0]; }
static float gl_wheel(void){ return wheel_acc; }

static void gl_screenshot(const char *path){
    int w,h; glfwGetFramebufferSize(win,&w,&h);
    unsigned char *px = malloc((size_t)w*h*4); if(!px) return;
    glReadPixels(0,0,w,h,GL_RGBA,GL_UNSIGNED_BYTE,px);
    stbi_flip_vertically_on_write(1);            /* GL origin is bottom-left */
    stbi_write_png(path,w,h,4,px,w*4);
    stbi_flip_vertically_on_write(0);
    free(px);
}

const GfxBackend *gfx_glfw(void){
    static const GfxBackend b = {
        "glfw", gl_init, gl_shutdown, gl_should_close, gl_poll, gl_begin, gl_present,
        gl_width, gl_height, gl_fill_rect, gl_rect_lines, gl_line, gl_circle, gl_text, gl_text_w,
        gl_key_pressed, gl_key_down, gl_mouse, gl_mouse_pressed, gl_mouse_down, gl_wheel, gl_screenshot,
    };
    return &b;
}

#include <KalyGFX.h>
#include <GL/gl.h>
#include <stdio.h>
static void face(float r, float g, float b, const float vertices[12]) {
    glColor3f(r, g, b);
    glBegin(GL_QUADS);
    for (int i = 0; i < 4; i++) {
        glVertex3f(vertices[i * 3], vertices[i * 3 + 1], vertices[i * 3 + 2]);
    }
    glEnd();
}
static void draw_cube(void) {
    static const float front[] = {-1, -1, 1, 1, -1, 1, 1, 1, 1, -1, 1, 1};
    static const float back[] = {1, -1, -1, -1, -1, -1, -1, 1, -1, 1, 1, -1};
    static const float left[] = {-1, -1, -1, -1, -1, 1, -1, 1, 1, -1, 1, -1};
    static const float right[] = {1, -1, 1, 1, -1, -1, 1, 1, -1, 1, 1, 1};
    static const float top[] = {-1, 1, 1, 1, 1, 1, 1, 1, -1, -1, 1, -1};
    static const float bottom[] = {-1, -1, -1, 1, -1, -1, 1, -1, 1, -1, -1, 1};
    face(0.2f, 0.7f, 1.0f, front);
    face(1.0f, 0.4f, 0.2f, back);
    face(0.3f, 1.0f, 0.5f, left);
    face(1.0f, 0.8f, 0.2f, right);
    face(0.8f, 0.3f, 1.0f, top);
    face(0.2f, 0.9f, 0.9f, bottom);
}
int main(void) {
    if (KGFX_Init()) {
        fprintf(stderr, "%s\n", KGFX_GetError());
        return 1;
    }
    KGFX_Window *window =
        KGFX_CreateWindow("Rotating Cube - Space: pause | W: wireframe | Esc: close", 640, 480);
    if (!window) {
        fprintf(stderr, "%s\n", KGFX_GetError());
        return 1;
    }
    KGFX_GLContext context = KGFX_GL_CreateContext(window);
    if (!context) {
        fprintf(stderr, "%s\n", KGFX_GetError());
        KGFX_DestroyWindow(window);
        return 1;
    }
    glEnable(GL_DEPTH_TEST);
    glEnable(GL_CULL_FACE);
    glCullFace(GL_BACK);
    glClearColor(0.045f, 0.07f, 0.11f, 1.0f);
    int running = 1, paused = 0, wireframe = 0;
    float angle = 25.0f;
    uint64_t last = KGFX_GetTicks();
    while (running) {
        KGFX_Event event;
        int status;
        while ((status = KGFX_PollEvent(window, &event)) > 0) {
            if (event.type == KGFX_QUIT || (event.type == KGFX_KEYDOWN && event.key == 27)) {
                running = 0;
            } else if (event.type == KGFX_KEYDOWN && event.key == ' ') {
                paused = !paused;
            } else if (event.type == KGFX_KEYDOWN && (event.key == 'w' || event.key == 'W')) {
                wireframe = !wireframe;
                glPolygonMode(GL_FRONT_AND_BACK, wireframe ? GL_LINE : GL_FILL);
            }
        }
        if (status < 0) {
            fprintf(stderr, "%s\n", KGFX_GetError());
            running = 0;
        }
        uint64_t now = KGFX_GetTicks();
        unsigned delta = now - last > 100 ? 100 : now - last;
        last = now;
        if (!paused) {
            angle += delta * 0.045f;
            if (angle >= 360.0f) {
                angle -= 360.0f;
            }
        }
        int width, height;
        KGFX_GetWindowSize(window, &width, &height);
        double aspect = (double)width / height;
        glViewport(0, 0, width, height);
        glMatrixMode(GL_PROJECTION);
        glLoadIdentity();
        glFrustum(-aspect, aspect, -1, 1, 2, 30);
        glMatrixMode(GL_MODELVIEW);
        glLoadIdentity();
        glTranslatef(0, 0, -6);
        glRotatef(angle, 0.6f, 1.0f, 0.25f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        draw_cube();
        GLenum error = glGetError();
        if (error) {
            fprintf(stderr, "KalyGL error 0x%x\n", error);
            running = 0;
        }
        if (KGFX_GL_SwapWindow(window)) {
            fprintf(stderr, "%s\n", KGFX_GetError());
            running = 0;
        }
        KGFX_Delay(16);
    }
    KGFX_GL_DeleteContext(context);
    KGFX_DestroyWindow(window);
    KGFX_Quit();
    return 0;
}

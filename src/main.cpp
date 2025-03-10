#include <signal.h>
#include <fcntl.h>
#include <errno.h>
#include <math.h>
#include <pthread.h>
#include <unistd.h>
#include <string.h>
#include <netinet/in.h>

#include <map>
#include <string>
#include <vector>
#include <algorithm>

#define IMGUI_DEFINE_MATH_OPERATORS
#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl2.h"

#include <GLFW/glfw3.h>
#include "implot.h"

static int run = 1;

static ImVec2 window_size = ImVec2(1300, 600);

static void
glfw_error_callback(int error, const char *description)
{
    fprintf(stderr, "GLFW Error %d: %s\n", error, description);
}

void
resize_callback(GLFWwindow *window, int width, int height)
{
    window_size.x = width;
    window_size.y = height;
}

static void
ctrlc(int)
{
    run = 0;
}

void
TextCenter(const char *text)
{
    float font_size = ImGui::GetFontSize() * strlen(text) / 2;
    ImGui::SameLine(ImGui::GetWindowSize().x / 2 - font_size + (font_size / 2));
    ImGui::Text("%s", text);
}

int
main(int argc, char **argv)
{
    glfwSetErrorCallback(glfw_error_callback);
    if(!glfwInit()) {
        fprintf(stderr, "Unable to init GLFW\n");
        exit(1);
    }

    GLFWwindow *window =
        glfwCreateWindow(window_size.x, window_size.y,
                         "Mios USB Signal Scope", NULL, NULL);
    if(window == NULL) {
        fprintf(stderr, "Unable to open window\n");
        exit(1);
    }

    glfwSetWindowPos(window, 50, 50);

    glfwSetWindowSizeCallback(window, resize_callback);
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImPlot::CreateContext();

    ImGuiIO &io = ImGui::GetIO();
    (void)io;

    ImGui::StyleColorsDark();
    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL2_Init();

    ImVec4 clear_color = ImVec4(0, 0, 0, 0);

    signal(SIGINT, ctrlc);

    run = 1;
    while(run && !glfwWindowShouldClose(window)) {

        glfwPollEvents();
        ImGui_ImplOpenGL2_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(window_size);

        if(ImGui::Begin(
               "main", NULL,
               ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                   ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                   ImGuiWindowFlags_NoBackground)) {

        }
        ImGui::End();

        ImGui::Render();
        int display_w, display_h;
        glfwGetFramebufferSize(window, &display_w, &display_h);
        glViewport(0, 0, display_w, display_h);
        glClearColor(clear_color.x, clear_color.y, clear_color.z,
                     clear_color.w);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL2_RenderDrawData(ImGui::GetDrawData());
        glfwMakeContextCurrent(window);
        glfwSwapBuffers(window);
    }
    ImGui_ImplOpenGL2_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();
}

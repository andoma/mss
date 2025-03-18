#include <signal.h>
#include <fcntl.h>
#include <errno.h>
#include <math.h>
#include <unistd.h>
#include <string.h>
#include <netinet/in.h>

#include <map>
#include <string>
#include <vector>
#include <algorithm>
#include <mutex>
#include <thread>
#include <fmt/format.h>

#define IMGUI_DEFINE_MATH_OPERATORS
#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl2.h"

#include <GLFW/glfw3.h>
#include "implot.h"

#include <libusb.h>

static int run = 1;

static ImVec2 window_size = ImVec2(1300, 400);

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


static uint16_t remote_vid;
static uint16_t remote_pid;
static uint16_t remote_subclass;

struct Channel {
    std::string m_name;
    std::string m_unit;
    float m_scale{0};
    size_t m_axis{0};
    std::vector<float> m_values;
};

#define MAX_CHANNELS 32

struct Scope {
    void handle_pkt(const uint8_t *pkt, size_t actual_length);

    std::mutex m_mutex;

    std::vector<std::string> m_axis;

    size_t m_depth{0};
    size_t m_num_channels{0};
    Channel m_channels[MAX_CHANNELS];

    uint32_t m_nominal_frequency{0};
    size_t m_active_channels{0};

    size_t m_columns_per_xfer{0};

    int m_autofit{0};

    int m_trig_offset{0};
};


typedef struct sc_usb_pkt_preamble {
    uint8_t pkt_type;
    uint8_t channels;
    uint16_t depth;
    uint32_t nominal_frequency;
    uint16_t trig_offset;
} sc_usb_pkt_preamble_t;


typedef struct sc_usb_pkt_channel {
    uint8_t pkt_type;
    uint8_t unit;
    char name[14];
    float scale;
} sc_usb_pkt_channel_t;


typedef enum {
    SIGCAPTURE_UNIT_UNUSED,
    SIGCAPTURE_UNIT_1,
    SIGCAPTURE_UNIT_VOLTAGE,
    SIGCAPTURE_UNIT_CURRENT,
    SIGCAPTURE_UNIT_TEMPERATURE,
} sigcapture_unit_t;


static const char *
unit_to_str(sigcapture_unit_t unit)
{
    switch(unit) {
    case SIGCAPTURE_UNIT_UNUSED:
        return "off";
    case SIGCAPTURE_UNIT_1:
        return "1";
    case SIGCAPTURE_UNIT_VOLTAGE:
        return "V";
    case SIGCAPTURE_UNIT_CURRENT:
        return "A";
    case SIGCAPTURE_UNIT_TEMPERATURE:
        return "°";
    default:
        return "?";
    }
}


void
Scope::handle_pkt(const uint8_t *pkt, size_t actual_length)
{
    std::unique_lock<std::mutex> lk(m_mutex);

    if(actual_length == sizeof(sc_usb_pkt_preamble_t)) {
        const sc_usb_pkt_preamble_t *p = (const sc_usb_pkt_preamble_t *)pkt;

        m_nominal_frequency = p->nominal_frequency;
        m_active_channels = p->channels;
        m_trig_offset = p->trig_offset;
        m_columns_per_xfer = 32 / p->channels;
        m_depth = p->depth;

        for(size_t i = 0; i < MAX_CHANNELS; i++) {
            m_channels[i].m_values.clear();
            m_channels[i].m_values.reserve(p->depth);
        }

        m_axis.clear();

    } else if(actual_length == sizeof(sc_usb_pkt_channel_t)) {
        const sc_usb_pkt_channel_t *c = (const sc_usb_pkt_channel_t *)pkt;

        int chidx = c->pkt_type;
        if(chidx >= MAX_CHANNELS)
            return;

        auto unit = unit_to_str((sigcapture_unit_t)c->unit);

        m_channels[chidx].m_name = fmt::format("{} ({})", c->name, unit);
        m_channels[chidx].m_scale = c->scale;

        m_channels[chidx].m_unit = unit;

        size_t i;
        for(i = 0; i < m_axis.size(); i++) {
            if(m_axis[i] == m_channels[chidx].m_unit) {
                break;
            }
        }

        if(i == m_axis.size()) {
            m_axis.push_back(unit);
        }

        m_channels[chidx].m_axis = i;

    } else if(actual_length == 1) {
        if(!m_autofit)
            m_autofit = 1;

    } else if(actual_length == 64) {

        const int16_t *src = (const int16_t *)pkt;
        for(size_t i = 0; i < m_columns_per_xfer; i++) {
            for(size_t j = 0; j < m_active_channels; j++) {
                auto &c = m_channels[j];
                float v = *src++ * c.m_scale;
                if(c.m_values.size() < m_depth)
                    c.m_values.push_back(v);
            }
        }
    }
}


static void
rx_thread(Scope &scope)
{
    libusb_context *ctx;

    if(libusb_init(&ctx)) {
        printf("Unable to open libusb\n");
        exit(1);
    }

    while(1) {
        struct libusb_device **devs;
        if(libusb_get_device_list(ctx, &devs) < 0) {
            printf("mss: Failed to open\n");
            sleep(1);
            continue;
        }

        libusb_device_handle *h = NULL;
        int interface = -1;
        int endpoint = -1;
        struct libusb_device *dev;
        size_t i = 0;

        while((dev = devs[i++]) != NULL) {
            struct libusb_device_descriptor dev_desc;
            int r = libusb_get_device_descriptor(dev, &dev_desc);
            if(r < 0)
                continue;

            if(dev_desc.idVendor != remote_vid ||
               dev_desc.idProduct != remote_pid)
                continue;

            struct libusb_config_descriptor *cfg_desc;
            r = libusb_get_active_config_descriptor(dev, &cfg_desc);
            if(r < 0)
                continue;

            for(int i = 0; i < cfg_desc->bNumInterfaces; i++) {
                for(int j = 0; j < cfg_desc->interface[i].num_altsetting; j++) {
                    if(cfg_desc->interface[i].altsetting[j].bInterfaceClass ==
                           255 &&
                       cfg_desc->interface[i]
                               .altsetting[j]
                               .bInterfaceSubClass == remote_subclass) {
                        endpoint = -1;
                        for(int k = 0;
                            k <
                            cfg_desc->interface[i].altsetting[j].bNumEndpoints;
                            k++) {
                            if(cfg_desc->interface[i]
                                   .altsetting[j]
                                   .endpoint[k]
                                   .bEndpointAddress &
                               0x80) {
                                endpoint = cfg_desc->interface[i]
                                               .altsetting[j]
                                               .endpoint[k]
                                               .bEndpointAddress;
                                break;
                            }
                        }
                        if(endpoint != -1)
                            interface = cfg_desc->interface[i]
                                            .altsetting[j]
                                            .bInterfaceNumber;
                        break;
                    }
                }
                if(interface != -1)
                    break;
            }
            libusb_free_config_descriptor(cfg_desc);
            if(interface != -1) {
                if(libusb_open(dev, &h) < 0) {
                    h = NULL;
                }
                break;
            }
        }

        libusb_free_device_list(devs, 1);

        if(h == NULL) {
            sleep(1);
            continue;
        }

        libusb_claim_interface(h, interface);
        printf("Interface %d endpoint:0x%02x running\n", interface,
               endpoint);
        uint8_t pkt[64];
        int actual_length;
        while(1) {
            int r = libusb_bulk_transfer(h, endpoint, pkt, sizeof(pkt),
                                         &actual_length, 0);
            if(r) {
                if(r == LIBUSB_ERROR_PIPE)
                    continue;  // Remote stalled
                printf("usb: bulk transfer error:%s\n",
                       libusb_error_name(r));
                break;
            }
            scope.handle_pkt(pkt, actual_length);
        }
        libusb_release_interface(h, interface);
        printf("usb: Closing\n");
        libusb_close(h);
        sleep(1);
    }
}


static int
timefmt(double value, char* buff, int size, void* user_data)
{
    Scope *s = (Scope *)user_data;
    value -= s->m_trig_offset;
    value /= s->m_nominal_frequency;
    return snprintf(buff, size, "%.2f", value * 1000.0);
}


void
TextCenter(const char *text)
{
    float font_size = ImGui::GetFontSize() * strlen(text) / 2;
    ImGui::SameLine(ImGui::GetWindowSize().x / 2 - font_size + (font_size / 2));
    ImGui::Text("%s", text);
}

static uint32_t channel_colors[8] = {
    0xff00ffff,
    0xffff00ff,
    0xffffff00,
    0xff55ff55,
    0xff0000ff,
    0xffff0000,
    0xff999999,
    0xff8888ff
};


int
main(int argc, char **argv)
{
    int opt;
    while ((opt = getopt(argc, argv, "v:p:c:")) != -1) {
        switch(opt) {
        case 'v':
            remote_vid = strtol(optarg, NULL, 0);
            break;
        case 'p':
            remote_pid = strtol(optarg, NULL, 0);
            break;
        case 'c':
            remote_subclass = strtol(optarg, NULL, 0);
            break;
        }
    }

    printf("vid:0x%04x pid:0x%04x c:%d\n",
           remote_vid, remote_pid, remote_subclass);

    Scope scope;

    glfwSetErrorCallback(glfw_error_callback);
    if(!glfwInit()) {
        fprintf(stderr, "Unable to init GLFW\n");
        exit(1);
    }

    std::thread([&scope](){
        rx_thread(scope);
    }).detach();

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

            if(scope.m_autofit == 1) {
                scope.m_autofit++;
                ImPlot::SetNextAxesToFit();
            }

            if(ImPlot::BeginPlot("scope", ImVec2(-1, 350))) {
                std::unique_lock<std::mutex> lk(scope.m_mutex);

                ImPlot::SetupAxis(ImAxis_X1, "ms");
                ImPlot::SetupAxisFormat(ImAxis_X1, timefmt, &scope);

                for(size_t i = 0; i < scope.m_axis.size(); i++) {
                    ImPlot::SetupAxis(ImAxis_Y1 + i, scope.m_axis[i].c_str());
                }

                for(size_t i = 0; i < scope.m_active_channels; i++) {
                    const auto &c = scope.m_channels[i];
                    if(c.m_values.size() == 0)
                        continue;
                    ImPlot::SetAxis(ImAxis_Y1 + c.m_axis);

                    ImPlot::PushStyleColor(ImPlotCol_Line,
                                           channel_colors[i & 7]);

                    ImPlot::PlotLine(c.m_name.c_str(),
                                     c.m_values.data(), c.m_values.size());
                    ImPlot::PopStyleColor();
                }
                ImPlot::EndPlot();
            }
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

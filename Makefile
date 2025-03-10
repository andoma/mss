include mk/$(shell uname -s).mk

O=build

PROG=build/mss

CPPFLAGS += -O2 -ggdb -Wall -Werror

CXXFLAGS += -DIMGUI_USER_CONFIG=\"imcfg.h\"
CXXFLAGS += -I. -Iext/imgui -Iext/imgui/backends/  -Iext/implot -std=c++14
LDFLAGS += -lm -lpthread

CPPFLAGS += ${shell pkg-config --cflags libusb-1.0}
LDFLAGS += ${shell pkg-config --libs libusb-1.0}

SRCS += \
	src/main.cpp \

SRCS += \
	ext/imgui/imgui.cpp \
	ext/imgui/imgui_draw.cpp \
	ext/imgui/imgui_widgets.cpp \
	ext/imgui/imgui_tables.cpp \
	ext/imgui/imgui_demo.cpp \
	ext/imgui/backends/imgui_impl_glfw.cpp \
	ext/imgui/backends/imgui_impl_opengl2.cpp \
	ext/implot/implot.cpp \
	ext/implot/implot_items.cpp \

ALLDEPS += Makefile

include mkglue/glfw3.mk

include mkglue/prog.mk

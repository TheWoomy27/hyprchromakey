SRC     = $(wildcard src/*.cpp)
OUT     = out/hyprchromakey.so
CXXFLAGS += -shared -fPIC --no-gnu-unique -std=c++2b -O2 -g -DWLR_USE_UNSTABLE -Isrc/
PKGS     = pixman-1 libdrm hyprland hyprlang

all: $(OUT)

$(OUT): $(SRC) $(wildcard src/*.hpp)
	mkdir -p out
	$(CXX) $(CXXFLAGS) $(SRC) -o $(OUT) `pkg-config --cflags $(PKGS)`

clean:
	rm -rf out

load: all
	hyprctl plugin unload $(shell pwd)/$(OUT) || true
	hyprctl plugin load $(shell pwd)/$(OUT)

unload:
	hyprctl plugin unload $(shell pwd)/$(OUT)

.PHONY: all clean load unload

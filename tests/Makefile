# Offline checks that need neither a running hyprland nor a display.
#
#   make -C tests HYPRLAND_SRC=/path/to/Hyprland
#
# chroma_math    - runs the key test on the GPU and checks the resulting alpha
# shader_patch   - preprocesses hyprland's own shaders exactly as hyprland does, splices the
#                  chroma code in, and compiles every feature variant. This is the one that
#                  tells you a new hyprland release changed its shaders out from under us.

HYPRLAND_SRC ?= ../hyprland
SHADER_DIR    = $(HYPRLAND_SRC)/src/render/shaders/glsl

CXXFLAGS += -std=c++2b -g
GL_FLAGS  = `pkg-config --cflags --libs egl glesv2`

all: chroma_math shader_patch

chroma_math: bin/chroma_math_test
	./bin/chroma_math_test

shader_patch: bin/shader_patch_test
	@test -d "$(SHADER_DIR)" || { echo "set HYPRLAND_SRC to a hyprland checkout (looked in $(SHADER_DIR))"; exit 1; }
	./bin/shader_patch_test $(SHADER_DIR)

bin/chroma_math_test: chroma_math_test.cpp ../src/ChromaGLSL.hpp
	mkdir -p bin
	$(CXX) $(CXXFLAGS) $< -o $@ $(GL_FLAGS)

bin/shader_patch_test: shader_patch_test.cpp ../src/ChromaGLSL.hpp
	mkdir -p bin
	$(CXX) $(CXXFLAGS) $< -o $@ $(GL_FLAGS) -lglslang-default-resource-limits -lglslang

clean:
	rm -rf bin

.PHONY: all chroma_math shader_patch clean

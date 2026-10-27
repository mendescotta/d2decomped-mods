CXX := i686-w64-mingw32-g++
CXXFLAGS := -std=c++17 -O2 -Wall -Wextra -Werror -DWIN32_LEAN_AND_MEAN -DUNICODE -D_UNICODE
LDFLAGS := -static -static-libgcc -static-libstdc++
OUT := build
NATIVE_CXX ?= c++
NATIVE_CXXFLAGS := -std=c++17 -O2 -Wall -Wextra -Werror -pthread

.PHONY: all clean test-native
all: $(OUT)/population_trace.dll $(OUT)/population_launcher.exe

$(OUT):
	mkdir -p $(OUT)

$(OUT)/population_trace.dll: trace.cpp sidecar_store.cpp sidecar_store.h | $(OUT)
	$(CXX) $(CXXFLAGS) -fno-omit-frame-pointer -shared -Wl,--add-stdcall-alias -o $@ trace.cpp sidecar_store.cpp $(LDFLAGS) -ladvapi32

$(OUT)/population_launcher.exe: launcher.cpp | $(OUT)
	$(CXX) $(CXXFLAGS) -municode -o $@ $< $(LDFLAGS)

$(OUT)/sidecar_store_test: sidecar_store_test.cpp sidecar_store.cpp sidecar_store.h | $(OUT)
	$(NATIVE_CXX) $(NATIVE_CXXFLAGS) -o $@ sidecar_store_test.cpp sidecar_store.cpp

$(OUT)/sidecar_store_test.exe: sidecar_store_test.cpp sidecar_store.cpp sidecar_store.h | $(OUT)
	$(CXX) $(CXXFLAGS) -o $@ sidecar_store_test.cpp sidecar_store.cpp $(LDFLAGS)

test-native: $(OUT)/sidecar_store_test
	./$(OUT)/sidecar_store_test

clean:
	rm -rf $(OUT)

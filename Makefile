CXX := g++
CXXFLAGS := -std=c++17 -O2 -Wall -Wextra -pthread -Iinclude
TARGET := memory_pool_demo
SOURCES := src/main.cpp src/memory_pool.cpp

.PHONY: all clean run

all: $(TARGET)

$(TARGET): $(SOURCES)
	$(CXX) $(CXXFLAGS) -o $@ $(SOURCES)

run: $(TARGET)
	.\$(TARGET)

clean:
	del /Q $(TARGET).exe 2> NUL || exit 0

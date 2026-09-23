CXX      ?= g++
CXXFLAGS ?= -O3 -march=native -std=c++17 -Wall -Wextra -pthread
LDLIBS   ?= -lnuma

TARGET := mbench
SRC    := mbench.cpp

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(SRC)
	$(CXX) $(CXXFLAGS) $(SRC) -o $@ $(LDLIBS)

clean:
	rm -f $(TARGET)

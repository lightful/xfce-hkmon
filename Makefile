CXX           ?= g++
CXXFLAGS      += -std=c++0x -O3 -Wall -Wextra -pedantic -march=native
LIBS          := -lrt
BIN           := xfce-hkmon

$(BIN): $(BIN).o
	$(CXX) -o $@ $^ $(CFLAGS) $(LIBS)
	strip $@

%.o: %.cpp
	$(CXX) -c -o $@ $< $(CXXFLAGS)

.PHONY: clean

clean:
	rm -f *.o $(BIN)

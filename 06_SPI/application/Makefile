CXX      := g++
CXXFLAGS := -Wall -Wextra -std=c++17

TARGETS  := spi_tx spi_rx

.PHONY: all clean

all: $(TARGETS)

spi_tx: spi.cpp spi.cpp spi_tx.cpp
	$(CXX) $(CXXFLAGS) -o $@ $^

spi_rx: spi.cpp spi.cpp spi_rx.cpp
	$(CXX) $(CXXFLAGS) -o $@ $^

clean:
	rm -f $(TARGETS)

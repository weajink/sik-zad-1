CXX = g++
CXXFLAGS = -std=c++23 -Wall -Wextra -Wpedantic -Werror -I.

.PHONY: all clean

all: kayles_server kayles_client

kayles_server: kayles_server.cpp
	$(CXX) $(CXXFLAGS) -o $@ $<

kayles_client: kayles_client.cpp
	$(CXX) $(CXXFLAGS) -o $@ $<

clean:
	rm -f kayles_server kayles_client

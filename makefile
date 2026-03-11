# Makefile — place this file in the root of MedorCoin-Blockchain

# Recreate Makefile for MedorCoin build

# Compiler and flags
CXX = g++
CXXFLAGS = -std=c++17 -Iinclude

# All source files in src/ folders
SRC_CPP = $(wildcard src/*.cpp) \
          $(wildcard src/db/*.cpp) \
          $(wildcard src/node/*.cpp)

# Path to your separately stored T.cpp
SECRETS_PATH = /home/username/MedorCoinSecrets/T.cpp

# Convert source list to object files
OBJ = $(patsubst %.cpp,build/%.o,$(notdir $(SRC_CPP))) build/T.o

# Default build target
all: build/medorcoin

# Compile .cpp files in src/
build/%.o: src/%.cpp
	@mkdir -p build
	$(CXX) $(CXXFLAGS) -c $< -o $@

build/%.o: src/db/%.cpp
	@mkdir -p build
	$(CXX) $(CXXFLAGS) -c $< -o $@

build/%.o: src/node/%.cpp
	@mkdir -p build
	$(CXX) $(CXXFLAGS) -c $< -o $@

# Compile T.cpp from your saved secrets folder
build/T.o:
	@mkdir -p build
	$(CXX) $(CXXFLAGS) -c $(SECRETS_PATH) -o build/T.o

# Link everything to create the final executable
build/medorcoin: $(OBJ)
	$(CXX) $(OBJ) -lssl -lcrypto -lleveldb -lrocksdb -lpthread -o $@


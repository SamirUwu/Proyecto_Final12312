CC       = gcc
CXX      = g++
DAISY    = ../dependencies/DaisySP/Source

# NI-DAQmx library path (64-bit)
NIDAQMX_LIB = "C:\Program Files (x86)\National Instruments\Shared\ExternalCompilerSupport\C\lib64\msvc"

CFLAGS   = -Iinclude -I$(DAISY) -I$(DAISY)/Utility -I$(DAISY)/Control \
           -I$(DAISY)/Effects -Wall -O2

CXXFLAGS = $(CFLAGS) -std=c++14

C_SRC    = $(filter-out src/chorus.c src/pitch_shifter.c src/overdrive.c src/ni_feeder.c, \
             $(wildcard src/*.c))

CPP_SRC  = src/chorus.cpp src/pitch_shifter.cpp src/overdrive.cpp \
           src/reverb.cpp \
           $(DAISY)/Control/phasor.cpp \
           $(DAISY)/Effects/overdrive.cpp

C_OBJ    = $(C_SRC:.c=.o)
CPP_OBJ  = $(CPP_SRC:.cpp=.o)

AUDIO_OUT = audio_engine.exe
FEEDER_OUT = ni_feeder.exe

all: $(AUDIO_OUT) $(FEEDER_OUT)

$(AUDIO_OUT): $(C_OBJ) $(CPP_OBJ)
	$(CXX) -o $@ $^ -lm -lportaudio -lws2_32 -ladvapi32 -L$(NIDAQMX_LIB) -lNIDAQmx

$(FEEDER_OUT): src/ni_feeder.c
	$(CC) $(CFLAGS) -o $@ $< -L$(NIDAQMX_LIB) -lNIDAQmx -lws2_32

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

clean:
	rm -f $(AUDIO_OUT) $(FEEDER_OUT) src/*.o
	del /Q $(AUDIO_OUT) $(FEEDER_OUT) src\*.o 2>NUL || true

.PHONY: all clean
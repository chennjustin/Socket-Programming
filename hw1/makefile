CXX = gcc
CXXFLAGS = -Wall -O2
TARGET = client

all: $(TARGET)

$(TARGET): client.c
	$(CXX) $(CXXFLAGS) -o $(TARGET) client.c

clean:
	rm -f $(TARGET)

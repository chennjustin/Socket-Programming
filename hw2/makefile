CC = gcc
CFLAGS = -Wall -Wextra -O2 -pthread

TARGET = server
SRC = server.c

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) $(SRC) -o $(TARGET)

clean:
	rm -f $(TARGET)

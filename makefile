CC = gcc
CFLAGS = -Wall -O2
TARGET = client

all: $(TARGET)

$(TARGET): client.c
	$(CC) $(CFLAGS) -o $(TARGET) client.c

clean:
	rm -f $(TARGET)

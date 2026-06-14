CC = gcc
CFLAGS = -Wall -Wextra -g -I../miniaudio -I../embed
LIBS = -lX11 -lXext -lcairo -lm -ldl

TARGET = goose
SRCS = main.c

$(TARGET): $(SRCS)
	$(CC) $(CFLAGS) -o $(TARGET) $(SRCS) $(LIBS)

clean:
	rm -f $(TARGET)

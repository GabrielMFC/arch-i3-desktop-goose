CC = gcc
CFLAGS = -Wall -Wextra -g
LIBS = -lX11 -lXext -lcairo -lm -lpthread

TARGET = goose
SRCS = main.c

$(TARGET): $(SRCS)
	$(CC) $(CFLAGS) -o $(TARGET) $(SRCS) $(LIBS)

clean:
	rm -f $(TARGET)

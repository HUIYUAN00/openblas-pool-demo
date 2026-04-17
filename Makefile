# OpenBLAS风格线程池与内存池

CC = gcc
CFLAGS = -O2 -Wall -pthread -std=c11
LDFLAGS = -pthread -lrt

TARGET = demo
OBJS = test.o pool.o

.PHONY: all clean run

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(LDFLAGS) -o $@ $^

test.o: test.c pool.h
	$(CC) $(CFLAGS) -c test.c

pool.o: pool.c pool.h
	$(CC) $(CFLAGS) -c pool.c

clean:
	rm -f $(TARGET) $(OBJS)

run: $(TARGET)
	./$(TARGET)
SRCS=$(wildcard *.c)
OBJS=$(SRCS:.c=.o)

all: stacy

%.o: %.c stacy.h
	gcc -O6 -c -o $@ -lm -lasound $<

stacy: $(OBJS)
	gcc -o stacy -lm -lasound $(OBJS)

clean:
	rm -f stacy *.o

memtest: memtest.c
	$(CC) -O2 -o $@ $< -lpthread

clean:
	rm -f memtest
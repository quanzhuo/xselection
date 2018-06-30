xselection: xselection.c
	gcc -o xselection xselection.c -lXmu -lX11

clean:
	rm xselection

all: xselection

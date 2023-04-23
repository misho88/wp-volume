SRC=wp-volume.c
EXE=wp-volume

all: $(EXE)

$(EXE): $(SRC)
	gcc -Wall -Wextra -lm `pkg-config --cflags --libs wireplumber-0.4` $< -o $@

clean:
	rm -f $(EXE)

install: $(EXE)
	install -d $(DESTDIR)/usr/local/bin
	install $(EXE) $(DESTDIR)/usr/local/bin

uninstall:
	rm -f $(DESTDIR)/usr/local/bin/$(EXE)
